/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK bdev 레이어의 핵심 구현 (bdev.c)
 *
 * === 파일의 역할 ===
 * SPDK의 "bdev" 추상화 — 모든 블록 디바이스의 공통 진입점 — 의 본체이다.
 * NVMe SSD, AIO 파일, malloc-bdev, RAID, logical volume, partition 등
 * 어떤 백엔드든 동일한 API(spdk_bdev_read/write/unmap/flush/zone/reset/...)로
 * 사용할 수 있도록 한다. 본 파일은 다음을 책임진다:
 *   1) **bdev 라이프사이클**: spdk_bdev_register / spdk_bdev_unregister, 모듈
 *      examine 체인, claim/release (v1/v2), descriptor open/close.
 *   2) **bdev_io 라이프사이클**: 채널 로컬 pool에서 객체 획득 → init → submit
 *      → 모듈 콜백 → completion → 사용자 cb → free. 모든 단계가 동일 채널
 *      소유 spdk_thread에서 실행되어 lockless.
 *   3) **I/O 분할(split)**: 모듈이 지원하는 최대 segment/length를 초과하면
 *      child bdev_io로 쪼개 발행 후 parent에 결과 집계.
 *   4) **QoS (Rate limiting)**: IOPS/MBps 단위로 4가지 limit(rw_ios/rw_mbps/
 *      r_mbps/w_mbps)를 timeslice(1ms)마다 token으로 차감. 한도 초과시 큐잉.
 *   5) **NOMEM 재시도**: 모듈이 -ENOMEM을 반환하면 shared_resource의 nomem 큐에
 *      적재했다가 outstanding I/O 완료로 token이 풀리면 자동 재발행.
 *   6) **Reset/Abort**: 채널 RESET 처리 — 모든 outstanding I/O가 drain될 때까지
 *      대기 후 모듈에 reset 명령 발행. ABORT는 특정 bdev_io 1개 취소.
 *   7) **통계 (I/O stat)**: 채널별 분산 카운터를 누적하고 spdk_for_each_channel
 *      으로 합산. 에러 status 분포까지 옵션으로 추적.
 *   8) **Histogram**: latency 분포를 채널 단위 bucket array로 수집, base64로 dump.
 *   9) **Memory domain / Accel sequence**: NVMe-oF RDMA/DPDK accel framework
 *      통합 — DMA pull/push, accel(crypto/compress) sequence 실행 큐 관리.
 *  10) **LBA range locking**: parent가 child split된 동안 다른 I/O가 해당 LBA에
 *      들어오지 못하게 막는 직렬화 메커니즘.
 *  11) **Hot-remove / quiesce**: 모듈/desc 이벤트 통지, partition 재바인딩 등.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택의 정중앙. 어플리케이션과 bdev 모듈을 잇는 단일 dispatch 지점:
 *
 *   [Application]
 *     ↓ spdk_bdev_open_ext("name", ...) → spdk_bdev_get_io_channel(desc)
 *     ↓ spdk_bdev_read/write/unmap/flush/zone_*/reset/abort(desc, ch, ...)
 *   [lib/bdev/bdev.c]                  ← 이 파일
 *     ↓ bdev_channel_get_io  (per-thread cache → mempool fallback)
 *     ↓ bdev_io_init         (콜백/주소/길이 세팅)
 *     ↓ bdev_io_submit       (QoS, split, nomem, lba_lock 게이트)
 *     ↓ bdev->fn_table->submit_request(ch, bdev_io)
 *   [bdev module: NVMe / AIO / malloc / RAID / lvol / partition / ...]
 *     ↓ (실제 디바이스/메모리 액세스 — NVMe SQE+doorbell, libaio, memcpy 등)
 *     ↓ 비동기 완료 (poller가 CQ 또는 결과 큐 폴링)
 *     ↑ spdk_bdev_io_complete()
 *   [lib/bdev/bdev.c]                  ← 이 파일
 *     ↑ bdev_io_complete (stat 갱신, child→parent 집계, lba unlock,
 *                           nomem 큐 drain, QoS rewind 등 후처리)
 *     ↑ user cb_fn(bdev_io, success, cb_arg)
 *   [Application]
 *     ↓ spdk_bdev_free_io(bdev_io) — pool 반납
 *
 * 실행 컨텍스트: bdev_io 라이프사이클의 모든 단계는 **bdev_channel을 만든
 * 동일한 spdk_thread**에서 실행된다. 채널은 thread-local 자료구조 (per-thread
 * io_pool 캐시, per-channel queue 등)를 lockless로 다루기 위함. cross-thread
 * 호출은 사용자가 spdk_thread_send_msg로 우회해야 한다. 단 일부 cold path
 * (등록/해제, QoS 설정 변경, 통계 리셋, claim 등)는 g_bdev_mgr.spinlock로
 * 보호되는 글로벌 자료구조에 접근한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - include/spdk/bdev.h, bdev_module.h, bdev_zone.h : 본 파일이 구현하는
 *     공개 API 선언과 모듈측 fn_table/struct 정의.
 *   - bdev_internal.h : lib/bdev 내부 헬퍼 (bdev_channel_get_io,
 *     bdev_io_init, bdev_io_submit) — bdev_zone.c, part.c가 호출.
 *   - lib/thread (spdk_io_device_register, spdk_get_io_channel,
 *     spdk_thread_send_msg, spdk_poller_register) : 채널 모델과 메시지 전달.
 *   - lib/iobuf (spdk_iobuf_channel) : NVMe DMA 호환 데이터 버퍼 풀.
 *   - lib/accel (spdk_accel_sequence) : crypto/compress/copy offload sequence.
 *   - lib/notify : examine 완료 등 이벤트 통지.
 *   - lib/trace : SPDK trace tool 통합 — bdev I/O begin/end 트레이싱.
 *   - lib/util (histogram_data) : latency histogram bucket 배열.
 *
 * 의존하는 모듈(이 파일을 사용):
 *   - lib/bdev/bdev_rpc.c    : RPC 핸들러가 spdk_bdev_get_device_stat /
 *     spdk_bdev_set_qos_rate_limits / spdk_bdev_examine 등 호출.
 *   - lib/bdev/part.c        : partition vbdev가 자식 bdev로 forwarding.
 *   - lib/bdev/bdev_zone.c   : zoned API가 bdev_io_init/submit 사용.
 *   - module/bdev/*          : 모든 bdev 모듈이 spdk_bdev_register로 등록.
 *   - lib/nvmf, lib/vhost 등 : bdev API의 주요 소비자.
 *
 * 공유 자료구조:
 *   - struct spdk_bdev_mgr g_bdev_mgr : 글로벌 bdev 목록/이름 트리/모듈 목록/
 *     bdev_io mempool. spinlock으로 보호.
 *   - struct spdk_bdev : 등록된 단일 디바이스. 통계, claim, descriptor 목록 등.
 *   - struct spdk_bdev_channel : per-thread 채널 ctx — io_submitted 큐, stat,
 *     QoS state 등. 락 없이 소유 스레드 단독 접근.
 *   - struct spdk_bdev_io : 단일 I/O 요청. union으로 R/W/unmap/zone/copy 등의
 *     필드를 공유. internal에 상태 머신 위치, 콜백, 부모/자식 링크 보관.
 *
 * === 주요 함수/구조체 요약 ===
 *  구조체:
 *    - struct spdk_bdev_mgr           : 글로벌 bdev 매니저 (mempool/리스트/락).
 *    - struct spdk_bdev_qos_limit     : 4가지 rate-limit 중 하나의 token bucket.
 *    - struct spdk_bdev_qos           : 채널 1개로 모은 QoS 메인 객체 + poller.
 *    - struct spdk_bdev_mgmt_channel  : per-thread bdev_io cache + iobuf 채널.
 *    - struct spdk_bdev_shared_resource: per-io_device per-thread 공유 큐 (nomem).
 *    - struct spdk_bdev_channel       : per-thread per-bdev 채널 ctx — I/O 큐 4종.
 *    - struct spdk_bdev_desc          : 사용자가 open한 디스크립터 — write 권한,
 *                                       event_fn, claim 정보 보유.
 *    - struct lba_range               : LBA 잠금 범위 (split 직렬화에 사용).
 *    - enum bdev_io_retry_state       : nomem 큐에서 어떤 단계로 재시도할지.
 *
 *  공개 API(대표):
 *    - spdk_bdev_open_ext / spdk_bdev_close   : 디스크립터 라이프사이클.
 *    - spdk_bdev_register / spdk_bdev_unregister : 모듈이 bdev 등록/해제.
 *    - spdk_bdev_get_io_channel               : per-thread channel 발급.
 *    - spdk_bdev_read/write/unmap/flush/reset : 표준 I/O API.
 *    - spdk_bdev_readv_blocks_ext / writev_blocks_ext : memory_domain/dif 옵션 포함.
 *    - spdk_bdev_io_complete                  : 모듈이 비동기 완료 통지.
 *    - spdk_bdev_set_qos_rate_limits          : QoS 4종 한도 변경.
 *    - spdk_bdev_get_device_stat / reset      : 통계 조회/리셋.
 *    - spdk_bdev_module_claim_bdev_ex(_v2)    : v2 단독/공유 claim.
 *    - spdk_bdev_examine / wait_for_examine   : examine 트리거 / 완료 대기.
 *    - spdk_bdev_for_each_channel             : 모든 채널 순회 (메시지 hop).
 *
 *  핵심 static 함수:
 *    - bdev_io_submit / bdev_io_complete : I/O hot path.
 *    - bdev_channel_create / destroy     : 채널 초기화/정리.
 *    - bdev_qos_io_submit / qos_io_*     : QoS 토큰 검사/poller.
 *    - bdev_ch_retry_io                  : nomem 큐 drain.
 *    - bdev_io_split / split_*           : 분할 발행.
 *    - bdev_lock_lba_range / unlock      : LBA 직렬화.
 *    - bdev_examine                      : 모듈 examine 체인 트리거.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 C 헤더 묶음 (stdint, stdbool, string, errno 등).
 *  uint64_t, bool, memset, errno 등 본 파일 전역에서 사용. */

#include "spdk/bdev.h"
/* [한국어] bdev 공개 API — 본 파일이 구현하는 모든 spdk_bdev_* 함수 시그니처와
 *  spdk_bdev_io, spdk_bdev_opts, spdk_bdev_io_stat 등 공개 타입 정의. */

#include "spdk/accel.h"
/* [한국어] SPDK accel framework — crypto/compress/copy offload 디스패치.
 *  spdk_accel_sequence_*, memory_domain pull/push 통합에 사용. */
#include "spdk/config.h"
/* [한국어] 빌드 옵션 매크로 — SPDK_CONFIG_VTUNE 등을 통해 조건부 컴파일. */
#include "spdk/env.h"
/* [한국어] DPDK 추상화 — spdk_mempool_*, spdk_get_ticks, spdk_zmalloc 등. */
#include "spdk/thread.h"
/* [한국어] SPDK thread/poller/io_channel 모델 — 채널 자체와 cross-thread 메시지. */
#include "spdk/likely.h"
/* [한국어] spdk_likely / spdk_unlikely — fast-path 분기 예측 힌트. */
#include "spdk/queue.h"
/* [한국어] TAILQ / RB_TREE / LIST_HEAD 매크로 — bdev 목록/이름 트리/IO 큐 사용. */
#include "spdk/nvme_spec.h"
/* [한국어] NVMe 스펙 상수 — sct/sc 코드, opcode 등 status 변환에 사용. */
#include "spdk/scsi_spec.h"
/* [한국어] SCSI 스펙 상수 — sense key/code 매핑에 사용 (passthrough 경로). */
#include "spdk/notify.h"
/* [한국어] SPDK notify — bdev 등록/등록 해제/examine 완료 이벤트를 broadcast. */
#include "spdk/util.h"
/* [한국어] SPDK_COUNTOF, SPDK_ALIGN_*, SPDK_CONTAINEROF 등 헬퍼 매크로. */
#include "spdk/trace.h"
/* [한국어] SPDK trace tool — bdev I/O 시작/완료 trace point 기록. */
#include "spdk/dma.h"
/* [한국어] spdk_memory_domain — NVMe-oF RDMA 등에서 호스트/디바이스 메모리 구분
 *  + memory_domain별 translate/pull/push 콜백. */

#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈측 API — struct spdk_bdev_module/fn_table 정의, examine
 *  콜백 시그니처, claim 인터페이스 등. */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG/INFOLOG/DEBUGLOG/NOTICELOG 매크로. */
#include "spdk/string.h"
/* [한국어] spdk_strerror — errno → 사람이 읽는 문자열. */

#include "bdev_internal.h"
/* [한국어] lib/bdev 내부 사적 헤더 — bdev_channel_get_io/init/submit 등 노출
 *  (bdev_zone.c, part.c도 동일 헤더 사용). */
#include "spdk_internal/trace_defs.h"
/* [한국어] SPDK 내부 trace_tpoint ID 정의 — TRACE_BDEV_IO_START/DONE 등. */
#include "spdk_internal/assert.h"
/* [한국어] SPDK 내부 SPDK_UNREACHABLE 등 assert 변형. */

#ifdef SPDK_CONFIG_VTUNE
/* [한국어] VTune profiler 통합 빌드 옵션이 켜진 경우만 ITT API 사용. */
#include "ittnotify.h"
/* [한국어] Intel VTune ITT API — __itt_task_begin/end, __itt_domain 등 inline 선언. */
#include "ittnotify_types.h"
/* [한국어] ITT 보조 타입 정의. */
int __itt_init_ittlib(const char *, __itt_group_id);
/* [한국어] ITT 정적 라이브러리 초기화 — VTune 데몬에 attach (lib/bdev/vtune.c 참조). */
#endif

#define SPDK_BDEV_IO_POOL_SIZE			(64 * 1024 - 1)
/* [한국어] 글로벌 bdev_io mempool 슬롯 수 = 65535. 모든 채널이 공유.
 *  -1은 DPDK mempool 권장 (2^n - 1로 cache aliasing 회피). */
#define SPDK_BDEV_IO_CACHE_SIZE			256
/* [한국어] per-thread bdev_io 캐시 크기 — 256개. 채널 hot path에서
 *  mempool atomic 회피용 thread-local stack. */
#define BDEV_IO_POPULATE_BATCH_SIZE		64
/* [한국어] 캐시가 비어 mempool에서 bulk get 시 한 번에 채우는 개수. */
#define SPDK_BDEV_AUTO_EXAMINE			true
/* [한국어] 기본 examine 정책 — bdev 등록 시 자동으로 모든 examine 모듈 실행. */
#define BUF_SMALL_CACHE_SIZE			128
/* [한국어] small iobuf (4KB 이하) 채널 캐시 크기. */
#define BUF_LARGE_CACHE_SIZE			16
/* [한국어] large iobuf (4KB 초과) 채널 캐시 크기. */
#define NOMEM_THRESHOLD_COUNT			8
/* [한국어] nomem 재시도 임계값 — outstanding I/O가 이 값 이하로 떨어지면
 *  nomem 큐 drain 시도. 너무 작으면 thrashing, 너무 크면 latency 증가. */

#define SPDK_BDEV_QOS_TIMESLICE_IN_USEC		1000
/* [한국어] QoS timeslice = 1ms. 매 1ms마다 token이 1초 한도의 1/1000 만큼 보충. */
#define SPDK_BDEV_QOS_MIN_IO_PER_TIMESLICE	1
/* [한국어] timeslice당 최소 IOPS 허용량 — 한도가 너무 낮아도 1 IO/ms는 보장. */
#define SPDK_BDEV_QOS_MIN_BYTE_PER_TIMESLICE	512
/* [한국어] timeslice당 최소 byte 허용량 — 한 sector(512B) 단위. */
#define SPDK_BDEV_QOS_MIN_IOS_PER_SEC		1000
/* [한국어] QoS rw_ios_per_sec 최소 설정값 — 1000 IOPS 미만은 비현실적. */
#define SPDK_BDEV_QOS_MIN_BYTES_PER_SEC		(1024 * 1024)
/* [한국어] QoS bytes_per_sec 최소 설정값 — 1 MiB/s. */
#define SPDK_BDEV_QOS_MAX_MBYTES_PER_SEC	(UINT64_MAX / (1024 * 1024))
/* [한국어] MBps 한도의 상한 — overflow 방지용 도출값. */
#define SPDK_BDEV_QOS_LIMIT_NOT_DEFINED		UINT64_MAX
/* [한국어] "한도 미설정" 마커. UINT64_MAX = QoS 비활성 의미. */

/* The maximum number of children requests for a UNMAP or WRITE ZEROES command
 * when splitting into children requests at a time.
 */
#define SPDK_BDEV_MAX_CHILDREN_UNMAP_WRITE_ZEROES_REQS (8)
/* [한국어] 분할 발행 시 한 번에 in-flight 가능한 child UNMAP/WRITE_ZEROES 수.
 *  너무 크면 큐 압박, 너무 작으면 분할 처리 지연. */
#define BDEV_RESET_CHECK_OUTSTANDING_IO_PERIOD_IN_USEC SPDK_SEC_TO_USEC
/* [한국어] reset 진행 중 outstanding I/O 폴링 주기 = 1초.
 *  모든 in-flight I/O가 완료될 때까지 본 주기로 검사. */

/* The maximum number of children requests for a COPY command
 * when splitting into children requests at a time.
 */
#define SPDK_BDEV_MAX_CHILDREN_COPY_REQS (8)
/* [한국어] 분할 발행 시 한 번에 in-flight 가능한 child COPY 수. */

#define LOG_ALREADY_CLAIMED_ERROR(detail, bdev) \
	log_already_claimed(SPDK_LOG_ERROR, __LINE__, __func__, detail, bdev)
/* [한국어] 이미 claim된 bdev에 또 claim 시도 시 ERROR 레벨로 로깅하는 매크로.
 *  __LINE__/__func__로 호출 위치 추적. */
#ifdef DEBUG
#define LOG_ALREADY_CLAIMED_DEBUG(detail, bdev) \
	log_already_claimed(SPDK_LOG_DEBUG, __LINE__, __func__, detail, bdev)
/* [한국어] DEBUG 빌드에서는 DEBUG 레벨로도 로깅 — 디버깅 시 정보 풍부. */
#else
#define LOG_ALREADY_CLAIMED_DEBUG(detail, bdev) do {} while(0)
/* [한국어] release 빌드에서는 no-op으로 컴파일 — 성능 영향 0. */
#endif

static void log_already_claimed(enum spdk_log_level level, const int line, const char *func,
				const char *detail, struct spdk_bdev *bdev);
/* [한국어] forward declaration — claim 매크로의 helper. 정의는 본 파일 하단. */

static const char *qos_rpc_type[] = {"rw_ios_per_sec",
				     "rw_mbytes_per_sec", "r_mbytes_per_sec", "w_mbytes_per_sec"
				    };
/* [한국어] QoS 4가지 rate limit 타입 이름 (RPC JSON 직렬화 시 키 이름).
 *  순서는 enum spdk_bdev_qos_rate_limit_type와 1:1 — index 변경 금지.
 *  - rw_ios_per_sec   : read+write 합산 IOPS 한도.
 *  - rw_mbytes_per_sec: read+write 합산 대역폭 한도(MBps).
 *  - r_mbytes_per_sec : read만 대역폭 한도.
 *  - w_mbytes_per_sec : write만 대역폭 한도.
 *  설정자: 컴파일타임 상수. 읽는 자: bdev_rpc.c dump 함수 및 본 파일의 로깅. */

TAILQ_HEAD(spdk_bdev_list, spdk_bdev);
/* [한국어] 등록된 모든 bdev의 글로벌 TAILQ head 타입.
 *  TAILQ 인스턴스는 g_bdev_mgr.bdevs로 존재 (라운드트립 순회용). */

RB_HEAD(bdev_name_tree, spdk_bdev_name);
/* [한국어] bdev 이름으로 검색하기 위한 red-black tree head 타입.
 *  spdk_bdev_get_by_name이 본 트리를 O(log N) 검색. */

/*
 * [한국어]
 * bdev_name_cmp - RB tree node 비교 함수 (이름 문자열 strcmp).
 *
 * @name1/name2: 비교할 두 spdk_bdev_name 노드.
 * @return:      strcmp 결과 — RB tree가 정렬·검색에 사용.
 *
 * 호출 컨텍스트: g_bdev_mgr.spinlock 보유 상태에서만 호출됨 (트리 무결성).
 */
static int
bdev_name_cmp(struct spdk_bdev_name *name1, struct spdk_bdev_name *name2)
{
	return strcmp(name1->name, name2->name);
	/* [한국어] 이름 문자열 lexicographic 비교 — RB tree 정렬 키. */
}

RB_GENERATE_STATIC(bdev_name_tree, spdk_bdev_name, node, bdev_name_cmp);
/* [한국어] RB tree 함수(insert/remove/find) static 생성 매크로 — bdev_name_tree
 *  타입에 대해 bdev_name_cmp를 키 함수로 사용. */

struct spdk_bdev_mgr {
	struct spdk_mempool *bdev_io_pool;
	/* [한국어] 모든 bdev_io 객체의 글로벌 mempool (DPDK rte_mempool 기반).
	 *  슬롯 수 = SPDK_BDEV_IO_POOL_SIZE = 64K-1. 채널은 자기 캐시(per_thread_cache)
	 *  를 우선 사용하고 부족 시 본 풀에서 bulk get.
	 *  설정자: spdk_bdev_initialize. 해제자: spdk_bdev_finish_done. */

	void *zero_buffer;
	/* [한국어] write_zeroes 폴백용 호스트 zero buffer 포인터.
	 *  크기 ZERO_BUFFER_SIZE (1MB) × 0으로 채워진 hugepage DMA 버퍼.
	 *  설정자: bdev_mgmt_channel_create. 해제자: bdev_module_finish. */

	TAILQ_HEAD(bdev_module_list, spdk_bdev_module) bdev_modules;
	/* [한국어] 등록된 모든 bdev_module 목록 (constructor에서 등록).
	 *  examine/init/fini 체인 순회에 사용. */

	struct spdk_bdev_list bdevs;
	/* [한국어] 등록된 모든 spdk_bdev TAILQ — spdk_bdev_first/next 순회 대상. */

	struct bdev_name_tree bdev_names;
	/* [한국어] bdev 이름 → bdev 매핑 RB tree. O(log N) 검색. */

	bool init_complete;
	/* [한국어] spdk_bdev_initialize 완료 여부. RPC 핸들러가 RUNTIME 가드용으로 확인. */
	bool module_init_complete;
	/* [한국어] 모든 모듈의 module_init 콜백 완료 여부. */

	struct spdk_spinlock spinlock;
	/* [한국어] bdevs / bdev_names / async_bdev_opens 접근 보호용.
	 *  hot path는 채널 lockless이지만 cold path(register/unregister/open)는 본 락 사용. */

	TAILQ_HEAD(, spdk_bdev_open_async_ctx) async_bdev_opens;
	/* [한국어] 아직 등록되지 않은 bdev에 대해 spdk_bdev_open_async 호출이 있을 때
	 *  대기 중인 컨텍스트 목록. bdev 등록 시 매칭되는 항목을 깨움. */

#ifdef SPDK_CONFIG_VTUNE
	__itt_domain	*domain;
	/* [한국어] VTune ITT domain — 모든 bdev I/O task가 이 domain 하위에 timeline 표시. */
#endif
};

static struct spdk_bdev_mgr g_bdev_mgr = {
	.bdev_modules = TAILQ_HEAD_INITIALIZER(g_bdev_mgr.bdev_modules),
	.bdevs = TAILQ_HEAD_INITIALIZER(g_bdev_mgr.bdevs),
	.bdev_names = RB_INITIALIZER(g_bdev_mgr.bdev_names),
	.init_complete = false,
	.module_init_complete = false,
	.async_bdev_opens = TAILQ_HEAD_INITIALIZER(g_bdev_mgr.async_bdev_opens),
};
/* [한국어] g_bdev_mgr 글로벌 인스턴스 — 모든 bdev 상태의 single source of truth.
 *  TAILQ/RB head는 정적 초기화, spinlock은 constructor _bdev_init에서 별도 초기화. */

/*
 * [한국어]
 * _bdev_init - 라이브러리 로드 시점에 자동 호출되는 constructor (spinlock init).
 *
 * GCC __attribute__((constructor)): main() 실행 전에 ld가 자동 호출.
 * SPDK 사용 측은 별도 명시적 초기화 호출 없이도 lib/bdev이 로드되면 본 함수 실행됨.
 * spinlock은 정적 초기화로 만들 수 없어 런타임에 호출이 필요.
 */
static void
__attribute__((constructor))
_bdev_init(void)
{
	spdk_spin_init(&g_bdev_mgr.spinlock);
	/* [한국어] glibc/pthread 기반 SPDK spinlock 초기화 — 글로벌 자료구조 보호용. */
}

typedef void (*lock_range_cb)(struct lba_range *range, void *ctx, int status);
/* [한국어] LBA range lock 비동기 완료 콜백 함수 포인터.
 *  bdev_lock_lba_range가 모든 in-flight I/O drain 후 본 콜백을 호출.
 *  설정자: 잠금 요청자 (예: split 진행자). 호출자: lock_range가 완료될 때. */

typedef void (*bdev_copy_bounce_buffer_cpl)(void *ctx, int rc);
/* [한국어] memory_domain pull/push 완료 콜백.
 *  RDMA 등 외부 메모리 도메인에서 호스트 DMA buffer로 데이터 이동 완료 통지. */

struct lba_range {
	struct spdk_bdev		*bdev;
	/* [한국어] 잠금이 걸린 대상 bdev. */
	uint64_t			offset;
	/* [한국어] 잠금 시작 블록 인덱스. */
	uint64_t			length;
	/* [한국어] 잠금 길이 (블록 수). */
	bool				quiesce;
	/* [한국어] quiesce 모드 잠금 여부 — true면 기존 in-flight I/O가 모두 drain될 때까지 대기,
	 *  false면 일반 LBA 잠금. */
	void				*locked_ctx;
	/* [한국어] 잠금을 건 자의 컨텍스트 포인터 — unlock 시 같은 ctx로 매칭하여 해제. */
	struct spdk_thread		*owner_thread;
	/* [한국어] 잠금을 요청한 spdk_thread — unlock 콜백 디스패치 시 사용. */
	struct spdk_bdev_channel	*owner_ch;
	/* [한국어] 잠금을 요청한 채널 — 단일 채널 잠금 케이스에서 owner 추적용. */
	TAILQ_ENTRY(lba_range)		tailq;
	/* [한국어] bdev_channel.locked_ranges TAILQ에 연결되는 노드 — 채널 단위 잠금 목록. */
	TAILQ_ENTRY(lba_range)		tailq_module;
	/* [한국어] bdev->internal.locked_ranges TAILQ에 연결되는 노드 — bdev 단위 잠금 목록. */
};

static struct spdk_bdev_opts	g_bdev_opts = {
	.bdev_io_pool_size = SPDK_BDEV_IO_POOL_SIZE,
	.bdev_io_cache_size = SPDK_BDEV_IO_CACHE_SIZE,
	.bdev_auto_examine = SPDK_BDEV_AUTO_EXAMINE,
	.iobuf_small_cache_size = BUF_SMALL_CACHE_SIZE,
	.iobuf_large_cache_size = BUF_LARGE_CACHE_SIZE,
};

static spdk_bdev_init_cb	g_init_cb_fn = NULL;
static void			*g_init_cb_arg = NULL;
static struct spdk_thread	*g_init_thread = NULL;

static spdk_bdev_fini_cb	g_fini_cb_fn = NULL;
static void			*g_fini_cb_arg = NULL;
static struct spdk_thread	*g_fini_thread = NULL;

struct spdk_bdev_qos_limit {
	/** IOs or bytes allowed per second (i.e., 1s). */
	uint64_t limit;
	/* [한국어] 사용자 설정 한도 — IOPS 또는 bytes/s.
	 *  UINT64_MAX(=SPDK_BDEV_QOS_LIMIT_NOT_DEFINED)이면 이 limit 종류는 비활성.
	 *  설정자: spdk_bdev_set_qos_rate_limits (RPC 또는 사용자 호출).
	 *  읽는 자: poller가 timeslice 시작 시 remaining_this_timeslice 보충에 사용. */

	/** Remaining IOs or bytes allowed in current timeslice (e.g., 1ms).
	 *  For remaining bytes, allowed to run negative if an I/O is submitted when
	 *  some bytes are remaining, but the I/O is bigger than that amount. The
	 *  excess will be deducted from the next timeslice.
	 */
	int64_t remaining_this_timeslice;
	/* [한국어] 현재 timeslice에서 남은 token 수 (IOPS는 int 개수, bytes는 byte 수).
	 *  bytes 한도의 경우 큰 I/O 1개가 남은 quota를 음수로 만들 수 있으며 다음
	 *  timeslice에서 보충되는 만큼이 차감되어 시작. (over-shoot 허용 모델.)
	 *  설정자: timeslice poller가 매 1ms마다 보충 (max_per_timeslice 가산),
	 *           queue_io 통과 시 차감.
	 *  읽는 자: queue_io 함수 (예: bdev_qos_io_per_timeslice_queue_io). */

	/** Minimum allowed IOs or bytes to be issued in one timeslice (e.g., 1ms). */
	uint32_t min_per_timeslice;
	/* [한국어] timeslice당 최소 token — 한도가 너무 낮아도 이 만큼은 보장.
	 *  IOPS: SPDK_BDEV_QOS_MIN_IO_PER_TIMESLICE(1), bytes: 512. */

	/** Maximum allowed IOs or bytes to be issued in one timeslice (e.g., 1ms). */
	uint32_t max_per_timeslice;
	/* [한국어] timeslice 1회 보충량 = limit × timeslice_ratio.
	 *  설정: bdev_qos_update_max_quota_per_timeslice이 한도 변경 시 재계산.
	 *  사용: poller가 remaining_this_timeslice에 가산할 때. */

	/** Function to check whether to queue the IO.
	 * If The IO is allowed to pass, the quota will be reduced correspondingly.
	 */
	bool (*queue_io)(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);
	/* [한국어] limit 종류별로 다른 검사 함수 — 본 I/O가 토큰 부족으로 큐잉되어야 하는지 판단.
	 *  true=큐잉 필요(token 부족), false=통과(token 차감 완료).
	 *  종류별 구현: bdev_qos_rw_queue_io / bdev_qos_rw_bps_queue_io /
	 *               bdev_qos_r_bps_queue_io / bdev_qos_w_bps_queue_io. */

	/** Function to rewind the quota once the IO was allowed to be sent by this
	 * limit but queued due to one of the further limits.
	 */
	void (*rewind_quota)(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);
	/* [한국어] 다른 limit이 차단해 이 I/O가 큐잉되었을 때 본 limit에서 미리 차감한 quota를 되돌림.
	 *  4가지 limit를 순회 검사하다 후순위에서 fail하면 앞에서 차감된 quota를 복원해야 일관성 유지. */
};

struct spdk_bdev_qos {
	/** Types of structure of rate limits. */
	struct spdk_bdev_qos_limit rate_limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];
	/* [한국어] 4가지 limit 배열 (rw_ios, rw_bps, r_bps, w_bps).
	 *  enum spdk_bdev_qos_rate_limit_type 순서로 인덱스. */

	/** The channel that all I/O are funneled through. */
	struct spdk_bdev_channel *ch;
	/* [한국어] QoS가 활성화된 bdev의 master 채널 — 모든 QoS-검사 I/O가 이 채널을 거침.
	 *  bdev 전체에서 단 하나의 채널이 QoS를 담당해 직렬화. */

	/** The thread on which the poller is running. */
	struct spdk_thread *thread;
	/* [한국어] poller가 실행되는 thread (= ch가 속한 thread). timeslice 보충 + 큐잉 I/O drain. */

	/** Size of a timeslice in tsc ticks. */
	uint64_t timeslice_size;
	/* [한국어] timeslice 길이 (CPU tsc 단위로 환산된 1ms). spdk_get_ticks_hz와 비례.
	 *  poller가 (현재 tsc - last_timeslice ≥ timeslice_size)인지 검사. */

	/** Timestamp of start of last timeslice. */
	uint64_t last_timeslice;
	/* [한국어] 마지막 timeslice 시작 tsc. 매 poll 시 (now - last)로 경과 timeslice 수 계산. */

	/** Poller that processes queued I/O commands each time slice. */
	struct spdk_poller *poller;
	/* [한국어] 1ms 주기 poller — timeslice 도래 시 remaining 보충 + qos_queued_io drain. */
};

struct spdk_bdev_mgmt_channel {
	/*
	 * Each thread keeps a cache of bdev_io - this allows
	 *  bdev threads which are *not* DPDK threads to still
	 *  benefit from a per-thread bdev_io cache.  Without
	 *  this, non-DPDK threads fetching from the mempool
	 *  incur a cmpxchg on get and put.
	 */
	bdev_io_stailq_t per_thread_cache;
	/* [한국어] thread-local bdev_io 캐시 (STAILQ — free list).
	 *  hot path: bdev_channel_get_io가 여기서 pop. 비면 mempool에서 bulk get.
	 *  put: bdev_io_put이 여기로 push. 초과분만 mempool 반납.
	 *  락 없음 — 이 채널을 소유한 thread만 접근. */
	uint32_t	per_thread_cache_count;
	/* [한국어] 캐시에 들어 있는 bdev_io 슬롯 개수. bdev_io_cache_size 한도 검사용. */
	uint32_t	bdev_io_cache_size;
	/* [한국어] 캐시 최대 크기 — SPDK_BDEV_IO_CACHE_SIZE(256). 초과 시 mempool 반납. */

	struct spdk_iobuf_channel iobuf;
	/* [한국어] iobuf (small/large) thread-local 채널 — NVMe DMA buffer 풀.
	 *  bdev_io에 데이터 buffer가 필요할 때 본 채널에서 할당. */

	TAILQ_HEAD(, spdk_bdev_shared_resource)	shared_resources;
	/* [한국어] 본 thread가 사용 중인 모든 (per-io_device) shared_resource 목록 — nomem 큐 등을 보유. */
	TAILQ_HEAD(, spdk_bdev_io_wait_entry)	io_wait_queue;
	/* [한국어] bdev_io pool이 고갈되어 대기 중인 사용자 콜백 큐.
	 *  put 시 1개 깨움 — 사용자가 등록한 cb_fn(cb_arg)로 재시도 trigger. */
};

/*
 * Per-module (or per-io_device) data. Multiple bdevs built on the same io_device
 * will queue here their IO that awaits retry. It makes it possible to retry sending
 * IO to one bdev after IO from other bdev completes.
 */
struct spdk_bdev_shared_resource {
	/* The bdev management channel */
	struct spdk_bdev_mgmt_channel *mgmt_ch;
	/* [한국어] 본 shared_resource가 속한 thread의 mgmt_channel — 같은 io_device를 쓰는
	 *  여러 bdev들이 이 mgmt_ch 하위의 같은 shared_resource를 공유. */

	/*
	 * Count of I/O submitted to bdev module and waiting for completion.
	 * Incremented before submit_request() is called on an spdk_bdev_io.
	 */
	uint64_t		io_outstanding;
	/* [한국어] 이 thread × io_device의 in-flight I/O 개수.
	 *  submit_request 호출 직전 ++, completion 처리 시 --.
	 *  nomem 큐 drain 임계값 비교에 사용. */

	/*
	 * Queue of IO awaiting retry because of a previous NOMEM status returned
	 *  on this channel.
	 */
	bdev_io_tailq_t		nomem_io;
	/* [한국어] 모듈이 -ENOMEM 또는 NOMEM_AND_RETRY를 반환한 I/O들의 재시도 대기 큐.
	 *  outstanding이 nomem_threshold 이하로 떨어지면 bdev_shared_ch_retry_io가 drain. */

	/*
	 * Threshold which io_outstanding must drop to before retrying nomem_io.
	 */
	uint64_t		nomem_threshold;
	/* [한국어] nomem 재시도 임계값. NOMEM이 발생한 시점의 outstanding count - NOMEM_THRESHOLD_COUNT.
	 *  thrashing 방지 — token이 충분히 풀린 후에만 재시도. */

	/*
	 * Indicate whether aborting nomem I/Os is in progress.
	 * If true, we should not touch the nomem_io list on I/O completions.
	 */
	bool			nomem_abort_in_progress;
	/* [한국어] nomem 큐의 I/O들을 abort 중인지 여부 — true면 completion 처리 시 nomem_io 큐를 건드리지 않음.
	 *  reset 진행 시 nomem 큐 일괄 abort를 위한 가드. */

	/* I/O channel allocated by a bdev module */
	struct spdk_io_channel	*shared_ch;
	/* [한국어] bdev 모듈이 만든 underlying io_channel (예: NVMe qpair channel).
	 *  같은 io_device를 공유하는 여러 bdev들이 모두 이 채널을 공유. */

	struct spdk_poller	*nomem_poller;
	/* [한국어] nomem 큐가 비어있지 않은 상태에서 outstanding=0인 경우(완전 stall 방지)
	 *  주기적으로 재시도 trigger하는 poller. */

	/* Refcount of bdev channels using this resource */
	uint32_t		ref;
	/* [한국어] 이 shared_resource를 가리키는 bdev_channel 개수.
	 *  같은 io_device 위에 여러 bdev이 있을 때 각 bdev_channel이 본 자원을 공유. */

	TAILQ_ENTRY(spdk_bdev_shared_resource) link;
	/* [한국어] mgmt_ch->shared_resources TAILQ 노드. */
};

#define BDEV_CH_RESET_IN_PROGRESS	(1 << 0)
/* [한국어] 채널 flag 비트 0 — 현재 채널에서 reset 진행 중. submit 경로가 거부하도록. */
#define BDEV_CH_QOS_ENABLED		(1 << 1)
/* [한국어] 채널 flag 비트 1 — 본 채널이 QoS master 채널임을 표시. */

struct spdk_bdev_channel {
	struct spdk_bdev	*bdev;
	/* [한국어] 이 채널이 속한 bdev — 모든 I/O가 이 bdev로 향함. 채널 lifetime 동안 불변. */

	/* The channel for the underlying device */
	struct spdk_io_channel	*channel;
	/* [한국어] 같은 thread에서 발급된 underlying bdev module의 io_channel.
	 *  bdev 모듈의 submit_request에 전달되어 실제 디바이스 큐에 접근. */

	/* Accel channel */
	struct spdk_io_channel	*accel_channel;
	/* [한국어] 같은 thread의 accel framework io_channel — sequence 실행에 사용.
	 *  crypto/compress/copy offload가 필요한 I/O가 본 채널로 sequence submit. */

	/* Per io_device per thread data */
	struct spdk_bdev_shared_resource *shared_resource;
	/* [한국어] 본 채널이 속한 thread × io_device의 공유 자원 — nomem 큐, outstanding 카운터 등. */

	struct spdk_bdev_io_stat *stat;
	/* [한국어] 본 채널의 I/O 통계 누적 — IOPS/bytes/latency/error 분포 등.
	 *  spdk_for_each_channel로 모든 채널 합산해 spdk_bdev_get_device_stat 응답. */

	/*
	 * Count of I/O submitted to the underlying dev module through this channel
	 * and waiting for completion.
	 */
	uint64_t		io_outstanding;
	/* [한국어] 본 채널이 발행한 in-flight I/O 수 (split된 child도 카운트).
	 *  reset 대기 / 통계용. */

	/*
	 * List of all submitted I/Os including I/O that are generated via splitting.
	 */
	bdev_io_tailq_t		io_submitted;
	/* [한국어] 본 채널이 제출한 모든 in-flight bdev_io 목록 (split된 child 포함).
	 *  reset 시 abort 대상 + timeout poller 검사 대상. */

	/*
	 * List of spdk_bdev_io that are currently queued because they write to a locked
	 * LBA range.
	 */
	bdev_io_tailq_t		io_locked;
	/* [한국어] 잠긴 LBA 범위에 들어와 대기 중인 I/O 목록 — unlock 시 drain. */

	/* List of I/Os with accel sequence being currently executed */
	bdev_io_tailq_t		io_accel_exec;
	/* [한국어] accel sequence 실행 중인 I/O 목록 — sequence 완료 시 제거 후 submit 진행. */

	/* List of I/Os doing memory domain pull/push */
	bdev_io_tailq_t		io_memory_domain;
	/* [한국어] memory_domain pull/push (RDMA buffer ↔ DMA buffer) 진행 중 I/O 목록. */

	uint32_t		flags;
	/* [한국어] BDEV_CH_RESET_IN_PROGRESS / BDEV_CH_QOS_ENABLED 비트 마스크. */

	/* Counts number of bdev_io in the io_submitted TAILQ */
	uint16_t		queue_depth;
	/* [한국어] io_submitted 길이 캐시 (TAILQ 길이를 O(1)로 노출). QD 샘플링 통계용. */

	uint16_t		trace_id;
	/* [한국어] SPDK trace tool용 채널 식별자 — trace 이벤트에 포함. */

	struct spdk_histogram_data *histogram;
	/* [한국어] 본 채널의 latency histogram bucket 배열. enable_histogram RPC로 활성화.
	 *  NULL이면 비활성. completion 시 spdk_histogram_data_tally로 bucket 갱신. */

#ifdef SPDK_CONFIG_VTUNE
	uint64_t		start_tsc;
	/* [한국어] 통계 interval 시작 tsc (VTune 통합 빌드 한정). */
	uint64_t		interval_tsc;
	/* [한국어] 한 interval의 tsc 단위 길이 — 1초 = spdk_get_ticks_hz. */
	__itt_string_handle	*handle;
	/* [한국어] VTune ITT task 이름 핸들 — task_begin/end에 사용. */
	struct spdk_bdev_io_stat *prev_stat;
	/* [한국어] 직전 interval의 통계 — 현재와의 delta 계산해 VTune에 보고. */
#endif

	lba_range_tailq_t	locked_ranges;
	/* [한국어] 본 채널이 건 LBA 잠금 목록 — split parent가 child 발행 중 사용. */

	/** List of I/Os queued by QoS. */
	bdev_io_tailq_t		qos_queued_io;
	/* [한국어] QoS token 부족으로 큐잉된 I/O 목록 — poller가 timeslice 보충 후 drain. */
};

struct media_event_entry {
	struct spdk_bdev_media_event	event;
	/* [한국어] media event 본체 (LBA 범위, 이벤트 종류). */
	TAILQ_ENTRY(media_event_entry)	tailq;
	/* [한국어] desc의 pending/free media event 큐에 연결. */
};

#define MEDIA_EVENT_POOL_SIZE 64
/* [한국어] desc 1개당 사전 할당하는 media event 슬롯 수 — 짧은 시간에 동시 발생 가능한 최대치. */

struct spdk_bdev_desc {
	struct spdk_bdev		*bdev;
	/* [한국어] 이 desc가 가리키는 bdev. open_ext 시 설정, close까지 유효. */
	bool				write;
	/* [한국어] write 권한 여부 — open_ext 시 write=true 호출 결과 보관.
	 *  write desc가 1개 이상이면 다른 desc는 write로 열 수 없음 (배타 제어). */
	struct spdk_bdev_open_opts	opts;
	/* [한국어] open 시 전달된 옵션 — hide_metadata 등. */
	struct spdk_thread		*thread;
	/* [한국어] desc를 open한 thread — close는 동일 thread에서만 가능. */
	struct {
		spdk_bdev_event_cb_t event_fn;
		/* [한국어] hot-remove/resize/MEDIA_MGMT 등 비동기 이벤트 콜백. */
		void *ctx;
		/* [한국어] event_fn에 전달되는 사용자 컨텍스트. */
	}				callback;
	bool				closed;
	/* [한국어] close 시작 표시 — true면 더 이상 I/O 발행 금지. ref==0이 되면 실제 free. */
	struct spdk_spinlock		spinlock;
	/* [한국어] refs / closed / media events 접근 보호. */
	uint32_t			refs;
	/* [한국어] 본 desc를 사용 중인 in-flight I/O 개수 + 외부 참조. close 후 refs==0이 되면 실제 해제. */
	TAILQ_HEAD(, media_event_entry)	pending_media_events;
	/* [한국어] 사용자에게 전달 대기 중인 media event 큐. */
	TAILQ_HEAD(, media_event_entry)	free_media_events;
	/* [한국어] 재사용 가능한 free media_event_entry 슬롯 큐. */
	struct media_event_entry	*media_events_buffer;
	/* [한국어] media_events_buffer 슬롯 배열 (calloc 한 번). free/pending 큐가 이 안에서 회전. */
	TAILQ_ENTRY(spdk_bdev_desc)	link;
	/* [한국어] bdev->internal.open_descs 목록 노드 — bdev이 가진 모든 desc 추적. */

	uint64_t		timeout_in_sec;
	/* [한국어] I/O 타임아웃 (초). 0이면 timeout 없음. spdk_bdev_set_timeout으로 설정. */
	spdk_bdev_io_timeout_cb	cb_fn;
	/* [한국어] timeout 발생 시 호출되는 콜백 — abort 또는 reset 결정은 사용자 책임. */
	void			*cb_arg;
	/* [한국어] cb_fn 컨텍스트. */
	struct spdk_poller	*io_timeout_poller;
	/* [한국어] 1초마다 호출되어 io_submitted 큐에서 timeout 도달한 I/O 검출. */
	struct spdk_bdev_module_claim	*claim;
	/* [한국어] v2 claim 시 desc가 가진 claim 객체. NULL이면 claim 없음. */
};

struct spdk_bdev_iostat_ctx {
	struct spdk_bdev_io_stat *stat;
	/* [한국어] for_each_channel 누적 결과를 모을 stat 출력 버퍼. */
	enum spdk_bdev_reset_stat_mode reset_mode;
	/* [한국어] 통계 조회와 동시에 리셋할지 여부 (NONE/ALL/MAXIMUMS). */
	spdk_bdev_get_device_stat_cb cb;
	/* [한국어] 모든 채널 합산 완료 시 호출될 사용자 콜백. */
	void *cb_arg;
	/* [한국어] cb 컨텍스트. */
};

struct set_qos_limit_ctx {
	void (*cb_fn)(void *cb_arg, int status);
	/* [한국어] QoS 한도 변경 비동기 완료 콜백. */
	void *cb_arg;
	/* [한국어] cb_fn 컨텍스트. */
	struct spdk_bdev *bdev;
	/* [한국어] 한도 변경 대상 bdev. */
};

struct spdk_bdev_channel_iter {
	spdk_bdev_for_each_channel_msg fn;
	/* [한국어] 각 채널에 메시지 send 시 실행될 함수. */
	spdk_bdev_for_each_channel_done cpl;
	/* [한국어] 모든 채널 순회 완료 시 호출될 콜백. */
	struct spdk_io_channel_iter *i;
	/* [한국어] spdk_thread 측 iter — 내부 상태 추적. */
	void *ctx;
	/* [한국어] 사용자 컨텍스트 — fn/cpl로 전달. */
};

struct spdk_bdev_io_error_stat {
	uint32_t error_status[-SPDK_MIN_BDEV_IO_STATUS];
	/* [한국어] bdev_io status 코드별 발생 횟수 카운터 배열.
	 *  index = -status (FAILED=-1, NOMEM=-2 등).
	 *  io_error_stat 옵션이 켜진 채널에서만 본 구조체 할당. */
};

enum bdev_io_retry_state {
	BDEV_IO_RETRY_STATE_INVALID,
	/* [한국어] 미정의/초기값 — 정상 흐름에서는 사용 안 됨. */
	BDEV_IO_RETRY_STATE_PULL,
	/* [한국어] memory_domain pull 단계에서 NOMEM 발생 — 재시도 시 pull부터. */
	BDEV_IO_RETRY_STATE_PULL_MD,
	/* [한국어] separate metadata pull 단계에서 NOMEM. */
	BDEV_IO_RETRY_STATE_SUBMIT,
	/* [한국어] 모듈 submit_request 단계에서 NOMEM — 가장 일반적 케이스. */
	BDEV_IO_RETRY_STATE_PUSH,
	/* [한국어] completion 후 memory_domain push 단계에서 NOMEM. */
	BDEV_IO_RETRY_STATE_PUSH_MD,
	/* [한국어] completion 후 separate metadata push 단계에서 NOMEM. */
	BDEV_IO_RETRY_STATE_GET_ACCEL_BUF,
	/* [한국어] accel framework buffer 획득 단계에서 NOMEM. */
};

#define __bdev_to_io_dev(bdev)		(((char *)bdev) + 1)
/* [한국어] bdev 포인터 → io_device 식별자 변환. 단순 +1 트릭으로 NULL bdev과
 *  io_device 등록 식별자가 같은 주소공간을 공유하지 않도록. */
#define __bdev_from_io_dev(io_dev)	((struct spdk_bdev *)(((char *)io_dev) - 1))
/* [한국어] io_device → bdev 역변환. */
#define __io_ch_to_bdev_ch(io_ch)	((struct spdk_bdev_channel *)spdk_io_channel_get_ctx(io_ch))
/* [한국어] spdk_io_channel → bdev_channel (trailing ctx 영역 cast). */
#define __io_ch_to_bdev_mgmt_ch(io_ch)	((struct spdk_bdev_mgmt_channel *)spdk_io_channel_get_ctx(io_ch))
/* [한국어] spdk_io_channel → bdev_mgmt_channel (mgmt io_device 채널 케이스). */

/* [한국어] 아래는 본 파일 내에서 서로 호출하는 static 함수들의 forward declaration.
 *  hot path 흐름: submit → 모듈 콜백 → complete → cb_fn.
 *  분기 경로: bounce buffer pull/push, accel sequence, QoS, lba lock 등. */
static inline void bdev_io_complete(void *ctx);
/* [한국어] bdev_io completion 진입점 — 모듈이 완료 통지하면 cb_fn 호출까지 진행. */
static inline void bdev_io_complete_unsubmitted(struct spdk_bdev_io *bdev_io);
/* [한국어] submit 전에 실패한 I/O 완료 처리 (예: split 실패) — submitted 큐를 거치지 않음. */
static void bdev_io_push_bounce_md_buf(struct spdk_bdev_io *bdev_io);
/* [한국어] completion 후 bounce metadata buffer를 사용자 buffer로 push (memory_domain). */
static void bdev_io_push_bounce_data(struct spdk_bdev_io *bdev_io);
/* [한국어] completion 후 bounce data buffer를 사용자 buffer로 push. */
static void _bdev_io_get_accel_buf(struct spdk_bdev_io *bdev_io);
/* [한국어] accel framework에서 buffer 할당 (NOMEM 시 재시도). */

static void bdev_write_zero_buffer_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
/* [한국어] write_zeroes 폴백의 한 chunk write 완료 콜백 — 다음 chunk 발행 trigger. */
static void bdev_write_zero_buffer(void *bdev_io);
/* [한국어] write_zeroes 폴백 본체 — 1MB zero buffer로 반복 write. */

static void bdev_enable_qos_msg(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
				struct spdk_io_channel *ch, void *_ctx);
/* [한국어] QoS 활성화 메시지 — 모든 채널에 hop하며 BDEV_CH_QOS_ENABLED 비트 set. */
static void bdev_enable_qos_done(struct spdk_bdev *bdev, void *_ctx, int status);
/* [한국어] QoS 활성화 완료 콜백 — 사용자 cb 호출. */

static int bdev_readv_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				     struct iovec *iov, int iovcnt, void *md_buf, uint64_t offset_blocks,
				     uint64_t num_blocks,
				     struct spdk_memory_domain *domain, void *domain_ctx,
				     struct spdk_accel_sequence *seq, uint32_t dif_check_flags,
				     spdk_bdev_io_completion_cb cb, void *cb_arg);
/* [한국어] readv 모든 변형의 공통 내부 진입점 — md buf, memory_domain, accel seq, dif 등 옵션 전부 받음. */
static int bdev_writev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				      struct iovec *iov, int iovcnt, void *md_buf,
				      uint64_t offset_blocks, uint64_t num_blocks,
				      struct spdk_memory_domain *domain, void *domain_ctx,
				      struct spdk_accel_sequence *seq, uint32_t dif_check_flags,
				      uint32_t nvme_cdw12_raw, uint32_t nvme_cdw13_raw,
				      spdk_bdev_io_completion_cb cb, void *cb_arg);
/* [한국어] writev 모든 변형의 공통 내부 진입점 — md buf, domain, seq, dif, NVMe raw 옵션 등 모두 받음. */

static int bdev_lock_lba_range(struct spdk_bdev_desc *desc, struct spdk_io_channel *_ch,
			       uint64_t offset, uint64_t length,
			       lock_range_cb cb_fn, void *cb_arg);
/* [한국어] LBA range 잠금 — 진행 중 I/O가 모두 drain되면 cb 호출. split 직렬화에 사용. */

static int bdev_unlock_lba_range(struct spdk_bdev_desc *desc, struct spdk_io_channel *_ch,
				 uint64_t offset, uint64_t length,
				 lock_range_cb cb_fn, void *cb_arg);
/* [한국어] LBA range 잠금 해제 — io_locked 대기 큐 drain. */

static bool bdev_abort_queued_io(bdev_io_tailq_t *queue, struct spdk_bdev_io *bio_to_abort);
/* [한국어] 특정 bdev_io를 큐에서 찾아 abort. abort 명령 처리 시 사용. */
static bool bdev_abort_buf_io(struct spdk_bdev_mgmt_channel *ch, struct spdk_bdev_io *bio_to_abort);
/* [한국어] mgmt 채널의 iobuf 대기 큐에서 abort. */
static bool bdev_abort_unsubmitted_buf_io(struct spdk_bdev_mgmt_channel *mgmt_ch, void *bio_cb_arg);
/* [한국어] iobuf 대기 중 아직 submit 안 된 I/O abort. */

static bool claim_type_is_v2(enum spdk_bdev_claim_type type);
/* [한국어] claim 타입이 v2(READ_MANY/READ_MANY_WRITE_ONE/READ_MANY_WRITE_MANY/READ_ONE_WRITE_ONE)인지 판별. */
static void bdev_desc_release_claims(struct spdk_bdev_desc *desc);
/* [한국어] desc close 시 해당 desc의 claim 목록 해제. */
static void claim_reset(struct spdk_bdev *bdev);
/* [한국어] bdev unregister 시 claim 객체 일괄 정리. */

static void bdev_ch_retry_io(struct spdk_bdev_channel *bdev_ch);
/* [한국어] 채널의 nomem 큐 drain — outstanding이 threshold 이하면 호출. */

static bool bdev_io_should_split(struct spdk_bdev_io *bdev_io);
/* [한국어] I/O가 모듈의 max_segments/max_size를 초과해 split 필요한지 판단. */

#define bdev_get_ext_io_opt(opts, field, defval) \
	((opts) != NULL ? SPDK_GET_FIELD(opts, field, defval) : (defval))
/* [한국어] ext_io_opts에서 field 안전 추출 매크로 — opts가 NULL이거나 해당 field가 opts_size
 *  범위 밖이면 defval 반환. ABI-safe 패턴. */

/*
 * [한국어]
 * bdev_ch_add_to_io_submitted - bdev_io를 채널의 io_submitted 큐에 추가 + queue_depth++.
 *
 * @bdev_io: submitted 표시할 I/O.
 *
 * submit 직전에 호출. io_submitted는 reset 시 abort 대상 + timeout 검사 대상 + 통계 QD 측정용.
 * 락 없음 — 채널 소유 thread만 접근.
 */
static inline void
bdev_ch_add_to_io_submitted(struct spdk_bdev_io *bdev_io)
{
	TAILQ_INSERT_TAIL(&bdev_io->internal.ch->io_submitted, bdev_io, internal.ch_link);
	/* [한국어] TAILQ 끝에 추가 — FIFO 순서 유지. */
	bdev_io->internal.ch->queue_depth++;
	/* [한국어] QD 카운터 증가 — 통계 샘플링 시 사용. */
}

/*
 * [한국어]
 * bdev_ch_remove_from_io_submitted - completion 시 io_submitted 큐에서 제거 + queue_depth--.
 *
 * @bdev_io: 완료된 I/O.
 *
 * completion 진입점에서 호출. 락 없음.
 */
static inline void
bdev_ch_remove_from_io_submitted(struct spdk_bdev_io *bdev_io)
{
	TAILQ_REMOVE(&bdev_io->internal.ch->io_submitted, bdev_io, internal.ch_link);
	/* [한국어] TAILQ에서 자신 제거. */
	bdev_io->internal.ch->queue_depth--;
	/* [한국어] QD 카운터 감소. */
}

/*
 * [한국어]
 * spdk_bdev_get_opts - 현재 글로벌 bdev 옵션을 사용자 buffer로 복사 (ABI-safe).
 *
 * @opts:      [out] 현재 옵션을 받을 buffer (호출자 스택/힙).
 * @opts_size: 사용자가 본 구조체 크기 (sizeof) — ABI versioning용.
 *
 * 사용자가 옵션을 일부 변경하기 전에 현재 값을 읽어오는 데 사용.
 * SET_FIELD 매크로는 호출자의 opts_size 안에 들어오는 필드만 복사 — 새 필드가
 * 추가되어도 이전 호출자는 자기가 모르는 영역을 건드리지 않는다.
 *
 * 호출 컨텍스트: 어느 thread에서든 안전 (g_bdev_opts는 init 후 거의 read-only).
 */
void
spdk_bdev_get_opts(struct spdk_bdev_opts *opts, size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		/* [한국어] NULL 검증 — 호출자 버그. */
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		/* [한국어] size=0 — ABI 추적 불가. 호출자 버그. */
		return;
	}

	opts->opts_size = opts_size;
	/* [한국어] 호출자가 본 크기 기록 — 향후 매크로 등이 참조 가능. */

#define SET_FIELD(field) \
	if (offsetof(struct spdk_bdev_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = g_bdev_opts.field; \
	} \

	SET_FIELD(bdev_io_pool_size);
	SET_FIELD(bdev_io_cache_size);
	SET_FIELD(bdev_auto_examine);
	SET_FIELD(iobuf_small_cache_size);
	SET_FIELD(iobuf_large_cache_size);

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_opts) == 32, "Incorrect size");

#undef SET_FIELD
}

/*
 * [한국어]
 * spdk_bdev_set_opts - 글로벌 bdev 옵션 설정 (init 전에 1회 호출).
 *
 * @opts:    설정할 옵션 buffer.
 * @return:  0=성공, -1=잘못된 인자.
 *
 * spdk_bdev_initialize 전에 호출되어야 의미 있음. 초기화 후 변경은 위험.
 * bdev_io_pool_size가 thread 수 × cache_size보다 작으면 mempool exhausted 가능 → 검증.
 *
 * 호출자: 보통 RPC bdev_set_options 핸들러 (STARTUP phase) 또는 사용자 init 코드.
 */
int
spdk_bdev_set_opts(struct spdk_bdev_opts *opts)
{
	uint32_t min_pool_size;
	/* [한국어] thread 수 × cache_size로 계산되는 최소 mempool 크기. */

	if (!opts) {
		SPDK_ERRLOG("opts cannot be NULL\n");
		return -1;
		/* [한국어] NULL 인자 — 호출자 버그. */
	}

	if (!opts->opts_size) {
		SPDK_ERRLOG("opts_size inside opts cannot be zero value\n");
		return -1;
		/* [한국어] opts_size=0 — ABI 추적 불가. */
	}

	/*
	 * Add 1 to the thread count to account for the extra mgmt_ch that gets created during subsystem
	 *  initialization.  A second mgmt_ch will be created on the same thread when the application starts
	 *  but before the deferred put_io_channel event is executed for the first mgmt_ch.
	 */
	min_pool_size = opts->bdev_io_cache_size * (spdk_thread_get_count() + 1);
	/* [한국어] 모든 thread가 캐시를 가득 채울 때 + init 시점 임시 mgmt_ch 1개 추가 마진.
	 *  이보다 작은 풀 크기는 캐시 갈증 시 다른 thread가 즉시 굶을 수 있음. */
	if (opts->bdev_io_pool_size < min_pool_size) {
		SPDK_ERRLOG("bdev_io_pool_size %" PRIu32 " is not compatible with bdev_io_cache_size %" PRIu32
			    " and %" PRIu32 " threads\n", opts->bdev_io_pool_size, opts->bdev_io_cache_size,
			    spdk_thread_get_count());
		SPDK_ERRLOG("bdev_io_pool_size must be at least %" PRIu32 "\n", min_pool_size);
		return -1;
	}

#define SET_FIELD(field) \
        if (offsetof(struct spdk_bdev_opts, field) + sizeof(opts->field) <= opts->opts_size) { \
                g_bdev_opts.field = opts->field; \
        } \

	SET_FIELD(bdev_io_pool_size);
	SET_FIELD(bdev_io_cache_size);
	SET_FIELD(bdev_auto_examine);
	SET_FIELD(iobuf_small_cache_size);
	SET_FIELD(iobuf_large_cache_size);

	g_bdev_opts.opts_size = opts->opts_size;

#undef SET_FIELD

	return 0;
}

/*
 * [한국어]
 * bdev_get_by_name - 글로벌 RB tree에서 이름으로 bdev 검색 (락 미보호 내부 helper).
 *
 * @bdev_name: 찾을 bdev 이름.
 * @return:    찾으면 spdk_bdev*, 없으면 NULL.
 *
 * 호출 전 g_bdev_mgr.spinlock 보유 필요. spdk_bdev_get_by_name이 본 함수를 락 잡고 호출.
 */
static struct spdk_bdev *
bdev_get_by_name(const char *bdev_name)
{
	struct spdk_bdev_name find;
	/* [한국어] RB_FIND용 임시 키 — name 필드만 채우면 됨 (cmp는 name만 봄). */
	struct spdk_bdev_name *res;
	/* [한국어] 검색 결과 노드. */

	find.name = (char *)bdev_name;
	/* [한국어] cast (const → non-const) — RB tree key 비교용. 실제로 수정하지 않음. */
	res = RB_FIND(bdev_name_tree, &g_bdev_mgr.bdev_names, &find);
	/* [한국어] O(log N) RB tree 검색. */
	if (res != NULL) {
		return res->bdev;
		/* [한국어] 찾은 노드의 bdev 포인터 반환. */
	}

	return NULL;
	/* [한국어] 미존재. */
}

/*
 * [한국어]
 * spdk_bdev_get_by_name - 이름으로 bdev 검색 (락 보호 공개 API).
 *
 * @bdev_name: 찾을 bdev 이름.
 * @return:    찾으면 spdk_bdev*, 없으면 NULL.
 *
 * 어느 thread에서든 호출 가능. 반환 후 bdev이 unregister될 수 있으므로 보통
 * spdk_bdev_open_ext로 desc를 얻어 안전한 reference를 확보하는 것이 권장.
 */
struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	struct spdk_bdev *bdev;

	spdk_spin_lock(&g_bdev_mgr.spinlock);
	/* [한국어] 글로벌 락 획득 — RB tree 무결성 보호. */
	bdev = bdev_get_by_name(bdev_name);
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	return bdev;
}

/* [한국어]
 * struct bdev_io_status_string - bdev_io 완료 상태(enum)와 사람이 읽는 문자열을 1:1로 묶는 테이블 행.
 *
 * spdk_bdev_io_status는 음수(에러)부터 양수(성공/대기)까지 분포하는 enum이고,
 * trace/RPC/디버그 로그에서 그 숫자값 대신 "nomem", "nvme_error" 같은 가독 문자열을
 * 출력하기 위한 룩업 테이블의 한 항목이다. 정적 const 배열로만 쓰이며 런타임 변경 없음. */
struct bdev_io_status_string {
	enum spdk_bdev_io_status status;
	/* [한국어] bdev_io 완료 상태 enum 값(키).
	 * 설정자: 컴파일 타임 정적 초기화(아래 배열)만이 설정. 런타임 변경 없음.
	 * 읽는 자: bdev_io_status_get_string()이 입력 status와 == 비교.
	 * 값 범위: SPDK_BDEV_IO_STATUS_* (예: AIO_ERROR=-8 ~ SUCCESS=1). 음수=에러군, 0=실패, 양수=대기/성공.
	 * 동기화: 읽기 전용 정적 데이터 — 락 불필요. */

	const char *str;
	/* [한국어] 해당 status에 대응하는 사람이 읽는 문자열(값).
	 * 설정자: 정적 초기화만. 읽는 자: get_string()이 매칭 시 이 포인터를 그대로 반환.
	 * 값 범위: .rodata 영역의 NUL 종결 리터럴(예: "nomem"). 절대 free 금지.
	 * 동기화: 읽기 전용 — 락 불필요. */
};

/* [한국어] bdev_io 상태 enum → 문자열 매핑 테이블(룩업용 정적 배열).
 * 각 행은 {enum, "이름"} 쌍이며, bdev_io_status_get_string()이 선형 탐색한다.
 * 행 순서는 의미 없음(선형 탐색이므로). 새 상태 추가 시 이 배열에도 한 행을 추가해야
 * 로그/트레이스에서 "reserved" 대신 정확한 이름이 출력된다. */
static const struct bdev_io_status_string bdev_io_status_strings[] = {
	{ SPDK_BDEV_IO_STATUS_AIO_ERROR, "aio_error" },		/* [한국어] libaio 백엔드 errno 보존 에러. */
	{ SPDK_BDEV_IO_STATUS_ABORTED, "aborted" },		/* [한국어] abort 요청에 의해 취소됨. */
	{ SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED, "first_fused_failed" },	/* [한국어] NVMe fused 2-op 중 첫 op 실패. */
	{ SPDK_BDEV_IO_STATUS_MISCOMPARE, "miscompare" },	/* [한국어] compare-and-write 등에서 데이터 불일치. */
	{ SPDK_BDEV_IO_STATUS_NOMEM, "nomem" },			/* [한국어] bdev_io/버퍼 풀 고갈 — 재시도 큐로 이동되는 일시적 실패. */
	{ SPDK_BDEV_IO_STATUS_SCSI_ERROR, "scsi_error" },	/* [한국어] SCSI sense key/ASC/ASCQ 보존 에러. */
	{ SPDK_BDEV_IO_STATUS_NVME_ERROR, "nvme_error" },	/* [한국어] NVMe SC/SCT 상태코드 보존 에러. */
	{ SPDK_BDEV_IO_STATUS_FAILED, "failed" },		/* [한국어] 일반 실패(상태코드 없는 generic error, 값 0). */
	{ SPDK_BDEV_IO_STATUS_PENDING, "pending" },		/* [한국어] 아직 완료되지 않음(제출됨/대기 중). */
	{ SPDK_BDEV_IO_STATUS_SUCCESS, "success" },		/* [한국어] 정상 완료(값 1). */
};

/*
 * [한국어]
 * bdev_io_status_get_string - bdev_io 완료 상태 enum을 사람이 읽는 문자열로 변환.
 *
 * @status: 변환할 spdk_bdev_io_status enum 값.
 * @return: 매칭되는 정적 문자열 포인터. 테이블에 없으면 상수 "reserved".
 *
 * I/O 완료 경로의 trace/디버그 로그에서 상태 숫자값 대신 이름을 찍기 위한 헬퍼.
 * 정적 테이블을 선형 탐색하므로 O(N) (N=10) — hot-path가 아닌 진단 경로에서만 호출되어 무방.
 * 실행 컨텍스트: 어느 스레드에서든 호출 가능(읽기 전용 정적 데이터, 락 불필요).
 * caller: bdev_io 완료 trace 기록 코드 등. callee: 없음(순수 비교 루프).
 * 에러 경로: 매칭 실패는 에러가 아니라 "reserved" 폴백 문자열 반환.
 *
 * 호출 체인:
 *   <trace/로그 기록 코드> → [bdev_io_status_get_string] → (반환 문자열 그대로 출력)
 */
static const char *
bdev_io_status_get_string(enum spdk_bdev_io_status status)
{
	uint32_t i;
	/* [한국어] 테이블 순회 인덱스. */

	for (i = 0; i < SPDK_COUNTOF(bdev_io_status_strings); i++) {
		/* [한국어] SPDK_COUNTOF = 배열 원소 수(컴파일 타임). 전체 행을 선형 스캔. */
		if (bdev_io_status_strings[i].status == status) {
			/* [한국어] enum 키가 일치하면 그 행의 문자열을 즉시 반환(early exit). */
			return bdev_io_status_strings[i].str;
		}
	}

	return "reserved";
	/* [한국어] 테이블에 없는 미등록/예약 상태값 — 폴백 문자열. */
}

/* [한국어]
 * struct spdk_bdev_wait_for_examine_ctx - "모든 bdev examine 완료"를 폴링으로 기다리는 비동기 컨텍스트.
 *
 * SPDK 부팅/bdev 등록 직후, 각 bdev 모듈이 examine_config/examine_disk 콜백으로 디스크를
 * 검사(claim/하위 bdev 생성)하는 작업이 "모두 끝났는지"를 호출자가 알아야 할 때가 있다
 * (예: 모든 디바이스 준비 완료 후 다음 단계 진행). 이 구조체는 그 대기 상태를 담는
 * heap 컨텍스트로, 짧은 주기 poller가 완료 여부를 확인하다 끝나면 cb_fn을 호출하고 자신을 free한다. */
struct spdk_bdev_wait_for_examine_ctx {
	struct spdk_poller              *poller;
	/* [한국어] 완료 여부를 주기적으로 확인하는 poller 핸들.
	 * 설정자: spdk_bdev_wait_for_examine()가 SPDK_POLLER_REGISTER로 등록 후 저장.
	 * 읽는 자: bdev_wait_for_examine_cb()가 완료 시 spdk_poller_unregister(&poller)로 해제.
	 * 값 범위: 유효 poller 포인터(NULL 불가). 등록한 spdk_thread에 바인딩됨.
	 * 동기화: poller는 등록 스레드에서만 실행되므로 단일 스레드 접근 — 락 불필요. */

	spdk_bdev_wait_for_examine_cb	cb_fn;
	/* [한국어] 모든 examine 완료 시 호출할 사용자 콜백.
	 * 설정자: spdk_bdev_wait_for_examine() 진입 시 인자로 받아 저장.
	 * 읽는 자: bdev_wait_for_examine_cb()가 완료 시 cb_fn(cb_arg)로 1회 호출.
	 * 값 범위: 유효 함수 포인터(API 계약상 NULL 불가). 동기화: 등록 스레드 단일 접근. */

	void				*cb_arg;
	/* [한국어] cb_fn에 그대로 전달되는 사용자 컨텍스트(불투명).
	 * 설정자/읽는 자: 위 cb_fn과 동일. SPDK는 내용을 해석하지 않음. */
};

/* [한국어] 모든 bdev 모듈의 진행 중 action(examine 등)이 0인지 검사하는 내부 함수의 전방 선언.
 * 정의는 이 파일 뒤쪽에 있으며, bdev_wait_for_examine_cb가 완료 판정에 사용한다. */
static bool bdev_module_all_actions_completed(void);

/*
 * [한국어]
 * bdev_wait_for_examine_cb - examine 완료를 폴링하는 poller 콜백.
 *
 * @arg: spdk_bdev_wait_for_examine_ctx* (등록 시 전달된 컨텍스트).
 * @return: SPDK_POLLER_IDLE(아직 미완료, 일 안 함) 또는 SPDK_POLLER_BUSY(완료 처리함).
 *
 * SPDK_POLLER_REGISTER(period=0)로 등록되어 reactor가 매 폴 루프마다 호출한다.
 * bdev_module_all_actions_completed()가 true가 될 때까지 IDLE을 반환하며 계속 재진입하고,
 * 완료되면 자기 poller를 해제→사용자 cb_fn 호출→컨텍스트 free 후 BUSY 반환(이번 폴에서 실제 일을 함).
 * 실행 컨텍스트: 등록된 spdk_thread(reactor)에서만 실행 — 단일 스레드, 재진입 없음.
 * caller: SPDK thread 폴 루프. callee: bdev_module_all_actions_completed / spdk_poller_unregister / cb_fn / free.
 * 에러 경로: 없음(완료 여부 폴링만). 완료 후 ctx는 free되어 더 이상 참조 불가.
 *
 * 호출 체인:
 *   reactor 폴 루프 → [bdev_wait_for_examine_cb] → bdev_module_all_actions_completed → (완료 시) cb_fn
 */
static int
bdev_wait_for_examine_cb(void *arg)
{
	struct spdk_bdev_wait_for_examine_ctx *ctx = arg;
	/* [한국어] poller 등록 시 넘긴 컨텍스트 복원(void* → 구조체 포인터). */

	if (!bdev_module_all_actions_completed()) {
		/* [한국어] 아직 검사 중인 모듈이 있음 — 이번 폴에선 아무것도 안 하고 다음 폴을 기다림. */
		return SPDK_POLLER_IDLE;
	}

	spdk_poller_unregister(&ctx->poller);
	/* [한국어] 완료됨 → 더는 폴링 불필요. poller 자신을 해제(이중 해제 방지 위해 핸들 NULL화). */
	ctx->cb_fn(ctx->cb_arg);
	/* [한국어] 사용자에게 "모든 examine 완료" 통지(이 poller 스레드 컨텍스트에서 동기 호출). */
	free(ctx);
	/* [한국어] 일회성 컨텍스트 해제 — 이후 ctx 접근 금지. */

	return SPDK_POLLER_BUSY;
	/* [한국어] 이번 폴에서 실제 작업을 수행했음을 reactor에 알림(폴링 통계용). */
}

/*
 * [한국어]
 * spdk_bdev_wait_for_examine - 모든 bdev examine이 끝나면 cb_fn을 호출하도록 비동기 대기 등록.
 *
 * @cb_fn: 모든 examine 완료 시 1회 호출될 사용자 콜백.
 * @cb_arg: cb_fn에 전달될 불투명 컨텍스트.
 * @return: 0=등록 성공, -ENOMEM=컨텍스트 할당 실패.
 *
 * bdev 등록/RPC replay 직후, 모든 모듈의 디스크 검사가 끝날 때까지 비동기로 기다리는 공개 API.
 * 동기 블로킹 대신 짧은 주기 poller를 등록해 두고 즉시 반환하며(SPDK polled-mode 모델),
 * 완료는 cb_fn으로 통지된다. 호출자는 보통 부팅 시퀀스/RPC 핸들러에서 사용.
 * 실행 컨텍스트: 호출한 spdk_thread에 poller가 바인딩되므로, 그 스레드가 계속 폴링되어야 cb_fn이 불린다.
 * caller: 부트스트랩/RPC 핸들러. callee: calloc / SPDK_POLLER_REGISTER.
 * 에러 경로: calloc 실패 시 -ENOMEM 즉시 반환(poller 미등록).
 *
 * 호출 체인:
 *   <부트/RPC 코드> → [spdk_bdev_wait_for_examine] → SPDK_POLLER_REGISTER → (이후) bdev_wait_for_examine_cb
 */
int
spdk_bdev_wait_for_examine(spdk_bdev_wait_for_examine_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev_wait_for_examine_ctx *ctx;
	/* [한국어] 대기 상태를 담을 heap 컨텍스트. */

	ctx = calloc(1, sizeof(*ctx));
	/* [한국어] 0으로 초기화된 컨텍스트 할당(poller 필드 등 NULL 보장). */
	if (ctx == NULL) {
		return -ENOMEM;
		/* [한국어] 메모리 부족 — poller를 등록하지 않고 실패 반환. */
	}
	ctx->cb_fn = cb_fn;
	/* [한국어] 완료 콜백 저장. */
	ctx->cb_arg = cb_arg;
	/* [한국어] 콜백 인자 저장. */
	ctx->poller = SPDK_POLLER_REGISTER(bdev_wait_for_examine_cb, ctx, 0);
	/* [한국어] period=0 poller 등록 → reactor 매 폴 루프마다 즉시 호출(busy-poll). ctx가 arg로 전달됨. */

	return 0;
	/* [한국어] 등록 성공 — 완료는 비동기로 cb_fn을 통해 통지됨. */
}

/* [한국어]
 * struct spdk_bdev_examine_item - "수동 examine 허용 목록(allowlist)"에 등록된 bdev 이름 한 건.
 *
 * auto_examine가 꺼져 있을 때, 사용자가 RPC(bdev_examine)로 명시적으로 검사를 허용한
 * bdev 이름/별칭을 보관하는 리스트 노드다. bdev 등록 시 이 목록에 이름이 있어야만
 * examine 콜백이 실행된다(화이트리스트 게이팅). */
struct spdk_bdev_examine_item {
	char *name;
	/* [한국어] 검사를 허용한 bdev 이름 또는 별칭(strdup로 복제 소유).
	 * 설정자: spdk_bdev_examine()가 strdup로 채움. 읽는 자: allowlist_check가 strcmp 비교.
	 * 값 범위: NUL 종결 문자열(NULL 불가). free 책임은 이 구조체(remove/free 시 함께 해제).
	 * 동기화: app 스레드에서만 조작(spdk_bdev_examine이 app-thread 강제) — 락 불필요. */

	TAILQ_ENTRY(spdk_bdev_examine_item) link;
	/* [한국어] g_bdev_examine_allowlist TAILQ 연결 고리(prev/next 포인터).
	 * 설정자/읽는 자: TAILQ_INSERT_TAIL/REMOVE/FOREACH 매크로가 사용. 동기화: app 스레드 단일 접근. */
};

/* [한국어] spdk_bdev_examine_item을 담는 TAILQ(이중 연결 리스트) 헤드 타입 정의. */
TAILQ_HEAD(spdk_bdev_examine_allowlist, spdk_bdev_examine_item);

/* [한국어] 전역 수동 examine 허용 목록 인스턴스(빈 리스트로 초기화).
 * 설정자: spdk_bdev_examine()가 항목 추가, remove/free가 제거. 읽는 자: allowlist_check 계열.
 * 동기화: app 스레드에서만 변경되므로 별도 락 없음(spdk_bdev_examine의 app-thread 강제에 의존). */
struct spdk_bdev_examine_allowlist g_bdev_examine_allowlist = TAILQ_HEAD_INITIALIZER(
			g_bdev_examine_allowlist);

/*
 * [한국어]
 * bdev_examine_allowlist_check - 주어진 이름이 수동 examine 허용 목록에 있는지 검사.
 *
 * @name: 찾을 bdev 이름 또는 별칭.
 * @return: 목록에 있으면 true, 없으면 false.
 *
 * auto_examine가 꺼진 환경에서 특정 bdev을 검사해도 되는지 판단하는 기본 헬퍼.
 * 선형 탐색 O(N) — allowlist는 보통 작아 무방. 실행 컨텍스트: app 스레드(락 없음 전제).
 * caller: bdev_in_examine_allowlist / spdk_bdev_examine. callee: strcmp.
 *
 * 호출 체인:
 *   bdev_in_examine_allowlist → [bdev_examine_allowlist_check] → strcmp
 */
static inline bool
bdev_examine_allowlist_check(const char *name)
{
	struct spdk_bdev_examine_item *item;
	/* [한국어] 순회용 노드 포인터. */
	TAILQ_FOREACH(item, &g_bdev_examine_allowlist, link) {
		/* [한국어] 허용 목록 전체를 순회. */
		if (strcmp(name, item->name) == 0) {
			/* [한국어] 이름 정확 일치 → 허용됨. */
			return true;
		}
	}
	return false;
	/* [한국어] 끝까지 못 찾음 → 허용 안 됨. */
}

/*
 * [한국어]
 * bdev_examine_allowlist_remove - 허용 목록에서 해당 이름의 항목 1건을 제거·해제.
 *
 * @name: 제거할 bdev 이름.
 * @return: 없음(이름이 없으면 조용히 무시).
 *
 * 보통 bdev 검사가 끝나 더 이상 allowlist에 둘 필요가 없을 때 호출. 첫 일치 항목만 제거 후 break.
 * 실행 컨텍스트: app 스레드(락 없음). callee: TAILQ_REMOVE / free.
 *
 * 호출 체인:
 *   <examine 완료 처리> → [bdev_examine_allowlist_remove] → TAILQ_REMOVE → free
 */
static inline void
bdev_examine_allowlist_remove(const char *name)
{
	struct spdk_bdev_examine_item *item;
	/* [한국어] 순회용 노드 포인터. */
	TAILQ_FOREACH(item, &g_bdev_examine_allowlist, link) {
		/* [한국어] 목록 순회하며 이름 매칭 탐색. */
		if (strcmp(name, item->name) == 0) {
			/* [한국어] 일치 항목 발견. */
			TAILQ_REMOVE(&g_bdev_examine_allowlist, item, link);
			/* [한국어] 리스트에서 노드 분리. */
			free(item->name);
			/* [한국어] strdup로 복제했던 이름 문자열 해제. */
			free(item);
			/* [한국어] 노드 자체 해제. */
			break;
			/* [한국어] 첫 일치만 처리하고 종료(중복 이름은 가정하지 않음). */
		}
	}
}

/*
 * [한국어]
 * bdev_examine_allowlist_free - 허용 목록 전체를 비우고 모든 항목을 해제.
 *
 * @return: 없음.
 *
 * bdev 서브시스템 종료(finish) 시 누수 방지를 위해 allowlist를 통째로 정리한다.
 * 실행 컨텍스트: app 스레드(종료 경로). callee: TAILQ_FIRST/REMOVE / free.
 *
 * 호출 체인:
 *   <bdev finish> → [bdev_examine_allowlist_free] → 반복 free
 */
static inline void
bdev_examine_allowlist_free(void)
{
	struct spdk_bdev_examine_item *item;
	/* [한국어] 매 반복에서 떼어낼 선두 노드. */
	while (!TAILQ_EMPTY(&g_bdev_examine_allowlist)) {
		/* [한국어] 목록이 빌 때까지 반복. */
		item = TAILQ_FIRST(&g_bdev_examine_allowlist);
		/* [한국어] 맨 앞 노드 선택. */
		TAILQ_REMOVE(&g_bdev_examine_allowlist, item, link);
		/* [한국어] 리스트에서 분리. */
		free(item->name);
		/* [한국어] 이름 문자열 해제. */
		free(item);
		/* [한국어] 노드 해제. */
	}
}

/*
 * [한국어]
 * bdev_in_examine_allowlist - 주어진 bdev이 (이름 또는 별칭으로) 허용 목록에 포함되는지 검사.
 *
 * @bdev: 검사 대상 bdev.
 * @return: 이름 또는 별칭 중 하나라도 allowlist에 있으면 true.
 *
 * bdev은 본명(name) 외에 여러 별칭(alias)을 가질 수 있어, 사용자가 별칭으로 allowlist에
 * 등록했을 수도 있으므로 별칭까지 모두 확인한다. 실행 컨텍스트: app 스레드(락 없음).
 * caller: bdev_ok_to_examine. callee: bdev_examine_allowlist_check.
 *
 * 호출 체인:
 *   bdev_ok_to_examine → [bdev_in_examine_allowlist] → bdev_examine_allowlist_check
 */
static inline bool
bdev_in_examine_allowlist(struct spdk_bdev *bdev)
{
	struct spdk_bdev_alias *tmp;
	/* [한국어] bdev의 별칭 리스트 순회용 노드. */
	if (bdev_examine_allowlist_check(bdev->name)) {
		/* [한국어] 먼저 본명으로 검사 — 일치하면 즉시 허용. */
		return true;
	}
	TAILQ_FOREACH(tmp, &bdev->aliases, tailq) {
		/* [한국어] 본명 불일치 시 모든 별칭을 순회. */
		if (bdev_examine_allowlist_check(tmp->alias.name)) {
			/* [한국어] 별칭 중 하나가 허용 목록에 있으면 허용. */
			return true;
		}
	}
	return false;
	/* [한국어] 본명·별칭 모두 불일치 → 허용 안 됨. */
}

/*
 * [한국어]
 * bdev_ok_to_examine - 이 bdev에 대해 examine(모듈 디스크 검사)을 수행해도 되는지 최종 판정.
 *
 * @bdev: 검사 후보 bdev.
 * @return: 검사 허용이면 true, 아니면 false.
 *
 * 검사는 보통 디스크에 READ를 발행해 메타데이터(파티션 테이블, lvol 슈퍼블록 등)를 읽으므로,
 * READ를 지원하지 않는 bdev은 애초에 검사 대상에서 제외한다. 그 다음 정책:
 *  - auto_examine 켜짐: 모든(READ 지원) bdev 검사 허용.
 *  - auto_examine 꺼짐: 허용 목록에 명시된 bdev만 검사.
 * 실행 컨텍스트: app 스레드(락 없음). caller: bdev_examine. callee: io_type_supported / allowlist 검사.
 *
 * 호출 체인:
 *   bdev_examine → [bdev_ok_to_examine] → spdk_bdev_io_type_supported / bdev_in_examine_allowlist
 */
static inline bool
bdev_ok_to_examine(struct spdk_bdev *bdev)
{
	/* Some bdevs may not support the READ command.
	 * Do not try to examine them.
	 */
	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_READ)) {
		/* [한국어] READ 미지원 bdev은 메타데이터를 읽을 수 없으므로 검사 자체를 건너뜀. */
		return false;
	}

	if (g_bdev_opts.bdev_auto_examine) {
		/* [한국어] 자동 검사 모드: READ만 지원하면 무조건 허용. */
		return true;
	} else {
		/* [한국어] 수동 모드: 허용 목록에 명시된 bdev(이름/별칭)만 검사. */
		return bdev_in_examine_allowlist(bdev);
	}
}

/*
 * [한국어]
 * bdev_examine - 새로 등록된 bdev에 대해 모든 bdev 모듈의 examine 콜백을 순서대로 실행.
 *
 * @bdev: 방금 spdk_bdev_register로 등록되어 검사가 필요한 bdev.
 * @return: 없음 (examine은 비동기적으로 진행될 수 있으며 모듈이
 *          spdk_bdev_module_examine_done()으로 완료를 통지).
 *
 * SPDK에서 새 bdev이 나타나면 "이 디바이스 위에 내가 올라탈 vbdev를 만들 수 있는가?"를
 * 모든 모듈에게 물어보는 단계가 examine이다. 두 단계로 진행된다:
 *  1) examine_config: 디스크 I/O 없이 설정(config)만으로 판단하는 가벼운 검사.
 *     예) lvol 모듈이 "이 bdev은 내 lvolstore의 base다"라고 미리 안 경우.
 *  2) examine_disk: 실제 디스크에서 READ로 메타데이터(슈퍼블록/파티션테이블)를 읽어
 *     판단하는 무거운 검사. claim 상태에 따라 누가 검사할지가 달라진다:
 *       - CLAIM_NONE: 아무도 소유하지 않음 → 모든 모듈이 검사.
 *       - EXCL_WRITE(v1): 단독 소유한 모듈 1개만 검사.
 *       - v2 claim: v2 claim을 가진 모든 모듈이 검사.
 * action_in_progress 카운터는 examine_config가 비동기 완료(examine_done) 호출을
 * 빠뜨리지 않았는지 검증하는 디버깅 장치다.
 * 실행 컨텍스트: app 스레드. bdev->internal.spinlock과 module->internal.spinlock을
 * 잡았다 놓았다 하며 콜백 중에는 락을 풀어 재진입(콜백 안에서 claim 변경)을 허용한다.
 * caller: bdev_start(등록 직후) / spdk_bdev_examine(수동). callee: 각 모듈 콜백.
 *
 * 호출 체인:
 *   bdev_start → [bdev_examine] → module->examine_config / module->examine_disk
 */
static void
bdev_examine(struct spdk_bdev *bdev)
{
	struct spdk_bdev_module *module;
	/* [한국어] g_bdev_mgr.bdev_modules / claims 리스트를 순회할 모듈 포인터. */
	struct spdk_bdev_module_claim *claim, *tmpclaim;
	/* [한국어] v2 claim 리스트 순회용 노드(claim)와 안전 삭제용 임시(tmpclaim). */
	uint32_t action;
	/* [한국어] examine_config 호출 전 action_in_progress 스냅샷 — 콜백이
	 *  examine_done을 호출했는지(카운터가 다시 줄었는지) 검증하기 위함. */

	if (!bdev_ok_to_examine(bdev)) {
		/* [한국어] READ 미지원이거나 수동 모드에서 allowlist 미포함이면 검사 생략. */
		return;
	}

	TAILQ_FOREACH(module, &g_bdev_mgr.bdev_modules, internal.tailq) {
		/* [한국어] 1단계: 등록된 모든 모듈에 대해 examine_config(가벼운 검사) 호출. */
		if (module->examine_config) {
			/* [한국어] examine_config 콜백을 구현한 모듈만 대상. */
			spdk_spin_lock(&module->internal.spinlock);
			/* [한국어] 모듈별 카운터를 원자적으로 다루기 위해 모듈 락 획득. */
			action = module->internal.action_in_progress;
			/* [한국어] 콜백 호출 직전 카운터 값을 스냅샷. */
			module->internal.action_in_progress++;
			/* [한국어] "검사 진행 중" 1건 추가 — 콜백이 examine_done으로 감소시켜야 함. */
			spdk_spin_unlock(&module->internal.spinlock);
			/* [한국어] 콜백은 락 밖에서 호출(콜백 내부에서 다시 락 잡을 수 있으므로). */
			module->examine_config(bdev);
			/* [한국어] 모듈에 "이 bdev 설정을 검사하라" 통지 — 동기/비동기 모두 가능. */
			if (action != module->internal.action_in_progress) {
				/* [한국어] 카운터가 스냅샷과 다르면 examine_done 미호출(버그) → 경고. */
				SPDK_ERRLOG("examine_config for module %s did not call "
					    "spdk_bdev_module_examine_done()\n", module->name);
			}
		}
	}

	spdk_spin_lock(&bdev->internal.spinlock);
	/* [한국어] claim_type을 안전하게 읽고 examine_in_progress를 갱신하기 위해 bdev 락 획득. */

	switch (bdev->internal.claim_type) {
	case SPDK_BDEV_CLAIM_NONE:
		/* Examine by all bdev modules */
		/* [한국어] 아무도 소유하지 않은 bdev — 모든 모듈이 디스크 검사 후보. */
		TAILQ_FOREACH(module, &g_bdev_mgr.bdev_modules, internal.tailq) {
			/* [한국어] 등록된 모든 모듈 순회. */
			if (module->examine_disk) {
				/* [한국어] examine_disk를 구현한 모듈만 대상. */
				spdk_spin_lock(&module->internal.spinlock);
				/* [한국어] 모듈 카운터 보호. */
				module->internal.action_in_progress++;
				/* [한국어] 디스크 검사 진행 1건 추가. */
				spdk_spin_unlock(&module->internal.spinlock);
				spdk_spin_unlock(&bdev->internal.spinlock);
				/* [한국어] 콜백 중에는 bdev 락도 풀어 콜백이 claim 등을 변경할 수 있게 함. */
				module->examine_disk(bdev);
				/* [한국어] 모듈이 실제 디스크 READ로 메타데이터를 검사 (비동기 가능). */
				spdk_spin_lock(&bdev->internal.spinlock);
				/* [한국어] 다음 순회 전 bdev 락 재획득. */
			}
		}
		break;
	case SPDK_BDEV_CLAIM_EXCL_WRITE:
		/* Examine by the one bdev module with a v1 claim */
		/* [한국어] v1 단독-쓰기 claim — 소유 모듈 1개만 디스크 검사. */
		module = bdev->internal.claim.v1.module;
		/* [한국어] v1 claim을 보유한 유일 모듈을 꺼냄. */
		if (module->examine_disk) {
			/* [한국어] 그 모듈이 examine_disk를 구현했으면 호출. */
			spdk_spin_lock(&module->internal.spinlock);
			module->internal.action_in_progress++;
			/* [한국어] 검사 진행 카운트 증가. */
			spdk_spin_unlock(&module->internal.spinlock);
			spdk_spin_unlock(&bdev->internal.spinlock);
			/* [한국어] 콜백 전 bdev 락 해제. */
			module->examine_disk(bdev);
			/* [한국어] 단일 소유 모듈에게만 디스크 검사 위임. */
			return;
			/* [한국어] v1은 한 모듈뿐이므로 콜백 후 곧장 종료(락은 이미 풀린 상태). */
		}
		break;
	default:
		/* Examine by all bdev modules with a v2 claim */
		/* [한국어] v2 claim(공유 또는 단독-읽기 등) — claim 리스트의 모든 모듈이 검사. */
		assert(claim_type_is_v2(bdev->internal.claim_type));
		/* [한국어] default에 도달했으면 반드시 v2 계열 claim임을 단언. */
		/*
		 * Removal of tailq nodes while iterating can cause the iteration to jump out of the
		 * list, perhaps accessing freed memory. Without protection, this could happen
		 * while the lock is dropped during the examine callback.
		 */
		bdev->internal.examine_in_progress++;
		/* [한국어] 검사 중 claim 리스트 노드가 콜백에서 삭제되어 순회가 깨지는 것을
		 *  막기 위한 가드 카운터 — 0이 될 때까지 실제 노드 free를 미룬다. */

		TAILQ_FOREACH(claim, &bdev->internal.claim.v2.claims, link) {
			/* [한국어] 이 bdev에 걸린 모든 v2 claim 순회. */
			module = claim->module;
			/* [한국어] claim을 보유한 모듈. */

			if (module == NULL) {
				/* This is a vestigial claim, held by examine_count */
				/* [한국어] 모듈 없는 흔적(vestigial) claim — examine_count가 잡고 있는 것 → 건너뜀. */
				continue;
			}

			if (module->examine_disk == NULL) {
				/* [한국어] examine_disk 미구현 모듈은 검사 불가 → 건너뜀. */
				continue;
			}

			spdk_spin_lock(&module->internal.spinlock);
			module->internal.action_in_progress++;
			/* [한국어] 검사 진행 카운트 증가. */
			spdk_spin_unlock(&module->internal.spinlock);

			/* Call examine_disk without holding internal.spinlock. */
			/* [한국어] 콜백 중 재진입 허용 위해 bdev 락 해제 후 호출. */
			spdk_spin_unlock(&bdev->internal.spinlock);
			module->examine_disk(bdev);
			/* [한국어] 해당 모듈에게 디스크 검사 위임. */
			spdk_spin_lock(&bdev->internal.spinlock);
			/* [한국어] 다음 순회 전 락 재획득. */
		}

		assert(bdev->internal.examine_in_progress > 0);
		/* [한국어] 위에서 증가시켰으므로 반드시 0보다 커야 함. */
		bdev->internal.examine_in_progress--;
		/* [한국어] 이 검사 라운드 종료 — 가드 카운터 감소. */
		if (bdev->internal.examine_in_progress == 0) {
			/* [한국어] 중첩된 검사가 모두 끝났을 때만 지연 정리를 수행. */
			/* Remove any claims that were released during examine_disk */
			TAILQ_FOREACH_SAFE(claim, &bdev->internal.claim.v2.claims, link, tmpclaim) {
				/* [한국어] 검사 도중 release된(desc가 NULL) claim 노드를 안전하게 정리. */
				if (claim->desc != NULL) {
					/* [한국어] 아직 살아있는 claim은 보존. */
					continue;
				}

				TAILQ_REMOVE(&bdev->internal.claim.v2.claims, claim, link);
				/* [한국어] desc가 끊긴 claim 노드를 리스트에서 제거. */
				free(claim);
				/* [한국어] claim 노드 메모리 해제. */
			}
			if (TAILQ_EMPTY(&bdev->internal.claim.v2.claims)) {
				/* [한국어] claim이 하나도 안 남았으면 bdev의 claim 상태를 NONE으로 리셋. */
				claim_reset(bdev);
			}
		}
	}

	spdk_spin_unlock(&bdev->internal.spinlock);
	/* [한국어] 모든 검사 디스패치 완료 — bdev 락 해제. */
}

/*
 * [한국어]
 * spdk_bdev_examine - 수동(manual) examine 모드에서 특정 이름의 bdev을 검사 대상으로 등록.
 *
 * @name: 검사를 허용할 bdev 이름(또는 별칭).
 * @return: 0 성공, -EINVAL(앱 스레드 아님 / auto_examine 켜짐), -EEXIST(중복), -ENOMEM.
 *
 * auto_examine을 끈 환경에서는 어떤 bdev을 examine할지 사용자가 명시적으로 지정해야 한다.
 * 이 함수는 그 allowlist에 항목을 추가하고, 만약 해당 bdev이 이미 등록되어 있으면
 * 곧바로 bdev_examine()을 호출해 검사를 트리거한다. (아직 없으면 추후 register 시점에
 * allowlist를 참조해 검사된다.) JSON-RPC "bdev_examine" 핸들러가 이 함수를 호출한다.
 * 실행 컨텍스트: 반드시 app 스레드(전역 g_bdev_examine_allowlist 변경이 락 없이 안전한 곳).
 * caller: RPC bdev_examine. callee: bdev_examine_allowlist_check / bdev_examine.
 *
 * 호출 체인:
 *   RPC → [spdk_bdev_examine] → bdev_examine
 */
int
spdk_bdev_examine(const char *name)
{
	struct spdk_bdev *bdev;
	/* [한국어] 이름으로 조회한 기존 bdev (있으면 즉시 검사). */
	struct spdk_bdev_examine_item *item;
	/* [한국어] allowlist에 새로 추가할 항목(이름 복사본 보유). */
	struct spdk_thread *thread = spdk_get_thread();
	/* [한국어] 현재 실행 스레드 — app 스레드 검증용. */

	if (spdk_unlikely(!spdk_thread_is_app_thread(thread))) {
		/* [한국어] 전역 allowlist는 app 스레드에서만 변경해야 lockless 안전 → 아니면 거부. */
		SPDK_ERRLOG("Cannot examine bdev %s on thread %p (%s)\n", name, thread,
			    thread ? spdk_thread_get_name(thread) : "null");
		return -EINVAL;
	}

	if (g_bdev_opts.bdev_auto_examine) {
		/* [한국어] 자동 검사가 켜진 상태에서 수동 등록은 정책상 금지. */
		SPDK_ERRLOG("Manual examine is not allowed if auto examine is enabled\n");
		return -EINVAL;
	}

	if (bdev_examine_allowlist_check(name)) {
		/* [한국어] 이미 동일 이름이 allowlist에 있으면 중복 등록 거부. */
		SPDK_ERRLOG("Duplicate bdev name for manual examine: %s\n", name);
		return -EEXIST;
	}

	item = calloc(1, sizeof(*item));
	/* [한국어] allowlist 항목 할당(0-초기화). */
	if (!item) {
		/* [한국어] 할당 실패 → 메모리 부족. */
		return -ENOMEM;
	}
	item->name = strdup(name);
	/* [한국어] 호출자 문자열 수명에 의존하지 않도록 이름을 복사 보관. */
	if (!item->name) {
		/* [한국어] 문자열 복사 실패 → 방금 할당한 item 해제 후 반환. */
		free(item);
		return -ENOMEM;
	}
	TAILQ_INSERT_TAIL(&g_bdev_examine_allowlist, item, link);
	/* [한국어] allowlist 끝에 항목 추가. */

	bdev = spdk_bdev_get_by_name(name);
	/* [한국어] 해당 이름의 bdev이 이미 등록돼 있는지 조회. */
	if (bdev) {
		/* [한국어] 이미 존재하면 지금 즉시 검사 트리거(추후 register를 기다리지 않음). */
		bdev_examine(bdev);
	}
	return 0;
	/* [한국어] allowlist 등록 성공. */
}

/*
 * [한국어]
 * bdev_examine_allowlist_config_json - manual examine allowlist를 JSON config로 직렬화.
 *
 * @w: spdk_json_write_ctx — 직렬화 대상 출력 스트림.
 * @return: 없음.
 *
 * SPDK는 현재 런타임 구성을 JSON으로 dump해 재현(replay)할 수 있게 한다. 이 함수는
 * allowlist의 각 항목을 {"method":"bdev_examine","params":{"name":...}} RPC 호출 형태로
 * 기록해, 설정 재로딩 시 동일한 수동 검사 등록이 재현되게 한다.
 * 실행 컨텍스트: app 스레드(config dump 경로). caller: bdev subsystem config_json.
 *
 * 호출 체인:
 *   subsystem config_json → [bdev_examine_allowlist_config_json] → spdk_json_write_*
 */
static inline void
bdev_examine_allowlist_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_bdev_examine_item *item;
	/* [한국어] allowlist 순회용 항목 포인터. */
	TAILQ_FOREACH(item, &g_bdev_examine_allowlist, link) {
		/* [한국어] 각 항목을 하나의 RPC 호출 JSON 객체로 출력. */
		spdk_json_write_object_begin(w);
		/* [한국어] '{' — RPC 호출 객체 시작. */
		spdk_json_write_named_string(w, "method", "bdev_examine");
		/* [한국어] "method":"bdev_examine" — 재현 시 호출할 RPC 이름. */
		spdk_json_write_named_object_begin(w, "params");
		/* [한국어] "params":{ — 인자 객체 시작. */
		spdk_json_write_named_string(w, "name", item->name);
		/* [한국어] "name":<bdev 이름> — 검사 대상 지정. */
		spdk_json_write_object_end(w);
		/* [한국어] params 객체 닫기 '}'. */
		spdk_json_write_object_end(w);
		/* [한국어] RPC 호출 객체 닫기 '}'. */
	}
}

/*
 * [한국어]
 * spdk_bdev_first - 전역 bdev 목록의 첫 번째 bdev 반환 (전체 순회 시작).
 *
 * @return: 첫 bdev 포인터, 목록이 비었으면 NULL.
 *
 * 등록된 모든 bdev을 순회하려는 사용자가 spdk_bdev_first/next 쌍으로 이터레이션할 때의
 * 시작점. claim 여부와 무관하게 모든 bdev을 본다.
 * 실행 컨텍스트: app 스레드(전역 목록 안정 가정). caller: 사용자/RPC 목록 출력.
 *
 * 호출 체인:
 *   사용자 → [spdk_bdev_first] → TAILQ_FIRST
 */
struct spdk_bdev *
spdk_bdev_first(void)
{
	struct spdk_bdev *bdev;
	/* [한국어] 반환할 첫 bdev. */

	bdev = TAILQ_FIRST(&g_bdev_mgr.bdevs);
	/* [한국어] 전역 bdev 목록의 선두 노드 획득. */
	if (bdev) {
		/* [한국어] 디버그 빌드에서 순회 시작을 로깅. */
		SPDK_DEBUGLOG(bdev, "Starting bdev iteration at %s\n", bdev->name);
	}

	return bdev;
	/* [한국어] 첫 bdev(또는 빈 목록이면 NULL) 반환. */
}

/*
 * [한국어]
 * spdk_bdev_next - 주어진 bdev 다음의 bdev 반환 (전체 순회 진행).
 *
 * @prev: 직전에 반환받은 bdev.
 * @return: 다음 bdev, 끝이면 NULL.
 *
 * spdk_bdev_first로 시작한 순회를 한 칸 전진시킨다.
 * 실행 컨텍스트: app 스레드. caller: 사용자/RPC. callee: TAILQ_NEXT.
 *
 * 호출 체인:
 *   사용자 → [spdk_bdev_next] → TAILQ_NEXT
 */
struct spdk_bdev *
spdk_bdev_next(struct spdk_bdev *prev)
{
	struct spdk_bdev *bdev;
	/* [한국어] 반환할 다음 bdev. */

	bdev = TAILQ_NEXT(prev, internal.link);
	/* [한국어] prev 노드의 다음 노드 획득(internal.link 연결고리 사용). */
	if (bdev) {
		/* [한국어] 디버그 빌드에서 순회 진행 로깅. */
		SPDK_DEBUGLOG(bdev, "Continuing bdev iteration at %s\n", bdev->name);
	}

	return bdev;
	/* [한국어] 다음 bdev(또는 끝이면 NULL) 반환. */
}

/*
 * [한국어]
 * _bdev_next_leaf - 주어진 위치부터 "leaf"(아무도 claim하지 않은) bdev을 앞으로 탐색.
 *
 * @bdev: 탐색 시작 bdev(이 노드 포함).
 * @return: claim_type이 NONE인 첫 bdev, 없으면 NULL.
 *
 * "leaf" bdev은 그 위에 올라탄 vbdev가 없는, 즉 사용자에게 직접 노출되는 최상위
 * 디바이스다. 파티션/RAID/lvol 등이 base로 claim한 bdev은 leaf가 아니므로 건너뛴다.
 * 실행 컨텍스트: app 스레드. caller: spdk_bdev_first/next_leaf. callee: TAILQ_NEXT.
 *
 * 호출 체인:
 *   spdk_bdev_first_leaf/next_leaf → [_bdev_next_leaf] → TAILQ_NEXT
 */
static struct spdk_bdev *
_bdev_next_leaf(struct spdk_bdev *bdev)
{
	while (bdev != NULL) {
		/* [한국어] 목록 끝(NULL)에 도달할 때까지 전진. */
		if (bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE) {
			/* [한국어] 아무도 소유하지 않은(=leaf) bdev을 찾으면 즉시 반환. */
			return bdev;
		} else {
			/* [한국어] claim된 bdev(다른 vbdev의 base)은 leaf가 아님 → 다음으로. */
			bdev = TAILQ_NEXT(bdev, internal.link);
		}
	}

	return bdev;
	/* [한국어] leaf를 못 찾고 목록 끝에 도달 → NULL 반환. */
}

/*
 * [한국어]
 * spdk_bdev_first_leaf - leaf bdev만 순회하는 이터레이션의 시작점.
 *
 * @return: 첫 leaf bdev, 없으면 NULL.
 *
 * 사용자에게 직접 노출 가능한 최상위 디바이스만 나열할 때 사용한다(예: lsblk 류).
 * 실행 컨텍스트: app 스레드. callee: _bdev_next_leaf.
 *
 * 호출 체인:
 *   사용자 → [spdk_bdev_first_leaf] → _bdev_next_leaf
 */
struct spdk_bdev *
spdk_bdev_first_leaf(void)
{
	struct spdk_bdev *bdev;
	/* [한국어] 반환할 첫 leaf bdev. */

	bdev = _bdev_next_leaf(TAILQ_FIRST(&g_bdev_mgr.bdevs));
	/* [한국어] 목록 선두부터 시작해 첫 leaf까지 전진. */

	if (bdev) {
		/* [한국어] 디버그 로깅. */
		SPDK_DEBUGLOG(bdev, "Starting bdev iteration at %s\n", bdev->name);
	}

	return bdev;
	/* [한국어] 첫 leaf 반환. */
}

/*
 * [한국어]
 * spdk_bdev_next_leaf - leaf 순회를 한 칸 전진.
 *
 * @prev: 직전에 반환받은 leaf bdev.
 * @return: 다음 leaf bdev, 끝이면 NULL.
 *
 * 실행 컨텍스트: app 스레드. callee: _bdev_next_leaf.
 *
 * 호출 체인:
 *   사용자 → [spdk_bdev_next_leaf] → _bdev_next_leaf
 */
struct spdk_bdev *
spdk_bdev_next_leaf(struct spdk_bdev *prev)
{
	struct spdk_bdev *bdev;
	/* [한국어] 반환할 다음 leaf bdev. */

	bdev = _bdev_next_leaf(TAILQ_NEXT(prev, internal.link));
	/* [한국어] prev 다음 노드부터 시작해 다음 leaf까지 전진. */

	if (bdev) {
		/* [한국어] 디버그 로깅. */
		SPDK_DEBUGLOG(bdev, "Continuing bdev iteration at %s\n", bdev->name);
	}

	return bdev;
	/* [한국어] 다음 leaf 반환. */
}

/*
 * [한국어]
 * bdev_io_use_memory_domain - 이 bdev_io가 memory domain(외부 메모리 주소공간)을 쓰는지 판정.
 *
 * @bdev_io: 검사 대상 I/O.
 * @return: memory domain이 설정돼 있으면 true.
 *
 * NVMe-oF RDMA 등에서 데이터 버퍼가 로컬 가상주소가 아니라 원격/디바이스 메모리 도메인에
 * 있을 수 있다. 이 경우 DMA pull/push가 필요하므로 빠른 비트 플래그로 분기한다.
 * 실행 컨텍스트: I/O 발행/완료 hot-path. caller: submit/완료 경로 다수.
 *
 * 호출 체인:
 *   I/O 경로 → [bdev_io_use_memory_domain] → (f.has_memory_domain 비트 읽기)
 */
static inline bool
bdev_io_use_memory_domain(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->internal.f.has_memory_domain;
	/* [한국어] internal.f 비트필드의 has_memory_domain 플래그를 그대로 반환. */
}

/*
 * [한국어]
 * bdev_io_use_accel_sequence - 이 bdev_io에 accel(crypto/compress/copy) 시퀀스가 붙어있는지 판정.
 *
 * @bdev_io: 검사 대상 I/O.
 * @return: accel sequence가 설정돼 있으면 true.
 *
 * DPDK accel framework로 오프로드할 변환 작업(암호화/압축/복사 체인)이 I/O에 연결돼 있으면
 * submit/완료 시점에 sequence 실행을 끼워넣어야 하므로 분기에 사용한다.
 * 실행 컨텍스트: I/O hot-path.
 *
 * 호출 체인:
 *   I/O 경로 → [bdev_io_use_accel_sequence] → (f.has_accel_sequence 비트 읽기)
 */
static inline bool
bdev_io_use_accel_sequence(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->internal.f.has_accel_sequence;
	/* [한국어] internal.f 비트필드의 has_accel_sequence 플래그를 그대로 반환. */
}

/*
 * [한국어]
 * bdev_desc_get_block_size - descriptor 관점의 논리 블록 크기 계산(메타데이터 숨김 반영).
 *
 * @desc: 사용자 디스크립터.
 * @return: 블록당 바이트 수. hide_metadata면 메타데이터 길이를 뺀 값.
 *
 * 한 블록은 데이터 + (선택적) 메타데이터로 구성된다. 사용자가 open 시 hide_metadata를
 * 켜면 메타데이터는 SPDK 내부에서만 다뤄지고 사용자에게는 data-only 블록 크기로 보여야 한다.
 * 따라서 길이/오프셋 계산에 쓰는 "블록 크기"가 desc 옵션에 따라 달라진다.
 * 실행 컨텍스트: I/O 인자 검증/분할 hot-path.
 *
 * 호출 체인:
 *   bdev_io_get_block_size 등 → [bdev_desc_get_block_size] → spdk_bdev_desc_get_bdev
 */
static inline uint32_t
bdev_desc_get_block_size(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	/* [한국어] desc가 가리키는 실제 bdev 획득(블록/메타 길이 출처). */

	if (spdk_unlikely(desc->opts.hide_metadata)) {
		/* [한국어] 메타데이터 숨김 모드: 사용자에게는 데이터-온리 블록 크기를 노출. */
		return bdev->blocklen - bdev->md_len;
	} else {
		/* [한국어] 일반 모드: 메타데이터 포함 전체 블록 길이. */
		return bdev->blocklen;
	}
}

/*
 * [한국어]
 * bdev_io_get_block_size - 이 I/O에 적용할 유효 블록 크기 계산(NVMe PRACT 특수 처리 포함).
 *
 * @bdev_io: 대상 I/O.
 * @return: 블록당 바이트 수.
 *
 * NVMe Protection Information의 PRACT(Protection Information Action) 플래그가 켜지면
 * 컨트롤러가 전송 중 PI(메타데이터)를 삽입/제거하므로, 호스트 버퍼 상의 블록 크기는
 * 메타데이터를 제외한 크기가 될 수 있다. 단, md_len이 PI 포맷 크기와 정확히 같을 때만
 * 그렇게 취급하고, 그 외에는 일반 desc 기반 블록 크기로 폴백한다.
 * 실행 컨텍스트: I/O 분할/길이 계산 hot-path.
 *
 * 호출 체인:
 *   split/검증 경로 → [bdev_io_get_block_size] → bdev_desc_get_block_size
 */
static inline uint32_t
bdev_io_get_block_size(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	/* [한국어] 대상 bdev — 블록/메타 길이와 PI 포맷의 출처. */

	if (bdev_io->u.bdev.dif_check_flags & SPDK_DIF_FLAGS_NVME_PRACT) {
		/* [한국어] PRACT 활성: 컨트롤러가 PI를 in-line으로 처리 → 호스트 버퍼에서
		 *  메타데이터가 빠질 수 있음. */
		if (bdev->md_len == spdk_dif_pi_format_get_size(bdev->dif_pi_format)) {
			/* [한국어] 메타 길이가 PI 포맷 크기와 정확히 일치할 때만 data-only 크기 사용. */
			return bdev->blocklen - bdev->md_len;
		} else {
			/* [한국어] 그 외에는 메타 포함 전체 블록 크기 사용. */
			return bdev->blocklen;
		}
	}

	return bdev_desc_get_block_size(bdev_io->internal.desc);
	/* [한국어] PRACT 비활성: desc 옵션(hide_metadata) 기반 블록 크기로 폴백. */
}

/*
 * [한국어]
 * bdev_queue_nomem_io_head - 모듈이 -ENOMEM을 반환한 I/O를 nomem 재시도 큐 선두에 적재.
 *
 * @shared_resource: 이 io_device를 공유하는 채널들의 nomem 큐를 보유한 공유 자원.
 * @bdev_io: -ENOMEM으로 거절되어 재시도 대기에 들어갈 I/O.
 * @state: 재시도 시 어느 단계(submit/pull/push 등)부터 다시 시작할지.
 * @return: 없음.
 *
 * 백엔드 모듈(예: NVMe qpair tracker 고갈)이 -ENOMEM을 주면 그 I/O를 버리지 않고
 * nomem_io 큐에 보관했다가, outstanding I/O가 일정 수 완료되어 자원이 풀리면 자동
 * 재발행한다. head에 넣는 이유: 이미 한 번 제출 시도된(원래 순서가 앞선) I/O이므로
 * 공정성을 위해 큐 앞쪽에 둔다. 이때 nomem_threshold를 다시 설정해 "얼마나 완료되면
 * 재시도를 시작할지"의 트리거 수위를 정한다 — 큐 깊이가 낮으면 절반, 보통은
 * NOMEM_THRESHOLD_COUNT만큼 빠질 때까지 기다린다.
 * 실행 컨텍스트: 채널 소유 스레드(락 없음 — shared_resource는 동일 io_device·스레드 단위).
 * caller: bdev_io_submit / 각종 pull/push 콜백의 -ENOMEM 처리부.
 *
 * 호출 체인:
 *   submit/pull/push(-ENOMEM) → [bdev_queue_nomem_io_head] → TAILQ_INSERT_HEAD
 */
static inline void
bdev_queue_nomem_io_head(struct spdk_bdev_shared_resource *shared_resource,
			 struct spdk_bdev_io *bdev_io, enum bdev_io_retry_state state)
{
	/* Wait for some of the outstanding I/O to complete before we retry any of the nomem_io.
	 * Normally we will wait for NOMEM_THRESHOLD_COUNT I/O to complete but for low queue depth
	 * channels we will instead wait for half to complete.
	 */
	shared_resource->nomem_threshold = spdk_max((int64_t)shared_resource->io_outstanding / 2,
					   (int64_t)shared_resource->io_outstanding - NOMEM_THRESHOLD_COUNT);
	/* [한국어] 재시도 트리거 수위 설정: outstanding의 절반과 (outstanding-임계상수) 중 큰 값.
	 *  outstanding이 이 값까지 줄면 nomem 큐 drain을 시작 → 너무 잦은 재시도/기아 방지. */

	assert(state != BDEV_IO_RETRY_STATE_INVALID);
	/* [한국어] 재시도 단계는 반드시 유효 enum이어야 함(어디서부터 재개할지 결정). */
	bdev_io->internal.retry_state = state;
	/* [한국어] 이 I/O를 재발행할 때 진입할 단계를 기록. */
	TAILQ_INSERT_HEAD(&shared_resource->nomem_io, bdev_io, internal.link);
	/* [한국어] nomem 큐 선두에 삽입(원래 순서가 앞선 I/O이므로 우선 재시도). */
}

/*
 * [한국어]
 * bdev_queue_nomem_io_tail - 사용자가 새로 제출한 I/O를 nomem 큐 꼬리에 적재.
 *
 * @shared_resource: nomem 큐를 보유한 공유 자원.
 * @bdev_io: 큐가 비어있지 않은 상태에서 사용자가 새로 제출한 I/O.
 * @state: 재시도 시작 단계.
 * @return: 없음.
 *
 * nomem 큐가 이미 차 있는데 사용자가 새 I/O를 제출하면, 순서 역전을 막기 위해 즉시
 * 모듈에 보내지 않고 큐 꼬리에 줄 세운다(이미 대기 중인 I/O가 먼저 나가야 하므로).
 * head 버전과 달리 threshold를 갱신하지 않는다 — 이미 누군가가 설정해 둔 수위를 유지.
 * 실행 컨텍스트: 채널 소유 스레드. caller: bdev_io_submit(큐가 비어있지 않은 경로).
 *
 * 호출 체인:
 *   bdev_io_submit(큐 non-empty) → [bdev_queue_nomem_io_tail] → TAILQ_INSERT_TAIL
 */
static inline void
bdev_queue_nomem_io_tail(struct spdk_bdev_shared_resource *shared_resource,
			 struct spdk_bdev_io *bdev_io, enum bdev_io_retry_state state)
{
	/* We only queue IOs at the end of the nomem_io queue if they're submitted by the user while
	 * the queue isn't empty, so we don't need to update the nomem_threshold here */
	assert(!TAILQ_EMPTY(&shared_resource->nomem_io));
	/* [한국어] 꼬리 적재는 큐가 이미 비어있지 않을 때만 발생(불변식 단언). */

	assert(state != BDEV_IO_RETRY_STATE_INVALID);
	/* [한국어] 재시도 단계 유효성 단언. */
	bdev_io->internal.retry_state = state;
	/* [한국어] 재발행 시 진입 단계 기록. */
	TAILQ_INSERT_TAIL(&shared_resource->nomem_io, bdev_io, internal.link);
	/* [한국어] nomem 큐 꼬리에 삽입(기존 대기 I/O 이후 순서 보장). */
}

/*
 * [한국어]
 * spdk_bdev_io_set_buf - bdev_io의 단일 데이터 버퍼(iovec[0])를 설정.
 *
 * @bdev_io: 버퍼를 붙일 I/O.
 * @buf: 데이터 버퍼 시작 주소.
 * @len: 버퍼 길이(바이트).
 * @return: 없음.
 *
 * iobuf 풀에서 받은 버퍼나 모듈이 준비한 버퍼를 I/O의 첫 iovec에 꽂는다. iovs 배열이
 * 아직 없으면 bdev_io 안에 내장된 단일 iov(bdev_io->iov)를 사용하도록 self-pointing
 * 시킨다. 모듈/get_buf 콜백이 데이터 버퍼를 확보한 직후 호출한다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: get_buf 콜백, 모듈.
 *
 * 호출 체인:
 *   get_buf cb → [spdk_bdev_io_set_buf] → (iov 채움)
 */
void
spdk_bdev_io_set_buf(struct spdk_bdev_io *bdev_io, void *buf, size_t len)
{
	struct iovec *iovs;
	/* [한국어] 설정 대상 iovec 배열 포인터. */

	if (bdev_io->u.bdev.iovs == NULL) {
		/* [한국어] 아직 iovec 배열이 없으면 bdev_io 내장 단일 iov로 초기화. */
		bdev_io->u.bdev.iovs = &bdev_io->iov;
		/* [한국어] 배열을 내장 iov 하나를 가리키게 설정(별도 할당 회피). */
		bdev_io->u.bdev.iovcnt = 1;
		/* [한국어] 단일 세그먼트이므로 개수 1. */
	}

	iovs = bdev_io->u.bdev.iovs;
	/* [한국어] (설정되었거나 기존) iovec 배열 획득. */

	assert(iovs != NULL);
	/* [한국어] 위 분기로 인해 NULL이 아님을 단언. */
	assert(bdev_io->u.bdev.iovcnt >= 1);
	/* [한국어] 최소 1개 세그먼트 존재 단언. */

	iovs[0].iov_base = buf;
	/* [한국어] 첫 세그먼트 주소를 사용자 버퍼로 설정. */
	iovs[0].iov_len = len;
	/* [한국어] 첫 세그먼트 길이를 설정. */
}

/*
 * [한국어]
 * spdk_bdev_io_set_md_buf - bdev_io의 메타데이터(분리형) 버퍼를 설정.
 *
 * @bdev_io: 메타데이터 버퍼를 붙일 I/O.
 * @md_buf: 메타데이터 버퍼 시작 주소.
 * @len: 메타데이터 버퍼 길이(바이트).
 * @return: 없음.
 *
 * separate(분리형) 메타데이터 레이아웃에서 데이터와 별도로 보관되는 PI/메타 버퍼를 설정한다.
 * 버퍼가 블록 수 × md_size를 모두 담을 만큼 충분한지 assert로 검증한다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: get_buf 콜백/모듈.
 *
 * 호출 체인:
 *   get_buf cb → [spdk_bdev_io_set_md_buf] → (md_buf 설정)
 */
void
spdk_bdev_io_set_md_buf(struct spdk_bdev_io *bdev_io, void *md_buf, size_t len)
{
	assert((len / spdk_bdev_get_md_size(bdev_io->bdev)) >= bdev_io->u.bdev.num_blocks);
	/* [한국어] 메타 버퍼가 블록 수만큼의 메타데이터를 담을 용량인지 검증. */
	bdev_io->u.bdev.md_buf = md_buf;
	/* [한국어] 분리형 메타데이터 버퍼 포인터 설정. */
}

/*
 * [한국어]
 * _is_buf_allocated - iovec[0]에 실제 데이터 버퍼가 이미 할당돼 있는지 판정.
 *
 * @iovs: 검사할 iovec 배열(NULL 가능).
 * @return: iovs[0].iov_base가 유효 주소면 true, NULL이거나 미할당이면 false.
 *
 * 사용자가 자신의 버퍼를 직접 제공했는지(true) 아니면 bdev 레이어가 iobuf 풀에서
 * 버퍼를 받아와야 하는지(false)를 구분하는 데 사용한다.
 * 실행 컨텍스트: I/O 제출 hot-path.
 *
 * 호출 체인:
 *   bdev_io_get_buf 등 → [_is_buf_allocated] → (iov_base 검사)
 */
static bool
_is_buf_allocated(const struct iovec *iovs)
{
	if (iovs == NULL) {
		/* [한국어] iovec 배열 자체가 없으면 버퍼 미할당. */
		return false;
	}

	return iovs[0].iov_base != NULL;
	/* [한국어] 첫 세그먼트 주소가 채워져 있으면 버퍼 보유로 간주. */
}

/*
 * [한국어]
 * _are_iovs_aligned - 모든 iovec 세그먼트의 시작 주소가 요구 정렬을 만족하는지 검사.
 *
 * @iovs: 검사할 iovec 배열.
 * @iovcnt: 세그먼트 개수.
 * @alignment: 요구 정렬 바이트(2의 거듭제곱). 1이면 정렬 제약 없음.
 * @return: 모든 세그먼트가 정렬되면 true, 하나라도 어긋나면 false.
 *
 * 일부 백엔드(특히 DMA를 직접 하는 NVMe)는 버퍼 정렬을 요구한다. 사용자 버퍼가 정렬을
 * 어기면 bdev 레이어가 정렬된 bounce buffer를 끼워야 하므로, 그 필요 여부를 먼저 판단한다.
 * 실행 컨텍스트: I/O 제출 hot-path.
 *
 * 호출 체인:
 *   bdev_io_get_buf 등 → [_are_iovs_aligned] → (주소 & (align-1) 검사)
 */
static bool
_are_iovs_aligned(struct iovec *iovs, int iovcnt, uint32_t alignment)
{
	int i;
	/* [한국어] 세그먼트 순회 인덱스. */
	uintptr_t iov_base;
	/* [한국어] 정렬 검사를 위해 정수로 캐스팅한 세그먼트 시작 주소. */

	if (spdk_likely(alignment == 1)) {
		/* [한국어] 정렬 1 = 제약 없음 → 항상 정렬된 것으로 간주(빠른 경로). */
		return true;
	}

	for (i = 0; i < iovcnt; i++) {
		/* [한국어] 모든 세그먼트를 검사. */
		iov_base = (uintptr_t)iovs[i].iov_base;
		/* [한국어] 주소를 정수로 변환. */
		if ((iov_base & (alignment - 1)) != 0) {
			/* [한국어] alignment-1 마스크로 하위 비트 검사 — 0이 아니면 미정렬. */
			return false;
		}
	}

	return true;
	/* [한국어] 모든 세그먼트가 정렬됨. */
}

/*
 * [한국어]
 * bdev_io_needs_metadata - 이 I/O가 메타데이터 변환 처리를 필요로 하는지 판정.
 *
 * @desc: 사용자 디스크립터(hide_metadata 옵션 출처).
 * @bdev_io: 대상 I/O.
 * @return: 메타데이터가 있고(md_len!=0) hide_metadata 또는 NVMe PRACT가 켜진 경우 true.
 *
 * 메타데이터가 존재하는 bdev에서, 사용자가 메타데이터를 숨기길 원하거나 NVMe PRACT로
 * PI를 in-line 처리해야 하는 경우 bdev 레이어가 추가 변환(삽입/제거/재배치)을 해야 한다.
 * 실행 컨텍스트: I/O 제출 hot-path.
 *
 * 호출 체인:
 *   submit 경로 → [bdev_io_needs_metadata] → (md_len/플래그 검사)
 */
static inline bool
bdev_io_needs_metadata(struct spdk_bdev_desc *desc, struct spdk_bdev_io *bdev_io)
{
	return (bdev_io->bdev->md_len != 0) &&
	       (desc->opts.hide_metadata ||
		(bdev_io->u.bdev.dif_check_flags & SPDK_DIF_FLAGS_NVME_PRACT));
	/* [한국어] 메타데이터가 실제로 존재하면서(앞 조건), 숨김 모드이거나 PRACT가 켜졌으면
	 *  bdev 레이어 메타데이터 처리가 필요(뒤 OR). */
}

/*
 * [한국어]
 * bdev_io_accel_sequence_supported - 이 I/O의 accel sequence를 백엔드가 직접 처리 가능한지.
 *
 * @bdev_io: 대상 I/O.
 * @return: 해당 io_type에 대해 모듈이 accel sequence를 지원하고, 또한 split되지 않은 경우 true.
 *
 * accel sequence(암호화/압축 등 변환 체인)가 붙은 I/O를 모듈이 스스로 실행할 수 있으면
 * bdev 레이어가 미리 실행할 필요가 없다. 단, split된 I/O는 현재 모듈에 sequence를
 * 넘기지 않는 정책이라 지원하지 않는 것으로 취급한다.
 * 실행 컨텍스트: I/O 제출 hot-path.
 *
 * 호출 체인:
 *   bdev_io_needs_sequence_exec → [bdev_io_accel_sequence_supported] → (지원 비트맵 검사)
 */
static bool
bdev_io_accel_sequence_supported(struct spdk_bdev_io *bdev_io)
{
	/* For now, we don't allow splitting IOs with an accel sequence and will treat them as if
	 * bdev module didn't support accel sequences */
	return (bdev_io->bdev->accel_sequence_supported & (1u << bdev_io->type)) &&
	       !bdev_io->internal.f.split;
	/* [한국어] accel_sequence_supported 비트맵에서 이 io_type 비트가 켜졌고(앞),
	 *  split되지 않은 I/O일 때만(뒤) 모듈 직접 처리 가능으로 판정. */
}

/*
 * [한국어]
 * bdev_io_needs_sequence_exec - bdev 레이어가 직접 accel sequence를 실행해야 하는지 판정.
 *
 * @bdev_io: 대상 I/O.
 * @return: accel sequence가 붙어있지만 모듈이 그것을 직접 처리하지 못하면 true.
 *
 * sequence가 붙어있고(use), 모듈이 지원하지 않으면(supported가 false) bdev 레이어가
 * 모듈에 I/O를 넘기기 전에 sequence를 먼저 실행해 데이터를 변환해 둬야 한다.
 * 실행 컨텍스트: I/O 제출 hot-path.
 *
 * 호출 체인:
 *   submit 경로 → [bdev_io_needs_sequence_exec] → bdev_io_use/accel_sequence_supported
 */
static inline bool
bdev_io_needs_sequence_exec(struct spdk_bdev_io *bdev_io)
{
	if (!bdev_io_use_accel_sequence(bdev_io)) {
		/* [한국어] 애초에 sequence가 없으면 bdev 레이어 실행 불필요. */
		return false;
	}

	return !bdev_io_accel_sequence_supported(bdev_io);
	/* [한국어] sequence가 있는데 모듈이 직접 처리 못하면 bdev 레이어가 실행해야 함. */
}

/*
 * [한국어]
 * bdev_io_increment_outstanding - outstanding I/O 카운터(채널/공유자원)를 증가.
 *
 * @bdev_ch: 이 I/O를 발행한 채널.
 * @shared_resource: 채널이 속한 io_device의 공유 자원.
 * @return: 없음.
 *
 * 모듈에 I/O를 제출하기 직전에 "진행 중 I/O" 수를 두 레벨(채널 단위, io_device 공유 단위)에서
 * 증가시킨다. 이 카운터는 reset drain 대기, nomem threshold 판정 등에 쓰인다.
 * 실행 컨텍스트: 채널 소유 스레드(lockless — 카운터는 스레드 로컬/공유자원 단위 단독 접근).
 *
 * 호출 체인:
 *   bdev_io_do_submit → [bdev_io_increment_outstanding]
 */
static inline void
bdev_io_increment_outstanding(struct spdk_bdev_channel *bdev_ch,
			      struct spdk_bdev_shared_resource *shared_resource)
{
	bdev_ch->io_outstanding++;
	/* [한국어] 채널 단위 진행 I/O 수 증가(reset drain 판정용). */
	shared_resource->io_outstanding++;
	/* [한국어] io_device 공유 단위 진행 I/O 수 증가(nomem threshold 판정용). */
}

/*
 * [한국어]
 * bdev_io_decrement_outstanding - outstanding I/O 카운터(채널/공유자원)를 감소.
 *
 * @bdev_ch: 완료된 I/O의 채널.
 * @shared_resource: 채널이 속한 io_device의 공유 자원.
 * @return: 없음.
 *
 * I/O가 완료되어 후처리할 때 두 레벨 카운터를 감소시킨다. 0 미만으로 내려가는 것을
 * assert로 방지한다. 이 감소가 nomem threshold를 가로지르면 재시도가 트리거된다.
 * 실행 컨텍스트: 채널 소유 스레드(lockless).
 *
 * 호출 체인:
 *   bdev_io_complete 계열 → [bdev_io_decrement_outstanding]
 */
static inline void
bdev_io_decrement_outstanding(struct spdk_bdev_channel *bdev_ch,
			      struct spdk_bdev_shared_resource *shared_resource)
{
	assert(bdev_ch->io_outstanding > 0);
	/* [한국어] 감소 전 채널 카운터가 양수여야 함(중복 완료 방지). */
	assert(shared_resource->io_outstanding > 0);
	/* [한국어] 공유 카운터도 양수여야 함. */
	bdev_ch->io_outstanding--;
	/* [한국어] 채널 단위 진행 I/O 수 감소. */
	shared_resource->io_outstanding--;
	/* [한국어] 공유 단위 진행 I/O 수 감소(이후 nomem retry 트리거 판단). */
}

/*
 * [한국어]
 * bdev_io_submit_sequence_cb - accel sequence 실행 완료 후 I/O를 실제 모듈에 제출하는 콜백.
 *
 * @ctx: 콜백 컨텍스트(= bdev_io).
 * @status: accel sequence 실행 결과(0=성공).
 * @return: 없음.
 *
 * 모듈이 sequence를 직접 처리하지 못해 bdev 레이어가 먼저 sequence를 실행한 경우,
 * 그 실행이 끝나면 이 콜백으로 돌아온다. 성공이면 변환이 완료된 데이터로 정상 submit을
 * 이어가고, 실패면 I/O를 FAILED로 즉시 완료한다. sequence는 이미 소비됐으므로 해제 표시한다.
 * 실행 컨텍스트: accel framework 완료 컨텍스트(보통 동일 스레드). caller: spdk_accel_sequence 실행.
 *
 * 호출 체인:
 *   accel sequence 완료 → [bdev_io_submit_sequence_cb] → bdev_io_submit / bdev_io_complete_unsubmitted
 */
static void
bdev_io_submit_sequence_cb(void *ctx, int status)
{
	struct spdk_bdev_io *bdev_io = ctx;
	/* [한국어] 콜백 ctx를 bdev_io로 복원. */

	assert(bdev_io_use_accel_sequence(bdev_io));
	/* [한국어] 이 경로는 sequence가 붙은 I/O에서만 도달. */

	bdev_io->u.bdev.accel_sequence = NULL;
	/* [한국어] sequence가 실행 완료되어 소비되었으므로 포인터 해제. */
	bdev_io->internal.f.has_accel_sequence = false;
	/* [한국어] sequence 보유 플래그 클리어 — 이후 경로에서 재실행하지 않도록. */

	if (spdk_unlikely(status != 0)) {
		/* [한국어] sequence 실행 실패: 데이터 변환이 안 됐으므로 I/O를 실패 처리. */
		SPDK_ERRLOG("Failed to execute accel sequence, status=%d\n", status);
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		/* [한국어] I/O 상태를 FAILED로 설정. */
		bdev_io_complete_unsubmitted(bdev_io);
		/* [한국어] 모듈에 제출되지 않은 I/O이므로 outstanding 감소 없이 완료 처리. */
		return;
	}

	bdev_io_submit(bdev_io);
	/* [한국어] 변환 성공 — 이제 실제 모듈에 I/O 제출 진행. */
}

/*
 * [한국어]
 * bdev_io_exec_sequence_cb - I/O 완료 후 데이터를 사용자 버퍼로 옮기는 accel sequence 실행 완료 콜백.
 *
 * @ctx: 콜백 컨텍스트(= bdev_io).
 * @status: sequence 실행 결과.
 * @return: 없음.
 *
 * 읽기 I/O처럼 완료 후에 변환(예: 복호화)이 필요한 경우, 모듈 완료 시점에 sequence를
 * 실행하고 끝나면 이 콜백으로 돌아온다. accel 실행 큐(io_accel_exec)에서 제거하고
 * outstanding을 줄인 뒤, nomem 큐에 대기 I/O가 있으면 재시도를 깨우고, 최종적으로
 * 원래 등록된 data_transfer_cpl 콜백을 호출해 완료 체인을 이어간다.
 * 실행 컨텍스트: accel framework 완료 컨텍스트(동일 채널 스레드). caller: spdk_accel_sequence 실행.
 *
 * 호출 체인:
 *   accel sequence 완료 → [bdev_io_exec_sequence_cb] → bdev_ch_retry_io / data_transfer_cpl
 */
static void
bdev_io_exec_sequence_cb(void *ctx, int status)
{
	struct spdk_bdev_io *bdev_io = ctx;
	/* [한국어] 콜백 ctx를 bdev_io로 복원. */
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] I/O가 속한 채널 — 큐/공유자원 접근에 사용. */

	TAILQ_REMOVE(&bdev_io->internal.ch->io_accel_exec, bdev_io, internal.link);
	/* [한국어] accel 실행 진행 큐에서 이 I/O 제거(실행이 끝났으므로). */
	bdev_io_decrement_outstanding(ch, ch->shared_resource);
	/* [한국어] sequence 실행도 outstanding으로 잡혀 있었으므로 카운터 감소. */

	if (spdk_unlikely(!TAILQ_EMPTY(&ch->shared_resource->nomem_io))) {
		/* [한국어] 방금 자원이 풀렸고 nomem 대기 I/O가 있으면 재시도를 트리거. */
		bdev_ch_retry_io(ch);
	}

	bdev_io->internal.data_transfer_cpl(bdev_io, status);
	/* [한국어] 원래 데이터 전송 완료 콜백 호출 — 본래의 완료 후처리 체인으로 복귀. */
}

/*
 * [한국어]
 * bdev_io_exec_sequence - I/O에 누적된 accel sequence를 실제로 실행하고 완료를 cb_fn으로 통지.
 *
 * @bdev_io: accel sequence가 붙은 READ 또는 WRITE I/O.
 * @cb_fn: sequence 실행 완료 시 호출할 콜백(ctx=bdev_io, status).
 * @return: 없음(비동기 — 실행 완료는 콜백으로).
 *
 * 모듈이 직접 처리하지 못하는 accel sequence를 bdev 레이어가 spdk_accel_sequence_finish로
 * 실행한다. READ의 경우 sequence는 제출 시 append된 역순으로 쌓여 있으므로(가장 최근에
 * 추가된 연산을 먼저 실행해야 함) 실행 전에 reverse한다. 실행 동안에는 io_accel_exec
 * 큐에 매달아 두고 outstanding으로 카운트해 reset/nomem 회계에 반영한다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: bdev_io_submit 등. callee: spdk_accel_sequence_finish.
 *
 * 호출 체인:
 *   submit 경로 → [bdev_io_exec_sequence] → spdk_accel_sequence_finish → bdev_io_exec_sequence_cb
 */
static void
bdev_io_exec_sequence(struct spdk_bdev_io *bdev_io, void (*cb_fn)(void *ctx, int status))
{
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] I/O가 속한 채널 — 큐/공유자원 접근용. */

	assert(bdev_io_needs_sequence_exec(bdev_io));
	/* [한국어] bdev 레이어 실행이 필요한 경우에만 진입. */
	assert(bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE || bdev_io->type == SPDK_BDEV_IO_TYPE_READ);
	/* [한국어] sequence 변환 대상은 READ/WRITE I/O뿐. */
	assert(bdev_io_use_accel_sequence(bdev_io));
	/* [한국어] 실제로 sequence가 붙어있어야 함. */

	/* Since the operations are appended during submission, they're in the opposite order than
	 * how we want to execute them for reads (i.e. we need to execute the most recently added
	 * operation first), so reverse the sequence before executing it.
	 */
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		/* [한국어] READ는 append 순서의 역순으로 실행해야 하므로 sequence를 뒤집음. */
		spdk_accel_sequence_reverse(bdev_io->internal.accel_sequence);
	}

	TAILQ_INSERT_TAIL(&bdev_io->internal.ch->io_accel_exec, bdev_io, internal.link);
	/* [한국어] 실행 중 I/O를 accel 실행 큐에 등록(완료 콜백에서 제거). */
	bdev_io_increment_outstanding(ch, ch->shared_resource);
	/* [한국어] 실행 동안 outstanding으로 카운트(reset drain/nomem 회계에 포함). */
	bdev_io->internal.data_transfer_cpl = cb_fn;
	/* [한국어] 실행 완료 후 호출할 콜백 저장(bdev_io_exec_sequence_cb가 이를 호출). */

	spdk_accel_sequence_finish(bdev_io->internal.accel_sequence,
				   bdev_io_exec_sequence_cb, bdev_io);
	/* [한국어] accel framework에 sequence 실행을 의뢰 — 완료 시 exec_sequence_cb 호출. */
}

/*
 * [한국어]
 * bdev_io_get_buf_complete - 데이터 버퍼 확보 절차를 끝내고 사용자/모듈의 get_buf_cb를 호출.
 *
 * @bdev_io: 버퍼 확보가 완료된 I/O.
 * @status: 버퍼 확보 성공(true)/실패(false).
 * @return: 없음.
 *
 * spdk_bdev_io_get_buf로 시작된 비동기 버퍼 확보(iobuf 풀 대기, bounce/메모리도메인 pull 포함)가
 * 끝나면 등록해 둔 get_buf_cb를 한 번 호출하고 즉시 NULL로 비워 재호출을 방지한다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: _bdev_io_set_md_buf / _bdev_io_pull_buffer_cpl 등.
 *
 * 호출 체인:
 *   버퍼 확보 완료 → [bdev_io_get_buf_complete] → get_buf_cb
 */
static void
bdev_io_get_buf_complete(struct spdk_bdev_io *bdev_io, bool status)
{
	struct spdk_io_channel *ch = spdk_bdev_io_get_io_channel(bdev_io);
	/* [한국어] get_buf_cb에 넘길 io_channel 핸들 획득. */

	assert(bdev_io->internal.get_buf_cb != NULL);
	/* [한국어] 버퍼 확보를 요청한 콜백이 반드시 등록돼 있어야 함. */
	bdev_io->internal.get_buf_cb(ch, bdev_io, status);
	/* [한국어] 버퍼가 준비됐음을(또는 실패를) 요청자에게 통지. */
	bdev_io->internal.get_buf_cb = NULL;
	/* [한국어] 콜백을 비워 동일 I/O에서 중복 호출되지 않게 함. */
}

/*
 * [한국어]
 * _bdev_io_pull_buffer_cpl - bounce 버퍼로의 데이터 pull 완료 콜백(최종 단계).
 *
 * @ctx: 콜백 컨텍스트(= bdev_io).
 * @rc: pull 결과(0=성공).
 * @return: 없음.
 *
 * 사용자 버퍼 → bounce 버퍼로 데이터를 끌어오는(WRITE 경로) 절차의 마지막 콜백.
 * 실패하면 I/O를 FAILED로 표시하고, 어쨌든 get_buf 완료를 통지한다.
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인:
 *   pull 완료 → [_bdev_io_pull_buffer_cpl] → bdev_io_get_buf_complete
 */
static void
_bdev_io_pull_buffer_cpl(void *ctx, int rc)
{
	struct spdk_bdev_io *bdev_io = ctx;
	/* [한국어] 콜백 ctx 복원. */

	if (rc) {
		/* [한국어] bounce 버퍼 설정/pull 실패 → I/O 실패로 마킹. */
		SPDK_ERRLOG("Set bounce buffer failed with rc %d\n", rc);
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	}
	bdev_io_get_buf_complete(bdev_io, !rc);
	/* [한국어] 성공 여부(!rc)를 담아 get_buf 완료 통지. */
}

/*
 * [한국어]
 * bdev_io_pull_md_buf_done - 메타데이터 버퍼의 memory-domain pull 완료 콜백.
 *
 * @ctx: 콜백 컨텍스트(= bdev_io).
 * @status: pull 결과.
 * @return: 없음.
 *
 * 메타데이터를 외부 memory domain에서 bounce md 버퍼로 끌어오는 작업이 끝나면 호출된다.
 * io_memory_domain 큐에서 제거하고 outstanding을 줄인 뒤, 자원이 풀렸으면 nomem 재시도를
 * 깨우고, 등록된 data_transfer_cpl로 다음 단계를 이어간다.
 * 실행 컨텍스트: memory domain 완료 컨텍스트(동일 채널 스레드).
 *
 * 호출 체인:
 *   memory domain pull 완료 → [bdev_io_pull_md_buf_done] → data_transfer_cpl
 */
static void
bdev_io_pull_md_buf_done(void *ctx, int status)
{
	struct spdk_bdev_io *bdev_io = ctx;
	/* [한국어] 콜백 ctx 복원. */
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] I/O 채널. */

	TAILQ_REMOVE(&ch->io_memory_domain, bdev_io, internal.link);
	/* [한국어] memory-domain 진행 큐에서 제거(pull 끝남). */
	bdev_io_decrement_outstanding(ch, ch->shared_resource);
	/* [한국어] pull도 outstanding이었으므로 카운터 감소. */

	if (spdk_unlikely(!TAILQ_EMPTY(&ch->shared_resource->nomem_io))) {
		/* [한국어] 자원이 풀렸고 nomem 대기 I/O가 있으면 재시도 트리거. */
		bdev_ch_retry_io(ch);
	}

	assert(bdev_io->internal.data_transfer_cpl);
	/* [한국어] 다음 단계 콜백이 반드시 등록돼 있어야 함. */
	bdev_io->internal.data_transfer_cpl(bdev_io, status);
	/* [한국어] 메타 pull 결과를 담아 다음 단계로 진행. */
}

/*
 * [한국어]
 * bdev_io_pull_md_buf - WRITE I/O의 메타데이터를 bounce md 버퍼로 끌어오기(pull).
 *
 * @bdev_io: 메타데이터 pull이 필요한 I/O.
 * @return: 없음(비동기 — memory domain 경로는 콜백으로 이어짐).
 *
 * WRITE는 디바이스로 보내기 전에 사용자 메타데이터를 SPDK가 관리하는 bounce md 버퍼로
 * 모아와야 한다. 데이터가 외부 memory domain에 있으면 spdk_memory_domain_pull_data로
 * 비동기 DMA pull을, 일반 메모리면 단순 memcpy로 동기 복사를 수행한다. pull이 -ENOMEM이면
 * nomem 큐에 적재해 나중에 재시도한다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: _bdev_io_pull_bounce_md_buf.
 * callee: spdk_memory_domain_pull_data / memcpy.
 *
 * 호출 체인:
 *   _bdev_io_pull_bounce_md_buf → [bdev_io_pull_md_buf] → memory_domain_pull_data / data_transfer_cpl
 */
static void
bdev_io_pull_md_buf(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] I/O 채널. */
	int rc = 0;
	/* [한국어] pull 결과 코드(0=성공/동기완료, -ENOMEM=자원부족, 그 외 오류). */

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		/* [한국어] WRITE만 디바이스 전송 전 메타데이터를 미리 모아와야 함. */
		assert(bdev_io->internal.f.has_bounce_buf);
		/* [한국어] bounce 버퍼가 이미 설정돼 있어야 함. */
		if (bdev_io_use_memory_domain(bdev_io)) {
			/* [한국어] 데이터가 외부 memory domain에 있으면 비동기 DMA pull 사용. */
			TAILQ_INSERT_TAIL(&ch->io_memory_domain, bdev_io, internal.link);
			/* [한국어] memory-domain 진행 큐에 등록(완료 콜백에서 제거). */
			bdev_io_increment_outstanding(ch, ch->shared_resource);
			/* [한국어] pull 동안 outstanding으로 카운트. */
			rc = spdk_memory_domain_pull_data(bdev_io->internal.memory_domain,
							  bdev_io->internal.memory_domain_ctx,
							  &bdev_io->internal.bounce_buf.orig_md_iov, 1,
							  &bdev_io->internal.bounce_buf.md_iov, 1,
							  bdev_io_pull_md_buf_done, bdev_io);
			/* [한국어] 원본 md(orig_md_iov)→bounce md(md_iov)로 DMA pull 요청. */
			if (rc == 0) {
				/* Continue to submit IO in completion callback */
				/* [한국어] 비동기 시작 성공 → 나머지는 done 콜백에서 이어감. */
				return;
			}
			bdev_io_decrement_outstanding(ch, ch->shared_resource);
			/* [한국어] 즉시 실패 시 위에서 올린 outstanding 원복. */
			TAILQ_REMOVE(&ch->io_memory_domain, bdev_io, internal.link);
			/* [한국어] 진행 큐 등록도 원복. */
			if (rc != -ENOMEM) {
				/* [한국어] -ENOMEM 외 실패는 로깅(ENOMEM은 아래에서 재시도 처리). */
				SPDK_ERRLOG("Failed to pull data from memory domain %s, rc %d\n",
					    spdk_memory_domain_get_dma_device_id(
						    bdev_io->internal.memory_domain), rc);
			}
		} else {
			/* [한국어] 일반 메모리: 동기 memcpy로 원본 md → bounce md 복사. */
			memcpy(bdev_io->internal.bounce_buf.md_iov.iov_base,
			       bdev_io->internal.bounce_buf.orig_md_iov.iov_base,
			       bdev_io->internal.bounce_buf.orig_md_iov.iov_len);
		}
	}

	if (spdk_unlikely(rc == -ENOMEM)) {
		/* [한국어] 자원 부족 → nomem 큐에 적재해 PULL_MD 단계부터 재시도. */
		bdev_queue_nomem_io_head(ch->shared_resource, bdev_io, BDEV_IO_RETRY_STATE_PULL_MD);
	} else {
		/* [한국어] 동기 완료(성공 또는 비-ENOMEM 오류) → 다음 단계 콜백 호출. */
		assert(bdev_io->internal.data_transfer_cpl);
		bdev_io->internal.data_transfer_cpl(bdev_io, rc);
	}
}

/*
 * [한국어]
 * _bdev_io_pull_bounce_md_buf - 분리형 메타데이터를 위한 bounce md 버퍼를 설치하고 pull 시작.
 *
 * @bdev_io: 대상 I/O.
 * @md_buf: 새로 마련한 bounce 메타데이터 버퍼.
 * @len: 메타데이터 길이.
 * @return: 없음.
 *
 * 원래 사용자 md_buf 위치/길이를 orig_md_iov에 백업하고, bounce 버퍼 정보를 md_iov에
 * 기록한 뒤, I/O의 md_buf를 bounce 버퍼로 교체한다. 그러면 디바이스는 bounce 버퍼로
 * I/O하고, bdev 레이어가 사용자 버퍼와의 복사를 책임진다. 설치 후 실제 pull을 트리거한다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: _bdev_io_set_md_buf. callee: bdev_io_pull_md_buf.
 *
 * 호출 체인:
 *   _bdev_io_set_md_buf → [_bdev_io_pull_bounce_md_buf] → bdev_io_pull_md_buf
 */
static void
_bdev_io_pull_bounce_md_buf(struct spdk_bdev_io *bdev_io, void *md_buf, size_t len)
{
	assert(bdev_io->internal.f.has_bounce_buf);
	/* [한국어] bounce 버퍼 컨텍스트가 이미 확보돼 있어야 함. */

	/* save original md_buf */
	bdev_io->internal.bounce_buf.orig_md_iov.iov_base = bdev_io->u.bdev.md_buf;
	/* [한국어] 사용자 원본 메타 버퍼 주소를 백업(완료 시 복사 대상). */
	bdev_io->internal.bounce_buf.orig_md_iov.iov_len = len;
	/* [한국어] 원본 메타 길이 백업. */
	bdev_io->internal.bounce_buf.md_iov.iov_base = md_buf;
	/* [한국어] bounce 메타 버퍼 주소 기록. */
	bdev_io->internal.bounce_buf.md_iov.iov_len = len;
	/* [한국어] bounce 메타 길이 기록. */
	/* set bounce md_buf */
	bdev_io->u.bdev.md_buf = md_buf;
	/* [한국어] I/O가 디바이스에 쓸 메타 버퍼를 bounce 버퍼로 교체. */

	bdev_io_pull_md_buf(bdev_io);
	/* [한국어] (WRITE면) 사용자 메타→bounce 메타 pull 수행. */
}

/*
 * [한국어]
 * _bdev_io_set_md_buf - 분리형 메타데이터 레이아웃에서 메타 버퍼를 데이터 버퍼 뒤에 배치.
 *
 * @bdev_io: 대상 I/O.
 * @return: 없음.
 *
 * separate metadata bdev에서는 데이터 버퍼 바로 뒤에 메타데이터 버퍼를 연속 배치한다.
 * 데이터 iov[0]의 끝 주소를 메타 버퍼 시작으로 삼고, 정렬을 확인한다. 사용자가 자체 md_buf를
 * 줬으면 bounce 경로로 pull하고, 아니면 계산한 위치를 md_buf로 설정한다. 분리형이 아니면
 * 메타 처리 없이 바로 버퍼 확보 완료를 통지한다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: bdev_io_pull_data_done.
 * callee: _bdev_io_pull_bounce_md_buf / spdk_bdev_io_set_md_buf / bdev_io_get_buf_complete.
 *
 * 호출 체인:
 *   bdev_io_pull_data_done → [_bdev_io_set_md_buf] → (bounce/직접 설정) → get_buf_complete
 */
static void
_bdev_io_set_md_buf(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	/* [한국어] 메타 길이/정렬 정보의 출처 bdev. */
	uint64_t md_len;
	/* [한국어] 전체 메타데이터 길이 = 블록수 × 블록당 메타길이. */
	void *buf;
	/* [한국어] 데이터 버퍼 뒤에 배치할 메타 버퍼 시작 주소. */

	if (spdk_bdev_is_md_separate(bdev)) {
		/* [한국어] 분리형 메타데이터일 때만 별도 메타 버퍼 배치가 필요. */
		assert(!bdev_io_use_accel_sequence(bdev_io));
		/* [한국어] 이 경로는 accel sequence와 함께 쓰이지 않음. */

		buf = (char *)bdev_io->u.bdev.iovs[0].iov_base + bdev_io->u.bdev.iovs[0].iov_len;
		/* [한국어] 데이터 버퍼 끝 주소를 메타 버퍼 시작으로 사용(연속 배치). */
		md_len = bdev_io->u.bdev.num_blocks * bdev->md_len;
		/* [한국어] 전체 메타 길이 계산. */

		assert(((uintptr_t)buf & (spdk_bdev_get_buf_align(bdev) - 1)) == 0);
		/* [한국어] 메타 버퍼 시작이 bdev 정렬 요구를 만족하는지 검증. */

		if (bdev_io->u.bdev.md_buf != NULL) {
			/* [한국어] 사용자가 자체 메타 버퍼를 제공 → bounce 경로로 복사 처리. */
			_bdev_io_pull_bounce_md_buf(bdev_io, buf, md_len);
			return;
			/* [한국어] bounce 경로는 비동기로 이어지므로 여기서 반환. */
		} else {
			/* [한국어] 사용자 메타 버퍼 없음 → 계산한 위치를 메타 버퍼로 직접 설정. */
			spdk_bdev_io_set_md_buf(bdev_io, buf, md_len);
		}
	}

	bdev_io_get_buf_complete(bdev_io, true);
	/* [한국어] 메타 처리 불필요/완료 → 버퍼 확보 성공 통지. */
}

/*
 * [한국어]
 * bdev_io_pull_data_done - 데이터 pull(사용자→bounce) 완료 후 메타 버퍼 설정 단계로 진행.
 *
 * @bdev_io: 대상 I/O.
 * @rc: 데이터 pull 결과(0=성공).
 * @return: 없음.
 *
 * 데이터 버퍼 확보/복사가 끝나면 호출된다. 실패면 즉시 완료 콜백으로 오류를 전파하고,
 * 성공이면 메타데이터 버퍼 배치 단계(_bdev_io_set_md_buf)로 넘어간다.
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인:
 *   pull 완료 → [bdev_io_pull_data_done] → _bdev_io_set_md_buf / data_transfer_cpl
 */
static inline void
bdev_io_pull_data_done(struct spdk_bdev_io *bdev_io, int rc)
{
	if (rc) {
		/* [한국어] 데이터 버퍼 확보 실패 → 완료 콜백으로 오류 전파. */
		SPDK_ERRLOG("Failed to get data buffer\n");
		assert(bdev_io->internal.data_transfer_cpl);
		bdev_io->internal.data_transfer_cpl(bdev_io, rc);
		return;
	}

	_bdev_io_set_md_buf(bdev_io);
	/* [한국어] 데이터 준비 완료 → 메타데이터 버퍼 배치 단계로. */
}

/*
 * [한국어]
 * bdev_io_pull_data_done_and_track - memory-domain 데이터 pull 완료 콜백(추적 큐 정리 포함).
 *
 * @ctx: 콜백 컨텍스트(= bdev_io).
 * @status: pull 결과.
 * @return: 없음.
 *
 * 외부 memory domain에서 데이터를 bounce 버퍼로 끌어오는 비동기 pull이 끝나면 호출된다.
 * io_memory_domain 추적 큐에서 제거하고 outstanding을 줄인 뒤, nomem 재시도를 깨우고,
 * 공통 후처리(bdev_io_pull_data_done)로 진행한다.
 * 실행 컨텍스트: memory domain 완료 컨텍스트(동일 채널 스레드).
 *
 * 호출 체인:
 *   memory domain pull 완료 → [bdev_io_pull_data_done_and_track] → bdev_io_pull_data_done
 */
static void
bdev_io_pull_data_done_and_track(void *ctx, int status)
{
	struct spdk_bdev_io *bdev_io = ctx;
	/* [한국어] 콜백 ctx 복원. */
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] I/O 채널. */

	TAILQ_REMOVE(&ch->io_memory_domain, bdev_io, internal.link);
	/* [한국어] memory-domain 추적 큐에서 제거(pull 끝남). */
	bdev_io_decrement_outstanding(ch, ch->shared_resource);
	/* [한국어] pull outstanding 감소. */

	if (spdk_unlikely(!TAILQ_EMPTY(&ch->shared_resource->nomem_io))) {
		/* [한국어] 자원 풀림 + nomem 대기 있으면 재시도 트리거. */
		bdev_ch_retry_io(ch);
	}

	bdev_io_pull_data_done(bdev_io, status);
	/* [한국어] 공통 pull 완료 후처리로 진행. */
}

/*
 * [한국어]
 * bdev_io_pull_data - I/O 데이터를 bounce 버퍼로 준비(메타 interleave/accel/memcpy 분기).
 *
 * @bdev_io: 데이터 버퍼 준비가 필요한 I/O.
 * @return: 없음(memory domain 경로는 콜백으로 이어짐).
 *
 * 사용자 버퍼와 디바이스가 요구하는 버퍼 형식이 다를 때(메타데이터 interleave, 정렬,
 * 외부 memory domain, accel 변환) bdev 레이어가 bounce 버퍼로 데이터를 옮기는 핵심 함수.
 * 세 가지 큰 분기:
 *   1) interleaved 메타데이터 필요: accel DIF generate/verify + copy를 sequence에 append.
 *   2) accel sequence 실행 또는 memory domain 사용: accel copy를 sequence에 append해
 *      이전 연산의 src/dst를 bounce↔원본으로 바꿔치기.
 *   3) 그 외 WRITE: memory domain이면 비동기 pull, 일반이면 동기 spdk_copy_iovs_to_buf.
 * -ENOMEM이면 nomem 큐에 PULL 단계로 적재해 재시도.
 * 실행 컨텍스트: 채널 소유 스레드. caller: _bdev_io_pull_bounce_data_buf.
 * callee: spdk_accel_append_* / spdk_memory_domain_pull_data / spdk_copy_iovs_to_buf.
 *
 * 호출 체인:
 *   _bdev_io_pull_bounce_data_buf → [bdev_io_pull_data] → accel/memory_domain/memcpy → pull_data_done
 */
static void
bdev_io_pull_data(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] I/O 채널 — accel_channel/큐/공유자원 접근용. */
	struct spdk_bdev_desc *desc = bdev_io->internal.desc;
	/* [한국어] 디스크립터 — 메타데이터 필요 여부 판정에 사용. */
	int rc = 0;
	/* [한국어] append/pull 결과 코드. */

	assert(bdev_io->internal.f.has_bounce_buf);
	/* [한국어] bounce 버퍼 컨텍스트가 설치된 상태에서만 호출. */

	if (bdev_io_needs_metadata(desc, bdev_io)) {
		/* [한국어] 분기1: interleaved 메타데이터(DIF/PI) 처리가 필요한 경우. */
		assert(bdev_io->bdev->md_interleave);
		/* [한국어] 이 경로는 메타데이터가 데이터와 섞인(interleave) 레이아웃 전제. */

		bdev_io->u.bdev.dif_check_flags &= ~SPDK_DIF_FLAGS_NVME_PRACT;
		/* [한국어] bdev 레이어가 DIF를 직접 처리하므로 컨트롤러 PRACT 위임 플래그 해제. */

		if (!bdev_io_use_accel_sequence(bdev_io)) {
			/* [한국어] 기존 sequence가 없으면 새로 시작하도록 NULL로 초기화. */
			bdev_io->internal.accel_sequence = NULL;
		}

		if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
			/* [한국어] WRITE: 사용자 데이터 → bounce로 복사하며 DIF를 생성(generate). */
			rc = spdk_accel_append_dif_generate_copy(&bdev_io->internal.accel_sequence, ch->accel_channel,
					bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					bdev_io->u.bdev.memory_domain,
					bdev_io->u.bdev.memory_domain_ctx,
					bdev_io->internal.bounce_buf.orig_iovs,
					bdev_io->internal.bounce_buf.orig_iovcnt,
					bdev_io_use_memory_domain(bdev_io) ? bdev_io->internal.memory_domain : NULL,
					bdev_io_use_memory_domain(bdev_io) ? bdev_io->internal.memory_domain_ctx : NULL,
					bdev_io->u.bdev.num_blocks,
					&bdev_io->u.bdev.dif_ctx,
					NULL, NULL);
		} else {
			/* [한국어] READ: bounce→사용자 데이터 복사하며 DIF를 검증(verify). */
			assert(bdev_io->type == SPDK_BDEV_IO_TYPE_READ);
			rc = spdk_accel_append_dif_verify_copy(&bdev_io->internal.accel_sequence, ch->accel_channel,
							       bdev_io->internal.bounce_buf.orig_iovs,
							       bdev_io->internal.bounce_buf.orig_iovcnt,
							       bdev_io_use_memory_domain(bdev_io) ? bdev_io->internal.memory_domain : NULL,
							       bdev_io_use_memory_domain(bdev_io) ? bdev_io->internal.memory_domain_ctx : NULL,
							       bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
							       bdev_io->u.bdev.memory_domain,
							       bdev_io->u.bdev.memory_domain_ctx,
							       bdev_io->u.bdev.num_blocks,
							       &bdev_io->u.bdev.dif_ctx,
							       &bdev_io->u.bdev.dif_err,
							       NULL, NULL);
		}

		if (spdk_likely(rc == 0)) {
			/* [한국어] append 성공 → sequence 보유 표시 및 u.bdev에도 노출. */
			bdev_io->internal.f.has_accel_sequence = true;
			bdev_io->u.bdev.accel_sequence = bdev_io->internal.accel_sequence;
		} else if (rc != -ENOMEM) {
			/* [한국어] -ENOMEM 외 실패는 로깅(ENOMEM은 아래에서 재시도). */
			SPDK_ERRLOG("Failed to append generate/verify_copy to accel sequence: %p\n",
				    bdev_io->internal.accel_sequence);
		}
	} else if (bdev_io_needs_sequence_exec(bdev_io) ||
		   (bdev_io_use_accel_sequence(bdev_io) && bdev_io_use_memory_domain(bdev_io))) {
		/* If we need to exec an accel sequence or the IO uses a memory domain buffer and has a
		 * sequence, append a copy operation making accel change the src/dst buffers of the previous
		 * operation */
		/* [한국어] 분기2: accel sequence 실행이 필요하거나, memory domain 버퍼 + sequence 조합.
		 *  bounce↔원본 버퍼 사이 복사 연산을 sequence 끝에 덧붙여 이전 연산의 src/dst를 교체. */
		assert(bdev_io_use_accel_sequence(bdev_io));
		/* [한국어] sequence가 반드시 존재해야 함. */
		if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
			/* [한국어] WRITE: src=사용자 iov, dst=bounce(orig) — 디바이스 전송 전 복사. */
			rc = spdk_accel_append_copy(&bdev_io->internal.accel_sequence, ch->accel_channel,
						    bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						    NULL, NULL,
						    bdev_io->internal.bounce_buf.orig_iovs,
						    bdev_io->internal.bounce_buf.orig_iovcnt,
						    bdev_io_use_memory_domain(bdev_io) ? bdev_io->internal.memory_domain : NULL,
						    bdev_io_use_memory_domain(bdev_io) ? bdev_io->internal.memory_domain_ctx : NULL,
						    NULL, NULL);
		} else {
			/* We need to reverse the src/dst for reads */
			/* [한국어] READ: src/dst를 뒤집어야 함(디바이스→bounce→사용자 방향). */
			assert(bdev_io->type == SPDK_BDEV_IO_TYPE_READ);
			rc = spdk_accel_append_copy(&bdev_io->internal.accel_sequence, ch->accel_channel,
						    bdev_io->internal.bounce_buf.orig_iovs,
						    bdev_io->internal.bounce_buf.orig_iovcnt,
						    bdev_io_use_memory_domain(bdev_io) ? bdev_io->internal.memory_domain : NULL,
						    bdev_io_use_memory_domain(bdev_io) ? bdev_io->internal.memory_domain_ctx : NULL,
						    bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						    NULL, NULL, NULL, NULL);
		}

		if (spdk_unlikely(rc != 0 && rc != -ENOMEM)) {
			/* [한국어] -ENOMEM 외 append 실패 로깅. */
			SPDK_ERRLOG("Failed to append copy to accel sequence: %p\n",
				    bdev_io->internal.accel_sequence);
		}
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		/* if this is write path, copy data from original buffer to bounce buffer */
		/* [한국어] 분기3: 일반 WRITE — 사용자 버퍼 → bounce 버퍼로 직접 복사. */
		if (bdev_io_use_memory_domain(bdev_io)) {
			/* [한국어] 외부 memory domain이면 비동기 DMA pull 사용. */
			TAILQ_INSERT_TAIL(&ch->io_memory_domain, bdev_io, internal.link);
			/* [한국어] memory-domain 추적 큐에 등록. */
			bdev_io_increment_outstanding(ch, ch->shared_resource);
			/* [한국어] pull 동안 outstanding 카운트. */
			rc = spdk_memory_domain_pull_data(bdev_io->internal.memory_domain,
							  bdev_io->internal.memory_domain_ctx,
							  bdev_io->internal.bounce_buf.orig_iovs,
							  (uint32_t)bdev_io->internal.bounce_buf.orig_iovcnt,
							  bdev_io->u.bdev.iovs, 1,
							  bdev_io_pull_data_done_and_track,
							  bdev_io);
			/* [한국어] 원본 iov → bounce iov로 비동기 pull 요청. */
			if (rc == 0) {
				/* Continue to submit IO in completion callback */
				/* [한국어] 비동기 시작 성공 → 완료 콜백에서 이어감. */
				return;
			}
			TAILQ_REMOVE(&ch->io_memory_domain, bdev_io, internal.link);
			/* [한국어] 즉시 실패 시 추적 큐 등록 원복. */
			bdev_io_decrement_outstanding(ch, ch->shared_resource);
			/* [한국어] outstanding 원복. */
			if (rc != -ENOMEM) {
				/* [한국어] -ENOMEM 외 실패 로깅. */
				SPDK_ERRLOG("Failed to pull data from memory domain %s\n",
					    spdk_memory_domain_get_dma_device_id(
						    bdev_io->internal.memory_domain));
			}
		} else {
			/* [한국어] 일반 메모리: 동기 복사로 사용자 iovs → bounce 버퍼. */
			assert(bdev_io->u.bdev.iovcnt == 1);
			/* [한국어] bounce는 단일 세그먼트로 합쳐졌으므로 iovcnt==1. */
			spdk_copy_iovs_to_buf(bdev_io->u.bdev.iovs[0].iov_base,
					      bdev_io->u.bdev.iovs[0].iov_len,
					      bdev_io->internal.bounce_buf.orig_iovs,
					      bdev_io->internal.bounce_buf.orig_iovcnt);
			/* [한국어] 흩어진 원본 iov들을 bounce 단일 버퍼로 모음. */
		}
	}

	if (spdk_unlikely(rc == -ENOMEM)) {
		/* [한국어] 자원 부족 → nomem 큐에 PULL 단계로 적재해 재시도. */
		bdev_queue_nomem_io_head(ch->shared_resource, bdev_io, BDEV_IO_RETRY_STATE_PULL);
	} else {
		/* [한국어] 동기 완료(성공/오류) → pull 완료 후처리로. */
		bdev_io_pull_data_done(bdev_io, rc);
	}
}

static void
/*
 * [한국어]
 * _bdev_io_pull_bounce_data_buf - bounce 데이터 버퍼 컨텍스트를 설치하고 pull을 시작.
 *
 * @bdev_io: bounce 버퍼가 필요한 I/O.
 * @buf: 새로 마련한 (정렬된) bounce 데이터 버퍼.
 * @len: 버퍼 길이.
 * @cpl_cb: bounce 처리 완료 시 호출할 콜백.
 * @return: 없음(비동기).
 *
 * 사용자가 직접 제공한 (정렬 안 됐거나 메모리 도메인에 있는) 버퍼를 디바이스가 다룰 수 있는
 * 단일 정렬 bounce 버퍼로 치환한다. 원본 iov를 백업하고, I/O의 iov를 내장 bounce iov 하나로
 * 바꾼 뒤, 단일 iov가 되면서 split 조건이 바뀔 수 있으므로 split 플래그를 재평가한다.
 * nomem 큐가 차 있으면 순서 보존을 위해 꼬리에 줄 세우고, 아니면 즉시 pull한다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: _bdev_io_set_buf. callee: bdev_io_pull_data.
 *
 * 호출 체인:
 *   _bdev_io_set_buf → [_bdev_io_pull_bounce_data_buf] → bdev_io_pull_data
 */
static void
_bdev_io_pull_bounce_data_buf(struct spdk_bdev_io *bdev_io, void *buf, size_t len,
			      bdev_copy_bounce_buffer_cpl cpl_cb)
{
	struct spdk_bdev_shared_resource *shared_resource = bdev_io->internal.ch->shared_resource;
	/* [한국어] nomem 큐 상태 확인용 공유 자원. */

	assert(bdev_io->internal.f.has_bounce_buf == false);
	/* [한국어] 아직 bounce 버퍼가 설치되지 않은 상태에서만 진입(이중 설치 방지). */

	bdev_io->internal.data_transfer_cpl = cpl_cb;
	/* [한국어] bounce 처리 완료 시 호출할 콜백 저장. */
	bdev_io->internal.f.has_bounce_buf = true;
	/* [한국어] bounce 버퍼 컨텍스트 사용 중 표시. */
	/* save original iovec */
	bdev_io->internal.bounce_buf.orig_iovs = bdev_io->u.bdev.iovs;
	/* [한국어] 원본 사용자 iov 배열 백업(완료 시 복원/복사 대상). */
	bdev_io->internal.bounce_buf.orig_iovcnt = bdev_io->u.bdev.iovcnt;
	/* [한국어] 원본 세그먼트 개수 백업. */
	/* zero the other data members */
	bdev_io->internal.bounce_buf.iov.iov_base = NULL;
	/* [한국어] bounce 데이터 iov 초기화. */
	bdev_io->internal.bounce_buf.md_iov.iov_base = NULL;
	/* [한국어] bounce 메타 iov 초기화. */
	bdev_io->internal.bounce_buf.orig_md_iov.iov_base = NULL;
	/* [한국어] 원본 메타 iov 초기화. */
	/* set bounce iov */
	bdev_io->u.bdev.iovs = &bdev_io->internal.bounce_buf.iov;
	/* [한국어] I/O가 보는 iov를 내장 bounce iov 하나로 교체. */
	bdev_io->u.bdev.iovcnt = 1;
	/* [한국어] bounce는 단일 세그먼트. */
	/* set bounce buffer for this operation */
	bdev_io->u.bdev.iovs[0].iov_base = buf;
	/* [한국어] bounce 버퍼 주소 설정. */
	bdev_io->u.bdev.iovs[0].iov_len = len;
	/* [한국어] bounce 버퍼 길이 설정. */
	/* Now we use 1 iov, the split condition could have been changed */
	bdev_io->internal.f.split = bdev_io_should_split(bdev_io);
	/* [한국어] iov 개수가 1로 줄어 split 필요 여부가 달라졌을 수 있어 재평가. */

	if (spdk_unlikely(!TAILQ_EMPTY(&shared_resource->nomem_io))) {
		/* [한국어] nomem 큐가 차 있으면 순서 역전 방지를 위해 꼬리에 줄 세움. */
		bdev_queue_nomem_io_tail(shared_resource, bdev_io, BDEV_IO_RETRY_STATE_PULL);
	} else {
		/* [한국어] 큐가 비었으면 즉시 데이터 pull 진행. */
		bdev_io_pull_data(bdev_io);
	}
}

/*
 * [한국어]
 * _bdev_io_set_buf - iobuf 풀에서 받은 raw 버퍼를 정렬해 I/O에 연결하고 다음 단계로.
 *
 * @bdev_io: 버퍼를 받을 I/O.
 * @buf: iobuf 풀에서 받은 정렬되지 않은 raw 버퍼.
 * @len: 데이터 길이.
 * @return: 없음.
 *
 * 풀에서 받은 버퍼 주소를 bdev 요구 정렬에 맞춰 올림한 aligned_buf를 만든다. 사용자가 이미
 * 자기 버퍼를 제공했다면(buf_allocated) 그 데이터를 정렬된 bounce 버퍼로 옮기는 pull 경로로
 * 가고, 아니면 정렬된 버퍼를 그대로 I/O 버퍼로 설정한 뒤 메타데이터 단계로 진행한다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: bdev_io_get_buf 콜백.
 * callee: _bdev_io_pull_bounce_data_buf / spdk_bdev_io_set_buf / _bdev_io_set_md_buf.
 *
 * 호출 체인:
 *   iobuf get → [_bdev_io_set_buf] → bounce pull 또는 _bdev_io_set_md_buf
 */
static void
_bdev_io_set_buf(struct spdk_bdev_io *bdev_io, void *buf, uint64_t len)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	/* [한국어] 정렬 요구의 출처 bdev. */
	bool buf_allocated;
	/* [한국어] 사용자가 자기 버퍼를 제공했는지(true면 bounce 복사 필요). */
	uint64_t alignment;
	/* [한국어] bdev 버퍼 정렬 요구(바이트). */
	void *aligned_buf;
	/* [한국어] raw 버퍼를 정렬 경계로 올림한 사용 가능 주소. */

	bdev_io->internal.buf.ptr = buf;
	/* [한국어] 나중에 풀에 반납할 원본(미정렬) 버퍼 주소 보관. */
	bdev_io->internal.f.has_buf = true;
	/* [한국어] 버퍼 보유 표시(put 시점 판단용). */

	alignment = spdk_bdev_get_buf_align(bdev);
	/* [한국어] 이 bdev의 버퍼 정렬 요구 획득. */
	buf_allocated = _is_buf_allocated(bdev_io->u.bdev.iovs);
	/* [한국어] 사용자 버퍼 존재 여부 판정. */
	aligned_buf = (void *)(((uintptr_t)buf + (alignment - 1)) & ~(alignment - 1));
	/* [한국어] (addr + align-1) & ~(align-1) — 정렬 경계로 올림. */

	if (buf_allocated) {
		/* [한국어] 사용자 버퍼가 있으면 그 데이터를 정렬 bounce 버퍼로 pull. */
		_bdev_io_pull_bounce_data_buf(bdev_io, aligned_buf, len, _bdev_io_pull_buffer_cpl);
		/* Continue in completion callback */
		/* [한국어] pull은 비동기일 수 있으므로 완료 콜백에서 이어감. */
		return;
	} else {
		/* [한국어] 사용자 버퍼 없으면 정렬 버퍼를 그대로 I/O 버퍼로 설정. */
		spdk_bdev_io_set_buf(bdev_io, aligned_buf, len);
	}

	_bdev_io_set_md_buf(bdev_io);
	/* [한국어] 데이터 버퍼 준비 완료 → 메타데이터 버퍼 단계로. */
}

/*
 * [한국어]
 * bdev_io_get_max_buf_len - I/O 한 건에 필요한 최대 버퍼 길이(데이터+정렬여유+메타) 계산.
 *
 * @bdev_io: 대상 I/O.
 * @len: 순수 데이터 길이.
 * @return: iobuf 풀에서 요청할 총 버퍼 길이.
 *
 * iobuf 풀에서 충분한 크기를 요청하려면 데이터 길이 외에 (1) 정렬 보정을 위한 여유
 * (alignment-1 바이트)와 (2) 분리형 메타데이터 길이를 더해야 한다.
 * 실행 컨텍스트: I/O 버퍼 확보 hot-path.
 *
 * 호출 체인:
 *   bdev_io_get_buf → [bdev_io_get_max_buf_len] → spdk_iobuf_get
 */
static inline uint64_t
bdev_io_get_max_buf_len(struct spdk_bdev_io *bdev_io, uint64_t len)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	/* [한국어] 메타/정렬 정보 출처. */
	uint64_t md_len, alignment;
	/* [한국어] md_len: 메타데이터 추가분, alignment: 정렬 여유분. */

	md_len = spdk_bdev_is_md_separate(bdev) ? bdev_io->u.bdev.num_blocks * bdev->md_len : 0;
	/* [한국어] 분리형 메타데이터면 블록수×메타길이를 추가, 아니면 0. */

	/* 1 byte alignment needs 0 byte of extra space, 64 bytes alignment needs 63 bytes of extra space, etc. */
	alignment = spdk_bdev_get_buf_align(bdev) - 1;
	/* [한국어] 정렬 보정 최대 여유 = 정렬-1 바이트. */

	return len + alignment + md_len;
	/* [한국어] 데이터 + 정렬여유 + 메타 = 풀에서 요청할 총 길이. */
}

/*
 * [한국어]
 * bdev_io_put_accel_buf - accel 메모리 도메인에서 받은 버퍼를 accel 채널에 반납.
 *
 * @bdev_io: 버퍼를 보유한 I/O.
 * @return: 없음.
 *
 * 버퍼를 iobuf 풀이 아니라 accel framework(spdk_accel_get_buf)에서 받은 경우, 같은
 * accel 채널로 spdk_accel_put_buf 하여 반납해야 한다(메모리 도메인 일관성).
 * 실행 컨텍스트: 채널 소유 스레드. caller: bdev_io_put_buf.
 *
 * 호출 체인:
 *   bdev_io_put_buf → [bdev_io_put_accel_buf] → spdk_accel_put_buf
 */
static void
bdev_io_put_accel_buf(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] accel_channel 보유 채널. */

	spdk_accel_put_buf(ch->accel_channel,
			   bdev_io->internal.buf.ptr,
			   bdev_io->u.bdev.memory_domain,
			   bdev_io->u.bdev.memory_domain_ctx);
	/* [한국어] accel 채널로 버퍼 반납(메모리 도메인/ctx와 함께). */
}

/*
 * [한국어]
 * _bdev_io_put_buf - iobuf 풀에서 받은 버퍼를 풀에 반납.
 *
 * @bdev_io: 버퍼를 보유한 I/O.
 * @buf: 반납할 버퍼 주소(원본 미정렬 ptr).
 * @buf_len: 데이터 길이(반납 시 max_buf_len으로 환산).
 * @return: 없음.
 *
 * iobuf 풀에서 get한 버퍼를 동일 mgmt 채널의 iobuf로 반납한다. 반납 길이는 get 때와
 * 동일하게 정렬/메타를 포함한 max_buf_len이어야 풀 정합성이 유지된다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: bdev_io_put_buf.
 *
 * 호출 체인:
 *   bdev_io_put_buf → [_bdev_io_put_buf] → spdk_iobuf_put
 */
static void
_bdev_io_put_buf(struct spdk_bdev_io *bdev_io, void *buf, uint64_t buf_len)
{
	struct spdk_bdev_mgmt_channel *ch;
	/* [한국어] iobuf 풀을 보유한 per-thread mgmt 채널. */

	ch = bdev_io->internal.ch->shared_resource->mgmt_ch;
	/* [한국어] 공유 자원을 통해 이 스레드의 mgmt 채널 획득. */
	spdk_iobuf_put(&ch->iobuf, buf, bdev_io_get_max_buf_len(bdev_io, buf_len));
	/* [한국어] get 때와 동일한 길이(정렬+메타 포함)로 풀에 반납. */
}

/*
 * [한국어]
 * bdev_io_put_buf - I/O가 보유한 데이터 버퍼를 출처(accel/iobuf)에 맞게 반납.
 *
 * @bdev_io: 버퍼를 보유한 I/O.
 * @return: 없음.
 *
 * I/O 완료 후 더 이상 필요 없는 버퍼를 반납한다. 버퍼가 accel 메모리 도메인에서 왔으면
 * accel 채널로, 아니면 iobuf 풀로 반납한다. 반납 후 has_buf 플래그를 내려 이중 반납을 막는다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: I/O 완료 후처리.
 *
 * 호출 체인:
 *   완료 후처리 → [bdev_io_put_buf] → bdev_io_put_accel_buf / _bdev_io_put_buf
 */
static void
bdev_io_put_buf(struct spdk_bdev_io *bdev_io)
{
	assert(bdev_io->internal.f.has_buf);
	/* [한국어] 반납 대상 버퍼가 실제로 있어야 함. */

	if (bdev_io->u.bdev.memory_domain == spdk_accel_get_memory_domain()) {
		/* [한국어] 버퍼 출처가 accel 메모리 도메인이면 accel 채널로 반납. */
		bdev_io_put_accel_buf(bdev_io);
	} else {
		/* [한국어] 그 외에는 일반 iobuf 풀로 반납(메모리 도메인이 NULL이어야 함). */
		assert(bdev_io->u.bdev.memory_domain == NULL);
		_bdev_io_put_buf(bdev_io, bdev_io->internal.buf.ptr,
				 bdev_io->internal.buf.len);
	}
	bdev_io->internal.buf.ptr = NULL;
	/* [한국어] 버퍼 포인터 비움. */
	bdev_io->internal.f.has_buf = false;
	/* [한국어] 버퍼 미보유 표시(이중 반납 방지). */
}

/*
 * [한국어]
 * bdev_submit_request - bdev_io를 백엔드 모듈의 submit_request fn_table 콜백으로 전달(최종 dispatch).
 *
 * @bdev: 대상 bdev(모듈 fn_table 소유).
 * @ioch: 모듈에 넘길 io_channel(모듈측 채널 ctx).
 * @bdev_io: 제출할 I/O.
 * @return: 없음.
 *
 * QoS/split/nomem/lba_lock 게이트를 모두 통과한 I/O를 실제 백엔드 모듈에 넘기는 마지막 관문.
 * 모듈에 넘기는 순간 accel sequence의 소유권이 모듈로 이전되므로, bdev 레이어는 더 이상
 * 만지지 않도록 내부 보유 플래그를 내린다. 또한 모듈이 지원하지 않는 DIF 플래그가 새어
 * 나가지 않았는지 assert로 검증한다. 마지막으로 모듈의 submit_request를 호출한다 — 여기서
 * NVMe라면 SQE 빌드 + doorbell write, AIO라면 io_submit() 등이 일어난다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: bdev_io_do_submit / bdev_ch_resubmit_io.
 * callee: bdev->fn_table->submit_request.
 *
 * 호출 체인:
 *   bdev_io_do_submit → [bdev_submit_request] → 모듈 submit_request → 디바이스
 */
static inline void
bdev_submit_request(struct spdk_bdev *bdev, struct spdk_io_channel *ioch,
		    struct spdk_bdev_io *bdev_io)
{
	/* After a request is submitted to a bdev module, the ownership of an accel sequence
	 * associated with that bdev_io is transferred to the bdev module. So, clear the internal
	 * sequence pointer to make sure we won't touch it anymore. */
	if ((bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE ||
	     bdev_io->type == SPDK_BDEV_IO_TYPE_READ) && bdev_io->u.bdev.accel_sequence != NULL) {
		/* [한국어] R/W에 sequence가 붙어있으면 소유권이 모듈로 넘어감. */
		assert(!bdev_io_needs_sequence_exec(bdev_io));
		/* [한국어] 모듈에 넘기는 시점엔 bdev 레이어가 실행할 sequence가 남아있으면 안 됨. */
		bdev_io->internal.f.has_accel_sequence = false;
		/* [한국어] bdev 레이어 보유 플래그 해제(모듈이 책임지므로 더 안 만짐). */
	}

	/* The generic bdev layer should not pass an I/O with a dif_check_flags set that
	 * the underlying bdev does not support. Add an assert to check this.
	 */
	assert((bdev_io->type != SPDK_BDEV_IO_TYPE_WRITE &&
		bdev_io->type != SPDK_BDEV_IO_TYPE_READ) ||
	       ((bdev_io->u.bdev.dif_check_flags & bdev->dif_check_flags) ==
		bdev_io->u.bdev.dif_check_flags));
	/* [한국어] R/W일 때, 요청 DIF 플래그가 bdev 지원 플래그의 부분집합인지 검증
	 *  (모듈이 모르는 PI 검사를 요구하지 않도록). */

	bdev->fn_table->submit_request(ioch, bdev_io);
	/* [한국어] 백엔드 모듈에 최종 제출 — NVMe SQE+doorbell / libaio / memcpy 등 실제 수행. */
}

/*
 * [한국어]
 * bdev_ch_resubmit_io - nomem 큐에서 꺼낸 I/O를 모듈에 다시 제출(재시도 SUBMIT 단계).
 *
 * @shared_resource: I/O가 속한 공유 자원.
 * @bdev_io: 재제출할 I/O.
 * @return: 없음.
 *
 * -ENOMEM으로 한 번 거절됐던 I/O를 자원이 풀린 뒤 다시 모듈에 제출한다. 재제출이므로
 * outstanding을 다시 올리고, 이전 NVMe 완료 코드(cdw0)를 초기화하며, 재시도 횟수를 센다.
 * 실행 컨텍스트: 채널 소유 스레드(완료 컨텍스트). caller: bdev_shared_ch_retry_io.
 *
 * 호출 체인:
 *   bdev_shared_ch_retry_io → [bdev_ch_resubmit_io] → bdev_submit_request
 */
static inline void
bdev_ch_resubmit_io(struct spdk_bdev_shared_resource *shared_resource, struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	/* [한국어] 재제출 대상 bdev. */

	bdev_io_increment_outstanding(bdev_io->internal.ch, shared_resource);
	/* [한국어] 다시 진행 중 I/O가 되므로 outstanding 증가. */
	bdev_io->internal.error.nvme.cdw0 = 0;
	/* [한국어] 직전 시도의 NVMe 완료 dword0 초기화(새 결과로 덮어쓰기 위함). */
	bdev_io->num_retries++;
	/* [한국어] 재시도 횟수 카운트(통계/디버깅). */
	bdev_submit_request(bdev, spdk_bdev_io_get_io_channel(bdev_io), bdev_io);
	/* [한국어] 모듈에 재제출. */
}

/*
 * [한국어]
 * bdev_shared_ch_retry_io - nomem 큐에 대기 중인 I/O들을 자원이 풀린 만큼 재시도.
 *
 * @shared_resource: nomem 큐를 보유한 공유 자원.
 * @return: 없음.
 *
 * I/O 완료로 자원(outstanding)이 nomem_threshold 이하로 내려가면, nomem 큐 선두부터
 * 하나씩 꺼내 retry_state가 가리키는 단계(SUBMIT/PULL/PUSH/GET_ACCEL_BUF 등)부터 재개한다.
 * 핵심 안전장치: 꺼낸 I/O가 또 -ENOMEM이면 항상 큐 선두에 재삽입되므로, "방금 꺼낸 I/O가
 * 다시 선두에 있으면" 더 진전이 없다고 보고 루프를 중단해 무한 재시도를 막는다.
 * nomem_abort_in_progress 중에는 큐를 건드리지 않아 abort와의 경쟁을 피한다.
 * 실행 컨텍스트: 채널 소유 스레드(주로 완료 컨텍스트). caller: bdev_ch_retry_io / nomem_poller.
 * callee: 각 재시도 단계 함수.
 *
 * 호출 체인:
 *   완료/poller → [bdev_shared_ch_retry_io] → bdev_ch_resubmit_io / bdev_io_pull_data / ...
 */
static void
bdev_shared_ch_retry_io(struct spdk_bdev_shared_resource *shared_resource)
{
	struct spdk_bdev_io *bdev_io;
	/* [한국어] 큐에서 꺼낸 재시도 대상 I/O. */

	if (shared_resource->nomem_abort_in_progress) {
		/**
		 * We are aborting nomem I/Os, do not touch nomem_io list now.
		 */
		/* [한국어] abort가 nomem 큐를 정리 중이면 동시에 건드리지 않음(경쟁 방지). */
		return;
	}

	if (shared_resource->io_outstanding > shared_resource->nomem_threshold) {
		/*
		 * Allow some more I/O to complete before retrying the nomem_io queue.
		 *  Some drivers (such as nvme) cannot immediately take a new I/O in
		 *  the context of a completion, because the resources for the I/O are
		 *  not released until control returns to the bdev poller.  Also, we
		 *  may require several small I/O to complete before a larger I/O
		 *  (that requires splitting) can be submitted.
		 */
		/* [한국어] 아직 진행 I/O가 임계 수위보다 많으면 자원이 덜 풀린 것 → 더 기다림.
		 *  NVMe 등은 완료 컨텍스트에서 즉시 새 I/O를 못 받고 poller 복귀 후에야 tracker가
		 *  풀리며, split I/O는 여러 작은 I/O가 빠져야 들어갈 자리가 생긴다. */
		return;
	}

	while (!TAILQ_EMPTY(&shared_resource->nomem_io)) {
		/* [한국어] nomem 큐가 빌 때까지(또는 진전이 멈출 때까지) 재시도. */
		bdev_io = TAILQ_FIRST(&shared_resource->nomem_io);
		/* [한국어] 큐 선두 I/O 선택(가장 오래 대기). */
		TAILQ_REMOVE(&shared_resource->nomem_io, bdev_io, internal.link);
		/* [한국어] 일단 큐에서 떼어냄(재시도 중 또 -ENOMEM이면 다시 선두에 삽입됨). */

		switch (bdev_io->internal.retry_state) {
		/* [한국어] 적재 시 기록해 둔 단계부터 재개. */
		case BDEV_IO_RETRY_STATE_SUBMIT:
			/* [한국어] 모듈 제출 단계에서 거절됐던 I/O → 다시 제출. */
			bdev_ch_resubmit_io(shared_resource, bdev_io);
			break;
		case BDEV_IO_RETRY_STATE_PULL:
			/* [한국어] 데이터 pull 단계에서 거절 → pull 재개. */
			bdev_io_pull_data(bdev_io);
			break;
		case BDEV_IO_RETRY_STATE_PULL_MD:
			/* [한국어] 메타 pull 단계에서 거절 → 메타 pull 재개. */
			bdev_io_pull_md_buf(bdev_io);
			break;
		case BDEV_IO_RETRY_STATE_PUSH:
			/* [한국어] 데이터 push(bounce→사용자) 단계에서 거절 → push 재개. */
			bdev_io_push_bounce_data(bdev_io);
			break;
		case BDEV_IO_RETRY_STATE_PUSH_MD:
			/* [한국어] 메타 push 단계에서 거절 → 메타 push 재개. */
			bdev_io_push_bounce_md_buf(bdev_io);
			break;
		case BDEV_IO_RETRY_STATE_GET_ACCEL_BUF:
			/* [한국어] accel 버퍼 확보 단계에서 거절 → 버퍼 재확보. */
			_bdev_io_get_accel_buf(bdev_io);
			break;
		default:
			/* [한국어] 도달 불가 — 잘못된 재시도 상태. */
			assert(0 && "invalid retry state");
			break;
		}

		if (bdev_io == TAILQ_FIRST(&shared_resource->nomem_io)) {
			/* This IO completed again with NOMEM status, so break the loop and
			 * don't try anymore.  Note that a bdev_io that fails with NOMEM
			 * always gets requeued at the front of the list, to maintain
			 * ordering.
			 */
			/* [한국어] 방금 재시도한 I/O가 또 -ENOMEM으로 선두에 되돌아왔다면 더 진전 없음
			 *  → 루프 중단(무한 재시도 방지, 순서 보존). */
			break;
		}
	}
}

/*
 * [한국어]
 * bdev_ch_retry_io - 채널의 공유 자원에 대해 nomem 재시도를 트리거하는 얇은 래퍼.
 *
 * @bdev_ch: 재시도를 시도할 채널.
 * @return: 없음.
 *
 * 채널은 자신의 shared_resource를 통해 nomem 큐를 공유하므로, 단순히 공유 자원 단위
 * 재시도 함수로 위임한다.
 * 실행 컨텍스트: 채널 소유 스레드. callee: bdev_shared_ch_retry_io.
 *
 * 호출 체인:
 *   완료 후처리 → [bdev_ch_retry_io] → bdev_shared_ch_retry_io
 */
static void
bdev_ch_retry_io(struct spdk_bdev_channel *bdev_ch)
{
	bdev_shared_ch_retry_io(bdev_ch->shared_resource);
	/* [한국어] 채널의 공유 자원 단위로 nomem 재시도 위임. */
}

/*
 * [한국어]
 * bdev_no_mem_poller - outstanding이 0인데 nomem 대기가 남은 교착을 푸는 안전망 poller.
 *
 * @ctx: poller 컨텍스트(= shared_resource).
 * @return: SPDK_POLLER_BUSY(계속 등록) 또는 SPDK_POLLER_IDLE(해제됨).
 *
 * 보통 nomem 재시도는 "다른 I/O의 완료"가 트리거한다. 그러나 queue depth가 1이거나 새
 * I/O 제출이 없는 상황에서는 트리거가 영영 오지 않아 nomem 큐가 멈출 수 있다. 이 poller는
 * 그런 교착(outstanding==0인데 nomem_io 비어있지 않음)을 주기적으로 깨워 재시도시킨다.
 * 큐가 비거나 outstanding이 생기면 스스로 등록 해제한다.
 * 실행 컨텍스트: 채널 소유 스레드(poller). caller: SPDK thread poller 루프.
 *
 * 호출 체인:
 *   thread poller → [bdev_no_mem_poller] → bdev_shared_ch_retry_io
 */
static int
bdev_no_mem_poller(void *ctx)
{
	struct spdk_bdev_shared_resource *shared_resource = ctx;
	/* [한국어] poller가 감시할 공유 자원. */

	if (!TAILQ_EMPTY(&shared_resource->nomem_io)) {
		/* [한국어] 대기 I/O가 있으면 재시도 시도. */
		bdev_shared_ch_retry_io(shared_resource);
	}

	/* Keep poller registered if list is not empty and there are no io outstanding. */
	if (!TAILQ_EMPTY(&shared_resource->nomem_io) && shared_resource->io_outstanding == 0) {
		/* [한국어] 여전히 대기 + outstanding 0(자체 트리거 없음)이면 poller 유지. */
		return SPDK_POLLER_BUSY;
	}

	spdk_poller_unregister(&shared_resource->nomem_poller);
	/* [한국어] 교착 해소(큐 비었거나 outstanding 생김) → poller 해제. */
	return SPDK_POLLER_IDLE;
	/* [한국어] 더 할 일 없음 통지. */
}

/*
 * [한국어]
 * _bdev_io_handle_no_mem - 완료된 I/O가 NOMEM이면 nomem 큐에 적재, 아니면 대기분 재시도.
 *
 * @bdev_io: 방금 모듈이 완료(또는 거절)한 I/O.
 * @state: NOMEM일 때 재시도를 재개할 단계.
 * @return: NOMEM이라 큐잉했으면 true(완료 처리 중단), 아니면 false(정상 완료 진행).
 *
 * 모듈이 -ENOMEM/NOMEM 상태로 돌려준 I/O는 실패가 아니라 "지금은 자원이 없으니 나중에"의
 * 의미다. 이 함수는 그 I/O를 PENDING으로 되돌려 nomem 큐 선두에 적재하고, qd==1 같은
 * 교착 상황을 대비해 안전망 poller를 등록한다. 또한 NOMEM과 함께 돌아온 accel sequence의
 * 소유권을 bdev 레이어로 되찾아 둔다(추후 abort 시 올바르게 정리하기 위함). NOMEM이
 * 아니면, 자원이 막 풀렸을 수 있으므로 큐에 대기 중인 I/O들의 재시도를 한 번 깨운다.
 * 실행 컨텍스트: 채널 소유 스레드(완료 컨텍스트). caller: bdev_io_complete 계열.
 *
 * 호출 체인:
 *   완료 경로 → [_bdev_io_handle_no_mem] → bdev_queue_nomem_io_head / bdev_ch_retry_io
 */
static inline bool
_bdev_io_handle_no_mem(struct spdk_bdev_io *bdev_io, enum bdev_io_retry_state state)
{
	struct spdk_bdev_channel *bdev_ch = bdev_io->internal.ch;
	/* [한국어] 완료된 I/O의 채널. */
	struct spdk_bdev_shared_resource *shared_resource = bdev_ch->shared_resource;
	/* [한국어] nomem 큐를 보유한 공유 자원. */

	if (spdk_unlikely(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM)) {
		/* [한국어] 모듈이 자원 부족으로 NOMEM 반환 → 실패가 아니라 재시도 대상. */
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_PENDING;
		/* [한국어] 상태를 PENDING으로 되돌려 "아직 미완료"로 표시. */
		bdev_queue_nomem_io_head(shared_resource, bdev_io, state);
		/* [한국어] nomem 큐 선두에 적재(주어진 단계부터 재시도). */

		if (shared_resource->io_outstanding == 0 && !shared_resource->nomem_poller) {
			/* Special case when we have nomem IOs and no outstanding IOs which completions
			 * could trigger retry of queued IOs
			 * Any IOs submitted may trigger retry of queued IOs. This poller handles a case when no
			 * new IOs submitted, e.g. qd==1 */
			/* [한국어] outstanding이 0이라 재시도를 트리거할 완료가 없을 교착(qd==1 등) →
			 *  안전망 poller를 등록해 주기적으로 재시도시킴. */
			shared_resource->nomem_poller = SPDK_POLLER_REGISTER(bdev_no_mem_poller, shared_resource,
							10 * SPDK_MSEC_TO_USEC);
			/* [한국어] 10ms 주기 poller 등록. */
		}
		/* If bdev module completed an I/O that has an accel sequence with NOMEM status, the
		 * ownership of that sequence is transferred back to the bdev layer, so we need to
		 * restore internal.accel_sequence to make sure that the sequence is handled
		 * correctly in case the I/O is later aborted. */
		if ((bdev_io->type == SPDK_BDEV_IO_TYPE_READ ||
		     bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) && bdev_io->u.bdev.accel_sequence) {
			/* [한국어] NOMEM으로 돌아온 R/W에 sequence가 붙어있으면 소유권이 bdev로 복귀. */
			assert(!bdev_io_use_accel_sequence(bdev_io));
			bdev_io->internal.f.has_accel_sequence = true;
			/* [한국어] bdev 레이어 보유 플래그 복원. */
			bdev_io->internal.accel_sequence = bdev_io->u.bdev.accel_sequence;
			/* [한국어] 내부 sequence 포인터 복원(추후 abort 시 정리에 사용). */
		}

		return true;
		/* [한국어] NOMEM 처리 완료 — 호출자는 정상 완료 후처리를 중단해야 함. */
	}

	if (spdk_unlikely(!TAILQ_EMPTY(&shared_resource->nomem_io))) {
		/* [한국어] 이 I/O는 정상 완료지만, 자원이 풀렸으니 대기 I/O 재시도를 깨움. */
		bdev_ch_retry_io(bdev_ch);
	}

	return false;
	/* [한국어] NOMEM 아님 — 호출자는 정상 완료 후처리를 계속. */
}

/*
 * [한국어]
 * _bdev_io_complete_push_bounce_done - bounce 데이터를 사용자 버퍼로 push 완료 후 최종 완료로 진입.
 *
 * @ctx: 콜백 컨텍스트(= bdev_io).
 * @rc: push 결과(0=성공).
 * @return: 없음.
 *
 * READ 완료 시 bounce 버퍼의 데이터를 사용자 버퍼로 옮기는 push가 끝나면 호출된다. 실패면
 * I/O를 FAILED로 표시한다. 이 시점에 bounce 버퍼는 역할을 다했으므로 즉시 반납하고(나중에
 * spdk_bdev_free_io의 조건부 free를 기다리지 않음), 자원이 풀렸으니 nomem 재시도를 깨운 뒤,
 * 본래의 I/O 완료 흐름(bdev_io_complete)으로 들어간다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: push 단계의 data_transfer_cpl.
 *
 * 호출 체인:
 *   push 완료 → [_bdev_io_complete_push_bounce_done] → bdev_io_put_buf → bdev_io_complete
 */
static void
_bdev_io_complete_push_bounce_done(void *ctx, int rc)
{
	struct spdk_bdev_io *bdev_io = ctx;
	/* [한국어] 콜백 ctx 복원. */
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] I/O 채널. */

	if (rc) {
		/* [한국어] push(사용자 버퍼로 복사) 실패 → I/O 실패로 마킹. */
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	}
	/* We want to free the bounce buffer here since we know we're done with it (as opposed
	 * to waiting for the conditional free of internal.buf.ptr in spdk_bdev_free_io()).
	 */
	bdev_io_put_buf(bdev_io);
	/* [한국어] bounce 버퍼는 임무 종료 → 즉시 반납(free_io까지 미루지 않음). */

	if (spdk_unlikely(!TAILQ_EMPTY(&ch->shared_resource->nomem_io))) {
		/* [한국어] 버퍼 반납으로 자원이 풀렸으니 nomem 재시도를 깨움. */
		bdev_ch_retry_io(ch);
	}

	/* Continue with IO completion flow */
	bdev_io_complete(bdev_io);
	/* [한국어] 본래의 완료 흐름(통계/콜백/free)으로 진입. */
}

/*
 * [한국어]
 * bdev_io_push_bounce_md_buf_done - 메타데이터 bounce push(memory domain) 완료 콜백.
 *
 * @ctx: 콜백 컨텍스트(= bdev_io).
 * @rc: push 결과.
 * @return: 없음.
 *
 * READ에서 bounce 메타 버퍼의 내용을 사용자 메타 버퍼로 옮기는 비동기 push가 끝나면 호출된다.
 * 추적 큐 정리, outstanding 감소, bounce 컨텍스트 해제, nomem 재시도 후 data_transfer_cpl로
 * 다음 단계를 이어간다.
 * 실행 컨텍스트: memory domain 완료 컨텍스트(동일 채널 스레드).
 *
 * 호출 체인:
 *   memory domain push 완료 → [bdev_io_push_bounce_md_buf_done] → data_transfer_cpl
 */
static void
bdev_io_push_bounce_md_buf_done(void *ctx, int rc)
{
	struct spdk_bdev_io *bdev_io = ctx;
	/* [한국어] 콜백 ctx 복원. */
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] I/O 채널. */

	TAILQ_REMOVE(&ch->io_memory_domain, bdev_io, internal.link);
	/* [한국어] memory-domain 추적 큐에서 제거. */
	bdev_io_decrement_outstanding(ch, ch->shared_resource);
	/* [한국어] push outstanding 감소. */
	bdev_io->internal.f.has_bounce_buf = false;
	/* [한국어] 메타까지 push 완료 → bounce 컨텍스트 해제. */

	if (spdk_unlikely(!TAILQ_EMPTY(&ch->shared_resource->nomem_io))) {
		/* [한국어] 자원 풀림 + 대기 있으면 재시도. */
		bdev_ch_retry_io(ch);
	}

	bdev_io->internal.data_transfer_cpl(bdev_io, rc);
	/* [한국어] 다음 단계 콜백으로 진행. */
}

/*
 * [한국어]
 * bdev_io_push_bounce_md_buf - READ 완료 후 bounce 메타 버퍼를 사용자 메타 버퍼로 push.
 *
 * @bdev_io: 메타 push가 필요할 수 있는 완료된 READ I/O.
 * @return: 없음(memory domain 경로는 콜백으로 이어짐).
 *
 * 분리형 메타데이터 READ에서 디바이스는 bounce 메타 버퍼로 읽었으므로, 그 내용을 사용자
 * 메타 버퍼로 되돌려야 한다. memory domain이면 비동기 push, 일반이면 memcpy. -ENOMEM이면
 * PUSH_MD 단계로 재시도 큐잉. 메타 처리가 필요 없으면(또는 끝나면) bounce를 해제하고
 * data_transfer_cpl로 완료를 이어간다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: bdev_io_push_bounce_data_done.
 *
 * 호출 체인:
 *   bdev_io_push_bounce_data_done → [bdev_io_push_bounce_md_buf] → memory_domain_push/memcpy
 */
static inline void
bdev_io_push_bounce_md_buf(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] I/O 채널. */
	int rc = 0;
	/* [한국어] push 결과 코드. */

	assert(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	/* [한국어] push는 성공한 I/O의 완료 경로에서만 수행. */
	assert(bdev_io->internal.f.has_bounce_buf);
	/* [한국어] bounce 컨텍스트가 살아있어야 함. */

	/* do the same for metadata buffer */
	if (spdk_unlikely(bdev_io->internal.bounce_buf.orig_md_iov.iov_base != NULL)) {
		/* [한국어] 사용자 원본 메타 버퍼가 백업돼 있으면(분리형 메타) 메타 push 필요. */
		assert(spdk_bdev_is_md_separate(bdev_io->bdev));

		if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
			/* [한국어] READ만 디바이스가 읽은 메타를 사용자 버퍼로 되돌림. */
			if (bdev_io_use_memory_domain(bdev_io)) {
				/* [한국어] 외부 memory domain이면 비동기 push. */
				TAILQ_INSERT_TAIL(&ch->io_memory_domain, bdev_io, internal.link);
				/* [한국어] 추적 큐 등록. */
				bdev_io_increment_outstanding(ch, ch->shared_resource);
				/* [한국어] push outstanding 증가. */
				/* If memory domain is used then we need to call async push function */
				rc = spdk_memory_domain_push_data(bdev_io->internal.memory_domain,
								  bdev_io->internal.memory_domain_ctx,
								  &bdev_io->internal.bounce_buf.orig_md_iov,
								  (uint32_t)bdev_io->internal.bounce_buf.orig_iovcnt,
								  &bdev_io->internal.bounce_buf.md_iov, 1,
								  bdev_io_push_bounce_md_buf_done,
								  bdev_io);
				/* [한국어] bounce 메타(md_iov) → 사용자 메타(orig_md_iov) 비동기 push. */
				if (rc == 0) {
					/* Continue IO completion in async callback */
					/* [한국어] 비동기 시작 성공 → 콜백에서 이어감. */
					return;
				}
				TAILQ_REMOVE(&ch->io_memory_domain, bdev_io, internal.link);
				/* [한국어] 즉시 실패 시 추적 큐 원복. */
				bdev_io_decrement_outstanding(ch, ch->shared_resource);
				/* [한국어] outstanding 원복. */
				if (rc != -ENOMEM) {
					/* [한국어] -ENOMEM 외 실패 로깅. */
					SPDK_ERRLOG("Failed to push md to memory domain %s\n",
						    spdk_memory_domain_get_dma_device_id(
							    bdev_io->internal.memory_domain));
				}
			} else {
				/* [한국어] 일반 메모리: bounce 메타 → 사용자 메타 동기 memcpy. */
				memcpy(bdev_io->internal.bounce_buf.orig_md_iov.iov_base, bdev_io->u.bdev.md_buf,
				       bdev_io->internal.bounce_buf.orig_md_iov.iov_len);
			}
		}
	}

	if (spdk_unlikely(rc == -ENOMEM)) {
		/* [한국어] 자원 부족 → PUSH_MD 단계로 재시도 큐잉. */
		bdev_queue_nomem_io_head(ch->shared_resource, bdev_io, BDEV_IO_RETRY_STATE_PUSH_MD);
	} else {
		/* [한국어] 동기 완료 → bounce 해제 후 다음 단계 콜백. */
		assert(bdev_io->internal.data_transfer_cpl);
		bdev_io->internal.f.has_bounce_buf = false;
		/* [한국어] 모든 push 완료 → bounce 컨텍스트 해제. */
		bdev_io->internal.data_transfer_cpl(bdev_io, rc);
	}
}

/*
 * [한국어]
 * bdev_io_push_bounce_data_done - 데이터 push 완료 후 원본 iov 복원 및 메타 push로 진행.
 *
 * @bdev_io: 대상 I/O.
 * @rc: 데이터 push 결과.
 * @return: 없음.
 *
 * 데이터를 사용자 버퍼로 되돌리는 push가 끝나면, I/O의 iov를 다시 원본 사용자 iov로 복원해
 * 사용자에게 올바른 버퍼가 보이게 한 뒤 메타데이터 push 단계로 넘어간다. (메타 push가
 * 남았으므로 여기서는 bounce 컨텍스트를 해제하지 않는다.)
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인:
 *   push 완료 → [bdev_io_push_bounce_data_done] → bdev_io_push_bounce_md_buf
 */
static inline void
bdev_io_push_bounce_data_done(struct spdk_bdev_io *bdev_io, int rc)
{
	assert(bdev_io->internal.data_transfer_cpl);
	/* [한국어] 다음 단계 콜백이 등록돼 있어야 함. */
	if (rc) {
		/* [한국어] push 실패 → 즉시 완료 콜백으로 오류 전파. */
		bdev_io->internal.data_transfer_cpl(bdev_io, rc);
		return;
	}

	/* set original buffer for this io */
	bdev_io->u.bdev.iovcnt = bdev_io->internal.bounce_buf.orig_iovcnt;
	/* [한국어] 사용자에게 보일 iov 개수를 원본으로 복원. */
	bdev_io->u.bdev.iovs = bdev_io->internal.bounce_buf.orig_iovs;
	/* [한국어] iov 배열을 원본 사용자 배열로 복원. */

	/* We don't set bdev_io->internal.f.has_bounce_buf to false here because
	 * we still need to clear the md buf */
	/* [한국어] 메타 push가 남았으므로 bounce 컨텍스트는 아직 유지. */

	bdev_io_push_bounce_md_buf(bdev_io);
	/* [한국어] 메타데이터 push 단계로 진행. */
}

/*
 * [한국어]
 * bdev_io_push_bounce_data_done_and_track - 데이터 push(memory domain) 완료 콜백(추적 정리 포함).
 *
 * @ctx: 콜백 컨텍스트(= bdev_io).
 * @status: push 결과.
 * @return: 없음.
 *
 * 외부 memory domain으로의 비동기 데이터 push가 끝나면 추적 큐 정리·outstanding 감소·
 * nomem 재시도 후 공통 후처리(bdev_io_push_bounce_data_done)로 진행한다.
 * 실행 컨텍스트: memory domain 완료 컨텍스트(동일 채널 스레드).
 *
 * 호출 체인:
 *   memory domain push 완료 → [bdev_io_push_bounce_data_done_and_track] → bdev_io_push_bounce_data_done
 */
static void
bdev_io_push_bounce_data_done_and_track(void *ctx, int status)
{
	struct spdk_bdev_io *bdev_io = ctx;
	/* [한국어] 콜백 ctx 복원. */
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] I/O 채널. */

	TAILQ_REMOVE(&ch->io_memory_domain, bdev_io, internal.link);
	/* [한국어] 추적 큐에서 제거. */
	bdev_io_decrement_outstanding(ch, ch->shared_resource);
	/* [한국어] push outstanding 감소. */

	if (spdk_unlikely(!TAILQ_EMPTY(&ch->shared_resource->nomem_io))) {
		/* [한국어] 자원 풀림 + 대기 있으면 재시도. */
		bdev_ch_retry_io(ch);
	}

	bdev_io_push_bounce_data_done(bdev_io, status);
	/* [한국어] 공통 push 완료 후처리로 진행. */
}

/*
 * [한국어]
 * bdev_io_push_bounce_data - READ 완료 후 bounce 데이터 버퍼를 사용자 버퍼로 push.
 *
 * @bdev_io: 완료된 READ I/O.
 * @return: 없음(memory domain 경로는 콜백으로 이어짐).
 *
 * 디바이스가 bounce 버퍼로 읽은 데이터를 사용자 원본 버퍼로 되돌린다. memory domain이면
 * 비동기 push, 일반이면 spdk_copy_buf_to_iovs로 동기 복사. -ENOMEM이면 PUSH 단계로
 * 재시도 큐잉. WRITE는 push가 필요 없으므로(이미 디바이스로 나감) 분기되지 않는다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: _bdev_io_push_bounce_data_buffer / 재시도.
 *
 * 호출 체인:
 *   완료 경로 → [bdev_io_push_bounce_data] → memory_domain_push / spdk_copy_buf_to_iovs
 */
static inline void
bdev_io_push_bounce_data(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] I/O 채널. */
	int rc = 0;
	/* [한국어] push 결과 코드. */

	assert(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	/* [한국어] 성공한 I/O의 완료 경로에서만 push. */
	assert(!bdev_io_use_accel_sequence(bdev_io));
	/* [한국어] 이 경로는 accel sequence와 함께 쓰이지 않음. */
	assert(bdev_io->internal.f.has_bounce_buf);
	/* [한국어] bounce 컨텍스트가 살아있어야 함. */

	/* if this is read path, copy data from bounce buffer to original buffer */
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		/* [한국어] READ만 bounce → 사용자 버퍼로 데이터를 되돌림(WRITE는 불필요). */
		if (bdev_io_use_memory_domain(bdev_io)) {
			/* [한국어] 외부 memory domain이면 비동기 push. */
			TAILQ_INSERT_TAIL(&ch->io_memory_domain, bdev_io, internal.link);
			/* [한국어] 추적 큐 등록. */
			bdev_io_increment_outstanding(ch, ch->shared_resource);
			/* [한국어] push outstanding 증가. */
			/* If memory domain is used then we need to call async push function */
			rc = spdk_memory_domain_push_data(bdev_io->internal.memory_domain,
							  bdev_io->internal.memory_domain_ctx,
							  bdev_io->internal.bounce_buf.orig_iovs,
							  (uint32_t)bdev_io->internal.bounce_buf.orig_iovcnt,
							  &bdev_io->internal.bounce_buf.iov, 1,
							  bdev_io_push_bounce_data_done_and_track,
							  bdev_io);
			/* [한국어] bounce 데이터(iov) → 사용자 원본(orig_iovs) 비동기 push. */
			if (rc == 0) {
				/* Continue IO completion in async callback */
				/* [한국어] 비동기 시작 성공 → 콜백에서 이어감. */
				return;
			}

			TAILQ_REMOVE(&ch->io_memory_domain, bdev_io, internal.link);
			/* [한국어] 즉시 실패 시 추적 큐 원복. */
			bdev_io_decrement_outstanding(ch, ch->shared_resource);
			/* [한국어] outstanding 원복. */
			if (rc != -ENOMEM) {
				/* [한국어] -ENOMEM 외 실패 로깅. */
				SPDK_ERRLOG("Failed to push data to memory domain %s\n",
					    spdk_memory_domain_get_dma_device_id(
						    bdev_io->internal.memory_domain));
			}
		} else {
			/* [한국어] 일반 메모리: bounce 단일 버퍼 → 흩어진 사용자 iov들로 동기 복사. */
			spdk_copy_buf_to_iovs(bdev_io->internal.bounce_buf.orig_iovs,
					      bdev_io->internal.bounce_buf.orig_iovcnt,
					      bdev_io->internal.bounce_buf.iov.iov_base,
					      bdev_io->internal.bounce_buf.iov.iov_len);
		}
	}

	if (spdk_unlikely(rc == -ENOMEM)) {
		/* [한국어] 자원 부족 → PUSH 단계로 재시도 큐잉. */
		bdev_queue_nomem_io_head(ch->shared_resource, bdev_io, BDEV_IO_RETRY_STATE_PUSH);
	} else {
		/* [한국어] 동기 완료 → 데이터 push 후처리로. */
		bdev_io_push_bounce_data_done(bdev_io, rc);
	}
}

/*
 * [한국어]
 * _bdev_io_push_bounce_data_buffer - 완료 콜백을 설정하고 bounce 데이터 push를 시작하는 진입점.
 *
 * @bdev_io: 대상 I/O.
 * @cpl_cb: push 체인이 모두 끝났을 때 호출할 콜백.
 * @return: 없음.
 *
 * push 단계 진입의 얇은 래퍼 — data_transfer_cpl을 설정한 뒤 데이터 push를 트리거한다.
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인:
 *   완료 후처리 → [_bdev_io_push_bounce_data_buffer] → bdev_io_push_bounce_data
 */
static inline void
_bdev_io_push_bounce_data_buffer(struct spdk_bdev_io *bdev_io, bdev_copy_bounce_buffer_cpl cpl_cb)
{
	bdev_io->internal.data_transfer_cpl = cpl_cb;
	/* [한국어] push 체인 완료 시 호출할 콜백 저장. */
	bdev_io_push_bounce_data(bdev_io);
	/* [한국어] 데이터 push 시작. */
}

/*
 * [한국어]
 * bdev_io_get_iobuf_cb - iobuf 풀에서 버퍼가 준비되면 호출되는 콜백(대기→재개).
 *
 * @iobuf: 풀이 콜백에 넘기는 iobuf entry(bdev_io에 내장됨).
 * @buf: 풀이 할당해 준 버퍼.
 * @return: 없음.
 *
 * iobuf 풀에 즉시 버퍼가 없어 대기에 들어갔던 I/O는, 버퍼가 반납되어 가용해지면 이 콜백으로
 * 재개된다. entry로부터 container_of 패턴으로 bdev_io를 복원하고 _bdev_io_set_buf로 이어간다.
 * 실행 컨텍스트: 채널 소유 스레드(풀 반납 컨텍스트). caller: spdk_iobuf 풀.
 *
 * 호출 체인:
 *   iobuf put(타 I/O) → [bdev_io_get_iobuf_cb] → _bdev_io_set_buf
 */
static void
bdev_io_get_iobuf_cb(struct spdk_iobuf_entry *iobuf, void *buf)
{
	struct spdk_bdev_io *bdev_io;
	/* [한국어] entry로부터 복원할 bdev_io. */

	bdev_io = SPDK_CONTAINEROF(iobuf, struct spdk_bdev_io, internal.iobuf);
	/* [한국어] 내장 iobuf entry 주소에서 바깥 bdev_io 주소 역산(container_of). */
	_bdev_io_set_buf(bdev_io, buf, bdev_io->internal.buf.len);
	/* [한국어] 받은 버퍼로 버퍼 설정 절차 재개. */
}

/*
 * [한국어]
 * bdev_io_get_buf - iobuf 풀에서 I/O용 데이터 버퍼를 확보(즉시 또는 대기).
 *
 * @bdev_io: 버퍼가 필요한 I/O.
 * @len: 순수 데이터 길이.
 * @return: 없음(버퍼가 즉시 없으면 콜백으로 비동기 재개).
 *
 * 데이터 + 정렬여유 + 메타를 합한 max_len을 풀에 요청한다. 요청 길이가 풀의 large 버퍼
 * 크기를 넘으면 곧바로 실패 통지한다. 풀에 즉시 버퍼가 있으면 _bdev_io_set_buf로 이어가고,
 * 없으면 spdk_iobuf_get이 대기 entry를 등록해 두고 NULL을 반환하며, 나중에 버퍼가 반납되면
 * bdev_io_get_iobuf_cb로 재개된다.
 * 실행 컨텍스트: 채널 소유 스레드(반드시 I/O 소유 스레드여야 함 — assert). caller: get_buf 계열.
 * callee: spdk_iobuf_get.
 *
 * 호출 체인:
 *   spdk_bdev_io_get_buf 등 → [bdev_io_get_buf] → spdk_iobuf_get → _bdev_io_set_buf
 */
static void
bdev_io_get_buf(struct spdk_bdev_io *bdev_io, uint64_t len)
{
	struct spdk_bdev_mgmt_channel *mgmt_ch;
	/* [한국어] iobuf 풀을 보유한 per-thread mgmt 채널. */
	uint64_t max_len;
	/* [한국어] 풀에 요청할 총 길이(데이터+정렬+메타). */
	void *buf;
	/* [한국어] 풀이 즉시 줄 수 있는 버퍼(없으면 NULL). */

	assert(spdk_bdev_io_get_thread(bdev_io) == spdk_get_thread());
	/* [한국어] 버퍼 확보는 반드시 I/O를 소유한 스레드에서(lockless 풀 접근). */
	mgmt_ch = bdev_io->internal.ch->shared_resource->mgmt_ch;
	/* [한국어] 이 스레드의 mgmt 채널 획득. */
	max_len = bdev_io_get_max_buf_len(bdev_io, len);
	/* [한국어] 정렬/메타 포함 총 길이 계산. */

	if (spdk_unlikely(max_len > mgmt_ch->iobuf.cache[0].large.bufsize)) {
		/* [한국어] 풀의 최대(large) 버퍼보다 크면 수용 불가 → 즉시 실패. */
		SPDK_ERRLOG("Length %" PRIu64 " is larger than allowed\n", max_len);
		bdev_io_get_buf_complete(bdev_io, false);
		return;
	}

	bdev_io->internal.buf.len = len;
	/* [한국어] 순수 데이터 길이 기록(반납 시 재계산에 사용). */
	buf = spdk_iobuf_get(&mgmt_ch->iobuf, max_len, &bdev_io->internal.iobuf,
			     bdev_io_get_iobuf_cb);
	/* [한국어] 풀에서 버퍼 요청 — 없으면 내장 entry를 대기 등록하고 NULL 반환. */
	if (buf != NULL) {
		/* [한국어] 즉시 받았으면 버퍼 설정 절차로 진행. */
		_bdev_io_set_buf(bdev_io, buf, len);
	}
}

/*
 * [한국어]
 * spdk_bdev_io_get_buf - 모듈이 I/O 처리에 필요한 정렬된 데이터 버퍼를 요청하는 공개 API.
 *
 * @bdev_io: 버퍼가 필요한 I/O.
 * @cb: 버퍼 준비 완료 시 호출할 콜백.
 * @len: 필요한 데이터 길이.
 * @return: 없음(콜백으로 결과 통지).
 *
 * bdev 모듈(예: malloc, crypto vbdev)이 submit_request 콜백 안에서 "이 I/O를 처리하려면
 * 정렬된 버퍼가 필요하다"고 요청할 때 쓴다. 이미 사용자 버퍼가 있고 정렬도 맞으면 즉시
 * 콜백을 호출하고(추가 할당 없음), 아니면 iobuf 풀에서 버퍼를 확보해 (필요 시 bounce 복사)
 * 준비되면 콜백을 호출한다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: bdev 모듈. callee: bdev_io_get_buf.
 *
 * 호출 체인:
 *   모듈 submit_request → [spdk_bdev_io_get_buf] → (즉시 cb) 또는 bdev_io_get_buf
 */
void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	/* [한국어] 정렬 요구의 출처. */
	uint64_t alignment;
	/* [한국어] bdev 버퍼 정렬 요구. */

	assert(cb != NULL);
	/* [한국어] 완료 콜백은 필수. */
	bdev_io->internal.get_buf_cb = cb;
	/* [한국어] 버퍼 준비 시 호출할 콜백 저장. */

	alignment = spdk_bdev_get_buf_align(bdev);
	/* [한국어] 이 bdev의 정렬 요구 획득. */

	if (_is_buf_allocated(bdev_io->u.bdev.iovs) &&
	    _are_iovs_aligned(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, alignment)) {
		/* Buffer already present and aligned */
		/* [한국어] 사용자가 이미 정렬된 버퍼를 제공 → 추가 할당 없이 즉시 콜백. */
		cb(spdk_bdev_io_get_io_channel(bdev_io), bdev_io, true);
		return;
	}

	bdev_io_get_buf(bdev_io, len);
	/* [한국어] 버퍼 없음/미정렬 → 풀에서 확보(필요 시 bounce). */
}

/*
 * [한국어]
 * _bdev_io_get_bounce_buf - bounce 버퍼 확보를 위해 콜백 설정 후 풀 버퍼 요청.
 *
 * @bdev_io: 대상 I/O.
 * @cb: 버퍼 준비 완료 콜백.
 * @len: 필요한 길이.
 * @return: 없음.
 *
 * 메타데이터 변환 등 내부 처리에서 항상 bounce 버퍼가 필요한 경로의 진입 래퍼.
 * 정렬 검사 없이 무조건 풀에서 버퍼를 받아온다.
 * 실행 컨텍스트: 채널 소유 스레드. callee: bdev_io_get_buf.
 *
 * 호출 체인:
 *   메타 변환 경로 → [_bdev_io_get_bounce_buf] → bdev_io_get_buf
 */
static void
_bdev_io_get_bounce_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb,
			uint64_t len)
{
	assert(cb != NULL);
	/* [한국어] 완료 콜백 필수. */
	bdev_io->internal.get_buf_cb = cb;
	/* [한국어] 콜백 저장. */

	bdev_io_get_buf(bdev_io, len);
	/* [한국어] 무조건 풀에서 버퍼 확보. */
}

/*
 * [한국어]
 * _bdev_io_get_accel_buf - accel framework에서 (메모리 도메인) 버퍼를 확보.
 *
 * @bdev_io: 대상 I/O.
 * @return: 없음(실패 시 nomem 큐잉).
 *
 * iobuf 풀이 아니라 accel framework(spdk_accel_get_buf)에서 버퍼를 받는다. accel 버퍼는
 * 메모리 도메인/ctx 정보를 함께 돌려주며, 확보 실패(-ENOMEM 등)면 GET_ACCEL_BUF 단계로
 * 재시도 큐잉한다. 성공 시 _bdev_io_set_buf로 이어간다.
 * 실행 컨텍스트: 채널 소유 스레드. caller: bdev_io_get_accel_buf / 재시도.
 * callee: spdk_accel_get_buf.
 *
 * 호출 체인:
 *   bdev_io_get_accel_buf → [_bdev_io_get_accel_buf] → spdk_accel_get_buf → _bdev_io_set_buf
 */
static void
_bdev_io_get_accel_buf(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] accel_channel 보유 채널. */
	void *buf;
	/* [한국어] accel이 돌려준 버퍼. */
	int rc;
	/* [한국어] 확보 결과 코드. */

	rc = spdk_accel_get_buf(ch->accel_channel,
				bdev_io->internal.buf.len,
				&buf,
				&bdev_io->u.bdev.memory_domain,
				&bdev_io->u.bdev.memory_domain_ctx);
	/* [한국어] accel 채널에서 버퍼 + 메모리 도메인/ctx 확보 요청. */
	if (rc != 0) {
		/* [한국어] 확보 실패 → GET_ACCEL_BUF 단계로 재시도 큐 꼬리에 적재. */
		bdev_queue_nomem_io_tail(ch->shared_resource, bdev_io,
					 BDEV_IO_RETRY_STATE_GET_ACCEL_BUF);
		return;
	}

	_bdev_io_set_buf(bdev_io, buf, bdev_io->internal.buf.len);
	/* [한국어] 받은 accel 버퍼로 버퍼 설정 절차 진행. */
}

/*
 * [한국어]
 * bdev_io_get_accel_buf - accel 버퍼 확보의 진입 래퍼(길이/콜백 설정 후 확보).
 *
 * @bdev_io: 대상 I/O.
 * @cb: 버퍼 준비 완료 콜백.
 * @len: 필요한 길이.
 * @return: 없음.
 *
 * 실행 컨텍스트: 채널 소유 스레드. callee: _bdev_io_get_accel_buf.
 *
 * 호출 체인:
 *   모듈 → [bdev_io_get_accel_buf] → _bdev_io_get_accel_buf
 */
static inline void
bdev_io_get_accel_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb,
		      uint64_t len)
{
	bdev_io->internal.buf.len = len;
	/* [한국어] 필요 길이 기록. */
	bdev_io->internal.get_buf_cb = cb;
	/* [한국어] 완료 콜백 저장. */

	_bdev_io_get_accel_buf(bdev_io);
	/* [한국어] accel 버퍼 확보 시작. */
}

/*
 * [한국어]
 * bdev_module_get_max_ctx_size - 등록된 모든 bdev 모듈이 요구하는 per-IO ctx 크기의 최댓값.
 *
 * @return: 최대 모듈 컨텍스트 바이트 수(없으면 0).
 *
 * bdev_io 객체는 끝에 모듈별 private 컨텍스트를 위한 공간을 두는데, 모든 모듈이 같은 풀의
 * bdev_io를 쓰므로 가장 큰 모듈의 ctx를 담을 만큼 공간을 잡아야 한다. 이 함수가 그 최댓값을
 * 계산해 bdev_io mempool 원소 크기 산정에 쓰인다.
 * 실행 컨텍스트: app 스레드(초기화 시점). caller: bdev subsystem init.
 *
 * 호출 체인:
 *   bdev_mgr 초기화 → [bdev_module_get_max_ctx_size] → 각 module->get_ctx_size
 */
static int
bdev_module_get_max_ctx_size(void)
{
	struct spdk_bdev_module *bdev_module;
	/* [한국어] 순회용 모듈 포인터. */
	int max_bdev_module_size = 0;
	/* [한국어] 지금까지 본 최대 ctx 크기. */

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, internal.tailq) {
		/* [한국어] 등록된 모든 모듈 순회. */
		if (bdev_module->get_ctx_size && bdev_module->get_ctx_size() > max_bdev_module_size) {
			/* [한국어] get_ctx_size를 구현했고 더 큰 값이면 최댓값 갱신. */
			max_bdev_module_size = bdev_module->get_ctx_size();
		}
	}

	return max_bdev_module_size;
	/* [한국어] 모든 모듈 중 최대 ctx 크기 반환. */
}

/*
 * [한국어]
 * bdev_enable_histogram_config_json - bdev의 histogram 활성 설정을 JSON config로 직렬화.
 *
 * @bdev: 대상 bdev.
 * @w: JSON 출력 스트림.
 * @return: 없음.
 *
 * latency histogram이 켜진 bdev에 대해 "bdev_enable_histogram" RPC 호출 형태로 설정을
 * 기록해, config 재로딩 시 동일하게 histogram이 켜지도록 한다. 특정 io_type만 추적하도록
 * 설정됐으면 opc도 함께 기록한다. 꺼져 있으면 아무것도 출력하지 않는다.
 * 실행 컨텍스트: app 스레드(config dump). caller: spdk_bdev_subsystem_config_json.
 *
 * 호출 체인:
 *   config_json → [bdev_enable_histogram_config_json] → spdk_json_write_*
 */
static void
bdev_enable_histogram_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	if (!bdev->internal.histogram_enabled) {
		/* [한국어] histogram이 꺼져 있으면 재현할 설정 없음 → 출력 생략. */
		return;
	}

	spdk_json_write_object_begin(w);
	/* [한국어] RPC 호출 객체 시작. */
	spdk_json_write_named_string(w, "method", "bdev_enable_histogram");
	/* [한국어] 재현 시 호출할 RPC 이름. */

	spdk_json_write_named_object_begin(w, "params");
	/* [한국어] 인자 객체 시작. */
	spdk_json_write_named_string(w, "name", bdev->name);
	/* [한국어] 대상 bdev 이름. */

	spdk_json_write_named_bool(w, "enable", bdev->internal.histogram_enabled);
	/* [한국어] 활성화 플래그(true). */

	if (bdev->internal.histogram_io_type) {
		/* [한국어] 특정 io_type만 추적하도록 설정됐으면 opc 문자열도 기록. */
		spdk_json_write_named_string(w, "opc",
					     spdk_bdev_get_io_type_name(bdev->internal.histogram_io_type));
	}

	spdk_json_write_object_end(w);
	/* [한국어] params 객체 닫기. */

	spdk_json_write_object_end(w);
	/* [한국어] RPC 호출 객체 닫기. */
}

/*
 * [한국어]
 * bdev_qos_config_json - bdev의 QoS rate-limit 설정을 JSON config로 직렬화.
 *
 * @bdev: 대상 bdev.
 * @w: JSON 출력 스트림.
 * @return: 없음.
 *
 * QoS(rate limit)가 설정된 bdev에 대해 "bdev_set_qos_limit" RPC 호출 형태로 4종 한도
 * (rw_ios/rw_mbps/r_mbps/w_mbps) 중 0보다 큰 값만 기록해, config 재로딩 시 동일한 QoS를
 * 재현하게 한다. QoS 객체가 없으면 생략한다.
 * 실행 컨텍스트: app 스레드(config dump). caller: spdk_bdev_subsystem_config_json.
 *
 * 호출 체인:
 *   config_json → [bdev_qos_config_json] → spdk_bdev_get_qos_rate_limits / spdk_json_write_*
 */
static void
bdev_qos_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	int i;
	/* [한국어] 4종 limit 순회 인덱스. */
	struct spdk_bdev_qos *qos = bdev->internal.qos;
	/* [한국어] bdev의 QoS 객체(없으면 NULL). */
	uint64_t limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];
	/* [한국어] 현재 한도 값을 담을 배열(타입별). */

	if (!qos) {
		/* [한국어] QoS 미설정 → 재현할 설정 없음. */
		return;
	}

	spdk_bdev_get_qos_rate_limits(bdev, limits);
	/* [한국어] 현재 한도 4종을 limits 배열로 조회. */

	spdk_json_write_object_begin(w);
	/* [한국어] RPC 호출 객체 시작. */
	spdk_json_write_named_string(w, "method", "bdev_set_qos_limit");
	/* [한국어] 재현 시 호출할 RPC 이름. */

	spdk_json_write_named_object_begin(w, "params");
	/* [한국어] 인자 객체 시작. */
	spdk_json_write_named_string(w, "name", bdev->name);
	/* [한국어] 대상 bdev 이름. */
	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		/* [한국어] 4종 한도 순회. */
		if (limits[i] > 0) {
			/* [한국어] 0보다 큰(실제 설정된) 한도만 기록. */
			spdk_json_write_named_uint64(w, qos_rpc_type[i], limits[i]);
		}
	}
	spdk_json_write_object_end(w);
	/* [한국어] params 객체 닫기. */

	spdk_json_write_object_end(w);
	/* [한국어] RPC 호출 객체 닫기. */
}

/*
 * [한국어]
 * spdk_bdev_subsystem_config_json - bdev 서브시스템 전체 상태를 JSON-RPC 재현 스크립트로 직렬화.
 *
 * @w: 출력 JSON writer(상위 spdk_subsystem_config_json이 만든 컨텍스트).
 * @return: 없음(출력은 w 스트림에 누적).
 *
 * 이 함수는 "config save" 기능의 핵심으로, 현재 실행 중인 bdev 레이어의 모든 설정을
 * 다시 적용하면 동일 상태를 복원할 수 있는 RPC 호출 배열을 만든다. spdk_subsystem_save_config()
 * 가 각 서브시스템을 순회하며 이 함수를 호출하고, 결과 JSON은 파일로 저장되어 다음 부팅 시
 * spdk_subsystem_load_config()로 그대로 재생(replay)된다.
 *
 * 동작 순서:
 *   1) bdev_set_options RPC — io_pool/cache size, auto_examine, iobuf 캐시 등 전역 옵션 1건.
 *   2) bdev_examine_allowlist_config_json — examine allowlist(어떤 bdev를 자동 examine할지).
 *   3) 각 bdev_module->config_json() — 모듈(nvme/aio/malloc 등)이 자기 bdev 생성 RPC를 출력.
 *   4) 각 bdev->fn_table->write_config_json() + QoS + histogram 설정.
 *   5) 마지막에 bdev_wait_for_examine — 모든 examine 완료를 보장하기 위해 반드시 배열 끝에 둔다.
 *
 * 실행 컨텍스트: app(init) 스레드. bdev 목록 순회 구간은 g_bdev_mgr.spinlock으로 보호한다
 * (다른 스레드가 동시에 bdev를 등록/해제할 수 있으므로). JSON 출력 자체는 콜백 없는 동기 작업.
 *
 * 호출 체인:
 *   spdk_subsystem_config_json(init 서브시스템) → [spdk_bdev_subsystem_config_json]
 *     → bdev_examine_allowlist_config_json / 모듈 config_json / fn_table->write_config_json
 *     → bdev_qos_config_json / bdev_enable_histogram_config_json
 */
void
spdk_bdev_subsystem_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_bdev_module *bdev_module;
	/* [한국어] 등록된 bdev 모듈 순회 포인터(각 모듈의 config_json 호출용). */
	struct spdk_bdev *bdev;
	/* [한국어] 개별 bdev 순회 포인터(bdev별 설정 직렬화용). */

	assert(w != NULL);
	/* [한국어] 출력 writer는 필수 — NULL이면 호출자 버그. */

	spdk_json_write_array_begin(w);
	/* [한국어] 전체 설정을 RPC 호출 객체들의 JSON 배열 '[' 로 감싼다(load 시 순차 replay). */

	spdk_json_write_object_begin(w);
	/* [한국어] 첫 RPC: bdev_set_options(전역 옵션) 객체 시작. */
	spdk_json_write_named_string(w, "method", "bdev_set_options");
	/* [한국어] replay할 RPC 메서드 이름. */
	spdk_json_write_named_object_begin(w, "params");
	/* [한국어] 옵션 인자 객체 시작. */
	spdk_json_write_named_uint32(w, "bdev_io_pool_size", g_bdev_opts.bdev_io_pool_size);
	/* [한국어] 전역 bdev_io mempool 크기(모든 채널이 공유하는 풀의 엔트리 수). */
	spdk_json_write_named_uint32(w, "bdev_io_cache_size", g_bdev_opts.bdev_io_cache_size);
	/* [한국어] per-thread bdev_io 캐시 크기(스레드별로 미리 채워 starvation 방지). */
	spdk_json_write_named_bool(w, "bdev_auto_examine", g_bdev_opts.bdev_auto_examine);
	/* [한국어] 신규 bdev 등록 시 자동 examine 수행 여부. */
	spdk_json_write_named_uint32(w, "iobuf_small_cache_size", g_bdev_opts.iobuf_small_cache_size);
	/* [한국어] iobuf small 버퍼 per-thread 캐시 크기(DMA 데이터 버퍼 풀). */
	spdk_json_write_named_uint32(w, "iobuf_large_cache_size", g_bdev_opts.iobuf_large_cache_size);
	/* [한국어] iobuf large 버퍼 per-thread 캐시 크기. */
	spdk_json_write_object_end(w);
	/* [한국어] params 객체 닫기. */
	spdk_json_write_object_end(w);
	/* [한국어] bdev_set_options RPC 객체 닫기. */

	bdev_examine_allowlist_config_json(w);
	/* [한국어] examine allowlist 항목들을 bdev_examine RPC로 직렬화. */

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, internal.tailq) {
		/* [한국어] 등록된 모든 bdev 모듈을 순회. */
		if (bdev_module->config_json) {
			/* [한국어] 모듈이 config_json 콜백을 제공하면(자기 bdev 생성 RPC를 안다면). */
			bdev_module->config_json(w);
			/* [한국어] 모듈에게 자기 디바이스 생성 RPC를 출력하게 위임(예: bdev_nvme_attach_controller). */
		}
	}

	spdk_spin_lock(&g_bdev_mgr.spinlock);
	/* [한국어] bdev 목록 순회 동안 다른 스레드의 등록/해제로부터 보호. */

	TAILQ_FOREACH(bdev, &g_bdev_mgr.bdevs, internal.link) {
		/* [한국어] 전역 bdev 목록을 순회하며 bdev별 추가 설정 직렬화. */
		if (bdev->fn_table->write_config_json) {
			/* [한국어] 모듈이 bdev 단위 설정 출력 콜백을 제공하면. */
			bdev->fn_table->write_config_json(bdev, w);
			/* [한국어] 해당 bdev의 모듈 고유 설정 RPC를 출력. */
		}

		bdev_qos_config_json(bdev, w);
		/* [한국어] QoS rate-limit이 설정돼 있으면 bdev_set_qos_limit RPC 출력. */
		bdev_enable_histogram_config_json(bdev, w);
		/* [한국어] I/O 레이턴시 히스토그램이 켜져 있으면 활성화 RPC 출력. */
	}

	spdk_spin_unlock(&g_bdev_mgr.spinlock);
	/* [한국어] bdev 목록 순회 종료 → 락 해제. */

	/* This has to be last RPC in array to make sure all bdevs finished examine */
	/* [한국어] examine은 비동기로 진행되므로, 이후 RPC들이 examine 완료에 의존하지 않도록
	 *          반드시 배열의 마지막에 wait_for_examine을 둔다(replay 순서상 동기화 장벽). */
	spdk_json_write_object_begin(w);
	/* [한국어] 마지막 RPC 객체 시작. */
	spdk_json_write_named_string(w, "method", "bdev_wait_for_examine");
	/* [한국어] 모든 bdev의 examine이 끝날 때까지 블록하는 RPC. */
	spdk_json_write_object_end(w);
	/* [한국어] wait_for_examine RPC 객체 닫기. */

	spdk_json_write_array_end(w);
	/* [한국어] 전체 RPC 배열 ']' 닫기. */
}

/*
 * [한국어]
 * bdev_mgmt_channel_destroy - per-thread bdev 관리 채널 파괴 콜백(io_device unregister 경로).
 *
 * @io_device: g_bdev_mgr를 키로 등록된 io_device 핸들(여기선 미사용, 시그니처 요구).
 * @ctx_buf: spdk_thread가 이 스레드용으로 잡아둔 spdk_bdev_mgmt_channel 인스턴스.
 * @return: 없음(void destroy 콜백).
 *
 * 매 spdk_thread가 bdev 레이어를 사용하려면 하나의 mgmt channel을 가지는데, 이 채널은
 * 그 스레드 전용 bdev_io 캐시와 iobuf 채널을 보유한다. 스레드가 채널 참조를 모두 놓으면
 * spdk_io_channel 프레임워크가 이 콜백을 호출해 자원을 회수한다. lockless 설계의 핵심:
 * per_thread_cache는 소유 스레드에서만 접근하므로 락이 필요 없다.
 *
 * 동작: iobuf 채널 정리 → per_thread_cache에 남은 bdev_io를 전부 전역 mempool로 반환 →
 * 카운트가 0이 됐는지 검증. (캐시에 남은 bdev_io는 "빌려온" 것이므로 풀로 돌려놓아야
 * 다른 스레드/채널이 재사용할 수 있다.)
 *
 * 실행 컨텍스트: 채널 소유 spdk_thread. 동기.
 *
 * 호출 체인:
 *   spdk_put_io_channel/io_device unregister → [bdev_mgmt_channel_destroy]
 *     → spdk_iobuf_channel_fini / spdk_mempool_put
 */
static void
bdev_mgmt_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_mgmt_channel *ch = ctx_buf;
	/* [한국어] 프레임워크가 넘긴 ctx_buf를 mgmt 채널 타입으로 캐스팅. */
	struct spdk_bdev_io *bdev_io;
	/* [한국어] 캐시에서 하나씩 꺼내 풀로 반환할 임시 포인터. */

	spdk_iobuf_channel_fini(&ch->iobuf);
	/* [한국어] 이 스레드의 iobuf 채널(DMA 버퍼 캐시) 해제 — large/small 버퍼 풀 반환. */

	while (!STAILQ_EMPTY(&ch->per_thread_cache)) {
		/* [한국어] per-thread bdev_io 캐시가 빌 때까지 반복. */
		bdev_io = STAILQ_FIRST(&ch->per_thread_cache);
		/* [한국어] 캐시 맨 앞 bdev_io를 꺼낸다. */
		STAILQ_REMOVE_HEAD(&ch->per_thread_cache, internal.buf_link);
		/* [한국어] 캐시 리스트에서 제거(buf_link는 캐시 연결용 STAILQ 노드). */
		ch->per_thread_cache_count--;
		/* [한국어] 캐시 보유 개수 감소. */
		spdk_mempool_put(g_bdev_mgr.bdev_io_pool, (void *)bdev_io);
		/* [한국어] bdev_io를 전역 DPDK mempool로 반환(다른 스레드가 재사용 가능). */
	}

	assert(ch->per_thread_cache_count == 0);
	/* [한국어] 모든 캐시 엔트리를 반환했는지 불변식 검증(누수 방지). */
}

/*
 * [한국어]
 * bdev_mgmt_channel_create - per-thread bdev 관리 채널 생성 콜백(io_device register 경로).
 *
 * @io_device: g_bdev_mgr 키로 등록된 io_device(여기선 미사용).
 * @ctx_buf: 프레임워크가 이 스레드용으로 할당한 spdk_bdev_mgmt_channel 버퍼.
 * @return: 0=성공, -1=실패(iobuf 초기화 실패 또는 mempool 고갈).
 *
 * 어떤 스레드가 처음으로 bdev mgmt 채널을 요청하면 spdk_get_io_channel이 이 콜백을 호출한다.
 * 채널은 (1) iobuf 채널(DMA 버퍼 per-thread 캐시), (2) per-thread bdev_io 캐시를 준비한다.
 * bdev_io 캐시를 미리 채워두는 이유: I/O 발행 hot-path에서 mempool 경합 없이 즉시 bdev_io를
 * 꺼내 쓰기 위해서다(lockless·저지연). 캐시가 비면 전역 mempool에서 직접 가져온다.
 *
 * 동작: iobuf 채널 초기화 → per_thread_cache STAILQ 초기화 → bdev_io_cache_size만큼
 * mempool에서 배치(BATCH_SIZE 단위)로 bulk-get하여 캐시에 적재 → shared_resources/io_wait_queue
 * 리스트 초기화. mempool 고갈 시 즉시 destroy로 롤백하고 실패 반환.
 *
 * 실행 컨텍스트: 채널을 요청한 spdk_thread. 동기.
 *
 * 호출 체인:
 *   spdk_get_io_channel → [bdev_mgmt_channel_create]
 *     → spdk_iobuf_channel_init / spdk_mempool_get_bulk
 */
static int
bdev_mgmt_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_mgmt_channel *ch = ctx_buf;
	/* [한국어] 프레임워크 제공 버퍼를 mgmt 채널 타입으로 본다. */
	struct spdk_bdev_io *bdev_ios[BDEV_IO_POPULATE_BATCH_SIZE];
	/* [한국어] mempool에서 한 번에 bulk-get할 bdev_io 포인터 임시 배열(배치 크기만큼). */
	uint32_t i, remaining, count;
	/* [한국어] i=배치 내 인덱스, remaining=아직 채워야 할 캐시 개수, count=이번 배치 크기. */
	int rc;
	/* [한국어] 하위 호출 반환값. */

	rc = spdk_iobuf_channel_init(&ch->iobuf, "bdev",
				     g_bdev_opts.iobuf_small_cache_size,
				     g_bdev_opts.iobuf_large_cache_size);
	/* [한국어] "bdev" 모듈 이름으로 iobuf 채널 생성 — small/large DMA 버퍼 per-thread 캐시 확보. */
	if (rc != 0) {
		/* [한국어] iobuf 풀 부족 등으로 실패하면. */
		SPDK_ERRLOG("Failed to create iobuf channel: %s\n", spdk_strerror(-rc));
		/* [한국어] 에러 로그(음수 errno를 문자열로). */
		return -1;
		/* [한국어] 채널 생성 실패 보고 → 프레임워크가 채널 획득 실패 처리. */
	}

	STAILQ_INIT(&ch->per_thread_cache);
	/* [한국어] per-thread bdev_io 캐시 리스트 초기화. */
	remaining = ch->bdev_io_cache_size = g_bdev_opts.bdev_io_cache_size;
	/* [한국어] 채널의 캐시 목표 크기를 전역 옵션값으로 설정하고, 남은 채울 개수도 동일하게 시작. */

	/* Pre-populate bdev_io cache to ensure this thread cannot be starved. */
	/* [한국어] 캐시를 미리 채워 이 스레드가 mempool 경합으로 굶지 않도록 보장. */
	ch->per_thread_cache_count = 0;
	/* [한국어] 현재 캐시 보유 개수 0에서 시작. */
	while (remaining > 0) {
		/* [한국어] 목표 캐시 크기를 다 채울 때까지 반복. */
		count = spdk_min(remaining, BDEV_IO_POPULATE_BATCH_SIZE);
		/* [한국어] 이번 배치 크기 = min(남은 개수, 배치 상한). */
		rc = spdk_mempool_get_bulk(g_bdev_mgr.bdev_io_pool, (void **)bdev_ios, count);
		/* [한국어] 전역 mempool에서 count개를 한 번에 가져옴(bulk가 개별보다 효율적). */
		if (rc) {
			/* [한국어] mempool에 충분한 bdev_io가 없으면(풀 크기 부족). */
			SPDK_ERRLOG("You need to increase bdev_io_pool_size using bdev_set_options RPC.\n");
			/* [한국어] 운영자에게 pool_size 증설을 안내. */
			assert(false);
			/* [한국어] 디버그 빌드에서는 즉시 abort — 설정 오류는 치명적. */
			bdev_mgmt_channel_destroy(io_device, ctx_buf);
			/* [한국어] 지금까지 캐시에 적재한 bdev_io와 iobuf를 롤백 회수. */
			return -1;
			/* [한국어] 채널 생성 실패 보고. */
		}

		for (i = 0; i < count; i++) {
			/* [한국어] 가져온 배치를 캐시 리스트에 적재. */
			STAILQ_INSERT_HEAD(&ch->per_thread_cache, bdev_ios[i], internal.buf_link);
			/* [한국어] 각 bdev_io를 캐시 앞쪽에 삽입(LIFO — 캐시 지역성 향상). */
			ch->per_thread_cache_count++;
			/* [한국어] 캐시 보유 개수 증가. */
		}

		remaining -= count;
		/* [한국어] 이번 배치만큼 남은 목표 개수 감소. */
	}

	TAILQ_INIT(&ch->shared_resources);
	/* [한국어] 이 mgmt 채널이 관리하는 (bdev,스레드)별 shared_resource 리스트 초기화. */
	TAILQ_INIT(&ch->io_wait_queue);
	/* [한국어] bdev_io 부족으로 대기 중인 I/O 요청 큐 초기화(NOMEM 백프레셔용). */

	return 0;
	/* [한국어] 채널 생성 성공. */
}

/*
 * [한국어]
 * _bdev_init_complete - bdev 서브시스템 초기화 완료를 최종 사용자 콜백으로 통지(init 스레드에서 실행).
 *
 * @ctx: 초기화 결과 rc를 포인터 크기 정수로 인코딩한 값(0=성공, 음수=실패).
 * @return: 없음.
 *
 * bdev 초기화는 비동기(모듈별 async_init/examine)로 진행되므로, 완료 시점에 반드시
 * 초기화를 시작했던 g_init_thread에서 사용자 콜백을 불러야 한다(스레드 affinity).
 * 이 함수는 spdk_thread_exec_msg로 g_init_thread에 전달되어 실행되는 메시지 핸들러다.
 *
 * 동작: 전역에 저장해 둔 g_init_cb_fn/arg를 지역으로 회수하고 전역은 NULL로 비운 뒤
 * (재진입 안전) 사용자 콜백을 호출한다.
 *
 * 실행 컨텍스트: g_init_thread(초기화 시작 스레드). 동기 콜백 호출.
 *
 * 호출 체인:
 *   bdev_init_complete → spdk_thread_exec_msg(g_init_thread) → [_bdev_init_complete] → cb_fn
 */
static void
_bdev_init_complete(void *ctx)
{
	spdk_bdev_init_cb cb_fn = g_init_cb_fn;
	/* [한국어] 사용자에게 알릴 완료 콜백을 전역에서 지역으로 회수. */
	void *cb_arg = g_init_cb_arg;
	/* [한국어] 콜백에 전달할 사용자 인자. */
	int rc = (int)(uintptr_t)ctx;
	/* [한국어] ctx 포인터에 인코딩된 초기화 결과 코드를 복원(0/음수). */

	g_init_cb_fn = NULL;
	/* [한국어] 전역 콜백 슬롯 비움 — 다음 초기화 또는 중복 호출 방지. */
	g_init_cb_arg = NULL;
	/* [한국어] 전역 인자 슬롯 비움. */

	cb_fn(cb_arg, rc);
	/* [한국어] 사용자(예: subsystem init 프레임워크)에게 완료/실패 통지. */
}

/*
 * [한국어]
 * bdev_init_complete - bdev 레이어 전체 초기화 완료 처리(모듈 통지 + 사용자 콜백 디스패치).
 *
 * @rc: 초기화 결과(0=성공, 음수=실패).
 * @return: 없음.
 *
 * 모든 bdev 모듈의 init/examine이 끝났을 때 한 번 호출된다. 성공 시 init_complete 콜백을
 * 가진 모듈들에게 "이제 서브시스템 전체가 준비됐다"고 알린 뒤(일부 모듈은 다른 모듈에
 * 의존하므로 전체 완료 시점을 알아야 함), 최종 사용자 콜백은 init 스레드로 메시지를 보내
 * 실행한다.
 *
 * 실행 컨텍스트: app 스레드. 모듈 순회는 init 단계라 추가 동기화 불필요.
 *
 * 호출 체인:
 *   bdev_module_action_complete / bdev_init_failed → [bdev_init_complete]
 *     → m->init_complete() / spdk_thread_exec_msg(_bdev_init_complete)
 */
static void
bdev_init_complete(int rc)
{
	struct spdk_bdev_module *m;
	/* [한국어] init_complete 통지를 위한 모듈 순회 포인터. */

	g_bdev_mgr.init_complete = true;
	/* [한국어] 서브시스템 초기화 완료 플래그 설정 — 이후 action_complete가 재진입해도 무시되게. */

	/*
	 * For modules that need to know when subsystem init is complete,
	 * inform them now.
	 */
	/* [한국어] 다른 모듈에 의존하는 모듈들은 전체 완료 시점을 알아야 하므로 지금 통지. */
	if (rc == 0) {
		/* [한국어] 성공한 경우에만 모듈에 완료 통지(실패면 통지 생략). */
		TAILQ_FOREACH(m, &g_bdev_mgr.bdev_modules, internal.tailq) {
			/* [한국어] 모든 등록 모듈 순회. */
			if (m->init_complete) {
				/* [한국어] init_complete 콜백을 등록한 모듈만. */
				m->init_complete();
				/* [한국어] 모듈에게 서브시스템 준비 완료 통지(후속 작업 트리거 가능). */
			}
		}
	}

	spdk_thread_exec_msg(g_init_thread, _bdev_init_complete, (void *)(uintptr_t)rc);
	/* [한국어] 최종 사용자 콜백은 반드시 init 스레드에서 — rc를 포인터로 인코딩해 메시지 전달. */
}

/*
 * [한국어]
 * bdev_module_all_actions_completed - 진행 중인 모듈 비동기 작업이 하나도 없는지 검사.
 *
 * @return: true=모든 모듈의 action_in_progress가 0, false=아직 진행 중인 작업 존재.
 *
 * 모듈은 async_init 또는 examine 같은 비동기 작업을 시작할 때 action_in_progress를 올리고
 * 끝나면 내린다. bdev 서브시스템 초기화는 이 카운터들이 모두 0이 되어야 완료로 간주할 수
 * 있으므로, 완료 판정 직전에 이 함수로 전체를 점검한다.
 *
 * 실행 컨텍스트: app 스레드. 각 모듈의 spinlock 없이 읽지만, 완료 판정 흐름상 안전.
 *
 * 호출 체인:
 *   bdev_module_action_complete → [bdev_module_all_actions_completed]
 */
static bool
bdev_module_all_actions_completed(void)
{
	struct spdk_bdev_module *m;
	/* [한국어] 진행 중 작업 점검용 모듈 순회 포인터. */

	TAILQ_FOREACH(m, &g_bdev_mgr.bdev_modules, internal.tailq) {
		/* [한국어] 모든 모듈을 순회. */
		if (m->internal.action_in_progress > 0) {
			/* [한국어] 어느 한 모듈이라도 비동기 작업 진행 중이면. */
			return false;
			/* [한국어] 아직 완료 불가 — 조기 반환. */
		}
	}
	return true;
	/* [한국어] 모든 모듈의 작업이 끝났음. */
}

/*
 * [한국어]
 * bdev_module_action_complete - 모듈 비동기 작업 1건 완료 시 서브시스템 완료 여부 재평가.
 *
 * @return: 없음.
 *
 * 모듈의 한 작업이 끝날 때마다 호출되어, "지금이 bdev 레이어 전체 초기화를 끝낼 시점인가"를
 * 판단한다. 아직 모듈 사전 초기화(module_init 루프)가 진행 중이거나 이미 완료됐으면 즉시
 * 반환하고, 모든 모듈 작업이 끝났을 때에만 bdev_init_complete(0)을 호출해 완료 처리한다.
 *
 * 실행 컨텍스트: app 스레드. 게이트 조건으로 중복 완료를 방지한다.
 *
 * 호출 체인:
 *   bdev_module_action_done / spdk_bdev_module_*_done → [bdev_module_action_complete]
 *     → bdev_module_all_actions_completed → bdev_init_complete
 */
static void
bdev_module_action_complete(void)
{
	/*
	 * Don't finish bdev subsystem initialization if
	 * module pre-initialization is still in progress, or
	 * the subsystem been already initialized.
	 */
	/* [한국어] module_init 루프가 아직 안 끝났거나, 이미 초기화 완료된 경우엔 완료 처리 금지. */
	if (!g_bdev_mgr.module_init_complete || g_bdev_mgr.init_complete) {
		/* [한국어] module_init 루프 미완(false) 또는 이미 완료(init_complete)면 조기 반환. */
		return;
	}

	/*
	 * Check all bdev modules for inits/examinations in progress. If any
	 * exist, return immediately since we cannot finish bdev subsystem
	 * initialization until all are completed.
	 */
	/* [한국어] 아직 진행 중인 모듈 비동기 작업(init/examine)이 있으면 완료 불가. */
	if (!bdev_module_all_actions_completed()) {
		/* [한국어] 하나라도 진행 중이면 대기 — 마지막 작업 완료 시 다시 호출된다. */
		return;
	}

	/*
	 * Modules already finished initialization - now that all
	 * the bdev modules have finished their asynchronous I/O
	 * processing, the entire bdev layer can be marked as complete.
	 */
	/* [한국어] 모든 모듈 작업이 끝났으므로 bdev 레이어 전체를 완료로 마킹(성공 rc=0). */
	bdev_init_complete(0);
}

/*
 * [한국어]
 * bdev_module_action_done - 모듈 비동기 작업 1건 완료를 카운트다운하고 완료 판정 트리거.
 *
 * @ctx: 작업을 끝낸 spdk_bdev_module 포인터.
 * @return: 없음.
 *
 * spdk_bdev_module_init_done/examine_done이 app 스레드로 보낸 메시지의 핸들러.
 * 모듈의 action_in_progress 카운터를 spinlock으로 보호하며 1 감소시키고(다른 스레드가
 * 동시에 카운터를 조작할 수 있으므로 락 필요), 서브시스템 완료 여부를 재평가한다.
 *
 * 실행 컨텍스트: app 스레드(메시지로 강제 이동됨). 카운터 갱신은 모듈 spinlock 보호.
 *
 * 호출 체인:
 *   spdk_bdev_module_(init|examine)_done → spdk_thread_exec_msg(app) → [bdev_module_action_done]
 *     → bdev_module_action_complete
 */
static void
bdev_module_action_done(void *ctx)
{
	struct spdk_bdev_module *module = ctx;
	/* [한국어] 작업을 끝낸 모듈. */

	spdk_spin_lock(&module->internal.spinlock);
	/* [한국어] action_in_progress 카운터 보호(여러 비동기 작업이 동시 완료될 수 있음). */
	assert(module->internal.action_in_progress > 0);
	/* [한국어] 완료를 보고하는데 진행 중 카운터가 0이면 논리 오류. */
	module->internal.action_in_progress--;
	/* [한국어] 진행 중 작업 1건 완료 반영. */
	spdk_spin_unlock(&module->internal.spinlock);
	/* [한국어] 카운터 갱신 끝 → 락 해제. */
	bdev_module_action_complete();
	/* [한국어] 이번 완료로 서브시스템 전체가 끝났는지 재평가. */
}

/*
 * [한국어]
 * spdk_bdev_module_init_done - 모듈이 자신의 async_init 완료를 bdev 코어에 통지(공개 API).
 *
 * @module: 비동기 초기화를 끝낸 모듈(자기 자신을 넘김).
 * @return: 없음.
 *
 * async_init=true로 등록한 모듈은 module_init()에서 즉시 끝내지 않고 나중에 이 함수로
 * 완료를 보고한다. 보고는 어느 스레드에서든 올 수 있으므로 app 스레드로 메시지를 보내
 * 카운터 갱신을 직렬화한다(thread affinity).
 *
 * 실행 컨텍스트: 모듈의 임의 스레드 → app 스레드로 디스패치.
 *
 * 호출 체인:
 *   모듈 코드 → [spdk_bdev_module_init_done] → spdk_thread_exec_msg(app, bdev_module_action_done)
 */
void
spdk_bdev_module_init_done(struct spdk_bdev_module *module)
{
	assert(module->async_init);
	/* [한국어] 동기 init 모듈이 이 API를 부르면 카운터 언더플로우 — async_init만 허용. */
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), bdev_module_action_done, module);
	/* [한국어] app 스레드로 완료 카운트다운 메시지 전달(이미 app 스레드면 즉시 실행). */
}

/*
 * [한국어]
 * spdk_bdev_module_examine_done - 모듈이 examine_config/examine_disk 완료를 통지(공개 API).
 *
 * @module: examine를 끝낸 모듈.
 * @return: 없음.
 *
 * 신규 bdev 등록 시 모듈은 examine 콜백에서 해당 bdev를 검사하며(예: 파티션/lvol 탐지),
 * 비동기 검사가 끝나면 이 함수로 완료를 알린다. init_done과 동일하게 app 스레드로
 * 라우팅하여 action_in_progress 카운터를 안전하게 감소시킨다.
 *
 * 실행 컨텍스트: 모듈의 임의 스레드 → app 스레드.
 *
 * 호출 체인:
 *   모듈 examine 콜백 → [spdk_bdev_module_examine_done] → spdk_thread_exec_msg(app, bdev_module_action_done)
 */
void
spdk_bdev_module_examine_done(struct spdk_bdev_module *module)
{
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), bdev_module_action_done, module);
	/* [한국어] examine 완료를 app 스레드에서 카운트다운하도록 메시지 전달. */
}

/** The last initialized bdev module */
/* [한국어] module_init 루프 중 현재(또는 마지막으로) 초기화한 모듈을 가리키는 전역 포인터.
 * 설정자: bdev_modules_init()이 각 모듈을 부르기 직전에 갱신, 루프 끝나면 NULL.
 * 읽는 자: 비동기 모듈이 자신의 init 완료를 보고할 때 "다음 모듈 init 재개" 흐름을 추적.
 * 값 범위: 유효 모듈 포인터 또는 NULL(루프 미진행).
 * 동기화: app(init) 스레드에서만 다뤄지므로 별도 락 불필요. */
static struct spdk_bdev_module *g_resume_bdev_module = NULL;

/*
 * [한국어]
 * bdev_init_failed - 모듈 초기화 실패 시 정리 후 서브시스템 초기화를 실패로 종료.
 *
 * @cb_arg: 실패를 유발한 spdk_bdev_module 포인터.
 * @return: 없음.
 *
 * bdev_modules_init()에서 module_init()이 음수를 반환하면, 즉시 완료 처리하지 않고
 * action_in_progress를 올려 다른 모듈의 완료가 끼어들지 못하게 막은 뒤, 메시지로 이 함수를
 * 지연 실행한다. 여기서 카운터를 다시 내리고 bdev_init_complete(-1)을 호출해 실패를 전파한다.
 *
 * 실행 컨텍스트: app 스레드(메시지로 지연 실행). 카운터는 모듈 spinlock 보호.
 *
 * 호출 체인:
 *   bdev_modules_init(실패) → spdk_thread_send_msg → [bdev_init_failed] → bdev_init_complete(-1)
 */
static void
bdev_init_failed(void *cb_arg)
{
	struct spdk_bdev_module *module = cb_arg;
	/* [한국어] 초기화에 실패한 모듈. */

	spdk_spin_lock(&module->internal.spinlock);
	/* [한국어] action_in_progress 갱신 보호. */
	assert(module->internal.action_in_progress > 0);
	/* [한국어] 실패 경로에서도 카운터가 올라가 있어야 함(modules_init이 올려둠). */
	module->internal.action_in_progress--;
	/* [한국어] 실패 처리를 시작하므로 카운터를 원복. */
	spdk_spin_unlock(&module->internal.spinlock);
	/* [한국어] 락 해제. */
	bdev_init_complete(-1);
	/* [한국어] 서브시스템 초기화를 실패(-1)로 종료 — 사용자 콜백에 에러 전파. */
}

/*
 * [한국어]
 * bdev_modules_init - 등록된 모든 bdev 모듈의 module_init()을 순차 호출.
 *
 * @return: 0=모든 모듈 init 성공, 음수=어느 모듈이 실패한 errno.
 *
 * bdev 서브시스템 초기화의 1단계. 등록 순서대로 각 모듈의 module_init을 부른다.
 * async_init 모듈은 즉시 끝나지 않으므로 action_in_progress=1을 미리 세팅해 두고,
 * 나중에 spdk_bdev_module_init_done으로 완료를 보고받는다. 어느 모듈이 실패하면
 * 다른 모듈의 완료가 서브시스템을 잘못 "완료"로 만들지 못하도록 카운터를 올린 뒤
 * bdev_init_failed를 지연 실행하고 즉시 반환한다.
 *
 * 실행 컨텍스트: app(init) 스레드. 동기 루프이지만 async 모듈은 비동기로 이어진다.
 *
 * 호출 체인:
 *   bdev_initialize → [bdev_modules_init] → module->module_init() / bdev_init_failed
 */
static int
bdev_modules_init(void)
{
	struct spdk_bdev_module *module;
	/* [한국어] 초기화할 모듈 순회 포인터. */
	int rc = 0;
	/* [한국어] module_init 반환값. */

	TAILQ_FOREACH(module, &g_bdev_mgr.bdev_modules, internal.tailq) {
		/* [한국어] 등록된 모든 모듈을 등록 순서대로 순회. */
		g_resume_bdev_module = module;
		/* [한국어] 현재 초기화 중인 모듈을 전역에 기록(재개 추적용). */
		if (module->async_init) {
			/* [한국어] 비동기 init 모듈은 module_init 반환 후에도 작업이 진행된다. */
			spdk_spin_lock(&module->internal.spinlock);
			/* [한국어] 카운터 갱신 보호. */
			module->internal.action_in_progress = 1;
			/* [한국어] 진행 중 작업 1건 등록 — 완료 보고 전까지 서브시스템 완료를 막는다. */
			spdk_spin_unlock(&module->internal.spinlock);
			/* [한국어] 락 해제. */
		}
		rc = module->module_init();
		/* [한국어] 모듈 초기화 콜백 실행(동기 모듈은 여기서 끝, async 모듈은 시작만). */
		if (rc != 0) {
			/* Bump action_in_progress to prevent other modules from completion of modules_init
			 * Send message to defer application shutdown until resources are cleaned up */
			/* [한국어] 실패 시: 다른 모듈의 완료가 끼어들어 서브시스템을 완료로 만들지 못하게
			 *          action_in_progress를 올려두고, 자원 정리 후 종료하도록 메시지로 지연 처리. */
			spdk_spin_lock(&module->internal.spinlock);
			/* [한국어] 카운터 갱신 보호. */
			module->internal.action_in_progress = 1;
			/* [한국어] 완료 차단용 작업 카운터 설정. */
			spdk_spin_unlock(&module->internal.spinlock);
			/* [한국어] 락 해제. */
			spdk_thread_send_msg(spdk_get_thread(), bdev_init_failed, module);
			/* [한국어] 현재 스레드에 bdev_init_failed를 지연 등록 — 호출 스택을 풀고 안전하게 실패 처리. */
			return rc;
			/* [한국어] 실패 코드 반환 — 호출자(bdev_initialize)가 인지. */
		}
	}

	g_resume_bdev_module = NULL;
	/* [한국어] 모든 모듈 init 호출 완료 → 재개 추적 포인터 초기화. */
	return 0;
	/* [한국어] 1단계(모듈 init 디스패치) 성공. */
}

/*
 * [한국어]
 * bdev_initialize - bdev 서브시스템 전역 자원 준비 및 모듈 초기화 시작(app 스레드 실행 본체).
 *
 * @not_used: spdk_thread_exec_msg 시그니처 충족용(미사용).
 * @return: 없음(완료는 콜백 체인으로 통지).
 *
 * spdk_bdev_initialize()가 app 스레드로 보낸 메시지의 실제 처리부. bdev 레이어가 동작하는 데
 * 필요한 전역 자원을 모두 만든다: (1) notify 타입 등록, (2) iobuf 모듈 등록, (3) 전역
 * bdev_io mempool(모든 채널이 공유), (4) write-zeroes용 zero buffer(DMA 가능 메모리),
 * (5) mgmt 채널 io_device 등록. 그 후 bdev_modules_init으로 각 모듈을 초기화하고 완료 판정을
 * 트리거한다. 어느 단계든 실패하면 bdev_init_complete(-1)로 즉시 실패 통지한다.
 *
 * 실행 컨텍스트: app 스레드. 전역 자원 1회 생성이므로 추가 락 불필요.
 *
 * 호출 체인:
 *   spdk_bdev_initialize → spdk_thread_exec_msg(app) → [bdev_initialize]
 *     → spdk_mempool_create / spdk_io_device_register / bdev_modules_init / bdev_module_action_complete
 */
static void
bdev_initialize(void *not_used)
{
	int rc = 0;
	/* [한국어] 하위 초기화 함수 반환값. */
	char mempool_name[32];
	/* [한국어] PID를 포함한 bdev_io mempool 이름 버퍼(멀티 프로세스 시 고유성 확보). */

	spdk_notify_type_register("bdev_register");
	/* [한국어] bdev 등록 이벤트 타입을 notify 서브시스템에 등록(관찰자 통지용). */
	spdk_notify_type_register("bdev_unregister");
	/* [한국어] bdev 해제 이벤트 타입 등록. */
	spdk_notify_type_register("bdev_resize");
	/* [한국어] bdev 크기 변경 이벤트 타입 등록. */

	snprintf(mempool_name, sizeof(mempool_name), "bdev_io_%d", getpid());
	/* [한국어] PID로 mempool 이름을 유일화 — 여러 SPDK 프로세스가 hugepage shm을 공유해도 충돌 방지. */

	rc = spdk_iobuf_register_module("bdev");
	/* [한국어] "bdev" 이름으로 iobuf 모듈 등록 — 이후 채널이 small/large DMA 버퍼 풀을 쓸 수 있게. */
	if (rc != 0) {
		/* [한국어] iobuf 모듈 등록 실패 시. */
		SPDK_ERRLOG("could not register bdev iobuf module: %s\n", spdk_strerror(-rc));
		/* [한국어] 에러 로그. */
		bdev_init_complete(-1);
		/* [한국어] 초기화 실패 통지. */
		return;
		/* [한국어] 이후 단계 진행 중단. */
	}

	g_bdev_mgr.bdev_io_pool = spdk_mempool_create(mempool_name,
				  g_bdev_opts.bdev_io_pool_size,
				  sizeof(struct spdk_bdev_io) +
				  bdev_module_get_max_ctx_size(),
				  0,
				  SPDK_ENV_NUMA_ID_ANY);
	/* [한국어] 전역 bdev_io mempool 생성 — 엔트리 크기 = spdk_bdev_io + 모든 모듈 중 최대 ctx 크기
	 *          (모듈별 per-IO 컨텍스트를 bdev_io 뒤에 함께 담기 위함). cache=0, NUMA 무관. */

	if (g_bdev_mgr.bdev_io_pool == NULL) {
		/* [한국어] hugepage 부족 등으로 mempool 생성 실패 시. */
		SPDK_ERRLOG("could not allocate spdk_bdev_io pool\n");
		/* [한국어] 에러 로그. */
		bdev_init_complete(-1);
		/* [한국어] 초기화 실패 통지. */
		return;
		/* [한국어] 중단. */
	}

	g_bdev_mgr.zero_buffer = spdk_zmalloc(ZERO_BUFFER_SIZE, ZERO_BUFFER_SIZE,
					      NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	/* [한국어] write_zeroes 폴백/패딩에 쓰는 0으로 채운 DMA 버퍼 할당(DMA 가능·정렬된 hugepage 메모리). */
	if (!g_bdev_mgr.zero_buffer) {
		/* [한국어] zero buffer 할당 실패 시. */
		SPDK_ERRLOG("create bdev zero buffer failed\n");
		/* [한국어] 에러 로그. */
		bdev_init_complete(-1);
		/* [한국어] 초기화 실패 통지. */
		return;
		/* [한국어] 중단. */
	}

#ifdef SPDK_CONFIG_VTUNE
	/* [한국어] Intel VTune 프로파일링 빌드일 때만: bdev I/O 트레이스용 ITT 도메인 생성. */
	g_bdev_mgr.domain = __itt_domain_create("spdk_bdev");
#endif

	spdk_io_device_register(&g_bdev_mgr, bdev_mgmt_channel_create,
				bdev_mgmt_channel_destroy,
				sizeof(struct spdk_bdev_mgmt_channel),
				"bdev_mgr");
	/* [한국어] g_bdev_mgr를 io_device로 등록 — 각 스레드가 get_io_channel 시 mgmt 채널을 생성/파괴하게. */

	rc = bdev_modules_init();
	/* [한국어] 등록된 모든 bdev 모듈의 module_init 순차 호출(async 모듈은 비동기로 이어짐). */
	g_bdev_mgr.module_init_complete = true;
	/* [한국어] module_init 디스패치 루프가 끝났음을 표시 — action_complete가 완료 판정을 진행할 수 있게. */
	if (rc != 0) {
		/* [한국어] 어느 모듈 init이 실패했으면(bdev_init_failed가 이미 지연 등록됨). */
		SPDK_ERRLOG("bdev modules init failed\n");
		/* [한국어] 에러 로그(완료 통지는 bdev_init_failed가 담당). */
		return;
		/* [한국어] 완료 판정으로 진행하지 않고 반환. */
	}

	bdev_module_action_complete();
	/* [한국어] 동기 모듈만 있었다면 여기서 즉시 완료 판정 — async 모듈이 남았으면 나중에 트리거된다. */
}

/* [한국어] spdk_bdev_initialize를 app 스레드 이외에서 호출하는 것은 deprecated임을 등록.
 *          (등록 시점부터 호출 시 경고 출력 — v26.05 제거 예정, ALWAYS=매 호출마다 경고) */
SPDK_LOG_DEPRECATION_REGISTER(spdk_bdev_initialize,
			      "calling spdk_bdev_initialize from any thread is deprecated",
			      "v26.05", SPDK_LOG_DEPRECATION_ALWAYS);

/*
 * [한국어]
 * spdk_bdev_initialize - bdev 서브시스템 초기화 진입점(공개 API).
 *
 * @cb_fn: 초기화 완료/실패 시 호출될 사용자 콜백(NULL 불가).
 * @cb_arg: 콜백에 전달할 사용자 인자.
 * @return: 없음(결과는 cb_fn으로 비동기 통지).
 *
 * 사용자(주로 SPDK init 서브시스템)가 bdev 레이어를 켜기 위해 호출한다. 콜백/스레드 정보를
 * 전역에 저장한 뒤, 실제 작업(bdev_initialize)은 반드시 app 스레드에서 수행되도록 메시지로
 * 라우팅한다. 호출 스레드를 g_init_thread에 기록해 두는 이유는, 완료 콜백을 호출자가 있던
 * 스레드에서 돌려주기 위함이다(thread affinity 보장).
 *
 * 실행 컨텍스트: 임의 스레드(권장은 app 스레드). app 외 스레드 호출은 deprecated 경고.
 *
 * 호출 체인:
 *   사용자/init 서브시스템 → [spdk_bdev_initialize] → spdk_thread_exec_msg(app, bdev_initialize)
 */
void
spdk_bdev_initialize(spdk_bdev_init_cb cb_fn, void *cb_arg)
{
	assert(cb_fn != NULL);
	/* [한국어] 완료 통지 콜백은 필수. */

	if (!spdk_thread_is_app_thread(NULL)) {
		/* [한국어] 현재 스레드가 app 스레드가 아니면. */
		SPDK_LOG_DEPRECATED(spdk_bdev_initialize);
		/* [한국어] deprecated 경로 사용 경고 출력. */
	}

	g_init_cb_fn = cb_fn;
	/* [한국어] 완료 콜백을 전역에 보관(_bdev_init_complete가 회수). */
	g_init_cb_arg = cb_arg;
	/* [한국어] 콜백 인자 전역 보관. */
	g_init_thread = spdk_get_thread();
	/* [한국어] 호출 스레드를 기록 — 완료 시 이 스레드에서 콜백 실행(affinity). */

	spdk_thread_exec_msg(spdk_thread_get_app_thread(), bdev_initialize, NULL);
	/* [한국어] 실제 초기화는 app 스레드에서 수행(이미 app 스레드면 즉시 실행). */
}

/*
 * [한국어]
 * _bdev_finish_complete - bdev 종료 완료를 사용자 콜백으로 통지(fini 스레드에서 실행).
 *
 * @not_used: 메시지 시그니처 충족용.
 * @return: 없음.
 *
 * spdk_bdev_finish의 거울상 종결 핸들러. _bdev_init_complete와 동일한 패턴으로, 전역에
 * 저장된 fini 콜백을 회수·비우고 호출한다. fini를 시작한 g_fini_thread에서 실행되도록
 * bdev_finish_complete가 메시지로 라우팅해 호출한다.
 *
 * 실행 컨텍스트: g_fini_thread. 동기 콜백.
 *
 * 호출 체인:
 *   bdev_finish_complete → spdk_thread_exec_msg(g_fini_thread) → [_bdev_finish_complete] → cb_fn
 */
static void
_bdev_finish_complete(void *not_used)
{
	spdk_bdev_fini_cb cb_fn = g_fini_cb_fn;
	/* [한국어] 종료 완료 콜백을 전역에서 회수. */
	void *cb_arg = g_fini_cb_arg;
	/* [한국어] 콜백 인자 회수. */

	g_fini_cb_fn = NULL;
	/* [한국어] 전역 슬롯 비움(재진입 방지). */
	g_fini_cb_arg = NULL;
	/* [한국어] 전역 인자 비움. */

	cb_fn(cb_arg);
	/* [한국어] 사용자에게 bdev 종료 완료 통지(init과 달리 rc 인자 없음). */
}

/*
 * [한국어]
 * bdev_finish_complete - bdev 전역 자원 해제 및 종료 완료 통지.
 *
 * @not_used: io_device unregister 콜백/메시지 시그니처 충족용.
 * @return: 없음.
 *
 * 모든 모듈 fini와 mgmt io_device unregister가 끝난 마지막 단계. 전역 bdev_io mempool을
 * 회수하기 전에 모든 bdev_io가 풀로 반환됐는지(count == pool_size) 점검해 누수를 경고하고,
 * zero buffer와 examine allowlist를 해제한 뒤 사용자 종료 콜백을 fini 스레드로 디스패치한다.
 * 마지막으로 init/module_init_complete 플래그를 리셋해 재초기화 가능 상태로 되돌린다.
 *
 * 실행 컨텍스트: app 스레드(io_device unregister 완료 콜백). 동기.
 *
 * 호출 체인:
 *   bdev_module_fini_iter → spdk_io_device_unregister(완료) → [bdev_finish_complete]
 *     → spdk_mempool_free / spdk_free / spdk_thread_exec_msg(_bdev_finish_complete)
 */
static void
bdev_finish_complete(void *not_used)
{
	if (g_bdev_mgr.bdev_io_pool) {
		/* [한국어] 전역 bdev_io mempool이 생성돼 있으면 해제 전 누수 검사. */
		if (spdk_mempool_count(g_bdev_mgr.bdev_io_pool) != g_bdev_opts.bdev_io_pool_size) {
			/* [한국어] 풀에 남은 개수가 원래 크기와 다르면 = 어딘가 반환 안 된 bdev_io가 있음. */
			SPDK_ERRLOG("bdev IO pool count is %zu but should be %u\n",
				    spdk_mempool_count(g_bdev_mgr.bdev_io_pool),
				    g_bdev_opts.bdev_io_pool_size);
			/* [한국어] 누수 경고(개수 불일치 보고) — 종료는 계속 진행. */
		}

		spdk_mempool_free(g_bdev_mgr.bdev_io_pool);
		/* [한국어] 전역 bdev_io mempool 해제(hugepage 메모리 반환). */
	}

	spdk_free(g_bdev_mgr.zero_buffer);
	/* [한국어] write_zeroes용 zero buffer DMA 메모리 해제. */

	bdev_examine_allowlist_free();
	/* [한국어] examine allowlist에 등록된 문자열들 해제. */

	spdk_thread_exec_msg(g_fini_thread, _bdev_finish_complete, NULL);
	/* [한국어] 사용자 종료 콜백을 fini 시작 스레드에서 실행하도록 메시지 전달. */
	g_bdev_mgr.init_complete = false;
	/* [한국어] 초기화 완료 플래그 리셋 — 이후 재초기화 가능하도록. */
	g_bdev_mgr.module_init_complete = false;
	/* [한국어] 모듈 init 완료 플래그 리셋. */
}

/*
 * [한국어]
 * bdev_module_fini_iter - 모든 bdev 모듈의 module_fini를 역순으로 순차 호출(재개 가능 반복자).
 *
 * @arg: 미사용(메시지 시그니처 충족).
 * @return: 없음.
 *
 * 모듈은 보통 의존 순서대로 init되므로, fini는 그 역순(TAILQ_LAST → TAILQ_PREV)으로 진행한다.
 * async_fini 모듈을 만나면 그 자리에서 멈추고(g_resume_bdev_module에 위치 저장) 반환하며,
 * 나중에 spdk_bdev_module_fini_done이 다시 이 함수를 불러 이어서 진행한다(재개 가능 반복자
 * 패턴). 모든 모듈 fini가 끝나면 mgmt io_device를 unregister하여 bdev_finish_complete로 이어진다.
 *
 * 실행 컨텍스트: app 스레드. async_fini 모듈이 즉시 fini_done을 부르며 재진입할 수 있으므로
 * g_resume_bdev_module 저장은 module_fini() 호출 전에 해 둔다(주석의 FIXME 참고).
 *
 * 호출 체인:
 *   bdev_finish_unregister_bdevs_iter / spdk_bdev_module_fini_done → [bdev_module_fini_iter]
 *     → module->module_fini() / spdk_io_device_unregister(bdev_finish_complete)
 */
static void
bdev_module_fini_iter(void *arg)
{
	struct spdk_bdev_module *bdev_module;
	/* [한국어] fini를 호출할 모듈 순회 포인터(역순 진행). */

	/* FIXME: Handling initialization failures is broken now,
	 * so we won't even try cleaning up after successfully
	 * initialized modules. if module_init_complete is false,
	 * just call spdk_bdev_mgr_unregister_cb
	 */
	/* [한국어] 초기화가 끝나기 전에 종료가 들어온 경우 모듈 정리 없이 곧장 자원 회수로 점프. */
	if (!g_bdev_mgr.module_init_complete) {
		/* [한국어] module_init 루프가 완료되지 않았으면 모듈 fini를 건너뛴다. */
		bdev_finish_complete(NULL);
		/* [한국어] 곧바로 전역 자원 회수 단계로. */
		return;
		/* [한국어] 모듈 순회 생략. */
	}

	/* Start iterating from the last touched module */
	/* [한국어] init 역순으로 시작 — 처음 진입이면 마지막 모듈부터, 재개면 직전 위치의 이전 모듈부터. */
	if (!g_resume_bdev_module) {
		/* [한국어] 재개 위치가 없으면(첫 진입) 목록의 마지막 모듈부터. */
		bdev_module = TAILQ_LAST(&g_bdev_mgr.bdev_modules, bdev_module_list);
	} else {
		/* [한국어] async_fini 중단 후 재개라면, 저장된 모듈의 이전(앞쪽) 모듈부터 이어서. */
		bdev_module = TAILQ_PREV(g_resume_bdev_module, bdev_module_list,
					 internal.tailq);
	}

	while (bdev_module) {
		/* [한국어] 목록 앞쪽으로 진행하며 각 모듈을 fini. */
		if (bdev_module->async_fini) {
			/* Save our place so we can resume later. We must
			 * save the variable here, before calling module_fini()
			 * below, because in some cases the module may immediately
			 * call spdk_bdev_module_fini_done() and re-enter
			 * this function to continue iterating. */
			/* [한국어] async_fini 모듈은 module_fini가 즉시 fini_done을 불러 재진입할 수 있으므로,
			 *          반드시 module_fini() 호출 '전에' 현재 위치를 저장해 둔다(재진입 안전). */
			g_resume_bdev_module = bdev_module;
			/* [한국어] 재개 위치 저장. */
		}

		if (bdev_module->module_fini) {
			/* [한국어] 모듈이 fini 콜백을 제공하면. */
			bdev_module->module_fini();
			/* [한국어] 모듈 정리 실행(동기 모듈은 여기서 끝, async 모듈은 나중에 fini_done). */
		}

		if (bdev_module->async_fini) {
			/* [한국어] async_fini 모듈이면 완료를 기다려야 하므로 여기서 멈춘다. */
			return;
			/* [한국어] spdk_bdev_module_fini_done이 호출되면 이 함수가 재개된다. */
		}

		bdev_module = TAILQ_PREV(bdev_module, bdev_module_list,
					 internal.tailq);
		/* [한국어] 동기 모듈이었으면 이전 모듈로 이동해 계속. */
	}

	g_resume_bdev_module = NULL;
	/* [한국어] 모든 모듈 fini 완료 → 재개 포인터 초기화. */
	spdk_io_device_unregister(&g_bdev_mgr, bdev_finish_complete);
	/* [한국어] mgmt io_device 해제 — 모든 스레드의 mgmt 채널 파괴 후 bdev_finish_complete 호출. */
}

/*
 * [한국어]
 * spdk_bdev_module_fini_done - async_fini 모듈이 자신의 종료 완료를 통지(공개 API).
 *
 * @return: 없음.
 *
 * async_fini=true 모듈이 module_fini 작업을 마친 뒤 호출한다. fini 반복을 이어가야 하므로
 * app 스레드로 bdev_module_fini_iter를 다시 디스패치하여 다음(앞쪽) 모듈로 진행시킨다.
 *
 * 실행 컨텍스트: 모듈의 임의 스레드 → app 스레드.
 *
 * 호출 체인:
 *   모듈 fini 완료 → [spdk_bdev_module_fini_done] → spdk_thread_exec_msg(app, bdev_module_fini_iter)
 */
void
spdk_bdev_module_fini_done(void)
{
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), bdev_module_fini_iter, NULL);
	/* [한국어] app 스레드에서 fini 반복자를 재개(다음 모듈 fini 진행). */
}

/*
 * [한국어]
 * bdev_finish_unregister_bdevs_iter - 종료 시 남은 모든 bdev를 역순·top-down으로 해제(재귀 반복자).
 *
 * @cb_arg: 직전에 unregister를 요청했던 bdev(첫 진입 시 NULL).
 * @bdeverrno: 직전 unregister 결과(0=성공, 음수=실패).
 * @return: 없음(다음 bdev로 콜백 체인 이어짐).
 *
 * spdk_bdev_finish 경로에서 등록된 모든 bdev를 안전하게 제거한다. 핵심은 "top-down": 가상
 * bdev(예: lvol, raid)는 자신을 claim한 상위가 사라진 뒤에야 해제돼야 하므로, claim된 bdev는
 * 일단 건너뛰고 unclaimed bdev를 목록 뒤에서부터(역순) 하나씩 unregister한다. 각 unregister는
 * 비동기이며 완료 시 이 함수를 콜백으로 다시 불러(자기 자신이 콜백) 다음 bdev로 진행한다.
 * 모든 bdev가 사라지면 모듈 fini 단계(bdev_module_fini_iter)로 넘어간다.
 *
 * 실행 컨텍스트: app 스레드. bdev별 claim_type 검사는 bdev->internal.spinlock으로 보호.
 *
 * 호출 체인:
 *   bdev_module_fini_start_iter / spdk_bdev_unregister(완료) → [bdev_finish_unregister_bdevs_iter]
 *     → spdk_bdev_unregister(다음 bdev) / bdev_module_fini_iter
 */
static void
bdev_finish_unregister_bdevs_iter(void *cb_arg, int bdeverrno)
{
	struct spdk_bdev *bdev = cb_arg;
	/* [한국어] 직전에 해제를 시도한 bdev(첫 호출 시 NULL). */

	if (bdeverrno && bdev) {
		/* [한국어] 직전 unregister가 실패했고 대상 bdev가 있으면. */
		SPDK_WARNLOG("Unable to unregister bdev '%s' during spdk_bdev_finish()\n",
			     bdev->name);

		/*
		 * Since the call to spdk_bdev_unregister() failed, we have no way to free this
		 *  bdev; try to continue by manually removing this bdev from the list and continue
		 *  with the next bdev in the list.
		 */
		/* [한국어] unregister 실패로 정상 해제가 불가하므로, 무한 루프를 막기 위해 목록에서
		 *          수동으로 떼어내고 다음 bdev로 진행한다(자원은 누수되지만 종료는 계속). */
		TAILQ_REMOVE(&g_bdev_mgr.bdevs, bdev, internal.link);
	}

	if (TAILQ_EMPTY(&g_bdev_mgr.bdevs)) {
		/* [한국어] 모든 bdev가 제거됐으면 다음 단계(모듈 fini)로. */
		SPDK_DEBUGLOG(bdev, "Done unregistering bdevs\n");
		/*
		 * Bdev module finish need to be deferred as we might be in the middle of some context
		 * (like bdev part free) that will use this bdev (or private bdev driver ctx data)
		 * after returning.
		 */
		/* [한국어] 이 콜백 스택이 여전히 bdev/드라이버 ctx를 참조 중일 수 있으므로,
		 *          모듈 fini는 메시지로 지연 실행해 현재 컨텍스트가 완전히 풀린 뒤 진행. */
		spdk_thread_send_msg(spdk_get_thread(), bdev_module_fini_iter, NULL);
		return;
		/* [한국어] bdev 순회 종료. */
	}

	/*
	 * Unregister last unclaimed bdev in the list, to ensure that bdev subsystem
	 * shutdown proceeds top-down. The goal is to give virtual bdevs an opportunity
	 * to detect clean shutdown as opposed to run-time hot removal of the underlying
	 * base bdevs.
	 *
	 * Also, walk the list in the reverse order.
	 */
	/* [한국어] 가상 bdev가 깨끗한 종료를 인지하도록 top-down으로 진행 — 목록 뒤에서부터
	 *          claim되지 않은 bdev를 찾아 먼저 해제한다(claim된 것은 상위가 아직 살아있음). */
	for (bdev = TAILQ_LAST(&g_bdev_mgr.bdevs, spdk_bdev_list);
	     bdev; bdev = TAILQ_PREV(bdev, spdk_bdev_list, internal.link)) {
		/* [한국어] 목록을 역순으로 순회. */
		spdk_spin_lock(&bdev->internal.spinlock);
		/* [한국어] claim_type 읽기 보호(다른 스레드가 claim/release 가능). */
		if (bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE) {
			/* [한국어] 누군가에게 claim된 bdev는 아직 해제하면 안 됨 — 건너뛴다. */
			LOG_ALREADY_CLAIMED_DEBUG("claimed, skipping", bdev);
			spdk_spin_unlock(&bdev->internal.spinlock);
			/* [한국어] 락 해제. */
			continue;
			/* [한국어] 다음(앞쪽) bdev 검사. */
		}
		spdk_spin_unlock(&bdev->internal.spinlock);
		/* [한국어] unclaimed 확인 후 락 해제. */

		SPDK_DEBUGLOG(bdev, "Unregistering bdev '%s'\n", bdev->name);
		spdk_bdev_unregister(bdev, bdev_finish_unregister_bdevs_iter, bdev);
		/* [한국어] 이 unclaimed bdev를 해제 — 완료 시 이 함수가 콜백으로 재진입해 다음 진행. */
		return;
		/* [한국어] 한 번에 하나씩만 처리하고 비동기 완료를 기다린다. */
	}

	/*
	 * If any bdev fails to unclaim underlying bdev properly, we may face the
	 * case of bdev list consisting of claimed bdevs only (if claims are managed
	 * correctly, this would mean there's a loop in the claims graph which is
	 * clearly impossible). Warn and unregister last bdev on the list then.
	 */
	/* [한국어] 위 루프에서 unclaimed bdev를 못 찾았다면(모두 claim 상태) = claim 관리 버그.
	 *          교착을 피하기 위해 경고 후 마지막 bdev를 강제로 해제한다. */
	for (bdev = TAILQ_LAST(&g_bdev_mgr.bdevs, spdk_bdev_list);
	     bdev; bdev = TAILQ_PREV(bdev, spdk_bdev_list, internal.link)) {
		/* [한국어] 역순으로 첫 bdev(=목록 마지막)만 잡아 해제. */
		SPDK_WARNLOG("Unregistering claimed bdev '%s'!\n", bdev->name);
		spdk_bdev_unregister(bdev, bdev_finish_unregister_bdevs_iter, bdev);
		/* [한국어] claim 상태라도 강제 해제(데드락 회피용 최후 수단). */
		return;
		/* [한국어] 하나만 처리하고 완료 대기. */
	}
}

/*
 * [한국어]
 * bdev_module_fini_start_iter - 모든 모듈의 fini_start 콜백을 역순 호출(재개 가능 반복자).
 *
 * @arg: 미사용.
 * @return: 없음.
 *
 * 본격적인 종료(bdev unregister)에 들어가기 전에 모듈들에게 "곧 종료한다"고 알리는 단계.
 * 일부 모듈은 fini_start에서 진행 중인 I/O를 멈추거나 정리를 시작한다. async_fini_start
 * 모듈을 만나면 위치를 저장하고 멈췄다가 spdk_bdev_module_fini_start_done으로 재개한다.
 * 모든 모듈의 fini_start가 끝나면 실제 bdev 해제 단계로 넘어간다.
 *
 * 실행 컨텍스트: app 스레드. async 모듈의 즉시 재진입 대비해 위치 저장은 콜백 호출 전.
 *
 * 호출 체인:
 *   bdev_finish_wait_for_examine_done / spdk_bdev_module_fini_start_done
 *     → [bdev_module_fini_start_iter] → module->fini_start() / bdev_finish_unregister_bdevs_iter
 */
static void
bdev_module_fini_start_iter(void *arg)
{
	struct spdk_bdev_module *bdev_module;
	/* [한국어] fini_start를 호출할 모듈 순회 포인터(역순). */

	if (!g_resume_bdev_module) {
		/* [한국어] 첫 진입이면 마지막 모듈부터. */
		bdev_module = TAILQ_LAST(&g_bdev_mgr.bdev_modules, bdev_module_list);
	} else {
		/* [한국어] async 중단 후 재개면 저장된 위치의 이전 모듈부터. */
		bdev_module = TAILQ_PREV(g_resume_bdev_module, bdev_module_list, internal.tailq);
	}

	while (bdev_module) {
		/* [한국어] 앞쪽으로 진행하며 각 모듈의 fini_start 호출. */
		if (bdev_module->async_fini_start) {
			/* Save our place so we can resume later. We must
			 * save the variable here, before calling fini_start()
			 * below, because in some cases the module may immediately
			 * call spdk_bdev_module_fini_start_done() and re-enter
			 * this function to continue iterating. */
			/* [한국어] async 모듈은 fini_start 안에서 즉시 done을 불러 재진입할 수 있으므로
			 *          호출 전에 위치를 저장한다. */
			g_resume_bdev_module = bdev_module;
			/* [한국어] 재개 위치 저장. */
		}

		if (bdev_module->fini_start) {
			/* [한국어] fini_start 콜백을 제공하는 모듈만. */
			bdev_module->fini_start();
			/* [한국어] 모듈에게 종료 시작 통지(I/O 중단 등). */
		}

		if (bdev_module->async_fini_start) {
			/* [한국어] 비동기 fini_start면 완료 대기. */
			return;
			/* [한국어] fini_start_done이 재개. */
		}

		bdev_module = TAILQ_PREV(bdev_module, bdev_module_list, internal.tailq);
		/* [한국어] 동기 모듈이면 이전 모듈로 계속. */
	}

	g_resume_bdev_module = NULL;
	/* [한국어] 모든 모듈 fini_start 완료 → 재개 포인터 초기화. */

	bdev_finish_unregister_bdevs_iter(NULL, 0);
	/* [한국어] 이제 실제 bdev 해제 단계로 진입(첫 호출이므로 인자 NULL/0). */
}

/*
 * [한국어]
 * spdk_bdev_module_fini_start_done - async_fini_start 모듈이 종료-시작 완료를 통지(공개 API).
 *
 * @return: 없음.
 *
 * async_fini_start 모듈이 자신의 fini_start 작업을 마치면 호출해 반복을 이어가게 한다.
 *
 * 실행 컨텍스트: 모듈의 임의 스레드 → app 스레드.
 *
 * 호출 체인:
 *   모듈 → [spdk_bdev_module_fini_start_done] → spdk_thread_exec_msg(app, bdev_module_fini_start_iter)
 */
void
spdk_bdev_module_fini_start_done(void)
{
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), bdev_module_fini_start_iter, NULL);
	/* [한국어] app 스레드에서 fini_start 반복자 재개. */
}

/*
 * [한국어]
 * bdev_finish_wait_for_examine_done - examine 대기 완료 후 모듈 fini_start 단계로 진입.
 *
 * @cb_arg: 미사용.
 * @return: 없음.
 *
 * 진행 중이던 examine이 모두 끝났을 때 호출되는 콜백. 종료 도중 새 bdev examine과
 * 충돌하지 않도록 examine 완료를 기다린 뒤 fini_start 반복을 시작한다.
 *
 * 실행 컨텍스트: app 스레드.
 *
 * 호출 체인:
 *   spdk_bdev_wait_for_examine(완료) → [bdev_finish_wait_for_examine_done] → bdev_module_fini_start_iter
 */
static void
bdev_finish_wait_for_examine_done(void *cb_arg)
{
	bdev_module_fini_start_iter(NULL);
	/* [한국어] examine 완료됨 → 모듈 fini_start 반복 시작. */
}

/* [한국어] 비동기 open 요청들을 종료 시 정리하는 헬퍼의 전방 선언(아래 정의 참조). */
static void bdev_open_async_fini(void);

/*
 * [한국어]
 * bdev_finish_wait_for_examine - 종료 전 진행 중 examine 완료를 대기(app 스레드 본체).
 *
 * @not_used: 메시지 시그니처 충족용.
 * @return: 없음.
 *
 * spdk_bdev_finish가 app 스레드로 보낸 메시지의 본체. examine이 비동기로 끝나기를 기다린 뒤
 * 종료 절차를 이어간다. wait 등록이 실패하면(드물게) 곧바로 완료 콜백을 직접 불러 진행한다.
 *
 * 실행 컨텍스트: app 스레드.
 *
 * 호출 체인:
 *   spdk_bdev_finish → spdk_thread_exec_msg(app) → [bdev_finish_wait_for_examine]
 *     → spdk_bdev_wait_for_examine → bdev_finish_wait_for_examine_done
 */
static void
bdev_finish_wait_for_examine(void *not_used)
{
	int rc;
	/* [한국어] wait_for_examine 등록 결과. */

	rc = spdk_bdev_wait_for_examine(bdev_finish_wait_for_examine_done, NULL);
	/* [한국어] 진행 중인 모든 examine이 끝나면 done 콜백을 부르도록 등록. */
	if (rc != 0) {
		/* [한국어] 등록 자체가 실패하면(예: 콜백 자원 부족). */
		SPDK_ERRLOG("wait_for_examine failed: %s\n", spdk_strerror(-rc));
		bdev_finish_wait_for_examine_done(NULL);
		/* [한국어] 대기 없이 곧바로 다음 단계로 진행(블록 방지). */
	}
}

/* [한국어] spdk_bdev_finish를 app 스레드 외에서 호출하는 것은 deprecated임을 등록(v26.05 제거 예정). */
SPDK_LOG_DEPRECATION_REGISTER(spdk_bdev_finish,
			      "calling spdk_bdev_finish from any thread is deprecated",
			      "v26.05", SPDK_LOG_DEPRECATION_ALWAYS);

/*
 * [한국어]
 * spdk_bdev_finish - bdev 서브시스템 종료 진입점(공개 API).
 *
 * @cb_fn: 종료 완료 시 호출될 사용자 콜백(NULL 불가).
 * @cb_arg: 콜백 인자.
 * @return: 없음(결과는 cb_fn으로 비동기 통지).
 *
 * spdk_bdev_initialize의 거울상. 종료 콜백/스레드를 전역에 저장하고, 비동기 open 요청들을
 * 먼저 정리한 뒤, 실제 종료 흐름(examine 대기 → 모듈 fini_start → bdev unregister → 모듈
 * fini → 자원 회수)을 app 스레드에서 시작한다.
 *
 * 실행 컨텍스트: 임의 스레드(권장 app). app 외 호출은 deprecated 경고.
 *
 * 호출 체인:
 *   사용자/init 서브시스템 → [spdk_bdev_finish]
 *     → bdev_open_async_fini → spdk_thread_exec_msg(app, bdev_finish_wait_for_examine)
 */
void
spdk_bdev_finish(spdk_bdev_fini_cb cb_fn, void *cb_arg)
{
	assert(cb_fn != NULL);
	/* [한국어] 종료 완료 콜백은 필수. */

	if (!spdk_thread_is_app_thread(NULL)) {
		/* [한국어] app 스레드가 아니면. */
		SPDK_LOG_DEPRECATED(spdk_bdev_finish);
		/* [한국어] deprecated 경로 경고. */
	}

	g_fini_cb_fn = cb_fn;
	/* [한국어] 종료 콜백 전역 저장(_bdev_finish_complete가 회수). */
	g_fini_cb_arg = cb_arg;
	/* [한국어] 콜백 인자 전역 저장. */
	g_fini_thread = spdk_get_thread();
	/* [한국어] 종료 시작 스레드 기록 — 완료 콜백을 이 스레드에서 실행(affinity). */

	bdev_open_async_fini();
	/* [한국어] 아직 처리 안 된 비동기 open 요청들을 먼저 취소/정리(dangling 콜백 방지). */
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), bdev_finish_wait_for_examine, NULL);
	/* [한국어] 실제 종료 흐름은 app 스레드에서 시작. */
}

/*
 * [한국어]
 * bdev_channel_get_io - I/O 발행 hot-path에서 bdev_io를 빠르게 획득(per-thread 캐시 우선).
 *
 * @channel: I/O를 발행하려는 bdev 채널(소유 스레드의 mgmt 채널을 통해 캐시 접근).
 * @return: 사용 가능한 bdev_io, 또는 NULL(캐시 비고 대기자 있거나 풀 고갈).
 *
 * 모든 bdev I/O는 먼저 이 함수로 bdev_io 객체를 얻는다. lockless 저지연을 위해 우선
 * per-thread 캐시에서 꺼내고(락 없음 — 소유 스레드만 접근), 캐시가 비었으면 전역 mempool에서
 * 가져온다. 단, io_wait_queue에 대기자가 있으면 새치기를 막기 위해 풀을 건드리지 않고 NULL을
 * 반환한다(공정성). NULL 반환 시 호출자는 spdk_bdev_queue_io_wait로 대기 등록한다.
 *
 * 실행 컨텍스트: 채널 소유 spdk_thread. lockless.
 *
 * 호출 체인:
 *   bdev_io_submit 경로 / spdk_bdev_* I/O API → [bdev_channel_get_io] → spdk_mempool_get
 */
struct spdk_bdev_io *
bdev_channel_get_io(struct spdk_bdev_channel *channel)
{
	struct spdk_bdev_mgmt_channel *ch = channel->shared_resource->mgmt_ch;
	/* [한국어] 이 채널이 속한 스레드의 mgmt 채널(per-thread 캐시 보유). */
	struct spdk_bdev_io *bdev_io;
	/* [한국어] 반환할 bdev_io. */

	if (ch->per_thread_cache_count > 0) {
		/* [한국어] (fast path) 캐시에 여분이 있으면 락 없이 즉시 꺼낸다. */
		bdev_io = STAILQ_FIRST(&ch->per_thread_cache);
		/* [한국어] 캐시 앞에서 하나 꺼냄. */
		STAILQ_REMOVE_HEAD(&ch->per_thread_cache, internal.buf_link);
		/* [한국어] 캐시 리스트에서 제거. */
		ch->per_thread_cache_count--;
		/* [한국어] 캐시 잔량 감소. */
	} else if (spdk_unlikely(!TAILQ_EMPTY(&ch->io_wait_queue))) {
		/*
		 * Don't try to look for bdev_ios in the global pool if there are
		 * waiters on bdev_ios - we don't want this caller to jump the line.
		 */
		/* [한국어] 이미 bdev_io를 기다리는 대기자가 있으면, 이 호출자가 새치기하지 않도록
		 *          전역 풀을 건드리지 않고 NULL을 돌려 공정성을 유지. */
		bdev_io = NULL;
	} else {
		/* [한국어] 캐시 비고 대기자도 없으면 전역 mempool에서 직접 획득. */
		bdev_io = spdk_mempool_get(g_bdev_mgr.bdev_io_pool);
		/* [한국어] 풀도 고갈됐으면 NULL 반환됨(호출자가 wait 처리). */
	}

	return bdev_io;
	/* [한국어] 획득한 bdev_io 또는 NULL. */
}

/*
 * [한국어]
 * spdk_bdev_free_io - 완료된 bdev_io를 캐시/풀로 반환(공개 API, I/O 완료 후 호출).
 *
 * @bdev_io: 완료 처리가 끝난 bdev_io(PENDING 상태가 아니어야 함).
 * @return: 없음.
 *
 * bdev_channel_get_io의 짝. I/O 완료 콜백 처리가 끝난 뒤 호출자가 이 함수로 객체를 돌려준다.
 * 데이터 버퍼가 붙어 있었다면 먼저 반환하고, per-thread 캐시에 여유가 있으면 캐시로 넣으며
 * (그 김에 io_wait_queue 대기자들을 깨워 재시도시킴), 캐시가 가득 차 있으면 전역 mempool로
 * 직접 반환한다.
 *
 * 실행 컨텍스트: bdev_io를 발행했던 채널의 소유 스레드. lockless.
 *
 * 호출 체인:
 *   사용자 완료 콜백 → [spdk_bdev_free_io] → bdev_io_put_buf / entry->cb_fn(대기자 재시도) / spdk_mempool_put
 */
void
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_mgmt_channel *ch;
	/* [한국어] 캐시/대기큐를 가진 mgmt 채널. */

	assert(bdev_io != NULL);
	/* [한국어] 반환 대상은 유효해야 함. */
	assert(bdev_io->internal.status != SPDK_BDEV_IO_STATUS_PENDING);
	/* [한국어] 아직 완료되지 않은(PENDING) I/O를 반환하면 안 됨 — 상태 전이 불변식. */

	ch = bdev_io->internal.ch->shared_resource->mgmt_ch;
	/* [한국어] 이 bdev_io를 발행했던 채널의 mgmt 채널을 찾는다(소유 스레드 캐시). */

	if (bdev_io->internal.f.has_buf) {
		/* [한국어] iobuf에서 빌린 데이터 버퍼가 붙어 있으면. */
		bdev_io_put_buf(bdev_io);
		/* [한국어] 데이터 버퍼를 iobuf 풀로 먼저 반환. */
	}

	if (ch->per_thread_cache_count < ch->bdev_io_cache_size) {
		/* [한국어] 캐시가 목표치 미만이면 캐시로 회수(다음 I/O에서 빠르게 재사용). */
		ch->per_thread_cache_count++;
		/* [한국어] 캐시 잔량 증가. */
		STAILQ_INSERT_HEAD(&ch->per_thread_cache, bdev_io, internal.buf_link);
		/* [한국어] 캐시 앞에 삽입(LIFO — 캐시 지역성). */
		while (ch->per_thread_cache_count > 0 && !TAILQ_EMPTY(&ch->io_wait_queue)) {
			/* [한국어] 캐시에 여유가 생겼으니, bdev_io 부족으로 대기 중이던 요청들을 깨운다. */
			struct spdk_bdev_io_wait_entry *entry;
			/* [한국어] 대기 항목. */

			entry = TAILQ_FIRST(&ch->io_wait_queue);
			/* [한국어] 가장 오래 기다린 대기자(FIFO 앞). */
			TAILQ_REMOVE(&ch->io_wait_queue, entry, link);
			/* [한국어] 대기큐에서 제거. */
			entry->cb_fn(entry->cb_arg);
			/* [한국어] 대기 콜백 호출 — 보통 여기서 다시 bdev_channel_get_io를 시도해 I/O 재발행. */
		}
	} else {
		/* We should never have a full cache with entries on the io wait queue. */
		/* [한국어] 캐시가 가득 차 있는데 대기자가 있으면 안 된다(불변식 위반 검증). */
		assert(TAILQ_EMPTY(&ch->io_wait_queue));
		spdk_mempool_put(g_bdev_mgr.bdev_io_pool, (void *)bdev_io);
		/* [한국어] 캐시가 꽉 찼으므로 전역 mempool로 직접 반환. */
	}
}

/*
 * [한국어]
 * bdev_qos_is_iops_rate_limit - 주어진 QoS 한도 타입이 IOPS 기반인지 판별.
 *
 * @limit: QoS rate-limit 타입(RW_IOPS / RW_BPS / R_BPS / W_BPS).
 * @return: true=IOPS 한도, false=대역폭(BPS) 한도.
 *
 * QoS 토큰 버킷 계산에서 IOPS 한도와 BPS 한도는 단위가 달라(요청 수 vs 바이트 수) 충전·차감
 * 방식이 다르다. 이 헬퍼로 타입을 구분해 적절한 분기를 탄다.
 *
 * 실행 컨텍스트: QoS 설정/계산 경로(주로 QoS poller 소유 스레드). 순수 함수.
 *
 * 호출 체인:
 *   bdev_qos_* 설정/리셋 → [bdev_qos_is_iops_rate_limit]
 */
static bool
bdev_qos_is_iops_rate_limit(enum spdk_bdev_qos_rate_limit_type limit)
{
	assert(limit != SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES);
	/* [한국어] sentinel(타입 개수)을 실제 타입으로 넘기면 안 됨. */

	switch (limit) {
	case SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT:
		/* [한국어] 읽기+쓰기 IOPS 한도 — 유일한 IOPS 기반 타입. */
		return true;
	case SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT:
	case SPDK_BDEV_QOS_R_BPS_RATE_LIMIT:
	case SPDK_BDEV_QOS_W_BPS_RATE_LIMIT:
		/* [한국어] RW/Read/Write 대역폭(바이트/초) 한도 — 모두 BPS 기반. */
		return false;
	case SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES:
	default:
		/* [한국어] 예외/미지정 값은 안전하게 IOPS 아님으로 처리. */
		return false;
	}
}

/*
 * [한국어]
 * bdev_qos_io_to_limit - 이 I/O가 QoS rate-limit 카운팅 대상인지 판별.
 *
 * @bdev_io: 검사할 I/O.
 * @return: true=한도에 포함시켜 토큰을 차감할 I/O, false=무제한(관리성 I/O 등).
 *
 * QoS는 실제 데이터 전송 I/O(READ/WRITE/NVMe IO)만 제한하고, reset/flush 같은 관리 명령은
 * 제한하지 않는다. ZCOPY는 start 단계만(실제 데이터 매핑 시점) 카운팅한다.
 *
 * 실행 컨텍스트: I/O submit 경로(QoS 활성 bdev). 순수 분류 함수.
 *
 * 호출 체인:
 *   bdev_qos_queue_io / bdev_io_do_submit → [bdev_qos_io_to_limit]
 */
static bool
bdev_qos_io_to_limit(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* [한국어] 실제 데이터 전송 I/O — QoS 한도 대상. */
		return true;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		/* [한국어] zero-copy I/O는 start(버퍼 매핑) 단계만 카운팅. */
		if (bdev_io->u.bdev.zcopy.start) {
			/* [한국어] zcopy 시작 요청이면 데이터 전송과 동등하게 취급. */
			return true;
		} else {
			/* [한국어] zcopy end(버퍼 반환)는 중복 카운팅 방지 위해 제외. */
			return false;
		}
	default:
		/* [한국어] reset/flush/unmap 등 그 외 명령은 QoS 비대상. */
		return false;
	}
}

/*
 * [한국어]
 * bdev_is_read_io - I/O가 읽기 방향인지 판별(R-BPS/W-BPS 한도 분기용).
 *
 * @bdev_io: 검사할 I/O.
 * @return: true=읽기, false=쓰기 또는 기타.
 *
 * R_BPS/W_BPS 한도는 방향별로 다른 토큰 버킷을 적용하므로 I/O 방향을 알아야 한다.
 * NVMe passthru는 opcode 비트로, READ는 자명하게, ZCOPY는 populate 플래그(디스크→메모리)로 판별.
 *
 * 실행 컨텍스트: QoS 계산 경로. 순수 함수.
 *
 * 호출 체인:
 *   bdev_qos_r_bps_queue / bdev_qos_w_bps_queue → [bdev_is_read_io]
 */
static bool
bdev_is_read_io(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		/* Bit 1 (0x2) set for read operation */
		/* [한국어] NVMe opcode 비트1(0x2)이 읽기 명령을 나타냄(NVMe spec §3.3 NVM Command Set). */
		if (bdev_io->u.nvme_passthru.cmd.opc & SPDK_NVME_OPC_READ) {
			/* [한국어] READ opcode 비트가 켜져 있으면 읽기. */
			return true;
		} else {
			/* [한국어] 그 외 NVMe 명령은 쓰기로 분류. */
			return false;
		}
	case SPDK_BDEV_IO_TYPE_READ:
		/* [한국어] 일반 READ는 당연히 읽기. */
		return true;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		/* Populate to read from disk */
		/* [한국어] zcopy populate=true는 디스크 데이터를 메모리로 채우므로 읽기에 해당. */
		if (bdev_io->u.bdev.zcopy.populate) {
			return true;
		} else {
			/* [한국어] populate 없는 zcopy(쓰기용 버퍼 매핑)는 쓰기로 분류. */
			return false;
		}
	default:
		/* [한국어] 그 외는 읽기 아님. */
		return false;
	}
}

/*
 * [한국어]
 * bdev_get_io_size_in_byte - I/O가 전송하는 데이터 크기를 바이트로 환산(BPS 한도 차감량).
 *
 * @bdev_io: 검사할 I/O.
 * @return: 전송 바이트 수(QoS 비대상이면 0).
 *
 * BPS(대역폭) 한도는 바이트 단위로 토큰을 차감하므로 각 I/O의 데이터 크기를 알아야 한다.
 * NVMe passthru는 nbytes 필드를, 블록 I/O는 num_blocks * blocklen을 사용한다.
 *
 * 실행 컨텍스트: QoS 계산 경로. 순수 함수.
 *
 * 호출 체인:
 *   bdev_qos_rw_bps_queue / *_rewind_quota → [bdev_get_io_size_in_byte] → bdev_io_get_block_size
 */
static uint64_t
bdev_get_io_size_in_byte(struct spdk_bdev_io *bdev_io)
{
	uint32_t blocklen = bdev_io_get_block_size(bdev_io);
	/* [한국어] 이 I/O 대상 bdev의 블록 크기(바이트). */

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		/* [한국어] NVMe passthru는 전송 바이트를 nbytes에 직접 보유. */
		return bdev_io->u.nvme_passthru.nbytes;
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_UNCORRECTABLE:
		/* [한국어] 블록 I/O는 블록 수 × 블록 크기로 환산. */
		return bdev_io->u.bdev.num_blocks * blocklen;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		/* Track the data in the start phase only */
		/* [한국어] zcopy는 start 단계에서만 데이터 크기를 카운팅(중복 방지). */
		if (bdev_io->u.bdev.zcopy.start) {
			return bdev_io->u.bdev.num_blocks * blocklen;
		} else {
			/* [한국어] zcopy end는 0(이미 start에서 셈). */
			return 0;
		}
	default:
		/* [한국어] 데이터 전송 없는 명령은 0. */
		return 0;
	}
}

/*
 * [한국어]
 * bdev_qos_rw_queue_io - 토큰 버킷에서 delta만큼 차감하고 한도 초과면 큐잉 필요 통지(QoS 코어).
 *
 * @limit: 대상 rate-limit(토큰 버킷 상태 보유).
 * @io: 검사 중인 I/O(여기선 미사용, 시그니처 일관성).
 * @delta: 이번 I/O가 소비하는 양(IOPS면 1, BPS면 바이트 수).
 * @return: true=한도 초과 → I/O를 큐에 보류해야 함, false=허용 → 즉시 발행 가능.
 *
 * QoS 동작의 심장부. remaining_this_timeslice(이번 타임슬라이스에 남은 토큰)에서 delta를
 * 원자적으로 차감한다. 차감 전 잔량이 양수였으면(약간의 오버런 허용) 허용하고, 음수였으면
 * 차감을 되돌리고 큐잉을 지시한다. __ATOMIC_RELAXED를 쓰는 이유: 이 카운터는 단일 채널
 * 스레드에서만 수정되지만 QoS poller가 다른 스레드에서 충전(리필)하므로 원자성은 필요하되
 * 순서 보장은 불필요(값 자체의 정합성만 중요).
 *
 * 오버런 허용: per-timeslice 한도보다 큰 단일 I/O도 가끔 통과시켜 굶지 않게 하고, 초과분은
 * 다음 타임슬라이스에서 poller가 다음 충전량을 계산할 때 반영한다.
 *
 * 실행 컨텍스트: 채널 소유 스레드(I/O submit 경로). 카운터는 atomic.
 *
 * 호출 체인:
 *   bdev_qos_rw_iops_queue / bdev_qos_rw_bps_queue → [bdev_qos_rw_queue_io]
 */
static inline bool
bdev_qos_rw_queue_io(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io, uint64_t delta)
{
	int64_t remaining_this_timeslice;
	/* [한국어] 차감 후의 남은 토큰(부호 있음 — 오버런 감지를 위해). */

	if (!limit->max_per_timeslice) {
		/* The QoS is disabled */
		/* [한국어] 이 한도가 비활성(0)이면 제한하지 않음 — 항상 허용. */
		return false;
	}

	remaining_this_timeslice = __atomic_sub_fetch(&limit->remaining_this_timeslice, delta,
				   __ATOMIC_RELAXED);
	/* [한국어] 토큰을 delta만큼 원자적으로 차감하고 차감 후 값을 받는다(RELAXED: 값 정합성만). */
	if (remaining_this_timeslice + (int64_t)delta > 0) {
		/* There was still a quota for this delta -> the IO shouldn't be queued
		 *
		 * We allow a slight quota overrun here so an IO bigger than the per-timeslice
		 * quota can be allowed once a while. Such overrun then taken into account in
		 * the QoS poller, where the next timeslice quota is calculated.
		 */
		/* [한국어] '차감 전' 잔량(remaining+delta)이 양수였으면 토큰이 있었던 것 → 허용.
		 *          큰 I/O로 음수가 돼도 한 번은 통과시키는 오버런 허용 정책(다음 슬라이스에서 보정). */
		return false;
	}

	/* There was no quota for this delta -> the IO should be queued
	 * The remaining_this_timeslice must be rewinded so it reflects the real
	 * amount of IOs or bytes allowed.
	 */
	/* [한국어] 차감 전에도 토큰이 없었으면(잔량 0 이하) 허용 불가 → 방금 뺀 delta를 도로 더해
	 *          잔량을 원복하고 큐잉을 지시한다(실제 허용량을 정확히 반영). */
	__atomic_add_fetch(
		&limit->remaining_this_timeslice, delta, __ATOMIC_RELAXED);
	/* [한국어] 차감 롤백. */
	return true;
	/* [한국어] 한도 초과 → 호출자가 I/O를 qos_queued_io에 보류. */
}

/*
 * [한국어]
 * bdev_qos_rw_rewind_io - 토큰 버킷에 delta만큼 되돌려 충전(차감 취소).
 *
 * @limit: 대상 한도.
 * @io: 미사용.
 * @delta: 되돌릴 양.
 * @return: 없음.
 *
 * 다차원 한도(예: IOPS와 BPS 동시 적용) 중 하나가 큐잉을 지시하면, 그 전에 통과해 토큰을
 * 이미 차감한 다른 한도들의 차감을 취소해야 정합성이 맞는다. 그 취소(rewind)에 쓰인다.
 *
 * 실행 컨텍스트: 채널 소유 스레드. atomic.
 *
 * 호출 체인:
 *   bdev_qos_*_rewind_quota / bdev_qos_queue_io(롤백) → [bdev_qos_rw_rewind_io]
 */
static inline void
bdev_qos_rw_rewind_io(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io, uint64_t delta)
{
	__atomic_add_fetch(&limit->remaining_this_timeslice, delta, __ATOMIC_RELAXED);
	/* [한국어] 차감했던 delta를 도로 더해 토큰 원복. */
}

/*
 * [한국어]
 * bdev_qos_rw_iops_queue - IOPS 한도에 I/O 1건을 차감(IOPS는 크기 무관 요청당 1).
 *
 * @limit: RW IOPS 한도.
 * @io: 검사 I/O.
 * @return: true=한도 초과로 큐잉, false=허용.
 *
 * 실행 컨텍스트: 채널 스레드.
 * 호출 체인: bdev_qos_queue_io(함수 포인터) → [bdev_qos_rw_iops_queue] → bdev_qos_rw_queue_io
 */
static bool
bdev_qos_rw_iops_queue(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	return bdev_qos_rw_queue_io(limit, io, 1);
	/* [한국어] IOPS는 I/O당 토큰 1 소비. */
}

/*
 * [한국어]
 * bdev_qos_rw_iops_rewind_quota - IOPS 한도 차감 1건을 롤백.
 *
 * @limit: RW IOPS 한도.
 * @io: 미사용.
 * @return: 없음.
 *
 * 실행 컨텍스트: 채널 스레드.
 * 호출 체인: bdev_qos_queue_io(롤백) → [bdev_qos_rw_iops_rewind_quota] → bdev_qos_rw_rewind_io
 */
static void
bdev_qos_rw_iops_rewind_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	bdev_qos_rw_rewind_io(limit, io, 1);
	/* [한국어] IOPS 토큰 1 원복. */
}

/*
 * [한국어]
 * bdev_qos_rw_bps_queue - RW 대역폭 한도에 I/O의 바이트 수만큼 차감.
 *
 * @limit: RW BPS 한도.
 * @io: 검사 I/O.
 * @return: true=한도 초과로 큐잉, false=허용.
 *
 * 실행 컨텍스트: 채널 스레드.
 * 호출 체인: bdev_qos_queue_io → [bdev_qos_rw_bps_queue] → bdev_get_io_size_in_byte / bdev_qos_rw_queue_io
 */
static bool
bdev_qos_rw_bps_queue(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	return bdev_qos_rw_queue_io(limit, io, bdev_get_io_size_in_byte(io));
	/* [한국어] BPS는 I/O의 바이트 수만큼 토큰 소비. */
}

/*
 * [한국어]
 * bdev_qos_rw_bps_rewind_quota - RW 대역폭 한도의 바이트 차감을 롤백.
 *
 * @limit: RW BPS 한도.
 * @io: 검사 I/O.
 * @return: 없음.
 *
 * 실행 컨텍스트: 채널 스레드.
 * 호출 체인: bdev_qos_queue_io(롤백) → [bdev_qos_rw_bps_rewind_quota] → bdev_qos_rw_rewind_io
 */
static void
bdev_qos_rw_bps_rewind_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	bdev_qos_rw_rewind_io(limit, io, bdev_get_io_size_in_byte(io));
	/* [한국어] 차감했던 바이트 수 원복. */
}

/*
 * [한국어]
 * bdev_qos_r_bps_queue - 읽기 전용 대역폭 한도에 차감(쓰기는 면제).
 *
 * @limit: Read BPS 한도.
 * @io: 검사 I/O.
 * @return: true=한도 초과로 큐잉, false=허용(쓰기는 무조건 허용).
 *
 * 실행 컨텍스트: 채널 스레드.
 * 호출 체인: bdev_qos_queue_io → [bdev_qos_r_bps_queue] → bdev_is_read_io / bdev_qos_rw_bps_queue
 */
static bool
bdev_qos_r_bps_queue(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (bdev_is_read_io(io) == false) {
		/* [한국어] 쓰기 I/O는 읽기 대역폭 한도와 무관 → 항상 허용. */
		return false;
	}

	return bdev_qos_rw_bps_queue(limit, io);
	/* [한국어] 읽기 I/O면 바이트 단위로 차감(공통 BPS 로직 재사용). */
}

/*
 * [한국어]
 * bdev_qos_r_bps_rewind_quota - 읽기 대역폭 한도 차감을 롤백(읽기였던 경우만).
 *
 * @limit: Read BPS 한도.
 * @io: 검사 I/O.
 * @return: 없음.
 *
 * 실행 컨텍스트: 채널 스레드.
 * 호출 체인: bdev_qos_queue_io(롤백) → [bdev_qos_r_bps_rewind_quota] → bdev_qos_rw_rewind_io
 */
static void
bdev_qos_r_bps_rewind_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (bdev_is_read_io(io) != false) {
		/* [한국어] 읽기였던 경우에만 차감했으므로 그때만 롤백. */
		bdev_qos_rw_rewind_io(limit, io, bdev_get_io_size_in_byte(io));
	}
}

/*
 * [한국어]
 * bdev_qos_w_bps_queue - 쓰기 전용 대역폭 한도에 차감(읽기는 면제).
 *
 * @limit: Write BPS 한도.
 * @io: 검사 I/O.
 * @return: true=한도 초과로 큐잉, false=허용(읽기는 무조건 허용).
 *
 * 실행 컨텍스트: 채널 스레드.
 * 호출 체인: bdev_qos_queue_io → [bdev_qos_w_bps_queue] → bdev_is_read_io / bdev_qos_rw_bps_queue
 */
static bool
bdev_qos_w_bps_queue(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (bdev_is_read_io(io) == true) {
		/* [한국어] 읽기 I/O는 쓰기 대역폭 한도와 무관 → 항상 허용. */
		return false;
	}

	return bdev_qos_rw_bps_queue(limit, io);
	/* [한국어] 쓰기 I/O면 바이트 단위로 차감. */
}

/*
 * [한국어]
 * bdev_qos_w_bps_rewind_quota - 쓰기 대역폭 한도 차감을 롤백(쓰기였던 경우만).
 *
 * @limit: Write BPS 한도.
 * @io: 검사 I/O.
 * @return: 없음.
 *
 * 실행 컨텍스트: 채널 스레드.
 * 호출 체인: bdev_qos_queue_io(롤백) → [bdev_qos_w_bps_rewind_quota] → bdev_qos_rw_rewind_io
 */
static void
bdev_qos_w_bps_rewind_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (bdev_is_read_io(io) != true) {
		/* [한국어] 쓰기였던 경우에만 차감했으므로 그때만 롤백. */
		bdev_qos_rw_rewind_io(limit, io, bdev_get_io_size_in_byte(io));
	}
}

/*
 * [한국어]
 * bdev_qos_set_ops - 각 rate-limit에 타입별 queue_io/rewind_quota 함수 포인터를 바인딩.
 *
 * @qos: 설정할 QoS 객체.
 * @return: 없음.
 *
 * QoS는 4종 한도(RW IOPS / RW BPS / R BPS / W BPS)를 함수 포인터 디스패치로 처리한다.
 * 이 함수는 설정값(limit)이 정의된 한도에만 해당 핸들러를 꽂고, 미정의(NOT_DEFINED) 한도는
 * queue_io=NULL로 비활성화한다. bdev_qos_queue_io가 이 포인터를 따라 토큰을 차감한다.
 *
 * 실행 컨텍스트: QoS 설정/갱신 시(주로 app 또는 QoS 채널 스레드). 1회성 설정.
 *
 * 호출 체인:
 *   bdev_qos_update_max_quota_per_timeslice / QoS 초기화 → [bdev_qos_set_ops]
 */
static void
bdev_qos_set_ops(struct spdk_bdev_qos *qos)
{
	int i;
	/* [한국어] 4종 한도 순회 인덱스. */

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		/* [한국어] 모든 한도 타입을 순회. */
		if (qos->rate_limits[i].limit == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
			/* [한국어] 설정되지 않은 한도는 핸들러를 비워 비활성화. */
			qos->rate_limits[i].queue_io = NULL;
			continue;
		}

		switch (i) {
		case SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT:
			/* [한국어] RW IOPS 한도 핸들러 바인딩. */
			qos->rate_limits[i].queue_io = bdev_qos_rw_iops_queue;
			qos->rate_limits[i].rewind_quota = bdev_qos_rw_iops_rewind_quota;
			break;
		case SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT:
			/* [한국어] RW 대역폭 한도 핸들러 바인딩. */
			qos->rate_limits[i].queue_io = bdev_qos_rw_bps_queue;
			qos->rate_limits[i].rewind_quota = bdev_qos_rw_bps_rewind_quota;
			break;
		case SPDK_BDEV_QOS_R_BPS_RATE_LIMIT:
			/* [한국어] 읽기 대역폭 한도 핸들러 바인딩. */
			qos->rate_limits[i].queue_io = bdev_qos_r_bps_queue;
			qos->rate_limits[i].rewind_quota = bdev_qos_r_bps_rewind_quota;
			break;
		case SPDK_BDEV_QOS_W_BPS_RATE_LIMIT:
			/* [한국어] 쓰기 대역폭 한도 핸들러 바인딩. */
			qos->rate_limits[i].queue_io = bdev_qos_w_bps_queue;
			qos->rate_limits[i].rewind_quota = bdev_qos_w_bps_rewind_quota;
			break;
		default:
			break;
		}
	}
}

/*
 * [한국어]
 * _bdev_io_complete_in_submit - submit 도중 즉시 완료 처리(outstanding 카운트 정합 유지).
 *
 * @bdev_ch: I/O가 속한 bdev 채널.
 * @bdev_io: 즉시 완료시킬 I/O.
 * @status: 완료 상태(SUCCESS/FAILED 등).
 * @return: 없음.
 *
 * submit 경로에서 모듈 드라이버까지 내려보내지 않고 바로 완료해야 하는 경우(예: abort 성공,
 * write_unit_size 불일치 실패)에 쓴다. spdk_bdev_io_complete는 outstanding 카운터가 증가돼
 * 있다고 가정하므로, 일관성을 위해 increment_outstanding을 먼저 호출하고, in_submit_request
 * 플래그로 "submit 컨텍스트 내 완료"임을 표시해 재진입 처리를 올바르게 한다.
 *
 * 실행 컨텍스트: 채널 소유 스레드(submit 중). 동기.
 *
 * 호출 체인:
 *   bdev_io_do_submit → [_bdev_io_complete_in_submit] → bdev_io_increment_outstanding / spdk_bdev_io_complete
 */
static void
_bdev_io_complete_in_submit(struct spdk_bdev_channel *bdev_ch,
			    struct spdk_bdev_io *bdev_io,
			    enum spdk_bdev_io_status status)
{
	bdev_io->internal.f.in_submit_request = true;
	/* [한국어] submit 컨텍스트 내에서 완료됨을 표시(completion 경로의 재진입 처리에 사용). */
	bdev_io_increment_outstanding(bdev_ch, bdev_ch->shared_resource);
	/* [한국어] complete가 outstanding 감소를 전제하므로 짝을 맞추기 위해 먼저 증가. */
	spdk_bdev_io_complete(bdev_io, status);
	/* [한국어] 주어진 상태로 즉시 완료 콜백 체인 실행. */
	bdev_io->internal.f.in_submit_request = false;
	/* [한국어] submit 컨텍스트 종료 표시. */
}

/*
 * [한국어]
 * bdev_io_do_submit - I/O를 실제로 모듈 드라이버에 제출하거나 nomem 큐로 백프레셔(submit 본체).
 *
 * @bdev_ch: I/O가 속한 bdev 채널.
 * @bdev_io: 제출할 I/O(이미 QoS/split을 통과한 상태).
 * @return: 없음.
 *
 * QoS·split 처리를 마친 I/O가 최종적으로 모듈로 내려가는 지점. 특수 처리 두 가지를 먼저 본다:
 * (1) ABORT 명령은 nomem 큐나 buf 대기 큐에 걸린 대상 I/O를 직접 취소할 수 있으면 그 자리에서
 * 성공 완료; (2) split_on_write_unit bdev에서 num_blocks가 write_unit_size 미만이면 검증 실패.
 * 그 외에는 nomem_io 큐가 비어 있을 때만(앞선 NOMEM I/O를 추월하지 않도록) bdev_submit_request로
 * 모듈에 제출하고, 비어 있지 않으면 자신도 nomem 큐 꼬리에 붙여 순서를 보존한다(백프레셔).
 *
 * 실행 컨텍스트: 채널 소유 스레드. lockless(채널은 단일 스레드 소유). in_submit_request 플래그로
 * 모듈이 동기 완료할 때의 재진입을 표시.
 *
 * 호출 체인:
 *   bdev_qos_io_submit / bdev_io_submit → [bdev_io_do_submit]
 *     → bdev_submit_request(모듈) / bdev_queue_nomem_io_tail / bdev_shared_ch_retry_io
 */
static inline void
bdev_io_do_submit(struct spdk_bdev_channel *bdev_ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	/* [한국어] 대상 bdev. */
	struct spdk_io_channel *ch = bdev_ch->channel;
	/* [한국어] 모듈에 넘길 하부 io_channel(모듈 드라이버의 채널). */
	struct spdk_bdev_shared_resource *shared_resource = bdev_ch->shared_resource;
	/* [한국어] 같은 (bdev,스레드)를 공유하는 채널들의 공용 자원(nomem 큐, outstanding 카운트 등). */

	if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_ABORT)) {
		/* [한국어] (드문 경로) ABORT 명령은 아직 모듈에 안 내려간 I/O를 직접 취소 시도. */
		struct spdk_bdev_mgmt_channel *mgmt_channel = shared_resource->mgmt_ch;
		/* [한국어] buf 대기 I/O를 찾기 위한 mgmt 채널. */
		struct spdk_bdev_io *bio_to_abort = bdev_io->u.abort.bio_to_abort;
		/* [한국어] 취소 대상 I/O. */

		if (bdev_abort_queued_io(&shared_resource->nomem_io, bio_to_abort) ||
		    bdev_abort_buf_io(mgmt_channel, bio_to_abort)) {
			/* [한국어] nomem 큐 또는 buf 대기 큐에서 대상을 찾아 취소했으면(모듈까지 안 갔으면). */
			_bdev_io_complete_in_submit(bdev_ch, bdev_io,
						    SPDK_BDEV_IO_STATUS_SUCCESS);
			/* [한국어] abort 자체를 성공으로 즉시 완료. */
			return;
			/* [한국어] 모듈로 내려보낼 필요 없음. */
		}
		/* [한국어] 못 찾았으면(이미 모듈에 있음) 아래로 진행해 모듈에 abort 제출. */
	}

	if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE &&
			  bdev_io->bdev->split_on_write_unit &&
			  bdev_io->u.bdev.num_blocks < bdev_io->bdev->write_unit_size)) {
		/* [한국어] write_unit 정렬 bdev인데 쓰기 크기가 단위 미만이면 규칙 위반. */
		SPDK_ERRLOG("IO num_blocks %lu does not match the write_unit_size %u\n",
			    bdev_io->u.bdev.num_blocks, bdev_io->bdev->write_unit_size);
		_bdev_io_complete_in_submit(bdev_ch, bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		/* [한국어] 즉시 실패 완료. */
		return;
	}

	if (spdk_likely(TAILQ_EMPTY(&shared_resource->nomem_io))) {
		/* [한국어] (fast path) 대기 중인 NOMEM I/O가 없으면 곧바로 모듈에 제출. */
		bdev_io_increment_outstanding(bdev_ch, shared_resource);
		/* [한국어] outstanding(제출됐으나 미완료) I/O 카운트 증가. */
		bdev_io->internal.f.in_submit_request = true;
		/* [한국어] 모듈이 동기 완료할 경우의 재진입 처리를 위해 submit 중 표시. */
		bdev_submit_request(bdev, ch, bdev_io);
		/* [한국어] 모듈 드라이버의 submit_request 콜백 호출 — 실제 하드웨어/백엔드로 I/O 전달. */
		bdev_io->internal.f.in_submit_request = false;
		/* [한국어] submit 종료 표시. */
	} else {
		/* [한국어] 앞서 NOMEM으로 보류된 I/O가 있으면, 추월하지 않도록 자신도 큐 꼬리에 보류. */
		bdev_queue_nomem_io_tail(shared_resource, bdev_io, BDEV_IO_RETRY_STATE_SUBMIT);
		/* [한국어] nomem 큐 끝에 추가(재시도는 SUBMIT 단계부터). */
		if (shared_resource->nomem_threshold == 0 && shared_resource->io_outstanding == 0) {
			/* Special case when we have nomem IOs and no outstanding IOs which completions
			 * could trigger retry of queued IOs */
			/* [한국어] 미완료 I/O가 0이면 완료 콜백이 재시도를 트리거할 일이 없으므로(데드락),
			 *          여기서 직접 재시도를 한 번 차본다. */
			bdev_shared_ch_retry_io(shared_resource);
		}
	}
}

/*
 * [한국어]
 * bdev_qos_queue_io - I/O를 모든 활성 QoS 한도에 통과시켜 큐잉 여부 결정(다차원 토큰 차감).
 *
 * @qos: QoS 객체(4종 한도 보유).
 * @bdev_io: 검사할 I/O.
 * @return: true=어느 한 한도라도 초과 → I/O를 보류해야 함, false=모든 한도 통과 → 발행 가능.
 *
 * 여러 한도가 동시에 걸릴 수 있으므로(예: IOPS와 BPS), 순서대로 각 한도에서 토큰을 차감한다.
 * 중간에 어느 한도가 초과를 보고하면, 그 전에 이미 통과시켜 토큰을 깎은 한도들의 차감을
 * 역순으로 모두 롤백(rewind)한 뒤 큐잉을 지시한다 — "전부 통과 아니면 아무것도 차감 안 함"
 * 원자성(all-or-nothing)을 보장하기 위함이다.
 *
 * 실행 컨텍스트: 채널 소유 스레드. 토큰 카운터는 atomic.
 *
 * 호출 체인:
 *   bdev_io_submit / bdev_qos_io_submit → [bdev_qos_queue_io]
 *     → rate_limits[i].queue_io / rate_limits[i].rewind_quota
 */
static bool
bdev_qos_queue_io(struct spdk_bdev_qos *qos, struct spdk_bdev_io *bdev_io)
{
	int i;
	/* [한국어] 한도 순회 인덱스. */

	if (bdev_qos_io_to_limit(bdev_io) == true) {
		/* [한국어] QoS 대상 I/O(실데이터 전송)일 때만 한도 검사. */
		for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
			/* [한국어] 4종 한도를 순서대로 검사. */
			if (!qos->rate_limits[i].queue_io) {
				/* [한국어] 비활성(미설정) 한도는 건너뜀. */
				continue;
			}

			if (qos->rate_limits[i].queue_io(&qos->rate_limits[i],
							 bdev_io) == true) {
				/* [한국어] 이 한도가 초과를 보고하면 → 앞서 통과한 한도들의 차감을 되돌려야 함. */
				for (i -= 1; i >= 0 ; i--) {
					/* [한국어] 이미 토큰을 깎은 이전 한도들을 역순으로 롤백. */
					if (!qos->rate_limits[i].queue_io) {
						/* [한국어] 비활성 한도는 차감하지 않았으므로 롤백 불필요. */
						continue;
					}

					qos->rate_limits[i].rewind_quota(&qos->rate_limits[i], bdev_io);
					/* [한국어] 해당 한도의 차감 취소(토큰 원복). */
				}
				return true;
				/* [한국어] 큐잉 필요 — all-or-nothing 보장됨. */
			}
		}
	}

	return false;
	/* [한국어] 모든 한도 통과(또는 QoS 비대상) → 발행 허용. */
}

/*
 * [한국어]
 * bdev_qos_io_submit - QoS 대기 큐에 보류된 I/O들을 토큰이 허용하는 만큼 재발행(poller가 호출).
 *
 * @ch: QoS 채널(qos_queued_io 보유).
 * @qos: QoS 객체(현재 토큰 잔량 반영).
 * @return: 이번에 발행에 성공한 I/O 개수.
 *
 * QoS poller가 매 타임슬라이스마다 토큰을 충전한 뒤, 그동안 한도 초과로 보류돼 있던 I/O들을
 * 앞에서부터 재검사한다. 토큰이 허용하는 I/O는 큐에서 빼서 bdev_io_do_submit으로 내려보내고,
 * 다시 초과를 만나면 그 I/O는 큐에 남겨둔 채 다음 순회로 넘어간다(FIFO 순서 보존).
 *
 * 실행 컨텍스트: QoS 채널 소유 스레드. lockless.
 *
 * 호출 체인:
 *   bdev_channel_poll_qos(poller) → [bdev_qos_io_submit] → bdev_qos_queue_io / bdev_io_do_submit
 */
static int
bdev_qos_io_submit(struct spdk_bdev_channel *ch, struct spdk_bdev_qos *qos)
{
	struct spdk_bdev_io		*bdev_io = NULL, *tmp = NULL;
	/* [한국어] 순회 중인 I/O와 SAFE 순회용 다음 노드. */
	int				submitted_ios = 0;
	/* [한국어] 이번에 발행한 I/O 수(반환값). */

	TAILQ_FOREACH_SAFE(bdev_io, &ch->qos_queued_io, internal.link, tmp) {
		/* [한국어] 보류된 I/O를 앞에서부터(FIFO) 순회 — 도중 제거 가능하므로 SAFE 변형. */
		if (!bdev_qos_queue_io(qos, bdev_io)) {
			/* [한국어] 한도가 허용하면(큐잉 불필요). */
			TAILQ_REMOVE(&ch->qos_queued_io, bdev_io, internal.link);
			/* [한국어] 대기 큐에서 제거. */
			bdev_io_do_submit(ch, bdev_io);
			/* [한국어] 모듈로 실제 발행. */

			submitted_ios++;
			/* [한국어] 발행 카운트 증가. */
		}
		/* [한국어] 여전히 초과면 큐에 남겨두고 다음 I/O로(토큰 소진 시 자연히 멈춤). */
	}

	return submitted_ios;
	/* [한국어] poller가 trace/통계 갱신에 사용. */
}

/*
 * [한국어]
 * bdev_queue_io_wait_with_cb - bdev_io 부족 시 재시도 콜백을 wait 큐에 등록(NOMEM 백프레셔).
 *
 * @bdev_io: bdev_io를 얻지 못해 보류해야 하는 I/O.
 * @cb_fn: bdev_io에 여유가 생겼을 때 호출될 재시도 콜백.
 * @return: 없음.
 *
 * bdev_channel_get_io가 NULL을 반환하면(풀 고갈), 이 함수로 waitq_entry를 채워
 * spdk_bdev_queue_io_wait에 등록한다. 나중에 누군가 bdev_io를 반환하면(spdk_bdev_free_io)
 * 등록된 cb_fn이 호출되어 I/O를 재시도한다. 등록 자체가 실패하면 즉시 실패로 완료시킨다.
 *
 * 실행 컨텍스트: 채널 소유 스레드.
 *
 * 호출 체인:
 *   각 I/O API의 get_io 실패 경로 → [bdev_queue_io_wait_with_cb] → spdk_bdev_queue_io_wait
 */
static void
bdev_queue_io_wait_with_cb(struct spdk_bdev_io *bdev_io, spdk_bdev_io_wait_cb cb_fn)
{
	int rc;
	/* [한국어] wait 등록 결과. */

	bdev_io->internal.waitq_entry.bdev = bdev_io->bdev;
	/* [한국어] 대기 항목에 대상 bdev 기록. */
	bdev_io->internal.waitq_entry.cb_fn = cb_fn;
	/* [한국어] 여유 발생 시 호출할 재시도 콜백. */
	bdev_io->internal.waitq_entry.cb_arg = bdev_io;
	/* [한국어] 콜백 인자(자기 자신 — 재시도 시 같은 I/O 사용). */
	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, spdk_io_channel_from_ctx(bdev_io->internal.ch),
				     &bdev_io->internal.waitq_entry);
	/* [한국어] 채널의 io_wait_queue에 대기 항목 등록. */
	if (rc != 0) {
		/* [한국어] 등록 실패(드묾) 시. */
		SPDK_ERRLOG("Queue IO failed, rc=%d\n", rc);
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		/* [한국어] I/O를 실패 상태로 표시. */
		bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
		/* [한국어] 사용자 완료 콜백을 실패(success=false)로 즉시 호출. */
	}
}

static inline uint32_t
bdev_rw_get_io_boundary(struct spdk_bdev *bdev, uint8_t io_type)
{
	uint32_t io_boundary;

	if (io_type == SPDK_BDEV_IO_TYPE_WRITE && bdev->split_on_write_unit) {
		io_boundary = bdev->write_unit_size;
	} else if (bdev->split_on_optimal_io_boundary) {
		io_boundary = bdev->optimal_io_boundary;
	} else {
		io_boundary = 0;
	}

	return io_boundary;
}

static bool
bdev_rw_should_split(struct spdk_bdev_io *bdev_io)
{
	uint32_t io_boundary;
	struct spdk_bdev *bdev = bdev_io->bdev;
	uint32_t max_segment_size = bdev->max_segment_size;
	uint32_t max_size = bdev->max_rw_size;
	int max_segs = bdev->max_num_segments;

	io_boundary = bdev_rw_get_io_boundary(bdev, bdev_io->type);

	if (spdk_likely(!io_boundary && !max_segs && !max_segment_size && !max_size)) {
		return false;
	}

	if (io_boundary) {
		uint64_t start_stripe, end_stripe;

		start_stripe = bdev_io->u.bdev.offset_blocks;
		end_stripe = start_stripe + bdev_io->u.bdev.num_blocks - 1;
		/* Avoid expensive div operations if possible.  These spdk_u32 functions are very cheap. */
		if (spdk_likely(spdk_u32_is_pow2(io_boundary))) {
			start_stripe >>= spdk_u32log2(io_boundary);
			end_stripe >>= spdk_u32log2(io_boundary);
		} else {
			start_stripe /= io_boundary;
			end_stripe /= io_boundary;
		}

		if (start_stripe != end_stripe) {
			return true;
		}
	}

	if (max_segs) {
		if (bdev_io->u.bdev.iovcnt > max_segs) {
			return true;
		}
	}

	if (max_segment_size) {
		for (int i = 0; i < bdev_io->u.bdev.iovcnt; i++) {
			if (bdev_io->u.bdev.iovs[i].iov_len > max_segment_size) {
				return true;
			}
		}
	}

	if (max_size) {
		if (bdev_io->u.bdev.num_blocks > max_size) {
			return true;
		}
	}

	return false;
}

static bool
bdev_unmap_should_split(struct spdk_bdev_io *bdev_io)
{
	uint32_t num_unmap_segments;

	if (!bdev_io->bdev->max_unmap || !bdev_io->bdev->max_unmap_segments) {
		return false;
	}
	num_unmap_segments = spdk_divide_round_up(bdev_io->u.bdev.num_blocks, bdev_io->bdev->max_unmap);
	if (num_unmap_segments > bdev_io->bdev->max_unmap_segments) {
		return true;
	}

	return false;
}

static bool
bdev_write_zeroes_should_split(struct spdk_bdev_io *bdev_io)
{
	if (!bdev_io->bdev->max_write_zeroes) {
		return false;
	}

	if (bdev_io->u.bdev.num_blocks > bdev_io->bdev->max_write_zeroes) {
		return true;
	}

	return false;
}

static bool
bdev_copy_should_split(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io->bdev->max_copy != 0 &&
	    bdev_io->u.bdev.num_blocks > bdev_io->bdev->max_copy) {
		return true;
	}

	return false;
}

static bool
bdev_io_should_split(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return bdev_rw_should_split(bdev_io);
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return bdev_unmap_should_split(bdev_io);
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		return bdev_write_zeroes_should_split(bdev_io);
	case SPDK_BDEV_IO_TYPE_COPY:
		return bdev_copy_should_split(bdev_io);
	default:
		return false;
	}
}

static uint32_t
_to_next_boundary(uint64_t offset, uint32_t boundary)
{
	return (boundary - (offset % boundary));
}

static void bdev_io_split_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

static void _bdev_rw_split(void *_bdev_io);

static void bdev_unmap_split(struct spdk_bdev_io *bdev_io);

static void
_bdev_unmap_split(void *_bdev_io)
{
	bdev_unmap_split((struct spdk_bdev_io *)_bdev_io);
}

static void bdev_write_zeroes_split(struct spdk_bdev_io *bdev_io);

static void
_bdev_write_zeroes_split(void *_bdev_io)
{
	bdev_write_zeroes_split((struct spdk_bdev_io *)_bdev_io);
}

static void bdev_copy_split(struct spdk_bdev_io *bdev_io);

static void
_bdev_copy_split(void *_bdev_io)
{
	bdev_copy_split((struct spdk_bdev_io *)_bdev_io);
}

static int
bdev_io_split_submit(struct spdk_bdev_io *bdev_io, struct iovec *iov, int iovcnt, void *md_buf,
		     uint64_t num_blocks, uint64_t *offset, uint64_t *remaining)
{
	int rc;
	uint64_t current_offset, current_remaining, current_src_offset;
	spdk_bdev_io_wait_cb io_wait_fn;

	current_offset = *offset;
	current_remaining = *remaining;

	assert(bdev_io->internal.f.split);

	bdev_io->internal.split.outstanding++;

	io_wait_fn = _bdev_rw_split;
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		assert(bdev_io->u.bdev.accel_sequence == NULL);
		rc = bdev_readv_blocks_with_md(bdev_io->internal.desc,
					       spdk_io_channel_from_ctx(bdev_io->internal.ch),
					       iov, iovcnt, md_buf, current_offset,
					       num_blocks,
					       bdev_io_use_memory_domain(bdev_io) ? bdev_io->u.bdev.memory_domain : NULL,
					       bdev_io_use_memory_domain(bdev_io) ? bdev_io->u.bdev.memory_domain_ctx : NULL,
					       NULL,
					       bdev_io->u.bdev.dif_check_flags,
					       bdev_io_split_done, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		assert(bdev_io->u.bdev.accel_sequence == NULL);
		rc = bdev_writev_blocks_with_md(bdev_io->internal.desc,
						spdk_io_channel_from_ctx(bdev_io->internal.ch),
						iov, iovcnt, md_buf, current_offset,
						num_blocks,
						bdev_io_use_memory_domain(bdev_io) ? bdev_io->u.bdev.memory_domain : NULL,
						bdev_io_use_memory_domain(bdev_io) ? bdev_io->u.bdev.memory_domain_ctx : NULL,
						NULL,
						bdev_io->u.bdev.dif_check_flags,
						bdev_io->u.bdev.nvme_cdw12.raw,
						bdev_io->u.bdev.nvme_cdw13.raw,
						bdev_io_split_done, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		io_wait_fn = _bdev_unmap_split;
		rc = spdk_bdev_unmap_blocks(bdev_io->internal.desc,
					    spdk_io_channel_from_ctx(bdev_io->internal.ch),
					    current_offset, num_blocks,
					    bdev_io_split_done, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		io_wait_fn = _bdev_write_zeroes_split;
		rc = spdk_bdev_write_zeroes_blocks(bdev_io->internal.desc,
						   spdk_io_channel_from_ctx(bdev_io->internal.ch),
						   current_offset, num_blocks,
						   bdev_io_split_done, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		io_wait_fn = _bdev_copy_split;
		current_src_offset = bdev_io->u.bdev.copy.src_offset_blocks +
				     (current_offset - bdev_io->u.bdev.offset_blocks);
		rc = spdk_bdev_copy_blocks(bdev_io->internal.desc,
					   spdk_io_channel_from_ctx(bdev_io->internal.ch),
					   current_offset, current_src_offset, num_blocks,
					   bdev_io_split_done, bdev_io);
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	if (rc == 0) {
		current_offset += num_blocks;
		current_remaining -= num_blocks;
		bdev_io->internal.split.current_offset_blocks = current_offset;
		bdev_io->internal.split.remaining_num_blocks = current_remaining;
		*offset = current_offset;
		*remaining = current_remaining;
	} else {
		bdev_io->internal.split.outstanding--;
		if (rc == -ENOMEM) {
			if (bdev_io->internal.split.outstanding == 0) {
				/* No I/O is outstanding. Hence we should wait here. */
				bdev_queue_io_wait_with_cb(bdev_io, io_wait_fn);
			}
		} else {
			bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
			if (bdev_io->internal.split.outstanding == 0) {
				bdev_ch_remove_from_io_submitted(bdev_io);
				spdk_trace_record(TRACE_BDEV_IO_DONE, bdev_io->internal.ch->trace_id,
						  0, (uintptr_t)bdev_io, bdev_io->internal.caller_ctx,
						  bdev_io->internal.ch->queue_depth);
				bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
			}
		}
	}

	return rc;
}

static void
_bdev_rw_split(void *_bdev_io)
{
	struct iovec *parent_iov, *iov;
	struct spdk_bdev_io *bdev_io = _bdev_io;
	struct spdk_bdev *bdev = bdev_io->bdev;
	uint64_t parent_offset, current_offset, remaining;
	uint32_t parent_iov_offset, parent_iovcnt, parent_iovpos, child_iovcnt;
	uint32_t to_next_boundary, to_next_boundary_bytes, to_last_block_bytes;
	uint32_t iovcnt, iov_len, child_iovsize;
	uint32_t blocklen;
	uint32_t io_boundary;
	uint32_t max_segment_size = bdev->max_segment_size;
	uint32_t max_child_iovcnt = bdev->max_num_segments;
	uint32_t max_size = bdev->max_rw_size;
	void *md_buf = NULL;
	int rc;

	blocklen = bdev_io_get_block_size(bdev_io);

	max_size = max_size ? max_size : UINT32_MAX;
	max_segment_size = max_segment_size ? max_segment_size : UINT32_MAX;
	max_child_iovcnt = max_child_iovcnt ? spdk_min(max_child_iovcnt, SPDK_BDEV_IO_NUM_CHILD_IOV) :
			   SPDK_BDEV_IO_NUM_CHILD_IOV;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE && bdev->split_on_write_unit) {
		io_boundary = bdev->write_unit_size;
	} else if (bdev->split_on_optimal_io_boundary) {
		io_boundary = bdev->optimal_io_boundary;
	} else {
		io_boundary = UINT32_MAX;
	}

	assert(bdev_io->internal.f.split);

	remaining = bdev_io->internal.split.remaining_num_blocks;
	current_offset = bdev_io->internal.split.current_offset_blocks;
	parent_offset = bdev_io->u.bdev.offset_blocks;
	parent_iov_offset = (current_offset - parent_offset) * blocklen;
	parent_iovcnt = bdev_io->u.bdev.iovcnt;

	for (parent_iovpos = 0; parent_iovpos < parent_iovcnt; parent_iovpos++) {
		parent_iov = &bdev_io->u.bdev.iovs[parent_iovpos];
		if (parent_iov_offset < parent_iov->iov_len) {
			break;
		}
		parent_iov_offset -= parent_iov->iov_len;
	}

	child_iovcnt = 0;
	while (remaining > 0 && parent_iovpos < parent_iovcnt &&
	       child_iovcnt < SPDK_BDEV_IO_NUM_CHILD_IOV) {
		to_next_boundary = _to_next_boundary(current_offset, io_boundary);
		to_next_boundary = spdk_min(remaining, to_next_boundary);
		to_next_boundary = spdk_min(max_size, to_next_boundary);
		to_next_boundary_bytes = to_next_boundary * blocklen;

		iov = &bdev_io->child_iov[child_iovcnt];
		iovcnt = 0;

		if (bdev_io->u.bdev.md_buf) {
			md_buf = (char *)bdev_io->u.bdev.md_buf +
				 (current_offset - parent_offset) * spdk_bdev_get_md_size(bdev);
		}

		child_iovsize = spdk_min(SPDK_BDEV_IO_NUM_CHILD_IOV - child_iovcnt, max_child_iovcnt);
		while (to_next_boundary_bytes > 0 && parent_iovpos < parent_iovcnt &&
		       iovcnt < child_iovsize) {
			parent_iov = &bdev_io->u.bdev.iovs[parent_iovpos];
			iov_len = parent_iov->iov_len - parent_iov_offset;

			iov_len = spdk_min(iov_len, max_segment_size);
			iov_len = spdk_min(iov_len, to_next_boundary_bytes);
			to_next_boundary_bytes -= iov_len;

			bdev_io->child_iov[child_iovcnt].iov_base = parent_iov->iov_base + parent_iov_offset;
			bdev_io->child_iov[child_iovcnt].iov_len = iov_len;

			if (iov_len < parent_iov->iov_len - parent_iov_offset) {
				parent_iov_offset += iov_len;
			} else {
				parent_iovpos++;
				parent_iov_offset = 0;
			}
			child_iovcnt++;
			iovcnt++;
		}

		if (to_next_boundary_bytes > 0) {
			/* We had to stop this child I/O early because we ran out of
			 * child_iov space or were limited by max_num_segments.
			 * Ensure the iovs to be aligned with block size and
			 * then adjust to_next_boundary before starting the
			 * child I/O.
			 */
			assert(child_iovcnt == SPDK_BDEV_IO_NUM_CHILD_IOV ||
			       iovcnt == child_iovsize);
			to_last_block_bytes = to_next_boundary_bytes % blocklen;
			if (to_last_block_bytes != 0) {
				uint32_t child_iovpos = child_iovcnt - 1;
				/* don't decrease child_iovcnt when it equals to SPDK_BDEV_IO_NUM_CHILD_IOV
				 * so the loop will naturally end
				 */

				to_last_block_bytes = blocklen - to_last_block_bytes;
				to_next_boundary_bytes += to_last_block_bytes;
				while (to_last_block_bytes > 0 && iovcnt > 0) {
					iov_len = spdk_min(to_last_block_bytes,
							   bdev_io->child_iov[child_iovpos].iov_len);
					bdev_io->child_iov[child_iovpos].iov_len -= iov_len;
					if (bdev_io->child_iov[child_iovpos].iov_len == 0) {
						child_iovpos--;
						if (--iovcnt == 0) {
							/* If the child IO is less than a block size just return.
							 * If the first child IO of any split round is less than
							 * a block size, an error exit.
							 */
							if (bdev_io->internal.split.outstanding == 0) {
								SPDK_ERRLOG("The first child io was less than a block size\n");
								bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
								bdev_ch_remove_from_io_submitted(bdev_io);
								spdk_trace_record(TRACE_BDEV_IO_DONE, bdev_io->internal.ch->trace_id,
										  0, (uintptr_t)bdev_io, bdev_io->internal.caller_ctx,
										  bdev_io->internal.ch->queue_depth);
								bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
							}

							return;
						}
					}

					to_last_block_bytes -= iov_len;

					if (parent_iov_offset == 0) {
						parent_iovpos--;
						parent_iov_offset = bdev_io->u.bdev.iovs[parent_iovpos].iov_len;
					}
					parent_iov_offset -= iov_len;
				}

				assert(to_last_block_bytes == 0);
			}
			to_next_boundary -= to_next_boundary_bytes / blocklen;
		}

		rc = bdev_io_split_submit(bdev_io, iov, iovcnt, md_buf, to_next_boundary,
					  &current_offset, &remaining);
		if (spdk_unlikely(rc)) {
			return;
		}
	}
}

static void
bdev_unmap_split(struct spdk_bdev_io *bdev_io)
{
	uint64_t offset, unmap_blocks, remaining, max_unmap_blocks;
	uint32_t num_children_reqs = 0;
	int rc;

	assert(bdev_io->internal.f.split);

	offset = bdev_io->internal.split.current_offset_blocks;
	remaining = bdev_io->internal.split.remaining_num_blocks;
	max_unmap_blocks = bdev_io->bdev->max_unmap * bdev_io->bdev->max_unmap_segments;

	while (remaining && (num_children_reqs < SPDK_BDEV_MAX_CHILDREN_UNMAP_WRITE_ZEROES_REQS)) {
		unmap_blocks = spdk_min(remaining, max_unmap_blocks);

		rc = bdev_io_split_submit(bdev_io, NULL, 0, NULL, unmap_blocks,
					  &offset, &remaining);
		if (spdk_likely(rc == 0)) {
			num_children_reqs++;
		} else {
			return;
		}
	}
}

static void
bdev_write_zeroes_split(struct spdk_bdev_io *bdev_io)
{
	uint64_t offset, write_zeroes_blocks, remaining;
	uint32_t num_children_reqs = 0;
	int rc;

	assert(bdev_io->internal.f.split);

	offset = bdev_io->internal.split.current_offset_blocks;
	remaining = bdev_io->internal.split.remaining_num_blocks;

	while (remaining && (num_children_reqs < SPDK_BDEV_MAX_CHILDREN_UNMAP_WRITE_ZEROES_REQS)) {
		write_zeroes_blocks = spdk_min(remaining, bdev_io->bdev->max_write_zeroes);

		rc = bdev_io_split_submit(bdev_io, NULL, 0, NULL, write_zeroes_blocks,
					  &offset, &remaining);
		if (spdk_likely(rc == 0)) {
			num_children_reqs++;
		} else {
			return;
		}
	}
}

static void
bdev_copy_split(struct spdk_bdev_io *bdev_io)
{
	uint64_t offset, copy_blocks, remaining;
	uint32_t num_children_reqs = 0;
	int rc;

	assert(bdev_io->internal.f.split);

	offset = bdev_io->internal.split.current_offset_blocks;
	remaining = bdev_io->internal.split.remaining_num_blocks;

	assert(bdev_io->bdev->max_copy != 0);
	while (remaining && (num_children_reqs < SPDK_BDEV_MAX_CHILDREN_COPY_REQS)) {
		copy_blocks = spdk_min(remaining, bdev_io->bdev->max_copy);

		rc = bdev_io_split_submit(bdev_io, NULL, 0, NULL, copy_blocks,
					  &offset, &remaining);
		if (spdk_likely(rc == 0)) {
			num_children_reqs++;
		} else {
			return;
		}
	}
}

static void
parent_bdev_io_complete(void *ctx, int rc)
{
	struct spdk_bdev_io *parent_io = ctx;

	if (rc) {
		parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	parent_io->internal.cb(parent_io, parent_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS,
			       parent_io->internal.caller_ctx);
}

static void
bdev_io_complete_parent_sequence_cb(void *ctx, int status)
{
	struct spdk_bdev_io *bdev_io = ctx;

	/* u.bdev.accel_sequence should have already been cleared at this point */
	assert(bdev_io->u.bdev.accel_sequence == NULL);
	assert(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	bdev_io->internal.f.has_accel_sequence = false;

	if (spdk_unlikely(status != 0)) {
		SPDK_ERRLOG("Failed to execute accel sequence, status=%d\n", status);
	}

	parent_bdev_io_complete(bdev_io, status);
}

static void
bdev_io_split_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;
	bool use_accel_sequence;
	void *caller_ctx;

	assert(parent_io->internal.f.split);

	if (!success) {
		parent_io->internal.status = bdev_io->internal.status;
		parent_io->internal.error = bdev_io->internal.error;
		/* If any child I/O failed, stop further splitting process. */
		parent_io->internal.split.current_offset_blocks += parent_io->internal.split.remaining_num_blocks;
		parent_io->internal.split.remaining_num_blocks = 0;
	}

	use_accel_sequence = bdev_io_use_accel_sequence(bdev_io);
	caller_ctx = bdev_io->internal.caller_ctx;
	spdk_bdev_free_io(bdev_io);

	parent_io->internal.split.outstanding--;
	if (parent_io->internal.split.outstanding != 0) {
		return;
	}

	/*
	 * Parent I/O finishes when all blocks are consumed.
	 */
	if (parent_io->internal.split.remaining_num_blocks == 0) {
		assert(parent_io->internal.cb != bdev_io_split_done);
		bdev_ch_remove_from_io_submitted(parent_io);
		spdk_trace_record(TRACE_BDEV_IO_DONE, parent_io->internal.ch->trace_id,
				  0, (uintptr_t)parent_io, caller_ctx,
				  parent_io->internal.ch->queue_depth);

		if (spdk_likely(parent_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS)) {
			if (bdev_io_needs_sequence_exec(parent_io)) {
				bdev_io_exec_sequence(parent_io, bdev_io_complete_parent_sequence_cb);
				return;
			} else if (parent_io->internal.f.has_bounce_buf &&
				   !use_accel_sequence) {
				/* bdev IO will be completed in the callback */
				_bdev_io_push_bounce_data_buffer(parent_io, parent_bdev_io_complete);
				return;
			}
		}

		parent_bdev_io_complete(parent_io, 0);
		return;
	}

	/*
	 * Continue with the splitting process.  This function will complete the parent I/O if the
	 * splitting is done.
	 */
	switch (parent_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		_bdev_rw_split(parent_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		bdev_unmap_split(parent_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		bdev_write_zeroes_split(parent_io);
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		bdev_copy_split(parent_io);
		break;
	default:
		assert(false);
		break;
	}
}

static void bdev_rw_split_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
				     bool success);

static void
bdev_io_split(struct spdk_bdev_io *bdev_io)
{
	assert(bdev_io_should_split(bdev_io));
	assert(bdev_io->internal.f.split);

	bdev_io->internal.split.current_offset_blocks = bdev_io->u.bdev.offset_blocks;
	bdev_io->internal.split.remaining_num_blocks = bdev_io->u.bdev.num_blocks;
	bdev_io->internal.split.outstanding = 0;
	bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		if (_is_buf_allocated(bdev_io->u.bdev.iovs)) {
			_bdev_rw_split(bdev_io);
		} else {
			assert(bdev_io->type == SPDK_BDEV_IO_TYPE_READ);
			spdk_bdev_io_get_buf(bdev_io, bdev_rw_split_get_buf_cb,
					     bdev_io->u.bdev.num_blocks * bdev_io_get_block_size(bdev_io));
		}
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		bdev_unmap_split(bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		bdev_write_zeroes_split(bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		bdev_copy_split(bdev_io);
		break;
	default:
		assert(false);
		break;
	}
}

static void
bdev_rw_split_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	_bdev_rw_split(bdev_io);
}

static inline void
_bdev_io_submit(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_bdev_channel *bdev_ch = bdev_io->internal.ch;

	if (spdk_likely(bdev_ch->flags == 0)) {
		bdev_io_do_submit(bdev_ch, bdev_io);
		return;
	}

	if (bdev_ch->flags & BDEV_CH_RESET_IN_PROGRESS) {
		_bdev_io_complete_in_submit(bdev_ch, bdev_io, SPDK_BDEV_IO_STATUS_ABORTED);
	} else if (bdev_ch->flags & BDEV_CH_QOS_ENABLED) {
		if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_ABORT) &&
		    bdev_abort_queued_io(&bdev_ch->qos_queued_io, bdev_io->u.abort.bio_to_abort)) {
			_bdev_io_complete_in_submit(bdev_ch, bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			TAILQ_INSERT_TAIL(&bdev_ch->qos_queued_io, bdev_io, internal.link);
			bdev_qos_io_submit(bdev_ch, bdev->internal.qos);
		}
	} else {
		SPDK_ERRLOG("unknown bdev_ch flag %x found\n", bdev_ch->flags);
		_bdev_io_complete_in_submit(bdev_ch, bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

bool bdev_lba_range_overlapped(struct lba_range *range1, struct lba_range *range2);

bool
bdev_lba_range_overlapped(struct lba_range *range1, struct lba_range *range2)
{
	if (range1->length == 0 || range2->length == 0) {
		return false;
	}

	if (range1->offset + range1->length <= range2->offset) {
		return false;
	}

	if (range2->offset + range2->length <= range1->offset) {
		return false;
	}

	return true;
}

static bool
bdev_io_range_is_locked(struct spdk_bdev_io *bdev_io, struct lba_range *range)
{
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	struct lba_range r;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		/* Don't try to decode the NVMe command - just assume worst-case and that
		 * it overlaps a locked range.
		 */
		return true;
	case SPDK_BDEV_IO_TYPE_READ:
		if (!range->quiesce) {
			return false;
		}
	/* fallthrough */
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_UNCORRECTABLE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_ZCOPY:
	case SPDK_BDEV_IO_TYPE_COPY:
		r.offset = bdev_io->u.bdev.offset_blocks;
		r.length = bdev_io->u.bdev.num_blocks;
		if (!bdev_lba_range_overlapped(range, &r)) {
			/* This I/O doesn't overlap the specified LBA range. */
			return false;
		} else if (range->owner_ch == ch && range->locked_ctx == bdev_io->internal.caller_ctx) {
			/* This I/O overlaps, but the I/O is on the same channel that locked this
			 * range, and the caller_ctx is the same as the locked_ctx.  This means
			 * that this I/O is associated with the lock, and is allowed to execute.
			 */
			return false;
		} else {
			return true;
		}
	default:
		return false;
	}
}

/*
 * [한국어]
 * bdev_io_submit - 준비된 bdev_io를 bdev 모듈로 전달 (split/lba_lock 게이트 포함).
 *
 * @bdev_io: bdev_io_init() 이후 호출자가 모든 필드(iov, offset, num_blocks, type 등)를
 *           채운 객체. 호출 직전이 핸드오프 시점이며, 호출 이후에는 SPDK 코어/모듈이 소유.
 *
 * 본 함수는 bdev_internal.h가 노출하는 내부 진입점으로 lib/bdev/{bdev_zone,part}.c와
 * 본 파일의 공개 spdk_bdev_* API 모두가 마지막에 호출한다. 처리 단계:
 *   1) LBA range lock 검사 — 부모가 split 중이거나 사용자가 lock 걸어둔 영역이면 io_locked 큐 대기.
 *      (child I/O는 부모가 이미 통과했으므로 패스.)
 *   2) io_submitted 큐에 추가, QD++. submit timestamp 기록 (latency 측정용).
 *   3) SPDK trace point 발행 — TRACE_BDEV_IO_START.
 *   4) split 필요하면 bdev_io_split으로 분기 (child로 쪼개 발행 후 부모는 대기).
 *   5) 일반 경로면 _bdev_io_submit으로 모듈 콜백 호출 (QoS/nomem 게이트 포함).
 *
 * 실행 컨텍스트: 채널 소유 spdk_thread. 락 없음.
 *
 * 호출자(caller): bdev_zone.c의 zone_* API, part.c의 forwarding, 본 파일의 공개 API.
 * 호출처(callee): bdev_io_split (분할 필요 시), _bdev_io_submit (일반 케이스),
 *                  io_locked 큐 enqueue (LBA 잠금 시).
 *
 * 호출 체인:
 *   spdk_bdev_read/write/...
 *     → bdev_channel_get_io → bdev_io_init
 *     → [bdev_io_submit] → _bdev_io_submit → bdev_submit_request
 *         → 모듈 fn_table->submit_request → 디바이스 명령 → 완료 → bdev_io_complete
 */
void
bdev_io_submit(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	/* [한국어] 이 I/O가 속한 채널. 채널 lifetime은 I/O 완료까지 보장. */

	assert(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_PENDING);
	/* [한국어] submit 시점 status는 반드시 PENDING — init 후 다른 경로로 status가 바뀌었다면 버그. */

	/* Child I/Os are not checked against locked ranges because their parent I/O was already
	 * checked before splitting, so they must be allowed to proceed. */
	if (!bdev_io->internal.f.child_io && !TAILQ_EMPTY(&ch->locked_ranges)) {
		/* [한국어] 부모 I/O이면서 (=split에서 파생된 child가 아니면서) 채널에 LBA 잠금이 있으면 검사.
		 *  child는 부모가 이미 통과한 검사를 재실행하지 않음 (deadlock 방지). */
		struct lba_range *range;
		/* [한국어] 잠금 범위 iterator. */

		TAILQ_FOREACH(range, &ch->locked_ranges, tailq) {
			/* [한국어] 채널의 모든 LBA 잠금 순회. */
			if (bdev_io_range_is_locked(bdev_io, range)) {
				/* [한국어] 본 I/O가 이 잠금 범위와 겹침 — 진입 금지. */
				TAILQ_INSERT_TAIL(&ch->io_locked, bdev_io, internal.ch_link);
				/* [한국어] io_locked 큐에 대기 — unlock 시 drain됨. */
				return;
			}
		}
	}

	bdev_ch_add_to_io_submitted(bdev_io);
	/* [한국어] io_submitted 큐에 추가 + QD++. reset/timeout 검사 대상이 됨. */

	bdev_io->internal.submit_tsc = spdk_get_ticks();
	/* [한국어] 현재 tsc 기록 — 완료 시 latency = complete_tsc - submit_tsc 계산. */
	spdk_trace_record_tsc(bdev_io->internal.submit_tsc, TRACE_BDEV_IO_START,
			      ch->trace_id, bdev_io->u.bdev.num_blocks,
			      (uintptr_t)bdev_io, (uint64_t)bdev_io->type, bdev_io->internal.caller_ctx,
			      bdev_io->u.bdev.offset_blocks, ch->queue_depth);
	/* [한국어] SPDK trace tool용 이벤트 — bdev_io 시작. 외부 tool이 timeline으로 시각화. */

	if (bdev_io->internal.f.split) {
		/* [한국어] 분할 발행 모드 — 모듈 max_segments/max_size 초과한 큰 I/O. */
		bdev_io_split(bdev_io);
		/* [한국어] 본 I/O를 child들로 쪼개 발행. 부모는 child 완료 시까지 대기. */
		return;
	}

	_bdev_io_submit(bdev_io);
	/* [한국어] 일반 경로 — QoS/nomem 게이트 통과 후 모듈 submit_request 호출. */
}

static inline int
bdev_io_init_dif_ctx(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_dif_ctx_init_ext_opts dif_opts;

	memset(&bdev_io->u.bdev.dif_err, 0, sizeof(struct spdk_dif_error));

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = bdev->dif_pi_format;

	return spdk_dif_ctx_init(&bdev_io->u.bdev.dif_ctx,
				 bdev->blocklen,
				 bdev->md_len,
				 bdev->md_interleave,
				 bdev->dif_is_head_of_md,
				 bdev->dif_type,
				 bdev_io->u.bdev.dif_check_flags,
				 bdev_io->u.bdev.offset_blocks & 0xFFFFFFFF,
				 0xFFFF, 0, 0, 0, &dif_opts);
}

static void
_bdev_memory_domain_get_io_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
			      bool success)
{
	if (!success) {
		SPDK_ERRLOG("Failed to get data buffer, completing IO\n");
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		bdev_io_complete_unsubmitted(bdev_io);
		return;
	}

	if (bdev_io_needs_sequence_exec(bdev_io)) {
		if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
			bdev_io_exec_sequence(bdev_io, bdev_io_submit_sequence_cb);
			return;
		}
		/* For reads we'll execute the sequence after the data is read, so, for now, only
		 * clear out accel_sequence pointer and submit the IO */
		assert(bdev_io->type == SPDK_BDEV_IO_TYPE_READ);
		bdev_io->u.bdev.accel_sequence = NULL;
	}

	bdev_io_submit(bdev_io);
}

static inline void
_bdev_io_ext_use_bounce_buffer(struct spdk_bdev_io *bdev_io)
{
	/* bdev doesn't support memory domains, thereby buffers in this IO request can't
	 * be accessed directly. It is needed to allocate buffers before issuing IO operation.
	 * For write operation we need to pull buffers from memory domain before submitting IO.
	 * Once read operation completes, we need to use memory_domain push functionality to
	 * update data in original memory domain IO buffer.
	 *
	 * If this I/O request is not aware of metadata, buffers in thsi IO request can't be
	 * accessed directly too. It is needed to allocate buffers before issuing IO operation.
	 * For write operation we need to insert metadata before submitting IO. Once read
	 * operation completes, we need to strip metadata in original IO buffer.
	 *
	 * This IO request will go through a regular IO flow, so clear memory domains pointers */
	assert(bdev_io_use_memory_domain(bdev_io) ||
	       bdev_io_needs_metadata(bdev_io->internal.desc, bdev_io));

	bdev_io->u.bdev.memory_domain = NULL;
	bdev_io->u.bdev.memory_domain_ctx = NULL;
	_bdev_io_get_bounce_buf(bdev_io, _bdev_memory_domain_get_io_cb,
				bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
}

static inline void
_bdev_io_ext_use_accel_buffer(struct spdk_bdev_io *bdev_io)
{
	assert(bdev_io_use_memory_domain(bdev_io));
	assert(bdev_io_needs_metadata(bdev_io->internal.desc, bdev_io));

	bdev_io->u.bdev.memory_domain = NULL;
	bdev_io->u.bdev.memory_domain_ctx = NULL;
	bdev_io_get_accel_buf(bdev_io, _bdev_memory_domain_get_io_cb,
			      bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
}

/* We need to allocate bounce buffer
 * - if bdev doesn't support memory domains,
 * -  if it does support them, but we need to execute an accel sequence and the data buffer is
 *    from accel memory domain (to avoid doing a push/pull from that domain), or
 * - if IO is not aware of metadata.
 */
static inline bool
bdev_io_needs_bounce_buffer(struct spdk_bdev_desc *desc, struct spdk_bdev_io *bdev_io)
{
	if (bdev_io_use_memory_domain(bdev_io)) {
		if (!bdev_io->bdev->memory_domains_supported ||
		    (bdev_io_needs_sequence_exec(bdev_io) &&
		     (bdev_io->internal.memory_domain == spdk_accel_get_memory_domain() ||
		      bdev_io_needs_metadata(desc, bdev_io)))) {
			return true;
		}

		return false;
	}

	if (bdev_io_needs_metadata(desc, bdev_io)) {
		return true;
	}

	return false;
}

/* We need to allocate fake accel buffer if bdev supports memory domains but IO is not
 * aware of metadata.
 */
static inline bool
bdev_io_needs_accel_buffer(struct spdk_bdev_desc *desc, struct spdk_bdev_io *bdev_io)
{
	if (bdev_io_needs_metadata(desc, bdev_io)) {
		assert(bdev_io_use_memory_domain(bdev_io));
		return true;
	}

	return false;
}

static inline void
_bdev_io_submit_ext(struct spdk_bdev_desc *desc, struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	int rc;

	if (spdk_unlikely(ch->flags & BDEV_CH_RESET_IN_PROGRESS)) {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_ABORTED;
		bdev_io_complete_unsubmitted(bdev_io);
		return;
	}

	if (bdev_io_needs_metadata(desc, bdev_io)) {
		rc = bdev_io_init_dif_ctx(bdev_io);
		if (spdk_unlikely(rc != 0)) {
			bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
			bdev_io_complete_unsubmitted(bdev_io);
			return;
		}
	}

	if (bdev_io_needs_bounce_buffer(desc, bdev_io)) {
		_bdev_io_ext_use_bounce_buffer(bdev_io);
		return;
	}

	if (bdev_io_needs_accel_buffer(desc, bdev_io)) {
		_bdev_io_ext_use_accel_buffer(bdev_io);
		return;
	}

	if (bdev_io_needs_sequence_exec(bdev_io)) {
		if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
			bdev_io_exec_sequence(bdev_io, bdev_io_submit_sequence_cb);
			return;
		}
		/* For reads we'll execute the sequence after the data is read, so, for now, only
		 * clear out accel_sequence pointer and submit the IO */
		assert(bdev_io->type == SPDK_BDEV_IO_TYPE_READ);
		bdev_io->u.bdev.accel_sequence = NULL;
	}

	bdev_io_submit(bdev_io);
}

static void
bdev_io_submit_reset(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_bdev_channel *bdev_ch = bdev_io->internal.ch;
	struct spdk_io_channel *ch = bdev_ch->channel;

	assert(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_PENDING);

	bdev_io->internal.f.in_submit_request = true;
	bdev_submit_request(bdev, ch, bdev_io);
	bdev_io->internal.f.in_submit_request = false;
}

/*
 * [한국어]
 * bdev_io_init - bdev_io 슬롯의 기본 필드를 초기화 (submit 직전 단계).
 *
 * @bdev_io: bdev_channel_get_io() 로 막 획득한 슬롯. 내용은 미정의 상태이며 본 함수가 정상화.
 * @bdev   : 타겟 spdk_bdev (NULL 불가).
 * @cb_arg : 사용자 컨텍스트 — 완료 콜백의 첫 인자로 전달됨.
 * @cb     : 비동기 완료 콜백 — (bdev_io, success, cb_arg) 시그니처.
 *
 * 호출자(=공개 API)는 본 함수 호출 후 자신이 발행하는 I/O 종류에 특화된 추가 필드
 * (iov, offset, num_blocks, type, ...)를 더 채우고 bdev_io_submit으로 발행한다.
 * cb가 bdev_io_split_done이면 본 I/O는 split된 child이므로 child_io flag set + split는 false.
 *
 * 실행 컨텍스트: 채널 소유 spdk_thread. 락 없음.
 */
void
bdev_io_init(struct spdk_bdev_io *bdev_io,
	     struct spdk_bdev *bdev, void *cb_arg,
	     spdk_bdev_io_completion_cb cb)
{
	bdev_io->bdev = bdev;
	/* [한국어] 타겟 bdev 보관 — 모듈 dispatch, blocklen 조회 등에 사용. */
	bdev_io->internal.f.raw = 0;
	/* [한국어] union 비트 플래그 전체 0 — 이전 사용 잔재 제거. f.split/f.child_io/f.in_submit_request 등이 모두 union 안에 있음. */
	bdev_io->internal.caller_ctx = cb_arg;
	/* [한국어] 사용자 ctx 보관 — 완료 시 cb 첫 인자로 전달. */
	bdev_io->internal.cb = cb;
	/* [한국어] 완료 콜백 저장. */
	bdev_io->internal.status = SPDK_BDEV_IO_STATUS_PENDING;
	/* [한국어] 상태 초기값 = PENDING. submit 후 모듈 완료까지 이 상태. */
	bdev_io->internal.f.in_submit_request = false;
	/* [한국어] submit_request 호출 중인지 표시 — 재진입 방지용. */
	bdev_io->internal.error.nvme.cdw0 = 0;
	/* [한국어] NVMe 완료 시 cdw0 (zone append ALBA 등) 보관 위치 초기화. */
	bdev_io->num_retries = 0;
	/* [한국어] NOMEM 재시도 횟수 — 모니터링/디버깅용. */
	bdev_io->internal.get_buf_cb = NULL;
	/* [한국어] iobuf 비동기 할당 콜백 — 모듈이 spdk_bdev_io_get_buf 호출 시 채워짐. */
	bdev_io->internal.data_transfer_cpl = NULL;
	/* [한국어] memory_domain pull/push 완료 콜백 — 본 경로 사용 시 채워짐. */
	bdev_io->internal.waitq_entry.dep_unblock = false;
	/* [한국어] bdev_io_wait 큐에 들어갈 때 사용되는 dependency 해제 flag. */
	if (cb == bdev_io_split_done) {
		/* [한국어] cb가 split 전용 콜백이라면 — 본 I/O는 부모로부터 파생된 child. */
		bdev_io->internal.f.child_io = true;
		/* [한국어] child 표시 — bdev_io_submit에서 LBA lock 검사 건너뛰는 단서. */
		bdev_io->internal.f.split = false;
		/* [한국어] child는 추가 split 안 함 (이미 한 번 분할됨). */
	} else {
		/* [한국어] 일반 사용자 I/O 진입 — split 필요 여부 판단. */
		bdev_io->internal.f.child_io = false;
		bdev_io->internal.f.split = bdev_io_should_split(bdev_io);
		/* [한국어] 모듈 max_segments/max_size 초과 시 true → submit에서 bdev_io_split 분기. */
	}
}

/*
 * [한국어]
 * bdev_module_accel_sequence_supported - 특정 I/O 타입에 대해 bdev 모듈이 accel 시퀀스를 지원하는지 확인
 *
 * @bdev:    확인할 bdev 포인터 (fn_table을 통해 모듈 콜백 참조)
 * @io_type: 확인할 I/O 타입 (SPDK_BDEV_IO_TYPE_READ, WRITE 등)
 * @return:  모듈이 accel 시퀀스를 지원하면 true, 아니면 false
 *
 * accel 시퀀스(encrypt/compress/copy 오프로드 체인)를 해당 bdev 모듈이 네이티브로
 * 지원하는지 질의한다. 지원하지 않으면 bdev 레이어가 bounce buffer를 통해 에뮬레이션한다.
 * 앱 스레드 컨텍스트에서만 호출되어야 한다 (assert로 보장).
 * fn_table->accel_sequence_supported 콜백이 없으면 무조건 false 반환.
 *
 * 호출 체인:
 *   bdev_io_should_split() / bdev_io_init() → [이 함수] → bdev->fn_table->accel_sequence_supported()
 */
static bool
bdev_module_accel_sequence_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{

	assert(spdk_thread_is_app_thread(NULL)); /* [한국어] 앱 스레드 전용 함수임을 assert로 보장 */

	if (!bdev->fn_table->accel_sequence_supported) {
		/* [한국어] 모듈이 accel_sequence_supported 콜백을 등록하지 않음 → 미지원 */
		return false;
	}

	return bdev->fn_table->accel_sequence_supported(bdev->ctxt, io_type); /* [한국어] 모듈 콜백으로 지원 여부 위임 */
}

/*
 * [한국어]
 * bdev_module_io_type_supported - bdev 모듈이 특정 I/O 타입을 네이티브 지원하는지 확인
 *
 * @bdev:    확인할 bdev 포인터
 * @io_type: 확인할 I/O 타입
 * @return:  모듈이 해당 타입을 지원하면 true, 아니면 false
 *
 * fn_table->io_type_supported()를 직접 호출하는 얇은 래퍼. bdev_io_type_supported()가
 * 캐시된 io_type_supported 비트마스크를 쓰는 것과 달리, 이 함수는 실시간으로
 * 모듈 콜백을 호출한다. 앱 스레드 컨텍스트에서만 사용.
 *
 * 호출 체인:
 *   spdk_bdev_register() 초기화 경로 → [이 함수] → bdev->fn_table->io_type_supported()
 */
static bool
bdev_module_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	assert(spdk_thread_is_app_thread(NULL)); /* [한국어] 앱 스레드 전용 확인 */

	return bdev->fn_table->io_type_supported(bdev->ctxt, io_type); /* [한국어] 모듈 콜백으로 지원 여부 반환 */
}

/*
 * [한국어]
 * bdev_io_type_supported - bdev가 특정 I/O 타입을 지원하는지 비트마스크로 빠르게 확인
 *
 * @bdev:    확인할 bdev 포인터
 * @io_type: 확인할 I/O 타입 (SPDK_BDEV_IO_TYPE_READ 등)
 * @return:  지원하면 true, 범위 초과하거나 미지원이면 false
 *
 * bdev->io_type_supported 비트마스크를 통해 모듈 콜백 없이 O(1)로 확인한다.
 * 이 비트마스크는 spdk_bdev_register() 시 모든 I/O 타입을 순회하며 설정된다.
 * spdk_bdev_io_type_supported()의 내부 구현으로 사용된다.
 *
 * 호출 체인:
 *   spdk_bdev_io_type_supported() → [이 함수] → bdev->io_type_supported 비트마스크 참조
 */
static bool
bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	SPDK_STATIC_ASSERT(SPDK_BDEV_NUM_IO_TYPES <= 32, "io_type exceeds 32 bits, adjust bitmask type");
	/* [한국어] 컴파일 타임 정적 단언: I/O 타입 수가 32비트를 초과하면 비트마스크 타입 변경 필요 */

	if (spdk_unlikely(io_type <= SPDK_BDEV_IO_TYPE_INVALID || io_type >= SPDK_BDEV_NUM_IO_TYPES)) {
		/* [한국어] 유효 범위(INVALID 초과 ~ NUM_IO_TYPES 미만) 벗어난 io_type → 즉시 false */
		return false;
	}

	return bdev->io_type_supported & (1u << (uint32_t)io_type);
	/* [한국어] io_type에 해당하는 비트가 설정되어 있으면 true — 모듈이 등록 시 이 비트를 세팅 */
}

/*
 * [한국어]
 * spdk_bdev_io_type_supported - bdev가 특정 I/O 타입을 지원하는지 확인 (에뮬레이션 포함)
 *
 * @bdev:    확인할 bdev 포인터
 * @io_type: 확인할 I/O 타입
 * @return:  네이티브 지원 또는 에뮬레이션 가능하면 true, 완전 미지원이면 false
 *
 * 내부 bdev_io_type_supported() 비트마스크 확인 + 에뮬레이션 가능 여부를 종합한다.
 * 현재 WRITE_ZEROES는 WRITE가 지원되면 bdev 레이어에서 0으로 채운 WRITE로 에뮬레이션되므로
 * 모듈이 WRITE_ZEROES를 선언하지 않아도 true를 반환한다.
 * 공개 API로 외부(RPC, 관리 플레인)에서 호출된다.
 *
 * 호출 체인:
 *   RPC/관리 레이어 → [이 함수] → bdev_io_type_supported() → bdev->io_type_supported 비트마스크
 */
bool
spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	bool supported;

	supported = bdev_io_type_supported(bdev, io_type); /* [한국어] 비트마스크에서 네이티브 지원 여부 확인 */

	if (!supported) {
		/* [한국어] 모듈이 직접 지원하지 않더라도 bdev 레이어가 에뮬레이션할 수 있는 타입 처리 */
		switch (io_type) {
		case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
			/* The bdev layer will emulate write zeroes as long as write is supported. */
			/* [한국어] WRITE_ZEROES: WRITE만 지원되면 0-fill 버퍼로 에뮬레이션 가능 → true */
			supported = bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE);
			break;
		default:
			break; /* [한국어] 다른 타입은 에뮬레이션 없이 false 유지 */
		}
	}

	return supported; /* [한국어] 네이티브 또는 에뮬레이션 가능 여부 반환 */
}

/* [한국어] I/O 타입 → 문자열 변환 테이블.
 * spdk_bdev_get_io_type_name()과 spdk_bdev_get_io_type()이 사용하는 정적 배열.
 * 지정 초기화(designated initializer) 방식으로 enum 값을 인덱스로 직접 매핑.
 * 새 I/O 타입 추가 시 여기에도 반드시 추가해야 한다. */
static const char *g_io_type_strings[] = {
	[SPDK_BDEV_IO_TYPE_READ] = "read",               /* [한국어] 읽기 */
	[SPDK_BDEV_IO_TYPE_WRITE] = "write",              /* [한국어] 쓰기 */
	[SPDK_BDEV_IO_TYPE_UNMAP] = "unmap",              /* [한국어] 블록 매핑 해제 (TRIM/DISCARD) */
	[SPDK_BDEV_IO_TYPE_FLUSH] = "flush",              /* [한국어] 캐시 플러시 */
	[SPDK_BDEV_IO_TYPE_RESET] = "reset",              /* [한국어] 디바이스 리셋 */
	[SPDK_BDEV_IO_TYPE_NVME_ADMIN] = "nvme_admin",    /* [한국어] NVMe Admin Command */
	[SPDK_BDEV_IO_TYPE_NVME_IO] = "nvme_io",          /* [한국어] NVMe I/O Command */
	[SPDK_BDEV_IO_TYPE_NVME_IO_MD] = "nvme_io_md",   /* [한국어] 메타데이터 포함 NVMe I/O */
	[SPDK_BDEV_IO_TYPE_WRITE_ZEROES] = "write_zeroes", /* [한국어] 0으로 채워 쓰기 */
	[SPDK_BDEV_IO_TYPE_ZCOPY] = "zcopy",              /* [한국어] Zero-copy I/O */
	[SPDK_BDEV_IO_TYPE_GET_ZONE_INFO] = "get_zone_info",     /* [한국어] ZNS 존 정보 조회 */
	[SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT] = "zone_management", /* [한국어] ZNS 존 관리 */
	[SPDK_BDEV_IO_TYPE_ZONE_APPEND] = "zone_append",         /* [한국어] ZNS 존 어펜드 */
	[SPDK_BDEV_IO_TYPE_COMPARE] = "compare",          /* [한국어] 블록 데이터 비교 */
	[SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE] = "compare_and_write", /* [한국어] 비교 후 쓰기 (원자적 RMW) */
	[SPDK_BDEV_IO_TYPE_ABORT] = "abort",              /* [한국어] 진행 중인 I/O 중단 */
	[SPDK_BDEV_IO_TYPE_SEEK_HOLE] = "seek_hole",      /* [한국어] 파일 hole 탐색 */
	[SPDK_BDEV_IO_TYPE_SEEK_DATA] = "seek_data",      /* [한국어] 파일 데이터 영역 탐색 */
	[SPDK_BDEV_IO_TYPE_COPY] = "copy",                /* [한국어] 내부 블록 복사 (offload) */
	[SPDK_BDEV_IO_TYPE_NVME_IOV_MD] = "nvme_iov_md", /* [한국어] scatter-gather + 메타데이터 NVMe I/O */
	[SPDK_BDEV_IO_TYPE_NVME_NSSR] = "nvme_nssr",     /* [한국어] NVMe Namespace-Specific Reconfig */
	[SPDK_BDEV_IO_TYPE_WRITE_UNCORRECTABLE] = "write_uncorrectable", /* [한국어] 비정정 오류로 쓰기 */
};

/*
 * [한국어]
 * spdk_bdev_get_io_type_name - I/O 타입 enum 값을 문자열로 변환
 *
 * @io_type: 변환할 I/O 타입 enum 값
 * @return:  해당 타입의 문자열 ("read", "write" 등), 범위 초과 시 NULL
 *
 * 로깅, JSON RPC 응답, 통계 출력 등에서 I/O 타입을 사람이 읽을 수 있는 형태로
 * 변환할 때 사용된다. g_io_type_strings 배열을 직접 인덱싱하므로 O(1) 조회.
 *
 * 호출 체인:
 *   RPC 핸들러 / 로깅 코드 → [이 함수] → g_io_type_strings[] 배열 참조
 */
const char *
spdk_bdev_get_io_type_name(enum spdk_bdev_io_type io_type)
{
	if (io_type <= SPDK_BDEV_IO_TYPE_INVALID || io_type >= SPDK_BDEV_NUM_IO_TYPES) {
		/* [한국어] 유효 범위(INVALID 초과 ~ NUM_IO_TYPES 미만) 벗어난 값 → NULL 반환 */
		return NULL;
	}

	return g_io_type_strings[io_type]; /* [한국어] enum 값을 배열 인덱스로 사용해 O(1) 조회 */
}

/*
 * [한국어]
 * spdk_bdev_get_io_type - I/O 타입 문자열을 enum 값으로 변환 (역방향 조회)
 *
 * @io_type_string: 조회할 I/O 타입 문자열 ("read", "write", "unmap" 등)
 * @return:         해당하는 spdk_bdev_io_type enum 값, 미발견 시 -1
 *
 * spdk_bdev_get_io_type_name()의 역방향 함수. JSON RPC 요청에서 문자열로 전달된
 * I/O 타입을 내부 enum 값으로 변환할 때 사용한다.
 * g_io_type_strings 배열을 선형 탐색하므로 O(N) — 관리 경로에서만 사용.
 *
 * 호출 체인:
 *   RPC 파싱 코드 → [이 함수] → strcmp()로 g_io_type_strings[] 선형 탐색
 */
int
spdk_bdev_get_io_type(const char *io_type_string)
{
	int i;

	for (i = SPDK_BDEV_IO_TYPE_READ; i < SPDK_BDEV_NUM_IO_TYPES; ++i) {
		/* [한국어] 각 I/O 타입의 문자열 표현과 비교 */
		if (!strcmp(io_type_string, g_io_type_strings[i])) {
			return i; /* [한국어] 매칭된 I/O 타입 enum 값 반환 */
		}
	}

	return -1; /* [한국어] 일치하는 문자열 없음 → 오류 코드 반환 */
}

/*
 * [한국어]
 * spdk_bdev_io_get_submit_tsc - bdev_io가 제출된 시각의 TSC(Time Stamp Counter) 값 반환
 *
 * @bdev_io: TSC를 조회할 bdev_io 포인터
 * @return:  제출 당시의 spdk_get_ticks() 값 (CPU 사이클 단위)
 *
 * I/O 지연 시간(latency) 측정을 위해 사용된다. 완료 콜백에서 현재 TSC와의 차이를
 * 계산해 히스토그램이나 통계에 기록할 수 있다.
 * internal.submit_tsc는 bdev_io_submit() 시점에 설정된다.
 *
 * 호출 체인:
 *   타임아웃 체크 / 통계 수집 코드 → [이 함수] → bdev_io->internal.submit_tsc 반환
 */
uint64_t
spdk_bdev_io_get_submit_tsc(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->internal.submit_tsc; /* [한국어] 제출 시각 TSC — 레이턴시 측정의 기준점 */
}

/*
 * [한국어]
 * spdk_bdev_io_hide_metadata - 이 I/O에서 메타데이터를 숨겨야 하는지 여부 반환
 *
 * @bdev_io: 확인할 bdev_io 포인터
 * @return:  메타데이터 숨김 옵션이 설정되어 있으면 true
 *
 * 디스크립터 옵션(hide_metadata)을 통해 사용자가 메타데이터(e.g. NVMe DIX/DIF PI)를
 * 직접 보지 않도록 설정한 경우 true를 반환한다. bdev 레이어가 메타데이터 처리를
 * 내부적으로 수행하고 상위에는 노출하지 않을 때 사용.
 *
 * 호출 체인:
 *   상위 I/O 처리 코드 → [이 함수] → desc->opts.hide_metadata 필드 참조
 */
bool
spdk_bdev_io_hide_metadata(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->internal.desc->opts.hide_metadata; /* [한국어] 디스크립터 옵션에서 메타데이터 숨김 여부 반환 */
}

/*
 * [한국어]
 * spdk_bdev_dump_info_json - bdev 모듈의 진단 정보를 JSON으로 출력
 *
 * @bdev: 정보를 출력할 bdev 포인터
 * @w:    JSON 직렬화 컨텍스트
 * @return: 성공 시 0, 실패 시 음수 오류 코드
 *
 * RPC "bdev_get_bdevs" 등 진단 명령에서 각 bdev의 모듈 고유 정보를 JSON으로 직렬화할 때 사용.
 * fn_table->dump_info_json 콜백이 없으면 추가 정보 없이 0 반환.
 *
 * 호출 체인:
 *   bdev_write_config_json() / RPC 핸들러 → [이 함수] → bdev->fn_table->dump_info_json()
 */
int
spdk_bdev_dump_info_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	if (bdev->fn_table->dump_info_json) {
		/* [한국어] 모듈이 dump_info_json 콜백을 등록한 경우 → 모듈별 고유 정보 직렬화 */
		return bdev->fn_table->dump_info_json(bdev->ctxt, w);
	}

	return 0; /* [한국어] 콜백 없는 모듈은 추가 정보 없이 성공 반환 */
}

static void
bdev_qos_update_max_quota_per_timeslice(struct spdk_bdev_qos *qos)
{
	uint32_t max_per_timeslice = 0;
	int i;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (qos->rate_limits[i].limit == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
			qos->rate_limits[i].max_per_timeslice = 0;
			continue;
		}

		max_per_timeslice = qos->rate_limits[i].limit *
				    SPDK_BDEV_QOS_TIMESLICE_IN_USEC / SPDK_SEC_TO_USEC;

		qos->rate_limits[i].max_per_timeslice = spdk_max(max_per_timeslice,
							qos->rate_limits[i].min_per_timeslice);

		__atomic_store_n(&qos->rate_limits[i].remaining_this_timeslice,
				 qos->rate_limits[i].max_per_timeslice, __ATOMIC_RELEASE);
	}

	bdev_qos_set_ops(qos);
}

/*
 * [한국어]
 * bdev_channel_submit_qos_io - QoS 폴링 시 채널 단위로 큐된 I/O를 제출하는 콜백
 *
 * @i:     spdk_bdev_for_each_channel() 반복자 (각 채널 처리 후 continue 필요)
 * @bdev:  대상 bdev 포인터
 * @io_ch: 현재 순회 중인 I/O 채널 (모듈 채널)
 * @ctx:   사용되지 않음 (qos 포인터가 전달되지만 bdev->internal.qos로도 접근 가능)
 * @return: 없음 (spdk_bdev_for_each_channel_continue로 다음 채널 신호)
 *
 * bdev_channel_poll_qos() 타임슬라이스 폴러에서 spdk_bdev_for_each_channel()로 호출되어
 * 각 채널에 큐된 qos_queued_io를 토큰 버킷 한도 내에서 실제 모듈에 제출한다.
 * qos_queued_io가 비어있으면 status=0(계속), 아직 남아있으면 status=1(중단)을 전달.
 * TODO: 현재는 순서대로 처리하지만 채널 간 round-robin이 필요하다.
 *
 * 호출 체인:
 *   bdev_channel_poll_qos() → spdk_bdev_for_each_channel() → [이 함수] → bdev_qos_io_submit()
 */
static void
bdev_channel_submit_qos_io(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			   struct spdk_io_channel *io_ch, void *ctx)
{
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(io_ch); /* [한국어] io_ch에서 bdev 채널 포인터 추출 */
	int status;

	bdev_qos_io_submit(bdev_ch, bdev->internal.qos); /* [한국어] 토큰 버킷 한도 내에서 큐된 I/O 제출 */

	/* if all IOs were sent then continue the iteration, otherwise - stop it */
	/* TODO: channels round robing */
	/* [한국어] qos_queued_io가 빈 경우(0) → 다음 채널 계속; 남아 있는 경우(1) → 반복 중단 */
	status = TAILQ_EMPTY(&bdev_ch->qos_queued_io) ? 0 : 1;

	spdk_bdev_for_each_channel_continue(i, status); /* [한국어] 다음 채널로 계속할지 여부 신호 전달 */
}


/*
 * [한국어]
 * bdev_channel_submit_qos_io_done - QoS I/O 제출 채널 순회 완료 콜백 (현재는 빈 stub)
 *
 * @bdev:   대상 bdev 포인터
 * @ctx:    사용되지 않음
 * @status: 순회 결과 상태 (0 = 정상, 1 = 중단)
 *
 * spdk_bdev_for_each_channel()의 done 콜백으로, 모든 채널을 순회한 후 호출된다.
 * 현재 구현은 빈 stub이지만, 향후 채널 간 round-robin 스케줄링이나 완료 통계 수집을
 * 위해 사용될 예정이다.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() 완료 후 → [이 함수] (현재 아무 동작 없음)
 */
static void
bdev_channel_submit_qos_io_done(struct spdk_bdev *bdev, void *ctx, int status)
{
	/* [한국어] 현재는 아무 작업도 하지 않는 완료 콜백 stub — 향후 확장 예정 */
}

/*
 * [한국어]
 * bdev_channel_poll_qos - QoS 토큰 버킷 타임슬라이스 폴러 (1ms마다 호출)
 *
 * @arg: bdev 포인터 (SPDK_POLLER_REGISTER 시 등록)
 * @return: SPDK_POLLER_BUSY (새 토큰 보충 및 I/O 제출) 또는 SPDK_POLLER_IDLE (조기 호출)
 *
 * QoS의 핵심 rate-limiting 함수. 1ms 타임슬라이스(SPDK_BDEV_QOS_TIMESLICE_IN_USEC)마다
 * 각 rate limit 타입의 remaining_this_timeslice 토큰을 max_per_timeslice만큼 보충하고,
 * 큐된 I/O를 bdev_channel_submit_qos_io로 각 채널에 분배 제출한다.
 *
 * 토큰 부족 처리:
 *   - 이전 타임슬라이스에서 초과 소비(remaining < 0)가 발생한 경우 → 초과분을 이월해
 *     다음 타임슬라이스를 감소시킨다 (토큰 버킷의 부채 처리).
 *   - __atomic_exchange_n / __atomic_add_fetch: bdev_qos_rw_queue_io()와의 경쟁 조건이
 *     있으나 두 연산이 각각 분리되어 있어 약간의 퍼지가 발생할 수 있음 (무해).
 *
 * 실행 컨텍스트: QoS 채널을 선택한 리액터 스레드의 폴러.
 *
 * 호출 체인:
 *   SPDK 리액터 폴러 루프 → [이 함수] → spdk_bdev_for_each_channel() → bdev_channel_submit_qos_io()
 */
static int
bdev_channel_poll_qos(void *arg)
{
	struct spdk_bdev *bdev = arg; /* [한국어] 폴러 등록 시 전달된 bdev 포인터 */
	struct spdk_bdev_qos *qos = bdev->internal.qos; /* [한국어] 이 bdev의 QoS 제어 구조체 */
	uint64_t now = spdk_get_ticks(); /* [한국어] 현재 시각 TSC 값 */
	int i;
	int64_t remaining_last_timeslice; /* [한국어] 이전 타임슬라이스 잔여 토큰 (음수이면 초과 소비) */

	if (spdk_unlikely(qos->thread == NULL)) {
		/* Old QoS was unbound to remove and new QoS is not enabled yet. */
		/* [한국어] 구 QoS 해제 중 → 아직 새 QoS가 활성화되지 않은 과도기 상태; 무작동 */
		return SPDK_POLLER_IDLE;
	}

	if (now < (qos->last_timeslice + qos->timeslice_size)) {
		/* We received our callback earlier than expected - return
		 *  immediately and wait to do accounting until at least one
		 *  timeslice has actually expired.  This should never happen
		 *  with a well-behaved timer implementation.
		 */
		/* [한국어] 아직 1 타임슬라이스가 완전히 경과하지 않음 → 조기 wake-up, IDLE 반환 */
		return SPDK_POLLER_IDLE;
	}

	/* Reset for next round of rate limiting */
	/* [한국어] 다음 타임슬라이스를 위한 토큰 리셋 및 이전 초과분 처리 */
	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		/* We may have allowed the IOs or bytes to slightly overrun in the last
		 * timeslice. remaining_this_timeslice is signed, so if it's negative
		 * here, we'll account for the overrun so that the next timeslice will
		 * be appropriately reduced.
		 */
		/* [한국어] __atomic_exchange_n: remaining_this_timeslice를 0으로 리셋하고 이전 값을 읽음.
		 * RELAXED: 여기서는 다른 원자 변수와의 순서 보장이 필요 없으므로 relaxed 사용 */
		remaining_last_timeslice = __atomic_exchange_n(&qos->rate_limits[i].remaining_this_timeslice,
					   0, __ATOMIC_RELAXED);
		if (remaining_last_timeslice < 0) {
			/* There could be a race condition here as both bdev_qos_rw_queue_io() and bdev_channel_poll_qos()
			 * potentially use 2 atomic ops each, so they can intertwine.
			 * This race can potentially cause the limits to be a little fuzzy but won't cause any real damage.
			 */
			/* [한국어] 이전 타임슬라이스 초과 소비 → 음수 값을 다시 저장해 다음 슬라이스 토큰에서 차감 */
			__atomic_store_n(&qos->rate_limits[i].remaining_this_timeslice,
					 remaining_last_timeslice, __ATOMIC_RELAXED);
		}
	}

	/* [한국어] 경과한 타임슬라이스 수만큼 토큰 보충 (복수 슬라이스 누락 시 한 번에 처리) */
	while (now >= (qos->last_timeslice + qos->timeslice_size)) {
		qos->last_timeslice += qos->timeslice_size; /* [한국어] 마지막 타임슬라이스 시각 한 슬라이스 전진 */
		for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
			/* [한국어] 각 rate limit 타입에 max_per_timeslice 토큰 추가 */
			__atomic_add_fetch(&qos->rate_limits[i].remaining_this_timeslice,
					   qos->rate_limits[i].max_per_timeslice, __ATOMIC_RELAXED);
		}
	}

	/* [한국어] 모든 채널에 걸쳐 QoS 큐된 I/O 제출 — 각 채널에서 bdev_channel_submit_qos_io 호출 */
	spdk_bdev_for_each_channel(bdev, bdev_channel_submit_qos_io, qos,
				   bdev_channel_submit_qos_io_done);

	return SPDK_POLLER_BUSY; /* [한국어] 토큰 보충 및 I/O 제출 수행 → BUSY 반환 */
}

/*
 * [한국어]
 * bdev_channel_destroy_resource - bdev 채널의 모든 리소스를 해제하는 내부 정리 함수
 *
 * @ch: 해제할 bdev 채널 포인터
 *
 * bdev_channel_destroy()가 호출하는 내부 정리 함수. I/O 통계 구조체, locked_ranges,
 * 모듈 I/O 채널(ch->channel), accel 채널(ch->accel_channel)을 순서대로 해제한다.
 * shared_resource의 ref를 감소시키고, 마지막 참조자이면 mgmt_ch와 nomem_poller도 해제한다.
 *
 * 사전 조건: io_locked/io_submitted/io_accel_exec/io_memory_domain이 모두 비어있어야 하고
 * io_outstanding이 0이어야 한다 (assert로 확인). 채널 파괴 전에 abort가 완료되어야 함.
 *
 * 실행 컨텍스트: 채널이 속한 리액터 스레드.
 *
 * 호출 체인:
 *   bdev_channel_destroy() → [이 함수] → spdk_put_io_channel() / free()
 */
static void
bdev_channel_destroy_resource(struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_shared_resource *shared_resource;
	struct lba_range *range;

	bdev_free_io_stat(ch->stat); /* [한국어] 채널별 I/O 통계 구조체 해제 */
#ifdef SPDK_CONFIG_VTUNE
	bdev_free_io_stat(ch->prev_stat); /* [한국어] VTune용 이전 통계 스냅샷도 해제 */
#endif

	/* [한국어] locked_ranges 목록 순회하며 모든 LBA 범위 락 해제 및 메모리 반환 */
	while (!TAILQ_EMPTY(&ch->locked_ranges)) {
		range = TAILQ_FIRST(&ch->locked_ranges); /* [한국어] 첫 번째 LBA 범위 항목 꺼냄 */
		TAILQ_REMOVE(&ch->locked_ranges, range, tailq); /* [한국어] 목록에서 제거 */
		free(range); /* [한국어] LBA 범위 구조체 메모리 해제 */
	}

	spdk_put_io_channel(ch->channel); /* [한국어] 모듈 I/O 채널 참조 해제 */
	spdk_put_io_channel(ch->accel_channel); /* [한국어] DPDK accel 채널 참조 해제 */

	shared_resource = ch->shared_resource; /* [한국어] 이 채널이 속한 공유 리소스 */

	/* [한국어] 채널 파괴 전 모든 큐가 비어있어야 함을 보장하는 assert 그룹 */
	assert(TAILQ_EMPTY(&ch->io_locked));         /* [한국어] LBA 락 대기 I/O가 없어야 함 */
	assert(TAILQ_EMPTY(&ch->io_submitted));      /* [한국어] 제출된 I/O가 없어야 함 */
	assert(TAILQ_EMPTY(&ch->io_accel_exec));     /* [한국어] accel 시퀀스 처리 중인 I/O가 없어야 함 */
	assert(TAILQ_EMPTY(&ch->io_memory_domain));  /* [한국어] 메모리 도메인 처리 중인 I/O가 없어야 함 */
	assert(ch->io_outstanding == 0);             /* [한국어] 미완료 I/O 카운터가 0이어야 함 */
	assert(shared_resource->ref > 0);            /* [한국어] 해제 전 ref > 0이어야 함 */
	shared_resource->ref--; /* [한국어] 공유 리소스 참조 카운트 감소 */
	if (shared_resource->ref == 0) {
		/* [한국어] 마지막 참조자 → shared_resource 자체도 완전 해제 */
		assert(shared_resource->io_outstanding == 0); /* [한국어] 공유 미완료 I/O도 0이어야 함 */
		TAILQ_REMOVE(&shared_resource->mgmt_ch->shared_resources, shared_resource, link);
		/* [한국어] mgmt_ch의 shared_resources 목록에서 제거 */
		spdk_put_io_channel(spdk_io_channel_from_ctx(shared_resource->mgmt_ch));
		/* [한국어] mgmt_ch에 대한 bdev 레이어의 참조 해제 */
		spdk_poller_unregister(&shared_resource->nomem_poller); /* [한국어] NOMEM 재시도 폴러 해제 */
		free(shared_resource); /* [한국어] 공유 리소스 구조체 메모리 반환 */
	}
}

/*
 * [한국어]
 * bdev_enable_qos - 새 채널에 QoS를 활성화하고 최초 채널 선택 시 QoS 폴러를 등록
 *
 * @bdev: QoS를 활성화할 bdev 포인터
 * @ch:   새로 생성된 bdev 채널 포인터
 *
 * bdev_channel_create()에서 spinlock 보호 하에 호출된다. bdev에 QoS(qos != NULL)가
 * 설정되어 있으면 채널 플래그에 BDEV_CH_QOS_ENABLED를 설정해 이 채널에서 발행되는
 * 모든 I/O가 QoS 큐를 거치도록 한다.
 *
 * QoS 채널 최초 선택 (qos->ch == NULL):
 *   1. spdk_get_io_channel()로 이 bdev의 I/O 채널에 추가 참조를 획득해 ch를 QoS 채널로 지정.
 *   2. 각 rate limit 타입의 min_per_timeslice를 IOPS/BPS 기준으로 설정.
 *   3. limit == 0인 항목은 SPDK_BDEV_QOS_LIMIT_NOT_DEFINED로 표시.
 *   4. bdev_qos_update_max_quota_per_timeslice()로 슬라이스당 최대 토큰 계산.
 *   5. 타임슬라이스 크기와 기준 시각을 설정하고 bdev_channel_poll_qos 폴러 등록.
 *
 * 실행 컨텍스트: 채널 생성 시 bdev->internal.spinlock 보유 상태.
 *
 * 호출 체인:
 *   bdev_channel_create() → [이 함수] → SPDK_POLLER_REGISTER(bdev_channel_poll_qos)
 */
static void
bdev_enable_qos(struct spdk_bdev *bdev, struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_qos	*qos = bdev->internal.qos; /* [한국어] 이 bdev의 QoS 제어 구조체 */
	int			i;

	assert(spdk_spin_held(&bdev->internal.spinlock)); /* [한국어] spinlock 보유 상태 보장 */

	/* Rate limiting on this bdev enabled */
	if (qos) {
		/* [한국어] QoS가 설정된 경우 → 채널에 QoS 활성화 */
		if (qos->ch == NULL) {
			/* [한국어] 아직 QoS 채널이 없음 → 이 채널을 QoS 채널로 선택 (최초 1회) */
			struct spdk_io_channel *io_ch;

			SPDK_DEBUGLOG(bdev, "Selecting channel %p as QoS channel for bdev %s on thread %p\n", ch,
				      bdev->name, spdk_get_thread());

			/* No qos channel has been selected, so set one up */

			/* Take another reference to ch */
			/* [한국어] ch를 QoS 채널로 유지하기 위해 추가 참조 획득 */
			io_ch = spdk_get_io_channel(__bdev_to_io_dev(bdev));
			assert(io_ch != NULL); /* [한국어] io_ch 획득 실패 시 프로그래밍 오류 */
			qos->ch = ch; /* [한국어] QoS 채널로 ch 지정 */

			qos->thread = spdk_io_channel_get_thread(io_ch); /* [한국어] QoS 폴러가 실행될 스레드 저장 */

			for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
				if (bdev_qos_is_iops_rate_limit(i) == true) {
					/* [한국어] IOPS 기반 rate limit: 슬라이스당 최소 I/O 수 설정 */
					qos->rate_limits[i].min_per_timeslice =
						SPDK_BDEV_QOS_MIN_IO_PER_TIMESLICE;
				} else {
					/* [한국어] BPS 기반 rate limit: 슬라이스당 최소 바이트 수 설정 */
					qos->rate_limits[i].min_per_timeslice =
						SPDK_BDEV_QOS_MIN_BYTE_PER_TIMESLICE;
				}

				if (qos->rate_limits[i].limit == 0) {
					/* [한국어] limit == 0은 "미정의"를 의미 → NOT_DEFINED로 명시 */
					qos->rate_limits[i].limit = SPDK_BDEV_QOS_LIMIT_NOT_DEFINED;
				}
			}
			bdev_qos_update_max_quota_per_timeslice(qos); /* [한국어] 슬라이스당 최대 토큰 계산 */
			qos->timeslice_size =
				SPDK_BDEV_QOS_TIMESLICE_IN_USEC * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
			/* [한국어] 타임슬라이스 크기를 TSC 단위로 변환 (1000us * ticks/sec / 1e6) */
			qos->last_timeslice = spdk_get_ticks(); /* [한국어] 기준 시각 초기화 */
			qos->poller = SPDK_POLLER_REGISTER(bdev_channel_poll_qos,
							   bdev,
							   SPDK_BDEV_QOS_TIMESLICE_IN_USEC);
			/* [한국어] bdev_channel_poll_qos 폴러를 1000us 주기로 등록 */
		}

		ch->flags |= BDEV_CH_QOS_ENABLED; /* [한국어] 채널 플래그에 QoS 활성 비트 설정 */
	}
}

/*
 * [한국어] poll_timeout_ctx - I/O 타임아웃 폴링 채널 순회 컨텍스트
 *
 * bdev_poll_timeout_io() 폴러가 각 채널에서 타임아웃 I/O를 검사할 때 사용하는
 * 임시 컨텍스트 구조체. spdk_bdev_for_each_channel()에 인자로 전달된다.
 * 채널 순회 완료 후 bdev_channel_poll_timeout_io_done()에서 해제된다.
 */
struct poll_timeout_ctx {
	struct spdk_bdev_desc	*desc;
	/* [한국어] 타임아웃을 감시하는 bdev 디스크립터.
	 * 설정자: bdev_poll_timeout_io()에서 desc를 복사.
	 * 읽는 자: bdev_channel_poll_timeout_io()에서 desc->closed 확인 및 I/O 매칭.
	 * 값 범위: 유효한 spdk_bdev_desc 포인터 (채널 순회 완료까지 ref 보호).
	 * 동기화: desc->spinlock으로 closed/refs 접근 보호. */

	uint64_t		timeout_in_sec;
	/* [한국어] 타임아웃 임계값 (초 단위).
	 * 설정자: bdev_poll_timeout_io()에서 desc->timeout_in_sec 복사.
	 * 읽는 자: bdev_channel_poll_timeout_io()에서 (now - submit_tsc) > timeout 비교.
	 * 값 범위: spdk_bdev_set_timeout()으로 설정된 양수 값.
	 * 동기화: 읽기 전용 필드; 경쟁 없음. */

	spdk_bdev_io_timeout_cb	cb_fn;
	/* [한국어] 타임아웃 발생 시 호출할 사용자 콜백 함수 포인터.
	 * 설정자: bdev_poll_timeout_io()에서 desc->cb_fn 복사.
	 * 읽는 자: bdev_channel_poll_timeout_io()에서 타임아웃 I/O 발견 시 호출.
	 * 값 범위: NULL이 아닌 유효한 함수 포인터.
	 * 동기화: 읽기 전용 필드. */

	void			*cb_arg;
	/* [한국어] 타임아웃 콜백에 전달할 사용자 지정 인자.
	 * 설정자: bdev_poll_timeout_io()에서 desc->cb_arg 복사.
	 * 읽는 자: bdev_channel_poll_timeout_io()에서 cb_fn 호출 시 인자로 전달.
	 * 값 범위: 사용자 임의 포인터; NULL 가능.
	 * 동기화: 읽기 전용 필드. */
};

/*
 * [한국어]
 * bdev_desc_free - bdev 디스크립터 구조체와 관련 리소스를 완전 해제
 *
 * @desc: 해제할 bdev 디스크립터 포인터
 *
 * 디스크립터 참조 카운트가 0이 되고 closed == true인 경우 최종 해제를 담당한다.
 * spinlock을 먼저 파괴하고, media_events_buffer(미디어 오류 이벤트 버퍼)와
 * 디스크립터 자체를 순서대로 해제한다.
 *
 * 호출 체인:
 *   bdev_channel_poll_timeout_io_done() / bdev_close() → [이 함수] → free()
 */
static void
bdev_desc_free(struct spdk_bdev_desc *desc)
{
	spdk_spin_destroy(&desc->spinlock); /* [한국어] 디스크립터의 spinlock 파괴 */
	free(desc->media_events_buffer);   /* [한국어] 미디어 오류 이벤트 버퍼 해제 */
	free(desc);                        /* [한국어] 디스크립터 구조체 자체 해제 */
}

/*
 * [한국어]
 * bdev_channel_poll_timeout_io_done - 타임아웃 I/O 채널 순회 완료 콜백
 *
 * @bdev:    대상 bdev 포인터
 * @_ctx:    poll_timeout_ctx 포인터 (채널 순회 시 전달된 컨텍스트)
 * @status:  순회 상태 (0 = 정상 완료, -1 = desc 닫힘으로 조기 종료)
 *
 * spdk_bdev_for_each_channel()이 모든 채널 순회를 마친 후 호출된다.
 * ctx를 해제하고, bdev_poll_timeout_io()에서 획득한 desc refs를 감소시킨다.
 * 만약 desc가 이미 닫혔고(closed == true) refs가 0이 되면 bdev_desc_free()로
 * 디스크립터를 완전히 해제한다 (deferred close 완료).
 *
 * 실행 컨텍스트: QoS/타임아웃 폴러가 실행되는 리액터 스레드.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() 완료 → [이 함수] → bdev_desc_free() (조건부)
 */
static void
bdev_channel_poll_timeout_io_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct poll_timeout_ctx *ctx  = _ctx; /* [한국어] 채널 순회 시 사용한 컨텍스트 */
	struct spdk_bdev_desc *desc = ctx->desc; /* [한국어] 감시 대상 bdev 디스크립터 */

	free(ctx); /* [한국어] 컨텍스트 구조체 해제 — desc는 아직 사용하므로 먼저 복사 후 free */

	spdk_spin_lock(&desc->spinlock); /* [한국어] desc 상태 확인 위해 spinlock 획득 */
	desc->refs--; /* [한국어] bdev_poll_timeout_io()에서 획득한 refs 반환 */
	if (desc->closed == true && desc->refs == 0) {
		/* [한국어] desc가 이미 닫혔고 마지막 참조가 해제됨 → 완전 해제 */
		spdk_spin_unlock(&desc->spinlock);
		bdev_desc_free(desc); /* [한국어] spinlock, media_events_buffer, desc 자체 순서로 해제 */
		return;
	}
	spdk_spin_unlock(&desc->spinlock); /* [한국어] 정상 경로: 락만 해제하고 반환 */
}

/*
 * [한국어]
 * bdev_channel_poll_timeout_io - 단일 채널의 I/O 타임아웃을 검사하는 채널 순회 콜백
 *
 * @i:     spdk_bdev_for_each_channel() 반복자
 * @bdev:  대상 bdev 포인터
 * @io_ch: 현재 순회 중인 I/O 채널
 * @_ctx:  poll_timeout_ctx 포인터 (타임아웃 임계값, 콜백 등 포함)
 *
 * 각 채널의 io_submitted TAILQ를 순회하며 타임아웃(submit_tsc + timeout_in_sec 경과)된
 * I/O를 찾아 사용자 콜백을 호출한다. io_submitted는 submit_tsc 오름차순으로 정렬되어 있으므로
 * 타임아웃이 아닌 I/O를 만나면 나머지도 타임아웃이 아님을 알 수 있어 루프를 조기 종료한다.
 *
 * split 생성 I/O(cb == bdev_io_split_done)는 제외 — 원본 I/O만 타임아웃 검사 대상.
 * desc가 이미 닫힌 경우 -1을 반환해 나머지 채널 순회를 취소한다.
 *
 * 실행 컨텍스트: desc의 타임아웃 폴러가 실행되는 리액터 스레드.
 *
 * 호출 체인:
 *   bdev_poll_timeout_io() → spdk_bdev_for_each_channel() → [이 함수] → ctx->cb_fn()
 */
static void
bdev_channel_poll_timeout_io(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			     struct spdk_io_channel *io_ch, void *_ctx)
{
	struct poll_timeout_ctx *ctx  = _ctx; /* [한국어] 타임아웃 검사 컨텍스트 */
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(io_ch); /* [한국어] io_ch에서 bdev 채널 추출 */
	struct spdk_bdev_desc *desc = ctx->desc; /* [한국어] 감시 대상 디스크립터 */
	struct spdk_bdev_io *bdev_io;
	uint64_t now;

	spdk_spin_lock(&desc->spinlock); /* [한국어] desc->closed 안전 확인을 위한 spinlock */
	if (desc->closed == true) {
		/* [한국어] desc가 이미 닫힘 → 나머지 채널 순회 취소 (-1 전달) */
		spdk_spin_unlock(&desc->spinlock);
		spdk_bdev_for_each_channel_continue(i, -1);
		return;
	}
	spdk_spin_unlock(&desc->spinlock);

	now = spdk_get_ticks(); /* [한국어] 타임아웃 비교를 위한 현재 TSC */
	TAILQ_FOREACH(bdev_io, &bdev_ch->io_submitted, internal.ch_link) {
		/* Exclude any I/O that are generated via splitting. */
		/* [한국어] split 자식 I/O는 타임아웃 검사 제외 — 원본 부모 I/O만 검사 */
		if (bdev_io->internal.cb == bdev_io_split_done) {
			continue;
		}

		/* Once we find an I/O that has not timed out, we can immediately
		 * exit the loop.
		 */
		/* [한국어] io_submitted는 submit_tsc 오름차순 → 타임아웃 미경과 발견 시 이후 I/O도 안전 */
		if (now < (bdev_io->internal.submit_tsc +
			   ctx->timeout_in_sec * spdk_get_ticks_hz())) {
			goto end; /* [한국어] 타임아웃 미경과 I/O 발견 → 루프 조기 종료 */
		}

		if (bdev_io->internal.desc == desc) {
			/* [한국어] 이 디스크립터로 제출된 타임아웃 I/O → 사용자 콜백 호출 */
			ctx->cb_fn(ctx->cb_arg, bdev_io);
		}
	}

end:
	spdk_bdev_for_each_channel_continue(i, 0); /* [한국어] 다음 채널 계속 처리 */
}

/*
 * [한국어]
 * bdev_poll_timeout_io - I/O 타임아웃 검사 주기 폴러
 *
 * @arg: bdev 디스크립터 포인터 (SPDK_POLLER_REGISTER 시 등록)
 * @return: SPDK_POLLER_BUSY (항상 — 타임아웃 검사를 수행함)
 *
 * spdk_bdev_set_timeout() 호출 시 등록되는 1초 주기 폴러. 매 폴링마다 poll_timeout_ctx를
 * 할당하고 현재 desc의 콜백 설정을 복사한 뒤, spdk_bdev_for_each_channel()로 모든 채널에서
 * 타임아웃 I/O를 검사한다.
 * 채널 순회 중 desc가 닫힐 수 있으므로 refs를 증가시켜 보호하고,
 * 완료 콜백(bdev_channel_poll_timeout_io_done)에서 refs를 감소시킨다.
 *
 * 실행 컨텍스트: desc->thread 리액터 폴러.
 *
 * 호출 체인:
 *   SPDK 리액터 폴러 루프 → [이 함수] → spdk_bdev_for_each_channel() →
 *   bdev_channel_poll_timeout_io() → ctx->cb_fn() (타임아웃 I/O 발견 시)
 */
static int
bdev_poll_timeout_io(void *arg)
{
	struct spdk_bdev_desc *desc = arg; /* [한국어] 타임아웃 감시 대상 디스크립터 */
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc); /* [한국어] 디스크립터로부터 bdev 포인터 획득 */
	struct poll_timeout_ctx *ctx;

	ctx = calloc(1, sizeof(struct poll_timeout_ctx)); /* [한국어] 채널 순회 컨텍스트 할당 (0으로 초기화) */
	if (!ctx) {
		SPDK_ERRLOG("failed to allocate memory\n"); /* [한국어] 메모리 부족 → 에러 로그 후 BUSY 반환 */
		return SPDK_POLLER_BUSY;
	}
	ctx->desc = desc;                          /* [한국어] 감시 대상 디스크립터 저장 */
	ctx->cb_arg = desc->cb_arg;               /* [한국어] 타임아웃 콜백 인자 복사 */
	ctx->cb_fn = desc->cb_fn;                 /* [한국어] 타임아웃 콜백 함수 포인터 복사 */
	ctx->timeout_in_sec = desc->timeout_in_sec; /* [한국어] 타임아웃 임계값 복사 */

	/* Take a ref on the descriptor in case it gets closed while we are checking
	 * all of the channels.
	 */
	/* [한국어] 채널 순회 중 desc 닫힘 방지를 위해 refs 증가 (deferred close 지원) */
	spdk_spin_lock(&desc->spinlock);
	desc->refs++; /* [한국어] 참조 카운트 증가 — 완료 콜백에서 감소 */
	spdk_spin_unlock(&desc->spinlock);

	/* [한국어] 모든 채널에서 타임아웃 I/O 검사 시작 */
	spdk_bdev_for_each_channel(bdev, bdev_channel_poll_timeout_io, ctx,
				   bdev_channel_poll_timeout_io_done);

	return SPDK_POLLER_BUSY; /* [한국어] 타임아웃 검사 수행 → BUSY 반환 */
}

/*
 * [한국어]
 * spdk_bdev_set_timeout - bdev 디스크립터에 I/O 타임아웃 감시를 설정
 *
 * @desc:           타임아웃을 설정할 bdev 디스크립터
 * @timeout_in_sec: 타임아웃 임계값 (초 단위); 0이면 타임아웃 비활성화
 * @cb_fn:          타임아웃 발생 시 호출할 콜백 (timeout_in_sec > 0이면 non-NULL이어야 함)
 * @cb_arg:         콜백에 전달할 사용자 인자
 * @return:         성공 0, 폴러 등록 실패 시 -1
 *
 * 기존 타임아웃 폴러를 해제하고 새 설정으로 재등록한다. timeout_in_sec == 0이면
 * 폴러 해제만 하고 타임아웃 감시를 비활성화한다.
 * 폴러는 1초 주기로 bdev_poll_timeout_io를 호출해 모든 채널의 io_submitted를 검사한다.
 *
 * 실행 컨텍스트: desc->thread (호출자 스레드에서만 호출 가능, assert로 확인).
 *
 * 호출 체인:
 *   사용자 / RPC 코드 → [이 함수] → SPDK_POLLER_REGISTER(bdev_poll_timeout_io)
 */
int
spdk_bdev_set_timeout(struct spdk_bdev_desc *desc, uint64_t timeout_in_sec,
		      spdk_bdev_io_timeout_cb cb_fn, void *cb_arg)
{
	assert(desc->thread == spdk_get_thread()); /* [한국어] 디스크립터 소유 스레드에서만 호출 가능 */

	spdk_poller_unregister(&desc->io_timeout_poller); /* [한국어] 기존 타임아웃 폴러 해제 */

	if (timeout_in_sec) {
		/* [한국어] timeout_in_sec > 0: 새 타임아웃 폴러 등록 */
		assert(cb_fn != NULL); /* [한국어] 타임아웃 감시 시 콜백은 필수 */
		desc->io_timeout_poller = SPDK_POLLER_REGISTER(bdev_poll_timeout_io, desc, SPDK_SEC_TO_USEC);
		/* [한국어] bdev_poll_timeout_io를 1초 주기로 등록 */
		if (desc->io_timeout_poller == NULL) {
			SPDK_ERRLOG("can not register the desc timeout IO poller\n");
			return -1; /* [한국어] 폴러 등록 실패 → 에러 반환 */
		}
	}

	desc->cb_fn = cb_fn;               /* [한국어] 타임아웃 콜백 함수 저장 */
	desc->cb_arg = cb_arg;             /* [한국어] 타임아웃 콜백 인자 저장 */
	desc->timeout_in_sec = timeout_in_sec; /* [한국어] 타임아웃 임계값 저장 */

	return 0; /* [한국어] 설정 성공 */
}

static int
bdev_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_bdev		*bdev = __bdev_from_io_dev(io_device);
	struct spdk_bdev_channel	*ch = ctx_buf;
	struct spdk_io_channel		*mgmt_io_ch;
	struct spdk_bdev_mgmt_channel	*mgmt_ch;
	struct spdk_bdev_shared_resource *shared_resource;
	struct lba_range		*range;

	ch->bdev = bdev;
	ch->channel = bdev->fn_table->get_io_channel(bdev->ctxt);
	if (!ch->channel) {
		return -1;
	}

	ch->accel_channel = spdk_accel_get_io_channel();
	if (!ch->accel_channel) {
		spdk_put_io_channel(ch->channel);
		return -1;
	}

	spdk_trace_record(TRACE_BDEV_IOCH_CREATE, bdev->internal.trace_id, 0, 0,
			  spdk_thread_get_id(spdk_io_channel_get_thread(ch->channel)));

	assert(ch->histogram == NULL);
	if (bdev->internal.histogram_enabled) {
		ch->histogram = spdk_histogram_data_alloc();
		if (ch->histogram == NULL) {
			SPDK_ERRLOG("Could not allocate histogram\n");
		}
	}

	mgmt_io_ch = spdk_get_io_channel(&g_bdev_mgr);
	if (!mgmt_io_ch) {
		spdk_put_io_channel(ch->channel);
		spdk_put_io_channel(ch->accel_channel);
		return -1;
	}

	mgmt_ch = __io_ch_to_bdev_mgmt_ch(mgmt_io_ch);
	TAILQ_FOREACH(shared_resource, &mgmt_ch->shared_resources, link) {
		if (shared_resource->shared_ch == ch->channel) {
			spdk_put_io_channel(mgmt_io_ch);
			shared_resource->ref++;
			break;
		}
	}

	if (shared_resource == NULL) {
		shared_resource = calloc(1, sizeof(*shared_resource));
		if (shared_resource == NULL) {
			spdk_put_io_channel(ch->channel);
			spdk_put_io_channel(ch->accel_channel);
			spdk_put_io_channel(mgmt_io_ch);
			return -1;
		}

		shared_resource->mgmt_ch = mgmt_ch;
		shared_resource->io_outstanding = 0;
		TAILQ_INIT(&shared_resource->nomem_io);
		shared_resource->nomem_threshold = 0;
		shared_resource->shared_ch = ch->channel;
		shared_resource->ref = 1;
		TAILQ_INSERT_TAIL(&mgmt_ch->shared_resources, shared_resource, link);
	}

	ch->io_outstanding = 0;
	TAILQ_INIT(&ch->locked_ranges);
	TAILQ_INIT(&ch->qos_queued_io);
	ch->flags = 0;
	ch->trace_id = bdev->internal.trace_id;
	ch->shared_resource = shared_resource;

	TAILQ_INIT(&ch->io_submitted);
	TAILQ_INIT(&ch->io_locked);
	TAILQ_INIT(&ch->io_accel_exec);
	TAILQ_INIT(&ch->io_memory_domain);

	ch->stat = bdev_alloc_io_stat(false);
	if (ch->stat == NULL) {
		bdev_channel_destroy_resource(ch);
		return -1;
	}

	ch->stat->ticks_rate = spdk_get_ticks_hz();

#ifdef SPDK_CONFIG_VTUNE
	{
		char *name;
		__itt_init_ittlib(NULL, 0);
		name = spdk_sprintf_alloc("spdk_bdev_%s_%p", ch->bdev->name, ch);
		if (!name) {
			bdev_channel_destroy_resource(ch);
			return -1;
		}
		ch->handle = __itt_string_handle_create(name);
		free(name);
		ch->start_tsc = spdk_get_ticks();
		ch->interval_tsc = spdk_get_ticks_hz() / 100;
		ch->prev_stat = bdev_alloc_io_stat(false);
		if (ch->prev_stat == NULL) {
			bdev_channel_destroy_resource(ch);
			return -1;
		}
	}
#endif

	spdk_spin_lock(&bdev->internal.spinlock);
	bdev_enable_qos(bdev, ch);

	TAILQ_FOREACH(range, &bdev->internal.locked_ranges, tailq) {
		struct lba_range *new_range;

		new_range = calloc(1, sizeof(*new_range));
		if (new_range == NULL) {
			spdk_spin_unlock(&bdev->internal.spinlock);
			bdev_channel_destroy_resource(ch);
			return -1;
		}
		new_range->length = range->length;
		new_range->offset = range->offset;
		new_range->locked_ctx = range->locked_ctx;
		TAILQ_INSERT_TAIL(&ch->locked_ranges, new_range, tailq);
	}

	spdk_spin_unlock(&bdev->internal.spinlock);

	return 0;
}

/*
 * [한국어]
 * bdev_abort_all_buf_io_cb - iobuf 대기 큐 순회 시 특정 채널의 I/O를 중단하는 콜백
 *
 * @ch:     iobuf 채널 포인터 (spdk_iobuf_entry_abort에 전달)
 * @entry:  현재 순회 중인 iobuf 대기 항목
 * @cb_ctx: 중단 대상 bdev_ch (struct spdk_bdev_channel 포인터)
 * @return: 0 (항상; 순회를 계속함)
 *
 * spdk_iobuf_for_each_entry()가 iobuf 대기 큐를 순회할 때 각 항목마다 호출된다.
 * SPDK_CONTAINEROF를 통해 iobuf 항목으로부터 bdev_io 포인터를 복원하고,
 * 이 I/O가 중단 대상 채널(bdev_ch) 소속인 경우 iobuf 버퍼 요청을 취소하고
 * bdev_io_get_buf_complete()로 ABORTED 완료를 처리한다.
 *
 * 호출 체인:
 *   bdev_abort_all_buf_io() → spdk_iobuf_for_each_entry() → [이 함수]
 *   → spdk_iobuf_entry_abort() + bdev_io_get_buf_complete()
 */
static int
bdev_abort_all_buf_io_cb(struct spdk_iobuf_channel *ch, struct spdk_iobuf_entry *entry,
			 void *cb_ctx)
{
	struct spdk_bdev_channel *bdev_ch = cb_ctx; /* [한국어] 중단 대상 bdev 채널 */
	struct spdk_bdev_io *bdev_io;
	uint64_t buf_len;

	bdev_io = SPDK_CONTAINEROF(entry, struct spdk_bdev_io, internal.iobuf);
	/* [한국어] iobuf 항목에서 bdev_io 포인터 복원 (internal.iobuf 필드로 역참조) */
	if (bdev_io->internal.ch == bdev_ch) {
		/* [한국어] 이 I/O가 중단 대상 채널 소속인 경우 → 버퍼 요청 취소 */
		buf_len = bdev_io_get_max_buf_len(bdev_io, bdev_io->internal.buf.len);
		/* [한국어] 취소할 버퍼의 최대 크기 계산 (iobuf 반환 크기 결정에 사용) */
		spdk_iobuf_entry_abort(ch, entry, buf_len); /* [한국어] iobuf 큐에서 이 항목 제거 */
		bdev_io_get_buf_complete(bdev_io, false);   /* [한국어] false = 버퍼 획득 실패 → ABORTED 완료 처리 */
	}

	return 0; /* [한국어] 0 반환으로 순회 계속 (1을 반환하면 조기 종료) */
}

/*
 * Abort I/O that are waiting on a data buffer.
 */
/*
 * [한국어]
 * bdev_abort_all_buf_io - 특정 채널에서 버퍼 대기 중인 모든 I/O를 중단
 *
 * @mgmt_ch: 버퍼 대기 큐를 보유한 bdev 관리 채널
 * @ch:      중단 대상 bdev 채널
 *
 * iobuf 대기 큐 전체를 순회하며 ch 소속 I/O를 모두 중단한다.
 * 채널 파괴 시 bdev_channel_abort_queued_ios()가 nomem 큐와 함께 이 함수를 호출한다.
 *
 * 호출 체인:
 *   bdev_channel_abort_queued_ios() → [이 함수] → spdk_iobuf_for_each_entry()
 *   → bdev_abort_all_buf_io_cb()
 */
static void
bdev_abort_all_buf_io(struct spdk_bdev_mgmt_channel *mgmt_ch, struct spdk_bdev_channel *ch)
{
	spdk_iobuf_for_each_entry(&mgmt_ch->iobuf, bdev_abort_all_buf_io_cb, ch);
	/* [한국어] iobuf 대기 큐 전체를 순회해 ch 소속 I/O를 모두 취소 */
}

/*
 * Abort I/O that are queued waiting for submission.  These types of I/O are
 *  linked using the spdk_bdev_io link TAILQ_ENTRY.
 */
/*
 * [한국어]
 * bdev_abort_all_queued_io - 제출 대기 큐의 특정 채널 I/O를 모두 ABORTED로 완료
 *
 * @queue: 중단할 I/O TAILQ (nomem_io 등 제출 대기 큐)
 * @ch:    중단 대상 bdev 채널
 *
 * TAILQ_FOREACH_SAFE로 큐를 순회하며 ch 소속 I/O를 찾아 제거하고 ABORTED 완료를 발행한다.
 * spdk_bdev_io_complete()는 io_outstanding을 감소시키므로, 실제로 모듈에 제출되지 않은
 * I/O에 대해서는 io_outstanding을 미리 증가시켜 균형을 맞춘다 (RESET 타입 제외).
 *
 * 실행 컨텍스트: 채널이 속한 리액터 스레드.
 *
 * 호출 체인:
 *   bdev_abort_all_nomem_io() → [이 함수] → spdk_bdev_io_complete()
 */
static void
bdev_abort_all_queued_io(bdev_io_tailq_t *queue, struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_io *bdev_io, *tmp;

	/* [한국어] TAILQ_FOREACH_SAFE: 순회 중 항목 제거가 안전하도록 tmp로 다음 포인터 미리 저장 */
	TAILQ_FOREACH_SAFE(bdev_io, queue, internal.link, tmp) {
		if (bdev_io->internal.ch == ch) {
			/* [한국어] 이 I/O가 중단 대상 채널 소속 → 큐에서 제거 후 완료 처리 */
			TAILQ_REMOVE(queue, bdev_io, internal.link);
			/*
			 * spdk_bdev_io_complete() assumes that the completed I/O had
			 *  been submitted to the bdev module.  Since in this case it
			 *  hadn't, bump io_outstanding to account for the decrement
			 *  that spdk_bdev_io_complete() will do.
			 */
			/* [한국어] spdk_bdev_io_complete()가 io_outstanding을 감소시키므로,
			 * 아직 모듈에 제출되지 않은 I/O에 대해 미리 카운터를 증가시켜 균형 유지.
			 * RESET 타입은 io_outstanding 계정에서 제외됨 */
			if (bdev_io->type != SPDK_BDEV_IO_TYPE_RESET) {
				bdev_io_increment_outstanding(ch, ch->shared_resource);
				/* [한국어] 미제출 I/O의 io_outstanding 사전 증가 */
			}
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_ABORTED);
			/* [한국어] ABORTED 상태로 완료 처리 → 완료 콜백 호출 후 io_outstanding 감소 */
		}
	}
}

/*
 * [한국어]
 * bdev_abort_all_nomem_io - 특정 채널의 NOMEM 재시도 큐 I/O를 모두 중단
 *
 * @ch: 중단 대상 bdev 채널
 *
 * shared_resource->nomem_io 큐에서 ch 소속 I/O를 모두 ABORTED로 완료시킨다.
 * nomem_abort_in_progress 플래그를 설정해 nomem 폴러가 abort 중에 큐를 건드리지 않도록 한다.
 * 채널 파괴 시 bdev_channel_abort_queued_ios()에서 호출된다.
 *
 * 호출 체인:
 *   bdev_channel_abort_queued_ios() → [이 함수] → bdev_abort_all_queued_io()
 */
static inline void
bdev_abort_all_nomem_io(struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_shared_resource *shared_resource = ch->shared_resource;
	/* [한국어] 이 채널의 공유 리소스 — nomem_io 큐를 보유 */

	shared_resource->nomem_abort_in_progress = true;
	/* [한국어] nomem_abort_in_progress 플래그 설정 → nomem 폴러의 동시 접근 방지 */
	bdev_abort_all_queued_io(&shared_resource->nomem_io, ch);
	/* [한국어] nomem_io 큐에서 ch 소속 I/O를 모두 ABORTED 완료 */
	shared_resource->nomem_abort_in_progress = false;
	/* [한국어] abort 완료 후 플래그 해제 */
}

/*
 * [한국어]
 * bdev_abort_queued_io - 큐에서 특정 I/O 하나를 찾아 ABORTED로 완료
 *
 * @queue:        탐색할 bdev_io TAILQ
 * @bio_to_abort: 중단할 특정 bdev_io 포인터
 * @return:       발견하여 중단했으면 true, 큐에 없으면 false
 *
 * bdev_abort_io()에서 특정 I/O를 목표로 abort할 때 사용한다.
 * TAILQ를 선형 탐색해 bio_to_abort를 찾고, 발견하면 큐에서 제거 후 ABORTED 완료를 발행한다.
 * bdev_abort_all_queued_io()와 동일하게 미제출 I/O에 대한 io_outstanding 보정을 수행한다.
 *
 * 호출 체인:
 *   bdev_abort_io() → [이 함수] → spdk_bdev_io_complete()
 */
static bool
bdev_abort_queued_io(bdev_io_tailq_t *queue, struct spdk_bdev_io *bio_to_abort)
{
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *ch;

	TAILQ_FOREACH(bdev_io, queue, internal.link) {
		/* [한국어] 큐 선형 탐색 — 목표 I/O 찾기 */
		if (bdev_io == bio_to_abort) {
			/* [한국어] 목표 I/O 발견 → 큐에서 제거 */
			TAILQ_REMOVE(queue, bio_to_abort, internal.link);
			if (bdev_io->type != SPDK_BDEV_IO_TYPE_RESET) {
				/* [한국어] RESET 제외: io_outstanding 사전 증가 (미제출 보정) */
				ch = bdev_io->internal.ch;
				bdev_io_increment_outstanding(ch, ch->shared_resource);
			}
			spdk_bdev_io_complete(bio_to_abort, SPDK_BDEV_IO_STATUS_ABORTED);
			/* [한국어] ABORTED 상태로 완료 → 완료 콜백 실행 후 io_outstanding 감소 */
			return true; /* [한국어] 성공적으로 abort함 */
		}
	}

	return false; /* [한국어] 큐에서 해당 I/O를 찾지 못함 */
}

/*
 * [한국어]
 * bdev_abort_buf_io_cb - iobuf 대기 큐에서 특정 I/O 하나를 찾아 중단하는 콜백
 *
 * @ch:     iobuf 채널 포인터
 * @entry:  현재 순회 중인 iobuf 대기 항목
 * @cb_ctx: 중단할 특정 bdev_io 포인터 (bio_to_abort)
 * @return: 발견/중단 시 1 (순회 조기 종료), 아니면 0 (계속 순회)
 *
 * bdev_abort_buf_io()가 spdk_iobuf_for_each_entry()에 등록하는 콜백.
 * entry에서 복원한 bdev_io가 bio_to_abort와 동일하면 iobuf 요청을 취소하고 1 반환.
 * spdk_iobuf_for_each_entry()는 1이 반환되면 순회를 조기 종료한다.
 *
 * 호출 체인:
 *   bdev_abort_buf_io() → spdk_iobuf_for_each_entry() → [이 함수]
 */
static int
bdev_abort_buf_io_cb(struct spdk_iobuf_channel *ch, struct spdk_iobuf_entry *entry, void *cb_ctx)
{
	struct spdk_bdev_io *bdev_io, *bio_to_abort = cb_ctx; /* [한국어] 중단 대상 I/O */
	uint64_t buf_len;

	bdev_io = SPDK_CONTAINEROF(entry, struct spdk_bdev_io, internal.iobuf);
	/* [한국어] iobuf 항목에서 bdev_io 복원 */
	if (bdev_io == bio_to_abort) {
		/* [한국어] 중단 대상 I/O 발견 → 버퍼 요청 취소 및 완료 처리 */
		buf_len = bdev_io_get_max_buf_len(bdev_io, bdev_io->internal.buf.len);
		spdk_iobuf_entry_abort(ch, entry, buf_len); /* [한국어] iobuf 대기 큐에서 제거 */
		bdev_io_get_buf_complete(bdev_io, false);   /* [한국어] false = 실패 → ABORTED 완료 */
		return 1; /* [한국어] 1 반환으로 for_each_entry 조기 종료 */
	}

	return 0; /* [한국어] 대상 I/O 아님 → 순회 계속 */
}

/*
 * [한국어]
 * bdev_abort_buf_io - iobuf 대기 큐에서 특정 I/O를 찾아 중단
 *
 * @mgmt_ch:      iobuf 채널을 보유한 관리 채널
 * @bio_to_abort: 중단할 특정 bdev_io 포인터
 * @return:       발견하여 중단했으면 true, 큐에 없으면 false
 *
 * bdev_abort_io()에서 iobuf 버퍼 대기 큐를 특정 I/O로 abort할 때 사용한다.
 *
 * 호출 체인:
 *   bdev_abort_io() → [이 함수] → spdk_iobuf_for_each_entry() → bdev_abort_buf_io_cb()
 */
static bool
bdev_abort_buf_io(struct spdk_bdev_mgmt_channel *mgmt_ch, struct spdk_bdev_io *bio_to_abort)
{
	int rc;

	rc = spdk_iobuf_for_each_entry(&mgmt_ch->iobuf, bdev_abort_buf_io_cb, bio_to_abort);
	/* [한국어] 콜백이 1을 반환하면 중단 성공, 끝까지 찾지 못하면 0 반환 */
	return rc == 1; /* [한국어] rc == 1이면 성공적으로 중단, 아니면 false */
}

/*
 * [한국어]
 * bdev_abort_unsubmitted_buf_io_cb - caller_ctx로 미제출 버퍼 대기 I/O를 찾아 중단하는 콜백
 *
 * @ch:     iobuf 채널
 * @entry:  현재 순회 중인 iobuf 대기 항목
 * @cb_ctx: 중단할 I/O의 caller_ctx 포인터 (bio_cb_arg)
 * @return: 발견/중단 시 true(1), 아니면 false(0)
 *
 * bdev_io 포인터가 아닌 caller_ctx로 미제출 I/O를 식별한다.
 * 주로 spdk_bdev_read/write 등 미제출 콜백 I/O를 abort할 때 사용.
 * bdev_abort_buf_io_cb와 달리 직접 포인터가 아닌 ctx 값으로 비교한다.
 *
 * 호출 체인:
 *   bdev_abort_unsubmitted_buf_io() → spdk_iobuf_for_each_entry() → [이 함수]
 */
static int
bdev_abort_unsubmitted_buf_io_cb(struct spdk_iobuf_channel *ch, struct spdk_iobuf_entry *entry,
				 void *cb_ctx)
{
	void *bio_cb_arg = cb_ctx; /* [한국어] 중단할 I/O의 caller_ctx 값 */
	struct spdk_bdev_io *bdev_io;
	uint64_t buf_len;

	bdev_io = SPDK_CONTAINEROF(entry, struct spdk_bdev_io, internal.iobuf);
	/* [한국어] iobuf 항목에서 bdev_io 복원 */
	if (bdev_io->internal.caller_ctx == bio_cb_arg) {
		/* [한국어] caller_ctx가 일치하는 I/O 발견 → 버퍼 요청 취소 */
		buf_len = bdev_io_get_max_buf_len(bdev_io, bdev_io->internal.buf.len);
		spdk_iobuf_entry_abort(ch, entry, buf_len); /* [한국어] iobuf 대기 큐에서 제거 */
		bdev_io_get_buf_complete(bdev_io, false);   /* [한국어] 실패 완료 → ABORTED 처리 */
		return true; /* [한국어] 발견/중단 완료 → 순회 조기 종료 */
	}

	return false; /* [한국어] 대상 I/O 아님 → 계속 순회 */
}

/*
 * [한국어]
 * bdev_abort_unsubmitted_buf_io - caller_ctx로 미제출 버퍼 대기 I/O를 찾아 중단
 *
 * @mgmt_ch:    iobuf 채널 보유 관리 채널
 * @bio_cb_arg: 중단할 I/O의 caller_ctx 포인터
 * @return:     발견하여 중단했으면 true, 없으면 false
 *
 * 미제출 콜백 기반 I/O(e.g. spdk_bdev_read_blocks_with_md)를 abort할 때 사용한다.
 * bdev_io 포인터 대신 caller_ctx로 식별한다.
 *
 * 호출 체인:
 *   abort 처리 경로 → [이 함수] → spdk_iobuf_for_each_entry() → bdev_abort_unsubmitted_buf_io_cb()
 */
static bool
bdev_abort_unsubmitted_buf_io(struct spdk_bdev_mgmt_channel *mgmt_ch, void *bio_cb_arg)
{
	int rc;

	rc = spdk_iobuf_for_each_entry(&mgmt_ch->iobuf, bdev_abort_unsubmitted_buf_io_cb,
				       bio_cb_arg);
	/* [한국어] 콜백이 1(true)를 반환하면 중단 성공, 아니면 0 */
	return rc == 1; /* [한국어] rc == 1이면 성공적으로 중단 */
}

/*
 * [한국어]
 * bdev_qos_channel_destroy - 구 QoS 채널과 폴러를 해제하는 비동기 콜백
 *
 * @cb_arg: 해제할 old_qos (struct spdk_bdev_qos 포인터)
 *
 * bdev_qos_destroy()가 old_qos->thread에 spdk_thread_send_msg()로 전송하는 콜백.
 * 해당 QoS 채널을 소유한 리액터 스레드에서 실행되어 I/O 채널 참조 해제와
 * QoS 폴러(bdev_channel_poll_qos)를 안전하게 종료한다.
 * 폴러와 채널이 모두 같은 스레드에 속해야 하므로 비동기 메시지로 처리.
 *
 * 실행 컨텍스트: old_qos->thread (QoS 채널 소유 스레드).
 *
 * 호출 체인:
 *   bdev_qos_destroy() → spdk_thread_send_msg() → [이 함수]
 *   → spdk_put_io_channel() + spdk_poller_unregister()
 */
static void
bdev_qos_channel_destroy(void *cb_arg)
{
	struct spdk_bdev_qos *qos = cb_arg; /* [한국어] 해제할 구 QoS 구조체 */

	spdk_put_io_channel(spdk_io_channel_from_ctx(qos->ch));
	/* [한국어] QoS 채널 ctx에서 io_channel 포인터 복원 후 참조 해제 */
	spdk_poller_unregister(&qos->poller); /* [한국어] QoS 타임슬라이스 폴러 등록 해제 */

	SPDK_DEBUGLOG(bdev, "Free QoS %p.\n", qos); /* [한국어] 디버그 로그 */

	free(qos); /* [한국어] 구 QoS 구조체 메모리 해제 */
}

/*
 * [한국어]
 * bdev_qos_destroy - QoS 채널을 안전하게 비동기 종료하는 함수
 *
 * @bdev: QoS를 종료할 bdev 포인터
 * @return: 성공 0, 메모리 부족 시 -ENOMEM
 *
 * QoS 폴러를 깨끗하게 종료하는 것이 까다롭다 — 비동기 처리 중에도 사용자가 새 디스크립터를
 * 열어 새 채널을 생성하고 새 QoS 폴러를 기동할 수 있기 때문이다.
 *
 * 해결 전략 (더블 버퍼 스왑):
 *   1. new_qos를 할당하고 old_qos의 내용을 복사.
 *   2. new_qos의 ch/thread/poller/타임슬라이스 카운터를 0으로 초기화 (limit 값은 보존).
 *   3. bdev->internal.qos = new_qos로 교체 — 이후 새 채널은 new_qos 기반으로 동작.
 *   4. old_qos는 해당 스레드에서 비동기로(bdev_qos_channel_destroy 메시지) 해제.
 *      old_qos->thread == NULL이면 채널이 없었으므로 즉시 free().
 *
 * bdev 파괴 경로는 old_qos 해제가 완료되지 않아도 계속 진행할 수 있다.
 * 최종 채널이 반환될 때까지 bdev 리소스는 유지되므로 안전하다.
 *
 * 실행 컨텍스트: 앱 스레드 (bdev->internal.spinlock 보유 상태에서 호출 가능).
 *
 * 호출 체인:
 *   spdk_bdev_set_qos_rate_limits() / bdev_unregister_unsafe() → [이 함수]
 *   → spdk_thread_send_msg() → bdev_qos_channel_destroy()
 */
static int
bdev_qos_destroy(struct spdk_bdev *bdev)
{
	int i;

	/*
	 * Cleanly shutting down the QoS poller is tricky, because
	 * during the asynchronous operation the user could open
	 * a new descriptor and create a new channel, spawning
	 * a new QoS poller.
	 *
	 * The strategy is to create a new QoS structure here and swap it
	 * in. The shutdown path then continues to refer to the old one
	 * until it completes and then releases it.
	 */
	struct spdk_bdev_qos *new_qos, *old_qos;

	old_qos = bdev->internal.qos; /* [한국어] 기존 QoS 구조체 포인터 저장 */

	new_qos = calloc(1, sizeof(*new_qos)); /* [한국어] 교체용 새 QoS 구조체 할당 (0으로 초기화) */
	if (!new_qos) {
		SPDK_ERRLOG("Unable to allocate memory to shut down QoS.\n");
		return -ENOMEM; /* [한국어] 할당 실패 → -ENOMEM 반환 */
	}

	/* Copy the old QoS data into the newly allocated structure */
	memcpy(new_qos, old_qos, sizeof(*new_qos)); /* [한국어] old_qos의 rate_limits.limit 값 등 보존을 위해 전체 복사 */

	/* Zero out the key parts of the QoS structure */
	/* [한국어] 새 QoS 구조체의 런타임 상태 초기화 — 채널/폴러/타임슬라이스 카운터 클리어 */
	new_qos->ch = NULL;     /* [한국어] 아직 채널 없음 → 다음 채널 생성 시 bdev_enable_qos가 선택 */
	new_qos->thread = NULL; /* [한국어] 아직 스레드 없음 */
	new_qos->poller = NULL; /* [한국어] 아직 폴러 없음 */
	/*
	 * The limit member of spdk_bdev_qos_limit structure is not zeroed.
	 * It will be used later for the new QoS structure.
	 */
	/* [한국어] limit 값은 0으로 초기화하지 않음 — 새 QoS에서 동일한 rate limit 유지 */
	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		/* [한국어] 타임슬라이스 카운터들은 0으로 리셋 — 새 채널이 생기면 재초기화 */
		new_qos->rate_limits[i].remaining_this_timeslice = 0;
		new_qos->rate_limits[i].min_per_timeslice = 0;
		new_qos->rate_limits[i].max_per_timeslice = 0;
	}

	bdev->internal.qos = new_qos; /* [한국어] 원자적으로 새 QoS 구조체로 교체 */

	if (old_qos->thread == NULL) {
		/* [한국어] 채널이 한 번도 선택되지 않은 경우 → 즉시 old_qos 해제 */
		free(old_qos);
	} else {
		/* [한국어] QoS 폴러가 실행 중인 스레드에 비동기 해제 메시지 전송 */
		spdk_thread_send_msg(old_qos->thread, bdev_qos_channel_destroy, old_qos);
	}

	/* It is safe to continue with destroying the bdev even though the QoS channel hasn't
	 * been destroyed yet. The destruction path will end up waiting for the final
	 * channel to be put before it releases resources. */
	/* [한국어] old_qos 해제가 완료되지 않아도 bdev 파괴를 계속 진행할 수 있음 */

	return 0; /* [한국어] QoS 스왑 성공 */
}

/*
 * [한국어]
 * spdk_bdev_add_io_stat - 두 I/O 통계 구조체를 합산 (채널 집계 등에 사용)
 *
 * @total: 누적 대상 통계 구조체 (합산 결과가 여기 저장됨)
 * @add:   더할 통계 구조체 (채널별 stat 등)
 *
 * bytes_read/num_read_ops 등 카운터는 단순 덧셈으로 합산.
 * 최대/최소 레이턴시는 min/max 비교를 통해 전체 최대·최소를 유지한다.
 * bdev_channel_destroy() 시 채널 stat을 bdev->internal.stat에 병합하거나,
 * spdk_bdev_get_device_stat()의 채널 순회 완료 후 합산에 사용된다.
 *
 * 실행 컨텍스트: bdev->internal.spinlock 보유 상태에서 호출 (채널 파괴 시).
 *
 * 호출 체인:
 *   bdev_channel_destroy() → [이 함수]
 *   bdev_get_device_stat_done() → [이 함수]
 */
void
spdk_bdev_add_io_stat(struct spdk_bdev_io_stat *total, struct spdk_bdev_io_stat *add)
{
	/* [한국어] 카운터 필드 단순 덧셈 합산 */
	total->bytes_read += add->bytes_read;             /* [한국어] 읽은 총 바이트 수 누적 */
	total->num_read_ops += add->num_read_ops;         /* [한국어] 읽기 I/O 완료 횟수 누적 */
	total->bytes_written += add->bytes_written;       /* [한국어] 쓴 총 바이트 수 누적 */
	total->num_write_ops += add->num_write_ops;       /* [한국어] 쓰기 I/O 완료 횟수 누적 */
	total->bytes_unmapped += add->bytes_unmapped;     /* [한국어] UNMAP된 총 바이트 수 누적 */
	total->num_unmap_ops += add->num_unmap_ops;       /* [한국어] UNMAP I/O 완료 횟수 누적 */
	total->bytes_copied += add->bytes_copied;         /* [한국어] 복사된 총 바이트 수 누적 */
	total->num_copy_ops += add->num_copy_ops;         /* [한국어] COPY I/O 완료 횟수 누적 */
	total->read_latency_ticks += add->read_latency_ticks;     /* [한국어] 읽기 레이턴시 누적 (평균 계산용) */
	total->write_latency_ticks += add->write_latency_ticks;   /* [한국어] 쓰기 레이턴시 누적 */
	total->unmap_latency_ticks += add->unmap_latency_ticks;   /* [한국어] UNMAP 레이턴시 누적 */
	total->copy_latency_ticks += add->copy_latency_ticks;     /* [한국어] COPY 레이턴시 누적 */
	/* [한국어] 최대/최소 레이턴시: min/max 비교로 전체 최대·최소를 유지 */
	if (total->max_read_latency_ticks < add->max_read_latency_ticks) {
		total->max_read_latency_ticks = add->max_read_latency_ticks; /* [한국어] 더 큰 최대값으로 업데이트 */
	}
	if (total->min_read_latency_ticks > add->min_read_latency_ticks) {
		total->min_read_latency_ticks = add->min_read_latency_ticks; /* [한국어] 더 작은 최소값으로 업데이트 */
	}
	if (total->max_write_latency_ticks < add->max_write_latency_ticks) {
		total->max_write_latency_ticks = add->max_write_latency_ticks;
	}
	if (total->min_write_latency_ticks > add->min_write_latency_ticks) {
		total->min_write_latency_ticks = add->min_write_latency_ticks;
	}
	if (total->max_unmap_latency_ticks < add->max_unmap_latency_ticks) {
		total->max_unmap_latency_ticks = add->max_unmap_latency_ticks;
	}
	if (total->min_unmap_latency_ticks > add->min_unmap_latency_ticks) {
		total->min_unmap_latency_ticks = add->min_unmap_latency_ticks;
	}
	if (total->max_copy_latency_ticks < add->max_copy_latency_ticks) {
		total->max_copy_latency_ticks = add->max_copy_latency_ticks;
	}
	if (total->min_copy_latency_ticks > add->min_copy_latency_ticks) {
		total->min_copy_latency_ticks = add->min_copy_latency_ticks;
	}
}

/*
 * [한국어]
 * bdev_get_io_stat - I/O 통계를 스냅샷 복사 (io_error 포인터는 보존하고 내용만 복사)
 *
 * @to_stat:   복사 대상 통계 구조체
 * @from_stat: 복사 원본 통계 구조체
 *
 * spdk_bdev_io_stat의 io_error 필드 앞까지만 memcpy로 복사하고,
 * to_stat의 io_error 포인터는 그대로 유지하면서 내용만 복사한다.
 * 이는 to_stat->io_error 포인터를 덮어쓰지 않기 위한 설계이다.
 *
 * 호출 체인:
 *   bdev_get_device_stat_cb() / VTune 통계 수집 → [이 함수]
 */
static void
bdev_get_io_stat(struct spdk_bdev_io_stat *to_stat, struct spdk_bdev_io_stat *from_stat)
{
	memcpy(to_stat, from_stat, offsetof(struct spdk_bdev_io_stat, io_error));
	/* [한국어] io_error 포인터 필드 직전까지만 복사 — to_stat->io_error 포인터 보존 */

	if (to_stat->io_error != NULL && from_stat->io_error != NULL) {
		/* [한국어] 양쪽 모두 io_error 통계 공간이 있는 경우 → 내용(에러 카운터) 복사 */
		memcpy(to_stat->io_error, from_stat->io_error,
		       sizeof(struct spdk_bdev_io_error_stat));
	}
}

/*
 * [한국어]
 * spdk_bdev_reset_io_stat - I/O 통계를 초기화 (리셋 모드에 따라 선택적 초기화)
 *
 * @stat: 초기화할 통계 구조체
 * @mode: 리셋 범위 (NONE=아무것도 안 함, ERROR=에러 카운터만, ALL=전체)
 *
 * 리셋 모드에 따라 에러 카운터, 최대/최소 레이턴시, 전체 카운터 중 선택적으로 초기화.
 * 최소 레이턴시는 UINT64_MAX로 초기화해야 첫 번째 실제 값으로 올바르게 업데이트된다.
 * bdev_alloc_io_stat(), spdk_bdev_get_device_stat() 등에서 통계 초기화 시 사용.
 *
 * 호출 체인:
 *   bdev_alloc_io_stat() → [이 함수]
 *   RPC "bdev_reset_stat" 핸들러 → [이 함수]
 */
void
spdk_bdev_reset_io_stat(struct spdk_bdev_io_stat *stat, enum spdk_bdev_reset_stat_mode mode)
{
	if (mode == SPDK_BDEV_RESET_STAT_NONE) {
		return; /* [한국어] NONE: 아무것도 초기화하지 않음 */
	}

	if (mode == SPDK_BDEV_RESET_STAT_ERROR || mode == SPDK_BDEV_RESET_STAT_ALL) {
		/* [한국어] ERROR 또는 ALL 모드: io_error 카운터 초기화 */
		if (stat->io_error != NULL) {
			memset(stat->io_error, 0, sizeof(struct spdk_bdev_io_error_stat));
			/* [한국어] 에러 상태별 카운터(error_status 배열) 전부 0으로 초기화 */
		}
		if (mode == SPDK_BDEV_RESET_STAT_ERROR) {
			return; /* [한국어] ERROR 모드: 에러 카운터만 초기화하고 반환 */
		}
	}

	/* [한국어] 최대/최소 레이턴시 초기화: max는 0, min은 UINT64_MAX로 설정 */
	stat->max_read_latency_ticks = 0;          /* [한국어] 최대 읽기 레이턴시 초기화 */
	stat->min_read_latency_ticks = UINT64_MAX; /* [한국어] 최소 읽기 레이턴시 초기화 (첫 값이 반드시 최소가 되도록) */
	stat->max_write_latency_ticks = 0;
	stat->min_write_latency_ticks = UINT64_MAX;
	stat->max_unmap_latency_ticks = 0;
	stat->min_unmap_latency_ticks = UINT64_MAX;
	stat->max_copy_latency_ticks = 0;
	stat->min_copy_latency_ticks = UINT64_MAX;

	if (mode != SPDK_BDEV_RESET_STAT_ALL) {
		return; /* [한국어] LATENCY 모드(미정의): 레이턴시만 초기화하고 반환 */
	}

	/* [한국어] ALL 모드: 카운터 전체 초기화 */
	stat->bytes_read = 0;
	stat->num_read_ops = 0;
	stat->bytes_written = 0;
	stat->num_write_ops = 0;
	stat->bytes_unmapped = 0;
	stat->num_unmap_ops = 0;
	stat->bytes_copied = 0;
	stat->num_copy_ops = 0;
	stat->read_latency_ticks = 0;
	stat->write_latency_ticks = 0;
	stat->unmap_latency_ticks = 0;
	stat->copy_latency_ticks = 0;
}

/*
 * [한국어]
 * bdev_alloc_io_stat - I/O 통계 구조체 할당 및 초기화
 *
 * @io_error_stat: true이면 io_error 에러 통계 서브구조체도 함께 할당
 * @return:        초기화된 spdk_bdev_io_stat 포인터, 메모리 부족 시 NULL
 *
 * 채널별 stat, bdev 전체 stat 등을 생성할 때 호출된다.
 * io_error_stat == true이면 에러 종류별 카운터(io_error) 공간도 별도 할당한다.
 * 할당 후 spdk_bdev_reset_io_stat(ALL)로 전부 초기화한다.
 *
 * 호출 체인:
 *   bdev_channel_create() → [이 함수] (채널 stat 생성)
 *   spdk_bdev_register() → [이 함수] (bdev 전체 stat 생성)
 */
struct spdk_bdev_io_stat *
bdev_alloc_io_stat(bool io_error_stat)
{
	struct spdk_bdev_io_stat *stat;

	stat = malloc(sizeof(struct spdk_bdev_io_stat)); /* [한국어] 기본 통계 구조체 할당 */
	if (stat == NULL) {
		return NULL; /* [한국어] 메모리 부족 → NULL 반환 */
	}

	if (io_error_stat) {
		/* [한국어] 에러 종류별 통계 서브구조체 추가 할당 */
		stat->io_error = malloc(sizeof(struct spdk_bdev_io_error_stat));
		if (stat->io_error == NULL) {
			free(stat); /* [한국어] 부분 할당 실패 시 기본 stat 해제 후 NULL */
			return NULL;
		}
	} else {
		stat->io_error = NULL; /* [한국어] 에러 통계 불필요 → 포인터 NULL로 초기화 */
	}

	spdk_bdev_reset_io_stat(stat, SPDK_BDEV_RESET_STAT_ALL); /* [한국어] 전체 통계 0으로 초기화 */

	return stat; /* [한국어] 초기화된 통계 구조체 반환 */
}

/*
 * [한국어]
 * bdev_free_io_stat - I/O 통계 구조체와 io_error 서브구조체 해제
 *
 * @stat: 해제할 통계 구조체 포인터 (NULL 허용 — 즉시 반환)
 *
 * bdev_alloc_io_stat()로 할당된 통계 구조체를 안전하게 해제한다.
 * io_error 포인터가 있으면 먼저 해제하고, 이후 stat 자체를 해제한다.
 * stat == NULL이면 아무 작업도 하지 않는다.
 *
 * 호출 체인:
 *   bdev_channel_destroy_resource() → [이 함수]
 *   bdev_free_stat_recursive() → [이 함수]
 */
void
bdev_free_io_stat(struct spdk_bdev_io_stat *stat)
{
	if (stat != NULL) {
		free(stat->io_error); /* [한국어] io_error 서브구조체 해제 (NULL이면 no-op) */
		free(stat);           /* [한국어] 통계 구조체 자체 해제 */
	}
}

/*
 * [한국어]
 * spdk_bdev_dump_io_stat_json - I/O 통계를 JSON으로 직렬화
 *
 * @stat: 직렬화할 통계 구조체
 * @w:    JSON 직렬화 컨텍스트
 *
 * bytes_read, num_read_ops 등 모든 통계 필드를 JSON named uint64로 출력한다.
 * 최소 레이턴시가 UINT64_MAX(초기값)이면 0으로 출력 — "아직 I/O 없음"을 의미.
 * io_error가 있으면 "io_error" 오브젝트 내에 에러 상태별 카운터를 출력한다.
 * RPC "bdev_get_device_stat" 응답 생성에 사용.
 *
 * 호출 체인:
 *   bdev_get_device_stat_done() / RPC 핸들러 → [이 함수]
 */
void
spdk_bdev_dump_io_stat_json(struct spdk_bdev_io_stat *stat, struct spdk_json_write_ctx *w)
{
	int i;

	/* [한국어] 기본 카운터 및 레이턴시 통계를 JSON named 필드로 출력 */
	spdk_json_write_named_uint64(w, "bytes_read", stat->bytes_read);
	spdk_json_write_named_uint64(w, "num_read_ops", stat->num_read_ops);
	spdk_json_write_named_uint64(w, "bytes_written", stat->bytes_written);
	spdk_json_write_named_uint64(w, "num_write_ops", stat->num_write_ops);
	spdk_json_write_named_uint64(w, "bytes_unmapped", stat->bytes_unmapped);
	spdk_json_write_named_uint64(w, "num_unmap_ops", stat->num_unmap_ops);
	spdk_json_write_named_uint64(w, "bytes_copied", stat->bytes_copied);
	spdk_json_write_named_uint64(w, "num_copy_ops", stat->num_copy_ops);
	spdk_json_write_named_uint64(w, "read_latency_ticks", stat->read_latency_ticks);
	spdk_json_write_named_uint64(w, "max_read_latency_ticks", stat->max_read_latency_ticks);
	spdk_json_write_named_uint64(w, "min_read_latency_ticks",
				     stat->min_read_latency_ticks != UINT64_MAX ?
				     stat->min_read_latency_ticks : 0);
	/* [한국어] min_read_latency_ticks가 UINT64_MAX(초기값)이면 0으로 출력 — I/O 없음 표시 */
	spdk_json_write_named_uint64(w, "write_latency_ticks", stat->write_latency_ticks);
	spdk_json_write_named_uint64(w, "max_write_latency_ticks", stat->max_write_latency_ticks);
	spdk_json_write_named_uint64(w, "min_write_latency_ticks",
				     stat->min_write_latency_ticks != UINT64_MAX ?
				     stat->min_write_latency_ticks : 0);
	spdk_json_write_named_uint64(w, "unmap_latency_ticks", stat->unmap_latency_ticks);
	spdk_json_write_named_uint64(w, "max_unmap_latency_ticks", stat->max_unmap_latency_ticks);
	spdk_json_write_named_uint64(w, "min_unmap_latency_ticks",
				     stat->min_unmap_latency_ticks != UINT64_MAX ?
				     stat->min_unmap_latency_ticks : 0);
	spdk_json_write_named_uint64(w, "copy_latency_ticks", stat->copy_latency_ticks);
	spdk_json_write_named_uint64(w, "max_copy_latency_ticks", stat->max_copy_latency_ticks);
	spdk_json_write_named_uint64(w, "min_copy_latency_ticks",
				     stat->min_copy_latency_ticks != UINT64_MAX ?
				     stat->min_copy_latency_ticks : 0);

	if (stat->io_error != NULL) {
		/* [한국어] io_error 통계가 있으면 "io_error" JSON 오브젝트로 출력 */
		spdk_json_write_named_object_begin(w, "io_error");
		for (i = 0; i < -SPDK_MIN_BDEV_IO_STATUS; i++) {
			/* [한국어] 각 에러 상태 인덱스를 음수 에러 코드로 변환해 카운터 출력 */
			if (stat->io_error->error_status[i] != 0) {
				/* [한국어] 카운터가 0인 에러는 출력 생략 (빈 필드 최소화) */
				spdk_json_write_named_uint32(w, bdev_io_status_get_string(-(i + 1)),
							     stat->io_error->error_status[i]);
			}
		}
		spdk_json_write_object_end(w); /* [한국어] "io_error" 오브젝트 닫기 */
	}
}

/*
 * [한국어]
 * bdev_channel_abort_queued_ios - 채널 파괴 전 모든 큐된 I/O를 ABORTED로 완료
 *
 * @ch: 정리할 bdev 채널 포인터
 *
 * 채널 파괴(bdev_channel_destroy) 시 호출된다. 두 종류의 대기 큐를 정리:
 *   1. nomem_io 큐: bdev_abort_all_nomem_io()로 ch 소속 NOMEM 재시도 I/O 전부 ABORTED.
 *   2. iobuf 버퍼 대기 큐: bdev_abort_all_buf_io()로 ch 소속 버퍼 대기 I/O 전부 ABORTED.
 *
 * 호출 체인:
 *   bdev_channel_destroy() → [이 함수] → bdev_abort_all_nomem_io() + bdev_abort_all_buf_io()
 */
static void
bdev_channel_abort_queued_ios(struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_shared_resource *shared_resource = ch->shared_resource; /* [한국어] 공유 리소스 참조 */
	struct spdk_bdev_mgmt_channel *mgmt_ch = shared_resource->mgmt_ch; /* [한국어] iobuf 채널 보유 관리 채널 */

	bdev_abort_all_nomem_io(ch);      /* [한국어] NOMEM 재시도 큐의 ch 소속 I/O 전부 ABORTED */
	bdev_abort_all_buf_io(mgmt_ch, ch); /* [한국어] iobuf 버퍼 대기 큐의 ch 소속 I/O 전부 ABORTED */
}

/*
 * [한국어]
 * bdev_channel_destroy - bdev I/O 채널 파괴 콜백 (spdk_io_channel 프레임워크 호출)
 *
 * @io_device: bdev의 io_device 포인터 (사용되지 않음)
 * @ctx_buf:   spdk_bdev_channel 구조체 포인터
 *
 * spdk_put_io_channel()에 의해 참조 카운트가 0이 되면 채널 소유 스레드에서 호출된다.
 * 처리 순서:
 *   1. 채널 통계를 bdev->internal.stat에 병합 (채널 소멸로 인한 통계 손실 방지).
 *   2. 큐된 I/O를 모두 ABORTED로 완료 (nomem_io, iobuf 버퍼 대기).
 *   3. 히스토그램 데이터 해제 (있는 경우).
 *   4. 채널 리소스(stat, LBA 범위, 모듈 채널, shared_resource) 해제.
 *
 * 실행 컨텍스트: 채널이 속한 리액터 스레드.
 *
 * 호출 체인:
 *   spdk_put_io_channel() → [이 함수] → bdev_channel_abort_queued_ios()
 *   → bdev_channel_destroy_resource()
 */
static void
bdev_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_channel *ch = ctx_buf; /* [한국어] 파괴할 bdev 채널 */

	SPDK_DEBUGLOG(bdev, "Destroying channel %p for bdev %s on thread %p\n", ch, ch->bdev->name,
		      spdk_get_thread()); /* [한국어] 디버그 로그 */

	spdk_trace_record(TRACE_BDEV_IOCH_DESTROY, ch->bdev->internal.trace_id, 0, 0,
			  spdk_thread_get_id(spdk_io_channel_get_thread(ch->channel)));
	/* [한국어] SPDK trace: 채널 파괴 이벤트 기록 */

	/* This channel is going away, so add its statistics into the bdev so that they don't get lost. */
	/* [한국어] 채널 소멸 전 통계를 bdev 전역 stat에 병합 — 채널 통계 손실 방지 */
	spdk_spin_lock(&ch->bdev->internal.spinlock); /* [한국어] bdev stat 접근 보호 */
	spdk_bdev_add_io_stat(ch->bdev->internal.stat, ch->stat); /* [한국어] 채널 통계 병합 */
	spdk_spin_unlock(&ch->bdev->internal.spinlock);

	bdev_channel_abort_queued_ios(ch); /* [한국어] 큐된 I/O 모두 ABORTED 완료 처리 */

	if (ch->histogram) {
		spdk_histogram_data_free(ch->histogram); /* [한국어] 히스토그램 데이터 해제 */
	}

	bdev_channel_destroy_resource(ch); /* [한국어] 채널 리소스(stat, LBA 범위, 채널) 해제 */
}

/*
 * If the name already exists in the global bdev name tree, RB_INSERT() returns a pointer
 * to it. Hence we do not have to call bdev_get_by_name() when using this function.
 */
/*
 * [한국어]
 * bdev_name_add - 전역 bdev 이름 RB-트리에 이름을 삽입
 *
 * @bdev_name: 초기화할 spdk_bdev_name 구조체 포인터 (bdev->name 또는 별칭 항목)
 * @bdev:      이 이름이 가리킬 bdev 포인터
 * @name:      삽입할 이름 문자열 (strdup으로 복사됨)
 * @return:    성공 0, 이름 중복 -EEXIST, 메모리 부족 -ENOMEM
 *
 * g_bdev_mgr.bdev_names RB-트리에 이름을 원자적으로 삽입한다.
 * RB_INSERT()는 중복 발견 시 기존 항목 포인터를 반환(NULL이 아님)하므로
 * 별도의 bdev_get_by_name() 호출 없이 중복 검사가 가능하다.
 * g_bdev_mgr.spinlock으로 RB-트리 접근을 보호한다.
 *
 * 호출 체인:
 *   spdk_bdev_register() / spdk_bdev_alias_add() → [이 함수]
 *   → RB_INSERT(bdev_name_tree)
 */
static int
bdev_name_add(struct spdk_bdev_name *bdev_name, struct spdk_bdev *bdev, const char *name)
{
	struct spdk_bdev_name *tmp;

	bdev_name->name = strdup(name); /* [한국어] 이름 문자열 복사 (RB-트리가 독립 소유) */
	if (bdev_name->name == NULL) {
		SPDK_ERRLOG("Unable to allocate bdev name\n");
		return -ENOMEM; /* [한국어] 문자열 복사 실패 → 메모리 부족 반환 */
	}

	bdev_name->bdev = bdev; /* [한국어] 이름이 가리킬 bdev 포인터 설정 */

	spdk_spin_lock(&g_bdev_mgr.spinlock); /* [한국어] RB-트리 접근 보호 */
	tmp = RB_INSERT(bdev_name_tree, &g_bdev_mgr.bdev_names, bdev_name);
	/* [한국어] RB_INSERT: 성공 시 NULL, 중복 시 기존 항목 포인터 반환 */
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	if (tmp != NULL) {
		/* [한국어] 이름 충돌: 이미 동일 이름의 bdev 또는 별칭이 존재 */
		SPDK_ERRLOG("Bdev name %s already exists\n", name);
		free(bdev_name->name); /* [한국어] 복사한 이름 문자열 해제 */
		return -EEXIST;
	}

	return 0; /* [한국어] 이름 삽입 성공 */
}

/*
 * [한국어]
 * bdev_name_del_unsafe - spinlock 없이 RB-트리에서 이름 제거 (이미 spinlock 보유 시 사용)
 *
 * @bdev_name: 제거할 spdk_bdev_name 구조체 포인터
 *
 * 이미 g_bdev_mgr.spinlock을 보유한 상태에서 RB-트리 항목을 제거할 때 사용하는 내부 함수.
 * bdev_name_del()과 달리 spinlock을 획득하지 않아 double-lock을 방지한다.
 *
 * 호출 체인:
 *   bdev_name_del() → [이 함수]
 *   bdev_unregister_unsafe() → [이 함수]
 */
static void
bdev_name_del_unsafe(struct spdk_bdev_name *bdev_name)
{
	RB_REMOVE(bdev_name_tree, &g_bdev_mgr.bdev_names, bdev_name); /* [한국어] RB-트리에서 항목 제거 */
	free(bdev_name->name); /* [한국어] 이름 문자열 메모리 해제 */
}

/*
 * [한국어]
 * bdev_name_del - spinlock을 획득하고 RB-트리에서 이름 제거
 *
 * @bdev_name: 제거할 spdk_bdev_name 구조체 포인터
 *
 * spinlock을 획득하고 bdev_name_del_unsafe()를 호출하는 래퍼.
 * 외부에서 spinlock을 보유하지 않은 상태에서 이름을 제거할 때 사용한다.
 *
 * 호출 체인:
 *   spdk_bdev_alias_del() → [이 함수] → bdev_name_del_unsafe()
 */
static void
bdev_name_del(struct spdk_bdev_name *bdev_name)
{
	spdk_spin_lock(&g_bdev_mgr.spinlock); /* [한국어] RB-트리 접근 보호 */
	bdev_name_del_unsafe(bdev_name);      /* [한국어] 실제 제거 수행 */
	spdk_spin_unlock(&g_bdev_mgr.spinlock);
}

/*
 * [한국어]
 * spdk_bdev_alias_add - bdev에 별칭(alias) 이름을 추가
 *
 * @bdev:  별칭을 추가할 bdev 포인터
 * @alias: 추가할 별칭 문자열 (NULL이면 -EINVAL)
 * @return: 성공 0, 빈 별칭 -EINVAL, 메모리 부족 -ENOMEM, 이름 중복 -EEXIST
 *
 * spdk_bdev_alias 구조체를 할당하고 bdev_name_add()를 통해 전역 RB-트리에 등록한다.
 * 성공하면 bdev->aliases TAILQ 끝에 추가된다.
 *
 * 호출 체인:
 *   모듈 초기화 / RPC "bdev_nvme_set_hotplug" 등 → [이 함수]
 *   → bdev_name_add() → RB_INSERT(bdev_name_tree)
 */
int
spdk_bdev_alias_add(struct spdk_bdev *bdev, const char *alias)
{
	struct spdk_bdev_alias *tmp;
	int ret;

	if (alias == NULL) {
		SPDK_ERRLOG("Empty alias passed\n");
		return -EINVAL; /* [한국어] NULL 별칭은 허용하지 않음 */
	}

	tmp = calloc(1, sizeof(*tmp)); /* [한국어] 별칭 구조체 할당 (0으로 초기화) */
	if (tmp == NULL) {
		SPDK_ERRLOG("Unable to allocate alias\n");
		return -ENOMEM;
	}

	ret = bdev_name_add(&tmp->alias, bdev, alias); /* [한국어] RB-트리에 별칭 이름 삽입 */
	if (ret != 0) {
		free(tmp); /* [한국어] RB-트리 삽입 실패 시 구조체 해제 */
		return ret;
	}

	TAILQ_INSERT_TAIL(&bdev->aliases, tmp, tailq); /* [한국어] bdev 별칭 목록 끝에 추가 */

	return 0; /* [한국어] 별칭 추가 성공 */
}

/*
 * [한국어]
 * bdev_alias_del - 특정 별칭을 bdev에서 제거하는 내부 구현
 *
 * @bdev:         별칭을 제거할 bdev 포인터
 * @alias:        제거할 별칭 문자열
 * @alias_del_fn: RB-트리 제거 함수 포인터 (bdev_name_del 또는 bdev_name_del_unsafe)
 * @return:       성공 0, 별칭이 없으면 -ENOENT
 *
 * bdev->aliases 목록을 선형 탐색해 이름이 일치하는 별칭을 찾아 제거한다.
 * alias_del_fn 인자를 통해 spinlock 보유 여부에 따라 다른 삭제 함수를 사용할 수 있다.
 *
 * 호출 체인:
 *   spdk_bdev_alias_del() / bdev_alias_del_all() → [이 함수]
 *   → alias_del_fn() → RB_REMOVE(bdev_name_tree)
 */
static int
bdev_alias_del(struct spdk_bdev *bdev, const char *alias,
	       void (*alias_del_fn)(struct spdk_bdev_name *n))
{
	struct spdk_bdev_alias *tmp;

	TAILQ_FOREACH(tmp, &bdev->aliases, tailq) {
		/* [한국어] 별칭 목록을 선형 탐색해 일치하는 항목 찾기 */
		if (strcmp(alias, tmp->alias.name) == 0) {
			TAILQ_REMOVE(&bdev->aliases, tmp, tailq); /* [한국어] 별칭 목록에서 제거 */
			alias_del_fn(&tmp->alias);                 /* [한국어] RB-트리에서도 제거 */
			free(tmp);                                  /* [한국어] 별칭 구조체 해제 */
			return 0;
		}
	}

	return -ENOENT; /* [한국어] 일치하는 별칭을 찾지 못함 */
}

/*
 * [한국어]
 * spdk_bdev_alias_del - 특정 별칭을 bdev에서 제거 (공개 API)
 *
 * @bdev:  별칭을 제거할 bdev 포인터
 * @alias: 제거할 별칭 문자열
 * @return: 성공 0, 별칭 없으면 -ENOENT (INFO 로그 출력)
 *
 * 호출 체인:
 *   RPC "bdev_nvme_detach_controller" 등 → [이 함수] → bdev_alias_del()
 */
int
spdk_bdev_alias_del(struct spdk_bdev *bdev, const char *alias)
{
	int rc;

	rc = bdev_alias_del(bdev, alias, bdev_name_del); /* [한국어] 별칭 제거 시도 */
	if (rc == -ENOENT) {
		SPDK_INFOLOG(bdev, "Alias %s does not exist\n", alias); /* [한국어] 별칭 없음 → INFO 로그 */
	}

	return rc;
}

/*
 * [한국어]
 * bdev_alias_del_all - bdev의 모든 별칭을 제거하는 내부 구현
 *
 * @bdev:         모든 별칭을 제거할 bdev 포인터
 * @alias_del_fn: 각 별칭의 RB-트리 제거 함수 포인터
 *
 * TAILQ_FOREACH_SAFE로 안전하게 순회하며 모든 별칭을 제거한다.
 * bdev 해제 경로에서 bdev_unregister_unsafe()가 spinlock 보유 시 bdev_name_del_unsafe,
 * 일반 경로에서는 bdev_name_del을 인자로 전달한다.
 *
 * 호출 체인:
 *   spdk_bdev_alias_del_all() / bdev_unregister_unsafe() → [이 함수]
 */
static void
bdev_alias_del_all(struct spdk_bdev *bdev, void (*alias_del_fn)(struct spdk_bdev_name *n))
{
	struct spdk_bdev_alias *p, *tmp;

	/* [한국어] TAILQ_FOREACH_SAFE: 순회 중 항목 제거가 안전하도록 tmp로 다음 포인터 미리 저장 */
	TAILQ_FOREACH_SAFE(p, &bdev->aliases, tailq, tmp) {
		TAILQ_REMOVE(&bdev->aliases, p, tailq); /* [한국어] 별칭 목록에서 제거 */
		alias_del_fn(&p->alias);                 /* [한국어] RB-트리에서도 제거 */
		free(p);                                  /* [한국어] 별칭 구조체 해제 */
	}
}

/*
 * [한국어]
 * spdk_bdev_alias_del_all - bdev의 모든 별칭을 제거 (공개 API)
 *
 * @bdev: 모든 별칭을 제거할 bdev 포인터
 *
 * 호출 체인:
 *   모듈 해제 코드 → [이 함수] → bdev_alias_del_all(bdev, bdev_name_del)
 */
void
spdk_bdev_alias_del_all(struct spdk_bdev *bdev)
{
	bdev_alias_del_all(bdev, bdev_name_del); /* [한국어] spinlock 획득 방식으로 전체 별칭 제거 */
}

/*
 * [한국어]
 * spdk_bdev_get_io_channel - bdev에 대한 I/O 채널을 획득
 *
 * @desc: bdev 디스크립터 포인터
 * @return: 현재 스레드의 bdev I/O 채널 (참조 카운트 증가됨)
 *
 * spdk_get_io_channel()을 통해 현재 리액터 스레드에 바인딩된 채널을 반환.
 * 반환된 채널은 사용 후 spdk_put_io_channel()로 반환해야 한다.
 *
 * 호출 체인:
 *   사용자 초기화 코드 → [이 함수] → spdk_get_io_channel() → bdev_channel_create()
 */
struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	return spdk_get_io_channel(__bdev_to_io_dev(spdk_bdev_desc_get_bdev(desc)));
	/* [한국어] desc → bdev → io_dev 변환 후 현재 스레드의 I/O 채널 획득 */
}

/*
 * [한국어]
 * spdk_bdev_get_module_ctx - bdev 모듈의 내부 컨텍스트를 반환
 *
 * @desc: bdev 디스크립터
 * @return: 모듈 컨텍스트 포인터 (get_module_ctx 콜백 없으면 NULL)
 *
 * 모듈이 get_module_ctx 콜백을 제공하면 해당 포인터를 반환한다.
 * 주로 NVMe 모듈에서 내부 컨트롤러 포인터 접근에 사용된다.
 */
void *
spdk_bdev_get_module_ctx(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	void *ctx = NULL;

	if (bdev->fn_table->get_module_ctx) {
		/* [한국어] 모듈이 get_module_ctx 콜백을 등록한 경우 → 모듈 컨텍스트 반환 */
		ctx = bdev->fn_table->get_module_ctx(bdev->ctxt);
	}

	return ctx; /* [한국어] 콜백 없으면 NULL 반환 */
}

/* [한국어] 아래 함수들은 bdev 속성 조회 accessor — 각 필드를 직접 반환하는 얇은 래퍼 */

const char *
spdk_bdev_get_module_name(const struct spdk_bdev *bdev)
{
	return bdev->module->name; /* [한국어] 이 bdev를 등록한 모듈의 이름 반환 */
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return bdev->name; /* [한국어] bdev 이름 반환 (RB-트리 키로도 사용됨) */
}

const char *
spdk_bdev_get_product_name(const struct spdk_bdev *bdev)
{
	return bdev->product_name; /* [한국어] 제품 이름 반환 (모듈이 설정한 문자열) */
}

const struct spdk_bdev_aliases_list *
spdk_bdev_get_aliases(const struct spdk_bdev *bdev)
{
	return &bdev->aliases; /* [한국어] 별칭 목록 TAILQ 포인터 반환 */
}

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	return bdev->blocklen; /* [한국어] 블록 크기(바이트) 반환 — 메타데이터 포함 */
}

uint32_t
spdk_bdev_get_write_unit_size(const struct spdk_bdev *bdev)
{
	return bdev->write_unit_size; /* [한국어] 원자적 쓰기 단위(블록 수) 반환 */
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return bdev->blockcnt; /* [한국어] 전체 블록 수 반환 (디스크 크기 = blockcnt * blocklen) */
}

const char *
spdk_bdev_get_qos_rpc_type(enum spdk_bdev_qos_rate_limit_type type)
{
	return qos_rpc_type[type]; /* [한국어] QoS rate limit 타입을 RPC 문자열로 변환 */
}

/*
 * [한국어]
 * spdk_bdev_get_qos_rate_limits - 현재 설정된 QoS rate limit 값을 읽기
 *
 * @bdev:   조회할 bdev 포인터
 * @limits: QoS_NUM_RATE_LIMIT_TYPES 크기의 uint64 배열 (결과 저장)
 *
 * QoS가 설정된 경우 각 rate limit 타입의 현재 limit 값을 limits 배열에 기록한다.
 * IOPS 타입은 I/O 수, BPS 타입은 MB/s 단위로 반환 (사용자 가시 단위).
 * QoS가 없거나 limit이 UNDEFINED인 항목은 0으로 반환된다.
 */
void
spdk_bdev_get_qos_rate_limits(struct spdk_bdev *bdev, uint64_t *limits)
{
	int i;

	memset(limits, 0, sizeof(*limits) * SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES);
	/* [한국어] 출력 배열을 0으로 초기화 — QoS 없는 항목은 0 유지 */

	spdk_spin_lock(&bdev->internal.spinlock); /* [한국어] QoS 구조체 접근 보호 */
	if (bdev->internal.qos) {
		/* [한국어] QoS가 활성화된 경우 각 rate limit 타입의 현재 값 읽기 */
		for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
			if (bdev->internal.qos->rate_limits[i].limit !=
			    SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
				/* [한국어] 정의된 rate limit만 출력 */
				limits[i] = bdev->internal.qos->rate_limits[i].limit;
				if (bdev_qos_is_iops_rate_limit(i) == false) {
					/* Change from Byte to Megabyte which is user visible. */
					/* [한국어] BPS 타입: 내부 Byte → 사용자 가시 MB/s 단위로 변환 */
					limits[i] = limits[i] / 1024 / 1024;
				}
			}
		}
	}
	spdk_spin_unlock(&bdev->internal.spinlock);
}

/*
 * [한국어]
 * spdk_bdev_get_buf_align - bdev I/O 버퍼에 요구되는 정렬(alignment) 크기 반환
 *
 * @bdev: 조회할 bdev 포인터
 * @return: required_alignment의 2^N 값 (바이트 단위)
 *
 * bdev->required_alignment은 비트 시프트 수를 저장하므로 1 << 로 바이트 값 계산.
 * DMA 버퍼 할당 시 이 값 기준으로 정렬해야 한다.
 */
size_t
spdk_bdev_get_buf_align(const struct spdk_bdev *bdev)
{
	return 1 << bdev->required_alignment; /* [한국어] required_alignment 비트 → 정렬 바이트 변환 */
}

uint32_t
spdk_bdev_get_optimal_io_boundary(const struct spdk_bdev *bdev)
{
	return bdev->optimal_io_boundary; /* [한국어] I/O 경계 최적화 블록 수 반환 (0이면 제한 없음) */
}

bool
spdk_bdev_has_write_cache(const struct spdk_bdev *bdev)
{
	return bdev->write_cache; /* [한국어] 쓰기 캐시(write-back) 활성 여부 반환 */
}

const struct spdk_uuid *
spdk_bdev_get_uuid(const struct spdk_bdev *bdev)
{
	return &bdev->uuid; /* [한국어] bdev UUID 포인터 반환 */
}

uint16_t
spdk_bdev_get_acwu(const struct spdk_bdev *bdev)
{
	return bdev->acwu; /* [한국어] Atomic Compare and Write Unit (블록 수) 반환 */
}

uint32_t
spdk_bdev_get_md_size(const struct spdk_bdev *bdev)
{
	return bdev->md_len; /* [한국어] 메타데이터 크기(바이트) 반환 (0이면 메타데이터 없음) */
}

bool
spdk_bdev_is_md_interleaved(const struct spdk_bdev *bdev)
{
	return (bdev->md_len != 0) && bdev->md_interleave;
	/* [한국어] 메타데이터가 있고 인터리브 방식이면 true (데이터+MD가 같은 블록에 혼재) */
}

bool
spdk_bdev_is_md_separate(const struct spdk_bdev *bdev)
{
	return (bdev->md_len != 0) && !bdev->md_interleave;
	/* [한국어] 메타데이터가 있고 별도 버퍼 방식이면 true (데이터와 MD 버퍼 분리) */
}

bool
spdk_bdev_is_zoned(const struct spdk_bdev *bdev)
{
	return bdev->zoned; /* [한국어] ZNS(Zoned Namespace) bdev 여부 반환 */
}

uint32_t
spdk_bdev_get_data_block_size(const struct spdk_bdev *bdev)
{
	if (spdk_bdev_is_md_interleaved(bdev)) {
		/* [한국어] 인터리브 MD: 실제 데이터 크기는 blocklen - md_len */
		return bdev->blocklen - bdev->md_len;
	} else {
		/* [한국어] 별도 MD 또는 MD 없음: blocklen이 곧 데이터 블록 크기 */
		return bdev->blocklen;
	}
}

uint32_t
spdk_bdev_get_physical_block_size(const struct spdk_bdev *bdev)
{
	return bdev->phys_blocklen; /* [한국어] 물리 블록 크기(바이트) 반환 (논리 블록과 다를 수 있음) */
}

uint32_t
spdk_bdev_get_preferred_write_alignment(const struct spdk_bdev *bdev)
{
	return bdev->preferred_write_alignment; /* [한국어] 쓰기 I/O의 권장 정렬 크기(블록 수) 반환 */
}

uint32_t
spdk_bdev_get_preferred_write_granularity(const struct spdk_bdev *bdev)
{
	return bdev->preferred_write_granularity; /* [한국어] 쓰기 I/O의 권장 최소 단위(블록 수) 반환 */
}

uint32_t
spdk_bdev_get_optimal_write_size(const struct spdk_bdev *bdev)
{
	return bdev->optimal_write_size; /* [한국어] 최적 쓰기 크기(블록 수) 반환 */
}

uint32_t
spdk_bdev_get_preferred_unmap_alignment(const struct spdk_bdev *bdev)
{
	return bdev->preferred_unmap_alignment; /* [한국어] UNMAP I/O의 권장 정렬 크기(블록 수) 반환 */
}

uint32_t
spdk_bdev_get_preferred_unmap_granularity(const struct spdk_bdev *bdev)
{
	return bdev->preferred_unmap_granularity; /* [한국어] UNMAP I/O의 권장 최소 단위(블록 수) 반환 */
}

/*
 * [한국어]
 * _bdev_get_block_size_with_md - 메타데이터 포함 블록 크기 계산
 *
 * @bdev: 조회할 bdev 포인터
 * @return: 메타데이터가 포함된 실효 블록 크기(바이트)
 *
 * 별도 MD(separate): blocklen + md_len → 데이터와 MD를 별도로 전송하므로 합산.
 * 인터리브 MD 또는 MD 없음: blocklen만 반환 (MD가 이미 blocklen에 포함되어 있음).
 * bdev_get_max_write()가 최대 쓰기 블록 수 계산 시 사용한다.
 */
static uint32_t
_bdev_get_block_size_with_md(const struct spdk_bdev *bdev)
{
	if (!spdk_bdev_is_md_interleaved(bdev)) {
		/* [한국어] 별도 MD: MD를 blocklen에 추가해 실효 전송 크기 계산 */
		return bdev->blocklen + bdev->md_len;
	} else {
		/* [한국어] 인터리브 MD 또는 MD 없음: blocklen 자체가 실효 크기 */
		return bdev->blocklen;
	}
}

/* We have to use the typedef in the function declaration to appease astyle. */
/* [한국어] astyle 포맷터 요건상 함수 선언에 typedef를 사용해야 함 */
typedef enum spdk_dif_type spdk_dif_type_t;
typedef enum spdk_dif_pi_format spdk_dif_pi_format_t;

/*
 * [한국어]
 * spdk_bdev_get_dif_type - DIF(Data Integrity Field) 타입 반환
 *
 * @bdev: 조회할 bdev
 * @return: DIF 타입 (SPDK_DIF_TYPE1/TYPE2/TYPE3 또는 SPDK_DIF_DISABLE)
 *
 * 메타데이터(md_len)가 없으면 DIF를 지원할 수 없으므로 SPDK_DIF_DISABLE 반환.
 */
spdk_dif_type_t
spdk_bdev_get_dif_type(const struct spdk_bdev *bdev)
{
	if (bdev->md_len != 0) {
		return bdev->dif_type; /* [한국어] MD가 있으면 설정된 DIF 타입 반환 */
	} else {
		return SPDK_DIF_DISABLE; /* [한국어] MD 없음 → DIF 불가 */
	}
}

spdk_dif_pi_format_t
spdk_bdev_get_dif_pi_format(const struct spdk_bdev *bdev)
{
	return bdev->dif_pi_format; /* [한국어] DIF PI(Protection Information) 포맷 반환 */
}

bool
spdk_bdev_is_dif_head_of_md(const struct spdk_bdev *bdev)
{
	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		return bdev->dif_is_head_of_md; /* [한국어] DIF 활성 시 PI가 MD 앞쪽에 위치하는지 여부 */
	} else {
		return false; /* [한국어] DIF 비활성 → PI 위치 무의미 */
	}
}

/*
 * [한국어]
 * spdk_bdev_is_dif_check_enabled - 특정 DIF 체크 타입이 활성화되어 있는지 확인
 *
 * @bdev:       조회할 bdev
 * @check_type: 확인할 체크 타입 (REFTAG/APPTAG/GUARD)
 * @return:     해당 체크가 활성화되어 있으면 true
 *
 * dif_check_flags 비트마스크에서 해당 체크 플래그를 확인한다.
 * DIF가 비활성(DISABLE)이면 모든 체크에 대해 false 반환.
 */
bool
spdk_bdev_is_dif_check_enabled(const struct spdk_bdev *bdev,
			       enum spdk_dif_check_type check_type)
{
	if (spdk_bdev_get_dif_type(bdev) == SPDK_DIF_DISABLE) {
		return false; /* [한국어] DIF 비활성 → 체크 불가 */
	}

	switch (check_type) {
	case SPDK_DIF_CHECK_TYPE_REFTAG:
		return (bdev->dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK) != 0;
		/* [한국어] Reference Tag 체크 활성 여부 */
	case SPDK_DIF_CHECK_TYPE_APPTAG:
		return (bdev->dif_check_flags & SPDK_DIF_FLAGS_APPTAG_CHECK) != 0;
		/* [한국어] Application Tag 체크 활성 여부 */
	case SPDK_DIF_CHECK_TYPE_GUARD:
		return (bdev->dif_check_flags & SPDK_DIF_FLAGS_GUARD_CHECK) != 0;
		/* [한국어] Guard CRC 체크 활성 여부 */
	default:
		return false;
	}
}

/*
 * [한국어]
 * bdev_get_max_write - 주어진 바이트 수에 대한 최대 쓰기 가능 블록 수 계산
 *
 * @bdev:      조회할 bdev
 * @num_bytes: 최대 버퍼 크기(바이트)
 * @return:    write_unit_size에 정렬된 최대 블록 수
 *
 * 버퍼 정렬 오버헤드를 제거한 후 블록 단위로 나누고 write_unit_size에 정렬한다.
 * write_zeroes 에뮬레이션 시 SPDK_BDEV_WRITE_ZEROES_SIZE 같은 제한을 적용하는 데 사용.
 */
static uint32_t
bdev_get_max_write(const struct spdk_bdev *bdev, uint64_t num_bytes)
{
	uint64_t aligned_length, max_write_blocks;

	aligned_length = num_bytes - (spdk_bdev_get_buf_align(bdev) - 1);
	/* [한국어] 버퍼 정렬 오버헤드(최대 align-1 바이트)를 제거한 실효 길이 */
	max_write_blocks = aligned_length / _bdev_get_block_size_with_md(bdev);
	/* [한국어] 실효 길이를 메타데이터 포함 블록 크기로 나눠 블록 수 계산 */
	max_write_blocks -= max_write_blocks % bdev->write_unit_size;
	/* [한국어] write_unit_size 배수로 내림 정렬 */

	return max_write_blocks;
}

uint32_t
spdk_bdev_get_max_copy(const struct spdk_bdev *bdev)
{
	return bdev->max_copy; /* [한국어] COPY I/O의 최대 블록 수 반환 */
}

uint64_t
spdk_bdev_get_qd(const struct spdk_bdev *bdev)
{
	return bdev->internal.measured_queue_depth; /* [한국어] 측정된 현재 Queue Depth 반환 */
}

uint64_t
spdk_bdev_get_qd_sampling_period(const struct spdk_bdev *bdev)
{
	return bdev->internal.period; /* [한국어] QD 샘플링 주기(μs) 반환 */
}

uint64_t
spdk_bdev_get_weighted_io_time(const struct spdk_bdev *bdev)
{
	return bdev->internal.weighted_io_time; /* [한국어] 가중 I/O 시간 누적값 반환 (QD * 샘플링 주기 합) */
}

uint64_t
spdk_bdev_get_io_time(const struct spdk_bdev *bdev)
{
	return bdev->internal.io_time; /* [한국어] I/O가 발생한 총 시간 누적값 반환 */
}

union spdk_bdev_nvme_ctratt spdk_bdev_get_nvme_ctratt(struct spdk_bdev *bdev)
{
	return bdev->ctratt; /* [한국어] NVMe Controller Attributes(CTRATT) 반환 */
}

uint32_t
spdk_bdev_get_nvme_nsid(struct spdk_bdev *bdev)
{
	return bdev->nsid; /* [한국어] NVMe Namespace ID(NSID) 반환 */
}

/* [한국어] 아래 spdk_bdev_desc_* 함수들은 hide_metadata 옵션이 적용된 디스크립터 관점의 accessor.
 * hide_metadata == true이면 메타데이터가 없는 것처럼 보이도록 값을 조정한다. */

uint32_t
spdk_bdev_desc_get_block_size(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = desc->bdev;

	return desc->opts.hide_metadata ? bdev->blocklen - bdev->md_len : bdev->blocklen;
	/* [한국어] hide_metadata: blocklen에서 md_len을 뺀 순수 데이터 블록 크기 반환 */
}

uint32_t
spdk_bdev_desc_get_md_size(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = desc->bdev;

	return desc->opts.hide_metadata ? 0 : bdev->md_len;
	/* [한국어] hide_metadata: MD 크기를 0으로 반환 (MD가 없는 것처럼 보임) */
}

bool
spdk_bdev_desc_is_md_interleaved(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = desc->bdev;

	return desc->opts.hide_metadata ? false : spdk_bdev_is_md_interleaved(bdev);
	/* [한국어] hide_metadata: false 반환 (MD가 없는 것처럼) */
}

bool
spdk_bdev_desc_is_md_separate(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = desc->bdev;

	return desc->opts.hide_metadata ? false : spdk_bdev_is_md_separate(bdev);
}

spdk_dif_type_t
spdk_bdev_desc_get_dif_type(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = desc->bdev;

	return desc->opts.hide_metadata ? SPDK_DIF_DISABLE : spdk_bdev_get_dif_type(bdev);
	/* [한국어] hide_metadata: DIF를 DISABLE로 반환 (PI를 숨김) */
}

spdk_dif_pi_format_t
spdk_bdev_desc_get_dif_pi_format(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = desc->bdev;

	return desc->opts.hide_metadata ? SPDK_DIF_PI_FORMAT_16 : spdk_bdev_get_dif_pi_format(bdev);
	/* [한국어] hide_metadata: 기본 PI 포맷 반환 (실제 포맷 숨김) */
}

bool
spdk_bdev_desc_is_dif_head_of_md(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = desc->bdev;

	return desc->opts.hide_metadata ? false : spdk_bdev_is_dif_head_of_md(bdev);
}

bool
spdk_bdev_desc_is_dif_check_enabled(struct spdk_bdev_desc *desc,
				    enum spdk_dif_check_type check_type)
{
	struct spdk_bdev *bdev = desc->bdev;

	return desc->opts.hide_metadata ? false : spdk_bdev_is_dif_check_enabled(bdev, check_type);
}

static void bdev_update_qd_sampling_period(void *ctx); /* [한국어] 선방 선언 — 순환 호출 구조 */

/*
 * [한국어]
 * _calculate_measured_qd_cpl - QD 측정 채널 순회 완료 콜백
 *
 * @bdev:   측정 대상 bdev
 * @_ctx:   사용되지 않음
 * @status: 채널 순회 결과 (0 = 정상)
 *
 * 모든 채널의 io_outstanding 합산이 완료된 후 호출된다.
 * temporary_queue_depth를 measured_queue_depth로 확정하고,
 * QD가 0이 아닌 경우 io_time과 weighted_io_time을 업데이트한다.
 * qd_poll_in_progress 플래그를 해제하고 샘플링 주기 변경이 있으면 적용한다.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() 완료 → [이 함수] → bdev_update_qd_sampling_period()
 */
static void
_calculate_measured_qd_cpl(struct spdk_bdev *bdev, void *_ctx, int status)
{
	bdev->internal.measured_queue_depth = bdev->internal.temporary_queue_depth;
	/* [한국어] 모든 채널 합산이 완료된 임시 QD를 측정값으로 확정 */

	if (bdev->internal.measured_queue_depth) {
		/* [한국어] QD > 0: I/O 활성 기간 통계 업데이트 */
		bdev->internal.io_time += bdev->internal.period;
		/* [한국어] I/O가 발생한 샘플링 기간 누적 */
		bdev->internal.weighted_io_time += bdev->internal.period * bdev->internal.measured_queue_depth;
		/* [한국어] 가중 I/O 시간 누적 = 샘플링 주기 × QD (평균 QD 계산에 사용) */
	}

	bdev->internal.qd_poll_in_progress = false; /* [한국어] 폴링 완료 플래그 해제 */

	bdev_update_qd_sampling_period(bdev); /* [한국어] 샘플링 주기 변경 요청이 있으면 적용 */
}

/*
 * [한국어]
 * _calculate_measured_qd - 단일 채널의 io_outstanding을 임시 QD에 합산하는 채널 순회 콜백
 *
 * @i:     채널 순회 반복자
 * @bdev:  측정 대상 bdev
 * @io_ch: 현재 채널
 * @_ctx:  사용되지 않음
 *
 * 각 채널의 io_outstanding(채널별 미완료 I/O 수)을 bdev->internal.temporary_queue_depth에 누적.
 *
 * 호출 체인:
 *   bdev_calculate_measured_queue_depth() → spdk_bdev_for_each_channel() → [이 함수]
 */
static void
_calculate_measured_qd(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
		       struct spdk_io_channel *io_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(io_ch); /* [한국어] io_ch에서 bdev 채널 추출 */

	bdev->internal.temporary_queue_depth += ch->io_outstanding;
	/* [한국어] 이 채널의 미완료 I/O 수를 임시 QD에 누적 */
	spdk_bdev_for_each_channel_continue(i, 0); /* [한국어] 다음 채널 계속 처리 */
}

/*
 * [한국어]
 * bdev_calculate_measured_queue_depth - QD 측정 폴러 (주기적 호출)
 *
 * @ctx: bdev 포인터
 * @return: SPDK_POLLER_BUSY
 *
 * 모든 채널의 io_outstanding을 합산해 bdev->internal.measured_queue_depth를 업데이트한다.
 * qd_poll_in_progress 플래그로 중복 실행을 방지한다.
 *
 * 호출 체인:
 *   SPDK 리액터 폴러 루프 → [이 함수] → spdk_bdev_for_each_channel() →
 *   _calculate_measured_qd() → _calculate_measured_qd_cpl()
 */
static int
bdev_calculate_measured_queue_depth(void *ctx)
{
	struct spdk_bdev *bdev = ctx; /* [한국어] 측정 대상 bdev */

	bdev->internal.qd_poll_in_progress = true;  /* [한국어] 폴링 시작 플래그 설정 */
	bdev->internal.temporary_queue_depth = 0;   /* [한국어] 임시 QD 초기화 */
	spdk_bdev_for_each_channel(bdev, _calculate_measured_qd, bdev, _calculate_measured_qd_cpl);
	/* [한국어] 모든 채널에서 io_outstanding 합산 시작 */
	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * bdev_update_qd_sampling_period - QD 샘플링 주기 변경 요청 처리
 *
 * @ctx: bdev 포인터
 *
 * spdk_bdev_set_qd_sampling_period()에서 new_period를 설정하면, 폴링 완료 시
 * 이 함수가 실제 period를 업데이트하고 폴러를 재등록한다.
 * 폴링 중(qd_poll_in_progress)이면 완료 후 재호출된다 (_calculate_measured_qd_cpl에서).
 * period == 0이면 qd_poller를 해제하고 bdev descriptor도 닫는다.
 *
 * 호출 체인:
 *   _calculate_measured_qd_cpl() → [이 함수] → SPDK_POLLER_REGISTER / spdk_bdev_close
 */
static void
bdev_update_qd_sampling_period(void *ctx)
{
	struct spdk_bdev *bdev = ctx;

	if (bdev->internal.period == bdev->internal.new_period) {
		return; /* [한국어] 변경 요청 없음 → 즉시 반환 */
	}

	if (bdev->internal.qd_poll_in_progress) {
		return; /* [한국어] 폴링 중 → 완료 후 재호출 대기 */
	}

	bdev->internal.period = bdev->internal.new_period; /* [한국어] 새 샘플링 주기 적용 */

	spdk_poller_unregister(&bdev->internal.qd_poller); /* [한국어] 기존 QD 폴러 해제 */
	if (bdev->internal.period != 0) {
		/* [한국어] 새 주기로 QD 폴러 재등록 */
		bdev->internal.qd_poller = SPDK_POLLER_REGISTER(bdev_calculate_measured_queue_depth,
					   bdev, bdev->internal.period);
	} else {
		/* [한국어] period == 0: QD 측정 비활성화 — qd_desc 닫기 */
		spdk_bdev_close(bdev->internal.qd_desc);
		bdev->internal.qd_desc = NULL;
	}
}

static void
_tmp_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
	SPDK_NOTICELOG("Unexpected event type: %d\n", type);
}

void
spdk_bdev_set_qd_sampling_period(struct spdk_bdev *bdev, uint64_t period)
{
	int rc;

	if (bdev->internal.new_period == period) {
		return;
	}

	bdev->internal.new_period = period;

	if (bdev->internal.qd_desc != NULL) {
		assert(bdev->internal.period != 0);

		spdk_thread_send_msg(bdev->internal.qd_desc->thread,
				     bdev_update_qd_sampling_period, bdev);
		return;
	}

	assert(bdev->internal.period == 0);

	rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), false, _tmp_bdev_event_cb,
				NULL, &bdev->internal.qd_desc);
	if (rc != 0) {
		return;
	}

	bdev->internal.period = period;
	bdev->internal.qd_poller = SPDK_POLLER_REGISTER(bdev_calculate_measured_queue_depth,
				   bdev, period);
}

struct bdev_get_current_qd_ctx {
	uint64_t current_qd;
	spdk_bdev_get_current_qd_cb cb_fn;
	void *cb_arg;
};

static void
bdev_get_current_qd_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct bdev_get_current_qd_ctx *ctx = _ctx;

	ctx->cb_fn(bdev, ctx->current_qd, ctx->cb_arg, 0);

	free(ctx);
}

static void
bdev_get_current_qd(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
		    struct spdk_io_channel *io_ch, void *_ctx)
{
	struct bdev_get_current_qd_ctx *ctx = _ctx;
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(io_ch);

	ctx->current_qd += bdev_ch->io_outstanding;

	spdk_bdev_for_each_channel_continue(i, 0);
}

void
spdk_bdev_get_current_qd(struct spdk_bdev *bdev, spdk_bdev_get_current_qd_cb cb_fn,
			 void *cb_arg)
{
	struct bdev_get_current_qd_ctx *ctx;

	assert(cb_fn != NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(bdev, 0, cb_arg, -ENOMEM);
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_bdev_for_each_channel(bdev, bdev_get_current_qd, ctx, bdev_get_current_qd_done);
}

static void
_event_notify(struct spdk_bdev_desc *desc, enum spdk_bdev_event_type type)
{
	assert(desc->thread == spdk_get_thread());

	spdk_spin_lock(&desc->spinlock);
	desc->refs--;
	if (!desc->closed) {
		spdk_spin_unlock(&desc->spinlock);
		desc->callback.event_fn(type,
					desc->bdev,
					desc->callback.ctx);
		return;
	} else if (desc->refs == 0) {
		/* This descriptor was closed after this event_notify message was sent.
		 * spdk_bdev_close() could not free the descriptor since this message was
		 * in flight, so we free it now using bdev_desc_free().
		 */
		spdk_spin_unlock(&desc->spinlock);
		bdev_desc_free(desc);
		return;
	}
	spdk_spin_unlock(&desc->spinlock);
}

static void
event_notify(struct spdk_bdev_desc *desc, spdk_msg_fn event_notify_fn)
{
	spdk_spin_lock(&desc->spinlock);
	desc->refs++;
	spdk_thread_send_msg(desc->thread, event_notify_fn, desc);
	spdk_spin_unlock(&desc->spinlock);
}

static void
_resize_notify(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	_event_notify(desc, SPDK_BDEV_EVENT_RESIZE);
}

int
spdk_bdev_notify_blockcnt_change(struct spdk_bdev *bdev, uint64_t size)
{
	struct spdk_bdev_desc *desc;
	int ret;

	if (size == bdev->blockcnt) {
		return 0;
	}

	spdk_spin_lock(&bdev->internal.spinlock);

	/* bdev has open descriptors */
	if (!TAILQ_EMPTY(&bdev->internal.open_descs) &&
	    bdev->blockcnt > size) {
		ret = -EBUSY;
	} else {
		spdk_notify_send("bdev_resize", spdk_bdev_get_name(bdev));
		bdev->blockcnt = size;
		TAILQ_FOREACH(desc, &bdev->internal.open_descs, link) {
			event_notify(desc, _resize_notify);
		}
		ret = 0;
	}

	spdk_spin_unlock(&bdev->internal.spinlock);

	return ret;
}

/*
 * Convert I/O offset and length from bytes to blocks.
 *
 * Returns zero on success or non-zero if the byte parameters aren't divisible by the block size.
 */
static uint64_t
bdev_bytes_to_blocks(struct spdk_bdev_desc *desc, uint64_t offset_bytes,
		     uint64_t *offset_blocks, uint64_t num_bytes, uint64_t *num_blocks)
{
	uint32_t block_size = bdev_desc_get_block_size(desc);
	uint8_t shift_cnt;

	/* Avoid expensive div operations if possible. These spdk_u32 functions are very cheap. */
	if (spdk_likely(spdk_u32_is_pow2(block_size))) {
		shift_cnt = spdk_u32log2(block_size);
		*offset_blocks = offset_bytes >> shift_cnt;
		*num_blocks = num_bytes >> shift_cnt;
		return (offset_bytes - (*offset_blocks << shift_cnt)) |
		       (num_bytes - (*num_blocks << shift_cnt));
	} else {
		*offset_blocks = offset_bytes / block_size;
		*num_blocks = num_bytes / block_size;
		return (offset_bytes % block_size) | (num_bytes % block_size);
	}
}

static bool
bdev_io_valid_blocks(struct spdk_bdev *bdev, uint64_t offset_blocks, uint64_t num_blocks)
{
	/* Return failure if offset_blocks + num_blocks is less than offset_blocks; indicates there
	 * has been an overflow and hence the offset has been wrapped around */
	if (offset_blocks + num_blocks < offset_blocks) {
		return false;
	}

	/* Return failure if offset_blocks + num_blocks exceeds the size of the bdev */
	if (offset_blocks + num_blocks > bdev->blockcnt) {
		return false;
	}

	return true;
}

/*
 * [한국어]
 * bdev_seek_complete_cb - bdev 에뮬레이션된 seek 완료를 비동기로 처리하는 메시지 콜백
 *
 * @ctx: bdev_io 포인터
 *
 * bdev가 SEEK_DATA/SEEK_HOLE을 지원하지 않을 때 즉시 에뮬레이션 결과를 설정한 후
 * spdk_thread_send_msg()로 비동기 완료를 발행한다.
 * 스택 오버플로우 방지를 위해 완료를 동일 스레드 메시지로 발행.
 *
 * 호출 체인:
 *   spdk_thread_send_msg() → [이 함수] → bdev_io->internal.cb()
 */
static void
bdev_seek_complete_cb(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;

	bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS; /* [한국어] 에뮬레이션 완료 → SUCCESS */
	bdev_io->internal.cb(bdev_io, true, bdev_io->internal.caller_ctx); /* [한국어] 완료 콜백 호출 */
}

/*
 * [한국어]
 * bdev_seek - SEEK_DATA/SEEK_HOLE I/O를 초기화하고 제출하는 내부 함수
 *
 * @desc:          bdev 디스크립터
 * @ch:            I/O 채널
 * @offset_blocks: 탐색 시작 블록 오프셋
 * @io_type:       SEEK_DATA 또는 SEEK_HOLE
 * @cb:            완료 콜백
 * @cb_arg:        완료 콜백 인자
 * @return:        성공 0, 범위 초과 -EINVAL, 메모리 부족 -ENOMEM
 *
 * bdev가 SEEK_DATA/SEEK_HOLE을 지원하면 모듈에 직접 제출한다.
 * 지원하지 않으면 에뮬레이션: SEEK_DATA → offset_blocks 그대로, SEEK_HOLE → UINT64_MAX.
 * 에뮬레이션 완료는 동일 스레드 메시지로 비동기 발행 (재진입 방지).
 *
 * 호출 체인:
 *   spdk_bdev_seek_data() / spdk_bdev_seek_hole() → [이 함수]
 *   → bdev_io_submit() 또는 spdk_thread_send_msg(bdev_seek_complete_cb)
 */
static int
bdev_seek(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	  uint64_t offset_blocks, enum spdk_bdev_io_type io_type,
	  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	assert(io_type == SPDK_BDEV_IO_TYPE_SEEK_DATA || io_type == SPDK_BDEV_IO_TYPE_SEEK_HOLE);
	/* [한국어] SEEK 타입만 허용 — 다른 타입이면 프로그래밍 오류 */

	/* Check if offset_blocks is valid looking at the validity of one block */
	if (!bdev_io_valid_blocks(bdev, offset_blocks, 1)) {
		return -EINVAL; /* [한국어] 오프셋이 bdev 범위 초과 */
	}

	bdev_io = bdev_channel_get_io(channel); /* [한국어] 메모리 풀에서 bdev_io 할당 */
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] 메모리 풀 고갈 */
	}

	bdev_io->internal.ch = channel;           /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;            /* [한국어] 디스크립터 설정 */
	bdev_io->type = io_type;                  /* [한국어] SEEK_DATA 또는 SEEK_HOLE */
	bdev_io->u.bdev.offset_blocks = offset_blocks; /* [한국어] 탐색 시작 블록 */
	bdev_io->u.bdev.memory_domain = NULL;
	bdev_io->u.bdev.memory_domain_ctx = NULL;
	bdev_io->u.bdev.accel_sequence = NULL;
	bdev_io_init(bdev_io, bdev, cb_arg, cb); /* [한국어] submit_tsc, 완료 콜백 등 초기화 */

	if (!spdk_bdev_io_type_supported(bdev, io_type)) {
		/* In case bdev doesn't support seek to next data/hole offset,
		 * it is assumed that only data and no holes are present */
		/* [한국어] 모듈이 SEEK를 지원하지 않음 → 에뮬레이션 */
		if (io_type == SPDK_BDEV_IO_TYPE_SEEK_DATA) {
			bdev_io->u.bdev.seek.offset = offset_blocks;
			/* [한국어] SEEK_DATA 에뮬레이션: 데이터만 존재하므로 요청 오프셋이 곧 결과 */
		} else {
			bdev_io->u.bdev.seek.offset = UINT64_MAX;
			/* [한국어] SEEK_HOLE 에뮬레이션: hole 없음을 UINT64_MAX로 표시 */
		}

		spdk_thread_send_msg(spdk_get_thread(), bdev_seek_complete_cb, bdev_io);
		/* [한국어] 에뮬레이션 완료를 현재 스레드 메시지로 비동기 발행 */
		return 0;
	}

	bdev_io_submit(bdev_io); /* [한국어] 모듈에 SEEK I/O 제출 */
	return 0;
}

/*
 * [한국어]
 * spdk_bdev_seek_data - 다음 데이터 영역을 탐색하는 공개 API
 *
 * @desc:          bdev 디스크립터
 * @ch:            I/O 채널
 * @offset_blocks: 탐색 시작 블록
 * @cb:            완료 콜백
 * @cb_arg:        완료 콜백 인자
 * @return:        성공 0, 오류 시 음수
 */
int
spdk_bdev_seek_data(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    uint64_t offset_blocks,
		    spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_seek(desc, ch, offset_blocks, SPDK_BDEV_IO_TYPE_SEEK_DATA, cb, cb_arg);
	/* [한국어] SEEK_DATA 타입으로 bdev_seek 호출 */
}

/*
 * [한국어]
 * spdk_bdev_seek_hole - 다음 hole 영역을 탐색하는 공개 API
 *
 * @desc:          bdev 디스크립터
 * @ch:            I/O 채널
 * @offset_blocks: 탐색 시작 블록
 * @cb:            완료 콜백
 * @cb_arg:        완료 콜백 인자
 * @return:        성공 0, 오류 시 음수
 */
int
spdk_bdev_seek_hole(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    uint64_t offset_blocks,
		    spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_seek(desc, ch, offset_blocks, SPDK_BDEV_IO_TYPE_SEEK_HOLE, cb, cb_arg);
	/* [한국어] SEEK_HOLE 타입으로 bdev_seek 호출 */
}

uint64_t
spdk_bdev_io_get_seek_offset(const struct spdk_bdev_io *bdev_io)
{
	return bdev_io->u.bdev.seek.offset;
	/* [한국어] SEEK 완료 후 결과 오프셋 반환 (hole 없으면 UINT64_MAX) */
}

/*
 * [한국어]
 * bdev_read_blocks_with_md - 단일 버퍼(flat buf) 읽기 I/O를 초기화하고 제출하는 내부 함수
 *
 * @desc:          bdev 디스크립터
 * @ch:            I/O 채널
 * @buf:           데이터를 읽어올 버퍼 포인터
 * @md_buf:        메타데이터 버퍼 포인터 (NULL이면 메타데이터 없음)
 * @offset_blocks: 읽기 시작 블록 오프셋
 * @num_blocks:    읽을 블록 수
 * @cb:            완료 콜백
 * @cb_arg:        완료 콜백 인자
 * @return:        성공 0, 범위 초과 -EINVAL, 메모리 부족 -ENOMEM
 *
 * bdev_io를 할당하고 READ 타입으로 초기화한 뒤 bdev_io_submit()으로 제출한다.
 * 단일 iovec(bdev_io->iov에 내장)을 사용하는 단순 읽기 경로.
 * scatter-gather가 필요하면 bdev_readv_blocks_with_md()를 사용해야 한다.
 *
 * 호출 체인:
 *   spdk_bdev_read_blocks() / spdk_bdev_read() → [이 함수] → bdev_io_submit()
 */
static int
bdev_read_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, void *buf,
			 void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
			 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL; /* [한국어] 블록 범위 초과 — overflow 또는 bdev 크기 초과 */
	}

	bdev_io = bdev_channel_get_io(channel); /* [한국어] 메모리 풀에서 bdev_io 할당 */
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] 메모리 풀 고갈 */
	}

	bdev_io->internal.ch = channel;          /* [한국어] 채널 연결 */
	bdev_io->internal.desc = desc;           /* [한국어] 디스크립터 연결 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_READ;  /* [한국어] READ I/O 타입 설정 */
	bdev_io->u.bdev.iovs = &bdev_io->iov;   /* [한국어] 내장 iov 사용 (단일 버퍼) */
	bdev_io->u.bdev.iovs[0].iov_base = buf; /* [한국어] 데이터 수신 버퍼 */
	bdev_io->u.bdev.iovs[0].iov_len = num_blocks * bdev_desc_get_block_size(desc);
	/* [한국어] 버퍼 길이 = 블록 수 × 블록 크기 (hide_metadata 적용) */
	bdev_io->u.bdev.iovcnt = 1;              /* [한국어] 단일 iovec */
	bdev_io->u.bdev.md_buf = md_buf;         /* [한국어] 메타데이터 버퍼 (NULL이면 없음) */
	bdev_io->u.bdev.num_blocks = num_blocks; /* [한국어] 읽을 블록 수 */
	bdev_io->u.bdev.offset_blocks = offset_blocks; /* [한국어] 읽기 시작 블록 오프셋 */
	bdev_io->u.bdev.memory_domain = NULL;    /* [한국어] 메모리 도메인 없음 */
	bdev_io->u.bdev.memory_domain_ctx = NULL;
	bdev_io->u.bdev.accel_sequence = NULL;   /* [한국어] accel 시퀀스 없음 */
	bdev_io->u.bdev.dif_check_flags = bdev->dif_check_flags; /* [한국어] DIF 체크 플래그 설정 */
	bdev_io_init(bdev_io, bdev, cb_arg, cb); /* [한국어] submit_tsc, 완료 콜백 등 초기화 */

	bdev_io_submit(bdev_io); /* [한국어] 큐잉 또는 모듈에 직접 제출 */
	return 0;
}

/*
 * [한국어]
 * spdk_bdev_read - 바이트 오프셋/길이로 단일 버퍼 읽기 (공개 API)
 *
 * @desc:   bdev 디스크립터
 * @ch:     I/O 채널
 * @buf:    데이터 수신 버퍼
 * @offset: 읽기 시작 바이트 오프셋 (블록 크기 배수여야 함)
 * @nbytes: 읽을 바이트 수 (블록 크기 배수여야 함)
 * @cb:     완료 콜백
 * @cb_arg: 완료 콜백 인자
 * @return: 성공 0, 정렬 오류 -EINVAL
 *
 * 바이트 단위 파라미터를 블록 단위로 변환 후 spdk_bdev_read_blocks()를 호출한다.
 *
 * 호출 체인:
 *   사용자 코드 → [이 함수] → spdk_bdev_read_blocks() → bdev_read_blocks_with_md()
 */
int
spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	       void *buf, uint64_t offset, uint64_t nbytes,
	       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(desc, offset, &offset_blocks, nbytes, &num_blocks) != 0) {
		return -EINVAL; /* [한국어] 바이트 값이 블록 크기 배수가 아님 */
	}

	return spdk_bdev_read_blocks(desc, ch, buf, offset_blocks, num_blocks, cb, cb_arg);
}

/*
 * [한국어]
 * spdk_bdev_read_blocks - 블록 오프셋/수로 단일 버퍼 읽기 (공개 API, 메타데이터 없음)
 *
 * @desc:          bdev 디스크립터
 * @ch:            I/O 채널
 * @buf:           데이터 수신 버퍼
 * @offset_blocks: 읽기 시작 블록 오프셋
 * @num_blocks:    읽을 블록 수
 * @cb:            완료 콜백
 * @cb_arg:        완료 콜백 인자
 * @return:        성공 0, 오류 시 음수
 *
 * 호출 체인:
 *   사용자 코드 → [이 함수] → bdev_read_blocks_with_md(md_buf=NULL)
 */
int
spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_read_blocks_with_md(desc, ch, buf, NULL, offset_blocks, num_blocks, cb, cb_arg);
	/* [한국어] md_buf=NULL → 메타데이터 없이 읽기 */
}

/*
 * [한국어]
 * spdk_bdev_read_blocks_with_md - 메타데이터 포함 단일 버퍼 읽기 (공개 API)
 *
 * @desc:          bdev 디스크립터
 * @ch:            I/O 채널
 * @buf:           데이터 수신 버퍼
 * @md_buf:        메타데이터 수신 버퍼 (NULL 허용)
 * @offset_blocks: 읽기 시작 블록 오프셋
 * @num_blocks:    읽을 블록 수
 * @cb:            완료 콜백
 * @cb_arg:        완료 콜백 인자
 * @return:        성공 0, 오류 시 음수
 *
 * md_buf 사용 시 bdev가 separate MD 방식이어야 하고, buf가 이미 할당된 상태여야 한다.
 *
 * 호출 체인:
 *   사용자 코드 → [이 함수] → bdev_read_blocks_with_md()
 */
int
spdk_bdev_read_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      void *buf, void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct iovec iov = {
		.iov_base = buf,
	};

	if (md_buf && !spdk_bdev_is_md_separate(spdk_bdev_desc_get_bdev(desc))) {
		return -EINVAL; /* [한국어] 별도 MD 미지원 bdev에서 md_buf 사용 시도 */
	}

	if ((md_buf || desc->opts.hide_metadata) && !_is_buf_allocated(&iov)) {
		return -EINVAL; /* [한국어] MD 또는 hide_metadata 사용 시 buf가 NULL이면 오류 */
	}

	return bdev_read_blocks_with_md(desc, ch, buf, md_buf, offset_blocks, num_blocks,
					cb, cb_arg);
}

int
spdk_bdev_readv(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt,
		uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(desc, offset, &offset_blocks, nbytes, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_readv_blocks(desc, ch, iov, iovcnt, offset_blocks, num_blocks, cb, cb_arg);
}

static int
bdev_readv_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  struct iovec *iov, int iovcnt, void *md_buf, uint64_t offset_blocks,
			  uint64_t num_blocks, struct spdk_memory_domain *domain, void *domain_ctx,
			  struct spdk_accel_sequence *seq, uint32_t dif_check_flags,
			  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (spdk_unlikely(!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks))) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (spdk_unlikely(!bdev_io)) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	bdev_io->u.bdev.iovs = iov;
	bdev_io->u.bdev.iovcnt = iovcnt;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	if (seq != NULL) {
		bdev_io->internal.f.has_accel_sequence = true;
		bdev_io->internal.accel_sequence = seq;
	}

	if (domain != NULL) {
		bdev_io->internal.f.has_memory_domain = true;
		bdev_io->internal.memory_domain = domain;
		bdev_io->internal.memory_domain_ctx = domain_ctx;
	}

	bdev_io->u.bdev.memory_domain = domain;
	bdev_io->u.bdev.memory_domain_ctx = domain_ctx;
	bdev_io->u.bdev.accel_sequence = seq;
	bdev_io->u.bdev.dif_check_flags = dif_check_flags;

	_bdev_io_submit_ext(desc, bdev_io);

	return 0;
}

int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iov, int iovcnt,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);

	return bdev_readv_blocks_with_md(desc, ch, iov, iovcnt, NULL, offset_blocks,
					 num_blocks, NULL, NULL, NULL, bdev->dif_check_flags, cb, cb_arg);
}

int
spdk_bdev_readv_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			       struct iovec *iov, int iovcnt, void *md_buf,
			       uint64_t offset_blocks, uint64_t num_blocks,
			       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);

	if (md_buf && !spdk_bdev_is_md_separate(bdev)) {
		return -EINVAL;
	}

	if (md_buf && !_is_buf_allocated(iov)) {
		return -EINVAL;
	}

	return bdev_readv_blocks_with_md(desc, ch, iov, iovcnt, md_buf, offset_blocks,
					 num_blocks, NULL, NULL, NULL, bdev->dif_check_flags, cb, cb_arg);
}

static inline bool
_bdev_io_check_opts(struct spdk_bdev_ext_io_opts *opts, struct iovec *iov)
{
	/*
	 * We check if opts size is at least of size when we first introduced
	 * spdk_bdev_ext_io_opts (ac6f2bdd8d) since access to those members
	 * are not checked internal.
	 */
	return opts->size >= offsetof(struct spdk_bdev_ext_io_opts, metadata) +
	       sizeof(opts->metadata) &&
	       opts->size <= sizeof(*opts) &&
	       /* When memory domain is used, the user must provide data buffers */
	       (!opts->memory_domain || (iov && iov[0].iov_base));
}

int
spdk_bdev_readv_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg,
			   struct spdk_bdev_ext_io_opts *opts)
{
	struct spdk_memory_domain *domain = NULL;
	struct spdk_accel_sequence *seq = NULL;
	void *domain_ctx = NULL, *md = NULL;
	uint32_t dif_check_flags = 0;
	uint32_t nvme_cdw12_raw;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);

	if (opts) {
		if (spdk_unlikely(!_bdev_io_check_opts(opts, iov))) {
			return -EINVAL;
		}

		md = opts->metadata;
		domain = bdev_get_ext_io_opt(opts, memory_domain, NULL);
		domain_ctx = bdev_get_ext_io_opt(opts, memory_domain_ctx, NULL);
		seq = bdev_get_ext_io_opt(opts, accel_sequence, NULL);
		nvme_cdw12_raw = bdev_get_ext_io_opt(opts, nvme_cdw12.raw, 0);
		if (md) {
			if (spdk_unlikely(!spdk_bdev_is_md_separate(bdev))) {
				return -EINVAL;
			}

			if (spdk_unlikely(!_is_buf_allocated(iov))) {
				return -EINVAL;
			}

			if (spdk_unlikely(seq != NULL)) {
				return -EINVAL;
			}

			if (nvme_cdw12_raw & SPDK_DIF_FLAGS_NVME_PRACT) {
				SPDK_ERRLOG("Separate metadata with NVMe PRACT is not supported.\n");
				return -ENOTSUP;
			}
		}

		if (nvme_cdw12_raw & SPDK_DIF_FLAGS_NVME_PRACT) {
			dif_check_flags |= SPDK_DIF_FLAGS_NVME_PRACT;
		}
	}

	dif_check_flags |= bdev->dif_check_flags &
			   ~(bdev_get_ext_io_opt(opts, dif_check_flags_exclude_mask, 0));

	return bdev_readv_blocks_with_md(desc, ch, iov, iovcnt, md, offset_blocks,
					 num_blocks, domain, domain_ctx, seq, dif_check_flags, cb, cb_arg);
}

/*
 * [한국어]
 * bdev_write_blocks_with_md - 단일 버퍼(flat buf) 쓰기 I/O를 초기화하고 제출하는 내부 함수
 *
 * @desc:          bdev 디스크립터 (write == true 여야 함)
 * @ch:            I/O 채널
 * @buf:           쓸 데이터 버퍼 포인터
 * @md_buf:        메타데이터 버퍼 포인터 (NULL이면 없음)
 * @offset_blocks: 쓰기 시작 블록 오프셋
 * @num_blocks:    쓸 블록 수
 * @cb:            완료 콜백
 * @cb_arg:        완료 콜백 인자
 * @return:        성공 0, 읽기 전용 디스크립터 -EBADF, 범위 초과 -EINVAL, 메모리 부족 -ENOMEM
 *
 * bdev_io를 할당하고 WRITE 타입으로 초기화한 뒤 bdev_io_submit()으로 제출한다.
 * bdev_read_blocks_with_md()의 쓰기 버전으로 구조가 유사하다.
 *
 * 호출 체인:
 *   spdk_bdev_write_blocks() / spdk_bdev_write() → [이 함수] → bdev_io_submit()
 */
static int
bdev_write_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  void *buf, void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
			  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF; /* [한국어] 읽기 전용 디스크립터로 쓰기 시도 */
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL; /* [한국어] 블록 범위 초과 */
	}

	bdev_io = bdev_channel_get_io(channel); /* [한국어] 메모리 풀에서 bdev_io 할당 */
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;           /* [한국어] 채널 연결 */
	bdev_io->internal.desc = desc;            /* [한국어] 디스크립터 연결 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;  /* [한국어] WRITE I/O 타입 설정 */
	bdev_io->u.bdev.iovs = &bdev_io->iov;    /* [한국어] 내장 iov 사용 (단일 버퍼) */
	bdev_io->u.bdev.iovs[0].iov_base = buf;  /* [한국어] 쓸 데이터 버퍼 */
	bdev_io->u.bdev.iovs[0].iov_len = num_blocks * bdev_desc_get_block_size(desc);
	/* [한국어] 버퍼 길이 = 블록 수 × 블록 크기 */
	bdev_io->u.bdev.iovcnt = 1;               /* [한국어] 단일 iovec */
	bdev_io->u.bdev.md_buf = md_buf;          /* [한국어] 메타데이터 버퍼 */
	bdev_io->u.bdev.num_blocks = num_blocks;  /* [한국어] 쓸 블록 수 */
	bdev_io->u.bdev.offset_blocks = offset_blocks; /* [한국어] 쓰기 시작 블록 오프셋 */
	bdev_io->u.bdev.memory_domain = NULL;
	bdev_io->u.bdev.memory_domain_ctx = NULL;
	bdev_io->u.bdev.accel_sequence = NULL;
	bdev_io->u.bdev.dif_check_flags = bdev->dif_check_flags; /* [한국어] DIF 체크 플래그 */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io); /* [한국어] 큐잉 또는 모듈에 직접 제출 */
	return 0;
}

/*
 * [한국어]
 * spdk_bdev_write - 바이트 오프셋/길이로 단일 버퍼 쓰기 (공개 API)
 *
 * @desc:   bdev 디스크립터 (write flag 필요)
 * @ch:     I/O 채널
 * @buf:    쓸 데이터 버퍼
 * @offset: 쓰기 시작 바이트 오프셋 (블록 크기 배수여야 함)
 * @nbytes: 쓸 바이트 수 (블록 크기 배수여야 함)
 * @cb:     완료 콜백
 * @cb_arg: 완료 콜백 인자
 * @return: 성공 0, 오류 시 음수
 */
int
spdk_bdev_write(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(desc, offset, &offset_blocks, nbytes, &num_blocks) != 0) {
		return -EINVAL; /* [한국어] 바이트 값이 블록 크기 배수가 아님 */
	}

	return spdk_bdev_write_blocks(desc, ch, buf, offset_blocks, num_blocks, cb, cb_arg);
}

/*
 * [한국어]
 * spdk_bdev_write_blocks - 블록 오프셋/수로 단일 버퍼 쓰기 (공개 API, 메타데이터 없음)
 *
 * @desc:          bdev 디스크립터
 * @ch:            I/O 채널
 * @buf:           쓸 데이터 버퍼
 * @offset_blocks: 쓰기 시작 블록 오프셋
 * @num_blocks:    쓸 블록 수
 * @cb:            완료 콜백
 * @cb_arg:        완료 콜백 인자
 * @return:        성공 0, 오류 시 음수
 */
int
spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_write_blocks_with_md(desc, ch, buf, NULL, offset_blocks, num_blocks,
					 cb, cb_arg);
	/* [한국어] md_buf=NULL → 메타데이터 없이 쓰기 */
}

/*
 * [한국어]
 * spdk_bdev_write_blocks_with_md - 메타데이터 포함 단일 버퍼 쓰기 (공개 API)
 *
 * @desc:          bdev 디스크립터
 * @ch:            I/O 채널
 * @buf:           쓸 데이터 버퍼
 * @md_buf:        메타데이터 버퍼 (NULL 허용)
 * @offset_blocks: 쓰기 시작 블록 오프셋
 * @num_blocks:    쓸 블록 수
 * @cb:            완료 콜백
 * @cb_arg:        완료 콜백 인자
 * @return:        성공 0, 오류 시 음수
 *
 * md_buf 사용 시 bdev가 separate MD 방식이어야 하고, buf가 이미 할당된 상태여야 한다.
 */
int
spdk_bdev_write_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			       void *buf, void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
			       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct iovec iov = {
		.iov_base = buf,
	};

	if (md_buf && !spdk_bdev_is_md_separate(spdk_bdev_desc_get_bdev(desc))) {
		return -EINVAL; /* [한국어] 별도 MD 미지원 bdev에서 md_buf 사용 시도 */
	}

	if (md_buf && !_is_buf_allocated(&iov)) {
		return -EINVAL; /* [한국어] md_buf 사용 시 buf가 NULL이면 오류 */
	}

	return bdev_write_blocks_with_md(desc, ch, buf, md_buf, offset_blocks, num_blocks,
					 cb, cb_arg);
}

static int
bdev_writev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt, void *md_buf,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   struct spdk_memory_domain *domain, void *domain_ctx,
			   struct spdk_accel_sequence *seq, uint32_t dif_check_flags,
			   uint32_t nvme_cdw12_raw, uint32_t nvme_cdw13_raw,
			   spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (spdk_unlikely(!desc->write)) {
		return -EBADF;
	}

	if (spdk_unlikely(!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks))) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (spdk_unlikely(!bdev_io)) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	bdev_io->u.bdev.iovs = iov;
	bdev_io->u.bdev.iovcnt = iovcnt;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	if (seq != NULL) {
		bdev_io->internal.f.has_accel_sequence = true;
		bdev_io->internal.accel_sequence = seq;
	}

	if (domain != NULL) {
		bdev_io->internal.f.has_memory_domain = true;
		bdev_io->internal.memory_domain = domain;
		bdev_io->internal.memory_domain_ctx = domain_ctx;
	}

	bdev_io->u.bdev.memory_domain = domain;
	bdev_io->u.bdev.memory_domain_ctx = domain_ctx;
	bdev_io->u.bdev.accel_sequence = seq;
	bdev_io->u.bdev.dif_check_flags = dif_check_flags;
	bdev_io->u.bdev.nvme_cdw12.raw = nvme_cdw12_raw;
	bdev_io->u.bdev.nvme_cdw13.raw = nvme_cdw13_raw;

	_bdev_io_submit_ext(desc, bdev_io);

	return 0;
}

int
spdk_bdev_writev(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		 struct iovec *iov, int iovcnt,
		 uint64_t offset, uint64_t len,
		 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(desc, offset, &offset_blocks, len, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_writev_blocks(desc, ch, iov, iovcnt, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);

	return bdev_writev_blocks_with_md(desc, ch, iov, iovcnt, NULL, offset_blocks,
					  num_blocks, NULL, NULL, NULL, bdev->dif_check_flags, 0, 0,
					  cb, cb_arg);
}

int
spdk_bdev_writev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				struct iovec *iov, int iovcnt, void *md_buf,
				uint64_t offset_blocks, uint64_t num_blocks,
				spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);

	if (md_buf && !spdk_bdev_is_md_separate(bdev)) {
		return -EINVAL;
	}

	if (md_buf && !_is_buf_allocated(iov)) {
		return -EINVAL;
	}

	return bdev_writev_blocks_with_md(desc, ch, iov, iovcnt, md_buf, offset_blocks,
					  num_blocks, NULL, NULL, NULL, bdev->dif_check_flags, 0, 0,
					  cb, cb_arg);
}

int
spdk_bdev_writev_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    struct iovec *iov, int iovcnt,
			    uint64_t offset_blocks, uint64_t num_blocks,
			    spdk_bdev_io_completion_cb cb, void *cb_arg,
			    struct spdk_bdev_ext_io_opts *opts)
{
	struct spdk_memory_domain *domain = NULL;
	struct spdk_accel_sequence *seq = NULL;
	void *domain_ctx = NULL, *md = NULL;
	uint32_t dif_check_flags = 0;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	uint32_t nvme_cdw12_raw = 0;
	uint32_t nvme_cdw13_raw = 0;

	if (opts) {
		if (spdk_unlikely(!_bdev_io_check_opts(opts, iov))) {
			return -EINVAL;
		}
		md = opts->metadata;
		domain = bdev_get_ext_io_opt(opts, memory_domain, NULL);
		domain_ctx = bdev_get_ext_io_opt(opts, memory_domain_ctx, NULL);
		seq = bdev_get_ext_io_opt(opts, accel_sequence, NULL);
		nvme_cdw12_raw = bdev_get_ext_io_opt(opts, nvme_cdw12.raw, 0);
		nvme_cdw13_raw = bdev_get_ext_io_opt(opts, nvme_cdw13.raw, 0);
		if (md) {
			if (spdk_unlikely(!spdk_bdev_is_md_separate(bdev))) {
				return -EINVAL;
			}

			if (spdk_unlikely(!_is_buf_allocated(iov))) {
				return -EINVAL;
			}

			if (spdk_unlikely(seq != NULL)) {
				return -EINVAL;
			}

			if (nvme_cdw12_raw & SPDK_DIF_FLAGS_NVME_PRACT) {
				SPDK_ERRLOG("Separate metadata with NVMe PRACT is not supported.\n");
				return -ENOTSUP;
			}
		}

		if (nvme_cdw12_raw & SPDK_DIF_FLAGS_NVME_PRACT) {
			dif_check_flags |= SPDK_DIF_FLAGS_NVME_PRACT;
		}
	}

	dif_check_flags |= bdev->dif_check_flags &
			   ~(bdev_get_ext_io_opt(opts, dif_check_flags_exclude_mask, 0));

	return bdev_writev_blocks_with_md(desc, ch, iov, iovcnt, md, offset_blocks, num_blocks,
					  domain, domain_ctx, seq, dif_check_flags,
					  nvme_cdw12_raw, nvme_cdw13_raw, cb, cb_arg);
}

/*
 * [한국어]
 * bdev_compare_do_read_done - COMPARE 에뮬레이션의 읽기 단계 완료 콜백
 *
 * @bdev_io:  읽기 I/O (이 콜백이 완료 처리 후 해제됨)
 * @success:  읽기 성공 여부
 * @cb_arg:   parent_io 포인터 (원본 COMPARE I/O)
 *
 * COMPARE 에뮬레이션: 먼저 디스크를 읽고(bdev_compare_do_read), 읽은 데이터를
 * 사용자가 제공한 버퍼와 memcmp로 비교한다.
 * - 읽기 실패: FAILED 완료.
 * - 데이터 불일치: MISCOMPARE 완료.
 * - 일치: SUCCESS 완료.
 * 별도 MD가 있는 경우 MD 버퍼도 별도로 비교한다.
 *
 * 호출 체인:
 *   bdev_compare_do_read() → spdk_bdev_read_blocks() 완료 → [이 함수]
 *   → parent_io->internal.cb() (COMPARE 완료 콜백)
 */
static void
bdev_compare_do_read_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg; /* [한국어] 원본 COMPARE bdev_io */
	struct spdk_bdev *bdev = parent_io->bdev;
	uint8_t *read_buf = bdev_io->u.bdev.iovs[0].iov_base; /* [한국어] 읽어온 데이터 포인터 */
	int i, rc = 0;

	if (!success) {
		/* [한국어] 읽기 실패 → COMPARE도 FAILED 완료 */
		parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		parent_io->internal.cb(parent_io, false, parent_io->internal.caller_ctx);
		spdk_bdev_free_io(bdev_io); /* [한국어] 읽기 I/O 해제 */
		return;
	}

	/* [한국어] 읽은 데이터와 사용자 버퍼를 iovec 단위로 비교 */
	for (i = 0; i < parent_io->u.bdev.iovcnt; i++) {
		rc = memcmp(read_buf,
			    parent_io->u.bdev.iovs[i].iov_base,
			    parent_io->u.bdev.iovs[i].iov_len);
		if (rc) {
			break; /* [한국어] 불일치 발견 → 루프 탈출 */
		}
		read_buf += parent_io->u.bdev.iovs[i].iov_len; /* [한국어] 다음 iov로 이동 */
	}

	if (rc == 0 && parent_io->u.bdev.md_buf && spdk_bdev_is_md_separate(bdev)) {
		/* [한국어] 데이터 일치하고 별도 MD 있는 경우 → MD도 비교 */
		rc = memcmp(bdev_io->u.bdev.md_buf,
			    parent_io->u.bdev.md_buf,
			    spdk_bdev_get_md_size(bdev));
	}

	spdk_bdev_free_io(bdev_io); /* [한국어] 읽기 I/O 해제 */

	if (rc == 0) {
		/* [한국어] 데이터 및 MD 모두 일치 → SUCCESS */
		parent_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
		parent_io->internal.cb(parent_io, true, parent_io->internal.caller_ctx);
	} else {
		/* [한국어] 불일치 발견 → MISCOMPARE 완료 */
		parent_io->internal.status = SPDK_BDEV_IO_STATUS_MISCOMPARE;
		parent_io->internal.cb(parent_io, false, parent_io->internal.caller_ctx);
	}
}

/*
 * [한국어]
 * bdev_compare_do_read - COMPARE 에뮬레이션의 읽기 단계 실행
 *
 * @_bdev_io: 원본 COMPARE bdev_io 포인터
 *
 * 모듈이 COMPARE를 지원하지 않을 때 bdev 레이어가 직접 READ + memcmp를 수행한다.
 * spdk_bdev_read_blocks()로 디스크 데이터를 읽고, 완료 콜백(bdev_compare_do_read_done)에서 비교.
 * ENOMEM 발생 시 bdev_queue_io_wait_with_cb()로 재시도 큐에 등록.
 *
 * 호출 체인:
 *   bdev_comparev_blocks_with_md() → [이 함수] → spdk_bdev_read_blocks()
 *   → bdev_compare_do_read_done()
 */
static void
bdev_compare_do_read(void *_bdev_io)
{
	struct spdk_bdev_io *bdev_io = _bdev_io; /* [한국어] 원본 COMPARE bdev_io */
	int rc;

	rc = spdk_bdev_read_blocks(bdev_io->internal.desc,
				   spdk_io_channel_from_ctx(bdev_io->internal.ch), NULL,
				   bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
				   bdev_compare_do_read_done, bdev_io);
	/* [한국어] 에뮬레이션을 위해 COMPARE 대상 블록을 읽기 시작 */

	if (rc == -ENOMEM) {
		/* [한국어] 메모리 부족 → nomem 큐에 등록해 재시도 */
		bdev_queue_io_wait_with_cb(bdev_io, bdev_compare_do_read);
	} else if (rc != 0) {
		/* [한국어] 기타 오류 → FAILED 완료 */
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
	}
}

static int
bdev_comparev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			     struct iovec *iov, int iovcnt, void *md_buf,
			     uint64_t offset_blocks, uint64_t num_blocks,
			     spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_COMPARE;
	bdev_io->u.bdev.iovs = iov;
	bdev_io->u.bdev.iovcnt = iovcnt;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	bdev_io->u.bdev.memory_domain = NULL;
	bdev_io->u.bdev.memory_domain_ctx = NULL;
	bdev_io->u.bdev.accel_sequence = NULL;

	if (bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE)) {
		bdev_io_submit(bdev_io);
		return 0;
	}

	bdev_compare_do_read(bdev_io);

	return 0;
}

int
spdk_bdev_comparev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  struct iovec *iov, int iovcnt,
			  uint64_t offset_blocks, uint64_t num_blocks,
			  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_comparev_blocks_with_md(desc, ch, iov, iovcnt, NULL, offset_blocks,
					    num_blocks, cb, cb_arg);
}

int
spdk_bdev_comparev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				  struct iovec *iov, int iovcnt, void *md_buf,
				  uint64_t offset_blocks, uint64_t num_blocks,
				  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (md_buf && !spdk_bdev_is_md_separate(spdk_bdev_desc_get_bdev(desc))) {
		return -EINVAL;
	}

	if (md_buf && !_is_buf_allocated(iov)) {
		return -EINVAL;
	}

	return bdev_comparev_blocks_with_md(desc, ch, iov, iovcnt, md_buf, offset_blocks,
					    num_blocks, cb, cb_arg);
}

static int
bdev_compare_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    void *buf, void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
			    spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_COMPARE;
	bdev_io->u.bdev.iovs = &bdev_io->iov;
	bdev_io->u.bdev.iovs[0].iov_base = buf;
	bdev_io->u.bdev.iovs[0].iov_len = num_blocks * bdev_desc_get_block_size(desc);
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	bdev_io->u.bdev.memory_domain = NULL;
	bdev_io->u.bdev.memory_domain_ctx = NULL;
	bdev_io->u.bdev.accel_sequence = NULL;

	if (bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE)) {
		bdev_io_submit(bdev_io);
		return 0;
	}

	bdev_compare_do_read(bdev_io);

	return 0;
}

int
spdk_bdev_compare_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			 void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_compare_blocks_with_md(desc, ch, buf, NULL, offset_blocks, num_blocks,
					   cb, cb_arg);
}

int
spdk_bdev_compare_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				 void *buf, void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
				 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct iovec iov = {
		.iov_base = buf,
	};

	if (md_buf && !spdk_bdev_is_md_separate(spdk_bdev_desc_get_bdev(desc))) {
		return -EINVAL;
	}

	if (md_buf && !_is_buf_allocated(&iov)) {
		return -EINVAL;
	}

	return bdev_compare_blocks_with_md(desc, ch, buf, md_buf, offset_blocks, num_blocks,
					   cb, cb_arg);
}

/*
 * [한국어]
 * bdev_comparev_and_writev_blocks_unlocked - LBA 락 해제 완료 후 COMPARE_AND_WRITE 완료 처리
 *
 * @range:         해제된 LBA 범위 (사용되지 않음)
 * @ctx:           COMPARE_AND_WRITE bdev_io 포인터
 * @unlock_status: LBA 락 해제 결과 (0=성공)
 *
 * LBA 범위 락이 해제된 후 호출되어 parent_io의 최종 완료 콜백을 실행한다.
 * 락 해제 실패는 로그만 남기고 작업 결과에는 영향 없이 이전 상태 그대로 완료한다.
 *
 * 호출 체인:
 *   bdev_unlock_lba_range() 완료 → [이 함수] → bdev_io->internal.cb()
 */
static void
bdev_comparev_and_writev_blocks_unlocked(struct lba_range *range, void *ctx, int unlock_status)
{
	struct spdk_bdev_io *bdev_io = ctx;

	if (unlock_status) {
		SPDK_ERRLOG("LBA range unlock failed\n"); /* [한국어] 락 해제 실패 로그 */
	}

	bdev_io->internal.cb(bdev_io, bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS ? true :
			     false, bdev_io->internal.caller_ctx);
	/* [한국어] internal.status를 true/false로 변환해 최종 완료 콜백 호출 */
}

/*
 * [한국어]
 * bdev_comparev_and_writev_blocks_unlock - COMPARE_AND_WRITE 완료 후 LBA 락 해제 시작
 *
 * @bdev_io: COMPARE_AND_WRITE bdev_io 포인터
 * @status:  완료 상태 (SUCCESS/FAILED/MISCOMPARE 등)
 *
 * status를 internal.status에 저장하고 LBA 범위 락을 비동기로 해제한다.
 * 락 해제 완료 후 bdev_comparev_and_writev_blocks_unlocked()에서 사용자 콜백을 호출.
 *
 * 호출 체인:
 *   bdev_compare_and_write_do_write_done() / bdev_compare_and_write_do_compare_done() →
 *   [이 함수] → bdev_unlock_lba_range() → bdev_comparev_and_writev_blocks_unlocked()
 */
static void
bdev_comparev_and_writev_blocks_unlock(struct spdk_bdev_io *bdev_io, int status)
{
	bdev_io->internal.status = status; /* [한국어] 최종 완료 상태 저장 */

	bdev_unlock_lba_range(bdev_io->internal.desc, spdk_io_channel_from_ctx(bdev_io->internal.ch),
			      bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
			      bdev_comparev_and_writev_blocks_unlocked, bdev_io);
	/* [한국어] LBA 범위 락 비동기 해제 시작 */
}

/*
 * [한국어]
 * bdev_compare_and_write_do_write_done - COMPARE_AND_WRITE의 쓰기 단계 완료 콜백
 *
 * @bdev_io:  쓰기 I/O (완료 후 해제됨)
 * @success:  쓰기 성공 여부
 * @cb_arg:   parent_io (원본 COMPARE_AND_WRITE bdev_io)
 *
 * 쓰기 성공 → SUCCESS, 실패 → FAILED 상태로 LBA 락 해제 시작.
 *
 * 호출 체인:
 *   spdk_bdev_writev_blocks() 완료 → [이 함수] → bdev_comparev_and_writev_blocks_unlock()
 */
static void
bdev_compare_and_write_do_write_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;

	if (!success) {
		SPDK_ERRLOG("Compare and write operation failed\n"); /* [한국어] 쓰기 실패 로그 */
	}

	spdk_bdev_free_io(bdev_io); /* [한국어] 쓰기 I/O 해제 */

	bdev_comparev_and_writev_blocks_unlock(parent_io,
					       success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
	/* [한국어] 최종 상태 설정 후 LBA 락 해제 시작 */
}

/*
 * [한국어]
 * bdev_compare_and_write_do_write - COMPARE_AND_WRITE의 쓰기 단계 실행
 *
 * @_bdev_io: 원본 COMPARE_AND_WRITE bdev_io (fused_iovs에 쓰기 데이터)
 *
 * COMPARE 성공 후 호출되어 fused_iovs(쓰기 버퍼)로 writev를 실행한다.
 * ENOMEM 시 nomem 큐에 등록해 재시도.
 *
 * 호출 체인:
 *   bdev_compare_and_write_do_compare_done() → [이 함수] → spdk_bdev_writev_blocks()
 *   → bdev_compare_and_write_do_write_done()
 */
static void
bdev_compare_and_write_do_write(void *_bdev_io)
{
	struct spdk_bdev_io *bdev_io = _bdev_io;
	int rc;

	rc = spdk_bdev_writev_blocks(bdev_io->internal.desc,
				     spdk_io_channel_from_ctx(bdev_io->internal.ch),
				     bdev_io->u.bdev.fused_iovs, bdev_io->u.bdev.fused_iovcnt,
				     bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
				     bdev_compare_and_write_do_write_done, bdev_io);
	/* [한국어] COMPARE 성공 확인 후 fused_iovs(쓰기 버퍼)로 writev 실행 */

	if (rc == -ENOMEM) {
		bdev_queue_io_wait_with_cb(bdev_io, bdev_compare_and_write_do_write);
		/* [한국어] 메모리 부족 → nomem 큐에 등록해 재시도 */
	} else if (rc != 0) {
		bdev_comparev_and_writev_blocks_unlock(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		/* [한국어] 쓰기 제출 오류 → FAILED 상태로 락 해제 */
	}
}

/*
 * [한국어]
 * bdev_compare_and_write_do_compare_done - COMPARE_AND_WRITE의 비교 단계 완료 콜백
 *
 * @bdev_io:  비교 I/O (완료 후 해제됨)
 * @success:  비교 성공 여부 (불일치 시 false)
 * @cb_arg:   parent_io (원본 COMPARE_AND_WRITE bdev_io)
 *
 * 비교 실패(MISCOMPARE) → 락 해제하고 MISCOMPARE 완료.
 * 비교 성공 → bdev_compare_and_write_do_write()로 쓰기 단계 진행.
 *
 * 호출 체인:
 *   spdk_bdev_comparev_blocks() 완료 → [이 함수]
 *   → bdev_comparev_and_writev_blocks_unlock() 또는 bdev_compare_and_write_do_write()
 */
static void
bdev_compare_and_write_do_compare_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;

	spdk_bdev_free_io(bdev_io); /* [한국어] 비교 I/O 해제 */

	if (!success) {
		/* [한국어] 비교 불일치 → MISCOMPARE 상태로 락 해제 */
		bdev_comparev_and_writev_blocks_unlock(parent_io, SPDK_BDEV_IO_STATUS_MISCOMPARE);
		return;
	}

	bdev_compare_and_write_do_write(parent_io); /* [한국어] 비교 성공 → 쓰기 단계 진행 */
}

/*
 * [한국어]
 * bdev_compare_and_write_do_compare - COMPARE_AND_WRITE의 비교 단계 실행
 *
 * @_bdev_io: 원본 COMPARE_AND_WRITE bdev_io (iovs에 비교 데이터)
 *
 * LBA 락 획득 후 호출되어 iovs(비교 버퍼)로 comparev를 실행한다.
 * ENOMEM 시 재시도, 기타 오류 시 FIRST_FUSED_FAILED 상태로 락 해제.
 *
 * 호출 체인:
 *   bdev_comparev_and_writev_blocks_locked() → [이 함수] → spdk_bdev_comparev_blocks()
 *   → bdev_compare_and_write_do_compare_done()
 */
static void
bdev_compare_and_write_do_compare(void *_bdev_io)
{
	struct spdk_bdev_io *bdev_io = _bdev_io;
	int rc;

	rc = spdk_bdev_comparev_blocks(bdev_io->internal.desc,
				       spdk_io_channel_from_ctx(bdev_io->internal.ch), bdev_io->u.bdev.iovs,
				       bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
				       bdev_compare_and_write_do_compare_done, bdev_io);
	/* [한국어] iovs(비교 버퍼)로 comparev 실행 */

	if (rc == -ENOMEM) {
		bdev_queue_io_wait_with_cb(bdev_io, bdev_compare_and_write_do_compare);
		/* [한국어] 메모리 부족 → nomem 큐에 등록해 재시도 */
	} else if (rc != 0) {
		bdev_comparev_and_writev_blocks_unlock(bdev_io, SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED);
		/* [한국어] 비교 제출 오류 → FIRST_FUSED_FAILED 상태로 락 해제 */
	}
}

/*
 * [한국어]
 * bdev_comparev_and_writev_blocks_locked - LBA 락 획득 후 COMPARE_AND_WRITE 비교 단계 시작
 *
 * @range:  획득된 LBA 범위 (사용되지 않음)
 * @ctx:    COMPARE_AND_WRITE bdev_io 포인터
 * @status: LBA 락 획득 결과 (0=성공)
 *
 * LBA 락 획득 성공 시 비교 단계(bdev_compare_and_write_do_compare)를 시작.
 * 락 획득 실패 시 FIRST_FUSED_FAILED 완료.
 *
 * 호출 체인:
 *   bdev_lock_lba_range() 완료 → [이 함수] → bdev_compare_and_write_do_compare()
 */
static void
bdev_comparev_and_writev_blocks_locked(struct lba_range *range, void *ctx, int status)
{
	struct spdk_bdev_io *bdev_io = ctx;

	if (status) {
		/* [한국어] LBA 락 획득 실패 → FIRST_FUSED_FAILED 완료 */
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED;
		bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
		return;
	}

	bdev_compare_and_write_do_compare(bdev_io); /* [한국어] 락 획득 성공 → 비교 단계 시작 */
}

/*
 * [한국어]
 * spdk_bdev_comparev_and_writev_blocks - COMPARE_AND_WRITE 연산 진입점
 *
 * @desc:          쓰기 가능한 bdev 디스크립터
 * @ch:            bdev 채널 (리액터 스레드에 속함)
 * @compare_iov:   비교 데이터 iovec 배열
 * @compare_iovcnt: compare_iov 개수
 * @write_iov:     쓰기 데이터 iovec 배열 (비교 성공 시 기록됨)
 * @write_iovcnt:  write_iov 개수
 * @offset_blocks: 시작 LBA
 * @num_blocks:    처리 블록 수 (acwu 초과 불가)
 * @cb:            완료 콜백
 * @cb_arg:        콜백 인자
 * @return:        0=성공, -EINVAL=범위/크기 오류, -EBADF=읽기전용, -ENOMEM=bdev_io 풀 소진
 *
 * Atomic Compare-and-Write 연산: 디스크의 LBA 범위를 compare_iov와 비교하여
 * 일치하면 write_iov로 교체한다. 불일치 시 MISCOMPARE 완료.
 *
 * 모듈이 COMPARE_AND_WRITE를 네이티브로 지원하면 직접 제출하고,
 * 지원하지 않으면 LBA 범위 락을 획득해 소프트웨어 에뮬레이션을 수행한다.
 * - acwu(Atomic Compare & Write Unit): NVMe IDENTIFY에서 제공하는 연산 단위 한계.
 *
 * 호출 체인:
 *   사용자/상위 모듈 → [이 함수]
 *   → bdev_io_submit() (네이티브 지원 시)
 *   → bdev_lock_lba_range() → bdev_comparev_and_writev_blocks_locked() (에뮬레이션 시)
 */
int
spdk_bdev_comparev_and_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				     struct iovec *compare_iov, int compare_iovcnt,
				     struct iovec *write_iov, int write_iovcnt,
				     uint64_t offset_blocks, uint64_t num_blocks,
				     spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF; /* [한국어] 읽기 전용 디스크립터 거부 */
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL; /* [한국어] LBA 범위 유효성 검사 실패 */
	}

	if (num_blocks > bdev->acwu) {
		return -EINVAL; /* [한국어] acwu(Atomic Compare & Write Unit) 초과 — 한 번에 처리 가능한 최대 블록 수 제한 */
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->internal.ch = channel;               /* [한국어] I/O 채널 설정 */
	bdev_io->internal.desc = desc;                /* [한국어] 디스크립터 설정 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE; /* [한국어] I/O 타입 */
	bdev_io->u.bdev.iovs = compare_iov;           /* [한국어] 비교 데이터 iovec */
	bdev_io->u.bdev.iovcnt = compare_iovcnt;      /* [한국어] 비교 iov 개수 */
	bdev_io->u.bdev.fused_iovs = write_iov;       /* [한국어] 쓰기 데이터 iovec (비교 성공 시 사용) */
	bdev_io->u.bdev.fused_iovcnt = write_iovcnt;  /* [한국어] 쓰기 iov 개수 */
	bdev_io->u.bdev.md_buf = NULL;                /* [한국어] 메타데이터 버퍼 없음 */
	bdev_io->u.bdev.num_blocks = num_blocks;      /* [한국어] 처리 블록 수 */
	bdev_io->u.bdev.offset_blocks = offset_blocks; /* [한국어] 시작 LBA */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);       /* [한국어] bdev/cb 공통 초기화 */
	bdev_io->u.bdev.memory_domain = NULL;         /* [한국어] 메모리 도메인 없음 */
	bdev_io->u.bdev.memory_domain_ctx = NULL;     /* [한국어] 메모리 도메인 컨텍스트 없음 */
	bdev_io->u.bdev.accel_sequence = NULL;        /* [한국어] accel 시퀀스 없음 */

	if (bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE)) {
		/* [한국어] 모듈이 COMPARE_AND_WRITE를 네이티브 지원 → 직접 제출 */
		bdev_io_submit(bdev_io);
		return 0;
	}

	return bdev_lock_lba_range(desc, ch, offset_blocks, num_blocks,
				   bdev_comparev_and_writev_blocks_locked, bdev_io);
	/* [한국어] 소프트웨어 에뮬레이션: LBA 범위 락 획득 후 비교→쓰기 단계 진행 */
}

/*
 * [한국어]
 * spdk_bdev_zcopy_start - 제로카피(Zero-Copy) I/O 세션 시작
 *
 * @desc:          bdev 디스크립터 (쓰기 가능 필수)
 * @ch:            bdev 채널
 * @iov:           I/O에 사용할 iovec 배열 (모듈이 채워줄 버퍼 포인터들)
 * @iovcnt:        iov 개수
 * @offset_blocks: 시작 LBA
 * @num_blocks:    처리 블록 수
 * @populate:      true=기존 데이터를 버퍼로 읽어옴(read-modify-write용), false=빈 버퍼 제공(순수 쓰기용)
 * @cb:            완료 콜백
 * @cb_arg:        콜백 인자
 * @return:        0=성공, -EBADF/-EINVAL/-ENOTSUP/-ENOMEM
 *
 * ZCOPY는 사용자 데이터를 복사하지 않고 모듈이 직접 버퍼를 제공하는 방식이다.
 * start() 완료 후 콜백에서 iov에 직접 접근해 데이터 읽기/쓰기 후 zcopy_end()를 호출한다.
 * ZCOPY 지원은 모듈이 보장해야 하므로 미지원 시 -ENOTSUP 반환.
 *
 * 호출 체인:
 *   상위 모듈 → [이 함수] → bdev_io_submit() → 모듈 ZCOPY 핸들러
 *   → spdk_bdev_zcopy_end()
 */
int
spdk_bdev_zcopy_start(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      struct iovec *iov, int iovcnt,
		      uint64_t offset_blocks, uint64_t num_blocks,
		      bool populate,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF; /* [한국어] 읽기 전용 디스크립터 거부 */
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL; /* [한국어] LBA 범위 유효성 검사 실패 */
	}

	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ZCOPY)) {
		return -ENOTSUP; /* [한국어] 모듈이 ZCOPY 미지원 */
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->internal.ch = channel;                    /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;                     /* [한국어] 디스크립터 설정 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_ZCOPY;           /* [한국어] I/O 타입 */
	bdev_io->u.bdev.num_blocks = num_blocks;           /* [한국어] 처리 블록 수 */
	bdev_io->u.bdev.offset_blocks = offset_blocks;    /* [한국어] 시작 LBA */
	bdev_io->u.bdev.iovs = iov;                        /* [한국어] 모듈이 채워줄 iovec */
	bdev_io->u.bdev.iovcnt = iovcnt;                   /* [한국어] iov 개수 */
	bdev_io->u.bdev.md_buf = NULL;                     /* [한국어] 메타데이터 버퍼 없음 */
	bdev_io->u.bdev.zcopy.populate = populate ? 1 : 0; /* [한국어] 기존 데이터 읽기 여부 (1=읽어옴) */
	bdev_io->u.bdev.zcopy.commit = 0;                  /* [한국어] start 단계이므로 commit=0 */
	bdev_io->u.bdev.zcopy.start = 1;                   /* [한국어] start 단계 표시 */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);            /* [한국어] bdev/cb 공통 초기화 */
	bdev_io->u.bdev.memory_domain = NULL;              /* [한국어] 메모리 도메인 없음 */
	bdev_io->u.bdev.memory_domain_ctx = NULL;          /* [한국어] 메모리 도메인 컨텍스트 없음 */
	bdev_io->u.bdev.accel_sequence = NULL;             /* [한국어] accel 시퀀스 없음 */

	bdev_io_submit(bdev_io); /* [한국어] 모듈에 ZCOPY start 요청 제출 */

	return 0;
}

/*
 * [한국어]
 * spdk_bdev_zcopy_end - 제로카피 I/O 세션 종료 및 데이터 커밋/완료
 *
 * @bdev_io: spdk_bdev_zcopy_start()로 시작된 ZCOPY bdev_io
 * @commit:  true=수정된 데이터를 디스크에 반영, false=변경 취소
 * @cb:      완료 콜백
 * @cb_arg:  콜백 인자
 * @return:  0=성공, -EINVAL=ZCOPY 타입이 아닌 경우
 *
 * ZCOPY 세션을 종료한다. commit=true이면 사용자가 버퍼에 쓴 데이터를 디스크에 반영.
 * commit=false이면 데이터 변경 없이 버퍼만 반환.
 * 동일한 bdev_io를 재사용하며 zcopy.start=0, zcopy.commit을 설정해 모듈에 전달.
 *
 * 호출 체인:
 *   사용자 콜백 (zcopy_start 완료 후) → [이 함수] → bdev_io_submit() → 모듈 ZCOPY end 핸들러
 */
int
spdk_bdev_zcopy_end(struct spdk_bdev_io *bdev_io, bool commit,
		    spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (bdev_io->type != SPDK_BDEV_IO_TYPE_ZCOPY) {
		return -EINVAL; /* [한국어] ZCOPY 세션이 아닌 bdev_io 거부 */
	}

	bdev_io->u.bdev.zcopy.commit = commit ? 1 : 0; /* [한국어] 커밋 여부 설정 */
	bdev_io->u.bdev.zcopy.start = 0;               /* [한국어] end 단계 표시 */
	bdev_io->internal.caller_ctx = cb_arg;          /* [한국어] 새 콜백 컨텍스트 */
	bdev_io->internal.cb = cb;                      /* [한국어] 새 완료 콜백 */
	bdev_io->internal.status = SPDK_BDEV_IO_STATUS_PENDING; /* [한국어] 상태 초기화 */

	bdev_io_submit(bdev_io); /* [한국어] 모듈에 ZCOPY end 요청 제출 */

	return 0;
}

/*
 * [한국어]
 * spdk_bdev_write_zeroes - 바이트 단위 오프셋으로 WRITE_ZEROES 요청
 *
 * @desc:   bdev 디스크립터
 * @ch:     bdev 채널
 * @offset: 바이트 단위 시작 오프셋
 * @len:    바이트 단위 길이
 * @cb:     완료 콜백
 * @cb_arg: 콜백 인자
 * @return: 0=성공, -EINVAL=블록 정렬 오류
 *
 * 바이트 오프셋을 블록 단위로 변환 후 spdk_bdev_write_zeroes_blocks()에 위임한다.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → spdk_bdev_write_zeroes_blocks()
 */
int
spdk_bdev_write_zeroes(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset, uint64_t len,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(desc, offset, &offset_blocks, len, &num_blocks) != 0) {
		return -EINVAL; /* [한국어] 블록 크기로 정렬되지 않은 바이트 오프셋/길이 */
	}

	return spdk_bdev_write_zeroes_blocks(desc, ch, offset_blocks, num_blocks, cb, cb_arg);
	/* [한국어] 블록 단위 WRITE_ZEROES 함수에 위임 */
}

/*
 * [한국어]
 * spdk_bdev_write_zeroes_blocks - 블록 범위에 0을 기록하는 WRITE_ZEROES 요청
 *
 * @desc:          bdev 디스크립터 (쓰기 가능 필수)
 * @ch:            bdev 채널
 * @offset_blocks: 시작 LBA
 * @num_blocks:    처리 블록 수
 * @cb:            완료 콜백
 * @cb_arg:        콜백 인자
 * @return:        0=성공, -EBADF/-EINVAL/-ENOTSUP/-ENOMEM
 *
 * WRITE_ZEROES 요청을 처리한다. 모듈이 네이티브 WRITE_ZEROES를 지원하거나 split이
 * 필요하면 직접 제출. 그렇지 않으면 zero_buffer(ZERO_BUFFER_SIZE 크기의 정적 버퍼)를
 * 이용한 일반 WRITE로 에뮬레이션(bdev_write_zero_buffer).
 * WRITE_ZEROES와 WRITE 모두 미지원이면 -ENOTSUP 반환.
 *
 * 호출 체인:
 *   spdk_bdev_write_zeroes() / 상위 모듈 → [이 함수]
 *   → bdev_io_submit() 또는 bdev_write_zero_buffer()
 */
int
spdk_bdev_write_zeroes_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      uint64_t offset_blocks, uint64_t num_blocks,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF; /* [한국어] 읽기 전용 디스크립터 거부 */
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL; /* [한국어] LBA 범위 유효성 검사 실패 */
	}

	if (!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES) &&
	    !bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE)) {
		return -ENOTSUP; /* [한국어] WRITE_ZEROES/WRITE 모두 미지원 → 에뮬레이션 불가 */
	}

	bdev_io = bdev_channel_get_io(channel);

	if (!bdev_io) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE_ZEROES;      /* [한국어] I/O 타입 */
	bdev_io->internal.ch = channel;                       /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;                        /* [한국어] 디스크립터 설정 */
	bdev_io->u.bdev.offset_blocks = offset_blocks;       /* [한국어] 시작 LBA */
	bdev_io->u.bdev.num_blocks = num_blocks;              /* [한국어] 처리 블록 수 */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);               /* [한국어] bdev/cb 공통 초기화 */
	bdev_io->u.bdev.memory_domain = NULL;                 /* [한국어] 메모리 도메인 없음 */
	bdev_io->u.bdev.memory_domain_ctx = NULL;             /* [한국어] 메모리 도메인 컨텍스트 없음 */
	bdev_io->u.bdev.accel_sequence = NULL;                /* [한국어] accel 시퀀스 없음 */

	/* If the write_zeroes size is large and should be split, use the generic split
	 * logic regardless of whether SPDK_BDEV_IO_TYPE_WRITE_ZEREOS is supported or not.
	 *
	 * Then, send the write_zeroes request if SPDK_BDEV_IO_TYPE_WRITE_ZEROES is supported
	 * or emulate it using regular write request otherwise.
	 */
	if (bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES) ||
	    bdev_io->internal.f.split) {
		/* [한국어] 네이티브 WRITE_ZEROES 지원 또는 split 필요 → 직접 제출
		 * split 경우 분할 로직이 알아서 WRITE_ZEROES or WRITE로 처리함 */
		bdev_io_submit(bdev_io);
		return 0;
	}

	assert(_bdev_get_block_size_with_md(bdev) <= ZERO_BUFFER_SIZE);
	/* [한국어] 블록 크기가 ZERO_BUFFER_SIZE를 넘지 않아야 에뮬레이션 가능 */

	bdev_write_zero_buffer(bdev_io); /* [한국어] 정적 zero_buffer를 이용한 일반 WRITE 에뮬레이션 */
	return 0;
}

/*
 * [한국어]
 * spdk_bdev_write_uncorrectable_blocks - 복구 불가 오류 블록 마킹(WRITE_UNCORRECTABLE) 요청
 *
 * @desc:          bdev 디스크립터 (쓰기 가능 필수)
 * @ch:            bdev 채널
 * @offset_blocks: 시작 LBA
 * @num_blocks:    처리 블록 수
 * @cb:            완료 콜백
 * @cb_arg:        콜백 인자
 * @return:        0=성공, -EBADF/-EINVAL/-ENOTSUP/-ENOMEM
 *
 * WRITE_UNCORRECTABLE(NVMe DSM Deallocate와 다름)은 특정 LBA를 "복구 불가 상태"로
 * 마킹하는 진단용 연산이다. 이후 해당 LBA 읽기 시 미디어 오류가 반환된다.
 * 에뮬레이션 없이 모듈 네이티브 지원 필수.
 *
 * 호출 체인:
 *   상위 모듈 → [이 함수] → bdev_io_submit()
 */
int
spdk_bdev_write_uncorrectable_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				     uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF; /* [한국어] 읽기 전용 디스크립터 거부 */
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL; /* [한국어] LBA 범위 유효성 검사 실패 */
	}

	if (!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_UNCORRECTABLE)) {
		return -ENOTSUP; /* [한국어] 모듈이 WRITE_UNCORRECTABLE 미지원 */
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE_UNCORRECTABLE; /* [한국어] I/O 타입 */
	bdev_io->internal.ch = channel;                         /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;                          /* [한국어] 디스크립터 설정 */
	bdev_io->u.bdev.iovs = NULL;                            /* [한국어] 데이터 버퍼 없음 (마킹 연산) */
	bdev_io->u.bdev.iovcnt = 0;                             /* [한국어] iov 없음 */
	bdev_io->u.bdev.offset_blocks = offset_blocks;         /* [한국어] 시작 LBA */
	bdev_io->u.bdev.num_blocks = num_blocks;                /* [한국어] 처리 블록 수 */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);                /* [한국어] bdev/cb 공통 초기화 */
	bdev_io->u.bdev.memory_domain = NULL;                   /* [한국어] 메모리 도메인 없음 */
	bdev_io->u.bdev.memory_domain_ctx = NULL;               /* [한국어] 메모리 도메인 컨텍스트 없음 */
	bdev_io->u.bdev.accel_sequence = NULL;                  /* [한국어] accel 시퀀스 없음 */

	bdev_io_submit(bdev_io); /* [한국어] 모듈에 WRITE_UNCORRECTABLE 요청 제출 */
	return 0;
}

/*
 * [한국어]
 * spdk_bdev_unmap - 바이트 단위 오프셋으로 UNMAP(Trim/Discard) 요청
 *
 * @desc:   bdev 디스크립터
 * @ch:     bdev 채널
 * @offset: 바이트 단위 시작 오프셋
 * @nbytes: 바이트 단위 길이
 * @cb:     완료 콜백
 * @cb_arg: 콜백 인자
 * @return: 0=성공, -EINVAL=블록 정렬 오류
 *
 * 바이트 오프셋을 블록 단위로 변환 후 spdk_bdev_unmap_blocks()에 위임한다.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → spdk_bdev_unmap_blocks()
 */
int
spdk_bdev_unmap(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(desc, offset, &offset_blocks, nbytes, &num_blocks) != 0) {
		return -EINVAL; /* [한국어] 블록 크기로 정렬되지 않은 바이트 오프셋/길이 */
	}

	return spdk_bdev_unmap_blocks(desc, ch, offset_blocks, num_blocks, cb, cb_arg);
	/* [한국어] 블록 단위 UNMAP 함수에 위임 */
}

/*
 * [한국어]
 * bdev_io_complete_cb - I/O를 SUCCESS로 즉시 완료하는 비동기 콜백
 *
 * @ctx: 완료할 bdev_io 포인터
 *
 * 현재 스레드에서 즉시 완료할 수 없는 상황에서 메시지 큐를 통해 비동기로 SUCCESS 완료를 수행.
 * 주로 num_blocks==0 UNMAP처럼 "아무것도 할 것이 없지만 비동기 완료 패턴을 유지"할 때 사용.
 *
 * 호출 체인:
 *   spdk_thread_send_msg() → [이 함수] (현재 스레드 메시지 루프에서)
 */
static void
bdev_io_complete_cb(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;

	bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS; /* [한국어] SUCCESS 상태 설정 */
	bdev_io->internal.cb(bdev_io, true, bdev_io->internal.caller_ctx); /* [한국어] 완료 콜백 호출 */
}

/*
 * [한국어]
 * spdk_bdev_unmap_blocks - 블록 범위 UNMAP(Trim/Discard) 요청
 *
 * @desc:          bdev 디스크립터 (쓰기 가능 필수)
 * @ch:            bdev 채널
 * @offset_blocks: 시작 LBA
 * @num_blocks:    처리 블록 수 (0이면 즉시 SUCCESS 완료)
 * @cb:            완료 콜백
 * @cb_arg:        콜백 인자
 * @return:        0=성공, -EBADF/-EINVAL/-ENOMEM
 *
 * UNMAP은 SSD에게 특정 LBA 범위가 더 이상 유효하지 않음을 알려 내부 블록 관리를 최적화한다.
 * NVMe DSM(Dataset Management) Deallocate 명령에 해당.
 * num_blocks==0이면 실제 I/O 없이 즉시 SUCCESS 비동기 완료.
 *
 * 호출 체인:
 *   spdk_bdev_unmap() / 상위 모듈 → [이 함수] → bdev_io_submit() 또는 bdev_io_complete_cb()
 */
int
spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF; /* [한국어] 읽기 전용 디스크립터 거부 */
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL; /* [한국어] LBA 범위 유효성 검사 실패 */
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->internal.ch = channel;          /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;           /* [한국어] 디스크립터 설정 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_UNMAP; /* [한국어] I/O 타입 */

	bdev_io->u.bdev.iovs = &bdev_io->iov;   /* [한국어] 내장 iov 사용 (데이터 없음) */
	bdev_io->u.bdev.iovs[0].iov_base = NULL; /* [한국어] UNMAP은 데이터 버퍼 없음 */
	bdev_io->u.bdev.iovs[0].iov_len = 0;    /* [한국어] 길이 0 */
	bdev_io->u.bdev.iovcnt = 1;             /* [한국어] iov 1개 (더미) */

	bdev_io->u.bdev.offset_blocks = offset_blocks; /* [한국어] 시작 LBA */
	bdev_io->u.bdev.num_blocks = num_blocks;       /* [한국어] 처리 블록 수 */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);        /* [한국어] bdev/cb 공통 초기화 */
	bdev_io->u.bdev.memory_domain = NULL;          /* [한국어] 메모리 도메인 없음 */
	bdev_io->u.bdev.memory_domain_ctx = NULL;      /* [한국어] 메모리 도메인 컨텍스트 없음 */
	bdev_io->u.bdev.accel_sequence = NULL;         /* [한국어] accel 시퀀스 없음 */

	if (num_blocks == 0) {
		/* [한국어] 0 블록 UNMAP은 실제 I/O 없이 비동기로 즉시 SUCCESS 완료 */
		spdk_thread_send_msg(spdk_get_thread(), bdev_io_complete_cb, bdev_io);
		return 0;
	}

	bdev_io_submit(bdev_io); /* [한국어] 모듈에 UNMAP 요청 제출 */
	return 0;
}

/*
 * [한국어]
 * spdk_bdev_flush - 바이트 단위 오프셋으로 FLUSH 요청
 *
 * @desc:   bdev 디스크립터
 * @ch:     bdev 채널
 * @offset: 바이트 단위 시작 오프셋 (FLUSH는 범위 무시, 전체 플러시)
 * @length: 바이트 단위 길이 (범위 힌트 — 실제로는 전체 캐시 플러시)
 * @cb:     완료 콜백
 * @cb_arg: 콜백 인자
 * @return: 0=성공, -EINVAL=블록 정렬 오류
 *
 * 바이트 오프셋을 블록 단위로 변환 후 spdk_bdev_flush_blocks()에 위임한다.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → spdk_bdev_flush_blocks()
 */
int
spdk_bdev_flush(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset, uint64_t length,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(desc, offset, &offset_blocks, length, &num_blocks) != 0) {
		return -EINVAL; /* [한국어] 블록 크기로 정렬되지 않은 바이트 오프셋/길이 */
	}

	return spdk_bdev_flush_blocks(desc, ch, offset_blocks, num_blocks, cb, cb_arg);
	/* [한국어] 블록 단위 FLUSH 함수에 위임 */
}

/*
 * [한국어]
 * spdk_bdev_flush_blocks - 블록 범위 캐시 FLUSH 요청
 *
 * @desc:          bdev 디스크립터 (쓰기 가능 필수)
 * @ch:            bdev 채널
 * @offset_blocks: 시작 LBA (힌트)
 * @num_blocks:    처리 블록 수 (힌트)
 * @cb:            완료 콜백
 * @cb_arg:        콜백 인자
 * @return:        0=성공, -EBADF/-ENOTSUP/-EINVAL/-ENOMEM
 *
 * FLUSH는 쓰기 캐시에 남아 있는 데이터를 영구 저장 매체에 동기화한다.
 * NVMe Flush 명령에 해당하며, 범위 매개변수는 힌트이나 대부분 모듈이 전체 캐시를 플러시한다.
 * FLUSH를 지원하지 않는 모듈은 -ENOTSUP 반환.
 *
 * 호출 체인:
 *   spdk_bdev_flush() / 상위 모듈 → [이 함수] → bdev_io_submit()
 */
int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF; /* [한국어] 읽기 전용 디스크립터 거부 */
	}

	if (spdk_unlikely(!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH))) {
		return -ENOTSUP; /* [한국어] 모듈이 FLUSH 미지원 */
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL; /* [한국어] LBA 범위 유효성 검사 실패 */
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->internal.ch = channel;           /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;            /* [한국어] 디스크립터 설정 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_FLUSH;  /* [한국어] I/O 타입 */
	bdev_io->u.bdev.iovs = NULL;              /* [한국어] 데이터 버퍼 없음 */
	bdev_io->u.bdev.iovcnt = 0;              /* [한국어] iov 없음 */
	bdev_io->u.bdev.offset_blocks = offset_blocks; /* [한국어] 시작 LBA (힌트) */
	bdev_io->u.bdev.num_blocks = num_blocks;       /* [한국어] 블록 수 (힌트) */
	bdev_io->u.bdev.memory_domain = NULL;          /* [한국어] 메모리 도메인 없음 */
	bdev_io->u.bdev.memory_domain_ctx = NULL;      /* [한국어] 메모리 도메인 컨텍스트 없음 */
	bdev_io->u.bdev.accel_sequence = NULL;         /* [한국어] accel 시퀀스 없음 */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);        /* [한국어] bdev/cb 공통 초기화 */

	bdev_io_submit(bdev_io); /* [한국어] 모듈에 FLUSH 요청 제출 */
	return 0;
}

static int bdev_reset_poll_for_outstanding_io(void *ctx);

/*
 * [한국어]
 * bdev_reset_check_outstanding_io_done - 모든 채널 outstanding I/O 확인 완료 후 처리
 *
 * @bdev:   대상 bdev
 * @_ctx:   RESET bdev_io 포인터
 * @status: 0=미outstanding, -EBUSY=아직 outstanding I/O 존재
 *
 * 채널 순회(bdev_reset_check_outstanding_io) 완료 후 호출된다.
 * - 아직 I/O가 있고(EBUSY) 타임아웃이 남았으면: wait_poller 재등록해 재확인.
 * - 아직 I/O가 있고 타임아웃 초과: memory_domain/accel_exec 완료 여부에 따라
 *   reset 제출 또는 FAILED 완료.
 * - I/O가 없으면(reset_io_drain_timeout 기다렸지만 자연 소멸): SUCCESS 완료.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() 완료 → [이 함수]
 *   → bdev_io_submit_reset() 또는 spdk_bdev_io_complete() 또는 폴러 재등록
 */
static void
bdev_reset_check_outstanding_io_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_io *bdev_io = _ctx;
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;

	if (status == -EBUSY) {
		/* [한국어] 아직 outstanding I/O 존재 */
		if (spdk_get_ticks() < bdev_io->u.reset.wait_poller.stop_time_tsc) {
			/* [한국어] 타임아웃 전 → wait_poller를 재등록해 주기적으로 재확인 */
			bdev_io->u.reset.wait_poller.poller = SPDK_POLLER_REGISTER(bdev_reset_poll_for_outstanding_io,
							      bdev_io, BDEV_RESET_CHECK_OUTSTANDING_IO_PERIOD_IN_USEC);
		} else {
			if (TAILQ_EMPTY(&ch->io_memory_domain) && TAILQ_EMPTY(&ch->io_accel_exec)) {
				/* If outstanding IOs are still present and reset_io_drain_timeout
				 * seconds passed, start the reset. */
				/* [한국어] 타임아웃 초과 + memory_domain/accel_exec 없음 → 강제 reset 제출 */
				bdev_io_submit_reset(bdev_io);
			} else {
				/* We still have in progress memory domain pull/push or we're
				 * executing accel sequence.  Since we cannot abort either of those
				 * operations, fail the reset request. */
				/* [한국어] memory_domain pull/push 또는 accel_exec 진행 중 → abort 불가이므로 FAILED */
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			}
		}
	} else {
		SPDK_DEBUGLOG(bdev,
			      "Skipping reset for underlying device of bdev: %s - no outstanding I/O.\n",
			      ch->bdev->name);
		/* Mark the completion status as a SUCCESS and complete the reset. */
		/* [한국어] outstanding I/O 없음 → reset 불필요, SUCCESS 완료 */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}
}

/*
 * [한국어]
 * bdev_reset_check_outstanding_io - 단일 채널의 outstanding I/O 존재 여부 확인
 *
 * @i:     채널 이터레이터 (계속/중단 제어)
 * @bdev:  대상 bdev
 * @io_ch: 현재 확인 중인 I/O 채널
 * @_ctx:  RESET bdev_io 포인터
 *
 * io_outstanding > 0이거나 io_memory_domain/io_accel_exec 큐가 비어있지 않으면
 * -EBUSY로 이터레이션을 중단한다. 모두 비어있으면 0으로 계속 진행.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → [이 함수] × 채널 수 → bdev_reset_check_outstanding_io_done()
 */
static void
bdev_reset_check_outstanding_io(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
				struct spdk_io_channel *io_ch, void *_ctx)
{
	struct spdk_bdev_channel *cur_ch = __io_ch_to_bdev_ch(io_ch);
	int status = 0;

	if (cur_ch->io_outstanding > 0 ||
	    !TAILQ_EMPTY(&cur_ch->io_memory_domain) ||
	    !TAILQ_EMPTY(&cur_ch->io_accel_exec)) {
		/* If a channel has outstanding IO, set status to -EBUSY code. This will stop
		 * further iteration over the rest of the channels and pass non-zero status
		 * to the callback function. */
		/* [한국어] outstanding I/O 존재 → -EBUSY로 이터레이션 중단 */
		status = -EBUSY;
	}
	spdk_bdev_for_each_channel_continue(i, status); /* [한국어] 이터레이터 진행 (status=-EBUSY면 조기 종료) */
}

/*
 * [한국어]
 * bdev_reset_poll_for_outstanding_io - reset_io_drain_timeout 대기 중 주기적 outstanding I/O 폴링
 *
 * @ctx: RESET bdev_io 포인터
 * @return: SPDK_POLLER_BUSY
 *
 * SPDK_POLLER_REGISTER로 등록되어 BDEV_RESET_CHECK_OUTSTANDING_IO_PERIOD_IN_USEC마다 호출.
 * 폴러를 해제하고 모든 채널의 outstanding I/O를 재확인한다.
 *
 * 호출 체인:
 *   SPDK 폴러 루프 → [이 함수] → spdk_bdev_for_each_channel() → bdev_reset_check_outstanding_io()
 */
static int
bdev_reset_poll_for_outstanding_io(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;

	spdk_poller_unregister(&bdev_io->u.reset.wait_poller.poller); /* [한국어] 현재 폴러 해제 */
	spdk_bdev_for_each_channel(bdev_io->bdev, bdev_reset_check_outstanding_io, bdev_io,
				   bdev_reset_check_outstanding_io_done);
	/* [한국어] 모든 채널의 outstanding I/O 재확인 */

	return SPDK_POLLER_BUSY; /* [한국어] 폴러가 작업을 수행했음을 리액터에 알림 */
}

/*
 * [한국어]
 * bdev_reset_freeze_channel_done - 모든 채널 동결(freeze) 완료 후 처리
 *
 * @bdev:   대상 bdev
 * @_ctx:   RESET bdev_io 포인터
 * @status: 항상 0 (freeze 단계에서 비에러)
 *
 * reset_io_drain_timeout이 0이면 즉시 모듈에 reset 제출.
 * 0이 아니면 stop_time_tsc를 계산하고 채널 outstanding I/O 확인 대기 루프 진입.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel(freeze) 완료 → [이 함수]
 *   → bdev_io_submit_reset() 또는 spdk_bdev_for_each_channel(outstanding_check)
 */
static void
bdev_reset_freeze_channel_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_io *bdev_io = _ctx;

	if (bdev->reset_io_drain_timeout == 0) {
		/* [한국어] drain 타임아웃 없음 → 즉시 모듈에 reset 제출 */
		bdev_io_submit_reset(bdev_io);
		return;
	}

	bdev_io->u.reset.wait_poller.stop_time_tsc = spdk_get_ticks() +
			(bdev->reset_io_drain_timeout * spdk_get_ticks_hz());
	/* [한국어] 타임아웃 만료 기준 TSC 계산: 현재 ticks + drain_timeout 초 */

	/* In case bdev->reset_io_drain_timeout is not equal to zero,
	 * submit the reset to the underlying module only if outstanding I/O
	 * remain after reset_io_drain_timeout seconds have passed. */
	/* [한국어] drain_timeout 내에 outstanding I/O가 소멸하면 reset 불필요 → 먼저 확인 */
	spdk_bdev_for_each_channel(bdev, bdev_reset_check_outstanding_io, bdev_io,
				   bdev_reset_check_outstanding_io_done);
}

/*
 * [한국어]
 * bdev_reset_freeze_channel - 단일 채널을 RESET 중 상태로 동결
 *
 * @i:    채널 이터레이터
 * @bdev: 대상 bdev
 * @ch:   현재 동결할 I/O 채널
 * @_ctx: RESET bdev_io 포인터
 *
 * BDEV_CH_RESET_IN_PROGRESS 플래그를 설정해 새 I/O 제출을 차단하고,
 * 이미 큐에 있는 nomem/buf/qos I/O들을 모두 abort한다.
 * nomem I/O를 먼저 abort하는 이유: 다른 I/O abort 시 nomem 재시도를 막기 위함.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → [이 함수] × 채널 수 → bdev_reset_freeze_channel_done()
 */
static void
bdev_reset_freeze_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			  struct spdk_io_channel *ch, void *_ctx)
{
	struct spdk_bdev_channel	*channel;
	struct spdk_bdev_mgmt_channel	*mgmt_channel;
	struct spdk_bdev_shared_resource *shared_resource;

	channel = __io_ch_to_bdev_ch(ch);         /* [한국어] io_ch → bdev_channel 변환 */
	shared_resource = channel->shared_resource; /* [한국어] 공유 리소스 (mgmt_ch 포함) */
	mgmt_channel = shared_resource->mgmt_ch;   /* [한국어] 관리 채널 (iobuf 대기 큐 포함) */

	channel->flags |= BDEV_CH_RESET_IN_PROGRESS; /* [한국어] 채널을 reset 중 상태로 표시 */

	/**
	 * Abort nomem I/Os first so that aborting other queued I/Os won't resubmit
	 * nomem I/Os of this channel.
	 */
	/* [한국어] nomem I/O 먼저 abort: 다른 큐 abort 중 nomem 재시도를 방지 */
	bdev_abort_all_nomem_io(channel);
	bdev_abort_all_buf_io(mgmt_channel, channel); /* [한국어] iobuf 대기 중인 I/O abort */

	if ((channel->flags & BDEV_CH_QOS_ENABLED) != 0) {
		/* [한국어] QoS 활성화된 채널 → QoS 큐의 대기 I/O abort */
		bdev_abort_all_queued_io(&channel->qos_queued_io, channel);
	}

	spdk_bdev_for_each_channel_continue(i, 0); /* [한국어] 이터레이터 계속 (다음 채널로) */
}

/*
 * [한국어]
 * bdev_start_reset - RESET I/O 처리 시작
 *
 * @bdev_io: RESET 타입의 bdev_io
 *
 * RESET은 bdev 전체를 초기화하는 연산이다.
 * 동시에 하나만 진행 가능하며, 이미 진행 중이면 queued_resets에 적재.
 * 처음 시작하는 reset은 채널 동결(freeze) 후 모듈에 제출.
 *
 * 채널 참조를 획득해 reset 완료 전까지 채널이 파괴되지 않도록 보호.
 *
 * 호출 체인:
 *   spdk_bdev_reset() → [이 함수]
 *   → spdk_bdev_for_each_channel(freeze) → bdev_reset_freeze_channel() × N
 *   → bdev_reset_freeze_channel_done() → bdev_io_submit_reset()
 */
static void
bdev_start_reset(struct spdk_bdev_io *bdev_io)
{
	struct spdk_io_channel *io_ch = spdk_io_channel_from_ctx(bdev_io->internal.ch);
	struct spdk_bdev *bdev = bdev_io->bdev;
	bool freeze_channel = false;

	bdev_ch_add_to_io_submitted(bdev_io); /* [한국어] submitted 카운터 증가 */

	/**
	 * Take a channel reference for the target bdev for the life of this
	 *  reset.  This guards against the channel getting destroyed before
	 *  the reset is completed.  We will release the reference when this
	 *  reset is completed.
	 */
	/* [한국어] reset 완료 시까지 채널 참조 획득 — 채널 파괴 방지 */
	bdev_io->u.reset.ch_ref = spdk_io_channel_ref(io_ch);

	spdk_spin_lock(&bdev->internal.spinlock); /* [한국어] reset_in_progress 접근 보호 */
	if (bdev->internal.reset_in_progress == NULL) {
		/* [한국어] 진행 중인 reset 없음 → 이 reset이 현재 활성 reset */
		bdev->internal.reset_in_progress = bdev_io;
		freeze_channel = true;
	} else {
		/* [한국어] 이미 reset 진행 중 → queued_resets에 적재해 순서 보장 */
		TAILQ_INSERT_TAIL(&bdev->internal.queued_resets, bdev_io, internal.link);
	}
	spdk_spin_unlock(&bdev->internal.spinlock);

	if (freeze_channel) {
		/* [한국어] 모든 채널을 RESET 중 상태로 동결 시작 */
		spdk_bdev_for_each_channel(bdev, bdev_reset_freeze_channel, bdev_io,
					   bdev_reset_freeze_channel_done);
	}
}

/*
 * [한국어]
 * spdk_bdev_nvme_nssr - NVMe NSSR(Namespace Specific Reset) 요청
 *
 * @desc:   bdev 디스크립터
 * @ch:     bdev 채널
 * @cb:     완료 콜백
 * @cb_arg: 콜백 인자
 * @return: 0=성공, -ENOTSUP/-ENOMEM
 *
 * NVME_NSSR은 네임스페이스 단위 소프트 리셋 명령이다.
 * RESET과 달리 채널 동결 없이 직접 모듈에 제출한다.
 *
 * 호출 체인:
 *   상위 모듈 → [이 함수] → bdev_io_submit()
 */
int
spdk_bdev_nvme_nssr(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!bdev_io_type_supported(spdk_bdev_desc_get_bdev(desc),
				    SPDK_BDEV_IO_TYPE_NVME_NSSR)) {
		return -ENOTSUP; /* [한국어] 모듈이 NVME_NSSR 미지원 */
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->internal.ch = channel;                       /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;                        /* [한국어] 디스크립터 설정 */
	bdev_io->internal.submit_tsc = spdk_get_ticks();      /* [한국어] 제출 시각 기록 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_NSSR;          /* [한국어] I/O 타입 */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);               /* [한국어] bdev/cb 공통 초기화 */

	bdev_io_submit(bdev_io); /* [한국어] 모듈에 NVME_NSSR 요청 제출 */
	return 0;
}

/*
 * [한국어]
 * spdk_bdev_reset - bdev 전체 RESET 요청 (외부 API)
 *
 * @desc:   bdev 디스크립터
 * @ch:     bdev 채널
 * @cb:     완료 콜백
 * @cb_arg: 콜백 인자
 * @return: 0=성공, -ENOMEM
 *
 * RESET은 bdev의 모든 채널을 동결하고, 모듈에 하드 리셋을 요청한다.
 * 동시에 하나의 RESET만 진행되며, 추가 요청은 queued_resets에 적재된다.
 * reset_io_drain_timeout이 설정된 경우 outstanding I/O 소멸을 기다린 후 reset을 제출한다.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → bdev_start_reset()
 *   → spdk_bdev_for_each_channel(freeze) → ... → bdev_io_submit_reset()
 */
int
spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->internal.ch = channel;                   /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;                    /* [한국어] 디스크립터 설정 */
	bdev_io->internal.submit_tsc = spdk_get_ticks();  /* [한국어] 제출 시각 기록 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_RESET;           /* [한국어] I/O 타입 */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);            /* [한국어] bdev/cb 공통 초기화 */

	bdev_start_reset(bdev_io); /* [한국어] RESET 처리 시작 (채널 동결 → 제출) */
	return 0;
}

/*
 * [한국어]
 * spdk_bdev_get_io_stat - 단일 채널의 I/O 통계 조회
 *
 * @bdev:       대상 bdev (사용되지 않음)
 * @ch:         통계를 가져올 I/O 채널
 * @stat:       결과를 채울 통계 구조체
 * @reset_mode: 조회 후 통계 초기화 방식 (NONE/CLEAR)
 *
 * 현재 채널의 stat을 stat 구조체에 복사하고, reset_mode에 따라 채널 stat 초기화.
 * 단일 채널 통계 조회 전용.
 *
 * 호출 체인:
 *   상위 모듈 → [이 함수] → bdev_get_io_stat() + spdk_bdev_reset_io_stat()
 */
void
spdk_bdev_get_io_stat(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		      struct spdk_bdev_io_stat *stat, enum spdk_bdev_reset_stat_mode reset_mode)
{
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	bdev_get_io_stat(stat, channel->stat);                  /* [한국어] 채널 통계 복사 */
	spdk_bdev_reset_io_stat(channel->stat, reset_mode);     /* [한국어] reset_mode에 따라 초기화 */
}

/*
 * [한국어]
 * bdev_get_device_stat_done - 모든 채널 통계 수집 완료 후 콜백 호출
 *
 * @bdev:   대상 bdev
 * @_ctx:   bdev_iostat_ctx 포인터
 * @status: 항상 0
 *
 * 채널 순회 완료 후 사용자 콜백을 호출하고 컨텍스트를 해제한다.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() 완료 → [이 함수]
 */
static void
bdev_get_device_stat_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_iostat_ctx *bdev_iostat_ctx = _ctx;

	bdev_iostat_ctx->cb(bdev, bdev_iostat_ctx->stat,
			    bdev_iostat_ctx->cb_arg, 0); /* [한국어] 사용자 콜백 호출 */
	free(bdev_iostat_ctx);                       /* [한국어] 컨텍스트 해제 */
}

/*
 * [한국어]
 * bdev_get_each_channel_stat - 채널 순회 중 단일 채널 통계 누적
 *
 * @i:    채널 이터레이터
 * @bdev: 대상 bdev
 * @ch:   현재 채널
 * @_ctx: bdev_iostat_ctx 포인터
 *
 * 채널 stat을 누적 합산하고 reset_mode에 따라 채널 stat 초기화.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → [이 함수] × 채널 수 → bdev_get_device_stat_done()
 */
static void
bdev_get_each_channel_stat(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			   struct spdk_io_channel *ch, void *_ctx)
{
	struct spdk_bdev_iostat_ctx *bdev_iostat_ctx = _ctx;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	spdk_bdev_add_io_stat(bdev_iostat_ctx->stat, channel->stat);       /* [한국어] 채널 통계 누적 */
	spdk_bdev_reset_io_stat(channel->stat, bdev_iostat_ctx->reset_mode); /* [한국어] 채널 통계 초기화 */
	spdk_bdev_for_each_channel_continue(i, 0);                          /* [한국어] 이터레이터 계속 */
}

/*
 * [한국어]
 * spdk_bdev_get_device_stat - bdev 전체 I/O 통계 비동기 조회
 *
 * @bdev:       대상 bdev
 * @stat:       결과를 채울 통계 구조체
 * @reset_mode: 조회 후 통계 초기화 방식
 * @cb:         완료 콜백
 * @cb_arg:     콜백 인자
 *
 * 삭제된 채널의 누적 통계(bdev->internal.stat)를 먼저 읽고,
 * 살아있는 모든 채널 stat을 순회해 누적한다.
 * spinlock으로 bdev->internal.stat 접근 보호.
 *
 * 호출 체인:
 *   상위 모듈 → [이 함수] → spdk_bdev_for_each_channel() → bdev_get_each_channel_stat() × N
 *   → bdev_get_device_stat_done()
 */
void
spdk_bdev_get_device_stat(struct spdk_bdev *bdev, struct spdk_bdev_io_stat *stat,
			  enum spdk_bdev_reset_stat_mode reset_mode, spdk_bdev_get_device_stat_cb cb, void *cb_arg)
{
	struct spdk_bdev_iostat_ctx *bdev_iostat_ctx;

	assert(bdev != NULL);
	assert(stat != NULL);
	assert(cb != NULL);

	bdev_iostat_ctx = calloc(1, sizeof(struct spdk_bdev_iostat_ctx));
	if (bdev_iostat_ctx == NULL) {
		SPDK_ERRLOG("Unable to allocate memory for spdk_bdev_iostat_ctx\n");
		cb(bdev, stat, cb_arg, -ENOMEM); /* [한국어] 메모리 부족 오류 콜백 */
		return;
	}

	bdev_iostat_ctx->stat = stat;             /* [한국어] 출력 통계 구조체 */
	bdev_iostat_ctx->cb = cb;                 /* [한국어] 완료 콜백 */
	bdev_iostat_ctx->cb_arg = cb_arg;         /* [한국어] 콜백 인자 */
	bdev_iostat_ctx->reset_mode = reset_mode; /* [한국어] 통계 초기화 모드 */

	/* Start with the statistics from previously deleted channels. */
	/* [한국어] 삭제된 채널의 누적 통계를 먼저 읽음 — spinlock 보호 */
	spdk_spin_lock(&bdev->internal.spinlock);
	bdev_get_io_stat(bdev_iostat_ctx->stat, bdev->internal.stat);
	spdk_bdev_reset_io_stat(bdev->internal.stat, reset_mode);
	spdk_spin_unlock(&bdev->internal.spinlock);

	/* Then iterate and add the statistics from each existing channel. */
	/* [한국어] 살아있는 모든 채널 stat 비동기 누적 */
	spdk_bdev_for_each_channel(bdev, bdev_get_each_channel_stat, bdev_iostat_ctx,
				   bdev_get_device_stat_done);
}

struct bdev_iostat_reset_ctx {
	enum spdk_bdev_reset_stat_mode mode;
	/* [한국어] 통계 초기화 모드 (NONE/CLEAR)
	 * 설정자: bdev_reset_device_stat()에서 ctx->mode = mode.
	 * 읽는 자: bdev_reset_each_channel_stat()에서 채널 stat 초기화 시 사용.
	 * 값 범위: spdk_bdev_reset_stat_mode enum.
	 * 동기화: 단일 채널 순회 컨텍스트이므로 락 불필요. */

	bdev_reset_device_stat_cb cb;
	/* [한국어] 완료 콜백 함수 포인터.
	 * 설정자: bdev_reset_device_stat()에서 ctx->cb = cb.
	 * 읽는 자: bdev_reset_device_stat_done()에서 호출.
	 * 값 범위: NULL이 아닌 콜백 함수.
	 * 동기화: 단일 스레드에서 사용되므로 락 불필요. */

	void *cb_arg;
	/* [한국어] 완료 콜백 인자.
	 * 설정자: bdev_reset_device_stat()에서 ctx->cb_arg = cb_arg.
	 * 읽는 자: bdev_reset_device_stat_done()에서 cb() 호출 시 전달.
	 * 값 범위: 임의 포인터 (NULL 포함).
	 * 동기화: 단일 스레드에서 사용되므로 락 불필요. */
};

/*
 * [한국어]
 * bdev_reset_device_stat_done - 모든 채널 통계 초기화 완료 후 콜백 호출
 *
 * @bdev:   대상 bdev
 * @_ctx:   bdev_iostat_reset_ctx 포인터
 * @status: 항상 0
 *
 * 채널 순회 완료 후 사용자 콜백을 호출하고 컨텍스트를 해제한다.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() 완료 → [이 함수]
 */
static void
bdev_reset_device_stat_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct bdev_iostat_reset_ctx *ctx = _ctx;

	ctx->cb(bdev, ctx->cb_arg, 0); /* [한국어] 완료 콜백 호출 */

	free(ctx); /* [한국어] 컨텍스트 해제 */
}

/*
 * [한국어]
 * bdev_reset_each_channel_stat - 채널 순회 중 단일 채널 통계 초기화
 *
 * @i:    채널 이터레이터
 * @bdev: 대상 bdev
 * @ch:   현재 채널
 * @_ctx: bdev_iostat_reset_ctx 포인터
 *
 * 채널 stat을 ctx->mode에 따라 초기화 후 이터레이터 계속.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → [이 함수] × 채널 수 → bdev_reset_device_stat_done()
 */
static void
bdev_reset_each_channel_stat(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			     struct spdk_io_channel *ch, void *_ctx)
{
	struct bdev_iostat_reset_ctx *ctx = _ctx;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	spdk_bdev_reset_io_stat(channel->stat, ctx->mode); /* [한국어] 채널 통계 초기화 */

	spdk_bdev_for_each_channel_continue(i, 0); /* [한국어] 이터레이터 계속 */
}

/*
 * [한국어]
 * bdev_reset_device_stat - bdev 전체 I/O 통계 초기화 (비동기)
 *
 * @bdev:   대상 bdev
 * @mode:   통계 초기화 모드
 * @cb:     완료 콜백
 * @cb_arg: 콜백 인자
 *
 * bdev->internal.stat(삭제 채널 누적)과 살아있는 모든 채널 stat을 초기화한다.
 * bdev->internal.stat 접근 시 spinlock 보호.
 *
 * 호출 체인:
 *   상위 모듈 → [이 함수] → spdk_bdev_for_each_channel() → bdev_reset_each_channel_stat() × N
 *   → bdev_reset_device_stat_done()
 */
void
bdev_reset_device_stat(struct spdk_bdev *bdev, enum spdk_bdev_reset_stat_mode mode,
		       bdev_reset_device_stat_cb cb, void *cb_arg)
{
	struct bdev_iostat_reset_ctx *ctx;

	assert(bdev != NULL);
	assert(cb != NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Unable to allocate bdev_iostat_reset_ctx.\n");
		cb(bdev, cb_arg, -ENOMEM); /* [한국어] 메모리 부족 오류 콜백 */
		return;
	}

	ctx->mode = mode;     /* [한국어] 초기화 모드 설정 */
	ctx->cb = cb;         /* [한국어] 완료 콜백 */
	ctx->cb_arg = cb_arg; /* [한국어] 콜백 인자 */

	/* [한국어] 삭제된 채널의 누적 통계 먼저 초기화 — spinlock 보호 */
	spdk_spin_lock(&bdev->internal.spinlock);
	spdk_bdev_reset_io_stat(bdev->internal.stat, mode);
	spdk_spin_unlock(&bdev->internal.spinlock);

	/* [한국어] 살아있는 모든 채널 통계 비동기 초기화 */
	spdk_bdev_for_each_channel(bdev,
				   bdev_reset_each_channel_stat,
				   ctx,
				   bdev_reset_device_stat_done);
}

/*
 * [한국어]
 * spdk_bdev_nvme_admin_passthru - NVMe Admin 명령 패스스루 요청
 *
 * @desc:   bdev 디스크립터 (쓰기 가능 필수)
 * @ch:     bdev 채널
 * @cmd:    NVMe Admin 명령 구조체 (Identify, Set Features 등)
 * @buf:    데이터 버퍼 (명령에 따라 사용)
 * @nbytes: 버퍼 크기
 * @cb:     완료 콜백
 * @cb_arg: 콜백 인자
 * @return: 0=성공, -EBADF/-ENOTSUP/-ENOMEM
 *
 * NVMe Admin 명령을 bdev 모듈에 그대로 전달한다.
 * Admin 명령은 컨트롤러 단위(채널 무관) 명령으로, NVMe 스펙의 Admin Command Set에 해당.
 * 쓰기 전용 디스크립터에만 허용.
 *
 * 호출 체인:
 *   상위 모듈 → [이 함수] → bdev_io_submit() → NVMe 드라이버
 */
int
spdk_bdev_nvme_admin_passthru(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF; /* [한국어] 읽기 전용 디스크립터 거부 */
	}

	if (spdk_unlikely(!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_ADMIN))) {
		return -ENOTSUP; /* [한국어] 모듈이 NVME_ADMIN 미지원 */
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->internal.ch = channel;                   /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;                    /* [한국어] 디스크립터 설정 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_ADMIN;     /* [한국어] I/O 타입 */
	bdev_io->u.nvme_passthru.cmd = *cmd;              /* [한국어] NVMe 명령 복사 */
	bdev_io->u.nvme_passthru.buf = buf;               /* [한국어] 데이터 버퍼 */
	bdev_io->u.nvme_passthru.nbytes = nbytes;         /* [한국어] 버퍼 크기 */
	bdev_io->u.nvme_passthru.md_buf = NULL;           /* [한국어] 메타데이터 버퍼 없음 */
	bdev_io->u.nvme_passthru.md_len = 0;              /* [한국어] 메타데이터 길이 0 */

	bdev_io_init(bdev_io, bdev, cb_arg, cb);           /* [한국어] bdev/cb 공통 초기화 */

	bdev_io_submit(bdev_io); /* [한국어] 모듈에 NVME_ADMIN 요청 제출 */
	return 0;
}

/*
 * [한국어]
 * spdk_bdev_nvme_io_passthru - NVMe I/O 명령 패스스루 요청 (단일 버퍼)
 *
 * @desc:   bdev 디스크립터 (쓰기 가능 필수)
 * @ch:     bdev 채널
 * @cmd:    NVMe I/O 명령 구조체
 * @buf:    단일 데이터 버퍼
 * @nbytes: 버퍼 크기
 * @cb:     완료 콜백
 * @cb_arg: 콜백 인자
 * @return: 0=성공, -EBADF/-ENOTSUP/-ENOMEM
 *
 * NVMe I/O 명령을 단일 버퍼와 함께 bdev 모듈에 직접 전달.
 * read-only 디스크립터 사용 불가 — NVMe 명령 파싱 없이 일률 거부.
 *
 * 호출 체인:
 *   상위 모듈 → [이 함수] → bdev_io_submit() → NVMe 드라이버
 */
int
spdk_bdev_nvme_io_passthru(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes,
			   spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		/*
		 * Do not try to parse the NVMe command - we could maybe use bits in the opcode
		 *  to easily determine if the command is a read or write, but for now just
		 *  do not allow io_passthru with a read-only descriptor.
		 */
		/* [한국어] NVMe 명령 파싱 없이 읽기 전용 디스크립터 거부 */
		return -EBADF;
	}

	if (spdk_unlikely(!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_IO))) {
		return -ENOTSUP; /* [한국어] 모듈이 NVME_IO 미지원 */
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->internal.ch = channel;               /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;                /* [한국어] 디스크립터 설정 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_IO;    /* [한국어] I/O 타입 */
	bdev_io->u.nvme_passthru.cmd = *cmd;          /* [한국어] NVMe 명령 복사 */
	bdev_io->u.nvme_passthru.buf = buf;           /* [한국어] 단일 데이터 버퍼 */
	bdev_io->u.nvme_passthru.nbytes = nbytes;     /* [한국어] 버퍼 크기 */
	bdev_io->u.nvme_passthru.md_buf = NULL;       /* [한국어] 메타데이터 버퍼 없음 */
	bdev_io->u.nvme_passthru.md_len = 0;          /* [한국어] 메타데이터 길이 0 */

	bdev_io_init(bdev_io, bdev, cb_arg, cb);       /* [한국어] bdev/cb 공통 초기화 */

	bdev_io_submit(bdev_io); /* [한국어] 모듈에 NVME_IO 요청 제출 */
	return 0;
}

/*
 * [한국어]
 * spdk_bdev_nvme_io_passthru_md - NVMe I/O 명령 패스스루 요청 (메타데이터 포함)
 *
 * @desc:   bdev 디스크립터 (쓰기 가능 필수)
 * @ch:     bdev 채널
 * @cmd:    NVMe I/O 명령 구조체
 * @buf:    데이터 버퍼
 * @nbytes: 데이터 버퍼 크기
 * @md_buf: 메타데이터 버퍼 (PI, DIF 등)
 * @md_len: 메타데이터 버퍼 크기
 * @cb:     완료 콜백
 * @cb_arg: 콜백 인자
 * @return: 0=성공, -EBADF/-ENOTSUP/-ENOMEM
 *
 * 메타데이터(PI/DIF) 버퍼를 별도로 제공하는 NVMe I/O 패스스루.
 * NVME_IO_MD 지원 여부 확인.
 *
 * 호출 체인:
 *   상위 모듈 → [이 함수] → bdev_io_submit() → NVMe 드라이버
 */
int
spdk_bdev_nvme_io_passthru_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes, void *md_buf, size_t md_len,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		/*
		 * Do not try to parse the NVMe command - we could maybe use bits in the opcode
		 *  to easily determine if the command is a read or write, but for now just
		 *  do not allow io_passthru with a read-only descriptor.
		 */
		/* [한국어] 읽기 전용 디스크립터 거부 */
		return -EBADF;
	}

	if (spdk_unlikely(!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_IO_MD))) {
		return -ENOTSUP; /* [한국어] 모듈이 NVME_IO_MD 미지원 */
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->internal.ch = channel;                /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;                 /* [한국어] 디스크립터 설정 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_IO_MD;  /* [한국어] I/O 타입 */
	bdev_io->u.nvme_passthru.cmd = *cmd;           /* [한국어] NVMe 명령 복사 */
	bdev_io->u.nvme_passthru.buf = buf;            /* [한국어] 데이터 버퍼 */
	bdev_io->u.nvme_passthru.nbytes = nbytes;      /* [한국어] 데이터 버퍼 크기 */
	bdev_io->u.nvme_passthru.md_buf = md_buf;      /* [한국어] 메타데이터 버퍼 */
	bdev_io->u.nvme_passthru.md_len = md_len;      /* [한국어] 메타데이터 버퍼 크기 */

	bdev_io_init(bdev_io, bdev, cb_arg, cb);        /* [한국어] bdev/cb 공통 초기화 */

	bdev_io_submit(bdev_io); /* [한국어] 모듈에 NVME_IO_MD 요청 제출 */
	return 0;
}

/*
 * [한국어]
 * spdk_bdev_nvme_iov_passthru_md - NVMe I/O 명령 패스스루 요청 (iovec + 메타데이터)
 *
 * @desc:   bdev 디스크립터 (쓰기 가능 필수)
 * @ch:     bdev 채널
 * @cmd:    NVMe I/O 명령 구조체
 * @iov:    산포 수집 iovec 배열
 * @iovcnt: iov 개수
 * @nbytes: 총 데이터 크기
 * @md_buf: 메타데이터 버퍼 (NULL이면 NVME_IO, 비NULL이면 NVME_IO_MD 지원 필요)
 * @md_len: 메타데이터 버퍼 크기
 * @cb:     완료 콜백
 * @cb_arg: 콜백 인자
 * @return: 0=성공, -EBADF/-ENOTSUP/-ENOMEM
 *
 * iovec + 메타데이터를 함께 사용하는 NVMe I/O 패스스루.
 * md_buf가 있으면 NVME_IO_MD 지원 필요, 없으면 NVME_IO 지원 필요.
 *
 * 호출 체인:
 *   상위 모듈 → [이 함수] → bdev_io_submit() → NVMe 드라이버
 */
int
spdk_bdev_nvme_iov_passthru_md(struct spdk_bdev_desc *desc,
			       struct spdk_io_channel *ch,
			       const struct spdk_nvme_cmd *cmd,
			       struct iovec *iov, int iovcnt, size_t nbytes,
			       void *md_buf, size_t md_len,
			       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		/*
		 * Do not try to parse the NVMe command - we could maybe use bits in the opcode
		 * to easily determine if the command is a read or write, but for now just
		 * do not allow io_passthru with a read-only descriptor.
		 */
		/* [한국어] 읽기 전용 디스크립터 거부 */
		return -EBADF;
	}

	if (md_buf && spdk_unlikely(!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_IO_MD))) {
		return -ENOTSUP; /* [한국어] 메타데이터 있음 + NVME_IO_MD 미지원 */
	} else if (spdk_unlikely(!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_IO))) {
		return -ENOTSUP; /* [한국어] 메타데이터 없음 + NVME_IO 미지원 */
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->internal.ch = channel;                  /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;                   /* [한국어] 디스크립터 설정 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_IOV_MD;   /* [한국어] I/O 타입 */
	bdev_io->u.nvme_passthru.cmd = *cmd;             /* [한국어] NVMe 명령 복사 */
	bdev_io->u.nvme_passthru.iovs = iov;             /* [한국어] 산포 수집 iovec */
	bdev_io->u.nvme_passthru.iovcnt = iovcnt;        /* [한국어] iov 개수 */
	bdev_io->u.nvme_passthru.nbytes = nbytes;        /* [한국어] 총 데이터 크기 */
	bdev_io->u.nvme_passthru.md_buf = md_buf;        /* [한국어] 메타데이터 버퍼 (NULL 가능) */
	bdev_io->u.nvme_passthru.md_len = md_len;        /* [한국어] 메타데이터 크기 */

	bdev_io_init(bdev_io, bdev, cb_arg, cb);          /* [한국어] bdev/cb 공통 초기화 */

	bdev_io_submit(bdev_io); /* [한국어] 모듈에 NVME_IOV_MD 요청 제출 */
	return 0;
}

static void bdev_abort_retry(void *ctx);
static void bdev_abort(struct spdk_bdev_io *parent_io);

/*
 * [한국어]
 * bdev_abort_io_done - ABORT I/O 완료 콜백 (단일 I/O 중단 결과 처리)
 *
 * @bdev_io:  완료된 ABORT bdev_io
 * @success:  ABORT 성공 여부 (모듈이 실제로 중단했는지)
 * @cb_arg:   parent_io (사용자 ABORT 요청)
 *
 * ABORT 성공/실패와 관계없이 중단 대상 I/O(bio_to_abort)가 아직 io_submitted에
 * 존재하면(채널에 남아 있으면) parent_io를 FAILED로 표시.
 * split.outstanding 감소 후 0이 되면 parent_io를 완료.
 *
 * 호출 체인:
 *   bdev_abort_io() → bdev_io_submit() → 모듈 → [이 함수]
 *   → bdev_abort_retry() 또는 bdev_io_complete()
 */
static void
bdev_abort_io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_channel *channel = bdev_io->internal.ch;
	struct spdk_bdev_io *parent_io = cb_arg;
	struct spdk_bdev_io *bio_to_abort, *tmp_io;

	bio_to_abort = bdev_io->u.abort.bio_to_abort;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		/* Check if the target I/O completed in the meantime. */
		TAILQ_FOREACH(tmp_io, &channel->io_submitted, internal.ch_link) {
			if (tmp_io == bio_to_abort) {
				break;
			}
		}

		/* If the target I/O still exists, set the parent to failed. */
		if (tmp_io != NULL) {
			parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	assert(parent_io->internal.f.split);

	parent_io->internal.split.outstanding--;
	if (parent_io->internal.split.outstanding == 0) {
		if (parent_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM) {
			bdev_abort_retry(parent_io);
		} else {
			bdev_io_complete(parent_io);
		}
	}
}

/*
 * [한국어]
 * bdev_abort_io - 단일 I/O 중단 요청 생성 및 제출
 *
 * @desc:         bdev 디스크립터
 * @channel:      bdev 채널
 * @bio_to_abort: 중단할 대상 I/O
 * @cb:           완료 콜백 (bdev_abort_io_done)
 * @cb_arg:       콜백 인자 (parent_io)
 * @return:       0=성공, -ENOTSUP=ABORT/RESET 대상, -ENOMEM=bdev_io 풀 소진
 *
 * 단일 I/O에 대한 ABORT bdev_io를 생성하고 모듈에 제출한다.
 * 대상 I/O가 split된 경우 각 분할 조각에 대해 bdev_abort()를 재귀적으로 호출.
 * ABORT와 RESET 타입 I/O는 중단 불가 (미구현 상태).
 *
 * 호출 체인:
 *   _bdev_abort() → [이 함수] → bdev_io_submit() → 모듈 → bdev_abort_io_done()
 *   또는 [이 함수] → bdev_abort() (split I/O의 경우)
 */
static int
bdev_abort_io(struct spdk_bdev_desc *desc, struct spdk_bdev_channel *channel,
	      struct spdk_bdev_io *bio_to_abort,
	      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;

	if (bio_to_abort->type == SPDK_BDEV_IO_TYPE_ABORT ||
	    bio_to_abort->type == SPDK_BDEV_IO_TYPE_RESET) {
		/* TODO: Abort reset or abort request. */
		/* [한국어] ABORT/RESET 타입 I/O는 현재 중단 불가 (미구현) */
		return -ENOTSUP;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (bdev_io == NULL) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->internal.ch = channel;              /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;               /* [한국어] 디스크립터 설정 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_ABORT;     /* [한국어] I/O 타입 */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);      /* [한국어] bdev/cb 공통 초기화 */

	if (bio_to_abort->internal.f.split) {
		/* [한국어] 대상 I/O가 split된 경우 → 분할 조각들을 일괄 abort */
		assert(bdev_io_should_split(bio_to_abort));
		bdev_io->u.bdev.abort.bio_cb_arg = bio_to_abort;

		/* Parent abort request is not submitted directly, but to manage its
		 * execution add it to the submitted list here.
		 */
		/* [한국어] split abort는 직접 제출하지 않지만 io_submitted에 추가해 추적 */
		bdev_io->internal.submit_tsc = spdk_get_ticks();
		bdev_ch_add_to_io_submitted(bdev_io);

		bdev_abort(bdev_io); /* [한국어] 분할 I/O에 대한 재귀적 abort */

		return 0;
	}

	bdev_io->u.abort.bio_to_abort = bio_to_abort; /* [한국어] 중단 대상 I/O 설정 */

	/* Submit the abort request to the underlying bdev module. */
	/* [한국어] 모듈에 ABORT 요청 제출 */
	bdev_io_submit(bdev_io);

	return 0;
}

/*
 * [한국어]
 * bdev_io_on_tailq - bdev_io가 특정 TAILQ에 존재하는지 선형 검색
 *
 * @bdev_io: 검색할 I/O
 * @tailq:   검색할 큐
 * @return:  true=존재, false=없음
 *
 * abort 시 대상 I/O가 io_accel_exec/io_memory_domain에 있는지 확인하는 데 사용.
 * 이 큐들에 있는 I/O는 abort 불가.
 *
 * 호출 체인:
 *   _bdev_abort() → [이 함수]
 */
static bool
bdev_io_on_tailq(struct spdk_bdev_io *bdev_io, bdev_io_tailq_t *tailq)
{
	struct spdk_bdev_io *iter;

	TAILQ_FOREACH(iter, tailq, internal.link) {
		if (iter == bdev_io) {
			return true; /* [한국어] 큐에 존재 */
		}
	}

	return false; /* [한국어] 큐에 없음 */
}

/*
 * [한국어]
 * _bdev_abort - caller_ctx 기준으로 매칭되는 I/O 목록에 ABORT 제출
 *
 * @parent_io: 사용자의 ABORT bdev_io (bio_cb_arg에 중단 대상 caller_ctx)
 * @return:    매칭되어 abort 제출된 I/O 수
 *
 * io_submitted 큐를 순회해 caller_ctx가 bio_cb_arg와 같고 submit_tsc가
 * ABORT보다 이전인 I/O들을 찾아 각각 bdev_abort_io()를 호출한다.
 *
 * split_outstanding 직접 조작을 피하고 matched_ios를 반환해 호출자가 처리.
 * accel_exec/memory_domain 중인 I/O는 abort 불가 → FAILED.
 *
 * 호출 체인:
 *   bdev_abort() / bdev_abort_retry() → [이 함수] → bdev_abort_io() × matched_ios
 */
static uint32_t
_bdev_abort(struct spdk_bdev_io *parent_io)
{
	struct spdk_bdev_desc *desc = parent_io->internal.desc;
	struct spdk_bdev_channel *channel = parent_io->internal.ch;
	void *bio_cb_arg;
	struct spdk_bdev_io *bio_to_abort;
	uint32_t matched_ios;
	int rc;

	bio_cb_arg = parent_io->u.bdev.abort.bio_cb_arg; /* [한국어] 중단 대상 caller_ctx */

	/* matched_ios is returned and will be kept by the caller.
	 *
	 * This function will be used for two cases, 1) the same cb_arg is used for
	 * multiple I/Os, 2) a single large I/O is split into smaller ones.
	 * Incrementing split_outstanding directly here may confuse readers especially
	 * for the 1st case.
	 *
	 * Completion of I/O abort is processed after stack unwinding. Hence this trick
	 * works as expected.
	 */
	/* [한국어] matched_ios를 반환해 호출자가 split_outstanding을 설정하도록 위임 */
	matched_ios = 0;
	parent_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS; /* [한국어] 초기 상태 SUCCESS */

	TAILQ_FOREACH(bio_to_abort, &channel->io_submitted, internal.ch_link) {
		if (bio_to_abort->internal.caller_ctx != bio_cb_arg) {
			continue; /* [한국어] caller_ctx 불일치 → 스킵 */
		}

		if (bio_to_abort->internal.submit_tsc > parent_io->internal.submit_tsc) {
			/* Any I/O which was submitted after this abort command should be excluded. */
			/* [한국어] ABORT 이후 제출된 I/O는 중단 대상에서 제외 */
			continue;
		}

		/* We can't abort a request that's being pushed/pulled or executed by accel */
		/* [한국어] accel_exec/memory_domain 중인 I/O는 abort 불가 → FAILED */
		if (bdev_io_on_tailq(bio_to_abort, &channel->io_accel_exec) ||
		    bdev_io_on_tailq(bio_to_abort, &channel->io_memory_domain)) {
			parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
			break;
		}

		rc = bdev_abort_io(desc, channel, bio_to_abort, bdev_abort_io_done, parent_io);
		if (rc != 0) {
			if (rc == -ENOMEM) {
				parent_io->internal.status = SPDK_BDEV_IO_STATUS_NOMEM; /* [한국어] ENOMEM 상태 기록 */
			} else {
				parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
			}
			break;
		}
		matched_ios++; /* [한국어] 성공적으로 abort 제출된 I/O 수 */
	}

	return matched_ios;
}

/*
 * [한국어]
 * bdev_abort_retry - ABORT 재시도 콜백 (NOMEM 후 iobuf 반납 시 재진입)
 *
 * @ctx: parent_io (사용자 ABORT bdev_io)
 *
 * _bdev_abort()를 재호출해 ABORT를 재시도한다.
 * matched_ios==0이고 NOMEM이면 다시 큐에 등록.
 * matched_ios==0이고 다른 상태면: 재시도 후 대상 없음 = 타겟 I/O가 완료됨 = SUCCESS.
 *
 * 호출 체인:
 *   bdev_queue_io_wait_with_cb() 완료 → [이 함수] → _bdev_abort()
 */
static void
bdev_abort_retry(void *ctx)
{
	struct spdk_bdev_io *parent_io = ctx;
	uint32_t matched_ios;

	matched_ios = _bdev_abort(parent_io); /* [한국어] ABORT 재시도 */

	if (matched_ios == 0) {
		if (parent_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM) {
			/* [한국어] 여전히 NOMEM → iobuf 반납 시 재시도 */
			bdev_queue_io_wait_with_cb(parent_io, bdev_abort_retry);
		} else {
			/* For retry, the case that no target I/O was found is success
			 * because it means target I/Os completed in the meantime.
			 */
			/* [한국어] 재시도 후 대상 없음 → 타겟이 이미 완료됨 → SUCCESS */
			bdev_io_complete(parent_io);
		}
		return;
	}

	/* Use split_outstanding to manage the progress of aborting I/Os. */
	/* [한국어] matched_ios개의 abort 완료를 추적하기 위해 split_outstanding 설정 */
	parent_io->internal.f.split = true;
	parent_io->internal.split.outstanding = matched_ios;
}

/*
 * [한국어]
 * bdev_abort_unsubmitted_io - iobuf 대기 큐의 미제출 I/O abort 시도
 *
 * @parent_io: 사용자 ABORT bdev_io
 * @return:    true=미제출 I/O를 abort했음, false=없음
 *
 * io_submitted에 없는 I/O(iobuf 대기 중인 I/O)를 bio_cb_arg 기준으로 찾아 abort한다.
 *
 * 호출 체인:
 *   bdev_abort() → [이 함수] → bdev_abort_unsubmitted_buf_io()
 */
static bool
bdev_abort_unsubmitted_io(struct spdk_bdev_io *parent_io)
{
	struct spdk_bdev_mgmt_channel *mgmt_ch;
	void *bio_cb_arg;

	mgmt_ch = parent_io->internal.ch->shared_resource->mgmt_ch; /* [한국어] 관리 채널 가져오기 */
	bio_cb_arg = parent_io->u.bdev.abort.bio_cb_arg;             /* [한국어] 중단 대상 caller_ctx */

	return bdev_abort_unsubmitted_buf_io(mgmt_ch, bio_cb_arg);
	/* [한국어] iobuf 대기 큐에서 bio_cb_arg 매칭 I/O abort */
}

/*
 * [한국어]
 * bdev_abort - ABORT 처리 진입점 (submitted + unsubmitted I/O 처리)
 *
 * @parent_io: 사용자 ABORT bdev_io
 *
 * io_submitted 큐의 I/O를 abort하고 (매칭 없으면) iobuf 대기 I/O도 abort 시도.
 * matched_ios > 0이면 split_outstanding 패턴으로 완료 추적.
 * matched_ios == 0이면:
 *  - NOMEM: 재시도 큐에 등록
 *  - unsubmitted에서 찾음: SUCCESS
 *  - 아무것도 없음: FAILED
 *
 * 호출 체인:
 *   spdk_bdev_abort() / bdev_abort_io() (split) → [이 함수]
 *   → _bdev_abort() → bdev_abort_io() × N
 */
static void
bdev_abort(struct spdk_bdev_io *parent_io)
{
	uint32_t matched_ios;

	matched_ios = _bdev_abort(parent_io); /* [한국어] submitted I/O abort 시도 */

	if (matched_ios == 0) {
		if (parent_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM) {
			/* [한국어] NOMEM → iobuf 반납 시 재시도 */
			bdev_queue_io_wait_with_cb(parent_io, bdev_abort_retry);
		} else if (bdev_abort_unsubmitted_io(parent_io)) {
			/* [한국어] iobuf 대기 I/O abort 성공 → SUCCESS */
			parent_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
			bdev_io_complete(parent_io);
		} else {
			/* The case the no target I/O was found is failure. */
			/* [한국어] 대상 I/O 없음 → FAILED */
			parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
			bdev_io_complete(parent_io);
		}
		return;
	}

	/* Use split_outstanding to manage the progress of aborting I/Os. */
	/* [한국어] matched_ios개의 abort 완료를 추적하기 위해 split_outstanding 설정 */
	parent_io->internal.f.split = true;
	parent_io->internal.split.outstanding = matched_ios;
}

/*
 * [한국어]
 * spdk_bdev_abort - caller_ctx 기준으로 I/O 중단 요청 (외부 API)
 *
 * @desc:        bdev 디스크립터
 * @ch:          bdev 채널
 * @bio_cb_arg:  중단할 I/O의 caller_ctx (NULL 불가)
 * @cb:          완료 콜백
 * @cb_arg:      콜백 인자
 * @return:      0=성공, -EINVAL=bio_cb_arg NULL, -ENOTSUP=ABORT 미지원, -ENOMEM
 *
 * bio_cb_arg과 동일한 caller_ctx를 가진 모든 미완료 I/O를 중단한다.
 * ABORT 타입의 bdev_io를 할당하고 bdev_abort()를 통해 io_submitted 큐를 순회.
 * ABORT I/O는 직접 모듈 제출하지 않고 io_submitted에만 등록해 추적.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → bdev_abort() → _bdev_abort() → bdev_abort_io() × N
 */
int
spdk_bdev_abort(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *bio_cb_arg,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);
	struct spdk_bdev_io *bdev_io;

	if (bio_cb_arg == NULL) {
		return -EINVAL; /* [한국어] NULL caller_ctx 거부 */
	}

	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ABORT)) {
		return -ENOTSUP; /* [한국어] 모듈이 ABORT 미지원 */
	}

	bdev_io = bdev_channel_get_io(channel);
	if (bdev_io == NULL) {
		return -ENOMEM; /* [한국어] bdev_io 풀 소진 */
	}

	bdev_io->internal.ch = channel;                  /* [한국어] 채널 설정 */
	bdev_io->internal.desc = desc;                   /* [한국어] 디스크립터 설정 */
	bdev_io->internal.submit_tsc = spdk_get_ticks(); /* [한국어] 제출 시각 기록 (타임스탬프 비교에 사용) */
	bdev_io->type = SPDK_BDEV_IO_TYPE_ABORT;         /* [한국어] I/O 타입 */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);          /* [한국어] bdev/cb 공통 초기화 */

	bdev_io->u.bdev.abort.bio_cb_arg = bio_cb_arg;   /* [한국어] 중단 대상 caller_ctx */

	/* Parent abort request is not submitted directly, but to manage its execution,
	 * add it to the submitted list here.
	 */
	/* [한국어] ABORT I/O는 모듈 직접 제출 없이 io_submitted에만 등록해 추적 */
	bdev_ch_add_to_io_submitted(bdev_io);

	bdev_abort(bdev_io); /* [한국어] 실제 abort 처리 시작 */

	return 0;
}

/*
 * [한국어]
 * spdk_bdev_queue_io_wait - bdev_io 풀 소진 시 재시도 대기 큐에 등록
 *
 * @bdev:  대상 bdev
 * @ch:    bdev 채널
 * @entry: 대기 항목 (entry->bdev가 bdev와 일치해야 함)
 * @return: 0=성공, -EINVAL=bdev 불일치 또는 per-thread 캐시 남음
 *
 * bdev_io 할당 실패(-ENOMEM) 시 호출자가 이 함수로 대기 큐에 등록한다.
 * bdev_io가 반납되면 대기 큐의 첫 항목을 깨워 재시도한다.
 * dep_unblock=true인 항목은 큐 앞에 삽입해 우선 처리.
 * per-thread 캐시에 아직 bdev_io가 남아있으면 등록 거부.
 *
 * 호출 체인:
 *   bdev_channel_get_io() 실패 시 → [이 함수]
 *   → bdev_io 반납 시 bdev_io_put_buf() 내에서 대기 큐 깨움
 */
int
spdk_bdev_queue_io_wait(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			struct spdk_bdev_io_wait_entry *entry)
{
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);
	struct spdk_bdev_mgmt_channel *mgmt_ch = channel->shared_resource->mgmt_ch;

	if (bdev != entry->bdev) {
		SPDK_ERRLOG("bdevs do not match\n");
		return -EINVAL; /* [한국어] entry->bdev가 bdev와 다름 */
	}

	if (mgmt_ch->per_thread_cache_count > 0) {
		SPDK_ERRLOG("Cannot queue io_wait if spdk_bdev_io available in per-thread cache\n");
		return -EINVAL; /* [한국어] per-thread 캐시에 bdev_io 남아 있음 — 먼저 소진해야 함 */
	}

	if (entry->dep_unblock) {
		/* [한국어] dep_unblock: 종속성에 의한 해제 대기 → 큐 앞에 삽입 */
		TAILQ_INSERT_HEAD(&mgmt_ch->io_wait_queue, entry, link);
	} else {
		/* [한국어] 일반 대기 → 큐 뒤에 삽입 */
		TAILQ_INSERT_TAIL(&mgmt_ch->io_wait_queue, entry, link);
	}
	return 0;
}

/*
 * [한국어]
 * bdev_io_update_io_stat - I/O 완료 시 채널 통계 갱신 (IOPS/bytes/latency/error)
 *
 * @bdev_io:  완료된 bdev_io
 * @tsc_diff: 완료 latency (완료 tsc - submit tsc)
 *
 * 성공 시 I/O 타입별로 바이트 수, 연산 횟수, 레이턴시(누적/max/min)를 갱신.
 * ZCOPY는 start 단계만 통계에 포함 (commit 단계 제외).
 * 오류 시 bdev->internal.stat의 io_error 테이블을 spinlock 보호 후 증가.
 * VTune 빌드 시 추가 인스트루먼테이션 데이터 전송.
 *
 * 호출 체인:
 *   bdev_io_complete() → [이 함수]
 */
static inline void
bdev_io_update_io_stat(struct spdk_bdev_io *bdev_io, uint64_t tsc_diff)
{
	enum spdk_bdev_io_status io_status = bdev_io->internal.status;   /* [한국어] 완료 상태 */
	struct spdk_bdev_io_stat *io_stat = bdev_io->internal.ch->stat;  /* [한국어] 채널 통계 구조체 */
	uint64_t num_blocks = bdev_io->u.bdev.num_blocks;                /* [한국어] 처리 블록 수 */
	uint32_t blocklen = bdev_io->bdev->blocklen;                     /* [한국어] 블록 크기 (bytes) */

	if (spdk_likely(io_status == SPDK_BDEV_IO_STATUS_SUCCESS)) {
		/* [한국어] 성공 → I/O 타입별 통계 갱신 */
		switch (bdev_io->type) {
		case SPDK_BDEV_IO_TYPE_READ:
			io_stat->bytes_read += num_blocks * blocklen;      /* [한국어] 읽은 바이트 누적 */
			io_stat->num_read_ops++;                           /* [한국어] 읽기 연산 횟수 */
			io_stat->read_latency_ticks += tsc_diff;           /* [한국어] 읽기 레이턴시 누적 */
			if (io_stat->max_read_latency_ticks < tsc_diff) {
				io_stat->max_read_latency_ticks = tsc_diff; /* [한국어] 최대 읽기 레이턴시 갱신 */
			}
			if (io_stat->min_read_latency_ticks > tsc_diff) {
				io_stat->min_read_latency_ticks = tsc_diff; /* [한국어] 최소 읽기 레이턴시 갱신 */
			}
			break;
		case SPDK_BDEV_IO_TYPE_WRITE:
			io_stat->bytes_written += num_blocks * blocklen;   /* [한국어] 쓴 바이트 누적 */
			io_stat->num_write_ops++;                          /* [한국어] 쓰기 연산 횟수 */
			io_stat->write_latency_ticks += tsc_diff;          /* [한국어] 쓰기 레이턴시 누적 */
			if (io_stat->max_write_latency_ticks < tsc_diff) {
				io_stat->max_write_latency_ticks = tsc_diff; /* [한국어] 최대 쓰기 레이턴시 갱신 */
			}
			if (io_stat->min_write_latency_ticks > tsc_diff) {
				io_stat->min_write_latency_ticks = tsc_diff; /* [한국어] 최소 쓰기 레이턴시 갱신 */
			}
			break;
		case SPDK_BDEV_IO_TYPE_UNMAP:
			io_stat->bytes_unmapped += num_blocks * blocklen;  /* [한국어] UNMAP 바이트 누적 */
			io_stat->num_unmap_ops++;                          /* [한국어] UNMAP 연산 횟수 */
			io_stat->unmap_latency_ticks += tsc_diff;          /* [한국어] UNMAP 레이턴시 누적 */
			if (io_stat->max_unmap_latency_ticks < tsc_diff) {
				io_stat->max_unmap_latency_ticks = tsc_diff; /* [한국어] 최대 UNMAP 레이턴시 갱신 */
			}
			if (io_stat->min_unmap_latency_ticks > tsc_diff) {
				io_stat->min_unmap_latency_ticks = tsc_diff; /* [한국어] 최소 UNMAP 레이턴시 갱신 */
			}
			break;
		case SPDK_BDEV_IO_TYPE_ZCOPY:
			/* Track the data in the start phase only */
			/* [한국어] ZCOPY는 start 단계(버퍼 획득)만 통계에 반영 */
			if (bdev_io->u.bdev.zcopy.start) {
				if (bdev_io->u.bdev.zcopy.populate) {
					/* [한국어] populate=1: 기존 데이터 읽어옴 → 읽기 통계 */
					io_stat->bytes_read += num_blocks * blocklen;
					io_stat->num_read_ops++;
					io_stat->read_latency_ticks += tsc_diff;
					if (io_stat->max_read_latency_ticks < tsc_diff) {
						io_stat->max_read_latency_ticks = tsc_diff;
					}
					if (io_stat->min_read_latency_ticks > tsc_diff) {
						io_stat->min_read_latency_ticks = tsc_diff;
					}
				} else {
					/* [한국어] populate=0: 빈 버퍼 제공 (순수 쓰기) → 쓰기 통계 */
					io_stat->bytes_written += num_blocks * blocklen;
					io_stat->num_write_ops++;
					io_stat->write_latency_ticks += tsc_diff;
					if (io_stat->max_write_latency_ticks < tsc_diff) {
						io_stat->max_write_latency_ticks = tsc_diff;
					}
					if (io_stat->min_write_latency_ticks > tsc_diff) {
						io_stat->min_write_latency_ticks = tsc_diff;
					}
				}
			}
			break;
		case SPDK_BDEV_IO_TYPE_COPY:
			io_stat->bytes_copied += num_blocks * blocklen;    /* [한국어] COPY 바이트 누적 */
			io_stat->num_copy_ops++;                           /* [한국어] COPY 연산 횟수 */
			bdev_io->internal.ch->stat->copy_latency_ticks += tsc_diff; /* [한국어] COPY 레이턴시 누적 */
			if (io_stat->max_copy_latency_ticks < tsc_diff) {
				io_stat->max_copy_latency_ticks = tsc_diff; /* [한국어] 최대 COPY 레이턴시 갱신 */
			}
			if (io_stat->min_copy_latency_ticks > tsc_diff) {
				io_stat->min_copy_latency_ticks = tsc_diff; /* [한국어] 최소 COPY 레이턴시 갱신 */
			}
			break;
		default:
			break;
		}
	} else if (io_status <= SPDK_BDEV_IO_STATUS_FAILED && io_status >= SPDK_MIN_BDEV_IO_STATUS) {
		/* [한국어] 오류 상태 → bdev->internal.stat의 error_status 카운터 증가
		 * -io_status - 1: FAILED=-1 → index 0, MISCOMPARE=-2 → index 1 등 */
		io_stat = bdev_io->bdev->internal.stat;
		assert(io_stat->io_error != NULL);

		spdk_spin_lock(&bdev_io->bdev->internal.spinlock); /* [한국어] spinlock: error_status 동시 접근 보호 */
		io_stat->io_error->error_status[-io_status - 1]++;  /* [한국어] 오류 코드별 카운터 증가 */
		spdk_spin_unlock(&bdev_io->bdev->internal.spinlock);
	}

#ifdef SPDK_CONFIG_VTUNE
	/* [한국어] VTune 인스트루먼테이션: 주기적으로 통계 스냅샷을 VTune에 전송 */
	uint64_t now_tsc = spdk_get_ticks();
	if (now_tsc > (bdev_io->internal.ch->start_tsc + bdev_io->internal.ch->interval_tsc)) {
		/* [한국어] 보고 주기 초과 → 이전 스냅샷과 차분 계산 */
		uint64_t data[5];
		struct spdk_bdev_io_stat *prev_stat = bdev_io->internal.ch->prev_stat;

		data[0] = io_stat->num_read_ops - prev_stat->num_read_ops;   /* [한국어] 읽기 연산 차분 */
		data[1] = io_stat->bytes_read - prev_stat->bytes_read;       /* [한국어] 읽기 바이트 차분 */
		data[2] = io_stat->num_write_ops - prev_stat->num_write_ops; /* [한국어] 쓰기 연산 차분 */
		data[3] = io_stat->bytes_written - prev_stat->bytes_written; /* [한국어] 쓰기 바이트 차분 */
		data[4] = bdev_io->bdev->fn_table->get_spin_time ?
			  bdev_io->bdev->fn_table->get_spin_time(spdk_bdev_io_get_io_channel(bdev_io)) : 0;
		/* [한국어] 스핀 타임 (지원하는 모듈만) */

		__itt_metadata_add(g_bdev_mgr.domain, __itt_null, bdev_io->internal.ch->handle,
				   __itt_metadata_u64, 5, data);
		/* [한국어] VTune에 5개 메타데이터 전송 */

		memcpy(prev_stat, io_stat, sizeof(struct spdk_bdev_io_stat)); /* [한국어] 스냅샷 갱신 */
		bdev_io->internal.ch->start_tsc = now_tsc;                    /* [한국어] 보고 주기 갱신 */
	}
#endif
}

/*
 * [한국어]
 * _bdev_io_complete - 사용자 완료 콜백 직전 최종 처리
 *
 * @ctx: 완료할 bdev_io
 *
 * accel_sequence가 사용 중이면서 실패 상태이면 시퀀스를 abort.
 * 콜백 및 스레드 일치를 assert로 검증 후 cb()를 호출.
 *
 * 호출 체인:
 *   bdev_io_complete() → [이 함수] → bdev_io->internal.cb()
 */
static inline void
_bdev_io_complete(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;

	if (spdk_unlikely(bdev_io_use_accel_sequence(bdev_io))) {
		/* [한국어] accel 시퀀스 사용 중 + 실패 → 시퀀스 abort */
		assert(bdev_io->internal.status != SPDK_BDEV_IO_STATUS_SUCCESS);
		spdk_accel_sequence_abort(bdev_io->internal.accel_sequence);
	}

	assert(bdev_io->internal.cb != NULL);                                /* [한국어] 콜백 NULL 검증 */
	assert(spdk_get_thread() == spdk_bdev_io_get_thread(bdev_io));       /* [한국어] 채널 스레드 일치 검증 */

	bdev_io->internal.cb(bdev_io, bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS,
			     bdev_io->internal.caller_ctx);
	/* [한국어] 최종 사용자 완료 콜백 호출 */
}

/*
 * [한국어]
 * bdev_io_complete - 실제 사용자 cb_fn 호출 직전 단계의 후처리.
 *
 * @ctx: bdev_io 포인터 (msg 라우팅 호환을 위한 void*).
 *
 * 후처리 단계:
 *   1) submit context에서 호출되었으면 재귀 방지 위해 msg로 defer.
 *   2) latency = (현재 tsc - submit_tsc) 계산.
 *   3) io_submitted 큐에서 제거, QD--.
 *   4) trace_tpoint TRACE_BDEV_IO_DONE 기록.
 *   5) histogram이 활성이면 latency를 적절 bucket에 tally.
 *   6) bdev_io_update_io_stat — IOPS/bytes/error 카운터 갱신.
 *   7) _bdev_io_complete으로 사용자 cb_fn 호출.
 *
 * 실행 컨텍스트: 채널 소유 spdk_thread. submit 컨텍스트에서 호출되었다면
 *  내부에서 자체 msg로 defer하여 호출자에게 즉시 반환.
 */
static inline void
bdev_io_complete(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct spdk_bdev_channel *bdev_ch = bdev_io->internal.ch;
	/* [한국어] 채널 ctx. */
	uint64_t tsc, tsc_diff;
	/* [한국어] 완료 시각과 submit~complete latency. */

	if (spdk_unlikely(bdev_io->internal.f.in_submit_request)) {
		/*
		 * Defer completion to avoid potential infinite recursion if the
		 * user's completion callback issues a new I/O.
		 */
		/* [한국어] 모듈이 submit_request 내부에서 즉시 complete를 호출하는 경우 — 사용자 cb가
		 *  또 새 I/O를 발행하면 재귀 무한루프 가능. msg로 한 단계 defer 처리. */
		spdk_thread_send_msg(spdk_bdev_io_get_thread(bdev_io),
				     bdev_io_complete, bdev_io);
		return;
	}

	tsc = spdk_get_ticks();
	/* [한국어] 현재 시각 tsc. */
	tsc_diff = tsc - bdev_io->internal.submit_tsc;
	/* [한국어] latency (tsc 단위). spdk_get_ticks_hz로 ns 변환 가능. */

	bdev_ch_remove_from_io_submitted(bdev_io);
	/* [한국어] io_submitted 큐에서 제거 + QD--. */
	spdk_trace_record_tsc(tsc, TRACE_BDEV_IO_DONE, bdev_ch->trace_id, 0, (uintptr_t)bdev_io,
			      bdev_io->internal.caller_ctx, bdev_ch->queue_depth);
	/* [한국어] trace tool 이벤트 — BDEV_IO_DONE. */

	if (bdev_ch->histogram) {
		/* [한국어] histogram 활성 시 — 모든 I/O 타입 또는 특정 타입만 집계. */
		if (bdev_io->bdev->internal.histogram_io_type == 0 ||
		    bdev_io->bdev->internal.histogram_io_type == bdev_io->type) {
			/*
			 * Tally all I/O types if the histogram_io_type is set to 0.
			 */
			spdk_histogram_data_tally(bdev_ch->histogram, tsc_diff);
			/* [한국어] latency를 buckets[log2 bucket]에 가산. */
		}
	}

	bdev_io_update_io_stat(bdev_io, tsc_diff);
	/* [한국어] IOPS/bytes/error 카운터 갱신 + 통계의 max latency 추적. */
	_bdev_io_complete(bdev_io);
	/* [한국어] 사용자 cb_fn(bdev_io, success, cb_arg) 호출. */
}

/* The difference between this function and bdev_io_complete() is that this should be called to
 * complete IOs that haven't been submitted via bdev_io_submit(), as they weren't added onto the
 * io_submitted list and don't have submit_tsc updated.
 */
/*
 * [한국어]
 * bdev_io_complete_unsubmitted - bdev_io_submit()을 거치지 않은 I/O의 완료 처리
 *
 * @bdev_io: 완료할 bdev_io (미제출 상태, 항상 실패)
 *
 * io_submitted에 등록되지 않고 submit_tsc도 없는 I/O(예: 검증 실패, 조기 거부)를
 * 완료 처리한다. 항상 실패 상태여야 하며, spdk_thread_send_msg()로 비동기 완료.
 * submit context 여부에 관계없이 항상 메시지로 defer (오류 경로이므로 성능 무관).
 *
 * 호출 체인:
 *   bdev_io_submit() 이전 오류 경로 → [이 함수] → spdk_thread_send_msg()
 *   → _bdev_io_complete()
 */
static inline void
bdev_io_complete_unsubmitted(struct spdk_bdev_io *bdev_io)
{
	/* Since the IO hasn't been submitted it's bound to be failed */
	assert(bdev_io->internal.status != SPDK_BDEV_IO_STATUS_SUCCESS); /* [한국어] 미제출 = 반드시 실패 상태 */

	/* At this point we don't know if the IO is completed from submission context or not, but,
	 * since this is an error path, we can always do an spdk_thread_send_msg(). */
	/* [한국어] submit context 여부 불확실 → 안전하게 메시지 큐를 통해 defer */
	spdk_thread_send_msg(spdk_bdev_io_get_thread(bdev_io),
			     _bdev_io_complete, bdev_io);
}

static void bdev_destroy_cb(void *io_device);

/*
 * [한국어]
 * _bdev_reset_complete - RESET I/O의 채널 참조 반납 및 최종 완료
 *
 * @ctx: 완료할 RESET bdev_io
 *
 * bdev_start_reset()에서 획득한 채널 참조를 반납하고 bdev_io_complete()로 완료.
 * queued_resets 처리 후 각 스레드에서 이 함수가 호출됨.
 *
 * 호출 체인:
 *   bdev_reset_complete() / spdk_thread_send_msg() → [이 함수] → bdev_io_complete()
 */
static inline void
_bdev_reset_complete(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;

	/* Put the channel reference we got in submission. */
	assert(bdev_io->u.reset.ch_ref != NULL); /* [한국어] ch_ref는 bdev_start_reset에서 획득됨 */
	spdk_put_io_channel(bdev_io->u.reset.ch_ref); /* [한국어] 채널 참조 반납 */
	bdev_io->u.reset.ch_ref = NULL;               /* [한국어] 참조 정리 */

	bdev_io_complete(bdev_io); /* [한국어] 사용자 콜백 호출 */
}

/*
 * [한국어]
 * bdev_reset_complete - 모든 채널 unfreeze 완료 후 queued_resets 처리
 *
 * @bdev:   대상 bdev
 * @_ctx:   현재 RESET bdev_io
 * @status: 항상 0
 *
 * 완료된 RESET의 상태를 queued_resets에 대기 중인 모든 RESET I/O에 전파하고
 * 각 스레드에 _bdev_reset_complete 메시지를 보낸다.
 * bdev 제거 중이고 open_descs가 없으면 io_device 해제 시작.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel(unfreeze) 완료 → [이 함수]
 *   → spdk_thread_send_msg(_bdev_reset_complete) × N + _bdev_reset_complete(현재)
 *   → spdk_io_device_unregister(bdev_destroy_cb) (필요 시)
 */
static void
bdev_reset_complete(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_io *bdev_io = _ctx;
	bdev_io_tailq_t queued_resets;
	struct spdk_bdev_io *queued_reset;

	assert(bdev_io == bdev->internal.reset_in_progress); /* [한국어] 현재 활성 RESET 검증 */

	TAILQ_INIT(&queued_resets); /* [한국어] 로컬 큐 초기화 */

	/* [한국어] queued_resets를 로컬로 이전 — spinlock 보호 */
	spdk_spin_lock(&bdev->internal.spinlock);
	TAILQ_SWAP(&bdev->internal.queued_resets, &queued_resets,
		   spdk_bdev_io, internal.link);
	bdev->internal.reset_in_progress = NULL; /* [한국어] 활성 RESET 완료 표시 */
	spdk_spin_unlock(&bdev->internal.spinlock);

	/* [한국어] queued_resets의 각 RESET에 현재 RESET 상태 전파 */
	while (!TAILQ_EMPTY(&queued_resets)) {
		queued_reset = TAILQ_FIRST(&queued_resets);
		TAILQ_REMOVE(&queued_resets, queued_reset, internal.link);
		queued_reset->internal.status = bdev_io->internal.status; /* [한국어] 상태 동일하게 설정 */
		spdk_thread_send_msg(spdk_bdev_io_get_thread(queued_reset),
				     _bdev_reset_complete, queued_reset);
		/* [한국어] 각 queued_reset을 해당 스레드에서 완료 */
	}

	_bdev_reset_complete(bdev_io); /* [한국어] 현재 RESET 완료 */

	if (bdev->internal.status == SPDK_BDEV_STATUS_REMOVING &&
	    TAILQ_EMPTY(&bdev->internal.open_descs)) {
		/* [한국어] bdev 제거 중 + open_descs 없음 → io_device 해제 시작 */
		spdk_io_device_unregister(__bdev_to_io_dev(bdev), bdev_destroy_cb);
	}
}

/*
 * [한국어]
 * bdev_unfreeze_channel - RESET 완료 후 채널 동결 해제
 *
 * @i:    채널 이터레이터
 * @bdev: 대상 bdev
 * @_ch:  현재 채널
 * @_ctx: 사용되지 않음
 *
 * BDEV_CH_RESET_IN_PROGRESS 플래그를 해제해 채널이 새 I/O를 받을 수 있도록 한다.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → [이 함수] × 채널 수 → bdev_reset_complete()
 */
static void
bdev_unfreeze_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
		      struct spdk_io_channel *_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);

	ch->flags &= ~BDEV_CH_RESET_IN_PROGRESS; /* [한국어] 채널 동결 해제 */

	spdk_bdev_for_each_channel_continue(i, 0); /* [한국어] 이터레이터 계속 */
}

/*
 * [한국어]
 * bdev_io_complete_sequence_cb - accel 시퀀스 실행 완료 후 I/O 완료 처리
 *
 * @ctx:    bdev_io 포인터
 * @status: accel 시퀀스 실행 결과 (0=성공)
 *
 * accel 시퀀스가 성공적으로 완료되면 has_accel_sequence=false로 리셋.
 * 시퀀스 실패 시 FAILED 상태로 bdev_io 완료.
 * bdev_io_exec_sequence()의 완료 콜백.
 *
 * 호출 체인:
 *   spdk_bdev_io_complete() → bdev_io_exec_sequence() → [이 함수] → bdev_io_complete()
 */
static void
bdev_io_complete_sequence_cb(void *ctx, int status)
{
	struct spdk_bdev_io *bdev_io = ctx;

	/* u.bdev.accel_sequence should have already been cleared at this point */
	assert(bdev_io->u.bdev.accel_sequence == NULL);           /* [한국어] 시퀀스는 이미 실행 완료 후 NULL */
	assert(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS); /* [한국어] 시퀀스 실행 전 SUCCESS 보장 */
	bdev_io->internal.f.has_accel_sequence = false;           /* [한국어] accel 시퀀스 플래그 해제 */

	if (spdk_unlikely(status != 0)) {
		SPDK_ERRLOG("Failed to execute accel sequence, status=%d\n", status);
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED; /* [한국어] 시퀀스 실패 → FAILED */
	}

	bdev_io_complete(bdev_io); /* [한국어] 실제 완료 처리 */
}

/*
 * [한국어]
 * spdk_bdev_io_complete - bdev 모듈이 비동기 I/O 완료를 SPDK 코어에 통지.
 *
 * @bdev_io: 완료된 I/O. (모듈이 submit_request에서 받았던 동일 포인터).
 * @status : 완료 상태 — SUCCESS / FAILED / NOMEM / ABORTED / NVME_ERROR / SCSI_ERROR / AIO_ERROR / MISCOMPARE 등.
 *
 * 핵심 동작:
 *   1) 상태 머신: PENDING → status 전이. PENDING이 아니었다면 중복 완료(버그).
 *   2) RESET 케이스는 별도 경로 — 모든 채널에 unfreeze 메시지 보내고 콜백.
 *   3) outstanding 카운터 감소.
 *   4) 성공이고 accel sequence가 남아 있으면 sequence 실행 후 진짜 완료.
 *      성공이고 bounce buffer를 썼다면 push 후 진짜 완료.
 *   5) 실패가 NOMEM이면 재시도 큐로 보내고 cb 호출 보류.
 *   6) 일반 경로: bdev_io_complete로 진짜 cb_fn 호출.
 *
 * 실행 컨텍스트: 모듈이 자신의 poller에서 호출 — 채널 소유 spdk_thread.
 *   (모듈이 cross-thread로 호출하면 안 됨 — bdev 코어가 thread 가정.)
 *
 * 호출자(caller): 모든 bdev 모듈의 완료 path (bdev_nvme_check_io_completions,
 *   bdev_aio_poll, raid bdev, lvol, malloc bdev 등).
 * 호출처(callee): bdev_io_complete (일반 케이스), bdev_io_exec_sequence (accel 남음),
 *   _bdev_io_push_bounce_data_buffer (bounce push), _bdev_io_handle_no_mem (NOMEM),
 *   spdk_bdev_for_each_channel (RESET).
 */
void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	/* [한국어] 대상 bdev. */
	struct spdk_bdev_channel *bdev_ch = bdev_io->internal.ch;
	/* [한국어] I/O가 발행된 채널 — outstanding 카운터/통계 갱신 대상. */
	struct spdk_bdev_shared_resource *shared_resource = bdev_ch->shared_resource;
	/* [한국어] thread × io_device 공유 자원 — outstanding 카운터, nomem 큐 보유. */

	if (spdk_unlikely(bdev_io->internal.status != SPDK_BDEV_IO_STATUS_PENDING)) {
		/* [한국어] 모듈이 두 번 complete 호출 등 라이프사이클 위반 — 명확히 로그하고 assert. */
		SPDK_ERRLOG("Unexpected completion on IO from %s module, status was %s\n",
			    spdk_bdev_get_module_name(bdev),
			    bdev_io_status_get_string(bdev_io->internal.status));
		assert(false);
	}
	bdev_io->internal.status = status;
	/* [한국어] PENDING → 모듈이 보고한 status. 이 이후 cb_fn 호출 시 success=(status==SUCCESS)로 결정. */

	if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_RESET)) {
		/* [한국어] RESET 완료는 특별 경로 — 모든 채널의 RESET_IN_PROGRESS flag 해제 필요. */
		assert(bdev_io == bdev->internal.reset_in_progress);
		/* [한국어] bdev 단위로 reset은 한 번에 하나만 가능 — 같은 객체여야 함. */
		spdk_bdev_for_each_channel(bdev, bdev_unfreeze_channel, bdev_io,
					   bdev_reset_complete);
		/* [한국어] 각 채널에 unfreeze 메시지 보냄 → 마지막에 bdev_reset_complete가 cb 호출. */
		return;
	} else {
		bdev_io_decrement_outstanding(bdev_ch, shared_resource);
		/* [한국어] in-flight 카운터 감소 — nomem threshold 검사용. */
		if (spdk_likely(status == SPDK_BDEV_IO_STATUS_SUCCESS)) {
			/* [한국어] 성공 케이스 — 추가 후처리 필요한지 점검. */
			if (bdev_io_needs_sequence_exec(bdev_io)) {
				/* [한국어] accel sequence가 아직 실행 안 됐다면 — 보통 READ 후 decrypt/decompress 등. */
				bdev_io_exec_sequence(bdev_io, bdev_io_complete_sequence_cb);
				return;
				/* [한국어] sequence 완료 콜백에서 실제 complete 진행. */
			} else if (spdk_unlikely(bdev_io->internal.f.has_bounce_buf &&
						 !bdev_io_use_accel_sequence(bdev_io))) {
				/* [한국어] bounce buffer를 썼다면 — 사용자 buffer로 push 필요. */
				_bdev_io_push_bounce_data_buffer(bdev_io,
								 _bdev_io_complete_push_bounce_done);
				/* bdev IO will be completed in the callback */
				return;
				/* [한국어] push 완료 콜백에서 진짜 complete. */
			}
		}

		if (spdk_unlikely(_bdev_io_handle_no_mem(bdev_io, BDEV_IO_RETRY_STATE_SUBMIT))) {
			/* [한국어] NOMEM 상태면 재시도 큐로 보내고 true 반환 → cb 호출 보류. */
			return;
		}
	}

	bdev_io_complete(bdev_io);
	/* [한국어] 진짜 사용자 cb_fn 호출 단계로 진입. */
}

/*
 * [한국어]
 * spdk_bdev_io_set_scsi_status - SCSI 상태 코드를 bdev_io에 설정
 *
 * @bdev_io: 상태를 설정할 bdev_io
 * @sc:      SCSI Status Code (GOOD=성공, CHECK_CONDITION=오류 등)
 * @sk:      Sense Key (오류 카테고리)
 * @asc:     Additional Sense Code
 * @ascq:    Additional Sense Code Qualifier
 * @return:  SPDK_BDEV_IO_STATUS_SUCCESS 또는 SPDK_BDEV_IO_STATUS_SCSI_ERROR
 *
 * SCSI 오류 정보를 bdev_io에 저장하고 대응하는 bdev_io 상태를 반환한다.
 * sc==GOOD이면 SUCCESS, 그 외는 SCSI_ERROR + error 필드에 상세 정보 저장.
 *
 * 호출 체인:
 *   spdk_bdev_io_complete_scsi_status() → [이 함수]
 */
spdk_bdev_io_status_t
spdk_bdev_io_set_scsi_status(struct spdk_bdev_io *bdev_io, enum spdk_scsi_status sc,
			     enum spdk_scsi_sense sk, uint8_t asc, uint8_t ascq)
{
	enum spdk_bdev_io_status status;

	if (sc == SPDK_SCSI_STATUS_GOOD) {
		status = SPDK_BDEV_IO_STATUS_SUCCESS; /* [한국어] SCSI GOOD → SUCCESS */
	} else {
		/* [한국어] SCSI 오류 → 상세 정보 저장 후 SCSI_ERROR */
		status = SPDK_BDEV_IO_STATUS_SCSI_ERROR;
		bdev_io->internal.error.scsi.sc = sc;     /* [한국어] Status Code */
		bdev_io->internal.error.scsi.sk = sk;     /* [한국어] Sense Key */
		bdev_io->internal.error.scsi.asc = asc;   /* [한국어] Additional Sense Code */
		bdev_io->internal.error.scsi.ascq = ascq; /* [한국어] ASC Qualifier */
	}

	return status;
}

/*
 * [한국어]
 * spdk_bdev_io_complete_scsi_status - SCSI 상태로 I/O 완료
 *
 * @bdev_io: 완료할 bdev_io
 * @sc,sk,asc,ascq: SCSI 상태 코드
 *
 * SCSI 오류 정보 설정 후 spdk_bdev_io_complete() 호출.
 *
 * 호출 체인:
 *   SCSI 모듈 → [이 함수] → spdk_bdev_io_complete()
 */
void
spdk_bdev_io_complete_scsi_status(struct spdk_bdev_io *bdev_io, enum spdk_scsi_status sc,
				  enum spdk_scsi_sense sk, uint8_t asc, uint8_t ascq)
{
	enum spdk_bdev_io_status status = spdk_bdev_io_set_scsi_status(bdev_io, sc, sk, asc, ascq);

	spdk_bdev_io_complete(bdev_io, status); /* [한국어] SCSI 상태 기반으로 완료 */
}

/*
 * [한국어]
 * spdk_bdev_io_get_scsi_status - bdev_io에서 SCSI 상태 코드 조회
 *
 * @bdev_io:  조회할 bdev_io
 * @sc,sk,asc,ascq: 결과 출력 포인터
 *
 * bdev_io 내부 상태를 SCSI 표준 형식으로 변환해 반환.
 * SUCCESS → GOOD, NVME_ERROR → NVMe→SCSI 변환, MISCOMPARE → CHECK_CONDITION+MISCOMPARE,
 * SCSI_ERROR → 저장된 값, 기타 → CHECK_CONDITION+ABORTED_COMMAND.
 *
 * 호출 체인:
 *   vhost_scsi / iscsi 등 SCSI 상위 레이어 → [이 함수]
 */
void
spdk_bdev_io_get_scsi_status(const struct spdk_bdev_io *bdev_io,
			     int *sc, int *sk, int *asc, int *ascq)
{
	assert(sc != NULL);
	assert(sk != NULL);
	assert(asc != NULL);
	assert(ascq != NULL);

	switch (bdev_io->internal.status) {
	case SPDK_BDEV_IO_STATUS_SUCCESS:
		/* [한국어] 성공 → SCSI GOOD + NO_SENSE */
		*sc = SPDK_SCSI_STATUS_GOOD;
		*sk = SPDK_SCSI_SENSE_NO_SENSE;
		*asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
		break;
	case SPDK_BDEV_IO_STATUS_NVME_ERROR:
		/* [한국어] NVMe 오류 → NVMe→SCSI 변환 함수 사용 */
		spdk_scsi_nvme_translate(bdev_io, sc, sk, asc, ascq);
		break;
	case SPDK_BDEV_IO_STATUS_MISCOMPARE:
		/* [한국어] COMPARE 불일치 → CHECK_CONDITION + MISCOMPARE sense */
		*sc = SPDK_SCSI_STATUS_CHECK_CONDITION;
		*sk = SPDK_SCSI_SENSE_MISCOMPARE;
		*asc = SPDK_SCSI_ASC_MISCOMPARE_DURING_VERIFY_OPERATION;
		*ascq = bdev_io->internal.error.scsi.ascq;
		break;
	case SPDK_BDEV_IO_STATUS_SCSI_ERROR:
		/* [한국어] 저장된 SCSI 오류 값 그대로 반환 */
		*sc = bdev_io->internal.error.scsi.sc;
		*sk = bdev_io->internal.error.scsi.sk;
		*asc = bdev_io->internal.error.scsi.asc;
		*ascq = bdev_io->internal.error.scsi.ascq;
		break;
	default:
		/* [한국어] 기타 오류 → CHECK_CONDITION + ABORTED_COMMAND */
		*sc = SPDK_SCSI_STATUS_CHECK_CONDITION;
		*sk = SPDK_SCSI_SENSE_ABORTED_COMMAND;
		*asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
		break;
	}
}

/*
 * [한국어]
 * spdk_bdev_io_set_aio_status - AIO(Async I/O) 결과를 bdev_io에 설정
 *
 * @bdev_io:    상태를 설정할 bdev_io
 * @aio_result: libaio/io_uring 결과 (0=성공, 음수=errno)
 * @return:     SPDK_BDEV_IO_STATUS_SUCCESS 또는 SPDK_BDEV_IO_STATUS_AIO_ERROR
 *
 * AIO 결과를 bdev_io에 저장하고 대응 bdev 상태를 반환.
 *
 * 호출 체인:
 *   AIO 모듈(bdev_aio) → [이 함수] 또는 spdk_bdev_io_complete_aio_status()
 */
spdk_bdev_io_status_t
spdk_bdev_io_set_aio_status(struct spdk_bdev_io *bdev_io, int aio_result)
{
	enum spdk_bdev_io_status status;

	if (aio_result == 0) {
		status = SPDK_BDEV_IO_STATUS_SUCCESS;     /* [한국어] AIO 성공 */
	} else {
		status = SPDK_BDEV_IO_STATUS_AIO_ERROR;   /* [한국어] AIO 오류 */
	}

	bdev_io->internal.error.aio_result = aio_result; /* [한국어] errno 저장 */

	return status;
}

/*
 * [한국어]
 * spdk_bdev_io_complete_aio_status - AIO 결과로 I/O 완료
 *
 * @bdev_io:    완료할 bdev_io
 * @aio_result: AIO 결과
 *
 * AIO 결과 설정 후 spdk_bdev_io_complete() 호출.
 *
 * 호출 체인:
 *   bdev_aio 모듈 → [이 함수] → spdk_bdev_io_complete()
 */
void
spdk_bdev_io_complete_aio_status(struct spdk_bdev_io *bdev_io, int aio_result)
{
	enum spdk_bdev_io_status status = spdk_bdev_io_set_aio_status(bdev_io, aio_result);

	spdk_bdev_io_complete(bdev_io, status); /* [한국어] AIO 결과 기반으로 완료 */
}

/*
 * [한국어]
 * spdk_bdev_io_get_aio_status - bdev_io에서 AIO 결과 조회
 *
 * @bdev_io:    조회할 bdev_io
 * @aio_result: 결과 출력 포인터
 *
 * AIO_ERROR → 저장된 errno, SUCCESS → 0, 기타 → -EIO.
 *
 * 호출 체인:
 *   AIO 상위 레이어 → [이 함수]
 */
void
spdk_bdev_io_get_aio_status(const struct spdk_bdev_io *bdev_io, int *aio_result)
{
	assert(aio_result != NULL);

	if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_AIO_ERROR) {
		*aio_result = bdev_io->internal.error.aio_result; /* [한국어] 저장된 errno 반환 */
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		*aio_result = 0;   /* [한국어] 성공 → 0 */
	} else {
		*aio_result = -EIO; /* [한국어] 기타 오류 → -EIO */
	}
}

/*
 * [한국어]
 * spdk_bdev_io_set_nvme_status - NVMe 완료 코드를 bdev_io에 설정
 *
 * @bdev_io: 상태를 설정할 bdev_io
 * @cdw0:    완료 더블워드 0 (결과 데이터)
 * @sct:     Status Code Type (Generic/Specific/Media Error 등)
 * @sc:      Status Code
 * @return:  SUCCESS, ABORTED, 또는 NVME_ERROR
 *
 * NVMe 완료 상태를 bdev_io에 저장하고 대응 bdev 상태를 반환.
 * sct=Generic+sc=SUCCESS → SUCCESS, sct=Generic+sc=ABORTED_BY_REQUEST → ABORTED,
 * 그 외 → NVME_ERROR.
 *
 * 호출 체인:
 *   NVMe 모듈(bdev_nvme) → [이 함수] 또는 spdk_bdev_io_complete_nvme_status()
 */
spdk_bdev_io_status_t
spdk_bdev_io_set_nvme_status(struct spdk_bdev_io *bdev_io, uint32_t cdw0, int sct, int sc)
{
	enum spdk_bdev_io_status status;

	if (spdk_likely(sct == SPDK_NVME_SCT_GENERIC && sc == SPDK_NVME_SC_SUCCESS)) {
		status = SPDK_BDEV_IO_STATUS_SUCCESS;  /* [한국어] NVMe 성공 */
	} else if (sct == SPDK_NVME_SCT_GENERIC && sc == SPDK_NVME_SC_ABORTED_BY_REQUEST) {
		status = SPDK_BDEV_IO_STATUS_ABORTED;  /* [한국어] Abort 요청에 의한 중단 */
	} else {
		status = SPDK_BDEV_IO_STATUS_NVME_ERROR; /* [한국어] NVMe 오류 */
	}

	bdev_io->internal.error.nvme.cdw0 = cdw0; /* [한국어] 완료 더블워드 0 저장 */
	bdev_io->internal.error.nvme.sct = sct;   /* [한국어] Status Code Type 저장 */
	bdev_io->internal.error.nvme.sc = sc;     /* [한국어] Status Code 저장 */

	return status;
}

/*
 * [한국어]
 * spdk_bdev_io_complete_nvme_status - NVMe 완료 코드로 I/O 완료
 *
 * @bdev_io: 완료할 bdev_io
 * @cdw0,sct,sc: NVMe 완료 코드
 *
 * NVMe 완료 코드 설정 후 spdk_bdev_io_complete() 호출.
 *
 * 호출 체인:
 *   bdev_nvme 모듈 → [이 함수] → spdk_bdev_io_complete()
 */
void
spdk_bdev_io_complete_nvme_status(struct spdk_bdev_io *bdev_io, uint32_t cdw0, int sct, int sc)
{
	enum spdk_bdev_io_status status = spdk_bdev_io_set_nvme_status(bdev_io, cdw0, sct, sc);

	spdk_bdev_io_complete(bdev_io, status); /* [한국어] NVMe 완료 코드 기반으로 완료 */
}

/*
 * [한국어]
 * spdk_bdev_io_get_nvme_status - bdev_io에서 NVMe 완료 코드 조회
 *
 * @bdev_io:    조회할 bdev_io
 * @cdw0,sct,sc: 결과 출력 포인터
 *
 * bdev_io 상태를 NVMe 완료 코드 형식으로 변환.
 * ABORT 타입은 특별 처리: sc=SUCCESS + cdw0으로 성공/실패 구분.
 *
 * 호출 체인:
 *   NVMe 상위 레이어(vhost_nvme, nvme-of 등) → [이 함수]
 */
void
spdk_bdev_io_get_nvme_status(const struct spdk_bdev_io *bdev_io, uint32_t *cdw0, int *sct, int *sc)
{
	assert(sct != NULL);
	assert(sc != NULL);
	assert(cdw0 != NULL);

	if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_ABORT)) {
		/* [한국어] ABORT 타입 특별 처리: sc=SUCCESS + cdw0=0(성공)/1(실패) */
		*sct = SPDK_NVME_SCT_GENERIC;
		*sc = SPDK_NVME_SC_SUCCESS;
		if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS) {
			*cdw0 = 0;   /* [한국어] ABORT 성공 */
		} else {
			*cdw0 = 1U;  /* [한국어] ABORT 실패 */
		}
		return;
	}

	if (spdk_likely(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS)) {
		*sct = SPDK_NVME_SCT_GENERIC;
		*sc = SPDK_NVME_SC_SUCCESS;               /* [한국어] 성공 → Generic+SUCCESS */
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NVME_ERROR) {
		*sct = bdev_io->internal.error.nvme.sct;
		*sc = bdev_io->internal.error.nvme.sc;    /* [한국어] NVMe 오류 → 저장된 코드 반환 */
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_ABORTED) {
		*sct = SPDK_NVME_SCT_GENERIC;
		*sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;    /* [한국어] ABORTED → Generic+ABORTED_BY_REQUEST */
	} else {
		*sct = SPDK_NVME_SCT_GENERIC;
		*sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR; /* [한국어] 기타 오류 → 내부 오류 */
	}

	*cdw0 = bdev_io->internal.error.nvme.cdw0; /* [한국어] 완료 더블워드 0 */
}

/*
 * [한국어]
 * spdk_bdev_io_get_nvme_fused_status - Fused 명령의 NVMe 상태 코드 조회
 *
 * @bdev_io:                    Fused(COMPARE_AND_WRITE 등) bdev_io
 * @cdw0:                       완료 더블워드 0
 * @first_sct,first_sc:         첫 번째 명령(COMPARE) 상태
 * @second_sct,second_sc:       두 번째 명령(WRITE) 상태
 *
 * Fused 명령은 두 개의 NVMe 명령이 원자적으로 실행되는 구조.
 * 첫 번째 명령 실패 시 두 번째도 ABORTED_FAILED_FUSED.
 * NVMe 스펙 Figure "Fused Command Status Codes" 참조.
 *
 * 호출 체인:
 *   NVMe 상위 레이어 → [이 함수]
 */
void
spdk_bdev_io_get_nvme_fused_status(const struct spdk_bdev_io *bdev_io, uint32_t *cdw0,
				   int *first_sct, int *first_sc, int *second_sct, int *second_sc)
{
	assert(first_sct != NULL);
	assert(first_sc != NULL);
	assert(second_sct != NULL);
	assert(second_sc != NULL);
	assert(cdw0 != NULL);

	if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NVME_ERROR) {
		if (bdev_io->internal.error.nvme.sct == SPDK_NVME_SCT_MEDIA_ERROR &&
		    bdev_io->internal.error.nvme.sc == SPDK_NVME_SC_COMPARE_FAILURE) {
			/* [한국어] COMPARE 실패: first=COMPARE_FAILURE, second=ABORTED_FAILED_FUSED */
			*first_sct = bdev_io->internal.error.nvme.sct;
			*first_sc = bdev_io->internal.error.nvme.sc;
			*second_sct = SPDK_NVME_SCT_GENERIC;
			*second_sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
		} else {
			/* [한국어] 두 번째 명령(WRITE) 실패: first=SUCCESS, second=오류 코드 */
			*first_sct = SPDK_NVME_SCT_GENERIC;
			*first_sc = SPDK_NVME_SC_SUCCESS;
			*second_sct = bdev_io->internal.error.nvme.sct;
			*second_sc = bdev_io->internal.error.nvme.sc;
		}
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_ABORTED) {
		/* [한국어] 외부 중단: 첫 번째/두 번째 모두 ABORTED_BY_REQUEST */
		*first_sct = SPDK_NVME_SCT_GENERIC;
		*first_sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
		*second_sct = SPDK_NVME_SCT_GENERIC;
		*second_sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		/* [한국어] 성공: 첫 번째/두 번째 모두 SUCCESS */
		*first_sct = SPDK_NVME_SCT_GENERIC;
		*first_sc = SPDK_NVME_SC_SUCCESS;
		*second_sct = SPDK_NVME_SCT_GENERIC;
		*second_sc = SPDK_NVME_SC_SUCCESS;
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED) {
		/* [한국어] 첫 번째 Fused 명령 실패: first=INTERNAL_ERROR, second=ABORTED_FAILED_FUSED */
		*first_sct = SPDK_NVME_SCT_GENERIC;
		*first_sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		*second_sct = SPDK_NVME_SCT_GENERIC;
		*second_sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_MISCOMPARE) {
		/* [한국어] COMPARE 불일치: first=COMPARE_FAILURE, second=ABORTED_FAILED_FUSED */
		*first_sct = SPDK_NVME_SCT_MEDIA_ERROR;
		*first_sc = SPDK_NVME_SC_COMPARE_FAILURE;
		*second_sct = SPDK_NVME_SCT_GENERIC;
		*second_sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
	} else {
		/* [한국어] 기타: 첫 번째/두 번째 모두 INTERNAL_DEVICE_ERROR */
		*first_sct = SPDK_NVME_SCT_GENERIC;
		*first_sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		*second_sct = SPDK_NVME_SCT_GENERIC;
		*second_sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	*cdw0 = bdev_io->internal.error.nvme.cdw0; /* [한국어] 완료 더블워드 0 */
}

/*
 * [한국어]
 * spdk_bdev_io_set_base_io_status - base_io의 오류 정보를 상위 bdev_io에 전파
 *
 * @bdev_io:   상태를 설정할 상위 bdev_io
 * @base_io:   오류 정보 원본 bdev_io
 * @return:    대응하는 spdk_bdev_io_status
 *
 * 계층화된 bdev(lvol, raid 등)에서 하위 I/O의 오류 정보를 상위 I/O에 복사.
 * NVMe/SCSI/AIO 오류 세부 정보가 보존되어 최종 사용자에게 정확한 오류가 전달.
 *
 * 호출 체인:
 *   spdk_bdev_io_complete_base_io_status() → [이 함수]
 */
spdk_bdev_io_status_t
spdk_bdev_io_set_base_io_status(struct spdk_bdev_io *bdev_io,
				const struct spdk_bdev_io *base_io)
{
	enum spdk_bdev_io_status status;

	switch (base_io->internal.status) {
	case SPDK_BDEV_IO_STATUS_NVME_ERROR:
		/* [한국어] NVMe 오류 세부 정보 복사 */
		status = spdk_bdev_io_set_nvme_status(bdev_io,
						      base_io->internal.error.nvme.cdw0,
						      base_io->internal.error.nvme.sct,
						      base_io->internal.error.nvme.sc);
		break;
	case SPDK_BDEV_IO_STATUS_SCSI_ERROR:
		/* [한국어] SCSI 오류 세부 정보 복사 */
		status = spdk_bdev_io_set_scsi_status(bdev_io,
						      base_io->internal.error.scsi.sc,
						      base_io->internal.error.scsi.sk,
						      base_io->internal.error.scsi.asc,
						      base_io->internal.error.scsi.ascq);
		break;
	case SPDK_BDEV_IO_STATUS_AIO_ERROR:
		/* [한국어] AIO 오류 결과 복사 */
		status = spdk_bdev_io_set_aio_status(bdev_io, base_io->internal.error.aio_result);
		break;
	default:
		/* [한국어] 기타 상태 그대로 복사 (SUCCESS, ABORTED, MISCOMPARE 등) */
		status = base_io->internal.status;
		break;
	}

	return status;
}

/*
 * [한국어]
 * spdk_bdev_io_complete_base_io_status - base_io 상태로 I/O 완료
 *
 * @bdev_io:   완료할 bdev_io
 * @base_io:   오류 정보 원본
 *
 * base_io 상태/오류 정보 복사 후 spdk_bdev_io_complete() 호출.
 *
 * 호출 체인:
 *   계층화된 bdev 모듈 → [이 함수] → spdk_bdev_io_complete()
 */
void
spdk_bdev_io_complete_base_io_status(struct spdk_bdev_io *bdev_io,
				     const struct spdk_bdev_io *base_io)
{
	enum spdk_bdev_io_status status = spdk_bdev_io_set_base_io_status(bdev_io, base_io);

	spdk_bdev_io_complete(bdev_io, status); /* [한국어] base_io 상태 기반으로 완료 */
}

/*
 * [한국어]
 * spdk_bdev_io_get_thread - bdev_io가 발행된 채널의 스레드 반환
 *
 * @bdev_io: 조회할 bdev_io
 * @return:  채널 소유 spdk_thread
 *
 * 모듈이 올바른 스레드에서 완료 콜백을 호출하는지 확인할 때 사용.
 *
 * 호출 체인:
 *   _bdev_io_complete() assert 검증 / 모듈 → [이 함수]
 */
struct spdk_thread *
spdk_bdev_io_get_thread(struct spdk_bdev_io *bdev_io)
{
	return spdk_io_channel_get_thread(bdev_io->internal.ch->channel);
	/* [한국어] 채널에서 소유 스레드 반환 */
}

/*
 * [한국어]
 * spdk_bdev_io_get_io_channel - bdev_io의 I/O 채널 반환
 *
 * @bdev_io: 조회할 bdev_io
 * @return:  채널의 spdk_io_channel 포인터
 *
 * 모듈이 채널 컨텍스트를 접근하거나 채널별 통계를 갱신할 때 사용.
 *
 * 호출 체인:
 *   bdev_io_update_io_stat() VTune / 모듈 → [이 함수]
 */
struct spdk_io_channel *
spdk_bdev_io_get_io_channel(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->internal.ch->channel;
	/* [한국어] bdev 채널에서 spdk_io_channel 포인터 반환 */
}

/*
 * [한국어]
 * bdev_register - bdev를 SPDK 관리 체계에 등록하는 핵심 내부 함수
 *
 * @bdev: 등록할 bdev 구조체 (모듈이 초기화한 상태)
 * @return: 0=성공, -EINVAL=이름 없음/빈 이름, -ENOMEM=메모리 부족, 기타 오류
 *
 * bdev 모듈이 spdk_bdev_register()를 호출하면 이 함수가 실행된다.
 * 수행하는 작업:
 *  1. bdev 이름 유효성 검사
 *  2. I/O 통계 구조체 할당
 *  3. internal 상태 초기화 (status=READY, claim=NONE, qos=NULL 등)
 *  4. UUID 생성 또는 검증, UUID 알리아스 추가
 *  5. io_type_supported/accel_sequence_supported 비트마스크 빌드
 *  6. write_unit_size/acwu/max_rw_size/max_copy/max_write_zeroes 기본값 설정
 *  7. spinlock 초기화 (채널 생성 전 필요)
 *  8. io_device 등록 (채널 생성/소멸 콜백 등록)
 *  9. bdev_name 해시 테이블 등록 (등록 후 다른 스레드가 bdev를 참조 가능해짐)
 *  10. g_bdev_mgr.bdevs 리스트에 삽입
 *
 * 호출 체인:
 *   spdk_bdev_register() → [이 함수]
 */
static int
bdev_register(struct spdk_bdev *bdev)
{
	char *bdev_name;
	char uuid[SPDK_UUID_STRING_LEN];
	struct spdk_iobuf_opts iobuf_opts;
	enum spdk_bdev_io_type io_type;
	int ret;

	assert(bdev->module != NULL); /* [한국어] 모듈 없이 등록 불가 */

	if (!bdev->name) {
		SPDK_ERRLOG("Bdev name is NULL\n");
		return -EINVAL; /* [한국어] 이름이 NULL */
	}

	if (!strlen(bdev->name)) {
		SPDK_ERRLOG("Bdev name must not be an empty string\n");
		return -EINVAL; /* [한국어] 이름이 빈 문자열 */
	}

	/* Users often register their own I/O devices using the bdev name. In
	 * order to avoid conflicts, prepend bdev_. */
	/* [한국어] io_device 이름 충돌 방지를 위해 "bdev_" 접두사 추가 */
	bdev_name = spdk_sprintf_alloc("bdev_%s", bdev->name);
	if (!bdev_name) {
		SPDK_ERRLOG("Unable to allocate memory for internal bdev name.\n");
		return -ENOMEM;
	}

	bdev->internal.stat = bdev_alloc_io_stat(true); /* [한국어] I/O 통계 구조체 할당 */
	if (!bdev->internal.stat) {
		SPDK_ERRLOG("Unable to allocate I/O statistics structure.\n");
		free(bdev_name);
		return -ENOMEM;
	}

	bdev->internal.status = SPDK_BDEV_STATUS_READY;     /* [한국어] bdev 상태: READY */
	bdev->internal.measured_queue_depth = UINT64_MAX;   /* [한국어] QD 측정값 초기화 (미측정) */
	bdev->internal.claim_type = SPDK_BDEV_CLAIM_NONE;   /* [한국어] 클레임 없음 */
	memset(&bdev->internal.claim, 0, sizeof(bdev->internal.claim)); /* [한국어] 클레임 정보 초기화 */
	bdev->internal.qd_poller = NULL;                    /* [한국어] QD 측정 폴러 없음 */
	bdev->internal.qos = NULL;                          /* [한국어] QoS 없음 */

	TAILQ_INIT(&bdev->internal.open_descs);             /* [한국어] 열린 디스크립터 리스트 초기화 */
	TAILQ_INIT(&bdev->internal.locked_ranges);          /* [한국어] LBA 락 범위 리스트 초기화 */
	TAILQ_INIT(&bdev->internal.pending_locked_ranges);  /* [한국어] 대기 중 LBA 락 리스트 초기화 */
	TAILQ_INIT(&bdev->internal.queued_resets);          /* [한국어] 대기 RESET 리스트 초기화 */
	TAILQ_INIT(&bdev->aliases);                         /* [한국어] 알리아스 리스트 초기화 */

	/* UUID may be specified by the user or defined by bdev itself.
	 * Otherwise it will be generated here, so this field will never be empty. */
	/* [한국어] UUID가 없으면 자동 생성 (bdev name과 달리 UUID는 항상 유효) */
	if (spdk_uuid_is_null(&bdev->uuid)) {
		spdk_uuid_generate(&bdev->uuid);
	}

	/* Add the UUID alias only if it's different than the name */
	/* [한국어] UUID 문자열 알리아스 추가 (이름과 다른 경우만) */
	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &bdev->uuid);
	if (strcmp(bdev->name, uuid) != 0) {
		ret = spdk_bdev_alias_add(bdev, uuid);
		if (ret != 0) {
			SPDK_ERRLOG("Unable to add uuid:%s alias for bdev %s\n", uuid, bdev->name);
			bdev_free_io_stat(bdev->internal.stat);
			free(bdev_name);
			return ret;
		}
	}

	/* [한국어] 모든 I/O 타입에 대해 지원 비트마스크와 accel_sequence 비트마스크 빌드 */
	for (io_type = SPDK_BDEV_IO_TYPE_READ; io_type < SPDK_BDEV_NUM_IO_TYPES; ++io_type) {
		if (bdev_module_io_type_supported(bdev, io_type)) {
			bdev->io_type_supported |= (1u << (uint32_t)io_type); /* [한국어] 해당 I/O 타입 지원 비트 설정 */
		}

		if (bdev_module_accel_sequence_supported(bdev, io_type)) {
			bdev->accel_sequence_supported |= (1u << (uint32_t)io_type); /* [한국어] accel 시퀀스 지원 비트 설정 */
		}
	}

	bdev->memory_domains_supported = spdk_bdev_get_memory_domains(bdev, NULL, 0) > 0;
	/* [한국어] 메모리 도메인 지원 여부 캐싱 */

	/* If the user didn't specify a write unit size, set it to one. */
	if (bdev->write_unit_size == 0) {
		bdev->write_unit_size = 1; /* [한국어] write_unit_size 기본값: 1블록 */
	}

	spdk_iobuf_get_opts(&iobuf_opts, sizeof(iobuf_opts)); /* [한국어] iobuf 설정 가져오기 */
	if (spdk_bdev_get_buf_align(bdev) > 1) {
		/* [한국어] 버퍼 정렬 요건이 있으면 max_rw_size를 iobuf large_bufsize 기준으로 제한 */
		bdev->max_rw_size = spdk_min(bdev->max_rw_size ? bdev->max_rw_size : UINT32_MAX,
					     bdev_get_max_write(bdev, iobuf_opts.large_bufsize));
	}

	/* Set ACWU value to the write unit size if bdev module did not set it (does not support it natively) */
	/* [한국어] ACWU(Atomic Compare & Write Unit)가 설정되지 않으면 write_unit_size 사용 */
	if (bdev->acwu == 0) {
		bdev->acwu = bdev->write_unit_size;
	}

	if (bdev->phys_blocklen == 0) {
		bdev->phys_blocklen = spdk_bdev_get_data_block_size(bdev); /* [한국어] 물리 블록 크기 기본값 */
	}

	if (!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COPY)) {
		/* [한국어] COPY 미지원 시 iobuf large_bufsize 기반으로 max_copy 계산 */
		bdev->max_copy = bdev_get_max_write(bdev, iobuf_opts.large_bufsize);
	}

	if (!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
		/* If WRITE_ZEROES is not supported, set max_write_zeroes based on write capability */
		/* [한국어] WRITE_ZEROES 미지원 시 zero_buffer(ZERO_BUFFER_SIZE) 기반으로 에뮬레이션 한계 계산 */
		uint32_t zero_buffer_num_blocks = bdev_get_max_write(bdev, ZERO_BUFFER_SIZE);
		uint32_t write_boundary = bdev_rw_get_io_boundary(bdev, SPDK_BDEV_IO_TYPE_WRITE);

		bdev->max_write_zeroes = spdk_min(write_boundary, zero_buffer_num_blocks);
		if (bdev->max_write_zeroes == 0) {
			bdev->max_write_zeroes = zero_buffer_num_blocks; /* [한국어] write_boundary가 0이면 zero_buffer 기준 사용 */
		}
	}

	bdev->internal.reset_in_progress = NULL;  /* [한국어] 진행 중인 RESET 없음 */
	bdev->internal.qd_poll_in_progress = false; /* [한국어] QD 폴링 진행 중 아님 */
	bdev->internal.period = 0;                /* [한국어] QD 측정 주기 0 (비활성) */
	bdev->internal.new_period = 0;            /* [한국어] 새 QD 측정 주기 0 */
	bdev->internal.trace_id = spdk_trace_register_owner(OWNER_TYPE_BDEV, bdev_name);
	/* [한국어] trace 소유자 등록 — "bdev_<name>" 이름으로 trace 이벤트 구분 */

	/*
	 * Initialize spinlock before registering IO device because spinlock is used in
	 * bdev_channel_create
	 */
	/* [한국어] spinlock을 io_device 등록 전에 초기화 (bdev_channel_create에서 사용) */
	spdk_spin_init(&bdev->internal.spinlock);

	spdk_io_device_register(__bdev_to_io_dev(bdev),
				bdev_channel_create, bdev_channel_destroy,
				sizeof(struct spdk_bdev_channel),
				bdev_name);
	/* [한국어] io_device 등록: 채널 생성/소멸 콜백과 채널 크기 지정 */

	/*
	 * Register bdev name only after the bdev object is ready.
	 * After bdev_name_add returns, it is possible for other threads to start using the bdev,
	 * create IO channels...
	 */
	/* [한국어] bdev 이름 등록 — 이후 다른 스레드가 이 bdev를 찾을 수 있게 됨 */
	ret = bdev_name_add(&bdev->internal.bdev_name, bdev, bdev->name);
	if (ret != 0) {
		/* [한국어] 이름 등록 실패 → io_device, 알리아스, stat, spinlock 모두 롤백 */
		spdk_io_device_unregister(__bdev_to_io_dev(bdev), NULL);
		if (strcmp(bdev->name, uuid) != 0) {
			spdk_bdev_alias_del(bdev, uuid);
		}
		bdev_free_io_stat(bdev->internal.stat);
		spdk_spin_destroy(&bdev->internal.spinlock);
		free(bdev_name);
		return ret;
	}

	free(bdev_name); /* [한국어] io_device 등록 후 이름 문자열 해제 */

	SPDK_DEBUGLOG(bdev, "Inserting bdev %s into list\n", bdev->name);
	TAILQ_INSERT_TAIL(&g_bdev_mgr.bdevs, bdev, internal.link); /* [한국어] g_bdev_mgr.bdevs 전역 리스트에 추가 */

	return 0;
}

/*
 * [한국어]
 * bdev_destroy_cb - io_device 해제 완료 후 bdev 자원 최종 정리 및 destruct 호출
 *
 * @io_device: bdev의 io_device 포인터
 *
 * spdk_io_device_unregister()의 콜백으로, 모든 채널이 파괴된 후 호출된다.
 * unregister_td가 현재 스레드와 다르면 해당 스레드로 메시지를 보내 다시 호출.
 * spinlock/qos/stat/trace_id 해제 후 모듈의 fn_table->destruct()를 호출.
 * destruct 완료(rc<=0)이면 unregister_cb를 호출해 사용자에게 완료 통지.
 *
 * 호출 체인:
 *   spdk_io_device_unregister() 완료 → [이 함수] → bdev->fn_table->destruct()
 *   → unregister_cb() (또는 spdk_bdev_destruct_done())
 */
static void
bdev_destroy_cb(void *io_device)
{
	int			rc;
	struct spdk_bdev	*bdev;
	spdk_bdev_unregister_cb	cb_fn;
	void			*cb_arg;

	bdev = __bdev_from_io_dev(io_device); /* [한국어] io_device에서 bdev 포인터 복원 */

	if (bdev->internal.unregister_td != spdk_get_thread()) {
		/* [한국어] 현재 스레드가 unregister 스레드와 다르면 해당 스레드로 전달 */
		spdk_thread_send_msg(bdev->internal.unregister_td, bdev_destroy_cb, io_device);
		return;
	}

	assert(TAILQ_EMPTY(&bdev->internal.locked_ranges)); /* [한국어] LBA 락 범위 없어야 함 */

	cb_fn = bdev->internal.unregister_cb;   /* [한국어] 완료 콜백 */
	cb_arg = bdev->internal.unregister_ctx; /* [한국어] 완료 콜백 인자 */

	spdk_spin_destroy(&bdev->internal.spinlock);  /* [한국어] spinlock 해제 */
	free(bdev->internal.qos);                      /* [한국어] QoS 구조체 해제 */
	bdev_free_io_stat(bdev->internal.stat);         /* [한국어] I/O 통계 구조체 해제 */
	spdk_trace_unregister_owner(bdev->internal.trace_id); /* [한국어] trace 소유자 해제 */

	rc = bdev->fn_table->destruct(bdev->ctxt); /* [한국어] 모듈의 destruct 호출 */
	if (rc < 0) {
		SPDK_ERRLOG("destruct failed\n"); /* [한국어] destruct 실패 로그 */
	}
	if (rc <= 0 && cb_fn != NULL) {
		cb_fn(cb_arg, rc); /* [한국어] destruct 완료(rc<=0) → unregister 완료 콜백 */
	}
}

/*
 * [한국어]
 * spdk_bdev_destruct_done - 비동기 destruct 완료 신호 (모듈이 호출)
 *
 * @bdev:       destruct 완료된 bdev
 * @bdeverrno:  destruct 결과 (0=성공, 음수=오류)
 *
 * 모듈이 destruct()에서 양수를 반환해 비동기 destruct를 시작한 경우,
 * destruct가 실제로 완료되면 이 함수를 호출한다.
 * unregister_cb를 통해 사용자에게 완료를 통지.
 *
 * 호출 체인:
 *   bdev 모듈 비동기 destruct 완료 → [이 함수] → unregister_cb()
 */
void
spdk_bdev_destruct_done(struct spdk_bdev *bdev, int bdeverrno)
{
	if (bdev->internal.unregister_cb != NULL) {
		bdev->internal.unregister_cb(bdev->internal.unregister_ctx, bdeverrno);
		/* [한국어] unregister 완료 콜백 호출 */
	}
}

/*
 * [한국어]
 * _remove_notify - 디스크립터에 REMOVE 이벤트 비동기 통지
 *
 * @arg: 통지할 spdk_bdev_desc
 *
 * 디스크립터 소유 스레드에서 SPDK_BDEV_EVENT_REMOVE 이벤트를 발생시킨다.
 * spdk_thread_send_msg()로 defer되므로 현재 컨텍스트가 완전히 언와인드된 후 실행.
 *
 * 호출 체인:
 *   event_notify(desc, _remove_notify) → spdk_thread_send_msg() → [이 함수]
 *   → _event_notify(desc, SPDK_BDEV_EVENT_REMOVE)
 */
static void
_remove_notify(void *arg)
{
	struct spdk_bdev_desc *desc = arg;

	_event_notify(desc, SPDK_BDEV_EVENT_REMOVE); /* [한국어] REMOVE 이벤트 발생 */
}

/* returns: 0 - bdev removed and ready to be destructed.
 *          -EBUSY - bdev can't be destructed yet.  */
/*
 * [한국어]
 * bdev_unregister_unsafe - bdev 등록 해제 수행 (spinlock 보유 상태에서 호출)
 *
 * @bdev: 해제할 bdev
 * @return: 0=해제 준비 완료, -EBUSY=아직 해제 불가
 *
 * g_bdev_mgr.spinlock + bdev->internal.spinlock을 모두 보유한 상태에서 호출.
 * 열린 디스크립터가 있으면 각각에 REMOVE 이벤트를 보내고 -EBUSY 반환.
 * QoS 설정 진행 중이면 -EBUSY 반환.
 * 디스크립터가 없으면: 이름/별칭/전역 리스트 정리, RESET 진행 중이면 -EBUSY.
 *
 * 호출 체인:
 *   bdev_unregister() → [이 함수] → event_notify() × N (desc별)
 */
static int
bdev_unregister_unsafe(struct spdk_bdev *bdev)
{
	struct spdk_bdev_desc	*desc, *tmp;
	struct spdk_bdev_alias	*alias;
	int			rc = 0;

	assert(spdk_spin_held(&g_bdev_mgr.spinlock));    /* [한국어] g_bdev_mgr.spinlock 보유 검증 */
	assert(spdk_spin_held(&bdev->internal.spinlock)); /* [한국어] bdev spinlock 보유 검증 */

	/* Notify each descriptor about hotremoval */
	TAILQ_FOREACH_SAFE(desc, &bdev->internal.open_descs, link, tmp) {
		rc = -EBUSY; /* [한국어] 열린 desc 있음 → EBUSY */
		/*
		 * Defer invocation of the event_cb to a separate message that will
		 *  run later on its thread.  This ensures this context unwinds and
		 *  we don't recursively unregister this bdev again if the event_cb
		 *  immediately closes its descriptor.
		 */
		/* [한국어] event_cb defer: 현재 컨텍스트 언와인드 후 이벤트 발생 (재귀 방지) */
		event_notify(desc, _remove_notify);
	}

	if (bdev->internal.qos_mod_in_progress) {
		/* QoS setup is in progress, can't unregister for now. */
		/* [한국어] QoS 설정 진행 중 → 해제 불가 */
		rc = -EBUSY;
	}

	/* If there are no descriptors, proceed removing the bdev */
	if (rc == 0) {
		/* [한국어] 모든 desc 닫힘 + QoS 설정 없음 → 실제 해제 진행 */
		bdev_examine_allowlist_remove(bdev->name);
		TAILQ_FOREACH(alias, &bdev->aliases, tailq) {
			bdev_examine_allowlist_remove(alias->alias.name); /* [한국어] 별칭 examine allowlist 제거 */
		}
		bdev_alias_del_all(bdev, bdev_name_del_unsafe);     /* [한국어] 모든 별칭 및 이름 테이블 제거 */
		TAILQ_REMOVE(&g_bdev_mgr.bdevs, bdev, internal.link); /* [한국어] 전역 bdev 리스트에서 제거 */
		SPDK_DEBUGLOG(bdev, "Removing bdev %s from list done\n", bdev->name);

		/* Delete the name */
		bdev_name_del_unsafe(&bdev->internal.bdev_name); /* [한국어] bdev 이름 해시 테이블에서 제거 */

		spdk_notify_send("bdev_unregister", spdk_bdev_get_name(bdev)); /* [한국어] bdev_unregister 이벤트 브로드캐스트 */

		if (bdev->internal.reset_in_progress != NULL) {
			/* If reset is in progress, let the completion callback for reset
			 * unregister the bdev.
			 */
			/* [한국어] RESET 진행 중 → RESET 완료 콜백이 io_device_unregister 처리 */
			rc = -EBUSY;
		}
	}

	return rc;
}

/*
 * [한국어]
 * bdev_unregister_abort_channel - bdev 해제 전 채널의 대기 I/O abort
 *
 * @i:     채널 이터레이터
 * @bdev:  대상 bdev
 * @io_ch: 현재 채널
 * @_ctx:  사용되지 않음
 *
 * bdev 해제 전 모든 채널의 대기 I/O(nomem/buf/qos 큐)를 abort.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → [이 함수] × 채널 수 → bdev_unregister()
 */
static void
bdev_unregister_abort_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			      struct spdk_io_channel *io_ch, void *_ctx)
{
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(io_ch);

	bdev_channel_abort_queued_ios(bdev_ch);       /* [한국어] 채널의 대기 I/O abort */
	spdk_bdev_for_each_channel_continue(i, 0);    /* [한국어] 이터레이터 계속 */
}

/*
 * [한국어]
 * bdev_unregister - 채널 abort 완료 후 bdev 등록 해제 처리
 *
 * @bdev:   대상 bdev
 * @_ctx:   사용되지 않음
 * @status: 항상 0
 *
 * 채널 abort 완료 후 호출되어 bdev 등록 해제를 진행한다.
 * locked_ranges가 있으면 대기 (잠금 해제 후 재호출됨).
 * status를 REMOVING으로 설정 후 bdev_unregister_unsafe() 호출.
 * 모든 desc가 닫혀있으면 io_device_unregister()로 채널 파괴 시작.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel(abort) 완료 → [이 함수]
 *   → bdev_unregister_unsafe() → spdk_io_device_unregister() → bdev_destroy_cb()
 */
static void
bdev_unregister(struct spdk_bdev *bdev, void *_ctx, int status)
{
	int rc;

	spdk_spin_lock(&g_bdev_mgr.spinlock); /* [한국어] 전역 bdev 리스트 접근 보호 */
	spdk_spin_lock(&bdev->internal.spinlock);
	if (!TAILQ_EMPTY(&bdev->internal.locked_ranges)) {
		/* Unregister will be continued after all ranges are unlocked. */
		/* [한국어] LBA 락 범위 있음 → 잠금 해제 후 재호출될 것임 */
		spdk_spin_unlock(&bdev->internal.spinlock);
		spdk_spin_unlock(&g_bdev_mgr.spinlock);
		return;
	}
	/*
	 * Set the status to REMOVING after completing to abort channels. Otherwise,
	 * the last spdk_bdev_close() may call spdk_io_device_unregister() while
	 * spdk_bdev_for_each_channel() is executed and spdk_io_device_unregister()
	 * may fail.
	 */
	/* [한국어] REMOVING 상태 설정: 채널 abort 완료 후에만 진행 (레이스 컨디션 방지) */
	bdev->internal.status = SPDK_BDEV_STATUS_REMOVING;
	rc = bdev_unregister_unsafe(bdev); /* [한국어] 실제 해제 수행 */
	spdk_spin_unlock(&bdev->internal.spinlock);
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	if (rc == 0) {
		/* [한국어] 모든 desc 닫힘 → io_device 해제 시작 (채널 파괴 → bdev_destroy_cb) */
		spdk_io_device_unregister(__bdev_to_io_dev(bdev), bdev_destroy_cb);
	}
}

/*
 * [한국어]
 * _bdev_unregister - 현재 스레드에서 bdev_unregister()를 호출하는 메시지 래퍼
 *
 * @ctx: bdev 포인터
 *
 * spdk_thread_send_msg()를 통해 다른 스레드에서 bdev_unregister를 실행할 때 사용.
 *
 * 호출 체인:
 *   spdk_thread_send_msg() → [이 함수] → bdev_unregister()
 */
static void
_bdev_unregister(void *ctx)
{
	struct spdk_bdev *bdev = ctx;

	bdev_unregister(bdev, NULL, 0); /* [한국어] bdev 해제 진행 */
}

SPDK_LOG_DEPRECATION_REGISTER(spdk_bdev_unregister,
			      "calling spdk_bdev_unregister from any thread is deprecated",
			      "v26.05", SPDK_LOG_DEPRECATION_EVERY_24H);

/*
 * [한국어]
 * spdk_bdev_unregister - bdev을 SPDK 시스템에서 해제 (비동기).
 *
 * @bdev:  해제할 bdev.
 * @cb_fn: unregister 완전 완료 시 호출될 콜백 (NULL 허용).
 * @cb_arg: cb_fn 컨텍스트.
 *
 * 단계:
 *   1) status = UNREGISTERING — 더 이상 새 desc open 불가.
 *   2) 열린 모든 desc에 REMOVE event 통지 — 사용자가 spdk_bdev_close 호출 trigger.
 *   3) 모든 desc가 close되면 _remove_notify에서 spdk_io_device_unregister 호출.
 *   4) 모든 채널이 destroy되면 bdev_destroy_cb에서 모듈 fn_table->destruct 호출.
 *   5) 모듈이 spdk_bdev_destruct_done로 신호하면 unregister_cb 호출 — 완전 종료.
 *
 * 실행 컨텍스트: SPDK app thread에서 호출 권장 (다른 thread에서 호출 시 deprecation log).
 *  완료 콜백은 unregister 진행 thread에서 호출됨.
 */
void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct spdk_thread	*thread;
	/* [한국어] 호출 thread — non-SPDK thread 검출용. */

	SPDK_DEBUGLOG(bdev, "Removing bdev %s from list\n", bdev->name);

	thread = spdk_get_thread();
	if (!thread) {
		/* The user called this from a non-SPDK thread. */
		if (cb_fn != NULL) {
			cb_fn(cb_arg, -ENOTSUP);
		}
		return;
	}

	if (!spdk_thread_is_app_thread(NULL)) {
		SPDK_LOG_DEPRECATED(spdk_bdev_unregister);
	}

	spdk_spin_lock(&g_bdev_mgr.spinlock);
	if (bdev->internal.status == SPDK_BDEV_STATUS_UNREGISTERING ||
	    bdev->internal.status == SPDK_BDEV_STATUS_REMOVING) {
		spdk_spin_unlock(&g_bdev_mgr.spinlock);
		if (cb_fn) {
			cb_fn(cb_arg, -EBUSY);
		}
		return;
	}

	spdk_spin_lock(&bdev->internal.spinlock);
	bdev->internal.status = SPDK_BDEV_STATUS_UNREGISTERING;
	bdev->internal.unregister_cb = cb_fn;
	bdev->internal.unregister_ctx = cb_arg;
	bdev->internal.unregister_td = thread;

	/* kill QoS, if it's still running */
	if (bdev->internal.qos && bdev->internal.qos->poller && TAILQ_EMPTY(&bdev->internal.open_descs)) {
		SPDK_DEBUGLOG(bdev, "Data race detected - QoS poller still present on closed bdev name: %s",
			      bdev->name);
		if (bdev_qos_destroy(bdev)) {
			SPDK_ERRLOG("Unable to shut down QoS poller. It will continue running on the current thread.\n");
		}
	}
	spdk_spin_unlock(&bdev->internal.spinlock);
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	spdk_bdev_set_qd_sampling_period(bdev, 0);

	spdk_bdev_for_each_channel(bdev, bdev_unregister_abort_channel, bdev,
				   bdev_unregister);
}

/*
 * [한국어]
 * spdk_bdev_unregister_by_name - bdev 이름으로 bdev를 찾아 해제
 *
 * @bdev_name: 해제할 bdev 이름
 * @module:    등록한 모듈 (소유권 검증용)
 * @cb_fn:     완료 콜백
 * @cb_arg:    완료 콜백 인자
 * @return:    0=성공 시작, -ENODEV/-EPERM 등
 *
 * bdev_name으로 bdev를 열고 module 일치 여부 확인 후 spdk_bdev_unregister() 호출.
 * module 불일치 시 -ENODEV 반환.
 *
 * 호출 체인:
 *   모듈 → [이 함수] → spdk_bdev_open_ext() + spdk_bdev_unregister() + spdk_bdev_close()
 */
int
spdk_bdev_unregister_by_name(const char *bdev_name, struct spdk_bdev_module *module,
			     spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	int rc;

	/* [한국어] 임시 디스크립터로 bdev 열기 */
	rc = spdk_bdev_open_ext(bdev_name, false, _tmp_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open bdev with name: %s\n", bdev_name);
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);

	if (bdev->module != module) {
		/* [한국어] 등록한 모듈과 다름 → 소유권 없음 */
		spdk_bdev_close(desc);
		SPDK_ERRLOG("Bdev %s was not registered by the specified module.\n",
			    bdev_name);
		return -ENODEV;
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg); /* [한국어] bdev 해제 시작 */

	spdk_bdev_close(desc); /* [한국어] 임시 디스크립터 닫기 */

	return 0;
}

/*
 * [한국어]
 * bdev_start_qos - bdev 열기 시 QoS 초기화
 *
 * @bdev: QoS를 시작할 bdev
 * @return: 0=성공, -ENOMEM
 *
 * bdev->internal.qos가 설정되어 있지만 스레드에 바인딩되지 않은 경우
 * QoS 활성화 메시지를 각 채널에 전달하여 QoS 폴러를 등록한다.
 * bdev_open()에서 첫 번째 desc 열릴 때 호출.
 *
 * 호출 체인:
 *   bdev_open() → [이 함수] → spdk_bdev_for_each_channel(bdev_enable_qos_msg)
 *   → bdev_enable_qos_done()
 */
static int
bdev_start_qos(struct spdk_bdev *bdev)
{
	struct set_qos_limit_ctx *ctx;

	/* Enable QoS */
	if (bdev->internal.qos && bdev->internal.qos->thread == NULL) {
		/* [한국어] QoS가 설정되어 있지만 스레드에 바인딩 안 됨 → 지금 활성화 */
		ctx = calloc(1, sizeof(*ctx));
		if (ctx == NULL) {
			SPDK_ERRLOG("Failed to allocate memory for QoS context\n");
			return -ENOMEM;
		}
		ctx->bdev = bdev;                                       /* [한국어] 대상 bdev */
		ctx->bdev->internal.qos_mod_in_progress = true;       /* [한국어] QoS 설정 진행 중 플래그 */
		spdk_bdev_for_each_channel(bdev, bdev_enable_qos_msg, ctx, bdev_enable_qos_done);
		/* [한국어] 각 채널에 QoS 활성화 메시지 전달 */
	}

	return 0;
}

/*
 * [한국어]
 * log_already_claimed - 이미 클레임된 bdev에 대한 로그 출력
 *
 * @level:  로그 레벨 (INFO/ERROR 등)
 * @line:   호출 라인
 * @func:   호출 함수명
 * @detail: 상황 설명 문자열
 * @bdev:   대상 bdev (spinlock 보유 상태에서 호출)
 *
 * 클레임 타입에 따라 v1(EXCL_WRITE) 또는 v2(read_many/write_one 등) 형식으로 모듈명 로그.
 * SPDK_LOG_bdev 플래그가 비활성화된 경우 INFO 레벨은 출력 안 함.
 *
 * 호출 체인:
 *   LOG_ALREADY_CLAIMED_ERROR 매크로 → [이 함수]
 */
static void
log_already_claimed(enum spdk_log_level level, const int line, const char *func, const char *detail,
		    struct spdk_bdev *bdev)
{
	enum spdk_bdev_claim_type type;
	const char *typename, *modname;
	extern struct spdk_log_flag SPDK_LOG_bdev;

	assert(spdk_spin_held(&bdev->internal.spinlock)); /* [한국어] spinlock 보유 검증 */

	if (level >= SPDK_LOG_INFO && !SPDK_LOG_bdev.enabled) {
		return; /* [한국어] INFO 레벨 + bdev 로그 비활성화 → 출력 안 함 */
	}

	type = bdev->internal.claim_type;
	typename = spdk_bdev_claim_get_name(type); /* [한국어] 클레임 타입 이름 */

	if (type == SPDK_BDEV_CLAIM_EXCL_WRITE) {
		/* [한국어] v1 단독 쓰기 클레임 → 단일 모듈 로그 */
		modname = bdev->internal.claim.v1.module->name;
		spdk_log(level, __FILE__, line, func, "bdev %s %s: type %s by module %s\n",
			 bdev->name, detail, typename, modname);
		return;
	}

	if (claim_type_is_v2(type)) {
		/* [한국어] v2 클레임 → 각 클레임 모듈별 로그 */
		struct spdk_bdev_module_claim *claim;

		TAILQ_FOREACH(claim, &bdev->internal.claim.v2.claims, link) {
			modname = claim->module->name;
			spdk_log(level, __FILE__, line, func, "bdev %s %s: type %s by module %s\n",
				 bdev->name, detail, typename, modname);
		}
		return;
	}

	assert(false); /* [한국어] 알 수 없는 클레임 타입 */
}

/*
 * [한국어]
 * bdev_open - bdev 디스크립터 초기화 및 open_descs 등록
 *
 * @bdev:  열 대상 bdev
 * @write: true=쓰기 가능 desc
 * @desc:  이미 할당된 디스크립터 (bdev_desc_alloc()으로 생성된)
 * @return: 0=성공, -ENOTSUP=비SPDK 스레드, -ENODEV=bdev 해제 중, -EPERM=클레임 충돌
 *
 * desc->bdev/thread/write를 설정하고 open_descs 리스트에 추가.
 * bdev가 UNREGISTERING/REMOVING 상태이면 -ENODEV 반환.
 * 쓰기 desc이고 이미 클레임이 있으면 -EPERM 반환.
 * QoS가 설정되어 있으면 bdev_start_qos()로 활성화.
 *
 * 호출 체인:
 *   spdk_bdev_open_ext() → bdev_desc_alloc() → [이 함수]
 */
static int
bdev_open(struct spdk_bdev *bdev, bool write, struct spdk_bdev_desc *desc)
{
	struct spdk_thread *thread;
	int rc = 0;

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("Cannot open bdev from non-SPDK thread.\n");
		return -ENOTSUP; /* [한국어] 비SPDK 스레드에서 호출 불가 */
	}

	SPDK_DEBUGLOG(bdev, "Opening descriptor %p for bdev %s on thread %p\n", desc, bdev->name,
		      spdk_get_thread());

	desc->bdev = bdev;     /* [한국어] desc의 대상 bdev 설정 */
	desc->thread = thread; /* [한국어] desc의 소유 스레드 설정 */
	desc->write = write;   /* [한국어] 쓰기 가능 여부 설정 */

	spdk_spin_lock(&bdev->internal.spinlock); /* [한국어] status/claim/open_descs 접근 보호 */
	if (bdev->internal.status == SPDK_BDEV_STATUS_UNREGISTERING ||
	    bdev->internal.status == SPDK_BDEV_STATUS_REMOVING) {
		/* [한국어] bdev 해제 중 → 열기 거부 */
		spdk_spin_unlock(&bdev->internal.spinlock);
		return -ENODEV;
	}

	if (write && bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE) {
		/* [한국어] 쓰기 desc인데 이미 클레임 있음 → 충돌 거부 */
		LOG_ALREADY_CLAIMED_ERROR("already claimed", bdev);
		spdk_spin_unlock(&bdev->internal.spinlock);
		return -EPERM;
	}

	rc = bdev_start_qos(bdev); /* [한국어] QoS 활성화 (필요한 경우) */
	if (rc != 0) {
		SPDK_ERRLOG("Failed to start QoS on bdev %s\n", bdev->name);
		spdk_spin_unlock(&bdev->internal.spinlock);
		return rc;
	}

	TAILQ_INSERT_TAIL(&bdev->internal.open_descs, desc, link); /* [한국어] open_descs에 등록 */

	spdk_spin_unlock(&bdev->internal.spinlock);

	return 0;
}

/*
 * [한국어]
 * bdev_open_opts_get_defaults - bdev_open 옵션 기본값 설정
 *
 * @opts:      기본값을 채울 옵션 구조체
 * @opts_size: 구조체 크기
 *
 * opts를 0으로 초기화 후 size와 hide_metadata=false 기본값 설정.
 * ABI 호환성 유지를 위해 opts_size 범위 내 필드만 설정.
 *
 * 호출 체인:
 *   bdev_desc_alloc() / spdk_bdev_open_opts_init() → [이 함수]
 */
static void
bdev_open_opts_get_defaults(struct spdk_bdev_open_opts *opts, size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL.\n");
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero.\n");
		return;
	}

	memset(opts, 0, opts_size); /* [한국어] opts 구조체 0으로 초기화 */
	opts->size = opts_size;     /* [한국어] 구조체 크기 기록 (ABI 호환용) */

#define FIELD_OK(field) \
	offsetof(struct spdk_bdev_open_opts, field) + sizeof(opts->field) <= opts_size

#define SET_FIELD(field, value) \
	if (FIELD_OK(field)) { \
		opts->field = value; \
	} \

	SET_FIELD(hide_metadata, false); /* [한국어] hide_metadata 기본값: false */

#undef FIELD_OK
#undef SET_FIELD
}

/*
 * [한국어]
 * bdev_open_opts_copy - 사용자 옵션을 내부 opts 구조체로 복사
 *
 * @opts:      대상 옵션 구조체
 * @opts_src:  원본 사용자 옵션
 * @opts_size: 원본의 크기 (구버전 ABI 대응)
 *
 * opts_size 범위 내 필드만 복사해 ABI 호환성 보장.
 * 새 필드 추가 시 STATIC_ASSERT로 크기 변경 감지.
 *
 * 호출 체인:
 *   bdev_desc_alloc() / spdk_bdev_open_opts_init() → [이 함수]
 */
static void
bdev_open_opts_copy(struct spdk_bdev_open_opts *opts,
		    const struct spdk_bdev_open_opts *opts_src, size_t opts_size)
{
	assert(opts);
	assert(opts_src);

#define SET_FIELD(field) \
	if (offsetof(struct spdk_bdev_open_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = opts_src->field; \
	} \

	SET_FIELD(hide_metadata); /* [한국어] hide_metadata 복사 (opts_size 범위 내) */

	opts->size = opts_src->size; /* [한국어] 크기 필드 복사 */

	/* We should not remove this statement, but need to update the assert statement
	 * if we add a new field, and also add a corresponding SET_FIELD statement.
	 */
	/* [한국어] 새 필드 추가 시 이 assert와 SET_FIELD를 업데이트해야 함 */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_open_opts) == 16, "Incorrect size");

#undef SET_FIELD
}

/*
 * [한국어]
 * spdk_bdev_open_opts_init - 사용자에게 bdev_open_opts 기본값 초기화 제공
 *
 * @opts:      초기화할 옵션 구조체
 * @opts_size: 구조체 크기
 *
 * 내부 로컬 opts에 기본값 설정 후 사용자 opts에 복사.
 * ABI 호환성: opts_size로 구버전 구조체도 올바르게 처리.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → bdev_open_opts_get_defaults() + bdev_open_opts_copy()
 */
void
spdk_bdev_open_opts_init(struct spdk_bdev_open_opts *opts, size_t opts_size)
{
	struct spdk_bdev_open_opts opts_local;

	bdev_open_opts_get_defaults(&opts_local, sizeof(opts_local)); /* [한국어] 기본값 설정 */
	bdev_open_opts_copy(opts, &opts_local, opts_size);            /* [한국어] 사용자 opts에 복사 */
}

/*
 * [한국어]
 * bdev_desc_alloc - bdev 디스크립터 할당 및 기본 초기화
 *
 * @bdev:       대상 bdev
 * @event_cb:   bdev 이벤트 콜백 (REMOVE/MEDIA_ERR 등)
 * @event_ctx:  이벤트 콜백 컨텍스트
 * @user_opts:  사용자 제공 옵션 (NULL이면 기본값 사용)
 * @_desc:      생성된 디스크립터 출력
 * @return:     0=성공, -ENOMEM/-EINVAL
 *
 * desc 메모리 할당, opts 설정, 이벤트 큐 초기화, spinlock 초기화.
 * hide_metadata=true이고 separate metadata이면 -EINVAL.
 * media_events 지원 bdev이면 미디어 이벤트 풀 할당.
 *
 * 호출 체인:
 *   spdk_bdev_open_ext() → [이 함수] → bdev_open()
 */
static int
bdev_desc_alloc(struct spdk_bdev *bdev, spdk_bdev_event_cb_t event_cb, void *event_ctx,
		struct spdk_bdev_open_opts *user_opts, struct spdk_bdev_desc **_desc)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev_open_opts opts;
	unsigned int i;

	bdev_open_opts_get_defaults(&opts, sizeof(opts)); /* [한국어] 기본 옵션 설정 */
	if (user_opts != NULL) {
		bdev_open_opts_copy(&opts, user_opts, user_opts->size); /* [한국어] 사용자 옵션 복사 */
	}

	desc = calloc(1, sizeof(*desc));
	if (desc == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for bdev descriptor\n");
		return -ENOMEM;
	}

	desc->opts = opts; /* [한국어] 옵션 적용 */

	TAILQ_INIT(&desc->pending_media_events); /* [한국어] 미디어 이벤트 대기 큐 초기화 */
	TAILQ_INIT(&desc->free_media_events);    /* [한국어] 미디어 이벤트 프리 풀 초기화 */

	desc->callback.event_fn = event_cb; /* [한국어] 이벤트 콜백 함수 */
	desc->callback.ctx = event_ctx;     /* [한국어] 이벤트 콜백 컨텍스트 */
	spdk_spin_init(&desc->spinlock);    /* [한국어] desc spinlock 초기화 */

	if (desc->opts.hide_metadata) {
		if (spdk_bdev_is_md_separate(bdev)) {
			/* [한국어] hide_metadata=true + separate metadata → 지원 안 함 */
			SPDK_ERRLOG("hide_metadata option is not supported with separate metadata.\n");
			bdev_desc_free(desc);
			return -EINVAL;
		}
	}

	if (bdev->media_events) {
		/* [한국어] 미디어 이벤트 지원 bdev → 이벤트 풀 할당 */
		desc->media_events_buffer = calloc(MEDIA_EVENT_POOL_SIZE,
						   sizeof(*desc->media_events_buffer));
		if (desc->media_events_buffer == NULL) {
			SPDK_ERRLOG("Failed to initialize media event pool\n");
			bdev_desc_free(desc);
			return -ENOMEM;
		}

		for (i = 0; i < MEDIA_EVENT_POOL_SIZE; ++i) {
			/* [한국어] 이벤트 버퍼를 프리 풀에 등록 */
			TAILQ_INSERT_TAIL(&desc->free_media_events,
					  &desc->media_events_buffer[i], tailq);
		}
	}

	*_desc = desc;

	return 0;
}

/*
 * [한국어]
 * bdev_open_ext - bdev 이름으로 디스크립터 할당 및 열기 (내부 구현)
 *
 * @bdev_name: 열 bdev 이름
 * @write:     쓰기 가능 여부
 * @event_cb:  bdev 이벤트 콜백
 * @event_ctx: 이벤트 콜백 컨텍스트
 * @opts:      오픈 옵션 (NULL이면 기본값)
 * @_desc:     생성된 디스크립터 출력
 * @return:    0=성공, -ENODEV/-ENOMEM/-EPERM/-EINVAL
 *
 * g_bdev_mgr.spinlock 보유 상태에서 호출된다.
 * bdev_get_by_name() → bdev_desc_alloc() → bdev_open() 순서로 진행.
 *
 * 호출 체인:
 *   spdk_bdev_open_ext_v2() → [이 함수] → bdev_desc_alloc() + bdev_open()
 */
static int
bdev_open_ext(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
	      void *event_ctx, struct spdk_bdev_open_opts *opts,
	      struct spdk_bdev_desc **_desc)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	int rc;

	bdev = bdev_get_by_name(bdev_name); /* [한국어] 이름으로 bdev 탐색 */

	if (bdev == NULL) {
		/* [한국어] bdev 미존재 → -ENODEV */
		SPDK_NOTICELOG("Currently unable to find bdev with name: %s\n", bdev_name);
		return -ENODEV;
	}

	rc = bdev_desc_alloc(bdev, event_cb, event_ctx, opts, &desc); /* [한국어] desc 할당 */
	if (rc != 0) {
		return rc;
	}

	rc = bdev_open(bdev, write, desc); /* [한국어] 디스크립터 활성화 */
	if (rc != 0) {
		bdev_desc_free(desc); /* [한국어] 열기 실패 → desc 해제 */
		desc = NULL;
	}

	*_desc = desc;

	return rc;
}

/*
 * [한국어]
 * spdk_bdev_open_ext_v2 - bdev 이름으로 open (옵션 구조체 포함 공개 API v2)
 *
 * @bdev_name: 열 bdev 이름
 * @write:     쓰기 가능 여부
 * @event_cb:  이벤트 콜백 (필수)
 * @event_ctx: 이벤트 콜백 컨텍스트
 * @opts:      오픈 옵션 (NULL이면 기본값)
 * @_desc:     생성된 디스크립터 출력
 * @return:    0=성공, -EINVAL/-ENODEV/-EPERM/-ENOMEM
 *
 * g_bdev_mgr.spinlock 획득 후 bdev_open_ext() 호출.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → bdev_open_ext()
 */
int
spdk_bdev_open_ext_v2(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
		      void *event_ctx, struct spdk_bdev_open_opts *opts,
		      struct spdk_bdev_desc **_desc)
{
	int rc;

	if (event_cb == NULL) {
		SPDK_ERRLOG("Missing event callback function\n");
		return -EINVAL; /* [한국어] 이벤트 콜백 필수 */
	}

	spdk_spin_lock(&g_bdev_mgr.spinlock); /* [한국어] g_bdev_mgr 전역 상태 보호 */
	rc = bdev_open_ext(bdev_name, write, event_cb, event_ctx, opts, _desc);
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	return rc;
}

/*
 * [한국어]
 * spdk_bdev_open_ext - bdev 이름으로 open (옵션 없는 공개 API)
 *
 * @bdev_name: 열 bdev 이름
 * @write:     쓰기 가능 여부
 * @event_cb:  이벤트 콜백
 * @event_ctx: 이벤트 콜백 컨텍스트
 * @_desc:     생성된 디스크립터 출력
 * @return:    0=성공, 기타 오류
 *
 * opts=NULL로 spdk_bdev_open_ext_v2()를 호출하는 래퍼.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → spdk_bdev_open_ext_v2()
 */
int
spdk_bdev_open_ext(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
		   void *event_ctx, struct spdk_bdev_desc **_desc)
{
	return spdk_bdev_open_ext_v2(bdev_name, write, event_cb, event_ctx, NULL, _desc);
}

/*
 * [한국어] spdk_bdev_open_async_ctx - 비동기 bdev open 컨텍스트 구조체
 *
 * bdev가 아직 존재하지 않을 때 반복적으로 시도하는 비동기 open 작업의 상태를 저장.
 * 폴러가 100ms마다 bdev_open_ext()를 재시도.
 */
struct spdk_bdev_open_async_ctx {
	char					*bdev_name;
	/* [한국어] 열려는 bdev 이름 (heap 복사본).
	 * 설정자: spdk_bdev_open_async()에서 strdup().
	 * 읽는 자: _bdev_open_async()에서 bdev_open_ext() 호출 시.
	 * 값 범위: NULL 불가.
	 * 동기화: g_bdev_mgr.spinlock으로 보호. */

	spdk_bdev_event_cb_t			event_cb;
	/* [한국어] open된 bdev의 이벤트 콜백.
	 * 설정자: spdk_bdev_open_async() 호출 시 사용자 제공.
	 * 읽는 자: bdev_open_ext() 내부에서 bdev_desc_alloc()으로 전달.
	 * 값 범위: NULL 불가.
	 * 동기화: 단일 스레드 접근이므로 별도 락 불필요. */

	void					*event_ctx;
	/* [한국어] 이벤트 콜백 컨텍스트.
	 * 설정자: spdk_bdev_open_async().
	 * 읽는 자: bdev_open_ext() → bdev_desc_alloc().
	 * 값 범위: 사용자 포인터 (NULL 가능).
	 * 동기화: 단일 스레드. */

	bool					write;
	/* [한국어] 쓰기 가능 여부.
	 * 설정자: spdk_bdev_open_async().
	 * 읽는 자: _bdev_open_async() → bdev_open_ext().
	 * 값 범위: true/false.
	 * 동기화: 불변(read-only after init). */

	int					rc;
	/* [한국어] 최종 결과 코드.
	 * 설정자: _bdev_open_async()가 bdev_open_ext() 결과를 저장, -ESHUTDOWN은 취소 표시.
	 * 읽는 자: bdev_open_async_done()에서 콜백에 전달.
	 * 값 범위: 0=성공, -ENODEV/-ETIMEDOUT/-ESHUTDOWN 등.
	 * 동기화: g_bdev_mgr.spinlock으로 보호. */

	spdk_bdev_open_async_cb_t		cb_fn;
	/* [한국어] 완료 콜백 함수.
	 * 설정자: spdk_bdev_open_async().
	 * 읽는 자: bdev_open_async_done()에서 호출.
	 * 값 범위: NULL 불가.
	 * 동기화: 불변. */

	void					*cb_arg;
	/* [한국어] 완료 콜백 인수.
	 * 설정자: spdk_bdev_open_async().
	 * 읽는 자: bdev_open_async_done().
	 * 값 범위: 사용자 포인터.
	 * 동기화: 불변. */

	struct spdk_bdev_desc			*desc;
	/* [한국어] 성공 시 생성된 디스크립터.
	 * 설정자: _bdev_open_async()가 bdev_open_ext() 호출 후 설정.
	 * 읽는 자: bdev_open_async_done()에서 콜백에 전달.
	 * 값 범위: 성공 시 유효 포인터, 실패 시 NULL.
	 * 동기화: g_bdev_mgr.spinlock으로 보호. */

	struct spdk_bdev_open_async_opts	opts;
	/* [한국어] 비동기 오픈 옵션 (timeout_ms 등).
	 * 설정자: spdk_bdev_open_async()에서 초기화.
	 * 읽는 자: _bdev_open_async()에서 타임아웃 계산.
	 * 값 범위: size/timeout_ms 필드.
	 * 동기화: 불변. */

	uint64_t				start_ticks;
	/* [한국어] 오픈 시작 시 spdk_get_ticks() 값.
	 * 설정자: spdk_bdev_open_async().
	 * 읽는 자: _bdev_open_async()에서 타임아웃 계산에 사용.
	 * 값 범위: 0 초과.
	 * 동기화: 불변. */

	struct spdk_thread			*orig_thread;
	/* [한국어] spdk_bdev_open_async()를 호출한 원래 스레드.
	 * 설정자: spdk_bdev_open_async()에서 spdk_get_thread()로 캡처.
	 * 읽는 자: _bdev_open_async()가 완료 시 원래 스레드에 done 메시지 전송.
	 * 값 범위: 유효한 SPDK 스레드.
	 * 동기화: 불변. */

	struct spdk_poller			*poller;
	/* [한국어] 100ms 주기 재시도 폴러.
	 * 설정자: spdk_bdev_open_async()에서 SPDK_POLLER_REGISTER로 생성.
	 * 읽는 자: _bdev_open_async() 성공/타임아웃 시 spdk_poller_unregister().
	 * 값 범위: 유효한 폴러 포인터 또는 NULL.
	 * 동기화: 폴러는 orig_thread에서만 실행. */

	TAILQ_ENTRY(spdk_bdev_open_async_ctx)	tailq;
	/* [한국어] g_bdev_mgr.async_bdev_opens 리스트 연결 노드.
	 * 설정자: spdk_bdev_open_async()에서 INSERT.
	 * 읽는 자: bdev_open_async_fini()에서 순회/삭제.
	 * 동기화: g_bdev_mgr.spinlock으로 보호. */
};

/*
 * [한국어]
 * bdev_open_async_done - 비동기 open 완료 처리 (orig_thread 컨텍스트)
 *
 * @arg: spdk_bdev_open_async_ctx 포인터
 *
 * 결과 콜백(cb_fn)을 호출하고 ctx 메모리를 해제한다.
 * orig_thread로 메시지를 보내 실행되므로 항상 올바른 스레드 컨텍스트.
 *
 * 호출 체인:
 *   _bdev_open_async() 완료 → spdk_thread_send_msg(orig_thread, [이 함수])
 */
static void
bdev_open_async_done(void *arg)
{
	struct spdk_bdev_open_async_ctx *ctx = arg;

	ctx->cb_fn(ctx->desc, ctx->rc, ctx->cb_arg); /* [한국어] 결과를 사용자 콜백에 전달 */

	free(ctx->bdev_name); /* [한국어] bdev 이름 복사본 해제 */
	free(ctx);            /* [한국어] ctx 해제 */
}

/*
 * [한국어]
 * bdev_open_async_cancel - bdev 라이브러리 종료 시 대기 중 open 취소
 *
 * @arg: spdk_bdev_open_async_ctx 포인터
 *
 * ctx->rc == -ESHUTDOWN인 상태에서만 호출.
 * 폴러를 해제하고 bdev_open_async_done()으로 콜백 전달.
 *
 * 호출 체인:
 *   bdev_open_async_fini() → spdk_thread_send_msg(orig_thread, [이 함수])
 */
static void
bdev_open_async_cancel(void *arg)
{
	struct spdk_bdev_open_async_ctx *ctx = arg;

	assert(ctx->rc == -ESHUTDOWN); /* [한국어] 종료 취소 경로임을 검증 */

	spdk_poller_unregister(&ctx->poller); /* [한국어] 재시도 폴러 해제 */

	bdev_open_async_done(ctx);
}

/* This is called when the bdev library finishes at shutdown. */
/*
 * [한국어]
 * bdev_open_async_fini - bdev 라이브러리 종료 시 모든 대기 중 비동기 open 취소
 *
 * async_bdev_opens 리스트의 모든 ctx를 ESHUTDOWN으로 표시하고
 * 각 orig_thread에 bdev_open_async_cancel 메시지를 전송해 정리.
 * 폴러가 먼저 실행되면 done이 두 번 호출될 수 있으므로 -ESHUTDOWN 플래그로 방지.
 *
 * 호출 체인:
 *   bdev_finish_unregister_bdevs_iter() 계열 → [이 함수]
 */
static void
bdev_open_async_fini(void)
{
	struct spdk_bdev_open_async_ctx *ctx, *tmp_ctx;

	spdk_spin_lock(&g_bdev_mgr.spinlock); /* [한국어] async_bdev_opens 리스트 보호 */
	TAILQ_FOREACH_SAFE(ctx, &g_bdev_mgr.async_bdev_opens, tailq, tmp_ctx) {
		TAILQ_REMOVE(&g_bdev_mgr.async_bdev_opens, ctx, tailq); /* [한국어] 리스트에서 제거 */
		/*
		 * We have to move to ctx->orig_thread to unregister ctx->poller.
		 * However, there is a chance that ctx->poller is executed before
		 * message is executed, which could result in bdev_open_async_done()
		 * being called twice. To avoid such race condition, set ctx->rc to
		 * -ESHUTDOWN.
		 */
		ctx->rc = -ESHUTDOWN; /* [한국어] 종료 플래그 설정 (이중 done 방지) */
		spdk_thread_send_msg(ctx->orig_thread, bdev_open_async_cancel, ctx); /* [한국어] 원래 스레드에서 폴러 해제 */
	}
	spdk_spin_unlock(&g_bdev_mgr.spinlock);
}

static int bdev_open_async(void *arg);

/*
 * [한국어]
 * _bdev_open_async - 비동기 open 실제 시도 (g_bdev_mgr.spinlock 보유 상태에서 호출)
 *
 * @ctx: 비동기 open 컨텍스트
 *
 * bdev_open_ext()를 시도하고:
 *  - 성공하면 폴러 해제 + orig_thread에 done 전송
 *  - 실패하고 타임아웃 없으면 다음 폴러 호출 대기
 *  - 타임아웃이면 -ETIMEDOUT으로 완료
 *  - ESHUTDOWN이면 아무 것도 안 함 (bdev_open_async_cancel이 처리)
 *
 * 호출 체인:
 *   bdev_open_async() 폴러 → [이 함수] → bdev_open_ext()
 *   또는 spdk_bdev_open_async() 최초 등록 시 → [이 함수]
 */
static void
_bdev_open_async(struct spdk_bdev_open_async_ctx *ctx)
{
	uint64_t timeout_ticks;

	if (ctx->rc == -ESHUTDOWN) {
		/* This context is being canceled. Do nothing. */
		return; /* [한국어] 종료 취소 중 → 아무 것도 안 함 */
	}

	ctx->rc = bdev_open_ext(ctx->bdev_name, ctx->write, ctx->event_cb, ctx->event_ctx,
				NULL, &ctx->desc); /* [한국어] bdev open 시도 */
	if (ctx->rc == 0 || ctx->opts.timeout_ms == 0) {
		goto exit; /* [한국어] 성공 또는 타임아웃 없음 → 완료 */
	}

	timeout_ticks = ctx->start_ticks + ctx->opts.timeout_ms * spdk_get_ticks_hz() / 1000ull;
	/* [한국어] 타임아웃 tick 계산: start + timeout_ms를 tick 단위로 변환 */
	if (spdk_get_ticks() >= timeout_ticks) {
		/* [한국어] 타임아웃 초과 → -ETIMEDOUT으로 완료 */
		SPDK_ERRLOG("Timed out while waiting for bdev '%s' to appear\n", ctx->bdev_name);
		ctx->rc = -ETIMEDOUT;
		goto exit;
	}

	return; /* [한국어] 타임아웃 미초과 → 폴러가 다음번에 재시도 */

exit:
	spdk_poller_unregister(&ctx->poller); /* [한국어] 재시도 폴러 해제 */
	TAILQ_REMOVE(&g_bdev_mgr.async_bdev_opens, ctx, tailq); /* [한국어] 전역 open 목록에서 제거 */

	/* Completion callback is processed after stack unwinding. */
	/* [한국어] 스택 언와인드 후 완료 콜백 처리 (재진입 방지) */
	spdk_thread_send_msg(ctx->orig_thread, bdev_open_async_done, ctx);
}

/*
 * [한국어]
 * bdev_open_async - 100ms 주기 폴러에서 비동기 open 재시도
 *
 * @arg: spdk_bdev_open_async_ctx 포인터
 * @return: SPDK_POLLER_BUSY (항상 busy 반환 — 내부에서 unregister 됨)
 *
 * g_bdev_mgr.spinlock 획득 후 _bdev_open_async()를 호출.
 *
 * 호출 체인:
 *   SPDK poller 타이머 → [이 함수] → _bdev_open_async()
 */
static int
bdev_open_async(void *arg)
{
	struct spdk_bdev_open_async_ctx *ctx = arg;

	spdk_spin_lock(&g_bdev_mgr.spinlock); /* [한국어] bdev_open_ext 보호 */

	_bdev_open_async(ctx);

	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	return SPDK_POLLER_BUSY; /* [한국어] 항상 BUSY (폴러는 내부에서 unregister) */
}

/*
 * [한국어]
 * bdev_open_async_opts_copy - 비동기 open 옵션 복사 (ABI 호환)
 *
 * @opts:     대상 옵션 구조체
 * @opts_src: 원본 옵션
 * @size:     원본 구조체 크기
 *
 * 호출 체인:
 *   spdk_bdev_open_async() → [이 함수]
 */
static void
bdev_open_async_opts_copy(struct spdk_bdev_open_async_opts *opts,
			  struct spdk_bdev_open_async_opts *opts_src,
			  size_t size)
{
	assert(opts);
	assert(opts_src);

	opts->size = size; /* [한국어] 크기 기록 */

#define SET_FIELD(field) \
	if (offsetof(struct spdk_bdev_open_async_opts, field) + sizeof(opts->field) <= size) { \
		opts->field = opts_src->field; \
	} \

	SET_FIELD(timeout_ms); /* [한국어] 타임아웃 복사 */

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	/* [한국어] 새 필드 추가 시 이 assert와 SET_FIELD를 함께 업데이트 */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_open_async_opts) == 16, "Incorrect size");

#undef SET_FIELD
}

/*
 * [한국어]
 * bdev_open_async_opts_get_default - 비동기 open 옵션 기본값 설정
 *
 * @opts: 초기화할 옵션 구조체
 * @size: 구조체 크기
 *
 * timeout_ms=0 (무한 대기) 기본값.
 *
 * 호출 체인:
 *   spdk_bdev_open_async() → [이 함수]
 */
static void
bdev_open_async_opts_get_default(struct spdk_bdev_open_async_opts *opts, size_t size)
{
	assert(opts);

	opts->size = size; /* [한국어] 크기 기록 */

#define SET_FIELD(field, value) \
	if (offsetof(struct spdk_bdev_open_async_opts, field) + sizeof(opts->field) <= size) { \
		opts->field = value; \
	} \

	SET_FIELD(timeout_ms, 0); /* [한국어] timeout_ms 기본값: 0 (무한 대기) */

#undef SET_FIELD
}

/*
 * [한국어]
 * spdk_bdev_open_async - bdev 이름으로 비동기 open (bdev가 아직 없어도 대기 가능)
 *
 * @bdev_name:   열 bdev 이름
 * @write:       쓰기 가능 여부
 * @event_cb:    bdev 이벤트 콜백 (필수)
 * @event_ctx:   이벤트 콜백 컨텍스트
 * @opts:        비동기 옵션 (timeout_ms 등; NULL이면 기본값)
 * @open_cb:     open 완료 콜백 (필수)
 * @open_cb_arg: 완료 콜백 인수
 * @return:      0=폴러 등록 성공, -EINVAL/-ENOMEM
 *
 * 컨텍스트 할당 → 폴러 등록 → async_bdev_opens 리스트 추가 → 즉시 첫 시도.
 * bdev가 없으면 100ms마다 재시도, timeout_ms 경과 시 -ETIMEDOUT.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → _bdev_open_async() (즉시 첫 시도)
 *   + bdev_open_async() 폴러 (100ms 재시도)
 */
int
spdk_bdev_open_async(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
		     void *event_ctx, struct spdk_bdev_open_async_opts *opts,
		     spdk_bdev_open_async_cb_t open_cb, void *open_cb_arg)
{
	struct spdk_bdev_open_async_ctx *ctx;

	if (event_cb == NULL) {
		SPDK_ERRLOG("Missing event callback function\n");
		return -EINVAL; /* [한국어] 이벤트 콜백 필수 */
	}

	if (open_cb == NULL) {
		SPDK_ERRLOG("Missing open callback function\n");
		return -EINVAL; /* [한국어] 완료 콜백 필수 */
	}

	if (opts != NULL && opts->size == 0) {
		SPDK_ERRLOG("size in the options structure should not be zero\n");
		return -EINVAL; /* [한국어] opts->size=0 불가 */
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate open context\n");
		return -ENOMEM;
	}

	ctx->bdev_name = strdup(bdev_name); /* [한국어] bdev 이름 heap 복사 */
	if (ctx->bdev_name == NULL) {
		SPDK_ERRLOG("Failed to duplicate bdev_name\n");
		free(ctx);
		return -ENOMEM;
	}

	ctx->poller = SPDK_POLLER_REGISTER(bdev_open_async, ctx, 100 * SPDK_MSEC_TO_USEC);
	/* [한국어] 100ms 주기 재시도 폴러 등록 */
	if (ctx->poller == NULL) {
		SPDK_ERRLOG("Failed to register bdev_open_async poller\n");
		free(ctx->bdev_name);
		free(ctx);
		return -ENOMEM;
	}

	ctx->cb_fn = open_cb;          /* [한국어] 완료 콜백 */
	ctx->cb_arg = open_cb_arg;     /* [한국어] 완료 콜백 인수 */
	ctx->write = write;            /* [한국어] 쓰기 여부 */
	ctx->event_cb = event_cb;      /* [한국어] 이벤트 콜백 */
	ctx->event_ctx = event_ctx;    /* [한국어] 이벤트 컨텍스트 */
	ctx->orig_thread = spdk_get_thread(); /* [한국어] 원래 호출 스레드 저장 */
	ctx->start_ticks = spdk_get_ticks();  /* [한국어] 타임아웃 기준 시작 tick */

	bdev_open_async_opts_get_default(&ctx->opts, sizeof(ctx->opts)); /* [한국어] 기본 옵션 설정 */
	if (opts != NULL) {
		bdev_open_async_opts_copy(&ctx->opts, opts, opts->size); /* [한국어] 사용자 옵션 적용 */
	}

	spdk_spin_lock(&g_bdev_mgr.spinlock); /* [한국어] async_bdev_opens 리스트 보호 */

	TAILQ_INSERT_TAIL(&g_bdev_mgr.async_bdev_opens, ctx, tailq); /* [한국어] 전역 목록에 등록 */
	_bdev_open_async(ctx); /* [한국어] 즉시 첫 번째 시도 */

	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	return 0;
}

/*
 * [한국어]
 * bdev_close - bdev 디스크립터 닫기 (내부 구현)
 *
 * @bdev: 대상 bdev
 * @desc: 닫을 디스크립터
 *
 * open_descs에서 제거 → claimed 해제 → refs==0이면 desc 메모리 해제.
 * 마지막 desc이고 QoS 활성화 중이면 QoS 파괴.
 * bdev가 REMOVING 상태이고 open_descs가 비었으면 unregister 완료 처리.
 *
 * 호출 체인:
 *   spdk_bdev_close() → [이 함수]
 *   또는 bdev_register_finished() → [이 함수]
 */
static void
bdev_close(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc)
{
	int rc;

	spdk_spin_lock(&bdev->internal.spinlock); /* [한국어] open_descs/QoS/status 보호 */
	spdk_spin_lock(&desc->spinlock);          /* [한국어] desc->closed/refs 보호 */

	TAILQ_REMOVE(&bdev->internal.open_descs, desc, link); /* [한국어] open_descs에서 제거 */

	desc->closed = true; /* [한국어] 닫힌 상태 표시 (이후 event 전달 차단) */

	if (desc->claim != NULL) {
		bdev_desc_release_claims(desc); /* [한국어] v2 클레임 해제 */
	}

	if (0 == desc->refs) {
		/* [한국어] 참조 카운트 0 → desc 메모리 해제 */
		spdk_spin_unlock(&desc->spinlock);
		bdev_desc_free(desc);
	} else {
		/* [한국어] 참조 카운트 남음 → desc는 유지 (put_io_channel 등에서 감소 예정) */
		spdk_spin_unlock(&desc->spinlock);
	}

	/* If no more descriptors, kill QoS channel */
	if (bdev->internal.qos && TAILQ_EMPTY(&bdev->internal.open_descs)) {
		/* [한국어] 마지막 desc 닫힘 + QoS 활성화 → QoS 파괴 */
		SPDK_DEBUGLOG(bdev, "Closed last descriptor for bdev %s on thread %p. Stopping QoS.\n",
			      bdev->name, spdk_get_thread());

		if (bdev_qos_destroy(bdev)) {
			/* There isn't anything we can do to recover here. Just let the
			 * old QoS poller keep running. The QoS handling won't change
			 * cores when the user allocates a new channel, but it won't break. */
			/* [한국어] QoS 파괴 실패 → 폴러가 계속 실행되지만 기능 무방 */
			SPDK_ERRLOG("Unable to shut down QoS poller. It will continue running on the current thread.\n");
		}
	}

	if (bdev->internal.status == SPDK_BDEV_STATUS_REMOVING && TAILQ_EMPTY(&bdev->internal.open_descs)) {
		/* [한국어] REMOVING 상태 + 모든 desc 닫힘 → unregister 완료 처리 */
		rc = bdev_unregister_unsafe(bdev);
		spdk_spin_unlock(&bdev->internal.spinlock);

		if (rc == 0) {
			spdk_io_device_unregister(__bdev_to_io_dev(bdev), bdev_destroy_cb); /* [한국어] io_device 해제 → destroy_cb */
		}
	} else {
		spdk_spin_unlock(&bdev->internal.spinlock);
	}
}

/*
 * [한국어]
 * spdk_bdev_close - bdev 디스크립터 닫기 (공개 API)
 *
 * @desc: 닫을 디스크립터
 *
 * io_timeout_poller 해제 후 g_bdev_mgr.spinlock 획득 → bdev_close() 호출.
 * desc->thread와 현재 스레드가 일치해야 한다 (assert).
 *
 * 호출 체인:
 *   사용자 → [이 함수] → bdev_close()
 */
void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);

	SPDK_DEBUGLOG(bdev, "Closing descriptor %p for bdev %s on thread %p\n", desc, bdev->name,
		      spdk_get_thread());

	assert(desc->thread == spdk_get_thread()); /* [한국어] desc는 생성한 스레드에서만 닫을 수 있음 */

	spdk_poller_unregister(&desc->io_timeout_poller); /* [한국어] I/O 타임아웃 폴러 해제 */

	spdk_spin_lock(&g_bdev_mgr.spinlock); /* [한국어] bdev_close 내부 전역 상태 보호 */

	bdev_close(bdev, desc);

	spdk_spin_unlock(&g_bdev_mgr.spinlock);
}

/*
 * [한국어]
 * spdk_bdev_get_numa_id - bdev의 NUMA 노드 ID 반환
 *
 * @bdev:   대상 bdev
 * @return: NUMA ID (numa.id_valid=true 시) 또는 SPDK_ENV_NUMA_ID_ANY
 *
 * bdev->numa.id_valid 플래그를 보고 유효한 경우만 실제 NUMA ID 반환.
 *
 * 호출 체인:
 *   사용자 → [이 함수]
 */
int32_t
spdk_bdev_get_numa_id(struct spdk_bdev *bdev)
{
	if (bdev->numa.id_valid) {
		return bdev->numa.id; /* [한국어] 유효한 NUMA 노드 ID */
	} else {
		return SPDK_ENV_NUMA_ID_ANY; /* [한국어] NUMA 정보 없음 */
	}
}

/*
 * [한국어]
 * bdev_register_finished - bdev 등록 완료 후 "bdev_register" 알림 전송
 *
 * @arg: 임시 디스크립터 (desc)
 *
 * spdk_bdev_wait_for_examine() 완료 후 호출.
 * "bdev_register" 알림 전송 후 임시 desc 닫기.
 *
 * 호출 체인:
 *   spdk_bdev_wait_for_examine() → [이 함수]
 */
static void
bdev_register_finished(void *arg)
{
	struct spdk_bdev_desc *desc = arg;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);

	spdk_notify_send("bdev_register", spdk_bdev_get_name(bdev)); /* [한국어] bdev 등록 완료 알림 */

	spdk_spin_lock(&g_bdev_mgr.spinlock);

	bdev_close(bdev, desc); /* [한국어] 임시 desc 닫기 */

	spdk_spin_unlock(&g_bdev_mgr.spinlock);
}

/*
 * [한국어]
 * spdk_bdev_register - bdev를 SPDK에 등록 (공개 API)
 *
 * @bdev:   등록할 bdev
 * @return: 0=성공, -EINVAL/-ENOMEM 등
 *
 * 앱 스레드에서만 호출 가능.
 * bdev_register() 호출 후 임시 desc를 열어 examine 중 삭제 방지.
 * spdk_bdev_wait_for_examine() 완료 시 bdev_register_finished()에서 알림 후 desc 닫기.
 *
 * 호출 체인:
 *   모듈 초기화 → [이 함수] → bdev_register() + bdev_examine() + spdk_bdev_wait_for_examine()
 */
int
spdk_bdev_register(struct spdk_bdev *bdev)
{
	struct spdk_bdev_desc *desc;
	struct spdk_thread *thread = spdk_get_thread();
	int rc;

	if (spdk_unlikely(!spdk_thread_is_app_thread(NULL))) {
		/* [한국어] 앱 스레드가 아닌 경우 등록 불가 */
		SPDK_ERRLOG("Cannot register bdev %s on thread %p (%s)\n", bdev->name, thread,
			    thread ? spdk_thread_get_name(thread) : "null");
		return -EINVAL;
	}

	rc = bdev_register(bdev); /* [한국어] 내부 등록 (해시/리스트/UUID 등) */
	if (rc != 0) {
		return rc;
	}

	/* A descriptor is opened to prevent bdev deletion during examination */
	/* [한국어] examine 중 bdev 삭제 방지를 위한 임시 desc 생성 */
	rc = bdev_desc_alloc(bdev, _tmp_bdev_event_cb, NULL, NULL, &desc);
	if (rc != 0) {
		spdk_bdev_unregister(bdev, NULL, NULL);
		return rc;
	}

	rc = bdev_open(bdev, false, desc); /* [한국어] 임시 desc 활성화 (읽기 전용) */
	if (rc != 0) {
		bdev_desc_free(desc);
		spdk_bdev_unregister(bdev, NULL, NULL);
		return rc;
	}

	/* Examine configuration before initializing I/O */
	/* [한국어] I/O 허용 전 모든 모듈의 examine 완료 대기 */
	bdev_examine(bdev); /* [한국어] 모든 모듈의 examine_config/examine_disk 호출 */

	rc = spdk_bdev_wait_for_examine(bdev_register_finished, desc);
	/* [한국어] 모든 examine 완료 시 bdev_register_finished()에서 알림 전송 */
	if (rc != 0) {
		bdev_close(bdev, desc);
		spdk_bdev_unregister(bdev, NULL, NULL);
	}

	return rc;
}

/*
 * [한국어]
 * spdk_bdev_module_claim_bdev - bdev에 v1 독점 쓰기 클레임 설정
 *
 * @bdev:   클레임할 bdev
 * @desc:   쓰기 가능으로 승격할 desc (NULL 가능)
 * @module: 클레임 모듈
 * @return: 0=성공, -EPERM=이미 클레임됨
 *
 * SPDK_BDEV_CLAIM_EXCL_WRITE 타입으로 bdev를 단독 점유.
 *
 * 호출 체인:
 *   bdev 모듈 → [이 함수]
 *   또는 spdk_bdev_module_claim_bdev_desc(EXCL_WRITE) → [이 함수]
 */
int
spdk_bdev_module_claim_bdev(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_bdev_module *module)
{
	spdk_spin_lock(&bdev->internal.spinlock); /* [한국어] claim_type 접근 보호 */

	if (bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE) {
		/* [한국어] 이미 클레임 있음 → 거부 */
		LOG_ALREADY_CLAIMED_ERROR("already claimed", bdev);
		spdk_spin_unlock(&bdev->internal.spinlock);
		return -EPERM;
	}

	if (desc && !desc->write) {
		desc->write = true; /* [한국어] 읽기 전용 desc를 쓰기 가능으로 승격 */
	}

	bdev->internal.claim_type = SPDK_BDEV_CLAIM_EXCL_WRITE; /* [한국어] v1 독점 쓰기 클레임 설정 */
	bdev->internal.claim.v1.module = module; /* [한국어] 클레임 모듈 기록 */

	spdk_spin_unlock(&bdev->internal.spinlock);
	return 0;
}

/*
 * [한국어]
 * spdk_bdev_module_release_bdev - bdev의 v1 독점 쓰기 클레임 해제
 *
 * @bdev: 클레임 해제할 bdev
 *
 * EXCL_WRITE 클레임을 NONE으로 초기화.
 *
 * 호출 체인:
 *   bdev 모듈 → [이 함수]
 */
void
spdk_bdev_module_release_bdev(struct spdk_bdev *bdev)
{
	spdk_spin_lock(&bdev->internal.spinlock); /* [한국어] claim_type/claim 접근 보호 */

	assert(bdev->internal.claim.v1.module != NULL);
	assert(bdev->internal.claim_type == SPDK_BDEV_CLAIM_EXCL_WRITE);
	bdev->internal.claim_type = SPDK_BDEV_CLAIM_NONE; /* [한국어] 클레임 타입 초기화 */
	bdev->internal.claim.v1.module = NULL;             /* [한국어] 클레임 모듈 초기화 */

	spdk_spin_unlock(&bdev->internal.spinlock);
}

/*
 * Start claims v2
 */

/*
 * [한국어]
 * spdk_bdev_claim_get_name - 클레임 타입을 문자열로 반환
 *
 * @type:   클레임 타입 enum
 * @return: 타입 이름 문자열
 *
 * 로그 출력 및 디버그용. 알 수 없는 타입은 "invalid_claim".
 *
 * 호출 체인:
 *   log_already_claimed() / 사용자 → [이 함수]
 */
const char *
spdk_bdev_claim_get_name(enum spdk_bdev_claim_type type)
{
	switch (type) {
	case SPDK_BDEV_CLAIM_NONE:
		return "not_claimed";
	case SPDK_BDEV_CLAIM_EXCL_WRITE:
		return "exclusive_write";
	case SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE:
		return "read_many_write_one";
	case SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE:
		return "read_many_write_none";
	case SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED:
		return "read_many_write_shared";
	default:
		break;
	}
	return "invalid_claim"; /* [한국어] 알 수 없는 타입 */
}

/*
 * [한국어]
 * claim_type_is_v2 - 클레임 타입이 v2인지 확인
 *
 * @type:   클레임 타입
 * @return: true=v2 타입 (READ_MANY_*)
 *
 * v2 클레임은 다중 클레임자를 지원하며 TAILQ로 관리.
 *
 * 호출 체인:
 *   여러 claim 관련 함수 → [이 함수]
 */
static bool
claim_type_is_v2(enum spdk_bdev_claim_type type)
{
	switch (type) {
	case SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE:
	case SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE:
	case SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED:
		return true; /* [한국어] v2 타입 */
	default:
		break;
	}
	return false; /* [한국어] v1 또는 NONE */
}

/* Returns true if taking a claim with desc->write == false should make the descriptor writable. */
/*
 * [한국어]
 * claim_type_promotes_to_write - 읽기 전용 desc를 쓰기 가능으로 승격하는 클레임 타입인지
 *
 * @type:   클레임 타입
 * @return: true=쓰기 승격 필요 (RWO, RWM)
 *
 * READ_MANY_WRITE_NONE은 읽기 전용이므로 승격 안 함.
 *
 * 호출 체인:
 *   claim_bdev() → [이 함수]
 */
static bool
claim_type_promotes_to_write(enum spdk_bdev_claim_type type)
{
	switch (type) {
	case SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE:
	case SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED:
		return true; /* [한국어] 쓰기 승격 필요 */
	default:
		break;
	}
	return false; /* [한국어] READ_ONLY_MANY는 쓰기 승격 안 함 */
}

/*
 * [한국어]
 * spdk_bdev_claim_opts_init - v2 클레임 옵션 기본값 초기화
 *
 * @opts: 초기화할 클레임 옵션
 * @size: 구조체 크기
 *
 * shared_claim_key=0 기본값 설정. ABI 호환 위해 size 범위 내 필드만.
 *
 * 호출 체인:
 *   spdk_bdev_module_claim_bdev_desc() / 사용자 → [이 함수]
 */
void
spdk_bdev_claim_opts_init(struct spdk_bdev_claim_opts *opts, size_t size)
{
	if (opts == NULL) {
		SPDK_ERRLOG("opts should not be NULL\n");
		assert(opts != NULL);
		return;
	}
	if (size == 0) {
		SPDK_ERRLOG("size should not be zero\n");
		assert(size != 0);
		return;
	}

	memset(opts, 0, size); /* [한국어] opts 구조체 0 초기화 */
	opts->opts_size = size; /* [한국어] 크기 기록 */

#define FIELD_OK(field) \
        offsetof(struct spdk_bdev_claim_opts, field) + sizeof(opts->field) <= size

#define SET_FIELD(field, value) \
        if (FIELD_OK(field)) { \
                opts->field = value; \
        } \

	SET_FIELD(shared_claim_key, 0); /* [한국어] shared_claim_key 기본값: 0 */

#undef FIELD_OK
#undef SET_FIELD
}

/*
 * [한국어]
 * claim_opts_copy - v2 클레임 옵션 복사 (ABI 호환)
 *
 * @src: 원본 옵션
 * @dst: 대상 옵션
 * @return: 0=성공, -1=opts_size==0
 *
 * 호출 체인:
 *   spdk_bdev_module_claim_bdev_desc() → [이 함수]
 */
static int
claim_opts_copy(struct spdk_bdev_claim_opts *src, struct spdk_bdev_claim_opts *dst)
{
	if (src->opts_size == 0) {
		SPDK_ERRLOG("size should not be zero\n");
		return -1;
	}

	memset(dst, 0, sizeof(*dst)); /* [한국어] dst 초기화 */
	dst->opts_size = src->opts_size; /* [한국어] 크기 복사 */

#define FIELD_OK(field) \
        offsetof(struct spdk_bdev_claim_opts, field) + sizeof(src->field) <= src->opts_size

#define SET_FIELD(field) \
        if (FIELD_OK(field)) { \
                dst->field = src->field; \
        } \

	if (FIELD_OK(name)) {
		snprintf(dst->name, sizeof(dst->name), "%s", src->name); /* [한국어] name 필드 복사 */
	}

	SET_FIELD(shared_claim_key); /* [한국어] shared_claim_key 복사 */

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	/* [한국어] 새 필드 추가 시 assert 크기와 SET_FIELD를 함께 업데이트 */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_claim_opts) == 48, "Incorrect size");

#undef FIELD_OK
#undef SET_FIELD
	return 0;
}

/* Returns 0 if a read-write-once claim can be taken. */
/*
 * [한국어]
 * claim_verify_rwo - READ_MANY_WRITE_ONE 클레임 취득 가능 여부 검증
 *
 * @desc:   클레임할 디스크립터
 * @type:   READ_MANY_WRITE_ONE
 * @opts:   클레임 옵션
 * @module: 클레임 모듈
 * @return: 0=가능, -EINVAL/-EPERM
 *
 * 조건: bdev에 다른 클레임 없음, desc에 기존 클레임 없음, 다른 write desc 없음.
 * shared_claim_key 옵션은 RWO에서 지원 안 함.
 *
 * 호출 체인:
 *   spdk_bdev_module_claim_bdev_desc() → [이 함수]
 */
static int
claim_verify_rwo(struct spdk_bdev_desc *desc, enum spdk_bdev_claim_type type,
		 struct spdk_bdev_claim_opts *opts, struct spdk_bdev_module *module)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_desc *open_desc;

	assert(spdk_spin_held(&bdev->internal.spinlock));
	assert(type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE);

	if (opts->shared_claim_key != 0) {
		/* [한국어] RWO에서는 key 옵션 불지원 */
		SPDK_ERRLOG("%s: key option not supported with read-write-once claims\n",
			    bdev->name);
		return -EINVAL;
	}
	if (bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE) {
		/* [한국어] bdev에 기존 클레임 있음 */
		LOG_ALREADY_CLAIMED_ERROR("already claimed", bdev);
		return -EPERM;
	}
	if (desc->claim != NULL) {
		/* [한국어] 동일 desc에 기존 클레임 있음 */
		SPDK_NOTICELOG("%s: descriptor already claimed bdev with module %s\n",
			       bdev->name, desc->claim->module->name);
		return -EPERM;
	}
	TAILQ_FOREACH(open_desc, &bdev->internal.open_descs, link) {
		if (desc != open_desc && open_desc->write) {
			/* [한국어] 다른 write desc가 열려 있음 → RWO 불가 */
			SPDK_NOTICELOG("%s: Cannot obtain read-write-once claim while "
				       "another descriptor is open for writing\n",
				       bdev->name);
			return -EPERM;
		}
	}

	return 0;
}

/* Returns 0 if a read-only-many claim can be taken. */
/*
 * [한국어]
 * claim_verify_rom - READ_MANY_WRITE_NONE 클레임 취득 가능 여부 검증
 *
 * @desc:   클레임할 디스크립터 (읽기 전용이어야 함)
 * @type:   READ_MANY_WRITE_NONE
 * @opts:   클레임 옵션
 * @module: 클레임 모듈
 * @return: 0=가능, -EINVAL/-EPERM
 *
 * desc가 write=true이거나 shared_claim_key가 있으면 -EINVAL.
 * 기존 클레임이 없는 경우 다른 write desc가 없어야 함.
 *
 * 호출 체인:
 *   spdk_bdev_module_claim_bdev_desc() → [이 함수]
 */
static int
claim_verify_rom(struct spdk_bdev_desc *desc, enum spdk_bdev_claim_type type,
		 struct spdk_bdev_claim_opts *opts, struct spdk_bdev_module *module)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_desc *open_desc;

	assert(spdk_spin_held(&bdev->internal.spinlock));
	assert(type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE);
	assert(desc->claim == NULL);

	if (desc->write) {
		/* [한국어] 쓰기 desc로는 read-only 클레임 불가 */
		SPDK_ERRLOG("%s: Cannot obtain read-only-many claim with writable descriptor\n",
			    bdev->name);
		return -EINVAL;
	}
	if (opts->shared_claim_key != 0) {
		/* [한국어] ROM에서는 key 옵션 불지원 */
		SPDK_ERRLOG("%s: key option not supported with read-only-may claims\n", bdev->name);
		return -EINVAL;
	}
	if (bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE) {
		/* [한국어] 첫 번째 클레임이면 다른 write desc가 없어야 함 */
		TAILQ_FOREACH(open_desc, &bdev->internal.open_descs, link) {
			if (open_desc->write) {
				SPDK_NOTICELOG("%s: Cannot obtain read-only-many claim while "
					       "another descriptor is open for writing\n",
					       bdev->name);
				return -EPERM;
			}
		}
	}

	return 0;
}

/* Returns 0 if a read-write-many claim can be taken. */
/*
 * [한국어]
 * claim_verify_rwm - READ_MANY_WRITE_SHARED 클레임 취득 가능 여부 검증
 *
 * @desc:   클레임할 디스크립터
 * @type:   READ_MANY_WRITE_SHARED
 * @opts:   클레임 옵션 (shared_claim_key 필수)
 * @module: 클레임 모듈
 * @return: 0=가능, -EINVAL/-EPERM/-EBUSY
 *
 * shared_claim_key=0이면 -EINVAL.
 * 기존 클레임이 RWM인데 key가 다르면 -EPERM.
 * 다른 타입이면 -EBUSY.
 *
 * 호출 체인:
 *   spdk_bdev_module_claim_bdev_desc() → [이 함수]
 */
static int
claim_verify_rwm(struct spdk_bdev_desc *desc, enum spdk_bdev_claim_type type,
		 struct spdk_bdev_claim_opts *opts, struct spdk_bdev_module *module)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_desc *open_desc;

	assert(spdk_spin_held(&bdev->internal.spinlock));
	assert(type == SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED);
	assert(desc->claim == NULL);

	if (opts->shared_claim_key == 0) {
		/* [한국어] RWM은 shared_claim_key 필수 */
		SPDK_ERRLOG("%s: shared_claim_key option required with read-write-may claims\n",
			    bdev->name);
		return -EINVAL;
	}
	switch (bdev->internal.claim_type) {
	case SPDK_BDEV_CLAIM_NONE:
		/* [한국어] 첫 번째 클레임 → 클레임 없는 write desc 없어야 함 */
		TAILQ_FOREACH(open_desc, &bdev->internal.open_descs, link) {
			if (open_desc == desc) {
				continue;
			}
			if (open_desc->write) {
				SPDK_NOTICELOG("%s: Cannot obtain read-write-many claim while "
					       "another descriptor is open for writing without a "
					       "claim\n", bdev->name);
				return -EPERM;
			}
		}
		break;
	case SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED:
		if (opts->shared_claim_key != bdev->internal.claim.v2.key) {
			/* [한국어] 다른 key로 이미 클레임됨 */
			LOG_ALREADY_CLAIMED_ERROR("already claimed with another key", bdev);
			return -EPERM;
		}
		break;
	default:
		/* [한국어] 다른 클레임 타입으로 이미 클레임됨 */
		LOG_ALREADY_CLAIMED_ERROR("already claimed", bdev);
		return -EBUSY;
	}

	return 0;
}

/* Updates desc and its bdev with a v2 claim. */
/*
 * [한국어]
 * claim_bdev - desc와 bdev에 v2 클레임 설정
 *
 * @desc:   클레임할 디스크립터
 * @type:   v2 클레임 타입
 * @opts:   클레임 옵션
 * @module: 클레임 모듈
 * @return: 0=성공, -ENOMEM
 *
 * spdk_bdev_module_claim 구조체 할당 → desc->claim에 설정 →
 * bdev->internal.claim.v2.claims에 추가.
 * 처음 클레임이면 bdev claim_type/key 초기화.
 *
 * 호출 체인:
 *   spdk_bdev_module_claim_bdev_desc() → claim_verify_*() → [이 함수]
 */
static int
claim_bdev(struct spdk_bdev_desc *desc, enum spdk_bdev_claim_type type,
	   struct spdk_bdev_claim_opts *opts, struct spdk_bdev_module *module)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_module_claim *claim;

	assert(spdk_spin_held(&bdev->internal.spinlock));
	assert(claim_type_is_v2(type));
	assert(desc->claim == NULL);

	claim = calloc(1, sizeof(*desc->claim)); /* [한국어] 클레임 구조체 할당 */
	if (claim == NULL) {
		SPDK_ERRLOG("%s: out of memory while allocating claim\n", bdev->name);
		return -ENOMEM;
	}
	claim->module = module;  /* [한국어] 클레임 모듈 */
	claim->desc = desc;      /* [한국어] 역참조: 소유 desc */
	SPDK_STATIC_ASSERT(sizeof(claim->name) == sizeof(opts->name), "sizes must match");
	memcpy(claim->name, opts->name, sizeof(claim->name)); /* [한국어] 클레임 이름 복사 */
	desc->claim = claim; /* [한국어] desc에 클레임 연결 */

	if (bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE) {
		/* [한국어] 첫 번째 클레임 → bdev에 타입/key 초기화 */
		bdev->internal.claim_type = type;
		TAILQ_INIT(&bdev->internal.claim.v2.claims);
		bdev->internal.claim.v2.key = opts->shared_claim_key;
	}
	assert(type == bdev->internal.claim_type);

	TAILQ_INSERT_TAIL(&bdev->internal.claim.v2.claims, claim, link); /* [한국어] 클레임 목록에 추가 */

	if (!desc->write && claim_type_promotes_to_write(type)) {
		desc->write = true; /* [한국어] RWO/RWM → 읽기 전용 desc를 쓰기 가능으로 승격 */
	}

	return 0;
}

/*
 * [한국어]
 * spdk_bdev_module_claim_bdev_desc - desc를 통해 v2 클레임 취득 (공개 API)
 *
 * @desc:    클레임할 디스크립터
 * @type:    클레임 타입 (EXCL_WRITE → v1 함수로 위임, 나머지 v2 처리)
 * @_opts:   클레임 옵션 (NULL이면 기본값)
 * @module:  클레임 모듈
 * @return:  0=성공, -EINVAL/-EPERM/-ENOMEM/-ENOTSUP
 *
 * 타입에 따라 claim_verify_rwo/rom/rwm() 검증 후 claim_bdev() 설정.
 * EXCL_WRITE는 기존 v1 함수로 위임.
 *
 * 호출 체인:
 *   bdev 모듈 → [이 함수] → claim_verify_*() + claim_bdev()
 */
int
spdk_bdev_module_claim_bdev_desc(struct spdk_bdev_desc *desc, enum spdk_bdev_claim_type type,
				 struct spdk_bdev_claim_opts *_opts,
				 struct spdk_bdev_module *module)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_claim_opts opts;
	int rc = 0;

	if (desc == NULL) {
		SPDK_ERRLOG("descriptor must not be NULL\n");
		return -EINVAL;
	}

	bdev = desc->bdev;

	if (_opts == NULL) {
		spdk_bdev_claim_opts_init(&opts, sizeof(opts)); /* [한국어] 기본 옵션 초기화 */
	} else if (claim_opts_copy(_opts, &opts) != 0) {
		return -EINVAL; /* [한국어] opts_size=0 오류 */
	}

	spdk_spin_lock(&bdev->internal.spinlock); /* [한국어] claim_type/claim 접근 보호 */

	if (bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE &&
	    bdev->internal.claim_type != type) {
		/* [한국어] 다른 타입으로 이미 클레임됨 */
		LOG_ALREADY_CLAIMED_ERROR("already claimed", bdev);
		spdk_spin_unlock(&bdev->internal.spinlock);
		return -EPERM;
	}

	if (claim_type_is_v2(type) && desc->claim != NULL) {
		/* [한국어] 동일 desc에 이미 v2 클레임 있음 */
		SPDK_ERRLOG("%s: descriptor already has %s claim with name '%s'\n",
			    bdev->name, spdk_bdev_claim_get_name(type), desc->claim->name);
		spdk_spin_unlock(&bdev->internal.spinlock);
		return -EPERM;
	}

	switch (type) {
	case SPDK_BDEV_CLAIM_EXCL_WRITE:
		/* [한국어] EXCL_WRITE → v1 함수로 위임 (spinlock 해제 후 호출) */
		spdk_spin_unlock(&bdev->internal.spinlock);
		return spdk_bdev_module_claim_bdev(bdev, desc, module);
	case SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE:
		rc = claim_verify_rwo(desc, type, &opts, module); /* [한국어] RWO 검증 */
		break;
	case SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE:
		rc = claim_verify_rom(desc, type, &opts, module); /* [한국어] ROM 검증 */
		break;
	case SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED:
		rc = claim_verify_rwm(desc, type, &opts, module); /* [한국어] RWM 검증 */
		break;
	default:
		SPDK_ERRLOG("%s: claim type %d not supported\n", bdev->name, type);
		rc = -ENOTSUP;
	}

	if (rc == 0) {
		rc = claim_bdev(desc, type, &opts, module); /* [한국어] 검증 통과 → 클레임 설정 */
	}

	spdk_spin_unlock(&bdev->internal.spinlock);
	return rc;
}

/*
 * [한국어]
 * claim_reset - v2 클레임이 모두 해제된 후 bdev 클레임 상태 초기화
 *
 * @bdev: 대상 bdev (spinlock 보유 상태)
 *
 * v2.claims가 비어있는 경우 호출.
 * claim 구조체 및 claim_type을 NONE으로 초기화.
 *
 * 호출 체인:
 *   bdev_desc_release_claims() → [이 함수]
 */
static void
claim_reset(struct spdk_bdev *bdev)
{
	assert(spdk_spin_held(&bdev->internal.spinlock));
	assert(claim_type_is_v2(bdev->internal.claim_type));
	assert(TAILQ_EMPTY(&bdev->internal.claim.v2.claims));

	memset(&bdev->internal.claim, 0, sizeof(bdev->internal.claim)); /* [한국어] claim 구조체 초기화 */
	bdev->internal.claim_type = SPDK_BDEV_CLAIM_NONE; /* [한국어] 클레임 없음으로 초기화 */
}

/*
 * [한국어]
 * bdev_desc_release_claims - desc 닫힐 때 v2 클레임 해제
 *
 * @desc: 닫히는 디스크립터 (bdev spinlock 보유 상태)
 *
 * examine 진행 중이면 module/desc를 NULL로 무효화 (나중에 examine 완료 후 정리).
 * 아니면 즉시 TAILQ에서 제거 → 모든 클레임 해제되면 claim_reset().
 *
 * 호출 체인:
 *   bdev_close() → [이 함수]
 */
static void
bdev_desc_release_claims(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = desc->bdev;

	assert(spdk_spin_held(&bdev->internal.spinlock));
	assert(claim_type_is_v2(bdev->internal.claim_type));

	if (bdev->internal.examine_in_progress == 0) {
		/* [한국어] examine 진행 중 아님 → 즉시 클레임 해제 */
		TAILQ_REMOVE(&bdev->internal.claim.v2.claims, desc->claim, link);
		free(desc->claim); /* [한국어] 클레임 구조체 해제 */
		if (TAILQ_EMPTY(&bdev->internal.claim.v2.claims)) {
			claim_reset(bdev); /* [한국어] 모든 클레임 해제 → bdev 초기화 */
		}
	} else {
		/* This is a dead claim that will be cleaned up when bdev_examine() is done. */
		/* [한국어] examine 중 → 무효화만 하고 나중에 정리 */
		desc->claim->module = NULL;
		desc->claim->desc = NULL;
	}
	desc->claim = NULL; /* [한국어] desc에서 클레임 참조 제거 */
}

/*
 * End claims v2
 */

/*
 * [한국어]
 * spdk_bdev_desc_get_bdev - 디스크립터에서 bdev 포인터 조회
 *
 * @desc:   대상 디스크립터
 * @return: desc->bdev
 *
 * 호출 체인:
 *   여러 공개 API → [이 함수]
 */
struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	assert(desc != NULL);
	return desc->bdev; /* [한국어] desc의 bdev 포인터 반환 */
}

/*
 * [한국어]
 * spdk_for_each_bdev - 등록된 모든 bdev에 대해 콜백 실행
 *
 * @ctx: 사용자 컨텍스트
 * @fn:  각 bdev에 적용할 콜백 함수
 * @return: 0=전체 성공, fn이 반환한 첫 번째 오류 코드
 *
 * 각 bdev에 임시 desc를 열고 fn 호출 후 닫음.
 * bdev 열기 실패(-ENODEV)는 건너뜀 (bdev가 삭제 중일 수 있음).
 *
 * 호출 체인:
 *   사용자 → [이 함수] → bdev_open() + fn(ctx, bdev) + bdev_close()
 */
int
spdk_for_each_bdev(void *ctx, spdk_for_each_bdev_fn fn)
{
	struct spdk_bdev *bdev, *tmp;
	struct spdk_bdev_desc *desc;
	int rc = 0;

	assert(fn != NULL);

	spdk_spin_lock(&g_bdev_mgr.spinlock);
	bdev = spdk_bdev_first(); /* [한국어] 첫 번째 bdev */
	while (bdev != NULL) {
		rc = bdev_desc_alloc(bdev, _tmp_bdev_event_cb, NULL, NULL, &desc);
		if (rc != 0) {
			break; /* [한국어] desc 할당 실패 */
		}
		rc = bdev_open(bdev, false, desc);
		if (rc != 0) {
			bdev_desc_free(desc);
			if (rc == -ENODEV) {
				/* Ignore the error and move to the next bdev. */
				/* [한국어] 삭제 중 bdev → 건너뜀 */
				rc = 0;
				bdev = spdk_bdev_next(bdev);
				continue;
			}
			break;
		}
		spdk_spin_unlock(&g_bdev_mgr.spinlock); /* [한국어] fn 호출 중 락 해제 */

		rc = fn(ctx, bdev); /* [한국어] 사용자 콜백 호출 */

		spdk_spin_lock(&g_bdev_mgr.spinlock); /* [한국어] fn 완료 후 락 재획득 */
		tmp = spdk_bdev_next(bdev);   /* [한국어] 다음 bdev 사전 저장 (close 전) */
		bdev_close(bdev, desc);       /* [한국어] 임시 desc 닫기 */
		if (rc != 0) {
			break; /* [한국어] fn 오류 → 순회 중단 */
		}
		bdev = tmp;
	}
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	return rc;
}

/*
 * [한국어]
 * spdk_for_each_bdev_leaf - 리프(자식 없는) bdev들에 대해 콜백 실행
 *
 * @ctx: 사용자 컨텍스트
 * @fn:  각 리프 bdev에 적용할 콜백
 * @return: 0=성공, 또는 fn 반환 오류
 *
 * spdk_bdev_first_leaf()/next_leaf()로 vbdev가 아닌 물리 bdev만 순회.
 *
 * 호출 체인:
 *   사용자 → [이 함수]
 */
int
spdk_for_each_bdev_leaf(void *ctx, spdk_for_each_bdev_fn fn)
{
	struct spdk_bdev *bdev, *tmp;
	struct spdk_bdev_desc *desc;
	int rc = 0;

	assert(fn != NULL);

	spdk_spin_lock(&g_bdev_mgr.spinlock);
	bdev = spdk_bdev_first_leaf(); /* [한국어] 첫 번째 리프 bdev */
	while (bdev != NULL) {
		rc = bdev_desc_alloc(bdev, _tmp_bdev_event_cb, NULL, NULL, &desc);
		if (rc != 0) {
			break;
		}
		rc = bdev_open(bdev, false, desc);
		if (rc != 0) {
			bdev_desc_free(desc);
			if (rc == -ENODEV) {
				/* Ignore the error and move to the next bdev. */
				/* [한국어] 삭제 중 → 건너뜀 */
				rc = 0;
				bdev = spdk_bdev_next_leaf(bdev);
				continue;
			}
			break;
		}
		spdk_spin_unlock(&g_bdev_mgr.spinlock);

		rc = fn(ctx, bdev); /* [한국어] 사용자 콜백 */

		spdk_spin_lock(&g_bdev_mgr.spinlock);
		tmp = spdk_bdev_next_leaf(bdev);
		bdev_close(bdev, desc);
		if (rc != 0) {
			break;
		}
		bdev = tmp;
	}
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	return rc;
}

/*
 * [한국어]
 * spdk_for_each_bdev_by_name - 이름 목록의 bdev들에 대해 콜백 실행
 *
 * @ctx:   사용자 컨텍스트
 * @fn:    각 bdev에 적용할 콜백
 * @names: bdev 이름 배열
 * @count: 이름 배열 크기
 * @return: 0=전체 성공, 또는 첫 번째 오류
 *
 * 각 이름으로 spdk_bdev_open_ext() → fn() → spdk_bdev_close() 순서.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → spdk_bdev_open_ext() + fn() + spdk_bdev_close()
 */
int
spdk_for_each_bdev_by_name(void *ctx, spdk_for_each_bdev_fn fn, const char **names, size_t count)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	int rc = 0;
	size_t i = 0;

	assert(fn != NULL);

	for (i = 0; i < count; i++) {
		rc = spdk_bdev_open_ext(names[i], false, _tmp_bdev_event_cb, NULL, &desc);
		/* [한국어] 이름으로 bdev 열기 */
		if (rc != 0) {
			SPDK_DEBUGLOG(bdev, "Failed to open bdev '%s': %d\n", names[i], rc);
			break;
		}
		bdev = spdk_bdev_desc_get_bdev(desc); /* [한국어] desc에서 bdev 포인터 획득 */
		rc = fn(ctx, bdev);                   /* [한국어] 사용자 콜백 */
		spdk_bdev_close(desc);                /* [한국어] 임시 desc 닫기 */
		if (rc != 0) {
			break;
		}
	}

	return rc;
}

/*
 * [한국어]
 * spdk_bdev_io_get_iovec - bdev_io의 iovec 배열 조회
 *
 * @bdev_io:  대상 bdev I/O
 * @iovp:     iovec 배열 포인터 출력 (NULL이면 무시)
 * @iovcntp:  iovec 개수 출력 (NULL이면 무시)
 *
 * READ/WRITE/ZCOPY 타입만 iovec 유효. 나머지는 NULL/0.
 *
 * 호출 체인:
 *   사용자 완료 콜백 → [이 함수]
 */
void
spdk_bdev_io_get_iovec(struct spdk_bdev_io *bdev_io, struct iovec **iovp, int *iovcntp)
{
	struct iovec *iovs;
	int iovcnt;

	if (bdev_io == NULL) {
		return; /* [한국어] NULL bdev_io → 무시 */
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		/* [한국어] 데이터 전송 타입 → u.bdev.iovs 유효 */
		iovs = bdev_io->u.bdev.iovs;
		iovcnt = bdev_io->u.bdev.iovcnt;
		break;
	default:
		/* [한국어] 다른 타입 (FLUSH/UNMAP 등) → iovec 없음 */
		iovs = NULL;
		iovcnt = 0;
		break;
	}

	if (iovp) {
		*iovp = iovs; /* [한국어] iovec 배열 포인터 반환 */
	}
	if (iovcntp) {
		*iovcntp = iovcnt; /* [한국어] iovec 개수 반환 */
	}
}

/*
 * [한국어]
 * spdk_bdev_io_get_md_buf - bdev_io의 메타데이터 버퍼 조회
 *
 * @bdev_io:  대상 bdev I/O
 * @return:   separate metadata 버퍼 포인터, 또는 NULL
 *
 * separate metadata가 없거나 READ/WRITE 타입이 아니면 NULL.
 *
 * 호출 체인:
 *   사용자 완료 콜백 → [이 함수]
 */
void *
spdk_bdev_io_get_md_buf(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io == NULL) {
		return NULL;
	}

	if (!spdk_bdev_is_md_separate(bdev_io->bdev)) {
		return NULL; /* [한국어] inline metadata 또는 메타데이터 없음 */
	}

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ ||
	    bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		return bdev_io->u.bdev.md_buf; /* [한국어] separate metadata 버퍼 포인터 */
	}

	return NULL; /* [한국어] 다른 타입 → 메타데이터 없음 */
}

/*
 * [한국어]
 * spdk_bdev_io_get_cb_arg - bdev_io의 콜백 인수(caller_ctx) 조회
 *
 * @bdev_io:  대상 bdev I/O
 * @return:   internal.caller_ctx 포인터
 *
 * 호출 체인:
 *   사용자 완료 콜백 → [이 함수]
 */
void *
spdk_bdev_io_get_cb_arg(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io == NULL) {
		assert(false);
		return NULL;
	}

	return bdev_io->internal.caller_ctx; /* [한국어] 완료 콜백에 전달된 사용자 컨텍스트 */
}

/*
 * [한국어]
 * spdk_bdev_module_list_add - bdev 모듈을 전역 모듈 리스트에 등록
 *
 * @bdev_module: 등록할 모듈
 *
 * examine 콜백이 있는 모듈은 HEAD에 삽입 (먼저 examine 받도록),
 * 없는 모듈은 TAIL에 삽입.
 * 중복 등록 시 assert.
 *
 * 호출 체인:
 *   SPDK_BDEV_MODULE_REGISTER 매크로 (생성자) → [이 함수]
 */
void
spdk_bdev_module_list_add(struct spdk_bdev_module *bdev_module)
{

	if (spdk_bdev_module_list_find(bdev_module->name)) {
		SPDK_ERRLOG("ERROR: module '%s' already registered.\n", bdev_module->name);
		assert(false); /* [한국어] 중복 등록 금지 */
	}

	spdk_spin_init(&bdev_module->internal.spinlock); /* [한국어] 모듈 spinlock 초기화 */
	TAILQ_INIT(&bdev_module->internal.quiesced_ranges); /* [한국어] quiesce 범위 목록 초기화 */

	/*
	 * Modules with examine callbacks must be initialized first, so they are
	 *  ready to handle examine callbacks from later modules that will
	 *  register physical bdevs.
	 */
	/* [한국어] examine 콜백이 있으면 HEAD에 삽입 → 먼저 초기화됨 */
	if (bdev_module->examine_config != NULL || bdev_module->examine_disk != NULL) {
		TAILQ_INSERT_HEAD(&g_bdev_mgr.bdev_modules, bdev_module, internal.tailq); /* [한국어] examine 모듈 → 앞에 삽입 */
	} else {
		TAILQ_INSERT_TAIL(&g_bdev_mgr.bdev_modules, bdev_module, internal.tailq); /* [한국어] 일반 모듈 → 뒤에 삽입 */
	}
}

/*
 * [한국어]
 * spdk_bdev_module_list_find - 이름으로 bdev 모듈 탐색
 *
 * @name:   탐색할 모듈 이름
 * @return: 해당 모듈 포인터, 없으면 NULL
 *
 * 호출 체인:
 *   spdk_bdev_module_list_add() / 사용자 → [이 함수]
 */
struct spdk_bdev_module *
spdk_bdev_module_list_find(const char *name)
{
	struct spdk_bdev_module *bdev_module;

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, internal.tailq) {
		if (strcmp(name, bdev_module->name) == 0) {
			break; /* [한국어] 이름 일치 → 반환 */
		}
	}

	return bdev_module; /* [한국어] 찾으면 포인터, 없으면 NULL */
}

/*
 * [한국어]
 * bdev_write_zero_buffer - WRITE_ZEROES 소프트웨어 에뮬레이션: 실제 WRITE 제출
 *
 * @ctx: bdev_io (WRITE_ZEROES 원본 I/O)
 *
 * g_bdev_mgr.zero_buffer를 소스로 실제 WRITE I/O를 제출해 WRITE_ZEROES 에뮬레이션.
 * separate metadata bdev이면 zero_buffer 내 metadata 영역도 사용.
 * -ENOMEM이면 버퍼 큐 대기 후 재시도.
 *
 * 호출 체인:
 *   bdev_io_submit() → [이 함수] (에뮬레이션 경로)
 */
static void
bdev_write_zero_buffer(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;
	uint64_t num_blocks;
	void *md_buf = NULL;
	int rc;

	num_blocks = bdev_io->u.bdev.num_blocks; /* [한국어] 쓸 블록 수 */

	if (spdk_bdev_is_md_separate(bdev_io->bdev)) {
		/* [한국어] separate metadata: zero_buffer 끝 부분에 metadata 영역 사용 */
		md_buf = (char *)g_bdev_mgr.zero_buffer +
			 spdk_bdev_get_block_size(bdev_io->bdev) * num_blocks;
	}

	rc = bdev_write_blocks_with_md(bdev_io->internal.desc,
				       spdk_io_channel_from_ctx(bdev_io->internal.ch),
				       g_bdev_mgr.zero_buffer, md_buf,
				       bdev_io->u.bdev.offset_blocks, num_blocks,
				       bdev_write_zero_buffer_done, bdev_io);
	/* [한국어] 전역 zero_buffer를 이용한 실제 WRITE 제출 */
	if (spdk_likely(rc == 0)) {
		return; /* [한국어] 제출 성공 → done 콜백 대기 */
	} else {
		if (spdk_unlikely(rc == -ENOMEM)) {
			/* [한국어] 버퍼 부족 → 버퍼 가용 시 재시도 */
			bdev_queue_io_wait_with_cb(bdev_io, bdev_write_zero_buffer);
			return;
		}

		/* [한국어] 다른 오류 → 실패 완료 */
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
	}
}

/*
 * [한국어]
 * bdev_write_zero_buffer_done - WRITE_ZEROES 에뮬레이션 WRITE 완료 콜백
 *
 * @bdev_io:  완료된 내부 WRITE I/O
 * @success:  성공 여부
 * @cb_arg:   원본 WRITE_ZEROES I/O (parent_io)
 *
 * 내부 WRITE I/O 해제 후 원본 WRITE_ZEROES I/O 완료 처리.
 *
 * 호출 체인:
 *   bdev_write_zero_buffer() → WRITE 완료 → [이 함수]
 */
static void
bdev_write_zero_buffer_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;

	spdk_bdev_free_io(bdev_io); /* [한국어] 내부 WRITE I/O 해제 */

	/* [한국어] 원본 WRITE_ZEROES I/O 완료 상태 설정 후 콜백 */
	parent_io->internal.status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	parent_io->internal.cb(parent_io, success, parent_io->internal.caller_ctx);
}

/*
 * [한국어]
 * bdev_set_qos_limit_done - QoS 설정 완료 후 qos_mod_in_progress 해제 및 콜백 호출
 *
 * @ctx:    QoS 설정 컨텍스트
 * @status: 완료 상태
 *
 * qos_mod_in_progress 플래그 해제 → 사용자 콜백 → ctx 해제.
 * bdev가 REMOVING 상태이고 open_descs가 비었으면 unregister 완료 처리.
 * (QoS 설정 중 unregister 요청이 있었던 경우의 경쟁 조건 처리)
 *
 * 호출 체인:
 *   bdev_enable_qos_done() / bdev_disable_qos_done() / bdev_update_qos_rate_limit_msg() → [이 함수]
 */
static void
bdev_set_qos_limit_done(struct set_qos_limit_ctx *ctx, int status)
{
	struct spdk_bdev *bdev;
	int rc;

	spdk_spin_lock(&ctx->bdev->internal.spinlock);
	ctx->bdev->internal.qos_mod_in_progress = false; /* [한국어] QoS 변경 완료 플래그 해제 */
	spdk_spin_unlock(&ctx->bdev->internal.spinlock);

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, status); /* [한국어] 사용자 완료 콜백 */
	}
	bdev = ctx->bdev; /* [한국어] ctx 해제 전 bdev 포인터 저장 */
	free(ctx);        /* [한국어] ctx 해제 */

	/* [한국어] QoS 설정 중 REMOVING 요청이 왔던 경우 unregister 완료 처리 */
	spdk_spin_lock(&g_bdev_mgr.spinlock);
	spdk_spin_lock(&bdev->internal.spinlock);
	if (bdev->internal.status == SPDK_BDEV_STATUS_REMOVING && TAILQ_EMPTY(&bdev->internal.open_descs)) {
		SPDK_DEBUGLOG(bdev, "Data race detected - trying to enable QoS on unregistered bdev %s",
			      bdev->name);
		rc = bdev_unregister_unsafe(bdev);
		spdk_spin_unlock(&bdev->internal.spinlock);

		if (rc == 0) {
			spdk_io_device_unregister(__bdev_to_io_dev(bdev), bdev_destroy_cb); /* [한국어] io_device 해제 */
		}
	} else {
		spdk_spin_unlock(&bdev->internal.spinlock);
	}
	spdk_spin_unlock(&g_bdev_mgr.spinlock);
}

/*
 * [한국어]
 * bdev_disable_qos_done - QoS 비활성화 완료: QoS 구조체 해제
 *
 * @cb_arg: set_qos_limit_ctx 포인터
 *
 * qos->thread가 있으면 채널/폴러 해제, 그 다음 qos 메모리 해제.
 * 완료 후 bdev_set_qos_limit_done() 호출.
 *
 * 호출 체인:
 *   bdev_disable_qos_msg_done() → spdk_thread_send_msg → [이 함수]
 */
static void
bdev_disable_qos_done(void *cb_arg)
{
	struct set_qos_limit_ctx *ctx = cb_arg;
	struct spdk_bdev *bdev = ctx->bdev;
	struct spdk_bdev_qos *qos;

	spdk_spin_lock(&bdev->internal.spinlock);
	qos = bdev->internal.qos;
	bdev->internal.qos = NULL; /* [한국어] bdev에서 QoS 포인터 제거 (재진입 방지) */
	spdk_spin_unlock(&bdev->internal.spinlock);

	if (qos->thread != NULL) {
		/* [한국어] QoS 채널과 폴러 해제 */
		spdk_put_io_channel(spdk_io_channel_from_ctx(qos->ch));
		spdk_poller_unregister(&qos->poller);
	}

	free(qos); /* [한국어] QoS 구조체 해제 */

	bdev_set_qos_limit_done(ctx, 0);
}

/*
 * [한국어]
 * bdev_disable_qos_msg_done - 모든 채널 QoS 비활성화 완료 후 QoS 스레드에 done 전송
 *
 * @bdev:   대상 bdev
 * @_ctx:   set_qos_limit_ctx 포인터
 * @status: 채널 순회 결과
 *
 * QoS가 바인딩된 스레드에 bdev_disable_qos_done 메시지 전송.
 * QoS 스레드가 없으면 직접 호출.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel(bdev_disable_qos_msg) 완료 → [이 함수]
 */
static void
bdev_disable_qos_msg_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct set_qos_limit_ctx *ctx = _ctx;
	struct spdk_thread *thread;

	spdk_spin_lock(&bdev->internal.spinlock);
	thread = bdev->internal.qos->thread; /* [한국어] QoS 바인딩 스레드 */
	spdk_spin_unlock(&bdev->internal.spinlock);

	if (thread != NULL) {
		/* [한국어] QoS 스레드에서 채널/폴러 해제 */
		spdk_thread_send_msg(thread, bdev_disable_qos_done, ctx);
	} else {
		/* [한국어] QoS 스레드 없음 → 직접 호출 */
		bdev_disable_qos_done(ctx);
	}
}

/*
 * [한국어]
 * bdev_disable_qos_msg - 채널별 QoS 비활성화: QOS_ENABLED 플래그 해제 및 QoS 큐 재제출
 *
 * @i:    채널 반복자
 * @bdev: 대상 bdev
 * @ch:   현재 채널
 * @_ctx: set_qos_limit_ctx 포인터
 *
 * BDEV_CH_QOS_ENABLED 플래그 해제 → qos_queued_io의 I/O를 직접 재제출.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → [이 함수] → bdev_disable_qos_msg_done()
 */
static void
bdev_disable_qos_msg(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
		     struct spdk_io_channel *ch, void *_ctx)
{
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(ch);
	struct spdk_bdev_io *bdev_io;

	bdev_ch->flags &= ~BDEV_CH_QOS_ENABLED; /* [한국어] 채널의 QoS 활성화 플래그 해제 */

	while (!TAILQ_EMPTY(&bdev_ch->qos_queued_io)) {
		/* Re-submit the queued I/O. */
		/* [한국어] QoS 큐에 대기 중인 I/O를 다시 직접 제출 */
		bdev_io = TAILQ_FIRST(&bdev_ch->qos_queued_io);
		TAILQ_REMOVE(&bdev_ch->qos_queued_io, bdev_io, internal.link);
		_bdev_io_submit(bdev_io); /* [한국어] QoS 없이 직접 제출 */
	}

	spdk_bdev_for_each_channel_continue(i, 0); /* [한국어] 다음 채널로 진행 */
}

/*
 * [한국어]
 * bdev_update_qos_rate_limit_msg - QoS 스레드에서 레이트 리밋 업데이트
 *
 * @cb_arg: set_qos_limit_ctx 포인터
 *
 * QoS 스레드 컨텍스트에서 max_quota_per_timeslice 재계산.
 *
 * 호출 체인:
 *   spdk_bdev_set_qos_rate_limits() → spdk_thread_send_msg → [이 함수]
 */
static void
bdev_update_qos_rate_limit_msg(void *cb_arg)
{
	struct set_qos_limit_ctx *ctx = cb_arg;
	struct spdk_bdev *bdev = ctx->bdev;

	spdk_spin_lock(&bdev->internal.spinlock);
	bdev_qos_update_max_quota_per_timeslice(bdev->internal.qos); /* [한국어] 타임슬라이스당 최대 quota 재계산 */
	spdk_spin_unlock(&bdev->internal.spinlock);

	bdev_set_qos_limit_done(ctx, 0); /* [한국어] QoS 업데이트 완료 */
}

/*
 * [한국어]
 * bdev_enable_qos_msg - 채널별 QoS 활성화 메시지 처리
 *
 * @i:    채널 반복자
 * @bdev: 대상 bdev
 * @ch:   현재 채널
 * @_ctx: set_qos_limit_ctx 포인터
 *
 * bdev 상태가 READY이면 해당 채널에 QoS 활성화.
 * 상태가 변경된 경우(경쟁 조건) QoS 구조체 정리 후 -EAGAIN으로 순회 중단.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → [이 함수] → bdev_enable_qos_done()
 */
static void
bdev_enable_qos_msg(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
		    struct spdk_io_channel *ch, void *_ctx)
{
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(ch);
	int rc = 0;

	spdk_spin_lock(&bdev->internal.spinlock);
	if (bdev->internal.status == SPDK_BDEV_STATUS_READY) {
		bdev_enable_qos(bdev, bdev_ch); /* [한국어] 채널에 QoS 활성화 */
	} else {
		/* [한국어] bdev 상태 변경 경쟁 조건 감지 */
		SPDK_DEBUGLOG(bdev,
			      "Data race detected - requested to enable QoS on wrong bdev state bdev name: %s, bdev state: %d",
			      bdev->name, bdev->internal.status);
		if (bdev->internal.qos &&
		    bdev->internal.qos->ch == NULL) { /* QoS has not been fully created yet, shall clean up */
			/* [한국어] QoS 채널 미생성 → QoS 구조체 정리 */
			free(bdev->internal.qos);
			bdev->internal.qos = NULL;
			rc = -EAGAIN; /* [한국어] 재시도 요청 */
		}
	}
	spdk_spin_unlock(&bdev->internal.spinlock);
	spdk_bdev_for_each_channel_continue(i, rc); /* [한국어] 다음 채널로 진행 */
}

/*
 * [한국어]
 * bdev_enable_qos_done - 모든 채널 QoS 활성화 완료 콜백
 *
 * @bdev:   대상 bdev
 * @_ctx:   set_qos_limit_ctx 포인터
 * @status: 완료 상태
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel(bdev_enable_qos_msg) 완료 → [이 함수]
 */
static void
bdev_enable_qos_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct set_qos_limit_ctx *ctx = _ctx;

	bdev_set_qos_limit_done(ctx, status); /* [한국어] QoS 활성화 최종 완료 처리 */
}

/*
 * [한국어]
 * bdev_set_qos_rate_limits - QoS 구조체의 레이트 리밋 값 업데이트
 *
 * @bdev:   대상 bdev (qos != NULL이어야 함)
 * @limits: 새 레이트 리밋 배열 (SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES 크기)
 *
 * limits[i] == 0이면 LIMIT_NOT_DEFINED (비활성화)로 변환.
 * LIMIT_NOT_DEFINED인 항목은 기존 값 유지.
 *
 * 호출 체인:
 *   spdk_bdev_set_qos_rate_limits() → [이 함수]
 */
static void
bdev_set_qos_rate_limits(struct spdk_bdev *bdev, uint64_t *limits)
{
	int i;

	assert(bdev->internal.qos != NULL);

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (limits[i] != SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
			bdev->internal.qos->rate_limits[i].limit = limits[i]; /* [한국어] 지정된 리밋 값 설정 */

			if (limits[i] == 0) {
				/* [한국어] 0 → 해당 타입 비활성화 (LIMIT_NOT_DEFINED로 변환) */
				bdev->internal.qos->rate_limits[i].limit =
					SPDK_BDEV_QOS_LIMIT_NOT_DEFINED;
			}
		}
	}
}

/*
 * [한국어]
 * spdk_bdev_set_qos_rate_limits - bdev의 QoS 레이트 리밋 설정 (공개 API)
 *
 * @bdev:   대상 bdev
 * @limits: 새 레이트 리밋 배열 (IOPS/bytes 타입)
 * @cb_fn:  완료 콜백
 * @cb_arg: 완료 콜백 인수
 *
 * 단계:
 *  1. MB/s 단위 한도를 bytes로 변환, 최소 단위로 올림
 *  2. 모든 한도가 0이면 disable_rate_limit=true
 *  3. qos_mod_in_progress 중이면 -EAGAIN
 *  4. 비활성화: 모든 채널에 bdev_disable_qos_msg 전송
 *  5. 활성화 (qos==NULL): qos 구조체 새로 할당 후 채널에 bdev_enable_qos_msg
 *  6. 업데이트 (qos!=NULL, thread 있음): QoS 스레드에 bdev_update_qos_rate_limit_msg
 *
 * 호출 체인:
 *   사용자 → [이 함수] → bdev_enable/disable/update_qos_*
 */
void
spdk_bdev_set_qos_rate_limits(struct spdk_bdev *bdev, uint64_t *limits,
			      void (*cb_fn)(void *cb_arg, int status), void *cb_arg)
{
	struct set_qos_limit_ctx	*ctx;
	uint32_t			limit_set_complement;
	uint64_t			min_limit_per_sec;
	int				i;
	bool				disable_rate_limit = true;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (limits[i] == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
			continue; /* [한국어] 이 타입은 변경 안 함 → 건너뜀 */
		}

		if (limits[i] > 0) {
			disable_rate_limit = false; /* [한국어] 유효한 한도가 있음 → 비활성화 아님 */
		}

		if (bdev_qos_is_iops_rate_limit(i) == true) {
			min_limit_per_sec = SPDK_BDEV_QOS_MIN_IOS_PER_SEC; /* [한국어] IOPS 최소 단위 */
		} else {
			if (limits[i] > SPDK_BDEV_QOS_MAX_MBYTES_PER_SEC) {
				/* [한국어] MB/s 최대값 초과 → uint64 오버플로 방지로 클램핑 */
				SPDK_WARNLOG("Requested rate limit %" PRIu64 " will result in uint64_t overflow, "
					     "reset to %" PRIu64 "\n", limits[i], SPDK_BDEV_QOS_MAX_MBYTES_PER_SEC);
				limits[i] = SPDK_BDEV_QOS_MAX_MBYTES_PER_SEC;
			}
			/* Change from megabyte to byte rate limit */
			limits[i] = limits[i] * 1024 * 1024; /* [한국어] MB → bytes 변환 */
			min_limit_per_sec = SPDK_BDEV_QOS_MIN_BYTES_PER_SEC; /* [한국어] bytes/s 최소 단위 */
		}

		limit_set_complement = limits[i] % min_limit_per_sec;
		/* [한국어] 최소 단위의 배수가 아니면 올림 */
		if (limit_set_complement) {
			SPDK_ERRLOG("Requested rate limit %" PRIu64 " is not a multiple of %" PRIu64 "\n",
				    limits[i], min_limit_per_sec);
			limits[i] += min_limit_per_sec - limit_set_complement; /* [한국어] 최소 단위로 올림 */
			SPDK_ERRLOG("Round up the rate limit to %" PRIu64 "\n", limits[i]);
		}
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		if (cb_fn) {
			cb_fn(cb_arg, -ENOMEM);
		}
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->bdev = bdev;

	spdk_spin_lock(&bdev->internal.spinlock);
	if (bdev->internal.qos_mod_in_progress) {
		/* [한국어] QoS 변경 진행 중 → 재시도 요청 */
		spdk_spin_unlock(&bdev->internal.spinlock);
		free(ctx);
		if (cb_fn) {
			cb_fn(cb_arg, -EAGAIN);
		}
		return;
	}
	bdev->internal.qos_mod_in_progress = true; /* [한국어] QoS 변경 진행 중 플래그 설정 */

	if (disable_rate_limit == true && bdev->internal.qos) {
		/* [한국어] 모든 한도가 0인데 현재 QoS가 활성화 중이면 → 실제로 비활성화인지 검증 */
		for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
			if (limits[i] == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED &&
			    (bdev->internal.qos->rate_limits[i].limit > 0 &&
			     bdev->internal.qos->rate_limits[i].limit !=
			     SPDK_BDEV_QOS_LIMIT_NOT_DEFINED)) {
				disable_rate_limit = false; /* [한국어] 기존에 활성화된 한도 있음 → 비활성화 아님 */
				break;
			}
		}
	}

	if (disable_rate_limit == false) {
		if (bdev->internal.qos == NULL) {
			/* [한국어] QoS 구조체 없음 → 새로 할당 */
			bdev->internal.qos = calloc(1, sizeof(*bdev->internal.qos));
			if (!bdev->internal.qos) {
				spdk_spin_unlock(&bdev->internal.spinlock);
				SPDK_ERRLOG("Unable to allocate memory for QoS tracking\n");
				bdev_set_qos_limit_done(ctx, -ENOMEM);
				return;
			}
		}

		if (bdev->internal.qos->thread == NULL) {
			/* Enabling */
			/* [한국어] QoS 스레드 없음 → 새로 활성화 */
			bdev_set_qos_rate_limits(bdev, limits); /* [한국어] 레이트 리밋 설정 */

			spdk_bdev_for_each_channel(bdev, bdev_enable_qos_msg, ctx,
						   bdev_enable_qos_done); /* [한국어] 모든 채널에 QoS 활성화 */
		} else {
			/* Updating */
			/* [한국어] QoS 이미 활성화 → 레이트 리밋만 업데이트 */
			bdev_set_qos_rate_limits(bdev, limits);

			spdk_thread_send_msg(bdev->internal.qos->thread,
					     bdev_update_qos_rate_limit_msg, ctx);
		}
	} else {
		/* [한국어] disable_rate_limit == true */
		if (bdev->internal.qos != NULL) {
			/* [한국어] QoS 활성화 중 → 레이트 리밋 0으로 설정 후 비활성화 */
			bdev_set_qos_rate_limits(bdev, limits);

			/* Disabling */
			/* [한국어] 모든 채널에서 QoS 비활성화 */
			spdk_bdev_for_each_channel(bdev, bdev_disable_qos_msg, ctx,
						   bdev_disable_qos_msg_done);
		} else {
			/* [한국어] QoS 없는 상태에서 비활성화 → 아무 것도 안 함 */
			spdk_spin_unlock(&bdev->internal.spinlock);
			bdev_set_qos_limit_done(ctx, 0);
			return;
		}
	}

	spdk_spin_unlock(&bdev->internal.spinlock);
}

/*
 * [한국어] spdk_bdev_histogram_ctx - 히스토그램 활성화/비활성화 컨텍스트
 */
struct spdk_bdev_histogram_ctx {
	spdk_bdev_histogram_status_cb cb_fn;
	/* [한국어] 완료 콜백.
	 * 설정자: spdk_bdev_histogram_enable_ext().
	 * 읽는 자: bdev_histogram_*_cb().
	 * 동기화: 불변. */

	void *cb_arg;
	/* [한국어] 완료 콜백 인수.
	 * 설정자: spdk_bdev_histogram_enable_ext().
	 * 동기화: 불변. */

	struct spdk_bdev *bdev;
	/* [한국어] 대상 bdev.
	 * 설정자: spdk_bdev_histogram_enable_ext().
	 * 읽는 자: 완료 콜백에서 histogram_in_progress 해제 시.
	 * 동기화: 불변. */

	int status;
	/* [한국어] 채널 순회 중 누적 오류 코드.
	 * 설정자: bdev_histogram_enable_channel_cb()에서 오류 시 설정.
	 * 읽는 자: bdev_histogram_disable_channel_cb()에서 최종 콜백에 전달.
	 * 값 범위: 0=성공, -ENOMEM 등.
	 * 동기화: 순차 채널 순회이므로 락 불필요. */
};

/*
 * [한국어]
 * bdev_histogram_disable_channel_cb - 모든 채널 히스토그램 비활성화 완료 콜백
 *
 * @bdev:   대상 bdev
 * @_ctx:   spdk_bdev_histogram_ctx 포인터
 * @status: 완료 상태
 *
 * histogram_in_progress 해제 후 사용자 콜백 호출.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel(bdev_histogram_disable_channel) 완료 → [이 함수]
 */
static void
bdev_histogram_disable_channel_cb(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_histogram_ctx *ctx = _ctx;

	spdk_spin_lock(&ctx->bdev->internal.spinlock);
	ctx->bdev->internal.histogram_in_progress = false; /* [한국어] 히스토그램 변경 완료 플래그 해제 */
	spdk_spin_unlock(&ctx->bdev->internal.spinlock);
	ctx->cb_fn(ctx->cb_arg, ctx->status); /* [한국어] 사용자 완료 콜백 */
	free(ctx);
}

/*
 * [한국어]
 * bdev_histogram_disable_channel - 채널별 히스토그램 데이터 해제
 *
 * @i:    채널 반복자
 * @bdev: 대상 bdev
 * @_ch:  현재 채널
 * @_ctx: spdk_bdev_histogram_ctx 포인터
 *
 * ch->histogram이 있으면 해제 후 NULL 설정.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → [이 함수] → bdev_histogram_disable_channel_cb()
 */
static void
bdev_histogram_disable_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			       struct spdk_io_channel *_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);

	if (ch->histogram != NULL) {
		spdk_histogram_data_free(ch->histogram); /* [한국어] 히스토그램 데이터 해제 */
		ch->histogram = NULL; /* [한국어] 포인터 초기화 */
	}
	spdk_bdev_for_each_channel_continue(i, 0); /* [한국어] 다음 채널로 진행 */
}

/*
 * [한국어]
 * bdev_histogram_enable_channel_cb - 모든 채널 히스토그램 활성화 완료 콜백
 *
 * @bdev:   대상 bdev
 * @_ctx:   spdk_bdev_histogram_ctx 포인터
 * @status: 완료 상태
 *
 * 오류 시 histogram_enabled 롤백 후 비활성화 순회.
 * 성공 시 histogram_in_progress 해제 후 콜백.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel(bdev_histogram_enable_channel) 완료 → [이 함수]
 */
static void
bdev_histogram_enable_channel_cb(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_histogram_ctx *ctx = _ctx;

	if (status != 0) {
		/* [한국어] 활성화 실패 → 비활성화 순회로 롤백 */
		ctx->status = status;
		ctx->bdev->internal.histogram_enabled = false;
		spdk_bdev_for_each_channel(ctx->bdev, bdev_histogram_disable_channel, ctx,
					   bdev_histogram_disable_channel_cb);
	} else {
		spdk_spin_lock(&ctx->bdev->internal.spinlock);
		ctx->bdev->internal.histogram_in_progress = false;
		spdk_spin_unlock(&ctx->bdev->internal.spinlock);
		ctx->cb_fn(ctx->cb_arg, ctx->status);
		free(ctx);
	}
}

static void
bdev_histogram_enable_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			      struct spdk_io_channel *_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	int status = 0;

	if (ch->histogram == NULL) {
		/* [한국어] 히스토그램 미할당 → 새로 할당 */
		ch->histogram = spdk_histogram_data_alloc_sized_ext(bdev->internal.histogram_granularity,
				bdev->internal.histogram_min_val, bdev->internal.histogram_max_val);
		if (ch->histogram == NULL) {
			status = -ENOMEM; /* [한국어] 할당 실패 */
		}
	}

	spdk_bdev_for_each_channel_continue(i, status); /* [한국어] 다음 채널로 진행 */
}

/*
 * [한국어]
 * spdk_bdev_histogram_enable_ext - bdev 히스토그램 활성화/비활성화 (옵션 포함)
 *
 * @bdev:   대상 bdev
 * @cb_fn:  완료 콜백
 * @cb_arg: 완료 콜백 인수
 * @enable: true=활성화, false=비활성화
 * @opts:   히스토그램 옵션 (io_type, granularity, min/max_nsec)
 *
 * histogram_in_progress 플래그로 동시 변경 방지.
 * 파라미터를 bdev에 저장 후 각 채널에 enable/disable 순회.
 *
 * 호출 체인:
 *   spdk_bdev_histogram_enable() / 사용자 → [이 함수]
 */
void
spdk_bdev_histogram_enable_ext(struct spdk_bdev *bdev, spdk_bdev_histogram_status_cb cb_fn,
			       void *cb_arg, bool enable, struct spdk_bdev_enable_histogram_opts *opts)
{
	struct spdk_bdev_histogram_ctx *ctx;

	ctx = calloc(1, sizeof(struct spdk_bdev_histogram_ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bdev = bdev;
	ctx->status = 0;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_spin_lock(&bdev->internal.spinlock);
	if (bdev->internal.histogram_in_progress) {
		/* [한국어] 이미 히스토그램 변경 진행 중 → 재시도 요청 */
		spdk_spin_unlock(&bdev->internal.spinlock);
		free(ctx);
		cb_fn(cb_arg, -EAGAIN);
		return;
	}

	bdev->internal.histogram_in_progress = true; /* [한국어] 히스토그램 변경 진행 중 플래그 */
	spdk_spin_unlock(&bdev->internal.spinlock);

	bdev->internal.histogram_enabled = enable;                /* [한국어] 활성화 여부 저장 */
	bdev->internal.histogram_io_type = opts->io_type;         /* [한국어] 측정할 I/O 타입 */
	bdev->internal.histogram_granularity = opts->granularity; /* [한국어] 히스토그램 세분도 */
	bdev->internal.histogram_min_val = opts->min_nsec * spdk_get_ticks_hz() / SPDK_SEC_TO_NSEC;
	/* [한국어] min_nsec → tick 단위로 변환 (Hz = tick/sec) */
	if (opts->max_nsec == UINT64_MAX) {
		bdev->internal.histogram_max_val = UINT64_MAX; /* [한국어] 최대값 제한 없음 */
	} else {
		bdev->internal.histogram_max_val = opts->max_nsec * spdk_get_ticks_hz() / SPDK_SEC_TO_NSEC;
		/* [한국어] max_nsec → tick 단위로 변환 */
	}

	if (enable) {
		/* Allocate histogram for each channel */
		/* [한국어] 활성화 → 각 채널에 히스토그램 할당 */
		spdk_bdev_for_each_channel(bdev, bdev_histogram_enable_channel, ctx,
					   bdev_histogram_enable_channel_cb);
	} else {
		/* [한국어] 비활성화 → 각 채널의 히스토그램 해제 */
		spdk_bdev_for_each_channel(bdev, bdev_histogram_disable_channel, ctx,
					   bdev_histogram_disable_channel_cb);
	}
}

/*
 * [한국어]
 * spdk_bdev_enable_histogram_opts_init - 히스토그램 옵션 기본값 초기화
 *
 * @opts: 초기화할 옵션 구조체
 * @size: 구조체 크기
 *
 * io_type=0, granularity=DEFAULT, min_nsec=0, max_nsec=UINT64_MAX 기본값.
 *
 * 호출 체인:
 *   spdk_bdev_histogram_enable() / 사용자 → [이 함수]
 */
void
spdk_bdev_enable_histogram_opts_init(struct spdk_bdev_enable_histogram_opts *opts, size_t size)
{
	if (opts == NULL) {
		SPDK_ERRLOG("opts should not be NULL\n");
		assert(opts != NULL);
		return;
	}
	if (size == 0) {
		SPDK_ERRLOG("size should not be zero\n");
		assert(size != 0);
		return;
	}

	memset(opts, 0, size); /* [한국어] opts 구조체 0 초기화 */
	opts->size = size;     /* [한국어] 크기 기록 */

#define FIELD_OK(field) \
        offsetof(struct spdk_bdev_enable_histogram_opts, field) + sizeof(opts->field) <= size

#define SET_FIELD(field, value) \
        if (FIELD_OK(field)) { \
                opts->field = value; \
        } \

	SET_FIELD(io_type, 0);           /* [한국어] io_type 기본값: 0 (모든 타입) */
	SET_FIELD(granularity, SPDK_HISTOGRAM_GRANULARITY_DEFAULT); /* [한국어] 기본 세분도 */
	SET_FIELD(min_nsec, 0);          /* [한국어] 최소 측정 범위: 0 ns */
	SET_FIELD(max_nsec, UINT64_MAX); /* [한국어] 최대 측정 범위: 제한 없음 */

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	/* [한국어] 새 필드 추가 시 assert 크기와 SET_FIELD를 함께 업데이트 */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_enable_histogram_opts) == 26, "Incorrect size");

#undef FIELD_OK
#undef SET_FIELD
}

/*
 * [한국어]
 * spdk_bdev_histogram_enable - bdev 히스토그램 활성화/비활성화 (기본 옵션 래퍼)
 *
 * @bdev:   대상 bdev
 * @cb_fn:  완료 콜백
 * @cb_arg: 완료 콜백 인수
 * @enable: true=활성화, false=비활성화
 *
 * 기본 옵션으로 spdk_bdev_histogram_enable_ext() 호출.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → spdk_bdev_histogram_enable_ext()
 */
void
spdk_bdev_histogram_enable(struct spdk_bdev *bdev, spdk_bdev_histogram_status_cb cb_fn,
			   void *cb_arg, bool enable)
{
	struct spdk_bdev_enable_histogram_opts opts;

	spdk_bdev_enable_histogram_opts_init(&opts, sizeof(opts)); /* [한국어] 기본 옵션 초기화 */
	spdk_bdev_histogram_enable_ext(bdev, cb_fn, cb_arg, enable, &opts); /* [한국어] 옵션 포함 호출 */
}

/*
 * [한국어] spdk_bdev_histogram_data_ctx - 히스토그램 데이터 수집 컨텍스트
 */
struct spdk_bdev_histogram_data_ctx {
	spdk_bdev_histogram_data_cb cb_fn;
	/* [한국어] 수집 완료 콜백.
	 * 설정자: spdk_bdev_histogram_get().
	 * 읽는 자: bdev_histogram_get_channel_cb(). */

	void *cb_arg;
	/* [한국어] 콜백 인수. */

	struct spdk_bdev *bdev;
	/* [한국어] 대상 bdev. */

	/** merged histogram data from all channels */
	struct spdk_histogram_data	*histogram;
	/* [한국어] 모든 채널의 히스토그램을 병합할 대상 (사용자 제공).
	 * 설정자: spdk_bdev_histogram_get()에서 사용자 histogram 포인터.
	 * 읽는 자: bdev_histogram_get_channel()에서 spdk_histogram_data_merge()로 병합.
	 * 동기화: 순차 채널 순회이므로 락 불필요. */
};

/*
 * [한국어]
 * bdev_histogram_get_channel_cb - 모든 채널 히스토그램 수집 완료 콜백
 *
 * @bdev:   대상 bdev
 * @_ctx:   spdk_bdev_histogram_data_ctx 포인터
 * @status: 완료 상태
 *
 * 병합된 히스토그램과 함께 사용자 콜백 호출 후 ctx 해제.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel(bdev_histogram_get_channel) 완료 → [이 함수]
 */
static void
bdev_histogram_get_channel_cb(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_histogram_data_ctx *ctx = _ctx;

	ctx->cb_fn(ctx->cb_arg, status, ctx->histogram); /* [한국어] 병합된 히스토그램 전달 */
	free(ctx);
}

/*
 * [한국어]
 * bdev_histogram_get_channel - 채널의 히스토그램을 ctx->histogram에 병합
 *
 * @i:    채널 반복자
 * @bdev: 대상 bdev
 * @_ch:  현재 채널
 * @_ctx: spdk_bdev_histogram_data_ctx 포인터
 *
 * ch->histogram이 없으면 -EFAULT (히스토그램 미활성화).
 * 있으면 병합.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → [이 함수] → bdev_histogram_get_channel_cb()
 */
static void
bdev_histogram_get_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			   struct spdk_io_channel *_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	struct spdk_bdev_histogram_data_ctx *ctx = _ctx;
	int status = 0;

	if (ch->histogram == NULL) {
		status = -EFAULT; /* [한국어] 히스토그램 미활성화 → 오류 */
	} else {
		spdk_histogram_data_merge(ctx->histogram, ch->histogram); /* [한국어] 채널 데이터 병합 */
	}

	spdk_bdev_for_each_channel_continue(i, status); /* [한국어] 다음 채널로 진행 */
}

/*
 * [한국어]
 * spdk_bdev_histogram_get - 모든 채널 히스토그램 비동기 수집
 *
 * @bdev:      대상 bdev
 * @histogram: 병합 결과를 저장할 히스토그램 (사용자 제공)
 * @cb_fn:     완료 콜백
 * @cb_arg:    완료 콜백 인수
 *
 * 각 채널의 히스토그램을 histogram에 병합 후 콜백 호출.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → bdev_histogram_get_channel() → bdev_histogram_get_channel_cb()
 */
void
spdk_bdev_histogram_get(struct spdk_bdev *bdev, struct spdk_histogram_data *histogram,
			spdk_bdev_histogram_data_cb cb_fn,
			void *cb_arg)
{
	struct spdk_bdev_histogram_data_ctx *ctx;

	ctx = calloc(1, sizeof(struct spdk_bdev_histogram_data_ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM, NULL);
		return;
	}

	ctx->bdev = bdev;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	ctx->histogram = histogram; /* [한국어] 사용자 제공 병합 대상 히스토그램 */

	spdk_bdev_for_each_channel(bdev, bdev_histogram_get_channel, ctx,
				   bdev_histogram_get_channel_cb); /* [한국어] 채널별 순회 */
}

/*
 * [한국어]
 * spdk_bdev_channel_get_histogram - 단일 채널의 히스토그램 동기 조회
 *
 * @ch:     대상 I/O 채널
 * @cb_fn:  콜백 함수 (status, histogram 전달)
 * @cb_arg: 콜백 인수
 *
 * ch의 히스토그램을 직접 콜백에 전달. 히스토그램 미활성화 시 -EFAULT.
 *
 * 호출 체인:
 *   사용자 → [이 함수]
 */
void
spdk_bdev_channel_get_histogram(struct spdk_io_channel *ch, spdk_bdev_histogram_data_cb cb_fn,
				void *cb_arg)
{
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(ch);
	int status = 0;

	assert(cb_fn != NULL);

	if (bdev_ch->histogram == NULL) {
		status = -EFAULT; /* [한국어] 히스토그램 미활성화 */
	}
	cb_fn(cb_arg, status, bdev_ch->histogram); /* [한국어] 채널 히스토그램 직접 전달 */
}

/*
 * [한국어]
 * spdk_bdev_get_media_events - desc의 대기 중인 미디어 이벤트 수집
 *
 * @desc:       대상 디스크립터
 * @events:     이벤트 저장 배열
 * @max_events: 수집할 최대 이벤트 수
 * @return:     수집된 이벤트 수
 *
 * pending_media_events에서 이벤트를 꺼내 events 배열에 저장.
 * 꺼낸 엔트리는 free_media_events로 반환.
 *
 * 호출 체인:
 *   사용자 MEDIA_MANAGEMENT 이벤트 콜백 → [이 함수]
 */
size_t
spdk_bdev_get_media_events(struct spdk_bdev_desc *desc, struct spdk_bdev_media_event *events,
			   size_t max_events)
{
	struct media_event_entry *entry;
	size_t num_events = 0;

	for (; num_events < max_events; ++num_events) {
		entry = TAILQ_FIRST(&desc->pending_media_events); /* [한국어] 대기 중인 첫 이벤트 */
		if (entry == NULL) {
			break; /* [한국어] 대기 이벤트 없음 → 종료 */
		}

		events[num_events] = entry->event; /* [한국어] 이벤트 배열에 복사 */
		TAILQ_REMOVE(&desc->pending_media_events, entry, tailq); /* [한국어] 대기 큐에서 제거 */
		TAILQ_INSERT_TAIL(&desc->free_media_events, entry, tailq); /* [한국어] 프리 풀로 반환 */
	}

	return num_events; /* [한국어] 수집된 이벤트 수 반환 */
}

/*
 * [한국어]
 * spdk_bdev_push_media_events - 미디어 이벤트를 desc의 이벤트 큐에 추가
 *
 * @bdev:       대상 bdev
 * @events:     추가할 이벤트 배열
 * @num_events: 이벤트 수
 * @return:     실제 추가된 이벤트 수 (프리 풀 부족 시 더 적을 수 있음), 또는 -ENODEV
 *
 * open_descs에서 write desc를 찾고 그 desc의 free_media_events에서 엔트리를 꺼내
 * pending_media_events에 삽입. 이후 spdk_bdev_notify_media_management()로 알림.
 *
 * 호출 체인:
 *   bdev 모듈 (미디어 오류 감지) → [이 함수]
 */
int
spdk_bdev_push_media_events(struct spdk_bdev *bdev, const struct spdk_bdev_media_event *events,
			    size_t num_events)
{
	struct spdk_bdev_desc *desc;
	struct media_event_entry *entry;
	size_t event_id;
	int rc = 0;

	assert(bdev->media_events); /* [한국어] media_events 지원 bdev에서만 호출 가능 */

	spdk_spin_lock(&bdev->internal.spinlock); /* [한국어] open_descs 순회 보호 */
	TAILQ_FOREACH(desc, &bdev->internal.open_descs, link) {
		if (desc->write) {
			break; /* [한국어] write desc 탐색 */
		}
	}

	if (desc == NULL || desc->media_events_buffer == NULL) {
		/* [한국어] write desc 없거나 미디어 이벤트 버퍼 없음 → 오류 */
		rc = -ENODEV;
		goto out;
	}

	for (event_id = 0; event_id < num_events; ++event_id) {
		entry = TAILQ_FIRST(&desc->free_media_events); /* [한국어] 프리 풀에서 엔트리 확보 */
		if (entry == NULL) {
			break; /* [한국어] 프리 풀 고갈 → 더 이상 추가 불가 */
		}

		TAILQ_REMOVE(&desc->free_media_events, entry, tailq);       /* [한국어] 프리 풀에서 제거 */
		TAILQ_INSERT_TAIL(&desc->pending_media_events, entry, tailq); /* [한국어] 대기 큐에 삽입 */
		entry->event = events[event_id]; /* [한국어] 이벤트 데이터 복사 */
	}

	rc = event_id; /* [한국어] 실제 추가된 이벤트 수 */
out:
	spdk_spin_unlock(&bdev->internal.spinlock);
	return rc;
}

/*
 * [한국어]
 * _media_management_notify - desc의 이벤트 콜백 스레드에서 MEDIA_MANAGEMENT 이벤트 전달
 *
 * @arg: spdk_bdev_desc 포인터
 *
 * desc 소유 스레드에서 실행: _event_notify()로 콜백 호출.
 *
 * 호출 체인:
 *   spdk_bdev_notify_media_management() → spdk_thread_send_msg → [이 함수]
 */
static void
_media_management_notify(void *arg)
{
	struct spdk_bdev_desc *desc = arg;

	_event_notify(desc, SPDK_BDEV_EVENT_MEDIA_MANAGEMENT); /* [한국어] MEDIA_MANAGEMENT 이벤트 전달 */
}

/*
 * [한국어]
 * spdk_bdev_notify_media_management - 미디어 이벤트가 있는 모든 desc에 알림 전송
 *
 * @bdev: 대상 bdev
 *
 * open_descs 순회 → pending_media_events가 있는 desc에 event_notify 전송.
 *
 * 호출 체인:
 *   bdev 모듈 → spdk_bdev_push_media_events() → [이 함수]
 */
void
spdk_bdev_notify_media_management(struct spdk_bdev *bdev)
{
	struct spdk_bdev_desc *desc;

	spdk_spin_lock(&bdev->internal.spinlock); /* [한국어] open_descs 순회 보호 */
	TAILQ_FOREACH(desc, &bdev->internal.open_descs, link) {
		if (!TAILQ_EMPTY(&desc->pending_media_events)) {
			/* [한국어] 대기 중인 미디어 이벤트가 있는 desc에 알림 전송 */
			event_notify(desc, _media_management_notify);
		}
	}
	spdk_spin_unlock(&bdev->internal.spinlock);
}

/*
 * [한국어] locked_lba_range_ctx - LBA 범위 잠금 작업 컨텍스트
 */
struct locked_lba_range_ctx {
	struct lba_range		range;
	/* [한국어] 잠글 LBA 범위 정보 (offset/length/owner_ch 등).
	 * 설정자: _bdev_lock_lba_range()에서 초기화.
	 * 읽는 자: bdev_lock_lba_range_get_channel()에서 채널별 복사 시 사용.
	 * 동기화: bdev->internal.spinlock으로 보호. */

	struct lba_range		*current_range;
	/* [한국어] 현재 처리 중인 채널의 range 포인터.
	 * 설정자: bdev_lock_lba_range_get_channel()에서 설정.
	 * 읽는 자: bdev_lock_lba_range_check_io()에서 I/O 완료 대기 시.
	 * 동기화: 단일 채널 순회이므로 락 불필요. */

	struct lba_range		*owner_range;
	/* [한국어] 잠금을 소유할 채널의 range 포인터.
	 * 설정자: bdev_lock_lba_range_get_channel()에서 owner_ch 일치 시 설정.
	 * 읽는 자: bdev_lock_lba_range_cb()에서 owner_ch 설정 시.
	 * 동기화: 단일 채널 순회이므로 락 불필요. */

	struct spdk_poller		*poller;
	/* [한국어] 진행 중인 I/O 완료 대기 폴러 (100us 주기).
	 * 설정자: bdev_lock_lba_range_check_io()에서 I/O 진행 중일 때 등록.
	 * 읽는 자: bdev_lock_lba_range_check_io()에서 재호출 시 unregister.
	 * 동기화: 단일 채널 컨텍스트. */

	struct spdk_io_channel		*ch_ref;
	/* [한국어] 폴러 실행 중 채널 해제 방지를 위한 추가 참조.
	 * 설정자: bdev_lock_lba_range_check_io()에서 폴러 등록 시 spdk_get_io_channel().
	 * 읽는 자: 폴러가 완료(또는 I/O 없음)되면 spdk_put_io_channel() 후 NULL.
	 * 동기화: 단일 채널 컨텍스트. */

	lock_range_cb			cb_fn;
	/* [한국어] 잠금 완료 콜백.
	 * 설정자: _bdev_lock_lba_range().
	 * 읽는 자: bdev_lock_lba_range_cb()에서 호출.
	 * 동기화: 불변. */

	void				*cb_arg;
	/* [한국어] 콜백 인수.
	 * 설정자: _bdev_lock_lba_range().
	 * 읽는 자: bdev_lock_lba_range_cb().
	 * 동기화: 불변. */
};

/*
 * [한국어]
 * bdev_lock_error_cleanup_cb - LBA 범위 잠금 중 -ENOMEM 발생 시 정리 완료 콜백
 *
 * @bdev:   대상 bdev
 * @_ctx:   locked_lba_range_ctx 포인터
 * @status: 정리 완료 상태
 *
 * unlock 기반 정리 완료 후 -ENOMEM으로 원래 콜백 호출 및 ctx 해제.
 *
 * 호출 체인:
 *   bdev_lock_lba_range_cb(-ENOMEM) → bdev_unlock_lba_range_get_channel → [이 함수]
 */
static void
bdev_lock_error_cleanup_cb(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct locked_lba_range_ctx *ctx = _ctx;

	ctx->cb_fn(&ctx->range, ctx->cb_arg, -ENOMEM); /* [한국어] -ENOMEM으로 원래 콜백 */
	free(ctx); /* [한국어] ctx 해제 */
}

static void bdev_unlock_lba_range_get_channel(struct spdk_bdev_channel_iter *i,
		struct spdk_bdev *bdev, struct spdk_io_channel *ch, void *_ctx);

/*
 * [한국어]
 * bdev_lock_lba_range_cb - 모든 채널 LBA 범위 잠금 완료 콜백
 *
 * @bdev:   대상 bdev
 * @_ctx:   locked_lba_range_ctx 포인터
 * @status: 완료 상태 (0=성공, -ENOMEM=할당 실패)
 *
 * 성공: owner_range의 owner_ch를 설정 후 콜백 호출.
 * -ENOMEM: unlock 함수를 이용한 롤백 정리 시작.
 * ctx는 잠금이 유지되는 동안 해제하지 않음 (unlock 시 해제).
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel(bdev_lock_lba_range_get_channel) 완료 → [이 함수]
 */
static void
bdev_lock_lba_range_cb(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct locked_lba_range_ctx *ctx = _ctx;

	if (status == -ENOMEM) {
		/* One of the channels could not allocate a range object.
		 * So we have to go back and clean up any ranges that were
		 * allocated successfully before we return error status to
		 * the caller.  We can reuse the unlock function to do that
		 * clean up.
		 */
		/* [한국어] 일부 채널 range 할당 실패 → unlock 방식으로 롤백 */
		spdk_bdev_for_each_channel(bdev, bdev_unlock_lba_range_get_channel, ctx,
					   bdev_lock_error_cleanup_cb);
		return;
	}

	/* All channels have locked this range and no I/O overlapping the range
	 * are outstanding!  Set the owner_ch for the range object for the
	 * locking channel, so that this channel will know that it is allowed
	 * to write to this range.
	 */
	/* [한국어] 모든 채널 잠금 완료 + 겹치는 I/O 없음 → owner_ch 설정 */
	if (ctx->owner_range != NULL) {
		ctx->owner_range->owner_ch = ctx->range.owner_ch; /* [한국어] 소유 채널 설정 */
	}

	ctx->cb_fn(&ctx->range, ctx->cb_arg, status); /* [한국어] 잠금 완료 콜백 */

	/* Don't free the ctx here.  Its range is in the bdev's global list of
	 * locked ranges still, and will be removed and freed when this range
	 * is later unlocked.
	 */
	/* [한국어] ctx 해제 안 함 - locked_ranges에 남아있음, unlock 시 해제 */
}

/*
 * [한국어]
 * bdev_lock_lba_range_check_io - 잠긴 LBA 범위와 겹치는 진행 중 I/O 완료 대기 폴러
 *
 * @_i:     spdk_bdev_channel_iter 포인터
 * @return: SPDK_POLLER_BUSY (항상)
 *
 * 채널의 io_submitted에서 범위가 겹치는 I/O를 찾아 완료 대기.
 * 겹치는 I/O 없으면 채널 잠금 완료 (for_each_channel 진행).
 * 채널 참조를 추가로 보유해 폴러 실행 중 채널 해제 방지.
 *
 * 호출 체인:
 *   bdev_lock_lba_range_get_channel() → [이 함수] (폴러로 재귀 호출 가능)
 */
static int
bdev_lock_lba_range_check_io(void *_i)
{
	struct spdk_bdev_channel_iter *i = _i;
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i->i);
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	struct locked_lba_range_ctx *ctx = i->ctx;
	struct lba_range *range = ctx->current_range;
	struct spdk_bdev_io *bdev_io;

	spdk_poller_unregister(&ctx->poller); /* [한국어] 이전 폴러 해제 (재시도 사이클) */

	/* The range is now in the locked_ranges, so no new IO can be submitted to this
	 * range.  But we need to wait until any outstanding IO overlapping with this range
	 * are completed.
	 */
	/* [한국어] 범위는 이미 locked_ranges에 있어 새 I/O 진입 차단됨.
	 * 단, 이미 진행 중인 I/O가 완료될 때까지 대기 필요. */
	TAILQ_FOREACH(bdev_io, &ch->io_submitted, internal.ch_link) {
		if (bdev_io_range_is_locked(bdev_io, range)) {
			/* [한국어] 범위 겹치는 I/O 발견 → 채널 참조 추가 후 폴러 재등록 */
			if (ctx->ch_ref == NULL) {
				/* Take another reference to ch to prevent it getting freed while
				 * the poller is running. */
				/* [한국어] 폴러 실행 중 채널 해제 방지를 위한 추가 참조 */
				ctx->ch_ref = spdk_get_io_channel(__bdev_to_io_dev(bdev_io->bdev));
				assert(ctx->ch_ref != NULL);
			}
			ctx->poller = SPDK_POLLER_REGISTER(bdev_lock_lba_range_check_io, i, 100);
			/* [한국어] 100us 후 재시도 폴러 등록 */
			return SPDK_POLLER_BUSY;
		}
	}

	if (ctx->ch_ref != NULL) {
		/* [한국어] 채널 추가 참조 해제 (채널 보호 불필요해짐) */
		spdk_put_io_channel(ctx->ch_ref);
		ctx->ch_ref = NULL;
	}

	spdk_bdev_for_each_channel_continue(i, 0); /* [한국어] 이 채널 잠금 완료 → 다음 채널로 */
	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * bdev_lock_lba_range_get_channel - 채널별 LBA 범위 잠금 설정 및 I/O 대기 시작
 *
 * @i:    채널 반복자
 * @bdev: 대상 bdev
 * @_ch:  현재 채널
 * @_ctx: locked_lba_range_ctx 포인터
 *
 * 이미 같은 range가 있으면 건너뜀 (새 채널 생성 경쟁 조건).
 * 없으면 range를 할당해 ch->locked_ranges에 추가 후 I/O 완료 대기.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → [이 함수] → bdev_lock_lba_range_check_io()
 */
static void
bdev_lock_lba_range_get_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
				struct spdk_io_channel *_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	struct locked_lba_range_ctx *ctx = _ctx;
	struct lba_range *range;

	TAILQ_FOREACH(range, &ch->locked_ranges, tailq) {
		if (range->length == ctx->range.length &&
		    range->offset == ctx->range.offset &&
		    range->locked_ctx == ctx->range.locked_ctx) {
			/* This range already exists on this channel, so don't add
			 * it again.  This can happen when a new channel is created
			 * while the for_each_channel operation is in progress.
			 * Do not check for outstanding I/O in that case, since the
			 * range was locked before any I/O could be submitted to the
			 * new channel.
			 */
			/* [한국어] 이미 같은 range 있음 (새 채널 생성 경쟁 조건) → 건너뜀 */
			spdk_bdev_for_each_channel_continue(i, 0);
			return;
		}
	}

	range = calloc(1, sizeof(*range)); /* [한국어] 채널별 range 구조체 할당 */
	if (range == NULL) {
		spdk_bdev_for_each_channel_continue(i, -ENOMEM); /* [한국어] 할당 실패 → 중단 */
		return;
	}

	range->length = ctx->range.length;         /* [한국어] 범위 길이 */
	range->offset = ctx->range.offset;         /* [한국어] 범위 시작 오프셋 */
	range->locked_ctx = ctx->range.locked_ctx; /* [한국어] 잠금 컨텍스트 (cb_arg) */
	range->quiesce = ctx->range.quiesce;       /* [한국어] quiesce 모드 여부 */
	ctx->current_range = range;                /* [한국어] 현재 채널의 range 포인터 저장 */
	if (ctx->range.owner_ch == ch) {
		/* This is the range object for the channel that will hold
		 * the lock.  Store it in the ctx object so that we can easily
		 * set its owner_ch after the lock is finally acquired.
		 */
		/* [한국어] 소유 채널의 range → ctx->owner_range에 저장 (나중에 owner_ch 설정용) */
		ctx->owner_range = range;
	}
	TAILQ_INSERT_TAIL(&ch->locked_ranges, range, tailq); /* [한국어] 채널 locked_ranges에 추가 */
	bdev_lock_lba_range_check_io(i); /* [한국어] 진행 중 I/O 완료 대기 시작 */
}

/*
 * [한국어]
 * bdev_lock_lba_range_ctx - 모든 채널에 LBA 범위 잠금 요청 시작
 *
 * @bdev: 대상 bdev
 * @ctx:  잠금 컨텍스트
 *
 * owner_thread에서만 호출 가능.
 * spdk_bdev_for_each_channel()로 모든 채널에 range 추가 및 I/O 대기.
 *
 * 호출 체인:
 *   _bdev_lock_lba_range() / bdev_lock_lba_range_ctx_msg() → [이 함수]
 */
static void
bdev_lock_lba_range_ctx(struct spdk_bdev *bdev, struct locked_lba_range_ctx *ctx)
{
	assert(spdk_get_thread() == ctx->range.owner_thread);
	assert(ctx->range.owner_ch == NULL ||
	       spdk_io_channel_get_thread(ctx->range.owner_ch->channel) == ctx->range.owner_thread);

	/* We will add a copy of this range to each channel now. */
	/* [한국어] 모든 채널에 range 복사본 추가 시작 */
	spdk_bdev_for_each_channel(bdev, bdev_lock_lba_range_get_channel, ctx,
				   bdev_lock_lba_range_cb);
}

/*
 * [한국어]
 * bdev_lba_range_overlaps_tailq - range가 tailq의 어떤 range와 겹치는지 확인
 *
 * @range: 확인할 범위
 * @tailq: 비교할 범위 목록
 * @return: true=겹치는 range 있음
 *
 * 호출 체인:
 *   _bdev_lock_lba_range() / bdev_unlock_lba_range_cb() → [이 함수]
 */
static bool
bdev_lba_range_overlaps_tailq(struct lba_range *range, lba_range_tailq_t *tailq)
{
	struct lba_range *r;

	TAILQ_FOREACH(r, tailq, tailq) {
		if (bdev_lba_range_overlapped(range, r)) {
			return true; /* [한국어] 겹치는 range 발견 */
		}
	}
	return false; /* [한국어] 겹치는 range 없음 */
}

static void bdev_quiesce_range_locked(struct lba_range *range, void *ctx, int status);

/*
 * [한국어]
 * _bdev_lock_lba_range - LBA 범위 잠금 내부 구현
 *
 * @bdev:   대상 bdev
 * @ch:     소유 채널 (NULL이면 quiesce)
 * @offset: 범위 시작 블록 오프셋
 * @length: 범위 블록 수
 * @cb_fn:  잠금 완료 콜백
 * @cb_arg: 콜백 인수 (locked_ctx로 사용)
 * @return: 0=성공, -ENOMEM
 *
 * 현재 겹치는 잠금이 있으면 pending_locked_ranges에 대기.
 * 없으면 locked_ranges에 즉시 추가 후 채널별 잠금 시작.
 *
 * 호출 체인:
 *   bdev_lock_lba_range() / spdk_bdev_quiesce_range() → [이 함수]
 */
static int
_bdev_lock_lba_range(struct spdk_bdev *bdev, struct spdk_bdev_channel *ch,
		     uint64_t offset, uint64_t length,
		     lock_range_cb cb_fn, void *cb_arg)
{
	struct locked_lba_range_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx)); /* [한국어] 잠금 컨텍스트 할당 */
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->range.offset = offset;                         /* [한국어] 범위 시작 오프셋 */
	ctx->range.length = length;                         /* [한국어] 범위 블록 수 */
	ctx->range.owner_thread = spdk_get_thread();        /* [한국어] 소유 스레드 */
	ctx->range.owner_ch = ch;                           /* [한국어] 소유 채널 (quiesce이면 NULL) */
	ctx->range.locked_ctx = cb_arg;                     /* [한국어] 잠금 컨텍스트 식별자 */
	ctx->range.bdev = bdev;                             /* [한국어] 대상 bdev */
	ctx->range.quiesce = (cb_fn == bdev_quiesce_range_locked); /* [한국어] quiesce 모드 여부 */
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_spin_lock(&bdev->internal.spinlock); /* [한국어] locked_ranges/pending 보호 */
	if (bdev_lba_range_overlaps_tailq(&ctx->range, &bdev->internal.locked_ranges)) {
		/* There is an active lock overlapping with this range.
		 * Put it on the pending list until this range no
		 * longer overlaps with another.
		 */
		/* [한국어] 겹치는 잠금 있음 → 대기 목록에 삽입 */
		TAILQ_INSERT_TAIL(&bdev->internal.pending_locked_ranges, &ctx->range, tailq);
	} else {
		/* [한국어] 겹치는 잠금 없음 → 즉시 잠금 시작 */
		TAILQ_INSERT_TAIL(&bdev->internal.locked_ranges, &ctx->range, tailq);
		bdev_lock_lba_range_ctx(bdev, ctx);
	}
	spdk_spin_unlock(&bdev->internal.spinlock);
	return 0;
}

/*
 * [한국어]
 * bdev_lock_lba_range - desc와 채널 기반 LBA 범위 잠금 (cb_arg NULL 검증)
 *
 * @desc:   대상 디스크립터
 * @_ch:    소유 I/O 채널
 * @offset: 범위 시작 오프셋
 * @length: 범위 블록 수
 * @cb_fn:  잠금 완료 콜백
 * @cb_arg: 콜백 인수 (locked_ctx로 사용, NULL 불가)
 * @return: 0=성공, -EINVAL/-ENOMEM
 *
 * 호출 체인:
 *   spdk_bdev_comparev_and_writev_blocks() / 사용자 → [이 함수] → _bdev_lock_lba_range()
 */
static int
bdev_lock_lba_range(struct spdk_bdev_desc *desc, struct spdk_io_channel *_ch,
		    uint64_t offset, uint64_t length,
		    lock_range_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);

	if (cb_arg == NULL) {
		SPDK_ERRLOG("cb_arg must not be NULL\n");
		return -EINVAL; /* [한국어] cb_arg는 locked_ctx로 사용되므로 NULL 불가 */
	}

	return _bdev_lock_lba_range(bdev, ch, offset, length, cb_fn, cb_arg);
}

/*
 * [한국어]
 * bdev_lock_lba_range_ctx_msg - 원래 소유 스레드에서 잠금 컨텍스트 시작
 *
 * @_ctx: locked_lba_range_ctx 포인터
 *
 * pending_locked_ranges에서 unlock 후 깨어날 때 원래 스레드에서 실행.
 *
 * 호출 체인:
 *   bdev_unlock_lba_range_cb() → spdk_thread_send_msg → [이 함수]
 */
static void
bdev_lock_lba_range_ctx_msg(void *_ctx)
{
	struct locked_lba_range_ctx *ctx = _ctx;

	bdev_lock_lba_range_ctx(ctx->range.bdev, ctx); /* [한국어] 잠금 프로세스 시작 */
}

/*
 * [한국어]
 * bdev_unlock_lba_range_cb - 모든 채널 LBA 범위 해제 완료 콜백
 *
 * @bdev:   대상 bdev
 * @_ctx:   locked_lba_range_ctx 포인터
 * @status: 완료 상태
 *
 * 대기 중인 pending_locked_ranges 중 해제된 범위와 겹치는 것을 활성화.
 * bdev가 UNREGISTERING 중이고 locked_ranges가 비었으면 unregister 재시작.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel(bdev_unlock_lba_range_get_channel) 완료 → [이 함수]
 */
static void
bdev_unlock_lba_range_cb(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct locked_lba_range_ctx *ctx = _ctx;
	struct locked_lba_range_ctx *pending_ctx;
	struct lba_range *range, *tmp;

	spdk_spin_lock(&bdev->internal.spinlock);
	/* Check if there are any pending locked ranges that overlap with this range
	 * that was just unlocked.  If there are, check that it doesn't overlap with any
	 * other locked ranges before calling bdev_lock_lba_range_ctx which will start
	 * the lock process.
	 */
	/* [한국어] 해제된 범위와 겹치는 pending range를 활성화 가능한지 확인 */
	TAILQ_FOREACH_SAFE(range, &bdev->internal.pending_locked_ranges, tailq, tmp) {
		if (bdev_lba_range_overlapped(range, &ctx->range) &&
		    !bdev_lba_range_overlaps_tailq(range, &bdev->internal.locked_ranges)) {
			/* [한국어] 이 pending range가 더 이상 다른 잠금과 겹치지 않음 → 활성화 */
			TAILQ_REMOVE(&bdev->internal.pending_locked_ranges, range, tailq);
			pending_ctx = SPDK_CONTAINEROF(range, struct locked_lba_range_ctx, range);
			TAILQ_INSERT_TAIL(&bdev->internal.locked_ranges, range, tailq);
			spdk_thread_send_msg(pending_ctx->range.owner_thread,
					     bdev_lock_lba_range_ctx_msg, pending_ctx);
			/* [한국어] 원래 소유 스레드에서 잠금 프로세스 재시작 */
		}
	}

	if (bdev->internal.status == SPDK_BDEV_STATUS_UNREGISTERING &&
	    TAILQ_EMPTY(&bdev->internal.locked_ranges)) {
		/* [한국어] UNREGISTERING 중 + 잠금 모두 해제 → unregister 재시도 */
		spdk_thread_send_msg(bdev->internal.unregister_td, _bdev_unregister, bdev);
	}
	spdk_spin_unlock(&bdev->internal.spinlock);

	ctx->cb_fn(&ctx->range, ctx->cb_arg, status); /* [한국어] 해제 완료 콜백 */
	free(ctx); /* [한국어] ctx 해제 */
}

/*
 * [한국어]
 * bdev_unlock_lba_range_get_channel - 채널별 LBA 범위 잠금 해제 및 대기 I/O 재제출
 *
 * @i:    채널 반복자
 * @bdev: 대상 bdev
 * @_ch:  현재 채널
 * @_ctx: locked_lba_range_ctx 포인터
 *
 * ch->locked_ranges에서 해당 range 탐색 후 제거/해제.
 * 새 채널 생성 경쟁 조건으로 range가 없는 경우도 허용 (assert 없음).
 * io_locked의 대기 I/O를 임시 큐로 교환 후 재제출.
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → [이 함수] → bdev_unlock_lba_range_cb()
 */
static void
bdev_unlock_lba_range_get_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
				  struct spdk_io_channel *_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	struct locked_lba_range_ctx *ctx = _ctx;
	TAILQ_HEAD(, spdk_bdev_io) io_locked;
	struct spdk_bdev_io *bdev_io;
	struct lba_range *range;

	/* [한국어] 채널의 locked_ranges에서 해당 range 탐색 */
	TAILQ_FOREACH(range, &ch->locked_ranges, tailq) {
		if (ctx->range.offset == range->offset &&
		    ctx->range.length == range->length &&
		    ctx->range.locked_ctx == range->locked_ctx) {
			TAILQ_REMOVE(&ch->locked_ranges, range, tailq); /* [한국어] locked_ranges에서 제거 */
			free(range); /* [한국어] 채널별 range 구조체 해제 */
			break;
		}
	}

	/* Note: we should almost always be able to assert that the range specified
	 * was found.  But there are some very rare corner cases where a new channel
	 * gets created simultaneously with a range unlock, where this function
	 * would execute on that new channel and wouldn't have the range.
	 * We also use this to clean up range allocations when a later allocation
	 * fails in the locking path.
	 * So we can't actually assert() here.
	 */
	/* [한국어] 새 채널 생성 경쟁 조건으로 range가 없는 경우도 정상이므로 assert 없음 */

	/* Swap the locked IO into a temporary list, and then try to submit them again.
	 * We could hyper-optimize this to only resubmit locked I/O that overlap
	 * with the range that was just unlocked, but this isn't a performance path so
	 * we go for simplicity here.
	 */
	/* [한국어] io_locked의 대기 I/O를 임시 큐로 교환 후 재제출 */
	TAILQ_INIT(&io_locked);
	TAILQ_SWAP(&ch->io_locked, &io_locked, spdk_bdev_io, internal.ch_link);
	/* [한국어] ch->io_locked와 임시 큐 교환 (원자적으로 모든 대기 I/O 가져오기) */
	while (!TAILQ_EMPTY(&io_locked)) {
		bdev_io = TAILQ_FIRST(&io_locked);
		TAILQ_REMOVE(&io_locked, bdev_io, internal.ch_link);
		bdev_io_submit(bdev_io); /* [한국어] 대기 I/O 재제출 (이제 범위 잠금 해제됨) */
	}

	spdk_bdev_for_each_channel_continue(i, 0); /* [한국어] 다음 채널로 진행 */
}

/*
 * [한국어]
 * _bdev_unlock_lba_range - LBA 범위 잠금 해제 내부 구현
 *
 * @bdev:   대상 bdev
 * @offset: 범위 시작 오프셋
 * @length: 범위 블록 수
 * @cb_fn:  해제 완료 콜백
 * @cb_arg: 콜백 인수 (잠금 ctx 식별용)
 * @return: 0=성공, -EINVAL=범위 미발견
 *
 * bdev->internal.locked_ranges에서 해당 range를 제거 (새 채널이 상속 안 함).
 * 각 채널에 bdev_unlock_lba_range_get_channel 메시지 전송.
 *
 * 호출 체인:
 *   bdev_unlock_lba_range() / _spdk_bdev_quiesce(unquiesce) → [이 함수]
 */
static int
_bdev_unlock_lba_range(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
		       lock_range_cb cb_fn, void *cb_arg)
{
	struct locked_lba_range_ctx *ctx;
	struct lba_range *range;

	spdk_spin_lock(&bdev->internal.spinlock);
	/* To start the unlock the process, we find the range in the bdev's locked_ranges
	 * and remove it. This ensures new channels don't inherit the locked range.
	 * Then we will send a message to each channel to remove the range from its
	 * per-channel list.
	 */
	/* [한국어] 먼저 bdev 전역 locked_ranges에서 제거 → 새 채널은 이 범위를 상속하지 않음 */
	TAILQ_FOREACH(range, &bdev->internal.locked_ranges, tailq) {
		if (range->offset == offset && range->length == length &&
		    (range->owner_ch == NULL || range->locked_ctx == cb_arg)) {
			break; /* [한국어] 해당 range 탐색 */
		}
	}
	if (range == NULL) {
		assert(false); /* [한국어] 잠금 없는 해제 → 프로그래밍 오류 */
		spdk_spin_unlock(&bdev->internal.spinlock);
		return -EINVAL;
	}
	TAILQ_REMOVE(&bdev->internal.locked_ranges, range, tailq); /* [한국어] 전역 목록에서 제거 */
	ctx = SPDK_CONTAINEROF(range, struct locked_lba_range_ctx, range); /* [한국어] range → ctx 변환 */
	spdk_spin_unlock(&bdev->internal.spinlock);

	ctx->cb_fn = cb_fn; /* [한국어] 새 완료 콜백 설정 */
	ctx->cb_arg = cb_arg;

	spdk_bdev_for_each_channel(bdev, bdev_unlock_lba_range_get_channel, ctx,
				   bdev_unlock_lba_range_cb); /* [한국어] 채널별 range 제거 시작 */
	return 0;
}

/*
 * [한국어]
 * bdev_unlock_lba_range - desc와 채널 기반 LBA 범위 잠금 해제 (owner_ch 검증)
 *
 * @desc:   대상 디스크립터
 * @_ch:    해제할 채널 (소유 채널이어야 함)
 * @offset: 범위 시작 오프셋
 * @length: 범위 블록 수
 * @cb_fn:  해제 완료 콜백
 * @cb_arg: 콜백 인수
 * @return: 0=성공, -EINVAL=범위 미발견
 *
 * 채널의 locked_ranges에서 owner_ch==ch이고 locked_ctx==cb_arg인 range 검증 후 해제.
 *
 * 호출 체인:
 *   spdk_bdev_comparev_and_writev_blocks() 완료 → [이 함수]
 */
static int
bdev_unlock_lba_range(struct spdk_bdev_desc *desc, struct spdk_io_channel *_ch,
		      uint64_t offset, uint64_t length,
		      lock_range_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	struct lba_range *range;
	bool range_found = false;

	/* Let's make sure the specified channel actually has a lock on
	 * the specified range.  Note that the range must match exactly.
	 */
	/* [한국어] 이 채널이 실제로 해당 범위를 잠금 소유하는지 검증 */
	TAILQ_FOREACH(range, &ch->locked_ranges, tailq) {
		if (range->offset == offset && range->length == length &&
		    range->owner_ch == ch && range->locked_ctx == cb_arg) {
			range_found = true; /* [한국어] 소유 range 발견 */
			break;
		}
	}

	if (!range_found) {
		return -EINVAL; /* [한국어] 소유하지 않는 범위 해제 시도 */
	}

	return _bdev_unlock_lba_range(bdev, offset, length, cb_fn, cb_arg);
}

/*
 * [한국어] bdev_quiesce_ctx - bdev quiesce/unquiesce 컨텍스트
 */
struct bdev_quiesce_ctx {
	spdk_bdev_quiesce_cb cb_fn;
	/* [한국어] quiesce/unquiesce 완료 콜백.
	 * 설정자: _spdk_bdev_quiesce().
	 * 읽는 자: bdev_quiesce_range_locked() / bdev_unquiesce_range_unlocked().
	 * 동기화: quiesce 콜백 실행 후 NULL로 설정 (중복 호출 방지). */

	void *cb_arg;
	/* [한국어] 콜백 인수.
	 * 설정자: _spdk_bdev_quiesce().
	 * 동기화: quiesce 콜백 후 NULL 설정. */
};

/*
 * [한국어]
 * bdev_unquiesce_range_unlocked - LBA 범위 unquiesce(잠금 해제) 완료 콜백
 *
 * @range: 해제된 LBA 범위
 * @ctx:   bdev_quiesce_ctx 포인터
 * @status: 해제 상태
 *
 * 완료 콜백 호출 후 ctx 해제.
 *
 * 호출 체인:
 *   _bdev_unlock_lba_range() 완료 → [이 함수]
 */
static void
bdev_unquiesce_range_unlocked(struct lba_range *range, void *ctx, int status)
{
	struct bdev_quiesce_ctx *quiesce_ctx = ctx;

	if (quiesce_ctx->cb_fn != NULL) {
		quiesce_ctx->cb_fn(quiesce_ctx->cb_arg, status); /* [한국어] unquiesce 완료 콜백 */
	}

	free(quiesce_ctx); /* [한국어] ctx 해제 */
}

/*
 * [한국어]
 * bdev_quiesce_range_locked - LBA 범위 quiesce(잠금) 완료 콜백
 *
 * @range:  잠긴 LBA 범위
 * @ctx:    bdev_quiesce_ctx 포인터
 * @status: 잠금 상태
 *
 * 성공: range를 module->internal.quiesced_ranges에 추가 → 콜백 호출.
 * 콜백 내부에서 unquiesce가 호출될 수 있으므로 tmp 복사 후 실행.
 * ctx는 unquiesce 시 해제됨.
 *
 * 호출 체인:
 *   _bdev_lock_lba_range() 완료 → [이 함수]
 */
static void
bdev_quiesce_range_locked(struct lba_range *range, void *ctx, int status)
{
	struct bdev_quiesce_ctx *quiesce_ctx = ctx;
	struct spdk_bdev_module *module = range->bdev->module;

	if (status != 0) {
		/* [한국어] 잠금 실패 → 콜백에 오류 전달 후 ctx 해제 */
		if (quiesce_ctx->cb_fn != NULL) {
			quiesce_ctx->cb_fn(quiesce_ctx->cb_arg, status);
		}
		free(quiesce_ctx);
		return;
	}

	spdk_spin_lock(&module->internal.spinlock);
	TAILQ_INSERT_TAIL(&module->internal.quiesced_ranges, range, tailq_module);
	/* [한국어] 모듈의 quiesce 목록에 등록 (unquiesce 시 탐색용) */
	spdk_spin_unlock(&module->internal.spinlock);

	if (quiesce_ctx->cb_fn != NULL) {
		/* copy the context in case the range is unlocked by the callback */
		/* [한국어] 콜백 내에서 unquiesce가 호출될 수 있으므로 tmp에 복사 */
		struct bdev_quiesce_ctx tmp = *quiesce_ctx;

		quiesce_ctx->cb_fn = NULL; /* [한국어] 중복 호출 방지 */
		quiesce_ctx->cb_arg = NULL;

		tmp.cb_fn(tmp.cb_arg, status); /* [한국어] quiesce 완료 콜백 */
	}
	/* quiesce_ctx will be freed on unquiesce */
	/* [한국어] ctx는 unquiesce 시 bdev_unquiesce_range_unlocked()에서 해제 */
}

/*
 * [한국어]
 * _spdk_bdev_quiesce - bdev LBA 범위 quiesce/unquiesce 내부 구현
 *
 * @bdev:      대상 bdev
 * @module:    요청 모듈
 * @offset:    범위 시작 오프셋
 * @length:    범위 블록 수
 * @cb_fn:     완료 콜백
 * @cb_arg:    콜백 인수
 * @unquiesce: true=unquiesce, false=quiesce
 * @return:    0=성공, -EINVAL/-ENOMEM/-ENODEV
 *
 * quiesce: LBA 범위 잠금 획득 → module.quiesced_ranges에 등록.
 * unquiesce: quiesced_ranges에서 제거 → 잠금 해제.
 *
 * 호출 체인:
 *   spdk_bdev_quiesce/unquiesce/_range → [이 함수]
 */
static int
_spdk_bdev_quiesce(struct spdk_bdev *bdev, struct spdk_bdev_module *module,
		   uint64_t offset, uint64_t length,
		   spdk_bdev_quiesce_cb cb_fn, void *cb_arg,
		   bool unquiesce)
{
	struct bdev_quiesce_ctx *quiesce_ctx; /* [한국어] quiesce 콜백·arg를 감싸는 컨텍스트 */
	int rc; /* [한국어] 각 내부 함수의 반환값 수집 */

	if (module != bdev->module) {
		/* [한국어] 모듈 소유권 검증 실패 — 잘못된 모듈이 quiesce 요청함 */
		SPDK_ERRLOG("Bdev does not belong to specified module.\n");
		return -EINVAL; /* [한국어] 호출자에게 잘못된 인수 오류 반환 */
	}

	if (!bdev_io_valid_blocks(bdev, offset, length)) {
		/* [한국어] LBA 범위 유효성 검증 실패 (범위 초과 또는 0 블록) */
		return -EINVAL; /* [한국어] 유효하지 않은 블록 범위 오류 */
	}

	spdk_spin_lock(&bdev->internal.spinlock); /* [한국어] bdev 상태 확인을 위한 스핀락 획득 */
	if (bdev->internal.status == SPDK_BDEV_STATUS_REMOVING && TAILQ_EMPTY(&bdev->internal.open_descs)) {
		/* [한국어] bdev가 제거 중이고 열린 디스크립터도 없음 → 더 이상 quiesce 불가 */
		spdk_spin_unlock(&bdev->internal.spinlock);
		return -ENODEV; /* [한국어] 디바이스 제거 중 오류 반환 */
	}
	spdk_spin_unlock(&bdev->internal.spinlock); /* [한국어] 상태 확인 완료 후 스핀락 해제 */

	if (unquiesce) {
		/* [한국어] unquiesce 경로: quiesced_ranges에서 해당 범위를 찾아 잠금 해제 */
		struct lba_range *range; /* [한국어] quiesced_ranges에서 찾은 대상 범위 포인터 */

		/* Make sure the specified range is actually quiesced in the specified module and
		 * then remove it from the list. Note that the range must match exactly.
		 */
		spdk_spin_lock(&module->internal.spinlock); /* [한국어] 모듈 quiesced_ranges 접근을 위한 스핀락 */
		TAILQ_FOREACH(range, &module->internal.quiesced_ranges, tailq_module) {
			/* [한국어] offset, length가 정확히 일치하는 범위 검색 (부분 일치 불허) */
			if (range->bdev == bdev && range->offset == offset && range->length == length) {
				TAILQ_REMOVE(&module->internal.quiesced_ranges, range, tailq_module);
				/* [한국어] quiesced_ranges에서 제거 — 이후 잠금 해제 진행 */
				break;
			}
		}
		spdk_spin_unlock(&module->internal.spinlock); /* [한국어] quiesced_ranges 탐색 완료 후 스핀락 해제 */

		if (range == NULL) {
			/* [한국어] 요청한 범위가 quiesced_ranges에 없음 — 잘못된 unquiesce 요청 */
			SPDK_ERRLOG("The range to unquiesce was not found.\n");
			return -EINVAL; /* [한국어] 범위를 찾지 못했으므로 오류 반환 */
		}

		quiesce_ctx = range->locked_ctx; /* [한국어] quiesce 시 저장한 ctx 복원 (unquiesce 콜백/arg 재사용) */
		quiesce_ctx->cb_fn = cb_fn; /* [한국어] unquiesce 완료 시 호출할 콜백 설정 */
		quiesce_ctx->cb_arg = cb_arg; /* [한국어] unquiesce 완료 콜백 인수 설정 */

		rc = _bdev_unlock_lba_range(bdev, offset, length, bdev_unquiesce_range_unlocked, quiesce_ctx);
		/* [한국어] LBA 범위 잠금 해제 요청 → bdev_unquiesce_range_unlocked()에서 ctx 해제 및 콜백 호출 */
	} else {
		/* [한국어] quiesce 경로: quiesce_ctx 할당 후 LBA 범위 잠금 요청 */
		quiesce_ctx = malloc(sizeof(*quiesce_ctx)); /* [한국어] quiesce 컨텍스트 동적 할당 */
		if (quiesce_ctx == NULL) {
			/* [한국어] 메모리 할당 실패 → 오류 반환 */
			return -ENOMEM;
		}

		quiesce_ctx->cb_fn = cb_fn; /* [한국어] quiesce 완료 시 호출할 콜백 설정 */
		quiesce_ctx->cb_arg = cb_arg; /* [한국어] quiesce 완료 콜백 인수 설정 */

		rc = _bdev_lock_lba_range(bdev, NULL, offset, length, bdev_quiesce_range_locked, quiesce_ctx);
		/* [한국어] LBA 범위 잠금 요청 → 완료 시 bdev_quiesce_range_locked() 콜백 호출
		 * ch=NULL: 특정 채널에 종속되지 않는 전역 quiesce 잠금 */
		if (rc != 0) {
			free(quiesce_ctx); /* [한국어] 잠금 요청 실패 시 ctx 즉시 해제 */
		}
	}

	return rc; /* [한국어] 0=잠금/해제 요청 성공, 음수=오류 코드 */
}

/*
 * [한국어]
 * spdk_bdev_quiesce - bdev 전체 범위를 quiesce (일시 정지)
 *
 * @bdev:   대상 bdev
 * @module: 요청 모듈 (bdev->module과 일치해야 함)
 * @cb_fn:  quiesce 완료 콜백
 * @cb_arg: 콜백 인수
 * @return: 0=성공(비동기 진행 중), 음수=즉시 오류
 *
 * bdev 전체 블록 범위(0 ~ blockcnt)에 대해 quiesce를 요청한다.
 * 실제 구현은 _spdk_bdev_quiesce()에 위임하며, 오프셋=0·길이=blockcnt·unquiesce=false로 전달한다.
 * quiesce가 완료되면 새로운 I/O는 해당 범위에 진입하지 못하며,
 * 진행 중인 I/O가 모두 완료된 후 cb_fn이 호출된다.
 *
 * 실행 컨텍스트: SPDK 스레드 (bdev reactor)
 *
 * 호출 체인:
 *   bdev 모듈(요청자) → [이 함수] → _spdk_bdev_quiesce()
 *                                 → _bdev_lock_lba_range()
 *                                 → bdev_quiesce_range_locked() [완료 콜백]
 */
int
spdk_bdev_quiesce(struct spdk_bdev *bdev, struct spdk_bdev_module *module,
		  spdk_bdev_quiesce_cb cb_fn, void *cb_arg)
{
	return _spdk_bdev_quiesce(bdev, module, 0, bdev->blockcnt, cb_fn, cb_arg, false);
	/* [한국어] offset=0, length=blockcnt로 전체 범위 quiesce 요청 */
}

/*
 * [한국어]
 * spdk_bdev_unquiesce - bdev 전체 범위 quiesce 해제
 *
 * @bdev:   대상 bdev
 * @module: 요청 모듈 (bdev->module과 일치해야 함)
 * @cb_fn:  unquiesce 완료 콜백
 * @cb_arg: 콜백 인수
 * @return: 0=성공(비동기 진행 중), -EINVAL=범위를 찾지 못함, 음수=기타 오류
 *
 * spdk_bdev_quiesce()로 걸어둔 전체 범위 quiesce를 해제한다.
 * quiesced_ranges에서 정확히 일치하는 범위를 찾아 LBA 잠금을 해제하며,
 * 대기 중이던 I/O들이 순차적으로 재개된다.
 * 완료 후 cb_fn이 호출된다.
 *
 * 실행 컨텍스트: SPDK 스레드 (bdev reactor)
 *
 * 호출 체인:
 *   bdev 모듈(요청자) → [이 함수] → _spdk_bdev_quiesce()
 *                                 → _bdev_unlock_lba_range()
 *                                 → bdev_unquiesce_range_unlocked() [완료 콜백]
 */
int
spdk_bdev_unquiesce(struct spdk_bdev *bdev, struct spdk_bdev_module *module,
		    spdk_bdev_quiesce_cb cb_fn, void *cb_arg)
{
	return _spdk_bdev_quiesce(bdev, module, 0, bdev->blockcnt, cb_fn, cb_arg, true);
	/* [한국어] offset=0, length=blockcnt로 전체 범위 unquiesce 요청 */
}

/*
 * [한국어]
 * spdk_bdev_quiesce_range - bdev의 특정 LBA 범위를 quiesce
 *
 * @bdev:    대상 bdev
 * @module:  요청 모듈
 * @offset:  quiesce할 시작 LBA (블록 단위)
 * @length:  quiesce할 블록 수
 * @cb_fn:   quiesce 완료 콜백
 * @cb_arg:  콜백 인수
 * @return:  0=성공(비동기 진행 중), 음수=즉시 오류
 *
 * bdev의 특정 LBA 범위만 선택적으로 quiesce한다.
 * 해당 범위에 대한 새로운 I/O 제출이 차단되고,
 * 진행 중인 I/O가 모두 완료된 후 cb_fn이 호출된다.
 * 다른 범위의 I/O는 영향받지 않는다.
 *
 * 실행 컨텍스트: SPDK 스레드 (bdev reactor)
 *
 * 호출 체인:
 *   bdev 모듈(요청자) → [이 함수] → _spdk_bdev_quiesce()
 */
int
spdk_bdev_quiesce_range(struct spdk_bdev *bdev, struct spdk_bdev_module *module,
			uint64_t offset, uint64_t length,
			spdk_bdev_quiesce_cb cb_fn, void *cb_arg)
{
	return _spdk_bdev_quiesce(bdev, module, offset, length, cb_fn, cb_arg, false);
	/* [한국어] 지정된 오프셋·길이로 부분 범위 quiesce 요청 */
}

/*
 * [한국어]
 * spdk_bdev_unquiesce_range - bdev의 특정 LBA 범위 quiesce 해제
 *
 * @bdev:    대상 bdev
 * @module:  요청 모듈
 * @offset:  해제할 시작 LBA (블록 단위)
 * @length:  해제할 블록 수
 * @cb_fn:   unquiesce 완료 콜백
 * @cb_arg:  콜백 인수
 * @return:  0=성공(비동기 진행 중), -EINVAL=범위 미존재, 음수=기타 오류
 *
 * spdk_bdev_quiesce_range()로 잠근 특정 LBA 범위를 해제한다.
 * offset과 length가 정확히 일치하는 범위만 해제되며,
 * 대기 중이던 해당 범위 I/O들이 재개된다.
 *
 * 실행 컨텍스트: SPDK 스레드 (bdev reactor)
 *
 * 호출 체인:
 *   bdev 모듈(요청자) → [이 함수] → _spdk_bdev_quiesce()
 *                                 → _bdev_unlock_lba_range()
 */
int
spdk_bdev_unquiesce_range(struct spdk_bdev *bdev, struct spdk_bdev_module *module,
			  uint64_t offset, uint64_t length,
			  spdk_bdev_quiesce_cb cb_fn, void *cb_arg)
{
	return _spdk_bdev_quiesce(bdev, module, offset, length, cb_fn, cb_arg, true);
	/* [한국어] 지정된 오프셋·길이로 부분 범위 unquiesce 요청 */
}

/*
 * [한국어]
 * spdk_bdev_get_memory_domains - bdev가 지원하는 메모리 도메인 목록 반환
 *
 * @bdev:       대상 bdev
 * @domains:    도메인 포인터 배열 (출력 버퍼, NULL 허용)
 * @array_size: domains 배열 크기 (0 허용)
 * @return:     지원하는 메모리 도메인 수 (배열 크기 초과 시에도 총 개수 반환),
 *              bdev==NULL이면 -EINVAL, 미지원 모듈이면 0
 *
 * SPDK 메모리 도메인(Memory Domain)은 DMA 가능 메모리의 위치(호스트/GPU/RDMA 등)를
 * 추상화한 개념이다. bdev 모듈이 특정 메모리 도메인의 I/O를 직접 지원하면
 * DMA copy 없이 제로카피 전송이 가능하다.
 *
 * domains=NULL로 호출하면 지원하는 도메인 수만 반환받을 수 있으며,
 * 이후 적절한 크기 배열을 할당하여 재호출하는 패턴이 일반적이다.
 *
 * 실행 컨텍스트: SPDK 스레드 또는 애플리케이션 스레드
 *
 * 호출 체인:
 *   애플리케이션/bdev 상위 계층 → [이 함수] → bdev->fn_table->get_memory_domains()
 */
int
spdk_bdev_get_memory_domains(struct spdk_bdev *bdev, struct spdk_memory_domain **domains,
			     int array_size)
{
	if (!bdev) {
		/* [한국어] NULL bdev 포인터 — 잘못된 호출 */
		return -EINVAL;
	}

	if (bdev->fn_table->get_memory_domains) {
		/* [한국어] 모듈이 get_memory_domains를 구현한 경우 → 모듈에 위임 */
		return bdev->fn_table->get_memory_domains(bdev->ctxt, domains, array_size);
	}

	return 0; /* [한국어] 모듈이 get_memory_domains를 구현하지 않음 → 도메인 없음(0) */
}

/*
 * [한국어]
 * spdk_bdev_for_each_io_ctx - spdk_bdev_for_each_bdev_io() 순회 컨텍스트
 *
 * spdk_bdev_for_each_channel()을 통해 모든 채널의 진행 중인 bdev_io를
 * 순회할 때 사용되는 내부 컨텍스트 구조체.
 * 호출자가 제공한 순회 콜백(fn)과 완료 콜백(cb)을 보관한다.
 */
struct spdk_bdev_for_each_io_ctx {
	void *ctx;
	/* [한국어] 호출자가 전달한 사용자 컨텍스트 포인터.
	 * 설정자: spdk_bdev_for_each_bdev_io() 호출 시 _ctx로 전달받아 저장.
	 * 읽는 자: bdev_channel_for_each_io()에서 fn() 콜백 호출 시 첫 번째 인수로 전달.
	 * 값 범위: 임의 포인터 (NULL 허용).
	 * 동기화: spdk_bdev_for_each_channel()이 직렬로 채널을 순회하므로 별도 락 불필요. */

	spdk_bdev_io_fn fn;
	/* [한국어] 각 bdev_io에 대해 호출되는 순회 콜백 함수 포인터.
	 * 설정자: spdk_bdev_for_each_bdev_io() 호출 시 fn 파라미터로 전달받아 저장.
	 * 읽는 자: bdev_channel_for_each_io()가 채널의 io_submitted 리스트를 순회하며 호출.
	 * 값 범위: NULL 불가 (assert로 보장됨).
	 * 동기화: 직렬 채널 순회 — 동시 접근 없음. */

	spdk_bdev_for_each_io_cb cb;
	/* [한국어] 모든 채널 순회가 완료됐을 때 호출되는 완료 콜백 함수 포인터.
	 * 설정자: spdk_bdev_for_each_bdev_io() 호출 시 cb 파라미터로 전달받아 저장.
	 * 읽는 자: bdev_for_each_io_done()에서 status와 함께 호출됨.
	 * 값 범위: NULL 불가 (assert로 보장됨).
	 * 동기화: bdev_for_each_io_done()은 마지막 채널 완료 후 단일 스레드에서 호출됨. */
};

/*
 * [한국어]
 * bdev_channel_for_each_io - 채널별 진행 중인 bdev_io 순회 콜백
 *
 * @i:     채널 이터레이터 (spdk_bdev_for_each_channel_continue 호출에 사용)
 * @bdev:  현재 순회 중인 bdev
 * @io_ch: 현재 처리 중인 io_channel (bdev_channel으로 변환하여 사용)
 * @_ctx:  spdk_bdev_for_each_io_ctx 포인터 (사용자 콜백·컨텍스트 보관)
 *
 * spdk_bdev_for_each_channel()이 각 채널에 대해 호출하는 순회 콜백.
 * 채널의 io_submitted TAILQ를 순회하며 각 bdev_io에 대해 사용자 fn()을 호출한다.
 * fn()이 0이 아닌 값을 반환하면 즉시 순회를 중단하고 오류를 전파한다.
 *
 * 실행 컨텍스트: 해당 채널의 SPDK 스레드(reactor)에서 직렬로 실행됨
 *
 * 호출 체인:
 *   spdk_bdev_for_each_channel() → bdev_each_channel_msg()
 *                                → [이 함수] → ctx->fn()
 *                                            → spdk_bdev_for_each_channel_continue()
 */
static void
bdev_channel_for_each_io(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			 struct spdk_io_channel *io_ch, void *_ctx)
{
	struct spdk_bdev_for_each_io_ctx *ctx = _ctx; /* [한국어] 사용자 fn/cb/ctx를 담은 순회 컨텍스트 */
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(io_ch); /* [한국어] io_channel → bdev_channel 변환 */
	struct spdk_bdev_io *bdev_io; /* [한국어] 현재 순회 중인 bdev_io 포인터 */
	int rc = 0; /* [한국어] fn() 반환값 누적 (0=계속, 비0=중단) */

	TAILQ_FOREACH(bdev_io, &bdev_ch->io_submitted, internal.ch_link) {
		/* [한국어] 이 채널에 제출된 모든 bdev_io를 순회하며 사용자 콜백 호출 */
		rc = ctx->fn(ctx->ctx, bdev_io); /* [한국어] 사용자 순회 콜백 — bdev_io마다 호출 */
		if (rc != 0) {
			/* [한국어] 콜백이 오류 반환 → 순회 즉시 중단 */
			break;
		}
	}

	spdk_bdev_for_each_channel_continue(i, rc);
	/* [한국어] 현재 채널 처리 완료 신호 전달 → 다음 채널로 진행 또는 cpl 호출
	 * rc!=0이면 이후 채널 순회 없이 bdev_for_each_io_done()으로 직행 */
}

/*
 * [한국어]
 * bdev_for_each_io_done - 모든 채널 bdev_io 순회 완료 콜백
 *
 * @bdev:   순회 대상 bdev (사용하지 않음)
 * @_ctx:   spdk_bdev_for_each_io_ctx 포인터
 * @status: 순회 결과 (0=모든 채널 성공, 비0=중간 채널에서 오류)
 *
 * spdk_bdev_for_each_channel()이 모든 채널 순회를 마친 후 호출하는 완료 콜백.
 * 사용자 완료 콜백(cb)을 호출한 뒤 순회 컨텍스트를 해제한다.
 * status가 비0이면 중간 채널에서 fn()이 오류를 반환한 것이므로
 * 호출자는 이를 확인하여 적절히 처리해야 한다.
 *
 * 실행 컨텍스트: spdk_bdev_for_each_channel()이 완료된 시점의 SPDK 스레드
 *
 * 호출 체인:
 *   spdk_for_each_channel() 완료 → bdev_each_channel_cpl()
 *                                → [이 함수] → ctx->cb()
 */
static void
bdev_for_each_io_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_for_each_io_ctx *ctx = _ctx; /* [한국어] 순회 컨텍스트 복원 */

	ctx->cb(ctx->ctx, status); /* [한국어] 사용자 완료 콜백 호출 — status 전달 */

	free(ctx); /* [한국어] 순회 컨텍스트 동적 해제 */
}

/*
 * [한국어]
 * spdk_bdev_for_each_bdev_io - bdev의 모든 채널에서 진행 중인 bdev_io 순회
 *
 * @bdev:  순회 대상 bdev
 * @_ctx:  사용자 컨텍스트 포인터 (fn/cb 호출 시 전달)
 * @fn:    각 bdev_io에 대해 호출되는 순회 콜백 (NULL 불가)
 * @cb:    모든 채널 순회 완료 후 호출되는 완료 콜백 (NULL 불가)
 *
 * 이 bdev를 사용하는 모든 채널의 io_submitted TAILQ를 순회하며
 * 각 bdev_io에 대해 fn()을 호출한다.
 * 모든 채널 순회가 완료되거나 fn()이 오류를 반환하면 cb()가 호출된다.
 *
 * 내부적으로 spdk_bdev_for_each_io_ctx를 할당하여 fn/cb/ctx를 보관하고,
 * spdk_bdev_for_each_channel()을 통해 채널별 순회를 수행한다.
 * 순회 컨텍스트는 bdev_for_each_io_done()에서 해제된다.
 *
 * 사용 사례: QoS 통계 수집, I/O 타임아웃 감지, 디버그 덤프 등
 *
 * 실행 컨텍스트: SPDK 스레드 (비동기 완료, cb는 동일 또는 다른 스레드에서 호출 가능)
 *
 * 호출 체인:
 *   애플리케이션/bdev 관리 코드 → [이 함수]
 *     → spdk_bdev_for_each_channel()
 *       → bdev_channel_for_each_io() [채널별]
 *         → fn()
 *     → bdev_for_each_io_done() → cb()
 */
void
spdk_bdev_for_each_bdev_io(struct spdk_bdev *bdev, void *_ctx, spdk_bdev_io_fn fn,
			   spdk_bdev_for_each_io_cb cb)
{
	struct spdk_bdev_for_each_io_ctx *ctx; /* [한국어] 순회 상태를 보관할 내부 컨텍스트 */

	assert(fn != NULL && cb != NULL); /* [한국어] 순회 콜백과 완료 콜백은 필수 — NULL이면 버그 */

	ctx = calloc(1, sizeof(*ctx)); /* [한국어] 순회 컨텍스트 동적 할당 (0으로 초기화) */
	if (ctx == NULL) {
		/* [한국어] 메모리 할당 실패 → 즉시 완료 콜백에 오류 전달 */
		SPDK_ERRLOG("Failed to allocate context.\n");
		cb(_ctx, -ENOMEM); /* [한국어] 비동기 경로 없이 즉시 오류 콜백 호출 */
		return;
	}

	ctx->ctx = _ctx; /* [한국어] 사용자 컨텍스트 저장 */
	ctx->fn = fn;    /* [한국어] 순회 콜백 저장 */
	ctx->cb = cb;    /* [한국어] 완료 콜백 저장 */

	spdk_bdev_for_each_channel(bdev, bdev_channel_for_each_io, ctx,
				   bdev_for_each_io_done);
	/* [한국어] 모든 채널에 bdev_channel_for_each_io 메시지 전송 시작
	 * 완료 시 bdev_for_each_io_done()에서 ctx 해제 및 cb 호출 */
}

/*
 * [한국어]
 * spdk_bdev_for_each_channel_continue - 채널 순회 이터레이터 계속 진행
 *
 * @iter:   현재 채널 이터레이터
 * @status: 현재 채널 처리 결과 (0=계속, 비0=오류로 인한 중단)
 *
 * bdev_channel_for_each_io() 또는 사용자 채널 순회 콜백에서 현재 채널 처리를
 * 완료했음을 spdk_for_each_channel 프레임워크에 알리는 함수.
 * status=0이면 다음 채널로 진행하고, 비0이면 나머지 채널을 건너뛰고
 * 완료 콜백(cpl)을 즉시 호출한다.
 *
 * SPDK의 spdk_for_each_channel_continue()를 bdev 계층 래퍼로 노출한다.
 *
 * 실행 컨텍스트: 현재 채널의 SPDK 스레드
 *
 * 호출 체인:
 *   bdev_channel_for_each_io() → [이 함수] → spdk_for_each_channel_continue()
 */
void
spdk_bdev_for_each_channel_continue(struct spdk_bdev_channel_iter *iter, int status)
{
	spdk_for_each_channel_continue(iter->i, status);
	/* [한국어] SPDK 채널 프레임워크에 현재 채널 완료 신호 전달
	 * iter->i: 내부 spdk_io_channel_iter 포인터 */
}

/*
 * [한국어]
 * io_channel_iter_get_bdev - 채널 이터레이터에서 bdev 포인터 추출
 *
 * @i:      SPDK 채널 이터레이터
 * @return: 이터레이터와 연관된 spdk_bdev 포인터
 *
 * spdk_for_each_channel() 순회 중에 io_device(bdev의 io_device 등록 핸들)로부터
 * 실제 spdk_bdev 포인터를 역산한다.
 * __bdev_from_io_dev()는 io_dev 포인터 오프셋 역산으로 bdev를 반환한다.
 *
 * 실행 컨텍스트: SPDK 채널 순회 콜백 내부
 *
 * 호출 체인:
 *   bdev_each_channel_msg() / bdev_each_channel_cpl() → [이 함수]
 */
static struct spdk_bdev *
io_channel_iter_get_bdev(struct spdk_io_channel_iter *i)
{
	void *io_device = spdk_io_channel_iter_get_io_device(i);
	/* [한국어] 이터레이터에서 io_device 포인터 추출 (spdk_bdev_register 시 등록한 디바이스 ID) */

	return __bdev_from_io_dev(io_device);
	/* [한국어] io_device 포인터에서 spdk_bdev로 역산 (컨테이너_of 패턴) */
}

/*
 * [한국어]
 * bdev_each_channel_msg - 각 채널에서 실행되는 bdev 채널 순회 메시지 핸들러
 *
 * @i: SPDK 채널 이터레이터 (현재 채널 정보 포함)
 *
 * spdk_for_each_channel()이 각 채널의 스레드에 메시지를 보낼 때 호출되는 핸들러.
 * 내부 spdk_io_channel_iter를 bdev_channel_iter로 래핑하여
 * 사용자 fn(iter, bdev, ch, ctx)를 호출한다.
 * iter->i를 현재 이터레이터로 갱신해야 fn() 내부에서
 * spdk_bdev_for_each_channel_continue()가 올바르게 동작한다.
 *
 * 실행 컨텍스트: 해당 채널을 소유한 SPDK 스레드 (reactor)
 *
 * 호출 체인:
 *   spdk_for_each_channel() [프레임워크] → [이 함수] → iter->fn()
 *                                                     → spdk_bdev_for_each_channel_continue()
 */
static void
bdev_each_channel_msg(struct spdk_io_channel_iter *i)
{
	struct spdk_bdev_channel_iter *iter = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] 이터레이터 컨텍스트에서 bdev_channel_iter 복원 */
	struct spdk_bdev *bdev = io_channel_iter_get_bdev(i);
	/* [한국어] 이터레이터에서 현재 순회 중인 bdev 추출 */
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	/* [한국어] 현재 처리할 io_channel 추출 */

	iter->i = i; /* [한국어] iter->i 갱신 — 이후 for_each_channel_continue()에서 사용 */
	iter->fn(iter, bdev, ch, iter->ctx);
	/* [한국어] 사용자 순회 콜백 호출 (반드시 spdk_bdev_for_each_channel_continue()를 호출해야 함) */
}

/*
 * [한국어]
 * bdev_each_channel_cpl - bdev 채널 순회 전체 완료 콜백
 *
 * @i:      SPDK 채널 이터레이터 (모든 채널 순회 완료 상태)
 * @status: 순회 최종 상태 (0=모든 채널 성공, 비0=중간 채널 오류)
 *
 * spdk_for_each_channel()이 모든 채널 처리를 마친 후 호출하는 완료 핸들러.
 * 사용자 완료 콜백(iter->cpl)을 bdev, ctx, status와 함께 호출하고
 * 이터레이터 메모리를 해제한다.
 *
 * iter는 이 함수에서 free()되므로 cpl() 호출 후 iter에 접근해서는 안 된다.
 *
 * 실행 컨텍스트: 마지막 채널 처리가 완료된 SPDK 스레드
 *
 * 호출 체인:
 *   spdk_for_each_channel() [프레임워크, 모든 채널 완료 후] → [이 함수] → iter->cpl()
 */
static void
bdev_each_channel_cpl(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_bdev_channel_iter *iter = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] 이터레이터 컨텍스트에서 bdev_channel_iter 복원 */
	struct spdk_bdev *bdev = io_channel_iter_get_bdev(i);
	/* [한국어] 이터레이터에서 bdev 추출 */

	iter->i = i; /* [한국어] iter->i 최종 갱신 (cpl() 내에서 iter에 접근할 경우 대비) */
	iter->cpl(bdev, iter->ctx, status); /* [한국어] 사용자 완료 콜백 호출 — 최종 status 전달 */

	free(iter); /* [한국어] 채널 이터레이터 메모리 해제 (spdk_bdev_for_each_channel에서 할당됨) */
}

/*
 * [한국어]
 * spdk_bdev_for_each_channel - bdev의 모든 I/O 채널에 대해 콜백 실행
 *
 * @bdev:  순회 대상 bdev
 * @fn:    각 채널에서 호출되는 메시지 콜백 (NULL 불가)
 * @ctx:   fn/cpl에 전달할 사용자 컨텍스트 (NULL 불가)
 * @cpl:   모든 채널 처리 완료 후 호출되는 완료 콜백
 *
 * bdev의 io_device에 등록된 모든 채널을 순차적으로 순회하며
 * 각 채널의 스레드에서 fn()을 실행한다.
 * 모든 채널이 처리되면(또는 fn()이 오류를 반환하면) cpl()이 호출된다.
 *
 * 내부적으로 spdk_bdev_channel_iter를 할당하여 fn/cpl/ctx를 보관하고
 * SPDK의 spdk_for_each_channel() 프레임워크를 활용한다.
 * 이터레이터는 bdev_each_channel_cpl()에서 해제된다.
 *
 * 주요 사용 사례:
 *   - QoS 활성화/비활성화 (채널별 메시지)
 *   - 히스토그램 수집/병합
 *   - LBA 범위 잠금 채널 등록
 *   - 채널별 I/O 목록 수집
 *
 * 실행 컨텍스트: SPDK 스레드 (비동기, cpl은 나중에 호출될 수 있음)
 *
 * 호출 체인:
 *   QoS/히스토그램/LBA락/for_each_io 등 → [이 함수]
 *     → spdk_for_each_channel()
 *       → bdev_each_channel_msg() [채널별 스레드]
 *         → fn()
 *     → bdev_each_channel_cpl() → cpl()
 */
void
spdk_bdev_for_each_channel(struct spdk_bdev *bdev, spdk_bdev_for_each_channel_msg fn,
			   void *ctx, spdk_bdev_for_each_channel_done cpl)
{
	struct spdk_bdev_channel_iter *iter; /* [한국어] 채널 순회 상태를 보관할 이터레이터 */

	assert(bdev != NULL && fn != NULL && ctx != NULL);
	/* [한국어] 필수 파라미터 NULL 검사 — 위반 시 버그 */

	iter = calloc(1, sizeof(struct spdk_bdev_channel_iter));
	/* [한국어] 채널 이터레이터 동적 할당 (0으로 초기화) */
	if (iter == NULL) {
		/* [한국어] 메모리 할당 실패 — 치명적 오류이므로 assert */
		SPDK_ERRLOG("Unable to allocate iterator\n");
		assert(false); /* [한국어] OOM은 치명적 버그로 처리 */
		return;
	}

	iter->fn = fn;   /* [한국어] 채널별 메시지 콜백 저장 */
	iter->cpl = cpl; /* [한국어] 완료 콜백 저장 */
	iter->ctx = ctx; /* [한국어] 사용자 컨텍스트 저장 */

	spdk_for_each_channel(__bdev_to_io_dev(bdev), bdev_each_channel_msg,
			      iter, bdev_each_channel_cpl);
	/* [한국어] SPDK 채널 프레임워크에 순회 요청
	 * __bdev_to_io_dev(bdev): bdev를 io_device 핸들로 변환
	 * bdev_each_channel_msg: 각 채널 스레드에 전달될 핸들러
	 * iter: 각 핸들러에 전달될 컨텍스트
	 * bdev_each_channel_cpl: 모든 채널 완료 후 호출될 핸들러 */
}

/*
 * [한국어]
 * bdev_copy_do_write_done - COPY 에뮬레이션 쓰기 완료 콜백
 *
 * @bdev_io: 쓰기 단계에서 생성된 임시 bdev_io (완료 후 해제 대상)
 * @success: 쓰기 성공 여부
 * @cb_arg:  원본 COPY bdev_io (parent_io)
 *
 * COPY 에뮬레이션의 최종 단계 콜백.
 * bdev_copy_do_read → bdev_copy_do_write → [이 함수] 체인의 마지막 단계.
 * 임시 쓰기 bdev_io를 해제하고 원본 COPY bdev_io의 상태를 설정한 뒤
 * 원래 호출자의 완료 콜백을 호출한다.
 *
 * 실행 컨텍스트: 쓰기를 처리한 bdev 채널의 SPDK 스레드
 *
 * 호출 체인:
 *   spdk_bdev_write_blocks_with_md() 완료 → [이 함수] → parent_io->internal.cb()
 */
static void
bdev_copy_do_write_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;
	/* [한국어] cb_arg로 전달된 원본 COPY bdev_io 복원 */

	spdk_bdev_free_io(bdev_io);
	/* [한국어] 쓰기 단계 임시 bdev_io 해제 — mempool에 반환 */

	/* Check return status of write */
	parent_io->internal.status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	/* [한국어] 쓰기 성공/실패에 따라 원본 COPY bdev_io 최종 상태 설정 */
	parent_io->internal.cb(parent_io, success, parent_io->internal.caller_ctx);
	/* [한국어] 원본 COPY 요청 호출자의 완료 콜백 호출 — COPY 에뮬레이션 완전히 종료 */
}

/*
 * [한국어]
 * bdev_copy_do_write - COPY 에뮬레이션 쓰기 단계 실행
 *
 * @_bdev_io: 원본 COPY bdev_io (읽기 버퍼가 채워진 상태)
 *
 * COPY 에뮬레이션의 2단계: 읽기로 채운 버퍼를 목적지 오프셋에 쓴다.
 * bdev_io->u.bdev.iovs[0].iov_base: bdev_io_get_buf로 할당된 읽기 데이터 버퍼
 * bdev_io->u.bdev.offset_blocks: 쓰기 목적지 (dst_offset_blocks)
 * bdev_io->u.bdev.num_blocks: 복사할 블록 수
 *
 * -ENOMEM 반환 시 bdev_io_wait 큐에 등록하여 자원 해제 후 재시도한다.
 * 기타 오류는 즉시 실패 처리한다.
 *
 * 실행 컨텍스트: SPDK 스레드 (bdev_copy_do_read_done() 또는 io_wait 재시도)
 *
 * 호출 체인:
 *   bdev_copy_do_read_done() → [이 함수] → spdk_bdev_write_blocks_with_md()
 *                                         → bdev_copy_do_write_done()
 *   또는 io_wait 재시도 → [이 함수]
 */
static void
bdev_copy_do_write(void *_bdev_io)
{
	struct spdk_bdev_io *bdev_io = _bdev_io; /* [한국어] 원본 COPY bdev_io 복원 */
	int rc; /* [한국어] 쓰기 제출 반환값 */

	/* Write blocks */
	rc = spdk_bdev_write_blocks_with_md(bdev_io->internal.desc,
					    spdk_io_channel_from_ctx(bdev_io->internal.ch),
					    bdev_io->u.bdev.iovs[0].iov_base,
					    /* [한국어] 읽기 단계에서 채운 버퍼 (get_buf로 할당) */
					    bdev_io->u.bdev.md_buf,
					    /* [한국어] 메타데이터 버퍼 (md 미지원 bdev면 NULL) */
					    bdev_io->u.bdev.offset_blocks,
					    /* [한국어] 쓰기 목적지 오프셋 (dst_offset_blocks) */
					    bdev_io->u.bdev.num_blocks,
					    bdev_copy_do_write_done, bdev_io);
	/* [한국어] 목적지 오프셋에 읽은 데이터 쓰기 — 완료 시 bdev_copy_do_write_done() 호출 */

	if (rc == -ENOMEM) {
		/* [한국어] bdev_io 풀 소진 → io_wait 큐에 등록하여 자원 해제 후 재시도 */
		bdev_queue_io_wait_with_cb(bdev_io, bdev_copy_do_write);
	} else if (rc != 0) {
		/* [한국어] 기타 쓰기 오류 → 즉시 실패 처리 */
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
	}
}

/*
 * [한국어]
 * bdev_copy_do_read_done - COPY 에뮬레이션 읽기 완료 콜백
 *
 * @bdev_io: 읽기 단계에서 생성된 임시 bdev_io (완료 후 해제 대상)
 * @success: 읽기 성공 여부
 * @cb_arg:  원본 COPY bdev_io (parent_io)
 *
 * COPY 에뮬레이션의 중간 단계 콜백.
 * 읽기 임시 bdev_io를 해제하고, 읽기가 성공했으면 쓰기 단계(bdev_copy_do_write)로 진행.
 * 읽기 실패 시 원본 COPY 요청 실패 처리 후 콜백을 즉시 호출한다.
 *
 * 실행 컨텍스트: 읽기를 처리한 bdev 채널의 SPDK 스레드
 *
 * 호출 체인:
 *   spdk_bdev_read_blocks_with_md() 완료 → [이 함수]
 *     → 실패: parent_io->internal.cb()
 *     → 성공: bdev_copy_do_write()
 */
static void
bdev_copy_do_read_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;
	/* [한국어] cb_arg로 전달된 원본 COPY bdev_io 복원 */

	spdk_bdev_free_io(bdev_io);
	/* [한국어] 읽기 단계 임시 bdev_io 해제 — mempool에 반환 */

	/* Check return status of read */
	if (!success) {
		/* [한국어] 읽기 실패 → 원본 COPY bdev_io 실패 처리 및 즉시 콜백 */
		parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		parent_io->internal.cb(parent_io, false, parent_io->internal.caller_ctx);
		return;
	}

	/* Do write */
	bdev_copy_do_write(parent_io);
	/* [한국어] 읽기 성공 → 버퍼에 데이터가 채워졌으므로 쓰기 단계 진행 */
}

/*
 * [한국어]
 * bdev_copy_do_read - COPY 에뮬레이션 읽기 단계 실행
 *
 * @_bdev_io: 원본 COPY bdev_io (get_buf로 버퍼 할당 완료 상태)
 *
 * COPY 에뮬레이션의 1단계: 소스 오프셋에서 데이터를 읽어 버퍼에 채운다.
 * bdev_io->u.bdev.iovs[0].iov_base: bdev_io_get_buf로 할당된 읽기 버퍼
 * bdev_io->u.bdev.copy.src_offset_blocks: 읽기 소스 오프셋
 * bdev_io->u.bdev.num_blocks: 복사할 블록 수
 *
 * -ENOMEM 반환 시 io_wait 큐에 등록하여 재시도한다.
 * 읽기 완료 후 bdev_copy_do_read_done()을 통해 쓰기 단계로 진행한다.
 *
 * 실행 컨텍스트: SPDK 스레드 (bdev_copy_get_buf_cb() 또는 io_wait 재시도)
 *
 * 호출 체인:
 *   bdev_copy_get_buf_cb() → [이 함수] → spdk_bdev_read_blocks_with_md()
 *                                       → bdev_copy_do_read_done()
 *   또는 io_wait 재시도 → [이 함수]
 */
static void
bdev_copy_do_read(void *_bdev_io)
{
	struct spdk_bdev_io *bdev_io = _bdev_io; /* [한국어] 원본 COPY bdev_io 복원 */
	int rc; /* [한국어] 읽기 제출 반환값 */

	/* Read blocks */
	rc = spdk_bdev_read_blocks_with_md(bdev_io->internal.desc,
					   spdk_io_channel_from_ctx(bdev_io->internal.ch),
					   bdev_io->u.bdev.iovs[0].iov_base,
					   /* [한국어] get_buf로 할당된 읽기 버퍼 — 쓰기 단계에서 재사용 */
					   bdev_io->u.bdev.md_buf,
					   /* [한국어] 메타데이터 버퍼 (md 지원 bdev면 채워짐) */
					   bdev_io->u.bdev.copy.src_offset_blocks,
					   /* [한국어] 읽기 소스 오프셋 (spdk_bdev_copy_blocks의 src_offset_blocks) */
					   bdev_io->u.bdev.num_blocks,
					   bdev_copy_do_read_done, bdev_io);
	/* [한국어] 소스 오프셋에서 num_blocks 블록 읽기 — 완료 시 bdev_copy_do_read_done() 호출 */

	if (rc == -ENOMEM) {
		/* [한국어] bdev_io 풀 소진 → io_wait 큐에 등록하여 재시도 */
		bdev_queue_io_wait_with_cb(bdev_io, bdev_copy_do_read);
	} else if (rc != 0) {
		/* [한국어] 기타 읽기 오류 → 즉시 실패 처리 */
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
	}
}

/*
 * [한국어]
 * bdev_copy_get_buf_cb - COPY 에뮬레이션용 I/O 버퍼 할당 완료 콜백
 *
 * @ch:      I/O 채널 (사용하지 않음)
 * @bdev_io: 원본 COPY bdev_io (버퍼 할당 완료 상태)
 * @success: 버퍼 할당 성공 여부
 *
 * spdk_bdev_io_get_buf()로 요청한 DMA 버퍼가 준비됐을 때 호출되는 콜백.
 * 버퍼 할당 실패 시 즉시 COPY 실패 처리하고,
 * 성공 시 에뮬레이션 첫 단계인 bdev_copy_do_read()를 시작한다.
 *
 * 실행 컨텍스트: SPDK 스레드 (버퍼 준비 시 해당 채널 스레드에서 호출)
 *
 * 호출 체인:
 *   spdk_bdev_io_get_buf() 완료 → [이 함수] → bdev_copy_do_read()
 *   또는 버퍼 할당 실패 → [이 함수] → bdev_io->internal.cb()
 */
static void
bdev_copy_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	if (!success) {
		/* [한국어] DMA 버퍼 할당 실패 → COPY 요청 즉시 실패 처리 */
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
		return;
	}

	bdev_copy_do_read(bdev_io);
	/* [한국어] 버퍼 준비 완료 → 에뮬레이션 첫 단계(읽기) 시작 */
}

/*
 * [한국어]
 * spdk_bdev_copy_blocks - bdev 블록 복사 요청 제출 (COPY 명령 또는 에뮬레이션)
 *
 * @desc:               쓰기 가능한 bdev 디스크립터
 * @ch:                 I/O 채널
 * @dst_offset_blocks:  복사 목적지 시작 LBA
 * @src_offset_blocks:  복사 소스 시작 LBA
 * @num_blocks:         복사할 블록 수
 * @cb:                 완료 콜백
 * @cb_arg:             완료 콜백 인수
 * @return:             0=성공(비동기 진행 중), -EBADF=읽기 전용 디스크립터,
 *                      -EINVAL=범위 초과, -ENOMEM=bdev_io 풀 소진
 *
 * SPDK_BDEV_IO_TYPE_COPY 명령을 지원하는 bdev(예: NVMe Simple Copy)는
 * 하드웨어 COPY 명령을 직접 사용하고, 미지원 bdev는 읽기→쓰기 에뮬레이션으로 처리한다.
 * 에뮬레이션 경로: get_buf(DMA 버퍼 할당) → read(소스) → write(목적지).
 *
 * dst==src 또는 num_blocks==0이면 즉시 성공(no-op) 처리한다.
 * 대용량이라 분할(split)이 필요한 경우, COPY 미지원이라도 분할 로직 먼저 적용한다.
 *
 * 실행 컨텍스트: SPDK 스레드 (동기 제출, 완료는 비동기)
 *
 * 호출 체인:
 *   애플리케이션/상위 bdev 모듈 → [이 함수]
 *     → COPY 지원 또는 split: bdev_io_submit()
 *     → COPY 미지원 에뮬레이션: spdk_bdev_io_get_buf()
 *       → bdev_copy_get_buf_cb() → bdev_copy_do_read()
 *         → bdev_copy_do_read_done() → bdev_copy_do_write()
 *           → bdev_copy_do_write_done() → cb()
 */
int
spdk_bdev_copy_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      uint64_t dst_offset_blocks, uint64_t src_offset_blocks, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc); /* [한국어] 디스크립터에서 bdev 추출 */
	struct spdk_bdev_io *bdev_io; /* [한국어] mempool에서 할당할 COPY bdev_io */
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	/* [한국어] io_channel에서 bdev_channel 컨텍스트 추출 */

	if (!desc->write) {
		/* [한국어] 읽기 전용 디스크립터로 쓰기 요청 — 허용 불가 */
		return -EBADF;
	}

	if (!bdev_io_valid_blocks(bdev, dst_offset_blocks, num_blocks) ||
	    !bdev_io_valid_blocks(bdev, src_offset_blocks, num_blocks)) {
		/* [한국어] 목적지 또는 소스 블록 범위가 bdev 경계를 초과 */
		SPDK_DEBUGLOG(bdev,
			      "Invalid offset or number of blocks: dst %lu, src %lu, count %lu\n",
			      dst_offset_blocks, src_offset_blocks, num_blocks);
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel); /* [한국어] 채널 mempool에서 bdev_io 할당 */
	if (!bdev_io) {
		/* [한국어] bdev_io 풀 소진 → 호출자는 io_wait 등록 후 재시도 필요 */
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;     /* [한국어] I/O를 처리할 채널 설정 */
	bdev_io->internal.desc = desc;      /* [한국어] 요청한 디스크립터 저장 */
	bdev_io->type = SPDK_BDEV_IO_TYPE_COPY; /* [한국어] I/O 타입 = COPY */

	bdev_io->u.bdev.offset_blocks = dst_offset_blocks;
	/* [한국어] 목적지 오프셋 저장 (쓰기 단계에서 사용) */
	bdev_io->u.bdev.copy.src_offset_blocks = src_offset_blocks;
	/* [한국어] 소스 오프셋 저장 (읽기 단계에서 사용) */
	bdev_io->u.bdev.num_blocks = num_blocks; /* [한국어] 복사할 블록 수 */
	bdev_io->u.bdev.memory_domain = NULL;     /* [한국어] 메모리 도메인 없음 (COPY 에뮬레이션은 표준 DMA) */
	bdev_io->u.bdev.memory_domain_ctx = NULL; /* [한국어] 메모리 도메인 컨텍스트 없음 */
	bdev_io->u.bdev.iovs = NULL;              /* [한국어] iovs는 get_buf 이후에 설정됨 */
	bdev_io->u.bdev.iovcnt = 0;              /* [한국어] iov 개수 초기화 */
	bdev_io->u.bdev.md_buf = NULL;           /* [한국어] 메타데이터 버퍼 초기화 */
	bdev_io->u.bdev.accel_sequence = NULL;   /* [한국어] 가속기 시퀀스 없음 */
	bdev_io_init(bdev_io, bdev, cb_arg, cb); /* [한국어] bdev_io 공통 필드 초기화 */

	if (dst_offset_blocks == src_offset_blocks || num_blocks == 0) {
		/* [한국어] 소스=목적지이거나 블록 수=0 → 복사 불필요, 즉시 성공 완료 */
		spdk_thread_send_msg(spdk_get_thread(), bdev_io_complete_cb, bdev_io);
		/* [한국어] 동일 스레드에 메시지 전송으로 비동기 완료 콜백 처리 */
		return 0;
	}


	/* If the copy size is large and should be split, use the generic split logic
	 * regardless of whether SPDK_BDEV_IO_TYPE_COPY is supported or not.
	 *
	 * Then, send the copy request if SPDK_BDEV_IO_TYPE_COPY is supported or
	 * emulate it using regular read and write requests otherwise.
	 */
	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COPY) ||
	    bdev_io->internal.f.split) {
		/* [한국어] COPY 하드웨어 지원 bdev: 직접 제출
		 * 또는 split 플래그: 분할 로직 먼저 적용 (COPY 지원 여부와 무관) */
		bdev_io_submit(bdev_io); /* [한국어] 모듈에 직접 COPY 명령 제출 또는 split 처리 */
		return 0;
	}

	spdk_bdev_io_get_buf(bdev_io, bdev_copy_get_buf_cb, num_blocks * spdk_bdev_get_block_size(bdev));
	/* [한국어] COPY 미지원 bdev: DMA 버퍼 할당 요청
	 * 버퍼 크기 = num_blocks × block_size (데이터 버퍼; md 버퍼는 별도)
	 * 준비되면 bdev_copy_get_buf_cb() → 읽기→쓰기 에뮬레이션 체인 시작 */

	return 0; /* [한국어] 비동기 에뮬레이션 시작됨 — 완료는 cb()로 통보 */
}

/* [한국어] SPDK 로그 컴포넌트 등록 매크로 — "bdev" 컴포넌트 ID를 생성한다.
 * 이 매크로는 SPDK_DEBUGLOG(bdev, ...) / SPDK_ERRLOG 등에서 사용되는
 * 로그 컴포넌트 식별자를 전역 구조체로 등록한다.
 * 런타임에 spdk_log_set_flag("bdev")로 디버그 로그를 활성화할 수 있다. */
SPDK_LOG_REGISTER_COMPONENT(bdev)

/*
 * [한국어]
 * bdev_trace - bdev 레이어 trace point(추적 지점) 등록
 *
 * SPDK trace 프레임워크에 bdev 레이어의 추적 지점 4개와 관련 오너/오브젝트를 등록한다.
 *
 * 등록하는 trace point:
 *   - BDEV_IO_START: bdev_io 제출 시점 — type(I/O 종류), ctx(콜백 컨텍스트),
 *                    offset(시작 LBA), qd(채널 큐 깊이) 인수 포함
 *   - BDEV_IO_DONE:  bdev_io 완료 시점 — ctx, qd 인수 포함
 *   - BDEV_IOCH_CREATE: bdev I/O 채널 생성 시점 — tid(스레드 ID)
 *   - BDEV_IOCH_DESTROY: bdev I/O 채널 소멸 시점 — tid(스레드 ID)
 *
 * 오너(OWNER_TYPE_BDEV, 'b'): trace 분석 시 bdev 오너를 식별하는 단축 문자
 * 오브젝트(OBJECT_BDEV_IO, 'i'): bdev_io를 trace 객체로 등록
 *
 * 또한 NVMe, BlobStore, RAID 등 하위 레이어의 trace point를
 * OBJECT_BDEV_IO와 연관 지어 end-to-end I/O 추적을 가능하게 한다.
 *
 * spdk_trace_record()가 hot path에서 호출되어 공유 메모리 링 버퍼에
 * trace 이벤트를 기록하며, spdk_trace 분석 도구로 후처리할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 시작 시 SPDK_TRACE_REGISTER_FN 매크로에 의해 자동 호출
 *
 * 호출 체인:
 *   [SPDK_TRACE_REGISTER_FN 매크로, 초기화 단계] → [이 함수]
 *     → spdk_trace_register_owner_type()
 *     → spdk_trace_register_object()
 *     → spdk_trace_register_description_ext()
 *     → spdk_trace_tpoint_register_relation() [하위 레이어 연결]
 */
static void
bdev_trace(void)
{
	/* [한국어] trace point 설정 배열 — 각 원소가 하나의 trace 이벤트를 정의함 */
	struct spdk_trace_tpoint_opts opts[] = {
		{
			/* [한국어] bdev_io 제출 시점 trace point */
			"BDEV_IO_START", TRACE_BDEV_IO_START,
			OWNER_TYPE_BDEV, OBJECT_BDEV_IO, 1,
			/* [한국어] 1=새 객체 생성(I/O 시작) */
			{
				{ "type",   SPDK_TRACE_ARG_TYPE_INT, 8 }, /* [한국어] I/O 타입 (SPDK_BDEV_IO_TYPE_*) */
				{ "ctx",    SPDK_TRACE_ARG_TYPE_PTR, 8 }, /* [한국어] 호출자 콜백 컨텍스트 포인터 */
				{ "offset", SPDK_TRACE_ARG_TYPE_INT, 8 }, /* [한국어] 시작 LBA 오프셋 */
				{ "qd",     SPDK_TRACE_ARG_TYPE_INT, 4 }  /* [한국어] 채널 큐 깊이 (queue depth) */
			}
		},
		{
			/* [한국어] bdev_io 완료 시점 trace point */
			"BDEV_IO_DONE", TRACE_BDEV_IO_DONE,
			OWNER_TYPE_BDEV, OBJECT_BDEV_IO, 0,
			/* [한국어] 0=기존 객체에 이벤트 기록(I/O 완료) */
			{
				{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 }, /* [한국어] 호출자 콜백 컨텍스트 포인터 */
				{ "qd",  SPDK_TRACE_ARG_TYPE_INT, 4 }  /* [한국어] 완료 후 채널 큐 깊이 */
			}
		},
		{
			/* [한국어] bdev I/O 채널 생성 trace point */
			"BDEV_IOCH_CREATE", TRACE_BDEV_IOCH_CREATE,
			OWNER_TYPE_BDEV, OBJECT_NONE, 0,
			/* [한국어] OBJECT_NONE: 채널은 bdev_io 객체와 무관한 이벤트 */
			{
				{ "tid", SPDK_TRACE_ARG_TYPE_INT, 8 } /* [한국어] 채널을 생성한 스레드 ID */
			}
		},
		{
			/* [한국어] bdev I/O 채널 소멸 trace point */
			"BDEV_IOCH_DESTROY", TRACE_BDEV_IOCH_DESTROY,
			OWNER_TYPE_BDEV, OBJECT_NONE, 0,
			{
				{ "tid", SPDK_TRACE_ARG_TYPE_INT, 8 } /* [한국어] 채널을 소멸한 스레드 ID */
			}
		},
	};


	spdk_trace_register_owner_type(OWNER_TYPE_BDEV, 'b');
	/* [한국어] bdev 오너 타입을 단축 문자 'b'로 등록 — trace 출력 시 bdev 식별자로 사용 */
	spdk_trace_register_object(OBJECT_BDEV_IO, 'i');
	/* [한국어] bdev_io를 trace 객체로 등록 — 'i' 단축 문자로 식별 */
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
	/* [한국어] opts 배열의 모든 trace point 설명 등록 */
	spdk_trace_tpoint_register_relation(TRACE_BDEV_NVME_IO_START, OBJECT_BDEV_IO, 0);
	/* [한국어] NVMe I/O 시작을 OBJECT_BDEV_IO에 연관 → end-to-end bdev→NVMe 추적 가능 */
	spdk_trace_tpoint_register_relation(TRACE_BDEV_NVME_IO_DONE, OBJECT_BDEV_IO, 0);
	/* [한국어] NVMe I/O 완료를 OBJECT_BDEV_IO에 연관 */
	spdk_trace_tpoint_register_relation(TRACE_BLOB_REQ_SET_START, OBJECT_BDEV_IO, 0);
	/* [한국어] BlobStore 요청 시작을 OBJECT_BDEV_IO에 연관 → blob bdev 추적 가능 */
	spdk_trace_tpoint_register_relation(TRACE_BLOB_REQ_SET_COMPLETE, OBJECT_BDEV_IO, 0);
	/* [한국어] BlobStore 요청 완료를 OBJECT_BDEV_IO에 연관 */
	spdk_trace_tpoint_register_relation(TRACE_BDEV_RAID_IO_START, OBJECT_BDEV_IO, 0);
	/* [한국어] RAID bdev I/O 시작을 OBJECT_BDEV_IO에 연관 → RAID 내부 추적 가능 */
	spdk_trace_tpoint_register_relation(TRACE_BDEV_RAID_IO_DONE, OBJECT_BDEV_IO, 0);
	/* [한국어] RAID bdev I/O 완료를 OBJECT_BDEV_IO에 연관 */
}
/* [한국어] SPDK trace 등록 매크로: 프로세스 초기화 시 bdev_trace()를 자동 실행한다.
 * "bdev" 이름으로 TRACE_GROUP_BDEV 그룹에 등록.
 * 이 매크로는 __attribute__((constructor))와 유사한 메커니즘으로 동작하며,
 * bdev 레이어의 모든 trace 기능을 프로세스 시작 시 활성화한다. */
SPDK_TRACE_REGISTER_FN(bdev_trace, "bdev", TRACE_GROUP_BDEV)
