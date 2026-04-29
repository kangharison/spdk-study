/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] 파일 디스크립터 헬퍼: 블록 크기·전체 크기 조회 + nonblock 토글
 *               (fd.c)
 *
 * === 파일의 역할 ===
 * SPDK가 일반 파일/블록 디바이스/캐릭터 디바이스를 다룰 때 공통으로 필요한
 * fd 메타데이터(블록 섹터 크기, 디바이스 전체 크기) 조회와 fd nonblocking
 * 모드 설정을 OS 차이를 흡수하며 제공한다. 리눅스(BLKGETSIZE64/BLKSSZGET),
 * FreeBSD(DIOCGMEDIASIZE/DIOCGSECTORSIZE), macOS(DKIOCGETBLOCKSIZE) 등의
 * ioctl 차이를 `#if defined(...)`로 분기하여 cross-platform 단일 API를 만든다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 본 파일은 lower-level 유틸이지만 핫패스가 아닌 셋업 경로에서 호출된다:
 *   - module/bdev/aio: AIO bdev 생성 시 백킹 파일/디바이스의 크기와
 *     섹터 크기를 채우기 위해 spdk_fd_get_size / spdk_fd_get_blocklen 사용.
 *   - lib/sock, app/* : 소켓/스트림 fd를 nonblocking으로 전환할 때
 *     spdk_fd_set_nonblock / clear_nonblock 사용.
 * 호출 흐름 예: 사용자가 RPC로 aio bdev 추가 → bdev_aio가 fd open →
 *   spdk_fd_get_size로 LBA 개수 계산 → spdk_bdev 등록.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `spdk/fd.h`(prototype), `spdk/string.h`(spdk_strerror),
 *   `spdk/log.h`(SPDK_ERRLOG), `spdk/stdinc.h`. OS별 헤더(Linux: linux/fs.h,
 *   FreeBSD: sys/disk.h)를 조건부 인클루드.
 * - 호출자: bdev 백엔드(aio/uring), sock, RPC 도구.
 * - 공유 상태: 없음. 모든 함수는 호출자가 소유한 fd를 인자로 받음.
 *
 * === 주요 함수/구조체 요약 ===
 * - dev_get_size(fd) [static]: 블록/캐릭터 디바이스의 전체 크기 byte로
 *   반환. OS별 ioctl 분기.
 * - spdk_fd_get_blocklen(fd): 디바이스의 논리 섹터 크기(byte).
 * - spdk_fd_get_size(fd): fstat으로 파일 종류 판별 후 적절한 경로 사용.
 *   심볼릭 링크/기타는 0.
 * - fd_update_nonblock(fd, set) [static]: F_GETFL → 비트 토글 → F_SETFL.
 * - spdk_fd_set_nonblock / clear_nonblock: 위 helper의 얇은 래퍼.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 C/POSIX 헤더 일괄: stat, fcntl, ioctl, errno 등. */

#include "spdk/fd.h"
/* [한국어] 본 파일이 외부 노출하는 prototype 모음. */
#include "spdk/string.h"
/* [한국어] spdk_strerror — errno → 메시지 변환 (TLS 안전판). */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG 매크로 — 에러 메시지 출력. */

#ifdef __linux__
#include <linux/fs.h>
/* [한국어] BLKGETSIZE64(블록 디바이스 byte 크기), BLKSSZGET(논리 섹터 크기)
 * 같은 리눅스 전용 ioctl 매크로 정의. /usr/include/linux/fs.h 제공. */
#endif

#ifdef __FreeBSD__
#include <sys/disk.h>
/* [한국어] DIOCGMEDIASIZE(전체 크기), DIOCGSECTORSIZE(섹터 크기) FreeBSD ioctl. */
#endif

/*
 * [한국어]
 * dev_get_size - 블록/캐릭터 디바이스 fd의 전체 크기 byte 단위 조회.
 *
 * @fd: 열린 디바이스 파일 디스크립터.
 * @return: byte 크기, 또는 ioctl 실패 시 0.
 *
 * OS별 ioctl이 다르기 때문에 컴파일 시 매크로 분기로 가장 적절한 호출을
 * 선택한다. 리눅스에서는 BLKGETSIZE64(uint64_t 결과)로 4TB 이상도 안전.
 * FreeBSD는 DIOCGMEDIASIZE(off_t).
 *
 * 실행 컨텍스트: 일반적으로 셋업 단계(aio/uring bdev 추가) 1회 호출.
 *
 * 호출 체인:
 *   spdk_fd_get_size → (블록/캐릭터일 때) → dev_get_size → ioctl(2).
 */
