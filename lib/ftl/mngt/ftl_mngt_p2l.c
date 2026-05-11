/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright 2023 Solidigm All Rights Reserved
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL P2L(Physical-to-Logical) checkpoint 관리 단계 (ftl_mngt_p2l.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 FTL의 P2L(Physical-to-Logical) checkpoint 메커니즘 — 즉, 각 밴드(band)에
 * 기록된 데이터의 PBA→LBA 역매핑 정보를 일정 주기로 디스크 MD region에 보존하는
 * 체크포인트 — 의 라이프사이클(init/deinit/wipe/restore) step들을 ftl_mngt 상태 머신용
 * 콜백 형태로 제공한다. P2L checkpoint는 dirty shutdown 후 recovery 단계에서 L2P를
 * 빠르게 재구성하는 핵심이며, 또한 P2L IO log(쓰기 직후 즉시 기록되는 더 세밀한 로그)도
 * 별도로 wipe할 수 있게 함수가 분리되어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트: 모두 FTL 코어 spdk_thread. ftl_mngt 상태 머신이 startup/shutdown/
 * recovery 파이프라인 단계 중 P2L 관련 단계에서 본 파일의 함수를 호출.
 *   호출 체인 (startup, fresh):
 *     startup desc → ftl_mngt_p2l_init_ckpt → ftl_p2l_ckpt_init →
 *       ftl_mngt_p2l_wipe → (각 P2L MD region에 대해 ftl_md_clear 비동기 발행) →
 *       콜백 체이닝으로 마지막 region까지 완료 → ftl_mngt_next_step
 *   호출 체인 (recovery):
 *     recovery desc → ftl_mngt_p2l_restore_ckpt → 모든 P2L MD region에
 *       ftl_md_restore 동시 발행 → ftl_mngt_p2l_restore_ckpt_cb로 카운트 →
 *       모두 완료되면 next/fail step
 *   호출 체인 (shutdown): ftl_mngt_p2l_deinit_ckpt → ftl_p2l_ckpt_deinit
 *
 * === 타 모듈과의 연결 ===
 * - 의존: lib/ftl/ftl_internal.h (P2L checkpoint API: ftl_p2l_ckpt_init/deinit),
 *   lib/ftl/ftl_core.h (spdk_ftl_dev, ftl_layout, FTL_LAYOUT_REGION_TYPE_P2L_*),
 *   lib/ftl/utils/ftl_md.* (ftl_md_clear, ftl_md_restore, ftl_md_free_buf,
 *   ftl_md_destroy_region_flags) — 모든 비동기 MD I/O를 추상화한 공통 레이어.
 * - 의존받음: lib/ftl/mngt/ftl_mngt_startup.c (init/wipe), ftl_mngt_shutdown.c (deinit),
 *   ftl_mngt_recovery.c (restore_ckpt, free_bufs, p2l_log_io_wipe).
 * - 데이터 흐름:
 *   write 경로에서 ftl_p2l_ckpt가 P2L 항목을 메모리 슬롯에 누적 → 일정 시점에
 *   디스크 MD region(P2L_CKPT_*)에 flush → recovery 시 본 파일의 restore_ckpt가
 *   디스크 region을 메모리로 다시 읽고 → 이후 ftl_mngt_recovery가 LBA→PBA 매핑
 *   재구성에 활용.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ftl_mngt_p2l_md_ctx : 다중 region 순회 상태(현재 region, 범위, 누적 status).
 * - ftl_mngt_p2l_init_ckpt()   : P2L checkpoint 모듈 초기화(메모리 슬롯/큐 준비).
 * - ftl_mngt_p2l_deinit_ckpt() : P2L 모듈 해제.
 * - ftl_p2l_wipe_md_region()   : 단일 region을 비동기 클리어 후 다음 region 체이닝.
 * - ftl_mngt_p2l_wipe_range()  : [min..max] region 범위 일괄 wipe 시작.
 * - ftl_mngt_p2l_wipe()        : P2L_CKPT 전 region wipe (첫 포맷용).
 * - ftl_mngt_p2l_log_io_wipe() : P2L IO log 전 region wipe (P2L log 옵션 활성 시만).
 * - ftl_mngt_p2l_free_bufs()   : P2L 메모리 버퍼 해제 (recovery 후 메모리 회수).
 * - ftl_mngt_p2l_restore_ckpt(): 모든 P2L checkpoint region 동시 복원(recovery 시).
 */

#include "ftl_mngt.h"
/* [한국어] ftl_mngt_process / next_step / fail_step / alloc_step_ctx / get_step_ctx /
 * skip_step API 사용에 필요. */
#include "ftl_mngt_steps.h"
/* [한국어] 본 파일에서 정의·노출할 step 함수들의 프로토타입. */
#include "ftl_internal.h"
/* [한국어] ftl_p2l_ckpt_init / ftl_p2l_ckpt_deinit / ftl_md_* 같은 FTL 내부 API. */
#include "ftl_core.h"
/* [한국어] spdk_ftl_dev / dev->layout / ftl_fast_startup() 등. */

/*
 * [한국어]
 * struct ftl_mngt_p2l_md_ctx - 다중 P2L MD region 순회용 step context.
 *
 * 사용 패턴: wipe / restore 같이 여러 region에 걸쳐 비동기 I/O를 발행해야 하는
 * 경우, ftl_mngt_alloc_step_ctx로 본 구조체를 step에 묶어두고 콜백 사이의 진행
 * 상태를 보존한다. 콜백 체인 끝에 ftl_mngt_next_step / fail_step을 호출.
 */
struct ftl_mngt_p2l_md_ctx {
	struct ftl_mngt_process *mngt;
	/* [한국어] 현재 진행 중인 ftl_mngt 핸들 — 콜백에서 next/fail step 트리거에 사용.
	 * 설정자: 각 step 진입부에서 mngt 포인터로 채움.
	 * 읽는 자: ftl_p2l_wipe_md_region_cb / ftl_mngt_p2l_restore_ckpt_cb.
	 * 값 범위: 유효한 ftl_mngt_process 포인터(NULL 불가).
	 * 동기화: 코어 스레드 단일 — 별도 락 불필요. */

	int md_region;
	/* [한국어] 현재 처리 중인 P2L region index (FTL_LAYOUT_REGION_TYPE_P2L_*).
	 * 설정자: wipe step의 진입부에서 min으로 초기화, 콜백에서 ++로 진행.
	 *         restore step에서는 진입부에서 0으로 시작, 콜백에서 카운터 용도로 ++.
	 * 읽는 자: 콜백/wipe 함수에서 dev->layout.md[md_region]에 접근.
	 * 값 범위: [md_region_min .. md_region_max] (wipe), [0 .. P2L_COUNT] (restore).
	 * 동기화: 단일 콜백 체인이므로 경쟁 없음. */

	int md_region_min;
	/* [한국어] wipe 범위의 시작 region index (포함).
	 * 설정자: ftl_mngt_p2l_wipe_range()에서 인자 min으로 셋팅.
	 * 읽는 자: ftl_p2l_wipe_md_region()에서 assert로 검증.
	 * 값 범위: P2L_CKPT_MIN 또는 P2L_LOG_IO_MIN.
	 * 동기화: 한 번 셋팅 후 read-only. */

	int md_region_max;
	/* [한국어] wipe 범위의 끝 region index (포함). 콜백에서 == max이면 next_step.
	 * 설정자: ftl_mngt_p2l_wipe_range()에서 인자 max로 셋팅.
	 * 읽는 자: ftl_p2l_wipe_md_region_cb()에서 종료 조건 비교.
	 * 값 범위: P2L_CKPT_MAX 또는 P2L_LOG_IO_MAX.
	 * 동기화: read-only. */

	int status;
	/* [한국어] restore_ckpt 시 모든 region의 비동기 결과를 누적하는 누적 status.
	 * 설정자: 콜백에서 status != 0이면 갱신(첫 에러를 보존).
	 * 읽는 자: 마지막 콜백에서 0이 아니면 fail_step 호출.
	 * 값 범위: 0 또는 음수 errno.
	 * 동기화: 다중 region이 병렬 발행되지만 코어 스레드 단일 콜백이라 atomic 불필요. */
};

static void ftl_p2l_wipe_md_region(struct spdk_ftl_dev *dev, struct ftl_mngt_p2l_md_ctx *ctx);
/* [한국어] forward 선언 — 콜백에서 호출하므로 위에서 미리 선언. */

/*
 * [한국어]
 * ftl_mngt_p2l_init_ckpt - P2L checkpoint 모듈을 메모리 자료구조로 초기화.
 *
 * @dev:  대상 FTL 디바이스. dev->layout이 이미 결정되어 있어야 함.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: write 경로에서 P2L 항목을 누적할 메모리 슬롯/큐, 그리고 디스크 region에
 * 발행할 비동기 I/O 컨텍스트가 미리 준비되어 있어야 한다.
 *
 * 동작: ftl_p2l_ckpt_init(dev) (동기 함수). 0=성공/비-0=실패.
 *
 * 호출 체인:
 *   startup desc → ftl_mngt_p2l_init_ckpt → ftl_p2l_ckpt_init → next/fail
 */
void
ftl_mngt_p2l_init_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (!ftl_p2l_ckpt_init(dev)) {
		/* [한국어] 0 반환 = 성공. 다음 step으로 진행. */
		ftl_mngt_next_step(mngt);
	} else {
		/* [한국어] 비-0 = 메모리 부족 등 실패. 롤백 트리거. */
		ftl_mngt_fail_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_p2l_deinit_ckpt - P2L checkpoint 모듈 해제.
 *
 * @dev:  대상 FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: 셧다운 시 P2L 슬롯/큐 메모리 회수.
 *
 * 동작: ftl_p2l_ckpt_deinit (실패 없음) 후 next_step.
 *
 * 호출 체인: shutdown desc → ftl_mngt_p2l_deinit_ckpt → next
 */
void
ftl_mngt_p2l_deinit_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_p2l_ckpt_deinit(dev);
	/* [한국어] 슬롯/큐/IO 컨텍스트 등 인메모리 자원 해제. */
	ftl_mngt_next_step(mngt);
	/* [한국어] 항상 성공. */
}

/*
 * [한국어]
 * ftl_p2l_wipe_md_region_cb - 단일 P2L region wipe 완료 콜백, 다음 region을 체이닝.
 *
 * @dev:    FTL 디바이스.
 * @md:     완료된 ftl_md 핸들. md->owner.cb_ctx에서 우리 ctx 복원.
 * @status: 0=성공, 비-0=clear 실패.
 *
 * 왜 필요한가: wipe는 region을 하나씩 순차 처리한다(병렬 발행하면 디스크 큐 폭주).
 * 한 region이 끝날 때마다 본 콜백이 호출되어 다음 region에 ftl_md_clear를 발행하고,
 * 마지막 region 완료 시 ftl_mngt_next_step으로 step 종료를 신호한다.
 *
 * 동작:
 *  1) ctx 복원, status 검사 — 실패 시 즉시 fail_step.
 *  2) md_region == md_region_max 이면 모두 끝 — next_step.
 *  3) 아니면 md_region을 ++하고 ftl_p2l_wipe_md_region 재호출.
 *
 * 실행 컨텍스트: ftl_md 비동기 I/O 완료 시 코어 스레드에서 호출.
 *
 * 호출 체인:
 *   ftl_md_clear(...) 완료 → ftl_p2l_wipe_md_region_cb →
 *     [next region 있으면] ftl_p2l_wipe_md_region (재귀적 체이닝) /
 *     [없으면] ftl_mngt_next_step
 */
static void
ftl_p2l_wipe_md_region_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_p2l_md_ctx *ctx = md->owner.cb_ctx;
	/* [한국어] ftl_md->owner.cb_ctx는 발행 시 ctx로 셋팅 — 콜백 컨텍스트 복원. */

	if (status) {
		/* [한국어] 비동기 clear 실패(디스크 write 오류 등). 즉시 step 실패 보고. */
		ftl_mngt_fail_step(ctx->mngt);
		return;
	}

	if (ctx->md_region == ctx->md_region_max) {
		/* [한국어] 마지막 region까지 wipe 완료 — 다음 step으로 진행. */
		ftl_mngt_next_step(ctx->mngt);
		return;
	}

	ctx->md_region++;
	/* [한국어] 다음 P2L region index로 진행. */
	ftl_p2l_wipe_md_region(dev, ctx);
	/* [한국어] 다음 region에 대해 ftl_md_clear 발행 — 콜백 체인 계속. */
}

