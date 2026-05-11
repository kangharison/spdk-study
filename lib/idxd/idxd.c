/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] IDXD/DSA 드라이버 코어 구현 (idxd.c)
 *
 * === 파일의 역할 ===
 * Intel DSA(Data Streaming Accelerator) / IAA(In-Memory Analytics Accelerator) 디바이스를
 * SPDK가 사용하기 위한 백엔드-중립 드라이버 코어다. 두 백엔드("user" — idxd_user.c, "kernel" —
 * idxd_kernel.c) 중 어느 것이 활성화되어 있든 동일한 공개 API(spdk_idxd_*)를 제공한다.
 * 본 파일이 책임지는 일은 (1) 채널(spdk_idxd_io_channel) 생성/파괴와 디스크립터/완료 메모리 풀 셋업,
 * (2) 다양한 op(memmove, dualcast, compare, fill, crc32c, copy_crc, compress/decompress, dif_check/insert/strip,
 * dix_generate, raw_desc)에 대한 디스크립터 빌드 + 제출, (3) batch 관리(여러 op를 하나의 batch 디스크립터로 묶음),
 * (4) iovec/페이지 분할에 따른 vtophys segmenting, (5) 완료 폴링(spdk_idxd_process_events)이다.
 * 디스크립터 제출은 _submit_to_hw()에서 _spdk_wmb 후 movdir64b로 portal에 64B 한 번에 기록 — DSA spec
 * Section 4의 WQ_DEDICATED 모드.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK accel framework(lib/accel + module/accel/*)가 IDXD 모듈을 통해 본 코어의 spdk_idxd_*
 * API를 호출한다. 그 위로는 bdev 레이어가 accel을 통해 copy/crc/dif 가속 요청을 보내며, 다시 그 위로
 * NVMe-oF / blobstore / NVMe bdev 등이 데이터 무브를 수행한다.
 *
 * 호출 체인 (제출):
 *   accel_idxd 모듈 → spdk_idxd_submit_copy/fill/crc32c/...
 *     → _idxd_setup_batch (chan->batch가 없으면 batch_pool에서 하나 가져와 시작)
 *     → _idxd_prep_batch_cmd (batch->user_desc[index]에 디스크립터 prep, op->batch=batch, parent 체이닝)
 *     → 반복 (iovec * 페이지 분할)
 *     → _idxd_flush_batch (batch->index >= IDXD_MIN_BATCH_FLUSH(=32)이면 idxd_batch_submit 호출)
 *       → _idxd_prep_command (batch 자체를 1개 디스크립터로 wrapping)
 *       → desc->opcode = IDXD_OPCODE_BATCH, desc_list_addr = batch->user_desc_addr, desc_count = batch->index
 *       → STAILQ_INSERT_TAIL(ops_outstanding) for each batch element
 *       → _submit_to_hw (wmb + movdir64b로 portal에 BATCH 디스크립터 1개 기록)
 *
 * 호출 체인 (완료):
 *   reactor poller → spdk_idxd_process_events
 *     → ops_outstanding STAILQ를 head부터 순회 (in-order 완료 가정)
 *     → op->hw.status가 0이 아니면(=완료) opcode별로 결과 추출 (crc32c_val, output_size, compare result 등)
 *     → op->count 감소, parent_op->count 감소 — 0이 되면 batch refcnt 해제 + 사용자 cb_fn 호출
 *     → batch refcnt가 0이면 _free_batch로 batch_pool에 반납
 *
 * 실행 컨텍스트: SPDK reactor 스레드. 한 채널은 한 reactor가 단독 사용 — chan->ops_pool/batch_pool/
 *   ops_outstanding STAILQ는 lockless. num_channels 카운터만 idxd->num_channels_lock으로 보호.
 *
 * === 타 모듈과의 연결 ===
 * 의존: lib/env_dpdk(spdk_zmalloc DMA, spdk_vtophys), lib/util(spdk_ioviter, spdk_dif_ctx), idxd_internal.h(레지스터/디스크립터 정의),
 *   spdk_internal/idxd.h(공개 인터페이스).
 * 데이터 흐름: 사용자 iovec → vtophys (또는 PASID 활성화 시 가상주소 그대로) → idxd_hw_desc.src_addr/dst_addr →
 *   디바이스 DMA. 완료 record는 op->hw에 디바이스가 직접 DMA write — completion_addr가 그 위치.
 * 공유 자료구조: g_idxd_impls(전역 STAILQ — 백엔드 등록), idxd->num_channels(채널 카운트, mutex 보호),
 *   chan->ops_pool/batch_pool/ops_outstanding(채널 단독 소유, lockless).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_idxd_get_channel/_put_channel: 채널 생성/해제. desc/op 메모리 풀 + DSA의 batch 풀까지 일괄 셋업.
 * - _submit_to_hw: 핫패스 끝. wmb + movdir64b. portal_offset이 chan_per_device * PORTAL_STRIDE 단위 stride.
 * - _idxd_prep_command / _idxd_prep_batch_cmd: 단일 op / batch 내 op를 ring/배열에서 꺼내 desc 초기화.
 * - idxd_batch_create/_cancel/_submit: batch lifecycle. batch_submit이 BATCH 디스크립터를 만들어 portal에 기록.
 * - spdk_idxd_submit_copy/dualcast/compare/fill/crc32c/copy_crc32c/compress/decompress/dif_check/dif_insert/dif_strip/dix_generate/raw_desc:
 *   각 op별 디스크립터 빌드 + flush_batch.
 * - spdk_idxd_process_events: 완료 폴링. ops_outstanding을 in-order 순회, opcode별 결과 추출, 콜백 호출, batch refcnt 정리.
 * - idxd_get_dif_flags / idxd_get_source_dif_flags / idxd_validate_dif_*: T10 DIF/DIX 헬퍼.
 * - idxd_impl_register: 백엔드(user/kernel)가 SPDK_IDXD_IMPL_REGISTER 매크로로 호출 — g_idxd_impls 헤드에 삽입.
 */

#include "spdk/stdinc.h"     /* [한국어] POSIX 표준 헤더 일괄 포함 */

#include "spdk/env.h"        /* [한국어] spdk_zmalloc(DMA hugepage), spdk_vtophys(IOVA 변환) */
#include "spdk/util.h"       /* [한국어] spdk_ioviter(iovec 페어링 순회), spdk_min, SPDK_COUNTOF, container_of */
#include "spdk/memory.h"     /* [한국어] _spdk_wmb (write memory barrier), spdk_mmio_read/write_4/_8 */
#include "spdk/likely.h"     /* [한국어] spdk_likely/unlikely — 핫패스(완료 폴링)에서 분기 힌트 */

#include "spdk/log.h"        /* [한국어] SPDK_ERRLOG/DEBUGLOG */
#include "spdk_internal/idxd.h"  /* [한국어] 공개 idxd 인터페이스: spdk_idxd_device, spdk_idxd_io_channel, spdk_idxd_impl */

#include "idxd_internal.h"   /* [한국어] 디스크립터 비트필드(idxd_hw_desc), opcode/flag enum, 매크로(PORTAL_STRIDE 등) */

#define ALIGN_4K 0x1000              /* [한국어] 4KB 정렬 마스크용 — dualcast가 dst 4KB 정렬 요구 (DSA spec) */
#define USERSPACE_DRIVER_NAME "user"   /* [한국어] g_idxd_impls 검색 키 — set_config(false)일 때 */
#define KERNEL_DRIVER_NAME "kernel"    /* [한국어] g_idxd_impls 검색 키 — set_config(true)일 때 */

/* The max number of completions processed per poll */
/* [한국어] poll 1회당 처리할 최대 완료 이벤트 수. 이 한계로 한 채널이 너무 오래 점유해 다른 poller를
 * 굶기지 않도록 함 — SPDK reactor의 fairness 보장. */
#define IDXD_MAX_COMPLETIONS      128

/* The minimum number of entries in batch per flush */
/* [한국어] batch 자동 플러시 임계치. accel이 이 수만큼 prep하면 _idxd_flush_batch가 batch_submit을 트리거.
 * 너무 작으면 portal 도어벨이 잦아 비효율, 너무 크면 latency 증가. 32는 경험치. */
#define IDXD_MIN_BATCH_FLUSH      32

/* [한국어] T10 DIF 블록 크기 상수 — guard_interval 검사에 사용 */
#define DATA_BLOCK_SIZE_512 512
#define DATA_BLOCK_SIZE_520 520
#define DATA_BLOCK_SIZE_4096 4096
#define DATA_BLOCK_SIZE_4104 4104

/* [한국어] T10 DIF/DIX 메타데이터 크기 — DSA가 8B 또는 16B만 지원 */
#define METADATA_SIZE_8 8
#define METADATA_SIZE_16 16

/* [한국어] 백엔드 impl 레지스트리 — user/kernel이 SPDK_IDXD_IMPL_REGISTER 매크로로 등록.
 * STAILQ로 head insert이므로 마지막 등록자가 첫 번째에 위치. */
static STAILQ_HEAD(, spdk_idxd_impl) g_idxd_impls = STAILQ_HEAD_INITIALIZER(g_idxd_impls);
/* [한국어] 현재 활성화된 impl. spdk_idxd_set_config가 이름으로 검색해 설정. spdk_idxd_probe가 이를 dispatch에 사용. */
static struct spdk_idxd_impl *g_idxd_impl;

/*
 * [한국어]
 * spdk_idxd_get_socket — 디바이스가 부착된 NUMA 소켓 ID 반환.
 * 호출자(accel 모듈)가 채널 분배 시 NUMA-affinity 결정에 사용.
 */
uint32_t
spdk_idxd_get_socket(struct spdk_idxd_device *idxd)
{
	return idxd->socket_id;
}

/*
 * [한국어]
 * _submit_to_hw — 디스크립터 1개를 portal에 기록(MOVDIR64B). I/O 핫패스 끝.
 *
 * @chan: 채널.
 * @op: 제출할 op (op->desc에 64B 디스크립터, op->hw에 완료 record).
 *
 * 1) ops_outstanding STAILQ 꼬리에 op 등록 — 완료 폴링이 head부터 순회하므로 in-order 완료 가정.
 * 2) _spdk_wmb() — store fence. 디스크립터/소스 데이터 buffer가 caching된 채로 디바이스가
 *    DMA fetch하기 전에 완전히 메모리에 가시화되도록 보장.
 * 3) movdir64b(portal+offset, desc) — Intel 명령어. 64B를 cache-line bypass로 직접 portal MMIO에 기록.
 *    DSA가 즉시 fetch & 처리. WQ_DEDICATED 모드 전용 메커니즘.
 * 4) portal_offset을 (chan_per_device * PORTAL_STRIDE) 단위로 진행하고 PORTAL_MASK로 wrap —
 *    여러 채널이 같은 디바이스를 공유할 때 채널 간 portal 슬롯 stride 분리.
 *
 * 실행 컨텍스트: 채널 소유 reactor 스레드. lockless.
 */
static inline void
_submit_to_hw(struct spdk_idxd_io_channel *chan, struct idxd_ops *op)
{
	STAILQ_INSERT_TAIL(&chan->ops_outstanding, op, link);  /* [한국어] 완료 폴링 대상 큐에 등록 — head부터 in-order 회수 */
	/*
	 * We must barrier before writing the descriptor to ensure that data
	 * has been correctly flushed from the associated data buffers before DMA
	 * operations begin.
	 */
	_spdk_wmb();   /* [한국어] write memory barrier — 디바이스 DMA가 stale 데이터를 읽지 않도록 store visibility 보장 */
	movdir64b(chan->portal + chan->portal_offset, op->desc);
	/* [한국어] MOVDIR64B: 인텔 SSE/AVX-512 외 별도 명령어. 64B 페이로드를 단일 atomic NT(non-temporal) write로
	 * portal에 기록 — DSA WQ에 디스크립터가 enqueue되는 hw-defined 메커니즘. */
	chan->portal_offset = (chan->portal_offset + chan->idxd->chan_per_device * PORTAL_STRIDE) &
			      PORTAL_MASK;
	/* [한국어] 다음 portal slot으로 진행. PORTAL_STRIDE는 64B(=한 portal entry), chan_per_device를 곱해
	 * 다른 채널의 슬롯과 충돌하지 않게 함. PORTAL_MASK는 portal 영역 크기로 wrap. */
}

/*
 * [한국어]
 * _vtophys — 가상주소를 디바이스가 사용할 IOVA(또는 PASID 모드에서는 가상주소 그대로)로 변환.
 *
 * @chan: 채널 (pasid_enabled 여부 확인용).
 * @buf: 가상주소.
 * @buf_addr: 출력 — 변환된 주소.
 * @size: 요청한 길이. updated_size로 실제 연속 매핑된 길이를 받음.
 * @return: 0 성공, -EINVAL(주소 변환 실패 또는 연속 매핑 부족).
 *
 * PASID(Process Address Space ID) 모드: 디바이스가 IOMMU를 통해 직접 호스트 가상주소를 사용 가능 →
 * vtophys 변환 불필요. 그렇지 않으면 spdk_vtophys로 hugepage IOVA 변환.
 */
inline static int
_vtophys(struct spdk_idxd_io_channel *chan, const void *buf, uint64_t *buf_addr, uint64_t size)
{
	uint64_t updated_size = size;  /* [한국어] vtophys가 실제 연속 매핑된 길이로 갱신할 in-out 인자 */

	if (chan->pasid_enabled) {
		/* We can just use virtual addresses */
		*buf_addr = (uint64_t)buf;  /* [한국어] PASID + IOMMU SVA — 가상주소 그대로 디바이스에 전달 가능 */
		return 0;
	}

	*buf_addr = spdk_vtophys(buf, &updated_size);

	if (*buf_addr == SPDK_VTOPHYS_ERROR) {  /* [한국어] hugepage 매핑 누락 등 */
		SPDK_ERRLOG("Error translating address\n");
		return -EINVAL;
	}

	if (updated_size < size) {
		/* [한국어] 요청 길이만큼 연속 매핑 안 됨 — 페이지 경계에서 잘렸을 수 있음.
		 * 단일 디스크립터에서는 연속 영역이 필수이므로 에러. 호출자가 더 작게 잘라 다시 호출해야 함. */
		SPDK_ERRLOG("Error translating size (0x%lx), return size (0x%lx)\n", size, updated_size);
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * struct idxd_vtophys_iter — src/dst 두 영역의 vtophys를 페이지 분할에 맞춰 동시 진행하는 이터레이터.
 *
 * memmove처럼 src와 dst 양쪽이 동일 길이를 처리하지만 각자의 페이지 경계가 다를 수 있을 때, 둘 모두에서
 * 연속 매핑된 가장 작은 청크 크기를 한 번에 제출하기 위해 사용.
 */
struct idxd_vtophys_iter {
	const void	*src;       /* [한국어] 원본 가상주소 베이스 */
	void		*dst;       /* [한국어] 목적 가상주소 베이스 */
	uint64_t	len;        /* [한국어] 총 처리할 길이 */

	uint64_t	offset;     /* [한국어] 지금까지 진행한 길이 */

	bool		pasid_enabled;  /* [한국어] PASID 모드면 vtophys 우회 */
};

/*
 * [한국어]
 * idxd_vtophys_iter_init — iter를 초기 상태로 설정.
 */
static void
idxd_vtophys_iter_init(struct spdk_idxd_io_channel *chan,
		       struct idxd_vtophys_iter *iter,
		       const void *src, void *dst, uint64_t len)
{
	iter->src = src;
	iter->dst = dst;
	iter->len = len;
	iter->offset = 0;
	iter->pasid_enabled = chan->pasid_enabled;
}

/*
 * [한국어]
 * idxd_vtophys_iter_next — 다음 청크의 src/dst IOVA를 반환하고 iter를 진행.
 *
 * @return: 청크 길이(>0). 끝났으면 0. 변환 실패 시 SPDK_VTOPHYS_ERROR.
 *
 * PASID 모드: 한 번에 전체 길이 반환 (변환 불필요).
 * 일반 모드: src/dst 각각 vtophys로 연속 매핑 길이 조회 → 둘 중 작은 것을 청크 길이로 결정 → offset 진행.
 */
static uint64_t
idxd_vtophys_iter_next(struct idxd_vtophys_iter *iter,
		       uint64_t *src_phys, uint64_t *dst_phys)
{
	uint64_t src_off, dst_off, len;
	const void *src;
	void *dst;

	src = iter->src + iter->offset;  /* [한국어] 현재 진행 위치의 가상주소 */
	dst = iter->dst + iter->offset;

	if (iter->offset == iter->len) {  /* [한국어] 다 처리함 — 종료 시그널 */
		return 0;
	}

	if (iter->pasid_enabled) {
		*src_phys = (uint64_t)src;
		*dst_phys = (uint64_t)dst;
		return iter->len;  /* [한국어] PASID — 한 번에 끝낼 수 있으므로 전체 길이 반환 (호출자가 단일 desc 사용) */
	}

	len = iter->len - iter->offset;  /* [한국어] 남은 총 길이 */

	src_off = len;
	*src_phys = spdk_vtophys(src, &src_off);  /* [한국어] src 연속 매핑 길이 조회 */
	if (*src_phys == SPDK_VTOPHYS_ERROR) {
		SPDK_ERRLOG("Error translating address\n");
		return SPDK_VTOPHYS_ERROR;
	}

	dst_off = len;
	*dst_phys = spdk_vtophys(dst, &dst_off);  /* [한국어] dst 연속 매핑 길이 조회 */
	if (*dst_phys == SPDK_VTOPHYS_ERROR) {
		SPDK_ERRLOG("Error translating address\n");
		return SPDK_VTOPHYS_ERROR;
	}

	len = spdk_min(src_off, dst_off);  /* [한국어] 양쪽 모두에서 연속 매핑된 만큼만 — 둘 중 작은 것 */
	iter->offset += len;

	return len;
}

/* helper function for DSA specific spdk_idxd_get_channel() stuff */
/*
 * [한국어]
 * _dsa_alloc_batches — DSA 채널 전용으로 batch 풀을 할당.
 *
 * IDXD의 BATCH 디스크립터는 desc_list_addr가 가리키는 N개의 sub-desc 배열을 한 번에 처리한다.
 * 채널은 이런 batch를 num_descriptors개 미리 할당해두고 batch_pool TAILQ로 순환 사용.
 *
 * 각 batch마다:
 *   - user_desc[]: batch가 소유하는 sub-desc 배열 (DMA-가능 메모리, 64B 정렬)
 *   - user_ops[]: 각 sub-desc의 완료/콜백 메타데이터 배열
 *   - user_desc_addr: user_desc의 IOVA — BATCH 디스크립터의 desc_list_addr에 들어감
 *   - user_ops[j].hw → user_desc[j].completion_addr 매핑 (각 sub-desc의 완료 record 위치)
 *
 * IAA는 batch op를 사용하지 않으므로 본 함수는 DSA에서만 호출.
 */
static int
_dsa_alloc_batches(struct spdk_idxd_io_channel *chan, int num_descriptors)
{
	struct idxd_batch *batch;
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	int i, j, num_batches, rc = -1;

	/* Allocate batches */
	num_batches = num_descriptors;  /* [한국어] num_descriptors 개수와 동일하게 1:1 — backpressure 동등 */
	chan->batch_base = calloc(num_batches, sizeof(struct idxd_batch));
	if (chan->batch_base == NULL) {
		SPDK_ERRLOG("Failed to allocate batch pool\n");
		return -ENOMEM;
	}
	batch = chan->batch_base;
	for (i = 0 ; i < num_batches ; i++) {
		batch->size = chan->idxd->batch_size;  /* [한국어] 한 batch가 담을 수 있는 sub-desc 최대 수 (디바이스 max_batch_shift로 결정) */
		batch->user_desc = desc = spdk_zmalloc(batch->size * sizeof(struct idxd_hw_desc),
						       0x40, NULL,
						       SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		/* [한국어] DMA 가능 영역에 sub-desc 배열 할당. 0x40=64B 정렬 (DSA 디스크립터 정렬 요구). */
		if (batch->user_desc == NULL) {
			SPDK_ERRLOG("Failed to allocate batch descriptor memory\n");
			goto error_user;
		}

		rc = _vtophys(chan, batch->user_desc, &batch->user_desc_addr,
			      batch->size * sizeof(struct idxd_hw_desc));
		/* [한국어] sub-desc 배열의 IOVA — BATCH 디스크립터가 가리킬 주소. 길이도 검증(연속 매핑 보장). */
		if (rc) {
			SPDK_ERRLOG("Failed to translate batch descriptor memory\n");
			goto error_user;
		}

		batch->user_ops = op = spdk_zmalloc(batch->size * sizeof(struct idxd_ops),
						    0x40, NULL,
						    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		/* [한국어] 각 sub-desc의 완료 record/콜백 메타. hw 필드도 디바이스 DMA 대상이므로 DMA-가능 영역 필요. */
		if (batch->user_ops == NULL) {
			SPDK_ERRLOG("Failed to allocate user completion memory\n");
			goto error_user;
		}

		for (j = 0; j < batch->size; j++) {
			rc = _vtophys(chan, &op->hw, &desc->completion_addr, sizeof(struct dsa_hw_comp_record));
			/* [한국어] 각 sub-desc.completion_addr를 자신의 op.hw IOVA로 설정 — 디바이스가 이 주소에 완료 record DMA write */
			if (rc) {
				SPDK_ERRLOG("Failed to translate batch entry completion memory\n");
				goto error_user;
			}
			op++;
			desc++;
		}
		TAILQ_INSERT_TAIL(&chan->batch_pool, batch, link);  /* [한국어] free 풀에 등록 */
		batch++;
	}
	return 0;

error_user:
	/* [한국어] 부분 할당 회수 — 이미 풀에 들어간 batch들의 desc/ops 모두 spdk_free */
	TAILQ_FOREACH(batch, &chan->batch_pool, link) {
		spdk_free(batch->user_ops);
		batch->user_ops = NULL;
		spdk_free(batch->user_desc);
		batch->user_desc = NULL;
	}
	return rc;
}

/*
 * [한국어]
 * spdk_idxd_get_channel — IDXD 채널 1개 생성. accel 모듈이 spdk_thread당 한 채널씩 잡음.
 *
 * 동작:
 *   1) chan 구조체 할당 + STAILQ/TAILQ 초기화.
 *   2) num_channels < chan_per_device 검사 (디바이스가 허용하는 최대 채널 수 한계).
 *   3) portal 주소(impl->portal_get_addr) + portal_offset 계산 — 채널마다 다른 시작 stride.
 *   4) num_descriptors = total_wq_size / chan_per_device — 채널당 분할.
 *   5) desc_base/ops_base를 각각 num_descriptors개 DMA-가능 메모리로 할당, 64B 정렬.
 *   6) DSA면 batch 풀 추가 할당 (_dsa_alloc_batches).
 *   7) 각 op.desc 매핑 + completion_addr 설정 + ops_pool에 enqueue.
 *
 * 실패 경로: 부분 할당된 자원을 ops_base/desc_base spdk_free 후 free(chan).
 */
struct spdk_idxd_io_channel *
spdk_idxd_get_channel(struct spdk_idxd_device *idxd)
{
	struct spdk_idxd_io_channel *chan;
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	int i, num_descriptors, rc = -1;
	uint32_t comp_rec_size;

	assert(idxd != NULL);

	chan = calloc(1, sizeof(struct spdk_idxd_io_channel));
	if (chan == NULL) {
		SPDK_ERRLOG("Failed to allocate idxd chan\n");
		return NULL;
	}

	chan->idxd = idxd;
	chan->pasid_enabled = idxd->pasid_enabled;  /* [한국어] PASID 비활성/활성 — vtophys 우회 결정 */
	STAILQ_INIT(&chan->ops_pool);               /* [한국어] free op 풀 — _idxd_prep_command가 head pop */
	TAILQ_INIT(&chan->batch_pool);              /* [한국어] free batch 풀 — idxd_batch_create가 head pop */
	STAILQ_INIT(&chan->ops_outstanding);        /* [한국어] 디바이스에 제출된 op들 — 완료 폴링이 head 순회 */

	/* Assign WQ, portal */
	pthread_mutex_lock(&idxd->num_channels_lock);  /* [한국어] num_channels 증가 직렬화 */
	if (idxd->num_channels == idxd->chan_per_device) {
		/* too many channels sharing this device */
		pthread_mutex_unlock(&idxd->num_channels_lock);
		SPDK_ERRLOG("Too many channels sharing this device\n");
		goto error;
	}

	/* Have each channel start at a different offset. */
	chan->portal = idxd->impl->portal_get_addr(idxd);  /* [한국어] BAR2 portal 가상주소 (user impl) 또는 char dev (kernel impl) */
	chan->portal_offset = (idxd->num_channels * PORTAL_STRIDE) & PORTAL_MASK;
	/* [한국어] 채널 0 → offset 0, 채널 1 → offset PORTAL_STRIDE, ... — 채널 간 portal 슬롯 분리 */
	idxd->num_channels++;

	pthread_mutex_unlock(&idxd->num_channels_lock);

	/* Allocate descriptors and completions */
	num_descriptors = idxd->total_wq_size / idxd->chan_per_device;
	/* [한국어] 채널당 디스크립터 수 = WQ 슬롯 / 채널 수. backpressure가 균등하게 분배됨. */
	chan->desc_base = desc = spdk_zmalloc(num_descriptors * sizeof(struct idxd_hw_desc),
					      0x40, NULL,
					      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->desc_base == NULL) {
		SPDK_ERRLOG("Failed to allocate DSA descriptor memory\n");
		goto error;
	}

	chan->ops_base = op = spdk_zmalloc(num_descriptors * sizeof(struct idxd_ops),
					   0x40, NULL,
					   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	/* [한국어] op.hw가 디바이스 DMA 대상이라 DMA-가능 영역 필요 */
	if (chan->ops_base == NULL) {
		SPDK_ERRLOG("Failed to allocate idxd_ops memory\n");
		goto error;
	}

	if (idxd->type == IDXD_DEV_TYPE_DSA) {
		comp_rec_size = sizeof(struct dsa_hw_comp_record);   /* [한국어] DSA 완료 record 크기 (32B) */
		if (_dsa_alloc_batches(chan, num_descriptors)) {
			goto error;
		}
	} else {
		comp_rec_size = sizeof(struct iaa_hw_comp_record);   /* [한국어] IAA 완료 record (출력 크기 등 추가 필드) */
	}

	for (i = 0; i < num_descriptors; i++) {
		STAILQ_INSERT_TAIL(&chan->ops_pool, op, link);   /* [한국어] free 풀에 op 등록 */
		op->desc = desc;                                 /* [한국어] op과 desc를 1:1 페어링 */
		rc = _vtophys(chan, &op->hw, &desc->completion_addr, comp_rec_size);
		/* [한국어] desc.completion_addr를 op.hw IOVA로 설정 — 디바이스가 완료 시 이 주소에 record DMA write */
		if (rc) {
			SPDK_ERRLOG("Failed to translate completion memory\n");
			goto error;
		}
		op++;
		desc++;
	}

	return chan;

error:
	spdk_free(chan->ops_base);
	chan->ops_base = NULL;
	spdk_free(chan->desc_base);
	chan->desc_base = NULL;
	free(chan);
	return NULL;
}

/* [한국어] 전방 선언 — _free_batch가 idxd_batch_cancel을 사용하지만 정의는 후행 */
static int idxd_batch_cancel(struct spdk_idxd_io_channel *chan, int status);

/*
 * [한국어]
 * spdk_idxd_put_channel — 채널 해제. 진행 중인 batch가 있으면 cancel 후 모든 풀 메모리 회수.
 *
 * 호출 컨텍스트: 채널 소유 reactor 스레드. ops_outstanding가 비어 있어야 안전 (호출자 책임).
 */
void
spdk_idxd_put_channel(struct spdk_idxd_io_channel *chan)
{
	struct idxd_batch *batch;

	assert(chan != NULL);
	assert(chan->idxd != NULL);

	if (chan->batch) {
		idxd_batch_cancel(chan, -ECANCELED);   /* [한국어] 진행 중 batch가 있으면 사용자 콜백에 ECANCELED 알림 */
	}

	pthread_mutex_lock(&chan->idxd->num_channels_lock);
	assert(chan->idxd->num_channels > 0);
	chan->idxd->num_channels--;
	pthread_mutex_unlock(&chan->idxd->num_channels_lock);

	spdk_free(chan->ops_base);
	spdk_free(chan->desc_base);
	while ((batch = TAILQ_FIRST(&chan->batch_pool))) {
		TAILQ_REMOVE(&chan->batch_pool, batch, link);
		spdk_free(batch->user_ops);
		spdk_free(batch->user_desc);
	}
	free(chan->batch_base);   /* [한국어] batch 헤더 배열 — calloc으로 할당 */
	free(chan);
}

/*
 * [한국어]
 * idxd_get_impl_by_name — g_idxd_impls에서 이름으로 backend 검색.
 */
static inline struct spdk_idxd_impl *
idxd_get_impl_by_name(const char *impl_name)
{
	struct spdk_idxd_impl *impl;

	assert(impl_name != NULL);
	STAILQ_FOREACH(impl, &g_idxd_impls, link) {
		if (0 == strcmp(impl_name, impl->name)) {
			return impl;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * spdk_idxd_set_config — 사용자/커널 백엔드 선택. 디바이스가 attach되기 전에 한 번만 호출 가능.
 *
 * @kernel_mode: true면 "kernel" impl(idxd 커널 드라이버 char dev), false면 "user"(VFIO + mmap).
 * @return: 0 성공, -EALREADY(이미 다른 impl로 attach됨), -EINVAL(impl 미등록).
 */
int
spdk_idxd_set_config(bool kernel_mode)
{
	struct spdk_idxd_impl *tmp;

	if (kernel_mode) {
		tmp = idxd_get_impl_by_name(KERNEL_DRIVER_NAME);
	} else {
		tmp = idxd_get_impl_by_name(USERSPACE_DRIVER_NAME);
	}

	if (g_idxd_impl != NULL && g_idxd_impl != tmp) {
		SPDK_ERRLOG("Cannot change idxd implementation after devices are initialized\n");
		assert(false);
		return -EALREADY;
	}
	g_idxd_impl = tmp;

	if (g_idxd_impl == NULL) {
		SPDK_ERRLOG("Cannot set the idxd implementation with %s mode\n",
			    kernel_mode ? KERNEL_DRIVER_NAME : USERSPACE_DRIVER_NAME);
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * idxd_device_destruct — 백엔드 destruct dispatch.
 */
static void
idxd_device_destruct(struct spdk_idxd_device *idxd)
{
	assert(idxd->impl != NULL);

	idxd->impl->destruct(idxd);
}

/*
 * [한국어]
 * spdk_idxd_probe — 공개 API. 선택된 백엔드의 probe로 위임.
 */
int
spdk_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb,
		spdk_idxd_probe_cb probe_cb)
{
	if (g_idxd_impl == NULL) {
		SPDK_ERRLOG("No idxd impl is selected\n");
		return -1;
	}

	return g_idxd_impl->probe(cb_ctx, attach_cb, probe_cb);
}

/*
 * [한국어]
 * spdk_idxd_detach — 공개 API. 디바이스 destruct.
 */
void
spdk_idxd_detach(struct spdk_idxd_device *idxd)
{
	assert(idxd != NULL);
	idxd_device_destruct(idxd);
}

/*
 * [한국어]
 * _idxd_prep_command — 단일 op용 desc/op 슬롯을 ops_pool에서 꺼내 기본 필드 초기화.
 *
 * @chan: 채널.
 * @cb_fn, @cb_arg: 완료 콜백.
 * @flags: opcode-specific flag 비트.
 * @_desc, @_op: 출력. 호출자가 opcode/주소 등 op-specific 필드를 채워야 함.
 * @return: 0 성공, -EBUSY(ops_pool 비어있음 — flow control 위반).
 *
 * 핵심: completion_addr는 채널 init 시 미리 _vtophys로 계산된 값을 보존(memset 후 복원).
 * IDXD_FLAG_COMPLETION_ADDR_VALID + REQUEST_COMPLETION 비트는 모든 op에 강제 set —
 * 디바이스가 완료 record를 반드시 DMA write하도록.
 */
static int
_idxd_prep_command(struct spdk_idxd_io_channel *chan, spdk_idxd_req_cb cb_fn, void *cb_arg,
		   int flags, struct idxd_hw_desc **_desc, struct idxd_ops **_op)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t comp_addr;

	if (!STAILQ_EMPTY(&chan->ops_pool)) {
		op = *_op = STAILQ_FIRST(&chan->ops_pool);
		desc = *_desc = op->desc;
		comp_addr = desc->completion_addr;       /* [한국어] init에서 계산된 IOVA 보존 */
		memset(desc, 0, sizeof(*desc));          /* [한국어] desc 전체 클리어 — 이전 op의 잔존값 제거 */
		desc->completion_addr = comp_addr;       /* [한국어] 보존한 값 복원 */
		STAILQ_REMOVE_HEAD(&chan->ops_pool, link);  /* [한국어] free 풀에서 제거 */
	} else {
		/* The application needs to handle this, violation of flow control */
		return -EBUSY;
	}

	flags |= IDXD_FLAG_COMPLETION_ADDR_VALID;
	flags |= IDXD_FLAG_REQUEST_COMPLETION;

	desc->flags = flags;
	op->cb_arg = cb_arg;
	op->cb_fn = cb_fn;
	op->batch = NULL;        /* [한국어] 단일 op이므로 batch 소속 없음 */
	op->parent = NULL;       /* [한국어] 부모 op 없음 (compound op 아님) */
	op->count = 1;           /* [한국어] 단일 op — 완료 카운트 1 */

	return 0;
}

/*
 * [한국어]
 * _idxd_prep_batch_cmd — batch 내 sub-op용 desc/op 슬롯을 batch->user_desc/user_ops 배열에서 할당.
 *
 * batch->index가 다음 사용 슬롯 인덱스. batch->size에 도달하면 -EBUSY.
 * op->batch = batch로 batch 소속 표시 — 완료 시 batch refcnt 관리에 사용.
 */
static int
_idxd_prep_batch_cmd(struct spdk_idxd_io_channel *chan, spdk_idxd_req_cb cb_fn,
		     void *cb_arg, int flags,
		     struct idxd_hw_desc **_desc, struct idxd_ops **_op)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t comp_addr;
	struct idxd_batch *batch;

	batch = chan->batch;

	assert(batch != NULL);
	if (batch->index == batch->size) {
		return -EBUSY;        /* [한국어] batch가 가득 참 — flush_batch가 미리 발동했어야 정상 */
	}

	desc = *_desc = &batch->user_desc[batch->index];
	op = *_op = &batch->user_ops[batch->index];

	op->desc = desc;
	SPDK_DEBUGLOG(idxd, "Prep batch %p index %u\n", batch, batch->index);

	batch->index++;

	comp_addr = desc->completion_addr;        /* [한국어] init에서 계산된 IOVA 보존 */
	memset(desc, 0, sizeof(*desc));
	desc->completion_addr = comp_addr;
	flags |= IDXD_FLAG_COMPLETION_ADDR_VALID;
	flags |= IDXD_FLAG_REQUEST_COMPLETION;
	desc->flags = flags;
	op->cb_arg = cb_arg;
	op->cb_fn = cb_fn;
	op->batch = batch;          /* [한국어] batch 소속 표시 */
	op->parent = NULL;          /* [한국어] 호출자가 첫 op이면 NULL, 후속 op이면 first_op로 set 가능 */
	op->count = 1;
	op->crc_dst = NULL;

	return 0;
}

/*
 * [한국어]
 * idxd_batch_create — batch_pool에서 free batch 1개 가져와 chan->batch에 설정.
 * 호출자: _idxd_setup_batch (chan->batch == NULL 시).
 */
static struct idxd_batch *
idxd_batch_create(struct spdk_idxd_io_channel *chan)
{
	struct idxd_batch *batch;

	assert(chan != NULL);
	assert(chan->batch == NULL);

	if (!TAILQ_EMPTY(&chan->batch_pool)) {
		batch = TAILQ_FIRST(&chan->batch_pool);
		batch->index = 0;       /* [한국어] 새 batch는 0번부터 채움 */
		batch->chan = chan;
		chan->batch = batch;    /* [한국어] 활성 batch로 설정 */
		TAILQ_REMOVE(&chan->batch_pool, batch, link);
	} else {
		/* The application needs to handle this. */
		return NULL;            /* [한국어] batch 풀 부족 — 호출자가 backpressure 처리 */
	}

	return batch;
}

/*
 * [한국어]
 * _free_batch — batch를 batch_pool로 반납. refcnt가 0일 때만 호출되어야 함.
 */
static void
_free_batch(struct idxd_batch *batch, struct spdk_idxd_io_channel *chan)
{
	SPDK_DEBUGLOG(idxd, "Free batch %p\n", batch);
	assert(batch->refcnt == 0);
	batch->index = 0;
	batch->chan = NULL;
	TAILQ_INSERT_TAIL(&chan->batch_pool, batch, link);
}

/*
 * [한국어]
 * idxd_batch_cancel — 활성 batch의 모든 sub-op에 status를 보고 후 batch 반납.
 *
 * 호출자: spdk_idxd_put_channel (-ECANCELED), 또는 진행 중 build 단계 실패 시.
 * 이미 HW에 제출된 batch(index==UINT16_MAX)는 cancel 불가 — 완료 폴링이 처리해야 함.
 */
static int
idxd_batch_cancel(struct spdk_idxd_io_channel *chan, int status)
{
	struct idxd_ops *op;
	struct idxd_batch *batch;
	int i;

	assert(chan != NULL);

	batch = chan->batch;
	assert(batch != NULL);

	if (batch->index == UINT16_MAX) {
		SPDK_ERRLOG("Cannot cancel batch, already submitted to HW.\n");
		return -EINVAL;
	}

	chan->batch = NULL;

	for (i = 0; i < batch->index; i++) {
		op = &batch->user_ops[i];
		if (op->cb_fn) {
			op->cb_fn(op->cb_arg, status);   /* [한국어] 사용자 콜백에 cancel status 알림 */
		}
	}

	_free_batch(batch, chan);

	return 0;
}

/*
 * [한국어]
 * idxd_batch_submit — 활성 batch를 BATCH 디스크립터 1개로 wrapping해 HW에 제출.
 *
 * 단계:
 *   1) batch->index == 0 → 빈 batch — cancel(0).
 *   2) _idxd_prep_command로 새 desc/op 1개 확보 (BATCH 디스크립터 그릇).
 *   3) batch->index == 1 → BATCH 오버헤드 회피, sub-desc[0]을 그대로 단일 desc로 변환.
 *   4) batch->index >= 2 → opcode=BATCH, desc_list_addr=user_desc_addr, desc_count=index 설정.
 *      각 sub-op을 ops_outstanding에 등록 + batch->refcnt 증가, batch->index=UINT16_MAX(=submitted 표시).
 *   5) chan->batch=NULL (활성 batch 해제), _submit_to_hw로 portal 기록.
 */
static int
idxd_batch_submit(struct spdk_idxd_io_channel *chan,
		  spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_batch *batch;
	struct idxd_ops *op;
	int i, rc, flags = 0;

	assert(chan != NULL);

	batch = chan->batch;
	assert(batch != NULL);

	if (batch->index == 0) {
		return idxd_batch_cancel(chan, 0);   /* [한국어] 빈 batch — 사용자 콜백에 status=0으로 알리고 반납 */
	}

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, flags, &desc, &op);
	if (rc) {
		return rc;     /* [한국어] ops_pool 부족 — 호출자가 backpressure 처리 */
	}

	if (batch->index == 1) {
		uint64_t completion_addr;

		/* If there's only one command, convert it away from a batch. */
		/* [한국어] 단일 sub-op 최적화 — BATCH wrapping 오버헤드 제거 + sub-op의 desc를 그대로 사용 */
		completion_addr = desc->completion_addr;       /* [한국어] op의 completion_addr 보존 */
		memcpy(desc, &batch->user_desc[0], sizeof(*desc));  /* [한국어] sub-desc[0]을 desc로 복사 */
		desc->completion_addr = completion_addr;        /* [한국어] op의 completion_addr 복원 */
		op->cb_fn = batch->user_ops[0].cb_fn;
		op->cb_arg = batch->user_ops[0].cb_arg;
		op->crc_dst = batch->user_ops[0].crc_dst;
		_free_batch(batch, chan);                       /* [한국어] 빈 batch 반납 */
	} else {
		/* Command specific. */
		desc->opcode = IDXD_OPCODE_BATCH;
		desc->desc_list_addr = batch->user_desc_addr;   /* [한국어] sub-desc 배열 IOVA */
		desc->desc_count = batch->index;                /* [한국어] sub-desc 개수 */
		assert(batch->index <= batch->size);

		/* Add the batch elements completion contexts to the outstanding list to be polled. */
		for (i = 0 ; i < batch->index; i++) {
			batch->refcnt++;     /* [한국어] sub-op마다 batch 참조 +1 — 모든 완료 시 0 도달 시 batch 반납 */
			STAILQ_INSERT_TAIL(&chan->ops_outstanding, (struct idxd_ops *)&batch->user_ops[i],
					   link);
			/* [한국어] sub-ops를 outstanding에 enqueue — 완료 폴링이 각 sub-op을 처리. */
		}
		batch->index = UINT16_MAX;   /* [한국어] "이미 제출됨" 마커 — cancel 시 거부에 사용 */
	}

	chan->batch = NULL;     /* [한국어] 활성 batch 해제 — 다음 build는 새 batch 생성 */

	/* Submit operation. */
	_submit_to_hw(chan, op);     /* [한국어] BATCH(또는 단일 변환) 디스크립터 portal에 기록 */
	SPDK_DEBUGLOG(idxd, "Submitted batch %p\n", batch);

	return 0;
}

/*
 * [한국어]
 * _idxd_setup_batch — chan->batch가 없으면 새로 생성.
 * 모든 submit_* 진입점이 첫머리에 호출.
 */
static int
_idxd_setup_batch(struct spdk_idxd_io_channel *chan)
{
	struct idxd_batch *batch;

	if (chan->batch == NULL) {
		batch = idxd_batch_create(chan);
		if (batch == NULL) {
			return -EBUSY;
		}
	}

	return 0;
}

/*
 * [한국어]
 * _idxd_flush_batch — 누적된 batch가 IDXD_MIN_BATCH_FLUSH(32) 이상이면 자동 제출.
 *
 * submit_*가 모든 prep 완료 후 끝에 호출. accel 패턴: 여러 op를 prep해 batch에 모은 뒤 일정 임계 도달 시 flush.
 * -EBUSY로 ops_pool이 비면 0을 반환하고 다음 process_events에서 재시도(코드 주석 참조).
 */
static int
_idxd_flush_batch(struct spdk_idxd_io_channel *chan)
{
	struct idxd_batch *batch = chan->batch;
	int rc;

	if (batch != NULL && batch->index >= IDXD_MIN_BATCH_FLUSH) {
		/* Close out the full batch */
		rc = idxd_batch_submit(chan, NULL, NULL);
		if (rc) {
			assert(rc == -EBUSY);
			/*
			 * Return 0. This will get re-submitted within idxd_process_events where
			 * if it fails, it will get correctly aborted.
			 */
			return 0;
		}
	}

	return 0;
}

/*
 * [한국어]
 * _update_write_flags — IDXD_FLAG_CACHE_CONTROL 비트를 토글.
 *
 * DSA에는 dst write를 LLC에 cache write-back 또는 directly to memory(non-temporal)로 분기하는 비트가 있다.
 * 현재 코드는 매번 XOR로 토글하여 round-robin 방식으로 분배 — 워크로드 특성에 따라 어느 한쪽이 유리할 때를 대비.
 */
static inline void
_update_write_flags(struct spdk_idxd_io_channel *chan, struct idxd_hw_desc *desc)
{
	desc->flags ^= IDXD_FLAG_CACHE_CONTROL;
}

/*
 * [한국어]
 * spdk_idxd_submit_copy — iovec → iovec memmove.
 *
 * spdk_ioviter로 src iovec과 dst iovec를 페어링하면서 동일 길이 청크를 추출 →
 * idxd_vtophys_iter로 페이지 분할에 맞춰 IOVA 추출 → 각 청크마다 _idxd_prep_batch_cmd로
 * MEMMOVE 디스크립터 prep.
 *
 * compound op 패턴: 첫 sub-op이 first_op로 cb_fn 보유, 후속 sub-op들은 op->parent=first_op로 체이닝하고
 * first_op->count++. process_events가 모든 sub-op 완료 시 parent의 count==0일 때 콜백 호출.
 */
int
spdk_idxd_submit_copy(struct spdk_idxd_io_channel *chan,
		      struct iovec *diov, uint32_t diovcnt,
		      struct iovec *siov, uint32_t siovcnt,
		      int flags, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op, *op;
	void *src, *dst;
	uint64_t src_addr, dst_addr;
	int rc, count;
	uint64_t len, seg_len;
	struct spdk_ioviter iter;
	struct idxd_vtophys_iter vtophys_iter;

	assert(chan != NULL);
	assert(diov != NULL);
	assert(siov != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	count = 0;
	first_op = NULL;
	for (len = spdk_ioviter_first(&iter, siov, siovcnt, diov, diovcnt, &src, &dst);
	     len > 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {
		/* [한국어] spdk_ioviter — siov[]와 diov[]를 페어링하며 가장 작은 동일 길이 청크 추출 */

		idxd_vtophys_iter_init(chan, &vtophys_iter, src, dst, len);

		while (len > 0) {
			if (first_op == NULL) {
				rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op = op;     /* [한국어] 첫 sub-op이 사용자 콜백을 소유 */
			} else {
				rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op->count++;     /* [한국어] compound op — first_op이 모든 sub-op 완료 카운트를 추적 */
				op->parent = first_op;
			}

			count++;

			src_addr = 0;
			dst_addr = 0;
			seg_len = idxd_vtophys_iter_next(&vtophys_iter, &src_addr, &dst_addr);
			if (seg_len == SPDK_VTOPHYS_ERROR) {
				rc = -EFAULT;
				goto error;
			}

			desc->opcode = IDXD_OPCODE_MEMMOVE;       /* [한국어] DSA memmove opcode */
			desc->src_addr = src_addr;
			desc->dst_addr = dst_addr;
			desc->xfer_size = seg_len;
			_update_write_flags(chan, desc);          /* [한국어] cache control 비트 토글 */

			len -= seg_len;
		}
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;     /* [한국어] 부분 prep 롤백 — 실패 지점 이전 슬롯들도 다시 사용 가능 */
	return rc;
}

/* Dual-cast copies the same source to two separate destination buffers. */
/*
 * [한국어]
 * spdk_idxd_submit_dualcast — 한 src를 dst1과 dst2 두 곳에 동시 복사.
 *
 * DSA 전용 op. RAID 미러링이나 데이터 백업 시 CPU 한 번에 두 곳에 쓰기 가능.
 * 제약: dst1, dst2 모두 4KB 정렬 필요 (DSA spec).
 *
 * 두 개의 vtophys iter를 병렬 진행 — outer는 src↔dst1, inner는 src↔dst2.
 * 각 청크는 outer.seg_len과 inner.seg_len 중 작은 쪽으로 잘림.
 */
int
spdk_idxd_submit_dualcast(struct spdk_idxd_io_channel *chan, void *dst1, void *dst2,
			  const void *src, uint64_t nbytes, int flags,
			  spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op, *op;
	uint64_t src_addr, dst1_addr, dst2_addr;
	int rc, count;
	uint64_t len;
	uint64_t outer_seg_len, inner_seg_len;
	struct idxd_vtophys_iter iter_outer, iter_inner;

	assert(chan != NULL);
	assert(dst1 != NULL);
	assert(dst2 != NULL);
	assert(src != NULL);

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;     /* [한국어] DSA 사양 위반 */
	}

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	idxd_vtophys_iter_init(chan, &iter_outer, src, dst1, nbytes);

	first_op = NULL;
	count = 0;
	while (nbytes > 0) {
		src_addr = 0;
		dst1_addr = 0;
		outer_seg_len = idxd_vtophys_iter_next(&iter_outer, &src_addr, &dst1_addr);
		if (outer_seg_len == SPDK_VTOPHYS_ERROR) {
			goto error;
		}

		idxd_vtophys_iter_init(chan, &iter_inner, src, dst2, nbytes);
		/* [한국어] 매 outer 청크마다 inner iter 재초기화 — src 시작점은 outer가 진행한 만큼 이미 src 포인터에 반영됨 */

		src += outer_seg_len;
		nbytes -= outer_seg_len;

		while (outer_seg_len > 0) {
			if (first_op == NULL) {
				rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op = op;
			} else {
				rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op->count++;
				op->parent = first_op;
			}

			count++;

			src_addr = 0;
			dst2_addr = 0;
			inner_seg_len = idxd_vtophys_iter_next(&iter_inner, &src_addr, &dst2_addr);
			if (inner_seg_len == SPDK_VTOPHYS_ERROR) {
				rc = -EFAULT;
				goto error;
			}

			len = spdk_min(outer_seg_len, inner_seg_len);   /* [한국어] dst1/dst2 양쪽 모두 연속 매핑된 만큼만 처리 */

			/* Command specific. */
			desc->opcode = IDXD_OPCODE_DUALCAST;
			desc->src_addr = src_addr;
			desc->dst_addr = dst1_addr;
			desc->dest2 = dst2_addr;       /* [한국어] dualcast 전용 두 번째 dst 필드 */
			desc->xfer_size = len;
			_update_write_flags(chan, desc);

			dst1_addr += len;
			outer_seg_len -= len;
		}
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

/*
 * [한국어]
 * spdk_idxd_submit_compare — 두 iovec(siov1, siov2)의 byte 단위 비교.
 * COMPARE 디스크립터의 결과는 op->hw.result에 0(match) 또는 1(mismatch)로 기록되어
 * process_events가 status에 반영.
 */
int
spdk_idxd_submit_compare(struct spdk_idxd_io_channel *chan,
			 struct iovec *siov1, size_t siov1cnt,
			 struct iovec *siov2, size_t siov2cnt,
			 int flags, spdk_idxd_req_cb cb_fn, void *cb_arg)
{

	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op, *op;
	void *src1, *src2;
	uint64_t src1_addr, src2_addr;
	int rc, count;
	uint64_t len, seg_len;
	struct spdk_ioviter iter;
	struct idxd_vtophys_iter vtophys_iter;

	assert(chan != NULL);
	assert(siov1 != NULL);
	assert(siov2 != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	count = 0;
	first_op = NULL;
	for (len = spdk_ioviter_first(&iter, siov1, siov1cnt, siov2, siov2cnt, &src1, &src2);
	     len > 0;
	     len = spdk_ioviter_next(&iter, &src1, &src2)) {

		idxd_vtophys_iter_init(chan, &vtophys_iter, src1, src2, len);

		while (len > 0) {
			if (first_op == NULL) {
				rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op = op;
			} else {
				rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op->count++;
				op->parent = first_op;
			}

			count++;

			src1_addr = 0;
			src2_addr = 0;
			seg_len = idxd_vtophys_iter_next(&vtophys_iter, &src1_addr, &src2_addr);
			if (seg_len == SPDK_VTOPHYS_ERROR) {
				rc = -EFAULT;
				goto error;
			}

			desc->opcode = IDXD_OPCODE_COMPARE;
			desc->src_addr = src1_addr;
			desc->src2_addr = src2_addr;       /* [한국어] COMPARE 전용 두 번째 src */
			desc->xfer_size = seg_len;

			len -= seg_len;
		}
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

/*
 * [한국어]
 * spdk_idxd_submit_fill — 8B 패턴을 dst에 nbytes만큼 반복 채움. (디바이스가 ioat과 달리 페이지 분할 필요)
 */
int
spdk_idxd_submit_fill(struct spdk_idxd_io_channel *chan,
		      struct iovec *diov, size_t diovcnt,
		      uint64_t fill_pattern, int flags,
		      spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op, *op;
	uint64_t dst_addr;
	int rc, count;
	uint64_t len, seg_len;
	void *dst;
	size_t i;

	assert(chan != NULL);
	assert(diov != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	count = 0;
	first_op = NULL;
	for (i = 0; i < diovcnt; i++) {
		len = diov[i].iov_len;
		dst = diov[i].iov_base;

		while (len > 0) {
			if (first_op == NULL) {
				rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op = op;
			} else {
				rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op->count++;
				op->parent = first_op;
			}

			count++;

			seg_len = len;
			if (chan->pasid_enabled) {
				dst_addr = (uint64_t)dst;     /* [한국어] PASID — 가상주소 그대로 */
			} else {
				dst_addr = spdk_vtophys(dst, &seg_len);
				if (dst_addr == SPDK_VTOPHYS_ERROR) {
					SPDK_ERRLOG("Error translating address\n");
					rc = -EFAULT;
					goto error;
				}
			}

			seg_len = spdk_min(seg_len, len);   /* [한국어] iovec 잔여와 vtophys 연속 길이 중 작은 것 */

			desc->opcode = IDXD_OPCODE_MEMFILL;
			desc->pattern = fill_pattern;       /* [한국어] 8B 패턴 — 디바이스가 dst에 반복 기록 */
			desc->dst_addr = dst_addr;
			desc->xfer_size = seg_len;
			_update_write_flags(chan, desc);

			len -= seg_len;
			dst += seg_len;
		}
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

/*
 * [한국어]
 * spdk_idxd_submit_crc32c — siov[]에 대해 CRC32-C 계산. NVMe T10DIF/iSCSI에서 사용.
 *
 * 청크가 여러 개로 분할되면 후속 청크는 IDXD_FLAG_CRC_READ_CRC_SEED + FENCE를 set하고
 * crc32c.addr=prev_crc로 이전 디스크립터의 결과를 seed로 사용 — chained CRC 계산.
 *
 * IDXD_FLAG_FENCE: 디바이스 내부 디스크립터 처리 순서에 fence를 걸어 이전 결과를 기다리게 함.
 *
 * 마지막 op->crc_dst = crc_dst — process_events가 op->hw.crc32c_val을 받아 사용자 메모리에 기록.
 */
int
spdk_idxd_submit_crc32c(struct spdk_idxd_io_channel *chan,
			struct iovec *siov, size_t siovcnt,
			uint32_t seed, uint32_t *crc_dst, int flags,
			spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op, *op;
	uint64_t src_addr;
	int rc, count;
	uint64_t len, seg_len;
	void *src;
	size_t i;
	uint64_t prev_crc = 0;     /* [한국어] 이전 디스크립터의 crc32c_val IOVA — 다음 디스크립터의 seed source */

	assert(chan != NULL);
	assert(siov != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	count = 0;
	op = NULL;
	first_op = NULL;
	for (i = 0; i < siovcnt; i++) {
		len = siov[i].iov_len;
		src = siov[i].iov_base;

		while (len > 0) {
			if (first_op == NULL) {
				rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op = op;
			} else {
				rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op->count++;
				op->parent = first_op;
			}

			count++;

			seg_len = len;
			if (chan->pasid_enabled) {
				src_addr = (uint64_t)src;
			} else {
				src_addr = spdk_vtophys(src, &seg_len);
				if (src_addr == SPDK_VTOPHYS_ERROR) {
					SPDK_ERRLOG("Error translating address\n");
					rc = -EFAULT;
					goto error;
				}
			}

			seg_len = spdk_min(seg_len, len);

			desc->opcode = IDXD_OPCODE_CRC32C_GEN;
			desc->src_addr = src_addr;
			if (op == first_op) {
				desc->crc32c.seed = seed;       /* [한국어] 첫 디스크립터 — 사용자 제공 초기 seed */
			} else {
				desc->flags |= IDXD_FLAG_FENCE | IDXD_FLAG_CRC_READ_CRC_SEED;
				/* [한국어] FENCE: 이전 디스크립터 완료 대기. CRC_READ_CRC_SEED: seed를 메모리에서 read. */
				desc->crc32c.addr = prev_crc;   /* [한국어] 이전 디스크립터 완료 record 안의 crc32c_val 위치 */
			}

			desc->xfer_size = seg_len;
			prev_crc = desc->completion_addr + offsetof(struct dsa_hw_comp_record, crc32c_val);
			/* [한국어] 다음 디스크립터가 읽어갈 seed의 IOVA — completion record의 crc32c_val 필드 시작점 */

			len -= seg_len;
			src += seg_len;
		}
	}

	/* Only the last op copies the crc to the destination */
	if (op) {
		op->crc_dst = crc_dst;     /* [한국어] 마지막 sub-op만 crc_dst 보유 — 모든 청크가 처리되면 최종 CRC를 사용자 메모리로 복사 */
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

/*
 * [한국어]
 * spdk_idxd_submit_copy_crc32c — copy + CRC32C를 한 번에 수행.
 *
 * NVMe write 경로에서 host buffer를 device buffer로 복사하면서 동시에 CRC를 계산해 PI(Protection Information)
 * 필드를 채우는 fast path 시나리오. 단일 디스크립터로 카피와 CRC 계산을 동시에 하므로 CPU/메모리 대역폭 절약.
 */
int
spdk_idxd_submit_copy_crc32c(struct spdk_idxd_io_channel *chan,
			     struct iovec *diov, size_t diovcnt,
			     struct iovec *siov, size_t siovcnt,
			     uint32_t seed, uint32_t *crc_dst, int flags,
			     spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op, *op;
	void *src, *dst;
	uint64_t src_addr, dst_addr;
	int rc, count;
	uint64_t len, seg_len;
	struct spdk_ioviter iter;
	struct idxd_vtophys_iter vtophys_iter;
	uint64_t prev_crc = 0;

	assert(chan != NULL);
	assert(diov != NULL);
	assert(siov != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	count = 0;
	op = NULL;
	first_op = NULL;
	for (len = spdk_ioviter_first(&iter, siov, siovcnt, diov, diovcnt, &src, &dst);
	     len > 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {


		idxd_vtophys_iter_init(chan, &vtophys_iter, src, dst, len);

		while (len > 0) {
			if (first_op == NULL) {
				rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op = op;
			} else {
				rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op->count++;
				op->parent = first_op;
			}

			count++;

			src_addr = 0;
			dst_addr = 0;
			seg_len = idxd_vtophys_iter_next(&vtophys_iter, &src_addr, &dst_addr);
			if (seg_len == SPDK_VTOPHYS_ERROR) {
				rc = -EFAULT;
				goto error;
			}

			desc->opcode = IDXD_OPCODE_COPY_CRC;     /* [한국어] copy + CRC 통합 opcode */
			desc->dst_addr = dst_addr;
			desc->src_addr = src_addr;
			_update_write_flags(chan, desc);
			if (op == first_op) {
				desc->crc32c.seed = seed;
			} else {
				desc->flags |= IDXD_FLAG_FENCE | IDXD_FLAG_CRC_READ_CRC_SEED;
				desc->crc32c.addr = prev_crc;
			}

			desc->xfer_size = seg_len;
			prev_crc = desc->completion_addr + offsetof(struct dsa_hw_comp_record, crc32c_val);

			len -= seg_len;
		}
	}

	/* Only the last op copies the crc to the destination */
	if (op) {
		op->crc_dst = crc_dst;
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

/*
 * [한국어]
 * _idxd_submit_compress_single — IAA 압축 단일 디스크립터 빌드.
 *
 * IAA 전용 op. desc->iaa.src2_addr = aecs_addr (device init 시 fixed Huffman으로 채운 영역) 사용.
 * IAA_FLAG_RD_SRC2_AECS: 디바이스가 src2를 AECS로 인식하라는 플래그.
 * IAA_COMP_FLAGS: 압축 모드 플래그 (deflate fixed Huffman 등).
 */
static inline int
_idxd_submit_compress_single(struct spdk_idxd_io_channel *chan, void *dst, const void *src,
			     uint64_t nbytes_dst, uint64_t nbytes_src, uint32_t *output_size,
			     int flags, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t src_addr, dst_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, flags, &desc, &op);
	if (rc) {
		return rc;
	}

	rc = _vtophys(chan, src, &src_addr, nbytes_src);   /* [한국어] src 단일 연속 매핑 변환 */
	if (rc) {
		goto error;
	}

	rc = _vtophys(chan, dst, &dst_addr, nbytes_dst);
	if (rc) {
		goto error;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_COMPRESS;
	desc->src1_addr = src_addr;
	desc->dst_addr = dst_addr;
	desc->src1_size = nbytes_src;
	desc->iaa.max_dst_size = nbytes_dst;       /* [한국어] 압축 후 결과가 이 크기 초과 시 디바이스가 에러 반환 */
	desc->iaa.src2_size = sizeof(struct iaa_aecs);
	desc->iaa.src2_addr = chan->idxd->aecs_addr;       /* [한국어] device init 시 IOVA 변환된 AECS 주소 */
	desc->flags |= IAA_FLAG_RD_SRC2_AECS;
	desc->compr_flags = IAA_COMP_FLAGS;
	op->output_size = output_size;     /* [한국어] 완료 후 process_events가 op.iaa_hw.output_size를 *output_size에 기록 */

	_submit_to_hw(chan, op);
	return 0;
error:
	STAILQ_INSERT_TAIL(&chan->ops_pool, op, link);   /* [한국어] 실패 시 op를 free 풀로 반납 */
	return rc;
}

/*
 * [한국어]
 * spdk_idxd_submit_compress — 공개 API. 현재는 단일 iovec만 지원.
 */
int
spdk_idxd_submit_compress(struct spdk_idxd_io_channel *chan,
			  void *dst, uint64_t nbytes,
			  struct iovec *siov, uint32_t siovcnt, uint32_t *output_size,
			  int flags, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	assert(chan != NULL);
	assert(dst != NULL);
	assert(siov != NULL);

	if (siovcnt == 1) {
		/* Simple case - copying one buffer to another */
		if (nbytes < siov[0].iov_len) {
			return -EINVAL;     /* [한국어] dst 크기가 src보다 작으면 압축 후 더 커질 가능성 — 사전 거부 */
		}

		return _idxd_submit_compress_single(chan, dst, siov[0].iov_base,
						    nbytes, siov[0].iov_len,
						    output_size, flags, cb_fn, cb_arg);
	}
	/* TODO: vectored support */
	return -EINVAL;
}

/*
 * [한국어]
 * _idxd_submit_decompress_single — IAA 압축 해제. AECS 사용 안 함 (해제는 stream 헤더만으로 충분).
 */
static inline int
_idxd_submit_decompress_single(struct spdk_idxd_io_channel *chan, void *dst, const void *src,
			       uint64_t nbytes_dst, uint64_t nbytes, int flags, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t src_addr, dst_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, flags, &desc, &op);
	if (rc) {
		return rc;
	}

	rc = _vtophys(chan, src, &src_addr, nbytes);
	if (rc) {
		goto error;
	}

	rc = _vtophys(chan, dst, &dst_addr, nbytes_dst);
	if (rc) {
		goto error;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_DECOMPRESS;
	desc->src1_addr = src_addr;
	desc->dst_addr = dst_addr;
	desc->src1_size = nbytes;
	desc->iaa.max_dst_size = nbytes_dst;
	desc->decompr_flags = IAA_DECOMP_FLAGS;

	_submit_to_hw(chan, op);
	return 0;
error:
	STAILQ_INSERT_TAIL(&chan->ops_pool, op, link);
	return rc;
}

/*
 * [한국어]
 * spdk_idxd_submit_decompress — 공개 API. 단일 iovec만 지원.
 */
int
spdk_idxd_submit_decompress(struct spdk_idxd_io_channel *chan,
			    struct iovec *diov, uint32_t diovcnt,
			    struct iovec *siov, uint32_t siovcnt,
			    int flags, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	assert(chan != NULL);
	assert(diov != NULL);
	assert(siov != NULL);

	if (diovcnt == 1 && siovcnt == 1) {
		/* Simple case - copying one buffer to another */
		if (diov[0].iov_len < siov[0].iov_len) {
			return -EINVAL;
		}

		return _idxd_submit_decompress_single(chan, diov[0].iov_base, siov[0].iov_base,
						      diov[0].iov_len, siov[0].iov_len,
						      flags, cb_fn, cb_arg);
	}
	/* TODO: vectored support */
	return -EINVAL;
}

/*
 * [한국어]
 * idxd_get_dif_flags — guard_interval(=block_size with metadata)에 해당하는 IDXD 플래그 변환.
 *
 * T10 DIF: NVMe namespace의 LBA 끝에 8B/16B PI(Protection Info)를 붙인 형태.
 * guard_interval은 block + metadata 합 크기. DSA가 지원하는 4종(512/520/4096/4104) 중 하나여야 함.
 */
static inline int
idxd_get_dif_flags(const struct spdk_dif_ctx *ctx, uint8_t *flags)
{
	uint32_t data_block_size = ctx->block_size - ctx->md_size;

	if (flags == NULL) {
		SPDK_ERRLOG("Flag should be non-null");
		return -EINVAL;
	}

	assert(ctx->md_interleave);     /* [한국어] DSA는 interleaved metadata만 지원 (separated layout 미지원) */

	switch (ctx->guard_interval) {
	case DATA_BLOCK_SIZE_512:
		*flags = IDXD_DIF_FLAG_DIF_BLOCK_SIZE_512;
		break;
	case DATA_BLOCK_SIZE_520:
		*flags = IDXD_DIF_FLAG_DIF_BLOCK_SIZE_520;
		break;
	case DATA_BLOCK_SIZE_4096:
		*flags = IDXD_DIF_FLAG_DIF_BLOCK_SIZE_4096;
		break;
	case DATA_BLOCK_SIZE_4104:
		*flags = IDXD_DIF_FLAG_DIF_BLOCK_SIZE_4104;
		break;
	default:
		SPDK_ERRLOG("Invalid DIF block size %d\n", data_block_size);
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * idxd_get_source_dif_flags — DIF 소스 측 검사 disable 비트 조합.
 *
 * SPDK_DIF_FLAGS_GUARD_CHECK 비활성 → IDXD에 GUARD_CHECK_DISABLE 비트 set.
 * Type1/2: app_tag=0xFFFF면 검사 skip. Type3: app_tag=0xFFFF + ref_tag=0xFFFFFFFF면 검사 skip.
 */
static inline int
idxd_get_source_dif_flags(const struct spdk_dif_ctx *ctx, uint8_t *flags)
{
	if (flags == NULL) {
		SPDK_ERRLOG("Flag should be non-null");
		return -EINVAL;
	}

	*flags = 0;

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK)) {
		*flags |= IDXD_DIF_SOURCE_FLAG_GUARD_CHECK_DISABLE;
	}

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK)) {
		*flags |= IDXD_DIF_SOURCE_FLAG_REF_TAG_CHECK_DISABLE;
	}

	switch (ctx->dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		/* If Type 1 or 2 is used, then all DIF checks are disabled when
		 * the Application Tag is 0xFFFF.
		 */
		*flags |= IDXD_DIF_SOURCE_FLAG_APP_TAG_F_DETECT;
		break;
	case SPDK_DIF_TYPE3:
		/* If Type 3 is used, then all DIF checks are disabled when the
		 * Application Tag is 0xFFFF and the Reference Tag is 0xFFFFFFFF
		 * (for PI 8 bytes format).
		 */
		*flags |= IDXD_DIF_SOURCE_FLAG_APP_AND_REF_TAG_F_DETECT;
		break;
	default:
		SPDK_ERRLOG("Invalid DIF type %d\n", ctx->dif_type);
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * idxd_get_app_tag_mask — APPTAG 검사 비활성 시 마스크를 0xFFFF로 — 모든 비트 무시(=검사 안 함).
 * 활성 시 사용자 마스크의 보수(~apptag_mask). DIF 사양에서 mask=1 비트가 "검사할 비트".
 */
static inline int
idxd_get_app_tag_mask(const struct spdk_dif_ctx *ctx, uint16_t *app_tag_mask)
{
	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_APPTAG_CHECK)) {
		/* The Source Application Tag Mask may be set to 0xffff
		 * to disable application tag checking */
		*app_tag_mask = 0xFFFF;
	} else {
		*app_tag_mask = ~ctx->apptag_mask;
	}

	return 0;
}

/*
 * [한국어]
 * idxd_validate_dif_common_params — DSA가 지원하는 DIF 설정인지 검증.
 *
 * 제약:
 *  - data_offset = 0 (전체 버퍼 0 오프셋부터).
 *  - guard_seed = 0.
 *  - md_size ∈ {8, 16}.
 *  - DIF PI 포맷 = 16-bit guard.
 *  - md_interleave = true (separated layout 미지원).
 *  - md_size=16 + (block=512 or 4096): "left alignment" 미지원.
 *  - data_block_size ∈ {512, 4096}.
 */
static inline int
idxd_validate_dif_common_params(const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size = ctx->block_size - ctx->md_size;

	/* Check byte offset from the start of the whole data buffer */
	if (ctx->data_offset != 0) {
		SPDK_ERRLOG("Byte offset from the start of the whole data buffer must be set to 0.");
		return -EINVAL;
	}

	/* Check seed value for guard computation */
	if (ctx->guard_seed != 0) {
		SPDK_ERRLOG("Seed value for guard computation must be set to 0.");
		return -EINVAL;
	}

	/* Check for supported metadata sizes */
	if (ctx->md_size != METADATA_SIZE_8 && ctx->md_size != METADATA_SIZE_16)  {
		SPDK_ERRLOG("Metadata size %d is not supported.\n", ctx->md_size);
		return -EINVAL;
	}

	/* Check for supported DIF PI formats */
	if (ctx->dif_pi_format != SPDK_DIF_PI_FORMAT_16) {
		SPDK_ERRLOG("DIF PI format %d is not supported.\n", ctx->dif_pi_format);
		return -EINVAL;
	}

	/* Check for supported metadata locations */
	if (ctx->md_interleave == false) {
		SPDK_ERRLOG("Separated metadata location is not supported.\n");
		return -EINVAL;
	}

	/* Check for supported DIF alignments */
	if (ctx->md_size == METADATA_SIZE_16 &&
	    (ctx->guard_interval == DATA_BLOCK_SIZE_512 ||
	     ctx->guard_interval == DATA_BLOCK_SIZE_4096)) {
		SPDK_ERRLOG("DIF left alignment in metadata is not supported.\n");
		return -EINVAL;
	}

	/* Check for supported DIF block sizes */
	if (data_block_size != DATA_BLOCK_SIZE_512 &&
	    data_block_size != DATA_BLOCK_SIZE_4096) {
		SPDK_ERRLOG("DIF block size %d is not supported.\n", data_block_size);
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * idxd_validate_dif_check_params — dif_check 전용 검증. 현재는 common 검증과 동일.
 */
static inline int
idxd_validate_dif_check_params(const struct spdk_dif_ctx *ctx)
{
	int rc = idxd_validate_dif_common_params(ctx);
	if (rc) {
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * idxd_validate_dif_check_buf_align — 버퍼 길이가 block_size의 배수인지 검증.
 * DSA는 split 처리를 못 하므로 한 iovec entry는 정수개의 블록을 담고 있어야 함.
 */
static inline int
idxd_validate_dif_check_buf_align(const struct spdk_dif_ctx *ctx, const uint64_t len)
{
	/* DSA can only process contiguous memory buffers, multiple of the block size */
	if (len % ctx->block_size != 0) {
		SPDK_ERRLOG("The memory buffer length (%ld) is not a multiple of block size with metadata (%d).\n",
			    len, ctx->block_size);
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_idxd_submit_dif_check — siov[]의 각 블록 PI(가드/AppTag/RefTag)를 검사.
 *
 * num_blocks_done은 ref_tag_seed 누적용 — DIF Type1은 LBA에 따라 ref_tag가 증가하므로
 * 다음 iovec의 시작 LBA를 정확히 맞춰야 함.
 */
int
spdk_idxd_submit_dif_check(struct spdk_idxd_io_channel *chan,
			   struct iovec *siov, size_t siovcnt,
			   uint32_t num_blocks, const struct spdk_dif_ctx *ctx, int flags,
			   spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op = NULL, *op = NULL;
	uint64_t src_seg_addr, src_seg_len;
	uint32_t num_blocks_done = 0;
	uint8_t dif_flags = 0, src_dif_flags = 0;
	uint16_t app_tag_mask = 0;
	int rc, count = 0;
	size_t i;

	assert(ctx != NULL);
	assert(chan != NULL);
	assert(siov != NULL);

	rc = idxd_validate_dif_check_params(ctx);
	if (rc) {
		return rc;
	}

	rc = idxd_get_dif_flags(ctx, &dif_flags);
	if (rc) {
		return rc;
	}

	rc = idxd_get_source_dif_flags(ctx, &src_dif_flags);
	if (rc) {
		return rc;
	}

	rc = idxd_get_app_tag_mask(ctx, &app_tag_mask);
	if (rc) {
		return rc;
	}

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	for (i = 0; i < siovcnt; i++) {
		src_seg_addr = (uint64_t)siov[i].iov_base;     /* [한국어] DIF는 vtophys 안 하고 가상주소 직접 사용 — PASID 가정? */
		src_seg_len = siov[i].iov_len;

		/* DSA processes the iovec buffers independently, so the buffers cannot
		 * be split (must be multiple of the block size) */

		/* Validate the memory buffer alignment */
		rc = idxd_validate_dif_check_buf_align(ctx, src_seg_len);
		if (rc) {
			goto error;
		}

		if (first_op == NULL) {
			rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
			if (rc) {
				goto error;
			}

			first_op = op;
		} else {
			rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
			if (rc) {
				goto error;
			}

			first_op->count++;
			op->parent = first_op;
		}

		count++;

		desc->opcode = IDXD_OPCODE_DIF_CHECK;
		desc->src_addr = src_seg_addr;
		desc->xfer_size = src_seg_len;
		desc->dif_chk.flags = dif_flags;
		desc->dif_chk.src_flags = src_dif_flags;
		desc->dif_chk.app_tag_seed = ctx->app_tag;
		desc->dif_chk.app_tag_mask = app_tag_mask;
		desc->dif_chk.ref_tag_seed = (uint32_t)ctx->init_ref_tag + num_blocks_done;
		/* [한국어] DIF Type1은 LBA마다 ref_tag가 +1 증가 — num_blocks_done으로 누적 */

		num_blocks_done += (src_seg_len / ctx->block_size);
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

/*
 * [한국어]
 * idxd_validate_dif_insert_params — DIF insert에서는 모든 검사 플래그(GUARD/APP/REF)가 set이어야 함.
 * insert는 PI 필드를 새로 생성하므로 모든 필드가 의미가 있어야 함.
 */
static inline int
idxd_validate_dif_insert_params(const struct spdk_dif_ctx *ctx)
{
	int rc = idxd_validate_dif_common_params(ctx);
	if (rc) {
		return rc;
	}

	/* Check for required DIF flags */
	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK))  {
		SPDK_ERRLOG("Guard check flag must be set.\n");
		return -EINVAL;
	}

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_APPTAG_CHECK))  {
		SPDK_ERRLOG("Application Tag check flag must be set.\n");
		return -EINVAL;
	}

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK))  {
		SPDK_ERRLOG("Reference Tag check flag must be set.\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * idxd_validate_dif_insert_iovecs — diov(insert 후) 길이 = siov(insert 전) 길이 + num_blocks * md_size.
 * insert는 데이터 블록마다 md_size 바이트의 PI를 추가하므로 dst가 src보다 num_blocks*md_size 만큼 큼.
 */
static inline int
idxd_validate_dif_insert_iovecs(const struct spdk_dif_ctx *ctx,
				const struct iovec *diov, const size_t diovcnt,
				const struct iovec *siov, const size_t siovcnt)
{
	uint32_t data_block_size = ctx->block_size - ctx->md_size;
	size_t src_len, dst_len;
	uint32_t num_blocks;
	size_t i;

	if (diovcnt != siovcnt) {
		SPDK_ERRLOG("Invalid number of elements in src (%ld) and dst (%ld) iovecs.\n",
			    siovcnt, diovcnt);
		return -EINVAL;
	}

	for (i = 0; i < siovcnt; i++) {
		src_len = siov[i].iov_len;
		dst_len = diov[i].iov_len;
		num_blocks = src_len / data_block_size;
		if (src_len != dst_len - num_blocks * ctx->md_size) {
			SPDK_ERRLOG("Invalid length of data in src (%ld) and dst (%ld) in iovecs[%ld].\n",
				    src_len, dst_len, i);
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * [한국어]
 * idxd_validate_dif_insert_buf_align — src는 data_block_size 배수, dst는 block_size 배수,
 * 그리고 동일한 블록 수.
 */
static inline int
idxd_validate_dif_insert_buf_align(const struct spdk_dif_ctx *ctx,
				   const uint64_t src_len, const uint64_t dst_len)
{
	uint32_t data_block_size = ctx->block_size - ctx->md_size;

	/* DSA can only process contiguous memory buffers, multiple of the block size */
	if (src_len % data_block_size != 0) {
		SPDK_ERRLOG("The memory source buffer length (%ld) is not a multiple of block size without metadata (%d).\n",
			    src_len, data_block_size);
		return -EINVAL;
	}

	if (dst_len % ctx->block_size != 0) {
		SPDK_ERRLOG("The memory destination buffer length (%ld) is not a multiple of block size with metadata (%d).\n",
			    dst_len, ctx->block_size);
		return -EINVAL;
	}

	/* The memory source and destination must hold the same number of blocks. */
	if (src_len / data_block_size != (dst_len / ctx->block_size)) {
		SPDK_ERRLOG("The memory source (%ld) and destination (%ld) must hold the same number of blocks.\n",
			    src_len / data_block_size, dst_len / ctx->block_size);
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_idxd_submit_dif_insert — 데이터에 PI 필드 삽입 (생성).
 * 디바이스가 각 블록의 끝에 8B/16B PI(guard, app_tag, ref_tag)를 계산해 dst에 기록.
 */
int
spdk_idxd_submit_dif_insert(struct spdk_idxd_io_channel *chan,
			    struct iovec *diov, size_t diovcnt,
			    struct iovec *siov, size_t siovcnt,
			    uint32_t num_blocks, const struct spdk_dif_ctx *ctx, int flags,
			    spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op = NULL, *op = NULL;
	uint32_t data_block_size = ctx->block_size - ctx->md_size;
	uint64_t src_seg_addr, src_seg_len;
	uint64_t dst_seg_addr, dst_seg_len;
	uint32_t num_blocks_done = 0;
	uint8_t dif_flags = 0;
	int rc, count = 0;
	size_t i;

	assert(ctx != NULL);
	assert(chan != NULL);
	assert(siov != NULL);

	rc = idxd_validate_dif_insert_params(ctx);
	if (rc) {
		return rc;
	}

	rc = idxd_validate_dif_insert_iovecs(ctx, diov, diovcnt, siov, siovcnt);
	if (rc) {
		return rc;
	}

	rc = idxd_get_dif_flags(ctx, &dif_flags);
	if (rc) {
		return rc;
	}

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	for (i = 0; i < siovcnt; i++) {
		src_seg_addr = (uint64_t)siov[i].iov_base;
		src_seg_len = siov[i].iov_len;
		dst_seg_addr = (uint64_t)diov[i].iov_base;
		dst_seg_len = diov[i].iov_len;

		/* DSA processes the iovec buffers independently, so the buffers cannot
		 * be split (must be multiple of the block size). The destination memory
		 * size needs to be same as the source memory size + metadata size */

		rc = idxd_validate_dif_insert_buf_align(ctx, src_seg_len, dst_seg_len);
		if (rc) {
			goto error;
		}

		if (first_op == NULL) {
			rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
			if (rc) {
				goto error;
			}

			first_op = op;
		} else {
			rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
			if (rc) {
				goto error;
			}

			first_op->count++;
			op->parent = first_op;
		}

		count++;

		desc->opcode = IDXD_OPCODE_DIF_INS;
		desc->src_addr = src_seg_addr;
		desc->dst_addr = dst_seg_addr;
		desc->xfer_size = src_seg_len;
		desc->dif_ins.flags = dif_flags;
		desc->dif_ins.app_tag_seed = ctx->app_tag;
		desc->dif_ins.app_tag_mask = ~ctx->apptag_mask;
		desc->dif_ins.ref_tag_seed = (uint32_t)ctx->init_ref_tag + num_blocks_done;

		num_blocks_done += src_seg_len / data_block_size;
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

/*
 * [한국어]
 * idxd_validate_dif_strip_buf_align — strip(PI 제거)에서는 src가 block_size 배수, dst가 data_block_size 배수.
 */
static inline int
idxd_validate_dif_strip_buf_align(const struct spdk_dif_ctx *ctx,
				  const uint64_t src_len, const uint64_t dst_len)
{
	uint32_t data_block_size = ctx->block_size - ctx->md_size;

	/* DSA can only process contiguous memory buffers, multiple of the block size. */
	if (src_len % ctx->block_size != 0) {
		SPDK_ERRLOG("The src buffer length (%ld) is not a multiple of block size (%d).\n",
			    src_len, ctx->block_size);
		return -EINVAL;
	}
	if (dst_len % data_block_size != 0) {
		SPDK_ERRLOG("The dst buffer length (%ld) is not a multiple of block size without metadata (%d).\n",
			    dst_len, data_block_size);
		return -EINVAL;
	}
	/* The memory source and destination must hold the same number of blocks. */
	if (src_len / ctx->block_size != dst_len / data_block_size) {
		SPDK_ERRLOG("The memory source (%ld) and destination (%ld) must hold the same number of blocks.\n",
			    src_len / data_block_size, dst_len / ctx->block_size);
		return -EINVAL;
	}
	return 0;
}

/*
 * [한국어]
 * spdk_idxd_submit_dif_strip — PI를 검사 후 제거. 디바이스가 검사 실패 시 op->hw.status에 IDXD_DSA_STATUS_DIF_ERROR.
 */
int
spdk_idxd_submit_dif_strip(struct spdk_idxd_io_channel *chan,
			   struct iovec *diov, size_t diovcnt,
			   struct iovec *siov, size_t siovcnt,
			   uint32_t num_blocks, const struct spdk_dif_ctx *ctx, int flags,
			   spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op = NULL, *op = NULL;
	uint64_t src_seg_addr, src_seg_len;
	uint64_t dst_seg_addr, dst_seg_len;
	uint8_t dif_flags = 0, src_dif_flags = 0;
	uint16_t app_tag_mask = 0;
	int rc, count = 0;
	size_t i;

	rc = idxd_validate_dif_common_params(ctx);
	if (rc) {
		return rc;
	}

	rc = idxd_get_dif_flags(ctx, &dif_flags);
	if (rc) {
		return rc;
	}

	rc = idxd_get_source_dif_flags(ctx, &src_dif_flags);
	if (rc) {
		return rc;
	}

	rc = idxd_get_app_tag_mask(ctx, &app_tag_mask);
	if (rc) {
		return rc;
	}

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	if (diovcnt != siovcnt) {
		SPDK_ERRLOG("Mismatched iovcnts: src=%ld, dst=%ld\n",
			    siovcnt, diovcnt);
		return -EINVAL;
	}

	for (i = 0; i < siovcnt; i++) {
		src_seg_addr = (uint64_t)siov[i].iov_base;
		src_seg_len = siov[i].iov_len;
		dst_seg_addr = (uint64_t)diov[i].iov_base;
		dst_seg_len = diov[i].iov_len;

		/* DSA processes the iovec buffers independently, so the buffers cannot
		 * be split (must be multiple of the block size). The source memory
		 * size needs to be same as the destination memory size + metadata size */

		rc = idxd_validate_dif_strip_buf_align(ctx, src_seg_len, dst_seg_len);
		if (rc) {
			goto error;
		}

		if (first_op == NULL) {
			rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
			if (rc) {
				goto error;
			}

			first_op = op;
		} else {
			rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
			if (rc) {
				goto error;
			}

			first_op->count++;
			op->parent = first_op;
		}

		count++;

		desc->opcode = IDXD_OPCODE_DIF_STRP;
		desc->src_addr = src_seg_addr;
		desc->dst_addr = dst_seg_addr;
		desc->xfer_size = src_seg_len;
		desc->dif_strip.flags = dif_flags;
		desc->dif_strip.src_flags = src_dif_flags;
		desc->dif_strip.app_tag_seed = ctx->app_tag;
		desc->dif_strip.app_tag_mask = app_tag_mask;
		desc->dif_strip.ref_tag_seed = (uint32_t)ctx->init_ref_tag;
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

/*
 * [한국어]
 * idxd_get_dix_flags — DIX(separated metadata)에서는 block_size 자체가 data_block_size.
 * DSA가 지원하는 DIX 블록 크기: 512, 4096.
 */
static inline int
idxd_get_dix_flags(const struct spdk_dif_ctx *ctx, uint8_t *flags)
{
	uint32_t data_block_size = ctx->block_size;

	assert(!ctx->md_interleave);     /* [한국어] DIX는 separated layout 전용 */

	if (flags == NULL) {
		SPDK_ERRLOG("Flag should be non-null");
		return -EINVAL;
	}

	switch (data_block_size) {
	case DATA_BLOCK_SIZE_512:
		*flags = IDXD_DIF_FLAG_DIF_BLOCK_SIZE_512;
		break;
	case DATA_BLOCK_SIZE_4096:
		*flags = IDXD_DIF_FLAG_DIF_BLOCK_SIZE_4096;
		break;
	default:
		SPDK_ERRLOG("Invalid DIX block size %d\n", data_block_size);
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * idxd_validate_dix_generate_params — DIX generate는 모든 검사 플래그가 set이어야 하고, md_size=8만 지원.
 */
static inline int
idxd_validate_dix_generate_params(const struct spdk_dif_ctx *ctx)
{
	/* Check for required DIF flags. Intel DSA is able to only generate all DIF fields. */
	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK))  {
		SPDK_ERRLOG("Guard check flag must be set.\n");
		return -EINVAL;
	}

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_APPTAG_CHECK))  {
		SPDK_ERRLOG("Application Tag check flag must be set.\n");
		return -EINVAL;
	}

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK))  {
		SPDK_ERRLOG("Reference Tag check flag must be set.\n");
		return -EINVAL;
	}

	/* Check byte offset from the start of the whole data buffer */
	if (ctx->data_offset != 0) {
		SPDK_ERRLOG("Byte offset from the start of the whole data buffer must be set to 0.");
		return -EINVAL;
	}

	/* Check seed value for guard computation */
	if (ctx->guard_seed != 0) {
		SPDK_ERRLOG("Seed value for guard computation must be set to 0.");
		return -EINVAL;
	}

	/* Check for supported metadata sizes */
	if (ctx->md_size != METADATA_SIZE_8)  {
		SPDK_ERRLOG("Metadata size %d is not supported.\n", ctx->md_size);
		return -EINVAL;
	}

	/* Check for supported DIF PI formats */
	if (ctx->dif_pi_format != SPDK_DIF_PI_FORMAT_16) {
		SPDK_ERRLOG("DIF PI format %d is not supported.\n", ctx->dif_pi_format);
		return -EINVAL;
	}

	/* Check for supported DIF block sizes */
	if (ctx->block_size != DATA_BLOCK_SIZE_512 &&
	    ctx->block_size != DATA_BLOCK_SIZE_4096) {
		SPDK_ERRLOG("DIF block size %d is not supported.\n", ctx->block_size);
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_idxd_submit_dix_generate — DIX(separated metadata) PI 필드를 mdiov에 생성.
 *
 * src(siov)는 데이터, dst(mdiov)는 메타데이터 버퍼(연속). 각 블록마다 8B PI를 mdiov에 추가.
 */
int
spdk_idxd_submit_dix_generate(struct spdk_idxd_io_channel *chan, struct iovec *siov,
			      size_t siovcnt, struct iovec *mdiov, uint32_t num_blocks,
			      const struct spdk_dif_ctx *ctx, int flags,
			      spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op = NULL, *op = NULL;
	uint64_t src_seg_addr, src_seg_len;
	uint64_t md_seg_addr, md_seg_len;
	uint32_t num_blocks_done = 0;
	uint8_t dif_flags = 0;
	uint16_t app_tag_mask = 0;
	int rc, count = 0;
	size_t i;

	rc = idxd_validate_dix_generate_params(ctx);
	if (rc) {
		return rc;
	}

	rc = idxd_get_dix_flags(ctx, &dif_flags);
	if (rc) {
		return rc;
	}

	rc = idxd_get_app_tag_mask(ctx, &app_tag_mask);
	if (rc) {
		return rc;
	}

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	md_seg_len = mdiov->iov_len;
	md_seg_addr = (uint64_t)mdiov->iov_base;

	if (md_seg_len % ctx->md_size != 0) {
		SPDK_ERRLOG("The metadata buffer length (%ld) is not a multiple of metadata size.\n",
			    md_seg_len);
		return -EINVAL;
	}

	for (i = 0; i < siovcnt; i++) {
		src_seg_addr = (uint64_t)siov[i].iov_base;
		src_seg_len = siov[i].iov_len;

		if (src_seg_len % ctx->block_size != 0) {
			SPDK_ERRLOG("The source buffer length (%ld) is not a multiple of block size (%d).\n",
				    src_seg_len, ctx->block_size);
			goto error;
		}

		if (first_op == NULL) {
			rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
			if (rc) {
				goto error;
			}

			first_op = op;
		} else {
			rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
			if (rc) {
				goto error;
			}

			first_op->count++;
			op->parent = first_op;
		}

		count++;

		desc->opcode = IDXD_OPCODE_DIX_GEN;
		desc->src_addr = src_seg_addr;
		desc->dst_addr = md_seg_addr;       /* [한국어] DIX는 dst가 메타데이터 영역 — md_seg_addr이 점진적으로 증가 */
		desc->xfer_size = src_seg_len;
		desc->dix_gen.flags = dif_flags;
		desc->dix_gen.app_tag_seed = ctx->app_tag;
		desc->dix_gen.app_tag_mask = ~ctx->apptag_mask;
		desc->dix_gen.ref_tag_seed = (uint32_t)ctx->init_ref_tag + num_blocks_done;

		num_blocks_done += src_seg_len / ctx->block_size;

		md_seg_addr = (uint64_t)mdiov->iov_base + (num_blocks_done * ctx->md_size);
		/* [한국어] 다음 sub-op의 메타 dst 시작점 = mdiov 베이스 + 누적 블록 수 * md_size */
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

/*
 * [한국어]
 * spdk_idxd_submit_raw_desc — 사용자가 작성한 임의의 idxd_hw_desc를 그대로 제출.
 *
 * 호출자가 직접 opcode/주소/플래그를 설정한 desc를 넘기면, 이 함수는 ops_pool에서 슬롯 하나 꺼내
 * 사용자 desc를 복사한 뒤 completion_addr 등 인프라 필드만 보존. 디바이스 실험/프로파일링 용도.
 */
int
spdk_idxd_submit_raw_desc(struct spdk_idxd_io_channel *chan,
			  struct idxd_hw_desc *_desc,
			  spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	int rc, flags = 0;
	uint64_t comp_addr;

	assert(chan != NULL);
	assert(_desc != NULL);

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, flags, &desc, &op);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	flags = desc->flags;       /* [한국어] prep_command가 set한 COMPLETION_VALID/REQUEST_COMPLETION 보존 */
	comp_addr = desc->completion_addr;
	memcpy(desc, _desc, sizeof(*desc));   /* [한국어] 사용자 desc 전체 복사 */
	desc->flags |= flags;       /* [한국어] 사용자 flags + 인프라 flags 합성 */
	desc->completion_addr = comp_addr;    /* [한국어] 인프라 IOVA 복원 */

	/* Submit operation. */
	_submit_to_hw(chan, op);

	return 0;
}

/*
 * [한국어]
 * _dump_sw_error_reg — 백엔드 dump_sw_error 콜백 호출.
 * 디바이스 sw_err 레지스터 dump — 디버깅 정보.
 */
static inline void
_dump_sw_error_reg(struct spdk_idxd_io_channel *chan)
{
	struct spdk_idxd_device *idxd = chan->idxd;

	assert(idxd != NULL);
	idxd->impl->dump_sw_error(idxd, chan->portal);
}

/* TODO: more performance experiments. */
/* [한국어] 완료 record의 status 필드 인터프리터 매크로.
 * IDXD_COMPLETION(x): x>0이면 완료 (디바이스가 0이 아닌 status 기록).
 * IDXD_FAILURE(x): x>1이면 실패 (1=정상 완료, 그 외 코드는 에러).
 * IDXD_SW_ERROR(x): &= 0x1로 SW 에러 비트 검사 (현재 미사용). */
#define IDXD_COMPLETION(x) ((x) > (0) ? (1) : (0))
#define IDXD_FAILURE(x) ((x) > (1) ? (1) : (0))
#define IDXD_SW_ERROR(x) ((x) &= (0x1) ? (1) : (0))

/*
 * [한국어]
 * spdk_idxd_process_events — 채널 완료 폴링. SPDK reactor poller가 주기적으로 호출.
 *
 * 흐름:
 *   1) ops_outstanding STAILQ를 head부터 SAFE 순회 (in-order 완료 가정).
 *   2) op->hw.status가 0(미완료)이면 break — 디바이스가 in-order로 완료하므로 head가 미완료면 뒤도 미완료.
 *   3) STAILQ에서 제거.
 *   4) FAILURE면 _dump_sw_error_reg로 디버그 dump 후 status=-EINVAL.
 *   5) opcode별 결과 추출:
 *      - BATCH: 로그만.
 *      - CRC32C_GEN/COPY_CRC: op->hw.crc32c_val을 *op->crc_dst에 ^= ~0 후 기록 (CRC 종결 XOR).
 *      - COMPARE: op->hw.result(0=match, 1=mismatch)를 status로.
 *      - COMPRESS: op->iaa_hw.output_size를 *op->output_size에.
 *      - DIF_CHECK/DIF_STRP: status==DIF_ERROR면 status=-EIO.
 *   6) op->hw.status=0 (재사용 위해 클리어).
 *   7) op->count--. parent_op이 있으면 parent->count도 -- → 0이면 batch refcnt-- → 0이면 _free_batch + parent cb_fn 호출.
 *   8) op->count==0이면 (단일 op이거나 마지막 sub-op) cb_fn 호출, batch가 없으면 ops_pool로 반납.
 *   9) IDXD_MAX_COMPLETIONS(128) 초과하면 break — 다른 poller fairness.
 *  10) chan->batch가 남아있으면 idxd_batch_submit으로 flush 시도.
 *
 * 반환: 처리한 op 수.
 */
int
spdk_idxd_process_events(struct spdk_idxd_io_channel *chan)
{
	struct idxd_ops *op, *tmp, *parent_op;
	int status = 0;
	int rc2, rc = 0;
	void *cb_arg;
	spdk_idxd_req_cb cb_fn;

	assert(chan != NULL);

	STAILQ_FOREACH_SAFE(op, &chan->ops_outstanding, link, tmp) {
		if (!IDXD_COMPLETION(op->hw.status)) {
			/*
			 * oldest locations are at the head of the list so if
			 * we've polled a location that hasn't completed, bail
			 * now as there are unlikely to be any more completions.
			 */
			/* [한국어] in-order 완료 가정 — head 미완료면 뒤도 미완료. 즉시 break하여 캐시 효율적. */
			break;
		}

		STAILQ_REMOVE_HEAD(&chan->ops_outstanding, link);
		rc++;

		/* Status is in the same location for both IAA and DSA completion records. */
		if (spdk_unlikely(IDXD_FAILURE(op->hw.status))) {
			SPDK_ERRLOG("Completion status 0x%x\n", op->hw.status);
			status = -EINVAL;
			_dump_sw_error_reg(chan);     /* [한국어] sw_err 레지스터 dump — 디버깅 */
		}

		switch (op->desc->opcode) {
		case IDXD_OPCODE_BATCH:
			SPDK_DEBUGLOG(idxd, "Complete batch %p\n", op->batch);
			break;
		case IDXD_OPCODE_CRC32C_GEN:
		case IDXD_OPCODE_COPY_CRC:
			if (spdk_likely(status == 0 && op->crc_dst != NULL)) {
				*op->crc_dst = op->hw.crc32c_val;
				*op->crc_dst ^= ~0;
				/* [한국어] CRC32-C 표준 종결: 결과를 ~0(=0xFFFFFFFF)와 XOR (one's complement). */
			}
			break;
		case IDXD_OPCODE_COMPARE:
			if (spdk_likely(status == 0)) {
				status = op->hw.result;
				/* [한국어] op->hw.result: 0=match, 1=mismatch. status에 그대로 전달. */
			}
			break;
		case IDXD_OPCODE_COMPRESS:
			if (spdk_likely(status == 0 && op->output_size != NULL)) {
				*op->output_size = op->iaa_hw.output_size;
				/* [한국어] 압축 후 실제 dst 크기 반환 */
			}
			break;
		case IDXD_OPCODE_DIF_CHECK:
		case IDXD_OPCODE_DIF_STRP:
			if (spdk_unlikely(op->hw.status == IDXD_DSA_STATUS_DIF_ERROR)) {
				status = -EIO;
				/* [한국어] DIF 검사 실패 — block의 PI 불일치. NVMe-oF target은 host에 EIO 반환. */
			}
			break;
		}

		/* TODO: WHAT IF THIS FAILED!? */
		op->hw.status = 0;     /* [한국어] op 재사용 위해 status 클리어 */

		assert(op->count > 0);
		op->count--;

		parent_op = op->parent;
		if (parent_op != NULL) {
			assert(parent_op->count > 0);
			parent_op->count--;

			if (parent_op->count == 0) {
				/* [한국어] 모든 sub-op 완료 — parent 콜백 호출 시점 */
				cb_fn = parent_op->cb_fn;
				cb_arg = parent_op->cb_arg;

				assert(parent_op->batch != NULL);

				/*
				 * Now that parent_op count is 0, we can release its ref
				 * to its batch. We have not released the ref to the batch
				 * that the op is pointing to yet, which will be done below.
				 */
				parent_op->batch->refcnt--;
				if (parent_op->batch->refcnt == 0) {
					_free_batch(parent_op->batch, chan);
					/* [한국어] batch의 모든 sub-op이 완료 — 풀로 반납 */
				}

				if (cb_fn) {
					cb_fn(cb_arg, status);
					/* [한국어] 사용자 콜백 호출 — reactor 컨텍스트에서 실행 */
				}
			}
		}

		if (op->count == 0) {
			cb_fn = op->cb_fn;
			cb_arg = op->cb_arg;

			if (op->batch != NULL) {
				assert(op->batch->refcnt > 0);
				op->batch->refcnt--;

				if (op->batch->refcnt == 0) {
					_free_batch(op->batch, chan);
				}
			} else {
				STAILQ_INSERT_HEAD(&chan->ops_pool, op, link);
				/* [한국어] 단일 op — ops_pool head에 반납하여 빠르게 재사용 (cache-hot) */
			}

			if (cb_fn) {
				cb_fn(cb_arg, status);
			}
		}

		/* reset the status */
		status = 0;
		/* break the processing loop to prevent from starving the rest of the system */
		if (rc > IDXD_MAX_COMPLETIONS) {
			break;     /* [한국어] reactor fairness — 다음 round에서 계속 */
		}
	}

	/* Submit any built-up batch */
	if (chan->batch) {
		rc2 = idxd_batch_submit(chan, NULL, NULL);
		if (rc2) {
			assert(rc2 == -EBUSY);
		}
		/* [한국어] 누적된 batch가 있으면 폴링 끝에 flush 시도 — flush_batch가 임계 미만이라 미제출된 케이스 회수 */
	}

	return rc;
}

/*
 * [한국어]
 * idxd_impl_register — backend(user/kernel)가 SPDK_IDXD_IMPL_REGISTER 매크로로 호출.
 * STAILQ_INSERT_HEAD로 g_idxd_impls에 등록 — 마지막 등록자가 head에 위치 (선두 검색 시 우선).
 *
 * __attribute__((constructor)) 매크로를 통해 main 진입 전 자동 호출.
 */
void
idxd_impl_register(struct spdk_idxd_impl *impl)
{
	STAILQ_INSERT_HEAD(&g_idxd_impls, impl, link);
}

/* [한국어] SPDK 로그 컴포넌트 등록 — SPDK_DEBUGLOG(idxd, ...) 활성화 */
SPDK_LOG_REGISTER_COMPONENT(idxd)
