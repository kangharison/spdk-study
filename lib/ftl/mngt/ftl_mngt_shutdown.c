/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 디바이스 셧다운 단계 파이프라인 정의 (ftl_mngt_shutdown.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 FTL 디바이스를 정상 종료할 때 실행되는 step 파이프라인 두 가지를 선언적으로
 * 정의한다 — (1) `desc_shutdown`: 일반 clean shutdown(메타데이터를 디스크에 완전 영속화),
 * (2) `desc_fast_shutdown`: shared memory(SHM) 기반 핸드오프를 위한 빠른 셧다운(최소
 * 메타데이터만 영속화). 두 파이프라인 모두 startup에서 만든 자원(io_channel, core poller,
 * L2P, P2L checkpoint, bdev 등)을 startup의 역순으로 정리한다.
 * 진입점은 `ftl_mngt_call_dev_shutdown()` 한 함수이며, dev->conf.fast_shutdown 플래그로
 * 두 파이프라인 중 하나를 골라 ftl_mngt_process_execute()에 위임한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 컨텍스트: spdk_ftl_dev_free() (또는 RPC bdev_ftl_delete) → ftl_mngt_call_dev_shutdown
 * → ftl_mngt_process_execute(desc_shutdown 또는 desc_fast_shutdown). 이후 step 콜백들은
 * FTL 코어 spdk_thread에서 순차 실행되며, 각 step은 비동기 완료 시
 * ftl_mngt_next_step()으로 다음 step을 트리거한다. 마지막 step 종료 후
 * caller 콜백(ftl_mngt_completion)이 호출되어 제어권을 spdk_ftl_dev_free에 반환.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: 본 파일은 ftl_mngt_steps.h가 노출하는 step 함수 포인터(L2P, P2L, MD, IO channel,
 *   core poller, bdev, statistics 모듈)들을 선언적으로 등록할 뿐 실제 로직은 호출 안 함.
 *   각 step 함수의 구현은 ftl_mngt_l2p.c, ftl_mngt_p2l.c, ftl_mngt_md.c, ftl_mngt_ioch.c,
 *   ftl_mngt_misc.c, ftl_mngt_bdev.c 등에 분산 위치.
 * - 의존받음: lib/ftl/ftl_core.c (spdk_ftl_dev_free, RPC handler), bdev_ftl 모듈.
 * - 데이터 흐름: shutdown 진행 동안 dev->layout.md[*]에 메타데이터 write가 발행되고
 *   완료 시 dev의 모든 자원이 해제된다. dev 구조체 자체의 free는 본 파이프라인 완료
 *   후 호출자가 수행.
 *
 * === 주요 함수/구조체 요약 ===
 * - desc_shutdown        : 정상 셧다운 step 배열. error_handler로 rollback_device 등록.
 * - desc_fast_shutdown   : 빠른 셧다운 step 배열. 메타데이터를 SHM에 두고 빠르게 종료.
 * - ftl_mngt_call_dev_shutdown(): 진입점 — fast_shutdown 플래그 보고 desc 선택 후
 *                                 process_execute() 호출.
 *
 * 두 desc의 step 순서 의미(역순 정리 패턴):
 *   1) Deinit IO channel  — 새로운 호스트 I/O가 코어 스레드에 들어오지 못하게 차단
 *   2) Unregister IO device — spdk_io_device_unregister로 채널 객체 해제
 *   3) Stop core poller   — 코어 폴러 정지(이후 어떤 백그라운드 작업도 진행 안 됨)
 *   4) Persist L2P / Trim — L2P 영속화와 누적 트림 반영(clean only)
 *   5) Persist MD / Set clean — 메타데이터 디스크 flush + clean shutdown 마크
 *   6) Dump stats         — 통계 출력
 *   7) Deinit L2P / P2L   — 메모리 자원 해제
 *   8) Rollback device    — 남은 startup-단 자원(bdev, mempool, layout 등) 모두 해제
 */

#include "ftl_core.h"
/* [한국어] spdk_ftl_dev 정의 — dev->conf.fast_shutdown 등 셧다운 모드 플래그 접근에 필요. */
#include "ftl_mngt.h"
/* [한국어] ftl_mngt_process_execute / ftl_mngt_completion 등 process 실행 API. */
#include "ftl_mngt_steps.h"
/* [한국어] step 함수 포인터들의 프로토타입(ftl_mngt_persist_md, ftl_mngt_deinit_l2p 등). */

/*
 * Steps executed during clean shutdown - includes persisting metadata and rolling
 * back any setup steps executed during startup (closing bdevs, io channels, etc)
 */
