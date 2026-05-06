/*   SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * [한국어 설명] spdk_thread 서브시스템 내부 타입 (thread_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK thread 서브시스템의 **내부** 핵심 자료구조인 `struct spdk_io_channel`
 * 의 실제 정의(메모리 레이아웃)를 담는 헤더이다. 공개 헤더 spdk/thread.h는
 * 외부 모듈을 위해 spdk_io_channel을 opaque(불투명) 타입으로만 노출하지만,
 * SPDK 내부 구현(특히 lib/thread/thread.c, lib/bdev/bdev.c)은 채널 객체의
 * 실제 필드(소유 스레드, 참조 카운터, RB 트리 노드 등)에 직접 접근해야
 * 하므로 이 내부 정의가 별도 헤더로 분리되어 있다. 이 파일은 SPDK 빌드
 * 트리 내부에서만 include되며 ABI 호환 보증 대상이 아니다 — SPDK 마이너
 * 버전 변경에서 필드가 추가/이동될 수 있다.
 *
 * `spdk_io_channel`은 SPDK I/O 경로의 핵심 추상화이다. "특정 SPDK thread
 * (=특정 reactor=특정 CPU 코어)에서 특정 I/O 디바이스(NVMe 컨트롤러,
 * blobstore, bdev 등)에 접근하기 위한 핸들"로 다음 불변식을 가진다:
 *   1) 동일한 (thread, io_device) 조합에 대해 정확히 하나의 spdk_io_channel
 *      만 존재한다 (RB 트리로 보장).
 *   2) 스레드 고정(thread affinity) — 채널은 생성된 spdk_thread에서만
 *      사용·해제할 수 있다. 다른 스레드가 동일 디바이스에 접근하려면
 *      각자 자신의 채널을 spdk_get_io_channel()으로 새로 발급받아야 한다.
 *   3) 참조 카운팅으로 수명을 관리한다 — 같은 스레드 내 다중 모듈이 동일
 *      채널을 공유할 때 안전하게 정리되도록 ref/destroy_ref 두 카운터를
 *      운용한다.
 *   4) 구조체 뒤쪽에 모듈별 컨텍스트(trailing context)가 가변 길이로
 *      "붙어 있다" — 모듈이 spdk_io_device_register()에서 ctx_size를 명시
 *      하면 spdk_io_channel 뒤에 그만큼 추가 메모리가 단일 할당된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 실행 모델은 [DPDK EAL] → [reactor 1개당 CPU 코어 1개] → [reactor에
 * 배치된 spdk_thread] → [spdk_thread가 보유한 다수의 spdk_io_channel] →
 * [trailing 컨텍스트(NVMe qpair, bdev_channel 등)] → [실제 하드웨어/모듈]
 * 의 계층을 따른다. 이 파일은 그 중 4번째 계층의 내부 표현을 정의한다.
 *
 * 공개 spdk_io_channel API(spdk_io_channel_get_thread, _get_io_device,
 * _get_ctx, _from_ctx 등)는 모두 이 구조체의 필드 또는 trailing 영역을
 * 가리키는 포인터 산술로 구현된다. 호출 체인 예시:
 *   spdk_get_io_channel(&controller)   ← 모듈/사용자 코드
 *     → io_channel_create (lib/thread/thread.c, 내부)
 *         → io_channel 메모리 할당 (sizeof(spdk_io_channel) + ctx_size)
 *         → thread/dev 필드 채우기, RB 트리에 삽입
 *         → 모듈의 create_cb(io_device, ctx) 호출 (trailing 영역 초기화)
 *     → 사용자에게 spdk_io_channel* 반환
 *   spdk_io_channel_get_ctx(ch)
 *     → (uint8_t*)ch + sizeof(struct spdk_io_channel) 반환 (trailing 시작)
 *
 * 실행 컨텍스트: 본 헤더가 정의하는 spdk_io_channel은 단일 SPDK thread
 * (단일 CPU 코어, polled-mode reactor) 내에서만 조작된다. 따라서 ref,
 * destroy_ref, RB tree 삽입/삭제 등 모든 변경은 락 없이 수행된다 —
 * SPDK lockless 설계의 근본 토대.
 *
 * === 타 모듈과의 연결 ===
 * 의존(이 파일이 #include 하는 것):
 *   - spdk/assert.h  : SPDK_STATIC_ASSERT 매크로(컴파일 타임 크기 검증)
 *   - spdk/thread.h  : 공개 타입(spdk_io_channel_destroy_cb 콜백 시그니처)
 *                       및 외부 모듈에 노출되는 SPDK_IO_CHANNEL_STRUCT_SIZE
 *                       상수. 이 상수와 sizeof(struct spdk_io_channel)이
 *                       정확히 일치해야 외부 모듈의 trailing 컨텍스트
 *                       오프셋 가정이 깨지지 않는다.
 *   - spdk/tree.h    : BSD sys/tree.h 포팅판. RB_ENTRY 매크로로 구조체
 *                       내부에 RB 트리 링크 필드를 임베딩.
 *
 * 의존하는 모듈(이 파일을 #include 하는 측):
 *   - lib/thread/thread.c : 채널 생성·조회·해제 본체 구현. 이 파일의 모든
 *                            필드를 read/write 한다.
 *   - lib/bdev/bdev.c     : bdev 채널 추적, bdev_io 라우팅을 위해
 *                            spdk_io_channel→thread 매핑 조회.
 *   - lib/nvme/*          : NVMe qpair 접근 시 trailing 컨텍스트 사용.
 *
 * 데이터 흐름:
 *   사용자 spdk_bdev_read(channel, ...)
 *     → bdev 코어가 channel→ctx (trailing) 에서 spdk_bdev_channel 추출
 *     → spdk_bdev_channel 내부의 NVMe channel 또는 다른 백엔드 channel로
 *       다시 디스패치 (ctx의 ctx — 다단 trailing)
 *     → 최종적으로 NVMe qpair에 SQE 작성 + doorbell write
 *
 * 공유 자료구조: 본 구조체 자체. 다만 "공유"라 해도 동일 스레드 내 다른
 * 코드 경로에서만 공유되므로 lock-free 공유다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_io_channel:
 *       per-(thread, io_device) I/O 핸들. 본 헤더는 이 구조체 단 하나만
 *       정의한다. 함수 정의는 없다(타입 노출 전용 헤더).
 *
 *       * thread       : 소유 SPDK thread (스레드 고정용 식별자)
 *       * dev          : 가리키는 io_device (NVMe ctrl, blobstore 등)
 *       * ref          : per-thread get/put 참조 카운트
 *       * destroy_ref  : 파괴 진행 중 임시 카운트 (재진입 안전)
 *       * node         : io_device 내 RB 트리 인덱스 노드
 *       * destroy_cb   : 모듈 측 cleanup 콜백 (trailing 해제용)
 *       * _padding     : ABI 안정성용 예약 영역
 *       * (trailing)   : 구조체 뒤에 ctx_size 만큼 모듈 컨텍스트가 따라옴
 *
 *   - SPDK_STATIC_ASSERT (구조체 끝):
 *       sizeof(struct spdk_io_channel)이 공개 헤더의 상수와 같다는
 *       컴파일 타임 검증.
 */

