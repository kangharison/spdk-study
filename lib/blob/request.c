/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] Blobstore 요청(sequence/batch/user_op) 구현 (request.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 request.h에 선언된 모든 sequence/batch/user_op 함수의 구현체이다.
 * 핵심 책임은:
 *   1) 채널의 free pool(reqs TAILQ)에서 set을 꺼내고/반환하는 객체 풀 관리.
 *   2) 사용자 I/O를 main bs_dev 또는 backing bs_dev로 발행하고 완료 콜백을 등록.
 *   3) sequence 단계 진행 / batch 병렬 추적 / user_op 지연 실행을 위한 상태기계 운영.
 *   4) 최종 완료 시 cpl_type에 따라 적절한 사용자 콜백 시그니처로 디스패치 (bs_call_cpl).
 *   5) SPDK trace 이벤트 기록 (TRACE_BLOB_REQ_SET_START/COMPLETE).
 * lockless 설계: set은 채널 단위 풀이므로 한 SPDK thread만 그 풀을 만진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (사용자 read 예시):
 *   spdk_blob_io_read (사용자) → blobstore.c가 split 후
 *     → bs_sequence_start_blob → 채널 reqs에서 set 획득
 *     → bs_sequence_read_dev → channel->dev->read → NVMe/AIO 백엔드로 I/O
 *     → 완료 콜백 bs_sequence_completion → set->u.sequence.cb_fn (다음 단계로 진전)
 *     → 최종적으로 bs_sequence_finish → bs_request_set_complete → bs_call_cpl → 사용자
 * 실행 컨텍스트: 호스트 유저스페이스, blob/메타 thread (md_thread for 메타).
 * trace_record는 비동기 이벤트 추적용 — 실제 I/O 차단 없음.
 *
 * === 타 모듈과의 연결 ===
 * - blobstore.h / request.h: 모든 자료구조와 헤더 인터페이스.
 * - spdk/thread.h: spdk_io_channel_get_ctx — 외부 채널 → bs_channel ctx 변환.
 * - spdk/queue.h: TAILQ_* 매크로 (객체 풀 / 큐 관리).
 * - spdk/trace.h + spdk_internal/trace_defs.h: trace 이벤트 기록 — 성능 분석용.
 * - spdk/log.h: SPDK_DEBUGLOG (blob_rw 컴포넌트) — 디버그 빌드에서 lba/count 로깅.
 * - bs_dev 인터페이스 (struct spdk_bs_dev): read/write/readv/writev/unmap/write_zeroes/copy.
 *   bdev_nvme/aio/malloc 등 어떤 백엔드든 같은 인터페이스로 호출 가능.
 * 데이터 흐름:
 *   사용자 → blobstore.c → request.c (set 획득/발행) → bs_dev 백엔드 → 비동기 완료
 *     → request.c (완료 처리/콜백) → blobstore.c → 사용자.
 *
 * === 주요 함수/구조체 요약 ===
 * - bs_call_cpl: cpl->type에 따라 사용자 콜백 디스패치 (7가지 타입 switch).
 * - bs_request_set_complete: set 종료 처리 — reqs 풀에 반환 + bs_call_cpl.
 * - bs_sequence_completion / bs_batch_completion: bs_dev 콜백 어댑터.
 * - bs_sequence_start (static): 풀에서 set 획득 + 초기화 (bs/blob 공통).
 * - bs_sequence_start_bs / bs_sequence_start_blob: back_channel 결정 어댑터.
 * - bs_sequence_read_dev / write_dev / readv_dev / writev_dev / write_zeroes_dev / copy_dev /
 *   read_bs_dev / readv_bs_dev: I/O 발행 함수들.
 * - bs_sequence_finish / bs_user_op_sequence_finish: 종료 진입점.
 * - bs_batch_open / bs_batch_*_dev / bs_batch_close: batch 라이프사이클.
 * - bs_sequence_to_batch: sequence → batch 의미 변환.
 * - bs_user_op_alloc / bs_user_op_execute / bs_user_op_abort: 지연 사용자 I/O 큐.
 * - SPDK_LOG_REGISTER_COMPONENT(blob_rw): "blob_rw" 디버그 컴포넌트 등록.
 */

#include "spdk/stdinc.h"
/* [한국어] POSIX 표준 — calloc/free, uintptr_t (trace_record 인자 캐스팅에 필요) 등. */

#include "blobstore.h"
/* [한국어] 내부 헤더 — struct spdk_bs_channel, struct spdk_blob, blob_esnap_get_io_channel,
 * spdk_blob_is_esnap_clone, spdk_blob_io_read/write/* 등. */
#include "request.h"
/* [한국어] 본 파일에서 구현하는 sequence/batch/user_op 인터페이스 선언. */

#include "spdk/thread.h"
/* [한국어] spdk_io_channel_get_ctx, spdk_io_channel_from_ctx — io_channel과 그 ctx 변환. */
#include "spdk/queue.h"
/* [한국어] TAILQ_* 매크로 — set 풀(reqs)과 그 외 큐(need_cluster_alloc, queued_io) 조작. */
#include "spdk/trace.h"
/* [한국어] spdk_trace_record — SPDK 내부 trace 이벤트 발행 (perf 분석용). */

#include "spdk_internal/trace_defs.h"
/* [한국어] TRACE_BLOB_REQ_SET_START / TRACE_BLOB_REQ_SET_COMPLETE 같은 trace ID 정의. */
#include "spdk/log.h"
/* [한국어] SPDK_DEBUGLOG / SPDK_LOG_REGISTER_COMPONENT — blob_rw 디버그 컴포넌트 사용. */

/*
 * [한국어]
 * bs_call_cpl - cpl->type에 따라 사용자 콜백을 적절한 시그니처로 디스패치한다.
 *
 * @cpl: 완료 디스크립터 (보통 set->cpl을 스택 복사한 것)
 * @bserrno: 최종 결과 코드 (0=성공, <0=에러)
 *
 * 이 함수가 왜 필요한가: blobstore의 사용자 콜백은 7가지 시그니처가 있고
 * (bs/blob × basic/handle/id 조합 + nested), 어느 것을 호출할지 type으로 식별해야
 * 한다. 또한 실패 시(bserrno!=0) 결과 핸들/ID를 NULL/INVALID로 대체해 사용자가
 * 잘못된 객체를 참조하지 않도록 안전하게 처리한다.
 * 동작: switch(type)으로 7가지 분기 + NONE은 no-op.
 * 실행 컨텍스트: 보통 bs_request_set_complete 직전에 호출 → set 점유 thread.
 *
 * 호출 체인:
 *   bs_request_set_complete → bs_call_cpl(&local_cpl, bserrno) → cpl->u.*.cb_fn
 */
