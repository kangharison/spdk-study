/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] bdev 코어 내부 헬퍼 선언 (bdev_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK bdev 레이어의 **공개되지 않은** 내부 헬퍼 함수 프로토타입과 내부
 * 상수를 모아둔 헤더이다. lib/bdev/ 디렉토리 내 여러 .c 파일이 서로
 * 호출해야 하지만 외부(애플리케이션, 다른 라이브러리, 외부 bdev 모듈)에는
 * 노출되어서는 안 되는 구현 세부 함수가 여기에 선언된다. 공개 헤더는
 * include/spdk/bdev.h 와 include/spdk/bdev_module.h 두 종류로 분리되어
 * 있고, 본 파일은 그 둘 어디에도 들어가지 않을 "셋째 카테고리"에 해당한다.
 * ABI 호환 보증 대상이 아니며, SPDK 마이너 버전마다 함수 시그니처가
 * 바뀔 수 있다. 외부 코드에서 본 헤더를 include 하는 행위는 금지된다.
 *
 * 본 파일이 제공하는 핵심 항목:
 *   1) ZERO_BUFFER_SIZE 매크로 — write_zeroes 폴백용 호스트 제로 버퍼
 *      청크 크기 상수.
 *   2) bdev_channel_get_io() — 채널 로컬 bdev_io 풀에서 슬롯 1개 획득.
 *   3) bdev_io_init() — bdev_io 객체 기본 필드 초기화 (타겟 bdev,
 *      완료 콜백, 컨텍스트 등).
 *   4) bdev_io_submit() — 준비된 bdev_io를 모듈의 submit_request 콜백
 *      또는 QoS/nomem 큐로 디스패치.
 *   5) bdev_alloc_io_stat() / bdev_free_io_stat() — per-device 통계
 *      구조체 할당/해제.
 *   6) bdev_reset_device_stat() + 콜백 typedef — bdev 전체 채널의
 *      통계를 비동기 리셋.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택의 bdev 레이어 내부에 위치한다. 호출 흐름:
 *
 *   [Application]
 *     → spdk_bdev_read/write/unmap/...        (공개 API, bdev.h)
 *         → bdev_channel_get_io()             (본 헤더)
 *         → bdev_io_init()                    (본 헤더)
 *         → bdev_io_submit()                  (본 헤더)
 *             → bdev module submit_request()  (bdev_module.h vtable)
 *               (NVMe / aio / uring / malloc / null / lvol / raid 등)
 *                 → 실제 백엔드 I/O (NVMe SQ doorbell, libaio io_submit,
 *                                    io_uring sqe push, memcpy 등)
 *             ← 비동기 완료
 *         → bdev_io 의 internal.cb(cb_arg, success) 호출
 *     ← 사용자 콜백 실행 (해당 채널 소유 스레드)
 *
 * 실행 컨텍스트: 본 헤더의 모든 함수는 "해당 bdev_channel의 소유
 * spdk_thread"에서 호출되어야 한다 — 이것이 bdev 코어가 락 없이 채널
 * 로컬 bdev_io 풀, 통계 카운터, in-flight 카운터를 관리할 수 있는
 * 근거다. 다른 reactor 스레드에서 bdev I/O를 발행하려면 사용자가
 * spdk_thread_send_msg()로 채널 소유 스레드로 점프시켜야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/bdev.h : 공개 타입 — spdk_bdev_io_completion_cb 시그니처,
 *                   spdk_bdev_io_stat (전방선언으로 충분), enum
 *                   spdk_bdev_reset_stat_mode 정의의 출처.
 * 의존하는 모듈(이 헤더를 include 하는 .c 파일):
 *   - lib/bdev/bdev.c       : 본 헤더에 선언된 모든 함수의 정의/사용처.
 *   - lib/bdev/bdev_rpc.c   : RPC 핸들러에서 통계 리셋 등을 호출.
 *   - lib/bdev/bdev_zone.c  : zoned bdev I/O 발행 시 bdev_io_init/submit.
 *   - lib/bdev/part.c       : 파티션 가상 bdev가 자식 bdev로 I/O를 다시
 *                              발행할 때 사용.
 *
 * 데이터 흐름: 사용자가 발행한 read/write 요청은 spdk_bdev_io 객체로
 * 직렬화되어 본 헤더의 함수들을 통해 모듈로 전달되고, 완료 시 같은
 * 채널 스레드에서 사용자 콜백이 실행된다. 채널/디바이스 단위 통계는
 * bdev_alloc_io_stat() 으로 할당된 spdk_bdev_io_stat 구조체에 누적된다.
 *
 * 공유 자료구조: spdk_bdev, spdk_bdev_io, spdk_bdev_channel — 모두
 * 본 파일에서는 incomplete type(전방선언)으로만 다루며, 실제 정의는
 * lib/bdev/bdev.c (또는 비공개 헤더)에 있다. 외부 코드가 내부 필드에
 * 접근하지 못하도록 의도적으로 숨긴 형태다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - ZERO_BUFFER_SIZE (매크로):
 *       write_zeroes 폴백용 1 MiB 제로 버퍼 청크 크기. 장치가
 *       WRITE_ZEROES 커맨드를 미지원할 때 SPDK가 호스트에서 제로
 *       버퍼를 만들고 일반 write로 분할 발행하기 위한 단위.
 *
 *   - bdev_channel_get_io():
 *       채널 로컬 free list/mempool에서 미사용 spdk_bdev_io 슬롯 1개
 *       반환. 풀 고갈 시 NULL.
 *
 *   - bdev_io_init():
 *       반환된 bdev_io 슬롯에 타겟 bdev, 사용자 cb, cb_arg, 내부
 *       기본값(상태, 리스트 노드 등)을 세팅. submit 직전 단계.
 *
 *   - bdev_io_submit():
 *       준비된 bdev_io를 (a) QoS 통과 시 모듈 submit_request 콜백으로
 *       즉시 전달 또는 (b) nomem/in-flight 한계 도달 시 nomem 큐에
 *       대기시킨 후 완료 이벤트에서 drain.
 *
 *   - bdev_alloc_io_stat() / bdev_free_io_stat():
 *       per-device I/O 통계 구조체의 할당과 해제. io_error_stat
 *       플래그가 켜지면 에러 타입별 카운터까지 포함하여 더 큰 구조체
 *       를 할당.
 *
 *   - bdev_reset_device_stat_cb (typedef):
 *       위 통계 리셋의 비동기 완료 콜백 시그니처.
 *
 *   - bdev_reset_device_stat():
 *       bdev 전체 채널의 통계를 mode(MAXIMUMS_RESET 등)에 따라
 *       리셋. 모든 채널 스레드를 spdk_thread_send_msg로 순회한 뒤
 *       마지막에 cb 호출.
 */

