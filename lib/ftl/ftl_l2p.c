/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL L2P(Logical-to-Physical) 디스패처 (ftl_l2p.c)
 *
 * === 파일의 역할 ===
 * SPDK FTL의 L2P 인터페이스 진입점이자 두 백엔드(flat / cache)를 추상화하는 디스패처이다.
 * SPDK_FTL_L2P_FLAT 빌드 매크로에 따라 매크로 FTL_L2P_OP(name)가
 * ftl_l2p_flat_<name> 또는 ftl_l2p_cache_<name>로 치환되어 호출된다.
 * 또한 본 파일은 백엔드와 무관한 공통 정책을 담당한다:
 *  - L2P pin 컨텍스트 초기화(ftl_l2p_pin_ctx_init)
 *  - pin이 -EAGAIN으로 지연된 경우 dev->l2p_deferred_pins 큐에 적재
 *  - poll 루프에서 deferred pin을 한 건씩 재시도
 *  - L2P 갱신의 핵심 로직(ftl_l2p_update_cache, ftl_l2p_update_base) — write-after-write/
 *    dirty shutdown/trim seq_id 등 엣지 케이스 처리
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   사용자 read/write/trim → ftl_l2p_pin → 본 파일 → flat/cache 백엔드 →
 *     (캐시면 비동기 페이지 fetch) → pin_complete → 사용자 콜백
 *   GC/Compaction 완료 → ftl_l2p_update_base / ftl_l2p_update_cache →
 *     ftl_l2p_set + ftl_invalidate_addr/ftl_band_set_addr/ftl_nv_cache_set_addr
 * 실행 컨텍스트: 코어 스레드 (assert(ftl_check_core_thread)로 강제).
 * 단, deferred_pins 큐 덕분에 pin 콜백이 다른 스레드에서 발생하는 경우(캐시 백엔드)
 * 핸들링이 가능하다.
 *
 * === 타 모듈과의 연결 ===
 * - ftl_l2p_flat.c / ftl_l2p_cache.c: 실제 매핑 저장소를 구현. 본 파일은 매크로로 디스패치.
 * - ftl_band: ftl_band_set_addr — 베이스 밴드의 P2L에 LBA 등록.
 * - ftl_nv_cache: NV cache chunk seq_id 비교, ftl_nv_cache_set_addr.
 * - dev->l2p_deferred_pins (TAILQ): -EAGAIN으로 지연된 pin들의 대기 큐.
 * - layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD]: trim_seq_id 페이지 메타.
 * 데이터 흐름: LBA → L2P 슬롯 read/write. NV cache write 시 chunk seq_id가 함께 갱신되어
 * 복구 시점에 가장 최신 데이터가 무엇인지 식별 가능.
 *
 * === 주요 함수/구조체 요약 ===
 * - FTL_L2P_OP 매크로: flat/cache 백엔드 함수로 컴파일 타임 디스패치.
 * - ftl_l2p_init/deinit: 백엔드 초기화 + deferred_pins 큐 초기화.
 * - ftl_l2p_pin/unpin/pin_skip: 사용자 I/O 진입 시 L2P 페이지 가용성 확보(또는 skip).
 * - ftl_l2p_set/get/clear/restore/persist/trim: 기본 매핑 연산 — 백엔드 위임.
 * - ftl_l2p_process: poll 루프에서 deferred pin 1건 재시도 + 백엔드 process 호출.
 * - ftl_l2p_is_halted/halt/resume: 정지 가능 여부와 상태 전환.
 * - ftl_l2p_update_cache: NV cache 쓰기 후 L2P 갱신 (write-after-write 처리 포함).
 * - ftl_l2p_update_base: 베이스(밴드) 쓰기 후 L2P 갱신 (compaction/GC 결과 반영).
 * - ftl_l2p_pin_complete: 백엔드의 pin 콜백 — -EAGAIN이면 deferred 큐에 보류.
 */

