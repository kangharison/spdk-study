/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] NVMe I/O 메시지 채널 (External Producer → SPDK Reactor) 구현 (nvme_io_msg.c)
 *
 * === 파일의 역할 ===
 * SPDK NVMe 드라이버에 "외부(non-SPDK) 스레드에서 발생한 I/O/admin 요청"을 안전하게
 * 전달하기 위한 메시지 채널 인프라를 구현한다. SPDK는 본래 polled-mode + 코어 affinity
 * 모델이라 "controller를 소유한 reactor 스레드"만이 admin/IO qpair를 만질 수 있는데,
 * NVMe 표준 외 부가 기능(예: opal/security 명령, namespace 관리, vendor-specific 요청)을
 * 외부 producer 모듈(예: bdev_nvme_opal, nvmf target의 일부 경로)이 보내야 할 때가 있다.
 * 이 파일은 그 producer가 lock-free ring(MP-SC)에 메시지를 enqueue하면, 나중에 controller
 * 소유 reactor가 매 polling tick마다 dequeue해서 자기 컨텍스트로 실제 함수(`fn(ctrlr, nsid, arg)`)를
 * 실행해주는 "비동기 message-passing" 패턴을 제공한다. ring(SPDK_RING_TYPE_MP_SC) +
 * 전용 io_qpair(`external_io_msgs_qpair`) + producer 등록 리스트(`io_producers` STAILQ)를
 * 핵심 자원으로 보유한다.
 *
 * **본 파일은 코드 수정 없이 한국어 주석만 추가/보강된 학습 사본**이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 일반 I/O 경로는 사용자 → spdk_nvme_ns_cmd_*() → 자기 코어의 qpair → 폴링이지만,
 * "controller를 소유하지 않은 외부 스레드"가 끼어들면 ring을 거치는 보조 경로를 탄다:
 *
 *   [외부 producer 모듈, 임의 pthread]                          [SPDK reactor (controller 소유)]
 *      nvme_io_msg_send(ctrlr, nsid, fn, arg)                       매 tick:
 *        → calloc(io)                                                  spdk_nvme_ctrlr_process_admin_completions
 *        → spdk_ring_enqueue(external_io_msgs)                          → 또는 nvme_ctrlr_proc 폴링 루프에서
 *      [enqueue 완료, producer는 즉시 리턴]                              → nvme_io_msg_process(ctrlr) 호출
 *                                                                        → spdk_ring_dequeue(최대 8개)
 *                                                                        → io->fn(ctrlr, io->nsid, io->arg) 실행
 *                                                                        → free(io)
 *
 * 호출 체인 (등록/해제):
 *   producer 모듈 init → nvme_io_msg_ctrlr_register
 *     → 첫 producer면 mutex_init + spdk_ring_create(MP-SC, 65536) +
 *       spdk_nvme_ctrlr_alloc_io_qpair(전용 admin/IO 작업용)
 *     → STAILQ_INSERT_TAIL(&ctrlr->io_producers)
 *   producer 모듈 fini → nvme_io_msg_ctrlr_unregister
 *     → STAILQ_REMOVE → 마지막 producer였다면 nvme_io_msg_ctrlr_detach(자원 일괄 정리)
 *
 * 실행 컨텍스트:
 *   - nvme_io_msg_send: **임의 pthread** (외부 producer). 동시 다수 producer 가능 →
 *     `external_io_msgs_lock` mutex로 ring producer 측 race 방지.
 *   - nvme_io_msg_process: **controller 소유 SPDK reactor 스레드 단독**. ring consumer 측은
 *     단일 스레드이므로 SPDK_RING_TYPE_MP_SC(Multi-Producer Single-Consumer)가 적합.
 *   - register/unregister/detach/update: ctrlr 락(`nvme_ctrlr_lock`)으로 보호되며,
 *     보통 controller init/fini 또는 hot-plug 경로에서 호출.
 *   - 모든 함수가 `spdk_process_is_primary()` 체크로 multi-process(secondary) 제외.
 *
 * === 타 모듈과의 연결 ===
 *  - **nvme_internal.h** — `struct spdk_nvme_ctrlr`의 `external_io_msgs`(ring),
 *    `external_io_msgs_qpair`(전용 qpair), `external_io_msgs_lock`(mutex),
 *    `io_producers`(STAILQ), `needs_io_msg_update`/`prepare_for_reset` 플래그를 사용.
 *  - **nvme_io_msg.h** — `struct nvme_io_msg_producer`(producer 콜백 vtable: name, update, stop)
 *    및 본 파일이 노출하는 함수 프로토타입 정의.
 *  - **spdk/env.h (DPDK ring 래퍼)** — `spdk_ring_create/enqueue/dequeue/free`. 내부적으로
 *    DPDK `rte_ring` 사용. MP-SC 모드는 다중 producer-단일 consumer 환경의 lockless 큐.
 *  - **nvme_qpair.c / nvme_ctrlr.c** — `spdk_nvme_ctrlr_alloc_io_qpair`,
 *    `spdk_nvme_qpair_process_completions`, `spdk_nvme_ctrlr_free_io_qpair`. 이 파일은
 *    외부 메시지 처리에 쓸 "전용" io_qpair를 생성·폴링·해제한다.
 *  - **이 파일을 사용하는 producer 모듈** — 대표적으로 `module/bdev/nvme/bdev_nvme_opal.c`
 *    (Opal security I/O), nvmf 등 reactor 외 스레드에서 admin/IO를 발사해야 하는 모듈.
 *  - **호출되는 진입점** — `nvme_io_msg_process`는 `nvme_ctrlr_process_active_ns_list`/
 *    `nvme_ctrlr_proc` 등 controller 폴링 루프 어딘가에서 매 tick 호출되어 ring을 비운다.
 *
 * 데이터 흐름:
 *   producer thread → calloc(spdk_nvme_io_msg) → ring put → reactor poll →
 *     ring get → io->fn(ctrlr, nsid, arg) [reactor 컨텍스트] → free(io) → 완료.
 *   fn 자체는 보통 그 안에서 `spdk_nvme_ctrlr_cmd_*`나 `spdk_nvme_ns_cmd_*`을 호출해
 *   external_io_msgs_qpair에 SQE를 submit하고, 완료는 그 다음 reactor tick에서 회수.
 *
 * === 주요 함수/구조체 요약 ===
 *  핵심 자료구조 (정의는 nvme_io_msg.h / nvme_internal.h):
 *    - struct spdk_nvme_io_msg: { ctrlr, nsid, fn, arg } — ring을 떠도는 단위 메시지.
 *    - struct nvme_io_msg_producer: { name, update(ctrlr), stop(ctrlr), link } — 외부
 *      producer 모듈이 등록하는 vtable. update/stop은 ctrlr reset/teardown 통지용.
 *    - ctrlr->external_io_msgs (spdk_ring*, 65536 슬롯, MP-SC) — 메시지 큐.
 *    - ctrlr->external_io_msgs_qpair (spdk_nvme_qpair*) — fn이 실제 명령을 submit할 전용 qpair.
 *    - ctrlr->external_io_msgs_lock (pthread_mutex) — producer enqueue 직렬화.
 *    - ctrlr->io_producers (STAILQ) — 등록된 producer 목록 (첫 등록 시 자원 lazy 할당).
 *
 *  주요 함수:
 *    [메시지 송수신]
 *    - nvme_io_msg_send — ★ 외부 producer가 호출. calloc + mutex 보호 ring put.
 *    - nvme_io_msg_process — ★ reactor가 호출. ring에서 최대 8개 dequeue → fn 실행 → free.
 *
 *    [producer 관리]
 *    - nvme_io_msg_ctrlr_register — producer 등록 (첫 등록 시 ring/qpair lazy 생성).
 *    - nvme_io_msg_ctrlr_unregister — producer 해제 (마지막이면 detach 호출).
 *    - nvme_io_msg_is_producer_registered — 중복 등록 방지용 STAILQ 선형 탐색 (static).
 *    - nvme_io_msg_ctrlr_update — 모든 producer의 update 콜백 호출 (예: ns 변경 통지).
 *    - nvme_io_msg_ctrlr_detach — 모든 producer stop + ring/qpair/mutex 일괄 해제.
 */

