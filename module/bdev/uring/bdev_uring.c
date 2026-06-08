/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Linux io_uring 기반 bdev 백엔드 모듈 (bdev_uring.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Linux 5.1+ 의 io_uring 인터페이스를 사용하여 일반 파일 또는 블록 디바이스
 * 노드(/dev/sdX, /dev/nvmeXnY 등)에 대해 비동기 I/O 를 수행하는 SPDK bdev 백엔드를
 * 구현한다. 사용자가 RPC bdev_uring_create 를 호출하면 호스트의 파일 시스템 경로를
 * O_DIRECT 로 열고 spdk_bdev 로 등록한다. SPDK 의 polled-mode 실행 모델 위에서
 * io_uring 의 SQ(Submission Queue) 에 SQE 를 채우고 io_uring_submit() 으로 일괄
 * 제출, 매 폴러 틱마다 CQ(Completion Queue) 의 CQE 를 peek 하여 완료를 spdk_bdev_io
 * 단위로 보고하는 구조이다. AIO 백엔드 대비 io_uring 은 syscall 한 번에 대량의 SQE 를
 * 제출할 수 있고, IORING_FEAT_FAST_POLL 등을 활용하면 hot path 에서 syscall 0회
 * (커널 쓰레드와의 메모리 공유) 까지 줄일 수 있어 SPDK polled-mode 와 잘 어울린다.
 * SPDK_CONFIG_URING_ZNS 가 정의되면 Zoned Namespace 디바이스의 zone management /
 * report 도 ioctl(BLKREPORTZONE 등) 로 우회 지원한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 의 I/O 스택에서 이 파일은 다음 위치에 자리한다:
 *   [Application] → spdk_bdev_read/write
 *   → [lib/bdev: 공통 bdev 코어] (submit_request)
 *   → [본 모듈: bdev_uring] (per-io_channel group_ch->uring 에 SQE enqueue)
 *   → io_uring_submit() → [Linux 커널 io_uring 백엔드]
 *   → [VFS/블록 레이어 → SCSI/NVMe 드라이버 → 디스크]
 * 실행 컨텍스트는 전부 호스트 유저스페이스 SPDK reactor 스레드이다. io_uring 인스턴스는
 * "그룹 채널(bdev_uring_group_channel)" 단위로 하나씩 두며, 같은 reactor 위의 모든
 * uring bdev 가 그 io_uring 을 공유한다 (모듈 io_device 채널). 따라서 thread-affinity
 * lockless 설계가 성립한다 — 한 io_uring 인스턴스는 단일 SPDK 스레드에서만 접근된다.
 * bdev 인스턴스 단위 채널(bdev_uring_io_channel) 은 단지 group_ch 포인터를 들고 있는
 * 얇은 래퍼이다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(호출자): lib/bdev (spdk_bdev_io 의 submit_request 가 bdev_uring_submit_request
 *   를 호출). RPC 계층 bdev_uring_rpc.c 가 bdev_uring_create/delete/rescan 진입점을 제공.
 * - 하위(피호출): spdk_internal/uring.h 가 wrap 한 liburing API (io_uring_queue_init,
 *   io_uring_get_sqe, io_uring_prep_readv/writev, io_uring_submit, io_uring_peek_cqe 등)
 *   와 Linux 커널 시스템 호출(open, close, ioctl BLKGETSIZE64/BLKREPORTZONE/BLK*ZONE).
 * - 데이터 흐름: 호출자가 전달한 spdk_bdev_io 의 driver_ctx 영역(bdev_uring_task) 을
 *   SQE 의 user_data 로 박아 넣고, 완료 시 CQE→user_data 로 다시 복원하여
 *   spdk_bdev_io_complete() 로 상위에 통보한다.
 * - 공유 자료구조: g_uring_bdev_head 전역 TAILQ (생성된 uring bdev 목록, app thread 에서만
 *   접근하는 것이 정상), 모듈 io_device 의 group_ch (per-reactor io_uring).
 *
 * === 주요 함수/구조체 요약 ===
 * - create_uring_bdev / delete_uring_bdev: RPC 핸들러가 부르는 생성/삭제 진입.
 * - bdev_uring_open / bdev_uring_close: O_DIRECT|O_NOATIME 으로 파일 fd 획득/반환.
 * - bdev_uring_readv / bdev_uring_writev: SQE 채워서 io_pending 카운터만 증가.
 * - bdev_uring_group_poll: 폴러 본체. io_uring_submit() → bdev_uring_reap() 순.
 * - bdev_uring_reap: CQE 를 peek 하여 완료된 I/O 를 spdk_bdev_io_complete 로 보고.
 * - bdev_uring_check_zoned_support / zone_management_op / zone_get_info: ZNS 지원.
 * - struct bdev_uring: SPDK bdev 객체 + filename/fd 보관.
 * - struct bdev_uring_group_channel: reactor 단위 io_uring 인스턴스와 카운터.
 * - struct bdev_uring_task: spdk_bdev_io->driver_ctx 영역에 박혀서 CQE 식별자로 쓰임.
 */

#include "bdev_uring.h"  /* [한국어] 본 모듈의 공개 API (create_uring_bdev/delete_uring_bdev/bdev_uring_rescan/bdev_uring_opts) 와 spdk_delete_uring_complete typedef 선언. RPC 핸들러 (bdev_uring_rpc.c) 와 본 파일이 공유. */

#include "spdk/stdinc.h"          /* [한국어] SPDK 표준 인클루드 (stddef/stdint/string/errno 등 cross-platform 모음). */
#include "spdk/config.h"          /* [한국어] 빌드 시 generate 된 컴파일 옵션 매크로 (SPDK_CONFIG_URING_ZNS 등). */
#include "spdk/barrier.h"         /* [한국어] 컴파일러/CPU memory barrier 매크로 (현 파일에서는 io_uring liburing 내부에서 사용). */
#include "spdk/bdev.h"            /* [한국어] spdk_bdev API — 본 모듈이 노출할 블록 디바이스 추상화. */
#include "spdk/env.h"             /* [한국어] SPDK env (DPDK 추상화) — 메모리/CPU 관련 API. */
#include "spdk/fd.h"              /* [한국어] spdk_fd_get_size / spdk_fd_get_blocklen — 일반 파일/블록 디바이스의 크기/블록 사이즈 조회. */
#include "spdk/likely.h"          /* [한국어] spdk_likely/spdk_unlikely (branch hint) — bdev_uring_reap 의 빠른 경로에서 사용. */
#include "spdk/thread.h"          /* [한국어] spdk_thread / spdk_poller / spdk_io_channel — reactor 위 폴러 등록 및 채널 ctx 획득. */
#include "spdk/json.h"            /* [한국어] dump_info_json / write_json_config 에서 JSON 직렬화 컨텍스트 사용. */
#include "spdk/util.h"            /* [한국어] SPDK_CONTAINEROF, spdk_u32_is_pow2, spdk_u32log2 등 유틸 매크로/함수. */
#include "spdk/string.h"          /* [한국어] spdk_strerror, spdk_sprintf_alloc — errno 문자열화 및 동적 문자열 포맷. */
#include "spdk/file.h"            /* [한국어] spdk_read_sysfs_attribute — ZNS sysfs 노드(queue/zoned 등) 파싱. */

#include "spdk/log.h"             /* [한국어] SPDK_ERRLOG/NOTICELOG/DEBUGLOG 매크로 — 본 모듈 로그 전부 여기에 의존. */
#include "spdk_internal/uring.h"  /* [한국어] SPDK 가 liburing 위에 wrap 한 internal 헤더. io_uring_get_sqe/io_uring_submit 등 모든 uring API 본체. */

#ifdef SPDK_CONFIG_URING_ZNS
#include <linux/blkzoned.h>       /* [한국어] Linux 커널의 zoned block device UAPI — blk_zone, BLKREPORTZONE/BLK*ZONE ioctl. */
#define SECTOR_SHIFT 9            /* [한국어] Linux 블록 레이어 기준 1 sector = 512B = 1<<9. ZNS ioctl 이 sector 단위로 동작하므로 LBA shift 계산용. */
#endif

/* [한국어] 모든 로그 라인에 공통으로 붙는 prefix 포맷. bdev 이름, struct 포인터, 백킹 파일 이름을 한꺼번에 노출하여 멀티-bdev 환경에서 어느 인스턴스의 로그인지 구분. */
#define URING_LOG_FMT "%s,uring:%p,filename:%s"
/* [한국어] URING_LOG_FMT 의 % 들에 대응하는 인자 튜플. 매크로 인라이닝으로 zero-cost. */
#define URING_LOG_ARGS(uring) \
  (uring)->bdev.name, \
  (uring), \
  (uring)->filename

/* [한국어] 기본 로그 매크로 — SPDK_##type##LOG (예: SPDK_ERRLOG) 에 URING_LOG_FMT prefix 를 자동으로 붙여준다.
 * do{}while(0) 으로 감싸 단일 명령처럼 사용 가능 (if 문 내 안전성 확보). */
#define URING_LOG(type, uring, format, ...) do { \
	SPDK_##type##LOG("["URING_LOG_FMT"] " format, URING_LOG_ARGS(uring), ##__VA_ARGS__); \
} while (0)

/* [한국어] component 인자가 필요한 로그 (INFOLOG/DEBUGLOG 처럼 SPDK_LOG_REGISTER_COMPONENT 로 등록된 카테고리를 동적으로 토글) 용 매크로. */
#define URING_LOG2(type, component, uring, format, ...) do { \
	SPDK_##type##LOG(component, "["URING_LOG_FMT"] " format, URING_LOG_ARGS(uring), ##__VA_ARGS__); \
} while (0)

#define URING_ERRLOG(uring, format, ...) URING_LOG(ERR, uring, format, ##__VA_ARGS__)         /* [한국어] 에러 로그 단축. */
#define URING_WARNLOG(uring, format, ...) URING_LOG(WARN, uring, format, ##__VA_ARGS__)       /* [한국어] 경고 로그 단축. */
#define URING_NOTICELOG(uring, format, ...) URING_LOG(NOTICE, uring, format, ##__VA_ARGS__)   /* [한국어] 일반 통지 로그 단축. */
#define URING_INFOLOG(uring, format, ...) URING_LOG2(INFO, uring, uring, format, ##__VA_ARGS__) /* [한국어] info 로그 (component='uring' 로 등록된 채널). */

#ifdef DEBUG
/* [한국어] 디버그 빌드(DEBUG 매크로 정의) 시에만 활성화되는 debug 로그. component='uring' 으로 등록된 카테고리. */
#define URING_DEBUGLOG(uring, format, ...) URING_LOG2(DEBUG, uring, uring, format, ##__VA_ARGS__)
#else
/* [한국어] 비-디버그 빌드에서는 no-op. hot path (readv/writev) 에 들어가는 로그라 release 빌드에서 비용을 0으로 만들기 위함. */
#define URING_DEBUGLOG(...) do { } while (0)
#endif

/* [한국어] ZNS (Zoned Namespace) 디바이스 메타정보 캐시. SPDK_CONFIG_URING_ZNS 활성 시 의미가 있으며 비-ZNS 빌드에서도 빈 채로 존재. */
struct bdev_uring_zoned_dev {
	uint64_t		num_zones;
	/* [한국어] 디바이스 내 zone 총 개수.
	 * 설정자: bdev_uring_check_zoned_support() 가 ioctl(BLKGETNRZONES) 결과를 저장.
	 * 읽는 자: bdev_uring_zone_get_info() 가 입력으로 받은 num_zones 가 디바이스 한계를 넘지 않는지 검증.
	 * 값 범위: 디바이스가 보고하는 양의 정수. 비-ZNS bdev 에서는 0 으로 남는다.
	 * 동기화: 부팅 시 한 번 설정 후 read-only — 동기화 불필요. */

	uint32_t		zone_shift;
	/* [한국어] zone size 의 log2 값 (LBA 단위). zone_id 를 zone 번호로 변환할 때 (zone_id >> zone_shift) 형태로 사용.
	 * 설정자: bdev_uring_check_zoned_support() 가 spdk_u32log2(zinfo >> lba_shift) 로 계산해 저장.
	 * 읽는 자: bdev_uring_zone_get_info() 의 루프 종료 조건.
	 * 값 범위: zone size 가 2의 거듭제곱이라는 가정 (NVMe ZNS 스펙 통상값).
	 * 동기화: read-only after init. */