#include "ftl_l2p.h"        /* [한국어] L2P 디스패처/공통 인터페이스 — 본 파일이 외부에 노출. */
#include "ftl_band.h"       /* [한국어] 베이스 밴드의 P2L 갱신(ftl_band_set_addr/from_addr 등). */
#include "ftl_nv_cache.h"   /* [한국어] NV cache chunk 메타(seq_id) 및 set_addr. */
#include "ftl_l2p_cache.h"  /* [한국어] 두-레벨 캐시 백엔드 — FTL_L2P_OP의 한 후보. */
#include "ftl_l2p_flat.h"   /* [한국어] 평탄(flat) 백엔드 — FTL_L2P_OP의 다른 후보. */


/* TODO: Verify why function pointers had worse performance than compile time constants */
/* [한국어] 함수 포인터 디스패치 대신 매크로 치환을 사용하는 이유는 성능 문제 때문.
 * 사용자 I/O 핫 패스에서 호출되므로 인라이닝/예측 가능성이 중요. */
#ifdef SPDK_FTL_L2P_FLAT
#define FTL_L2P_OP(name)	ftl_l2p_flat_ ## name  /* [한국어] flat 백엔드 빌드 — 모든 호출이 ftl_l2p_flat_*로 치환. */
#else
#define FTL_L2P_OP(name)	ftl_l2p_cache_ ## name /* [한국어] cache 백엔드(기본) — ftl_l2p_cache_*로 치환. */
#endif


/*
 * [한국어]
 * ftl_l2p_init - L2P 디스패처/백엔드 초기화
 *
 * @dev: FTL 디바이스 핸들.
 * @return: 0 성공, 음수 errno (백엔드 init 결과 그대로).
 *
 * 1) deferred pins 큐 초기화.
 * 2) 백엔드(flat 또는 cache)의 init 호출.
 * 실행 컨텍스트: 코어 스레드. 관리 FSM의 startup 단계에서 호출.
 *
 * 호출 체인:
 *   ftl_mngt_* (startup) → [ftl_l2p_init] → ftl_l2p_flat_init / ftl_l2p_cache_init
 */
int
ftl_l2p_init(struct spdk_ftl_dev *dev)
{
	TAILQ_INIT(&dev->l2p_deferred_pins); /* [한국어] -EAGAIN으로 지연된 핀 요청 대기 큐 초기화. */
	return FTL_L2P_OP(init)(dev);          /* [한국어] 백엔드 초기화 위임. */
}

/*
 * [한국어]
 * ftl_l2p_deinit - L2P 디스패처/백엔드 해제
 *
 * @dev: FTL 디바이스 핸들.
 *
 * 백엔드 deinit만 호출. deferred 큐는 비어 있어야 정상(상위 레이어가 사전에 drain).
 * 실행 컨텍스트: 코어 스레드. 관리 FSM의 shutdown 단계.
 */
void
ftl_l2p_deinit(struct spdk_ftl_dev *dev)
{
	FTL_L2P_OP(deinit)(dev); /* [한국어] 백엔드 해제 위임. */
}

/*
 * [한국어]
 * ftl_l2p_pin_ctx_init - 핀 컨텍스트 멤버 채움 헬퍼
 *
 * @pin_ctx: 채울 컨텍스트(호출자 소유).
 * @lba/count/cb/cb_ctx: 사용자 핀 요청 인자.
 *
 * 백엔드/디스패처 모두에서 사용되는 공통 초기화. inline static으로 핫 패스 비용 최소화.
 * 실행 컨텍스트: 코어 스레드.
 */
static inline void
ftl_l2p_pin_ctx_init(struct ftl_l2p_pin_ctx *pin_ctx, uint64_t lba, uint64_t count,
		     ftl_l2p_pin_cb cb, void *cb_ctx)
{
	pin_ctx->lba = lba;       /* [한국어] 시작 LBA. */
	pin_ctx->count = count;   /* [한국어] 핀할 LBA 개수. */
	pin_ctx->cb = cb;         /* [한국어] 사용자 핀 완료 콜백. */
	pin_ctx->cb_ctx = cb_ctx; /* [한국어] 사용자 콜백 컨텍스트. */
}