#ifndef SPDK_BDEV_INTERNAL_H
/* [한국어] include 가드 시작 — 동일 컴파일 단위에서 다중 포함되어도
 *  타입/매크로 중복 정의가 발생하지 않도록 한다. 일반적인 C 헤더 관용구. */
#define SPDK_BDEV_INTERNAL_H

#include "spdk/bdev.h"
/* [한국어] bdev 레이어 공개 헤더.
 *  - 이 헤더에서 직접 필요한 항목:
 *      (a) spdk_bdev_io_completion_cb 함수 포인터 타입
 *           — bdev_io_init() 의 cb 인자 타입.
 *      (b) enum spdk_bdev_reset_stat_mode 의 식별자 자체
 *           (본 헤더에서는 forward-declare만 하지만, 사용처에서는
 *            완전 정의가 필요하므로 함께 끌어오는 것이 일반적).
 *      (c) struct spdk_bdev_io_stat 의 존재 표시(전방선언이 충분).
 *  - 왜 공개 헤더를 끌어오는가: 본 내부 헤더가 사용하는 콜백 시그니처가
 *    공개 API와 동일해야 하므로, 정의의 단일 출처를 spdk/bdev.h 로
 *    유지한다. */

#define ZERO_BUFFER_SIZE	0x100000
/* [한국어] write_zeroes 폴백 시 호스트 메모리에 할당하는 제로 버퍼의
 *  단위 청크 크기 = 0x100000 = 1 MiB.
 *  - 왜 필요한가: NVMe Write Zeroes 커맨드(opcode 0x08)를 지원하지
 *    않는 장치(또는 영역 매핑이 제한적인 가상 bdev)에서 사용자
 *    spdk_bdev_write_zeroes() 가 호출되면, bdev 코어는 호스트 측에
 *    영(0)으로 채워진 버퍼를 만들고 일반 write 커맨드를 반복 발행하여
 *    동일한 의미를 달성한다. 그 단위 버퍼 크기.
 *  - 1 MiB 선택 근거: 너무 작으면 NVMe SQ에 enqueue되는 커맨드 수가
 *    많아져 doorbell write·SQE 처리 오버헤드 증가. 너무 크면 hugepage
 *    DMA 버퍼 pinning 비용·캐시 압박 증가. 1 MiB는 다양한 NVMe SSD
 *    에서 단일 커맨드 처리에 적당한 절충값으로 SPDK가 채택한 휴리스틱.
 *  - 사용처: lib/bdev/bdev.c 의 write_zeroes 폴백 경로.
 *  - 동기화: 컴파일 타임 상수, 동기화 무관. */