	uint32_t		lba_shift;
	/* [한국어] (bdev->required_alignment - SECTOR_SHIFT). SPDK LBA 단위와 Linux 블록 레이어 512B sector 단위 간 변환에 사용.
	 * 설정자: bdev_uring_check_zoned_support() 에서 결정.
	 * 읽는 자: zone_management_op (range.sector 계산), zone_get_info (write_pointer/capacity shift).
	 * 값 범위: 보통 0~7 (512B~64KB block). 0 이면 SPDK LBA == Linux sector.
	 * 동기화: read-only after init. */
};

/* [한국어] bdev 인스턴스 단위로 spdk_io_device_register 에 의해 할당되는 채널 컨텍스트.
 * 단지 reactor-wide group_ch 포인터를 들고 있는 얇은 래퍼이다 — 실제 io_uring 인스턴스는 group_ch 가 소유. */
struct bdev_uring_io_channel {
	struct bdev_uring_group_channel		*group_ch;
	/* [한국어] 같은 reactor 에 속한 모든 uring bdev 가 공유하는 그룹 채널.
	 * 설정자: bdev_uring_create_cb() 가 모듈 io_device 의 채널을 얻어와 저장.
	 * 읽는 자: bdev_uring_readv/writev 가 group_ch->uring 으로 SQE 를 얻고 group_ch->io_pending 증가.
	 * 값 범위: 유효 포인터 (NULL 불가, lifetime 은 채널 reference 동안).
	 * 동기화: 단일 reactor 스레드에서만 접근 — 락 불필요. */
};

/* [한국어] 모듈 자체에 등록된 io_device("uring_if") 의 채널. reactor 1개당 1개 존재하며 io_uring 인스턴스를 owning.
 * 같은 reactor 위 여러 uring bdev 가 같은 io_uring 을 공유하므로 SQ/CQ syscall 비용이 한 번에 amortize 된다. */
struct bdev_uring_group_channel {
	uint64_t				io_inflight;
	/* [한국어] 현재 io_uring 에 제출되었지만 아직 CQE 가 도착하지 않은 I/O 개수.
	 * 설정자: bdev_uring_group_poll() 이 submit 성공 시 to_submit 만큼 증가, bdev_uring_reap() 이 CQE 처리 시 감소.
	 * 읽는 자: bdev_uring_group_poll() 이 reap 할 최대 개수 결정.
	 * 값 범위: 0 ~ SPDK_URING_QUEUE_DEPTH (512).
	 * 동기화: 한 reactor 스레드에서만 접근. */

	uint64_t				io_pending;
	/* [한국어] SQE 는 채워졌지만 아직 io_uring_submit() 호출 전 — 제출 대기 큐의 크기.
	 * 설정자: readv/writev 가 SQE prep 후 1 증가.
	 * 읽는 자: bdev_uring_group_poll() 이 매 틱마다 to_submit 으로 읽고 0 으로 reset.
	 * 값 범위: 0 ~ SPDK_URING_QUEUE_DEPTH.
	 * 동기화: 단일 reactor. */

	struct spdk_poller			*poller;
	/* [한국어] bdev_uring_group_poll() 을 등록한 SPDK poller 핸들. spdk_poller_unregister() 시 필요.
	 * 설정자: bdev_uring_group_create_cb() 가 SPDK_POLLER_REGISTER 로 받음.
	 * 읽는 자: bdev_uring_group_destroy_cb() 가 unregister 시 사용.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: 단일 reactor 에서 lifetime 관리. */

	struct io_uring				uring;
	/* [한국어] liburing 의 io_uring 인스턴스 — SQ/CQ ring buffer 의 user-space mmap 핸들.
	 * 설정자: io_uring_queue_init(SPDK_URING_QUEUE_DEPTH, ...) 가 io_uring_setup(2) 시스템 호출로 초기화.
	 * 읽는 자: readv/writev 의 io_uring_get_sqe, group_poll 의 io_uring_submit/peek_cqe.
	 * 값 범위: 커널이 부여한 ring fd 와 mmap 영역을 가리키는 구조.
	 * 동기화: io_uring 은 단일 producer/consumer 가정에서 lockless — 본 모듈은 단일 reactor 가 producer & consumer. */
};

/* [한국어] 각 spdk_bdev_io 마다 driver_ctx 영역에 in-place 로 할당되는 per-IO 메타.
 * bdev_uring_get_ctx_size() 가 sizeof(struct bdev_uring_task) 를 보고하여 lib/bdev 가 공간을 예약해준다. */
struct bdev_uring_task {
	uint64_t			len;
	/* [한국어] 이번 I/O 가 요청한 바이트 길이 (num_blocks * blocklen).
	 * 설정자: bdev_uring_readv/writev 가 SQE 채울 때 저장.
	 * 읽는 자: bdev_uring_reap() 이 CQE->res 가 len 과 동일한지로 short read/write 판정.
	 * 값 범위: 양의 정수 (bdev->required_alignment 배수).
	 * 동기화: 한 IO 는 단일 reactor 만 다루므로 락 불필요. */

	struct bdev_uring_io_channel	*ch;
	/* [한국어] 이 IO 를 제출한 io_channel. 완료 시 group_ch->io_inflight 감소시키려고 보관.
	 * 설정자: readv/writev 가 io_channel ctx 를 박아 저장.
	 * 읽는 자: bdev_uring_reap() 이 io_inflight 카운터 갱신.
	 * 값 범위: 유효 채널 포인터 — 채널 lifetime 이 IO lifetime 보다 길도록 보장됨.
	 * 동기화: 단일 reactor. */

	TAILQ_ENTRY(bdev_uring_task)	link;
	/* [한국어] 향후 retry 큐 등에 사용 가능한 list 링크. 현재 코드 경로에서는 활성 사용 없음 (예비 필드).
	 * 설정자/읽는 자: TAILQ_INSERT/REMOVE 매크로 (현재 미사용).
	 * 값 범위: 표준 sys/queue TAILQ_ENTRY 구조.
	 * 동기화: 단일 reactor. */
};

/* [한국어] uring bdev 인스턴스의 모든 상태를 담은 메인 구조체. spdk_bdev 가 첫 멤버라
 * uring_from_bdev() 가 SPDK_CONTAINEROF 로 안전하게 변환할 수 있다. */
struct bdev_uring {
	struct spdk_bdev	bdev;
	/* [한국어] SPDK bdev 코어 객체. 반드시 첫 번째 멤버여야 SPDK_CONTAINEROF 로 base→derived 변환 가능.
	 * 설정자: create_uring_bdev() 가 name/blocklen/blockcnt/fn_table 등 채움.
	 * 읽는 자: lib/bdev 와 상위 모든 사용자.
	 * 값 범위: lib/bdev 가 정의하는 모든 필드.
	 * 동기화: spdk_bdev 자체의 thread model 을 따른다 (대개 app thread + I/O channels). */

	struct bdev_uring_zoned_dev	zd;
	/* [한국어] ZNS 메타 (위 §4 참조). 비-ZNS bdev 에서는 0 초기화 상태로 남는다.
	 * 설정자/읽는 자: bdev_uring_check_zoned_support 및 zone_* 함수들.
	 * 값 범위: 위 zoned_dev 구조 참조.
	 * 동기화: 부팅 시 1회 설정 후 read-only. */

	char			*filename;
	/* [한국어] 백킹 파일/디바이스의 경로 (예: "/dev/nvme0n1", "/var/tmp/file.bin"). strdup 한 소유 메모리.
	 * 설정자: create_uring_bdev() 가 opts->filename 을 strdup.
	 * 읽는 자: bdev_uring_open(open syscall 인자), dump_info_json, log 포맷.
	 * 값 범위: 유효 경로 문자열. NULL 이면 생성 실패.
	 * 동기화: 부팅 시 1회 설정 후 read-only. */

	int			fd;
	/* [한국어] open(2) 으로 획득한 파일 디스크립터. io_uring SQE 의 fd 필드로 사용.
	 * 설정자: bdev_uring_open() 가 open(O_RDWR|O_DIRECT|O_NOATIME) 결과 저장.
	 * 읽는 자: readv/writev 의 io_uring_prep_*, zone_* ioctl, hot-remove 시 spdk_fd_get_size.
	 * 값 범위: 유효 fd(>=0) 또는 -1 (닫힌 상태).
	 * 동기화: 부팅 시 설정 후 변하지 않음 (단 close 시 -1 로 마킹). */

	TAILQ_ENTRY(bdev_uring)  link;
	/* [한국어] 전역 g_uring_bdev_head 에 연결되는 링크. 모든 활성 uring bdev 의 list 멤버.
	 * 설정자/읽는 자: create_uring_bdev/destruct 가 INSERT/REMOVE.
	 * 값 범위: TAILQ_ENTRY 표준.
	 * 동기화: app thread 에서만 다뤄지는 것이 기대됨. */

	bool			hot_remove_in_progress;
	/* [한국어] hot-remove 가 진행 중인지 표시하는 single-set flag. atomic test_and_set 으로 중복 unregister 방지.
	 * 설정자: bdev_uring_try_hot_remove 가 __atomic_test_and_set 으로 set, 실패 시 clear.
	 * 읽는 자: 같은 함수 — 동시에 여러 reactor 가 hot-remove 를 검출했을 때 한 번만 unregister 발사하기 위한 가드.
	 * 값 범위: 0/1.
	 * 동기화: __ATOMIC_RELAXED 원자 연산 — 순서 보장은 약하지만 race-free 한 단일 게이트키퍼 역할만 필요. */
};

static int bdev_uring_init(void);                  /* [한국어] 모듈 초기화 forward decl — SPDK_BDEV_MODULE_REGISTER 에 함수 포인터로 전달. */
static void bdev_uring_fini(void);                 /* [한국어] 모듈 종료 forward decl. */
static void uring_free_bdev(struct bdev_uring *uring); /* [한국어] uring bdev 메모리 해제 forward decl — destruct/error path 양쪽에서 사용. */
/* [한국어] 모든 활성 uring bdev 의 전역 리스트. 모듈 fini 시 cleanup 이나 디버깅 시점 enumeration 목적.
 * INSERT_TAIL 은 create_uring_bdev, REMOVE 는 bdev_uring_destruct. app thread 가 정상 접근자. */
static TAILQ_HEAD(, bdev_uring) g_uring_bdev_head = TAILQ_HEAD_INITIALIZER(g_uring_bdev_head);

#define SPDK_URING_QUEUE_DEPTH 512  /* [한국어] io_uring_queue_init 에 전달할 SQ/CQ 엔트리 수. 512 는 일반적 NVMe outstanding I/O 와 균형 잡힌 값. 너무 크면 mmap 메모리 낭비, 너무 작으면 -ENOMEM (SQE 부족) 빈발. */
#define MAX_EVENTS_PER_POLL 32      /* [한국어] (현재 코드에서 직접 참조되지 않음 — 향후 reap 상한 또는 호환 용도) 한 폴 틱 당 처리할 CQE 최대치 가이드. */

/*
 * [한국어]
 * bdev_uring_get_ctx_size - lib/bdev 가 spdk_bdev_io 의 driver_ctx 로 예약할 바이트 수 반환.
 *
 * @return: sizeof(struct bdev_uring_task) — IO 당 메타데이터 크기.
 *
 * 본 모듈의 .get_ctx_size 콜백. lib/bdev 가 spdk_bdev_io 객체를 풀에서 꺼낼 때 모듈마다
 * 추가로 필요한 per-IO 영역을 contiguous 하게 할당하기 위해 사용된다.
 * 호출 컨텍스트: 모듈 등록 시점 (SPDK_BDEV_MODULE_REGISTER) 에서 한 번, 그리고 lib/bdev 가
 * IO 풀 사이즈 계산 시. 호출자 → lib/bdev. 피호출 없음.
 */
static int
bdev_uring_get_ctx_size(void)
{
	return sizeof(struct bdev_uring_task);  /* [한국어] driver_ctx 영역 크기 — 매 spdk_bdev_io 마다 이만큼 reserve. */
}

