/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Block Device Module Interface
 *
 * For information on how to write a bdev module, see @ref bdev_module.
 */

/*
 * [한국어 설명] bdev 모듈 작성자용 인터페이스 (bdev_module.h) — 1972 라인
 *
 * === 파일의 역할 ===
 * bdev(SPDK Block Device 추상화) 서브시스템의 **"모듈 측"** 인터페이스를
 * 정의한다. 공개 헤더 `spdk/bdev.h`가 "사용자(애플리케이션) 측" API라면,
 * 이 헤더는 "NVMe/AIO/malloc/raid/lvol 등 실제 백엔드를 구현하는 bdev 모듈"이
 * 필요로 하는 타입·콜백·등록 API·I/O 요청 객체 구조를 제공한다.
 *
 * 주요 내용:
 *   (1) struct spdk_bdev_module — 모듈 생애주기 훅 (module_init/fini,
 *       examine, config dump, 등록)
 *   (2) struct spdk_bdev_fn_table — bdev 인스턴스별 모듈 콜백
 *       (★ submit_request 가 I/O 제출 진입점)
 *   (3) enum spdk_bdev_io_status / io_type — I/O 결과·종류 분류
 *   (4) struct spdk_bdev — bdev 인스턴스 메타데이터 (블록 크기, 지원 기능,
 *       UUID, 채널 생성 훅 등)
 *   (5) struct spdk_bdev_io_*_params — I/O 요청 종류별 페이로드 구조
 *       · block_params (read/write/unmap 등)
 *       · reset_params, abort_params
 *       · nvme_passthru_params
 *       · zone_mgmt_params
 *   (6) ★ struct spdk_bdev_io ★ — 단일 bdev I/O 요청 객체 (bdev 레이어의
 *       핵심 데이터)
 *   (7) I/O 상태 설정 헬퍼 (spdk_bdev_io_set_nvme_status, set_scsi_status,
 *       set_aio_status 등)
 *   (8) 모듈 등록 매크로, 파티션 bdev 도움 API
 *   (9) 채널/통계/RAID/ZBD 관련 부수 API
 *
 * === 전체 아키텍처에서의 위치 ===
 * 애플리케이션 I/O 경로:
 *   app → spdk_bdev_read() [bdev.h]
 *     → bdev 레이어: bdev_channel_get_io → bdev_io_init → bdev_io_submit [bdev_internal.h]
 *       → 모듈의 spdk_bdev_fn_table.submit_request(ch, bdev_io) [이 헤더 fn_table]
 *         예) module/bdev/nvme/bdev_nvme.c의 콜백이 bdev_io를 분석해
 *             spdk_nvme_ns_cmd_read/write로 변환해 NVMe 드라이버로 dispatch
 *
 * 완료 경로:
 *   모듈 콜백이 처리 후 spdk_bdev_io_complete(bdev_io, status) [bdev.h]
 *     → bdev 레이어가 상위 사용자 콜백 호출
 *
 * 실행 컨텍스트: 모든 bdev I/O는 I/O channel(=spdk_thread)에 고정. 한
 * bdev_io는 생성한 채널(즉 스레드)에서만 완료될 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/bdev.h (공개 I/O API, 타입 정의)
 *   - spdk/bdev_zone.h (ZNS 스펙 상수)
 *   - spdk/log.h, queue.h, thread.h, tree.h, util.h, uuid.h
 *   - spdk/scsi_spec.h (SCSI 에러 변환 경로)
 * 의존하는 모듈:
 *   - lib/bdev/bdev.c (bdev 코어) — 이 헤더의 타입을 구현
 *   - module/bdev/* (nvme, aio, malloc, raid, lvol, ...) — 모듈 작성자 사용
 *   - lib/nvmf/ctrlr_bdev.c (NVMe-oF target이 bdev로 I/O dispatch)
 * 공유 자료구조: spdk_bdev, spdk_bdev_io, spdk_bdev_module — bdev 코어와
 *   모듈이 공동 소유.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_bdev_module:          모듈 등록 루트
 *   - spdk_bdev_fn_table:        I/O 제출·채널·상태 조회 콜백 vtable
 *   - spdk_bdev:                 bdev 인스턴스 메타데이터
 *   - spdk_bdev_io:              단일 I/O 요청 (★ I/O 경로 중앙 객체 ★)
 *   - spdk_bdev_io_type:         READ/WRITE/UNMAP/FLUSH/RESET/PASSTHRU/COPY/...
 *   - spdk_bdev_io_status:       SUCCESS/FAILED/NVME_ERROR/SCSI_ERROR/ABORTED/...
 *   - SPDK_BDEV_MODULE_REGISTER: 모듈 정적 등록 매크로
 *
 * 본 주석은 파일 상단 블록을 제공한다. 개별 구조체(특히 `struct spdk_bdev_io`
 * 1125줄)는 후속 세션에서 섹션 분할로 점진 주석을 확장한다.
 */

#ifndef SPDK_BDEV_MODULE_H       /* [한국어] include 가드 */
#define SPDK_BDEV_MODULE_H

#include "spdk/stdinc.h"         /* [한국어] 표준 타입 */

#include "spdk/bdev.h"           /* [한국어] 공개 bdev 타입 (spdk_bdev_io_completion_cb 등) */
#include "spdk/bdev_zone.h"      /* [한국어] Zoned bdev 스펙 */
#include "spdk/log.h"            /* [한국어] SPDK_*LOG 매크로 */
#include "spdk/queue.h"          /* [한국어] TAILQ/STAILQ — 모듈·bdev 리스트 */
#include "spdk/scsi_spec.h"      /* [한국어] SCSI 센스 키 등 — 상태 변환 API 사용 */
#include "spdk/thread.h"         /* [한국어] spdk_io_channel — bdev channel 기반 */
#include "spdk/tree.h"           /* [한국어] RB 매크로 — 일부 bdev 내부 인덱스 */
#include "spdk/util.h"           /* [한국어] container_of 등 */
#include "spdk/uuid.h"           /* [한국어] bdev UUID 식별 */

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_BDEV_CLAIM_NAME_LEN	32
                                  /* [한국어] bdev claim 이름 문자열 최대 길이 (NUL 포함). 모듈이 "이 bdev는 내가 점유"를 등록할 때 사용 */

/* This parameter is best defined for bdevs that share an underlying bdev,
 * such as multiple lvol bdevs sharing an nvme device, to avoid unnecessarily
 * resetting the underlying bdev and affecting other bdevs that are sharing it. */
#define SPDK_BDEV_RESET_IO_DRAIN_RECOMMENDED_VALUE 5
                                  /* [한국어] reset 시 in-flight I/O drain 대기 시간 권장값 (초)
                                   *  - 공유 bdev(예: lvol 여러 개가 같은 NVMe 디바이스 사용)에서 불필요한 하위 reset 전파 방지
                                   *  - 모듈 작성자가 fn_table 내 해당 필드에 이 값을 사용할 수 있음 */

/** Block device module */
struct spdk_bdev_module {
	/**
	 * Initialization function for the module. Called by the bdev library
	 * during startup.
	 *
	 * Modules are required to define this function.
	 */
	int (*module_init)(void);

	/**
	 * Optional callback for modules that require notification of when
	 * the bdev subsystem has completed initialization.
	 *
	 * Modules are not required to define this function.
	 */
	void (*init_complete)(void);

	/**
	 * Optional callback for modules that require notification of when
	 * the bdev subsystem is starting the fini process. Called by
	 * the bdev library before starting to unregister the bdevs.
	 *
	 * If a module claimed a bdev without presenting virtual bdevs on top of it,
	 * it has to release that claim during this call.
	 *
	 * Modules are not required to define this function.
	 */
	void (*fini_start)(void);

	/**
	 * Finish function for the module. Called by the bdev library
	 * after all bdevs for all modules have been unregistered.  This allows
	 * the module to do any final cleanup before the bdev library finishes operation.
	 *
	 * Modules are not required to define this function.
	 */
	void (*module_fini)(void);

	/**
	 * Function called to return a text string representing the module-level
	 * JSON RPCs required to regenerate the current configuration.  This will
	 * include module-level configuration options, or methods to construct
	 * bdevs when one RPC may generate multiple bdevs (for example, an NVMe
	 * controller with multiple namespaces).
	 *
	 * Per-bdev JSON RPCs (where one "construct" RPC always creates one bdev)
	 * may be implemented here, or by the bdev's write_config_json function -
	 * but not both.  Bdev module implementers may choose which mechanism to
	 * use based on the module's design.
	 *
	 * \return 0 on success or Bdev specific negative error code.
	 */
	int (*config_json)(struct spdk_json_write_ctx *w);

	/** Name for the modules being defined. */
	const char *name;

	/**
	 * Returns the allocation size required for the backend for uses such as local
	 * command structs, local SGL, iovecs, or other user context.
	 */
	int (*get_ctx_size)(void);

	/**
	 * First notification that a bdev should be examined by a virtual bdev module.
	 * Virtual bdev modules may use this to examine newly-added bdevs and automatically
	 * create their own vbdevs, but no I/O to device can be send to bdev at this point.
	 * Only vbdevs based on config files can be created here. This callback must make
	 * its decision to claim the module synchronously.
	 * It must also call spdk_bdev_module_examine_done() before returning. If the module
	 * needs to perform asynchronous operations such as I/O after claiming the bdev,
	 * it may define an examine_disk callback.  The examine_disk callback will then
	 * be called immediately after the examine_config callback returns.
	 */
	void (*examine_config)(struct spdk_bdev *bdev);

	/**
	 * Second notification that a bdev should be examined by a virtual bdev module.
	 * Virtual bdev modules may use this to examine newly-added bdevs and automatically
	 * create their own vbdevs. This callback may use I/O operations and finish asynchronously.
	 * Once complete spdk_bdev_module_examine_done() must be called.
	 */
	void (*examine_disk)(struct spdk_bdev *bdev);

	/**
	 * Denotes if the module_init function may complete asynchronously. If set to true,
	 * the module initialization has to be explicitly completed by calling
	 * spdk_bdev_module_init_done().
	 */
	bool async_init;

	/**
	 * Denotes if the module_fini function may complete asynchronously.
	 * If set to true finishing has to be explicitly completed by calling
	 * spdk_bdev_module_fini_done().
	 */
	bool async_fini;

	/**
	 * Denotes if the fini_start function may complete asynchronously.
	 * If set to true finishing has to be explicitly completed by calling
	 * spdk_bdev_module_fini_start_done().
	 */
	bool async_fini_start;

