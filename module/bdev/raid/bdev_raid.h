/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK RAID 코어 내부 헤더 (bdev_raid.h)
 *
 * === 파일의 역할 ===
 * 본 헤더는 SPDK bdev RAID 프레임워크 (lib/bdev_raid.c, bdev_raid_sb.c, bdev_raid_rpc.c, 그리고
 * 각 레벨별 모듈 raid0/raid1/raid5f/concat) 사이에서 공유되는 자료구조, 상수, enum, 인라인
 * 헬퍼, 모듈 등록 매크로를 모두 정의한다. 외부 공개 API(include/spdk/raid_bdev.h 등)와
 * 별개로 RAID 모듈 내부에서만 보이는 "internal" 헤더 역할을 한다 (가드 매크로
 * SPDK_BDEV_RAID_INTERNAL_H 가 그 사실을 표명).
 *
 * 핵심 데이터 모델은 다음과 같다:
 *  - struct raid_bdev          : RAID 가상 디바이스 자체. spdk_bdev를 첫 필드로 임베딩.
 *  - struct raid_base_bdev_info: RAID에 속하는 각 base bdev (디스크) 정보.
 *  - struct raid_bdev_io       : RAID I/O 요청 컨텍스트 (spdk_bdev_io의 driver_ctx).
 *  - struct raid_bdev_module   : RAID 레벨별 모듈 디스크립터 (vtable + 메타데이터).
 *  - struct raid_bdev_superblock: 디스크에 영속되는 RAID 메타데이터 (256B 고정 + 가변 base 배열).
 *
 * 또한 base bdev에 발행하는 R/W/UNMAP/FLUSH 헬퍼들이 인라인으로 제공되어, 각 RAID 모듈은
 * data_offset 보정과 DIX REFTAG remap을 별도 처리하지 않고 그대로 호출만 하면 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   사용자 spdk_bdev_read/write
 *     → lib/bdev (raid_bdev이 등록된 가상 bdev)
 *       → bdev_raid.c (raid_bdev_submit_request) — 본 헤더의 raid_bdev_io_init / channel API 사용
 *         → 본 헤더의 raid_bdev_module->submit_rw_request (raid0/raid1/raid5f/concat 중 1)
 *           → 본 헤더의 raid_bdev_*_blocks_ext / unmap_blocks / flush_blocks 인라인 헬퍼
 *             → spdk_bdev_*_blocks_ext (lib/bdev) → base bdev 모듈
 *
 * 영속성 경로:
 *   bdev_raid_create → raid_bdev_alloc_superblock → raid_bdev_init_superblock
 *     → raid_bdev_write_superblock → 모든 base bdev에 fan-out write
 *   기동 시 examine → raid_bdev_load_base_bdev_superblock → sb 검증 → RAID 재구성
 *
 * 실행 컨텍스트는 SPDK reactor 스레드. 본 헤더 자체는 코드 흐름이 없고 자료구조 정의만이
 * 모든 모듈에 공유된다.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/bdev_module.h: spdk_bdev, spdk_bdev_io, spdk_bdev_desc, spdk_bdev_io_wait_entry,
 *   spdk_bdev_*_blocks_ext, spdk_io_channel 등 lib/bdev 인터페이스.
 * - spdk/uuid.h: spdk_uuid 타입과 헬퍼.
 * - bdev_raid.c: 본 헤더의 외부 함수들의 실제 구현 (raid_bdev_create, raid_bdev_io_complete,
 *   raid_bdev_channel_get_*, raid_bdev_module_list_add 등).
 * - bdev_raid_sb.c: superblock 관련 함수들의 구현.
 * - bdev_raid_rpc.c: JSON-RPC 핸들러로 RAID 인스턴스 생성/제거/조회.
 * - raid0.c / raid1.c / raid5f.c / concat.c: 각 레벨 모듈, RAID_MODULE_REGISTER로 자동 등록.
 *
 * === 주요 함수/구조체 요약 ===
 * - enum raid_level / raid_bdev_state / raid_process_type : 식별자 enum.
 * - struct raid_base_bdev_info : 디스크 슬롯 정보 (desc, data_offset/size, 상태 플래그).
 * - struct raid_bdev_io        : I/O 컨텍스트 (driver_ctx). 코어와 모듈이 공유.
 * - struct raid_bdev           : RAID 가상 디바이스 (spdk_bdev 임베딩 + RAID 메타).
 * - struct raid_bdev_module    : 모듈 vtable (level, start/stop, submit_rw, submit_null, ...).
 * - struct raid_bdev_superblock: 디스크 영속 메타 (256B 고정 부분 + base_bdevs[] 가변).
 * - 인라인 헬퍼 raid_bdev_readv/writev/unmap/flush_blocks : data_offset 보정 + DIX 재매핑.
 * - RAID_MODULE_REGISTER       : 모듈을 코어 리스트에 자동 등록 (constructor attribute).
 * - RAID_FOR_EACH_BASE_BDEV    : base_bdev_info[] 순회 매크로.
 */

#ifndef SPDK_BDEV_RAID_INTERNAL_H
#define SPDK_BDEV_RAID_INTERNAL_H

/* [한국어] spdk/bdev_module.h: lib/bdev이 노출하는 모듈 측 API. spdk_bdev, spdk_bdev_io,
 * spdk_bdev_desc, spdk_bdev_io_wait_entry, spdk_bdev_*_blocks_ext, spdk_io_channel 등을 사용. */
#include "spdk/bdev_module.h"
/* [한국어] spdk/uuid.h: struct spdk_uuid (16바이트 UUID), spdk_uuid_copy 등. RAID 인스턴스 및
 * base bdev의 영속 식별자로 사용. */
#include "spdk/uuid.h"

/* [한국어] base bdev의 LBA 0 ~ data_offset 사이는 RAID 메타데이터(superblock 등) 예약 영역.
 * 이 상수는 그 최소 크기 = 1 MiB. data_size 는 base bdev->blockcnt - data_offset 으로 계산.
 * RAID_BDEV_SB_MAX_LENGTH < RAID_BDEV_MIN_DATA_OFFSET_SIZE 이 만족됨을 SPDK_STATIC_ASSERT 로
 * 컴파일 시 보장. */
#define RAID_BDEV_MIN_DATA_OFFSET_SIZE	(1024*1024) /* 1 MiB */

/*
 * [한국어]
 * enum raid_level - RAID 레벨 식별자.
 *
 * 각 RAID 모듈(raid0/raid1/raid5f/concat)은 자신의 level 값을 g_*_module.level 로 등록하며,
 * 사용자 RPC에서 받은 문자열은 raid_bdev_str_to_level/level_to_str 로 변환한다. 값은
 * superblock에 저장되므로 변경하면 ABI 깨짐 → 새 값만 추가 가능.
 */
enum raid_level {
	INVALID_RAID_LEVEL	= -1,
	/* [한국어] 미식별/오류 케이스. raid_bdev_str_to_level이 알 수 없는 문자열 입력 시 반환. */

	RAID0			= 0,
	/* [한국어] RAID 0: 스트라이핑. 라운드 로빈으로 strip_size 단위 분배. redundancy 없음. */

	RAID1			= 1,
	/* [한국어] RAID 1: 미러링. 모든 base bdev에 동일 데이터 복제. N-way mirror. */

	RAID5F			= 95, /* 0x5f */
	/* [한국어] RAID 5F: full-stripe-only RAID 5 변형. 스트라이프 단위 write에서 패리티를 한 번에
	 * 계산해 기록 (read-modify-write 회피). SPDK의 accel offload(예: ISA-L) 친화적 변형. */

	CONCAT			= 99,
	/* [한국어] concat: 선형 이어붙이기. base bdev들을 LBA 공간에 순차 배치. concat.c 참고. */
};

