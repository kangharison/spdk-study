/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK virtio vhost-user 트랜스포트 (virtio_vhost_user.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK virtio 이니시에이터의 **vhost-user 백엔드(=virtio_user)** 를 구현한다.
 * vhost-user는 원래 QEMU가 외부 유저스페이스 데이터플레인(예: DPDK vhost-user 백엔드)과
 * UNIX 도메인 소켓 위에서 vring 정보를 교환하기 위해 정의한 프로토콜이다. SPDK는 이 프로토콜의
 * **드라이버 측**(QEMU 역할)을 직접 흉내내어, 다른 프로세스(예: 또 다른 SPDK target,
 * QEMU vhost-user-blk)와 통신한다. 본 파일의 책임:
 *  1) UNIX 소켓 연결(vhost_user_setup) + cmsg(SCM_RIGHTS)로 fd 전달 가능한 sendmsg 구현,
 *  2) vhost-user 메시지 송수신 헬퍼(vhost_user_write/read) — 18종 메시지 dispatcher,
 *  3) /proc/self/maps를 파싱해 hugepage 파일 경로/주소 수집 → SET_MEM_TABLE으로 백엔드에
 *     SPDK 프로세스의 모든 hugepage 메모리를 매핑하게 함 (백엔드가 동일 VA로 vring을 읽기
 *     위한 핵심 단계),
 *  4) 큐 셋업: kickfd(이니시에이터→백엔드 통지), callfd(백엔드→이니시에이터 통지) eventfd
 *     생성 + 백엔드에 SET_VRING_NUM/BASE/ADDR/KICK 메시지 전송,
 *  5) virtio status에 따라 START/STOP_DEVICE 자동 전환 (set_status 콜백 안에서),
 *  6) read/write_dev_config: VHOST_USER_GET/SET_CONFIG 메시지로 device-specific cfg R/W,
 *  7) notify_queue: kickfd write로 백엔드 깨움.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 프로세스 A (이니시에이터)        SPDK 프로세스 B 또는 QEMU (백엔드)
 *   bdev_virtio                              vhost-user-blk / virtio_user_target
 *   ↓ virtio_user_dev_init(socket path)
 *   [이 파일: virtio_vhost_user.c]
 *   ↓ vhost_user_sock (메시지 송수신)
 *   UNIX 도메인 소켓 ─────────────────────→ 백엔드의 vhost-user listener
 *   (kickfd / callfd / hugepage fds 전달)
 *   ↑ virtio.c의 vring 관리 콜백이 ops로 호출
 * 실행 컨텍스트: SPDK reactor 위 호스트 유저스페이스. polled-mode이지만
 * notify_queue는 kickfd write로 백엔드를 wake — 백엔드도 폴링이면 무시.
 *
 * === 타 모듈과의 연결 ===
 * - 상위 의존: virtio.c (vring 관리), bdev_virtio (init 경로)
 * - 하위 의존:
 *     spdk_internal/vhost_user.h: 메시지 페이로드 구조체와 enum vhost_user_request
 *     spdk_mem_map_*           : hugepage 등록 변경 알림 콜백 — SET_MEM_TABLE 재전송 트리거
 *     /proc/self/maps          : hugepage 파일 경로 발견 (DPDK가 만든 hugefs 매핑)
 * - 공유 자료구조:
 *     struct virtio_user_dev   : 사설 컨텍스트 (vhostfd, callfds[], kickfds[], vrings[], status, mem_map)
 *     struct vhost_user_msg    : 메시지 헤더 + union 페이로드 (lib 외부)
 *     struct vring             : virtio.c와 공유 — desc/avail/used 포인터
 * - 데이터 흐름: virtio.c가 ops 호출 → vhost_user_sock → sendmsg(소켓+fd) → 백엔드 처리 → recv
 *
 * === 주요 함수/구조체 요약 ===
 *  - virtio_user_dev_init       : 진입점 — 소켓 setup + SET_OWNER 송신
 *  - vhost_user_sock            : 메시지 dispatch — 18종 request별 페이로드 채움 + reply 처리
 *  - vhost_user_write/read      : sendmsg/recv 래퍼 (cmsg fd 전달 포함)
 *  - get_hugepage_file_info     : /proc/self/maps에서 hugefs %smap_%d 파일 발견
 *  - prepare_vhost_memory_user  : SET_MEM_TABLE 메시지 페이로드(메모리 region들 + fd[]) 준비
 *  - virtio_user_set_status     : DRIVER_OK / RESET 전이를 자동으로 START/STOP_DEVICE에 매핑
 *  - virtio_user_setup_queue    : eventfd(kick/call) 생성 + 큐 메모리 할당 + SET_VRING_ENABLE
 *  - virtio_user_kick_queue     : SET_VRING_NUM/BASE/ADDR/KICK 시퀀스로 큐 활성
 *  - virtio_user_notify_queue   : kickfd에 8바이트 1 write — 백엔드 wake
 *  - virtio_user_map_notify     : DPDK가 새 hugepage를 등록하면 SET_MEM_TABLE 재전송
 */

#include "spdk/stdinc.h"  /* [한국어] 표준 라이브러리 추상화. */

#include <sys/eventfd.h>   /* [한국어] eventfd(2) — kickfd/callfd 생성에 사용. */

#include "spdk/string.h"   /* [한국어] spdk_strerror — errno → 문자열. */
#include "spdk/config.h"   /* [한국어] SPDK 빌드 옵션 매크로. */
#include "spdk/util.h"     /* [한국어] SPDK_ALIGN_CEIL 등. */

#include "spdk_internal/virtio.h"     /* [한국어] virtio 공통 정의. */
#include "spdk_internal/vhost_user.h" /* [한국어] vhost_user_msg, vhost_user_request enum 등 메시지 정의. */

/* The version of the protocol we support */
/* [한국어] vhost-user 프로토콜 버전 — 메시지 flags 필드에 OR해서 호환성 표시. */
#define VHOST_USER_VERSION    0x1

/* [한국어] 우리가 지원하는 vhost-user 프로토콜 피처:
 * - F_MQ: 멀티큐 (디바이스가 여러 큐 노출 — virtio-scsi 등)
 * - F_CONFIG: GET/SET_CONFIG 메시지 지원 (device-specific cfg R/W) */
#define VIRTIO_USER_SUPPORTED_PROTOCOL_FEATURES \
	((1ULL << VHOST_USER_PROTOCOL_F_MQ) | \
	(1ULL << VHOST_USER_PROTOCOL_F_CONFIG))

/*
 * [한국어]
 * struct virtio_user_dev - vhost-user 트랜스포트 사설 컨텍스트 (virtio_dev->ctx)
 *
 * vhost-user 소켓 fd, 큐별 kick/call eventfd, vring 메타 캐시, 메모리 맵 등을 보관.
 * SPDK가 vhost-user의 "드라이버 측"이므로 status를 직접 추적해야 한다 (가상 PCI cfg 없음).
 */
struct virtio_user_dev {
	int		vhostfd;
	/* [한국어] 백엔드 vhost-user UNIX 소켓 fd.
	 * 설정자: vhost_user_setup이 connect() 후 저장.
	 * 읽는 자: 모든 vhost_user_sock 호출이 이 fd로 sendmsg/recv.
	 * 동기화: 메시지는 직렬 송수신 — 호출자(주로 init/설정 경로)가 단일 thread 보장. */

	int		callfds[SPDK_VIRTIO_MAX_VIRTQUEUES];
	/* [한국어] 큐별 callfd — 백엔드가 used 게시 시 이 fd에 write로 통지.
	 * 설정자: virtio_user_setup_queue에서 eventfd 생성 후 저장.
	 * 읽는 자: virtio_user_create_queue가 SET_VRING_CALL 메시지로 백엔드에 fd 전달.
	 * 동기화: 큐별 단일 thread 사용 — 인덱스로 분리. */

	int		kickfds[SPDK_VIRTIO_MAX_VIRTQUEUES];
	/* [한국어] 큐별 kickfd — 이니시에이터가 avail 게시 후 이 fd에 write로 백엔드 통지.
	 * 설정자: virtio_user_setup_queue에서 eventfd 생성 후 저장.
	 * 읽는 자: virtio_user_kick_queue가 SET_VRING_KICK으로 fd 전달, virtio_user_notify_queue가 write. */

	uint32_t	queue_size;
	/* [한국어] 모든 큐가 공유하는 큐 크기 (사용자 지정).
	 * 설정자: virtio_user_dev_init 인자.
	 * 읽는 자: virtio_user_get_queue_size 콜백. */

	uint8_t		status;
	/* [한국어] virtio 디바이스 status 비트 캐시 — 백엔드는 별도 status 레지스터 없음.
	 * 설정자: virtio_user_set_status가 비트 누적 또는 RESET.
	 * 읽는 자: virtio_user_get_status. */

	bool		is_stopping;
	/* [한국어] virtio_user_unregister_mem 진입 후 true — map_notify가 종료 단계임을 인지.
	 * 설정자: virtio_user_unregister_mem.
	 * 읽는 자: virtio_user_map_notify (assert 분기 회피용). */

	char		path[PATH_MAX];
	/* [한국어] vhost-user UNIX 소켓 경로 — 디버그 + write_json_config의 traddr.
	 * 설정자: virtio_user_dev_init에서 복사. */

	uint64_t	protocol_features;
	/* [한국어] 협상된 vhost-user 프로토콜 피처 비트마스크 (F_MQ, F_CONFIG 등).
	 * 설정자: virtio_user_set_features에서 GET/SET_PROTOCOL_FEATURES 후 저장.
	 * 읽는 자: read/write_dev_config가 F_CONFIG 검사, virtio_user_start_device가 F_MQ 검사. */