void
bs_call_cpl(struct spdk_bs_cpl *cpl, int bserrno)
{
	switch (cpl->type) {
	/* [한국어] 7가지 cpl_type에 따라 분기. */
	case SPDK_BS_CPL_TYPE_BS_BASIC:
		/* [한국어] BS 단순 op — (cb_arg, bserrno) 시그니처. */
		cpl->u.bs_basic.cb_fn(cpl->u.bs_basic.cb_arg,
				      bserrno);
		break;
	case SPDK_BS_CPL_TYPE_BS_HANDLE:
		/* [한국어] BS 핸들 반환 — 성공 시 bs 포인터, 실패 시 NULL 전달. */
		cpl->u.bs_handle.cb_fn(cpl->u.bs_handle.cb_arg,
				       bserrno == 0 ? cpl->u.bs_handle.bs : NULL,
				       /* [한국어] 삼항 연산자 — 실패 시 NULL 강제로 사용자가
				        * 유효하지 않은 BS를 참조하는 것을 방지. */
				       bserrno);
		break;
	case SPDK_BS_CPL_TYPE_BLOB_BASIC:
		/* [한국어] blob 단순 op — 가장 흔한 read/write/close 등. */
		cpl->u.blob_basic.cb_fn(cpl->u.blob_basic.cb_arg,
					bserrno);
		break;
	case SPDK_BS_CPL_TYPE_BLOBID:
		/* [한국어] blob_id 반환 — 성공 시 ID, 실패 시 SPDK_BLOBID_INVALID. */
		cpl->u.blobid.cb_fn(cpl->u.blobid.cb_arg,
				    bserrno == 0 ? cpl->u.blobid.blobid : SPDK_BLOBID_INVALID,
				    /* [한국어] 실패 시 INVALID로 사용자에게 명확한 실패 신호. */
				    bserrno);
		break;
	case SPDK_BS_CPL_TYPE_BLOB_HANDLE:
		/* [한국어] blob 핸들 반환 — 성공 시 blob*, 실패 시 NULL. */
		cpl->u.blob_handle.cb_fn(cpl->u.blob_handle.cb_arg,
					 bserrno == 0 ? cpl->u.blob_handle.blob : NULL,
					 /* [한국어] 실패 시 NULL — 잘못된 blob 핸들 사용 방지. */
					 bserrno);
		break;
	case SPDK_BS_CPL_TYPE_NESTED_SEQUENCE:
		/* [한국어] 중첩 sequence — 부모 sequence 포인터를 함께 전달. */
		cpl->u.nested_seq.cb_fn(cpl->u.nested_seq.cb_arg,
					cpl->u.nested_seq.parent,
					bserrno);
		break;
	case SPDK_BS_CPL_TYPE_NONE:
		/* this completion's callback is handled elsewhere */
		/* [한국어] 콜백 디스패치 안 함 — 호출자가 set 자체만 회수하고 별도 처리. */
		break;
	}
}

/*
 * [한국어]
 * bs_request_set_complete - set의 모든 활동이 끝난 시점의 종결 처리.
 *
 * @set: 완료된 set
 *
 * 동작:
 *   1) cpl과 bserrno를 로컬 변수로 복사 (이후 set이 풀로 반환되므로 stale 참조 방지).
 *   2) trace 이벤트 기록 (REQ_SET_COMPLETE).
 *   3) set을 채널의 reqs 풀로 반환 — 다음 사용자가 재사용 가능.
 *   4) 로컬 복사된 cpl로 bs_call_cpl 호출 (set은 이미 풀에 들어가 있어도 안전).
 * 실행 컨텍스트: 모든 sequence/batch 완료 경로의 마지막 단계. 단일 thread 점유.
 *
 * 왜 set을 먼저 풀에 반환하는가: 사용자 콜백이 즉시 새 sequence를 시작할 수 있고,
 * 그때 풀에서 set을 꺼내야 한다. 콜백 실행 전 풀에 반환하지 않으면 풀이 1개 비어 있는
 * 상태에서 콜백 안의 새 시작이 실패할 가능성이 있다.
 */
static void
bs_request_set_complete(struct spdk_bs_request_set *set)
{
	struct spdk_bs_cpl cpl = set->cpl;
	/* [한국어] cpl 본체 스택 복사 — 풀 반환 후에도 디스패치 가능하게. */
	int bserrno = set->bserrno;
	/* [한국어] 결과 코드 스택 복사. */

	spdk_trace_record(TRACE_BLOB_REQ_SET_COMPLETE, 0, 0, (uintptr_t)&set->cb_args,
			  (uintptr_t)set->cpl.u.blob_basic.cb_arg);
	/* [한국어] trace 이벤트 — &cb_args 주소가 set의 unique ID 역할. perf 분석 도구에서 START와 매칭.
	 * blob_basic.cb_arg는 가장 흔한 케이스라 식별자로 사용 (다른 타입에선 의미는 다르나 오버레이 위치 동일). */

	TAILQ_INSERT_TAIL(&set->channel->reqs, set, link);
	/* [한국어] 채널 풀 끝에 set 반환 — TAILQ는 lockless 단일 thread 가정. */

	bs_call_cpl(&cpl, bserrno);
	/* [한국어] 사용자 콜백 디스패치 — 위에서 복사한 로컬 cpl 사용. */
}

/*
 * [한국어]
 * bs_sequence_completion - bs_dev I/O 완료 콜백 (sequence용 어댑터).
 *
 * @channel: bs_dev가 보낸 채널 인자 (set->cb_args.channel과 동일)
 * @cb_arg: 시작 시 cb_args.cb_arg로 등록한 set 포인터
 * @bserrno: bs_dev I/O 결과
 *
 * 동작: bserrno를 set에 기록하고 sequence 사용자 콜백(set->u.sequence.cb_fn)을 호출.
 * 사용자 콜백은 보통 다음 단계 I/O를 발행하거나 bs_sequence_finish를 호출.
 * 실행 컨텍스트: bs_dev 완료 컨텍스트 (보통 같은 thread).
 *
 * 호출 체인:
 *   bs_dev->read/write 완료 → bs_sequence_completion → set->u.sequence.cb_fn
 *     → (다음 단계 또는 bs_sequence_finish)
 */
static void
bs_sequence_completion(struct spdk_io_channel *channel, void *cb_arg, int bserrno)
{
	struct spdk_bs_request_set *set = cb_arg;
	/* [한국어] cb_arg에서 set 포인터 회수. */

	set->bserrno = bserrno;
	/* [한국어] bs_dev 결과를 set에 누적 기록 — 사용자 콜백이 참조 가능. */
	set->u.sequence.cb_fn((spdk_bs_sequence_t *)set, set->u.sequence.cb_arg, bserrno);
	/* [한국어] sequence 단계 사용자 콜백 호출 — 다음 단계로 진전 또는 종료 결정.
	 * (spdk_bs_sequence_t *) 캐스팅은 typedef alias이므로 같은 포인터 값. */
}

/*
 * [한국어]
 * bs_sequence_start - sequence 시작의 공통 헬퍼 (static, inline).
 *
 * @_channel: 사용자 io_channel (외부 인터페이스)
 * @cpl: 최종 사용자 완료 디스크립터
 * @back_channel: backing dev I/O에 사용할 채널 (esnap 등 분기 결정 결과)
 * @return: 새 sequence 포인터 (set), 풀이 비었으면 NULL
 *
 * 동작:
 *   1) _channel의 ctx로부터 spdk_bs_channel 획득.
 *   2) reqs 풀에서 set 하나 획득 (TAILQ_FIRST + REMOVE) — 비었으면 NULL.
 *   3) trace 이벤트 START 기록.
 *   4) set 초기화: cpl 복사, bserrno=0, channel/back_channel/cb_args 설정.
 *   5) cb_args.cb_fn = bs_sequence_completion 어댑터로 등록.
 * 실행 컨텍스트: 단일 thread 점유 (채널 affinity).
 *
 * 호출 체인:
 *   bs_sequence_start_bs / bs_sequence_start_blob → bs_sequence_start
 */