/*
 * Raid state describes the state of the raid. This raid bdev can be either in
 * configured list or configuring list
 */
/*
 * [한국어]
 * enum raid_bdev_state - RAID 인스턴스 상태.
 *
 * 사용자에게 노출되는 가상 bdev의 가시성 (online vs configuring vs offline) 결정.
 */
enum raid_bdev_state {
	/* raid bdev is ready and is seen by upper layers */
	RAID_BDEV_STATE_ONLINE,
	/* [한국어] 정상 동작 중. lib/bdev에 등록되어 사용자가 spdk_bdev_open_ext로 열 수 있음. */

	/*
	 * raid bdev is configuring, not all underlying bdevs are present.
	 * And can't be seen by upper layers.
	 */
	RAID_BDEV_STATE_CONFIGURING,
	/* [한국어] 일부 base bdev이 아직 발견되지 않은 구성 중. examine 진행 중이거나 add_base_bdev
	 * 대기 상태. 외부에서 bdev로 보이지 않음. */

	/*
	 * In offline state, raid bdev layer will complete all incoming commands without
	 * submitting to underlying base nvme bdevs
	 */
	RAID_BDEV_STATE_OFFLINE,
	/* [한국어] 오프라인 상태. 들어오는 모든 I/O를 base bdev에 발행하지 않고 즉시 (실패 또는
	 * no-op) 완료. detach 진행 중에 in-flight 명령이 종료될 때까지 사용. */

	/* raid bdev state max, new states should be added before this */
	RAID_BDEV_STATE_MAX
	/* [한국어] enum 카운트 가드. 향후 새 상태는 이 위에 추가. */
};

/*
 * [한국어]
 * enum raid_process_type - RAID 백그라운드 프로세스 종류.
 *
 * 현재는 REBUILD 만 정의됨 (RAID 1/5F에서 새 base bdev로 데이터 복원/재계산).
 */
enum raid_process_type {
	RAID_PROCESS_NONE,
	/* [한국어] 백그라운드 프로세스 없음. 정상 동작 상태. */

	RAID_PROCESS_REBUILD,
	/* [한국어] 리빌드 진행 중. 새 또는 결손된 base bdev에 데이터 복원 (RAID 1: 복사,
	 * RAID 5F: 패리티 기반 재계산). 본 헤더에서는 process 포인터로만 추적. */

	RAID_PROCESS_MAX
	/* [한국어] enum 카운트 가드. */
};

/* [한국어] base bdev 구성/제거 작업 완료 콜백 타입. status: 0 성공, 음수 errno 실패. */
typedef void (*raid_base_bdev_cb)(void *ctx, int status);

/*
 * raid_base_bdev_info contains information for the base bdevs which are part of some
 * raid. This structure contains the per base bdev information. Whatever is
 * required per base device for raid bdev will be kept here
 */
/*
 * [한국어]
 * struct raid_base_bdev_info - RAID에 속한 단일 base bdev (디스크 슬롯) 메타데이터.
 *
 * raid_bdev::base_bdev_info[]에 num_base_bdevs 길이만큼 배치된다. 인덱스 = 슬롯 번호 = 라운드
 * 로빈 분배의 disk_idx 와 일치. 슬롯이 비어 있을 수 있으며 그 경우 desc == NULL 이고
 * is_configured == false. 모든 RAID 레벨 모듈이 본 구조체를 통해 base bdev에 접근.
 */
struct raid_base_bdev_info {
	/* The raid bdev that this base bdev belongs to */
	struct raid_bdev	*raid_bdev;
	/* [한국어] 이 base bdev이 속한 부모 raid_bdev 포인터. base_info → raid_bdev 역참조에 사용.
	 * 설정자: raid_bdev_create 시 base_bdev_info 배열을 만들 때 한 번. 읽는 자: 각 RAID 모듈. */

	/* name of the bdev */
	char			*name;
	/* [한국어] base bdev 이름 (사용자가 RPC로 제공한 문자열). examine/매칭 단계에서 사용.
	 * malloc된 문자열이며 raid_bdev 해체 시 free. */

	/* uuid of the bdev */
	struct spdk_uuid	uuid;
	/* [한국어] base bdev UUID. superblock 매칭 시 권위 식별자. name보다 충돌 가능성 적음. */

	/*
	 * Pointer to base bdev descriptor opened by raid bdev. This is NULL when the bdev for
	 * this slot is missing.
	 */
	struct spdk_bdev_desc	*desc;
	/* [한국어] RAID가 직접 소유한 base bdev descriptor. NULL이면 슬롯 결손 (디스크 미발견 또는
	 * 제거 진행 중). 모든 I/O 발행은 spdk_bdev_*_blocks_ext(desc, ...) 형태로 진행.
	 * 설정자: raid_bdev_add_base_bdev → spdk_bdev_open_ext.
	 * 읽는 자: 모든 RAID 레벨 모듈 + bdev_raid 코어.
	 * 동기화: app thread가 open/close, I/O 스레드는 read-only로 사용 → race 방지를 위해 모듈은
	 *         desc==NULL 검사를 명시적으로 수행. */

	/* offset in blocks from the start of the base bdev to the start of the data region */
	uint64_t		data_offset;
	/* [한국어] base bdev 시작점에서 데이터 영역까지의 오프셋 (블록). 메타데이터(superblock 등)을
	 * 위해 RAID_BDEV_MIN_DATA_OFFSET_SIZE 이상 확보. R/W 발행 시 raid_bdev_*_blocks_ext가 자동
	 * 보정 (offset_blocks += data_offset). */

	/* size in blocks of the base bdev's data region */
	uint64_t		data_size;
	/* [한국어] base bdev의 데이터 영역 크기 (블록). data_offset 이후 사용 가능 영역. RAID 모듈이
	 * 자기 정렬 정책에 맞춰 strip 경계로 내림 정렬해 사용 (예: raid0_start, concat_start). */

	/*
	 * When underlying base device calls the hot plug function on drive removal,
	 * this flag will be set and later after doing some processing, base device
	 * descriptor will be closed
	 */
	bool			remove_scheduled;
	/* [한국어] 핫플러그(드라이브 제거) 이벤트 수신 후 정리 대기 중 표식. 새 I/O 발행 차단,
	 * in-flight 완료 후 desc 닫고 슬롯 비움. superblock fan-out write 시에는 skip 대상. */

	/* callback for base bdev removal */
	raid_base_bdev_cb	remove_cb;
	void			*remove_cb_ctx;
	/* [한국어] 위 정리 작업 완료 시 호출될 사용자 콜백과 컨텍스트. RPC remove_base_bdev 핸들러가
	 * 등록. 두 필드는 한 쌍. */

	/* Hold the number of blocks to know how large the base bdev is resized. */
	uint64_t		blockcnt;
	/* [한국어] 마지막으로 인지한 base bdev 의 총 블록 수. resize 알림 처리 시 변경 감지에 사용.
	 * 설정자: 처음 등록 시 + base bdev resize 콜백.
	 * 읽는 자: raid_bdev::module->resize 콜백이 spdk_bdev->blockcnt와 비교 시 (간접). */

	/* io channel for the app thread */
	struct spdk_io_channel	*app_thread_ch;
	/* [한국어] app thread 전용 base bdev I/O 채널. 메타데이터 (superblock) write 같은 fan-out
	 * 작업이 app thread에서 수행되며, 그 채널은 이 필드를 사용. data path는 reactor 스레드별로
	 * 별도 채널을 raid_bdev_io_channel 안에 가짐. */

	/* Set to true when base bdev has completed the configuration process */
	bool			is_configured;
	/* [한국어] true = 슬롯이 정상 등록·검증 완료되어 I/O 가능. false = 결손/구성 중. RAID 코어가
	 * 상태 전이 + 모든 슬롯 configured 시 RAID_BDEV_STATE_ONLINE 으로 진입. */