	struct vring	vrings[SPDK_VIRTIO_MAX_VIRTQUEUES];
	/* [한국어] 큐별 vring 포인터 캐시 (desc/avail/used VA + num).
	 * 설정자: virtio_user_setup_queue가 ring 영역 분할 후 저장.
	 * 읽는 자: virtio_user_kick_queue가 vring->num을 SET_VRING_NUM에, virtio_user_set_vring_addr가 VA들을 SET_VRING_ADDR에 사용. */

	struct spdk_mem_map *mem_map;
	/* [한국어] hugepage 등록 변경을 받기 위한 SPDK memory map.
	 * 설정자: virtio_user_register_mem이 alloc.
	 * 읽는 자: SPDK env가 새 hugepage 등록 시 virtio_user_map_notify 콜백 트리거.
	 * 동기화: spdk_mem_map 내부에서 처리. */
};

/*
 * [한국어]
 * vhost_user_write - 메시지 + (선택) fd 배열을 UNIX 소켓에 sendmsg 전송
 *
 * @fd     : vhostfd (UNIX 소켓)
 * @buf    : 메시지 본문 포인터
 * @len    : 메시지 길이 (헤더 + payload)
 * @fds    : 전달할 fd 배열 (kickfd/callfd/hugepage fd 등)
 * @fd_num : fds 길이
 * @return: 0 성공, -errno
 *
 * UNIX 소켓에서 fd를 다른 프로세스로 전달하려면 cmsg(SCM_RIGHTS) ancillary data를 써야 한다.
 * sendmsg가 fd 자체가 아닌 fd가 가리키는 OS 객체를 복제 전송 — 백엔드 측에 새 fd가 생긴다.
 * EINTR로 인터럽트되면 재시도.
 *
 * 호출 체인:
 *   vhost_user_sock → [vhost_user_write] → kernel(sendmsg)
 */
static int
vhost_user_write(int fd, void *buf, int len, int *fds, int fd_num)
{
	int r;                                        /* [한국어] sendmsg 결과. */
	struct msghdr msgh;                           /* [한국어] sendmsg 인자 묶음. */
	struct iovec iov;                             /* [한국어] 본문 iovec. */
	size_t fd_size = fd_num * sizeof(int);        /* [한국어] cmsg에 담을 fd 배열 바이트 크기. */
	char control[CMSG_SPACE(fd_size)];            /* [한국어] cmsg 영역 — CMSG_SPACE로 정렬된 크기 계산. */
	struct cmsghdr *cmsg;                         /* [한국어] cmsg 헤더. */

	memset(&msgh, 0, sizeof(msgh));               /* [한국어] 안전 초기화. */
	memset(control, 0, sizeof(control));

	iov.iov_base = (uint8_t *)buf;                /* [한국어] 메시지 본문. */
	iov.iov_len = len;

	msgh.msg_iov = &iov;
	msgh.msg_iovlen = 1;

	if (fds && fd_num > 0) {                       /* [한국어] fd 전달이 필요한 메시지(SET_MEM_TABLE 등). */
		msgh.msg_control = control;
		msgh.msg_controllen = sizeof(control);
		cmsg = CMSG_FIRSTHDR(&msgh);          /* [한국어] 첫 cmsg 헤더 위치. */
		if (!cmsg) {
			SPDK_WARNLOG("First HDR is NULL\n"); /* [한국어] 정상 케이스에서는 발생 안 함. */
			return -EIO;
		}
		cmsg->cmsg_len = CMSG_LEN(fd_size);   /* [한국어] cmsg 페이로드 크기. */
		cmsg->cmsg_level = SOL_SOCKET;        /* [한국어] 소켓 수준 cmsg. */
		cmsg->cmsg_type = SCM_RIGHTS;         /* [한국어] fd(=권한) 전달 타입. */
		memcpy(CMSG_DATA(cmsg), fds, fd_size); /* [한국어] cmsg 페이로드에 fd 배열 복사. */
	} else {
		msgh.msg_control = NULL;
		msgh.msg_controllen = 0;
	}

	do {
		r = sendmsg(fd, &msgh, 0);            /* [한국어] 시스템 콜 — 메시지 전송. */
	} while (r < 0 && errno == EINTR);            /* [한국어] 시그널 인터럽트 재시도. */

	if (r == -1) {
		return -errno;                         /* [한국어] 다른 에러 — 호출자에 음수 errno로 반환. */
	}

	return 0;
}

/*
 * [한국어]
 * vhost_user_read - vhost-user 메시지 1건 수신 (헤더 → 페이로드)
 *
 * @fd : vhostfd
 * @msg: [out] 메시지 구조체
 * @return: 0 성공, -errno/-EBUSY/-EIO
 *
 * 동작:
 *   1) recv로 헤더(VHOST_USER_HDR_SIZE) 수신 — request type, flags, payload size 포함
 *   2) flags 검증: REPLY_MASK | VERSION 정확히 일치해야 — 다른 값이면 잘못된 응답
 *   3) payload 크기 검증 (오버플로 방지)
 *   4) payload 추가 recv (size > 0 이면)
 *
 * 호출 체인:
 *   vhost_user_sock (need_reply 케이스) → [vhost_user_read] → kernel(recv)
 */
static int
vhost_user_read(int fd, struct vhost_user_msg *msg)
{
	uint32_t valid_flags = VHOST_USER_REPLY_MASK | VHOST_USER_VERSION; /* [한국어] 유효 응답 flags 셋. */
	ssize_t ret;
	size_t sz_hdr = VHOST_USER_HDR_SIZE, sz_payload; /* [한국어] 헤더 크기와 payload 크기. */

	ret = recv(fd, (void *)msg, sz_hdr, 0);           /* [한국어] 헤더 먼저. */
	if ((size_t)ret != sz_hdr) {
		SPDK_WARNLOG("Failed to recv msg hdr: %zd instead of %zu.\n",
			     ret, sz_hdr);
		if (ret == -1) {
			return -errno;
		} else {
			return -EBUSY;  /* [한국어] partial read — 백엔드 비정상. */
		}
	}

	/* validate msg flags */
	if (msg->flags != (valid_flags)) {
		SPDK_WARNLOG("Failed to recv msg: flags %"PRIx32" instead of %"PRIx32".\n",
			     msg->flags, valid_flags);
		return -EIO;                              /* [한국어] 프로토콜 위반. */
	}

	sz_payload = msg->size;

	if (sz_payload > VHOST_USER_PAYLOAD_SIZE) {       /* [한국어] payload 크기가 union보다 크면 부적합. */
		SPDK_WARNLOG("Received oversized msg: payload size %zu > available space %zu\n",
			     sz_payload, VHOST_USER_PAYLOAD_SIZE);
		return -EIO;
	}

	if (sz_payload) {
		ret = recv(fd, (void *)((char *)msg + sz_hdr), sz_payload, 0); /* [한국어] payload 수신 — 헤더 뒤. */
		if ((size_t)ret != sz_payload) {
			SPDK_WARNLOG("Failed to recv msg payload: %zd instead of %"PRIu32".\n",
				     ret, msg->size);
			if (ret == -1) {
				return -errno;
			} else {
				return -EBUSY;
			}
		}
	}

	return 0;
}

/*
 * [한국어]
 * struct hugepage_file_info - /proc/self/maps에서 발견한 hugepage 파일 메타
 *
 * SET_MEM_TABLE 메시지 작성 시 SPDK 프로세스의 hugepage 메모리를 백엔드에 등록하기 위한 정보.
 */
struct hugepage_file_info {
	uint64_t addr;            /**< virtual addr */
	/* [한국어] 매핑 시작 가상 주소 — vhost-user의 guest_phys_addr/userspace_addr 둘 다에 그대로 사용
	 * (vhost-user는 SPDK가 driver 역할이므로 guest_phys = host VA = userspace 일치). */

	size_t   size;            /**< the file size */
	/* [한국어] 매핑 길이 — 인접 매핑이면 누적 합산. */

	char     path[PATH_MAX];  /**< path to backing file */
	/* [한국어] hugepage 파일 경로 — open 후 fd 전달에 사용. */
};

/* Two possible options:
 * 1. Match HUGEPAGE_INFO_FMT to find the file storing struct hugepage_file
 * array. This is simple but cannot be used in secondary process because
 * secondary process will close and munmap that file.
 * 2. Match HUGEFILE_FMT to find hugepage files directly.
 *
 * We choose option 2.
 */
/*
 * [한국어]
 * get_hugepage_file_info - /proc/self/maps 파싱하여 hugepage 파일 정보 수집
 *
 * @hugepages: [out] 발견한 hugepage 정보 배열
 * @max      : 배열 최대 크기
 * @return: 발견한 항목 개수 (>=0), 음수 errno
 *
 * /proc/self/maps의 각 라인은 "vstart-vend perm offset dev inode pathname" 포맷.
 * pathname이 "<dir>/<prefix>map_<index>"로 끝나면 (DPDK rte_eal의 HUGEFILE_FMT) hugepage 매핑.
 * 인접한 같은 경로 매핑은 하나로 통합 (size 누적).
 *
 * 실행 컨텍스트: virtio_user_dev_setup → vhost_user_sock(SET_MEM_TABLE) 경로의 단일 thread.
 *
 * 호출 체인:
 *   prepare_vhost_memory_user → [get_hugepage_file_info]
 */
