/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK vhost 서브시스템의 내부 공용 헤더 (vhost_internal.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK vhost 서브시스템(vhost-user 프로토콜 구현) 내부에서 공유되는
 * 자료구조, 매크로, 함수 프로토타입을 정의한다. vhost-user는 QEMU 등 게스트 가상화
 * 호스트가 자신의 virtqueue를 SPDK target에게 위임하여 SPDK가 직접 폴링하면서
 * I/O를 처리하도록 하는 프로토콜이다. 이 헤더는 vhost.c, vhost_user.c, rte_vhost_user.c,
 * vhost_blk.c, vhost_scsi.c, vhost_rpc.c 등에서 모두 참조된다.
 * virtqueue/세션/디바이스 추상화, virtio descriptor 접근, IRQ/이벤트 시그널링,
 * blk transport 등록까지 vhost 백엔드가 필요로 하는 모든 빌딩 블록을 모은다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK vhost 스택은 다음과 같이 구성된다:
 *   [QEMU/게스트 VM]
 *      ↕ (UNIX 도메인 소켓, vhost-user 프로토콜)
 *   [DPDK rte_vhost 라이브러리]  -- 협상/메모리 매핑/Inflight 처리 담당
 *      ↕ (rte_vhost callback)
 *   [SPDK rte_vhost_user.c]       -- DPDK 콜백 → SPDK thread 메시지 변환
 *      ↕
 *   [SPDK vhost.c (이 헤더)]      -- 디바이스/세션/공통 RPC 인프라
 *      ↕
 *   [vhost_blk.c / vhost_scsi.c]  -- 백엔드별 virtio-blk / virtio-scsi 처리
 *      ↕ (spdk_bdev API)
 *   [bdev 레이어 → bdev 모듈 → 실 디바이스]
 *
 * 이 헤더는 위 그림의 가운데 두 계층(SPDK vhost.c, 백엔드)을 묶는 접착제이다.
 * 호출 컨텍스트는 SPDK reactor thread (디바이스에 할당된 lcore의 spdk_thread)이며,
 * DPDK 내부 pthread에서 진입하는 콜백은 dpdk_sem 세마포어로 SPDK thread와 동기화된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: <rte_vhost.h> (DPDK), <linux/virtio_config.h>, spdk_internal/vhost_user.h,
 *   spdk/bdev.h, spdk/log.h, spdk/util.h, spdk/rpc.h, spdk/tree.h.
 * - 사용처: lib/vhost/*.c, module/vhost/*, test/vhost/*.
 * - 데이터 흐름:
 *     게스트 → virtqueue avail ring → vhost_vq_avail_ring_get()
 *           → vhost_vq_get_desc() / vhost_vq_get_desc_packed() (descriptor 파싱)
 *           → vhost_vring_desc_to_iov() (게스트 GPA → 호스트 VVA 매핑)
 *           → spdk_vhost_blk_task / scsi_task 생성 → spdk_bdev_io 발행
 *           → bdev 완료 콜백 → vhost_vq_used_ring_enqueue()
 *           → vhost_vq_used_signal() (게스트 IRQ 알림)
 * - 공유 자료구조: spdk_vhost_dev (디바이스), spdk_vhost_user_dev (vhost-user 전용 컨텍스트),
 *   spdk_vhost_session (한 게스트와의 연결), spdk_vhost_virtqueue (per-VQ 상태).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_vhost_virtqueue: 한 virtqueue의 split/packed 상태, 통계, 인터럽트.
 * - struct spdk_vhost_session: 한 게스트 연결(VID 단위)의 모든 virtqueue + 메모리 + 상태.
 * - struct spdk_vhost_user_dev: vhost-user 프로토콜 컨텍스트 (lock, 세션 리스트).
 * - struct spdk_vhost_dev: 디바이스 공통(이름/소켓 경로/feature/스레드/RB 노드).
 * - struct spdk_vhost_blk_task: virtio-blk 요청을 bdev I/O로 매핑하는 태스크.
 * - vhost_vq_avail_ring_get / vhost_vq_get_desc(_packed) / vhost_vring_desc_to_iov:
 *   virtqueue에서 요청을 꺼내 호스트 IOV로 변환하는 핵심 헬퍼.
 * - vhost_vq_used_ring_enqueue / vhost_vq_packed_ring_enqueue / vhost_vq_used_signal:
 *   완료된 요청을 used ring에 넣고 게스트에 IRQ를 알리는 헬퍼.
 * - vhost_user_dev_foreach_session: 다중 세션을 각 세션의 lcore에서 순회 실행.
 * - SPDK_VIRTIO_BLK_TRANSPORT_REGISTER: virtio-blk transport 자동 등록 매크로.
 */

#ifndef SPDK_VHOST_INTERNAL_H
#define SPDK_VHOST_INTERNAL_H
/* [한국어] include guard - 이 헤더가 다중 포함되어 자료구조/프로토타입이 중복 정의되는 것을 방지. */

#include <linux/virtio_config.h>
/* [한국어] virtio 표준 정의(VIRTIO_F_VERSION_1, VIRTIO_F_NOTIFY_ON_EMPTY 등 feature bit).
 * Linux UAPI 헤더에서 가져온다 — vhost-user는 virtio 1.0/1.1 스펙을 그대로 따른다. */

#include "spdk/stdinc.h"
/* [한국어] 표준 C 라이브러리(stdint, stdbool, stdio, sys/queue 등) SPDK 통합 인클루드. */

#include <rte_vhost.h>
/* [한국어] DPDK rte_vhost 라이브러리 헤더. struct rte_vhost_vring, rte_vhost_memory,
 * rte_vhost_inflight_desc_packed 등 vhost-user 프로토콜의 공통 자료구조와 콜백 API 정의.
 * SPDK vhost target은 이 라이브러리 위에 백엔드(blk/scsi)를 구현한다. */

#include "spdk_internal/vhost_user.h"
/* [한국어] SPDK 내부 vhost-user 헬퍼(spdk_vhost_fini_cb 등 콜백 타입, vhost.c와의 인터페이스). */
#include "spdk/bdev.h"
/* [한국어] SPDK bdev 레이어 API - 백엔드(blk)에서 게스트 I/O를 spdk_bdev_io로 변환하기 위해 필요. */
#include "spdk/log.h"
/* [한국어] SPDK 로그 매크로 (SPDK_DEBUGLOG, SPDK_ERRLOG 등). */
#include "spdk/util.h"
/* [한국어] SPDK 유틸 매크로 (SPDK_COUNTOF, container_of, SPDK_CACHE_LINE_SIZE 등). */
#include "spdk/rpc.h"
/* [한국어] SPDK JSON-RPC 인프라 - vhost_rpc.c에서 RPC 핸들러 등록. */
#include "spdk/config.h"
/* [한국어] 빌드 타임 컴파일 옵션 매크로 - SPDK_CONFIG_VHOST_INTERNAL_LIB 등 분기에 사용. */
#include "spdk/tree.h"
/* [한국어] BSD sys/tree.h 호환 매크로 - vdev 전역 디렉토리를 RB tree(RB_ENTRY)로 관리한다. */

#define SPDK_VHOST_MAX_VQUEUES	256
/* [한국어] 한 vhost 세션이 보유할 수 있는 virtqueue의 최대 개수.
 * spdk_vhost_session::virtqueue 배열 길이가 된다. virtio-scsi의 경우 control/event/IO[N]
 * 큐가 모두 여기 매핑되므로 256은 충분히 큰 상한값으로 설정. */

#define SPDK_VHOST_MAX_VQ_SIZE	1024
/* [한국어] 한 virtqueue에서 SPDK가 핸들링하는 descriptor 슬롯 최대 수.
 * 게스트가 negotiate한 큐 크기가 이 값을 넘지 않도록 제한 — split ring의 size 필드 검증 등에 사용. */

#define SPDK_VHOST_SCSI_CTRLR_MAX_DEVS 8
/* [한국어] virtio-scsi 컨트롤러 하나가 가질 수 있는 SCSI target(LUN 그룹) 최대 수.
 * vhost_scsi.c의 spdk_scsi_dev 배열 길이로 사용되며, RPC vhost_scsi_controller_add_target에서
 * scsi_target_num의 유효 범위 [0, 7]을 검증한다. */

#define SPDK_VHOST_IOVS_MAX 129
/* [한국어] 한 virtio 요청이 변환된 호스트 측 iovec(struct iovec) 배열 최대 길이.
 * 128(데이터) + 1(상태/헤더) 형태가 일반적 — 큰 SGL 요청을 한 시점에 메모리상 표현하는 한계. */

#define SPDK_VHOST_VQ_MAX_SUBMISSIONS	32
/* [한국어] 한 virtqueue에서 한 번의 폴링 사이클에 처리(submit)하는 요청 최대 수.
 * 이 값을 넘기면 다른 큐/태스크에 양보(yield)하기 위해 폴링을 끊어 fairness를 확보한다. */

/*
 * Rate at which stats are checked for interrupt coalescing.
 */
#define SPDK_VHOST_STATS_CHECK_INTERVAL_MS 10
/* [한국어] coalescing(완료 IRQ 묶음 처리)을 위한 통계 검사 주기(밀리초).
 * 10ms마다 req_cnt 등 카운터를 평가해 임계치 초과 시 coalescing on/off를 결정. */
/*
 * Default threshold at which interrupts start to be coalesced.
 */
#define SPDK_VHOST_VQ_IOPS_COALESCING_THRESHOLD 60000
/* [한국어] coalescing이 시작되는 기본 IOPS 임계치(요청/초).
 * 큐 IOPS가 60K를 넘으면 게스트로의 IRQ를 모아서 보내 게스트 측 컨텍스트 스위칭을 줄인다. */

/*
 * Timeout in seconds for vhost-user session stop message.
 */
#define SPDK_VHOST_SESSION_STOP_TIMEOUT_IN_SEC 3
/* [한국어] 세션 stop 메시지 처리 타임아웃(초). 게스트의 disconnect/destroy 메시지에 대해
 * 진행 중 task가 빠지길 기다리는 최대 시간. 초과하면 강제 정리 경로로 진입. */
/*
 * Stop retry timeout in seconds, this value should be greater than SPDK_VHOST_SESSION_STOP_TIMEOUT_IN_SEC.
 */
#define SPDK_VHOST_SESSION_STOP_RETRY_TIMEOUT_IN_SEC (SPDK_VHOST_SESSION_STOP_TIMEOUT_IN_SEC + 1)
/* [한국어] stop 재시도 전체 타임아웃(초) — 위 값보다 1초 크게 설정해 stop 콜백이 종료된 뒤
 * 한 번 더 정리할 여지를 남긴다. */
/*
 * Stop retry period in microseconds
 */
#define SPDK_VHOST_SESSION_STOP_RETRY_PERIOD_IN_US 1000
/* [한국어] stop poller가 1ms 주기로 동작하며, 매 주기 stop_retry_count를 감소시킨다. */

/*
 * Currently coalescing is not used by default.
 * Setting this to value > 0 here or by RPC will enable coalescing.
 */
#define SPDK_VHOST_COALESCING_DELAY_BASE_US 0
/* [한국어] coalescing 기본 지연(마이크로초). 0이면 coalescing 비활성.
 * RPC vhost_controller_set_coalescing이나 이 매크로로 양수 설정 시 활성화. */

#define SPDK_VHOST_FEATURES ((1ULL << VHOST_F_LOG_ALL) | \
	(1ULL << VHOST_USER_F_PROTOCOL_FEATURES) | \
	(1ULL << VIRTIO_F_VERSION_1) | \
	(1ULL << VIRTIO_F_NOTIFY_ON_EMPTY) | \
	(1ULL << VIRTIO_RING_F_EVENT_IDX) | \
	(1ULL << VIRTIO_RING_F_INDIRECT_DESC) | \
	(1ULL << VIRTIO_F_ANY_LAYOUT))