#include "nvme_internal.h"
/* [한국어] SPDK NVMe 드라이버 내부 정의 헤더 - struct spdk_nvme_ctrlr의 모든 필드
 * (external_io_msgs, external_io_msgs_qpair, external_io_msgs_lock, io_producers,
 * needs_io_msg_update, prepare_for_reset, is_resetting, NVME_CTRLR_ERRLOG 등)와
 * nvme_ctrlr_lock/unlock 함수 정의를 가져오기 위해 필수. */

#include "nvme_io_msg.h"
/* [한국어] 본 파일이 외부에 노출하는 인터페이스 헤더 - struct spdk_nvme_io_msg(ring 메시지),
 * struct nvme_io_msg_producer(producer vtable), nvme_io_msg_send/process/register/unregister/
 * update/detach 프로토타입 선언. nvme/nvme_internal.h에서 분리되어 있어 producer 모듈만
 * 이 헤더를 include해서 외부에서 SPDK NVMe로 메시지를 보낼 수 있다. */

#define SPDK_NVME_MSG_IO_PROCESS_SIZE 8
/* [한국어] nvme_io_msg_process가 한 번에 ring에서 dequeue할 메시지의 최대 개수.
 * 너무 작으면 외부 producer 처리 지연, 너무 크면 한 tick에서 reactor가 다른 일을 못함 →
 * 8은 NVMe admin/외부 명령 polling tick 균형점으로 채택된 값. ring 용량(65536)에 비해
 * 한참 작아 1회 dequeue로 ring을 다 비우지 않고 다음 tick에서 이어서 처리한다.
 * 이는 reactor 메인 루프의 "공평한 책임 분배(fair time-sharing)"를 위한 디자인 선택. */

/**
 * Send message to IO queue.
 */
/*
 * [한국어]
 * nvme_io_msg_send - 외부(non-SPDK) 스레드가 SPDK NVMe controller에 비동기 작업을 위탁
 *
 * @ctrlr: 메시지를 보낼 대상 NVMe controller. 호출자는 이 ctrlr이 valid 상태이며
 *         register가 완료되었음을 보장해야 한다 (그렇지 않으면 ring이 NULL).
 * @nsid:  대상 namespace ID (1-base). 0이면 controller-wide admin 명령 의도.
 *         메시지 콜백 fn에 그대로 전달되어 fn이 어느 namespace를 다룰지 결정.
 * @fn:    reactor 컨텍스트에서 실행될 콜백. 시그니처: void fn(ctrlr, nsid, arg).
 *         이 안에서 spdk_nvme_ctrlr_cmd_*/spdk_nvme_ns_cmd_* 등을 직접 호출 가능.
 * @arg:   fn에 그대로 전달될 사용자 컨텍스트 포인터.
 *
 * @return: 0 = ring enqueue 성공 (실제 fn 실행은 다음 reactor tick에 비동기로 일어남),
 *          -ENOMEM = io 메시지 calloc 실패 또는 ring 가득 참(매우 이례적, assert 트리거).
 *
 * 왜 필요한가: SPDK는 "controller를 소유한 reactor 스레드"만이 qpair에 명령을 발사할 수
 *   있다 (lockless 디자인). 외부 producer 모듈(e.g. Opal security, namespace 관리)이
 *   임의 pthread에서 admin/IO 작업을 발사하려면 reactor 컨텍스트로 우회해야 하고,
 *   이 함수는 그 우회 진입점이다.
 *
 * 동작 단계:
 *   1) external_io_msgs_lock mutex로 enqueue 직렬화 (다중 producer 가능).
 *   2) calloc으로 io 메시지 객체 동적 할당 (ring에 포인터만 들어가므로 별도 lifecycle).
 *   3) ctrlr/nsid/fn/arg를 io 객체에 채움.
 *   4) spdk_ring_enqueue로 MP-SC ring에 1개 삽입.
 *   5) 실패 시 io free + mutex unlock + -ENOMEM 반환.
 *   6) 성공 시 mutex unlock 후 즉시 0 반환 (실행은 reactor가 비동기 처리).
 *
 * 실행 컨텍스트: **임의 pthread (외부 producer)**. SPDK reactor 외부에서 호출 가능한
 *   몇 안 되는 NVMe 드라이버 API. mutex 사용은 SPDK lockless 원칙의 의도적 예외.
 *
 * caller: opal/security 모듈, nvmf 일부 경로 등 producer 측 외부 스레드.
 * callee: pthread_mutex_lock/unlock, calloc, spdk_ring_enqueue (DPDK rte_ring MP enqueue).
 *
 * 에러 경로: 메모리 부족이거나 ring full → -ENOMEM. 호출자는 retry 또는 producer 측에서
 *   backpressure 처리 필요. 65536 슬롯 ring이 가득 차는 상황은 reactor가 멈춰있는 등의
 *   비정상 상황이므로 assert(false)로 디버그 빌드에서 즉시 발견.
 *
 * 호출 체인:
 *   외부 producer 모듈(임의 pthread) → [nvme_io_msg_send] → spdk_ring_enqueue (DPDK MP-SC)
 *     → 별도 reactor tick에서 nvme_io_msg_process가 dequeue 후 fn 실행
 */