static int
get_hugepage_file_info(struct hugepage_file_info hugepages[], int max)
{
	int idx, rc;                       /* [한국어] 발견 인덱스, 결과 코드. */
	FILE *f;                            /* [한국어] /proc/self/maps 파일 포인터. */
	char buf[BUFSIZ], *tmp, *tail;      /* [한국어] 라인 버퍼와 진행 포인터. */
	char *str_underline, *str_start;    /* [한국어] "map_" 패턴 탐색용. */
	int huge_index;                     /* [한국어] map_%d의 %d 값 (검증용). */
	uint64_t v_start, v_end;            /* [한국어] 매핑 시작/끝 VA. */

	f = fopen("/proc/self/maps", "r");
	if (!f) {
		SPDK_ERRLOG("cannot open /proc/self/maps\n");
		rc = -errno;
		assert(rc < 0); /* scan-build hack */
		return rc;
	}

	idx = 0;
	while (fgets(buf, sizeof(buf), f) != NULL) {  /* [한국어] 라인 단위 순회. */
		if (sscanf(buf, "%" PRIx64 "-%" PRIx64, &v_start, &v_end) < 2) {
			SPDK_ERRLOG("Failed to parse address\n");
			rc = -EIO;
			goto out;
		}

		/* [한국어] /proc/self/maps의 각 라인 형식:
		 * "addr perm offset dev inode pathname" — 공백 5번 건너뛰면 pathname 시작. */
		tmp = strchr(buf, ' ') + 1; /** skip address */
		tmp = strchr(tmp, ' ') + 1; /** skip perm */
		tmp = strchr(tmp, ' ') + 1; /** skip offset */
		tmp = strchr(tmp, ' ') + 1; /** skip dev */
		tmp = strchr(tmp, ' ') + 1; /** skip inode */
		while (*tmp == ' ') {       /** skip spaces */
			tmp++;
		}
		tail = strrchr(tmp, '\n');  /** remove newline if exists */
		if (tail) {
			*tail = '\0';
		}

		/* Match HUGEFILE_FMT, aka "%s/%smap_%d",
		 * which is defined in eal_filesystem.h
		 */
		/* [한국어] DPDK는 hugepage를 "<mountpt>/<prefix>map_<index>" 형태 파일로 만든다. */
		str_underline = strrchr(tmp, '_');  /* [한국어] 맨 마지막 '_' — map_N의 _. */
		if (!str_underline) {
			continue;                    /* [한국어] map_ 패턴 없음 — 일반 매핑 스킵. */
		}

		str_start = str_underline - strlen("map");  /* [한국어] "map" 시작점 후보. */
		if (str_start < tmp) {                      /* [한국어] 경계 검사 — buf 앞쪽 침범 방지. */
			continue;
		}

		if (sscanf(str_start, "map_%d", &huge_index) != 1) { /* [한국어] "map_<숫자>" 매칭. */
			continue;
		}

		if (idx >= max) {
			SPDK_ERRLOG("Exceed maximum of %d\n", max);
			rc = -ENOSPC;
			goto out;
		}

		/* [한국어] 직전 항목과 같은 경로이고 인접 VA면 통합 — DPDK가 같은 hugepage 파일을
		 * 여러 매핑으로 분리할 수 있으므로 vhost-user region 수 절약. */
		if (idx > 0 &&
		    strncmp(tmp, hugepages[idx - 1].path, PATH_MAX) == 0 &&
		    v_start == hugepages[idx - 1].addr + hugepages[idx - 1].size) {
			hugepages[idx - 1].size += (v_end - v_start);
			continue;
		}

		hugepages[idx].addr = v_start;
		hugepages[idx].size = v_end - v_start;
		snprintf(hugepages[idx].path, PATH_MAX, "%s", tmp);
		idx++;
	}

	rc = idx;
out:
	fclose(f);
	return rc;
}

/*
 * [한국어]
 * prepare_vhost_memory_user - SET_MEM_TABLE 메시지 페이로드(메모리 region들 + fd 배열) 준비
 *
 * @msg: [out] 메시지 본문 (msg->payload.memory에 region들 채움)
 * @fds: [out] hugepage 파일 fd 배열 (cmsg로 전달될 것)
 * @return: 0 성공
 *
 * 동작:
 *   1) get_hugepage_file_info로 hugepage 매핑 수집
 *   2) 각 region에 (guest_phys_addr=VA, userspace_addr=VA, size, flags=0) 채우기
 *   3) 각 hugepage 파일 open으로 fd 획득 — sendmsg가 cmsg로 백엔드에 전달
 * 백엔드는 받은 fd를 mmap하여 같은 메모리(=같은 hugepage)를 자신의 주소공간에 매핑 →
 * SPDK가 보낸 vring 주소를 백엔드도 동일 VA로 접근 가능.
 *
 * 호출 체인:
 *   vhost_user_sock(SET_MEM_TABLE) → [prepare_vhost_memory_user] → get_hugepage_file_info
 */
static int
prepare_vhost_memory_user(struct vhost_user_msg *msg, int fds[])
{
	int i, num;
	struct hugepage_file_info hugepages[VHOST_USER_MEMORY_MAX_NREGIONS]; /* [한국어] 최대 region 수만큼 임시. */

	num = get_hugepage_file_info(hugepages, VHOST_USER_MEMORY_MAX_NREGIONS);
	if (num < 0) {
		SPDK_ERRLOG("Failed to prepare memory for vhost-user\n");
		return num;
	}

	for (i = 0; i < num; ++i) {
		/* the memory regions are unaligned */
		/* [한국어] vhost-user는 guest_phys_addr를 VA와 동일하게 두고 userspace_addr도 VA — IOVA=VA 방식.
		 * 백엔드가 mmap 후 (userspace_addr - userspace_addr_in_remote)로 변환해 SPDK 측 vring 접근. */
		msg->payload.memory.regions[i].guest_phys_addr = hugepages[i].addr; /* use vaddr! */
		msg->payload.memory.regions[i].userspace_addr = hugepages[i].addr;
		msg->payload.memory.regions[i].memory_size = hugepages[i].size;
		msg->payload.memory.regions[i].flags_padding = 0;
		fds[i] = open(hugepages[i].path, O_RDWR); /* [한국어] hugepage 파일 fd — cmsg로 백엔드에 전달. */
	}

	msg->payload.memory.nregions = num;       /* [한국어] region 수. */
	msg->payload.memory.padding = 0;

	return 0;
}

/* [한국어] 디버그 로그용 메시지 이름 테이블. */
static const char *const vhost_msg_strings[VHOST_USER_MAX] = {
	[VHOST_USER_SET_OWNER] = "VHOST_SET_OWNER",                         /* [한국어] 디바이스 소유 선언. */
	[VHOST_USER_RESET_OWNER] = "VHOST_RESET_OWNER",                     /* [한국어] 소유 반환. */
	[VHOST_USER_SET_FEATURES] = "VHOST_SET_FEATURES",                   /* [한국어] virtio 피처 협상. */
	[VHOST_USER_GET_FEATURES] = "VHOST_GET_FEATURES",                   /* [한국어] 디바이스 피처 query. */
	[VHOST_USER_SET_VRING_CALL] = "VHOST_SET_VRING_CALL",               /* [한국어] 큐의 callfd 등록. */
	[VHOST_USER_GET_PROTOCOL_FEATURES] = "VHOST_USER_GET_PROTOCOL_FEATURES", /* [한국어] 프로토콜 피처 query. */
	[VHOST_USER_SET_PROTOCOL_FEATURES] = "VHOST_USER_SET_PROTOCOL_FEATURES", /* [한국어] 프로토콜 피처 set. */
	[VHOST_USER_SET_VRING_NUM] = "VHOST_SET_VRING_NUM",                 /* [한국어] 큐 크기 통보. */
	[VHOST_USER_SET_VRING_BASE] = "VHOST_SET_VRING_BASE",               /* [한국어] 큐 시작 인덱스 set. */
	[VHOST_USER_GET_VRING_BASE] = "VHOST_GET_VRING_BASE",               /* [한국어] 큐 정지 + 마지막 인덱스 query. */
	[VHOST_USER_SET_VRING_ADDR] = "VHOST_SET_VRING_ADDR",               /* [한국어] desc/avail/used VA 통보. */
	[VHOST_USER_SET_VRING_KICK] = "VHOST_SET_VRING_KICK",               /* [한국어] 큐의 kickfd 등록. */
	[VHOST_USER_SET_MEM_TABLE] = "VHOST_SET_MEM_TABLE",                 /* [한국어] hugepage region + fd 등록. */
	[VHOST_USER_SET_VRING_ENABLE] = "VHOST_SET_VRING_ENABLE",           /* [한국어] 멀티큐: 큐 활성/비활성. */
	[VHOST_USER_GET_QUEUE_NUM] = "VHOST_USER_GET_QUEUE_NUM",            /* [한국어] 백엔드 최대 큐 수 query. */
	[VHOST_USER_GET_CONFIG] = "VHOST_USER_GET_CONFIG",                  /* [한국어] device-specific cfg read. */
	[VHOST_USER_SET_CONFIG] = "VHOST_USER_SET_CONFIG",                  /* [한국어] device-specific cfg write. */
};

/*
 * [한국어]
 * vhost_user_sock - vhost-user 메시지 dispatcher (request 종류별 페이로드 채움 + reply 처리)
 *
 * @dev: 대상 디바이스 (vhostfd 보유)
 * @req: 메시지 종류 (enum vhost_user_request)
 * @arg: 요청별 사용자 입출력 (페이로드)
 * @return: 0 성공, 음수 errno
 *
 * 동작:
 *   1) request별 페이로드/플래그/fds 준비
 *   2) need_reply가 1이면 응답 읽기
 *   3) 응답이 있는 경우 응답 페이로드를 arg로 출력
 * 일부 메시지(SET_MEM_TABLE)는 prepare_vhost_memory_user를 통해 hugepage fd 배열을 cmsg로 전달.
 * 응답 후에도 보낸 fd들은 close (백엔드가 이미 dup받았으므로).
 *
 * 실행 컨텍스트: 단일 thread 직렬 호출 — 메시지 순서가 프로토콜에서 중요.
 *
 * 호출 체인:
 *   모든 ops 콜백 / start/stop_device / virtio_user_dev_init → [vhost_user_sock] →
 *      vhost_user_write/read
 */
