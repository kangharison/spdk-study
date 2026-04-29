/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */


/** \file
 * SPDK DMA device framework
 */

/*
 * [한국어 설명] SPDK DMA memory domain 추상화 공개 API (dma.h) — 약 472 라인
 *
 * === 파일의 역할 ===
 * SPDK가 다양한 종류의 메모리(host RAM, GPU memory, RDMA registered, NVMe CMB
 * 등)를 통합적으로 표현·번역할 수 있도록 하는 "memory domain" 추상화의 공개
 * 인터페이스. bdev I/O를 처리할 때 데이터 버퍼가 단순한 host process 메모리에
 * 있다는 가정을 깨고, GPU에 있을 수도, RDMA NIC에 등록된 영역일 수도, NVMe
 * CMB(Controller Memory Buffer)일 수도 있다는 다양성을 허용한다. 각 memory
 * domain은 (1) DMA 디바이스 type (2) translate/pull/push/transfer/memzero/
 * invalidate 콜백 묶음으로 정의되며, bdev/accel framework는 호출 시 도메인의
 * 콜백을 디스패치하여 실제 데이터를 호스트로 가져오거나(pull) 보내거나(push),
 * 또는 zero-copy로 source→destination 도메인 간 직접 전송(transfer)한다.
 * translate 콜백은 데이터 이동 없이 같은 물리 메모리를 다른 도메인의 주소
 * 표현(예: RDMA의 lkey/rkey)으로 변환만 하므로 실제 zero-copy I/O를 가능하게
 * 한다. accel_sequence와 결합하면 호스트 메모리를 거치지 않는 GPU↔SSD 직접
 * 데이터 경로를 구성할 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * bdev/nvme/nvmf/accel/RDMA 전반에서 사용되는 cross-cutting 추상화.
 * 호출 흐름:
 *   - 등록 측: 각 transport/accel 모듈이 spdk_memory_domain_create()로 자신의
 *              도메인 생성(예: lib/nvme/nvme_rdma.c가 PD 단위 RDMA 도메인 등록).
 *   - 사용 측: bdev_io를 제출할 때 호출자가 spdk_bdev_ext_io_opts.memory_domain
 *              필드로 자기 데이터의 출처 domain을 명시.
 *   - 디스패치: bdev/transport는 데이터 버퍼가 자기 도메인에 직접 접근 가능한지
 *              판단 → 가능하면 translate로 zero-copy, 불가하면 pull/push로 복사.
 *   - 완료: cpl_cb(ctx, rc) 콜백을 (대개) 호출 측 SPDK thread에서 호출.
 * 이 추상화 덕분에 동일한 bdev API로 host 버퍼와 GPU 버퍼를 모두 다룰 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/assert.h, spdk/queue.h(전역 도메인 리스트 TAILQ),
 *             spdk/stdinc.h(uint*_t/size_t/iovec).
 * 의존(구현): lib/dma/dma.c가 본 API들의 실체 제공. 시스템 도메인
 *             (host process 메모리)을 init 시 1개 자동 등록.
 * 의존하는 모듈:
 *   - lib/bdev/bdev.c — bdev_io의 memory_domain 필드 디스패치.
 *   - lib/nvme/* — NVMe-oF RDMA에서 PD 단위 도메인 등록.
 *   - lib/nvmf/* — target 측 transport 도메인.
 *   - lib/accel/* — DMA engine accel domain (가속기 도메인).
 *   - module/bdev/nvme — controller 단위 도메인 라우팅.
 * 데이터 흐름:
 *   사용자 버퍼 (any domain) → bdev API + memory_domain 명시 → bdev/transport가
 *   translate(zero-copy) 또는 pull/push(복사) 결정 → 디바이스 DMA 또는 RDMA WR.
 *
 * === 주요 함수/구조체 요약 ===
 *   - enum spdk_dma_device_type:        RDMA / DMA / ACCEL / vendor-specific 범위.
 *   - struct spdk_memory_domain:        opaque 도메인 객체 (forward 선언).
 *   - struct spdk_memory_domain_ctx:    create 시 사용자 컨텍스트 묶음 (size+user_ctx).
 *   - struct spdk_memory_domain_rdma_ctx: RDMA용 user_ctx 캐스팅 타입 (ibv_pd 보유).
 *   - struct spdk_memory_domain_translation_result: translate 결과 (iov + lkey/rkey).
 *   - struct spdk_memory_domain_translation_ctx:    translate 호출 시 dst 측 ancillary
 *                                                  (예: ibv_qp).
 *   - typedef 콜백 6종:
 *       _data_cpl_cb       — pull/push/memzero 비동기 완료 알림.
 *       _pull_data_cb      — src_domain → local 으로 비동기 데이터 끌어오기.
 *       _push_data_cb      — local → dst_domain 으로 비동기 데이터 보내기.
 *       _memzero_cb        — 도메인 내 영역 0으로 채우기.
 *       _transfer_data_cb  — src_domain → dst_domain 직접 전송 (zero-copy 가능).
 *       _translate_memory_cb — 데이터 이동 없이 주소 표현만 변환.
 *       _invalidate_data_cb — translate 결과에 대한 invalidate (선택).
 *   - 도메인 lifecycle: create / destroy.
 *   - 콜백 setter: set_translation/_invalidate/_pull/_push/_data_transfer/_memzero.
 *   - 컨텍스트 getter: get_context / get_user_context / get_dma_device_type/_id.
 *   - 데이터 연산 진입점: pull_data / push_data / transfer_data / translate_data /
 *                       invalidate_data / memzero.
 *   - 도메인 순회: get_first / get_next / get_system_domain.
 */

