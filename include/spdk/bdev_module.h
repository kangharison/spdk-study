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
/*
 * [한국어] ★ struct spdk_bdev_module — bdev 모듈 등록 루트 ★
 *
 * 각 bdev 백엔드(NVMe, AIO, malloc, uring, raid, lvol, crypto, delay, ...)는
 * `SPDK_BDEV_MODULE_REGISTER` 매크로로 이 구조체 인스턴스 하나를 등록한다.
 * 애플리케이션 초기화 시 bdev 코어가 모든 등록 모듈을 순회하며 module_init
 * → examine_config → examine_disk 순으로 부트스트랩한다.
 *
 * 수명 주기:
 *   1) SPDK_BDEV_MODULE_REGISTER로 모듈 전역 변수 정적 등록(컨스트럭터)
 *   2) spdk_bdev_initialize 시 bdev 코어가 module_init() 호출 (순서: 등록 순)
 *   3) 각 bdev 인스턴스 등록 시 examine_config → examine_disk 순회
 *   4) init_complete() 호출 (optional)
 *   5) 종료 시 fini_start → bdev unregister → module_fini
 *
 * 모듈 종류:
 *   - 실제(physical) bdev 모듈: NVMe, AIO, uring, malloc, null — 직접 장치 노출
 *   - virtual(vbdev) 모듈: raid, lvol, crypto, delay, error, passthru, split —
 *     기존 bdev을 claim하여 상위에 새 bdev을 만든다 (examine_config/disk에서)
 */
struct spdk_bdev_module {
	/**
	 * Initialization function for the module. Called by the bdev library
	 * during startup.
	 *
	 * Modules are required to define this function.
	 */
	int (*module_init)(void);
                                  /* [한국어] 모듈 초기화 콜백 — spdk_bdev_initialize 시 호출
                                   *  - 동기: 0 성공 / 음수 errno 실패
                                   *  - 비동기(async_init=true): 이 함수에서 spdk_bdev_module_init_done() 호출 전까지 반환해도 init 완료 대기
                                   *  - 필수 구현 */

	/**
	 * Optional callback for modules that require notification of when
	 * the bdev subsystem has completed initialization.
	 *
	 * Modules are not required to define this function.
	 */
	void (*init_complete)(void);
                                  /* [한국어] 모든 모듈 init 완료 시 호출 — 모듈 간 의존성 해결 후 최종 설정 필요 시 사용 */

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
                                  /* [한국어] 종료 프로세스 시작 통지
                                   *  - 중요: vbdev을 노출하지 않고 claim만 건 모듈은 여기서 반드시 release
                                   *  - async_fini_start=true이면 spdk_bdev_module_fini_start_done() 호출까지 대기 */

	/**
	 * Finish function for the module. Called by the bdev library
	 * after all bdevs for all modules have been unregistered.  This allows
	 * the module to do any final cleanup before the bdev library finishes operation.
	 *
	 * Modules are not required to define this function.
	 */
	void (*module_fini)(void);
                                  /* [한국어] 모든 bdev unregister 완료 후 최종 cleanup
                                   *  - async_fini=true이면 spdk_bdev_module_fini_done() 호출까지 대기 */

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
                                  /* [한국어] 모듈 수준 JSON-RPC 구성 출력 — "save_config" 재현용
                                   *  - 한 RPC가 여러 bdev을 만드는 경우(NVMe 컨트롤러 → 다중 NS)에 적합
                                   *  - bdev 단위 1:1 대응은 spdk_bdev_fn_table.write_config_json 사용 — 두 중 하나만 선택 */

	/** Name for the modules being defined. */
	const char *name;
                                  /* [한국어] 모듈 고유 이름 (예: "nvme", "aio", "malloc") — RPC/로그 식별자 */

	/**
	 * Returns the allocation size required for the backend for uses such as local
	 * command structs, local SGL, iovecs, or other user context.
	 */
	int (*get_ctx_size)(void);
                                  /* [한국어] 이 모듈이 bdev_io 당 필요한 trailing ctx(driver_ctx) 크기 반환
                                   *  - bdev 코어가 mempool 엔트리 크기를 이 값만큼 확장해 할당
                                   *  - NULL이면 모듈이 추가 컨텍스트 필요 없음 */

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
                                  /* [한국어] vbdev 모듈의 1차 검사 — 새 bdev 등장 알림
                                   *  - I/O 발행 불가 (아직 open 전)
                                   *  - config 기반 vbdev 생성(예: 구성 파일에 정의된 lvol store 복원)만 가능
                                   *  - claim 결정을 **동기적으로** 확정해야 함
                                   *  - 반환 전 반드시 spdk_bdev_module_examine_done() 호출 */

