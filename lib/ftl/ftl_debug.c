/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 디버그/통계 헬퍼 (ftl_debug.c)
 *
 * === 파일의 역할 ===
 * SPDK FTL의 디버그 빌드 전용 검증 루틴과 운영 통계 출력 함수를 제공한다.
 * 가장 중요한 기능은 ftl_band_validate_md()로, 한 밴드(band)의 P2L(Physical-to-Logical)
 * 매핑이 L2P(Logical-to-Physical)와 정합한지 검사한다(=같은 LBA에 대해 양방향 매핑이
 * 일치하는지). 이 검사는 비동기적으로 L2P 페이지를 pin하고, 한 회당
 * FTL_MD_VALIDATE_LBA_PER_ITERATION(=128) LBA씩 순회하며 진행한다.
 * 추가로 ftl_dev_dump_bands() (DEBUG 빌드)와 ftl_dev_dump_stats()는 운영 가시성을 위한
 * 로그 출력 함수이며, 후자는 WAF(Write Amplification Factor) 등 핵심 지표를 보여준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용 호출자 측면:
 *   - ftl_band_validate_md: 밴드 close/open 시 안정성 검사를 위해 ftl_band 모듈에서 호출
 *   - ftl_dev_dump_bands/stats: RPC/관리 루틴이 디바이스 상태를 로그로 남길 때 호출
 * 의존 모듈 측면:
 *   - ftl_l2p_pin/unpin/get → 비동기 L2P 캐시 페이지 관리
 *   - ftl_bitmap → P2L의 valid 비트맵 조회
 *   - ftl_band → 밴드 메타데이터 (p2l_map, md->state, md->wr_cnt)
 *   - spdk_thread_send_msg → 코어 스레드로의 콜백 디스패치
 * 실행 컨텍스트: 코어 스레드. validate_md 콜백은 핀 완료가 어떤 스레드에서 발생하든
 * 최종적으로 spdk_thread_send_msg로 코어 스레드에 모이도록 보장된다.
 *
 * === 타 모듈과의 연결 ===
 * - ftl_l2p.c (디스패처): pin/unpin/get을 통해 L2P 페이지 가용성 확보 후 조회.
 * - ftl_band: p2l_map.valid 비트맵과 band_map[] 테이블에서 P2L 정보를 읽음.
 * - 통계 출처(ftl_dev_dump_stats): dev->stats.entries[FTL_STATS_TYPE_*]의
 *   write 카운터를 모아 사용자 쓰기 vs 총 쓰기 비율(WAF)을 계산.
 * 데이터 흐름: 밴드의 각 LBA에 대해 P2L(주어진 블록 오프셋의 LBA) → L2P(그 LBA가
 * 가리키는 ftl_addr) → 양방향 일치 여부를 비교하는 검증 흐름.
 * 공유 자료구조: dev->bands[], dev->stats, band->p2l_map.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ftl_band_validate_ctx: 밴드 검증 비동기 컨텍스트(가변 길이 멤버 l2p_pin_ctx[]).
 * - ftl_band_validate_md: 검증 진입점. 컨텍스트 할당 후 ftl_band_validate_md_pin로 진입.
 * - ftl_band_validate_md_pin: 한 회 분량(LBA 128개)을 비동기로 pin 요청하고 콜백 대기.
 * - ftl_band_validate_md_l2p_pin_cb: 페이지 핀 완료 콜백, remaining==0이면 코어 스레드로
 *   _ftl_band_validate_md를 디스패치.
 * - _ftl_band_validate_md: 핀 완료된 LBA에 대해 L2P 조회 및 P2L과 비교, 다음 회차로 재진입
 *   또는 최종 cb(band, valid) 호출.
 * - ftl_dev_dump_bands: 모든 밴드의 valid 카운트/wr_cnt/state를 NOTICELOG로 출력.
 * - ftl_dev_dump_stats: UUID, 총 LBA, 총/사용자 쓰기, WAF, limits 카운터를 NOTICELOG로 출력.
 */

