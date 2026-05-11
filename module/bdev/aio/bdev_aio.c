/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK AIO bdev 모듈 구현 (bdev_aio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK bdev 추상화 위에서 "리눅스 커널 비동기 I/O(libaio) 또는 FreeBSD POSIX AIO"를
 * 백엔드로 사용하는 bdev 모듈을 구현한다. 즉, 일반 파일이나 블록 디바이스(/dev/sdX, 루프백
 * 파일 등)를 SPDK bdev로 노출시켜 SPDK 스택(NVMe-oF target, vhost, 테스트 등)이 마치
 * 폴드 모드 NVMe처럼 다룰 수 있도록 해준다. SPDK 본연의 polled-mode 유저스페이스 NVMe
 * 드라이버를 사용할 수 없는 환경(가상 디스크, 일반 파일, 비-NVMe HW)에서 매우 유용하다.
 * 핵심 동작은 (1) open(O_DIRECT)으로 fd를 잡고, (2) io_submit()으로 매 요청을 커널에 제출,
 * (3) reactor의 SPDK poller가 mmap된 AIO ring buffer를 직접 읽어 io_getevents 시스템 호출을
 * 우회(고속화)하면서 완료를 회수, (4) 완료된 spdk_bdev_io를 SUCCESS/FAILED로 보고하는 것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택에서의 위치는 다음과 같다.
 *   [Application / NVMe-oF / vhost target]
 *      ↓ spdk_bdev_read/write
 *   [bdev core (lib/bdev/)]
 *      ↓ fn_table->submit_request → bdev_aio_submit_request
 *   [bdev_aio (이 파일)]
 *      ↓ io_submit() (Linux libaio)  또는  aio_readv()/aio_writev() (FreeBSD)
 *   [Linux Kernel AIO 또는 FreeBSD AIO]
 *      ↓ block layer / VFS
 *   [블록 디바이스 또는 파일시스템 위 파일]
 *
 * 완료 경로:
 *   [Kernel completes I/O] → AIO completion ring (커널이 mmap에 저장) →
 *   reactor poller(bdev_aio_group_poll) → bdev_user_io_getevents (ring 직접 읽기) →
 *   spdk_bdev_io_complete
 *
 * 모듈은 두 단계의 io_device를 가진다. 하나는 module 글로벌(`aio_if` 주소)인 group channel —
 * 이 그룹 채널이 reactor당 1개의 poller를 갖고, 그 reactor에 묶인 모든 AIO 디스크의 채널을
 * 폴링한다. 다른 하나는 각 디스크별 io_device(`fdisk`) — 디스크별로 io_context_t 하나씩
 * 분리되어 있어 디스크끼리 영향이 없다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/bdev/ : spdk_bdev_module / spdk_bdev / spdk_bdev_fn_table / spdk_bdev_io / 등록·완료
 *   API. SPDK_BDEV_MODULE_REGISTER로 자기 자신을 등록.
 * - lib/thread/ : spdk_io_device_register/spdk_get_io_channel/spdk_for_each_channel/
 *   SPDK_POLLER_REGISTER/SPDK_INTERRUPT_REGISTER. 코어 로컬 채널, group 채널, poller, 그리고
 *   eventfd 기반 인터럽트 모드 처리에 사용.
 * - lib/env_dpdk/ : spdk_env이 제공하는 메모리·환경 추상화는 직접 사용하지 않으나, hugepage가
 *   필요한 다른 모듈과 같은 메모리 풀에서 io_u 버퍼를 받게 됨(spdk_bdev_io_get_buf 통해).
 * - lib/util/ : spdk_fd_get_size/spdk_fd_get_blocklen(파일 디스크의 크기/블록 크기 자동 감지),
 *   spdk_u32_is_pow2/spdk_u32log2(블록 크기 검증), spdk_min(ring 폴링 길이 제한).
 * - 시스템 라이브러리: libaio (`io_setup/io_submit/io_destroy/io_prep_*` — Linux),
 *   POSIX AIO (`aio_readv/aio_writev/aio_return` — FreeBSD), `eventfd(2)`(인터럽트 모드 통보),
 *   `fallocate(2)`(UNMAP=PUNCH_HOLE / WRITE_ZEROES=ZERO_RANGE), `fsync(2)`(FLUSH).
 * - module/bdev/aio/bdev_aio_rpc.c : RPC 핸들러가 별도 파일에 있고, 본 파일의
 *   create_aio_bdev/bdev_aio_delete/bdev_aio_rescan을 호출.
 *
 * 데이터 흐름: spdk_bdev_io → bdev_aio_submit_request → spdk_bdev_io_get_buf(정렬된 버퍼 확보)
 * → bdev_aio_get_buf_cb → bdev_aio_rw → bdev_aio_submit_io → io_submit() → 커널 → AIO ring
 * → bdev_aio_io_channel_poll → spdk_bdev_io_complete.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct file_disk : AIO bdev 인스턴스(파일/디바이스 1개당 1개). 파일 핸들/옵션/예약 작업 보유.
 * - struct bdev_aio_io_channel : 코어 로컬 채널. io_context_t (Linux) 또는 kqfd (FreeBSD) 보유.
 * - struct bdev_aio_group_channel : reactor당 1개의 모듈 그룹 채널. 그 reactor에 묶인 io 채널을
 *   리스트로 관리하고 단일 poller/interrupt로 폴링.
 * - struct bdev_aio_task : bdev_io당 컨텍스트. iocb/aiocb를 임베드.
 * - struct spdk_aio_ring : Linux libaio가 내부에서 사용하는 mmap된 완료 링버퍼 헤더 모사.
 * - bdev_aio_submit_request / _bdev_aio_submit_request : I/O 진입점.
 * - bdev_aio_io_channel_poll / bdev_aio_group_poll : 완료 폴러.
 * - create_aio_bdev / bdev_aio_delete / bdev_aio_rescan : RPC 백엔드.
 */

#include "bdev_aio.h"
/* [한국어] 같은 디렉토리의 공개 헤더. create_aio_bdev/delete/rescan 함수 프로토타입과
 * delete_aio_bdev_complete 콜백 타입을 노출. RPC 핸들러(bdev_aio_rpc.c)가 사용. */

#include "spdk/stdinc.h"
/* [한국어] 표준 헤더 모음 — stdio/stdlib/string/errno/unistd/fcntl 등 OS 표준 헤더 일괄 포함. */

#include "spdk/barrier.h"
/* [한국어] 메모리 배리어 매크로(spdk_smp_rmb/spdk_smp_mb/spdk_compiler_barrier).
 * 사용자 공간에서 mmap된 AIO ring을 안전하게 읽기 위해 head/tail 인덱스 사이의 순서 보장에 사용. */

#include "spdk/bdev.h"
/* [한국어] bdev 공개 API. spdk_bdev_open_ext, spdk_bdev_close, spdk_bdev_desc_get_bdev,
 * spdk_bdev_notify_blockcnt_change 등 rescan 동작에 사용. */

#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈 작성 API. spdk_bdev_module 등록, spdk_bdev_io_complete,
 * spdk_bdev_io_complete_aio_status, spdk_bdev_io_get_buf, spdk_bdev_register 등 사용. */

#include "spdk/env.h"
/* [한국어] DPDK 환경 추상화. 본 파일에서 직접 hugepage alloc은 안 하지만 다른 헤더 의존성으로 포함. */

#include "spdk/fd.h"
/* [한국어] 파일 디스크립터 유틸. spdk_fd_get_size, spdk_fd_get_blocklen — 파일 또는 블록
 * 디바이스에서 크기와 논리 블록 크기를 stat/ioctl을 통해 자동 감지. */

#include "spdk/likely.h"
/* [한국어] 분기 예측 매크로 spdk_likely/spdk_unlikely. I/O 핫패스의 에러 분기에 unlikely 적용. */

#include "spdk/thread.h"
/* [한국어] spdk_thread/io_device/poller/interrupt API. 코어 로컬 채널과 polling/interrupt
 * 등록 및 spdk_for_each_channel(채널 순회) 사용. */

#include "spdk/json.h"
/* [한국어] JSON 직렬화 — dump_info_json, write_config_json 콜백 출력에 사용. */

#include "spdk/util.h"
/* [한국어] SPDK_CONTAINEROF, spdk_min, spdk_u32_is_pow2, spdk_u32log2 등 비트/포인터 유틸. */

#include "spdk/string.h"
/* [한국어] spdk_strerror — errno → 메시지 변환. 로그 출력에 사용. */

#include "spdk/log.h"
/* [한국어] 로그 매크로(SPDK_ERRLOG/SPDK_NOTICELOG/SPDK_DEBUGLOG) 및
 * SPDK_LOG_REGISTER_COMPONENT. */

#include <sys/eventfd.h>
/* [한국어] Linux eventfd(2) 시스템 호출 인터페이스. 인터럽트 모드일 때 커널 AIO 완료를
 * SPDK reactor에 통보하는 채널로 사용 — io_set_eventfd로 iocb에 efd를 묶어두면 커널이
 * 완료 시 efd를 깨운다. */

#ifndef __FreeBSD__
#include <libaio.h>
/* [한국어] Linux 한정: 커널 AIO를 직접 호출하기 위한 사용자 라이브러리(io_setup/submit/
 * destroy/getevents/io_prep_preadv/io_prep_pwritev/io_set_eventfd 등). FreeBSD 빌드에서는
 * POSIX AIO(aio_readv 등)를 사용하므로 미포함. */
#endif

/* [한국어] AIO_FDISK_LOG_* 매크로 묶음
 * 한 줄 로그에 "[bdev이름,fdisk포인터,파일이름] 메시지" 형태의 공통 헤더를 자동으로 붙이는
 * 가독성 매크로. 디버깅 시 어떤 디스크에서 어떤 일이 일어났는지 한 줄로 식별 가능. */
#define AIO_FDISK_LOG_FMT "%s,fdisk:%p,filename:%s"
/* [한국어] 공통 prefix 포맷 문자열. */
#define AIO_FDISK_LOG_ARGS(fdisk) \
  (fdisk)->disk.name, \
  (fdisk), \
  (fdisk)->filename
/* [한국어] 위 prefix에 매핑되는 인자 3종 — bdev 이름, fdisk 포인터, 백엔드 파일 경로. */

#define AIO_FDISK_LOG(type, fdisk, format, ...) do { \
	SPDK_##type##LOG("["AIO_FDISK_LOG_FMT"] " format, AIO_FDISK_LOG_ARGS(fdisk), ##__VA_ARGS__); \
} while (0)
/* [한국어] 공용 로그 매크로. type=ERR/NOTICE/WARN 등 SPDK 로그 레벨에 prefix를 자동 추가. */

#define AIO_FDISK_LOG2(type, component, fdisk, format, ...) do { \
	SPDK_##type##LOG(component, "["AIO_FDISK_LOG_FMT"] " format, AIO_FDISK_LOG_ARGS(fdisk), ##__VA_ARGS__); \
} while (0)
/* [한국어] 컴포넌트 필터링이 가능한 로그 매크로(SPDK_DEBUGLOG/SPDK_INFOLOG는 컴포넌트 인자 필요). */

#define AIO_FDISK_ERRLOG(fdisk, format, ...) AIO_FDISK_LOG(ERR, fdisk, format, ##__VA_ARGS__)
/* [한국어] 에러 로그 헬퍼. */
#define AIO_FDISK_WARNLOG(fdisk, format, ...) AIO_FDISK_LOG(WARN, fdisk, format, ##__VA_ARGS__)
/* [한국어] 경고 로그 헬퍼. */
#define AIO_FDISK_NOTICELOG(fdisk, format, ...) AIO_FDISK_LOG(NOTICE, fdisk, format, ##__VA_ARGS__)
/* [한국어] notice 로그 헬퍼. */
#define AIO_FDISK_INFOLOG(fdisk, format, ...) AIO_FDISK_LOG2(INFO, aio, fdisk, format, ##__VA_ARGS__)
/* [한국어] info 로그 헬퍼. "aio" 컴포넌트로 필터링 가능. */

#ifdef DEBUG
#define AIO_FDISK_DEBUGLOG(fdisk, format, ...) AIO_FDISK_LOG2(DEBUG, aio, fdisk, format, ##__VA_ARGS__)
/* [한국어] DEBUG 빌드에서만 활성. read/write 진입을 추적할 때 사용. */
#else
#define AIO_FDISK_DEBUGLOG(...) do { } while (0)
/* [한국어] 릴리스 빌드에서는 no-op으로 컴파일러가 모든 호출을 제거. */
#endif

/*
 * [한국어] struct bdev_aio_io_channel
 *
 * AIO bdev의 코어 로컬 I/O 채널. 디스크별 io_device(fdisk)에 대해 reactor 스레드가 처음
 * spdk_get_io_channel을 호출하면 코어가 sizeof(bdev_aio_io_channel) 메모리를 할당해 이
 * 구조체로 채운다. 핵심은 io_context_t를 디스크/채널마다 분리해 reactor 간 간섭을 없애는 것.
 */