	/**
	 * Fields that are used by the internal bdev subsystem. Bdev modules
	 *  must not read or write to these fields.
	 */
	struct __bdev_module_internal_fields {
		/**
		 * Protects action_in_progress and quiesced_ranges.
		 * Take no locks while holding this one.
		 */
		struct spdk_spinlock spinlock;

		/**
		 * Count of bdev inits/examinations in progress. Used by generic bdev
		 * layer and must not be modified by bdev modules.
		 *
		 * \note Used internally by bdev subsystem, don't change this value in bdev module.
		 */
		uint32_t action_in_progress;

		/**
		 * List of quiesced lba ranges in all bdevs of this module.
		 */
		TAILQ_HEAD(, lba_range) quiesced_ranges;

		TAILQ_ENTRY(spdk_bdev_module) tailq;
	} internal;
};

/** Claim types */
enum spdk_bdev_claim_type {
	/* Not claimed. Must not be used to request a claim. */
	SPDK_BDEV_CLAIM_NONE = 0,

	/**
	 * Exclusive writer, with allowances for legacy behavior.  This matches the behavior of
	 * `spdk_bdev_module_claim_bdev()` as of SPDK 22.09.  New consumer should use
	 * SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE instead.
	 */
	SPDK_BDEV_CLAIM_EXCL_WRITE,

	/**
	 * The descriptor passed with this claim request is the only writer. Other claimless readers
	 * are allowed.
	 */
	SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE,

	/**
	 * Any number of readers, no writers. Readers without a claim are allowed.
	 */
	SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE,

	/**
	 * Any number of writers with matching shared_claim_key. After the first writer establishes
	 * a claim, future aspiring writers should open read-only and pass the read-only descriptor.
	 * If the shared claim is granted to the aspiring writer, the descriptor will be upgraded to
	 * read-write.
	 */
	SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED
};

/** Options used when requesting a claim. */
struct spdk_bdev_claim_opts {
	/* Size of this structure in bytes */
	size_t opts_size;
	/**
	 * An arbitrary name for the claim. If set, it should be a string suitable for printing in
	 * error messages. Must be '\0' terminated.
	 */
	char name[SPDK_BDEV_CLAIM_NAME_LEN];
	/**
	 * Used with SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED claims. Any non-zero value is considered
	 * a key.
	 */
	uint64_t shared_claim_key;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_claim_opts) == 48, "Incorrect size");

/**
 * Retrieve the name of the bdev module claim type.
 * See function definition for mapping claims types to name.
 *
 * \param claim_type The claim type.
 * \return A string that describes the claim type.
 */
const char *spdk_bdev_claim_get_name(enum spdk_bdev_claim_type claim_type);

/**
 * Initialize bdev module claim options structure.
 *
 * \param opts The structure to initialize.
 * \param size The size of *opts.
 */
void spdk_bdev_claim_opts_init(struct spdk_bdev_claim_opts *opts, size_t size);

/**
 * Claim the bdev referenced by the open descriptor. The claim is released as the descriptor is
 * closed.
 *
 * \param desc An open bdev descriptor. Some claim types may upgrade this from read-only to
 * read-write.
 * \param type The type of claim to establish.
 * \param opts NULL or options required by the particular claim type.
 * \param module The bdev module making this claim.
 * \return 0 on success
 * \return -ENOMEM if insufficient memory to track the claim
 * \return -EBUSY if the claim cannot be granted due to a conflict
 * \return -EINVAL if the claim type required options that were not passed or required parameters
 * were NULL.
 */
int spdk_bdev_module_claim_bdev_desc(struct spdk_bdev_desc *desc,
				     enum spdk_bdev_claim_type type,
				     struct spdk_bdev_claim_opts *opts,
				     struct spdk_bdev_module *module);

/**
 * Called by a bdev module to lay exclusive claim to a bdev.
 *
 * Also upgrades that bdev's descriptor to have write access if desc
 * is not NULL.
 *
 * \param bdev Block device to be claimed.
 * \param desc Descriptor for the above block device or NULL.
 * \param module Bdev module attempting to claim bdev.
 *
 * \return 0 on success
 * \return -EPERM if the bdev is already claimed by another module.
 */
int spdk_bdev_module_claim_bdev(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				struct spdk_bdev_module *module);

/**
 * Called to release a write claim on a block device.
 *
 * \param bdev Block device to be released.
 */
void spdk_bdev_module_release_bdev(struct spdk_bdev *bdev);

/* Libraries may define __SPDK_BDEV_MODULE_ONLY so that they include
 * only the struct spdk_bdev_module definition, and the relevant APIs
 * to claim/release a bdev. This may be useful in some cases to avoid
 * abidiff errors related to including the struct spdk_bdev structure
 * unnecessarily.
 */
#ifndef __SPDK_BDEV_MODULE_ONLY

typedef void (*spdk_bdev_unregister_cb)(void *cb_arg, int rc);

/**
 * Function table for a block device backend.
 *
 * The backend block device function table provides a set of APIs to allow
 * communication with a backend. The main commands are read/write API
 * calls for I/O via submit_request.
 */
/*
 * [한국어] ★ struct spdk_bdev_fn_table - bdev 모듈 vtable ★
 *
 * 각 bdev 인스턴스가 모듈 측에 연결하는 "콜백 테이블". bdev 레이어가
 * I/O를 제출하거나 정보를 조회할 때 여기에 등록된 함수 포인터를 호출한다.
 *
 * 가장 중요한 필드: **submit_request** — 이 함수가 bdev → 모듈 디스패치의
 * 진입점이며 호출 빈도가 가장 높다. 예) bdev_nvme 모듈의 경우 bdev_io를
 * 분해해 spdk_nvme_ns_cmd_read/write로 NVMe 드라이버에 forward.
 */
struct spdk_bdev_fn_table {
	/** Destroy the backend block device object. If the destruct process
	 *  for the bdev is asynchronous, return 1 from this function, and
	 *  then call spdk_bdev_destruct_done() once the async work is
	 *  complete. If the destruct process is synchronous, return 0 if
	 *  successful, or <0 if unsuccessful.
	 */
	int (*destruct)(void *ctx);
                                  /* [한국어] bdev 인스턴스 해제 콜백
                                   *  - 동기: 0 성공 / 음수 실패 반환
                                   *  - 비동기: 1 반환 후 작업 완료 시 spdk_bdev_destruct_done() 호출
                                   *  - @ctx: bdev 생성 시 등록한 module context 포인터 */

	/** Process the IO. */
	void (*submit_request)(struct spdk_io_channel *ch, struct spdk_bdev_io *);
                                  /* [한국어] ★★★ I/O 제출 진입점 ★★★
                                   *  - @ch: I/O가 제출되는 채널 (bdev_io가 속한 스레드와 동일)
                                   *  - @bdev_io: 요청 객체 — opcode/LBA/payload 등 포함
                                   *  - 모듈이 bdev_io를 처리(백엔드 큐로 포워드)하고, 완료 시 spdk_bdev_io_complete() 호출
                                   *  - NOMEM 상황: spdk_bdev_io_complete(..., SPDK_BDEV_IO_STATUS_NOMEM)로 즉시 반환
                                   *    → bdev 코어가 다음 완료 이벤트에서 재시도 */

	/** Check if the block device supports a specific I/O type. */
	bool (*io_type_supported)(void *ctx, enum spdk_bdev_io_type);
                                  /* [한국어] 특정 I/O 타입(UNMAP/FLUSH/COPY/ZONE_* 등) 지원 여부 질의
                                   *  - bdev 코어가 read/write 이외 명령 제출 전 체크 */

	/** Get an I/O channel for the specific bdev for the calling thread. */
	struct spdk_io_channel *(*get_io_channel)(void *ctx);
                                  /* [한국어] 호출 스레드용 I/O 채널 반환
                                   *  - 모듈이 내부적으로 spdk_io_device_register한 디바이스에 대해 spdk_get_io_channel 호출
                                   *  - NVMe 모듈이면 반환된 채널의 trailing ctx에 nvme_qpair 연결 */

	/**
	 * Output driver-specific information to a JSON stream. Optional - may be NULL.
	 *
	 * The JSON write context will be initialized with an open object, so the bdev
	 * driver should write a name (based on the driver name) followed by a JSON value
	 * (most likely another nested object).
	 */
	int (*dump_info_json)(void *ctx, struct spdk_json_write_ctx *w);
                                  /* [한국어] bdev 드라이버별 정보 JSON 출력 — bdev_get_bdevs RPC 응답에 삽입
                                   *  - 선택적 (NULL 허용) */

	/**
	 * Output bdev-specific RPC configuration to a JSON stream. Optional - may be NULL.
	 *
	 * This function should only be implemented for bdevs which can be configured
	 * independently of other bdevs.  For example, RPCs to create a bdev for an NVMe
	 * namespace may not be generated by this function, since enumerating an NVMe
	 * namespace requires attaching to an NVMe controller, and that controller may
	 * contain multiple namespaces.  The spdk_bdev_module's config_json function should
	 * be used instead for these cases.
	 *
	 * The JSON write context will be initialized with an open object, so the bdev
	 * driver should write all data necessary to recreate this bdev by invoking
	 * constructor method. No other data should be written.
	 */
	void (*write_config_json)(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);
                                  /* [한국어] 이 bdev를 재생성하는 RPC 구성을 JSON으로 출력 — 구성 save 용
                                   *  - 독립 구성 가능한 bdev(malloc, aio 등)에만 의미. NVMe ns처럼 상위 컨트롤러 의존적 bdev는 module config_json으로 처리 */

	/** Get spin-time per I/O channel in microseconds.
	 *  Optional - may be NULL.
	 */
	uint64_t (*get_spin_time)(struct spdk_io_channel *ch);
                                  /* [한국어] 채널별 busy spin 시간(µs) — 스케줄러가 CPU 사용량 추정에 활용 */

	/** Get bdev module context. */
	void *(*get_module_ctx)(void *ctx);
                                  /* [한국어] bdev 모듈별 내부 컨텍스트 반환 */

	/** Get memory domains used by bdev. Optional - may be NULL.
	 * Vbdev module implementation should call \ref spdk_bdev_get_memory_domains for underlying bdev.
	 * Vbdev module must inspect types of memory domains returned by base bdev and report only those
	 * memory domains that it can work with. */
	int (*get_memory_domains)(void *ctx, struct spdk_memory_domain **domains, int array_size);
                                  /* [한국어] 이 bdev가 사용하는 메모리 도메인 enumerate — RDMA 등 특수 도메인 지원 확인 */