/*
 * [한국어]
 * uring_from_bdev - 공통 spdk_bdev 포인터 → uring 특화 컨테이너 변환.
 *
 * @bdev: lib/bdev 가 다루는 공통 bdev 포인터 (struct bdev_uring 의 첫 멤버).
 * @return: 해당 bdev 를 감싸는 struct bdev_uring 포인터.
 *
 * SPDK 의 표준 OOP-in-C 패턴: 모듈은 spdk_bdev 를 첫 멤버로 두고 SPDK_CONTAINEROF 로
 * 역참조한다. 따라서 bdev 가 본 모듈 소속(uring_if) 인 경우에만 호출되어야 한다.
 * 호출 컨텍스트: 모듈 내부 어디서나 (rescan/destruct/IO submit/reap 등).
 */
static struct bdev_uring *
uring_from_bdev(struct spdk_bdev *bdev)
{
	return SPDK_CONTAINEROF(bdev, struct bdev_uring, bdev);  /* [한국어] offsetof 기반 컨테이너 매크로. 컴파일 타임에 안전. */
}

/* [한국어] SPDK bdev 모듈 인터페이스 디스크립터. SPDK_BDEV_MODULE_REGISTER 매크로가 init 함수에서 lib/bdev 에 등록.
 * .name 은 RPC 와 로그에서 모듈 식별자, 나머지 콜백은 lib/bdev 가 적절한 시점에 호출. */
static struct spdk_bdev_module uring_if = {
	.name		= "uring",                /* [한국어] 모듈 이름. RPC bdev_uring_create 의 module 식별자. */
	.module_init	= bdev_uring_init,        /* [한국어] subsystem init 시 호출 — group_channel 용 io_device 등록. */
	.module_fini	= bdev_uring_fini,        /* [한국어] subsystem fini 시 호출 — io_device 해제. */
	.get_ctx_size	= bdev_uring_get_ctx_size,/* [한국어] spdk_bdev_io 의 driver_ctx 크기 (bdev_uring_task). */
};

/* [한국어] SPDK 부팅 시 정적 초기화로 모듈 자기 자신을 bdev 코어에 자동 등록하는 매크로.
 * 매크로 내부는 GCC __attribute__((constructor)) 또는 SPDK 의 register 리스트에 의존. */
SPDK_BDEV_MODULE_REGISTER(uring, &uring_if)

/*
 * [한국어]
 * bdev_uring_open - 백킹 파일/디바이스를 O_DIRECT 우선으로 열어 fd 저장.
 *
 * @uring: 대상 uring bdev 인스턴스 (filename 필드가 미리 채워져 있어야 함).
 * @return: 0 성공, -1 실패.
 *
 * O_DIRECT 는 페이지 캐시를 우회하여 사용자 버퍼에서 직접 DMA — SPDK 의 latency 목표상
 * 거의 필수이지만, tmpfs 등 일부 파일 시스템은 O_DIRECT 를 지원하지 않으므로 1차 시도가
 * EINVAL 등으로 실패하면 O_DIRECT 없이 fallback. O_NOATIME 은 access time 업데이트를 막아
 * 추가 metadata write 를 회피.
 * 호출 체인: create_uring_bdev → bdev_uring_open → open(2).
 * 에러 시 fd=-1 마킹하여 destruct 시 close 중복 방지.
 */
static int
bdev_uring_open(struct bdev_uring *uring)
{
	int fd;                                                  /* [한국어] open() 시도 결과 임시 fd. 성공 시 uring->fd 로 commit. */

	fd = open(uring->filename, O_RDWR | O_DIRECT | O_NOATIME); /* [한국어] 1차: O_DIRECT 포함 시도 — 페이지 캐시 우회로 DMA 효율 ↑. */
	if (fd < 0) {                                            /* [한국어] O_DIRECT 미지원 파일시스템 등에서 실패할 수 있음. */
		/* Try without O_DIRECT for non-disk files */
		fd = open(uring->filename, O_RDWR | O_NOATIME);  /* [한국어] 2차 fallback: O_DIRECT 제거하고 재시도 — buffered I/O 로라도 열어준다. */
		if (fd < 0) {                                    /* [한국어] 두 번째도 실패면 진짜 에러 — 권한/존재여부 등. */
			URING_ERRLOG(uring, "open() failed, rc %d: %s\n", fd, spdk_strerror(errno));  /* [한국어] errno 를 사람이 읽을 수 있는 문자열로 로그. */
			uring->fd = -1;                          /* [한국어] destruct 가 close 를 건너뛰도록 sentinel 마킹. */
			return -1;                               /* [한국어] 상위(create_uring_bdev) 에 에러 전파. */
		}
	}

	uring->fd = fd;                                          /* [한국어] 정상 fd 저장 — 이후 io_uring SQE 의 fd 필드로 사용. */

	return 0;                                                /* [한국어] 성공. */
}

/*
 * [한국어]
 * bdev_uring_hot_remove - app thread 위에서 실제 delete_uring_bdev 를 실행하는 메시지 핸들러.
 *
 * @ctx: strdup 으로 복사된 bdev name. 본 함수가 ownership 을 받아 마지막에 free.
 *
 * 왜 별도 함수가 필요한가: IO reaping 중 -EIO/-ENODEV 가 검출되면 hot-remove 가 필요하지만
 * spdk_bdev_unregister 는 app thread 에서만 호출 가능. 따라서 spdk_thread_send_msg 로
 * 메시지 전송 → app thread 에서 본 함수가 실행되어 delete_uring_bdev 호출.
 * 호출 컨텍스트: app thread (spdk_thread_send_msg 의 target).
 */
static void
bdev_uring_hot_remove(void *ctx)
{
	char *name = ctx;                                /* [한국어] strdup 된 bdev 이름 — ownership 인수받음. */

	delete_uring_bdev(name, NULL, NULL);             /* [한국어] 정상 삭제 경로 진입 — 비동기 unregister 후 destruct 콜백에서 close/free. */
	free(name);                                      /* [한국어] strdup 메모리 해제 — delete_uring_bdev 는 동기적으로 unregister 만 시작하므로 안전. */
}

/*
 * [한국어]
 * bdev_uring_try_hot_remove - 디스크 분리 등 비정상 상태 검출 시 hot-remove 를 한 번만 시작.
 *
 * @uring: 분리가 감지된 bdev 인스턴스.
 *
 * IO 완료 처리 중 res=0 (디바이스 크기 0) / res=-EIO 가 반환되면 디바이스가 사라진 것으로
 * 판단. 여러 IO 가 동시에 같은 상태를 검출할 수 있으므로 hot_remove_in_progress 플래그를
 * atomic test_and_set 으로 잠가 단일 unregister 만 발사.
 * 호출 체인: bdev_uring_reap → bdev_uring_try_hot_remove
 *           → spdk_thread_send_msg(app_thread, bdev_uring_hot_remove)
 *           → delete_uring_bdev.
 */
static void
bdev_uring_try_hot_remove(struct bdev_uring *uring)
{
	char	*name;                                                                    /* [한국어] app thread 에 넘길 bdev 이름 사본. */

	if (__atomic_test_and_set(&uring->hot_remove_in_progress, __ATOMIC_RELAXED)) {    /* [한국어] 이미 set 이면 다른 IO 가 먼저 처리 중 — 중복 호출 방지. */
		return;
	}

	name = strdup(uring->bdev.name);                                                  /* [한국어] uring 객체가 곧 해제될 수 있으므로 이름을 사본으로 보존. */
	if (!name) {                                                                      /* [한국어] OOM 시 플래그 되돌리고 포기 — 다음 IO 가 다시 시도. */
		__atomic_clear(&uring->hot_remove_in_progress, __ATOMIC_RELAXED);
		return;
	}

	URING_ERRLOG(uring, "hot-remove detected, unregistering bdev...\n");              /* [한국어] 관리자/사용자에게 명시적 통지. */
	spdk_thread_send_msg(spdk_thread_get_app_thread(), bdev_uring_hot_remove, name);  /* [한국어] cross-thread message 로 app thread 에서 실제 unregister 수행. lockless. */
}

/*
 * [한국어]
 * dummy_bdev_event_cb - 이벤트를 무시하는 빈 콜백.
 *
 * @type/@bdev/@ctx: spdk_bdev_open_ext 시그니처를 만족시키기 위한 인자 — 본 함수는 사용 안 함.
 *
 * bdev_uring_rescan 이 임시로 desc 를 얻을 때 콜백이 필수이지만 rescan 시점 동안은
 * 이벤트(HOTREMOVE 등) 를 처리할 필요가 없으므로 no-op 으로 둔다.
 */
static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
	/* [한국어] 본 콜백은 의도적으로 no-op. rescan 동안 발생할 수 있는 이벤트는 무시. */
}

/*
 * [한국어]
 * bdev_uring_rescan - 백킹 디바이스 크기 재확인 및 bdev blockcnt 갱신 (RPC 진입점).
 *
 * @name: 재스캔할 uring bdev 이름.
 * @return: 0 성공, -ENODEV (모듈 불일치), 기타 음수 errno.
 *
 * 사용 시나리오: 백킹 블록 디바이스가 online resize 되었을 때 RPC bdev_uring_rescan 으로
 * SPDK 가 인지하는 블록 카운트를 다시 동기화. 또한 크기가 0 으로 줄었다면 (디바이스 분리)
 * hot-remove 트리거.
 * 호출 컨텍스트: bdev_uring_rpc.c 의 rpc_bdev_uring_rescan — app thread.
 * 호출 체인: rpc_bdev_uring_rescan → bdev_uring_rescan
 *           → spdk_bdev_open_ext / spdk_fd_get_size / spdk_bdev_notify_blockcnt_change.
 */
int
bdev_uring_rescan(const char *name)
{
	struct spdk_bdev_desc *desc;             /* [한국어] open 의 결과로 받는 descriptor — close 까지 hold. */
	struct spdk_bdev *bdev;                  /* [한국어] desc 로부터 얻는 bdev 객체. */
	struct bdev_uring *uring;                /* [한국어] 우리 모듈 컨테이너. */
	uint64_t uring_size, blockcnt;           /* [한국어] 현재 fd 가 가리키는 실제 바이트 사이즈와 그에 따른 블록 수. */
	int rc;                                  /* [한국어] 에러 코드. */

	rc = spdk_bdev_open_ext(name, false, dummy_bdev_event_cb, NULL, &desc);  /* [한국어] write=false 로 열어 read-only desc 획득. dummy 이벤트 콜백 사용. */
	if (rc != 0) {                                                           /* [한국어] 이름 없음 등 실패 시 즉시 반환. */
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);                                    /* [한국어] desc 에서 bdev 추출. */
	if (bdev->module != &uring_if) {                                         /* [한국어] 다른 모듈 소속이면 rescan 권한 없음. */
		rc = -ENODEV;
		goto exit;
	}

	uring = uring_from_bdev(bdev);                                           /* [한국어] 안전한 다운캐스트. */
	uring_size = spdk_fd_get_size(uring->fd);                                /* [한국어] BLKGETSIZE64 or fstat 으로 현재 디바이스 크기 재조회. */
	blockcnt = uring_size / bdev->blocklen;                                  /* [한국어] 블록 단위로 환산. */

	if (uring_size == 0) {                                                   /* [한국어] 크기 0 — 디바이스가 분리되었을 가능성. */
		bdev_uring_try_hot_remove(uring);                                /* [한국어] hot-remove 흐름으로 전환. */
		goto exit;
	}

	if (bdev->blockcnt != blockcnt) {                                        /* [한국어] online resize 발생 — SPDK 측 metadata 업데이트 필요. */
		URING_NOTICELOG(uring, "URING device is resized: old block count %" PRIu64 ", new block count %"
				PRIu64 "\n",
				bdev->blockcnt,
				blockcnt);
		rc = spdk_bdev_notify_blockcnt_change(bdev, blockcnt);           /* [한국어] lib/bdev 가 모든 채널에 변경을 propagate. */
		if (rc != 0) {                                                   /* [한국어] 상위가 거부할 수도 있음 (예: 진행 중 IO 가 새 크기를 초과). */
			URING_ERRLOG(uring, "Could not change num blocks, rc: %d\n", rc);
			goto exit;
		}
	}

exit:
	spdk_bdev_close(desc);                                                   /* [한국어] 임시 desc 반환 — open 과 짝. */
	return rc;
}

