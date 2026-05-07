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
 * [한국어 설명] bdev 모듈 작성자용 인터페이스 (bdev_module.h)
 *
 * === 파일의 역할 ===
 * bdev(SPDK Block Device 추상화) 서브시스템의 **"모듈 측(백엔드 측)"** 인터페이스를
 * 정의한다. 공개 헤더 `spdk/bdev.h`는 "사용자(애플리케이션) 측" API
 * (spdk_bdev_open_ext / read / write / unmap / flush 등)를 제공하는 반면,
 * 이 헤더는 "NVMe/AIO/malloc/raid/lvol/crypto 등 실제 백엔드를 구현하는 bdev 모듈"이
 * 필요로 하는 타입·콜백·등록 API·I/O 요청 객체 구조를 노출한다. 즉,
 * "사용자 → bdev 코어"는 bdev.h, "bdev 코어 → 모듈"은 이 헤더가 경계가 된다.
 *
 * 주요 내용:
 *   (1) struct spdk_bdev_module — 모듈 생애주기 훅 (module_init/fini,
 *       examine_config/disk, config_json, get_ctx_size, async_init flag 등)
 *   (2) struct spdk_bdev_fn_table — bdev 인스턴스별 모듈 콜백 vtable
 *       (★ submit_request 가 I/O 제출 진입점)
 *   (3) enum spdk_bdev_io_status / io_type — I/O 결과·종류 분류
 *   (4) struct spdk_bdev — bdev 인스턴스 메타데이터 (블록 크기, 지원 기능,
 *       UUID, NUMA, ZNS, DIF, QoS, 통계 등)
 *   (5) struct spdk_bdev_io_*_params — I/O 요청 종류별 페이로드
 *       · block_params (read/write/unmap/copy/zcopy 등)
 *       · reset_params, abort_params, nvme_passthru_params, zone_mgmt_params
 *   (6) ★ struct spdk_bdev_io ★ — 단일 bdev I/O 요청 객체 (bdev 레이어의
 *       핵심 데이터, mempool 풀에서 할당)
 *   (7) Claim API — vbdev stacking을 위한 점유 관리 (claim_v1, claim_v2)
 *   (8) Examine 메커니즘 — 새 bdev 등장 시 vbdev 모듈이 검사·점유
 *   (9) I/O 상태 설정 헬퍼 (set_nvme_status / set_scsi_status / set_aio_status,
 *       complete 변형들)
 *   (10) 등록/해제 API (spdk_bdev_register, unregister, alias 관리)
 *   (11) Buffer 헬퍼 (io_get_buf / io_set_buf / io_set_md_buf — bounce buf)
 *   (12) struct spdk_bdev_part / part_base — 파티션 vbdev 보조 API
 *   (13) Quiesce/unquiesce — 모듈 작성자가 LBA 범위 일시 정지
 *   (14) SPDK_BDEV_MODULE_REGISTER — 정적 컨스트럭터 매크로
 *
 * === 전체 아키텍처에서의 위치 ===
 * 애플리케이션 I/O 경로:
 *   app → spdk_bdev_read() [bdev.h]
 *     → bdev 레이어: bdev_channel_get_io → bdev_io_init → bdev_io_submit
 *       [lib/bdev/bdev.c, bdev_internal.h]
 *       → 모듈의 spdk_bdev_fn_table.submit_request(ch, bdev_io) [이 헤더]
 *         예) module/bdev/nvme/bdev_nvme.c 의 콜백이 bdev_io 를 분석해
 *             spdk_nvme_ns_cmd_read/write 로 변환 → NVMe 드라이버로 dispatch
 *             → MMIO doorbell write → SQE submission
 *
 * 완료 경로:
 *   모듈이 백엔드 완료 처리 후 spdk_bdev_io_complete(bdev_io, status)
 *     → bdev 레이어 (lib/bdev/bdev.c) 가 상위 사용자 콜백 invoke
 *     → bdev_io를 mempool에 반납
 *
 * 모듈 등록 경로 (부트스트랩):
 *   bdev 모듈 .c 파일에 SPDK_BDEV_MODULE_REGISTER(name, &mod_var) 매크로
 *     → __attribute__((constructor)) 가 main() 전에 spdk_bdev_module_list_add
 *     → app 시작 후 bdev 코어가 모듈 리스트 순회: module_init → examine_config
 *       → examine_disk → init_complete
 *
 * Examine + Claim 의 vbdev stacking 모델:
 *   - 실제(physical) bdev 모듈(NVMe, aio, malloc)이 spdk_bdev_register 로
 *     bdev 등록 → bdev 코어가 모든 vbdev 모듈에 examine_config/disk 통지
 *   - vbdev 모듈(lvol, raid, crypto, passthru)은 examine 콜백 안에서
 *     spdk_bdev_module_claim_bdev (v1) 또는 spdk_bdev_module_claim_bdev_desc (v2) 로
 *     하위 bdev 점유 → 새 (vbdev) bdev_register
 *   - 결과: bdev 트리 형태의 stacking (예: NVMe → lvol_store → lvol_blob → crypto_bdev)
 *
 * 실행 컨텍스트: 모든 bdev I/O는 I/O channel(=spdk_thread)에 고정.
 * 한 bdev_io는 생성한 채널(즉 스레드)에서만 submit/complete가 가능하다.
 * 등록/해제·examine 같은 관리 API는 SPDK app thread (g_main_thread)에서 호출.
 *
 * === 타 모듈과의 연결 ===
 * 의존(이 헤더가 #include 하는):
 *   - spdk/bdev.h (공개 I/O API 타입: spdk_bdev_io_completion_cb 등)
 *   - spdk/bdev_zone.h (ZNS 스펙 상수)
 *   - spdk/log.h, queue.h, thread.h, tree.h, util.h, uuid.h
 *   - spdk/scsi_spec.h (SCSI 센스 키 — set_scsi_status API)
 * 이 헤더에 의존(이 헤더를 #include 하는):
 *   - lib/bdev/bdev.c (bdev 코어) — 이 헤더의 타입을 구현
 *   - module/bdev/* (nvme, aio, malloc, raid, lvol, crypto, delay, error, …)
 *   - lib/nvmf/ctrlr_bdev.c (NVMe-oF target — bdev로 I/O dispatch 시 모듈 측 API 사용)
 * 공유 자료구조: spdk_bdev, spdk_bdev_io, spdk_bdev_module — bdev 코어와
 *   모듈이 공동 소유. internal 필드는 코어 전용, 모듈 접근 금지.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_bdev_module:                  모듈 등록 루트 구조체
 *   - spdk_bdev_fn_table:                I/O 제출·채널·상태 조회 vtable
 *   - spdk_bdev:                         bdev 인스턴스 메타데이터
 *   - spdk_bdev_io:                      단일 I/O 요청 (★ I/O 경로 중앙 객체 ★)
 *   - spdk_bdev_io_type:                 READ/WRITE/UNMAP/FLUSH/RESET/PASSTHRU/COPY/...
 *   - spdk_bdev_io_status:               SUCCESS/FAILED/NVME_ERROR/SCSI_ERROR/NOMEM/...
 *   - spdk_bdev_register / unregister:   bdev 등록/해제
 *   - spdk_bdev_io_complete:             I/O 완료 보고 (모듈 → 코어)
 *   - spdk_bdev_io_get_buf:              bounce buffer 자동 할당
 *   - spdk_bdev_module_claim_bdev_desc:  vbdev stacking 점유 (v2 API)
 *   - spdk_bdev_module_examine_done:     examine 콜백 완료 통지
 *   - spdk_bdev_part_*:                  파티션 vbdev 보조 (offset 변환, hotremove)
 *   - SPDK_BDEV_MODULE_REGISTER:         모듈 정적 등록 매크로 (constructor)
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
/*
 * [한국어] enum spdk_bdev_claim_type — bdev 점유(claim) 타입
 *
 * 점유는 "이 bdev의 I/O 제어 권한을 어떤 모듈/desc가 가지는가"를 선언하는
 * 메커니즘이다. vbdev stacking(예: lvol → 하위 NVMe bdev)에서 상위 모듈이
 * 하위 bdev을 독점하기 위해, 또는 다중 writer가 협력적으로 공유하기 위해 사용.
 *
 * v1 API(`spdk_bdev_module_claim_bdev`)는 EXCL_WRITE 의미만 지원했고, v2 API
 * (`spdk_bdev_module_claim_bdev_desc`)가 아래 4가지 타입을 모두 지원한다.
 */
enum spdk_bdev_claim_type {
	/* Not claimed. Must not be used to request a claim. */
	SPDK_BDEV_CLAIM_NONE = 0,
	                              /* [한국어] 점유 없음 — claim 상태 조회 결과로만 사용 (요청 시 무효) */

	/**
	 * Exclusive writer, with allowances for legacy behavior.  This matches the behavior of
	 * `spdk_bdev_module_claim_bdev()` as of SPDK 22.09.  New consumer should use
	 * SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE instead.
	 */
	SPDK_BDEV_CLAIM_EXCL_WRITE,
	                              /* [한국어] 단일 writer 독점 — 레거시 v1 호환용. 신규 코드는 READ_MANY_WRITE_ONE 권장 */

	/**
	 * The descriptor passed with this claim request is the only writer. Other claimless readers
	 * are allowed.
	 */
	SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE,
	                              /* [한국어] 단일 writer + 다수 reader — claim 등록한 desc만 쓰기 가능, claim 없는 reader는 read-only로 open 허용 */

	/**
	 * Any number of readers, no writers. Readers without a claim are allowed.
	 */
	SPDK_BDEV_CLAIM_READ_MANY_WRITE_NONE,
	                              /* [한국어] 모든 reader 허용, writer 금지 — 스냅샷 같은 read-only 보호 시 */

	/**
	 * Any number of writers with matching shared_claim_key. After the first writer establishes
	 * a claim, future aspiring writers should open read-only and pass the read-only descriptor.
	 * If the shared claim is granted to the aspiring writer, the descriptor will be upgraded to
	 * read-write.
	 */
	SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED
	                              /* [한국어] 동일 key를 가진 다수 writer 허용 — 협력적 분산 모듈
	                               *  - 첫 writer가 claim 등록 후, 이후 후보는 read-only로 open + 같은 key 제시
	                               *  - 키가 일치하면 desc가 R/W로 upgrade됨 */
};

/** Options used when requesting a claim. */
/*
 * [한국어] struct spdk_bdev_claim_opts — claim 요청 옵션
 *
 * spdk_bdev_module_claim_bdev_desc 호출 시 전달. opts_size 는 SPDK ABI 호환을
 * 위한 versioning 패턴(spdk_bdev_claim_opts_init이 sizeof 자동 기록).
 */
struct spdk_bdev_claim_opts {
	/* Size of this structure in bytes */
	size_t opts_size;             /* [한국어] 구조체 크기 (ABI versioning) — spdk_bdev_claim_opts_init이 sizeof로 채움 */
	/**
	 * An arbitrary name for the claim. If set, it should be a string suitable for printing in
	 * error messages. Must be '\0' terminated.
	 */
	char name[SPDK_BDEV_CLAIM_NAME_LEN];
	                              /* [한국어] claim 식별 이름 (진단·로그용) — 충돌 발생 시 누가 claim 했는지 표시 */
	/**
	 * Used with SPDK_BDEV_CLAIM_READ_MANY_WRITE_SHARED claims. Any non-zero value is considered
	 * a key.
	 */
	uint64_t shared_claim_key;    /* [한국어] SHARED claim 키 — 같은 키를 가진 모듈만 추가 writer로 합류 가능. 0은 무효 */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_claim_opts) == 48, "Incorrect size");
                              /* [한국어] ABI 크기 검증 — 필드 추가 시 build 시점에 실패 */

/**
 * Retrieve the name of the bdev module claim type.
 * See function definition for mapping claims types to name.
 *
 * \param claim_type The claim type.
 * \return A string that describes the claim type.
 */
/*
 * [한국어]
 * spdk_bdev_claim_get_name - claim 타입의 사람이 읽을 수 있는 문자열 반환
 *
 * @claim_type: enum spdk_bdev_claim_type 중 하나.
 * @return: "none" / "exclusive_write" / "read_many_write_one" 등의 문자열 (정적 저장).
 *
 * 진단 로그·RPC JSON 출력에서 claim 타입을 표시할 때 사용. 함수 본문은
 * lib/bdev/bdev.c 에서 enum 값별로 const char* 매핑.
 */
const char *spdk_bdev_claim_get_name(enum spdk_bdev_claim_type claim_type);

/**
 * Initialize bdev module claim options structure.
 *
 * \param opts The structure to initialize.
 * \param size The size of *opts.
 */
/*
 * [한국어]
 * spdk_bdev_claim_opts_init - claim_opts 구조체 기본값 초기화
 *
 * @opts: 초기화할 구조체 포인터 (caller 소유).
 * @size: opts 구조체 크기 (sizeof) — ABI versioning에 사용.
 *
 * 모든 필드를 0/기본값으로 채우고 opts_size 필드에 size를 기록. 호출자는
 * 이후 필요한 필드만 덮어써서 spdk_bdev_module_claim_bdev_desc로 전달.
 * SPDK_BDEV_CLAIM_OPTS_INIT 패턴이 다른 구조체에서도 반복됨.
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
/*
 * [한국어]
 * spdk_bdev_module_claim_bdev_desc - bdev 점유 v2 API (descriptor 기반)
 *
 * @desc:   open된 bdev_desc — claim 종료(release)는 desc close 시 자동 수행.
 *          타입에 따라 read-only desc가 R/W로 자동 upgrade될 수 있음.
 * @type:   요청 claim 타입 (EXCL_WRITE / READ_MANY_WRITE_ONE / _NONE / _SHARED).
 * @opts:   NULL 또는 spdk_bdev_claim_opts. SHARED claim은 shared_claim_key 필수.
 * @module: claim을 거는 bdev 모듈 포인터 (충돌 진단용).
 * @return: 0 성공 / -ENOMEM 메모리 부족 / -EBUSY 충돌 / -EINVAL 인자 오류.
 *
 * 사용 사례: vbdev 모듈(lvol, raid, crypto)이 examine_config/disk 콜백에서
 * 하위 bdev을 점유한 뒤 spdk_bdev_register로 자신의 vbdev을 등록.
 * 호출 시 g_bdev_mgr.spinlock과 bdev->internal.spinlock을 들어 atomic하게
 * claim_type/claim 상태 갱신 (락 순서: 상단 spinlock 규칙 참조).
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
/*
 * [한국어]
 * spdk_bdev_module_claim_bdev - 레거시 v1 claim API (단순 독점)
 *
 * @bdev:   점유 대상 bdev.
 * @desc:   대응 desc (NULL 가능) — non-NULL이면 R/W로 upgrade.
 * @module: claim 모듈 포인터.
 * @return: 0 성공 / -EPERM 이미 다른 모듈이 점유 중.
 *
 * SPDK 22.09 이전 동작과 호환을 위해 유지. 내부적으로 EXCL_WRITE 타입과 동등.
 * 신규 모듈은 spdk_bdev_module_claim_bdev_desc 사용 권장.
 */
int spdk_bdev_module_claim_bdev(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				struct spdk_bdev_module *module);

/**
 * Called to release a write claim on a block device.
 *
 * \param bdev Block device to be released.
 */
/*
 * [한국어]
 * spdk_bdev_module_release_bdev - v1 claim 해제
 *
 * @bdev: claim을 해제할 bdev.
 *
 * v1 API(claim_bdev)로 건 점유를 해제. v2 claim은 desc close 시 자동 release.
 * vbdev 모듈이 자신의 vbdev을 unregister 직전에 호출하여 하위 bdev을 풀어줌.
 */
void spdk_bdev_module_release_bdev(struct spdk_bdev *bdev);

/* Libraries may define __SPDK_BDEV_MODULE_ONLY so that they include
 * only the struct spdk_bdev_module definition, and the relevant APIs
 * to claim/release a bdev. This may be useful in some cases to avoid
 * abidiff errors related to including the struct spdk_bdev structure
 * unnecessarily.
 */
#ifndef __SPDK_BDEV_MODULE_ONLY

/*
 * [한국어]
 * spdk_bdev_unregister_cb - bdev 비등록 완료 콜백 typedef
 *
 * @cb_arg: 호출자가 unregister 호출 시 전달한 임의 컨텍스트.
 * @rc:     0 성공 / 음수 errno (예: -ENODEV bdev 미존재).
 *
 * spdk_bdev_unregister[_by_name] 호출자가 등록하는 완료 콜백. unregister는
 * 모든 open desc가 close될 때까지 지연 가능 — 콜백은 unregister를 발행한
 * spdk_thread에서 호출됨(메시지 전달).
 */
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
/*
 * [한국어]
 * spdk_bdev_io_get_buf_cb - 버퍼 할당 완료 콜백 typedef
 *
 * @ch:      해당 I/O가 처리되는 채널.
 * @bdev_io: 버퍼를 요청한 bdev_io.
 * @success: true = 할당 성공 또는 SGL 이미 보유 / false = 실패 (요청 크기가 max 초과 등).
 *
 * spdk_bdev_io_get_buf 호출 시 등록. 사용자 버퍼가 정렬 요건 위반이거나 미제공일
 * 때 bdev 코어가 bounce buffer를 자동 할당하고 이 콜백으로 모듈에 통지. 메모리
 * 부족이면 콜백이 deferred 되어 다른 I/O 완료로 메모리 해방 시 재시도됨.
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
/*
 * [한국어]
 * spdk_bdev_io_get_aux_buf_cb - 보조 버퍼 할당 완료 콜백 typedef
 *
 * @ch:      I/O 채널.
 * @bdev_io: 보조 버퍼를 요청한 bdev_io.
 * @aux_buf: 할당된 버퍼 포인터 (NULL이면 실패).
 *
 * 일부 vbdev(예: crypto)이 원본 데이터와 별개의 작업 버퍼가 필요할 때 사용.
 * 데이터 버퍼와 분리되어 자유롭게 사용 가능 (모듈이 명시적으로 free 책임).
 */
typedef void (*spdk_bdev_io_get_aux_buf_cb)(struct spdk_io_channel *ch,
		struct spdk_bdev_io *bdev_io, void *aux_buf);

/* Maximum number of IOVs used for I/O splitting */
#define SPDK_BDEV_IO_NUM_CHILD_IOV 32
                              /* [한국어] split 시 자식 I/O 하나가 가질 수 있는 최대 iovec 개수
                               *  - bdev 코어가 large I/O를 NVMe MDTS 등 한계에 맞춰 split할 때
                               *    child_iov[] 배열의 정적 크기로 사용
                               *  - 32개로 통상 NVMe MDTS(예: 128KB)에서 4KB iovec 32개 충분 */

/*
 * [한국어] struct spdk_bdev_io_block_params - 일반 블록 I/O 파라미터
 *
 * type이 READ/WRITE/UNMAP/FLUSH/COMPARE/COMPARE_AND_WRITE/COPY/ZONE_APPEND/
 * WRITE_ZEROES/SEEK_DATA/SEEK_HOLE/ZCOPY일 때 spdk_bdev_io.u.bdev로 사용.
 *
 * 사용자 API(spdk_bdev_read 등)가 채워서 모듈로 전달. 모듈은 type을 보고
 * 어느 보조 필드(zcopy/abort/seek/copy)를 해석할지 결정.
 */
struct spdk_bdev_io_block_params {
	/** For SG buffer cases, array of iovecs to transfer. */
	struct iovec *iovs;
	                              /* [한국어] 사용자 데이터 iovec 배열 — Scatter-Gather DMA 입력
	                               *  - 설정자: spdk_bdev_*v 계열 사용자 API
	                               *  - 읽는 자: 모듈 submit_request → NVMe SGL/PRP 변환 */

	/** For SG buffer cases, number of iovecs in iovec array. */
	int iovcnt;                   /* [한국어] iovs 배열 원소 개수 */

	/** Total size of data to be transferred. */
	uint64_t num_blocks;          /* [한국어] 전송할 총 블록 수 (논리 블록) — bdev->blocklen × num_blocks = 바이트 */

	/** Starting offset (in blocks) of the bdev for this I/O. */
	uint64_t offset_blocks;       /* [한국어] bdev 시작점부터의 시작 LBA — 모듈이 NVMe SLBA로 그대로 사용 */

	/** Memory domain and its context to be used by bdev modules */
	struct spdk_memory_domain *memory_domain;
	                              /* [한국어] 사용자 버퍼가 속한 메모리 도메인 (RDMA / GPU 등) — DMA 직접 가능 여부 결정 */
	void *memory_domain_ctx;      /* [한국어] memory_domain 핸들러에 전달할 컨텍스트 (RDMA의 경우 PD/QP 정보 등) */

	/* Sequence of accel operations */
	struct spdk_accel_sequence *accel_sequence;
	                              /* [한국어] DMA 엔진/암호화 가속기로 처리할 연산 시퀀스 — 데이터 변환을 백엔드 IO와 일괄 처리 */

	/* Metadata buffer */
	void *md_buf;                 /* [한국어] PI/메타데이터 버퍼 (separate metadata layout) — bdev->md_interleave=false일 때 사용 */

	/** For fused operations such as COMPARE_AND_WRITE, array of iovecs
	 *  for the second operation.
	 */
	struct iovec *fused_iovs;     /* [한국어] fused 명령(NVMe Compare+Write)의 두 번째 연산용 iovec — 첫 명령은 iovs, 둘째는 fused_iovs */

	/** Number of iovecs in fused_iovs. */
	int fused_iovcnt;             /* [한국어] fused_iovs 원소 수 */

	/** Specify whether each DIF check type is enabled. */
	uint32_t dif_check_flags;     /* [한국어] DIF 검사 활성화 플래그 (GUARD/APPTAG/REFTAG) — 호스트 검증 on/off 비트마스크 */

	/** defined by \ref spdk_bdev_nvme_cdw12 */
	union spdk_bdev_nvme_cdw12 nvme_cdw12;
	                              /* [한국어] NVMe R/W 명령 CDW12 (DIF 옵션, FUA, LR 등) — 호스트 직접 제어용 */

	/** defined by \ref spdk_bdev_nvme_cdw13 */
	union spdk_bdev_nvme_cdw13 nvme_cdw13;
	                              /* [한국어] NVMe R/W 명령 CDW13 (Dataset Management 힌트 등) */

	/*
	 * [한국어] zcopy(zero-copy) 보조 — type==SPDK_BDEV_IO_TYPE_ZCOPY일 때 의미
	 *  - 시작(start) 단계: 모듈이 백엔드 메모리에 직접 매핑된 iovec을 노출
	 *  - 끝(end) 단계: 사용자 작업 종료 후 commit/discard
	 */
	struct {
		/** Whether the buffer should be populated with the real data */
		uint8_t populate : 1;
	                              /* [한국어] zcopy start: 백엔드의 실제 데이터를 채워 노출할지 여부 (read-modify-write 시 true) */

		/** Whether the buffer should be committed back to disk */
		uint8_t commit : 1;
	                              /* [한국어] zcopy end: 사용자 수정 내용을 디스크에 commit할지 여부 (false면 폐기) */

		/** True if this request is in the 'start' phase of zcopy. False if in 'end'. */
		uint8_t start : 1;
	                              /* [한국어] zcopy 단계 식별 — true=start (mapping 획득), false=end (mapping 반환) */
	} zcopy;

	/*
	 * [한국어] abort 보조 — type==SPDK_BDEV_IO_TYPE_ABORT일 때 의미
	 */
	struct {
		/** The callback argument for the outstanding request which this abort
		 *  attempts to cancel.
		 */
		void *bio_cb_arg;     /* [한국어] 취소 대상 I/O의 사용자 콜백 인자 — 모듈이 이 값으로 in-flight 매칭 */
	} abort;

	/*
	 * [한국어] seek 보조 — type==SEEK_DATA/SEEK_HOLE일 때 의미 (sparse 파일 빈공간 탐색)
	 */
	struct {
		/** The offset of next data/hole.  */
		uint64_t offset;      /* [한국어] 모듈이 발견한 다음 data/hole의 LBA offset (응답에 채움) */
	} seek;

	/*
	 * [한국어] copy 보조 — type==SPDK_BDEV_IO_TYPE_COPY일 때 의미 (NVMe Simple Copy)
	 */
	struct {
		/** Starting source offset (in blocks) of the bdev for copy I/O. */
		uint64_t src_offset_blocks;
	                              /* [한국어] 복사 원본 LBA — 대상 LBA는 offset_blocks 사용. 같은 bdev 내 LBA-LBA 복사 */
	} copy;

	/** DIF context */
	struct spdk_dif_ctx dif_ctx;  /* [한국어] DIF 컨텍스트 — APPTAG/REFTAG 시드, 블록 크기, PI 포맷 등 */

	/** DIF error information */
	struct spdk_dif_error dif_err;/* [한국어] DIF 검증 실패 시 상세 에러 (잘못된 GUARD/APPTAG/REFTAG의 LBA 등) */
};

/*
 * [한국어] struct spdk_bdev_io_reset_params - RESET 명령 파라미터
 *
 * type==SPDK_BDEV_IO_TYPE_RESET 일 때 사용. RESET은 모든 채널의 in-flight I/O를
 * drain/cancel하는 무거운 작업이므로 channel reference와 timed poller를 동반.
 */
struct spdk_bdev_io_reset_params {
	/** Channel reference held while messages for this reset are in progress. */
	struct spdk_io_channel *ch_ref;
	                              /* [한국어] reset 처리 중 채널이 사라지지 않도록 보유한 채널 참조 — 모든 send_msg 완료 후 put */
	struct {
		/* Handle to timed poller that checks each channel for outstanding IO. */
		struct spdk_poller *poller;
	                              /* [한국어] in-flight I/O drain 대기용 timed poller — reset_io_drain_timeout 동안 실행 */
		/* Store calculated time value, when a poller should stop its work. */
		uint64_t  stop_time_tsc;
	                              /* [한국어] poller 종료 절대 tsc 시각 — spdk_get_ticks() 비교로 timeout 판정 */
	} wait_poller;
};

/*
 * [한국어] struct spdk_bdev_io_abort_params - ABORT 명령 파라미터
 *
 * type==SPDK_BDEV_IO_TYPE_ABORT 일 때 사용. 특정 in-flight bdev_io를 지정해 취소.
 */
struct spdk_bdev_io_abort_params {
	/** The outstanding request matching bio_cb_arg which this abort attempts to cancel. */
	struct spdk_bdev_io *bio_to_abort;
	                              /* [한국어] 취소 대상 bdev_io 직접 포인터 — 코어가 cb_arg 매칭 후 채움 */
};

/*
 * [한국어] struct spdk_bdev_io_nvme_passthru_params - NVMe passthrough 파라미터
 *
 * type==NVME_ADMIN/NVME_IO/NVME_IO_MD 일 때 사용. raw NVMe SQE를 그대로 전달하는
 * 백도어 — bdev 모듈이 spdk_nvme_ns_cmd_io_raw* 등으로 forward.
 */
struct spdk_bdev_io_nvme_passthru_params {
	/* The NVMe command to execute */
	struct spdk_nvme_cmd cmd;     /* [한국어] 사용자가 직접 채운 NVMe Submission Queue Entry (64B) — opcode/cdw* 모두 포함 */

	/* For SG buffer cases, array of iovecs to transfer. */
	struct iovec *iovs;           /* [한국어] passthrough 데이터 iovec 배열 (vectored 변형용) */

	/* For SG buffer cases, number of iovecs in iovec array. */
	int iovcnt;                   /* [한국어] iovs 원소 수 */

	/* The data buffer to transfer */
	void *buf;                    /* [한국어] 단일 buffer 변형 — iovs/iovcnt 미사용 시 */

	/* The number of bytes to transfer */
	size_t nbytes;                /* [한국어] buf의 바이트 수 */

	/* The meta data buffer to transfer */
	void *md_buf;                 /* [한국어] 메타데이터 별도 버퍼 (NVME_IO_MD에서 사용) */

	/* meta data buffer size to transfer */
	size_t md_len;                /* [한국어] md_buf 크기 */
};

/*
 * [한국어] struct spdk_bdev_io_zone_mgmt_params - ZNS Zone Management 파라미터
 *
 * type==ZONE_MANAGEMENT 일 때 사용 (Zoned Namespace 명령).
 */
struct spdk_bdev_io_zone_mgmt_params {
	/* First logical block of a zone */
	uint64_t zone_id;             /* [한국어] zone의 시작 LBA (Zone Slba) — zone_size로 정렬 */

	/* Number of zones */
	uint32_t num_zones;           /* [한국어] 대상 zone 개수 — REPORT ZONES 등에서 사용 */

	/* Used to change zoned device zone state */
	enum spdk_bdev_zone_action zone_action;
	                              /* [한국어] zone 상태 변경 명령 (OPEN/CLOSE/FINISH/RESET/OFFLINE) — NVMe ZSA 필드와 1:1 */

	/* The data buffer */
	void *buf;                    /* [한국어] REPORT ZONES 응답 버퍼 — zone descriptor 배열 수신 */
};

/**
 *  Fields that are used internally by the bdev subsystem.  Bdev modules
 *  must not read or write to these fields.
 */
/*
 * [한국어] ★ struct spdk_bdev_io_internal_fields - bdev 코어 전용 내부 필드 ★
 *
 * spdk_bdev_io 내부에 임베드되는 코어 전용 영역. 모듈이 직접 접근하면 ABI/내부
 * 인변량이 깨질 수 있어 **읽기·쓰기 모두 금지**. 상태 변경은 항상 코어 API
 * (spdk_bdev_io_complete, set_*_status 등)를 통해서만.
 *
 * 주요 역할:
 *   - 채널/desc 포인터, 사용자 콜백 보관
 *   - 분할(split), bounce buffer, accel sequence 진행 상태
 *   - NOMEM/QoS/accel/memory-domain pull/push 대기 큐 링크
 *   - NVMe/SCSI/AIO 에러 정보
 */
struct spdk_bdev_io_internal_fields {
	/** The bdev I/O channel that this was handled on. */
	struct spdk_bdev_channel *ch; /* [한국어] 이 I/O가 처리되는 bdev 채널 — 스레드 친화도 식별 (해당 spdk_thread에서만 다룸) */

	/*
	 * [한국어] 비트필드 플래그 묶음 — bdev_io의 동적 상태 8비트
	 *  - union으로 raw 8비트 접근 / 비트별 접근 둘 다 가능
	 */
	union {
		struct {

			/** Whether the accel_sequence member is valid */
			uint8_t has_accel_sequence		: 1;
	                              /* [한국어] accel_sequence가 설정되어 있는가 — DMA 엔진 체인 처리 중 */

			/** Whether memory_domain member is valid */
			uint8_t has_memory_domain		: 1;
	                              /* [한국어] memory_domain이 설정되어 있는가 — RDMA/GPU 도메인 pull/push 필요 */

			/** Whether the split data structure is valid */
			uint8_t split				: 1;
	                              /* [한국어] 이 I/O가 split의 parent인가 — split 자식 추적 중 */

			/** Whether ptr in the buf data structure is valid */
			uint8_t has_buf				: 1;
	                              /* [한국어] internal.buf.ptr이 유효 — bdev 코어가 버퍼를 할당해 보유 중 */

			/** Whether the bounce_buf data structure is valid */
			uint8_t has_bounce_buf			: 1;
	                              /* [한국어] bounce_buf 사용 중 — 정렬 위반/메타 처리로 double buffering */

			/** Whether we are currently inside the submit request call */
			uint8_t in_submit_request		: 1;
	                              /* [한국어] 모듈 submit_request 콜백 호출 중 — 재진입 감지/완료 timing 제어 */

			/** Whether the I/O is a sub-I/O of a split parent I/O */
			uint8_t child_io		: 1;
	                              /* [한국어] split 자식 I/O인가 — 완료 시 parent 카운터 감소 처리 */

			uint8_t reserved			: 1;
	                              /* [한국어] 예약 비트 */
		};
		uint8_t raw;          /* [한국어] 8비트 통째 접근 — 빠른 zero-clear 등 */
	} f;

	/** Status for the IO */
	int8_t status;                /* [한국어] enum spdk_bdev_io_status 값 — 모듈은 set_*_status / complete API로만 변경 */

	/** Retry state (resubmit, re-pull, re-push, etc.) */
	uint8_t retry_state;          /* [한국어] retry 상태 머신 — NOMEM 후 어떤 단계(submit/pull/push)에서 재개할지 */

	uint8_t	reserved[5];          /* [한국어] 정렬 예약 */

	/** The bdev descriptor that was used when submitting this I/O. */
	struct spdk_bdev_desc *desc;  /* [한국어] 이 I/O가 발행된 desc — 사용자 식별/락 추적 */

	/** User function that will be called when this completes */
	spdk_bdev_io_completion_cb cb;
	                              /* [한국어] 완료 시 호출할 사용자 콜백 (bdev.h API 호출자가 등록) */

	/** Context that will be passed to the completion callback */
	void *caller_ctx;             /* [한국어] cb의 첫 인자 — 사용자 컨텍스트 */

	/** Current tsc at submit time. Used to calculate latency at completion. */
	uint64_t submit_tsc;          /* [한국어] 제출 시각(틱, spdk_get_ticks) — 완료 시 latency 통계 계산 입력 */

	/** Entry to the list io_submitted of struct spdk_bdev_channel */
	TAILQ_ENTRY(spdk_bdev_io) ch_link;
	                              /* [한국어] 채널의 submitted I/O 리스트 링크 — drain/abort 시 순회 */

	/** bdev_io pool entry */
	STAILQ_ENTRY(spdk_bdev_io) buf_link;
	                              /* [한국어] iobuf 풀/큐 링크 — 풀 free list나 buf 대기 큐에서 사용 */

	/*
	 * [한국어] 백엔드 에러 정보 union — status에 따라 어느 분기 유효한지 결정
	 */
	union {
		/*
		 * [한국어] NVMe 에러 — status==NVME_ERROR 시 유효 (bdev_nvme 모듈 등)
		 */
		struct {
			/** NVMe completion queue entry DW0 */
			uint32_t cdw0;/* [한국어] CQE DW0 (명령별 응답 데이터 — 예: WRITE_ZEROES count) */
			/** NVMe status code type */
			uint8_t sct;  /* [한국어] Status Code Type — 0:Generic, 1:Cmd-Specific, 2:Media, ... */
			/** NVMe status code */
			uint8_t sc;   /* [한국어] Status Code — sct에 따른 세부 코드 (예: 0x81 LBA Out of Range) */
		} nvme;
		/** Only valid when status is SPDK_BDEV_IO_STATUS_SCSI_ERROR */
		/*
		 * [한국어] SCSI 에러 — status==SCSI_ERROR 시 유효 (iSCSI/SCSI bdev 등)
		 */
		struct {
			/** SCSI status code */
			uint8_t sc;   /* [한국어] SCSI Status (CHECK CONDITION 등) */
			/** SCSI sense key */
			uint8_t sk;   /* [한국어] Sense Key (NOT READY, ILLEGAL REQUEST, ...) */
			/** SCSI additional sense code */
			uint8_t asc;  /* [한국어] Additional Sense Code */
			/** SCSI additional sense code qualifier */
			uint8_t ascq; /* [한국어] ASC Qualifier */
		} scsi;
		/** Only valid when status is SPDK_BDEV_IO_STATUS_AIO_ERROR */
		int aio_result;       /* [한국어] AIO 에러 — bdev_aio가 libaio errno를 그대로 보고 (음수 errno) */
	} error;

	/*
	 * [한국어] split 진행 상태 — f.split=1일 때 유효
	 *  - parent I/O가 여러 child I/O로 쪼개질 때 코어가 사용
	 */
	struct {
		/** stored user callback in case we split the I/O and use a temporary callback */
		spdk_bdev_io_completion_cb stored_user_cb;
	                              /* [한국어] 원래 사용자 콜백 보관 — split 임시 콜백으로 cb를 덮어쓰는 동안 백업 */

		/** number of blocks remaining in a split i/o */
		uint64_t remaining_num_blocks;
	                              /* [한국어] split parent의 미처리 잔여 블록 수 — 0 도달 시 사용자 콜백 호출 */

		/** current offset of the split I/O in the bdev */
		uint64_t current_offset_blocks;
	                              /* [한국어] 다음 child I/O의 시작 LBA — child 발행마다 전진 */

		/** count of outstanding batched split I/Os */
		uint32_t outstanding;
	                              /* [한국어] 동시 발행 중인 child I/O 수 — atomic decrement로 완료 감지 */
	} split;

	/*
	 * [한국어] 코어가 자동 할당한 데이터 버퍼 — f.has_buf=1일 때 유효
	 */
	struct {
		/** bdev allocated memory associated with this request */
		void *ptr;            /* [한국어] iobuf 풀에서 할당된 버퍼 포인터 — 완료 시 자동 반납 */

		/** requested size of the buffer associated with this I/O */
		uint64_t len;         /* [한국어] 요청된 버퍼 크기 (바이트) */
	} buf;

	/** if the request is double buffered, store original request iovs here */
	/*
	 * [한국어] bounce buffer 정보 — f.has_bounce_buf=1일 때 유효
	 *  - 사용자 버퍼가 정렬 위반/메모리 도메인 mismatch 시 코어가 임시 버퍼 통해 데이터 전달
	 */
	struct {
		struct iovec  iov;    /* [한국어] bounce buffer iovec (코어가 사용자 iovec을 이 하나로 대체) */
		struct iovec  md_iov; /* [한국어] bounce 메타데이터 iovec */
		struct iovec  orig_md_iov;
	                              /* [한국어] 원래 메타데이터 iovec 백업 — 완료 시 데이터 복사 후 복원 */
		struct iovec *orig_iovs;
	                              /* [한국어] 원래 사용자 iovec 배열 백업 — 완료 시 read이면 bounce → orig 복사 */
		int           orig_iovcnt;
	                              /* [한국어] 원래 iovcnt 백업 */
	} bounce_buf;

	/** Callback for when buf is allocated */
	spdk_bdev_io_get_buf_cb get_buf_cb;
	                              /* [한국어] spdk_bdev_io_get_buf 등록 콜백 — 메모리 확보 후 호출 */

	/**
	 * Queue entry used in several cases:
	 *  1. IOs awaiting retry due to NOMEM status,
	 *  2. IOs awaiting submission due to QoS,
	 *  3. IOs with an accel sequence being executed,
	 *  4. IOs awaiting memory domain pull/push,
	 *  5. queued reset requests.
	 */
	TAILQ_ENTRY(spdk_bdev_io) link;
	                              /* [한국어] 다목적 대기 큐 링크 — 같은 I/O는 한 번에 하나의 큐에만 속함
	                               *  - NOMEM retry 큐, QoS 대기 큐, accel 진행 큐, memory-domain pull/push 큐, queued_resets */

	/** iobuf queue entry */
	struct spdk_iobuf_entry iobuf;/* [한국어] iobuf 풀 대기 엔트리 — 메모리 부족 시 대기 큐에 등록 */

	/** Enables queuing parent I/O when no bdev_ios available for split children. */
	struct spdk_bdev_io_wait_entry waitq_entry;
	                              /* [한국어] split 시 child용 bdev_io 풀 부족하면 parent 대기 — bdev_io 풀이 회복되면 parent 재개 */

	/** Memory domain and its context passed by the user in ext API */
	struct spdk_memory_domain *memory_domain;
	                              /* [한국어] 사용자가 ext API로 전달한 memory domain 사본 (u.bdev에서도 보유, 여기는 코어 처리용) */
	void *memory_domain_ctx;      /* [한국어] memory_domain 컨텍스트 사본 */

	/* Sequence of accel operations passed by the user */
	struct spdk_accel_sequence *accel_sequence;
	                              /* [한국어] 사용자가 전달한 accel 시퀀스 사본 */

	/** Data transfer completion callback */
	void (*data_transfer_cpl)(void *ctx, int rc);
	                              /* [한국어] 메모리 도메인 pull/push 등 데이터 전송 단계 완료 콜백 — bdev 코어 내부에서만 사용 */
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
/*
 * [한국어]
 * spdk_bdev_register - 새 bdev을 bdev 코어에 등록
 *
 * @bdev:   완전히 채워진 spdk_bdev 구조체 (name, blocklen, blockcnt, fn_table, module 필수).
 * @return: 0 성공 / -EINVAL name이 NULL / -EEXIST 같은 이름의 bdev/alias 존재.
 *
 * 호출 컨텍스트: SPDK app thread (g_main_thread). 다른 스레드에서 호출 시 ABI 깨짐.
 *
 * 동작:
 *   1) bdev_names RB 트리에 이름 삽입 (충돌 검사)
 *   2) UUID 미설정이면 자동 생성
 *   3) 전역 bdev 리스트(internal.link) 추가
 *   4) 등록된 모든 vbdev 모듈의 examine_config 콜백 트리거 (sync)
 *   5) 이어서 examine_disk 콜백 (async — examine_done 호출까지 대기)
 *
 * 호출 체인:
 *   bdev_module의 module_init/examine 콜백 → spdk_bdev_register
 *     → bdev_examine() → 각 vbdev module의 examine_config/disk
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
/*
 * [한국어]
 * spdk_bdev_unregister - bdev 비등록 시작 (deferred)
 *
 * @bdev:   해제할 bdev.
 * @cb_fn:  비등록 완료 콜백 (호출 스레드와 동일 스레드에서 invoke).
 * @cb_arg: cb_fn에 전달할 컨텍스트.
 *
 * 호출 컨텍스트: SPDK app thread 권장 (26.05 release부터 다른 스레드 호출 disallowed).
 *
 * 동작 단계:
 *   1) 모든 open desc에 SPDK_BDEV_EVENT_REMOVE 통지 → 사용자가 close()
 *   2) 모든 desc가 close되면 fn_table->destruct 호출
 *   3) destruct 완료 후 cb_fn(cb_arg, rc) 호출
 *
 * 권장: spdk_bdev_unregister_by_name 사용 (race condition 안전).
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
/*
 * [한국어]
 * spdk_bdev_unregister_by_name - 이름과 모듈로 bdev 비등록 (race-safe 변형)
 *
 * @bdev_name: 해제할 bdev 이름.
 * @module:    해당 bdev을 등록한 모듈 — 일치하지 않으면 -ENODEV 에러로 거부.
 * @cb_fn:     완료 콜백.
 * @cb_arg:    콜백 인자.
 * @return:    0 성공 / 음수 errno (ENODEV: bdev 없음 또는 모듈 mismatch).
 *
 * spdk_bdev_unregister 대비 안전한 점: bdev 포인터를 미리 보관하지 않고 이름으로
 * lookup하므로 동시 unregister/리네임 race를 회피. 권장 API.
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
/*
 * [한국어]
 * spdk_bdev_destruct_done - 비동기 destruct 완료 통지
 *
 * @bdev:      destruct가 완료된 bdev.
 * @bdeverrno: destruct 결과 (0 성공 / 음수 실패).
 *
 * fn_table->destruct가 비동기 처리(1 반환)를 선언했을 때, 모듈이 백엔드 정리를
 * 마친 뒤 반드시 호출. 코어가 이 시점에 unregister 후속 단계(open_descs 정리 등)
 * 를 진행한 뒤 unregister_cb 호출.
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
/*
 * [한국어]
 * spdk_bdev_module_examine_done - examine 콜백 완료 통지
 *
 * @module: 검사를 완료한 모듈.
 *
 * 모듈의 examine_config/examine_disk 콜백 안에서(또는 비동기적으로 이후) 반드시
 * 호출. bdev 코어는 모든 모듈의 examine이 완료될 때까지 다음 등록/검사 단계를
 * 보류한다 (action_in_progress 카운터 감소).
 *
 * 호출 컨텍스트: 검사 콜백을 받은 스레드 또는 그 비동기 후속 콜백 스레드.
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
/*
 * [한국어]
 * spdk_bdev_module_init_done - 비동기 module_init 완료 통지
 *
 * @module: init이 완료된 모듈.
 *
 * spdk_bdev_module.async_init=true인 모듈만 호출. module_init이 즉시 반환했더라도
 * 실제 부트스트랩(예: NVMe controller probe, DPDK device attach)이 비동기일 때
 * 작업 완료 후 이 함수를 호출해 코어에 알린다. 호출 전까지 bdev 코어는 examine
 * 단계로 진행하지 않음.
 */
void spdk_bdev_module_init_done(struct spdk_bdev_module *module);

/**
 * Indicate that the module finish has completed.
 *
 * To be called in response to the module_fini, only if async_fini is set.
 *
 */
/*
 * [한국어]
 * spdk_bdev_module_fini_done - 비동기 module_fini 완료 통지
 *
 * async_fini=true 모듈만 호출. module_fini가 비동기 cleanup(예: 백엔드 thread
 * join, 핸들 close)을 수행한 뒤 반드시 호출 — 코어가 bdev 서브시스템 종료의
 * 다음 단계로 진행.
 */
void spdk_bdev_module_fini_done(void);

/**
 * Indicate that the module fini start has completed.
 *
 * To be called in response to the fini_start, only if async_fini_start is set.
 * May be called during fini_start or asynchronously.
 *
 */
/*
 * [한국어]
 * spdk_bdev_module_fini_start_done - 비동기 fini_start 완료 통지
 *
 * async_fini_start=true 모듈만 호출. fini_start 안에서 또는 비동기 후속에서
 * 호출하여 종료 단계 진행을 허용. 보통 vbdev 모듈이 자기 vbdev들을 unregister
 * 시작한 뒤 호출.
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
/*
 * [한국어]
 * spdk_bdev_alias_add - bdev에 별칭(alias) 추가
 *
 * @bdev:   대상 bdev (등록 후여야 함).
 * @alias:  추가할 별칭 문자열 (NUL-terminated, 비어있으면 EINVAL).
 * @return: 0 성공 / -EEXIST 중복 / -ENOMEM 메모리 부족 / -EINVAL 빈 문자열.
 *
 * 별칭은 본 이름과 동일하게 lookup 가능 (전역 bdev_names RB 트리에 함께 등록).
 * unregister 시 모든 별칭 자동 제거. NVMe 모듈이 namespace serial+nsid 등을
 * 별칭으로 노출하는 식의 사용.
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
/*
 * [한국어]
 * spdk_bdev_alias_del - bdev에서 별칭 제거
 *
 * @bdev:   대상 bdev.
 * @alias:  제거할 별칭.
 * @return: 0 성공 / -ENOENT 별칭 없음.
 */
int spdk_bdev_alias_del(struct spdk_bdev *bdev, const char *alias);

/**
 * Removes all alias from block device alias list.
 *
 * \param bdev Block device to operate.
 */
/*
 * [한국어]
 * spdk_bdev_alias_del_all - bdev의 모든 별칭 제거
 *
 * @bdev: 대상 bdev. unregister 직전 정리 작업으로 코어가 호출.
 */
void spdk_bdev_alias_del_all(struct spdk_bdev *bdev);

/**
 * Get pointer to block device aliases list.
 *
 * \param bdev Block device to query.
 * \return Pointer to bdev aliases list.
 */
/*
 * [한국어]
 * spdk_bdev_get_aliases - bdev의 별칭 리스트 조회
 *
 * @bdev:   대상 bdev (const).
 * @return: 별칭 리스트 head 포인터 (TAILQ_HEAD 기반, 사용자는 read-only로 순회).
 *
 * RPC bdev_get_bdevs 응답 등에서 별칭 enumerate에 사용.
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
/*
 * [한국어]
 * spdk_bdev_io_get_buf - bdev_io에 데이터 버퍼 할당 (또는 정렬 위반 시 bounce buf)
 *
 * @bdev_io: 대상 I/O.
 * @cb:      버퍼 준비 완료 시 호출할 콜백.
 * @len:     필요한 버퍼 크기 (바이트). SGL 미지정 시 SPDK_BDEV_LARGE_BUF_MAX_SIZE 이하.
 *
 * 동작:
 *   - SGL 이미 보유 + 정렬 OK → 즉시 cb(success=true)
 *   - SGL 미지정 또는 정렬 위반 → iobuf 풀에서 정렬된 버퍼 할당 + bounce buffer 설정 후 cb 호출
 *   - 메모리 부족 → 콜백을 deferred queue에 넣고, 다른 I/O 완료로 메모리 해방 시 자동 재시도
 *
 * 호출 컨텍스트: bdev_io를 발행한 스레드와 동일해야 함 (코어가 검증).
 *
 * 사용 사례: 모듈 submit_request에서 bdev_io->u.bdev.iovs 미설정이면 이 함수로
 * 버퍼 할당 후 콜백 안에서 백엔드로 forward.
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
/*
 * [한국어]
 * spdk_bdev_io_set_buf - 외부 제공 버퍼를 bdev_io 데이터 버퍼로 설정
 *
 * @bdev_io: 대상 I/O.
 * @buf:     사용할 버퍼 포인터.
 * @len:     버퍼 길이.
 *
 * 정렬 요건 충족을 위해 사용 영역이 일부 조정될 수 있음. 모듈이 자체 풀에서
 * 버퍼를 가져와 사용할 때 호출.
 */
void spdk_bdev_io_set_buf(struct spdk_bdev_io *bdev_io, void *buf, size_t len);

/**
 * Set the given buffer as metadata buffer described by this bdev_io.
 *
 * \param bdev_io I/O to set the buffer on.
 * \param md_buf The buffer to set as the active metadata buffer.
 * \param len The length of the metadata buffer.
 */
/*
 * [한국어]
 * spdk_bdev_io_set_md_buf - 외부 제공 버퍼를 메타데이터 버퍼로 설정
 *
 * @bdev_io: 대상 I/O.
 * @md_buf:  메타데이터 버퍼 포인터 (PI/Reftag 영역 포함).
 * @len:     버퍼 길이.
 *
 * Separate metadata layout (md_interleave=false)에서 사용. 모듈이 PI 검증/생성
 * 작업 후 결과를 이 버퍼에 노출.
 */
void spdk_bdev_io_set_md_buf(struct spdk_bdev_io *bdev_io, void *md_buf, size_t len);

/**
 * Complete a bdev_io
 *
 * \param bdev_io I/O to complete.
 * \param status The I/O completion status.
 */
/*
 * [한국어]
 * spdk_bdev_io_complete - bdev I/O 완료 보고 (모듈 → 코어)
 *
 * @bdev_io: 완료할 I/O.
 * @status:  spdk_bdev_io_status 값 (SUCCESS / FAILED / NOMEM / ABORTED / NVME_ERROR / ...).
 *
 * 동작:
 *   1) bdev_io->internal.status 갱신
 *   2) split parent의 카운터 감소 / 모든 child 완료 시 부모 완료 로직 트리거
 *   3) bounce_buf 사용 시 read 데이터 → 사용자 iovec 복사
 *   4) 사용자 콜백 invoke → bdev_io를 채널 mempool에 반납
 *   5) NOMEM이면 채널의 NOMEM 큐에 넣어 다음 완료 기점에 자동 재시도
 *
 * 호출 컨텍스트: bdev_io를 받은 채널의 spdk_thread (모듈 콜백과 동일 스레드).
 * 다른 스레드면 spdk_thread_send_msg로 마샬링 필요.
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
/*
 * [한국어]
 * spdk_bdev_io_set_nvme_status - NVMe 에러 코드를 bdev_io에 기록 (완료는 별도)
 *
 * @bdev_io: 대상 I/O.
 * @cdw0:    NVMe Completion Queue Entry DW0 (해당 명령 응답 데이터, 무관 시 0).
 * @sct:     NVMe Status Code Type (Generic/CmdSpecific/Media/Path).
 * @sc:      NVMe Status Code (sct별 세부 코드).
 * @return:  매핑된 spdk_bdev_io_status (성공 코드면 SUCCESS, 그 외 NVME_ERROR).
 *
 * NVMe 모듈이 SQE 완료를 받은 후 SCT/SC를 그대로 보존하면서 bdev 상태도 설정.
 * 즉시 완료까지 하려면 spdk_bdev_io_complete_nvme_status 사용.
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
/*
 * [한국어]
 * spdk_bdev_io_complete_nvme_status - NVMe 에러 설정 + 즉시 완료
 *
 * @bdev_io / @cdw0 / @sct / @sc: 위 set_nvme_status와 동일.
 *
 * 가장 빈번한 사용 패턴: bdev_nvme 모듈이 NVMe completion polling에서 CQE를
 * 받자마자 이 함수로 bdev 코어에 한 번에 보고 (set + complete 통합).
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
/*
 * [한국어]
 * spdk_bdev_io_set_scsi_status - SCSI 에러 코드를 bdev_io에 기록 (완료는 별도)
 *
 * @bdev_io: 대상.
 * @sc:      SCSI Status (예: CHECK CONDITION).
 * @sk:      Sense Key (예: ILLEGAL REQUEST).
 * @asc:     Additional Sense Code.
 * @ascq:    ASC Qualifier.
 * @return:  매핑된 spdk_bdev_io_status.
 *
 * iSCSI/SCSI 백엔드에서 사용. NVMe→SCSI 변환은 spdk_scsi_nvme_translate 참조.
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
/*
 * [한국어]
 * spdk_bdev_io_complete_scsi_status - SCSI 에러 설정 + 즉시 완료
 *
 * 위 set_scsi_status를 한 번에 처리 + spdk_bdev_io_complete 호출 효과.
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
/*
 * [한국어]
 * spdk_bdev_io_set_aio_status - AIO errno를 bdev_io에 기록
 *
 * @bdev_io:    대상.
 * @aio_result: AIO에서 반환된 음수 errno.
 * @return:     매핑된 spdk_bdev_io_status (대개 AIO_ERROR).
 *
 * bdev_aio 모듈(libaio 백엔드)에서 io_event.res<0인 경우 호출.
 */
enum spdk_bdev_io_status spdk_bdev_io_set_aio_status(struct spdk_bdev_io *bdev_io, int aio_result);

/**
 * Set and complete a bdev_io with AIO errno.
 *
 * \param bdev_io I/O to complete.
 * \param aio_result Negative errno returned from AIO.
 */
/*
 * [한국어]
 * spdk_bdev_io_complete_aio_status - AIO 에러 설정 + 즉시 완료
 *
 * bdev_aio 모듈의 일반 완료 경로 (음수 errno → AIO_ERROR로 보고).
 */
void spdk_bdev_io_complete_aio_status(struct spdk_bdev_io *bdev_io, int aio_result);

/**
 * Copy a bdev_io status from another bdev_io.
 *
 * \param bdev_io I/O to set status.
 * \param base_io I/O from which to copy the status.
 * \return IO status corresponding to the base_io status
 */
/*
 * [한국어]
 * spdk_bdev_io_set_base_io_status - 다른 bdev_io의 상태를 그대로 복사
 *
 * @bdev_io: 대상.
 * @base_io: 상태를 가져올 원본 (보통 vbdev이 forward한 하위 bdev의 child IO).
 * @return:  복사된 status.
 *
 * vbdev 모듈(passthru, raid 등)이 하위 bdev의 child IO 완료를 받아 자신의 상위
 * bdev_io에 그대로 전파할 때 사용. NVMe/SCSI 에러 union도 함께 복사.
 */
enum spdk_bdev_io_status spdk_bdev_io_set_base_io_status(struct spdk_bdev_io *bdev_io,
		const struct spdk_bdev_io *base_io);

/**
 * Complete a bdev_io copying a status from another bdev_io.
 *
 * \param bdev_io I/O to complete.
 * \param base_io I/O from which to copy the status.
 */
/*
 * [한국어]
 * spdk_bdev_io_complete_base_io_status - 위 set_base_io_status + 즉시 완료
 *
 * vbdev 모듈의 가장 흔한 forward 패턴: child 완료 콜백에서 한 줄로 상위 IO 완료.
 */
void spdk_bdev_io_complete_base_io_status(struct spdk_bdev_io *bdev_io,
		const struct spdk_bdev_io *base_io);

/**
 * Get a thread that given bdev_io was submitted on.
 *
 * \param bdev_io I/O
 * \return thread that submitted the I/O
 */
/*
 * [한국어]
 * spdk_bdev_io_get_thread - bdev_io를 발행한 spdk_thread 반환
 *
 * @bdev_io: 조회 대상.
 * @return:  발행 스레드 — 완료 콜백이 반드시 이 스레드에서 invoke되어야 함.
 *
 * 모듈이 다른 스레드에서 완료 처리를 마쳤다면 이 스레드로 spdk_thread_send_msg
 * 마샬링 후 spdk_bdev_io_complete 호출.
 */
struct spdk_thread *spdk_bdev_io_get_thread(struct spdk_bdev_io *bdev_io);

/**
 * Get the bdev module's I/O channel that the given bdev_io was submitted on.
 *
 * \param bdev_io I/O
 * \return the bdev module's I/O channel that the given bdev_io was submitted on.
 */
/*
 * [한국어]
 * spdk_bdev_io_get_io_channel - bdev_io의 모듈 측 io_channel 반환
 *
 * @bdev_io: 조회 대상.
 * @return:  fn_table->get_io_channel가 반환했던 모듈 채널 (NVMe qpair 등 컨테이너).
 *
 * 모듈 함수가 bdev_io에서 자신의 채널 컨텍스트를 빠르게 복원할 때 사용.
 */
struct spdk_io_channel *spdk_bdev_io_get_io_channel(struct spdk_bdev_io *bdev_io);

/**
 * Get the submit_tsc of a bdev I/O.
 *
 * \param bdev_io The bdev I/O to get the submit_tsc.
 *
 * \return The submit_tsc of the specified bdev I/O.
 */
/*
 * [한국어]
 * spdk_bdev_io_get_submit_tsc - bdev_io 제출 시각 (틱) 조회
 *
 * @bdev_io: 조회 대상.
 * @return:  internal.submit_tsc — 제출 시점 spdk_get_ticks() 값.
 *
 * 모듈이 별도 latency 통계를 계산할 때 사용. 코어 통계는 자동 처리됨.
 */
uint64_t spdk_bdev_io_get_submit_tsc(struct spdk_bdev_io *bdev_io);

/**
 * Query if metadata is hidden from the bdev I/O.
 *
 * \param bdev_io The bdev I/O to query.
 *
 * \return true if metadata is hidden from the bdev I/O, or false otherwise.
 */
/*
 * [한국어]
 * spdk_bdev_io_hide_metadata - 사용자에게 메타데이터를 숨기는지 여부 조회
 *
 * @bdev_io: 조회 대상.
 * @return:  true = bdev 코어가 PI 검증/생성을 수행하고 사용자는 데이터만 다룸.
 *           false = 사용자 책임.
 *
 * vbdev 모듈이 PI를 자체 처리하면서 상위 사용자에게는 noop으로 보일 때 활용.
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
/*
 * [한국어]
 * spdk_bdev_notify_blockcnt_change - bdev 용량 변경 통지 (resize)
 *
 * @bdev:   대상 bdev (등록된 상태여야 함).
 * @size:   새 블록 수.
 * @return: 0 성공 / 음수 errno.
 *
 * 동작:
 *   - bdev->blockcnt를 새 값으로 갱신
 *   - 모든 open desc에 SPDK_BDEV_EVENT_RESIZE 전파 → 사용자가 새 크기 인지
 *
 * 사용 사례: lvol grow, NVMe NS 크기 변경 등 동적 resize.
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
/*
 * [한국어]
 * spdk_scsi_nvme_translate - NVMe 에러 → SCSI 에러 코드 변환
 *
 * @bdev_io:           NVMe 에러를 담은 bdev_io (status==NVME_ERROR + internal.error.nvme 유효).
 * @sc/@sk/@asc/@ascq: 출력 — SCSI Status/Sense Key/ASC/ASCQ.
 *
 * iSCSI target 등 SCSI 응답을 만들어야 하는 상위 레이어가 NVMe SCT/SC를
 * 표준 SAM 매핑 표에 따라 변환할 때 호출. 매핑은 lib/bdev/bdev.c 내부 표 참조.
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
/*
 * [한국어]
 * spdk_bdev_module_list_add - 등록 모듈 리스트에 모듈 추가 (constructor 경로)
 *
 * @bdev_module: 등록할 모듈 포인터 (전역 정적 변수여야 함).
 *
 * 보통 직접 호출하지 않고 SPDK_BDEV_MODULE_REGISTER 매크로가 GCC constructor
 * attribute를 통해 main 진입 전 자동 호출. bdev 코어가 후속 module_init 단계에서
 * 이 리스트를 순회.
 */
void spdk_bdev_module_list_add(struct spdk_bdev_module *bdev_module);

/**
 * Find registered module with name pointed by \c name.
 *
 * \param name name of module to be searched for.
 * \return pointer to module or NULL if no module with \c name exist
 */
/*
 * [한국어]
 * spdk_bdev_module_list_find - 이름으로 등록된 모듈 검색
 *
 * @name:   찾을 모듈 이름 ("nvme", "aio" 등).
 * @return: 일치하는 모듈 포인터 또는 NULL.
 *
 * RPC 핸들러가 모듈별 동작을 분기할 때 사용 (예: bdev_nvme_attach_controller).
 */
struct spdk_bdev_module *spdk_bdev_module_list_find(const char *name);

/*
 * [한국어]
 * spdk_bdev_io_from_ctx - driver_ctx 포인터에서 부모 bdev_io 복원
 *
 * @ctx:    bdev_io->driver_ctx 시작 주소 (모듈이 자기 컨텍스트로 사용 중인 영역).
 * @return: container_of 패턴으로 계산한 부모 spdk_bdev_io 포인터.
 *
 * 모듈이 NVMe completion 콜백 등에서 자기 ctx만 인자로 받은 후 부모 bdev_io를
 * 복원할 때 사용. inline으로 빠른 오프셋 계산.
 */
static inline struct spdk_bdev_io *
spdk_bdev_io_from_ctx(void *ctx)
{
	return SPDK_CONTAINEROF(ctx, struct spdk_bdev_io, driver_ctx);
                              /* [한국어] driver_ctx 시작 오프셋을 빼서 부모 spdk_bdev_io 베이스 주소 복원 */
}

/*
 * [한국어] === 파티션 vbdev 헬퍼 (spdk_bdev_part_*) ===
 *
 * 한 base bdev을 LBA 범위로 쪼개 여러 개의 파티션 vbdev을 만드는 모듈
 * (split, GPT 파티션 등)을 위한 보조 API. 모듈은 다음 단계를 따른다:
 *
 *   1) spdk_bdev_part_base_construct_ext(base_bdev_name, ..., &part_base)
 *      → base bdev open + claim + per-base 컨텍스트 보유
 *   2) 각 파티션마다 spdk_bdev_part_construct(_ext)(part, part_base, name,
 *      offset_blocks, num_blocks, product_name)
 *      → part->internal.bdev (자체 spdk_bdev) 자동 register
 *   3) 모듈의 fn_table.submit_request → spdk_bdev_part_submit_request
 *      → 코어가 offset_blocks를 자동 가산해 base로 forward
 *   4) hotremove → spdk_bdev_part_base_hotremove로 파트들 일괄 unregister
 *
 * 핵심 이점: offset 변환과 채널 라이프사이클을 코어가 처리하므로 모듈은
 * 메타데이터 파싱과 파티션 enumeration에만 집중.
 */
struct spdk_bdev_part_base;
                              /* [한국어] forward declaration — 실제 정의는 lib/bdev/part.c (모듈은 opaque 핸들로 사용) */

/**
 * Returns a pointer to the spdk_bdev associated with an spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return A pointer to the base's spdk_bdev struct.
 */
/*
 * [한국어]
 * spdk_bdev_part_base_get_bdev - part_base가 점유한 base bdev 포인터 조회
 *
 * @part_base: 파티션 베이스 핸들.
 * @return:    하위 base bdev 포인터.
 */
struct spdk_bdev *spdk_bdev_part_base_get_bdev(struct spdk_bdev_part_base *part_base);

/**
 * Returns a spdk_bdev name of the corresponding spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return A text string representing the name of the base bdev.
 */
/*
 * [한국어]
 * spdk_bdev_part_base_get_bdev_name - base bdev 이름 문자열 반환
 *
 * @part_base: 파티션 베이스.
 * @return:    base bdev->name (정적 — modify 금지).
 */
const char *spdk_bdev_part_base_get_bdev_name(struct spdk_bdev_part_base *part_base);

/**
 * Returns a pointer to the spdk_bdev_descriptor associated with an spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return A pointer to the base's spdk_bdev_desc struct.
 */
/*
 * [한국어]
 * spdk_bdev_part_base_get_desc - part_base가 base bdev에 대해 보유한 desc 조회
 *
 * @part_base: 베이스.
 * @return:    open desc — 모든 파트의 I/O가 이 desc 통해 forward됨.
 */
struct spdk_bdev_desc *spdk_bdev_part_base_get_desc(struct spdk_bdev_part_base *part_base);

/**
 * Returns a pointer to the tailq associated with an spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return The head of a tailq of spdk_bdev_part structs registered to the base's module.
 */
/*
 * [한국어]
 * spdk_bdev_part_base_get_tailq - 이 베이스에 등록된 파트들의 리스트 head 반환
 *
 * @part_base: 베이스.
 * @return:    spdk_bdev_part 리스트 head — 각 파트 enumerate 시 사용.
 */
struct bdev_part_tailq *spdk_bdev_part_base_get_tailq(struct spdk_bdev_part_base *part_base);

/**
 * Returns a pointer to the module level context associated with an spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return A pointer to the module level context registered with the base in spdk_bdev_part_base_construct.
 */
/*
 * [한국어]
 * spdk_bdev_part_base_get_ctx - 모듈이 등록한 base 컨텍스트 조회
 *
 * @part_base: 베이스.
 * @return:    base 생성 시 모듈이 전달한 ctx 포인터.
 */
void *spdk_bdev_part_base_get_ctx(struct spdk_bdev_part_base *part_base);

/*
 * [한국어]
 * spdk_bdev_part_base_free_fn - base ctx 해제 콜백 typedef
 *
 * @ctx: 모듈이 base 생성 시 전달한 컨텍스트.
 *
 * base 자체가 hotremove/shutdown으로 해제될 때 모듈 측 자원 정리 기회 제공.
 */
typedef void (*spdk_bdev_part_base_free_fn)(void *ctx);

/*
 * [한국어] struct spdk_bdev_part - 단일 파티션 vbdev
 *
 * 모듈이 자신의 파티션 디스크립터 안에 이 구조체를 임베드 + 추가 필드 보유.
 * 코어 part.c는 이 구조체의 internal 필드를 통해 base+offset 정보를 활용.
 */
struct spdk_bdev_part {
	/* Entry into the module's global list of bdev parts */
	TAILQ_ENTRY(spdk_bdev_part)	tailq;
	                              /* [한국어] base의 part 리스트 링크 — base_get_tailq 결과 리스트의 노드 */

	/**
	 * Fields that are used internally by part.c These fields should only
	 * be accessed from a module using any pertinent get and set methods.
	 */
	/*
	 * [한국어] part.c 전용 내부 필드 — get/set 메서드를 통해서만 접근
	 */
	struct bdev_part_internal_fields {

		/* This part's corresponding bdev object. Not to be confused with the base bdev */
		struct spdk_bdev		bdev;
	                              /* [한국어] 이 파트가 노출하는 spdk_bdev 인스턴스 (base와 별개)
	                               *  - spdk_bdev_part_construct가 자동 register */

		/* The base to which this part belongs */
		struct spdk_bdev_part_base	*base;
	                              /* [한국어] 소속 base 포인터 — I/O forward의 종착지 */

		/* number of blocks from the start of the base bdev to the start of this part */
		uint64_t			offset_blocks;
	                              /* [한국어] base bdev 시작점부터 이 파트 시작 LBA까지의 거리 (블록)
	                               *  - submit_request_ext가 사용자 LBA에 자동 가산 */
	} internal;
};

/*
 * [한국어] struct spdk_bdev_part_channel - 파트 I/O 채널 컨텍스트
 *
 * 모듈이 fn_table->get_io_channel에서 반환하는 채널의 trailing 영역에 임베드.
 * 코어 submit_request가 base_ch를 사용해 하위 bdev로 forward.
 */
struct spdk_bdev_part_channel {
	struct spdk_bdev_part		*part;
	                              /* [한국어] 이 채널이 속한 파트 — 완료 콜백에서 offset 등 메타 복원에 사용 */
	struct spdk_io_channel		*base_ch;
	                              /* [한국어] 하위 base bdev에 대한 io_channel — spdk_bdev_get_io_channel(base_desc)로 획득 */
};

typedef TAILQ_HEAD(bdev_part_tailq, spdk_bdev_part)	SPDK_BDEV_PART_TAILQ;
                              /* [한국어] 파트 리스트 타입 — 모듈이 정적 인스턴스 선언에 사용 */

/**
 * Free the base corresponding to one or more spdk_bdev_part.
 *
 * \param base The base to free.
 */
/*
 * [한국어]
 * spdk_bdev_part_base_free - part_base 해제
 *
 * @base: 해제할 베이스. base bdev close + claim release + free_fn 호출.
 */
void spdk_bdev_part_base_free(struct spdk_bdev_part_base *base);

/**
 * Free an spdk_bdev_part context.
 *
 * \param part The part to free.
 *
 * \return 1 always. To indicate that the operation is asynchronous.
 */
/*
 * [한국어]
 * spdk_bdev_part_free - 단일 파티션 해제
 *
 * @part:   해제할 파트.
 * @return: 항상 1 (비동기 — 실제 해제는 spdk_bdev_unregister 비동기 경로 따라감).
 *
 * 파트의 spdk_bdev을 unregister하고, 베이스의 마지막 파트라면 자동으로
 * base도 free.
 */
int spdk_bdev_part_free(struct spdk_bdev_part *part);

/**
 * Calls spdk_bdev_unregister on the bdev for each part associated with base_bdev.
 *
 * \param part_base The part base object built on top of an spdk_bdev
 * \param tailq The list of spdk_bdev_part bdevs associated with this base bdev.
 */
/*
 * [한국어]
 * spdk_bdev_part_base_hotremove - base bdev hotremove 시 모든 파트 unregister
 *
 * @part_base: hotremove된 base.
 * @tailq:     파트 리스트.
 *
 * base bdev이 사라질 때(예: NVMe surprise removal) 등록된 모든 파티션 vbdev에
 * spdk_bdev_unregister를 발행. 모듈의 fn_table.destruct는 base bdev 사용을
 * 즉시 중단해야 함.
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
/*
 * [한국어]
 * spdk_bdev_part_base_construct_ext - 새 part_base 생성 (base bdev 위에 올라감)
 *
 * @bdev_name:     기반이 될 base bdev 이름.
 * @remove_cb:     base의 hotremove 통지 콜백.
 * @module:        이 part_base를 만드는 모듈.
 * @fn_table:      각 파트에 적용할 fn_table — submit_request 등을 모듈이 채움.
 * @tailq:         모듈 전역 파트 리스트 head.
 * @free_fn:       base 해제 시 모듈 ctx 정리 콜백.
 * @ctx:           모듈별 base 컨텍스트 (free_fn에 전달).
 * @channel_size:  spdk_bdev_part_channel을 포함한 채널 컨텍스트 총 크기.
 * @ch_create_cb:  채널 할당 후 콜백 — 모듈별 추가 초기화.
 * @ch_destroy_cb: 채널 해제 시 콜백.
 * @base:          출력 — 생성된 part_base 핸들.
 * @return:        0 성공 / 음수 errno (base bdev open/claim 실패 등).
 *
 * 동작:
 *   1) bdev_name으로 base bdev open
 *   2) 모듈 명의로 EXCL_WRITE claim
 *   3) part_base 할당 + 컨텍스트 보관
 *   4) 후속 spdk_bdev_part_construct(_ext) 호출 가능 상태로 만듦
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
/*
 * [한국어] struct spdk_bdev_part_construct_opts - 파트 생성 옵션
 *
 * spdk_bdev_part_construct_ext에 전달. 외부에서 UUID를 지정하고 싶을 때 사용
 * (그렇지 않으면 코어가 자동 생성).
 */
struct spdk_bdev_part_construct_opts {
	/* Size of this structure in bytes */
	uint64_t opts_size;           /* [한국어] ABI versioning — opts_init이 sizeof로 채움 */
	/** UUID of the bdev */
	struct spdk_uuid uuid;        /* [한국어] 파트 bdev의 UUID — 영속 식별자 (예: GPT 파티션 UUID 그대로 사용) */
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_part_construct_opts) == 24, "Incorrect size");
                              /* [한국어] ABI 크기 검증 */

/**
 * Initialize options that will be passed to spdk_bdev_part_construct_ext().
 *
 * \param opts Options structure to initialize
 * \param size Size of opts structure.
 */
/*
 * [한국어]
 * spdk_bdev_part_construct_opts_init - 파트 생성 opts 초기화
 *
 * @opts: 초기화할 구조체.
 * @size: sizeof(*opts) — ABI versioning.
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
/*
 * [한국어]
 * spdk_bdev_part_construct - base 위에 파티션 vbdev 하나 생성
 *
 * @part:          모듈이 미리 할당한 spdk_bdev_part (코어가 internal 채움).
 * @base:          소속 part_base.
 * @name:          새 파트의 bdev 이름.
 * @offset_blocks: base bdev 시작점부터 이 파트 시작 LBA까지의 거리 (블록).
 * @num_blocks:    파트 크기 (블록).
 * @product_name:  사용자 표시용 product 이름 (예: "GPT Partition").
 * @return:        0 성공 / -1 base 점유 실패.
 *
 * 동작: part->internal.bdev 채움 → blocklen/io_type_supported 등을 base에서 상속
 *      → spdk_bdev_register 자동 호출. 이후 사용자 I/O는 spdk_bdev_part_submit_request
 *      를 통해 자동 offset 변환 후 base로 forward.
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
/*
 * [한국어]
 * spdk_bdev_part_construct_ext - opts 포함 변형 (UUID 지정 가능)
 *
 * @opts: 추가 옵션 (UUID 등) — NULL이면 코어가 자동 생성.
 *
 * 다른 인자는 spdk_bdev_part_construct와 동일.
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
/*
 * [한국어]
 * spdk_bdev_part_submit_request - 파티션 → base bdev I/O forward
 *
 * @ch:      파트 채널 (spdk_bdev_part_channel) — base_ch 보유.
 * @bdev_io: 사용자가 제출한 I/O.
 * @return:  0 성공 / 음수 실패.
 *
 * 동작:
 *   1) bdev_io->u.bdev.offset_blocks에 part->internal.offset_blocks 자동 가산
 *   2) base_ch를 사용해 base bdev에 동일 종류의 I/O 발행
 *   3) base 완료 시 자동으로 상위 bdev_io 완료 처리
 *
 * 모듈은 fn_table.submit_request에서 단순히 이 함수를 호출하면 됨 — 사용자가
 * 직접 offset_blocks를 미리 가산해서는 안 됨.
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
/*
 * [한국어]
 * spdk_bdev_part_submit_request_ext - 사용자 정의 완료 콜백을 가진 forward
 *
 * @ch / @bdev_io: 위와 동일.
 * @cb:            forward된 child I/O 완료 시 호출 — 콜백 안에서 반드시
 *                 spdk_bdev_io_complete (또는 변형) 호출 필수.
 *
 * 모듈이 forward 후 추가 가공(통계, 로깅, 에러 변환)을 끼우고 싶을 때 사용.
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
/*
 * [한국어]
 * spdk_bdev_part_get_bdev - 파트의 spdk_bdev 객체 반환
 *
 * @part:   파트.
 * @return: part->internal.bdev 포인터 — 코어 등록된 vbdev.
 */
struct spdk_bdev *spdk_bdev_part_get_bdev(struct spdk_bdev_part *part);

/**
 * Return a pointer to this part's base.
 *
 * \param part An spdk_bdev_part object.
 *
 * \return A pointer to this part's spdk_bdev_part_base object.
 */
/*
 * [한국어]
 * spdk_bdev_part_get_base - 파트의 part_base 반환
 *
 * @part:   파트.
 * @return: 소속 part_base 핸들.
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
/*
 * [한국어]
 * spdk_bdev_part_get_base_bdev - 파트가 올라간 하위 base bdev 반환 (편의)
 *
 * @part:   파트.
 * @return: base bdev 포인터 — part_base_get_bdev(part_get_base(part))의 단축.
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
/*
 * [한국어]
 * spdk_bdev_part_get_offset_blocks - 파트의 base 내 시작 LBA 반환
 *
 * @part:   파트.
 * @return: offset_blocks (블록 단위).
 *
 * 주의: I/O 경로에서 직접 호출하지 말 것 — submit_request가 자동 처리. 메타정보
 * 표시 등 비-I/O 컨텍스트에서만 사용.
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
/*
 * [한국어]
 * spdk_bdev_push_media_events - 미디어 관리 이벤트 큐에 push
 *
 * @bdev:       대상 bdev (media_events=true이어야 함).
 * @events:     이벤트 배열 (예: bad block 알림, wear-out 경고).
 * @num_events: 배열 크기.
 * @return:     실제 push된 이벤트 수 (큐 가득찰 수 있음) 또는 음수 errno.
 *
 * 푸시 후 spdk_bdev_notify_media_management로 사용자에게 통지.
 */
int spdk_bdev_push_media_events(struct spdk_bdev *bdev, const struct spdk_bdev_media_event *events,
				size_t num_events);

/**
 * Send SPDK_BDEV_EVENT_MEDIA_MANAGEMENT to all open descriptors that have
 * pending media events.
 *
 * \param bdev Block device
 */
/*
 * [한국어]
 * spdk_bdev_notify_media_management - 큐에 쌓인 미디어 이벤트를 사용자에게 통지
 *
 * @bdev: 대상.
 *
 * 모든 open desc에 SPDK_BDEV_EVENT_MEDIA_MANAGEMENT 전파. 사용자는 콜백에서
 * spdk_bdev_get_media_events로 큐를 비움.
 */
void spdk_bdev_notify_media_management(struct spdk_bdev *bdev);

/*
 * [한국어] spdk_bdev_io_fn - bdev_io에 적용할 함수 typedef
 *
 * @ctx:     for_each_bdev_io 호출 시 전달한 컨텍스트.
 * @bdev_io: 대상 I/O.
 * @return:  0 계속 / 음수 중단.
 */
typedef int (*spdk_bdev_io_fn)(void *ctx, struct spdk_bdev_io *bdev_io);

/*
 * [한국어] spdk_bdev_for_each_io_cb - for_each_bdev_io 완료 콜백 typedef
 *
 * @ctx: 호출자 컨텍스트.
 * @rc:  0 성공 / 음수 errno (fn이 음수 반환했거나 내부 실패).
 */
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
/*
 * [한국어]
 * spdk_bdev_for_each_bdev_io - bdev에 제출된 모든 in-flight I/O에 fn 적용
 *
 * @bdev: 대상 bdev (unregister되지 않도록 모듈이 보장 필요).
 * @ctx:  fn/cb에 전달할 컨텍스트.
 * @fn:   각 bdev_io에 대해 적합한 스레드에서 호출될 함수 (필수).
 * @cb:   전체 순회 완료 시 호출 (필수).
 *
 * 동작:
 *   - 모든 채널을 순회 (spdk_for_each_channel)하며 채널의 io_submitted 리스트 traverse
 *   - 각 채널은 자기 스레드에서 fn 호출 → cross-thread iteration이지만 atomic 처리
 *   - 모든 채널 완료 후 호출 스레드에서 cb 호출
 *
 * 사용 예: abort 매칭, 통계 수집, 디버깅 dump.
 */
void spdk_bdev_for_each_bdev_io(struct spdk_bdev *bdev, void *ctx, spdk_bdev_io_fn fn,
				spdk_bdev_for_each_io_cb cb);

/*
 * [한국어] spdk_bdev_get_current_qd_cb - 현재 QD 측정 결과 콜백 typedef
 *
 * @bdev:       대상.
 * @current_qd: 모든 채널 합산 in-flight I/O 개수.
 * @cb_arg:     호출자 컨텍스트.
 * @rc:         0 성공 / 음수 errno.
 */
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
/*
 * [한국어]
 * spdk_bdev_get_current_qd - bdev의 현재 QD를 비동기 측정
 *
 * @bdev:   대상.
 * @cb_fn:  결과 콜백 (필수).
 * @cb_arg: 콜백 인자.
 *
 * spdk_bdev_get_qd와 달리 QD 샘플링이 비활성이어도 동작 (즉석 측정).
 * 모든 채널을 for_each_channel로 순회하므로 측정 중 추가 submit/complete가 발생할
 * 수 있어 결과는 근사치. 모듈 컨텍스트에서만 호출, bdev unregister 방지 필요.
 */
void spdk_bdev_get_current_qd(struct spdk_bdev *bdev,
			      spdk_bdev_get_current_qd_cb cb_fn, void *cb_arg);

/**
 * Add I/O statistics.
 *
 * \param total The aggregated I/O statistics.
 * \param add The I/O statistics to be added.
 */
/*
 * [한국어]
 * spdk_bdev_add_io_stat - I/O 통계 누적 (total += add)
 *
 * @total: 누적 대상.
 * @add:   가산할 통계.
 *
 * vbdev이 하위 채널 통계를 자기 단위로 합산할 때 사용.
 */
void spdk_bdev_add_io_stat(struct spdk_bdev_io_stat *total, struct spdk_bdev_io_stat *add);

/**
 * Output bdev I/O statistics information to a JSON stream.
 *
 * \param stat The bdev I/O statistics to output.
 * \param w JSON write context.
 */
/*
 * [한국어]
 * spdk_bdev_dump_io_stat_json - I/O 통계를 JSON으로 직렬화
 *
 * @stat: 출력할 통계.
 * @w:    JSON write context.
 *
 * RPC bdev_get_iostat 응답에 사용. 모듈은 자체 통계 추가 시 이 함수를 base
 * 통계에 적용한 뒤 자기 필드 추가.
 */
void spdk_bdev_dump_io_stat_json(struct spdk_bdev_io_stat *stat, struct spdk_json_write_ctx *w);

/**
 * Reset I/O statistics structure.
 *
 * \param stat The I/O statistics to reset.
 * \param mode The mode to reset I/O statistics.
 */
/*
 * [한국어]
 * spdk_bdev_reset_io_stat - I/O 통계 구조체 리셋
 *
 * @stat: 대상 통계.
 * @mode: 리셋 범위 (ALL/MAXMIN 등 enum spdk_bdev_reset_stat_mode).
 *
 * 누적 카운터/지연 시간/QD 최댓값을 모드에 따라 0/초기값으로.
 */
void spdk_bdev_reset_io_stat(struct spdk_bdev_io_stat *stat, enum spdk_bdev_reset_stat_mode mode);

/*
 * [한국어] spdk_bdev_quiesce_cb - quiesce/unquiesce 완료 콜백 typedef
 *
 * @ctx:    호출자 컨텍스트.
 * @status: 0 성공 / 음수 errno.
 *
 * outstanding I/O drain 완료 또는 큐 재개 완료 시 호출됨.
 */
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
/*
 * [한국어]
 * spdk_bdev_quiesce - bdev I/O 일시 정지 (drain + queue)
 *
 * @bdev:   대상 bdev.
 * @module: bdev을 등록한 모듈만 호출 가능 (권한 제어).
 * @cb_fn:  outstanding I/O drain 완료 시 호출 (선택).
 * @cb_arg: 콜백 인자.
 * @return: 0 성공 / 음수 errno.
 *
 * 동작:
 *   1) 신규 I/O를 채널에 큐잉 (제출 보류)
 *   2) outstanding I/O가 모두 완료되기를 대기
 *   3) cb_fn 호출 → 이제 모듈은 안전한 상태에서 메타 작업 수행 가능
 *   4) spdk_bdev_unquiesce 호출 시 큐잉된 I/O 재개
 *
 * 사용 예: 스냅샷 생성, 메타데이터 갱신 중 I/O 정합성 보호.
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
/*
 * [한국어]
 * spdk_bdev_unquiesce - quiesce 해제 (큐 비우고 정상 제출 재개)
 *
 * 인자/리턴은 quiesce와 동일.
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
/*
 * [한국어]
 * spdk_bdev_quiesce_range - 특정 LBA 범위만 quiesce
 *
 * @bdev / @module / @cb_fn / @cb_arg: spdk_bdev_quiesce와 동일.
 * @offset: 정지할 범위 시작 LBA.
 * @length: 정지할 범위 길이 (블록).
 * @return: 0 성공 / 음수 errno.
 *
 * 전체 bdev 정지보다 영향이 작아 부분 작업(예: 특정 lvol blob clone)에 적합.
 * lba_range_tailq에 등록되어 모듈의 internal.quiesced_ranges에 반영됨.
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
/*
 * [한국어]
 * spdk_bdev_unquiesce_range - 특정 LBA 범위 quiesce 해제
 *
 * 위 quiesce_range와 동일한 offset/length로만 호출 가능 (정확 일치 필수).
 */
int spdk_bdev_unquiesce_range(struct spdk_bdev *bdev, struct spdk_bdev_module *module,
			      uint64_t offset, uint64_t length,
			      spdk_bdev_quiesce_cb cb_fn, void *cb_arg);

/*
 *  Macro used to register module for later initialization.
 */
/*
 * [한국어] SPDK_BDEV_MODULE_REGISTER - bdev 모듈 정적 등록 매크로
 *
 * @name:   모듈 식별 토큰 (심볼 이름 생성에 사용 — 다른 모듈과 충돌 금지).
 * @module: spdk_bdev_module 구조체 인스턴스 포인터.
 *
 * 동작 원리:
 *   - GCC __attribute__((constructor))가 main() 진입 전 자동 호출되는 함수를 생성
 *   - 그 함수가 spdk_bdev_module_list_add(module)를 실행
 *   - 결과: 모든 bdev 모듈이 SPDK 부팅 시점에 자동으로 g_bdev_modules 리스트에 등록
 *
 * 사용 예 (module/bdev/nvme/bdev_nvme.c):
 *   static struct spdk_bdev_module nvme_if = { ... };
 *   SPDK_BDEV_MODULE_REGISTER(nvme, &nvme_if)
 *
 * 주의: name은 C 식별자 토큰이어야 함 (따옴표 없음). 동적 모듈 로딩 사용 시
 * dlopen 직후 컨스트럭터 자동 실행으로 동일 메커니즘 동작.
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