#ifndef SPDK_DMA_H            /* [한국어] include 가드 */
#define SPDK_DMA_H

#include "spdk/assert.h"      /* [한국어] SPDK_STATIC_ASSERT 등 컴파일 타임 검증 매크로 */
#include "spdk/queue.h"       /* [한국어] BSD TAILQ — 전역 memory_domain 리스트 연결용 (구현부에서 사용) */
#include "spdk/stdinc.h"      /* [한국어] uint*_t, size_t, struct iovec 등 표준 타입 */

#ifdef __cplusplus
extern "C" {                  /* [한국어] C++에서도 사용 가능하도록 C 링키지 강제 */
#endif

/**
 * Identifier of SPDK internal DMA device of RDMA type
 */
/* [한국어] SPDK 내부에서 RDMA 도메인을 식별하기 위해 사용하는 고정 문자열.
 * spdk_memory_domain_create(.., "SPDK_RDMA_DMA_DEVICE")로 등록하고
 * spdk_memory_domain_get_first(SPDK_RDMA_DMA_DEVICE)로 검색.
 * RDMA transport(NVMe-oF rdma 등) 모듈들이 공유. */
#define SPDK_RDMA_DMA_DEVICE "SPDK_RDMA_DMA_DEVICE"

/* [한국어] DMA 디바이스 종류 — 도메인이 어떤 종류의 메모리/주소 공간을 다루는지 분류.
 * bdev/transport가 도메인 호환성 판단 시 이 값으로 분기. */
enum spdk_dma_device_type {
	/** RDMA devices are capable of performing DMA operations on memory domains using the standard
	 *  RDMA model (protection domain, remote key, address). */
	SPDK_DMA_DEVICE_TYPE_RDMA,
	/* [한국어] RDMA 모델 — Protection Domain(PD) + lkey/rkey + virtual address.
	 * NVMe-oF RDMA, RDMA-aware bdev에서 사용. translate 결과에 lkey/rkey 포함. */

	/** DMA devices are capable of performing DMA operations on memory domains using physical or
	 *  I/O virtual addresses. */
	SPDK_DMA_DEVICE_TYPE_DMA,
	/* [한국어] 일반 DMA 모델 — physical 또는 IOVA 기반 주소.
	 * NVMe PCIe(PRP/SGL), CMB(Controller Memory Buffer) 등에서 사용. */

	/** Virtual memory domain representing memory being transformed by accel framework */
	SPDK_DMA_DEVICE_TYPE_ACCEL,
	/* [한국어] accel framework가 변환 중인 가상 도메인.
	 * crypto/checksum/copy chain 같은 가속 sequence가 결과 버퍼를 표현할 때 사용 —
	 * 실제 메모리는 sequence 실행 후에야 확정. */

	/**
	 * Start of the range of vendor-specific DMA device types
	 */
	SPDK_DMA_DEVICE_VENDOR_SPECIFIC_TYPE_START = 1000,
	/* [한국어] 벤더 정의 type 범위의 시작 (1000~1999).
	 * 외부 모듈이 자체 도메인 type을 만들 때 이 범위 내에서 선택하여
	 * SPDK 표준 type과 충돌 회피. */

	/**
	 * End of the range of vendor-specific DMA device types
	 */
	SPDK_DMA_DEVICE_VENDOR_SPECIFIC_TYPE_END = SPDK_DMA_DEVICE_VENDOR_SPECIFIC_TYPE_START + 999
	/* [한국어] 벤더 정의 type 범위의 끝 (1999). 총 1000개 슬롯. */
};

/* [한국어] memory domain의 본체 — opaque, 정의는 lib/dma/dma.c 내부.
 * 사용자는 spdk_memory_domain_create가 반환한 포인터만 다룸. */
struct spdk_memory_domain;

/**
 * Definition of completion callback to be called by pull, push or memzero functions.
 *
 * \param ctx User context passed to pull of push functions
 * \param rc Result of asynchronous data pull or push function
 */
/*
 * [한국어]
 * spdk_memory_domain_data_cpl_cb - pull/push/memzero 비동기 완료 알림 콜백.
 *
 * @ctx: 호출 시 전달한 사용자 컨텍스트 (cpl_cb_arg).
 * @rc:  결과 코드 (0 성공, 음수 errno 실패).
 *
 * 호출 컨텍스트: 도메인 구현에 따라 다르지만, 보통 호출 측 SPDK thread에서
 *               안전하게 실행되도록 보장 (cross-thread 디스패치는 도메인 구현 책임).
 * 콜백 내에서는 빠르고 non-blocking이어야 하며, 추가 I/O 제출은 가능.
 */
typedef void (*spdk_memory_domain_data_cpl_cb)(void *ctx, int rc);

/**
 * Definition of function which asynchronously pulles data from src_domain to local memory domain.
 * Implementation of this function must call \b cpl_cb only when it returns 0. All other return codes mean failure.
 *
 * \param src_domain Memory domain to which the data buffer belongs
 * \param src_domain_ctx Optional context passed by upper layer with IO request
 * \param src_iov Iov vector in \b src_domain space
 * \param src_iovcnt src_iov array size
 * \param dst_iov Iov vector in local memory domain space, data buffers must be allocated by the caller of
 * this function, total size of data buffers must not be less than the size of data in \b src_iov.
 * \param dst_iovcnt dst_iov array size
 * \param cpl_cb A callback to be called when pull operation completes
 * \param cpl_cb_arg Optional argument to be passed to \b cpl_cb
 * \return 0 on success, negated errno on failure
 */
/*
 * [한국어]
 * spdk_memory_domain_pull_data_cb - 외부 도메인의 데이터를 로컬 host 메모리로 끌어오는 콜백.
 *
 * @src_domain:     데이터가 위치한 origin 도메인 (예: GPU 메모리 도메인).
 * @src_domain_ctx: upper layer가 IO 요청 시 전달한 컨텍스트 (도메인별 의미).
 * @src_iov/cnt:    src_domain 주소 공간의 source iov.
 * @dst_iov/cnt:    호스트 로컬 메모리의 destination iov (호출자가 사전 할당).
 * @cpl_cb/_arg:    비동기 완료 콜백 + 인자.
 * @return:         0 = 비동기 시작 성공 (cpl_cb 호출 보장).
 *                  음수 errno = 즉시 실패 (cpl_cb 호출되지 않음).
 *
 * 핵심 계약: 0 반환 시에만 cpl_cb를 호출해야 함. 호출 측은 0 받으면 cpl_cb 대기.
 * 사용 예: 로컬에서 처리해야 하는 연산(예: 호스트 SHA 계산) 전에 GPU→host 복사.
 */
typedef int (*spdk_memory_domain_pull_data_cb)(struct spdk_memory_domain *src_domain,
		void *src_domain_ctx,
		struct iovec *src_iov, uint32_t src_iovcnt, struct iovec *dst_iov, uint32_t dst_iovcnt,
		spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

/**
 * Definition of function which asynchronously pushes data from local memory to destination memory domain.
 * Implementation of this function must call \b cpl_cb only when it returns 0. All other return codes mean failure.
 *
 * \param dst_domain Memory domain to which the data should be pushed
 * \param dst_domain_ctx Optional context passed by upper layer with IO request
 * \param dst_iov Iov vector in dst_domain space
 * \param dst_iovcnt dst_iov array size
 * \param src_iov Iov vector in local memory
 * \param src_iovcnt src_iov array size
 * \param cpl_cb A callback to be called when push operation completes
 * \param cpl_cb_arg Optional argument to be passed to \b cpl_cb
 * \return 0 on success, negated errno on failure
 */
/*
 * [한국어]
 * spdk_memory_domain_push_data_cb - 로컬 host 메모리의 데이터를 외부 도메인으로 보내는 콜백.
 *
 * @dst_domain:     데이터가 향할 destination 도메인.
 * @dst_iov/cnt:    dst_domain 주소 공간의 destination iov.
 * @src_iov/cnt:    호스트 로컬 메모리의 source iov.
 * @cpl_cb/_arg:    비동기 완료 콜백 + 인자.
 * @return:         0 = 비동기 시작 성공 / 음수 errno = 즉시 실패.
 *
 * pull의 대칭. 호스트에서 만든 데이터를 GPU/원격 노드로 복사할 때 사용.
 * 핵심 계약: 0 반환 시에만 cpl_cb 호출.
 */
typedef int (*spdk_memory_domain_push_data_cb)(struct spdk_memory_domain *dst_domain,
		void *dst_domain_ctx,
		struct iovec *dst_iov, uint32_t dst_iovcnt, struct iovec *src_iov, uint32_t src_iovcnt,
		spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

/**
 * Definition of function which asynchronously fills memory in \b domain with zeroes
 *
 * \param domain Memory domain in which address space data buffer is located
 * \param domain_ctx User defined context
 * \param iov iov in \b domain memory space to be filled with zeroes
 * \param iovcnt \b iov array size
 * \param cpl_cb Completion callback
 * \param cpl_cb_arg Completion callback argument
 * \return 0 on success, negated errno on failure
 */
/*
 * [한국어]
 * spdk_memory_domain_memzero_cb - 도메인 내 영역을 0으로 채우는 콜백.
 *
 * @domain:        대상 메모리가 위치한 도메인.
 * @iov/cnt:       0으로 채울 영역 (도메인 주소 공간).
 * @cpl_cb/_arg:   비동기 완료 콜백 + 인자.
 * @return:        0 비동기 성공 / 음수 errno 실패.
 *
 * 사용: NVMe write zeros, 사전 zero-fill 등 — host 왕복 없이 도메인 내부 가속 가능.
 * 예: GPU 도메인이면 cudaMemset 같은 디바이스 명령으로 처리.
 */
typedef int (*spdk_memory_domain_memzero_cb)(struct spdk_memory_domain *domain, void *domain_ctx,
		struct iovec *iov, uint32_t iovcnt, spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

/* [한국어] translate 콜백의 출력 — 변환된 주소 표현 (실제 데이터는 이동하지 않음).
 * iov_count==1이면 iov 단일 사용, 그 이상이면 iovs 배열 사용 (구현이 메모리 관리). */
struct spdk_memory_domain_translation_result {
	/** size of this structure in bytes */
	size_t size;
	/* [한국어] ABI 호환용 — 호출자가 인지한 sizeof(struct ...) 채움.
	 * 구현이 신규 필드 유효 여부 판단. */

	/** Number of elements in iov */
	uint32_t iov_count;
	/* [한국어] 결과 iovec 개수.
	 * 1이면 단일 매핑(아래 iov 사용), >1이면 fragmented 매핑 (iovs 배열 사용). */

	/** Translation results, holds single address, length pair. Should only be used if \b iov_count is 1 */
	struct iovec iov;
	/* [한국어] iov_count==1 전용 단일 매핑.
	 * iov.iov_base는 dst_domain 주소 공간의 시작, iov.iov_len은 길이.
	 * 인라인 저장으로 흔한 케이스의 할당 비용 회피. */

	/** Translation results, array of addresses and lengths. Should only be used if \b iov_count is
	 * bigger than 1. The implementer of the translation callback is responsible for allocating and
	 * storing of this array until IO request completes */
	struct iovec *iovs;
	/* [한국어] iov_count>1일 때의 fragmented 결과.
	 * 메모리 소유권: translate 콜백 구현체가 IO 완료까지 살아있도록 관리.
	 * 호출자는 read-only로 소비. */

	/** Destination domain passed to translation function */
	struct spdk_memory_domain *dst_domain;
	/* [한국어] translate 호출 시 지정된 dst_domain 그대로 보존.
	 * 호출자가 결과 후속 처리 시 컨텍스트 회복용. */

	union {
		struct {
			uint32_t lkey;
			/* [한국어] RDMA Local Key — 본 노드 NIC이 이 메모리에 DMA할 때 사용.
			 * dst_domain.type == RDMA일 때만 유효. */

			uint32_t rkey;
			/* [한국어] RDMA Remote Key — 원격 노드가 RDMA READ/WRITE로 본 메모리에
			 * 접근할 때 사용. NVMe-oF RDMA target에서 호스트 노드가 캡슐과 함께 전달. */
		} rdma;
		/* [한국어] dst_domain.type == RDMA일 때 사용되는 키 묶음. */
	};
	/* [한국어] dst_domain.type별 ancillary 결과의 union — 미래에 다른 type 확장 가능. */
};

/* [한국어] translate 호출 시 dst_domain에 추가로 전달하는 ancillary 컨텍스트.
 * dst_domain.type별로 union 멤버 선택. */
struct spdk_memory_domain_translation_ctx {
	/** size of this structure in bytes */
	size_t size;
	/* [한국어] ABI 호환용 sizeof. */

	union {
		struct {
			/* Opaque handle for ibv_qp */
			void *ibv_qp;
			/* [한국어] RDMA Queue Pair 핸들 (struct ibv_qp*).
			 * verbs ibv_qp는 PD/lkey/rkey 결정에 영향 — translate 시 어느 QP가
			 * 사용될지 알아야 적절한 키를 발급할 수 있음. */
		} rdma;
		/* [한국어] dst_domain.type == RDMA일 때만 의미 있음. */
	};
};

/**
 * Definition of function which starts asynchronous operation to transfer data from the source to
 * destination memory domains.
 * Implementation of this function must call \b cpl_cb only when it returns 0. All other return codes mean failure.
 *
 * \param dst_domain Memory domain to which the data should be transferred
 * \param dst_domain_ctx Optional context passed by upper layer with IO request
 * \param dst_iov Iov vector in dst_domain space
 * \param dst_iovcnt dst_iov array size
 * \param src_domain Memory domain from which the data should be transferred
 * \param src_domain_ctx Optional context passed by the caller
 * \param src_iov Iov vector in local memory
 * \param src_iovcnt src_iov array size
 * \param src_translation Optional memory translation from the source to the destination memory domain
 * \param cpl_cb A callback to be called when push operation completes
 * \param cpl_cb_arg Optional argument to be passed to \b cpl_cb
 * \return 0 on success, negated errno on failure
 */
/*
 * [한국어]
 * spdk_memory_domain_transfer_data_cb - src_domain → dst_domain 직접 전송 콜백.
 *
 * @dst_domain/_ctx, @dst_iov/cnt: 목적지 도메인과 iov.
 * @src_domain/_ctx, @src_iov/cnt: 소스 도메인과 iov.
 * @src_translation: 사전 계산된 src 측 translation 결과 (재사용 가능, 옵션).
 * @cpl_cb/_arg: 비동기 완료 콜백.
 * @return: 0 비동기 성공 / 음수 errno 실패.
 *
 * pull+push의 짧은 경로 — 호스트 메모리 왕복 없이 도메인 간 직접 데이터 전송.
 * 예: GPU↔NVMe(zero-copy), RDMA NIC↔NVMe (peer-to-peer DMA).
 * src_translation은 호출자가 미리 translate해두었다면 재계산 비용 절감.
 */
typedef int (*spdk_memory_domain_transfer_data_cb)(struct spdk_memory_domain *dst_domain,
		void *dst_domain_ctx,
		struct iovec *dst_iov, uint32_t dst_iovcnt,
		struct spdk_memory_domain *src_domain, void *src_domain_ctx,
		struct iovec *src_iov, uint32_t src_iovcnt,
		struct spdk_memory_domain_translation_result *src_translation,
		spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

/**
 * Definition of function which translates data from src_domain to a form accessible by dst_domain.
 *
 * \param src_domain Memory domain to which the data buffer belongs
 * \param src_domain_ctx Optional context passed by upper layer with IO request
 * \param dst_domain Memory domain which determines type of translation function
 * \param dst_domain_ctx Ancillary data for dst_domain
 * \param addr Data buffer address in \b src_domain memory space which should be translated into \b dst_domain
 * \param len Length of data buffer
 * \param result Result of translation function
 * \return 0 on success, negated errno on failure
 */
/*
 * [한국어]
 * spdk_memory_domain_translate_memory_cb - 데이터 이동 없이 주소 표현만 변환.
 *
 * @src_domain/_ctx: source 도메인.
 * @dst_domain/_ctx: 목표 표현 도메인 (변환 결과 형식 결정).
 * @addr/@len: src_domain 공간의 영역.
 * @result: 출력 — translation_result로 채워짐 (lkey/rkey 또는 평문 IOVA 등).
 * @return: 0 성공 / 음수 errno 실패 (등록 안 됨, 변환 불가능 등).
 *
 * 동기 함수 — 결과 즉시 사용 가능. 데이터는 그대로, 주소 표현만 바뀜.
 * 핵심: 같은 물리 메모리를 두 도메인이 다르게 보는 경우의 변환 (zero-copy I/O 핵심).
 */
typedef int (*spdk_memory_domain_translate_memory_cb)(struct spdk_memory_domain *src_domain,
		void *src_domain_ctx, struct spdk_memory_domain *dst_domain,
		struct spdk_memory_domain_translation_ctx *dst_domain_ctx, void *addr, size_t len,
		struct spdk_memory_domain_translation_result *result);

/**
 * Definition of function which invalidates the data range in the given domain
 *
 * \param domain Memory domain to which the data buffer belongs
 * \param domain_ctx Optional context passed by upper layer
 * \param iov Iov array in \b domain memory space to be invalidated
 * \param iovcnt Iov array size
 */
/*
 * [한국어]
 * spdk_memory_domain_invalidate_data_cb - translate 결과에 대한 invalidate 콜백.
 *
 * @domain:    무효화할 도메인.
 * @iov/cnt:   무효화할 영역.
 *
 * 일부 도메인은 translate 결과를 캐시 (예: RDMA MR 등록).
 * IO 완료 후 동일 매핑이 더 이상 유효하지 않게 만드는 옵션 콜백.
 * 등록되지 않으면 호출자가 별도 처리하지 않아도 됨.
 */
typedef void (*spdk_memory_domain_invalidate_data_cb)(struct spdk_memory_domain *domain,
		void *domain_ctx, struct iovec *iov, uint32_t iovcnt);

/** Context of memory domain of RDMA type */
/* [한국어] RDMA 도메인 user_ctx의 표준 캐스팅 타입. create 시 ctx->user_ctx로 전달. */
struct spdk_memory_domain_rdma_ctx {
	/** size of this structure in bytes */
	size_t size;
	/* [한국어] ABI 호환용 sizeof. */

	/** Opaque handle for ibv_pd */
	void *ibv_pd;
	/* [한국어] RDMA Protection Domain 핸들 (struct ibv_pd*).
	 * 이 PD에 등록된 메모리만 lkey/rkey로 접근 가능 → 도메인의 정체성 결정.
	 * NVMe-oF RDMA에서 transport별 PD가 도메인 1개에 대응. */
};

/* [한국어] memory_domain create 시 함께 전달되는 컨텍스트 묶음.
 * 도메인 type별 추가 정보를 user_ctx로 전달 (예: RDMA면 위 rdma_ctx 캐스팅). */
struct spdk_memory_domain_ctx {
	/** size of this structure in bytes */
	size_t size;
	/* [한국어] ABI 호환용 sizeof. 새 필드 추가 시 끝에 append. */

	/** Optional user context
	 * Depending on memory domain type, this pointer can be cast to a specific structure,
	 * e.g. to spdk_memory_domain_rdma_ctx structure for RDMA memory domain */
	void *user_ctx;
	/* [한국어] type별 ancillary 데이터. RDMA면 rdma_ctx, ACCEL이면 다른 형 등.
	 * 수명: 도메인이 살아있는 동안 호출자가 보장 (구현이 복사하지 않음). */

	/** size of \b user_ctx in bytes */
	size_t user_ctx_size;
	/* [한국어] user_ctx가 가리키는 구조의 바이트 크기.
	 * spdk_memory_domain_get_user_context()의 두 번째 출력 인자로 반환. */
};

/**
 * Creates a new memory domain of the specified type.
 *
 * Translation functions can be provided to translate addresses from one memory domain to another.
 * If the two domains both use the same addressing scheme for, then this translation does nothing.
 * However, it is possible that the two memory domains may address the same physical memory
 * differently, so this translation step is required.
 *
 * \param domain Double pointer to memory domain to be allocated by this function
 * \param type Type of the DMA device which can access this memory domain
 * \param ctx Optional memory domain context to be copied by this function. Later \b ctx can be
 * retrieved using \ref spdk_memory_domain_get_context function
 * \param id String identifier representing the DMA device that can access this memory domain.
 * \return 0 on success, negated errno on failure
 */
/*
 * [한국어]
 * spdk_memory_domain_create - 새 memory domain 객체 생성.
 *
 * @domain: 출력 — 새로 할당된 도메인 포인터를 받는다 (NULL 불가).
 * @type:   enum spdk_dma_device_type 값.
 * @ctx:    옵션 컨텍스트 (구조체 자체는 복사됨, user_ctx는 포인터만 보존).
 * @id:     이 DMA 디바이스를 식별하는 문자열 (예: SPDK_RDMA_DMA_DEVICE).
 *          get_first/get_next에서 매칭 키로 사용.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * 생성 후 set_translation/_pull/_push/_transfer/_memzero/_invalidate 콜백을
 * 등록해야 도메인이 실제로 동작. 호출자는 도메인 lifecycle 동안 destroy 책임.
 * 내부에서 전역 TAILQ에 추가되어 get_first/_next로 순회 가능.
 */
int spdk_memory_domain_create(struct spdk_memory_domain **domain, enum spdk_dma_device_type type,
			      struct spdk_memory_domain_ctx *ctx, const char *id);

/**
 * Set translation function for memory domain. Overwrites existing translation function.
 *
 * \param domain Memory domain
 * \param translate_cb Translation function
 */
/*
 * [한국어]
 * spdk_memory_domain_set_translation - translate 콜백 등록 (덮어쓰기).
 *
 * @domain: 대상 도메인.
 * @translate_cb: 새 콜백 (NULL이면 translate 비활성).
 *
 * spdk_memory_domain_translate_data()가 결국 이 콜백으로 디스패치.
 * RDMA 도메인이면 lkey/rkey 결정 함수, GPU 도메인이면 GPU↔CPU 주소 변환 함수 등.
 */
void spdk_memory_domain_set_translation(struct spdk_memory_domain *domain,
					spdk_memory_domain_translate_memory_cb translate_cb);

/**
 * Set invalidate function for memory domain. Overwrites existing invalidate function.
 *
 * @param domain Memory domain
 * @param invalidate_cb Invalidate function
 */
/*
 * [한국어]
 * spdk_memory_domain_set_invalidate - invalidate 콜백 등록 (덮어쓰기, 옵션).
 *
 * @domain: 대상 도메인.
 * @invalidate_cb: 새 콜백 (NULL이면 invalidate 미지원).
 *
 * 캐시된 translation 결과를 무효화할 필요가 있는 도메인에서만 등록.
 */
void spdk_memory_domain_set_invalidate(struct spdk_memory_domain *domain,
				       spdk_memory_domain_invalidate_data_cb invalidate_cb);

/**
 * Set pull function for memory domain. Overwrites existing pull function.
 *
 * \param domain Memory domain
 * \param pull_cb pull function
 */
/*
 * [한국어]
 * spdk_memory_domain_set_pull - pull 콜백 등록 (덮어쓰기).
 *
 * @domain: 대상 도메인.
 * @pull_cb: 새 콜백 (NULL이면 pull 미지원, 호출자는 transfer 등 다른 경로 사용).
 *
 * 외부 도메인 → 호스트 메모리 비동기 복사. 호스트에서 데이터 변환이
 * 필요한 경로의 prerequisite.
 */
void spdk_memory_domain_set_pull(struct spdk_memory_domain *domain,
				 spdk_memory_domain_pull_data_cb pull_cb);

/**
 * Set push function for memory domain. Overwrites existing push function.
 *
 * \param domain Memory domain
 * \param push_cb push function
 */
/*
 * [한국어]
 * spdk_memory_domain_set_push - push 콜백 등록 (덮어쓰기).
 *
 * 호스트 메모리 → 외부 도메인 비동기 복사. pull의 대칭.
 */
void spdk_memory_domain_set_push(struct spdk_memory_domain *domain,
				 spdk_memory_domain_push_data_cb push_cb);

/**
 * Set data transfer for memory domain. Overwrites existing function.
 *
 * \param domain Memory domain
 * \param transfer_cb Data transfer function
 */
/*
 * [한국어]
 * spdk_memory_domain_set_data_transfer - transfer 콜백 등록 (덮어쓰기).
 *
 * @transfer_cb: src↔dst 도메인 간 직접 전송 함수 (zero-copy 가능).
 *
 * 두 도메인이 P2P DMA 등으로 호스트 왕복 없이 직접 통신 가능할 때 등록.
 * NULL이면 호출자가 pull+push로 대체.
 */
void spdk_memory_domain_set_data_transfer(struct spdk_memory_domain *domain,
		spdk_memory_domain_transfer_data_cb transfer_cb);

/**
 * Set memzero function for memory domain. Overwrites existing memzero function.
 *
 * \param domain Memory domain
 * \param memzero_cb memzero function
 */
/*
 * [한국어]
 * spdk_memory_domain_set_memzero - memzero 콜백 등록 (덮어쓰기).
 *
 * 도메인 내부 가속 zero-fill 기능. 미등록 시 호출자가 직접 zero 버퍼 push 필요.
 */
void spdk_memory_domain_set_memzero(struct spdk_memory_domain *domain,
				    spdk_memory_domain_memzero_cb memzero_cb);

/**
 * Get the context passed by the user in \ref spdk_memory_domain_create
 *
 * \param domain Memory domain
 * \return Memory domain context
 */
/*
 * [한국어]
 * spdk_memory_domain_get_context - create 시 전달된 ctx 구조체 포인터 반환.
 *
 * @domain: 도메인.
 * @return: spdk_memory_domain_ctx* (도메인 내부 복사본).
 *
 * size 필드로 ABI 호환 처리, user_ctx로 type-specific 데이터 접근.
 */
struct spdk_memory_domain_ctx *spdk_memory_domain_get_context(struct spdk_memory_domain *domain);

/**
 * Get an opaque pointer to the user context and its size.
 *
 * \param domain Memory domain
 * \param ctx_size Stores size of the user context. NULL pointer is not allowed
 * \return User context pointer
 */
/*
 * [한국어]
 * spdk_memory_domain_get_user_context - user_ctx 포인터와 그 크기를 반환.
 *
 * @domain:    도메인.
 * @ctx_size:  출력 — user_ctx 바이트 크기 (NULL 불가).
 * @return:    user_ctx (NULL 가능, 등록 안 한 경우).
 *
 * 호출자는 type별 캐스팅 (예: RDMA면 spdk_memory_domain_rdma_ctx*).
 */
void *spdk_memory_domain_get_user_context(struct spdk_memory_domain *domain, size_t *ctx_size);

/**
 * Get type of the DMA device that can access this memory domain
 *
 * \param domain Memory domain
 * \return DMA device type
 */
/*
 * [한국어]
 * spdk_memory_domain_get_dma_device_type - 도메인의 DMA type 조회.
 *
 * @return: enum spdk_dma_device_type 값.
 *
 * 호출자가 도메인 호환성 판단 (예: RDMA endpoint와 호환되는 도메인인가?).
 */
enum spdk_dma_device_type spdk_memory_domain_get_dma_device_type(struct spdk_memory_domain *domain);

/**
 * Get an identifier representing the DMA device that can access this memory domain
 * \param domain Memory domain
 * \return DMA device identifier
 */
/*
 * [한국어]
 * spdk_memory_domain_get_dma_device_id - create 시 등록한 id 문자열 반환.
 *
 * @return: const char* (도메인 내부 보유).
 *
 * RPC 응답이나 디버깅 출력용.
 */
const char *spdk_memory_domain_get_dma_device_id(struct spdk_memory_domain *domain);

/**
 * Destroy memory domain
 *
 * \param domain Memory domain
 */
/*
 * [한국어]
 * spdk_memory_domain_destroy - 도메인 객체 해제 및 전역 리스트에서 제거.
 *
 * @domain: NULL 허용.
 *
 * 호출 후 도메인 포인터 사용 금지. 진행 중 IO가 없음을 호출자가 보장해야 함.
 */
void spdk_memory_domain_destroy(struct spdk_memory_domain *domain);

/**
 * Asynchronously pull data which is described by \b src_domain and located in \b src_iov to a location
 * \b dst_iov local memory space.
 *
 * \param src_domain Memory domain in which space data buffer is located
 * \param src_domain_ctx User defined context
 * \param src_iov Source data iov
 * \param src_iov_cnt The number of elements in \b src_iov
 * \param dst_iov Destination iov
 * \param dst_iov_cnt The number of elements in \b dst_iov
 * \param cpl_cb Completion callback
 * \param cpl_cb_arg Completion callback argument
 * \return 0 on success, negated errno on failure. pull_cb implementation must only call the callback when 0
 * is returned
 */
/*
 * [한국어]
 * spdk_memory_domain_pull_data - 외부 도메인에서 호스트 로컬 메모리로 데이터 끌어오기.
 *
 * @src_domain/_ctx, @src_iov/_iov_cnt: 소스 (도메인 공간).
 * @dst_iov/_iov_cnt: 호스트 로컬 destination (호출자 사전 할당).
 * @cpl_cb/_arg: 비동기 완료 콜백.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * src_domain에 등록된 pull_cb로 디스패치. 미등록이면 -ENOTSUP.
 * 핵심 계약: 0 반환 시에만 cpl_cb 호출.
 */
int spdk_memory_domain_pull_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				 struct iovec *src_iov, uint32_t src_iov_cnt, struct iovec *dst_iov, uint32_t dst_iov_cnt,
				 spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

/**
 * Asynchronously push data located in local memory to \b dst_domain
 *
 * \param dst_domain Memory domain to which the data should be pushed
 * \param dst_domain_ctx Optional context passed by upper layer with IO request
 * \param dst_iov Iov vector in dst_domain space
 * \param dst_iovcnt dst_iov array size
 * \param src_iov Iov vector in local memory
 * \param src_iovcnt src_iov array size
 * \param cpl_cb Completion callback
 * \param cpl_cb_arg Completion callback argument
 * \return 0 on success, negated errno on failure. push_cb implementation must only call the callback when 0
 * is returned
 */
/*
 * [한국어]
 * spdk_memory_domain_push_data - 호스트 로컬 메모리에서 외부 도메인으로 데이터 보내기.
 *
 * pull의 대칭. dst_domain의 push_cb로 디스패치.
 */
int spdk_memory_domain_push_data(struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				 struct iovec *dst_iov, uint32_t dst_iovcnt, struct iovec *src_iov, uint32_t src_iovcnt,
				 spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

/**
 * Asynchronously transfer data from the source memory domain to the destination memory domain
 *
 * \param dst_domain Memory domain to which the data should be transferred
 * \param dst_domain_ctx Optional context passed by upper layer with IO request
 * \param dst_iov Iov vector in dst_domain space
 * \param dst_iovcnt dst_iov array size
 * \param src_domain Memory domain from which the data should be transferred
 * \param src_domain_ctx Optional context passed by the caller
 * \param src_iov Iov vector in local memory
 * \param src_iovcnt src_iov array size
 * \param src_translation Optional memory translation from the source to the destination memory domain
 * \param cpl_cb A callback to be called when push operation completes
 * \param cpl_cb_arg Optional argument to be passed to \b cpl_cb
 * \return 0 on success, negated errno on failure. push_cb implementation must only call the callback when 0
 * is returned
 */
/*
 * [한국어]
 * spdk_memory_domain_transfer_data - src_domain → dst_domain 직접 비동기 전송.
 *
 * @src_translation: 옵션 — 미리 계산된 translate 결과 (재사용으로 비용 절감).
 *
 * dst_domain의 transfer_cb로 디스패치. 호스트 메모리 왕복 없이 도메인 간 데이터
 * 이동 (P2P DMA 등). 미등록이면 -ENOTSUP → 호출자가 pull+push fallback.
 */
int spdk_memory_domain_transfer_data(struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				     struct iovec *dst_iov, uint32_t dst_iovcnt,
				     struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				     struct iovec *src_iov, uint32_t src_iovcnt,
				     struct spdk_memory_domain_translation_result *src_translation,
				     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

/**
 * Translate data located in \b src_domain space at address \b addr with size \b len into an equivalent
 * description of memory in dst_domain.
 *
 * This function calls \b src_domain translation callback, the callback needs to be set using \ref
 * spdk_memory_domain_set_translation function.
 * No data is moved during this operation. Both src_domain and dst_domain must describe the same physical memory,
 * just from the point of view of two different memory domain. This is a translation of the description of the memory only.
 * Result of translation is stored in \b result, its content depends on the type of \b dst_domain.
 *
 * \param src_domain Memory domain in which address space data buffer is located
 * \param src_domain_ctx User defined context
 * \param dst_domain Memory domain in which memory space data buffer should be translated
 * \param dst_domain_ctx Ancillary data for dst_domain
 * \param addr Address in \b src_domain memory space
 * \param len Length of the data
 * \param result Translation result. The content of the translation result is only valid if this
 * function returns 0.
 * \return 0 on success, negated errno on failure.
 */
/*
 * [한국어]
 * spdk_memory_domain_translate_data - 데이터 이동 없이 주소 표현 동기 변환.
 *
 * @src_domain/_ctx: 소스 도메인.
 * @dst_domain/_ctx: 표현 변환 대상 도메인 + ancillary (예: ibv_qp).
 * @addr/@len:       src 공간의 영역.
 * @result:          translation_result로 채워짐. 0 반환 시에만 유효.
 * @return:          0 성공 / 음수 errno 실패.
 *
 * 핵심 가정: src와 dst가 동일 물리 메모리를 다르게 보는 경우에만 의미.
 * 동기 함수 — 결과 즉시 사용 가능.
 * 사용 예: bdev/transport가 사용자 버퍼의 lkey/rkey를 RDMA WR에 채우기 전 호출.
 */
int spdk_memory_domain_translate_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				      struct spdk_memory_domain *dst_domain, struct spdk_memory_domain_translation_ctx *dst_domain_ctx,
				      void *addr, size_t len, struct spdk_memory_domain_translation_result *result);

/**
 * Invalidate memory in the given domain.
 *
 * This function calls \b domain invalidate callback, the callback needs to be set using \ref
 * spdk_memory_domain_set_invalidate function.
 * This operation is optional and is meant to be executed on the translation result \ref spdk_memory_domain_translate_data.
 *
 * \param domain Memory domain in which address space of the buffer is located
 * \param domain_ctx User defined context
 * \param iov Iov vector in \b domain memory space to be invalidated
 * \param iovcnt iov array size
 */
/*
 * [한국어]
 * spdk_memory_domain_invalidate_data - 도메인 영역 무효화 (옵션 연산).
 *
 * @domain/_ctx: 대상 도메인.
 * @iov/cnt: 무효화 영역.
 *
 * 보통 직전 translate 결과에 대한 후속 정리. invalidate_cb가 등록된 도메인만 의미.
 * 미등록이면 nop.
 */
void spdk_memory_domain_invalidate_data(struct spdk_memory_domain *domain, void *domain_ctx,
					struct iovec *iov, uint32_t iovcnt);

/**
 * Fills memory in \b domain with zeroes
 *
 * \param domain Memory domain in which address space data buffer is located
 * \param domain_ctx User defined context
 * \param iov iov in \b domain memory space to be filled with zeroes
 * \param iovcnt \b iov array size
 * \param cpl_cb Completion callback
 * \param cpl_cb_arg Completion callback argument
 * \return 0 on success, negated errno on failure. memzero implementation must only call the callback when 0
 * is returned
 */
/*
 * [한국어]
 * spdk_memory_domain_memzero - 도메인 내 영역을 0으로 채우는 비동기 연산.
 *
 * @domain/_ctx: 대상 도메인.
 * @iov/cnt: 채울 영역.
 * @cpl_cb/_arg: 비동기 완료 콜백.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * memzero_cb가 등록된 도메인의 가속 zero-fill로 디스패치.
 * 핵심 계약: 0 반환 시에만 cpl_cb 호출.
 */
int spdk_memory_domain_memzero(struct spdk_memory_domain *domain, void *domain_ctx,
			       struct iovec *iov, uint32_t iovcnt, spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

/**
 * Get the first memory domain.
 *
 * Combined with \ref spdk_memory_domain_get_next to iterate over all memory domains
 *
 * \param id Optional identifier representing the DMA device that can access a memory domain, if set
 * then this function returns the first memory domain which id matches or NULL
 * \return Pointer to the first memory domain or NULL
 */
/*
 * [한국어]
 * spdk_memory_domain_get_first - 등록된 memory domain 리스트 순회 시작점.
 *
 * @id: NULL이면 모든 도메인 순회 시작, 비-NULL이면 id 일치 도메인만 필터.
 * @return: 첫 도메인 / 없으면 NULL.
 *
 * 사용:
 *   for (d = spdk_memory_domain_get_first(NULL); d;
 *        d = spdk_memory_domain_get_next(d, NULL)) { ... }
 */
struct spdk_memory_domain *spdk_memory_domain_get_first(const char *id);

/**
 * Get the next memory domain.
 *
 * \param prev Previous memory domain
 * \param id Optional identifier representing the DMA device that can access a memory domain, if set
 * then this function returns the next memory domain which id matches or NULL
 * \return Pointer to next memory domain or NULL;
 */
/*
 * [한국어]
 * spdk_memory_domain_get_next - 다음 도메인 반환 (id 필터 동일).
 *
 * @prev: 현재 도메인 (NULL 불가).
 * @id:   NULL이면 무필터, 비-NULL이면 id 일치만.
 * @return: 다음 도메인 / 마지막이면 NULL.
 */
struct spdk_memory_domain *spdk_memory_domain_get_next(struct spdk_memory_domain *prev,
		const char *id);

/**
 * Get the System memory domain.
 *
 * \return Pointer to the System memory domain.
 */
/*
 * [한국어]
 * spdk_memory_domain_get_system_domain - 시스템(host process) 기본 도메인 반환.
 *
 * @return: lib/dma/dma.c가 init 시 자동 생성한 시스템 도메인 (항상 존재).
 *
 * "특별한 도메인 없음" = 일반 호스트 메모리를 표현. bdev API의 memory_domain
 * 필드가 NULL일 때 의미적으로 이 도메인과 동치.
 */
struct spdk_memory_domain *spdk_memory_domain_get_system_domain(void);

#ifdef __cplusplus
}
#endif

#endif /* [한국어] SPDK_DMA_H include 가드 닫기 */