static inline spdk_bs_sequence_t *
bs_sequence_start(struct spdk_io_channel *_channel, struct spdk_bs_cpl *cpl,
		  struct spdk_io_channel *back_channel)
{
	struct spdk_bs_channel		*channel;
	/* [한국어] 외부 io_channel의 내부 ctx (struct spdk_bs_channel) — 풀과 dev에 접근. */
	struct spdk_bs_request_set	*set;
	/* [한국어] 풀에서 꺼낸 set 객체. */

	channel = spdk_io_channel_get_ctx(_channel);
	/* [한국어] io_channel → bs_channel ctx 변환 (SPDK 표준 패턴 — io_channel 끝에 ctx가 붙어있음). */
	assert(channel != NULL);
	/* [한국어] 정상 경로에서는 채널이 항상 유효. */
	set = TAILQ_FIRST(&channel->reqs);
	/* [한국어] 풀의 첫 free set 획득 — TAILQ는 단일 thread 가정 lockless. */
	if (!set) {
		/* [한국어] 풀이 비어 있음 — queue depth 초과로 즉시 시작 불가. 호출자가 재시도 결정. */
		return NULL;
	}
	TAILQ_REMOVE(&channel->reqs, set, link);
	/* [한국어] 풀에서 제거 — 이제 set은 활성 상태. 완료 시 다시 INSERT_TAIL로 반환. */

	spdk_trace_record(TRACE_BLOB_REQ_SET_START, 0, 0, (uintptr_t)&set->cb_args,
			  (uintptr_t)cpl->u.blob_basic.cb_arg);
	/* [한국어] START 이벤트 기록 — 이후 COMPLETE 이벤트와 짝이 됨 (perf 분석에서 latency 계산). */

	set->cpl = *cpl;
	/* [한국어] 사용자 cpl 복사 (얕은 복사 — union 멤버 포함). */
	set->bserrno = 0;
	/* [한국어] 결과 코드 초기화 — 단계마다 누적될 예정. */
	set->channel = channel;
	/* [한국어] main 채널 저장 — main bs_dev I/O에 사용. */
	set->back_channel = back_channel;
	/* [한국어] backing 채널 저장 — back_bs_dev I/O에 사용 (esnap 등). */

	set->cb_args.cb_fn = bs_sequence_completion;
	/* [한국어] bs_dev 완료 어댑터 등록 — 모든 bs_dev I/O가 이 함수로 돌아온다. */
	set->cb_args.cb_arg = set;
	/* [한국어] 어댑터에게 set 자신을 컨텍스트로 전달. */
	set->cb_args.channel = channel->dev_channel;
	/* [한국어] bs_dev I/O에 실제로 사용될 디바이스 채널 — bdev_module이 사용하는 채널. */
	set->ext_io_opts = NULL;
	/* [한국어] ext I/O 옵션 초기값 NULL — 사용자가 ext 변형을 호출하면 별도로 설정됨. */

	return (spdk_bs_sequence_t *)set;
	/* [한국어] sequence 포인터로 반환 (typedef alias). */
}

/* Use when performing IO directly on the blobstore (e.g. metadata - not a blob). */
/*
 * [한국어]
 * bs_sequence_start_bs - blobstore 메타데이터(슈퍼블록, 메타페이지 등)에 대한 sequence 시작.
 *
 * @_channel: 사용자 io_channel
 * @cpl: 완료 디스크립터
 * @return: 새 sequence (실패 시 NULL)
 *
 * 메타데이터 I/O는 main bs_dev에서만 일어나므로 back_channel = _channel로 전달.
 * 단순히 bs_sequence_start의 얇은 래퍼.
 */
spdk_bs_sequence_t *
bs_sequence_start_bs(struct spdk_io_channel *_channel, struct spdk_bs_cpl *cpl)
{
	return bs_sequence_start(_channel, cpl, _channel);
	/* [한국어] back_channel = _channel — 메타 I/O는 backing dev를 거치지 않음. */
}

/* Use when performing IO on a blob. */
/*
 * [한국어]
 * bs_sequence_start_blob - 특정 blob에 대한 I/O sequence 시작.
 *
 * @_channel: 사용자 io_channel
 * @cpl: 완료 디스크립터
 * @blob: 대상 blob
 * @return: 새 sequence (esnap 채널 획득 실패 또는 풀 비었으면 NULL)
 *
 * 동작:
 *   1) blob이 esnap clone이면 외부 device 전용 채널을 획득해 back_channel로 사용.
 *   2) 일반 blob이면 _channel을 그대로 back_channel로 사용.
 *   3) bs_sequence_start로 위임.
 * 실행 컨텍스트: blob 소유 thread.
 */
spdk_bs_sequence_t *
bs_sequence_start_blob(struct spdk_io_channel *_channel, struct spdk_bs_cpl *cpl,
		       struct spdk_blob *blob)
{
	struct spdk_io_channel	*esnap_ch = _channel;
	/* [한국어] 기본값: 일반 blob은 back_channel = _channel. esnap이면 아래에서 덮어씀. */

	if (spdk_blob_is_esnap_clone(blob)) {
		/* [한국어] 외부 snapshot의 clone — backing device가 외부에 있으므로 별도 채널 필요. */
		esnap_ch = blob_esnap_get_io_channel(_channel, blob);
		/* [한국어] 외부 device 전용 채널 획득 (lazy 할당). 실패 시 NULL. */
		if (esnap_ch == NULL) {
			/*
			 * The most likely reason we are here is because of some logic error
			 * elsewhere that caused channel allocations to fail. We could get here due
			 * to being out of memory as well. If we are out of memory, the process is
			 * this will be just one of many problems that this process will be having.
			 * Killing it off debug builds now due to logic errors is the right thing to
			 * do and killing it off due to ENOMEM is no big loss.
			 */
			/* [한국어] 채널 할당 실패 — 일반적으로 logic error이거나 OOM.
			 * 디버그 빌드에서는 abort, 릴리스에서는 NULL 반환으로 호출자에게 실패 알림. */
			assert(false);
			return NULL;
		}
	}
	return bs_sequence_start(_channel, cpl, esnap_ch);
	/* [한국어] 결정된 back_channel(esnap_ch)로 sequence 시작. */
}

/*
 * [한국어]
 * bs_sequence_read_bs_dev - 임의의 bs_dev에 read 발행 (back_channel 사용).
 *
 * @seq: 진행 중 sequence
 * @bs_dev: 읽을 디바이스 (보통 back_bs_dev)
 * @payload: 결과 버퍼
 * @lba/@lba_count: 읽을 LBA 범위
 * @cb_fn/@cb_arg: 단계 완료 콜백
 *
 * 동작: set의 sequence 콜백 등록 후 bs_dev->read 호출 — 완료는 bs_sequence_completion으로.
 * back_channel을 사용하는 이유: backing dev는 esnap 같이 main bs_dev와 다른 device일 수
 * 있어 별도 채널이 필요.
 */