/*
 * [한국어]
 * bdev_uring_close - 백킹 파일 fd 를 close 하고 -1 로 마킹.
 *
 * @uring: 닫을 대상.
 * @return: 0 성공 또는 이미 닫힘, -1 close(2) 실패.
 *
 * idempotent: fd == -1 이면 즉시 0 반환. destruct 와 error path 양쪽에서 호출.
 * 호출 컨텍스트: app thread (destruct 콜백 또는 create 의 error_return).
 */
static int
bdev_uring_close(struct bdev_uring *uring)
{
	int rc;                                                       /* [한국어] close syscall 결과 임시 저장. */

	if (uring->fd == -1) {                                        /* [한국어] 이미 닫혔거나 열린 적 없음 — no-op. */
		return 0;
	}

	rc = close(uring->fd);                                        /* [한국어] OS 에 fd 반환. O_DIRECT 였다면 메타데이터 flush 가 동반될 수 있음. */
	if (rc < 0) {                                                 /* [한국어] EBADF/EIO 등 — 드물지만 로그. */
		URING_ERRLOG(uring, "close() failed (fd=%d), rc %d: %s\n",
			     uring->fd, rc, spdk_strerror(errno));
		return -1;
	}

	uring->fd = -1;                                               /* [한국어] sentinel — 이후 재호출 시 no-op 보장. */

	return 0;                                                     /* [한국어] 정상 종료. */
}

/*
 * [한국어]
 * bdev_uring_readv - vectored READ 요청을 io_uring SQ 에 enqueue.
 *
 * @uring: 백킹 디바이스 핸들.
 * @ch:    이 IO 가 도착한 spdk_io_channel (reactor 단위).
 * @uring_task: 이 IO 의 driver_ctx (sqe->user_data 로 박힘).
 * @iov/@iovcnt: 사용자 버퍼 scatter-gather 리스트.
 * @nbytes: 총 전송 바이트 수 (검증용으로 task->len 에 저장).
 * @offset: 디바이스 바이트 오프셋.
 * @return: nbytes (성공) 또는 -ENOMEM (SQ 가득 참).
 *
 * 실제 시스템 호출은 발생하지 않음 — SQE 만 채워두고 카운터만 증가시킨다. 폴러가
 * 다음 tick 에 io_uring_submit() 으로 일괄 처리. 이 패턴 덕분에 batched submission 의
 * 효율을 얻는다 (한 syscall 로 여러 IO 제출).
 * 호출 체인: lib/bdev → bdev_uring_submit_request → bdev_uring_get_buf_cb → bdev_uring_readv
 *           → io_uring_get_sqe / io_uring_prep_readv. 실제 syscall 은 group_poll 에서.
 * 컨텍스트: reactor 스레드 — 단일 producer.
 */
static int64_t
bdev_uring_readv(struct bdev_uring *uring, struct spdk_io_channel *ch,
		 struct bdev_uring_task *uring_task,
		 struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct bdev_uring_io_channel *uring_ch = spdk_io_channel_get_ctx(ch);    /* [한국어] 채널 컨텍스트 추출. */
	struct bdev_uring_group_channel *group_ch = uring_ch->group_ch;          /* [한국어] reactor 공용 io_uring 인스턴스 소유자. */
	struct io_uring_sqe *sqe;                                                /* [한국어] SQ 의 빈 슬롯 포인터. */

	sqe = io_uring_get_sqe(&group_ch->uring);                                /* [한국어] SQ tail 에서 슬롯 할당. 가득 차면 NULL. */
	if (!sqe) {                                                              /* [한국어] backpressure — 호출자는 nomem retry. */
		URING_DEBUGLOG(uring, "get sqe failed as out of resource\n");
		return -ENOMEM;
	}

	io_uring_prep_readv(sqe, uring->fd, iov, iovcnt, offset);                /* [한국어] SQE 의 opcode=IORING_OP_READV 로 채움 (fd, iovec, offset). */
	io_uring_sqe_set_data(sqe, uring_task);                                  /* [한국어] CQE 에 그대로 반환될 user_data 로 task 포인터 박음 — 완료 시 식별자. */
	uring_task->len = nbytes;                                                /* [한국어] 완료 시 short read 판정용 기대 길이 저장. */
	uring_task->ch = uring_ch;                                               /* [한국어] reap 시 io_inflight 감소 위치 식별. */

	URING_DEBUGLOG(uring, "read %d iovs size %lu to off: %#lx\n", iovcnt, nbytes, offset);  /* [한국어] 디버그 빌드에서만 로그. */

	group_ch->io_pending++;                                                  /* [한국어] 폴러가 다음 틱에 io_uring_submit 호출할 카운터 증가. */
	return nbytes;                                                           /* [한국어] 호출자는 성공으로 처리 (실제 완료는 비동기). */
}

/*
 * [한국어]
 * bdev_uring_writev - readv 의 WRITE 대응 버전. opcode 만 IORING_OP_WRITEV 로 다르고 흐름 동일.
 *
 * 자세한 흐름은 bdev_uring_readv 참조. nbytes 가 size_t 인 점 외에는 동일.
 */
static int64_t
bdev_uring_writev(struct bdev_uring *uring, struct spdk_io_channel *ch,
		  struct bdev_uring_task *uring_task,
		  struct iovec *iov, int iovcnt, size_t nbytes, uint64_t offset)
{
	struct bdev_uring_io_channel *uring_ch = spdk_io_channel_get_ctx(ch);    /* [한국어] 채널 ctx. */
	struct bdev_uring_group_channel *group_ch = uring_ch->group_ch;          /* [한국어] reactor 공용 io_uring. */
	struct io_uring_sqe *sqe;                                                /* [한국어] SQ 슬롯. */

	sqe = io_uring_get_sqe(&group_ch->uring);                                /* [한국어] SQ tail 슬롯 할당. */
	if (!sqe) {                                                              /* [한국어] 가득 참 — backpressure. */
		URING_DEBUGLOG(uring, "get sqe failed as out of resource\n");
		return -ENOMEM;
	}

	io_uring_prep_writev(sqe, uring->fd, iov, iovcnt, offset);               /* [한국어] opcode=IORING_OP_WRITEV. */
	io_uring_sqe_set_data(sqe, uring_task);                                  /* [한국어] CQE 매칭용 user_data. */
	uring_task->len = nbytes;                                                /* [한국어] short write 판정 기대 길이. */
	uring_task->ch = uring_ch;                                               /* [한국어] 완료 시 io_inflight 카운터 위치. */

	URING_DEBUGLOG(uring, "write %d iovs size %lu from off: %#lx\n", iovcnt, nbytes, offset);

	group_ch->io_pending++;                                                  /* [한국어] 폴러가 처리할 제출 대기 수 ++. */
	return nbytes;                                                           /* [한국어] 호출자에게 enqueue 성공 신호. */
}

/*
 * [한국어]
 * bdev_uring_destruct - lib/bdev 가 bdev 를 해제할 때 호출하는 콜백 (.destruct).
 *
 * @ctx: bdev->ctxt 로 박혀있는 struct bdev_uring 포인터.
 * @return: 0 또는 close 실패 시 -1.
 *
 * 순서: 전역 리스트에서 제거 → fd close → io_device 해제 → 메모리 free.
 * 컨텍스트: app thread (lib/bdev 의 unregister 콜백 경로).
 */
static int
bdev_uring_destruct(void *ctx)
{
	struct bdev_uring *uring = ctx;                          /* [한국어] bdev 등록 시 ctxt 로 넣어둔 자기 포인터 복원. */
	int rc = 0;                                              /* [한국어] 최종 반환값. */

	TAILQ_REMOVE(&g_uring_bdev_head, uring, link);           /* [한국어] 모듈 전역 리스트에서 빼냄 — 더 이상 enumeration 대상 아님. */
	rc = bdev_uring_close(uring);                            /* [한국어] open 때 받은 fd 정리. */
	spdk_io_device_unregister(uring, NULL);                  /* [한국어] per-bdev io_device 해제 — 모든 채널 destroy_cb 후 콜백 없이 비동기 unregister. */
	uring_free_bdev(uring);                                  /* [한국어] filename/name/uring 자체 free. */
	return rc;                                               /* [한국어] close 실패는 알리지만 그래도 cleanup 은 끝마침. */
}

/*
 * [한국어]
 * bdev_uring_reap - io_uring CQ 에서 완료된 IO 를 최대 max 개까지 수거하고 상위에 보고.
 *
 * @ring: io_uring 인스턴스 포인터 (group_ch->uring).
 * @max:  이번 호출에서 처리할 최대 CQE 수.
 * @return: 실제로 처리한 CQE 개수.
 *
 * 핵심 흐름:
 *  1) io_uring_peek_cqe 로 비-블로킹 CQE 조회 → 비어 있으면(-EAGAIN) 즉시 종료.
 *  2) CQE->user_data 에서 task 포인터 복원 → spdk_bdev_io 도출.
 *  3) cqe->res 와 기대 len 비교 → 정상/실패/디바이스 분리/NOMEM 분류.
 *  4) io_uring_cqe_seen 으로 CQ head 진행 → spdk_bdev_io_complete 로 상위에 알림.
 * 컨텍스트: reactor 스레드 (단일 consumer).
 * 호출 체인: bdev_uring_group_poll → bdev_uring_reap → io_uring_peek_cqe / spdk_bdev_io_complete.
 */
static int
bdev_uring_reap(struct io_uring *ring, int max)
{
	int i, count, rc;                                /* [한국어] 루프 변수, 처리 카운트, syscall/CQE 결과. */
	struct io_uring_cqe *cqe;                        /* [한국어] peek 결과 CQE 포인터. */
	struct bdev_uring_task *uring_task;              /* [한국어] CQE->user_data 에서 복원한 task. */
	enum spdk_bdev_io_status status;                 /* [한국어] 상위에 통보할 최종 IO 상태. */
	struct spdk_bdev_io *bdev_io;                    /* [한국어] task 로부터 도출한 spdk_bdev_io. */
	struct bdev_uring *uring;                        /* [한국어] 디바이스 분리 검출/로그용 컨테이너. */

	count = 0;                                       /* [한국어] 처리 누적. */
	for (i = 0; i < max; i++) {                      /* [한국어] 한 폴 틱당 최대 max 개까지 — 다른 폴러를 starve 시키지 않기 위함. */
		rc = io_uring_peek_cqe(ring, &cqe);      /* [한국어] CQ head 조회. lockless mmap shared ring 의 비-블로킹 읽기. */
		if (rc != 0) {                           /* [한국어] CQ 비어있음 — 더 처리할 게 없음. */
			assert(rc == -EAGAIN || rc == -EWOULDBLOCK);  /* [한국어] peek 의 기대 에러 외에는 비정상. */
			return count;
		}

		assert(cqe != NULL);                     /* [한국어] rc==0 이면 cqe 는 반드시 유효. */

		uring_task = (struct bdev_uring_task *)cqe->user_data;  /* [한국어] SQE 에 박아둔 task 포인터 복원. */
		bdev_io = spdk_bdev_io_from_ctx(uring_task);            /* [한국어] driver_ctx → 자신을 포함하는 spdk_bdev_io 로 역참조. */
		rc = cqe->res;                                          /* [한국어] 커널이 보고한 IO 결과 (성공 시 전송 바이트, 실패 시 음수 errno). */
		if (spdk_unlikely(rc != (signed)uring_task->len)) {     /* [한국어] 기대 길이와 다르면 비정상 경로 진입 (보통 hot path 는 매치). */
			/* When the block device device is detached from the system, IOs fail with res of 0.
			 * In this case the ioctl BLKGETSIZE64 yields a device size of 0.
			 * Note that re-attaching the device will not correct this because the existing fd is
			 * still invalid.
			 * When the fd is a file and the mount backing the file is detached, IOs fail
			 * with a res of -EIO and the ioctl BLKGETSIZE64 yields a device size of 0.
			 */
			uring = uring_from_bdev(bdev_io->bdev);          /* [한국어] hot-remove 판정용 컨테이너. */
			if (rc == -EIO || rc >= 0) {                     /* [한국어] -EIO 거나 짧게 끝난 경우 — 디바이스 분리 의심. */
				if (spdk_fd_get_size(uring->fd) == 0) {  /* [한국어] BLKGETSIZE64=0 이면 디바이스가 disappear 했다는 강한 시그널. */
					rc = -ENODEV;                    /* [한국어] hot-remove 트리거 분기로 fallthrough. */
				}
			}

			if (rc == -EAGAIN || rc == -EWOULDBLOCK) {       /* [한국어] 일시 자원 부족 — 상위가 재시도 정책에 따라 다시 큐잉. */
				status = SPDK_BDEV_IO_STATUS_NOMEM;
			} else {
				if (rc == -ENODEV) {                     /* [한국어] 디바이스 분리 확정 — unregister 흐름 시작. */
					bdev_uring_try_hot_remove(uring);
				} else {
					URING_ERRLOG(uring, "I/O failed with error %d\n", rc);  /* [한국어] 기타 영구 실패 — 로그. */
				}
				status = SPDK_BDEV_IO_STATUS_FAILED;
			}
		} else {
			status = SPDK_BDEV_IO_STATUS_SUCCESS;            /* [한국어] 기대 길이와 정확히 일치 — 성공. */
		}

		uring_task->ch->group_ch->io_inflight--;                 /* [한국어] inflight 카운터 감소 — 다음 폴러 결정에 반영. */
		io_uring_cqe_seen(ring, cqe);                            /* [한국어] CQ head 전진 — 커널에게 이 CQE 처리 완료 통지 (lockless). */
		spdk_bdev_io_complete(bdev_io, status);                  /* [한국어] 상위 lib/bdev 에 완료 콜백 디스패치. */
		count++;
	}

	return count;                                                    /* [한국어] 처리한 CQE 개수 반환 — 폴러 BUSY/IDLE 판정에 활용. */
}