/* [한국어] SPDK vhost target이 게스트와의 협상에서 광고하는 공통 feature 비트맵.
 *  - VHOST_F_LOG_ALL: live migration용 dirty page 로깅 지원.
 *  - VHOST_USER_F_PROTOCOL_FEATURES: vhost-user 확장 프로토콜 협상 활성.
 *  - VIRTIO_F_VERSION_1: virtio 1.0 모드(LE 강제, modern layout) 강제.
 *  - VIRTIO_F_NOTIFY_ON_EMPTY: 큐가 비었을 때 게스트가 알림을 받도록.
 *  - VIRTIO_RING_F_EVENT_IDX: 인터럽트/notification 줄이기 위한 event idx 사용.
 *  - VIRTIO_RING_F_INDIRECT_DESC: indirect descriptor table 사용 가능.
 *  - VIRTIO_F_ANY_LAYOUT: 헤더/페이로드 layout을 임의로 둘 수 있음. */

#define SPDK_VHOST_DISABLED_FEATURES ((1ULL << VIRTIO_RING_F_EVENT_IDX) | \
	(1ULL << VIRTIO_F_NOTIFY_ON_EMPTY))
/* [한국어] 기본적으로 비활성화하는 feature 집합 — 백엔드별 필요에 따라 활성화 가능.
 * EVENT_IDX/NOTIFY_ON_EMPTY가 활성화되면 코드 경로가 복잡해지므로 안전한 기본값으로 끔. */

#define VRING_DESC_F_AVAIL	(1ULL << VRING_PACKED_DESC_F_AVAIL)
/* [한국어] packed virtqueue descriptor의 AVAIL 플래그 비트(스펙: VRING_PACKED_DESC_F_AVAIL).
 * 게스트가 채워서 호스트에게 넘긴 상태(available)를 표시 — 비트 위치를 한 단어 뎟마스크로 변환. */
#define VRING_DESC_F_USED	(1ULL << VRING_PACKED_DESC_F_USED)
/* [한국어] packed virtqueue descriptor의 USED 플래그 비트.
 * 호스트가 처리 완료해 게스트에게 돌려준 상태를 표시. */
#define VRING_DESC_F_AVAIL_USED	(VRING_DESC_F_AVAIL | VRING_DESC_F_USED)
/* [한국어] AVAIL과 USED 비트를 동시에 마스크할 때 사용 — wrap counter 매칭 검사 등. */

typedef struct rte_vhost_resubmit_desc spdk_vhost_resubmit_desc;
/* [한국어] DPDK의 inflight 재제출 descriptor 타입을 SPDK 네이밍으로 alias.
 * 비정상 종료 후 복구 시 inflight I/O를 다시 제출할 때 사용. */
typedef struct rte_vhost_resubmit_info spdk_vhost_resubmit_info;
/* [한국어] inflight 재제출 정보 컨테이너 alias — 재제출 desc 배열과 카운트를 보유. */
typedef struct rte_vhost_inflight_desc_packed	spdk_vhost_inflight_desc;
/* [한국어] packed ring용 inflight descriptor alias - shared inflight 영역에 기록되는 보조 desc. */

struct spdk_vhost_virtqueue {
	struct rte_vhost_vring vring;
	/* [한국어] DPDK가 노출하는 virtqueue 메타 — desc/avail/used 링 포인터, size, kickfd, callfd 등.
	 * 설정자: rte_vhost_get_vhost_vring()로 채워짐 (세션 시작 시).
	 * 읽는 자: 모든 vhost_vq_*() 헬퍼가 desc/avail/used 접근 시 참조.
	 * 동기화: 한 세션의 한 큐는 항상 단일 SPDK thread에서만 처리(thread affinity)되므로 lock 불필요. */
	struct rte_vhost_ring_inflight vring_inflight;
	/* [한국어] inflight 영역(공유 메모리에 미완료 요청을 기록하여 crash recovery 지원).
	 * 설정자: rte_vhost_get_vring_base() 등을 통해 세션 시작 시 채워짐.
	 * 읽는 자: vhost_inflight_queue_get_desc 가 retry desc 인덱싱에 사용. */
	uint16_t last_avail_idx;
	/* [한국어] 호스트가 처리한 avail ring의 마지막 인덱스(split ring 용).
	 * 설정자: vhost_vq_avail_ring_get()이 새 요청 가져올 때 증가.
	 * 읽는 자: 같은 함수에서 다음 요청의 시작 위치 결정.
	 * 값 범위: 0 ~ vring.size-1 사이를 자유롭게 wrap (free-running counter 모듈로 size). */
	uint16_t last_used_idx;
	/* [한국어] 호스트가 used ring에 마지막으로 쓴 인덱스(split ring 용).
	 * 설정자: vhost_vq_used_ring_enqueue()가 완료 시 +1.
	 * 읽는 자: 같은 함수에서 다음 슬롯 결정. */

