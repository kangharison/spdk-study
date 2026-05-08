/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright 2023 Solidigm All Rights Reserved
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 레이아웃(MD region) 업그레이드 파이프라인 헤더 (ftl_layout_upgrade.h)
 *
 * === 파일의 역할 ===
 * FTL 메타데이터 영역(레이아웃 region)이 펌웨어 업데이트 등으로 새 버전 포맷으로 마이그레이션
 * 되어야 할 때 사용하는 비동기 파이프라인 인터페이스를 정의한다. 한 번에 한 영역씩 처리하며,
 * 각 영역마다 verify 콜백으로 마이그레이션 가능 여부를 검증한 후 upgrade 콜백을 호출해 실제
 * 데이터 변환과 디스크 영속화를 수행한다. 영역별로 등록된 ftl_region_upgrade_desc 배열
 * (n→n+1, n+1→n+2 단계 ...)을 따라 단계적으로 latest_ver까지 끌어올린다.
 * 또한 영역 단위 업그레이드 정책(disabled/enabled/major)을 결정하는 헬퍼와 슈퍼블록 자체
 * 업그레이드(ftl_superblock_upgrade), 전체 레이아웃 검증/덤프(ftl_layout_verify,
 * ftl_upgrade_layout_dump) 함수를 노출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 시작 mngt 단계의 "버전 업그레이드" 서브파이프라인.
 * 호출 체인: ftl_mngt_startup → ftl_superblock_upgrade → ftl_layout_verify →
 *   ftl_layout_upgrade_init_ctx로 영역 선택 → ftl_region_upgrade로 비동기 업그레이드 시작 →
 *   업그레이드 완료 시 ftl_region_upgrade_completed 호출 → 다음 영역 선택 반복.
 * 실행 컨텍스트: SPDK reactor 스레드. ftl_region_upgrade는 비동기(완료는 cb로),
 *   verify/disabled/enabled/superblock_upgrade 같은 동기 함수는 같은 스레드에서 즉시 반환.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: ftl_core.h(spdk_ftl_dev), ftl_layout.h(ftl_layout_region, ftl_layout_region_type).
 * 의존되는 모듈: ftl_mngt_*, ftl_sb.c, ftl_sb_v3.c, ftl_sb_v5.c, ftl_layout_upgrade.c(구현),
 *   영역별 업그레이드 디스크립터(예: ftl_band_upgrade.c, ftl_p2l_upgrade.c).
 * 데이터 흐름: 디스크 옛 버전 메타데이터 → upgrade 콜백 → 새 버전 메타데이터 직렬화 →
 *   bdev write → ftl_region_upgrade_completed로 슈퍼블록 layout descriptor 갱신.
 * 공유 상태: ftl_layout_upgrade_ctx가 한 영역 업그레이드의 모든 상태를 보유.
 *
 * === 주요 함수/구조체 요약 ===
 *   - enum ftl_layout_upgrade_result: 업그레이드 디스패처 진행 상태(CONTINUE/DONE/FAULT).
 *   - ftl_region_upgrade_verify_fn / ftl_region_upgrade_fn: 영역 업그레이드 콜백 시그니처.
 *   - struct ftl_region_upgrade_desc: 한 단계 업그레이드 정의(verify/upgrade/new_version/ctx_size).
 *   - struct ftl_layout_upgrade_desc_list: 한 영역의 latest_ver와 단계 디스크립터 배열.
 *   - struct ftl_layout_upgrade_ctx: 진행 중 한 영역의 업그레이드 컨텍스트.
 *   - ftl_region_upgrade_disabled/enabled/major_upgrade_enabled: 정책 헬퍼.
 *   - ftl_superblock_upgrade: 슈퍼블록 자체 업그레이드(동기).
 *   - ftl_layout_verify: 모든 영역 verify 호출.
 *   - ftl_upgrade_layout_dump: 레이아웃 덤프(영역 겹침 검증 포함).
 *   - ftl_region_upgrade / ftl_region_upgrade_completed: 비동기 영역 업그레이드 시작/종료 페어.
 *   - ftl_layout_upgrade_init_ctx: 다음 업그레이드 대상 영역 선택.
 *   - ftl_layout_upgrade_region_get_latest_version: 특정 영역 타입의 최신 버전 조회.
 */