int
nvme_io_msg_send(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, spdk_nvme_io_msg_fn fn,
		 void *arg)
{
	int rc;
	/* [한국어] spdk_ring_enqueue 반환값 - 실제 enqueue된 메시지 개수.
	 * 1을 요청했으니 정상 시 1, ring이 가득 차면 0이 반환된다. */

	struct spdk_nvme_io_msg *io;
	/* [한국어] ring에 enqueue할 메시지 객체 포인터. ring은 객체 자체가 아닌
	 * 포인터만 보관하므로 calloc으로 별도 동적 할당하고, consumer 측에서 free한다. */

	/* Protect requests ring against preemptive producers */
	pthread_mutex_lock(&ctrlr->external_io_msgs_lock);
	/* [한국어] 다수의 외부 producer 스레드가 동시에 send를 호출할 수 있으므로
	 * mutex로 enqueue 구간 전체를 직렬화한다. ring은 MP-SC 모드라 lock-free
	 * 다중 producer를 자체 지원하지만, "calloc 실패 시 mutex 일관 처리"와
	 * "ring put 실패 시 io free 보장"을 위해 추가 lock을 두는 보수적 디자인. */

	io = (struct spdk_nvme_io_msg *)calloc(1, sizeof(struct spdk_nvme_io_msg));
	/* [한국어] 메시지 객체 동적 할당. calloc으로 모든 필드 0 초기화 → 안전성 확보.
	 * 이후 reactor 측 nvme_io_msg_process()가 dequeue 후 free()로 해제 책임짐. */

	if (!io) {
		/* [한국어] 메모리 부족 시 분기 - 시스템 OOM 등 비정상 상황. */
		NVME_CTRLR_ERRLOG(ctrlr, "IO msg allocation failed.");
		/* [한국어] ctrlr 단위 에러 로그 매크로 (SPDK_ERRLOG에 ctrlr 식별자 prefix 추가). */

		pthread_mutex_unlock(&ctrlr->external_io_msgs_lock);
		/* [한국어] 락을 잡고 있던 상태이므로 반드시 해제 후 리턴 (deadlock 방지). */

		return -ENOMEM;
		/* [한국어] errno 음수 컨벤션으로 OOM 통지 (SPDK/Linux 커널 컨벤션). */
	}

	io->ctrlr = ctrlr;
	/* [한국어] fn에 전달될 controller 포인터 - fn이 어느 ctrlr에 명령 발사할지 식별. */

	io->nsid = nsid;
	/* [한국어] fn에 전달될 namespace ID - fn이 어느 ns에 명령 발사할지 식별. */

	io->fn = fn;
	/* [한국어] reactor 컨텍스트에서 실행될 사용자 콜백 함수 포인터. */

	io->arg = arg;
	/* [한국어] fn 호출 시 그대로 전달될 producer-private 컨텍스트. */

	rc = spdk_ring_enqueue(ctrlr->external_io_msgs, (void **)&io, 1, NULL);
	/* [한국어] DPDK rte_ring 기반 MP-SC ring에 io 포인터 1개 삽입.
	 * 4번째 인자(NULL): free_space 출력 - 사용 안 함. ring은 lockless이지만
	 * 위 mutex로 calloc/enqueue/free 일관성을 묶어두었다. */

	if (rc != 1) {
		/* [한국어] 1을 요청했는데 1이 안 들어감 = ring full. 65536 슬롯이
		 * 가득 차는 건 reactor가 polling을 멈췄거나 producer 폭주 상황. */

		assert(false);
		/* [한국어] 디버그 빌드에서 즉시 abort - 정상 동작에서는 발생 불가능한 상황.
		 * 릴리즈 빌드에서는 무시되고 아래 -ENOMEM 반환으로 graceful fallback. */

		free(io);
		/* [한국어] enqueue 실패 → 메시지 객체 누수 방지를 위해 즉시 해제. */

		pthread_mutex_unlock(&ctrlr->external_io_msgs_lock);
		/* [한국어] 에러 경로에서도 mutex 해제 보장 (deadlock 방지). */

		return -ENOMEM;
		/* [한국어] ring full을 호출자에게 OOM류 에러로 통지. */
	}

	pthread_mutex_unlock(&ctrlr->external_io_msgs_lock);
	/* [한국어] 정상 경로의 mutex 해제 - 다음 producer가 enqueue 가능. */

	return 0;
	/* [한국어] enqueue 성공. 실제 fn 실행은 controller 소유 reactor의 다음 tick에서
	 * nvme_io_msg_process를 통해 일어난다 (비동기 fire-and-forget 패턴). */
}