	struct {
		/* To mark a descriptor as available in packed ring
		 * Equal to avail_wrap_counter in spec.
		 */
		uint8_t avail_phase	: 1;
		/* [한국어] packed ring의 avail wrap counter (1비트). virtio 1.1 스펙의 avail_wrap_counter.
		 * 설정자: vhost_vq_get_desc_packed()가 큐를 한 바퀴 돌 때마다 토글.
		 * 읽는 자: descriptor.flags의 AVAIL 비트와 비교해 게스트가 새 요청을 넣었는지 판별. */
		/* To mark a descriptor as used in packed ring
		 * Equal to used_wrap_counter in spec.
		 */
		uint8_t used_phase	: 1;
		/* [한국어] packed ring의 used wrap counter. 호스트가 슬롯을 한 바퀴 돌 때마다 토글.
		 * 설정자: vhost_vq_packed_ring_enqueue()가 슬롯을 채울 때마다 갱신.
		 * 읽는 자: 게스트는 desc.flags의 USED 비트와 wrap이 일치하면 자기 슬롯이 처리됐다고 판단. */
		uint8_t padding		: 5;
		/* [한국어] 비트 필드 정렬용 패딩 — 향후 필드 추가 여지 확보. 현재 의미 없음. */
		bool packed_ring	: 1;
		/* [한국어] true면 이 큐가 packed virtqueue, false면 split virtqueue.
		 * 설정자: 세션 시작 시 협상된 feature(VIRTIO_F_RING_PACKED)에 따라 결정.
		 * 읽는 자: 모든 vhost_vq_* 헬퍼가 분기 결정에 사용. */
	} packed;
	/* [한국어] packed ring 관련 비트 모음(공간 절약을 위해 비트필드 구조체).
	 * split ring 큐에서는 packed_ring=false이고 phase 비트는 의미 없음. */

	void *tasks;
	/* [한국어] 큐별 사전 할당된 task 풀 포인터(백엔드별 의미).
	 * blk: spdk_vhost_blk_task 배열, scsi: spdk_vhost_scsi_task 배열 등.
	 * 설정자: backend->alloc_vq_tasks() 콜백 (세션 시작 시).
	 * 읽는 자: 백엔드 폴링 루프가 free task를 인덱스로 빠르게 획득.
	 * 동기화: 큐 단일 thread 처리이므로 lock 불필요. */

	/* Request count from last stats check */
	uint32_t req_cnt;
	/* [한국어] 마지막 통계 검사 이후 누적된 요청 수 — coalescing 의사결정용 모니터링.
	 * 설정자: 폴링 루프에서 요청 수신 시 +=1.
	 * 읽는 자: SPDK_VHOST_STATS_CHECK_INTERVAL_MS 주기 검사 시 IOPS 환산. */

	/* Request count from last event */
	uint16_t used_req_cnt;
	/* [한국어] 마지막 IRQ 이벤트 송신 이후 누적된 완료 수 — coalescing window 카운터.
	 * 설정자: vhost_vq_used_ring_enqueue()/packed_ring_enqueue()에서 +1.
	 * 읽는 자: vhost_vq_used_signal()이 임계치 도달 시 IRQ 발사 후 0으로 리셋. */

	/* How long interrupt is delayed */
	uint32_t irq_delay_time;
	/* [한국어] 인터럽트를 지연시키는 시간(마이크로초) — coalescing 활성 시 사용.
	 * 설정자: set_coalescing 경로에서 delay_base_us를 기반으로 갱신.
	 * 읽는 자: vhost_vq_used_signal()이 next_event_time 계산에 사용. */

	/* Next time when we need to send event */
	uint64_t next_event_time;
	/* [한국어] 다음 IRQ 이벤트를 보낼 절대 시각(spdk_get_ticks 단위).
	 * 설정자: vhost_vq_used_signal()가 IRQ 발사 후 현재시각+irq_delay_time으로 갱신.
	 * 읽는 자: 폴링 루프가 현재 시각이 이를 넘었는지 확인해 IRQ 송신 결정. */

	/* Associated vhost_virtqueue in the virtio device's virtqueue list */
	uint32_t vring_idx;
	/* [한국어] 이 virtqueue가 vsession->virtqueue[] 배열에서 갖는 인덱스(=virtio 큐 번호).
	 * 설정자: 세션 초기화 시 정적으로 부여.
	 * 읽는 자: 로그/디버그/이벤트 페이로드 등에서 큐를 식별. */

	struct spdk_vhost_session *vsession;
	/* [한국어] 이 큐가 속한 vhost 세션의 역참조 포인터.
	 * 설정자: 세션 초기화 시 vsession 주소를 기록.
	 * 읽는 자: 콜백/이벤트 처리에서 큐만 받아도 세션을 복원. */

	struct spdk_interrupt *intr;
	/* [한국어] 큐 kickfd에 등록된 SPDK interrupt source(인터럽트 모드일 때만 사용).
	 * 설정자: vhost_user_session_set_interrupt_mode(true)에서 등록.
	 * 읽는 자: 게스트가 kickfd를 트리거하면 콜백이 호출되어 폴링 함수 진입. */
} __attribute((aligned(SPDK_CACHE_LINE_SIZE)));
/* [한국어] 캐시 라인 정렬 — false sharing 방지(서로 다른 큐가 같은 캐시라인에 놓이지 않도록).
 * SPDK_CACHE_LINE_SIZE는 보통 64B (x86) — 한 큐당 별도 캐시라인. */

struct spdk_vhost_session {
	struct spdk_vhost_dev *vdev;
	/* [한국어] 이 세션이 속한 vhost 디바이스(컨트롤러) 포인터.
	 * 설정자: 세션 생성 시 connect 콜백에서 설정.
	 * 읽는 자: 거의 모든 세션 핸들러가 백엔드 정보를 얻기 위해 사용. */

	/* rte_vhost connection ID. */
	int vid;
	/* [한국어] DPDK rte_vhost 라이브러리가 부여하는 연결 식별자.
	 * 설정자: rte_vhost의 new_connection 콜백에서 전달.
	 * 읽는 자: rte_vhost_* API 호출 시 인자로 사용. -1이면 미연결. */

	/* Unique session ID. */
	uint64_t id;
	/* [한국어] SPDK가 자체적으로 부여하는 세션 ID — vsessions_num 증가시킨 값.
	 * 디버깅/로그에서 세션을 고유하게 식별. */
	/* Unique session name. */
	char *name;
	/* [한국어] 세션 이름 문자열(보통 "ctrlr.s%d" 형식, sprintf로 생성).
	 * 설정자: 세션 alloc 시 할당. 해제: 세션 free 시 free(). */

	bool started;
	/* [한국어] 세션이 시작 완료되어 I/O 처리 중이면 true.
	 * 설정자: backend->start_session() 성공 후 true.
	 * 읽는 자: I/O 폴러/RPC 핸들러가 세션 활성 여부 확인. */
	bool starting;
	/* [한국어] 시작 중인 과도기 상태 — start_session 콜백 진입 후 완료 전까지 true.
	 * 설정자: backend->start_session() 진입 시. 동시에 start/stop이 들어오는 레이스 방지. */
	bool needs_restart;
	/* [한국어] 큐 수가 바뀌어 재시작이 필요한 상태 표시.
	 * 설정자: 게스트가 max_queues를 변경 요청 시. 읽는 자: stop/start 시퀀스에서 분기. */

	struct rte_vhost_memory *mem;
	/* [한국어] 게스트의 메모리 영역 표(GPA→VVA 매핑) — DPDK가 관리하는 불투명 구조체.
	 * 설정자: rte_vhost_get_mem_table()로 획득.
	 * 읽는 자: vhost_gpa_to_vva()가 게스트 주소 변환 시 참조. */

	int task_cnt;
	/* [한국어] 현재 처리 중인 비동기 task 수 — 0이 되어야 stop 완료.
	 * 설정자: 백엔드가 task 발급/완료 시 ±1.
	 * 읽는 자: stop poller가 0인지 검사해 cleanup 진행. */

	uint16_t max_queues;
	/* [한국어] 현재 세션이 사용하는 virtqueue 수.
	 * 설정자: 게스트와 협상한 값으로 세션 시작 시 결정.
	 * 읽는 자: 폴링 루프가 [0..max_queues) 범위를 순회. */
	/* Maximum number of queues before restart, used with 'needs_restart' flag */
	uint16_t original_max_queues;
	/* [한국어] needs_restart=true일 때 이전 max_queues를 보존해 두는 임시 변수.
	 * 재시작 시퀀스에서 큐 풀을 정확히 복원하는 데 사용. */

