/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 디스크 layout 업그레이드 단계 (ftl_mngt_upgrade.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 구버전 SPDK에서 만든 FTL layout(메타데이터 region들의 위치/크기/포맷)을
 * 현재 버전 layout으로 마이그레이션하는 step을 정의한다. 업그레이드는 region 단위로
 * 점진 진행되며(각 region이 자신의 version 필드를 가짐), region 하나가 끝날 때마다
 * 슈퍼블록을 다시 영속화해 도중에 전원이 끊겨도 다음 부팅이 어디까지 끝났는지 알 수
 * 있게 한다. 또한 단순 layout 검증(verify) step도 함께 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트: startup 파이프라인 중 슈퍼블록 로드 직후 `ftl_mngt_layout_upgrade`가
 * 호출됨. 업그레이드 sub-process가 region을 한 개씩 변환→슈퍼블록 영속화 → 다시
 * 다음 region 변환으로 진행. 모든 region이 최신이면 done.
 *   호출 체인:
 *     ftl_mngt_call_dev_startup → ... → ftl_mngt_layout_upgrade →
 *       ftl_mngt_call_process(desc_layout_upgrade) →
 *         layout_upgrade() [parent] →
 *           ftl_mngt_process_execute(desc_region_upgrade) [child] →
 *             region_upgrade → ftl_region_upgrade(version transform 함수 등록) →
 *             region_upgrade_cb → ftl_superblock_store_blob_area + next →
 *             ftl_mngt_persist_superblock →
 *           [child 종료] layout_upgrade_cb → ftl_mngt_continue_step(parent) →
 *           layout_upgrade() 재호출 (다음 region or done)
 *
 * === 타 모듈과의 연결 ===
 * - 의존: lib/ftl/upgrade/ftl_layout_upgrade.[ch] (region별 version transform 함수
 *   테이블, FTL_LAYOUT_UPGRADE_CONTINUE/DONE/FAULT 반환 코드),
 *   lib/ftl/ftl_sb.h (슈퍼블록 / blob area 영속화).
 * - 의존받음: lib/ftl/mngt/ftl_mngt_startup.c (layout_upgrade step), ftl_mngt_recovery.c.
 * - 데이터 흐름: 디스크의 구버전 region → ftl_region_upgrade가 메모리에서 변환 →
 *   ftl_superblock_store_blob_area로 슈퍼블록의 region 메타에 새 버전 기록 →
 *   persist_superblock으로 디스크 슈퍼블록 갱신 → 다음 region.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ftl_mngt_upgrade_ctx : parent/child mngt 핸들과 ftl_layout_upgrade_ctx 보유.
 * - region_upgrade()       : 단일 region에 대해 version transform 호출.
 * - region_upgrade_cb()    : 위 완료 후 슈퍼블록 blob area 저장.
 * - desc_region_upgrade    : "단일 region 업그레이드" sub-process step 배열.
 * - layout_upgrade()       : "다음 업그레이드 대상 region 선택" — 반복적으로 자기 자신을
 *                            continue_step으로 재호출하면서 region을 하나씩 처리.
 * - layout_upgrade_cb()    : region sub-process 완료 후 parent를 continue_step.
 * - desc_layout_upgrade    : 외부에서 호출하는 풀 layout 업그레이드 process.
 * - ftl_mngt_layout_verify(): layout 검증만 수행 (업그레이드 없이).
 * - ftl_mngt_layout_upgrade(): layout 업그레이드 process 진입점.
 */

#include "ftl_core.h"
/* [한국어] spdk_ftl_dev 및 FTL_ERRLOG. */
#include "ftl_mngt.h"
/* [한국어] ftl_mngt API (call_process, continue_step 등). */
#include "ftl_mngt_steps.h"
/* [한국어] ftl_mngt_persist_superblock 프로토타입. */
#include "ftl_sb.h"
/* [한국어] ftl_superblock_store_blob_area — 슈퍼블록 region 메타 갱신. */
#include "upgrade/ftl_layout_upgrade.h"
/* [한국어] ftl_layout_upgrade_ctx, ftl_layout_upgrade_init_ctx, ftl_region_upgrade,
 * FTL_LAYOUT_UPGRADE_* 반환 코드, ftl_upgrade_layout_dump 등 업그레이드 핵심 API. */

/*
 * [한국어]
 * struct ftl_mngt_upgrade_ctx - layout 업그레이드 process의 context.
 *
 * 두 개의 mngt 핸들이 필요한 이유: layout_upgrade는 자기 자신을 continue_step으로
 * 재호출하면서 매번 region 단위 sub-process(desc_region_upgrade)를 새로 띄우기 때문.
 * parent는 외부 startup이 띄운 mngt, mngt(이 ctx 내부)는 sub-process용.
 */
struct ftl_mngt_upgrade_ctx {
	struct ftl_mngt_process *parent;
	/* [한국어] 외부에서 본 process를 호출한 부모 mngt 핸들. layout_upgrade 시작 시 셋팅,
	 * sub-process 끝나면 continue_step 호출 대상. */

	struct ftl_mngt_process *mngt;
	/* [한국어] 현재 처리 중인 region용 sub-process 핸들. region_upgrade 진입 시 셋팅,
	 * region_upgrade_cb에서 next/fail step 트리거 대상. */

	struct ftl_layout_upgrade_ctx upgrade_ctx;
	/* [한국어] ftl_layout_upgrade 라이브러리가 사용하는 upgrade 진행 상태 컨텍스트.
	 * - reg: 현재 업그레이드 중인 ftl_layout_region 포인터
	 * - upgrade: region 타입별 version transform 함수 테이블
	 * - ctx: 변환 함수가 내부적으로 쓰는 동적 컨텍스트(여기서 calloc/free 관리)
	 * - cb/cb_ctx: 비동기 완료 콜백과 그 인자 */
};

/*
 * [한국어]
 * region_upgrade_cb - 단일 region 변환 완료 콜백.
 *
 * @dev:    FTL 디바이스.
 * @_ctx:   ftl_mngt_upgrade_ctx 포인터.
 * @status: 0=성공, 음수=실패.
 *
 * 동작:
 *  1) 변환 함수가 쓴 동적 컨텍스트(upgrade_ctx.ctx) 해제.
 *  2) 실패 시 step 실패 보고.
 *  3) 성공 시 슈퍼블록 blob area에 변경된 region 메타 저장 후 next_step.
 *
 * 호출 체인:
 *   ftl_region_upgrade(...) → (변환 끝) → region_upgrade_cb →
 *     [성공] ftl_superblock_store_blob_area + ftl_mngt_next_step →
 *     desc_region_upgrade의 "Persist superblock" step → ...
 */
static void
region_upgrade_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_mngt_upgrade_ctx *ctx = _ctx;

	free(ctx->upgrade_ctx.ctx);
	/* [한국어] 변환 함수가 사용한 동적 메모리 해제. */
	ctx->upgrade_ctx.ctx = NULL;
	/* [한국어] dangling pointer 방지 — 다음 region 처리 시 calloc 직전 NULL 확인. */

	if (status) {
		/* [한국어] 변환 실패(예: 손상된 데이터). 에러 로깅 후 step 실패. */
		FTL_ERRLOG(dev, "Upgrade failed for region %d (rc=%d)\n", ctx->upgrade_ctx.reg->type, status);
		ftl_mngt_fail_step(ctx->mngt);
	} else {
		/* [한국어] 변환 성공 — 슈퍼블록 blob area에 새 region 메타(예: version 필드)
		 * 직렬화. 디스크 영속화는 다음 step("Persist superblock")에서 수행. */
		ftl_superblock_store_blob_area(dev);
		ftl_mngt_next_step(ctx->mngt);
	}
}

