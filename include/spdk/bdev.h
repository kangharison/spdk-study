/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Block device abstraction layer
 */

/*
 * [한국어 설명] bdev 공개 사용자 API (bdev.h) — 4194 라인 (전 공개 API 주석 완료)
 *
 * === 파일의 역할 ===
 * SPDK의 "블록 디바이스 추상화 레이어(bdev)" **사용자/애플리케이션 API**.
 * NVMe, AIO, malloc, raid, lvol 등 어떤 백엔드든 동일한 함수 시그니처로
 * read/write/unmap/flush/reset/zone I/O를 발행할 수 있도록 추상화한다.
 * 이 헤더가 노출하는 API는 두 축으로 나뉜다:
 *   (1) 관리(관리자/RPC 핸들러): 초기화·열기/닫기·등록 조회·QoS·히스토그램
 *   (2) I/O 데이터 경로(애플리케이션·NVMe-oF 타겟·iSCSI 타겟): read/write/
 *       unmap/flush/reset/compare/compare_and_write/zone_* /copy/passthru
 *
 * bdev 모듈 작성자 API는 별도 파일 `spdk/bdev_module.h`에 있다. 이 파일은
 * "bdev을 쓰는 쪽" 전용.
 *
 * 주요 내용:
 *   - 이벤트 타입·콜백: REMOVE/RESIZE/MEDIA_MANAGEMENT
 *   - enum spdk_bdev_io_type (23종): READ/WRITE/UNMAP/FLUSH/RESET/COMPARE/SEEK_DATA/HOLE/COPY/...
 *   - enum spdk_bdev_qos_rate_limit_type: RW IOPS/BPS, R BPS, W BPS (4종)
 *   - struct spdk_bdev_opts: 전역 옵션 (pool_size, cache_size, iobuf 캐시)
 *   - struct spdk_bdev_io_stat: per-bdev 통계 (RW/UNMAP/COPY 카운터·지연·에러)
 *   - struct spdk_bdev_enable_histogram_opts: 히스토그램 세밀 옵션 (granularity, min/max_nsec, io_type 필터)
 *   - struct spdk_bdev_ext_io_opts: _ext API 옵션 (memory_domain, accel_sequence, NVMe cdw12·13)
 *   - struct spdk_bdev_open_opts/_async_opts: 열기 옵션 (hide_metadata, timeout_ms)
 *   - 초기화/종료: spdk_bdev_initialize/finish
 *   - 열기/닫기: spdk_bdev_open_ext / _v2 / _async / close
 *   - 속성 질의: get_block_size, get_num_blocks, UUID, NUMA, optimal_io_boundary, dif_*, max_copy
 *   - QoS: get_qos_rpc_type, get/set_qos_rate_limits
 *   - QD 모니터링: get_qd, get/set_qd_sampling_period, get_io_time, get_weighted_io_time
 *   - ★ I/O 제출 API (43+종): read/readv/readv_blocks_with_md/_ext,
 *     write/writev/writev_blocks_with_md/_ext, write_zeroes, write_uncorrectable,
 *     unmap, flush, reset, nvme_nssr, abort, compare/comparev/comparev_and_writev,
 *     zcopy_start/end, copy_blocks, seek_data/seek_hole,
 *     nvme_admin/io/io_md/iov_md_passthru
 *   - 완료 처리: free_io, get_nvme_status/fused/scsi/aio_status, get_iovec/md_buf/cb_arg, get_seek_offset
 *   - I/O 대기 큐: io_wait_entry, queue_io_wait (NOMEM 재시도 패턴)
 *   - 히스토그램: histogram_enable / _ext / _opts_init / _get / channel_get_histogram
 *   - 통계: get_io_stat (단일 채널 동기), get_device_stat (모든 채널 비동기 집계)
 *   - 채널 순회: for_each_channel + continue (sync/async 패턴 모두 지원)
 *   - 메모리 도메인: get_memory_domains (RDMA/GPU 호환성 확인)
 *   - 미디어 이벤트: get_media_events (Open Channel SSD bad block 등)
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 흐름:
 *   [애플리케이션]
 *     → spdk_bdev_open_ext() → spdk_bdev_desc (핸들 획득)
 *     → spdk_bdev_get_io_channel(desc) → spdk_io_channel (스레드 고정)
 *     → spdk_bdev_read_blocks(desc, ch, buf, lba, nblk, cb, ctx) [이 헤더]
 *       → bdev 코어 내부에서 bdev_io 획득·초기화·split 판정
 *       → 모듈 submit_request 콜백 (bdev_module.h fn_table)
 *         → 모듈이 백엔드(NVMe 드라이버 등)에 dispatch
 *       → 완료 시 spdk_bdev_io_complete() → 사용자 cb 호출
 *
 * 실행 컨텍스트: 각 desc/channel은 생성한 SPDK thread에 고정. 사용자는
 * 반드시 같은 스레드에서 I/O를 제출·완료해야 한다(`spdk_thread_send_msg`
 * 로 cross-thread 우회 가능).
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/accel.h           - accel 가속기 sequence 전달 구조
 *   - spdk/scsi_spec.h       - SCSI 에러 변환 시 상태 코드 참조
 *   - spdk/nvme_spec.h       - NVMe passthru 경로의 SQE/CQE
 *   - spdk/json.h            - RPC/구성 JSON 출력
 *   - spdk/queue.h           - 내부 TAILQ 기반 타입
 *   - spdk/histogram_data.h  - 지연시간 히스토그램
 *   - spdk/dif.h             - T10 DIF 기능
 * 의존하는 모듈:
 *   - lib/bdev/bdev.c (구현)
 *   - lib/nvmf/ctrlr_bdev.c (NVMe-oF target → bdev dispatch)
 *   - lib/iscsi, lib/vhost, lib/vhost_blk (각 프로토콜 타겟)
 *   - app/*, examples/bdev, module/bdev/*(모듈이 동일 API로 상호 호출)
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_bdev_desc:    열기 결과 반환되는 descriptor (opaque). 이후 I/O/속성 질의 키
 *   - spdk_bdev_open_ext/close: descriptor 수명 관리
 *   - spdk_bdev_get_io_channel: 스레드별 I/O 채널 획득
 *   - spdk_bdev_read_blocks / write_blocks / unmap / flush / reset: 핵심 I/O API
 *   - readv_blocks_with_md / _ext: scatter-gather + 메타 + 확장 옵션 경로
 *   - spdk_bdev_seek_data / seek_hole: 스파스 영역 탐색 (POSIX SEEK_DATA/HOLE 등가)
 *   - spdk_bdev_histogram_enable_ext / channel_get_histogram: per-IO-type latency 분포 측정
 *   - spdk_bdev_for_each_channel + continue: per-channel 비동기 작업 패턴
 *   - spdk_bdev_io_timeout_cb 등: timeout/통계/히스토그램 관측
 *
 * 핵심 인사이트:
 *   - DIF/PI: NVMe Base Spec 5.27 (Type1/2/3, PIF=16/32/64B variants), GUARD/APPTAG/REFTAG의
 *     세 검사 항목이 PRINFO/PRCHK 비트로 SQE에 매핑. desc 시점 vs bdev 시점 getter 분리는
 *     hide_metadata 옵션 지원 때문 (상위 계층이 PI를 보지 않도록 마스킹).
 *   - seek_data/seek_hole: thin-provisioned bdev(lvol)의 cluster 할당 비트맵 또는 NVMe DSM
 *     정보를 활용하여 빈 영역 skip — rsync-style 스파스 복사의 핵심 프리미티브.
 *   - histogram: bdev 단위 enable이지만 누적은 채널별. histogram_get은 모든 채널 메시지
 *     집계(비동기), channel_get_histogram은 단일 채널 직접 조회. opts.granularity로 메모리·
 *     CPU 비용 vs 해상도 trade-off, io_type 필터로 READ/WRITE 분리 분석 가능.
 */

#ifndef SPDK_BDEV_H_             /* [한국어] include 가드 */
#define SPDK_BDEV_H_

#include "spdk/stdinc.h"         /* [한국어] 표준 타입 */

#include "spdk/accel.h"          /* [한국어] spdk_accel_sequence — DMA/가속기 offload 시퀀스 */
#include "spdk/scsi_spec.h"      /* [한국어] SCSI 센스/상태 코드 — set_scsi_status API */
#include "spdk/nvme_spec.h"      /* [한국어] NVMe SQE/CQE — passthru API, set_nvme_status */
#include "spdk/json.h"           /* [한국어] JSON write ctx — 구성/RPC 출력 */
#include "spdk/queue.h"          /* [한국어] TAILQ/STAILQ — 이벤트/대기 리스트 */
#include "spdk/histogram_data.h" /* [한국어] 지연시간 히스토그램 API */
#include "spdk/dif.h"            /* [한국어] T10 DIF — PI 처리 관련 타입 */

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_BDEV_SMALL_BUF_MAX_SIZE 8192
                                  /* [한국어] 내부 iobuf 풀의 "작은 버퍼" 상한 (8KB)
                                   *  - bdev 코어가 get_buf 콜백 경로에서 "작은 풀" vs "큰 풀"을 크기 기반으로 선택 */
#define SPDK_BDEV_LARGE_BUF_MAX_SIZE (64 * 1024)
                                  /* [한국어] "큰 버퍼" 상한 (64KB) — 초과 요청은 bdev 코어가 실패 처리하거나 split */

#define SPDK_BDEV_MAX_INTERLEAVED_MD_SIZE (64)
                                  /* [한국어] 인터리브 메타데이터 최대 바이트/블록 (64)
                                   *  - 512B 블록당 최대 64B PI — NVMe PI 극단값을 커버 */

/* Increase the buffer size to store interleaved metadata.  Increment is the
 *  amount necessary to store metadata per data block.  SPDK_BDEV_MAX_INTERLEAVED_MD_SIZE
 *  bytes metadata per 512 byte data block is the current maximum ratio of
 *  metadata per block.
 */
#define SPDK_BDEV_BUF_SIZE_WITH_MD(x)	(((x) / 512) * (512 + SPDK_BDEV_MAX_INTERLEAVED_MD_SIZE))
                                  /* [한국어] 사용자 buf 크기 x에 extended-LBA 메타를 포함했을 때 실제 필요 버퍼 크기 계산
                                   *  - 예: 4096B 페이로드 → 4096/512 = 8블록 → 8 × (512 + 64) = 4608B 할당
                                   *  - 사용자가 직접 버퍼 사이징을 할 때 매크로로 간소화 */

/** Asynchronous event type */
/*
 * [한국어] bdev 비동기 이벤트 타입
 *
 * `spdk_bdev_open_ext(..., event_cb, ...)`에 등록된 사용자 콜백으로
 * bdev 코어가 외부 상태 변화를 전달. 애플리케이션은 각 이벤트에 대해
 * 적절한 복구/정리(예: REMOVE 시 close, RESIZE 시 num_blocks 재조회)를
 * 수행해야 한다.
 */
enum spdk_bdev_event_type {
	SPDK_BDEV_EVENT_REMOVE,
                                  /* [한국어] bdev 제거 예정 — 애플리케이션은 즉시 spdk_bdev_close(desc) 해야 함
                                   *  - hot-unplug, unregister RPC 등에서 발생 */
	SPDK_BDEV_EVENT_RESIZE,
                                  /* [한국어] 용량 변경 — 애플리케이션이 spdk_bdev_get_num_blocks 재호출해 새 크기 확인 */
	SPDK_BDEV_EVENT_MEDIA_MANAGEMENT,
                                  /* [한국어] ZNS 등 미디어 관리 이벤트 — spdk_bdev_media_event 구조체로 세부 정보 */
};

/** Media management event details */
struct spdk_bdev_media_event {
	uint64_t	offset;
                                  /* [한국어] 영향 받은 영역 시작 LBA */
	uint64_t	num_blocks;
                                  /* [한국어] 영향 받은 블록 수 */
};

/**
 * \brief SPDK block device.
 *
 * This is a virtual representation of a block device that is exported by the backend.
 */
struct spdk_bdev;
                                  /* [한국어] opaque — 실제 정의는 spdk/bdev_module.h */

/**
 * Block device remove callback.
 *
 * \param remove_ctx Context for the removed block device.
 */
typedef void (*spdk_bdev_remove_cb_t)(void *remove_ctx);
                                  /* [한국어] REMOVE 콜백 legacy 형태 — 신규 코드는 event_cb_t 사용 권장 */

/**
 * Block device event callback.
 *
 * \param type Event type.
 * \param bdev Block device that triggered event.
 * \param event_ctx Context for the block device event.
 */
typedef void (*spdk_bdev_event_cb_t)(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				     void *event_ctx);
                                  /* [한국어] 통합 이벤트 콜백 — spdk_bdev_open_ext에 등록
                                   *  - 실행 컨텍스트: bdev 코어가 적절한 스레드로 메시지 전달 후 호출 */

/**
 * Block device I/O
 *
 * This is an I/O that is passed to an spdk_bdev.
 */
struct spdk_bdev_io;
                                  /* [한국어] opaque — 정의는 bdev_module.h. I/O 요청 객체 */

struct spdk_bdev_fn_table;        /* [한국어] 모듈 콜백 vtable (bdev_module.h) */
struct spdk_io_channel;           /* [한국어] 스레드별 I/O 채널 (thread.h) */
struct spdk_json_write_ctx;       /* [한국어] JSON 출력 컨텍스트 (json.h) */
struct spdk_uuid;                 /* [한국어] UUID 타입 (uuid.h) */

/** bdev status */
enum spdk_bdev_status {
	SPDK_BDEV_STATUS_INVALID,
                                  /* [한국어] 유효하지 않음 — 초기화 실패/zero-init */
	SPDK_BDEV_STATUS_READY,
                                  /* [한국어] 준비 완료 — I/O 제출 가능 */
	SPDK_BDEV_STATUS_UNREGISTERING,
                                  /* [한국어] 등록 해제 진행 중 — 새 descriptor open 거부, in-flight I/O drain */
	SPDK_BDEV_STATUS_REMOVING,
                                  /* [한국어] 하드웨어 제거 중 — 모든 pending I/O가 에러로 완료됨 */
};

/**
 * \brief Handle to an opened SPDK block device.
 */
struct spdk_bdev_desc;
                                  /* [한국어] opaque — spdk_bdev_open_ext 결과. I/O 제출·속성 질의의 키
                                   *  - 한 bdev에 대해 다수 descriptor 가능 (각기 다른 스레드/용도) */

/** bdev I/O type */
/*
 * [한국어] bdev_io의 I/O 타입 — bdev 코어·모듈이 어느 union 멤버를 해석할지 결정
 *
 * 사용자 API(spdk_bdev_read 등)가 내부에서 이 enum 값을 bdev_io.type에 기록.
 * 모듈의 submit_request는 이 값을 switch로 분기해 각 백엔드 커맨드로 변환.
 */
enum spdk_bdev_io_type {
	SPDK_BDEV_IO_TYPE_INVALID = 0,
                                  /* [한국어] 무효 — 초기값, 제출 금지 */
	SPDK_BDEV_IO_TYPE_READ,
                                  /* [한국어] 블록 읽기 (hot path — 가장 빈번) */
	SPDK_BDEV_IO_TYPE_WRITE,
                                  /* [한국어] 블록 쓰기 (hot path) */
	SPDK_BDEV_IO_TYPE_UNMAP,
                                  /* [한국어] TRIM/DSM Deallocate — 영역 무효화 (SSD 성능·수명) */
	SPDK_BDEV_IO_TYPE_FLUSH,
                                  /* [한국어] 쓰기 캐시 플러시 — 볼륨 내구성 보장 */
	SPDK_BDEV_IO_TYPE_RESET,
                                  /* [한국어] bdev 리셋 — outstanding I/O abort + 장치 상태 복구 */
	SPDK_BDEV_IO_TYPE_NVME_ADMIN,
                                  /* [한국어] NVMe admin 커맨드 passthru — qpair 0번 발행 */
	SPDK_BDEV_IO_TYPE_NVME_IO,
                                  /* [한국어] NVMe I/O 커맨드 passthru — raw SQE 전달 */
	SPDK_BDEV_IO_TYPE_NVME_IO_MD,
                                  /* [한국어] NVMe I/O + 메타데이터 분리 버퍼 passthru */
	SPDK_BDEV_IO_TYPE_WRITE_ZEROES,
                                  /* [한국어] 제로 패턴 쓰기 — 네이티브 WRITE ZEROES 또는 zero 버퍼 write로 폴백 */
	SPDK_BDEV_IO_TYPE_ZCOPY,
                                  /* [한국어] zero-copy I/O (POPULATE/COMMIT 단계) */
	SPDK_BDEV_IO_TYPE_GET_ZONE_INFO,
                                  /* [한국어] ZNS: zone 상태 조회 */
	SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT,
                                  /* [한국어] ZNS: OPEN/CLOSE/FINISH/RESET/OFFLINE 관리 */
	SPDK_BDEV_IO_TYPE_ZONE_APPEND,
                                  /* [한국어] ZNS: zone append (장치가 write pointer 자동 할당) */
	SPDK_BDEV_IO_TYPE_COMPARE,
                                  /* [한국어] 비교 커맨드 — 데이터 매치 확인 */
	SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE,
                                  /* [한국어] atomic compare+write (fused) — 분산 합의 프리미티브 */
	SPDK_BDEV_IO_TYPE_ABORT,
                                  /* [한국어] 특정 bdev_io abort 요청 */
	SPDK_BDEV_IO_TYPE_SEEK_HOLE,
                                  /* [한국어] lseek SEEK_HOLE 의미 (thin-provisioned 영역 탐색) */
	SPDK_BDEV_IO_TYPE_SEEK_DATA,
                                  /* [한국어] lseek SEEK_DATA 의미 */
	SPDK_BDEV_IO_TYPE_COPY,
                                  /* [한국어] 장치 내 블록 복사 (NVMe Simple Copy, SCSI XCOPY) */
	SPDK_BDEV_IO_TYPE_NVME_IOV_MD,
                                  /* [한국어] NVMe I/O + iovec scatter + 메타 버퍼 */
	SPDK_BDEV_IO_TYPE_NVME_NSSR,
                                  /* [한국어] NVMe NVM Subsystem Reset */
	SPDK_BDEV_IO_TYPE_WRITE_UNCORRECTABLE,
                                  /* [한국어] 복구 불가 마킹 쓰기 — 이후 read 시 에러 반환 */
	SPDK_BDEV_NUM_IO_TYPES /* Keep last */
                                  /* [한국어] enum 크기 — 비트맵/배열 크기 산정에 사용 */
};

/**
 * Structure with optional enable histogram parameters
 */
/*
 * [한국어] struct spdk_bdev_enable_histogram_opts - 히스토그램 활성화 확장 옵션
 *
 * spdk_bdev_histogram_enable_ext()에 전달되어 히스토그램의 측정 범위·해상도·
 * 대상 I/O 타입을 세밀하게 제어. 기본 enable() API는 이 구조체 없이 호출되며,
 * 측정 범위·해상도는 SPDK 기본값을 따름.
 *
 * ABI 호환성: size 필드 우선 — 호출자가 sizeof 결과를 기록 → SPDK는 모르는
 * 신규 필드를 기본값으로 무시 (구형 호출자도 계속 동작).
 */
struct spdk_bdev_enable_histogram_opts {
	/** Size of this structure in bytes */
	size_t size;
	                                  /* [한국어] 호출자가 알고 있는 구조체 크기. spdk_bdev_enable_histogram_opts_init이 자동 설정 */

	/** Min value in nanoseconds to track in histogram */
	uint64_t min_nsec;
	                                  /* [한국어] 히스토그램 최소 측정 지연(ns) — 이 값 미만은 첫 버킷에 합산
	                                   *  - 0: SPDK 기본 (최저 버킷 = 1 tick)
	                                   *  - 작은 값(예: 100ns): 매우 빠른 NVMe 장치의 미세 분포까지 추적 */
	/** Max value in nanoseconds to track in histogram */
	uint64_t max_nsec;
	                                  /* [한국어] 히스토그램 최대 측정 지연(ns) — 이 값 초과는 마지막 버킷에 합산
	                                   *  - 0: SPDK 기본 (충분히 큰 상한)
	                                   *  - tail latency(예: 1초)까지 분포가 필요한 경우 명시 */
	uint8_t io_type;
	                                  /* [한국어] 측정 대상 I/O 타입 (enum spdk_bdev_io_type 값을 8bit로 압축)
	                                   *  - SPDK_BDEV_IO_TYPE_INVALID(0): 모든 타입 측정 (기본)
	                                   *  - 특정 타입 지정 시 그 타입의 latency만 누적 → READ/WRITE 분리 분석 */
	uint8_t granularity;
	                                  /* [한국어] 히스토그램 버킷 해상도 (bucket_shift) — 2^granularity ns 단위 버킷
	                                   *  - 작을수록 세밀, 클수록 메모리·CPU 절감
	                                   *  - 0: SPDK 기본 (대략 64 버킷/decade) */
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_enable_histogram_opts) == 26, "Incorrect size");
                                  /* [한국어] 26B 고정 — packed로 패딩 제거. 필드 추가는 끝에 (ABI 호환) */