void
bs_sequence_read_bs_dev(spdk_bs_sequence_t *seq, struct spdk_bs_dev *bs_dev,
			void *payload, uint64_t lba, uint32_t lba_count,
			spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)seq;
	/* [한국어] sequence → set 캐스팅 (typedef alias). */
	struct spdk_io_channel		*back_channel = set->back_channel;
	/* [한국어] backing dev 채널 — 시작 시 결정된 값. */

	SPDK_DEBUGLOG(blob_rw, "Reading %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);
	/* [한국어] blob_rw 컴포넌트 디버그 로그 — 디버그 빌드에서 stderr 출력. */

	set->u.sequence.cb_fn = cb_fn;
	/* [한국어] 단계 완료 콜백 등록. */
	set->u.sequence.cb_arg = cb_arg;
	/* [한국어] 콜백 인자. */

	bs_dev->read(bs_dev, back_channel, payload, lba, lba_count, &set->cb_args);
	/* [한국어] backing dev에 read 발행 — 완료는 cb_args.cb_fn (= bs_sequence_completion)으로. */
}

/*
 * [한국어]
 * bs_sequence_read_dev - main bs_dev에 read 발행 (메타페이지 등).
 *
 * back_channel 대신 channel->dev/dev_channel 사용. 그 외는 read_bs_dev와 동일 패턴.
 */