/*
 * [한국어]
 * region_upgrade - 단일 region의 version transform 함수 호출.
 *
 * @dev:  FTL 디바이스.
 * @mngt: sub-process(desc_region_upgrade) 핸들.
 *
 * 왜 필요한가: 각 region은 자기 version 필드를 갖고 있고, 그 version에 매핑된 변환
 * 함수가 ftl_layout_upgrade의 desc 테이블에 등록되어 있다. 본 함수는 그 변환 함수에
 * 필요한 동적 컨텍스트(ctx_size 만큼)를 calloc한 뒤 ftl_region_upgrade를 호출.
 *
 * 실행 컨텍스트: sub-process step의 첫 action. parent ctx에서 upgrade_ctx 복원.
 *
 * 호출 체인:
 *   desc_region_upgrade의 "Region upgrade" step → region_upgrade →
 *     ftl_region_upgrade(...) → 변환 함수 (비동기 가능) → region_upgrade_cb
 */
static void
region_upgrade(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_upgrade_ctx *ctx = ftl_mngt_get_caller_ctx(mngt);
	/* [한국어] sub-process의 caller(즉 desc_layout_upgrade의 process ctx) 가져오기.
	 * call_process 호출 시 init_ctx로 전달된 ctx와 동일. */
	struct ftl_layout_upgrade_ctx *upgrade_ctx = &ctx->upgrade_ctx;
	size_t ctx_size = upgrade_ctx->upgrade->desc[upgrade_ctx->reg->current.version].ctx_size;
	/* [한국어] 현재 region 버전에서 다음 버전으로 변환할 함수가 요구하는 동적
	 * 컨텍스트 크기(0이면 ctx 없이 동작 가능). */
	int rc = -1;
	/* [한국어] 실패 기본값 — 어떤 경로로든 성공 시 ftl_region_upgrade 반환값으로 덮어씀. */

	assert(upgrade_ctx->ctx == NULL);
	/* [한국어] 이전 region 처리 후 region_upgrade_cb에서 NULL로 만들었어야 함. */
	if (ctx_size) {
		upgrade_ctx->ctx = calloc(1, ctx_size);
		/* [한국어] 변환 함수용 zero-init 메모리 할당. */
		if (!upgrade_ctx->ctx) {
			/* [한국어] 할당 실패 — 0이 아닌 rc로 cb 강제 호출 경로로 이동. */
			goto exit;
		}
	}
	upgrade_ctx->cb = region_upgrade_cb;
	/* [한국어] ftl_region_upgrade가 비동기 완료 시 호출할 콜백. */
	upgrade_ctx->cb_ctx = ctx;
	/* [한국어] 콜백 1번째 인자(우리 ctx). */
	ctx->mngt = mngt;
	/* [한국어] 콜백에서 ftl_mngt_next/fail_step 호출용 sub-process mngt 저장. */

	rc = ftl_region_upgrade(dev, upgrade_ctx);
	/* [한국어] 실제 region 변환 호출. 동기 완료면 cb 호출 후 rc=0, 비동기면 즉시 0
	 * 반환하고 추후 cb 호출. 즉시 에러면 음수 반환. */
exit:
	if (rc) {
		/* [한국어] calloc 실패(rc=-1) 또는 ftl_region_upgrade 동기 실패.
		 * region_upgrade_cb를 직접 호출해 일관된 에러 경로 사용. */
		region_upgrade_cb(dev, ctx, rc);
	}
}