/*
 * [한국어]
 * bdev_uring_group_poll - reactor 마다 등록된 폴러. 매 틱마다 SQ 제출 + CQ 수거.
 *
 * @arg: group_channel 포인터.
 * @return: SPDK_POLLER_BUSY (이번 틱에 의미 있는 작업 했음) 또는 SPDK_POLLER_IDLE.
 *
 * 폴러 본체 — SPDK 의 polled-mode 철학을 그대로 구현. 인터럽트 없이 무한 루프에서 호출.
 *  1) io_pending > 0 이면 io_uring_submit() (실제 io_uring_enter syscall 자동 발사).
 *  2) io_inflight > 0 이면 bdev_uring_reap() 으로 CQE 수거.
 *  3) 둘 다 0 이면 IDLE — SPDK 가 다른 폴러로 양보.
 * io_uring 의 IORING_FEAT_FAST_POLL 이 활성이면 submit 도 syscall 없이 끝날 수 있음.
 * 컨텍스트: reactor 스레드 (lockless, 단일 소유자).
 */
static int
bdev_uring_group_poll(void *arg)
{
	struct bdev_uring_group_channel *group_ch = arg;  /* [한국어] 등록 시 user data 로 전달된 그룹 채널. */
	int to_complete, to_submit;                       /* [한국어] 이번 틱의 제출 대기/완료 대기 수. */
	int count, ret;                                   /* [한국어] reap 한 개수, submit 결과. */

	to_submit = group_ch->io_pending;                 /* [한국어] readv/writev 가 쌓아둔 SQE 개수 snapshot. */

	if (to_submit > 0) {
		/* If there are I/O to submit, use io_uring_submit here.
		 * It will automatically call spdk_io_uring_enter appropriately. */
		ret = io_uring_submit(&group_ch->uring);  /* [한국어] SQ 의 SQE 들을 한꺼번에 커널에 제출 (필요 시 io_uring_enter syscall). */
		if (ret < 0) {                            /* [한국어] -EAGAIN/-EBUSY 등 — 일시 실패, 다음 틱 재시도. */
			return SPDK_POLLER_BUSY;
		}

		group_ch->io_pending = 0;                 /* [한국어] 모두 제출되었다고 가정 후 pending 초기화. */
		group_ch->io_inflight += to_submit;       /* [한국어] inflight 로 이동 — 이제부터 CQE 수거 대상. */
	}

	to_complete = group_ch->io_inflight;              /* [한국어] reap 시도할 상한. */
	count = 0;                                        /* [한국어] 실제 수거된 개수. */
	if (to_complete > 0) {
		count = bdev_uring_reap(&group_ch->uring, to_complete);  /* [한국어] CQ peek 루프 — 호출자 콜백 발사. */
	}

	if (count + to_submit > 0) {                      /* [한국어] 의미 있는 일이 있었음 — BUSY. */
		return SPDK_POLLER_BUSY;
	} else {
		return SPDK_POLLER_IDLE;                  /* [한국어] 아무 일도 없음 — SPDK 스케줄러에게 idle 보고. */
	}
}

/*
 * [한국어]
 * bdev_uring_get_buf_cb - spdk_bdev_io_get_buf 의 콜백 — 정렬된 버퍼 확보 후 실제 IO 발사.
 *
 * @ch: 이 IO 의 io_channel.
 * @bdev_io: 처리할 spdk_bdev_io.
 * @success: 버퍼 확보 성공 여부.
 *
 * O_DIRECT 는 사용자 버퍼가 디바이스의 required_alignment (보통 512/4096) 에 정렬되어
 * 있어야 한다. lib/bdev 의 spdk_bdev_io_get_buf 가 풀에서 정렬된 버퍼를 빌려와 콜백
 * 시점에 iovs 가 정렬 상태임을 보장한다. 여기서 READ/WRITE 분기해서 readv/writev 호출.
 * 호출 체인: bdev_uring_submit_request → spdk_bdev_io_get_buf → bdev_uring_get_buf_cb
 *           → bdev_uring_readv/writev.
 * 컨텍스트: reactor 스레드 (lib/bdev 가 동일 스레드에서 콜백 발사 보장).
 */
static void
bdev_uring_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		      bool success)
{
	int64_t ret = 0;                                                   /* [한국어] readv/writev 반환값 (NOMEM 검출용). */
	struct bdev_uring *uring = uring_from_bdev(bdev_io->bdev);         /* [한국어] 모듈 컨테이너 복원. */

	if (!success) {                                                    /* [한국어] 버퍼 풀 고갈 등 — 상위에 실패 통보 후 종료. */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:                                       /* [한국어] 읽기. */
		ret = bdev_uring_readv(uring,
				       ch,
				       (struct bdev_uring_task *)bdev_io->driver_ctx,                    /* [한국어] driver_ctx 영역 = task. */
				       bdev_io->u.bdev.iovs,                                              /* [한국어] 정렬된 iovec 배열. */
				       bdev_io->u.bdev.iovcnt,                                            /* [한국어] iovec 개수. */
				       bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,              /* [한국어] 바이트 길이로 환산. */
				       bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);          /* [한국어] 바이트 오프셋으로 환산. */
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:                                      /* [한국어] 쓰기 — 인자 동일, 호출 함수만 다름. */
		ret = bdev_uring_writev(uring,
					ch,
					(struct bdev_uring_task *)bdev_io->driver_ctx,
					bdev_io->u.bdev.iovs,
					bdev_io->u.bdev.iovcnt,
					bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
					bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		break;
	default:
		URING_ERRLOG(uring, "Wrong io type\n");                    /* [한국어] io_type_supported 가 거르지 못한 경우 — 방어 로그. */
		break;
	}

	if (ret == -ENOMEM) {                                              /* [한국어] SQ 가득 참 — 상위에 NOMEM 통보, 큐잉/재시도 정책 위임. */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
	}
}

#ifdef SPDK_CONFIG_URING_ZNS
/*
 * [한국어]
 * bdev_uring_fill_zone_type - Linux blk_zone->type → SPDK zone type 변환.
 *
 * @uring: 로그용 디바이스.
 * @zone_info: 채울 SPDK 출력 구조체.
 * @zones_rep: 커널이 BLKREPORTZONE 으로 반환한 raw zone descriptor.
 * @return: 0 성공 / -EIO 알 수 없는 타입.
 *
 * NVMe ZNS 스펙의 zone type (Conventional / Seq-write Required / Seq-write Preferred) 를
 * SPDK 가 정의한 enum 으로 1:1 매핑. zone_get_info 의 보조 함수.
 */
static int
bdev_uring_fill_zone_type(struct bdev_uring *uring, struct spdk_bdev_zone_info *zone_info,
			  struct blk_zone *zones_rep)
{
	switch (zones_rep->type) {
	case BLK_ZONE_TYPE_CONVENTIONAL:                                                   /* [한국어] 일반(랜덤 쓰기 가능) zone. */
		zone_info->type = SPDK_BDEV_ZONE_TYPE_CNV;
		break;
	case BLK_ZONE_TYPE_SEQWRITE_REQ:                                                   /* [한국어] 시퀀셜 쓰기 강제 zone. */
		zone_info->type = SPDK_BDEV_ZONE_TYPE_SEQWR;
		break;
	case BLK_ZONE_TYPE_SEQWRITE_PREF:                                                  /* [한국어] 시퀀셜 쓰기 권장 zone (HM-SMR 등). */
		zone_info->type = SPDK_BDEV_ZONE_TYPE_SEQWP;
		break;
	default:
		URING_ERRLOG(uring, "Invalid zone type: %#x in zone report\n", zones_rep->type);  /* [한국어] 미지원 — 펌웨어 버그 가능성. */
		return -EIO;
	}
	return 0;
}

/*
 * [한국어]
 * bdev_uring_fill_zone_state - Linux blk_zone->cond → SPDK zone state 변환.
 *
 * @uring/@zone_info/@zones_rep: bdev_uring_fill_zone_type 와 동일.
 * @return: 0 성공 / -EIO 알 수 없는 상태.
 *
 * 8가지 zone condition (NVMe ZNS 스펙): empty / imp open / exp open / closed / read-only
 * / full / offline / not write pointer 을 매핑.
 */
static int
bdev_uring_fill_zone_state(struct bdev_uring *uring, struct spdk_bdev_zone_info *zone_info,
			   struct blk_zone *zones_rep)
{
	switch (zones_rep->cond) {
	case BLK_ZONE_COND_EMPTY:                                                          /* [한국어] 비어 있음 (wp=zone_start). */
		zone_info->state = SPDK_BDEV_ZONE_STATE_EMPTY;
		break;
	case BLK_ZONE_COND_IMP_OPEN:                                                       /* [한국어] 묵시적으로 open 됨 (쓰기로 인해 자동 transition). */
		zone_info->state = SPDK_BDEV_ZONE_STATE_IMP_OPEN;
		break;
	case BLK_ZONE_COND_EXP_OPEN:                                                       /* [한국어] 명시적으로 open (BLKOPENZONE). */
		zone_info->state = SPDK_BDEV_ZONE_STATE_EXP_OPEN;
		break;
	case BLK_ZONE_COND_CLOSED:                                                         /* [한국어] close 됨 (자원 절약, 데이터 유지). */
		zone_info->state = SPDK_BDEV_ZONE_STATE_CLOSED;
		break;
	case BLK_ZONE_COND_READONLY:                                                       /* [한국어] 읽기 전용. */
		zone_info->state = SPDK_BDEV_ZONE_STATE_READ_ONLY;
		break;
	case BLK_ZONE_COND_FULL:                                                           /* [한국어] wp 가 zone 끝까지 도달. */
		zone_info->state = SPDK_BDEV_ZONE_STATE_FULL;
		break;
	case BLK_ZONE_COND_OFFLINE:                                                        /* [한국어] 오프라인 — 접근 불가. */
		zone_info->state = SPDK_BDEV_ZONE_STATE_OFFLINE;
		break;
	case BLK_ZONE_COND_NOT_WP:                                                         /* [한국어] write pointer 없음 (Conventional zone). */
		zone_info->state = SPDK_BDEV_ZONE_STATE_NOT_WP;
		break;
	default:
		URING_ERRLOG(uring, "Invalid zone state: %#x in zone report\n", zones_rep->cond);
		return -EIO;
	}
	return 0;
}

/*
 * [한국어]
 * bdev_uring_zone_management_op - SPDK zone action 을 Linux BLK*ZONE ioctl 로 변환 실행.
 *
 * @bdev_io: SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT 요청.
 * @return: 0 성공, -EINVAL 실패.
 *
 * io_uring 자체에 zone management opcode 가 없어 동기 ioctl 로 처리. 빠르게 끝나고
 * 빈도가 낮은 메타 동작이라 동기여도 SPDK 전체 흐름에 큰 영향 없음.
 */
static int
bdev_uring_zone_management_op(struct spdk_bdev_io *bdev_io)
{
	struct bdev_uring *uring;                                  /* [한국어] 컨테이너. */
	struct blk_zone_range range;                               /* [한국어] ioctl 파라미터 (sector, nr_sectors). */
	long unsigned zone_mgmt_op;                                /* [한국어] 매핑된 BLK*ZONE ioctl 번호. */
	uint64_t zone_id = bdev_io->u.zone_mgmt.zone_id;           /* [한국어] 대상 zone 의 시작 LBA. */

	uring = uring_from_bdev(bdev_io->bdev);                    /* [한국어] 모듈 컨테이너 복원. */

	switch (bdev_io->u.zone_mgmt.zone_action) {
	case SPDK_BDEV_ZONE_RESET:
		zone_mgmt_op = BLKRESETZONE;                       /* [한국어] zone 을 empty 상태로 reset. */
		break;
	case SPDK_BDEV_ZONE_OPEN:
		zone_mgmt_op = BLKOPENZONE;                        /* [한국어] explicit open. */
		break;
	case SPDK_BDEV_ZONE_CLOSE:
		zone_mgmt_op = BLKCLOSEZONE;                       /* [한국어] open zone 닫기. */
		break;
	case SPDK_BDEV_ZONE_FINISH:
		zone_mgmt_op = BLKFINISHZONE;                      /* [한국어] zone 을 full 로 전환 (wp 를 끝으로). */
		break;
	default:
		return -EINVAL;                                    /* [한국어] 지원하지 않는 action. */
	}

	range.sector = (zone_id << uring->zd.lba_shift);           /* [한국어] SPDK LBA → 512B sector 변환. */
	range.nr_sectors = (uring->bdev.zone_size << uring->zd.lba_shift);  /* [한국어] zone 전체 길이 (sector 단위). */

	if (ioctl(uring->fd, zone_mgmt_op, &range)) {              /* [한국어] 동기 ioctl. 성공 0, 실패 -1+errno. */
		URING_ERRLOG(uring, "Ioctl BLKXXXZONE(%#x) failed errno: %d(%s)\n",
			     bdev_io->u.zone_mgmt.zone_action, errno, strerror(errno));
		return -EINVAL;
	}

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);  /* [한국어] 즉시 성공 콜백 — 동기 완료. */

	return 0;
}