/*
 * [한국어]
 * ftl_p2l_wipe_md_region - 단일 P2L MD region에 대해 비동기 ftl_md_clear 발행.
 *
 * @dev: FTL 디바이스.
 * @ctx: 진행 중인 wipe step context. ctx->md_region이 처리 대상.
 *
 * 왜 필요한가: ftl_p2l_wipe_md_region_cb에서 다음 region으로 체인을 이어가는 작업
 * 단위. dev->layout.md[ctx->md_region]에서 핸들을 가져와 owner와 cb를 셋팅한 뒤
 * ftl_md_clear를 호출한다.
 *
 * 동작:
 *  1) layout.md[ctx->md_region] 핸들 획득. NULL이면 fail_step (layout 손상).
 *  2) md->owner.cb_ctx = ctx, md->cb = wipe_md_region_cb 셋팅 — 콜백 라우팅.
 *  3) ftl_md_clear(md, 0, NULL) 호출 — region 전체를 0으로 비동기 fill.
 *
 * 실행 컨텍스트: 코어 스레드. wipe step 진입 시 또는 wipe_cb 안에서 재귀 호출됨.
 */
static void
ftl_p2l_wipe_md_region(struct spdk_ftl_dev *dev, struct ftl_mngt_p2l_md_ctx *ctx)
{
	struct ftl_layout *layout = &dev->layout;
	/* [한국어] dev->layout는 모든 MD region 핸들 배열을 보유. */
	struct ftl_md *md = layout->md[ctx->md_region];
	/* [한국어] 처리 대상 region의 MD 핸들. */

	assert(ctx->md_region >= ctx->md_region_min);
	/* [한국어] 디버그 빌드에서 진행 범위 위반 검출(min 미만). */
	assert(ctx->md_region <= ctx->md_region_max);
	/* [한국어] 디버그 빌드에서 max 초과 검출. */

	if (!md) {
		/* [한국어] layout에 해당 region MD가 없음 = layout 초기화 실패 또는
		 * 미지원 region. step을 실패시킨다. */
		ftl_mngt_fail_step(ctx->mngt);
		return;
	}

	md->owner.cb_ctx = ctx;
	/* [한국어] 콜백에서 본 ctx를 복원할 수 있도록 owner에 저장. */
	md->cb = ftl_p2l_wipe_md_region_cb;
	/* [한국어] ftl_md_clear 완료 시 호출될 콜백 함수 포인터 등록. */
	ftl_md_clear(md, 0, NULL);
	/* [한국어] region 전체를 패턴 0으로 채우는 비동기 디스크 write 발행.
	 * pattern=0, vss(per-block 메타) NULL — P2L MD는 vss 없음. */
}