/*
 * [한국어]
 * ftl_l2p_pin - 사용자 I/O 전 L2P 페이지 가용성 확보
 *
 * @dev: FTL 디바이스 핸들.
 * @lba: 시작 LBA.
 * @count: 핀할 LBA 개수.
 * @cb: 핀 완료 콜백.
 * @cb_ctx: 사용자 컨텍스트.
 * @pin_ctx: 호출자 소유 핀 컨텍스트(자료구조 보관 슬롯).
 *
 * cache 백엔드에서는 해당 LBA의 L2P 페이지를 메모리에 fetch하고 pinned 상태로 만든다.
 * flat은 항상 메모리에 있어 즉시 완료. 완료는 pin_ctx->cb로 비동기 통지.
 * 실행 컨텍스트: 코어 스레드.
 *
 * 호출 체인:
 *   ftl_io 진입 → [ftl_l2p_pin] → 백엔드 pin → ftl_l2p_pin_complete → 사용자 cb
 */
void
ftl_l2p_pin(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count, ftl_l2p_pin_cb cb, void *cb_ctx,
	    struct ftl_l2p_pin_ctx *pin_ctx)
{
	ftl_l2p_pin_ctx_init(pin_ctx, lba, count, cb, cb_ctx); /* [한국어] 컨텍스트 멤버 채움. */
	FTL_L2P_OP(pin)(dev, pin_ctx);                          /* [한국어] 백엔드 pin 호출. */
}

/*
 * [한국어]
 * ftl_l2p_unpin - pin과 짝이 되는 unpin
 *
 * @dev: FTL 디바이스.
 * @lba: 시작 LBA.
 * @count: 핀 해제할 LBA 개수.
 *
 * cache 백엔드: 페이지 참조 카운트 감소, 0이면 evict 후보. flat: no-op.
 * 실행 컨텍스트: 코어 스레드.
 */
void
ftl_l2p_unpin(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count)
{
	FTL_L2P_OP(unpin)(dev, lba, count); /* [한국어] 백엔드 unpin 위임. */
}

/*
 * [한국어]
 * ftl_l2p_pin_skip - 핀이 필요 없는 경우의 즉시 완료 헬퍼
 *
 * @dev: FTL 디바이스.
 * @cb: 사용자 콜백.
 * @cb_ctx: 사용자 컨텍스트.
 * @pin_ctx: 호출자 소유 컨텍스트.
 *
 * trim 등 LBA를 핀할 필요가 없는 경로에서 사용. lba/count를 INVALID/0으로 채워
 * 실수로 unpin이 일어나도 안전하게 만들고, cb를 즉시 호출.
 * 실행 컨텍스트: 코어 스레드.
 */
void
ftl_l2p_pin_skip(struct spdk_ftl_dev *dev, ftl_l2p_pin_cb cb, void *cb_ctx,
		 struct ftl_l2p_pin_ctx *pin_ctx)
{
	ftl_l2p_pin_ctx_init(pin_ctx, FTL_LBA_INVALID, 0, cb, cb_ctx); /* [한국어] 핀 정보를 무효 값으로 초기화. */
	cb(dev, 0, pin_ctx);                                            /* [한국어] 즉시 완료 통지. */
}

/*
 * [한국어]
 * ftl_l2p_set - 단일 LBA의 L2P 매핑 갱신 (저수준)
 *
 * @dev: FTL 디바이스.
 * @lba: 갱신 대상.
 * @addr: 새 ftl_addr.
 *
 * update_cache/update_base/복구 경로에서 호출되는 공통 setter — 추가 정책 없이 백엔드에 위임.
 * 실행 컨텍스트: 코어 스레드.
 */
void
ftl_l2p_set(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr addr)
{
	FTL_L2P_OP(set)(dev, lba, addr); /* [한국어] 백엔드 set. */
}

/*
 * [한국어]
 * ftl_l2p_get - LBA의 현재 L2P 매핑 조회
 *
 * @dev: FTL 디바이스.
 * @lba: 조회할 LBA.
 * @return: ftl_addr (FTL_ADDR_INVALID이면 매핑 없음).
 *
 * 실행 컨텍스트: 코어 스레드.
 */
ftl_addr
ftl_l2p_get(struct spdk_ftl_dev *dev, uint64_t lba)
{
	return FTL_L2P_OP(get)(dev, lba); /* [한국어] 백엔드 get. */
}