/*
 * [한국어]
 * desc_region_upgrade - 단일 region 업그레이드용 sub-process step 배열.
 *
 * 두 step으로 구성: (1) 변환, (2) 슈퍼블록 영속화. 변환만 끝내고 슈퍼블록 안 쓰면
 * 도중에 전원 끊기면 어디까지 됐는지 알 수 없으므로 매 region마다 디스크 영속화 필수.
 */
static const struct ftl_mngt_process_desc desc_region_upgrade = {
	.name = "FTL region upgrade",
	/* [한국어] 로그용 sub-process 이름. */
	.steps = {
		{
			.name = "Region upgrade",
			/* [한국어] 단일 region version transform. */
			.action = region_upgrade,
		},
		{
			.name = "Persist superblock",
			/* [한국어] 슈퍼블록을 디스크에 비동기 flush — 다음 부팅에서 이 region의
			 * 새 version이 반영된 상태로 보임. */
			.action = ftl_mngt_persist_superblock,
		},
		{}
		/* [한국어] sentinel. */
	}
};

/*
 * [한국어]
 * layout_upgrade_cb - region sub-process 완료 콜백.
 *
 * @dev:    FTL 디바이스.
 * @_ctx:   ftl_mngt_upgrade_ctx 포인터.
 * @status: sub-process 결과.
 *
 * 동작:
 *  - 실패 시 변환용 ctx 해제하고 parent를 fail_step.
 *  - 성공 시 ctx->parent로 continue_step → layout_upgrade가 다시 호출되어 다음
 *    region/version 선택.
 *
 * 실행 컨텍스트: sub-process가 끝나면서 caller에게 콜백 호출.
 */