struct bdev_aio_io_channel {
	uint64_t				io_inflight;
	/* [한국어] 이 채널에 제출된 후 아직 완료되지 않은 I/O 개수.
	 * 설정자: bdev_aio_rw가 io_submit 성공 시 +1, completion 폴러가 -1.
	 * 읽는 자: _bdev_aio_get_io_inflight (RESET 처리에서 in-flight=0 대기 판정).
	 * 값 범위: 0 이상.
	 * 동기화: 채널 소유 reactor 안에서만 접근하므로 락 없이 안전. */
#ifdef __FreeBSD__
	int					kqfd;
	/* [한국어] FreeBSD: kqueue(2) fd. POSIX AIO 완료가 sigevent를 통해 이 kqueue로 알려진다.
	 * 설정자: bdev_aio_create_io의 kqueue() 호출.
	 * 읽는 자: bdev_user_io_getevents의 kevent(2) 폴링.
	 * 동기화: 채널 로컬. */
#else
	io_context_t				io_ctx;
	/* [한국어] Linux libaio 컨텍스트 핸들. io_setup이 만들고, 사용자 공간으로 mmap된
	 * 완료 ring 헤드를 가리킨다. 이 ctx 단위로 io_submit/io_destroy를 호출.
	 * 설정자: bdev_aio_create_io의 io_setup(SPDK_AIO_QUEUE_DEPTH, &io_ctx).
	 * 읽는 자: io_submit, bdev_user_io_getevents (ring 직접 읽기).
	 * 값 범위: 비-NULL. 채널당 하나.
	 * 동기화: 한 채널 = 한 reactor가 사용하므로 락 불필요. */
#endif
	struct bdev_aio_group_channel		*group_ch;
	/* [한국어] 이 채널이 속한 reactor의 그룹 채널 포인터. 그룹 poller가 그룹의 모든 io
	 * 채널을 한 번에 폴링하므로 양방향 연결이 필요.
	 * 설정자: bdev_aio_create_cb에서 spdk_get_io_channel(&aio_if)로 얻음.
	 * 읽는 자: bdev_aio_submit_io에서 io_set_eventfd(인터럽트 모드)에 group_ch->efd 사용. */
	TAILQ_ENTRY(bdev_aio_io_channel)	link;
	/* [한국어] group_ch->io_ch_head에 자신을 연결하는 노드.
	 * 설정자/읽는 자: create_cb의 INSERT_TAIL, destroy_cb의 REMOVE, group_poll의 FOREACH. */
};

/*
 * [한국어] struct bdev_aio_group_channel
 *
 * "AIO 모듈 자체"를 io_device로 등록한 결과로 reactor마다 한 번 만들어지는 그룹 채널.
 * 같은 reactor에 묶인 모든 file_disk의 채널을 io_ch_head에 모아두고, 단일 poller(또는 인터럽트
 * 핸들러)로 한꺼번에 완료를 회수한다. 이렇게 하면 디스크가 N개여도 reactor당 poller는 1개.
 */
struct bdev_aio_group_channel {
	/* eventfd for io completion notification in interrupt mode.
	 * Negative value like '-1' indicates it is invalid or unused.
	 */
	int					efd;
	/* [한국어] 인터럽트 모드일 때 사용. eventfd(2)로 만들고 각 iocb에 io_set_eventfd로 묶어
	 * 두면 커널이 AIO 완료마다 efd를 1만큼 증가시켜 깨운다. polled 모드에서는 -1.
	 * 설정자: bdev_aio_register_interrupt(eventfd 생성), bdev_aio_unregister_interrupt(-1).
	 * 읽는 자: bdev_aio_submit_io의 io_set_eventfd, bdev_aio_group_interrupt의 read(efd).
	 * 값 범위: 유효한 fd 또는 -1.
	 * 동기화: 채널 로컬. */
	struct spdk_interrupt			*intr;
	/* [한국어] eventfd에 등록된 interrupt 핸들. SPDK_INTERRUPT_REGISTER 반환값.
	 * spdk_interrupt가 깨어나면 bdev_aio_group_interrupt가 호출됨. */
	struct spdk_poller			*poller;
	/* [한국어] 폴드 모드의 polling 콜백 핸들. SPDK_POLLER_REGISTER 결과.
	 * 설정자: bdev_aio_group_create_cb. 읽는 자: bdev_aio_group_destroy_cb의 unregister. */
	TAILQ_HEAD(, bdev_aio_io_channel)	io_ch_head;
	/* [한국어] 이 reactor에 속한 모든 AIO 디스크 io 채널 리스트. group_poll이 순회.
	 * 설정자: io_channel create/destroy_cb. 동기화: reactor 로컬. */
};

/*
 * [한국어] struct bdev_aio_task
 *
 * spdk_bdev_io 1개당 모듈 컨텍스트(driver_ctx). bdev core가 spdk_bdev_io를 alloc할 때
 * sizeof(bdev_aio_task) 만큼 추가 할당해 driver_ctx에 매핑. iocb/aiocb를 임베드해두면
 * io_submit 호출 시 별도 할당이 필요 없어 핫패스가 빠르다.
 */
struct bdev_aio_task {
#ifdef __FreeBSD__
	struct aiocb			aiocb;
	/* [한국어] FreeBSD POSIX AIO 작업 구조체. aio_readv/aio_writev에 전달. */
#else
	struct iocb			iocb;
	/* [한국어] Linux libaio I/O Control Block. io_prep_preadv/io_prep_pwritev로 채우고
	 * io_submit으로 커널에 제출. 완료 시 io_event.data가 이 iocb.data(=aio_task) 포인터로 회복. */
#endif
	uint64_t			len;
	/* [한국어] 요청 바이트 수. 완료 시 io_event.res와 비교해 부분 완료를 감지(res < len이면 실패). */
	struct bdev_aio_io_channel	*ch;
	/* [한국어] 이 task를 제출한 채널. 완료 시 ch->io_inflight를 감소시키기 위해 보관. */
};

/*
 * [한국어] struct file_disk
 *
 * AIO bdev 인스턴스(파일/블록 디바이스 1개당 1개). spdk_bdev를 임베드하고, 백엔드 파일/디바이스의
 * fd, 옵션 플래그, RESET 보류 상태 등을 보유한다. fdisk_from_bdev로 spdk_bdev → file_disk 역산.
 */
struct file_disk {
	struct bdev_aio_task	*reset_task;
	/* [한국어] RESET I/O가 진행 중일 때 그 spdk_bdev_io의 driver_ctx 포인터를 보관.
	 * RESET은 in-flight I/O가 모두 끝나야 완료할 수 있어 retry 타이머 동안 보관됨.
	 * 설정자: bdev_aio_reset. 읽는 자: _bdev_aio_get_io_inflight_done의 spdk_bdev_io_complete. */
	struct spdk_poller	*reset_retry_timer;
	/* [한국어] RESET 보류 시 500ms마다 in-flight=0 검사를 다시 수행하는 retry 타이머.
	 * 설정자: _bdev_aio_get_io_inflight_done(미해소 시 등록).
	 * 읽는 자: bdev_aio_reset_retry_timer가 자기 자신을 unregister 후 재시도. */
	struct spdk_bdev	disk;
	/* [한국어] bdev core가 인식하는 공개 메타데이터(name, blocklen, blockcnt, uuid, fn_table…).
	 * 설정자: create_aio_bdev. 읽는 자: bdev core, 사용자. 동기화: 등록 후 read-only가 일반적. */
	char			*filename;
	/* [한국어] 백엔드 파일/디바이스 경로(strdup 사본). 디버그 로그/JSON 직렬화에 사용. */
	int			fd;
	/* [한국어] open()이 반환한 파일 디스크립터. -1이면 닫혀 있음.
	 * 설정자: bdev_aio_open(open). 읽는 자: io_submit(O_DIRECT), fsync, fallocate. */
	bool			use_nowait;
	/* [한국어] true면 io_prep_*에 RWF_NOWAIT 플래그를 추가 → 큐 포화 시 I/O를 즉시 -EAGAIN으로
	 * 반환받아 backpressure 가능. 블록 디바이스에서만 의미가 있음. */
	TAILQ_ENTRY(file_disk)  link;
	/* [한국어] g_aio_disk_head 글로벌 리스트 노드. */
	bool			block_size_override;
	/* [한국어] 사용자가 RPC에서 block_size를 명시했는지(true) 자동 감지인지(false). config_json에 반영. */
	bool			readonly;
	/* [한국어] true면 O_RDONLY로 open, write 요청을 거절. */
	bool			fallocate;
	/* [한국어] true면 UNMAP/WRITE_ZEROES를 fallocate(2)로 매핑(파일 시스템 지원 필수).
	 * Linux 전용 옵션. FreeBSD에서는 강제로 false. */

	bool			hot_remove_in_progress;
	/* [한국어] hot-remove(디바이스 분리) 처리가 이미 시작됐는지를 표시하는 플래그.
	 * __atomic_test_and_set으로 race-free하게 검사 — 동시에 여러 reactor에서 ENODEV를
	 * 보고할 수 있기 때문. */
};

/* For user space reaping of completions */
/*
 * [한국어] struct spdk_aio_ring
 *
 * Linux libaio가 io_setup() 시점에 사용자 공간으로 mmap해 두는 완료 링버퍼의 헤더 레이아웃을
 * SPDK가 직접 모사한 구조체. io_getevents(2) 시스템 호출을 우회하고 mmap된 영역의 head/tail을
 * 직접 읽어 완료 이벤트를 회수함으로써 syscall 비용을 0으로 만든다. version 필드가
 * SPDK_AIO_RING_VERSION 매직과 다르거나 incompat_features가 0이 아니면 fallback으로 io_getevents를 호출.
 */
struct spdk_aio_ring {
	uint32_t id;
	/* [한국어] 링버퍼 ID(libaio 내부용). SPDK는 사용하지 않음. */
	uint32_t size;
	/* [한국어] 링의 슬롯 개수. count = (tail - head) mod size로 완료 개수 산출. */
	uint32_t head;
	/* [한국어] 사용자가 다음에 소비할 위치(SPDK가 갱신). */
	uint32_t tail;
	/* [한국어] 커널이 다음에 채울 위치(커널이 갱신). */

	uint32_t version;
	/* [한국어] 호환성 매직 — SPDK_AIO_RING_VERSION과 일치해야 ring을 직접 읽음. */
	uint32_t compat_features;
	/* [한국어] 호환 가능 기능 비트맵(SPDK는 검사하지 않음). */
	uint32_t incompat_features;
	/* [한국어] 비호환 기능 비트맵 — 0이 아니면 ring 레이아웃이 다를 수 있어 fallback. */
	uint32_t header_length;
	/* [한국어] 헤더 + padding 크기. 이벤트 배열은 ring 시작 + header_length 위치부터. */
};

#define SPDK_AIO_RING_VERSION	0xa10a10a1
/* [한국어] libaio 사용자 공간 ring의 알려진 매직 값. 커널 버전에 따라 다른 레이아웃이면 다른 값. */

static int bdev_aio_initialize(void);
/* [한국어] 전방 선언: 모듈 init 콜백. */
static void bdev_aio_fini(void);
/* [한국어] 전방 선언: 모듈 fini 콜백. */
static void aio_free_disk(struct file_disk *fdisk);
/* [한국어] 전방 선언: file_disk 메모리 해제 헬퍼. */
static TAILQ_HEAD(, file_disk) g_aio_disk_head = TAILQ_HEAD_INITIALIZER(g_aio_disk_head);
/* [한국어] 모듈 글로벌: 등록된 모든 AIO bdev 인스턴스의 리스트.
 * 단일 RPC 처리 스레드에서만 갱신되므로 락 불필요. */

#define SPDK_AIO_QUEUE_DEPTH 128
/* [한국어] io_setup에 전달할 큐 깊이. 채널당 동시에 in-flight 가능한 최대 I/O 수.
 * 너무 크면 fs.aio-max-nr 시스템 한계를 넘어설 수 있음. */
#define MAX_EVENTS_PER_POLL 32
/* [한국어] (현재 코드에선 미사용 — 과거 호환을 위해 남아있음.) 한 번의 poll에서 회수할 최대 이벤트 수. */

/*
 * [한국어]
 * bdev_aio_get_ctx_size - bdev_io당 driver_ctx 크기 반환 (모듈 등록 콜백)
 *
 * @return: sizeof(struct bdev_aio_task).
 *
 * bdev core가 모든 모듈의 ctx_size 중 최대치를 spdk_bdev_io_pool 객체 크기에 합산해 단일 풀에서
 * 할당하므로, 본 함수가 반환하는 크기만큼이 driver_ctx로 사용 가능.
 */
static int
bdev_aio_get_ctx_size(void)
{
	return sizeof(struct bdev_aio_task);
	/* [한국어] iocb/aiocb + len + ch 포인터를 담을 만큼 필요. */
}

/*
 * [한국어]
 * fdisk_from_bdev - spdk_bdev → file_disk 역포인터 변환 헬퍼
 *
 * @bdev: file_disk::disk 멤버를 가리키는 포인터.
 * @return: 그 spdk_bdev를 임베드하고 있는 file_disk 포인터.
 *
 * SPDK_CONTAINEROF(=offsetof 기반)로 임베드 부모를 회복. fn_table 콜백이 spdk_bdev/ctx만
 * 받기 때문에 자주 호출됨.
 */
static struct file_disk *
fdisk_from_bdev(struct spdk_bdev *bdev)
{
	return SPDK_CONTAINEROF(bdev, struct file_disk, disk);
	/* [한국어] bdev 포인터가 file_disk::disk의 주소이므로 offsetof만큼 빼면 file_disk 시작점. */
}

/*
 * [한국어] aio_if (정적 할당)
 * AIO 모듈을 bdev core에 등록하기 위한 spdk_bdev_module 인스턴스.
 */
