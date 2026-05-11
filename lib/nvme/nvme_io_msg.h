/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * SPDK cuse
 */

/*
 * [한국어 설명] NVMe IO message 포워딩 내부 헤더 (nvme_io_msg.h)
 *
 * === 파일의 역할 ===
 * 외부(non-NVMe-driver) 컨텍스트에서 SPDK NVMe 컨트롤러에 admin/IO 명령을 보내야 할 때
 * 안전한 통로를 제공한다. SPDK NVMe 드라이버의 SQ/CQ 큐는 한 thread에서만 접근해야 하는데,
 * CUSE worker(외부 thread)나 다른 produce 모듈이 직접 doorbell을 두드릴 수 없으므로,
 * lockless ring에 (fn, ctx) 메시지를 넣고 NVMe 드라이버 thread가 polling으로 꺼내
 * fn(ctrlr, nsid, arg)을 자기 컨텍스트에서 실행하게 한다. 이 패턴이 nvme_io_msg이며,
 * 본 헤더는 그 producer/consumer API를 선언한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   외부 producer(예: CUSE ioctl thread)
 *     → nvme_io_msg_send(ctrlr, nsid, fn, arg)
 *       → ctrlr 내부 spdk_ring에 enqueue
 *   NVMe driver thread (per-ctrlr poller)
 *     → nvme_io_msg_process(ctrlr) 주기 호출
 *       → ring에서 dequeue → fn(ctrlr, nsid, arg) 실행
 *         → fn 내부에서 admin/IO command 직접 호출 가능 (안전)
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk_nvme_ctrlr 내부 ring(spdk/env.h spdk_ring 기반).
 * - 의존받음: nvme_cuse.c (CUSE ioctl 처리), 사용자 정의 producer 모듈.
 * - 데이터 흐름: lockless mpsc ring (외부 다중 producer → 단일 NVMe thread consumer).
 * - 공유 자료구조: struct spdk_nvme_io_msg(ring 슬롯), struct nvme_io_msg_producer
 *   (등록된 producer 리스트 — update/stop 콜백 보유).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_nvme_io_msg_fn       : 메시지 실행 콜백 시그니처.
 * - struct spdk_nvme_io_msg   : ring에 들어가는 메시지 슬롯.
 * - struct nvme_io_msg_producer: 등록된 producer 모듈 (update/stop 훅).
 * - nvme_io_msg_send          : 메시지 enqueue.
 * - nvme_io_msg_process       : NVMe thread에서 dequeue+실행.
 * - nvme_io_msg_ctrlr_register/unregister/detach/update: producer 라이프사이클 관리.
 */

#ifndef SPDK_NVME_IO_MSG_H_
#define SPDK_NVME_IO_MSG_H_
/* [한국어] 다중 포함 가드. */

typedef void (*spdk_nvme_io_msg_fn)(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				    void *arg);
/* [한국어] io_msg 콜백 시그니처.
 * @ctrlr: 메시지가 향한 NVMe 컨트롤러. NVMe 드라이버 thread 컨텍스트에서 안전하게 사용 가능.
 * @nsid : 대상 namespace ID (admin command면 무관, 0xFFFFFFFF로 전체 등).
 * @arg  : producer가 enqueue 시 함께 넣은 ctx (보통 동적 할당된 요청 컨텍스트).
 * 콜백은 NVMe driver thread에서 실행되므로 SPDK NVMe API를 자유롭게 호출 가능. */

struct spdk_nvme_io_msg {
	/* [한국어] ring에 enqueue되는 메시지 한 개의 페이로드.
	 * 큐는 lockless이므로 producer/consumer 사이의 동기화는 ring 자체가 담당. */

	struct spdk_nvme_ctrlr	*ctrlr;
	/* [한국어] 메시지가 향하는 컨트롤러.
	 * 설정자: nvme_io_msg_send(). 읽는 자: nvme_io_msg_process()의 dispatcher.
	 * 값 범위: 유효한 attach 상태의 컨트롤러 포인터. detach 진행 중일 수 있어
	 *   consumer 측에서 STATE 검사 후 사용. */

	uint32_t		nsid;
	/* [한국어] namespace ID (NVMe 1.x §1.6.1).
	 * 0xFFFFFFFF는 broadcast nsid, 0은 invalid. fn에서 어떤 namespace에 명령을
	 *   보낼지 결정할 때 사용. */

	spdk_nvme_io_msg_fn	fn;
	/* [한국어] consumer가 호출할 콜백.
	 * 설정자: send() 시점. 읽는 자: process() 시점. NULL이면 ring 손상 — 패닉 대상. */

	void			*arg;
	/* [한국어] 콜백에 전달할 ctx 포인터.
	 * 콜백이 free 또는 caller로 결과 반환할 책임을 가짐 — 라이프타임 계약은 producer/콜백 사이. */
};

struct nvme_io_msg_producer {
	/* [한국어] 등록된 producer(외부 모듈)의 메타데이터.
	 * 컨트롤러는 자신의 producer 리스트를 STAILQ로 들고 있으며, attach/detach 시
	 * 모든 producer에게 update/stop을 통보한다. */

