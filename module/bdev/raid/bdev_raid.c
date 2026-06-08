/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK RAID bdev 모듈의 공통 코디네이터 (bdev_raid.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 의 RAID bdev 모듈에서 RAID 레벨에 비종속적인 **공통 코어 로직**을
 * 구현한다. 즉, raid0/raid1/raid5f/concat 같은 개별 RAID 레벨 구현 모듈
 * (module/bdev/raid/raid0.c, raid1.c, raid5f.c, concat.c) 들이 공통으로 사용하는
 * "RAID bdev 생성/삭제, base bdev 등록/제거, examine 흐름, raid_bdev_io 라이프사이클,
 * 백그라운드 process (rebuild) 코디네이션, JSON 설정/덤프, bdev_module 등록"
 * 같은 인프라를 제공한다.
 * - 핵심 자료구조: struct raid_bdev (RAID 전체) / struct raid_base_bdev_info
 *   (각 member bdev) / struct raid_bdev_io_channel (CPU 코어당 IO 채널) /
 *   struct raid_bdev_io (RAID 레벨에서 본 I/O 요청) / struct raid_bdev_process
 *   (rebuild 등 백그라운드 데이터 이전 작업).
 * - 외부에 노출되는 entry point: spdk_bdev_fn_table (g_raid_bdev_fn_table) ─
 *   submit_request/io_type_supported/get_io_channel/dump_info_json/write_config_json/
 *   get_memory_domains, 그리고 spdk_bdev_module (g_raid_if) ─ module_init/
 *   fini_start/module_fini/config_json/get_ctx_size/examine_disk.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK bdev 스택에서 RAID 모듈은 "bdev 위의 bdev (stacked bdev)" 이다.
 *
 *   [Application]
 *     ↓ spdk_bdev_read/write
 *   [bdev layer (lib/bdev)]                ─ spdk_bdev_io 객체 생성/디스패치
 *     ↓ spdk_bdev_fn_table.submit_request
 *   [RAID bdev (module/bdev/raid/bdev_raid.c)] ─ ★ 이 파일
 *     ↓ split → strip 단위로 분할 → module->submit_rw_request
 *   [RAID level module (raid0/raid1/raid5f/concat)] ─ base bdev 인덱스 계산
 *     ↓ spdk_bdev_writev/readv
 *   [base bdev (NVMe / AIO / malloc …)]
 *     ↓
 *   [SSD 등 실제 디바이스]
 *
 * 호출 체인 (READ/WRITE 정상 경로):
 *   spdk_bdev_submit_request → raid_bdev_submit_request (이 파일) →
 *   raid_bdev_io_init → _raid_bdev_request_split (process 진행 중인 윈도우 회피) →
 *   raid_bdev->module->submit_rw_request → 개별 RAID 모듈 → base bdev I/O →
 *   base bdev 완료 콜백 → raid_bdev_io_complete_part → (최종 완료 시)
 *   raid_bdev_io_complete → spdk_bdev_io_complete → bdev 레이어 → 애플리케이션.
 *
 * 실행 컨텍스트:
 * - 호스트 유저스페이스 (DPDK EAL 기반 reactor 스레드).
 * - I/O fast path (submit/complete): 해당 raid_ch 의 SPDK thread 에 고정 (per-channel).
 * - 관리 path (create/configure/delete/remove_base_bdev/examine): app thread (보통 첫
 *   reactor) 에서만 실행 ─ assert(spdk_get_thread() == spdk_thread_get_app_thread()).
 * - 백그라운드 process (rebuild): 전용 spdk_thread (raid_bdev_process_thread_init 에서
 *   spdk_thread_create 로 생성) 에서 실행.
 *
 * === 타 모듈과의 연결 ===
 * - bdev 레이어 (lib/bdev): spdk_bdev_module 로 등록되고 fn_table 을 통해
 *   IO·examine·deinit hook 을 제공받는다. spdk_bdev_open_ext / spdk_bdev_get_io_channel /
 *   spdk_bdev_module_claim_bdev / spdk_bdev_quiesce 등 다수의 API 를 호출.
 * - DIF/DIX (spdk/dif.h): RAID 가 base bdev 의 DIF 를 흘려보낼 때 reference tag 를
 *   raid 의 가상 LBA 기준으로 remap/verify 한다 (raid_bdev_remap_dix_reftag /
 *   raid_bdev_verify_dix_reftag).
 * - 개별 RAID 모듈 (raid0/1/5f/concat): raid_bdev_module_list_add 로 등록된 후
 *   bdev_raid 가 module->start / submit_rw_request / submit_null_payload_request /
 *   submit_process_request / get_io_channel / stop / resize 콜백을 호출.
 * - bdev_raid_sb.c: 슈퍼블록(메타데이터) 로드/저장 (raid_bdev_load_base_bdev_superblock /
 *   raid_bdev_write_superblock / raid_bdev_alloc_superblock / raid_bdev_init_superblock).
 * - JSON-RPC (lib/json + bdev_raid_rpc.c): bdev_raid_create / bdev_raid_delete /
 *   bdev_raid_set_options / bdev_raid_add_base_bdev / bdev_raid_remove_base_bdev RPC 가
 *   이 파일의 raid_bdev_create / raid_bdev_delete / raid_bdev_add_base_bdev /
 *   raid_bdev_remove_base_bdev 등을 호출.
 * - 전역 상태:
 *   g_raid_bdev_list (TAILQ) — 모든 raid_bdev 등록.
 *   g_raid_modules  (TAILQ) — raid0/raid1/raid5f/concat 모듈 등록 리스트.
 *   g_opts                  — process_window_size / max_bandwidth 등 옵션.
 *   g_raid_if               — spdk_bdev_module (RAID 자체).
 *   g_raid_bdev_fn_table    — RAID bdev 가 bdev 레이어에 제공하는 vtable.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수 (entry point):
 * - raid_bdev_create / _raid_bdev_create   : RAID bdev 생성. RAID 레벨/strip/uuid 검증
 *                                            후 raid_bdev 할당 + base_bdev_info[] 배열.
 * - raid_bdev_add_base_bdev                : base bdev 1 개 추가 (slot 검색 후
 *                                            raid_bdev_configure_base_bdev 호출).
 * - raid_bdev_configure / _cont            : 모든 base bdev 가 확보되면 spdk_bdev_register
 *                                            로 bdev 레이어에 노출 (state=ONLINE).
 * - raid_bdev_remove_base_bdev / _bdev_quiesce / _do_remove : base bdev 1 개 제거. quiesce
 *                                            → 슈퍼블록 업데이트 → 채널에서 채널 제거.
 * - raid_bdev_delete                       : RAID bdev 전체 삭제 (deconfigure + 모든
 *                                            base bdev free).
 * - raid_bdev_examine                      : 새 bdev 출현 시 슈퍼블록 검사하여 멤버인지
 *                                            판별. 슈퍼블록 없으면 raid_bdev_examine_no_sb
 *                                            로 미리 설정된 raid_bdev 의 슬롯에 매칭.
 * - raid_bdev_submit_request               : bdev 레이어의 submit hook. raid_bdev_io 초기화
 *                                            후 IO type 별 분기 (READ / WRITE / RESET /
 *                                            FLUSH / UNMAP).
 * - raid_bdev_io_complete / _complete_part : 완료 fan-in. base_bdev_io_remaining
 *                                            카운터를 감소시키고 0 일 때 상위로 완료.
 * - raid_bdev_process_*                    : rebuild 등 백그라운드 데이터 이전.
 *                                            quiesce-범위 lock → 처리 → unquiesce 반복.
 *
 * 구조체:
 * - raid_bdev_io_channel  : per-channel 컨텍스트 (CPU 코어당 1 개). base_channel[] 와
 *                           module_channel, 그리고 process 진행 상태를 보관.
 * - raid_bdev_process     : 백그라운드 process 컨텍스트 (현재는 rebuild). 윈도우 기반
 *                           처리 상태, QoS, 종료 시 액션 큐를 관리.
 * - raid_process_qos      : process 의 대역폭 제한 (token bucket).
 * - raid_bdev_process_request : process 가 발행하는 큰 단위 I/O 요청 (DMA 버퍼 포함).
 */

#include "bdev_raid.h"                 /* [한국어] RAID 모듈의 내부/공개 자료구조 (raid_bdev,
                                        * raid_base_bdev_info, raid_bdev_io, raid_bdev_module,
                                        * enum raid_level, RAID_FOR_EACH_BASE_BDEV 매크로 등)
                                        * 정의. 같은 디렉토리의 bdev_raid.h. */
#include "spdk/env.h"                  /* [한국어] DPDK 추상화 — spdk_dma_malloc/free 등 DMA
                                        * 가능 메모리 할당 (process 의 DMA 버퍼용). */
#include "spdk/thread.h"               /* [한국어] spdk_thread / spdk_io_channel /
                                        * spdk_poller / spdk_thread_send_msg /
                                        * spdk_thread_exec_msg 등 — RAID 가 cross-thread 작업을
                                        * 위해 app thread 로 메시지 보낼 때 사용. */
#include "spdk/log.h"                  /* [한국어] SPDK_ERRLOG / SPDK_NOTICELOG /
                                        * SPDK_DEBUGLOG / SPDK_WARNLOG — 로그 매크로. */
#include "spdk/string.h"               /* [한국어] spdk_strerror — errno → 문자열 변환. */
#include "spdk/util.h"                 /* [한국어] spdk_u32_is_pow2 / spdk_u32log2 /
                                        * spdk_divide_round_up / spdk_min / spdk_max /
                                        * spdk_round_up / SPDK_COUNTOF — 산술/배열 매크로. */
#include "spdk/json.h"                 /* [한국어] JSON 작성 (spdk_json_write_*) — dump_info /
                                        * write_config / RPC 응답. */
#include "spdk/likely.h"               /* [한국어] spdk_likely / spdk_unlikely — 컴파일러 분기
                                        * 예측 힌트 (__builtin_expect 래퍼). hot path 인
                                        * raid_bdev_io_complete 등에서 사용. */
#include "spdk/trace.h"                /* [한국어] spdk_trace_record / spdk_trace_register_* —
                                        * 이벤트 트레이싱 (TRACE_BDEV_RAID_IO_START/DONE). */
#include "spdk_internal/trace_defs.h"  /* [한국어] TRACE_GROUP_BDEV_RAID 등 내부 trace 그룹
                                        * 정의. SPDK 내부 헤더. */

#define RAID_OFFSET_BLOCKS_INVALID	UINT64_MAX
/* [한국어] split offset / process offset 의 "유효하지 않음" 표시값.
 * raid_io->split.offset 이 RAID_OFFSET_BLOCKS_INVALID 면 "이 IO 는 split 되지 않았다",
 * raid_ch->process.offset 이 이 값이면 "이 채널에서 background process 가 진행 중이 아니다"
 * 라는 의미. UINT64_MAX 를 sentinel 로 사용 (실제 LBA 가 64-bit 풀 값일 일은 없음). */
#define RAID_BDEV_PROCESS_MAX_QD	16
/* [한국어] background process (rebuild 등) 가 동시에 in-flight 로 발행할 수 있는
 * raid_bdev_process_request 의 최대 개수 (Queue Depth). raid_bdev_process_alloc 에서
 * 이 개수만큼 DMA 버퍼와 함께 미리 할당해 process->requests 풀에 넣어둔다. 너무 크면
 * 메모리 사용량 증가, 너무 작으면 process 대역폭 감소. */

#define RAID_BDEV_PROCESS_WINDOW_SIZE_KB_DEFAULT	1024
/* [한국어] process 윈도우 크기의 기본값 (KB). 한 번 quiesce-처리-unquiesce 사이클에서
 * 처리하는 LBA 범위 크기. 1024KB = 1MB. 큰 값일수록 fast-path I/O 가 더 오래 지연되지만
 * rebuild 가 빨라진다. raid_bdev_set_opts 로 런타임 변경 가능. */
#define RAID_BDEV_PROCESS_MAX_BANDWIDTH_MB_SEC_DEFAULT	0
/* [한국어] process 의 대역폭 상한 (MB/s). 0 = 무제한 (QoS 비활성). 0 이 아니면
 * raid_bdev_process_alloc 에서 token bucket 을 초기화하고 raid_bdev_process_consume_token
 * 으로 윈도우 처리 전에 토큰을 차감한다. */

static bool g_shutdown_started = false;
/* [한국어] bdev 레이어 shutdown 진행 여부 플래그. raid_bdev_fini_start 에서 true 로 세팅
 * (raid_bdev_exit 보다 먼저 호출됨). _raid_bdev_destruct 가 이 플래그를 확인하여 shutdown
 * 중이면 remove_scheduled 와 무관하게 base bdev 자원을 적극적으로 해제한다.
 * 설정자: raid_bdev_fini_start.
 * 읽는 자: _raid_bdev_destruct.
 * 동기화: bdev 레이어가 보장하는 셧다운 순서 — app thread 단일 진입이므로 락 불필요. */

/* List of all raid bdevs */
struct raid_all_tailq g_raid_bdev_list = TAILQ_HEAD_INITIALIZER(g_raid_bdev_list);
/* [한국어] 현재 시스템에 등록된 모든 raid_bdev 의 전역 TAILQ. global_link 멤버로 연결.
 * 설정자: _raid_bdev_create 가 INSERT_TAIL, raid_bdev_cleanup 이 REMOVE.
 * 읽는 자: raid_bdev_find_by_name / find_by_uuid / fini_start / exit / examine_no_sb 등.
 * 동기화: 관리 path 는 모두 app thread 에서만 동작하므로 락 불필요 (SPDK 의 lockless 패턴). */

static TAILQ_HEAD(, raid_bdev_module) g_raid_modules = TAILQ_HEAD_INITIALIZER(g_raid_modules);
/* [한국어] 등록된 RAID 레벨별 모듈 (raid0/raid1/raid5f/concat) 의 TAILQ. 각 모듈은
 * 자기 .c 파일에서 RAID_MODULE_REGISTER 매크로(__attribute__((constructor)) 기반) 로
 * raid_bdev_module_list_add 를 호출하여 여기에 등록.
 * 읽는 자: raid_bdev_module_find (RAID 레벨 → 모듈 검색).
 * 동기화: 등록은 프로그램 시작 시 1 회 (constructor) → 이후 read-only. */

/*
 * raid_bdev_io_channel is the context of spdk_io_channel for raid bdev device. It
 * contains the relationship of raid bdev io channel with base bdev io channels.
 */
/* [한국어] raid_bdev 의 spdk_io_channel 컨텍스트 (CPU 코어당 1 개).
 * spdk_io_device_register(raid_bdev, create_cb, destroy_cb, sizeof(*ch), name) 호출 시
 * SPDK thread 레이어가 각 reactor 별로 이 구조체 1 개씩 자동 할당. fast-path 의 모든
 * I/O 요청은 호출 코어의 raid_ch 를 통해 base bdev IO 채널로 분배된다.
 * 실행 컨텍스트: I/O 와 같은 SPDK thread 에 고정 → lockless. */
struct raid_bdev_io_channel {
	/* Array of IO channels of base bdevs */
	struct spdk_io_channel	**base_channel;
	/* [한국어] base bdev 1 개당 1 개의 spdk_io_channel 포인터 배열. 크기는
	 * raid_bdev->num_base_bdevs. base bdev 가 제거되었거나 (NULL) process target 이라면
	 * 해당 슬롯은 NULL. raid0/raid1/concat 등 RAID 레벨 모듈이 base_channel[i] 를 사용해
	 * spdk_bdev_writev(desc[i], base_channel[i], …) 를 호출한다.
	 * 설정자: raid_bdev_create_cb (채널 생성 시 spdk_bdev_get_io_channel 로 채움).
	 * 읽는 자: 모든 RAID 레벨 모듈의 submit_rw_request / null_payload_request.
	 * 동기화: 채널 단일 스레드 소유 → 락 불필요. */

	/* Private raid module IO channel */
	struct spdk_io_channel	*module_channel;
	/* [한국어] RAID 레벨 모듈이 자체적으로 사용하는 IO 채널 (예: raid5f 가 parity 버퍼
	 * 풀을 채널마다 할당). module->get_io_channel 콜백이 NULL 이 아닐 때만 설정.
	 * 설정자: raid_bdev_create_cb 가 module->get_io_channel(raid_bdev) 호출.
	 * 읽는 자: raid_bdev_channel_get_module_ctx → 해당 모듈의 코드.
	 * 동기화: 채널 단일 스레드 소유. */

	/* Background process data */
	struct {
		uint64_t offset;
		/* [한국어] 이 채널 입장에서 보이는 "process 가 이미 처리 완료한 LBA 범위의 끝".
		 * 0 ~ offset 미만은 process_ch 를 통해 target 까지 mirror 됨. RAID_OFFSET_BLOCKS_INVALID
		 * 이면 이 채널에 process 가 attach 되지 않음. _raid_bdev_request_split 가 이 값을
		 * 보고 처리 완료 영역이면 process.ch_processed 로 채널 스왑 (target 도 함께 쓰기),
		 * 처리 안 된 영역이면 일반 base_channel, 걸치면 split 한다.
		 * 설정자: raid_bdev_process_channel_update (process 가 한 윈도우 완료 시
		 *         each_channel iteration 으로 모든 채널 offset 을 올림).
		 * 읽는 자: _raid_bdev_request_split (fast path).
		 * 동기화: SPDK thread 메시지로 update → 채널은 자기 스레드에서만 read. */
		struct spdk_io_channel *target_ch;
		/* [한국어] rebuild target base bdev 의 IO 채널. process 가 활성화되면
		 * raid_bdev_ch_process_setup 에서 spdk_bdev_get_io_channel(target->desc) 로 획득.
		 * 처리 완료 영역의 mirror write 에 사용.
		 * 설정자: raid_bdev_ch_process_setup.
		 * 읽는 자: raid_bdev_ch_process_cleanup / channel_process_finish.
		 * 동기화: 채널 소유 스레드 전용. */
		struct raid_bdev_io_channel *ch_processed;
		/* [한국어] "처리 완료 영역 전용" 가상 채널. base_channel[] 가 동일하되 process target
		 * 슬롯은 target_ch 로 대체된 사본. _raid_bdev_request_split 가 처리 완료 영역
		 * I/O 에 대해 raid_io->raid_ch 를 이 채널로 교체하여 RAID 모듈이 target 까지
		 * 자연스럽게 write 하도록 한다.
		 * 설정자: raid_bdev_ch_process_setup (calloc + base_channel 복사).
		 * 읽는 자: _raid_bdev_request_split / split 후 raid_io_complete.
		 * 동기화: 부모 채널과 동일 스레드. */
	} process;
};

enum raid_bdev_process_state {
	RAID_PROCESS_STATE_INIT,
	/* [한국어] process 객체 할당 직후 초기 상태. raid_bdev_process_thread_init 가
	 * RUNNING 으로 전이시키기 전. */
	RAID_PROCESS_STATE_RUNNING,
	/* [한국어] process thread 가 활성화되어 윈도우 단위로 데이터 이전 중. */
	RAID_PROCESS_STATE_STOPPING,
	/* [한국어] 종료 요청을 받았으나 마무리 절차 (윈도우 unlock, target removal 등)
	 * 진행 중. raid_bdev_process_finish 가 진입점. */
	RAID_PROCESS_STATE_STOPPED,
	/* [한국어] 모든 정리가 끝나고 thread 종료 직전. raid_bdev_process_finish_done 에서 설정. */
};

struct raid_process_qos {
	bool enable_qos;
	/* [한국어] QoS 활성화 여부. g_opts.process_max_bandwidth_mb_sec != 0 일 때 true.
	 * 설정자: raid_bdev_process_alloc.
	 * 읽는 자: raid_bdev_process_lock_window_range. */
	uint64_t last_tsc;
	/* [한국어] 마지막으로 토큰을 충전한 시점의 spdk_get_ticks() 값. 다음 충전 시
	 * (now - last_tsc) * bytes_per_tsc 만큼 토큰 누적.
	 * 설정자: raid_bdev_process_alloc / consume_token. */
	double bytes_per_tsc;
	/* [한국어] 1 tick 당 충전되는 토큰량 (bytes). max_bandwidth_mb_sec * 1MB /
	 * spdk_get_ticks_hz() 로 계산. */
	double bytes_available;
	/* [한국어] 현재 가용 토큰 (bytes). 윈도우 처리 시 window_size*blocklen 만큼 차감. */
	double bytes_max;
	/* [한국어] 토큰 버킷 상한 (= 1ms 분량). 토큰 무한 누적 방지. */
	struct spdk_poller *process_continue_poller;
	/* [한국어] 토큰 고갈 시 토큰 충전을 기다리며 주기 호출되는 poller. lock_window_range
	 * 에서 토큰 부족 → spdk_poller_resume; 충전되어 lock 성공 → spdk_poller_pause.
	 * 설정자: raid_bdev_process_thread_init.
	 * 동기화: process thread 전용. */
};

struct raid_bdev_process {
	struct raid_bdev		*raid_bdev;
	/* [한국어] 이 process 가 속한 RAID bdev. process 전체 라이프타임 동안 불변. */
	enum raid_process_type		type;
	/* [한국어] process 종류. 현재는 RAID_PROCESS_REBUILD 만 존재. 확장 시 scrub 등 추가 가능. */
	enum raid_bdev_process_state	state;
	/* [한국어] 상태 머신 — INIT → RUNNING → STOPPING → STOPPED.
	 * 설정자: raid_bdev_process_thread_init / _finish / _finish_done 등.
	 * 동기화: 거의 모든 전이가 process->thread 에서 발생. */
	struct spdk_thread		*thread;
	/* [한국어] process 전용 SPDK thread (예: "raid_bdev_name_rebuild"). spdk_thread_create
	 * 로 생성, finish 시 spdk_thread_exit. cross-thread 호출은 spdk_thread_send_msg. */
	struct raid_bdev_io_channel	*raid_ch;
	/* [한국어] process thread 가 보유하는 raid_bdev_io_channel (자기 thread 의 채널).
	 * spdk_get_io_channel(raid_bdev) 로 획득. */
	TAILQ_HEAD(, raid_bdev_process_request) requests;
	/* [한국어] 미리 할당된 process_request 풀 (RAID_BDEV_PROCESS_MAX_QD 개). idle 요청만
	 * 큐에 있고 in-flight 요청은 큐에서 빠진 상태. completion 시 다시 INSERT_TAIL. */
	uint64_t			max_window_size;
	/* [한국어] 한 사이클에서 처리할 수 있는 최대 LBA 범위. g_opts.process_window_size_kb
	 * 와 bdev 의 write_unit_size 중 큰 값으로 계산. */
	uint64_t			window_size;
	/* [한국어] 이번 사이클에서 실제로 발행한 윈도우 크기. submit 결과로 조정됨. */
	uint64_t			window_remaining;
	/* [한국어] 이번 윈도우에서 아직 완료되지 않은 블록 수. completion 마다 감소.
	 * 0 도달 시 channels_update → unlock_window_range. */
	int				window_status;
	/* [한국어] 이번 윈도우의 누적 status (한 base I/O 라도 실패하면 errno 저장). */
	uint64_t			window_offset;
	/* [한국어] 현재 처리 중인 윈도우의 시작 LBA. window_remaining 가 0 도달 시
	 * window_offset += window_size 진행. */
	bool				window_range_locked;
	/* [한국어] spdk_bdev_quiesce_range 로 해당 LBA 범위에 대해 application I/O 를
	 * 일시 차단했는지 여부. lock 후 처리, unlock 으로 차단 해제. */
	struct raid_base_bdev_info	*target;
	/* [한국어] rebuild 대상 base bdev. 처리 완료된 LBA 영역만큼 application write 가
	 * mirror 되어 들어간다. process 완료 후 정식 멤버로 승격 (is_process_target = false). */
	int				status;
	/* [한국어] process 전체의 최종 상태. 0=성공, 음수 errno=실패. _ECANCELED (셧다운),
	 * -ENODEV (base bdev 제거) 등이 들어갈 수 있다. */
	TAILQ_HEAD(, raid_process_finish_action) finish_actions;
	/* [한국어] process 종료 시 실행할 콜백 큐. unregister 진행 중 process 가 살아 있으면
	 * unregister 콜백을 여기에 enqueue. _raid_bdev_process_finish_done 가 일괄 실행. */
	struct raid_process_qos		qos;
	/* [한국어] 대역폭 제한 (token bucket). enable 시 lock_window_range 전에 토큰 검사. */
};

struct raid_process_finish_action {
	spdk_msg_fn cb;
	/* [한국어] process 종료 시 호출할 콜백 함수 (단일 인자 ctx). */
	void *cb_ctx;
	/* [한국어] cb 에 전달할 컨텍스트. unregister 의 경우 raid_bdev 포인터. */
	TAILQ_ENTRY(raid_process_finish_action) link;
	/* [한국어] finish_actions TAILQ 의 노드 링크. */
};

static struct spdk_raid_bdev_opts g_opts = {
	.process_window_size_kb = RAID_BDEV_PROCESS_WINDOW_SIZE_KB_DEFAULT,
	.process_max_bandwidth_mb_sec = RAID_BDEV_PROCESS_MAX_BANDWIDTH_MB_SEC_DEFAULT,
};
/* [한국어] RAID 모듈 전역 옵션. RPC bdev_raid_set_options 로 변경 가능.
 * 설정자: raid_bdev_set_opts.
 * 읽는 자: raid_bdev_process_alloc / raid_bdev_opts_config_json. */

/*
 * [한국어]
 * raid_bdev_get_opts - 현재 RAID 모듈 전역 옵션을 호출자 버퍼로 복사한다.
 *
 * @opts: 출력 버퍼 (호출자가 할당). 복사 후 process_window_size_kb /
 *        process_max_bandwidth_mb_sec 가 채워진다.
 *
 * 동기/배경: bdev_raid_get_options RPC 핸들러가 사용. 사용자가 현재 설정을 조회.
 * 실행 컨텍스트: 보통 app thread (RPC). 단일 스레드 가정.
 *
 * 호출 체인: bdev_raid_get_options RPC → [raid_bdev_get_opts].
 */
void
raid_bdev_get_opts(struct spdk_raid_bdev_opts *opts)
{
	*opts = g_opts;                /* [한국어] 단순 구조체 복사 — 옵션 멤버 전체 복사. */
}

/*
 * [한국어]
 * raid_bdev_set_opts - RAID 모듈 전역 옵션을 갱신한다.
 *
 * @opts: 새로 적용할 옵션. process_window_size_kb == 0 은 거부.
 * @return: 0 성공, -EINVAL 유효성 위반.
 *
 * 동기: bdev_raid_set_options RPC. process_window_size 가 0 이면 background process 가
 * 진행을 못 하므로 거부.
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인: RPC → [raid_bdev_set_opts] → g_opts 업데이트.
 */
int
raid_bdev_set_opts(const struct spdk_raid_bdev_opts *opts)
{
	if (opts->process_window_size_kb == 0) {  /* [한국어] 윈도우 크기 0 은 의미 없음 → 거부. */
		return -EINVAL;
	}

	g_opts = *opts;                /* [한국어] 전체 옵션 한 번에 적용. 진행 중 process 는
	                                * 이미 alloc 시점의 값을 사용 중이므로 영향 없음. */

	return 0;
}

/*
 * [한국어]
 * raid_bdev_module_find - RAID 레벨로 등록된 모듈 검색.
 *
 * @level: 검색할 RAID 레벨 (RAID0 / RAID1 / RAID5F / CONCAT).
 * @return: 매칭되는 raid_bdev_module 포인터, 없으면 NULL.
 *
 * 동기: _raid_bdev_create 가 사용자가 지정한 level → 실제 구현 모듈 매핑.
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인: _raid_bdev_create / raid_bdev_module_list_add → [raid_bdev_module_find].
 */
static struct raid_bdev_module *
raid_bdev_module_find(enum raid_level level)
{
	struct raid_bdev_module *raid_module;

	TAILQ_FOREACH(raid_module, &g_raid_modules, link) {  /* [한국어] 등록된 모든 모듈 순회. */
		if (raid_module->level == level) {           /* [한국어] level 일치하면 즉시 반환. */
			return raid_module;
		}
	}

	return NULL;                   /* [한국어] 매칭 없음 — 호출자가 -EINVAL 처리. */
}

/*
 * [한국어]
 * raid_bdev_module_list_add - RAID 레벨별 모듈을 전역 등록 리스트에 추가한다.
 *
 * @raid_module: raid0/raid1/raid5f/concat 의 정적 구조체. level / base_bdevs_min /
 *               base_bdevs_constraint / start / stop / submit_rw_request 등이 채워져 있음.
 *
 * 동기: 각 RAID 레벨 .c 파일이 RAID_MODULE_REGISTER (constructor) 매크로로 호출.
 * 동일 level 중복 등록은 프로그래밍 오류이므로 assert.
 * 실행 컨텍스트: 프로그램 시작 직후 (main 전), single-thread.
 *
 * 호출 체인: __attribute__((constructor)) → [raid_bdev_module_list_add].
 */
void
raid_bdev_module_list_add(struct raid_bdev_module *raid_module)
{
	if (raid_bdev_module_find(raid_module->level) != NULL) {  /* [한국어] 중복 등록 검사. */
		SPDK_ERRLOG("module for raid level '%s' already registered.\n",
			    raid_bdev_level_to_str(raid_module->level));
		assert(false);                                    /* [한국어] 디버그 빌드 시 즉시 abort. */
	} else {
		TAILQ_INSERT_TAIL(&g_raid_modules, raid_module, link);  /* [한국어] 정상 등록. */
	}
}

/*
 * [한국어]
 * raid_bdev_channel_get_base_channel - 채널에서 인덱스에 해당하는 base bdev IO 채널 반환.
 *
 * @raid_ch: 호출 스레드의 raid 채널 컨텍스트.
 * @idx: base bdev 슬롯 인덱스 (0 ~ num_base_bdevs-1).
 * @return: 해당 슬롯의 spdk_io_channel * (제거되었거나 process target 이면 NULL).
 *
 * 동기: RAID 레벨 모듈 (raid0/1/5f/concat) 이 strip 계산 후 어느 base bdev 의 채널로
 * 발행할지 결정할 때 호출하는 1-라인 접근자. 함수로 노출해 raid_bdev_io_channel 의
 * 내부 구조를 모듈에 숨김.
 * 실행 컨텍스트: I/O fast path. 채널 단일 스레드.
 *
 * 호출 체인: 개별 RAID 모듈의 submit_rw_request → [raid_bdev_channel_get_base_channel].
 */
struct spdk_io_channel *
raid_bdev_channel_get_base_channel(struct raid_bdev_io_channel *raid_ch, uint8_t idx)
{
	return raid_ch->base_channel[idx];   /* [한국어] 단순 배열 접근. NULL 일 수 있음. */
}

/*
 * [한국어]
 * raid_bdev_channel_get_module_ctx - RAID 레벨 모듈의 채널 ctx 반환.
 *
 * @raid_ch: 호출 스레드의 raid 채널.
 * @return: module->get_io_channel 이 반환한 채널의 ctx (모듈 정의 자료구조).
 *
 * 동기: 예) raid5f 가 채널별 parity 버퍼 풀에 접근할 때 사용. module_channel 이
 * NULL 이면 모듈이 채널을 요구하지 않은 것이므로 assert (호출자 책임).
 * 실행 컨텍스트: I/O fast path.
 *
 * 호출 체인: raid5f / raid1 의 submit 콜백 → [raid_bdev_channel_get_module_ctx].
 */
void *
raid_bdev_channel_get_module_ctx(struct raid_bdev_io_channel *raid_ch)
{
	assert(raid_ch->module_channel != NULL);            /* [한국어] 모듈이 채널을 등록한 경우만 호출돼야 함. */

	return spdk_io_channel_get_ctx(raid_ch->module_channel);  /* [한국어] SPDK 표준 매크로 — 채널 메모리 다음에 붙은 ctx 영역. */
}

/*
 * [한국어]
 * raid_bdev_channel_get_base_info - 채널과 base bdev 포인터로 base_info 검색.
 *
 * @raid_ch: 채널 ctx.
 * @base_bdev: 찾을 base bdev (예: completion 콜백의 bdev_io->bdev).
 * @return: 매칭되는 raid_base_bdev_info *, 없으면 NULL.
 *
 * 동기: 완료 콜백에서 어느 base bdev 인지 알아야 할 때 사용 (예: raid1 의 reconstruction).
 * 실행 컨텍스트: 채널 스레드.
 *
 * 호출 체인: RAID 모듈 completion → [raid_bdev_channel_get_base_info].
 */
struct raid_base_bdev_info *
raid_bdev_channel_get_base_info(struct raid_bdev_io_channel *raid_ch, struct spdk_bdev *base_bdev)
{
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(raid_ch);   /* [한국어] ctx → 채널 객체 역참조 (SPDK 매크로). */
	struct raid_bdev *raid_bdev = spdk_io_channel_get_io_device(ch);  /* [한국어] 채널이 속한 io_device = raid_bdev. */
	uint8_t i;

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {                 /* [한국어] 모든 base bdev 슬롯 순회. */
		struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[i];

		if (base_info->is_configured &&                          /* [한국어] 설정된 슬롯만 검사 — */
		    spdk_bdev_desc_get_bdev(base_info->desc) == base_bdev) {  /* [한국어] desc 의 bdev 가 매칭? */
			return base_info;
		}
	}

	return NULL;                   /* [한국어] 찾지 못함 — 호출자가 처리. */
}

/* Function declarations */
/* [한국어] forward declaration — bdev_module 정적 초기화 (g_raid_if) 에서 사용하지만
 * 정의는 파일 후반에 있으므로 미리 선언. examine_disk / module_init / deconfigure 흐름의
 * 진입점들이다. */
static void	raid_bdev_examine(struct spdk_bdev *bdev);
static int	raid_bdev_init(void);
static void	raid_bdev_deconfigure(struct raid_bdev *raid_bdev,
				      raid_bdev_destruct_cb cb_fn, void *cb_arg);

/*
 * [한국어]
 * raid_bdev_ch_process_cleanup - 채널에서 background process 관련 자원 해제.
 *
 * @raid_ch: 정리 대상 채널 ctx.
 *
 * 동기: process 가 종료되거나, channel destroy 시, 혹은 setup 실패 롤백 경로에서 사용.
 * process.offset 을 INVALID 로 표시하여 fast path 가 일반 base_channel 로 동작하게 만든다.
 * 동작:
 *   1) offset INVALID 로 마킹.
 *   2) target_ch 가 잡혀 있으면 spdk_put_io_channel.
 *   3) ch_processed (가상 채널) 의 base_channel 배열과 본체 free.
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인: raid_bdev_destroy_cb / channel_process_finish / channel_abort_start_process /
 *           raid_bdev_ch_process_setup(err) → [raid_bdev_ch_process_cleanup].
 */
static void
raid_bdev_ch_process_cleanup(struct raid_bdev_io_channel *raid_ch)
{
	raid_ch->process.offset = RAID_OFFSET_BLOCKS_INVALID;   /* [한국어] fast path 가 process 분기를 건너뛰도록 표시. */

	if (raid_ch->process.target_ch != NULL) {               /* [한국어] target 채널이 attach 되어 있던 경우만 해제. */
		spdk_put_io_channel(raid_ch->process.target_ch);
		raid_ch->process.target_ch = NULL;
	}

	if (raid_ch->process.ch_processed != NULL) {            /* [한국어] 가상 채널 (calloc 두 번) 해제. */
		free(raid_ch->process.ch_processed->base_channel); /* [한국어] base_channel 배열은 raid_bdev_ch_process_setup 에서 별도 calloc. */
		free(raid_ch->process.ch_processed);
		raid_ch->process.ch_processed = NULL;
	}
}

/*
 * [한국어]
 * raid_bdev_ch_process_setup - 채널에 background process 연결 (가상 채널 구성).
 *
 * @raid_ch: 설정 대상 채널.
 * @process: 진행 중인 raid_bdev_process.
 * @return: 0 성공, -ENOMEM 메모리 부족.
 *
 * 동기: process 가 시작될 때 (raid_bdev_process_start → for_each_channel) 또는 채널이
 * 새로 생성될 때 (raid_bdev_create_cb 가 raid_bdev->process != NULL 인 경우) 호출.
 * 동작:
 *   1) raid_ch->process.offset 을 현재 window_offset 으로 동기화.
 *   2) target bdev 의 IO 채널 획득 → raid_ch->process.target_ch.
 *   3) 가상 채널 raid_ch_processed 할당 (base_channel 배열은 원본 채널 복사 + target 슬롯
 *      만 target_ch 로 교체). 이 가상 채널은 "처리 완료 영역의 I/O 는 target 까지 mirror"
 *      라는 의미를 자연스럽게 구현한다.
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인: raid_bdev_create_cb / raid_bdev_channel_start_process →
 *           [raid_bdev_ch_process_setup].
 */
