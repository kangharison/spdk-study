/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * [한국어 설명] SPDK vhost <-> DPDK rte_vhost 통합 레이어 (rte_vhost_user.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK vhost 백엔드와 DPDK rte_vhost 라이브러리 사이의 어댑터다.
 * DPDK rte_vhost는 QEMU와의 vhost-user 프로토콜(UNIX 소켓 위 메시지 교환,
 * 게스트 메모리 mmap, virtqueue 셋업 등)을 모두 처리해 주는 라이브러리이지만,
 * 이를 SPDK의 단일 thread reactor 모델과 결합하려면:
 *   - DPDK 내부 pthread에서 호출되는 콜백을 SPDK thread로 옮기고,
 *   - GPA→VVA 변환, used ring/IRQ 시그널링, dirty page 로깅, inflight 처리,
 *   - virtqueue desc chain 순회 (split/packed) 등 핵심 헬퍼를 제공해야 한다.
 * 이 모든 결합 코드가 이 파일에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [QEMU/게스트 VM]
 *      ↕ (UNIX 도메인 소켓, vhost-user 메시지)
 *   [DPDK rte_vhost 라이브러리]
 *      ↕ (콜백: new_device/destroy_device/new_connection/.../get_config 등)
 *   [이 파일: rte_vhost_user.c]   <— SPDK ↔ DPDK 어댑터
 *      ↕ (spdk_thread_send_msg/sem_wait)
 *   [vhost_blk.c / vhost_scsi.c] (백엔드)
 *
 * 호출 컨텍스트: 두 종류 thread를 명확히 구분.
 *   1) DPDK pthread (rte_vhost 내부): 콜백(new_device 등) 진입점. 동기 호출.
 *   2) SPDK reactor thread: vsession이 할당된 lcore의 spdk_thread.
 * 두 thread 사이는 dpdk_sem 세마포어로 동기화 (DPDK pthread가 wait, SPDK가 post).
 *
 * === 타 모듈과의 연결 ===
 * - 의존: rte_vhost.h, vhost_internal.h, spdk/thread.h, spdk_internal/vhost_user.h.
 * - 사용처: vhost_blk.c, vhost_scsi.c (백엔드 콜백 vtable 통해), vhost.c (공통 인프라).
 * - 데이터 흐름:
 *     게스트 메모리 → rte_vhost가 mmap → vhost_session_mem_register로 DPDK env에 등록 →
 *     백엔드가 vhost_gpa_to_vva로 호스트 주소 변환해 SPDK bdev I/O 발급.
 *
 * === 주요 함수/구조체 요약 ===
 * - vhost_gpa_to_vva: 게스트 GPA → 호스트 VVA 변환 (모든 백엔드의 핵심 헬퍼).
 * - vhost_vq_avail_ring_get / vhost_vq_used_ring_enqueue / vhost_vq_used_signal:
 *   split ring 처리 핵심 (avail 폴, used 등록, IRQ).
 * - vhost_vq_get_desc / _packed: descriptor 체인 획득.
 * - vhost_vring_desc_to_iov / _packed_desc_to_iov: desc → 호스트 iovec 변환.
 * - new_connection / destroy_connection / new_device / destroy_device:
 *   DPDK rte_vhost가 호출하는 콜백들 (vhost-user 라이프사이클).
 * - vhost_user_session_send_event: DPDK pthread → SPDK thread 메시지 전달 + sem_wait.
 * - vhost_user_session_start / stop: 백엔드 콜백 디스패처 (foreach_session 패턴 사용).
 * - vhost_register_unix_socket: UNIX 소켓을 rte_vhost에 등록 (디바이스 생성 시 호출).
 * - vhost_session_install_rte_compat_hooks: DPDK 버전별 콜백 호환 등록.
 * - dpdk_sem 동기화 패턴: DPDK 콜백 → 메시지 enqueue → sem_wait → SPDK가 post.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 C 헤더 통합. */

#include "spdk/env.h"
/* [한국어] DPDK 환경 추상화. */
#include "spdk/likely.h"
/* [한국어] 분기 힌트. */
#include "spdk/string.h"
/* [한국어] spdk_strerror 등. */
#include "spdk/util.h"
/* [한국어] SPDK_CONTAINEROF 등. */
#include "spdk/memory.h"
/* [한국어] DMA 메모리 등록 헬퍼. */
#include "spdk/barrier.h"
/* [한국어] spdk_smp_rmb/wmb 등 메모리 배리어 (멀티코어 가시성 보장). */
#include "spdk/vhost.h"
/* [한국어] vhost 공개 API. */
#include "vhost_internal.h"
/* [한국어] vhost 내부 헤더. */
#include <rte_version.h>
/* [한국어] DPDK 버전 매크로 — 콜백 시그니처가 버전마다 다르므로 #if 분기에 사용. */

#include "spdk_internal/vhost_user.h"
/* [한국어] SPDK 내부 vhost-user 인터페이스 (spdk_vhost_fini_cb 등). */

/* Path to folder where character device will be created. Can be set by user. */
static char g_vhost_user_dev_dirname[PATH_MAX] = "";
/* [한국어] vhost-user UNIX 소켓을 만들 디렉토리(보통 /var/tmp). 사용자가 spdk_vhost_set_socket_path로 설정.
 * 빈 문자열이면 path 인자가 절대 경로여야 함. */

static struct spdk_thread *g_vhost_user_init_thread;
/* [한국어] vhost-user 서브시스템 초기화 시점의 thread 포인터.
 * 디바이스 등록 등 관리 작업의 기본 thread로 사용. */

struct vhost_session_fn_ctx {
	/** Device pointer obtained before enqueueing the event */
	struct spdk_vhost_dev *vdev;
	/* [한국어] foreach_session enqueue 시점에 캡처한 vdev 포인터.
	 * 콜백 진입 시 vdev 유효성 보장(이 컨텍스트가 살아있으면 unregister 진행 안 됨). */

	/** ID of the session to send event to. */
	uint32_t vsession_id;
	/* [한국어] 대상 세션의 SPDK 자체 ID — 콜백 진입 후 vid_to_session으로 변환. */

	/** User provided function to be executed on session's thread. */
	spdk_vhost_session_fn cb_fn;
	/* [한국어] 세션의 lcore에서 호출될 사용자 콜백. */

	/**
	 * User provided function to be called on the init thread
	 * after iterating through all sessions.
	 */
	spdk_vhost_dev_fn cpl_fn;
	/* [한국어] 모든 세션 처리 후 init thread에서 호출할 완료 콜백 (옵션). */

	/** Custom user context */
	void *user_ctx;
	/* [한국어] cb_fn/cpl_fn에 전달할 사용자 컨텍스트. */
};

static int vhost_user_wait_for_session_stop(struct spdk_vhost_session *vsession,
		unsigned timeout_sec, const char *errmsg);
/* [한국어] forward declaration — DPDK pthread가 SPDK 백엔드의 stop 완료를 기다리는 함수. */

/*
 * [한국어]
 * vhost_gpa_to_vva - 게스트 물리 주소(GPA) → 호스트 가상 주소(VVA) 변환.
 *
 * @vsession: 메모리 매핑 표(vsession->mem)를 보유한 세션.
 * @addr: 변환할 게스트 물리 주소.
 * @len: 필요한 연속 길이 (이 길이만큼이 단일 매핑 영역 안에 있어야 함).
 * @return: 호스트 가상 주소 또는 NULL(매핑 영역 경계를 넘는 경우).
 *
 * 모든 백엔드가 게스트 desc/buf 접근 시 사용하는 가장 중요한 헬퍼.
 * rte_vhost_va_from_guest_pa는 newlen에 실제 매핑된 연속 길이를 채워주므로
 * 요청 len과 일치하지 않으면 NULL 반환 — 부분 매핑은 호출자가 처리해야 함.
 *
 * 호출 컨텍스트: SPDK reactor thread (백엔드 핫패스).
 */
void *
vhost_gpa_to_vva(struct spdk_vhost_session *vsession, uint64_t addr, uint64_t len)
{
	void *vva;
	/* [한국어] 변환 결과. */
	uint64_t newlen;
	/* [한국어] 실제 매핑 연속 길이 (rte_vhost가 채워줌). */

	newlen = len;
	/* [한국어] 입력 길이로 초기화. */
	vva = (void *)rte_vhost_va_from_guest_pa(vsession->mem, addr, &newlen);
	/* [한국어] DPDK가 mmap한 영역에서 GPA를 검색해 호스트 주소 반환.
	 * mem은 rte_vhost_get_mem_table로 받은 게스트 메모리 영역 표. */
	if (newlen != len) {
		/* [한국어] 매핑 경계를 넘음 — 두 영역에 걸친 GPA, 안전하게 NULL 반환. */
		return NULL;
	}

	return vva;

}

/*
 * [한국어]
 * vhost_log_req_desc - 요청 desc chain의 모든 device-write buf 페이지를 dirty page log에 기록.
 *
 * @vsession: 세션.
 * @virtqueue: 큐.
 * @req_id: avail ring에서 받은 desc head 인덱스.
 *
 * VHOST_F_LOG_ALL이 협상돼 있을 때만 동작 — live migration 시 dirty page tracking에 사용.
 * 실제로는 device-write buf의 전체 영역을 dirty로 표시 — 정확도보다 단순함 우선.
 */
static void
vhost_log_req_desc(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *virtqueue,
		   uint16_t req_id)
{
	struct vring_desc *desc, *desc_table;
	uint32_t desc_table_size;
	int rc;

	if (spdk_likely(!vhost_dev_has_feature(vsession, VHOST_F_LOG_ALL))) {
		return;
		/* [한국어] migration 비활성 시 fast path 즉시 리턴. */
	}

	rc = vhost_vq_get_desc(vsession, virtqueue, req_id, &desc, &desc_table, &desc_table_size);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Can't log used ring descriptors!\n");
		return;
	}

	do {
		if (vhost_vring_desc_is_wr(desc)) {
			/* To be honest, only pages really touched should be logged, but
			 * doing so would require tracking those changes in each backed.
			 * Also backend most likely will touch all/most of those pages so
			 * for lets assume we touched all pages passed to as writeable buffers. */
			rte_vhost_log_write(vsession->vid, desc->addr, desc->len);
			/* [한국어] 게스트 GPA 영역을 dirty로 마킹 — DPDK가 log 비트맵에 기록. */
		}
		vhost_vring_desc_get_next(&desc, desc_table, desc_table_size);
		/* [한국어] 다음 desc 이동. */
	} while (desc);
}

/*
 * [한국어]
 * vhost_log_used_vring_elem - used ring의 한 엔트리(idx)를 dirty page log에 기록.
 *
 * 게스트가 보는 used ring 자체도 마이그레이션 시 동기화돼야 하므로, 우리가 used를 갱신할 때마다
 * 그 영역을 dirty로 표시.
 */
static void
vhost_log_used_vring_elem(struct spdk_vhost_session *vsession,
			  struct spdk_vhost_virtqueue *virtqueue,
			  uint16_t idx)
{
	uint64_t offset, len;

	if (spdk_likely(!vhost_dev_has_feature(vsession, VHOST_F_LOG_ALL))) {
		return;
	}

	if (spdk_unlikely(virtqueue->packed.packed_ring)) {
		/* [한국어] packed ring: 단일 desc 배열에 used flag도 함께 들어감 — 배열 i 위치 기록. */
		offset = idx * sizeof(struct vring_packed_desc);
		len = sizeof(struct vring_packed_desc);
	} else {
		/* [한국어] split ring: used 영역 ring[i] 위치만 dirty. */
		offset = offsetof(struct vring_used, ring[idx]);
		len = sizeof(virtqueue->vring.used->ring[idx]);
	}

	rte_vhost_log_used_vring(vsession->vid, virtqueue->vring_idx, offset, len);
}