/** bdev QoS rate limit type */
/*
 * [한국어] QoS 속도 제한 타입 — 4가지 독립 리미터
 *
 * 같은 bdev에 대해 IOPS와 BPS를 동시에 설정 가능 (어느 쪽이든 걸리면 throttle).
 * 읽기 전용 BPS, 쓰기 전용 BPS를 분리해 비대칭 한도도 지원.
 */
enum spdk_bdev_qos_rate_limit_type {
	/** IOPS rate limit for both read and write */
	SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT = 0,
                                  /* [한국어] 읽기+쓰기 합산 IOPS 제한 */
	/** Byte per second rate limit for both read and write */
	SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT,
                                  /* [한국어] 읽기+쓰기 합산 초당 바이트(BPS) 제한 */
	/** Byte per second rate limit for read only */
	SPDK_BDEV_QOS_R_BPS_RATE_LIMIT,
                                  /* [한국어] 읽기 전용 BPS */
	/** Byte per second rate limit for write only */
	SPDK_BDEV_QOS_W_BPS_RATE_LIMIT,
                                  /* [한국어] 쓰기 전용 BPS */
	/** Keep last */
	SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES
                                  /* [한국어] enum 크기 — QoS 배열 차원 */
};

/**
 * Block device completion callback.
 *
 * \param bdev_io Block device I/O that has completed.
 * \param success True if I/O completed successfully or false if it failed;
 * additional error information may be retrieved from bdev_io by calling
 * spdk_bdev_io_get_nvme_status() or spdk_bdev_io_get_scsi_status().
 * \param cb_arg Callback argument specified when bdev_io was submitted.
 */
typedef void (*spdk_bdev_io_completion_cb)(struct spdk_bdev_io *bdev_io,
		bool success,
		void *cb_arg);
                                  /* [한국어] I/O 완료 콜백 — 모든 bdev I/O API가 이 시그니처의 cb를 받음
                                   *  - 실행 컨텍스트: 제출한 스레드와 동일 (채널 고정 보장)
                                   *  - success=false면 spdk_bdev_io_get_{nvme,scsi,aio}_status()로 세부 에러 조회
                                   *  - 콜백 내에서 spdk_bdev_free_io(bdev_io) 호출 필수 */

struct spdk_bdev_io_error_stat;   /* [한국어] opaque — 에러 타입별 카운터 (bdev 내부 정의) */

/*
 * [한국어] struct spdk_bdev_io_stat - bdev 통계 스냅샷
 *
 * `spdk_bdev_get_device_stat()` 호출 시 이 구조체에 복사되어 반환.
 * RPC(bdev_get_iostat)가 이 값을 JSON으로 직렬화.
 *
 * latency는 ticks_rate를 제수로 써서 초 단위로 환산 가능.
 */
struct spdk_bdev_io_stat {
	uint64_t bytes_read;          /* [한국어] 총 읽은 바이트 */
	uint64_t num_read_ops;        /* [한국어] 읽기 명령 수 */
	uint64_t bytes_written;       /* [한국어] 총 쓴 바이트 */
	uint64_t num_write_ops;       /* [한국어] 쓰기 명령 수 */
	uint64_t bytes_unmapped;      /* [한국어] UNMAP된 총 바이트 */
	uint64_t num_unmap_ops;       /* [한국어] UNMAP 명령 수 */
	uint64_t bytes_copied;        /* [한국어] 복사된 총 바이트 (COPY 명령 기준) */
	uint64_t num_copy_ops;        /* [한국어] COPY 명령 수 */
	uint64_t read_latency_ticks;  /* [한국어] 읽기 지연 누적 틱 — 평균 = read_latency_ticks / num_read_ops */
	uint64_t max_read_latency_ticks;
                                  /* [한국어] 최대 읽기 지연 (tail latency 관찰) */
	uint64_t min_read_latency_ticks;
                                  /* [한국어] 최소 읽기 지연 */
	uint64_t write_latency_ticks; /* [한국어] 쓰기 지연 누적 */
	uint64_t max_write_latency_ticks;
	uint64_t min_write_latency_ticks;
	uint64_t unmap_latency_ticks; /* [한국어] UNMAP 지연 누적 */
	uint64_t max_unmap_latency_ticks;
	uint64_t min_unmap_latency_ticks;
	uint64_t copy_latency_ticks;  /* [한국어] COPY 지연 누적 */
	uint64_t max_copy_latency_ticks;
	uint64_t min_copy_latency_ticks;
	uint64_t ticks_rate;          /* [한국어] 초당 틱 수 — spdk_get_ticks_hz()와 동일. latency 환산 제수 */

	/* This data structure is privately defined in the bdev library.
	 * This data structure is only used by the bdev_get_iostat RPC now.
	 */
	struct spdk_bdev_io_error_stat *io_error;
                                  /* [한국어] 에러 타입별 카운터 (현재는 bdev_get_iostat RPC만 사용)
                                   *  - NULL이면 에러 통계 비활성 빌드 */

	/* For efficient deep copy, no members should be added after io_error. */
                                  /* [한국어] deep copy 효율 — io_error 뒤에 멤버 추가 금지 */
};

/*
 * [한국어] struct spdk_bdev_opts - bdev 서브시스템 전역 튜닝 옵션
 *
 * spdk_bdev_set_opts()로 초기화 전 미리 적용 (initialize 후 변경 시 일부 필드 무시).
 * RPC bdev_set_options 또는 JSON config에서 값 주입.
 * ABI 호환성: opts_size 필드로 호출자 인지 크기 전달.
 */
struct spdk_bdev_opts {
	uint32_t bdev_io_pool_size;
	                                  /* [한국어] 전역 spdk_bdev_io 풀 크기 (요소 수)
	                                   *  - 풀 고갈 시 제출 API가 -ENOMEM 반환
	                                   *  - 권장값: 동시 in-flight 최대 + 마진 (기본 65536) */
	uint32_t bdev_io_cache_size;
	                                  /* [한국어] 채널당(thread별) bdev_io 캐시 크기
	                                   *  - 전역 풀 → 채널 캐시 → 채널 큐 의 2단계 lockless allocation
	                                   *  - 캐시 hit 시 락 없이 빠른 할당 (기본 256) */
	bool bdev_auto_examine;
	                                  /* [한국어] 새 bdev 등록 시 모든 모듈의 examine_config/examine_disk 자동 호출 여부
	                                   *  - true(기본): vbdev이 leaf 위에 자동 적층 (lvol/raid 등 자동 발견)
	                                   *  - false: 수동 spdk_bdev_examine 호출만 허용 (단위 테스트/특수 시나리오) */

	/* Hole at bytes 9-15. */
	uint8_t reserved9[7];
	                                  /* [한국어] 정렬·예약 패딩 — 향후 bool 추가 여지 */

	/**
	 * The size of spdk_bdev_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	                                  /* [한국어] ABI 호환: 호출자가 알고 있는 구조체 크기 — spdk_bdev_get_opts에 sizeof로 전달 */

	/* Size of the per-thread iobuf caches */
	uint32_t iobuf_small_cache_size;
	                                  /* [한국어] iobuf 풀 "작은 버퍼"(<=8KB) 채널별 캐시 크기 */
	uint32_t iobuf_large_cache_size;
	                                  /* [한국어] iobuf 풀 "큰 버퍼"(<=64KB) 채널별 캐시 크기
	                                   *  - 작은/큰 풀 분리 — 작은 I/O 대량/큰 I/O 산발 등 워크로드별 튜닝 */
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_opts) == 32, "Incorrect size");
                                  /* [한국어] 32B 고정 — 필드 추가 시 끝부분에만, opts_size로 ABI 분기 */

/**
 * Union for controller attributes field, to list whether bdev supports fdp etc.
 * By convention we match the NVMe definition, allowing other bdevs to use this feature
 */
/*
 * [한국어] union spdk_bdev_nvme_ctratt - NVMe Controller Attributes 비트맵
 *
 * NVMe 1.4+ Identify Controller 응답의 CTRATT 필드(384B 오프셋)를 그대로 미러링.
 * NVMe가 아닌 bdev이라도 같은 비트 의미로 기능 노출 가능 (예: malloc bdev이 fdps=1 설정).
 * spdk_bdev_get_nvme_ctratt(bdev)로 조회.
 */
union spdk_bdev_nvme_ctratt {
	uint32_t raw;
	                                  /* [한국어] 32비트 원본 — CTRATT 그대로 */

	struct {
		uint32_t reserved	: 19;
	                                  /* [한국어] reserved 비트 (현재 NVMe 스펙에서 미정의) */
		/* Supports flexible data placement */
		uint32_t fdps		: 1;
	                                  /* [한국어] FDP(Flexible Data Placement) 지원 여부 — NVMe TP4146
	                                   *  - 1이면 사용자가 cdw12.dtype=2(placement) + cdw13.dspec(handle)로 placement 힌트 전달 가능
	                                   *  - 모듈/장치가 hot/cold 데이터 분리·GC 비용 절감 */
		uint32_t reserved2	: 12;
	                                  /* [한국어] 향후 확장용 reserved */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_bdev_nvme_ctratt) == 4, "Incorrect size");
                                  /* [한국어] 4B 고정 (NVMe DWORD) */

/**
 * Union for command dword 12, which by convention matches the NVMe command dword 12 definition.
 * This is used to pass NVMe specific fields to bdevs, that reports support for them as indicated
 * by \ref spdk_bdev_get_nvme_ctratt
 */
/*
 * [한국어] union spdk_bdev_nvme_cdw12 - NVMe SQE Command DWORD 12 미러
 *
 * spdk_bdev_ext_io_opts.nvme_cdw12로 전달 → bdev_nvme 모듈이 그대로 SQE 빌드.
 * 비트 정의는 NVMe Base Spec 의 Write/Read 커맨드 cdw12와 일치.
 */
union spdk_bdev_nvme_cdw12 {
	uint32_t raw;
	                                  /* [한국어] 32비트 원본 — 사용자가 NVMe 스펙대로 직접 비트 조립 */

	struct {
		uint32_t reserved	: 20;
	                                  /* [한국어] reserved (NLB 등 코어 필드는 bdev 코어가 채움) */
		/* Directive type */
		uint32_t dtype		: 4;
	                                  /* [한국어] Directive Type — Write 명령에서 지시자(directive) 종류
	                                   *  - 1: Streams Directive
	                                   *  - 2: Data Placement Directive (FDP)
	                                   *  - cdw13.dspec와 짝으로 동작 */
		uint32_t reserved2	: 8;
	                                  /* [한국어] reserved (PRINFO/FUA/LR 등 NVMe 코어 비트는 bdev 코어가 관리) */
	} write;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_bdev_nvme_cdw12) == 4, "Incorrect size");
                                  /* [한국어] 4B (NVMe DWORD) */

/**
 * Union for command dword 13, which by convention matches the NVMe command dword 13 definition.
 * This is used to pass NVMe specific fields to bdevs, that reports support for them as indicated
 * by \ref spdk_bdev_get_nvme_ctratt
 */
/*
 * [한국어] union spdk_bdev_nvme_cdw13 - NVMe SQE Command DWORD 13 미러
 */
union spdk_bdev_nvme_cdw13 {
	uint32_t raw;
	                                  /* [한국어] 32비트 원본 */

	struct {
		uint32_t reserved	: 16;
	                                  /* [한국어] reserved */
		/* Directive specific */
		uint32_t dspec		: 16;
	                                  /* [한국어] Directive Specific (cdw12.dtype과 짝)
	                                   *  - dtype=1(Streams): Stream Identifier
	                                   *  - dtype=2(FDP): Placement Handle (Reclaim Unit Handle Identifier) */
	} write;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_bdev_nvme_cdw13) == 4, "Incorrect size");
                                  /* [한국어] 4B (NVMe DWORD) */

/**
 * Structure with optional IO request parameters
 */
/*
 * [한국어] struct spdk_bdev_ext_io_opts — ★ _ext variant API의 옵션 번들 ★
 *
 * spdk_bdev_readv_blocks_ext/writev_blocks_ext 등이 받는 확장 옵션.
 * ABI 호환성: size 필드로 호출자가 인지한 구조체 크기 전달. SPDK는 모르는
 * 신규 필드를 기본값으로 무시, 구형 호출자도 계속 동작.
 *
 * 주요 용도:
 *   - NVMe-oF 타겟: RDMA 메모리 도메인 전달 (호스트 메모리 직접 접근 불가)
 *   - DMA 엔진 오프로드: accel_sequence로 CRC/checksum/compress 체이닝
 *   - 인라인 메타데이터: _with_md 대신 metadata 필드 경로
 *   - PI 검사 세밀 제어: 특정 DIF 검사만 이 I/O에서 비활성화
 *   - NVMe 스펙 확장: cdw12/cdw13으로 directive 전달
 */
struct spdk_bdev_ext_io_opts {
	/** Size of this structure in bytes */
	size_t size;
                                  /* [한국어] ABI 호환: 호출자가 sizeof로 전달. bdev 코어가 알고 있는 크기와 min으로 비교하여 안전 처리 */
	/** Memory domain which describes payload in this IO request. bdev must support DMA device type that
	 * can access this memory domain, refer to \ref spdk_bdev_get_memory_domains and \ref spdk_memory_domain_get_dma_device_type
	 * If set, that means that data buffers can't be accessed directly and the memory domain must
	 * be used to fetch data to local buffers or to translate data to another memory domain */
	struct spdk_memory_domain *memory_domain;
                                  /* [한국어] 페이로드의 메모리 도메인 기술자 (RDMA 원격 메모리, GPU 메모리 등)
                                   *  - NULL이면 호스트 DRAM 직접 접근 가능
                                   *  - 설정 시 bdev 모듈은 memory_domain API로 fetch/translate 수행 — CPU 직접 복사 금지
                                   *  - bdev이 이 도메인 타입을 지원하지 않으면 -ENOTSUP 반환 */
	/** Context to be passed to memory domain operations */
	void *memory_domain_ctx;
                                  /* [한국어] 메모리 도메인 연산 시 사용자 정의 컨텍스트 (RDMA pd/qp 등) */
	/** Metadata buffer, optional */
	void *metadata;
                                  /* [한국어] 메타데이터 버퍼 (optional, _with_md 변형을 대체) — NULL이면 메타 전송 없음 */
	/**
	 * Sequence of accel operations to be executed before/after (depending on the IO type) the
	 * request is submitted.
	 */
	struct spdk_accel_sequence *accel_sequence;
                                  /* [한국어] accel 시퀀스 — DMA 엔진·CRC·복사·압축·암호화 체이닝 (write 전 / read 후)
                                   *  - NULL이면 offload 없음
                                   *  - bdev의 accel_sequence_supported 비트로 지원 여부 사전 확인 */
	/**
	 * Specify which DIF check flags to exclude on a per-IO basis. The default value is
	 * all zeroes, which includes all of the flags set for this bdev. If any of the flags
	 * is set, that flag will be excluded from any DIF operations for this IO.
	 */
	uint32_t dif_check_flags_exclude_mask;
                                  /* [한국어] 이 I/O 한정 DIF 검사 비활성화 마스크 (GUARD/APPTAG/REFTAG 개별 bit)
                                   *  - bdev 전역 dif_check_flags에서 여기 비트를 뺀 것이 실제 적용 플래그
                                   *  - 사용처: 초기 포맷, 마이그레이션 중 PI 재계산 구간에서 일부 검사만 끄기 */
	/** defined by \ref spdk_bdev_nvme_cdw12 */
	union spdk_bdev_nvme_cdw12 nvme_cdw12;
                                  /* [한국어] NVMe CDW12 원본 값 (directive type 등) — bdev_nvme 모듈이 그대로 SQE에 복사 */
	/** defined by \ref spdk_bdev_nvme_cdw13 */
	union spdk_bdev_nvme_cdw13 nvme_cdw13;
                                  /* [한국어] NVMe CDW13 (directive specific) */
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_ext_io_opts) == 52, "Incorrect size");
                                  /* [한국어] 크기 52B 고정 — 필드 추가 시 반드시 끝에 (ABI 호환) */

/**
 * Get the options for the bdev module.
 *
 * \param opts Output parameter for options.
 * \param opts_size sizeof(*opts)
 */
void spdk_bdev_get_opts(struct spdk_bdev_opts *opts, size_t opts_size);
/*
 * [한국어]
 * spdk_bdev_get_opts - 현재 bdev 서브시스템 전역 옵션 조회
 *
 * @opts: 출력 — 호출자가 미리 할당한 spdk_bdev_opts에 현재값 복사
 * @opts_size: sizeof(*opts) — ABI 호환성 위해 호출자 인지 크기 전달
 *
 * 사용 패턴:
 *   struct spdk_bdev_opts opts;
 *   spdk_bdev_get_opts(&opts, sizeof(opts));   // 현재값 로드
 *   opts.bdev_io_pool_size = 131072;            // 일부만 수정
 *   spdk_bdev_set_opts(&opts);                  // 적용
 *
 * spdk_bdev_initialize 전에 호출해야 의미 있음 (이후엔 일부 필드만 동적 변경 가능).
 */

int spdk_bdev_set_opts(struct spdk_bdev_opts *opts);
/*
 * [한국어]
 * spdk_bdev_set_opts - bdev 서브시스템 전역 옵션 설정
 *
 * @opts: 적용할 옵션. opts->opts_size로 호출자 인지 크기 검증
 * @return: 0 성공, -EINVAL 잘못된 값(예: pool_size < cache_size)
 *
 * 호출 시점: 반드시 spdk_bdev_initialize() **이전**에 호출.
 * 일부 필드(pool_size 등)는 init 후 변경 시 무시 — 풀이 이미 할당된 상태이므로.
 * 사용처: app 시작 단계의 RPC bdev_set_options 또는 JSON config "bdev_set_options" 메서드.
 */

typedef void (*spdk_bdev_wait_for_examine_cb)(void *arg);
                                  /* [한국어] examine 완료 통지 콜백 (spdk_bdev_wait_for_examine 등록)
                                   *  - 모든 모듈의 examine_disk가 완료되면 호출 (1회성)
                                   *  - vbdev 자동 적층 완료 후 첫 I/O 가능 시점 식별에 사용 */

/*
 * [한국어] enum spdk_bdev_reset_stat_mode - 통계 조회 후 리셋 정책
 *
 * spdk_bdev_get_io_stat / get_device_stat 호출 시 통계 조회 후 어떤 카운터를 0으로
 * 되돌릴지 결정. 주기적으로 통계를 샘플링할 때 누적/주기 모드 선택.
 */
enum spdk_bdev_reset_stat_mode {
	/** Reset all stats */
	SPDK_BDEV_RESET_STAT_ALL,
	                                  /* [한국어] 모든 통계 초기화 — 매 호출마다 직전 호출 이후 델타로 동작 */
	/** Reset only max and min stats */
	SPDK_BDEV_RESET_STAT_MAXMIN,
	                                  /* [한국어] max/min 지연만 리셋 (누적 카운터·합계는 유지)
	                                   *  - 윈도우별 최악·최저 latency 추적용 */
	/** Reset i/o error stats */
	SPDK_BDEV_RESET_STAT_ERROR,
	                                  /* [한국어] 에러 카운터만 리셋 (정상 카운터·지연은 유지) */
	/** Do not reset stats at all */
	SPDK_BDEV_RESET_STAT_NONE,
	                                  /* [한국어] 리셋 없음 — 누적값 그대로 조회 (모니터링 시스템에서 차이 계산) */
};

/**
 * Report when all bdevs finished the examine process.
 * The registered cb_fn will be called just once.
 * This function needs to be called again to receive
 * further reports on examine process.
 *
 * \param cb_fn Callback function.
 * \param cb_arg Callback argument.
 * \return 0 if function was registered, suitable errno value otherwise
 */