/*
 * [한국어]
 * desc_shutdown - 정상 clean shutdown 절차의 step 파이프라인 디스크립터.
 *
 * 의미: dev->conf.fast_shutdown == false인 일반 종료 경로. 모든 메타데이터(L2P, P2L,
 * 밴드 정보, 슈퍼블록)를 디스크에 완전 영속화한 뒤 자원을 해제한다. 다음 부팅에서
 * recovery를 건너뛰고 곧장 빠른 startup이 가능하도록 "clean" 마크를 설정한다.
 *
 * error_handler: ftl_mngt_rollback_device — step 도중 어느 단계에서든 실패가 발생하면
 * 자원 누수를 막기 위해 rollback_device가 호출된다(이미 만든 mempool, bdev, layout 등을
 * 모두 정리). 일반 step의 cleanup과 별개로 동작하는 추가 안전망.
 *
 * steps 배열 끝의 빈 `{}`는 ftl_mngt_process_execute()가 종단을 인식하기 위한
 * sentinel(action == NULL).
 */
static const struct ftl_mngt_process_desc desc_shutdown = {
	.name = "FTL shutdown",
	/* [한국어] 디버그/로그 출력에 사용되는 프로세스 이름. */
	.error_handler = ftl_mngt_rollback_device,
	/* [한국어] step 도중 fail_step이 호출되면 이 핸들러가 추가로 호출되어
	 * 디바이스 전체를 rollback(자원 해제)한다. */
	.steps = {
		{
			.name = "Deinit core IO channel",
			/* [한국어] dev->ioch(코어 스레드용 IO 채널) 해제 — 새 I/O 차단 첫 단계. */
			.action = ftl_mngt_deinit_io_channel
		},
		{
			.name = "Unregister IO device",
			/* [한국어] spdk_io_device_unregister 호출 — 모든 spdk_thread의
			 * channel destroy_cb를 비동기 트리거 후 unregister_cb로 완료. */
			.action = ftl_mngt_unregister_io_device
		},
		{
			.name = "Stop core poller",
			/* [한국어] dev->core_poller 정지 — GC/wbuf flush/리퍼 등 백그라운드 잡 중지. */
			.action = ftl_mngt_stop_core_poller
		},
		{
			.name = "Persist L2P",
			/* [한국어] L2P 매핑 테이블 → 디스크 MD region(L2P) flush. */
			.action = ftl_mngt_persist_l2p
		},
		{
			.name = "Finish L2P trims",
			/* [한국어] trim_map에 쌓인 trim 요청을 L2P unmap으로 일괄 반영. */
			.action = ftl_mngt_trim_l2p,
		},
		{
			.name = "Persist metadata",
			/* [한국어] 슈퍼블록·밴드 정보·NV cache MD 등 모든 메타데이터를 디스크에 flush. */
			.action = ftl_mngt_persist_md
		},
		{
			.name = "Set FTL clean state",
			/* [한국어] 슈퍼블록 dirty 비트를 clean으로 변경 — 다음 부팅에서 recovery 생략. */
			.action = ftl_mngt_set_clean
		},
		{
			.name = "Dump statistics",
			/* [한국어] write/read amplification, GC 횟수 등 운영 통계 로그 출력. */
			.action = ftl_mngt_dump_stats
		},
		{
			.name = "Deinitialize L2P",
			/* [한국어] L2P 인메모리 자원 해제 — persist 이후이므로 데이터 손실 없음. */
			.action = ftl_mngt_deinit_l2p
		},
		{
			.name = "Deinitialize P2L checkpointing",
			/* [한국어] P2L checkpoint 모듈 해제. */
			.action = ftl_mngt_p2l_deinit_ckpt
		},
		{
			.name = "Rollback FTL device",
			/* [한국어] startup에서 만든 모든 잔여 자원(layout, bands, mempools, bdev,
			 * superblock, NV cache)을 역순 cleanup으로 해제하는 만능 정리 step. */
			.action = ftl_mngt_rollback_device
		},
		{}
		/* [한국어] sentinel — action == NULL은 step 배열 끝을 의미. */
	}
};

/*
 * Steps executed during fast clean shutdown (shutting down to shared memory). Utilizes
 * minimum amount of metadata persistence and rolls back any setup steps executed during
 * startup (closing bdevs, io channels, etc)
 */
/*
 * [한국어]
 * desc_fast_shutdown - SHM 핸드오프용 빠른 셧다운 파이프라인 디스크립터.
 *
 * 의미: dev->conf.fast_shutdown == true일 때 사용. 프로세스 재시작/업그레이드 시
 * 모든 메타데이터를 SHM(/dev/shm)에 두고 새 프로세스가 그대로 픽업하도록 한다.
 * 디스크 flush를 최소화하므로 종료 시간이 짧다.
 *
 * desc_shutdown과의 차이:
 *  - L2P persist / trim / Set clean 단계 없음 (디스크에 안 씀)
 *  - "Persist metadata" 대신 "Fast persist metadata" (SHM-only flush)
 *  - "Set FTL clean state" 대신 "Set FTL SHM clean state" (SHM 마크만)
 *  - error_handler 미정의 — rollback_device가 마지막 step으로 항상 실행됨
 */