/*
 * [한국어]
 * nvme_io_msg_process - reactor가 매 tick 호출, ring에서 외부 메시지 수확 + 전용 qpair 폴링
 *
 * @ctrlr: 처리할 controller. 호출자는 controller 소유 reactor 스레드여야 한다 (단일 consumer).
 *
 * @return: 이번 호출에서 dequeue+실행한 메시지 개수 (0 ~ SPDK_NVME_MSG_IO_PROCESS_SIZE).
 *          0이면 ring 비어있거나 ready 상태가 아님 → 호출자가 다른 작업으로 진행 가능.
 *
 * 왜 필요한가: nvme_io_msg_send로 enqueue된 메시지를 실제 controller 컨텍스트에서
 *   실행하는 consumer 측 진입점. 동시에 외부 메시지가 submit한 명령의 완료를 위해
 *   전용 qpair(`external_io_msgs_qpair`)도 함께 폴링한다 (1석2조).
 *
 * 동작 단계:
 *   1) primary process 체크 - secondary process(multi-process 환경의 보조 프로세스)에서는
 *      ring/qpair가 primary가 소유하므로 즉시 0 반환.
 *   2) 자원 가용성 체크 - ring/qpair가 아직 lazy 생성 안 됐거나(producer 미등록) 또는
 *      reset 준비 중(prepare_for_reset)이면 안전하게 skip.
 *   3) needs_io_msg_update 플래그 체크 - secondary가 update를 요청했었다면 primary가
 *      대신 모든 producer의 update 콜백 실행.
 *   4) 전용 qpair에서 완료 수확 (이전 tick에서 fn이 submit한 명령들의 CQE).
 *   5) ring에서 최대 8개 dequeue.
 *   6) 각 io에 대해 io->fn(ctrlr, io->nsid, io->arg) 호출 + free(io).
 *
 * 실행 컨텍스트: **controller 소유 SPDK reactor 스레드 (단일 consumer)**.
 *   ring은 SPDK_RING_TYPE_MP_SC이므로 consumer가 1개임을 전제로 lock-free 작동.
 *   이 함수는 controller polling 루프(nvme_ctrlr_proc 등)에서 매 tick 호출됨.
 *
 * caller: nvme_ctrlr_process_io_msgs/nvme_ctrlr 폴링 루프.
 * callee: spdk_process_is_primary, nvme_io_msg_ctrlr_update,
 *   spdk_nvme_qpair_process_completions, spdk_ring_dequeue, io->fn(사용자 콜백), free.
 *
 * 에러 경로: io->fn 내부 에러는 fn 자체가 처리. 이 함수 자체는 에러를 반환하지 않음 (count만).
 *   ring/qpair 미준비 상태는 에러가 아니라 정상적인 idle로 간주.
 *
 * 호출 체인:
 *   reactor 메인 루프 → nvme_ctrlr 폴링 → [nvme_io_msg_process]
 *     → spdk_nvme_qpair_process_completions (전용 qpair CQE 수확)
 *     → spdk_ring_dequeue (외부 producer 메시지 수확)
 *     → io->fn (사용자 콜백 실행, 이 안에서 또 spdk_nvme_*cmd_* 발사 가능)
 */
int
nvme_io_msg_process(struct spdk_nvme_ctrlr *ctrlr)
{
	int i;
	/* [한국어] 메시지 배열 순회 인덱스. */

	int count;
	/* [한국어] spdk_ring_dequeue가 실제로 가져온 메시지 개수 (요청은 8, 실제는 0~8). */

	struct spdk_nvme_io_msg *io;
	/* [한국어] 루프 내에서 현재 처리 중인 메시지 포인터 (requests[i]를 캐스팅 보관). */

	void *requests[SPDK_NVME_MSG_IO_PROCESS_SIZE];
	/* [한국어] dequeue된 메시지 포인터들을 받을 스택 배열. void* 배열인 이유는
	 * spdk_ring API가 타입-제너릭(rte_ring 호환)이기 때문. 8개 고정 크기. */

	if (!spdk_process_is_primary()) {
		/* [한국어] SPDK는 multi-process(primary + secondary) 환경 지원 - secondary는
		 * primary가 만든 ring/qpair에 접근하지 않는다. primary만 외부 메시지 처리. */
		return 0;
	}

	if (!ctrlr->external_io_msgs || !ctrlr->external_io_msgs_qpair || ctrlr->prepare_for_reset) {
		/* Not ready or pending reset */
		/* [한국어] 3가지 skip 조건:
		 *  - external_io_msgs == NULL: 아직 producer가 한 번도 등록 안 됨 (lazy 할당).
		 *  - external_io_msgs_qpair == NULL: 전용 qpair 미생성 (drift 또는 detach 중).
		 *  - prepare_for_reset == true: controller reset 준비 중 → 새 명령 발사 금지.
		 * 어떤 경우든 ring을 만지면 안 되므로 idle처럼 0 반환하고 즉시 종료. */
		return 0;
	}

	if (ctrlr->needs_io_msg_update) {
		/* [한국어] secondary가 ns 변경 등을 감지하고 nvme_io_msg_ctrlr_update 호출했지만
		 * 본인은 처리할 수 없어 needs_io_msg_update=true로 표시만 해둔 상태.
		 * primary가 여기서 그 미완 작업을 인계받아 실제 update 실행. */

		ctrlr->needs_io_msg_update = false;
		/* [한국어] 처리 직전에 플래그를 먼저 끔 - update 중 중복 진입/누수 방지. */

		nvme_io_msg_ctrlr_update(ctrlr);
		/* [한국어] 등록된 모든 producer의 update(ctrlr) 콜백 호출 → ns 변경 반영 등. */
	}

	spdk_nvme_qpair_process_completions(ctrlr->external_io_msgs_qpair, 0);
	/* [한국어] 외부 메시지 콜백(fn)이 발사한 명령들의 CQE를 수확.
	 * max_completions=0은 "ring/CQ에 있는 만큼 모두 수확" 의미.
	 * 이 호출이 fn 콜백이 spdk_nvme_*cmd_*에 등록한 사용자 cb_fn을 트리거하여
	 * 외부 producer가 약속받은 완료 통지를 받게 된다. */

	count = spdk_ring_dequeue(ctrlr->external_io_msgs, requests,
				  SPDK_NVME_MSG_IO_PROCESS_SIZE);
	/* [한국어] MP-SC ring에서 최대 8개 dequeue. consumer가 단일 스레드라 lock-free.
	 * SPDK_NVME_MSG_IO_PROCESS_SIZE=8로 한 tick의 처리량을 제한 → 다른 reactor 작업
	 * 기아 방지 (fair time-sharing). */

	if (count == 0) {
		/* [한국어] ring이 비어있음 = 처리할 외부 메시지 없음. 정상적인 idle. */
		return 0;
	}

	for (i = 0; i < count; i++) {
		/* [한국어] dequeue된 만큼 순차 실행. ring은 FIFO이므로 producer 측 order 보존. */

		io = requests[i];
		/* [한국어] void* → spdk_nvme_io_msg* 변환. */

		assert(io != NULL);
		/* [한국어] ring에는 NULL 메시지가 들어갈 수 없음 (send에서 calloc 후 enqueue) -
		 * NULL이면 메모리 corruption. 디버그 빌드에서 즉시 abort. */

		io->fn(io->ctrlr, io->nsid, io->arg);
		/* [한국어] ★ 실제 사용자 콜백 실행. reactor 컨텍스트이므로 fn 내부에서
		 * spdk_nvme_ctrlr_cmd_*/spdk_nvme_ns_cmd_* 직접 호출 가능 (qpair affinity OK).
		 * fn은 비동기 명령을 submit만 하고 즉시 리턴, 완료는 다음 tick에서 위
		 * spdk_nvme_qpair_process_completions가 회수하여 사용자 cb_fn 트리거. */

		free(io);
		/* [한국어] send에서 calloc한 메시지 객체 해제. ring에서 빠진 시점이
		 * 메시지 lifecycle의 끝. */
	}

	return count;
	/* [한국어] 처리한 메시지 개수 반환 - 호출자(reactor)가 통계 또는 backpressure 판단에 사용. */
}