	uint64_t negotiated_features;
	/* [한국어] 게스트와 합의한 virtio feature 비트맵.
	 * 설정자: feature 협상 콜백에서 결정.
	 * 읽는 자: vhost_dev_has_feature() 헬퍼가 비트 검사. */

	/* Local copy of device coalescing settings. */
	uint32_t coalescing_delay_time_base;
	/* [한국어] 디바이스의 coalescing delay(us) 사본 — 큐별 irq_delay_time 계산 베이스.
	 * 설정자: vhost_user_session_set_coalescing()에서 갱신. */
	uint32_t coalescing_io_rate_threshold;
	/* [한국어] coalescing이 켜지는 IOPS 임계 사본.
	 * 설정자: 위와 동일. 읽는 자: 통계 검사 시 비교. */

	/* Next time when stats for event coalescing will be checked. */
	uint64_t next_stats_check_time;
	/* [한국어] 다음 통계 검사 절대 시각.
	 * 설정자: 통계 검사가 끝날 때 +stats_check_interval로 갱신.
	 * 읽는 자: 폴링 루프가 현재 시각과 비교. */

	/* Interval used for event coalescing checking. */
	uint64_t stats_check_interval;
	/* [한국어] 통계 검사 간격(틱). SPDK_VHOST_STATS_CHECK_INTERVAL_MS를 ticks로 변환한 값. */

	/* Session's stop poller will only try limited times to destroy the session. */
	uint32_t stop_retry_count;
	/* [한국어] stop poller가 task_cnt가 0이 되길 기다리며 시도할 남은 횟수.
	 * 설정자: stop 시작 시 재시도 횟수로 초기화 후 매 주기 -1.
	 * 0 도달 시 강제 종료 경로 진입. */

	/**
	 * DPDK calls our callbacks synchronously but the work those callbacks
	 * perform needs to be async. Luckily, all DPDK callbacks are called on
	 * a DPDK-internal pthread and only related to the current session, so we'll
	 * just wait on a semaphore of this session in there.
	 */
	sem_t dpdk_sem;
	/* [한국어] DPDK pthread와 SPDK thread 간 동기화 세마포어.
	 * DPDK는 vhost-user 메시지를 자체 pthread에서 동기 콜백으로 호출하지만
	 * SPDK는 reactor thread로 작업을 옮긴 뒤 비동기로 처리한다.
	 * DPDK pthread는 메시지 전달 후 이 세마포어 wait, SPDK는 처리 끝나면 post.
	 * 한 세션 단위 1개 — 동시에 한 콜백만 진행됨. */

	/** Return code for the current DPDK callback */
	int dpdk_response;
	/* [한국어] SPDK가 DPDK 콜백 결과를 기록하는 반환 코드 (0/음수 errno).
	 * 설정자: SPDK reactor가 콜백 처리 완료 시점에 작성.
	 * 읽는 자: DPDK pthread가 sem_wait 깨어난 후 이 값을 DPDK에 반환. */

	struct spdk_vhost_virtqueue virtqueue[SPDK_VHOST_MAX_VQUEUES];
	/* [한국어] 이 세션의 모든 virtqueue 슬롯 배열. 인덱스 i는 virtio queue i에 대응.
	 * 활성 큐는 [0..max_queues) 범위만 — 나머지는 사용되지 않음.
	 * 설정자: 세션 시작 시 큐 메타가 채워짐. */

	TAILQ_ENTRY(spdk_vhost_session) tailq;
	/* [한국어] 디바이스의 vsessions 리스트(spdk_vhost_user_dev::vsessions)에 연결되는 링크. */
};

struct spdk_vhost_user_dev {
	struct spdk_vhost_dev *vdev;
	/* [한국어] 부모 spdk_vhost_dev 포인터(역참조). spdk_vhost_dev::ctxt가 이 구조체를 가리킨다.
	 * to_user_dev() 헬퍼로 vdev → user_dev 변환. */

	const struct spdk_vhost_user_dev_backend *user_backend;
	/* [한국어] vhost-user 단계에서 호출되는 백엔드 콜백 모음.
	 * blk/scsi마다 다른 start_session/stop_session/alloc_vq_tasks 등을 보유. */

	/* Saved original values used to setup coalescing to avoid integer
	 * rounding issues during save/load config.
	 */
	uint32_t coalescing_delay_us;
	/* [한국어] RPC가 설정한 coalescing 지연(us) 원본 값(반올림 손실 방지용).
	 * 설정자: vhost_user_dev_set_coalescing(). 읽는 자: get_coalescing 응답. */
	uint32_t coalescing_iops_threshold;
	/* [한국어] RPC가 설정한 IOPS 임계 원본 값. */

	bool registered;
	/* [한국어] 이 디바이스의 UNIX 소켓이 rte_vhost에 등록 완료됐는지 표시.
	 * 설정자: vhost_register_unix_socket() 성공 후 true. */

	/* Use this lock to protect multiple sessions. */
	pthread_mutex_t lock;
	/* [한국어] 같은 디바이스의 여러 세션 리스트(vsessions)를 보호하는 락.
	 * 다중 게스트가 동시에 connect/disconnect할 때 리스트 일관성 유지.
	 * 동기화: 일반 I/O 경로(폴링/완료)는 single thread라 락 불필요, 등록/해제 경로만 잠금. */

	/* Current connections to the device */
	TAILQ_HEAD(, spdk_vhost_session) vsessions;
	/* [한국어] 이 디바이스에 연결된 모든 활성 세션 리스트(헤드).
	 * 보호: lock. */

	/* Increment-only session counter */
	uint64_t vsessions_num;
	/* [한국어] 누적 세션 카운터(증가만, 감소 없음) — 새 세션 ID 부여 시 사용. */

	/* Number of pending asynchronous operations */
	uint32_t pending_async_op_num;
	/* [한국어] 진행 중인 비동기 작업 수 — 디바이스 unregister 시 0이 되어야 안전.
	 * 설정자: 비동기 작업 시작/종료 시 ±1. */
};

struct spdk_vhost_dev {
	char *name;
	/* [한국어] 디바이스(컨트롤러) 이름 — RPC 인자나 spdk_vhost_dev_find()의 키.
	 * 설정자: vhost_dev_register()에서 strdup. 해제: unregister에서 free. */
	char *path;
	/* [한국어] vhost-user UNIX 소켓 경로(보통 /var/tmp/<ctrlr>).
	 * 게스트(QEMU)는 이 경로에 connect하여 vhost-user 협상을 시작한다. */

	bool use_default_cpumask;
	/* [한국어] true면 RPC에서 cpumask가 명시되지 않아 기본값(앱 reactor mask)을 사용 중.
	 * 설정자: vhost_dev_register()에서 cpumask 인자가 NULL일 때 true. */
	struct spdk_thread *thread;
	/* [한국어] 이 디바이스의 모든 폴링/관리 작업이 실행되는 SPDK thread.
	 * cpumask로부터 선택된 lcore 위에 만들어지며 — 디바이스 이름과 1:1.
	 * 다른 thread에서 디바이스에 접근하려면 spdk_thread_send_msg로 메시지 전달 필요. */

	uint64_t virtio_features;
	/* [한국어] 이 디바이스가 광고할 virtio feature 비트맵(SPDK_VHOST_FEATURES + 백엔드 추가).
	 * 게스트와 협상의 시작점. */
	uint64_t disabled_features;
	/* [한국어] 협상에서 제외할 feature 비트맵(SPDK_VHOST_DISABLED_FEATURES + 백엔드 추가). */
	uint64_t protocol_features;
	/* [한국어] vhost-user 확장 프로토콜 feature 비트맵(VHOST_USER_PROTOCOL_F_*). */

	const struct spdk_vhost_dev_backend *backend;
	/* [한국어] 백엔드(blk/scsi) 콜백 모음 — get_config/dump_info_json 등.
	 * 설정자: vhost_dev_register()에서 백엔드 시작 시 부여. */

	/* Context passed from transport */
	void *ctxt;
	/* [한국어] transport(vhost-user)별 컨텍스트 포인터.
	 * vhost-user의 경우 spdk_vhost_user_dev*를 가리킨다 — to_user_dev() 헬퍼로 캐스팅. */

	RB_ENTRY(spdk_vhost_dev) node;
	/* [한국어] 전역 vhost 디바이스 RB tree(name 기준)에 연결되는 노드. */
};

