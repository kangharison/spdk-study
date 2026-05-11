/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] IDXD/DSA(Intel Data Streaming Accelerator) 내부 헤더 (idxd_internal.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK가 Intel DSA(Data Streaming Accelerator) 및 IAA(In-Memory Analytics Accelerator)
 * 가속기를 유저스페이스에서 polled-mode로 구동할 때 필요한 내부 자료구조와 매크로를 정의한다.
 * DSA는 Sapphire Rapids 이후의 Xeon CPU에 내장된 차세대 메모리 가속기로, IOAT을 대체하여
 * 대용량 memcpy/memset/CRC32/compare 등 데이터 이동·연산 오프로드를 수행한다.
 * 본 헤더는 사용자/배치 디스크립터(spdk_idxd_io_channel/idxd_batch/idxd_ops),
 * 디바이스 추상화(spdk_idxd_device/spdk_idxd_impl), 그리고 MOVDIR64B 명령어로 디스크립터를
 * portal에 제출하는 인라인 헬퍼(movdir64b)를 선언한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [SPDK accel framework (lib/accel/accel_sw.c, accel_dsa.c)]
 *     → spdk_idxd_submit_copy/fill/crc32c/dualcast/compare(공개 API in spdk/idxd.h)
 *     → idxd.c (이 헤더의 자료구조 사용)
 *     → 두 backend 중 하나 분기 (impl→probe/portal_get_addr 등)
 *         · idxd_user.c   : 유저스페이스 직접 매핑 backend (VFIO + UACCE/SVM)
 *         · idxd_kernel.c : kernel 드라이버 경유 backend (/dev/dsa, libaccel-config)
 *     → MOVDIR64B → DSA/IAA HW
 * 실행 컨텍스트: 호스트 유저스페이스. 한 io_channel은 단일 SPDK thread에 고정되며,
 * descriptor 제출/완료 polling은 모두 그 스레드에서 직렬로 이뤄진다 (lockless).
 *
 * === 타 모듈과의 연결 ===
 * - spdk/idxd.h           : 공개 API (submit_copy/fill/crc32c, attach/probe 콜백 시그니처).
 * - spdk/idxd_spec.h      : DSA/IAA 하드웨어 디스크립터 포맷, opcode, completion record 정의.
 * - spdk/queue.h          : TAILQ/STAILQ - batch_pool, ops_pool, ops_outstanding 관리에 사용.
 * - spdk/mmio.h           : MMIO 접근 헬퍼 (이 파일에서는 주로 portal write에 movdir64b 사용).
 * - spdk_internal/idxd.h  : impl 등록 (SPDK_IDXD_IMPL_REGISTER) 매크로용 내부 헤더.
 * 데이터 흐름: 사용자가 submit_*() 호출 → idxd_ops 슬롯 1개를 ops_pool에서 꺼내 디스크립터를 채움
 *   → portal(MMIO)로 movdir64b 명령으로 64-byte 디스크립터 전송 → HW가 처리 후 completion record에 기록
 *   → process_events 폴러가 ops_outstanding 리스트를 훑어 완료 검출 → cb_fn 호출 → ops_pool 반환.
 *
 * === 주요 함수/구조체 요약 ===
 * - movdir64b()              : DSA portal에 64B 디스크립터를 atomic하게 enqueue하는 인라인 어셈블리.
 * - struct idxd_batch        : 배치 작업 구조체 - 한 batch당 user_desc/user_ops 배열을 가짐.
 * - struct spdk_idxd_io_channel : per-thread channel - portal, ops_pool, batch_pool, outstanding 리스트.
 * - struct idxd_ops          : 32B HW completion record + SW 콜백/메타데이터 (총 128B).
 * - struct spdk_idxd_impl    : backend(user/kernel) 추상화 - probe/destruct/portal_get_addr 콜백 테이블.
 * - struct spdk_idxd_device  : 디바이스 1개당 컨텍스트 - portal, NUMA, channel 카운트, IAA AECS 등.
 * - SPDK_IDXD_IMPL_REGISTER  : constructor 속성으로 backend impl을 g_idxd_impls 리스트에 등록.
 */

#ifndef __IDXD_H__
#define __IDXD_H__

/* [한국어] SPDK 표준 C 라이브러리 래퍼 - stdint/stdbool/string/unistd 등을 플랫폼 독립적으로 묶음. */
#include "spdk/stdinc.h"

/* [한국어] IDXD 공개 API: spdk_idxd_attach_cb, spdk_idxd_req_cb 등 콜백 시그니처. */
#include "spdk/idxd.h"
/* [한국어] BSD-스타일 STAILQ/TAILQ 매크로 - ops_pool/batch_pool 등 lockless 자료구조 관리. */
#include "spdk/queue.h"
/* [한국어] MMIO read/write 래퍼 - 컴파일러 재배치 방지(volatile + 메모리 배리어). */
#include "spdk/mmio.h"
/* [한국어] DSA/IAA 하드웨어 스펙: idxd_hw_desc, dsa_hw_comp_record, opcode 상수 등. */
#include "spdk/idxd_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TODO: get the gcc intrinsic to work. */
/* [한국어] CPU "no-operation" 명령 - 디버깅/타이밍 지연 용도(현재 코드에선 사용 위치 적음).
 * volatile은 컴파일러가 최적화로 제거하지 못하도록 보장. */
#define nop() asm volatile ("nop")

/*
 * [한국어]
 * movdir64b - 64-byte 디스크립터를 atomic하게 메모리(또는 MMIO portal)로 비-템포럴 이동
 *
 * @dst: 대상 주소 (DSA portal 또는 메모리). 64-byte 정렬 권장.
 * @src: 원본 디스크립터 주소 (struct idxd_hw_desc, 64 byte). 64-byte 정렬 필요.
 * @return: 없음 (void). 명령 자체는 비동기 - HW 큐 만석 시 retry는 호출자가 처리.
 *
 * 왜 필요한가: DSA는 디스크립터를 portal(=WQ의 MMIO 페이지)에 64-byte 단위로 전송해야 한다.
 * MOVDIR64B는 64-byte 단일 atomic write를 보장하는 Intel 명령어로, store buffer를 우회해
 * cache pollution 없이 디스크립터를 디바이스에 전달한다. 일반 memcpy는 64-byte atomicity를
 * 보장하지 않으므로 사용 불가.
 *
 * 동작 과정:
 *   1) 인라인 어셈블리로 0x66 0x0f 0x38 0xf8 0x02 (= MOVDIR64B [rdx], rax) 바이트 시퀀스를 직접 삽입.
 *   2) "d"(src)는 EDX/RDX, "a"(dst)는 EAX/RAX에 매핑되어 src→dst로 64B를 전송.
 *   3) "=m"(*(char *)dst) 출력 제약은 컴파일러에게 dst가 수정됨을 알려 reordering 방지.
 *
 * 실행 컨텍스트: 채널 소유 SPDK 스레드. submit() 핫패스에서 매 디스크립터 1개당 1회 호출.
 *
 * 호출 체인:
 *   spdk_idxd_submit_*() → idxd.c 내부 → [movdir64b] → DSA WQ portal
 */
static inline void movdir64b(void *dst, const void *src)
{
	/* [한국어] MOVDIR64B 명령의 raw 바이트(0x66 0x0f 0x38 0xf8 0x02) 삽입.
	 * 구형 컴파일러는 이 인텔 명령어를 모르므로 .byte 디렉티브로 직접 인코딩. */
	asm volatile(".byte 0x66, 0x0f, 0x38, 0xf8, 0x02"
		     : "=m"(*(char *)dst)   /* [한국어] 출력: dst가 메모리에서 수정된다고 컴파일러에 알림 (reordering 방지). */
		     : "d"(src), "a"(dst)); /* [한국어] 입력: RDX←src (디스크립터 원본), RAX←dst (portal 주소). */
}

/* [한국어] DSA 레지스터 작업 후 안정화 대기 시간 - 50µs.
 * 일부 레지스터(예: ENQCMD 결과 status)는 즉시 반영되지 않을 수 있어 polling 시 사용. */
#define IDXD_REGISTER_TIMEOUT_US		50
/* [한국어] WQ drain 명령(WQ를 비우는 동안 새 enqueue 차단)이 완료될 때까지의 최대 대기 - 500ms.
 * device reset/teardown 경로에서 in-flight 디스크립터를 모두 회수할 때 사용. */
#define IDXD_DRAIN_TIMEOUT_US			500000

/* [한국어] DSA WQ 모드 상수 - DEDICATED(전용 모드) 표시.
 * DSA WQ에는 DEDICATED(단일 producer 보장)와 SHARED(ENQCMDS로 다중 producer)가 있는데,
 * SPDK는 현재 DEDICATED 모드만 지원 (단일 SPDK 스레드 = single producer 가정). */
#define WQ_MODE_DEDICATED	1

/* TODO: consider setting the max per batch limit via RPC. */

/* [한국어] 디스크립터 1개의 최대 transfer 크기를 2의 지수로 표현 - 2^30 = 1 GiB.
 * DSA 한 디스크립터는 최대 2 GB까지 지원하지만 SPDK는 30비트(=1 GiB)로 보수적으로 제한.
 * 큰 요청은 idxd.c에서 max_xfer_size 단위로 split. */
#define LOG2_WQ_MAX_XFER	30 /* 2^30 = 1073741824 */
/* [한국어] 디스크립터 우선순위 (1 = lowest). DSA는 0..15 우선순위를 지원하며 1이 default. */
#define WQ_PRIORITY_1		1
/* [한국어] SPDK가 동시에 다룰 수 있는 IDXD 채널의 최대 개수 (per-device 상한).
 * 한 디바이스 당 chan_per_device(보통 4 또는 8)만 사용하지만, 컴파일 타임 안전 상한값. */
#define IDXD_MAX_QUEUES		64

/* [한국어] IDXD 디바이스 종류 enum - DSA(Data Streaming) 와 IAA(In-Memory Analytics) 구분.
 * SPDK는 두 디바이스 모두 같은 idxd_*() API로 다루지만, 일부 opcode/completion record는 다름. */
enum idxd_dev {
	IDXD_DEV_TYPE_DSA	= 0,
	/* [한국어] DSA: memcpy/fill/crc32/dualcast/compare 등 데이터 이동·간단 연산 가속기.
	 * 설정자: probe 시 디바이스 ID로 결정. 읽는 자: idxd.c가 opcode 결정 시 분기.
	 * 값 의미: 0 = 일반 SPDK accel offload 대상. */

	IDXD_DEV_TYPE_IAA	= 1,
	/* [한국어] IAA: 압축/해제(deflate)·필터링 등 분석 가속기. AECS(application execution context)
	 * 메모리가 추가로 필요하다.
	 * 설정자: probe 시 디바이스 ID로 결정. 읽는 자: aecs 할당/해제 경로에서 분기.
	 * 값 의미: 1 = compress/decompress 가속 대상. */
};

/* Each pre-allocated batch structure goes on a per channel list and
 * contains the memory for both user descriptors.
 */
/* [한국어] 배치 작업 구조체 - 여러 디스크립터를 한 번에 제출하는 "batch descriptor"의 SW 메타데이터.
 * DSA의 batch는 하나의 "outer" descriptor가 N개의 "inner(user)" descriptor 배열을 가리키는 구조.
 * 채널마다 batch_pool에 미리 할당되어 재사용된다. */
struct idxd_batch {
	struct idxd_hw_desc		*user_desc;
	/* [한국어] 이 batch에 속한 inner 디스크립터들의 HW descriptor 배열 (DMA 가능 메모리).
	 * 설정자: idxd_batch_create()/init이 desc_base에서 슬라이싱해 할당.
	 * 읽는 자: spdk_idxd_batch_prep_*()이 이 배열을 채움, HW가 batch outer desc의 desc_list로 참조.
	 * 값 범위: 길이 = chan->idxd->batch_size. 64-byte 정렬, 물리적으로 연속.
	 * 동기화: 한 batch는 한 스레드에서만 채워지므로 락 불필요. */

	struct idxd_ops			*user_ops;
	/* [한국어] inner 디스크립터들의 SW 측 op (callback/context) 배열, user_desc와 1:1 매칭.
	 * 설정자: 채널 init 시 ops_base에서 슬라이싱하여 batch마다 user_ops를 할당.
	 * 읽는 자: process_events()가 완료된 inner desc의 cb_fn을 찾을 때 user_ops[i]로 인덱싱.
	 * 값 범위: 길이 = batch_size. 각 entry는 32B HW completion record + SW 메타.
	 * 동기화: 채널 단일 스레드 접근. */

	uint64_t			user_desc_addr;
	/* [한국어] user_desc 배열의 시작 물리 주소(IOVA) - outer batch desc의 desc_list_addr 필드에 사용.
	 * 설정자: batch 초기화 시 spdk_vtophys(user_desc)로 1회 계산.
	 * 읽는 자: spdk_idxd_batch_submit()이 outer desc 채울 때 사용.
	 * 값 범위: hugepage 영역 IOVA. 64-byte 정렬.
	 * 동기화: 초기화 후 read-only. */

	uint16_t			index;
	/* [한국어] batch_pool 내에서의 인덱스 - 디버깅/추적용 식별자.
	 * 설정자: 채널 init 시 0..N-1로 부여.
	 * 읽는 자: 트레이스/로그에서 batch 식별.
	 * 값 범위: 0..(batch_pool 크기-1).
	 * 동기화: 변경되지 않음. */

	uint16_t			refcnt;
	/* [한국어] 이 batch에 현재 채워진 inner descriptor 개수(=완료 대기 중인 op 수).
	 * 설정자: spdk_idxd_batch_prep_*()마다 +1, process_events()가 완료 시 -1.
	 * 읽는 자: refcnt가 0이 되면 batch 자체를 batch_pool에 반환.
	 * 값 범위: 0 ~ batch_size. 0이면 idle 상태.
	 * 동기화: 채널 단일 스레드 접근 - atomic 불필요. */

	uint16_t			size;
	/* [한국어] 이 batch에 들어간 inner descriptor의 총 개수 (제출 시점의 refcnt 시작값).
	 * 설정자: spdk_idxd_batch_submit()이 현재 refcnt를 size로 캡처.
	 * 읽는 자: process_events()가 batch 완료 추적 시 사용.
	 * 값 범위: 1..batch_size.
	 * 동기화: submit 시점 1회 갱신. */

	struct spdk_idxd_io_channel	*chan;
	/* [한국어] 이 batch가 속한 io_channel 역참조 - completion 시 ops_pool 반환에 사용.
	 * 설정자: batch 초기화 시 자기 채널을 저장.
	 * 읽는 자: process_events() 등 cleanup 경로.
	 * 값 범위: 유효한 채널 포인터(NULL 불가).
	 * 동기화: 초기화 후 read-only. */

	TAILQ_ENTRY(idxd_batch)		link;
	/* [한국어] batch_pool TAILQ 노드 - free batch 풀에 삽입/제거에 사용.
	 * 설정자: TAILQ_INSERT_TAIL/REMOVE 매크로 (idxd.c).
	 * 읽는 자: batch 할당 시 TAILQ_FIRST로 꺼냄.
	 * 값 범위: 리스트 prev/next 포인터.
	 * 동기화: 채널 소유 스레드 단독 접근. */
};

/* [한국어] 디바이스 그룹 설정 정보 - DSA WQ를 그룹 단위로 묶는 컨피그.
 * 현재 SPDK 코드는 user backend에서만 활용 (kernel backend는 accel-config가 처리). */
struct device_config {
	uint8_t		config_num;
	/* [한국어] 미리 정의된 디바이스 설정 번호(0,1,2 …) - idxd_user.c의 g_dev_cfg[] 인덱스.
	 * 값 의미: 0=균등 분할, 1=단일 그룹 등 백엔드별로 다름. */

	uint8_t		num_groups;
	/* [한국어] DSA가 지원하는 그룹 개수 - WQ/Engine을 묶는 단위.
	 * 값 범위: 1~디바이스 GRPCAP. */

	uint16_t	total_wqs;
	/* [한국어] 디바이스 전체 WQ 개수 - 보통 4(DSA Gen1).
	 * 값 범위: 디바이스 GENCAP의 WQ 수. */

	uint16_t	total_engines;
	/* [한국어] 디바이스 엔진(=실제 데이터 이동 unit) 수 - 보통 4(DSA Gen1).
	 * 값 범위: 디바이스 GENCAP의 Engine 수. */
};

/* [한국어] 전방 선언 - struct idxd_ops은 아래에 정의되지만 spdk_idxd_io_channel에서 먼저 참조. */
struct idxd_ops;

/* [한국어] IDXD I/O 채널 - 한 스레드(=accel channel)당 1개의 인스턴스.
 * 채널은 portal(MMIO 페이지)을 단일 스레드 전용으로 사용해 lockless로 디스크립터 제출. */
struct spdk_idxd_io_channel {
	struct spdk_idxd_device			*idxd;
	/* [한국어] 이 채널이 속한 디바이스 역참조.
	 * 설정자: spdk_idxd_get_channel()이 디바이스 → 채널 결합 시 설정.
	 * 읽는 자: 거의 모든 채널 함수가 idxd→portal/batch_size 등 메타 접근에 사용.
	 * 값 범위: 유효한 디바이스 포인터.
	 * 동기화: 초기화 후 read-only, 채널 종료 시 num_channels_lock으로 보호. */

	/* The portal is the address that we write descriptors to for submission. */
	void					*portal;
	/* [한국어] 이 채널이 디스크립터 제출에 사용할 MMIO portal 주소(=WQ에 매핑된 4 KiB 페이지).
	 * 설정자: spdk_idxd_get_channel()이 디바이스 portal에서 4 KiB offset을 더해 채널별 분리 주소를 부여.
	 * 읽는 자: idxd.c가 movdir64b(portal + portal_offset, desc)로 디스크립터 enqueue.
	 * 값 범위: 4 KiB 정렬된 MMIO 주소.
	 * 동기화: 채널이 단일 스레드 소유 - lockless write. */

	uint32_t				portal_offset;
	/* [한국어] portal 페이지 내에서의 현재 write offset - DSA는 같은 portal 페이지의 다른 64B
	 * 슬롯에 라운드로빈 write를 권장하여 cache line bouncing을 분산.
	 * 설정자: 매 enqueue 후 64B 단위로 +0x40, 0x1000(4 KiB) 도달 시 0으로 wrap.
	 * 읽는 자: 매 movdir64b 호출 시 portal + portal_offset으로 실제 주소 계산.
	 * 값 범위: 0..0xFC0, 64B aligned.
	 * 동기화: 채널 단일 스레드 접근. */

	bool					pasid_enabled;
	/* [한국어] PASID(Process Address Space ID, SVM)가 이 채널에 활성화됐는지.
	 * 설정자: 채널 init 시 디바이스의 pasid_enabled을 그대로 복사.
	 * 읽는 자: 디스크립터 채울 때 PASID 사용 비트(0x10) 설정 여부 결정.
	 * 값 범위: true(IOMMU SVM, 가상주소 직접 사용) / false(IOVA 변환 필요).
	 * 동기화: 초기화 후 read-only. */

	/* The currently open batch */
	struct idxd_batch			*batch;
	/* [한국어] "현재 작성 중인" batch - 사용자가 spdk_idxd_batch_get으로 받아간 미제출 batch.
	 * 설정자: batch_get/submit 시 set/clear.
	 * 읽는 자: 채널이 batch 모드인지 확인.
	 * 값 범위: NULL(현재 열린 batch 없음) 또는 유효한 idxd_batch 포인터.
	 * 동기화: 채널 단일 스레드. */

	/*
	 * User descriptors (those included in a batch) are managed independently from
	 * data descriptors and are located in the batch structure.
	 */
	void					*desc_base;
	/* [한국어] batch 내부의 user_desc 영역 전체를 한꺼번에 담은 DMA 메모리 베이스.
	 * 설정자: 채널 init 시 spdk_zmalloc(SPDK_MALLOC_DMA)로 batch_pool 크기 × batch_size 만큼 할당.
	 * 읽는 자: 각 batch->user_desc가 이 영역의 슬라이스를 가리킴.
	 * 값 범위: hugepage DMA 영역.
	 * 동기화: 초기화 후 read-only. */

	STAILQ_HEAD(, idxd_ops)			ops_pool;
	/* [한국어] 사용 가능한(=free) idxd_ops 슬롯의 LIFO 리스트.
	 * 설정자: 채널 init이 N개 채워둠. submit 시 STAILQ_FIRST/REMOVE로 꺼냄, 완료 시 INSERT_HEAD로 반환.
	 * 읽는 자: spdk_idxd_submit_*()이 항상 ops_pool에서 1개를 가져옴.
	 * 값 범위: 0~num_ops 개의 free 슬롯.
	 * 동기화: 채널 단일 스레드 - lockless. */

	/* Current list of outstanding operations to poll. */
	STAILQ_HEAD(op_head, idxd_ops)		ops_outstanding;
	/* [한국어] in-flight(=HW에 제출 후 완료 대기 중) op들의 FIFO 리스트.
	 * 설정자: submit_*()이 INSERT_TAIL로 추가, process_events()가 완료된 head를 REMOVE.
	 * 읽는 자: process_events()가 STAILQ_FIRST부터 순차 검사.
	 * 값 범위: 0..num_ops.
	 * 동기화: 채널 단일 스레드 - lockless. */

	void					*ops_base;
	/* [한국어] 모든 idxd_ops 슬롯이 들어 있는 hugepage DMA 베이스 - completion record가 64-byte
	 * 정렬되어야 하므로 별도 할당.
	 * 설정자: 채널 init이 num_ops × sizeof(idxd_ops) 만큼 할당.
	 * 읽는 자: 채널 destruct 시 한꺼번에 spdk_free.
	 * 값 범위: hugepage DMA 영역.
	 * 동기화: 초기화 후 read-only. */

	TAILQ_HEAD(, idxd_batch)		batch_pool;
	/* [한국어] 사용 가능한 idxd_batch 풀 - 채널당 미리 N개 할당 후 재사용.
	 * 설정자: 채널 init이 INSERT_TAIL로 채움.
	 * 읽는 자: spdk_idxd_batch_create()가 TAILQ_FIRST로 꺼냄.
	 * 값 범위: NUM_BATCHES_PER_CHANNEL 만큼.
	 * 동기화: 채널 단일 스레드 - lockless. */

	void					*batch_base;
	/* [한국어] 모든 batch 구조체 자체를 담은 메모리 베이스 (batch는 호스트 메모리이지 DMA 영역 아님).
	 * 설정자: 채널 init이 calloc 또는 spdk_zmalloc.
	 * 읽는 자: 채널 destruct 시 일괄 해제.
	 * 값 범위: 호스트 메모리.
	 * 동기화: 초기화 후 read-only. */
};

/* [한국어] PCI 디바이스 식별자 쌍 (vendor, device) - probe 시 매칭에 사용.
 * 설정자: idxd_user.c의 정적 ID 테이블에 하드코드 (DSA: 0x8086:0x0b25 등). */
struct pci_dev_id {
	int vendor_id;
	/* [한국어] PCI vendor ID (Intel = 0x8086).
	 * 값 범위: PCI 표준 16비트 vendor ID. */

	int device_id;
	/* [한국어] PCI device ID (DSA Gen1 = 0x0b25 등).
	 * 값 범위: PCI 표준 16비트 device ID. */
};

/*
 * This struct wraps the hardware completion record which is 32 bytes in
 * size and must be 32 byte aligned.
 */
/* [한국어] DSA/IAA 1개 작업당의 SW 메타데이터 + HW completion record 결합 구조.
 * HW가 작업 완료 시 hw/iaa_hw 영역(앞 32B)에 status를 기록 → SW는 cb_fn 호출 후 ops_pool 반환. */
struct idxd_ops {
	union {
		struct dsa_hw_comp_record	hw;
		/* [한국어] DSA용 32B HW completion record - status, fault_addr, bytes_completed 등.
		 * 설정자: HW가 작업 종료 시 DMA로 직접 기록 (busy bit clear → 완료 표시).
		 * 읽는 자: process_events()가 hw.status 비트로 완료/에러 판정.
		 * 값 범위: 32 byte 비트필드. status 필드의 LSB가 1로 바뀌면 완료.
		 * 동기화: HW write, SW polled read. volatile 처리는 spdk/idxd_spec.h 내부에서. */

		struct iaa_hw_comp_record	iaa_hw;
		/* [한국어] IAA용 completion record - DSA보다 추가 필드(output_size 등)가 있음.
		 * 설정자: IAA HW가 작업 종료 시 DMA write.
		 * 읽는 자: IAA 작업의 process_events()가 output_size 등을 가져갈 때 사용.
		 * 값 범위: 32 byte. status 필드 사용은 dsa_hw와 호환.
		 * 동기화: 동일 (HW write, SW read). */
	};

	void				*cb_arg;
	/* [한국어] cb_fn 호출 시 첫 인자로 넘길 사용자 컨텍스트 포인터.
	 * 설정자: spdk_idxd_submit_*()이 사용자 인자를 그대로 저장.
	 * 읽는 자: process_events()가 cb_fn(cb_arg, status) 호출.
	 * 값 범위: 임의 (NULL 허용).
	 * 동기화: 채널 단일 스레드. */

	spdk_idxd_req_cb		cb_fn;
	/* [한국어] 사용자 완료 콜백 - 작업 결과(rc)를 인자로 호출됨.
	 * 설정자: submit_*()이 사용자 함수 포인터 저장.
	 * 읽는 자: process_events()가 완료 시 호출.
	 * 값 범위: NULL(parent op의 child면 NULL) 또는 사용자 함수 포인터.
	 * 동기화: 채널 단일 스레드. */

	struct idxd_batch		*batch;
	/* [한국어] 이 op이 batch의 inner op인 경우 소속 batch 포인터, 아니면 NULL.
	 * 설정자: batch_prep_*() 시 set, 일반 submit은 NULL.
	 * 읽는 자: process_events()가 batch refcnt 감소에 사용.
	 * 값 범위: NULL 또는 유효한 batch 포인터.
	 * 동기화: 채널 단일 스레드. */

	struct idxd_hw_desc		*desc;
	/* [한국어] 이 op이 HW에 제출한 디스크립터의 SW 측 포인터 - 완료 후 reuse 시 참조.
	 * 설정자: submit_*()이 디스크립터 영역 1개를 가리키도록 설정.
	 * 읽는 자: 디버깅/재처리 시 사용.
	 * 값 범위: 유효한 idxd_hw_desc 포인터.
	 * 동기화: 채널 단일 스레드. */

	union {
		uint32_t		*crc_dst;
		/* [한국어] CRC32C 작업의 결과 32-bit CRC 값을 쓸 사용자 메모리 주소.
		 * 설정자: spdk_idxd_submit_crc32c() 등이 사용자 출력 포인터 저장.
		 * 읽는 자: process_events()가 hw.crc 값을 *crc_dst로 복사.
		 * 값 범위: 유효한 호스트 메모리 주소(NULL 불가 - CRC 작업 시).
		 * 동기화: 채널 단일 스레드. */

		uint32_t		*output_size;
		/* [한국어] IAA 압축/해제 작업의 출력 크기를 쓸 사용자 메모리 주소 (또는 unused).
		 * 설정자: IAA submit 함수가 사용자 출력 포인터 저장.
		 * 읽는 자: process_events()가 iaa_hw.output_size를 *output_size로 복사.
		 * 값 범위: 유효한 호스트 메모리 또는 NULL.
		 * 동기화: 채널 단일 스레드. */
	};

	struct idxd_ops			*parent;
	/* [한국어] 이 op이 큰 transfer를 split해서 만들어진 child인 경우, parent op 포인터.
	 * 설정자: split 로직(idxd.c)이 child의 parent에 원본 op 저장.
	 * 읽는 자: process_events()가 child 완료 시 parent.count를 감소, 0이 되면 parent의 cb_fn 호출.
	 * 값 범위: NULL(독립 op) 또는 parent op.
	 * 동기화: 채널 단일 스레드. */

	uint32_t			count;
	/* [한국어] split 시 child 개수 (parent op에서만 사용).
	 * 설정자: split 함수가 child 개수로 set.
	 * 읽는 자: child 완료 콜백이 -- 후 0 도달 시 parent cb_fn 호출.
	 * 값 범위: 1..N. parent op이 아니면 0/미사용.
	 * 동기화: 채널 단일 스레드 - atomic 불필요. */

	STAILQ_ENTRY(idxd_ops)		link;
	/* [한국어] ops_pool 또는 ops_outstanding STAILQ 노드.
	 * 설정자/읽는 자: idxd.c의 STAILQ_INSERT/REMOVE.
	 * 값 범위: STAILQ next 포인터.
	 * 동기화: 채널 단일 스레드. */
};
/* [한국어] 컴파일 타임 단언: idxd_ops 크기는 정확히 128 byte여야 함 (cache line 정렬과 ops_base 슬라이싱 가정). */
SPDK_STATIC_ASSERT(sizeof(struct idxd_ops) == 128, "size mismatch");

/* [한국어] IDXD backend 구현체(impl) 인터페이스 - "user"(VFIO) 와 "kernel"(libaccel-config) 두 종류.
 * idxd_impl_register()로 g_idxd_impls 리스트에 등록되며, idxd_probe()가 순회하여 디바이스 탐색. */
struct spdk_idxd_impl {
	const char *name;
	/* [한국어] 백엔드 이름 ("user" 또는 "kernel") - 사용자 RPC로 어떤 백엔드를 쓸지 선택할 때 비교 키.
	 * 설정자: 정적 g_*_idxd_impl 구조체에 하드코드.
	 * 읽는 자: idxd_attach_*()가 strcmp로 매칭. */

	int (*probe)(void *cb_ctx, spdk_idxd_attach_cb attach_cb,
		     spdk_idxd_probe_cb probe_cb);
	/* [한국어] 디바이스 탐색 콜백 - 시스템에서 사용 가능한 IDXD 디바이스를 enumerate하고 attach_cb를 호출.
	 * 호출 컨텍스트: spdk_idxd_probe() 호출 스레드 (보통 mgmt thread/init 단계).
	 * @cb_ctx: 호출자가 attach_cb로 전달받을 컨텍스트.
	 * @attach_cb: 디바이스 1개 발견 시 호출 - 사용자가 채택 여부 결정.
	 * @probe_cb: 사전 필터링용 콜백 (선택적).
	 * @return: 0=성공, 음수=errno. */

	void (*destruct)(struct spdk_idxd_device *idxd);
	/* [한국어] 디바이스 1개 해제 콜백 - mmap/fd/메모리 정리.
	 * 호출 컨텍스트: 마지막 채널이 닫힌 후 spdk_idxd_detach()가 호출. */

	void (*dump_sw_error)(struct spdk_idxd_device *idxd, void *portal);
	/* [한국어] 디바이스 SW 에러 상태 덤프 콜백 - 디버깅용. user backend는 SWERROR MMIO를 읽어 출력.
	 * 호출 컨텍스트: 채널이 비정상 상태 감지 시 진단 정보 출력. */

	char *(*portal_get_addr)(struct spdk_idxd_device *idxd);
	/* [한국어] 이 디바이스의 portal 가상주소 반환 콜백.
	 * 호출 컨텍스트: 채널 생성 시 portal 주소 결정. */

	STAILQ_ENTRY(spdk_idxd_impl) link;
	/* [한국어] g_idxd_impls 글로벌 리스트 노드 - 모든 등록된 backend impl을 묶음.
	 * 설정자: idxd_impl_register()가 INSERT_TAIL.
	 * 읽는 자: spdk_idxd_probe()가 순회. */
};

/* [한국어] IDXD 디바이스 1개에 대응되는 컨텍스트 - DSA 1개당 1개 인스턴스.
 * impl(user/kernel) 추상화 위에서 SPDK accel framework가 본 구조체만 보고 디바이스를 다룸. */
struct spdk_idxd_device {
	struct spdk_idxd_impl		*impl;
	/* [한국어] 이 디바이스를 다루는 backend 구현체 포인터(g_user_idxd_impl 또는 g_kernel_idxd_impl).
	 * 설정자: backend probe()가 디바이스 발견 시 set.
	 * 읽는 자: destruct/portal_get_addr 등 모든 backend 분기.
	 * 값 범위: 유효한 impl 포인터.
	 * 동기화: 초기화 후 read-only. */

	void				*portal;
	/* [한국어] 이 디바이스의 portal MMIO 매핑 주소 (4 KiB 또는 그 배수, WQ별 분할).
	 * 설정자: backend probe()가 mmap 또는 PCI BAR으로 매핑.
	 * 읽는 자: 채널 생성 시 portal_offset과 결합해 채널별 주소를 부여.
	 * 값 범위: 유효한 mmap 주소.
	 * 동기화: 초기화 후 read-only. */

	uint32_t			socket_id;
	/* [한국어] 디바이스가 위치한 NUMA socket(=노드) ID - 채널을 같은 NUMA의 코어에 할당하기 위함.
	 * 설정자: probe 시 PCI numa_node 또는 accfg_device_get_numa_node로 획득.
	 * 읽는 자: SPDK accel framework가 channel-thread affinity 결정 시 참조.
	 * 값 범위: 0..N(보통 0 또는 1).
	 * 동기화: 초기화 후 read-only. */

	uint32_t			num_channels;
	/* [한국어] 현재 이 디바이스에서 활성화된 io_channel 수.
	 * 설정자: spdk_idxd_get_channel/put_channel이 num_channels_lock 하에 ++/--.
	 * 읽는 자: chan_per_device 한도 체크 시 사용.
	 * 값 범위: 0..chan_per_device.
	 * 동기화: num_channels_lock으로 보호. */

	uint32_t			total_wq_size;
	/* [한국어] 디바이스 WQ의 전체 capacity (디스크립터 개수) - 채널당 분배 시 분모.
	 * 설정자: probe 시 WQCFG/accfg_wq_get_size에서 획득.
	 * 읽는 자: chan_per_device 결정과 in-flight 한도 결정에 사용.
	 * 값 범위: 보통 16~128 (DSA Gen1 기준).
	 * 동기화: 초기화 후 read-only. */

	uint32_t			chan_per_device;
	/* [한국어] 이 디바이스에서 동시에 만들 수 있는 채널 수 - WQ 크기에 따라 4 또는 8.
	 * 설정자: probe 시 total_wq_size 기반으로 결정.
	 * 읽는 자: spdk_idxd_get_channel()이 한도 초과 여부 검사.
	 * 값 범위: 1..IDXD_MAX_QUEUES.
	 * 동기화: 초기화 후 read-only. */

	uint16_t			batch_size;
	/* [한국어] 1 batch에 들어갈 수 있는 inner descriptor의 최대 개수.
	 * 설정자: probe 시 GENCAP의 max batch shift 또는 accfg_wq_get_max_batch_size에서 획득.
	 * 읽는 자: batch 할당 크기 결정.
	 * 값 범위: 보통 16, 32, 64, 128, 256, 512, 1024.
	 * 동기화: 초기화 후 read-only. */

	pthread_mutex_t			num_channels_lock;
	/* [한국어] num_channels 변수 보호용 mutex - 채널 생성/소멸이 다른 스레드에서 호출될 수 있음.
	 * 설정자: spdk_idxd_device_create()가 pthread_mutex_init.
	 * 읽는 자: get_channel/put_channel 진입/종료 시 lock/unlock.
	 * 동기화: PTHREAD_PROCESS_PRIVATE. */

	bool				pasid_enabled;
	/* [한국어] PASID(SVM) 활성 여부 - 채널이 호스트 가상주소를 직접 IOVA로 사용 가능한지.
	 * 설정자: probe 시 디바이스 capability 또는 accfg_device_get_pasid_enabled로 결정.
	 * 읽는 자: 채널 생성 시 io_channel.pasid_enabled로 복사.
	 * 값 범위: true(SVM)/false(IOVA).
	 * 동기화: 초기화 후 read-only. */

	enum idxd_dev			type;
	/* [한국어] 디바이스 종류 - DSA 또는 IAA. opcode 매핑 분기에 사용.
	 * 설정자: probe 시 PCI device_id로 결정.
	 * 읽는 자: submit_*()이 IAA-only opcode 사용 가능 여부 검사.
	 * 값 범위: enum idxd_dev. */

	struct iaa_aecs			*aecs;
	/* [한국어] IAA(분석 가속기) 전용 AECS(Application Execution Context Set) 메모리.
	 * 설정자: IAA 디바이스 init 시 spdk_zmalloc(DMA)로 할당, DSA에서는 NULL.
	 * 읽는 자: IAA 압축/해제 디스크립터의 aecs 필드에 IOVA 전달.
	 * 값 범위: NULL(DSA) 또는 유효한 hugepage DMA 주소.
	 * 동기화: 초기화 후 read-only. */

	uint64_t			aecs_addr;
	/* [한국어] aecs의 IOVA(또는 가상주소 - PASID 사용 시) - 디스크립터에 직접 채워짐.
	 * 설정자: aecs 할당 후 spdk_vtophys 또는 그대로 가상주소.
	 * 읽는 자: IAA submit 함수가 디스크립터 채울 때 사용.
	 * 값 범위: hugepage DMA 주소.
	 * 동기화: 초기화 후 read-only. */

	uint32_t			version;
	/* [한국어] 디바이스 GENCAP/version 레지스터 값 - 디바이스 세대 식별(DSA Gen1 vs Gen2).
	 * 설정자: probe 시 accfg_device_get_version 또는 GENCAP.
	 * 읽는 자: 일부 capability 분기.
	 * 값 범위: 디바이스 의존.
	 * 동기화: 초기화 후 read-only. */
};

/*
 * [한국어]
 * idxd_impl_register - backend impl(user 또는 kernel)을 전역 리스트에 등록
 *
 * @impl: 등록할 spdk_idxd_impl 정적 구조체 포인터.
 * @return: 없음 (void).
 *
 * SPDK_IDXD_IMPL_REGISTER 매크로가 만든 constructor 함수가 main() 이전에 호출되어
 * 모든 backend가 자기 자신을 g_idxd_impls 리스트(idxd.c에 정의)에 추가한다. 이후
 * spdk_idxd_probe()는 이 리스트를 순회하며 각 backend의 probe() 콜백을 차례로 호출한다.
 *
 * 호출 컨텍스트: 프로세스 시작 시 (constructor) - 단일 스레드 가정.
 *
 * 호출 체인:
 *   __attribute__((constructor)) idxd_impl_register_<name> → [idxd_impl_register]
 */
void idxd_impl_register(struct spdk_idxd_impl *impl);

/*
 * [한국어]
 * SPDK_IDXD_IMPL_REGISTER - backend impl을 자동으로 등록하는 매크로
 *
 * @name: backend 이름 식별자 (예: user, kernel) - 함수명 충돌 방지용 토큰.
 * @impl: 등록할 spdk_idxd_impl 구조체 포인터.
 *
 * 동작: __attribute__((constructor)) 속성이 붙은 함수 idxd_impl_register_<name>을 만들어
 *      main() 진입 전에 자동으로 idxd_impl_register(impl)를 호출하게 한다.
 *      C++의 정적 초기화처럼 backend가 자기 자신을 SPDK 코어에 등록하는 패턴이며,
 *      idxd_user.c와 idxd_kernel.c 양쪽 모두 이 매크로로 등록된다.
 *
 * 사용 예: SPDK_IDXD_IMPL_REGISTER(kernel, &g_kernel_idxd_impl);
 */
#define SPDK_IDXD_IMPL_REGISTER(name, impl) \
static void __attribute__((constructor)) idxd_impl_register_##name(void) \
{ \
	idxd_impl_register(impl); \
}

#ifdef __cplusplus
}
#endif

#endif /* __IDXD_H__ */