/*
 * [한국어]
 * ftl_l2p_clear - L2P 전체를 INVALID로 초기화 (포맷/리셋 시)
 *
 * @dev: FTL 디바이스.
 * @cb: 완료 콜백.
 * @cb_ctx: 사용자 컨텍스트.
 *
 * 실행 컨텍스트: 코어 스레드. 관리 FSM에서 호출.
 */
void
ftl_l2p_clear(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	FTL_L2P_OP(clear)(dev, cb, cb_ctx); /* [한국어] 백엔드 clear. */
}

/*
 * [한국어]
 * ftl_l2p_restore - 베이스에 저장된 L2P를 메모리로 복원
 *
 * @dev: FTL 디바이스.
 * @cb: 완료 콜백.
 * @cb_ctx: 사용자 컨텍스트.
 *
 * 실행 컨텍스트: 코어 스레드. startup 시 호출.
 */
void
ftl_l2p_restore(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	FTL_L2P_OP(restore)(dev, cb, cb_ctx); /* [한국어] 백엔드 restore. */
}

/*
 * [한국어]
 * ftl_l2p_persist - 현재 L2P를 베이스 디바이스에 영속화
 *
 * @dev: FTL 디바이스.
 * @cb: 완료 콜백.
 * @cb_ctx: 사용자 컨텍스트.
 *
 * 실행 컨텍스트: 코어 스레드. shutdown/체크포인트에서 호출.
 */
void
ftl_l2p_persist(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	FTL_L2P_OP(persist)(dev, cb, cb_ctx); /* [한국어] 백엔드 persist. */
}

/*
 * [한국어]
 * ftl_l2p_trim - TRIM(논리 삭제) 후 L2P/seq_id 갱신 (백엔드 위임)
 *
 * @dev: FTL 디바이스.
 * @cb: 완료 콜백.
 * @cb_ctx: 사용자 컨텍스트.
 *
 * 실행 컨텍스트: 코어 스레드. trim FSM에서 호출.
 */
void
ftl_l2p_trim(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	FTL_L2P_OP(trim)(dev, cb, cb_ctx); /* [한국어] 백엔드 trim. */
}

/*
 * [한국어]
 * ftl_l2p_process - poller tick 처리
 *
 * @dev: FTL 디바이스.
 *
 * 동작:
 *  1) deferred_pins 큐의 맨 앞 1건을 꺼내 재시도(=백엔드 pin 호출).
 *     그 결과 다시 -EAGAIN이면 ftl_l2p_pin_complete가 큐 끝에 다시 적재.
 *  2) 백엔드의 process를 호출하여 백엔드 내부 비동기 작업(페이지 fetch/evict 등)을 한 단계 진행.
 * deferred 큐를 한 번에 다 처리하지 않고 1건만 빼는 이유:
 *  - 다른 작업과의 공정성 유지(다른 poller가 starve되지 않도록).
 *  - 캐시 압박 분산.
 * 실행 컨텍스트: 코어 스레드의 메인 poller에서 매 tick 호출.
 */
void
ftl_l2p_process(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_pin_ctx *pin_ctx; /* [한국어] 큐에서 꺼낼 핀 요청. */

	pin_ctx = TAILQ_FIRST(&dev->l2p_deferred_pins); /* [한국어] 가장 오래된 deferred 핀(FIFO). */
	if (pin_ctx) {
		TAILQ_REMOVE(&dev->l2p_deferred_pins, pin_ctx, link); /* [한국어] 큐에서 제거. */
		FTL_L2P_OP(pin)(dev, pin_ctx);                          /* [한국어] 백엔드 pin 재시도. */
	}

	FTL_L2P_OP(process)(dev); /* [한국어] 백엔드 자체 진행 단계(예: 페이지 fetch poll). */
}

/*
 * [한국어]
 * ftl_l2p_is_halted - L2P 모듈이 정지 가능 상태인지
 *
 * @dev: FTL 디바이스.
 * @return: true면 모든 비동기 작업/대기 중인 핀이 없음 — 안전한 shutdown 시점.
 *
 * deferred 큐가 비어 있어야 하고, 백엔드도 halt 상태여야 true.
 * 실행 컨텍스트: 코어 스레드. shutdown FSM이 polling.
 */
