/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK ublk (User-mode Block) target 구현 (ublk.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK가 호스트 커널의 ublk_drv(Linux 6.0+에서 추가된 io_uring 기반 user-mode
 * block 드라이버)와 연동하여 임의의 SPDK bdev를 /dev/ublkbN 블록 디바이스로 노출하는
 * target 구현이다. ublk는 NBD와 달리 NETLINK/소켓이 아닌 두 개의 io_uring(컨트롤 + 데이터)
 * 큐로 커널과 통신하며, 데이터 경로는 zero-copy(user_copy) 또는 미리 등록한 buffer로
 * 처리한다. 이 파일은:
 *   1) /dev/ublk-control ioctl/io_uring 인터페이스로 ublk 디바이스를 add/start/del.
 *   2) per-queue thread/poll_group 모델로 ublk 디바이스의 N개 큐를 SPDK reactor들에 분산.
 *   3) ublk_drv가 보내는 IO 명령(uring CQE)을 dequeue → SPDK bdev_read/write/unmap으로 변환
 *      → 완료 시 commit-and-fetch (UBLK_IO_COMMIT_AND_FETCH_REQ) uring SQE로 결과 보고.
 *   4) recovery 기능 — ublk 드라이버 재시작 시 미완료 IO를 복구.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [Linux 사용자 프로세스: dd / mkfs / mount /dev/ublkb0]
 *     → 커널 ublk_drv → io_uring (per-queue 데이터 uring) → SPDK reactor의 ublk_io_poll
 *     → ublk_queue_io_cmd → 본 파일 함수 → spdk_bdev_readv/writev/unmap (bdev 레이어)
 *     → 완료 콜백 → io_uring SQE로 commit → 커널이 사용자 read/write 시스템콜에 응답
 * 실행 컨텍스트:
 *   - 컨트롤 plane (디바이스 add/del/start): g_ublk_tgt.ctrl_thread (1개 thread).
 *   - 데이터 plane (IO 처리): 각 ublk_queue가 하나의 poll_group(=SPDK thread)에 할당.
 *     queue 단위로 thread-affined → lockless.
 * polled-mode: ublk_poll이 등록된 SPDK poller로 io_uring CQE를 무한 폴링.
 *
 * === 타 모듈과의 연결 ===
 * - liburing: io_uring SQE/CQE 조작 API.
 * - linux/ublk_cmd.h: ublk_drv UAPI (ublksrv_ctrl_cmd, ublksrv_io_cmd, UBLK_IO_OP_*).
 * - spdk/bdev.h: spdk_bdev_open_ext, spdk_bdev_readv/writev/unmap/flush 호출.
 * - spdk/thread.h: spdk_io_channel, spdk_poller, spdk_thread_send_msg.
 * - spdk/event.h: spdk_for_each_thread (이전 버전) / iobuf.
 * - ublk_internal.h: 내부 자료구조 (spdk_ublk_dev, ublk_queue, ublk_io, ublk_poll_group).
 * - ublk_rpc.c: JSON-RPC 핸들러 (rpc_ublk_create_target / start_disk / stop_disk).
 * 데이터 흐름:
 *   커널 ublk_drv 큐 → io_uring CQE → ublk_io 구조체 → bdev_io 발행 → 완료 → uring SQE 응답.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_ublk_init / fini: 라이브러리 초기화/종료, /dev/ublk-control 열기.
 * - ublk_create_target / destroy_target: ublk_drv 초기화.
 * - spdk_ublk_start_disk / stop_disk: bdev → /dev/ublkbN 디바이스 노출/제거.
 * - ublk_poll: 각 poll_group의 io_uring CQE 폴링 (SPDK poller).
 * - ublk_queue_io_cmd: 커널이 보낸 IO 요청을 SPDK bdev I/O로 변환.
 * - ublk_io_done: bdev 완료 콜백 → commit-and-fetch SQE로 커널에 응답.
 * - struct spdk_ublk_dev: ublk 디바이스 인스턴스 (큐 배열, bdev_desc, dev_id 등).
 * - struct ublk_queue: per-queue 컨텍스트 (uring, ublk_io 배열, IO 카운터).
 * - struct ublk_io: 개별 IO 요청 (커널 cmd, bdev_io, payload pointer).
 */

#include <liburing.h>
/* [한국어] liburing — io_uring 사용자 라이브러리. ublk_drv 와 통신하는 두 종류의 ring
 * (control ring + per-queue data ring) 의 SQE 빌드/제출, CQE peek/seen, 파라미터 setup 등을
 * 제공. ublk 는 NBD 와 달리 소켓이 아닌 io_uring 으로 커널과 통신하므로 필수 의존성이다. */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 include 묶음 — open/close/mmap/getpid/errno 등 POSIX 함수와 stdint 타입. */
#include "spdk/string.h"
/* [한국어] spdk_strerror (errno → 문자열), snprintf 래퍼 등 문자열 유틸 — 에러 로그에 사용. */
#include "spdk/bdev.h"
/* [한국어] spdk_bdev_open_ext / read/write/unmap/flush_blocks / get_io_channel 등 bdev 공개
 * API — ublk 가 노출하는 백엔드는 임의의 SPDK bdev 이므로 핵심 의존성. */
#include "spdk/endian.h"
/* [한국어] 빅/리틀 엔디언 변환 헬퍼 — ublk UAPI 의 일부 필드 처리에 사용. */
#include "spdk/env.h"
/* [한국어] DPDK 추상화 — spdk_env_get_cpuset / get_core_count / SPDK_ENV_FOREACH_CORE 로
 * poll_group 을 배치할 코어 집합을 결정. */
#include "spdk/likely.h"
/* [한국어] spdk_likely/spdk_unlikely — hot path (ublk_poll, io_recv) 의 분기 예측 힌트. */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG/NOTICELOG/DEBUGLOG + SPDK_LOG_REGISTER_COMPONENT (파일 끝의 ublk/ublk_io). */
#include "spdk/util.h"
/* [한국어] SPDK_CONTAINEROF, spdk_u32log2, spdk_max, SPDK_ALIGN_CEIL 등 — 비트/포인터 유틸. */
#include "spdk/queue.h"
/* [한국어] TAILQ_* 매크로 — g_ublk_devs / queue_list / inflight·completed_io_list 등 연결 리스트. */
#include "spdk/json.h"
/* [한국어] spdk_json_write_* — spdk_ublk_write_config_json 의 RPC 설정 직렬화. */
#include "spdk/ublk.h"
/* [한국어] ublk 라이브러리의 공개 헤더 — spdk_ublk_init/fini, fini_cb typedef 선언. */
#include "spdk/thread.h"
/* [한국어] spdk_thread / poller / io_channel / send_msg / iobuf — per-queue thread affinity 와
 * cross-thread 메시지 패싱, polled-mode poller 등록의 기반. */
#include "spdk/file.h"
/* [한국어] spdk_read_sysfs_attribute_uint32 — /sys/module/ublk_drv/parameters 읽기에 사용. */

#include "ublk_internal.h"
/* [한국어] 본 라이브러리 내부 헤더 — ublk_ctrl_cb typedef, ublk_start_disk / stop_disk /
 * create_target / destroy_target 등 ublk_rpc.c 와 공유하는 내부 함수 프로토타입. */

#define UBLK_CTRL_DEV					"/dev/ublk-control"
/* [한국어] control plane character device 경로 — 디바이스 add/del/start/stop 명령을 보내는
 * ctrl io_uring 의 fd 가 이 노드를 연다 (open()). ublk_drv 모듈 로드 시 udev 가 생성. */
#define UBLK_BLK_CDEV					"/dev/ublkc"
/* [한국어] per-device character device 경로 prefix — 실제 사용은 "/dev/ublkc<N>" 형태.
 * 이 노드의 fd 를 io_cmd_buf mmap 과 data ring 의 fixed file 로 사용. (/dev/ublkb<N> 은
 * 사용자가 실제 read/write 하는 block device 이고, /dev/ublkc<N> 은 SPDK 가 쓰는 제어용.) */

#define LINUX_SECTOR_SHIFT				9
/* [한국어] Linux block layer 의 고정 sector 크기 512B = 2^9. ublk_drv 의 io_desc 는 항상
 * 512B sector 단위 — bdev 의 logical block (보통 4KB) 과 변환 시 이 shift 를 사용. */
#define UBLK_IO_MAX_BYTES				SPDK_BDEV_LARGE_BUF_MAX_SIZE
/* [한국어] 단일 IO 최대 바이트 = bdev large iobuf 한 칸 크기. dev_info.max_io_buf_bytes 와
 * dev_params.basic.max_sectors 에 반영 — 커널이 이보다 큰 IO 를 잘라 보내도록. */
#define UBLK_DEV_MAX_QUEUES				32
/* [한국어] 디바이스당 큐 수 상한 — spdk_ublk_dev.queues[] 배열 크기. 큐 수는 보통 코어 수에
 * 맞춰 설정되며 32 면 충분. start_disk 가 이 값으로 클램프. */
#define UBLK_DEV_MAX_QUEUE_DEPTH			1024
/* [한국어] 큐당 깊이 상한 — 동시 inflight IO 수의 최대치. ios[] 배열 크기를 좌우. */
#define UBLK_QUEUE_REQUEST				32
/* [한국어] ublk_io_recv 한 번에 처리할 CQE 수 batching 한계 — 한 큐가 CPU 를 독점해 다른
 * 큐를 starvation 시키지 않도록 32 개 처리 후 break, 다음 poll iteration 에 재개. */
#define UBLK_STOP_BUSY_WAITING_MS			10000
/* [한국어] 디바이스 stop 시 ctrl 명령 완료를 기다리는 최대 시간(ms) — _ublk_close_dev_retry
 * 의 retry_count 산정에 사용 (이 시간 내 응답 없으면 강제 진행). */
#define UBLK_BUSY_POLLING_INTERVAL_US			20000
/* [한국어] stop 대기 retry poller 의 폴링 주기(us, 20ms). 완료 대기는 빈번할 필요 없어 느리게. */
#define UBLK_DEFAULT_CTRL_URING_POLLING_INTERVAL_US	1000
/* [한국어] ctrl_poller 의 폴링 주기(us, 1ms). control plane 은 빈도가 낮아 1ms 면 충분히 빠르고
 * CPU 소모도 낮음 (data plane 의 ublk_poll 은 interval 0 = 매 reactor iteration). */
/* By default, kernel ublk_drv driver can support up to 64 block devices */
#define UBLK_DEFAULT_MAX_SUPPORTED_DEVS			64
/* [한국어] g_ublks_max 의 기본값 — kernel ublk_drv 의 ublks_max 파라미터 기본치. ublk_open 이
 * /sys 에서 실제 값을 읽어 덮어쓸 수 있음. */

#define UBLK_IOBUF_SMALL_CACHE_SIZE			128
/* [한국어] poll_group 의 iobuf 채널 small buffer 풀 per-thread 캐시 크기 — user_copy 모드에서
 * IO data buffer 를 빌릴 때의 lockless 로컬 캐시 엔트리 수. */
#define UBLK_IOBUF_LARGE_CACHE_SIZE			32
/* [한국어] iobuf 채널 large buffer 풀 per-thread 캐시 크기 (large IO 용). */