int spdk_bdev_wait_for_examine(spdk_bdev_wait_for_examine_cb cb_fn, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_wait_for_examine - 모든 bdev examine 완료 시점 통지 등록
 *
 * @cb_fn: 모든 모듈의 examine_disk 체인이 완료되면 호출 (1회성)
 * @cb_arg: 콜백 컨텍스트
 * @return: 0 등록 성공, 음수 errno 실패
 *
 * 동기:
 *   - bdev 등록 시 examine 체인이 비동기로 vbdev을 적층 (NVMe → lvol_store → lvol_blob)
 *   - 사용자가 lvol을 open하기 위해선 examine이 끝나야 함
 *   - 이 함수가 그 "안정 상태(quiescent)" 시점을 알려줌
 *
 * 재호출 필요: 콜백은 1회만 호출 → 후속 examine 라운드 감지하려면 다시 등록.
 * 사용처: RPC bdev_wait_for_examine, app 부팅 시퀀스에서 명시적 동기화.
 */

/**
 * Examine a block device explicitly
 *
 * This function must be called from the SPDK app thread.
 *
 * \param name the name or alias of the block device
 * \return 0 if block device was examined successfully, suitable errno value otherwise
 */
int spdk_bdev_examine(const char *name);
/*
 * [한국어]
 * spdk_bdev_examine - 특정 bdev에 대해 명시적 examine 트리거
 *
 * @name: bdev 본이름 또는 별칭
 * @return: 0 성공, 음수 errno (-ENODEV 해당 bdev 없음)
 *
 * 동기: bdev_auto_examine=false 환경에서 사용자가 vbdev 적층을 수동 제어할 때 사용.
 * 동작: 모든 모듈의 examine_config + examine_disk 콜백을 이 bdev에 대해 호출.
 * 제약: SPDK app thread에서만 호출 가능 (모듈 examine 콜백이 단일 스레드 가정).
 */

/**
 * Block device initialization callback.
 *
 * \param cb_arg Callback argument.
 * \param rc 0 if block device initialized successfully or negative errno if it failed.
 */
typedef void (*spdk_bdev_init_cb)(void *cb_arg, int rc);
                                  /* [한국어] spdk_bdev_initialize 완료 콜백
                                   *  - @rc==0: 모든 모듈 init 성공
                                   *  - @rc<0: 일부 모듈 init 실패 (errno) — 애플리케이션은 spdk_app_stop 등으로 중단 결정
                                   *  - 호출 스레드: app thread (initialize 호출 스레드와 동일) */

/**
 * Block device finish callback.
 *
 * \param cb_arg Callback argument.
 */
typedef void (*spdk_bdev_fini_cb)(void *cb_arg);
                                  /* [한국어] spdk_bdev_finish 완료 콜백 — 모든 모듈 fini 후 호출 (rc 없음) */
typedef void (*spdk_bdev_get_device_stat_cb)(struct spdk_bdev *bdev,
		struct spdk_bdev_io_stat *stat, void *cb_arg, int rc);
                                  /* [한국어] spdk_bdev_get_device_stat 비동기 완료 콜백
                                   *  - @stat: 모든 채널을 합산한 통계 (호출자가 할당, get_device_stat에 전달한 그대로)
                                   *  - @rc: 0 성공, 음수 errno (drain 도중 실패 등)
                                   *  - 실행 컨텍스트: get_device_stat 호출 스레드 */

/**
 * Block device channel IO timeout callback
 *
 * \param cb_arg Callback argument
 * \param bdev_io The IO cause the timeout
 */
typedef void (*spdk_bdev_io_timeout_cb)(void *cb_arg, struct spdk_bdev_io *bdev_io);
                                  /* [한국어] I/O 타임아웃 콜백 (spdk_bdev_set_timeout 등록)
                                   *  - @bdev_io: 타임아웃 발생한 in-flight I/O — abort/reset/log 등을 호출자가 결정
                                   *  - 호출 스레드: bdev_io를 제출한 채널 스레드 (poller가 검사 후 발견) */

/**
 * Initialize block device modules.
 *
 * Calling this function from any thread is deprecated and will be disallowed in the 26.05 release.
 * This function should be called from the SPDK app thread.
 *
 * \param cb_fn Called when the initialization is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bdev_initialize(spdk_bdev_init_cb cb_fn, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_initialize - bdev 서브시스템 전체 초기화 (★ 애플리케이션 부팅 진입점 ★)
 *
 * 동작:
 *   1) 모든 등록된 bdev 모듈의 module_init() 순차 호출
 *   2) 각 모듈이 자체 bdev 생성 후 examine_config/examine_disk 체인으로 vbdev 자동 발견
 *   3) 전 과정 완료 시 cb_fn(cb_arg, rc) 호출
 *
 * 제약: SPDK app thread(reactor 0번)에서 호출 권장 (26.05에서 강제화 예정).
 * 비동기: 모듈의 async_init=true일 때 실제 완료는 이후 spdk_bdev_module_init_done() 경유
 * 호출 체인: spdk_app_start → 서브시스템 초기화 → spdk_bdev_initialize → 모듈 init → cb_fn
 */

/**
 * Perform cleanup work to remove the registered block device modules.
 *
 * Calling this function from any thread is deprecated and will be disallowed in the 26.05 release.
 * This function should be called from the SPDK app thread.
 *
 * \param cb_fn Called when the removal is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bdev_finish(spdk_bdev_fini_cb cb_fn, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_finish - bdev 서브시스템 전체 종료
 *
 * 동작: 모듈별 fini_start → 모든 bdev unregister → module_fini → cb_fn 호출.
 * 주의: 호출 전 모든 descriptor가 close 되어 있어야 함 (미완료 open이 있으면 무한 대기).
 * 제약: SPDK app thread에서 호출.
 */

/**
 * Get the full configuration options for the registered block device modules and created bdevs.
 *
 * \param w pointer to a JSON write context where the configuration will be written.
 */
void spdk_bdev_subsystem_config_json(struct spdk_json_write_ctx *w);
/*
 * [한국어]
 * spdk_bdev_subsystem_config_json - bdev 서브시스템 전체 설정을 JSON 출력
 *
 * @w: spdk_json_write_ctx (이미 array context 시작 상태)
 * 동작:
 *   - bdev_set_options 메서드 출력 (전역 옵션)
 *   - 각 등록 모듈의 config_json 콜백 호출 → 모듈별 RPC 메서드 시퀀스 작성
 *   - 결과: app 재시작 시 그대로 재현 가능한 RPC 메서드 배열
 * 사용처: spdk_app_save_config, "spdk_app_get_config" 응답.
 */

/**
 * Get block device module name.
 *
 * \param bdev Block device to query.
 * \return Name of bdev module as a null-terminated string.
 */
const char *spdk_bdev_get_module_name(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_module_name - bdev이 속한 모듈 이름 반환 ("nvme", "malloc", "lvol", "raid" 등)
 * RPC bdev_get_bdevs 응답의 "driver_specific" 키 분기에 사용.
 */

/**
 * Get block device by the block device name.
 *
 * \param bdev_name The name of the block device.
 * \return Block device associated with the name or NULL if no block device with
 * bdev_name is currently registered.
 */
struct spdk_bdev *spdk_bdev_get_by_name(const char *bdev_name);
/*
 * [한국어]
 * spdk_bdev_get_by_name - 이름으로 bdev 포인터 조회 (별칭 포함)
 *
 * @bdev_name: 본이름 또는 별칭
 * @return: 매칭 bdev 포인터, 없으면 NULL
 *
 * 내부: 전역 이름 RB 트리(bdev_names)에서 O(log n) lookup.
 * 주의: 반환 포인터의 수명을 호출자가 보장해야 함(동시 unregister와의 경합).
 * 안전한 사용: spdk_bdev_open_ext(bdev_name, ...) 경로로 desc 획득 권장.
 */

/**
 * Get the first registered block device.
 *
 * \return The first registered block device.
 */
struct spdk_bdev *spdk_bdev_first(void);
/*
 * [한국어]
 * spdk_bdev_first - 등록된 bdev 리스트의 첫 엔트리 반환 (열거 시작점)
 * 호출 체인: spdk_bdev_first → spdk_bdev_next(...) 반복 → NULL 반환으로 종료
 */

/**
 * Get the next registered block device.
 *
 * \param prev The current block device.
 * \return The next registered block device.
 */
struct spdk_bdev *spdk_bdev_next(struct spdk_bdev *prev);
/*
 * [한국어]
 * spdk_bdev_next - prev 다음의 bdev 반환. 마지막이면 NULL
 */

/**
 * Get the first block device without virtual block devices on top.
 *
 * This function only traverses over block devices which have no virtual block
 * devices on top of them, then get the first one.
 *
 * \return The first block device without virtual block devices on top.
 */
struct spdk_bdev *spdk_bdev_first_leaf(void);
/*
 * [한국어]
 * spdk_bdev_first_leaf - "leaf" bdev 열거 시작 (그 위에 vbdev이 없음)
 *
 * 사용처: 사용자에게 실제 저장소로 I/O 발행 가능한 최상위 bdev만 나열
 *   - 예: NVMe ns 위에 lvol이 있으면 lvol이 leaf, 밑의 NVMe ns는 제외
 */

/**
 * Get the next block device without virtual block devices on top.
 *
 * This function only traverses over block devices which have no virtual block
 * devices on top of them, then get the next one.
 *
 * \param prev The current block device.
 * \return The next block device without virtual block devices on top.
 */
struct spdk_bdev *spdk_bdev_next_leaf(struct spdk_bdev *prev);
/*
 * [한국어]
 * spdk_bdev_next_leaf - leaf 열거의 다음 엔트리
 */

/**
 * Structure with optional synchronous bdev open parameters.
 */
/*
 * [한국어] struct spdk_bdev_open_opts - bdev 열기 확장 옵션 (v2)
 *
 * ABI 호환성: size 필드로 호출자가 인지한 구조체 버전 전달 → SPDK가 모르는
 * 새 필드는 기본값으로 무시, 구형 호출자도 계속 동작.
 */
struct spdk_bdev_open_opts {
	/* Size of this structure in bytes. */
	size_t size;
                                  /* [한국어] 호출자가 알고 있는 구조체 크기 (sizeof 결과). opts_init으로 자동 설정 */

	/* To indicate that the upper layer do not want to use metadata
	 * with this bdev.
	 */
	bool hide_metadata;
                                  /* [한국어] true면 상위 계층이 PI/메타데이터를 보지 않도록 bdev 레이어가 숨김
                                   *  - 예: blobstore가 PI 처리를 bdev 모듈에 위임하지 않고 자체 정책으로 운영 */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_open_opts) == 16, "Incorrect size");
                                  /* [한국어] 구조체 크기 16B — 필드 추가·순서 변경 시 검증 */

/**
 * Initialize bdev open options.
 *
 * \param opts Bdev open options.
 * \param opts_size Must be set to sizeof(struct spdk_bdev_open_opts).
 */
void spdk_bdev_open_opts_init(struct spdk_bdev_open_opts *opts, size_t opts_size);
/*
 * [한국어]
 * spdk_bdev_open_opts_init - opts를 기본값으로 초기화
 *
 * 사용 관례:
 *   struct spdk_bdev_open_opts opts;
 *   spdk_bdev_open_opts_init(&opts, sizeof(opts));
 *   opts.hide_metadata = true;
 *   spdk_bdev_open_ext_v2(name, true, cb, ctx, &opts, &desc);
 *
 * opts_size를 sizeof로 자동 계산 → 향후 필드 추가에도 호출측 코드 변경 불필요.
 */

/**
 * Open a block device for I/O operations.
 *
 * \param bdev_name Block device name to open.
 * \param write true is read/write access requested, false if read-only
 * \param event_cb notification callback to be called when the bdev triggers
 * asynchronous event such as bdev removal. This will always be called on the
 * same thread that spdk_bdev_open_ext() was called on. In case of removal event
 * the descriptor will have to be manually closed to make the bdev unregister
 * proceed.
 * \param event_ctx param for event_cb.
 * \param desc output parameter for the descriptor when operation is successful
 * \return 0 if operation is successful, suitable errno value otherwise
 */
int spdk_bdev_open_ext(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
		       void *event_ctx, struct spdk_bdev_desc **desc);
/*
 * [한국어]
 * spdk_bdev_open_ext - bdev을 I/O용으로 열어 descriptor 획득
 *
 * @bdev_name: 본이름 또는 별칭
 * @write: true면 R/W, false면 R/O 모드 (R/O로 열면 write API 호출 실패)
 * @event_cb: 비동기 이벤트(REMOVE/RESIZE/MEDIA_MANAGEMENT) 알림 콜백
 *            - 호출 스레드: spdk_bdev_open_ext를 호출한 스레드 (thread affinity)
 *            - REMOVE 이벤트 수신 시 반드시 spdk_bdev_close() 호출 — 그래야 unregister 진행
 * @event_ctx: 콜백 컨텍스트
 * @desc: OUT — 성공 시 descriptor 포인터 기록
 * @return: 0 성공 / 음수 errno (ENODEV 해당 bdev 없음, EPERM claim 충돌, EBUSY 등)
 *
 * 호출 체인: spdk_bdev_open_ext → 채널 획득(spdk_bdev_get_io_channel) → I/O API
 * 주의: write=true로 여는 것이 자동으로 EXCL_WRITE claim을 거는 것은 아님.
 *       claim이 필요하면 spdk_bdev_module_claim_bdev_desc 별도 호출.
 */

/**
 * Open a block device for I/O operations with options.
 *
 * \param bdev_name Block device name to open.
 * \param write true is read/write access requested, false if read-only
 * \param event_cb notification callback to be called when the bdev triggers
 * asynchronous event such as bdev removal. This will always be called on the
 * same thread that spdk_bdev_open_ext() was called on. In case of removal event
 * the descriptor will have to be manually closed to make the bdev unregister
 * proceed.
 * \param event_ctx param for event_cb.
 * \param opts Option for block device open. If NULL, default values are used.
 * \param desc output parameter for the descriptor when operation is successful
 * \return 0 if operation is successful, suitable errno value otherwise
 */
int spdk_bdev_open_ext_v2(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
			  void *event_ctx, struct spdk_bdev_open_opts *opts,
			  struct spdk_bdev_desc **desc);
/*
 * [한국어]
 * spdk_bdev_open_ext_v2 - open_ext + 옵션 전달 확장 (hide_metadata 등)
 * opts가 NULL이면 기본값. 그 외 파라미터는 open_ext와 동일.
 */

/**
 * Block device asynchronous open callback.
 *
 * \param desc Output parameter for the descriptor when operation is successful.
 * \param rc 0 if block device is opened successfully or negated errno if failed.
 * \param cb_arg Callback argument.
 */
typedef void (*spdk_bdev_open_async_cb_t)(struct spdk_bdev_desc *desc, int rc, void *cb_arg);
                                  /* [한국어] spdk_bdev_open_async 완료 콜백
                                   *  - @desc: 성공 시 열린 descriptor (rc==0일 때만 유효)
                                   *  - @rc: 0 성공, 음수 errno */

/**
 * Structure with optional asynchronous bdev open parameters.
 */
struct spdk_bdev_open_async_opts {
	/* Size of this structure in bytes. */
	size_t size;
                                  /* [한국어] ABI 호환: 호출자 인지 구조체 크기 */
	/*
	 * Time in milliseconds to wait for the block device to appear.
	 *
	 * When the block device does not exist, wait until the block device appears or the timeout
	 * is expired if nonzero, or return immediately otherwise.
	 *
	 * Default value is zero and is used when options are omitted.
	 */
	uint64_t timeout_ms;
                                  /* [한국어] bdev이 아직 등록되지 않았을 때 기다릴 시간 (ms)
                                   *  - 0: 즉시 실패 반환
                                   *  - >0: 등록될 때까지 대기, 초과 시 타임아웃 에러
                                   *  - 용도: probe가 진행 중인 상황에서 사용자 측이 등록 완료를 대기 */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_open_async_opts) == 16, "Incorrect size");
                                  /* [한국어] async opts 16B */

/**
 * Open a block device for I/O operations asynchronously with options.
 *
 * \param bdev_name Block device name to open.
 * \param write true is read/write access requested, false if read-only
 * \param event_cb Notification callback to be called when the bdev triggers
 * asynchronous event such as bdev removal. This will always be called on the
 * same thread that spdk_bdev_open_async() was called on. In case of removal event
 * the descriptor will have to be manually closed to make the bdev unregister
 * proceed.
 * \param event_ctx param for event_cb.
 * \param opts Options for asynchronous block device open. If NULL, default values are used.
 * \param open_cb Open callback.
 * \param open_cb_arg Parameter for open_cb.
 * \return 0 if operation started successfully, suitable errno value otherwise
 */
int spdk_bdev_open_async(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
			 void *event_ctx, struct spdk_bdev_open_async_opts *opts,
			 spdk_bdev_open_async_cb_t open_cb, void *open_cb_arg);
/*
 * [한국어]
 * spdk_bdev_open_async - 비동기 open (bdev 등록 대기 가능)
 *
 * 동기 open_ext와 달리 이 함수는 즉시 반환하고, 열기 결과는 open_cb로 전달.
 * timeout_ms>0이면 bdev 등장 대기 포함 가능. bdev probe와 I/O 경로가 엉키는
 * 상황(예: bdev_nvme attach 도중 RPC 호출)에서 race 없이 open 가능.
 * @return: 0이면 비동기 동작 시작 성공, 음수면 파라미터 오류/자원 부족 등
 */

/**
 * Close a previously opened block device.
 *
 * Must be called on the same thread that the spdk_bdev_open_ext()
 * was performed on.
 *
 * \param desc Block device descriptor to close.
 */
void spdk_bdev_close(struct spdk_bdev_desc *desc);
/*
 * [한국어]
 * spdk_bdev_close - 열려있는 bdev descriptor 해제
 *
 * 제약: 반드시 open을 수행한 **같은 스레드**에서 호출해야 함 (thread affinity)
 *  - 다른 스레드에서 호출하려면 spdk_thread_send_msg로 소유 스레드에 메시지 전송
 *
 * 동작:
 *   - 이 desc와 연결된 I/O channel이 먼저 put 되어야 함 (채널 먼저 해제, desc 나중에)
 *   - open_descs 리스트에서 제거
 *   - 마지막 desc가 close되면 bdev이 unregister 상태로 전이 가능
 *
 * REMOVE 이벤트 수신 시 호출자가 반드시 이 함수를 불러야 bdev unregister가
 * 완료될 수 있다(블로킹 reference count 메커니즘).
 */

/**
 * Get the NUMA node ID for the specified bdev.
 *
 * \param bdev Block device to get the NUMA node ID for
 *
 * \returns NUMA node ID for the bdev, SPDK_ENV_NODE_ID_ANY if the ID
 *	    ID is unknown.
 */
int32_t spdk_bdev_get_numa_id(struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_numa_id - bdev이 부착된 NUMA 노드 ID 반환
 *
 * @return: NUMA 노드 번호 (≥0), 또는 SPDK_ENV_NODE_ID_ANY(=-1) 미지정/모름
 *
 * 사용처:
 *   - 같은 NUMA 노드의 reactor에 채널을 배치하면 메모리 latency 최소화
 *   - DPDK hugepage memory도 가능하면 같은 노드에서 할당 (NUMA-local DMA)
 *   - PCIe 카드는 특정 socket의 RC에 연결 → cross-socket 트래픽 회피
 */

/**
 * Callback function for spdk_for_each_bdev() and spdk_for_each_bdev_leaf().
 *
 * \param ctx Context passed to the callback.
 * \param bdev Block device the callback handles.
 */
typedef int (*spdk_for_each_bdev_fn)(void *ctx, struct spdk_bdev *bdev);
                                  /* [한국어] spdk_for_each_bdev*() 콜백
                                   *  - @return: 0 정상 (다음 bdev 진행), 음수 errno (순회 즉시 중단 + 같은 값 반환) */

/**
 * Call the provided callback function for every registered block device.
 * If fn returns negated errno, spdk_for_each_bdev() terminates iteration.
 *
 * spdk_for_each_bdev() opens before and closes after executing the provided
 * callback function for each bdev internally.
 *
 * \param ctx Context passed to the callback function.
 * \param fn Callback function for each block device.
 *
 * \return 0 if operation is successful, or suitable errno value one of the
 * callback returned otherwise.
 */
int spdk_for_each_bdev(void *ctx, spdk_for_each_bdev_fn fn);
/*
 * [한국어]
 * spdk_for_each_bdev - 모든 등록 bdev에 대해 fn 호출
 *
 * 안전성: 내부에서 각 bdev에 대해 임시 desc를 open/close 처리 → 콜백 실행 중
 * unregister 진행되지 않도록 reference 보호.
 * fn이 음수 반환 시 즉시 중단하고 같은 값 반환.
 * spdk_bdev_first/next 보다 권장 — race-free.
 */

/**
 * Call the provided callback function for every block device without virtual
 * block devices on top.
 *
 * spdk_for_each_bdev_leaf() opens before and closes after executing the provided
 * callback function for each unclaimed bdev internally.
 *
 * \param ctx Context passed to the callback function.
 * \param fn Callback function for each block device without virtual block devices on top.
 *
 * \return 0 if operation is successful, or suitable errno value one of the
 * callback returned otherwise.
 */
int spdk_for_each_bdev_leaf(void *ctx, spdk_for_each_bdev_fn fn);
/*
 * [한국어]
 * spdk_for_each_bdev_leaf - leaf bdev (claim 없는 최상위)만 순회
 * 사용처: 사용자에게 "이 위에 vbdev를 더 쌓을 수 있는 bdev 후보" 목록 제공.
 */

/**
 * Call the provided callback function on block devices with provided names.
 *
 * spdk_for_each_bdev_by_name() stops iteration if bdev with one of provided names does not exist,
 * or if fn returns negated errno.
 *
 * \param ctx Context passed to the callback function.
 * \param fn Callback function for each block device.
 * \param names Array of bdev names to iterate, all of them should exist to finish iteration successfully.
 * \param count Count of bdevs to iterate.
 *
 * \return 0 if operation is successful, or suitable errno value one of the
 * callback returned or -ENODEV if bdev with passed name is not present.
 */
int spdk_for_each_bdev_by_name(void *ctx, spdk_for_each_bdev_fn fn, const char **names,
			       size_t count);
/*
 * [한국어]
 * spdk_for_each_bdev_by_name - 이름 배열로 지정된 bdev들에 대해서만 fn 호출
 *
 * @names: bdev 이름(또는 별칭) 배열, count개. 모두 존재해야 정상 완료
 * @return: -ENODEV 하나라도 없으면, 또는 fn이 음수 반환 시 그 값
 * 사용처: RPC bdev_get_iostat이 일부 bdev만 선택해 통계 조회.
 */

/**
 * Get the bdev associated with a bdev descriptor.
 *
 * \param desc Open block device descriptor
 * \return bdev associated with the descriptor
 */
struct spdk_bdev *spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc);
/*
 * [한국어]
 * spdk_bdev_desc_get_bdev - desc → bdev 포인터 변환
 *
 * 안전성: desc가 valid한 동안 반환 bdev 포인터도 valid (desc가 reference 보유).
 * 사용처: open 후 bdev 속성 조회 시 (get_block_size 등은 bdev 인자 요구).
 */