	/* Set to true if this base bdev is the target of a background process */
	bool			is_process_target;
	/* [한국어] 리빌드 등 백그라운드 프로세스의 대상 슬롯 표식. 일반 R/W 라우팅에서는 정책에 따라
	 * 우선순위 조정 등에 사용. */

	/* Set to true to indicate that the base bdev is being removed because of a failure */
	bool			is_failed;
	/* [한국어] base bdev 실패로 강제 제거 중임을 표시. is_configured와 함께 종합적으로 슬롯 상태
	 * 결정. RAID 1/5F 등 redundancy 모듈은 failed 슬롯을 우회 (다른 미러로 read 등). */

	/* callback for base bdev configuration */
	raid_base_bdev_cb	configure_cb;
	void			*configure_cb_ctx;
	/* [한국어] base bdev이 구성 완료(is_configured=true) 또는 실패하면 호출되는 사용자 콜백과
	 * 컨텍스트. RPC add_base_bdev 핸들러가 등록. */
};

/* [한국어] 전방 선언. 본 구조체는 본 헤더 아래에서 정의. */
struct raid_bdev_io;
/* [한국어] RAID I/O 사용자(=상위 코어/모듈) 정의 완료 콜백. raid_bdev_io::completion_cb 를
 * 통해 코어 기본 완료 경로(spdk_bdev_io_complete)를 가로채서 RAID 자체 처리(예: 패리티 재계산
 * 후 재발행, 리빌드 추적 등)를 끼워 넣는 데 사용. */
typedef void (*raid_bdev_io_completion_cb)(struct raid_bdev_io *raid_io,
		enum spdk_bdev_io_status status);

/*
 * raid_bdev_io is the context part of bdev_io. It contains the information
 * related to bdev_io for a raid bdev
 */
/*
 * [한국어]
 * struct raid_bdev_io - RAID I/O 요청 컨텍스트 (spdk_bdev_io::driver_ctx).
 *
 * lib/bdev이 spdk_bdev_io를 만들 때 driver_ctx 영역을 driver context size 만큼 예약하며,
 * RAID 모듈은 그 영역을 raid_bdev_io 로 캐스팅해 사용한다. 따라서 spdk_bdev_io 와 raid_bdev_io
 * 는 1:1 라이프사이클. 본 구조체의 필드는 발행/완료 경로에서 모듈과 코어가 공동으로 갱신하며
 * 모두 발행 스레드 단일 소유라 lockless.
 */
struct raid_bdev_io {
	/* The raid bdev associated with this IO */
	struct raid_bdev *raid_bdev;
	/* [한국어] 이 I/O가 속한 RAID 가상 디바이스. raid_bdev_io_init에서 설정. */

	uint64_t offset_blocks;
	/* [한국어] 가상 RAID LBA 시작 (블록). 사용자가 spdk_bdev_read/write에 넘긴 값. */

	uint64_t num_blocks;
	/* [한국어] 발행 길이 (블록). 0 보다 큼. */

	struct iovec *iovs;
	/* [한국어] iovec 배열 포인터. spdk_bdev_io::u.bdev.iovs와 같은 메모리. */

	int iovcnt;
	/* [한국어] iovec 개수. iovs[0..iovcnt-1] 유효. */

	enum spdk_bdev_io_type type;
	/* [한국어] I/O 종류 (READ/WRITE/UNMAP/FLUSH 등). 모듈이 분기에 사용. */

	struct spdk_memory_domain *memory_domain;
	void *memory_domain_ctx;
	/* [한국어] iovs 의 메모리 도메인. RDMA/CUDA/메모리 매핑된 영역 등 비표준 메모리에서 zero-copy
	 * I/O를 가능하게 한다. RAID 모듈은 base bdev에 그대로 패스스루 (memory_domains_supported=true
	 * 인 경우만). 둘은 한 쌍. */

	void *md_buf;
	/* [한국어] DIF/T10-PI 메타데이터 버퍼. 보호 정보(reftag/guard/apptag)가 담김. NULL이면 메타
	 * 없음. 모듈이 base bdev로 패스스루하며 raid_bdev_writev_blocks_ext에서 reftag remap 수행. */

	/* WaitQ entry, used only in waitq logic */
	struct spdk_bdev_io_wait_entry	waitq_entry;
	/* [한국어] base bdev에 발행 시 ENOMEM이면 lib/bdev이 자원 회복 시 콜백을 부르도록 등록되는
	 * wait 엔트리. raid_bdev_queue_io_wait 가 채움. 한 raid_io 당 한 엔트리. */

	/* Context of the original channel for this IO */
	struct raid_bdev_io_channel	*raid_ch;
	/* [한국어] 이 I/O가 발행된 RAID I/O 채널 (per-thread). base bdev I/O 채널 모음을 통해 child
	 * 발행 시 사용. 본 헤더에선 incomplete type, 정의는 bdev_raid.c 내부. */

	/* Used for tracking progress on io requests sent to member disks. */
	uint64_t			base_bdev_io_remaining;
	/* [한국어] N-way fan-out 시 아직 완료 보고를 받지 못한 child 수. 0 → 첫 진입(아직 미초기화)
	 * 으로 모듈이 인지. 모듈이 첫 진입에 N으로 설정 후 raid_bdev_io_complete_part가 1씩 감소.
	 * 0 도달 시 코어가 자동으로 raid_bdev_io_complete 호출. */

	uint8_t				base_bdev_io_submitted;
	/* [한국어] N-way fan-out 시 이미 발행 시도된 child 수 (성공/실패/skip 포함). ENOMEM 재시도
	 * 시 이 값을 보고 이미 발행한 child를 건너뛴다. */

	enum spdk_bdev_io_status	base_bdev_io_status;
	/* [한국어] 누적된 child 상태값 (가장 나쁜 상태로 갱신). 모든 child 완료 시 사용자에게 통지. */

	/* This will be the raid_io completion status unless any base io's status is different. */
	enum spdk_bdev_io_status	base_bdev_io_status_default;
	/* [한국어] 기본 상태값. raid_bdev_io_set_default_status로 모듈이 사전 설정. RAID 1처럼 일부
	 * child 실패가 허용되는 경우 default=SUCCESS, 하나라도 다른 상태면 그 값으로 덮어씀. */

	/* Private data for the raid module */
	void				*module_private;
	/* [한국어] 모듈 전용 데이터. 예: RAID 5F가 stripe write 컨텍스트(파리티 버퍼 등)를 보관. */

	/* Custom completion callback. Overrides bdev_io completion if set. */
	raid_bdev_io_completion_cb	completion_cb;
	/* [한국어] 코어가 raid_bdev_io_complete 시 spdk_bdev_io_complete 대신 이 콜백을 부르도록
	 * 설정. 리빌드 등 RAID 자체 발행한 I/O가 사용자에게 노출되지 않도록 가로채는 데 사용. */

	struct {
		uint64_t		offset;
		struct iovec		*iov;
		struct iovec		iov_copy;
	} split;
	/* [한국어] strip 경계 분할 후 모듈이 임시로 사용할 split 보관 영역. offset = 분할 후 가상
	 * LBA, iov / iov_copy = 분할된 iovec 백업. RAID 5F 같이 자체 분할이 필요한 모듈에서 활용. */
};

/*
 * [한국어]
 * struct raid_bdev_process_request - 백그라운드 프로세스 (리빌드)용 I/O 요청.
 *
 * 사용자 I/O와 별개로 RAID 코어가 자체 발행하는 데이터 복원/패리티 재계산 I/O. 그 자체가
 * spdk_bdev_io 로서 base bdev에 발행되도록 driver_ctx에 raid_bdev_io를 임베딩 (재사용을 위해).
 * 필드 순서가 중요: bdev_io 와 raid_io 는 driver_ctx 가 곧 raid_io이므로 메모리 레이아웃이
 * 맞아야 한다.
 */