/*
 * [한국어]
 * ftl_mngt_p2l_wipe_range - [min..max] 범위의 P2L region을 순차 wipe 시작.
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 * @min:  wipe 시작 region index (FTL_LAYOUT_REGION_TYPE_P2L_*_MIN).
 * @max:  wipe 끝 region index (FTL_LAYOUT_REGION_TYPE_P2L_*_MAX).
 *
 * 왜 필요한가: P2L checkpoint(P2L_CKPT_*)와 P2L IO log(P2L_LOG_IO_*)는 region 그룹이
 * 다르지만 wipe 로직은 동일하므로 공통화한 헬퍼.
 *
 * 동작:
 *  1) step ctx 할당(ftl_mngt_p2l_md_ctx). 실패 시 fail_step.
 *  2) ctx에 mngt/min/max 셋팅, md_region = min으로 초기화.
 *  3) ftl_p2l_wipe_md_region 호출로 첫 region wipe 시작 — 이후 콜백 체인이 자동 진행.
 *
 * 실행 컨텍스트: 코어 스레드, step action.
 */
static void
ftl_mngt_p2l_wipe_range(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt, int min, int max)
{
	struct ftl_mngt_p2l_md_ctx *ctx;

	if (ftl_mngt_alloc_step_ctx(mngt, sizeof(struct ftl_mngt_p2l_md_ctx))) {
		/* [한국어] step ctx 할당 실패(메모리 부족). 즉시 step 실패. */
		ftl_mngt_fail_step(mngt);
		return;
	}
	ctx = ftl_mngt_get_step_ctx(mngt);
	/* [한국어] 방금 할당된 step ctx 포인터 획득. */
	ctx->mngt = mngt;
	/* [한국어] 콜백에서 next/fail 호출용 mngt 핸들 저장. */
	ctx->md_region_min = min;
	/* [한국어] wipe 범위 시작. */
	ctx->md_region_max = max;
	/* [한국어] wipe 범위 끝. */
	ctx->md_region = ctx->md_region_min;
	/* [한국어] 첫 region부터 시작. */
	ftl_p2l_wipe_md_region(dev, ctx);
	/* [한국어] 첫 region wipe 발행. 이후 콜백 체인이 max까지 진행. */
}

