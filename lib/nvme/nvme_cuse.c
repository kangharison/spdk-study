/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK NVMe 컨트롤러를 Linux char device(/dev/nvmeX[nY])로 노출하는
 *               CUSE(Character device in USErspace) 통합 (nvme_cuse.c)
 *
 * === 파일의 역할 ===
 * SPDK는 본래 커널을 우회한 유저스페이스 NVMe 드라이버이므로, 일반 사용자 도구(`nvme-cli`,
 * `nvme list`, `smartctl`, `nvme id-ctrl` 등)는 SPDK가 점유한 디바이스를 보지 못한다.
 * 이 파일은 그 간극을 메우기 위해 **libfuse3의 CUSE 저레벨 API**를 사용해 유저스페이스에서
 * `/dev/spdk/nvmeX` (controller char device) 와 `/dev/spdk/nvmeXnY` (namespace char device)
 * 가짜 char device를 만들어 띄운다. 그 디바이스로 들어온 NVMe ioctl(NVME_IOCTL_ADMIN_CMD,
 * NVME_IOCTL_IO_CMD, NVME_IOCTL_SUBMIT_IO, NVME_IOCTL_RESET, NVME_IOCTL_RESCAN, BLK*GET 등)을
 * SPDK NVMe 드라이버 admin/IO 명령으로 변환·전달하여, 외부 도구가 SPDK 관리 디바이스에
 * 접근할 수 있게 한다.
 *
 * 책임 4가지:
 *   1) **CUSE 세션 생성**: 컨트롤러/네임스페이스 단위로 `cuse_lowlevel_setup()`을 호출해
 *      char device를 등록하고 ioctl 콜백 vtable(`cuse_ctrlr_clop`/`cuse_ns_clop`)을 부착.
 *   2) **단일 CUSE thread**: 별도 pthread(`cuse_thread`)가 모든 CUSE 세션의 fd를
 *      `spdk_fd_group`로 멀티플렉싱해 polling — 즉 device 1개당 thread 1개가 아니라
 *      **전 device 1 thread**가 fuse 이벤트를 받아 처리한다.
 *   3) **ioctl → NVMe 명령 변환**: NVMe spec ioctl 페이로드(`struct nvme_passthru_cmd`,
 *      `struct nvme_user_io`)를 `struct spdk_nvme_cmd` SQE로 매핑하여 `nvme_io_msg_send`로
 *      controller 소유 reactor에 위임. 응답이 오면 비동기 콜백에서 `fuse_reply_ioctl_iov`로
 *      회신.
 *   4) **다중 인스턴스 충돌 방지**: `/var/tmp/spdk_nvme_cuse_lock_<idx>` 파일에 fcntl
 *      F_SETLK 락을 잡아 동일 SPDK 인스턴스/외부 프로세스가 같은 인덱스를 점유하지 못하게 함.
 *
 * 빌드 가드: `SPDK_CONFIG_HAVE_FUSE3` 매크로가 활성화될 때만 컴파일되며, 외부 의존성으로
 * libfuse3 (`fuse3/cuse_lowlevel.h`)와 Linux nvme ioctl 헤더(`linux/nvme_ioctl.h`,
 * `linux/fs.h`)를 사용한다.
 *
 * **본 파일은 코드 수정 없이 한국어 주석만 추가/보강된 학습 사본**이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 본 파일은 SPDK NVMe 드라이버의 **선택적(optional) 관리 포털**이며, I/O 데이터 경로의
 * critical path가 아니다. 즉 bdev → nvme → PCIe로 흐르는 메인 I/O 흐름과는 분리되어
 * "사용자가 SPDK 디바이스에 nvme-cli를 쓰고 싶을 때만" 활성화된다 (`spdk_nvme_cuse_register`).
 *
 * 양방향 데이터 흐름:
 *
 *   [사용자 도구: nvme-cli]
 *     → open("/dev/spdk/nvme0") + ioctl(NVME_IOCTL_ADMIN_CMD, ...)
 *         (커널 VFS → CUSE char-major → libfuse 커널 모듈 → fuse fd로 패킷 전달)
 *   [CUSE thread (pthread, non-reactor)]
 *     → spdk_fd_group_wait(epoll on fuse_efd)
 *     → fuse_session_receive_buf + fuse_session_process_buf
 *     → cuse_ns_ioctl / cuse_ctrlr_ioctl 디스패처
 *     → cuse_nvme_passthru_cmd_send (메모리/iov 준비)
 *     → nvme_io_msg_send(ctrlr, nsid, cuse_nvme_passthru_cmd_execute, ctx)  [ring enqueue]
 *   [SPDK reactor (controller 소유)]
 *     → 폴링 중 nvme_io_msg_process가 ring dequeue
 *     → cuse_nvme_passthru_cmd_execute(ctrlr, nsid, ctx) 실행
 *     → spdk_nvme_ctrlr_cmd_admin_raw / spdk_nvme_ctrlr_cmd_io_raw_with_md
 *         (external_io_msgs_qpair로 SQE 발사)
 *     → 디바이스 → CQE → cuse_nvme_passthru_cmd_cb 콜백
 *         → fuse_reply_ioctl_iov(ctx->req, status, out_iov, ...)
 *           (libfuse가 사용자 ioctl()에 결과 회신)
 *
 * 실행 컨텍스트 분리:
 *   - **CUSE thread**: `cuse_thread()` 함수가 도는 일반 pthread. SPDK reactor가 아니므로
 *     spdk_nvme_* 자료구조를 직접 만지면 안 됨 → 모든 NVMe 작업은 nvme_io_msg_send를 통해
 *     reactor로 위임.
 *   - **SPDK reactor**: 실제 NVMe 명령 발사와 완료 콜백이 실행되는 곳. 콜백 내에서
 *     `fuse_reply_*`를 호출해도 libfuse는 thread-safe하므로 안전(reply는 fd write).
 *   - **management thread (사용자 init/fini)**: `spdk_nvme_cuse_register/unregister`가 도는
 *     컨텍스트로, 보통 SPDK 앱의 메인 스레드. `g_cuse_mtx`로 등록 리스트 보호.
 *
 * === 타 모듈과의 연결 ===
 *  - **nvme_io_msg.c/h** — 본 파일의 가장 중요한 의존. CUSE thread(외부 스레드)에서
 *    controller 소유 reactor로 작업을 넘기는 message-passing 채널.
 *    `nvme_io_msg_ctrlr_register`로 cuse를 producer로 등록하고, 모든 ioctl은
 *    `nvme_io_msg_send`로 reactor에 위임. unregister/stop/update 콜백 vtable은
 *    `cuse_nvme_io_msg_producer`로 export.
 *  - **nvme_internal.h / nvme_ctrlr_cmd.c** — `spdk_nvme_ctrlr_cmd_admin_raw`,
 *    `spdk_nvme_ctrlr_cmd_io_raw_with_md`, `spdk_nvme_ns_cmd_read/write_with_md` 등을 호출.
 *    `ctrlr->external_io_msgs_qpair`(io_msg.c가 만든 전용 큐페어) 위에서 실행.
 *  - **libfuse3 (`<fuse3/cuse_lowlevel.h>`)** — `cuse_lowlevel_setup`, `fuse_session_*`,
 *    `fuse_reply_*`, `cuse_lowlevel_teardown` 등. CUSE는 FUSE의 char device 변형으로
 *    /dev/cuse 디바이스를 통해 커널과 통신.
 *  - **`<linux/nvme_ioctl.h>`** — `NVME_IOCTL_ADMIN_CMD`(0xC0484E41), `NVME_IOCTL_IO_CMD`,
 *    `NVME_IOCTL_RESET`, `NVME_IOCTL_RESCAN`, `NVME_IOCTL_ID`, `NVME_IOCTL_SUBMIT_IO`,
 *    `NVME_IOCTL_SUBSYS_RESET`, `struct nvme_passthru_cmd`, `struct nvme_user_io` 정의.
 *  - **`<linux/fs.h>`** — `BLKGETSIZE`, `BLKGETSIZE64`, `BLKSSZGET`, `BLKPBSZGET` 블록
 *    디바이스 ioctl 정의.
 *  - **spdk/fd_group.h** — epoll 추상화. CUSE thread가 다수 fuse fd + 통지용 eventfd를
 *    한 번에 polling.
 *  - **spdk/bit_array.h** — 컨트롤러 인덱스(0~127) 할당/회수. 동일 인덱스 중복 방지.
 *  - **spdk/env.h** — `spdk_malloc`/`spdk_zmalloc`(DPDK hugepage DMA 메모리),
 *    `spdk_unaffinitize_thread`(CUSE thread에서 코어 핀닝 해제 — 어떤 코어든 OK).
 *  - **사용자**: 이 파일의 export 함수(`spdk_nvme_cuse_register/unregister/...`)는
 *    `module/bdev/nvme/bdev_nvme_cuse_rpc.c` 등 RPC 핸들러나 NVMe controller attach
 *    플로우의 선택 단계에서 호출됨.
 *
 * === 주요 함수/구조체 요약 ===
 *  핵심 자료구조:
 *    - struct cuse_device — controller 1개 또는 namespace 1개에 대응. ctrlr/ns 공통 구조체.
 *    - struct cuse_io_ctx — 1건의 ioctl 처리 중 저장해야 할 in-flight 컨텍스트
 *      (NVMe SQE, payload 버퍼, fuse_req).
 *    - struct cuse_transport — SPDK_CUSE_GET_TRANSPORT 응답용 트랜스포트 정보(trstring/traddr).
 *
 *  전역 상태:
 *    - g_cuse_mtx — controller 등록 리스트 + bit_array 보호.
 *    - g_ctrlr_ctx_head — 등록된 모든 controller cuse_device의 TAILQ.
 *    - g_ctrlr_started — 컨트롤러 인덱스 비트 배열(0~127).
 *    - g_pending_device_mtx — 새로 만들어진 cuse_device를 CUSE thread fd_group에 추가하기
 *      전 임시 보관 큐 보호.
 *    - g_pending_device_head / g_active_device_head — CUSE thread에 위임 대기 중/위임 완료.
 *    - g_device_fdgrp — CUSE thread가 polling하는 epoll group.
 *    - g_cuse_thread_msg_fd — eventfd로 "새 device 추가됨" 통지를 CUSE thread에게 보냄.
 *
 *  주요 함수 (10여 개):
 *    - spdk_nvme_cuse_register / spdk_nvme_cuse_unregister — 외부 진입점. 컨트롤러 1개를
 *      CUSE에 등록/해제.
 *    - nvme_cuse_start / cuse_nvme_ctrlr_stop — 컨트롤러 cuse_device 생성 + 모든 namespace
 *      cuse_device 생성 / 일괄 정리.
 *    - cuse_session_create — 1개 cuse_device에 대해 libfuse 세션 생성, fd를 pending 큐에
 *      넣고 eventfd 통지.
 *    - cuse_thread — pthread 메인 루프. fd_group_wait → fuse_session_receive_buf →
 *      fuse_session_process_buf 반복.
 *    - cuse_ctrlr_ioctl / cuse_ns_ioctl — fuse가 호출하는 ioctl 디스패처. cmd 번호로 분기.
 *    - cuse_nvme_passthru_cmd / _send / _execute / _cb — NVME_IOCTL_ADMIN_CMD/IO_CMD 처리
 *      (in/out iov 준비 → ctx 할당 → io_msg 위임 → CQE 콜백 → fuse_reply).
 *    - cuse_nvme_submit_io / _read / _write — NVME_IOCTL_SUBMIT_IO 처리 (read/write 분기).
 *    - cuse_nvme_reset / cuse_nvme_subsys_reset — controller/subsystem 리셋.
 *    - nvme_cuse_claim / unclaim — 인덱스 락 파일 점유/해제.
 *
 *  ioctl 매핑 요약:
 *    NVME_IOCTL_ADMIN_CMD     → spdk_nvme_ctrlr_cmd_admin_raw (admin SQE)
 *    NVME_IOCTL_IO_CMD        → spdk_nvme_ctrlr_cmd_io_raw_with_md (IO SQE, namespace 한정)
 *    NVME_IOCTL_SUBMIT_IO     → spdk_nvme_ns_cmd_read/write_with_md (read/write opc)
 *    NVME_IOCTL_RESET         → spdk_nvme_ctrlr_reset
 *    NVME_IOCTL_SUBSYS_RESET  → spdk_nvme_ctrlr_reset_subsystem
 *    NVME_IOCTL_RESCAN        → 모든 active ns에 nvme_ns_construct (Identify Namespace 갱신)
 *    NVME_IOCTL_ID            → ctrlr면 ENOTTY, ns면 nsid를 result로 반환
 *    BLKGETSIZE/BLKGETSIZE64  → namespace 용량 (512B 단위 / sector 단위)
 *    BLKSSZGET/BLKPBSZGET     → sector size
 *    SPDK_CUSE_GET_TRANSPORT  → SPDK 고유 ioctl. trid의 trstring/traddr 회신.
 */

#include "spdk/stdinc.h"        /* [한국어] SPDK 표준 헤더 — POSIX/표준 C 헤더를 한 번에 포함. */
#include "spdk/string.h"        /* [한국어] spdk_strerror 등 문자열 유틸. errno→메시지 변환에 사용. */
#include "spdk/config.h"        /* [한국어] 빌드 시 ./configure가 생성한 매크로(SPDK_CONFIG_HAVE_FUSE3 등). */
#include "spdk/fd_group.h"      /* [한국어] epoll 추상화. CUSE thread가 fuse fd 다수를 동시 polling. */
#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG/NOTICELOG/DEBUGLOG 매크로. */
#include "spdk/nvme.h"          /* [한국어] SPDK NVMe 공개 API: spdk_nvme_ctrlr*, spdk_nvme_ns_cmd_*, opc 상수. */

#define FUSE_USE_VERSION 31
/* [한국어] libfuse3 ABI 버전 명시 — fuse3 API 31(파일·핸들 기반 ioctl, fuse_log 등 포함).
 *         반드시 fuse3 헤더 include 전에 정의해야 헤더가 적절한 prototype을 노출함. */

#include <fuse3/cuse_lowlevel.h>
/* [한국어] CUSE 저레벨 API — cuse_lowlevel_setup/teardown, struct cuse_lowlevel_ops,
 *         fuse_req_t, fuse_reply_*. CUSE는 FUSE의 char device 변형으로
 *         /dev/cuse 디바이스를 통해 커널과 통신해 임의의 char device를 유저스페이스에서 시뮬레이션. */

#include <linux/nvme_ioctl.h>
/* [한국어] Linux 커널의 NVMe ioctl 정의. nvme-cli 등 표준 도구가 사용하는 ABI:
 *         NVME_IOCTL_ADMIN_CMD/IO_CMD/SUBMIT_IO/RESET/RESCAN/ID/SUBSYS_RESET,
 *         struct nvme_passthru_cmd, struct nvme_user_io. */
#include <linux/fs.h>
/* [한국어] 일반 블록 디바이스 ioctl: BLKGETSIZE(섹터 수, 512B 단위, long),
 *         BLKGETSIZE64(바이트 단위 또는 섹터 단위 uint64_t),
 *         BLKSSZGET(논리 sector size, int), BLKPBSZGET(physical block size, int). */

#include "nvme_internal.h"
/* [한국어] SPDK NVMe 드라이버 내부 헤더. struct spdk_nvme_ctrlr 본체, external_io_msgs_qpair
 *         같은 비공개 필드 접근, nvme_ns_construct 등 내부 함수 프로토타입 사용. */
#include "nvme_io_msg.h"
/* [한국어] external producer message channel API — nvme_io_msg_send, nvme_io_msg_producer,
 *         nvme_io_msg_ctrlr_register/unregister. CUSE는 producer로 등록되어 외부 thread에서
 *         reactor로 작업을 위임한다. */
#include "nvme_cuse.h"
/* [한국어] 본 파일이 export하는 spdk_nvme_cuse_* 프로토타입. (외부에서 cuse 등록 호출 시 사용) */

/*
 * [한국어] struct cuse_device — controller 또는 namespace 1개에 대응하는 cuse 단위.
 *         같은 구조체로 컨트롤러("/dev/spdk/nvmeX", nsid==0)와 네임스페이스("/dev/spdk/nvmeXnY", nsid>0)를
 *         모두 표현하며, nsid 필드로 구분한다. 컨트롤러 device는 그 산하 namespace device들을
 *         ns_devices 리스트로 들고 있다.
 */
struct cuse_device {
	bool				force_exit;
	/* [한국어] 컨트롤러/네임스페이스 종료 시 cuse_thread가 fuse session teardown 후 free까지
	 *         하도록 알리는 플래그. 일반적인 fuse session 종료(외부 SIGTERM 등)와
	 *         SPDK 내부 unregister 흐름을 구분하기 위해 존재.
	 *         설정자: cuse_nvme_ns_stop / cuse_nvme_ctrlr_stop.
	 *         읽는 자: cuse_thread (메인 루프).
	 *         값 범위: false(기본) | true(SPDK가 명시적으로 종료 요청).
	 *         동기화: g_cuse_mtx 보호 하에 set, cuse_thread 단독으로 read. */

	char				dev_name[128];
	/* [한국어] CUSE 디바이스 이름. 컨트롤러는 "spdk/nvme%d" (예: "spdk/nvme0"),
	 *         네임스페이스는 "spdk/nvme%dn%d" (예: "spdk/nvme0n1") 형태.
	 *         libfuse가 DEVNAME=...로 받아 /dev/<dev_name>로 char device 생성.
	 *         설정자: nvme_cuse_start (ctrlr) / cuse_nvme_ns_start (ns).
	 *         읽는 자: cuse_session_create, spdk_nvme_cuse_get_*_name. */

