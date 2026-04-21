/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Standard C headers
 *
 * This file is intended to be included first by all other SPDK files.
 */

/*
 * [한국어 설명] SPDK 공통 표준 include 묶음 (stdinc.h)
 *
 * === 파일의 역할 ===
 * SPDK가 필요로 하는 **표준 C + POSIX + 플랫폼 확장** 헤더를 하나로 묶어
 * "SPDK의 모든 .c/.h 파일이 이 헤더 하나만 선행 포함하면 표준 기능은
 * 모두 사용 가능"한 상태를 만든다. 개별 파일이 필요한 표준 헤더를 각자
 * 선택적으로 포함하게 놔두면, 다음 문제가 발생한다:
 *   - 플랫폼별(Linux/FreeBSD) 헤더 차이로 빌드 깨짐
 *   - 일부 소스에서 실수로 중요한 헤더 누락 → 이식성 저하
 *   - `_POSIX_C_SOURCE` 등 feature test 매크로 정의 순서 실수
 * 본 헤더는 이런 위험을 표준 include set을 프로젝트 차원에서 확정해 회피한다.
 *
 * 또한 FreeBSD에 없는 errno 코드(`ENOKEY`)를 보충하는 호환성 정의를 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 공개 헤더의 최상위 선행 include. 본 파일은 SPDK 고유 타입이나 함수를
 * 선언하지 않으며, 오직 컴파일러/libc가 제공하는 표준 심볼을 모은다. 모든
 * 다른 spdk/* 헤더 첫 줄에 이 파일 포함이 관례이다.
 * 실행 컨텍스트: 컴파일 타임만. 런타임 비용 없음.
 *
 * === 타 모듈과의 연결 ===
 * 의존: 컴파일러·libc 표준 헤더(수십 개).
 * 의존하는 모듈: SPDK 전체 공개 헤더와 구현 파일.
 * 공유 자료구조: 없음.
 *
 * === 주요 함수/구조체 요약 ===
 * 헤더 include만 담당. 아래 분류로 그룹화되어 있음:
 *   (1) Standard C: assert / ctype / errno / inttypes / limits / math /
 *                   stdarg / stdbool / stddef / stdint / stdio / stdlib /
 *                   string / strings / time
 *   (2) POSIX:       arpa/inet, dirent, fcntl, fnmatch, ftw, glob, ifaddrs,
 *                   libgen, netdb, poll, pthread, semaphore, signal, syslog,
 *                   termios, unistd, net/if, netinet/{in,tcp}, sys/{ioctl,
 *                   mman, resource, types, socket, stat, uio, un, user, wait,
 *                   statvfs, syscall, file}, regex
 *   (3) GNU 확장:    getopt
 *   (4) Linux 전용: sys/xattr, sys/eventfd, sys/epoll, sched
 *   (5) FreeBSD 전용: sys/sysctl
 *   (6) Linux+FreeBSD: aio
 *   (7) 호환성 패치: ENOKEY (FreeBSD에 없음)
 */

#ifndef SPDK_STDINC_H            /* [한국어] include 가드 시작 */
#define SPDK_STDINC_H            /* [한국어] 가드 심볼 */

#ifdef __cplusplus               /* [한국어] C++에서 이 헤더가 포함되는 경우 C 링크 규약으로 전환
                                  *  - 표준 헤더 자체는 이미 __BEGIN_DECLS/__END_DECLS 등을 통해 C 링크 규약을 선언하므로 실질 영향은 없으나, 관례로 감쌈 */