#include "spdk/ftl.h"  /* [한국어] FTL 공개 API (struct spdk_ftl_dev, SPDK_FTL_LIMIT_*, FTL_STATS_TYPE_* 등). */
#include "ftl_debug.h" /* [한국어] 본 파일이 외부에 노출하는 ftl_band_validate_md/ftl_dev_dump_* 시그니처. */
#include "ftl_band.h"  /* [한국어] struct ftl_band, p2l_map, ftl_band_addr_from_block_offset 등. */

/* TODO: Switch to INFOLOG instead, we can control the printing via spdk_log_get_flag */
/* [한국어] 본 블록은 DEBUG 빌드에서만 컴파일됨.
 * 운영 빌드에서는 검증/덤프 루틴이 코드에 포함되지 않아 오버헤드 0이 보장된다.
 * 향후 INFOLOG로 전환되어 동적 토글이 가능해지면 이 #if는 제거될 예정. */
#if defined(DEBUG)

/* [한국어] ftl_band_state(enum)의 각 값에 대응하는 사람이 읽을 수 있는 문자열 표.
 * dump_bands에서 band->md->state로 인덱싱되어 출력에 사용된다.
 * 인덱스 의미: free(0), prep(1), opening(2), open(3), full(4), closing(5), closed(6), max(7).
 * "max"는 enum의 끝을 표시하는 sentinel 값이며 실제 상태는 아님. */
static const char *ftl_band_state_str[] = {
	"free",    /* [한국어] 밴드가 풀(free pool)에 있음 — 아직 어떤 데이터도 매핑되지 않음. */
	"prep",    /* [한국어] 사용을 위해 준비 중 (P2L 메타 등 초기화 단계). */
	"opening", /* [한국어] open 트랜지션 진행 중 (writer가 첫 쓰기를 준비). */
	"open",    /* [한국어] 활성 상태 — 새 데이터를 쓸 수 있음. */
	"full",    /* [한국어] 더 이상 쓸 자리가 없음 — close 대상. */
	"closing", /* [한국어] close 트랜지션 진행 중 (메타 영속화 등). */
	"closed",  /* [한국어] 영속화 완료, GC 또는 재할당 대상이 됨. */
	"max"      /* [한국어] enum 경계 sentinel — 실제 밴드에는 부여되지 않음. */
};

/*
 * [한국어]
 * struct ftl_band_validate_ctx - 밴드 메타데이터 검증의 비동기 진행 컨텍스트
 *
 * 가변 길이 배열 l2p_pin_ctx[]는 밴드의 LBA 슬롯 수만큼 할당되며, 각 슬롯이
 * 비동기 핀 요청 1건의 컨텍스트를 가진다. 검증은 전체 밴드를
 * FTL_MD_VALIDATE_LBA_PER_ITERATION(=128) 단위로 끊어 처리하므로, 한 회마다
 * 일부 슬롯만 활성화되고 나머지는 다음 회에 재사용된다.
 */
struct ftl_band_validate_ctx {
	struct ftl_band *band;
	/* [한국어] 검증 대상 밴드.
	 * 설정자: ftl_band_validate_md()가 사용자가 넘긴 band를 그대로 저장.
	 * 읽는 자: 모든 단계에서 band->p2l_map, band->dev 접근에 사용.
	 * 값 범위: 유효한 ftl_band 포인터(NULL 불가). 검증 동안 별도 핀 처리는 하지 않으나,
	 *   상위 호출자가 band 수명을 보장. */

	ftl_band_validate_md_cb cb;
	/* [한국어] 검증 완료 시 호출되는 사용자 콜백 (typedef: void(*)(struct ftl_band*, bool)).
	 * 설정자: ftl_band_validate_md()에서 인자 그대로 저장.
	 * 읽는 자: _ftl_band_validate_md()가 모든 회차 완료 후 cb(band, valid)로 호출. */

	int remaining;
	/* [한국어] 현재 회차에서 아직 완료되지 않은 비동기 핀 요청 수.
	 * 설정자: ftl_band_validate_md_pin()이 1로 초기화 후 LBA마다 ++.
	 *   ftl_band_validate_md_l2p_pin_cb()가 핀 완료 시 -- 한다.
	 * 읽는 자: l2p_pin_cb에서 0이 되면 코어 스레드로 _ftl_band_validate_md를 디스패치.
	 * 값 범위: 0~size+1. 1로 시작하는 트릭은 첫 LBA 핀이 동기 즉시 완료될 가능성에 대비. */