	uint32_t			index;
	/* [한국어] 컨트롤러 인덱스 (0~127). g_ctrlr_started bit_array에서 할당된 슬롯 번호.
	 *         dev_name과 lock_name에 사용. namespace device의 경우 부모 controller의 index가
	 *         dev_name 형성에 사용되며 자기 자신의 index는 의미 없음. */

	int				claim_fd;
	/* [한국어] /var/tmp/spdk_nvme_cuse_lock_<index>를 fcntl(F_SETLK, F_WRLCK)로 잡고 있는 fd.
	 *         이 fd가 살아있는 동안 락이 유지됨 → close하면 자동 해제.
	 *         같은 SPDK 또는 외부 프로세스가 같은 인덱스를 두 번 점유하지 못하게 함.
	 *         설정자: nvme_cuse_claim. 읽는 자: nvme_cuse_unclaim. */

	char				lock_name[64];
	/* [한국어] claim_fd가 가리키는 락 파일 경로. unclaim 시 unlink로 제거. */

	struct spdk_nvme_ctrlr		*ctrlr;		/**< NVMe controller */
	/* [한국어] 이 cuse device가 표현하는 SPDK NVMe controller. ns_device의 경우 부모 controller.
	 *         설정자: nvme_cuse_start, cuse_nvme_ns_start.
	 *         읽는 자: 모든 ioctl 핸들러 (cuse_nvme_passthru_cmd_send 등).
	 *         동기화: cuse_device가 살아있는 동안은 항상 유효 (unregister가 stop 후 unregister 보장). */

	uint32_t			nsid;		/**< NVMe name space id, or 0 */
	/* [한국어] 네임스페이스 ID. 0이면 "이 cuse device는 controller 자체"를 의미.
	 *         1 이상이면 namespace device. ioctl 핸들러가 ctrlr/ns 분기를 결정할 때 검사.
	 *         설정자: 생성 시점에 결정되며 이후 불변. */

	struct fuse_session		*session;
	/* [한국어] libfuse가 만들어 준 fuse_session 핸들. cuse_lowlevel_setup이 반환한 불투명 객체.
	 *         fuse_session_fd로 underlying fd를 얻어 fd_group polling.
	 *         fuse_session_process_buf로 받은 ioctl 요청을 처리.
	 *         fuse_session_exited / fuse_session_reset / cuse_lowlevel_teardown으로 수명 관리.
	 *         설정자: cuse_session_create.
	 *         소유 스레드: 생성은 어떤 스레드든 가능하나 polling/process는 cuse_thread 단독. */

	int				fuse_efd;
	/* [한국어] fuse_session_fd(session)이 반환한 underlying fd. epoll에 등록할 readable fd로 사용.
	 *         커널 CUSE 모듈이 사용자 ioctl을 받으면 이 fd가 readable이 됨 → cuse_thread 깨움. */

	struct cuse_device		*ctrlr_device;
	/* [한국어] namespace device의 경우 부모 controller cuse_device 포인터. ctrlr device의 경우 의미 없음.
	 *         현재 코드에서는 직접 참조하지 않지만, 디버깅/추적용으로 보존됨. */

	TAILQ_HEAD(, cuse_device)	ns_devices;
	/* [한국어] controller cuse_device가 자기 산하 namespace cuse_device들을 묶어두는 리스트 헤드.
	 *         cuse_nvme_ns_start로 추가, cuse_nvme_ns_stop로 제거.
	 *         namespace cuse_device는 이 리스트의 tailq 필드로 연결됨. */

	TAILQ_ENTRY(cuse_device)	tailq;
	/* [한국어] 두 가지 용도로 사용되는 링크:
	 *         - controller device: g_ctrlr_ctx_head 전역 리스트의 엔트리.
	 *         - namespace device: 부모 controller의 ns_devices 리스트의 엔트리. */

	TAILQ_ENTRY(cuse_device)	cuse_thread_tailq;
	/* [한국어] CUSE thread에 위임 대기 중인 g_pending_device_head, 또는 위임 완료된
	 *         g_active_device_head 리스트의 엔트리. tailq와 별도의 링크 — 동시에 양쪽에 들어갈 수 있게. */
};

/*
 * [한국어] g_cuse_mtx — controller 등록/해제 경로(g_ctrlr_ctx_head, g_ctrlr_started)를 보호.
 *         spdk_nvme_cuse_register/unregister, nvme_cuse_start/stop, cuse_thread 종료 시 사용.
 *         초기화: PTHREAD_MUTEX_INITIALIZER (정적 초기화).
 *         쓰레드: 등록은 사용자 thread, 종료는 cuse_thread 또는 사용자 thread 양쪽. */
static pthread_mutex_t g_cuse_mtx = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] g_ctrlr_ctx_head — 등록된 모든 controller cuse_device의 전역 TAILQ.
 *         lookup(nvme_cuse_get_cuse_ctrlr_device)으로 ctrlr→cuse_device 역참조에 사용.
 *         보호: g_cuse_mtx. */
static TAILQ_HEAD(, cuse_device) g_ctrlr_ctx_head = TAILQ_HEAD_INITIALIZER(g_ctrlr_ctx_head);
/* [한국어] g_ctrlr_started — 컨트롤러 인덱스 0~127의 사용 중 여부를 표현하는 비트 배열.
 *         첫 controller 등록 시 동적 생성, 마지막 controller 해제 시 free.
 *         spdk_bit_array_find_first_clear로 빈 슬롯 찾고 set, stop 시 clear. */
static struct spdk_bit_array *g_ctrlr_started;

/* [한국어] g_pending_device_mtx — pending/active device 큐 보호.
 *         새로 만든 cuse_device를 g_pending_device_head에 넣고 eventfd로 cuse_thread를 깨움.
 *         cuse_thread는 락을 잡고 pending → active로 옮기고 fd_group에 fuse_efd 등록. */
static pthread_mutex_t g_pending_device_mtx = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] g_device_fdgrp — CUSE thread가 polling하는 epoll group.
 *         g_cuse_thread_msg_fd(통지용 eventfd) + 각 cuse_device의 fuse_efd가 등록됨.
 *         start_cuse_thread에서 생성, cuse_thread 종료 시 destroy. */
static struct spdk_fd_group *g_device_fdgrp;
/* [한국어] g_cuse_thread_msg_fd — "새 device가 pending에 추가되었으니 fd_group에 넣어달라"는
 *         통지용 eventfd (EFD_NONBLOCK | EFD_CLOEXEC).
 *         cuse_session_create가 eventfd_write(1)으로 깨우고, cuse_thread_add_session 핸들러가
 *         eventfd_read 후 pending 큐를 active로 이동. */
static int g_cuse_thread_msg_fd;
/* [한국어] g_pending_device_head — 새로 만들어졌지만 아직 cuse_thread fd_group에 등록되지 않은
 *         cuse_device들. cuse_session_create가 INSERT, cuse_thread_add_session이 DRAIN. */
static TAILQ_HEAD(, cuse_device) g_pending_device_head = TAILQ_HEAD_INITIALIZER(
			g_pending_device_head);
/* [한국어] g_active_device_head — fd_group에 등록되어 cuse_thread가 polling 중인 cuse_device들.
 *         cuse_thread가 fuse_session_exited(종료된 세션)을 발견하면 여기서 제거하고 free. */
static TAILQ_HEAD(, cuse_device) g_active_device_head = TAILQ_HEAD_INITIALIZER(
			g_active_device_head);

/*
 * [한국어] struct cuse_io_ctx — 1건의 ioctl 처리 진행 상태를 담는 in-flight 컨텍스트.
 *         CUSE thread가 ioctl 페이로드 파싱 후 할당하여 nvme_io_msg_send로 reactor에 넘기고,
 *         reactor가 NVMe 명령 발사 후 CQE 콜백에서 fuse_reply 한 다음 free한다.
 *         즉 CUSE thread → reactor → CUSE 사용자 응답까지의 lifetime을 가진다.
 */
struct cuse_io_ctx {
	struct spdk_nvme_cmd		nvme_cmd;
	/* [한국어] 디바이스에 보낼 NVMe SQE 64바이트. opc, nsid, cdw10~cdw15가 nvme_passthru_cmd 페이로드에서
	 *         그대로 복사됨. cuse_nvme_passthru_cmd_execute에서 SQE로 사용. */

	enum spdk_nvme_data_transfer	data_transfer;
	/* [한국어] 데이터 방향: NONE / HOST_TO_CONTROLLER / CONTROLLER_TO_HOST / BIDIRECTIONAL.
	 *         opcode의 [1:0] 비트(NVMe 스펙)에서 spdk_nvme_opc_get_data_transfer로 추출.
	 *         CONTROLLER_TO_HOST면 완료 콜백에서 data/metadata를 out_iov에 추가하여 사용자에게 회신. */

	uint64_t			lba;
	/* [한국어] NVME_IOCTL_SUBMIT_IO 전용. read/write의 시작 LBA (Logical Block Address).
	 *         user_io->slba에서 복사. */
	uint32_t			lba_count;
	/* [한국어] 전송 LBA 개수. NVMe 스펙상 nblocks 필드는 0-based이므로 user_io->nblocks + 1로 저장. */
	uint16_t			apptag;
	/* [한국어] T10 PI(Protection Information) Application Tag. metadata 동반 시 사용. */
	uint16_t			appmask;
	/* [한국어] T10 PI Application Tag mask. 어느 비트를 검사할지 결정. */

	void				*data;
	/* [한국어] 호스트↔디바이스 전송용 DMA 버퍼. spdk_malloc/spdk_zmalloc(SPDK_MALLOC_DMA)로 할당
	 *         → DPDK hugepage 기반 핀된 메모리. NVMe DMA 엔진이 직접 접근 가능.
	 *         정렬: 4096(0x1000) 바이트. 해제: cuse_io_ctx_free → spdk_free. */
	void				*metadata;
	/* [한국어] separate metadata 버퍼 (extended LBA가 아닌 경우). T10 PI 등에 사용. */

	int				data_len;
	/* [한국어] data 버퍼 크기 (바이트). passthru의 경우 cmd->data_len, submit_io의 경우 lba_count*block_size. */
	int				metadata_len;
	/* [한국어] metadata 버퍼 크기 (바이트). 0이면 metadata 미사용. */

	fuse_req_t			req;
	/* [한국어] fuse가 콜백으로 넘겨준 요청 핸들. 완료 시 fuse_reply_ioctl_iov(req, status, ...)로 회신.
	 *         libfuse 내부 자료구조 — 처리 끝나기 전엔 살아있음. */
};

/*
 * [한국어]
 * cuse_io_ctx_free - in-flight ioctl 컨텍스트 해제.
 *
 * @ctx: cuse_nvme_passthru_cmd_send 등에서 할당된 컨텍스트.
 *
 * data/metadata는 spdk_malloc(DMA)으로 할당했으므로 spdk_free로 해제, ctx 본체는 일반 calloc이므로 free.
 * 정상 완료 콜백, 에러 경로 양쪽에서 호출. spdk_free(NULL)/free(NULL) 모두 안전.
 *
 * 호출 체인:
 *   cuse_nvme_passthru_cmd_cb / cuse_nvme_submit_io_*_done / 에러 분기 → [이 함수] → spdk_free + free
 */
static void
cuse_io_ctx_free(struct cuse_io_ctx *ctx)
{
	spdk_free(ctx->data);       /* [한국어] DMA 버퍼 해제. NULL이면 noop. */
	spdk_free(ctx->metadata);   /* [한국어] metadata DMA 버퍼 해제. NULL이면 noop. */
	free(ctx);                  /* [한국어] ctx 본체 해제(일반 calloc 메모리). */
}

/*
 * [한국어] FUSE_REPLY_CHECK_BUFFER 매크로 — fuse ioctl 2단계 핸드셰이크의 첫 단계 응답 헬퍼.
 *
 * fuse ioctl은 다음과 같이 두 번에 걸쳐 호출된다:
 *   1) 첫 호출: in/out 버퍼 크기 모름 → out_bufsz==0. 핸들러는 "응답에 sizeof(val) 바이트가 필요하다"고
 *      fuse_reply_ioctl_retry로 알리고 리턴. 커널이 사용자 공간에 적절한 버퍼를 매핑한 뒤 재호출.
 *   2) 두 번째 호출: out_bufsz==sizeof(val). 핸들러가 실제 데이터를 채워 fuse_reply_ioctl로 회신.
 *
 * 이 매크로는 1단계를 자동으로 처리하기 위함. BLKGETSIZE/BLKSSZGET 등 출력 1개짜리 단순 ioctl에서 사용.
 *
 * @req: fuse 요청 핸들. @arg: 사용자 공간 포인터(아직 매핑되지 않은). @out_bufsz: 0이면 1단계.
 * @val: 실제 회신할 변수 (sizeof로 크기 추출).
 */
#define FUSE_REPLY_CHECK_BUFFER(req, arg, out_bufsz, val)		\
	if (out_bufsz == 0) {						\
		struct iovec out_iov;					\
		out_iov.iov_base = (void *)arg;				\
		out_iov.iov_len = sizeof(val);				\
		fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);	\
		return;							\
	}

#define FUSE_MAX_SIZE 128*1024
/* [한국어] FUSE 요청당 허용 최대 페이로드 크기 = 128KB.
 *         libfuse의 default 제한과 일치. 이를 넘으면 ENOMEM으로 거부. */

/*
 * [한국어]
 * fuse_check_req_size - iovec 배열의 누적 크기가 FUSE_MAX_SIZE 이하인지 검사.
 *
 * @req: fuse 요청 핸들 (초과 시 fuse_reply_err로 에러 응답하기 위해 필요).
 * @iov: 검사할 iovec 배열. @iovcnt: 배열 길이.
 * @return: true=한도 이하 (계속 처리), false=한도 초과 (이미 에러 응답함, 호출자는 즉시 리턴).
 *
 * passthru cmd나 submit_io의 in_iov/out_iov 합산이 128KB를 넘는지 미리 체크하여, 큰 요청을
 * 미리 거부함으로써 libfuse 내부 버퍼 오버플로우를 방지. 단순 누적 합산.
 */
static bool
fuse_check_req_size(fuse_req_t req, struct iovec iov[], int iovcnt)
{
	int total_iov_len = 0;                              /* [한국어] iov 누적 크기 누산기. */
	for (int i = 0; i < iovcnt; i++) {                  /* [한국어] 각 iov의 길이를 더함. */
		total_iov_len += iov[i].iov_len;            /* [한국어] 누적. overflow 가능성은 무시(실용상 충분). */
		if (total_iov_len > FUSE_MAX_SIZE) {        /* [한국어] 128KB 초과 검출. */
			fuse_reply_err(req, ENOMEM);        /* [한국어] 사용자에게 ENOMEM 회신 후 종료. */
			SPDK_ERRLOG("FUSE request cannot be larger that %d\n", FUSE_MAX_SIZE);
			return false;                       /* [한국어] 호출자에게 처리 중단 신호. */
		}
	}
	return true;                                        /* [한국어] 모두 한도 이내 — 계속 진행 가능. */
}

/*
 * [한국어]
 * cuse_nvme_passthru_cmd_cb - NVMe admin/IO passthru 명령의 비동기 완료 콜백.
 *
 * @arg: cuse_io_ctx (queue 호출 시 cb_arg로 넘겼던 것).
 * @cpl: NVMe Completion Queue Entry. status_raw, cdw0(=명령 결과 dword) 등을 담음.
 *
 * 실행 컨텍스트: SPDK reactor (controller 소유 thread). spdk_nvme_qpair_process_completions가
 * 폴링 중 CQE를 발견하고 이 콜백을 동기적으로 호출.
 *
 * 동작:
 *   1) NVMe status 필드 추출 (phase bit 제거 — phase는 큐 wraparound 추적용이라 사용자에게 무의미).
 *   2) iov[0] = cdw0 (명령 결과를 담는 32비트). nvme-cli가 이 값을 result로 받음.
 *   3) C2H(read) 방향이면 data + metadata도 iov에 추가.
 *   4) fuse_reply_ioctl_iov로 사용자 ioctl()에 회신 — libfuse가 내부적으로 fuse fd write.
 *   5) ctx 해제.
 *
 * 호출 체인:
 *   PCIe device → CQE → spdk_nvme_qpair_process_completions → [이 콜백] → fuse_reply_ioctl_iov
 */