static struct spdk_bdev_module aio_if = {
	.name		= "aio",
	/* [한국어] 모듈 식별자. RPC delete/rescan에서 모듈 일치 검사에 사용. */
	.module_init	= bdev_aio_initialize,
	/* [한국어] 부팅 시 호출. group_channel io_device 등록만 수행. */
	.module_fini	= bdev_aio_fini,
	/* [한국어] 종료 시 호출. group_channel io_device 해제. async_fini 미설정 → 동기 종료. */
	.get_ctx_size	= bdev_aio_get_ctx_size,
	/* [한국어] driver_ctx 크기 콜백. */
};

SPDK_BDEV_MODULE_REGISTER(aio, &aio_if)
/* [한국어] 컴파일 타임에 &aio_if를 module list에 등록. SPDK 부팅 시 자동 init. */

/*
 * [한국어]
 * bdev_aio_close - 백엔드 파일/디바이스 fd를 close (open 짝)
 *
 * @disk: 대상 file_disk.
 * @return: 0 성공(또는 이미 닫힘), -1 close 실패.
 *
 * 안전하게 두 번 호출 가능(idempotent) — disk->fd가 -1이면 즉시 0 반환. 닫은 후 fd를 -1로
 * 마킹해 이후 호출이 중복 close를 시도하지 않도록 한다.
 *
 * 호출 컨텍스트: RPC 스레드(create 실패 경로) 또는 destruct 콜백 스레드.
 */
static int
bdev_aio_close(struct file_disk *disk)
{
	int rc;
	/* [한국어] close(2) 반환값 임시 저장. */

	if (disk->fd == -1) {
		/* [한국어] 이미 닫힘 → 그냥 성공 처리(중복 호출 방지). */
		return 0;
	}

	rc = close(disk->fd);
	/* [한국어] OS에 fd 반납. block device close는 커널 측 in-flight I/O 정리도 동반. */
	if (rc < 0) {
		/* [한국어] close 실패는 매우 드묾(EBADF 등). 로그만 남기고 -1 반환. */
		SPDK_ERRLOG("close() failed (fd=%d), rc %d: %s\n", disk->fd, rc, spdk_strerror(errno));
		return -1;
	}

	disk->fd = -1;
	/* [한국어] 닫힘 표시. 이후 bdev_aio_close가 다시 호출되어도 0 반환. */

	return 0;
}

/*
 * [한국어]
 * bdev_aio_open - 백엔드 파일/디바이스를 O_DIRECT로 open
 *
 * @disk: 대상 file_disk. filename/readonly/fd가 채워져 있어야 함.
 * @nowait: true면 RWF_NOWAIT 사용을 시도(블록 디바이스에서만 가능).
 * @return: 0 성공, -1 실패(errno 설정).
 *
 * 단계:
 *   1) O_DIRECT|O_RDONLY 또는 O_DIRECT|O_RDWR로 open. (O_DIRECT는 페이지 캐시 우회 → AIO 정합성)
 *   2) O_DIRECT 실패 시(일반 파일이 fs 미지원일 때) O_DIRECT 없이 재시도.
 *   3) nowait이면 RWF_NOWAIT 지원 여부와 블록 디바이스 여부를 검사.
 *
 * 호출 체인:
 *   create_aio_bdev → [이 함수] → open(2) → fstat(2)
 */
static int
bdev_aio_open(struct file_disk *disk, bool nowait)
{
	int fd;
	/* [한국어] open(2) 반환 fd 임시 저장. */
	int io_flag = disk->readonly ? O_RDONLY : O_RDWR;
	/* [한국어] readonly 옵션에 따라 open 모드 결정. read-write가 기본. */
#ifdef RWF_NOWAIT
	struct stat st;
	/* [한국어] fstat 결과 저장 — RWF_NOWAIT은 블록 디바이스에서만 안전. */
#endif

	fd = open(disk->filename, io_flag | O_DIRECT);
	/* [한국어] O_DIRECT: 페이지 캐시를 거치지 않고 디스크와 직접 DMA. AIO와 잘 맞음.
	 * 그러나 일부 fs(예: tmpfs)는 O_DIRECT를 지원하지 않으므로 실패 시 폴백. */
	if (fd < 0) {
		/* Try without O_DIRECT for non-disk files */
		/* [한국어] 위 영문 주석 부연: 디스크가 아닌 일반 파일에서 O_DIRECT 실패 시 시도. */
		fd = open(disk->filename, io_flag);
		if (fd < 0) {
			/* [한국어] 두 번째 시도도 실패 → 경로/권한 문제. errno 그대로 사용자에게 노출. */
			AIO_FDISK_ERRLOG(disk, "open() failed, rc %d: %s\n", fd, spdk_strerror(errno));
			disk->fd = -1;
			return -1;
		}
	}

	disk->fd = fd;
	/* [한국어] 성공한 fd 저장. */

	if (nowait) {
#ifdef RWF_NOWAIT
		/* Some aio operations can block, for example if number outstanding
		 * I/O exceeds number of block layer tags. But not all files can
		 * support RWF_NOWAIT flag. So use RWF_NOWAIT on block devices only.
		 */
		/* [한국어] 위 영문 주석 부연: 일반 AIO는 block layer 태그가 부족하면 블로킹할 수 있음.
		 * RWF_NOWAIT 플래그를 주면 즉시 -EAGAIN으로 반환됨 — 단, 모든 파일이 지원하는 건
		 * 아니라 블록 디바이스만 사용 가능. */
		if (!(fstat(fd, &st) == 0 && S_ISBLK(st.st_mode))) {
			/* [한국어] fstat 실패 또는 블록 디바이스가 아니면 nowait 거절. */
			AIO_FDISK_ERRLOG(disk, "Device is not block device; do not enable nowait usage.\n");
			goto err;
		}
#else
		/* [한국어] 컴파일 타임에 RWF_NOWAIT 매크로가 없는 커널 헤더(오래된 커널) → 포기. */
		AIO_FDISK_ERRLOG(disk,
				 "RWF_NOWAIT not defined; do not enable nowait usage or update the kernel.\n");
		goto err;
#endif
	}

	disk->use_nowait = nowait;
	/* [한국어] use_nowait 플래그를 디스크에 저장. submit 시 io_prep에 RWF_NOWAIT 추가용. */
	return 0;

err:
	errno = ENOTSUP;
	/* [한국어] 호출자에게 "지원 안 함" 신호. */
	bdev_aio_close(disk);
	/* [한국어] 이미 연 fd를 닫고 디스크 상태를 fd=-1로 되돌림. */
	return -1;
}

#ifdef __FreeBSD__
/*
 * [한국어] (FreeBSD 빌드)
 * bdev_aio_submit_io - POSIX AIO로 read/write 한 건을 비동기 제출
 *
 * @type: SPDK_BDEV_IO_TYPE_READ 또는 WRITE.
 * @fdisk, @ch, @aio_task, @iov, @iovcnt, @nbytes, @offset: I/O 파라미터.
 * @return: 0 이상(성공), 음수 -errno(실패).
 *
 * aiocb 구조체를 채우고 sigevent를 SIGEV_KEVENT로 설정해 완료를 채널의 kqueue로 통보. 채널
 * poller(bdev_user_io_getevents)가 kevent(2)로 회수.
 */
static int
bdev_aio_submit_io(enum spdk_bdev_io_type type, struct file_disk *fdisk,
		   struct spdk_io_channel *ch, struct bdev_aio_task *aio_task,
		   struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct aiocb *aiocb = &aio_task->aiocb;
	/* [한국어] task에 임베드된 aiocb 사용 → 별도 힙 할당 회피. */
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);
	/* [한국어] 코어 로컬 채널 컨텍스트. kqfd, io_inflight 등에 접근. */
	int rc;
	/* [한국어] aio_readv/writev 반환값. */

	memset(aiocb, 0, sizeof(struct aiocb));
	/* [한국어] 이전 사용 흔적 제거 — bdev_io 풀에서 재사용되므로 매번 초기화 필요. */
	aiocb->aio_fildes = fdisk->fd;
	/* [한국어] 대상 파일 fd. */
	aiocb->aio_iov = iov;
	/* [한국어] iovec 배열 포인터(scatter-gather). */
	aiocb->aio_iovcnt = iovcnt;
	/* [한국어] iovec 개수. */
	aiocb->aio_offset = offset;
	/* [한국어] 파일 시작에서의 바이트 오프셋. */
	aiocb->aio_sigevent.sigev_notify_kqueue = aio_ch->kqfd;
	/* [한국어] 완료 통보 채널 = 채널 kqueue. */
	aiocb->aio_sigevent.sigev_value.sival_ptr = aio_task;
	/* [한국어] kevent.udata로 전달될 user data — 완료 시 task로 회복. */
	aiocb->aio_sigevent.sigev_notify = SIGEV_KEVENT;
	/* [한국어] 통보 방식: signal 대신 kqueue. */

	aio_task->len = nbytes;
	/* [한국어] 완료 시 부분 완료 검출용 기대 바이트 수. */
	aio_task->ch = aio_ch;
	/* [한국어] 완료 시 io_inflight 감소를 위한 채널 포인터. */

	if (type == SPDK_BDEV_IO_TYPE_READ) {
		rc = aio_readv(aiocb);
		/* [한국어] POSIX AIO read submit. 비동기로 큐에만 등록. */
	} else {
		rc = aio_writev(aiocb);
		/* [한국어] POSIX AIO write submit. */
	}

	if (spdk_unlikely(rc < 0)) {
		return -errno;
		/* [한국어] errno → 음수 반환. 호출자가 -EAGAIN/-기타로 분기. */
	}

	return rc;
}
#else
/*
 * [한국어] (Linux 빌드)
 * bdev_aio_submit_io - libaio io_submit으로 read/write 한 건을 커널에 비동기 제출
 *
 * @type, @fdisk, @ch, @aio_task, @iov, @iovcnt, @nbytes, @offset: I/O 파라미터.
 * @return: 1 = 성공(1건 제출), 음수 = 실패.
 *
 * io_prep_preadv/pwritev로 iocb를 채우고, 인터럽트 모드라면 io_set_eventfd로 efd 등록.
 * iocb->data에 task 포인터를 심어 완료 시 회복 가능하게 한다. RWF_NOWAIT은 SPDK 빌드 옵션과
 * 커널 지원이 모두 있을 때만 활성. io_submit은 시스템 호출이지만 큐잉만 하므로 빠르다.
 */
static int
bdev_aio_submit_io(enum spdk_bdev_io_type type, struct file_disk *fdisk,
		   struct spdk_io_channel *ch, struct bdev_aio_task *aio_task,
		   struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct iocb *iocb = &aio_task->iocb;
	/* [한국어] task에 임베드된 iocb 사용. */
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);
	/* [한국어] 채널 컨텍스트(io_ctx, group_ch, io_inflight). */

	if (type == SPDK_BDEV_IO_TYPE_READ) {
		io_prep_preadv(iocb, fdisk->fd, iov, iovcnt, offset);
		/* [한국어] read iocb 초기화 — fd, iovec, offset 설정 + opcode=PREADV. */
	} else {
		io_prep_pwritev(iocb, fdisk->fd, iov, iovcnt, offset);
		/* [한국어] write iocb 초기화 — opcode=PWRITEV. */
	}

	if (aio_ch->group_ch->efd >= 0) {
		/* [한국어] 인터럽트 모드 활성: efd 유효 → 커널이 완료 시 efd를 깨우도록 설정. */
		io_set_eventfd(iocb, aio_ch->group_ch->efd);
		/* [한국어] iocb->aio_resfd = efd. 커널 AIO 완료 path가 efd write로 reactor 깨움. */
	}
	iocb->data = aio_task;
	/* [한국어] user data — 완료 시 io_event.data로 회복되어 task → bdev_io 추적. */
#if defined(RWF_NOWAIT) && defined(SPDK_CONFIG_AIO_HAVE_RW_FLAGS)
	if (fdisk->use_nowait) {
		/* [한국어] 사용자가 nowait를 활성화하고 빌드/커널이 지원한다면 RWF_NOWAIT 추가. */
		iocb->aio_rw_flags = RWF_NOWAIT;
		/* [한국어] 커널이 즉시 완료 가능하지 않으면 io_submit이 -EAGAIN 반환. */
	}
#endif
	aio_task->len = nbytes;
	/* [한국어] 완료 검사용 기대 길이. */
	aio_task->ch = aio_ch;
	/* [한국어] 완료 시 io_inflight 감소용. */

	return io_submit(aio_ch->io_ctx, 1, &iocb);
	/* [한국어] 시스템 호출: ctx에 1개의 iocb를 큐잉. 성공 시 1, 실패 시 음수 errno. */
}
#endif

/*
 * [한국어]
 * bdev_aio_rw - submit_io 호출 + 결과 처리(in_flight 카운트, 완료 보고)
 *
 * @type, @fdisk, @ch, @aio_task, @iov, @iovcnt, @nbytes, @offset: 동일.
 *
 * read/write 공통 래퍼. submit_io가 -EAGAIN이면 NOMEM 상태로 즉시 완료(상위가 retry 큐로 보냄),
 * 다른 음수면 aio_status 변환해 실패 보고, 성공이면 io_inflight를 1 증가시키고 종료(완료 보고는
 * poller가 담당).
 *
 * 호출 컨텍스트: 채널 reactor 스레드.
 *
 * 호출 체인:
 *   bdev_aio_get_buf_cb → [이 함수] → bdev_aio_submit_io → io_submit
 */