static uint64_t
dev_get_size(int fd)
{
#if defined(DIOCGMEDIASIZE) /* FreeBSD */
	off_t size;
	/* [한국어] FreeBSD DIOCGMEDIASIZE 결과 타입(off_t). */

	if (ioctl(fd, DIOCGMEDIASIZE, &size) == 0) {
		/* [한국어] 성공 시 채워진 size를 부호 없는 uint64_t로 반환.
		 * (off_t는 signed지만 디바이스 크기는 음수가 될 일이 없음.) */
		return size;
	}
#elif defined(__linux__) && defined(BLKGETSIZE64)
	uint64_t size;
	/* [한국어] BLKGETSIZE64는 64비트 byte 크기를 직접 반환. */

	if (ioctl(fd, BLKGETSIZE64, &size) == 0) {
		return size;
	}
#endif

	return 0;
	/* [한국어] 실패 또는 미지원 OS. SPDK 호출자는 0을 "조회 실패/미지원"
	 * 시그널로 해석한다. */
}

/*
 * [한국어]
 * spdk_fd_get_blocklen - fd가 가리키는 디바이스의 논리 섹터 크기 조회.
 *
 * @fd: 디바이스 fd.
 * @return: 논리 섹터 byte 크기(보통 512 또는 4096), 실패 시 0.
 *
 * NVMe·SATA·NVDIMM 등 디바이스가 노출하는 LBA 단위가 다르기 때문에 bdev
 * 등록 시 정확한 값이 필요하다. OS별 ioctl을 분기.
 *
 * 호출 체인: bdev_aio_create / 유사 경로 → spdk_fd_get_blocklen → ioctl(2).
 */
uint32_t
spdk_fd_get_blocklen(int fd)
{
#if defined(DIOCGSECTORSIZE) /* FreeBSD */
	uint32_t blocklen;
	/* [한국어] FreeBSD: 32비트 결과. */

	if (ioctl(fd, DIOCGSECTORSIZE, &blocklen) == 0) {
		return blocklen;
	}
#elif defined(DKIOCGETBLOCKSIZE)
	uint32_t blocklen;
	/* [한국어] macOS/Darwin: DKIOCGETBLOCKSIZE. */

	if (ioctl(fd, DKIOCGETBLOCKSIZE, &blocklen) == 0) {
		return blocklen;
	}
#elif defined(__linux__) && defined(BLKSSZGET)
	uint32_t blocklen;
	/* [한국어] Linux: BLKSSZGET. */

	if (ioctl(fd, BLKSSZGET, &blocklen) == 0) {
		return blocklen;
	}
#endif

	return 0;
	/* [한국어] 실패/미지원 시 0 → 호출자가 기본값(예: 512) 추론. */
}

/*
 * [한국어]
 * spdk_fd_get_size - 파일/디바이스 어느 쪽이든 byte 단위 크기 조회.
 *
 * @fd: 일반 파일이거나 블록/캐릭터 디바이스 fd.
 * @return: byte 크기, 또는 0(심볼릭 링크/기타 미지원/실패).
 *
 * fstat으로 파일 종류를 먼저 판별하고:
 *   - 일반 파일: st_size 사용.
 *   - 블록/캐릭터 디바이스: dev_get_size로 ioctl.
 *   - 심볼릭 링크: open 단계에서 이미 따라가기를 했어야 하므로 비정상 상태로
 *     간주하고 0.
 *
 * 호출 체인: bdev_aio 등 → spdk_fd_get_size → fstat / dev_get_size.
 */