	uint64_t pin_cnt;
	/* [한국어] 누적 핀 수 (디버그용 sanity check).
	 * 설정자: ftl_band_validate_md_pin()이 핀 1건당 ++,
	 *   _ftl_band_validate_md()가 unpin 1건당 --.
	 * 읽는 자: 회차 종료 시 assert(pin_cnt == 0)로 누락 unpin 검출.
	 * 값 범위: 회차 진행 중 양수, 회차 종료 시 0. */

	uint64_t current_offset;
	/* [한국어] 현재 처리 중인 밴드 내 블록 오프셋(=다음 회차의 시작 LBA 인덱스).
	 * 설정자: ftl_band_validate_md()에서 0으로 초기화, _ftl_band_validate_md()가
	 *   매 회차 끝에 size만큼 증가.
	 * 읽는 자: 회차마다 [current_offset, current_offset+size)를 순회.
	 * 값 범위: 0 ~ ftl_get_num_blocks_in_band(dev). 후자에 도달하면 검증 종료. */

	struct ftl_l2p_pin_ctx l2p_pin_ctx[];
	/* [한국어] 가변 길이 배열 — 밴드 내 각 블록 오프셋의 핀 컨텍스트.
	 * 크기: ftl_get_num_blocks_in_band(band->dev) (malloc 시 명시적으로 확보).
	 * 사용 패턴: i번째 슬롯 = 밴드 i번 블록의 LBA 핀 상태.
	 *   유효하지 않은 블록은 l2p_pin_ctx[i].lba = FTL_LBA_INVALID로 마킹.
	 * 동기화: 한 ctx는 한 시점에 한 회차의 LBA들만 다루므로 슬롯 단위 race 없음. */
};

/* [한국어] 콜백 함수의 전방 선언 — pin → cb → pin → cb 형태의 상호 참조 해소용. */
static void ftl_band_validate_md_l2p_pin_cb(struct spdk_ftl_dev *dev, int status,
		struct ftl_l2p_pin_ctx *pin_ctx);

/* [한국어] 한 회차에 처리할 LBA 개수 상한.
 * 너무 큰 값을 한 번에 핀하면 L2P 캐시 압박이 심해지고 다른 I/O가 굶을 수 있어
 * 128로 제한해 점진 검증을 수행한다(스로틀링 효과). */
#define FTL_MD_VALIDATE_LBA_PER_ITERATION 128

/*
 * [한국어]
 * ftl_band_validate_md_pin - 한 회차 분량(최대 128 LBA)을 비동기로 pin
 *
 * @ctx: 진행 중인 검증 컨텍스트. current_offset부터 처리.
 * @return: 없음. 핀 콜백을 통해 다음 단계가 트리거됨.
 *
 * 동작:
 *  1) remaining=1로 초기화 — 첫 핀이 동기 즉시 완료되는 경우에도 _ftl_band_validate_md
 *     로 조기 진입하지 않게 하는 가드. 마지막에 수동으로 한 번 더 콜백 호출하여
 *     1을 소진해 정확히 0에 도달시킴.
 *  2) [current_offset, +size) 구간을 순회하며:
 *     - 해당 블록이 P2L valid 비트맵에서 invalid면 lba=INVALID로 표시 후 skip.
 *     - 그렇지 않으면 band_map[i].lba를 ftl_l2p_pin으로 핀 요청 (비동기).
 *     - remaining/pin_cnt를 ++.
 *  3) tmp_pin_ctx로 가짜 콜백 호출 → remaining=0 도달 시 다음 단계 진입.
 * 실행 컨텍스트: 코어 스레드. ftl_l2p_pin은 캐시 백엔드에서는 비동기, flat에서는 동기 완료.
 *
 * 호출 체인:
 *   ftl_band_validate_md → [ftl_band_validate_md_pin] → ftl_l2p_pin
 *     → (비동기) ftl_band_validate_md_l2p_pin_cb → spdk_thread_send_msg
 *     → _ftl_band_validate_md → (재귀) ftl_band_validate_md_pin
 */