static int
raid_bdev_ch_process_setup(struct raid_bdev_io_channel *raid_ch, struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	struct raid_bdev_io_channel *raid_ch_processed;
	struct raid_base_bdev_info *base_info;

	raid_ch->process.offset = process->window_offset;        /* [한국어] 이 채널이 보는 "처리 완료 경계" 를 process 의 현재 진행도로 동기화. */

	/* In the future we may have other types of processes which don't use a target bdev,
	 * like data scrubbing or strip size migration. Until then, expect that there always is
	 * a process target. */
	assert(process->target != NULL);                         /* [한국어] 현재는 rebuild 만 — target 없는 process 는 미구현. */

	raid_ch->process.target_ch = spdk_bdev_get_io_channel(process->target->desc);  /* [한국어] target bdev 의 채널 획득 (이 채널 thread 에 종속). */
	if (raid_ch->process.target_ch == NULL) {
		goto err;
	}

	raid_ch_processed = calloc(1, sizeof(*raid_ch_processed));   /* [한국어] 가상 채널 본체 할당. */
	if (raid_ch_processed == NULL) {
		goto err;
	}
	raid_ch->process.ch_processed = raid_ch_processed;

	raid_ch_processed->base_channel = calloc(raid_bdev->num_base_bdevs,
					  sizeof(*raid_ch_processed->base_channel));  /* [한국어] 가상 채널의 base_channel[] 배열. */
	if (raid_ch_processed->base_channel == NULL) {
		goto err;
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {          /* [한국어] 모든 base bdev 순회. */
		uint8_t slot = raid_bdev_base_bdev_slot(base_info);  /* [한국어] base_info 인덱스 계산 (포인터 산술). */

		if (base_info != process->target) {              /* [한국어] target 이 아닌 슬롯 — 원본 채널 그대로 사용. */
			raid_ch_processed->base_channel[slot] = raid_ch->base_channel[slot];
		} else {                                         /* [한국어] target 슬롯 — 새로 잡은 target_ch 로 교체. */
			raid_ch_processed->base_channel[slot] = raid_ch->process.target_ch;
		}
	}

	raid_ch_processed->module_channel = raid_ch->module_channel;   /* [한국어] 모듈 채널은 공유 (필요 시 동일 ctx 사용). */
	raid_ch_processed->process.offset = RAID_OFFSET_BLOCKS_INVALID; /* [한국어] 가상 채널 자체는 "처리 영역" 이므로 추가 process 분기 없음. */

	return 0;
err:
	raid_bdev_ch_process_cleanup(raid_ch);                   /* [한국어] 부분 할당된 자원 롤백. */
	return -ENOMEM;
}

/*
 * brief:
 * raid_bdev_create_cb function is a cb function for raid bdev which creates the
 * hierarchy from raid bdev to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to raid bdev io device represented by raid_bdev
 * ctx_buf - pointer to context buffer for raid bdev io channel
 * returns:
 * 0 - success
 * non zero - failure
 */
/*
 * [한국어]
 * raid_bdev_create_cb - raid_bdev io_device 의 채널 생성 콜백 (per CPU core).
 *
 * @io_device: spdk_io_device_register 에서 등록한 raid_bdev 포인터.
 * @ctx_buf: SPDK 가 미리 할당한 ctx 버퍼 (= raid_bdev_io_channel).
 * @return: 0 성공, -ENOMEM 등 음수 실패.
 *
 * 동기: 첫 spdk_get_io_channel 호출 시 코어마다 한 번 호출되어 base bdev 의 IO 채널들을
 * 모아둔다. fast path 의 모든 IO 는 이 채널을 통해 base bdev 로 분배됨.
 * 동작:
 *   1) base_channel 배열 calloc (size = num_base_bdevs).
 *   2) 각 base bdev 마다 spdk_bdev_get_io_channel 호출. is_process_target 인 슬롯은
 *      skip (target 은 아직 정식 멤버가 아니므로 일반 I/O 가 들어가면 안 됨).
 *   3) 모듈이 module->get_io_channel 을 정의했다면 호출 → module_channel 저장.
 *   4) raid_bdev->process 가 있으면 raid_bdev_ch_process_setup 으로 가상 채널 구성.
 * 실행 컨텍스트: spdk_get_io_channel 의 호출자 스레드 (보통 각 reactor).
 *
 * 호출 체인: spdk_get_io_channel(raid_bdev) → SPDK thread 레이어 → [raid_bdev_create_cb].
 */
static int
raid_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct raid_bdev            *raid_bdev = io_device;     /* [한국어] io_device 등록 시 전달한 raid_bdev. */
	struct raid_bdev_io_channel *raid_ch = ctx_buf;         /* [한국어] SPDK 가 채널과 함께 ctx 영역 (sizeof(raid_bdev_io_channel)) 을 할당. */
	uint8_t i;
	int ret = -ENOMEM;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_create_cb, %p\n", raid_ch);

	assert(raid_bdev != NULL);
	assert(raid_bdev->state == RAID_BDEV_STATE_ONLINE);     /* [한국어] ONLINE 인 raid_bdev 에 대해서만 채널 생성. */

	raid_ch->base_channel = calloc(raid_bdev->num_base_bdevs, sizeof(struct spdk_io_channel *));  /* [한국어] base 채널 포인터 배열 할당. */
	if (!raid_ch->base_channel) {
		SPDK_ERRLOG("Unable to allocate base bdevs io channel\n");
		return -ENOMEM;
	}

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/*
		 * Get the spdk_io_channel for all the base bdevs. This is used during
		 * split logic to send the respective child bdev ios to respective base
		 * bdev io channel.
		 * Skip missing base bdevs and the process target, which should also be treated as
		 * missing until the process completes.
		 */
		if (raid_bdev->base_bdev_info[i].is_configured == false ||  /* [한국어] 미설정 슬롯 — NULL 로 둠 (제거된 멤버). */
		    raid_bdev->base_bdev_info[i].is_process_target == true) {  /* [한국어] rebuild 대상은 일반 I/O 받지 않음. */
			continue;
		}
		raid_ch->base_channel[i] = spdk_bdev_get_io_channel(
						   raid_bdev->base_bdev_info[i].desc);    /* [한국어] base bdev 의 채널 획득. */
		if (!raid_ch->base_channel[i]) {
			SPDK_ERRLOG("Unable to create io channel for base bdev\n");
			goto err;
		}
	}

	if (raid_bdev->module->get_io_channel) {                /* [한국어] RAID 레벨 모듈이 채널을 요구하면 추가 획득. */
		raid_ch->module_channel = raid_bdev->module->get_io_channel(raid_bdev);
		if (!raid_ch->module_channel) {
			SPDK_ERRLOG("Unable to create io channel for raid module\n");
			goto err;
		}
	}

	if (raid_bdev->process != NULL) {                       /* [한국어] background process 진행 중이면 가상 채널 구성. */
		ret = raid_bdev_ch_process_setup(raid_ch, raid_bdev->process);
		if (ret != 0) {
			SPDK_ERRLOG("Failed to setup process io channel\n");
			goto err;
		}
	} else {
		raid_ch->process.offset = RAID_OFFSET_BLOCKS_INVALID;   /* [한국어] process 없음 — fast path 분기 건너뛰도록 표시. */
	}

	return 0;
err:
	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {       /* [한국어] 부분 할당된 base 채널 롤백. */
		if (raid_ch->base_channel[i] != NULL) {
			spdk_put_io_channel(raid_ch->base_channel[i]);
		}
	}
	free(raid_ch->base_channel);

	raid_bdev_ch_process_cleanup(raid_ch);                  /* [한국어] process 자원도 함께 롤백 (target_ch / ch_processed 가 잡혔을 수 있음). */

	return ret;
}

/*
 * brief:
 * raid_bdev_destroy_cb function is a cb function for raid bdev which deletes the
 * hierarchy from raid bdev to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to raid bdev io device represented by raid_bdev
 * ctx_buf - pointer to context buffer for raid bdev io channel
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_destroy_cb - 마지막 spdk_put_io_channel 시점에 호출되는 채널 소멸 콜백.
 *
 * @io_device: raid_bdev.
 * @ctx_buf:   raid_bdev_io_channel (해제 직전).
 *
 * 동기: spdk_io_device_unregister 와 결합되어 채널 자원을 모두 반환. base_channel /
 * module_channel / process 가상 채널 모두 정리.
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인: spdk_put_io_channel (참조 0) → SPDK thread 레이어 → [raid_bdev_destroy_cb].
 */
static void
raid_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct raid_bdev *raid_bdev = io_device;
	struct raid_bdev_io_channel *raid_ch = ctx_buf;
	uint8_t i;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_destroy_cb\n");

	assert(raid_ch != NULL);
	assert(raid_ch->base_channel);

	if (raid_ch->module_channel) {                          /* [한국어] 모듈 채널이 있다면 먼저 해제. */
		spdk_put_io_channel(raid_ch->module_channel);
	}

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/* Free base bdev channels */
		if (raid_ch->base_channel[i] != NULL) {         /* [한국어] 잡혀있던 base 채널만 해제 (NULL = 미설정/process target). */
			spdk_put_io_channel(raid_ch->base_channel[i]);
		}
	}
	free(raid_ch->base_channel);                            /* [한국어] 배열 자체 free. */
	raid_ch->base_channel = NULL;

	raid_bdev_ch_process_cleanup(raid_ch);                  /* [한국어] process 가상 채널 정리. */
}

/*
 * brief:
 * raid_bdev_cleanup is used to cleanup raid_bdev related data
 * structures.
 * params:
 * raid_bdev - pointer to raid_bdev
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_cleanup - raid_bdev 의 base_info name 들과 전역 리스트 등록 해제.
 *
 * @raid_bdev: 정리할 raid_bdev.
 *
 * 동기: deconfigure 후 모든 base bdev 가 해제된 시점에 호출. 메모리는 free 하지 않고
 * (= raid_bdev_free 가 별도) name 문자열과 전역 리스트 노드만 정리.
 * 실행 컨텍스트: app thread (assert).
 *
 * 호출 체인: raid_bdev_io_device_unregister_cb / raid_bdev_cleanup_and_free → [_cleanup].
 */
static void
raid_bdev_cleanup(struct raid_bdev *raid_bdev)
{
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_cleanup, %p name %s, state %s\n",
		      raid_bdev, raid_bdev->bdev.name, raid_bdev_state_to_str(raid_bdev->state));
	assert(raid_bdev->state != RAID_BDEV_STATE_ONLINE);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		assert(base_info->desc == NULL);
		free(base_info->name);
	}

	TAILQ_REMOVE(&g_raid_bdev_list, raid_bdev, global_link);
}

/*
 * [한국어]
 * raid_bdev_free - raid_bdev 의 모든 메모리 free.
 *
 * @raid_bdev: 해제할 객체.
 *
 * 동기: 슈퍼블록 + base_info 배열 + name + raid_bdev 본체 순으로 해제.
 * 호출 컨텍스트: _raid_bdev_create 실패 롤백, raid_bdev_cleanup_and_free,
 *               raid_bdev_io_device_unregister_cb 의 마지막 단계.
 */
static void
raid_bdev_free(struct raid_bdev *raid_bdev)
{
	raid_bdev_free_superblock(raid_bdev);    /* [한국어] sb 가 있다면 spdk_dma_free. */
	free(raid_bdev->base_bdev_info);         /* [한국어] num_base_bdevs 크기의 배열. */
	free(raid_bdev->bdev.name);              /* [한국어] strdup 된 이름. */
	free(raid_bdev);
}

/*
 * [한국어]
 * raid_bdev_cleanup_and_free - cleanup 과 free 의 편의 결합.
 *
 * 동기: raid_bdev_exit / raid_bdev_delete 등 "이미 base 가 다 빠진 경우" 의 일괄 정리.
 */
static void
raid_bdev_cleanup_and_free(struct raid_bdev *raid_bdev)
{
	raid_bdev_cleanup(raid_bdev);    /* [한국어] base_info name + 전역 리스트 정리. */
	raid_bdev_free(raid_bdev);       /* [한국어] 메모리 free. */
}

/*
 * [한국어]
 * raid_bdev_deconfigure_base_bdev - base bdev 1 개를 "설정 해제" 상태로 표시.
 *
 * @base_info: 해제할 base_info.
 *
 * 동기: 멤버에서 제외 → discovered 카운터 감소 + is_configured/is_process_target false.
 * 자원 (desc, app_thread_ch) 해제는 호출자가 별도로.
 */
static void
raid_bdev_deconfigure_base_bdev(struct raid_base_bdev_info *base_info)
{
	struct raid_bdev *raid_bdev = base_info->raid_bdev;

	assert(base_info->is_configured);
	assert(raid_bdev->num_base_bdevs_discovered);
	raid_bdev->num_base_bdevs_discovered--;          /* [한국어] discovered 카운터 감소 — configured 와 일치 유지. */
	base_info->is_configured = false;
	base_info->is_process_target = false;             /* [한국어] rebuild target 이었다면 함께 해제. */
}

/*
 * brief:
 * free resource of base bdev for raid bdev
 * params:
 * base_info - raid base bdev info
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_free_base_bdev_resource - base bdev 1 슬롯의 모든 자원 (desc/ch/name) 해제.
 *
 * @base_info: 해제할 base_info.
 *
 * 동기: base bdev 제거 / RAID 삭제 / 셧다운 경로의 공통 마무리.
 * 동작:
 *   1) name free + null 화.
 *   2) state != CONFIGURING 이면 uuid 도 null 화 (구성 도중에는 uuid 보존해야 examine 가능).
 *   3) is_failed false 로 리셋.
 *   4) data_offset 0 으로 (재구성 시 다시 계산되도록).
 *   5) desc 가 있으면 spdk_bdev_module_release_bdev + spdk_bdev_close + put app_thread_ch.
 *   6) is_configured 였다면 raid_bdev_deconfigure_base_bdev 호출.
 * 실행 컨텍스트: app thread (assert).
 */
static void
raid_bdev_free_base_bdev_resource(struct raid_base_bdev_info *base_info)
{
	struct raid_bdev *raid_bdev = base_info->raid_bdev;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	assert(base_info->configure_cb == NULL);

	free(base_info->name);
	base_info->name = NULL;
	if (raid_bdev->state != RAID_BDEV_STATE_CONFIGURING) {
		spdk_uuid_set_null(&base_info->uuid);
	}
	base_info->is_failed = false;

	/* clear `data_offset` to allow it to be recalculated during configuration */
	base_info->data_offset = 0;

	if (base_info->desc == NULL) {
		return;
	}

	spdk_bdev_module_release_bdev(spdk_bdev_desc_get_bdev(base_info->desc));
	spdk_bdev_close(base_info->desc);
	base_info->desc = NULL;
	spdk_put_io_channel(base_info->app_thread_ch);
	base_info->app_thread_ch = NULL;

	if (base_info->is_configured) {
		raid_bdev_deconfigure_base_bdev(base_info);
	}
}

/*
 * [한국어]
 * raid_bdev_io_device_unregister_cb - io_device unregister 가 완료된 시점의 콜백.
 *
 * @io_device: raid_bdev.
 *
 * 동기: spdk_io_device_unregister 는 모든 채널이 닫힌 뒤 비동기로 이 콜백을 호출. 이
 * 시점에 모든 base bdev 가 빠졌다면 raid_bdev 까지 free, 아니면 bdev 만 destruct 완료
 * 통지하고 raid_bdev 자체는 남겨둔다 (개별 base bdev 들이 나중에 빠지면 그때 free).
 * 실행 컨텍스트: app thread.
 */
static void
raid_bdev_io_device_unregister_cb(void *io_device)
{
	struct raid_bdev *raid_bdev = io_device;

	if (raid_bdev->num_base_bdevs_discovered == 0) {
		/* Free raid_bdev when there are no base bdevs left */
		SPDK_DEBUGLOG(bdev_raid, "raid bdev base bdevs is 0, going to free all in destruct\n");
		raid_bdev_cleanup(raid_bdev);                            /* [한국어] 전역 리스트에서 제거. */
		spdk_bdev_destruct_done(&raid_bdev->bdev, 0);            /* [한국어] bdev 레이어에 "destruct 완료" 통지. */
		raid_bdev_free(raid_bdev);                               /* [한국어] 메모리 free. */
	} else {
		spdk_bdev_destruct_done(&raid_bdev->bdev, 0);            /* [한국어] base bdev 가 남아 있으면 free 보류. */
	}
}

/*
 * [한국어]
 * raid_bdev_module_stop_done - 개별 RAID 모듈이 stop 콜백 완료 후 호출하는 후속 단계.
 *
 * @raid_bdev: 정리 대상.
 *
 * 동기: module->stop 이 async 인 경우 (raid5f 등) 완료 시 이 함수로 unregister 진행.
 * CONFIGURING 상태에서는 io_device 가 아직 등록되지 않았으므로 unregister 안 함.
 */
void
raid_bdev_module_stop_done(struct raid_bdev *raid_bdev)
{
	if (raid_bdev->state != RAID_BDEV_STATE_CONFIGURING) {
		spdk_io_device_unregister(raid_bdev, raid_bdev_io_device_unregister_cb);  /* [한국어] 비동기 unregister 시작. */
	}
}

/*
 * [한국어]
 * _raid_bdev_destruct - bdev 레이어의 destruct 콜백 본체 (app thread 에서 실행).
 *
 * @ctxt: raid_bdev.
 *
 * 동기: bdev 레이어가 spdk_bdev_unregister 처리 중 fn_table.destruct 를 호출하면
 * raid_bdev_destruct → spdk_thread_exec_msg(app_thread, _raid_bdev_destruct) 로 위임된다.
 * 동작:
 *   1) g_shutdown_started 또는 remove_scheduled 인 base 들은 자원 해제.
 *   2) shutdown 이면 state = OFFLINE.
 *   3) module->stop 호출. async 모듈은 false 반환 → 이후 raid_bdev_module_stop_done 에서 마무리.
 *   4) sync 모듈 (true) 또는 stop NULL → 즉시 raid_bdev_module_stop_done.
 */
static void
_raid_bdev_destruct(void *ctxt)
{
	struct raid_bdev *raid_bdev = ctxt;
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_destruct\n");

	assert(raid_bdev->process == NULL);

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		/*
		 * Close all base bdev descriptors for which call has come from below
		 * layers.  Also close the descriptors if we have started shutdown.
		 */
		if (g_shutdown_started || base_info->remove_scheduled == true) {
			raid_bdev_free_base_bdev_resource(base_info);
		}
	}

	if (g_shutdown_started) {
		raid_bdev->state = RAID_BDEV_STATE_OFFLINE;
	}

	if (raid_bdev->module->stop != NULL) {
		if (raid_bdev->module->stop(raid_bdev) == false) {
			return;
		}
	}

	raid_bdev_module_stop_done(raid_bdev);
}

/*
 * [한국어]
 * raid_bdev_destruct - spdk_bdev_fn_table.destruct 진입점.
 *
 * @ctx: raid_bdev.
 * @return: 1 = "async destruct" 표시. 실제 완료는 spdk_bdev_destruct_done 으로 통지.
 *
 * 동기: bdev 레이어가 unregister 시 호출. 반드시 app thread 에서 처리해야 하므로
 * spdk_thread_exec_msg 로 위임 (현재 thread 가 app thread 면 즉시 실행).
 * 반환 1 은 "비동기 처리 중" 임을 bdev 레이어에 알려 추가 호출을 막는다.
 */
static int
raid_bdev_destruct(void *ctx)
{
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), _raid_bdev_destruct, ctx);

	return 1;
}

/*
 * [한국어]
 * raid_bdev_remap_dix_reftag - DIX (separate metadata) 의 reference tag 를 base bdev 의
 *                              실제 LBA 기준으로 remap.
 *
 * @md_buf: separate metadata 버퍼.
 * @num_blocks: 블록 수.
 * @bdev: base bdev.
 * @remapped_offset: base bdev 상의 실제 LBA.
 * @return: 0 성공, 음수 errno (DIF 초기화 또는 remap 실패).
 *
 * 동기: RAID 가 virtual LBA 를 base bdev 의 다른 LBA 로 매핑하므로 DIF reference tag 도
 * 그에 맞춰 재계산 필요 (NVMe / T10 DIF 스펙). 그렇지 않으면 base bdev 가 reftag 불일치
 * 로 read/write 를 거부.
 * 실행 컨텍스트: I/O fast path (RAID 모듈이 base I/O 전 호출).
 */
int
raid_bdev_remap_dix_reftag(void *md_buf, uint64_t num_blocks,
			   struct spdk_bdev *bdev, uint32_t remapped_offset)
{
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_error err_blk = {};
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct iovec md_iov = {
		.iov_base	= md_buf,
		.iov_len	= num_blocks * bdev->md_len,
	};

	if (md_buf == NULL) {
		return 0;
	}

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = bdev->dif_pi_format;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen, bdev->md_len, bdev->md_interleave,
			       bdev->dif_is_head_of_md, bdev->dif_type,
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF context failed\n");
		return rc;
	}

	spdk_dif_ctx_set_remapped_init_ref_tag(&dif_ctx, remapped_offset);

	rc = spdk_dix_remap_ref_tag(&md_iov, num_blocks, &dif_ctx, &err_blk, false);
	if (rc != 0) {
		SPDK_ERRLOG("Remapping reference tag failed. type=%d, offset=%d"
			    PRIu32 "\n", err_blk.err_type, err_blk.err_offset);
	}

	return rc;
}

/*
 * [한국어]
 * raid_bdev_verify_dix_reftag - DIX reference tag 를 검증 (raid virtual LBA 기준).
 *
 * @iovs/@iovcnt: 데이터 버퍼.
 * @md_buf: separate metadata 버퍼.
 * @num_blocks: 블록 수.
 * @bdev: 검증 기준 bdev (raid 자신 일 수도, base 일 수도).
 * @offset_blocks: 검증 기준 LBA (raid virtual).
 * @return: 0 성공, 음수 errno.
 *
 * 동기: base bdev 에서 읽어온 후 라이언트로 돌려주기 전, reftag 가 raid 의 virtual LBA
 * 기준과 일치하는지 확인. RAID 모듈이 base → raid 방향 변환 시 사용.
 */
int
raid_bdev_verify_dix_reftag(struct iovec *iovs, int iovcnt, void *md_buf,
			    uint64_t num_blocks, struct spdk_bdev *bdev, uint32_t offset_blocks)
{
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_error err_blk = {};
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct iovec md_iov = {
		.iov_base	= md_buf,
		.iov_len	= num_blocks * bdev->md_len,
	};

	if (md_buf == NULL) {
		return 0;
	}

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = bdev->dif_pi_format;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen, bdev->md_len, bdev->md_interleave,
			       bdev->dif_is_head_of_md, bdev->dif_type,
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       offset_blocks, 0, 0, 0, 0, &dif_opts);
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF context failed\n");
		return rc;
	}

	rc = spdk_dix_verify(iovs, iovcnt, &md_iov, num_blocks, &dif_ctx, &err_blk);
	if (rc != 0) {
		SPDK_ERRLOG("Reference tag check failed. type=%d, offset=%d"
			    PRIu32 "\n", err_blk.err_type, err_blk.err_offset);
	}

	return rc;
}

/*
 * [한국어]
 * raid_bdev_io_complete - raid_io 의 최종 완료. split 복원 + DIF remap + bdev 레이어 완료.
 *
 * @raid_io: 완료할 raid_io.
 * @status: SPDK_BDEV_IO_STATUS_SUCCESS / _FAILED / _ABORTED 등.
 *
 * 동기: RAID 모듈이 (혹은 raid_bdev_io_complete_part 의 fan-in 결과) raid_io 의 완료를
 * 통지. split 된 IO 라면 두 번째 부분도 발행하거나 첫 번째 split 만 끝나면 추가 처리.
 * 동작:
 *   1) trace 기록.
 *   2) split 상태면: 첫 split (offset != 0) 완료 시 두 번째 split 발행 (이때 raid_ch 를
 *      ch_processed 로 교체 = 처리 완료 영역 mirror), 두 번째 split 완료 시 원본 iovs 복원.
 *   3) completion_cb 가 있으면 그 콜백으로 위임 (RAID 모듈 내부 후처리).
 *   4) READ + DIF + REFTAG_CHECK + SUCCESS 라면 reftag remap.
 *   5) spdk_bdev_io_complete 호출 → bdev 레이어 → 애플리케이션 콜백.
 * 실행 컨텍스트: 채널 소유 스레드.
 */
void
raid_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
	int rc;

	spdk_trace_record(TRACE_BDEV_RAID_IO_DONE, 0, 0, (uintptr_t)raid_io, (uintptr_t)bdev_io);

	if (raid_io->split.offset != RAID_OFFSET_BLOCKS_INVALID) {
		struct iovec *split_iov = raid_io->split.iov;
		const struct iovec *split_iov_orig = &raid_io->split.iov_copy;

		/*
		 * Non-zero offset here means that this is the completion of the first part of the
		 * split I/O (the higher LBAs). Then, we submit the second part and set offset to 0.
		 */
		if (raid_io->split.offset != 0) {
			raid_io->offset_blocks = bdev_io->u.bdev.offset_blocks;
			raid_io->md_buf = bdev_io->u.bdev.md_buf;

			if (status == SPDK_BDEV_IO_STATUS_SUCCESS) {
				raid_io->num_blocks = raid_io->split.offset;
				raid_io->iovcnt = raid_io->iovs - bdev_io->u.bdev.iovs;
				raid_io->iovs = bdev_io->u.bdev.iovs;
				if (split_iov != NULL) {
					raid_io->iovcnt++;
					split_iov->iov_len = split_iov->iov_base - split_iov_orig->iov_base;
					split_iov->iov_base = split_iov_orig->iov_base;
				}

				raid_io->split.offset = 0;
				raid_io->base_bdev_io_submitted = 0;
				raid_io->raid_ch = raid_io->raid_ch->process.ch_processed;

				switch (bdev_io->type) {
				case SPDK_BDEV_IO_TYPE_READ:
				case SPDK_BDEV_IO_TYPE_WRITE:
					raid_io->raid_bdev->module->submit_rw_request(raid_io);
					break;

				case SPDK_BDEV_IO_TYPE_FLUSH:
				case SPDK_BDEV_IO_TYPE_UNMAP:
					raid_io->raid_bdev->module->submit_null_payload_request(raid_io);
					break;
				default:
					SPDK_ERRLOG("io type %u should not happen split\n", bdev_io->type);
					raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
					break;
				}
				return;
			}
		}

		raid_io->num_blocks = bdev_io->u.bdev.num_blocks;
		raid_io->iovcnt = bdev_io->u.bdev.iovcnt;
		raid_io->iovs = bdev_io->u.bdev.iovs;
		if (split_iov != NULL) {
			*split_iov = *split_iov_orig;
		}
	}

	if (spdk_unlikely(raid_io->completion_cb != NULL)) {
		raid_io->completion_cb(raid_io, status);
	} else {
		if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_READ &&
				  spdk_bdev_get_dif_type(bdev_io->bdev) != SPDK_DIF_DISABLE &&
				  bdev_io->bdev->dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK &&
				  status == SPDK_BDEV_IO_STATUS_SUCCESS)) {

			rc = raid_bdev_remap_dix_reftag(bdev_io->u.bdev.md_buf,
							bdev_io->u.bdev.num_blocks, bdev_io->bdev,
							bdev_io->u.bdev.offset_blocks);
			if (rc != 0) {
				status = SPDK_BDEV_IO_STATUS_FAILED;
			}
		}
		spdk_bdev_io_complete(bdev_io, status);
	}
}

/*
 * brief:
 * raid_bdev_io_complete_part - signal the completion of a part of the expected
 * base bdev IOs and complete the raid_io if this is the final expected IO.
 * The caller should first set raid_io->base_bdev_io_remaining. This function
 * will decrement this counter by the value of the 'completed' parameter and
 * complete the raid_io if the counter reaches 0. The caller is free to
 * interpret the 'base_bdev_io_remaining' and 'completed' values as needed,
 * it can represent e.g. blocks or IOs.
 * params:
 * raid_io - pointer to raid_bdev_io
 * completed - the part of the raid_io that has been completed
 * status - status of the base IO
 * returns:
 * true - if the raid_io is completed
 * false - otherwise
 */
/*
 * [한국어]
 * raid_bdev_io_complete_part - 여러 base bdev I/O 의 부분 완료를 누적하고 0 에 도달하면 최종 완료.
 *
 * @raid_io: 누적 대상 raid_io.
 * @completed: 이번에 완료된 양 (블록 또는 IO 단위 — RAID 모듈이 의미를 결정).
 * @status: 이번 부분의 status. base_bdev_io_status_default 와 다르면 raid_io 의 status 갱신.
 * @return: true = 최종 완료 도달, false = 아직 남음.
 *
 * 동기: RAID 가 N way fan-out (스트라이프/미러) 한 후 fan-in 완료 카운터. lock-free
 * 누적 — raid_io 는 단일 채널 단일 스레드에서만 다루므로 락 불필요.
 * 호출자: RAID 레벨 모듈의 base IO completion 콜백.
 */
bool
raid_bdev_io_complete_part(struct raid_bdev_io *raid_io, uint64_t completed,
			   enum spdk_bdev_io_status status)
{
	assert(raid_io->base_bdev_io_remaining >= completed);
	raid_io->base_bdev_io_remaining -= completed;

	if (status != raid_io->base_bdev_io_status_default) {
		raid_io->base_bdev_io_status = status;
	}

	if (raid_io->base_bdev_io_remaining == 0) {
		raid_bdev_io_complete(raid_io, raid_io->base_bdev_io_status);
		return true;
	} else {
		return false;
	}
}

/*
 * brief:
 * raid_bdev_queue_io_wait function processes the IO which failed to submit.
 * It will try to queue the IOs after storing the context to bdev wait queue logic.
 * params:
 * raid_io - pointer to raid_bdev_io
 * bdev - the block device that the IO is submitted to
 * ch - io channel
 * cb_fn - callback when the spdk_bdev_io for bdev becomes available
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_queue_io_wait - base bdev 의 -ENOMEM 시 자원이 가용해질 때 재시도 등록.
 *
 * @raid_io: 재시도할 raid_io.
 * @bdev: -ENOMEM 을 반환한 bdev.
 * @ch: 해당 채널.
 * @cb_fn: bdev 자원 가용 시 호출될 콜백 (보통 submit 재시도 함수).
 *
 * 동기: SPDK bdev 레이어의 표준 백프레셔 메커니즘. bdev 가 io_wait queue 로 진입시켜
 * spdk_bdev_io 가 free 될 때 cb_fn 을 호출한다.
 */
void
raid_bdev_queue_io_wait(struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn)
{
	raid_io->waitq_entry.bdev = bdev;
	raid_io->waitq_entry.cb_fn = cb_fn;
	raid_io->waitq_entry.cb_arg = raid_io;
	raid_io->waitq_entry.dep_unblock = true;

	spdk_bdev_queue_io_wait(bdev, ch, &raid_io->waitq_entry);
}

/*
 * [한국어]
 * raid_base_bdev_reset_complete - base bdev 의 spdk_bdev_reset 완료 콜백.
 *
 * @bdev_io: base bdev 의 reset bdev_io.
 * @success: true = 성공.
 * @cb_arg: 원본 raid_io.
 *
 * 동기: RESET 은 fan-out 으로 모든 base bdev 에 발행되므로 1 개 완료 시 partial 누적.
 */
static void
raid_base_bdev_reset_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);             /* [한국어] base 의 bdev_io 반환. */

	raid_bdev_io_complete_part(raid_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);   /* [한국어] 1 단위 완료 — 모두 끝나면 raid 레이어 완료. */
}

static void raid_bdev_submit_reset_request(struct raid_bdev_io *raid_io);
/* [한국어] forward decl — wait queue 재시도 콜백에서 사용. */

/*
 * [한국어]
 * _raid_bdev_submit_reset_request - wait queue 가 자원 가용 시 호출하는 어댑터.
 *
 * @_raid_io: 재시도할 raid_io (void * 시그니처 변환용).
 */
static void
_raid_bdev_submit_reset_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid_bdev_submit_reset_request(raid_io);
}

/*
 * brief:
 * raid_bdev_submit_reset_request function submits reset requests
 * to member disks; it will submit as many as possible unless a reset fails with -ENOMEM, in
 * which case it will queue it for later submission
 * params:
 * raid_io
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_submit_reset_request - 모든 base bdev 에 RESET fan-out.
 *
 * @raid_io: RESET 형 raid_io.
 *
 * 동기: NVMe RESET (Format / Sanitize 등 long op) 은 멤버 디스크 전부에 발행해야 한다.
 * -ENOMEM 시 queue_io_wait 로 부분 재시도 (이미 발행된 IO 는 그대로 진행).
 */
static void
raid_bdev_submit_reset_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev		*raid_bdev;
	int				ret;
	uint8_t				i;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	raid_bdev = raid_io->raid_bdev;

	if (raid_io->base_bdev_io_remaining == 0) {
		raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs;
	}

	for (i = raid_io->base_bdev_io_submitted; i < raid_bdev->num_base_bdevs; i++) {
		base_info = &raid_bdev->base_bdev_info[i];
		base_ch = raid_io->raid_ch->base_channel[i];
		if (base_ch == NULL) {
			raid_io->base_bdev_io_submitted++;
			raid_bdev_io_complete_part(raid_io, 1, SPDK_BDEV_IO_STATUS_SUCCESS);
			continue;
		}
		ret = spdk_bdev_reset(base_info->desc, base_ch,
				      raid_base_bdev_reset_complete, raid_io);
		if (ret == 0) {
			raid_io->base_bdev_io_submitted++;
		} else if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
						base_ch, _raid_bdev_submit_reset_request);
			return;
		} else {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}
}

/*
 * [한국어]
 * raid_bdev_io_split - raid_io 를 split_offset 기준으로 둘로 나눠 첫 부분만 즉시 처리하도록 조정.
 *
 * @raid_io: 분할 대상.
 * @split_offset: virtual LBA 의 분할점 (raid_io 시작 기준 상대 블록 수).
 *
 * 동기: process 가 진행 중인 영역과 미진행 영역에 걸친 IO 는 두 번에 나눠 발행해야 한다.
 * 첫 부분 (split_offset 만큼) 만 발행하고, 나머지는 raid_bdev_io_complete 에서 두 번째 부분
 * 으로 재발행. iovs 와 num_blocks/offset_blocks/md_buf 를 조정하며, iov 경계가 split 점과
 * 어긋나면 split.iov 에 원본 백업 저장 후 잘라낸다.
 * 실행 컨텍스트: 채널 스레드 (submit path).
 */
static void
raid_bdev_io_split(struct raid_bdev_io *raid_io, uint64_t split_offset)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	size_t iov_offset = split_offset * raid_bdev->bdev.blocklen;
	int i;

	assert(split_offset != 0);
	assert(raid_io->split.offset == RAID_OFFSET_BLOCKS_INVALID);
	raid_io->split.offset = split_offset;

	raid_io->offset_blocks += split_offset;
	raid_io->num_blocks -= split_offset;
	if (raid_io->md_buf != NULL) {
		raid_io->md_buf += (split_offset * raid_bdev->bdev.md_len);
	}

	for (i = 0; i < raid_io->iovcnt; i++) {
		struct iovec *iov = &raid_io->iovs[i];

		if (iov_offset < iov->iov_len) {
			if (iov_offset == 0) {
				raid_io->split.iov = NULL;
			} else {
				raid_io->split.iov = iov;
				raid_io->split.iov_copy = *iov;
				iov->iov_base += iov_offset;
				iov->iov_len -= iov_offset;
			}
			raid_io->iovs += i;
			raid_io->iovcnt -= i;
			break;
		}

		iov_offset -= iov->iov_len;
	}
}