struct raid_bdev_process_request {
	struct raid_bdev_process *process;
	/* [한국어] 이 요청이 속한 백그라운드 프로세스. process->raid_bdev 으로 RAID 추적. */

	struct raid_base_bdev_info *target;
	/* [한국어] 복원/재계산 대상 base bdev (예: 새로 추가된 미러 디스크). */

	struct spdk_io_channel *target_ch;
	/* [한국어] target에 발행할 I/O 채널. */

	uint64_t offset_blocks;
	uint32_t num_blocks;
	/* [한국어] 처리할 LBA 범위 (가상 RAID LBA 기준). */

	struct iovec iov;
	/* [한국어] 단일 iovec (process I/O는 단일 contiguous 버퍼 사용). */

	void *md_buf;
	/* [한국어] DIF 메타 버퍼 (T10-PI 활성 시). NULL 가능. */

	/* bdev_io is raid_io's driver_ctx - don't reorder them!
	 * These are needed for re-using raid module I/O functions for process I/O. */
	struct spdk_bdev_io bdev_io;
	struct raid_bdev_io raid_io;
	/* [한국어] 임베딩된 spdk_bdev_io 와 그 driver_ctx 위치의 raid_bdev_io. 모듈의 submit_rw/
	 * submit_null 핸들러를 그대로 재사용해 process I/O를 처리하기 위함. 두 필드 순서 변경 금지. */

	TAILQ_ENTRY(raid_bdev_process_request) link;
	/* [한국어] 프로세스 내부 큐(아이들/인플라이트) 연결 노드. */
};

/* [한국어] raid_bdev_create 경로의 비동기 완료 콜백. cb_ctx 와 rc(0=성공/음수 errno)를 전달. */
typedef void (*raid_bdev_configure_cb)(void *cb_ctx, int rc);

/*
 * raid_bdev is the single entity structure which contains SPDK block device
 * and the information related to any raid bdev either configured or
 * in configuring list. io device is created on this.
 */
/*
 * [한국어]
 * struct raid_bdev - RAID 가상 디바이스 본체.
 *
 * 첫 필드 spdk_bdev 를 임베딩하여 lib/bdev이 raid_bdev::bdev 포인터로 디바이스를 식별/등록.
 * RAID 코어는 spdk_bdev_register(&raid_bdev->bdev) 호출로 lib/bdev에 노출시킨다. 단일 RAID
 * 인스턴스는 g_raid_bdev_list 에 연결되어 RPC/examine 시 순회 가능.
 */
struct raid_bdev {
	/* raid bdev device, this will get registered in bdev layer */
	struct spdk_bdev		bdev;
	/* [한국어] lib/bdev에 등록되는 가상 bdev 본체. 첫 필드 = container_of 패턴으로 raid_bdev →
	 * spdk_bdev 변환과 그 역변환을 가능케 함. blockcnt/blocklen/uuid/name 등 사용자 가시 속성. */

	/* the raid bdev descriptor, opened for internal use */
	struct spdk_bdev_desc		*self_desc;
	/* [한국어] RAID 코어가 자기 가상 bdev에 대한 자체 desc. 백그라운드 프로세스가 자기 자신에
	 * 대해 I/O를 발행할 필요가 있을 때 사용. 일반 사용자 desc와 별개. */

	/* link of raid bdev to link it to global raid bdev list */
	TAILQ_ENTRY(raid_bdev)		global_link;
	/* [한국어] 전역 RAID 리스트 g_raid_bdev_list 연결 노드. */

	/* array of base bdev info */
	struct raid_base_bdev_info	*base_bdev_info;
	/* [한국어] num_base_bdevs 길이의 base bdev 슬롯 배열. 인덱스 = 슬롯 = 라운드 로빈 disk_idx. */

	/* strip size of raid bdev in blocks */
	uint32_t			strip_size;
	/* [한국어] strip 크기 (블록). 2의 거듭제곱. RAID 0/5F의 라운드 분배 단위. */

	/* strip size of raid bdev in KB */
	uint32_t			strip_size_kb;
	/* [한국어] strip 크기 (KiB). 사용자 표시/RPC 용. = strip_size * blocklen / 1024. */

	/* strip size bit shift for optimized calculation */
	uint32_t			strip_size_shift;
	/* [한국어] log2(strip_size). 빠른 분할/모듈로 계산을 위한 시프트 값. */

	/* state of raid bdev */
	enum raid_bdev_state		state;
	/* [한국어] 현재 RAID 상태 (ONLINE/CONFIGURING/OFFLINE). */

	/* number of base bdevs comprising raid bdev  */
	uint8_t				num_base_bdevs;
	/* [한국어] 총 슬롯 수 (= base_bdev_info[] 길이). */

	/* number of base bdevs discovered */
	uint8_t				num_base_bdevs_discovered;
	/* [한국어] 현재까지 발견된(open된) 슬롯 수. CONFIGURING → ONLINE 전이 조건 체크에 사용. */

	/*
	 * Number of operational base bdevs, i.e. how many we know/expect to be working. This
	 * will be less than num_base_bdevs when starting a degraded array.
	 */
	uint8_t				num_base_bdevs_operational;
	/* [한국어] "동작 가능"으로 알려진 슬롯 수. 결손 상태로 시작한 degraded array 에서 num보다 작음. */

	/* minimum number of viable base bdevs that are required by array to operate */
	uint8_t				min_base_bdevs_operational;
	/* [한국어] 최소 동작 슬롯 수 (모듈이 요구). 예: RAID 0 = N (모두 필요), RAID 1 = 1 (하나면 OK),
	 * RAID 5F = N-1 (한 개 결손 허용). 이보다 작으면 RAID failed. */

	/* Raid Level of this raid bdev */
	enum raid_level			level;
	/* [한국어] RAID 레벨 (RAID0/1/5F/CONCAT). 모듈 디스패치에 사용. */

	/* Set to true if destroy of this raid bdev is started. */
	bool				destroy_started;
	/* [한국어] raid_bdev_delete 진입 후 true. in-flight I/O 종료 후 unregister 진행. */

	/* Module for RAID-level specific operations */
	struct raid_bdev_module		*module;
	/* [한국어] 이 RAID 레벨을 처리하는 모듈 vtable. raid_bdev_create 시 level로 검색해 설정. */

	/* Private data for the raid module */
	void				*module_private;
	/* [한국어] 모듈 전용 데이터. 예: concat의 block_range[], RAID 5F의 stripe 메타. */

	/* Superblock */
	bool				superblock_enabled;
	/* [한국어] 영속 superblock 사용 여부. true면 모든 변경 시 모든 base bdev에 sb fan-out write. */

	struct raid_bdev_superblock	*sb;
	/* [한국어] 메모리 표현 superblock. raid_bdev_alloc_superblock으로 잡힘. */

	/* Superblock buffer used for I/O */
	void				*sb_io_buf;
	uint32_t			sb_io_buf_size;
	/* [한국어] 디스크 I/O에 직접 사용되는 sb 버퍼. MD-interleaved bdev면 sb와 별개, 아니면 sb와
	 * 동일 포인터. size는 디스크 측 정렬 길이 (blocklen 정수배). */

	/* Raid bdev background process, e.g. rebuild */
	struct raid_bdev_process	*process;
	/* [한국어] 활성 백그라운드 프로세스 (리빌드 등). NULL이면 없음. 본 헤더에는 incomplete type. */