/*
 * [한국어]
 * vhost_log_used_vring_idx - used ring의 idx 필드 변경을 dirty page log에 기록.
 *
 * used ring의 idx 필드는 SPDK가 갱신할 때마다 게스트 측 동기화가 필요.
 */
static void
vhost_log_used_vring_idx(struct spdk_vhost_session *vsession,
			 struct spdk_vhost_virtqueue *virtqueue)
{
	uint64_t offset, len;
	uint16_t vq_idx;

	if (spdk_likely(!vhost_dev_has_feature(vsession, VHOST_F_LOG_ALL))) {
		return;
	}

	offset = offsetof(struct vring_used, idx);
	len = sizeof(virtqueue->vring.used->idx);
	vq_idx = virtqueue - vsession->virtqueue;
	/* [한국어] 포인터 산술로 큐 인덱스 추출 (배열 첫 원소 주소 빼기). */

	rte_vhost_log_used_vring(vsession->vid, vq_idx, offset, len);
}

/*
 * Get available requests from avail ring.
 */
/*
 * [한국어]
 * vhost_vq_avail_ring_get - split ring avail 영역에서 새 요청들을 한 번에 가져오기.
 *
 * @virtqueue: 대상 큐.
 * @reqs: 채울 desc head 인덱스 배열.
 * @reqs_len: reqs 배열 크기 (보통 32).
 * @return: 채워진 요청 수(<=reqs_len).
 *
 * 핵심 단계:
 *   1) spdk_smp_rmb로 메모리 배리어 — avail->idx의 최신값 읽기 보장.
 *   2) interrupt 모드면 kickfd read (0으로 클리어).
 *   3) avail->idx - last_avail_idx로 새 요청 수 계산.
 *   4) reqs에 desc 인덱스 복사 (size_mask로 wrap).
 *   5) interrupt 모드에서 race 발생 시 kickfd 자가 트리거.
 *
 * 호출 컨텍스트: 디바이스 thread (백엔드 폴러).
 */
uint16_t
vhost_vq_avail_ring_get(struct spdk_vhost_virtqueue *virtqueue, uint16_t *reqs,
			uint16_t reqs_len)
{
	struct rte_vhost_vring *vring = &virtqueue->vring;
	/* [한국어] 큐 메타. */
	struct vring_avail *avail = vring->avail;
	/* [한국어] avail ring 포인터 (게스트 메모리에 직접 접근). */
	uint16_t size_mask = vring->size - 1;
	/* [한국어] 큐 크기 - 1 (큐 size는 2의 거듭제곱이라 가정 — wrap을 비트 AND로 처리). */
	uint16_t last_idx = virtqueue->last_avail_idx, avail_idx = avail->idx;
	/* [한국어] 마지막 처리 인덱스(SPDK 측), 게스트가 채운 인덱스 — 둘의 차가 새 요청 수. */
	uint16_t count, i;
	/* [한국어] 새 요청 수, 루프 변수. */
	int rc;
	uint64_t u64_value;
	/* [한국어] kickfd read/write 임시 변수. */

	spdk_smp_rmb();
	/* [한국어] 메모리 배리어 — 게스트가 desc 작성 후 avail->idx 갱신했으므로,
	 * avail->idx 읽기 전 reordering 방지가 필요. */

	if (virtqueue->vsession && spdk_unlikely(spdk_interrupt_mode_is_enabled())) {
		/* Read to clear vring's kickfd */
		/* [한국어] interrupt 모드에서는 kickfd가 일반 fd처럼 동작 — 이벤트 회수를 위해 read 필요. */
		rc = read(vring->kickfd, &u64_value, sizeof(u64_value));
		if (rc < 0) {
			SPDK_ERRLOG("failed to acknowledge kickfd: %s.\n", spdk_strerror(errno));
			return -errno;
		}
	}

	count = avail_idx - last_idx;
	/* [한국어] 16비트 wrap-around 의미 — count는 자연스럽게 모듈로 65536. */
	if (spdk_likely(count == 0)) {
		return 0;
		/* [한국어] 새 요청 없음 — fast path. */
	}

	if (spdk_unlikely(count > vring->size)) {
		/* TODO: the queue is unrecoverably broken and should be marked so.
		 * For now we will fail silently and report there are no new avail entries.
		 */
		/* [한국어] 큐 크기보다 큰 요청 수 — 게스트 측 손상. 안전하게 무시. */
		return 0;
	}

	count = spdk_min(count, reqs_len);
	/* [한국어] 호출자가 받을 수 있는 만큼만 처리. */

	virtqueue->last_avail_idx += count;
	/* [한국어] last_avail_idx 갱신 — 다음 호출 시 여기부터 시작. */
	/* Check whether there are unprocessed reqs in vq, then kick vq manually */
	if (virtqueue->vsession && spdk_unlikely(spdk_interrupt_mode_is_enabled())) {
		/* If avail_idx is larger than virtqueue's last_avail_idx, then there is unprocessed reqs.
		 * avail_idx should get updated here from memory, in case of race condition with guest.
		 */
		/* [한국어] interrupt 모드 race: 게스트가 우리가 read 후 또 enqueue했을 수 있음.
		 * avail->idx 다시 읽어 unprocessed가 있으면 자가 kickfd write로 다음 콜백 트리거. */
		avail_idx = * (volatile uint16_t *) &avail->idx;
		/* [한국어] volatile 캐스팅으로 컴파일러 캐시 회피 — 매번 메모리에서 읽기. */
		if (avail_idx > virtqueue->last_avail_idx) {
			/* Write to notify vring's kickfd */
			rc = write(vring->kickfd, &u64_value, sizeof(u64_value));
			/* [한국어] kickfd에 카운터 write — eventfd라면 자기 자신 깨움. */
			if (rc < 0) {
				SPDK_ERRLOG("failed to kick vring: %s.\n", spdk_strerror(errno));
				return -errno;
			}
		}
	}

	for (i = 0; i < count; i++) {
		reqs[i] = vring->avail->ring[(last_idx + i) & size_mask];
		/* [한국어] avail ring[i]에 든 desc head 인덱스를 reqs로 복사. */
	}

	SPDK_DEBUGLOG(vhost_ring,
		      "AVAIL: last_idx=%"PRIu16" avail_idx=%"PRIu16" count=%"PRIu16"\n",
		      last_idx, avail_idx, count);

	return count;
}

/*
 * [한국어]
 * vhost_vring_desc_is_indirect - split desc의 INDIRECT 플래그 검사.
 *
 * VIRTIO_RING_F_INDIRECT_DESC가 협상되면 게스트는 한 desc 안에 별도의 desc table 주소를 둘 수 있다.
 * 그 desc는 head desc로만 쓰이고 실제 chain은 이 indirect table을 따라간다.
 */
static bool
vhost_vring_desc_is_indirect(struct vring_desc *cur_desc)
{
	return !!(cur_desc->flags & VRING_DESC_F_INDIRECT);
	/* [한국어] flags의 INDIRECT 비트 boolean 반환. */
}

/*
 * [한국어]
 * vhost_vring_packed_desc_is_indirect - packed desc의 INDIRECT 검사 (split과 동일 비트). */
static bool
vhost_vring_packed_desc_is_indirect(struct vring_packed_desc *cur_desc)
{
	return (cur_desc->flags & VRING_DESC_F_INDIRECT) != 0;
}

/*
 * [한국어]
 * vhost_inflight_packed_desc_is_indirect - inflight 영역의 packed desc INDIRECT 검사. */
static bool
vhost_inflight_packed_desc_is_indirect(spdk_vhost_inflight_desc *cur_desc)
{
	return (cur_desc->flags & VRING_DESC_F_INDIRECT) != 0;
}

/*
 * [한국어]
 * vhost_vq_get_desc - split ring에서 req_idx 위치 desc + desc_table 획득.
 *
 * @vsession: 세션 (GPA 변환에 사용).
 * @virtqueue: 큐.
 * @req_idx: avail에서 받은 desc head 인덱스.
 * @desc: 출력 - 첫 desc 포인터.
 * @desc_table: 출력 - 사용할 desc table (기본 vring 또는 indirect).
 * @desc_table_size: 출력 - desc table 크기.
 * @return: 0=성공, -1=실패(범위 초과 또는 indirect 매핑 실패).
 *
 * INDIRECT 비트가 켜져 있으면 desc->addr이 게스트 메모리의 별도 desc table을 가리킴 → 매핑하여
 * desc를 첫 원소로 설정. 아니면 큐의 기본 desc 배열을 쓰고 desc는 큐 desc[req_idx]를 가리킴.
 *
 * 호출 컨텍스트: 디바이스 thread (백엔드 핫패스).
 */
int
vhost_vq_get_desc(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *virtqueue,
		  uint16_t req_idx, struct vring_desc **desc, struct vring_desc **desc_table,
		  uint32_t *desc_table_size)
{
	if (spdk_unlikely(req_idx >= virtqueue->vring.size)) {
		return -1;
		/* [한국어] 범위 가드 — 게스트가 보낸 손상된 인덱스 방어. */
	}

	*desc = &virtqueue->vring.desc[req_idx];
	/* [한국어] 일단 큐의 desc 배열에서 가져오기. */

	if (vhost_vring_desc_is_indirect(*desc)) {
		/* [한국어] INDIRECT — desc 자체가 별도 desc table을 가리킴. */
		*desc_table_size = (*desc)->len / sizeof(**desc);
		/* [한국어] table 크기 = 영역 길이 / desc 크기 (16바이트). */
		*desc_table = vhost_gpa_to_vva(vsession, (*desc)->addr,
					       sizeof(**desc) * *desc_table_size);
		/* [한국어] 게스트 GPA → 호스트 주소 매핑. */
		*desc = *desc_table;
		/* [한국어] 첫 desc는 indirect table[0]. */
		if (*desc == NULL) {
			return -1;
		}

		return 0;
	}

	*desc_table = virtqueue->vring.desc;
	/* [한국어] 일반 — 큐 desc 배열 그대로. */
	*desc_table_size = virtqueue->vring.size;

	return 0;
}

static bool
vhost_packed_desc_indirect_to_desc_table(struct spdk_vhost_session *vsession,
		uint64_t addr, uint32_t len,
		struct vring_packed_desc **desc_table,
		uint32_t *desc_table_size)
{
	*desc_table_size = len / sizeof(struct vring_packed_desc);

	*desc_table = vhost_gpa_to_vva(vsession, addr, len);
	if (spdk_unlikely(*desc_table == NULL)) {
		return false;
	}

	return true;
}

int
vhost_vq_get_desc_packed(struct spdk_vhost_session *vsession,
			 struct spdk_vhost_virtqueue *virtqueue,
			 uint16_t req_idx, struct vring_packed_desc **desc,
			 struct vring_packed_desc **desc_table, uint32_t *desc_table_size)
{
	*desc =  &virtqueue->vring.desc_packed[req_idx];