static void
ftl_band_validate_md_pin(struct ftl_band_validate_ctx *ctx)
{
	struct ftl_band *band = ctx->band;                 /* [한국어] 검증 대상 밴드. */
	struct spdk_ftl_dev *dev = band->dev;              /* [한국어] FTL 디바이스 핸들. */
	struct ftl_p2l_map *p2l_map = &band->p2l_map;      /* [한국어] 밴드의 P2L 매핑 (valid 비트맵 + band_map). */
	size_t i, size;                                     /* [한국어] 루프 인덱스/한 회차 처리량. */
	struct ftl_l2p_pin_ctx tmp_pin_ctx = {              /* [한국어] 수동 콜백을 호출하기 위한 임시 컨텍스트. */
		.cb_ctx = ctx                                  /* [한국어] cb_ctx만 설정 — pin_cb는 cb_ctx만 참조. */
	};

	/* Since the first L2P page may already be pinned, the ftl_band_validate_md_l2p_pin_cb could be prematurely
	 * triggered. Initializing to 1 and then triggering the callback again manually prevents the issue.
	 */
	/* [한국어] 첫 LBA의 L2P 페이지가 이미 캐시되어 있으면 ftl_l2p_pin이 즉시 콜백을 부르고,
	 * 그러면 remaining이 0이 되어 다음 단계로 곧장 진입하는 race가 생긴다.
	 * 1로 초기화하고 마지막에 수동으로 콜백을 한 번 더 호출하여 정확한 종료 시점을 보장. */
	ctx->remaining = 1;
	size = spdk_min(FTL_MD_VALIDATE_LBA_PER_ITERATION,
			ftl_get_num_blocks_in_band(dev) - ctx->current_offset);
	/* [한국어] 이번 회차에서 처리할 LBA 개수 = min(128, 남은 블록 수). */

	for (i = ctx->current_offset; i < ctx->current_offset + size; ++i) {
		if (!ftl_bitmap_get(p2l_map->valid, i)) {  /* [한국어] 이 블록이 P2L에서 invalid면 검증 대상 아님. */
			ctx->l2p_pin_ctx[i].lba = FTL_LBA_INVALID; /* [한국어] _ftl_band_validate_md가 skip하도록 INVALID 마킹. */
			continue;                                  /* [한국어] 다음 블록으로. */
		}

		assert(p2l_map->band_map[i].lba != FTL_LBA_INVALID); /* [한국어] valid면 LBA가 반드시 정상값이어야 함 — 일관성 검사. */
		ctx->remaining++;                                     /* [한국어] 비동기 핀 요청 수 증가. */
		ctx->pin_cnt++;                                       /* [한국어] 누적 핀 수 증가. */
		ftl_l2p_pin(dev, p2l_map->band_map[i].lba, 1, ftl_band_validate_md_l2p_pin_cb, ctx,
			    &ctx->l2p_pin_ctx[i]);
		/* [한국어] L2P 페이지 1개를 핀 요청.
		 * - lba: P2L에서 읽은 이 블록의 LBA.
		 * - count=1: 한 LBA만 핀.
		 * - 콜백 ctx는 검증 컨텍스트 자체.
		 * - 슬롯은 i번 블록 전용 핀 컨텍스트 (배열 i번째 사용). */
	}

	ftl_band_validate_md_l2p_pin_cb(dev, 0, &tmp_pin_ctx);
	/* [한국어] 마지막에 수동으로 콜백 한 번 호출 — remaining=1 가드를 소진.
	 * 모든 ftl_l2p_pin이 동기적으로 즉시 완료되었더라도 여기서 정확히 0에 도달. */
}