/*
 * [한국어]
 * bdev_uring_zone_get_info - 지정 zone 부터 num_zones 개의 zone 메타데이터를 수집.
 *
 * @bdev_io: SPDK_BDEV_IO_TYPE_GET_ZONE_INFO 요청.
 * @return: 0 성공, -EINVAL/-ENOMEM 실패.
 *
 * BLKREPORTZONE ioctl 은 한 번에 모든 zone 을 못 채울 수 있으므로 마지막으로 받은 zone 의
 * end+1 sector 부터 다시 호출하는 루프 형태.
 */
static int
bdev_uring_zone_get_info(struct spdk_bdev_io *bdev_io)
{
	struct bdev_uring *uring;                                          /* [한국어] 컨테이너. */
	struct blk_zone *zones;                                            /* [한국어] rep 다음에 이어지는 blk_zone 배열. */
	struct blk_zone_report *rep;                                       /* [한국어] ioctl 입출력 헤더 + 배열. */
	struct spdk_bdev_zone_info *zone_info = bdev_io->u.zone_mgmt.buf;  /* [한국어] 호출자 출력 버퍼 (SPDK 표준 zone 정보). */
	size_t repsize;                                                    /* [한국어] 총 할당 크기. */
	uint32_t i, shift;                                                 /* [한국어] 루프 인덱스, sector→LBA shift. */
	uint32_t num_zones = bdev_io->u.zone_mgmt.num_zones;               /* [한국어] 요청된 zone 개수. */
	uint64_t zone_id = bdev_io->u.zone_mgmt.zone_id;                   /* [한국어] 시작 zone 의 LBA. */

	uring = uring_from_bdev(bdev_io->bdev);
	shift = uring->zd.lba_shift;                                       /* [한국어] sector → SPDK LBA. */

	if ((num_zones > uring->zd.num_zones) || !num_zones) {             /* [한국어] 디바이스 zone 수 초과 또는 0 — 무의미. */
		return -EINVAL;
	}

	repsize = sizeof(struct blk_zone_report) + (sizeof(struct blk_zone) * num_zones);  /* [한국어] ioctl 가 요구하는 가변 길이 버퍼. */
	rep = (struct blk_zone_report *)malloc(repsize);                   /* [한국어] heap 할당 — 한 IO 동안만 임시. */
	if (!rep) {
		return -ENOMEM;
	}

	zones = (struct blk_zone *)(rep + 1);                              /* [한국어] header 바로 뒤가 zones[]. */

	while (num_zones && ((zone_id >> uring->zd.zone_shift) <= num_zones)) {  /* [한국어] 남은 zone 이 있고 zone_id 가 범위 내일 동안 반복. */
		memset(rep, 0, repsize);                                   /* [한국어] 매 ioctl 직전 zero-fill (커널이 일부 필드 입력값 검사). */
		rep->sector = zone_id;                                     /* [한국어] 시작 LBA — 커널이 sector 단위로 해석 (zone_id가 SPDK LBA 라 lba_shift 적용 안된 채? 실제 코드는 그대로 전달). */
		rep->nr_zones = num_zones;                                 /* [한국어] 최대 보고할 zone 수. */

		if (ioctl(uring->fd, BLKREPORTZONE, rep)) {                /* [한국어] 동기 zone report 요청. */
			URING_ERRLOG(uring, "Ioctl BLKREPORTZONE failed errno: %d(%s)\n",
				     errno, strerror(errno));
			free(rep);
			return -EINVAL;
		}

		if (!rep->nr_zones) {                                      /* [한국어] 더 보고할 zone 없음 — 루프 탈출. */
			break;
		}

		for (i = 0; i < rep->nr_zones; i++) {                      /* [한국어] 보고된 zone 들을 SPDK 출력 형식으로 변환. */
			zone_info->zone_id = ((zones + i)->start >> shift);          /* [한국어] sector → LBA 변환. */
			zone_info->write_pointer = ((zones + i)->wp >> shift);       /* [한국어] WP sector → LBA. */
			zone_info->capacity = ((zones + i)->capacity >> shift);      /* [한국어] usable capacity LBA. */

			bdev_uring_fill_zone_state(uring, zone_info, zones + i);     /* [한국어] cond 변환. */
			bdev_uring_fill_zone_type(uring, zone_info, zones + i);      /* [한국어] type 변환. */

			zone_id = ((zones + i)->start + (zones + i)->len) >> shift;  /* [한국어] 다음 루프의 시작점 = 이번 zone 끝 + 1. */
			zone_info++;                                                 /* [한국어] 출력 배열 진행. */
			num_zones--;                                                 /* [한국어] 남은 요청 수 감소. */
		}
	}

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);       /* [한국어] 동기 완료 보고. */
	free(rep);                                                         /* [한국어] 임시 버퍼 반환. */
	return 0;
}

/*
 * [한국어]
 * bdev_uring_check_zoned_support - sysfs /sys/block/<dev>/queue/zoned 를 확인해 ZNS 메타 채움.
 *
 * @uring: 대상 bdev. zoned 필드와 zd 서브구조를 채움.
 * @name:  로그용 bdev 이름 (현재 미사용 인자, 시그니처 유지).
 * @filename: 백킹 디바이스 경로 (symlink 해석 후 basename 추출).
 * @return: 0 성공 (zoned 여부와 무관하게 검사 자체가 성공), -1 실패.
 *
 * 일반 파일은 ZNS 가 될 수 없으므로 stat 으로 블록 디바이스인지 확인 후 sysfs 읽음.
 * "host-aware"/"host-managed" 면 ZNS — BLKGETNRZONES/BLKGETZONESZ ioctl 로 메타 수집.
 */
static int
bdev_uring_check_zoned_support(struct bdev_uring *uring, const char *name, const char *filename)
{
	char *filename_dup = NULL, *base;                             /* [한국어] basename() 이 in-place 수정하므로 사본 필요. */
	char *str = NULL;                                             /* [한국어] sysfs 가 반환한 문자열 (예: "host-managed"). */
	uint32_t val;                                                 /* [한국어] sysfs uint32 결과 임시. */
	uint32_t zinfo;                                               /* [한국어] BLKGETNRZONES/BLKGETZONESZ ioctl 결과 임시. */
	int retval = -1;                                              /* [한국어] 누적 반환값 — 마지막에 free 후 return. */
	struct stat sb;                                               /* [한국어] stat(2) 결과. */
	char resolved_path[PATH_MAX], *rp;                            /* [한국어] symlink 해석 결과 버퍼와 포인터. */
	char *sysfs_path = NULL;                                      /* [한국어] /sys/block/<base>/queue/zoned 경로. */

	uring->bdev.zoned = false;                                    /* [한국어] 기본값 — 검사 통과해도 zoned 가 아닐 수 있음. */

	/* Follow symlink */
	if ((rp = realpath(filename, resolved_path))) {               /* [한국어] /dev/disk/by-id 같은 symlink 를 실제 /dev/sdX 로 해석. */
		filename = rp;
	}

	/* Perform check on block devices only */
	if (stat(filename, &sb) == 0 && !S_ISBLK(sb.st_mode)) {       /* [한국어] 일반 파일이면 ZNS 가 아니므로 곧장 성공 종료. */
		return 0;
	}

	/* strdup() because basename() may modify the passed parameter */
	filename_dup = strdup(filename);                              /* [한국어] basename 안전 사용 위해 가변 사본 확보. */
	if (filename_dup == NULL) {
		URING_ERRLOG(uring, "Could not duplicate string %s\n", filename);
		return -1;
	}

	base = basename(filename_dup);                                /* [한국어] "/dev/sdb" → "sdb". */
	sysfs_path = spdk_sprintf_alloc("/sys/block/%s/queue/zoned", base);  /* [한국어] sysfs attribute 경로 동적 생성. */
	retval = spdk_read_sysfs_attribute(&str, "%s", sysfs_path);   /* [한국어] 파일 한 줄 읽어 str 에 strdup 저장. */
	/* Check if this is a zoned block device */
	if (retval < 0) {
		URING_ERRLOG(uring, "Unable to open file %s. errno: %d\n", sysfs_path, retval);  /* [한국어] sysfs 미존재 등 — 일반 디바이스 가능성. */
	} else if (strcmp(str, "host-aware") == 0 || strcmp(str, "host-managed") == 0) {
		/* Only host-aware & host-managed zns devices */
		uring->bdev.zoned = true;                             /* [한국어] ZNS 마킹. */

		if (ioctl(uring->fd, BLKGETNRZONES, &zinfo)) {        /* [한국어] 디바이스의 zone 총 개수 조회. */
			URING_ERRLOG(uring, "ioctl BLKNRZONES failed %d (%s)\n", errno, strerror(errno));
			goto err_ret;
		}
		uring->zd.num_zones = zinfo;

		if (ioctl(uring->fd, BLKGETZONESZ, &zinfo)) {         /* [한국어] zone size (sector 단위) 조회. */
			URING_ERRLOG(uring, "ioctl BLKGETZONESZ failed %d (%s)\n", errno, strerror(errno));
			goto err_ret;
		}

		uring->zd.lba_shift = uring->bdev.required_alignment - SECTOR_SHIFT;  /* [한국어] 예: 4096B block → 4096=2^12, sector=2^9, shift=3. */
		uring->bdev.zone_size = (zinfo >> uring->zd.lba_shift);               /* [한국어] sector → LBA 변환. */
		uring->zd.zone_shift = spdk_u32log2(zinfo >> uring->zd.lba_shift);    /* [한국어] LBA 단위 zone size 의 log2 (zone 번호 계산용). */

		retval = spdk_read_sysfs_attribute_uint32(&val, "/sys/block/%s/queue/max_open_zones", base);
		if (retval < 0) {
			URING_ERRLOG(uring, "Failed to get max open zones %d (%s)\n", retval, strerror(-retval));
			goto err_ret;
		}
		uring->bdev.max_open_zones = uring->bdev.optimal_open_zones = val;    /* [한국어] 동시에 open 가능한 zone 한계 — SPDK 상위가 throttle 에 사용. */

		retval = spdk_read_sysfs_attribute_uint32(&val, "/sys/block/%s/queue/max_active_zones", base);
		if (retval < 0) {
			URING_ERRLOG(uring, "Failed to get max active zones %d (%s)\n", retval, strerror(-retval));
			goto err_ret;
		}
		uring->bdev.max_active_zones = val;                                   /* [한국어] active (open+closed) zone 한계. */
		retval = 0;                                                           /* [한국어] 전체 성공. */
	} else {
		retval = 0;        /* queue/zoned=none */                             /* [한국어] 일반 블록 디바이스 (예: SSD/HDD) — ZNS 아님, 정상. */
	}
err_ret:
	free(str);                                                                /* [한국어] sysfs 결과 해제. */
	free(sysfs_path);                                                         /* [한국어] 동적 sysfs 경로 해제. */
	free(filename_dup);                                                       /* [한국어] basename 용 사본 해제. */
	return retval;
}
#else
/* No support for zoned devices */
/*
 * [한국어]
 * bdev_uring_zone_management_op (no-ZNS stub) - 컴파일 옵션이 ZNS 미지원일 때 호출되면 즉시 실패.
 * 호출되어선 안 되는 경로 — io_type_supported 가 GET_ZONE_INFO/ZONE_MANAGEMENT 를 false 로 보고하므로.
 */