uint64_t
spdk_fd_get_size(int fd)
{
	struct stat st;
	/* [한국어] stat 결과 보관 구조체. st_mode와 st_size 사용. */

	if (fstat(fd, &st) != 0) {
		/* [한국어] fstat 자체가 실패하면 fd 자체가 잘못된 것. 0 반환. */
		return 0;
	}

	if (S_ISLNK(st.st_mode)) {
		/* [한국어] 심볼릭 링크는 open 시 일반적으로 따라가지므로 fd가
		 * S_IFLNK일 일은 거의 없다. 안전하게 미지원으로 처리. */
		return 0;
	}

	if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
		/* [한국어] 블록(/dev/sdX, /dev/nvme*) 또는 캐릭터(/dev/raw*) 디바이스 →
		 * ioctl로 크기 조회. */
		return dev_get_size(fd);
	} else if (S_ISREG(st.st_mode)) {
		/* [한국어] 일반 파일은 stat의 st_size가 정확하다. */
		return st.st_size;
	}

	/* Not REG, CHR or BLK */
	/* [한국어] 파이프/소켓/디렉토리 등은 크기 개념이 없으므로 0. */
	return 0;
}

/* If set flag is true then set nonblock, clear otherwise. */
/*
 * [한국어]
 * fd_update_nonblock - fd의 O_NONBLOCK 플래그를 설정 또는 해제.
 *
 * @fd: 대상 디스크립터.
 * @set: true면 O_NONBLOCK 추가, false면 제거.
 * @return: 0 성공, -errno 실패.
 *
 * fcntl F_GETFL/F_SETFL 패턴: 현재 플래그를 읽어와 비트만 토글한 뒤 다시
 * 쓴다. 이미 원하는 상태이면 SETFL 호출 자체를 생략(불필요한 syscall 절감).
 *
 * 실행 컨텍스트: 임의의 SPDK/POSIX 스레드. fd 자체는 호출자 소유.
 *
 * 호출 체인:
 *   spdk_fd_set_nonblock / clear_nonblock → fd_update_nonblock → fcntl(2).
 */
static int
fd_update_nonblock(int fd, bool set)
{
	int flag;
	/* [한국어] 현재 fd 플래그 비트마스크(F_GETFL 결과). */

	flag = fcntl(fd, F_GETFL);
	/* [한국어] 현재 파일 상태 플래그 조회. 실패 시 -1 반환. */
	if (flag < 0) {
		/* [한국어] 잘못된 fd → SPDK 로그로 기록 후 -errno 반환.
		 * spdk_strerror로 사람이 읽을 수 있는 errno 메시지 첨부. */
		SPDK_ERRLOG("fcntl can't get file status flag, fd: %d (%s)\n", fd, spdk_strerror(errno));
		return -errno;
	}

	if (set) {
		/* [한국어] nonblocking 활성 요청. */
		if (flag & O_NONBLOCK) {
			/* [한국어] 이미 활성 → 변경 불필요 → 빠른 종료. */
			return 0;
		}

		flag |= O_NONBLOCK;
		/* [한국어] 비트 OR로 O_NONBLOCK 추가. */
	} else {
		/* [한국어] nonblocking 해제 요청. */
		if (!(flag & O_NONBLOCK)) {
			/* [한국어] 이미 비활성 → 변경 불필요. */
			return 0;
		}

		flag &= ~O_NONBLOCK;
		/* [한국어] 비트 AND-NOT으로 O_NONBLOCK 제거. */
	}

	if (fcntl(fd, F_SETFL, flag) < 0) {
		/* [한국어] 변경된 플래그를 커널에 반영. 실패 사유는 fd 권한/타입. */
		SPDK_ERRLOG("fcntl can't set %sblocking mode, fd: %d (%s)\n", set ? "non" : "", fd,
			    spdk_strerror(errno));
		return -errno;
	}

	return 0;
	/* [한국어] 정상 변경 완료. */
}

/*
 * [한국어]
 * spdk_fd_set_nonblock - fd를 nonblocking 모드로 전환.
 *
 * 호출 체인: 호출자(예: 소켓 셋업) → spdk_fd_set_nonblock →
 *           fd_update_nonblock(true).
 */
int
spdk_fd_set_nonblock(int fd)
{
	return fd_update_nonblock(fd, true);
	/* [한국어] 공통 헬퍼에 위임. */
}

/*
 * [한국어]
 * spdk_fd_clear_nonblock - fd의 nonblocking 모드 해제(blocking으로).
 *
 * 사용 사례: blocking 동기 read/write가 필요한 일회성 시그널 fd 등.
 */
int
spdk_fd_clear_nonblock(int fd)
{
	return fd_update_nonblock(fd, false);
	/* [한국어] 공통 헬퍼에 위임. */
}