static int
vhost_user_sock(struct virtio_user_dev *dev,
		enum vhost_user_request req,
		void *arg)
{
	struct vhost_user_msg msg;                                 /* [한국어] 송신 메시지 본문. */
	struct vhost_vring_file *file = 0;                          /* [한국어] KICK/CALL 메시지용 (vring + fd). */
	int need_reply = 0;                                         /* [한국어] 응답 read 필요 여부. */
	int fds[VHOST_USER_MEMORY_MAX_NREGIONS];                    /* [한국어] cmsg로 전달할 fd 배열. */
	int fd_num = 0;                                             /* [한국어] fd 개수. */
	int i, len, rc;
	int vhostfd = dev->vhostfd;                                 /* [한국어] 소켓 fd 캐시. */

	SPDK_DEBUGLOG(virtio_user, "sent message %d = %s\n", req, vhost_msg_strings[req]);

	msg.request = req;
	msg.flags = VHOST_USER_VERSION;                              /* [한국어] flags = 프로토콜 버전. */
	msg.size = 0;

	switch (req) {
	case VHOST_USER_GET_FEATURES:
	case VHOST_USER_GET_PROTOCOL_FEATURES:
	case VHOST_USER_GET_QUEUE_NUM:
		need_reply = 1;                                       /* [한국어] 단순 query — 응답 필요. */
		break;

	case VHOST_USER_SET_FEATURES:
	case VHOST_USER_SET_LOG_BASE:
	case VHOST_USER_SET_PROTOCOL_FEATURES:
		msg.payload.u64 = *((__u64 *)arg);                    /* [한국어] 64-bit 값 페이로드. */
		msg.size = sizeof(msg.payload.u64);
		break;

	case VHOST_USER_SET_OWNER:
	case VHOST_USER_RESET_OWNER:
		break;                                                /* [한국어] 페이로드 없음. */

	case VHOST_USER_SET_MEM_TABLE:
		rc = prepare_vhost_memory_user(&msg, fds);            /* [한국어] /proc/self/maps에서 region+fd 수집. */
		if (rc < 0) {
			return rc;
		}
		fd_num = msg.payload.memory.nregions;                 /* [한국어] region 수만큼 fd 전달. */
		msg.size = sizeof(msg.payload.memory.nregions);
		msg.size += sizeof(msg.payload.memory.padding);
		msg.size += fd_num * sizeof(struct vhost_memory_region); /* [한국어] 가변 길이 region 배열. */
		break;

	case VHOST_USER_SET_LOG_FD:
		fds[fd_num++] = *((int *)arg);                        /* [한국어] 로그 fd 1개 전달. */
		break;

	case VHOST_USER_SET_VRING_NUM:
	case VHOST_USER_SET_VRING_BASE:
	case VHOST_USER_SET_VRING_ENABLE:
		memcpy(&msg.payload.state, arg, sizeof(msg.payload.state)); /* [한국어] {index, num} 상태 페이로드. */
		msg.size = sizeof(msg.payload.state);
		break;

	case VHOST_USER_GET_VRING_BASE:
		memcpy(&msg.payload.state, arg, sizeof(msg.payload.state)); /* [한국어] 정지 시 마지막 base index 회수. */
		msg.size = sizeof(msg.payload.state);
		need_reply = 1;
		break;

	case VHOST_USER_SET_VRING_ADDR:
		memcpy(&msg.payload.addr, arg, sizeof(msg.payload.addr)); /* [한국어] desc/avail/used VA. */
		msg.size = sizeof(msg.payload.addr);
		break;

	case VHOST_USER_SET_VRING_KICK:
	case VHOST_USER_SET_VRING_CALL:
	case VHOST_USER_SET_VRING_ERR:
		file = arg;
		msg.payload.u64 = file->index & VHOST_USER_VRING_IDX_MASK; /* [한국어] 페이로드는 큐 인덱스. */
		msg.size = sizeof(msg.payload.u64);
		if (file->fd > 0) {
			fds[fd_num++] = file->fd;                  /* [한국어] kickfd/callfd를 cmsg로 전달. */
		} else {
			msg.payload.u64 |= VHOST_USER_VRING_NOFD_MASK; /* [한국어] fd 없음 표시 비트. */
		}
		break;

	case VHOST_USER_GET_CONFIG:
		memcpy(&msg.payload.cfg, arg, sizeof(msg.payload.cfg)); /* [한국어] (offset, size, region) cfg query. */
		msg.size = sizeof(msg.payload.cfg);
		need_reply = 1;
		break;

	case VHOST_USER_SET_CONFIG:
		memcpy(&msg.payload.cfg, arg, sizeof(msg.payload.cfg));
		msg.size = sizeof(msg.payload.cfg);
		break;

	default:
		SPDK_ERRLOG("trying to send unknown msg\n");
		return -EINVAL;
	}

	len = VHOST_USER_HDR_SIZE + msg.size;                          /* [한국어] 총 길이 = 헤더 + payload. */
	rc = vhost_user_write(vhostfd, &msg, len, fds, fd_num);        /* [한국어] sendmsg + cmsg(fd 전달). */
	if (rc < 0) {
		SPDK_ERRLOG("%s failed: %s\n",
			    vhost_msg_strings[req], spdk_strerror(-rc));
		return rc;
	}

	if (req == VHOST_USER_SET_MEM_TABLE)
		for (i = 0; i < fd_num; ++i) {
			close(fds[i]);                          /* [한국어] 백엔드가 dup 받았으므로 SPDK 측 fd는 close. */
		}

	if (need_reply) {
		rc = vhost_user_read(vhostfd, &msg);            /* [한국어] 응답 읽기. */
		if (rc < 0) {
			SPDK_WARNLOG("Received msg failed: %s\n", spdk_strerror(-rc));
			return rc;
		}

		if (req != msg.request) {                       /* [한국어] 응답의 request 타입은 송신과 동일해야 함. */
			SPDK_WARNLOG("Received unexpected msg type\n");
			return -EIO;
		}

		switch (req) {                                  /* [한국어] 응답 페이로드별 분기. */
		case VHOST_USER_GET_FEATURES:
		case VHOST_USER_GET_PROTOCOL_FEATURES:
		case VHOST_USER_GET_QUEUE_NUM:
			if (msg.size != sizeof(msg.payload.u64)) {
				SPDK_WARNLOG("Received bad msg size\n");
				return -EIO;
			}
			*((__u64 *)arg) = msg.payload.u64;       /* [한국어] u64 응답 → arg에 출력. */
			break;
		case VHOST_USER_GET_VRING_BASE:
			if (msg.size != sizeof(msg.payload.state)) {
				SPDK_WARNLOG("Received bad msg size\n");
				return -EIO;
			}
			memcpy(arg, &msg.payload.state,
			       sizeof(struct vhost_vring_state)); /* [한국어] state 응답. */
			break;
		case VHOST_USER_GET_CONFIG:
			if (msg.size != sizeof(msg.payload.cfg)) {
				SPDK_WARNLOG("Received bad msg size\n");
				return -EIO;
			}
			memcpy(arg, &msg.payload.cfg, sizeof(msg.payload.cfg)); /* [한국어] cfg 응답. */
			break;
		default:
			SPDK_WARNLOG("Received unexpected msg type\n");
			return -EBADMSG;
		}
	}

	return 0;
}

/**
 * Set up environment to talk with a vhost user backend.
 *
 * @return
 *   - (-1) if fail;
 *   - (0) if succeed.
 */
/*
 * [한국어]
 * vhost_user_setup - UNIX 도메인 소켓 connect (드라이버 측이 client)
 *
 * @dev: 대상 — dev->path 경로 사용
 * @return: 0 성공, 음수 errno
 *
 * SPDK가 vhost-user의 driver(=client)이므로 백엔드(server)의 소켓에 connect.
 * FD_CLOEXEC를 설정해 자식 프로세스에 누출 방지.
 *
 * 호출 체인:
 *   virtio_user_dev_setup → [vhost_user_setup]
 */
static int
vhost_user_setup(struct virtio_user_dev *dev)
{
	int fd;
	int flag;
	struct sockaddr_un un;
	ssize_t rc;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);  /* [한국어] UNIX 도메인 STREAM 소켓 — 메시지 경계는 protocol-defined. */
	if (fd < 0) {
		SPDK_ERRLOG("socket() error, %s\n", spdk_strerror(errno));
		return -errno;
	}

	flag = fcntl(fd, F_GETFD);
	if (fcntl(fd, F_SETFD, flag | FD_CLOEXEC) < 0) {
		SPDK_ERRLOG("fcntl failed, %s\n", spdk_strerror(errno)); /* [한국어] CLOEXEC 실패는 치명적 아님 — 경고만. */
	}

	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	rc = snprintf(un.sun_path, sizeof(un.sun_path), "%s", dev->path); /* [한국어] sun_path 길이 검증. */
	if (rc < 0 || (size_t)rc >= sizeof(un.sun_path)) {
		SPDK_ERRLOG("socket path too long\n");
		close(fd);
		if (rc < 0) {
			return -errno;
		} else {
			return -EINVAL;
		}
	}
	if (connect(fd, (struct sockaddr *)&un, sizeof(un)) < 0) {
		SPDK_ERRLOG("connect error, %s\n", spdk_strerror(errno)); /* [한국어] 백엔드 미가동 또는 권한 부족. */
		close(fd);
		return -errno;
	}

	dev->vhostfd = fd;       /* [한국어] 이후 모든 메시지가 이 fd로. */
	return 0;
}

/*
 * [한국어]
 * virtio_user_create_queue - SET_VRING_CALL 메시지로 큐 생성 (callfd 등록)
 *
 * @vdev     : 대상
 * @queue_sel: 큐 인덱스
 * @return: vhost_user_sock 결과
 *
 * vhost-user 백엔드는 SET_VRING_CALL 메시지를 받으면 해당 큐 컨텍스트를 할당. 따라서 다른
 * 큐 메시지보다 먼저 호출되어야 한다 (위 주석 참조).
 *
 * 호출 체인:
 *   virtio_user_start_device → virtio_user_queue_setup(create) → [virtio_user_create_queue]
 */