static void
bdev_aio_rw(enum spdk_bdev_io_type type, struct file_disk *fdisk,
	    struct spdk_io_channel *ch, struct bdev_aio_task *aio_task,
	    struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);
	/* [한국어] 채널 컨텍스트 — io_inflight 카운터에 접근하기 위해. */
	int rc;
	/* [한국어] submit 결과. */

	if (type == SPDK_BDEV_IO_TYPE_READ) {
		AIO_FDISK_DEBUGLOG(fdisk, "read %d iovs size %lu to off: %#lx\n",
				   iovcnt, nbytes, offset);
		/* [한국어] DEBUG 빌드에서만 출력 — 핫패스 추적용. */
	} else {
		AIO_FDISK_DEBUGLOG(fdisk, "write %d iovs size %lu from off: %#lx\n",
				   iovcnt, nbytes, offset);
	}

	rc = bdev_aio_submit_io(type, fdisk, ch, aio_task, iov, iovcnt, nbytes, offset);
	/* [한국어] 실제 io_submit/aio_readv 호출. */
	if (spdk_unlikely(rc < 0)) {
		/* [한국어] 음수 반환 = 제출 실패. */
		if (rc == -EAGAIN) {
			/* [한국어] 큐 포화/RWF_NOWAIT 차단 — 일시적 메모리 부족으로 모델링.
			 * NOMEM 상태로 보고하면 bdev core가 자동으로 retry 큐로 보낸다. */
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			/* [한국어] 그 외 -EINVAL 등 영구 오류 — aio_status로 변환해 사용자에게 보고. */
			spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), rc);
			AIO_FDISK_ERRLOG(fdisk, "%s: io_submit returned %d\n", __func__, rc);
		}
	} else {
		aio_ch->io_inflight++;
		/* [한국어] 성공 제출 → in-flight 카운트 +1. 완료 시 poller가 -1. RESET 처리에서 사용. */
	}
}

/*
 * [한국어]
 * bdev_aio_flush - FLUSH I/O를 fsync(2)로 처리
 *
 * @fdisk: 대상 디스크.
 * @aio_task: 완료 통보용 task.
 *
 * fsync는 동기 호출이라 reactor를 잠시 블로킹하지만, FLUSH는 빈도가 낮고 일반적으로 SPDK
 * 사용자가 명시적으로 요청하므로 허용. 실제로는 페이지 캐시를 우회하는 O_DIRECT라 fsync가
 * 대부분 짧다.
 */
static void
bdev_aio_flush(struct file_disk *fdisk, struct bdev_aio_task *aio_task)
{
	int rc = fsync(fdisk->fd);
	/* [한국어] 시스템 호출: 파일/디바이스의 메타데이터+데이터를 영구 매체에 동기화. */

	if (rc == 0) {
		/* [한국어] 성공 → bdev_io를 SUCCESS로 보고. */
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		/* [한국어] 실패 → errno를 aio_status로 변환해 보고. */
		spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), -errno);
	}
}

#ifndef __FreeBSD__
/*
 * [한국어] (Linux 빌드 한정)
 * bdev_aio_fallocate - UNMAP/WRITE_ZEROES를 fallocate(2) 시스템 호출로 매핑
 *
 * @bdev_io: 처리할 I/O.
 * @mode: FALLOC_FL_PUNCH_HOLE 등의 fallocate 모드.
 *
 * UNMAP은 PUNCH_HOLE(블록 해제), WRITE_ZEROES는 ZERO_RANGE(0으로 채움 또는 hole) 매핑.
 * 사용자가 fallocate=true 옵션 없이 디스크를 만들었으면 -ENOTSUP 즉시 반환.
 */
static void
bdev_aio_fallocate(struct spdk_bdev_io *bdev_io, int mode)
{
	struct file_disk *fdisk = fdisk_from_bdev(bdev_io->bdev);
	/* [한국어] bdev → file_disk 역포인터. fd, fallocate 플래그에 접근. */
	struct bdev_aio_task *aio_task = (struct bdev_aio_task *)bdev_io->driver_ctx;
	/* [한국어] 모듈 컨텍스트. 완료 통보 시 spdk_bdev_io_from_ctx 입력. */
	uint64_t offset_bytes = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;
	/* [한국어] 블록 단위 → 바이트 단위 변환. fallocate는 바이트 인자. */
	uint64_t length_bytes = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
	/* [한국어] 처리할 길이(바이트). */
	int rc;
	/* [한국어] fallocate 반환값. */

	if (!fdisk->fallocate) {
		/* [한국어] RPC에서 fallocate=true로 디스크를 만들지 않았으면 미지원으로 즉시 거절. */
		spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), -ENOTSUP);
		return;
	}

	rc = fallocate(fdisk->fd, mode, offset_bytes, length_bytes);
	/* [한국어] 시스템 호출: 파일에 대한 hole punch / zero fill. fs가 이를 지원해야 함(ext4/xfs). */
	if (rc == 0) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);
		/* [한국어] 성공 보고. */
	} else {
		spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), -errno);
		/* [한국어] errno → 음수 errno로 보고. */
	}
}

/*
 * [한국어]
 * bdev_aio_unmap - UNMAP I/O 처리. fallocate(PUNCH_HOLE)로 파일 영역을 해제.
 */
static void
bdev_aio_unmap(struct spdk_bdev_io *bdev_io)
{
	int mode = FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE;
	/* [한국어] PUNCH_HOLE: 지정 영역의 매핑을 제거(파일 크기는 유지). 블록 디바이스에서는
	 * discard(TRIM)와 동등한 효과로 매핑됨. */

	bdev_aio_fallocate(bdev_io, mode);
}


/*
 * [한국어]
 * bdev_aio_write_zeros - WRITE_ZEROES I/O 처리. fallocate(ZERO_RANGE).
 */
static void
bdev_aio_write_zeros(struct spdk_bdev_io *bdev_io)
{
	int mode = FALLOC_FL_ZERO_RANGE;
	/* [한국어] ZERO_RANGE: 지정 영역을 모두 0으로 채움(또는 hole로 변환). 블록 디바이스에서는
	 * Write Zeroes 명령으로 변환되어 효율적. */

	bdev_aio_fallocate(bdev_io, mode);
}
#endif

/*
 * [한국어]
 * bdev_aio_destruct_cb - io_device 모든 채널 destroy 후의 후속 정리
 *
 * @io_device: file_disk 포인터(spdk_io_device_register 키).
 *
 * spdk_io_device_unregister가 모든 reactor의 채널 destroy를 끝내면 호출. 글로벌 리스트에서
 * 자기를 빼고, fd close, 메모리 free. bdev core는 이 콜백 후 spdk_bdev_unregister 완료를
 * 사용자에게 통보한다.
 *
 * 호출 컨텍스트: 마지막 reactor의 destroy 콜백 이후 호출됨(보통 main thread).
 */
static void
bdev_aio_destruct_cb(void *io_device)
{
	struct file_disk *fdisk = io_device;
	/* [한국어] io_device 키는 fdisk 포인터로 등록되어 있음. */

	TAILQ_REMOVE(&g_aio_disk_head, fdisk, link);
	/* [한국어] 글로벌 리스트에서 제거. */
	bdev_aio_close(fdisk);
	/* [한국어] fd close. */
	aio_free_disk(fdisk);
	/* [한국어] filename, name 등 메모리 free + fdisk 자체 free. */
}

/*
 * [한국어]
 * bdev_aio_destruct - bdev 인스턴스 destruct 콜백 (fn_table.destruct)
 *
 * @ctx: spdk_bdev->ctxt = file_disk.
 * @return: 0 = 즉시 완료(여기서는 0), 1 = async.
 *
 * 실제 정리는 비동기 — io_device unregister가 모든 채널을 destroy한 다음에 destruct_cb가
 * 호출되어 메모리/파일 자원을 정리. 이 함수 자체는 unregister 시작만 한다.
 *
 * 호출 컨텍스트: bdev unregister 처리 스레드.
 */
static int
bdev_aio_destruct(void *ctx)
{
	struct file_disk *fdisk = ctx;
	/* [한국어] ctx → file_disk. */

	spdk_io_device_unregister(fdisk, bdev_aio_destruct_cb);
	/* [한국어] 모든 채널 destroy 후 destruct_cb 호출 예약. */

	return 0;
	/* [한국어] 0 반환 — bdev core는 unregister가 비동기로 진행됨을 인지. */
}

/*
 * [한국어]
 * bdev_aio_hot_remove - hot-remove 처리: app thread에서 bdev_aio_delete 호출
 *
 * @ctx: bdev 이름 문자열(strdup 사본). 호출 후 해제 책임은 본 함수가 가짐.
 *
 * 디바이스가 시스템에서 분리(hot-removal)되면 io 완료가 -EIO/-ENODEV로 들어오는데, 이 시점에
 * bdev_aio_delete를 직접 호출하면 안 된다(잘못된 스레드에서 호출 가능성). spdk_thread_send_msg로
 * app thread에 일을 미루고 그 결과 이 함수가 안전한 컨텍스트에서 실행된다.
 */
static void
bdev_aio_hot_remove(void *ctx)
{
	char *name = ctx;
	/* [한국어] strdup된 이름. */

	bdev_aio_delete(name, NULL, NULL);
	/* [한국어] 콜백/인자 NULL — fire-and-forget delete. */

	free(name);
	/* [한국어] strdup 짝. */
}

/*
 * [한국어]
 * bdev_aio_try_hot_remove - hot-remove를 한 번만 시작하도록 race-free 가드
 *
 * @fdisk: 대상 디스크.
 *
 * 여러 reactor가 동시에 ENODEV를 검출해 hot_remove를 호출할 수 있으므로, atomic test-and-set으로
 * 첫 호출자만 진행시키고 나머지는 즉시 반환. 이름을 strdup해 app thread로 메시지 송신.
 *
 * 동기화 메커니즘: __atomic_test_and_set(__ATOMIC_RELAXED) — 단일 비트 플래그를 1로 set하고
 * 이전 값 반환. 이전이 1이면 이미 처리 중이므로 즉시 종료(경쟁 회피).
 *
 * 호출 컨텍스트: 임의 reactor의 completion 폴러.
 *
 * 호출 체인:
 *   bdev_aio_io_channel_poll → ENODEV 분기 → [이 함수] → spdk_thread_send_msg(app)
 *     → bdev_aio_hot_remove → bdev_aio_delete
 */
static void
bdev_aio_try_hot_remove(struct file_disk *fdisk)
{
	char	*name;
	/* [한국어] strdup된 이름 임시 보관. */

	if (__atomic_test_and_set(&fdisk->hot_remove_in_progress, __ATOMIC_RELAXED)) {
		/* [한국어] 이전 값이 1 → 이미 다른 코어가 처리 중. 중복 진입 방지.
		 * RELAXED: 동기화 자체가 핵심이고 다른 메모리와의 순서는 무관 → 가장 약한 모델로 충분. */
		return;
	}

	name = strdup(fdisk->disk.name);
	/* [한국어] app thread로 보낼 인자(이름)는 fdisk가 곧 free되므로 사본이 필요. */
	if (!name) {
		/* [한국어] OOM. 플래그 원복해 다른 코어가 다시 시도하게 함. */
		__atomic_clear(&fdisk->hot_remove_in_progress, __ATOMIC_RELAXED);
		return;
	}

	AIO_FDISK_ERRLOG(fdisk, "hot-remove detected, unregistering bdev...\n");
	/* [한국어] 운영자에게 hot-remove 알림. */
	spdk_thread_send_msg(spdk_thread_get_app_thread(), bdev_aio_hot_remove, name);
	/* [한국어] app thread(주 RPC/관리 스레드)로 hot_remove 작업을 위임. lockless 메시지 큐 사용. */
}

#ifdef __FreeBSD__
/*
 * [한국어] (FreeBSD)
 * bdev_user_io_getevents - kqueue로 완료 이벤트 수신
 */
static int
bdev_user_io_getevents(int kq, unsigned int max, struct kevent *events)
{
	struct timespec ts;
	/* [한국어] kevent timeout — 0/0으로 즉시 반환(non-blocking). */
	int count;
	/* [한국어] 회수된 이벤트 수. */

	memset(events, 0, max * sizeof(struct kevent));
	/* [한국어] 결과 버퍼 0 초기화 — 이전 호출 잔재 제거. */
	memset(&ts, 0, sizeof(ts));
	/* [한국어] timeout 0 → 즉시 폴링. */

	count = kevent(kq, NULL, 0, events, max, &ts);
	/* [한국어] kevent(2) 시스템 호출. changelist=NULL → 새 이벤트 등록 없이 회수만. */
	if (count < 0) {
		SPDK_ERRLOG("failed to get kevents: %s.\n", spdk_strerror(errno));
		return -errno;
	}

	return count;
}

/*
 * [한국어] (FreeBSD)
 * bdev_aio_io_channel_poll - 한 채널의 완료 이벤트를 회수해 bdev_io 완료 보고
 *
 * @io_ch: 폴링 대상 채널.
 * @return: 처리한 이벤트 수.
 */