static void
layout_upgrade_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_mngt_upgrade_ctx *ctx = _ctx;

	if (status) {
		/* [한국어] sub-process 실패 — 자원 해제 후 parent에 실패 보고. */
		free(ctx->upgrade_ctx.ctx);
		ctx->upgrade_ctx.ctx = NULL;
		ftl_mngt_fail_step(ctx->parent);
		return;
	}

	/* go back to ftl_mngt_upgrade() step and select the next region/version to upgrade */
	/* [한국어] parent step을 다시 호출(같은 step.action을 또 실행). 그러면 layout_upgrade가
	 * 다음 region/version을 고르거나, 모두 끝났으면 next_step 한다. */
	ftl_mngt_continue_step(ctx->parent);
}

/*
 * [한국어]
 * layout_upgrade - "다음 업그레이드 대상" 선택 + 처리 또는 종료 결정.
 *
 * @dev:    FTL 디바이스.
 * @parent: 외부 호출자(예: startup desc) mngt 핸들.
 *
 * 왜 이런 구조인가: 한 step의 action은 한 번에 region 하나만 처리하므로, layout 전체
 * 업그레이드가 끝날 때까지 자기 자신을 continue_step으로 반복 호출해야 한다.
 * ftl_layout_upgrade_init_ctx가 반환한 코드로 분기:
 *  - CONTINUE: 처리할 region 더 있음 → desc_region_upgrade sub-process 실행.
 *  - DONE: 모두 끝남 → layout dump 검증 후 next_step.
 *  - FAULT: 비가역적 오류 → fail_step.
 *
 * 호출 체인:
 *   ftl_mngt_layout_upgrade → call_process(desc_layout_upgrade) →
 *     layout_upgrade (이 함수) [반복] →
 *       call_process(desc_region_upgrade) → ... → layout_upgrade_cb →
 *         ftl_mngt_continue_step → layout_upgrade 재호출
 */