bool
ftl_l2p_is_halted(struct spdk_ftl_dev *dev)
{
	if (!TAILQ_EMPTY(&dev->l2p_deferred_pins)) { /* [한국어] 대기 핀이 남아 있으면 아직 정지 불가. */
		return false;
	}

	return FTL_L2P_OP(is_halted)(dev); /* [한국어] 백엔드 자체의 halt 상태도 확인. */
}

/*
 * [한국어]
 * ftl_l2p_resume - L2P halt 해제/재개
 *
 * @dev: FTL 디바이스.
 *
 * 실행 컨텍스트: 코어 스레드.
 */
void
ftl_l2p_resume(struct spdk_ftl_dev *dev)
{
	return FTL_L2P_OP(resume)(dev); /* [한국어] 백엔드 resume. */
}

/*
 * [한국어]
 * ftl_l2p_halt - L2P 정지 요청
 *
 * @dev: FTL 디바이스.
 *
 * 백엔드는 신규 비동기 작업을 받지 않고 기존 작업만 마무리해 is_halted=true에 도달하도록 한다.
 * 실행 컨텍스트: 코어 스레드. shutdown FSM에서 호출.
 */
void
ftl_l2p_halt(struct spdk_ftl_dev *dev)
{
	return FTL_L2P_OP(halt)(dev); /* [한국어] 백엔드 halt. */
}

/*
 * [한국어]
 * get_trim_seq_id - 주어진 LBA가 속한 페이지의 trim 시퀀스 ID 조회
 *
 * @dev: FTL 디바이스.
 * @lba: 조회할 LBA.
 * @return: 해당 페이지의 trim_seq_id (uint64_t).
 *
 * trim_md 메타 영역은 LBA가 트리밍된 시점의 시퀀스를 페이지 단위로 기록한다.
 * lba를 lbas_in_page로 나눠 페이지 번호를 구한 뒤 그 페이지의 seq_id를 반환.
 * NV cache write가 trim 이후 도착했는지 검증하는 데 사용된다.
 * 실행 컨텍스트: 코어 스레드.
 *
 * 호출 체인:
 *   ftl_l2p_update_cache → [get_trim_seq_id] → ftl_md_get_buffer
 */
static uint64_t
get_trim_seq_id(struct spdk_ftl_dev *dev, uint64_t lba)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD]; /* [한국어] trim 메타 영역의 ftl_md. */
	uint64_t *page = ftl_md_get_buffer(md);                              /* [한국어] 페이지 단위 seq_id 배열의 시작 포인터. */
	uint64_t page_no = lba / dev->layout.l2p.lbas_in_page;               /* [한국어] LBA가 속한 페이지 인덱스. */

	return page[page_no]; /* [한국어] 해당 페이지의 trim_seq_id. */
}

/*
 * [한국어]
 * ftl_l2p_update_cache - NV cache 쓰기 후 L2P 갱신 (사용자 쓰기 경로)
 *
 * @dev: FTL 디바이스.
 * @lba: 쓰기 대상 LBA.
 * @new_addr: 방금 NV cache에 기록한 ftl_addr (ftl_addr_in_nvc(dev, new_addr)==true 보장).
 * @old_addr: 사용자 입장 이전 매핑(현 시점 caller가 알고 있던 값).
 *
 * 사용자 쓰기 경로의 L2P 갱신은 베이스 갱신보다 복잡한데, dirty shutdown에서 일관성을
 * 보장하기 위해 다음 엣지 케이스를 처리한다:
 *
 *  Case A: current_addr이 INVALID가 아님(이미 매핑 존재).
 *    - current_addr != old_addr이고 current_addr이 NV cache에 있으면
 *      "동시 두 사용자 쓰기"가 발생한 것 — 어느 쪽이 'win'인지 결정 필요.
 *      * 같은 chunk: 더 큰 ftl_addr이 win (write pointer가 더 진행된 쪽).
 *      * 다른 chunk: 더 큰 seq_id chunk가 win.
 *    - 그 외의 정상 경로: NV cache 측 P2L valid 비트를 먼저 set,
 *      L2P를 set, 마지막으로 old(current)_addr을 invalidate.
 *      "DO NOT CHANGE ORDER"는 SHM 기반 dirty shutdown 복구에서 valid 비트가 너무 적게
 *      세팅되는 것이 너무 많은 것보다 위험하기 때문(데이터 누락 vs 잠시 중복).
 *
 *  Case B: current_addr이 INVALID(매핑 없음 — 트림되었거나 처음 쓰는 LBA).
 *    - get_trim_seq_id로 해당 페이지의 trim_seq_id를 얻고, NV cache의 새 chunk seq_id가
 *      그것보다 작으면 이 쓰기는 trim 이후의 stale 데이터로 간주 → 무시.
 *    - 그 외에는 P2L valid set + L2P set.
 *
 * 실행 컨텍스트: 코어 스레드. assert(ftl_check_core_thread)로 강제.
 *
 * 호출 체인:
 *   사용자 write 완료 (NV cache) → [ftl_l2p_update_cache] → ftl_l2p_get/set,
 *     ftl_nv_cache_set_addr, ftl_invalidate_addr
 */