static int
bdev_aio_io_channel_poll(struct bdev_aio_io_channel *io_ch)
{
	int nr, i, rc;
	struct bdev_aio_task *aio_task;
	struct kevent events[SPDK_AIO_QUEUE_DEPTH];
	/* [한국어] 한 번에 회수할 최대 이벤트 = 큐 깊이. */
	struct spdk_bdev_io *bdev_io;

	nr = bdev_user_io_getevents(io_ch->kqfd, SPDK_AIO_QUEUE_DEPTH, events);
	if (nr < 0) {
		return 0;
		/* [한국어] errno로 실패 — 이번 tick은 처리 0건. */
	}

	for (i = 0; i < nr; i++) {
		aio_task = events[i].udata;
		/* [한국어] sigevent.sival_ptr로 심어둔 task 포인터 회복. */
		aio_task->ch->io_inflight--;
		/* [한국어] in-flight 카운트 -1. RESET 처리에 사용됨. */
		bdev_io = (struct spdk_bdev_io *)spdk_bdev_io_from_ctx(aio_task);

		if (aio_task == NULL) {
			/* [한국어] (방어적) udata가 NULL이면 심각한 버그 — 일단 실패 보고 후 루프 종료. */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			break;
		} else if ((uint64_t)aio_return(&aio_task->aiocb) == aio_task->len) {
			/* [한국어] aio_return = 실제 처리 바이트. 기대 길이와 같으면 SUCCESS. */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			/* [한국어] 부분 완료 또는 에러 — aio_error로 errno 회수. */
			AIO_FDISK_ERRLOG(fdisk_from_bdev(bdev_io->bdev), "failed to complete: rc %d\n",
					 aio_error(&aio_task->aiocb));
			rc = aio_error(&aio_task->aiocb);
			if (rc != 0) {
				/* [한국어] 에러 코드를 aio_status로 변환해 보고. */
				spdk_bdev_io_complete_aio_status(bdev_io, rc);
			} else {
				/* [한국어] 에러는 아니지만 부분 완료 — generic FAILED로 보고. */
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			}
		}
	}

	return nr;
}
#else
/*
 * [한국어] (Linux)
 * bdev_user_io_getevents - mmap된 AIO ring을 직접 읽어 io_getevents 시스템 호출 우회
 *
 * @io_ctx: io_setup으로 만든 컨텍스트(=ring 시작 주소).
 * @max: 회수 상한.
 * @uevents: [out] 회수한 io_event를 채울 사용자 버퍼.
 * @return: 회수한 이벤트 수.
 *
 * 핵심 최적화: io_getevents(2)는 시스템 호출이라 비싸지만, libaio가 io_setup 시 mmap한 사용자
 * 공간 ring의 head/tail을 직접 읽으면 syscall 없이 완료 이벤트를 회수 가능하다. 단 ring 레이아웃은
 * 커널 버전에 따라 변할 수 있어 version 매직과 incompat_features를 검사해 fallback 경로를 둔다.
 *
 * 메모리 모델:
 *   - head/tail 로드 후 spdk_smp_rmb() — 이벤트 배열 카피가 head/tail 순서 뒤로 밀리지 않도록.
 *   - 카피 후 head 갱신 전 배리어 — 갱신이 카피 완료 전에 보이지 않도록(특히 ARM).
 */
static int
bdev_user_io_getevents(io_context_t io_ctx, unsigned int max, struct io_event *uevents)
{
	uint32_t head, tail, count;
	struct spdk_aio_ring *ring;
	struct timespec timeout;
	struct io_event *kevents;

	ring = (struct spdk_aio_ring *)io_ctx;
	/* [한국어] io_context_t가 곧 mmap된 ring 시작 주소. 헤더 레이아웃을 우리 구조체로 해석. */

	if (spdk_unlikely(ring->version != SPDK_AIO_RING_VERSION || ring->incompat_features != 0)) {
		/* [한국어] 매직 불일치 또는 비호환 기능 → 안전을 위해 시스템 호출 fallback. */
		timeout.tv_sec = 0;
		timeout.tv_nsec = 0;
		/* [한국어] non-blocking. */

		return io_getevents(io_ctx, 0, max, uevents, &timeout);
		/* [한국어] 정식 시스템 호출 사용. min_nr=0 → 즉시 반환. */
	}

	/* Read the current state out of the ring */
	/* [한국어] 위 영문 주석 부연: ring의 현재 head/tail을 읽음. */
	head = ring->head;
	tail = ring->tail;

	/* This memory barrier is required to prevent the loads above
	 * from being re-ordered with stores to the events array
	 * potentially occurring on other threads. */
	/* [한국어] 위 영문 주석 부연: 위의 head/tail 로드가 이벤트 배열 카피와 재정렬되지 않도록
	 * read memory barrier. 다른 스레드(=커널)가 이벤트를 ring에 쓰는 stores와의 순서를 보장. */
	spdk_smp_rmb();

	/* Calculate how many items are in the circular ring */
	/* [한국어] 위 영문 주석 부연: 원형 큐에 있는 항목 수 계산. */
	count = tail - head;
	if (tail < head) {
		/* [한국어] tail이 한 바퀴 돌아 head 앞에 있는 경우. size를 더해 정상화. */
		count += ring->size;
	}

	/* Reduce the count to the limit provided by the user */
	/* [한국어] 위 영문 주석 부연: 사용자 한도로 잘라냄. */
	count = spdk_min(max, count);

	/* Grab the memory location of the event array */
	/* [한국어] 위 영문 주석 부연: 이벤트 배열 시작 = ring 시작 + header_length(헤더 + 패딩). */
	kevents = (struct io_event *)((uintptr_t)ring + ring->header_length);

	/* Copy the events out of the ring. */
	/* [한국어] 위 영문 주석 부연: ring에서 사용자 버퍼로 이벤트 복사. */
	if ((head + count) <= ring->size) {
		/* Only one copy is required */
		/* [한국어] 위 영문 주석 부연: 한 번에 카피. wrap-around 없음. */
		memcpy(uevents, &kevents[head], count * sizeof(struct io_event));
	} else {
		uint32_t first_part = ring->size - head;
		/* Two copies are required */
		/* [한국어] 위 영문 주석 부연: head ~ size-1, 그리고 0 ~ 나머지를 두 번에 나눠 카피. */
		memcpy(uevents, &kevents[head], first_part * sizeof(struct io_event));
		memcpy(&uevents[first_part], &kevents[0], (count - first_part) * sizeof(struct io_event));
	}

	/* Update the head pointer. On x86, stores will not be reordered with older loads,
	 * so the copies out of the event array will always be complete prior to this
	 * update becoming visible. On other architectures this is not guaranteed, so
	 * add a barrier. */
	/* [한국어] 위 영문 주석 부연: head 갱신 전 메모리 배리어. x86은 TSO(Total Store Order)이므로
	 * 컴파일러 배리어로 충분하지만, ARM 등 약한 모델에서는 명시적 SMP 배리어가 필요. */
#if defined(__i386__) || defined(__x86_64__)
	spdk_compiler_barrier();
	/* [한국어] 컴파일러 재정렬만 막음(하드웨어 배리어 불필요). */
#else
	spdk_smp_mb();
	/* [한국어] full SMP barrier — store-store/load-store 모두 차단. */
#endif
	ring->head = (head + count) % ring->size;
	/* [한국어] 회수한 만큼 head 진행. 커널이 이 갱신을 보고 같은 슬롯을 재사용 가능. */

	return count;
}

/*
 * [한국어] (Linux)
 * bdev_aio_io_channel_poll - 한 채널의 완료 이벤트를 mmap ring에서 회수해 bdev_io 완료 보고
 *
 * @io_ch: 폴링 대상 채널.
 * @return: 처리한 이벤트 수.
 *
 * 각 io_event에 대해:
 *   - data 필드 = task 포인터(우리가 io_submit 시 심어둔 것).
 *   - res 필드 = 실제 처리 바이트(>=0) 또는 음수 errno.
 *   - res == task->len → SUCCESS. 그 외 분기는 EAGAIN/ENODEV/일반 오류 처리.
 *
 * Hot-remove 검출: 디바이스가 분리되면 io가 0 또는 -EIO로 완료. 이 경우 BLKGETSIZE64가 0을
 * 반환하는 것을 spdk_fd_get_size로 확인해 ENODEV로 변환.
 */
static int
bdev_aio_io_channel_poll(struct bdev_aio_io_channel *io_ch)
{
	int nr, i, rc;
	struct bdev_aio_task *aio_task;
	struct io_event events[SPDK_AIO_QUEUE_DEPTH];
	/* [한국어] 한 번 폴링에 회수할 이벤트 버퍼(스택 할당). */
	struct spdk_bdev_io *bdev_io;
	struct file_disk *fdisk;

	nr = bdev_user_io_getevents(io_ch->io_ctx, SPDK_AIO_QUEUE_DEPTH, events);
	if (nr < 0) {
		return 0;
		/* [한국어] 실패 → 이번 tick 처리 0. */
	}

	for (i = 0; i < nr; i++) {
		aio_task = events[i].data;
		/* [한국어] iocb->data로 심어둔 task 포인터 회복. */
		aio_task->ch->io_inflight--;
		/* [한국어] in-flight 카운트 -1. */
		bdev_io = (struct spdk_bdev_io *)spdk_bdev_io_from_ctx(aio_task);

		if (events[i].res == aio_task->len) {
			/* [한국어] 정상 — 요청한 만큼 처리됨. SUCCESS 보고 후 다음 이벤트로. */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
			continue;
		}

		/* From aio_abi.h, io_event.res is defined __s64, negative errno
		 * will be assigned to io_event.res for error situation.
		 * But from libaio.h, io_event.res is defined unsigned long, so
		 * convert it to signed value for error detection.
		 */
		/* [한국어] 위 영문 주석 부연: 커널 헤더 aio_abi.h는 res를 부호 있는 64비트로 정의해
		 * 음수 errno를 반환하지만, 사용자 헤더 libaio.h는 unsigned long으로 노출해 음수 검출이
		 * 어렵다. (int)캐스트로 부호 회복 후 분기. */
		rc = (int)events[i].res;
		fdisk = fdisk_from_bdev(bdev_io->bdev);

		/* When the block device device is detached from the system, IOs fail with res of 0.
		 * In this case the ioctl BLKGETSIZE64 yields a device size of 0.
		 * Note that re-attaching the device will not correct this because the existing fd is
		 * still invalid.
		 * When the fd is a file and the mount backing the file is detached, IOs fail
		 * with a res of -EIO and the ioctl BLKGETSIZE64 yields a device size of 0.
		 */
		/* [한국어] 위 영문 주석 부연: 블록 디바이스 hot-removal 시 res=0(부분 완료처럼 보임)이고,
		 * 파일이 들어있는 mount가 분리되면 res=-EIO로 옴. 둘 다 BLKGETSIZE64가 0을 반환하므로
		 * 그 신호를 ENODEV로 매핑해 동일 경로로 처리. */
		if (rc == -EIO || rc >= 0) {
			if (spdk_fd_get_size(fdisk->fd) == 0) {
				rc = -ENODEV;
				/* [한국어] hot-removal 신호 → ENODEV로 정규화. */
			}
		}

		if (rc < 0) {
			if (rc == -EAGAIN) {
				/* [한국어] RWF_NOWAIT 차단 등 일시적 실패 → NOMEM으로 보고하면 bdev core가
				 * 자동 retry 큐로 보냄. */
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
			} else if (rc == -ENODEV) {
				/* [한국어] hot-remove 트리거. */
				bdev_aio_try_hot_remove(fdisk);
				spdk_bdev_io_complete_aio_status(bdev_io, rc);
			} else {
				/* [한국어] 일반 실패 — errno 그대로 보고. */
				AIO_FDISK_ERRLOG(fdisk, "failed to complete: rc %"PRId64"\n", events[i].res);
				spdk_bdev_io_complete_aio_status(bdev_io, rc);
			}
		} else {
			/* [한국어] rc>=0인데 task->len과 다름 → 부분 완료. 일반 FAILED로 보고. */
			AIO_FDISK_ERRLOG(fdisk, "failed to complete: rc %"PRId64"\n", events[i].res);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}

	return nr;
}
#endif

/*
 * [한국어]
 * bdev_aio_group_poll - 그룹 채널 polling 콜백 (폴드 모드)
 *
 * @arg: bdev_aio_group_channel.
 * @return: SPDK_POLLER_BUSY = 일을 했음, SPDK_POLLER_IDLE = 빈 폴링.
 *
 * reactor에 묶인 모든 AIO io 채널을 순회하며 io_channel_poll. 채널이 N개여도 reactor당 단일
 * poller로 합산하므로 컨텍스트 전환과 자료구조 비용이 일정.
 */
static int
bdev_aio_group_poll(void *arg)
{
	struct bdev_aio_group_channel *group_ch = arg;
	struct bdev_aio_io_channel *io_ch;
	int nr = 0;
	/* [한국어] 이번 tick에 처리한 총 이벤트 수. */

	TAILQ_FOREACH(io_ch, &group_ch->io_ch_head, link) {
		nr += bdev_aio_io_channel_poll(io_ch);
		/* [한국어] 각 채널의 완료 회수 결과 합산. */
	}

	return nr > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
	/* [한국어] reactor가 idle 추적에 사용 — busy면 다음 tick을 더 빠르게 돌리고 idle이면
	 * 전력 절감 모드(있다면) 진입. */
}

/*
 * [한국어]
 * bdev_aio_group_interrupt - 인터럽트 모드 핸들러 (eventfd 깨움 시 호출)
 *
 * @arg: bdev_aio_group_channel.
 * @return: poll 결과(이벤트 수 또는 음수).
 *
 * 커널이 AIO 완료마다 efd를 1만큼 증가시키므로, 인터럽트가 발생하면 read(efd)로 누적 카운터를
 * 회수하고 폴러를 한 번 돈다. 누적이 큐 깊이를 넘으면 다음 tick에 다시 깨우도록 efd에 차이만큼
 * 다시 write — "일이 더 있음" 자기 통보 패턴.
 */
static int
bdev_aio_group_interrupt(void *arg)
{
	struct bdev_aio_group_channel *group_ch = arg;
	int rc;
	uint64_t num_events;
	/* [한국어] eventfd read는 8바이트 누적값을 돌려준다. */

	assert(group_ch->efd >= 0);
	/* [한국어] 인터럽트 모드에서만 호출되므로 efd 유효성 보장. */

	/* if completed IO number is larger than SPDK_AIO_QUEUE_DEPTH,
	 * io_getevent should be called again to ensure all completed IO are processed.
	 */
	/* [한국어] 위 영문 주석 부연: 한 번에 회수할 수 있는 이벤트 한도가 큐 깊이라서, 누적이 더
	 * 많으면 한 번의 io_getevents로는 다 못 받음. 차이만큼 efd에 재발생시켜 다음 tick에 처리. */
	rc = read(group_ch->efd, &num_events, sizeof(num_events));
	/* [한국어] 시스템 호출: efd의 누적 카운터를 회수(0으로 리셋). */
	if (rc < 0) {
		SPDK_ERRLOG("failed to acknowledge aio group: %s.\n", spdk_strerror(errno));
		return -errno;
	}

	if (num_events > SPDK_AIO_QUEUE_DEPTH) {
		num_events -= SPDK_AIO_QUEUE_DEPTH;
		/* [한국어] 한도를 넘는 차이만 남김. */
		rc = write(group_ch->efd, &num_events, sizeof(num_events));
		/* [한국어] efd에 차이만큼 다시 적어 다음 tick에 같은 핸들러가 깨어나게 함. */
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify aio group: %s.\n", spdk_strerror(errno));
		}
	}

	return bdev_aio_group_poll(group_ch);
	/* [한국어] 실제 폴링은 group_poll이 처리. */
}