void
bs_sequence_read_dev(spdk_bs_sequence_t *seq, void *payload,
		     uint64_t lba, uint32_t lba_count,
		     spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	/* [한국어] set 캐스팅. */
	struct spdk_bs_channel       *channel = set->channel;
	/* [한국어] main bs_channel — main bs_dev와 dev_channel을 들고 있다. */

	SPDK_DEBUGLOG(blob_rw, "Reading %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);
	/* [한국어] 디버그 로그. */

	set->u.sequence.cb_fn = cb_fn;
	/* [한국어] 단계 완료 콜백. */
	set->u.sequence.cb_arg = cb_arg;
	/* [한국어] 콜백 인자. */

	channel->dev->read(channel->dev, channel->dev_channel, payload, lba, lba_count, &set->cb_args);
	/* [한국어] main bs_dev에 read 발행. dev_channel은 백엔드(bdev)가 사용하는 채널. */
}

/*
 * [한국어]
 * bs_sequence_write_dev - main bs_dev에 write 발행.
 *
 * read_dev와 동일 패턴이지만 write 발행. 메타데이터 업데이트나 blob의 sync에서 사용.
 */
void
bs_sequence_write_dev(spdk_bs_sequence_t *seq, void *payload,
		      uint64_t lba, uint32_t lba_count,
		      spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	/* [한국어] set 캐스팅. */
	struct spdk_bs_channel       *channel = set->channel;
	/* [한국어] main 채널. */

	SPDK_DEBUGLOG(blob_rw, "Writing %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);
	/* [한국어] 디버그 로그. */

	set->u.sequence.cb_fn = cb_fn;
	/* [한국어] 콜백 등록. */
	set->u.sequence.cb_arg = cb_arg;
	/* [한국어] 콜백 인자. */

	channel->dev->write(channel->dev, channel->dev_channel, payload, lba, lba_count,
			    &set->cb_args);
	/* [한국어] main bs_dev에 write 발행. */
}

/*
 * [한국어]
 * bs_sequence_readv_bs_dev - 임의 bs_dev에 vectored read 발행.
 *
 * ext_io_opts가 설정되어 있으면 readv_ext 변형 호출, 아니면 readv. back_channel 사용.
 */
void
bs_sequence_readv_bs_dev(spdk_bs_sequence_t *seq, struct spdk_bs_dev *bs_dev,
			 struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
			 spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	/* [한국어] set 캐스팅. */
	struct spdk_io_channel		*back_channel = set->back_channel;
	/* [한국어] backing dev 채널. */

	SPDK_DEBUGLOG(blob_rw, "Reading %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);
	/* [한국어] 디버그 로그. */

	set->u.sequence.cb_fn = cb_fn;
	/* [한국어] 단계 완료 콜백. */
	set->u.sequence.cb_arg = cb_arg;
	/* [한국어] 인자. */

	if (set->ext_io_opts) {
		/* [한국어] 사용자 ext I/O 옵션이 설정되어 있으면 ext 변형 사용 (memory_domain 등). */
		assert(bs_dev->readv_ext);
		/* [한국어] 디바이스가 ext 변형을 지원해야 함을 확인. */
		bs_dev->readv_ext(bs_dev, back_channel, iov, iovcnt, lba, lba_count,
				  &set->cb_args, set->ext_io_opts);
		/* [한국어] ext 변형 호출 — ext_opts를 디바이스에 전달. */
	} else {
		/* [한국어] 일반 vectored read. */
		bs_dev->readv(bs_dev, back_channel, iov, iovcnt, lba, lba_count, &set->cb_args);
		/* [한국어] readv 호출. */
	}
}

/*
 * [한국어]
 * bs_sequence_readv_dev - main bs_dev에 vectored read 발행.
 *
 * readv_bs_dev와 동일 패턴이지만 main 채널/디바이스 사용.
 */
void
bs_sequence_readv_dev(spdk_bs_sequence_t *seq, struct iovec *iov, int iovcnt,
		      uint64_t lba, uint32_t lba_count, spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	/* [한국어] set 캐스팅. */
	struct spdk_bs_channel       *channel = set->channel;
	/* [한국어] main 채널. */

	SPDK_DEBUGLOG(blob_rw, "Reading %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);
	/* [한국어] 디버그 로그. */

	set->u.sequence.cb_fn = cb_fn;
	/* [한국어] 콜백 등록. */
	set->u.sequence.cb_arg = cb_arg;
	/* [한국어] 인자. */
	if (set->ext_io_opts) {
		/* [한국어] ext 변형 분기. */
		assert(channel->dev->readv_ext);
		/* [한국어] 디바이스 지원 확인. */
		channel->dev->readv_ext(channel->dev, channel->dev_channel, iov, iovcnt, lba, lba_count,
					&set->cb_args, set->ext_io_opts);
		/* [한국어] readv_ext 발행. */
	} else {
		/* [한국어] 일반 readv. */
		channel->dev->readv(channel->dev, channel->dev_channel, iov, iovcnt, lba, lba_count, &set->cb_args);
		/* [한국어] readv 발행. */
	}
}

/*
 * [한국어]
 * bs_sequence_writev_dev - main bs_dev에 vectored write 발행. ext 변형 분기 포함.
 */
void
bs_sequence_writev_dev(spdk_bs_sequence_t *seq, struct iovec *iov, int iovcnt,
		       uint64_t lba, uint32_t lba_count,
		       spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	/* [한국어] set 캐스팅. */
	struct spdk_bs_channel       *channel = set->channel;
	/* [한국어] main 채널. */

	SPDK_DEBUGLOG(blob_rw, "Writing %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);
	/* [한국어] 디버그 로그. */

	set->u.sequence.cb_fn = cb_fn;
	/* [한국어] 콜백. */
	set->u.sequence.cb_arg = cb_arg;
	/* [한국어] 인자. */

	if (set->ext_io_opts) {
		/* [한국어] ext 변형 분기. */
		assert(channel->dev->writev_ext);
		/* [한국어] 지원 확인. */
		channel->dev->writev_ext(channel->dev, channel->dev_channel, iov, iovcnt, lba, lba_count,
					 &set->cb_args, set->ext_io_opts);
		/* [한국어] writev_ext 발행. */
	} else {
		/* [한국어] 일반 writev. */
		channel->dev->writev(channel->dev, channel->dev_channel, iov, iovcnt, lba, lba_count,
				     &set->cb_args);
		/* [한국어] writev 발행. */
	}
}

/*
 * [한국어]
 * bs_sequence_write_zeroes_dev - main bs_dev에 WRITE_ZEROES 명령 발행.
 *
 * NVMe Write Zeroes (opcode 0x08)에 대응하는 추상화 — 백엔드가 실제 명령을 보내거나
 * 일반 write로 fallback하는지는 bdev 모듈 구현에 따른다.
 */
void
bs_sequence_write_zeroes_dev(spdk_bs_sequence_t *seq,
			     uint64_t lba, uint64_t lba_count,
			     spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	/* [한국어] set 캐스팅. */
	struct spdk_bs_channel       *channel = set->channel;
	/* [한국어] main 채널. */

	SPDK_DEBUGLOG(blob_rw, "writing zeroes to %" PRIu64 " blocks at LBA %" PRIu64 "\n",
		      lba_count, lba);
	/* [한국어] 디버그 로그. */

	set->u.sequence.cb_fn = cb_fn;
	/* [한국어] 콜백. */
	set->u.sequence.cb_arg = cb_arg;
	/* [한국어] 인자. */

	channel->dev->write_zeroes(channel->dev, channel->dev_channel, lba, lba_count,
				   &set->cb_args);
	/* [한국어] WRITE_ZEROES 발행. */
}

/*
 * [한국어]
 * bs_sequence_copy_dev - main bs_dev에 디바이스 내부 copy 명령 발행.
 *
 * NVMe Copy (opcode 0x19) 또는 SCSI EXTENDED COPY 등 디바이스 offload copy를 지원하는
 * bdev에서 사용. CoW 시 read+write 두 단계 대신 한 번에 처리해 효율을 높인다.
 */
void
bs_sequence_copy_dev(spdk_bs_sequence_t *seq, uint64_t dst_lba, uint64_t src_lba,
		     uint64_t lba_count, spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set *set = (struct spdk_bs_request_set *)seq;
	/* [한국어] set 캐스팅. */
	struct spdk_bs_channel     *channel = set->channel;
	/* [한국어] main 채널. */

	SPDK_DEBUGLOG(blob_rw, "Copying %" PRIu64 " blocks from LBA %" PRIu64 " to LBA %" PRIu64 "\n",
		      lba_count, src_lba, dst_lba);
	/* [한국어] 디버그 로그 — src→dst 정보 함께 기록. */

	set->u.sequence.cb_fn = cb_fn;
	/* [한국어] 콜백. */
	set->u.sequence.cb_arg = cb_arg;
	/* [한국어] 인자. */

	channel->dev->copy(channel->dev, channel->dev_channel, dst_lba, src_lba, lba_count, &set->cb_args);
	/* [한국어] copy 발행 — 디바이스 내부에서 src→dst LBA 복사. */
}

/*
 * [한국어]
 * bs_sequence_finish - sequence 종료 진입점.
 *
 * @seq: 종료할 sequence
 * @bserrno: 최종 결과 (0=성공, !=0=실패)
 *
 * 동작: bserrno가 0이 아니면 set->bserrno에 기록 (이전 누적 에러 유지 — 첫 에러 우선
 * 정책은 호출자가 결정), 그 후 bs_request_set_complete로 완료 처리.
 * 호출자: 사용자 콜백이 없으면 보통 sequence cb_fn 본문에서 직접 호출.
 */
void
bs_sequence_finish(spdk_bs_sequence_t *seq, int bserrno)
{
	if (bserrno != 0) {
		/* [한국어] 실패 시에만 set의 bserrno를 갱신 (성공 호출이 이전 에러를 덮지 않도록). */
		seq->bserrno = bserrno;
	}
	bs_request_set_complete((struct spdk_bs_request_set *)seq);
	/* [한국어] 완료 처리 — 풀 반환 + 사용자 콜백 디스패치. */
}

/*
 * [한국어]
 * bs_user_op_sequence_finish - user_op이 sequence 시그니처로 끝나야 할 때 어댑터.
 *
 * @cb_arg: spdk_bs_sequence_t 포인터 (caller가 sequence를 cb_arg로 등록)
 * @bserrno: 결과
 *
 * 단순히 bs_sequence_finish로 위임 — void* cb_arg 시그니처를 sequence 시그니처로 맞춰주는 어댑터.
 */
void
bs_user_op_sequence_finish(void *cb_arg, int bserrno)
{
	spdk_bs_sequence_t *seq = cb_arg;
	/* [한국어] cb_arg에서 sequence 포인터 회수. */

	bs_sequence_finish(seq, bserrno);
	/* [한국어] sequence 종료 위임. */
}

/*
 * [한국어]
 * bs_batch_completion - batch 자식 op 하나가 완료될 때 호출되는 어댑터.
 *
 * @_channel: bs_dev가 보낸 채널 (사용 안 함)
 * @cb_arg: set 포인터
 * @bserrno: 자식 op 결과
 *
 * 동작:
 *   1) outstanding_ops 감소.
 *   2) 자식 op가 실패했다면 set->bserrno 갱신 (마지막 실패 코드가 남음 — 호출자 정책).
 *   3) outstanding_ops == 0 && batch_closed면 batch 완료 — cb_fn이 있으면 sequence
 *      변환 콜백 호출, 없으면 bs_request_set_complete로 종결.
 * 실행 컨텍스트: bs_dev I/O 완료 컨텍스트, 단일 thread.
 *
 * 동시성: 같은 batch에 속한 자식 op들의 완료가 같은 thread에서 직렬로 처리되므로
 * outstanding_ops/bserrno에 lock 불필요.
 */
static void
bs_batch_completion(struct spdk_io_channel *_channel,
		    void *cb_arg, int bserrno)
{
	struct spdk_bs_request_set	*set = cb_arg;
	/* [한국어] cb_arg에서 set 회수. */

	set->u.batch.outstanding_ops--;
	/* [한국어] 미완료 자식 op 카운트 감소. lockless — 같은 thread에서만 변경. */
	if (bserrno != 0) {
		/* [한국어] 자식 op 실패 — bserrno 기록 (이전 에러를 덮을 수 있으나,
		 * batch 의미상 마지막/대표 에러로 충분). */
		set->bserrno = bserrno;
	}

	if (set->u.batch.outstanding_ops == 0 && set->u.batch.batch_closed) {
		/* [한국어] 모든 자식 완료 + 사용자가 close 호출 완료 → batch 종결. */
		if (set->u.batch.cb_fn) {
			/* [한국어] sequence에서 변환된 batch라 다음 단계 sequence 콜백이 등록되어 있음. */
			set->cb_args.cb_fn = bs_sequence_completion;
			/* [한국어] 이후 단계 호환을 위해 cb_args를 sequence 어댑터로 다시 설정. */
			set->u.batch.cb_fn((spdk_bs_sequence_t *)set, set->u.batch.cb_arg, bserrno);
			/* [한국어] sequence 콜백 호출 — 다음 단계로 진전 또는 종료 결정. */
		} else {
			/* [한국어] 단순 batch — 즉시 사용자 콜백 디스패치. */
			bs_request_set_complete(set);
		}
	}
}

/*
 * [한국어]
 * bs_batch_open - 새 batch 시작.
 *
 * @_channel: 사용자 io_channel
 * @cpl: 완료 디스크립터
 * @blob: 대상 blob (esnap 여부 판정용)
 * @return: 새 batch 또는 NULL (esnap 채널 실패 / 풀 비었음)
 *
 * 동작:
 *   1) blob이 esnap이면 외부 채널을 back_channel로 사용, 아니면 _channel 사용.
 *   2) 풀에서 set 획득.
 *   3) set 초기화: cpl/channel/back_channel 설정 + batch 상태 변수(outstanding=0, closed=0) 초기화.
 *   4) cb_args.cb_fn = bs_batch_completion으로 등록.
 * sequence_start와 거의 같지만 union을 batch로 초기화하는 점이 다르다.
 */
spdk_bs_batch_t *
bs_batch_open(struct spdk_io_channel *_channel, struct spdk_bs_cpl *cpl, struct spdk_blob *blob)
{
	struct spdk_bs_channel		*channel;
	/* [한국어] main bs_channel. */
	struct spdk_bs_request_set	*set;
	/* [한국어] 풀에서 꺼낼 set. */
	struct spdk_io_channel		*back_channel = _channel;
	/* [한국어] 기본값: back_channel = main 채널. esnap이면 아래에서 덮어씀. */

	if (spdk_blob_is_esnap_clone(blob)) {
		/* [한국어] 외부 snapshot의 clone — 외부 device 전용 채널 필요. */
		back_channel = blob_esnap_get_io_channel(_channel, blob);
		/* [한국어] esnap 채널 획득. */
		if (back_channel == NULL) {
			/* [한국어] 채널 할당 실패 — 호출자에게 NULL 반환 (sequence_start_blob과 달리 assert 없음). */
			return NULL;
		}
	}

	channel = spdk_io_channel_get_ctx(_channel);
	/* [한국어] io_channel ctx → bs_channel 변환. */
	assert(channel != NULL);
	/* [한국어] 채널 유효성 표명. */
	set = TAILQ_FIRST(&channel->reqs);
	/* [한국어] 풀에서 첫 free set 획득. */
	if (!set) {
		/* [한국어] 풀 빈 상태 — queue depth 초과. */
		return NULL;
	}
	TAILQ_REMOVE(&channel->reqs, set, link);
	/* [한국어] 풀에서 제거 → 활성. */

	spdk_trace_record(TRACE_BLOB_REQ_SET_START, 0, 0, (uintptr_t)&set->cb_args,
			  (uintptr_t)cpl->u.blob_basic.cb_arg);
	/* [한국어] START 이벤트. */

	set->cpl = *cpl;
	/* [한국어] 사용자 cpl 복사. */
	set->bserrno = 0;
	/* [한국어] 결과 초기화. */
	set->channel = channel;
	/* [한국어] main 채널. */
	set->back_channel = back_channel;
	/* [한국어] backing 채널. */

	set->u.batch.cb_fn = NULL;
	/* [한국어] sequence 변환된 batch가 아니므로 NULL. */
	set->u.batch.cb_arg = NULL;
	/* [한국어] cb_fn이 NULL이므로 cb_arg도 NULL. */
	set->u.batch.outstanding_ops = 0;
	/* [한국어] 미완료 자식 op 수 — 시작 시 0. */
	set->u.batch.batch_closed = 0;
	/* [한국어] close 호출 전 — 0. */

	set->cb_args.cb_fn = bs_batch_completion;
	/* [한국어] 자식 op 완료 어댑터 등록. */
	set->cb_args.cb_arg = set;
	/* [한국어] 어댑터 컨텍스트로 set 자신. */
	set->cb_args.channel = channel->dev_channel;
	/* [한국어] bs_dev I/O 채널. */

	return (spdk_bs_batch_t *)set;
	/* [한국어] batch alias로 반환. */
}

/*
 * [한국어]
 * bs_batch_read_bs_dev - batch에 자식 read 추가 (back_channel 사용).
 *
 * outstanding_ops++ 후 bs_dev->read 발행. 완료는 bs_batch_completion이 처리.
 */
void
bs_batch_read_bs_dev(spdk_bs_batch_t *batch, struct spdk_bs_dev *bs_dev,
		     void *payload, uint64_t lba, uint32_t lba_count)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)batch;
	/* [한국어] batch → set 캐스팅. */
	struct spdk_io_channel		*back_channel = set->back_channel;
	/* [한국어] backing 채널. */

	SPDK_DEBUGLOG(blob_rw, "Reading %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);
	/* [한국어] 디버그 로그. */

	set->u.batch.outstanding_ops++;
	/* [한국어] 자식 op 발행 전 카운트 증가 — 완료 시 bs_batch_completion이 감소. */
	bs_dev->read(bs_dev, back_channel, payload, lba, lba_count, &set->cb_args);
	/* [한국어] read 발행 — 완료는 cb_args.cb_fn = bs_batch_completion으로. */
}

/*
 * [한국어]
 * bs_batch_read_dev - batch에 main bs_dev read 자식 op 추가.
 */
void
bs_batch_read_dev(spdk_bs_batch_t *batch, void *payload,
		  uint64_t lba, uint32_t lba_count)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)batch;
	/* [한국어] batch → set 캐스팅. */
	struct spdk_bs_channel		*channel = set->channel;
	/* [한국어] main 채널. */

	SPDK_DEBUGLOG(blob_rw, "Reading %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);
	/* [한국어] 디버그 로그. */

	set->u.batch.outstanding_ops++;
	/* [한국어] outstanding 증가. */
	channel->dev->read(channel->dev, channel->dev_channel, payload, lba, lba_count, &set->cb_args);
	/* [한국어] main bs_dev read 발행. */
}

/*
 * [한국어]
 * bs_batch_write_dev - batch에 main bs_dev write 자식 op 추가.
 */
void
bs_batch_write_dev(spdk_bs_batch_t *batch, void *payload,
		   uint64_t lba, uint32_t lba_count)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)batch;
	/* [한국어] batch → set 캐스팅. */
	struct spdk_bs_channel		*channel = set->channel;
	/* [한국어] main 채널. */

	SPDK_DEBUGLOG(blob_rw, "Writing %" PRIu32 " blocks to LBA %" PRIu64 "\n", lba_count, lba);
	/* [한국어] 디버그 로그. */

	set->u.batch.outstanding_ops++;
	/* [한국어] outstanding 증가. */
	channel->dev->write(channel->dev, channel->dev_channel, payload, lba, lba_count,
			    &set->cb_args);
	/* [한국어] write 발행. */
}

/*
 * [한국어]
 * bs_batch_unmap_dev - batch에 main bs_dev UNMAP 자식 op 추가.
 *
 * NVMe Dataset Management Deallocate / SCSI UNMAP에 대응.
 */
void
bs_batch_unmap_dev(spdk_bs_batch_t *batch,
		   uint64_t lba, uint64_t lba_count)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)batch;
	/* [한국어] batch → set 캐스팅. */
	struct spdk_bs_channel		*channel = set->channel;
	/* [한국어] main 채널. */

	SPDK_DEBUGLOG(blob_rw, "Unmapping %" PRIu64 " blocks at LBA %" PRIu64 "\n", lba_count,
		      lba);
	/* [한국어] 디버그 로그. */

	set->u.batch.outstanding_ops++;
	/* [한국어] outstanding 증가. */
	channel->dev->unmap(channel->dev, channel->dev_channel, lba, lba_count,
			    &set->cb_args);
	/* [한국어] UNMAP 발행. */
}