void
ftl_l2p_update_cache(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr new_addr, ftl_addr old_addr)
{
	struct ftl_nv_cache_chunk *current_chunk, *new_chunk; /* [한국어] win 결정에 사용할 chunk 메타. */
	ftl_addr current_addr;                                  /* [한국어] L2P가 현재 가리키는 주소. */
	/* Updating L2P for data in cache device - used by user writes.
	 * Split off from updating L2P in base due to extra edge cases for handling dirty shutdown in the cache case,
	 * namely keeping two simultaneous writes to same LBA consistent before/after shutdown - on base device we
	 * can simply ignore the L2P update, here we need to keep the address with more advanced write pointer
	 */
	/* [한국어] cache용 update가 base와 분리된 이유:
	 * dirty shutdown 복구 일관성을 위해 동시 두 사용자 쓰기 처리/seq_id 비교가 필요.
	 * base에서는 그냥 무시할 수 있는 상황이 cache에서는 정교한 'winner' 선택이 필요. */
	assert(ftl_check_core_thread(dev));     /* [한국어] 코어 스레드 강제. */
	assert(new_addr != FTL_ADDR_INVALID);    /* [한국어] 막 쓴 주소이므로 유효해야 함. */
	assert(ftl_addr_in_nvc(dev, new_addr));  /* [한국어] cache 경로이므로 NV cache 주소여야 함. */

	current_addr = ftl_l2p_get(dev, lba);    /* [한국어] 현재 매핑 조회. */

	if (current_addr != FTL_ADDR_INVALID) {

		/* Check if write-after-write happened (two simultaneous user writes to the same LBA) */
		/* [한국어] 동일 LBA에 대한 두 개의 동시 사용자 쓰기 검출. */
		if (spdk_unlikely(current_addr != old_addr
				  && ftl_addr_in_nvc(dev, current_addr))) {

			current_chunk = ftl_nv_cache_get_chunk_from_addr(dev, current_addr); /* [한국어] 기존 매핑이 속한 chunk. */
			new_chunk = ftl_nv_cache_get_chunk_from_addr(dev, new_addr);          /* [한국어] 새 매핑이 속한 chunk. */

			/* To keep data consistency after recovery skip oldest block */
			/* If both user writes are to the same chunk, the highest address should 'win', to keep data after
			 * dirty shutdown recovery consistent. If they're on different chunks, then higher seq_id chunk 'wins' */
			/* [한국어] 같은 chunk 내라면 write pointer가 더 진행된 쪽(=더 큰 ftl_addr)이 win.
			 * 다른 chunk라면 seq_id가 더 큰 쪽이 win.
			 * win이 아닌 경우 즉시 return하여 L2P 갱신을 skip. */
			if (current_chunk == new_chunk) {
				if (new_addr < current_addr) { /* [한국어] new_addr이 더 앞쪽 — 기존이 win. */
					return;
				}
			} else {
				if (new_chunk->md->seq_id < current_chunk->md->seq_id) { /* [한국어] new chunk가 더 오래됨. */
					return;
				}
			}
		}

		/* For recovery from SHM case valid maps need to be set before l2p set and
		 * invalidated after it */
		/* [한국어] SHM 기반 dirty shutdown 복구에서는 valid 비트 시퀀스가 중요:
		 * "set 이전 valid set, set 이후 invalidate" 순서를 지켜야 일관성 보장. */

		/* DO NOT CHANGE ORDER - START */
		ftl_nv_cache_set_addr(dev, lba, new_addr); /* [한국어] NV cache 측 P2L에 (lba → new_addr) 등록 (valid 비트 set 포함). */
		ftl_l2p_set(dev, lba, new_addr);            /* [한국어] L2P를 새 주소로 갱신. */
		ftl_invalidate_addr(dev, current_addr);     /* [한국어] 기존 주소의 valid 비트 클리어. */
		/* DO NOT CHANGE ORDER - END */
		return;
	} else {
		uint64_t trim_seq_id = get_trim_seq_id(dev, lba);                                       /* [한국어] 이 페이지의 마지막 trim seq_id. */
		uint64_t new_seq_id = ftl_nv_cache_get_chunk_from_addr(dev, new_addr)->md->seq_id;       /* [한국어] 새 chunk의 seq_id. */

		/* Check if region hasn't been trimmed during IO */
		/* [한국어] I/O 중에 trim이 끼어든 경우, 새 데이터가 trim보다 오래된 시점의 것이라면 무시.
		 * (trim이 더 최신이므로 그 LBA는 비어 있어야 함.) */
		if (new_seq_id < trim_seq_id) {
			return;
		}
	}

	/* If current address doesn't have any value (ie. it was never set, or it was trimmed), then we can just set L2P */
	/* DO NOT CHANGE ORDER - START (need to set P2L maps/valid map first) */
	/* [한국어] current_addr이 INVALID였고 trim 검사도 통과한 경로 — 단순 신규 매핑.
	 * 그래도 valid map을 먼저 set한 뒤 L2P를 set해야 dirty shutdown 시 누락 없음. */
	ftl_nv_cache_set_addr(dev, lba, new_addr); /* [한국어] NV cache 측 P2L valid set + 매핑 등록. */
	ftl_l2p_set(dev, lba, new_addr);            /* [한국어] L2P 갱신. */
	/* DO NOT CHANGE ORDER - END */
}