	/* Callback and context for raid_bdev configuration */
	raid_bdev_configure_cb		configure_cb;
	void				*configure_cb_ctx;
	/* [한국어] 비동기 raid_bdev_create 완료 통지용 콜백/컨텍스트. */
};

/* [한국어] base_bdev_info[] 순회 매크로. i는 raid_base_bdev_info* 형 루프 변수.
 * for (i = first; i < end; i++) 패턴이며, i 자체가 포인터로 idx 산출은 raid_bdev_base_bdev_slot
 * 인라인 헬퍼로 수행. */
#define RAID_FOR_EACH_BASE_BDEV(r, i) \
	for (i = r->base_bdev_info; i < r->base_bdev_info + r->num_base_bdevs; i++)

/* [한국어] 전방 선언. 정의는 bdev_raid.c 내부 (raid_io 발행 채널 모음). */
struct raid_bdev_io_channel;

/* TAIL head for raid bdev list */
/* [한국어] 전역 RAID bdev 리스트 헤드 타입. 정의는 bdev_raid.c 에서 g_raid_bdev_list 인스턴스 생성. */
TAILQ_HEAD(raid_all_tailq, raid_bdev);

/* [한국어] 모든 RAID 인스턴스를 잇는 전역 리스트. RPC bdev_raid_get_bdevs / examine 시 순회. */
extern struct raid_all_tailq		g_raid_bdev_list;

/* [한국어] raid_bdev_delete 비동기 완료 콜백 타입. */
typedef void (*raid_bdev_destruct_cb)(void *cb_ctx, int rc);

/* [한국어] 다음 함수들은 외부에서 호출 가능한 RAID 코어 API. 구현은 bdev_raid.c. */
/* [한국어] 새 RAID 인스턴스 생성. base bdev은 별도로 raid_bdev_add_base_bdev로 추가. 성공 시
 * raid_bdev_out 에 포인터 반환. */
int raid_bdev_create(const char *name, uint32_t strip_size, uint8_t num_base_bdevs,
		     enum raid_level level, bool superblock, const struct spdk_uuid *uuid,
		     struct raid_bdev **raid_bdev_out);
/* [한국어] RAID 인스턴스 제거. 비동기 (in-flight I/O 종료까지 대기) — 완료 시 cb_fn 호출. */
void raid_bdev_delete(struct raid_bdev *raid_bdev, raid_bdev_destruct_cb cb_fn, void *cb_ctx);
/* [한국어] 슬롯에 base bdev 추가 (open). 비동기 — 완료/실패는 cb_fn 으로 통지. */
int raid_bdev_add_base_bdev(struct raid_bdev *raid_bdev, const char *name,
			    raid_base_bdev_cb cb_fn, void *cb_ctx);
/* [한국어] 이름으로 RAID 인스턴스 찾기. 없으면 NULL. */
struct raid_bdev *raid_bdev_find_by_name(const char *name);
/* [한국어] enum 문자열 변환 헬퍼들. RPC 핸들러에서 사용. */
enum raid_level raid_bdev_str_to_level(const char *str);
const char *raid_bdev_level_to_str(enum raid_level level);
enum raid_bdev_state raid_bdev_str_to_state(const char *str);
const char *raid_bdev_state_to_str(enum raid_bdev_state state);
const char *raid_bdev_process_to_str(enum raid_process_type value);
/* [한국어] RPC bdev_raid_get_bdevs 응답 JSON 작성. */
void raid_bdev_write_info_json(struct raid_bdev *raid_bdev, struct spdk_json_write_ctx *w);
/* [한국어] base bdev이 사라지거나 사용자가 제거를 요청했을 때 RAID에서 떼어내기. cb로 완료 통지. */
int raid_bdev_remove_base_bdev(struct spdk_bdev *base_bdev, raid_base_bdev_cb cb_fn, void *cb_ctx);

/*
 * RAID module descriptor
 */
/*
 * [한국어]
 * struct raid_bdev_module - RAID 레벨별 모듈 vtable.
 *
 * 각 레벨 모듈(raid0/raid1/raid5f/concat)이 자기 정적 인스턴스를 RAID_MODULE_REGISTER로 등록.
 * RAID 코어는 raid_bdev::level 로 모듈을 찾아 콜백 디스패치.
 */
struct raid_bdev_module {
	/* RAID level implemented by this module */
	enum raid_level level;
	/* [한국어] 이 모듈이 처리할 RAID 레벨. enum raid_level 의 한 값. */

	/* Minimum required number of base bdevs. Must be > 0. */
	uint8_t base_bdevs_min;
	/* [한국어] 최소 base bdev 수. RAID 0/concat=1, RAID 1=2, RAID 5F=3 등. 0 불가. */

	/*
	 * RAID constraint. Determines number of base bdevs that can be removed
	 * without failing the array.
	 */
	struct {
		enum {
			CONSTRAINT_UNSET = 0,
			CONSTRAINT_MAX_BASE_BDEVS_REMOVED,
			CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL,
		} type;
		uint8_t value;
	} base_bdevs_constraint;
	/* [한국어] 슬롯 결손 허용 정책. UNSET = 모든 슬롯 필요 (RAID 0/concat). MAX_BASE_BDEVS_REMOVED
	 * = 최대 N개 결손 허용 (RAID 5F는 1). MIN_BASE_BDEVS_OPERATIONAL = 최소 N개 동작 (RAID 1는 1).
	 * RAID 코어가 detach/attach 시 이 정책으로 RAID 상태 전이를 결정. */

	/* Set to true if this module supports memory domains. */
	bool memory_domains_supported;
	/* [한국어] memory_domain 패스스루 가능 여부. true면 RDMA/CUDA 등 비표준 메모리에서 zero-copy. */

	/* Set to true if this module supports DIF/DIX */
	bool dif_supported;
	/* [한국어] T10-PI(DIF/DIX) 지원 여부. true면 reftag/guard/apptag 보호 정보를 처리한다. */

	/*
	 * Called when the raid is starting, right before changing the state to
	 * online and registering the bdev. Parameters of the bdev like blockcnt
	 * should be set here.
	 *
	 * Non-zero return value will abort the startup process.
	 */
	int (*start)(struct raid_bdev *raid_bdev);
	/* [한국어] 시작 콜백. 모듈 전용 자료 할당, blockcnt 설정 등. 0 성공, 음수 시 시작 abort. */

	/*
	 * Called when the raid is stopping, right before changing the state to
	 * offline and unregistering the bdev. Optional.
	 *
	 * The function should return false if it is asynchronous. Then, after
	 * the async operation has completed and the module is fully stopped
	 * raid_bdev_module_stop_done() must be called.
	 */
	bool (*stop)(struct raid_bdev *raid_bdev);
	/* [한국어] 종료 콜백 (선택). true 즉시 동기 종료 완료, false 비동기 — 완료 시 모듈이
	 * raid_bdev_module_stop_done()를 호출해 코어에 통지. */

	/* Handler for R/W requests */
	void (*submit_rw_request)(struct raid_bdev_io *raid_io);
	/* [한국어] R/W I/O 발행 콜백. 모듈이 base bdev로 라우팅/분할. */

	/* Handler for requests without payload (flush, unmap). Optional. */
	void (*submit_null_payload_request)(struct raid_bdev_io *raid_io);
	/* [한국어] FLUSH/UNMAP 발행 콜백 (선택). RAID 1/5F는 없는 게 자연스러우면 NULL. */

	/*
	 * Called when the bdev's IO channel is created to get the module's private IO channel.
	 * Optional.
	 */
	struct spdk_io_channel *(*get_io_channel)(struct raid_bdev *raid_bdev);
	/* [한국어] 모듈이 자기 io_device를 갖는 경우(예: stripe write 컨텍스트 풀)를 위해 채널 발급
	 * 콜백 제공. NULL이면 모듈 채널 없음. */