#ifndef FTL_LAYOUT_UPGRADE_H
#define FTL_LAYOUT_UPGRADE_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "ftl_core.h"
/* [한국어] struct spdk_ftl_dev 및 FTL 코어 타입 정의. */
#include "ftl_layout.h"
/* [한국어] struct ftl_layout_region 및 enum ftl_layout_region_type 정의. */

struct spdk_ftl_dev;
/* [한국어] forward declaration — 위 include로 이미 알려져 있으나 헤더 자체적 선언으로 안전 마진. */
struct ftl_layout_region;
/* [한국어] forward declaration — ftl_layout.h 정의. */
struct ftl_layout_upgrade_ctx;
/* [한국어] forward declaration — 아래에서 정의되지만 콜백 타입 typedef에서 먼저 참조됨. */

enum ftl_layout_upgrade_result {
	/* Continue with the selected region upgrade */
	FTL_LAYOUT_UPGRADE_CONTINUE = 0,
	/* [한국어] 디스패처가 다음 영역을 선택했고 업그레이드를 계속 진행해야 함을 의미.
	 * 사용 예: ftl_layout_upgrade_init_ctx의 정상 반환. */

	/* Layout upgrade done */
	FTL_LAYOUT_UPGRADE_DONE,
	/* [한국어] 모든 영역이 최신 버전이며 더 이상 업그레이드할 영역이 없음을 의미. mngt 다음 단계로 진행. */

	/* Layout upgrade fault */
	FTL_LAYOUT_UPGRADE_FAULT,
	/* [한국어] 업그레이드 도중 복구 불가 오류 발생. mngt 파이프라인이 abort되고 디바이스 마운트 실패. */
};

/* MD region upgrade verify fn: return 0 on success */
typedef int (*ftl_region_upgrade_verify_fn)(struct spdk_ftl_dev *dev,
		struct ftl_layout_region *region);
/* [한국어] 영역 업그레이드 자격 검증 콜백 타입.
 *  - 반환 0: 이 영역은 upgrade 호출 가능.
 *  - 반환 음수: 업그레이드 불가(예: SB가 dirty이거나 SHM clean이 아니거나) — 호출자는 마운트 실패 또는 abort 처리.
 *  실행 컨텍스트: 동기 호출, SPDK reactor 스레드. */

/* MD region upgrade fn: return 0 on success */
typedef int (*ftl_region_upgrade_fn)(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx);
/* [한국어] 영역 업그레이드 본체 콜백 타입.
 *  - 반환 0: 비동기 업그레이드 시작 성공 — 완료 시 ftl_region_upgrade_completed가 호출됨.
 *  - 반환 음수: 즉시 실패 — 호출자가 mngt 파이프라인을 abort.
 *  실행 컨텍스트: SPDK reactor 스레드. 콜백 내부에서 bdev I/O 등 비동기 작업 발행 가능. */

/* MD region upgrade descriptor */
struct ftl_region_upgrade_desc {
	/* Qualifies the region for upgrade */
	ftl_region_upgrade_verify_fn verify;
	/* [한국어] 이 영역을 이 단계로 업그레이드해도 안전한지 검증하는 콜백 포인터.
	 * 설정자: 영역별 업그레이드 모듈이 정적으로 등록. 읽는 자: ftl_layout_verify가 모든 영역에 호출.
	 * 값 범위: 유효 함수 포인터 또는 NULL(검증 없음 시).
	 * 동기화: 정적 등록 후 read-only. */

	/* Upgrades the region */
	ftl_region_upgrade_fn upgrade;
	/* [한국어] 실제 데이터 변환과 영속화를 수행하는 비동기 콜백 포인터.
	 * 설정자: 영역별 모듈이 정적 등록. 읽는 자: ftl_region_upgrade가 호출.
	 * 값 범위: 유효 함수 포인터(필수). */