	/**
	 * Reset I/O statistics specific for this bdev context.
	 */
	void (*reset_device_stat)(void *ctx);
                                  /* [한국어] bdev 모듈 내부 통계 초기화 (bdev 코어 통계와 별개) */

	/**
	 * Dump I/O statistics specific for this bdev context.
	 */
	void (*dump_device_stat_json)(void *ctx, struct spdk_json_write_ctx *w);
                                  /* [한국어] 모듈별 통계를 JSON으로 덤프 */

	/** Check if bdev can handle spdk_accel_sequence to handle I/O of specific type. */
	bool (*accel_sequence_supported)(void *ctx, enum spdk_bdev_io_type type);
                                  /* [한국어] 이 bdev가 accel 시퀀스(DMA 엔진/가속기 체인)로 해당 I/O 타입을 처리할 수 있는지 — bdev 코어가 offload 경로 선택에 사용 */
};

/** bdev I/O completion status */
/*
 * [한국어] bdev I/O 완료 상태 코드
 *
 * 음수 = 오류 종류, 0 = 대기, 1 = 성공. 모듈은 spdk_bdev_io_complete()
 * 호출 시 이 enum 값 중 하나를 전달한다.
 *
 * NOMEM 관례: 모듈이 내부 자원 부족으로 지금 제출 불가면 NOMEM을 주어
 * bdev 코어가 완료 이벤트에 편승해 재시도하도록 한다(자동 retry). 다른
 * 에러 상태는 즉시 상위 콜백으로 전달됨.
 */
enum spdk_bdev_io_status {
	SPDK_BDEV_IO_STATUS_AIO_ERROR = -8,
                                  /* [한국어] bdev_aio 모듈 경로에서 libaio 에러 — 별도 aio_result 필드 해석 필요 */
	SPDK_BDEV_IO_STATUS_ABORTED = -7,
                                  /* [한국어] abort 요청에 의해 중단된 I/O */
	SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED = -6,
                                  /* [한국어] fused 명령(COMPARE+WRITE 등)의 첫 조각 실패 — 두 번째 조각은 자동 폐기 */
	SPDK_BDEV_IO_STATUS_MISCOMPARE = -5,
                                  /* [한국어] COMPARE 명령의 데이터 불일치 */
	/*
	 * NOMEM should be returned when a bdev module cannot start an I/O because of
	 *  some lack of resources.  It may not be returned for RESET I/O.  I/O completed
	 *  with NOMEM status will be retried after some I/O from the same channel have
	 *  completed.
	 */
	SPDK_BDEV_IO_STATUS_NOMEM = -4,
                                  /* [한국어] 모듈 자원 부족 — bdev 코어가 채널 내 다른 I/O 완료 후 자동 재시도
                                   *  - RESET I/O에는 사용 불가 (retry 안전하지 않음) */
	SPDK_BDEV_IO_STATUS_SCSI_ERROR = -3,
                                  /* [한국어] SCSI 에러 — 센스 키 등은 bdev_io 내 별도 필드 참조 */
	SPDK_BDEV_IO_STATUS_NVME_ERROR = -2,
                                  /* [한국어] NVMe 에러 — SCT/SC 필드는 bdev_io 내부 nvme 섹션 참조 */
	SPDK_BDEV_IO_STATUS_FAILED = -1,
                                  /* [한국어] 일반 실패 (세부 원인 미분류) */
	SPDK_BDEV_IO_STATUS_PENDING = 0,
                                  /* [한국어] 아직 완료되지 않음 (초기 상태) */
	SPDK_BDEV_IO_STATUS_SUCCESS = 1,
                                  /* [한국어] 정상 완료 */

	/* This may be used as the size of an error status array by negation.
	 * Hence, this should be updated when adding new error statuses.
	 */
	SPDK_MIN_BDEV_IO_STATUS = SPDK_BDEV_IO_STATUS_AIO_ERROR,
                                  /* [한국어] 최소(가장 음수) 에러 코드 — 에러 통계 배열 크기 계산에 사용 */
};

/* We have to use the typedef in the function declaration to appease astyle. */
typedef enum spdk_bdev_io_status spdk_bdev_io_status_t;

struct spdk_bdev_name {
	char *name;
	struct spdk_bdev *bdev;
	RB_ENTRY(spdk_bdev_name) node;
};

struct spdk_bdev_alias {
	struct spdk_bdev_name alias;
	TAILQ_ENTRY(spdk_bdev_alias) tailq;
};

struct spdk_bdev_module_claim {
	struct spdk_bdev_module *module;
	struct spdk_bdev_desc *desc;
	char name[SPDK_BDEV_CLAIM_NAME_LEN];
	TAILQ_ENTRY(spdk_bdev_module_claim) link;
};

typedef TAILQ_HEAD(, spdk_bdev_io) bdev_io_tailq_t;
typedef STAILQ_HEAD(, spdk_bdev_io) bdev_io_stailq_t;
typedef TAILQ_HEAD(, lba_range) lba_range_tailq_t;

struct spdk_bdev {
	/** User context passed in by the backend */
	void *ctxt;

	/** Unique name for this block device. */
	char *name;

	/** Unique aliases for this block device. */
	TAILQ_HEAD(spdk_bdev_aliases_list, spdk_bdev_alias) aliases;

	/** Unique product name for this kind of block device. */
	char *product_name;

	/** write cache enabled, not used at the moment */
	int write_cache;

	/** Size in bytes of a logical block for the backend */
	uint32_t blocklen;

	/** Size in bytes of a physical block for the backend */
	uint32_t phys_blocklen;

	/** Bitmap of supported io types */
	uint32_t io_type_supported;

	/** Number of blocks */
	uint64_t blockcnt;

	struct {
		/**
		 * Specifies whether the write_unit_size is mandatory or
		 * only advisory. If set to true, the bdev layer will split
		 * WRITE I/O that span the write_unit_size before
		 * submitting them to the bdev module.
		 *
		 * This field takes precedence over split_on_optimal_io_boundary
		 * for WRITE I/O if both are set to true.
		 *
		 * Note that this field cannot be used to force splitting of
		 * UNMAP, WRITE_ZEROES or FLUSH I/O.
		 */
		uint32_t split_on_write_unit : 1;

		/**
		 * Specifies whether the optimal_io_boundary is mandatory or
		 * only advisory.  If set to true, the bdev layer will split
		 * READ and WRITE I/O that span the optimal_io_boundary before
		 * submitting them to the bdev module.
		 *
		 * Note that this field cannot be used to force splitting of
		 * UNMAP, WRITE_ZEROES or FLUSH I/O.
		 */
		uint32_t split_on_optimal_io_boundary : 1;

		/**
		 * Specify metadata location and set to true if metadata is interleaved
		 * with block data or false if metadata is separated with block data.
		 *
		 * Note that this field is valid only if there is metadata.
		 */
		uint32_t md_interleave : 1;

		/*
		 * DIF location.
		 *
		 * Set to true if DIF is set in the first 8/16 bytes of metadata or false
		 * if DIF is set in the last 8/16 bytes of metadata.
		 *
		 * Note that this field is valid only if DIF is enabled.
		 */
		uint32_t dif_is_head_of_md : 1;

		/**
		 * Specify whether bdev is zoned device.
		 */
		uint32_t zoned : 1;

		/**
		 * Specifies whether bdev supports media management events.
		 */
		uint32_t media_events : 1;

		uint32_t memory_domains_supported : 1;

		uint32_t reserved : 25;
	};

	/** Number of blocks required for write */
	uint32_t write_unit_size;

	/** Atomic compare & write unit */
	uint16_t acwu;

	/**
	 * Specifies an alignment requirement for data buffers associated with an spdk_bdev_io.
	 * 0 = no alignment requirement
	 * >0 = alignment requirement is 2 ^ required_alignment.
	 * bdev layer will automatically double buffer any spdk_bdev_io that violates this
	 * alignment, before the spdk_bdev_io is submitted to the bdev module.
	 */
	uint8_t required_alignment;

	uint8_t reserved1;

	/**
	 * Optimal I/O boundary in blocks, or 0 for no value reported.
	 */
	uint32_t optimal_io_boundary;

	/** Size in blocks of the preferred write alignment for the backend */
	uint32_t preferred_write_alignment;

	/** Size in blocks of the preferred write granularity for the backend */
	uint32_t preferred_write_granularity;

	/** Size in blocks of the optimal write size for the backend */
	uint32_t optimal_write_size;

	/** Size in blocks of the preferred unmap alignment for the backend */
	uint32_t preferred_unmap_alignment;

	/** Size in blocks of the preferred unmap granularity for the backend */
	uint32_t preferred_unmap_granularity;

	/**
	 * Max io size in bytes of a single segment
	 *
	 * Note: both max_segment_size and max_num_segments
	 * should be zero or non-zero.
	 */
	uint32_t max_segment_size;

	/* Maximum number of segments in a I/O */
	uint32_t max_num_segments;

	/* Maximum unmap in unit of logical block */
	uint32_t max_unmap;

	/* Maximum unmap block segments */
	uint32_t max_unmap_segments;

	/* Maximum write zeroes in unit of logical block */
	uint32_t max_write_zeroes;

	/**
	 * Maximum copy size in unit of logical block
	 * Should be set explicitly when backing device support copy command
	 */
	uint32_t max_copy;

	/**
	 * Maximum number of blocks in a single read/write I/O.  Requests exceeding this value will
	 * be split by the bdev layer.
	 */
	uint32_t max_rw_size;

	/**
	 * UUID for this bdev.
	 *
	 * If not provided, it will be generated by bdev layer.
	 */
	struct spdk_uuid uuid;

	/** Size in bytes of a metadata for the backend */
	uint32_t md_len;

	uint8_t reserved2[4];

	/**
	 * DIF type for this bdev.
	 *
	 * Note that this field is valid only if there is metadata.
	 */
	enum spdk_dif_type dif_type;

	/**
	 * DIF protection information format for this bdev.
	 *
	 * Note that this field is valid only if there is metadata and dif_type is
	 * not SPDK_DIF_DISABLE.
	 */
	enum spdk_dif_pi_format dif_pi_format;

	uint8_t reserved3[4];

	/**
	 * Specify whether each DIF check type is enabled.
	 */
	uint32_t dif_check_flags;

	uint8_t reserved4[8];

	/**
	 * Default size of each zone (in blocks).
	 */
	uint64_t zone_size;

	/**
	 * Maximum zone append data transfer size (in blocks).
	 */
	uint32_t max_zone_append_size;

	/**
	 * Maximum number of open zones.
	 */
	uint32_t max_open_zones;