/*
 * [한국어]
 * _raid_bdev_request_split - submit 직전 process 진행 영역에 대한 split / 채널 스왑 결정.
 *
 * @raid_io: submit 직전의 raid_io.
 *
 * 동기: process 진행 영역과 IO 범위 관계에 따라 3 가지 동작:
 *   - 진행 영역에만 들어감 → raid_ch 를 ch_processed (가상) 로 교체.
 *   - 미진행 영역에만 → 그대로.
 *   - 걸쳐 있음 → split 하여 미진행 영역만 먼저 발행 (나머지는 complete 시 재발행).
 * 실행 컨텍스트: I/O fast path. process.offset == INVALID 이면 early return → 오버헤드 X.
 */
static inline void
_raid_bdev_request_split(struct raid_bdev_io *raid_io)
{
	struct raid_bdev_io_channel *raid_ch = raid_io->raid_ch;

	if (raid_ch->process.offset != RAID_OFFSET_BLOCKS_INVALID) {
		uint64_t offset_begin = raid_io->offset_blocks;
		uint64_t offset_end = offset_begin + raid_io->num_blocks;

		if (offset_end > raid_ch->process.offset) {
			if (offset_begin < raid_ch->process.offset) {
				/*
				 * If the I/O spans both the processed and unprocessed ranges,
				 * split it and first handle the unprocessed part. After it
				 * completes, the rest will be handled.
				 * This situation occurs when the process thread is not active
				 * or is waiting for the process window range to be locked
				 * (quiesced). When a window is being processed, such I/Os will be
				 * deferred by the bdev layer until the window is unlocked.
				 */
				SPDK_DEBUGLOG(bdev_raid, "split: process_offset: %lu offset_begin: %lu offset_end: %lu\n",
					      raid_ch->process.offset, offset_begin, offset_end);
				raid_bdev_io_split(raid_io, raid_ch->process.offset - offset_begin);
			}
		} else {
			/* Use the child channel, which corresponds to the already processed range */
			raid_io->raid_ch = raid_ch->process.ch_processed;
		}
	}
}

/*
 * [한국어]
 * raid_bdev_submit_rw_request - READ/WRITE 의 RAID 모듈 디스패치 래퍼.
 *
 * @raid_io: 발행할 raid_io.
 *
 * 동기: split 결정 후 RAID 레벨 모듈 (raid0/1/5f/concat) 의 submit_rw_request 콜백 호출.
 */
static void
raid_bdev_submit_rw_request(struct raid_bdev_io *raid_io)
{
	_raid_bdev_request_split(raid_io);                              /* [한국어] process 영역 보정. */
	raid_io->raid_bdev->module->submit_rw_request(raid_io);         /* [한국어] 모듈로 디스패치. */
}

/*
 * [한국어]
 * raid_bdev_submit_null_payload_request - FLUSH/UNMAP 의 RAID 모듈 디스패치 래퍼.
 *
 * 동기: 데이터 페이로드 없는 op. 모듈이 submit_null_payload_request 를 정의해야 지원.
 */
static void
raid_bdev_submit_null_payload_request(struct raid_bdev_io *raid_io)
{
	_raid_bdev_request_split(raid_io);
	raid_io->raid_bdev->module->submit_null_payload_request(raid_io);
}

/*
 * brief:
 * Callback function to spdk_bdev_io_get_buf.
 * params:
 * ch - pointer to raid bdev io channel
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * success - True if buffer is allocated or false otherwise.
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_get_buf_cb - spdk_bdev_io_get_buf 의 완료 콜백 (READ 경로).
 *
 * @ch: raid 채널.
 * @bdev_io: raid 의 bdev_io.
 * @success: true = 버퍼 할당 성공.
 *
 * 동기: READ 는 bdev_io 가 자체 버퍼를 가지지 않을 수 있어 bdev 레이어가 풀에서 버퍼를
 * 잡아준 후 이 콜백을 호출. 여기서 raid_io 의 iovs/iovcnt/md_buf 를 갱신하고 RW 발행.
 */
static void
raid_bdev_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		     bool success)
{
	struct raid_bdev_io *raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;

	if (!success) {
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	raid_io->iovs = bdev_io->u.bdev.iovs;
	raid_io->iovcnt = bdev_io->u.bdev.iovcnt;
	raid_io->md_buf = bdev_io->u.bdev.md_buf;

	raid_bdev_submit_rw_request(raid_io);
}

/*
 * [한국어]
 * raid_bdev_io_init - raid_bdev_io 객체 초기화 (submit_request 의 첫 단계).
 *
 * @raid_io: 초기화 대상 (bdev_io->driver_ctx).
 * @raid_ch: 호출 채널.
 * @type/offset/num_blocks/iovs/iovcnt/md_buf/memory_domain/memory_domain_ctx: 원본 bdev_io 의 IO 파라미터.
 *
 * 동기: bdev_io 의 unstructured 필드를 raid_io 구조체로 정규화하여 RAID 모듈이 일관된
 * 인터페이스로 접근하게 한다. base_bdev_io_remaining/submitted 카운터 초기화, split.offset
 * INVALID 마킹, default status SUCCESS 설정.
 */
void
raid_bdev_io_init(struct raid_bdev_io *raid_io, struct raid_bdev_io_channel *raid_ch,
		  enum spdk_bdev_io_type type, uint64_t offset_blocks,
		  uint64_t num_blocks, struct iovec *iovs, int iovcnt, void *md_buf,
		  struct spdk_memory_domain *memory_domain, void *memory_domain_ctx)
{
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(raid_ch);
	struct raid_bdev *raid_bdev = spdk_io_channel_get_io_device(ch);

	raid_io->type = type;
	raid_io->offset_blocks = offset_blocks;
	raid_io->num_blocks = num_blocks;
	raid_io->iovs = iovs;
	raid_io->iovcnt = iovcnt;
	raid_io->memory_domain = memory_domain;
	raid_io->memory_domain_ctx = memory_domain_ctx;
	raid_io->md_buf = md_buf;

	raid_io->raid_bdev = raid_bdev;
	raid_io->raid_ch = raid_ch;
	raid_io->base_bdev_io_remaining = 0;
	raid_io->base_bdev_io_submitted = 0;
	raid_io->completion_cb = NULL;
	raid_io->split.offset = RAID_OFFSET_BLOCKS_INVALID;

	raid_bdev_io_set_default_status(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

/*
 * brief:
 * raid_bdev_submit_request function is the submit_request function pointer of
 * raid bdev function table. This is used to submit the io on raid_bdev to below
 * layers.
 * params:
 * ch - pointer to raid bdev io channel
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_submit_request - bdev_fn_table.submit_request 진입점. bdev_io → raid_io 변환 + 디스패치.
 *
 * @ch: raid 채널.
 * @bdev_io: bdev 레이어가 만든 IO 객체. raid_io 는 bdev_io->driver_ctx 영역에 in-place.
 *
 * 동작:
 *   1) raid_bdev_io_init 으로 raid_io 채움.
 *   2) trace 기록 (TRACE_BDEV_RAID_IO_START).
 *   3) IO type 별 분기:
 *      - READ → spdk_bdev_io_get_buf (버퍼 풀에서 잡고 raid_bdev_get_buf_cb 에서 발행).
 *      - WRITE → 즉시 raid_bdev_submit_rw_request.
 *      - RESET → raid_bdev_submit_reset_request (fan-out).
 *      - FLUSH/UNMAP → submit_null_payload_request.
 *      - 그 외 → FAILED 완료.
 * 실행 컨텍스트: 채널 스레드.
 */
static void
raid_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct raid_bdev_io *raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;

	raid_bdev_io_init(raid_io, spdk_io_channel_get_ctx(ch), bdev_io->type,
			  bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
			  bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.md_buf,
			  bdev_io->u.bdev.memory_domain, bdev_io->u.bdev.memory_domain_ctx);

	spdk_trace_record(TRACE_BDEV_RAID_IO_START, 0, 0, (uintptr_t)raid_io, (uintptr_t)bdev_io);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, raid_bdev_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		raid_bdev_submit_rw_request(raid_io);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		raid_bdev_submit_reset_request(raid_io);
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		raid_bdev_submit_null_payload_request(raid_io);
		break;

	default:
		SPDK_ERRLOG("submit request, invalid io type %u\n", bdev_io->type);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

/*
 * brief:
 * _raid_bdev_io_type_supported checks whether io_type is supported in
 * all base bdev modules of raid bdev module. If anyone among the base_bdevs
 * doesn't support, the raid device doesn't supports.
 *
 * params:
 * raid_bdev - pointer to raid bdev context
 * io_type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
/*
 * [한국어]
 * _raid_bdev_io_type_supported - 모든 base bdev 가 io_type 을 지원하는지 검사.
 *
 * @raid_bdev: RAID.
 * @io_type: 검사할 IO 타입.
 * @return: true = 전부 지원.
 *
 * 동기: RAID 는 멤버 디스크의 공통 분모만 지원할 수 있음. 1 개라도 미지원이면 false.
 */
inline static bool
_raid_bdev_io_type_supported(struct raid_bdev *raid_bdev, enum spdk_bdev_io_type io_type)
{
	struct raid_base_bdev_info *base_info;

	if (io_type == SPDK_BDEV_IO_TYPE_FLUSH ||
	    io_type == SPDK_BDEV_IO_TYPE_UNMAP) {
		if (raid_bdev->module->submit_null_payload_request == NULL) {
			return false;
		}
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->desc == NULL) {
			continue;
		}

		if (spdk_bdev_io_type_supported(spdk_bdev_desc_get_bdev(base_info->desc), io_type) == false) {
			return false;
		}
	}

	return true;
}

/*
 * brief:
 * raid_bdev_io_type_supported is the io_supported function for bdev function
 * table which returns whether the particular io type is supported or not by
 * raid bdev module
 * params:
 * ctx - pointer to raid bdev context
 * type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
/*
 * [한국어]
 * raid_bdev_io_type_supported - bdev_fn_table.io_type_supported 진입점.
 *
 * @ctx: raid_bdev.
 * @io_type: 검사할 타입.
 * @return: true = 지원.
 *
 * 동기: READ/WRITE 은 무조건 지원. FLUSH/RESET/UNMAP 은 모든 base bdev 가 지원해야만 true.
 */
static bool
raid_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return _raid_bdev_io_type_supported(ctx, io_type);

	default:
		return false;
	}

	return false;
}

/*
 * brief:
 * raid_bdev_get_io_channel is the get_io_channel function table pointer for
 * raid bdev. This is used to return the io channel for this raid bdev
 * params:
 * ctxt - pointer to raid_bdev
 * returns:
 * pointer to io channel for raid bdev
 */
/*
 * [한국어]
 * raid_bdev_get_io_channel - bdev_fn_table.get_io_channel 진입점.
 *
 * @ctxt: raid_bdev.
 * @return: 호출 thread 에 해당하는 spdk_io_channel.
 */
static struct spdk_io_channel *
raid_bdev_get_io_channel(void *ctxt)
{
	struct raid_bdev *raid_bdev = ctxt;

	return spdk_get_io_channel(raid_bdev);     /* [한국어] SPDK thread 레이어가 (필요 시) create_cb 호출. */
}

/*
 * [한국어]
 * raid_bdev_write_info_json - raid_bdev 의 상태/구성 정보를 JSON 으로 직렬화.
 *
 * @raid_bdev: 직렬화 대상.
 * @w: JSON writer ctx.
 *
 * 동기: dump_info_json 및 bdev_get_bdevs RPC 응답에서 사용. process 진행 중이면
 * process 객체 정보 (type/target/percent) 도 포함.
 * 실행 컨텍스트: app thread (assert).
 */
void
raid_bdev_write_info_json(struct raid_bdev *raid_bdev, struct spdk_json_write_ctx *w)
{
	struct raid_base_bdev_info *base_info;

	assert(raid_bdev != NULL);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	spdk_json_write_named_uuid(w, "uuid", &raid_bdev->bdev.uuid);
	spdk_json_write_named_uint32(w, "strip_size_kb", raid_bdev->strip_size_kb);
	spdk_json_write_named_string(w, "state", raid_bdev_state_to_str(raid_bdev->state));
	spdk_json_write_named_string(w, "raid_level", raid_bdev_level_to_str(raid_bdev->level));
	spdk_json_write_named_bool(w, "superblock", raid_bdev->superblock_enabled);
	spdk_json_write_named_uint32(w, "num_base_bdevs", raid_bdev->num_base_bdevs);
	spdk_json_write_named_uint32(w, "num_base_bdevs_discovered", raid_bdev->num_base_bdevs_discovered);
	spdk_json_write_named_uint32(w, "num_base_bdevs_operational",
				     raid_bdev->num_base_bdevs_operational);
	if (raid_bdev->process) {
		struct raid_bdev_process *process = raid_bdev->process;
		uint64_t offset = process->window_offset;

		spdk_json_write_named_object_begin(w, "process");
		spdk_json_write_name(w, "type");
		spdk_json_write_string(w, raid_bdev_process_to_str(process->type));
		spdk_json_write_named_string(w, "target", process->target->name);
		spdk_json_write_named_object_begin(w, "progress");
		spdk_json_write_named_uint64(w, "blocks", offset);
		spdk_json_write_named_uint32(w, "percent", offset * 100.0 / raid_bdev->bdev.blockcnt);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_name(w, "base_bdevs_list");
	spdk_json_write_array_begin(w);
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		spdk_json_write_object_begin(w);
		spdk_json_write_name(w, "name");
		if (base_info->name) {
			spdk_json_write_string(w, base_info->name);
		} else {
			spdk_json_write_null(w);
		}
		spdk_json_write_named_uuid(w, "uuid", &base_info->uuid);
		spdk_json_write_named_bool(w, "is_configured", base_info->is_configured);
		spdk_json_write_named_uint64(w, "data_offset", base_info->data_offset);
		spdk_json_write_named_uint64(w, "data_size", base_info->data_size);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
}

/*
 * brief:
 * raid_bdev_dump_info_json is the function table pointer for raid bdev
 * params:
 * ctx - pointer to raid_bdev
 * w - pointer to json context
 * returns:
 * 0 - success
 * non zero - failure
 */
/*
 * [한국어]
 * raid_bdev_dump_info_json - bdev_fn_table.dump_info_json 진입점.
 *
 * 동기: bdev_get_bdevs RPC 응답에서 driver_specific.raid 객체를 추가.
 */
static int
raid_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct raid_bdev *raid_bdev = ctx;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_dump_config_json\n");

	/* Dump the raid bdev configuration related information */
	spdk_json_write_named_object_begin(w, "raid");
	raid_bdev_write_info_json(raid_bdev, w);
	spdk_json_write_object_end(w);

	return 0;
}

/*
 * brief:
 * raid_bdev_write_config_json is the function table pointer for raid bdev
 * params:
 * bdev - pointer to spdk_bdev
 * w - pointer to json context
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_write_config_json - 셧다운 / save_config 시 호출되는 vtable. 재구성 RPC 를 JSON 으로 출력.
 *
 * @bdev: raid_bdev 의 spdk_bdev.
 * @w: JSON writer.
 *
 * 동기: spdk_save_config 또는 framework_get_config RPC 가 모든 bdev 를 순회하면서 호출.
 * superblock 이 활성화된 경우 슈퍼블록이 저장 매체가 되므로 JSON 출력 생략.
 * 출력 형식: { "method": "bdev_raid_create", "params": { name/uuid/strip_size_kb/raid_level/base_bdevs[] } }.
 */
static void
raid_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct raid_bdev *raid_bdev = bdev->ctxt;
	struct raid_base_bdev_info *base_info;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (raid_bdev->superblock_enabled) {
		/* raid bdev configuration is stored in the superblock */
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_raid_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uuid(w, "uuid", &raid_bdev->bdev.uuid);
	if (raid_bdev->strip_size_kb != 0) {
		spdk_json_write_named_uint32(w, "strip_size_kb", raid_bdev->strip_size_kb);
	}
	spdk_json_write_named_string(w, "raid_level", raid_bdev_level_to_str(raid_bdev->level));

	spdk_json_write_named_array_begin(w, "base_bdevs");
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->name) {
			spdk_json_write_string(w, base_info->name);
		} else {
			char str[32];

			snprintf(str, sizeof(str), "removed_base_bdev_%u", raid_bdev_base_bdev_slot(base_info));
			spdk_json_write_string(w, str);
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * raid_bdev_get_memory_domains - bdev_fn_table.get_memory_domains 진입점.
 *
 * @ctx: raid_bdev.
 * @domains: 출력 배열.
 * @array_size: 배열 크기.
 * @return: 총 도메인 수 (배열에 다 못 넣었어도 전체 수 반환). 음수면 errno.
 *
 * 동기: SPDK memory domain 은 RDMA / KMEM 같이 메모리 위치를 표현. 상위가 zero-copy 가능
 * 여부 판단에 사용. RAID 는 멤버 디스크의 도메인을 모두 수집해 반환.
 */
static int
raid_bdev_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct raid_bdev *raid_bdev = ctx;
	struct raid_base_bdev_info *base_info;
	int domains_count = 0, rc = 0;

	if (raid_bdev->module->memory_domains_supported == false) {
		return 0;
	}

	/* First loop to get the number of memory domains */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->is_configured == false) {
			continue;
		}
		rc = spdk_bdev_get_memory_domains(spdk_bdev_desc_get_bdev(base_info->desc), NULL, 0);
		if (rc < 0) {
			return rc;
		}
		domains_count += rc;
	}

	if (!domains || array_size < domains_count) {
		return domains_count;
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->is_configured == false) {
			continue;
		}
		rc = spdk_bdev_get_memory_domains(spdk_bdev_desc_get_bdev(base_info->desc), domains, array_size);
		if (rc < 0) {
			return rc;
		}
		domains += rc;
		array_size -= rc;
	}

	return domains_count;
}

/* g_raid_bdev_fn_table is the function table for raid bdev */
/* [한국어] RAID bdev 가 spdk_bdev 레이어에 등록하는 vtable. spdk_bdev_register 시
 * raid_bdev->bdev.fn_table = &g_raid_bdev_fn_table 로 연결되어 bdev 레이어가 RAID 의
 * 동작을 invoke 한다.
 * 멤버별 의미:
 * - destruct          : spdk_bdev_unregister 시 호출. async (return 1).
 * - submit_request    : 모든 IO 의 진입점.
 * - io_type_supported : READ/WRITE 외에 FLUSH/RESET/UNMAP 지원 여부.
 * - get_io_channel    : 호출 thread 의 채널 반환.
 * - dump_info_json    : bdev_get_bdevs RPC 응답에 RAID 정보 첨부.
 * - write_config_json : save_config 시 재구성 RPC 출력 (superblock 비활성 시).
 * - get_memory_domains: zero-copy 판정용 메모리 도메인 수집.
 * 미정의 멤버: io_msg, get_module_ctx, copy 등 — RAID 가 사용하지 않음. */
static const struct spdk_bdev_fn_table g_raid_bdev_fn_table = {
	.destruct		= raid_bdev_destruct,
	.submit_request		= raid_bdev_submit_request,
	.io_type_supported	= raid_bdev_io_type_supported,
	.get_io_channel		= raid_bdev_get_io_channel,
	.dump_info_json		= raid_bdev_dump_info_json,
	.write_config_json	= raid_bdev_write_config_json,
	.get_memory_domains	= raid_bdev_get_memory_domains,
};

/*
 * [한국어]
 * raid_bdev_find_by_name - 이름으로 raid_bdev 검색 (전역 리스트 선형 탐색).
 *
 * @name: 찾을 RAID bdev 이름.
 * @return: 매칭 raid_bdev *, 없으면 NULL.
 *
 * 동기: RPC 진입에서 사용자가 지정한 이름을 객체로 변환. 같은 이름 중복은 _raid_bdev_create
 * 시점에 차단되므로 결과는 유일.
 * 실행 컨텍스트: app thread.
 */
struct raid_bdev *
raid_bdev_find_by_name(const char *name)
{
	struct raid_bdev *raid_bdev;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		if (strcmp(raid_bdev->bdev.name, name) == 0) {
			return raid_bdev;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * raid_bdev_find_by_uuid - UUID 로 raid_bdev 검색.
 *
 * 동기: 슈퍼블록 examine 경로에서 sb->uuid 로 기존 RAID 객체 매칭.
 */
static struct raid_bdev *
raid_bdev_find_by_uuid(const struct spdk_uuid *uuid)
{
	struct raid_bdev *raid_bdev;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		if (spdk_uuid_compare(&raid_bdev->bdev.uuid, uuid) == 0) {
			return raid_bdev;
		}
	}

	return NULL;
}

/* [한국어] 사용자 입력 문자열 ↔ raid_level 매핑. 두 표기 ("raid1", "1") 모두 받음.
 * 마지막 sentinel { } 로 종료 (name == NULL). */
static struct {
	const char *name;
	enum raid_level value;
} g_raid_level_names[] = {
	{ "raid0", RAID0 },
	{ "0", RAID0 },
	{ "raid1", RAID1 },
	{ "1", RAID1 },
	{ "raid5f", RAID5F },
	{ "5f", RAID5F },
	{ "concat", CONCAT },
	{ }
};

/* [한국어] raid_bdev_state → 문자열 변환표. designated initializer 로 enum index 직접 매핑. */
const char *g_raid_state_names[] = {
	[RAID_BDEV_STATE_ONLINE]	= "online",
	[RAID_BDEV_STATE_CONFIGURING]	= "configuring",
	[RAID_BDEV_STATE_OFFLINE]	= "offline",
	[RAID_BDEV_STATE_MAX]		= NULL
};

/* [한국어] raid_process_type → 문자열 변환표. */
static const char *g_raid_process_type_names[] = {
	[RAID_PROCESS_NONE]	= "none",
	[RAID_PROCESS_REBUILD]	= "rebuild",
	[RAID_PROCESS_MAX]	= NULL
};

/* We have to use the typedef in the function declaration to appease astyle. */
/* [한국어] astyle (코드 포매터) 가 함수 반환 타입이 enum 이면 인식 못 해서 인덴팅이 깨짐 →
 * typedef 로 우회. 실질적 의미 변화 없음. */
typedef enum raid_level raid_level_t;
typedef enum raid_bdev_state raid_bdev_state_t;

/*
 * [한국어]
 * raid_bdev_str_to_level - 사용자 문자열 → raid_level 변환.
 *
 * @str: "raid0"/"1"/"raid5f"/"concat" 등.
 * @return: 매칭된 enum 값, 없으면 INVALID_RAID_LEVEL.
 *
 * 동기: RPC bdev_raid_create 의 raid_level 파라미터 파싱.
 */
raid_level_t
raid_bdev_str_to_level(const char *str)
{
	unsigned int i;

	assert(str != NULL);

	for (i = 0; g_raid_level_names[i].name != NULL; i++) {
		if (strcasecmp(g_raid_level_names[i].name, str) == 0) {
			return g_raid_level_names[i].value;
		}
	}

	return INVALID_RAID_LEVEL;
}

/*
 * [한국어]
 * raid_bdev_level_to_str - raid_level → 정규 문자열 변환 (출력용).
 *
 * @level: 변환할 enum.
 * @return: "raid0"/"raid1"/"raid5f"/"concat" 또는 빈 문자열.
 */
const char *
raid_bdev_level_to_str(enum raid_level level)
{
	unsigned int i;

	for (i = 0; g_raid_level_names[i].name != NULL; i++) {
		if (g_raid_level_names[i].value == level) {
			return g_raid_level_names[i].name;
		}
	}

	return "";
}

/*
 * [한국어]
 * raid_bdev_str_to_state - "online"/"configuring"/"offline" → enum 변환.
 *
 * @return: 매칭 enum, 없으면 RAID_BDEV_STATE_MAX.
 */
raid_bdev_state_t
raid_bdev_str_to_state(const char *str)
{
	unsigned int i;

	assert(str != NULL);

	for (i = 0; i < RAID_BDEV_STATE_MAX; i++) {
		if (strcasecmp(g_raid_state_names[i], str) == 0) {
			break;
		}
	}

	return i;
}

/*
 * [한국어]
 * raid_bdev_state_to_str - state enum → 문자열 (JSON 출력).
 */
const char *
raid_bdev_state_to_str(enum raid_bdev_state state)
{
	if (state >= RAID_BDEV_STATE_MAX) {
		return "";
	}

	return g_raid_state_names[state];
}

/*
 * [한국어]
 * raid_bdev_process_to_str - raid_process_type → "rebuild" 등 문자열.
 */
const char *
raid_bdev_process_to_str(enum raid_process_type value)
{
	if (value >= RAID_PROCESS_MAX) {
		return "";
	}

	return g_raid_process_type_names[value];
}

/*
 * brief:
 * raid_bdev_fini_start is called when bdev layer is starting the
 * shutdown process
 * params:
 * none
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_fini_start - bdev_module.fini_start 진입점. SHUTDOWN 시작 통지.
 *
 * 동기: spdk_bdev_finish 의 첫 단계에서 호출. CONFIGURING 상태로 남아 있는 (즉 등록되지
 * 않은) raid 의 base bdev 자원을 미리 해제 — 그렇지 않으면 base bdev 가 먼저 unregister
 * 될 때 raid 에 deadlock 유발 가능.
 * g_shutdown_started = true 설정 → _raid_bdev_destruct 가 적극적 자원 해제로 분기.
 */
static void
raid_bdev_fini_start(void)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_fini_start\n");

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
			RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
				raid_bdev_free_base_bdev_resource(base_info);
			}
		}
	}

	g_shutdown_started = true;
}

/*
 * brief:
 * raid_bdev_exit is called on raid bdev module exit time by bdev layer
 * params:
 * none
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_exit - bdev_module.module_fini 진입점. 남아 있는 raid_bdev 전부 cleanup_and_free.
 *
 * 동기: fini_start 이후, 모든 spdk_bdev 가 unregister 된 뒤에 호출. 이 시점에는 base bdev
 * 도 모두 빠졌으므로 단순 메모리 정리만 수행.
 */
static void
raid_bdev_exit(void)
{
	struct raid_bdev *raid_bdev, *tmp;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_exit\n");

	TAILQ_FOREACH_SAFE(raid_bdev, &g_raid_bdev_list, global_link, tmp) {
		raid_bdev_cleanup_and_free(raid_bdev);
	}
}

/*
 * [한국어]
 * raid_bdev_opts_config_json - bdev_raid_set_options RPC 를 JSON 으로 직렬화 (save_config 용).
 */
static void
raid_bdev_opts_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_raid_set_options");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "process_window_size_kb", g_opts.process_window_size_kb);
	spdk_json_write_named_uint32(w, "process_max_bandwidth_mb_sec",
				     g_opts.process_max_bandwidth_mb_sec);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * raid_bdev_config_json - bdev_module.config_json 진입점. 전역 옵션 출력.
 *
 * 동기: 각 raid_bdev 의 config 는 write_config_json 이 별도로 처리. 여기서는 옵션만.
 */
static int
raid_bdev_config_json(struct spdk_json_write_ctx *w)
{
	raid_bdev_opts_config_json(w);

	return 0;
}

/*
 * brief:
 * raid_bdev_get_ctx_size is used to return the context size of bdev_io for raid
 * module
 * params:
 * none
 * returns:
 * size of spdk_bdev_io context for raid
 */
/*
 * [한국어]
 * raid_bdev_get_ctx_size - bdev_module.get_ctx_size 진입점. bdev_io 의 driver_ctx 영역 크기.
 *
 * 동기: bdev 레이어는 모든 bdev_io 풀에서 가장 큰 driver_ctx 를 수용해야 하므로 모듈별로
 * 필요 크기를 보고한다. 여기서는 raid_bdev_io 크기.
 */
static int
raid_bdev_get_ctx_size(void)
{
	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_get_ctx_size\n");
	return sizeof(struct raid_bdev_io);
}

/* [한국어] RAID 모듈을 bdev 레이어에 등록하는 spdk_bdev_module. SPDK_BDEV_MODULE_REGISTER 매크로가
 * constructor 시점에 spdk_bdev_module_list_add 호출.
 * - module_init  : RAID 모듈 초기 부팅 (현재는 no-op).
 * - fini_start   : SHUTDOWN 첫 단계 — CONFIGURING raid 들 정리.
 * - module_fini  : 모든 raid_bdev free.
 * - config_json  : save_config 시 옵션 출력.
 * - get_ctx_size : bdev_io driver_ctx 크기.
 * - examine_disk : 새 bdev 출현 시 슈퍼블록 검사 (raid_bdev_examine).
 * - async_init/fini = false : 동기 (즉시 완료). */
static struct spdk_bdev_module g_raid_if = {
	.name = "raid",
	.module_init = raid_bdev_init,
	.fini_start = raid_bdev_fini_start,
	.module_fini = raid_bdev_exit,
	.config_json = raid_bdev_config_json,
	.get_ctx_size = raid_bdev_get_ctx_size,
	.examine_disk = raid_bdev_examine,
	.async_init = false,
	.async_fini = false,
};
SPDK_BDEV_MODULE_REGISTER(raid, &g_raid_if)

/*
 * brief:
 * raid_bdev_init is the initialization function for raid bdev module
 * params:
 * none
 * returns:
 * 0 - success
 * non zero - failure
 */
/*
 * [한국어]
 * raid_bdev_init - bdev_module.module_init. 현재는 추가 초기화 필요 없음.
 *
 * 동기: 향후 RAID 레벨 모듈을 일괄 init 하는 hook 으로 확장 가능. 현재는 0 만 반환.
 */
static int
raid_bdev_init(void)
{
	return 0;
}

/*
 * [한국어]
 * _raid_bdev_create - raid_bdev 객체 할당 및 기본 설정 (raid_bdev_create 의 내부 헬퍼).
 *
 * @name: RAID bdev 이름 (RAID_BDEV_SB_NAME_SIZE 제한).
 * @strip_size: KB 단위 strip size (RAID1 은 0, 그 외는 2의 거듭제곱).
 * @num_base_bdevs: 멤버 수 (모듈의 base_bdevs_min 이상).
 * @level: RAID 레벨.
 * @superblock_enabled: true 면 슈퍼블록 사용.
 * @uuid: RAID UUID (NULL 불가; superblock 활성+null 이면 호출자가 생성).
 * @raid_bdev_out: 출력.
 * @return: 0 또는 음수 errno.
 *
 * 동작 단계:
 *   1) 이름 길이/중복/strip 유효성 검사.
 *   2) raid_bdev_module_find 로 RAID 레벨 모듈 매칭.
 *   3) base_bdevs_constraint 로 min_operational 계산 (CONSTRAINT_MAX_BASE_BDEVS_REMOVED /
 *      CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL / CONSTRAINT_UNSET).
 *   4) raid_bdev 와 base_bdev_info 배열 할당, 초기 필드 채움.
 *   5) bdev.fn_table = g_raid_bdev_fn_table, module = g_raid_if.
 *   6) g_raid_bdev_list 에 등록.
 * 호출자: raid_bdev_create (RPC), raid_bdev_create_from_sb (superblock examine).
 */
static int
_raid_bdev_create(const char *name, uint32_t strip_size, uint8_t num_base_bdevs,
		  enum raid_level level, bool superblock_enabled, const struct spdk_uuid *uuid,
		  struct raid_bdev **raid_bdev_out)
{
	struct raid_bdev *raid_bdev;
	struct spdk_bdev *raid_bdev_gen;
	struct raid_bdev_module *module;
	struct raid_base_bdev_info *base_info;
	uint8_t min_operational;

	if (strnlen(name, RAID_BDEV_SB_NAME_SIZE) == RAID_BDEV_SB_NAME_SIZE) {
		SPDK_ERRLOG("Raid bdev name '%s' exceeds %d characters\n", name, RAID_BDEV_SB_NAME_SIZE - 1);
		return -EINVAL;
	}

	if (raid_bdev_find_by_name(name) != NULL) {
		SPDK_ERRLOG("Duplicate raid bdev name found: %s\n", name);
		return -EEXIST;
	}

	if (level == RAID1) {
		if (strip_size != 0) {
			SPDK_ERRLOG("Strip size is not supported by raid1\n");
			return -EINVAL;
		}
	} else if (spdk_u32_is_pow2(strip_size) == false) {
		SPDK_ERRLOG("Invalid strip size %" PRIu32 "\n", strip_size);
		return -EINVAL;
	}

	module = raid_bdev_module_find(level);
	if (module == NULL) {
		SPDK_ERRLOG("Unsupported raid level '%d'\n", level);
		return -EINVAL;
	}

	assert(module->base_bdevs_min != 0);
	if (num_base_bdevs < module->base_bdevs_min) {
		SPDK_ERRLOG("At least %u base devices required for %s\n",
			    module->base_bdevs_min,
			    raid_bdev_level_to_str(level));
		return -EINVAL;
	}

	switch (module->base_bdevs_constraint.type) {
	case CONSTRAINT_MAX_BASE_BDEVS_REMOVED:
		min_operational = num_base_bdevs - module->base_bdevs_constraint.value;
		break;
	case CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL:
		min_operational = module->base_bdevs_constraint.value;
		break;
	case CONSTRAINT_UNSET:
		if (module->base_bdevs_constraint.value != 0) {
			SPDK_ERRLOG("Unexpected constraint value '%u' provided for raid bdev '%s'.\n",
				    (uint8_t)module->base_bdevs_constraint.value, name);
			return -EINVAL;
		}
		min_operational = num_base_bdevs;
		break;
	default:
		SPDK_ERRLOG("Unrecognised constraint type '%u' in module for raid level '%s'.\n",
			    (uint8_t)module->base_bdevs_constraint.type,
			    raid_bdev_level_to_str(module->level));
		return -EINVAL;
	};

	if (min_operational == 0 || min_operational > num_base_bdevs) {
		SPDK_ERRLOG("Wrong constraint value for raid level '%s'.\n",
			    raid_bdev_level_to_str(module->level));
		return -EINVAL;
	}

	raid_bdev = calloc(1, sizeof(*raid_bdev));
	if (!raid_bdev) {
		SPDK_ERRLOG("Unable to allocate memory for raid bdev\n");
		return -ENOMEM;
	}

	raid_bdev->module = module;
	raid_bdev->num_base_bdevs = num_base_bdevs;
	raid_bdev->base_bdev_info = calloc(raid_bdev->num_base_bdevs,
					   sizeof(struct raid_base_bdev_info));
	if (!raid_bdev->base_bdev_info) {
		SPDK_ERRLOG("Unable able to allocate base bdev info\n");
		raid_bdev_free(raid_bdev);
		return -ENOMEM;
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->raid_bdev = raid_bdev;
	}

	/* strip_size_kb is from the rpc param.  strip_size is in blocks and used
	 * internally and set later.
	 */
	raid_bdev->strip_size = 0;
	raid_bdev->strip_size_kb = strip_size;
	raid_bdev->state = RAID_BDEV_STATE_CONFIGURING;
	raid_bdev->level = level;
	raid_bdev->min_base_bdevs_operational = min_operational;
	raid_bdev->superblock_enabled = superblock_enabled;

	raid_bdev_gen = &raid_bdev->bdev;

	raid_bdev_gen->name = strdup(name);
	if (!raid_bdev_gen->name) {
		SPDK_ERRLOG("Unable to allocate name for raid\n");
		raid_bdev_free(raid_bdev);
		return -ENOMEM;
	}

	raid_bdev_gen->product_name = "Raid Volume";
	raid_bdev_gen->ctxt = raid_bdev;
	raid_bdev_gen->fn_table = &g_raid_bdev_fn_table;
	raid_bdev_gen->module = &g_raid_if;
	raid_bdev_gen->write_cache = 0;
	spdk_uuid_copy(&raid_bdev_gen->uuid, uuid);

	TAILQ_INSERT_TAIL(&g_raid_bdev_list, raid_bdev, global_link);

	*raid_bdev_out = raid_bdev;

	return 0;
}

/*
 * brief:
 * raid_bdev_create allocates raid bdev based on passed configuration
 * params:
 * name - name for raid bdev
 * strip_size - strip size in KB
 * num_base_bdevs - number of base bdevs
 * level - raid level
 * superblock_enabled - true if raid should have superblock
 * uuid - uuid to set for the bdev
 * raid_bdev_out - the created raid bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
/*
 * [한국어]
 * raid_bdev_create - RPC bdev_raid_create 의 백엔드. _raid_bdev_create 호출 + UUID 보정.
 *
 * 동기: superblock_enabled && uuid null 이면 superblock 작성 전에 UUID 가 필요하므로 생성.
 * num_base_bdevs_operational 을 처음에는 전체 멤버 수로 설정 (이후 examine 에서 조정).
 */