/*
 * [한국어]
 * bs_batch_write_zeroes_dev - batch에 main bs_dev WRITE_ZEROES 자식 op 추가.
 */
void
bs_batch_write_zeroes_dev(spdk_bs_batch_t *batch,
			  uint64_t lba, uint64_t lba_count)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)batch;
	/* [한국어] batch → set 캐스팅. */
	struct spdk_bs_channel		*channel = set->channel;
	/* [한국어] main 채널. */

	SPDK_DEBUGLOG(blob_rw, "Zeroing %" PRIu64 " blocks at LBA %" PRIu64 "\n", lba_count, lba);
	/* [한국어] 디버그 로그. */

	set->u.batch.outstanding_ops++;
	/* [한국어] outstanding 증가. */
	channel->dev->write_zeroes(channel->dev, channel->dev_channel, lba, lba_count,
				   &set->cb_args);
	/* [한국어] WRITE_ZEROES 발행. */
}

/*
 * [한국어]
 * bs_batch_close - 사용자가 batch에 더 이상 op를 추가하지 않음을 알림.
 *
 * 동작:
 *   - batch_closed = 1로 마킹.
 *   - outstanding_ops가 이미 0이면 즉시 완료 처리 (cb_fn 있으면 sequence 콜백, 아니면 set complete).
 *   - 0이 아니면 마지막 자식 op 완료 시 bs_batch_completion이 종결한다.
 *
 * 호출자: 사용자가 모든 batch_*_dev 호출을 마친 후 정확히 한 번 호출해야 함.
 */