extern "C" {
#endif

/* Standard C */                 /* [한국어] 표준 C 라이브러리 헤더 그룹 — C89/C99/C11에서 정의 */
#include <assert.h>              /* [한국어] assert(), static_assert (C11) — spdk/assert.h가 활용 */
#include <ctype.h>               /* [한국어] isdigit/isalpha 등 문자 분류 함수 */
#include <errno.h>               /* [한국어] errno 전역 변수 및 EAGAIN/EIO 등 에러 코드 — SPDK는 대부분 POSIX errno를 직접 반환 */
#include <inttypes.h>            /* [한국어] PRIu64/PRIx64 포맷 매크로 — printf에서 64비트 값 이식성 있는 출력 */
#include <limits.h>              /* [한국어] INT_MAX, PATH_MAX 등 한계값 */
#include <math.h>                /* [한국어] fabs, sqrt 등 (주로 스케줄러 통계, 히스토그램에서 사용) */
#include <stdarg.h>              /* [한국어] va_list 등 가변 인자 지원 — spdk_vlog/rpc 포매터 */
#include <stdbool.h>             /* [한국어] C99 bool 타입 — SPDK 코드는 int 대신 bool을 명시적으로 사용 */
#include <stddef.h>              /* [한국어] size_t, offsetof, NULL — offsetof는 SPDK container_of 관용구에서 필수 */
#include <stdint.h>              /* [한국어] uint8_t ~ uint64_t — SPDK는 폭이 명시된 정수형을 강제 */
#include <stdio.h>               /* [한국어] FILE, printf/fprintf, snprintf — 로그/CLI 출력 */
#include <stdlib.h>              /* [한국어] malloc/free, qsort, strtol 등 */
#include <string.h>              /* [한국어] memcpy/memset/strcmp — hot path 포함 전방위 사용 */
#include <strings.h>             /* [한국어] strcasecmp, bzero — POSIX 확장 문자열 함수 */
#include <time.h>                /* [한국어] clock_gettime, struct timespec — 폴러 주기·timeout 계산 */

/* POSIX */                      /* [한국어] POSIX 시스템 인터페이스 헤더 그룹 */
#include <arpa/inet.h>           /* [한국어] inet_ntop/inet_pton — NVMe-oF, iSCSI에서 IP 주소 문자열↔이진 변환 */
#include <dirent.h>              /* [한국어] opendir/readdir — /sys, /proc 스캔 (PCI 디바이스 enumeration 등) */
#include <fcntl.h>               /* [한국어] open 플래그(O_DIRECT 등), fcntl */
#include <fnmatch.h>             /* [한국어] 글로브 패턴 매칭 — 구성 파일 필터 */
#include <ftw.h>                 /* [한국어] 파일 트리 walk — 복잡한 파일 시스템 순회 */
#include <glob.h>                /* [한국어] glob() — /sys/bus/pci/devices/* 열거 등 */
#include <ifaddrs.h>             /* [한국어] getifaddrs — NIC 목록/IP 수집 (NVMe-oF listener 구성) */
#include <libgen.h>              /* [한국어] basename/dirname */
#include <netdb.h>               /* [한국어] getaddrinfo, struct addrinfo — 네트워크 트랜스포트 초기화 */
#include <poll.h>                /* [한국어] poll()/struct pollfd — 일부 sock 구현에서 사용 */
#include <pthread.h>             /* [한국어] pthread_create/mutex/cond — SPDK는 reactor 고정 스레드를 pthread로 spawn */
#include <semaphore.h>           /* [한국어] sem_t — 초기화 동기화에서 일부 사용 */
#include <signal.h>              /* [한국어] signal/sigaction — SIGINT 핸들링, 안전 종료 */
#include <syslog.h>              /* [한국어] syslog() — 데몬 모드 로그 경로 */
#include <termios.h>             /* [한국어] 터미널 제어 — spdk_top 등 대화형 앱에서 사용 */
#include <unistd.h>              /* [한국어] read/write/close/pipe, usleep, getpid 등 */
#include <net/if.h>              /* [한국어] if_nametoindex, struct ifreq — NIC 인덱스/플래그 */
#include <netinet/in.h>          /* [한국어] struct sockaddr_in, IPPROTO_TCP 등 */
#include <netinet/tcp.h>         /* [한국어] TCP_NODELAY, TCP_QUICKACK 등 소켓 옵션 — SPDK TCP 트랜스포트가 사용 */
#include <sys/ioctl.h>           /* [한국어] ioctl() — 일부 드라이버/장치 제어 */
#include <sys/mman.h>            /* [한국어] mmap/munmap/madvise — BAR 매핑, hugepage, shared mem */
#include <sys/resource.h>        /* [한국어] getrlimit/setrlimit — RLIMIT_MEMLOCK(hugepage lock 한도), RLIMIT_NOFILE */
#include <sys/types.h>           /* [한국어] pid_t, off_t, ssize_t — 플랫폼 타입 */
#include <sys/socket.h>          /* [한국어] socket/accept/bind/listen — NVMe-oF TCP, iSCSI, vhost 등 */
#include <sys/stat.h>            /* [한국어] stat/fstat, S_IFREG 등 — 파일 속성 */
#include <sys/uio.h>             /* [한국어] readv/writev, struct iovec — scatter-gather I/O */
#include <sys/un.h>              /* [한국어] struct sockaddr_un — RPC용 유닉스 도메인 소켓 */
#include <sys/user.h>            /* [한국어] PAGE_SIZE 등 일부 플랫폼 상수 */
#include <sys/wait.h>            /* [한국어] waitpid — 자식 프로세스 종료 동기화 */
#include <regex.h>               /* [한국어] POSIX regex — 구성/로그 필터 */
#include <sys/statvfs.h>         /* [한국어] 파일 시스템 통계 */
#include <sys/syscall.h>         /* [한국어] SYS_xxx 상수 (gettid 등 libc 미제공 syscall 직접 호출) */
#include <sys/file.h>            /* [한국어] flock — 파일 기반 락 (/var/run 등) */

/* GNU extension */              /* [한국어] GNU 확장 — glibc/musl에서 제공 */
#include <getopt.h>              /* [한국어] getopt_long — long option 명령 줄 파싱 (SPDK 앱 기본) */

/* Linux */                      /* [한국어] 리눅스 전용 기능 */
#ifdef __linux__
#include <sys/xattr.h>           /* [한국어] 확장 속성 — 일부 fsdev 모듈에서 사용 */
#include <sys/eventfd.h>         /* [한국어] eventfd — lockless cross-thread wakeup (spdk_thread msg, interrupt 연동) */
#include <sys/epoll.h>           /* [한국어] epoll — sock/fd_group의 이벤트 멀티플렉싱 */
#include <sched.h>               /* [한국어] sched_setaffinity, CPU_SET — reactor를 특정 코어에 핀닝할 때 사용 */
#endif

/* FreeBSD */                    /* [한국어] FreeBSD 전용 */
#if defined(__FreeBSD__)
#include <sys/sysctl.h>          /* [한국어] sysctl — FreeBSD 커널 파라미터 조회/설정 */
#endif

/* FreeBSD or Linux */           /* [한국어] 두 플랫폼 공통 */
#if defined(__FreeBSD__) || defined(__linux__)
#include <aio.h>                 /* [한국어] POSIX AIO — bdev_aio 모듈에서 사용. 리눅스에서는 glibc가 POSIX AIO를 스레드풀로 구현하므로 실 성능 경로가 아니며, 주로 호환성/테스트 목적 */
#endif

/* FreeBSD doesn't define ENOKEY */
#ifndef ENOKEY
#define ENOKEY 126               /* [한국어] Linux 전용 errno인 ENOKEY(127 대체)를 FreeBSD 빌드용으로 수동 정의
                                  *  - NVMe 인증/키 누락 에러 반환에 사용
                                  *  - 126은 임의 선정이 아니라 BSD 쪽에서 충돌하지 않는 값 */
#endif

#ifdef __cplusplus               /* [한국어] C++ 가드 닫기 */
}
#endif

#endif /* SPDK_STDINC_H */        /* [한국어] include 가드 종료 */