/*
 * [한국어]
 * nvme_io_msg_is_producer_registered - 동일 producer 객체가 이미 등록되어 있는지 확인 (static)
 *
 * @ctrlr:           검사 대상 controller (io_producers STAILQ 보유).
 * @io_msg_producer: 등록 여부를 확인하려는 producer 객체 포인터.
 *
 * @return: true = 이미 등록됨, false = 미등록.
 *
 * 왜 필요한가: nvme_io_msg_ctrlr_register/unregister가 STAILQ 중복 삽입/이중 삭제를
 *   방지하기 위한 가드. 같은 producer 객체를 두 번 INSERT_TAIL하면 STAILQ가 망가지므로
 *   register 진입 직후 이 함수로 -EEXIST 조기 반환, unregister는 missing skip 결정에 사용.
 *
 * 동작 방식: STAILQ_FOREACH로 io_producers 리스트를 선형 탐색하며 포인터 동일성 비교.
 *   producer 모듈은 보통 시스템에 한두 개라 O(N) 탐색이 충분.
 *
 * 실행 컨텍스트: nvme_ctrlr_lock 보호 하에서 호출됨 (caller 책임). 직접 락 잡지 않음.
 *
 * caller: nvme_io_msg_ctrlr_register, nvme_io_msg_ctrlr_unregister.
 * callee: STAILQ_FOREACH 매크로 (sys/queue.h, BSD 큐).
 *
 * 호출 체인:
 *   nvme_io_msg_ctrlr_(un)register → [nvme_io_msg_is_producer_registered] → STAILQ 순회
 */
static bool
nvme_io_msg_is_producer_registered(struct spdk_nvme_ctrlr *ctrlr,
				   struct nvme_io_msg_producer *io_msg_producer)
{
	struct nvme_io_msg_producer *tmp;
	/* [한국어] STAILQ 순회용 임시 포인터 - 각 iteration에서 현재 노드를 가리킴. */

	STAILQ_FOREACH(tmp, &ctrlr->io_producers, link) {
		/* [한국어] BSD 스타일 single-tail 큐 순회 매크로. 'link'는 producer 구조체에
		 * 정의된 STAILQ_ENTRY 필드명. ctrlr->io_producers는 STAILQ_HEAD. */

		if (tmp == io_msg_producer) {
			/* [한국어] 포인터 동일성으로 동일 producer 식별 - 등록 시 사용자가 넘긴
			 * 객체와 정확히 같은 인스턴스만 매치 (이름이 같아도 다른 객체면 false). */
			return true;
		}
	}
	return false;
	/* [한국어] 끝까지 못 찾음 = 미등록 상태. */
}

/*
 * [한국어]
 * nvme_io_msg_ctrlr_register - 외부 producer를 controller에 등록 (첫 등록 시 ring/qpair 생성)
 *
 * @ctrlr:           등록 대상 controller.
 * @io_msg_producer: producer 모듈이 제공하는 vtable 포인터 (name, update, stop 콜백 보유).
 *                   호출자가 lifetime을 관리해야 하며 unregister 전까지 유효해야 함.
 *
 * @return: 0 = 등록 성공, -EINVAL = NULL producer, -EEXIST = 중복 등록, -ENOMEM = 자원 부족.
 *
 * 왜 필요한가: 외부 메시지 채널은 자원(ring, qpair, mutex)이 비싸므로 항상 띄워두지 않고
 *   "첫 producer 등록 시 lazy 생성, 마지막 producer 해제 시 즉시 정리"하는 reference-counting
 *   유사 패턴을 쓴다. 이 함수는 그 lifecycle의 진입점.
 *
 * 동작 단계:
 *   1) producer NULL 검증.
 *   2) ctrlr 락 획득 (lifecycle race 방지).
 *   3) 중복 등록 검사 → -EEXIST.
 *   4) 이미 다른 producer가 있거나 reset 중이면 자원은 이미 있거나 reset 후 재생성됨 →
 *      STAILQ에만 추가하고 종료 (fast path).
 *   5) 첫 등록이고 reset 아님 → mutex_init + ring create (MP-SC, 65536) + io_qpair 할당.
 *   6) 자원 생성 실패 시 부분 cleanup 후 -ENOMEM 반환.
 *   7) 성공 시 STAILQ_INSERT_TAIL 후 0 반환.
 *
 * 실행 컨텍스트: 보통 producer 모듈 init 경로 (예: bdev_nvme_opal init). reactor 어디서든
 *   호출 가능하지만 ctrlr 락으로 직렬화. spdk_ring_create와 alloc_io_qpair는 EAL/DPDK
 *   메모리 핀 작업을 동반하므로 가벼운 함수가 아님 - init 시 1회 호출.
 *
 * caller: 외부 producer 모듈 (opal, nvmf 등) 초기화 코드.
 * callee: nvme_ctrlr_lock/unlock, nvme_io_msg_is_producer_registered,
 *   pthread_mutex_init, spdk_ring_create(DPDK rte_ring 래퍼),
 *   spdk_nvme_ctrlr_alloc_io_qpair(NVMe SQ/CQ 쌍 할당 + 컨트롤러 connect).
 *
 * 에러 경로: ring create 실패 → 락 풀고 ENOMEM. qpair 할당 실패 → ring free 후 NULL 처리,
 *   락 풀고 ENOMEM. STAILQ에는 아직 안 넣었으므로 producer 입장에서는 깨끗하게 미등록.
 *
 * 호출 체인:
 *   producer init → [nvme_io_msg_ctrlr_register]
 *     → spdk_ring_create (DPDK rte_ring)
 *     → spdk_nvme_ctrlr_alloc_io_qpair → 트랜스포트별 connect
 *     → STAILQ_INSERT_TAIL
 */
