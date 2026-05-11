/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 자가 점검(self test) — L2P와 valid map 일관성 검사 (ftl_mngt_self_test.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 startup 끝나기 직전에 (환경변수 FTL_SELF_TEST가 설정된 경우에만) 모든 L2P
 * 매핑을 순회하며 다음 두 가지 일관성을 검증한다:
 *  (1) 같은 PBA를 두 LBA가 가리키지 않는다 (no double reference) — 발견 시 -EINVAL.
 *  (2) L2P가 가리키는 모든 PBA가 dev->valid_map에서 set 상태이어야 한다 — mismatch 검출.
 * 또한 L2P에서 셋팅된 PBA 개수와 valid_map의 set 비트 개수가 일치하는지도 비교.
 * 디버깅·릴리즈 회귀 테스트용이며, FTL_SELF_TEST 환경변수가 없으면 step을 그냥 skip한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트: startup 파이프라인 끝부분의 "Self test" step에서 호출.
 *   호출 체인:
 *     startup desc → ftl_mngt_self_test → [FTL_SELF_TEST 미설정] next_step
 *                                       → [설정] call_process(desc_self_test) →
 *                                         test_prepare (전 LBA용 비트맵 calloc) →
 *                                         test_valid_map (4096 LBA씩 L2P pin → 콜백 검증)
 *                                            → continue_step 반복으로 모든 LBA 처리
 *                                         test_cleanup (비트맵 해제)
 *
 * === 타 모듈과의 연결 ===
 * - 의존: lib/ftl/utils/ftl_bitmap.[ch] (검증용 비트맵), lib/ftl/ftl_l2p.[ch]
 *   (ftl_l2p_pin/unpin/get), lib/ftl/ftl_internal.h (FTL_ADDR_INVALID, valid_map).
 * - 의존받음: lib/ftl/mngt/ftl_mngt_startup.c (startup desc의 self_test step).
 * - 데이터 흐름: dev의 L2P 인메모리 + valid_map을 read-only로 순회 — 디스크 I/O 없음.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ftl_validate_ctx : 검증용 비트맵 + 카운터 보유.
 * - ftl_mngt_test_prepare()  : 전체 LBA 수에 맞는 비트맵 calloc/생성.
 * - ftl_mngt_test_cleanup()  : 비트맵/buffer 해제.
 * - test_valid_map_pin_cb()  : L2P pin 완료 콜백, batch 단위 비교/카운트.
 * - ftl_mngt_test_valid_map(): 4096 LBA씩 pin 발행 — 모두 끝나면 카운트 일치 비교 후 next.
 * - desc_self_test           : prepare → validate → cleanup 3-step sub-process.
 * - ftl_mngt_self_test()     : startup step 진입점 — env로 sub-process 호출 결정.
 */

#include "ftl_mngt.h"
/* [한국어] ftl_mngt_process / step API. */
#include "ftl_mngt_steps.h"
/* [한국어] step 프로토타입 (ftl_mngt_self_test 노출). */
#include "ftl_internal.h"
/* [한국어] FTL_ADDR_INVALID, ftl_addr 타입. */
#include "ftl_core.h"
/* [한국어] spdk_ftl_dev (num_lbas, valid_map 등) 및 ftl_l2p API. */
#include "ftl_band.h"
/* [한국어] ftl_addr_in_nvc — 주소가 NV cache에 속하는지 판단. */

/*
 * [한국어]
 * struct ftl_validate_ctx - self-test 진행 중 사용하는 process context.
 *
 * 모든 필드가 prepare 단계에서 셋팅되어 validate 콜백 사이에 공유된다.
 */
struct ftl_validate_ctx {
	struct {
		struct ftl_bitmap *bitmap;
		/* [한국어] L2P가 어떤 PBA를 이미 셋팅했는지 추적하는 검증용 비트맵 핸들.
		 * 설정자: ftl_mngt_test_prepare에서 ftl_bitmap_create.
		 * 읽는 자: test_valid_map_pin_cb에서 ftl_bitmap_get/set으로 double ref 검출.
		 * 값 범위: NULL(미할당) 또는 유효한 ftl_bitmap.
		 * 동기화: 코어 스레드 단일. */

		void *buffer;
		/* [한국어] 위 비트맵의 백킹 버퍼(calloc).
		 * 설정자: prepare. 읽는 자: cleanup에서 free.
		 * 값 범위: 유효 포인터(생성 후) 또는 NULL.
		 * 동기화: 단일 스레드. */

		uint64_t buffer_size;
		/* [한국어] buffer의 바이트 크기(비트맵 정렬 적용 후).
		 * 설정자: prepare. 읽는 자: validate가 memset(0)할 때.
		 * 값 범위: ceil(bit_count/8)을 ftl_bitmap_buffer_alignment로 올림한 값. */

		uint64_t bit_count;
		/* [한국어] 검증 대상 PBA 개수 = base.total_blocks + nvc.total_blocks. */

		uint64_t base_valid_count;
		/* [한국어] L2P가 base device 영역으로 매핑한 PBA 개수.
		 * 설정자: pin_cb에서 매핑마다 ++. 읽는 자: validate 종료 시 valid_map과 비교. */

		uint64_t cache_valid_count;
		/* [한국어] L2P가 NV cache 영역으로 매핑한 PBA 개수. */
	} valid_map;

	int status;
	/* [한국어] 누적 에러 status (음수 errno) — 첫 에러를 보존하고 종료 시 fail/next 결정. */
};

/*
 * [한국어]
 * ftl_mngt_test_prepare - 검증용 비트맵을 calloc/생성하는 step.
 *
 * @dev:  FTL 디바이스.
 * @mngt: sub-process mngt 핸들.
 *
 * 동작:
 *  1) bit_count = 전체 PBA 수 (base + NVC).
 *  2) buffer_size = ceil(bit_count/8) → bitmap 정렬로 올림.
 *  3) calloc + ftl_bitmap_create. 실패 시 fail_step.
 */
static void
ftl_mngt_test_prepare(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_validate_ctx *cntx = ftl_mngt_get_process_ctx(mngt);

	cntx->valid_map.bit_count = dev->layout.base.total_blocks +
				    dev->layout.nvc.total_blocks;
	/* [한국어] base + NV cache 양쪽의 PBA를 모두 한 비트맵으로 추적. */
	cntx->valid_map.buffer_size = spdk_divide_round_up(cntx->valid_map.bit_count, 8);
	/* [한국어] 비트 수를 바이트 단위로 올림(8비트=1바이트). */
	cntx->valid_map.buffer_size = SPDK_ALIGN_CEIL(cntx->valid_map.buffer_size,
				      ftl_bitmap_buffer_alignment);
	/* [한국어] ftl_bitmap이 요구하는 정렬(보통 64B)으로 다시 올림. */

	cntx->valid_map.buffer = calloc(cntx->valid_map.buffer_size, 1);
	/* [한국어] 0으로 초기화된 백킹 버퍼 할당. */
	if (!cntx->valid_map.buffer) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	cntx->valid_map.bitmap = ftl_bitmap_create(cntx->valid_map.buffer,
				 cntx->valid_map.buffer_size);
	/* [한국어] buffer를 백킹으로 사용하는 비트맵 핸들 생성. */
	if (!cntx->valid_map.bitmap) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * ftl_mngt_test_cleanup - 비트맵·buffer 해제 step (정상 종료/cleanup 양쪽에서 사용).
 *
 * @dev:  FTL 디바이스.
 * @mngt: sub-process mngt 핸들.
 *
 * desc_self_test 배열에서 prepare의 cleanup 콜백으로 등록되기도 하고, 마지막 step의
 * action으로도 등록되어 두 경로 모두에서 호출.
 */
static void
ftl_mngt_test_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_validate_ctx *cntx = ftl_mngt_get_process_ctx(mngt);

	ftl_bitmap_destroy(cntx->valid_map.bitmap);
	cntx->valid_map.bitmap = NULL;
	/* [한국어] 핸들 해제 + dangling 방지. */

	free(cntx->valid_map.buffer);
	cntx->valid_map.buffer = NULL;

	ftl_mngt_next_step(mngt);
}

/*
 * [한국어]
 * test_valid_map_pin_cb - 4096 LBA batch에 대한 L2P pin 완료 콜백.
 *
 * @dev:     FTL 디바이스.
 * @status:  pin 결과 (0=성공, 음수=L2P 페이지 로드 실패).
 * @pin_ctx: pin 요청 컨텍스트 — lba/count/cb_ctx 보유.
 *
 * 왜 pin이 필요한가: L2P 페이지가 cold면 페이지 캐시에서 evict될 수 있으므로 검증 동안
 * 메모리에 fix해야 한다. ftl_l2p_pin은 비동기일 수 있어 콜백으로 결과 받음.
 *
 * 동작 (성공 시):
 *  1) lba..lba+count 범위 순회.
 *  2) ftl_l2p_get으로 PBA 획득. INVALID(미매핑)이면 skip.
 *  3) 검증 비트맵에서 해당 PBA가 이미 set이면 → "double reference" 에러.
 *  4) 비트맵 set 후 base/cache 카운터 ++.
 *  5) dev->valid_map에서 그 PBA가 set인지 확인 — unset이면 "L2P/valid_map mismatch".
 *  6) ftl_l2p_unpin으로 페이지 unfix.
 *  7) pin_ctx->lba += count로 다음 batch 위치 갱신.
 *  8) 에러 없으면 ftl_mngt_continue_step → ftl_mngt_test_valid_map 다시 호출 (다음 batch).
 *  9) 에러 있으면 fail_step.
 */
static void
test_valid_map_pin_cb(struct spdk_ftl_dev *dev, int status,
		      struct ftl_l2p_pin_ctx *pin_ctx)
{
	struct ftl_mngt_process *mngt = pin_ctx->cb_ctx;
	struct ftl_validate_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	uint64_t lba, end;

	if (status) {
		/* [한국어] L2P 페이지 로드 실패 — 검증 자체가 의미 없음. fail. */
		FTL_ERRLOG(dev, "L2P pin ERROR when testing valid map\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	lba = pin_ctx->lba;
	end = pin_ctx->lba + pin_ctx->count;

	for (; lba < end; ++lba) {
		ftl_addr addr = ftl_l2p_get(dev, lba);
		bool valid;

		if (FTL_ADDR_INVALID == addr) {
			/* [한국어] LBA가 매핑되지 않은 상태(unmapped) — 검증 대상 아님. */
			continue;
		}

		if (ftl_bitmap_get(ctx->valid_map.bitmap, addr)) {
			/* [한국어] 이미 다른 LBA가 같은 PBA를 가리킨 적 있음 = double reference.
			 * L2P 손상 — 즉시 종료. */
			status = -EINVAL;
			FTL_ERRLOG(dev, "L2P mapping ERROR, double reference, "
				   "address 0x%.16"PRIX64"\n", addr);
			break;
		} else {
			/* [한국어] 처음 보는 PBA — 비트맵에 마킹. */
			ftl_bitmap_set(ctx->valid_map.bitmap, addr);
		}

		if (ftl_addr_in_nvc(dev, addr)) {
			/* [한국어] PBA가 NV cache 영역에 속하면 cache 카운터. */
			ctx->valid_map.cache_valid_count++;
		} else {
			/* [한국어] base device 영역이면 base 카운터. */
			ctx->valid_map.base_valid_count++;
		}

		valid = ftl_bitmap_get(dev->valid_map, addr);
		if (!valid) {
			/* [한국어] L2P는 이 PBA를 가리키는데 dev->valid_map에는 unset =
			 * 두 자료구조 불일치. 데이터 무결성 위협 — fail. */
			status = -EINVAL;
			FTL_ERRLOG(dev, "L2P and valid map mismatch"
				   ", LBA 0x%.16"PRIX64
				   ", address 0x%.16"PRIX64" unset\n",
				   lba, addr);
			break;
		}
	}

	ftl_l2p_unpin(dev, pin_ctx->lba, pin_ctx->count);
	/* [한국어] 검증 끝났으니 L2P 페이지 unfix(향후 evict 가능). */
	pin_ctx->lba += pin_ctx->count;
	/* [한국어] 다음 batch 위치 = 현재 시작 + count. */

	if (!status) {
		/* [한국어] 에러 없음 — ftl_mngt_test_valid_map을 다시 호출해 다음 batch 처리. */
		ftl_mngt_continue_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_test_valid_map - 4096 LBA씩 batch로 L2P pin 발행하는 step (자기 자신을 반복 호출).
 *
 * @dev:  FTL 디바이스.
 * @mngt: sub-process mngt 핸들.
 *
 * 동작:
 *  - step ctx에 ftl_l2p_pin_ctx를 두고 lba 진행 상태 보존(첫 호출에 alloc + lba=0).
 *  - 남은 LBA = num_lbas - pin_ctx->lba. count = min(left, 4096).
 *  - count > 0이면 ftl_l2p_pin 발행 → 콜백이 continue_step으로 본 함수 재호출.
 *  - count == 0(끝)이면 base+cache 카운터 합 = valid_map count인지 비교.
 *    일치하면 next_step, 불일치면 fail.
 */
static void
ftl_mngt_test_valid_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_l2p_pin_ctx *pin_ctx;
	struct ftl_validate_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	uint64_t left;

	pin_ctx = ftl_mngt_get_step_ctx(mngt);
	if (!pin_ctx) {
		/* [한국어] 첫 진입 — step ctx가 아직 없으므로 할당. */
		if (ftl_mngt_alloc_step_ctx(mngt, sizeof(*pin_ctx))) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		pin_ctx = ftl_mngt_get_step_ctx(mngt);
		assert(pin_ctx);

		pin_ctx->lba = 0;
		/* [한국어] 첫 batch는 LBA 0부터. */
		memset(ctx->valid_map.buffer, 0, ctx->valid_map.buffer_size);
		/* [한국어] 검증 비트맵을 0으로 초기화 — 이전 시도 잔여 보장. */
	}

	left = dev->num_lbas - pin_ctx->lba;
	pin_ctx->count = spdk_min(left, 4096);
	/* [한국어] 한 번에 최대 4096 LBA씩 pin (L2P 페이지 캐시 압박 회피). */

	if (pin_ctx->count) {
		/* [한국어] 처리할 LBA가 남음 — pin 발행. 비동기일 수 있음. */
		ftl_l2p_pin(dev, pin_ctx->lba, pin_ctx->count,
			    test_valid_map_pin_cb, mngt, pin_ctx);
	} else {
		/* [한국어] 모든 LBA 검증 끝 — 카운트 일치 검증. */
		if (!ctx->status) {
			uint64_t valid = ctx->valid_map.base_valid_count +
					 ctx->valid_map.cache_valid_count;

			if (ftl_bitmap_count_set(dev->valid_map) != valid) {
				/* [한국어] L2P가 가리키는 PBA 수와 valid_map의 set 비트 수
				 * 불일치 — 데이터 누수 또는 stale 매핑. */
				ctx->status = -EINVAL;
			}
		}

		/* All done */
		if (ctx->status) {
			ftl_mngt_fail_step(mngt);
		} else {
			ftl_mngt_next_step(mngt);
		}
	}
}

/*
 * Verifies the contents of L2P versus valid map. Makes sure any physical addresses in the L2P
 * have their corresponding valid bits set and that two different logical addresses don't point
 * to the same physical address.
 *
 * For debugging purposes only, directed via environment variable - whole L2P needs to be loaded in
 * and checked.
 */
/*
 * [한국어]
 * desc_self_test - L2P/valid_map 일관성 검증 sub-process desc.
 *
 * 3 step: prepare(비트맵 생성) → validate(반복 pin) → cleanup(해제).
 * prepare에 cleanup 콜백을 두어 validate 도중 실패해도 비트맵이 누수되지 않게 함.
 */
static const struct ftl_mngt_process_desc desc_self_test = {
	.name = "[Test] Startup Test",
	.ctx_size = sizeof(struct ftl_validate_ctx),
	/* [한국어] process ctx 메모리 자동 할당. */
	.steps = {
		{
			.name = "[TEST] Initialize selftest",

			.action = ftl_mngt_test_prepare,
			.cleanup = ftl_mngt_test_cleanup
			/* [한국어] action 후 어디서든 fail_step 발생 시 본 cleanup이 자동 호출됨. */
		},
		{
			.name = "[TEST] Validate map and L2P consistency",
			.action = ftl_mngt_test_valid_map
			/* [한국어] continue_step으로 자기 자신을 반복 호출해 모든 LBA 처리. */
		},
		{
			.name = "[TEST] Deinitialize cleanup",
			.action = ftl_mngt_test_cleanup
			/* [한국어] 정상 종료 경로에서 비트맵 해제. */
		},
		{}
	}
};

/*
 * [한국어]
 * ftl_mngt_self_test - startup의 self-test step 진입점.
 *
 * @dev:  FTL 디바이스.
 * @mngt: startup desc mngt 핸들.
 *
 * 환경변수 FTL_SELF_TEST가 설정되어 있으면 desc_self_test sub-process 호출.
 * 아니면 즉시 next_step (가장 흔한 운영 경로 — self_test는 비싼 동작).
 */
void
ftl_mngt_self_test(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (getenv("FTL_SELF_TEST")) {
		/* [한국어] 환경변수 활성 — 풀 검증 sub-process 실행. */
		ftl_mngt_call_process(mngt, &desc_self_test, NULL);
	} else {
		/* [한국어] 일반 운영 경로 — skip하고 startup 계속. */
		FTL_NOTICELOG(dev, "Self test skipped\n");
		ftl_mngt_next_step(mngt);
	}
}