static const struct ftl_mngt_process_desc desc_fast_shutdown = {
	.name = "FTL fast shutdown",
	/* [한국어] 로그 식별용 프로세스 이름. */
	.steps = {
		{
			.name = "Deinit core IO channel",
			/* [한국어] 코어 IO 채널 해제 — 새 I/O 차단 첫 단계. */
			.action = ftl_mngt_deinit_io_channel
		},
		{
			.name = "Unregister IO device",
			/* [한국어] IO device unregister — 모든 채널 비동기 destroy 트리거. */
			.action = ftl_mngt_unregister_io_device
		},
		{
			.name = "Stop core poller",
			/* [한국어] 코어 폴러 정지 — 백그라운드 잡 중지. */
			.action = ftl_mngt_stop_core_poller
		},
		{
			.name = "Fast persist metadata",
			/* [한국어] 메타데이터를 SHM에만 flush(디스크 IO 회피). 다음 프로세스가
			 * 그대로 픽업해 즉시 startup 가능. */
			.action = ftl_mngt_fast_persist_md
		},
		{
			.name = "Set FTL SHM clean state",
			/* [한국어] SHM 슈퍼블록 영역에만 clean 마크 — 디스크는 dirty 그대로
			 * 둠으로써 SHM 픽업이 실패하면 정식 recovery로 fallback 가능. */
			.action = ftl_mngt_set_shm_clean
		},
		{
			.name = "Dump statistics",
			/* [한국어] 통계 로그 출력. */
			.action = ftl_mngt_dump_stats
		},
		{
			.name = "Deinitialize L2P",
			/* [한국어] L2P 메모리 해제(SHM 영역의 데이터는 그대로 보존). */
			.action = ftl_mngt_deinit_l2p
		},
		{
			.name = "Deinitialize P2L checkpointing",
			/* [한국어] P2L 체크포인트 모듈 해제. */
			.action = ftl_mngt_p2l_deinit_ckpt
		},
		{
			.name = "Rollback FTL device",
			/* [한국어] 잔여 자원 일괄 해제. */
			.action = ftl_mngt_rollback_device
		},
		{}
		/* [한국어] step 배열 종단 sentinel. */
	}
};

/*
 * [한국어]
 * ftl_mngt_call_dev_shutdown - FTL 디바이스 셧다운 진입점.
 *
 * @dev:     셧다운할 FTL 디바이스. dev->conf.fast_shutdown 플래그로 모드 결정.
 * @cb:      셧다운 완료 시 호출될 caller 콜백. 인자는 (dev, cb_cntx, status).
 * @cb_cntx: caller 컨텍스트 — cb의 두 번째 인자로 그대로 전달.
 * @return:  0 = process가 정상 시작됨(완료는 비동기 cb로). 비-0 = 시작 실패.
 *
 * 왜 필요한가: 외부(bdev_ftl, RPC, ftl_core 등)가 셧다운을 요청할 때 어떤 desc를
 * 쓸지 모르도록 캡슐화하기 위한 단일 진입점.
 *
 * 동작:
 *  1) dev->conf.fast_shutdown 플래그를 보고 desc_shutdown vs desc_fast_shutdown 선택.
 *  2) 선택된 desc를 ftl_mngt_process_execute에 넘겨 step 파이프라인 실행 시작.
 *
 * 실행 컨텍스트: 호출자(보통 RPC 핸들러나 dev_free 핸들러)가 FTL 코어 스레드 또는
 * spdk_thread_send_msg를 통해 본 함수를 코어 스레드에서 호출해야 한다. step들은
 * 코어 스레드에서 비동기로 진행.
 *
 * 호출 체인:
 *   spdk_ftl_dev_free() (or RPC bdev_ftl_delete) → ftl_mngt_call_dev_shutdown →
 *     ftl_mngt_process_execute(desc) → step 파이프라인 → cb(dev, cb_cntx, 0/err)
 */
int
ftl_mngt_call_dev_shutdown(struct spdk_ftl_dev *dev, ftl_mngt_completion cb, void *cb_cntx)
{
	const struct ftl_mngt_process_desc *pdesc;
	/* [한국어] 선택된 셧다운 desc 포인터를 보관할 로컬 변수. */

	if (dev->conf.fast_shutdown) {
		/* [한국어] SHM 핸드오프 모드 — 디스크 flush 최소화. */
		pdesc = &desc_fast_shutdown;
	} else {
		/* [한국어] 일반 clean shutdown — 모든 메타데이터 디스크 영속화. */
		pdesc = &desc_shutdown;
	}
	return ftl_mngt_process_execute(dev, pdesc, cb, cb_cntx);
	/* [한국어] step 파이프라인 시작. 0 반환 시 실행 시작 성공(완료는 cb로 비동기 통보). */
}