/*
 * [한국어]
 * to_user_dev - spdk_vhost_dev → spdk_vhost_user_dev 변환 헬퍼.
 *
 * @vdev: 디바이스 공통 구조체 (ctxt가 spdk_vhost_user_dev*를 가리킴).
 * @return: vhost-user 컨텍스트 포인터.
 *
 * vhost-user transport는 vdev->ctxt에 spdk_vhost_user_dev를 저장한다.
 * 이 헬퍼는 그 변환을 한 줄로 처리하면서 NULL 가드도 수행한다.
 * 호출 컨텍스트: 디바이스의 thread (다른 thread에서 호출 시 안전성은 호출자 책임).
 *
 * 호출 체인:
 *   다양한 vhost-user 핸들러 → to_user_dev() → spdk_vhost_user_dev* 사용
 */
static inline struct spdk_vhost_user_dev *
to_user_dev(struct spdk_vhost_dev *vdev)
{
	assert(vdev != NULL);
	/* [한국어] vdev가 NULL이면 즉시 abort — 잘못된 호출 조기 발견. */
	return vdev->ctxt;
	/* [한국어] ctxt에 저장된 spdk_vhost_user_dev*를 반환 (vhost-user 등록 시 채워짐). */
}

/**
 * \param vdev vhost device.
 * \param vsession vhost session.
 * \param arg user-provided parameter.
 *
 * \return negative values will break the foreach call, meaning
 * the function won't be called again. Return codes zero and
 * positive don't have any effect.
 */
typedef int (*spdk_vhost_session_fn)(struct spdk_vhost_dev *vdev,
				     struct spdk_vhost_session *vsession,
				     void *arg);
/* [한국어] vhost_user_dev_foreach_session()이 각 세션의 lcore에서 호출하는 콜백 타입.
 * 음수 반환 시 순회 중단 — 에러 전파에 사용. */

/**
 * \param vdev vhost device.
 * \param arg user-provided parameter.
 */
typedef void (*spdk_vhost_dev_fn)(struct spdk_vhost_dev *vdev, void *arg);
/* [한국어] foreach_session 종료 후 vhost 관리 thread에서 호출되는 완료 콜백 타입. */

struct spdk_vhost_user_dev_backend {
	/**
	 * Size of additional per-session context data
	 * allocated whenever a new client connects.
	 */
	size_t session_ctx_size;
	/* [한국어] 세션 생성 시 vsession 뒤에 추가로 할당할 백엔드 컨텍스트 크기.
	 * 백엔드는 컨테이너 패턴(struct backend_session { spdk_vhost_session base; ... })으로 사용. */

	spdk_vhost_session_fn start_session;
	/* [한국어] 세션 시작 콜백 — 큐 task 풀 alloc, 백엔드 자원 셋업.
	 * 호출 컨텍스트: 디바이스 thread. */
	spdk_vhost_session_fn stop_session;
	/* [한국어] 세션 종료 콜백 — 진행 중 task drain 및 자원 해제.
	 * 호출 컨텍스트: 디바이스 thread. */
	int (*alloc_vq_tasks)(struct spdk_vhost_session *vsession, uint16_t qid);
	/* [한국어] 큐별 task 배열 할당 콜백 — virtqueue::tasks를 백엔드 task 풀로 채운다.
	 * 호출 시점: 큐가 enable 될 때. */
	int (*enable_vq)(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *vq);
	/* [한국어] 큐 활성화 콜백 — 게스트가 VHOST_USER_SET_VRING_ENABLE를 보낼 때.
	 * 폴러 등록/Interrupt 모드 설정 등 큐별 셋업을 수행. */
};

enum vhost_backend_type {
	VHOST_BACKEND_BLK = 0,
	/* [한국어] virtio-blk 백엔드(단순 블록 디바이스 1개 노출). */
	VHOST_BACKEND_SCSI,
	/* [한국어] virtio-scsi 백엔드(여러 LUN 지원, SCSI 명령 처리). */
};

struct spdk_vhost_dev_backend {
	enum vhost_backend_type type;
	/* [한국어] 백엔드 타입 — blk/scsi 분기에 사용 (예: vhost.c의 generic 핸들링). */

	int (*vhost_get_config)(struct spdk_vhost_dev *vdev, uint8_t *config, uint32_t len);
	/* [한국어] virtio device-specific config 영역 read 콜백 (게스트가 PCI cfg space를 read 시).
	 * blk: capacity/blk_size, scsi: num_queues/sense_size 등 반환. */
	int (*vhost_set_config)(struct spdk_vhost_dev *vdev, uint8_t *config,
				uint32_t offset, uint32_t size, uint32_t flags);
	/* [한국어] virtio config 영역 write 콜백 — 보통 read-only이므로 거의 사용 안 됨. */