int
raid_bdev_create(const char *name, uint32_t strip_size, uint8_t num_base_bdevs,
		 enum raid_level level, bool superblock_enabled, const struct spdk_uuid *uuid,
		 struct raid_bdev **raid_bdev_out)
{
	struct raid_bdev *raid_bdev;
	int rc;

	assert(uuid != NULL);

	rc = _raid_bdev_create(name, strip_size, num_base_bdevs, level, superblock_enabled, uuid,
			       &raid_bdev);
	if (rc != 0) {
		return rc;
	}

	if (superblock_enabled && spdk_uuid_is_null(uuid)) {
		/* we need to have the uuid to store in the superblock before the bdev is registered */
		spdk_uuid_generate(&raid_bdev->bdev.uuid);
	}

	raid_bdev->num_base_bdevs_operational = num_base_bdevs;

	*raid_bdev_out = raid_bdev;

	return 0;
}

/*
 * [한국어]
 * _raid_bdev_unregistering_cont - self_desc 닫기 (app thread 실행 본체).
 *
 * 동기: raid_bdev_configure_cont 에서 self open 한 desc 를 unregister 시점에 닫아야
 * spdk_bdev_unregister 가 진행될 수 있다.
 */
static void
_raid_bdev_unregistering_cont(void *ctx)
{
	struct raid_bdev *raid_bdev = ctx;

	spdk_bdev_close(raid_bdev->self_desc);
	raid_bdev->self_desc = NULL;
}

/*
 * [한국어]
 * raid_bdev_unregistering_cont - app thread 로 위임.
 */
static void
raid_bdev_unregistering_cont(void *ctx)
{
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), _raid_bdev_unregistering_cont, ctx);
}

/*
 * [한국어]
 * raid_bdev_process_add_finish_action - process 가 stop 된 뒤 실행할 액션 큐에 추가.
 *
 * @process: 대상 process (RUNNING 또는 STOPPING).
 * @cb/@cb_ctx: 실행할 콜백.
 * @return: 0 / -ENOMEM.
 *
 * 동기: process 종료 시점에 일괄 처리해야 하는 후속 작업 (예: unregister 진행) 을 미리 큐잉.
 * 실행 컨텍스트: process->thread.
 */
static int
raid_bdev_process_add_finish_action(struct raid_bdev_process *process, spdk_msg_fn cb, void *cb_ctx)
{
	struct raid_process_finish_action *finish_action;

	assert(spdk_get_thread() == process->thread);
	assert(process->state < RAID_PROCESS_STATE_STOPPED);

	finish_action = calloc(1, sizeof(*finish_action));
	if (finish_action == NULL) {
		return -ENOMEM;
	}

	finish_action->cb = cb;
	finish_action->cb_ctx = cb_ctx;

	TAILQ_INSERT_TAIL(&process->finish_actions, finish_action, link);

	return 0;
}

/*
 * [한국어]
 * raid_bdev_unregistering_stop_process - 외부 unregister 진입 시 process 를 stop 시키며 후속 등록.
 *
 * 동기: spdk_bdev_unregister 가 process 진행 중인 raid 에 들어오면, process 가 끝나야
 * unregister 가 안전. STOPPING 으로 전이시키고 finish action 큐에 unregistering_cont 등록.
 */
static void
raid_bdev_unregistering_stop_process(void *ctx)
{
	struct raid_bdev_process *process = ctx;
	struct raid_bdev *raid_bdev = process->raid_bdev;
	int rc;

	process->state = RAID_PROCESS_STATE_STOPPING;
	if (process->status == 0) {
		process->status = -ECANCELED;
	}

	rc = raid_bdev_process_add_finish_action(process, raid_bdev_unregistering_cont, raid_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to add raid bdev '%s' process finish action: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-rc));
	}
}

/*
 * [한국어]
 * raid_bdev_event_cb - self_desc (raid 가 자기 자신을 open 한 desc) 의 event 콜백.
 *
 * @type: event 타입 (REMOVE 만 처리).
 * @bdev: 이벤트 대상 (= raid_bdev->bdev).
 * @event_ctx: raid_bdev.
 *
 * 동기: spdk_bdev_unregister 가 호출되면 모든 open desc 에 REMOVE 이벤트 전달. raid 가
 * 자기 자신을 open 해둔 이유 — process 가 진행 중이면 unregister 를 process 완료 시까지
 * 지연시키기 위함.
 */
static void
raid_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct raid_bdev *raid_bdev = event_ctx;

	if (type == SPDK_BDEV_EVENT_REMOVE) {
		if (raid_bdev->process != NULL) {
			spdk_thread_send_msg(raid_bdev->process->thread, raid_bdev_unregistering_stop_process,
					     raid_bdev->process);
		} else {
			raid_bdev_unregistering_cont(raid_bdev);
		}
	}
}

/*
 * [한국어]
 * raid_bdev_configure_cont - configure 의 마지막 단계 — io_device + bdev 레이어 등록.
 *
 * @raid_bdev: 등록할 raid_bdev.
 *
 * 동작:
 *   1) state = ONLINE.
 *   2) spdk_io_device_register(raid_bdev, create_cb, destroy_cb, ch_size, name) — 채널 인프라.
 *   3) spdk_bdev_register — bdev 레이어에 노출 (이 시점 이후 다른 모듈/RPC 가 raid 를 발견).
 *   4) self open: 자기 자신을 open 한 desc 보관 (raid_bdev_event_cb 로 unregister 인터셉트).
 *   5) configure_cb 콜백 호출 (RPC 응답 등).
 * 실행 컨텍스트: app thread. superblock 작성 후 (write_sb_cb 에서) 또는 즉시 호출.
 */
static void
raid_bdev_configure_cont(struct raid_bdev *raid_bdev)
{
	struct spdk_bdev *raid_bdev_gen = &raid_bdev->bdev;
	int rc;

	raid_bdev->state = RAID_BDEV_STATE_ONLINE;
	SPDK_DEBUGLOG(bdev_raid, "io device register %p\n", raid_bdev);
	SPDK_DEBUGLOG(bdev_raid, "blockcnt %" PRIu64 ", blocklen %u\n",
		      raid_bdev_gen->blockcnt, raid_bdev_gen->blocklen);
	spdk_io_device_register(raid_bdev, raid_bdev_create_cb, raid_bdev_destroy_cb,
				sizeof(struct raid_bdev_io_channel),
				raid_bdev_gen->name);
	rc = spdk_bdev_register(raid_bdev_gen);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register raid bdev '%s': %s\n",
			    raid_bdev_gen->name, spdk_strerror(-rc));
		goto out;
	}

	/*
	 * Open the bdev internally to delay unregistering if we need to stop a background process
	 * first. The process may still need to unquiesce a range but it will fail because the
	 * bdev's internal.spinlock is destroyed by the time the destruct callback is reached.
	 * During application shutdown, bdevs automatically get unregistered by the bdev layer
	 * so this is the only way currently to do this correctly.
	 * TODO: try to handle this correctly in bdev layer instead.
	 */
	rc = spdk_bdev_open_ext(raid_bdev_gen->name, false, raid_bdev_event_cb, raid_bdev,
				&raid_bdev->self_desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open raid bdev '%s': %s\n",
			    raid_bdev_gen->name, spdk_strerror(-rc));
		spdk_bdev_unregister(raid_bdev_gen, NULL, NULL);
		goto out;
	}

	SPDK_DEBUGLOG(bdev_raid, "raid bdev generic %p\n", raid_bdev_gen);
	SPDK_DEBUGLOG(bdev_raid, "raid bdev is created with name %s, raid_bdev %p\n",
		      raid_bdev_gen->name, raid_bdev);
out:
	if (rc != 0) {
		if (raid_bdev->module->stop != NULL) {
			raid_bdev->module->stop(raid_bdev);
		}
		spdk_io_device_unregister(raid_bdev, NULL);
		raid_bdev->state = RAID_BDEV_STATE_CONFIGURING;
	}

	if (raid_bdev->configure_cb != NULL) {
		raid_bdev->configure_cb(raid_bdev->configure_cb_ctx, rc);
		raid_bdev->configure_cb = NULL;
	}
}

/*
 * [한국어]
 * raid_bdev_configure_write_sb_cb - 슈퍼블록 쓰기 완료 콜백 → configure_cont 또는 에러 종료.
 *
 * 동기: 슈퍼블록이 디스크에 안전히 기록되어야 bdev 레이어 등록을 진행.
 */
static void
raid_bdev_configure_write_sb_cb(int status, struct raid_bdev *raid_bdev, void *ctx)
{
	if (status == 0) {
		raid_bdev_configure_cont(raid_bdev);
	} else {
		SPDK_ERRLOG("Failed to write raid bdev '%s' superblock: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
		if (raid_bdev->module->stop != NULL) {
			raid_bdev->module->stop(raid_bdev);
		}
		if (raid_bdev->configure_cb != NULL) {
			raid_bdev->configure_cb(raid_bdev->configure_cb_ctx, status);
			raid_bdev->configure_cb = NULL;
		}
	}
}

/*
 * brief:
 * If raid bdev config is complete, then only register the raid bdev to
 * bdev layer and remove this raid bdev from configuring list and
 * insert the raid bdev to configured list
 * params:
 * raid_bdev - pointer to raid bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
/*
 * [한국어]
 * raid_bdev_configure - 모든 base bdev 확보 완료 시 raid 를 ONLINE 으로 전이.
 *
 * @raid_bdev: CONFIGURING 상태의 raid (num_base_bdevs_discovered == num_base_bdevs_operational).
 * @cb: 완료 콜백 (RPC 응답 등).
 * @cb_ctx: 콜백 ctx.
 * @return: 0 / 음수 errno (모듈 start 실패, strip 크기 0, superblock 불일치 등).
 *
 * 동작:
 *   1) strip_size_kb → 블록 단위 strip_size 변환 + log2.
 *   2) module->start 호출 (RAID 레벨 모듈이 자기 컨텍스트 초기화, bdev.blockcnt 등 설정).
 *   3) superblock 활성: 새 sb 면 alloc+init, 기존 sb 면 검증, 그 후 디스크에 기록 →
 *      write_sb_cb 가 configure_cont 호출.
 *   4) superblock 비활성: 즉시 configure_cont.
 */
static int
raid_bdev_configure(struct raid_bdev *raid_bdev, raid_bdev_configure_cb cb, void *cb_ctx)
{
	uint32_t data_block_size = spdk_bdev_get_data_block_size(&raid_bdev->bdev);
	int rc;

	assert(raid_bdev->state == RAID_BDEV_STATE_CONFIGURING);
	assert(raid_bdev->num_base_bdevs_discovered == raid_bdev->num_base_bdevs_operational);
	assert(raid_bdev->bdev.blocklen > 0);

	/* The strip_size_kb is read in from user in KB. Convert to blocks here for
	 * internal use.
	 */
	raid_bdev->strip_size = (raid_bdev->strip_size_kb * 1024) / data_block_size;
	if (raid_bdev->strip_size == 0 && raid_bdev->level != RAID1) {
		SPDK_ERRLOG("Strip size cannot be smaller than the device block size\n");
		return -EINVAL;
	}
	raid_bdev->strip_size_shift = spdk_u32log2(raid_bdev->strip_size);

	rc = raid_bdev->module->start(raid_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("raid module startup callback failed\n");
		return rc;
	}

	assert(raid_bdev->configure_cb == NULL);
	raid_bdev->configure_cb = cb;
	raid_bdev->configure_cb_ctx = cb_ctx;

	if (raid_bdev->superblock_enabled) {
		if (raid_bdev->sb == NULL) {
			rc = raid_bdev_alloc_superblock(raid_bdev, data_block_size);
			if (rc == 0) {
				raid_bdev_init_superblock(raid_bdev);
			}
		} else {
			assert(spdk_uuid_compare(&raid_bdev->sb->uuid, &raid_bdev->bdev.uuid) == 0);
			if (raid_bdev->sb->block_size != data_block_size) {
				SPDK_ERRLOG("blocklen does not match value in superblock\n");
				rc = -EINVAL;
			}
			if (raid_bdev->sb->raid_size != raid_bdev->bdev.blockcnt) {
				SPDK_ERRLOG("blockcnt does not match value in superblock\n");
				rc = -EINVAL;
			}
		}

		if (rc != 0) {
			raid_bdev->configure_cb = NULL;
			if (raid_bdev->module->stop != NULL) {
				raid_bdev->module->stop(raid_bdev);
			}
			return rc;
		}

		raid_bdev_write_superblock(raid_bdev, raid_bdev_configure_write_sb_cb, NULL);
	} else {
		raid_bdev_configure_cont(raid_bdev);
	}

	return 0;
}

/*
 * brief:
 * If raid bdev is online and registered, change the bdev state to
 * configuring and unregister this raid device. Queue this raid device
 * in configuring list
 * params:
 * raid_bdev - pointer to raid bdev
 * cb_fn - callback function
 * cb_arg - argument to callback function
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_deconfigure - ONLINE raid 를 OFFLINE 으로 전이시키고 bdev 레이어에서 unregister.
 *
 * @raid_bdev: 대상.
 * @cb_fn/@cb_arg: unregister 완료 콜백.
 *
 * 동기: base bdev 가 정족수 미만 되거나 raid_bdev_delete 호출 시 진입. ONLINE 이 아니면
 * 콜백만 즉시 호출하고 return.
 */
static void
raid_bdev_deconfigure(struct raid_bdev *raid_bdev, raid_bdev_destruct_cb cb_fn,
		      void *cb_arg)
{
	if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
		return;
	}

	raid_bdev->state = RAID_BDEV_STATE_OFFLINE;
	SPDK_DEBUGLOG(bdev_raid, "raid bdev state changing from online to offline\n");

	spdk_bdev_unregister(&raid_bdev->bdev, cb_fn, cb_arg);
}

/*
 * brief:
 * raid_bdev_find_base_info_by_bdev function finds the base bdev info by bdev.
 * params:
 * base_bdev - pointer to base bdev
 * returns:
 * base bdev info if found, otherwise NULL.
 */
/*
 * [한국어]
 * raid_bdev_find_base_info_by_bdev - 전역 raid 리스트 + 각 raid 의 base_info 배열 이중 순회.
 *
 * @base_bdev: 찾을 base bdev.
 * @return: 해당 base bdev 가 멤버로 속한 raid 의 base_info, 없으면 NULL.
 *
 * 동기: base bdev event (REMOVE/RESIZE) 콜백에서 어느 raid 가 영향을 받는지 검색.
 */
static struct raid_base_bdev_info *
raid_bdev_find_base_info_by_bdev(struct spdk_bdev *base_bdev)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
			if (base_info->desc != NULL &&
			    spdk_bdev_desc_get_bdev(base_info->desc) == base_bdev) {
				return base_info;
			}
		}
	}

	return NULL;
}

/*
 * [한국어]
 * raid_bdev_remove_base_bdev_done - 1 개 base bdev 제거의 최종 단계 — 카운터 조정 + 콜백.
 *
 * @base_info: 제거 완료된 base.
 * @status: 제거 결과 (0=성공, 음수 errno).
 *
 * 동기: 슈퍼블록 업데이트, quiesce/unquiesce, channel iteration 이 모두 끝난 후 진입.
 * num_base_bdevs_operational 감소 후 min 미만이면 raid 전체 deconfigure.
 */
static void
raid_bdev_remove_base_bdev_done(struct raid_base_bdev_info *base_info, int status)
{
	struct raid_bdev *raid_bdev = base_info->raid_bdev;

	assert(base_info->remove_scheduled);
	base_info->remove_scheduled = false;

	if (status == 0) {
		raid_bdev->num_base_bdevs_operational--;
		if (raid_bdev->num_base_bdevs_operational < raid_bdev->min_base_bdevs_operational) {
			/* There is not enough base bdevs to keep the raid bdev operational. */
			raid_bdev_deconfigure(raid_bdev, base_info->remove_cb, base_info->remove_cb_ctx);
			return;
		}
	}

	if (base_info->remove_cb != NULL) {
		base_info->remove_cb(base_info->remove_cb_ctx, status);
	}
}

/*
 * [한국어]
 * raid_bdev_remove_base_bdev_on_unquiesced - unquiesce 완료 후 호출. done 으로 이어짐.
 */
static void
raid_bdev_remove_base_bdev_on_unquiesced(void *ctx, int status)
{
	struct raid_base_bdev_info *base_info = ctx;
	struct raid_bdev *raid_bdev = base_info->raid_bdev;

	if (status != 0) {
		SPDK_ERRLOG("Failed to unquiesce raid bdev %s: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
	}

	raid_bdev_remove_base_bdev_done(base_info, status);
}

/*
 * [한국어]
 * raid_bdev_channel_remove_base_bdev - spdk_for_each_channel iter — 각 채널에서 해당 슬롯 닫기.
 *
 * @i: iter. ctx = base_info.
 *
 * 동기: base bdev 1 개가 빠지면 모든 채널에서 해당 슬롯의 spdk_io_channel 을 put.
 * process 가 진행 중인 채널이라면 ch_processed 의 슬롯도 null 화.
 * 실행 컨텍스트: 채널 소유 스레드.
 */
static void
raid_bdev_channel_remove_base_bdev(struct spdk_io_channel_iter *i)
{
	struct raid_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct raid_bdev_io_channel *raid_ch = spdk_io_channel_get_ctx(ch);
	uint8_t idx = raid_bdev_base_bdev_slot(base_info);

	SPDK_DEBUGLOG(bdev_raid, "slot: %u raid_ch: %p\n", idx, raid_ch);

	if (raid_ch->base_channel[idx] != NULL) {
		spdk_put_io_channel(raid_ch->base_channel[idx]);
		raid_ch->base_channel[idx] = NULL;
	}

	if (raid_ch->process.ch_processed != NULL) {
		raid_ch->process.ch_processed->base_channel[idx] = NULL;
	}

	spdk_for_each_channel_continue(i, 0);
}

/*
 * [한국어]
 * raid_bdev_channels_remove_base_bdev_done - 모든 채널 처리 완료 후 base 자원 해제 + unquiesce.
 */
static void
raid_bdev_channels_remove_base_bdev_done(struct spdk_io_channel_iter *i, int status)
{
	struct raid_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);
	struct raid_bdev *raid_bdev = base_info->raid_bdev;

	raid_bdev_free_base_bdev_resource(base_info);

	spdk_bdev_unquiesce(&raid_bdev->bdev, &g_raid_if, raid_bdev_remove_base_bdev_on_unquiesced,
			    base_info);
}

/*
 * [한국어]
 * raid_bdev_remove_base_bdev_do_remove - deconfigure 표시 + 모든 채널 iterate.
 */
static void
raid_bdev_remove_base_bdev_do_remove(struct raid_base_bdev_info *base_info)
{
	raid_bdev_deconfigure_base_bdev(base_info);

	spdk_for_each_channel(base_info->raid_bdev, raid_bdev_channel_remove_base_bdev, base_info,
			      raid_bdev_channels_remove_base_bdev_done);
}

/*
 * [한국어]
 * raid_bdev_remove_base_bdev_reset_done - 제거 전 RESET 완료 후 채널 정리 단계로 진입.
 */
static void
raid_bdev_remove_base_bdev_reset_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_base_bdev_info *base_info = cb_arg;

	spdk_bdev_free_io(bdev_io);

	raid_bdev_remove_base_bdev_do_remove(base_info);
}

/*
 * [한국어]
 * raid_bdev_remove_base_bdev_cont - 제거 직전에 RESET 발행 → in-flight IO 안전하게 정리.
 *
 * 동기: NVMe 비휘발 큐에 남아있을 수 있는 IO 를 비우기 위해 RESET. 실패해도 제거는 진행.
 */
static void
raid_bdev_remove_base_bdev_cont(struct raid_base_bdev_info *base_info)
{
	int rc;

	rc = spdk_bdev_reset(base_info->desc, base_info->app_thread_ch,
			     raid_bdev_remove_base_bdev_reset_done, base_info);
	if (rc != 0) {
		SPDK_WARNLOG("Reset base bdev '%s' before removal failed: %s\n",
			     base_info->name, spdk_strerror(-rc));
		/* Proceed with removal even if reset submission failed */
		raid_bdev_remove_base_bdev_do_remove(base_info);
	}
}

/*
 * [한국어]
 * raid_bdev_remove_base_bdev_write_sb_cb - 슈퍼블록 (멤버 상태 MISSING/FAILED) 쓰기 완료 콜백.
 */
static void
raid_bdev_remove_base_bdev_write_sb_cb(int status, struct raid_bdev *raid_bdev, void *ctx)
{
	struct raid_base_bdev_info *base_info = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to write raid bdev '%s' superblock: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
		raid_bdev_remove_base_bdev_done(base_info, status);
		return;
	}

	raid_bdev_remove_base_bdev_cont(base_info);
}

/*
 * [한국어]
 * raid_bdev_remove_base_bdev_on_quiesced - quiesce 완료 후 슈퍼블록 갱신 (해당 슬롯 MISSING/FAILED).
 *
 * 동기: 멤버 1 개 제거 시 슈퍼블록의 base_bdevs[] 배열에서 해당 slot 상태를 갱신. 슈퍼블록
 * 비활성 시 즉시 cont 단계로 진입.
 */
static void
raid_bdev_remove_base_bdev_on_quiesced(void *ctx, int status)
{
	struct raid_base_bdev_info *base_info = ctx;
	struct raid_bdev *raid_bdev = base_info->raid_bdev;

	if (status != 0) {
		SPDK_ERRLOG("Failed to quiesce raid bdev %s: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
		raid_bdev_remove_base_bdev_done(base_info, status);
		return;
	}

	if (raid_bdev->sb) {
		struct raid_bdev_superblock *sb = raid_bdev->sb;
		uint8_t slot = raid_bdev_base_bdev_slot(base_info);
		uint8_t i;

		for (i = 0; i < sb->base_bdevs_size; i++) {
			struct raid_bdev_sb_base_bdev *sb_base_bdev = &sb->base_bdevs[i];

			if (sb_base_bdev->state == RAID_SB_BASE_BDEV_CONFIGURED &&
			    sb_base_bdev->slot == slot) {
				if (base_info->is_failed) {
					sb_base_bdev->state = RAID_SB_BASE_BDEV_FAILED;
				} else {
					sb_base_bdev->state = RAID_SB_BASE_BDEV_MISSING;
				}

				raid_bdev_write_superblock(raid_bdev, raid_bdev_remove_base_bdev_write_sb_cb, base_info);
				return;
			}
		}
	}

	raid_bdev_remove_base_bdev_cont(base_info);
}

/*
 * [한국어]
 * raid_bdev_remove_base_bdev_quiesce - 전체 raid 를 quiesce → in-flight IO drain.
 *
 * 동기: 멤버 제거의 첫 단계. quiesce 가 완료되면 spdk_bdev_quiesce 콜백 (on_quiesced) 진입.
 */
static int
raid_bdev_remove_base_bdev_quiesce(struct raid_base_bdev_info *base_info)
{
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	return spdk_bdev_quiesce(&base_info->raid_bdev->bdev, &g_raid_if,
				 raid_bdev_remove_base_bdev_on_quiesced, base_info);
}

/* [한국어] process 진행 중에 base bdev 가 제거될 때 process thread 와 app thread 사이
 * 메시지 전달용 컨텍스트. raid_bdev 의 필드는 process thread 에서 직접 접근하지 않기
 * 위해 ctx 에 복사된 값 (num_base_bdevs_operational) 을 보낸다. */
struct raid_bdev_process_base_bdev_remove_ctx {
	struct raid_bdev_process *process;
	/* [한국어] 영향 받는 process. base bdev 가 target 인 경우 stop 필요. */
	struct raid_base_bdev_info *base_info;
	/* [한국어] 제거할 base. */
	uint8_t num_base_bdevs_operational;
	/* [한국어] 메시지 발송 시점의 operational 수 — process 가 stop 해야 하는지 판단용. */
};

/*
 * [한국어]
 * _raid_bdev_process_base_bdev_remove_cont - app thread 에서 quiesce 호출 (process 동기화 후).
 */
static void
_raid_bdev_process_base_bdev_remove_cont(void *ctx)
{
	struct raid_base_bdev_info *base_info = ctx;
	int ret;

	ret = raid_bdev_remove_base_bdev_quiesce(base_info);
	if (ret != 0) {
		raid_bdev_remove_base_bdev_done(base_info, ret);
	}
}

/*
 * [한국어]
 * raid_bdev_process_base_bdev_remove_cont - process thread → app thread 로 전환 후 quiesce.
 */
static void
raid_bdev_process_base_bdev_remove_cont(void *_ctx)
{
	struct raid_bdev_process_base_bdev_remove_ctx *ctx = _ctx;
	struct raid_base_bdev_info *base_info = ctx->base_info;

	free(ctx);

	spdk_thread_send_msg(spdk_thread_get_app_thread(), _raid_bdev_process_base_bdev_remove_cont,
			     base_info);
}

/*
 * [한국어]
 * _raid_bdev_process_base_bdev_remove - process thread 에서 실행. process stop 여부 판단.
 *
 * 동기: 제거 대상이 process->target 이거나, operational 수가 min 이하로 떨어지면 process
 * 종료가 필요. 그 외에는 process 가 계속 진행.
 */
static void
_raid_bdev_process_base_bdev_remove(void *_ctx)
{
	struct raid_bdev_process_base_bdev_remove_ctx *ctx = _ctx;
	struct raid_bdev_process *process = ctx->process;
	int ret;

	if (ctx->base_info != process->target &&
	    ctx->num_base_bdevs_operational > process->raid_bdev->min_base_bdevs_operational) {
		/* process doesn't need to be stopped */
		raid_bdev_process_base_bdev_remove_cont(ctx);
		return;
	}

	assert(process->state > RAID_PROCESS_STATE_INIT &&
	       process->state < RAID_PROCESS_STATE_STOPPED);

	ret = raid_bdev_process_add_finish_action(process, raid_bdev_process_base_bdev_remove_cont, ctx);
	if (ret != 0) {
		raid_bdev_remove_base_bdev_done(ctx->base_info, ret);
		free(ctx);
		return;
	}

	process->state = RAID_PROCESS_STATE_STOPPING;

	if (process->status == 0) {
		process->status = -ENODEV;
	}
}

/*
 * [한국어]
 * raid_bdev_process_base_bdev_remove - process 가 살아 있는 raid 의 base bdev 제거 진입점.
 *
 * @process: 진행 중인 process.
 * @base_info: 제거할 base.
 * @return: 0 / -ENOMEM.
 *
 * 동기: process thread 가 raid_bdev->* 를 직접 보면 race 발생 → ctx 에 필요한 정보 (process,
 * base_info, num_base_bdevs_operational) 를 복사해 메시지 전달.
 */
static int
raid_bdev_process_base_bdev_remove(struct raid_bdev_process *process,
				   struct raid_base_bdev_info *base_info)
{
	struct raid_bdev_process_base_bdev_remove_ctx *ctx;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	/*
	 * We have to send the process and num_base_bdevs_operational in the message ctx
	 * because the process thread should not access raid_bdev's properties. Particularly,
	 * raid_bdev->process may be cleared by the time the message is handled, but ctx->process
	 * will still be valid until the process is fully stopped.
	 */
	ctx->base_info = base_info;
	ctx->process = process;
	/*
	 * raid_bdev->num_base_bdevs_operational can't be used here because it is decremented
	 * after the removal and more than one base bdev may be removed at the same time
	 */
	RAID_FOR_EACH_BASE_BDEV(process->raid_bdev, base_info) {
		if (base_info->is_configured && !base_info->remove_scheduled) {
			ctx->num_base_bdevs_operational++;
		}
	}

	spdk_thread_send_msg(process->thread, _raid_bdev_process_base_bdev_remove, ctx);

	return 0;
}

/*
 * [한국어]
 * _raid_bdev_remove_base_bdev - base bdev 1 개 제거의 분기 디스패처.
 *
 * @base_info: 제거 대상.
 * @cb_fn/@cb_ctx: 완료 콜백.
 * @return: 0/음수 errno (-ENODEV: 이미 제거 진행 또는 미설정).
 *
 * 동기: 4 가지 분기:
 *  (a) raid 가 ONLINE 아님 → 즉시 자원 해제 + 마지막 멤버였다면 raid free.
 *  (b) min_operational == num_base_bdevs → fault tolerance 0 → raid 전체 deconfigure.
 *  (c) process 진행 중 → raid_bdev_process_base_bdev_remove 위임.
 *  (d) 일반 경우 → quiesce → sb 업데이트 → 채널 정리 → unquiesce.
 */
static int
_raid_bdev_remove_base_bdev(struct raid_base_bdev_info *base_info,
			    raid_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct raid_bdev *raid_bdev = base_info->raid_bdev;
	int ret = 0;

	SPDK_DEBUGLOG(bdev_raid, "%s\n", base_info->name);

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (base_info->remove_scheduled || !base_info->is_configured) {
		return -ENODEV;
	}

	assert(base_info->desc);
	base_info->remove_scheduled = true;

	if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
		/*
		 * As raid bdev is not registered yet or already unregistered,
		 * so cleanup should be done here itself.
		 *
		 * Removing a base bdev at this stage does not change the number of operational
		 * base bdevs, only the number of discovered base bdevs.
		 */
		raid_bdev_free_base_bdev_resource(base_info);
		base_info->remove_scheduled = false;
		if (raid_bdev->num_base_bdevs_discovered == 0 &&
		    raid_bdev->state == RAID_BDEV_STATE_OFFLINE) {
			/* There is no base bdev for this raid, so free the raid device. */
			raid_bdev_cleanup_and_free(raid_bdev);
		}
		if (cb_fn != NULL) {
			cb_fn(cb_ctx, 0);
		}
	} else if (raid_bdev->min_base_bdevs_operational == raid_bdev->num_base_bdevs) {
		/* This raid bdev does not tolerate removing a base bdev. */
		raid_bdev->num_base_bdevs_operational--;
		raid_bdev_deconfigure(raid_bdev, cb_fn, cb_ctx);
	} else {
		base_info->remove_cb = cb_fn;
		base_info->remove_cb_ctx = cb_ctx;

		if (raid_bdev->process != NULL) {
			ret = raid_bdev_process_base_bdev_remove(raid_bdev->process, base_info);
		} else {
			ret = raid_bdev_remove_base_bdev_quiesce(base_info);
		}

		if (ret != 0) {
			base_info->remove_scheduled = false;
		}
	}

	return ret;
}

/*
 * brief:
 * raid_bdev_remove_base_bdev function is called by below layers when base_bdev
 * is removed. This function checks if this base bdev is part of any raid bdev
 * or not. If yes, it takes necessary action on that particular raid bdev.
 * params:
 * base_bdev - pointer to base bdev which got removed
 * cb_fn - callback function
 * cb_arg - argument to callback function
 * returns:
 * 0 - success
 * non zero - failure
 */
/*
 * [한국어]
 * raid_bdev_remove_base_bdev - bdev event (REMOVE) 진입점. base_bdev → base_info 매핑 후 제거.
 */
int
raid_bdev_remove_base_bdev(struct spdk_bdev *base_bdev, raid_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct raid_base_bdev_info *base_info;

	/* Find the raid_bdev which has claimed this base_bdev */
	base_info = raid_bdev_find_base_info_by_bdev(base_bdev);
	if (!base_info) {
		SPDK_ERRLOG("bdev to remove '%s' not found\n", base_bdev->name);
		return -ENODEV;
	}

	return _raid_bdev_remove_base_bdev(base_info, cb_fn, cb_ctx);
}

/*
 * [한국어]
 * raid_bdev_fail_base_remove_cb - fail_base_bdev 의 제거 완료 콜백. 실패 시 is_failed 롤백.
 */
static void
raid_bdev_fail_base_remove_cb(void *ctx, int status)
{
	struct raid_base_bdev_info *base_info = ctx;

	if (status != 0) {
		SPDK_WARNLOG("Failed to remove base bdev %s\n", base_info->name);
		base_info->is_failed = false;
	}
}

/*
 * [한국어]
 * _raid_bdev_fail_base_bdev - app thread 에서 멤버 1 개를 "failed" 로 마킹 후 제거 시도.
 *
 * 동기: RAID 모듈이 (예: raid1 mirror read 가 데이터 손상 감지) 자체적으로 멤버를 fail
 * 처리할 때 사용. 이후 슈퍼블록에 FAILED 로 기록됨 (정상 제거는 MISSING).
 */
static void
_raid_bdev_fail_base_bdev(void *ctx)
{
	struct raid_base_bdev_info *base_info = ctx;
	int rc;

	if (base_info->is_failed) {
		return;
	}
	base_info->is_failed = true;

	SPDK_NOTICELOG("Failing base bdev in slot %d ('%s') of raid bdev '%s'\n",
		       raid_bdev_base_bdev_slot(base_info), base_info->name, base_info->raid_bdev->bdev.name);

	rc = _raid_bdev_remove_base_bdev(base_info, raid_bdev_fail_base_remove_cb, base_info);
	if (rc != 0) {
		raid_bdev_fail_base_remove_cb(base_info, rc);
	}
}

/*
 * [한국어]
 * raid_bdev_fail_base_bdev - RAID 모듈에서 멤버 fail 요청. cross-thread 안전한 진입점.
 *
 * 동기: RAID 모듈은 임의 SPDK thread 에서 호출할 수 있으므로 spdk_thread_exec_msg 로
 * app thread 에 위임.
 */
void
raid_bdev_fail_base_bdev(struct raid_base_bdev_info *base_info)
{
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), _raid_bdev_fail_base_bdev, base_info);
}

/*
 * [한국어]
 * raid_bdev_resize_write_sb_cb - resize 후 슈퍼블록 갱신 완료 콜백.
 */
static void
raid_bdev_resize_write_sb_cb(int status, struct raid_bdev *raid_bdev, void *ctx)
{
	if (status != 0) {
		SPDK_ERRLOG("Failed to write raid bdev '%s' superblock after resizing the bdev: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
	}
}

/*
 * brief:
 * raid_bdev_resize_base_bdev function is called by below layers when base_bdev
 * is resized. This function checks if the smallest size of the base_bdevs is changed.
 * If yes, call module handler to resize the raid_bdev if implemented.
 * params:
 * base_bdev - pointer to base bdev which got resized.
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_resize_base_bdev - base bdev RESIZE 이벤트 핸들러. raid 전체 크기 재산정.
 *
 * 동기: base bdev (예: thin-provisioned NVMe) 의 blockcnt 증가 시. 모든 멤버의 최소
 * 블록 수가 raid 의 데이터 영역 크기를 결정 → module->resize 콜백에 위임. RAID 모듈이
 * raid.bdev.blockcnt 를 직접 갱신 (RAID 0 stripe, RAID 1 mirror 등 레벨별 식 다름).
 * superblock 활성 시 sb 도 함께 업데이트.
 */
static void
raid_bdev_resize_base_bdev(struct spdk_bdev *base_bdev)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;
	uint64_t blockcnt_old;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_resize_base_bdev\n");

	base_info = raid_bdev_find_base_info_by_bdev(base_bdev);

	/* Find the raid_bdev which has claimed this base_bdev */
	if (!base_info) {
		SPDK_ERRLOG("raid_bdev whose base_bdev '%s' not found\n", base_bdev->name);
		return;
	}
	raid_bdev = base_info->raid_bdev;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	SPDK_NOTICELOG("base_bdev '%s' was resized: old size %" PRIu64 ", new size %" PRIu64 "\n",
		       base_bdev->name, base_info->blockcnt, base_bdev->blockcnt);

	base_info->blockcnt = base_bdev->blockcnt;

	if (!raid_bdev->module->resize) {
		return;
	}

	blockcnt_old = raid_bdev->bdev.blockcnt;
	if (raid_bdev->module->resize(raid_bdev) == false) {
		return;
	}

	SPDK_NOTICELOG("raid bdev '%s': block count was changed from %" PRIu64 " to %" PRIu64 "\n",
		       raid_bdev->bdev.name, blockcnt_old, raid_bdev->bdev.blockcnt);

	if (raid_bdev->superblock_enabled) {
		struct raid_bdev_superblock *sb = raid_bdev->sb;
		uint8_t i;

		for (i = 0; i < sb->base_bdevs_size; i++) {
			struct raid_bdev_sb_base_bdev *sb_base_bdev = &sb->base_bdevs[i];

			if (sb_base_bdev->slot < raid_bdev->num_base_bdevs) {
				base_info = &raid_bdev->base_bdev_info[sb_base_bdev->slot];
				sb_base_bdev->data_size = base_info->data_size;
			}
		}
		sb->raid_size = raid_bdev->bdev.blockcnt;
		raid_bdev_write_superblock(raid_bdev, raid_bdev_resize_write_sb_cb, NULL);
	}
}