	/* In packed ring when the desc is non-indirect we get next desc
	 * by judging (desc->flag & VRING_DESC_F_NEXT) != 0. When the desc
	 * is indirect we get next desc by idx and desc_table_size. It's
	 * different from split ring.
	 */
	if (vhost_vring_packed_desc_is_indirect(*desc)) {
		if (!vhost_packed_desc_indirect_to_desc_table(vsession, (*desc)->addr, (*desc)->len,
				desc_table, desc_table_size)) {
			return -1;
		}

		*desc = *desc_table;
	} else {
		*desc_table = NULL;
		*desc_table_size  = 0;
	}

	return 0;
}

int
vhost_inflight_queue_get_desc(struct spdk_vhost_session *vsession,
			      spdk_vhost_inflight_desc *desc_array,
			      uint16_t req_idx, spdk_vhost_inflight_desc **desc,
			      struct vring_packed_desc  **desc_table, uint32_t *desc_table_size)
{
	*desc = &desc_array[req_idx];

	if (vhost_inflight_packed_desc_is_indirect(*desc)) {
		if (!vhost_packed_desc_indirect_to_desc_table(vsession, (*desc)->addr, (*desc)->len,
				desc_table, desc_table_size)) {
			return -1;
		}

		/* This desc is the inflight desc not the packed desc.
		 * When set the F_INDIRECT the table entry should be the packed desc
		 * so set the inflight desc NULL.
		 */
		*desc = NULL;
	} else {
		/* When not set the F_INDIRECT means there is no packed desc table */
		*desc_table = NULL;
		*desc_table_size = 0;
	}

	return 0;
}

/*
 * [한국어]
 * vhost_vq_used_signal - 누적된 used 변경분에 대해 게스트에 IRQ(callfd) 신호 송신.
 *
 * @vsession: 세션.
 * @virtqueue: 큐.
 * @return: 1=IRQ 송신, 0=송신 안 함.
 *
 * used_req_cnt가 0이면 송신 불필요(게스트는 이미 모든 used를 봤음).
 * rte_vhost_vring_call(_nonblock)이 callfd write — 게스트는 이를 IRQ로 인지.
 *
 * DPDK 22.11 이후 nonblock 버전 사용 — call이 차단되어 SPDK reactor를 막는 일 방지.
 */
int
vhost_vq_used_signal(struct spdk_vhost_session *vsession,
		     struct spdk_vhost_virtqueue *virtqueue)
{
	if (virtqueue->used_req_cnt == 0) {
		return 0;
		/* [한국어] 보낼 게 없음 — fast path. */
	}

	SPDK_DEBUGLOG(vhost_ring,
		      "Queue %td - USED RING: sending IRQ: last used %"PRIu16"\n",
		      virtqueue - vsession->virtqueue, virtqueue->last_used_idx);

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
	if (rte_vhost_vring_call(vsession->vid, virtqueue->vring_idx) == 0) {
	/* [한국어] 22.11 이전: 차단형 — 콜백이 끝나야 리턴. */
#else
	if (rte_vhost_vring_call_nonblock(vsession->vid, virtqueue->vring_idx) == 0) {
	/* [한국어] 22.11 이후: nonblock — 즉시 리턴, 송신 실패 시 다음에 재시도. */
#endif
		/* interrupt signalled */
		virtqueue->req_cnt += virtqueue->used_req_cnt;
		/* [한국어] 통계용 누적 — coalescing 의사결정에 사용. */
		virtqueue->used_req_cnt = 0;
		/* [한국어] 송신 완료 — coalescing window 리셋. */
		return 1;
	} else {
		/* interrupt not signalled */
		return 0;
	}
}

static void
session_vq_io_stats_update(struct spdk_vhost_session *vsession,
			   struct spdk_vhost_virtqueue *virtqueue, uint64_t now)
{
	uint32_t irq_delay_base = vsession->coalescing_delay_time_base;
	uint32_t io_threshold = vsession->coalescing_io_rate_threshold;
	int32_t irq_delay;
	uint32_t req_cnt;

	req_cnt = virtqueue->req_cnt + virtqueue->used_req_cnt;
	if (req_cnt <= io_threshold) {
		return;
	}

	irq_delay = (irq_delay_base * (req_cnt - io_threshold)) / io_threshold;
	virtqueue->irq_delay_time = (uint32_t) spdk_max(0, irq_delay);

	virtqueue->req_cnt = 0;
	virtqueue->next_event_time = now;
}

static void
check_session_vq_io_stats(struct spdk_vhost_session *vsession,
			  struct spdk_vhost_virtqueue *virtqueue, uint64_t now)
{
	if (now < vsession->next_stats_check_time) {
		return;
	}

	vsession->next_stats_check_time = now + vsession->stats_check_interval;
	session_vq_io_stats_update(vsession, virtqueue, now);
}

static inline bool
vhost_vq_event_is_suppressed(struct spdk_vhost_virtqueue *vq)
{
	spdk_smp_mb();

	if (spdk_unlikely(vq->packed.packed_ring)) {
		if (vq->vring.driver_event->flags & VRING_PACKED_EVENT_FLAG_DISABLE) {
			return true;
		}
	} else {
		if (vq->vring.avail->flags & VRING_AVAIL_F_NO_INTERRUPT) {
			return true;
		}
	}

	return false;
}

void
vhost_session_vq_used_signal(struct spdk_vhost_virtqueue *virtqueue)
{
	struct spdk_vhost_session *vsession = virtqueue->vsession;
	uint64_t now;

	if (vsession->coalescing_delay_time_base == 0) {
		if (virtqueue->vring.desc == NULL) {
			return;
		}

		if (vhost_vq_event_is_suppressed(virtqueue)) {
			return;
		}

		vhost_vq_used_signal(vsession, virtqueue);
	} else {
		now = spdk_get_ticks();
		check_session_vq_io_stats(vsession, virtqueue, now);

		/* No need for event right now */
		if (now < virtqueue->next_event_time) {
			return;
		}

		if (vhost_vq_event_is_suppressed(virtqueue)) {
			return;
		}

		if (!vhost_vq_used_signal(vsession, virtqueue)) {
			return;
		}

		/* Syscall is quite long so update time */
		now = spdk_get_ticks();
		virtqueue->next_event_time = now + virtqueue->irq_delay_time;
	}
}

/*
 * Enqueue id and len to used ring.
 */
/*
 * [한국어]
 * vhost_vq_used_ring_enqueue - split ring used 영역에 (id, len) 엔트리 enqueue.
 *
 * @vsession: 세션.
 * @virtqueue: 큐.
 * @id: 완료된 desc head 인덱스.
 * @len: device가 작성한 바이트 수 (게스트가 used에서 읽음).
 *
 * 핵심 단계:
 *   1) dirty page 로깅 (req desc 영역 + used ring 영역).
 *   2) last_used_idx++ → used->ring[i] 채움.
 *   3) spdk_smp_wmb로 ring 갱신을 idx 갱신 전 가시화.
 *   4) inflight 영역에서 이 io를 "마지막 처리됨"으로 마킹 후, 완료 후 클리어.
 *   5) used->idx volatile write — 게스트가 이 변화를 감지해 새 used 발견.
 *   6) used_req_cnt++로 IRQ 송신 통계.
 *   7) interrupt 모드면 즉시 IRQ 송신.
 *
 * 호출 컨텍스트: 디바이스 thread (백엔드 완료 콜백).
 */
void
vhost_vq_used_ring_enqueue(struct spdk_vhost_session *vsession,
			   struct spdk_vhost_virtqueue *virtqueue,
			   uint16_t id, uint32_t len)
{
	struct rte_vhost_vring *vring = &virtqueue->vring;
	/* [한국어] 큐 메타. */
	struct vring_used *used = vring->used;
	/* [한국어] used ring 포인터 (게스트 메모리). */
	uint16_t last_idx = virtqueue->last_used_idx & (vring->size - 1);
	/* [한국어] used ring slot 위치 (모듈로 size). */
	uint16_t vq_idx = virtqueue->vring_idx;
	/* [한국어] 큐 인덱스. */

	SPDK_DEBUGLOG(vhost_ring,
		      "Queue %td - USED RING: last_idx=%"PRIu16" req id=%"PRIu16" len=%"PRIu32"\n",
		      virtqueue - vsession->virtqueue, virtqueue->last_used_idx, id, len);

	vhost_log_req_desc(vsession, virtqueue, id);
	/* [한국어] migration용 dirty page 로깅 (요청 desc chain의 device-write buf). */

	virtqueue->last_used_idx++;
	/* [한국어] free-running counter — wrap은 캐스팅에 의해 자연 처리. */
	used->ring[last_idx].id = id;
	/* [한국어] 슬롯에 desc head id 기록. */
	used->ring[last_idx].len = len;
	/* [한국어] 슬롯에 device가 쓴 바이트 수 기록. */

	/* Ensure the used ring is updated before we log it or increment used->idx. */
	spdk_smp_wmb();
	/* [한국어] write 배리어 — 게스트가 used->idx 보고 ring을 read할 때 부분 갱신 보지 않도록.
	 * x86에서는 컴파일러 배리어, ARM 등에서는 dmb st 등 실제 명령. */

	rte_vhost_set_last_inflight_io_split(vsession->vid, vq_idx, id);
	/* [한국어] inflight 공유 메모리에 "이 io를 곧 used에 기록할 것"이라고 표시.
	 * crash 시점이 used 갱신 직전이면 재기동 후 inflight 분석으로 복구. */

	vhost_log_used_vring_elem(vsession, virtqueue, last_idx);
	/* [한국어] used ring 슬롯 dirty 로깅. */
	* (volatile uint16_t *) &used->idx = virtqueue->last_used_idx;
	/* [한국어] used->idx 갱신 — 게스트가 이 값으로 새 used 인지.
	 * volatile 캐스팅으로 컴파일러 최적화 회피. */
	vhost_log_used_vring_idx(vsession, virtqueue);
	/* [한국어] used->idx 영역도 dirty 로깅. */

	rte_vhost_clr_inflight_desc_split(vsession->vid, vq_idx, virtqueue->last_used_idx, id);
	/* [한국어] inflight 마킹 클리어 — used 갱신까지 완료됐으므로 더 이상 inflight 아님. */

	virtqueue->used_req_cnt++;
	/* [한국어] coalescing window 카운트 증가 — used_signal이 임계 도달 시 IRQ 발사. */

	if (spdk_unlikely(spdk_interrupt_mode_is_enabled())) {
		/* [한국어] interrupt 모드면 즉시 IRQ 송신 — 폴러 없음. */
		if (virtqueue->vring.desc == NULL || vhost_vq_event_is_suppressed(virtqueue)) {
			return;
			/* [한국어] 큐 비활성 또는 게스트가 IRQ 억제 중이면 송신 안 함. */
		}

		vhost_vq_used_signal(vsession, virtqueue);
	}
}

void
vhost_vq_packed_ring_enqueue(struct spdk_vhost_session *vsession,
			     struct spdk_vhost_virtqueue *virtqueue,
			     uint16_t num_descs, uint16_t buffer_id,
			     uint32_t length, uint16_t inflight_head)
{
	struct vring_packed_desc *desc = &virtqueue->vring.desc_packed[virtqueue->last_used_idx];
	bool used, avail;

	SPDK_DEBUGLOG(vhost_ring,
		      "Queue %td - RING: buffer_id=%"PRIu16"\n",
		      virtqueue - vsession->virtqueue, buffer_id);

	/* When the descriptor is used, two flags in descriptor
	 * avail flag and used flag are set to equal
	 * and used flag value == used_wrap_counter.
	 */
	used = !!(desc->flags & VRING_DESC_F_USED);
	avail = !!(desc->flags & VRING_DESC_F_AVAIL);
	if (spdk_unlikely(used == virtqueue->packed.used_phase && used == avail)) {
		SPDK_ERRLOG("descriptor has been used before\n");
		return;
	}

	/* In used desc addr is unused and len specifies the buffer length
	 * that has been written to by the device.
	 */
	desc->addr = 0;
	desc->len = length;

	/* This bit specifies whether any data has been written by the device */
	if (length != 0) {
		desc->flags |= VRING_DESC_F_WRITE;
	}

	/* Buffer ID is included in the last descriptor in the list.
	 * The driver needs to keep track of the size of the list corresponding
	 * to each buffer ID.
	 */
	desc->id = buffer_id;

	/* A device MUST NOT make the descriptor used before buffer_id is
	 * written to the descriptor.
	 */
	spdk_smp_wmb();

	rte_vhost_set_last_inflight_io_packed(vsession->vid, virtqueue->vring_idx, inflight_head);
	/* To mark a desc as used, the device sets the F_USED bit in flags to match
	 * the internal Device ring wrap counter. It also sets the F_AVAIL bit to
	 * match the same value.
	 */
	if (virtqueue->packed.used_phase) {
		desc->flags |= VRING_DESC_F_AVAIL_USED;
	} else {
		desc->flags &= ~VRING_DESC_F_AVAIL_USED;
	}
	rte_vhost_clr_inflight_desc_packed(vsession->vid, virtqueue->vring_idx, inflight_head);

	vhost_log_used_vring_elem(vsession, virtqueue, virtqueue->last_used_idx);
	virtqueue->last_used_idx += num_descs;
	if (virtqueue->last_used_idx >= virtqueue->vring.size) {
		virtqueue->last_used_idx -= virtqueue->vring.size;
		virtqueue->packed.used_phase = !virtqueue->packed.used_phase;
	}

	virtqueue->used_req_cnt++;
}

bool
vhost_vq_packed_ring_is_avail(struct spdk_vhost_virtqueue *virtqueue)
{
	uint16_t flags = virtqueue->vring.desc_packed[virtqueue->last_avail_idx].flags;

	/* To mark a desc as available, the driver sets the F_AVAIL bit in flags
	 * to match the internal avail wrap counter. It also sets the F_USED bit to
	 * match the inverse value but it's not mandatory.
	 */
	return (!!(flags & VRING_DESC_F_AVAIL) == virtqueue->packed.avail_phase);
}

bool
vhost_vring_packed_desc_is_wr(struct vring_packed_desc *cur_desc)
{
	return (cur_desc->flags & VRING_DESC_F_WRITE) != 0;
}

bool
vhost_vring_inflight_desc_is_wr(spdk_vhost_inflight_desc *cur_desc)
{
	return (cur_desc->flags & VRING_DESC_F_WRITE) != 0;
}

int
vhost_vring_packed_desc_get_next(struct vring_packed_desc **desc, uint16_t *req_idx,
				 struct spdk_vhost_virtqueue *vq,
				 struct vring_packed_desc *desc_table,
				 uint32_t desc_table_size)
{
	if (desc_table != NULL) {
		/* When the desc_table isn't NULL means it's indirect and we get the next
		 * desc by req_idx and desc_table_size. The return value is NULL means
		 * we reach the last desc of this request.
		 */
		(*req_idx)++;
		if (*req_idx < desc_table_size) {
			*desc = &desc_table[*req_idx];
		} else {
			*desc = NULL;
		}
	} else {
		/* When the desc_table is NULL means it's non-indirect and we get the next
		 * desc by req_idx and F_NEXT in flags. The return value is NULL means
		 * we reach the last desc of this request. When return new desc
		 * we update the req_idx too.
		 */
		if (((*desc)->flags & VRING_DESC_F_NEXT) == 0) {
			*desc = NULL;
			return 0;
		}

		*req_idx = (*req_idx + 1) % vq->vring.size;
		*desc = &vq->vring.desc_packed[*req_idx];
	}

	return 0;
}

static int
vhost_vring_desc_payload_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
				uint16_t *iov_index, uintptr_t payload, uint64_t remaining)
{
	uintptr_t vva;
	uint64_t len;

	do {
		if (*iov_index >= SPDK_VHOST_IOVS_MAX) {
			SPDK_ERRLOG("SPDK_VHOST_IOVS_MAX(%d) reached\n", SPDK_VHOST_IOVS_MAX);
			return -1;
		}
		len = remaining;
		vva = (uintptr_t)rte_vhost_va_from_guest_pa(vsession->mem, payload, &len);
		if (vva == 0 || len == 0) {
			SPDK_ERRLOG("gpa_to_vva(%p) == NULL\n", (void *)payload);
			return -1;
		}
		iov[*iov_index].iov_base = (void *)vva;
		iov[*iov_index].iov_len = len;
		remaining -= len;
		payload += len;
		(*iov_index)++;
	} while (remaining);

	return 0;
}