	void (*dump_info_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
	/* [한국어] vhost_get_controllers RPC 응답 시 backend_specific 필드를 채우는 콜백.
	 * blk면 bdev 이름/transport, scsi면 LUN 리스트 등을 출력. */
	void (*write_config_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
	/* [한국어] save_config(JSON 덤프) 시 호출 — 컨트롤러 재구성에 필요한 RPC 호출 시퀀스 출력. */
	int (*remove_device)(struct spdk_vhost_dev *vdev);
	/* [한국어] 컨트롤러 삭제 콜백 — bdev close, 자원 해제 등 백엔드별 cleanup. */
	int (*set_coalescing)(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			      uint32_t iops_threshold);
	/* [한국어] coalescing 파라미터 설정 콜백. */
	void (*get_coalescing)(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			       uint32_t *iops_threshold);
	/* [한국어] coalescing 파라미터 조회 콜백. */
};

void *vhost_gpa_to_vva(struct spdk_vhost_session *vsession, uint64_t addr, uint64_t len);
/* [한국어] 게스트 물리 주소(GPA)를 호스트 가상 주소(VVA)로 변환.
 * vsession->mem 매핑 표(메모리 영역 리스트)를 검색해 변환 — vhost-user의 핵심 헬퍼.
 * len 만큼이 단일 매핑 영역에 들어가야 NULL 반환을 피한다. */

uint16_t vhost_vq_avail_ring_get(struct spdk_vhost_virtqueue *vq, uint16_t *reqs,
				 uint16_t reqs_len);
/* [한국어] split ring의 avail ring에서 사용 가능한 요청 인덱스들을 한 번에 가져온다.
 * 반환값: 채워진 요청 수(<= reqs_len). reqs[i]는 desc table의 head 인덱스. */

/**
 * Get a virtio split descriptor at given index in given virtqueue.
 * The descriptor will provide access to the entire descriptor
 * chain. The subsequent descriptors are accessible via
 * \c spdk_vhost_vring_desc_get_next.
 * \param vsession vhost session
 * \param vq virtqueue
 * \param req_idx descriptor index
 * \param desc pointer to be set to the descriptor
 * \param desc_table descriptor table to be used with
 * \c spdk_vhost_vring_desc_get_next. This might be either
 * default virtqueue descriptor table or per-chain indirect
 * table.
 * \param desc_table_size size of the *desc_table*
 * \return 0 on success, -1 if given index is invalid.
 * If -1 is returned, the content of params is undefined.
 */
int vhost_vq_get_desc(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *vq,
		      uint16_t req_idx, struct vring_desc **desc, struct vring_desc **desc_table,
		      uint32_t *desc_table_size);
/* [한국어] split ring에서 req_idx 위치의 descriptor를 획득. INDIRECT 플래그가 있으면
 * desc_table을 게스트 indirect table로, 없으면 기본 vring desc로 설정. */

/**
 * Get a virtio packed descriptor at given index in given virtqueue.
 * The descriptor will provide access to the entire descriptor
 * chain. The subsequent descriptors are accessible via
 * \c vhost_vring_packed_desc_get_next.
 * \param vsession vhost session
 * \param vq virtqueue
 * \param req_idx descriptor index
 * \param desc pointer to be set to the descriptor
 * \param desc_table descriptor table to be used with
 * \c spdk_vhost_vring_desc_get_next. This might be either
 * \c NULL or per-chain indirect table.
 * \param desc_table_size size of the *desc_table*
 * \return 0 on success, -1 if given index is invalid.
 * If -1 is returned, the content of params is undefined.
 */
int vhost_vq_get_desc_packed(struct spdk_vhost_session *vsession,
			     struct spdk_vhost_virtqueue *virtqueue,
			     uint16_t req_idx, struct vring_packed_desc **desc,
			     struct vring_packed_desc **desc_table, uint32_t *desc_table_size);
/* [한국어] packed ring 버전. avail/used wrap counter 매칭 후 desc 획득. */

int vhost_inflight_queue_get_desc(struct spdk_vhost_session *vsession,
				  spdk_vhost_inflight_desc *desc_array,
				  uint16_t req_idx, spdk_vhost_inflight_desc **desc,
				  struct vring_packed_desc  **desc_table, uint32_t *desc_table_size);
/* [한국어] inflight 영역의 desc를 가져온다 — 비정상 종료 후 재기동 시 미완료 요청 복구용. */

/**
 * Send IRQ/call client (if pending) for \c vq.
 * \param vsession vhost session
 * \param vq virtqueue
 * \return
 *   0 - if no interrupt was signalled
 *   1 - if interrupt was signalled
 */
int vhost_vq_used_signal(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *vq);
/* [한국어] used 영역에 변경이 있을 경우 게스트의 callfd에 eventfd write 발사 → IRQ 트리거. */

/**
 * Send IRQs for the queue that need to be signaled.
 * \param vq virtqueue
 */
void vhost_session_vq_used_signal(struct spdk_vhost_virtqueue *virtqueue);
/* [한국어] vhost_vq_used_signal의 wrapper (vsession을 vq->vsession에서 추출). */

void vhost_vq_used_ring_enqueue(struct spdk_vhost_session *vsession,
				struct spdk_vhost_virtqueue *vq,
				uint16_t id, uint32_t len);
/* [한국어] split ring의 used ring에 (id, len) 쌍을 enqueue — 게스트가 완료된 요청을 회수. */

/**
 * Enqueue the entry to the used ring when device complete the request.
 * \param vsession vhost session
 * \param vq virtqueue
 * \req_idx descriptor index. It's the first index of this descriptor chain.
 * \num_descs descriptor count. It's the count of the number of buffers in the chain.
 * \buffer_id descriptor buffer ID.
 * \length device write length. Specify the length of the buffer that has been initialized
 * (written to) by the device
 * \inflight_head the head idx of this IO inflight desc chain.
 */
void vhost_vq_packed_ring_enqueue(struct spdk_vhost_session *vsession,
				  struct spdk_vhost_virtqueue *virtqueue,
				  uint16_t num_descs, uint16_t buffer_id,
				  uint32_t length, uint16_t inflight_head);
/* [한국어] packed ring 버전 enqueue — used flag와 wrap counter 갱신, inflight 영역 정리. */

/**
 * Get subsequent descriptor from given table.
 * \param desc current descriptor, will be set to the
 * next descriptor (NULL in case this is the last
 * descriptor in the chain or the next desc is invalid)
 * \param desc_table descriptor table
 * \param desc_table_size size of the *desc_table*
 * \return 0 on success, -1 if given index is invalid
 * The *desc* param will be set regardless of the
 * return value.
 */
int vhost_vring_desc_get_next(struct vring_desc **desc,
			      struct vring_desc *desc_table, uint32_t desc_table_size);
/* [한국어] split ring descriptor 체인의 다음 desc로 이동(VRING_DESC_F_NEXT 따라). */

/*
 * [한국어]
 * vhost_vring_desc_is_wr - 이 descriptor가 device-write(=호스트가 게스트로 데이터 회신) 여부 판별.
 *
 * @cur_desc: 검사할 split ring descriptor.
 * @return: VRING_DESC_F_WRITE 비트가 켜져 있으면 true.
 *
 * virtio descriptor의 flags 중 VRING_DESC_F_WRITE는 "device가 이 버퍼에 쓸 권리"를 의미.
 * read I/O라면 read 응답 페이로드 버퍼/상태 바이트가 write 가능하고, write I/O라면 상태 바이트만 write.
 *
 * 호출 체인:
 *   백엔드 process_request → vhost_vring_desc_is_wr → IOV 권한 분기
 */
static inline bool
vhost_vring_desc_is_wr(struct vring_desc *cur_desc)
{
	return !!(cur_desc->flags & VRING_DESC_F_WRITE);
	/* [한국어] flags에 WRITE 비트가 있는지 boolean으로 정규화. */
}

int vhost_vring_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
			    uint16_t *iov_index, const struct vring_desc *desc);
/* [한국어] split desc 1개를 호스트 iovec 배열로 변환(GPA → VVA + len). */

bool vhost_vq_packed_ring_is_avail(struct spdk_vhost_virtqueue *virtqueue);
/* [한국어] packed ring에서 last_avail_idx 위치 desc가 게스트가 채운 새 요청인지 검사. */

/**
 * Get subsequent descriptor from vq or desc table.
 * \param desc current descriptor, will be set to the
 * next descriptor (NULL in case this is the last
 * descriptor in the chain or the next desc is invalid)
 * \req_idx index of current desc, will be set to the next
 * index. If desc_table != NULL the req_idx is the the vring index
 * or the req_idx is the desc_table index.
 * \param desc_table descriptor table
 * \param desc_table_size size of the *desc_table*
 * \return 0 on success, -1 if given index is invalid
 * The *desc* param will be set regardless of the
 * return value.
 */
int vhost_vring_packed_desc_get_next(struct vring_packed_desc **desc, uint16_t *req_idx,
				     struct spdk_vhost_virtqueue *vq,
				     struct vring_packed_desc *desc_table,
				     uint32_t desc_table_size);
/* [한국어] packed ring desc 체인의 다음 desc로 이동(VRING_DESC_F_NEXT + indirect 처리). */

bool vhost_vring_packed_desc_is_wr(struct vring_packed_desc *cur_desc);
/* [한국어] packed ring desc의 device-write 여부. */

int vhost_vring_packed_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
				   uint16_t *iov_index, const struct vring_packed_desc *desc);
/* [한국어] packed desc → iovec 변환. */

bool vhost_vring_inflight_desc_is_wr(spdk_vhost_inflight_desc *cur_desc);
/* [한국어] inflight desc의 device-write 여부. */

int vhost_vring_inflight_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
				     uint16_t *iov_index, const spdk_vhost_inflight_desc *desc);
/* [한국어] inflight desc → iovec 변환 (재제출 시 사용). */

uint16_t vhost_vring_packed_desc_get_buffer_id(struct spdk_vhost_virtqueue *vq, uint16_t req_idx,
		uint16_t *num_descs);
/* [한국어] packed desc 체인의 buffer_id와 chain 길이(num_descs)를 추출. */

/*
 * [한국어]
 * vhost_dev_has_feature - 이 세션이 협상한 feature에 특정 비트가 켜져 있는지 검사.
 *
 * @vsession: 검사 대상 세션.
 * @feature_id: virtio feature 비트 번호 (예: VIRTIO_F_VERSION_1).
 * @return: 비트가 켜져 있으면 true.
 *
 * negotiated_features 비트맵을 1ULL << feature_id로 마스킹해 검사.
 * always_inline 어트리뷰트로 코드 내 곳곳의 feature 분기에서 함수 호출 비용 제거.
 *
 * 호출 체인:
 *   세션 핸들러 → vhost_dev_has_feature → 분기
 */
static inline bool
__attribute__((always_inline))
vhost_dev_has_feature(struct spdk_vhost_session *vsession, unsigned feature_id)
{
	return vsession->negotiated_features & (1ULL << feature_id);
	/* [한국어] 비트 마스킹 — 0이 아니면 true(c++ bool 변환 규칙). */
}

int vhost_scsi_controller_start(const char *name);
/* [한국어] vhost_create_scsi_controller가 delay=true로 만들어 두었던 컨트롤러를 시작 — RPC vhost_start_scsi_controller에서 호출. */

int vhost_dev_register(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
		       const struct spdk_json_val *params, const struct spdk_vhost_dev_backend *backend,
		       const struct spdk_vhost_user_dev_backend *user_backend, bool delay);
/* [한국어] vhost 디바이스 등록 — 이름/cpumask/backend 인자를 받아 vdev 초기화 + 소켓 등록.
 * delay=true면 소켓 listen만 안 한 채 보관 (start_controller에서 활성화). */

int vhost_dev_unregister(struct spdk_vhost_dev *vdev);
/* [한국어] vhost 디바이스 등록 해제 — 소켓 close, RB tree 제거, 자원 해제. */

void vhost_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
/* [한국어] backend->dump_info_json 디스패처. */

/*
 * Set vhost session to run in interrupt or poll mode
 */
void vhost_user_session_set_interrupt_mode(struct spdk_vhost_session *vsession,
		bool interrupt_mode);