	/*
	 * Called when a base_bdev is resized to resize the raid if the condition
	 * is satisfied. Optional.
	 *
	 * Returns true if the resize was performed.
	 */
	bool (*resize)(struct raid_bdev *raid_bdev);
	/* [한국어] base bdev 크기 변경 시 RAID 가상 크기 재계산 (선택). true 변경됨. */

	/* Handler for raid process requests. Required for raid modules with redundancy. */
	int (*submit_process_request)(struct raid_bdev_process_request *process_req,
				      struct raid_bdev_io_channel *raid_ch);
	/* [한국어] 백그라운드 프로세스(리빌드) I/O 발행. RAID 1/5F는 필수, RAID 0/concat은 NULL. */

	TAILQ_ENTRY(raid_bdev_module) link;
	/* [한국어] 코어가 등록된 모든 모듈을 연결하는 리스트 노드. */
};

/* [한국어] RAID_MODULE_REGISTER 매크로의 내부 구현이 호출하는 등록 함수. bdev_raid.c. */
void raid_bdev_module_list_add(struct raid_bdev_module *raid_module);

/* [한국어] 토큰 결합 매크로 (식별자 충돌 방지용 __LINE__ 결합 패턴). */
#define __RAID_MODULE_REGISTER(line) __RAID_MODULE_REGISTER_(line)
#define __RAID_MODULE_REGISTER_(line) raid_module_register_##line

/* [한국어] RAID_MODULE_REGISTER(_module): _module 을 라이브러리 로드 시 자동으로 코어 모듈
 * 리스트에 등록. constructor attribute 가 main() 이전에 정적 함수를 실행. 매크로는 LINE 기반
 * 식별자로 같은 헤더를 여러 번 include 해도 충돌 안 함. */
#define RAID_MODULE_REGISTER(_module)					\
__attribute__((constructor)) static void				\
__RAID_MODULE_REGISTER(__LINE__)(void)					\
{									\
    raid_bdev_module_list_add(_module);					\
}

/* [한국어] RAID 코어가 노출하는 I/O 진행 헬퍼들. 모듈은 child 완료 시 raid_bdev_io_complete_part
 * 로 부분 보고하고, 발행 ENOMEM 시 raid_bdev_queue_io_wait로 자원 회복 대기 등록.
 * raid_bdev_io_complete은 즉시 부모 raid_io 종료 (단일 child 케이스). */
bool raid_bdev_io_complete_part(struct raid_bdev_io *raid_io, uint64_t completed,
				enum spdk_bdev_io_status status);
void raid_bdev_queue_io_wait(struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
			     struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn);
void raid_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status);
/* [한국어] 비동기 stop 완료를 코어에 통지 (모듈 stop이 false를 반환한 경우). */
void raid_bdev_module_stop_done(struct raid_bdev *raid_bdev);
/* [한국어] RAID 채널에서 idx 슬롯의 base bdev I/O 채널 가져오기. */
struct spdk_io_channel *raid_bdev_channel_get_base_channel(struct raid_bdev_io_channel *raid_ch,
		uint8_t idx);
/* [한국어] RAID 채널에서 모듈 전용 컨텍스트 가져오기 (모듈이 io_device 보유 시). */
void *raid_bdev_channel_get_module_ctx(struct raid_bdev_io_channel *raid_ch);
/* [한국어] base bdev → 슬롯 인덱스 + base_info 역검색 (RAID 채널 단위). */
struct raid_base_bdev_info *raid_bdev_channel_get_base_info(struct raid_bdev_io_channel *raid_ch,
		struct spdk_bdev *base_bdev);
/* [한국어] 백그라운드 프로세스 요청 완료 보고. */
void raid_bdev_process_request_complete(struct raid_bdev_process_request *process_req, int status);
/* [한국어] raid_bdev_io 필드 초기화 헬퍼. 코어가 spdk_bdev_io 도착 시 호출. */
void raid_bdev_io_init(struct raid_bdev_io *raid_io, struct raid_bdev_io_channel *raid_ch,
		       enum spdk_bdev_io_type type, uint64_t offset_blocks,
		       uint64_t num_blocks, struct iovec *iovs, int iovcnt, void *md_buf,
		       struct spdk_memory_domain *memory_domain, void *memory_domain_ctx);
/* [한국어] base bdev을 즉시 실패 상태로 표시 (모듈이 명시적 실패를 알릴 때 호출). */
void raid_bdev_fail_base_bdev(struct raid_base_bdev_info *base_info);

/*
 * [한국어]
 * raid_bdev_base_bdev_slot - base_info → 슬롯 인덱스 변환.
 *
 * raid_bdev::base_bdev_info 가 배열이므로 포인터 차이 = 인덱스. 슬롯 번호는 superblock의
 * sb_base_bdev::slot 으로도 영속된다.
 */
static inline uint8_t
raid_bdev_base_bdev_slot(struct raid_base_bdev_info *base_info)
{
	/* [한국어] 포인터 산술: 두 포인터 차이는 (sizeof(elem) 단위) 정수 인덱스. */
	return base_info - base_info->raid_bdev->base_bdev_info;
}

/*
 * [한국어]
 * raid_bdev_io_set_default_status - fan-out 시작 전 기본 누적 상태값 설정.
 *
 * 호출 조건: base_bdev_io_submitted == 0 (아직 child가 하나도 발행되지 않은 상태). 즉 발행
 * 루프 시작 직전에 모듈이 1회 호출. RAID 1처럼 일부 child 실패가 허용되는 모듈은 default를
 * SUCCESS로 두고 전체 실패 시점에만 FAILED로 덮어쓴다.
 */
static inline void
raid_bdev_io_set_default_status(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	/* [한국어] 발행 직전에만 호출 가능 (이후엔 누적 상태가 child 결과로 갱신되어야 함). */
	assert(raid_io->base_bdev_io_submitted == 0);
	/* [한국어] 현재 누적 상태 = default (둘 다 동기화). */
	raid_io->base_bdev_io_status = status;
	raid_io->base_bdev_io_status_default = status;
}

/* [한국어] DIX/DIF 메타데이터 헬퍼. RAID 가상 LBA → base bdev 로컬 LBA 매핑이 일어날 때
 * reftag(보호 정보의 LBA 필드)도 가상→로컬로 remap해야 하므로 raid_bdev_remap_dix_reftag,
 * 그리고 검증을 위한 raid_bdev_verify_dix_reftag 가 코어에서 제공됨. */
int raid_bdev_remap_dix_reftag(void *md_buf, uint64_t num_blocks,
			       struct spdk_bdev *bdev, uint32_t remapped_offset);
int raid_bdev_verify_dix_reftag(struct iovec *iovs, int iovcnt, void *md_buf,
				uint64_t num_blocks, struct spdk_bdev *bdev, uint32_t offset_blocks);

/**
 * Raid bdev I/O read/write wrapper for spdk_bdev_readv_blocks_ext function.
 */
/*
 * [한국어]
 * raid_bdev_readv_blocks_ext - base bdev에 readv 발행 + data_offset 자동 보정.
 *
 * 모듈은 LBA를 "데이터 영역 기준" 으로 전달하고, 본 헬퍼가 base_info->data_offset 을 더해
 * 실제 base bdev LBA로 환산. 이 일관된 추상화 덕분에 각 RAID 모듈 코드는 메타데이터 영역을
 * 신경 쓸 필요가 없다.
 */