/*
 * [한국어]
 * _ftl_band_validate_md - 한 회차 분량의 핀 완료 후 P2L↔L2P 정합성 검증
 *
 * @_ctx: ftl_band_validate_ctx 포인터 (void*로 받음 — spdk_thread_send_msg 시그니처).
 * @return: 없음. 검증이 끝나면 사용자 cb 호출, 아직 남았으면 다음 회차로 재진입.
 *
 * 회차 동작:
 *  1) [current_offset, +size) 구간을 순회.
 *  2) lba가 INVALID로 마킹된 슬롯은 skip.
 *  3) 그 외에 대해 ftl_l2p_get(lba)로 L2P 매핑 조회.
 *  4) 매핑이 INVALID가 아니고, NV cache가 아니며, P2L이 가리키는 ftl_addr와 다르면 valid=false.
 *     - cache에 있는 경우(ftl_addr_in_nvc=true)는 GC 도중 일시적 불일치가 정상이므로 통과.
 *  5) 모든 슬롯에 대해 unpin → pin_cnt 0 도달.
 *  6) current_offset += size; 끝까지 갔으면 사용자 cb(band, valid)와 ctx free.
 *  7) 아니면 ftl_band_validate_md_pin로 재진입(다음 회차).
 * 실행 컨텍스트: 코어 스레드 (l2p_pin_cb가 spdk_thread_send_msg로 디스패치).
 *
 * 호출 체인:
 *   spdk_thread_send_msg(core_thread, [_ftl_band_validate_md], ctx)
 *     → ftl_l2p_get/unpin → 사용자 cb 또는 ftl_band_validate_md_pin 재진입
 */
static void
_ftl_band_validate_md(void *_ctx)
{
	struct ftl_band_validate_ctx *ctx = _ctx;     /* [한국어] void*에서 검증 컨텍스트로 캐스팅. */
	struct ftl_band *band = ctx->band;            /* [한국어] 검증 대상 밴드. */
	struct spdk_ftl_dev *dev = band->dev;         /* [한국어] FTL 디바이스. */
	ftl_addr addr_l2p;                            /* [한국어] L2P에서 조회한 ftl_addr. */
	size_t i, size;                                /* [한국어] 루프 인덱스/처리량. */
	bool valid = true;                             /* [한국어] 검증 결과 누적 — 한 번이라도 불일치면 false. */
	uint64_t lba;                                  /* [한국어] 현재 슬롯의 LBA 캐시. */

	size = spdk_min(FTL_MD_VALIDATE_LBA_PER_ITERATION,
			ftl_get_num_blocks_in_band(dev) - ctx->current_offset);
	/* [한국어] 이번 회차 길이 — pin 단계와 동일 계산. */

	for (i = ctx->current_offset; i < ctx->current_offset + size; ++i) {
		lba = ctx->l2p_pin_ctx[i].lba;        /* [한국어] pin 단계에서 저장한 lba. INVALID이면 skip. */
		if (lba == FTL_LBA_INVALID) {
			continue;
		}

		if (ftl_bitmap_get(band->p2l_map.valid, i)) { /* [한국어] 이 블록이 여전히 P2L valid면 검증 진행. */
			addr_l2p = ftl_l2p_get(dev, lba);    /* [한국어] L2P에서 lba의 현재 매핑 조회. */

			if (addr_l2p != FTL_ADDR_INVALID && !ftl_addr_in_nvc(dev, addr_l2p) &&
			    addr_l2p != ftl_band_addr_from_block_offset(band, i)) {
				/* [한국어] 매핑이 유효하고, 베이스 디바이스 주소이며,
				 * 이 밴드의 i번 블록이 가져야 할 ftl_addr와 다르다면 P2L↔L2P 불일치.
				 * NV cache 주소(in_nvc)는 GC 도중 정상적으로 발생할 수 있어 제외. */
				valid = false;
			}
		}

		ctx->pin_cnt--;                       /* [한국어] 핀 카운트 감소. */
		ftl_l2p_unpin(dev, lba, 1);           /* [한국어] pin 단계에서 잡은 1 LBA 페이지 핀 해제. */
	}
	assert(ctx->pin_cnt == 0);                    /* [한국어] 회차 종료 시 모든 핀이 해제되어야 함 — 누락 검출. */

	ctx->current_offset += size;                  /* [한국어] 다음 회차의 시작점으로 이동. */

	if (ctx->current_offset == ftl_get_num_blocks_in_band(dev)) { /* [한국어] 모든 블록 검증 완료? */
		ctx->cb(band, valid);                                      /* [한국어] 사용자 콜백에 (band, valid) 통지. */
		free(ctx);                                                 /* [한국어] 검증 컨텍스트 해제. */
		return;                                                    /* [한국어] 종료. */
	}

	ftl_band_validate_md_pin(ctx);                /* [한국어] 아직 남았으므로 다음 128 LBA 회차 시작. */
}