/*
 * brief:
 * raid_bdev_event_base_bdev function is called by below layers when base_bdev
 * triggers asynchronous event.
 * params:
 * type - event details.
 * bdev - bdev that triggered event.
 * event_ctx - context for event.
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_event_base_bdev - base bdev open 시 등록한 event 콜백.
 *
 * @type: REMOVE (멤버 빠짐) / RESIZE (크기 변경) 처리. 그 외 무시.
 */
static void
raid_bdev_event_base_bdev(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			  void *event_ctx)
{
	int rc;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		rc = raid_bdev_remove_base_bdev(bdev, NULL, NULL);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to remove base bdev %s: %s\n",
				    spdk_bdev_get_name(bdev), spdk_strerror(-rc));
		}
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		raid_bdev_resize_base_bdev(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/*
 * brief:
 * Deletes the specified raid bdev
 * params:
 * raid_bdev - pointer to raid bdev
 * cb_fn - callback function
 * cb_arg - argument to callback function
 */
/*
 * [한국어]
 * raid_bdev_delete - RAID bdev 전체 삭제. RPC bdev_raid_delete 의 백엔드.
 *
 * 동기: destroy_started 플래그로 재진입 차단. CONFIGURING (아직 미등록) 이면 base 자원만
 * 해제. 모두 빠지면 cleanup_and_free. ONLINE 이면 deconfigure → bdev 레이어 unregister.
 */
void
raid_bdev_delete(struct raid_bdev *raid_bdev, raid_bdev_destruct_cb cb_fn, void *cb_arg)
{
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "delete raid bdev: %s\n", raid_bdev->bdev.name);

	if (raid_bdev->destroy_started) {
		SPDK_DEBUGLOG(bdev_raid, "destroying raid bdev %s is already started\n",
			      raid_bdev->bdev.name);
		if (cb_fn) {
			cb_fn(cb_arg, -EALREADY);
		}
		return;
	}

	raid_bdev->destroy_started = true;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->remove_scheduled = true;

		if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
			/*
			 * As raid bdev is not registered yet or already unregistered,
			 * so cleanup should be done here itself.
			 */
			raid_bdev_free_base_bdev_resource(base_info);
		}
	}

	if (raid_bdev->num_base_bdevs_discovered == 0) {
		/* There is no base bdev for this raid, so free the raid device. */
		raid_bdev_cleanup_and_free(raid_bdev);
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
	} else {
		raid_bdev_deconfigure(raid_bdev, cb_fn, cb_arg);
	}
}

/*
 * [한국어]
 * raid_bdev_process_finish_write_sb_cb - process 종료 후 슈퍼블록 commit 콜백.
 */
static void
raid_bdev_process_finish_write_sb_cb(int status, struct raid_bdev *raid_bdev, void *ctx)
{
	if (status != 0) {
		SPDK_ERRLOG("Failed to write raid bdev '%s' superblock after background process finished: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
	}
}

/*
 * [한국어]
 * raid_bdev_process_finish_write_sb - rebuild 완료 후 슈퍼블록의 멤버 상태 CONFIGURED 로 갱신.
 *
 * 동기: rebuild 성공 시 target 이 정식 멤버가 되었으므로 슈퍼블록의 base_bdevs[] 에서
 * MISSING/FAILED 였던 슬롯을 CONFIGURED 로 변경 후 disk write.
 * 실행 컨텍스트: app thread (cross-thread message via spdk_thread_send_msg).
 */
static void
raid_bdev_process_finish_write_sb(void *ctx)
{
	struct raid_bdev *raid_bdev = ctx;
	struct raid_bdev_superblock *sb = raid_bdev->sb;
	struct raid_bdev_sb_base_bdev *sb_base_bdev;
	struct raid_base_bdev_info *base_info;
	uint8_t i;

	for (i = 0; i < sb->base_bdevs_size; i++) {
		sb_base_bdev = &sb->base_bdevs[i];

		if (sb_base_bdev->state != RAID_SB_BASE_BDEV_CONFIGURED &&
		    sb_base_bdev->slot < raid_bdev->num_base_bdevs) {
			base_info = &raid_bdev->base_bdev_info[sb_base_bdev->slot];
			if (base_info->is_configured) {
				sb_base_bdev->state = RAID_SB_BASE_BDEV_CONFIGURED;
				sb_base_bdev->data_offset = base_info->data_offset;
				spdk_uuid_copy(&sb_base_bdev->uuid, &base_info->uuid);
			}
		}
	}

	raid_bdev_write_superblock(raid_bdev, raid_bdev_process_finish_write_sb_cb, NULL);
}

static void raid_bdev_process_free(struct raid_bdev_process *process);
/* [한국어] forward decl — finish_done 에서 process 해제 필요. */

/*
 * [한국어]
 * _raid_bdev_process_finish_done - process thread 의 마지막 단계. finish_actions 실행 + free + thread_exit.
 *
 * 동기: 종료가 완료되어 thread 가 빠져나오는 시점. finish_actions 큐에 enqueue 된 모든
 * 콜백을 순차 실행 (예: unregister 진행). 이후 메모리 해제 + spdk_thread_exit.
 */
static void
_raid_bdev_process_finish_done(void *ctx)
{
	struct raid_bdev_process *process = ctx;
	struct raid_process_finish_action *finish_action;

	while ((finish_action = TAILQ_FIRST(&process->finish_actions)) != NULL) {
		TAILQ_REMOVE(&process->finish_actions, finish_action, link);
		finish_action->cb(finish_action->cb_ctx);
		free(finish_action);
	}

	spdk_poller_unregister(&process->qos.process_continue_poller);

	raid_bdev_process_free(process);

	spdk_thread_exit(spdk_get_thread());
}

/*
 * [한국어]
 * raid_bdev_process_finish_target_removed - rebuild 실패 시 target 까지 제거된 뒤 호출.
 */
static void
raid_bdev_process_finish_target_removed(void *ctx, int status)
{
	struct raid_bdev_process *process = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to remove target bdev: %s\n", spdk_strerror(-status));
	}

	spdk_thread_send_msg(process->thread, _raid_bdev_process_finish_done, process);
}

/*
 * [한국어]
 * raid_bdev_process_finish_unquiesced - unquiesce 완료 후 분기: 성공이면 thread 종료, 실패면 target 제거.
 */
static void
raid_bdev_process_finish_unquiesced(void *ctx, int status)
{
	struct raid_bdev_process *process = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to unquiesce bdev: %s\n", spdk_strerror(-status));
	}

	if (process->status != 0) {
		status = _raid_bdev_remove_base_bdev(process->target, raid_bdev_process_finish_target_removed,
						     process);
		if (status != 0) {
			raid_bdev_process_finish_target_removed(process, status);
		}
		return;
	}

	spdk_thread_send_msg(process->thread, _raid_bdev_process_finish_done, process);
}

/*
 * [한국어]
 * raid_bdev_process_finish_unquiesce - app thread 에서 unquiesce 발행.
 */
static void
raid_bdev_process_finish_unquiesce(void *ctx)
{
	struct raid_bdev_process *process = ctx;
	int rc;

	rc = spdk_bdev_unquiesce(&process->raid_bdev->bdev, &g_raid_if,
				 raid_bdev_process_finish_unquiesced, process);
	if (rc != 0) {
		raid_bdev_process_finish_unquiesced(process, rc);
	}
}

/*
 * [한국어]
 * raid_bdev_process_finish_done - process thread 에서 final 처리 진입 (state=STOPPED + sb 갱신 + unquiesce).
 */
static void
raid_bdev_process_finish_done(void *ctx)
{
	struct raid_bdev_process *process = ctx;
	struct raid_bdev *raid_bdev = process->raid_bdev;

	if (process->raid_ch != NULL) {
		spdk_put_io_channel(spdk_io_channel_from_ctx(process->raid_ch));
	}

	process->state = RAID_PROCESS_STATE_STOPPED;

	if (process->status == 0) {
		SPDK_NOTICELOG("Finished %s on raid bdev %s\n",
			       raid_bdev_process_to_str(process->type),
			       raid_bdev->bdev.name);
		if (raid_bdev->superblock_enabled) {
			spdk_thread_send_msg(spdk_thread_get_app_thread(),
					     raid_bdev_process_finish_write_sb,
					     raid_bdev);
		}
	} else {
		SPDK_WARNLOG("Finished %s on raid bdev %s: %s\n",
			     raid_bdev_process_to_str(process->type),
			     raid_bdev->bdev.name,
			     spdk_strerror(-process->status));
	}

	spdk_thread_send_msg(spdk_thread_get_app_thread(), raid_bdev_process_finish_unquiesce,
			     process);
}

/*
 * [한국어]
 * __raid_bdev_process_finish - for_each_channel 종료 후 process thread 로 전환.
 */
static void
__raid_bdev_process_finish(struct spdk_io_channel_iter *i, int status)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);

	spdk_thread_send_msg(process->thread, raid_bdev_process_finish_done, process);
}

/*
 * [한국어]
 * raid_bdev_channel_process_finish - 채널 iter — 성공 시 target_ch 를 정식 슬롯으로 승격 + cleanup.
 *
 * 동기: rebuild 가 성공적으로 끝나면 채널의 target_ch (rebuild 임시 채널) 가 곧 정식
 * base 채널이 된다. status != 0 (실패) 면 단순 cleanup 만 (raid_bdev_process_finish_unquiesced
 * 가 target 을 제거할 것).
 */
static void
raid_bdev_channel_process_finish(struct spdk_io_channel_iter *i)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct raid_bdev_io_channel *raid_ch = spdk_io_channel_get_ctx(ch);

	if (process->status == 0) {
		uint8_t slot = raid_bdev_base_bdev_slot(process->target);

		raid_ch->base_channel[slot] = raid_ch->process.target_ch;
		raid_ch->process.target_ch = NULL;
	}

	raid_bdev_ch_process_cleanup(raid_ch);

	spdk_for_each_channel_continue(i, 0);
}

/*
 * [한국어]
 * raid_bdev_process_finish_quiesced - quiesce 완료 후 raid_bdev->process 분리 + 각 채널 finish iter.
 */
static void
raid_bdev_process_finish_quiesced(void *ctx, int status)
{
	struct raid_bdev_process *process = ctx;
	struct raid_bdev *raid_bdev = process->raid_bdev;

	if (status != 0) {
		SPDK_ERRLOG("Failed to quiesce bdev: %s\n", spdk_strerror(-status));
		return;
	}

	raid_bdev->process = NULL;
	process->target->is_process_target = false;

	spdk_for_each_channel(process->raid_bdev, raid_bdev_channel_process_finish, process,
			      __raid_bdev_process_finish);
}

/*
 * [한국어]
 * _raid_bdev_process_finish - app thread 에서 raid 전체 quiesce 발행.
 *
 * 동기: process 가 끝났음을 raid 전체에 알리기 위해 quiesce → 진행 중 IO drain → 채널
 * 정리 → unquiesce → thread 종료 순서.
 */
static void
_raid_bdev_process_finish(void *ctx)
{
	struct raid_bdev_process *process = ctx;
	int rc;

	rc = spdk_bdev_quiesce(&process->raid_bdev->bdev, &g_raid_if,
			       raid_bdev_process_finish_quiesced, process);
	if (rc != 0) {
		raid_bdev_process_finish_quiesced(ctx, rc);
	}
}

/*
 * [한국어]
 * raid_bdev_process_do_finish - process thread → app thread 로 전환하여 quiesce 시작.
 */
static void
raid_bdev_process_do_finish(struct raid_bdev_process *process)
{
	spdk_thread_send_msg(spdk_thread_get_app_thread(), _raid_bdev_process_finish, process);
}

static void raid_bdev_process_unlock_window_range(struct raid_bdev_process *process);
static void raid_bdev_process_thread_run(struct raid_bdev_process *process);
/* [한국어] forward decl — finish/thread_run 상호 참조. */

/*
 * [한국어]
 * raid_bdev_process_finish - process 종료 진입 (process thread). RUNNING → STOPPING.
 *
 * @process: 종료 대상.
 * @status: 종료 사유 (0=정상 완료, 음수=에러).
 *
 * 동기: 정상 완료 / 에러 / 외부 요청 (unregister, base 제거) 등 다양한 경로의 공통 종료
 * 진입점. window_range_locked 상태에 따라 unlock 또는 thread_run (do_finish) 호출.
 */
static void
raid_bdev_process_finish(struct raid_bdev_process *process, int status)
{
	assert(spdk_get_thread() == process->thread);

	if (process->status == 0) {
		process->status = status;
	}

	if (process->state >= RAID_PROCESS_STATE_STOPPING) {
		return;
	}

	assert(process->state == RAID_PROCESS_STATE_RUNNING);
	process->state = RAID_PROCESS_STATE_STOPPING;

	if (process->window_range_locked) {
		raid_bdev_process_unlock_window_range(process);
	} else {
		raid_bdev_process_thread_run(process);
	}
}

/*
 * [한국어]
 * raid_bdev_process_window_range_unlocked - 윈도우 unquiesce 완료 콜백.
 *
 * 동기: window_offset 을 다음 윈도우로 진행시킨 후 thread_run.
 */
static void
raid_bdev_process_window_range_unlocked(void *ctx, int status)
{
	struct raid_bdev_process *process = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to unlock LBA range: %s\n", spdk_strerror(-status));
		raid_bdev_process_finish(process, status);
		return;
	}

	process->window_range_locked = false;
	process->window_offset += process->window_size;

	raid_bdev_process_thread_run(process);
}

/*
 * [한국어]
 * raid_bdev_process_unlock_window_range - 처리 완료된 윈도우를 unquiesce.
 *
 * 동기: 차단되어 있던 application I/O 가 다시 흐를 수 있게 unlock. unlock 완료는
 * window_range_unlocked 콜백으로.
 */
static void
raid_bdev_process_unlock_window_range(struct raid_bdev_process *process)
{
	int rc;

	assert(process->window_range_locked == true);

	rc = spdk_bdev_unquiesce_range(&process->raid_bdev->bdev, &g_raid_if,
				       process->window_offset, process->max_window_size,
				       raid_bdev_process_window_range_unlocked, process);
	if (rc != 0) {
		raid_bdev_process_window_range_unlocked(process, rc);
	}
}

/*
 * [한국어]
 * raid_bdev_process_channels_update_done - 모든 채널의 process.offset 갱신 후 unlock 단계로.
 */
static void
raid_bdev_process_channels_update_done(struct spdk_io_channel_iter *i, int status)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);

	raid_bdev_process_unlock_window_range(process);
}

/*
 * [한국어]
 * raid_bdev_process_channel_update - 채널 iter — 각 채널의 process.offset 을 이번 윈도우 끝으로 진행.
 *
 * 동기: 이 호출 이후 _raid_bdev_request_split 가 이번 윈도우 영역을 "처리 완료" 로 인식 →
 * application I/O 가 ch_processed 로 라우팅되어 target 까지 mirror 된다.
 */
static void
raid_bdev_process_channel_update(struct spdk_io_channel_iter *i)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct raid_bdev_io_channel *raid_ch = spdk_io_channel_get_ctx(ch);

	raid_ch->process.offset = process->window_offset + process->window_size;

	spdk_for_each_channel_continue(i, 0);
}

/*
 * [한국어]
 * raid_bdev_process_request_complete - RAID 모듈이 process 요청 완료를 통지.
 *
 * @process_req: 완료된 요청.
 * @status: 결과.
 *
 * 동기: process->window_remaining 카운터 감소. 0 도달 시 채널 offset 일괄 update → unlock.
 * 호출자: RAID 레벨 모듈 (raid1/raid5f 등) 이 자기 submit_process_request 의 끝에서.
 */
void
raid_bdev_process_request_complete(struct raid_bdev_process_request *process_req, int status)
{
	struct raid_bdev_process *process = process_req->process;

	TAILQ_INSERT_TAIL(&process->requests, process_req, link);

	assert(spdk_get_thread() == process->thread);
	assert(process->window_remaining >= process_req->num_blocks);

	if (status != 0) {
		process->window_status = status;
	}

	process->window_remaining -= process_req->num_blocks;
	if (process->window_remaining == 0) {
		if (process->window_status != 0) {
			raid_bdev_process_finish(process, process->window_status);
			return;
		}

		spdk_for_each_channel(process->raid_bdev, raid_bdev_process_channel_update, process,
				      raid_bdev_process_channels_update_done);
	}
}

/*
 * [한국어]
 * raid_bdev_submit_process_request - process 요청 1 개를 모듈로 디스패치.
 *
 * @process: 진행 중 process.
 * @offset_blocks/num_blocks: 발행 범위.
 * @return: 발행된 블록 수, 0=재시도 필요, 음수=에러.
 *
 * 동기: requests 풀에서 idle 요청 꺼내 모듈의 submit_process_request 호출. 모듈이 더 작은
 * 단위로 잘라 발행했을 수 있어 실제 발행량 (반환값) 으로 process_req 와 window 진행 동기화.
 */
static int
raid_bdev_submit_process_request(struct raid_bdev_process *process, uint64_t offset_blocks,
				 uint32_t num_blocks)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	struct raid_bdev_process_request *process_req;
	int ret;

	process_req = TAILQ_FIRST(&process->requests);
	if (process_req == NULL) {
		assert(process->window_remaining > 0);
		return 0;
	}

	process_req->target = process->target;
	process_req->target_ch = process->raid_ch->process.target_ch;
	process_req->offset_blocks = offset_blocks;
	process_req->num_blocks = num_blocks;
	process_req->iov.iov_len = num_blocks * raid_bdev->bdev.blocklen;

	ret = raid_bdev->module->submit_process_request(process_req, process->raid_ch);
	if (ret <= 0) {
		if (ret < 0) {
			SPDK_ERRLOG("Failed to submit process request on %s: %s\n",
				    raid_bdev->bdev.name, spdk_strerror(-ret));
			process->window_status = ret;
		}
		return ret;
	}

	process_req->num_blocks = ret;
	TAILQ_REMOVE(&process->requests, process_req, link);

	return ret;
}

/*
 * [한국어]
 * _raid_bdev_process_thread_run - 윈도우 범위 lock 직후 호출되어 process 요청 다발 발행.
 *
 * 동기: max_window_size 만큼 max_QD 안에서 요청 발행. 발행 0 이면 window_status 로 finish.
 */
static void
_raid_bdev_process_thread_run(struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	uint64_t offset = process->window_offset;
	const uint64_t offset_end = spdk_min(offset + process->max_window_size, raid_bdev->bdev.blockcnt);
	int ret;

	while (offset < offset_end) {
		ret = raid_bdev_submit_process_request(process, offset, offset_end - offset);
		if (ret <= 0) {
			break;
		}

		process->window_remaining += ret;
		offset += ret;
	}

	if (process->window_remaining > 0) {
		process->window_size = process->window_remaining;
	} else {
		raid_bdev_process_finish(process, process->window_status);
	}
}

/*
 * [한국어]
 * raid_bdev_process_window_range_locked - 윈도우 범위 lock (quiesce) 완료 콜백.
 *
 * 동기: lock 이 성공해야 안전하게 데이터 이전 가능. STOPPING 상태면 즉시 unlock 으로 종료.
 */
static void
raid_bdev_process_window_range_locked(void *ctx, int status)
{
	struct raid_bdev_process *process = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to lock LBA range: %s\n", spdk_strerror(-status));
		raid_bdev_process_finish(process, status);
		return;
	}

	process->window_range_locked = true;

	if (process->state == RAID_PROCESS_STATE_STOPPING) {
		raid_bdev_process_unlock_window_range(process);
		return;
	}

	_raid_bdev_process_thread_run(process);
}

/*
 * [한국어]
 * raid_bdev_process_consume_token - QoS token bucket 차감 (대역폭 제한).
 *
 * @process: QoS 활성화된 process.
 * @return: true = 토큰 충분 (윈도우 처리 가능), false = 토큰 부족.
 *
 * 동기: 토큰 = bytes_available. 마지막 충전 이후 경과 tick × bytes_per_tsc 만큼 누적하고
 * 최대 bytes_max 로 캡. 충분하면 윈도우 크기만큼 차감.
 */
static bool
raid_bdev_process_consume_token(struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	uint64_t now = spdk_get_ticks();

	process->qos.bytes_available = spdk_min(process->qos.bytes_max,
						process->qos.bytes_available +
						(now - process->qos.last_tsc) * process->qos.bytes_per_tsc);
	process->qos.last_tsc = now;
	if (process->qos.bytes_available > 0.0) {
		process->qos.bytes_available -= process->window_size * raid_bdev->bdev.blocklen;
		return true;
	}
	return false;
}

/*
 * [한국어]
 * raid_bdev_process_lock_window_range - 새 윈도우 처리 시작 — quiesce_range 발행.
 *
 * @process: 활성 process.
 * @return: true = lock 시도 성공 (또는 sync 에러로 곧 콜백), false = QoS 토큰 부족.
 *
 * 동기: QoS 가 있으면 토큰 확인 → 부족 시 continue poller 활성화하여 충전 대기. 토큰 충분
 * 또는 QoS 비활성이면 quiesce_range 발행. quiesce 완료는 window_range_locked 콜백.
 */
static bool
raid_bdev_process_lock_window_range(struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	int rc;

	assert(process->window_range_locked == false);

	if (process->qos.enable_qos) {
		if (raid_bdev_process_consume_token(process)) {
			spdk_poller_pause(process->qos.process_continue_poller);
		} else {
			spdk_poller_resume(process->qos.process_continue_poller);
			return false;
		}
	}

	rc = spdk_bdev_quiesce_range(&raid_bdev->bdev, &g_raid_if,
				     process->window_offset, process->max_window_size,
				     raid_bdev_process_window_range_locked, process);
	if (rc != 0) {
		raid_bdev_process_window_range_locked(process, rc);
	}
	return true;
}

/*
 * [한국어]
 * raid_bdev_process_continue_poll - QoS 토큰 충전 대기 poller.
 *
 * @arg: process.
 * @return: SPDK_POLLER_BUSY 면 토큰 확보됨 / IDLE 면 아직 부족.
 *
 * 동기: lock_window_range 가 토큰 부족으로 false 반환 시 활성화. 토큰이 충분해지면
 * lock_window_range true → 자체 pause.
 */
static int
raid_bdev_process_continue_poll(void *arg)
{
	struct raid_bdev_process *process = arg;

	if (raid_bdev_process_lock_window_range(process)) {
		return SPDK_POLLER_BUSY;
	}
	return SPDK_POLLER_IDLE;
}

/*
 * [한국어]
 * raid_bdev_process_thread_run - process thread 의 메인 루프 진입점.
 *
 * 동기: 윈도우 단위 사이클의 시작. STOPPING 이면 do_finish 로 종료. blockcnt 도달 시
 * finish(0) — 정상 완료. 아니면 max_window_size 계산 후 lock_window_range.
 */
static void
raid_bdev_process_thread_run(struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;

	assert(spdk_get_thread() == process->thread);
	assert(process->window_remaining == 0);
	assert(process->window_range_locked == false);

	if (process->state == RAID_PROCESS_STATE_STOPPING) {
		raid_bdev_process_do_finish(process);
		return;
	}

	if (process->window_offset == raid_bdev->bdev.blockcnt) {
		SPDK_DEBUGLOG(bdev_raid, "process completed on %s\n", raid_bdev->bdev.name);
		raid_bdev_process_finish(process, 0);
		return;
	}

	process->max_window_size = spdk_min(raid_bdev->bdev.blockcnt - process->window_offset,
					    process->max_window_size);
	raid_bdev_process_lock_window_range(process);
}

/*
 * [한국어]
 * raid_bdev_process_thread_init - process 전용 thread 의 진입점 (spdk_thread_send_msg 로 호출).
 *
 * 동기: 새 SPDK thread 가 자기 컨텍스트 (raid 채널, QoS poller) 를 초기화 후 thread_run.
 */
static void
raid_bdev_process_thread_init(void *ctx)
{
	struct raid_bdev_process *process = ctx;
	struct raid_bdev *raid_bdev = process->raid_bdev;
	struct spdk_io_channel *ch;

	process->thread = spdk_get_thread();

	ch = spdk_get_io_channel(raid_bdev);
	if (ch == NULL) {
		process->status = -ENOMEM;
		raid_bdev_process_do_finish(process);
		return;
	}

	process->raid_ch = spdk_io_channel_get_ctx(ch);
	process->state = RAID_PROCESS_STATE_RUNNING;

	if (process->qos.enable_qos) {
		process->qos.process_continue_poller = SPDK_POLLER_REGISTER(raid_bdev_process_continue_poll,
						       process, 0);
		spdk_poller_pause(process->qos.process_continue_poller);
	}

	SPDK_NOTICELOG("Started %s on raid bdev %s\n",
		       raid_bdev_process_to_str(process->type), raid_bdev->bdev.name);

	raid_bdev_process_thread_run(process);
}

/*
 * [한국어]
 * raid_bdev_channels_abort_start_process_done - process 시작 롤백(abort)의 모든 채널 정리 완료 콜백.
 *
 * @i: spdk_for_each_channel 반복자. ctx 에 raid_bdev_process 포인터가 들어 있음.
 * @status: 채널 abort 단계의 누적 상태(여기선 항상 0 — abort 는 실패하지 않음).
 *
 * 동기: process 시작이 도중에 실패하면(예: thread 생성 실패, base bdev 가 먼저 제거됨)
 * 이미 각 채널에 부분적으로 설정해 둔 process 컨텍스트를 모두 되돌려야 한다. 이 함수는
 * 모든 채널에서 raid_bdev_channel_abort_start_process 가 끝난 뒤 한 번 호출되어, rebuild
 * 대상(target) base bdev 를 RAID 에서 제거하고 process 객체 메모리를 해제하는 마지막 단계다.
 *
 * 동작 단계:
 *   1) 반복자 ctx 에서 process 복원.
 *   2) _raid_bdev_remove_base_bdev(target) — rebuild 하려던 target base bdev 를 array 에서 제거.
 *      (시작에 실패했으므로 reconstruction 대상이었던 슬롯을 비워 둔다.)
 *   3) raid_bdev_process_free(process) — 미리 할당된 process_request 풀과 process 자체 해제.
 *
 * 실행 컨텍스트: app thread(spdk_for_each_channel 의 완료 콜백은 호출 thread 에서 실행).
 *
 * 호출 체인:
 *   raid_bdev_channels_start_process_done(err) → spdk_for_each_channel(abort) →
 *     [raid_bdev_channels_abort_start_process_done] → _raid_bdev_remove_base_bdev / raid_bdev_process_free
 */
static void
raid_bdev_channels_abort_start_process_done(struct spdk_io_channel_iter *i, int status)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] 반복자 컨텍스트에서 abort 대상 process 복원 — 시작 실패로 롤백할 객체. */

	_raid_bdev_remove_base_bdev(process->target, NULL, NULL);
	/* [한국어] rebuild 대상이던 target base bdev 를 RAID array 에서 제거(슬롯 비움).
	 * cb_fn/cb_ctx 가 NULL 이므로 제거 완료를 기다리지 않는 fire-and-forget 호출. */
	raid_bdev_process_free(process);
	/* [한국어] process 객체와 그에 딸린 process_request 풀(DMA 버퍼 포함) 전부 해제. */

	/* TODO: update sb */
	/* [한국어] 원본 주석: 향후 superblock 의 상태를 갱신해 target 제거를 영속화할 필요가 있음. */
}

/*
 * [한국어]
 * raid_bdev_channel_abort_start_process - 채널 1개에서 부분 설정된 process 컨텍스트를 정리하는 per-channel 콜백.
 *
 * @i: spdk_for_each_channel 반복자. 현재 순회 중인 채널과 ctx(process)를 담음.
 *
 * 동기: process 시작 실패 시 각 raid 채널마다 raid_bdev_ch_process_setup 으로 만들어 둔
 * process 전용 가상 채널(process_target_ch / process_open_window_ch 등)을 해제해야 한다.
 * spdk_for_each_channel 은 모든 코어의 채널을 순차 방문하며, 각 채널에서 이 콜백을 실행한다.
 *
 * 동작 단계:
 *   1) 반복자에서 현재 채널 → raid_bdev_io_channel(raid_ch) 컨텍스트 추출.
 *   2) raid_bdev_ch_process_cleanup(raid_ch) — 이 채널의 process 관련 자원 해제.
 *   3) spdk_for_each_channel_continue(i, 0) — 다음 채널로 진행(0=계속).
 *
 * 실행 컨텍스트: 각 채널을 소유한 코어의 spdk_thread (spdk_for_each_channel 이 메시지로 디스패치).
 *
 * 호출 체인:
 *   spdk_for_each_channel(abort) → [raid_bdev_channel_abort_start_process] →
 *     raid_bdev_ch_process_cleanup → (모든 채널 후) raid_bdev_channels_abort_start_process_done
 */
static void
raid_bdev_channel_abort_start_process(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	/* [한국어] 현재 순회 중인 spdk_io_channel — 특정 코어의 raid 채널. */
	struct raid_bdev_io_channel *raid_ch = spdk_io_channel_get_ctx(ch);
	/* [한국어] 채널의 raid 전용 컨텍스트(base_channel 배열, process target 채널 등). */

	raid_bdev_ch_process_cleanup(raid_ch);
	/* [한국어] 이 채널에 설정된 process 가상 채널/타깃 채널을 해제 — setup 의 역연산. */

	spdk_for_each_channel_continue(i, 0);
	/* [한국어] 0(성공)을 전달하며 다음 채널 순회로 진행. abort 는 실패하지 않으므로 항상 0. */
}

/*
 * [한국어]
 * raid_bdev_channels_start_process_done - 모든 채널에 process 설정이 끝난 뒤 process 전용 thread 를 띄우는 완료 콜백.
 *
 * @i: spdk_for_each_channel 반복자. ctx 에 process 포인터.
 * @status: per-channel setup 단계의 누적 상태(0=모든 채널 setup 성공, 음수=어느 채널 실패).
 *
 * 동기: rebuild 같은 background process 를 시작하려면 (1) 먼저 모든 데이터 채널에 process
 * 컨텍스트를 깔고(raid_bdev_channel_start_process), (2) 그다음 전용 SPDK thread 를 생성해
 * 그 위에서 윈도우 단위 데이터 이전을 돌려야 한다. 이 함수는 (1)이 끝난 뒤 (2)를 수행한다.
 * 설정 도중 base bdev 가 제거되었거나 운영 가능 멤버 수가 최소 이하로 떨어졌으면 시작을 포기한다.
 *
 * 동작 단계:
 *   1) status==0 이라도 target 이 remove 예약/미구성 상태이거나 operational 멤버 수가
 *      최소치 이하면 경쟁적으로 base bdev 가 빠진 것 → -ENODEV 로 강제 실패.
 *   2) status!=0 이면 에러 로그 후 err 로 점프하여 부분 설정 롤백.
 *   3) "<raid이름>_<process종류>" 형식 thread 이름 구성.
 *   4) spdk_thread_create — process 전용 SPDK thread 생성(별도 코어/스케줄링 단위).
 *   5) raid_bdev->process 에 게시(이제 RAID 가 process 진행 중임을 외부에 노출).
 *   6) spdk_thread_send_msg(thread, raid_bdev_process_thread_init) — 새 thread 컨텍스트에서
 *      초기화가 실행되도록 메시지 전달(cross-thread 안전 디스패치).
 *
 * 실행 컨텍스트: app thread. thread_init 은 새로 만든 process thread 에서 실행됨.
 *
 * 호출 체인:
 *   raid_bdev_process_start → spdk_for_each_channel(start) →
 *     [raid_bdev_channels_start_process_done] → spdk_thread_create / spdk_thread_send_msg(thread_init)
 *   (실패) → spdk_for_each_channel(abort) → raid_bdev_channels_abort_start_process_done
 */
static void
raid_bdev_channels_start_process_done(struct spdk_io_channel_iter *i, int status)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] 시작하려는 process 객체 복원. */
	struct raid_bdev *raid_bdev = process->raid_bdev;
	/* [한국어] process 가 속한 RAID bdev — thread 이름/상태 갱신에 사용. */
	struct spdk_thread *thread;
	/* [한국어] 새로 생성할 process 전용 SPDK thread 핸들. */
	char thread_name[RAID_BDEV_SB_NAME_SIZE + 16];
	/* [한국어] thread 이름 버퍼. RAID 이름(최대 SB_NAME_SIZE) + "_<process종류>" 여유 16바이트. */

	if (status == 0 &&
	    (process->target->remove_scheduled || !process->target->is_configured ||
	     raid_bdev->num_base_bdevs_operational <= raid_bdev->min_base_bdevs_operational)) {
		/* a base bdev was removed before we got here */
		/* [한국어] setup 자체는 성공했지만 그사이 target 이 제거 예약되었거나, 미구성 상태이거나,
		 * 운영 가능 멤버 수가 최소 이하로 떨어진 경쟁 상황 → rebuild 의미 없음. */
		status = -ENODEV;
		/* [한국어] 디바이스 없음으로 강제 실패시켜 아래 err 경로 진입. */
	}

	if (status != 0) {
		/* [한국어] setup 실패 또는 위 경쟁 상황 → 시작 포기. */
		SPDK_ERRLOG("Failed to start %s on %s: %s\n",
			    raid_bdev_process_to_str(process->type), raid_bdev->bdev.name,
			    spdk_strerror(-status));
		/* [한국어] process 종류(예: "rebuild")와 RAID 이름, errno 문자열로 에러 로그. */
		goto err;
		/* [한국어] 부분 설정된 채널 컨텍스트를 롤백하기 위해 abort 경로로 점프. */
	}

	snprintf(thread_name, sizeof(thread_name), "%s_%s",
		 raid_bdev->bdev.name, raid_bdev_process_to_str(process->type));
	/* [한국어] "<raid>_<rebuild>" 형식으로 thread 이름 구성 — trace/디버깅 식별용. */

	thread = spdk_thread_create(thread_name, NULL);
	/* [한국어] process 전용 SPDK thread 생성(cpumask=NULL → 기본 스케줄러 배치).
	 * 이 thread 가 윈도우 단위 데이터 이전 루프를 독립적으로 구동한다. */
	if (thread == NULL) {
		/* [한국어] thread 생성 실패(메모리/스케줄러 한계) → 롤백. */
		SPDK_ERRLOG("Failed to create %s thread for %s\n",
			    raid_bdev_process_to_str(process->type), raid_bdev->bdev.name);
		goto err;
	}

	raid_bdev->process = process;
	/* [한국어] RAID 에 진행 중 process 게시 — 이후 add/remove base bdev 등이 이를 보고 동작 제한. */

	spdk_thread_send_msg(thread, raid_bdev_process_thread_init, process);
	/* [한국어] 새 process thread 에 thread_init 실행을 메시지로 전달.
	 * cross-thread 직접 호출 대신 send_msg 를 쓰는 이유: thread 컨텍스트(get_io_channel,
	 * poller 등록 등)는 반드시 그 thread 위에서 초기화되어야 lockless 모델이 성립하기 때문. */

	return;
	/* [한국어] 정상 경로 종료 — 이후는 process thread 가 이어받음. */
err:
	spdk_for_each_channel(process->raid_bdev, raid_bdev_channel_abort_start_process, process,
			      raid_bdev_channels_abort_start_process_done);
	/* [한국어] 모든 채널을 다시 순회하며 부분 설정을 정리(abort)하고, 완료 시 target 제거 + process 해제. */
}