static int
virtio_user_create_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;

	/* Of all per virtqueue MSGs, make sure VHOST_SET_VRING_CALL come
	 * firstly because vhost depends on this msg to allocate virtqueue
	 * pair.
	 */
	struct vhost_vring_file file;

	file.index = queue_sel;
	file.fd = dev->callfds[queue_sel];   /* [한국어] 큐별 callfd — 백엔드가 used 후 write로 통지. */
	return vhost_user_sock(dev, VHOST_USER_SET_VRING_CALL, &file);
}

/*
 * [한국어]
 * virtio_user_set_vring_addr - SET_VRING_ADDR 메시지로 desc/avail/used VA를 백엔드에 전달
 *
 * @vdev     : 대상
 * @queue_sel: 큐 인덱스
 * @return: vhost_user_sock 결과
 *
 * vhost-user의 IOVA = host VA 모델 — SPDK가 보내는 VA를 백엔드도 (mmap된 hugepage에서) 동일 VA로 접근.
 *
 * 호출 체인:
 *   virtio_user_kick_queue → [virtio_user_set_vring_addr]
 */
static int
virtio_user_set_vring_addr(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vring *vring = &dev->vrings[queue_sel];   /* [한국어] setup_queue가 채워둔 ring 메타. */
	struct vhost_vring_addr addr = {
		.index = queue_sel,                                   /* [한국어] 큐 인덱스. */
		.desc_user_addr = (uint64_t)(uintptr_t)vring->desc,   /* [한국어] desc 영역 host VA. */
		.avail_user_addr = (uint64_t)(uintptr_t)vring->avail, /* [한국어] avail 영역 host VA. */
		.used_user_addr = (uint64_t)(uintptr_t)vring->used,   /* [한국어] used 영역 host VA. */
		.log_guest_addr = 0,                                   /* [한국어] dirty page 로깅 미사용 — 0. */
		.flags = 0, /* disable log */                          /* [한국어] 로그 플래그 없음. */
	};

	return vhost_user_sock(dev, VHOST_USER_SET_VRING_ADDR, &addr);
}

/*
 * [한국어]
 * virtio_user_kick_queue - 큐 활성화 시퀀스 (NUM → BASE → ADDR → KICK)
 *
 * @vdev     : 대상
 * @queue_sel: 큐 인덱스
 * @return: 0 성공, 음수 errno
 *
 * vhost-user 프로토콜은 SET_VRING_KICK 메시지가 도착해야 백엔드가 "초기화 완료"로 인식하고
 * 처리 시작 — 따라서 KICK이 마지막. 시퀀스:
 *   1) SET_VRING_NUM: 큐 크기 통보
 *   2) SET_VRING_BASE: 시작 인덱스 (보통 0)
 *   3) SET_VRING_ADDR: desc/avail/used VA
 *   4) SET_VRING_KICK: 이니시에이터 측 kickfd 등록
 *
 * 호출 체인:
 *   virtio_user_start_device → virtio_user_queue_setup(kick) → [virtio_user_kick_queue]
 */
static int
virtio_user_kick_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_file file;
	struct vhost_vring_state state;
	struct vring *vring = &dev->vrings[queue_sel];
	int rc;

	state.index = queue_sel;
	state.num = vring->num;       /* [한국어] 큐 크기 (디스크립터 수). */
	rc = vhost_user_sock(dev, VHOST_USER_SET_VRING_NUM, &state);
	if (rc < 0) {
		return rc;
	}

	state.index = queue_sel;
	state.num = 0; /* no reservation */ /* [한국어] base 인덱스 0 — 큐가 새로 시작. */
	rc = vhost_user_sock(dev, VHOST_USER_SET_VRING_BASE, &state);
	if (rc < 0) {
		return rc;
	}

	virtio_user_set_vring_addr(vdev, queue_sel); /* [한국어] desc/avail/used VA 통보. */

	/* Of all per virtqueue MSGs, make sure VHOST_USER_SET_VRING_KICK comes
	 * lastly because vhost depends on this msg to judge if
	 * virtio is ready.
	 */
	file.index = queue_sel;
	file.fd = dev->kickfds[queue_sel]; /* [한국어] 이니시에이터의 kickfd — 백엔드 wake용. */
	return vhost_user_sock(dev, VHOST_USER_SET_VRING_KICK, &file);
}

/*
 * [한국어]
 * virtio_user_stop_queue - GET_VRING_BASE 메시지로 큐 정지 + 마지막 인덱스 회수
 *
 * @vdev     : 대상
 * @queue_sel: 큐 인덱스
 * @return: vhost_user_sock 결과
 *
 * vhost-user에서 GET_VRING_BASE는 "큐 정지"의 의미도 가진다 — 백엔드가 이 큐의 처리를 중단하고
 * 마지막으로 처리한 avail 인덱스를 응답.
 *
 * 호출 체인:
 *   virtio_user_stop_device → virtio_user_queue_setup(stop) → [virtio_user_stop_queue]
 */
static int
virtio_user_stop_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_state state;

	state.index = queue_sel;
	state.num = 0;

	return vhost_user_sock(dev, VHOST_USER_GET_VRING_BASE, &state);
}

/*
 * [한국어]
 * virtio_user_queue_setup - 모든 큐에 대해 동일 함수(fn) 적용 헬퍼
 *
 * @vdev: 대상
 * @fn  : 큐별로 호출할 함수 (create_queue / kick_queue / stop_queue 등)
 * @return: 0 성공, 첫 실패 코드
 *
 * 큐 0..max_queues-1 순회로 fn 호출.
 */
static int
virtio_user_queue_setup(struct virtio_dev *vdev,
			int (*fn)(struct virtio_dev *, uint32_t))
{
	uint32_t i;
	int rc;

	for (i = 0; i < vdev->max_queues; ++i) {
		rc = fn(vdev, i);
		if (rc < 0) {
			SPDK_ERRLOG("setup tx vq fails: %"PRIu32".\n", i);
			return rc;
		}
	}

	return 0;
}

/*
 * [한국어]
 * virtio_user_map_notify - DPDK가 새 hugepage 등록 시 호출되는 콜백 (SET_MEM_TABLE 재전송)
 *
 * @cb_ctx: virtio_dev*
 * @map   : SPDK mem_map (미사용)
 * @action: REGISTER/UNREGISTER
 * @vaddr : 변경 주소 (미사용)
 * @size  : 크기 (미사용)
 * @return: 0 성공, -1 실패
 *
 * SPDK는 동적 hugepage 추가/제거 시 등록된 콜백을 호출. virtio-user는 동적 메모리 변경을
 * 정식 지원하지 않으므로 — 초기 등록(mem_map==NULL)과 종료 정리(is_stopping==true)만 허용.
 * 기타 변경은 assert로 잡고 -1 반환.
 *
 * 동작:
 *   1) 변경된 매핑을 일일이 추적하지 않고 SET_MEM_TABLE 메시지를 통째로 재전송 (전체 hugepage 재등록)
 *   2) 후속 GET_FEATURES로 reply-ack 효과 — 백엔드가 SET_MEM_TABLE 처리 완료될 때까지 동기화
 *
 * 실행 컨텍스트: spdk_mem_map 콜백 — 단일 thread (SPDK env init/exit).
 *
 * 호출 체인:
 *   spdk env hugepage 변경 → spdk_mem_map_notify → [virtio_user_map_notify]
 */
static int
virtio_user_map_notify(void *cb_ctx, struct spdk_mem_map *map,
		       enum spdk_mem_map_notify_action action,
		       void *vaddr, size_t size)
{
	struct virtio_dev *vdev = cb_ctx;
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t features;
	int ret;

	/* We do not support dynamic memory allocation with virtio-user.  If this is the
	 * initial notification when the device is started, dev->mem_map will be NULL.  If
	 * this is the final notification when the device is stopped, dev->is_stopping will
	 * be true.  All other cases are unsupported.
	 */
	/* [한국어] 정상 케이스는 init(아직 mem_map=NULL)과 stop(is_stopping=true) 두 가지 — 그 외는 미지원. */
	if (dev->mem_map != NULL && !dev->is_stopping) {
		assert(false);
		SPDK_ERRLOG("Memory map change with active virtio_user_devs not allowed.\n");
		SPDK_ERRLOG("Pre-allocate memory for application using -s (mem_size) option.\n");
		return -1;
	}

	/* We have to resend all mappings anyway, so don't bother with any
	 * page tracking.
	 */
	/* [한국어] 부분 갱신이 아니라 전체 hugepage 다시 보냄 — 페이지별 추적 불필요. */
	ret = vhost_user_sock(dev, VHOST_USER_SET_MEM_TABLE, NULL);
	if (ret < 0) {
		return ret;
	}

	/* Since we might want to use that mapping straight away, we have to
	 * make sure the guest has already processed our SET_MEM_TABLE message.
	 * F_REPLY_ACK is just a feature and the host is not obliged to
	 * support it, so we send a simple message that always has a response
	 * and we wait for that response. Messages are always processed in order.
	 */
	/* [한국어] reply-ack 피처가 없는 백엔드 호환을 위해 GET_FEATURES(반드시 응답하는 메시지)를 후속 송신.
	 * vhost-user는 메시지가 직렬로 처리되므로 GET_FEATURES 응답이 오면 SET_MEM_TABLE도 처리 완료. */
	return vhost_user_sock(dev, VHOST_USER_GET_FEATURES, &features);
}