/*
 * [한국어]
 * _bdev_aio_get_io_inflight - 채널 순회 중 in-flight=0 검사 콜백
 *
 * @i: spdk_for_each_channel iteration 핸들.
 *
 * RESET 처리 시 모든 reactor를 돌며 자기 채널의 io_inflight를 검사한다. 0이 아니면 -1로
 * 멈추고, 모두 0이면 0으로 종료.
 */
static void
_bdev_aio_get_io_inflight(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	/* [한국어] 현재 reactor의 채널 핸들 획득. */
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);
	/* [한국어] 채널 컨텍스트. */

	if (aio_ch->io_inflight) {
		/* [한국어] 아직 처리 중인 I/O가 있음 → -1을 가지고 종료(early-exit). */
		spdk_for_each_channel_continue(i, -1);
		return;
	}

	spdk_for_each_channel_continue(i, 0);
	/* [한국어] 0 → 다음 채널로 진행. */
}

static int bdev_aio_reset_retry_timer(void *arg);
/* [한국어] 전방 선언: 아래에서 정의. */

/*
 * [한국어]
 * _bdev_aio_get_io_inflight_done - 모든 채널 순회 후 결과 처리
 *
 * @i: iteration 핸들.
 * @status: 0 = 모두 in-flight 0, -1 = 어딘가에 보류 중.
 *
 * 모두 0이면 RESET을 즉시 SUCCESS로 보고. 보류 중이면 500ms 후 다시 검사하는 retry timer를 등록.
 * (RESET 자체는 단순 fsync로는 안 되고, 현재 in-flight 모두 끝나길 기다리는 의미만 가짐 —
 * 본 모듈의 RESET 의미는 "fence")
 */
static void
_bdev_aio_get_io_inflight_done(struct spdk_io_channel_iter *i, int status)
{
	struct file_disk *fdisk = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] iter ctx로 fdisk 회복. */

	if (status == -1) {
		/* [한국어] 아직 in-flight 있음 → 500ms 후 재검사. */
		fdisk->reset_retry_timer = SPDK_POLLER_REGISTER(bdev_aio_reset_retry_timer, fdisk, 500);
		return;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(fdisk->reset_task), SPDK_BDEV_IO_STATUS_SUCCESS);
	/* [한국어] 모든 in-flight 완료 → RESET 작업도 완료 보고. */
}

/*
 * [한국어]
 * bdev_aio_reset_retry_timer - RESET retry timer 콜백 + RESET 트리거 진입점
 *
 * @arg: file_disk.
 * @return: SPDK_POLLER_BUSY (이번 tick에 작업 시작했음).
 *
 * 등록된 타이머가 있으면 unregister 후, 모든 채널 순회를 시작한다. 본 함수는 bdev_aio_reset에서
 * 직접 호출되기도 하고(첫 시도) timer 콜백으로 다시 들어오기도 한다.
 */
static int
bdev_aio_reset_retry_timer(void *arg)
{
	struct file_disk *fdisk = arg;

	if (fdisk->reset_retry_timer) {
		/* [한국어] 이전 타이머가 살아있으면 정리 — 한 시점에 한 번만 시도. */
		spdk_poller_unregister(&fdisk->reset_retry_timer);
	}

	spdk_for_each_channel(fdisk,
			      _bdev_aio_get_io_inflight,
			      fdisk,
			      _bdev_aio_get_io_inflight_done);
	/* [한국어] 모든 reactor 채널 순회 시작 — io_inflight=0 검사 후 done 콜백. */

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * bdev_aio_reset - RESET I/O 처리 진입점
 *
 * @fdisk: 대상 디스크.
 * @aio_task: RESET task — 완료 시 SUCCESS 보고용으로 fdisk->reset_task에 보관.
 *
 * fence 의미: 현재 in-flight인 모든 I/O가 완료될 때까지 대기 후 SUCCESS. AIO 자체에는
 * "디바이스 리셋" 같은 명령이 없으므로 단순 동기화로 매핑.
 */
static void
bdev_aio_reset(struct file_disk *fdisk, struct bdev_aio_task *aio_task)
{
	fdisk->reset_task = aio_task;
	/* [한국어] retry done 콜백이 spdk_bdev_io_complete에 사용. */

	bdev_aio_reset_retry_timer(fdisk);
	/* [한국어] 즉시 첫 시도. in-flight 없으면 즉시 SUCCESS, 있으면 retry 타이머 등록. */
}

/*
 * [한국어]
 * bdev_aio_get_buf_cb - 정렬된 버퍼 확보 후 호출되는 read/write 진입점
 *
 * @ch: 채널.
 * @bdev_io: I/O 요청. iovs/iovcnt가 정렬된 버퍼로 채워져 있음.
 * @success: 버퍼 할당 성공 여부.
 *
 * spdk_bdev_io_get_buf는 사용자 버퍼가 required_alignment에 맞지 않을 때 bdev pool에서
 * 정렬된 bounce buffer를 확보해주고 본 콜백을 호출. 사용자 버퍼가 이미 정렬되어 있으면 즉시 호출.
 *
 * 호출 컨텍스트: 채널 reactor 스레드.
 */
static void
bdev_aio_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		    bool success)
{
	struct file_disk *fdisk = fdisk_from_bdev(bdev_io->bdev);
	/* [한국어] bdev → file_disk 역포인터. */

	if (!success) {
		/* [한국어] 버퍼 풀 고갈 등 — bdev_io 즉시 실패 보고. */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_aio_rw(bdev_io->type,
			    fdisk,
			    ch,
			    (struct bdev_aio_task *)bdev_io->driver_ctx,
			    bdev_io->u.bdev.iovs,
			    bdev_io->u.bdev.iovcnt,
			    bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
			    /* [한국어] 블록 단위 → 바이트 단위. */
			    bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		break;
	default:
		/* [한국어] 본 콜백은 read/write에만 등록되므로 이 분기는 비정상. */
		AIO_FDISK_ERRLOG(fdisk, "Wrong io type: %d\n", bdev_io->type);
		break;
	}
}

/*
 * [한국어]
 * _bdev_aio_submit_request - I/O 타입별 분기해 적절한 처리기 호출
 *
 * @ch: 채널.
 * @bdev_io: I/O.
 * @return: 0 = 처리 시작(또는 즉시 완료), -1 = 미지원.
 *
 * read/write는 spdk_bdev_io_get_buf를 통해 정렬된 버퍼를 확보한 뒤 bdev_aio_get_buf_cb로
 * 진행. 그 외(flush/reset/unmap/write_zeros)는 즉시 해당 처리 함수로 위임.
 */
static int
_bdev_aio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct file_disk *fdisk = fdisk_from_bdev(bdev_io->bdev);

	switch (bdev_io->type) {
	/* Read and write operations must be performed on buffers aligned to
	 * bdev->required_alignment. If user specified unaligned buffers,
	 * get the aligned buffer from the pool by calling spdk_bdev_io_get_buf. */
	/* [한국어] 위 영문 주석 부연: O_DIRECT는 정렬된 버퍼를 요구한다(보통 블록 크기 정렬).
	 * 사용자가 정렬되지 않은 버퍼를 줬으면 spdk_bdev_io_get_buf가 정렬된 bounce buffer를 풀에서
	 * 빌려와 카피해 준다. 이미 정렬되어 있으면 즉시 콜백을 호출. */
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_aio_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		/* [한국어] read 진행. 콜백에서 bdev_aio_rw 호출. */
		return 0;
	case SPDK_BDEV_IO_TYPE_WRITE:
		if (fdisk->readonly) {
			/* [한국어] read-only 옵션으로 열린 디스크에 write → 즉시 실패. */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		} else {
			spdk_bdev_io_get_buf(bdev_io, bdev_aio_get_buf_cb,
					     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		}
		return 0;

	case SPDK_BDEV_IO_TYPE_FLUSH:
		/* [한국어] fsync(2)로 처리. 동기 호출이지만 빈도 낮음. */
		bdev_aio_flush(fdisk, (struct bdev_aio_task *)bdev_io->driver_ctx);
		return 0;

	case SPDK_BDEV_IO_TYPE_RESET:
		/* [한국어] in-flight 모두 끝날 때까지 기다린 후 SUCCESS. */
		bdev_aio_reset(fdisk, (struct bdev_aio_task *)bdev_io->driver_ctx);
		return 0;

#ifndef __FreeBSD__
	case SPDK_BDEV_IO_TYPE_UNMAP:
		bdev_aio_unmap(bdev_io);
		/* [한국어] fallocate(PUNCH_HOLE) — fallocate 옵션 활성 시만 동작. */
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		bdev_aio_write_zeros(bdev_io);
		/* [한국어] fallocate(ZERO_RANGE). */
		return 0;
#endif

	default:
		/* [한국어] 모듈이 지원하지 않는 타입(예: COMPARE/COPY 등). 호출자가 FAILED 처리. */
		return -1;
	}
}

/*
 * [한국어]
 * bdev_aio_submit_request - bdev core 진입점 (fn_table.submit_request)
 *
 * 단순 래퍼. _bdev_aio_submit_request가 -1이면(미지원 타입) 즉시 FAILED 보고.
 */
static void
bdev_aio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_aio_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * bdev_aio_io_type_supported - 모듈이 어떤 I/O 타입을 지원하는지 응답
 *
 * @ctx: file_disk 포인터(인스턴스별로 fallocate 옵션이 달라 검사 필요).
 * @io_type: 질의 타입.
 * @return: true/false.
 */
static bool
bdev_aio_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct file_disk *fdisk = ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
		/* [한국어] 핵심 4종은 항상 지원. */
		return true;

	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* [한국어] fallocate 옵션이 켜져 있을 때만 지원. */
		return fdisk->fallocate;

	default:
		return false;
	}
}

#ifdef __FreeBSD__
/*
 * [한국어] (FreeBSD)
 * bdev_aio_create_io - 채널의 kqueue 생성
 */
