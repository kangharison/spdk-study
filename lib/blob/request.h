/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] Blobstore 요청 객체 인터페이스 헤더 (request.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK Blobstore 내부에서 비동기 I/O 요청을 관리하기 위한 핵심 자료구조 3종 —
 * sequence(직렬 요청), batch(병렬 요청), user_op(지연 사용자 연산) — 의 통합 표현인
 * struct spdk_bs_request_set과, 완료 콜백 디스패치를 위한 spdk_bs_cpl(완료 디스크립터),
 * 그리고 모든 sequence/batch/user_op API 함수 선언을 제공한다. 세 가지 요청 타입은
 * 모두 같은 spdk_bs_request_set 구조체를 typedef alias로 재사용하며, union u 안에
 * 타입별 상태(콜백 함수, outstanding_ops 카운트, user_op 파라미터)를 둔다.
 * 요청 객체는 채널의 free pool(reqs TAILQ)에서 꺼내어 사용하고 완료 후 다시 반환되는
 * "객체 풀" 패턴으로 lockless 동작을 보장한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   blobstore.c (메타데이터 read/write, blob I/O 분할)
 *     → bs_sequence_start_bs/blob() 또는 bs_batch_open() 또는 bs_user_op_alloc()
 *     → spdk_bs_request_set 객체를 채널 풀에서 꺼냄
 *     → bs_sequence_read_dev / bs_sequence_write_dev / bs_batch_* 등으로 bs_dev I/O 발행
 *     → 비동기 완료 → bs_sequence_completion or bs_batch_completion
 *     → bs_request_set_complete → bs_call_cpl → 사용자 콜백 + 객체 풀로 반환
 * 실행 컨텍스트: 호스트 유저스페이스, blob 소유 SPDK thread (메타데이터는 md_thread).
 * 채널 단위 풀이므로 한 채널을 쓰는 thread만 그 풀을 건드린다 → lock 불필요.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/blob.h: spdk_bs_op_complete, spdk_blob_op_complete, spdk_blob_id 등
 *   사용자에게 노출되는 콜백 시그니처 정의.
 * - blobstore.h: struct spdk_bs_channel(req_mem 풀, reqs TAILQ), struct spdk_bs_dev,
 *   enum spdk_blob_op_type 등 내부 정의 — 이 헤더와 짝을 이룬다.
 * - request.c: 이 헤더의 모든 함수의 구현체.
 * - blobstore.c: 가장 큰 소비자 — 모든 메타/유저 I/O가 결국 여기로 모인다.
 * 데이터 흐름:
 *   spdk_blob_io_read (사용자) → blobstore가 bs_sequence_start_blob → bs_request_set 획득
 *     → bs_sequence_read_dev → bs_dev->read → 비동기 완료 → bs_sequence_completion
 *     → 사용자 콜백.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_bs_request_set: 모든 요청의 통합 표현. cpl/bserrno/channel/cb_args/u(union)/
 *   ext_io_opts/link로 구성. typedef로 sequence/batch/user_op 가명 사용.
 * - struct spdk_bs_cpl: 완료 시 어떤 사용자 콜백을 어떤 시그니처로 호출할지 디스크립터.
 *   union u에 7가지 타입별 (cb_fn, cb_arg [+ extra]) 묶음.
 * - enum spdk_bs_cpl_type: 완료 타입 enum (BS_BASIC, BS_HANDLE, BLOB_BASIC, BLOBID,
 *   BLOB_HANDLE, NESTED_SEQUENCE, NONE).
 * - bs_sequence_start_bs/blob(): 메타/blob I/O sequence 시작 — 풀에서 set 획득.
 * - bs_sequence_read/write/readv/writev_dev(): bs_dev I/O 발행 + 완료 콜백 등록.
 * - bs_sequence_finish(): sequence 종료 + bs_call_cpl로 사용자 알림.
 * - bs_batch_open / bs_batch_*_dev / bs_batch_close: 병렬 요청 관리.
 * - bs_user_op_alloc / bs_user_op_execute / bs_user_op_abort: 클러스터 할당 대기 등으로
 *   지연된 사용자 연산을 보관·재실행·취소.
 */

#ifndef SPDK_BS_REQUEST_H
#define SPDK_BS_REQUEST_H
/* [한국어] 헤더 가드 — 다중 include 시 중복 정의 방지. */

#include "spdk/stdinc.h"
/* [한국어] POSIX 표준 헤더 모음 — uint32_t/uint64_t/struct iovec 등. */

#include "spdk/blob.h"
/* [한국어] Blobstore 공개 API — spdk_bs_op_complete, spdk_blob_op_complete,
 * spdk_blob_op_with_id_complete, spdk_blob_op_with_handle_complete, spdk_blob_id 등
 * 사용자에게 노출되는 콜백 시그니처와 타입을 가져온다. */

/*
 * [한국어]
 * enum spdk_bs_cpl_type — 완료 디스크립터의 타입.
 * 사용자에게 알려야 할 콜백 시그니처가 무엇인지 식별하여 bs_call_cpl이 union의 어느 멤버를
 * 디스패치할지 결정한다. 한 sequence/batch/user_op은 정확히 하나의 cpl_type을 가진다.
 */
enum spdk_bs_cpl_type {
	SPDK_BS_CPL_TYPE_NONE,
	/* [한국어] 콜백 디스패치 안 함 — 외부에서 별도로 처리하는 경우 (예: 중첩 시퀀스 내부 단계).
	 * 설정자: 일부 내부 sequence가 cpl_type을 NONE으로 두고 bserrno만 추적.
	 * 읽는 자: bs_call_cpl이 이 타입을 만나면 아무 콜백도 호출하지 않고 바로 반환.
	 * 값 범위: 다른 타입과 상호 배타적. */
	SPDK_BS_CPL_TYPE_BS_BASIC,
	/* [한국어] 단순 blobstore 연산 완료 — spdk_bs_op_complete(cb_arg, bserrno) 시그니처.
	 * 설정자: spdk_bs_unload, spdk_bs_init 등 blob을 다루지 않는 BS-level 연산.
	 * 읽는 자: bs_call_cpl이 cpl->u.bs_basic.cb_fn(cb_arg, bserrno) 디스패치.
	 * 값 범위: u.bs_basic 멤버가 유효. */
	SPDK_BS_CPL_TYPE_BS_HANDLE,
	/* [한국어] BS 핸들을 함께 반환하는 완료 — spdk_bs_op_with_handle_complete(cb_arg, bs, errno).
	 * 설정자: spdk_bs_load, spdk_bs_init 등 blobstore 핸들이 결과로 필요한 연산.
	 * 읽는 자: bs_call_cpl이 errno==0일 때 cpl->u.bs_handle.bs를 사용자에게 전달, 실패 시 NULL.
	 * 값 범위: u.bs_handle 멤버 유효. */
	SPDK_BS_CPL_TYPE_BLOB_BASIC,
	/* [한국어] blob 단위 단순 완료 — spdk_blob_op_complete(cb_arg, bserrno).
	 * 설정자: spdk_blob_io_read/write, spdk_blob_close 등 대부분의 blob 연산.
	 * 읽는 자: bs_call_cpl이 cpl->u.blob_basic.cb_fn 호출.
	 * 값 범위: u.blob_basic 유효. */
	SPDK_BS_CPL_TYPE_BLOBID,
	/* [한국어] blob_id를 결과로 돌려주는 완료 — spdk_blob_op_with_id_complete.
	 * 설정자: spdk_bs_create_blob, snapshot/clone 생성 등 새 blob_id가 결과인 연산.
	 * 읽는 자: bs_call_cpl이 errno==0일 때 cpl->u.blobid.blobid를, 실패 시 SPDK_BLOBID_INVALID 전달.
	 * 값 범위: u.blobid 유효. */
	SPDK_BS_CPL_TYPE_BLOB_HANDLE,
	/* [한국어] blob 핸들을 결과로 돌려주는 완료 — spdk_blob_op_with_handle_complete.
	 * 설정자: spdk_bs_open_blob 등 사용자에게 spdk_blob* 가 필요한 연산.
	 * 읽는 자: bs_call_cpl이 errno==0일 때 cpl->u.blob_handle.blob을 전달, 실패 시 NULL.
	 * 값 범위: u.blob_handle 유효. */
	SPDK_BS_CPL_TYPE_NESTED_SEQUENCE,
	/* [한국어] 다른 sequence가 부모인 중첩 sequence 완료 — spdk_bs_nested_seq_complete.
	 * 설정자: 큰 연산을 여러 sub-sequence로 분해할 때, 각 sub의 완료를 부모 sequence가 받음.
	 * 읽는 자: bs_call_cpl이 cpl->u.nested_seq.cb_fn(cb_arg, parent_seq, errno) 호출.
	 * 값 범위: u.nested_seq 유효. parent 필드가 부모 sequence 포인터. */
};

enum spdk_blob_op_type;
/* [한국어] forward declaration — blobstore.h에서 정의되는 enum (READ/WRITE/...).
 * 이 헤더는 정의 본체를 필요로 하지 않으므로 전방 선언만으로 컴파일이 가능. */

struct spdk_bs_request_set;
/* [한국어] forward declaration — 본체는 아래에서 정의. typedef alias가 먼저 등장하므로 필요. */

/* Use a sequence to submit a set of requests serially */
typedef struct spdk_bs_request_set spdk_bs_sequence_t;
/* [한국어] sequence 가명 — 같은 set 객체이지만 "직렬 실행" 의미로 사용.
 * sequence는 한 번에 하나의 I/O만 in-flight (cb_fn으로 다음 단계로 진행). */

/* Use a batch to submit a set of requests in parallel */
typedef struct spdk_bs_request_set spdk_bs_batch_t;
/* [한국어] batch 가명 — 같은 set 객체. "병렬 실행" 의미로 사용.
 * outstanding_ops 카운트로 여러 I/O를 동시에 추적, 모두 끝나면 cb_fn 호출. */

/* Use a user_op to queue a user operation for later execution */
typedef struct spdk_bs_request_set spdk_bs_user_op_t;
/* [한국어] user_op 가명 — 같은 set 객체. 클러스터 할당 등을 기다리느라 즉시 실행할 수 없는
 * 사용자 I/O를 보관해 두었다가 나중에 bs_user_op_execute로 재시도하는 용도. */

typedef void (*spdk_bs_nested_seq_complete)(void *cb_arg, spdk_bs_sequence_t *parent, int bserrno);
/* [한국어] 중첩 sequence 완료 콜백 시그니처 — 부모 sequence 포인터를 함께 전달받아
 * 부모의 컨텍스트에서 후속 작업을 이어갈 수 있게 한다. */

/*
 * [한국어]
 * struct spdk_bs_cpl — 완료 디스크립터.
 * 어떤 종류의 사용자 콜백을 어떤 인자로 호출할지 type + union으로 표현.
 * 한 set은 하나의 cpl을 가지며, bs_request_set_complete → bs_call_cpl이 type에 따라 디스패치.
 * 동기화: set은 채널 단위 객체이므로 단일 thread에서만 다뤄짐 → lock 불필요.
 */
struct spdk_bs_cpl {
	enum spdk_bs_cpl_type type;
	/* [한국어] 어느 union 멤버가 유효한지 식별.
	 * 설정자: 모든 sequence/batch/user_op 시작 함수가 cpl 구조체를 받아 그대로 set->cpl에 복사.
	 * 읽는 자: bs_call_cpl이 switch(type)으로 디스패치.
	 * 값 범위: spdk_bs_cpl_type enum 값 중 하나.
	 * 동기화: 한 set은 단일 thread 점유 → 별도 락 없음. */
	union {
		/* [한국어] 타입별 콜백 묶음. 한 번에 하나의 멤버만 의미를 갖는다 (type으로 식별). */
		struct {
			/* [한국어] BS_BASIC: blobstore 단순 op 완료. */
			spdk_bs_op_complete     cb_fn;
			/* [한국어] 사용자 콜백 함수 — (cb_arg, bserrno) 시그니처.
			 * 설정자: spdk_bs_unload 등의 호출자.
			 * 읽는 자: bs_call_cpl. 값 범위: 함수 포인터(NULL 가능 X — 항상 유효해야 함).
			 * 동기화: 호출 시점에 set 점유 thread에서 동기 호출. */
			void                    *cb_arg;
			/* [한국어] 사용자 콜백 인자 — opaque 포인터로 사용자가 자유롭게 사용.
			 * 설정자: 사용자, 읽는 자: bs_call_cpl이 cb_fn에 전달.
			 * 값 범위: NULL 허용. 동기화: set 점유 thread에서만 접근. */
		} bs_basic;

		struct {
			/* [한국어] BS_HANDLE: blobstore 핸들과 함께 완료. */
			spdk_bs_op_with_handle_complete cb_fn;
			/* [한국어] 콜백 — (cb_arg, bs, bserrno) 시그니처.
			 * 설정자: spdk_bs_load 등 호출자. 읽는 자: bs_call_cpl. 동기화: 단일 thread. */
			void                            *cb_arg;
			/* [한국어] 사용자 인자 — opaque 포인터.
			 * 설정자: 사용자. 읽는 자: bs_call_cpl. 값 범위: NULL 허용. */
			struct spdk_blob_store          *bs;
			/* [한국어] 결과 blobstore 핸들 — 성공 시 사용자에게 전달, 실패 시 NULL로 대체됨.
			 * 설정자: blobstore.c가 BS load/init 완료 단계에서 set->cpl.u.bs_handle.bs에 저장.
			 * 읽는 자: bs_call_cpl. 값 범위: 유효한 spdk_blob_store 포인터 또는 NULL.
			 * 동기화: 설정 후 한 번만 읽힘 (선언적 복사). */
		} bs_handle;

		struct {
			/* [한국어] BLOB_BASIC: blob 단순 op 완료 (가장 흔한 타입). */
			spdk_blob_op_complete   cb_fn;
			/* [한국어] 콜백 — (cb_arg, bserrno).
			 * 설정자: spdk_blob_io_read/write 등 사용자. 읽는 자: bs_call_cpl. 동기화: 단일 thread. */
			void                    *cb_arg;
			/* [한국어] 사용자 인자.
			 * 설정자: 사용자. 읽는 자: bs_call_cpl. 값 범위: NULL 허용. */
		} blob_basic;

		struct {
			/* [한국어] BLOBID: 결과로 blob_id를 반환. */
			spdk_blob_op_with_id_complete   cb_fn;
			/* [한국어] 콜백 — (cb_arg, blob_id, bserrno).
			 * 설정자: spdk_bs_create_blob 등 새 blob 생성 호출자.
			 * 읽는 자: bs_call_cpl. 동기화: 단일 thread. */
			void                            *cb_arg;
			/* [한국어] 사용자 인자.
			 * 설정자: 사용자. 읽는 자: bs_call_cpl. 값 범위: NULL 허용. */
			spdk_blob_id                     blobid;
			/* [한국어] 결과 blob_id — 성공 시 전달, 실패 시 SPDK_BLOBID_INVALID로 대체.
			 * 설정자: blobstore.c가 새 blob 할당 후 ID를 여기 기록.
			 * 읽는 자: bs_call_cpl. 값 범위: 유효 blob_id 또는 INVALID. */
		} blobid;

		struct {
			/* [한국어] BLOB_HANDLE: blob 포인터를 결과로 반환. */
			spdk_blob_op_with_handle_complete       cb_fn;
			/* [한국어] 콜백 — (cb_arg, blob*, bserrno).
			 * 설정자: spdk_bs_open_blob 등. 읽는 자: bs_call_cpl. 동기화: 단일 thread. */
			void                                    *cb_arg;
			/* [한국어] 사용자 인자.
			 * 설정자: 사용자. 읽는 자: bs_call_cpl. 값 범위: NULL 허용. */
			struct spdk_blob                        *blob;
			/* [한국어] 결과 blob 핸들 — 성공 시 전달, 실패 시 NULL.
			 * 설정자: blobstore.c가 open 완료 단계에서 기록.
			 * 읽는 자: bs_call_cpl. 값 범위: 유효 blob 포인터 또는 NULL. */
			void					*esnap_ctx;
			/* [한국어] 외부 snapshot 컨텍스트 — esnap blob open 시 device 생성 콜백에 사용된 ctx.
			 * 설정자: blobstore.c esnap 경로. 읽는 자: 사용자 정의 esnap 핸들러.
			 * 값 범위: opaque 포인터, NULL 허용. 동기화: 단일 thread. */
		} blob_handle;

		struct {
			/* [한국어] NESTED_SEQUENCE: 부모 sequence가 있는 중첩 완료. */
			spdk_bs_nested_seq_complete	cb_fn;
			/* [한국어] 콜백 — (cb_arg, parent_seq, bserrno).
			 * 설정자: 부모 sequence가 sub-sequence를 만들 때.
			 * 읽는 자: bs_call_cpl. 동기화: 단일 thread. */
			void				*cb_arg;
			/* [한국어] 사용자 인자.
			 * 설정자: 부모. 읽는 자: bs_call_cpl. 값 범위: NULL 허용. */
			spdk_bs_sequence_t		*parent;
			/* [한국어] 부모 sequence 포인터 — sub 완료 시 부모 컨텍스트에 결과 반영.
			 * 설정자: 중첩 시작 시 부모 set의 주소.
			 * 읽는 자: cb_fn 본문이 부모 sequence를 진전. 동기화: 단일 thread. */
		} nested_seq;
	} u;
	/* [한국어] union 본체. type이 가리키는 멤버만 의미를 가지며 다른 멤버는 미정의 메모리.
	 * 설정자: cpl 작성 시 type 설정 + 해당 union 멤버 채움.
	 * 읽는 자: bs_call_cpl이 type으로 분기. 동기화: 단일 thread 점유 set 내부. */
};

typedef void (*spdk_bs_sequence_cpl)(spdk_bs_sequence_t *sequence,
				     void *cb_arg, int bserrno);
/* [한국어] sequence 단계 완료 콜백 시그니처 — sequence 포인터를 함께 전달받아 다음 단계로 진전. */

/* A generic request set. Can be a sequence, batch or a user_op. */
/*
 * [한국어]
 * struct spdk_bs_request_set — sequence/batch/user_op의 통합 표현.
 * 채널 단위 풀(spdk_bs_channel::reqs TAILQ)에서 꺼내 사용하고 완료 후 반환되는 객체.
 * 한 set은 단일 thread(채널 소유 thread)에서만 다뤄지므로 동기화 락이 불필요하다 (lockless).
 */
struct spdk_bs_request_set {
	struct spdk_bs_cpl      cpl;
	/* [한국어] 사용자 완료 디스크립터 — 어떤 콜백을 어떤 인자로 호출할지.
	 * 설정자: bs_sequence_start_*/bs_batch_open/bs_user_op_alloc이 호출자로부터 받아 복사.
	 * 읽는 자: bs_request_set_complete → bs_call_cpl이 type으로 분기 호출.
	 * 값 범위: 위 spdk_bs_cpl 정의 참고. 동기화: set 점유 thread 내부에서만 접근. */

	int                     bserrno;
	/* [한국어] 누적 에러 코드 — sequence 단계나 batch 자식 op 중 한 곳이라도 실패하면 기록.
	 * 설정자: bs_sequence_completion / bs_batch_completion / bs_sequence_finish.
	 * 읽는 자: bs_request_set_complete가 사용자 콜백에 전달.
	 * 값 범위: 0(성공) 또는 음수 errno. 동기화: 단일 thread 점유. */

	/*
	 * The blobstore's channel, obtained by blobstore consumers via
	 * spdk_bs_alloc_io_channel(). Used for IO to the blobstore.
	 */
	struct spdk_bs_channel		*channel;
	/* [한국어] blobstore 채널 — 사용자가 spdk_bs_alloc_io_channel로 얻은 채널의 ctx 객체.
	 * 풀(reqs TAILQ)이 여기 매달려 있고, dev/dev_channel을 통해 실제 bs_dev에 I/O 발행.
	 * 설정자: bs_sequence_start_* 등에서 spdk_io_channel_get_ctx로 획득해 저장.
	 * 읽는 자: 모든 bs_sequence_*_dev / bs_batch_*_dev 함수가 channel->dev로 I/O.
	 * 값 범위: 유효한 spdk_bs_channel 포인터 (set 사용 중 NULL 불가).
	 * 동기화: 채널은 thread-affined → 단일 thread 점유. */
	/*
	 * The channel used by the blobstore to perform IO on back_bs_dev. Unless the blob
	 * is an esnap clone, back_channel == spdk_io_channel_get_ctx(set->channel).
	 */
	struct spdk_io_channel		*back_channel;
	/* [한국어] back_bs_dev I/O에 사용할 채널 — esnap clone이 아니면 보통 channel과 동일.
	 * esnap의 경우 외부 디바이스 전용 채널이어야 하므로 별도 보관.
	 * 설정자: bs_sequence_start_blob/bs_batch_open이 esnap clone 여부에 따라 결정.
	 * 읽는 자: bs_sequence_read_bs_dev / bs_sequence_readv_bs_dev / bs_batch_read_bs_dev.
	 * 값 범위: 유효한 spdk_io_channel 포인터, user_op은 NULL일 수 있음.
	 * 동기화: 단일 thread 점유. */

	struct spdk_bs_dev_cb_args	cb_args;
	/* [한국어] bs_dev 콜백 인자 묶음 — bs_dev->read/write에 전달되는 cb_fn/cb_arg/channel.
	 * 설정자: 시작 함수가 bs_sequence_completion/bs_batch_completion 등을 cb_fn으로 등록.
	 * 읽는 자: bs_dev 구현체가 I/O 완료 시 cb_args->cb_fn(channel, cb_arg, errno) 호출.
	 * 값 범위: 항상 유효 (set과 lifetime 동일).
	 * 동기화: I/O 발행→완료 사이 동일 thread에서만 다뤄짐. */

	union {
		/* [한국어] sequence/batch/user_op 타입별 상태. 한 set은 셋 중 하나 의미만 가짐. */
		struct {
			/* [한국어] sequence 상태 — 다음 단계로 진행시킬 콜백. */
			spdk_bs_sequence_cpl    cb_fn;
			/* [한국어] 단계 완료 시 호출될 사용자 콜백 (sequence 가시 콜백).
			 * 설정자: bs_sequence_read/write_dev 등이 호출자에게 받은 cb_fn 저장.
			 * 읽는 자: bs_sequence_completion이 cb_fn(seq, cb_arg, bserrno) 호출.
			 * 값 범위: NULL 불가. 동기화: 단일 thread. */
			void                    *cb_arg;
			/* [한국어] 사용자 인자.
			 * 설정자: bs_sequence_*_dev 호출자. 읽는 자: cb_fn에 전달.
			 * 값 범위: NULL 허용. 동기화: 단일 thread. */
		} sequence;

		struct {
			/* [한국어] batch 상태 — 병렬 op 카운트와 종료 플래그. */
			uint32_t		outstanding_ops;
			/* [한국어] 아직 완료되지 않은 자식 op 수. 0이 되고 batch_closed=1이면 batch 완료.
			 * 설정자: bs_batch_*_dev 발행 시 ++, bs_batch_completion 완료 시 --.
			 * 읽는 자: bs_batch_completion / bs_batch_close가 0인지 확인.
			 * 값 범위: 0 이상 정수. 동기화: 단일 thread → atomic 불필요. */
			uint32_t		batch_closed;
			/* [한국어] 사용자가 bs_batch_close를 호출했는지 플래그 — 호출 후 더 이상 op 추가 불가.
			 * 설정자: bs_batch_close. 읽는 자: bs_batch_completion이 closed && outstanding==0 검사.
			 * 값 범위: 0 또는 1. 동기화: 단일 thread. */
			spdk_bs_sequence_cpl	cb_fn;
			/* [한국어] batch가 sequence에서 변환된 경우 사용되는 후속 sequence 콜백 (보통 NULL).
			 * 설정자: bs_sequence_to_batch가 등록. 읽는 자: bs_batch_completion이 사용.
			 * 값 범위: NULL 또는 함수 포인터. 동기화: 단일 thread. */
			void			*cb_arg;
			/* [한국어] cb_fn 인자 (sequence 변환된 batch에서만 의미).
			 * 설정자: bs_sequence_to_batch. 값 범위: NULL 허용. 동기화: 단일 thread. */
		} batch;

		struct spdk_bs_user_op_args {
			/* [한국어] user_op 인자 — 지연 실행할 사용자 I/O의 모든 정보. */
			int			type;
			/* [한국어] 연산 타입 — enum spdk_blob_op_type 값 (READ/WRITE/UNMAP/...).
			 * 설정자: bs_user_op_alloc. 읽는 자: bs_user_op_execute의 switch 분기.
			 * 값 범위: enum 값. 동기화: 단일 thread. */
			int			iovcnt;
			/* [한국어] vectored I/O인 경우 iov 원소 수, 단일 buffer면 의미 없음.
			 * 설정자: bs_user_op_alloc. 읽는 자: bs_user_op_execute가 readv/writev 분기 시 사용.
			 * 값 범위: 0 이상. 동기화: 단일 thread. */
			struct spdk_blob	*blob;
			/* [한국어] 대상 blob 포인터.
			 * 설정자: 사용자 호출 → bs_user_op_alloc.
			 * 읽는 자: bs_user_op_execute가 spdk_blob_io_*에 전달.
			 * 값 범위: 유효 포인터. 동기화: blob은 open된 상태로 유지되어야 함. */
			uint64_t		offset;
			/* [한국어] blob 내 io_unit 시작 오프셋.
			 * 설정자: 사용자. 읽는 자: bs_user_op_execute. 값 범위: blob 크기 미만.
			 * 동기화: 단일 thread. */
			uint64_t		length;
			/* [한국어] io_unit 단위 길이.
			 * 설정자: 사용자. 읽는 자: bs_user_op_execute. 값 범위: offset+length가 blob 안.
			 * 동기화: 단일 thread. */
			spdk_blob_op_complete	cb_fn;
			/* [한국어] (참고용) 사용자 콜백 — 실제로는 set->cpl.u.blob_basic.cb_fn에 저장되어
			 * bs_user_op_execute가 그쪽을 사용한다. 이 필드는 구조 유지용/legacy.
			 * 설정자: bs_user_op_alloc (현재 코드는 사용하지 않음). 동기화: 단일 thread. */
			void			*cb_arg;
			/* [한국어] (참고용) 사용자 콜백 인자 — 위와 같은 이유. */
			void			*payload; /* cast to iov for readv/writev */
			/* [한국어] 단일 buffer 또는 iovec 배열 시작 포인터 — readv/writev면 (struct iovec*)로 캐스팅.
			 * 설정자: bs_user_op_alloc.
			 * 읽는 자: bs_user_op_execute가 spdk_blob_io_read/write 또는 io_readv/writev에 전달.
			 * 값 범위: 유효 메모리 포인터 (UNMAP/WRITE_ZEROES면 NULL 가능).
			 * 동기화: 단일 thread. */
		} user_op;
	} u;
	/* [한국어] union 본체 — sequence/batch/user_op 중 하나의 타입만 의미를 가진다.
	 * 어느 멤버가 유효한지는 어떤 시작 함수(bs_sequence_start_*, bs_batch_open, bs_user_op_alloc)로
	 * 만들었는지에 따라 결정되며, 동시에 여러 멤버를 사용하지 않는다. */
	/* Pointer to ext_io_opts passed by the user */
	struct spdk_blob_ext_io_opts *ext_io_opts;
	/* [한국어] 사용자가 전달한 외부 I/O 옵션 (memory_domain 등).
	 * 설정자: spdk_blob_io_*_ext 변형이 set에 저장.
	 * 읽는 자: bs_sequence_readv_dev / bs_sequence_writev_dev 등이 NULL 여부로 ext 변형 분기.
	 * 값 범위: NULL(일반) 또는 사용자 메모리(사용자가 lifetime 보장).
	 * 동기화: 단일 thread, set 사용 중에만 의미. */
	TAILQ_ENTRY(spdk_bs_request_set) link;
	/* [한국어] 채널 reqs free list (또는 need_cluster_alloc/queued_io 등 다른 큐)에 매달리는 링크.
	 * 설정자/읽는 자: TAILQ_INSERT_TAIL/REMOVE를 호출하는 모든 큐 조작 코드.
	 * 값 범위: 큐에 들어있을 때만 유효. 동기화: 채널 단일 thread → 락 불필요. */
};

/*
 * [한국어]
 * bs_call_cpl - cpl_type에 따라 사용자 콜백을 디스패치하는 핵심 함수.
 * 모든 sequence/batch/user_op의 최종 완료 단계에서 정확히 한 번 호출된다.
 */
void bs_call_cpl(struct spdk_bs_cpl *cpl, int bserrno);

/*
 * [한국어]
 * bs_sequence_start_bs - blobstore 메타데이터(슈퍼블록/메타페이지)에 대한 sequence 시작.
 * back_channel은 그대로 channel을 사용 (메타 I/O는 main bs_dev에서만 일어남).
 */
spdk_bs_sequence_t *bs_sequence_start_bs(struct spdk_io_channel *channel,
		struct spdk_bs_cpl *cpl);

/*
 * [한국어]
 * bs_sequence_start_blob - 특정 blob에 대한 I/O sequence 시작.
 * 해당 blob이 esnap clone이면 외부 device용 별도 채널을 back_channel로 설정.
 */
spdk_bs_sequence_t *bs_sequence_start_blob(struct spdk_io_channel *channel,
		struct spdk_bs_cpl *cpl, struct spdk_blob *blob);

/*
 * [한국어]
 * bs_sequence_start_esnap - 외부 snapshot 전용 sequence 시작 (선언만 — 구현은 별도 위치 가능성).
 */
spdk_bs_sequence_t *bs_sequence_start_esnap(struct spdk_io_channel *channel,
		struct spdk_bs_cpl *cpl, struct spdk_blob *blob);

/*
 * [한국어]
 * bs_sequence_read_bs_dev - 임의의 bs_dev에 대해 read 발행 (back_channel 사용).
 * 주로 back_bs_dev로부터 데이터를 가져올 때 사용.
 */
void bs_sequence_read_bs_dev(spdk_bs_sequence_t *seq, struct spdk_bs_dev *bs_dev,
			     void *payload, uint64_t lba, uint32_t lba_count,
			     spdk_bs_sequence_cpl cb_fn, void *cb_arg);

/*
 * [한국어]
 * bs_sequence_read_dev - 채널의 main bs_dev에 read 발행 (메타데이터 read 등).
 */
void bs_sequence_read_dev(spdk_bs_sequence_t *seq, void *payload,
			  uint64_t lba, uint32_t lba_count,
			  spdk_bs_sequence_cpl cb_fn, void *cb_arg);

/*
 * [한국어]
 * bs_sequence_write_dev - 채널의 main bs_dev에 write 발행.
 */
void bs_sequence_write_dev(spdk_bs_sequence_t *seq, void *payload,
			   uint64_t lba, uint32_t lba_count,
			   spdk_bs_sequence_cpl cb_fn, void *cb_arg);

/*
 * [한국어]
 * bs_sequence_readv_bs_dev - 임의의 bs_dev에 vectored read 발행.
 * ext_io_opts가 설정되어 있으면 readv_ext로, 아니면 일반 readv로 분기.
 */
void bs_sequence_readv_bs_dev(spdk_bs_batch_t *batch, struct spdk_bs_dev *bs_dev,
			      struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
			      spdk_bs_sequence_cpl cb_fn, void *cb_arg);

/*
 * [한국어]
 * bs_sequence_readv_dev - main bs_dev에 vectored read 발행.
 */
void bs_sequence_readv_dev(spdk_bs_batch_t *batch, struct iovec *iov, int iovcnt,
			   uint64_t lba, uint32_t lba_count,
			   spdk_bs_sequence_cpl cb_fn, void *cb_arg);

/*
 * [한국어]
 * bs_sequence_writev_dev - main bs_dev에 vectored write 발행.
 */
void bs_sequence_writev_dev(spdk_bs_batch_t *batch, struct iovec *iov, int iovcnt,
			    uint64_t lba, uint32_t lba_count,
			    spdk_bs_sequence_cpl cb_fn, void *cb_arg);

/*
 * [한국어]
 * bs_sequence_write_zeroes_dev - main bs_dev에 WRITE_ZEROES 발행.
 */
void bs_sequence_write_zeroes_dev(spdk_bs_sequence_t *seq,
				  uint64_t lba, uint64_t lba_count,
				  spdk_bs_sequence_cpl cb_fn, void *cb_arg);

/*
 * [한국어]
 * bs_sequence_copy_dev - main bs_dev에 디바이스 내부 copy 발행 (offload copy 지원 시).
 */
void bs_sequence_copy_dev(spdk_bs_sequence_t *seq,
			  uint64_t dst_lba, uint64_t src_lba, uint64_t lba_count,
			  spdk_bs_sequence_cpl cb_fn, void *cb_arg);

/*
 * [한국어]
 * bs_sequence_finish - sequence 종료. bserrno!=0이면 set->bserrno에 기록 후
 * bs_request_set_complete로 사용자 콜백 호출 + set 풀에 반환.
 */
void bs_sequence_finish(spdk_bs_sequence_t *seq, int bserrno);

/*
 * [한국어]
 * bs_user_op_sequence_finish - user_op이 sequence 콜백 시그니처로 끝났을 때 어댑터.
 * bs_sequence_finish로 위임.
 */
void bs_user_op_sequence_finish(void *cb_arg, int bserrno);

/*
 * [한국어]
 * bs_batch_open - batch 시작. 풀에서 set을 꺼내 outstanding_ops=0/closed=0로 초기화.
 * blob의 esnap 여부에 따라 back_channel 설정.
 */
spdk_bs_batch_t *bs_batch_open(struct spdk_io_channel *channel,
			       struct spdk_bs_cpl *cpl, struct spdk_blob *blob);

/*
 * [한국어]
 * bs_batch_read_bs_dev - 임의의 bs_dev에 read 발행 (back_channel 사용).
 * outstanding_ops++ — 완료되면 bs_batch_completion이 -- 후 종료 검사.
 */
void bs_batch_read_bs_dev(spdk_bs_batch_t *batch, struct spdk_bs_dev *bs_dev,
			  void *payload, uint64_t lba, uint32_t lba_count);

/*
 * [한국어]
 * bs_batch_read_dev - main bs_dev에 read 발행. outstanding_ops++.
 */
void bs_batch_read_dev(spdk_bs_batch_t *batch, void *payload,
		       uint64_t lba, uint32_t lba_count);

/*
 * [한국어]
 * bs_batch_write_dev - main bs_dev에 write 발행. outstanding_ops++.
 */
void bs_batch_write_dev(spdk_bs_batch_t *batch, void *payload,
			uint64_t lba, uint32_t lba_count);

/*
 * [한국어]
 * bs_batch_unmap_dev - main bs_dev에 UNMAP 발행. outstanding_ops++.
 */
void bs_batch_unmap_dev(spdk_bs_batch_t *batch,
			uint64_t lba, uint64_t lba_count);

/*
 * [한국어]
 * bs_batch_write_zeroes_dev - main bs_dev에 WRITE_ZEROES 발행. outstanding_ops++.
 */
void bs_batch_write_zeroes_dev(spdk_bs_batch_t *batch,
			       uint64_t lba, uint64_t lba_count);

/*
 * [한국어]
 * bs_batch_close - batch 종료 마킹. outstanding_ops==0이면 즉시 사용자 콜백 호출,
 * 아니면 마지막 op 완료 시 호출됨.
 */
void bs_batch_close(spdk_bs_batch_t *batch);

/*
 * [한국어]
 * bs_sequence_to_batch - 진행 중 sequence를 batch로 변환.
 * 같은 set 객체에서 union의 의미만 batch로 재해석.
 */
spdk_bs_batch_t *bs_sequence_to_batch(spdk_bs_sequence_t *seq,
				      spdk_bs_sequence_cpl cb_fn,
				      void *cb_arg);

/*
 * [한국어]
 * bs_user_op_alloc - 지연 실행할 사용자 I/O를 set에 저장.
 * 클러스터 할당 대기, queue depth 초과 등으로 즉시 실행 불가능할 때 사용.
 */
spdk_bs_user_op_t *bs_user_op_alloc(struct spdk_io_channel *channel, struct spdk_bs_cpl *cpl,
				    enum spdk_blob_op_type op_type, struct spdk_blob *blob,
				    void *payload, int iovcnt, uint64_t offset, uint64_t length);

/*
 * [한국어]
 * bs_user_op_execute - 보관해 둔 user_op을 실제 spdk_blob_io_*로 발행하고 set을 풀에 반환.
 */
void bs_user_op_execute(spdk_bs_user_op_t *op);

/*
 * [한국어]
 * bs_user_op_abort - user_op을 실행하지 않고 사용자 콜백에 bserrno 전달 후 set 반환.
 */
void bs_user_op_abort(spdk_bs_user_op_t *op, int bserrno);

#endif
/* [한국어] 헤더 가드 종료. */