/*
 * [한국어]
 * ftl_l2p_update_base - 베이스(밴드) 쓰기 후 L2P 갱신 (compaction/GC 경로)
 *
 * @dev: FTL 디바이스.
 * @lba: 갱신 대상 LBA.
 * @new_addr: 방금 베이스 밴드에 기록한 ftl_addr (NV cache 아님).
 * @old_addr: GC/compaction이 알고 있던 이전 위치(반드시 유효).
 *
 * compaction/GC가 베이스에 데이터를 옮긴 직후 호출되는 L2P 갱신 경로.
 * cache와 달리 다음 차이가 있다:
 *  - new_addr이 NV cache가 아닐 것(베이스 영역 ftl_addr).
 *  - old_addr이 INVALID일 수 없음(GC가 옮긴 출처가 있어야 하므로).
 *  - 도중에 사용자 쓰기가 끼어들어 current_addr이 old_addr과 달라지면,
 *    GC가 옮긴 데이터는 무효화해야 함(=new_addr invalidate).
 *
 * 동작:
 *  - current_addr == old_addr: 정상 경로 — 밴드 P2L에 (lba→new_addr) 등록 후 L2P 갱신.
 *  - 그 외: 사용자가 도중에 같은 LBA를 NV cache로 다시 썼음 — new_addr는 stale이므로 invalidate.
 *  - 어느 경로든 마지막에 old_addr를 invalidate(GC 출처는 더 이상 valid 아님).
 *
 * 실행 컨텍스트: 코어 스레드. assert로 강제.
 *
 * 호출 체인:
 *   compaction/GC writer 완료 → [ftl_l2p_update_base] → ftl_l2p_get/set,
 *     ftl_band_set_addr, ftl_invalidate_addr
 */