static int
bdev_aio_create_io(struct bdev_aio_io_channel *ch)
{
	ch->kqfd = kqueue();
	/* [한국어] 새 kqueue fd. POSIX AIO 완료 통보 채널. */
	if (ch->kqfd < 0) {
		SPDK_ERRLOG("async I/O context setup failure: %s.\n", spdk_strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * [한국어] (FreeBSD)
 * bdev_aio_destroy_io - 채널의 kqueue 닫기
 */
static void
bdev_aio_destroy_io(struct bdev_aio_io_channel *ch)
{
	close(ch->kqfd);
	/* [한국어] kqueue fd 반납. */
}
#else
/*
 * [한국어] (Linux)
 * bdev_aio_create_io - 채널의 io_context_t 생성
 *
 * io_setup(2)는 fs.aio-max-nr 시스템 한도를 사용한다. 채널 수가 많아지면 여기서 실패할 수 있다.
 */
static int
bdev_aio_create_io(struct bdev_aio_io_channel *ch)
{
	if (io_setup(SPDK_AIO_QUEUE_DEPTH, &ch->io_ctx) < 0) {
		/* [한국어] 시스템 한도(fs.aio-max-nr) 초과가 가장 흔한 원인. 운영자에게 우회 방법 안내. */
		SPDK_ERRLOG("Async I/O context setup failure, likely due to exceeding kernel limit.\n");
		SPDK_ERRLOG("This limit may be increased using 'sysctl -w fs.aio-max-nr'.\n");
		return -1;
	}

	return 0;
}

/*
 * [한국어] (Linux)
 * bdev_aio_destroy_io - 채널의 io_context_t 해제
 */
static void
bdev_aio_destroy_io(struct bdev_aio_io_channel *ch)
{
	io_destroy(ch->io_ctx);
	/* [한국어] mmap된 ring 해제, 커널 리소스 반납. */
}
#endif

/*
 * [한국어]
 * bdev_aio_create_cb - 디스크 io_device의 코어 로컬 채널 생성 콜백
 *
 * @io_device: file_disk 포인터.
 * @ctx_buf: 코어가 할당한 sizeof(bdev_aio_io_channel) 메모리.
 *
 * 1) io context(kqueue 또는 io_setup) 생성.
 * 2) 그룹 채널(spdk_get_io_channel(&aio_if))을 가져와 자기 채널을 그 그룹의 io_ch_head에 추가.
 *    이렇게 하면 reactor당 단일 group poller가 본 채널까지 같이 폴링하게 된다.
 */
static int
bdev_aio_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_io_channel *ch = ctx_buf;
	int rc;

	rc = bdev_aio_create_io(ch);
	if (rc < 0) {
		/* [한국어] io context 생성 실패 — 채널 생성 자체 실패. */
		return rc;
	}

	ch->group_ch = spdk_io_channel_get_ctx(spdk_get_io_channel(&aio_if));
	/* [한국어] aio_if를 io_device 키로 등록된 그룹 채널을 reactor 별로 받음. ctx 포인터 직접 보관.
	 * 이 호출 자체가 reactor 첫 호출이면 group_create_cb가 트리거되어 group_channel을 만든다. */
	TAILQ_INSERT_TAIL(&ch->group_ch->io_ch_head, ch, link);
	/* [한국어] 그룹 폴러가 자기 채널을 폴링하도록 등록. */

	return 0;
}

/*
 * [한국어]
 * bdev_aio_destroy_cb - 디스크 io_device의 코어 로컬 채널 파괴 콜백
 */
static void
bdev_aio_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_io_channel *ch = ctx_buf;

	bdev_aio_destroy_io(ch);
	/* [한국어] io_context 해제. */

	assert(ch->group_ch);
	/* [한국어] create_cb가 정상 동작했다면 항상 비-NULL. */
	TAILQ_REMOVE(&ch->group_ch->io_ch_head, ch, link);
	/* [한국어] 그룹 폴링 리스트에서 제거. */

	spdk_put_io_channel(spdk_io_channel_from_ctx(ch->group_ch));
	/* [한국어] create 시 spdk_get_io_channel로 잡았던 그룹 채널 참조를 반납. group_channel은
	 * 마지막 io 채널이 사라지면 destroy_cb로 함께 해제됨. */
}

/*
 * [한국어]
 * bdev_aio_get_io_channel - 채널 획득 (fn_table.get_io_channel)
 *
 * 디스크별 io_device는 file_disk 주소이므로 spdk_get_io_channel(fdisk)을 호출.
 */
static struct spdk_io_channel *
bdev_aio_get_io_channel(void *ctx)
{
	struct file_disk *fdisk = ctx;

	return spdk_get_io_channel(fdisk);
}


/*
 * [한국어]
 * bdev_aio_dump_info_json - bdev 추가 정보를 JSON으로 출력 (fn_table.dump_info_json)
 *
 * RPC bdev_get_bdevs 응답에 모듈별 정보 객체로 포함된다. 사용자에게 백엔드 파일 경로,
 * 옵션 플래그를 보여주는 진단 용도.
 */
static int
bdev_aio_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct file_disk *fdisk = ctx;

	spdk_json_write_named_object_begin(w, "aio");
	/* [한국어] "aio" 키 아래에 옵션들을 객체로 출력. */
	spdk_json_write_named_string(w, "filename", fdisk->filename);
	spdk_json_write_named_bool(w, "block_size_override", fdisk->block_size_override);
	spdk_json_write_named_bool(w, "readonly", fdisk->readonly);
	spdk_json_write_named_bool(w, "fallocate", fdisk->fallocate);
	spdk_json_write_named_bool(w, "nowait", fdisk->use_nowait);
	spdk_json_write_object_end(w);
	return 0;
}

/*
 * [한국어]
 * bdev_aio_write_json_config - RPC 재현 명령을 JSON으로 직렬화 (fn_table.write_config_json)
 */
static void
bdev_aio_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct file_disk *fdisk = fdisk_from_bdev(bdev);
	const struct spdk_uuid *uuid = spdk_bdev_get_uuid(bdev);
	/* [한국어] uuid는 bdev에서 직접 가져옴(생성 시 설정 또는 자동 생성). */

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "bdev_aio_create");
	/* [한국어] 재현용 RPC 메서드. */

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	if (fdisk->block_size_override) {
		/* [한국어] 사용자가 block_size를 명시한 경우만 출력 — 자동 감지로 만든 디스크에서는 생략. */
		spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	}
	spdk_json_write_named_string(w, "filename", fdisk->filename);
	spdk_json_write_named_bool(w, "readonly", fdisk->readonly);
	spdk_json_write_named_bool(w, "fallocate", fdisk->fallocate);
	if (!spdk_uuid_is_null(uuid)) {
		/* [한국어] 자동 생성된 UUID도 보존. NULL이면 생략. */
		spdk_json_write_named_uuid(w, "uuid", uuid);
	}
	spdk_json_write_named_bool(w, "nowait", fdisk->use_nowait);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

/*
 * [한국어] aio_fn_table — bdev core가 호출할 콜백 묶음.
 */
static const struct spdk_bdev_fn_table aio_fn_table = {
	.destruct		= bdev_aio_destruct,
	/* [한국어] bdev 해제 시 호출. async cleanup. */
	.submit_request		= bdev_aio_submit_request,
	/* [한국어] I/O 진입점. */
	.io_type_supported	= bdev_aio_io_type_supported,
	/* [한국어] 지원 I/O 종류 질의. */
	.get_io_channel		= bdev_aio_get_io_channel,
	/* [한국어] 채널 획득. */
	.dump_info_json		= bdev_aio_dump_info_json,
	/* [한국어] 진단 정보 직렬화. */
	.write_config_json	= bdev_aio_write_json_config,
	/* [한국어] config save. */
};

/*
 * [한국어]
 * aio_free_disk - file_disk 메모리 일괄 해제
 */
static void
aio_free_disk(struct file_disk *fdisk)
{
	if (fdisk == NULL) {
		/* [한국어] NULL-safe — create 도중 실패 경로에서 호출 가능성. */
		return;
	}
	free(fdisk->filename);
	/* [한국어] strdup된 파일 경로. */
	free(fdisk->disk.name);
	/* [한국어] strdup된 bdev 이름. */
	free(fdisk);
	/* [한국어] 본체. */
}

/*
 * [한국어]
 * bdev_aio_register_interrupt - 그룹 채널에 eventfd 기반 인터럽트 핸들러 등록
 *
 * @ch: 대상 그룹 채널.
 * @return: 0 성공, -1 실패.
 *
 * eventfd(2)로 fd를 만들고 SPDK_INTERRUPT_REGISTER로 핸들러 연결. EFD_NONBLOCK으로 read가
 * 블록되지 않게 하고 EFD_CLOEXEC로 fork 시 누수 방지.
 */
static int
bdev_aio_register_interrupt(struct bdev_aio_group_channel *ch)
{
	int efd;

	efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	/* [한국어] 시스템 호출: 새 eventfd 생성. 초기 카운터 0. */
	if (efd < 0) {
		return -1;
	}

	ch->intr = SPDK_INTERRUPT_REGISTER(efd, bdev_aio_group_interrupt, ch);
	/* [한국어] efd가 깨어나면 bdev_aio_group_interrupt가 호출되도록 등록. ch가 인자로 전달됨. */
	if (ch->intr == NULL) {
		/* [한국어] 등록 실패 — fd만 닫고 종료. */
		close(efd);
		return -1;
	}
	ch->efd = efd;
	/* [한국어] iocb에 묶을 fd 저장. */

	return 0;
}

/*
 * [한국어]
 * bdev_aio_unregister_interrupt - 인터럽트 자원 해제
 */
static void
bdev_aio_unregister_interrupt(struct bdev_aio_group_channel *ch)
{
	spdk_interrupt_unregister(&ch->intr);
	/* [한국어] 핸들러 등록 해제. */
	close(ch->efd);
	/* [한국어] eventfd close. */
	ch->efd = -1;
	/* [한국어] 무효화 표시. */
}

/*
 * [한국어]
 * bdev_aio_group_create_cb - 모듈 io_device(=&aio_if)의 reactor당 그룹 채널 생성
 *
 * 핵심: 한 reactor에 여러 AIO 디스크가 있어도 group poller는 1개. 인터럽트 모드면 추가로
 * eventfd 기반 핸들러도 등록.
 */
static int
bdev_aio_group_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_group_channel *ch = ctx_buf;
	int rc;

	TAILQ_INIT(&ch->io_ch_head);
	/* [한국어] 그룹에 묶일 io 채널 리스트 초기화. */
	/* Initialize ch->efd to be invalid and unused. */
	/* [한국어] 위 영문 주석 부연: 폴드 모드 기본. 인터럽트 모드일 때만 register가 efd를 채움. */
	ch->efd = -1;
	if (spdk_interrupt_mode_is_enabled()) {
		/* [한국어] 사용자가 spdk_interrupt_mode_enable을 호출한 경우(저전력 모드 등). */
		rc = bdev_aio_register_interrupt(ch);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to prepare intr resource to bdev_aio\n");
			return rc;
		}
	}

	ch->poller = SPDK_POLLER_REGISTER(bdev_aio_group_poll, ch, 0);
	/* [한국어] 폴드 모드 폴러 등록. period=0 → 매 tick 호출. */
	spdk_poller_register_interrupt(ch->poller, NULL, NULL);
	/* [한국어] 인터럽트 모드 전환을 지원하기 위해 poller에 인터럽트 콜백을 비워서 등록(현재
	 * 별도 set/clr fn은 없음). */

	return 0;
}

/*
 * [한국어]
 * bdev_aio_group_destroy_cb - 그룹 채널 파괴 콜백
 */
static void
bdev_aio_group_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_group_channel *ch = ctx_buf;

	if (!TAILQ_EMPTY(&ch->io_ch_head)) {
		/* [한국어] 정상이라면 모든 io 채널이 먼저 사라진 뒤 그룹 채널이 destroy 되어야 함.
		 * 그렇지 않다면 잠재적 leak 또는 unregister 순서 문제 — 경고만 출력. */
		SPDK_ERRLOG("Group channel of bdev aio has uncleared io channel\n");
	}

	spdk_poller_unregister(&ch->poller);
	/* [한국어] 폴러 해제. */
	if (spdk_interrupt_mode_is_enabled()) {
		bdev_aio_unregister_interrupt(ch);
		/* [한국어] eventfd 자원 정리. */
	}
}

/*
 * [한국어]
 * create_aio_bdev - RPC가 호출하는 AIO bdev 인스턴스 생성 (공개 API)
 *
 * @name: 새 bdev 이름.
 * @filename: 백엔드 파일/디바이스 경로.
 * @block_size: 블록 크기. 0이면 자동 감지.
 * @readonly: read-only 디스크로 만들지 여부.
 * @fallocate: UNMAP/WRITE_ZEROES를 fallocate로 지원할지(Linux 전용).
 * @uuid: 명시 UUID(선택).
 * @nowait: RWF_NOWAIT 사용 여부.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 단계:
 *   1) FreeBSD에서 fallocate 거절.
 *   2) file_disk 할당, name/filename strdup.
 *   3) open(O_DIRECT[|RDWR]).
 *   4) 디스크 크기/블록 크기 자동 감지(spdk_fd_get_size/blocklen).
 *   5) 사용자가 block_size를 지정하면 검증, 아니면 감지값 사용.
 *   6) blocklen이 512 이상 + 2의 거듭제곱인지 검증.
 *   7) required_alignment 설정(O_DIRECT 정렬 요구).
 *   8) blockcnt = disk_size / blocklen.
 *   9) io_device 등록 + bdev 등록 + 글로벌 리스트 추가.
 *
 * 호출 컨텍스트: RPC 처리 스레드(보통 main).
 */