void
bs_batch_close(spdk_bs_batch_t *batch)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)batch;
	/* [한국어] batch → set 캐스팅. */

	set->u.batch.batch_closed = 1;
	/* [한국어] 닫힘 마킹 — 이후 자식 op 추가 금지(호출자 약속). */

	if (set->u.batch.outstanding_ops == 0) {
		/* [한국어] 모든 자식 op가 close 호출 시점 이전에 이미 완료된 경우 — 즉시 종결. */
		if (set->u.batch.cb_fn) {
			/* [한국어] sequence 변환 batch — 다음 sequence 콜백 호출. */
			set->cb_args.cb_fn = bs_sequence_completion;
			/* [한국어] 후속 단계 호환을 위해 cb_args를 sequence 어댑터로 재설정. */
			set->u.batch.cb_fn((spdk_bs_sequence_t *)set, set->u.batch.cb_arg, set->bserrno);
			/* [한국어] sequence 콜백 호출. */
		} else {
			/* [한국어] 단순 batch — 즉시 사용자 콜백 디스패치. */
			bs_request_set_complete(set);
		}
	}
	/* [한국어] outstanding != 0 case는 bs_batch_completion이 마지막 자식 완료 시 처리. */
}

/*
 * [한국어]
 * bs_sequence_to_batch - 진행 중인 sequence를 batch로 의미 전환.
 *
 * @seq: 변환할 sequence
 * @cb_fn/@cb_arg: 변환 후 batch 완료 시 호출될 sequence 시그니처 콜백
 * @return: 같은 set을 batch alias로 반환
 *
 * 동작: 같은 메모리 객체에서 union 의미만 batch로 재해석. cb_args도 batch 어댑터로 교체.
 * 사용처: sequence 한 단계가 여러 병렬 자식 I/O를 발행해야 할 때 — 끝나면 다시 sequence로 복귀.
 */
spdk_bs_batch_t *
bs_sequence_to_batch(spdk_bs_sequence_t *seq, spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set *set = (struct spdk_bs_request_set *)seq;
	/* [한국어] sequence → set. */

	set->u.batch.cb_fn = cb_fn;
	/* [한국어] 후속 sequence 콜백 등록 (batch 완료 → sequence 진전). */
	set->u.batch.cb_arg = cb_arg;
	/* [한국어] cb_fn 인자. */
	set->u.batch.outstanding_ops = 0;
	/* [한국어] 카운트 초기화. */
	set->u.batch.batch_closed = 0;
	/* [한국어] 닫힘 플래그 초기화. */

	set->cb_args.cb_fn = bs_batch_completion;
	/* [한국어] cb_args를 batch 완료 어댑터로 교체 — 이후 자식 op 완료가 batch_completion으로 갈 수 있게. */

	return set;
	/* [한국어] 같은 set 포인터를 batch alias로 반환. */
}

/*
 * [한국어]
 * bs_user_op_alloc - 지연 실행할 사용자 I/O를 set에 보관.
 *
 * @_channel: 사용자 io_channel
 * @cpl: 완료 디스크립터 (사용자 콜백 등록)
 * @op_type: enum spdk_blob_op_type (READ/WRITE/UNMAP/WRITE_ZEROES/READV/WRITEV)
 * @blob: 대상 blob
 * @payload: 단일 buffer 또는 iov 배열
 * @iovcnt: vectored일 때 iov 원소 수
 * @offset/@length: blob 내 io_unit 단위 좌표
 * @return: 새 user_op (풀이 비었으면 NULL)
 *
 * 사용 시나리오: thin blob에 write가 들어왔는데 클러스터 할당이 비동기적으로 진행 중이면
 * 사용자 I/O를 즉시 실행할 수 없다. 그때 bs_user_op_alloc으로 보관해 두고 클러스터 할당
 * 완료 후 bs_user_op_execute로 실제 발행한다.
 *
 * 동작: 풀에서 set 획득 → cpl/channel/op_type/iovcnt/blob/offset/length/payload 저장.
 */
spdk_bs_user_op_t *
bs_user_op_alloc(struct spdk_io_channel *_channel, struct spdk_bs_cpl *cpl,
		 enum spdk_blob_op_type op_type, struct spdk_blob *blob,
		 void *payload, int iovcnt, uint64_t offset, uint64_t length)
{
	struct spdk_bs_channel		*channel;
	/* [한국어] main bs_channel. */
	struct spdk_bs_request_set	*set;
	/* [한국어] 풀에서 꺼낼 set. */
	struct spdk_bs_user_op_args	*args;
	/* [한국어] u.user_op 영역의 별칭 (편의용). */

	channel = spdk_io_channel_get_ctx(_channel);
	/* [한국어] io_channel ctx → bs_channel. */
	assert(channel != NULL);
	/* [한국어] 채널 유효성. */
	set = TAILQ_FIRST(&channel->reqs);
	/* [한국어] 풀 첫 free set. */
	if (!set) {
		/* [한국어] 풀 비었음 — 호출자가 처리(보통 추가 큐에 매달거나 ENOMEM). */
		return NULL;
	}
	TAILQ_REMOVE(&channel->reqs, set, link);
	/* [한국어] 풀 제거 → 활성. */

	spdk_trace_record(TRACE_BLOB_REQ_SET_START, 0, 0, (uintptr_t)&set->cb_args,
			  (uintptr_t)cpl->u.blob_basic.cb_arg);
	/* [한국어] START 이벤트 기록 — execute/abort 시점까지의 latency 추적 가능. */

	set->cpl = *cpl;
	/* [한국어] cpl 복사. */
	set->channel = channel;
	/* [한국어] main 채널 저장. */
	set->back_channel = NULL;
	/* [한국어] user_op은 본 발행 시 spdk_blob_io_*가 새로 채널을 사용하므로 NULL OK. */
	set->ext_io_opts = NULL;
	/* [한국어] ext I/O 옵션 초기 NULL — 사용자가 ext 변형이면 별도로 설정됨. */

	args = &set->u.user_op;
	/* [한국어] union의 user_op 영역을 가리키는 포인터 — 코드 가독성을 위해. */

	args->type = op_type;
	/* [한국어] 연산 타입 — execute의 switch 분기에 사용. */
	args->iovcnt = iovcnt;
	/* [한국어] vectored I/O일 때 원소 수 (단일 버퍼면 무시). */
	args->blob = blob;
	/* [한국어] 대상 blob. */
	args->offset = offset;
	/* [한국어] blob 내 시작 io_unit. */
	args->length = length;
	/* [한국어] io_unit 단위 길이. */
	args->payload = payload;
	/* [한국어] 단일 buffer 또는 (struct iovec*)로 캐스팅될 포인터. */

	return (spdk_bs_user_op_t *)set;
	/* [한국어] user_op alias로 반환. */
}