	/* New region version (i.e. after the upgrade) */
	uint32_t new_version;
	/* [한국어] 이 단계 업그레이드 후 해당 영역이 갖게 될 새 버전 번호.
	 * 설정자: 정적 등록 시. 읽는 자: ftl_region_upgrade_completed가 슈퍼블록 layout 갱신 시 사용. */

	/* Context buffer allocated for upgrade() */
	size_t ctx_size;
	/* [한국어] upgrade 콜백이 사용할 컨텍스트 버퍼 크기(바이트).
	 * 설정자: 정적 등록. 읽는 자: ftl_layout_upgrade_init_ctx가 calloc(ctx_size)로 할당해 ctx 멤버에 저장. */
};

/* MD layout upgrade descriptor - one instance contains information about the upgrade steps necessary
 * for updating to latest metadata version. For example version 0 of metadata region will have no descriptors
 * (as it's by definition up to date and there's no need to upgrade it), while version 2 will have 2 (0 -> 1
 * and 1 -> 2).
 */
struct ftl_layout_upgrade_desc_list {
	/* Latest version of the region */
	uint64_t latest_ver;
	/* [한국어] 이 영역 타입의 최신 버전 번호. 모든 영역이 이 값까지 끌어올려져야 함.
	 * 설정자: 영역별 모듈 컴파일 시점 상수. 읽는 자: 진행 디스패처가 종료 조건 비교에 사용.
	 * 값 범위: 0..N 단조 증가. */

	/* # of entries in the region upgrade descriptor */
	size_t count;
	/* [한국어] desc 배열의 단계 수(latest_ver + 1과 같지 않을 수 있음 — 미사용 버전 건너뛰기 가능).
	 * 설정자: 정적 등록. 읽는 자: 디스패처가 desc 배열 인덱싱 시 한계로 사용. */

	/* Region upgrade descriptor */
	struct ftl_region_upgrade_desc *desc;
	/* [한국어] 단계별 업그레이드 디스크립터 배열 포인터. desc[i]는 버전 i → desc[i].new_version 변환을 정의.
	 * 설정자: 영역별 모듈 정적 배열 주소. 읽는 자: 디스패처. 동기화: read-only after 정적 등록. */
};

/* Region upgrade callback */
typedef void (*ftl_region_upgrade_cb)(struct spdk_ftl_dev *dev, void *ctx, int status);
/* [한국어] 영역 업그레이드 완료를 외부(mngt 파이프라인)에 알리는 콜백 타입.
 *  - status: 0 성공, 음수 실패. 호출 컨텍스트: SPDK reactor 스레드.
 *  - ctx: cb 등록 시 같이 전달된 cb_ctx — 호출자 측 상태 복원에 사용. */

/* MD layout upgrade context */
struct ftl_layout_upgrade_ctx {
	/* MD region being upgraded */
	struct ftl_layout_region *reg;
	/* [한국어] 현재 업그레이드 중인 layout 영역 포인터.
	 * 설정자: ftl_layout_upgrade_init_ctx가 다음 영역 선택 시. 읽는 자: upgrade 콜백.
	 * 동기화: 한 ctx는 한 시점에 한 영역만 처리하므로 별도 락 불필요. */

	/* MD region upgrade descriptor */
	struct ftl_layout_upgrade_desc_list *upgrade;
	/* [한국어] 이 영역에 대응하는 desc list 포인터(전역 정적 데이터).
	 * 설정자: init_ctx가 영역 type을 보고 매핑. 읽는 자: 디스패처가 desc 배열 인덱싱에 사용. */

	/* New region version (i.e. after the upgrade) */
	uint64_t next_reg_ver;
	/* [한국어] 이번 단계 완료 시 영역이 갖게 될 새 버전 번호.
	 * 설정자: init_ctx 또는 다음 단계 진입 시. 읽는 자: ftl_region_upgrade_completed가 슈퍼블록 layout 갱신에 사용. */

