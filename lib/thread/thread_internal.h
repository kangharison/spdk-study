/*   SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * [한국어 설명] spdk_thread 서브시스템 내부 타입 (thread_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK thread 서브시스템의 **내부** 구조체 `struct spdk_io_channel` 정의를
 * 담는다. 공개 헤더 spdk/thread.h는 spdk_io_channel을 opaque로 노출하지만,
 * 구현부(lib/thread/thread.c, bdev 코어 등)에서는 실제 필드 접근이 필요하다.
 * 이 헤더는 그런 내부 접근자를 위한 타입 노출 용도이며 공개 API가 아니다.
 *
 * `spdk_io_channel`은 SPDK I/O 경로의 핵심 추상화이다. "특정 스레드에서 특정
 * I/O 디바이스(NVMe 컨트롤러, blobstore 등)에 접근하기 위한 핸들"로, 다음
 * 불변식을 가진다:
 *   - 하나의 (thread, device) 쌍에 대해 정확히 하나의 spdk_io_channel
 *   - 스레드 고정 — 생성된 스레드에서만 사용 가능
 *   - 참조 카운팅으로 수명 관리
 *   - 구조체 뒤쪽에 모듈별 컨텍스트가 **붙어 있음**(flexible trailing area)
 *
 * === 전체 아키텍처에서의 위치 ===
 * 공개 spdk_io_channel API(spdk_io_channel_get_thread, _get_io_device,
 * _get_ctx 등)는 이 구조체를 기반으로 동작. NVMe 경로에서는 이 채널의
 * trailing 컨텍스트에 `struct nvme_qpair` 포인터가 저장되어 read/write
 * 커맨드가 해당 qpair로 제출된다. bdev 경로에서는 trailing이
 * `struct spdk_bdev_channel`이 된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/assert.h - SPDK_STATIC_ASSERT (구조체 크기 불변식 검증)
 *   - spdk/thread.h - 공개 타입 및 SPDK_IO_CHANNEL_STRUCT_SIZE 상수
 *   - spdk/tree.h   - RB tree 매크로 (io_device 내 채널 인덱싱)
 * 의존하는 모듈:
 *   - lib/thread/thread.c (채널 생성·해제·조회 구현)
 *   - lib/bdev/bdev.c (bdev_channel → io_channel 래핑)
 *   - lib/nvme (qpair 접근 채널)
 * 공유 자료구조: struct spdk_io_channel 자체.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_io_channel: per-thread I/O 디바이스 핸들
 */

#ifndef SPDK_THREAD_INTERNAL_H_  /* [한국어] include 가드 시작 */
#define SPDK_THREAD_INTERNAL_H_

#include "spdk/assert.h"         /* [한국어] SPDK_STATIC_ASSERT — 구조체 크기 검증 */
#include "spdk/thread.h"         /* [한국어] spdk_io_channel_destroy_cb 등 타입, SPDK_IO_CHANNEL_STRUCT_SIZE 상수 */
#include "spdk/tree.h"           /* [한국어] RB_ENTRY 매크로 — RB 트리 링크 필드 임베딩용 */

/**
 * \brief Represents a per-thread channel for accessing an I/O device.
 *
 * An I/O device may be a physical entity (i.e. NVMe controller) or a software
 *  entity (i.e. a blobstore).
 *
 * This structure is not part of the API - all accesses should be done through
 *  spdk_io_channel function calls.
 */
struct spdk_io_channel {
	struct spdk_thread		*thread;
                                 /* [한국어] 이 채널을 소유한 SPDK thread (reactor 스레드)
                                  *  - 설정자: spdk_get_io_channel 내부 — 호출한 스레드를 기록
                                  *  - 읽는 자: spdk_io_channel_get_thread, 크로스스레드 호출 검증
                                  *  - 값 범위: 유효한 spdk_thread* (NULL 불가)
                                  *  - 동기화: 생성 이후 불변. 소유 스레드 외에서 필드 변경 금지 */
	struct io_device		*dev;
                                 /* [한국어] 이 채널이 가리키는 I/O 디바이스 (내부 핸들)
                                  *  - spdk_io_device_register로 등록된 장치의 내부 표현
                                  *  - NVMe 컨트롤러, blobstore, bdev 등이 모두 io_device로 추상화 */
	uint32_t			ref;
                                 /* [한국어] 참조 카운터. 같은 스레드·디바이스 조합에 대해 get 시 증가, put 시 감소
                                  *  - 0에 도달하면 destroy_cb 호출 후 구조체 해제
                                  *  - 동기화: 소유 스레드에서만 조작(락 없음) */
	uint32_t			destroy_ref;
                                 /* [한국어] 파괴 중 임시로 유지되는 참조 수
                                  *  - destroy_cb 실행 동안 재입/재get을 안전하게 처리하기 위한 임시 카운터 */
	RB_ENTRY(spdk_io_channel)	node;
                                 /* [한국어] io_device의 RB 트리 내 채널 인덱스 링크 필드
                                  *  - (thread, device) 조합으로 channel lookup을 O(log n)에 수행 */
	spdk_io_channel_destroy_cb	destroy_cb;
                                 /* [한국어] 채널 파괴 시 모듈 측 cleanup 콜백 (trailing 컨텍스트 해제 등)
                                  *  - 설정자: spdk_io_device_register 등록 시 지정된 destroy_cb 포인터를 복사 */

	uint8_t				_padding[40];
                                 /* [한국어] 고정 패딩 — ABI 안정성 보장 목적
                                  *  - 현재 미사용 필드 자리. 추후 멤버 추가 시 이 영역 사용하면 trailing 모듈 컨텍스트 오프셋 불변 유지
                                  *  - SPDK_STATIC_ASSERT로 총 크기가 SPDK_IO_CHANNEL_STRUCT_SIZE와 일치함을 보장 */
	/*
	 * Modules will allocate extra memory off the end of this structure
	 *  to store references to hardware-specific references (i.e. NVMe queue
	 *  pairs, or references to child device spdk_io_channels (i.e.
	 *  virtual bdevs).
	 */
                                 /* [한국어] 이 구조체 뒤쪽에 모듈별 컨텍스트가 붙는다 — "flexible trailing" 관례
                                  *  - NVMe: trailing에 nvme_qpair*가 들어감 → 호출자가 qpair로 SQ 제출
                                  *  - bdev: trailing에 spdk_bdev_channel이 들어감 → bdev_io submit 경로
                                  *  - 모듈 등록 시 `ctx_size`를 알려주면 spdk_io_channel 뒤 ctx_size 바이트가 자동 포함됨 */
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_io_channel) == SPDK_IO_CHANNEL_STRUCT_SIZE, "incorrect size");
                                 /* [한국어] 컴파일 타임 검증 — 구조체 크기가 공개 헤더의 상수와 일치해야 함
                                  *  - 이 조건이 깨지면 외부 모듈이 잘못된 오프셋으로 trailing ctx에 접근 → 메모리 훼손 */

#endif /* SPDK_THREAD_INTERNAL_H_ */ /* [한국어] include 가드 종료 */