/*
 * [한국어]
 * virtio_user_register_mem - SPDK mem_map alloc + 콜백 등록
 *
 * @vdev: 대상
 * @return: 0 성공, -1 실패
 *
 * spdk_mem_map은 SPDK 측 hugepage 등록 변경 시 콜백을 호출 — 새 hugepage가 추가되면
 * 즉시 백엔드에 SET_MEM_TABLE 재전송됨.
 *
 * 호출 체인:
 *   virtio_user_start_device → [virtio_user_register_mem] → spdk_mem_map_alloc
 */
static int
virtio_user_register_mem(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	const struct spdk_mem_map_ops virtio_user_map_ops = {
		.notify_cb = virtio_user_map_notify,         /* [한국어] hugepage 변경 콜백. */
		.are_contiguous = NULL                        /* [한국어] 연속성 체크 미사용. */
	};

	dev->mem_map = spdk_mem_map_alloc(0, &virtio_user_map_ops, vdev);
	if (dev->mem_map == NULL) {
		SPDK_ERRLOG("spdk_mem_map_alloc() failed\n");
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * virtio_user_unregister_mem - mem_map 해제 (is_stopping flag 켜기)
 *
 * @vdev: 대상
 *
 * is_stopping을 먼저 set해서 free 동안 발생하는 unregister 콜백이 assert에 걸리지 않게 함.
 *
 * 호출 체인:
 *   virtio_user_stop_device → [virtio_user_unregister_mem] → spdk_mem_map_free
 */
static void
virtio_user_unregister_mem(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;

	dev->is_stopping = true;       /* [한국어] map_notify가 종료 단계임을 인식. */
	spdk_mem_map_free(&dev->mem_map);
}

/*
 * [한국어]
 * virtio_user_start_device - 디바이스 본격 동작 시작 (DRIVER_OK 진입 시)
 *
 * @vdev: 대상
 * @return: 0 성공, 음수 errno
 *
 * 동작:
 *   1) F_MQ 미협상이면 max_queues를 1+fixed로 강제 (단일 데이터 큐만)
 *   2) GET_QUEUE_NUM으로 백엔드 최대 큐 수 query → max_queues 조정
 *   3) 모든 큐에 대해 SET_VRING_CALL (큐 컨텍스트 할당)
 *   4) mem_map 등록 (백엔드에 hugepage 통보)
 *   5) 모든 큐에 대해 kick 시퀀스(NUM/BASE/ADDR/KICK)
 *
 * 실행 컨텍스트: virtio_user_set_status(DRIVER_OK) 경로 — 단일 thread.
 *
 * 호출 체인:
 *   virtio_user_set_status (DRIVER_OK) → [virtio_user_start_device]
 */
static int
virtio_user_start_device(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t host_max_queues;
	int ret;

	if ((dev->protocol_features & (1ULL << VHOST_USER_PROTOCOL_F_MQ)) == 0 &&
	    vdev->max_queues > 1 + vdev->fixed_queues_num) {
		/* [한국어] F_MQ 미협상 — 백엔드는 큐 1개만 지원 → 데이터 큐도 1개로 축소. */
		SPDK_WARNLOG("%s: requested %"PRIu16" request queues, but the "
			     "host doesn't support VHOST_USER_PROTOCOL_F_MQ. "
			     "Only one request queue will be used.\n",
			     vdev->name, vdev->max_queues - vdev->fixed_queues_num);
		vdev->max_queues = 1 + vdev->fixed_queues_num;
	}

	/* negotiate the number of I/O queues. */
	ret = vhost_user_sock(dev, VHOST_USER_GET_QUEUE_NUM, &host_max_queues);
	if (ret < 0) {
		return ret;
	}

	if (vdev->max_queues > host_max_queues + vdev->fixed_queues_num) {
		/* [한국어] 백엔드 한계 초과 — 축소. */
		SPDK_WARNLOG("%s: requested %"PRIu16" request queues"
			     "but only %"PRIu64" available\n",
			     vdev->name, vdev->max_queues - vdev->fixed_queues_num,
			     host_max_queues);
		vdev->max_queues = host_max_queues;
	}

	/* tell vhost to create queues */
	ret = virtio_user_queue_setup(vdev, virtio_user_create_queue); /* [한국어] SET_VRING_CALL × N. */
	if (ret < 0) {
		return ret;
	}

	ret = virtio_user_register_mem(vdev); /* [한국어] hugepage 콜백 등록 — 즉시 SET_MEM_TABLE 송신. */
	if (ret < 0) {
		return ret;
	}

	return virtio_user_queue_setup(vdev, virtio_user_kick_queue); /* [한국어] NUM/BASE/ADDR/KICK × N. */
}

/*
 * [한국어]
 * virtio_user_stop_device - 디바이스 정지 (RESET 진입 시)
 *
 * @vdev: 대상
 * @return: 큐 정지 결과 (mem 해제는 항상 진행)
 *
 * 동작:
 *   1) 모든 큐에 GET_VRING_BASE (=정지)
 *   2) mem_map 해제 (소켓 다운으로 큐 정지가 실패해도 메모리는 반드시 해제)
 *
 * 호출 체인:
 *   virtio_user_set_status (RESET) → [virtio_user_stop_device]
 */
static int
virtio_user_stop_device(struct virtio_dev *vdev)
{
	int ret;

	ret = virtio_user_queue_setup(vdev, virtio_user_stop_queue);
	/* a queue might fail to stop for various reasons, e.g. socket
	 * connection going down, but this mustn't prevent us from freeing
	 * the mem map.
	 */
	/* [한국어] 큐 stop 실패해도 mem_map은 반드시 해제 — 자원 누수 방지. */
	virtio_user_unregister_mem(vdev);
	return ret;
}

/*
 * [한국어]
 * virtio_user_dev_setup - vhost-user 컨텍스트 초기화 (fd 슬롯 -1 + 소켓 connect)
 *
 * @vdev: 대상
 * @return: 0 성공, 음수 errno
 *
 * callfds/kickfds 배열을 -1로 초기화 (eventfd 미생성 표시) 후 vhost_user_setup으로 connect.
 *
 * 호출 체인:
 *   virtio_user_dev_init → [virtio_user_dev_setup] → vhost_user_setup
 */
static int
virtio_user_dev_setup(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint16_t i;

	dev->vhostfd = -1;

	for (i = 0; i < SPDK_VIRTIO_MAX_VIRTQUEUES; ++i) {
		dev->callfds[i] = -1;   /* [한국어] 미생성 표시. */
		dev->kickfds[i] = -1;
	}

	return vhost_user_setup(dev);
}

/*
 * [한국어]
 * virtio_user_read_dev_config - device-specific cfg read (GET_CONFIG 메시지)
 *
 * @vdev  : 대상
 * @offset: cfg 시작 오프셋
 * @dst   : [out] 버퍼
 * @length: 바이트 수
 * @return: 0 성공, -ENOTSUP(F_CONFIG 미협상), 음수 errno
 *
 * F_CONFIG 프로토콜 피처가 협상돼야 사용 가능. 메시지는 전체 cfg region을 가져온 뒤
 * offset부터 length만큼 dst에 복사.
 *
 * 호출 체인:
 *   virtio_dev_read_dev_config → backend_ops->read_dev_cfg(=[virtio_user_read_dev_config])
 */
static int
virtio_user_read_dev_config(struct virtio_dev *vdev, size_t offset,
			    void *dst, int length)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_user_config cfg = {0};
	int rc;

	if ((dev->protocol_features & (1ULL << VHOST_USER_PROTOCOL_F_CONFIG)) == 0) {
		return -ENOTSUP;  /* [한국어] 협상 안 됨 — 메시지 보내지 않음. */
	}

	cfg.offset = 0;
	cfg.size = VHOST_USER_MAX_CONFIG_SIZE;  /* [한국어] 백엔드가 채울 최대 크기 — 전체 region 가져옴. */

	rc = vhost_user_sock(dev, VHOST_USER_GET_CONFIG, &cfg);
	if (rc < 0) {
		SPDK_ERRLOG("get_config failed: %s\n", spdk_strerror(-rc));
		return rc;
	}

	memcpy(dst, cfg.region + offset, length); /* [한국어] 사용자 요청 영역만 복사. */
	return 0;
}

/*
 * [한국어]
 * virtio_user_write_dev_config - device-specific cfg write (SET_CONFIG 메시지)
 *
 * @vdev  : 대상
 * @offset: 오프셋
 * @src   : 데이터
 * @length: 바이트 수
 * @return: 0 성공, -ENOTSUP, 음수 errno
 *
 * 호출 체인:
 *   virtio_dev_write_dev_config → backend_ops->write_dev_cfg(=[virtio_user_write_dev_config])
 */
static int
virtio_user_write_dev_config(struct virtio_dev *vdev, size_t offset,
			     const void *src, int length)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_user_config cfg = {0};
	int rc;

	if ((dev->protocol_features & (1ULL << VHOST_USER_PROTOCOL_F_CONFIG)) == 0) {
		return -ENOTSUP;
	}

	cfg.offset = offset;
	cfg.size = length;
	memcpy(cfg.region, src, length);   /* [한국어] 메시지 region에 사용자 데이터 복사. */

	rc = vhost_user_sock(dev, VHOST_USER_SET_CONFIG, &cfg);
	if (rc < 0) {
		SPDK_ERRLOG("set_config failed: %s\n", spdk_strerror(-rc));
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * virtio_user_set_status - status 전이를 자동으로 START/STOP_DEVICE에 매핑
 *
 * @vdev  : 대상
 * @status: 새 status 값
 *
 * vhost-user는 PCI 같은 status 레지스터가 없으므로 SPDK가 직접 추적. 핵심 전이:
 *   - DRIVER_OK 진입 → virtio_user_start_device (모든 큐 활성)
 *   - RESET 진입 (이전 DRIVER_OK였음) → virtio_user_stop_device
 *   - NEEDS_RESET 상태에서 RESET 외 전이 시도 → 거절
 * 실패 시 NEEDS_RESET 비트 set — 다음 호출이 RESET이어야 함.
 *
 * 호출 체인:
 *   virtio_dev_set_status → backend_ops->set_status(=[virtio_user_set_status])
 */
static void
virtio_user_set_status(struct virtio_dev *vdev, uint8_t status)
{
	struct virtio_user_dev *dev = vdev->ctx;
	int rc = 0;

	if ((dev->status & VIRTIO_CONFIG_S_NEEDS_RESET) &&
	    status != VIRTIO_CONFIG_S_RESET) {
		/* [한국어] NEEDS_RESET 상태에서는 RESET만 허용 — 다른 전이는 무시. */
		rc = -1;
	} else if (status & VIRTIO_CONFIG_S_DRIVER_OK) {
		rc = virtio_user_start_device(vdev);   /* [한국어] DRIVER_OK 진입 → 큐 활성. */
	} else if (status == VIRTIO_CONFIG_S_RESET &&
		   (dev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
		rc = virtio_user_stop_device(vdev);    /* [한국어] DRIVER_OK였다 RESET → 정지. */
	}

	if (rc != 0) {
		dev->status |= VIRTIO_CONFIG_S_NEEDS_RESET; /* [한국어] 실패 시 NEEDS_RESET — 다음은 반드시 RESET. */
	} else {
		dev->status = status;                       /* [한국어] 성공 시 status 갱신. */
	}
}

/*
 * [한국어]
 * virtio_user_get_status - 캐시된 status 반환
 *
 * @vdev: 대상
 * @return: dev->status
 *
 * 호출 체인:
 *   virtio_dev_get_status → backend_ops->get_status(=[virtio_user_get_status])
 */
static uint8_t
virtio_user_get_status(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;

	return dev->status;
}

/*
 * [한국어]
 * virtio_user_get_features - 백엔드 지원 피처 query (GET_FEATURES 메시지)
 *
 * @vdev: 대상
 * @return: 백엔드 보고 64-bit 피처 마스크 (실패 시 0)
 *
 * 호출 체인:
 *   virtio_negotiate_features → backend_ops->get_features(=[virtio_user_get_features])
 */
static uint64_t
virtio_user_get_features(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t features;
	int rc;

	rc = vhost_user_sock(dev, VHOST_USER_GET_FEATURES, &features);
	if (rc < 0) {
		SPDK_ERRLOG("get_features failed: %s\n", spdk_strerror(-rc));
		return 0;
	}

	return features;
}

/*
 * [한국어]
 * virtio_user_set_features - 협상 피처 set + protocol_features 협상
 *
 * @vdev    : 대상
 * @features: 협상 결과 마스크
 * @return: 0 성공, 음수 errno
 *
 * 동작:
 *   1) SET_FEATURES로 virtio 피처 통보
 *   2) negotiated_features 캐싱, modern flag 갱신 (VERSION_1 비트 검사)
 *   3) F_PROTOCOL_FEATURES 협상되면 GET/SET_PROTOCOL_FEATURES로 vhost-user 프로토콜 피처 협상
 *      (F_MQ, F_CONFIG 중 우리가 지원하는 것만 통과)
 *
 * 호출 체인:
 *   virtio_negotiate_features → backend_ops->set_features(=[virtio_user_set_features])
 */
static int
virtio_user_set_features(struct virtio_dev *vdev, uint64_t features)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t protocol_features;
	int ret;

	ret = vhost_user_sock(dev, VHOST_USER_SET_FEATURES, &features);
	if (ret < 0) {
		return ret;
	}

	vdev->negotiated_features = features;
	vdev->modern = virtio_dev_has_feature(vdev, VIRTIO_F_VERSION_1); /* [한국어] virtio 1.x 협상되면 modern flag set. */

	if (!virtio_dev_has_feature(vdev, VHOST_USER_F_PROTOCOL_FEATURES)) {
		/* nothing else to do */
		/* [한국어] 백엔드가 protocol features 미지원 — 추가 협상 불필요. */
		return 0;
	}

	ret = vhost_user_sock(dev, VHOST_USER_GET_PROTOCOL_FEATURES, &protocol_features);
	if (ret < 0) {
		return ret;
	}

	protocol_features &= VIRTIO_USER_SUPPORTED_PROTOCOL_FEATURES; /* [한국어] 우리가 지원하는 것만 (F_MQ | F_CONFIG). */
	ret = vhost_user_sock(dev, VHOST_USER_SET_PROTOCOL_FEATURES, &protocol_features);
	if (ret < 0) {
		return ret;
	}

	dev->protocol_features = protocol_features; /* [한국어] 캐싱 — read/write_dev_config가 F_CONFIG 검사. */
	return 0;
}

/*
 * [한국어]
 * virtio_user_get_queue_size - 큐 크기 반환 (모든 큐 동일 — dev->queue_size)
 *
 * @vdev    : 대상
 * @queue_id: 미사용 (모든 큐가 같은 크기)
 * @return: dev->queue_size
 *
 * vhost-user에서는 큐 크기가 드라이버 측이 결정 (이니시에이터가 dev_init 시 인자로 받음).
 *
 * 호출 체인:
 *   virtio_init_queue → backend_ops->get_queue_size(=[virtio_user_get_queue_size])
 */
static uint16_t
virtio_user_get_queue_size(struct virtio_dev *vdev, uint16_t queue_id)
{
	struct virtio_user_dev *dev = vdev->ctx;

	/* Currently each queue has same queue size */
	return dev->queue_size;
}

/*
 * [한국어]
 * virtio_user_setup_queue - 큐별 eventfd 생성 + vring 메모리 할당 + (F_PROTOCOL이면) ENABLE
 *
 * @vdev: 대상
 * @vq  : 셋업할 virtqueue
 * @return: 0 성공, 음수 errno
 *
 * 동작:
 *   1) 중복 셋업 방지 (callfd/kickfd가 -1이어야 함)
 *   2) callfd/kickfd eventfd 생성 (CLOEXEC | NONBLOCK)
 *   3) vring 메모리 hugepage 할당 (모든 hugepage가 SET_MEM_TABLE로 백엔드에 등록되므로 백엔드가 같은 VA 접근 가능)
 *   4) is_hw=0이라 vq_ring_mem = SPDK_VTOPHYS_ERROR (사용 안 함 표시), virt_mem만 의미.
 *   5) F_PROTOCOL_FEATURES 협상 시 SET_VRING_ENABLE
 *   6) dev->vrings[]에 num/desc/avail/used 캐시
 *
 * 호출 체인:
 *   virtio_init_queue → backend_ops->setup_queue(=[virtio_user_setup_queue])
 */
static int
virtio_user_setup_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_state state;
	uint16_t queue_idx = vq->vq_queue_index;
	void *queue_mem;
	uint64_t desc_addr, avail_addr, used_addr;
	int callfd, kickfd, rc;

	if (dev->callfds[queue_idx] != -1 || dev->kickfds[queue_idx] != -1) {
		SPDK_ERRLOG("queue %"PRIu16" already exists\n", queue_idx);
		return -EEXIST;  /* [한국어] 이미 셋업된 큐. */
	}

	/* May use invalid flag, but some backend uses kickfd and
	 * callfd as criteria to judge if dev is alive. so finally we
	 * use real event_fd.
	 */
	/* [한국어] callfd: 백엔드가 used 후 wake — 폴링 모드 SPDK는 사용 안 하지만 백엔드 호환성을 위해 진짜 fd 만듦. */
	callfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (callfd < 0) {
		SPDK_ERRLOG("callfd error, %s\n", spdk_strerror(errno));
		return -errno;
	}

	/* [한국어] kickfd: 이니시에이터가 avail 후 wake용 — virtio_user_notify_queue가 write. */
	kickfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (kickfd < 0) {
		SPDK_ERRLOG("kickfd error, %s\n", spdk_strerror(errno));
		close(callfd);
		return -errno;
	}

	queue_mem = spdk_zmalloc(vq->vq_ring_size, VIRTIO_PCI_VRING_ALIGN, NULL,
				 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	/* [한국어] hugepage DMA 메모리 — SET_MEM_TABLE을 통해 백엔드도 같은 VA로 접근 가능. */
	if (queue_mem == NULL) {
		close(kickfd);
		close(callfd);
		return -ENOMEM;
	}

	vq->vq_ring_mem = SPDK_VTOPHYS_ERROR; /* [한국어] is_hw=0이라 IOVA 미사용 표시. */
	vq->vq_ring_virt_mem = queue_mem;

	state.index = vq->vq_queue_index;
	state.num = 1;  /* [한국어] enable=1. */

	if (virtio_dev_has_feature(vdev, VHOST_USER_F_PROTOCOL_FEATURES)) {
		/* [한국어] F_PROTOCOL_FEATURES 협상 시: SET_VRING_ENABLE 메시지 송신.
		 * 멀티큐 지원 백엔드는 이 메시지로 큐별 활성/비활성 제어. */
		rc = vhost_user_sock(dev, VHOST_USER_SET_VRING_ENABLE, &state);
		if (rc < 0) {
			SPDK_ERRLOG("failed to send VHOST_USER_SET_VRING_ENABLE: %s\n",
				    spdk_strerror(-rc));
			close(kickfd);
			close(callfd);
			spdk_free(queue_mem);
			return -rc;
		}
	}

	dev->callfds[queue_idx] = callfd;
	dev->kickfds[queue_idx] = kickfd;

	desc_addr = (uintptr_t)vq->vq_ring_virt_mem;
	avail_addr = desc_addr + vq->vq_nentries * sizeof(struct vring_desc);
	used_addr = SPDK_ALIGN_CEIL(avail_addr + offsetof(struct vring_avail,
				    ring[vq->vq_nentries]),
				    VIRTIO_PCI_VRING_ALIGN);
	/* [한국어] desc → avail → (정렬) → used. virtio.c의 vring_init과 동일 레이아웃. */

	dev->vrings[queue_idx].num = vq->vq_nentries;
	dev->vrings[queue_idx].desc = (void *)(uintptr_t)desc_addr;
	dev->vrings[queue_idx].avail = (void *)(uintptr_t)avail_addr;
	dev->vrings[queue_idx].used = (void *)(uintptr_t)used_addr;
	/* [한국어] 캐싱 — kick_queue/set_vring_addr이 SET_VRING_ADDR 메시지에 사용. */

	return 0;
}

/*
 * [한국어]
 * virtio_user_del_queue - 큐 정리 (eventfd close + vring 메모리 해제)
 *
 * @vdev: 대상
 * @vq  : 해제할 큐
 *
 * SET_VRING_ENABLE=0 등을 보내지 않고 단순 fd close + 메모리 해제. 백엔드는 fd close를
 * EOF로 인식해 큐 처리 중단 — 또는 디바이스 RESET 시 일괄 정리.
 *
 * 호출 체인:
 *   virtio_free_queues → backend_ops->del_queue(=[virtio_user_del_queue])
 */
static void
virtio_user_del_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	/* For legacy devices, write 0 to VIRTIO_PCI_QUEUE_PFN port, QEMU
	 * correspondingly stops the ioeventfds, and reset the status of
	 * the device.
	 * For modern devices, set queue desc, avail, used in PCI bar to 0,
	 * not see any more behavior in QEMU.
	 *
	 * Here we just care about what information to deliver to vhost-user.
	 * So we just close ioeventfd for now.
	 */
	/* [한국어] vhost-user에는 큐 비활성 메시지를 명시적으로 보내지 않고, eventfd close로 종료 신호.
	 * 디바이스 RESET 시 stop_device가 GET_VRING_BASE를 보내 정지를 보장. */
	struct virtio_user_dev *dev = vdev->ctx;

	close(dev->callfds[vq->vq_queue_index]);
	close(dev->kickfds[vq->vq_queue_index]);
	dev->callfds[vq->vq_queue_index] = -1;
	dev->kickfds[vq->vq_queue_index] = -1;

	spdk_free(vq->vq_ring_virt_mem);    /* [한국어] vring 메모리 해제. */
}

/*
 * [한국어]
 * virtio_user_notify_queue - kickfd write로 백엔드 wake (doorbell 대용)
 *
 * @vdev: 대상
 * @vq  : 통지할 큐
 *
 * eventfd는 8바이트 write가 카운터를 증가시키고 reader를 wake. 백엔드가 epoll 또는 read로
 * 대기 중이면 즉시 깨어남. 백엔드가 폴링 중이면 eventfd write는 사실상 무시(가벼운 비용만).
 *
 * 실행 컨텍스트: 큐 owner_thread (req_flush 경로).
 *
 * 호출 체인:
 *   virtqueue_req_flush → backend_ops->notify_queue(=[virtio_user_notify_queue])
 */
static void
virtio_user_notify_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	uint64_t buf = 1;   /* [한국어] eventfd 카운터 증가량. */
	struct virtio_user_dev *dev = vdev->ctx;

	if (write(dev->kickfds[vq->vq_queue_index], &buf, sizeof(buf)) < 0) {
		SPDK_ERRLOG("failed to kick backend: %s.\n", spdk_strerror(errno)); /* [한국어] 소켓 다운 등 — 백엔드 사망 가능. */
	}
}