struct spdk_bdev;
/* [한국어] 전방선언(opaque) — bdev 레이어가 등록·관리하는 단일 블록
 *  디바이스의 표현. NVMe namespace 1개, AIO 파일 1개, malloc bdev 1개
 *  등 다양한 백엔드가 모두 이 타입으로 추상화된다.
 *  - 본 헤더에서는 포인터로만 다루므로 incomplete type으로 충분.
 *  - 완전 정의: lib/bdev/bdev.c 내부의 비공개 정의(공개 헤더에는
 *    노출되지 않는다).
 *  - 동기화: bdev 객체 자체는 등록 후 비교적 정적이며, 채널 단위로
 *    분산된 mutable 상태(통계, in-flight 큐 등)는 spdk_bdev_channel
 *    측에 존재. */

struct spdk_bdev_io;
/* [한국어] 전방선언(opaque) — 단일 I/O 요청 객체.
 *  - 의미: spdk_bdev_read/write/unmap/flush/zone-mgmt 등 모든 bdev
 *    레벨 요청은 이 객체 하나로 표현된다. 사용자 cb_fn, cb_arg,
 *    오프셋/길이, IOV, 타겟 bdev 포인터, 내부 상태(상태 머신
 *    위치, nomem 큐 링크 등) 가 들어 있다.
 *  - 수명: bdev_channel_get_io()로 풀에서 획득 → bdev_io_init() 으로
 *    필드 세팅 → bdev_io_submit() 으로 모듈 제출 → 모듈 완료 콜백 →
 *    사용자 cb 호출 → 풀에 반환. 모두 동일 채널 스레드에서.
 *  - 본 헤더에서는 포인터로만 다루므로 incomplete type으로 충분. */

struct spdk_bdev_channel;
/* [한국어] 전방선언(opaque) — bdev 채널의 thread-local 상태.
 *  - 의미: spdk_io_channel(공통 채널 추상화)의 trailing 컨텍스트로
 *    배치되며, 채널이 속한 bdev 포인터, 채널 로컬 bdev_io 풀, 통계
 *    카운터, QoS 토큰 버킷, in-flight 큐, nomem 대기 큐 등을 모두
 *    소유한다.
 *  - 설정자/해제자: io_device(=bdev) create_cb/destroy_cb 가 trailing
 *    영역 위에 placement-style 로 초기화/정리.
 *  - 동기화: 소유 spdk_thread 단일 접근 (lockless). */