/* [한국어] 세션 모드 전환 — interrupt_mode=true면 kickfd interrupt 등록(폴링 안함),
 * false면 polling 모드(주기적 큐 검사). 전력/지연 트레이드오프 조절. */

/*
 * Memory registration functions used in start/stop device callbacks
 */
void vhost_session_mem_register(struct rte_vhost_memory *mem);
/* [한국어] 게스트 메모리 영역들을 SPDK env(DPDK)에 DMA 가능 영역으로 등록. */
void vhost_session_mem_unregister(struct rte_vhost_memory *mem);
/* [한국어] 위에서 등록한 메모리 영역을 해제 — 세션 종료 시 호출. */

/*
 * Call a function for each session of the provided vhost device.
 * The function will be called one-by-one on each session's thread.
 *
 * \param vdev vhost device
 * \param fn function to call on each session's thread
 * \param cpl_fn function to be called at the end of the iteration on
 * the vhost management thread.
 * Optional, can be NULL.
 * \param arg additional argument to the both callbacks
 */
void vhost_user_dev_foreach_session(struct spdk_vhost_dev *dev,
				    spdk_vhost_session_fn fn,
				    spdk_vhost_dev_fn cpl_fn,
				    void *arg);
/* [한국어] 디바이스의 모든 세션을 순회하며 fn 콜백을 각 세션의 lcore에서 실행.
 * 모든 세션 처리 후 cpl_fn을 vhost 관리 thread에서 한 번 더 실행. cross-thread 메시지 활용. */

/**
 * Finish a blocking vhost_user_wait_for_session_stop() call and finally
 * stop the session. This must be called on the session's lcore which
 * used to receive all session-related messages (e.g. from
 * vhost_user_dev_foreach_session()). After this call, the session-
 * related messages will be once again processed by any arbitrary thread.
 *
 * Must be called under the vhost user device's session access lock.
 *
 * \param vsession vhost session
 * \param response return code
 */
void vhost_user_session_stop_done(struct spdk_vhost_session *vsession, int response);
/* [한국어] stop 완료 신호 — DPDK pthread가 sem_wait 중인 응답 코드를 깨운다. */

struct spdk_vhost_session *vhost_session_find_by_vid(int vid);
/* [한국어] vid로 세션 검색 — DPDK 콜백에서 vid만 받았을 때 사용. */
void vhost_session_install_rte_compat_hooks(struct spdk_vhost_session *vsession);
/* [한국어] DPDK 버전 호환을 위한 hook 등록 — rte_vhost API 차이 완화. */
int vhost_register_unix_socket(const char *path, const char *ctrl_name,
			       uint64_t virtio_features, uint64_t disabled_features, uint64_t protocol_features);
/* [한국어] UNIX 소켓을 rte_vhost에 등록 — 디바이스를 vhost-user listener로 만든다. */
int vhost_driver_unregister(const char *path);
/* [한국어] UNIX 소켓을 rte_vhost에서 등록 해제. */
int vhost_get_mem_table(int vid, struct rte_vhost_memory **mem);
/* [한국어] vid의 게스트 메모리 영역 표 가져오기 — rte_vhost wrapper. */
int vhost_get_negotiated_features(int vid, uint64_t *negotiated_features);
/* [한국어] vid의 협상된 feature 비트맵 가져오기. */

int remove_vhost_controller(struct spdk_vhost_dev *vdev);
/* [한국어] 컨트롤러 삭제 통합 진입점 — backend->remove_device 호출 + unregister. */

struct spdk_io_channel *vhost_blk_get_io_channel(struct spdk_vhost_dev *vdev);
/* [한국어] vhost-blk이 사용 중인 bdev I/O channel 획득 (디버깅/공유용). */
void vhost_blk_put_io_channel(struct spdk_io_channel *ch);
/* [한국어] 위에서 가져온 I/O channel 반환. */

/* The spdk_bdev pointer should only be used to retrieve
 * the device properties, ex. number of blocks or I/O type supported. */
struct spdk_bdev *vhost_blk_get_bdev(struct spdk_vhost_dev *vdev);
/* [한국어] vhost-blk 컨트롤러가 노출하는 bdev 포인터 반환 (read-only 속성 조회용). */

/* Function calls from vhost.c to rte_vhost_user.c,
 * shall removed once virtio transport abstraction is complete. */
int vhost_user_session_set_coalescing(struct spdk_vhost_dev *dev,
				      struct spdk_vhost_session *vsession, void *ctx);
/* [한국어] 세션별 coalescing 파라미터 적용. */
int vhost_user_dev_set_coalescing(struct spdk_vhost_user_dev *user_dev, uint32_t delay_base_us,
				  uint32_t iops_threshold);
/* [한국어] 디바이스 레벨 coalescing 파라미터 저장 + 활성 세션 모두에 전파. */
int vhost_user_dev_create(struct spdk_vhost_dev *vdev, const char *name,
			  struct spdk_cpuset *cpumask,
			  const struct spdk_vhost_user_dev_backend *user_backend, bool dealy);
/* [한국어] vhost-user dev 생성(socket 등록 포함). delay=true면 listener는 나중에 활성화. */
int vhost_user_dev_init(struct spdk_vhost_dev *vdev, const char *name,
			struct spdk_cpuset *cpumask, const struct spdk_vhost_user_dev_backend *user_backend);
/* [한국어] vhost-user dev 초기화(소켓 등록 X) — vhost_user_dev_create의 부분집합. */
int vhost_user_dev_start(struct spdk_vhost_dev *vdev);
/* [한국어] delay 모드로 만들어진 vhost-user dev의 listener를 활성화. */
bool vhost_user_dev_busy(struct spdk_vhost_dev *vdev);
/* [한국어] 진행 중 비동기 작업/세션이 있어 unregister가 -EBUSY가 나는지 검사. */
int vhost_user_dev_unregister(struct spdk_vhost_dev *vdev);
/* [한국어] vhost-user dev 정리 — 세션이 모두 닫혀야 진행 가능. */
int vhost_user_init(void);
/* [한국어] vhost-user 서브시스템 초기화 (subsystem init 콜백). */
void vhost_user_fini(spdk_vhost_fini_cb vhost_cb);
/* [한국어] vhost-user 서브시스템 종료 — 비동기, 완료 시 vhost_cb 호출. */
int vhost_user_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			      uint32_t iops_threshold);
/* [한국어] backend->set_coalescing 디스패처(vhost-user 전용). */
void vhost_user_get_coalescing(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			       uint32_t *iops_threshold);
/* [한국어] backend->get_coalescing 디스패처. */

int virtio_blk_construct_ctrlr(struct spdk_vhost_dev *vdev, const char *address,
			       struct spdk_cpuset *cpumask, const struct spdk_json_val *params,
			       const struct spdk_vhost_user_dev_backend *user_backend);
/* [한국어] virtio-blk 컨트롤러 생성 — transport별 create_ctrlr 호출. */
int virtio_blk_destroy_ctrlr(struct spdk_vhost_dev *vdev);
/* [한국어] virtio-blk 컨트롤러 파괴. */

struct spdk_vhost_blk_task;
/* [한국어] forward declaration — 아래에서 정의하는 blk task 타입. */

typedef void (*virtio_blk_request_cb)(uint8_t status, struct spdk_vhost_blk_task *task,
				      void *cb_arg);
/* [한국어] virtio-blk 요청 처리 완료 시 호출되는 콜백 타입.
 * status는 VIRTIO_BLK_S_OK/IOERR/UNSUPP 중 하나. */

struct spdk_vhost_blk_task {
	struct spdk_bdev_io *bdev_io;
	/* [한국어] bdev에 발급된 I/O 핸들. 완료 시 spdk_bdev_free_io로 해제.
	 * 설정자: process_request 경로에서 bdev_*_blocks 호출 후 받음. */
	virtio_blk_request_cb cb;
	/* [한국어] 처리 완료 콜백 — vhost-blk이 used ring에 enqueue + IRQ 발사. */
	void *cb_arg;
	/* [한국어] cb로 전달할 사용자 데이터(보통 백엔드 task 컨테이너). */

	volatile uint8_t *status;
	/* [한국어] virtio-blk 응답 status 바이트 포인터 — 게스트 가시 메모리 안 위치.
	 * volatile: 다른 스레드/디바이스 접근 가능성으로 컴파일러 캐시 금지.
	 * 설정자: process_request에서 마지막 desc(=상태 바이트)를 가리키도록.
	 * 읽는 자: 게스트가 read하여 I/O 결과 확인. */