	/**
	 * Second notification that a bdev should be examined by a virtual bdev module.
	 * Virtual bdev modules may use this to examine newly-added bdevs and automatically
	 * create their own vbdevs. This callback may use I/O operations and finish asynchronously.
	 * Once complete spdk_bdev_module_examine_done() must be called.
	 */
	void (*examine_disk)(struct spdk_bdev *bdev);
                                  /* [한국어] vbdev 모듈의 2차 검사 — 디스크 I/O 수행 가능
                                   *  - 예: blobstore super block 읽어서 lvol store 자동 발견
                                   *  - 비동기 — 완료 시 spdk_bdev_module_examine_done() 호출 */

	/**
	 * Denotes if the module_init function may complete asynchronously. If set to true,
	 * the module initialization has to be explicitly completed by calling
	 * spdk_bdev_module_init_done().
	 */
	bool async_init;
                                  /* [한국어] module_init 비동기 완료 표시 */

	/**
	 * Denotes if the module_fini function may complete asynchronously.
	 * If set to true finishing has to be explicitly completed by calling
	 * spdk_bdev_module_fini_done().
	 */
	bool async_fini;
                                  /* [한국어] module_fini 비동기 완료 표시 */

	/**
	 * Denotes if the fini_start function may complete asynchronously.
	 * If set to true finishing has to be explicitly completed by calling
	 * spdk_bdev_module_fini_start_done().
	 */
	bool async_fini_start;
                                  /* [한국어] fini_start 비동기 완료 표시 */

	/**
	 * Fields that are used by the internal bdev subsystem. Bdev modules
	 *  must not read or write to these fields.
	 */
	struct __bdev_module_internal_fields {
                                  /* [한국어] bdev 코어 전용 내부 필드 — 모듈 코드가 접근 금지 */
		/**
		 * Protects action_in_progress and quiesced_ranges.
		 * Take no locks while holding this one.
		 */
		struct spdk_spinlock spinlock;
                                  /* [한국어] action_in_progress·quiesced_ranges 보호 spin lock
                                   *  - 이 lock을 들고 다른 lock 획득 금지 (데드락 방지 규칙) */

		/**
		 * Count of bdev inits/examinations in progress. Used by generic bdev
		 * layer and must not be modified by bdev modules.
		 *
		 * \note Used internally by bdev subsystem, don't change this value in bdev module.
		 */
		uint32_t action_in_progress;
                                  /* [한국어] 진행 중인 init/examine 카운터 — bdev 코어가 완료 동기화에 사용 */

		/**
		 * List of quiesced lba ranges in all bdevs of this module.
		 */
		TAILQ_HEAD(, lba_range) quiesced_ranges;
                                  /* [한국어] quiesce된 LBA 범위 리스트 — 모듈의 모든 bdev을 망라
                                   *  - quiesce: 해당 구간 I/O 일시 정지 (snapshot/정합성 작업용) */