/*
 * [한국어]
 * raid_bdev_channel_start_process - 채널 1개에 process 전용 컨텍스트를 설치하는 per-channel 콜백.
 *
 * @i: spdk_for_each_channel 반복자(현재 채널 + process ctx).
 *
 * 동기: rebuild 가 동작하려면 모든 데이터 채널이 "처리 완료 영역 / 미처리 영역"을 구분하고
 * target base bdev 로의 채널을 알아야 한다. 이 콜백이 각 채널에서 그 가상 채널들을 만든다.
 *
 * 동작 단계:
 *   1) 반복자에서 process 와 현재 채널의 raid_ch 추출.
 *   2) raid_bdev_ch_process_setup(raid_ch, process) — target 채널/윈도우 채널 생성(실패 시 음수).
 *   3) spdk_for_each_channel_continue(i, rc) — rc 를 전파하며 다음 채널로(rc!=0 이면 순회 중단되고
 *      완료 콜백에 status 로 전달됨).
 *
 * 실행 컨텍스트: 각 채널 소유 코어의 spdk_thread.
 *
 * 호출 체인:
 *   raid_bdev_process_start → spdk_for_each_channel(start) → [raid_bdev_channel_start_process]
 *     → raid_bdev_ch_process_setup → (모든 채널 후) raid_bdev_channels_start_process_done
 */
static void
raid_bdev_channel_start_process(struct spdk_io_channel_iter *i)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] 설정 대상 process. */
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	/* [한국어] 현재 순회 중인 채널. */
	struct raid_bdev_io_channel *raid_ch = spdk_io_channel_get_ctx(ch);
	/* [한국어] 채널의 raid 전용 컨텍스트 — 여기에 process 채널을 단다. */
	int rc;
	/* [한국어] setup 결과(0=성공, 음수 errno=실패). */

	rc = raid_bdev_ch_process_setup(raid_ch, process);
	/* [한국어] 이 채널에 process target 채널 및 윈도우 처리 채널을 설치. */

	spdk_for_each_channel_continue(i, rc);
	/* [한국어] rc 를 전파하며 다음 채널로. rc!=0 이면 순회가 중단되고 완료 콜백이 그 status 를 받음. */
}

/*
 * [한국어]
 * raid_bdev_process_start - background process(rebuild) 시작 진입점 — 전 채널에 설정을 뿌리는 fan-out.
 *
 * @process: raid_bdev_process_alloc 로 만들어진, 아직 시작되지 않은 process 객체.
 *
 * 동기: process 를 시작하려면 모든 코어의 채널에 동일한 설정을 일관되게 적용해야 한다.
 * spdk_for_each_channel 이 그 fan-out/fan-in(완료 집계)을 lockless 하게 보장한다.
 *
 * 동작 단계:
 *   1) RAID 레벨 모듈이 submit_process_request 콜백을 구현하는지 assert(없으면 데이터 이전 불가).
 *   2) spdk_for_each_channel — 각 채널에서 raid_bdev_channel_start_process 실행,
 *      전부 끝나면 raid_bdev_channels_start_process_done 호출.
 *
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인:
 *   raid_bdev_start_rebuild → [raid_bdev_process_start] → spdk_for_each_channel(...)
 */
static void
raid_bdev_process_start(struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	/* [한국어] process 가 속한 RAID. for_each_channel 의 io_device 키로 사용. */

	assert(raid_bdev->module->submit_process_request != NULL);
	/* [한국어] RAID 레벨 모듈(raid1/raid5f 등)이 process 데이터 이전 요청 핸들러를 제공해야 함을 보장. */

	spdk_for_each_channel(raid_bdev, raid_bdev_channel_start_process, process,
			      raid_bdev_channels_start_process_done);
	/* [한국어] 전 채널에 start_process 적용 후 done 콜백에서 process thread 를 띄운다. */
}

/*
 * [한국어]
 * raid_bdev_process_request_free - process_request 1개와 그 DMA 버퍼들을 해제.
 *
 * @process_req: 해제할 요청. iov.iov_base(데이터)와 md_buf(메타데이터)는 spdk_dma_malloc 로 할당된 핀 메모리.
 *
 * 동기: process 요청은 DMA 가능한 hugepage 메모리(spdk_dma_malloc)를 데이터/메타데이터용으로
 * 들고 있으므로, 일반 free 가 아니라 spdk_dma_free 로 먼저 풀어야 메모리 누수/IOVA 매핑 잔재를 막는다.
 *
 * 동작 단계: iov_base(데이터 버퍼) → md_buf(메타데이터 버퍼) → 구조체 자체 순으로 해제.
 * (md_buf 가 NULL 이어도 spdk_dma_free(NULL) 은 무해하다.)
 *
 * 실행 컨텍스트: process thread 또는 app thread(할당 실패 롤백 경로).
 *
 * 호출 체인:
 *   raid_bdev_process_alloc_request(실패) / raid_bdev_process_free → [raid_bdev_process_request_free]
 */
static void
raid_bdev_process_request_free(struct raid_bdev_process_request *process_req)
{
	spdk_dma_free(process_req->iov.iov_base);
	/* [한국어] 윈도우 데이터용 DMA 버퍼 해제 — hugepage 기반 핀 메모리이므로 dma_free 사용. */
	spdk_dma_free(process_req->md_buf);
	/* [한국어] 분리(separate) 메타데이터 버퍼 해제. md 비분리면 NULL → no-op. */
	free(process_req);
	/* [한국어] 요청 구조체 본체 해제(일반 heap). */
}

/*
 * [한국어]
 * raid_bdev_process_alloc_request - process 1개에 대한 in-flight 요청 슬롯 하나를 할당.
 *
 * @process: 이 요청이 속할 process. max_window_size 와 RAID blocklen 으로 버퍼 크기를 산정.
 * @return: 초기화된 process_request 포인터(성공) / NULL(메모리 부족).
 *
 * 동기: rebuild 는 한 윈도우(최대 max_window_size 블록)를 통째로 읽어 target 에 쓰는 식으로
 * 진행되므로, 윈도우 1개를 담을 수 있는 데이터/메타데이터 DMA 버퍼를 미리 확보한 요청이 필요하다.
 * RAID_BDEV_PROCESS_MAX_QD 개를 풀로 만들어 두고 재사용한다.
 *
 * 동작 단계:
 *   1) 요청 구조체 calloc(0 초기화).
 *   2) process 역참조 저장, iov 길이 = max_window_size * blocklen 계산.
 *   3) 데이터 버퍼를 4096바이트 정렬로 spdk_dma_malloc(핀 메모리, IOVA 매핑 가능).
 *   4) 분리 메타데이터를 쓰는 RAID 면 md_buf 도 4096정렬로 추가 할당.
 *   에러: 어느 단계든 실패하면 이미 잡은 자원을 풀고 NULL 반환.
 *
 * 실행 컨텍스트: app thread(process 생성 시점).
 *
 * 호출 체인:
 *   raid_bdev_process_alloc → [raid_bdev_process_alloc_request] → spdk_dma_malloc / spdk_bdev_is_md_separate
 */
static struct raid_bdev_process_request *
raid_bdev_process_alloc_request(struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	/* [한국어] blocklen / md_len 조회용 RAID bdev. */
	struct raid_bdev_process_request *process_req;
	/* [한국어] 새로 만들 요청 슬롯. */

	process_req = calloc(1, sizeof(*process_req));
	/* [한국어] 요청 구조체를 0으로 초기화하여 할당(미설정 포인터를 NULL 로 보장). */
	if (process_req == NULL) {
		/* [한국어] 구조체 할당 실패 → 즉시 NULL. */
		return NULL;
	}

	process_req->process = process;
	/* [한국어] 완료 시 어느 process 에 보고할지 역참조 저장. */
	process_req->iov.iov_len = process->max_window_size * raid_bdev->bdev.blocklen;
	/* [한국어] 한 윈도우 데이터 길이 = 최대 윈도우 블록 수 × 블록 크기(바이트). */
	process_req->iov.iov_base = spdk_dma_malloc(process_req->iov.iov_len, 4096, 0);
	/* [한국어] 데이터 버퍼를 4096바이트 정렬로 DMA 가능 메모리에 할당.
	 * 4096 정렬: NVMe PRP 페이지 경계/디바이스 정렬 요건을 만족시키기 위함. */
	if (process_req->iov.iov_base == NULL) {
		/* [한국어] 데이터 버퍼 실패 → 구조체만 풀고 NULL. */
		free(process_req);
		return NULL;
	}
	if (spdk_bdev_is_md_separate(&raid_bdev->bdev)) {
		/* [한국어] 메타데이터가 데이터와 분리 저장되는 포맷이면 별도 버퍼 필요. */
		process_req->md_buf = spdk_dma_malloc(process->max_window_size * raid_bdev->bdev.md_len, 4096, 0);
		/* [한국어] 윈도우 블록 수 × md_len 크기의 메타데이터 DMA 버퍼(4096 정렬). */
		if (process_req->md_buf == NULL) {
			/* [한국어] md 버퍼 실패 → 데이터 버퍼+구조체까지 일괄 해제 후 NULL. */
			raid_bdev_process_request_free(process_req);
			return NULL;
		}
	}

	return process_req;
	/* [한국어] 데이터(+메타) 버퍼가 준비된 요청 반환. */
}

/*
 * [한국어]
 * raid_bdev_process_free - process 객체와 그에 매달린 요청 풀 전체를 해제.
 *
 * @process: 해제할 process. requests TAILQ 에 idle 요청들이 들어 있어야 함(in-flight 없음 전제).
 *
 * 동기: process 시작 실패 또는 종료 후, 미리 풀로 잡아 둔 RAID_BDEV_PROCESS_MAX_QD 개의
 * 요청(각각 DMA 버퍼 포함)과 process 본체를 누수 없이 회수해야 한다.
 *
 * 동작 단계:
 *   1) requests 큐에서 요청을 하나씩 dequeue 하며 raid_bdev_process_request_free 로 해제.
 *   2) 큐가 비면 process 구조체 자체 free.
 *
 * 실행 컨텍스트: app thread(시작 실패 롤백) 또는 process 종료 완료 경로.
 *
 * 호출 체인:
 *   raid_bdev_process_alloc(실패) / channels_abort_start_process_done → [raid_bdev_process_free]
 */
static void
raid_bdev_process_free(struct raid_bdev_process *process)
{
	struct raid_bdev_process_request *process_req;
	/* [한국어] 큐에서 꺼내 해제할 요청 임시 포인터. */

	while ((process_req = TAILQ_FIRST(&process->requests)) != NULL) {
		/* [한국어] requests 큐의 맨 앞 요청을 꺼내며, 비워질 때까지 반복. */
		TAILQ_REMOVE(&process->requests, process_req, link);
		/* [한국어] 큐에서 분리(이중 해제 방지). */
		raid_bdev_process_request_free(process_req);
		/* [한국어] 요청의 DMA 버퍼와 구조체 해제. */
	}

	free(process);
	/* [한국어] 모든 요청 회수 후 process 본체 해제. */
}

/*
 * [한국어]
 * raid_bdev_process_alloc - process 객체를 만들고 윈도우 크기/QoS/요청 풀을 초기화.
 *
 * @raid_bdev: process 가 동작할 RAID bdev.
 * @type: process 종류(현재 RAID_PROCESS_REBUILD).
 * @target: 재구성 대상 base bdev(rebuild 의 목적지 슬롯).
 * @return: 완성된 process 포인터(성공) / NULL(메모리 부족).
 *
 * 동기: rebuild 를 시작하기 전에 (1) 한 사이클 윈도우 크기, (2) 대역폭 QoS 토큰 버킷,
 * (3) 미리 할당된 요청 풀을 갖춘 process 컨텍스트를 만들어야 한다. 윈도우 크기는 옵션값과
 * write_unit_size 중 큰 쪽으로 정렬하여 디바이스 쓰기 단위 위반을 막는다.
 *
 * 동작 단계:
 *   1) process calloc + raid_bdev/type/target 설정.
 *   2) max_window_size = max(올림(window_size_kb*1024 / 데이터블록크기), write_unit_size).
 *   3) requests / finish_actions 큐 초기화.
 *   4) g_opts.process_max_bandwidth_mb_sec!=0 이면 QoS 토큰 버킷 파라미터 계산
 *      (bytes_per_tsc, bytes_max=1ms 분량 등).
 *   5) RAID_BDEV_PROCESS_MAX_QD 개 요청을 할당해 requests 풀에 채움(하나라도 실패하면 전체 롤백).
 *
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인:
 *   raid_bdev_start_rebuild → [raid_bdev_process_alloc] → raid_bdev_process_alloc_request ×QD
 */
static struct raid_bdev_process *
raid_bdev_process_alloc(struct raid_bdev *raid_bdev, enum raid_process_type type,
			struct raid_base_bdev_info *target)
{
	struct raid_bdev_process *process;
	/* [한국어] 새로 만들 process 객체. */
	struct raid_bdev_process_request *process_req;
	/* [한국어] 풀 채우기용 임시 요청 포인터. */
	int i;
	/* [한국어] 요청 풀 채우기 루프 인덱스. */

	process = calloc(1, sizeof(*process));
	/* [한국어] process 구조체 0초기화 할당(모든 포인터/카운터를 NULL/0 으로). */
	if (process == NULL) {
		/* [한국어] 할당 실패 → NULL. */
		return NULL;
	}

	process->raid_bdev = raid_bdev;
	/* [한국어] 소속 RAID 설정(라이프타임 내내 불변). */
	process->type = type;
	/* [한국어] process 종류 저장(현재 rebuild). */
	process->target = target;
	/* [한국어] 재구성 목적지 base bdev 슬롯. */
	process->max_window_size = spdk_max(spdk_divide_round_up(g_opts.process_window_size_kb * 1024UL,
					    spdk_bdev_get_data_block_size(&raid_bdev->bdev)),
					    raid_bdev->bdev.write_unit_size);
	/* [한국어] 한 사이클 최대 윈도우(블록 수). 옵션 KB 를 데이터블록 크기로 올림 나눈 값과
	 * 디바이스 write_unit_size 중 큰 쪽 — 쓰기 단위 정렬 위반을 방지. */
	TAILQ_INIT(&process->requests);
	/* [한국어] idle 요청 풀 큐 초기화. */
	TAILQ_INIT(&process->finish_actions);
	/* [한국어] 종료 시 실행할 콜백 액션 큐 초기화. */

	if (g_opts.process_max_bandwidth_mb_sec != 0) {
		/* [한국어] 대역폭 상한이 설정되어 있으면 토큰 버킷 QoS 활성화. */
		process->qos.enable_qos = true;
		/* [한국어] QoS on — 윈도우 처리 전 토큰 검사를 수행하게 됨. */
		process->qos.last_tsc = spdk_get_ticks();
		/* [한국어] 토큰 충전 기준 시각을 현재 TSC 로 초기화. */
		process->qos.bytes_per_tsc = g_opts.process_max_bandwidth_mb_sec * 1024 * 1024.0 /
					     spdk_get_ticks_hz();
		/* [한국어] 1 TSC tick 당 충전 바이트 = (MB/s × 1MB) / (틱/초). 부동소수 누적. */
		process->qos.bytes_max = g_opts.process_max_bandwidth_mb_sec * 1024 * 1024.0 / SPDK_SEC_TO_MSEC;
		/* [한국어] 토큰 상한 = 1ms 분량 바이트. 무한 누적(버스트 폭주) 방지. */
		process->qos.bytes_available = 0.0;
		/* [한국어] 시작 시 가용 토큰 0 — 첫 충전 전까지 즉시 송신 방지. */
	}

	for (i = 0; i < RAID_BDEV_PROCESS_MAX_QD; i++) {
		/* [한국어] in-flight 가능 최대 깊이만큼 요청을 미리 할당. */
		process_req = raid_bdev_process_alloc_request(process);
		/* [한국어] 윈도우 1개 분량 DMA 버퍼를 가진 요청 생성. */
		if (process_req == NULL) {
			/* [한국어] 풀 채우다 실패 → 지금까지 만든 요청 포함 process 전체 해제. */
			raid_bdev_process_free(process);
			return NULL;
		}

		TAILQ_INSERT_TAIL(&process->requests, process_req, link);
		/* [한국어] 완성된 요청을 idle 풀 큐 끝에 등록. */
	}

	return process;
	/* [한국어] 윈도우/QoS/요청 풀이 모두 준비된 process 반환. */
}

/*
 * [한국어]
 * raid_bdev_start_rebuild - target base bdev 에 대한 rebuild process 를 할당하고 시작.
 *
 * @target: 재구성 대상 base bdev 슬롯(새로 추가/재추가되어 데이터를 채워야 하는 멤버).
 * @return: 0(시작 디스패치 성공) / -ENOMEM(process 할당 실패).
 *
 * 동기: degraded 상태에서 새 base bdev 가 들어오면, 다른 멤버들의 데이터로부터 이 멤버의
 * 내용을 재계산해 채워 넣어야 한다(rebuild). 이 함수가 그 작업을 시작하는 얇은 래퍼다.
 *
 * 동작 단계:
 *   1) app thread 에서 호출됨을 assert(RAID 메타데이터 변경은 app thread 직렬화).
 *   2) RAID_PROCESS_REBUILD 타입 process 할당.
 *   3) raid_bdev_process_start 로 전 채널 fan-out 시작.
 *
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인:
 *   raid_bdev_configure_base_bdev_cont → [raid_bdev_start_rebuild] →
 *     raid_bdev_process_alloc / raid_bdev_process_start
 */
static int
raid_bdev_start_rebuild(struct raid_base_bdev_info *target)
{
	struct raid_bdev_process *process;
	/* [한국어] 시작할 rebuild process 객체. */

	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	/* [한국어] RAID 멤버십/process 게시는 app thread 에서만 — 직렬화로 lock 없이 안전. */

	process = raid_bdev_process_alloc(target->raid_bdev, RAID_PROCESS_REBUILD, target);
	/* [한국어] target 의 RAID 에 대해 rebuild process 와 요청 풀/QoS 를 준비. */
	if (process == NULL) {
		/* [한국어] 메모리 부족 → 호출자에 전파. */
		return -ENOMEM;
	}

	raid_bdev_process_start(process);
	/* [한국어] 전 채널에 process 설정을 뿌리고, 끝나면 전용 thread 가 데이터 이전을 시작. */

	return 0;
	/* [한국어] 시작 디스패치 성공(실제 완료는 비동기). */
}

/* [한국어] forward decl — check_sb_cb / examine 경로에서 base bdev 구성 이어가기를 상호 참조. */
static void raid_bdev_configure_base_bdev_cont(struct raid_base_bdev_info *base_info);

/*
 * [한국어]
 * _raid_bdev_configure_base_bdev_cont - 채널 동기화(barrier) 완료 후 base bdev 구성을 재개하는 콜백.
 *
 * @i: spdk_for_each_channel 반복자(ctx=base_info).
 * @status: 동기화 결과(raid_bdev_ch_sync 는 항상 0 전파).
 *
 * 동기: is_process_target 플래그를 모든 채널이 관측한 뒤에 is_configured 를 세워야 한다(가시성 순서 보장).
 * spdk_for_each_channel 을 빈 작업(ch_sync)으로 한 바퀴 돌리면 메모리 가시성 배리어 역할을 하며,
 * 그 완료 시점에 이 함수가 실제 구성 로직(cont)을 이어간다.
 *
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인:
 *   raid_bdev_configure_base_bdev_cont → spdk_for_each_channel(ch_sync) →
 *     [_raid_bdev_configure_base_bdev_cont] → raid_bdev_configure_base_bdev_cont(재진입)
 */
static void
_raid_bdev_configure_base_bdev_cont(struct spdk_io_channel_iter *i, int status)
{
	struct raid_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] 동기화 barrier 의 대상 base bdev 정보 복원. */

	raid_bdev_configure_base_bdev_cont(base_info);
	/* [한국어] 모든 채널이 is_process_target 을 본 뒤이므로 구성 진행을 재개(이번엔 process target 분기로). */
}

/*
 * [한국어]
 * raid_bdev_ch_sync - 아무 작업 없이 다음 채널로 넘기는 동기화용 per-channel 콜백(메모리 배리어 목적).
 *
 * @i: spdk_for_each_channel 반복자.
 *
 * 동기: spdk_for_each_channel 은 모든 채널을 순차 방문하므로, 채널에서 할 일이 없어도 한 바퀴
 * 돌면 "이전에 설정한 플래그가 모든 채널 thread 에 전파됨"을 보장하는 배리어로 쓸 수 있다.
 * 여기서는 is_process_target 설정의 가시성을 확보하기 위해 사용된다.
 *
 * 실행 컨텍스트: 각 채널 소유 코어 thread.
 *
 * 호출 체인:
 *   raid_bdev_configure_base_bdev_cont → spdk_for_each_channel(ch_sync) → [raid_bdev_ch_sync]
 */
static void
raid_bdev_ch_sync(struct spdk_io_channel_iter *i)
{
	spdk_for_each_channel_continue(i, 0);
	/* [한국어] 별도 작업 없이 0(성공) 전달하며 다음 채널로 — 순회 자체가 가시성 배리어. */
}

/*
 * [한국어]
 * raid_bdev_configure_base_bdev_cont - base bdev 1개의 자원 확보 이후, RAID 구성/rebuild 시작을 결정하는 핵심 분기.
 *
 * @base_info: 방금 desc/채널/UUID 검증을 마친 base bdev 슬롯.
 *
 * 동기: base bdev 가 RAID 에 붙을 때, 그 의미는 상황에 따라 세 가지다 — (a) 초기 구성 중
 * 마지막 멤버라 RAID 자체를 online 화, (b) 이미 online 인 RAID 에 추가된 새 멤버라 rebuild 대상,
 * (c) 아직 멤버가 더 필요해 단순히 발견 카운트만 증가. 이 함수가 그 셋을 갈라 처리한다.
 *
 * 동작 단계:
 *   1) "이미 운영 멤버 수만큼 발견됐는데 이 멤버는 process target 이 아직 아님" → 즉 online RAID 에
 *      들어온 추가 멤버. is_process_target=true 로 표시하고, 그 플래그가 모든 채널에 보이도록
 *      ch_sync 배리어를 돌린 뒤(cont 재진입) 다시 이 함수로 돌아온다(early return).
 *   2) is_configured=true 설정, num_base_bdevs_discovered 증가 + 불변식 assert.
 *   3) configure_cb 를 지역 변수로 빼서 base_info 에서 떼어냄(중복 호출 방지).
 *   4) 분기:
 *      - 발견 수 == 운영 수 → raid_bdev_configure 로 RAID online 화(여기서 cb 처리됨).
 *      - is_process_target → operational++ 후 raid_bdev_start_rebuild.
 *      - 그 외 → 추가 멤버 대기, rc=0.
 *   5) configure 경로가 cb 를 소비하지 않았다면 여기서 configure_cb 호출.
 *
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인:
 *   raid_bdev_configure_base_bdev / check_sb_cb / _raid_bdev_configure_base_bdev_cont →
 *     [raid_bdev_configure_base_bdev_cont] → raid_bdev_configure / raid_bdev_start_rebuild
 */
static void
raid_bdev_configure_base_bdev_cont(struct raid_base_bdev_info *base_info)
{
	struct raid_bdev *raid_bdev = base_info->raid_bdev;
	/* [한국어] 이 멤버가 속한 RAID. */
	raid_base_bdev_cb configure_cb;
	/* [한국어] 구성 완료를 알릴 콜백(중복 호출 방지를 위해 지역으로 빼서 처리). */
	int rc;
	/* [한국어] 구성/rebuild 결과 코드. */

	if (raid_bdev->num_base_bdevs_discovered == raid_bdev->num_base_bdevs_operational &&
	    base_info->is_process_target == false) {
		/* TODO: defer if rebuild in progress on another base bdev */
		/* [한국어] 이미 운영 멤버 정족수를 채운 online RAID 에 들어온 추가 멤버 → rebuild 대상.
		 * (원본 TODO: 다른 base bdev 에서 rebuild 진행 중이면 지연해야 함.) */
		assert(raid_bdev->process == NULL);
		/* [한국어] 동시에 두 process 는 미지원 — 현재 process 없음을 보장. */
		assert(raid_bdev->state == RAID_BDEV_STATE_ONLINE);
		/* [한국어] 이 경로는 RAID 가 이미 online 일 때만 도달. */
		base_info->is_process_target = true;
		/* [한국어] 이 멤버를 process(rebuild) 목적지로 표시. */
		/* To assure is_process_target is set before is_configured when checked in raid_bdev_create_cb() */
		spdk_for_each_channel(raid_bdev, raid_bdev_ch_sync, base_info, _raid_bdev_configure_base_bdev_cont);
		/* [한국어] is_process_target 이 모든 채널에 가시화된 뒤 is_configured 가 설정되도록 배리어.
		 * 완료 콜백에서 이 함수로 재진입(이번엔 첫 if 를 건너뜀). */
		return;
		/* [한국어] 배리어 비동기 완료 대기 — 이후는 _cont 콜백이 이어감. */
	}

	base_info->is_configured = true;
	/* [한국어] 이 멤버가 RAID 에 정상 구성됨을 표시(데이터 경로에서 사용 가능). */

	raid_bdev->num_base_bdevs_discovered++;
	/* [한국어] 발견된(자원 확보된) 멤버 수 증가. */
	assert(raid_bdev->num_base_bdevs_discovered <= raid_bdev->num_base_bdevs);
	/* [한국어] 발견 수는 전체 슬롯 수를 넘을 수 없음. */
	assert(raid_bdev->num_base_bdevs_operational <= raid_bdev->num_base_bdevs);
	/* [한국어] 운영 멤버 수도 전체 슬롯 수 이하. */
	assert(raid_bdev->num_base_bdevs_operational >= raid_bdev->min_base_bdevs_operational);
	/* [한국어] 운영 멤버 수는 RAID 레벨이 요구하는 최소 정족수 이상이어야 함. */

	configure_cb = base_info->configure_cb;
	/* [한국어] 콜백을 지역으로 복사. */
	base_info->configure_cb = NULL;
	/* [한국어] slot 에서 떼어내 이후 경로의 중복 호출 방지. */
	/*
	 * Configure the raid bdev when the number of discovered base bdevs reaches the number
	 * of base bdevs we know to be operational members of the array. Usually this is equal
	 * to the total number of base bdevs (num_base_bdevs) but can be less - when the array is
	 * degraded.
	 */
	if (raid_bdev->num_base_bdevs_discovered == raid_bdev->num_base_bdevs_operational) {
		/* [한국어] 운영 정족수만큼 발견 완료 → RAID 를 online 화할 시점. degraded 면 정족수<전체. */
		rc = raid_bdev_configure(raid_bdev, configure_cb, base_info->configure_cb_ctx);
		/* [한국어] RAID bdev 를 실제 등록/online 화. 성공 시 내부에서 configure_cb 를 호출함. */
		if (rc != 0) {
			/* [한국어] online 화 실패 → 에러 로그(cb 는 아래에서 rc 와 함께 호출). */
			SPDK_ERRLOG("Failed to configure raid bdev: %s\n", spdk_strerror(-rc));
		} else {
			/* [한국어] 성공 시 configure 가 이미 cb 를 소비했으므로 중복 호출 방지. */
			configure_cb = NULL;
		}
	} else if (base_info->is_process_target) {
		/* [한국어] online RAID 에 추가된 멤버 → rebuild 시작 경로. */
		raid_bdev->num_base_bdevs_operational++;
		/* [한국어] 이 멤버를 운영 멤버로 승격(rebuild 후 정상 멤버가 됨). */
		rc = raid_bdev_start_rebuild(base_info);
		/* [한국어] 데이터 재구성 process 시작. */
		if (rc != 0) {
			/* [한국어] rebuild 시작 실패 → 방금 추가한 멤버를 다시 제거. */
			SPDK_ERRLOG("Failed to start rebuild: %s\n", spdk_strerror(-rc));
			_raid_bdev_remove_base_bdev(base_info, NULL, NULL);
		}
	} else {
		/* [한국어] 아직 멤버가 더 필요한 초기 구성 단계 → 추가 발견을 기다림. */
		rc = 0;
	}

	if (configure_cb != NULL) {
		/* [한국어] configure 경로가 cb 를 소비하지 않은 경우(실패 또는 rebuild/대기 경로) 여기서 호출. */
		configure_cb(base_info->configure_cb_ctx, rc);
	}
}

/* [한국어] forward decl — 다른 RAID 의 superblock 발견 시 그쪽 examine 경로로 위임하기 위해 필요. */
static void raid_bdev_examine_sb(const struct raid_bdev_superblock *sb, struct spdk_bdev *bdev,
				 raid_base_bdev_cb cb_fn, void *cb_ctx);

/*
 * [한국어]
 * raid_bdev_configure_base_bdev_check_sb_cb - 신규 base bdev 에서 읽은 superblock 검사 결과 처리 콜백.
 *
 * @sb: 읽어들인 RAID superblock(유효할 때만 의미 있음).
 * @status: superblock 로드 결과. 0=유효 SB 발견, -EINVAL=SB 없음, 그 외=I/O 에러.
 * @ctx: 검사 대상 base_info.
 *
 * 동기: 기존 bdev 가 아닌 "새" bdev 를 RAID 에 추가할 때, 그 디스크에 이미 다른(또는 같은) RAID 의
 * superblock 이 있을 수 있다. 같은 RAID 의 SB 라면 그 SB 기준으로 examine 경로를 다시 타야 하고,
 * 다른 RAID 의 SB 라면 충돌(-EEXIST)이며, SB 가 없으면 그냥 새 멤버로 구성을 이어간다.
 *
 * 동작 단계(status 분기):
 *   - 0(유효 SB):
 *       * SB.uuid 가 현재 RAID 의 uuid 와 같으면 → 자원 해제 후 raid_bdev_examine_sb 로 위임(재추가 흐름).
 *       * 다르면 → 다른 RAID 디스크 → -EEXIST, 자원 해제 후 cb 로 실패 통지.
 *   - -EINVAL(SB 없음): 일반 신규 멤버로서 raid_bdev_configure_base_bdev_cont 진행.
 *   - 그 외(에러): 로그 후 cb 로 status 통지.
 *
 * 실행 컨텍스트: app thread(superblock 로드 완료 콜백).
 *
 * 호출 체인:
 *   raid_bdev_load_base_bdev_superblock → [raid_bdev_configure_base_bdev_check_sb_cb] →
 *     raid_bdev_examine_sb / raid_bdev_configure_base_bdev_cont / configure_cb
 */
static void
raid_bdev_configure_base_bdev_check_sb_cb(const struct raid_bdev_superblock *sb, int status,
		void *ctx)
{
	struct raid_base_bdev_info *base_info = ctx;
	/* [한국어] 검사 대상 base bdev 슬롯. */
	raid_base_bdev_cb configure_cb = base_info->configure_cb;
	/* [한국어] 구성 완료/실패를 알릴 콜백(지역 복사). */

	switch (status) {
	case 0:
		/* valid superblock found */
		/* [한국어] 이 디스크에 유효한 RAID superblock 존재. */
		base_info->configure_cb = NULL;
		/* [한국어] 위임/실패 어느 쪽이든 여기서 slot 의 cb 를 떼어 중복 호출 방지. */
		if (spdk_uuid_compare(&base_info->raid_bdev->bdev.uuid, &sb->uuid) == 0) {
			/* [한국어] SB 의 RAID uuid 가 현재 RAID 와 동일 → 같은 array 의 멤버(재추가) 흐름. */
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);
			/* [한국어] desc 로부터 실제 bdev 핸들 획득(examine_sb 에 넘길 대상). */

			raid_bdev_free_base_bdev_resource(base_info);
			/* [한국어] 여기서 잡았던 desc/채널/claim 을 일단 풀고 examine_sb 가 정식 경로로 재구성. */
			raid_bdev_examine_sb(sb, bdev, configure_cb, base_info->configure_cb_ctx);
			/* [한국어] SB 기준 examine 경로로 위임(슬롯 매핑/상태 검증 포함). */
			return;
		}
		SPDK_ERRLOG("Superblock of a different raid bdev found on bdev %s\n", base_info->name);
		/* [한국어] 다른 RAID 의 SB → 이 디스크는 다른 array 소속, 추가하면 데이터 파괴 위험. */
		status = -EEXIST;
		/* [한국어] 이미 존재(다른 array) 충돌로 표시. */
		raid_bdev_free_base_bdev_resource(base_info);
		/* [한국어] 잡았던 자원 해제. */
		break;
	case -EINVAL:
		/* no valid superblock */
		/* [한국어] SB 없음 → 깨끗한 신규 멤버. 일반 구성 진행. */
		raid_bdev_configure_base_bdev_cont(base_info);
		return;
	default:
		/* [한국어] SB 로드 중 I/O 등 에러. */
		SPDK_ERRLOG("Failed to examine bdev %s: %s\n",
			    base_info->name, spdk_strerror(-status));
		break;
	}

	if (configure_cb != NULL) {
		/* [한국어] 충돌/에러 경로 → 콜백으로 status 통지. */
		base_info->configure_cb = NULL;
		/* [한국어] (case 0 충돌 경로 등에서 이미 NULL 일 수 있으나) 재차 보장. */
		configure_cb(base_info->configure_cb_ctx, status);
	}
}

/*
 * [한국어]
 * raid_bdev_configure_base_bdev - base bdev 하나를 열고(claim) RAID 멤버로 준비하는 핵심 함수.
 *
 * @base_info: 구성할 멤버 슬롯(이름 또는 uuid 가 채워져 있어야 함).
 * @existing: true=이미 SB 등 통해 멤버로 확인된 기존 디스크(SB 검사 생략), false=신규 추가(SB 검사 필요).
 * @cb_fn: 구성 완료/실패 통지 콜백.
 * @cb_ctx: 콜백 컨텍스트.
 * @return: 0(동기 성공 또는 비동기 진행 시작) / 음수 errno(실패). -ENODEV 는 아직 bdev 미존재(지연 허용).
 *
 * 동기: RAID 가 데이터를 읽고 쓰려면 각 base bdev 에 대해 descriptor 를 열고, 모듈로
 * claim 하여 다른 곳에서 못 쓰게 잠그고, IO 채널을 얻고, 블록 크기/메타데이터 포맷이 RAID 의
 * 다른 멤버들과 일치하는지 검증해야 한다. superblock 모드면 data_offset/data_size 도 계산한다.
 *
 * 동작 단계:
 *   1) uuid 가 있으면 그 uuid alias 로 bdev 를 찾아 name 을 채우거나 일치 검증.
 *   2) spdk_bdev_open_ext 로 desc 생성(이벤트 콜백 등록). -ENODEV 면 아직 없음(지연 가능).
 *   3) bdev uuid 를 base_info->uuid 에 채우거나 일치 검증.
 *   4) spdk_bdev_module_claim_bdev 로 RAID 모듈이 독점 소유 주장.
 *   5) app thread IO 채널 획득(메타데이터 I/O 용).
 *   6) desc/blockcnt 저장. superblock 모드면 data_offset(정렬 포함)/data_size 계산·검증.
 *   7) DIF 지원/블록 크기/메타데이터 포맷이 RAID 첫 멤버 기준과 일치하는지 설정 또는 검증.
 *   8) existing 이면 곧장 cont, 아니면 SB 로드 후 check_sb_cb 로 분기.
 *   에러: 어느 단계 실패든 잡은 자원을 풀고 음수 반환(out 라벨에서 일괄 정리).
 *
 * 실행 컨텍스트: app thread(claim/멤버십 변경 직렬화).
 *
 * 호출 체인:
 *   raid_bdev_add_base_bdev / raid_bdev_examine_* → [raid_bdev_configure_base_bdev] →
 *     spdk_bdev_open_ext / claim_bdev / raid_bdev_configure_base_bdev_cont 또는 load_superblock
 */