static void
cuse_nvme_passthru_cmd_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct cuse_io_ctx *ctx = arg;                                /* [한국어] cb_arg로 전달된 컨텍스트 복원. */
	struct iovec out_iov[3];                                      /* [한국어] 응답 iov: cdw0(필수) + 선택 data + 선택 metadata. */
	struct spdk_nvme_cpl _cpl;                                    /* [한국어] cpl을 로컬에 복사 — phase bit 제거 후 cdw0만 가리키기 위해. */
	int out_iovcnt = 0;                                           /* [한국어] 실제 사용한 iov 개수. */
	uint16_t status_field = cpl->status_raw >> 1; /* Drop out phase bit */
	/* [한국어] NVMe CQE의 status_raw[15:0]는 [phase tag(1) | status(15)] 구성.
	 *         phase는 SQ/CQ wraparound 추적용 — 사용자 ioctl에는 status만 필요하므로 1비트 시프트로 제거.
	 *         결과: nvme-cli의 NVME_STATUS_*와 호환되는 형태. */

	memcpy(&_cpl, cpl, sizeof(struct spdk_nvme_cpl));             /* [한국어] cpl 복사 — 원본은 SPDK 내부 자료. */
	out_iov[out_iovcnt].iov_base = &_cpl.cdw0;                    /* [한국어] iov[0] = 명령 결과 32비트(cdw0). */
	out_iov[out_iovcnt].iov_len = sizeof(_cpl.cdw0);              /* [한국어] cdw0 크기 = 4바이트. */
	out_iovcnt += 1;                                              /* [한국어] iov 1개 사용. */

	if (ctx->data_transfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		/* [한국어] read류 명령(C2H): 디바이스가 호스트 버퍼로 데이터를 적었으므로 사용자에게 돌려줘야 함. */
		if (ctx->data_len > 0) {
			out_iov[out_iovcnt].iov_base = ctx->data; /* [한국어] DMA 버퍼를 사용자 응답 iov에 추가. */
			out_iov[out_iovcnt].iov_len = ctx->data_len;
			out_iovcnt += 1;
		}
		if (ctx->metadata_len > 0) {
			out_iov[out_iovcnt].iov_base = ctx->metadata; /* [한국어] metadata도 동일하게 추가. */
			out_iov[out_iovcnt].iov_len = ctx->metadata_len;
			out_iovcnt += 1;
		}
	}

	fuse_reply_ioctl_iov(ctx->req, status_field, out_iov, out_iovcnt);
	/* [한국어] libfuse에 응답 회신. status_field가 사용자 ioctl()의 반환값(>=0)으로 사용됨.
	 *         NVMe status가 0이면 사용자도 0(성공)을 받음. iov 데이터는 사용자 버퍼에 복사됨. */
	cuse_io_ctx_free(ctx);                                        /* [한국어] 더 이상 필요 없는 컨텍스트 해제. */
}

/*
 * [한국어]
 * cuse_nvme_passthru_cmd_execute - reactor 컨텍스트에서 실제 NVMe 명령 발사.
 *
 * @ctrlr: NVMe controller (nvme_io_msg_process가 이 콜백 호출 시 넘겨줌).
 * @nsid: 네임스페이스 ID. 0이면 admin 명령, >0이면 IO 명령으로 분기.
 * @arg: cuse_io_ctx (CUSE thread에서 nvme_io_msg_send로 넘긴 것).
 *
 * 실행 컨텍스트: SPDK reactor (controller 소유 thread). nvme_io_msg.c의 ring dequeue 후 호출.
 * 즉 CUSE thread → ring → 여기 = reactor 안전 진입.
 *
 * 동작:
 *   - ns 명령(IO_CMD): spdk_nvme_ctrlr_cmd_io_raw_with_md (external_io_msgs_qpair에 IO SQE 발사).
 *   - admin 명령(ADMIN_CMD): spdk_nvme_ctrlr_cmd_admin_raw (admin qpair에 SQE 발사).
 *   완료는 비동기 — cuse_nvme_passthru_cmd_cb가 나중에 호출됨.
 *   submit 자체가 실패(rc<0)하면 즉시 fuse_reply_err + ctx 해제.
 *
 * 호출 체인:
 *   nvme_io_msg_process → [이 함수] → spdk_nvme_ctrlr_cmd_*_raw → 디바이스
 */
static void
cuse_nvme_passthru_cmd_execute(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;                                              /* [한국어] submit 결과 코드. */
	struct cuse_io_ctx *ctx = arg;                       /* [한국어] 컨텍스트 복원. */

	if (nsid != 0) {
		/* [한국어] namespace 대상 IO 명령. external_io_msgs_qpair는 nvme_io_msg.c가 controller 등록 시
		 *         만들어 둔 전용 IO 큐페어 — 외부 producer 전용으로 사용됨. */
		rc = spdk_nvme_ctrlr_cmd_io_raw_with_md(ctrlr, ctrlr->external_io_msgs_qpair, &ctx->nvme_cmd,
							ctx->data,
							ctx->data_len, ctx->metadata, cuse_nvme_passthru_cmd_cb, (void *)ctx);
	} else {
		/* [한국어] admin 명령은 controller의 admin qpair로 발사 (별도 인자 불필요).
		 *         metadata는 admin에서 일반적으로 사용하지 않으므로 _admin_raw 사용. */
		rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &ctx->nvme_cmd, ctx->data, ctx->data_len,
						   cuse_nvme_passthru_cmd_cb, (void *)ctx);
	}
	if (rc < 0) {
		/* [한국어] submit 실패: 큐 가득참, ctrlr 비활성, 메모리 부족 등.
		 *         사용자에게 EINVAL로 회신 후 컨텍스트 정리. */
		fuse_reply_err(ctx->req, EINVAL);
		cuse_io_ctx_free(ctx);
	}
}

/*
 * [한국어]
 * cuse_nvme_passthru_cmd_send - NVMe passthru ioctl을 위한 컨텍스트 준비 + reactor에 위임.
 *
 * @req: fuse 요청. @passthru_cmd: 사용자가 ioctl로 보낸 nvme_passthru_cmd 구조체.
 * @data: 사용자 공간 입력 데이터(H2C일 때 NULL이 아님). @metadata: 사용자 입력 metadata.
 * @cmd: ioctl 번호 (NVME_IOCTL_ADMIN_CMD 또는 IO_CMD/etc) — admin/IO 분기 판단용.
 *
 * 실행 컨텍스트: CUSE thread.
 *
 * 동작:
 *   1) ctx 할당 + 사용자 nvme_passthru_cmd → SPDK nvme_cmd로 필드 매핑 (opc, nsid, cdw10..cdw15).
 *   2) data/metadata DMA 버퍼 할당 (spdk_malloc, 4KB 정렬, hugepage). H2C면 사용자 데이터 복사.
 *   3) nvme_io_msg_send로 ring enqueue → reactor가 나중에 cuse_nvme_passthru_cmd_execute 호출.
 *      ADMIN_CMD면 nsid=0으로 보내 admin 큐로, 아니면 cmd의 nsid로 IO 큐로 분기.
 *   4) enqueue 실패 시 fuse_reply_err + 정리.
 *
 * 호출 체인:
 *   cuse_nvme_passthru_cmd → [이 함수] → nvme_io_msg_send → ring → reactor
 */
static void
cuse_nvme_passthru_cmd_send(fuse_req_t req, struct nvme_passthru_cmd *passthru_cmd,
			    const void *data, const void *metadata, int cmd)
{
	struct cuse_io_ctx *ctx;                               /* [한국어] 새로 할당할 in-flight 컨텍스트. */
	struct cuse_device *cuse_device = fuse_req_userdata(req); /* [한국어] cuse_lowlevel_setup 시 등록한 userdata = cuse_device. */
	int rv;                                                /* [한국어] enqueue 결과 코드. */

	ctx = (struct cuse_io_ctx *)calloc(1, sizeof(struct cuse_io_ctx));
	/* [한국어] 0-init 컨텍스트 할당. spdk_malloc이 아닌 일반 calloc — DMA 대상 아님(SQE 빌더용). */
	if (!ctx) {
		SPDK_ERRLOG("Cannot allocate memory for cuse_io_ctx\n");
		fuse_reply_err(req, ENOMEM);                   /* [한국어] OOM은 사용자에게 ENOMEM. */
		return;
	}

	ctx->req = req;                                        /* [한국어] 완료 시 회신할 fuse 요청 보존. */
	ctx->data_transfer = spdk_nvme_opc_get_data_transfer(passthru_cmd->opcode);
	/* [한국어] opcode의 [1:0] 비트로 데이터 방향 추출 (NVMe Base Spec — Opcode field).
	 *         NONE / H2C / C2H / BIDIRECTIONAL 중 하나. 완료 콜백에서 회신 iov 구성에 사용. */

	memset(&ctx->nvme_cmd, 0, sizeof(ctx->nvme_cmd));      /* [한국어] SQE 64바이트 0으로 초기화. */
	ctx->nvme_cmd.opc = passthru_cmd->opcode;              /* [한국어] cdw0[7:0] — Opcode. */
	ctx->nvme_cmd.nsid = passthru_cmd->nsid;               /* [한국어] cdw1 — Namespace ID. */
	ctx->nvme_cmd.cdw10 = passthru_cmd->cdw10;             /* [한국어] cdw10 — opc별 의미(예: Identify면 CNS+CNTID). */
	ctx->nvme_cmd.cdw11 = passthru_cmd->cdw11;             /* [한국어] cdw11 — opc별. */
	ctx->nvme_cmd.cdw12 = passthru_cmd->cdw12;             /* [한국어] cdw12 — opc별. */
	ctx->nvme_cmd.cdw13 = passthru_cmd->cdw13;             /* [한국어] cdw13 — opc별. */
	ctx->nvme_cmd.cdw14 = passthru_cmd->cdw14;             /* [한국어] cdw14 — opc별. */
	ctx->nvme_cmd.cdw15 = passthru_cmd->cdw15;             /* [한국어] cdw15 — opc별. */

	ctx->data_len = passthru_cmd->data_len;                /* [한국어] 데이터 페이로드 크기 (바이트). */
	ctx->metadata_len = passthru_cmd->metadata_len;        /* [한국어] metadata 페이로드 크기. */

	if (ctx->data_len > 0) {
		ctx->data = spdk_malloc(ctx->data_len, 4096, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		/* [한국어] DMA용 hugepage 메모리 할당, 4KB 정렬(NVMe PRP 요구).
		 *         SPDK_ENV_LCORE_ID_ANY = 어느 NUMA든 OK. SPDK_MALLOC_DMA = NVMe DMA 가능 영역. */
		if (!ctx->data) {
			SPDK_ERRLOG("Cannot allocate memory for data\n");
			fuse_reply_err(req, ENOMEM);
			free(ctx);                              /* [한국어] DMA 버퍼 할당 실패 — ctx만 free (data는 NULL). */
			return;
		}
		if (data != NULL) {
			memcpy(ctx->data, data, ctx->data_len); /* [한국어] H2C: 사용자 입력을 DMA 버퍼로 복사. */
		}
	}

	if (ctx->metadata_len > 0) {
		ctx->metadata = spdk_malloc(ctx->metadata_len, 4096, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		/* [한국어] metadata 전용 DMA 버퍼. data와 별도 — separate metadata 모드용. */
		if (!ctx->metadata) {
			SPDK_ERRLOG("Cannot allocate memory for metadata\n");
			fuse_reply_err(req, ENOMEM);
			cuse_io_ctx_free(ctx);                  /* [한국어] data까지 할당된 상태이므로 _free로 일괄 정리. */
			return;
		}
		if (metadata != NULL) {
			memcpy(ctx->metadata, metadata, ctx->metadata_len); /* [한국어] H2C: metadata 복사. */
		}
	}

	if ((unsigned int)cmd != NVME_IOCTL_ADMIN_CMD) {
		/* Send NS for IO IOCTLs */
		/* [한국어] IO_CMD: namespace 대상이므로 nsid를 그대로 reactor에 전달.
		 *         nvme_io_msg_send 내부에서 nsid를 콜백에 전달. */
		rv = nvme_io_msg_send(cuse_device->ctrlr, passthru_cmd->nsid, cuse_nvme_passthru_cmd_execute, ctx);
	} else {
		/* NS == 0 for Admin IOCTLs */
		/* [한국어] ADMIN_CMD: nsid=0 (admin 큐 사용). cuse_nvme_passthru_cmd_execute가 nsid==0 분기. */
		rv = nvme_io_msg_send(cuse_device->ctrlr, 0, cuse_nvme_passthru_cmd_execute, ctx);
	}
	if (rv) {
		/* [한국어] ring enqueue 실패 — ring full, OOM, ctrlr 해제 중 등. 사용자에게 회신 후 정리. */
		SPDK_ERRLOG("Cannot send io msg to the controller\n");
		fuse_reply_err(req, -rv);                       /* [한국어] -rv = errno-style. */
		cuse_io_ctx_free(ctx);
		return;
	}
}

/*
 * [한국어]
 * cuse_nvme_passthru_cmd - NVME_IOCTL_ADMIN_CMD/IO_CMD ioctl 진입 핸들러.
 *
 * @req: fuse 요청. @cmd: ioctl 번호 (ADMIN_CMD/IO_CMD).
 * @arg: 사용자 공간의 nvme_passthru_cmd 포인터(아직 매핑 안 됐을 수 있음).
 * @fi: fuse_file_info (미사용). @flags: ioctl 플래그.
 * @in_buf/in_bufsz: 매핑된 입력 버퍼. @out_bufsz: 출력 버퍼 크기 힌트.
 *
 * 실행 컨텍스트: CUSE thread.
 *
 * fuse ioctl 2단계 핸드셰이크:
 *   1) 첫 호출: in_bufsz==0. in_iov로 "passthru_cmd 헤더 + (H2C면) data/metadata 입력" 요청 후 retry.
 *   2) 두 번째 호출: in_buf 매핑됨. out_iov 준비 (result 필드 + (C2H면) data/metadata 출력).
 *      out_bufsz==0이면 다시 한 번 retry로 출력 버퍼 매핑 요청.
 *   3) 세 번째 호출: in/out 모두 매핑됨. cuse_nvme_passthru_cmd_send로 실제 처리 진행.
 *
 * BIDIRECTIONAL은 NVMe spec에 거의 사용되지 않으므로 EINVAL.
 *
 * 호출 체인: cuse_ctrlr_ioctl/cuse_ns_ioctl → [이 함수] → cuse_nvme_passthru_cmd_send
 */
static void
cuse_nvme_passthru_cmd(fuse_req_t req, int cmd, void *arg,
		       struct fuse_file_info *fi, unsigned flags,
		       const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct nvme_passthru_cmd *passthru_cmd;            /* [한국어] 사용자 ioctl 페이로드 헤더. */
	struct iovec in_iov[3], out_iov[3];                /* [한국어] in/out iov: 헤더 + 선택 data + 선택 metadata. */
	int in_iovcnt = 0, out_iovcnt = 0;                 /* [한국어] 실제 iov 개수. */
	const void *dptr = NULL, *mdptr = NULL;            /* [한국어] in_buf 내 data/metadata 시작 포인터. */
	enum spdk_nvme_data_transfer data_transfer;        /* [한국어] opcode에서 추출한 데이터 방향. */

	in_iov[in_iovcnt].iov_base = (void *)arg;          /* [한국어] iov[0] = 사용자 공간의 nvme_passthru_cmd. */
	in_iov[in_iovcnt].iov_len = sizeof(*passthru_cmd); /* [한국어] 헤더 크기. */
	in_iovcnt += 1;
	if (in_bufsz == 0) {
		/* [한국어] 1단계: 입력 헤더만 요청하여 매핑 받음. data/metadata는 다음 단계에서 추가. */
		fuse_reply_ioctl_retry(req, in_iov, in_iovcnt, NULL, out_iovcnt);
		return;
	}

	passthru_cmd = (struct nvme_passthru_cmd *)in_buf; /* [한국어] 매핑된 헤더. */
	data_transfer = spdk_nvme_opc_get_data_transfer(passthru_cmd->opcode); /* [한국어] 방향 결정. */

	if (data_transfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		/* Make data pointer accessible (RO) */
		/* [한국어] H2C: 사용자 buffer의 data 포인터(가상 주소)를 in_iov에 추가하여 매핑 요청.
		 *         RO 매핑이면 충분(우리가 디바이스로 복사만 함). */
		if (passthru_cmd->addr != 0) {
			in_iov[in_iovcnt].iov_base = (void *)passthru_cmd->addr;
			in_iov[in_iovcnt].iov_len = passthru_cmd->data_len;
			in_iovcnt += 1;
		}
		/* Make metadata pointer accessible (RO) */
		if (passthru_cmd->metadata != 0) {
			in_iov[in_iovcnt].iov_base = (void *)passthru_cmd->metadata;
			in_iov[in_iovcnt].iov_len = passthru_cmd->metadata_len;
			in_iovcnt += 1;
		}
	}

	if (!fuse_check_req_size(req, in_iov, in_iovcnt)) {
		/* [한국어] 누적 in_iov가 128KB 초과 — 이미 ENOMEM 회신됨, 즉시 종료. */
		return;
	}
	/* Always make result field writeable regardless of data transfer bits */
	/* [한국어] passthru_cmd->result(=cdw0)는 항상 사용자에게 돌려줘야 하므로 out_iov[0]에 writeable로 매핑.
	 *         opcode가 NONE이어도 result는 항상 회신. */
	out_iov[out_iovcnt].iov_base = &((struct nvme_passthru_cmd *)arg)->result;
	out_iov[out_iovcnt].iov_len = sizeof(uint32_t);
	out_iovcnt += 1;

	if (data_transfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		/* Make data pointer accessible (WO) */
		/* [한국어] C2H: 디바이스가 호스트 buffer에 적을 영역. WO 매핑으로 사용자 공간에 노출. */
		if (passthru_cmd->data_len > 0) {
			out_iov[out_iovcnt].iov_base = (void *)passthru_cmd->addr;
			out_iov[out_iovcnt].iov_len = passthru_cmd->data_len;
			out_iovcnt += 1;
		}
		/* Make metadata pointer accessible (WO) */
		if (passthru_cmd->metadata_len > 0) {
			out_iov[out_iovcnt].iov_base = (void *)passthru_cmd->metadata;
			out_iov[out_iovcnt].iov_len = passthru_cmd->metadata_len;
			out_iovcnt += 1;
		}
	}

	if (!fuse_check_req_size(req, out_iov, out_iovcnt)) {
		/* [한국어] out_iov 누적이 한도 초과. */
		return;
	}

	if (out_bufsz == 0) {
		/* [한국어] 2단계: 출력 buffer 매핑 요청. 다음 호출에서 in/out 모두 준비됨. */
		fuse_reply_ioctl_retry(req, in_iov, in_iovcnt, out_iov, out_iovcnt);
		return;
	}

	if (data_transfer == SPDK_NVME_DATA_BIDIRECTIONAL) {
		/* [한국어] 양방향 명령은 fuse 매핑 모델과 맞지 않으므로 거부. NVMe spec상 매우 드묾. */
		fuse_reply_err(req, EINVAL);
		return;
	}

	if (data_transfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		/* [한국어] H2C: in_buf 안에 [헤더 | data | metadata] 순으로 매핑되어 있음.
		 *         dptr/mdptr는 헤더 다음에 위치. addr/metadata가 0이면 NULL로 두어 미사용 표시. */
		dptr = (passthru_cmd->addr == 0) ? NULL : (uint8_t *)in_buf + sizeof(*passthru_cmd);
		mdptr = (passthru_cmd->metadata == 0) ? NULL : (uint8_t *)in_buf + sizeof(*passthru_cmd) +
			passthru_cmd->data_len;
	}

	cuse_nvme_passthru_cmd_send(req, passthru_cmd, dptr, mdptr, cmd);
	/* [한국어] 모든 입력 준비 완료 — 컨텍스트 할당 + reactor에 위임. */
}

/*
 * [한국어]
 * cuse_nvme_reset_execute - reactor 컨텍스트에서 controller 리셋 실행.
 *
 * @ctrlr: NVMe controller. @nsid: 미사용. @arg: fuse_req_t (req 회신용).
 *
 * 실행 컨텍스트: SPDK reactor. spdk_nvme_ctrlr_reset은 동기 함수로, controller를 disable→enable하며
 * 모든 in-flight I/O를 abort. 비교적 무거운 동작이므로 reactor에서 단독으로 실행해야 함.
 *
 * 호출 체인: nvme_io_msg_process → [이 함수] → spdk_nvme_ctrlr_reset → fuse_reply_*
 */
static void
cuse_nvme_reset_execute(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;                                              /* [한국어] reset 결과. */
	fuse_req_t req = arg;                                /* [한국어] arg는 fuse 요청 핸들 자체 (별도 ctx 불필요). */

	rc = spdk_nvme_ctrlr_reset(ctrlr);                   /* [한국어] CC.EN clear → enable 사이클. */
	if (rc) {
		fuse_reply_err(req, rc);                     /* [한국어] reset 실패 — 그대로 errno로 회신. */
		return;
	}

	fuse_reply_ioctl_iov(req, 0, NULL, 0);               /* [한국어] 성공 — 회신 페이로드 없음, status=0. */
}

/*
 * [한국어]
 * cuse_nvme_subsys_reset_execute - reactor 컨텍스트에서 subsystem 리셋 실행.
 *
 * subsystem reset은 controller reset보다 더 광범위한 NVMe Subsystem Reset(NSSR)에 해당하며
 * subsystem 단위의 모든 controller에 영향. 일부 디바이스만 지원.
 *
 * 호출 체인: nvme_io_msg_process → [이 함수] → spdk_nvme_ctrlr_reset_subsystem → fuse_reply_*
 */
static void
cuse_nvme_subsys_reset_execute(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;
	fuse_req_t req = arg;

	rc = spdk_nvme_ctrlr_reset_subsystem(ctrlr);         /* [한국어] NSSR(Subsystem Reset) 발사. */
	if (rc) {
		fuse_reply_err(req, rc);
		return;
	}

	fuse_reply_ioctl_iov(req, 0, NULL, 0);
}

/*
 * [한국어]
 * cuse_nvme_reset - NVME_IOCTL_RESET 또는 NVME_IOCTL_SUBSYS_RESET ioctl 핸들러.
 *
 * 컨트롤러 device(nsid==0)에서만 허용. namespace device에서 호출되면 EINVAL.
 * reset은 데이터 페이로드가 없으므로 fuse retry 단계 없이 바로 reactor에 위임.
 *
 * 호출 체인: cuse_ctrlr_ioctl(NVME_IOCTL_RESET/SUBSYS_RESET) → [이 함수] → nvme_io_msg_send
 */
static void
cuse_nvme_reset(fuse_req_t req, int cmd, void *arg,
		struct fuse_file_info *fi, unsigned flags,
		const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	int rv;
	struct cuse_device *cuse_device = fuse_req_userdata(req); /* [한국어] cuse_device 복원. */

	if (cuse_device->nsid) {
		/* [한국어] namespace device에서 reset 요청 — NVMe는 controller 단위 reset만 정의.
		 *         Linux 커널 driver와 동작 일치. */
		SPDK_ERRLOG("Namespace reset not supported\n");
		fuse_reply_err(req, EINVAL);
		return;
	}

	if (cmd == NVME_IOCTL_SUBSYS_RESET) {
		/* [한국어] subsystem 전체 리셋 (NSSR). 디바이스 미지원 시 reactor에서 errno 반환. */
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_SUBSYS_RESET\n");
		rv = nvme_io_msg_send(cuse_device->ctrlr, cuse_device->nsid, cuse_nvme_subsys_reset_execute,
				      (void *)req);
	} else {
		/* [한국어] controller 리셋 (CC.EN cycle). */
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_RESET\n");
		rv = nvme_io_msg_send(cuse_device->ctrlr, cuse_device->nsid, cuse_nvme_reset_execute, (void *)req);
	}
	if (rv) {
		SPDK_ERRLOG("Cannot send reset\n");
		fuse_reply_err(req, EINVAL);                  /* [한국어] enqueue 실패. */
	}
}

/*
 * [한국어]
 * cuse_nvme_rescan_execute - reactor 컨텍스트에서 모든 active namespace의 Identify Namespace 갱신.
 *
 * @unused_nsid: 0 (controller-level 작업이라 의미 없음).
 * @arg: fuse_req_t.
 *
 * 모든 active namespace에 대해 nvme_ns_construct를 다시 호출 → Identify Namespace 명령으로
 * NS 메타데이터(LBA size, capacity 등) 갱신. nvme-cli "nvme rescan-controller"에 대응.
 * Identify 실패는 무시 (일부 NS만 일시 비활성일 수 있음).
 *
 * 호출 체인: nvme_io_msg_process → [이 함수] → nvme_ns_construct (각 ns)
 */
static void
cuse_nvme_rescan_execute(struct spdk_nvme_ctrlr *ctrlr, uint32_t unused_nsid, void *arg)
{
	fuse_req_t req = arg;                                  /* [한국어] 회신할 요청 핸들. */
	struct spdk_nvme_ns *ns;                               /* [한국어] 각 namespace 객체. */
	uint32_t nsid;                                         /* [한국어] active ns 순회 인덱스. */

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		/* [한국어] active ns ID를 작은 값부터 순회. 0 반환이면 더 이상 없음. */
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);      /* [한국어] nsid → ns 객체 매핑. */
		if (ns == NULL) {
			continue;                              /* [한국어] 비정상 NULL은 스킵 (race 가능성). */
		}
		/* Identify to namespace can fail, do not check return. */
		/* [한국어] Identify Namespace를 재발사하여 NS 메타데이터(NSZE, NCAP, LBAF 등) 최신화.
		 *         일시적 실패는 무시 — best-effort 갱신. */
		nvme_ns_construct(ns, nsid, ctrlr);
	}

	fuse_reply_ioctl_iov(req, 0, NULL, 0);                  /* [한국어] 항상 성공으로 회신 (best-effort 의미). */
}

