/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK Flash Translation Layer 공개 API 헤더 (ftl.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 가 자체 구현한 host-managed Flash Translation Layer 의 공개
 * C API 를 선언한다. FTL 은 NAND 플래시의 특성(쓰기 전 erase 필요, wear-leveling,
 * bad block 관리, GC) 을 호스트에서 직접 다루기 위한 계층으로, 보통 ZNS(Zoned
 * Namespace) NVMe 또는 OCSSD(OpenChannel SSD) 같이 호스트가 erase/program
 * 단위를 직접 제어할 수 있는 디바이스 위에 올라간다. SPDK FTL 은 base bdev
 * (대용량 ZNS 또는 OCSSD)와 cache bdev (작은 NVMe SSD 일부) 두 개를 묶어 하나의
 * 일반 블록 디바이스(spdk_bdev) 처럼 노출한다. Logical → Physical 매핑(L2P),
 * Garbage Collection, write 합병/redirect, Compaction, fast/full shutdown 등이
 * 라이브러리 내부에서 자동 처리되며 사용자는 readv/writev/unmap 만 호출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 스택은 다음과 같다:
 *   [Application] → spdk_bdev_*  → [bdev_ftl 모듈(module/bdev/ftl)]
 *      → spdk_ftl_writev / readv / unmap  (이 헤더의 API)
 *      → [lib/ftl 내부] L2P 조회 → 캐시/베이스 bdev 로 라우팅
 *      → spdk_bdev_writev_blocks / readv_blocks → NVMe 드라이버
 *      → ZNS/OCSSD NVMe 디바이스
 * 실행 컨텍스트는 SPDK reactor 스레드 — 모든 API 는 비동기로 spdk_ftl_fn 콜백을
 * I/O 채널이 속한 스레드에서 호출한다. core_mask 로 지정된 추가 reactor 들이
 * relocation/compaction/GC 같은 백그라운드 작업을 분담한다.
 * 파일 자체는 헤더이므로 어떤 .c 도 호출하지 않으며, lib/ftl/*.c 와
 * module/bdev/ftl/*.c 가 본 헤더를 include 한다.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/uuid.h    : 디바이스 UUID 영구 식별자.
 * - spdk/thread.h  : spdk_io_channel, spdk_thread (콜백 컨텍스트).
 * - spdk/bdev.h    : 베이스/캐시 bdev 핸들. FTL 은 두 bdev 를 사용해 새 bdev 를 만든다.
 * - spdk_jsonrpc_request : RPC 핸들러에서 properties 를 JSON 으로 직렬화.
 * 데이터 흐름:
 *   사용자 LBA 쓰기 → cache bdev 에 우선 기록 (NV cache, persistence) →
 *   compaction 에 의해 base bdev 의 band/zone 으로 옮김 → L2P 갱신 →
 *   free band 부족 시 GC 가 valid 블록을 새 band 로 재배치 후 erase.
 * 공유 자료구조: 사용자가 직접 접근 불가능한 spdk_ftl_dev / ftl_io 는
 * 내부 자료구조로 캡슐화. 사용자는 핸들과 ftl_stats 만 본다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_ftl_init/fini             : FTL 라이브러리 전역 초기화/종료
 * - spdk_ftl_dev_init/dev_free     : 한 FTL 디바이스의 비동기 생성/해제
 * - spdk_ftl_writev/readv/unmap    : 사용자 I/O (비동기, cb_fn 호출)
 * - spdk_ftl_get_attrs/get_conf    : 디바이스 속성/구성 조회
 * - spdk_ftl_get_stats             : 디바이스 통계 (read/write/GC/wear)
 * - spdk_ftl_get_properties / set_property : RPC 용 동적 프로퍼티 인터페이스
 * - spdk_ftl_dev_set_fast_shutdown : 빠른 종료 모드 활성화 (메타데이터를 SHM 에 유지)
 * - struct spdk_ftl_conf           : 디바이스 생성 구성 (overprovisioning, cache 비율, 코어마스크 등)
 * - struct spdk_ftl_attrs          : 노출 LBA 수/블록 크기/optimal IO 크기
 * - struct ftl_stats               : limits, io_activity_total, 7가지 카테고리별 read/write 통계
 * - enum ftl_stats_type            : USER/CMP/GC/MD_BASE/MD_NV_CACHE/L2P 통계 분류
 * - SPDK_FTL_LIMIT_*               : 라이트 스로틀 4단계 (CRIT/HIGH/LOW/START)
 */

#ifndef SPDK_FTL_H
#define SPDK_FTL_H

#include "spdk/stdinc.h"
/* [한국어] 표준 C 헤더 묶음. uint64_t/size_t/bool 등 타입 제공. */

#include "spdk/uuid.h"
/* [한국어] struct spdk_uuid (16B UUID) 정의.
 * spdk_ftl_conf.uuid 에 사용되어 디바이스 영구 식별자를 부여한다 — 디스크에
 * 저장된 메타데이터의 superblock 과 매칭해 디바이스 복원 시 일관성 검사. */

#include "spdk/thread.h"
/* [한국어] spdk_io_channel, spdk_thread, spdk_poller 정의.
 * FTL 의 모든 I/O 는 spdk_io_channel 단위로 동작하며, 콜백은 채널이 속한
 * SPDK 스레드(reactor) 에서 호출된다. core_mask 의 보조 스레드들도 여기서 관리. */

#include "spdk/bdev.h"
/* [한국어] spdk_bdev / spdk_bdev_desc / iovec 관련 정의.
 * FTL 은 base/cache bdev 에 의존하며, 사용자에게도 일반 bdev 처럼 노출된다.
 * iovec 기반 readv/writev API 를 위해 필요. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 사용자가 본 헤더를 include 할 때 C 링크 규칙 적용. */
#endif

struct spdk_ftl_dev;
/* [한국어] FTL 디바이스 핸들 (불투명 구조체).
 * 설정자: spdk_ftl_dev_init() 의 콜백에서 dev 포인터를 사용자에게 전달.
 * 읽는 자: 모든 FTL API 의 첫 인자로 사용 (writev, readv, get_stats, ...).
 * 동기화: 한 dev 는 단일 코어(core_mask 의 첫 코어)에 owner thread 가 있으며
 *        cross-thread 호출은 spdk_thread_send_msg 로 위임. */

struct ftl_io;
/* [한국어] FTL I/O 객체 (불투명).
 * 사용자가 spdk_ftl_io_size() 로 크기를 얻고, 자체 메모리에서 할당해 readv/writev
 * 의 io 인자로 넘긴다. FTL 내부에서 L2P, sub-IO 분할, 재시도 상태 추적용으로 활용. */

struct spdk_jsonrpc_request;
/* [한국어] JSON-RPC 요청 핸들 (lib/jsonrpc 정의).
 * spdk_ftl_get_properties 가 비동기로 응답을 작성할 때 컨텍스트로 사용. */

/* Limit thresholds */
enum {
	/* [한국어] FTL 의 사용자 쓰기 throttling 단계. free band 수가 임계 이하로
	 * 떨어질 때 GC/Compaction/사용자 I/O 의 우선순위를 단계적으로 조정한다.
	 * 설정자: spdk_ftl_conf.limits[] 가 단계별 % 한계를 보유. 내부 상태 머신이
	 *        free 비율을 모니터링하며 단계를 전환. */
	SPDK_FTL_LIMIT_CRIT,
	/* [한국어] CRIT 단계 — free band 가 매우 부족. 사용자 쓰기 차단(또는 큰 폭
	 * 스로틀), GC 만 동작 허용. compaction 도 멈출 수 있다. */
	SPDK_FTL_LIMIT_HIGH,
	/* [한국어] HIGH 단계 — 사용자 쓰기 강한 스로틀, GC 강도 ↑. */
	SPDK_FTL_LIMIT_LOW,
	/* [한국어] LOW 단계 — 가벼운 스로틀, GC 본격 시작. */
	SPDK_FTL_LIMIT_START,
	/* [한국어] START 단계 — GC 활동 개시 시점. 정상 사용자 I/O 는 영향 없음. */
	SPDK_FTL_LIMIT_MAX
	/* [한국어] enum 카운트(=4) — 배열 크기로 사용. */
};

struct ftl_stats_error {
	/* [한국어] I/O 에러 카운트 카테고리.
	 * 설정자: lib/ftl 내부 — 베이스/캐시 bdev 콜백이 실패 코드 분류 후 증가.
	 * 읽는 자: spdk_ftl_get_stats 가 사용자 콜백에 복사해 전달. */
	uint64_t media;
	/* [한국어] 매체 에러 (NAND 비트 에러 정정 실패 등). 값 범위: 누적 카운터. */
	uint64_t crc;
	/* [한국어] 메타/데이터 CRC 미스매치. 손상 검출 시 증가. */
	uint64_t other;
	/* [한국어] 위 두 분류에 속하지 않는 일반 에러 (타임아웃, abort 등). */
};

struct ftl_stats_group {
	/* [한국어] read 또는 write 한 방향의 통계 묶음. */
	uint64_t ios;
	/* [한국어] I/O 요청 개수 (논리 단위). 분할되어 sub-IO 로 펴졌어도 1로 카운트. */
	uint64_t blocks;
	/* [한국어] 처리된 블록(섹터) 총 개수. block_size 와 곱하면 바이트. */
	struct ftl_stats_error errors;
	/* [한국어] 이 그룹에서 발생한 에러 카운트(매체/CRC/기타). */
};

struct ftl_stats_entry {
	/* [한국어] 한 카테고리에 대한 read/write 통계.
	 * 설정자: 내부 카운터 갱신. 읽는 자: get_stats 의 사용자.
	 * 동기화: dev owner thread 에서만 변경되어 lockless. */
	struct ftl_stats_group read;
	/* [한국어] 읽기 측 통계. */
	struct ftl_stats_group write;
	/* [한국어] 쓰기 측 통계. */
};

enum ftl_stats_type {
	/* [한국어] 통계 카테고리. ftl_stats.entries[] 의 인덱스로 사용. */
	FTL_STATS_TYPE_USER = 0,
	/* [한국어] 사용자(애플리케이션) 발 I/O. 외부에서 보이는 워크로드. */
	FTL_STATS_TYPE_CMP,
	/* [한국어] Compaction — NV cache → base bdev 로 데이터를 옮기는 내부 I/O. */
	FTL_STATS_TYPE_GC,
	/* [한국어] Garbage Collection — base bdev band 의 valid 블록을 다른 band 로
	 * 재배치 후 원본 erase. wear-leveling 과 free band 확보가 목적. */
	FTL_STATS_TYPE_MD_BASE,
	/* [한국어] Base bdev 메타데이터 영역 I/O (L2P 백업, band metadata 등). */
	FTL_STATS_TYPE_MD_NV_CACHE,
	/* [한국어] NV cache bdev 메타데이터 I/O. */
	FTL_STATS_TYPE_L2P,
	/* [한국어] L2P 매핑 페이지 자체에 대한 I/O (DRAM 미적재 영역 swap-in/out). */
	FTL_STATS_TYPE_MAX,
	/* [한국어] enum count — entries 배열 크기 결정. */
};

struct ftl_stats {
	/* [한국어] FTL 디바이스의 종합 통계.
	 * 설정자: 내부 카운터 (dev owner thread).
	 * 읽는 자: spdk_ftl_get_stats 의 비동기 콜백을 통해 사용자에게 전달.
	 * 동기화: 사용자가 자체 할당한 버퍼이며 콜백 시점에만 채워져 안전. */
	/* Number of times write limits were triggered by FTL writers
	 * (gc and compaction) dependent on number of free bands. GC starts at
	 * SPDK_FTL_LIMIT_START level, while at SPDK_FTL_LIMIT_CRIT compaction stops
	 * and only GC is allowed to work.
	 */
	uint64_t		limits[SPDK_FTL_LIMIT_MAX];
	/* [한국어] 각 limit 단계가 트리거된 누적 횟수.
	 * limits[CRIT] 이 자주 증가하면 베이스 디바이스 free band 가 만성 부족 →
	 * overprovisioning 을 늘리거나 워크로드 가벼운 시점에 trim 권장. */

	/* Total number of blocks with IO to the underlying devices
	 * 1. nv cache read/write
	 * 2. base bdev read/write
	 */
	uint64_t		io_activity_total;
	/* [한국어] 베이스/캐시 bdev 에 발행된 모든 블록 수의 합 (사용자+내부).
	 * write amplification = io_activity_total / (USER write blocks) 추정에 사용. */

	struct ftl_stats_entry	entries[FTL_STATS_TYPE_MAX];
	/* [한국어] 카테고리별 read/write 통계 (USER, CMP, GC, MD_BASE, MD_NV_CACHE, L2P). */
};

typedef void (*spdk_ftl_stats_fn)(struct ftl_stats *stats, void *cb_arg);
/* [한국어] spdk_ftl_get_stats 의 비동기 완료 콜백 타입.
 * @stats: 사용자가 미리 할당해 넘긴 ftl_stats 가 채워져 콜백에 다시 전달됨.
 * @cb_arg: 사용자 컨텍스트 그대로 전달.
 * 호출 컨텍스트: dev owner spdk_thread (호출 스레드와 다를 수 있음). */

/*
 * FTL configuration.
 *
 * NOTE: Do not change the layout of this structure. Only add new fields at the end.
 */
struct spdk_ftl_conf {
	/* [한국어] FTL 디바이스 생성 구성. ABI 호환을 위해 구조체 끝에만 필드 추가
	 * 가능, 중간 변경 금지. conf_size 로 호출자 ABI 버전을 식별.
	 * 설정자: 사용자가 spdk_ftl_get_default_conf 로 초기화 후 필요한 필드 수정.
	 * 읽는 자: spdk_ftl_dev_init. */
	/* Device's name */
	char					*name;
	/* [한국어] 디바이스 이름 (외부 노출 bdev 이름과 매칭).
	 * 메모리 소유자: 사용자 → spdk_ftl_conf_copy 시 deep copy. deinit 으로 해제. */

	/* Device UUID (valid when restoring device from disk) */
	struct spdk_uuid			uuid;
	/* [한국어] 영구 UUID (16B). 디스크 superblock 의 UUID 와 일치해야 정상 복원.
	 * MODE_CREATE 이면 새로 발급, 아니면 기존 디바이스 재오픈에 사용. */

	/* Percentage of base device blocks not exposed to the user */
	uint64_t				overprovisioning;
	/* [한국어] 사용자에게 노출하지 않고 GC/wear leveling 여유로 남길 베이스 블록의 %.
	 * 값 범위: 0~99. 높을수록 GC 부담↓, 노출 용량↓. 일반적으로 20~30. */

	/* l2p cache size that could reside in DRAM (in MiB) */
	size_t					l2p_dram_limit;
	/* [한국어] DRAM 에 캐시할 L2P 매핑 영역의 최대 크기(MiB).
	 * 부족분은 NV cache/베이스에 swap. 작을수록 DRAM 절약, 큰 워크로드에서 미스↑. */

	/* Core mask - core thread plus additional relocation threads */
	char					*core_mask;
	/* [한국어] FTL 이 사용할 CPU 코어 마스크 (16진수 문자열, 예: "0xF0").
	 * 첫 코어가 dev owner, 나머지는 relocation/GC 보조 스레드. */

	/* IO pool size per user thread */
	size_t					user_io_pool_size;
	/* [한국어] 사용자 스레드(채널) 당 미리 할당할 ftl_io 객체 풀 크기.
	 * 동시 진행 가능한 사용자 I/O 의 상한과 관련. */

	/* User writes limits */
	size_t					limits[SPDK_FTL_LIMIT_MAX];
	/* [한국어] 각 limit 단계의 free band 임계 백분율(%).
	 * 예: limits[START]=20 이면 free 가 20% 이하로 떨어지면 GC 시작. */

	/* FTL startup mode mask, see spdk_ftl_mode enum for possible values */
	uint32_t				mode;
	/* [한국어] 시작 모드 비트마스크 (현재는 SPDK_FTL_MODE_CREATE 만 정의).
	 * 1 이면 새 디바이스 생성, 0 이면 기존 디바이스 복원. */

	struct {
		/* Start compaction when full chunks exceed given % of entire chunks */
		uint32_t			chunk_compaction_threshold;
		/* [한국어] NV cache 의 full chunk 비율이 이 %를 넘으면 compaction 시작.
		 * 너무 낮으면 잦은 compaction 으로 write amp ↑, 너무 높으면 user I/O 정체. */

		/* Percentage of chunks to maintain free */
		uint32_t			chunk_free_target;
		/* [한국어] compaction 이 도달하려는 free chunk 비율 목표(%).
		 * compaction 종료 조건. */
	} nv_cache;
	/* [한국어] NV cache 동작 튜닝 파라미터 묶음. */

	/*
	 * This flags indicates that FTL during shutdown should execute all
	 * actions which are needed for upgrade to a new version
	 */
	bool					prep_upgrade_on_shutdown;
	/* [한국어] true 면 종료 시 새 버전 업그레이드를 위한 마이그레이션 절차 수행.
	 * 새 메타데이터 포맷으로 변환 후 종료. */

	/* In verbose mode, user is able to get access to additional advanced FTL properties.
	 *
	 * Advanced properties currently include entries, which will result in printing large amount of data
	 * (e.g. state of all bands, or chunks); or allow for receiving internal state of FTL (e.g. bands currently
	 * used for garbage collection) - live data which may be useful for profiling, or debugging.
	 */
	bool					verbose_mode;
	/* [한국어] true 면 spdk_ftl_get_properties 가 모든 band/chunk 상태와 GC 진행
	 * 등 고급 정보를 노출. 디버깅/프로파일링용 (출력량 매우 큼). */

	/* Hole at bytes 0x66 - 0x67. */
	uint8_t					reserved[2];
	/* [한국어] ABI 정렬을 위한 reserved 패딩(0x66-0x67). 항상 0 으로 두어야 함. */

	/* Name of base block device (zoned or non-zoned) */
	char					*base_bdev;
	/* [한국어] 베이스 블록 디바이스(주 저장 매체) 이름. ZNS NVMe 권장.
	 * 메모리 소유자: 사용자. */

	/* Name of cache block device (must support extended metadata) */
	char					*cache_bdev;
	/* [한국어] NV cache 용 작은 NVMe SSD bdev 이름. extended metadata 지원 필수
	 * (write 시 LBA 와 함께 추가 메타를 같이 기록할 수 있어야 함). */

	/* Enable fast shutdown path */
	bool					fast_shutdown;
	/* [한국어] true 면 종료 시 메타데이터를 디스크가 아닌 SHM 에 유지하여
	 * 다음 부팅에서 즉시 복원. 업그레이드 다운타임 단축에 유리.
	 * SHM 손실(전원차) 시에는 fall back 으로 정상 디스크 복원. */

	/* Hole at bytes 0x79 - 0x7f. */
	uint8_t					reserved2[7];
	/* [한국어] 추가 ABI 정렬용 패딩. */

	/*
	 * The size of spdk_ftl_conf according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 */
	size_t					conf_size;
	/* [한국어] 호출자 측 sizeof(spdk_ftl_conf). 라이브러리는 이 값으로
	 * 호환되는 ABI 범위를 판단하고, 신규 필드는 기본값으로 채운다.
	 * 사용자 책임: spdk_ftl_get_default_conf 호출 시 이 값을 sizeof(...) 로 넘겨야 함. */
} __attribute__((packed));
/* [한국어] packed: 컴파일러 자동 패딩 금지 → ABI 안정성 확보 (위치별 reserved 직접 관리). */
SPDK_STATIC_ASSERT(sizeof(struct spdk_ftl_conf) == 136, "Incorrect size");
/* [한국어] 구조체 크기가 정확히 136B 임을 컴파일 타임에 검증.
 * 누군가 필드 위치를 바꾸거나 추가하면 빌드 실패 → ABI 변형 즉시 감지. */

enum spdk_ftl_mode {
	/* Create new device */
	SPDK_FTL_MODE_CREATE = (1 << 0),
	/* [한국어] 비트 0 — 새 FTL 디바이스 생성. 미설정 시 기존 디바이스 복원 모드.
	 * spdk_ftl_conf.mode 에 OR 비트마스크로 적용. */
};

/*
 * FTL device attributes.
 *
 * NOTE: Do not change the layout of this structure. Only add new fields at the end.
 */
struct spdk_ftl_attrs {
	/* [한국어] FTL 디바이스 속성 (런타임 노출 정보).
	 * 설정자: 라이브러리 — spdk_ftl_dev_get_attrs 호출 시 채움.
	 * 읽는 자: 사용자 / bdev_ftl 모듈 — bdev 등록 시 사용. */
	/* Number of logical blocks */
	uint64_t			num_blocks;
	/* [한국어] 사용자에게 노출되는 LBA 개수.
	 * = base 블록수 * (1 - overprovisioning/100) - 메타 영역. */
	/* Logical block size */
	uint64_t			block_size;
	/* [한국어] 한 LBA 의 바이트 크기 (보통 4096). */
	/* Optimal IO size - bdev layer will split requests over this size */
	uint64_t			optimum_io_size;
	/* [한국어] FTL 이 한 번에 처리하기 효율적인 I/O 크기 (블록 단위).
	 * bdev 레이어가 이보다 큰 요청은 분할. 보통 band/zone 크기와 정렬. */
};

typedef void (*spdk_ftl_fn)(void *cb_arg, int status);
/* [한국어] 일반 FTL 비동기 완료 콜백 타입.
 * @cb_arg: 사용자 컨텍스트 그대로 전달.
 * @status: 0 = 성공, 음수 errno = 실패.
 * 호출 컨텍스트: I/O 채널이 속한 SPDK thread (또는 dev owner thread). */

typedef void (*spdk_ftl_init_fn)(struct spdk_ftl_dev *dev, void *cb_arg, int status);
/* [한국어] spdk_ftl_dev_init 전용 콜백.
 * @dev: 생성된 FTL 디바이스 핸들 (status==0 일 때만 유효).
 * @cb_arg: 사용자 컨텍스트.
 * @status: 0=success, 음수 errno=실패.
 * 호출 컨텍스트: dev 가 owner 로 선택한 reactor 스레드. */

/**
 * Initializes the FTL library.
 *
 * @return 0 on success, negative errno otherwise.
 */
/*
 * [한국어] spdk_ftl_init - FTL 라이브러리 전역 초기화.
 *
 * @return: 0=성공, 음수 errno=실패.
 *
 * spdk_ftl_dev_init 호출 전에 한 번 실행해야 한다. 내부 메모리 풀,
 * 캐시 영역, 메타데이터 reactor 스레드를 준비.
 * 호출 컨텍스트: SPDK 환경 초기화 직후, 메인 스레드.
 *
 * 호출 체인:
 *   spdk_app_start → user init_fn → spdk_ftl_init → (이후 spdk_ftl_dev_init 가능)
 */
int spdk_ftl_init(void);

/**
 * Deinitializes the FTL library.
 */
/*
 * [한국어] spdk_ftl_fini - FTL 라이브러리 전역 정리.
 *
 * 모든 FTL 디바이스가 spdk_ftl_dev_free 로 해제된 뒤 호출.
 * 호출 컨텍스트: SPDK 종료 시퀀스의 메인 스레드.
 *
 * 호출 체인:
 *   user shutdown → 모든 spdk_ftl_dev_free 완료 → spdk_ftl_fini → spdk_app_stop
 */
void spdk_ftl_fini(void);

/**
 * Initialize the FTL on the given pair of bdevs - base and cache bdev.
 * Upon receiving a successful completion callback user is free to use I/O calls.
 *
 * \param conf configuration for new device
 * \param cb callback function to call when the device is created
 * \param cb_arg callback's argument
 *
 * \return 0 if initialization was started successfully, negative errno otherwise.
 */
/*
 * [한국어] spdk_ftl_dev_init - FTL 디바이스 비동기 생성/복원.
 *
 * @conf: 디바이스 구성 (base/cache bdev 이름, UUID, 코어마스크, mode 등).
 *        spdk_ftl_conf_copy 로 deep copy 후 라이브러리 내부에서 보존.
 * @cb:   생성 완료/실패 시 호출될 콜백.
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 = 비동기 작업 성공적으로 큐잉, 음수 errno = 즉시 실패 (잘못된 conf 등).
 *
 * 동작:
 *  1) base/cache bdev 열기, sanity check (extended metadata, 크기 등)
 *  2) MODE_CREATE 면 새 superblock 작성, 아니면 기존 superblock 검증/L2P 복원
 *  3) 백그라운드 reactor 스레드 등록 (relocation/GC)
 *  4) 준비 완료 시 cb(dev, cb_arg, 0) 호출
 * 호출 컨텍스트: 임의 SPDK thread. 콜백은 dev owner 스레드에서.
 *
 * 호출 체인:
 *   bdev_ftl_create_bdev → spdk_ftl_dev_init → (내부 비동기 절차) → cb
 */
int spdk_ftl_dev_init(const struct spdk_ftl_conf *conf, spdk_ftl_init_fn cb, void *cb_arg);

/**
 * Deinitialize and free given device.
 *
 * \param dev device
 * \param cb callback function to call when the device is freed
 * \param cb_arg callback's argument
 *
 * \return 0 if deinitialization was started successfully, negative errno otherwise.
 */
/*
 * [한국어] spdk_ftl_dev_free - FTL 디바이스 비동기 해제.
 *
 * @dev: 대상 디바이스 핸들.
 * @cb:  해제 완료 시 호출될 콜백 (status=0 또는 음수 errno).
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0=비동기 작업 큐잉 성공, 음수=즉시 실패.
 *
 * 동작 (full shutdown):
 *  1) 진행 중 사용자 I/O drain
 *  2) NV cache → base 로 dirty 데이터 flush(compaction)
 *  3) L2P/메타데이터 superblock 영구 기록
 *  4) base/cache bdev close, dev 메모리 해제, cb 호출
 * fast_shutdown=true 면 일부 단계 생략하고 SHM 에 메타 보존.
 *
 * 호출 체인:
 *   bdev_ftl_destruct → spdk_ftl_dev_free → cb
 */
int spdk_ftl_dev_free(struct spdk_ftl_dev *dev, spdk_ftl_fn cb, void *cb_arg);

/**
 * Retrieve device’s attributes.
 *
 * \param dev device
 * \param attr Attribute structure to fill
 * \param attrs_size Must be set to sizeof(struct spdk_ftl_attrs)
 */
/*
 * [한국어] spdk_ftl_dev_get_attrs - 디바이스 속성(num_blocks, block_size, optimum_io_size) 조회.
 *
 * @dev: 디바이스 핸들.
 * @attr: 사용자 할당 버퍼 — 라이브러리가 채워준다.
 * @attrs_size: sizeof(struct spdk_ftl_attrs) — ABI 호환 검증용.
 *
 * 호출 컨텍스트: 임의 SPDK thread (값 읽기는 atomic-safe 한 monotonic 값들).
 *
 * 호출 체인:
 *   bdev_ftl_get_block_count/size → spdk_ftl_dev_get_attrs
 */
void spdk_ftl_dev_get_attrs(const struct spdk_ftl_dev *dev, struct spdk_ftl_attrs *attr,
			    size_t attrs_size);

/**
 * Retrieve device’s configuration.
 *
 * \param dev device
 * \param conf FTL configuration structure to fill
 * \param conf_size Must be set to sizeof(struct spdk_ftl_conf)
 */
/*
 * [한국어] spdk_ftl_dev_get_conf - 현재 디바이스에 적용된 구성 복사.
 *
 * @dev:  디바이스.
 * @conf: 사용자 할당 spdk_ftl_conf — 채워서 돌려준다.
 * @conf_size: sizeof(struct spdk_ftl_conf) — ABI 호환 검증.
 *
 * RPC 핸들러에서 현재 구성 덤프 또는 설정 변경 전 백업용으로 사용.
 *
 * 호출 체인:
 *   bdev_ftl_dump_info_json → spdk_ftl_dev_get_conf → JSON 직렬화
 */
void spdk_ftl_dev_get_conf(const struct spdk_ftl_dev *dev, struct spdk_ftl_conf *conf,
			   size_t conf_size);

/**
 * Obtain an I/O channel for the device.
 *
 * \param dev device
 *
 * \return A handle to the I/O channel or NULL on failure.
 */
/*
 * [한국어] spdk_ftl_get_io_channel - 현재 SPDK thread 에서 사용할 I/O 채널 획득.
 *
 * @dev: 디바이스.
 * @return: spdk_io_channel 핸들 또는 NULL (자원 부족 시).
 *
 * 사용 후에는 spdk_put_io_channel 로 해제. SPDK 의 채널 모델은 thread 별로
 * 별도 컨텍스트(자원 풀)를 갖게 하여 lockless 하게 I/O 를 발행할 수 있게 한다.
 *
 * 호출 체인:
 *   bdev_ftl_get_io_channel(bdev fn) → spdk_ftl_get_io_channel
 *   → spdk_get_io_channel(...) 내부 호출
 */
struct spdk_io_channel *spdk_ftl_get_io_channel(struct spdk_ftl_dev *dev);

/**
 * Make a deep copy of an FTL configuration structure
 *
 * \param dst The destination FTL configuration
 * \param src The source FTL configuration
 */
/*
 * [한국어] spdk_ftl_conf_copy - spdk_ftl_conf 의 깊은 복사 (문자열/포인터 포함).
 *
 * @dst: 복사 대상 구조체. 내부적으로 strdup 된 메모리를 보유 → 사용 후
 *       spdk_ftl_conf_deinit 로 해제 필수.
 * @src: 원본 구조체.
 * @return: 0=성공, 음수 errno=메모리 부족.
 *
 * 호출 컨텍스트: 일반 thread.
 */
int spdk_ftl_conf_copy(struct spdk_ftl_conf *dst, const struct spdk_ftl_conf *src);

/**
 * Release the FTL configuration resources. This does not free the structure itself.
 *
 * \param conf FTL configuration to deinitialize
 */
/*
 * [한국어] spdk_ftl_conf_deinit - spdk_ftl_conf 내부 자원(name/base_bdev/cache_bdev/core_mask) 해제.
 *
 * @conf: 해제할 구성. conf 자체 메모리는 해제하지 않음 (스택/사용자 소유).
 *
 * spdk_ftl_conf_copy 로 만든 사본은 반드시 이 함수로 deinit 한 뒤 폐기.
 */
void spdk_ftl_conf_deinit(struct spdk_ftl_conf *conf);

/**
 * Initialize FTL configuration structure with default values.
 *
 * \param conf FTL configuration to initialize
 * \param conf_size Must be set to sizeof(struct spdk_ftl_conf)
 */
/*
 * [한국어] spdk_ftl_get_default_conf - spdk_ftl_conf 를 기본값으로 채움.
 *
 * @conf: 사용자 할당 spdk_ftl_conf.
 * @conf_size: sizeof(struct spdk_ftl_conf) — ABI 식별자.
 *
 * 사용자는 일반적으로:
 *   struct spdk_ftl_conf c;
 *   spdk_ftl_get_default_conf(&c, sizeof(c));
 *   c.name = strdup("ftl0"); ...
 *   spdk_ftl_dev_init(&c, cb, arg);
 * 와 같이 사용.
 */
void spdk_ftl_get_default_conf(struct spdk_ftl_conf *conf, size_t conf_size);

/**
 * Submits a read to the specified device.
 *
 * \param dev Device
 * \param io Allocated ftl_io
 * \param ch I/O channel
 * \param lba Starting LBA to read the data
 * \param lba_cnt Number of sectors to read
 * \param iov Single IO vector or pointer to IO vector table
 * \param iov_cnt Number of IO vectors
 * \param cb_fn Callback function to invoke when the I/O is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
/*
 * [한국어] spdk_ftl_readv - LBA 범위에 대한 비동기 vectored 읽기.
 *
 * @dev: 디바이스.
 * @io:  사용자가 spdk_ftl_io_size() 만큼 할당한 ftl_io 버퍼 — 라이브러리가 내부 상태 저장.
 * @ch:  현재 thread 의 I/O 채널.
 * @lba: 시작 논리 블록 주소.
 * @lba_cnt: 읽을 블록 수.
 * @iov: scatter-gather buffer 배열.
 * @iov_cnt: iov 길이.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0=비동기 작업 시작 성공, 음수 errno=즉시 실패.
 *
 * 내부 동작:
 *  1) L2P 조회 → 각 LBA 의 물리 위치(NV cache 또는 base) 결정
 *  2) 위치별 sub-IO 로 분할 (cache hit / base read 분기)
 *  3) spdk_bdev_readv_blocks 발행, 모든 sub-IO 완료 후 cb_fn(cb_arg, status) 호출
 * 호출 컨텍스트: 채널이 속한 SPDK thread. 콜백도 동일 스레드에서.
 *
 * 호출 체인:
 *   bdev_ftl_submit_request(READ) → spdk_ftl_readv → ... → cb_fn
 */
int spdk_ftl_readv(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
		   uint64_t lba, uint64_t lba_cnt,
		   struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg);

/**
 * Submits a write to the specified device.
 *
 * \param dev Device
 * \param io Allocated ftl_io
 * \param ch I/O channel
 * \param lba Starting LBA to write the data
 * \param lba_cnt Number of sectors to write
 * \param iov Single IO vector or pointer to IO vector table
 * \param iov_cnt Number of IO vectors
 * \param cb_fn Callback function to invoke when the I/O is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
/*
 * [한국어] spdk_ftl_writev - LBA 범위에 대한 비동기 vectored 쓰기.
 *
 * @dev/@io/@ch/@lba/@lba_cnt/@iov/@iov_cnt/@cb_fn/@cb_arg: readv 와 동일.
 * @return: 0=성공, 음수 errno=실패.
 *
 * 내부 동작:
 *  1) NV cache 의 free chunk 에 데이터 + LBA 메타데이터 동시 기록
 *  2) L2P 갱신 (이전 위치는 invalid 마킹 — 추후 GC 대상)
 *  3) cache 압력이 임계 이상이면 background compaction 트리거
 *  4) 완료 시 cb_fn 호출.
 * limit 단계가 CRIT 이면 사용자 쓰기는 큐잉되며 spdk_bdev_io 가 SPDK_BDEV_IO_STATUS_NOMEM
 * 으로 재시도될 수 있다.
 *
 * 호출 체인:
 *   bdev_ftl_submit_request(WRITE) → spdk_ftl_writev → NV cache write → cb_fn
 */
int spdk_ftl_writev(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
		    uint64_t lba, uint64_t lba_cnt,
		    struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg);

/**
 * Submits a unmap to the specified device.
 *
 * \param dev Device
 * \param io Allocated ftl_io
 * \param ch I/O channel
 * \param lba Starting LBA to write the data
 * \param lba_cnt Number of blocks to unmap
 * \param cb_fn Callback function to invoke when the I/O is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
/*
 * [한국어] spdk_ftl_unmap - LBA 범위 unmap (TRIM/discard) 비동기 발행.
 *
 * @dev/@io/@ch: 위와 동일.
 * @lba/@lba_cnt: unmap 대상 LBA 범위.
 * @cb_fn/@cb_arg: 완료 콜백/컨텍스트.
 * @return: 0=성공, 음수 errno=실패.
 *
 * 내부 동작: L2P 에서 해당 LBA 들을 invalid 마킹 → 차후 GC 가 valid 블록만
 * 재배치 하여 free band 회수. 데이터 자체를 즉시 erase 하지 않음.
 * 효과: write amplification 감소, 가용 공간 확보.
 *
 * 호출 체인:
 *   bdev_ftl_submit_request(UNMAP) → spdk_ftl_unmap → L2P 갱신 → cb_fn
 */
int spdk_ftl_unmap(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
		   uint64_t lba, uint64_t lba_cnt, spdk_ftl_fn cb_fn, void *cb_arg);

/**
 * Returns the size of ftl_io struct that needs to be passed to spdk_ftl_read/write
 *
 * \return The size of struct
 */
/*
 * [한국어] spdk_ftl_io_size - struct ftl_io 의 sizeof 반환.
 *
 * @return: ftl_io 가 차지하는 바이트 수.
 *
 * struct ftl_io 는 불투명형이므로 사용자 코드는 sizeof 를 직접 쓸 수 없다.
 * 사용자는 이 함수가 반환한 크기만큼 메모리를 할당해 readv/writev/unmap 의
 * io 인자로 넘긴다 (예: bdev_io->driver_ctx 안에 임베드).
 *
 * 호출 체인:
 *   bdev_ftl init/per-IO setup → spdk_ftl_io_size → 메모리 할당
 */
size_t spdk_ftl_io_size(void);

/**
 * Enable fast shutdown.
 *
 * During fast shutdown FTL will keep the necessary metadata in shared memory instead
 * of serializing it to storage. This allows for shorter downtime during upgrade process.
 */
/*
 * [한국어] spdk_ftl_dev_set_fast_shutdown - 런타임에 fast shutdown 모드 토글.
 *
 * @dev: 디바이스.
 * @fast_shutdown: true 면 다음 spdk_ftl_dev_free 가 메타데이터를 SHM 에 보존
 *                (다음 부팅 시 즉시 복원), false 면 디스크에 정상 직렬화.
 *
 * 사용 예: SPDK 프로세스 라이브 업그레이드 직전에 true 로 설정해 다운타임 최소화.
 * 호출 컨텍스트: 임의 SPDK thread, owner 스레드 아닐 시 내부적으로 send_msg 됨.
 *
 * 호출 체인:
 *   bdev_ftl rpc("set_fast_shutdown") → spdk_ftl_dev_set_fast_shutdown
 */
void spdk_ftl_dev_set_fast_shutdown(struct spdk_ftl_dev *dev, bool fast_shutdown);

/*
 * Returns current FTL I/O statistics.
 *
 * \param dev Device
 * \param stats Allocated ftl_stats
 * \param cb_fn Callback function to invoke when the call is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
/*
 * [한국어] spdk_ftl_get_stats - FTL I/O 통계 비동기 수집.
 *
 * @dev: 디바이스.
 * @stats: 사용자 할당 ftl_stats 버퍼 — 라이브러리가 dev owner 스레드에서 채워서 cb 로 돌려줌.
 * @cb_fn: 완료 시 호출 (stats 와 cb_arg 인자).
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0=비동기 작업 큐잉 성공, 음수 errno=즉시 실패.
 *
 * 내부적으로 dev owner 스레드로 send_msg 후 카운터를 한꺼번에 스냅샷 → cb_fn 호출.
 * 이렇게 하는 이유: 카운터들이 owner 스레드에서만 갱신되므로 cross-thread atomic
 * 없이도 일관성 있는 스냅샷을 얻기 위함.
 *
 * 호출 체인:
 *   bdev_ftl_get_stats RPC → spdk_ftl_get_stats → owner thread → cb_fn
 */
int spdk_ftl_get_stats(struct spdk_ftl_dev *dev, struct ftl_stats *stats, spdk_ftl_stats_fn cb_fn,
		       void *cb_arg);

/**
 * Gets properties of the specified device.
 *
 * \param dev FTL device
 * \param request JSON RPC request where the properties will be stored
 * \param cb_fn Callback function to invoke when the operation is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
/*
 * [한국어] spdk_ftl_get_properties - 현재 디바이스의 모든 프로퍼티를 JSON-RPC 응답으로 직렬화.
 *
 * @dev: 디바이스.
 * @request: 미완료 jsonrpc 요청 — 라이브러리가 응답 본문을 작성한다.
 * @cb_fn: 완료 콜백 (RPC 응답 send 후 호출).
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0=성공, 음수 errno=실패.
 *
 * 출력에는 conf 의 일부 + 통계 + (verbose_mode 시) 모든 band/chunk 상태가 포함됨.
 *
 * 호출 체인:
 *   RPC 핸들러("bdev_ftl_get_properties") → spdk_ftl_get_properties
 *   → JSON 직렬화 → spdk_jsonrpc_send_response → cb_fn
 */
int spdk_ftl_get_properties(struct spdk_ftl_dev *dev, struct spdk_jsonrpc_request *request,
			    spdk_ftl_fn cb_fn, void *cb_arg);

/**
 * Sets the property of the specified device.
 *
 * \param dev FTL device
 * \param property The property name to be modified
 * \param value The new value to property
 * \param value_size The size of the value buffer
 * \param cb_fn Callback function to invoke when the operation is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
/*
 * [한국어] spdk_ftl_set_property - 디바이스 프로퍼티 동적 변경.
 *
 * @dev: 디바이스.
 * @property: 프로퍼티 이름 문자열 (NUL 종료).
 * @value: 새 값 (raw 바이트 또는 문자열).
 * @value_size: value 버퍼 길이.
 * @cb_fn/@cb_arg: 완료 콜백/컨텍스트.
 * @return: 0=성공, 음수 errno=실패 (지원 안 되는 property, 잘못된 value 등).
 *
 * 내부적으로 dev owner 스레드로 send_msg 한 뒤 안전하게 적용. 어떤 property 가
 * 지원되는지는 spdk_ftl_get_properties 결과의 키들로 확인.
 *
 * 호출 체인:
 *   RPC 핸들러("bdev_ftl_set_property") → spdk_ftl_set_property → owner thread → cb_fn
 */
int spdk_ftl_set_property(struct spdk_ftl_dev *dev, const char *property, const char *value,
			  size_t value_size, spdk_ftl_fn cb_fn, void *cb_arg);

#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 닫기. */
#endif

#endif /* SPDK_FTL_H */
/* [한국어] include guard 종료. */