static int
raid_bdev_configure_base_bdev(struct raid_base_bdev_info *base_info, bool existing,
			      raid_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct raid_bdev *raid_bdev = base_info->raid_bdev;
	/* [한국어] 이 멤버가 속할 RAID. blocklen/md 포맷 기준 비교에 사용. */
	struct spdk_bdev_desc *desc;
	/* [한국어] base bdev 에 대한 열린 descriptor(open_ext 결과). */
	struct spdk_bdev *bdev;
	/* [한국어] desc 가 가리키는 실제 bdev 핸들. */
	const struct spdk_uuid *bdev_uuid;
	/* [한국어] bdev 의 uuid — base_info->uuid 채우기/검증용. */
	int rc;
	/* [한국어] 각 단계 결과 코드. */

	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	/* [한국어] open/claim 등 멤버십 변경은 app thread 직렬화 — 경쟁 방지(lock 불필요 근거). */
	assert(base_info->desc == NULL);
	/* [한국어] 아직 구성 전이어야 함(중복 구성 금지). */

	/*
	 * Base bdev can be added by name or uuid. Here we assure both properties are set and valid
	 * before claiming the bdev.
	 */

	if (!spdk_uuid_is_null(&base_info->uuid)) {
		/* [한국어] uuid 로 추가된 경우 — uuid 로 bdev 를 찾고 name 을 보강/검증. */
		char uuid_str[SPDK_UUID_STRING_LEN];
		/* [한국어] uuid 문자열 표현(alias 조회 키). */
		const char *bdev_name;
		/* [한국어] 조회된 bdev 의 실제 이름. */

		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &base_info->uuid);
		/* [한국어] 바이너리 uuid → 소문자 문자열 변환. */

		/* UUID of a bdev is registered as its alias */
		bdev = spdk_bdev_get_by_name(uuid_str);
		/* [한국어] bdev 의 uuid 는 alias 로 등록되므로 uuid 문자열로 조회 가능. */
		if (bdev == NULL) {
			/* [한국어] 해당 uuid 의 bdev 가 아직 시스템에 없음 → 지연(나중에 examine 시 재시도). */
			return -ENODEV;
		}

		bdev_name = spdk_bdev_get_name(bdev);
		/* [한국어] 실제 이름 획득. */

		if (base_info->name == NULL) {
			/* [한국어] uuid 만으로 추가된 경우 name 을 채움. */
			assert(existing == true);
			/* [한국어] name 없이 들어오는 건 기존 멤버 재구성 경로뿐. */
			base_info->name = strdup(bdev_name);
			/* [한국어] 이름 복제 저장(소유권 base_info). */
			if (base_info->name == NULL) {
				/* [한국어] 복제 실패 → 메모리 부족. */
				return -ENOMEM;
			}
		} else if (strcmp(base_info->name, bdev_name) != 0) {
			/* [한국어] name 과 uuid 가 가리키는 bdev 가 불일치 → 구성 오류. */
			SPDK_ERRLOG("Name mismatch for base bdev '%s' - expected '%s'\n",
				    bdev_name, base_info->name);
			return -EINVAL;
		}
	}

	assert(base_info->name != NULL);
	/* [한국어] 이 시점엔 이름이 반드시 확정되어 있어야 함. */

	rc = spdk_bdev_open_ext(base_info->name, true, raid_bdev_event_base_bdev, NULL, &desc);
	/* [한국어] base bdev 를 쓰기 가능(true)으로 열고 이벤트 콜백 등록. desc 반환. */
	if (rc != 0) {
		/* [한국어] open 실패. */
		if (rc != -ENODEV) {
			/* [한국어] -ENODEV(아직 없음)는 정상적인 지연 사유이므로 로그 생략, 그 외만 에러 로그. */
			SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", base_info->name);
		}
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);
	/* [한국어] 열린 desc 로부터 bdev 핸들 획득. */
	bdev_uuid = spdk_bdev_get_uuid(bdev);
	/* [한국어] bdev 의 uuid 포인터 획득. */

	if (spdk_uuid_is_null(&base_info->uuid)) {
		/* [한국어] name 으로만 추가된 경우 → bdev uuid 를 base_info 에 채움. */
		spdk_uuid_copy(&base_info->uuid, bdev_uuid);
	} else if (spdk_uuid_compare(&base_info->uuid, bdev_uuid) != 0) {
		/* [한국어] 미리 지정된 uuid 와 실제 bdev uuid 불일치 → 엉뚱한 디스크. */
		SPDK_ERRLOG("UUID mismatch for base bdev '%s'\n", base_info->name);
		spdk_bdev_close(desc);
		/* [한국어] 열어 둔 desc 닫고 실패. */
		return -EINVAL;
	}

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &g_raid_if);
	/* [한국어] RAID 모듈(g_raid_if)이 이 bdev 를 독점 소유(claim) — 다른 모듈/RAID 가 못 쓰게 잠금. */
	if (rc != 0) {
		/* [한국어] 이미 다른 곳에서 claim 됨 → 사용 불가. */
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return rc;
	}

	SPDK_DEBUGLOG(bdev_raid, "bdev %s is claimed\n", bdev->name);
	/* [한국어] claim 성공 디버그 로그. */

	base_info->app_thread_ch = spdk_bdev_get_io_channel(desc);
	/* [한국어] app thread 용 IO 채널 확보 — superblock 읽기/쓰기 등 관리 I/O 에 사용. */
	if (base_info->app_thread_ch == NULL) {
		/* [한국어] 채널 획득 실패 → claim/desc 롤백. */
		SPDK_ERRLOG("Failed to get io channel\n");
		spdk_bdev_module_release_bdev(bdev);
		spdk_bdev_close(desc);
		return -ENOMEM;
	}

	base_info->desc = desc;
	/* [한국어] desc 게시 — 이후 데이터 경로가 이 멤버를 사용 가능. */
	base_info->blockcnt = bdev->blockcnt;
	/* [한국어] base bdev 의 전체 블록 수 기록(data_offset/size 검증 기준). */

	if (raid_bdev->superblock_enabled) {
		/* [한국어] superblock 모드 — 디스크 앞부분에 RAID 메타데이터를 두므로 데이터 시작 오프셋 계산 필요. */
		uint64_t data_offset;
		/* [한국어] 사용자 데이터가 시작되는 LBA. */

		if (base_info->data_offset == 0) {
			/* [한국어] 오프셋 미지정 → 최소 데이터 오프셋(SB 영역 크기)을 블록 단위로 환산. */
			assert((RAID_BDEV_MIN_DATA_OFFSET_SIZE % spdk_bdev_get_data_block_size(bdev)) == 0);
			/* [한국어] 최소 오프셋은 블록 크기로 나누어떨어져야 정렬이 깨지지 않음. */
			data_offset = RAID_BDEV_MIN_DATA_OFFSET_SIZE / spdk_bdev_get_data_block_size(bdev);
			/* [한국어] 바이트 단위 SB 예약 크기를 블록 수로 변환. */
		} else {
			/* [한국어] 이미 지정된(SB 에서 읽은) 오프셋 사용. */
			data_offset = base_info->data_offset;
		}

		if (bdev->optimal_io_boundary != 0) {
			/* [한국어] 디바이스가 최적 I/O 경계를 알려주면 데이터 시작을 그 경계로 올림 정렬. */
			data_offset = spdk_round_up(data_offset, bdev->optimal_io_boundary);
			if (base_info->data_offset != 0 && base_info->data_offset != data_offset) {
				/* [한국어] SB 에 기록된 오프셋과 최적 정렬값이 다르면 경고하되 기존 SB 값을 신뢰(데이터 보존). */
				SPDK_WARNLOG("Data offset %lu on bdev '%s' is different than optimal value %lu\n",
					     base_info->data_offset, base_info->name, data_offset);
				data_offset = base_info->data_offset;
			}
		}

		base_info->data_offset = data_offset;
		/* [한국어] 최종 데이터 시작 오프셋 확정 저장. */
	}

	if (base_info->data_offset >= bdev->blockcnt) {
		/* [한국어] 데이터 시작이 디스크 용량을 넘어섬 → 구성 불가. */
		SPDK_ERRLOG("Data offset %lu exceeds base bdev capacity %lu on bdev '%s'\n",
			    base_info->data_offset, bdev->blockcnt, base_info->name);
		rc = -EINVAL;
		goto out;
	}

	if (base_info->data_size == 0) {
		/* [한국어] 데이터 크기 미지정 → 오프셋 이후 끝까지 전부 사용. */
		base_info->data_size = bdev->blockcnt - base_info->data_offset;
	} else if (base_info->data_offset + base_info->data_size > bdev->blockcnt) {
		/* [한국어] 오프셋+크기가 용량 초과 → 구성 불가. */
		SPDK_ERRLOG("Data offset and size exceeds base bdev capacity %lu on bdev '%s'\n",
			    bdev->blockcnt, base_info->name);
		rc = -EINVAL;
		goto out;
	}

	if (!raid_bdev->module->dif_supported && spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		/* [한국어] 이 RAID 레벨이 DIF/DIX(T10 데이터 무결성)를 미지원하는데 디스크가 PI 활성 → 비호환. */
		SPDK_ERRLOG("Base bdev '%s' has DIF or DIX enabled - unsupported RAID configuration\n",
			    bdev->name);
		rc = -EINVAL;
		goto out;
	}

	/*
	 * Set the raid bdev properties if this is the first base bdev configured,
	 * otherwise - verify. Assumption is that all the base bdevs for any raid bdev should
	 * have the same blocklen and metadata format.
	 */
	if (raid_bdev->bdev.blocklen == 0) {
		/* [한국어] 첫 멤버 → RAID 의 블록/메타데이터 포맷을 이 디스크 기준으로 확정. */
		raid_bdev->bdev.blocklen = bdev->blocklen;
		/* [한국어] 논리 블록 크기 채택. */
		raid_bdev->bdev.md_len = spdk_bdev_get_md_size(bdev);
		/* [한국어] 블록당 메타데이터 크기. */
		raid_bdev->bdev.md_interleave = spdk_bdev_is_md_interleaved(bdev);
		/* [한국어] 메타데이터가 데이터와 인터리브되는지 여부. */
		raid_bdev->bdev.dif_type = spdk_bdev_get_dif_type(bdev);
		/* [한국어] DIF 보호 정보 타입(Type 1/2/3 또는 disable). */
		raid_bdev->bdev.dif_check_flags = bdev->dif_check_flags;
		/* [한국어] DIF 검사 플래그(Guard/AppTag/RefTag). */
		raid_bdev->bdev.dif_is_head_of_md = spdk_bdev_is_dif_head_of_md(bdev);
		/* [한국어] DIF 가 메타데이터 영역 앞쪽에 위치하는지. */
		raid_bdev->bdev.dif_pi_format = bdev->dif_pi_format;
		/* [한국어] 보호정보 포맷(16/32/64비트 Guard). */
	} else {
		/* [한국어] 두 번째 이후 멤버 → 첫 멤버가 확정한 포맷과 일치하는지 검증. */
		if (raid_bdev->bdev.blocklen != bdev->blocklen) {
			/* [한국어] 블록 크기 불일치 → 스트라이프 매핑이 불가능. */
			SPDK_ERRLOG("Raid bdev '%s' blocklen %u differs from base bdev '%s' blocklen %u\n",
				    raid_bdev->bdev.name, raid_bdev->bdev.blocklen, bdev->name, bdev->blocklen);
			rc = -EINVAL;
			goto out;
		}

		if (raid_bdev->bdev.md_len != spdk_bdev_get_md_size(bdev) ||
		    raid_bdev->bdev.md_interleave != spdk_bdev_is_md_interleaved(bdev) ||
		    raid_bdev->bdev.dif_type != spdk_bdev_get_dif_type(bdev) ||
		    raid_bdev->bdev.dif_check_flags != bdev->dif_check_flags ||
		    raid_bdev->bdev.dif_is_head_of_md != spdk_bdev_is_dif_head_of_md(bdev) ||
		    raid_bdev->bdev.dif_pi_format != bdev->dif_pi_format) {
			/* [한국어] 메타데이터/DIF 포맷 중 하나라도 첫 멤버와 다르면 혼합 불가. */
			SPDK_ERRLOG("Raid bdev '%s' has different metadata format than base bdev '%s'\n",
				    raid_bdev->bdev.name, bdev->name);
			rc = -EINVAL;
			goto out;
		}
	}

	assert(base_info->configure_cb == NULL);
	/* [한국어] 콜백이 비어 있어야(중복 구성 아님) 함. */
	base_info->configure_cb = cb_fn;
	/* [한국어] 구성 완료 콜백 게시 — cont/check_sb_cb 경로에서 사용. */
	base_info->configure_cb_ctx = cb_ctx;
	/* [한국어] 콜백 컨텍스트 게시. */

	if (existing) {
		/* [한국어] 이미 멤버임이 확인된 디스크 → SB 검사 없이 곧장 구성 이어가기. */
		raid_bdev_configure_base_bdev_cont(base_info);
	} else {
		/* check for existing superblock when using a new bdev */
		/* [한국어] 신규 디스크 → 기존 SB 가 있는지 비동기로 읽어 check_sb_cb 에서 분기. */
		rc = raid_bdev_load_base_bdev_superblock(desc, base_info->app_thread_ch,
				raid_bdev_configure_base_bdev_check_sb_cb, base_info);
		if (rc) {
			/* [한국어] SB 읽기 발행 자체가 실패 → out 에서 자원 정리. */
			SPDK_ERRLOG("Failed to read bdev %s superblock: %s\n",
				    bdev->name, spdk_strerror(-rc));
		}
	}
out:
	if (rc != 0) {
		/* [한국어] 어떤 단계든 실패 시 게시했던 콜백을 떼고 잡은 자원(desc/채널/claim) 일괄 해제. */
		base_info->configure_cb = NULL;
		raid_bdev_free_base_bdev_resource(base_info);
	}
	return rc;
	/* [한국어] 0=성공/진행, 음수=실패. -ENODEV 는 호출자가 지연으로 처리할 수 있음. */
}

/*
 * [한국어]
 * raid_bdev_add_base_bdev - RAID 에 base bdev 하나를 추가하는 공개 API(빈 슬롯 검색 후 구성).
 *
 * @raid_bdev: 멤버를 추가할 RAID.
 * @name: 추가할 base bdev 이름.
 * @cb_fn: 구성 완료/실패 통지 콜백.
 * @cb_ctx: 콜백 컨텍스트.
 * @return: 0(성공/진행) / -EPERM(process 중) / -EINVAL(빈 슬롯 없음) / -ENOMEM / 기타 errno.
 *
 * 동기: 사용자가 RPC(bdev_raid_add_base_bdev) 등으로 RAID 에 디스크를 붙일 때의 진입점.
 * RAID 상태(CONFIGURING vs ONLINE)에 따라 적절한 슬롯을 찾아 구성을 위임한다. 우선 SB 로
 * 예약된(uuid 만 채워진 name==NULL) 슬롯에 매칭을 시도하고, 없으면 완전 빈 슬롯을 쓴다.
 *
 * 동작 단계:
 *   1) app thread 확인. 이미 process(rebuild) 진행 중이면 -EPERM(동시 멤버 변경 금지).
 *   2) CONFIGURING 상태면, name 으로 bdev 를 찾아 그 uuid 가 예약된 슬롯(name==NULL)과 일치하는지 탐색.
 *   3) 매칭 슬롯이 없거나 ONLINE 상태면, name/uuid 가 모두 비어 있는 첫 슬롯 선택.
 *   4) 슬롯이 없으면 -EINVAL. ONLINE 이면 data_size 가 이미 정해져 있어야 함(assert).
 *   5) 슬롯에 name 복제 저장 후 raid_bdev_configure_base_bdev(existing=false) 호출.
 *   6) 실패하되 "CONFIGURING 중 -ENODEV(아직 디스크 없음)"가 아니면 → 슬롯 name 롤백.
 *      (그 예외 케이스는 examine 시 재시도되므로 슬롯 예약을 유지한다.)
 *
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인:
 *   RPC bdev_raid_add_base_bdev → [raid_bdev_add_base_bdev] → raid_bdev_configure_base_bdev
 */
int
raid_bdev_add_base_bdev(struct raid_bdev *raid_bdev, const char *name,
			raid_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct raid_base_bdev_info *base_info = NULL, *iter;
	/* [한국어] 선택된 빈/예약 슬롯(base_info)과 순회용 iter. */
	int rc;
	/* [한국어] 구성 결과 코드. */

	assert(name != NULL);
	/* [한국어] 이름은 필수. */
	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	/* [한국어] 멤버 추가는 app thread 직렬화. */

	if (raid_bdev->process != NULL) {
		/* [한국어] rebuild 등 process 진행 중에는 멤버 변경 불가(상태 일관성 보호). */
		SPDK_ERRLOG("raid bdev '%s' is in process\n",
			    raid_bdev->bdev.name);
		return -EPERM;
	}

	if (raid_bdev->state == RAID_BDEV_STATE_CONFIGURING) {
		/* [한국어] 초기 구성 중 — SB 로 예약된(uuid 만 있는) 슬롯에 우선 매칭 시도. */
		struct spdk_bdev *bdev = spdk_bdev_get_by_name(name);
		/* [한국어] 이름으로 실제 bdev 조회(없을 수도 있음 → NULL). */

		if (bdev != NULL) {
			/* [한국어] bdev 가 존재하면 그 uuid 로 예약 슬롯 탐색. */
			RAID_FOR_EACH_BASE_BDEV(raid_bdev, iter) {
				if (iter->name == NULL &&
				    spdk_uuid_compare(&bdev->uuid, &iter->uuid) == 0) {
					/* [한국어] name 미설정 + uuid 일치 = 이 디스크용으로 예약된 슬롯. */
					base_info = iter;
					break;
				}
			}
		}
	}

	if (base_info == NULL || raid_bdev->state == RAID_BDEV_STATE_ONLINE) {
		/* [한국어] 예약 매칭 실패했거나 ONLINE(rebuild 추가) → 완전 빈 슬롯을 찾는다. */
		RAID_FOR_EACH_BASE_BDEV(raid_bdev, iter) {
			if (iter->name == NULL && spdk_uuid_is_null(&iter->uuid)) {
				/* [한국어] name/uuid 모두 비어 있는 첫 슬롯 선택. */
				base_info = iter;
				break;
			}
		}
	}

	if (base_info == NULL) {
		/* [한국어] 쓸 수 있는 슬롯이 전혀 없음 → 추가 불가. */
		SPDK_ERRLOG("no empty slot found in raid bdev '%s' for new base bdev '%s'\n",
			    raid_bdev->bdev.name, name);
		return -EINVAL;
	}

	assert(base_info->is_configured == false);
	/* [한국어] 선택된 슬롯은 아직 구성 전이어야 함. */

	if (raid_bdev->state == RAID_BDEV_STATE_ONLINE) {
		/* [한국어] ONLINE 에 추가하는 경우(rebuild) data_size 는 SB 로 이미 확정되어 있어야 함. */
		assert(base_info->data_size != 0);
		assert(base_info->desc == NULL);
	}

	base_info->name = strdup(name);
	/* [한국어] 슬롯에 이름 복제 저장(소유권 base_info). */
	if (base_info->name == NULL) {
		/* [한국어] 복제 실패 → 메모리 부족. */
		return -ENOMEM;
	}

	rc = raid_bdev_configure_base_bdev(base_info, false, cb_fn, cb_ctx);
	/* [한국어] 신규(existing=false)로 구성 — SB 검사 경로를 탄다. */
	if (rc != 0 && (rc != -ENODEV || raid_bdev->state != RAID_BDEV_STATE_CONFIGURING)) {
		/* [한국어] 실패했고, "CONFIGURING 중 디스크 아직 없음(-ENODEV)" 의 정상 지연 케이스가 아니면 롤백.
		 * 그 예외에선 슬롯 name 을 남겨 두어 examine 시 자동 재시도되게 한다. */
		SPDK_ERRLOG("base bdev '%s' configure failed: %s\n", name, spdk_strerror(-rc));
		free(base_info->name);
		base_info->name = NULL;
	}

	return rc;
	/* [한국어] 구성 결과 반환(비동기 진행이면 0). */
}

/*
 * [한국어]
 * raid_bdev_create_from_sb - 디스크에서 읽은 superblock 으로부터 RAID bdev 골격을 재생성.
 *
 * @sb: 어느 멤버 디스크에서 발견한 유효 RAID superblock(이름/레벨/스트라이프/멤버 메타 포함).
 * @raid_bdev_out: 생성된 raid_bdev 포인터 출력.
 * @return: 0(성공) / 음수 errno(생성/SB할당 실패).
 *
 * 동기: 시스템 재시작/디스크 재발견 시, 사용자 명령 없이도 SB 만으로 RAID 구조(멤버 수,
 * 레벨, 스트라이프 크기, uuid, 각 멤버의 data_offset/size)를 복원해야 한다. 이 함수가
 * 그 메타데이터를 raid_bdev 와 슬롯 배열에 채워 넣는다(아직 멤버 디스크는 안 붙임).
 *
 * 동작 단계:
 *   1) _raid_bdev_create — SB 의 이름/스트라이프(KB)/멤버 수/레벨/uuid 로 빈 RAID 골격 생성.
 *      (strip_size*block_size/1024 = KB 단위 스트라이프.)
 *   2) raid_bdev_alloc_superblock — SB 사본을 담을 메모리 할당(실패 시 RAID 해제).
 *   3) SB 원본을 raid_bdev->sb 로 memcpy(length 만큼).
 *   4) SB 의 각 base bdev 엔트리를 순회하며: CONFIGURED 멤버면 uuid 복사 + operational++,
 *      모든 멤버의 data_offset/data_size 를 슬롯에 기록.
 *   5) 출력 포인터 설정.
 *
 * 실행 컨텍스트: app thread(examine 경로).
 *
 * 호출 체인:
 *   raid_bdev_examine_sb → [raid_bdev_create_from_sb] → _raid_bdev_create / raid_bdev_alloc_superblock
 */
static int
raid_bdev_create_from_sb(const struct raid_bdev_superblock *sb, struct raid_bdev **raid_bdev_out)
{
	struct raid_bdev *raid_bdev;
	/* [한국어] 생성할 RAID 골격. */
	uint8_t i;
	/* [한국어] SB 멤버 엔트리 순회 인덱스. */
	int rc;
	/* [한국어] 생성 결과 코드. */

	rc = _raid_bdev_create(sb->name, (sb->strip_size * sb->block_size) / 1024, sb->num_base_bdevs,
			       sb->level, true, &sb->uuid, &raid_bdev);
	/* [한국어] SB 메타로 빈 RAID 생성. 스트라이프 = (블록당 strip × 블록바이트)/1024 = KB. superblock=true. */
	if (rc != 0) {
		/* [한국어] 골격 생성 실패 → 전파. */
		return rc;
	}

	rc = raid_bdev_alloc_superblock(raid_bdev, sb->block_size);
	/* [한국어] SB 사본을 담을 정렬된 버퍼 할당(block_size 정렬 I/O 용). */
	if (rc != 0) {
		/* [한국어] SB 버퍼 할당 실패 → 방금 만든 RAID 해제 후 전파. */
		raid_bdev_free(raid_bdev);
		return rc;
	}

	assert(sb->length <= RAID_BDEV_SB_MAX_LENGTH);
	/* [한국어] SB 길이가 최대 한도를 넘지 않아야 함(버퍼 오버런 방지). */
	memcpy(raid_bdev->sb, sb, sb->length);
	/* [한국어] 발견한 SB 를 RAID 의 SB 버퍼로 복사(이후 seq_number 갱신/쓰기의 기준). */

	for (i = 0; i < sb->base_bdevs_size; i++) {
		/* [한국어] SB 에 기록된 모든 멤버 슬롯 메타를 RAID 슬롯 배열에 반영. */
		const struct raid_bdev_sb_base_bdev *sb_base_bdev = &sb->base_bdevs[i];
		/* [한국어] i번째 SB 멤버 엔트리. */
		struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[sb_base_bdev->slot];
		/* [한국어] 그 멤버가 들어갈 실제 슬롯(slot 인덱스로 매핑). */

		if (sb_base_bdev->state == RAID_SB_BASE_BDEV_CONFIGURED) {
			/* [한국어] 정상 구성된 멤버 → uuid 를 슬롯에 채우고 운영 멤버 수 증가. */
			spdk_uuid_copy(&base_info->uuid, &sb_base_bdev->uuid);
			raid_bdev->num_base_bdevs_operational++;
		}

		base_info->data_offset = sb_base_bdev->data_offset;
		/* [한국어] 멤버의 데이터 시작 오프셋 복원(상태 무관). */
		base_info->data_size = sb_base_bdev->data_size;
		/* [한국어] 멤버의 데이터 크기 복원. */
	}

	*raid_bdev_out = raid_bdev;
	/* [한국어] 복원된 RAID 골격 반환. */
	return 0;
}

/*
 * [한국어]
 * raid_bdev_examine_no_sb - superblock 이 없는(레거시/비-SB) RAID 구성에 새 bdev 를 매칭.
 *
 * @bdev: 방금 등장한 base bdev 후보.
 *
 * 동기: SB 를 안 쓰는 RAID(JSON config 로 멤버를 미리 선언한 경우)에서는, 멤버 이름/uuid 가
 * 미리 슬롯에 예약되어 있다. 새 bdev 가 나타나면 그 예약 슬롯과 매칭하여 구성을 잇는다.
 *
 * 동작 단계:
 *   1) 전역 RAID 리스트를 순회하며, CONFIGURING 상태이고 SB 없는(sb==NULL) RAID 만 대상.
 *   2) 각 RAID 의 멤버 슬롯 중 아직 desc 가 없고(미구성) 이름 또는 uuid 가 이 bdev 와 일치하는 슬롯 탐색.
 *   3) 매칭되면 raid_bdev_configure_base_bdev(existing=true, cb=NULL) 로 구성(SB 검사 생략).
 *
 * 실행 컨텍스트: app thread(examine 경로).
 *
 * 호출 체인:
 *   raid_bdev_examine_cont / raid_bdev_examine(DIF 경로) → [raid_bdev_examine_no_sb] →
 *     raid_bdev_configure_base_bdev
 */
static void
raid_bdev_examine_no_sb(struct spdk_bdev *bdev)
{
	struct raid_bdev *raid_bdev;
	/* [한국어] 순회 중인 RAID. */
	struct raid_base_bdev_info *base_info;
	/* [한국어] 매칭 후보 멤버 슬롯. */

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		/* [한국어] 전역 RAID 목록 전체 순회. */
		if (raid_bdev->state != RAID_BDEV_STATE_CONFIGURING || raid_bdev->sb != NULL) {
			/* [한국어] 아직 구성 중이 아니거나 SB 기반 RAID 면 이 경로 대상 아님 → 건너뜀. */
			continue;
		}
		RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
			/* [한국어] 이 RAID 의 멤버 슬롯들 검사. */
			if (base_info->desc == NULL &&
			    ((base_info->name != NULL && strcmp(bdev->name, base_info->name) == 0) ||
			     spdk_uuid_compare(&base_info->uuid, &bdev->uuid) == 0)) {
				/* [한국어] 미구성 슬롯이면서 이름 또는 uuid 가 이 bdev 와 일치 → 이 슬롯의 멤버. */
				raid_bdev_configure_base_bdev(base_info, true, NULL, NULL);
				/* [한국어] existing=true 로 구성(SB 없음 전제, fire-and-forget). */
				break;
			}
		}
	}
}

/*
 * [한국어]
 * struct raid_bdev_examine_others_ctx - SB 로 RAID 를 새로 만든 뒤 나머지 멤버 디스크를 연쇄 examine 하기 위한 컨텍스트.
 */
struct raid_bdev_examine_others_ctx {
	struct spdk_uuid raid_bdev_uuid;
	/* [한국어] 대상 RAID 의 uuid.
	 * 설정자: raid_bdev_examine_sb 가 새 RAID 생성 직후 SB.uuid 로 채움.
	 * 읽는 자: raid_bdev_examine_others 가 매 연쇄 단계에서 RAID 를 재조회하는 키로 사용.
	 * 값 범위: 유효한 RAID uuid(NULL 아님).
	 * 동기화: app thread 단일 스레드 흐름 — 별도 락 불필요. */

	uint8_t current_base_bdev_idx;
	/* [한국어] 다음에 examine 할 멤버 슬롯 인덱스(연쇄 진행 커서).
	 * 설정자: raid_bdev_examine_others 가 다음 후보 슬롯으로 전진시킬 때 갱신.
	 * 읽는 자: 같은 함수가 루프 시작 위치로 사용.
	 * 값 범위: 0 ~ num_base_bdevs-1.
	 * 동기화: app thread 직렬 — 락 불필요. */

	raid_base_bdev_cb cb_fn;
	/* [한국어] 모든 멤버 examine 연쇄가 끝났을 때 호출할 최종 콜백.
	 * 설정자: examine_sb 가 원래 호출자의 cb_fn 을 보존.
	 * 읽는 자: raid_bdev_examine_others_done.
	 * 값 범위: 유효 함수 포인터 또는 NULL.
	 * 동기화: app thread 직렬. */

	void *cb_ctx;
	/* [한국어] cb_fn 에 전달할 컨텍스트.
	 * 설정자: examine_sb. 읽는 자: examine_others_done.
	 * 값 범위: 임의 포인터(콜백 의미에 따름).
	 * 동기화: app thread 직렬. */
};

/*
 * [한국어]
 * raid_bdev_examine_others_done - 나머지 멤버 연쇄 examine 종료 시 최종 콜백 호출 및 컨텍스트 해제.
 *
 * @_ctx: raid_bdev_examine_others_ctx.
 * @status: 연쇄 종료 상태(마지막 단계 결과).
 *
 * 동기: SB 기반 RAID 재구성에서 멤버들을 하나씩 연쇄적으로 붙이는 과정이 끝나면, 원래
 * examine 를 시작한 쪽에 결과를 알리고 임시 컨텍스트를 정리해야 한다.
 *
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인:
 *   raid_bdev_examine_others / examine_others_load_cb → [raid_bdev_examine_others_done] → ctx->cb_fn
 */
static void
raid_bdev_examine_others_done(void *_ctx, int status)
{
	struct raid_bdev_examine_others_ctx *ctx = _ctx;
	/* [한국어] 연쇄 컨텍스트 복원. */

	if (ctx->cb_fn != NULL) {
		/* [한국어] 원래 호출자가 지정한 최종 콜백이 있으면 결과 통지. */
		ctx->cb_fn(ctx->cb_ctx, status);
	}
	free(ctx);
	/* [한국어] 연쇄용 임시 컨텍스트 해제. */
}

/* [한국어] SB 로드 완료 콜백 타입 — bdev/sb/status/ctx 를 받음(examine 경로 공통). */
typedef void (*raid_bdev_examine_load_sb_cb)(struct spdk_bdev *bdev,
		const struct raid_bdev_superblock *sb, int status, void *ctx);
/* [한국어] forward decl — 이름으로 bdev 를 열어 SB 를 읽는 헬퍼(연쇄 examine 에서 사용). */
static int raid_bdev_examine_load_sb(const char *bdev_name, raid_bdev_examine_load_sb_cb cb,
				     void *cb_ctx);
/* [한국어] forward decl — SB 기준 멤버 매칭/구성의 핵심(상호 재귀). */
static void raid_bdev_examine_sb(const struct raid_bdev_superblock *sb, struct spdk_bdev *bdev,
				 raid_base_bdev_cb cb_fn, void *cb_ctx);
/* [한국어] forward decl — 멤버 하나 구성 후 다음 멤버로 넘어가는 연쇄 콜백. */
static void raid_bdev_examine_others(void *_ctx, int status);

/*
 * [한국어]
 * raid_bdev_examine_others_load_cb - 다음 멤버 디스크의 SB 로드 완료 후 examine_sb 로 위임하는 콜백.
 *
 * @bdev: SB 를 읽은 멤버 bdev.
 * @sb: 읽어들인 superblock.
 * @status: SB 로드 결과(0=성공).
 * @_ctx: examine_others 연쇄 컨텍스트.
 *
 * 동기: SB 기반 RAID 의 멤버를 연쇄로 붙일 때, 각 후보 디스크의 SB 를 먼저 읽고 그 SB 로
 * examine_sb 를 다시 호출하여 슬롯 매핑/구성을 수행한다. 이 콜백이 로드와 examine 을 잇는다.
 *
 * 실행 컨텍스트: app thread(SB 로드 완료 콜백).
 *
 * 호출 체인:
 *   raid_bdev_examine_load_sb → [raid_bdev_examine_others_load_cb] →
 *     raid_bdev_examine_sb(..., raid_bdev_examine_others, ctx)
 */
static void
raid_bdev_examine_others_load_cb(struct spdk_bdev *bdev, const struct raid_bdev_superblock *sb,
				 int status, void *_ctx)
{
	struct raid_bdev_examine_others_ctx *ctx = _ctx;
	/* [한국어] 연쇄 컨텍스트 복원. */

	if (status != 0) {
		/* [한국어] 이 멤버 SB 로드 실패 → 연쇄 종료(최종 콜백에 status 전달). */
		raid_bdev_examine_others_done(ctx, status);
		return;
	}

	raid_bdev_examine_sb(sb, bdev, raid_bdev_examine_others, ctx);
	/* [한국어] 로드된 SB 로 examine_sb 재진입 — 구성 후 다음 멤버를 위해 examine_others 를 콜백으로 지정. */
}

/*
 * [한국어]
 * raid_bdev_examine_others - SB 로 만든 RAID 의 나머지 멤버들을 슬롯 순서대로 연쇄 examine.
 *
 * @_ctx: examine_others 연쇄 컨텍스트(대상 RAID uuid + 진행 커서 + 최종 콜백).
 * @status: 직전 멤버 구성 결과(0 또는 -EEXIST 면 계속, 그 외면 종료).
 *
 * 동기: 하나의 멤버 디스크 SB 로 RAID 를 복원하면, SB 에 적힌 다른 멤버들도 시스템에 이미
 * 존재할 수 있다. 이 함수는 그런 멤버 후보를 슬롯 순서로 찾아 한 번에 하나씩(비동기 연쇄)
 * 붙여 나가, 가능한 한 많은 멤버를 자동으로 재조립한다.
 *
 * 동작 단계:
 *   1) status 가 0/−EEXIST 가 아니면(치명 에러) out 으로 종료. (-EEXIST 는 이미 붙음 → 무시하고 계속.)
 *   2) uuid 로 RAID 재조회(중간에 사라졌을 수 있음) — 없으면 -ENODEV 종료.
 *   3) current_base_bdev_idx 부터 슬롯 순회: 이미 구성됐거나 uuid 가 비어 있으면 skip.
 *   4) uuid alias 로 bdev 가 시스템에 존재하는지 확인(없으면 skip).
 *   5) 커서를 이 슬롯으로 갱신하고 raid_bdev_examine_load_sb 로 SB 로드 발행 → 성공 시 return(연쇄 진행),
 *      발행 실패면 다음 슬롯 시도.
 *   6) 더 붙일 멤버가 없으면 out 으로 최종 콜백.
 *
 * 실행 컨텍스트: app thread(examine 연쇄 콜백).
 *
 * 호출 체인:
 *   examine_sb / examine_others_load_cb → [raid_bdev_examine_others] →
 *     raid_bdev_examine_load_sb(다음 멤버) 또는 raid_bdev_examine_others_done
 */
static void
raid_bdev_examine_others(void *_ctx, int status)
{
	struct raid_bdev_examine_others_ctx *ctx = _ctx;
	/* [한국어] 연쇄 컨텍스트. */
	struct raid_bdev *raid_bdev;
	/* [한국어] 대상 RAID(uuid 로 재조회). */
	struct raid_base_bdev_info *base_info;
	/* [한국어] 슬롯 순회 포인터. */
	char uuid_str[SPDK_UUID_STRING_LEN];
	/* [한국어] 멤버 uuid 문자열(alias 조회 키). */

	if (status != 0 && status != -EEXIST) {
		/* [한국어] -EEXIST(이미 붙음)는 정상으로 보고 계속, 그 외 에러는 연쇄 중단. */
		goto out;
	}

	raid_bdev = raid_bdev_find_by_uuid(&ctx->raid_bdev_uuid);
	/* [한국어] 비동기 연쇄 중 RAID 가 삭제됐을 수 있으므로 매번 uuid 로 재조회. */
	if (raid_bdev == NULL) {
		/* [한국어] RAID 가 사라짐 → 더 진행 불가. */
		status = -ENODEV;
		goto out;
	}

	for (base_info = &raid_bdev->base_bdev_info[ctx->current_base_bdev_idx];
	     base_info < &raid_bdev->base_bdev_info[raid_bdev->num_base_bdevs];
	     base_info++) {
		/* [한국어] 진행 커서 슬롯부터 마지막 멤버 슬롯까지 순회. */
		if (base_info->is_configured || spdk_uuid_is_null(&base_info->uuid)) {
			/* [한국어] 이미 구성됐거나 uuid 미지정(SB 에 없는 슬롯) → 다음으로. */
			continue;
		}

		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &base_info->uuid);
		/* [한국어] 멤버 uuid 를 문자열로 변환. */

		if (spdk_bdev_get_by_name(uuid_str) == NULL) {
			/* [한국어] 해당 멤버 디스크가 아직 시스템에 없음 → 지금은 붙일 수 없음, skip. */
			continue;
		}

		ctx->current_base_bdev_idx = raid_bdev_base_bdev_slot(base_info);
		/* [한국어] 다음 연쇄가 이 슬롯부터 이어지도록 커서 갱신. */

		status = raid_bdev_examine_load_sb(uuid_str, raid_bdev_examine_others_load_cb, ctx);
		/* [한국어] 이 멤버의 SB 로드를 비동기 발행 → 완료 시 load_cb 가 examine_sb 로 위임. */
		if (status != 0) {
			/* [한국어] 발행 자체 실패면 이 슬롯은 건너뛰고 다음 후보 시도. */
			continue;
		}
		return;
		/* [한국어] 발행 성공 → 비동기 완료까지 대기(연쇄가 콜백으로 이어짐). */
	}