		TAILQ_ENTRY(spdk_bdev_module) tailq;
                                  /* [한국어] 전역 bdev_modules 리스트 링크 */
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

/*
 * [한국어] struct spdk_bdev_name - bdev 이름 엔트리
 *
 * 이름과 bdev 포인터를 RB 트리 노드로 묶음. 전역 bdev 이름 트리(bdev_names)의
 * 엔트리로 사용되어 이름→bdev 포인터를 O(log n)에 lookup한다.
 */
struct spdk_bdev_name {
	char *name;                   /* [한국어] bdev의 이름 (예: "Nvme0n1", "Malloc0"). heap-allocated */
	struct spdk_bdev *bdev;       /* [한국어] 이 이름이 가리키는 bdev 포인터 */
	RB_ENTRY(spdk_bdev_name) node;/* [한국어] RB 트리 링크 */
};

/*
 * [한국어] struct spdk_bdev_alias - bdev 별칭 엔트리
 *
 * 하나의 bdev이 본이름 외 여러 별칭을 가질 수 있음. 각 별칭은 동일 bdev을 가리키는
 * 추가 이름으로 등록되어 이름 lookup에 동일하게 참여.
 */
struct spdk_bdev_alias {
	struct spdk_bdev_name alias;  /* [한국어] 별칭 이름 엔트리 (bdev 포인터 공유) */
	TAILQ_ENTRY(spdk_bdev_alias) tailq; /* [한국어] bdev->aliases 리스트 링크 */
};

/*
 * [한국어] struct spdk_bdev_module_claim - bdev claim 엔트리 (v2 API)
 *
 * claim은 "이 bdev의 I/O 제어를 이 모듈이 점유한다"는 선언. v1 API(spdk_bdev_module_claim_bdev)는
 * 모듈 포인터 하나만 저장했지만, v2 API(spdk_bdev_module_claim_bdev_desc)는 여러 claim 타입과
 * 다중 writer 공유를 지원하므로 각 claim을 이 엔트리로 개별 표현.
 */
struct spdk_bdev_module_claim {
	struct spdk_bdev_module *module;
                                  /* [한국어] claim을 건 모듈 */
	struct spdk_bdev_desc *desc;  /* [한국어] claim 시 사용된 descriptor (closed 시 claim 자동 해제) */
	char name[SPDK_BDEV_CLAIM_NAME_LEN];
                                  /* [한국어] claim 식별 이름 (로그/진단용, 임의 문자열) */
	TAILQ_ENTRY(spdk_bdev_module_claim) link;
                                  /* [한국어] bdev->internal.claim.v2.claims 리스트 링크 */
};

typedef TAILQ_HEAD(, spdk_bdev_io) bdev_io_tailq_t;
                                  /* [한국어] bdev_io TAILQ 헤드 타입 별칭 — 여러 내부 리스트에서 반복 사용 */
typedef STAILQ_HEAD(, spdk_bdev_io) bdev_io_stailq_t;
                                  /* [한국어] bdev_io STAILQ 헤드 (단방향 연결 버전) */
typedef TAILQ_HEAD(, lba_range) lba_range_tailq_t;
                                  /* [한국어] LBA range TAILQ 헤드 — quiesce/lock 범위 관리 */

/*
 * [한국어] ★★★ struct spdk_bdev - bdev 인스턴스 메타데이터 ★★★
 *
 * SPDK가 노출하는 하나의 논리 블록 디바이스를 표현. 모듈이
 * spdk_bdev_register()로 등록할 때 이 구조체를 채워 전달하며, bdev 코어가
 * 이 메타데이터로 사용자 I/O를 라우팅·검증·split한다.
 *
 * 주요 정보 범주:
 *   - 식별: ctxt, name, aliases, product_name, uuid, nsid
 *   - 기하(geometry): blocklen, phys_blocklen, blockcnt, md_len
 *   - 지원 기능 비트맵: io_type_supported, accel_sequence_supported
 *   - I/O 제약/최적화 힌트: required_alignment, optimal_io_boundary,
 *     write_unit_size, max_rw_size, max_segment_size, max_num_segments 등
 *   - 제한(split 기준): max_unmap, max_write_zeroes, max_copy
 *   - DIF/PI: dif_type, dif_pi_format, dif_check_flags, md_interleave, dif_is_head_of_md
 *   - ZNS: zoned, zone_size, max_open/active_zones, optimal_open_zones, max_zone_append_size
 *   - NVMe: ctratt, nsid
 *   - reset 동작: reset_io_drain_timeout
 *   - NUMA locality: numa
 *   - 연결: module(등록자), fn_table(콜백 vtable)
 *   - internal: bdev 코어 전용 (QoS, claim, open_descs, reset 진행, 통계 등)
 *
 * I/O 경로에서의 역할:
 *   - 사용자 API가 blocklen/blockcnt로 LBA·길이 검증
 *   - bdev 코어가 optimal_io_boundary·max_rw_size로 자동 split 결정
 *   - 모듈 콜백이 bdev->ctxt로 backend context 복원 (NVMe ns 포인터 등)
 */
struct spdk_bdev {
	/** User context passed in by the backend */
	void *ctxt;
                                  /* [한국어] 모듈이 bdev 등록 시 넣는 자유 컨텍스트
                                   *  - 예: NVMe 모듈은 spdk_nvme_ns* 를 여기 저장
                                   *  - fn_table의 모든 콜백이 첫 인자 ctx로 이 값을 받음 */

	/** Unique name for this block device. */
	char *name;
                                  /* [한국어] 전역 고유 이름 (예: "Nvme0n1") — RPC/CLI 참조 키
                                   *  - 전역 bdev_names RB 트리 키 */