/*
 * [한국어]
 * bs_user_op_execute - 보관된 user_op을 실제 spdk_blob_io_*로 발행.
 *
 * @op: bs_user_op_alloc로 만든 user_op (sequence/batch와 같은 set이지만 union 의미만 다름)
 *
 * 동작:
 *   1) op_type별로 spdk_blob_io_* 함수 호출 — 사용자 콜백은 set->cpl.u.blob_basic.cb_fn 직접 사용.
 *   2) 발행 후 set을 풀로 즉시 반환 (bs_request_set_complete 미사용 — 새 발행이 자체 콜백 라이프사이클).
 *
 * 주의: 여기서 발행되는 spdk_blob_io_*는 별도의 sequence/batch set을 새로 할당해서 사용한다.
 * 즉, 이 함수의 set은 단순히 args를 담아두는 컨테이너 역할만 하고, 실제 I/O는 새 set이 처리한다.
 * 그래서 풀로 즉시 반환해도 안전.
 *
 * 호출 시나리오: 클러스터 할당 완료 → bs_user_op_execute(저장해둔 op).
 */
void
bs_user_op_execute(spdk_bs_user_op_t *op)
{
	struct spdk_bs_request_set	*set;
	/* [한국어] op → set 캐스팅 변수. */
	struct spdk_bs_user_op_args	*args;
	/* [한국어] u.user_op 별칭. */
	struct spdk_io_channel		*ch;
	/* [한국어] spdk_blob_io_* 호출에 필요한 외부 io_channel. */

	set = (struct spdk_bs_request_set *)op;
	/* [한국어] 캐스팅. */
	args = &set->u.user_op;
	/* [한국어] user_op 영역 접근. */
	ch = spdk_io_channel_from_ctx(set->channel);
	/* [한국어] bs_channel ctx → 외부 io_channel 변환 (get_ctx의 역연산).
	 * spdk_blob_io_*는 외부 io_channel을 받기 때문에 이 변환이 필요. */

	switch (args->type) {
	/* [한국어] 6가지 op_type에 따라 spdk_blob_io_* 발행. */
	case SPDK_BLOB_READ:
		/* [한국어] 단일 buffer read. */
		spdk_blob_io_read(args->blob, ch, args->payload, args->offset, args->length,
				  set->cpl.u.blob_basic.cb_fn, set->cpl.u.blob_basic.cb_arg);
		/* [한국어] 사용자 콜백을 cpl에서 직접 가져와 그대로 위임 — 새 sequence가 그 콜백을 호출. */
		break;
	case SPDK_BLOB_WRITE:
		/* [한국어] 단일 buffer write. */
		spdk_blob_io_write(args->blob, ch, args->payload, args->offset, args->length,
				   set->cpl.u.blob_basic.cb_fn, set->cpl.u.blob_basic.cb_arg);
		break;
	case SPDK_BLOB_UNMAP:
		/* [한국어] UNMAP — payload 없음. */
		spdk_blob_io_unmap(args->blob, ch, args->offset, args->length,
				   set->cpl.u.blob_basic.cb_fn, set->cpl.u.blob_basic.cb_arg);
		break;
	case SPDK_BLOB_WRITE_ZEROES:
		/* [한국어] WRITE_ZEROES — payload 없음. */
		spdk_blob_io_write_zeroes(args->blob, ch, args->offset, args->length,
					  set->cpl.u.blob_basic.cb_fn, set->cpl.u.blob_basic.cb_arg);
		break;
	case SPDK_BLOB_READV:
		/* [한국어] vectored read — payload는 (struct iovec*)로 캐스팅, ext 변형 사용 (ext_io_opts 전달). */
		spdk_blob_io_readv_ext(args->blob, ch, args->payload, args->iovcnt,
				       args->offset, args->length,
				       set->cpl.u.blob_basic.cb_fn, set->cpl.u.blob_basic.cb_arg,
				       set->ext_io_opts);
		break;
	case SPDK_BLOB_WRITEV:
		/* [한국어] vectored write — ext 변형 사용. */
		spdk_blob_io_writev_ext(args->blob, ch, args->payload, args->iovcnt,
					args->offset, args->length,
					set->cpl.u.blob_basic.cb_fn, set->cpl.u.blob_basic.cb_arg,
					set->ext_io_opts);
		break;
	}
	TAILQ_INSERT_TAIL(&set->channel->reqs, set, link);
	/* [한국어] user_op set은 args 컨테이너 역할만 했으므로 발행 직후 풀로 반환 — 실제 I/O는
	 * spdk_blob_io_*가 새로 할당한 set이 처리한다. */
}

/*
 * [한국어]
 * bs_user_op_abort - user_op을 실행하지 않고 사용자 콜백에 에러를 전달 후 set 반환.
 *
 * @op: 취소할 user_op
 * @bserrno: 사용자에게 알릴 에러 코드 (보통 -ENOMEM, -EIO 등)
 *
 * 사용 시나리오: 클러스터 할당이 실패해서 원래 발행 의도가 무산된 경우 — 사용자에게는
 * 그 실패를 즉시 알리고 set을 풀에 반환.
 *
 * 동작: 사용자 콜백 직접 호출(cpl.u.blob_basic.cb_fn) + set을 reqs 풀로 반환.
 * bs_call_cpl을 거치지 않는 이유: user_op은 cpl_type 분기 없이 BLOB_BASIC 시그니처만
 * 가정하므로 직접 호출이 더 단순.
 */
void
bs_user_op_abort(spdk_bs_user_op_t *op, int bserrno)
{
	struct spdk_bs_request_set	*set;
	/* [한국어] op → set 캐스팅 변수. */

	set = (struct spdk_bs_request_set *)op;
	/* [한국어] 캐스팅. */

	set->cpl.u.blob_basic.cb_fn(set->cpl.u.blob_basic.cb_arg, bserrno);
	/* [한국어] 사용자 콜백 직접 호출 — bs_call_cpl 우회 (BLOB_BASIC만 가정). */
	TAILQ_INSERT_TAIL(&set->channel->reqs, set, link);
	/* [한국어] set을 풀로 반환. */
}

SPDK_LOG_REGISTER_COMPONENT(blob_rw)
/* [한국어] "blob_rw" 디버그 컴포넌트 등록 — 사용자가 SPDK 로그 레벨로 이 컴포넌트를
 * 활성화하면 SPDK_DEBUGLOG(blob_rw, ...)가 stderr에 출력된다.
 * 매크로 끝에 세미콜론이 없는 이유: 자체적으로 정적 변수 선언으로 확장됨. */