out:
	raid_bdev_examine_others_done(ctx, status);
	/* [한국어] 더 붙일 멤버가 없거나 에러 → 최종 콜백 호출 + 컨텍스트 해제. */
}

/*
 * [한국어]
 * raid_bdev_examine_sb - 발견한 superblock 으로 RAID 를 식별/생성하고 이 bdev 를 해당 멤버 슬롯으로 구성.
 *
 * @sb: 이 bdev 에서 읽은 RAID superblock.
 * @bdev: 검사 대상 base bdev.
 * @cb_fn: 구성 완료/실패 통지 콜백.
 * @cb_ctx: 콜백 컨텍스트.
 *
 * 동기: 부팅/핫플러그 시 디스크에 적힌 SB 만으로 RAID 멤버십을 자동 재조립하는 핵심 로직.
 * SB 검증(블록 크기/uuid), seq_number 비교를 통한 stale/newer SB 처리, RAID 신규 생성 또는
 * 기존 RAID 매칭, 그리고 이 bdev 가 SB 의 어느 슬롯에 해당하는지 찾아 구성을 위임한다.
 *
 * 동작 단계:
 *   1) SB block_size 와 실제 bdev 데이터 블록 크기 불일치 → -EINVAL.
 *   2) SB uuid 가 NULL → -EINVAL.
 *   3) uuid 로 기존 RAID 조회:
 *      - SB.seq > 기존: 더 새 SB. 기존이 CONFIGURING 이 아니면 -EBUSY(이미 online 이라 교체 불가),
 *        맞으면 기존 RAID 삭제 후 새 SB 로 재생성하도록 raid_bdev=NULL.
 *      - SB.seq < 기존: stale SB. 기존 RAID 의 SB 를 기준으로 사용(sb 교체).
 *   4) SB 의 멤버 목록에서 이 bdev 의 uuid 와 일치하는 엔트리(sb_base_bdev/슬롯) 탐색. 없으면 -EINVAL.
 *   5) 기존 RAID 가 없으면 examine_others 연쇄 컨텍스트를 만들고 raid_bdev_create_from_sb 로 생성.
 *      이후 cb_fn 을 examine_others 로 바꿔, 이 멤버 구성 후 나머지 멤버도 자동 탐색.
 *   6) RAID 가 ONLINE(이미 운영 중)이면 → 이 bdev 는 MISSING/FAILED 였던 멤버의 재추가(re-add).
 *      슬롯에 uuid 채우고 existing=true 로 구성(이 경로는 rebuild 로 이어짐).
 *   7) ONLINE 이 아니면(구성 중): SB 상 이 bdev 가 CONFIGURED 멤버가 아니면 무시(-EINVAL).
 *      맞으면 uuid 로 슬롯을 찾아 existing=true 구성. 이미 구성됐으면 -EEXIST.
 *   에러/직접 실패: out 에서 cb_fn 호출(연쇄 가능).
 *
 * 실행 컨텍스트: app thread(examine 경로, 상호 재귀 가능).
 *
 * 호출 체인:
 *   raid_bdev_examine_cont / check_sb_cb / examine_others_load_cb → [raid_bdev_examine_sb] →
 *     raid_bdev_create_from_sb / raid_bdev_configure_base_bdev / raid_bdev_delete
 */
static void
raid_bdev_examine_sb(const struct raid_bdev_superblock *sb, struct spdk_bdev *bdev,
		     raid_base_bdev_cb cb_fn, void *cb_ctx)
{
	const struct raid_bdev_sb_base_bdev *sb_base_bdev = NULL;
	/* [한국어] SB 멤버 목록에서 이 bdev 에 해당하는 엔트리(슬롯/상태 보유). */
	struct raid_bdev *raid_bdev;
	/* [한국어] 매칭/생성된 RAID. */
	struct raid_base_bdev_info *iter, *base_info;
	/* [한국어] iter=슬롯 순회용, base_info=최종 선택된 멤버 슬롯. */
	uint8_t i;
	/* [한국어] SB 멤버 목록 순회 인덱스. */
	int rc;
	/* [한국어] 결과 코드. */

	if (sb->block_size != spdk_bdev_get_data_block_size(bdev)) {
		/* [한국어] SB 가 기록한 블록 크기와 실제 디스크 블록 크기가 다름 → 동일 디스크 아님/포맷 깨짐. */
		SPDK_WARNLOG("Bdev %s block size (%u) does not match the value in superblock (%u)\n",
			     bdev->name, sb->block_size, spdk_bdev_get_data_block_size(bdev));
		rc = -EINVAL;
		goto out;
	}

	if (spdk_uuid_is_null(&sb->uuid)) {
		/* [한국어] RAID uuid 가 비어 있는 SB → 식별 불가. */
		SPDK_WARNLOG("NULL raid bdev UUID in superblock on bdev %s\n", bdev->name);
		rc = -EINVAL;
		goto out;
	}

	raid_bdev = raid_bdev_find_by_uuid(&sb->uuid);
	/* [한국어] SB 의 uuid 로 이미 알려진 RAID 가 있는지 조회. */

	if (raid_bdev) {
		/* [한국어] 같은 uuid 의 RAID 가 이미 존재 → SB 버전(seq_number) 비교로 처리 방식 결정. */
		if (raid_bdev->sb == NULL) {
			/* [한국어] 기존 RAID 가 SB 모드가 아닌데 SB 디스크가 들어옴 → 불일치. */
			SPDK_WARNLOG("raid superblock is null\n");
			rc = -EINVAL;
			goto out;
		}

		if (sb->seq_number > raid_bdev->sb->seq_number) {
			/* [한국어] 이 디스크의 SB 가 더 최신 → 메타데이터가 그사이 갱신된 상태. */
			SPDK_DEBUGLOG(bdev_raid,
				      "raid superblock seq_number on bdev %s (%lu) greater than existing raid bdev %s (%lu)\n",
				      bdev->name, sb->seq_number, raid_bdev->bdev.name, raid_bdev->sb->seq_number);

			if (raid_bdev->state != RAID_BDEV_STATE_CONFIGURING) {
				/* [한국어] 이미 online 인 RAID 는 실행 중 SB 교체가 위험 → 거부(-EBUSY). */
				SPDK_WARNLOG("Newer version of raid bdev %s superblock found on bdev %s but raid bdev is not in configuring state.\n",
					     raid_bdev->bdev.name, bdev->name);
				rc = -EBUSY;
				goto out;
			}

			/* remove and then recreate the raid bdev using the newer superblock */
			/* [한국어] 구성 중이라면 안전하게 기존 RAID 를 삭제하고 더 새 SB 로 다시 만든다. */
			raid_bdev_delete(raid_bdev, NULL, NULL);
			raid_bdev = NULL;
			/* [한국어] 아래에서 create_from_sb 경로를 타도록 NULL 로 리셋. */
		} else if (sb->seq_number < raid_bdev->sb->seq_number) {
			/* [한국어] 이 디스크 SB 가 더 오래됨(stale) → 기존 RAID 의 SB 를 진실로 채택. */
			SPDK_DEBUGLOG(bdev_raid,
				      "raid superblock seq_number on bdev %s (%lu) smaller than existing raid bdev %s (%lu)\n",
				      bdev->name, sb->seq_number, raid_bdev->bdev.name, raid_bdev->sb->seq_number);
			/* use the current raid bdev superblock */
			sb = raid_bdev->sb;
			/* [한국어] 이후 슬롯 매칭은 최신 SB 기준으로 수행. */
		}
	}

	for (i = 0; i < sb->base_bdevs_size; i++) {
		/* [한국어] SB 멤버 목록에서 이 bdev 의 uuid 와 일치하는 엔트리 탐색. */
		sb_base_bdev = &sb->base_bdevs[i];

		assert(spdk_uuid_is_null(&sb_base_bdev->uuid) == false);
		/* [한국어] SB 멤버 엔트리는 항상 유효 uuid 를 가져야 함. */

		if (spdk_uuid_compare(&sb_base_bdev->uuid, spdk_bdev_get_uuid(bdev)) == 0) {
			/* [한국어] 이 bdev 가 SB 의 i번째 멤버임을 확인 → 슬롯/상태 확보. */
			break;
		}
	}

	if (i == sb->base_bdevs_size) {
		/* [한국어] SB 에 이 bdev 의 uuid 가 없음 → 이 RAID 의 멤버가 아님. */
		SPDK_DEBUGLOG(bdev_raid, "raid superblock does not contain this bdev's uuid\n");
		rc = -EINVAL;
		goto out;
	}

	if (!raid_bdev) {
		/* [한국어] 기존 RAID 가 없음(또는 위에서 삭제) → SB 로 새로 생성. */
		struct raid_bdev_examine_others_ctx *ctx;
		/* [한국어] 생성 후 나머지 멤버를 연쇄 examine 하기 위한 컨텍스트. */

		ctx = calloc(1, sizeof(*ctx));
		/* [한국어] 연쇄 컨텍스트 할당. */
		if (ctx == NULL) {
			rc = -ENOMEM;
			goto out;
		}

		rc = raid_bdev_create_from_sb(sb, &raid_bdev);
		/* [한국어] SB 메타데이터로 RAID 골격 생성. */
		if (rc != 0) {
			/* [한국어] 생성 실패 → ctx 해제 후 종료. */
			SPDK_ERRLOG("Failed to create raid bdev %s: %s\n",
				    sb->name, spdk_strerror(-rc));
			free(ctx);
			goto out;
		}

		/* after this base bdev is configured, examine other base bdevs that may be present */
		/* [한국어] 이 멤버를 붙인 뒤 나머지 멤버도 자동 탐색하도록 연쇄 콜백을 끼워 넣는다. */
		spdk_uuid_copy(&ctx->raid_bdev_uuid, &sb->uuid);
		/* [한국어] 연쇄에서 RAID 재조회용 uuid 보존. */
		ctx->cb_fn = cb_fn;
		/* [한국어] 원래 호출자의 최종 콜백 보존. */
		ctx->cb_ctx = cb_ctx;
		/* [한국어] 원래 콜백 컨텍스트 보존. */

		cb_fn = raid_bdev_examine_others;
		/* [한국어] 이번 멤버 구성 완료 시 examine_others 가 다음 멤버를 이어가도록 콜백 교체. */
		cb_ctx = ctx;
		/* [한국어] 그 연쇄 콜백의 컨텍스트로 ctx 전달. */
	}

	if (raid_bdev->state == RAID_BDEV_STATE_ONLINE) {
		/* [한국어] 이미 운영 중인 RAID 에 SB 디스크가 들어옴 → MISSING/FAILED 였던 멤버의 재추가. */
		assert(sb_base_bdev->slot < raid_bdev->num_base_bdevs);
		/* [한국어] SB 가 가리키는 슬롯이 유효 범위 내. */
		base_info = &raid_bdev->base_bdev_info[sb_base_bdev->slot];
		/* [한국어] 재추가할 멤버 슬롯. */
		assert(base_info->is_configured == false);
		/* [한국어] 아직 구성 안 된 슬롯이어야 함(빈 자리). */
		assert(sb_base_bdev->state == RAID_SB_BASE_BDEV_MISSING ||
		       sb_base_bdev->state == RAID_SB_BASE_BDEV_FAILED);
		/* [한국어] online RAID 에 다시 들어오는 멤버는 이전에 빠졌던(MISSING/FAILED) 상태여야 함. */
		assert(spdk_uuid_is_null(&base_info->uuid));
		/* [한국어] 슬롯의 uuid 는 비어 있어야(아직 미할당). */
		spdk_uuid_copy(&base_info->uuid, &sb_base_bdev->uuid);
		/* [한국어] 슬롯에 멤버 uuid 채움. */
		SPDK_NOTICELOG("Re-adding bdev %s to raid bdev %s.\n", bdev->name, raid_bdev->bdev.name);
		/* [한국어] 재추가 안내 로그. */
		rc = raid_bdev_configure_base_bdev(base_info, true, cb_fn, cb_ctx);
		/* [한국어] existing=true 로 구성 → cont 에서 rebuild 가 시작됨(degraded 복구). */
		if (rc != 0) {
			SPDK_ERRLOG("Failed to configure bdev %s as base bdev of raid %s: %s\n",
				    bdev->name, raid_bdev->bdev.name, spdk_strerror(-rc));
		}
		goto out;
	}

	if (sb_base_bdev->state != RAID_SB_BASE_BDEV_CONFIGURED) {
		/* [한국어] 구성 중 RAID 인데 이 멤버가 SB 상 정상 멤버가 아님 → 무시. */
		SPDK_NOTICELOG("Bdev %s is not an active member of raid bdev %s. Ignoring.\n",
			       bdev->name, raid_bdev->bdev.name);
		rc = -EINVAL;
		goto out;
	}

	base_info = NULL;
	/* [한국어] 구성 중 RAID: uuid 로 해당 멤버 슬롯을 직접 찾는다. */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, iter) {
		if (spdk_uuid_compare(&iter->uuid, spdk_bdev_get_uuid(bdev)) == 0) {
			/* [한국어] uuid 일치 슬롯 발견. */
			base_info = iter;
			break;
		}
	}

	if (base_info == NULL) {
		/* [한국어] uuid 가 일치하는 슬롯이 없음 → 멤버 아님. */
		SPDK_ERRLOG("Bdev %s is not a member of raid bdev %s\n",
			    bdev->name, raid_bdev->bdev.name);
		rc = -EINVAL;
		goto out;
	}

	if (base_info->is_configured) {
		/* [한국어] 이미 구성된 멤버 → 중복(연쇄에선 -EEXIST 가 정상으로 처리됨). */
		rc = -EEXIST;
		goto out;
	}

	rc = raid_bdev_configure_base_bdev(base_info, true, cb_fn, cb_ctx);
	/* [한국어] 구성 중 RAID 의 멤버로 existing=true 구성(정족수 채워지면 online 화). */
	if (rc != 0) {
		SPDK_ERRLOG("Failed to configure bdev %s as base bdev of raid %s: %s\n",
			    bdev->name, raid_bdev->bdev.name, spdk_strerror(-rc));
	}
out:
	if (rc != 0 && cb_fn != 0) {
		/* [한국어] 동기 실패(configure 가 비동기로 cb 를 부르지 못한 경우)면 여기서 cb_fn 으로 통지.
		 * cb_fn 이 examine_others 면 다음 멤버 연쇄가, 일반 콜백이면 최종 결과 통지가 일어남. */
		cb_fn(cb_ctx, rc);
	}
}

/*
 * [한국어]
 * struct raid_bdev_examine_ctx - SB 를 읽기 위해 임시로 연 desc/채널과 호출자 콜백을 묶는 컨텍스트.
 */
struct raid_bdev_examine_ctx {
	struct spdk_bdev_desc *desc;
	/* [한국어] SB 를 읽으려고 임시로 연 base bdev descriptor.
	 * 설정자: raid_bdev_examine_load_sb 의 spdk_bdev_open_ext.
	 * 읽는 자: load_sb_done(bdev 추출), ctx_free(close).
	 * 값 범위: 유효 desc 또는 NULL(미오픈/정리 후).
	 * 동기화: app thread 단일 흐름. */

	struct spdk_io_channel *ch;
	/* [한국어] SB 읽기 I/O 용 임시 IO 채널.
	 * 설정자: raid_bdev_examine_load_sb. 읽는 자: load_superblock 호출/ctx_free(put).
	 * 값 범위: 유효 채널 또는 NULL.
	 * 동기화: app thread 단일 흐름. */

	raid_bdev_examine_load_sb_cb cb;
	/* [한국어] SB 로드 완료 시 호출할 호출자 콜백(bdev/sb/status/ctx).
	 * 설정자: raid_bdev_examine_load_sb. 읽는 자: load_sb_done.
	 * 값 범위: 유효 함수 포인터(assert 로 NULL 금지).
	 * 동기화: app thread 단일 흐름. */

	void *cb_ctx;
	/* [한국어] cb 에 전달할 컨텍스트.
	 * 설정자: raid_bdev_examine_load_sb. 읽는 자: load_sb_done.
	 * 값 범위: 임의 포인터.
	 * 동기화: app thread 단일 흐름. */
};

/*
 * [한국어]
 * raid_bdev_examine_ctx_free - examine 임시 컨텍스트의 채널/desc 를 닫고 메모리 해제.
 *
 * @ctx: 해제할 컨텍스트(NULL 허용).
 *
 * 동기: SB 읽기를 위해 잠깐 연 desc/채널은 읽기가 끝나면(성공/실패 무관) 반드시 닫아야
 * 자원 누수와 불필요한 claim 잔재를 막는다.
 *
 * 동작 단계: NULL 가드 → 채널 put → desc close → 구조체 free(역순 정리).
 *
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인:
 *   raid_bdev_examine_load_sb(err) / load_sb_done → [raid_bdev_examine_ctx_free]
 */
static void
raid_bdev_examine_ctx_free(struct raid_bdev_examine_ctx *ctx)
{
	if (!ctx) {
		/* [한국어] NULL 안전 — 호출자 단순화. */
		return;
	}

	if (ctx->ch) {
		/* [한국어] 임시 IO 채널 반환(채널 ref 카운트 감소). */
		spdk_put_io_channel(ctx->ch);
	}

	if (ctx->desc) {
		/* [한국어] 임시 desc 닫기(claim/open 해제). */
		spdk_bdev_close(ctx->desc);
	}

	free(ctx);
	/* [한국어] 컨텍스트 구조체 해제. */
}

/*
 * [한국어]
 * raid_bdev_examine_load_sb_done - SB 로드 완료 시 호출자 콜백을 호출하고 임시 컨텍스트를 정리.
 *
 * @sb: 읽어들인 superblock(status==0 일 때 유효).
 * @status: 로드 결과(0=유효 SB, -EINVAL=SB 없음, 기타=I/O 에러).
 * @_ctx: raid_bdev_examine_ctx.
 *
 * 동기: 저수준 SB 로드(raid_bdev_load_base_bdev_superblock)의 완료 콜백. 여기서 desc 로부터
 * bdev 를 복원해 상위 콜백(examine_cont/others_load_cb 등)에 넘기고, 임시 자원을 닫는다.
 *
 * 실행 컨텍스트: app thread(SB I/O 완료 콜백).
 *
 * 호출 체인:
 *   raid_bdev_load_base_bdev_superblock → [raid_bdev_examine_load_sb_done] →
 *     ctx->cb(...) → raid_bdev_examine_ctx_free
 */
static void
raid_bdev_examine_load_sb_done(const struct raid_bdev_superblock *sb, int status, void *_ctx)
{
	struct raid_bdev_examine_ctx *ctx = _ctx;
	/* [한국어] 임시 컨텍스트 복원. */
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ctx->desc);
	/* [한국어] 임시 desc 로부터 실제 bdev 핸들 복원(콜백 인자). */

	ctx->cb(bdev, sb, status, ctx->cb_ctx);
	/* [한국어] 상위 콜백에 bdev/sb/status 전달 — examine 흐름을 이어감. */

	raid_bdev_examine_ctx_free(ctx);
	/* [한국어] 콜백이 끝났으니 임시 desc/채널/컨텍스트 정리. */
}

/*
 * [한국어]
 * raid_bdev_examine_event_cb - SB 읽기용 임시 open 의 비동기 이벤트 콜백(의도적으로 무시).
 *
 * @type: bdev 이벤트 종류(remove/resize 등). @bdev: 대상. @event_ctx: 미사용.
 *
 * 동기: spdk_bdev_open_ext 는 이벤트 콜백을 요구한다. 그러나 이 open 은 SB 한 번 읽고 즉시
 * 닫는 단기 핸들이므로, 어떤 이벤트도 처리할 필요가 없어 빈 함수로 둔다(콜백 시그니처 충족용).
 *
 * 실행 컨텍스트: app thread(bdev 이벤트 디스패치).
 *
 * 호출 체인:
 *   bdev 레이어 이벤트 → [raid_bdev_examine_event_cb] (no-op)
 */
static void
raid_bdev_examine_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	/* [한국어] 단기 open 이므로 이벤트 무시 — 처리 로직 없음. */
}

/*
 * [한국어]
 * raid_bdev_examine_load_sb - 이름으로 bdev 를 잠시 열어 superblock 을 비동기로 읽는 헬퍼.
 *
 * @bdev_name: SB 를 읽을 base bdev 이름(또는 uuid alias).
 * @cb: 로드 완료 시 호출할 콜백(bdev/sb/status/ctx).
 * @cb_ctx: 콜백 컨텍스트.
 * @return: 0(SB 읽기 발행 성공) / 음수 errno(open/채널/발행 실패).
 *
 * 동기: examine 경로 곳곳에서 "디스크를 잠깐 열어 SB 만 읽는" 패턴이 반복되므로, 임시 desc/채널을
 * 만들어 SB 를 읽고 완료 시 정리까지 하는 절차를 한 함수로 캡슐화했다.
 *
 * 동작 단계:
 *   1) 임시 컨텍스트 calloc.
 *   2) spdk_bdev_open_ext(write=false) — 읽기 전용으로 열기(이벤트는 무시 콜백).
 *   3) IO 채널 획득.
 *   4) cb/cb_ctx 저장 후 raid_bdev_load_base_bdev_superblock 로 SB 읽기 발행.
 *   에러: 어느 단계든 실패하면 err 에서 임시 자원 정리 후 음수 반환.
 *
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인:
 *   raid_bdev_examine / examine_others → [raid_bdev_examine_load_sb] →
 *     raid_bdev_load_base_bdev_superblock → (완료) raid_bdev_examine_load_sb_done
 */
static int
raid_bdev_examine_load_sb(const char *bdev_name, raid_bdev_examine_load_sb_cb cb, void *cb_ctx)
{
	struct raid_bdev_examine_ctx *ctx;
	/* [한국어] open/채널/콜백을 묶는 임시 컨텍스트. */
	int rc;
	/* [한국어] 각 단계 결과. */

	assert(cb != NULL);
	/* [한국어] 콜백은 필수(완료 통지 대상). */

	ctx = calloc(1, sizeof(*ctx));
	/* [한국어] 컨텍스트 0초기화 할당. */
	if (!ctx) {
		/* [한국어] 메모리 부족. */
		return -ENOMEM;
	}

	rc = spdk_bdev_open_ext(bdev_name, false, raid_bdev_examine_event_cb, NULL, &ctx->desc);
	/* [한국어] 읽기 전용(write=false)으로 bdev 열기 — SB 만 읽을 것이므로 쓰기 권한 불필요. */
	if (rc) {
		/* [한국어] open 실패 → err. */
		SPDK_ERRLOG("Failed to open bdev %s: %s\n", bdev_name, spdk_strerror(-rc));
		goto err;
	}

	ctx->ch = spdk_bdev_get_io_channel(ctx->desc);
	/* [한국어] SB 읽기 I/O 를 발행할 채널 획득. */
	if (!ctx->ch) {
		/* [한국어] 채널 실패 → err. */
		SPDK_ERRLOG("Failed to get io channel for bdev %s\n", bdev_name);
		rc = -ENOMEM;
		goto err;
	}

	ctx->cb = cb;
	/* [한국어] 완료 콜백 저장. */
	ctx->cb_ctx = cb_ctx;
	/* [한국어] 콜백 컨텍스트 저장. */

	rc = raid_bdev_load_base_bdev_superblock(ctx->desc, ctx->ch, raid_bdev_examine_load_sb_done, ctx);
	/* [한국어] 디스크 앞부분의 SB 영역을 비동기 read. 완료 시 load_sb_done 가 cb 호출+정리. */
	if (rc) {
		/* [한국어] 읽기 발행 자체 실패 → err. */
		SPDK_ERRLOG("Failed to read bdev %s superblock: %s\n",
			    bdev_name, spdk_strerror(-rc));
		goto err;
	}

	return 0;
	/* [한국어] 발행 성공(완료는 비동기). */
err:
	raid_bdev_examine_ctx_free(ctx);
	/* [한국어] 실패 경로 — 임시 desc/채널/컨텍스트 정리. */
	return rc;
}

/*
 * [한국어]
 * raid_bdev_examine_done - 한 bdev 에 대한 examine 완료를 bdev 레이어에 통지하는 종료 콜백.
 *
 * @ctx: 검사한 spdk_bdev 포인터(void* 로 전달).
 * @status: examine 결과(0=성공/무관, 음수=실패).
 *
 * 동기: bdev 레이어는 모듈의 examine 가 끝날 때까지 그 bdev 의 등록 완료를 보류한다. 따라서
 * RAID 가 examine 를 마치면 반드시 spdk_bdev_module_examine_done 으로 "다 봤다"를 알려야
 * bdev 등록 파이프라인이 진행된다(미호출 시 시스템이 멈춤).
 *
 * 실행 컨텍스트: app thread.
 *
 * 호출 체인:
 *   raid_bdev_examine / examine_cont / examine_sb(완료) → [raid_bdev_examine_done] →
 *     spdk_bdev_module_examine_done
 */
static void
raid_bdev_examine_done(void *ctx, int status)
{
	struct spdk_bdev *bdev = ctx;
	/* [한국어] 검사 대상 bdev 복원. */

	if (status != 0) {
		/* [한국어] 실패 사유 로그(검사 자체는 비치명 — 다른 모듈이 claim 할 수 있음). */
		SPDK_ERRLOG("Failed to examine bdev %s: %s\n",
			    bdev->name, spdk_strerror(-status));
	}
	spdk_bdev_module_examine_done(&g_raid_if);
	/* [한국어] 이 bdev 에 대한 RAID 모듈의 examine 종료를 bdev 레이어에 통지 — 등록 진행 재개. */
}

/*
 * [한국어]
 * raid_bdev_examine_cont - SB 로드 결과에 따라 SB 기반/비-SB 구성 경로로 분기하는 examine 연속 콜백.
 *
 * @bdev: 검사 대상 bdev.
 * @sb: 읽은 superblock(status==0 일 때 유효).
 * @status: SB 로드 결과(0=유효 SB, -EINVAL=SB 없음).
 * @ctx: 미사용(NULL).
 *
 * 동기: raid_bdev_examine_load_sb 의 완료를 받아, SB 가 있으면 SB 기반 매칭(examine_sb)을,
 * 없으면 비-SB(JSON config) 매칭(examine_no_sb)을 시도한 뒤 examine 를 마무리한다.
 *
 * 동작 단계(status 분기):
 *   - 0: 유효 SB → raid_bdev_examine_sb 로 위임(완료 콜백은 examine_done). return.
 *   - -EINVAL: SB 없음 → examine_no_sb 로 비-SB 매칭 시도, status=0 으로 정상화.
 *   - 그 외: 그대로 done 으로 전달.
 *
 * 실행 컨텍스트: app thread(SB 로드 완료 콜백).
 *
 * 호출 체인:
 *   raid_bdev_examine_load_sb_done → [raid_bdev_examine_cont] →
 *     raid_bdev_examine_sb / raid_bdev_examine_no_sb / raid_bdev_examine_done
 */
static void
raid_bdev_examine_cont(struct spdk_bdev *bdev, const struct raid_bdev_superblock *sb, int status,
		       void *ctx)
{
	switch (status) {
	case 0:
		/* valid superblock found */
		/* [한국어] 유효 SB → SB 기반 자동 재조립 경로. */
		SPDK_DEBUGLOG(bdev_raid, "raid superblock found on bdev %s\n", bdev->name);
		raid_bdev_examine_sb(sb, bdev, raid_bdev_examine_done, bdev);
		/* [한국어] SB 로 멤버/RAID 매칭. 완료 시 examine_done 으로 bdev 레이어에 통지. */
		return;
	case -EINVAL:
		/* no valid superblock, check if it can be claimed anyway */
		/* [한국어] SB 없음 → JSON config 로 미리 선언된 멤버 슬롯과 매칭 시도. */
		raid_bdev_examine_no_sb(bdev);
		status = 0;
		/* [한국어] 비-SB 경로는 동기 완료이므로 성공으로 정상화. */
		break;
	}

	raid_bdev_examine_done(bdev, status);
	/* [한국어] (SB 없음/기타) examine 종료 통지. */
}

/*
 * brief:
 * raid_bdev_examine function is the examine function call by the below layers
 * like bdev_nvme layer. This function will check if this base bdev can be
 * claimed by this raid bdev or not.
 * params:
 * bdev - pointer to base bdev
 * returns:
 * none
 */
/*
 * [한국어]
 * raid_bdev_examine - bdev 레이어가 모든 신규 bdev 에 대해 호출하는 RAID 모듈의 examine 진입점.
 *
 * @bdev: 새로 등장한(또는 등록되는) base bdev 후보.
 *
 * 동기: SPDK bdev 레이어는 어떤 bdev 가 등록될 때, 그 위에 쌓일 수 있는 가상 모듈들(RAID, LVS 등)에게
 * "이 디스크를 너의 멤버로 claim 하겠느냐"고 묻는다(examine 콜백). RAID 는 이 디스크가 자기 멤버인지
 * SB 또는 JSON config 로 판별하고, 맞으면 멤버로 구성한다. 반드시 examine_done 으로 끝내야
 * bdev 등록 파이프라인이 막히지 않는다.
 *
 * 동작 단계:
 *   1) 이미 어떤 RAID 의 멤버로 등록된 bdev 면(중복 examine) 곧장 done.
 *   2) DIF/DIX(T10 PI) 활성 디스크는 SB 를 둘 수 없으므로 SB 읽기를 건너뛰고 비-SB 경로(examine_no_sb)로.
 *   3) 그 외에는 SB 를 비동기로 읽어 examine_cont 에서 분기(가장 일반적 경로). 발행 성공이면 return(비동기 대기).
 *   에러/즉시 종료: done 라벨에서 examine_done 호출.
 *
 * 실행 컨텍스트: app thread(bdev 레이어 examine 디스패치).
 *
 * 호출 체인:
 *   bdev 레이어(예: bdev_nvme 등록) → [raid_bdev_examine] →
 *     raid_bdev_examine_load_sb / raid_bdev_examine_no_sb → ... → raid_bdev_examine_done
 */
static void
raid_bdev_examine(struct spdk_bdev *bdev)
{
	int rc = 0;
	/* [한국어] 즉시 종료 경로에서 examine_done 에 넘길 결과 코드. */

	if (raid_bdev_find_base_info_by_bdev(bdev) != NULL) {
		/* [한국어] 이미 어떤 RAID 멤버로 잡혀 있는 bdev → 다시 볼 필요 없음, 곧장 종료. */
		goto done;
	}

	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		/* [한국어] DIF/DIX 활성 디스크는 SB 를 기록할 수 없으므로 SB 읽기 생략, 비-SB 매칭만 시도. */
		raid_bdev_examine_no_sb(bdev);
		goto done;
	}

	rc = raid_bdev_examine_load_sb(bdev->name, raid_bdev_examine_cont, NULL);
	/* [한국어] 일반 경로 — SB 를 비동기로 읽어 examine_cont 에서 SB/비-SB 분기. */
	if (rc != 0) {
		/* [한국어] SB 읽기 발행 실패 → 즉시 종료(examine_done 으로 통지). */
		goto done;
	}

	return;
	/* [한국어] SB 읽기 발행 성공 → 비동기 완료가 이어받음(여기서 done 호출하지 않음). */
done:
	raid_bdev_examine_done(bdev, rc);
	/* [한국어] 동기 종료 경로 — examine 완료를 bdev 레이어에 통지. */
}

/* Log component for bdev raid bdev module */
/* [한국어] "bdev_raid" 로그 컴포넌트 등록 — SPDK_DEBUGLOG(bdev_raid, ...) 활성화 플래그를 만든다. */
SPDK_LOG_REGISTER_COMPONENT(bdev_raid)

/*
 * [한국어]
 * bdev_raid_trace - RAID I/O 추적용 trace point(tpoint)와 객체 관계를 등록.
 *
 * 동기: SPDK trace 프레임워크는 /dev/shm 공유메모리에 per-lcore lockless 환형버퍼로 이벤트를
 * 기록한다. RAID 가 자체 I/O 의 시작/완료를 추적하려면 그 tpoint 와, 상위 bdev I/O 와의
 * 부모-자식 관계를 미리 등록해야 trace 파서가 타임라인을 올바르게 재구성한다.
 *
 * 동작 단계:
 *   1) opts[] — BDEV_RAID_IO_START/DONE 두 tpoint 정의(소유자 없음, OBJECT_BDEV_RAID_IO, ctx 포인터 1개).
 *      세 번째 필드(1/0)는 새 객체 생성 여부(START 가 객체 lifetime 시작).
 *   2) spdk_trace_register_object('R') — RAID I/O 객체를 문자 'R' 로 표시 등록.
 *   3) spdk_trace_register_description_ext — 위 tpoint 들을 일괄 등록.
 *   4) tpoint_register_relation — bdev I/O START/DONE 와 RAID I/O 객체의 관계 등록(중첩 추적).
 *
 * 실행 컨텍스트: 모듈 로드 시점(constructor 매크로로 자동 호출), 단일 스레드.
 *
 * 호출 체인:
 *   SPDK_TRACE_REGISTER_FN(constructor) → [bdev_raid_trace] → spdk_trace_register_*
 */
static void
bdev_raid_trace(void)
{
	struct spdk_trace_tpoint_opts opts[] = {
		/* [한국어] 이 모듈이 등록할 trace point 목록(이름/ID/소유자/객체/객체생성여부/인자). */
		{
			"BDEV_RAID_IO_START", TRACE_BDEV_RAID_IO_START,
			OWNER_TYPE_NONE, OBJECT_BDEV_RAID_IO, 1,
			/* [한국어] RAID I/O 시작 tpoint. OWNER 없음, RAID_IO 객체, 1=이 시점에 객체 생성(lifetime 시작). */
			{{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 }}
			/* [한국어] 인자: 8바이트 포인터 "ctx"(해당 raid_bdev_io 식별자). */
		},
		{
			"BDEV_RAID_IO_DONE", TRACE_BDEV_RAID_IO_DONE,
			OWNER_TYPE_NONE, OBJECT_BDEV_RAID_IO, 0,
			/* [한국어] RAID I/O 완료 tpoint. 0=객체 생성 아님(START 가 만든 객체를 종료). */
			{{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 }}
			/* [한국어] 인자: START 와 동일한 ctx 포인터(시작-완료 매칭 키). */
		}
	};


	spdk_trace_register_object(OBJECT_BDEV_RAID_IO, 'R');
	/* [한국어] RAID I/O 객체 타입을 trace 출력에서 'R' 로 표기하도록 등록. */
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
	/* [한국어] 위 opts 배열의 tpoint 들을 trace 프레임워크에 일괄 등록(개수=SPDK_COUNTOF). */
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_START, OBJECT_BDEV_RAID_IO, 1);
	/* [한국어] 상위 bdev I/O START 와 RAID I/O 객체의 관계 등록 — 파서가 중첩(부모→자식) 추적. */
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_DONE, OBJECT_BDEV_RAID_IO, 0);
	/* [한국어] 상위 bdev I/O DONE 와의 관계 등록(완료 측). */
}
/* [한국어] bdev_raid_trace 를 trace 그룹 TRACE_GROUP_BDEV_RAID 의 등록 함수로 매다는 constructor 매크로.
 * 모듈 로드 시 자동 실행되어 위 tpoint 들을 등록한다. */
SPDK_TRACE_REGISTER_FN(bdev_raid_trace, "bdev_raid", TRACE_GROUP_BDEV_RAID)