	/**
	 * Maximum number of active zones.
	 */
	uint32_t max_active_zones;

	/**
	 * Optimal number of open zones.
	 */
	uint32_t optimal_open_zones;

	uint8_t reserved6[4];

	/**
	 * Specifies the bdev nvme controller attributes.
	 */
	union spdk_bdev_nvme_ctratt ctratt;

	/**
	 * NVMe namespace ID.
	 */
	uint32_t nsid;

	/* Upon receiving a reset request, this is the amount of time in seconds
	 * to wait for all I/O to complete before moving forward with the reset.
	 * If all I/O completes prior to this time out, the reset will be skipped.
	 * A value of 0 is special and will always send resets immediately, even
	 * if there is no I/O outstanding.
	 *
	 * Use case example:
	 * A shared bdev (e.g. multiple lvol bdevs sharing an underlying nvme bdev)
	 * needs to be reset. For a non-zero value bdev reset code will wait
	 * `reset_io_drain_timeout` seconds for outstanding IO that are present
	 * on any bdev channel, before sending a reset down to the underlying device.
	 * That way we can avoid sending "empty" resets and interrupting work of
	 * other lvols that use the same bdev. SPDK_BDEV_RESET_IO_DRAIN_RECOMMENDED_VALUE
	 * is a good choice for the value of this parameter.
	 *
	 * If this parameter remains equal to zero, the bdev reset will be forcefully
	 * sent down to the device, without any delays and waiting for outstanding IO. */
	uint16_t reset_io_drain_timeout;

	uint8_t reserved7[2];

	struct {
		/** Is numa.id valid? Needed to know whether numa.id == 0 was
		 *  explicitly set by bdev module or implicitly set when
		 *  calloc()'ing the structure.
		 */
		uint32_t id_valid : 1;
		/** NUMA node ID for the bdev */
		int32_t id : 31;
	} numa;

	/** Bitmap of supported io types */
	uint32_t accel_sequence_supported;

	/**
	 * Pointer to the bdev module that registered this bdev.
	 */
	struct spdk_bdev_module *module;

	/** function table for all LUN ops */
	const struct spdk_bdev_fn_table *fn_table;

	/** Fields that are used internally by the bdev subsystem.  Bdev modules
	 *  must not read or write to these fields.
	 */
	struct __bdev_internal_fields {
		/** Quality of service parameters */
		struct spdk_bdev_qos *qos;

		/** True if the state of the QoS is being modified */
		bool qos_mod_in_progress;

		/** Trace ID for this bdev. */
		uint16_t trace_id;

		/**
		 * SPDK spinlock protecting many of the internal fields of this structure. If
		 * multiple locks need to be held, the following order must be used:
		 *   g_bdev_mgr.spinlock
		 *   bdev->internal.spinlock
		 *   bdev_desc->spinlock
		 *   bdev_module->internal.spinlock
		 */
		struct spdk_spinlock spinlock;

		/** The bdev status */
		enum spdk_bdev_status status;

		/**
		 * How many bdev_examine() calls are iterating claim.v2.claims. When non-zero claims
		 * that are released will be cleared but remain on the claims list until
		 * bdev_examine() finishes. Must hold spinlock on all updates.
		 */
		uint32_t examine_in_progress;

		/**
		 * The claim type: used in conjunction with claim. Must hold spinlock on all
		 * updates.
		 */
		enum spdk_bdev_claim_type claim_type;

		/** Which module has claimed this bdev. Must hold spinlock on all updates. */
		union __bdev_internal_claim {
			/** Claims acquired with spdk_bdev_module_claim_bdev() */
			struct __bdev_internal_claim_v1 {
				/**
				 * Pointer to the module that has claimed this bdev for purposes of
				 * creating virtual bdevs on top of it. Set to NULL and set
				 * claim_type to SPDK_BDEV_CLAIM_NONE if the bdev has not been
				 * claimed.
				 */
				struct spdk_bdev_module		*module;
			} v1;
			/** Claims acquired with spdk_bdev_module_claim_bdev_desc() */
			struct __bdev_internal_claim_v2 {
				/** The claims on this bdev */
				TAILQ_HEAD(v2_claims, spdk_bdev_module_claim) claims;
				/** See spdk_bdev_claim_opts.shared_claim_key */
				uint64_t key;
			} v2;
		} claim;

		/** Callback function that will be called after bdev destruct is completed. */
		spdk_bdev_unregister_cb	unregister_cb;

		/** Unregister call context */
		void *unregister_ctx;

		/** Thread that issued the unregister.  The cb must be called on this thread. */
		struct spdk_thread *unregister_td;

		/** List of open descriptors for this block device. */
		TAILQ_HEAD(, spdk_bdev_desc) open_descs;

		TAILQ_ENTRY(spdk_bdev) link;

		/** points to a reset bdev_io if one is in progress. */
		struct spdk_bdev_io *reset_in_progress;

		/** List of reset bdev_ios that are not submitted to the underlying device. */
		bdev_io_tailq_t		queued_resets;

		/** poller for tracking the queue_depth of a device, NULL if not tracking */
		struct spdk_poller *qd_poller;

		/** open descriptor to use qd_poller safely */
		struct spdk_bdev_desc *qd_desc;

		/** period at which we poll for queue depth information */
		uint64_t period;

		/** new period to be used to poll for queue depth information */
		uint64_t new_period;

		/** used to aggregate queue depth while iterating across the bdev's open channels */
		uint64_t temporary_queue_depth;

		/** queue depth as calculated the last time the telemetry poller checked. */
		uint64_t measured_queue_depth;

		/** most recent value of ticks spent performing I/O. Used to calculate the weighted time doing I/O */
		uint64_t io_time;

		/** weighted time performing I/O. Equal to measured_queue_depth * period */
		uint64_t weighted_io_time;

		/** accumulated I/O statistics for previously deleted channels of this bdev */
		struct spdk_bdev_io_stat *stat;

		/** true if tracking the queue_depth of a device is in progress */
		bool	qd_poll_in_progress;

		/** histogram enabled on this bdev */
		bool	 histogram_enabled;
		bool	 histogram_in_progress;
		uint8_t	 histogram_io_type;
		uint8_t	 histogram_granularity;
		uint64_t histogram_min_val;
		uint64_t histogram_max_val;

		/** Currently locked ranges for this bdev.  Used to populate new channels. */
		lba_range_tailq_t locked_ranges;

		/** Pending locked ranges for this bdev.  These ranges are not currently
		 *  locked due to overlapping with another locked range.
		 */
		lba_range_tailq_t pending_locked_ranges;

		/** Bdev name used for quick lookup */
		struct spdk_bdev_name bdev_name;
	} internal;
};

/**
 * Callback when buffer is allocated for the bdev I/O.
 *
 * \param ch The I/O channel the bdev I/O was handled on.
 * \param bdev_io The bdev I/O
 * \param success True if buffer is allocated successfully or the bdev I/O has an SGL
 * assigned already, or false if it failed. The possible reason of failure is the size
 * of the buffer to allocate is greater than the permitted maximum.
 */
typedef void (*spdk_bdev_io_get_buf_cb)(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
					bool success);

/**
 * Callback when an auxiliary buffer is allocated for the bdev I/O.
 *
 * \param ch The I/O channel the bdev I/O was handled on.
 * \param bdev_io The bdev I/O
 * \param aux_buf Pointer to the allocated buffer.  NULL if there was a failure such as
 * the size of the buffer to allocate is greater than the permitted maximum.
 */
typedef void (*spdk_bdev_io_get_aux_buf_cb)(struct spdk_io_channel *ch,
		struct spdk_bdev_io *bdev_io, void *aux_buf);

/* Maximum number of IOVs used for I/O splitting */
#define SPDK_BDEV_IO_NUM_CHILD_IOV 32

struct spdk_bdev_io_block_params {
	/** For SG buffer cases, array of iovecs to transfer. */
	struct iovec *iovs;

	/** For SG buffer cases, number of iovecs in iovec array. */
	int iovcnt;

	/** Total size of data to be transferred. */
	uint64_t num_blocks;

	/** Starting offset (in blocks) of the bdev for this I/O. */
	uint64_t offset_blocks;

	/** Memory domain and its context to be used by bdev modules */
	struct spdk_memory_domain *memory_domain;
	void *memory_domain_ctx;

	/* Sequence of accel operations */
	struct spdk_accel_sequence *accel_sequence;

	/* Metadata buffer */
	void *md_buf;

	/** For fused operations such as COMPARE_AND_WRITE, array of iovecs
	 *  for the second operation.
	 */
	struct iovec *fused_iovs;

	/** Number of iovecs in fused_iovs. */
	int fused_iovcnt;

	/** Specify whether each DIF check type is enabled. */
	uint32_t dif_check_flags;

	/** defined by \ref spdk_bdev_nvme_cdw12 */
	union spdk_bdev_nvme_cdw12 nvme_cdw12;

	/** defined by \ref spdk_bdev_nvme_cdw13 */
	union spdk_bdev_nvme_cdw13 nvme_cdw13;

	struct {
		/** Whether the buffer should be populated with the real data */
		uint8_t populate : 1;

		/** Whether the buffer should be committed back to disk */
		uint8_t commit : 1;

		/** True if this request is in the 'start' phase of zcopy. False if in 'end'. */
		uint8_t start : 1;
	} zcopy;

	struct {
		/** The callback argument for the outstanding request which this abort
		 *  attempts to cancel.
		 */
		void *bio_cb_arg;
	} abort;

	struct {
		/** The offset of next data/hole.  */
		uint64_t offset;
	} seek;

	struct {
		/** Starting source offset (in blocks) of the bdev for copy I/O. */
		uint64_t src_offset_blocks;
	} copy;

	/** DIF context */
	struct spdk_dif_ctx dif_ctx;

	/** DIF error information */
	struct spdk_dif_error dif_err;
};

struct spdk_bdev_io_reset_params {
	/** Channel reference held while messages for this reset are in progress. */
	struct spdk_io_channel *ch_ref;
	struct {
		/* Handle to timed poller that checks each channel for outstanding IO. */
		struct spdk_poller *poller;
		/* Store calculated time value, when a poller should stop its work. */
		uint64_t  stop_time_tsc;
	} wait_poller;
};

struct spdk_bdev_io_abort_params {
	/** The outstanding request matching bio_cb_arg which this abort attempts to cancel. */
	struct spdk_bdev_io *bio_to_abort;
};

struct spdk_bdev_io_nvme_passthru_params {
	/* The NVMe command to execute */
	struct spdk_nvme_cmd cmd;