/*
 * [한국어]
 * cuse_nvme_rescan - NVME_IOCTL_RESCAN ioctl 핸들러.
 *
 * 컨트롤러 device(nsid==0)에서만 허용. namespace device에서는 EINVAL.
 *
 * 호출 체인: cuse_ctrlr_ioctl(NVME_IOCTL_RESCAN) → [이 함수] → nvme_io_msg_send
 */
static void
cuse_nvme_rescan(fuse_req_t req, int cmd, void *arg,
		 struct fuse_file_info *fi, unsigned flags,
		 const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	int rv;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	if (cuse_device->nsid) {
		/* [한국어] rescan은 controller 전체 NS 목록을 다시 살피는 작업이라 NS device에서 의미 없음. */
		SPDK_ERRLOG("Namespace rescan not supported\n");
		fuse_reply_err(req, EINVAL);
		return;
	}

	rv = nvme_io_msg_send(cuse_device->ctrlr, cuse_device->nsid, cuse_nvme_rescan_execute, (void *)req);
	/* [한국어] reactor에 위임 (nsid==0). */
	if (rv) {
		SPDK_ERRLOG("Cannot send rescan\n");
		fuse_reply_err(req, EINVAL);
	}
}

/*****************************************************************************
 * Namespace IO requests
 */
/* [한국어] 아래 섹션은 NVME_IOCTL_SUBMIT_IO 처리 — read/write 직접 제출.
 *         passthru와 달리 사용자가 opcode/lba/nblocks 형태로 명시하고, SPDK는
 *         spdk_nvme_ns_cmd_read_with_md / write_with_md를 사용해 SQE 빌드. */

/*
 * [한국어]
 * cuse_nvme_submit_io_write_done - SUBMIT_IO write 완료 콜백.
 *
 * write는 응답 데이터가 없으므로 status만 회신. data/metadata 버퍼는 송신용이므로 정리.
 * 실행 컨텍스트: reactor.
 *
 * 호출 체인: 디바이스 → CQE → spdk_nvme_qpair_process_completions → [이 콜백]
 */
static void
cuse_nvme_submit_io_write_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct cuse_io_ctx *ctx = (struct cuse_io_ctx *)ref;   /* [한국어] cb_arg 복원. */
	uint16_t status_field = cpl->status_raw >> 1; /* Drop out phase bit */
	/* [한국어] phase bit 제거 후 status만 추출. */

	fuse_reply_ioctl_iov(ctx->req, status_field, NULL, 0);  /* [한국어] write 응답: 페이로드 없음, status만. */

	cuse_io_ctx_free(ctx);                                  /* [한국어] DMA 버퍼 + ctx 해제. */
}

/*
 * [한국어]
 * cuse_nvme_submit_io_write_cb - reactor 컨텍스트에서 write SQE 발사.
 *
 * @ctrlr/@nsid: io_msg_process가 전달. @arg: cuse_io_ctx.
 *
 * spdk_nvme_ns_cmd_write_with_md로 NS write 명령을 external_io_msgs_qpair에 발사.
 * appmask/apptag(0이면 PI 비활성)와 LBA/lba_count는 ctx에 저장된 값 사용.
 *
 * 호출 체인: nvme_io_msg_process → [이 함수] → spdk_nvme_ns_cmd_write_with_md
 */
static void
cuse_nvme_submit_io_write_cb(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;
	struct cuse_io_ctx *ctx = arg;
	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid); /* [한국어] nsid → ns 객체. */

	rc = spdk_nvme_ns_cmd_write_with_md(ns, ctrlr->external_io_msgs_qpair, ctx->data, ctx->metadata,
					    ctx->lba, /* LBA start */
					    ctx->lba_count, /* number of LBAs */
					    cuse_nvme_submit_io_write_done, ctx, 0,
					    ctx->appmask, ctx->apptag);
	/* [한국어] external producer 전용 IO 큐페어에 write SQE 발사.
	 *         io_flags=0 (Force Unit Access 등 미사용).
	 *         appmask/apptag는 T10 PI 사용시 Application Tag 검증용 — 0이면 검증 비활성. */

	if (rc != 0) {
		SPDK_ERRLOG("write failed: rc = %d\n", rc);
		fuse_reply_err(ctx->req, rc);                  /* [한국어] submit 실패 시 사용자에게 errno 회신. */
		cuse_io_ctx_free(ctx);
		return;
	}
}

/*
 * [한국어]
 * cuse_nvme_submit_io_write - SUBMIT_IO write 경로의 컨텍스트 준비 + 위임.
 *
 * @cuse_device: 호출된 ns의 cuse_device. @block_size/@md_size: 해당 ns의 sector/metadata 크기.
 * @in_buf: 매핑된 입력 = [nvme_user_io | data | metadata?] 직렬화된 형태.
 *
 * 동작:
 *   - lba_count = nblocks + 1 (NVMe 0-based).
 *   - data_len = lba_count * block_size로 DMA 버퍼 할당하고 사용자 데이터 복사.
 *   - metadata 포인터가 있으면 추가 DMA 버퍼 + 사용자 metadata 복사 + apptag/appmask 저장.
 *   - nvme_io_msg_send로 reactor에 위임.
 *
 * 호출 체인: cuse_nvme_submit_io → [이 함수] → nvme_io_msg_send
 */
static void
cuse_nvme_submit_io_write(struct cuse_device *cuse_device, fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags, uint32_t block_size, uint32_t md_size,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	const struct nvme_user_io *user_io = in_buf;            /* [한국어] in_buf 시작은 nvme_user_io 헤더. */
	struct cuse_io_ctx *ctx;
	int rc;

	ctx = (struct cuse_io_ctx *)calloc(1, sizeof(struct cuse_io_ctx));
	if (!ctx) {
		SPDK_ERRLOG("Cannot allocate memory for context\n");
		fuse_reply_err(req, ENOMEM);
		return;
	}

	ctx->req = req;
	ctx->lba = user_io->slba;                               /* [한국어] 시작 LBA. */
	ctx->lba_count = user_io->nblocks + 1;                  /* [한국어] NVMe nblocks는 0-based 표현 → +1. */
	ctx->data_len = ctx->lba_count * block_size;            /* [한국어] 총 전송 바이트 수. */

	ctx->data = spdk_zmalloc(ctx->data_len, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY,
				 SPDK_MALLOC_DMA);
	/* [한국어] zero-init DMA 버퍼 할당, 4KB 정렬. NUMA_ID_ANY = 어느 노드든 OK. */
	if (ctx->data == NULL) {
		SPDK_ERRLOG("Write buffer allocation failed\n");
		fuse_reply_err(ctx->req, ENOMEM);
		free(ctx);
		return;
	}

	memcpy(ctx->data, (uint8_t *)in_buf + sizeof(*user_io), ctx->data_len);
	/* [한국어] in_buf 배치: [user_io 헤더 | 실제 write 데이터]. 헤더 다음부터 data_len만큼 복사. */

	if (user_io->metadata) {
		/* [한국어] separate metadata 모드: 사용자가 metadata 포인터를 제공한 경우만 활성화. */
		ctx->apptag = user_io->apptag;                  /* [한국어] T10 PI Application Tag. */
		ctx->appmask = user_io->appmask;                /* [한국어] Tag mask. */
		ctx->metadata_len = md_size * ctx->lba_count;   /* [한국어] metadata 총 크기. */
		ctx->metadata = spdk_zmalloc(ctx->metadata_len, 4096, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		/* [한국어] metadata DMA 버퍼 할당. */

		if (ctx->metadata == NULL) {
			SPDK_ERRLOG("Cannot allocate memory for metadata\n");
			if (ctx->metadata_len == 0) {
				/* [한국어] md_size==0이면 디바이스가 metadata 미지원 — 진단 메시지. */
				SPDK_ERRLOG("Device format does not support metadata\n");
			}
			fuse_reply_err(req, ENOMEM);
			cuse_io_ctx_free(ctx);
			return;
		}

		memcpy(ctx->metadata, (uint8_t *)in_buf + sizeof(*user_io) + ctx->data_len,
		       ctx->metadata_len);
		/* [한국어] in_buf 배치: [헤더 | data | metadata]. data 다음부터 metadata_len만큼 복사. */
	}

	rc = nvme_io_msg_send(cuse_device->ctrlr, cuse_device->nsid, cuse_nvme_submit_io_write_cb,
			      ctx);
	/* [한국어] reactor에 위임. */
	if (rc < 0) {
		SPDK_ERRLOG("Cannot send write io\n");
		fuse_reply_err(ctx->req, rc);
		cuse_io_ctx_free(ctx);
	}
}

/*
 * [한국어]
 * cuse_nvme_submit_io_read_done - SUBMIT_IO read 완료 콜백.
 *
 * read는 사용자에게 data(+metadata)를 회신해야 하므로 iov로 패킹하여 fuse_reply.
 * 실행 컨텍스트: reactor.
 */
static void
cuse_nvme_submit_io_read_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct cuse_io_ctx *ctx = (struct cuse_io_ctx *)ref;
	struct iovec iov[2];                                    /* [한국어] 응답 iov: data + 선택 metadata. */
	int iovcnt = 0;
	uint16_t status_field = cpl->status_raw >> 1; /* Drop out phase bit */

	iov[iovcnt].iov_base = ctx->data;                       /* [한국어] 디바이스가 적은 데이터를 사용자에게 회신. */
	iov[iovcnt].iov_len = ctx->data_len;
	iovcnt += 1;

	if (ctx->metadata) {
		iov[iovcnt].iov_base = ctx->metadata;           /* [한국어] metadata가 있으면 추가. */
		iov[iovcnt].iov_len = ctx->metadata_len;
		iovcnt += 1;
	}

	fuse_reply_ioctl_iov(ctx->req, status_field, iov, iovcnt); /* [한국어] 사용자에게 회신. */

	cuse_io_ctx_free(ctx);                                  /* [한국어] 정리. */
}

/*
 * [한국어]
 * cuse_nvme_submit_io_read_cb - reactor 컨텍스트에서 read SQE 발사.
 *
 * spdk_nvme_ns_cmd_read_with_md로 NS read 명령을 external_io_msgs_qpair에 발사.
 * 완료는 cuse_nvme_submit_io_read_done이 처리.
 */