struct spdk_bdev_io *bdev_channel_get_io(struct spdk_bdev_channel *channel);
/*
 * [한국어]
 * bdev_channel_get_io - 채널별 bdev_io 풀에서 사용 가능한 슬롯 1개 획득
 *
 * @channel: 호출 컨텍스트의 spdk_bdev_channel. 호출 스레드는 반드시 이
 *           채널의 소유 스레드여야 한다 (thread-affinity 위반 시 풀
 *           카운터가 망가져 정의되지 않은 동작).
 * @return : (성공) 사용 준비가 된 spdk_bdev_io* — 호출자는 이어서
 *            bdev_io_init() 으로 필드를 채우고 bdev_io_submit() 한다.
 *           (실패) NULL — 채널 풀이 고갈되었음을 의미. 일반적으로
 *            호출자가 사용자 cb를 즉시 -ENOMEM 으로 완료시키거나,
 *            상위가 nomem 큐에 적재해 재시도한다.
 *
 * 왜 필요한가: bdev I/O hot path에서 매번 malloc 호출을 피하기 위해
 *   각 채널은 자체 bdev_io 풀(free list 또는 DPDK mempool)을 보유한다.
 *   본 함수는 그 풀에서 비어 있는 한 슬롯을 pop 하는 단순 인터페이스다.
 *
 * 동작:
 *   1) channel 내부 free list가 비어 있는지 확인.
 *   2) 비어 있지 않으면 head 1개를 떼어내어 반환.
 *   3) 비어 있으면 NULL 반환 (확장 할당은 하지 않는다).
 *
 * 실행 컨텍스트: 채널 소유 spdk_thread (단일 reactor). 락 없음.
 *
 * 호출자(caller): 공개 API 진입 함수 — spdk_bdev_read, spdk_bdev_write,
 *   spdk_bdev_unmap, spdk_bdev_flush, spdk_bdev_zone_management,
 *   spdk_bdev_reset, 그리고 part.c 의 부모-자식 forwarding 경로.
 *
 * 호출처(callee): 채널 내부 free-list 헤드 조작(매크로 또는 인라인).
 *   이 함수 자신은 어떠한 모듈 콜백도 부르지 않는다.
 *
 * 에러 경로: NULL 반환만이 유일한 실패 신호. 호출자가 이를 -ENOMEM
 *   으로 변환해 사용자 cb를 즉시 완료시키거나, 가능한 경우 nomem
 *   대기 큐에 요청을 보류해두고 in-flight I/O 완료 시 drain 한다.
 *
 * 호출 체인:
 *   spdk_bdev_read/write/...
 *     → [bdev_channel_get_io]
 *     → bdev_io_init
 *     → bdev_io_submit
 *     → 모듈 submit_request
 */

void bdev_io_init(struct spdk_bdev_io *bdev_io, struct spdk_bdev *bdev, void *cb_arg,
		  spdk_bdev_io_completion_cb cb);