int
nvme_io_msg_ctrlr_register(struct spdk_nvme_ctrlr *ctrlr,
			   struct nvme_io_msg_producer *io_msg_producer)
{
	if (io_msg_producer == NULL) {
		/* [한국어] NULL producer는 무의미 - vtable 콜백을 호출할 수 없으므로 거부. */
		NVME_CTRLR_ERRLOG(ctrlr, "io_msg_producer cannot be NULL\n");
		return -EINVAL;
	}

	nvme_ctrlr_lock(ctrlr);
	/* [한국어] controller-wide 락 획득 - register/unregister/detach 간 race 방지.
	 * 내부적으로 pthread_mutex이며 SPDK 다중 init 경로에서 한 번에 한 producer만
	 * lifecycle 변경하도록 직렬화. */

	if (nvme_io_msg_is_producer_registered(ctrlr, io_msg_producer)) {
		/* [한국어] 같은 객체를 두 번 등록하려는 시도 - STAILQ 중복 삽입은 무한 루프
		 * 또는 corruption을 유발하므로 -EEXIST로 거부. */
		nvme_ctrlr_unlock(ctrlr);
		return -EEXIST;
	}

	if (!STAILQ_EMPTY(&ctrlr->io_producers) || ctrlr->is_resetting) {
		/* There are registered producers - IO messaging already started */
		/* [한국어] fast path 분기:
		 *  - io_producers가 비어있지 않음: 다른 producer가 이미 자원을 만들어둠 →
		 *    재생성 불필요, STAILQ에 한 항목만 추가하면 끝.
		 *  - is_resetting == true: controller reset 진행 중 → reset 완료 시 자원이
		 *    재생성될 것이므로 지금 만들어두면 leak. STAILQ 추가만 하고 reset 끝나면
		 *    nvme_io_msg_ctrlr_update가 통지받아 사용. */

		STAILQ_INSERT_TAIL(&ctrlr->io_producers, io_msg_producer, link);
		/* [한국어] producer 객체를 io_producers 리스트의 꼬리에 삽입. */

		nvme_ctrlr_unlock(ctrlr);
		return 0;
	}

	pthread_mutex_init(&ctrlr->external_io_msgs_lock, NULL);
	/* [한국어] 첫 producer 등록 - send 측 mutex 초기화. NULL = default attr.
	 * 이 mutex는 외부 producer 다중 호출의 enqueue 직렬화 전용. */

	/**
	 * Initialize ring and qpair for controller
	 */
	ctrlr->external_io_msgs = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 65536, SPDK_ENV_NUMA_ID_ANY);
	/* [한국어] DPDK rte_ring 기반 메시지 큐 생성.
	 *  - SPDK_RING_TYPE_MP_SC: Multi-Producer Single-Consumer (외부 다중 producer →
	 *    단일 reactor consumer 패턴에 정확히 맞는 모드, lockless).
	 *  - 65536: 슬롯 개수 (반드시 2의 거듭제곱). 외부 메시지 폭주 시에도 여유.
	 *  - SPDK_ENV_NUMA_ID_ANY: NUMA 노드 무관 - 어느 노드에서 할당해도 OK.
	 * hugepage 메모리에서 할당되어 캐시-친화적. */

	if (!ctrlr->external_io_msgs) {
		/* [한국어] hugepage 부족 또는 EAL 미초기화 시 실패. */
		NVME_CTRLR_ERRLOG(ctrlr, "Unable to allocate memory for message ring\n");
		nvme_ctrlr_unlock(ctrlr);
		return -ENOMEM;
	}

	ctrlr->external_io_msgs_qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	/* [한국어] 외부 메시지 fn이 사용할 전용 NVMe I/O qpair 할당.
	 *  - opts=NULL: 기본 옵션 (default qsize, priority 등).
	 *  - opts_size=0: opts NULL이므로 무관.
	 * 내부적으로 SQ/CQ 쌍을 디바이스에 생성하고 controller에 연결. 이 qpair는
	 * controller 소유 reactor에서만 사용되므로 lockless. */

	if (ctrlr->external_io_msgs_qpair == NULL) {
		/* [한국어] qpair 할당 실패 - 디바이스 자원 부족 또는 max_io_qpairs 초과. */
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_alloc_io_qpair() failed\n");

		spdk_ring_free(ctrlr->external_io_msgs);
		/* [한국어] 부분 cleanup - ring은 성공했으니 leak 방지 위해 즉시 해제. */

		ctrlr->external_io_msgs = NULL;
		/* [한국어] dangling pointer 방지 - process()가 NULL 체크로 skip하도록. */

		nvme_ctrlr_unlock(ctrlr);
		return -ENOMEM;
	}

	STAILQ_INSERT_TAIL(&ctrlr->io_producers, io_msg_producer, link);
	/* [한국어] 모든 자원 준비 완료 → producer를 리스트에 추가. 이 시점부터
	 * producer는 nvme_io_msg_send를 호출 가능. */

	nvme_ctrlr_unlock(ctrlr);

	return 0;
}