static void
cuse_nvme_submit_io_read_cb(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;
	struct cuse_io_ctx *ctx = arg;
	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

	rc = spdk_nvme_ns_cmd_read_with_md(ns, ctrlr->external_io_msgs_qpair, ctx->data, ctx->metadata,
					   ctx->lba, /* LBA start */
					   ctx->lba_count, /* number of LBAs */
					   cuse_nvme_submit_io_read_done, ctx, 0,
					   ctx->appmask, ctx->apptag);
	/* [한국어] read SQE 발사. io_flags=0. PI 검증은 appmask/apptag 적용. */

	if (rc != 0) {
		SPDK_ERRLOG("read failed: rc = %d\n", rc);
		fuse_reply_err(ctx->req, rc);
		cuse_io_ctx_free(ctx);
		return;
	}
}

/*
 * [한국어]
 * cuse_nvme_submit_io_read - SUBMIT_IO read 경로의 컨텍스트 준비 + 위임.
 *
 * write와 대칭이지만 사용자 입력 데이터 복사 단계가 없음 (디바이스가 채워줄 것).
 * data 버퍼는 zmalloc으로 0-init하여 할당만 해두고, 완료 후 사용자에게 iov로 회신.
 */
static void
cuse_nvme_submit_io_read(struct cuse_device *cuse_device, fuse_req_t req, int cmd, void *arg,
			 struct fuse_file_info *fi, unsigned flags, uint32_t block_size, uint32_t md_size,
			 const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	int rc;
	struct cuse_io_ctx *ctx;
	const struct nvme_user_io *user_io = in_buf;

	ctx = (struct cuse_io_ctx *)calloc(1, sizeof(struct cuse_io_ctx));
	if (!ctx) {
		SPDK_ERRLOG("Cannot allocate memory for context\n");
		fuse_reply_err(req, ENOMEM);
		return;
	}

	ctx->req = req;
	ctx->lba = user_io->slba;
	ctx->lba_count = user_io->nblocks + 1;

	ctx->data_len = ctx->lba_count * block_size;
	ctx->data = spdk_zmalloc(ctx->data_len, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY,
				 SPDK_MALLOC_DMA);
	/* [한국어] read는 디바이스가 데이터 채울 영역만 준비. */
	if (ctx->data == NULL) {
		SPDK_ERRLOG("Read buffer allocation failed\n");
		fuse_reply_err(ctx->req, ENOMEM);
		free(ctx);
		return;
	}

	if (user_io->metadata) {
		ctx->apptag = user_io->apptag;
		ctx->appmask = user_io->appmask;
		ctx->metadata_len = md_size * ctx->lba_count;
		ctx->metadata = spdk_zmalloc(ctx->metadata_len, 4096, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

		if (ctx->metadata == NULL) {
			SPDK_ERRLOG("Cannot allocate memory for metadata\n");
			if (ctx->metadata_len == 0) {
				SPDK_ERRLOG("Device format does not support metadata\n");
			}
			fuse_reply_err(req, ENOMEM);
			cuse_io_ctx_free(ctx);
			return;
		}
	}

	rc = nvme_io_msg_send(cuse_device->ctrlr, cuse_device->nsid, cuse_nvme_submit_io_read_cb, ctx);
	/* [한국어] reactor에 read 위임. */
	if (rc < 0) {
		SPDK_ERRLOG("Cannot send read io\n");
		fuse_reply_err(ctx->req, rc);
		cuse_io_ctx_free(ctx);
	}
}


/*
 * [한국어]
 * cuse_nvme_submit_io - NVME_IOCTL_SUBMIT_IO ioctl 진입 핸들러.
 *
 * @req: fuse 요청. @cmd: NVME_IOCTL_SUBMIT_IO. @arg: 사용자 nvme_user_io 포인터.
 *
 * 실행 컨텍스트: CUSE thread.
 *
 * fuse 핸드셰이크와 read/write 분기:
 *   1) in_bufsz==0: nvme_user_io 헤더만 매핑 요청.
 *   2) opcode 검사 → READ: out_iov로 데이터 받을 영역 준비, out_bufsz==0이면 retry.
 *      WRITE: in_iov에 데이터/metadata 추가, in_bufsz==sizeof(user_io)면 retry.
 *   3) 모두 준비되면 cuse_nvme_submit_io_read/write로 분기.
 *
 * 호출 체인: cuse_ns_ioctl(SUBMIT_IO) → [이 함수] → cuse_nvme_submit_io_read/write
 */
static void
cuse_nvme_submit_io(fuse_req_t req, int cmd, void *arg,
		    struct fuse_file_info *fi, unsigned flags,
		    const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	const struct nvme_user_io *user_io;                     /* [한국어] 매핑된 사용자 페이로드 헤더. */
	struct iovec in_iov[3], out_iov[2];                     /* [한국어] in: 헤더+data+metadata, out: data+metadata. */
	int in_iovcnt = 0, out_iovcnt = 0;
	struct cuse_device *cuse_device = fuse_req_userdata(req);
	struct spdk_nvme_ns *ns;                                /* [한국어] block/md size 조회용. */
	uint32_t block_size;                                    /* [한국어] sector size (LBA size). */
	uint32_t md_size;                                       /* [한국어] separate metadata size per LBA. */

	in_iov[in_iovcnt].iov_base = (void *)arg;               /* [한국어] iov[0] = 사용자 nvme_user_io. */
	in_iov[in_iovcnt].iov_len = sizeof(*user_io);
	in_iovcnt += 1;
	if (in_bufsz == 0) {
		fuse_reply_ioctl_retry(req, in_iov, in_iovcnt, NULL, 0); /* [한국어] 1단계: 헤더 매핑 요청. */
		return;
	}

	user_io = in_buf;                                       /* [한국어] 매핑된 헤더. */

	ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid); /* [한국어] 이 device의 ns. */
	block_size = spdk_nvme_ns_get_sector_size(ns);          /* [한국어] LBA size (예: 512, 4096). */
	md_size = spdk_nvme_ns_get_md_size(ns);                 /* [한국어] separate metadata size per LBA. */

	switch (user_io->opcode) {
	case SPDK_NVME_OPC_READ:
		/* [한국어] READ: 디바이스 → 호스트. out_iov로 사용자 buffer 매핑 요청. */
		out_iov[out_iovcnt].iov_base = (void *)user_io->addr;
		out_iov[out_iovcnt].iov_len = (user_io->nblocks + 1) * block_size;
		out_iovcnt += 1;
		if (user_io->metadata != 0) {
			out_iov[out_iovcnt].iov_base = (void *)user_io->metadata;
			out_iov[out_iovcnt].iov_len = (user_io->nblocks + 1) * md_size;
			out_iovcnt += 1;
		}
		if (!fuse_check_req_size(req, out_iov, out_iovcnt)) {
			return;                                  /* [한국어] 한도 초과 — 이미 회신됨. */
		}
		if (out_bufsz == 0) {
			fuse_reply_ioctl_retry(req, in_iov, in_iovcnt, out_iov, out_iovcnt);
			/* [한국어] 2단계: out_buf 매핑 요청. */
			return;
		}

		cuse_nvme_submit_io_read(cuse_device, req, cmd, arg, fi, flags,
					 block_size, md_size, in_buf, in_bufsz, out_bufsz);
		/* [한국어] 3단계: 실제 read 처리. */
		break;
	case SPDK_NVME_OPC_WRITE:
		/* [한국어] WRITE: 호스트 → 디바이스. in_iov에 사용자 데이터 추가 매핑 요청. */
		in_iov[in_iovcnt].iov_base = (void *)user_io->addr;
		in_iov[in_iovcnt].iov_len = (user_io->nblocks + 1) * block_size;
		in_iovcnt += 1;
		if (user_io->metadata != 0) {
			in_iov[in_iovcnt].iov_base = (void *)user_io->metadata;
			in_iov[in_iovcnt].iov_len = (user_io->nblocks + 1) * md_size;
			in_iovcnt += 1;
		}
		if (!fuse_check_req_size(req, in_iov, in_iovcnt)) {
			return;
		}
		if (in_bufsz == sizeof(*user_io)) {
			/* [한국어] 헤더만 매핑된 상태이고 데이터는 아직 — 다시 retry로 데이터 매핑 요청. */
			fuse_reply_ioctl_retry(req, in_iov, in_iovcnt, NULL, out_iovcnt);
			return;
		}

		cuse_nvme_submit_io_write(cuse_device, req, cmd, arg, fi, flags,
					  block_size, md_size, in_buf, in_bufsz, out_bufsz);
		/* [한국어] 모든 입력 매핑 완료 — write 진행. */
		break;
	default:
		/* [한국어] SUBMIT_IO는 read/write 두 종류만 지원. compare/write_zeroes 등은 미지원. */
		SPDK_ERRLOG("SUBMIT_IO: opc:%d not valid\n", user_io->opcode);
		fuse_reply_err(req, EINVAL);
		return;
	}

}

/*****************************************************************************
 * Other namespace IOCTLs
 */
/* [한국어] 아래 섹션은 일반 블록 디바이스 ioctl(BLKGETSIZE/BLKSSZGET 등) — Linux fs 헬퍼.
 *         libblkid, lsblk 등이 사용. 모두 단순 read-only — reactor 위임 없이 즉시 회신. */

/*
 * [한국어]
 * cuse_blkgetsize64 - BLKGETSIZE64 ioctl 핸들러. 디바이스 크기를 sector 단위 uint64_t로 반환.
 *
 * 주의: 변수명 size이지만 실제로는 spdk_nvme_ns_get_num_sectors()의 반환(섹터 수)을 그대로 회신.
 * Linux 커널 BLKGETSIZE64는 본래 바이트 단위인데 이 코드는 sector count로 잘못 회신하는 부분이
 * 있음에 유의 (역사적 동작 보존).
 */
static void
cuse_blkgetsize64(fuse_req_t req, int cmd, void *arg,
		  struct fuse_file_info *fi, unsigned flags,
		  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	uint64_t size;                                          /* [한국어] 회신할 64비트 값. */
	struct spdk_nvme_ns *ns;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	FUSE_REPLY_CHECK_BUFFER(req, arg, out_bufsz, size);     /* [한국어] 1단계 retry 핸드셰이크 처리. */

	ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);
	size = spdk_nvme_ns_get_num_sectors(ns);                /* [한국어] NS 총 섹터 수. */
	fuse_reply_ioctl(req, 0, &size, sizeof(size));          /* [한국어] 사용자에게 회신. */
}

/*
 * [한국어]
 * cuse_blkpbszget - BLKPBSZGET ioctl 핸들러. physical block size (int) 반환.
 *
 * SPDK는 logical/physical 구분 없이 sector_size를 그대로 회신.
 */
static void
cuse_blkpbszget(fuse_req_t req, int cmd, void *arg,
		struct fuse_file_info *fi, unsigned flags,
		const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	int pbsz;                                                /* [한국어] physical block size. */
	struct spdk_nvme_ns *ns;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	FUSE_REPLY_CHECK_BUFFER(req, arg, out_bufsz, pbsz);

	ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);
	pbsz = spdk_nvme_ns_get_sector_size(ns);                 /* [한국어] LBA size를 physical로 회신. */
	fuse_reply_ioctl(req, 0, &pbsz, sizeof(pbsz));
}

/*
 * [한국어]
 * cuse_blkgetsize - BLKGETSIZE ioctl. 디바이스 크기를 512바이트 블록 단위 long으로 반환.
 *
 * 커널 ABI 호환을 위해 항상 512B 블록으로 환산. NS LBA size가 512가 아니면 비율 계산.
 */
static void
cuse_blkgetsize(fuse_req_t req, int cmd, void *arg,
		struct fuse_file_info *fi, unsigned flags,
		const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	long size;                                               /* [한국어] 32/64-bit long. */
	struct spdk_nvme_ns *ns;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	FUSE_REPLY_CHECK_BUFFER(req, arg, out_bufsz, size);

	ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);

	/* return size in 512 bytes blocks */
	/* [한국어] num_sectors * sector_size = 총 바이트 → / 512로 512B 블록 수 환산. */
	size = spdk_nvme_ns_get_num_sectors(ns) * 512 / spdk_nvme_ns_get_sector_size(ns);
	fuse_reply_ioctl(req, 0, &size, sizeof(size));
}

/*
 * [한국어]
 * cuse_blkgetsectorsize - BLKSSZGET ioctl. logical sector size (int) 반환.
 */
static void
cuse_blkgetsectorsize(fuse_req_t req, int cmd, void *arg,
		      struct fuse_file_info *fi, unsigned flags,
		      const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	int ssize;
	struct spdk_nvme_ns *ns;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	FUSE_REPLY_CHECK_BUFFER(req, arg, out_bufsz, ssize);

	ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);
	ssize = spdk_nvme_ns_get_sector_size(ns);                /* [한국어] LBA size. */
	fuse_reply_ioctl(req, 0, &ssize, sizeof(ssize));
}

/*
 * [한국어]
 * cuse_getid - NVME_IOCTL_ID 핸들러. NS device의 nsid를 result로 반환.
 *
 * Linux 커널 nvme driver의 NVME_IOCTL_ID는 nsid를 반환값(>=0)으로 회신하는 특수 ioctl.
 * fuse_reply_ioctl의 result 인자에 nsid를 직접 전달.
 */
static void
cuse_getid(fuse_req_t req, int cmd, void *arg,
	   struct fuse_file_info *fi, unsigned flags,
	   const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	fuse_reply_ioctl(req, cuse_device->nsid, NULL, 0);       /* [한국어] result=nsid, 페이로드 없음. */
}

/*
 * [한국어] struct cuse_transport — SPDK 고유 ioctl(SPDK_CUSE_GET_TRANSPORT) 응답 페이로드.
 *         사용자가 SPDK 디바이스의 transport(PCIe/RDMA/TCP) 타입과 주소(traddr)를 알고 싶을 때 사용.
 */
struct cuse_transport {
	char trstring[SPDK_NVMF_TRSTRING_MAX_LEN + 1];           /* [한국어] transport 문자열 ("PCIE", "RDMA", "TCP" 등). */
	char traddr[SPDK_NVMF_TRADDR_MAX_LEN + 1];               /* [한국어] transport 주소 (BDF 또는 IP). */
};

#define SPDK_CUSE_GET_TRANSPORT _IOWR('n', 0x1, struct cuse_transport)
/* [한국어] SPDK 고유 ioctl 정의. type='n' (nvme), nr=0x1, r/w 양방향, 페이로드 = cuse_transport. */

/*
 * [한국어]
 * cuse_get_transport - SPDK_CUSE_GET_TRANSPORT 핸들러. controller의 trid 정보 회신.
 */
static void
cuse_get_transport(fuse_req_t req, int cmd, void *arg,
		   struct fuse_file_info *fi, unsigned flags,
		   const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct cuse_device *cuse_device = fuse_req_userdata(req);
	struct cuse_transport tr = {};                            /* [한국어] 회신 페이로드. */

	FUSE_REPLY_CHECK_BUFFER(req, arg, out_bufsz, tr);         /* [한국어] retry 핸드셰이크. */

	memcpy(tr.trstring, cuse_device->ctrlr->trid.trstring, SPDK_NVMF_TRSTRING_MAX_LEN + 1);
	/* [한국어] ctrlr->trid는 controller의 transport ID — probe 시 채워짐. */
	memcpy(tr.traddr, cuse_device->ctrlr->trid.traddr, SPDK_NVMF_TRADDR_MAX_LEN + 1);

	fuse_reply_ioctl(req, 0, &tr, sizeof(tr));                /* [한국어] 사용자에게 회신. */
}

/*
 * [한국어]
 * cuse_ctrlr_ioctl - controller char device("/dev/spdk/nvmeX")의 ioctl 디스패처.
 *
 * 실행 컨텍스트: CUSE thread.
 *
 * cmd 분기:
 *   - NVME_IOCTL_ADMIN_CMD: admin passthru (Identify Controller, Get Log Page, Format 등).
 *   - NVME_IOCTL_RESET / SUBSYS_RESET: 리셋.
 *   - NVME_IOCTL_RESCAN: namespace 갱신.
 *   - NVME_IOCTL_ID: ENOTTY (controller에는 nsid 없음).
 *   - SPDK_CUSE_GET_TRANSPORT: SPDK 고유 transport 조회.
 *
 * FUSE_IOCTL_COMPAT (32-bit 호환 모드 ioctl)은 페이로드 정렬 차이로 미지원 — ENOSYS.
 *
 * 호출 체인: 사용자 ioctl() → 커널 CUSE → libfuse → cuse_lowlevel_ops.ioctl = [이 함수]
 */