/**
 * Get logical block size, specific to a bdev descriptor.
 *
 * \param desc Open block device descriptor.
 * \return Size of logical block for this bdev in bytes.
 */
uint32_t spdk_bdev_desc_get_block_size(struct spdk_bdev_desc *desc);
/*
 * [한국어]
 * spdk_bdev_desc_get_block_size - desc 시점의 논리 블록 크기 (바이트)
 *
 * 일반 bdev에서는 spdk_bdev_get_block_size와 동일 결과.
 * 단, hide_metadata=true로 open된 desc에서는 PI 미포함 외부 표시 블록 크기로 보정.
 */

/**
 * Get metadata size, specific to a bdev descriptor.
 *
 * \param desc Open block device descriptor
 * \return Size of metadata for this bdev in bytes.
 */
uint32_t spdk_bdev_desc_get_md_size(struct spdk_bdev_desc *desc);
/*
 * [한국어]
 * spdk_bdev_desc_get_md_size - desc 시점의 메타데이터 크기
 *
 * hide_metadata=true면 0 반환 (사용자에게 메타 노출 안 함).
 * 그 외에는 spdk_bdev_get_md_size와 동일.
 */

/**
 * Query whether metadata is interleaved with block data or separated
 * with block data, specific to a bdev descriptor.
 *
 * Note this function is valid only if there is metadata.
 *
 * \param desc Open block device descriptor.
 * \return true if metadata is interleaved with block data or false
 * if metadata is separated with block data.
 */
bool spdk_bdev_desc_is_md_interleaved(struct spdk_bdev_desc *desc);
/*
 * [한국어]
 * spdk_bdev_desc_is_md_interleaved - 인터리브 메타데이터 모드 여부 (desc 시점)
 *
 * NVMe 스펙 (Identify NS의 FLBAS.MS=0): true=extended LBA (블록 + PI 한 덩어리),
 *                                      false=별도 메타 버퍼 (DPS·MSET=1).
 * hide_metadata=true로 열린 desc에서는 항상 false 반환.
 */

/**
 * Query whether metadata is interleaved with block data or separated
 * from block data, specific to a bdev descriptor
 *
 * Note this function is valid only if there is metadata.
 *
 * \param desc Open block device descriptor.
 * \return true if metadata is separated from block data, false
 * otherwise.
 */
bool spdk_bdev_desc_is_md_separate(struct spdk_bdev_desc *desc);
/*
 * [한국어]
 * spdk_bdev_desc_is_md_separate - 분리 메타 버퍼 모드 여부 (desc 시점)
 *
 * is_md_interleaved의 보수 — true이면 read/write_blocks_with_md API로 별도 md 버퍼 전달.
 */

/**
 * Get DIF type, specific to a bdev descriptor.
 *
 * \param desc Open block device descriptor.
 * \return DIF type of the block device.
 */
enum spdk_dif_type spdk_bdev_desc_get_dif_type(struct spdk_bdev_desc *desc);
/*
 * [한국어]
 * spdk_bdev_desc_get_dif_type - desc의 T10 DIF/PI 타입 (NVMe 스펙: NS Identify DPS.PIT)
 *
 * 반환값:
 *   SPDK_DIF_DISABLE(0): PI 미사용
 *   SPDK_DIF_TYPE1: GUARD + APPTAG + REFTAG(LBA 시작값)
 *   SPDK_DIF_TYPE2: GUARD + APPTAG + REFTAG(EILBRT 사용자 지정)
 *   SPDK_DIF_TYPE3: GUARD + APPTAG (REFTAG 없음, 자유 매핑)
 *
 * NVMe Base Spec 8.3 / SCSI SBC-3 5.2 (Protection Information).
 * hide_metadata=true desc에서는 항상 SPDK_DIF_DISABLE.
 */

/**
 * Get DIF protection information format of the block device, specific to
 * a bdev descriptor.
 *
 * Note that this function is valid only if DIF type is not SPDK_DIF_DISABLE.
 *
 * \param desc Open block device descriptor.
 * \return DIF protection information format of the block device.
 */
enum spdk_dif_pi_format spdk_bdev_desc_get_dif_pi_format(struct spdk_bdev_desc *desc);
/*
 * [한국어]
 * spdk_bdev_desc_get_dif_pi_format - PI 포맷 (16B / 32B / 64B Variant)
 *
 * NVMe 2.0+의 확장 PI: PIF=0 (16-bit GUARD + 16-bit APPTAG + 32-bit REFTAG),
 *                     PIF=1 (32-bit), PIF=2 (64-bit CRC, NVMe Storage Tag 포함).
 * NVM Express Base Spec 5.27.2.1.4 / 5.27.2.1.5.
 * DIF_DISABLE인 bdev에서는 호출 무의미 (반환값 미정).
 */

/**
 * Check whether DIF is set in the first 8/16 bytes or the last 8/16 bytes of metadata,
 * specific to a bdev descriptor.
 *
 * Note that this function is valid only if DIF type is not SPDK_DIF_DISABLE.
 *
 * \param desc Open block device descriptor..
 * \return true if DIF is set in the first 8/16 bytes of metadata, or false
 * if DIF is set in the last 8/16 bytes of metadata.
 */
bool spdk_bdev_desc_is_dif_head_of_md(struct spdk_bdev_desc *desc);
/*
 * [한국어]
 * spdk_bdev_desc_is_dif_head_of_md - PI 위치(메타 앞쪽 vs 뒤쪽)
 *
 * NVMe NS Identify의 DPS.PIP(Protection Information Position):
 *   true(=DPS.PIP=1): 메타 첫 8/16/32B에 PI (앞부분)
 *   false(=DPS.PIP=0): 메타 마지막 8/16/32B에 PI (뒷부분, 기본)
 *
 * spdk_dif 라이브러리가 PI 검사·삽입 시 이 위치를 따라 오프셋 결정.
 */

/**
 * Check whether the DIF check type is enabled, specific to a bdev descriptor.
 *
 * \param desc Open block device descriptor.
 * \param check_type The specific DIF check type.
 * \return true if enabled, false otherwise.
 */
bool spdk_bdev_desc_is_dif_check_enabled(struct spdk_bdev_desc *desc,
		enum spdk_dif_check_type check_type);
/*
 * [한국어]
 * spdk_bdev_desc_is_dif_check_enabled - 특정 DIF 검사 항목 활성 여부 (desc 시점)
 *
 * @check_type:
 *   SPDK_DIF_CHECK_TYPE_REFTAG: REFTAG = LBA(또는 EILBRT) 매칭
 *   SPDK_DIF_CHECK_TYPE_APPTAG: APPTAG (사용자 정의) 매칭
 *   SPDK_DIF_CHECK_TYPE_GUARD: GUARD = CRC-16/CRC-32/CRC-64 of data
 *
 * NVMe Read/Write의 PRINFO 비트와 매핑 — 비활성화된 검사는 SQE에서 PRACT/PRCHK 클리어.
 * 사용처: NVMe-oF 타겟이 호스트 PRCHK 비트와 bdev 정책을 정렬할 때.
 */

/**
 * Set a time limit for the timeout IO of the bdev and timeout callback.
 * We can use this function to enable/disable the timeout handler. If
 * the timeout_in_sec > 0 then it means to enable the timeout IO handling
 * or change the time limit. If the timeout_in_sec == 0 it means to
 * disable the timeout IO handling. If you want to enable or change the
 * timeout IO handle you need to specify the spdk_bdev_io_timeout_cb it
 * means the upper user determines what to do if you meet the timeout IO,
 * for example, you can reset the device or abort the IO.
 * Note: This function must run in the desc's thread.
 *
 * \param desc Block device descriptor.
 * \param timeout_in_sec Timeout value
 * \param cb_fn Bdev IO timeout callback
 * \param cb_arg Callback argument
 *
 * \return 0 on success, negated errno on failure.
 */
int spdk_bdev_set_timeout(struct spdk_bdev_desc *desc, uint64_t timeout_in_sec,
			  spdk_bdev_io_timeout_cb cb_fn, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_set_timeout - I/O 타임아웃 감지 활성/비활성
 *
 * @timeout_in_sec:
 *   - 0: 타임아웃 감지 비활성
 *   - >0: 지정 초 초과 pending I/O에 대해 cb_fn 호출
 * @cb_fn: 타임아웃 발생 시 호출 — 호출측이 abort/reset/로그 등 결정
 * @cb_arg: 콜백 컨텍스트
 *
 * 제약: desc 소유 스레드에서 호출 (thread affinity)
 * 내부: 스레드별 poller가 outstanding bdev_io의 submit_tsc를 확인
 * 사용처: 감지 없이 무한 대기 방지 (드라이버 버그/장치 hang 복구).
 */

/**
 * Check whether the block device supports the I/O type.
 *
 * \param bdev Block device to check.
 * \param io_type The specific I/O type like read, write, flush, unmap.
 * \return true if support, false otherwise.
 */
bool spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type);
/*
 * [한국어]
 * spdk_bdev_io_type_supported - 특정 I/O 타입 지원 여부 조회
 *
 * 제출 전 반드시 확인 권장 — UNMAP/WRITE_ZEROES/COMPARE/ZONE_* 등은 장치/모듈별 차이.
 * 내부: bdev->io_type_supported 비트맵 검사 + 모듈의 fn_table->io_type_supported 콜백.
 */

/**
 * return the name of an IO type based on the io_type.
 *
 * \param io_type The specific I/O type like read, write, flush, unmap.
 * \return Name of the IO type as a null-terminated string.
 */
const char *spdk_bdev_get_io_type_name(enum spdk_bdev_io_type io_type);
/*
 * [한국어]
 * spdk_bdev_get_io_type_name - enum 값 → 사람이 읽는 이름 ("read", "write", "unmap"...)
 * RPC bdev_enable_histogram io_type 인자 파싱과 짝으로 사용.
 */

/**
 * Return the io_type based on the io_type_string.
 *
 * \param io_type_string Name of the IO type as a null-terminated string.
 * \return io_type The specific I/O type like read, write, flush, unmap etc.
 * This will map to enum spdk_bdev_io_type.
 */
int spdk_bdev_get_io_type(const char *io_type_string);
/*
 * [한국어]
 * spdk_bdev_get_io_type - 문자열 → enum spdk_bdev_io_type 변환 (역함수)
 *
 * @return: 매칭 enum 값, 또는 SPDK_BDEV_IO_TYPE_INVALID(0) — 알 수 없는 문자열
 * RPC bdev_enable_histogram에서 사용자가 "io_type":"read"를 보낼 때 enum으로 매핑.
 */

/**
 * Output driver-specific information to a JSON stream.
 *
 * The JSON write context will be initialized with an open object, so the bdev
 * driver should write a name(based on the driver name) followed by a JSON value
 * (most likely another nested object).
 *
 * \param bdev Block device to query.
 * \param w JSON write context. It will store the driver-specific configuration context.
 * \return 0 on success, negated errno on failure.
 */
int spdk_bdev_dump_info_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);
/*
 * [한국어]
 * spdk_bdev_dump_info_json - 모듈별 추가 정보를 JSON object로 직렬화
 *
 * 동작: bdev->fn_table->dump_info_json(ctx, w) 콜백 위임 → 모듈이 자체 키/값 작성
 * 예: NVMe bdev → {"trid": {...}, "ns_data": {...}, "ctrlr_data": {...}}
 *     Malloc bdev → {} (특화 정보 없음)
 * 사용처: RPC bdev_get_bdevs 응답의 "driver_specific" 필드.
 */

/**
 * Get block device name.
 *
 * \param bdev Block device to query.
 * \return Name of bdev as a null-terminated string.
 */