static int
bdev_uring_zone_management_op(struct spdk_bdev_io *bdev_io)
{
	return -1;                                                                /* [한국어] 절대 호출되지 않을 코드 — defensive. */
}

/*
 * [한국어]
 * bdev_uring_zone_get_info (no-ZNS stub) - 위와 동일한 이유로 -1.
 */
static int
bdev_uring_zone_get_info(struct spdk_bdev_io *bdev_io)
{
	return -1;                                                                /* [한국어] no-op stub. */
}

/*
 * [한국어]
 * bdev_uring_check_zoned_support (no-ZNS stub) - 검사 자체를 스킵하고 zoned=false 로 두기 위해 0 반환.
 */
static int
bdev_uring_check_zoned_support(struct bdev_uring *uring, const char *name, const char *filename)
{
	return 0;                                                                 /* [한국어] 무조건 성공 — uring->bdev.zoned 는 calloc 으로 false 인 상태. */
}
#endif

/*
 * [한국어]
 * _bdev_uring_submit_request - bdev_io 타입별 디스패치 (래퍼 내부 함수).
 *
 * @ch: 이 IO 의 io_channel.
 * @bdev_io: 처리할 요청.
 * @return: 0 (정상 dispatch — 비동기 완료), -1 (실패 — 호출자가 즉시 실패 complete).
 *
 * READ/WRITE 는 정렬 버퍼 확보를 위해 spdk_bdev_io_get_buf 우회 후 콜백에서 실제 submit.
 * ZNS 메타 동작은 직접 ioctl 후 동기 완료.
 */
static int
_bdev_uring_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_GET_ZONE_INFO:
		return bdev_uring_zone_get_info(bdev_io);                                   /* [한국어] ZNS report. */
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
		return bdev_uring_zone_management_op(bdev_io);                              /* [한국어] ZNS open/close/reset/finish. */
	/* Read and write operations must be performed on buffers aligned to
	 * bdev->required_alignment. If user specified unaligned buffers,
	 * get the aligned buffer from the pool by calling spdk_bdev_io_get_buf. */
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		spdk_bdev_io_get_buf(bdev_io, bdev_uring_get_buf_cb,                        /* [한국어] 풀에서 정렬 버퍼 확보 후 콜백. */
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;
	default:
		return -1;                                                                  /* [한국어] io_type_supported 가 거른 외 타입 — defensive 실패. */
	}
}

/*
 * [한국어]
 * bdev_uring_submit_request - lib/bdev 가 부르는 .submit_request 진입점.
 *
 * @ch/@bdev_io: 처리 대상.
 *
 * _bdev_uring_submit_request 가 실패면 즉시 complete 로 실패 보고. 성공이면 비동기.
 * 컨텍스트: reactor 스레드.
 */
static void
bdev_uring_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_uring_submit_request(ch, bdev_io) < 0) {                                  /* [한국어] dispatch 실패 시 즉시 알림. */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * bdev_uring_io_type_supported - 이 모듈이 지원하는 IO 타입 보고.
 *
 * @ctx/@io_type: 표준 콜백 시그니처.
 * @return: true 지원, false 미지원.
 *
 * READ/WRITE 만 항상 지원. ZNS 빌드면 GET_ZONE_INFO/ZONE_MANAGEMENT 추가 지원.
 * FLUSH/UNMAP/RESET 등은 미지원 (O_DIRECT 한계).
 */
static bool
bdev_uring_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
#ifdef SPDK_CONFIG_URING_ZNS
	case SPDK_BDEV_IO_TYPE_GET_ZONE_INFO:                                              /* [한국어] zone metadata 조회. */
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:                                            /* [한국어] zone state 변경. */
#endif
	case SPDK_BDEV_IO_TYPE_READ:                                                       /* [한국어] readv 지원. */
	case SPDK_BDEV_IO_TYPE_WRITE:                                                      /* [한국어] writev 지원. */
		return true;
	default:
		return false;                                                              /* [한국어] FLUSH/UNMAP/COMPARE 등 미지원. */
	}
}

/*
 * [한국어]
 * bdev_uring_create_cb - per-bdev io_channel 생성 시 호출. group_ch 포인터 캐싱.
 *
 * @io_device: 본 bdev (spdk_io_device_register 시 등록).
 * @ctx_buf: bdev_uring_io_channel 메모리 (이미 lib/bdev 가 할당).
 * @return: 0 성공.
 */
static int
bdev_uring_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_uring_io_channel *ch = ctx_buf;                                        /* [한국어] 채널 ctx 캐스팅. */

	ch->group_ch = spdk_io_channel_get_ctx(spdk_get_io_channel(&uring_if));            /* [한국어] 모듈 io_device 의 채널을 reference + ctx 추출. */

	return 0;
}

/*
 * [한국어]
 * bdev_uring_destroy_cb - per-bdev io_channel 해제 시 호출. group_ch reference 반환.
 */
static void
bdev_uring_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_uring_io_channel *ch = ctx_buf;

	spdk_put_io_channel(spdk_io_channel_from_ctx(ch->group_ch));                       /* [한국어] reference 해제 — refcount 0 시 group destroy_cb 가 io_uring 정리. */
}

/*
 * [한국어]
 * bdev_uring_get_io_channel - lib/bdev 가 .get_io_channel 로 호출. 채널 reference 반환.
 */
static struct spdk_io_channel *
bdev_uring_get_io_channel(void *ctx)
{
	struct bdev_uring *uring = ctx;

	return spdk_get_io_channel(uring);                                                 /* [한국어] per-bdev io_device 의 reactor-local 채널 획득. */
}

/*
 * [한국어]
 * bdev_uring_dump_info_json - bdev_get_bdevs RPC 등에서 모듈별 추가 정보 JSON 출력.
 *
 * @ctx: bdev->ctxt = struct bdev_uring.
 * @w: JSON writer 컨텍스트.
 * @return: 0.
 *
 * 출력 예: "uring": { "filename": "/dev/nvme0n1" }.
 */
static int
bdev_uring_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_uring *uring = ctx;

	spdk_json_write_named_object_begin(w, "uring");                                    /* [한국어] 객체 열기. */

	spdk_json_write_named_string(w, "filename", uring->filename);                      /* [한국어] 백킹 경로 노출. */

	spdk_json_write_object_end(w);                                                     /* [한국어] 객체 닫기. */

	return 0;
}

/*
 * [한국어]
 * bdev_uring_write_json_config - save_config RPC 가 호출하는 재구성용 설정 출력.
 *
 * @bdev: 직렬화할 bdev.
 * @w: JSON writer.
 *
 * bdev_uring_create 메서드로 재구성 가능한 형태로 출력 — name, block_size, filename, uuid.
 */
static void
bdev_uring_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct bdev_uring *uring = uring_from_bdev(bdev);
	char uuid_str[SPDK_UUID_STRING_LEN];                                               /* [한국어] UUID 텍스트 표현 임시 버퍼. */

	spdk_json_write_object_begin(w);                                                   /* [한국어] 전체 RPC 호출 객체 시작. */

	spdk_json_write_named_string(w, "method", "bdev_uring_create");                    /* [한국어] re-config 시 호출할 메서드 이름. */

	spdk_json_write_named_object_begin(w, "params");                                   /* [한국어] params 서브객체. */
	spdk_json_write_named_string(w, "name", bdev->name);                               /* [한국어] bdev 이름. */
	spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);                     /* [한국어] 블록 크기. */
	spdk_json_write_named_string(w, "filename", uring->filename);                      /* [한국어] 백킹 경로. */
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);                      /* [한국어] uuid → 소문자 헥스 문자열. */
	spdk_json_write_named_string(w, "uuid", uuid_str);                                 /* [한국어] uuid 출력. */
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

/* [한국어] lib/bdev 가 사용할 함수 테이블. 본 모듈이 구현한 콜백들을 모은 vtable. */
static const struct spdk_bdev_fn_table uring_fn_table = {
	.destruct		= bdev_uring_destruct,             /* [한국어] bdev 해제. */
	.submit_request		= bdev_uring_submit_request,       /* [한국어] IO 진입점. */
	.io_type_supported	= bdev_uring_io_type_supported,    /* [한국어] 지원 IO 타입 보고. */
	.get_io_channel		= bdev_uring_get_io_channel,       /* [한국어] io_channel 획득. */
	.dump_info_json		= bdev_uring_dump_info_json,       /* [한국어] RPC info 노출. */
	.write_config_json	= bdev_uring_write_json_config,    /* [한국어] save_config 출력. */
};

/*
 * [한국어]
 * uring_free_bdev - bdev 구조체와 종속 문자열 메모리 해제 헬퍼.
 *
 * @uring: 해제 대상 (NULL 안전).
 *
 * 호출 컨텍스트: bdev_uring_destruct, create_uring_bdev 의 error_return.
 */
static void
uring_free_bdev(struct bdev_uring *uring)
{
	if (uring == NULL) {                              /* [한국어] NULL 안전 — 부분 실패 경로에서도 호출 가능. */
		return;
	}
	free(uring->filename);                            /* [한국어] strdup 한 백킹 경로. */
	free(uring->bdev.name);                           /* [한국어] strdup 한 bdev 이름. */
	free(uring);                                      /* [한국어] 컨테이너 자체. */
}

/*
 * [한국어]
 * bdev_uring_group_create_cb - 모듈 io_device 채널 생성 콜백 — io_uring 인스턴스 init + 폴러 등록.
 *
 * @io_device: &uring_if (모듈 자기 자신).
 * @ctx_buf: bdev_uring_group_channel 메모리.
 * @return: 0 성공, -1 실패.
 *
 * 매 reactor 가 처음 uring bdev 채널을 요청할 때 한 번 호출. io_uring_setup syscall 발생.
 * IORING_SETUP_IOPOLL flag 는 의도적으로 미사용 — 커널 polled-mode 는 일부 백엔드만 지원.
 */