	/** Unique aliases for this block device. */
	TAILQ_HEAD(spdk_bdev_aliases_list, spdk_bdev_alias) aliases;
                                  /* [한국어] 별칭 리스트 — 하나의 bdev가 여러 이름으로 조회 가능 */

	/** Unique product name for this kind of block device. */
	char *product_name;
                                  /* [한국어] 제품/드라이버 이름 (예: "NVMe disk", "Malloc disk") — 사용자 표시용 */

	/** write cache enabled, not used at the moment */
	int write_cache;
                                  /* [한국어] 쓰기 캐시 활성 플래그 (현재 미사용, 향후 확장용) */

	/** Size in bytes of a logical block for the backend */
	uint32_t blocklen;
                                  /* [한국어] 논리 블록 크기 (바이트, 일반적으로 512 또는 4096)
                                   *  - 사용자 API의 LBA 단위와 I/O 크기 계산에 사용 */

	/** Size in bytes of a physical block for the backend */
	uint32_t phys_blocklen;
                                  /* [한국어] 물리 블록 크기 — 4K 섹터 드라이브에서 논리=512, 물리=4096 가능 */

	/** Bitmap of supported io types */
	uint32_t io_type_supported;
                                  /* [한국어] 지원하는 I/O 타입 비트마스크 (enum spdk_bdev_io_type의 각 비트)
                                   *  - 사용자 API가 제출 전 SPDK_BDEV_IO_TYPE_*에 대한 이 비트를 확인 */

	/** Number of blocks */
	uint64_t blockcnt;
                                  /* [한국어] 총 블록 수 — 총 용량 = blockcnt * blocklen */

	struct {
                                  /* [한국어] 비트필드 플래그 묶음 — 32비트 하나 내에 여러 bool 압축 */
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
                                  /* [한국어] WRITE I/O가 write_unit_size 경계를 넘으면 자동 split
                                   *  - true면 강제 분할 (NVMe NPWG 같은 하드웨어 요구사항)
                                   *  - UNMAP/WRITE_ZEROES/FLUSH에는 적용 안 됨 */

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
                                  /* [한국어] optimal_io_boundary 경계에서 R/W 강제 split
                                   *  - NVMe NOIOB (Namespace Optimal I/O Boundary)에 대응 */

		/**
		 * Specify metadata location and set to true if metadata is interleaved
		 * with block data or false if metadata is separated with block data.
		 *
		 * Note that this field is valid only if there is metadata.
		 */
		uint32_t md_interleave : 1;
                                  /* [한국어] 메타데이터가 블록 데이터와 섞여있는지(extended LBA)
                                   *  - true: 각 LBA 말미에 메타 (예: 512+8)
                                   *  - false: 별도 메타 버퍼(mptr)에 분리 저장 */

		/*
		 * DIF location.
		 *
		 * Set to true if DIF is set in the first 8/16 bytes of metadata or false
		 * if DIF is set in the last 8/16 bytes of metadata.
		 *
		 * Note that this field is valid only if DIF is enabled.
		 */
		uint32_t dif_is_head_of_md : 1;
                                  /* [한국어] DIF가 메타데이터의 앞/뒤 어디에 위치하는지
                                   *  - NVMe: DPS(Data Protection Settings) 비트 0이 head/tail 지정
                                   *  - true = 앞, false = 뒤 */

		/**
		 * Specify whether bdev is zoned device.
		 */
		uint32_t zoned : 1;
                                  /* [한국어] Zoned Namespace(ZNS) 여부 — true면 zone_size 등 zone 관련 필드 유효 */

		/**
		 * Specifies whether bdev supports media management events.
		 */
		uint32_t media_events : 1;
                                  /* [한국어] 미디어 관리 이벤트(예: wear-out 경고) 지원 여부 */

		uint32_t memory_domains_supported : 1;
                                  /* [한국어] 메모리 도메인 지원(RDMA 등) — get_memory_domains 콜백 활성화 */

		uint32_t reserved : 25;
                                  /* [한국어] 예약 비트 — 25개 추가 플래그 공간 */
	};

	/** Number of blocks required for write */
	uint32_t write_unit_size;
                                  /* [한국어] 쓰기 단위 블록 수 (NVMe NPWG/NPWA 기반)
                                   *  - 이 크기 경계로 정렬된 write이 최적 성능 */

	/** Atomic compare & write unit */
	uint16_t acwu;
                                  /* [한국어] Atomic Compare & Write Unit — atomic 비교+쓰기가 보장되는 블록 수
                                   *  - NVMe 컨트롤러의 ACWU identify 필드에 대응 */