const char *spdk_bdev_get_name(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_name - bdev 본이름 반환 (별칭 아님)
 */

/**
 * Get block device product name.
 *
 * \param bdev Block device to query.
 * \return Product name of bdev as a null-terminated string.
 */
const char *spdk_bdev_get_product_name(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_product_name - 드라이버/제품 식별 문자열 ("NVMe disk", "Malloc disk" 등)
 */

/**
 * Get block device logical block size.
 *
 * \param bdev Block device to query.
 * \return Size of logical block for this bdev in bytes.
 */
uint32_t spdk_bdev_get_block_size(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_block_size - 논리 블록 크기(바이트)
 * LBA·byte 변환에 사용: `byte_offset = lba * block_size`
 */

/**
 * Get the write unit size for this bdev.
 *
 * Write unit size is required number of logical blocks to perform write
 * operation on block device.
 *
 * Unit of write unit size is logical block and the minimum of write unit
 * size is one. Write operations must be multiple of write unit size.
 *
 * \param bdev Block device to query.
 *
 * \return The write unit size in logical blocks.
 */
uint32_t spdk_bdev_get_write_unit_size(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_write_unit_size - 최소 쓰기 단위 (블록 단위, 최소 1)
 *
 * NVMe: Identify NS의 NPWG(Namespace Preferred Write Granularity).
 * write num_blocks는 이 값의 배수여야 함 (위반 시 -EINVAL 또는 split 발생).
 * ZNS의 경우 write_unit_size는 write granularity로 사용 가능.
 */

/**
 * Get size of block device in logical blocks.
 *
 * \param bdev Block device to query.
 * \return Size of bdev in logical blocks.
 *
 * Logical blocks are numbered from 0 to spdk_bdev_get_num_blocks(bdev) - 1, inclusive.
 */
uint64_t spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_num_blocks - 총 블록 수 반환
 *
 * 유효 LBA 범위: [0, num_blocks - 1]
 * 용량 = num_blocks * block_size (bytes)
 * RESIZE 이벤트 수신 후 재호출하여 갱신된 값 조회 권장.
 */

/**
 * Get the string of quality of service rate limit.
 *
 * \param type Type of rate limit to query.
 * \return String of QoS type.
 */
const char *spdk_bdev_get_qos_rpc_type(enum spdk_bdev_qos_rate_limit_type type);
/*
 * [한국어]
 * spdk_bdev_get_qos_rpc_type - QoS 타입을 RPC 이름 문자열로 변환
 * RPC 응답 JSON에서 "rw_ios_per_sec", "rw_mbytes_per_sec" 등의 키 문자열 생성.
 */

/**
 * Get the quality of service rate limits on a bdev.
 *
 * \param bdev Block device to query.
 * \param limits Pointer to the QoS rate limits array which holding the limits.
 *
 * The limits are ordered based on the @ref spdk_bdev_qos_rate_limit_type enum.
 */
void spdk_bdev_get_qos_rate_limits(struct spdk_bdev *bdev, uint64_t *limits);
/*
 * [한국어]
 * spdk_bdev_get_qos_rate_limits - 현재 QoS 제한값 4개 조회
 *
 * @limits: SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES 크기 배열 (인덱스는 enum 순서)
 *   [0] RW_IOPS_RATE_LIMIT   (IOPS)
 *   [1] RW_BPS_RATE_LIMIT    (bytes/sec)
 *   [2] R_BPS_RATE_LIMIT     (read-only)
 *   [3] W_BPS_RATE_LIMIT     (write-only)
 * 값 0 = 해당 제한 비활성.
 */

/**
 * Set the quality of service rate limits on a bdev.
 *
 * \param bdev Block device.
 * \param limits Pointer to the QoS rate limits array which holding the limits.
 * \param cb_fn Callback function to be called when the QoS limit has been updated.
 * \param cb_arg Argument to pass to cb_fn.
 *
 * The limits are ordered based on the @ref spdk_bdev_qos_rate_limit_type enum.
 */
void spdk_bdev_set_qos_rate_limits(struct spdk_bdev *bdev, uint64_t *limits,
				   void (*cb_fn)(void *cb_arg, int status), void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_set_qos_rate_limits - QoS 제한 4종 일괄 적용
 *
 * @limits: 새 제한값 배열 (get_qos_rate_limits와 같은 순서)
 *   각 값 0 = 해당 제한 끔
 * @cb_fn: 적용 완료 통지 (IOPS 버킷 재초기화 등 비동기 처리 후 호출)
 *
 * 동시 set 요청은 직렬화 — bdev->internal.qos_mod_in_progress 플래그로 보호.
 * 적용 방식: 토큰 버킷 알고리즘 (1ms 주기 poller가 토큰 재충전).
 */

/**
 * Get minimum I/O buffer address alignment for a bdev.
 *
 * \param bdev Block device to query.
 * \return Required alignment of I/O buffers in bytes.
 */
size_t spdk_bdev_get_buf_align(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_buf_align - 최소 I/O 버퍼 정렬(바이트) 반환
 *
 * 반환값의 배수로 정렬된 버퍼를 제출하면 bounce buffer 우회 → 최고 성능.
 * 미정렬 버퍼는 bdev 코어가 내부 복사 경로로 우회 (성능 저하, 추가 메모리).
 * 사용 예: `posix_memalign(&buf, spdk_bdev_get_buf_align(bdev), size)`
 */

/**
 * Get optimal I/O boundary for a bdev.
 *
 * \param bdev Block device to query.
 * \return Optimal I/O boundary in blocks that should not be crossed for best performance, or 0 if
 *         no optimal boundary is reported.
 */
uint32_t spdk_bdev_get_optimal_io_boundary(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_optimal_io_boundary - 최적 I/O 경계 (블록 단위, 0=없음)
 *
 * NVMe: Identify NS의 NOIOB(Namespace Optimal I/O Boundary).
 * 이 경계를 가로지르는 I/O는 두 개의 내부 명령으로 분할되어 latency 증가 가능.
 * bdev 코어가 자동 split: I/O가 boundary를 넘으면 [start, boundary)와 [boundary, end)로 분리.
 * 사용자는 align/size를 boundary에 맞추면 split 없이 단일 명령으로 처리.
 */

/**
 * Query whether block device has an enabled write cache.
 *
 * \param bdev Block device to query.
 * \return true if block device has a volatile write cache enabled.
 *
 * If this function returns true, written data may not be persistent until a flush command
 * is issued.
 */
bool spdk_bdev_has_write_cache(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_has_write_cache - 휘발성 쓰기 캐시 존재 여부
 *
 * true이면 write 완료 후에도 flush 전까지 데이터가 휘발 가능 →
 * 내구성이 필요한 경우 spdk_bdev_flush_blocks() 호출 필요.
 * false(write-through)이면 flush 호출 불필요 — 성능 최적화로 생략 가능.
 */

/**
 * Get a bdev's UUID.
 *
 * \param bdev Block device to query.
 * \return Pointer to UUID.
 *
 * All bdevs will have a UUID, but not all UUIDs will be persistent across
 * application runs.
 */
const struct spdk_uuid *spdk_bdev_get_uuid(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_uuid - bdev의 UUID(128-bit) 포인터 반환
 *
 * 모든 bdev은 UUID를 가짐 — 단, 영구성 보장 없음:
 *   - NVMe: NGUID/EUI64 → UUID 변환 (장치 영구)
 *   - lvol: blob UUID (블롭스토어 영구)
 *   - malloc: 매 등록마다 새로 생성 (휘발)
 *
 * 사용처: 이름이 변할 수 있는 환경에서 bdev 식별 (config 영속화).
 * 반환 포인터는 bdev 수명 동안 유효 (변경되지 않음).
 */

/**
 * Get block device atomic compare and write unit.
 *
 * \param bdev Block device to query.
 * \return Atomic compare and write unit for this bdev in blocks.
 */
uint16_t spdk_bdev_get_acwu(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_acwu - Atomic Compare-and-Write Unit (블록 단위)
 *
 * NVMe: Identify NS의 NACWU(Namespace Atomic Compare and Write Unit) +1.
 * 이 크기 이내의 COMPARE+WRITE fused 명령은 장치가 원자성 보장.
 * 분산 합의(consensus) 알고리즘이 lock 없이 update 가능한 최대 단위.
 * 0이면 보장 없음 (소프트웨어 락 필요).
 */

/**
 * Get block device metadata size.
 *
 * \param bdev Block device to query.
 * \return Size of metadata for this bdev in bytes.
 */
uint32_t spdk_bdev_get_md_size(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_md_size - 메타데이터 바이트/블록
 *
 * 0이면 메타 없음. 일반적으로 8B(PI Type 1/2/3) 또는 16B(extended PI).
 * NVMe NS Identify의 LBADS와 함께 사용 (LBA Format).
 * is_md_interleaved=true면 block_size에 이 값이 포함, false면 별도 버퍼 필요.
 */

/**
 * Query whether metadata is interleaved with block data or separated
 * with block data.
 *
 * Note this function is valid only if there is metadata.
 *
 * \param bdev Block device to query.
 * \return true if metadata is interleaved with block data or false
 * if metadata is separated with block data.
 */
bool spdk_bdev_is_md_interleaved(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_is_md_interleaved - 인터리브(extended LBA) 모드 여부
 *
 * NVMe Identify NS의 FLBAS.MS=1 (Metadata Settings: extended).
 * true: 블록과 PI가 한 버퍼에 순차 저장 (예: 4096B + 64B PI = 4160B 단위)
 * false: 분리 (read_blocks_with_md API로 별도 md 버퍼 전달)
 *
 * 호스트 메모리 사용량과 PI 검증 경로(데이터 복사 vs 분리 가능)에 영향.
 */

/**
 * Query whether metadata is interleaved with block data or separated
 * from block data.
 *
 * Note this function is valid only if there is metadata.
 *
 * \param bdev Block device to query.
 * \return true if metadata is separated from block data, false
 * otherwise.
 */
bool spdk_bdev_is_md_separate(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_is_md_separate - 분리 메타 모드 여부 (is_md_interleaved의 보수)
 */

/**
 * Checks if bdev supports zoned namespace semantics.
 *
 * \param bdev Block device to query.
 * \return true if device supports zoned namespace semantics.
 */
bool spdk_bdev_is_zoned(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_is_zoned - ZNS(Zoned Namespace) 장치 여부
 *
 * true이면 blockcnt는 zone_size 배수, zone_append/zone_mgmt API 사용 가능.
 * 일반 SSD처럼 임의 offset 쓰기는 불가 — 각 zone은 sequential write 강제.
 */

/**
 * Get block device data block size.
 *
 * Data block size is equal to block size if there is no metadata or
 * metadata is separated with block data, or equal to block size minus
 * metadata size if there is metadata and it is interleaved with
 * block data.
 *
 * \param bdev Block device to query.
 * \return Size of data block for this bdev in bytes.
 */
uint32_t spdk_bdev_get_data_block_size(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_data_block_size - 메타 제외 순수 데이터 블록 크기
 *
 * 계산:
 *   - 메타 없음 또는 분리(separate): block_size 그대로 (예: 4096B)
 *   - 인터리브: block_size - md_size (예: 4160 - 64 = 4096B)
 *
 * 사용자 데이터 영역 크기 계산용 — 파일시스템/blobstore가 실제 페이로드 크기로 사용.
 */

/**
 * Get block device physical block size.
 *
 * \param bdev Block device to query.
 * \return Size of physical block size for this bdev in bytes.
 */
uint32_t spdk_bdev_get_physical_block_size(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_physical_block_size - 물리 블록 크기 (PMA, atomic write 단위)
 *
 * NVMe: Identify NS의 NPDG(Namespace Preferred Deallocate Granularity)/NPWA 등을 종합.
 * 일반: 4096B SSD가 logical=512B로 표시되더라도 physical=4096B.
 * 정렬되지 않은 쓰기는 read-modify-write를 유발 → 성능·내구성 저하.
 */

/**
 * Get block device preferred write alignment.
 *
 * \param bdev Block device to query.
 * \return preferred write alignment for this bdev in blocks. Value 0 means there is no preferred write alignment.
 */
uint32_t spdk_bdev_get_preferred_write_alignment(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_preferred_write_alignment - 권장 쓰기 정렬 (블록 단위, 0=없음)
 *
 * NVMe: Identify NS의 NPWA(Namespace Preferred Write Alignment).
 * 이 값의 배수 LBA로 시작하는 쓰기가 RMW 없이 처리.
 */

/**
 * Get block device preferred write granularity.
 *
 * \param bdev Block device to query.
 * \return preferred write granularity for this bdev in blocks. Value 0 means there is no preferred write granularity.
 */
uint32_t spdk_bdev_get_preferred_write_granularity(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_preferred_write_granularity - 권장 쓰기 단위 (블록 단위, 0=없음)
 *
 * NVMe: Identify NS의 NPWG. 이 값 배수 길이의 쓰기가 최적.
 * 예: NPWG=8(=4KB at 512B blocks)일 때 4KB 단위 쓰기가 권장.
 */

/**
 * Get block device optimal write size.
 *
 * \param bdev Block device to query.
 * \return preferred write size for this bdev in blocks. Value 0 means there is no preferred write size.
 */
uint32_t spdk_bdev_get_optimal_write_size(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_optimal_write_size - 최적 쓰기 크기 (블록 단위, 0=없음)
 *
 * NVMe: Identify NS의 NOWS(Namespace Optimal Write Size).
 * 이 크기로 쓰면 write amplification 최소화 (NAND erase block 정합).
 */

/**
 * Get block device preferred unmap alignment.
 *
 * \param bdev Block device to query.
 * \return preferred unmap alignment for this bdev in blocks. Value 0 means there is no preferred unmap alignment.
 */
uint32_t spdk_bdev_get_preferred_unmap_alignment(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_preferred_unmap_alignment - 권장 UNMAP/Deallocate 정렬 (블록 단위)
 * NVMe Identify NS의 NPDA(Namespace Preferred Deallocate Alignment).
 * 정렬된 UNMAP만 실제 NAND erase로 변환됨 (미정렬은 mark only).
 */

/**
 * Get block device preferred unmap granularity.
 *
 * \param bdev Block device to query.
 * \return preferred unmap granularity for this bdev in blocks. Value 0 means there is no preferred unmap granularity.
 */
uint32_t spdk_bdev_get_preferred_unmap_granularity(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_preferred_unmap_granularity - 권장 UNMAP 길이 단위 (블록 단위)
 * NVMe Identify NS의 NPDG(Namespace Preferred Deallocate Granularity).
 * 이 단위 배수의 UNMAP만 실제 GC trigger.
 */

/**
 * Get DIF type of the block device.
 *
 * \param bdev Block device to query.
 * \return DIF type of the block device.
 */
enum spdk_dif_type spdk_bdev_get_dif_type(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_dif_type - bdev의 PI Type (NVMe NS Identify DPS.PIT)
 *
 * SPDK_DIF_DISABLE / TYPE1 / TYPE2 / TYPE3.
 * desc 기반(spdk_bdev_desc_get_dif_type)이 호출자에 권장 — hide_metadata 지원.
 * NVMe Base Spec 5.27.2.1.4 (PI Field Position and Format).
 */

/**
 * Get DIF protection information format of the block device.
 *
 * Note that this function is valid only if DIF type is not SPDK_DIF_DISABLE.
 *
 * \param bdev Block device to query.
 * \return DIF protection information format of the block device.
 */
enum spdk_dif_pi_format spdk_bdev_get_dif_pi_format(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_dif_pi_format - PI 포맷 (NVMe NS Identify DPS.PIF)
 *
 * 16B(PIF=0): GUARD16 + APPTAG16 + REFTAG32
 * 32B(PIF=1): GUARD16 + APPTAG16 + STORAGETAG48 + REFTAG48 (NVMe 2.0)
 * 64B(PIF=2): GUARD64 + APPTAG16 + REFTAG48 (CRC64-NVMe, ZNS·DSM)
 *
 * 미사용 시(DISABLE) 호출은 의미 없음 (반환값 불정).
 */

/**
 * Check whether DIF is set in the first 8/16 bytes or the last 8/16 bytes of metadata.
 *
 * Note that this function is valid only if DIF type is not SPDK_DIF_DISABLE.
 *
 * \param bdev Block device to query.
 * \return true if DIF is set in the first 8/16 bytes of metadata, or false
 * if DIF is set in the last 8/16 bytes of metadata.
 */
bool spdk_bdev_is_dif_head_of_md(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_is_dif_head_of_md - PI 위치 (메타 영역 head vs tail)
 *
 * NVMe DPS.PIP(Protection Information Position):
 *   true(=PIP=1): 메타 앞쪽에 PI
 *   false(=PIP=0): 메타 뒤쪽에 PI (기본)
 *
 * spdk_dif_ctx 빌드 시 PI 오프셋 결정에 사용.
 */

/**
 * Check whether the DIF check type is enabled.
 *
 * \param bdev Block device to query.
 * \param check_type The specific DIF check type.
 * \return true if enabled, false otherwise.
 */
bool spdk_bdev_is_dif_check_enabled(const struct spdk_bdev *bdev,
				    enum spdk_dif_check_type check_type);
/*
 * [한국어]
 * spdk_bdev_is_dif_check_enabled - 특정 PI 검사 종류 활성화 여부
 *
 * @check_type:
 *   SPDK_DIF_CHECK_TYPE_GUARD: CRC 검증
 *   SPDK_DIF_CHECK_TYPE_APPTAG: 사용자 정의 태그 검증
 *   SPDK_DIF_CHECK_TYPE_REFTAG: 참조 태그(LBA 기반) 검증
 *
 * NVMe SQE의 PRINFO/PRCHK 비트와 매핑. Type 3는 REFTAG 검사 자동 비활성.
 * 사용처: bdev 모듈이 자기 dif_ctx 초기화 시 어떤 검사 항목을 켤지 결정.
 */

/**
 * Get block device max copy size.
 *
 * \param bdev Block device to query.
 * \return Max copy size for this bdev in blocks. 0 means unlimited.
 */
uint32_t spdk_bdev_get_max_copy(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_max_copy - 단일 COPY 명령의 최대 블록 수 (0=제한 없음)
 *
 * NVMe Simple Copy Command(MSSRL: Maximum Source Range Length).
 * spdk_bdev_copy_blocks가 num_blocks > max_copy인 요청을 자동 split.
 * 0이면 split 없이 원-샷 전송 (장치가 무제한 지원).
 */

/**
 * Get the most recently measured queue depth from a bdev.
 *
 * The reported queue depth is the aggregate of outstanding I/O
 * across all open channels associated with this bdev.
 *
 * \param bdev Block device to query.
 *
 * \return The most recent queue depth measurement for the bdev.
 * If tracking is not enabled, the function will return UINT64_MAX
 * It is also possible to receive UINT64_MAX after enabling tracking
 * but before the first period has expired.
 */
uint64_t
spdk_bdev_get_qd(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_qd - 최근 측정된 queue depth 반환
 *
 * "현재 장치에 in-flight인 I/O 총수"의 근사치. 모든 채널 집계.
 * 반환 UINT64_MAX = 추적 비활성 또는 첫 측정 전. bdev_set_qd_sampling_period로 활성화 후 기다림 필요.
 * 사용처: 로드 기반 스케줄링, 성능 모니터링, RPC bdev_get_iostat 응답.
 */

/**
 * Get the queue depth polling period.
 *
 * The return value of this function is only valid if the bdev's
 * queue depth tracking status is set to true.
 *
 * \param bdev Block device to query.
 *
 * \return The period at which this bdev's gueue depth is being refreshed.
 */
uint64_t
spdk_bdev_get_qd_sampling_period(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_qd_sampling_period - QD 샘플링 주기(틱 단위) 조회
 *
 * 0이면 샘플링 비활성. 활성 상태에서만 spdk_bdev_get_qd 결과가 의미 있음.
 * 사용처: RPC bdev_get_iostat에서 측정 주기 보고.
 */

/**
 * Enable or disable queue depth sampling for this bdev.
 *
 * Enables queue depth sampling when period is greater than 0. Disables it when the period
 * is equal to zero. The resulting queue depth is stored in the spdk_bdev object as
 * measured_queue_depth.
 *
 * \param bdev Block device on which to enable queue depth tracking.
 * \param period The period at which to poll this bdev's queue depth. If this is set
 * to zero, polling will be disabled.
 */
void spdk_bdev_set_qd_sampling_period(struct spdk_bdev *bdev, uint64_t period);
/*
 * [한국어]
 * spdk_bdev_set_qd_sampling_period - QD 샘플링 활성화/비활성/주기 변경
 *
 * @period: 0=비활성, >0=마이크로초(또는 tick) 단위 샘플링 주기
 *
 * 동작: poller가 등록되어 주기마다 모든 채널의 outstanding 카운트를 합산해 measured_queue_depth 갱신.
 * 부가 효과: 샘플 시점마다 io_time / weighted_io_time도 누적.
 * 비용: 샘플링 주기마다 모든 채널 메시지 → 짧은 주기는 CPU 비용 증가.
 */

/**
 * Get the time spent processing IO for this device.
 *
 * This value is dependent upon the queue depth sampling period and is
 * incremented at sampling time by the sampling period only if the measured
 * queue depth is greater than 0.
 *
 * The disk utilization can be calculated by the following formula:
 * disk_util = (io_time_2 - io_time_1) / elapsed_time.
 * The user is responsible for tracking the elapsed time between two measurements.
 *
 * \param bdev Block device to query.
 *
 * \return The io time for this device in microseconds.
 */
uint64_t spdk_bdev_get_io_time(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_io_time - 누적 I/O 처리 시간(마이크로초)
 *
 * 정의: 샘플링 시점에 measured_qd>0이면 sampling_period만큼 가산.
 * 디스크 사용률 = (io_time_2 - io_time_1) / elapsed_time (Linux iostat의 %util과 동일 의미).
 * iostat-style 계산: 호출자가 두 시점에 호출하여 차이 계산 + 자체 elapsed 측정.
 */

/**
 * Get the weighted IO processing time for this bdev.
 *
 * This value is dependent upon the queue depth sampling period and is
 * equal to the time spent reading from or writing to a device times
 * the measured queue depth during each sampling period.
 *
 * The average queue depth can be calculated by the following formula:
 * queue_depth = (weighted_io_time_2 - weighted_io_time_1) / elapsed_time.
 * The user is responsible for tracking the elapsed time between two measurements.
 *
 * \param bdev Block device to query.
 *
 * \return The weighted io time for this device in microseconds.
 */
uint64_t spdk_bdev_get_weighted_io_time(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_weighted_io_time - 가중 I/O 시간 (마이크로초 × measured_qd)
 *
 * 정의: 매 샘플마다 sampling_period × measured_qd 가산.
 * 평균 QD = (weighted_io_time_2 - weighted_io_time_1) / elapsed_time.
 * Linux iostat의 aveq(uint avqu-sz) 계산과 동등 — 시간 가중 평균 큐 길이 도출.
 */

/**
 * Obtain an I/O channel for the block device opened by the specified
 * descriptor. I/O channels are bound to threads, so the resulting I/O
 * channel may only be used from the thread it was originally obtained
 * from.
 *
 * \param desc Block device descriptor.
 *
 * \return A handle to the I/O channel or NULL on failure.
 */
struct spdk_io_channel *spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc);
/*
 * [한국어]
 * spdk_bdev_get_io_channel - 호출 스레드용 I/O 채널 획득 (★ I/O 경로 필수 단계 ★)
 *
 * @desc: open된 descriptor
 * @return: spdk_io_channel 핸들 (이후 모든 I/O API의 @ch 파라미터로 전달). 실패 시 NULL
 *
 * 스레드 고정 규칙:
 *   - 반환된 채널은 이 함수를 호출한 SPDK thread에만 귀속
 *   - 다른 스레드에서 같은 bdev에 I/O를 발행하려면 각 스레드가 별도로 이 함수를 호출해 자기 채널 획득
 *   - 사용 후 spdk_put_io_channel(ch)로 해제 (desc close 전에 반드시)
 *
 * 내부 효과:
 *   - bdev 모듈의 fn_table->get_io_channel(ctx) 콜백 호출 → trailing ctx 채움
 *   - NVMe 모듈의 경우: nvme qpair 하나를 이 채널에 연결 → 이 스레드에서 발행되는
 *     모든 I/O가 해당 qpair로 제출됨
 *
 * 전형 사용 패턴:
 *   spdk_bdev_open_ext(name, true, event_cb, ctx, &desc);
 *   ch = spdk_bdev_get_io_channel(desc);
 *   spdk_bdev_read_blocks(desc, ch, buf, lba, nblk, cb, cb_arg);
 *   // ... 완료 콜백 ...
 *   spdk_put_io_channel(ch);
 *   spdk_bdev_close(desc);
 */

/**
 * Obtain a bdev module context for the block device opened by the specified
 * descriptor.
 *
 * \param desc Block device descriptor.
 *
 * \return A bdev module context or NULL on failure.
 */
void *spdk_bdev_get_module_ctx(struct spdk_bdev_desc *desc);
/*
 * [한국어]
 * spdk_bdev_get_module_ctx - bdev 모듈별 컨텍스트 노출 (진단/고급 용도)
 *
 * fn_table->get_module_ctx(bdev->ctxt) 경유. 일반 I/O 경로에서는 사용 불필요.
 * NVMe 모듈 등에서 특정 진단/튜닝 RPC가 원시 컨텍스트(예: nvme_ctrlr)에 접근할 때 사용.
 */

/**
 * \defgroup bdev_io_submit_functions bdev I/O Submit Functions
 *
 * These functions submit a new I/O request to a bdev.  The I/O request will
 *  be represented by an spdk_bdev_io structure allocated from a global pool.
 *  These functions will return -ENOMEM if the spdk_bdev_io pool is empty.
 */
/*
 * [한국어] ===== bdev I/O 제출 함수 그룹 =====
 *
 * 공통 규칙:
 *   - 각 호출은 새 spdk_bdev_io(채널 풀에서)를 할당
 *   - 풀 고갈 시 -ENOMEM 반환 → 호출자가 spdk_bdev_queue_io_wait로 대기 등록 후 재시도 가능
 *   - 반환 0이면 제출 성공 — 완료(정상/에러 모두)는 반드시 cb로 통지됨
 *   - 반환 ≠0이면 cb는 **호출되지 않음** (호출자가 오류 처리 책임)
 *   - @ch는 @desc와 같은 스레드에서 얻은 채널이어야 함
 *   - 완료 cb 내에서 spdk_bdev_free_io(bdev_io) 호출 필수
 *
 * 이름 규약:
 *   - *_read / _write         : byte 오프셋·길이 단위 (offset/nbytes) — 내부에서 블록 단위로 변환
 *   - *_read_blocks / _write_blocks : 블록 단위 직접 지정 (offset_blocks/num_blocks) — 권장
 *   - *_readv / _writev       : 단일 버퍼 대신 iovec scatter-gather
 *   - *_with_md               : 분리 메타데이터 버퍼 동시 전송 (PI)
 *   - *_ext                   : 확장 옵션 구조체(memory_domain, accel_sequence 등) 전달
 */

/**
 * Submit a data seek request to the bdev on the given channel.
 * Starting from offset_blocks, search for next allocated data:
 * seek result can be obtained with spdk_bdev_io_get_seek_offset
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks is out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_seek_data(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t offset_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_seek_data - ★ thin-provisioned 영역에서 다음 "데이터 있는" LBA 탐색 ★
 *
 * @desc: open된 descriptor
 * @ch: I/O 채널 (같은 스레드)
 * @offset_blocks: 탐색 시작 LBA
 * @cb: 완료 콜백 — 결과는 spdk_bdev_io_get_seek_offset(bdev_io)로 조회
 * @cb_arg: 콜백 컨텍스트
 * @return: 0 제출 성공 (cb 호출 보장) / -EINVAL 범위 위반 / -ENOMEM 풀 고갈 /
 *          -ENOTSUP 모듈 미지원 (sparse 미지원 bdev: malloc, raid 등)
 *
 * 의미: POSIX의 lseek(SEEK_DATA)와 동일 — offset_blocks 이후 첫 번째 할당된 데이터 위치 반환.
 *       모두 hole이면 UINT64_MAX 반환 (get_seek_offset 결과).
 *
 * NVMe Dataset Management(DSM) 정보 또는 lvol blob의 cluster 할당 비트맵 활용.
 * 사용처: rsync-style 스파스 복사, dm-thin migration, blob walker.
 *
 * 호출 체인:
 *   사용자 → spdk_bdev_seek_data → bdev_io_submit(SEEK_DATA) →
 *   모듈 submit_request → 결과를 bdev_io.u.bdev.offset_blocks에 저장 →
 *   완료 cb 내부 spdk_bdev_io_get_seek_offset로 회수 → spdk_bdev_free_io
 */

/**
 * Submit a hole seek request to the bdev on the given channel.
 * Starting from offset_blocks, search for next unallocated hole:
 * seek result can be obtained with spdk_bdev_io_get_seek_offset
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks is out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_seek_hole(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t offset_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_seek_hole - ★ thin-provisioned 영역에서 다음 "데이터 없는(hole)" LBA 탐색 ★
 *
 * lseek(SEEK_HOLE) 등가 — offset_blocks 이후 첫 번째 미할당(unmapped) 영역 시작 LBA 반환.
 * hole이 없으면 (= 끝까지 모두 할당) num_blocks 반환 (POSIX 관례: EOF 직전).
 * 결과 회수: 완료 cb 내부 spdk_bdev_io_get_seek_offset(bdev_io).
 *
 * seek_data와 짝으로 사용:
 *   uint64_t off = 0;
 *   while (off < num_blocks) {
 *       off = seek_data(off);   // 다음 데이터 시작
 *       end = seek_hole(off);   // 그 데이터 영역의 끝
 *       copy(off..end);
 *       off = end;
 *   }
 * → 데이터가 없는 영역은 자동 skip → 스파스 효율 복사.
 *
 * @return 추가: -ENOTSUP — 모듈/장치가 sparse를 지원하지 않음.
 */

/**
 * Submit a read request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to read into.
 * \param offset The offset, in bytes, from the start of the block device.
 * \param nbytes The number of bytes to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		   void *buf, uint64_t offset, uint64_t nbytes,
		   spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_read - ★ 기본 읽기 API (byte 단위) ★
 *
 * @desc: open 결과
 * @ch: get_io_channel 결과 (같은 스레드)
 * @buf: 읽기 대상 메모리 버퍼. bdev->required_alignment 미충족 시 내부 bounce buffer 경유 (성능 저하)
 * @offset: bdev 시작부터의 바이트 오프셋. blocklen 정렬 필수
 * @nbytes: 읽을 바이트 수. blocklen 배수 필수
 * @cb: 완료 콜백 (성공/실패 모두 통지)
 * @cb_arg: cb 컨텍스트
 * @return:
 *   0: 제출 성공 (cb가 반드시 호출됨)
 *  -EINVAL: 정렬·범위 위반
 *  -ENOMEM: bdev_io 풀 고갈 — spdk_bdev_queue_io_wait로 재시도 등록 가능
 *  그 외 음수: 모듈 검증 실패 등
 *
 * 내부 흐름:
 *   bdev_channel_get_io → bdev_io_init(READ) → u.bdev 채움 → fn_table.submit_request
 *   → 모듈이 장치로 디스패치 → 완료 시 cb 호출 (같은 스레드)
 *
 * 권장: blocks 버전 `spdk_bdev_read_blocks` 사용 (정렬 체크 불필요, 명시적)
 */

/**
 * Submit a read request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to read into.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			  spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_read_blocks - ★ 권장 읽기 API (블록 단위) ★
 *
 * @offset_blocks: bdev 시작부터의 LBA
 * @num_blocks: 읽을 블록 수
 *
 * 사용자 코드가 직접 블록 단위로 표현 → byte 버전의 정렬 체크 비용 없음,
 * 의도가 명확 (blocklen=512면 512B, blocklen=4096이면 4KB).
 *
 * 내부 처리:
 *   - num_blocks가 max_rw_size 초과 시 bdev 코어가 자동 split (parent/child bdev_io)
 *   - required_alignment 미충족 buf는 bounce buffer 경유 (모듈로 가기 전)
 *   - QoS rate limit 적용되면 채널 대기 큐에 일시 보관 후 조건부 제출
 */

/**
 * Submit a read request to the bdev on the given channel. This function uses
 * separate buffer for metadata transfer (valid only if bdev supports this
 * mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to read into.
 * \param md Metadata buffer.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_read_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				  void *buf, void *md, uint64_t offset_blocks, uint64_t num_blocks,
				  spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_read_blocks_with_md - 분리 메타데이터 버퍼 경로 읽기
 *
 * @md: 메타데이터 전용 버퍼 (PI 포함). 크기 = num_blocks * bdev->md_len
 *
 * 전제: bdev->md_interleave == false (별도 버퍼 지원). interleave된 bdev에 호출 시 -EINVAL.
 * 사용처: NVMe PI type1/2/3 검증, T10 DIF 경로.
 */

/**
 * Submit a read request to the bdev on the given channel. This differs from
 * spdk_bdev_read by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer into the buffers provided. In
 * this case, the request may fail.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be read into.
 * \param iovcnt The number of elements in iov.
 * \param offset The offset, in bytes, from the start of the block device.
 * \param nbytes The number of bytes to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_readv(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    struct iovec *iov, int iovcnt,
		    uint64_t offset, uint64_t nbytes,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a read request to the bdev on the given channel. This differs from
 * spdk_bdev_read by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer into the buffers provided. In
 * this case, the request may fail.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be read into.
 * \param iovcnt The number of elements in iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_readv_blocks - scatter-gather 읽기 (블록 단위)
 *
 * @iov: 연속되지 않은 버퍼들의 iovec 배열
 * @iovcnt: 배열 크기 — bdev->max_num_segments 초과 시 -EINVAL
 *
 * 각 iov[i].iov_base는 bdev->required_alignment 충족 권장 (미충족 시 bounce buffer).
 * 여러 애플리케이션 버퍼를 하나의 I/O로 묶을 수 있어 지연 최소화.
 * 합계 길이(iov_len의 합)는 num_blocks * blocklen과 정확히 일치해야 함.
 */

/**
 * Submit a read request to the bdev on the given channel. This differs from
 * spdk_bdev_read by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data or metadata and may not be able to directly transfer into the buffers
 * provided. In this case, the request may fail. This function uses separate
 * buffer for metadata transfer (valid only if bdev supports this mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be read into.
 * \param iovcnt The number of elements in iov.
 * \param md Metadata buffer.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_readv_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				   struct iovec *iov, int iovcnt, void *md,
				   uint64_t offset_blocks, uint64_t num_blocks,
				   spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_readv_blocks_with_md - scatter-gather 데이터 + 연속 메타 버퍼 읽기
 * 데이터는 iov로 분산 수신, 메타데이터(PI 등)는 @md가 가리키는 연속 버퍼로 수신.
 * 전제: bdev->md_interleave == false.
 */

/**
 * Submit a read request to the bdev on the given channel. This differs from
 * spdk_bdev_read by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data or metadata and may not be able to directly transfer into the buffers
 * provided. In this case, the request may fail. This function uses separate
 * buffer for metadata transfer (valid only if bdev supports this mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be read into.
 * \param iovcnt The number of elements in iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 * \param opts Optional structure with extended IO request options. `size` member of this structure
 *             is used for ABI compatibility and must be set to sizeof(struct spdk_bdev_ext_io_opts).
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported or opts_size is incorrect
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_readv_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			       struct iovec *iov, int iovcnt, uint64_t offset_blocks,
			       uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg,
			       struct spdk_bdev_ext_io_opts *opts);
/*
 * [한국어]
 * spdk_bdev_readv_blocks_ext - readv_blocks + 확장 옵션
 *
 * @opts: 메모리 도메인 / accel_sequence / metadata(md_buf/md_size) 등 전달
 *        - opts_size 필수 (ABI 호환 체크). sizeof(struct spdk_bdev_ext_io_opts) 전달
 *        - NULL 전달 시 readv_blocks와 동일 동작
 *
 * 사용처: NVMe-oF target 등 RDMA 메모리 도메인이나 DMA 엔진 오프로드가 필요할 때.
 */

/**
 * Submit a write request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to written from.
 * \param offset The offset, in bytes, from the start of the block device.
 * \param nbytes The number of bytes to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_write(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    void *buf, uint64_t offset, uint64_t nbytes,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_write - ★ 기본 쓰기 API (byte 단위) ★
 *
 * @buf: 쓸 데이터 버퍼. nbytes 이상 유효해야 함
 * @offset, @nbytes: blocklen 정렬 필수
 * @return 추가: -EBADF — desc가 R/O로 열린 경우
 *
 * write 경로는 read와 거의 동일하나 추가 제약:
 *   - desc가 write=true로 open 되어야 함 (open_ext의 write 인자)
 *   - QoS write BPS 제한이 걸리면 채널 대기 큐에 적재
 *   - FLUSH/FUA 등 추가 플래그는 별도 API 또는 bdev_io 플래그로 전달
 *
 * 권장: blocks 버전 사용.
 */

/**
 * Submit a write request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to written from.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_write_blocks - ★ 권장 쓰기 API (블록 단위) ★
 *
 * @buf: num_blocks * blocklen 바이트 이상의 유효 버퍼
 * @offset_blocks: 시작 LBA
 * @num_blocks: 쓸 블록 수
 *
 * read_blocks와 대칭. max_rw_size 초과 시 자동 split, write_unit_size 경계
 * 교차 시(split_on_write_unit=true일 때) 강제 분할. bounce buffer 조건도 동일.
 *
 * 호출 체인:
 *   spdk_bdev_write_blocks → bdev_channel_get_io → bdev_io_init(WRITE)
 *   → u.bdev.iovs[0]={buf,len} 채움 → fn_table.submit_request
 *   → 모듈(bdev_nvme 등) → NVMe WRITE 커맨드(opcode 0x01) 빌드 → qpair 제출
 */

/**
 * Submit a write request to the bdev on the given channel. This function uses
 * separate buffer for metadata transfer (valid only if bdev supports this
 * mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to written from.
 * \param md Metadata buffer.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_write_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				   void *buf, void *md, uint64_t offset_blocks, uint64_t num_blocks,
				   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write request to the bdev on the given channel. This differs from
 * spdk_bdev_write by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer out of the buffers provided. In
 * this case, the request may fail.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iov.
 * \param offset The offset, in bytes, from the start of the block device.
 * \param len The size of data to write.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_writev(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		     struct iovec *iov, int iovcnt,
		     uint64_t offset, uint64_t len,
		     spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write request to the bdev on the given channel. This differs from
 * spdk_bdev_write by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer out of the buffers provided. In
 * this case, the request may fail.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to write.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    struct iovec *iov, int iovcnt,
			    uint64_t offset_blocks, uint64_t num_blocks,
			    spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_writev_blocks - scatter-gather 쓰기 (블록 단위, 권장)
 *
 * readv_blocks의 쓰기 대응. 애플리케이션이 여러 버퍼에서 모은 데이터를
 * 하나의 I/O로 묶어 제출할 때 사용(예: 직렬화된 헤더 + 페이로드 연속 기록).
 */

/**
 * Submit a write request to the bdev on the given channel. This differs from
 * spdk_bdev_write by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data or metadata and may not be able to directly transfer out of the buffers
 * provided. In this case, the request may fail.  This function uses separate
 * buffer for metadata transfer (valid only if bdev supports this mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iov.
 * \param md Metadata buffer.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to write.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_writev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				    struct iovec *iov, int iovcnt, void *md,
				    uint64_t offset_blocks, uint64_t num_blocks,
				    spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_writev_blocks_with_md - scatter 데이터 + 분리 메타(PI 포함) 쓰기
 * 전제: bdev->md_interleave == false. 데이터 iov + 연속 md 버퍼를 함께 장치에 전송.
 */

/**
 * Submit a write request to the bdev on the given channel. This differs from
 * spdk_bdev_write by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data or metadata and may not be able to directly transfer out of the buffers
 * provided. In this case, the request may fail.  This function uses separate
 * buffer for metadata transfer (valid only if bdev supports this mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to write.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 * \param opts Optional structure with extended IO request options. `size` member of this structure
 *             is used for ABI compatibility and must be set to sizeof(struct spdk_bdev_ext_io_opts).
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported or opts_size is incorrect
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_writev_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				struct iovec *iov, int iovcnt, uint64_t offset_blocks,
				uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg,
				struct spdk_bdev_ext_io_opts *opts);
/*
 * [한국어]
 * spdk_bdev_writev_blocks_ext - writev_blocks + 확장 옵션 (메모리 도메인, accel_sequence, md_buf 등)
 * NVMe-oF 타겟 / DMA 엔진 오프로드 경로의 표준 제출 API.
 */

/**
 * Submit a compare request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to compare to.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to compare. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_compare_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			     void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			     spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_compare_blocks - 장치 블록을 buf와 비교 (NVMe COMPARE 대응)
 *
 * 장치가 네이티브 compare 미지원 시 bdev 코어가 내부적으로 read → memcmp로 대체.
 * 불일치 시 완료 status = SPDK_BDEV_IO_STATUS_MISCOMPARE.
 * 사용처: 낙관적 동시성 제어(optimistic concurrency), 데이터 무결성 검증.
 */

/**
 * Submit a compare request to the bdev on the given channel. This function uses
 * separate buffer for metadata transfer (valid only if bdev supports this
 * mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to compare to.
 * \param md Metadata buffer.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to compare. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_compare_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				     void *buf, void *md, uint64_t offset_blocks, uint64_t num_blocks,
				     spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_compare_blocks_with_md - compare + 메타데이터(PI 포함) 비교
 * 데이터와 PI Guard·AppTag·RefTag가 모두 일치해야 성공.
 */

/**
 * Submit a compare request to the bdev on the given channel. This differs from
 * spdk_bdev_compare by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer out of the buffers provided. In
 * this case, the request may fail.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be compared to.
 * \param iovcnt The number of elements in iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to compare.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_comparev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      struct iovec *iov, int iovcnt,
			      uint64_t offset_blocks, uint64_t num_blocks,
			      spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_comparev_blocks - scatter-gather compare
 */

/**
 * Submit a compare request to the bdev on the given channel. This differs from
 * spdk_bdev_compare by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data or metadata and may not be able to directly transfer out of the buffers
 * provided. In this case, the request may fail.  This function uses separate
 * buffer for metadata transfer (valid only if bdev supports this mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be compared to.
 * \param iovcnt The number of elements in iov.
 * \param md Metadata buffer.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to compare.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_comparev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				      struct iovec *iov, int iovcnt, void *md,
				      uint64_t offset_blocks, uint64_t num_blocks,
				      spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_comparev_blocks_with_md - scatter compare + 메타 비교
 */

/**
 * Submit an atomic compare-and-write request to the bdev on the given channel.
 * For bdevs that do not natively support atomic compare-and-write, the bdev layer
 * will quiesce I/O to the specified LBA range, before performing the read,
 * compare and write operations.
 *
 * Currently this supports compare-and-write of only one block.
 *
 * The data buffers for both the compare and write operations are described in a
 * scatter gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer out of the buffers provided. In
 * this case, the request may fail.
 *
 * spdk_bdev_io_get_nvme_fused_status() function should be called in callback function
 * to get status for the individual operation.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param compare_iov A scatter gather list of buffers to be compared.
 * \param compare_iovcnt The number of elements in compare_iov.
 * \param write_iov A scatter gather list of buffers to be written if the compare is
 *                  successful.
 * \param write_iovcnt The number of elements in write_iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to compare-and-write.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_comparev_and_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *compare_iov, int compare_iovcnt,
		struct iovec *write_iov, int write_iovcnt,
		uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_comparev_and_writev_blocks - ★ atomic Compare-and-Write ★
 *
 * @compare_iov / @compare_iovcnt: 예상값 (COMPARE phase가 읽어 비교)
 * @write_iov / @write_iovcnt:     compare 성공 시 새 값 (WRITE phase)
 * @num_blocks: 현재 구현은 1블록만 지원 (NVMe fused COMPARE+WRITE 제약)
 *
 * 동작:
 *   - 네이티브 fused COMPARE+WRITE 지원 장치: 두 SQE를 fused로 원자 실행
 *   - 미지원 장치: bdev 코어가 LBA 범위를 **quiesce**(일시 I/O 차단) 후
 *     read→compare→write 시퀀스를 소프트웨어로 원자 모방
 *
 * 완료 상태:
 *   - compare 불일치: SPDK_BDEV_IO_STATUS_MISCOMPARE
 *   - 첫 조각 실패: SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED
 *   - 두 단계 개별 상태는 spdk_bdev_io_get_nvme_fused_status()로 조회
 *
 * 사용처: 분산 락, optimistic CAS, 멱등 업데이트 프리미티브.
 */

/**
 * Submit a request to acquire a data buffer that represents the given
 * range of blocks. The data buffer is placed in the spdk_bdev_io structure
 * and can be obtained by calling spdk_bdev_io_get_iovec().
 *
 * \param desc Block device descriptor
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list to be populated with the buffers
 * \param iovcnt The maximum number of elements in iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks.
 * \param populate Whether the data buffer should be populated with the
 *                 data at the given blocks. Populating the data buffer can
 *                 be skipped if the user writes new data to the entire buffer.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 */
int spdk_bdev_zcopy_start(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  struct iovec *iov, int iovcnt,
			  uint64_t offset_blocks, uint64_t num_blocks,
			  bool populate,
			  spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_zcopy_start - zero-copy I/O 시작 (데이터 버퍼 획득 phase)
 *
 * 동작: bdev이 내부 DMA 버퍼를 빌려주어 사용자가 직접 읽고·쓰게 함.
 * @iov: (OUT) 완료 콜백에서 spdk_bdev_io_get_iovec로 획득할 버퍼의 등록지
 * @populate:
 *   - true:  기존 블록 데이터를 버퍼에 채워 반환 (read-modify-write 용)
 *   - false: 버퍼만 할당, 내용은 정의되지 않음 (전체 덮어쓸 때)
 *
 * 완료 후 처리:
 *   1) 콜백에서 spdk_bdev_io_get_iovec()로 iov 획득
 *   2) 애플리케이션이 iov에 직접 쓰기·읽기 수행
 *   3) spdk_bdev_zcopy_end(bdev_io, commit, ...)로 반납
 *      · commit=true  : 수정한 내용을 장치로 flush(WRITE)
 *      · commit=false : 수정 없이 버퍼만 반납 (REBACK)
 *
 * 사용처: RDMA 타겟에서 호스트 메모리 복사 없이 wire로 직송, 대용량 데이터 편집.
 * 지원 장치 제한적 — io_type_supported(ZCOPY) 확인 필요.
 */

/**
 * Submit a request to release a data buffer representing a range of blocks.
 *
 * \param bdev_io I/O request returned in the completion callback of spdk_bdev_zcopy_start().
 * \param commit Whether to commit the data in the buffers to the blocks before releasing.
 *               The data does not need to be committed if it was not modified.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 */
int spdk_bdev_zcopy_end(struct spdk_bdev_io *bdev_io, bool commit,
			spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_zcopy_end - zcopy_start로 빌린 버퍼 반납 (commit/rollback)
 * @bdev_io: zcopy_start 완료 콜백의 bdev_io 재사용 (새로 할당하지 않음)
 * @commit: true면 버퍼 내용을 장치에 WRITE, false면 단순 반납
 */

/**
 * Submit a write zeroes request to the bdev on the given channel. This command
 *  ensures that all bytes in the specified range are set to 00h
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset The offset, in bytes, from the start of the block device.
 * \param len The size of data to zero.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_write_zeroes(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t offset, uint64_t len,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_write_zeroes - 지정 범위를 0으로 채움 (byte 단위)
 *
 * 장치가 네이티브 WRITE_ZEROES 커맨드를 지원하면 단일 명령으로 즉시 처리 →
 * 대역폭 소모 없이 논리적 제로화 가능. 미지원이면 bdev 코어가 제로 버퍼로
 * 일반 write를 반복(ZERO_BUFFER_SIZE=1MB 단위, bdev_internal.h 참조).
 * max_write_zeroes 초과 범위는 자동 분할.
 */

/**
 * Submit a write zeroes request to the bdev on the given channel. This command
 *  ensures that all bytes in the specified range are set to 00h
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to zero.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_write_zeroes_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				  uint64_t offset_blocks, uint64_t num_blocks,
				  spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_write_zeroes_blocks - 블록 단위 WRITE_ZEROES (권장)
 * 사용처: 파일 시스템 truncate/unlink 시 보안 제로화, lvol snapshot 초기화 등.
 */

/**
 * Submit a write uncorrectable request to the bdev on the given channel. This command writes logical
 * bad block to the device.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to write bad block.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 *   * -ENOTSUP - the bdev does not support the command.
 */
int spdk_bdev_write_uncorrectable_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_write_uncorrectable_blocks - 지정 블록을 "복구 불가 bad block"으로 마킹
 *
 * NVMe "Write Uncorrectable" 커맨드에 대응. 이후 read는 의도적 에러 반환.
 * 용도: 스토리지 결함 재현 테스트, 사용자가 특정 블록을 "읽지 말 것"으로 표시.
 * @return 추가: -ENOTSUP — 장치 미지원
 */

/**
 * Submit an unmap request to the block device. Unmap is sometimes also called trim or
 * deallocate. This notifies the device that the data in the blocks described is no
 * longer valid. Reading blocks that have been unmapped results in indeterminate data.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset The offset, in bytes, from the start of the block device.
 * \param nbytes The number of bytes to unmap. Must be a multiple of the block size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_unmap(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    uint64_t offset, uint64_t nbytes,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_unmap - 지정 범위를 UNMAP/TRIM (byte 단위)
 *
 * SSD에 "이 영역 데이터는 더 이상 필요 없음"을 알림 → FTL이 garbage collection에
 * 활용 → 쓰기 증폭 감소, 수명·성능 향상.
 * 주의: UNMAP된 블록 read 결과는 비결정적(장치별로 0 또는 stale 데이터).
 */

/**
 * Submit an unmap request to the block device. Unmap is sometimes also called trim or
 * deallocate. This notifies the device that the data in the blocks described is no
 * longer valid. Reading blocks that have been unmapped results in indeterminate data.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to unmap.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_unmap_blocks - 블록 단위 UNMAP/TRIM (권장)
 *
 * NVMe DSM(Dataset Management) Deallocate 커맨드로 구현.
 * max_unmap 초과 범위는 자동 분할 — 여러 DSM 범위 세트로 분해.
 * max_unmap_segments 한도도 분할 기준.
 */

/**
 * Submit a flush request to the bdev on the given channel. For devices with volatile
 * caches, data is not guaranteed to be persistent until the completion of a flush
 * request. Call spdk_bdev_has_write_cache() to check if the bdev has a volatile cache.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset The offset, in bytes, from the start of the block device.
 * \param length The number of bytes.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_flush(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    uint64_t offset, uint64_t length,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_flush - 쓰기 캐시 플러시 (byte 단위)
 *
 * 휘발성 캐시가 있는 장치(대부분의 엔터프라이즈 SSD)에서만 의미 있음.
 * - spdk_bdev_has_write_cache(bdev) 로 필요성 확인
 * - 플러시 완료 후에만 장치 내구성(power-loss-safe) 보장
 * NVMe FLUSH 커맨드의 범위 인자는 사실상 무시되고 전체 flush로 동작 (장치 스펙상).
 */

/**
 * Submit a flush request to the bdev on the given channel. For devices with volatile
 * caches, data is not guaranteed to be persistent until the completion of a flush
 * request. Call spdk_bdev_has_write_cache() to check if the bdev has a volatile cache.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_flush_blocks - 블록 단위 flush (권장)
 * 트랜잭션 로깅(예: ZFS ZIL, DB WAL) 경로에서 내구성 확정 시점에 호출.
 */

/**
 * Submit a reset request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_reset - bdev 리셋 (복구 명령)
 *
 * 동작:
 *   1) 모든 채널의 in-flight I/O에 대해 reset_io_drain_timeout 만큼 완료 대기
 *   2) 대기 초과 시 outstanding I/O를 abort + 하위 장치 reset 전파
 *   3) 공유 bdev(lvol이 하위 NVMe 공유 등)은 다른 lvol 영향 방지 위해 drain 우선
 *
 * 주의: 동시에 하나의 reset만 진행 가능(bdev->internal.reset_in_progress) —
 *       연속 호출 시 두 번째 이후는 queued_resets에 적재되어 순차 처리.
 * 사용처: 장치 hang 복구, 오류 복구. hot path는 아님.
 */

/**
 * Submit a NVMe Subsystem Reset request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_nvme_nssr(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_nvme_nssr - NVMe Subsystem Reset (NSSR)
 *
 * NVMe 컨트롤러 전체 초기화 (CC.EN=0→1 시퀀스 포함) — reset보다 강한 복구.
 * bdev_nvme 모듈 전용 (다른 bdev 모듈은 -ENOTSUP).
 * 사용 시 경고: subsystem의 모든 namespace I/O가 일시 중단됨.
 */

/**
 * Submit abort requests to abort all I/Os which has bio_cb_arg as its callback
 * context to the bdev on the given channel.
 *
 * This goes all the way down to the bdev driver module and attempts to abort all
 * I/Os which have bio_cb_arg as their callback context if they exist. This is a best
 * effort command. Upon completion of this, the status SPDK_BDEV_IO_STATUS_SUCCESS
 * indicates all the I/Os were successfully aborted, or the status
 * SPDK_BDEV_IO_STATUS_FAILED indicates any I/O was failed to abort for any reason
 * or no I/O which has bio_cb_arg as its callback context was found.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch The I/O channel which the I/Os to be aborted are associated with.
 * \param bio_cb_arg Callback argument for the outstanding requests which this
 * function attempts to abort.
 * \param cb Called when the abort request is completed.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always be called (even if the
 * request ultimately failed). Return negated errno on failure, in which case the
 * callback will not be called.
 *   * -EINVAL - bio_cb_arg was not specified.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated.
 *   * -ENOTSUP - the bdev does not support abort.
 */
int spdk_bdev_abort(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    void *bio_cb_arg,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_abort - cb_arg로 매칭된 모든 I/O 취소 시도 (best effort)
 *
 * @bio_cb_arg: 취소 대상 I/O들의 공통 cb_arg (완료 콜백 컨텍스트)
 *   → 이 값과 일치하는 모든 in-flight bdev_io를 찾아 abort 커맨드 발행
 *
 * 완료 상태:
 *   - SUCCESS: 모든 매칭 I/O abort 완료
 *   - FAILED : 일부/전부 실패 또는 매칭 I/O 없음
 *
 * 사용처: 타임아웃 초과 요청 정리, 사용자 취소 이벤트 처리, NVMe-oF CONNECT 실패 정리.
 * 주의: NVMe Abort 커맨드 자체가 best-effort — 이미 장치가 처리 중인 I/O는 못 막을 수 있음.
 */

/**
 * Submit an NVMe Admin command to the bdev. This passes directly through
 * the block layer to the device. Support for NVMe passthru is optional,
 * indicated by calling spdk_bdev_io_type_supported().
 *
 * The SGL/PRP will be automated generated based on the given buffer,
 * so that portion of the command may be left empty.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param cmd The raw NVMe command. Must be an admin command.
 * \param buf Data buffer to written from.
 * \param nbytes The number of bytes to transfer. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_nvme_admin_passthru(struct spdk_bdev_desc *desc,
				  struct spdk_io_channel *ch,
				  const struct spdk_nvme_cmd *cmd,
				  void *buf, size_t nbytes,
				  spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_nvme_admin_passthru - ★ NVMe admin 명령을 그대로 장치에 전달 ★
 *
 * @cmd: 호출자가 완성한 SQE (64B) — opcode·CDW10~15까지 모두 지정
 * @buf: 데이터 전송 버퍼 (GET LOG PAGE, IDENTIFY 등 응답 저장소)
 * @nbytes: 전송 크기. PRP/SGL은 bdev_nvme가 자동 생성
 *
 * 사용처: NVMe 사용자 도구(smart info 조회, firmware 업데이트, directive 설정 등)
 *   - spdk_nvme_admin/io_cmd API가 더 높은 수준이지만 raw SQE 제어 필요 시 이 API
 * 지원 확인: spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_ADMIN)
 */

/**
 * Submit an NVMe I/O command to the bdev. This passes directly through
 * the block layer to the device. Support for NVMe passthru is optional,
 * indicated by calling spdk_bdev_io_type_supported().
 *
 * \ingroup bdev_io_submit_functions
 *
 * The SGL/PRP will be automated generated based on the given buffer,
 * so that portion of the command may be left empty. Also, the namespace
 * id (nsid) will be populated automatically.
 *
 * \param bdev_desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param cmd The raw NVMe command. Must be in the NVM command set.
 * \param buf Data buffer to written from.
 * \param nbytes The number of bytes to transfer. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_nvme_io_passthru(struct spdk_bdev_desc *bdev_desc,
			       struct spdk_io_channel *ch,
			       const struct spdk_nvme_cmd *cmd,
			       void *buf, size_t nbytes,
			       spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_nvme_io_passthru - NVMe I/O 명령 raw passthru
 *
 * @cmd: NVM command set SQE. nsid는 bdev_nvme가 자동 채움 (호출자는 설정 불필요)
 * 사용처: 벤더 확장 I/O 명령, 표준 read/write로 표현 불가능한 명령 전달.
 */

/**
 * Submit an NVMe I/O command to the bdev. This passes directly through
 * the block layer to the device. Support for NVMe passthru is optional,
 * indicated by calling spdk_bdev_io_type_supported().
 *
 * \ingroup bdev_io_submit_functions
 *
 * The SGL/PRP will be automated generated based on the given buffer,
 * so that portion of the command may be left empty. Also, the namespace
 * id (nsid) will be populated automatically.
 *
 * \param bdev_desc Block device descriptor
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param cmd The raw NVMe command. Must be in the NVM command set.
 * \param buf Data buffer to written from.
 * \param nbytes The number of bytes to transfer. buf must be greater than or equal to this size.
 * \param md_buf Meta data buffer to written from.
 * \param md_len md_buf size to transfer. md_buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_nvme_io_passthru_md(struct spdk_bdev_desc *bdev_desc,
				  struct spdk_io_channel *ch,
				  const struct spdk_nvme_cmd *cmd,
				  void *buf, size_t nbytes, void *md_buf, size_t md_len,
				  spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_nvme_io_passthru_md - NVMe I/O passthru + 분리 메타 버퍼
 * PI가 활성화된 네임스페이스에서 메타(PI Guard/AppTag/RefTag) 버퍼를 별도 전달.
 */

/**
 * Submit an NVMe I/O command to the bdev. This passes directly through
 * the block layer to the device. Support for NVMe passthru is optional,
 * indicated by calling spdk_bdev_io_type_supported().
 *
 * \ingroup bdev_io_submit_functions
 *
 * The namespace id (nsid) will be populated automatically.
 *
 * \param desc Block device descriptor
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param cmd The raw NVMe command. Must be in the NVM command set.
 * \param iov A scatter gather list of buffers for the command to use.
 * \param iovcnt The number of elements in iov.
 * \param nbytes The number of bytes to transfer. The total size of the buffers in iov must be greater than or equal to this size.
 * \param md_buf Meta data buffer to written from.
 * \param md_len md_buf size to transfer. md_buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_nvme_iov_passthru_md(struct spdk_bdev_desc *desc,
				   struct spdk_io_channel *ch,
				   const struct spdk_nvme_cmd *cmd,
				   struct iovec *iov, int iovcnt,
				   size_t nbytes, void *md_buf, size_t md_len,
				   spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_nvme_iov_passthru_md - scatter-gather 데이터 + 분리 메타의 NVMe I/O passthru
 * iov의 총 길이가 nbytes와 정확히 일치해야 함. NVMe-oF 타겟이 CONNECT 응답을 iov로 처리할 때 등.
 */

/**
 * Submit a copy request to the block device.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param dst_offset_blocks The destination offset, in blocks, from the start of the block device.
 * \param src_offset_blocks The source offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to copy.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - dst_offset_blocks, src_offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 *   * -ENOTSUP - copy operation is not supported
 */
int spdk_bdev_copy_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  uint64_t dst_offset_blocks, uint64_t src_offset_blocks,
			  uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_copy_blocks - ★ 장치 내 블록 복사 (offloaded) ★
 *
 * 호스트 메모리 왕복 없이 장치 내부에서 데이터 복사 (NVMe Simple Copy, SCSI XCOPY).
 * 대역폭/CPU/PCIe 부하를 대폭 절감. snapshot/clone 구현의 핵심 프리미티브.
 *
 * @dst_offset_blocks / @src_offset_blocks: 대상·원본 LBA (같은 bdev 내)
 * @num_blocks: 복사 블록 수. max_copy 초과 시 자동 분할.
 *
 * @return 추가: -ENOTSUP — 장치 미지원 (fallback은 호출자가 read+write 조합으로 직접 구현)
 * 지원 확인: spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COPY)
 */

/**
 * Free an I/O request. This should only be called after the completion callback
 * for the I/O has been called and notifies the bdev layer that memory may now
 * be released.
 *
 * \param bdev_io I/O request.
 */
void spdk_bdev_free_io(struct spdk_bdev_io *bdev_io);
/*
 * [한국어]
 * spdk_bdev_free_io - ★ 완료 콜백 내부에서 반드시 호출 ★
 *
 * 역할: 완료된 bdev_io를 bdev 채널 mempool에 반환 → 다음 요청 할당 가능.
 * 호출 조건: spdk_bdev_io_completion_cb 안에서만 호출해야 함(미리 호출하면 UAF).
 * 누락 시 증상: bdev_io 풀 고갈로 이후 제출이 -ENOMEM 영구 반환 (자원 누수).
 */

/**
 * Block device I/O wait callback
 *
 * Callback function to notify when an spdk_bdev_io structure is available
 * to satisfy a call to one of the @ref bdev_io_submit_functions.
 */
typedef void (*spdk_bdev_io_wait_cb)(void *cb_arg);
                                  /* [한국어] bdev_io 풀에 여유가 생겼을 때 호출될 콜백 (spdk_bdev_queue_io_wait 등록) */

/**
 * Structure to register a callback when an spdk_bdev_io becomes available.
 */
/*
 * [한국어] struct spdk_bdev_io_wait_entry - NOMEM 재시도 등록 엔트리
 *
 * 사용 패턴:
 *   static struct spdk_bdev_io_wait_entry wait;
 *   int rc = spdk_bdev_read_blocks(...);
 *   if (rc == -ENOMEM) {
 *       wait.bdev = bdev;
 *       wait.cb_fn = my_retry_fn;
 *       wait.cb_arg = ctx;
 *       spdk_bdev_queue_io_wait(bdev, ch, &wait);
 *   }
 *   // 풀에 여유 생기면 my_retry_fn 호출 → 내부에서 다시 spdk_bdev_read_blocks
 */
struct spdk_bdev_io_wait_entry {
	struct spdk_bdev			*bdev;
                                  /* [한국어] 대상 bdev — queue_io_wait의 bdev 인자와 일치 필요 */
	spdk_bdev_io_wait_cb			cb_fn;
                                  /* [한국어] 풀 가용 시 호출할 사용자 콜백 */
	void					*cb_arg;
                                  /* [한국어] 콜백 컨텍스트 */
	/**
	 * When true, this I/O is critical to unblock other I/Os that
	 * holding resource and depend on the completion of this IO.
	 * If resource allocation fails such as ENOMEM,
	 * this entry should be queued at the head to avoid deadlock.
	 */
	bool					dep_unblock;
                                  /* [한국어] true면 "다른 I/O의 완료를 해제하기 위해 반드시 먼저 처리"
                                   *  - 대기 큐 head에 삽입 → 데드락 회피 (예: 완료 콜백이 새 I/O를 trigger하는 경우) */
	uint8_t					pad[7];
                                  /* [한국어] 정렬 패딩 */
	TAILQ_ENTRY(spdk_bdev_io_wait_entry)	link;
                                  /* [한국어] 채널의 wait 리스트 링크 */
};

/**
 * Add an entry into the calling thread's queue to be notified when an
 * spdk_bdev_io becomes available.
 *
 * When one of the @ref bdev_io_submit_functions returns -ENOMEM, it means
 * the spdk_bdev_io buffer pool has no available buffers. This function may
 * be called to register a callback to be notified when a buffer becomes
 * available on the calling thread.
 *
 * The callback function will always be called on the same thread as this
 * function was called.
 *
 * This function must only be called immediately after one of the
 * @ref bdev_io_submit_functions returns -ENOMEM.
 *
 * \param bdev Block device.  The block device that the caller will submit
 *             an I/O to when the callback is invoked.  Must match the bdev
 *             member in the entry parameter.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param entry Data structure allocated by the caller specifying the callback
 *              function and argument.
 *
 * \return 0 on success.
 *         -EINVAL if bdev parameter does not match bdev member in entry
 *         -EINVAL if an spdk_bdev_io structure was available on this thread.
 */
int spdk_bdev_queue_io_wait(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			    struct spdk_bdev_io_wait_entry *entry);
/*
 * [한국어]
 * spdk_bdev_queue_io_wait - NOMEM 이후 재시도 등록
 *
 * 제약:
 *   - spdk_bdev_* 제출 API가 -ENOMEM 반환 **직후에만** 호출해야 함
 *   - 이미 bdev_io가 가용한 상황에서 호출하면 -EINVAL 반환 (잘못된 사용)
 *   - @entry는 호출자가 수명 관리 (콜백 실행까지 valid 유지)
 * 콜백 실행 스레드: 이 함수를 호출한 스레드와 동일 (채널 기준)
 * 콜백 내부에서 다시 제출 시도 가능 — 여전히 -ENOMEM이면 재등록 가능
 */

/**
 * Return I/O statistics for this channel.
 *
 * \param bdev Block device.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param stat The per-channel statistics.
 * \param reset_mode Mode to determine how I/O stat should be reset after obtaining it.
 *
 */
void spdk_bdev_get_io_stat(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			   struct spdk_bdev_io_stat *stat, enum spdk_bdev_reset_stat_mode reset_mode);
/*
 * [한국어]
 * spdk_bdev_get_io_stat - 단일 채널의 통계 동기 조회 + 선택적 리셋
 *
 * @bdev: 대상 bdev
 * @ch: I/O 채널 (현재 스레드 소유)
 * @stat: OUT — 호출자가 할당, 채널 통계가 복사됨
 * @reset_mode: 조회 후 카운터 리셋 정책 (enum spdk_bdev_reset_stat_mode 참조)
 *
 * 호출 컨텍스트: 채널 소유 스레드에서 호출 (thread affinity).
 * 동기 동작: 메시지 없이 직접 채널 통계 읽음 → 완료 콜백 없이 즉시 반환.
 * 디바이스 전체 통계는 spdk_bdev_get_device_stat(비동기) 사용.
 */


/**
 * Return I/O statistics for this bdev. All the required information will be passed
 * via the callback function.
 *
 * \param bdev Block device to query.
 * \param stat Structure for aggregating collected statistics.  Passed as argument to cb.
 * \param reset_mode Mode to determine how I/O stat should be reset after obtaining it.
 * \param cb Called when this operation completes.
 * \param cb_arg Argument passed to callback function.
 */
void spdk_bdev_get_device_stat(struct spdk_bdev *bdev, struct spdk_bdev_io_stat *stat,
			       enum spdk_bdev_reset_stat_mode reset_mode, spdk_bdev_get_device_stat_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_get_device_stat - ★ 모든 채널 통계 비동기 집계 ★
 *
 * 동작:
 *   1) for_each_channel 패턴으로 모든 채널 순회
 *   2) 각 채널에서 get_io_stat 호출 후 결과 stat에 누적
 *   3) reset_mode에 따라 채널 통계 리셋 (또는 유지)
 *   4) 모든 채널 완료 후 호출 스레드에서 cb(bdev, stat, cb_arg, rc) 호출
 *
 * @stat: 호출자가 할당, 함수 반환 시점에는 아직 미완성. 콜백 진입 시 완성.
 *        콜백 종료까지 stat 메모리 유지 필수 (bdev 코어가 비동기 채움).
 *
 * 호출 컨텍스트: 임의 스레드 (단, cb는 호출 스레드에서 실행).
 * 사용처: RPC bdev_get_iostat — JSON 응답 빌드.
 */

/**
 * Get the status of bdev_io as an NVMe status code and command specific
 * completion queue value.
 *
 * \param bdev_io I/O to get the status from.
 * \param cdw0 Command specific completion queue value
 * \param sct Status Code Type return value, as defined by the NVMe specification.
 * \param sc Status Code return value, as defined by the NVMe specification.
 */
void spdk_bdev_io_get_nvme_status(const struct spdk_bdev_io *bdev_io, uint32_t *cdw0, int *sct,
				  int *sc);
/*
 * [한국어]
 * spdk_bdev_io_get_nvme_status - 실패 bdev_io의 NVMe 세부 상태 조회
 *
 * 사용 시점: completion_cb의 success==false일 때 호출하여 세부 에러 획득
 * @cdw0: NVMe CQE의 dword 0 (명령별 반환값, 예: GET_LOG_PAGE의 length)
 * @sct: Status Code Type (Generic=0, CmdSpecific=1, Media=2 등)
 * @sc: Status Code (SCT와 조합하여 유일 의미)
 *
 * bdev_io의 type이 NVMe 직접/간접 경로일 때만 유효. SCSI 기반 bdev에서는 0 반환.
 */

/**
 * Get the status of bdev_io as an NVMe status codes and command specific
 * completion queue value for fused operations such as compare-and-write.
 *
 * \param bdev_io I/O to get the status from.
 * \param cdw0 Command specific completion queue value
 * \param first_sct Status Code Type return value for the first operation, as defined by the NVMe specification.
 * \param first_sc Status Code return value for the first operation, as defined by the NVMe specification.
 * \param second_sct Status Code Type return value for the second operation, as defined by the NVMe specification.
 * \param second_sc Status Code return value for the second operation, as defined by the NVMe specification.
 */
void spdk_bdev_io_get_nvme_fused_status(const struct spdk_bdev_io *bdev_io, uint32_t *cdw0,
					int *first_sct, int *first_sc, int *second_sct, int *second_sc);
/*
 * [한국어]
 * spdk_bdev_io_get_nvme_fused_status - fused 명령(COMPARE+WRITE)의 두 상태 조회
 *
 * fused 명령은 두 SQE가 원자적으로 실행 → 각각의 CQE 상태를 별도로 보고.
 * 예: COMPARE(first)는 성공했지만 WRITE(second)가 실패 등 세밀한 상태 판정.
 */

/**
 * Get the status of bdev_io as a SCSI status code.
 *
 * \param bdev_io I/O to get the status from.
 * \param sc SCSI Status Code.
 * \param sk SCSI Sense Key.
 * \param asc SCSI Additional Sense Code.
 * \param ascq SCSI Additional Sense Code Qualifier.
 */
void spdk_bdev_io_get_scsi_status(const struct spdk_bdev_io *bdev_io,
				  int *sc, int *sk, int *asc, int *ascq);
/*
 * [한국어]
 * spdk_bdev_io_get_scsi_status - 실패 bdev_io의 SCSI 세부 상태 조회
 *
 * @sc: SCSI Status Code (GOOD, CHECK_CONDITION 등)
 * @sk: Sense Key (NOT_READY, MEDIUM_ERROR 등)
 * @asc/@ascq: Additional Sense Code / Qualifier (세부 원인)
 *
 * 사용처: iSCSI 타겟이 호스트 이니시에이터에게 CHECK_CONDITION 응답 구성 시.
 */

/**
 * Get the status of bdev_io as aio errno.
 *
 * \param bdev_io I/O to get the status from.
 * \param aio_result Negative errno returned from AIO.
 */
void spdk_bdev_io_get_aio_status(const struct spdk_bdev_io *bdev_io, int *aio_result);
/*
 * [한국어]
 * spdk_bdev_io_get_aio_status - bdev_aio 모듈 경로의 원시 errno 조회
 *
 * @aio_result: libaio가 반환한 음수 errno 그대로 (예: -EIO, -ENOSPC)
 * SCSI/NVMe 변환 없이 원본 커널 에러 그대로 필요할 때 사용 (bdev_aio 백엔드 한정).
 */

/**
 * Get the iovec describing the data buffer of a bdev_io.
 *
 * \param bdev_io I/O to describe with iovec.
 * \param iovp Pointer to be filled with iovec.
 * \param iovcntp Pointer to be filled with number of iovec entries.
 */
void spdk_bdev_io_get_iovec(struct spdk_bdev_io *bdev_io, struct iovec **iovp, int *iovcntp);
/*
 * [한국어]
 * spdk_bdev_io_get_iovec - bdev_io가 가진 데이터 iovec 접근
 *
 * 주로 모듈 측에서 submit_request 콜백 내부에서 사용 — bdev_io의 payload를
 * 자체 백엔드 전송 형식으로 변환 시 iovec 배열을 꺼냄.
 * NVMe 모듈: 이 iovec을 기반으로 PRP/SGL 디스크립터 빌드.
 */

/**
 * Get metadata buffer. Only makes sense if the IO uses separate buffer for
 * metadata transfer.
 *
 * \param bdev_io I/O to retrieve the buffer from.
 * \return Pointer to metadata buffer, NULL if the IO doesn't use separate
 * buffer for metadata transfer.
 */
void *spdk_bdev_io_get_md_buf(struct spdk_bdev_io *bdev_io);
/*
 * [한국어]
 * spdk_bdev_io_get_md_buf - 분리 메타 버퍼 포인터 (없으면 NULL)
 * interleave 모드 bdev_io에서는 NULL. 모듈의 PI 빌드 경로에서 사용.
 */

/**
 * Get the callback argument of bdev_io to abort it by spdk_bdev_abort.
 *
 * \param bdev_io I/O to get the callback argument from.
 * \return Callback argument of bdev_io.
 */
void *spdk_bdev_io_get_cb_arg(struct spdk_bdev_io *bdev_io);
/*
 * [한국어]
 * spdk_bdev_io_get_cb_arg - bdev_io의 사용자 cb_arg 반환
 *
 * 주요 용도: spdk_bdev_abort가 in-flight I/O를 식별할 때 cb_arg를 키로 사용 →
 *           특정 abort 대상 I/O의 cb_arg를 찾을 때.
 * 일반 데이터 경로에서는 사용 불필요 (완료 cb의 cb_arg 인자가 직접 전달됨).
 */

typedef void (*spdk_bdev_histogram_status_cb)(void *cb_arg, int status);
                                  /* [한국어] 히스토그램 enable/disable 완료 콜백
                                   *  - @status: 0 성공, 음수 errno (모든 채널 적용 도중 실패)
                                   *  - 호출 컨텍스트: enable/disable 호출 스레드 */
typedef void (*spdk_bdev_histogram_data_cb)(void *cb_arg, int status,
		struct spdk_histogram_data *histogram);
                                  /* [한국어] 히스토그램 조회 완료 콜백 (get / channel_get)
                                   *  - @histogram: 집계된 결과 (호출자가 미리 할당, 또는 채널의 임시 객체)
                                   *  - 채널별 호출 시(@histogram)은 cb 실행 동안만 valid — cb 후 참조 금지
                                   *  - @status: 0 성공, 음수 errno */

/**
 * Get the result of a previous seek function.
 * After calling spdk_bdev_seek_data or spdk_bdev_seek_hole, call this function
 * to retrieve the offset of next allocated data or next unallocated hole.
 *
 * \param bdev_io I/O to get the status from.
 *
 * \return data/hole offset in blocks or UINT64_MAX if not found
 */
uint64_t spdk_bdev_io_get_seek_offset(const struct spdk_bdev_io *bdev_io);
/*
 * [한국어]
 * spdk_bdev_io_get_seek_offset - seek_data/seek_hole 완료 결과 회수
 *
 * 사용 시점: 완료 cb 내부에서 호출하여 LBA 결과 취득.
 * @return:
 *   - seek_data 성공: 다음 데이터 시작 LBA
 *   - seek_hole 성공: 다음 hole 시작 LBA
 *   - 더 이상 매칭 없음: UINT64_MAX (POSIX SEEK_DATA의 ENXIO 등가)
 *
 * bdev_io는 SEEK_DATA / SEEK_HOLE 타입이어야 함 — 다른 I/O에 호출하면 의미 없는 값 반환.
 */

/**
 * Enable or disable collecting histogram data on a bdev.
 *
 * \param bdev Block device.
 * \param cb_fn Callback function to be called when histograms are enabled.
 * \param cb_arg Argument to pass to cb_fn.
 * \param enable Enable/disable flag
 */
void spdk_bdev_histogram_enable(struct spdk_bdev *bdev, spdk_bdev_histogram_status_cb cb_fn,
				void *cb_arg, bool enable);
/*
 * [한국어]
 * spdk_bdev_histogram_enable - bdev 지연시간 히스토그램 수집 on/off
 *
 * 활성화 시 모든 채널에 히스토그램 버킷 할당, 완료 콜백에서 레이턴시 누적.
 * 기본 설정: 전체 I/O 타입 대상, 기본 granularity.
 * 세밀 제어 필요 시 spdk_bdev_histogram_enable_ext 사용.
 * 성능 영향: 각 I/O 완료 시 버킷 업데이트 → 약간의 오버헤드.
 */

/**
 * Enable or disable collecting histogram data on a bdev. This differs from
 * spdk_bdev_histogram_enable by allowing Optional structure with extended enable
 * histogram options.
 *
 * \param bdev Block device.
 * \param cb_fn Callback function to be called when histograms are enabled.
 * \param cb_arg Argument to pass to cb_fn.
 * \param enable Enable/disable flag
 * \param opts Optional structure with extended enable histogram options. `size` member of this structure
 *             is used for ABI compatibility and must be set to sizeof(struct spdk_bdev_enable_histogram_opts).
 */
void spdk_bdev_histogram_enable_ext(struct spdk_bdev *bdev, spdk_bdev_histogram_status_cb cb_fn,
				    void *cb_arg, bool enable, struct spdk_bdev_enable_histogram_opts *opts);
/*
 * [한국어]
 * spdk_bdev_histogram_enable_ext - 확장 옵션 기반 히스토그램 enable/disable
 *
 * @bdev: 대상 bdev
 * @cb_fn: 모든 채널에 enable/disable 적용 후 호출 (비동기)
 * @cb_arg: 콜백 컨텍스트
 * @enable: true=수집 시작, false=중단 (기존 데이터 유지)
 * @opts: 측정 범위·해상도·io_type 필터. NULL이면 기본값 (전 I/O 타입, 표준 해상도)
 *
 * 동작:
 *   1) bdev의 모든 채널을 spdk_bdev_for_each_channel 패턴으로 순회
 *   2) 각 채널에서 enable=true면 spdk_histogram_data_alloc(granularity), false면 free
 *   3) min/max_nsec 범위로 첫/마지막 버킷 fold 동작 결정
 *   4) 모든 채널 완료 후 cb_fn(status=0) 호출
 *
 * io_type=특정값이면 그 타입 I/O 완료 시에만 latency 누적 → 타입별 분포 분리 분석.
 */

/**
 * Initialize bdev enable histogram options structure.
 *
 * \param opts The structure to initialize.
 * \param size The size of *opts.
 */
void
spdk_bdev_enable_histogram_opts_init(struct spdk_bdev_enable_histogram_opts *opts, size_t size);
/*
 * [한국어]
 * spdk_bdev_enable_histogram_opts_init - histogram_opts를 기본값으로 초기화
 *
 * @opts: 호출자가 스택/힙에 할당한 구조체
 * @size: sizeof(struct spdk_bdev_enable_histogram_opts) 또는 호출자 인지 크기
 *
 * 기본값:
 *   io_type = 0 (모든 타입 측정)
 *   granularity = 0 (기본 해상도)
 *   min_nsec = 0, max_nsec = 0 (기본 범위)
 *   size = @size
 *
 * 권장 사용 패턴:
 *   struct spdk_bdev_enable_histogram_opts opts;
 *   spdk_bdev_enable_histogram_opts_init(&opts, sizeof(opts));
 *   opts.io_type = SPDK_BDEV_IO_TYPE_READ;
 *   opts.granularity = 6;  // 64ns 버킷
 *   spdk_bdev_histogram_enable_ext(bdev, cb, ctx, true, &opts);
 */

/**
 * Get aggregated histogram data from a bdev. Callback provides merged histogram
 * for specified bdev.
 *
 * \param bdev Block device.
 * \param histogram Histogram for aggregated data
 * \param cb_fn Callback function to be called with data collected on bdev.
 * \param cb_arg Argument to pass to cb_fn.
 */
void spdk_bdev_histogram_get(struct spdk_bdev *bdev, struct spdk_histogram_data *histogram,
			     spdk_bdev_histogram_data_cb cb_fn,
			     void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_histogram_get - 모든 채널의 히스토그램 집계 조회 (비동기)
 *
 * @histogram: 호출자가 미리 할당 (spdk_histogram_data_alloc로 생성)
 * 각 채널 스레드에 메시지 보내 자기 버킷을 합계에 더함 → cb_fn 호출.
 * 사용처: RPC bdev_get_histogram 응답, p99 latency 모니터링.
 */

/**
 * Get histogram data of the specified channel for a bdev. The histogram passed to cb_fn
 * is only valid during the execution of cb_fn. Referencing the histogram after cb_fn
 * returns is not supported and yields undetermined behavior.
 *
 * \param ch IO channel of bdev.
 * \param cb_fn Callback function to process the histogram of the channel.
 * \param cb_arg Argument to pass to cb_fn.
 */
void spdk_bdev_channel_get_histogram(struct spdk_io_channel *ch, spdk_bdev_histogram_data_cb cb_fn,
				     void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_channel_get_histogram - ★ 단일 채널의 히스토그램만 조회 ★
 *
 * @ch: 대상 I/O 채널
 * @cb_fn: cb_fn(cb_arg, status, histogram) — histogram은 cb 동안만 valid
 * @cb_arg: 콜백 컨텍스트
 *
 * spdk_bdev_histogram_get(전체 집계)와의 차이:
 *   - 이 함수는 단일 채널의 버킷만 반환 → 채널별(reactor별) 분포 분석
 *   - 메시지 합산 단계 없음 → 빠른 조회
 *   - histogram 객체는 채널이 보유한 임시 객체 → cb 종료 후 즉시 무효 (참조 금지)
 *
 * 호출 컨텍스트: 채널 소유 스레드에서 직접 호출해야 함 (thread affinity).
 * 사용처: 특정 reactor의 NUMA-locality 효과 측정, per-CPU latency 분포 디버깅.
 */

/**
 * Retrieves media events.  Can only be called from the context of
 * SPDK_BDEV_EVENT_MEDIA_MANAGEMENT event callback.  These events are sent by
 * devices exposing raw access to the physical medium (e.g. Open Channel SSD).
 *
 * \param bdev_desc Block device descriptor
 * \param events Array of media management event descriptors
 * \param max_events Size of the events array
 *
 * \return number of events retrieved
 */
size_t spdk_bdev_get_media_events(struct spdk_bdev_desc *bdev_desc,
				  struct spdk_bdev_media_event *events, size_t max_events);
/*
 * [한국어]
 * spdk_bdev_get_media_events - 미디어 관리 이벤트 큐에서 이벤트 회수
 *
 * 호출 컨텍스트: SPDK_BDEV_EVENT_MEDIA_MANAGEMENT 이벤트 콜백 내부에서만 호출 가능
 * @events: 호출자가 할당한 이벤트 배열 (offset/num_blocks 정보)
 * @max_events: 배열 크기
 * @return: 실제 채워진 이벤트 수 (≤max_events). 추가 이벤트가 더 있으면 다음 호출로 회수
 *
 * 사용처: Open Channel SSD 등 raw 미디어를 노출하는 장치에서 bad block 알림,
 *         host-managed FTL이 GC를 위해 영향 영역 추적.
 */

/**
 * Get SPDK memory domains used by the given bdev. If bdev reports that it uses memory domains
 * that means that it can work with data buffers located in those memory domains.
 *
 * The user can call this function with \b domains set to NULL and \b array_size set to 0 to get the
 * number of memory domains used by bdev
 *
 * \param bdev Block device
 * \param domains Pointer to an array of memory domains to be filled by this function. The user should allocate big enough
 * array to keep all memory domains used by bdev and all underlying bdevs
 * \param array_size size of \b domains array
 * \return the number of entries in \b domains array or negated errno. If returned value is bigger than \b array_size passed by the user
 * then the user should increase the size of \b domains array and call this function again. There is no guarantees that
 * the content of \b domains array is valid in that case.
 *         -EINVAL if input parameters were invalid
 */
int spdk_bdev_get_memory_domains(struct spdk_bdev *bdev, struct spdk_memory_domain **domains,
				 int array_size);
/*
 * [한국어]
 * spdk_bdev_get_memory_domains - bdev이 지원하는 모든 메모리 도메인 열거
 *
 * @bdev: 대상 bdev (vbdev이면 하위 모든 bdev의 도메인 합집합)
 * @domains: 결과 배열 (NULL 가능 — 개수만 조회)
 * @array_size: 배열 크기
 * @return: 실제 도메인 수. array_size보다 크면 호출자가 배열 키워 재호출
 *
 * 두 단계 호출 패턴:
 *   int n = spdk_bdev_get_memory_domains(bdev, NULL, 0);   // 개수 먼저
 *   struct spdk_memory_domain **arr = calloc(n, ...);
 *   spdk_bdev_get_memory_domains(bdev, arr, n);            // 본 조회
 *
 * 사용처: NVMe-oF 타겟이 RDMA 호스트 메모리 도메인 호환성 확인 — 호환되는 도메인이면
 *         memory_domain 옵션으로 zero-copy I/O 가능, 아니면 bounce buffer 경유.
 */

/**
 * \brief SPDK bdev channel iterator.
 *
 * This is a virtual representation of a bdev channel iterator.
 */
struct spdk_bdev_channel_iter;
                                  /* [한국어] opaque — spdk_bdev_for_each_channel 순회 상태 (내부에서 동적 할당)
                                   *  - 사용자는 fn 콜백의 인자로만 받아 spdk_bdev_for_each_channel_continue에 전달 */

/**
 * Called on the appropriate thread for each channel associated with the given bdev.
 *
 * \param i bdev channel iterator.
 * \param bdev Block device.
 * \param ch I/O channel.
 * \param ctx context of the bdev channel iterator.
 */
typedef void (*spdk_bdev_for_each_channel_msg)(struct spdk_bdev_channel_iter *i,
		struct spdk_bdev *bdev, struct spdk_io_channel *ch, void *ctx);
                                  /* [한국어] for_each_channel의 per-channel 콜백
                                   *  - @i: iterator (continue에 전달 필요)
                                   *  - @bdev: 대상 bdev
                                   *  - @ch: 현재 채널 (해당 채널 소유 스레드에서 호출됨)
                                   *  - @ctx: for_each_channel 호출 시 전달한 공유 컨텍스트
                                   *  - 콜백 내에서 동기/비동기 작업 후 반드시 spdk_bdev_for_each_channel_continue(i, status) 호출 */

/**
 * spdk_bdev_for_each_channel() function's final callback with the given bdev.
 *
 * \param bdev Block device.
 * \param ctx context of the bdev channel iterator.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_bdev_for_each_channel_done)(struct spdk_bdev *bdev, void *ctx, int status);
                                  /* [한국어] for_each_channel 최종 완료 콜백
                                   *  - @status: 모든 채널 정상 0, 또는 어느 fn이 비0 status로 continue 호출 시 그 값
                                   *  - 호출 스레드: for_each_channel을 처음 호출한 스레드 (스레드 메시지 경유) */

/**
 * Helper function to iterate the next channel for spdk_bdev_for_each_channel().
 *
 * \param i bdev channel iterator.
 * \param status Status for the bdev channel iterator;
 * for non 0 status remaining iterations are terminated.
 */
void spdk_bdev_for_each_channel_continue(struct spdk_bdev_channel_iter *i, int status);
/*
 * [한국어]
 * spdk_bdev_for_each_channel_continue - for_each_channel iteration 다음 단계로 진행
 *
 * 사용자 fn 콜백 내에서(동기 또는 비동기) 호출해야 iterator가 다음 채널로 이동.
 * @status: 0 성공, 비0이면 순회 조기 종료 + cpl에 전달
 * 비동기 지원: fn이 I/O 하나를 제출하고 그 완료 콜백에서 continue 호출 가능 → per-channel 비동기 작업 가능
 */

/**
 * Call 'fn' on each channel associated with the given bdev.
 *
 * This happens asynchronously, so fn may be called after spdk_bdev_for_each_channel
 * returns. 'fn' will be called for each channel serially, such that two calls
 * to 'fn' will not overlap in time. After 'fn' has been called, call
 * spdk_bdev_for_each_channel_continue() to continue iterating. Note that the
 * spdk_bdev_for_each_channel_continue() function can be called asynchronously.
 *
 * \param bdev 'fn' will be called on each channel associated with this given bdev.
 * \param fn Called on the appropriate thread for each channel associated with the given bdev.
 * \param ctx Context for the caller.
 * \param cpl Called on the thread that spdk_bdev_for_each_channel was initially called
 * from when 'fn' has been called on each channel.
 */
void spdk_bdev_for_each_channel(struct spdk_bdev *bdev, spdk_bdev_for_each_channel_msg fn,
				void *ctx, spdk_bdev_for_each_channel_done cpl);
/*
 * [한국어]
 * spdk_bdev_for_each_channel - ★ bdev의 모든 채널에 대해 작업 순차 실행 ★
 *
 * @fn: 각 채널 소유 스레드에서 호출될 콜백 (스레드 메시지 경유)
 * @ctx: fn/cpl에 전달할 공유 컨텍스트
 * @cpl: 모든 채널 순회 완료 후 시작 스레드에서 호출
 *
 * 보장:
 *   - 두 fn 호출이 시간상 겹치지 않음(serial) — 공유 ctx 업데이트 안전
 *   - fn 호출 후 반드시 continue를 불러야 다음 채널 진행
 *
 * 사용 패턴: QoS 업데이트, 히스토그램 수집, 통계 리셋, quiesce — 모든
 * 채널에 일관된 작업 수행이 필요할 때.
 *
 * 내부: bdev 코어의 reactor/channel 메시지 시스템(spdk_thread_send_msg)으로 순회.
 */

/**
 * Get controller attributes for the bdev.
 *
 * \param bdev Block device to query.
 * \return controller attributes for the bdev.
 */
union spdk_bdev_nvme_ctratt spdk_bdev_get_nvme_ctratt(struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_nvme_ctratt - bdev의 컨트롤러 어트리뷰트 비트맵 조회
 *
 * 반환: union spdk_bdev_nvme_ctratt — bits.fdps 등 검사 가능
 * 사용처: 사용자가 _ext API로 cdw12.dtype=2(FDP) 전달 전 fdps 비트 확인.
 *
 * 비-NVMe bdev이라도 모듈이 같은 의미로 노출 가능 (spdk_bdev_register 시 설정).
 */

/**
 * Get NVMe namespace ID for a given bdev (only for NVMe bdevs).
 *
 * \param bdev Block device to query.
 *
 * \return Namespace ID or 0 if it's not available.
 */
uint32_t spdk_bdev_get_nvme_nsid(struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_nvme_nsid - NVMe bdev의 Namespace ID 조회
 *
 * @return: NSID (1..N), 또는 0 — NVMe bdev이 아니거나 NSID 미노출
 *
 * 사용처:
 *   - NVMe-oF 타겟이 호스트로 보낼 NSID 매핑 빌드
 *   - RPC bdev_nvme_get_controllers 응답에서 namespace 식별
 *   - admin passthru에서 NSID 인자 자동 주입
 *
 * 일반 bdev API는 LBA 기반이므로 NSID 직접 사용 거의 없음 — passthru 경로 한정.
 */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_H_ */