int
vhost_vring_packed_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
			       uint16_t *iov_index, const struct vring_packed_desc *desc)
{
	return vhost_vring_desc_payload_to_iov(vsession, iov, iov_index,
					       desc->addr, desc->len);
}

int
vhost_vring_inflight_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
				 uint16_t *iov_index, const spdk_vhost_inflight_desc *desc)
{
	return vhost_vring_desc_payload_to_iov(vsession, iov, iov_index,
					       desc->addr, desc->len);
}

/* 1, Traverse the desc chain to get the buffer_id and return buffer_id as task_idx.
 * 2, Update the vq->last_avail_idx to point next available desc chain.
 * 3, Update the avail_wrap_counter if last_avail_idx overturn.
 */
uint16_t
vhost_vring_packed_desc_get_buffer_id(struct spdk_vhost_virtqueue *vq, uint16_t req_idx,
				      uint16_t *num_descs)
{
	struct vring_packed_desc *desc;
	uint16_t desc_head = req_idx;

	*num_descs = 1;

	desc =  &vq->vring.desc_packed[req_idx];
	if (!vhost_vring_packed_desc_is_indirect(desc)) {
		while ((desc->flags & VRING_DESC_F_NEXT) != 0) {
			req_idx = (req_idx + 1) % vq->vring.size;
			desc = &vq->vring.desc_packed[req_idx];
			(*num_descs)++;
		}
	}

	/* Queue Size doesn't have to be a power of 2
	 * Device maintains last_avail_idx so we can make sure
	 * the value is valid(0 ~ vring.size - 1)
	 */
	vq->last_avail_idx = (req_idx + 1) % vq->vring.size;
	if (vq->last_avail_idx < desc_head) {
		vq->packed.avail_phase = !vq->packed.avail_phase;
	}

	return desc->id;
}

int
vhost_vring_desc_get_next(struct vring_desc **desc,
			  struct vring_desc *desc_table, uint32_t desc_table_size)
{
	struct vring_desc *old_desc = *desc;
	uint16_t next_idx;

	if ((old_desc->flags & VRING_DESC_F_NEXT) == 0) {
		*desc = NULL;
		return 0;
	}

	next_idx = old_desc->next;
	if (spdk_unlikely(next_idx >= desc_table_size)) {
		*desc = NULL;
		return -1;
	}

	*desc = &desc_table[next_idx];
	return 0;
}

int
vhost_vring_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
			uint16_t *iov_index, const struct vring_desc *desc)
{
	return vhost_vring_desc_payload_to_iov(vsession, iov, iov_index,
					       desc->addr, desc->len);
}

/*
 * [한국어]
 * vhost_session_mem_region_calc - 한 메모리 region의 등록 범위(start/end/len)를 2MB 정렬로 계산.
 *
 * @previous_start: 직전 region의 시작 주소(in/out) — 같으면 한 페이지 건너뛰기.
 * @region: 입력 region.
 *
 * SPDK env(DPDK)는 hugepage 단위(2MB)로 메모리 매핑을 추적한다. 따라서 게스트 region을
 * 2MB 경계로 floor/ceil 정렬하여 등록한다. 같은 시작 주소가 또 등장하면 +2MB로 미뤄 중복 등록 방지.
 */
static inline void
vhost_session_mem_region_calc(uint64_t *previous_start, uint64_t *start, uint64_t *end,
			      uint64_t *len, struct rte_vhost_mem_region *region)
{
	*start = FLOOR_2MB(region->mmap_addr);
	/* [한국어] 시작 주소를 2MB 경계로 floor. */
	*end = CEIL_2MB(region->mmap_addr + region->mmap_size);
	/* [한국어] 끝 주소를 2MB 경계로 ceil. */
	if (*start == *previous_start) {
		*start += (size_t) VALUE_2MB;
		/* [한국어] 직전 region과 시작이 겹치면 한 페이지 건너뛰어 중복 회피. */
	}
	*previous_start = *start;
	*len = *end - *start;
}

/*
 * [한국어]
 * vhost_session_mem_register - 게스트 메모리 영역들을 SPDK env(DPDK)에 DMA 가능 영역으로 등록.
 *
 * @mem: rte_vhost가 채워준 게스트 메모리 영역 배열.
 *
 * 백엔드가 spdk_vtophys로 GPA→DMA addr 변환할 수 있게 메모리를 등록한다.
 * NVMe bdev이 게스트 buf로 직접 DMA할 때 필수.
 *
 * 호출 컨텍스트: 디바이스 thread (세션 시작 시).
 */
void
vhost_session_mem_register(struct rte_vhost_memory *mem)
{
	uint64_t start, end, len;
	uint32_t i;
	uint64_t previous_start = UINT64_MAX;
	/* [한국어] 첫 region에서 비교가 거짓이 되도록 큰 값으로 초기화. */


	for (i = 0; i < mem->nregions; i++) {
		vhost_session_mem_region_calc(&previous_start, &start, &end, &len, &mem->regions[i]);
		SPDK_INFOLOG(vhost, "Registering VM memory for vtophys translation - 0x%jx len:0x%jx\n",
			     start, len);

		if (spdk_mem_register((void *)start, len) != 0) {
			/* [한국어] DPDK env에 등록 — 이후 spdk_vtophys로 변환 가능. 실패해도 다음 region 시도. */
			SPDK_WARNLOG("Failed to register memory region %"PRIu32". Future vtophys translation might fail.\n",
				     i);
			continue;
		}
	}
}

/*
 * [한국어]
 * vhost_session_mem_unregister - 위에서 등록한 메모리 영역을 모두 해제.
 *
 * @mem: rte_vhost 메모리 영역 배열.
 *
 * 등록된 영역만 해제 — spdk_vtophys 결과로 미등록 region은 건너뜀.
 * 호출 컨텍스트: 세션 종료 또는 메모리 영역 변경 시.
 */
void
vhost_session_mem_unregister(struct rte_vhost_memory *mem)
{
	uint64_t start, end, len;
	uint32_t i;
	uint64_t previous_start = UINT64_MAX;

	for (i = 0; i < mem->nregions; i++) {
		vhost_session_mem_region_calc(&previous_start, &start, &end, &len, &mem->regions[i]);
		if (spdk_vtophys((void *) start, NULL) == SPDK_VTOPHYS_ERROR) {
			continue; /* region has not been registered */
			/* [한국어] vtophys가 변환 실패하면 미등록 region — 건너뛰기. */
		}

		spdk_mem_unregister((void *)start, len);
		/* [한국어] DPDK env에서 등록 해제. */
	}
}