	/**
	 * Specifies an alignment requirement for data buffers associated with an spdk_bdev_io.
	 * 0 = no alignment requirement
	 * >0 = alignment requirement is 2 ^ required_alignment.
	 * bdev layer will automatically double buffer any spdk_bdev_io that violates this
	 * alignment, before the spdk_bdev_io is submitted to the bdev module.
	 */
	uint8_t required_alignment;
                                  /* [한국어] 데이터 버퍼 정렬 요구사항 — 2^required_alignment 바이트 단위
                                   *  - 0: 정렬 요구 없음
                                   *  - 위반 시 bdev 코어가 bounce buffer로 double buffering 자동 수행 (성능 저하 발생)
                                   *  - NVMe는 통상 블록 크기(9 또는 12) */

	uint8_t reserved1;            /* [한국어] 정렬 예약 */

	/**
	 * Optimal I/O boundary in blocks, or 0 for no value reported.
	 */
	uint32_t optimal_io_boundary;
                                  /* [한국어] 최적 I/O 경계 (블록 단위) — 이 경계를 넘는 I/O는 성능 저하 가능
                                   *  - split_on_optimal_io_boundary=true면 강제 분할
                                   *  - 0이면 제약 없음 */

	/** Size in blocks of the preferred write alignment for the backend */
	uint32_t preferred_write_alignment;
                                  /* [한국어] 권장 쓰기 정렬 (블록) — 이 정렬로 쓰면 최적 성능 (강제 아님) */

	/** Size in blocks of the preferred write granularity for the backend */
	uint32_t preferred_write_granularity;
                                  /* [한국어] 권장 쓰기 단위 (블록) — 이 크기의 배수로 쓰는 것이 최적 */

	/** Size in blocks of the optimal write size for the backend */
	uint32_t optimal_write_size;
                                  /* [한국어] 최적 쓰기 크기 (블록) — 한 번에 이 크기로 쓸 때 최대 throughput */

	/** Size in blocks of the preferred unmap alignment for the backend */
	uint32_t preferred_unmap_alignment;
                                  /* [한국어] 권장 UNMAP 정렬 (블록) */

	/** Size in blocks of the preferred unmap granularity for the backend */
	uint32_t preferred_unmap_granularity;
                                  /* [한국어] 권장 UNMAP 단위 (블록) */

	/**
	 * Max io size in bytes of a single segment
	 *
	 * Note: both max_segment_size and max_num_segments
	 * should be zero or non-zero.
	 */
	uint32_t max_segment_size;
                                  /* [한국어] 단일 iovec 세그먼트의 최대 크기 (바이트)
                                   *  - max_num_segments와 쌍으로 0 또는 모두 비영 (어긋나면 검증 실패) */

	/* Maximum number of segments in a I/O */
	uint32_t max_num_segments;
                                  /* [한국어] 한 I/O가 가질 수 있는 iovec 최대 개수 */

	/* Maximum unmap in unit of logical block */
	uint32_t max_unmap;
                                  /* [한국어] UNMAP 명령의 최대 블록 수 — 초과 시 bdev 코어가 자동 분할 */

	/* Maximum unmap block segments */
	uint32_t max_unmap_segments;
                                  /* [한국어] UNMAP의 최대 range 개수 (NVMe DSM 범위 세트 수) */

	/* Maximum write zeroes in unit of logical block */
	uint32_t max_write_zeroes;
                                  /* [한국어] WRITE_ZEROES 최대 블록 수 — 초과 시 분할 */

	/**
	 * Maximum copy size in unit of logical block
	 * Should be set explicitly when backing device support copy command
	 */
	uint32_t max_copy;
                                  /* [한국어] COPY 명령(NVMe SIMPLE COPY) 최대 블록 수 */

	/**
	 * Maximum number of blocks in a single read/write I/O.  Requests exceeding this value will
	 * be split by the bdev layer.
	 */
	uint32_t max_rw_size;
                                  /* [한국어] 단일 R/W I/O 최대 블록 수 — 장치 MDTS 기반
                                   *  - 초과 시 bdev 코어가 bdev_io를 split하여 여러 sub-IO로 제출 */

	/**
	 * UUID for this bdev.
	 *
	 * If not provided, it will be generated by bdev layer.
	 */
	struct spdk_uuid uuid;
                                  /* [한국어] bdev 고유 UUID — 영속 식별자 (재시작해도 같은 bdev을 식별)
                                   *  - 모듈이 설정 안 하면 bdev 코어가 자동 생성 */