/*
 * [한국어]
 * bdev_io_init - bdev_io 객체의 기본 필드를 초기화 (submit 직전 단계)
 *
 * @bdev_io: bdev_channel_get_io() 로 막 획득한 슬롯. 내용은 미정의 상태
 *           이며 본 함수가 정상 상태로 만든다.
 * @bdev   : I/O가 향할 타겟 spdk_bdev 포인터. NULL 불가.
 * @cb_arg : 사용자 컨텍스트. 완료 콜백 cb 의 첫 인자로 그대로 전달됨.
 * @cb     : 비동기 완료 콜백. 시그니처는
 *             void (*)(struct spdk_bdev_io *bdev_io, bool success,
 *                      void *cb_arg);
 *           — 모듈로부터 완료 통지가 오면 채널 소유 스레드에서 호출된다.
 *
 * 왜 필요한가: bdev_io 슬롯은 풀에서 재사용되므로 이전 사용 잔재를
 *   확실히 지우고 본 요청에 맞는 기본값을 세팅해야 한다. 호출자는
 *   본 함수 호출 후 자신이 발행하는 I/O 종류(read/write/unmap/...)에
 *   특화된 추가 필드(예: iov, offset, num_blocks)를 더 채운다.
 *
 * 동작 (개략):
 *   1) bdev_io->bdev = bdev
 *   2) bdev_io->internal.cb = cb, internal.caller_ctx = cb_arg
 *   3) 상태 머신 진입 위치 초기화 (예: in-flight 아님, status_pending)
 *   4) 내부 리스트 노드 초기화 (TAILQ INIT 등 — nomem 큐, child 큐 대기에
 *      안전하도록)
 *   5) 통계/타임스탬프 슬롯 초기화 (선택적).
 *
 * 실행 컨텍스트: 채널 소유 spdk_thread.
 *
 * 호출자(caller): bdev_channel_get_io() 직후의 모든 공개 API 경로.
 * 호출처(callee): 단순 필드 대입 외 외부 호출 거의 없음.
 * 에러 경로: 본 함수는 실패하지 않는다 (반환값 없음).
 *
 * 호출 체인:
 *   bdev_channel_get_io → [bdev_io_init] → 호출자별 추가 필드 세팅
 *   → bdev_io_submit
 */

void bdev_io_submit(struct spdk_bdev_io *bdev_io);
/*
 * [한국어]
 * bdev_io_submit - 준비된 bdev_io를 bdev 모듈 submit_request 경로로 전달
 *
 * @bdev_io: bdev_io_init() 이후 호출자가 모든 추가 필드(iov, offset,
 *           num_blocks 등)를 세팅 완료한 객체. 호출 직전이 hand-off
 *           시점이며, 호출 이후에는 모듈/완료 경로가 소유한다.
 *
 * 왜 필요한가: bdev_io 의 실제 디스패치는 단순한 모듈 콜백 호출만으로
 *   끝나지 않는다. SPDK bdev 코어는 다음을 본 함수에서 일괄 처리한다:
 *     (1) QoS rate limiter 토큰 차감 — 한도를 넘어서면 QoS 큐로 보류,
 *         poller가 토큰 보충 시 drain.
 *     (2) in-flight 카운터 증가, 채널 통계 시작 시점 기록.
 *     (3) 채널이 reset/abort 진행 중이면 즉시 실패 완료 경로로 전환.
 *     (4) 모듈 fn_table->submit_request(channel, bdev_io) 호출.
 *     (5) 모듈이 -ENOMEM 동치 신호를 즉시 반환하면 nomem 대기 큐에
 *         적재. 다른 I/O 완료 시 drain 호출됨.
 *
 * 비동기 완료 모델: 모듈 submit_request 자체는 단순 enqueue로 즉시
 *   리턴하고, 실제 디바이스 완료는 poller 가 폴링한 결과로 별도
 *   bdev_io_complete 경로를 거쳐 사용자 cb_fn(bdev_io, success, cb_arg)
 *   를 부른다. 이 모든 단계는 동일 채널 소유 스레드에서 실행된다.
 *
 * 실행 컨텍스트: 채널 소유 spdk_thread. cross-thread 호출 시 사용자가
 *   먼저 spdk_thread_send_msg 로 우회해야 한다.
 *
 * 호출자(caller): 공개 API spdk_bdev_read/write/... 의 마지막 단계,
 *   QoS/nomem drain 경로(완료 핸들러), part.c 자식-bdev forwarding.
 * 호출처(callee): 모듈의 submit_request 콜백 (bdev_module.h 등록).
 * 에러 경로: 즉시 실패는 사용자 cb 를 false 로 부르는 형태로 반환.
 *
 * 호출 체인:
 *   공개 bdev API
 *     → bdev_channel_get_io → bdev_io_init
 *     → [bdev_io_submit]
 *         → (모듈) submit_request — 예: bdev_nvme_submit_request
 *             → NVMe SQE 작성 + doorbell write
 *         (or) → nomem 큐 enqueue (한도 초과 시)
 */