static bool
vhost_memory_changed(struct rte_vhost_memory *new,
		     struct rte_vhost_memory *old)
{
	uint32_t i;

	if (new->nregions != old->nregions) {
		return true;
	}

	for (i = 0; i < new->nregions; ++i) {
		struct rte_vhost_mem_region *new_r = &new->regions[i];
		struct rte_vhost_mem_region *old_r = &old->regions[i];

		if (new_r->guest_phys_addr != old_r->guest_phys_addr) {
			return true;
		}
		if (new_r->size != old_r->size) {
			return true;
		}
		if (new_r->guest_user_addr != old_r->guest_user_addr) {
			return true;
		}
		if (new_r->mmap_addr != old_r->mmap_addr) {
			return true;
		}
		if (new_r->fd != old_r->fd) {
			return true;
		}
	}

	return false;
}

static int
vhost_register_memtable_if_required(struct spdk_vhost_session *vsession, int vid)
{
	struct rte_vhost_memory *new_mem;

	if (vhost_get_mem_table(vid, &new_mem) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get guest memory table\n", vid);
		return -1;
	}

	if (vsession->mem == NULL) {
		SPDK_INFOLOG(vhost, "Start to set memtable\n");
		vsession->mem = new_mem;
		vhost_session_mem_register(vsession->mem);
		return 0;
	}

	if (vhost_memory_changed(new_mem, vsession->mem)) {
		SPDK_INFOLOG(vhost, "Memtable is changed\n");
		vhost_session_mem_unregister(vsession->mem);
		free(vsession->mem);

		vsession->mem = new_mem;
		vhost_session_mem_register(vsession->mem);
		return 0;

	}

	SPDK_INFOLOG(vhost, "Memtable is unchanged\n");
	free(new_mem);
	return 0;
}

/*
 * [한국어]
 * _stop_session - 세션 종료 헬퍼 — 백엔드 stop 대기 + 큐 메타 DPDK에 반환.
 *
 * @vsession: 종료할 세션.
 * @return: 0=성공, 음수=백엔드 stop 실패.
 *
 * 1) 백엔드(blk/scsi)의 stop이 완료될 때까지 대기 (3초 타임아웃).
 * 2) 모든 큐에 대해 last_avail_idx/last_used_idx를 DPDK에 반환 — packed ring은 wrap 비트도 인코딩.
 * 3) max_queues=0으로 표시.
 *
 * 호출 컨텍스트: DPDK pthread (destroy_device 콜백 안).
 */
static int
_stop_session(struct spdk_vhost_session *vsession)
{
	struct spdk_vhost_virtqueue *q;
	int rc;
	uint16_t i;

	rc = vhost_user_wait_for_session_stop(vsession, SPDK_VHOST_SESSION_STOP_TIMEOUT_IN_SEC,
					      "stop session");
	/* [한국어] 백엔드 stop_session 콜백 완료 대기 (sem_wait 패턴). */
	if (rc != 0) {
		SPDK_ERRLOG("Couldn't stop device with vid %d.\n", vsession->vid);
		return rc;
	}

	for (i = 0; i < vsession->max_queues; i++) {
		q = &vsession->virtqueue[i];

		/* vring.desc and vring.desc_packed are in a union struct
		 * so q->vring.desc can replace q->vring.desc_packed.
		 */
		if (q->vring.desc == NULL) {
			continue;
			/* [한국어] 비활성 큐 스킵. */
		}

		/* Packed virtqueues support up to 2^15 entries each
		 * so left one bit can be used as wrap counter.
		 */
		if (q->packed.packed_ring) {
			/* [한국어] packed ring은 last_avail_idx 최상위 비트(15)에 wrap 인코딩 후 DPDK에 반환. */
			q->last_avail_idx = q->last_avail_idx |
					    ((uint16_t)q->packed.avail_phase << 15);
			q->last_used_idx = q->last_used_idx |
					   ((uint16_t)q->packed.used_phase << 15);
		}

		rte_vhost_set_vring_base(vsession->vid, i, q->last_avail_idx, q->last_used_idx);
		/* [한국어] DPDK에 마지막 인덱스 보고 — 다음 start 시 이 위치부터 재개. */
		q->vring.desc = NULL;
		/* [한국어] desc 포인터 NULL로 — 큐 비활성화 표시. */
	}
	vsession->max_queues = 0;

	return 0;
}

/*
 * [한국어]
 * new_connection - DPDK rte_vhost가 새 게스트 연결을 발견했을 때 호출하는 콜백.
 *
 * @vid: DPDK가 부여한 연결 ID.
 * @return: 0=성공, -1=실패.
 *
 * 처리 흐름:
 *   1) rte_vhost_get_ifname으로 UNIX 소켓 경로 → 디바이스 이름 추출.
 *   2) spdk_vhost_dev_find로 매칭되는 vdev 검색.
 *   3) backend session_ctx_size를 고려해 vsession + 백엔드 컨테이너 통합 할당.
 *   4) dpdk_sem 세마포어 초기화 (DPDK pthread ↔ SPDK thread 동기화용).
 *   5) user_dev->vsessions 리스트에 append (id 오름차순 정렬 유지 — foreach 구현 가정).
 *   6) DPDK 호환 hook 등록.
 *
 * 호출 컨텍스트: DPDK pthread (rte_vhost-internal). SPDK thread 아님.
 * 동기화: spdk_vhost_lock으로 vdev tree 보호, user_dev->lock으로 vsessions 리스트 보호.
 */
static int
new_connection(int vid)
{
	struct spdk_vhost_dev *vdev;
	/* [한국어] 매칭된 vhost 디바이스. */
	struct spdk_vhost_user_dev *user_dev;
	/* [한국어] vhost-user 컨텍스트. */
	struct spdk_vhost_session *vsession;
	/* [한국어] 새로 만들 세션. */
	size_t dev_dirname_len;
	/* [한국어] 디렉토리 prefix 길이. */
	char ifname[PATH_MAX];
	/* [한국어] DPDK가 알려주는 소켓 경로 임시 버퍼. */
	char *ctrlr_name;
	/* [한국어] prefix 제거 후 컨트롤러 이름. */

	if (rte_vhost_get_ifname(vid, ifname, PATH_MAX) < 0) {
		/* [한국어] vid에 해당하는 ifname(소켓 경로) 조회 실패. */
		SPDK_ERRLOG("Couldn't get a valid ifname for device with vid %d\n", vid);
		return -1;
	}

	ctrlr_name = &ifname[0];
	dev_dirname_len = strlen(g_vhost_user_dev_dirname);
	if (strncmp(ctrlr_name, g_vhost_user_dev_dirname, dev_dirname_len) == 0) {
		ctrlr_name += dev_dirname_len;
		/* [한국어] /var/tmp/ctrlr → ctrlr 형태로 prefix 제거. */
	}

	spdk_vhost_lock();
	vdev = spdk_vhost_dev_find(ctrlr_name);
	if (vdev == NULL) {
		SPDK_ERRLOG("Couldn't find device with vid %d to create connection for.\n", vid);
		spdk_vhost_unlock();
		return -1;
	}
	spdk_vhost_unlock();
	/* [한국어] vdev 포인터 획득 후 락 해제 — vdev 자체는 unregister 전까지 유효. */

	user_dev = to_user_dev(vdev);
	pthread_mutex_lock(&user_dev->lock);
	/* [한국어] 디바이스 단위 lock — vsessions 리스트 보호. */
	if (user_dev->registered == false) {
		SPDK_ERRLOG("Device %s is unregistered\n", ctrlr_name);
		pthread_mutex_unlock(&user_dev->lock);
		return -1;
	}

	/* We expect sessions inside user_dev->vsessions to be sorted in ascending
	 * order in regard of vsession->id. For now we always set id = vsessions_num++
	 * and append each session to the very end of the vsessions list.
	 * This is required for vhost_user_dev_foreach_session() to work.
	 */
	if (user_dev->vsessions_num == UINT_MAX) {
		/* [한국어] 카운터 overflow 방어. */
		pthread_mutex_unlock(&user_dev->lock);
		assert(false);
		return -EINVAL;
	}

	if (posix_memalign((void **)&vsession, SPDK_CACHE_LINE_SIZE, sizeof(*vsession) +
			   user_dev->user_backend->session_ctx_size)) {
		/* [한국어] base + 백엔드 컨텍스트를 한 덩어리로 캐시라인 정렬 할당. */
		SPDK_ERRLOG("vsession alloc failed\n");
		pthread_mutex_unlock(&user_dev->lock);
		return -1;
	}
	memset(vsession, 0, sizeof(*vsession) + user_dev->user_backend->session_ctx_size);
	/* [한국어] 0 클리어 — 모든 포인터 NULL, 카운터 0. */

	vsession->vdev = vdev;
	/* [한국어] 부모 vdev 역참조. */
	vsession->vid = vid;
	/* [한국어] DPDK 부여 vid 저장. */
	vsession->id = user_dev->vsessions_num++;
	/* [한국어] SPDK 자체 ID — 카운터 증가. */
	vsession->name = spdk_sprintf_alloc("%ss%u", vdev->name, vsession->vid);
	/* [한국어] "ctrlr_namesNN" 형식 이름 생성. */
	if (vsession->name == NULL) {
		SPDK_ERRLOG("vsession alloc failed\n");
		free(vsession);
		pthread_mutex_unlock(&user_dev->lock);
		return -1;
	}

	if (sem_init(&vsession->dpdk_sem, 0, 0) != 0) {
		/* [한국어] DPDK pthread ↔ SPDK thread 동기화 세마포어 초기화. value=0, pshared=0. */
		SPDK_ERRLOG("Failed to initialize semaphore for rte_vhost pthread.\n");
		free(vsession->name);
		free(vsession);
		pthread_mutex_unlock(&user_dev->lock);
		return -1;
	}

	vsession->started = false;
	vsession->starting = false;
	vsession->next_stats_check_time = 0;
	vsession->stats_check_interval = SPDK_VHOST_STATS_CHECK_INTERVAL_MS *
					 spdk_get_ticks_hz() / 1000UL;
	/* [한국어] coalescing 통계 검사 주기를 ticks로 변환 (10ms × ticks_per_sec / 1000). */
	TAILQ_INSERT_TAIL(&user_dev->vsessions, vsession, tailq);
	/* [한국어] 리스트 끝에 append — id 오름차순 유지. */
	vhost_session_install_rte_compat_hooks(vsession);
	/* [한국어] DPDK 버전별 콜백 hook 등록. */
	pthread_mutex_unlock(&user_dev->lock);

	return 0;
}

/*
 * [한국어]
 * vhost_user_session_start - 디바이스 thread에서 실행되는 세션 시작 메시지 콜백.
 *
 * @arg1: spdk_vhost_session*.
 *
 * start_device 콜백이 spdk_thread_send_msg로 enqueue한 메시지의 진입점.
 * backend->start_session(blk: vhost_blk_start, scsi: vhost_scsi_start)을 호출.
 *
 * 호출 컨텍스트: 디바이스 SPDK reactor thread.
 */