	/** Size in bytes of a metadata for the backend */
	uint32_t md_len;
                                  /* [한국어] 블록당 메타데이터 크기 (바이트, PI 포함) — 0이면 메타 없음 */

	uint8_t reserved2[4];         /* [한국어] ABI 예약 */

	/**
	 * DIF type for this bdev.
	 *
	 * Note that this field is valid only if there is metadata.
	 */
	enum spdk_dif_type dif_type;
                                  /* [한국어] DIF 타입 (DISABLE/TYPE1/TYPE2/TYPE3) — md_len>0일 때만 유효 */

	/**
	 * DIF protection information format for this bdev.
	 *
	 * Note that this field is valid only if there is metadata and dif_type is
	 * not SPDK_DIF_DISABLE.
	 */
	enum spdk_dif_pi_format dif_pi_format;
                                  /* [한국어] PI 포맷 — 16/32/64-bit Guard 선택 (NVMe 2.0) */

	uint8_t reserved3[4];         /* [한국어] 예약 */

	/**
	 * Specify whether each DIF check type is enabled.
	 */
	uint32_t dif_check_flags;
                                  /* [한국어] DIF 검사 플래그 비트마스크 (GUARD/APPTAG/REFTAG 개별 on/off) */

	uint8_t reserved4[8];         /* [한국어] 예약 */

	/**
	 * Default size of each zone (in blocks).
	 */
	uint64_t zone_size;
                                  /* [한국어] ZNS: zone 하나의 크기 (블록). zoned=true일 때만 의미 */

	/**
	 * Maximum zone append data transfer size (in blocks).
	 */
	uint32_t max_zone_append_size;
                                  /* [한국어] ZNS: Zone Append 최대 크기 (블록) */

	/**
	 * Maximum number of open zones.
	 */
	uint32_t max_open_zones;
                                  /* [한국어] 동시에 open 상태일 수 있는 zone 최대 수 */

	/**
	 * Maximum number of active zones.
	 */
	uint32_t max_active_zones;
                                  /* [한국어] active(open+closed) zone 최대 수 */

	/**
	 * Optimal number of open zones.
	 */
	uint32_t optimal_open_zones;
                                  /* [한국어] 권장 open zone 수 (호스트 스케줄러 참고용) */

	uint8_t reserved6[4];         /* [한국어] 예약 */

	/**
	 * Specifies the bdev nvme controller attributes.
	 */
	union spdk_bdev_nvme_ctratt ctratt;
                                  /* [한국어] NVMe 컨트롤러 속성 비트필드 — bdev_nvme 모듈이 CTRATT identify 값을 전달 */

	/**
	 * NVMe namespace ID.
	 */
	uint32_t nsid;
                                  /* [한국어] NVMe NSID — bdev_nvme 모듈에서만 의미 (다른 모듈은 0) */

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
                                  /* [한국어] reset 수신 시 in-flight I/O drain 대기 시간 (초)
                                   *  - 이 시간 안에 I/O 완료되면 reset 생략(skip)
                                   *  - 0: 즉시 reset 강제 발행 (대기 없음, outstanding 관계없이)
                                   *  - 공유 bdev(lvol이 하위 NVMe 공유) 등에서 불필요 reset 전파 방지
                                   *  - 권장값: SPDK_BDEV_RESET_IO_DRAIN_RECOMMENDED_VALUE (5초) */

	uint8_t reserved7[2];         /* [한국어] 예약 */

	struct {
		/** Is numa.id valid? Needed to know whether numa.id == 0 was
		 *  explicitly set by bdev module or implicitly set when
		 *  calloc()'ing the structure.
		 */
		uint32_t id_valid : 1;
                                  /* [한국어] numa.id가 명시적으로 설정되었는지 (0이 유효 노드일 수 있어 별도 플래그 필요) */
		/** NUMA node ID for the bdev */
		int32_t id : 31;
                                  /* [한국어] bdev가 속한 NUMA 노드 — 채널/qpair 배치 최적화에 사용 */
	} numa;

	/** Bitmap of supported io types */
	uint32_t accel_sequence_supported;
                                  /* [한국어] accel 시퀀스로 처리 가능한 I/O 타입 비트맵 — bdev 코어가 DMA 엔진 offload 경로 선택 */