#define UBLK_DEBUGLOG(ublk, format, ...) \
	SPDK_DEBUGLOG(ublk, "ublk%d: " format, ublk->ublk_id, ##__VA_ARGS__);
/* [한국어] 디바이스 ID 접두("ublk%d: ")를 자동으로 붙이는 디버그 로그 래퍼 — 다중 디바이스
 * 환경에서 로그를 디바이스별로 구분하기 위함. SPDK_DEBUGLOG 는 'ublk' 컴포넌트 플래그가
 * 켜졌을 때만 출력 (파일 끝 SPDK_LOG_REGISTER_COMPONENT(ublk)). */

static uint32_t g_num_ublk_poll_groups = 0;
/* [한국어] 생성된 poll_group(=ublk thread) 개수.
 * 설정자: ublk_create_target 이 cpumask 안 코어마다 +1. 읽는 자: 큐 round-robin 분배 (g_next
 * wrap 기준), fini 시 순회 한계.
 * 값 범위: 0 ~ spdk_env_get_core_count. 동기화: app_thread 만 변경/읽음 → 별도 락 불필요. */
static uint32_t g_next_ublk_poll_group = 0;
/* [한국어] 다음 큐를 배정할 poll_group 인덱스 (round-robin 커서).
 * 설정자/읽는 자: ublk_start_dev 가 큐마다 읽고 ++ 후 g_num_ublk_poll_groups 에서 wrap.
 * 동기화: app_thread 만 접근 → 락 불필요. */
static uint32_t g_ublks_max = UBLK_DEFAULT_MAX_SUPPORTED_DEVS;
/* [한국어] 동시에 만들 수 있는 ublk 디바이스 수 상한.
 * 설정자: ublk_open 이 /sys/module/ublk_drv/parameters/ublks_max 를 읽어 덮어씀.
 * 읽는 자: ublk_start_disk 가 num_ublk_devs 와 비교해 한도 초과 검사. */
static struct spdk_cpuset g_core_mask;
/* [한국어] poll_group 을 배치할 CPU 코어 집합.
 * 설정자: ublk_parse_core_mask (RPC cpumask 인자 파싱). 읽는 자: create_target 의
 * SPDK_ENV_FOREACH_CORE 필터, write_config_json. 동기화: app_thread 한정. */
static bool g_disable_user_copy = false;
/* [한국어] true 면 UBLK_F_USER_COPY 비활성 — 모든 디바이스가 미리 등록된 buffer 경로 사용.
 * 설정자: ublk_create_target(disable_user_copy 인자). 읽는 자: ublk_ctrl_cmd_get_features 가
 * 커널 user_copy 지원과 AND. 런타임 변경 불가 (target 단위 고정). */

struct ublk_queue;       /* [한국어] 전방 선언 — 아래 헬퍼들이 정의보다 먼저 참조. */
struct ublk_poll_group;  /* [한국어] 전방 선언 — poll_group 구조체. */
struct ublk_io;          /* [한국어] 전방 선언 — 개별 IO 요청 구조체. */
static void _ublk_submit_bdev_io(struct ublk_queue *q, struct ublk_io *io);
/* [한국어] 전방 선언 — ublk_resubmit_io / read_get_buffer_done 가 정의보다 먼저 호출. */
static void ublk_dev_queue_fini(struct ublk_queue *q);
/* [한국어] 전방 선언 — ublk_delete_dev 가 정의보다 먼저 호출. */
static int ublk_poll(void *arg);
/* [한국어] 전방 선언 — ublk_poller_register 가 SPDK_POLLER_REGISTER 인자로 먼저 참조. */

static int ublk_set_params(struct spdk_ublk_dev *ublk);
/* [한국어] 전방 선언 — ublk_ctrl_process_cqe(ADD_DEV 응답) 가 먼저 호출. */
static int ublk_start_dev(struct spdk_ublk_dev *ublk, bool is_recovering);
/* [한국어] 전방 선언 — ublk_ctrl_process_cqe(SET_PARAMS/START_USER_RECOVERY 응답) 가 먼저 호출. */
static void ublk_free_dev(struct spdk_ublk_dev *ublk);
/* [한국어] 전방 선언 — ublk_ctrl_cmd_error / start_disk 에러 경로가 먼저 호출. */
static void ublk_delete_dev(void *arg);
/* [한국어] 전방 선언 — ublk_ctrl_cmd_error / _ublk_close_dev_retry 가 먼저 호출. */
static int ublk_close_dev(struct spdk_ublk_dev *ublk);
/* [한국어] 전방 선언 — ublk_ctrl_cmd_error / _ublk_fini / stop_disk 가 먼저 호출. */
static int ublk_ctrl_start_recovery(struct spdk_ublk_dev *ublk);
/* [한국어] 전방 선언 — ublk_ctrl_process_cqe(GET_DEV_INFO 응답) 가 먼저 호출. */

static int ublk_ctrl_cmd_submit(struct spdk_ublk_dev *ublk, uint32_t cmd_op);
/* [한국어] 전방 선언 — 거의 모든 control plane 호출자가 정의보다 먼저 사용하는 중심 함수. */

/* [한국어] ublk control/IO 명령 opcode → 이름 문자열 매핑 테이블 (designated initializer).
 * 인덱스는 opcode 숫자, 값은 로그용 문자열. 미정의 슬롯은 NULL. ublk_ctrl_cmd_error /
 * process_cqe 가 current_cmd_op 로 인덱싱해 사람이 읽을 로그를 출력. 64 칸 고정 (opcode 6비트). */
static const char *ublk_op_name[64] = {
	[UBLK_CMD_GET_DEV_INFO] = "UBLK_CMD_GET_DEV_INFO",
	/* [한국어] 커널로부터 dev_info(상태/큐수/flags) 조회 — recovery 진입 시 사용. */
	[UBLK_CMD_ADD_DEV] =	"UBLK_CMD_ADD_DEV",
	/* [한국어] 새 ublk 디바이스 커널 등록 (dev_info in/out). start_disk chain 의 첫 명령. */
	[UBLK_CMD_DEL_DEV] =	"UBLK_CMD_DEL_DEV",
	/* [한국어] 디바이스 커널 제거 — destroy 마지막 단계. */
	[UBLK_CMD_START_DEV] =	"UBLK_CMD_START_DEV",
	/* [한국어] /dev/ublkbN 활성화 — owner pid 전달, 이후 사용자 IO 가능. */
	[UBLK_CMD_STOP_DEV] =	"UBLK_CMD_STOP_DEV",
	/* [한국어] 디바이스 중지 — 사용자 IO 차단, 진행 IO drain 시작. */
	[UBLK_CMD_SET_PARAMS] =	"UBLK_CMD_SET_PARAMS",
	/* [한국어] dev_params(블록크기/max_sectors/discard) 커널 적용 — ADD 다음 단계. */
	[UBLK_CMD_START_USER_RECOVERY] = "UBLK_CMD_START_USER_RECOVERY",
	/* [한국어] 재시작 후 복구 시작 — 커널이 quiesce 된 디바이스 재초기화 준비. */
	[UBLK_CMD_END_USER_RECOVERY] = "UBLK_CMD_END_USER_RECOVERY",
	/* [한국어] 복구 완료 통지 — 모든 큐가 다시 fetch SQE 를 걸면 발행. */
};

typedef void (*ublk_get_buf_cb)(struct ublk_io *io);
/* [한국어] iobuf 풀에서 buffer를 비동기로 얻은 뒤 호출될 콜백 typedef.
 * 사용: ublk_io->get_buf_cb 필드. iobuf 가용 시 ublk_resubmit_get_buf가 트리거. */

/*
 * [한국어]
 * struct ublk_io — 단일 ublk IO 요청 표현체.
 * 큐의 ios[] 배열에 미리 할당되어 슬롯 단위로 재사용. 한 IO는 ublk_drv가 보낸 ublksrv_io_desc
 * → 본 구조체로 매핑 → spdk_bdev_io 발행 → 완료 후 UBLK_IO_COMMIT_AND_FETCH_REQ로 응답.
 */
struct ublk_io {
	void			*payload;
	/* [한국어] IO data buffer 포인터 — user_copy=false면 미리 등록된 buffer,
	 * true면 iobuf에서 빌린 buffer. 설정자: ublk_resubmit_get_buf. 읽는 자: bdev_io 발행 시 iovec 구성. */
	void			*mpool_entry;
	/* [한국어] iobuf 풀에서 빌린 entry 추적용 — 완료 후 spdk_iobuf_put으로 반환. */
	bool			need_data;
	/* [한국어] true면 데이터 buffer 필요 (read/write 등). false는 flush 등 데이터 없는 op. */
	bool			user_copy;
	/* [한국어] UBLK_F_USER_COPY 사용 여부 — true면 kernel이 user buffer로 직접 copy,
	 * SPDK는 자체 buffer를 사용. */
	uint16_t		tag;
	/* [한국어] IO 요청 식별자 — ios[] 배열 인덱스와 동일. CQE user_data에서 추출. */
	uint64_t		payload_size;
	/* [한국어] data buffer 크기 (바이트). bdev I/O에 전달. */
	uint32_t		cmd_op;
	/* [한국어] ublk 명령 opcode (UBLK_IO_FETCH_REQ/COMMIT_AND_FETCH_REQ/UBLK_IO_NEED_GET_DATA 등). */
	int32_t			result;
	/* [한국어] bdev IO 결과 — 0 또는 음수 errno. UBLK_IO_COMMIT_AND_FETCH_REQ SQE에 첨부. */
	struct spdk_bdev_desc	*bdev_desc;
	/* [한국어] bdev open descriptor — 디바이스의 bdev_desc 캐시. */
	struct spdk_io_channel	*bdev_ch;
	/* [한국어] 이 큐의 bdev io_channel. spdk_bdev_*v 호출에 사용. */
	const struct ublksrv_io_desc	*iod;
	/* [한국어] 커널 ublk_drv가 보낸 IO descriptor (op, offset, len, addr 등). */
	ublk_get_buf_cb		get_buf_cb;
	/* [한국어] iobuf 비동기 획득 완료 콜백. */
	struct ublk_queue	*q;
	/* [한국어] 소속 큐 back-pointer. */
	/* for bdev io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	/* [한국어] bdev I/O 발행 시 자원 부족으로 EAGAIN을 받으면 이 entry로 wait 큐에 매달려 재시도. */
	struct spdk_iobuf_entry	iobuf;
	/* [한국어] iobuf 풀 wait 큐 entry — buffer 가용 대기. */

	TAILQ_ENTRY(ublk_io)	tailq;
	/* [한국어] completed_io_list 또는 inflight_io_list에 매달리는 링크. */
};

/*
 * [한국어]
 * struct ublk_queue — 한 ublk 디바이스의 한 큐 컨텍스트.
 * 디바이스당 N개(num_queues) 큐가 poll_group들에 분산 배치되어 lockless 처리.
 */
struct ublk_queue {
	uint32_t		q_id;
	/* [한국어] 큐 인덱스 (0 ~ num_queues-1). */
	uint32_t		q_depth;
	/* [한국어] 이 큐의 깊이 = ublk_drv가 협상한 queue depth. ios[] 크기와 동일. */
	struct ublk_io		*ios;
	/* [한국어] IO 슬롯 배열 (calloc(q_depth)). */
	TAILQ_HEAD(, ublk_io)	completed_io_list;
	/* [한국어] 완료된 IO 큐 — commit-and-fetch SQE 발행 대기. */
	TAILQ_HEAD(, ublk_io)	inflight_io_list;
	/* [한국어] 현재 처리 중인 IO 큐. */
	uint32_t		cmd_inflight;
	/* [한국어] 현재 inflight 명령 수 (디버깅/통계). */
	bool			is_stopping;
	/* [한국어] true면 신규 IO 거부 + 진행 중 IO 완료 대기 중. */
	struct ublksrv_io_desc	*io_cmd_buf;
	/* [한국어] 커널과 공유되는 IO descriptor 배열 (mmap된 영역). */
	/* ring depth == dev_info->queue_depth. */
	struct io_uring		ring;
	/* [한국어] 이 큐 전용 io_uring — 커널과 비동기 IO command 교환. */
	struct spdk_ublk_dev	*dev;
	/* [한국어] 소속 디바이스 back-pointer. */
	struct ublk_poll_group	*poll_group;
	/* [한국어] 이 큐를 폴링하는 poll_group (할당된 SPDK thread). */
	struct spdk_io_channel	*bdev_ch;
	/* [한국어] poll_group thread 위의 bdev io_channel. */

	TAILQ_ENTRY(ublk_queue)	tailq;
	/* [한국어] poll_group->queue_list 링크. */
};

/*
 * [한국어]
 * struct spdk_ublk_dev — ublk 디바이스 인스턴스 (= /dev/ublkbN 하나).
 * RPC ublk_start_disk로 생성, ublk_stop_disk로 파괴.
 */
struct spdk_ublk_dev {
	struct spdk_bdev	*bdev;
	/* [한국어] 백엔드 bdev 포인터. */
	struct spdk_bdev_desc	*bdev_desc;
	/* [한국어] bdev open descriptor. spdk_bdev_open_ext로 획득. */

	int			cdev_fd;
	/* [한국어] /dev/ublkcN character device fd — io_cmd_buf 매핑 등에 사용. */
	struct ublk_params	dev_params;
	/* [한국어] 디바이스 파라미터 (blocksize, max_sectors 등). */
	struct ublksrv_ctrl_dev_info	dev_info;
	/* [한국어] ublk_drv가 광고한 디바이스 정보 (num_queues, queue_depth, flags 등). */

	uint32_t		ublk_id;
	/* [한국어] 디바이스 인덱스 (= /dev/ublkb<N>의 N). */
	uint32_t		num_queues;
	/* [한국어] 큐 개수. */
	uint32_t		queue_depth;
	/* [한국어] 큐당 깊이. */
	uint32_t		online_num_queues;
	/* [한국어] 실제 활성화된 큐 수 — start 진행 추적. */
	uint32_t		sector_per_block_shift;
	/* [한국어] bdev block과 ublk sector(512B) 사이 변환 shift. */
	struct ublk_queue	queues[UBLK_DEV_MAX_QUEUES];
	/* [한국어] 큐 배열 (최대 32개). */

	struct spdk_poller	*retry_poller;
	/* [한국어] start 재시도 poller — 커널이 device 준비 안 됐을 때 주기적 재시도. */
	int			retry_count;
	/* [한국어] 남은 재시도 카운트. */
	uint32_t		queues_closed;
	/* [한국어] 닫힌 큐 수 — stop 진행 추적. */
	ublk_ctrl_cb		ctrl_cb;
	/* [한국어] ctrl io_uring 명령 완료 콜백. */
	void			*cb_arg;
	/* [한국어] ctrl_cb 인자. */
	uint32_t		current_cmd_op;
	/* [한국어] 현재 진행 중인 ctrl 명령 op (디버깅). */
	uint32_t		ctrl_ops_in_progress;
	/* [한국어] 진행 중인 ctrl 명령 수. */
	bool			is_closing;
	/* [한국어] true면 destroy 진행 중. */
	bool			is_recovering;
	/* [한국어] true면 ublk_drv 재시작 후 recovery 모드 (UBLK_F_USER_RECOVERY). */

	TAILQ_ENTRY(spdk_ublk_dev) tailq;
	/* [한국어] g_ublk_devs 전역 리스트 링크. */
	TAILQ_ENTRY(spdk_ublk_dev) wait_tailq;
	/* [한국어] 재시도 대기 큐 링크. */
};

/*
 * [한국어]
 * struct ublk_poll_group — IO 큐를 폴링하는 SPDK thread.
 * 부팅 시 g_num_ublk_poll_groups 개수만큼 생성. round-robin으로 큐를 분배.
 */
struct ublk_poll_group {
	struct spdk_thread		*ublk_thread;
	/* [한국어] 이 poll_group이 도는 SPDK thread. */
	struct spdk_poller		*ublk_poller;
	/* [한국어] io_uring CQE 폴링 poller (ublk_poll). */
	struct spdk_iobuf_channel	iobuf_ch;
	/* [한국어] data buffer 풀 채널 — user_copy 모드에서 IO buffer 빌림용. */
	TAILQ_HEAD(, ublk_queue)	queue_list;
	/* [한국어] 이 poll_group이 담당하는 ublk_queue 리스트. */
};

/*
 * [한국어]
 * struct ublk_tgt — ublk target 글로벌 컨텍스트 (싱글톤).
 * spdk_ublk_init이 g_ublk_tgt를 초기화. /dev/ublk-control + 모든 poll_group을 보유.
 */
struct ublk_tgt {
	int			ctrl_fd;
	/* [한국어] /dev/ublk-control fd — ctrl io_uring과 ioctl 발행에 사용. */
	bool			active;
	/* [한국어] true면 target 활성 (init 완료, fini 진행 전). */
	bool			is_destroying;
	/* [한국어] true면 fini 진행 중. */
	spdk_ublk_fini_cb	cb_fn;
	/* [한국어] fini 완료 시 호출할 사용자 콜백. */
	void			*cb_arg;
	/* [한국어] cb_fn 인자. */
	struct io_uring		ctrl_ring;
	/* [한국어] 컨트롤 명령용 io_uring — add/start/del/recover_start/recover_done 등 발행. */
	struct spdk_poller	*ctrl_poller;
	/* [한국어] ctrl_ring CQE 폴링 poller (ctrl_thread에서 동작). */
	uint32_t		ctrl_ops_in_progress;
	/* [한국어] 진행 중인 ctrl 명령 수 — fini 시 0 대기. */
	struct ublk_poll_group	*poll_groups;
	/* [한국어] poll_group 배열 (g_num_ublk_poll_groups 개). */
	uint32_t		num_ublk_devs;
	/* [한국어] 현재 활성 디바이스 수. */
	uint64_t		features;
	/* [한국어] ublk_drv가 광고한 feature 비트맵 — UBLK_F_*. */
	/* `ublk_drv` supports UBLK_F_CMD_IOCTL_ENCODE */
	bool			ioctl_encode;
	/* [한국어] kernel 6.2+의 IOCTL_ENCODE 지원 여부. */
	/* `ublk_drv` supports UBLK_F_USER_COPY */
	bool			user_copy;
	/* [한국어] kernel 6.5+의 USER_COPY 지원 여부 (zero-copy 데이터 전달). */
	/* `ublk_drv` supports UBLK_F_USER_RECOVERY */
	bool			user_recovery;
	/* [한국어] kernel의 USER_RECOVERY 지원 여부 — ublk_drv 재시작 후 inflight IO 복구. */
};

static TAILQ_HEAD(, spdk_ublk_dev) g_ublk_devs = TAILQ_HEAD_INITIALIZER(g_ublk_devs);
static struct ublk_tgt g_ublk_tgt;

/* helpers for using io_uring */
/*
 * [한국어]
 * ublk_setup_ring - io_uring 인스턴스를 ublk 용 파라미터로 초기화하는 래퍼.
 *
 * @depth: SQ/CQ 엔트리 수 (큐 깊이).
 * @r: 초기화할 io_uring 핸들 (출력).
 * @flags: IORING_SETUP_* 플래그 (호출자가 SQE128 또는 SQPOLL 등 지정).
 * @return: 0 성공, 음수 errno (io_uring_queue_init_params 결과 그대로).
 *
 * ctrl ring(ublk_open)과 per-queue data ring(ublk_dev_queue_init) 양쪽이 공통으로 사용.
 * IORING_SETUP_CQSIZE 를 강제로 OR 하여 CQ 크기를 depth 로 명시 — 기본값(SQ의 2배)이 아니라
 * SQ 와 동일 크기로 고정해 메모리 사용을 줄인다.
 * 실행 컨텍스트: ring 을 소유할 thread(ctrl=app_thread, data=poll_group thread)에서 호출.
 *
 * 호출 체인:
 *   ublk_open / ublk_dev_queue_init → [본 함수] → io_uring_queue_init_params (커널 io_uring_setup)
 */
static inline int
ublk_setup_ring(uint32_t depth, struct io_uring *r, unsigned flags)
{
	struct io_uring_params p = {};   /* [한국어] setup 파라미터 — 0 초기화 후 필요 필드만 채움. */

	p.flags = flags | IORING_SETUP_CQSIZE;  /* [한국어] CQSIZE 강제 — cq_entries 를 명시값으로. */
	p.cq_entries = depth;            /* [한국어] CQ 엔트리 수 = depth (SQ 와 동일 크기로 고정). */

	return io_uring_queue_init_params(depth, r, &p);  /* [한국어] 커널 io_uring_setup 시스템콜 래핑. */
}

/*
 * [한국어]
 * ublk_uring_get_sqe - SQE128 모드에서 인덱스로 SQE 슬롯 주소를 직접 계산.
 *
 * @r: data io_uring 핸들.
 * @idx: 논리 SQE 인덱스 (0 ~ q_depth-1).
 * @return: 해당 인덱스의 io_uring_sqe 포인터.
 *
 * ublk data ring 은 IORING_SETUP_SQE128(128B SQE)로 만들어졌다. liburing 의 일반
 * io_uring_get_sqe 는 다음 빈 슬롯을 순차 반환하지만, ublk_dev_init_io_cmds 는 모든 슬롯의
 * 불변 필드를 미리 채우려고 임의 인덱스 접근이 필요하다. 128B SQE 는 표준 64B SQE 두 칸을
 * 차지하므로 배열 인덱스를 idx<<1(×2)로 변환해야 올바른 슬롯을 가리킨다.
 * 실행 컨텍스트: 해당 큐를 소유한 poll_group thread.
 *
 * 호출 체인:
 *   ublk_dev_init_io_cmds → [본 함수] → &r->sq.sqes[idx<<1]
 */
static inline struct io_uring_sqe *
ublk_uring_get_sqe(struct io_uring *r, uint32_t idx)
{
	/* Need to update the idx since we set IORING_SETUP_SQE128 parameter in ublk_setup_ring */
	/* [한국어] 128B SQE = 64B 슬롯 2칸 → 논리 인덱스를 ×2 해 실제 배열 오프셋으로 변환. */
	return &r->sq.sqes[idx << 1];
}

/*
 * [한국어]
 * ublk_get_sqe_cmd - SQE 내부의 ublk 명령 페이로드(inline command) 시작 주소 반환.
 *
 * @sqe: 대상 io_uring_sqe.
 * @return: SQE 의 addr3 필드 이후 영역 = uring_cmd payload 포인터.
 *
 * IORING_OP_URING_CMD 는 SQE 의 뒤쪽 영역(SQE128 의 확장 64B)에 디바이스별 명령 구조체를
 * 인라인으로 싣는다. ublk 의 경우 ublksrv_ctrl_cmd(control) 또는 ublksrv_io_cmd(data)를
 * 이 위치에 채운다. addr3 는 그 인라인 영역의 시작 오프셋.
 * 실행 컨텍스트: ring 소유 thread.
 *
 * 호출 체인:
 *   ublk_ctrl_cmd_submit / ublksrv_queue_io_cmd → [본 함수]
 */
static inline void *
ublk_get_sqe_cmd(struct io_uring_sqe *sqe)
{
	return (void *)&sqe->addr3;  /* [한국어] addr3 위치 = uring_cmd 인라인 페이로드 시작. */
}

/*
 * [한국어]
 * ublk_set_sqe_cmd_op - ublk 명령 opcode 를 커널 버전에 맞게 인코딩해 SQE 에 기록.
 *
 * @sqe: 대상 SQE.
 * @cmd_op: 논리 opcode (UBLK_CMD_* 또는 UBLK_IO_*).
 *
 * kernel 6.2+ 는 UBLK_F_CMD_IOCTL_ENCODE 를 도입 — uring_cmd 의 op 를 _IOR/_IOWR 매크로로
 * 인코딩(방향 + 타입 + 크기 비트 포함)해야 한다. g_ublk_tgt.ioctl_encode 가 true 면 각 opcode 를
 * 대응 _IOWR(...) 값으로 변환하고, false(구버전)면 raw opcode 그대로 사용한다. 결과를 sqe->off
 * 에 저장(ublk UAPI 가 cmd_op 를 off 필드에 싣기로 약속).
 * 실행 컨텍스트: ring 소유 thread.
 *
 * 호출 체인:
 *   ublk_ctrl_cmd_submit / ublk_ctrl_cmd_get_features / ublksrv_queue_io_cmd → [본 함수]
 */
static inline void
ublk_set_sqe_cmd_op(struct io_uring_sqe *sqe, uint32_t cmd_op)
{
	uint32_t opc = cmd_op;   /* [한국어] 기본값 = raw opcode (구버전 커널 경로). */

	/* [한국어] kernel 6.2+ 만 IOCTL_ENCODE — opcode 를 _IOR/_IOWR 로 방향·타입·크기 인코딩. */
	if (g_ublk_tgt.ioctl_encode) {
		switch (cmd_op) {
		/* ctrl uring */
		/* [한국어] GET_DEV_INFO 만 _IOR (read-only: 커널→사용자 출력). */
		case UBLK_CMD_GET_DEV_INFO:
			opc = _IOR('u', UBLK_CMD_GET_DEV_INFO, struct ublksrv_ctrl_cmd);
			break;
		/* [한국어] 이하 ctrl 명령은 _IOWR (사용자→커널 입력 + 일부 출력). 'u'=ublk 매직. */
		case UBLK_CMD_ADD_DEV:
			opc = _IOWR('u', UBLK_CMD_ADD_DEV, struct ublksrv_ctrl_cmd);
			break;
		case UBLK_CMD_DEL_DEV:
			opc = _IOWR('u', UBLK_CMD_DEL_DEV, struct ublksrv_ctrl_cmd);
			break;
		case UBLK_CMD_START_DEV:
			opc = _IOWR('u', UBLK_CMD_START_DEV, struct ublksrv_ctrl_cmd);
			break;
		case UBLK_CMD_STOP_DEV:
			opc = _IOWR('u', UBLK_CMD_STOP_DEV, struct ublksrv_ctrl_cmd);
			break;
		case UBLK_CMD_SET_PARAMS:
			opc = _IOWR('u', UBLK_CMD_SET_PARAMS, struct ublksrv_ctrl_cmd);
			break;
		case UBLK_CMD_START_USER_RECOVERY:
			opc = _IOWR('u', UBLK_CMD_START_USER_RECOVERY, struct ublksrv_ctrl_cmd);
			break;
		case UBLK_CMD_END_USER_RECOVERY:
			opc = _IOWR('u', UBLK_CMD_END_USER_RECOVERY, struct ublksrv_ctrl_cmd);
			break;

		/* io uring */
		/* [한국어] data ring IO 명령들 — payload 타입이 ublksrv_io_cmd 로 다름. */
		case UBLK_IO_FETCH_REQ:
			opc = _IOWR('u', UBLK_IO_FETCH_REQ, struct ublksrv_io_cmd);
			break;
		case UBLK_IO_COMMIT_AND_FETCH_REQ:
			opc = _IOWR('u', UBLK_IO_COMMIT_AND_FETCH_REQ, struct ublksrv_io_cmd);
			break;
		case UBLK_IO_NEED_GET_DATA:
			opc = _IOWR('u', UBLK_IO_NEED_GET_DATA, struct ublksrv_io_cmd);
			break;
		default:
			break;   /* [한국어] 미지정 opcode 는 raw 값 유지. */
		}
	}

	sqe->off = opc;   /* [한국어] ublk UAPI 약속 — cmd_op 를 SQE 의 off 필드에 저장. */
}

/*
 * [한국어]
 * build_user_data - tag + op 를 io_uring SQE 의 64-bit user_data 로 패킹.
 *
 * @tag: IO 슬롯 인덱스 (0~q_depth-1, 16비트 이내).
 * @op: 명령 op (8비트 이내).
 * @return: 패킹된 user_data (low16=tag, bits16-23=op).
 *
 * io_uring 은 SQE 의 user_data 를 그대로 CQE 로 되돌려 준다. ublk 는 그 64비트에 tag 와 op 를
 * 인코딩해, CQE 수신 시 어느 IO 슬롯/명령의 완료인지 역추적한다 (user_data_to_tag/op).
 * assert 로 tag/op 가 비트 범위를 넘지 않음을 디버그 빌드에서 검증.
 *
 * 호출 체인:
 *   ublksrv_queue_io_cmd / ublk_queue_user_copy → [본 함수] → io_uring_sqe_set_data64
 */
static inline uint64_t
build_user_data(uint16_t tag, uint8_t op)
{
	assert(!(tag >> 16) && !(op >> 8));  /* [한국어] tag<16비트, op<8비트 범위 검증. */

	return tag | (op << 16);  /* [한국어] low16=tag, bits16-23=op 로 패킹. */
}

/*
 * [한국어]
 * user_data_to_tag - CQE user_data 에서 IO 슬롯 tag 추출.
 *
 * @user_data: CQE 의 user_data (build_user_data 로 인코딩된 값).
 * @return: tag (low 16비트) = q->ios[] 인덱스.
 *
 * 호출 체인: ublk_io_recv → [본 함수] → q->ios[tag] 로 IO 객체 복원.
 */
static inline uint16_t
user_data_to_tag(uint64_t user_data)
{
	return user_data & 0xffff;  /* [한국어] low 16비트 = tag. */
}

/*
 * [한국어]
 * user_data_to_op - CQE user_data 에서 명령 op 추출 (로그/디버그용).
 *
 * @user_data: CQE 의 user_data.
 * @return: op (bits 16-23).
 *
 * 호출 체인: ublk_io_recv 의 디버그 로그 → [본 함수].
 */
static inline uint8_t
user_data_to_op(uint64_t user_data)
{
	return (user_data >> 16) & 0xff;  /* [한국어] bits16-23 = op. */
}

/*
 * [한국어]
 * ublk_user_copy_pos - USER_COPY 모드에서 커널 IO buffer 의 가상 offset 계산.
 *
 * @q_id: 큐 인덱스.
 * @tag: IO 슬롯 인덱스.
 * @return: /dev/ublkcN 파일 내의 offset — read/write SQE 의 위치 인자로 사용.
 *
 * USER_COPY 모드에서는 커널이 사용자 IO 데이터를 /dev/ublkcN 파일의 특정 offset 영역에
 * 매핑해 둔다. SPDK 는 io_uring_prep_read/write 의 offset 으로 이 값을 주어, 자신의 payload
 * buffer 와 커널 buffer 사이에 데이터를 복사한다. offset 은 UBLKSRV_IO_BUF_OFFSET 기준에
 * q_id 와 tag 를 정해진 비트 위치(UBLK_QID_OFF/UBLK_TAG_OFF)로 OR 해 구성 — ublk UAPI 의 layout.
 *
 * 호출 체인:
 *   ublk_queue_user_copy → [본 함수] → io_uring_prep_read/write 의 offset
 */
static inline uint64_t
ublk_user_copy_pos(uint16_t q_id, uint16_t tag)
{
	/* [한국어] UBLKSRV_IO_BUF_OFFSET 기준 + (q_id<<QID_OFF | tag<<TAG_OFF) — 커널 buffer 위치. */
	return (uint64_t)UBLKSRV_IO_BUF_OFFSET + ((((uint64_t)q_id) << UBLK_QID_OFF) | (((
				uint64_t)tag) << UBLK_TAG_OFF));
}

/*
 * [한국어]
 * spdk_ublk_init - ublk 라이브러리 전역 상태의 1회성 초기화 (공개 API).
 *
 * ublk subsystem(init.c) 부팅 시 호출 — 아직 디바이스나 target 은 만들지 않고, g_ublk_tgt 의
 * fd 핸들들을 "닫힘"을 의미하는 -1 로만 세팅한다. 이렇게 해야 이후 정리 경로(fini)에서 열린
 * 적 없는 fd 를 close 하지 않는다.
 * 실행 컨텍스트: app_thread(reactor 0)에서만 호출 — assert 로 강제.
 *
 * 호출 체인:
 *   ublk subsystem init → [본 함수]
 */
void
spdk_ublk_init(void)
{
	assert(spdk_thread_is_app_thread(NULL));  /* [한국어] app_thread 한정 — 전역 상태 단독 접근. */

	g_ublk_tgt.ctrl_fd = -1;             /* [한국어] /dev/ublk-control fd = 닫힘 표시. */
	g_ublk_tgt.ctrl_ring.ring_fd = -1;   /* [한국어] ctrl io_uring fd = 닫힘 표시 (fini 가드용). */
}

/*
 * [한국어]
 * ublk_ctrl_cmd_error - control plane 명령 실패 시 사용자 콜백 통지 + 자원 롤백.
 *
 * @ublk: 실패한 명령의 대상 디바이스.
 * @res: 음수 errno (cqe->res, 반드시 != 0).
 *
 * ublk_ctrl_process_cqe 가 cqe->res != 0 을 발견하면 호출. (1) 에러 로그, (2) 등록된 ctrl_cb
 * 가 있으면 res 로 즉시 통지하고 한 번만 호출되도록 NULL 로 클리어, (3) 실패한 명령 단계에
 * 따라 부분 생성된 자원을 적절한 정리 함수로 롤백한다:
 *   - ADD/SET_PARAMS/RECOVERY 단계 실패 → ublk_delete_dev (커널 DEL_DEV + 큐 정리).
 *   - START_DEV 실패 → ublk_close_dev (STOP_DEV).
 *   - GET_DEV_INFO 실패 → ublk_free_dev (메모리만 해제, 커널 등록 전).
 *   - STOP/DEL 실패 → 추가 롤백 없음 (이미 teardown 경로).
 * 실행 컨텍스트: ctrl_poller(app_thread).
 *
 * 호출 체인:
 *   ublk_ctrl_poller → ublk_ctrl_process_cqe → [본 함수] → ublk_delete/close/free_dev + ctrl_cb
 */
static void
ublk_ctrl_cmd_error(struct spdk_ublk_dev *ublk, int32_t res)
{
	assert(res != 0);  /* [한국어] 에러 핸들러 — res 는 반드시 음수 errno. */

	/* [한국어] 실패한 명령 이름과 errno 문자열 로그. */
	SPDK_ERRLOG("ctrlr cmd %s failed, %s\n", ublk_op_name[ublk->current_cmd_op], spdk_strerror(-res));
	/* [한국어] 사용자 콜백 통지 — RPC 응답. 한 번만 호출되도록 즉시 NULL 클리어. */
	if (ublk->ctrl_cb) {
		ublk->ctrl_cb(ublk->cb_arg, res);
		ublk->ctrl_cb = NULL;
	}

	/* [한국어] 실패 단계별 자원 롤백 — 어디까지 생성됐는지에 따라 정리 깊이가 다름. */
	switch (ublk->current_cmd_op) {
	case UBLK_CMD_ADD_DEV:
	case UBLK_CMD_SET_PARAMS:
	case UBLK_CMD_START_USER_RECOVERY:
	case UBLK_CMD_END_USER_RECOVERY:
		/* [한국어] 커널에 ADD 까지 된 상태 → DEL_DEV + 큐 정리. */
		ublk_delete_dev(ublk);
		break;
	case UBLK_CMD_START_DEV:
		/* [한국어] START 실패 → STOP_DEV 로 되돌림. */
		ublk_close_dev(ublk);
		break;
	case UBLK_CMD_GET_DEV_INFO:
		/* [한국어] 아직 커널 등록 전(recovery 조회) → 메모리만 해제. */
		ublk_free_dev(ublk);
		break;
	case UBLK_CMD_STOP_DEV:
	case UBLK_CMD_DEL_DEV:
		break;  /* [한국어] 이미 teardown 경로 — 추가 롤백 불필요. */
	default:
		SPDK_ERRLOG("No match cmd operation,cmd_op = %d\n", ublk->current_cmd_op);
		break;
	}
}

/*
 * [한국어]
 * _ublk_get_device_state_retry - recovery 시 디바이스가 QUIESCED 될 때까지 GET_DEV_INFO 재시도.
 *
 * @arg: spdk_ublk_dev.
 * @return: 항상 SPDK_POLLER_BUSY.
 *
 * recovery 진입 시 커널 디바이스 상태가 아직 UBLK_S_DEV_QUIESCED 가 아니면(-EBUSY) 1초 간격
 * retry_poller 로 GET_DEV_INFO 를 다시 보낸다. 본 함수는 그 poller 콜백 — 먼저 자신을
 * unregister(1회성) 한 뒤 명령을 재제출한다. 제출 자체가 실패하면 디바이스를 삭제하고
 * 사용자 콜백에 에러 통지.
 * 실행 컨텍스트: app_thread 의 poller.
 *
 * 호출 체인:
 *   ublk_ctrl_process_cqe(GET_DEV_INFO, 비QUIESCED) → SPDK_POLLER_REGISTER → [본 함수]
 *     → ublk_ctrl_cmd_submit(GET_DEV_INFO)
 */
static int
_ublk_get_device_state_retry(void *arg)
{
	struct spdk_ublk_dev *ublk = arg;
	int rc;

	spdk_poller_unregister(&ublk->retry_poller);  /* [한국어] 1회성 poller — 즉시 해제 후 재제출. */

	rc = ublk_ctrl_cmd_submit(ublk, UBLK_CMD_GET_DEV_INFO);  /* [한국어] 상태 재조회. */
	if (rc < 0) {
		ublk_delete_dev(ublk);  /* [한국어] 제출 실패 → 디바이스 삭제. */
		/* [한국어] 사용자 콜백에 에러 통지 (1회). */
		if (ublk->ctrl_cb) {
			ublk->ctrl_cb(ublk->cb_arg, rc);
			ublk->ctrl_cb = NULL;
		}
	}

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * ublk_ctrl_process_cqe - control plane 명령 1건의 완료 CQE 처리 + 다음 단계 chain.
 *
 * @cqe: 완료된 ctrl io_uring CQE (user_data = spdk_ublk_dev 포인터).
 *
 * ublk 디바이스 생성/제거/복구는 여러 control 명령이 비동기로 이어지는 상태머신이다. 본 함수는
 * ctrl_poller 가 CQE 를 받을 때마다 호출되어, 직전에 완료된 current_cmd_op 를 보고 다음 명령을
 * 발행한다:
 *   ADD_DEV → ublk_set_params(SET_PARAMS)
 *   SET_PARAMS → ublk_start_dev(false) → (cdev open + 큐 init + START_DEV)
 *   START_DEV → 성공 콜백 (cb_done)
 *   GET_DEV_INFO → 상태가 QUIESCED 면 ublk_ctrl_start_recovery, 아니면 재시도
 *   START_USER_RECOVERY → ublk_start_dev(true)
 *   END_USER_RECOVERY → recovery 완료 콜백
 *   DEL_DEV → 사용자 콜백 + ublk_free_dev
 * cqe->res != 0 이면 ublk_ctrl_cmd_error 로 위임. 각 단계 내부 실패 시 ublk_delete_dev 후
 * cb_done 라벨에서 콜백 통지.
 * 실행 컨텍스트: ctrl_poller(app_thread) — 단일 스레드라 상태머신에 락 불필요.
 *
 * 호출 체인:
 *   ublk_ctrl_poller → [본 함수] → ublk_set_params/start_dev/start_recovery/free_dev + ctrl_cb
 */
static void
ublk_ctrl_process_cqe(struct io_uring_cqe *cqe)
{
	struct spdk_ublk_dev *ublk;
	int rc = 0;

	/* [한국어] CQE user_data 에 submit 시 심어둔 ublk 포인터로 디바이스 복원. */
	ublk = (struct spdk_ublk_dev *)cqe->user_data;
	UBLK_DEBUGLOG(ublk, "ctrl cmd %s completed\n", ublk_op_name[ublk->current_cmd_op]);
	ublk->ctrl_ops_in_progress--;  /* [한국어] per-dev inflight 감소 — destroy 대기 카운트. */

	/* [한국어] 명령 자체 실패 → 에러 핸들러 위임 (롤백 + 콜백). */
	if (spdk_unlikely(cqe->res != 0)) {
		ublk_ctrl_cmd_error(ublk, cqe->res);
		return;
	}

	/* [한국어] 완료된 명령에 따라 비동기 chain 의 다음 단계 발행. */
	switch (ublk->current_cmd_op) {
	case UBLK_CMD_ADD_DEV:
		/* [한국어] ADD 완료 → 파라미터 설정 단계. */
		rc = ublk_set_params(ublk);
		if (rc < 0) {
			ublk_delete_dev(ublk);  /* [한국어] 실패 → 롤백. */
			goto cb_done;
		}
		break;
	case UBLK_CMD_SET_PARAMS:
		/* [한국어] 파라미터 완료 → 디바이스 start (cdev open + 큐 init + START_DEV). */
		rc = ublk_start_dev(ublk, false);
		if (rc < 0) {
			ublk_delete_dev(ublk);
			goto cb_done;
		}
		break;
	case UBLK_CMD_START_DEV:
		/* [한국어] start 완료 = 전체 생성 chain 성공 → 사용자 콜백. */
		goto cb_done;
		break;
	case UBLK_CMD_STOP_DEV:
		break;  /* [한국어] stop 완료 — 후속은 큐 drain 후 try_close_dev 가 DEL 진행. */
	case UBLK_CMD_DEL_DEV:
		/* [한국어] DEL 완료 = 제거 chain 끝 → 콜백 통지 후 메모리 해제. */
		if (ublk->ctrl_cb) {
			ublk->ctrl_cb(ublk->cb_arg, 0);
			ublk->ctrl_cb = NULL;
		}
		ublk_free_dev(ublk);
		break;
	case UBLK_CMD_GET_DEV_INFO:
		/* [한국어] recovery 조회 응답 — dev_id 일치 검증. */
		if (ublk->ublk_id != ublk->dev_info.dev_id) {
			SPDK_ERRLOG("Invalid ublk ID\n");
			rc = -EINVAL;
			goto cb_done;
		}

		UBLK_DEBUGLOG(ublk, "Ublk %u device state %u\n", ublk->ublk_id, ublk->dev_info.state);
		/* kernel ublk_drv driver returns -EBUSY if device state isn't UBLK_S_DEV_QUIESCED */
		/* [한국어] 아직 QUIESCED 아니면 1초 후 재조회 (최대 3회) — 커널이 quiesce 완료 대기. */
		if ((ublk->dev_info.state != UBLK_S_DEV_QUIESCED) && (ublk->retry_count < 3)) {
			ublk->retry_count++;
			ublk->retry_poller = SPDK_POLLER_REGISTER(_ublk_get_device_state_retry, ublk, 1000000);
			return;
		}

		/* [한국어] QUIESCED 확인 → 복구 시작 (START_USER_RECOVERY 발행). */
		rc = ublk_ctrl_start_recovery(ublk);
		if (rc < 0) {
			ublk_delete_dev(ublk);
			goto cb_done;
		}
		break;
	case UBLK_CMD_START_USER_RECOVERY:
		/* [한국어] 복구 시작 완료 → 큐 재초기화 + START_DEV (is_recovering=true). */
		rc = ublk_start_dev(ublk, true);
		if (rc < 0) {
			ublk_delete_dev(ublk);
			goto cb_done;
		}
		break;
	case UBLK_CMD_END_USER_RECOVERY:
		/* [한국어] 복구 종료 — is_recovering 해제 후 성공 콜백. */
		SPDK_NOTICELOG("Ublk %u recover done successfully\n", ublk->ublk_id);
		ublk->is_recovering = false;
		goto cb_done;
		break;
	default:
		SPDK_ERRLOG("No match cmd operation,cmd_op = %d\n", ublk->current_cmd_op);
		break;
	}

	return;  /* [한국어] 중간 단계 — 콜백은 chain 끝에서만. */

cb_done:
	/* [한국어] chain 종료점 — 성공(rc=0) 또는 실패(rc<0) 를 사용자 콜백에 1회 통지. */
	if (ublk->ctrl_cb) {
		ublk->ctrl_cb(ublk->cb_arg, rc);
		ublk->ctrl_cb = NULL;
	}
}

/*
 * [한국어]
 * ublk_ctrl_poller - /dev/ublk-control 의 control plane CQE 폴링.
 *
 * @arg: 미사용.
 * @return: SPDK_POLLER_BUSY/IDLE.
 *
 * ublk_create_target 이 등록 (1ms interval). data plane (per-queue ublk_poll) 과 분리된
 * control plane poller — add_dev/start_dev/del_dev/get_dev_info 등 응답 처리.
 *
 * 단계:
 *   1. ctrl_ops_in_progress == 0 → 즉시 IDLE (대기 중 명령 없음).
 *   2. io_uring_peek_cqe 로 최대 8개 CQE 일괄 처리.
 *      - EAGAIN = 더 없음 → break.
 *   3. 각 CQE 마다 ublk_ctrl_process_cqe → cmd_op 별 분기 (ADD_DEV 응답 → 다음 단계 등).
 *   4. ctrl_ops_in_progress -- (in-flight 카운트 감소).
 *
 * peek_cqe + cqe_seen 패턴: peek 로 ring 진행 안 함 → seen 호출 시 ring head 갱신.
 * for_each_cqe 와 달리 batching 보다 명령별 처리 (control plane 은 응답 분기 복잡).
 *
 * batching=8: control plane 은 빈도 낮음 — 큰 batching 불필요.
 *
 * 호출 체인:
 *   SPDK reactor (1ms) → [본 함수] → io_uring_peek_cqe → ublk_ctrl_process_cqe
 *     → ublk->ctrl_cb 사용자 콜백
 */
static int
ublk_ctrl_poller(void *arg)
{
	struct io_uring *ring = &g_ublk_tgt.ctrl_ring;
	struct io_uring_cqe *cqe;
	const int max = 8;
	int i, count = 0, rc;

	/* [한국어] in-flight 0 = 폴링 의미 없음 — 즉시 IDLE (CPU 절약). */
	if (!g_ublk_tgt.ctrl_ops_in_progress) {
		return SPDK_POLLER_IDLE;
	}

	for (i = 0; i < max; i++) {
		/* [한국어] peek = ring head 진행 안 함, seen 호출 시 갱신. */
		rc = io_uring_peek_cqe(ring, &cqe);
		if (rc == -EAGAIN) {
			break;
		}

		assert(cqe != NULL);
		g_ublk_tgt.ctrl_ops_in_progress--;

		ublk_ctrl_process_cqe(cqe);

		io_uring_cqe_seen(ring, cqe);
		count++;
	}

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

/*
 * [한국어]
 * ublk_ctrl_cmd_submit - ★ ublk control plane 명령 SQE 빌드 + 제출.
 *
 * @ublk: 대상 ublk_dev.
 * @cmd_op: UBLK_CMD_* (ADD_DEV/START_DEV/STOP_DEV/DEL_DEV/GET_DEV_INFO/SET_PARAMS/
 *          START_USER_RECOVERY/END_USER_RECOVERY).
 * @return: 0 성공 (응답은 ctrl_poller 가 처리), 음수 errno.
 *
 * ublk 디바이스 lifecycle 명령은 모두 본 함수로 통합. UBLK_CMD_* 별 cmd 페이로드 차이:
 *   - ADD_DEV / GET_DEV_INFO: dev_info 구조체 in/out (addr/len 채움).
 *   - SET_PARAMS: dev_params 구조체 in.
 *   - START_DEV / END_USER_RECOVERY: data[0] = getpid() (kernel 이 owner 추적).
 *   - STOP_DEV / DEL_DEV / START_USER_RECOVERY: 페이로드 없음.
 *
 * io_uring_cmd 패턴: IORING_OP_URING_CMD opcode + uring_cmd payload 24B (ublksrv_ctrl_cmd).
 * fd = ctrl_fd, sqe->cmd_op (별도 함수 ublk_set_sqe_cmd_op 로 설정 — UBLK_U_CMD_* 등 인코딩).
 *
 * ctrl_ops_in_progress 카운트: g_ublk_tgt + ublk 양쪽 증가 — 전체 inflight + per-dev inflight
 * 추적 (전체 카운트로 poller IDLE 결정, per-dev 카운트로 dev destroy 대기).
 *
 * 호출 체인:
 *   ublk_start_dev / ublk_set_params / ublk_close_dev / etc → [본 함수]
 *     → io_uring SQE build → io_uring_submit → 커널 처리 → ctrl_poller 가 CQE 수신
 */
static int
ublk_ctrl_cmd_submit(struct spdk_ublk_dev *ublk, uint32_t cmd_op)
{
	uint32_t dev_id = ublk->ublk_id;
	int rc = -EINVAL;
	struct io_uring_sqe *sqe;
	struct ublksrv_ctrl_cmd *cmd;

	UBLK_DEBUGLOG(ublk, "ctrl cmd %s\n", ublk_op_name[cmd_op]);

	sqe = io_uring_get_sqe(&g_ublk_tgt.ctrl_ring);
	if (!sqe) {
		SPDK_ERRLOG("No available sqe in ctrl ring\n");
		assert(false);
		return -ENOENT;
	}

	/* [한국어] uring_cmd payload — SQE 의 28B+ 영역 (cmd 라 부르는 inline data). */
	cmd = (struct ublksrv_ctrl_cmd *)ublk_get_sqe_cmd(sqe);
	sqe->fd = g_ublk_tgt.ctrl_fd;
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->ioprio = 0;
	cmd->dev_id = dev_id;
	cmd->queue_id = -1;            /* [한국어] ctrl cmd 는 device-wide (queue_id 무관). */
	ublk->current_cmd_op = cmd_op;

	/* [한국어] cmd_op 별 payload 채우기. */
	switch (cmd_op) {
	case UBLK_CMD_ADD_DEV:
	case UBLK_CMD_GET_DEV_INFO:
		cmd->addr = (__u64)(uintptr_t)&ublk->dev_info;
		cmd->len = sizeof(ublk->dev_info);
		break;
	case UBLK_CMD_SET_PARAMS:
		cmd->addr = (__u64)(uintptr_t)&ublk->dev_params;
		cmd->len = sizeof(ublk->dev_params);
		break;
	case UBLK_CMD_START_DEV:
		/* [한국어] kernel 이 owner pid 추적 — owner 죽으면 device 자동 cleanup. */
		cmd->data[0] = getpid();
		break;
	case UBLK_CMD_STOP_DEV:
	case UBLK_CMD_DEL_DEV:
	case UBLK_CMD_START_USER_RECOVERY:
		/* [한국어] payload 없는 명령들. */
		break;
	case UBLK_CMD_END_USER_RECOVERY:
		cmd->data[0] = getpid();
		break;
	default:
		SPDK_ERRLOG("No match cmd operation,cmd_op = %d\n", cmd_op);
		return -EINVAL;
	}
	/* [한국어] cmd_op 인코딩 — UBLK_U_CMD_* (커널 버전별 differences 처리). */
	ublk_set_sqe_cmd_op(sqe, cmd_op);
	/* [한국어] user_data = ublk pointer — ctrl_process_cqe 가 cqe->user_data 로 ublk 역추적. */
	io_uring_sqe_set_data(sqe, ublk);

	rc = io_uring_submit(&g_ublk_tgt.ctrl_ring);
	if (rc < 0) {
		SPDK_ERRLOG("uring submit rc %d\n", rc);
		assert(false);
		return rc;
	}
	/* [한국어] inflight 카운트 양쪽 — 전체 (poller IDLE 결정) + per-dev (destroy 대기). */
	g_ublk_tgt.ctrl_ops_in_progress++;
	ublk->ctrl_ops_in_progress++;

	return 0;
}

/*
 * [한국어]
 * ublk_ctrl_cmd_get_features - 커널 ublk_drv 가 지원하는 feature 비트맵을 동기적으로 질의.
 *
 * @return: 0 성공 (g_ublk_tgt.features 및 파생 bool 들 채움), 음수 errno.
 *
 * ublk_open 직후 1회 호출되어 kernel 의 UBLK_F_* 기능(IOCTL_ENCODE/USER_COPY/USER_RECOVERY)을
 * 파악한다. 다른 ctrl 명령들과 달리 비동기 ctrl_poller 경로가 아니라 io_uring_wait_cqe 로
 * **블로킹 대기**한다 — target 초기화 단계라 동기 처리가 단순하고 안전하기 때문.
 * UBLK_U_CMD_GET_FEATURES 는 디바이스 무관 명령이라 dev_id/queue_id 를 -1 로 둔다.
 * 응답으로 받은 features 비트를 ioctl_encode/user_copy/user_recovery bool 로 디코드하고,
 * user_copy 는 g_disable_user_copy(RPC 옵션)와 AND 해 최종 활성 여부를 정한다.
 * 실행 컨텍스트: app_thread(ublk_create_target → ublk_open 경로).
 *
 * 호출 체인:
 *   ublk_open → [본 함수] → io_uring_submit/wait_cqe (커널 ublk_drv GET_FEATURES)
 */
static int
ublk_ctrl_cmd_get_features(void)
{
	int rc;
	struct io_uring_sqe *sqe;   /* [한국어] 제출용 SQE — ctrl ring 에서 1칸 확보. */
	struct io_uring_cqe *cqe;   /* [한국어] 완료 CQE — wait_cqe 로 동기 수신. */
	struct ublksrv_ctrl_cmd *cmd;  /* [한국어] SQE 인라인 영역에 채울 ublk ctrl 명령 페이로드. */
	uint32_t cmd_op;            /* [한국어] 인코딩 전 논리 opcode. */

	sqe = io_uring_get_sqe(&g_ublk_tgt.ctrl_ring);  /* [한국어] ctrl ring 에서 빈 SQE 1칸 획득. */
	if (!sqe) {                                     /* [한국어] ring full = 초기화 단계라 발생하면 버그. */
		SPDK_ERRLOG("No available sqe in ctrl ring\n");
		assert(false);
		return -ENOENT;
	}

	cmd = (struct ublksrv_ctrl_cmd *)ublk_get_sqe_cmd(sqe);  /* [한국어] addr3 위치 = uring_cmd 인라인 payload. */
	sqe->fd = g_ublk_tgt.ctrl_fd;          /* [한국어] /dev/ublk-control fd 대상. */
	sqe->opcode = IORING_OP_URING_CMD;     /* [한국어] uring_cmd 방식 (ublk 전용 명령 전달). */
	sqe->ioprio = 0;                       /* [한국어] 우선순위 미사용. */
	cmd->dev_id = -1;                      /* [한국어] feature 질의는 디바이스 무관 → -1. */
	cmd->queue_id = -1;                    /* [한국어] 큐 무관 → -1. */
	cmd->addr = (__u64)(uintptr_t)&g_ublk_tgt.features;  /* [한국어] 커널이 feature 비트맵을 쓸 출력 버퍼. */
	cmd->len = sizeof(g_ublk_tgt.features);             /* [한국어] 출력 버퍼 크기 (8B). */

	cmd_op = UBLK_U_CMD_GET_FEATURES;      /* [한국어] 이미 _IOWR 인코딩된 UAPI 상수 사용. */
	ublk_set_sqe_cmd_op(sqe, cmd_op);      /* [한국어] sqe->off 에 opcode 기록. */

	rc = io_uring_submit(&g_ublk_tgt.ctrl_ring);  /* [한국어] 커널에 SQE 제출 (io_uring_enter). */
	if (rc < 0) {
		SPDK_ERRLOG("uring submit rc %d\n", rc);
		return rc;
	}

	rc = io_uring_wait_cqe(&g_ublk_tgt.ctrl_ring, &cqe);  /* [한국어] 동기 블로킹 — 완료까지 대기. */
	if (rc < 0) {
		SPDK_ERRLOG("wait cqe rc %d\n", rc);
		return rc;
	}

	/* [한국어] cqe->res==0 = 명령 성공 → features 비트를 개별 bool 로 디코드. */
	if (cqe->res == 0) {
		g_ublk_tgt.ioctl_encode = !!(g_ublk_tgt.features & UBLK_F_CMD_IOCTL_ENCODE);  /* [한국어] 6.2+ ioctl 인코딩. */
		g_ublk_tgt.user_copy = !!(g_ublk_tgt.features & UBLK_F_USER_COPY);            /* [한국어] zero-copy 지원. */
		g_ublk_tgt.user_copy &= !g_disable_user_copy;  /* [한국어] RPC 로 비활성 요청 시 강제 off. */
		g_ublk_tgt.user_recovery = !!(g_ublk_tgt.features & UBLK_F_USER_RECOVERY);    /* [한국어] 재시작 복구 지원. */
		SPDK_NOTICELOG("User Copy %s\n", g_ublk_tgt.user_copy ? "enabled" : "disabled");
	}
	io_uring_cqe_seen(&g_ublk_tgt.ctrl_ring, cqe);  /* [한국어] CQE 소비 표시 — ring head 전진. */

	return 0;
}

/*
 * [한국어]
 * ublk_queue_cmd_buf_sz - 큐의 io_cmd_buf(공유 ublksrv_io_desc 배열) mmap 크기를 페이지 정렬해 계산.
 *
 * @q_depth: 큐 깊이 (= 슬롯/io_desc 개수).
 * @return: 페이지 크기로 올림된 바이트 수.
 *
 * 각 큐는 커널과 공유하는 ublksrv_io_desc 배열을 /dev/ublkcN 의 고정 offset 에서 mmap 한다.
 * mmap 은 페이지 단위로만 매핑 가능하므로 q_depth * sizeof(io_desc) 를 페이지 경계로 올림한다.
 * 비트 트릭 (x + page-1) & ~(page-1) = page_sz 의 배수로 올림 (page_sz 가 2^n 임을 전제).
 * 실행 컨텍스트: ublk_dev_queue_init(큐 소유 poll_group thread).
 *
 * 호출 체인:
 *   ublk_dev_queue_init → [본 함수]
 */
static int
ublk_queue_cmd_buf_sz(uint32_t q_depth)
{
	uint32_t size = q_depth * sizeof(struct ublksrv_io_desc);  /* [한국어] io_desc 배열의 raw 바이트 크기. */
	uint32_t page_sz = getpagesize();   /* [한국어] 시스템 페이지 크기 (보통 4KB) — mmap 정렬 단위. */

	/* round up size */
	/* [한국어] page_sz 배수로 올림 (page_sz 는 2^n) — mmap 은 페이지 단위만 매핑. */
	return (size + page_sz - 1) & ~(page_sz - 1);
}

/*
 * [한국어]
 * ublk_open - /dev/ublk-control 을 열고 ctrl io_uring + feature 질의까지 수행.
 *
 * @return: 0 성공, 음수 errno (open/ring init/get_features 실패).
 *
 * ublk_create_target 의 핵심 하위 단계. (1) /dev/ublk-control 캐릭터 디바이스를 O_RDWR 로 열어
 * ctrl_fd 확보, (2) /sys 에서 ublks_max 실제값을 읽어 g_ublks_max 갱신, (3) ctrl io_uring 을
 * SQE128 + SQPOLL 로 생성, (4) GET_FEATURES 로 커널 기능 확인. SQPOLL 을 쓰는 이유는 주석대로
 * 6.1 이하 커널이 ctrl ring 처리를 workqueue 로 미루지 않기 때문 — ctrl 명령은 가볍고 직렬
 * 실행이라 커널 SQ 폴링 스레드를 써도 무방하다. 실패 시 ctrl_fd 를 close 하고 -1 로 복원.
 * 실행 컨텍스트: app_thread.
 *
 * 호출 체인:
 *   ublk_create_target → [본 함수] → open + ublk_setup_ring + ublk_ctrl_cmd_get_features
 */
static int
ublk_open(void)
{
	uint32_t ublks_max;   /* [한국어] /sys 에서 읽은 커널 ublks_max — 디바이스 수 상한. */
	int rc;

	g_ublk_tgt.ctrl_fd = open(UBLK_CTRL_DEV, O_RDWR);  /* [한국어] /dev/ublk-control 열기 — ctrl 명령 채널. */
	if (g_ublk_tgt.ctrl_fd < 0) {
		rc = errno;   /* [한국어] open 실패 errno 보존 (다음 로그/SPDK_ERRLOG 전에 캡처). */
		SPDK_ERRLOG("UBLK control dev %s can't be opened, error=%s\n", UBLK_CTRL_DEV, spdk_strerror(errno));
		return -rc;
	}

	/* [한국어] 커널 모듈 파라미터에서 실제 ublks_max 읽어 기본값(64) 덮어쓰기. */
	rc = spdk_read_sysfs_attribute_uint32(&ublks_max, "%s",
					      "/sys/module/ublk_drv/parameters/ublks_max");
	if (rc == 0 && ublks_max > 0) {   /* [한국어] 읽기 성공 + 유효값일 때만 반영. */
		g_ublks_max = ublks_max;
	}

	/* We need to set SQPOLL for kernels 6.1 and earlier, since they would not defer ublk ctrl
	 * ring processing to a workqueue.  Ctrl ring processing is minimal, so SQPOLL is fine.
	 * All the commands sent via control uring for a ublk device is executed one by one, so use
	 * ublks_max * 2 as the number of uring entries is enough.
	 */
	/* [한국어] ctrl ring 생성 — depth=ublks_max*2 (명령이 직렬 실행이라 충분), SQE128(uring_cmd
	 * 인라인 payload 수용) + SQPOLL(6.1 이하 커널 호환). */
	rc = ublk_setup_ring(g_ublks_max * 2, &g_ublk_tgt.ctrl_ring,
			     IORING_SETUP_SQE128 | IORING_SETUP_SQPOLL);
	if (rc < 0) {
		SPDK_ERRLOG("UBLK ctrl queue_init: %s\n", spdk_strerror(-rc));
		goto err;
	}

	rc = ublk_ctrl_cmd_get_features();  /* [한국어] 커널 feature 비트맵 동기 질의. */
	if (rc) {
		goto err;
	}

	return 0;

err:
	/* [한국어] ring init 또는 get_features 실패 → 열었던 ctrl_fd 정리 후 -1 복원. */
	close(g_ublk_tgt.ctrl_fd);
	g_ublk_tgt.ctrl_fd = -1;
	return rc;
}

/*
 * [한국어]
 * ublk_parse_core_mask - poll_group 를 배치할 CPU 코어 집합(g_core_mask)을 파싱·검증.
 *
 * @mask: hex cpumask 문자열 ("0x3" 등) 또는 NULL.
 * @return: 0 성공, -EINVAL (파싱 실패 / 빈 집합 / SPDK 코어 범위 밖).
 *
 * RPC ublk_create_target 의 cpumask 인자를 g_core_mask 로 변환한다. NULL 이면 SPDK 가 가진
 * 전체 reactor cpuset 을 그대로 사용. 명시되면 파싱 후 (1) 비어있지 않은지, (2) SPDK 앱이
 * 실제 점유한 코어(env cpuset)의 부분집합인지를 검증한다 — SPDK reactor 가 없는 코어에
 * poll_group thread 를 만들 수 없기 때문.
 * 실행 컨텍스트: app_thread.
 *
 * 호출 체인:
 *   ublk_create_target → [본 함수] → spdk_cpuset_parse/and/equal
 */
static int
ublk_parse_core_mask(const char *mask)
{
	struct spdk_cpuset tmp_mask;  /* [한국어] env cpuset ∩ 요청 mask 결과 비교용 임시. */
	int rc;

	if (mask == NULL) {   /* [한국어] mask 미지정 → SPDK 전체 코어 사용. */
		spdk_env_get_cpuset(&g_core_mask);
		return 0;
	}

	rc = spdk_cpuset_parse(&g_core_mask, mask);  /* [한국어] hex 문자열 → cpuset 비트맵. */
	if (rc < 0) {
		SPDK_ERRLOG("invalid cpumask %s\n", mask);
		return -EINVAL;
	}

	if (spdk_cpuset_count(&g_core_mask) == 0) {  /* [한국어] 빈 집합이면 poll_group 0개 → 에러. */
		SPDK_ERRLOG("no cpus specified\n");
		return -EINVAL;
	}

	spdk_env_get_cpuset(&tmp_mask);          /* [한국어] SPDK 가 점유한 전체 코어. */
	spdk_cpuset_and(&tmp_mask, &g_core_mask);  /* [한국어] 교집합 = 요청 중 실제 사용 가능한 코어. */

	/* [한국어] 교집합 != 요청 → 요청에 SPDK 밖 코어가 섞임 → 거부. */
	if (!spdk_cpuset_equal(&tmp_mask, &g_core_mask)) {
		SPDK_ERRLOG("one of selected cpu is outside of core mask(=%s)\n",
			    spdk_cpuset_fmt(&g_core_mask));
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * ublk_poller_register - poll_group thread 컨텍스트에서 ublk_poll poller 와 iobuf 채널을 설치.
 *
 * @args: 대상 ublk_poll_group.
 *
 * ublk_create_target 이 각 poll_group thread 를 만든 뒤 spdk_thread_send_msg 로 그 thread 에
 * 디스패치하는 초기화 콜백. **반드시 해당 poll_group->ublk_thread 에서 실행되어야** poller 와
 * iobuf 채널이 올바른 reactor 에 affine 된다(assert 로 강제). 또한 spdk_thread_bind(true) 로
 * 이 spdk_thread 를 현재 CPU 코어에 고정 — ublk 커널 드라이버가 uring 처리 중 thread context
 * switch 가 일어나지 않을 것을 요구하기 때문(영어 주석 참조). 이어서 queue_list 초기화,
 * ublk_poll poller 등록(interval 0 = 매 reactor iteration), iobuf 채널 init.
 * 실행 컨텍스트: 대상 poll_group 의 ublk_thread.
 *
 * 호출 체인:
 *   ublk_create_target → spdk_thread_send_msg → [본 함수] → SPDK_POLLER_REGISTER(ublk_poll)
 */
static void
ublk_poller_register(void *args)
{
	struct ublk_poll_group *poll_group = args;
	int rc;

	assert(spdk_get_thread() == poll_group->ublk_thread);  /* [한국어] 올바른 thread 에서 실행 보장. */
	/* Bind ublk spdk_thread to current CPU core in order to avoid thread context switch
	 * during uring processing as required by ublk kernel.
	 */
	/* [한국어] spdk_thread 를 현재 코어에 고정 — ublk_drv 가 uring 처리 중 thread 이동을 금지. */
	spdk_thread_bind(spdk_get_thread(), true);

	TAILQ_INIT(&poll_group->queue_list);   /* [한국어] 담당 큐 리스트 초기화 (아직 큐 없음). */
	/* [한국어] ublk_poll 등록 — interval 0 = 매 reactor 루프마다 io_uring CQE 폴링 (polled-mode). */
	poll_group->ublk_poller = SPDK_POLLER_REGISTER(ublk_poll, poll_group, 0);
	/* [한국어] iobuf 채널 init — user_copy 모드에서 IO data buffer 를 빌릴 per-thread lockless 캐시. */
	rc = spdk_iobuf_channel_init(&poll_group->iobuf_ch, "ublk",
				     UBLK_IOBUF_SMALL_CACHE_SIZE, UBLK_IOBUF_LARGE_CACHE_SIZE);
	if (rc != 0) {
		assert(false);   /* [한국어] iobuf 채널 실패는 치명적 — 디버그 빌드에서 즉시 abort. */
	}
}

static int
ublk_open(void)
{
	uint32_t ublks_max;
	int rc;

	g_ublk_tgt.ctrl_fd = open(UBLK_CTRL_DEV, O_RDWR);
	if (g_ublk_tgt.ctrl_fd < 0) {
		rc = errno;
		SPDK_ERRLOG("UBLK control dev %s can't be opened, error=%s\n", UBLK_CTRL_DEV, spdk_strerror(errno));
		return -rc;
	}

	rc = spdk_read_sysfs_attribute_uint32(&ublks_max, "%s",
					      "/sys/module/ublk_drv/parameters/ublks_max");
	if (rc == 0 && ublks_max > 0) {
		g_ublks_max = ublks_max;
	}

	/* We need to set SQPOLL for kernels 6.1 and earlier, since they would not defer ublk ctrl
	 * ring processing to a workqueue.  Ctrl ring processing is minimal, so SQPOLL is fine.
	 * All the commands sent via control uring for a ublk device is executed one by one, so use
	 * ublks_max * 2 as the number of uring entries is enough.
	 */
	rc = ublk_setup_ring(g_ublks_max * 2, &g_ublk_tgt.ctrl_ring,
			     IORING_SETUP_SQE128 | IORING_SETUP_SQPOLL);
	if (rc < 0) {
		SPDK_ERRLOG("UBLK ctrl queue_init: %s\n", spdk_strerror(-rc));
		goto err;
	}

	rc = ublk_ctrl_cmd_get_features();
	if (rc) {
		goto err;
	}

	return 0;

err:
	close(g_ublk_tgt.ctrl_fd);
	g_ublk_tgt.ctrl_fd = -1;
	return rc;
}

static int
ublk_parse_core_mask(const char *mask)
{
	struct spdk_cpuset tmp_mask;
	int rc;

	if (mask == NULL) {
		spdk_env_get_cpuset(&g_core_mask);
		return 0;
	}

	rc = spdk_cpuset_parse(&g_core_mask, mask);
	if (rc < 0) {
		SPDK_ERRLOG("invalid cpumask %s\n", mask);
		return -EINVAL;
	}

	if (spdk_cpuset_count(&g_core_mask) == 0) {
		SPDK_ERRLOG("no cpus specified\n");
		return -EINVAL;
	}

	spdk_env_get_cpuset(&tmp_mask);
	spdk_cpuset_and(&tmp_mask, &g_core_mask);

	if (!spdk_cpuset_equal(&tmp_mask, &g_core_mask)) {
		SPDK_ERRLOG("one of selected cpu is outside of core mask(=%s)\n",
			    spdk_cpuset_fmt(&g_core_mask));
		return -EINVAL;
	}

	return 0;
}

static void
ublk_poller_register(void *args)
{
	struct ublk_poll_group *poll_group = args;
	int rc;

	assert(spdk_get_thread() == poll_group->ublk_thread);
	/* Bind ublk spdk_thread to current CPU core in order to avoid thread context switch
	 * during uring processing as required by ublk kernel.
	 */
	spdk_thread_bind(spdk_get_thread(), true);

	TAILQ_INIT(&poll_group->queue_list);
	poll_group->ublk_poller = SPDK_POLLER_REGISTER(ublk_poll, poll_group, 0);
	rc = spdk_iobuf_channel_init(&poll_group->iobuf_ch, "ublk",
				     UBLK_IOBUF_SMALL_CACHE_SIZE, UBLK_IOBUF_LARGE_CACHE_SIZE);
	if (rc != 0) {
		assert(false);
	}
}

/*
 * [한국어]
 * ublk_create_target - ★ ublk target 활성화 — /dev/ublk-control + poll_groups + ctrl_poller.
 *
 * @cpumask_str: poll_group 가 사용할 CPU mask (hex string, "0x3" = core 0,1).
 * @disable_user_copy: true = user_copy 비활성 (zero-copy + buffer share).
 * @return: 0 성공, -EBUSY (이미 활성), -ENOMEM, 기타 errno.
 *
 * RPC ublk_create_target 진입점. SPDK 가 ublk_drv 와 통신할 컨트롤 채널 + per-core
 * poll_group thread 들 생성. 일종의 "ublk subsystem 초기화" 단계.
 *
 * 단계:
 *   1. **중복 활성 검사**: 이미 active 면 -EBUSY (singleton 패턴).
 *   2. **CPU mask 파싱**: g_core_mask 채움.
 *   3. **g_disable_user_copy 저장**: 전체 ublk_dev 가 이 옵션 상속.
 *   4. **poll_groups 배열 calloc**: spdk_env_get_core_count 만큼 (실제 사용은 cpumask 안 코어만).
 *   5. **ublk_open**: /dev/ublk-control 열기 + io_uring 생성 + UBLK_U_CMD_GET_FEATURES.
 *   6. **iobuf 모듈 등록**: ublk_dev 가 사용할 iobuf 풀.
 *   7. **per-core spdk_thread + poll_group 생성**: cpumask 의 각 코어마다 thread 1개.
 *      `ublk_poller_register` 가 그 thread 에서 ublk_poll 등록 (per-thread polled mode).
 *   8. **g_ublk_tgt.active=true** + **ctrl_poller 등록** (1ms interval, /dev/ublk-control 의
 *      CQE 폴링).
 *
 * thread affinity: app_thread (보통 reactor 0) 에서 본 함수 호출 → 각 ublk poll_group
 * thread 가 자기 reactor 에 affine. 이후 ublk_io 처리는 그 thread lockless.
 *
 * 호출 체인:
 *   RPC ublk_create_target → [본 함수] → ublk_open + thread create × N + ctrl_poller
 */
int
ublk_create_target(const char *cpumask_str, bool disable_user_copy)
{
	int rc;
	uint32_t i;
	char thread_name[32];
	struct ublk_poll_group *poll_group;

	/* [한국어] singleton 패턴 — 두 번째 호출은 -EBUSY (재초기화는 destroy 후). */
	if (g_ublk_tgt.active == true) {
		SPDK_ERRLOG("UBLK target has been created\n");
		return -EBUSY;
	}

	rc = ublk_parse_core_mask(cpumask_str);
	if (rc != 0) {
		return rc;
	}

	/* [한국어] 모든 ublk_dev 가 같은 user_copy 옵션 — runtime 변경 불가. */
	g_disable_user_copy = disable_user_copy;

	assert(g_ublk_tgt.poll_groups == NULL);
	/* [한국어] 전체 코어 수만큼 slot — cpumask 에 없는 코어는 unused (단순 indexing). */
	g_ublk_tgt.poll_groups = calloc(spdk_env_get_core_count(), sizeof(*poll_group));
	if (!g_ublk_tgt.poll_groups) {
		return -ENOMEM;
	}

	/* [한국어] /dev/ublk-control 열기 + ctrl io_uring 생성 + UBLK_U_CMD_GET_FEATURES 로
	 * kernel 의 지원 기능 (user_copy/user_recovery/ioctl_encode) 확인. */
	rc = ublk_open();
	if (rc != 0) {
		SPDK_ERRLOG("Fail to open UBLK, error=%s\n", spdk_strerror(-rc));
		free(g_ublk_tgt.poll_groups);
		g_ublk_tgt.poll_groups = NULL;
		return rc;
	}

	spdk_iobuf_register_module("ublk");

	/* [한국어] cpumask 안 코어마다 thread 생성. _FOREACH_CORE 로 SPDK reactor 가 attach 된
	 * 코어만 순회. */
	SPDK_ENV_FOREACH_CORE(i) {
		if (!spdk_cpuset_get_cpu(&g_core_mask, i)) {
			continue;
		}
		snprintf(thread_name, sizeof(thread_name), "ublk_thread%u", i);
		poll_group = &g_ublk_tgt.poll_groups[g_num_ublk_poll_groups];
		poll_group->ublk_thread = spdk_thread_create(thread_name, &g_core_mask);
		/* [한국어] thread create 직후 send_msg — ublk_poller_register 가 그 thread context
		 * 에서 ublk_poll 등록 (poller 가 등록 thread 의 reactor 에 affine). */
		spdk_thread_send_msg(poll_group->ublk_thread, ublk_poller_register, poll_group);
		g_num_ublk_poll_groups++;
	}

	/* [한국어] app_thread (reactor 0) 검증 — ctrl_poller 등록은 app thread 한정. */
	assert(spdk_thread_is_app_thread(NULL));
	g_ublk_tgt.active = true;
	g_ublk_tgt.ctrl_ops_in_progress = 0;
	/* [한국어] ctrl_poller — /dev/ublk-control 의 CQE 폴링 (add_dev/start_dev/del_dev 응답).
	 * 1ms interval = 적당히 빠른 응답 + 낮은 CPU 사용. */
	g_ublk_tgt.ctrl_poller = SPDK_POLLER_REGISTER(ublk_ctrl_poller, NULL,
				 UBLK_DEFAULT_CTRL_URING_POLLING_INTERVAL_US);

	SPDK_NOTICELOG("UBLK target created successfully\n");

	return 0;
}

/*
 * [한국어]
 * _ublk_fini_done - target teardown 의 최종 단계 — 전역 상태 리셋 + 사용자 fini 콜백 호출.
 *
 * @args: 미사용.
 *
 * _ublk_fini → spdk_for_each_thread(ublk_thread_exit, _ublk_fini_done) 의 완료 콜백.
 * 모든 poll_group thread 가 exit 한 뒤 호출되어 g_num_ublk_poll_groups/active/features 등
 * 전역 상태를 초기값으로 되돌리고, spdk_ublk_fini 가 등록한 사용자 cb_fn 을 호출(한 번만,
 * NULL 클리어)한 뒤 poll_groups 배열을 free 한다. 이로써 ublk subsystem 은 다시 create 가능.
 * 실행 컨텍스트: app_thread (for_each_thread 완료 콜백).
 *
 * 호출 체인:
 *   _ublk_fini → spdk_for_each_thread(..., [본 함수]) → g_ublk_tgt.cb_fn 사용자 콜백
 */
static void
_ublk_fini_done(void *args)
{
	SPDK_DEBUGLOG(ublk, "\n");

	g_num_ublk_poll_groups = 0;      /* [한국어] poll_group 개수 리셋 — 다음 create 를 위해 0. */
	g_next_ublk_poll_group = 0;      /* [한국어] round-robin 커서 리셋. */
	g_ublk_tgt.is_destroying = false;  /* [한국어] teardown 완료 — destroy 플래그 해제. */
	g_ublk_tgt.active = false;       /* [한국어] target 비활성 — 다시 create 가능 상태. */
	g_ublk_tgt.features = 0;         /* [한국어] 커널 feature 캐시 초기화. */
	g_ublk_tgt.ioctl_encode = false; /* [한국어] 파생 bool 들 리셋. */
	g_ublk_tgt.user_copy = false;
	g_ublk_tgt.user_recovery = false;

	/* [한국어] 사용자 fini 콜백 (RPC/subsystem fini) 1회 통지 후 NULL 클리어. */
	if (g_ublk_tgt.cb_fn) {
		g_ublk_tgt.cb_fn(g_ublk_tgt.cb_arg);
		g_ublk_tgt.cb_fn = NULL;
		g_ublk_tgt.cb_arg = NULL;
	}

	/* [한국어] poll_groups 배열 해제 — create 시 다시 calloc. */
	if (g_ublk_tgt.poll_groups) {
		free(g_ublk_tgt.poll_groups);
		g_ublk_tgt.poll_groups = NULL;
	}

}

/*
 * [한국어]
 * ublk_thread_exit - 각 poll_group thread 에서 자신의 poller/iobuf 를 정리하고 thread 종료.
 *
 * @args: 미사용.
 *
 * _ublk_fini 가 호출한 spdk_for_each_thread 가 모든 SPDK thread 에서 본 함수를 실행한다.
 * 자신(현재 thread)이 어떤 poll_group 의 ublk_thread 인지 배열에서 찾아, 매칭되면 ublk_poll
 * poller 해제, iobuf 채널 fini, spdk_thread_bind(false) 로 코어 고정 해제, spdk_thread_exit 로
 * 종료를 시작한다. for_each_thread 가 ublk thread 가 아닌 thread 들도 순회하므로 매칭 검사 필수.
 * 실행 컨텍스트: 각 SPDK thread (for_each_thread 디스패치).
 *
 * 호출 체인:
 *   _ublk_fini → spdk_for_each_thread([본 함수], ..., _ublk_fini_done)
 */
static void
ublk_thread_exit(void *args)
{
	struct spdk_thread *ublk_thread = spdk_get_thread();  /* [한국어] 현재 실행 중인 thread. */
	uint32_t i;

	/* [한국어] 현재 thread 가 ublk poll_group 들 중 하나인지 탐색. */
	for (i = 0; i < g_num_ublk_poll_groups; i++) {
		if (g_ublk_tgt.poll_groups[i].ublk_thread == ublk_thread) {
			spdk_poller_unregister(&g_ublk_tgt.poll_groups[i].ublk_poller);  /* [한국어] ublk_poll 중지. */
			spdk_iobuf_channel_fini(&g_ublk_tgt.poll_groups[i].iobuf_ch);    /* [한국어] iobuf 채널 반납. */
			spdk_thread_bind(ublk_thread, false);  /* [한국어] register 시 건 코어 고정 해제. */
			spdk_thread_exit(ublk_thread);         /* [한국어] 이 spdk_thread 종료 개시. */
		}
	}
}

/*
 * [한국어]
 * ublk_close_dev - 디바이스에 STOP_DEV 를 보내 닫기를 개시 (idempotent).
 *
 * @ublk: 닫을 디바이스.
 * @return: 0 (STOP_DEV 제출 성공), -EBUSY (이미 닫는 중), 음수 errno.
 *
 * 디바이스 teardown 의 첫 ctrl 명령. is_closing 플래그로 중복 진입을 막고(이미 true 면 -EBUSY),
 * UBLK_CMD_STOP_DEV 를 비동기 제출한다. STOP_DEV 가 완료되면 커널이 사용자 IO 를 차단하고
 * 각 큐가 drain 되며, 이후 ublk_try_close_queue → ublk_try_close_dev → DEL_DEV 로 이어진다.
 * 실행 컨텍스트: app_thread (fini / stop_disk / ctrl 에러 롤백 경로).
 *
 * 호출 체인:
 *   _ublk_fini / ublk_stop_disk / ublk_ctrl_cmd_error → [본 함수] → ublk_ctrl_cmd_submit(STOP_DEV)
 */
static int
ublk_close_dev(struct spdk_ublk_dev *ublk)
{
	int rc;

	/* set is_closing */
	if (ublk->is_closing) {   /* [한국어] 중복 닫기 방지 — 이미 진행 중이면 -EBUSY. */
		return -EBUSY;
	}
	ublk->is_closing = true;  /* [한국어] 닫기 진행 표시 — 신규 STOP/stop 거부 근거. */

	rc = ublk_ctrl_cmd_submit(ublk, UBLK_CMD_STOP_DEV);  /* [한국어] 커널에 디바이스 중지 요청. */
	if (rc < 0) {
		SPDK_ERRLOG("stop dev %d failed\n", ublk->ublk_id);
	}
	return rc;
}

/*
 * [한국어]
 * _ublk_fini - 모든 ublk 디바이스를 닫고, 다 닫히면 target 자원(ctrl ring/fd/thread)을 해제.
 *
 * @args: 미사용.
 *
 * spdk_ublk_fini 의 작업 루프. (1) 살아있는 모든 디바이스에 ublk_close_dev 를 보내고,
 * (2) g_ublk_devs 가 비었는지 확인한다. 아직 디바이스가 남아 있으면 spdk_thread_send_msg 로
 * **자기 자신을 다시 스케줄**하여 다음 reactor iteration 에 재검사한다(비동기 close 가 완료될
 * 시간을 줌). 모두 닫히면 ctrl_poller 해제, ctrl io_uring exit, ctrl_fd close 후
 * spdk_for_each_thread 로 각 poll_group thread 를 종료하고 _ublk_fini_done 으로 마무리.
 * 실행 컨텍스트: app_thread (재진입은 send_msg 로 같은 thread 에서).
 *
 * 호출 체인:
 *   spdk_ublk_fini → [본 함수] (재귀 send_msg) → spdk_for_each_thread(ublk_thread_exit, _ublk_fini_done)
 */
static void
_ublk_fini(void *args)
{
	struct spdk_ublk_dev	*ublk, *ublk_tmp;

	/* [한국어] 살아있는 모든 디바이스에 닫기 명령 — _SAFE 로 순회 중 제거 안전. */
	TAILQ_FOREACH_SAFE(ublk, &g_ublk_devs, tailq, ublk_tmp) {
		ublk_close_dev(ublk);
	}

	/* Check if all ublks closed */
	/* [한국어] 모든 디바이스가 실제 제거되었는지 확인 (close 는 비동기). */
	if (TAILQ_EMPTY(&g_ublk_devs)) {
		SPDK_DEBUGLOG(ublk, "finish shutdown\n");
		spdk_poller_unregister(&g_ublk_tgt.ctrl_poller);  /* [한국어] ctrl plane 폴링 중지. */
		/* [한국어] ctrl io_uring 해제 — fd 유효할 때만 (init 안 됐으면 -1). */
		if (g_ublk_tgt.ctrl_ring.ring_fd >= 0) {
			io_uring_queue_exit(&g_ublk_tgt.ctrl_ring);
			g_ublk_tgt.ctrl_ring.ring_fd = -1;
		}
		/* [한국어] /dev/ublk-control fd 닫기. */
		if (g_ublk_tgt.ctrl_fd >= 0) {
			close(g_ublk_tgt.ctrl_fd);
			g_ublk_tgt.ctrl_fd = -1;
		}
		/* [한국어] 모든 thread 에서 ublk_thread_exit 실행 후 _ublk_fini_done 으로 최종 정리. */
		spdk_for_each_thread(ublk_thread_exit, NULL, _ublk_fini_done);
	} else {
		/* [한국어] 아직 닫히는 중 → 다음 iteration 에 자신을 재스케줄 (busy-wait 없이 양보). */
		spdk_thread_send_msg(spdk_get_thread(), _ublk_fini, NULL);
	}
}

/*
 * [한국어]
 * spdk_ublk_fini - ublk subsystem 종료 진입점 (공개 API).
 *
 * @cb_fn: 종료 완료 시 호출할 사용자 콜백.
 * @cb_arg: cb_fn 인자.
 * @return: 0 시작 성공, -EBUSY (이미 종료 진행 중).
 *
 * subsystem fini 또는 ublk_destroy_target 이 호출. is_destroying 플래그로 재진입을 막고,
 * 콜백을 저장한 뒤 _ublk_fini 워크 루프를 시작한다. 실제 완료는 비동기로 _ublk_fini_done 에서.
 * 실행 컨텍스트: app_thread (assert 강제).
 *
 * 호출 체인:
 *   subsystem fini / ublk_destroy_target → [본 함수] → _ublk_fini
 */
int
spdk_ublk_fini(spdk_ublk_fini_cb cb_fn, void *cb_arg)
{
	assert(spdk_thread_is_app_thread(NULL));  /* [한국어] 전역 상태 단독 접근 — app_thread 한정. */

	if (g_ublk_tgt.is_destroying == true) {   /* [한국어] 이미 종료 진행 중이면 중복 거부. */
		/* UBLK target is being destroying */
		return -EBUSY;
	}
	g_ublk_tgt.cb_fn = cb_fn;       /* [한국어] 완료 콜백 저장 — _ublk_fini_done 이 호출. */
	g_ublk_tgt.cb_arg = cb_arg;
	g_ublk_tgt.is_destroying = true;  /* [한국어] 종료 진행 표시. */
	_ublk_fini(NULL);               /* [한국어] 비동기 teardown 루프 시작. */

	return 0;
}

/*
 * [한국어]
 * ublk_destroy_target - RPC ublk_destroy_target 핸들러 — active 검증 후 fini 위임.
 *
 * @cb_fn: 종료 완료 콜백.
 * @cb_arg: 콜백 인자.
 * @return: 0, -ENOENT (target 미생성), 또는 spdk_ublk_fini 결과.
 *
 * spdk_ublk_fini 와의 차이는 active 검증 — create 되지 않은 target 을 destroy 하면 -ENOENT.
 * 실행 컨텍스트: app_thread (RPC 핸들러).
 *
 * 호출 체인:
 *   RPC ublk_destroy_target → [본 함수] → spdk_ublk_fini
 */
int
ublk_destroy_target(spdk_ublk_fini_cb cb_fn, void *cb_arg)
{
	int rc;

	if (g_ublk_tgt.active == false) {   /* [한국어] 생성된 적 없는 target → -ENOENT. */
		/* UBLK target has not been created */
		return -ENOENT;
	}

	rc = spdk_ublk_fini(cb_fn, cb_arg);  /* [한국어] 실제 종료 로직 위임. */

	return rc;
}

/*
 * [한국어]
 * ublk_dev_find_by_id - ublk_id 로 전역 디바이스 리스트에서 디바이스 검색.
 *
 * @ublk_id: 디바이스 인덱스 (= /dev/ublkb<N> 의 N).
 * @return: 매칭 spdk_ublk_dev 포인터, 없으면 NULL.
 *
 * 실행 컨텍스트: app_thread (g_ublk_devs 는 app_thread 만 수정).
 *
 * 호출 체인:
 *   ublk_stop_disk / ublk_start_disk / RPC 등 → [본 함수]
 */
struct spdk_ublk_dev *
ublk_dev_find_by_id(uint32_t ublk_id)
{
	struct spdk_ublk_dev *ublk;

	/* check whether ublk has already been registered by ublk path. */
	/* [한국어] g_ublk_devs 선형 탐색 (디바이스 수는 적어 O(n) 무방). */
	TAILQ_FOREACH(ublk, &g_ublk_devs, tailq) {
		if (ublk->ublk_id == ublk_id) {
			return ublk;
		}
	}

	return NULL;   /* [한국어] 미등록 ID. */
}

/*
 * [한국어]
 * ublk_dev_get_id - 디바이스의 ublk_id 접근자 (RPC/로그용).
 * @ublk: 대상 디바이스. @return: ublk_id.
 * 실행 컨텍스트: app_thread. 호출 체인: RPC/로그 → [본 함수].
 */
uint32_t
ublk_dev_get_id(struct spdk_ublk_dev *ublk)
{
	return ublk->ublk_id;   /* [한국어] 디바이스 인덱스 반환. */
}

/*
 * [한국어]
 * ublk_dev_first - 전역 디바이스 리스트의 첫 디바이스 반환 (열거 시작점).
 * @return: 첫 spdk_ublk_dev, 비었으면 NULL. 호출 체인: RPC list 핸들러 → [본 함수].
 */
struct spdk_ublk_dev *ublk_dev_first(void)
{
	return TAILQ_FIRST(&g_ublk_devs);   /* [한국어] g_ublk_devs head. */
}

/*
 * [한국어]
 * ublk_dev_next - 디바이스 리스트 순회 — prev 다음 디바이스.
 * @prev: 현재 디바이스. @return: 다음 디바이스, 끝이면 NULL.
 * 호출 체인: RPC list 핸들러 순회 → [본 함수].
 */
struct spdk_ublk_dev *ublk_dev_next(struct spdk_ublk_dev *prev)
{
	return TAILQ_NEXT(prev, tailq);   /* [한국어] tailq 링크의 다음 노드. */
}

/*
 * [한국어]
 * ublk_dev_get_queue_depth - 디바이스 큐 깊이 접근자.
 * @ublk: 대상. @return: queue_depth. 호출 체인: RPC 정보 조회 → [본 함수].
 */
uint32_t
ublk_dev_get_queue_depth(struct spdk_ublk_dev *ublk)
{
	return ublk->queue_depth;   /* [한국어] 큐당 깊이. */
}

/*
 * [한국어]
 * ublk_dev_get_num_queues - 디바이스 큐 개수 접근자.
 * @ublk: 대상. @return: num_queues. 호출 체인: RPC 정보 조회 → [본 함수].
 */
uint32_t
ublk_dev_get_num_queues(struct spdk_ublk_dev *ublk)
{
	return ublk->num_queues;   /* [한국어] 큐 개수. */
}

/*
 * [한국어]
 * ublk_dev_get_bdev_name - 디바이스의 백엔드 bdev 이름 접근자.
 * @ublk: 대상. @return: bdev 이름 문자열.
 * 호출 체인: write_config_json / RPC 정보 조회 → [본 함수] → spdk_bdev_get_name.
 */
const char *
ublk_dev_get_bdev_name(struct spdk_ublk_dev *ublk)
{
	return spdk_bdev_get_name(ublk->bdev);   /* [한국어] bdev 객체에서 이름 추출. */
}

/*
 * [한국어]
 * spdk_ublk_write_config_json - 현재 ublk 구성을 RPC config 재생용 JSON 으로 직렬화 (공개 API).
 *
 * @w: JSON write 컨텍스트.
 *
 * spdk_subsystem_config_json 이 호출 — 현재 런타임 상태를 RPC 명령 배열로 덤프해, 나중에
 * 동일 구성을 재생할 수 있게 한다. (1) target 이 active 면 ublk_create_target 명령(cpumask),
 * (2) 각 디바이스마다 ublk_start_disk 명령(bdev_name/ublk_id/num_queues/queue_depth)을 JSON
 * 객체로 출력한다. 출력은 RPC 클라이언트가 그대로 다시 보낼 수 있는 method/params 형태.
 * 실행 컨텍스트: app_thread (config dump).
 *
 * 호출 체인:
 *   subsystem config_json → [본 함수] → spdk_json_write_*
 */
void
spdk_ublk_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_ublk_dev *ublk;

	spdk_json_write_array_begin(w);   /* [한국어] RPC 명령 배열 시작. */

	/* [한국어] target 이 활성이면 ublk_create_target 재생 명령 1건. */
	if (g_ublk_tgt.active) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "ublk_create_target");   /* [한국어] RPC method 이름. */
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "cpumask", spdk_cpuset_fmt(&g_core_mask));  /* [한국어] 코어 mask 재현. */
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	/* [한국어] 각 디바이스마다 ublk_start_disk 재생 명령. */
	TAILQ_FOREACH(ublk, &g_ublk_devs, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "ublk_start_disk");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "bdev_name", ublk_dev_get_bdev_name(ublk));  /* [한국어] 백엔드 bdev. */
		spdk_json_write_named_uint32(w, "ublk_id", ublk->ublk_id);        /* [한국어] 디바이스 인덱스. */
		spdk_json_write_named_uint32(w, "num_queues", ublk->num_queues);  /* [한국어] 큐 개수. */
		spdk_json_write_named_uint32(w, "queue_depth", ublk->queue_depth);  /* [한국어] 큐 깊이. */
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);   /* [한국어] 배열 닫기. */
}