static void
vhost_user_session_start(void *arg1)
{
	struct spdk_vhost_session *vsession = arg1;
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vsession->vdev);
	const struct spdk_vhost_user_dev_backend *backend;
	int rc;

	SPDK_INFOLOG(vhost, "Starting new session for device %s with vid %d\n", vdev->name, vsession->vid);
	pthread_mutex_lock(&user_dev->lock);
	/* [한국어] 디바이스 lock — start_session 진행 중 다른 세션 변경 차단. */
	vsession->starting = false;
	/* [한국어] 과도기 플래그 해제 — start_session 후 started로 전환. */
	backend = user_dev->user_backend;
	rc = backend->start_session(vdev, vsession, NULL);
	/* [한국어] 백엔드별 시작 콜백 호출 (poller 등록 등). */
	if (rc == 0) {
		vsession->started = true;
		/* [한국어] 시작 성공 — I/O 처리 가능 상태. */
	}
	pthread_mutex_unlock(&user_dev->lock);
}

static int
set_device_vq_callfd(struct spdk_vhost_session *vsession, uint16_t qid)
{
	struct spdk_vhost_virtqueue *q;

	if (qid >= SPDK_VHOST_MAX_VQUEUES) {
		return -EINVAL;
	}

	q = &vsession->virtqueue[qid];
	/* vq isn't enabled yet */
	if (q->vring_idx != qid) {
		return 0;
	}

	/* vring.desc and vring.desc_packed are in a union struct
	 * so q->vring.desc can replace q->vring.desc_packed.
	 */
	if (q->vring.desc == NULL || q->vring.size == 0) {
		return 0;
	}

	/*
	 * Not sure right now but this look like some kind of QEMU bug and guest IO
	 * might be frozed without kicking all queues after live-migration. This look like
	 * the previous vhost instance failed to effectively deliver all interrupts before
	 * the GET_VRING_BASE message. This shouldn't harm guest since spurious interrupts
	 * should be ignored by guest virtio driver.
	 *
	 * Tested on QEMU 2.10.91 and 2.11.50.
	 *
	 * Make sure a successful call of
	 * `rte_vhost_vring_call` will happen
	 * after starting the device.
	 */
	q->used_req_cnt += 1;

	return 0;
}

static int
enable_device_vq(struct spdk_vhost_session *vsession, uint16_t qid)
{
	struct spdk_vhost_virtqueue *q;
	bool packed_ring;
	const struct spdk_vhost_user_dev_backend *backend;
	int rc;

	if (qid >= SPDK_VHOST_MAX_VQUEUES) {
		return -EINVAL;
	}

	q = &vsession->virtqueue[qid];
	memset(q, 0, sizeof(*q));
	packed_ring = ((vsession->negotiated_features & (1ULL << VIRTIO_F_RING_PACKED)) != 0);

	q->vsession = vsession;
	q->vring_idx = -1;
	if (rte_vhost_get_vhost_vring(vsession->vid, qid, &q->vring)) {
		return 0;
	}
	q->vring_idx = qid;
	rte_vhost_get_vhost_ring_inflight(vsession->vid, qid, &q->vring_inflight);

	/* vring.desc and vring.desc_packed are in a union struct
	 * so q->vring.desc can replace q->vring.desc_packed.
	 */
	if (q->vring.desc == NULL || q->vring.size == 0) {
		return 0;
	}

	if (rte_vhost_get_vring_base(vsession->vid, qid, &q->last_avail_idx, &q->last_used_idx)) {
		q->vring.desc = NULL;
		return 0;
	}

	backend = to_user_dev(vsession->vdev)->user_backend;
	rc = backend->alloc_vq_tasks(vsession, qid);
	if (rc) {
		return rc;
	}

	/*
	 * This shouldn't harm guest since spurious interrupts should be ignored by
	 * guest virtio driver.
	 *
	 * Make sure a successful call of `rte_vhost_vring_call` will happen after
	 * restarting the device.
	 */
	if (vsession->needs_restart) {
		q->used_req_cnt += 1;
	}

	if (packed_ring) {
		/* Since packed ring flag is already negotiated between SPDK and VM, VM doesn't
		 * restore `last_avail_idx` and `last_used_idx` for packed ring, so use the
		 * inflight mem to restore the `last_avail_idx` and `last_used_idx`.
		 */
		rte_vhost_get_vring_base_from_inflight(vsession->vid, qid, &q->last_avail_idx,
						       &q->last_used_idx);

		/* Packed virtqueues support up to 2^15 entries each
		 * so left one bit can be used as wrap counter.
		 */
		q->packed.avail_phase = q->last_avail_idx >> 15;
		q->last_avail_idx = q->last_avail_idx & 0x7FFF;
		q->packed.used_phase = q->last_used_idx >> 15;
		q->last_used_idx = q->last_used_idx & 0x7FFF;

		if (!spdk_interrupt_mode_is_enabled()) {
			/* Disable I/O submission notifications, we'll be polling. */
			q->vring.device_event->flags = VRING_PACKED_EVENT_FLAG_DISABLE;
		} else {
			/* Enable I/O submission notifications, we'll be interrupting. */
			q->vring.device_event->flags = VRING_PACKED_EVENT_FLAG_ENABLE;
		}
	} else {
		if (!spdk_interrupt_mode_is_enabled()) {
			/* Disable I/O submission notifications, we'll be polling. */
			q->vring.used->flags = VRING_USED_F_NO_NOTIFY;
		} else {
			/* Enable I/O submission notifications, we'll be interrupting. */
			q->vring.used->flags = 0;
		}
	}

	if (backend->enable_vq) {
		rc = backend->enable_vq(vsession, q);
		if (rc) {
			return rc;
		}
	}

	q->packed.packed_ring = packed_ring;
	vsession->max_queues = spdk_max(vsession->max_queues, qid + 1);

	return 0;
}

static int
start_device(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;
	int rc = 0;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		return -1;
	}
	vdev = vsession->vdev;
	user_dev = to_user_dev(vdev);

	pthread_mutex_lock(&user_dev->lock);
	if (vsession->started) {
		/* already started, nothing to do */
		goto out;
	}

	if (!vsession->mem) {
		rc = -1;
		SPDK_ERRLOG("Session %s doesn't set memory table yet\n", vsession->name);
		goto out;
	}

	vsession->starting = true;
	SPDK_INFOLOG(vhost, "Session %s is scheduled to start\n", vsession->name);
	vhost_user_session_set_coalescing(vdev, vsession, NULL);
	spdk_thread_send_msg(vdev->thread, vhost_user_session_start, vsession);

out:
	pthread_mutex_unlock(&user_dev->lock);
	return rc;
}

static void
stop_device(int vid)
{
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		return;
	}
	user_dev = to_user_dev(vsession->vdev);

	pthread_mutex_lock(&user_dev->lock);
	if (!vsession->started && !vsession->starting) {
		pthread_mutex_unlock(&user_dev->lock);
		/* already stopped, nothing to do */
		return;
	}

	_stop_session(vsession);
	pthread_mutex_unlock(&user_dev->lock);
}

static void
destroy_connection(int vid)
{
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		return;
	}
	user_dev = to_user_dev(vsession->vdev);

	pthread_mutex_lock(&user_dev->lock);
	if (vsession->started || vsession->starting) {
		if (_stop_session(vsession) != 0) {
			pthread_mutex_unlock(&user_dev->lock);
			return;
		}
	}

	if (vsession->mem) {
		vhost_session_mem_unregister(vsession->mem);
		free(vsession->mem);
	}

	TAILQ_REMOVE(&to_user_dev(vsession->vdev)->vsessions, vsession, tailq);
	sem_destroy(&vsession->dpdk_sem);
	free(vsession->name);
	free(vsession);
	pthread_mutex_unlock(&user_dev->lock);
}

static const struct rte_vhost_device_ops g_spdk_vhost_ops = {
	.new_device =  start_device,
	.destroy_device = stop_device,
	.new_connection = new_connection,
	.destroy_connection = destroy_connection,
};

static struct spdk_vhost_session *
vhost_session_find_by_id(struct spdk_vhost_dev *vdev, unsigned id)
{
	struct spdk_vhost_session *vsession;

	TAILQ_FOREACH(vsession, &to_user_dev(vdev)->vsessions, tailq) {
		if (vsession->id == id) {
			return vsession;
		}
	}

	return NULL;
}

struct spdk_vhost_session *
vhost_session_find_by_vid(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;

	spdk_vhost_lock();
	for (vdev = spdk_vhost_dev_next(NULL); vdev != NULL;
	     vdev = spdk_vhost_dev_next(vdev)) {
		user_dev = to_user_dev(vdev);

		pthread_mutex_lock(&user_dev->lock);
		TAILQ_FOREACH(vsession, &user_dev->vsessions, tailq) {
			if (vsession->vid == vid) {
				pthread_mutex_unlock(&user_dev->lock);
				spdk_vhost_unlock();
				return vsession;
			}
		}
		pthread_mutex_unlock(&user_dev->lock);
	}
	spdk_vhost_unlock();

	return NULL;
}

static void
vhost_session_wait_for_semaphore(struct spdk_vhost_session *vsession, int timeout_sec,
				 const char *errmsg)
{
	struct timespec timeout;
	int rc;

	clock_gettime(CLOCK_REALTIME, &timeout);
	timeout.tv_sec += timeout_sec;
	rc = sem_timedwait(&vsession->dpdk_sem, &timeout);
	if (rc != 0) {
		SPDK_ERRLOG("Timeout waiting for event: %s.\n", errmsg);
		sem_wait(&vsession->dpdk_sem);
	}
}

void
vhost_user_session_stop_done(struct spdk_vhost_session *vsession, int response)
{
	if (response == 0) {
		vsession->started = false;
	}

	vsession->dpdk_response = response;
	sem_post(&vsession->dpdk_sem);
}

static void
vhost_user_session_stop_event(void *arg1)
{
	struct vhost_session_fn_ctx *ctx = arg1;
	struct spdk_vhost_dev *vdev = ctx->vdev;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);
	struct spdk_vhost_session *vsession;

	if (pthread_mutex_trylock(&user_dev->lock) != 0) {
		spdk_thread_send_msg(spdk_get_thread(), vhost_user_session_stop_event, arg1);
		return;
	}

	vsession = vhost_session_find_by_id(vdev, ctx->vsession_id);
	user_dev->user_backend->stop_session(vdev, vsession, NULL);
	pthread_mutex_unlock(&user_dev->lock);
}

static int
vhost_user_wait_for_session_stop(struct spdk_vhost_session *vsession,
				 unsigned timeout_sec, const char *errmsg)
{
	struct vhost_session_fn_ctx ev_ctx = {0};
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);

	ev_ctx.vdev = vdev;
	ev_ctx.vsession_id = vsession->id;

	spdk_thread_send_msg(vdev->thread, vhost_user_session_stop_event, &ev_ctx);

	pthread_mutex_unlock(&user_dev->lock);
	vhost_session_wait_for_semaphore(vsession, timeout_sec, errmsg);
	pthread_mutex_lock(&user_dev->lock);

	return vsession->dpdk_response;
}

