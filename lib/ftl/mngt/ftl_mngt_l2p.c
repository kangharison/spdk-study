/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL L2P(Logical-to-Physical) 매핑 관리 단계 어댑터 (ftl_mngt_l2p.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 FTL(Flash Translation Layer)의 L2P(Logical-to-Physical) 매핑 테이블에
 * 대해 수행되는 5가지 라이프사이클 동작 — init(초기화) / deinit(해제) /
 * clear(전체 무효화) / persist(영속화) / trim(빈 영역 정리) / restore(디스크에서 복원) —
 * 를 ftl_mngt 단계 파이프라인의 step 콜백으로 감싸 노출한다. 즉, 실제 L2P 알고리즘은
 * lib/ftl/ftl_l2p.c에 있으며, 이 파일은 그 함수들을 ftl_mngt 상태 머신이 호출할 수 있는
 * 형태(`(dev, mngt)` 시그니처)로 변환하는 얇은 어댑터 역할만 담당한다.
 * L2P는 호스트 LBA(Logical Block Address)를 SSD 내부의 물리적 위치(밴드/오프셋)로
 * 매핑하는 핵심 자료구조이며, 부팅·셧다운·복구 시 반드시 단계적으로 다뤄야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트는 모두 FTL의 코어 스레드(spdk_thread)에서 실행되며, ftl_mngt 상태 머신이
 * `desc_startup` / `desc_shutdown` / `desc_recovery` 등의 step 배열을 순차 실행하다가
 * 본 파일의 step 함수를 호출한다.
 *   호출 체인 (startup):
 *     ftl_mngt_call_dev_startup() → ftl_mngt_process_execute() →
 *       (단계 #N) ftl_mngt_init_l2p() → ftl_l2p_init() → next_step
 *   호출 체인 (recovery):
 *     ftl_mngt_recover() → ... → ftl_mngt_restore_l2p() →
 *       ftl_l2p_restore(..., l2p_cb) → (비동기 완료) → l2p_cb() → next_step
 *   호출 체인 (shutdown):
 *     ftl_mngt_call_dev_shutdown() → ... → ftl_mngt_persist_l2p() →
 *       ftl_l2p_persist(..., l2p_cb) → l2p_cb() → next_step →
 *       ftl_mngt_deinit_l2p() → ftl_l2p_deinit()
 * 모든 step 콜백은 비동기 완료를 ftl_mngt_next_step()/ftl_mngt_fail_step()으로
 * 신호하여 다음 step으로 진행하거나 롤백을 트리거한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: lib/ftl/ftl_l2p.[ch] (실제 L2P 자료구조와 알고리즘 구현),
 *   lib/ftl/ftl_band.[ch] (밴드 메타데이터 — restore 시 valid bitmap 참조),
 *   lib/ftl/mngt/ftl_mngt.h (process_handle / next_step / fail_step API).
 * - 의존받음: lib/ftl/mngt/ftl_mngt_startup.c, ftl_mngt_shutdown.c,
 *   ftl_mngt_recovery.c — 이들의 step 배열에서 본 파일의 함수 포인터를 등록한다.
 * - 데이터 흐름: 부팅 시 ftl_l2p_init이 메모리에 빈 매핑 테이블을 만들고,
 *   recovery에서 ftl_l2p_restore가 P2L(Physical-to-Logical) 로그를 역방향으로 읽어
 *   L2P를 복구하며, 셧다운 시 ftl_l2p_persist가 L2P를 디스크 메타데이터 영역(L2P MD region)에
 *   기록한다. 모든 비동기 완료는 `l2p_cb(dev, status, ctx)` 형태로 본 파일에 돌아온다.
 * - 공유 자료구조: spdk_ftl_dev (ftl_l2p가 dev->l2p 등에 데이터를 저장),
 *   ftl_mngt_process (현재 step 진행 상태/롤백 스택 보유).
 *
 * === 주요 함수/구조체 요약 ===
 * - l2p_cb()              : 비동기 L2P 동작의 공통 완료 콜백. status를 보고 next/fail을 결정.
 * - ftl_mngt_init_l2p()   : 동기 초기화(메모리 할당). 실패 시 fail_step.
 * - ftl_mngt_deinit_l2p() : 동기 해제. 항상 next_step (해제는 실패할 일 없음).
 * - ftl_mngt_clear_l2p()  : 비동기 — 모든 LBA 매핑을 무효(unmapped)로 만든다.
 * - ftl_mngt_persist_l2p(): 비동기 — 메모리상 L2P를 디스크 MD region으로 flush.
 * - ftl_mngt_trim_l2p()   : 비동기 — 트림 큐에 쌓인 LBA들을 일괄 unmap.
 * - ftl_mngt_restore_l2p(): 비동기 — 디스크의 L2P MD를 메모리로 로드 (clean shutdown 시
 *                          빠른 경로) 또는 P2L 로그 재생으로 복구 (dirty shutdown 시).
 */

#include "spdk/thread.h"
/* [한국어] SPDK 스레드 라이브러리 — 본 파일에서 직접 쓰진 않으나, ftl_mngt가 spdk_thread
 * 컨텍스트에서 실행됨을 명시하는 의존성으로 포함. 호출자(ftl_mngt) 측 헤더가 의존. */

#include "ftl_core.h"
/* [한국어] spdk_ftl_dev 정의(FTL 디바이스의 모든 상태를 보유하는 최상위 구조체). */
#include "ftl_mngt.h"
/* [한국어] ftl_mngt_process / ftl_mngt_next_step / ftl_mngt_fail_step 등 상태 머신 API. */
#include "ftl_mngt_steps.h"
/* [한국어] 본 파일에서 정의·노출할 step 함수들의 프로토타입 선언 헤더. */
#include "ftl_band.h"
/* [한국어] 밴드 자료구조 — L2P 복구 시 밴드 단위 valid bitmap을 참조해야 하므로 포함. */
#include "ftl_l2p.h"
/* [한국어] 실제 L2P 구현(ftl_l2p_init/deinit/clear/persist/trim/restore) 프로토타입. */

/*
 * [한국어]
 * l2p_cb - 비동기 L2P 작업(clear/persist/trim/restore)의 공통 완료 콜백.
 *
 * @dev:    완료된 작업이 속한 FTL 디바이스. ftl_l2p_*이 콜백 인자로 전달.
 * @status: 0 = 성공, 음수 errno = 실패. 디스크 I/O 오류 또는 메타데이터 손상 시 음수.
 * @ctx:    원래 step 함수에서 ctx로 전달했던 ftl_mngt_process 핸들. 진행 상태 보유.
 * @return: 없음 — 결과는 mngt 핸들 내부 상태로 전달.
 *
 * 왜 필요한가: ftl_l2p_*는 비동기 라이브러리 함수라 완료 시점에 결과를 알려주려면
 * 콜백이 필요하다. 그러나 ftl_l2p 측은 ftl_mngt의 next/fail step API를 모르므로
 * 본 파일에서 어댑터 콜백을 만들어 두 인터페이스를 결합한다.
 *
 * 동작:
 *  1) ctx를 ftl_mngt_process로 캐스팅.
 *  2) status가 0이 아니면(에러) ftl_mngt_fail_step으로 롤백을 시작.
 *  3) status가 0이면 ftl_mngt_next_step으로 다음 step 진행.
 *
 * 실행 컨텍스트: FTL 코어 spdk_thread. ftl_l2p의 비동기 I/O 완료 폴러에서 호출됨.
 *
 * 호출 체인:
 *   ftl_mngt_clear_l2p()/persist/trim/restore → ftl_l2p_xxx(..., l2p_cb)
 *     → (비동기 완료) → l2p_cb() → ftl_mngt_next_step()/ftl_mngt_fail_step()
 */
static void
l2p_cb(struct spdk_ftl_dev *dev, int status, void *ctx)
{
	struct ftl_mngt_process *mngt = ctx;
	/* [한국어] step 호출 시 cb_ctx로 넘긴 mngt 핸들 복원 — 다음 step 트리거에 필요. */

	if (status) {
		/* [한국어] 비-0 status는 비동기 작업 실패. 롤백 경로(이미 실행된 step의
		 * cleanup을 역순 호출)로 진입시킨다. */
		ftl_mngt_fail_step(mngt);
	} else {
		/* [한국어] 성공 — 상태 머신을 다음 step으로 전진. 더 이상 step이 없으면
		 * mngt 내부에서 caller 콜백(ftl_mngt_completion)이 자동으로 호출된다. */
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_init_l2p - L2P 매핑 테이블 인메모리 자료구조를 할당·초기화.
 *
 * @dev:  대상 FTL 디바이스. dev->layout이 이미 결정되어 있어야 함.
 * @mngt: ftl_mngt 핸들 — step 결과(next/fail) 전달용.
 *
 * 왜 필요한가: 부팅(startup) 또는 recovery 단계 초반에 L2P 메모리(매핑 페이지 풀,
 * 캐시 등)를 만들지 않으면 이후 모든 read/write가 LBA→PBA 변환을 못한다.
 *
 * 동작:
 *  - ftl_l2p_init(dev) 호출. 동기 함수이며 0=성공, 음수=실패.
 *  - 결과에 따라 fail/next step을 즉시 호출(비동기 콜백 불필요).
 *
 * 실행 컨텍스트: FTL 코어 스레드, startup 단계 파이프라인 내부.
 *
 * 호출 체인:
 *   ftl_mngt_call_dev_startup() → ... → ftl_mngt_init_l2p() →
 *     ftl_l2p_init() → next_step/fail_step
 */
void
ftl_mngt_init_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_l2p_init(dev)) {
		/* [한국어] ftl_l2p_init이 비-0 반환 = 메모리 할당 실패 또는 layout 불일치.
		 * 롤백 트리거 — 이미 만든 자원(예: io_channel)들이 cleanup 콜백으로 정리됨. */
		ftl_mngt_fail_step(mngt);
	} else {
		/* [한국어] L2P 메모리 준비 완료 — 다음 step(ex: clear_l2p / restore_l2p)으로. */
		ftl_mngt_next_step(mngt);
	}
}

/*
 * [한국어]
 * ftl_mngt_deinit_l2p - L2P 메모리 자료구조를 해제.
 *
 * @dev:  대상 FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: 셧다운/롤백 시 L2P가 잡고 있던 메모리(수십~수백 MB의 매핑 페이지)를
 * 반환해야 한다.
 *
 * 동작: ftl_l2p_deinit(dev) 호출 후 무조건 next_step. 해제는 실패할 수 없다고 가정.
 *
 * 실행 컨텍스트: FTL 코어 스레드, shutdown step 파이프라인.
 *
 * 호출 체인:
 *   ftl_mngt_call_dev_shutdown() → ... → ftl_mngt_deinit_l2p() →
 *     ftl_l2p_deinit() → next_step
 */
void
ftl_mngt_deinit_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_l2p_deinit(dev);
	/* [한국어] L2P 페이지 풀, 캐시, 인덱스 자료구조 등 모든 인메모리 자원 해제. */
	ftl_mngt_next_step(mngt);
	/* [한국어] 항상 성공 — 다음 shutdown step으로 진행. */
}

/*
 * [한국어]
 * ftl_mngt_clear_l2p - 모든 LBA 매핑을 "미할당(unmapped)"으로 비동기 무효화.
 *
 * @dev:  대상 FTL 디바이스.
 * @mngt: ftl_mngt 핸들 — 비동기 완료 후 l2p_cb를 통해 next_step 트리거.
 *
 * 왜 필요한가: 새로 포맷되거나 첫 부팅(startup with no recovery)인 디바이스는
 * L2P 전체를 명시적으로 unmapped 상태로 셋팅해야 호스트 read 요청 시
 * "unmapped LBA"를 정확히 보고할 수 있다.
 *
 * 실행 컨텍스트: FTL 코어 스레드. ftl_l2p_clear는 내부적으로 page-level 메모리
 * 초기화 및 NV cache 갱신을 비동기로 수행할 수 있음.
 *
 * 호출 체인:
 *   ftl_mngt_init_l2p → ftl_mngt_clear_l2p → ftl_l2p_clear(..., l2p_cb, mngt)
 *     → (비동기 완료) → l2p_cb → next_step
 */
void
ftl_mngt_clear_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_l2p_clear(dev, l2p_cb, mngt);
	/* [한국어] 비동기 — 콜백(l2p_cb) 안에서 next/fail step 결정. 즉시 반환. */
}