/*
 * [한국어]
 * ublk_dev_list_register - 디바이스를 전역 리스트에 등록 + 활성 디바이스 카운트 증가.
 * @ublk: 등록할 디바이스. 실행 컨텍스트: app_thread.
 * 호출 체인: ublk_start_disk(성공 경로) → [본 함수].
 */
static void
ublk_dev_list_register(struct spdk_ublk_dev *ublk)
{
	UBLK_DEBUGLOG(ublk, "add to tailq\n");
	TAILQ_INSERT_TAIL(&g_ublk_devs, ublk, tailq);  /* [한국어] g_ublk_devs 끝에 추가. */
	g_ublk_tgt.num_ublk_devs++;                    /* [한국어] 활성 디바이스 수 +1. */
}

/*
 * [한국어]
 * ublk_dev_list_unregister - 디바이스를 전역 리스트에서 제거 (등록되어 있었던 경우만).
 *
 * @ublk: 제거할 디바이스.
 *
 * 디바이스는 등록 전에 stop 될 수 있으므로 ublk_dev_find_by_id 로 실제 등록 여부를 먼저 확인.
 * 등록돼 있으면 리스트에서 빼고 카운트 감소, 아니면 (정상 경로에선 발생하면 안 되므로) assert.
 * 실행 컨텍스트: app_thread. 호출 체인: ublk_free_dev → [본 함수].
 */