#ifndef SPDK_THREAD_INTERNAL_H_
/* [한국어] include 가드 시작 — 동일 컴파일 단위에서 다중 포함 시 타입
 *  중복 정의 에러 방지. 일반적인 C 헤더 관용구. */
#define SPDK_THREAD_INTERNAL_H_

#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT(cond, msg) 매크로를 가져온다.
 *  - 사용 목적: 본 헤더 끝에서 sizeof(struct spdk_io_channel) ==
 *    SPDK_IO_CHANNEL_STRUCT_SIZE 를 컴파일 타임에 검증. 이 검증이
 *    실패하면 빌드 자체가 중단되어, 외부 모듈의 trailing-ctx 오프셋
 *    가정이 깨진 채 런타임 메모리 훼손이 발생하는 사고를 막는다.
 *  - 동기화: 컴파일 타임 검증이므로 런타임 비용 0. */

#include "spdk/thread.h"
/* [한국어] SPDK thread/poller/io_channel 의 **공개** API 선언.
 *  - 이 헤더에서 직접 필요한 것:
 *      (a) spdk_io_channel_destroy_cb 함수 포인터 타입
 *           — 본 구조체의 destroy_cb 필드 타입.
 *      (b) SPDK_IO_CHANNEL_STRUCT_SIZE 매크로
 *           — 외부 모듈이 채널 trailing 컨텍스트의 시작 오프셋을
 *             계산할 때 참조하는 ABI 약속값.
 *  - 공개 헤더에서 spdk_io_channel은 incomplete type이지만, 본 내부
 *    헤더에서 정의가 완성된다. */