struct spdk_bdev_io_stat *bdev_alloc_io_stat(bool io_error_stat);
void bdev_free_io_stat(struct spdk_bdev_io_stat *stat);
/*
 * [한국어]
 * bdev_alloc_io_stat / bdev_free_io_stat
 *   — per-device I/O 통계 구조체 spdk_bdev_io_stat 의 할당과 해제
 *
 * @io_error_stat (할당 시):
 *   - true  : 에러 타입별(예: NVMe SC 코드 카테고리) 카운터 배열까지
 *             포함하는 확장 형태로 할당. 메모리 사용량 더 크지만
 *             장애 분석/오류 분포 추적이 가능해진다.
 *   - false : 기본 통계(읽기/쓰기 IOPS, 바이트, latency 누적 등)만
 *             포함하는 작은 형태로 할당.
 * @return (할당 시): 0으로 초기화된 spdk_bdev_io_stat* — 즉시 사용
 *   가능한 상태. 메모리 부족 시 NULL.
 *
 * @stat (해제 시): bdev_alloc_io_stat() 가 반환한 포인터. NULL이면
 *   no-op. (보통 free 와 같은 관용 보호)
 *
 * 왜 함수 분리: 일반 calloc/free 가 아니라 별도 함수로 두는 이유는
 *   (a) io_error_stat 분기로 크기가 동적으로 결정되고,
 *   (b) 향후 풀 캐시·hugepage 정렬 변경 시 한 곳만 수정하면 되도록
 *       추상화 경계를 둔 것이다.
 *
 * 실행 컨텍스트: 보통 bdev 등록/해제 시(특정 스레드 보장 없음).
 *   다만 호출 빈도가 매우 낮은 cold path 이므로 락 비용이 문제되지
 *   않는다. 통계 구조체 자체에 대한 락은 별도로 관리된다.
 *
 * 호출자(caller):
 *   - 할당: bdev 등록 경로(spdk_bdev_register 내부) — bdev 1개당 1회.
 *   - 해제: bdev 등록 해제 경로(spdk_bdev_unregister 의 마지막 단계).
 *
 * 호출처(callee): 표준 calloc/free (또는 spdk_zmalloc), 그 이상은 없음.
 *
 * 에러 경로: 할당 실패 시 NULL 반환 — 호출자는 등록을 -ENOMEM 으로
 *   실패시킨다.
 */

enum spdk_bdev_reset_stat_mode;
/* [한국어] 전방선언 — 실제 enum 정의는 공개 헤더 spdk/bdev.h 에 위치.
 *  - 본 헤더에서는 함수 인자 타입으로만 사용되며, 정의 없이도 포인터·
 *    값 전달 시그니처를 컴파일러가 검증할 수 있도록 forward-declare로
 *    충분.
 *  - 가능한 모드 예(스펙 출처: spdk/bdev.h):
 *      SPDK_BDEV_RESET_STAT_ALL       — 모든 카운터 0으로 리셋.
 *      SPDK_BDEV_RESET_STAT_MAXIMUMS  — 최대값 추적 카운터만 리셋.
 *      SPDK_BDEV_RESET_STAT_NONE      — 리셋 없음(상태 조회 용도).
 *  - 동기화: 값 자체는 enum이라 동기화 무관. */