/*
 * [한국어]
 * ftl_mngt_persist_l2p - 메모리상 L2P 매핑을 디스크 메타데이터 영역으로 비동기 flush.
 *
 * @dev:  대상 FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: 정상 셧다운 시 L2P를 디스크에 영속화해 둬야 다음 부팅에서
 * P2L 로그 재생(=수 분~수십 분 소요)을 건너뛰고 빠른 startup이 가능하다.
 *
 * 실행 컨텍스트: shutdown step. 내부적으로 dev->layout.md[L2P]에 대규모 write를
 * 발행하므로 base bdev I/O를 동반.
 *
 * 호출 체인:
 *   ftl_mngt_call_dev_shutdown → ... → ftl_mngt_persist_l2p →
 *     ftl_l2p_persist(..., l2p_cb, mngt) → (디스크 write 완료) → l2p_cb → next_step
 */
void
ftl_mngt_persist_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_l2p_persist(dev, l2p_cb, mngt);
	/* [한국어] 비동기 디스크 flush. 완료/실패는 l2p_cb에서 mngt로 전달. */
}

/*
 * [한국어]
 * ftl_mngt_trim_l2p - 호스트가 발행한 trim/discard 요청을 L2P에 일괄 반영(unmap).
 *
 * @dev:  대상 FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: trim 요청은 즉시 처리되지 않고 trim_map에 누적될 수 있는데,
 * shutdown 직전이나 명시적 동기화 시점에 누적된 trim을 L2P에 반영해서
 * 셧다운 후 다시 켰을 때도 trim된 LBA가 unmapped로 보이도록 한다.
 *
 * 실행 컨텍스트: shutdown step (clean shutdown desc의 "Finish L2P trims").
 *
 * 호출 체인:
 *   ftl_mngt_persist_l2p → ftl_mngt_trim_l2p →
 *     ftl_l2p_trim(..., l2p_cb, mngt) → l2p_cb → next_step
 */