static void
ublk_dev_list_unregister(struct spdk_ublk_dev *ublk)
{
	/*
	 * ublk device may be stopped before registered.
	 * check whether it was registered.
	 */

	/* [한국어] 실제 등록되어 있을 때만 제거 (등록 전 stop 경로 방어). */
	if (ublk_dev_find_by_id(ublk->ublk_id)) {
		UBLK_DEBUGLOG(ublk, "remove from tailq\n");
		TAILQ_REMOVE(&g_ublk_devs, ublk, tailq);   /* [한국어] 리스트에서 제거. */
		assert(g_ublk_tgt.num_ublk_devs);          /* [한국어] 카운트 underflow 방어. */
		g_ublk_tgt.num_ublk_devs--;
		return;
	}

	UBLK_DEBUGLOG(ublk, "not found in tailq\n");
	assert(false);   /* [한국어] 등록 안 된 디바이스를 unregister 시도 = 로직 버그. */
}

/*
 * [한국어]
 * ublk_delete_dev - 디바이스 자원(큐/cdev fd)을 정리하고 커널에 DEL_DEV 발행.
 *
 * @arg: 대상 spdk_ublk_dev.
 *
 * teardown 의 마지막 단계 또는 ctrl 에러 롤백 경로. 모든 큐의 io_uring 을 fini 하고
 * /dev/ublkcN fd 를 닫은 뒤 UBLK_CMD_DEL_DEV 를 비동기 제출한다. DEL_DEV 완료 시
 * ublk_ctrl_process_cqe 가 사용자 콜백 통지 + ublk_free_dev 로 메모리를 해제한다.
 * 실행 컨텍스트: app_thread (assert 강제).
 *
 * 호출 체인:
 *   ublk_try_close_dev / _ublk_close_dev_retry / ublk_ctrl_cmd_error → [본 함수]
 *     → ublk_dev_queue_fini × N + ublk_ctrl_cmd_submit(DEL_DEV)
 */