static void
foreach_session_finish_cb(void *arg1)
{
	struct vhost_session_fn_ctx *ev_ctx = arg1;
	struct spdk_vhost_dev *vdev = ev_ctx->vdev;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);

	if (pthread_mutex_trylock(&user_dev->lock) != 0) {
		spdk_thread_send_msg(spdk_get_thread(),
				     foreach_session_finish_cb, arg1);
		return;
	}

	assert(user_dev->pending_async_op_num > 0);
	user_dev->pending_async_op_num--;
	if (ev_ctx->cpl_fn != NULL) {
		ev_ctx->cpl_fn(vdev, ev_ctx->user_ctx);
	}

	pthread_mutex_unlock(&user_dev->lock);
	free(ev_ctx);
}

static void
foreach_session(void *arg1)
{
	struct vhost_session_fn_ctx *ev_ctx = arg1;
	struct spdk_vhost_dev *vdev = ev_ctx->vdev;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);
	struct spdk_vhost_session *vsession;
	int rc;

	if (pthread_mutex_trylock(&user_dev->lock) != 0) {
		spdk_thread_send_msg(spdk_get_thread(), foreach_session, arg1);
		return;
	}

	TAILQ_FOREACH(vsession, &user_dev->vsessions, tailq) {
		rc = ev_ctx->cb_fn(vdev, vsession, ev_ctx->user_ctx);
		if (rc < 0) {
			goto out;
		}
	}

out:
	pthread_mutex_unlock(&user_dev->lock);
	spdk_thread_send_msg(g_vhost_user_init_thread, foreach_session_finish_cb, arg1);
}

void
vhost_user_dev_foreach_session(struct spdk_vhost_dev *vdev,
			       spdk_vhost_session_fn fn,
			       spdk_vhost_dev_fn cpl_fn,
			       void *arg)
{
	struct vhost_session_fn_ctx *ev_ctx;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);

	ev_ctx = calloc(1, sizeof(*ev_ctx));
	if (ev_ctx == NULL) {
		SPDK_ERRLOG("Failed to alloc vhost event.\n");
		assert(false);
		return;
	}

	ev_ctx->vdev = vdev;
	ev_ctx->cb_fn = fn;
	ev_ctx->cpl_fn = cpl_fn;
	ev_ctx->user_ctx = arg;

	pthread_mutex_lock(&user_dev->lock);
	assert(user_dev->pending_async_op_num < UINT32_MAX);
	user_dev->pending_async_op_num++;
	pthread_mutex_unlock(&user_dev->lock);

	spdk_thread_send_msg(vdev->thread, foreach_session, ev_ctx);
}

void
vhost_user_session_set_interrupt_mode(struct spdk_vhost_session *vsession, bool interrupt_mode)
{
	uint16_t i;
	int rc = 0;

	for (i = 0; i < vsession->max_queues; i++) {
		struct spdk_vhost_virtqueue *q = &vsession->virtqueue[i];
		uint64_t num_events = 1;

		/* vring.desc and vring.desc_packed are in a union struct
		 * so q->vring.desc can replace q->vring.desc_packed.
		 */
		if (q->vring.desc == NULL || q->vring.size == 0) {
			continue;
		}

		if (interrupt_mode) {

			/* In case of race condition, always kick vring when switch to intr */
			rc = write(q->vring.kickfd, &num_events, sizeof(num_events));
			if (rc < 0) {
				SPDK_ERRLOG("failed to kick vring: %s.\n", spdk_strerror(errno));
			}
		}
	}
}

static int
extern_vhost_pre_msg_handler(int vid, void *_msg)
{
	struct vhost_user_msg *msg = _msg;
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Received a message to uninitialized session (vid %d).\n", vid);
		assert(false);
		return RTE_VHOST_MSG_RESULT_ERR;
	}
	user_dev = to_user_dev(vsession->vdev);

	switch (msg->request) {
	case VHOST_USER_GET_VRING_BASE:
		pthread_mutex_lock(&user_dev->lock);
		if (vsession->started || vsession->starting) {
			pthread_mutex_unlock(&user_dev->lock);
			g_spdk_vhost_ops.destroy_device(vid);
			break;
		}
		pthread_mutex_unlock(&user_dev->lock);
		break;
	case VHOST_USER_SET_MEM_TABLE:
		pthread_mutex_lock(&user_dev->lock);
		if (vsession->started || vsession->starting) {
			vsession->original_max_queues = vsession->max_queues;
			pthread_mutex_unlock(&user_dev->lock);
			g_spdk_vhost_ops.destroy_device(vid);
			vsession->needs_restart = true;
			break;
		}
		pthread_mutex_unlock(&user_dev->lock);
		break;
	case VHOST_USER_GET_CONFIG: {
		int rc = 0;

		pthread_mutex_lock(&user_dev->lock);
		if (vsession->vdev->backend->vhost_get_config) {
			rc = vsession->vdev->backend->vhost_get_config(vsession->vdev,
					msg->payload.cfg.region, msg->payload.cfg.size);
			if (rc != 0) {
				msg->size = 0;
			}
		}
		pthread_mutex_unlock(&user_dev->lock);

		return RTE_VHOST_MSG_RESULT_REPLY;
	}
	case VHOST_USER_SET_CONFIG: {
		int rc = 0;

		pthread_mutex_lock(&user_dev->lock);
		if (vsession->vdev->backend->vhost_set_config) {
			rc = vsession->vdev->backend->vhost_set_config(vsession->vdev,
					msg->payload.cfg.region, msg->payload.cfg.offset,
					msg->payload.cfg.size, msg->payload.cfg.flags);
		}
		pthread_mutex_unlock(&user_dev->lock);

		return rc == 0 ? RTE_VHOST_MSG_RESULT_OK : RTE_VHOST_MSG_RESULT_ERR;
	}
	default:
		break;
	}

	return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
}

static int
extern_vhost_post_msg_handler(int vid, void *_msg)
{
	struct vhost_user_msg *msg = _msg;
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;
	uint16_t qid;
	int rc;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Received a message to uninitialized session (vid %d).\n", vid);
		assert(false);
		return RTE_VHOST_MSG_RESULT_ERR;
	}
	user_dev = to_user_dev(vsession->vdev);

	switch (msg->request) {
	case VHOST_USER_SET_FEATURES:
		rc = vhost_get_negotiated_features(vid, &vsession->negotiated_features);
		if (rc) {
			SPDK_ERRLOG("vhost device %d: Failed to get negotiated driver features\n", vid);
			return RTE_VHOST_MSG_RESULT_ERR;
		}
		break;
	case VHOST_USER_SET_VRING_CALL:
		qid = ((uint16_t)msg->payload.u64) & VHOST_USER_VRING_IDX_MASK;
		rc = set_device_vq_callfd(vsession, qid);
		if (rc) {
			return RTE_VHOST_MSG_RESULT_ERR;
		}
		break;
	case VHOST_USER_SET_VRING_KICK:
		qid = ((uint16_t)msg->payload.u64) & VHOST_USER_VRING_IDX_MASK;
		rc = enable_device_vq(vsession, qid);
		if (rc) {
			return RTE_VHOST_MSG_RESULT_ERR;
		}

		/* vhost-user spec tells us to start polling a queue after receiving
		 * its SET_VRING_KICK message. Let's do it!
		 */
		pthread_mutex_lock(&user_dev->lock);
		if (!vsession->started && !vsession->starting) {
			pthread_mutex_unlock(&user_dev->lock);
			g_spdk_vhost_ops.new_device(vid);
			return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
		}
		pthread_mutex_unlock(&user_dev->lock);
		break;
	case VHOST_USER_SET_MEM_TABLE:
	case VHOST_USER_ADD_MEM_REG:
		vhost_register_memtable_if_required(vsession, vid);
		pthread_mutex_lock(&user_dev->lock);
		if (vsession->needs_restart) {
			pthread_mutex_unlock(&user_dev->lock);
			for (qid = 0; qid < vsession->original_max_queues; qid++) {
				enable_device_vq(vsession, qid);
			}
			vsession->original_max_queues = 0;
			vsession->needs_restart = false;
			g_spdk_vhost_ops.new_device(vid);
			break;
		}
		pthread_mutex_unlock(&user_dev->lock);
		break;
	default:
		break;
	}

	return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
}

struct rte_vhost_user_extern_ops g_spdk_extern_vhost_ops = {
	.pre_msg_handle = extern_vhost_pre_msg_handler,
	.post_msg_handle = extern_vhost_post_msg_handler,
};

void
vhost_session_install_rte_compat_hooks(struct spdk_vhost_session *vsession)
{
	int rc;

	rc = rte_vhost_extern_callback_register(vsession->vid, &g_spdk_extern_vhost_ops, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("rte_vhost_extern_callback_register() failed for vid = %d\n",
			    vsession->vid);
		return;
	}
}

int
vhost_register_unix_socket(const char *path, const char *ctrl_name,
			   uint64_t virtio_features, uint64_t disabled_features, uint64_t protocol_features)
{
	struct stat file_stat;
	uint64_t features = 0;
	uint64_t flags = 0;

	/* Register vhost driver to handle vhost messages. */
	if (stat(path, &file_stat) != -1) {
		if (!S_ISSOCK(file_stat.st_mode)) {
			SPDK_ERRLOG("Cannot create a domain socket at path \"%s\": "
				    "The file already exists and is not a socket.\n",
				    path);
			return -EIO;
		} else if (unlink(path) != 0) {
			SPDK_ERRLOG("Cannot create a domain socket at path \"%s\": "
				    "The socket already exists and failed to unlink.\n",
				    path);
			return -EIO;
		}
	}

	flags = spdk_iommu_is_enabled() ? 0 : RTE_VHOST_USER_ASYNC_COPY;
	if (rte_vhost_driver_register(path, flags) != 0) {
		SPDK_ERRLOG("Could not register controller %s with vhost library\n", ctrl_name);
		SPDK_ERRLOG("Check if domain socket %s already exists\n", path);
		return -EIO;
	}
	if (rte_vhost_driver_set_features(path, virtio_features) ||
	    rte_vhost_driver_disable_features(path, disabled_features)) {
		SPDK_ERRLOG("Couldn't set vhost features for controller %s\n", ctrl_name);

		rte_vhost_driver_unregister(path);
		return -EIO;
	}

	if (rte_vhost_driver_callback_register(path, &g_spdk_vhost_ops) != 0) {
		rte_vhost_driver_unregister(path);
		SPDK_ERRLOG("Couldn't register callbacks for controller %s\n", ctrl_name);
		return -EIO;
	}

	rte_vhost_driver_get_protocol_features(path, &features);
	features |= protocol_features;
	rte_vhost_driver_set_protocol_features(path, features);

	if (rte_vhost_driver_start(path) != 0) {
		SPDK_ERRLOG("Failed to start vhost driver for controller %s (%d): %s\n",
			    ctrl_name, errno, spdk_strerror(errno));
		rte_vhost_driver_unregister(path);
		return -EIO;
	}

	return 0;
}

int
vhost_get_mem_table(int vid, struct rte_vhost_memory **mem)
{
	return rte_vhost_get_mem_table(vid, mem);
}

int
vhost_driver_unregister(const char *path)
{
	return rte_vhost_driver_unregister(path);
}

int
vhost_get_negotiated_features(int vid, uint64_t *negotiated_features)
{
	return rte_vhost_get_negotiated_features(vid, negotiated_features);
}