int
create_aio_bdev(const char *name, const char *filename, uint32_t block_size, bool readonly,
		bool fallocate, const struct spdk_uuid *uuid, bool nowait)
{
	struct file_disk *fdisk;
	/* [한국어] 새 인스턴스 임시 포인터. */
	uint32_t detected_block_size;
	/* [한국어] OS가 알려주는 논리 블록 크기. */
	uint64_t disk_size;
	/* [한국어] 디스크/파일 총 바이트 크기. */
	int rc;

#ifdef __FreeBSD__
	if (fallocate) {
		/* [한국어] FreeBSD에는 Linux fallocate(2) 직접 대응이 없음 → 옵션 거절. */
		SPDK_ERRLOG("Unable to support fallocate on this platform\n");
		return -ENOTSUP;
	}
#endif

	fdisk = calloc(1, sizeof(*fdisk));
	/* [한국어] 0으로 초기화된 인스턴스. fd=0이 의미를 갖지 않도록 곧 -1로 설정. */
	if (!fdisk) {
		SPDK_ERRLOG("Unable to allocate enough memory for aio backend\n");
		return -ENOMEM;
	}
	fdisk->readonly = readonly;
	/* [한국어] 옵션 그대로 보관. */
	fdisk->fallocate = fallocate;
	fdisk->fd = -1;
	/* [한국어] 명시적으로 닫힌 상태 표시. open 실패 시 destruct에서 close 안 하도록. */

	fdisk->filename = strdup(filename);
	/* [한국어] 사용자 인자를 모듈이 소유하기 위해 사본. */
	if (!fdisk->filename) {
		rc = -ENOMEM;
		goto error_return;
	}

	fdisk->disk.name = strdup(name);
	/* [한국어] bdev 이름 사본. */
	if (!fdisk->disk.name) {
		rc = -ENOMEM;
		goto error_return;
	}

	if (bdev_aio_open(fdisk, nowait)) {
		/* [한국어] open 실패 — errno에 원인. */
		rc = -errno;
		goto error_return;
	}

	disk_size = spdk_fd_get_size(fdisk->fd);
	/* [한국어] 일반 파일이면 stat::st_size, 블록 디바이스면 BLKGETSIZE64 ioctl로 회수. */

	fdisk->disk.product_name = "AIO disk";
	/* [한국어] bdev_get_bdevs 등에서 표시될 product 이름. */
	fdisk->disk.module = &aio_if;
	/* [한국어] 모듈 포인터 — unregister_by_name 모듈 검사에 사용. */

	fdisk->disk.write_cache = 1;
	/* [한국어] AIO 백엔드는 page cache(O_DIRECT 미사용 폴백 시) 또는 device write cache가
	 * 있을 수 있으므로 write_cache=1로 보고 — FLUSH가 의미를 가짐을 알림. */

	detected_block_size = spdk_fd_get_blocklen(fdisk->fd);
	/* [한국어] 일반 파일이면 stat::st_blksize 또는 fs hint, 블록 디바이스면 BLKSSZGET ioctl. */
	if (block_size == 0) {
		/* User did not specify block size - use autodetected block size. */
		/* [한국어] 위 영문 주석 부연: 사용자가 0을 줬으면 자동 감지값 채택. */
		if (detected_block_size == 0) {
			/* [한국어] 자동 감지도 실패 — 디바이스를 식별할 수 없음. */
			AIO_FDISK_ERRLOG(fdisk, "Block size could not be auto-detected\n");
			rc = -EINVAL;
			goto error_return;
		}
		fdisk->block_size_override = false;
		/* [한국어] config_json 출력에서 block_size를 생략하기 위해 false로 표시. */
		block_size = detected_block_size;
	} else {
		if (block_size < detected_block_size) {
			/* [한국어] 사용자가 지정한 값이 디바이스 sector 크기보다 작으면 unaligned I/O 위험. */
			AIO_FDISK_ERRLOG(fdisk, "Specified block size %" PRIu32 " is smaller than "
					 "auto-detected block size %" PRIu32 "\n",
					 block_size, detected_block_size);
			rc = -EINVAL;
			goto error_return;
		} else if (detected_block_size != 0 && block_size != detected_block_size) {
			/* [한국어] 일치하진 않지만 더 큰 값 — 정렬 측면에서는 안전. 경고만 출력하고 진행. */
			AIO_FDISK_ERRLOG(fdisk, "Specified block size %" PRIu32 " does not match "
					 "auto-detected block size %" PRIu32 "\n",
					 block_size, detected_block_size);
		}
		fdisk->block_size_override = true;
		/* [한국어] config_json에서 block_size를 출력해 재현 가능하도록. */
	}

	if (block_size < 512) {
		/* [한국어] 전통적 sector 최소값 512B 미만은 거절. */
		AIO_FDISK_ERRLOG(fdisk, "Invalid block size %" PRIu32 " (must be at least 512).\n", block_size);
		rc = -EINVAL;
		goto error_return;
	}

	if (!spdk_u32_is_pow2(block_size)) {
		/* [한국어] required_alignment를 log2로 계산하므로 2의 거듭제곱이어야 함. */
		AIO_FDISK_ERRLOG(fdisk, "Invalid block size %" PRIu32 " (must be a power of 2.)\n", block_size);
		rc = -EINVAL;
		goto error_return;
	}

	fdisk->disk.blocklen = block_size;
	/* [한국어] 결정된 블록 크기 저장. */
	if (fdisk->block_size_override && detected_block_size) {
		/* [한국어] override 시 정렬은 실제 디바이스 sector 기준이 안전. */
		fdisk->disk.required_alignment = spdk_u32log2(detected_block_size);
	} else {
		fdisk->disk.required_alignment = spdk_u32log2(block_size);
	}
	/* [한국어] required_alignment는 log2 표현. O_DIRECT를 만족시키기 위해 spdk_bdev_io_get_buf가
	 * 이 값에 맞춰 정렬된 bounce buffer를 빌려준다. */

	if (disk_size % fdisk->disk.blocklen != 0) {
		/* [한국어] 디스크 크기가 블록 크기 배수가 아니면 마지막 부분 블록을 안전하게 다룰 수 없음. */
		AIO_FDISK_ERRLOG(fdisk, "Disk size %" PRIu64 " is not a multiple of block size %" PRIu32 "\n",
				 disk_size, fdisk->disk.blocklen);
		rc = -EINVAL;
		goto error_return;
	}

	fdisk->disk.blockcnt = disk_size / fdisk->disk.blocklen;
	/* [한국어] 사용 가능한 블록 수. */
	fdisk->disk.ctxt = fdisk;
	/* [한국어] fn_table 콜백 첫 인자. */
	if (uuid) {
		/* [한국어] 사용자 UUID 지정 시 복사. NULL이면 register가 자동 생성. */
		spdk_uuid_copy(&fdisk->disk.uuid, uuid);
	}

	fdisk->disk.fn_table = &aio_fn_table;
	/* [한국어] 콜백 묶음 연결. */

	spdk_io_device_register(fdisk, bdev_aio_create_cb, bdev_aio_destroy_cb,
				sizeof(struct bdev_aio_io_channel),
				fdisk->disk.name);
	/* [한국어] 디스크별 io_device 등록. 채널은 reactor마다 1개 + sizeof(io_channel) 크기. */
	rc = spdk_bdev_register(&fdisk->disk);
	if (rc) {
		/* [한국어] 등록 실패(이름 충돌 등) → io_device도 되돌림. */
		spdk_io_device_unregister(fdisk, NULL);
		goto error_return;
	}

	TAILQ_INSERT_TAIL(&g_aio_disk_head, fdisk, link);
	/* [한국어] 글로벌 리스트에 등록. */
	return 0;

error_return:
	bdev_aio_close(fdisk);
	/* [한국어] open 했다면 close. fd=-1이면 no-op. */
	aio_free_disk(fdisk);
	/* [한국어] 메모리 일괄 해제. */
	return rc;
}

/*
 * [한국어]
 * dummy_bdev_event_cb - rescan에서 spdk_bdev_open_ext에 전달하는 빈 이벤트 콜백
 *
 * @type, @bdev, @ctx: 사용하지 않음. open_ext API가 콜백을 강제로 요구하기 때문.
 */
static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
	/* [한국어] no-op. rescan은 짧은 시간 동안만 desc를 잡아둔다. */
}

/*
 * [한국어]
 * bdev_aio_rescan - 백엔드 디스크 크기 변동을 다시 읽어 bdev에 반영 (공개 API)
 *
 * @name: 대상 bdev 이름.
 * @return: 0 성공 또는 hot-remove 트리거됨, 음수 errno 실패.
 *
 * 시나리오: 운영자가 백엔드 블록 디바이스(예: lvm)의 크기를 늘렸을 때, SPDK가 bdev->blockcnt를
 * 새로 인지하도록 재감지를 트리거. disk_size가 0으로 떨어졌으면 hot-remove로 간주.
 */
int
bdev_aio_rescan(const char *name)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	struct file_disk *fdisk;
	uint64_t disk_size, blockcnt;
	int rc;

	rc = spdk_bdev_open_ext(name, false, dummy_bdev_event_cb, NULL, &desc);
	/* [한국어] read-only로 잠깐 열어 desc 획득. */
	if (rc != 0) {
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);
	if (bdev->module != &aio_if) {
		/* [한국어] AIO 모듈 소속이 아니면 거절. */
		rc = -ENODEV;
		goto exit;
	}

	fdisk = fdisk_from_bdev(bdev);
	disk_size = spdk_fd_get_size(fdisk->fd);
	/* [한국어] 현재 디바이스 크기 재감지. */
	blockcnt = disk_size / bdev->blocklen;
	/* [한국어] 새 블록 수. */

	if (disk_size == 0) {
		/* [한국어] 0 → 디바이스가 분리됨(hot-removal). hot_remove 경로로 위임. */
		bdev_aio_try_hot_remove(fdisk);
		goto exit;
	}

	if (bdev->blockcnt != blockcnt) {
		/* [한국어] 크기 변화 감지 → bdev core에 알리고 사용자 EVENT_RESIZE 콜백 깨움. */
		AIO_FDISK_NOTICELOG(fdisk, "device is resized: old block count %" PRIu64 ", new block count %"
				    PRIu64 "\n",
				    bdev->blockcnt,
				    blockcnt);
		rc = spdk_bdev_notify_blockcnt_change(bdev, blockcnt);
		if (rc != 0) {
			AIO_FDISK_ERRLOG(fdisk, "Could not change num blocks, errno: %d.\n", rc);
			goto exit;
		}
	}

exit:
	spdk_bdev_close(desc);
	return rc;
}

/*
 * [한국어] struct delete_aio_bdev_ctx
 *
 * delete API의 콜백 컨텍스트. 사용자 콜백+인자 보관.
 */
struct delete_aio_bdev_ctx {
	delete_aio_bdev_complete cb_fn;
	/* [한국어] 사용자 완료 콜백. */
	void *cb_arg;
	/* [한국어] 콜백 첫 인자. */
};

/*
 * [한국어]
 * aio_bdev_unregister_cb - unregister 완료 콜백
 *
 * @arg: delete_aio_bdev_ctx 포인터.
 * @bdeverrno: unregister 결과 코드.
 *
 * 사용자 cb_fn이 있으면 호출하고 ctx를 free. 본 함수가 spdk_bdev_unregister_by_name의 비동기
 * 완료 통보 지점.
 */
static void
aio_bdev_unregister_cb(void *arg, int bdeverrno)
{
	struct delete_aio_bdev_ctx *ctx = arg;

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, bdeverrno);
	}
	free(ctx);
}

/*
 * [한국어]
 * bdev_aio_delete - RPC가 호출하는 AIO bdev 삭제 (공개 API)
 *
 * @name: 대상 bdev 이름.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 인자.
 *
 * app thread에서만 호출 가능(assertion). ctx를 alloc해 spdk_bdev_unregister_by_name에 전달.
 * unregister가 비동기로 진행되며 마지막에 aio_bdev_unregister_cb가 호출된다.
 */
void
bdev_aio_delete(const char *name, delete_aio_bdev_complete cb_fn, void *cb_arg)
{
	struct delete_aio_bdev_ctx *ctx;
	int rc;

	assert(spdk_thread_is_app_thread(NULL));
	/* [한국어] 안전성 보장: app thread(보통 main)에서만 호출. 다른 thread에서 들어오면 race. */

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		/* [한국어] OOM — 사용자 콜백이 있으면 즉시 -ENOMEM 통보. */
		if (cb_fn) {
			cb_fn(cb_arg, -ENOMEM);
		}
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	rc = spdk_bdev_unregister_by_name(name, &aio_if, aio_bdev_unregister_cb, ctx);
	/* [한국어] 비동기 unregister 시작. 성공 시 ctx는 콜백에서 free. */
	if (rc != 0) {
		/* [한국어] 즉시 실패(이름 없음 등) → 콜백을 직접 호출해 정리. */
		aio_bdev_unregister_cb(ctx, rc);
	}
}

/*
 * [한국어]
 * bdev_aio_initialize - 모듈 init (spdk_bdev_module.module_init)
 *
 * 모듈 자체를 io_device로 등록 → group channel이 reactor당 1개씩 자동 생성. 디스크별 io_device는
 * create_aio_bdev에서 별도 등록.
 */
static int
bdev_aio_initialize(void)
{
	spdk_io_device_register(&aio_if, bdev_aio_group_create_cb, bdev_aio_group_destroy_cb,
				sizeof(struct bdev_aio_group_channel), "aio_module");
	/* [한국어] aio_if 주소를 키로 사용. 진단 이름 "aio_module". */

	return 0;
}

/*
 * [한국어]
 * bdev_aio_fini - 모듈 fini
 *
 * 그룹 io_device 해제. async_fini가 false이므로 동기 종료(콜백 NULL).
 */
static void
bdev_aio_fini(void)
{
	spdk_io_device_unregister(&aio_if, NULL);
	/* [한국어] 모든 그룹 채널 destroy 후 unregister 완료(콜백 미등록 → 동기 종료 가정). */
}

SPDK_LOG_REGISTER_COMPONENT(aio)
/* [한국어] "aio" 로그 컴포넌트 등록. SPDK_INFOLOG/SPDK_DEBUGLOG의 컴포넌트 필터링에 사용. */