	/* Context buffer for the region upgrade */
	void *ctx;
	/* [한국어] 단계별 desc->ctx_size 만큼 calloc된 컨텍스트 버퍼.
	 * 설정자: init_ctx. 읽는 자/소유: upgrade 콜백 — 비동기 진행 중 상태 보관에 자유롭게 사용.
	 * 해제 시점: ftl_region_upgrade_completed 직후 init_ctx가 다음 단계로 재할당하거나 종료 시 free. */

	/* Region upgrade callback */
	ftl_region_upgrade_cb cb;
	/* [한국어] 한 영역 업그레이드 완료 시 호출될 외부 콜백.
	 * 설정자: 호출자(mngt)가 init_ctx 직전에 등록. 읽는 자: ftl_region_upgrade_completed. */

	/* Ctx for the region upgrade callback */
	void *cb_ctx;
	/* [한국어] cb 호출 시 두 번째 인자로 그대로 전달될 호출자 컨텍스트 포인터.
	 * 설정자: mngt가 등록. 읽는 자: cb 콜백 본체. */
};

/**
 * @brief Disable region upgrade for particular version.
 *
 * @param dev FTL device
 * @param region FTL layout region descriptor
 * @return int -1
 */
/*
 * [한국어]
 * ftl_region_upgrade_disabled - 특정 영역 업그레이드를 명시적으로 차단하는 verify 콜백
 *
 * @param dev: FTL 디바이스(미사용 — 시그니처 호환용).
 * @param region: 검증 대상 영역(미사용).
 * @return: 항상 -1 — "이 단계 업그레이드 불가" 표시.
 *
 * 동기/배경: 일부 영역은 특정 버전 사이의 업그레이드를 지원하지 않으므로, desc 테이블에 이 함수를
 *   verify 슬롯으로 등록해 verify 단계에서 즉시 -1 반환을 강제한다.
 * 실행 컨텍스트: ftl_layout_verify가 동기 호출.
 */
int ftl_region_upgrade_disabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region);

/**
 * @brief Enable region upgrade for particular version.
 *
 * @param dev FTL device
 * @param region FTL layout region descriptor
 * @return int -1 (i.e. disable) if SB is dirty or SHM clean, 0 otherwise (i.e. enable)
 */
/*
 * [한국어]
 * ftl_region_upgrade_enabled - 특정 영역의 일반 업그레이드 가능 여부를 SB clean/SHM 상태로 판단하는 verify 콜백
 *
 * @param dev: FTL 디바이스 — 슈퍼블록 clean 플래그와 SHM 상태 검사에 사용.
 * @param region: 검증 대상 영역.
 * @return: 0 = 업그레이드 가능, -1 = SB가 dirty거나 SHM이 clean하지 않아 업그레이드 불가.
 *
 * 동기/배경: 정상 종료된 디바이스만 안전하게 업그레이드되도록 보장. dirty 데이터가 남아 있으면
 *   먼저 복구 후 재시도해야 한다.
 */
int ftl_region_upgrade_enabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region);

/**
 * @brief Enable major upgrade for particular region.
 *
 * @param dev FTL device
 * @param region the region to be upgraded in major mode
 *
 * @retval 0 Upgrade enabled and possible
 * @retval -1 Upgrade not possible
 */
/*
 * [한국어]
 * ftl_region_major_upgrade_enabled - 영역의 메이저 버전 업그레이드 가능 여부 판단 verify 콜백
 *
 * @param dev: FTL 디바이스.
 * @param region: 업그레이드 대상.
 * @return: 0 = 메이저 업그레이드 가능, -1 = 불가.
 *
 * 동기/배경: 메이저 업그레이드는 데이터 호환성이 크게 깨지므로 별도 정책 함수로 분리. 일반 enabled보다
 *   더 엄격한 조건(예: upgrade_ready 플래그 포함)을 검사.
 */
int ftl_region_major_upgrade_enabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region);

/**
 * @brief Upgrade the superblock.
 *
 * This call is synchronous.
 *
 * @param dev FTL device
 * @return int 0: success, error code otherwise
 */