/*
 * [한국어]
 * ftl_mngt_p2l_wipe - P2L checkpoint MD 영역 전체 wipe (첫 포맷용).
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: 새로 포맷된 디바이스는 P2L checkpoint region에 잔여 데이터가 있을 수
 * 있어 0으로 클리어 해야 향후 ckpt restore가 잘못된 데이터를 읽지 않는다.
 *
 * 호출 체인: startup(fresh) desc → ftl_mngt_p2l_wipe → ftl_mngt_p2l_wipe_range
 *            → 콜백 체인으로 모든 P2L_CKPT region 클리어
 */
void
ftl_mngt_p2l_wipe(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_p2l_wipe_range(dev, mngt, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN,
				FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX);
	/* [한국어] P2L checkpoint region 그룹 [MIN..MAX] 전체를 wipe. */
}

/*
 * [한국어]
 * ftl_mngt_p2l_log_io_wipe - P2L IO log MD 영역 wipe (P2L log 옵션 활성 시만).
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: P2L IO log 옵션을 켠 빌드에서만 P2L_LOG_IO region이 존재한다. region이
 * 없거나 크기가 0이면 wipe 자체를 건너뛰어야 한다(skip_step).
 *
 * 동작:
 *  1) P2L_LOG_IO_MIN..MAX 순회하며 region.current.blocks > 0 인 region이 하나라도
 *     있는지 확인 — 있으면 wipe 필요.
 *  2) 없으면 skip_step (이 step 건너뜀).
 *  3) 있으면 ftl_mngt_p2l_wipe_range로 일괄 wipe.
 */