/*
 * [한국어]
 * virtio_user_destroy - vhostfd close + 사설 컨텍스트 free
 *
 * @vdev: 대상
 *
 * 호출 체인:
 *   virtio_dev_destruct → backend_ops->destruct_dev(=[virtio_user_destroy])
 */
static void
virtio_user_destroy(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;

	if (dev) {
		close(dev->vhostfd);  /* [한국어] 소켓 close — 백엔드는 EOF로 connection 종료 인지. */
		free(dev);
	}
}

/*
 * [한국어]
 * virtio_user_dump_json_info - RPC 응답에 user 트랜스포트 정보 추가
 *
 * @vdev: 대상
 * @w   : JSON writer
 *
 * "type":"user", "socket":"<path>" — 사용자/관리 도구가 식별.
 */
static void
virtio_user_dump_json_info(struct virtio_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct virtio_user_dev *dev = vdev->ctx;

	spdk_json_write_named_string(w, "type", "user");
	spdk_json_write_named_string(w, "socket", dev->path);
}

/*
 * [한국어]
 * virtio_user_write_json_config - config dump (save_config RPC)
 *
 * @vdev: 대상
 * @w   : JSON writer
 *
 * "trtype":"user", "traddr":"<path>", "vq_count":<N>, "vq_size":<크기> — 동일한 디바이스를
 * 재구성할 수 있는 정보.
 */