/*
 * [한국어]
 * ftl_band_validate_md_l2p_pin_cb - L2P pin 완료 콜백 (회차당 size+1번 호출)
 *
 * @dev: FTL 디바이스 핸들 (현재 미사용).
 * @status: 핀 결과. 0이 아니면 비정상 — assert로 즉시 중단.
 * @pin_ctx: 핀 컨텍스트 (cb_ctx에 검증 ctx 저장).
 *
 * 핀 1건이 끝날 때마다 remaining을 감소시키고, 0이 되면 코어 스레드로
 * _ftl_band_validate_md를 보낸다. spdk_thread_send_msg를 거치는 이유:
 * 핀 콜백은 L2P 캐시 모듈의 백그라운드 처리 스레드에서 실행될 수 있어,
 * 검증 본체는 반드시 코어 스레드에서 돌아야 일관성이 보장되기 때문이다.
 * 실행 컨텍스트: ftl_l2p_pin의 완료 스레드(통상은 코어 스레드, 캐시 백엔드 시 다를 수 있음).
 *
 * 호출 체인:
 *   ftl_l2p_pin → (L2P 백엔드) → [ftl_band_validate_md_l2p_pin_cb]
 *     → (remaining==0 시) spdk_thread_send_msg → _ftl_band_validate_md
 */
static void
ftl_band_validate_md_l2p_pin_cb(struct spdk_ftl_dev *dev, int status,
				struct ftl_l2p_pin_ctx *pin_ctx)
{
	struct ftl_band_validate_ctx *ctx = pin_ctx->cb_ctx; /* [한국어] cb_ctx에 저장된 검증 컨텍스트 복원. */

	assert(status == 0); /* [한국어] 핀이 실패하면 검증 자체가 무효 — 디버그 빌드에서 즉시 중단. */

	if (--ctx->remaining == 0) { /* [한국어] 현재 회차의 모든 핀(+가짜 1건) 완료. */
		spdk_thread_send_msg(dev->core_thread, _ftl_band_validate_md, ctx);
		/* [한국어] 코어 스레드로 검증 본체 디스패치.
		 * spdk_thread_send_msg는 lockless 메시지 큐를 통해 안전하게 컨텍스트 전환. */
	}
}

/*
 * [한국어]
 * ftl_band_validate_md - 밴드 메타데이터(P2L↔L2P) 정합성 검증 진입점
 *
 * @band: 검증할 밴드 (호출자가 수명 보장).
 * @cb: 완료 콜백. cb(band, valid). valid=false면 메타 손상 의심.
 * @return: 없음. 비동기 — 결과는 cb로.
 *
 * 동작:
 *  - 밴드 LBA 슬롯 수만큼 가변 배열을 포함하는 ctx를 malloc
 *  - 실패 시 cb(band, false)를 즉시 호출(=비유효로 간주)
 *  - 첫 회차 ftl_band_validate_md_pin 호출
 * 실행 컨텍스트: 코어 스레드 (ftl_band 모듈에서 호출).
 *
 * 호출 체인:
 *   ftl_band_close 등 → [ftl_band_validate_md]
 *     → ftl_band_validate_md_pin → ... → cb(band, valid)
 */