void
ftl_mngt_p2l_log_io_wipe(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout_region *region;
	bool wipe = false;
	/* [한국어] wipe 필요 여부 플래그. */

	/* Check if P2L IO logs are enabled and we have to clear this MD and region */
	for (int i = FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MIN; i <= FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MAX;
	     i++) {
		/* [한국어] P2L IO log region 그룹 전체를 순회. */
		region = &dev->layout.region[i];
		/* [한국어] 해당 region의 layout 디스크립터. */
		if (region->current.blocks) {
			/* [한국어] 한 region이라도 blocks > 0이면 활성 — wipe 필요. */
			wipe = true;
			break;
		}
	}

	if (!wipe) {
		/* [한국어] 모든 P2L_LOG_IO region이 0블록 = P2L log 옵션 비활성.
		 * step을 건너뛰면 ftl_mngt가 다음 step으로 자동 진행. */
		ftl_mngt_skip_step(mngt);
		return;
	}

	ftl_mngt_p2l_wipe_range(dev, mngt, FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MIN,
				FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MAX);
	/* [한국어] P2L IO log region 그룹 일괄 wipe. */
}

/*
 * [한국어]
 * ftl_mngt_p2l_free_bufs - P2L checkpoint 모든 region의 메모리 버퍼만 해제.
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: recovery 후, P2L checkpoint를 메모리에 유지할 필요가 없는 경우
 * 디스크 핸들(ftl_md)은 그대로 두고 RAM 버퍼만 회수해 메모리를 절약한다.
 *
 * 동작: P2L_CKPT_MIN..MAX 순회 → ftl_md_free_buf로 각 region 버퍼 해제.
 *      ftl_md_destroy_region_flags(...)로 region별 플래그를 얻어 free 동작 결정
 *      (예: SHM 영역은 munmap, 일반 영역은 spdk_dma_free).
 *
 * 호출 체인: recovery desc 끝부분에서 본 step 호출 → next_step.
 */
void
ftl_mngt_p2l_free_bufs(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md;
	int region_type;

	for (region_type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     region_type <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX;
	     region_type++) {
		/* [한국어] 모든 P2L checkpoint region 순회. */
		md = dev->layout.md[region_type];
		assert(md);
		/* [한국어] 디버그 빌드에서 누락된 region 핸들 검출. */
		ftl_md_free_buf(md, ftl_md_destroy_region_flags(dev, dev->layout.region[region_type].type));
		/* [한국어] region 타입별 destroy 플래그(SHM 여부 등)를 적용해 buf 해제.
		 * ftl_md 핸들 자체(io 컨텍스트, region 정보)는 그대로 유지. */
	}
	ftl_mngt_next_step(mngt);
	/* [한국어] 모든 buf 해제 완료 — 다음 step. */
}