	/* For SG buffer cases, array of iovecs to transfer. */
	struct iovec *iovs;

	/* For SG buffer cases, number of iovecs in iovec array. */
	int iovcnt;

	/* The data buffer to transfer */
	void *buf;

	/* The number of bytes to transfer */
	size_t nbytes;

	/* The meta data buffer to transfer */
	void *md_buf;

	/* meta data buffer size to transfer */
	size_t md_len;
};

struct spdk_bdev_io_zone_mgmt_params {
	/* First logical block of a zone */
	uint64_t zone_id;

	/* Number of zones */
	uint32_t num_zones;

	/* Used to change zoned device zone state */
	enum spdk_bdev_zone_action zone_action;

	/* The data buffer */
	void *buf;
};

/**
 *  Fields that are used internally by the bdev subsystem.  Bdev modules
 *  must not read or write to these fields.
 */
struct spdk_bdev_io_internal_fields {
	/** The bdev I/O channel that this was handled on. */
	struct spdk_bdev_channel *ch;

	union {
		struct {

			/** Whether the accel_sequence member is valid */
			uint8_t has_accel_sequence		: 1;

			/** Whether memory_domain member is valid */
			uint8_t has_memory_domain		: 1;

			/** Whether the split data structure is valid */
			uint8_t split				: 1;

			/** Whether ptr in the buf data structure is valid */
			uint8_t has_buf				: 1;

			/** Whether the bounce_buf data structure is valid */
			uint8_t has_bounce_buf			: 1;

			/** Whether we are currently inside the submit request call */
			uint8_t in_submit_request		: 1;

			/** Whether the I/O is a sub-I/O of a split parent I/O */
			uint8_t child_io		: 1;

			uint8_t reserved			: 1;
		};
		uint8_t raw;
	} f;

	/** Status for the IO */
	int8_t status;

	/** Retry state (resubmit, re-pull, re-push, etc.) */
	uint8_t retry_state;

	uint8_t	reserved[5];

	/** The bdev descriptor that was used when submitting this I/O. */
	struct spdk_bdev_desc *desc;

	/** User function that will be called when this completes */
	spdk_bdev_io_completion_cb cb;

	/** Context that will be passed to the completion callback */
	void *caller_ctx;

	/** Current tsc at submit time. Used to calculate latency at completion. */
	uint64_t submit_tsc;

	/** Entry to the list io_submitted of struct spdk_bdev_channel */
	TAILQ_ENTRY(spdk_bdev_io) ch_link;

	/** bdev_io pool entry */
	STAILQ_ENTRY(spdk_bdev_io) buf_link;

	/** Error information from a device */
	union {
		struct {
			/** NVMe completion queue entry DW0 */
			uint32_t cdw0;
			/** NVMe status code type */
			uint8_t sct;
			/** NVMe status code */
			uint8_t sc;
		} nvme;
		/** Only valid when status is SPDK_BDEV_IO_STATUS_SCSI_ERROR */
		struct {
			/** SCSI status code */
			uint8_t sc;
			/** SCSI sense key */
			uint8_t sk;
			/** SCSI additional sense code */
			uint8_t asc;
			/** SCSI additional sense code qualifier */
			uint8_t ascq;
		} scsi;
		/** Only valid when status is SPDK_BDEV_IO_STATUS_AIO_ERROR */
		int aio_result;
	} error;

	struct {
		/** stored user callback in case we split the I/O and use a temporary callback */
		spdk_bdev_io_completion_cb stored_user_cb;

		/** number of blocks remaining in a split i/o */
		uint64_t remaining_num_blocks;

		/** current offset of the split I/O in the bdev */
		uint64_t current_offset_blocks;

		/** count of outstanding batched split I/Os */
		uint32_t outstanding;
	} split;

	struct {
		/** bdev allocated memory associated with this request */
		void *ptr;

		/** requested size of the buffer associated with this I/O */
		uint64_t len;
	} buf;

	/** if the request is double buffered, store original request iovs here */
	struct {
		struct iovec  iov;
		struct iovec  md_iov;
		struct iovec  orig_md_iov;
		struct iovec *orig_iovs;
		int           orig_iovcnt;
	} bounce_buf;

	/** Callback for when buf is allocated */
	spdk_bdev_io_get_buf_cb get_buf_cb;

	/**
	 * Queue entry used in several cases:
	 *  1. IOs awaiting retry due to NOMEM status,
	 *  2. IOs awaiting submission due to QoS,
	 *  3. IOs with an accel sequence being executed,
	 *  4. IOs awaiting memory domain pull/push,
	 *  5. queued reset requests.
	 */
	TAILQ_ENTRY(spdk_bdev_io) link;

	/** iobuf queue entry */
	struct spdk_iobuf_entry iobuf;

	/** Enables queuing parent I/O when no bdev_ios available for split children. */
	struct spdk_bdev_io_wait_entry waitq_entry;

	/** Memory domain and its context passed by the user in ext API */
	struct spdk_memory_domain *memory_domain;
	void *memory_domain_ctx;

	/* Sequence of accel operations passed by the user */
	struct spdk_accel_sequence *accel_sequence;

	/** Data transfer completion callback */
	void (*data_transfer_cpl)(void *ctx, int rc);
};

/*
 * [한국어] ★★★ struct spdk_bdev_io - bdev 레이어 I/O 요청 객체 ★★★
 *
 * bdev 서브시스템에서 "하나의 I/O 요청"을 표현하는 최상위 객체.
 * 애플리케이션이 `spdk_bdev_read/write/unmap/flush/...`를 호출하면 내부에서
 * 채널별 풀(bdev_channel_get_io)에서 이 구조체 하나를 획득하여 초기화한 뒤,
 * `submit_request(ch, bdev_io)` 콜백으로 모듈에 전달한다.
 *
 * 구성 원칙:
 *   - 공개 필드(bdev, type, u) : 모듈이 읽어서 I/O를 수행
 *   - internal 필드            : bdev 코어 전용, 모듈은 **접근 금지**
 *   - driver_ctx[]             : 모듈 전용 per-I/O 컨텍스트. flexible array
 *   - reserved 필드들          : ABI 안정성 — 향후 필드 추가 시 사용. 크기 불변
 *   - driver_ctx 오프셋은 캐시 라인 정렬 (SPDK_STATIC_ASSERT로 강제)
 *
 * 수명 주기:
 *   1) bdev 코어가 채널 mempool에서 획득(bdev_channel_get_io)
 *   2) 사용자 요청 정보를 u.{bdev,reset,abort,...} 중 적합한 하나에 기록
 *   3) internal.ch_link로 채널의 io_submitted 리스트에 등록
 *   4) 모듈의 submit_request 콜백 호출 → 백엔드로 dispatch
 *   5) 완료 시 spdk_bdev_io_complete(bdev_io, status) 호출
 *   6) 코어가 사용자 콜백 invoke → 구조체 mempool 반납
 *
 * 메모리 레이아웃:
 *   [ bdev / type / retries / iov / child_iov[] ]   ← 공개 읽기
 *   [ u (opcode별 파라미터 union) ]                 ← 공개 쓰기
 *   [ internal (status, split, bounce_buf, etc.) ] ← 모듈 접근 금지
 *   [ driver_ctx[] ]                                ← 모듈 전용
 */
struct spdk_bdev_io {
	/** The block device that this I/O belongs to. */
	struct spdk_bdev *bdev;
                                  /* [한국어] 이 I/O가 대상으로 하는 bdev 인스턴스
                                   *  - 설정자: spdk_bdev_read/write 등 공개 API
                                   *  - 읽는 자: 모듈 submit_request 콜백 — 블록 크기·UUID 조회 등 */

	/** Enumerated value representing the I/O type. */
	uint8_t type;
                                  /* [한국어] enum spdk_bdev_io_type (READ/WRITE/UNMAP/FLUSH/RESET/COMPARE/COMPARE_AND_WRITE/ZONE_APPEND/NVME_ADMIN/NVME_IO/NVME_IO_MD/WRITE_ZEROES/COPY 등)
                                   *  - 모듈이 이 값을 보고 u의 어느 필드를 해석할지 결정 */

	uint8_t reserved0;            /* [한국어] 예약 1바이트 (future flags) */

	/** Number of IO submission retries */
	uint16_t num_retries;
                                  /* [한국어] NOMEM retry 누적 횟수 — bdev 코어가 재시도할 때마다 증가 */

	uint32_t reserved1;           /* [한국어] 정렬 예약 */

	/** A single iovec element for use by this bdev_io. */
	struct iovec iov;
                                  /* [한국어] 단일 iovec 슬롯 — 페이로드가 연속 버퍼일 때 사용자가 직접 입력하지 않은 경우 코어가 bounce_buf 경로에서 구성 */

	/** Array of iovecs used for I/O splitting. */
	struct iovec child_iov[SPDK_BDEV_IO_NUM_CHILD_IOV];
                                  /* [한국어] split 경로에서 child I/O가 사용하는 iovec 배열
                                   *  - parent의 사용자 iovec을 MDTS 등 한계에 맞게 쪼개 이 배열에 재구성
                                   *  - SPDK_BDEV_IO_NUM_CHILD_IOV: 기본 32 (일반 NVMe MDTS 하에서 충분) */

	uint8_t reserved2[32];        /* [한국어] ABI 예약 — 향후 필드 추가 공간 (크기 불변 보장) */

	/** Parameters filled in by the user */
	/*
	 * [한국어] 사용자 입력 파라미터 union — type에 따라 어느 필드를 사용할지 결정
	 *
	 * type별 매핑:
	 *   READ/WRITE/UNMAP/FLUSH/COMPARE/COPY/ZONE_APPEND/WRITE_ZEROES → u.bdev
	 *   RESET                                                         → u.reset
	 *   ABORT                                                         → u.abort
	 *   NVME_ADMIN/NVME_IO/NVME_IO_MD                                 → u.nvme_passthru
	 *   ZONE_MANAGEMENT                                               → u.zone_mgmt
	 */
	union {
		struct spdk_bdev_io_block_params bdev;
                                  /* [한국어] 블록 I/O 파라미터 — iovec, offset, num_blocks, unmap range 등
                                   *  - 가장 자주 사용되는 필드 */
		struct spdk_bdev_io_reset_params reset;
                                  /* [한국어] Reset 파라미터 */
		struct spdk_bdev_io_abort_params abort;
                                  /* [한국어] Abort 대상 bdev_io 포인터 포함 */
		struct spdk_bdev_io_nvme_passthru_params nvme_passthru;
                                  /* [한국어] NVMe passthru — raw SQE를 그대로 전달하는 경로 */
		struct spdk_bdev_io_zone_mgmt_params zone_mgmt;
                                  /* [한국어] Zone management (OPEN/CLOSE/RESET/FINISH/OFFLINE 등) */
	} u;