/*
 * [한국어]
 * ftl_superblock_upgrade - 슈퍼블록 자체를 새 버전으로 동기 업그레이드
 *
 * @param dev: FTL 디바이스.
 * @return: 0 = 성공, 음수 = 에러.
 *
 * 동기/배경: 영역 업그레이드 전에 슈퍼블록 자체가 새 버전이어야 layout 정보 해석이 가능.
 *   이 함수는 동기적으로 슈퍼블록을 v3 → v5 등으로 변환 후 영속화한다.
 * 실행 컨텍스트: 디바이스 시작 mngt 단계, SPDK reactor 스레드.
 */
int ftl_superblock_upgrade(struct spdk_ftl_dev *dev);

/**
 * @brief Qualify the MD layout for upgrade.
 *
 * The SB MD layout is built or loaded.
 * If loaded, walk through all MD layout and filter out all MD regions that need an upgrade.
 * Call .verify() on region upgrade descriptors for all such regions.
 *
 * @param dev FTL device
 * @return int 0: success, error code otherwise
 */
/*
 * [한국어]
 * ftl_layout_verify - 모든 MD 레이아웃 영역을 순회하며 업그레이드 가능 여부 일괄 검증
 *
 * @param dev: FTL 디바이스 — dev->layout 영역 배열 사용.
 * @return: 0 = 모든 영역 검증 통과, 음수 = 한 영역이라도 verify 실패.
 *
 * 동기/배경: 업그레이드 본격 시작 전, 모든 영역이 안전하게 마이그레이션될 수 있는지 사전 검증해야
 *   중간에 abort되어 디바이스가 불일치 상태로 남는 사고를 막을 수 있다.
 * 동작: dev->layout 모든 region에 대해 desc->verify() 호출 → 한 번이라도 실패하면 즉시 반환.
 */
int ftl_layout_verify(struct spdk_ftl_dev *dev);

/**
 * @brief Dump the FTL layout
 *
 * Verify MD layout in terms of region overlaps.
 *
 * @param dev FTL device
 * @return int 0: success, error code otherwise
 */
/*
 * [한국어]
 * ftl_upgrade_layout_dump - 현재 MD 레이아웃을 로그로 덤프하면서 영역 겹침까지 검증
 *
 * @param dev: FTL 디바이스.
 * @return: 0 = 영역 겹침 없음, 음수 = 영역이 디스크 상에서 겹침(레이아웃 손상).
 *
 * 동기/배경: 디버그 목적과 일관성 검증을 동시에 — 영역들의 [blk_offs, blk_offs+blk_sz) 범위가
 *   서로 겹치면 데이터 손상 위험이므로 마운트 거부.
 */
int ftl_upgrade_layout_dump(struct spdk_ftl_dev *dev);

/**
 * @brief Upgrade the MD region.
 *
 * Call .upgrade() on the selected region upgrade descriptor.
 * When returned 0, this call is asynchronous.
 * The .upgrade() is expected to convert and persist the metadata.
 * When that's done, the call to ftl_region_upgrade_completed() is expected
 * to continue with the layout upgrade.
 *
 * When returned an error code, the caller is responsible to abort the mngt pipeline.
 *
 * @param dev FTL device
 * @param ctx Layout upgrade context
 * @return int 0: upgrade in progress, error code otherwise
 */
/*
 * [한국어]
 * ftl_region_upgrade - 한 영역의 한 단계 업그레이드를 비동기 시작
 *
 * @param dev: FTL 디바이스.
 * @param ctx: 업그레이드 컨텍스트(reg/upgrade/next_reg_ver/ctx/cb/cb_ctx 모두 채워져 있어야 함).
 * @return: 0 = 비동기 시작 성공(완료는 ftl_region_upgrade_completed로 통보), 음수 = 즉시 실패.
 *
 * 동기/배경: 영역 데이터를 새 포맷으로 변환하고 영속화하는 작업은 bdev I/O를 수반하므로 비동기.
 *   호출자는 0을 받으면 다음 cb 호출까지 mngt 파이프라인을 일시 정지.
 * 실행 컨텍스트: SPDK reactor 스레드 — 내부 upgrade 콜백이 bdev 작업을 발행한다.
 *
 * 호출 체인:
 *   ftl_mngt_upgrade → [이 함수] → desc->upgrade() → bdev write → ftl_region_upgrade_completed → ctx->cb
 */