static void
cuse_ctrlr_ioctl(fuse_req_t req, int cmd, void *arg,
		 struct fuse_file_info *fi, unsigned flags,
		 const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	if (flags & FUSE_IOCTL_COMPAT) {
		/* [한국어] 32-bit 사용자 프로세스가 64-bit 커널을 통해 보낸 ioctl — 페이로드 정렬 차이로
		 *         정확한 변환 어려움. 미지원으로 회신. */
		fuse_reply_err(req, ENOSYS);
		return;
	}

	switch ((unsigned int)cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		/* [한국어] admin passthru (가장 빈번 — Identify, Get Log, Set/Get Features 등). */
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_ADMIN_CMD\n");
		cuse_nvme_passthru_cmd(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_RESET:
	case NVME_IOCTL_SUBSYS_RESET:
		/* [한국어] 두 reset 모두 cuse_nvme_reset이 cmd로 분기 처리. */
		cuse_nvme_reset(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_RESCAN:
		/* [한국어] active NS 목록 재조회 + Identify 재발사. */
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_RESCAN\n");
		cuse_nvme_rescan(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_ID:
		/* Return error but don't ERRLOG - nvme-cli will frequently send this
		 * IOCTL to controller devices.
		 */
		/* [한국어] nvme-cli가 device가 namespace인지 확인하려 controller에도 자주 보냄.
		 *         로그 스팸 방지를 위해 ERRLOG 없이 ENOTTY만 회신. */
		fuse_reply_err(req, ENOTTY);
		break;

	case SPDK_CUSE_GET_TRANSPORT:
		/* [한국어] SPDK 전용 — trid 조회. */
		SPDK_DEBUGLOG(nvme_cuse, "SPDK_CUSE_GET_TRANSPORT\n");
		cuse_get_transport(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	default:
		/* [한국어] 미지원 ioctl — ENOTTY. */
		SPDK_ERRLOG("Unsupported IOCTL 0x%X.\n", cmd);
		fuse_reply_err(req, ENOTTY);
	}
}

/*
 * [한국어]
 * cuse_ns_ioctl - namespace char device("/dev/spdk/nvmeXnY")의 ioctl 디스패처.
 *
 * 실행 컨텍스트: CUSE thread.
 *
 * cmd 분기:
 *   - NVME_IOCTL_ADMIN_CMD / IO_CMD: passthru (NS 컨텍스트).
 *   - NVME_IOCTL_SUBMIT_IO: read/write 직접 제출.
 *   - NVME_IOCTL_ID: nsid 반환.
 *   - BLKPBSZGET / BLKSSZGET / BLKGETSIZE / BLKGETSIZE64: 일반 블록 디바이스 ioctl.
 */
static void
cuse_ns_ioctl(fuse_req_t req, int cmd, void *arg,
	      struct fuse_file_info *fi, unsigned flags,
	      const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	if (flags & FUSE_IOCTL_COMPAT) {
		/* [한국어] 32-bit 호환 모드 — 미지원. */
		fuse_reply_err(req, ENOSYS);
		return;
	}

	switch ((unsigned int)cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		/* [한국어] NS device에서도 admin 명령 가능 (예: Identify NS). */
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_ADMIN_CMD\n");
		cuse_nvme_passthru_cmd(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_SUBMIT_IO:
		/* [한국어] 사용자 친화적 read/write 제출 (passthru보다 단순). */
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_SUBMIT_IO\n");
		cuse_nvme_submit_io(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_IO_CMD:
		/* [한국어] IO opcode를 임의로 보내는 passthru (예: Dataset Management, Reservation). */
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_IO_CMD\n");
		cuse_nvme_passthru_cmd(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_ID:
		/* [한국어] nsid 반환 (nvme-cli가 device 식별에 사용). */
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_ID\n");
		cuse_getid(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKPBSZGET:
		/* [한국어] physical block size. */
		SPDK_DEBUGLOG(nvme_cuse, "BLKPBSZGET\n");
		cuse_blkpbszget(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKSSZGET:
		/* [한국어] logical sector size. */
		SPDK_DEBUGLOG(nvme_cuse, "BLKSSZGET\n");
		cuse_blkgetsectorsize(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKGETSIZE:
		SPDK_DEBUGLOG(nvme_cuse, "BLKGETSIZE\n");
		/* Returns the device size as a number of 512-byte blocks (returns pointer to long) */
		/* [한국어] 디바이스 크기 → 512B 블록 수 (long). */
		cuse_blkgetsize(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKGETSIZE64:
		SPDK_DEBUGLOG(nvme_cuse, "BLKGETSIZE64\n");
		/* Returns the device size in sectors (returns pointer to uint64_t) */
		/* [한국어] 디바이스 크기 (uint64_t). 본 코드는 sector 수로 회신 — 위 함수 주석 참고. */
		cuse_blkgetsize64(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	default:
		/* [한국어] 미지원 ioctl. */
		SPDK_ERRLOG("Unsupported IOCTL 0x%X.\n", cmd);
		fuse_reply_err(req, ENOTTY);
	}
}

/*****************************************************************************
 * CUSE threads initialization.
 */
/* [한국어] 아래 섹션: CUSE thread 생성/관리, fd_group 폴링 루프, 세션 생성/추가/제거. */

/*
 * [한국어]
 * cuse_open - char device open() 콜백. 별다른 권한 검증 없이 그대로 수락.
 *
 * fuse_file_info를 그대로 회신하면 사용자 open()이 성공함. 모든 권한은 device가 만들어지는
 * 시점의 OS 권한(0660 등)으로 결정.
 */
static void
cuse_open(fuse_req_t req, struct fuse_file_info *fi)
{
	fuse_reply_open(req, fi);                                /* [한국어] open 허용. fi의 fh 등을 그대로 회신. */
}

/*
 * [한국어] cuse_ctrlr_clop / cuse_ns_clop — libfuse에 등록할 cuse_lowlevel_ops vtable.
 *         open과 ioctl 두 콜백만 사용 (read/write/poll 등은 fuse가 기본 EOPNOTSUPP 응답).
 *         controller와 namespace는 동일한 open이지만 ioctl 디스패처가 다름.
 */
static const struct cuse_lowlevel_ops cuse_ctrlr_clop = {
	.open		= cuse_open,
	.ioctl		= cuse_ctrlr_ioctl,
};

static const struct cuse_lowlevel_ops cuse_ns_clop = {
	.open		= cuse_open,
	.ioctl		= cuse_ns_ioctl,
};

/*
 * [한국어]
 * cuse_session_create - 1개 cuse_device에 대해 fuse 세션 생성 + cuse_thread에 위임.
 *
 * @cuse_device: 세션을 만들 device (controller 또는 namespace).
 * @return: 0 성공, 음수 실패.
 *
 * 동작:
 *   1) cuse 인자 ("cuse -f", DEVNAME=<dev_name>) 구성.
 *   2) cuse_lowlevel_setup 호출 — libfuse가 /dev/cuse를 통해 커널에 char device 등록.
 *      성공 시 세션 핸들 + underlying fd 획득.
 *   3) g_pending_device_head에 device 추가하고 eventfd로 cuse_thread를 깨움.
 *      cuse_thread는 다음 wait에서 g_cuse_thread_msg_fd readable 감지 → cuse_thread_add_session
 *      핸들러가 pending → active로 옮기며 fuse_efd를 fd_group에 등록.
 *
 * 호출 체인: nvme_cuse_start / cuse_nvme_ns_start → [이 함수] → cuse_lowlevel_setup + eventfd_write
 */
static int
cuse_session_create(struct cuse_device *cuse_device)
{
	char *cuse_argv[] = { "cuse", "-f" };                    /* [한국어] cuse main args: -f = foreground. */
	int multithreaded;                                       /* [한국어] cuse_lowlevel_setup 출력 — 미사용. */
	int cuse_argc = SPDK_COUNTOF(cuse_argv);                 /* [한국어] argv 길이 = 2. */
	struct cuse_info ci;                                     /* [한국어] cuse 디바이스 메타정보. */
	char devname_arg[128 + 8];                               /* [한국어] "DEVNAME=spdk/nvme0" 등의 인자 버퍼. */
	const char *dev_info_argv[] = { devname_arg };           /* [한국어] dev_info에 들어갈 인자 배열. */

	snprintf(devname_arg, sizeof(devname_arg), "DEVNAME=%s", cuse_device->dev_name);
	/* [한국어] libfuse가 DEVNAME 인자를 보고 /dev/<DEVNAME>로 char device 생성. */

	memset(&ci, 0, sizeof(ci));                              /* [한국어] cuse_info 0-init. */
	ci.dev_info_argc = 1;                                    /* [한국어] dev_info 인자 1개. */
	ci.dev_info_argv = dev_info_argv;                        /* [한국어] DEVNAME 포함 배열. */
	ci.flags = CUSE_UNRESTRICTED_IOCTL;
	/* [한국어] CUSE_UNRESTRICTED_IOCTL: 어떤 ioctl 번호든 허용 (기본은 _IOR/_IOW만 허용).
	 *         NVMe ioctl 번호가 표준 매크로로 만들어지지 않은 경우도 있어 필요. */

	if (cuse_device->nsid) {
		/* [한국어] namespace device — ns 전용 ioctl 디스패처 사용. cuse_device를 userdata로 등록. */
		cuse_device->session = cuse_lowlevel_setup(cuse_argc, cuse_argv, &ci, &cuse_ns_clop,
				       &multithreaded, cuse_device);
	} else {
		/* [한국어] controller device — ctrlr 전용 디스패처 사용. */
		cuse_device->session = cuse_lowlevel_setup(cuse_argc, cuse_argv, &ci, &cuse_ctrlr_clop,
				       &multithreaded, cuse_device);
	}

	if (!cuse_device->session) {
		/* [한국어] 세션 생성 실패 — /dev/cuse 미존재, 권한 부족, libfuse 미설치 등. */
		SPDK_ERRLOG("Cannot create cuse session\n");
		return -1;
	}
	SPDK_NOTICELOG("fuse session for device %s created\n", cuse_device->dev_name);
	cuse_device->fuse_efd = fuse_session_fd(cuse_device->session);
	/* [한국어] 세션 underlying fd — cuse_thread가 epoll로 감시할 readable fd. */

	pthread_mutex_lock(&g_pending_device_mtx);
	TAILQ_INSERT_TAIL(&g_pending_device_head, cuse_device, cuse_thread_tailq);
	/* [한국어] cuse_thread가 처리할 대기 큐에 추가. */
	if (eventfd_write(g_cuse_thread_msg_fd, 1) != 0) {
		/* [한국어] eventfd write 실패 — 매우 드묾. 추가한 device를 다시 빼고 에러. */
		TAILQ_REMOVE(&g_pending_device_head, cuse_device, cuse_thread_tailq);
		pthread_mutex_unlock(&g_pending_device_mtx);
		SPDK_ERRLOG("eventfd_write failed: (%s).\n", spdk_strerror(errno));
		return -errno;
	}
	pthread_mutex_unlock(&g_pending_device_mtx);
	return 0;
}

/*
 * [한국어]
 * process_cuse_event - 단일 cuse 세션의 fuse 이벤트 1건 처리.
 *
 * spdk_fd_group이 fuse_efd readable 감지 시 호출하는 콜백. fuse_session_receive_buf로
 * 1건 받고 fuse_session_process_buf로 dispatch (→ cuse_*_ioctl 호출).
 *
 * @arg: fuse_session* (fd_group 등록 시 cuse_device->session으로 전달).
 *
 * 실행 컨텍스트: CUSE thread (fd_group_wait 안에서 호출됨).
 *
 * 호출 체인: cuse_thread → spdk_fd_group_wait → [이 콜백] → fuse_session_receive_buf + process_buf
 */
static int
process_cuse_event(void *arg)
{
	struct fuse_session *session = arg;                      /* [한국어] 세션 핸들 복원. */
	struct fuse_buf buf = { .mem = NULL };                   /* [한국어] receive_buf가 mem을 알아서 할당하도록 NULL 시작. */
	int rc = fuse_session_receive_buf(session, &buf);        /* [한국어] 커널에서 1건 read. */

	if (rc > 0) {
		/* [한국어] 받은 데이터 있음 → fuse 디스패처로 처리 (해당 cuse_*_ioctl 콜백 호출). */
		fuse_session_process_buf(session, &buf);
	}
	free(buf.mem);                                            /* [한국어] receive_buf가 할당한 버퍼 해제. */
	return 0;                                                 /* [한국어] fd_group 콜백 규약 — 0이면 정상. */
}

/*
 * [한국어]
 * cuse_thread_add_session - g_cuse_thread_msg_fd eventfd readable 시 호출되는 핸들러.
 *
 * pending 큐의 모든 cuse_device를 fd_group에 등록(fuse_efd → process_cuse_event 매핑) 후
 * active 큐로 이동.
 *
 * 실행 컨텍스트: CUSE thread.
 */
static int
cuse_thread_add_session(void *arg)
{
	struct cuse_device *cuse_device, *tmp;                    /* [한국어] 순회 + safe 제거를 위한 두 변수. */
	int ret;
	eventfd_t val;                                            /* [한국어] eventfd_read 결과 (사용 안 함). */

	eventfd_read(g_cuse_thread_msg_fd, &val);                 /* [한국어] eventfd 카운터 0으로 reset. */

	pthread_mutex_lock(&g_pending_device_mtx);
	TAILQ_FOREACH_SAFE(cuse_device, &g_pending_device_head, cuse_thread_tailq, tmp) {
		/* [한국어] pending의 모든 device를 fd_group에 등록. */
		ret = spdk_fd_group_add(g_device_fdgrp, cuse_device->fuse_efd, process_cuse_event,
					cuse_device->session, cuse_device->dev_name);
		/* [한국어] fuse_efd가 readable이면 process_cuse_event(session) 호출.
		 *         dev_name은 fd_group 디버깅용 라벨. */
		if (ret < 0) {
			SPDK_ERRLOG("Failed to add fd %d: (%s).\n", cuse_device->fuse_efd,
				    spdk_strerror(-ret));
			TAILQ_REMOVE(&g_pending_device_head, cuse_device, cuse_thread_tailq);
			free(cuse_device);                         /* [한국어] 등록 실패 → device 폐기. */
			assert(false);                             /* [한국어] dev 환경에서 중단. 프로덕션에선 NDEBUG로 무시. */
		}
	}
	TAILQ_CONCAT(&g_active_device_head, &g_pending_device_head, cuse_thread_tailq);
	/* [한국어] pending → active로 일괄 이동. concat은 두 큐를 한 번에 합쳐 pending을 비움. */
	pthread_mutex_unlock(&g_pending_device_mtx);
	return 0;
}

/*
 * [한국어]
 * cuse_thread - CUSE pthread의 메인 루프.
 *
 * 동작 (무한 polling):
 *   1) spdk_unaffinitize_thread — 이 thread는 SPDK reactor가 아니므로 코어 pinning 해제,
 *      OS scheduler가 어디든 배치 가능. 부하 적음.
 *   2) spdk_fd_group_wait(timeout=500ms): fuse fd 또는 eventfd readable 대기.
 *      readable이면 등록된 콜백(process_cuse_event 또는 cuse_thread_add_session) 자동 실행.
 *   3) active 큐 순회하며 fuse_session_exited(외부 SIGTERM 또는 force_exit) 감지 시 정리:
 *      - fd_group에서 fd 제거.
 *      - fuse_session_reset (재사용 가능 상태로) 또는 force_exit이면 teardown + free.
 *   4) active가 비고 pending도 없으면 종료. pending이 남아 있으면 retry (즉 새 device가 들어왔는데
 *      처리 못한 케이스).
 *   5) 종료 시 g_cuse_thread_msg_fd close + fd_group destroy + g_device_fdgrp NULL 표시.
 *
 * 실행 컨텍스트: 별도 pthread (start_cuse_thread에서 pthread_create).
 *
 * 호출 체인: pthread_create → [이 함수] → spdk_fd_group_wait → 등록된 콜백들
 */
static void *
cuse_thread(void *unused)
{
	struct cuse_device *cuse_device, *tmp;
	int timeout_msecs = 500;                                  /* [한국어] fd_group_wait 타임아웃 = 500ms. */
	bool retry;                                               /* [한국어] outer do-while 종료/재시작 플래그. */

	spdk_unaffinitize_thread();
	/* [한국어] CPU affinity 마스크 초기화 — SPDK env가 부모 thread에 설정한 코어 핀을 풀어줌.
	 *         CUSE thread는 polled-mode reactor가 아니므로 어떤 코어든 OK, 다른 reactor 코어를 점유하면 안 됨. */

	do {
		retry = false;                                    /* [한국어] 매 iteration 시작 시 false로 reset. */
		spdk_fd_group_wait(g_device_fdgrp, timeout_msecs);
		/* [한국어] epoll_wait — readable 이벤트가 오면 등록 콜백 자동 실행.
		 *         eventfd readable → cuse_thread_add_session, fuse_efd readable → process_cuse_event. */
		while (!TAILQ_EMPTY(&g_active_device_head)) {
			TAILQ_FOREACH_SAFE(cuse_device, &g_active_device_head, cuse_thread_tailq, tmp) {
				if (fuse_session_exited(cuse_device->session)) {
					/* [한국어] 세션 종료 감지 — 외부 SIGTERM 또는 SPDK가 force_exit 설정 후 fuse_session_exit. */
					spdk_fd_group_remove(g_device_fdgrp, cuse_device->fuse_efd);
					/* [한국어] fd_group에서 제거 — 더 이상 polling하지 않음. */
					fuse_session_reset(cuse_device->session);
					/* [한국어] 세션 reset — 종료 상태 초기화 (teardown 전에 필요). */
					TAILQ_REMOVE(&g_active_device_head, cuse_device, cuse_thread_tailq);
					if (cuse_device->force_exit) {
						/* [한국어] SPDK 내부 종료 요청이었음 — 세션 완전 해체 + free. */
						cuse_lowlevel_teardown(cuse_device->session);
						free(cuse_device);
					}
				}
			}
			/* Receive and process fuse event and new cuse device addition requests. */
			/* [한국어] active 큐 정리 후에도 새 fuse 이벤트나 새 device 추가가 올 수 있으므로 한 번 더 wait. */
			spdk_fd_group_wait(g_device_fdgrp, timeout_msecs);
		}
		pthread_mutex_lock(&g_cuse_mtx);
		if (!TAILQ_EMPTY(&g_pending_device_head)) {
			pthread_mutex_unlock(&g_cuse_mtx);
			/* Retry as we have some cuse devices pending to be polled on. */
			/* [한국어] active가 비었지만 pending이 남아있으면 (race) 다시 시도. */
			retry = true;
		}
	} while (retry);

	/* [한국어] 정리 단계: 새 device도 없고 active도 비었으므로 thread 종료. */
	spdk_fd_group_remove(g_device_fdgrp, g_cuse_thread_msg_fd); /* [한국어] eventfd 콜백 등록 해제. */
	close(g_cuse_thread_msg_fd);                                /* [한국어] eventfd close. */
	spdk_fd_group_destroy(g_device_fdgrp);                      /* [한국어] fd_group 자체 해제. */
	g_device_fdgrp = NULL;                                      /* [한국어] 다음 register 시 새로 만들어야 함을 표시. */
	pthread_mutex_unlock(&g_cuse_mtx);                          /* [한국어] outer do-while 직후 잡은 락 해제. */
	SPDK_NOTICELOG("Cuse thread exited.\n");
	return NULL;
}

static struct cuse_device *nvme_cuse_get_cuse_ns_device(struct spdk_nvme_ctrlr *ctrlr,
		uint32_t nsid);
/* [한국어] forward declaration — cuse_nvme_ns_start가 중복 등록 검사에 사용. */

/*****************************************************************************
 * CUSE devices management
 */
/* [한국어] 아래 섹션: cuse_device 구조체 자체의 lifecycle (start/stop, claim/unclaim, ns 추가/제거). */

/*
 * [한국어]
 * cuse_nvme_ns_start - controller 산하 namespace 1개에 대한 cuse_device 생성/세션 시작.
 *
 * @ctrlr_device: 부모 controller cuse_device.
 * @nsid: 네임스페이스 ID (1 이상).
 * @return: 0 성공, 음수 실패.
 *
 * 동작:
 *   1) 이미 등록된 ns이면 0 반환 (idempotent).
 *   2) 새 cuse_device 할당 + ctrlr/nsid/dev_name 설정.
 *   3) cuse_session_create로 char device 등록.
 *   4) 부모의 ns_devices 리스트에 삽입.
 *
 * 호출 체인: cuse_nvme_ctrlr_update_namespaces → [이 함수] → cuse_session_create
 */
static int
cuse_nvme_ns_start(struct cuse_device *ctrlr_device, uint32_t nsid)
{
	struct cuse_device *ns_device = NULL;
	int rv;

	ns_device = nvme_cuse_get_cuse_ns_device(ctrlr_device->ctrlr, nsid);
	/* [한국어] 이미 등록된 ns 검사 — rescan/update 시 중복 방지. */
	if (ns_device != NULL) {
		return 0;
	}

	ns_device = calloc(1, sizeof(struct cuse_device));        /* [한국어] 새 cuse_device 0-init 할당. */
	if (ns_device == NULL) {
		return -ENOMEM;
	}

	ns_device->ctrlr = ctrlr_device->ctrlr;                   /* [한국어] 부모 controller 공유. */
	ns_device->ctrlr_device = ctrlr_device;                   /* [한국어] 부모 cuse_device 역참조. */
	ns_device->nsid = nsid;                                   /* [한국어] 네임스페이스 ID 저장. */
	rv = snprintf(ns_device->dev_name, sizeof(ns_device->dev_name), "%sn%d",
		      ctrlr_device->dev_name, ns_device->nsid);
	/* [한국어] "spdk/nvme0n1" 형태 device name 생성. */
	if (rv < 0) {
		SPDK_ERRLOG("Device name too long.\n");
		rv = -ENAMETOOLONG;
		goto free_device;
	}

	rv = cuse_session_create(ns_device);                       /* [한국어] CUSE 세션 + thread 위임. */
	if (rv != 0) {
		goto free_device;
	}

	TAILQ_INSERT_TAIL(&ctrlr_device->ns_devices, ns_device, tailq);
	/* [한국어] 부모 controller의 ns 리스트에 등록. */

	return 0;

free_device:
	free(ns_device);
	return rv;
}

/*
 * [한국어]
 * cuse_nvme_ns_stop - namespace cuse_device 세션 종료 + 리스트에서 제거.
 *
 * 실제 free는 cuse_thread가 force_exit 감지 후 수행 (race 방지를 위해 thread가 단독 책임).
 *
 * 호출 체인: cuse_nvme_ctrlr_stop / cuse_nvme_ctrlr_update_namespaces → [이 함수]
 */
static void
cuse_nvme_ns_stop(struct cuse_device *ctrlr_device, struct cuse_device *ns_device)
{
	TAILQ_REMOVE(&ctrlr_device->ns_devices, ns_device, tailq); /* [한국어] 부모 ns 리스트에서 제거. */
	/* ns_device will be freed by cuse_thread */
	if (ns_device->session != NULL) {
		ns_device->force_exit = true;                       /* [한국어] cuse_thread에 free 요청. */
		fuse_session_exit(ns_device->session);              /* [한국어] 세션 종료 신호 — 다음 wait에서 exited 감지. */
	}
}

/*
 * [한국어]
 * nvme_cuse_claim - controller 인덱스에 대한 cross-process lock 획득.
 *
 * @ctrlr_device: claim 대상. @index: 후보 인덱스 (lock 파일 이름에 사용).
 * @return: 0 성공, 음수 실패 (-EACCES면 다른 프로세스가 점유 중).
 *
 * 동작:
 *   1) "/var/tmp/spdk_nvme_cuse_lock_<index>" 파일 open + truncate.
 *   2) mmap으로 4바이트(int) 매핑.
 *   3) fcntl(F_SETLK, F_WRLCK) 시도 — 다른 프로세스가 락 잡고 있으면 실패.
 *   4) 성공 시 파일에 자기 PID 기록 (디버깅: 누가 잡았는지 확인용).
 *   5) dev_fd는 close하지 않고 cuse_device->claim_fd로 보존 — 프로세스 종료 또는 unclaim까지 락 유지.
 *
 * 호출 체인: nvme_cuse_start → [이 함수] (성공할 때까지 index++로 재시도)
 */
static int
nvme_cuse_claim(struct cuse_device *ctrlr_device, uint32_t index)
{
	int dev_fd;                                                  /* [한국어] 락 파일 fd. */
	int pid;                                                     /* [한국어] 충돌 시 점유 중인 PID 진단. */
	void *dev_map;                                               /* [한국어] mmap 결과. */
	struct flock cusedev_lock = {
		.l_type = F_WRLCK,                                   /* [한국어] write lock — 단독 점유. */
		.l_whence = SEEK_SET,                                /* [한국어] offset 기준 = 파일 시작. */
		.l_start = 0,                                        /* [한국어] 파일 전체 락. */
		.l_len = 0,                                          /* [한국어] 0 = EOF까지 (전체). */
	};

	snprintf(ctrlr_device->lock_name, sizeof(ctrlr_device->lock_name),
		 "/var/tmp/spdk_nvme_cuse_lock_%" PRIu32, index);
	/* [한국어] 락 파일 경로 생성. /var/tmp는 재부팅 후에도 유지되므로 stale 락이 남을 수 있는데
	 *         fcntl 락은 프로세스 종료 시 자동 해제되므로 stale 파일은 무시 가능. */

	dev_fd = open(ctrlr_device->lock_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	/* [한국어] 0600 권한으로 생성. 다른 사용자/그룹은 접근 불가. */
	if (dev_fd == -1) {
		SPDK_ERRLOG("could not open %s\n", ctrlr_device->lock_name);
		return -errno;
	}

	if (ftruncate(dev_fd, sizeof(int)) != 0) {
		/* [한국어] 파일 크기를 4바이트로 — PID 저장용. */
		SPDK_ERRLOG("could not truncate %s\n", ctrlr_device->lock_name);
		close(dev_fd);
		return -errno;
	}

	dev_map = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
		       MAP_SHARED, dev_fd, 0);
	/* [한국어] PID를 저장/조회할 4바이트 mmap. SHARED여야 다른 프로세스도 PID 읽기 가능. */
	if (dev_map == MAP_FAILED) {
		SPDK_ERRLOG("could not mmap dev %s (%d)\n", ctrlr_device->lock_name, errno);
		close(dev_fd);
		return -errno;
	}

	if (fcntl(dev_fd, F_SETLK, &cusedev_lock) != 0) {
		/* [한국어] non-blocking lock 시도 — 다른 프로세스가 잡고 있으면 즉시 실패.
		 *         실패 시 파일에 적힌 PID로 누가 점유 중인지 진단 로그 출력. */
		pid = *(int *)dev_map;
		SPDK_ERRLOG("Cannot create lock on device %s, probably"
			    " process %d has claimed it\n", ctrlr_device->lock_name, pid);
		munmap(dev_map, sizeof(int));
		close(dev_fd);
		/* F_SETLK returns unspecified errnos, normalize them */
		/* [한국어] EACCES 또는 EAGAIN이 올 수 있어 EACCES로 통일. */
		return -EACCES;
	}

	*(int *)dev_map = (int)getpid();                              /* [한국어] 자기 PID 기록. */
	munmap(dev_map, sizeof(int));                                 /* [한국어] mmap 해제 (락은 fd가 유지). */
	ctrlr_device->claim_fd = dev_fd;                              /* [한국어] 락 유지를 위해 fd 보존. */
	ctrlr_device->index = index;                                  /* [한국어] 할당된 인덱스 기록. */
	/* Keep dev_fd open to maintain the lock. */
	/* [한국어] fd close하면 fcntl 락 자동 해제되므로 절대 닫지 않음. */
	return 0;
}

/*
 * [한국어]
 * nvme_cuse_unclaim - claim 해제: fd close + 락 파일 unlink.
 *
 * close로 fcntl 락 자동 해제, unlink로 디스크에서 락 파일 제거.
 */
static void
nvme_cuse_unclaim(struct cuse_device *ctrlr_device)
{
	close(ctrlr_device->claim_fd);                                /* [한국어] fd close → fcntl 락 자동 해제. */
	ctrlr_device->claim_fd = -1;                                  /* [한국어] sentinel 값. */
	unlink(ctrlr_device->lock_name);                              /* [한국어] 락 파일 디스크에서 제거. */
}

/*
 * [한국어]
 * cuse_nvme_ctrlr_stop - controller cuse_device 종료 + 모든 산하 namespace 정리.
 *
 * 동작:
 *   1) 모든 ns cuse_device를 cuse_nvme_ns_stop으로 종료 신호 전달.
 *   2) bit_array에서 인덱스 회수, 마지막이면 bit_array 자체 free.
 *   3) lock 파일 unlink.
 *   4) g_ctrlr_ctx_head에서 제거.
 *   5) controller 세션 force_exit + exit 신호 — cuse_thread가 받아서 free.
 *
 * 호출 체인: spdk_nvme_cuse_unregister / nvme_cuse_stop → [이 함수]
 */
static void
cuse_nvme_ctrlr_stop(struct cuse_device *ctrlr_device)
{
	struct cuse_device *ns_device, *tmp;

	TAILQ_FOREACH_SAFE(ns_device, &ctrlr_device->ns_devices, tailq, tmp) {
		/* [한국어] 모든 ns에 종료 신호. cuse_thread가 비동기로 free. */
		cuse_nvme_ns_stop(ctrlr_device, ns_device);
	}

	assert(TAILQ_EMPTY(&ctrlr_device->ns_devices));                /* [한국어] 모두 빠졌는지 sanity check. */

	spdk_bit_array_clear(g_ctrlr_started, ctrlr_device->index);    /* [한국어] 인덱스 슬롯 회수. */
	if (spdk_bit_array_count_set(g_ctrlr_started) == 0) {
		/* [한국어] 더 이상 등록된 controller 없음 — bit_array도 free. */
		spdk_bit_array_free(&g_ctrlr_started);
	}
	nvme_cuse_unclaim(ctrlr_device);                                /* [한국어] 락 파일 해제. */

	TAILQ_REMOVE(&g_ctrlr_ctx_head, ctrlr_device, tailq);           /* [한국어] 전역 리스트에서 제거. */
	/* ctrlr_device will be freed by cuse_thread */
	ctrlr_device->force_exit = true;                                /* [한국어] cuse_thread에 free 요청. */
	fuse_session_exit(ctrlr_device->session);                       /* [한국어] 세션 종료 신호. */
}

/*
 * [한국어]
 * cuse_nvme_ctrlr_update_namespaces - controller의 active NS 리스트와 cuse ns_devices를 동기화.
 *
 * - 사라진 NS는 cuse_nvme_ns_stop으로 제거.
 * - 새로 나타난 NS는 cuse_nvme_ns_start로 추가.
 *
 * controller의 NS 목록이 변할 때(NSSR 후, namespace management 후 등) 호출.
 *
 * 호출 체인: nvme_cuse_start (초기) / nvme_cuse_update (스펙 변경) → [이 함수]
 */
static int
cuse_nvme_ctrlr_update_namespaces(struct cuse_device *ctrlr_device)
{
	struct cuse_device *ns_device, *tmp;
	uint32_t nsid;

	/* Remove namespaces that have disappeared */
	TAILQ_FOREACH_SAFE(ns_device, &ctrlr_device->ns_devices, tailq, tmp) {
		if (!spdk_nvme_ctrlr_is_active_ns(ctrlr_device->ctrlr, ns_device->nsid)) {
			/* [한국어] controller가 더 이상 active로 보지 않는 NS는 제거. */
			cuse_nvme_ns_stop(ctrlr_device, ns_device);
		}
	}

	/* Add new namespaces */
	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr_device->ctrlr);
	while (nsid != 0) {
		if (cuse_nvme_ns_start(ctrlr_device, nsid) < 0) {
			/* [한국어] 신규 ns 시작 실패는 치명적 — 이미 시작된 ns는 그대로 두고 에러. */
			SPDK_ERRLOG("Cannot start CUSE namespace device.");
			return -1;
		}

		nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr_device->ctrlr, nsid);
	}

	return 0;
}

#ifdef FUSE_LOG_H_
/*
 * [한국어]
 * nvme_fuse_log_func - libfuse의 기본 로그 핸들러를 대체하는 사용자 정의 핸들러.
 *
 * libfuse가 세션 teardown 시 "fuse_remove_signal_handlers: unknown session" 로그를
 * 두 번째 세션부터 매번 출력하는 노이즈를 막기 위함. 그 외 메시지는 stderr에 그대로 출력.
 *
 * fuse_log.h가 있는 libfuse 3.x에서만 등록됨 (이전 버전은 핸들러 교체 API 미존재).
 */
static void
nvme_fuse_log_func(enum fuse_log_level level, const char *fmt, va_list ap)
{
	/* fuse will unnecessarily print this log message when tearing down
	 * sessions, once for every session after the first. So use this custom
	 * log handler to silence that specific log message.
	 */
	if (strstr(fmt, "fuse_remove_signal_handlers: unknown session") != NULL) {
		/* [한국어] 알려진 노이즈 메시지 — 무시. */
		return;
	}

	vfprintf(stderr, fmt, ap);                                  /* [한국어] 그 외는 그대로 출력. */
}
#endif

/*
 * [한국어]
 * nvme_cuse_start - controller 1개를 CUSE에 등록 (cuse_device 생성 + 모든 ns 시작).
 *
 * @ctrlr: 등록할 NVMe controller.
 * @return: 0 성공, 음수 실패.
 *
 * 동작:
 *   1) g_ctrlr_started 비트 배열이 없으면 첫 등록이므로 생성 (128 슬롯).
 *      libfuse 로그 핸들러도 첫 등록 시 등록.
 *   2) cuse_device 할당 + 빈 인덱스 검색 + claim 시도 (충돌 시 다음 인덱스 재시도).
 *   3) dev_name = "spdk/nvme<index>" 생성 + cuse_session_create.
 *   4) g_ctrlr_ctx_head에 추가 + ns_devices 초기화.
 *   5) 모든 active ns에 대해 cuse_nvme_ns_start.
 *
 * 호출 체인: spdk_nvme_cuse_register → [이 함수]
 */
static int
nvme_cuse_start(struct spdk_nvme_ctrlr *ctrlr)
{
	int rv = 0;
	struct cuse_device *ctrlr_device;

	SPDK_NOTICELOG("Creating cuse device for controller\n");

	if (g_ctrlr_started == NULL) {
		g_ctrlr_started = spdk_bit_array_create(128);
		/* [한국어] 첫 등록 — bit_array 생성. 128개 controller까지 지원. */
		if (g_ctrlr_started == NULL) {
			SPDK_ERRLOG("Cannot create bit array\n");
			return -ENOMEM;
		}
#ifdef FUSE_LOG_H_
		/* Older versions of libfuse don't have fuse_set_log_func nor
		 * fuse_log.h, so this is the easiest way to check for it
		 * without adding a separate CONFIG flag.
		 */
		/* [한국어] libfuse 3.x — 사용자 정의 로그 핸들러 등록으로 노이즈 억제. */
		fuse_set_log_func(nvme_fuse_log_func);
#endif
	}

	ctrlr_device = (struct cuse_device *)calloc(1, sizeof(struct cuse_device));
	if (!ctrlr_device) {
		SPDK_ERRLOG("Cannot allocate memory for ctrlr_device.");
		rv = -ENOMEM;
		goto free_device;
	}

	ctrlr_device->ctrlr = ctrlr;                                  /* [한국어] controller 보존. */

	/* Check if device already exists, if not increment index until success */
	ctrlr_device->index = 0;
	while (1) {
		ctrlr_device->index = spdk_bit_array_find_first_clear(g_ctrlr_started, ctrlr_device->index);
		/* [한국어] 비어있는 첫 인덱스 검색. UINT32_MAX 반환이면 가득참. */
		if (ctrlr_device->index == UINT32_MAX) {
			SPDK_ERRLOG("Too many registered controllers\n");
			goto free_device;
		}

		if (nvme_cuse_claim(ctrlr_device, ctrlr_device->index) == 0) {
			/* [한국어] cross-process lock 성공 — 이 인덱스 사용 가능. */
			break;
		}
		ctrlr_device->index++;                                /* [한국어] 다른 프로세스가 점유 중 — 다음 인덱스 시도. */
	}
	spdk_bit_array_set(g_ctrlr_started, ctrlr_device->index);     /* [한국어] 인덱스 사용 표시. */
	snprintf(ctrlr_device->dev_name, sizeof(ctrlr_device->dev_name), "spdk/nvme%d",
		 ctrlr_device->index);
	/* [한국어] dev_name = "spdk/nvme0", "spdk/nvme1" ... 형태. */

	rv = cuse_session_create(ctrlr_device);                       /* [한국어] CUSE 세션 + thread 위임. */
	if (rv != 0) {
		goto clear_and_free;
	}

	TAILQ_INSERT_TAIL(&g_ctrlr_ctx_head, ctrlr_device, tailq);    /* [한국어] 전역 리스트에 추가. */

	TAILQ_INIT(&ctrlr_device->ns_devices);                        /* [한국어] ns 리스트 초기화. */

	/* Start all active namespaces */
	if (cuse_nvme_ctrlr_update_namespaces(ctrlr_device) < 0) {
		/* [한국어] ns 시작 중 실패 — controller 자체를 stop하여 일관성 유지. */
		SPDK_ERRLOG("Cannot start CUSE namespace devices.");
		cuse_nvme_ctrlr_stop(ctrlr_device);
		return -1;
	}

	return 0;

clear_and_free:
	spdk_bit_array_clear(g_ctrlr_started, ctrlr_device->index);   /* [한국어] 인덱스 회수. */
free_device:
	free(ctrlr_device);
	if (spdk_bit_array_count_set(g_ctrlr_started) == 0) {
		/* [한국어] 등록된 controller 0개 — bit_array도 해제. */
		spdk_bit_array_free(&g_ctrlr_started);
	}
	return rv;
}

/*
 * [한국어]
 * nvme_cuse_get_cuse_ctrlr_device - ctrlr → cuse_device 역참조.
 *
 * g_ctrlr_ctx_head를 선형 검색. controller 수가 적어 OK (보통 < 128).
 * 호출자가 g_cuse_mtx를 잡고 있어야 함.
 */
static struct cuse_device *
nvme_cuse_get_cuse_ctrlr_device(struct spdk_nvme_ctrlr *ctrlr)
{
	struct cuse_device *ctrlr_device = NULL;

	TAILQ_FOREACH(ctrlr_device, &g_ctrlr_ctx_head, tailq) {
		if (ctrlr_device->ctrlr == ctrlr) {
			break;                                          /* [한국어] 일치 발견 — 즉시 종료. */
		}
	}

	return ctrlr_device;                                            /* [한국어] 미발견 시 NULL. */
}

/*
 * [한국어]
 * nvme_cuse_get_cuse_ns_device - (ctrlr, nsid) → ns cuse_device 역참조.
 *
 * 먼저 controller cuse_device를 찾고, 그 산하 ns_devices를 선형 검색.
 */
static struct cuse_device *
nvme_cuse_get_cuse_ns_device(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct cuse_device *ctrlr_device = NULL;
	struct cuse_device *ns_device;

	ctrlr_device = nvme_cuse_get_cuse_ctrlr_device(ctrlr);          /* [한국어] 부모 ctrlr device 검색. */
	if (!ctrlr_device) {
		return NULL;                                            /* [한국어] controller 미등록. */
	}

	TAILQ_FOREACH(ns_device, &ctrlr_device->ns_devices, tailq) {
		if (ns_device->nsid == nsid) {
			return ns_device;                               /* [한국어] nsid 일치 — 반환. */
		}
	}

	return NULL;                                                    /* [한국어] 해당 nsid 미등록. */
}

/*
 * [한국어]
 * nvme_cuse_stop - io_msg_producer.stop 콜백. controller 해제 시 자동 호출.
 *
 * cuse_nvme_ctrlr_stop을 g_cuse_mtx 보호 하에 호출. primary process에서만 실행.
 *
 * 호출 체인: nvme_io_msg_ctrlr_unregister 경로에서 producer.stop으로 호출됨.
 */
static void
nvme_cuse_stop(struct spdk_nvme_ctrlr *ctrlr)
{
	struct cuse_device *ctrlr_device;

	assert(spdk_process_is_primary());                              /* [한국어] secondary 프로세스 호출 금지. */

	pthread_mutex_lock(&g_cuse_mtx);

	ctrlr_device = nvme_cuse_get_cuse_ctrlr_device(ctrlr);
	if (!ctrlr_device) {
		SPDK_ERRLOG("Cannot find associated CUSE device\n");
		pthread_mutex_unlock(&g_cuse_mtx);
		return;
	}

	cuse_nvme_ctrlr_stop(ctrlr_device);                              /* [한국어] 종료 절차 위임. */

	pthread_mutex_unlock(&g_cuse_mtx);
}

/*
 * [한국어]
 * nvme_cuse_update - io_msg_producer.update 콜백. controller 상태 변경(NS 추가/삭제) 시 호출.
 *
 * cuse_nvme_ctrlr_update_namespaces를 g_cuse_mtx 보호 하에 호출. ctrlr_device 미등록 시 무시.
 *
 * 호출 체인: nvme_io_msg.c가 ctrlr->needs_io_msg_update 감지 시 producer.update 콜백으로 호출.
 */
static void
nvme_cuse_update(struct spdk_nvme_ctrlr *ctrlr)
{
	struct cuse_device *ctrlr_device;

	assert(spdk_process_is_primary());

	pthread_mutex_lock(&g_cuse_mtx);

	ctrlr_device = nvme_cuse_get_cuse_ctrlr_device(ctrlr);
	if (!ctrlr_device) {
		pthread_mutex_unlock(&g_cuse_mtx);                       /* [한국어] 미등록 controller — silent return. */
		return;
	}

	cuse_nvme_ctrlr_update_namespaces(ctrlr_device);                 /* [한국어] NS 동기화. */

	pthread_mutex_unlock(&g_cuse_mtx);
}

/*
 * [한국어] cuse_nvme_io_msg_producer — nvme_io_msg.c에 등록할 producer vtable.
 *         name: 식별자, stop: 컨트롤러 정리 통지, update: NS 변경 통지.
 *         spdk_nvme_cuse_register가 nvme_io_msg_ctrlr_register로 등록한다.
 */
static struct nvme_io_msg_producer cuse_nvme_io_msg_producer = {
	.name = "cuse",
	.stop = nvme_cuse_stop,
	.update = nvme_cuse_update,
};

/*
 * [한국어]
 * start_cuse_thread - CUSE polling thread 1회 생성 + fd_group/eventfd 초기화.
 *
 * SPDK 프로세스 전체에서 1번만 호출됨 (g_device_fdgrp == NULL일 때).
 *
 * 동작:
 *   1) spdk_fd_group_create — epoll fd group 생성.
 *   2) eventfd 생성 (NONBLOCK | CLOEXEC) — pending 추가 통지용.
 *   3) eventfd를 fd_group에 등록 (콜백 = cuse_thread_add_session).
 *   4) pthread_create로 cuse_thread 시작 + detach + 이름 "cuse_thread" 설정.
 *
 * 모든 단계에 실패 경로(rollback)가 정의되어 있음.
 *
 * 호출 체인: spdk_nvme_cuse_register → [이 함수] → pthread_create
 */
static int
start_cuse_thread(void)
{
	int rc = 0;
	pthread_t tid;

	rc = spdk_fd_group_create(&g_device_fdgrp);                      /* [한국어] epoll group 생성. */
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create fd group: (%s).\n", spdk_strerror(-rc));
		return rc;
	}

	g_cuse_thread_msg_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	/* [한국어] 통지용 eventfd. NONBLOCK = read 시 즉시 리턴(없으면 EAGAIN), CLOEXEC = exec 시 자동 close. */
	if (g_cuse_thread_msg_fd < 0) {
		SPDK_ERRLOG("Failed to create eventfd: (%s).\n", spdk_strerror(errno));
		rc = -errno;
		goto destroy_fd_group;
	}

	rc = SPDK_FD_GROUP_ADD(g_device_fdgrp, g_cuse_thread_msg_fd,
			       cuse_thread_add_session, NULL);
	/* [한국어] eventfd가 readable이면 cuse_thread_add_session 콜백 자동 실행. arg=NULL. */
	if (rc < 0) {
		SPDK_ERRLOG("Failed to add fd %d: %s.\n", g_cuse_thread_msg_fd,
			    spdk_strerror(-rc));
		goto close_and_destroy_fd;
	}

	rc = pthread_create(&tid, NULL, cuse_thread, NULL);              /* [한국어] CUSE thread 시작. */
	if (rc != 0) {
		SPDK_ERRLOG("pthread_create failed\n");
		rc = -rc;                                                /* [한국어] pthread_create는 errno-style 양수 반환. */
		goto remove_close_and_destroy_fd;
	}
	pthread_detach(tid);                                              /* [한국어] join하지 않을 thread — 자원 자동 회수. */
	pthread_setname_np(tid, "cuse_thread");                           /* [한국어] top/htop에서 식별 용이. */
	SPDK_NOTICELOG("Successfully started cuse thread to poll for admin commands\n");
	return rc;

	/* [한국어] 아래는 단계별 rollback 라벨. 늦게 실패할수록 더 많은 리소스를 정리해야 함. */
remove_close_and_destroy_fd:
	spdk_fd_group_remove(g_device_fdgrp, g_cuse_thread_msg_fd);
close_and_destroy_fd:
	close(g_cuse_thread_msg_fd);
destroy_fd_group:
	spdk_fd_group_destroy(g_device_fdgrp);
	g_device_fdgrp = NULL;
	return rc;
}

/*
 * [한국어]
 * spdk_nvme_cuse_register - PUBLIC API. controller 1개를 CUSE에 등록.
 *
 * @ctrlr: 등록할 NVMe controller (이미 attach된 상태).
 * @return: 0 성공, 음수 errno.
 *
 * 동작:
 *   1) primary process 검사 — secondary는 불가 (같은 dev path 충돌).
 *   2) nvme_io_msg_ctrlr_register — cuse를 producer로 등록 (실패 시 즉시 리턴).
 *   3) g_cuse_mtx 잡고: cuse thread가 없으면 start_cuse_thread, 그 다음 nvme_cuse_start.
 *      실패 시 io_msg producer 등록도 롤백.
 *
 * 사용자 진입점 — 보통 RPC 핸들러 또는 controller attach 콜백 직후 호출.
 *
 * 호출 체인: 사용자 (RPC 등) → [이 함수] → start_cuse_thread + nvme_cuse_start
 */
int
spdk_nvme_cuse_register(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	if (!spdk_process_is_primary()) {
		/* [한국어] multi-process 환경에서 secondary는 cuse 등록 불가
		 *         (char device 이름 충돌, claim 락 등 이유). */
		SPDK_ERRLOG("only allowed from primary process\n");
		return -EINVAL;
	}

	rc = nvme_io_msg_ctrlr_register(ctrlr, &cuse_nvme_io_msg_producer);
	/* [한국어] cuse를 io_msg producer로 등록 — 이로써 nvme_io_msg_send가 가능해짐.
	 *         첫 producer면 nvme_io_msg.c가 ring + external_io_msgs_qpair도 만든다. */
	if (rc) {
		return rc;
	}

	pthread_mutex_lock(&g_cuse_mtx);

	if (g_device_fdgrp == NULL) {
		/* [한국어] cuse thread가 아직 없으면 (첫 register) thread 시작. */
		rc = start_cuse_thread();
		if (rc < 0) {
			SPDK_ERRLOG("Failed to start cuse thread to poll for admin commands\n");
			pthread_mutex_unlock(&g_cuse_mtx);
			return rc;
		}
	}

	rc = nvme_cuse_start(ctrlr);                                      /* [한국어] 실제 cuse_device 생성. */
	if (rc) {
		nvme_io_msg_ctrlr_unregister(ctrlr, &cuse_nvme_io_msg_producer);
		/* [한국어] cuse_device 생성 실패 — io_msg producer도 rollback. */
	}

	pthread_mutex_unlock(&g_cuse_mtx);

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_cuse_unregister - PUBLIC API. controller 1개를 CUSE에서 해제.
 *
 * @ctrlr: 해제할 controller.
 * @return: 0 성공, 음수 (-ENODEV: 등록 안 됨, -EINVAL: secondary).
 *
 * 동작:
 *   1) primary 검사.
 *   2) g_cuse_mtx 잡고 ctrlr_device lookup → cuse_nvme_ctrlr_stop으로 종료 절차.
 *   3) nvme_io_msg_ctrlr_unregister로 producer 해제.
 *
 * cuse thread 자체는 등록된 controller가 0개여도 자동 종료되지 않으며,
 * cuse_thread 내부 do-while이 active+pending이 모두 빌 때 자연 종료한다.
 */
int
spdk_nvme_cuse_unregister(struct spdk_nvme_ctrlr *ctrlr)
{
	struct cuse_device *ctrlr_device;

	if (!spdk_process_is_primary()) {
		SPDK_ERRLOG("only allowed from primary process\n");
		return -EINVAL;
	}

	pthread_mutex_lock(&g_cuse_mtx);

	ctrlr_device = nvme_cuse_get_cuse_ctrlr_device(ctrlr);
	if (!ctrlr_device) {
		SPDK_ERRLOG("Cannot find associated CUSE device\n");
		pthread_mutex_unlock(&g_cuse_mtx);
		return -ENODEV;
	}

	cuse_nvme_ctrlr_stop(ctrlr_device);                                /* [한국어] 컨트롤러 + ns 일괄 종료. */

	pthread_mutex_unlock(&g_cuse_mtx);

	nvme_io_msg_ctrlr_unregister(ctrlr, &cuse_nvme_io_msg_producer);   /* [한국어] producer 등록 해제. */

	return 0;
}

/*
 * [한국어]
 * spdk_nvme_cuse_update_namespaces - PUBLIC API. controller의 NS 목록을 cuse와 동기화.
 *
 * 사용자가 namespace management로 NS를 추가/삭제했을 때 호출. nvme_cuse_update 래퍼.
 */
void
spdk_nvme_cuse_update_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_cuse_update(ctrlr);                                           /* [한국어] 락 + update 위임. */
}

/*
 * [한국어]
 * spdk_nvme_cuse_get_ctrlr_name - PUBLIC API. controller의 cuse dev_name을 사용자 버퍼에 복사.
 *
 * @ctrlr: 등록된 controller. @name: 사용자 출력 버퍼. @size: in/out — 호출 시 버퍼 크기,
 *         반환 시 실제 필요 크기 (성공/실패 모두).
 * @return: 0 성공, -ENODEV: 미등록, -ENOSPC: 버퍼 부족 (*size에 필요 길이 기록).
 *
 * 사용 예: bdev_nvme_cuse RPC가 등록 후 client에 device path를 보고할 때.
 */
int
spdk_nvme_cuse_get_ctrlr_name(struct spdk_nvme_ctrlr *ctrlr, char *name, size_t *size)
{
	struct cuse_device *ctrlr_device;
	size_t req_len;                                                    /* [한국어] 필요한 buffer 길이 (NUL 제외). */

	pthread_mutex_lock(&g_cuse_mtx);

	ctrlr_device = nvme_cuse_get_cuse_ctrlr_device(ctrlr);
	if (!ctrlr_device) {
		pthread_mutex_unlock(&g_cuse_mtx);
		return -ENODEV;
	}

	req_len = strnlen(ctrlr_device->dev_name, sizeof(ctrlr_device->dev_name));
	/* [한국어] dev_name의 실제 길이 (배열 한도 내에서). */
	if (*size < req_len) {
		*size = req_len;                                            /* [한국어] 사용자에게 필요 크기 알림. */
		pthread_mutex_unlock(&g_cuse_mtx);
		return -ENOSPC;
	}
	snprintf(name, req_len + 1, "%s", ctrlr_device->dev_name);          /* [한국어] +1은 NUL terminator. */

	pthread_mutex_unlock(&g_cuse_mtx);

	return 0;
}

/*
 * [한국어]
 * spdk_nvme_cuse_get_ns_name - PUBLIC API. namespace의 cuse dev_name을 사용자 버퍼에 복사.
 *
 * spdk_nvme_cuse_get_ctrlr_name과 동일한 패턴. (ctrlr, nsid) → "spdk/nvme0n1" 등 회신.
 */
int
spdk_nvme_cuse_get_ns_name(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, char *name, size_t *size)
{
	struct cuse_device *ns_device;
	size_t req_len;

	pthread_mutex_lock(&g_cuse_mtx);

	ns_device = nvme_cuse_get_cuse_ns_device(ctrlr, nsid);
	if (!ns_device) {
		pthread_mutex_unlock(&g_cuse_mtx);
		return -ENODEV;
	}

	req_len = strnlen(ns_device->dev_name, sizeof(ns_device->dev_name));
	if (*size < req_len) {
		*size = req_len;
		pthread_mutex_unlock(&g_cuse_mtx);
		return -ENOSPC;
	}
	snprintf(name, req_len + 1, "%s", ns_device->dev_name);

	pthread_mutex_unlock(&g_cuse_mtx);

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(nvme_cuse)
/* [한국어] SPDK 로그 컴포넌트 "nvme_cuse" 등록 — SPDK_DEBUGLOG(nvme_cuse, ...) 매크로가
 *         RPC log_set_flag로 활성화될 때만 출력되게 함. ioctl 디버깅 시 유용. */