	uint8_t reserved3[40];        /* [한국어] ABI 예약 */

	/**
	 *  Fields that are used internally by the bdev subsystem.  Bdev modules
	 *  must not read or write to these fields.
	 */
	struct spdk_bdev_io_internal_fields internal;
                                  /* [한국어] ★ bdev 코어 내부 전용 필드 ★
                                   *  - status, NVMe/SCSI/AIO 에러 정보, split 상태, bounce buffer, 대기 큐 링크 등
                                   *  - 모듈은 절대 접근 금지 — 읽기/쓰기 모두 disallowed
                                   *  - 완료 상태는 spdk_bdev_io_complete() / set_nvme_status() 등 API로만 변경 */
	uint8_t reserved4[64];        /* [한국어] ABI 예약 */

	/**
	 * Per I/O context for use by the bdev module.
	 */
	uint8_t driver_ctx[0];
                                  /* [한국어] ★ 모듈 전용 per-I/O 컨텍스트 (flexible array) ★
                                   *  - bdev 등록 시 모듈이 요청한 크기만큼 확장 — bdev_io 풀 할당 시 자동 확보
                                   *  - 예: bdev_nvme 모듈은 여기에 nvme_request 연관 포인터를 저장
                                   *  - 캐시 라인 정렬 (SPDK_STATIC_ASSERT로 강제) — 모듈이 hot data 배치에 유리 */

	/* No members may be added after driver_ctx! */
                                  /* [한국어] flexible array 이후 필드 추가 금지 — driver_ctx가 끝까지 확장 */
};
SPDK_STATIC_ASSERT(offsetof(struct spdk_bdev_io, driver_ctx) % SPDK_CACHE_LINE_SIZE == 0,
		   "driver_ctx not cache line aligned");
                                  /* [한국어] driver_ctx 시작이 64B 경계 — 모듈의 hot data가 false sharing 없이 배치되도록 */

/**
 * Register a new bdev.
 *
 * This function must be called from the SPDK app thread.
 *
 * \param bdev Block device to register.
 *
 * \return 0 on success.
 * \return -EINVAL if the bdev name is NULL.
 * \return -EEXIST if a bdev or bdev alias with the same name already exists.
 */
int spdk_bdev_register(struct spdk_bdev *bdev);

/**
 * Start unregistering a bdev. This will notify each currently open descriptor
 * on this bdev of the hotremoval to request the upper layers to stop using this bdev
 * and manually close all the descriptors with spdk_bdev_close().
 * The actual bdev unregistration may be deferred until all descriptors are closed.
 *
 * Calling this function from any thread is deprecated and will be disallowed in the 26.05 release.
 * This function should be called from the SPDK app thread.
 *
 * The cb_fn will be called from the context of the same spdk_thread that called
 * spdk_bdev_unregister.
 *
 * Note: spdk_bdev_unregister() can be unsafe unless the bdev is not opened before and
 * closed after unregistration. It is recommended to use spdk_bdev_unregister_by_name().
 *
 * \param bdev Block device to unregister.
 * \param cb_fn Callback function to be called when the unregister is complete.
 * \param cb_arg Argument to be supplied to cb_fn
 */
void spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

/**
 * Start unregistering a bdev. This will notify each currently open descriptor
 * on this bdev of the hotremoval to request the upper layer to stop using this bdev
 * and manually close all the descriptors with spdk_bdev_close().
 * The actual bdev unregistration may be deferred until all descriptors are closed.
 *
 * Calling this function from any thread is deprecated and will be disallowed in the 26.05 release.
 * This function should be called from the SPDK app thread.
 *
 * The cb_fn will be called from the context of the same spdk_thread that called
 * spdk_bdev_unregister.
 *
 * \param bdev_name Block device name to unregister.
 * \param module Module by which the block device was registered.
 * \param cb_fn Callback function to be called when the unregister is complete.
 * \param cb_arg Argument to be supplied to cb_fn
 *
 * \return 0 on success, or suitable errno value otherwise
 */
int spdk_bdev_unregister_by_name(const char *bdev_name, struct spdk_bdev_module *module,
				 spdk_bdev_unregister_cb cb_fn, void *cb_arg);

/**
 * Notify the bdev layer that an asynchronous destruct operation is complete.
 *
 * A Bdev with an asynchronous destruct path should return 1 from its
 * destruct function and call this function at the conclusion of that path.
 * Bdevs with synchronous destruct paths should return 0 from their destruct
 * path.
 *
 * \param bdev Block device that was destroyed.
 * \param bdeverrno Error code returned from bdev's destruct callback.
 */
void spdk_bdev_destruct_done(struct spdk_bdev *bdev, int bdeverrno);

/**
 * Indicate to the bdev layer that the module is done examining a bdev.
 *
 * To be called during examine_config function or asynchronously in response to the
 * module's examine_disk function being called.
 *
 * \param module Pointer to the module completing the examination.
 */
void spdk_bdev_module_examine_done(struct spdk_bdev_module *module);

/**
 * Indicate to the bdev layer that the module is done initializing.
 *
 * To be called once after an asynchronous operation required for module initialization is
 * completed. If module->async_init is false, the module must not call this function.
 *
 * \param module Pointer to the module completing the initialization.
 */
void spdk_bdev_module_init_done(struct spdk_bdev_module *module);

/**
 * Indicate that the module finish has completed.
 *
 * To be called in response to the module_fini, only if async_fini is set.
 *
 */
void spdk_bdev_module_fini_done(void);

/**
 * Indicate that the module fini start has completed.
 *
 * To be called in response to the fini_start, only if async_fini_start is set.
 * May be called during fini_start or asynchronously.
 *
 */
void spdk_bdev_module_fini_start_done(void);

/**
 * Add alias to block device names list.
 * Aliases can be add only to registered bdev.
 * All aliases are removed when bdev is unregistered.
 *
 * \param bdev Block device to query.
 * \param alias Alias to be added to list.
 *
 * \return 0 on success
 * \return -EEXIST if alias already exists as name or alias on any bdev
 * \return -ENOMEM if memory cannot be allocated to store alias
 * \return -EINVAL if passed alias is empty
 */
int spdk_bdev_alias_add(struct spdk_bdev *bdev, const char *alias);

/**
 * Removes name from block device names list.
 *
 * \param bdev Block device to query.
 * \param alias Alias to be deleted from list.
 * \return 0 on success
 * \return -ENOENT if alias does not exists
 */
int spdk_bdev_alias_del(struct spdk_bdev *bdev, const char *alias);

/**
 * Removes all alias from block device alias list.
 *
 * \param bdev Block device to operate.
 */
void spdk_bdev_alias_del_all(struct spdk_bdev *bdev);

/**
 * Get pointer to block device aliases list.
 *
 * \param bdev Block device to query.
 * \return Pointer to bdev aliases list.
 */
const struct spdk_bdev_aliases_list *spdk_bdev_get_aliases(const struct spdk_bdev *bdev);

/**
 * Allocate a buffer for given bdev_io.  Allocation will happen
 * only if the bdev_io has no assigned SGL yet or SGL is not
 * aligned to \c bdev->required_alignment.  If SGL is not aligned,
 * this call will cause copy from SGL to bounce buffer on write
 * path or copy from bounce buffer to SGL before completion
 * callback on read path.  The buffer will be freed automatically
 * on \c spdk_bdev_free_io() call. This call will never fail.
 * In case of lack of memory given callback \c cb will be deferred
 * until enough memory is freed.  This function *must* be called
 * from the thread issuing \c bdev_io.
 *
 * \param bdev_io I/O to allocate buffer for.
 * \param cb callback to be called when the buffer is allocated
 * or the bdev_io has an SGL assigned already.
 * \param len size of the buffer to allocate. In case the bdev_io
 * doesn't have an SGL assigned this field must be no bigger than
 * \c SPDK_BDEV_LARGE_BUF_MAX_SIZE.
 */
void spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len);

/**
 * Set the given buffer as the data buffer described by this bdev_io.
 *
 * The portion of the buffer used may be adjusted for memory alignment
 * purposes.
 *
 * \param bdev_io I/O to set the buffer on.
 * \param buf The buffer to set as the active data buffer.
 * \param len The length of the buffer.
 *
 */
void spdk_bdev_io_set_buf(struct spdk_bdev_io *bdev_io, void *buf, size_t len);

/**
 * Set the given buffer as metadata buffer described by this bdev_io.
 *
 * \param bdev_io I/O to set the buffer on.
 * \param md_buf The buffer to set as the active metadata buffer.
 * \param len The length of the metadata buffer.
 */
void spdk_bdev_io_set_md_buf(struct spdk_bdev_io *bdev_io, void *md_buf, size_t len);

/**
 * Complete a bdev_io
 *
 * \param bdev_io I/O to complete.
 * \param status The I/O completion status.
 */
void spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io,
			   enum spdk_bdev_io_status status);

/**
 * Set a bdev_io with an NVMe status code and DW0 completion queue entry
 *
 * \param bdev_io I/O to set status.
 * \param cdw0 NVMe Completion Queue DW0 value (set to 0 if not applicable)
 * \param sct NVMe Status Code Type.
 * \param sc NVMe Status Code.
 * \return IO status corresponding to the NVMe status
 */
enum spdk_bdev_io_status spdk_bdev_io_set_nvme_status(struct spdk_bdev_io *bdev_io, uint32_t cdw0,
		int sct, int sc);

/**
 * Set and complete a bdev_io with an NVMe status code and DW0 completion queue entry
 *
 * \param bdev_io I/O to complete.
 * \param cdw0 NVMe Completion Queue DW0 value (set to 0 if not applicable)
 * \param sct NVMe Status Code Type.
 * \param sc NVMe Status Code.
 */
void spdk_bdev_io_complete_nvme_status(struct spdk_bdev_io *bdev_io, uint32_t cdw0, int sct,
				       int sc);

/**
 * Set a bdev_io with a SCSI status code.
 *
 * \param bdev_io I/O to set status.
 * \param sc SCSI Status Code.
 * \param sk SCSI Sense Key.
 * \param asc SCSI Additional Sense Code.
 * \param ascq SCSI Additional Sense Code Qualifier.
 * \return IO status corresponding to the SCSI status
 */