static void
ublk_delete_dev(void *arg)
{
	struct spdk_ublk_dev *ublk = arg;
	int rc = 0;
	uint32_t q_idx;

	assert(spdk_thread_is_app_thread(NULL));   /* [한국어] app_thread 한정 — 큐/fd 정리. */
	/* [한국어] 모든 큐의 io_uring 자원 해제. */
	for (q_idx = 0; q_idx < ublk->num_queues; q_idx++) {
		ublk_dev_queue_fini(&ublk->queues[q_idx]);
	}

	/* [한국어] /dev/ublkcN fd 닫기 (열려 있었으면). */
	if (ublk->cdev_fd >= 0) {
		close(ublk->cdev_fd);
	}

	rc = ublk_ctrl_cmd_submit(ublk, UBLK_CMD_DEL_DEV);  /* [한국어] 커널에서 디바이스 제거. */
	if (rc < 0) {
		SPDK_ERRLOG("delete dev %d failed\n", ublk->ublk_id);
	}
}

/*
 * [한국어]
 * _ublk_close_dev_retry - STOP_DEV 후 잔여 ctrl 명령이 끝나길 기다렸다가 DEL_DEV 진행하는 poller.
 *
 * @arg: 대상 spdk_ublk_dev.
 * @return: SPDK_POLLER_BUSY.
 *
 * ublk_try_close_dev 가 모든 큐는 닫혔지만 아직 inflight ctrl 명령(예: STOP_DEV)이 남았을 때
 * 등록하는 재시도 poller. ctrl_ops_in_progress 가 0 이 될 때까지 retry_count 만큼 기다리고,
 * 한도(10초/20ms ≈ 500회) 초과 시 타임아웃 로그 후 강제로 ublk_delete_dev 진행. 끝나면
 * 자신을 unregister.
 * 실행 컨텍스트: app_thread poller.
 *
 * 호출 체인:
 *   ublk_try_close_dev → SPDK_POLLER_REGISTER → [본 함수] → ublk_delete_dev
 */
static int
_ublk_close_dev_retry(void *arg)
{
	struct spdk_ublk_dev *ublk = arg;

	/* [한국어] 아직 ctrl 명령이 진행 중이면 카운트 다운하며 대기. */
	if (ublk->ctrl_ops_in_progress > 0) {
		if (ublk->retry_count-- > 0) {
			return SPDK_POLLER_BUSY;   /* [한국어] 한도 내 → 다음 주기 재시도. */
		}
		SPDK_ERRLOG("Timeout on ctrl op completion.\n");   /* [한국어] 한도 초과 → 강제 진행. */
	}
	spdk_poller_unregister(&ublk->retry_poller);   /* [한국어] 1회성 — 진행 결정 시 해제. */
	ublk_delete_dev(ublk);                         /* [한국어] DEL_DEV 발행. */
	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * ublk_try_close_dev - 모든 큐가 닫히면 디바이스 삭제로 진행 (큐 close 집계 지점).
 *
 * @arg: 대상 spdk_ublk_dev.
 *
 * 각 큐가 drain 완료 후 ublk_try_close_queue 가 app_thread 로 send_msg 해 호출한다.
 * queues_closed 를 증가시키고, 아직 모든 큐가 닫히지 않았으면 리턴. 모두 닫혔으면
 * inflight ctrl 명령 유무에 따라 분기: 남아 있으면 재시도 poller(_ublk_close_dev_retry)를
 * 걸고, 없으면 즉시 ublk_delete_dev. retry_count 는 (10초/20ms) 로 산정.
 * 실행 컨텍스트: app_thread (큐 thread 에서 send_msg 로 디스패치).
 *
 * 호출 체인:
 *   ublk_try_close_queue → spdk_thread_send_msg(app_thread, [본 함수]) → ublk_delete_dev / retry_poller
 */
static void
ublk_try_close_dev(void *arg)
{
	struct spdk_ublk_dev *ublk = arg;

	assert(spdk_thread_is_app_thread(NULL));   /* [한국어] 집계는 app_thread 단일 처리 → 락 불필요. */

	ublk->queues_closed += 1;   /* [한국어] 닫힌 큐 수 +1. */
	SPDK_DEBUGLOG(ublk_io, "ublkb%u closed queues %u\n", ublk->ublk_id, ublk->queues_closed);

	if (ublk->queues_closed < ublk->num_queues) {   /* [한국어] 아직 다 안 닫힘 → 대기. */
		return;
	}

	/* [한국어] 모든 큐 닫힘 → inflight ctrl 명령 유무로 분기. */
	if (ublk->ctrl_ops_in_progress > 0) {
		assert(ublk->retry_poller == NULL);   /* [한국어] 중복 poller 방지. */
		/* [한국어] 10초 / 20ms ≈ 500회 재시도 한도 산정. */
		ublk->retry_count = UBLK_STOP_BUSY_WAITING_MS * 1000ULL / UBLK_BUSY_POLLING_INTERVAL_US;
		ublk->retry_poller = SPDK_POLLER_REGISTER(_ublk_close_dev_retry, ublk,
				     UBLK_BUSY_POLLING_INTERVAL_US);
	} else {
		ublk_delete_dev(ublk);   /* [한국어] 잔여 명령 없음 → 즉시 DEL_DEV. */
	}
}

/*
 * [한국어]
 * ublk_try_close_queue - 큐의 모든 IO 가 drain 되면 큐를 닫고 디바이스 close 집계로 통지.
 *
 * @q: 닫으려는 큐.
 *
 * 큐 stop 진행 중(is_stopping) ublk_poll 등이 주기적으로 호출. inflight/completed IO 가
 * 남아 있거나 cmd_inflight 가 0 이 아니면 아직 닫을 수 없으므로 리턴(다음 retry 대기).
 * 모든 IO 가 정리되면 poll_group 의 queue_list 에서 제거하고 bdev io_channel 을 반납한 뒤,
 * **app_thread 로 send_msg** 하여 ublk_try_close_dev 를 호출한다(큐는 poll_group thread 에서
 * 닫지만 디바이스 집계는 app_thread 가 단독 처리 — cross-thread 안전).
 * 실행 컨텍스트: 큐 소유 poll_group thread.
 *
 * 호출 체인:
 *   ublk_poll(stop 중) → [본 함수] → spdk_thread_send_msg(app_thread, ublk_try_close_dev)
 */
static void
ublk_try_close_queue(struct ublk_queue *q)
{
	struct spdk_ublk_dev *ublk = q->dev;

	/* Close queue until no I/O is submitted to bdev in flight,
	 * no I/O is waiting to commit result, and all I/Os are aborted back.
	 */
	/* [한국어] 진행/완료대기 IO 또는 uring inflight 가 남으면 아직 닫지 못함 → 다음 retry. */
	if (!TAILQ_EMPTY(&q->inflight_io_list) || !TAILQ_EMPTY(&q->completed_io_list) || q->cmd_inflight) {
		/* wait for next retry */
		return;
	}

	TAILQ_REMOVE(&q->poll_group->queue_list, q, tailq);   /* [한국어] poll_group 에서 큐 제거 → 더 이상 폴링 안 됨. */
	spdk_put_io_channel(q->bdev_ch);   /* [한국어] bdev io_channel 반납. */
	q->bdev_ch = NULL;

	/* [한국어] 디바이스 close 집계는 app_thread 단독 → cross-thread send_msg. */
	spdk_thread_send_msg(spdk_thread_get_app_thread(), ublk_try_close_dev, ublk);
}

/*
 * [한국어]
 * ublk_stop_disk - RPC ublk_stop_disk 핸들러 — 디바이스 닫기 개시 + 완료 콜백 등록.
 *
 * @ublk_id: 닫을 디바이스 인덱스.
 * @ctrl_cb: 닫기 완료 시 호출할 RPC 응답 콜백.
 * @cb_arg: ctrl_cb 인자.
 * @return: 0, -ENODEV (없음), -EBUSY (이미 닫는 중 / RPC 진행 중).
 *
 * ublk_id 로 디바이스를 찾아 (1) 이미 닫는 중인지(is_closing), (2) 다른 RPC 가 진행 중인지
 * (ctrl_cb 점유) 검사 후, 완료 콜백을 저장하고 ublk_close_dev 로 닫기를 개시한다.
 * 실행 컨텍스트: app_thread (RPC 핸들러, assert 강제).
 *
 * 호출 체인:
 *   RPC ublk_stop_disk → [본 함수] → ublk_close_dev(STOP_DEV)
 */
int
ublk_stop_disk(uint32_t ublk_id, ublk_ctrl_cb ctrl_cb, void *cb_arg)
{
	struct spdk_ublk_dev *ublk;

	assert(spdk_thread_is_app_thread(NULL));   /* [한국어] RPC 핸들러 — app_thread 한정. */

	ublk = ublk_dev_find_by_id(ublk_id);   /* [한국어] ID 로 디바이스 조회. */
	if (ublk == NULL) {
		SPDK_ERRLOG("no ublk dev with ublk_id=%u\n", ublk_id);
		return -ENODEV;
	}
	if (ublk->is_closing) {   /* [한국어] 이미 닫는 중 → 중복 거부. */
		SPDK_WARNLOG("ublk %d is closing\n", ublk->ublk_id);
		return -EBUSY;
	}
	if (ublk->ctrl_cb) {   /* [한국어] 다른 RPC ctrl 명령 진행 중 → 거부 (콜백 슬롯 1개). */
		SPDK_WARNLOG("ublk %d is busy with RPC call\n", ublk->ublk_id);
		return -EBUSY;
	}

	ublk->ctrl_cb = ctrl_cb;   /* [한국어] 완료 콜백 등록 — DEL_DEV 완료 시 통지. */
	ublk->cb_arg = cb_arg;
	return ublk_close_dev(ublk);   /* [한국어] STOP_DEV 개시. */
}

/*
 * [한국어]
 * ublk_mark_io_done - IO 를 "완료" 상태로 마킹해 commit-and-fetch 로 보고할 준비.
 *
 * @io: 대상 IO.
 * @res: bdev 결과 (0 또는 음수 errno) — 커널에 전달될 응답 코드.
 *
 * bdev I/O 가 끝나면 ublk_io 의 다음 uring 명령을 UBLK_IO_COMMIT_AND_FETCH_REQ 로 설정한다.
 * 이 한 SQE 가 (1) 직전 IO 결과(result)를 커널에 commit 하고 (2) 동시에 같은 슬롯의 다음
 * 요청을 fetch 한다(ublk 의 단일 round-trip 최적화). need_data 는 false 로 클리어 — 응답
 * 단계는 추가 데이터 GET 이 불필요하기 때문.
 * 실행 컨텍스트: 큐 소유 poll_group thread (bdev 완료 콜백 내부).
 *
 * 호출 체인:
 *   ublk_io_done → [본 함수]
 */
static inline void
ublk_mark_io_done(struct ublk_io *io, int res)
{
	/*
	 * mark io done by target, so that SPDK can commit its
	 * result and fetch new request via io_uring command.
	 */
	io->cmd_op = UBLK_IO_COMMIT_AND_FETCH_REQ;  /* [한국어] 결과 commit + 다음 요청 fetch 를 한 SQE 로. */
	io->result = res;        /* [한국어] 커널에 보고할 결과 코드 (cqe.res 로 전달). */
	io->need_data = false;   /* [한국어] 응답 단계는 데이터 GET 불필요. */
}

/*
 * [한국어]
 * ublk_io_done - bdev I/O 완료 콜백 — inflight → completed list 이동.
 *
 * @bdev_io: 완료된 bdev I/O (NULL 가능 — user_copy_read_done error 경로).
 * @success: bdev 측 성공 여부.
 * @cb_arg: ublk_io.
 *
 * spdk_bdev_readv/writev 등의 완료 콜백. result 결정 (success 면 io->result 보존 값,
 * 실패면 -EIO) → ublk_mark_io_done 가 io->result + status 세팅 → completed_io_list 이동.
 *
 * inflight → completed 이동 의미: 다음 ublk_io_xmit (poller iteration) 가 completed 큐
 * 순회하며 commit-and-fetch SQE 발행 → ublk_drv 에 응답 전달.
 *
 * bdev_io NULL 케이스: user_copy_read_done 에러 fallthrough 시 — bdev_io 는 이미 free 됨.
 * 본 함수 호출자가 다중 경로라 NULL 가드 필수.
 *
 * 호출 체인:
 *   spdk_bdev_readv 완료 → [본 함수]
 *     → ublk_mark_io_done + list 이동
 *     → 다음 ublk_poll → ublk_io_xmit → CQE → ublk_drv → 사용자 syscall unblock
 */
static void
ublk_io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ublk_io	*io = cb_arg;
	struct ublk_queue *q = io->q;
	int res;

	/* [한국어] success 시 io->result 보존값 (io_xmit 가 응답 cqe.res 로 사용),
	 * 실패 시 -EIO 표준 에러. */
	if (success) {
		res = io->result;
	} else {
		res = -EIO;
	}

	ublk_mark_io_done(io, res);

	SPDK_DEBUGLOG(ublk_io, "(qid %d tag %d res %d)\n",
		      q->q_id, io->tag, res);
	/* [한국어] list 이동 — 다음 ublk_poll iteration 의 xmit 가 처리. */
	TAILQ_REMOVE(&q->inflight_io_list, io, tailq);
	TAILQ_INSERT_TAIL(&q->completed_io_list, io, tailq);

	/* [한국어] bdev_io NULL = 다른 경로에서 이미 free 한 경우 (user_copy error fallthrough). */
	if (bdev_io != NULL) {
		spdk_bdev_free_io(bdev_io);
	}
}

/*
 * [한국어]
 * ublk_queue_user_copy - USER_COPY 모드에서 SPDK payload ↔ 커널 buffer 간 데이터 전송 SQE 발행.
 *
 * @io: 대상 IO.
 * @is_write: true=쓰기 IO (커널 user buffer → SPDK payload 로 read), false=읽기 IO (SPDK payload
 *            → 커널 user buffer 로 write).
 *
 * USER_COPY 모드에서는 데이터가 미리 등록된 buffer 가 아니라 /dev/ublkcN 의 특정 offset 에
 * 매핑된 커널 영역에 있다. 따라서 (a) WRITE 처리 시 bdev 에 쓰기 전에 커널 buffer 를 SPDK
 * payload 로 **read** 해와야 하고, (b) READ 처리 시 bdev 에서 읽은 데이터를 커널 buffer 로
 * **write** 해줘야 한다 — io_uring_prep_read/write 의 offset 으로 ublk_user_copy_pos 사용.
 * SQE 에 IOSQE_FIXED_FILE(미리 등록한 cdev fd 사용)과 user_data(tag)를 설정하고, io 를
 * completed_io_list 로 옮겨 다음 xmit 가 이 user_copy SQE 를 함께 제출하게 한다.
 * 실행 컨텍스트: 큐 소유 poll_group thread.
 *
 * 호출 체인:
 *   ublk_submit_bdev_io(WRITE, user_copy) / ublk_user_copy_read_done → [본 함수]
 *     → io_uring_prep_read/write
 */
static void
ublk_queue_user_copy(struct ublk_io *io, bool is_write)
{
	struct ublk_queue *q = io->q;
	const struct ublksrv_io_desc *iod = io->iod;   /* [한국어] 커널 IO descriptor (sector 수 등). */
	struct io_uring_sqe *sqe;
	uint64_t pos;       /* [한국어] /dev/ublkcN 내 커널 buffer 가상 offset. */
	uint32_t nbytes;    /* [한국어] 전송 바이트 수. */

	nbytes = iod->nr_sectors * (1ULL << LINUX_SECTOR_SHIFT);  /* [한국어] sector(512B) 수 → 바이트. */
	pos = ublk_user_copy_pos(q->q_id, io->tag);  /* [한국어] 커널 buffer 위치 계산. */
	sqe = io_uring_get_sqe(&q->ring);   /* [한국어] data ring 에서 SQE 1칸. */
	assert(sqe);   /* [한국어] 큐 depth 와 IO 수가 일치하므로 항상 확보 가능. */

	/* [한국어] WRITE IO → 커널 buffer 를 payload 로 read (bdev 쓰기 전 데이터 확보). */
	if (is_write) {
		io_uring_prep_read(sqe, 0, io->payload, nbytes, pos);
	} else {
		/* [한국어] READ IO → bdev 에서 읽은 payload 를 커널 buffer 로 write. */
		io_uring_prep_write(sqe, 0, io->payload, nbytes, pos);
	}
	io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);  /* [한국어] fd 인덱스 0 = 등록된 cdev fd. */
	io_uring_sqe_set_data64(sqe, build_user_data(io->tag, 0));  /* [한국어] CQE 역추적용 tag. */

	io->user_copy = true;   /* [한국어] 이 IO 가 user_copy 단계임을 표시 (xmit/recv 분기). */
	TAILQ_REMOVE(&q->inflight_io_list, io, tailq);   /* [한국어] inflight → completed 이동. */
	TAILQ_INSERT_TAIL(&q->completed_io_list, io, tailq);
}

/*
 * [한국어]
 * ublk_user_copy_read_done - USER_COPY READ IO 의 bdev 읽기 완료 콜백 → 커널 buffer 로 복사 발행.
 *
 * @bdev_io: 완료된 bdev read.
 * @success: 읽기 성공 여부.
 * @cb_arg: ublk_io.
 *
 * USER_COPY 모드 READ 는 2단계다: (1) bdev 에서 SPDK payload 로 읽고(본 콜백 진입), (2) 그
 * payload 를 커널 user buffer 로 write(ublk_queue_user_copy(false)). 성공이면 2단계로 진행,
 * 실패면 bdev_io 를 이미 free 했으므로 ublk_io_done(NULL, false) 로 에러 보고.
 * 실행 컨텍스트: 큐 소유 poll_group thread (bdev 완료 콜백).
 *
 * 호출 체인:
 *   spdk_bdev_readv 완료(user_copy) → [본 함수] → ublk_queue_user_copy(false) / ublk_io_done
 */
static void
ublk_user_copy_read_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ublk_io	*io = cb_arg;

	spdk_bdev_free_io(bdev_io);   /* [한국어] read bdev_io 즉시 반환 (다음 단계는 uring write). */

	/* [한국어] 읽기 성공 → payload 를 커널 buffer 로 복사하는 user_copy write 발행. */
	if (success) {
		ublk_queue_user_copy(io, false);
		return;
	}
	/* READ IO Error */
	ublk_io_done(NULL, false, cb_arg);   /* [한국어] 읽기 실패 → -EIO 응답 (bdev_io 이미 free → NULL). */
}

/*
 * [한국어]
 * ublk_resubmit_io - bdev io_wait 깨어남 시 bdev I/O 재제출 콜백.
 *
 * @arg: ublk_io.
 *
 * spdk_bdev_queue_io_wait 로 매달아 둔 IO 가 bdev 자원 가용 시 호출. 단순히 _ublk_submit_bdev_io
 * 로 재제출한다.
 * 실행 컨텍스트: 큐 소유 poll_group thread (bdev wait 콜백).
 *
 * 호출 체인:
 *   spdk_bdev_queue_io_wait 깨어남 → [본 함수] → _ublk_submit_bdev_io
 */
static void
ublk_resubmit_io(void *arg)
{
	struct ublk_io *io = (struct ublk_io *)arg;

	_ublk_submit_bdev_io(io->q, io);   /* [한국어] 자원 가용 → bdev I/O 다시 제출. */
}

/*
 * [한국어]
 * ublk_queue_io - bdev I/O 발행이 -ENOMEM 이면 io_wait 큐에 매달아 재시도 예약.
 *
 * @io: 재시도 대상 IO.
 *
 * _ublk_submit_bdev_io 가 bdev 자원 부족(-ENOMEM)을 받으면 호출. bdev_io_wait entry 에
 * 재제출 콜백(ublk_resubmit_io)을 채워 spdk_bdev_queue_io_wait 로 등록한다. 등록 자체가
 * 실패하면(드문 경우) ublk_io_done(NULL, false) 로 에러 종료.
 * 실행 컨텍스트: 큐 소유 poll_group thread.
 *
 * 호출 체인:
 *   _ublk_submit_bdev_io(-ENOMEM) → [본 함수] → spdk_bdev_queue_io_wait → (가용 시) ublk_resubmit_io
 */
static void
ublk_queue_io(struct ublk_io *io)
{
	int rc;
	struct spdk_bdev *bdev = io->q->dev->bdev;
	struct ublk_queue *q = io->q;

	io->bdev_io_wait.bdev = bdev;            /* [한국어] 대기 대상 bdev. */
	io->bdev_io_wait.cb_fn = ublk_resubmit_io;  /* [한국어] 자원 가용 시 재제출 콜백. */
	io->bdev_io_wait.cb_arg = io;

	rc = spdk_bdev_queue_io_wait(bdev, q->bdev_ch, &io->bdev_io_wait);  /* [한국어] wait 큐 등록. */
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in ublk_queue_io, rc=%d.\n", rc);
		ublk_io_done(NULL, false, io);   /* [한국어] 등록 실패 → 에러 종료. */
	}
}

/*
 * [한국어]
 * ublk_io_get_buffer_cb - iobuf 풀에서 buffer 를 얻었을 때 호출되는 콜백 — payload 정렬 후 진행.
 *
 * @iobuf: 완료된 iobuf entry (ublk_io 내부 멤버).
 * @buf: 할당된 buffer 포인터.
 *
 * USER_COPY 모드에서 data buffer 를 iobuf 풀에서 비동기로 빌릴 때, 가용 시 호출된다.
 * SPDK_CONTAINEROF 로 entry → ublk_io 복원, buffer 를 4KB 경계로 올림 정렬해 payload 에 저장
 * (bdev DMA 정렬 요건 충족), 원래 IO 가 등록해 둔 get_buf_cb 로 처리 재개.
 * 실행 컨텍스트: 큐 소유 poll_group thread (iobuf 콜백).
 *
 * 호출 체인:
 *   spdk_iobuf_get 콜백 → [본 함수] → io->get_buf_cb (예: read_get_buffer_done)
 */
static void
ublk_io_get_buffer_cb(struct spdk_iobuf_entry *iobuf, void *buf)
{
	struct ublk_io *io = SPDK_CONTAINEROF(iobuf, struct ublk_io, iobuf);  /* [한국어] entry → ublk_io 복원. */

	io->mpool_entry = buf;   /* [한국어] 반환용 원본 buffer 포인터 보관 (put 시 사용). */
	assert(io->payload == NULL);  /* [한국어] 중복 할당 방지. */
	/* [한국어] 4KB 경계로 올림 — bdev DMA 정렬 요건. mpool_entry 로 원본 추적하므로 정렬 손실 OK. */
	io->payload = (void *)(uintptr_t)SPDK_ALIGN_CEIL((uintptr_t)buf, 4096ULL);
	io->get_buf_cb(io);   /* [한국어] buffer 확보 완료 → 원래 처리 재개. */
}

/*
 * [한국어]
 * ublk_io_get_buffer - IO 의 data buffer 를 iobuf 풀에서 (동기 또는 비동기로) 확보.
 *
 * @io: 대상 IO.
 * @iobuf_ch: 이 poll_group thread 의 iobuf 채널 (lockless per-thread 캐시).
 * @get_buf_cb: buffer 확보 완료 후 호출할 콜백 (read_get_buffer_done 등).
 *
 * payload_size 를 iod 의 sector 수로 계산하고 get_buf_cb 를 저장한 뒤 spdk_iobuf_get 을 호출.
 * iobuf 가 즉시 가용하면 buf != NULL 로 동기 반환 → 직접 ublk_io_get_buffer_cb 호출(빠른 경로).
 * 풀이 비어 있으면 NULL 을 반환하고 entry 가 wait 큐에 매달려, 나중에 ublk_io_get_buffer_cb 가
 * 비동기로 호출된다(콜백은 SPDK iobuf 가 트리거).
 * 실행 컨텍스트: 큐 소유 poll_group thread.
 *
 * 호출 체인:
 *   ublk_submit_bdev_io / ublk_io_recv → [본 함수] → spdk_iobuf_get → ublk_io_get_buffer_cb
 */
static void
ublk_io_get_buffer(struct ublk_io *io, struct spdk_iobuf_channel *iobuf_ch,
		   ublk_get_buf_cb get_buf_cb)
{
	void *buf;

	io->payload_size = io->iod->nr_sectors * (1ULL << LINUX_SECTOR_SHIFT);  /* [한국어] sector → 바이트. */
	io->get_buf_cb = get_buf_cb;   /* [한국어] 확보 후 재개 콜백 저장. */
	/* [한국어] iobuf 풀에서 buffer 요청 — 즉시 가용이면 buf, 아니면 NULL + wait 큐 등록. */
	buf = spdk_iobuf_get(iobuf_ch, io->payload_size, &io->iobuf, ublk_io_get_buffer_cb);

	if (buf != NULL) {   /* [한국어] 동기 성공 경로 — 콜백 직접 호출. */
		ublk_io_get_buffer_cb(&io->iobuf, buf);
	}
}

/*
 * [한국어]
 * ublk_io_put_buffer - IO 의 data buffer 를 iobuf 풀로 반환.
 *
 * @io: 대상 IO.
 * @iobuf_ch: poll_group thread 의 iobuf 채널.
 *
 * IO 완료/응답 후 빌린 buffer 를 반납한다. mpool_entry(정렬 전 원본 포인터)로 반환하고
 * payload/mpool_entry 를 NULL 로 클리어해 슬롯 재사용 시 중복 반환을 막는다. payload 가
 * NULL 이면(이미 반환 또는 user_copy 무할당) no-op.
 * 실행 컨텍스트: 큐 소유 poll_group thread.
 *
 * 호출 체인:
 *   ublk_io_xmit (buffer_free_list) → [본 함수] → spdk_iobuf_put
 */
static void
ublk_io_put_buffer(struct ublk_io *io, struct spdk_iobuf_channel *iobuf_ch)
{
	if (io->payload) {   /* [한국어] 빌린 buffer 가 있을 때만 반환. */
		spdk_iobuf_put(iobuf_ch, io->mpool_entry, io->payload_size);  /* [한국어] 정렬 전 원본으로 반납. */
		io->mpool_entry = NULL;   /* [한국어] 중복 반환 방지. */
		io->payload = NULL;
	}
}