/*
 * [한국어]
 * nvme_io_msg_ctrlr_update - 등록된 모든 producer에게 controller 변경 통지
 *
 * @ctrlr: 변경이 일어난 controller.
 *
 * @return: 없음 (void). 통지는 best-effort.
 *
 * 왜 필요한가: namespace attach/detach, controller reset 완료 등 producer가 알아야 할
 *   상태 변경이 일어났을 때 producer가 자체 캐시를 갱신할 기회를 준다.
 *   예: opal/nvmf producer가 ns 리스트를 캐시한 경우 ns 변경 시 재구성 필요.
 *
 * 동작 단계:
 *   1) primary 프로세스 체크 - secondary는 producer 콜백 직접 호출 권한이 없으므로
 *      needs_io_msg_update 플래그를 켜두고 다음 nvme_io_msg_process 시 primary가 처리.
 *   2) ctrlr 락 획득 후 STAILQ 순회 - 각 producer의 update(ctrlr) 콜백 호출.
 *
 * 실행 컨텍스트: 보통 controller reset 후 또는 ns 변경 감지 경로에서 호출. update 콜백은
 *   ctrlr 락을 잡은 상태로 호출되므로 콜백 내부에서 같은 ctrlr 락 재진입 금지.
 *
 * caller: nvme_ctrlr.c의 ns 갱신 경로, reset 완료 콜백, 또는 nvme_io_msg_process가
 *   needs_io_msg_update를 처리할 때.
 * callee: io_msg_producer->update(ctrlr) - producer 모듈이 제공한 콜백.
 *
 * 호출 체인:
 *   nvme_ctrlr_*ns_changed/reset_complete 또는 nvme_io_msg_process(needs flag)
 *     → [nvme_io_msg_ctrlr_update] → STAILQ_FOREACH → producer->update(ctrlr)
 */
void
nvme_io_msg_ctrlr_update(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_io_msg_producer *io_msg_producer;
	/* [한국어] STAILQ 순회용 producer 포인터. */

	if (!spdk_process_is_primary()) {
		/* [한국어] secondary 프로세스는 producer 콜백을 직접 호출할 수 없음 - 자원
		 * (ring, qpair)을 primary가 소유하므로. 대신 needs_io_msg_update 플래그를 켜고
		 * 다음 nvme_io_msg_process(primary 컨텍스트)에서 처리되도록 위임. */

		ctrlr->needs_io_msg_update = true;
		/* [한국어] 1바이트 플래그 - process()의 다음 호출에서 false로 리셋되며 update 실행. */

		return;
	}

	/* Update all producers */
	nvme_ctrlr_lock(ctrlr);
	/* [한국어] producer 리스트 변경 race 방지. update 콜백 실행 중 다른 스레드가
	 * register/unregister하면 STAILQ_FOREACH가 깨지므로 락 필수. */

	STAILQ_FOREACH(io_msg_producer, &ctrlr->io_producers, link) {
		/* [한국어] 등록된 모든 producer 순차 통지. 순서는 register 순. */

		io_msg_producer->update(ctrlr);
		/* [한국어] producer 모듈의 update 콜백 호출 - 보통 내부 ns 캐시 재구성 등.
		 * 콜백은 ctrlr 락 보유 상태에서 실행되므로 가벼워야 하며 ctrlr 락 재진입 불가. */
	}
	nvme_ctrlr_unlock(ctrlr);
}

/*
 * [한국어]
 * nvme_io_msg_ctrlr_detach - 외부 메시지 채널 일괄 정리 (모든 producer stop + 자원 해제)
 *
 * @ctrlr: 정리 대상 controller.
 *
 * @return: 없음 (void). 정리는 best-effort, 실패해도 계속 진행.
 *
 * 왜 필요한가: controller 종료(unregister last producer 또는 ctrlr destruct) 시 ring/qpair/
 *   mutex/producer 리스트를 한꺼번에 정리하는 단일 진입점. 부분 누수 방지.
 *
 * 동작 단계:
 *   1) primary 프로세스 체크 - secondary는 자원 소유자가 아니므로 skip.
 *   2) STAILQ_FOREACH_SAFE로 producer 리스트 순회 - 각 producer의 stop 콜백 호출 후 즉시 REMOVE.
 *      _SAFE 매크로로 순회 중 삭제 안전성 확보.
 *   3) external_io_msgs ring이 있으면 free + NULL.
 *   4) external_io_msgs_qpair가 있으면 SQ/CQ 해제 + NULL.
 *   5) external_io_msgs_lock mutex destroy.
 *
 * 실행 컨텍스트: nvme_io_msg_ctrlr_unregister가 마지막 producer 제거 후 호출, 또는
 *   controller destruct 경로에서 호출. ctrlr 락은 caller가 잡고 들어옴 (또는 caller가
 *   destruct 컨텍스트라 race 없음).
 *
 * 주의: 이 함수는 ctrlr 락을 추가로 잡지 않는다 (caller 책임). STAILQ_FOREACH_SAFE도
 *   caller 락에 의존.
 *
 * caller: nvme_io_msg_ctrlr_unregister (마지막 producer 해제 시), ctrlr destruct.
 * callee: io_msg_producer->stop, spdk_ring_free, spdk_nvme_ctrlr_free_io_qpair,
 *   pthread_mutex_destroy.
 *
 * 호출 체인:
 *   ctrlr destruct/마지막 unregister → [nvme_io_msg_ctrlr_detach]
 *     → producer->stop (모든 producer)
 *     → spdk_ring_free / free_io_qpair / mutex_destroy
 */