#include "spdk/tree.h"
/* [한국어] SPDK 포팅판 BSD sys/tree.h — RB(Red-Black) 트리 매크로 모음.
 *  - 사용 매크로: RB_ENTRY(spdk_io_channel) — 구조체 내부에 RB 트리
 *    링크 필드(부모/자식/색상)를 직접 임베딩하기 위해 사용.
 *  - 왜 필요한가: 하나의 io_device는 다수의 spdk_thread로부터 채널
 *    요청을 받을 수 있다. (thread 키)→spdk_io_channel 매핑을 빠르게
 *    조회하기 위해 io_device가 RB 트리를 보유하며, 각 채널은 그 트리의
 *    노드가 된다. O(log n) 조회. */

/**
 * \brief Represents a per-thread channel for accessing an I/O device.
 *
 * An I/O device may be a physical entity (i.e. NVMe controller) or a software
 *  entity (i.e. a blobstore).
 *
 * This structure is not part of the API - all accesses should be done through
 *  spdk_io_channel function calls.
 */
/* [한국어]
 * struct spdk_io_channel — per-(thread, io_device) I/O 접근 핸들.
 *
 * 핵심 의미: "이 spdk_thread에서 이 io_device를 사용하기 위한 토큰".
 * 공개 헤더에서는 incomplete type으로만 노출되며, 외부 모듈은 본 구조체의
 * 필드를 직접 접근하지 않고 spdk_io_channel_get_thread / _get_io_device /
 * _get_ctx / _from_ctx 같은 접근자 API만 사용한다. 그 접근자들이 내부에서
 * 본 정의의 필드 오프셋을 사용한다.
 *
 * 메모리 레이아웃: 사용자가 spdk_get_io_channel(io_device)를 호출하면 SPDK
 * 코어는 sizeof(struct spdk_io_channel) + io_device->ctx_size 만큼을 단일
 * 할당하고, 앞쪽 sizeof(struct spdk_io_channel) 영역에 본 구조체를 배치,
 * 뒤쪽 ctx_size 영역(=trailing context)을 모듈에 넘긴다.
 *
 * 동시성: 모든 필드는 thread 필드가 가리키는 SPDK thread 위에서만
 * 변경된다 — 다른 reactor가 만진다면 그것은 곧 버그.
 */
struct spdk_io_channel {
	struct spdk_thread		*thread;
	/* [한국어] 이 채널을 소유한 SPDK thread (=특정 reactor / CPU 코어).
	 *  - 의미: 채널은 "이 스레드에서만" 사용 가능하다는 thread-affinity
	 *    불변식을 명시적으로 인코딩한다. 다른 스레드가 이 채널 포인터를
	 *    역참조해 I/O를 발행하는 것은 정의되지 않은 동작이며, SPDK는
	 *    디버그 빌드에서 이를 검증하는 데 이 필드를 활용한다.
	 *  - 설정자: lib/thread/thread.c:spdk_get_io_channel() 내부 —
	 *    spdk_get_thread()로 현재 reactor를 얻어 즉시 기록한다. 채널
	 *    수명 동안 다시 쓰이지 않는다 (사실상 const).
	 *  - 읽는 자: 공개 API spdk_io_channel_get_thread(), 채널 파괴
	 *    경로의 thread 일치 검증, bdev/nvme 모듈의 디버그 assert 등.
	 *  - 값 범위: 유효한 spdk_thread* (NULL 불가). 채널 수명 동안 절대
	 *    NULL 이 되지 않는다.
	 *  - 동기화: 생성 시 1회 기록 후 불변(immutable)이므로 추가 동기화
	 *    불필요. read만 발생. */

	struct io_device		*dev;
	/* [한국어] 이 채널이 가리키는 I/O 디바이스의 내부 표현(io_device).
	 *  - 의미: spdk_io_device_register(ctx, ..., ctx_size, name)으로
	 *    등록된 객체의 내부 핸들. NVMe 컨트롤러, blobstore, bdev,
	 *    가상 디바이스 등이 모두 io_device로 추상화되어 동일한 채널
	 *    매커니즘을 사용한다.
	 *  - 설정자: spdk_get_io_channel() 내부에서 사용자가 넘긴
	 *    io_device 검색 키(보통 컨트롤러 객체 포인터)를 lookup해서
	 *    얻은 내부 io_device* 를 기록.
	 *  - 읽는 자: 공개 API spdk_io_channel_get_io_device()(외부에는
	 *    원래 등록 키를 반환), 내부 RB 트리 비교 함수, destroy 경로.
	 *  - 값 범위: 유효한 io_device* (NULL 불가).
	 *  - 동기화: 생성 시 불변. 디바이스 자체의 수명은 SPDK가 보장한다
	 *    — 모든 채널이 close되기 전까지 io_device unregister는
	 *    실제로는 지연 처리된다. */