/*
 * [한국어]
 * _ublk_submit_bdev_io - ★ ublk_drv 의 IO 요청을 SPDK bdev I/O 로 변환 + 발행.
 *
 * @q: ublk queue.
 * @io: ublk_io (커널 iod 파싱 완료, payload 준비됨).
 *
 * 5종 IO opcode → bdev API 1:1 매핑:
 *   - UBLK_IO_OP_READ → spdk_bdev_read_blocks (user_copy 모드면 user_copy_read_done callback 사용).
 *   - UBLK_IO_OP_WRITE → spdk_bdev_write_blocks.
 *   - UBLK_IO_OP_FLUSH → spdk_bdev_flush_blocks (전체 범위).
 *   - UBLK_IO_OP_DISCARD → spdk_bdev_unmap_blocks.
 *   - UBLK_IO_OP_WRITE_ZEROES → spdk_bdev_write_zeroes_blocks.
 *
 * 단위 변환: ublk_drv 는 512B sector, bdev 는 LBA blocks (보통 4KB). sector_per_block_shift
 * 로 right shift (sector → block 변환). bdev 의 logical block size 가 변경되면 이 shift 도 갱신.
 *
 * 에러 처리:
 *   - -ENOMEM: bdev_io 풀 고갈 → ublk_queue_io 로 wait queue 등록 → 다음 free 시 resubmit.
 *   - 그 외 음수: 즉시 ublk_io_done(false) → 에러 응답 chain.
 *
 * user_copy 모드: read 완료 후 user buffer 로 데이터 복사 필요 → ublk_user_copy_read_done
 * 콜백이 io_uring SQE 로 복사 트리거. user_copy 비활성 시 zero-copy (payload pointer 직접 사용).
 *
 * 호출 체인:
 *   ublk_io_recv (커널 CQE) → ublk_submit_bdev_io → ublk_io_get_buffer (iobuf) → [본 함수]
 *     → spdk_bdev_*_blocks → bdev module → 완료 → ublk_io_done
 */
static void
_ublk_submit_bdev_io(struct ublk_queue *q, struct ublk_io *io)
{
	struct spdk_ublk_dev *ublk = q->dev;
	struct spdk_bdev_desc *desc = io->bdev_desc;
	struct spdk_io_channel *ch = io->bdev_ch;
	uint64_t offset_blocks, num_blocks;
	spdk_bdev_io_completion_cb read_cb;
	uint8_t ublk_op;
	int rc = 0;
	const struct ublksrv_io_desc *iod = io->iod;

	ublk_op = ublksrv_get_op(iod);
	/* [한국어] sector (512B) → block (bdev 의 LBA 단위) 변환. */
	offset_blocks = iod->start_sector >> ublk->sector_per_block_shift;
	num_blocks = iod->nr_sectors >> ublk->sector_per_block_shift;

	switch (ublk_op) {
	case UBLK_IO_OP_READ:
		/* [한국어] user_copy 모드면 완료 후 user buffer 복사 필요 — 별도 콜백.
		 * 비활성 시 payload 가 이미 user 가 볼 영역이라 ublk_io_done 직접. */
		if (g_ublk_tgt.user_copy) {
			read_cb = ublk_user_copy_read_done;
		} else {
			read_cb = ublk_io_done;
		}
		rc = spdk_bdev_read_blocks(desc, ch, io->payload, offset_blocks, num_blocks, read_cb, io);
		break;
	case UBLK_IO_OP_WRITE:
		rc = spdk_bdev_write_blocks(desc, ch, io->payload, offset_blocks, num_blocks, ublk_io_done, io);
		break;
	case UBLK_IO_OP_FLUSH:
		/* [한국어] FLUSH 는 범위 무관 — bdev 전체. ublk_drv 가 부분 flush 보내지 않음. */
		rc = spdk_bdev_flush_blocks(desc, ch, 0, spdk_bdev_get_num_blocks(ublk->bdev), ublk_io_done, io);
		break;
	case UBLK_IO_OP_DISCARD:
		rc = spdk_bdev_unmap_blocks(desc, ch, offset_blocks, num_blocks, ublk_io_done, io);
		break;
	case UBLK_IO_OP_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(desc, ch, offset_blocks, num_blocks, ublk_io_done, io);
		break;
	default:
		rc = -1;
	}

	if (rc < 0) {
		if (rc == -ENOMEM) {
			/* [한국어] bdev_io 풀 고갈 — wait queue 등록 → resubmit 콜백. */
			SPDK_INFOLOG(ublk, "No memory, start to queue io.\n");
			ublk_queue_io(io);
		} else {
			/* [한국어] 그 외 에러 — 즉시 에러 응답 chain (synthetic ublk_io_done). */
			SPDK_ERRLOG("ublk io failed in ublk_queue_io, rc=%d, ublk_op=%u\n", rc, ublk_op);
			ublk_io_done(NULL, false, io);
		}
	}
}

/*
 * [한국어]
 * read_get_buffer_done - READ IO 의 buffer 확보 완료 → bdev read 발행 콜백.
 * @io: 대상 IO. 실행 컨텍스트: 큐 poll_group thread.
 * 호출 체인: ublk_io_get_buffer(READ) → [본 함수] → _ublk_submit_bdev_io (spdk_bdev_read_blocks).
 */
static void
read_get_buffer_done(struct ublk_io *io)
{
	_ublk_submit_bdev_io(io->q, io);   /* [한국어] payload 준비됨 → bdev read 발행. */
}

/*
 * [한국어]
 * user_copy_write_get_buffer_done - USER_COPY WRITE IO 의 buffer 확보 완료 콜백.
 * @io: 대상 IO. 실행 컨텍스트: 큐 poll_group thread.
 *
 * USER_COPY WRITE 는 (1) buffer 확보 → (2) 커널 user buffer 를 payload 로 read(본 콜백) →
 * (3) bdev write 순서. 여기서는 (2) 단계인 user_copy read SQE 를 발행한다.
 * 호출 체인: ublk_io_get_buffer(WRITE,user_copy) → [본 함수] → ublk_queue_user_copy(true).
 */
static void
user_copy_write_get_buffer_done(struct ublk_io *io)
{
	ublk_queue_user_copy(io, true);   /* [한국어] 커널 buffer → payload read SQE 발행. */
}

/*
 * [한국어]
 * ublk_submit_bdev_io - 커널 IO 요청을 opcode 별로 분류해 buffer 확보 경로로 dispatch.
 *
 * @q: ublk queue.
 * @io: 새로 수신된 IO 요청 (iod 채워짐).
 *
 * ublk_io_recv 가 UBLK_IO_RES_OK(새 요청)를 받으면 호출하는 진입점. io->result 에 데이터
 * 바이트 수를 미리 채우고(성공 시 커널에 보고할 전송량) opcode 로 분기한다:
 *   - READ: 항상 buffer 확보 필요 → read_get_buffer_done 경로.
 *   - WRITE: user_copy 모드면 buffer 확보 후 커널→payload read(user_copy_write_get_buffer_done),
 *            아니면 payload 가 이미 커널 zero-copy 영역이라 바로 _ublk_submit_bdev_io.
 *   - 그 외(FLUSH/DISCARD/WRITE_ZEROES): 데이터 buffer 불필요 → 바로 발행.
 * 실행 컨텍스트: 큐 소유 poll_group thread.
 *
 * 호출 체인:
 *   ublk_io_recv(RES_OK) → [본 함수] → ublk_io_get_buffer / _ublk_submit_bdev_io
 */
static void
ublk_submit_bdev_io(struct ublk_queue *q, struct ublk_io *io)
{
	struct spdk_iobuf_channel *iobuf_ch = &q->poll_group->iobuf_ch;
	const struct ublksrv_io_desc *iod = io->iod;
	uint8_t ublk_op;

	io->result = iod->nr_sectors * (1ULL << LINUX_SECTOR_SHIFT);  /* [한국어] 성공 시 보고할 전송 바이트. */
	ublk_op = ublksrv_get_op(iod);   /* [한국어] iod 에서 IO opcode 추출. */
	switch (ublk_op) {
	case UBLK_IO_OP_READ:
		ublk_io_get_buffer(io, iobuf_ch, read_get_buffer_done);   /* [한국어] READ 는 항상 buffer 필요. */
		break;
	case UBLK_IO_OP_WRITE:
		if (g_ublk_tgt.user_copy) {
			/* [한국어] user_copy WRITE — buffer 확보 후 커널 데이터 가져옴. */
			ublk_io_get_buffer(io, iobuf_ch, user_copy_write_get_buffer_done);
		} else {
			_ublk_submit_bdev_io(q, io);   /* [한국어] zero-copy — payload 가 이미 데이터. */
		}
		break;
	default:
		_ublk_submit_bdev_io(q, io);   /* [한국어] FLUSH/DISCARD/WRITE_ZEROES — 데이터 무관. */
		break;
	}
}

/*
 * [한국어]
 * ublksrv_queue_io_cmd - ublk IO uring_cmd(FETCH/NEED_GET_DATA/COMMIT_AND_FETCH) SQE 를 빌드.
 *
 * @q: ublk queue.
 * @io: 대상 IO (cmd_op 이 발행할 명령 결정).
 * @tag: IO 슬롯 인덱스 (= io 의 q->ios[] 위치).
 *
 * data ring 의 한 SQE 에 IORING_OP_URING_CMD + ublksrv_io_cmd 인라인 payload 를 채운다.
 * io->cmd_op 가 FETCH_REQ(다음 요청 대기) / NEED_GET_DATA(WRITE 데이터 요청) / COMMIT_AND_FETCH
 * (결과 commit + 다음 fetch) 중 하나여야 함(assert). COMMIT_AND_FETCH 면 cmd->result 에 IO
 * 결과를 싣는다. addr 은 user_copy 모드면 0(커널이 buffer 관리), 아니면 payload 물리 위치.
 * fd 는 fixed file index 0(등록된 cdev_fd), flags=IOSQE_FIXED_FILE. user_data 에 tag+op 인코딩.
 * 마지막에 io->cmd_op=0 으로 클리어 — 같은 SQE 를 중복 제출하지 않도록.
 * 실행 컨텍스트: 큐 소유 poll_group thread.
 *
 * 호출 체인:
 *   ublk_io_xmit → [본 함수] → io_uring SQE 채움 (제출은 호출자 io_uring_submit)
 */
static inline void
ublksrv_queue_io_cmd(struct ublk_queue *q,
		     struct ublk_io *io, unsigned tag)
{
	struct ublksrv_io_cmd *cmd;
	struct io_uring_sqe *sqe;
	unsigned int cmd_op = 0;
	uint64_t user_data;

	/* each io should have operation of fetching or committing */
	/* [한국어] 모든 IO 는 fetch/commit/need_data 중 하나의 명령이어야 함 (상태 검증). */
	assert((io->cmd_op == UBLK_IO_FETCH_REQ) || (io->cmd_op == UBLK_IO_NEED_GET_DATA) ||
	       (io->cmd_op == UBLK_IO_COMMIT_AND_FETCH_REQ));
	cmd_op = io->cmd_op;

	sqe = io_uring_get_sqe(&q->ring);   /* [한국어] data ring 에서 SQE 1칸. */
	assert(sqe);   /* [한국어] depth 와 IO 수 일치 → 항상 확보. */

	cmd = (struct ublksrv_io_cmd *)ublk_get_sqe_cmd(sqe);   /* [한국어] SQE 인라인 payload 위치. */
	if (cmd_op == UBLK_IO_COMMIT_AND_FETCH_REQ) {
		cmd->result = io->result;   /* [한국어] commit 단계 — IO 결과를 커널에 전달. */
	}

	/* These fields should be written once, never change */
	ublk_set_sqe_cmd_op(sqe, cmd_op);   /* [한국어] opcode 인코딩 → sqe->off. */
	/* dev->cdev_fd */
	sqe->fd		= 0;                /* [한국어] fixed file index 0 = 등록된 cdev_fd. */
	sqe->opcode	= IORING_OP_URING_CMD;  /* [한국어] ublk 명령 전달 방식. */
	sqe->flags	= IOSQE_FIXED_FILE;   /* [한국어] fd 대신 fixed index 사용 (fdget 절약). */
	sqe->rw_flags	= 0;
	cmd->tag	= tag;                /* [한국어] 커널이 IO 슬롯 식별에 사용. */
	cmd->addr	= g_ublk_tgt.user_copy ? 0 : (__u64)(uintptr_t)(io->payload);  /* [한국어] user_copy=0, zero-copy=payload. */
	cmd->q_id	= q->q_id;            /* [한국어] 큐 식별자. */

	user_data = build_user_data(tag, cmd_op);   /* [한국어] CQE 역추적용 tag+op. */
	io_uring_sqe_set_data64(sqe, user_data);

	io->cmd_op = 0;   /* [한국어] 명령 소비 표시 — 같은 SQE 중복 제출 방지. */

	SPDK_DEBUGLOG(ublk_io, "(qid %d tag %u cmd_op %u) iof %x stopping %d\n",
		      q->q_id, tag, cmd_op,
		      io->cmd_op, q->is_stopping);
}

/*
 * [한국어]
 * ublk_io_xmit - ★ 완료된 IO 들의 응답 commit + 다음 IO fetch SQE 일괄 발행.
 *
 * @q: ublk queue.
 * @return: io_uring_submit 결과 (양수 = 제출한 SQE 수, 음수 = errno).
 *
 * UBLK_IO_COMMIT_AND_FETCH_REQ 패턴: ublk_drv 의 통합 명령 = "직전 IO 결과 응답 + 다음
 * IO 요청 미리 fetch". 응답과 fetch 를 하나의 SQE 로 묶어 syscall 횟수 절반.
 *
 * 단계:
 *   1. completed_io_list 비어 있으면 즉시 return (no work).
 *   2. 각 completed_io 마다:
 *      - 미리 list 에서 remove (scan-build use-after-free warning 회피 트릭).
 *      - user_copy 아니면 ublksrv_queue_io_cmd 로 COMMIT_AND_FETCH SQE 채움.
 *      - need_data=false 면 buffer_free_list 에 적재 → submit 후 일괄 해제.
 *   3. io_uring_submit — 누적 SQE 들 한 번에 커널에 제출.
 *   4. cmd_inflight += count (커널 처리 대기 카운트).
 *   5. buffer free pass: READ IO 의 경우 ublk_drv 가 submit context 에서 copy 완료
 *      (SQPOLL 미사용 → kthread 비동기 copy 없음) → 즉시 iobuf 반환 가능.
 *
 * Buffer free 시점 트릭: 정상 설계라면 별도 COMMIT_REQ ack 후 free 가 맞지만, ublk_drv
 * 가 COMMIT_AND_FETCH 통합 명령만 제공 → SQPOLL 미사용 가정 하에 submit 직후 free 안전.
 * 향후 async copy 지원하려면 별도 ack callback 필요.
 *
 * scan-build 회피 트릭: 항목 처리 전 list 에서 remove → 다음 iter 에 같은 슬롯 다시 보지
 * 않음. static analyzer 의 use-after-free 오탐 회피 (실제론 io 객체 free 하지 않음).
 *
 * 호출 체인:
 *   ublk_poll → [본 함수] → ublksrv_queue_io_cmd × N → io_uring_submit → 커널 ublk_drv
 *     → 사용자 syscall unblock + 다음 IO 요청 prepare
 */
static int
ublk_io_xmit(struct ublk_queue *q)
{
	TAILQ_HEAD(, ublk_io) buffer_free_list;
	struct spdk_iobuf_channel *iobuf_ch;
	int rc = 0, count = 0;
	struct ublk_io *io;

	if (TAILQ_EMPTY(&q->completed_io_list)) {
		return 0;
	}

	TAILQ_INIT(&buffer_free_list);
	while (!TAILQ_EMPTY(&q->completed_io_list)) {
		io = TAILQ_FIRST(&q->completed_io_list);
		assert(io != NULL);
		/*
		 * Remove IO from list now assuming it will be completed. It will be inserted
		 * back to the head if it cannot be completed. This approach is specifically
		 * taken to work around a scan-build use-after-free mischaracterization.
		 */
		/* [한국어] scan-build 오탐 회피 — 미리 remove. 실제 free 는 io_uring submit 후 buffer free. */
		TAILQ_REMOVE(&q->completed_io_list, io, tailq);
		if (!io->user_copy) {
			/* [한국어] need_data=true 면 NEED_GET_DATA 응답 — buffer 아직 필요. */
			if (!io->need_data) {
				TAILQ_INSERT_TAIL(&buffer_free_list, io, tailq);
			}
			/* [한국어] COMMIT_AND_FETCH SQE 채우기 — io->result + status + 다음 IO fetch. */
			ublksrv_queue_io_cmd(q, io, io->tag);
		}
		count++;
	}

	q->cmd_inflight += count;
	/* [한국어] 누적 SQE 들 일괄 제출 — single syscall 로 batching. */
	rc = io_uring_submit(&q->ring);
	if (rc != count) {
		SPDK_ERRLOG("could not submit all commands\n");
		assert(false);
	}

	/* Note: for READ io, ublk will always copy the data out of
	 * the buffers in the io_uring_submit context.  Since we
	 * are not using SQPOLL for IO rings, we can safely free
	 * those IO buffers here.  This design doesn't seem ideal,
	 * but it's what's possible since there is no discrete
	 * COMMIT_REQ operation.  That will need to change in the
	 * future should we ever want to support async copy
	 * operations.
	 */
	/* [한국어] iobuf 일괄 해제 — SQPOLL 미사용 가정 하에 submit 직후 안전. */
	iobuf_ch = &q->poll_group->iobuf_ch;
	while (!TAILQ_EMPTY(&buffer_free_list)) {
		io = TAILQ_FIRST(&buffer_free_list);
		TAILQ_REMOVE(&buffer_free_list, io, tailq);
		ublk_io_put_buffer(io, iobuf_ch);
	}
	return rc;
}

/*
 * [한국어]
 * write_get_buffer_done - zero-copy WRITE 의 buffer 확보 완료 → NEED_GET_DATA 응답 준비 콜백.
 *
 * @io: 대상 WRITE IO.
 *
 * zero-copy(비 user_copy) WRITE 경로: 커널이 RES_NEED_GET_DATA 를 보내면 SPDK 가 payload
 * buffer 를 확보한 뒤(본 콜백), UBLK_IO_NEED_GET_DATA 응답 SQE 를 보내 ublk_drv 가 사용자
 * 데이터를 이 payload 로 복사하게 한다. need_data=true 로 마킹하고 completed_io_list 로 옮겨
 * 다음 ublk_io_xmit 가 NEED_GET_DATA SQE 를 발행하게 한다(result=0: 아직 IO 미완료).
 * 실행 컨텍스트: 큐 소유 poll_group thread (iobuf 콜백).
 *
 * 호출 체인:
 *   ublk_io_recv(RES_NEED_GET_DATA) → ublk_io_get_buffer → [본 함수] → 다음 xmit 가 NEED_GET_DATA SQE
 */
static void
write_get_buffer_done(struct ublk_io *io)
{
	io->need_data = true;                     /* [한국어] 데이터 GET 단계임을 표시. */
	io->cmd_op = UBLK_IO_NEED_GET_DATA;       /* [한국어] 다음 xmit 가 보낼 명령 = NEED_GET_DATA. */
	io->result = 0;                           /* [한국어] 아직 IO 미완료 (commit 아님). */

	TAILQ_REMOVE(&io->q->inflight_io_list, io, tailq);   /* [한국어] inflight → completed 이동. */
	TAILQ_INSERT_TAIL(&io->q->completed_io_list, io, tailq);
}

/*
 * [한국어]
 * ublk_io_recv - ★ io_uring CQE 폴링 → 커널 IO 요청 수신 → bdev 발행 dispatcher.
 *
 * @q: ublk queue.
 * @return: 처리한 CQE 수 (UBLK_QUEUE_REQUEST=32 한계).
 *
 * io_uring_for_each_cqe 로 모든 ready CQE 일괄 순회. user_data 64-bit 에 tag + cmd_op
 * 인코딩 — io->user_copy 플래그로 두 경로 분기:
 *
 * 경로 A — 일반 COMMIT_AND_FETCH 응답 (!user_copy):
 *   - UBLK_IO_RES_OK: 새 IO 요청 수신 → ublk_submit_bdev_io → bdev I/O 발행.
 *   - UBLK_IO_RES_NEED_GET_DATA: WRITE 의 zero-copy 경로 — buffer 확보 후 NEED_GET_DATA
 *     응답 SQE 보내 ublk_drv 가 데이터 복사하게 함.
 *   - UBLK_IO_RES_ABORT: ublk_drv 종료 신호 → is_stopping=true.
 *   - 그 외 음수: 에러 로그 + inflight 에서 제거.
 *
 * 경로 B — user_copy SQE 완료:
 *   - cqe->res != io->result: EIO 처리.
 *   - READ: bdev_io 이미 free 됨 → io_done 직접.
 *   - WRITE: 데이터 복사 끝남 → bdev write 발행 (_ublk_submit_bdev_io).
 *
 * UBLK_QUEUE_REQUEST=32 batching 한계: 한 번에 너무 많이 처리하면 다른 queue starvation.
 * 32 처리 후 break → 다음 ublk_poll iter 에서 재개.
 *
 * io_uring_cq_advance: 처리한 CQE 수만큼 ring head 갱신 — 커널이 새 CQE 쓸 공간 확보.
 *
 * fetch 결정 로직: ABORT 가 아니고 stopping 아닐 때만 다음 fetch → stopping 시 모든 IO
 * cmd_op 클리어 (xmit 가 commit 만 보내고 fetch 안 보냄).
 *
 * 호출 체인:
 *   ublk_poll → [본 함수] → io_uring_for_each_cqe → ublk_submit_bdev_io
 *     → spdk_bdev_readv 등 → 완료 후 ublk_io_done → 다음 iter ublk_io_xmit 가 응답
 */
static int
ublk_io_recv(struct ublk_queue *q)
{
	struct io_uring_cqe *cqe;
	unsigned head, tag;
	int fetch, count = 0;
	struct ublk_io *io;
	struct spdk_iobuf_channel *iobuf_ch;

	if (q->cmd_inflight == 0) {
		return 0;
	}

	iobuf_ch = &q->poll_group->iobuf_ch;
	io_uring_for_each_cqe(&q->ring, head, cqe) {
		/* [한국어] user_data 디코딩 — xmit 시점에 tag + cmd_op 인코딩한 값. */
		tag = user_data_to_tag(cqe->user_data);
		io = &q->ios[tag];

		SPDK_DEBUGLOG(ublk_io, "res %d qid %d tag %u, user copy %u, cmd_op %u\n",
			      cqe->res, q->q_id, tag, io->user_copy, user_data_to_op(cqe->user_data));

		q->cmd_inflight--;
		TAILQ_INSERT_TAIL(&q->inflight_io_list, io, tailq);

		if (!io->user_copy) {
			/* [한국어] fetch 결정 — ABORT 아니고 stopping 아닐 때만 다음 IO 요청 수신. */
			fetch = (cqe->res != UBLK_IO_RES_ABORT) && !q->is_stopping;
			if (!fetch) {
				q->is_stopping = true;
				if (io->cmd_op == UBLK_IO_FETCH_REQ) {
					io->cmd_op = 0;
				}
			}

			if (cqe->res == UBLK_IO_RES_OK) {
				/* [한국어] 새 IO 요청 도착 — bdev 발행 dispatch. */
				ublk_submit_bdev_io(q, io);
			} else if (cqe->res == UBLK_IO_RES_NEED_GET_DATA) {
				/* [한국어] WRITE zero-copy 경로 — buffer 확보 후 ublk_drv 가 copy 트리거. */
				ublk_io_get_buffer(io, iobuf_ch, write_get_buffer_done);
			} else {
				/* [한국어] 에러 또는 ABORT. ABORT 는 정상 종료 신호라 로그 skip. */
				if (cqe->res != UBLK_IO_RES_ABORT) {
					SPDK_ERRLOG("ublk received error io: res %d qid %d tag %u cmd_op %u\n",
						    cqe->res, q->q_id, tag, user_data_to_op(cqe->user_data));
				}
				TAILQ_REMOVE(&q->inflight_io_list, io, tailq);
			}
		} else {

			/* clear `user_copy` for next use of this IO structure */
			io->user_copy = false;

			/* [한국어] user_copy 경로는 READ/WRITE 만 — 다른 opcode 는 user_copy 안 씀. */
			assert((ublksrv_get_op(io->iod) == UBLK_IO_OP_READ) ||
			       (ublksrv_get_op(io->iod) == UBLK_IO_OP_WRITE));
			if (cqe->res != io->result) {
				/* EIO */
				ublk_io_done(NULL, false, io);
			} else {
				if (ublksrv_get_op(io->iod) == UBLK_IO_OP_READ) {
					/* bdev_io is already freed in first READ cycle */
					/* [한국어] READ: 첫 cycle 에서 bdev read + user_copy_read_done → user copy SQE.
					 * 이 cycle 은 copy 완료 알림 → io_done 직접 (bdev_io 이미 free). */
					ublk_io_done(NULL, true, io);
				} else {
					/* [한국어] WRITE: data 가 user 에서 payload 로 copy 완료 → 이제 bdev write 발행. */
					_ublk_submit_bdev_io(q, io);
				}
			}
		}
		count += 1;
		/* [한국어] batching 한계 — 다른 queue starvation 회피. 다음 iter 에서 재개. */
		if (count == UBLK_QUEUE_REQUEST) {
			break;
		}
	}
	/* [한국어] 처리한 CQE 수만큼 ring head advance — 커널이 새 CQE 쓸 공간 확보. */
	io_uring_cq_advance(&q->ring, count);

	return count;
}

/*
 * [한국어]
 * ublk_poll - ★ ublk transport hot path ★ — poll_group 의 모든 queue 의 io_uring CQE 폴링.
 *
 * @arg: ublk_poll_group.
 * @return: SPDK_POLLER_BUSY (작업 처리함) 또는 SPDK_POLLER_IDLE (없음).
 *
 * 매 reactor iteration 마다 호출. ublk_drv ↔ SPDK 간 io_uring 큐 양방향 처리:
 *   1. ublk_io_xmit: 완료된 SPDK bdev I/O 응답을 io_uring SQE 로 보내고 ring submit.
 *      (UBLK_IO_COMMIT_AND_FETCH_REQ = 응답 + 다음 IO 요청 fetch 통합 명령)
 *   2. ublk_io_recv: io_uring CQE 폴링 → 커널이 보낸 IO 요청 수신 → SPDK bdev 발행.
 *   3. is_stopping 인 queue 면 try_close — drain 후 close 진행.
 *
 * _SAFE iteration: ublk_try_close_queue 내부에서 queue 가 list 에서 제거될 수 있어 next 미리 캐싱.
 *
 * xmit 우선 호출 이유: 완료 응답을 먼저 보내면 ublk_drv 가 응답 기다리던 사용자 syscall 을
 * unblock — latency 감소. recv 가 먼저면 새 요청 처리 후 응답 → 평균 latency 증가.
 *
 * polled-mode: 인터럽트 없이 매 reactor iter 마다 호출. CPU 사용량 ↑ but latency ↓.
 *
 * 호출 체인:
 *   SPDK reactor → [본 함수] → ublk_io_xmit (bdev 완료 → CQE commit) +
 *                              ublk_io_recv (io_uring CQE → bdev 발행)
 */