/*
 * [한국어]
 * ftl_mngt_p2l_restore_ckpt_cb - 단일 region restore 완료 시 카운터 갱신/종료 판정 콜백.
 *
 * @dev:    FTL 디바이스.
 * @md:     완료된 region MD 핸들. cb_ctx에서 ctx 복원.
 * @status: 0=성공, 비-0=실패.
 *
 * 왜 필요한가: restore는 wipe와 달리 모든 region을 병렬 발행하므로(I/O 처리량 ↑),
 * 콜백에서 카운트를 ++해 P2L_COUNT만큼 모이면 종료를 결정한다.
 * 하나라도 실패하면 ctx->status를 갱신해두고, 모두 모인 후 fail_step 호출.
 *
 * 실행 컨텍스트: 코어 스레드. 다중 region 비동기 완료가 코어 스레드에 직렬화되어 호출됨.
 */
static void
ftl_mngt_p2l_restore_ckpt_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_p2l_md_ctx *ctx = md->owner.cb_ctx;
	assert(ctx);
	/* [한국어] ctx가 NULL이면 owner 셋팅 누락 = 버그. 디버그 빌드에서 즉시 abort. */
	if (status) {
		/* [한국어] 비동기 read 실패 — 첫 에러를 보존(이후 콜백에서 덮어써도 됨). */
		ctx->status = status;
	}

	if (++ctx->md_region == FTL_LAYOUT_REGION_TYPE_P2L_COUNT) {
		/* [한국어] 모든 P2L checkpoint region 완료 — 종료 판정. */
		if (!ctx->status) {
			/* [한국어] 모두 성공 — 다음 step. */
			ftl_mngt_next_step(ctx->mngt);
		} else {
			/* [한국어] 하나라도 실패 — 롤백 트리거. */
			ftl_mngt_fail_step(ctx->mngt);
		}
	}
}

/*
 * [한국어]
 * ftl_mngt_p2l_restore_ckpt - 모든 P2L checkpoint region을 디스크에서 메모리로 병렬 복원.
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: dirty shutdown 후 recovery 단계에서 P2L checkpoint를 메모리에 올려야
 * L2P 재구성이 가능하다. 단, fast startup(SHM 픽업)인 경우 SHM에 이미 데이터가
 * 있으므로 디스크 read를 건너뛴다.
 *
 * 동작:
 *  1) ftl_fast_startup이면 디스크 read 생략 — next_step만 호출하고 종료.
 *  2) step ctx 할당(실패 시 fail_step).
 *  3) ctx 초기화 (md_region=0 카운터, status=0).
 *  4) P2L_CKPT_MIN..MAX 순회 — 모든 region에 ftl_md_restore 동시 발행.
 *  5) 각 region 완료 시 ftl_mngt_p2l_restore_ckpt_cb가 호출되어 카운트 ++.
 *  6) 마지막 콜백에서 next/fail step 결정.
 *
 * 호출 체인:
 *   recovery desc → ftl_mngt_p2l_restore_ckpt → 모든 region에 ftl_md_restore →
 *     restore_ckpt_cb 다중 호출 → 카운트 == P2L_COUNT 시점에 next/fail
 */
void
ftl_mngt_p2l_restore_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_md *md;
	struct ftl_mngt_p2l_md_ctx *ctx;
	int md_region;

	if (ftl_fast_startup(dev)) {
		/* [한국어] SHM 핸드오프 부팅 — 메모리에 P2L 데이터가 이미 보존됨.
		 * 디스크 read 생략하고 즉시 다음 step. */
		FTL_NOTICELOG(dev, "SHM: skipping p2l ckpt restore\n");
		ftl_mngt_next_step(mngt);
		return;
	}

	if (ftl_mngt_alloc_step_ctx(mngt, sizeof(struct ftl_mngt_p2l_md_ctx))) {
		/* [한국어] step ctx 할당 실패. */
		ftl_mngt_fail_step(mngt);
		return;
	}
	ctx = ftl_mngt_get_step_ctx(mngt);
	ctx->mngt = mngt;
	ctx->md_region = 0;
	/* [한국어] 완료 카운터 초기화 — 콜백에서 ++해서 P2L_COUNT까지 모음. */
	ctx->status = 0;
	/* [한국어] 누적 status 초기화 — 첫 에러를 보존. */

	for (md_region = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     md_region <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX; md_region++) {
		/* [한국어] 모든 P2L checkpoint region에 대해 비동기 restore 동시 발행. */
		md = layout->md[md_region];
		assert(md);
		md->owner.cb_ctx = ctx;
		/* [한국어] 모든 region이 같은 ctx를 공유 — 콜백에서 카운터 합산. */
		md->cb = ftl_mngt_p2l_restore_ckpt_cb;
		/* [한국어] 완료 콜백 등록. */
		ftl_md_restore(md);
		/* [한국어] 비동기 디스크 read 발행. 완료는 cb로 통보. */
	}
}