static inline int
raid_bdev_readv_blocks_ext(struct raid_base_bdev_info *base_info, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt, uint64_t offset_blocks,
			   uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg,
			   struct spdk_bdev_ext_io_opts *opts)
{
	/* [한국어] data_offset을 더해 base bdev 실제 LBA로 변환 후 lib/bdev 호출. */
	return spdk_bdev_readv_blocks_ext(base_info->desc, ch, iov, iovcnt,
					  base_info->data_offset + offset_blocks, num_blocks, cb, cb_arg, opts);
}

/**
 * Raid bdev I/O read/write wrapper for spdk_bdev_writev_blocks_ext function.
 */
/*
 * [한국어]
 * raid_bdev_writev_blocks_ext - base bdev에 writev 발행 + data_offset 보정 + DIX reftag remap.
 *
 * write 경로에선 사용자 메타데이터의 reftag가 가상 RAID LBA 기준이므로, 디스크에 영구 저장
 * 되기 전에 base bdev 로컬 LBA 기준으로 remap해야 추후 read 시 base bdev 자체 PI 검증이
 * 일관된다 (READ 후 raid0_bdev_io_completion 에서 가상 LBA로 다시 검증하는 구조).
 */
static inline int
raid_bdev_writev_blocks_ext(struct raid_base_bdev_info *base_info, struct spdk_io_channel *ch,
			    struct iovec *iov, int iovcnt, uint64_t offset_blocks,
			    uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg,
			    struct spdk_bdev_ext_io_opts *opts)
{
	int rc;
	/* [한국어] base bdev 실제 LBA. */
	uint64_t remapped_offset_blocks = base_info->data_offset + offset_blocks;

	/* [한국어] DIF/DIX REFTAG 검사 활성 시 reftag 재매핑. spdk_unlikely 는 일반 경로(보호 비활성)
	 * 에서 분기 예측이 빠르도록 힌트. */
	if (spdk_unlikely(spdk_bdev_get_dif_type(&base_info->raid_bdev->bdev) != SPDK_DIF_DISABLE &&
			  (base_info->raid_bdev->bdev.dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK))) {
		/* [한국어] 가상 LBA → base bdev LBA 기준으로 metadata의 reftag 필드를 다시 계산. */
		rc = raid_bdev_remap_dix_reftag(opts->metadata, num_blocks, &base_info->raid_bdev->bdev,
						remapped_offset_blocks);
		if (rc != 0) {
			/* [한국어] 메타 형식 오류 등으로 remap 실패 → 발행 중단 (디스크에 잘못된 reftag
			 * 저장 방지). */
			return rc;
		}
	}

	/* [한국어] base bdev에 실제 writev 발행. */
	return spdk_bdev_writev_blocks_ext(base_info->desc, ch, iov, iovcnt,
					   remapped_offset_blocks, num_blocks, cb, cb_arg, opts);
}

/**
 * Raid bdev I/O read/write wrapper for spdk_bdev_unmap_blocks function.
 */
/*
 * [한국어]
 * raid_bdev_unmap_blocks - UNMAP 발행 + data_offset 보정.
 *
 * UNMAP은 데이터 버퍼가 없어 reftag remap 불요. data_offset만 더해 발행.
 */
static inline int
raid_bdev_unmap_blocks(struct raid_base_bdev_info *base_info, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return spdk_bdev_unmap_blocks(base_info->desc, ch, base_info->data_offset + offset_blocks,
				      num_blocks, cb, cb_arg);
}

/**
 * Raid bdev I/O read/write wrapper for spdk_bdev_flush_blocks function.
 */
/*
 * [한국어]
 * raid_bdev_flush_blocks - FLUSH 발행 + data_offset 보정.
 *
 * 본질적으로 LBA 범위는 정보적이며 일부 백엔드는 디바이스 전체 flush를 수행하지만, 인터페이스
 * 일관성을 위해 data_offset만큼 보정.
 */
static inline int
raid_bdev_flush_blocks(struct raid_base_bdev_info *base_info, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return spdk_bdev_flush_blocks(base_info->desc, ch, base_info->data_offset + offset_blocks,
				      num_blocks, cb, cb_arg);
}

/*
 * Definitions related to raid bdev superblock
 */
/*
 * [한국어]
 * RAID superblock 정의 영역. 디스크에 영속되는 메타데이터의 binary 레이아웃이므로
 * 변경은 반드시 version.major/minor 정책을 따르고 SPDK_STATIC_ASSERT 로 사이즈를 못 박아 둠.
 */

/* [한국어] superblock 메이저 버전. 비호환 변경 시 ++. 다른 메이저 = 사용 거부. */
#define RAID_BDEV_SB_VERSION_MAJOR	1
/* [한국어] superblock 마이너 버전. 호환 변경 시 ++. 미래 마이너 인지 시 경고만 출력. */
#define RAID_BDEV_SB_VERSION_MINOR	0

/* [한국어] RAID 이름 최대 길이 (NUL 포함 안 함, 단순 고정 64바이트). */
#define RAID_BDEV_SB_NAME_SIZE		64

/*
 * [한국어]
 * enum raid_bdev_sb_base_bdev_state - sb에 영속되는 base bdev 슬롯 상태.
 *
 * 메모리상 raid_base_bdev_info의 is_configured/is_failed 플래그와는 별개로, 디스크에 기록된
 * "공식" 상태. examine 시 이 값으로 슬롯 상태 복원.
 */
enum raid_bdev_sb_base_bdev_state {
	RAID_SB_BASE_BDEV_MISSING	= 0,
	/* [한국어] 슬롯 비어 있음 (디스크 결손/추가 대기). */
	RAID_SB_BASE_BDEV_CONFIGURED	= 1,
	/* [한국어] 정상 동작 중 슬롯. */
	RAID_SB_BASE_BDEV_FAILED	= 2,
	/* [한국어] 실패로 강제 제거된 슬롯. RAID 1/5F는 다른 슬롯으로 동작 가능. */
	RAID_SB_BASE_BDEV_SPARE		= 3,
	/* [한국어] hot spare 슬롯 (현재 데이터 없음, 리빌드 대상으로 전환 가능). */
};

/*
 * [한국어]
 * struct raid_bdev_sb_base_bdev - sb 안의 단일 base bdev 슬롯 디스크립터 (64바이트 고정).
 *
 * sb 본체 뒤에 가변 길이 배열로 base_bdevs_size 만큼 이어 붙어 있다.
 */
struct raid_bdev_sb_base_bdev {
	/* uuid of the base bdev */
	struct spdk_uuid	uuid;
	/* [한국어] 슬롯에 매핑된 base bdev UUID. examine 시 이 UUID로 base bdev을 찾아 매칭.
	 * 16바이트. */

	/* offset in blocks from base device start to the start of raid data area */
	uint64_t		data_offset;
	/* [한국어] base bdev에서 RAID 데이터 영역 시작 LBA. 메타 영역(자체 sb 등) 이후 위치. */

	/* size in blocks of the base device raid data area */
	uint64_t		data_size;
	/* [한국어] base bdev에서 RAID가 사용하는 데이터 영역 크기 (블록). strip 정렬됨. */

	/* state of the base bdev */
	uint32_t		state;
	/* [한국어] enum raid_bdev_sb_base_bdev_state 값. 영속 상태. */

	/* feature/status flags */
	uint32_t		flags;
	/* [한국어] 향후 확장용 비트 플래그 영역. 현재 정의 없음. */

	/* slot number of this base bdev in the raid */
	uint8_t			slot;
	/* [한국어] RAID 내 슬롯 번호 (0..num_base_bdevs-1). RAID 0/5F의 분배 인덱스로 사용. */

	uint8_t			reserved[23];
	/* [한국어] 64바이트 정렬을 위한 예약 영역. 향후 필드 추가 시 이 공간에서 사용. */
};
/* [한국어] 64바이트 고정 사이즈 보장. 변경 시 컴파일 실패로 ABI 깨짐 방지. */
SPDK_STATIC_ASSERT(sizeof(struct raid_bdev_sb_base_bdev) == 64, "incorrect size");