void
nvme_io_msg_ctrlr_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_io_msg_producer *io_msg_producer, *tmp;
	/* [한국어] _SAFE 순회용 - io_msg_producer는 현재 노드, tmp는 다음 노드 백업.
	 * STAILQ_REMOVE 호출 후에도 다음 iteration이 안전하게 진행되도록. */

	if (!spdk_process_is_primary()) {
		/* [한국어] secondary는 자원 미소유 - detach해도 의미 없고 primary 자원 망가뜨림. */
		return;
	}

	/* Stop all producers */
	STAILQ_FOREACH_SAFE(io_msg_producer, &ctrlr->io_producers, link, tmp) {
		/* [한국어] _SAFE 매크로 - 순회 중 현재 노드 삭제(STAILQ_REMOVE) 안전 보장.
		 * 일반 STAILQ_FOREACH는 next 포인터를 매 iteration마다 재계산하므로 삭제하면 깨진다. */

		io_msg_producer->stop(ctrlr);
		/* [한국어] producer 모듈에 "곧 자원이 해제됩니다" 통지 - producer는 미완료
		 * 내부 작업 정리, 자체 캐시 해제 등 수행. stop은 동기 함수여야 함. */

		STAILQ_REMOVE(&ctrlr->io_producers, io_msg_producer, nvme_io_msg_producer, link);
		/* [한국어] producer를 리스트에서 제거. 3번째 인자는 STAILQ_ENTRY를 보유한
		 * 구조체 타입 이름 (nvme_io_msg_producer). 4번째는 link 필드명. */
	}

	if (ctrlr->external_io_msgs) {
		/* [한국어] register에서 ring 생성 실패 후 detach가 호출될 가능성 대비 NULL 가드. */

		spdk_ring_free(ctrlr->external_io_msgs);
		/* [한국어] DPDK rte_ring 자원 해제. ring 안에 남은 메시지가 있다면 leak되지만,
		 * detach 전에 process()가 비웠을 것이라 가정 (또는 dropping 의도). */

		ctrlr->external_io_msgs = NULL;
		/* [한국어] dangling 방지 - process()가 NULL 체크로 skip. */
	}

	if (ctrlr->external_io_msgs_qpair) {
		/* [한국어] register 부분 실패 시 qpair NULL일 수 있어 가드. */

		spdk_nvme_ctrlr_free_io_qpair(ctrlr->external_io_msgs_qpair);
		/* [한국어] NVMe SQ/CQ 쌍 해제 + 디바이스에 deletion 명령 전송 + 메모리 unmap. */

		ctrlr->external_io_msgs_qpair = NULL;
	}

	pthread_mutex_destroy(&ctrlr->external_io_msgs_lock);
	/* [한국어] send 측 mutex 해제. 이 시점에는 producer가 모두 제거되었으므로
	 * 더 이상 send 호출자가 없다고 가정. 만약 race로 누가 들어오면 정의되지 않은 동작. */
}

/*
 * [한국어]
 * nvme_io_msg_ctrlr_unregister - 외부 producer를 controller에서 해제 (마지막이면 자원 정리)
 *
 * @ctrlr:           해제 대상 controller.
 * @io_msg_producer: 해제할 producer 객체. register 시 넘긴 것과 동일 포인터여야 함.
 *
 * @return: 없음 (void). 미등록 producer 해제는 silent skip.
 *
 * 왜 필요한가: register와 짝을 이루는 lifecycle 종료 함수. 미사용 자원을 즉시 해제해야
 *   메모리/디바이스 자원(SQ/CQ) 누수 방지. reference counting과 유사하게 마지막 producer가
 *   해제될 때 ring/qpair까지 일괄 정리.
 *
 * 동작 단계:
 *   1) producer NULL 검증 (assert).
 *   2) ctrlr 락 획득.
 *   3) 미등록 producer면 silent return (idempotent하게 동작).
 *   4) STAILQ_REMOVE로 producer를 리스트에서 제거.
 *   5) 리스트가 비었으면 nvme_io_msg_ctrlr_detach 호출하여 ring/qpair/mutex 정리.
 *
 * 실행 컨텍스트: producer 모듈 fini 경로 (예: bdev_nvme_opal teardown). 보통 reactor 또는
 *   init/fini 스레드에서 호출. ctrlr 락으로 register와 직렬화.
 *
 * caller: 외부 producer 모듈 fini 코드.
 * callee: nvme_ctrlr_lock/unlock, nvme_io_msg_is_producer_registered, STAILQ_REMOVE,
 *   nvme_io_msg_ctrlr_detach (마지막인 경우만).
 *
 * 호출 체인:
 *   producer fini → [nvme_io_msg_ctrlr_unregister]
 *     → STAILQ_REMOVE
 *     → (마지막이면) nvme_io_msg_ctrlr_detach → ring_free/free_io_qpair/mutex_destroy
 */
void
nvme_io_msg_ctrlr_unregister(struct spdk_nvme_ctrlr *ctrlr,
			     struct nvme_io_msg_producer *io_msg_producer)
{
	assert(io_msg_producer != NULL);
	/* [한국어] NULL producer 해제는 호출자 버그 - 디버그 빌드에서 즉시 발견. */

	nvme_ctrlr_lock(ctrlr);
	/* [한국어] register/update/detach와 직렬화. STAILQ 변경은 반드시 이 락 보호 하에. */

	if (!nvme_io_msg_is_producer_registered(ctrlr, io_msg_producer)) {
		/* [한국어] 미등록 producer 해제 시도 - 이중 해제 또는 잘못된 객체.
		 * 에러로 만들지 않고 silent return으로 idempotent 보장 (방어적). */
		nvme_ctrlr_unlock(ctrlr);
		return;
	}

	STAILQ_REMOVE(&ctrlr->io_producers, io_msg_producer, nvme_io_msg_producer, link);
	/* [한국어] producer를 리스트에서 제거. detach와 달리 stop 콜백은 호출하지 않음 -
	 * unregister는 producer 모듈이 자체적으로 stop을 처리한 후 호출하는 정상 경로이므로. */

	if (STAILQ_EMPTY(&ctrlr->io_producers)) {
		/* [한국어] 마지막 producer가 빠졌으니 ring/qpair/mutex를 유지할 이유 없음 →
		 * detach 호출로 일괄 정리. 다음 register 시에 lazy 재생성됨. */

		nvme_io_msg_ctrlr_detach(ctrlr);
		/* [한국어] 위에서 STAILQ_REMOVE로 마지막 producer를 이미 뺐으므로 detach의
		 * STAILQ_FOREACH_SAFE 루프는 비어 있는 채로 통과 → ring/qpair/mutex만 정리. */
	}
	nvme_ctrlr_unlock(ctrlr);
}