static void
virtio_user_write_json_config(struct virtio_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct virtio_user_dev *dev = vdev->ctx;

	spdk_json_write_named_string(w, "trtype", "user");
	spdk_json_write_named_string(w, "traddr", dev->path);
	spdk_json_write_named_uint32(w, "vq_count", vdev->max_queues - vdev->fixed_queues_num); /* [한국어] 데이터 큐 수 (control 큐 제외). */
	spdk_json_write_named_uint32(w, "vq_size", virtio_dev_backend_ops(vdev)->get_queue_size(vdev, 0));
}

/*
 * [한국어]
 * virtio_user_ops - vhost-user 트랜스포트 콜백 테이블
 */
static const struct virtio_dev_ops virtio_user_ops = {
	.read_dev_cfg	= virtio_user_read_dev_config,    /* [한국어] GET_CONFIG 메시지. */
	.write_dev_cfg	= virtio_user_write_dev_config,    /* [한국어] SET_CONFIG 메시지. */
	.get_status	= virtio_user_get_status,          /* [한국어] dev->status 캐시 read. */
	.set_status	= virtio_user_set_status,          /* [한국어] DRIVER_OK/RESET 자동 매핑. */
	.get_features	= virtio_user_get_features,        /* [한국어] GET_FEATURES 메시지. */
	.set_features	= virtio_user_set_features,        /* [한국어] SET_FEATURES + protocol features. */
	.destruct_dev	= virtio_user_destroy,             /* [한국어] 소켓 close + free. */
	.get_queue_size	= virtio_user_get_queue_size,      /* [한국어] dev->queue_size 반환. */
	.setup_queue	= virtio_user_setup_queue,         /* [한국어] eventfd + vring 메모리 + ENABLE. */
	.del_queue	= virtio_user_del_queue,           /* [한국어] eventfd close + 메모리 해제. */
	.notify_queue	= virtio_user_notify_queue,        /* [한국어] kickfd write. */
	.dump_json_info = virtio_user_dump_json_info,      /* [한국어] RPC 응답 — type/socket. */
	.write_json_config = virtio_user_write_json_config, /* [한국어] config save — trtype/traddr/vq_count/vq_size. */
};

/*
 * [한국어]
 * virtio_user_dev_init - vhost-user 디바이스 진입점 (소켓 connect + SET_OWNER)
 *
 * @vdev      : 호출자가 할당한 virtio_dev
 * @name      : 디바이스 이름
 * @path      : vhost-user 소켓 경로
 * @queue_size: 큐 크기 (모든 큐 동일)
 * @return: 0 성공, 음수 errno
 *
 * 동작:
 *   1) calloc으로 사설 컨텍스트 할당
 *   2) virtio_dev_construct로 vdev 기본 초기화 (ops 바인딩)
 *   3) is_hw=0 (vhost-user는 VA를 그대로 사용 — vtophys 안 함)
 *   4) path 보존, queue_size 캐싱
 *   5) virtio_user_dev_setup으로 소켓 connect
 *   6) SET_OWNER 메시지로 디바이스 소유 선언 (vhost-user 프로토콜 첫 단계)
 * 실패 시 virtio_dev_destruct로 일괄 롤백 (destruct가 ops->destruct_dev = virtio_user_destroy 호출 → free).
 *
 * 실행 컨텍스트: 디바이스 attach 경로 — 단일 thread.
 *
 * 호출 체인:
 *   bdev_virtio_user_create / RPC → [virtio_user_dev_init]
 */
int
virtio_user_dev_init(struct virtio_dev *vdev, const char *name, const char *path,
		     uint32_t queue_size)
{
	struct virtio_user_dev *dev;
	int rc;

	if (name == NULL) {
		SPDK_ERRLOG("No name given for controller: %s\n", path);
		return -EINVAL;
	}

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		return -ENOMEM;
	}

	rc = virtio_dev_construct(vdev, name, &virtio_user_ops, dev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to init device: %s\n", path);
		free(dev);
		return rc;
	}

	vdev->is_hw = 0;       /* [한국어] is_hw=0 → virtqueue_req_add_iovs가 VA를 그대로 desc.addr에 사용. */

	snprintf(dev->path, PATH_MAX, "%s", path);
	dev->queue_size = queue_size;

	rc = virtio_user_dev_setup(vdev);
	if (rc < 0) {
		SPDK_ERRLOG("backend set up fails\n");
		goto err;
	}

	rc = vhost_user_sock(dev, VHOST_USER_SET_OWNER, NULL);
	/* [한국어] SET_OWNER: vhost-user 프로토콜 첫 메시지 — 단일 driver만 디바이스 소유 가능을 선언. */
	if (rc < 0) {
		SPDK_ERRLOG("set_owner fails: %s\n", spdk_strerror(-rc));
		goto err;
	}

	return 0;

err:
	virtio_dev_destruct(vdev); /* [한국어] 부분 성공 롤백 — destruct가 fd close + free 처리. */
	return rc;
}
/* [한국어] SPDK 로그 컴포넌트 등록 — `--logflag virtio_user`로 활성화. */
SPDK_LOG_REGISTER_COMPONENT(virtio_user)