int
vhost_user_dev_set_coalescing(struct spdk_vhost_user_dev *user_dev, uint32_t delay_base_us,
			      uint32_t iops_threshold)
{
	uint64_t delay_time_base = delay_base_us * spdk_get_ticks_hz() / 1000000ULL;
	uint32_t io_rate = iops_threshold * SPDK_VHOST_STATS_CHECK_INTERVAL_MS / 1000U;

	if (delay_time_base >= UINT32_MAX) {
		SPDK_ERRLOG("Delay time of %"PRIu32" is to big\n", delay_base_us);
		return -EINVAL;
	} else if (io_rate == 0) {
		SPDK_ERRLOG("IOPS rate of %"PRIu32" is too low. Min is %u\n", io_rate,
			    1000U / SPDK_VHOST_STATS_CHECK_INTERVAL_MS);
		return -EINVAL;
	}

	user_dev->coalescing_delay_us = delay_base_us;
	user_dev->coalescing_iops_threshold = iops_threshold;
	return 0;
}

int
vhost_user_session_set_coalescing(struct spdk_vhost_dev *vdev,
				  struct spdk_vhost_session *vsession, void *ctx)
{
	vsession->coalescing_delay_time_base =
		to_user_dev(vdev)->coalescing_delay_us * spdk_get_ticks_hz() / 1000000ULL;
	vsession->coalescing_io_rate_threshold =
		to_user_dev(vdev)->coalescing_iops_threshold * SPDK_VHOST_STATS_CHECK_INTERVAL_MS / 1000U;
	return 0;
}

int
vhost_user_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			  uint32_t iops_threshold)
{
	int rc;

	rc = vhost_user_dev_set_coalescing(to_user_dev(vdev), delay_base_us, iops_threshold);
	if (rc != 0) {
		return rc;
	}

	vhost_user_dev_foreach_session(vdev, vhost_user_session_set_coalescing, NULL, NULL);

	return 0;
}

void
vhost_user_get_coalescing(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			  uint32_t *iops_threshold)
{
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);

	if (delay_base_us) {
		*delay_base_us = user_dev->coalescing_delay_us;
	}

	if (iops_threshold) {
		*iops_threshold = user_dev->coalescing_iops_threshold;
	}
}

int
spdk_vhost_set_socket_path(const char *basename)
{
	int ret;

	if (basename && strlen(basename) > 0) {
		ret = snprintf(g_vhost_user_dev_dirname, sizeof(g_vhost_user_dev_dirname) - 2, "%s", basename);
		if (ret <= 0) {
			return -EINVAL;
		}
		if ((size_t)ret >= sizeof(g_vhost_user_dev_dirname) - 2) {
			SPDK_ERRLOG("Char dev dir path length %d is too long\n", ret);
			return -EINVAL;
		}

		if (g_vhost_user_dev_dirname[ret - 1] != '/') {
			g_vhost_user_dev_dirname[ret] = '/';
			g_vhost_user_dev_dirname[ret + 1]  = '\0';
		}
	}

	return 0;
}

static void
vhost_dev_thread_exit(void *arg1)
{
	spdk_thread_exit(spdk_get_thread());
}

static bool g_vhost_user_started = false;

int
vhost_user_dev_init(struct spdk_vhost_dev *vdev, const char *name,
		    struct spdk_cpuset *cpumask, const struct spdk_vhost_user_dev_backend *user_backend)
{
	char path[PATH_MAX];
	struct spdk_vhost_user_dev *user_dev;

	if (snprintf(path, sizeof(path), "%s%s", g_vhost_user_dev_dirname, name) >= (int)sizeof(path)) {
		SPDK_ERRLOG("Resulting socket path for controller %s is too long: %s%s\n",
			    name, g_vhost_user_dev_dirname, name);
		return -EINVAL;
	}

	vdev->path = strdup(path);
	if (vdev->path == NULL) {
		return -EIO;
	}

	user_dev = calloc(1, sizeof(*user_dev));
	if (user_dev == NULL) {
		free(vdev->path);
		return -ENOMEM;
	}
	vdev->ctxt = user_dev;

	vdev->thread = spdk_thread_create(vdev->name, cpumask);
	if (vdev->thread == NULL) {
		free(user_dev);
		free(vdev->path);
		SPDK_ERRLOG("Failed to create thread for vhost controller %s.\n", name);
		return -EIO;
	}

	user_dev->user_backend = user_backend;
	user_dev->vdev = vdev;
	user_dev->registered = true;
	TAILQ_INIT(&user_dev->vsessions);
	pthread_mutex_init(&user_dev->lock, NULL);

	vhost_user_dev_set_coalescing(user_dev, SPDK_VHOST_COALESCING_DELAY_BASE_US,
				      SPDK_VHOST_VQ_IOPS_COALESCING_THRESHOLD);

	return 0;
}

int
vhost_user_dev_start(struct spdk_vhost_dev *vdev)
{
	return vhost_register_unix_socket(vdev->path, vdev->name, vdev->virtio_features,
					  vdev->disabled_features,
					  vdev->protocol_features);
}

int
vhost_user_dev_create(struct spdk_vhost_dev *vdev, const char *name, struct spdk_cpuset *cpumask,
		      const struct spdk_vhost_user_dev_backend *user_backend, bool delay)
{
	int rc;
	struct spdk_vhost_user_dev *user_dev;

	rc = vhost_user_dev_init(vdev, name, cpumask, user_backend);
	if (rc != 0) {
		return rc;
	}

	if (delay == false) {
		rc = vhost_user_dev_start(vdev);
		if (rc != 0) {
			user_dev = to_user_dev(vdev);
			spdk_thread_send_msg(vdev->thread, vhost_dev_thread_exit, NULL);
			pthread_mutex_destroy(&user_dev->lock);
			free(user_dev);
			free(vdev->path);
		}
	}

	return rc;
}


bool
vhost_user_dev_busy(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);

	if (pthread_mutex_trylock(&user_dev->lock) != 0) {
		return true;
	}

	/* This is the case that uses RPC call `vhost_delete_controller` while VM is connected */
	if (!TAILQ_EMPTY(&user_dev->vsessions) && g_vhost_user_started) {
		SPDK_ERRLOG("Controller %s has still valid connection.\n", vdev->name);
		pthread_mutex_unlock(&user_dev->lock);
		return true;
	}
	pthread_mutex_unlock(&user_dev->lock);
	return false;
}

int
vhost_user_dev_unregister(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);
	struct spdk_vhost_session *vsession, *tmp_vsession;

	if (pthread_mutex_trylock(&user_dev->lock) != 0) {
		return -EBUSY;
	}

	if (user_dev->pending_async_op_num) {
		pthread_mutex_unlock(&user_dev->lock);
		return -EBUSY;
	}

	/* This is the case that uses RPC call `vhost_delete_controller` while VM is connected */
	if (!TAILQ_EMPTY(&user_dev->vsessions) && g_vhost_user_started) {
		SPDK_ERRLOG("Controller %s has still valid connection.\n", vdev->name);
		pthread_mutex_unlock(&user_dev->lock);
		return -EBUSY;
	}

	/* This is the case that quits the subsystem while VM is connected, the VM
	 * should be stopped by the shutdown thread.
	 */
	if (!g_vhost_user_started) {
		TAILQ_FOREACH_SAFE(vsession, &user_dev->vsessions, tailq, tmp_vsession) {
			assert(vsession->started == false);
			TAILQ_REMOVE(&user_dev->vsessions, vsession, tailq);
			if (vsession->mem) {
				vhost_session_mem_unregister(vsession->mem);
				free(vsession->mem);
			}
			sem_destroy(&vsession->dpdk_sem);
			free(vsession->name);
			free(vsession);
		}
	}

	user_dev->registered = false;
	pthread_mutex_unlock(&user_dev->lock);

	/* There are no valid connections now, and it's not an error if the domain
	 * socket was already removed by shutdown thread.
	 */
	vhost_driver_unregister(vdev->path);

	spdk_thread_send_msg(vdev->thread, vhost_dev_thread_exit, NULL);
	pthread_mutex_destroy(&user_dev->lock);

	free(user_dev);
	free(vdev->path);

	return 0;
}

int
vhost_user_init(void)
{
	size_t len;

	if (g_vhost_user_started) {
		return 0;
	}

	if (g_vhost_user_dev_dirname[0] == '\0') {
		if (getcwd(g_vhost_user_dev_dirname, sizeof(g_vhost_user_dev_dirname) - 1) == NULL) {
			SPDK_ERRLOG("getcwd failed (%d): %s\n", errno, spdk_strerror(errno));
			return -1;
		}

		len = strlen(g_vhost_user_dev_dirname);
		if (g_vhost_user_dev_dirname[len - 1] != '/') {
			g_vhost_user_dev_dirname[len] = '/';
			g_vhost_user_dev_dirname[len + 1] = '\0';
		}
	}

	g_vhost_user_started = true;

	g_vhost_user_init_thread = spdk_get_thread();
	assert(g_vhost_user_init_thread != NULL);

	return 0;
}

static void
vhost_user_session_shutdown_on_init(void *vhost_cb)
{
	spdk_vhost_fini_cb fn = vhost_cb;

	fn();
}

static void *
vhost_user_session_shutdown(void *vhost_cb)
{
	struct spdk_vhost_dev *vdev = NULL;
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;
	int ret;

	for (vdev = spdk_vhost_dev_next(NULL); vdev != NULL;
	     vdev = spdk_vhost_dev_next(vdev)) {
		user_dev = to_user_dev(vdev);
		ret = 0;
		pthread_mutex_lock(&user_dev->lock);
		TAILQ_FOREACH(vsession, &user_dev->vsessions, tailq) {
			if (vsession->started || vsession->starting) {
				ret += _stop_session(vsession);
			}
		}
		pthread_mutex_unlock(&user_dev->lock);
		if (ret == 0) {
			vhost_driver_unregister(vdev->path);
		}
	}

	SPDK_INFOLOG(vhost, "Exiting\n");
	spdk_thread_send_msg(g_vhost_user_init_thread, vhost_user_session_shutdown_on_init, vhost_cb);
	return NULL;
}

void
vhost_user_fini(spdk_vhost_fini_cb vhost_cb)
{
	pthread_t tid;
	int rc;

	if (!g_vhost_user_started) {
		vhost_cb();
		return;
	}

	g_vhost_user_started = false;

	/* rte_vhost API for removing sockets is not asynchronous. Since it may call SPDK
	 * ops for stopping a device or removing a connection, we need to call it from
	 * a separate thread to avoid deadlock.
	 */
	rc = pthread_create(&tid, NULL, &vhost_user_session_shutdown, vhost_cb);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to start session shutdown thread (%d): %s\n", rc, spdk_strerror(rc));
		abort();
	}
	pthread_detach(tid);
}

void
vhost_session_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;

	user_dev = to_user_dev(vdev);
	pthread_mutex_lock(&user_dev->lock);
	TAILQ_FOREACH(vsession, &user_dev->vsessions, tailq) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint32(w, "vid", vsession->vid);
		spdk_json_write_named_uint32(w, "id", vsession->id);
		spdk_json_write_named_string(w, "name", vsession->name);
		spdk_json_write_named_bool(w, "started", vsession->started);
		spdk_json_write_named_uint32(w, "max_queues", vsession->max_queues);
		spdk_json_write_named_uint32(w, "inflight_task_cnt", vsession->task_cnt);
		spdk_json_write_object_end(w);
	}
	pthread_mutex_unlock(&user_dev->lock);
}