enum spdk_bdev_io_status spdk_bdev_io_set_scsi_status(struct spdk_bdev_io *bdev_io,
		enum spdk_scsi_status sc, enum spdk_scsi_sense sk, uint8_t asc, uint8_t ascq);

/**
 * Set and complete a bdev_io with a SCSI status code.
 *
 * \param bdev_io I/O to complete.
 * \param sc SCSI Status Code.
 * \param sk SCSI Sense Key.
 * \param asc SCSI Additional Sense Code.
 * \param ascq SCSI Additional Sense Code Qualifier.
 */
void spdk_bdev_io_complete_scsi_status(struct spdk_bdev_io *bdev_io, enum spdk_scsi_status sc,
				       enum spdk_scsi_sense sk, uint8_t asc, uint8_t ascq);

/**
 * Set a bdev_io with AIO errno.
 *
 * \param bdev_io I/O to set status.
 * \param aio_result Negative errno returned from AIO.
 * \return IO status corresponding to the AIO result
 */
enum spdk_bdev_io_status spdk_bdev_io_set_aio_status(struct spdk_bdev_io *bdev_io, int aio_result);

/**
 * Set and complete a bdev_io with AIO errno.
 *
 * \param bdev_io I/O to complete.
 * \param aio_result Negative errno returned from AIO.
 */
void spdk_bdev_io_complete_aio_status(struct spdk_bdev_io *bdev_io, int aio_result);

/**
 * Copy a bdev_io status from another bdev_io.
 *
 * \param bdev_io I/O to set status.
 * \param base_io I/O from which to copy the status.
 * \return IO status corresponding to the base_io status
 */
enum spdk_bdev_io_status spdk_bdev_io_set_base_io_status(struct spdk_bdev_io *bdev_io,
		const struct spdk_bdev_io *base_io);

/**
 * Complete a bdev_io copying a status from another bdev_io.
 *
 * \param bdev_io I/O to complete.
 * \param base_io I/O from which to copy the status.
 */
void spdk_bdev_io_complete_base_io_status(struct spdk_bdev_io *bdev_io,
		const struct spdk_bdev_io *base_io);

/**
 * Get a thread that given bdev_io was submitted on.
 *
 * \param bdev_io I/O
 * \return thread that submitted the I/O
 */
struct spdk_thread *spdk_bdev_io_get_thread(struct spdk_bdev_io *bdev_io);

/**
 * Get the bdev module's I/O channel that the given bdev_io was submitted on.
 *
 * \param bdev_io I/O
 * \return the bdev module's I/O channel that the given bdev_io was submitted on.
 */
struct spdk_io_channel *spdk_bdev_io_get_io_channel(struct spdk_bdev_io *bdev_io);

/**
 * Get the submit_tsc of a bdev I/O.
 *
 * \param bdev_io The bdev I/O to get the submit_tsc.
 *
 * \return The submit_tsc of the specified bdev I/O.
 */
uint64_t spdk_bdev_io_get_submit_tsc(struct spdk_bdev_io *bdev_io);

/**
 * Query if metadata is hidden from the bdev I/O.
 *
 * \param bdev_io The bdev I/O to query.
 *
 * \return true if metadata is hidden from the bdev I/O, or false otherwise.
 */
bool spdk_bdev_io_hide_metadata(struct spdk_bdev_io *bdev_io);

/**
 * Resize for a bdev.
 *
 * Change number of blocks for provided block device.
 * It can only be called on a registered bdev.
 *
 * \param bdev Block device to change.
 * \param size New size of bdev.
 * \return 0 on success, negated errno on failure.
 */
int spdk_bdev_notify_blockcnt_change(struct spdk_bdev *bdev, uint64_t size);

/**
 * Translates NVMe status codes to SCSI status information.
 *
 * The codes are stored in the user supplied integers.
 *
 * \param bdev_io I/O containing status codes to translate.
 * \param sc SCSI Status Code will be stored here.
 * \param sk SCSI Sense Key will be stored here.
 * \param asc SCSI Additional Sense Code will be stored here.
 * \param ascq SCSI Additional Sense Code Qualifier will be stored here.
 */
void spdk_scsi_nvme_translate(const struct spdk_bdev_io *bdev_io,
			      int *sc, int *sk, int *asc, int *ascq);

/**
 * Add the given module to the list of registered modules.
 * This function should be invoked by referencing the macro
 * SPDK_BDEV_MODULE_REGISTER in the module c file.
 *
 * \param bdev_module Module to be added.
 */
void spdk_bdev_module_list_add(struct spdk_bdev_module *bdev_module);

/**
 * Find registered module with name pointed by \c name.
 *
 * \param name name of module to be searched for.
 * \return pointer to module or NULL if no module with \c name exist
 */
struct spdk_bdev_module *spdk_bdev_module_list_find(const char *name);

static inline struct spdk_bdev_io *
spdk_bdev_io_from_ctx(void *ctx)
{
	return SPDK_CONTAINEROF(ctx, struct spdk_bdev_io, driver_ctx);
}

struct spdk_bdev_part_base;

/**
 * Returns a pointer to the spdk_bdev associated with an spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return A pointer to the base's spdk_bdev struct.
 */
struct spdk_bdev *spdk_bdev_part_base_get_bdev(struct spdk_bdev_part_base *part_base);

/**
 * Returns a spdk_bdev name of the corresponding spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return A text string representing the name of the base bdev.
 */
const char *spdk_bdev_part_base_get_bdev_name(struct spdk_bdev_part_base *part_base);

/**
 * Returns a pointer to the spdk_bdev_descriptor associated with an spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return A pointer to the base's spdk_bdev_desc struct.
 */
struct spdk_bdev_desc *spdk_bdev_part_base_get_desc(struct spdk_bdev_part_base *part_base);

/**
 * Returns a pointer to the tailq associated with an spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return The head of a tailq of spdk_bdev_part structs registered to the base's module.
 */
struct bdev_part_tailq *spdk_bdev_part_base_get_tailq(struct spdk_bdev_part_base *part_base);

/**
 * Returns a pointer to the module level context associated with an spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return A pointer to the module level context registered with the base in spdk_bdev_part_base_construct.
 */
void *spdk_bdev_part_base_get_ctx(struct spdk_bdev_part_base *part_base);

typedef void (*spdk_bdev_part_base_free_fn)(void *ctx);

struct spdk_bdev_part {
	/* Entry into the module's global list of bdev parts */
	TAILQ_ENTRY(spdk_bdev_part)	tailq;

	/**
	 * Fields that are used internally by part.c These fields should only
	 * be accessed from a module using any pertinent get and set methods.
	 */
	struct bdev_part_internal_fields {

		/* This part's corresponding bdev object. Not to be confused with the base bdev */
		struct spdk_bdev		bdev;

		/* The base to which this part belongs */
		struct spdk_bdev_part_base	*base;

		/* number of blocks from the start of the base bdev to the start of this part */
		uint64_t			offset_blocks;
	} internal;
};

struct spdk_bdev_part_channel {
	struct spdk_bdev_part		*part;
	struct spdk_io_channel		*base_ch;
};

typedef TAILQ_HEAD(bdev_part_tailq, spdk_bdev_part)	SPDK_BDEV_PART_TAILQ;

/**
 * Free the base corresponding to one or more spdk_bdev_part.
 *
 * \param base The base to free.
 */
void spdk_bdev_part_base_free(struct spdk_bdev_part_base *base);

/**
 * Free an spdk_bdev_part context.
 *
 * \param part The part to free.
 *
 * \return 1 always. To indicate that the operation is asynchronous.
 */
int spdk_bdev_part_free(struct spdk_bdev_part *part);

/**
 * Calls spdk_bdev_unregister on the bdev for each part associated with base_bdev.
 *
 * \param part_base The part base object built on top of an spdk_bdev
 * \param tailq The list of spdk_bdev_part bdevs associated with this base bdev.
 */
void spdk_bdev_part_base_hotremove(struct spdk_bdev_part_base *part_base,
				   struct bdev_part_tailq *tailq);

/**
 * Construct a new spdk_bdev_part_base on top of the provided bdev.
 *
 * \param bdev_name Name of the bdev upon which this base will be built.
 * \param remove_cb Function to be called upon hotremove of the bdev.
 * \param module The module to which this bdev base belongs.
 * \param fn_table Function table for communicating with the bdev backend.
 * \param tailq The head of the list of all spdk_bdev_part structures registered to this base's module.
 * \param free_fn User provided function to free base related context upon bdev removal or shutdown.
 * \param ctx Module specific context for this bdev part base.
 * \param channel_size Channel size in bytes.
 * \param ch_create_cb Called after a new channel is allocated.
 * \param ch_destroy_cb Called upon channel deletion.
 * \param base output parameter for the part object when operation is successful.
 *
 * \return 0 if operation is successful, or suitable errno value otherwise.
 */
int spdk_bdev_part_base_construct_ext(const char *bdev_name,
				      spdk_bdev_remove_cb_t remove_cb,
				      struct spdk_bdev_module *module,
				      struct spdk_bdev_fn_table *fn_table,
				      struct bdev_part_tailq *tailq,
				      spdk_bdev_part_base_free_fn free_fn,
				      void *ctx,
				      uint32_t channel_size,
				      spdk_io_channel_create_cb ch_create_cb,
				      spdk_io_channel_destroy_cb ch_destroy_cb,
				      struct spdk_bdev_part_base **base);

/** Options used when constructing a part bdev. */
struct spdk_bdev_part_construct_opts {
	/* Size of this structure in bytes */
	uint64_t opts_size;
	/** UUID of the bdev */
	struct spdk_uuid uuid;
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_part_construct_opts) == 24, "Incorrect size");

/**
 * Initialize options that will be passed to spdk_bdev_part_construct_ext().
 *
 * \param opts Options structure to initialize
 * \param size Size of opts structure.
 */
void spdk_bdev_part_construct_opts_init(struct spdk_bdev_part_construct_opts *opts, uint64_t size);

/**
 * Create a logical spdk_bdev_part on top of a base.
 *
 * \param part The part object allocated by the user.
 * \param base The base from which to create the part.
 * \param name The name of the new spdk_bdev_part.
 * \param offset_blocks The offset into the base bdev at which this part begins.
 * \param num_blocks The number of blocks that this part will span.
 * \param product_name Unique name for this type of block device.
 *
 * \return 0 on success.
 * \return -1 if the bases underlying bdev cannot be claimed by the current module.
 */
int spdk_bdev_part_construct(struct spdk_bdev_part *part, struct spdk_bdev_part_base *base,
			     char *name, uint64_t offset_blocks, uint64_t num_blocks,
			     char *product_name);