void
ftl_l2p_update_base(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr new_addr, ftl_addr old_addr)
{
	ftl_addr current_addr; /* [한국어] L2P가 현재 가리키는 주소. */

	/* Updating L2P for data in base device - used by compaction and GC, may be invalidated by user write.
	 * Split off from updating L2P in cache due to extra edge cases for handling dirty shutdown in the cache case.
	 * Also some assumptions are not the same (can't assign INVALID address for base device - trim cases are done on cache)
	 */
	/* [한국어] base용 update의 가정: trim은 cache 측에서만 처리되고, base는 항상 유효한 old_addr가 있음. */
	assert(ftl_check_core_thread(dev));      /* [한국어] 코어 스레드 강제. */
	assert(new_addr != FTL_ADDR_INVALID);     /* [한국어] 새 주소는 유효. */
	assert(old_addr != FTL_ADDR_INVALID);     /* [한국어] base 경로는 출처가 반드시 있음. */
	assert(!ftl_addr_in_nvc(dev, new_addr));  /* [한국어] base 영역이어야 함. */

	current_addr = ftl_l2p_get(dev, lba);     /* [한국어] 현재 매핑 조회. */

	if (current_addr == old_addr) {
		/* DO NOT CHANGE ORDER - START (need to set L2P (and valid bits), before invalidating old ones,
		 * due to dirty shutdown from shm recovery - it's ok to have too many bits set, but not ok to
		 * have too many cleared) */
		/* [한국어] 정상 경로:
		 * SHM 기반 dirty shutdown 복구 시 "valid 비트가 너무 많으면 OK, 너무 적으면 문제".
		 * 그래서 set을 먼저, invalidate는 나중에. */
		ftl_band_set_addr(ftl_band_from_addr(dev, new_addr), lba, new_addr);
		/* [한국어] 새 ftl_addr이 속한 밴드의 P2L에 (lba→new_addr) 등록 + valid set. */
		ftl_l2p_set(dev, lba, new_addr); /* [한국어] L2P를 새 주소로 갱신. */
		/* DO NOT CHANGE ORDER - END */
	} else {
		/* new addr could be set by running p2l checkpoint but in the time window between
		 * p2l checkpoint completion and l2p set operation new data could be written on
		 * open chunk so this address need to be invalidated */
		/* [한국어] GC가 옮기는 동안 사용자가 같은 LBA를 다시 썼음 → 우리 new_addr는 사용되지 않음.
		 * 검증/복구를 위해 명시적으로 무효화해야 future GC가 잘못된 valid를 청소함. */
		ftl_invalidate_addr(dev, new_addr);
	}

	ftl_invalidate_addr(dev, old_addr); /* [한국어] 어느 경로든 기존 베이스 주소는 더 이상 유효하지 않음. */
}

/*
 * [한국어]
 * ftl_l2p_pin_complete - 백엔드 pin 콜백을 통해 호출되는 완료/지연 처리기
 *
 * @dev: FTL 디바이스.
 * @status: pin 결과. 0 성공, -EAGAIN이면 페이지 fetch가 진행 중(나중에 다시 시도).
 * @pin_ctx: 핀 컨텍스트.
 *
 * 동작:
 *  - status == -EAGAIN: 큐 끝에 적재 → ftl_l2p_process가 다음 tick에 재시도.
 *  - 그 외: 사용자 콜백을 즉시 호출.
 * 실행 컨텍스트: 코어 스레드 (백엔드가 그렇게 보장).
 *
 * 호출 체인:
 *   백엔드 pin 완료 → [ftl_l2p_pin_complete]
 *     → (EAGAIN) TAILQ_INSERT_TAIL → ftl_l2p_process 재시도
 *     → (그 외) pin_ctx->cb (사용자 콜백)
 */
void
ftl_l2p_pin_complete(struct spdk_ftl_dev *dev, int status, struct ftl_l2p_pin_ctx *pin_ctx)
{
	if (spdk_unlikely(status == -EAGAIN)) {                          /* [한국어] 일시 지연 — 큐에 보류. */
		TAILQ_INSERT_TAIL(&dev->l2p_deferred_pins, pin_ctx, link); /* [한국어] FIFO 끝에 삽입 — process가 한 건씩 꺼내 재시도. */
	} else {
		pin_ctx->cb(dev, status, pin_ctx);                          /* [한국어] 사용자 콜백 즉시 호출. */
	}
}