	/**
	 * Pointer to the bdev module that registered this bdev.
	 */
	struct spdk_bdev_module *module;
                                  /* [한국어] 이 bdev를 등록한 모듈 포인터 (back-reference) */

	/** function table for all LUN ops */
	const struct spdk_bdev_fn_table *fn_table;
                                  /* [한국어] 모듈이 제공하는 콜백 vtable — I/O 제출/채널/통계/JSON 등 */

	/** Fields that are used internally by the bdev subsystem.  Bdev modules
	 *  must not read or write to these fields.
	 */
	/*
	 * [한국어] ★ bdev 코어 전용 내부 필드 — 모듈 접근 금지 ★
	 *
	 * spinlock 순서 규칙 (데드락 방지 — 반드시 이 순서로만 다중 획득):
	 *   g_bdev_mgr.spinlock → bdev->internal.spinlock → bdev_desc->spinlock → bdev_module->internal.spinlock
	 */
	struct __bdev_internal_fields {
		/** Quality of service parameters */
		struct spdk_bdev_qos *qos;
                                  /* [한국어] QoS(Quality of Service) 설정 포인터 — IOPS/대역폭 throttling
                                   *  - NULL이면 QoS 미적용 */

		/** True if the state of the QoS is being modified */
		bool qos_mod_in_progress;
                                  /* [한국어] QoS 변경 진행 중 플래그 — 중첩 수정 방지 */

		/** Trace ID for this bdev. */
		uint16_t trace_id;
                                  /* [한국어] SPDK trace 프레임워크에 할당된 bdev ID — 트레이스 이벤트 태그 */

		/**
		 * SPDK spinlock protecting many of the internal fields of this structure. If
		 * multiple locks need to be held, the following order must be used:
		 *   g_bdev_mgr.spinlock
		 *   bdev->internal.spinlock
		 *   bdev_desc->spinlock
		 *   bdev_module->internal.spinlock
		 */
		struct spdk_spinlock spinlock;
                                  /* [한국어] internal 필드 보호 spinlock — claim/examine/open_descs/reset 상태 수정 시 필수
                                   *  - 상단 순서 규칙 엄수 */

		/** The bdev status */
		enum spdk_bdev_status status;
                                  /* [한국어] bdev 상태 (READY/REMOVING/UNREGISTERED 등) */

		/**
		 * How many bdev_examine() calls are iterating claim.v2.claims. When non-zero claims
		 * that are released will be cleared but remain on the claims list until
		 * bdev_examine() finishes. Must hold spinlock on all updates.
		 */
		uint32_t examine_in_progress;
                                  /* [한국어] examine 진행 카운터 — 순회 중 claim 제거가 발생해도 리스트에서 즉시 빼지 않고 순회 완료까지 유지
                                   *  - spinlock 필수 */

		/**
		 * The claim type: used in conjunction with claim. Must hold spinlock on all
		 * updates.
		 */
		enum spdk_bdev_claim_type claim_type;
                                  /* [한국어] 현재 bdev에 걸린 claim 타입 (NONE/EXCL_WRITE/READ_MANY_WRITE_ONE/…/SHARED) */

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
                                  /* [한국어] v1 claim API 경로 — 단일 모듈 포인터만 저장 */
			} v1;
			/** Claims acquired with spdk_bdev_module_claim_bdev_desc() */
			struct __bdev_internal_claim_v2 {
				/** The claims on this bdev */
				TAILQ_HEAD(v2_claims, spdk_bdev_module_claim) claims;
                                  /* [한국어] v2 API의 다중 claim 리스트 (SHARED writer 등) */
				/** See spdk_bdev_claim_opts.shared_claim_key */
				uint64_t key;
                                  /* [한국어] SHARED claim 키 — 동일 키를 가진 claimer만 추가 가능 */
			} v2;
		} claim;
                                  /* [한국어] claim 정보 union (claim_type에 따라 v1/v2 선택) */

		/** Callback function that will be called after bdev destruct is completed. */
		spdk_bdev_unregister_cb	unregister_cb;
                                  /* [한국어] unregister 완료 콜백 */

		/** Unregister call context */
		void *unregister_ctx;
                                  /* [한국어] unregister_cb에 전달할 컨텍스트 */

		/** Thread that issued the unregister.  The cb must be called on this thread. */
		struct spdk_thread *unregister_td;
                                  /* [한국어] unregister를 호출한 스레드 — cb를 이 스레드에서 반드시 호출 (메시지 전달) */