void
ftl_mngt_trim_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_l2p_trim(dev, l2p_cb, mngt);
	/* [한국어] 비동기 — trim_map에 쌓인 LBA들을 L2P에서 unmap. 완료는 l2p_cb. */
}

/*
 * [한국어]
 * ftl_mngt_restore_l2p - 디스크에 저장된 L2P MD를 메모리로 비동기 복원(또는 P2L 재생).
 *
 * @dev:  대상 FTL 디바이스.
 * @mngt: ftl_mngt 핸들.
 *
 * 왜 필요한가: 부팅 시 L2P를 메모리로 올려야 호스트 read/write를 처리할 수 있다.
 * 정상 셧다운이었으면 디스크의 L2P MD region을 그대로 읽으면 되고(빠른 경로),
 * dirty shutdown이었으면 ftl_l2p_restore 내부에서 P2L 체크포인트와 P2L 로그를
 * 재생해 복구한다(느린 경로 — recovery에서 호출).
 *
 * 실행 컨텍스트: startup 또는 recovery step. 디스크 I/O 다수 발생.
 *
 * 호출 체인 (startup, clean):
 *   ftl_mngt_init_l2p → ftl_mngt_restore_l2p → ftl_l2p_restore (디스크 read만) →
 *     l2p_cb → next_step
 * 호출 체인 (recovery, dirty):
 *   ftl_mngt_recover → ... → ftl_mngt_restore_l2p → ftl_l2p_restore
 *     (P2L 재생 포함) → l2p_cb → next_step
 */
void
ftl_mngt_restore_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_l2p_restore(dev, l2p_cb, mngt);
	/* [한국어] 비동기 복원 — clean이면 단순 read, dirty면 P2L log replay 포함. */
}