	const char *name;
	/* [한국어] producer 식별 문자열 ("cuse" 등). 디버깅 출력용 — 중복 확인은 호출자 책임. */

	void (*update)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] 컨트롤러 상태 변화(예: namespace 추가/삭제) 통지 콜백.
	 * NVMe driver thread에서 호출되며, producer는 이 시점에 자신이 노출하는
	 * 인터페이스(예: CUSE namespace 캐릭터 디바이스)를 갱신해야 한다. NULL 허용. */

	void (*stop)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] 컨트롤러 detach 시 호출 — producer가 자원을 정리해야 한다.
	 * 호출 후 producer는 더 이상 send()를 발생시키면 안 된다. */

	STAILQ_ENTRY(nvme_io_msg_producer) link;
	/* [한국어] 컨트롤러의 producer STAILQ 링크 노드.
	 * 설정/리셋: register/unregister. 동기화: ctrlr 락 보호 하 변경. */
};

/*
 * [한국어]
 * nvme_io_msg_send - 외부 컨텍스트에서 컨트롤러로 io_msg를 enqueue.
 *
 * @ctrlr: 대상 컨트롤러.
 * @nsid : 대상 namespace ID.
 * @fn   : NVMe driver thread에서 실행할 콜백.
 * @arg  : 콜백에 전달할 ctx.
 * @return: 0 성공, -ENOMEM ring 가득 / -ENXIO 컨트롤러가 io_msg 미지원.
 *
 * lockless ring으로 enqueue만 하고 즉시 반환 — non-blocking.
 *
 * 호출 체인:
 *   외부 producer(CUSE 등) → [nvme_io_msg_send] → spdk_ring_enqueue
 */
int nvme_io_msg_send(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, spdk_nvme_io_msg_fn fn,
		     void *arg);

/**
 * Process IO message sent to controller from external module.
 *
 * This call process requests from the ring, send IO to an allocated qpair or
 * admin commands in its context. This call is non-blocking and intended to be
 * polled by SPDK thread to provide safe environment for NVMe request
 * completion sent by external module to controller.
 *
 * The caller must ensure that each controller is polled by only one thread at
 * a time.
 *
 * This function may be called at any point while the controller is attached to
 * the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return number of processed external IO messages.
 */
/*
 * [한국어]
 * nvme_io_msg_process - NVMe driver thread가 polling으로 ring에서 메시지를 꺼내 실행.
 *
 * @ctrlr: 자기 thread가 담당하는 컨트롤러.
 * @return: 이번 호출에서 처리한 메시지 개수 (poller가 0이면 idle 가산).
 *
 * 컨트롤러당 단일 consumer 보장: 호출자가 동일 ctrlr에 대해 동시에 여러 thread에서
 * 호출하지 않도록 직접 보장해야 한다(주로 admin poller가 자기 thread에서만 호출).
 */
int nvme_io_msg_process(struct spdk_nvme_ctrlr *ctrlr);

/*
 * [한국어]
 * nvme_io_msg_ctrlr_register - producer를 컨트롤러의 producer 리스트에 등록.
 *
 * @ctrlr           : 대상 컨트롤러.
 * @io_msg_producer : 호출자가 정적/동적 할당해 채운 producer 메타데이터.
 * @return          : 0 성공, 음수 errno (-EEXIST 등).
 *
 * 등록 후 attach 알림(update)이 즉시 전달되어 producer는 현재 namespace 상태를 학습한다.
 */
int nvme_io_msg_ctrlr_register(struct spdk_nvme_ctrlr *ctrlr,
			       struct nvme_io_msg_producer *io_msg_producer);

/*
 * [한국어]
 * nvme_io_msg_ctrlr_unregister - 등록 해제.
 *
 * @ctrlr           : 대상.
 * @io_msg_producer : 등록 시 사용한 동일 포인터.
 *
 * 호출 후 producer는 더 이상 update/stop 통지를 받지 않는다.
 */
void nvme_io_msg_ctrlr_unregister(struct spdk_nvme_ctrlr *ctrlr,
				  struct nvme_io_msg_producer *io_msg_producer);

/*
 * [한국어]
 * nvme_io_msg_ctrlr_detach - 컨트롤러 detach 시 모든 producer에 stop 통지하고 ring 비움.
 *
 * @ctrlr: detach 진행 중인 컨트롤러.
 *
 * 내부적으로 list 순회하며 producer->stop을 호출하고 자료구조를 정리한다.
 */
void nvme_io_msg_ctrlr_detach(struct spdk_nvme_ctrlr *ctrlr);

/*
 * [한국어]
 * nvme_io_msg_ctrlr_update - 컨트롤러 상태 변화(namespace add/remove 등)를 모든 producer에 전파.
 *
 * @ctrlr: 변화가 일어난 컨트롤러.
 *
 * 호출 체인: NVMe AER 처리 → [nvme_io_msg_ctrlr_update] → 각 producer->update
 */
void nvme_io_msg_ctrlr_update(struct spdk_nvme_ctrlr *ctrlr);

#endif /* SPDK_NVME_IO_MSG_H_ */
/* [한국어] 다중 포함 가드 종결. */