/*
 * [한국어]
 * struct raid_bdev_superblock - 디스크에 영속되는 RAID 메타데이터 (256B 고정 + 가변 base 배열).
 *
 * 디스크 LBA 0 영역에 sb_io_buf_size 만큼 기록된다. 모든 base bdev에 동일 sb를 fan-out write
 * 하여 어느 디스크에서 읽어도 같은 RAID를 식별/재구성할 수 있게 한다.
 */
struct raid_bdev_superblock {
#define RAID_BDEV_SB_SIG "SPDKRAID"
	/* [한국어] 매직 시그너처 매크로. 8바이트 ASCII "SPDKRAID". examine 빠른 reject에 사용. */

	uint8_t			signature[8];
	/* [한국어] 매직 바이트열. 다른 모듈/일반 데이터 디스크와 구분. */

	struct {
		/* incremented when a breaking change in the superblock structure is made */
		uint16_t	major;
		/* [한국어] 메이저 버전. 다르면 비호환 → 사용 거부. */
		/* incremented for changes in the superblock that are backward compatible */
		uint16_t	minor;
		/* [한국어] 마이너 버전. 더 높으면 forward compat 경고만. */
	} version;
	/* length in bytes of the entire superblock */
	uint32_t		length;
	/* [한국어] sb 본체 + base_bdevs[] 합산 길이 (바이트). parse 시 buf 크기 부족 판단용. */

	/* crc32c checksum of the entire superblock */
	uint32_t		crc;
	/* [한국어] 전체 sb (CRC 자체는 0으로 두고 계산) 의 CRC32C. 무결성 검증용. */

	/* feature/status flags */
	uint32_t		flags;
	/* [한국어] 향후 확장용 비트 플래그. 현재 정의 없음. */

	/* unique id of the raid bdev */
	struct spdk_uuid	uuid;
	/* [한국어] RAID 인스턴스 UUID. 여러 base bdev이 같은 RAID에 속함을 식별. */

	/* name of the raid bdev */
	uint8_t			name[RAID_BDEV_SB_NAME_SIZE];
	/* [한국어] RAID 이름 (NUL 패딩, 64바이트 고정). 사용자 친화적 식별자. */

	/* size of the raid bdev in blocks */
	uint64_t		raid_size;
	/* [한국어] RAID 가상 디바이스 총 블록 수 (= bdev.blockcnt). 재구성 시 검증용. */

	/* the raid bdev block size - must be the same for all base bdevs */
	uint32_t		block_size;
	/* [한국어] 데이터 블록 크기 (모든 base bdev이 동일해야 함). MD-interleaved 케이스에서도
	 * 데이터 영역만의 블록 크기. */

	/* the raid level */
	uint32_t		level;
	/* [한국어] enum raid_level 값. 재구성 시 어느 모듈을 디스패치할지 결정. */

	/* strip (chunk) size in blocks */
	uint32_t		strip_size;
	/* [한국어] strip 크기 (블록). RAID 0/5F의 분배 단위. */

	/* state of the raid */
	uint32_t		state;
	/* [한국어] RAID 상태 (현재 사용 형태는 TODO 상태이며 sb_init 에선 미설정). */

	/* sequence number, incremented on every superblock update */
	uint64_t		seq_number;
	/* [한국어] 단조 증가 시퀀스 번호. write 직전 ++. 여러 base bdev에서 읽었을 때 가장 큰
	 * seq를 가진 sb가 가장 최신 (멀티 디스크 부분 write 동안 일부만 갱신된 상태 식별 가능). */

	/* number of raid base devices */
	uint8_t			num_base_bdevs;
	/* [한국어] 활성 base bdev 수. */

	uint8_t			reserved[118];
	/* [한국어] 향후 필드 확장 예약 영역. 처음에 빈 공간을 충분히 둬 마이너 호환 추가 가능. */

	/* size of the base bdevs array */
	uint8_t			base_bdevs_size;
	/* [한국어] base_bdevs[] 배열 크기. num_base_bdevs와 일반적으로 동일하지만, 동적 add/remove
	 * 진행 중에는 두 값이 다를 수 있도록 분리. */
	/* array of base bdev descriptors */
	struct raid_bdev_sb_base_bdev base_bdevs[];
	/* [한국어] 가변 길이 base bdev 슬롯 배열. base_bdevs_size 길이. C99 flexible array member. */
};
/* [한국어] sb 헤더(가변 부분 제외) 256바이트 보장. 변경 시 ABI 깨짐 방지. */
SPDK_STATIC_ASSERT(sizeof(struct raid_bdev_superblock) == 256, "incorrect size");

/* [한국어] sb의 최대 가능 길이 = 헤더 + 최대 슬롯(255) × 64. UINT8_MAX 가 슬롯 수의 자연 상한
 * (num_base_bdevs/base_bdevs_size 가 uint8_t 이므로). */
#define RAID_BDEV_SB_MAX_LENGTH (sizeof(struct raid_bdev_superblock) + UINT8_MAX * sizeof(struct raid_bdev_sb_base_bdev))

/* [한국어] sb 최대 길이가 데이터 시작 오프셋(1MiB) 이내로 들어옴을 보장. RAID 데이터 영역과
 * sb 영역이 겹치지 않게 보장하는 컴파일 시 검증. */
SPDK_STATIC_ASSERT(RAID_BDEV_SB_MAX_LENGTH < RAID_BDEV_MIN_DATA_OFFSET_SIZE,
		   "Incorrect min data offset");

/* [한국어] sb write/load 비동기 콜백 타입. */
typedef void (*raid_bdev_write_sb_cb)(int status, struct raid_bdev *raid_bdev, void *ctx);
typedef void (*raid_bdev_load_sb_cb)(const struct raid_bdev_superblock *sb, int status, void *ctx);

/* [한국어] sb 라이프사이클 / read / write API. 구현은 bdev_raid_sb.c. */
int raid_bdev_alloc_superblock(struct raid_bdev *raid_bdev, uint32_t block_size);
void raid_bdev_free_superblock(struct raid_bdev *raid_bdev);
void raid_bdev_init_superblock(struct raid_bdev *raid_bdev);
void raid_bdev_write_superblock(struct raid_bdev *raid_bdev, raid_bdev_write_sb_cb cb,
				void *cb_ctx);
int raid_bdev_load_base_bdev_superblock(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
					raid_bdev_load_sb_cb cb, void *cb_ctx);

/*
 * [한국어]
 * struct spdk_raid_bdev_opts - RAID 코어 전역 옵션.
 *
 * 백그라운드 프로세스 (리빌드 등)가 사용자 I/O와 자원을 공유하므로, 동시 윈도우 크기와
 * 대역폭 상한을 설정해 사용자 워크로드에 미치는 영향을 제어. RPC bdev_raid_set_options.
 */
struct spdk_raid_bdev_opts {
	/* Size of the background process window in KiB */
	uint32_t process_window_size_kb;
	/* [한국어] 한 번에 진행 중인 백그라운드 작업 윈도우 (KiB). 너무 크면 RAM 소모 증가. */

	/* Maximum bandwidth in MiB to process per second */
	uint32_t process_max_bandwidth_mb_sec;
	/* [한국어] 백그라운드 처리 대역폭 상한 (MiB/s). 0 = 제한 없음. 사용자 I/O 우선화. */
};

/* [한국어] 옵션 조회/설정. 구현은 bdev_raid.c. */
void raid_bdev_get_opts(struct spdk_raid_bdev_opts *opts);
int raid_bdev_set_opts(const struct spdk_raid_bdev_opts *opts);

#endif /* SPDK_BDEV_RAID_INTERNAL_H */