typedef void (*bdev_reset_device_stat_cb)(struct spdk_bdev *bdev, void *cb_arg, int rc);
/* [한국어] bdev_reset_device_stat() 의 비동기 완료 콜백 함수 포인터 타입.
 *  - 시그니처:
 *      void cb(struct spdk_bdev *bdev, void *cb_arg, int rc);
 *      @bdev   : 대상 bdev (요청 시 전달한 포인터 그대로).
 *      @cb_arg : 사용자 컨텍스트(요청 시 전달한 값 그대로).
 *      @rc     : 0 성공, 음수 errno(EINVAL/EIO 등) 실패.
 *  - 호출 스레드: 보통 bdev_reset_device_stat() 를 호출했던 스레드
 *    또는 bdev 마스터 스레드. 자세한 보장은 lib/bdev/bdev.c 구현 참조.
 *  - 설정자: bdev_reset_device_stat() 호출자가 인자로 넘긴 값.
 *  - 읽는 자: bdev 코어가 모든 채널 통계 리셋을 마치고 마지막에 1회 호출.
 *  - 값 범위: 유효한 함수 포인터(NULL 일반적으로 비허용 — 비동기
 *    완료를 알 길이 없으므로).
 *  - 동기화: 콜백 내부 동기화는 사용자 책임. */

void bdev_reset_device_stat(struct spdk_bdev *bdev, enum spdk_bdev_reset_stat_mode mode,
			    bdev_reset_device_stat_cb cb, void *cb_arg);
/*
 * [한국어]
 * bdev_reset_device_stat - bdev 전체 채널의 I/O 통계를 비동기 리셋
 *
 * @bdev  : 대상 spdk_bdev. 등록된 채널이 0개여도 정상 동작(즉시 cb 호출).
 * @mode  : 리셋 범위(전체 vs 최대값만 등) — enum spdk_bdev_reset_stat_mode.
 * @cb    : 모든 채널 리셋 완료 후 호출될 비동기 콜백.
 * @cb_arg: cb 의 첫 컨텍스트 인자로 전달될 값.
 *
 * 왜 비동기인가: bdev 통계는 (전역 합계) + (채널별 분산 카운터) 형태로
 *   분리 보관된다. 채널 카운터는 채널 소유 스레드에서만 안전하게 접근
 *   가능하므로, 모든 채널을 리셋하려면 각 채널 소유 스레드로
 *   spdk_thread_send_msg() 메시지를 보내 그 스레드에서 카운터를 0으로
 *   만들고, 마지막 메시지 처리 후 집계 응답을 모아야 한다. 이 한 번의
 *   왕복은 호출자 입장에서는 비동기로만 표현 가능하다.
 *
 * 동작 (개략):
 *   1) bdev->internal 등에서 등록된 채널 목록을 순회 시작.
 *   2) 각 채널 소유 스레드로 reset 메시지 송신
 *      (spdk_for_each_channel 패턴).
 *   3) 모든 채널이 응답 완료하면 글로벌 통계도 mode에 따라 정리.
 *   4) 사용자 cb(bdev, cb_arg, rc) 호출.
 *
 * 실행 컨텍스트: 호출은 보통 bdev 마스터 스레드 또는 RPC 핸들러
 *   스레드에서 발생. 콜백 cb 는 SPDK 내부가 결정한 스레드에서 실행됨.
 *
 * 호출자(caller): RPC 통계 리셋 경로(bdev_rpc.c), 일부 테스트/도구.
 * 호출처(callee): spdk_for_each_channel(또는 동치) → 각 채널 스레드의
 *   리셋 함수. 마지막에 사용자 cb 호출.
 * 에러 경로: 채널 순회 중 비정상 상태 발견 시 cb 의 rc 인자에 음수
 *   errno 전달. cb 자체는 항상 호출됨(memory allocation 실패 등 매우
 *   초기 단계 실패가 아닌 한).
 *
 * 호출 체인:
 *   RPC handler / tool
 *     → [bdev_reset_device_stat]
 *         → spdk_for_each_channel(...)  (각 채널 스레드로 메시지)
 *             → 채널별 카운터 0 처리
 *         → 마지막 단계에서 cb(bdev, cb_arg, rc) 호출
 */

#endif /* SPDK_BDEV_INTERNAL_H */
/* [한국어] include 가드 종료. 이 위치 아래에 코드를 추가하지 말 것 —
 *  가드 밖 정의는 다중 포함 보호를 받지 못한다. */