static void
layout_upgrade(struct spdk_ftl_dev *dev, struct ftl_mngt_process *parent)
{
	struct ftl_mngt_upgrade_ctx *ctx = ftl_mngt_get_process_ctx(parent);
	/* [한국어] desc_layout_upgrade.ctx_size에 등록된 메모리를 process ctx로 사용. */
	int rc;

	ctx->parent = parent;
	/* [한국어] 콜백에서 다시 continue_step / fail_step할 부모 핸들 저장. */
	rc = ftl_layout_upgrade_init_ctx(dev, &ctx->upgrade_ctx);
	/* [한국어] upgrade_ctx를 "다음 처리 대상 region/version" 상태로 초기화.
	 * 처음 호출이면 첫 region을, 두 번째 호출부터는 이전에 처리 못한 다음 후보를 가리킴. */

	switch (rc) {
	case FTL_LAYOUT_UPGRADE_CONTINUE:
		/* [한국어] 처리할 region 있음 — sub-process로 위임. */
		if (!ftl_mngt_process_execute(dev, &desc_region_upgrade, layout_upgrade_cb, ctx)) {
			/* [한국어] sub-process 실행 시작 성공(0 반환). 완료는 layout_upgrade_cb로. */
			return;
		}

		/* [한국어] sub-process 시작 실패 — parent에 fail 보고. */
		ftl_mngt_fail_step(parent);
		break;

	case FTL_LAYOUT_UPGRADE_DONE:
		/* [한국어] 모든 region 업그레이드 완료. */
		if (ftl_upgrade_layout_dump(dev)) {
			/* [한국어] 최종 layout 검증 실패 = 업그레이드 결과가 손상 상태.
			 * fail로 처리. */
			FTL_ERRLOG(dev, "MD layout verification failed after upgrade.\n");
			ftl_mngt_fail_step(parent);
		} else {
			/* [한국어] 검증 OK — 다음 startup step으로 진행. */
			ftl_mngt_next_step(parent);
		}
		break;

	case FTL_LAYOUT_UPGRADE_FAULT:
		/* [한국어] 비가역 오류(예: 지원되지 않는 version transition). */
		ftl_mngt_fail_step(parent);
		break;
	}
	if (ctx->upgrade_ctx.ctx) {
		/* [한국어] 비동기 경로로 가지 않은 케이스(DONE/FAULT/sub-process 시작 실패)에서
		 * 잔여 동적 메모리 해제. CONTINUE 성공 시는 위 return 때문에 여기 도달하지 않음. */
		free(ctx->upgrade_ctx.ctx);
	}
}

/*
 * [한국어]
 * desc_layout_upgrade - 외부에서 호출되는 풀 layout 업그레이드 process desc.
 *
 * ctx_size에 ftl_mngt_upgrade_ctx 크기를 등록하면 ftl_mngt가 자동으로 process ctx 메모리를
 * 잡아준다(layout_upgrade에서 ftl_mngt_get_process_ctx로 획득).
 */
static const struct ftl_mngt_process_desc desc_layout_upgrade = {
	.name = "FTL layout upgrade",
	/* [한국어] 로그용 이름. */
	.ctx_size = sizeof(struct ftl_mngt_upgrade_ctx),
	/* [한국어] process ctx 크기 등록 — ftl_mngt가 calloc해서 step에 공유. */
	.steps = {
		{
			.name = "Layout upgrade",
			/* [한국어] 단일 step이지만 self-continue로 반복 호출되어 모든 region 처리. */
			.action = layout_upgrade,
		},
		{}
		/* [한국어] sentinel. */
	}
};


/*
 * [한국어]
 * ftl_mngt_layout_verify - 현재 디스크 layout이 기대 layout과 일치하는지만 검증.
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: 업그레이드 없이 그냥 startup하는 정상 경로에서 layout이 어긋났는지
 * 확인. 다르면 fail.
 */
void
ftl_mngt_layout_verify(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_layout_verify(dev)) {
		/* [한국어] 검증 실패(예: region 크기 불일치). 롤백 트리거. */
		ftl_mngt_fail_step(mngt);
	} else {
		/* [한국어] OK — 다음 step. */
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_layout_upgrade - layout 업그레이드 step 진입점.
 *
 * @dev:  FTL 디바이스.
 * @mngt: ftl_mngt 핸들 (예: startup desc 안).
 *
 * 동작: desc_layout_upgrade sub-process를 call_process로 호출. init_ctx는 NULL이지만
 * desc.ctx_size > 0이므로 ftl_mngt가 자체적으로 ctx 메모리 할당.
 *
 * 호출 체인:
 *   startup desc → ftl_mngt_layout_upgrade → call_process(desc_layout_upgrade) →
 *     layout_upgrade → (반복) → next_step
 */
void
ftl_mngt_layout_upgrade(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process(mngt, &desc_layout_upgrade, NULL);
	/* [한국어] 업그레이드 sub-process 호출. init_ctx는 사용하지 않으므로 NULL. */
}