int ftl_region_upgrade(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx);

/**
 * @brief Called when MD region upgrade is completed - see ftl_region_upgrade().
 *
 * Upgrades the SB MD layout and region's prev version descriptor to the just upgraded version.
 * Executes the layout upgrade owner's callback (mngt region_upgrade_cb()) to continue
 * with the layout upgrade.
 *
 * @param dev FTL device
 * @param ctx Layout upgrade context
 * @param entry_size Entry size in the upgraded region or 0 if no change
 * @param num_entries Number of entries in the upgraded region or 0 if no change
 * @param status Region upgrade status: 0: success, error code otherwise
 */
/*
 * [한국어]
 * ftl_region_upgrade_completed - 영역 업그레이드 단계 완료 통보 + 슈퍼블록 layout 갱신
 *
 * @param dev: FTL 디바이스.
 * @param ctx: 진행 중이던 업그레이드 컨텍스트.
 * @param entry_size: 업그레이드 후 영역의 엔트리 크기(0이면 변화 없음).
 * @param num_entries: 업그레이드 후 영역의 엔트리 개수(0이면 변화 없음).
 * @param status: 0 = 성공, 음수 = 실패.
 *
 * 동기/배경: upgrade 콜백 본체가 모든 비동기 I/O를 마치고 호출하는 종결 함수.
 *   슈퍼블록의 MD 레이아웃 영역 디스크립터를 새 버전/크기로 갱신하고, ctx->cb를 호출해
 *   상위 mngt 파이프라인이 다음 영역으로 진행할 수 있게 한다.
 *
 * 호출 체인:
 *   desc->upgrade() 본체(완료 시점) → [이 함수] → 슈퍼블록 layout 갱신 → ctx->cb(dev, cb_ctx, status)
 */
void ftl_region_upgrade_completed(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx,
				  uint64_t entry_size, uint64_t num_entries, int status);

/**
 * @brief Initialize the layout upgrade context.
 *
 * Select the next region to be upgraded.
 *
 * @param dev FTL device
 * @param ctx Layout upgrade context
 * @return int see enum ftl_layout_upgrade_result
 */
/*
 * [한국어]
 * ftl_layout_upgrade_init_ctx - 다음 업그레이드 대상 영역을 선택하고 ctx를 초기화
 *
 * @param dev: FTL 디바이스 — 모든 layout 영역을 순회.
 * @param ctx: 초기화할 업그레이드 컨텍스트(reg, upgrade, next_reg_ver, ctx 버퍼 갱신됨).
 * @return: ftl_layout_upgrade_result 값.
 *   - CONTINUE: 다음 대상 발견 — ftl_region_upgrade로 진행.
 *   - DONE: 모든 영역 최신 — 파이프라인 종료.
 *   - FAULT: 손상/모순 — abort.
 *
 * 동기/배경: 한 영역을 단계별로 latest_ver까지 끌어올린 뒤 다음 영역으로 진행하는 디스패처.
 */
int ftl_layout_upgrade_init_ctx(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx);

/**
 * @brief Returns the highest defined version of the given region
 *
 * @param reg_type FTL layout region
 *
 * @return region's version
 */
/*
 * [한국어]
 * ftl_layout_upgrade_region_get_latest_version - 영역 타입의 최신 버전 번호 조회
 *
 * @param reg_type: ftl_layout_region_type enum 값.
 * @return: 해당 영역 타입의 latest_ver(struct ftl_layout_upgrade_desc_list::latest_ver).
 *
 * 동기/배경: 새 디바이스 빌드 시 영역에 부여할 버전을 결정할 때 사용.
 */
uint64_t ftl_layout_upgrade_region_get_latest_version(enum ftl_layout_region_type reg_type);

#endif /* FTL_LAYOUT_UPGRADE_H */
/* [한국어] 헤더 가드 종료. */