void
ftl_band_validate_md(struct ftl_band *band, ftl_band_validate_md_cb cb)
{
	struct ftl_band_validate_ctx *ctx; /* [한국어] 새로 할당할 검증 컨텍스트. */
	size_t size;                        /* [한국어] 가변 배열 길이 (밴드 LBA 수). */

	assert(cb); /* [한국어] cb 미지정은 사용자 실수 — 디버그에서 즉시 검출. */

	size = ftl_get_num_blocks_in_band(band->dev); /* [한국어] 한 밴드의 블록(=LBA 슬롯) 수. */

	ctx = malloc(sizeof(*ctx) + size * sizeof(*ctx->l2p_pin_ctx));
	/* [한국어] 가변 길이 배열을 포함한 단일 할당.
	 * sizeof(*ctx)는 고정 멤버까지, 뒤에 size 개의 ftl_l2p_pin_ctx를 이어 붙임. */

	if (!ctx) { /* [한국어] OOM — 실패로 간주하고 즉시 cb 호출. */
		FTL_ERRLOG(band->dev, "Failed to allocate memory for band validate context");
		cb(band, false);
		return;
	}

	ctx->band = band;                              /* [한국어] 검증 대상. */
	ctx->cb = cb;                                  /* [한국어] 완료 콜백. */
	ctx->pin_cnt = 0;                              /* [한국어] 누적 핀 수 0에서 시작. */
	ctx->current_offset = 0;                       /* [한국어] 첫 회차는 오프셋 0부터. */

	ftl_band_validate_md_pin(ctx);                 /* [한국어] 첫 회차 핀 단계 시작. */
}

/*
 * [한국어]
 * ftl_dev_dump_bands - 모든 밴드의 valid 카운트/wr_cnt/state를 NOTICELOG로 덤프
 *
 * @dev: FTL 디바이스 핸들. dev->bands가 미할당이면 즉시 반환.
 * @return: 없음.
 *
 * 운영 시 디바이스의 GC 진행도/소진도를 시각화하기 위해 사용. RPC 또는 디버그
 * 트리거로 호출되며, 출력은 SPDK 로그(NOTICE) 채널.
 * 실행 컨텍스트: 코어 스레드. 출력만 하므로 락 불필요(단일 스레드 가정).
 */
void
ftl_dev_dump_bands(struct spdk_ftl_dev *dev)
{
	uint64_t i; /* [한국어] 밴드 인덱스. */

	if (!dev->bands) { /* [한국어] 디바이스 부분 초기화 상태에서는 덤프할 게 없음 — 안전 종료. */
		return;
	}

	FTL_NOTICELOG(dev, "Bands validity:\n"); /* [한국어] 헤더 라인. */
	for (i = 0; i < ftl_get_num_bands(dev); ++i) { /* [한국어] 모든 밴드 순회. */
		FTL_NOTICELOG(dev, " Band %3zu: %8zu / %zu \twr_cnt: %"PRIu64
			      "\tstate: %s\n",
			      i + 1, dev->bands[i].p2l_map.num_valid,
			      ftl_band_user_blocks(&dev->bands[i]),
			      dev->bands[i].md->wr_cnt,
			      ftl_band_state_str[dev->bands[i].md->state]);
		/* [한국어] 각 밴드 한 줄 요약:
		 * - "i+1": 사람이 읽을 1-based 번호.
		 * - "valid/total": 현재 유효 LBA 수 / 사용자 영역 총 블록 수.
		 * - "wr_cnt": P/E 카운터에 해당하는 누적 쓰기 횟수 (수명 추정).
		 * - "state": ftl_band_state_str로 변환된 상태 문자열. */
	}
}

#endif /* defined(DEBUG) */

/*
 * [한국어]
 * ftl_dev_dump_stats - 디바이스 운영 통계(WAF 등)를 NOTICELOG로 출력
 *
 * @dev: FTL 디바이스 핸들 (const — 출력만, 변경 없음).
 * @return: 없음.
 *
 * 출력 항목:
 *  - device UUID
 *  - 모든 밴드의 num_valid 합 (총 유효 LBA 수)
 *  - 총 쓰기 블록 수, 사용자 쓰기 블록 수
 *  - WAF = 총 쓰기 / 사용자 쓰기 (Write Amplification Factor — FTL 효율 지표)
 *  - DEBUG 빌드 한정: SPDK_FTL_LIMIT_*(crit/high/low/start) 카운터
 * WAF 산식의 분모는 FTL_STATS_TYPE_CMP(컴팩션 사용자 데이터의 write 카운트),
 * 분자는 사용자 + GC + MD_BASE 쓰기의 총합 = SSD에 실제 기록된 양.
 * 실행 컨텍스트: 코어 스레드.
 */