/**
 * Create a logical spdk_bdev_part on top of a base with a non-NULL bdev UUID
 *
 * \param part The part object allocated by the user.
 * \param base The base from which to create the part.
 * \param name The name of the new spdk_bdev_part.
 * \param offset_blocks The offset into the base bdev at which this part begins.
 * \param num_blocks The number of blocks that this part will span.
 * \param product_name Unique name for this type of block device.
 * \param opts Additional options.
 *
 * \return 0 on success.
 * \return -1 if the bases underlying bdev cannot be claimed by the current module.
 */
int spdk_bdev_part_construct_ext(struct spdk_bdev_part *part, struct spdk_bdev_part_base *base,
				 char *name, uint64_t offset_blocks, uint64_t num_blocks,
				 char *product_name,
				 const struct spdk_bdev_part_construct_opts *opts);

/**
 * Forwards I/O from an spdk_bdev_part to the underlying base bdev.
 *
 * This function will apply the offset_blocks the user provided to
 * spdk_bdev_part_construct to the I/O. The user should not manually
 * apply this offset before submitting any I/O through this function.
 *
 * \param ch The I/O channel associated with the spdk_bdev_part.
 * \param bdev_io The I/O to be submitted to the underlying bdev.
 * \return 0 on success or non-zero if submit request failed.
 */
int spdk_bdev_part_submit_request(struct spdk_bdev_part_channel *ch, struct spdk_bdev_io *bdev_io);

/**
 * Forwards I/O from an spdk_bdev_part to the underlying base bdev.
 *
 * This function will apply the offset_blocks the user provided to
 * spdk_bdev_part_construct to the I/O. The user should not manually
 * apply this offset before submitting any I/O through this function.
 *
 * This function enables user to specify a completion callback. It is required that
 * the completion callback calls spdk_bdev_io_complete() for the forwarded I/O.
 *
 * \param ch The I/O channel associated with the spdk_bdev_part.
 * \param bdev_io The I/O to be submitted to the underlying bdev.
 * \param cb Called when the forwarded I/O completes.
 * \return 0 on success or non-zero if submit request failed.
 */
int spdk_bdev_part_submit_request_ext(struct spdk_bdev_part_channel *ch,
				      struct spdk_bdev_io *bdev_io,
				      spdk_bdev_io_completion_cb cb);

/**
 * Return a pointer to this part's spdk_bdev.
 *
 * \param part An spdk_bdev_part object.
 *
 * \return A pointer to this part's spdk_bdev object.
 */
struct spdk_bdev *spdk_bdev_part_get_bdev(struct spdk_bdev_part *part);

/**
 * Return a pointer to this part's base.
 *
 * \param part An spdk_bdev_part object.
 *
 * \return A pointer to this part's spdk_bdev_part_base object.
 */
struct spdk_bdev_part_base *spdk_bdev_part_get_base(struct spdk_bdev_part *part);

/**
 * Return a pointer to this part's base bdev.
 *
 * The return value of this function is equivalent to calling
 * spdk_bdev_part_base_get_bdev on this part's base.
 *
 * \param part An spdk_bdev_part object.
 *
 * \return A pointer to the bdev belonging to this part's base.
 */
struct spdk_bdev *spdk_bdev_part_get_base_bdev(struct spdk_bdev_part *part);

/**
 * Return this part's offset from the beginning of the base bdev.
 *
 * This function should not be called in the I/O path. Any block
 * translations to I/O will be handled in spdk_bdev_part_submit_request.
 *
 * \param part An spdk_bdev_part object.
 *
 * \return the block offset of this part from it's underlying bdev.
 */
uint64_t spdk_bdev_part_get_offset_blocks(struct spdk_bdev_part *part);

/**
 * Push media management events.  To send the notification that new events are
 * available, spdk_bdev_notify_media_management needs to be called.
 *
 * \param bdev Block device
 * \param events Array of media events
 * \param num_events Size of the events array
 *
 * \return number of events pushed or negative errno in case of failure
 */
int spdk_bdev_push_media_events(struct spdk_bdev *bdev, const struct spdk_bdev_media_event *events,
				size_t num_events);

/**
 * Send SPDK_BDEV_EVENT_MEDIA_MANAGEMENT to all open descriptors that have
 * pending media events.
 *
 * \param bdev Block device
 */
void spdk_bdev_notify_media_management(struct spdk_bdev *bdev);

typedef int (*spdk_bdev_io_fn)(void *ctx, struct spdk_bdev_io *bdev_io);
typedef void (*spdk_bdev_for_each_io_cb)(void *ctx, int rc);

/**
 * Call the provided function on the appropriate thread for each bdev_io submitted
 * to the provided bdev.
 *
 * Note: This function should be used only in the bdev module and it should be
 * ensured that the bdev is not unregistered while executing the function.
 * Both fn and cb are required to specify.
 *
 * \param bdev Block device to query.
 * \param ctx Context passed to the function for each bdev_io and the completion
 * callback function.
 * \param fn Called on the appropriate thread for each bdev_io submitted to the bdev.
 * \param cb Called when this operation completes.
 */
void spdk_bdev_for_each_bdev_io(struct spdk_bdev *bdev, void *ctx, spdk_bdev_io_fn fn,
				spdk_bdev_for_each_io_cb cb);

typedef void (*spdk_bdev_get_current_qd_cb)(struct spdk_bdev *bdev, uint64_t current_qd,
		void *cb_arg, int rc);

/**
 * Measure and return the queue depth from a bdev.
 *
 * Note: spdk_bdev_get_qd() works only when the user enables queue depth sampling,
 * while this new function works even when queue depth sampling is disabled.
 * The returned queue depth may not be exact, for example, some additional I/Os may
 * have been submitted or completed during the for_each_channel operation.
 * This function should be used only in the bdev module and it should be ensured
 * that the dev is not unregistered while executing the function.
 * cb_fn is required to specify.
 *
 * \param bdev Block device to query.
 * \param cb_fn Callback function to be called with queue depth measured for a bdev.
 * \param cb_arg Argument to pass to callback function.
 */
void spdk_bdev_get_current_qd(struct spdk_bdev *bdev,
			      spdk_bdev_get_current_qd_cb cb_fn, void *cb_arg);

/**
 * Add I/O statistics.
 *
 * \param total The aggregated I/O statistics.
 * \param add The I/O statistics to be added.
 */
void spdk_bdev_add_io_stat(struct spdk_bdev_io_stat *total, struct spdk_bdev_io_stat *add);

/**
 * Output bdev I/O statistics information to a JSON stream.
 *
 * \param stat The bdev I/O statistics to output.
 * \param w JSON write context.
 */
void spdk_bdev_dump_io_stat_json(struct spdk_bdev_io_stat *stat, struct spdk_json_write_ctx *w);

/**
 * Reset I/O statistics structure.
 *
 * \param stat The I/O statistics to reset.
 * \param mode The mode to reset I/O statistics.
 */
void spdk_bdev_reset_io_stat(struct spdk_bdev_io_stat *stat, enum spdk_bdev_reset_stat_mode mode);

typedef void (*spdk_bdev_quiesce_cb)(void *ctx, int status);

/**
 * Quiesce a bdev. All I/O submitted after this function is called will be queued until
 * the bdev is unquiesced. A callback will be called when all outstanding I/O on this bdev
 * submitted before calling this function have completed.
 *
 * Only the module that registered the bdev may call this function.
 *
 * \param bdev Block device.
 * \param module The module that registered the bdev.
 * \param cb_fn Callback function to be called when the bdev is quiesced. Optional.
 * \param cb_arg Argument to be supplied to cb_fn.
 *
 * \return 0 on success, or suitable errno value otherwise.
 */
int spdk_bdev_quiesce(struct spdk_bdev *bdev, struct spdk_bdev_module *module,
		      spdk_bdev_quiesce_cb cb_fn, void *cb_arg);

/**
 * Unquiesce a previously quiesced bdev. All I/O queued after the bdev was quiesced
 * will be submitted.
 *
 * Only the module that registered the bdev may call this function.
 *
 * \param bdev Block device.
 * \param module The module that registered the bdev.
 * \param cb_fn Callback function to be called when the bdev is unquiesced. Optional.
 * \param cb_arg Argument to be supplied to cb_fn.
 *
 * \return 0 on success, or suitable errno value otherwise.
 */
int spdk_bdev_unquiesce(struct spdk_bdev *bdev, struct spdk_bdev_module *module,
			spdk_bdev_quiesce_cb cb_fn, void *cb_arg);

/**
 * Quiesce a bdev LBA range.
 * Same as spdk_bdev_quiesce() but limited to the specified LBA range.
 *
 * \param bdev Block device.
 * \param module The module that registered the bdev.
 * \param offset The offset of the start of the range, in blocks,
 *               from the start of the block device.
 * \param length The length of the range, in blocks.
 * \param cb_fn Callback function to be called when the range is quiesced. Optional.
 * \param cb_arg Argument to be supplied to cb_fn.
 *
 * \return 0 on success, or suitable errno value otherwise.
 */
int spdk_bdev_quiesce_range(struct spdk_bdev *bdev, struct spdk_bdev_module *module,
			    uint64_t offset, uint64_t length,
			    spdk_bdev_quiesce_cb cb_fn, void *cb_arg);

/**
 * Unquiesce a previously quiesced bdev LBA range.
 * Same as spdk_bdev_unquiesce() but limited to the specified LBA range.
 * The specified range must match exactly a previously quiesced LBA range.
 *
 * \param bdev Block device.
 * \param module The module that registered the bdev.
 * \param offset The offset of the start of the range, in blocks,
 *               from the start of the block device.
 * \param length The length of the range, in blocks.
 * \param cb_fn Callback function to be called when the range is unquiesced. Optional.
 * \param cb_arg Argument to be supplied to cb_fn.
 *
 * \return 0 on success, or suitable errno value otherwise.
 */
int spdk_bdev_unquiesce_range(struct spdk_bdev *bdev, struct spdk_bdev_module *module,
			      uint64_t offset, uint64_t length,
			      spdk_bdev_quiesce_cb cb_fn, void *cb_arg);

/*
 *  Macro used to register module for later initialization.
 */
#define SPDK_BDEV_MODULE_REGISTER(name, module) \
static void __attribute__((constructor)) _spdk_bdev_module_register_##name(void) \
{ \
	spdk_bdev_module_list_add(module); \
}

#endif /* __SPDK_BDEV_MODULE_ONLY */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_MODULE_H */