		/** List of open descriptors for this block device. */
		TAILQ_HEAD(, spdk_bdev_desc) open_descs;
                                  /* [한국어] 이 bdev에 대해 현재 open된 모든 descriptor 리스트
                                   *  - unregister 시 각 desc에 remove 이벤트 통지하여 close 유도 */

		TAILQ_ENTRY(spdk_bdev) link;
                                  /* [한국어] 전역 bdev 리스트 링크 */

		/** points to a reset bdev_io if one is in progress. */
		struct spdk_bdev_io *reset_in_progress;
                                  /* [한국어] 진행 중인 reset bdev_io (중복 reset 방지 — 하나만 동시 진행) */

		/** List of reset bdev_ios that are not submitted to the underlying device. */
		bdev_io_tailq_t		queued_resets;
                                  /* [한국어] 장치로 아직 보내지 않고 대기 중인 reset 요청 큐 */

		/** poller for tracking the queue_depth of a device, NULL if not tracking */
		struct spdk_poller *qd_poller;
                                  /* [한국어] 큐 깊이 측정 poller — enable_histogram 경로에서 활성 */

		/** open descriptor to use qd_poller safely */
		struct spdk_bdev_desc *qd_desc;
                                  /* [한국어] qd_poller가 bdev을 안전하게 참조하기 위한 전용 descriptor */

		/** period at which we poll for queue depth information */
		uint64_t period;
                                  /* [한국어] QD 폴링 주기 (틱) */

		/** new period to be used to poll for queue depth information */
		uint64_t new_period;
                                  /* [한국어] 다음 주기 (런타임 변경 반영용) */

		/** used to aggregate queue depth while iterating across the bdev's open channels */
		uint64_t temporary_queue_depth;
                                  /* [한국어] 모든 채널의 QD를 집계할 때 쓰는 임시 누적값 */

		/** queue depth as calculated the last time the telemetry poller checked. */
		uint64_t measured_queue_depth;
                                  /* [한국어] 가장 최근 측정된 QD — 사용자 쿼리 응답 */

		/** most recent value of ticks spent performing I/O. Used to calculate the weighted time doing I/O */
		uint64_t io_time;
                                  /* [한국어] 최근 관찰된 I/O 실행 누적 틱 — weighted_io_time 계산 입력 */

		/** weighted time performing I/O. Equal to measured_queue_depth * period */
		uint64_t weighted_io_time;
                                  /* [한국어] 가중 I/O 시간 = QD × period. 장치 활용도 지표 */

		/** accumulated I/O statistics for previously deleted channels of this bdev */
		struct spdk_bdev_io_stat *stat;
                                  /* [한국어] 삭제된 채널들의 누적 I/O 통계 — bdev 단위 총합에 포함 */

		/** true if tracking the queue_depth of a device is in progress */
		bool	qd_poll_in_progress;
                                  /* [한국어] QD 수집 이터레이션 진행 중 (재진입 방지) */

		/** histogram enabled on this bdev */
		bool	 histogram_enabled;
                                  /* [한국어] 지연시간 히스토그램 활성 여부 */
		bool	 histogram_in_progress;
                                  /* [한국어] 히스토그램 수집 진행 중 */
		uint8_t	 histogram_io_type;
                                  /* [한국어] 히스토그램 대상 I/O 타입 (READ/WRITE/ALL 등) */
		uint8_t	 histogram_granularity;
                                  /* [한국어] 히스토그램 버킷 세분성 (지수 단위) */
		uint64_t histogram_min_val;
                                  /* [한국어] 히스토그램 최소 관찰값 (nsec) */
		uint64_t histogram_max_val;
                                  /* [한국어] 히스토그램 최대 관찰값 */

		/** Currently locked ranges for this bdev.  Used to populate new channels. */
		lba_range_tailq_t locked_ranges;
                                  /* [한국어] 현재 잠겨 있는 LBA 범위 목록 (read/write serialization)
                                   *  - 새 채널 생성 시 상태 전파 */

		/** Pending locked ranges for this bdev.  These ranges are not currently
		 *  locked due to overlapping with another locked range.
		 */
		lba_range_tailq_t pending_locked_ranges;
                                  /* [한국어] 잠금 요청됐지만 기존 잠금과 겹쳐 대기 중인 범위 */

		/** Bdev name used for quick lookup */
		struct spdk_bdev_name bdev_name;
                                  /* [한국어] 전역 이름 트리 엔트리 (RB) — bdev_names에 연결됨 */
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