static int
ublk_poll(void *arg)
{
	struct ublk_poll_group *poll_group = arg;
	struct ublk_queue *q, *q_tmp;
	int sent, received, count = 0;

	/* [한국어] _SAFE: try_close_queue 가 list 에서 q 제거 가능 → next 미리. */
	TAILQ_FOREACH_SAFE(q, &poll_group->queue_list, tailq, q_tmp) {
		/* [한국어] xmit 먼저 — 완료 응답 우선 (사용자 syscall unblock latency 감소). */
		sent = ublk_io_xmit(q);
		received = ublk_io_recv(q);
		if (spdk_unlikely(q->is_stopping)) {
			ublk_try_close_queue(q);
		}
		count += sent + received;
	}
	if (count > 0) {
		return SPDK_POLLER_BUSY;
	} else {
		return SPDK_POLLER_IDLE;
	}
}

/*
 * [한국어]
 * ublk_bdev_hot_remove - 백엔드 bdev 가 hot-remove 되면 ublk 디바이스를 닫는다.
 * @ublk: 영향받는 디바이스. 실행 컨텍스트: app_thread (bdev event).
 * 호출 체인: ublk_bdev_event_cb(REMOVE) → [본 함수] → ublk_close_dev(STOP_DEV).
 */
static void
ublk_bdev_hot_remove(struct spdk_ublk_dev *ublk)
{
	ublk_close_dev(ublk);   /* [한국어] bdev 가 사라졌으니 디바이스도 닫음. */
}

/*
 * [한국어]
 * ublk_bdev_event_cb - bdev 레이어 이벤트 콜백 (open_ext 등록).
 *
 * @type: 이벤트 종류 (SPDK_BDEV_EVENT_REMOVE 등).
 * @bdev: 이벤트 발생 bdev.
 * @event_ctx: 등록 시 넘긴 컨텍스트 = spdk_ublk_dev.
 *
 * spdk_bdev_open_ext 에 등록되어 bdev 이벤트를 받는다. 현재는 REMOVE(hot-unplug)만 처리 —
 * 디바이스를 닫는다. 다른 이벤트(RESIZE 등)는 NOTICELOG 로 무시.
 * 실행 컨텍스트: app_thread (bdev event 디스패치).
 *
 * 호출 체인:
 *   bdev layer 이벤트 → [본 함수] → ublk_bdev_hot_remove
 */
static void
ublk_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		   void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		ublk_bdev_hot_remove(event_ctx);   /* [한국어] bdev 제거 → 디바이스 닫기. */
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);   /* [한국어] 미지원 이벤트 무시. */
		break;
	}
}

/*
 * [한국어]
 * ublk_dev_init_io_cmds - data ring 의 모든 SQE 슬롯에 불변 공통 필드를 1회 선기록.
 *
 * @r: data io_uring 핸들.
 * @q_depth: SQE 슬롯 수 (= 큐 깊이).
 *
 * 매 IO 마다 SQE 의 flags/rw_flags/ioprio/off 를 다시 채우는 비용을 줄이기 위해, 큐 초기화
 * 시점에 모든 슬롯의 변하지 않는 필드(IOSQE_FIXED_FILE 등)를 미리 설정한다. SQE128 모드라
 * 인덱스 접근은 ublk_uring_get_sqe(×2 변환)로 한다.
 * 실행 컨텍스트: 큐 소유 poll_group thread (ublk_dev_queue_init 내부).
 *
 * 호출 체인:
 *   ublk_dev_queue_init → [본 함수] → ublk_uring_get_sqe
 */
static void
ublk_dev_init_io_cmds(struct io_uring *r, uint32_t q_depth)
{
	struct io_uring_sqe *sqe;
	uint32_t i;

	/* [한국어] 모든 슬롯 순회하며 불변 필드 선기록. */
	for (i = 0; i < q_depth; i++) {
		sqe = ublk_uring_get_sqe(r, i);   /* [한국어] SQE128 인덱스(×2) 접근. */

		/* These fields should be written once, never change */
		sqe->flags = IOSQE_FIXED_FILE;   /* [한국어] fixed file index 사용 고정. */
		sqe->rw_flags = 0;
		sqe->ioprio = 0;
		sqe->off = 0;
	}
}

/*
 * [한국어]
 * ublk_dev_queue_init - ★ per-queue io_uring + io_cmd_buf mmap + IO 슬롯 초기화.
 *
 * @q: ublk_queue (q_id, q_depth, dev 채워진 상태).
 * @return: 0 성공, 음수 errno.
 *
 * ublk 디바이스 1개에 N 개 queue, 각 queue 마다 독립 io_uring + io_cmd_buf 영역.
 * io_cmd_buf 는 ublk_drv 가 mmap 으로 공유하는 ublksrv_io_desc 배열 — 커널이 새 IO
 * 요청 마다 이 영역에 desc 채우고, SPDK 는 read-only 로 desc 읽음 (zero-copy 요청 전달).
 *
 * 단계:
 *   1. **io_cmd_buf mmap**: PROT_READ + MAP_SHARED + MAP_POPULATE.
 *      offset = UBLKSRV_CMD_BUF_OFFSET + q_id × (MAX_QUEUE_DEPTH × sizeof(io_desc))
 *      → ublk_drv 가 미리 정의한 큐별 offset.
 *      MAP_POPULATE: page fault 최소화 위해 미리 페이지 가져오기.
 *   2. **IO 슬롯 초기화**: q_depth 만큼 ublk_io 객체 — cmd_op=FETCH_REQ (첫 IO 요청 대기 명령),
 *      iod 포인터 = io_cmd_buf[i].
 *   3. **io_uring setup**: ublk_setup_ring 가 SQE128 plagin (UBLK 가 128B SQE 사용 — 일반
 *      io_uring 의 64B 보다 큼, ublk_drv 명령 페이로드 수용).
 *   4. **fixed file register**: cdev_fd 를 io_uring 의 fixed file slot 0 에 등록 →
 *      이후 SQE 가 fd 대신 fd index 0 사용 (kernel 측 fdget 오버헤드 절약).
 *   5. **init_io_cmds**: 각 SQE 의 불변 필드 (flags=FIXED_FILE 등) 미리 채움 — 매 IO 마다
 *      재설정 비용 절감.
 *
 * io_uring SQE128: io_uring_setup IORING_SETUP_SQE128 플래그 — SQE 크기를 128B 로 확장.
 * ublk_drv 의 ublksrv_io_cmd 페이로드가 standard SQE 의 24B 패딩 영역에 안 들어가므로 필요.
 *
 * 에러 정리: 각 단계 실패 시 이전 단계 자원 (mmap/ring/files) 명시적 정리 후 -errno.
 *
 * 호출 체인:
 *   ublk_start_dev → [본 함수] (per-queue) → io_uring + io_cmd_buf 준비 →
 *     이후 ublk_dev_queue_io_init 가 첫 FETCH SQE 들 submit
 */
static int
ublk_dev_queue_init(struct ublk_queue *q)
{
	int rc = 0, cmd_buf_size;
	uint32_t j;
	struct spdk_ublk_dev *ublk = q->dev;
	unsigned long off;

	cmd_buf_size = ublk_queue_cmd_buf_sz(q->q_depth);
	/* [한국어] queue 별 io_desc 배열의 mmap offset — ublk_drv 가 미리 정한 layout. */
	off = UBLKSRV_CMD_BUF_OFFSET +
	      q->q_id * (UBLK_MAX_QUEUE_DEPTH * sizeof(struct ublksrv_io_desc));
	/* [한국어] PROT_READ — SPDK 는 io_desc 만 읽음 (커널이 write). MAP_POPULATE 로 page fault 회피. */
	q->io_cmd_buf = (struct ublksrv_io_desc *)mmap(0, cmd_buf_size, PROT_READ,
			MAP_SHARED | MAP_POPULATE, ublk->cdev_fd, off);
	if (q->io_cmd_buf == MAP_FAILED) {
		q->io_cmd_buf = NULL;
		rc = -errno;
		SPDK_ERRLOG("Failed at mmap: %s\n", spdk_strerror(-rc));
		return rc;
	}

	/* [한국어] 모든 IO 슬롯 초기화 — cmd_op=FETCH_REQ (첫 명령은 "IO 요청 fetch"). */
	for (j = 0; j < q->q_depth; j++) {
		q->ios[j].cmd_op = UBLK_IO_FETCH_REQ;
		q->ios[j].iod = &q->io_cmd_buf[j];
	}

	/* [한국어] SQE128 플래그 — ublk_drv 의 io_cmd 페이로드가 standard 64B SQE 안 들어감. */
	rc = ublk_setup_ring(q->q_depth, &q->ring, IORING_SETUP_SQE128);
	if (rc < 0) {
		SPDK_ERRLOG("Failed at setup uring: %s\n", spdk_strerror(-rc));
		munmap(q->io_cmd_buf, ublk_queue_cmd_buf_sz(q->q_depth));
		q->io_cmd_buf = NULL;
		return rc;
	}

	/* [한국어] cdev_fd 를 fixed file slot 0 등록 — SQE 가 fd_index 사용 (fdget 비용 절감). */
	rc = io_uring_register_files(&q->ring, &ublk->cdev_fd, 1);
	if (rc != 0) {
		SPDK_ERRLOG("Failed at uring register files: %s\n", spdk_strerror(-rc));
		io_uring_queue_exit(&q->ring);
		q->ring.ring_fd = -1;
		munmap(q->io_cmd_buf, ublk_queue_cmd_buf_sz(q->q_depth));
		q->io_cmd_buf = NULL;
		return rc;
	}

	/* [한국어] SQE 의 불변 필드 미리 채움 — flags=IOSQE_FIXED_FILE 등. 매 IO 마다 재설정 절감. */
	ublk_dev_init_io_cmds(&q->ring, q->q_depth);

	return 0;
}

/*
 * [한국어]
 * ublk_dev_queue_fini - 큐의 io_uring 과 io_cmd_buf mmap 을 해제 (queue_init 역연산).
 *
 * @q: 정리할 큐.
 *
 * ublk_delete_dev 가 디바이스 teardown 시 각 큐에 대해 호출. fixed file 등록 해제, io_uring
 * exit(ring_fd=-1 로 재정리 방지), io_cmd_buf munmap. ring_fd<0 또는 io_cmd_buf==NULL 이면
 * 해당 단계 skip(부분 초기화/이중 호출 방어).
 * 실행 컨텍스트: app_thread (ublk_delete_dev).
 *
 * 호출 체인:
 *   ublk_delete_dev → [본 함수] → io_uring_queue_exit + munmap
 */
static void
ublk_dev_queue_fini(struct ublk_queue *q)
{
	/* [한국어] ring 이 유효할 때만 정리 — 부분 초기화/이중 호출 방어. */
	if (q->ring.ring_fd >= 0) {
		io_uring_unregister_files(&q->ring);   /* [한국어] fixed file(cdev_fd) 등록 해제. */
		io_uring_queue_exit(&q->ring);         /* [한국어] data ring 파괴. */
		q->ring.ring_fd = -1;                  /* [한국어] 재정리 방지 마커. */
	}
	if (q->io_cmd_buf) {   /* [한국어] mmap 된 io_desc 배열 해제. */
		munmap(q->io_cmd_buf, ublk_queue_cmd_buf_sz(q->q_depth));
	}
}

/*
 * [한국어]
 * ublk_dev_queue_io_init - 큐의 모든 슬롯에 FETCH_REQ SQE 를 채워 첫 IO 수신 준비 + 일괄 제출.
 *
 * @q: 초기화할 큐 (bdev_ch 가 설정된 상태).
 *
 * START_DEV 후 또는 recovery 시 큐가 IO 를 받을 수 있도록, q_depth 개 슬롯 전부에 대해
 * UBLK_IO_FETCH_REQ 명령을 채워 커널에 제출한다(이로써 커널이 새 IO 마다 이 슬롯들로 요청을
 * 채워 보냄). 일부 구버전 커널은 NEED_GET_DATA 를 지정해도 buffer 가 posted 되길 요구하므로,
 * 임시 64B 버퍼를 모든 io->payload 에 임시로 물려 제출 후 즉시 NULL 로 되돌리고 free 한다
 * (실제 사용되지 않는 workaround). cmd_inflight 를 q_depth 만큼 올려 커널 처리 대기를 추적.
 * 실행 컨텍스트: **큐 소유 poll_group thread** (ublk_queue_run 내부) — IO 채움은 IO thread 에서.
 *
 * 호출 체인:
 *   ublk_queue_run → [본 함수] → ublksrv_queue_io_cmd × q_depth + io_uring_submit
 */
static void
ublk_dev_queue_io_init(struct ublk_queue *q)
{
	struct ublk_io *io;
	uint32_t i;
	int rc __attribute__((unused));   /* [한국어] release 빌드에서 assert 제거 시 미사용 경고 억제. */
	void *buf;

	/* Some older kernels require a buffer to get posted, even
	 * when NEED_GET_DATA has been specified.  So allocate a
	 * temporary buffer, only for purposes of this workaround.
	 * It never actually gets used, so we will free it immediately
	 * after all of the commands are posted.
	 */
	buf = malloc(64);   /* [한국어] 구버전 커널 workaround용 더미 buffer (실사용 안 됨). */

	assert(q->bdev_ch != NULL);   /* [한국어] IO thread 에서 io_channel 확보된 후여야 함. */

	/* Initialize and submit all io commands to ublk driver */
	/* [한국어] 모든 슬롯에 FETCH_REQ SQE 채움 — 커널이 이 슬롯들로 새 IO 요청 전달. */
	for (i = 0; i < q->q_depth; i++) {
		io = &q->ios[i];
		io->tag = (uint16_t)i;          /* [한국어] 슬롯 인덱스 = tag. */
		io->payload = buf;              /* [한국어] 더미 buffer (workaround). */
		io->bdev_ch = q->bdev_ch;       /* [한국어] 이 큐의 bdev io_channel 캐시. */
		io->bdev_desc = q->dev->bdev_desc;  /* [한국어] bdev open descriptor 캐시. */
		ublksrv_queue_io_cmd(q, io, i); /* [한국어] FETCH_REQ SQE 빌드 (cmd_op=FETCH_REQ). */
	}

	q->cmd_inflight += q->q_depth;   /* [한국어] 커널 처리 대기 카운트. */
	rc = io_uring_submit(&q->ring);  /* [한국어] q_depth 개 FETCH SQE 일괄 제출. */
	assert(rc == (int)q->q_depth);   /* [한국어] 모두 제출되어야 정상. */
	/* [한국어] 더미 buffer 분리 — 이후 실제 IO 가 자기 payload 사용. */
	for (i = 0; i < q->q_depth; i++) {
		io = &q->ios[i];
		io->payload = NULL;
	}
	free(buf);   /* [한국어] workaround buffer 즉시 해제. */
}

/*
 * [한국어]
 * ublk_set_params - 디바이스 파라미터(블록크기/max_sectors/discard)를 커널에 적용 (SET_PARAMS).
 *
 * @ublk: 대상 디바이스 (dev_params 채워진 상태).
 * @return: 0 (SET_PARAMS 제출 성공), 음수 errno.
 *
 * ADD_DEV 완료 후 ublk_ctrl_process_cqe 가 호출하는 비동기 chain 단계. dev_params 를
 * UBLK_CMD_SET_PARAMS 로 커널에 전달한다. 완료되면 ublk_start_dev(false) 로 이어진다.
 * 실행 컨텍스트: ctrl_poller(app_thread).
 *
 * 호출 체인:
 *   ublk_ctrl_process_cqe(ADD_DEV) → [본 함수] → ublk_ctrl_cmd_submit(SET_PARAMS)
 */
static int
ublk_set_params(struct spdk_ublk_dev *ublk)
{
	int rc;

	rc = ublk_ctrl_cmd_submit(ublk, UBLK_CMD_SET_PARAMS);   /* [한국어] dev_params 커널 적용. */
	if (rc < 0) {
		SPDK_ERRLOG("UBLK can't set params for dev %d, rc %s\n", ublk->ublk_id, spdk_strerror(-rc));
	}

	return rc;
}

/*
 * [한국어]
 * ublk_dev_info_init - ADD_DEV 에 보낼 ublksrv_ctrl_dev_info(큐수/깊이/flags)를 구성.
 *
 * @ublk: 대상 디바이스.
 *
 * 디바이스 생성 전(ublk_start_disk)에 호출되어 dev_info 를 채운다. queue_depth/nr_hw_queues/
 * dev_id/max_io_buf_bytes/pid 와 flags 를 설정한다. flags 핵심:
 *   - UBLK_F_URING_CMD_COMP_IN_TASK: uring_cmd 완료를 task 컨텍스트에서 — SPDK polling 모델 적합.
 *   - user_copy 지원 시 UBLK_F_USER_COPY, 아니면 UBLK_F_NEED_GET_DATA(WRITE 2단계 데이터 GET).
 *   - user_recovery 지원 시 USER_RECOVERY + USER_RECOVERY_REISSUE(재시작 후 미완료 IO 재발행).
 * 실행 컨텍스트: app_thread.
 *
 * 호출 체인:
 *   ublk_start_disk → [본 함수]
 */
static void
ublk_dev_info_init(struct spdk_ublk_dev *ublk)
{
	/* [한국어] 커널에 광고할 디바이스 정보 — designated init 로 핵심 필드 채움. */
	struct ublksrv_ctrl_dev_info uinfo = {
		.queue_depth = ublk->queue_depth,     /* [한국어] 큐당 깊이. */
		.nr_hw_queues = ublk->num_queues,     /* [한국어] 큐 개수. */
		.dev_id = ublk->ublk_id,              /* [한국어] /dev/ublkb<N> 의 N. */
		.max_io_buf_bytes = UBLK_IO_MAX_BYTES,  /* [한국어] 단일 IO 최대 바이트. */
		.ublksrv_pid = getpid(),              /* [한국어] owner pid — 죽으면 커널이 cleanup. */
		.flags = UBLK_F_URING_CMD_COMP_IN_TASK,  /* [한국어] 완료를 task 컨텍스트로 (polling 적합). */
	};

	/* [한국어] zero-copy(user_copy) vs 2단계 데이터 GET 선택. */
	if (g_ublk_tgt.user_copy) {
		uinfo.flags |= UBLK_F_USER_COPY;
	} else {
		uinfo.flags |= UBLK_F_NEED_GET_DATA;
	}

	/* [한국어] 재시작 복구 지원 시 RECOVERY + REISSUE(미완료 IO 재발행) 플래그. */
	if (g_ublk_tgt.user_recovery) {
		uinfo.flags |= UBLK_F_USER_RECOVERY;
		uinfo.flags |= UBLK_F_USER_RECOVERY_REISSUE;
	}

	ublk->dev_info = uinfo;   /* [한국어] 구성 완료 → 디바이스에 저장 (ADD_DEV 가 사용). */
}

/*
 * [한국어]
 * ublk_info_param_init - bdev 특성으로부터 ublk dev_params(블록 geometry/discard)를 도출.
 *
 * @ublk: 대상 디바이스 (bdev 열린 상태).
 *
 * SET_PARAMS 로 커널에 보낼 dev_params 를 백엔드 bdev 의 logical/physical block size, optimal
 * IO boundary, num_blocks 로부터 계산한다. ublk 의 *_bs_shift 필드는 log2 표현이라 spdk_u32log2
 * 로 변환. dev_sectors 는 512B sector 환산 총량, max_sectors 는 단일 IO 한도. bdev 가 FLUSH 를
 * 지원하면 VOLATILE_CACHE 속성, UNMAP 을 지원하면 DISCARD 파라미터(정렬/granularity/최대 sector)
 * 추가, WRITE_ZEROES 까지 지원하면 max_write_zeroes_sectors 도 설정.
 * 실행 컨텍스트: app_thread.
 *
 * 호출 체인:
 *   ublk_start_disk / ublk_ctrl_start_recovery → [본 함수]
 */
/* Set ublk device parameters based on bdev */
static void
ublk_info_param_init(struct spdk_ublk_dev *ublk)
{
	struct spdk_bdev *bdev = ublk->bdev;
	uint32_t blk_size = spdk_bdev_get_data_block_size(bdev);   /* [한국어] logical block size (보통 512/4096). */
	uint32_t pblk_size = spdk_bdev_get_physical_block_size(bdev);  /* [한국어] physical block size. */
	uint32_t io_opt_blocks = spdk_bdev_get_optimal_io_boundary(bdev);  /* [한국어] 최적 IO 경계(블록). */
	uint64_t num_blocks = spdk_bdev_get_num_blocks(bdev);      /* [한국어] 총 블록 수. */
	uint8_t sectors_per_block = blk_size >> LINUX_SECTOR_SHIFT;  /* [한국어] block 당 512B sector 수. */
	uint32_t io_min_size = blk_size;                          /* [한국어] 최소 IO = 1블록. */
	uint32_t io_opt_size = spdk_max(io_opt_blocks * blk_size, io_min_size);  /* [한국어] 최적 IO 크기. */

	/* [한국어] ublk basic 파라미터 — *_shift 는 log2 표현이라 spdk_u32log2 변환. */
	struct ublk_params uparams = {
		.types = UBLK_PARAM_TYPE_BASIC,         /* [한국어] basic 파라미터 그룹 포함. */
		.len = sizeof(struct ublk_params),
		.basic = {
			.logical_bs_shift = spdk_u32log2(blk_size),    /* [한국어] log2(logical bs). */
			.physical_bs_shift = spdk_u32log2(pblk_size),  /* [한국어] log2(physical bs). */
			.io_min_shift = spdk_u32log2(io_min_size),     /* [한국어] log2(최소 IO). */
			.io_opt_shift = spdk_u32log2(io_opt_size),     /* [한국어] log2(최적 IO). */
			.dev_sectors = num_blocks * sectors_per_block, /* [한국어] 총 용량(512B sector). */
			.max_sectors = UBLK_IO_MAX_BYTES >> LINUX_SECTOR_SHIFT,  /* [한국어] 단일 IO 한도(sector). */
		}
	};

	/* [한국어] FLUSH 지원 = 휘발성 캐시 존재 → VOLATILE_CACHE 속성. */
	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH)) {
		uparams.basic.attrs = UBLK_ATTR_VOLATILE_CACHE;
	}

	/* [한국어] UNMAP 지원 → DISCARD 파라미터 그룹 추가. */
	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		uparams.types |= UBLK_PARAM_TYPE_DISCARD;
		uparams.discard.discard_alignment = sectors_per_block;   /* [한국어] discard 정렬(sector). */
		/* 32768 sectors for 16MiB */
		uparams.discard.max_discard_sectors = 32768;   /* [한국어] 단일 discard 최대(16MiB). */
		uparams.discard.max_discard_segments = 1;      /* [한국어] 단일 세그먼트만. */
		uparams.discard.discard_granularity = blk_size;  /* [한국어] discard 최소 단위. */
		/* [한국어] WRITE_ZEROES 도 지원하면 max_write_zeroes 설정. */
		if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
			/* 32768 sectors for 16MiB */
			uparams.discard.max_write_zeroes_sectors = 32768;
		}
	}

	ublk->dev_params = uparams;   /* [한국어] 구성 완료 → SET_PARAMS 가 사용. */
}

/*
 * [한국어]
 * _ublk_free_dev - app_thread 컨텍스트에서 ublk_free_dev 를 재개하는 래퍼.
 * @arg: spdk_ublk_dev. 실행 컨텍스트: app_thread.
 *
 * free_buffers 가 큐 thread 에서 buffer 반환을 끝낸 뒤 app_thread 로 send_msg 해 호출 —
 * ublk_free_dev 의 남은 큐 순회를 app_thread 에서 이어가게 한다(g_ublk_devs 등 app_thread 자원).
 * 호출 체인: free_buffers → spdk_thread_send_msg(app_thread, [본 함수]) → ublk_free_dev.
 */
static void
_ublk_free_dev(void *arg)
{
	struct spdk_ublk_dev *ublk = arg;

	ublk_free_dev(ublk);   /* [한국어] app_thread 에서 free 재개. */
}

/*
 * [한국어]
 * free_buffers - 한 큐의 모든 IO buffer 를 그 큐의 iobuf 채널로 반환하고 ios 배열 해제.
 *
 * @arg: 대상 ublk_queue.
 *
 * iobuf buffer 는 빌린 thread(=poll_group thread)의 채널로만 반환할 수 있으므로, ublk_free_dev
 * 가 이 함수를 큐 소유 thread 로 send_msg 한다. 모든 슬롯의 buffer 를 put 한 뒤 ios 배열을
 * free 하고 q->ios=NULL 로 표시한 다음, app_thread 로 _ublk_free_dev 를 send_msg 해 디바이스
 * 해제를 이어가게 한다(다음 큐 처리).
 * 실행 컨텍스트: 큐 소유 poll_group thread.
 *
 * 호출 체인:
 *   ublk_free_dev → spdk_thread_send_msg(poll_group thread, [본 함수])
 *     → spdk_thread_send_msg(app_thread, _ublk_free_dev)
 */
static void
free_buffers(void *arg)
{
	struct ublk_queue *q = arg;
	uint32_t i;

	/* [한국어] 빌린 thread 의 채널로만 반환 가능 → 큐 thread 에서 실행. */
	for (i = 0; i < q->q_depth; i++) {
		ublk_io_put_buffer(&q->ios[i], &q->poll_group->iobuf_ch);
	}
	free(q->ios);   /* [한국어] IO 슬롯 배열 해제. */
	q->ios = NULL;  /* [한국어] 처리 완료 표시 — ublk_free_dev 재개 시 이 큐 skip. */
	/* [한국어] 디바이스 해제 재개는 app_thread 에서. */
	spdk_thread_send_msg(spdk_thread_get_app_thread(), _ublk_free_dev, q->dev);
}