	/* for io wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	/* [한국어] bdev 큐 가득 찰 때 재시도 등록용 wait 엔트리. spdk_bdev_queue_io_wait 사용.
	 * 설정자: -ENOMEM 발생 시 cb_fn/cb_arg/bdev 채워서 대기 큐에 등록. */
	struct spdk_io_channel *bdev_io_wait_ch;
	/* [한국어] wait를 등록한 io_channel 캐싱 — 재시도 시 동일 채널 사용. */
	struct spdk_vhost_dev *bdev_io_wait_vdev;
	/* [한국어] wait 컨텍스트의 vdev 캐싱 — 콜백에서 process_request 재호출 시 필요. */

	/** Number of bytes that were written. */
	uint32_t used_len;
	/* [한국어] used ring에 보고할 "device가 쓴 바이트 수".
	 * 설정자: process_request이 페이로드 + 상태 바이트(=1)을 합산. */
	uint16_t iovcnt;
	/* [한국어] iovs 배열에 채워진 원소 수.
	 * 설정자: descriptor 체인 → iov 변환 시 증가. */
	struct iovec iovs[SPDK_VHOST_IOVS_MAX];
	/* [한국어] 게스트 desc chain을 호스트 가상 주소로 풀어놓은 IOV 배열.
	 * 설정자: vhost_vring_desc_to_iov 등이 채움.
	 * 읽는 자: spdk_bdev_*v 계열에 그대로 전달. */

	/** Size of whole payload in bytes */
	uint32_t payload_size;
	/* [한국어] 데이터 페이로드 총 크기(상태 바이트 제외).
	 * 설정자: iovs 합 - 1byte(status). 읽는 자: bdev I/O 길이 인자. */
};

int virtio_blk_process_request(struct spdk_vhost_dev *vdev, struct spdk_io_channel *ch,
			       struct spdk_vhost_blk_task *task, virtio_blk_request_cb cb, void *cb_arg);
/* [한국어] virtio-blk 요청 처리 진입점 — task의 iovs/payload를 분석해 bdev I/O 발행.
 * 호출 컨텍스트: 디바이스 thread (폴링 루프 또는 다른 transport 진입점). */

typedef void (*bdev_event_cb_complete)(struct spdk_vhost_dev *vdev, void *ctx);
/* [한국어] bdev 이벤트(REMOVE/RESIZE) 처리 완료 알림 콜백 — vhost-blk과 transport 간. */

#define SPDK_VIRTIO_BLK_TRSTRING_MAX_LEN 32
/* [한국어] virtio-blk transport 이름 최대 길이("vhost_user", "vfio_user" 등). */

struct spdk_virtio_blk_transport_ops {
	/**
	 * Transport name
	 */
	char name[SPDK_VIRTIO_BLK_TRSTRING_MAX_LEN];
	/* [한국어] transport를 식별하는 문자열. RPC virtio_blk_create_transport에서 매칭 키. */

	/**
	 * Create a transport for the given transport opts
	 */
	struct spdk_virtio_blk_transport *(*create)(const struct spdk_json_val *params);
	/* [한국어] transport 인스턴스 생성. JSON 파라미터로 옵션 수신. */

	/**
	 * Dump transport-specific opts into JSON
	 */
	void (*dump_opts)(struct spdk_virtio_blk_transport *transport, struct spdk_json_write_ctx *w);
	/* [한국어] 현재 transport의 옵션을 RPC virtio_blk_get_transports 응답으로 출력. */

	/**
	 * Destroy the transport
	 */
	int (*destroy)(struct spdk_virtio_blk_transport *transport,
		       spdk_vhost_fini_cb cb_fn);
	/* [한국어] transport 파괴(비동기) — 완료 시 cb_fn 호출. */

	/**
	 * Create vhost block controller
	 */
	int (*create_ctrlr)(struct spdk_vhost_dev *vdev, struct spdk_cpuset *cpumask,
			    const char *address, const struct spdk_json_val *params,
			    void *custom_opts);
	/* [한국어] transport 위에 vhost-blk 컨트롤러 생성 (소켓 등록 포함). */

	/**
	 * Destroy vhost block controller
	 */
	int (*destroy_ctrlr)(struct spdk_vhost_dev *vdev);
	/* [한국어] vhost-blk 컨트롤러 파괴. */

	/*
	 * Signal removal of the bdev.
	 */
	void (*bdev_event)(enum spdk_bdev_event_type type, struct spdk_vhost_dev *vdev,
			   bdev_event_cb_complete cb, void *cb_arg);
	/* [한국어] 백엔드 bdev에서 발생한 이벤트(REMOVE/RESIZE) 전달. transport별 처리.
	 * REMOVE: I/O drain → 컨트롤러 제거. */

	/**
	 * Set coalescing parameters.
	 */
	int (*set_coalescing)(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			      uint32_t iops_threshold);
	/* [한국어] transport별 coalescing 설정 (vhost-user는 활성, vfio-user는 미지원 가능). */

	/**
	 * Get coalescing parameters.
	 */
	void (*get_coalescing)(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			       uint32_t *iops_threshold);
	/* [한국어] transport별 coalescing 조회. */
};

struct spdk_virtio_blk_transport {
	const struct spdk_virtio_blk_transport_ops	*ops;
	/* [한국어] transport 콜백 vtable 포인터 — 등록된 ops를 가리킴.
	 * 설정자: ops->create()에서. 읽는 자: 모든 transport 호출 시. */
	TAILQ_ENTRY(spdk_virtio_blk_transport)		tailq;
	/* [한국어] 활성 transport 인스턴스 리스트 노드 — virtio_blk_transport_get_first/next 순회용. */
};

struct virtio_blk_transport_ops_list_element {
	struct spdk_virtio_blk_transport_ops			ops;
	/* [한국어] 등록된 transport ops 사본(또는 원본 참조). */
	TAILQ_ENTRY(virtio_blk_transport_ops_list_element)	link;
	/* [한국어] 등록된 ops 리스트 노드. */
};

void virtio_blk_transport_register(const struct spdk_virtio_blk_transport_ops *ops);
/* [한국어] transport ops 등록 — 보통 SPDK_VIRTIO_BLK_TRANSPORT_REGISTER 매크로로 호출. */
int virtio_blk_transport_create(const char *transport_name, const struct spdk_json_val *params);
/* [한국어] 이름으로 transport 인스턴스 생성 (RPC virtio_blk_create_transport). */
int virtio_blk_transport_destroy(struct spdk_virtio_blk_transport *transport,
				 spdk_vhost_fini_cb cb_fn);
/* [한국어] transport 파괴(비동기). */
struct spdk_virtio_blk_transport *virtio_blk_transport_get_first(void);
/* [한국어] 활성 transport 리스트 첫 노드. */
struct spdk_virtio_blk_transport *virtio_blk_transport_get_next(
	struct spdk_virtio_blk_transport *transport);
/* [한국어] 다음 transport 노드 — NULL이면 끝. */
void virtio_blk_transport_dump_opts(struct spdk_virtio_blk_transport *transport,
				    struct spdk_json_write_ctx *w);
/* [한국어] transport 옵션 JSON 덤프 (RPC 응답용). */
struct spdk_virtio_blk_transport *virtio_blk_tgt_get_transport(const char *transport_name);
/* [한국어] 이름으로 활성 transport 인스턴스 조회. */
const struct spdk_virtio_blk_transport_ops *virtio_blk_get_transport_ops(
	const char *transport_name);
/* [한국어] 이름으로 등록된 transport ops 조회. */

void vhost_session_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
/* [한국어] 디바이스에 연결된 모든 세션 정보(vid/started/queue 수)를 JSON으로 덤프. */

/*
 * Macro used to register new transports.
 */
#define SPDK_VIRTIO_BLK_TRANSPORT_REGISTER(name, transport_ops) \
static void __attribute__((constructor)) _virtio_blk_transport_register_##name(void) \
{ \
	virtio_blk_transport_register(transport_ops); \
}
/* [한국어] virtio-blk transport 자동 등록 매크로.
 *  - __attribute__((constructor)): main() 이전 자동 호출 — 라이브러리 로드 시 등록 보장.
 *  - 함수 이름에 ##name 토큰 결합으로 transport별 고유 이름 생성 (중복 정의 방지).
 *  - 사용처: vhost_user_blk.c 등에서 SPDK_VIRTIO_BLK_TRANSPORT_REGISTER(vhost_user, &ops). */

#endif /* SPDK_VHOST_INTERNAL_H */
/* [한국어] include guard 닫힘. */