	uint32_t			ref;
	/* [한국어] 같은 (thread, io_device) 조합에 대한 정상 사용자 참조
	 *  카운트. 동일 스레드 내에서 동일 디바이스 채널을 두 번째로
	 *  spdk_get_io_channel() 한 경우, 새 채널을 만드는 대신 기존
	 *  채널의 ref만 증가시킨다 (RB 트리 unique key 보장).
	 *  - 설정자(증가): spdk_get_io_channel() 호출 경로 — 트리에서
	 *    기존 채널을 발견하면 ref++. 신규 생성 시 1로 초기화.
	 *    설정자(감소): spdk_put_io_channel() — ref-- 후 0이 되면
	 *    파괴 절차 시작.
	 *  - 읽는 자: spdk_put_io_channel()의 0 비교, 디버깅용 dump
	 *    함수.
	 *  - 값 범위: 0이 되면 그 즉시 destroy 경로에 진입하므로 정상
	 *    상태에서 보이는 값은 [1, UINT32_MAX].
	 *  - 동기화: 소유 스레드에서만 ++/--, 락 불필요. 다른 reactor가
	 *    cross-thread로 put 하려면 spdk_thread_send_msg 로 우회해야
	 *    하므로 실제 메모리 접근은 항상 단일 스레드. */

	uint32_t			destroy_ref;
	/* [한국어] 파괴(destroy) 진행 중에만 의미를 갖는 임시 참조 카운터.
	 *  - 의미: ref가 0이 되어 destroy_cb를 호출해야 하지만, destroy_cb
	 *    내부 또는 진행 중 어떤 비동기 단계가 다시 spdk_get_io_channel()
	 *    을 호출해 동일 채널을 부활(resurrect)시킬 가능성이 있다.
	 *    SPDK는 이런 경우 새 채널을 만들지 않고 destroy_ref로 임시
	 *    유지하다가, 모든 reuse가 종료되면 최종 free 한다.
	 *  - 설정자: lib/thread/thread.c 내부 destroy 경로에서 단계별
	 *    증감.
	 *  - 읽는 자: 동일 destroy 경로의 분기 결정.
	 *  - 값 범위: 정상 사용 중 0, destroy 진행 시 양수.
	 *  - 동기화: 소유 스레드 단일 접근. */

	RB_ENTRY(spdk_io_channel)	node;
	/* [한국어] io_device가 보유한 채널 RB(Red-Black) 트리의 노드 링크
	 *  필드. spdk/tree.h(BSD sys/tree.h 포팅판)의 매크로가 좌/우/부모
	 *  포인터와 색상 비트를 자동으로 임베딩한다.
	 *  - 키(=비교 기준): 채널의 thread 포인터값. 같은 io_device 내에서
	 *    (thread → channel) 단일 매핑을 보장하기 위함.
	 *  - 설정자: 채널 생성 시 RB_INSERT, 파괴 시 RB_REMOVE.
	 *  - 읽는 자: 후속 spdk_get_io_channel() 호출이 동일 thread에서
	 *    재진입했을 때 RB_FIND로 기존 채널 조회.
	 *  - 값 범위: 트리에 삽입되어 있는 동안만 의미가 있는 내부 링크.
	 *  - 동기화: 트리 자체는 io_device가 소유하며 채널 등록/해제 시
	 *    SPDK 내부 락 또는 메시지로 직렬화된다. 일반 I/O 경로는
	 *    트리를 건드리지 않으므로 hot path에서는 비용이 없다. */

	spdk_io_channel_destroy_cb	destroy_cb;
	/* [한국어] 채널이 마지막 ref를 잃고 free 직전 호출되는 모듈 측
	 *  cleanup 콜백.
	 *  - 시그니처: void (*)(void *io_device, void *ctx_buf) — 모듈은
	 *    여기서 trailing 컨텍스트 안의 자원(NVMe qpair, 통계 버퍼 등)
	 *    을 해제한다.
	 *  - 설정자: spdk_io_device_register() 시 등록한 destroy_cb 함수
	 *    포인터를 채널 생성 시점에 본 필드로 복사.
	 *  - 읽는 자: spdk_put_io_channel() → ref==0 분기 → 본 콜백 호출.
	 *  - 값 범위: 유효한 함수 포인터 또는 NULL(정리할 trailing이
	 *    없는 단순 디바이스). NULL이면 호출을 생략한다.
	 *  - 동기화: 소유 스레드에서 호출되므로 모듈 측에서 추가 락 불필요. */