static int
bdev_uring_group_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_uring_group_channel *ch = ctx_buf;

	/* Do not use IORING_SETUP_IOPOLL until the Linux kernel can support not only
	 * local devices but also devices attached from remote target */
	if (io_uring_queue_init(SPDK_URING_QUEUE_DEPTH, &ch->uring, 0) < 0) {   /* [한국어] flags=0 — 표준 mode. 512 SQE/CQE 할당. */
		SPDK_ERRLOG("uring I/O context setup failure\n");
		return -1;
	}

	ch->poller = SPDK_POLLER_REGISTER(bdev_uring_group_poll, ch, 0);       /* [한국어] period_microseconds=0 → 매 reactor tick. */
	return 0;
}

/*
 * [한국어]
 * bdev_uring_group_destroy_cb - 모듈 io_device 채널 해제 콜백 — io_uring 종료 + 폴러 해제.
 */
static void
bdev_uring_group_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_uring_group_channel *ch = ctx_buf;

	io_uring_queue_exit(&ch->uring);                                       /* [한국어] munmap + ring fd close. */

	spdk_poller_unregister(&ch->poller);                                   /* [한국어] 폴 루프 제거 (다음 reactor tick 부터 호출 안 됨). */
}

/*
 * [한국어]
 * create_uring_bdev - RPC bdev_uring_create 본체 — uring bdev 인스턴스 생성·등록.
 *
 * @opts: 사용자 파라미터 (name, filename, block_size, uuid).
 * @return: 등록된 spdk_bdev 포인터 또는 NULL.
 *
 * 절차: alloc → open → 블록 크기 검증 (auto-detect 가능) → ZNS 검사 → bdev_register.
 * 모든 실패 경로는 error_return 으로 가서 close + free.
 * 컨텍스트: app thread (RPC).
 * 호출 체인: rpc_bdev_uring_create → create_uring_bdev
 *           → bdev_uring_open / bdev_uring_check_zoned_support
 *           → spdk_io_device_register / spdk_bdev_register.
 */
struct spdk_bdev *
create_uring_bdev(const struct bdev_uring_opts *opts)
{
	struct bdev_uring *uring;                           /* [한국어] 새로 할당할 인스턴스. */
	uint32_t detected_block_size;                       /* [한국어] OS 가 보고하는 블록 크기 (logical block size). */
	uint64_t bdev_size;                                 /* [한국어] 백킹 디바이스 총 바이트 크기. */
	int rc;                                             /* [한국어] 에러 코드. */
	uint32_t block_size = opts->block_size;             /* [한국어] 사용자 지정 블록 크기 (0 = autodetect). */

	uring = calloc(1, sizeof(*uring));                  /* [한국어] zero-fill 할당 — 모든 필드 0 으로 초기화. */
	if (!uring) {
		SPDK_ERRLOG("Unable to allocate enough memory for uring backend\n");
		return NULL;
	}

	uring->filename = strdup(opts->filename);           /* [한국어] 경로 소유 사본. */
	if (!uring->filename) {
		goto error_return;
	}

	uring->bdev.name = strdup(opts->name);              /* [한국어] bdev 이름 소유 사본. */
	if (!uring->bdev.name) {
		goto error_return;
	}

	if (bdev_uring_open(uring)) {                       /* [한국어] O_DIRECT|O_NOATIME open. */
		goto error_return;
	}

	bdev_size = spdk_fd_get_size(uring->fd);            /* [한국어] BLKGETSIZE64 or fstat 으로 크기 조회. */

	uring->bdev.product_name = "URING bdev";            /* [한국어] RPC info 에 표시될 product 이름. */
	uring->bdev.module = &uring_if;                     /* [한국어] 소속 모듈 — destruct 디스패치 기준. */

	uring->bdev.write_cache = 0;                        /* [한국어] write cache 없음 — O_DIRECT 라 OS 캐시 우회. */

	detected_block_size = spdk_fd_get_blocklen(uring->fd);  /* [한국어] BLKSSZGET (logical sector size). */
	if (block_size == 0) {
		/* User did not specify block size - use autodetected block size. */
		if (detected_block_size == 0) {             /* [한국어] 자동 검출 실패 — 일반 파일일 경우 사용자가 명시 필요. */
			URING_ERRLOG(uring, "Block size could not be auto-detected\n");
			goto error_return;
		}
		block_size = detected_block_size;
	} else {
		if (block_size < detected_block_size) {     /* [한국어] 사용자 값 < OS 값 — O_DIRECT 부정합. */
			URING_ERRLOG(uring, "Specified block size %" PRIu32 " is smaller than "
				     "auto-detected block size %" PRIu32 "\n",
				     block_size, detected_block_size);
			goto error_return;
		} else if (detected_block_size != 0 && block_size != detected_block_size) {
			URING_WARNLOG(uring, "Specified block size %" PRIu32 " does not match "
				      "auto-detected block size %" PRIu32 "\n",
				      block_size, detected_block_size);  /* [한국어] 더 크면 허용하되 경고 — 성능/정합성 영향. */
		}
	}

	if (block_size < 512) {                             /* [한국어] 512B 미만은 NVMe 스펙상 불가. */
		URING_ERRLOG(uring, "Invalid block size %" PRIu32 " (must be at least 512).\n", block_size);
		goto error_return;
	}

	if (!spdk_u32_is_pow2(block_size)) {                /* [한국어] 2의 거듭제곱이어야 alignment 매크로 사용 가능. */
		URING_ERRLOG(uring, "Invalid block size %" PRIu32 " (must be a power of 2.)\n", block_size);
		goto error_return;
	}

	uring->bdev.blocklen = block_size;                  /* [한국어] bdev 의 LBA 단위. */
	uring->bdev.required_alignment = spdk_u32log2(block_size);  /* [한국어] DMA 버퍼 정렬 요구사항 (log2). */

	rc = bdev_uring_check_zoned_support(uring, opts->name, opts->filename);  /* [한국어] ZNS 검사. */
	if (rc) {
		goto error_return;
	}

	if (bdev_size % uring->bdev.blocklen != 0) {        /* [한국어] 비정렬 크기 — 마지막 partial block 무시 위해 거부. */
		URING_ERRLOG(uring, "Disk size %" PRIu64 " is not a multiple of block size %" PRIu32 "\n",
			     bdev_size, uring->bdev.blocklen);
		goto error_return;
	}

	uring->bdev.blockcnt = bdev_size / uring->bdev.blocklen;  /* [한국어] 전체 블록 수 — bdev API 의 핵심 메타. */
	uring->bdev.ctxt = uring;                           /* [한국어] 콜백에서 본 구조체 복원용. */

	uring->bdev.fn_table = &uring_fn_table;             /* [한국어] vtable 연결 — 이후 lib/bdev 가 모든 호출에 사용. */

	if (!spdk_mem_all_zero(&opts->uuid, sizeof(opts->uuid))) {     /* [한국어] 사용자가 UUID 지정 시에만 복사 (zero = auto). */
		spdk_uuid_copy(&uring->bdev.uuid, &opts->uuid);
	}

	spdk_io_device_register(uring, bdev_uring_create_cb, bdev_uring_destroy_cb,
				sizeof(struct bdev_uring_io_channel),
				uring->bdev.name);                              /* [한국어] per-bdev io_device 등록 — 채널 ctx 크기와 콜백 명시. */
	rc = spdk_bdev_register(&uring->bdev);                                  /* [한국어] lib/bdev 에 정식 등록 — 이 시점부터 examine 등 진행. */
	if (rc) {
		spdk_io_device_unregister(uring, NULL);                         /* [한국어] 부분 실패 시 io_device 도 되돌림. */
		goto error_return;
	}

	TAILQ_INSERT_TAIL(&g_uring_bdev_head, uring, link);                     /* [한국어] 전역 enumeration 리스트에 추가. */
	return &uring->bdev;

error_return:
	bdev_uring_close(uring);                                                /* [한국어] open 했다면 close. */
	uring_free_bdev(uring);                                                 /* [한국어] 메모리 해제. */
	return NULL;
}

/* [한국어] 비동기 delete 흐름에서 콜백/인자 보관용 컨텍스트. */
struct delete_uring_bdev_ctx {
	spdk_delete_uring_complete cb_fn;
	/* [한국어] delete 완료 알림 콜백 (RPC 응답용).
	 * 설정자: delete_uring_bdev 가 호출자 인자로 받음.
	 * 읽는 자: uring_bdev_unregister_cb 가 unregister 완료 시 발사.
	 * 값 범위: NULL 가능 (콜백 불필요한 hot-remove 등).
	 * 동기화: app thread 에서만. */

	void *cb_arg;
	/* [한국어] cb_fn 의 첫 인자로 전달될 컨텍스트 (RPC 의 경우 jsonrpc_request).
	 * 설정자/읽는 자: delete_uring_bdev / uring_bdev_unregister_cb.
	 * 값 범위: 콜백 사용자가 정의. */
};

/*
 * [한국어]
 * uring_bdev_unregister_cb - spdk_bdev_unregister_by_name 의 완료 콜백.
 *
 * @arg: delete_uring_bdev_ctx*.
 * @bdeverrno: 0 성공, 음수 errno 실패.
 */
static void
uring_bdev_unregister_cb(void *arg, int bdeverrno)
{
	struct delete_uring_bdev_ctx *ctx = arg;                         /* [한국어] 자기 컨텍스트 복원. */

	if (ctx->cb_fn) {                                                /* [한국어] 호출자가 콜백 지정했으면 발사. */
		ctx->cb_fn(ctx->cb_arg, bdeverrno);
	}
	free(ctx);                                                       /* [한국어] 컨텍스트 해제. */
}

/*
 * [한국어]
 * delete_uring_bdev - RPC bdev_uring_delete 본체 + hot-remove 경로의 공용 진입.
 *
 * @name: 삭제할 bdev 이름.
 * @cb_fn/@cb_arg: 완료 콜백 (선택).
 *
 * spdk_bdev_unregister_by_name 으로 lib/bdev 에 unregister 요청 → 모든 채널이 닫히고
 * desc 가 0 이 되면 destruct 콜백 발사 → uring_bdev_unregister_cb.
 * 컨텍스트: app thread.
 */
void
delete_uring_bdev(const char *name, spdk_delete_uring_complete cb_fn, void *cb_arg)
{
	struct delete_uring_bdev_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));                                   /* [한국어] 비동기 라이프타임 ctx 할당. */
	if (ctx == NULL) {
		if (cb_fn) {
			cb_fn(cb_arg, -ENOMEM);                          /* [한국어] OOM — 즉시 실패 통보. */
		}
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	rc = spdk_bdev_unregister_by_name(name, &uring_if, uring_bdev_unregister_cb, ctx);  /* [한국어] 이름+모듈로 안전하게 매칭. */
	if (rc != 0) {                                                   /* [한국어] 이름 없음/다른 모듈 등 — 동기 실패. */
		uring_bdev_unregister_cb(ctx, rc);                       /* [한국어] 콜백 경로로 통일하여 ctx 도 함께 정리. */
	}
}

/*
 * [한국어]
 * bdev_uring_init - 모듈 초기화 (SPDK_BDEV_MODULE_REGISTER .module_init).
 *
 * @return: 0 성공.
 *
 * 모듈 io_device (group channel) 만 등록. 실제 io_uring 인스턴스는 채널 create_cb 에서.
 */
static int
bdev_uring_init(void)
{
	spdk_io_device_register(&uring_if, bdev_uring_group_create_cb, bdev_uring_group_destroy_cb,
				sizeof(struct bdev_uring_group_channel), "uring_module");  /* [한국어] reactor 별 io_uring 컨텍스트 인프라. */

	return 0;
}

/*
 * [한국어]
 * bdev_uring_fini - 모듈 종료 (SPDK_BDEV_MODULE_REGISTER .module_fini).
 *
 * 모듈 io_device 해제 — 등록 시 콜백 NULL 이므로 callback 없이 즉시.
 */
static void
bdev_uring_fini(void)
{
	spdk_io_device_unregister(&uring_if, NULL);                      /* [한국어] 마지막 정리. */
}

/* [한국어] SPDK log 컴포넌트 등록 — DEBUGLOG 토글 단위 ('uring' 카테고리). */
SPDK_LOG_REGISTER_COMPONENT(uring)