/*
 * [한국어]
 * ublk_free_dev - 디바이스의 모든 자원(큐 buffer/ios, bdev_desc, 메모리)을 단계적으로 해제.
 *
 * @ublk: 해제할 디바이스.
 *
 * DEL_DEV 완료 후 또는 생성 실패 롤백에서 호출. **큐별로 buffer 반환은 그 큐 소유 thread 에서만
 * 가능**하므로, ios 가 살아있고 poll_group 이 있는 큐를 만나면 free_buffers 를 그 thread 로
 * send_msg 하고 **즉시 return**한다. free_buffers 가 끝나면 _ublk_free_dev 로 다시 본 함수가
 * 호출되어 다음 큐를 처리한다(비동기 순회). poll_group 이 없는 큐(아직 thread 미할당)는
 * 바로 ios 를 free. 모든 큐 처리 후 bdev_desc close, 리스트 unregister, 디바이스 free.
 * 실행 컨텍스트: app_thread (큐 buffer 단계만 큐 thread 위임).
 *
 * 호출 체인:
 *   ublk_ctrl_process_cqe(DEL_DEV) / 생성 실패 → [본 함수]
 *     → (각 큐) free_buffers → _ublk_free_dev → [본 함수] 재진입 → spdk_bdev_close + free
 */
static void
ublk_free_dev(struct spdk_ublk_dev *ublk)
{
	struct ublk_queue *q;
	uint32_t q_idx;

	/* [한국어] 큐별 buffer/ios 정리 — buffer 반환은 큐 thread 위임이라 비동기. */
	for (q_idx = 0; q_idx < ublk->num_queues; q_idx++) {
		q = &ublk->queues[q_idx];

		/* The ublk_io of this queue are not initialized. */
		if (q->ios == NULL) {   /* [한국어] 이미 처리됐거나 미초기화 큐 → skip. */
			continue;
		}

		/* We found a queue that has an ios array that may have buffers
		 * that need to be freed.  Send a message to the queue's thread
		 * so it can free the buffers back to that thread's iobuf channel.
		 * When it's done, it will set q->ios to NULL and send a message
		 * back to this function to continue.
		 */
		/* [한국어] thread 할당된 큐 → buffer 반환을 그 thread 에 위임하고 즉시 return (비동기 순회). */
		if (q->poll_group) {
			spdk_thread_send_msg(q->poll_group->ublk_thread, free_buffers, q);
			return;
		} else {
			/* [한국어] thread 미할당 큐(생성 초기 실패) → buffer 없으므로 바로 free. */
			free(q->ios);
			q->ios = NULL;
		}
	}

	/* All of the buffers associated with the queues have been freed, so now
	 * continue with releasing resources for the rest of the ublk device.
	 */
	/* [한국어] 모든 큐 정리 완료 → bdev_desc 닫기. */
	if (ublk->bdev_desc) {
		spdk_bdev_close(ublk->bdev_desc);
		ublk->bdev_desc = NULL;
	}

	ublk_dev_list_unregister(ublk);   /* [한국어] 전역 리스트에서 제거. */
	SPDK_NOTICELOG("ublk dev %d stopped\n", ublk->ublk_id);

	free(ublk);   /* [한국어] 디바이스 구조체 자체 해제. */
}

/*
 * [한국어]
 * ublk_ios_init - 디바이스의 모든 큐와 IO 슬롯 배열을 할당·초기화.
 *
 * @ublk: 대상 디바이스 (num_queues/queue_depth 채워진 상태).
 * @return: 0 성공, -ENOMEM (ios 할당 실패).
 *
 * 각 큐의 completed/inflight 리스트 초기화, q_id/q_depth/dev 설정, ios 배열 calloc, 각 IO 의
 * back-pointer(q) 설정을 수행한다. 어느 큐에서 calloc 이 실패하면 이미 할당한 큐들의 ios 를
 * 정리하고 -ENOMEM. (단, err 경로는 마지막 q 만 free 하는 점에 주의 — 원본 코드 그대로.)
 * 실행 컨텍스트: app_thread.
 *
 * 호출 체인:
 *   ublk_start_disk / ublk_ctrl_start_recovery → [본 함수]
 */
static int
ublk_ios_init(struct spdk_ublk_dev *ublk)
{
	int rc;
	uint32_t i, j;
	struct ublk_queue *q;

	/* [한국어] 모든 큐 순회하며 IO 슬롯 배열 할당. */
	for (i = 0; i < ublk->num_queues; i++) {
		q = &ublk->queues[i];

		TAILQ_INIT(&q->completed_io_list);   /* [한국어] 완료 대기 IO 리스트. */
		TAILQ_INIT(&q->inflight_io_list);    /* [한국어] 처리 중 IO 리스트. */
		q->dev = ublk;                       /* [한국어] 디바이스 back-pointer. */
		q->q_id = i;                         /* [한국어] 큐 인덱스. */
		q->q_depth = ublk->queue_depth;      /* [한국어] 큐 깊이. */
		q->ios = calloc(q->q_depth, sizeof(struct ublk_io));   /* [한국어] IO 슬롯 배열. */
		if (!q->ios) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate queue ios\n");
			goto err;
		}
		/* [한국어] 각 IO 슬롯에 큐 back-pointer 설정. */
		for (j = 0; j < q->q_depth; j++) {
			q->ios[j].q = q;
		}
	}

	return 0;

err:
	/* [한국어] 실패 시 정리 (원본 코드 — 마지막 q 기준). */
	for (i = 0; i < ublk->num_queues; i++) {
		free(q->ios);
		q->ios = NULL;
	}
	return rc;
}

/*
 * [한국어]
 * ublk_queue_recovery_done - recovery 시 한 큐가 online 되면 집계하고, 전부 online 이면 END.
 *
 * @arg: spdk_ublk_dev.
 *
 * ublk_queue_run 이 각 큐 init 후 app_thread 로 send_msg 해 호출. online_num_queues 를 올리고,
 * recovery 모드에서 모든 큐가 다시 fetch SQE 를 건 상태가 되면 UBLK_CMD_END_USER_RECOVERY 를
 * 발행해 복구를 마무리한다(일반 start 경로에서는 is_recovering=false 라 END 미발행).
 * 실행 컨텍스트: app_thread.
 *
 * 호출 체인:
 *   ublk_queue_run → spdk_thread_send_msg(app_thread, [본 함수]) → ublk_ctrl_cmd_submit(END_USER_RECOVERY)
 */
static void
ublk_queue_recovery_done(void *arg)
{
	struct spdk_ublk_dev *ublk = arg;

	ublk->online_num_queues++;   /* [한국어] online 큐 수 +1. */
	/* [한국어] recovery 모드 + 모든 큐 online → 복구 완료 명령. */
	if (ublk->is_recovering && (ublk->online_num_queues == ublk->num_queues)) {
		ublk_ctrl_cmd_submit(ublk, UBLK_CMD_END_USER_RECOVERY);
	}
}

/*
 * [한국어]
 * ublk_queue_run - 큐를 담당 poll_group thread 에서 가동 (io_channel 확보 + FETCH SQE 제출).
 *
 * @arg1: 대상 ublk_queue.
 *
 * ublk_start_dev 가 큐를 poll_group 에 round-robin 배정한 뒤 그 thread 로 send_msg 해 호출.
 * **반드시 배정된 poll_group thread 에서 실행**(assert) — bdev io_channel 은 thread-local 자원이고
 * IO 채움도 IO thread 에서 해야 하기 때문. spdk_bdev_get_io_channel 로 채널 확보, queue_io_init
 * 로 모든 슬롯에 FETCH SQE 제출, poll_group 의 queue_list 에 추가(이제 ublk_poll 이 폴링), 그리고
 * app_thread 로 recovery_done 집계를 통지.
 * 실행 컨텍스트: 큐 소유 poll_group thread.
 *
 * 호출 체인:
 *   ublk_start_dev → spdk_thread_send_msg(poll_group thread, [본 함수])
 *     → spdk_bdev_get_io_channel + ublk_dev_queue_io_init + ublk_queue_recovery_done
 */
static void
ublk_queue_run(void *arg1)
{
	struct ublk_queue	*q = arg1;
	struct spdk_ublk_dev *ublk = q->dev;
	struct ublk_poll_group *poll_group = q->poll_group;

	assert(spdk_get_thread() == poll_group->ublk_thread);   /* [한국어] 배정된 IO thread 에서만 실행. */
	q->bdev_ch = spdk_bdev_get_io_channel(ublk->bdev_desc);  /* [한국어] thread-local bdev io_channel 확보. */
	/* Queues must be filled with IO in the io pthread */
	ublk_dev_queue_io_init(q);   /* [한국어] 모든 슬롯에 FETCH SQE 제출 (IO 수신 시작). */

	TAILQ_INSERT_TAIL(&poll_group->queue_list, q, tailq);   /* [한국어] ublk_poll 이 이 큐 폴링하도록 등록. */
	/* [한국어] online 큐 집계는 app_thread — cross-thread 통지. */
	spdk_thread_send_msg(spdk_thread_get_app_thread(), ublk_queue_recovery_done, ublk);
}

/*
 * [한국어]
 * ublk_start_disk - ★ ublk 디바이스 생성 + /dev/ublkbN 노출 진입점 (RPC ublk_start_disk).
 *
 * @bdev_name: 노출할 SPDK bdev 이름.
 * @ublk_id: ublk 디바이스 ID (= /dev/ublkb<ID>). -1 이면 kernel 자동 할당.
 * @num_queues: 디바이스의 큐 수 (≤ UBLK_DEV_MAX_QUEUES=32).
 * @queue_depth: 큐 당 깊이 (≤ 1024).
 * @ctrl_cb: 비동기 결과 콜백 (ADD_DEV → SET_PARAMS → START_DEV 전체 완료 후 호출).
 * @cb_arg: 콜백 인자.
 * @return: 0 성공 (비동기 진행), 음수 errno (즉시 실패).
 *
 * ublk_drv 와 협조해 bdev 을 /dev/ublkbN block device 로 노출 — Linux 의 mkfs/mount/fio
 * 등 일반 도구가 SPDK bdev 사용 가능. RDMA/TCP target 과 달리 동일 호스트 안에서만 사용.
 *
 * 단계 (본 함수는 비동기 chain 시작만):
 *   1. **사전 검증**: app_thread + target active + ublk_id 중복 X + max_dev 한도.
 *   2. **ublk_dev calloc** + 기본 필드 (ctrlr_cb / cdev_fd=-1).
 *   3. **bdev open**: spdk_bdev_open_ext (write enable, event_cb 등록).
 *   4. **sector_per_block_shift** 계산: bdev block size / 512B sector.
 *   5. **이하 (코드는 더 있음)**: dev_info / dev_params 채우기, num_queues 검증,
 *      poll_group 할당, ublk_ctrl_cmd_submit(UBLK_CMD_ADD_DEV) 호출 → 비동기 chain 시작.
 *
 * 비동기 chain:
 *   ADD_DEV cmd → ctrl_poller → ublk_ctrl_process_cqe(ADD) → SET_PARAMS → ctrl_poller →
 *   (params 적용) → cdev open + queue init × N → START_DEV → ctrlr_cb(success)
 *
 * 호출 체인:
 *   RPC ublk_start_disk → [본 함수] → spdk_bdev_open_ext + ctrl chain 시작
 */
int
ublk_start_disk(const char *bdev_name, uint32_t ublk_id,
		uint32_t num_queues, uint32_t queue_depth,
		ublk_ctrl_cb ctrl_cb, void *cb_arg)
{
	int			rc;
	uint32_t		i;
	struct spdk_bdev	*bdev;
	struct spdk_ublk_dev	*ublk = NULL;
	uint32_t		sector_per_block;

	/* [한국어] app_thread 만 — ctrl_cmd_submit 도 app_thread 가 호출하는 ctrl_ring 사용. */
	assert(spdk_thread_is_app_thread(NULL));

	if (g_ublk_tgt.active == false) {
		SPDK_ERRLOG("NO ublk target exist\n");
		return -ENODEV;
	}

	/* [한국어] ublk_id 중복 검사 — kernel 의 /dev/ublkbN 이름 충돌 회피. */
	ublk = ublk_dev_find_by_id(ublk_id);
	if (ublk != NULL) {
		SPDK_DEBUGLOG(ublk, "ublk id %d is in use.\n", ublk_id);
		return -EBUSY;
	}

	/* [한국어] g_ublks_max 한도 (기본 64) — kernel ublk_drv 의 NR_UBLK_DEVICES. */
	if (g_ublk_tgt.num_ublk_devs >= g_ublks_max) {
		SPDK_DEBUGLOG(ublk, "Reached maximum number of supported devices: %u\n", g_ublks_max);
		return -ENOTSUP;
	}

	ublk = calloc(1, sizeof(*ublk));
	if (ublk == NULL) {
		return -ENOMEM;
	}
	ublk->ctrl_cb = ctrl_cb;
	ublk->cb_arg = cb_arg;
	ublk->cdev_fd = -1;
	ublk->ublk_id = ublk_id;
	UBLK_DEBUGLOG(ublk, "bdev %s num_queues %d queue_depth %d\n",
		      bdev_name, num_queues, queue_depth);

	/* [한국어] bdev open (write enable) + event_cb 등록 → bdev hot-remove 시 자동 close. */
	rc = spdk_bdev_open_ext(bdev_name, true, ublk_bdev_event_cb, ublk, &ublk->bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("could not open bdev %s, error=%d\n", bdev_name, rc);
		free(ublk);
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(ublk->bdev_desc);
	ublk->bdev = bdev;
	sector_per_block = spdk_bdev_get_data_block_size(ublk->bdev) >> LINUX_SECTOR_SHIFT;
	ublk->sector_per_block_shift = spdk_u32log2(sector_per_block);

	ublk->queues_closed = 0;
	ublk->num_queues = num_queues;
	ublk->queue_depth = queue_depth;
	if (ublk->queue_depth > UBLK_DEV_MAX_QUEUE_DEPTH) {
		SPDK_WARNLOG("Set Queue depth %d of UBLK %d to maximum %d\n",
			     ublk->queue_depth, ublk->ublk_id, UBLK_DEV_MAX_QUEUE_DEPTH);
		ublk->queue_depth = UBLK_DEV_MAX_QUEUE_DEPTH;
	}
	if (ublk->num_queues > UBLK_DEV_MAX_QUEUES) {
		SPDK_WARNLOG("Set Queue num %d of UBLK %d to maximum %d\n",
			     ublk->num_queues, ublk->ublk_id, UBLK_DEV_MAX_QUEUES);
		ublk->num_queues = UBLK_DEV_MAX_QUEUES;
	}
	for (i = 0; i < ublk->num_queues; i++) {
		ublk->queues[i].ring.ring_fd = -1;
	}

	ublk_dev_info_init(ublk);
	ublk_info_param_init(ublk);
	rc = ublk_ios_init(ublk);
	if (rc != 0) {
		spdk_bdev_close(ublk->bdev_desc);
		free(ublk);
		return rc;
	}

	SPDK_INFOLOG(ublk, "Enabling kernel access to bdev %s via ublk %d\n",
		     bdev_name, ublk_id);

	/* Add ublk_dev to the end of disk list */
	ublk_dev_list_register(ublk);
	rc = ublk_ctrl_cmd_submit(ublk, UBLK_CMD_ADD_DEV);
	if (rc < 0) {
		SPDK_ERRLOG("UBLK can't add dev %d, rc %s\n", ublk->ublk_id, spdk_strerror(-rc));
		ublk_free_dev(ublk);
	}

	return rc;
}

/*
 * [한국어]
 * ublk_start_dev - cdev 열기 + per-queue io_uring 초기화 + START_DEV + 큐를 poll_group 분배.
 *
 * @ublk: 대상 디바이스.
 * @is_recovering: true=recovery 경로(START_DEV 건너뜀, 커널이 이미 디바이스 보유), false=신규 생성.
 * @return: 0 성공, 음수 errno.
 *
 * 비동기 생성 chain 의 핵심 단계 — SET_PARAMS(또는 START_USER_RECOVERY) 완료 후 ctrl_poller 가
 * 호출. (1) /dev/ublkcN 캐릭터 디바이스를 열어 cdev_fd 확보, (2) 각 큐의 io_uring + io_cmd_buf
 * mmap 초기화(ublk_dev_queue_init), (3) 신규 생성이면 UBLK_CMD_START_DEV 발행(/dev/ublkbN 활성화),
 * (4) 각 큐를 round-robin(g_next_ublk_poll_group)으로 poll_group thread 에 배정하고 ublk_queue_run
 * 을 그 thread 로 send_msg — 부하 분산. recovery 시엔 커널이 이미 디바이스를 갖고 있어 START_DEV
 * 를 건너뛰고 큐만 다시 가동한다.
 * 실행 컨텍스트: ctrl_poller(app_thread). 큐 가동은 각 poll_group thread 로 위임.
 *
 * 호출 체인:
 *   ublk_ctrl_process_cqe(SET_PARAMS/START_USER_RECOVERY) → [본 함수]
 *     → ublk_dev_queue_init × N + START_DEV + spdk_thread_send_msg(ublk_queue_run)
 */
static int
ublk_start_dev(struct spdk_ublk_dev *ublk, bool is_recovering)
{
	int			rc;
	uint32_t		q_id;
	struct spdk_thread	*ublk_thread;
	char			buf[64];

	snprintf(buf, 64, "%s%d", UBLK_BLK_CDEV, ublk->ublk_id);   /* [한국어] "/dev/ublkc<N>" 경로 생성. */
	ublk->cdev_fd = open(buf, O_RDWR);   /* [한국어] cdev 열기 — io_cmd_buf mmap/fixed file 용. */
	if (ublk->cdev_fd < 0) {
		rc = ublk->cdev_fd;
		SPDK_ERRLOG("can't open %s, rc %d\n", buf, rc);
		return rc;
	}

	/* [한국어] 각 큐의 io_uring + io_cmd_buf 초기화 (커널 공유 desc 매핑). */
	for (q_id = 0; q_id < ublk->num_queues; q_id++) {
		rc = ublk_dev_queue_init(&ublk->queues[q_id]);
		if (rc) {
			return rc;
		}
	}

	/* [한국어] 신규 생성만 START_DEV — recovery 는 커널이 이미 디바이스 보유. */
	if (!is_recovering) {
		rc = ublk_ctrl_cmd_submit(ublk, UBLK_CMD_START_DEV);   /* [한국어] /dev/ublkbN 활성화. */
		if (rc < 0) {
			SPDK_ERRLOG("start dev %d failed, rc %s\n", ublk->ublk_id,
				    spdk_strerror(-rc));
			return rc;
		}
	}

	/* Send queue to different spdk_threads for load balance */
	/* [한국어] 큐를 poll_group 들에 round-robin 분배 — 코어 간 부하 분산. */
	for (q_id = 0; q_id < ublk->num_queues; q_id++) {
		ublk->queues[q_id].poll_group = &g_ublk_tgt.poll_groups[g_next_ublk_poll_group];  /* [한국어] poll_group 배정. */
		ublk_thread = g_ublk_tgt.poll_groups[g_next_ublk_poll_group].ublk_thread;
		/* [한국어] 큐 가동은 배정된 IO thread 에서 — io_channel/IO 채움이 thread-local. */
		spdk_thread_send_msg(ublk_thread, ublk_queue_run, &ublk->queues[q_id]);
		g_next_ublk_poll_group++;   /* [한국어] 다음 큐는 다음 poll_group 으로. */
		if (g_next_ublk_poll_group == g_num_ublk_poll_groups) {   /* [한국어] wrap-around. */
			g_next_ublk_poll_group = 0;
		}
	}

	return 0;
}

/*
 * [한국어]
 * ublk_ctrl_start_recovery - GET_DEV_INFO(QUIESCED) 확인 후 복구를 시작 (START_USER_RECOVERY).
 *
 * @ublk: 복구 대상 디바이스.
 * @return: 0 (START_USER_RECOVERY 제출 성공), 음수 errno.
 *
 * ublk_drv 가 재시작/장애 후 디바이스가 QUIESCED 상태일 때, 커널이 광고한 dev_info(큐수/깊이)로
 * SPDK 측 디바이스 구성을 복원한다. num_queues/queue_depth 를 커널 값으로 맞추고 owner pid 갱신,
 * 큐 ring_fd 초기화, dev_params 재구성, ios 재할당 후 is_recovering=true 로 표시하고
 * UBLK_CMD_START_USER_RECOVERY 를 발행한다(완료 시 ublk_start_dev(true) 로 이어짐).
 * 실행 컨텍스트: ctrl_poller(app_thread).
 *
 * 호출 체인:
 *   ublk_ctrl_process_cqe(GET_DEV_INFO, QUIESCED) → [본 함수] → ublk_ctrl_cmd_submit(START_USER_RECOVERY)
 */
static int
ublk_ctrl_start_recovery(struct spdk_ublk_dev *ublk)
{
	int                     rc;
	uint32_t                i;

	ublk->num_queues = ublk->dev_info.nr_hw_queues;   /* [한국어] 커널이 광고한 큐 수로 복원. */
	ublk->queue_depth = ublk->dev_info.queue_depth;   /* [한국어] 커널 큐 깊이로 복원. */
	ublk->dev_info.ublksrv_pid = getpid();            /* [한국어] 새 owner pid (재시작된 프로세스). */

	SPDK_DEBUGLOG(ublk, "Recovering ublk %d, num queues %u, queue depth %u, flags 0x%llx\n",
		      ublk->ublk_id,
		      ublk->num_queues, ublk->queue_depth, ublk->dev_info.flags);

	/* [한국어] 큐 ring_fd 를 -1 로 초기화 (재초기화 준비). */
	for (i = 0; i < ublk->num_queues; i++) {
		ublk->queues[i].ring.ring_fd = -1;
	}

	ublk_info_param_init(ublk);   /* [한국어] bdev 기반 dev_params 재구성. */
	rc = ublk_ios_init(ublk);     /* [한국어] IO 슬롯 재할당. */
	if (rc != 0) {
		return rc;
	}

	ublk->is_recovering = true;   /* [한국어] 복구 모드 — START_DEV 건너뜀 + END_RECOVERY 발행 근거. */
	return ublk_ctrl_cmd_submit(ublk, UBLK_CMD_START_USER_RECOVERY);   /* [한국어] 복구 시작 명령. */
}

/*
 * [한국어]
 * ublk_start_disk_recovery - RPC 복구 진입점 — 기존 ublk 디바이스의 미완료 IO 복구 시작.
 *
 * @bdev_name: 복구 대상 bdev 이름.
 * @ublk_id: 복구할 디바이스 ID.
 * @ctrl_cb: 복구 완료 콜백.
 * @cb_arg: 콜백 인자.
 * @return: 0 (비동기 진행), 음수 errno.
 *
 * ublk_start_disk 와 유사하지만, ADD_DEV 대신 UBLK_CMD_GET_DEV_INFO 로 시작하는 복구 chain 의
 * 진입점이다. ublk_drv 가 재시작된 뒤 커널에 남아 있는 디바이스를 SPDK 가 다시 인수한다.
 * 사전 검증: app_thread + target active + user_recovery 지원(6.4+) + ID 미사용 + max_dev 한도.
 * bdev open + sector shift 계산 후 디바이스를 리스트에 등록하고 GET_DEV_INFO 를 발행 — 그 응답이
 * QUIESCED 면 ublk_ctrl_start_recovery 로 이어진다(ublk_ctrl_process_cqe 참조).
 * 실행 컨텍스트: app_thread (RPC 핸들러).
 *
 * 호출 체인:
 *   RPC ublk_recover_disk → [본 함수] → spdk_bdev_open_ext + ublk_ctrl_cmd_submit(GET_DEV_INFO)
 */
int
ublk_start_disk_recovery(const char *bdev_name, uint32_t ublk_id, ublk_ctrl_cb ctrl_cb,
			 void *cb_arg)
{
	int			rc;
	struct spdk_bdev	*bdev;
	struct spdk_ublk_dev	*ublk = NULL;
	uint32_t		sector_per_block;

	assert(spdk_thread_is_app_thread(NULL));   /* [한국어] RPC 핸들러 — app_thread 한정. */

	if (g_ublk_tgt.active == false) {   /* [한국어] target 미생성 → 복구 불가. */
		SPDK_ERRLOG("NO ublk target exist\n");
		return -ENODEV;
	}

	if (!g_ublk_tgt.user_recovery) {   /* [한국어] 커널이 USER_RECOVERY 미지원(6.4 미만) → 거부. */
		SPDK_ERRLOG("User recovery is enabled with kernel version >= 6.4\n");
		return -ENOTSUP;
	}

	ublk = ublk_dev_find_by_id(ublk_id);   /* [한국어] ID 중복 검사. */
	if (ublk != NULL) {
		SPDK_DEBUGLOG(ublk, "ublk id %d is in use.\n", ublk_id);
		return -EBUSY;
	}

	if (g_ublk_tgt.num_ublk_devs >= g_ublks_max) {   /* [한국어] 디바이스 수 한도. */
		SPDK_DEBUGLOG(ublk, "Reached maximum number of supported devices: %u\n", g_ublks_max);
		return -ENOTSUP;
	}

	ublk = calloc(1, sizeof(*ublk));   /* [한국어] 디바이스 구조체 할당. */
	if (ublk == NULL) {
		return -ENOMEM;
	}
	ublk->ctrl_cb = ctrl_cb;   /* [한국어] 복구 완료 콜백 등록. */
	ublk->cb_arg = cb_arg;
	ublk->cdev_fd = -1;        /* [한국어] cdev 아직 안 열림. */
	ublk->ublk_id = ublk_id;

	/* [한국어] bdev open (write enable) + hot-remove event_cb. */
	rc = spdk_bdev_open_ext(bdev_name, true, ublk_bdev_event_cb, ublk, &ublk->bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("could not open bdev %s, error=%d\n", bdev_name, rc);
		free(ublk);
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(ublk->bdev_desc);
	ublk->bdev = bdev;
	sector_per_block = spdk_bdev_get_data_block_size(ublk->bdev) >> LINUX_SECTOR_SHIFT;  /* [한국어] block 당 sector 수. */
	ublk->sector_per_block_shift = spdk_u32log2(sector_per_block);   /* [한국어] sector↔block 변환 shift. */

	SPDK_NOTICELOG("Recovering ublk %d with bdev %s\n", ublk->ublk_id, bdev_name);

	ublk_dev_list_register(ublk);   /* [한국어] 전역 리스트 등록. */
	rc = ublk_ctrl_cmd_submit(ublk, UBLK_CMD_GET_DEV_INFO);   /* [한국어] 커널 상태 조회로 복구 chain 시작. */
	if (rc < 0) {
		ublk_free_dev(ublk);   /* [한국어] 제출 실패 → 롤백. */
	}

	return rc;
}

/* [한국어] 'ublk' 디버그 로그 컴포넌트 등록 — UBLK_DEBUGLOG/SPDK_DEBUGLOG(ublk) 출력 게이트. */
SPDK_LOG_REGISTER_COMPONENT(ublk)
/* [한국어] 'ublk_io' 디버그 로그 컴포넌트 등록 — IO hot-path 전용 로그(SPDK_DEBUGLOG(ublk_io)). */
SPDK_LOG_REGISTER_COMPONENT(ublk_io)