	uint8_t				_padding[40];
	/* [한국어] 향후 필드 추가를 위한 명시적 예약 패딩(40바이트).
	 *  - 왜 필요한가: 본 구조체 뒤쪽 메모리는 모듈별 trailing 컨텍스트가
	 *    차지한다. SPDK 마이너 버전 업데이트에서 새로운 내부 필드를
	 *    추가하면, trailing 시작 오프셋이 변하여 외부 모듈의 ABI가
	 *    깨진다. 이를 막기 위해 미리 충분한 슬랙을 넣어두고, 필드
	 *    추가 시 _padding 영역을 줄여 총 크기를 유지한다.
	 *  - 설정자: 사용되지 않음. 0으로 초기화될 수 있으나 정의되지 않음.
	 *  - 읽는 자: 사용되지 않음.
	 *  - 값 범위: 의미 없음(미사용 영역).
	 *  - 동기화: 의미 없음. */

	/*
	 * Modules will allocate extra memory off the end of this structure
	 *  to store references to hardware-specific references (i.e. NVMe queue
	 *  pairs, or references to child device spdk_io_channels (i.e.
	 *  virtual bdevs).
	 */
	/* [한국어] (트레일링 컨텍스트 영역 — 구조체 정의에는 명시되지 않은
	 *  가변 길이 영역)
	 *  - 메모리 배치: 본 구조체 끝(=_padding 뒤) 바로 다음 바이트부터
	 *    io_device 등록 시 명시한 ctx_size 만큼이 단일 할당으로 함께
	 *    잡혀 있다. 즉 채널 객체 1개 = 본 구조체 + ctx_size 바이트.
	 *  - 접근자: 공개 API spdk_io_channel_get_ctx(ch) 가
	 *    (uint8_t*)ch + sizeof(struct spdk_io_channel) 을 반환하여
	 *    이 영역을 가리킨다. 역방향(spdk_io_channel_from_ctx)도 동일.
	 *  - 모듈별 사용 예:
	 *      * NVMe : trailing에 nvme_qpair* 또는 qpair 자체를 두어
	 *               channel→qpair O(1) 매핑. read/write 커맨드는
	 *               이 qpair의 SQ에 SQE로 enqueue 후 doorbell write.
	 *      * bdev : trailing에 struct spdk_bdev_channel — bdev_io
	 *               할당 풀, 통계 카운터, 자식 bdev로의 다단 채널
	 *               참조 등이 들어 있다.
	 *      * vbdev: trailing에 자식 디바이스의 spdk_io_channel*들이
	 *               들어가 다단 forwarding 가능.
	 *  - 동기화: trailing 영역 자체는 채널 소유 스레드만 접근. */
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_io_channel) == SPDK_IO_CHANNEL_STRUCT_SIZE, "incorrect size");
/* [한국어] 컴파일 타임 검증: sizeof(struct spdk_io_channel)이 공개 헤더에
 *  선언된 약속값(SPDK_IO_CHANNEL_STRUCT_SIZE)과 정확히 일치해야 한다.
 *  - 왜 중요한가: 외부 모듈(예: NVMe 모듈)은 SPDK 라이브러리와 별도로
 *    컴파일/배포될 수 있다. 외부 모듈은 trailing 컨텍스트의 시작
 *    오프셋을 SPDK_IO_CHANNEL_STRUCT_SIZE 상수로 가정하고 빌드된다.
 *    내부 구조체 정의가 바뀌면서 크기가 달라지면, 외부 모듈이 잘못된
 *    오프셋으로 trailing ctx에 접근 → 메모리 훼손/크래시.
 *  - 위반 시 동작: 이 매크로는 typedef-with-bitfield 트릭으로 빌드
 *    실패를 유발하므로, 잘못된 변경은 런타임이 아닌 컴파일 타임에
 *    즉시 잡힌다. */

#endif /* SPDK_THREAD_INTERNAL_H_ */
/* [한국어] include 가드 종료. 이 위치 아래에 코드를 추가하지 말 것 —
 *  가드 밖의 정의는 다중 포함 보호를 받지 못한다. */