void
ftl_dev_dump_stats(const struct spdk_ftl_dev *dev)
{
	uint64_t i, total = 0;                  /* [한국어] 밴드 인덱스 / num_valid 합산. */
	char uuid[SPDK_UUID_STRING_LEN];         /* [한국어] UUID 문자열 버퍼 (36+1). */
	double waf;                              /* [한국어] Write Amplification Factor 계산 결과. */
	uint64_t write_user, write_total;        /* [한국어] 사용자 쓰기 / 총 쓰기 블록 수. */
	const char *limits[] = {                 /* [한국어] limit 종류 → 문자열 매핑 (DEBUG 출력 전용). */
		[SPDK_FTL_LIMIT_CRIT]  = "crit",    /* [한국어] 임계 — 즉시 사용자 I/O 차단. */
		[SPDK_FTL_LIMIT_HIGH]  = "high",    /* [한국어] 높음 — 강한 GC 압박. */
		[SPDK_FTL_LIMIT_LOW]   = "low",     /* [한국어] 낮음 — 약한 GC 압박. */
		[SPDK_FTL_LIMIT_START] = "start"    /* [한국어] 시작 — GC가 막 시작된 시점. */
	};

	(void)limits; /* [한국어] DEBUG 빌드가 아닐 때 unused 경고 회피. */

	if (!dev->bands) { /* [한국어] 밴드 미초기화 상태에서는 의미 있는 통계가 없음. */
		return;
	}

	/* Count the number of valid LBAs */
	/* [한국어] 모든 밴드를 순회하여 유효 LBA 수의 총합을 계산. */
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		total += dev->bands[i].p2l_map.num_valid; /* [한국어] 밴드별 num_valid 누적. */
	}

	write_user = dev->stats.entries[FTL_STATS_TYPE_CMP].write.blocks;
	/* [한국어] 사용자 쓰기 (compaction에 의해 사용자 데이터로 카운트되는 항목). */
	write_total = write_user +
		      dev->stats.entries[FTL_STATS_TYPE_GC].write.blocks +
		      dev->stats.entries[FTL_STATS_TYPE_MD_BASE].write.blocks;
	/* [한국어] 총 쓰기 = 사용자 + GC 재기록 + 메타데이터(베이스). NV cache 측 쓰기는 별도. */

	waf = (double)write_total / (double)write_user;
	/* [한국어] WAF — 사용자가 N 블록 쓸 때 SSD에는 N×WAF 블록이 기록됨. 1.0이 이상적. */

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &dev->conf.uuid); /* [한국어] UUID를 표준 8-4-4-4-12 소문자 문자열로 포매팅. */
	FTL_NOTICELOG(dev, "\n");                                  /* [한국어] 가독성을 위한 빈 줄. */
	FTL_NOTICELOG(dev, "device UUID:         %s\n", uuid);    /* [한국어] UUID 출력. */
	FTL_NOTICELOG(dev, "total valid LBAs:    %zu\n", total);  /* [한국어] 총 유효 LBA. */
	FTL_NOTICELOG(dev, "total writes:        %"PRIu64"\n", write_total); /* [한국어] 총 쓰기 블록 수. */
	FTL_NOTICELOG(dev, "user writes:         %"PRIu64"\n", write_user);  /* [한국어] 사용자 쓰기 블록 수. */
	FTL_NOTICELOG(dev, "WAF:                 %.4lf\n", waf);  /* [한국어] WAF 소수 4자리. */
#ifdef DEBUG
	/* [한국어] DEBUG 빌드에서만 GC limits 카운터를 추가 출력. */
	FTL_NOTICELOG(dev, "limits:\n");
	for (i = 0; i < SPDK_FTL_LIMIT_MAX; ++i) {
		FTL_NOTICELOG(dev, " %5s: %"PRIu64"\n", limits[i], dev->stats.limits[i]);
		/* [한국어] 각 limit 종류별 발생 횟수 출력 — GC 압박 빈도 추적용. */
	}
#endif
}
