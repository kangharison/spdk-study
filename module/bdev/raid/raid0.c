/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK RAID 0 (스트라이핑) 모듈 구현 (raid0.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 SPDK bdev RAID 프레임워크의 RAID 0 (striping) 레벨을 구현한다. RAID 0는
 * N개의 base bdev들을 strip 단위(strip_size 블록)로 라운드 로빈 분배하여 단일 가상 bdev로
 * 노출한다. 즉 가상 LBA 공간의 strip 0은 disk 0의 strip 0에, strip 1은 disk 1의 strip 0에,
 * ..., strip k는 disk (k mod N)의 strip (k / N)에 매핑된다. concat과 달리 강한 분산 → 큰 I/O가
 * 자동으로 N개의 디스크에 동시에 흩뿌려져 처리량(throughput)이 N배가 된다 (단, 단일 디스크
 * 실패 시 데이터 보호 없음).
 *
 * RAID 코어가 split_on_optimal_io_boundary=true 와 optimal_io_boundary=strip_size 로 설정해
 * 큰 R/W를 strip 경계에서 자동 분할해 본 모듈로 전달하므로, raid0_submit_rw_request 는
 * "단일 strip 안의 단일 base bdev 라우팅"만 처리한다. FLUSH/UNMAP은 분할되지 않은 큰 범위로
 * 들어올 수 있어 raid0_submit_null_payload_request에서 strip 단위로 N-way fan-out 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   사용자 spdk_bdev_read/write
 *     → lib/bdev (RAID bdev 가상 디바이스)
 *       → bdev_raid 코어 (raid_bdev_submit_request)
 *         → 본 파일 raid0_submit_rw_request / raid0_submit_null_payload_request
 *           → 매핑된 base bdev (raid_bdev_readv/writev/flush/unmap_blocks)
 *             → bdev_nvme/aio/malloc 등 백엔드
 *               → 완료 시 raid0_bdev_io_completion / raid0_base_io_complete
 *                 → raid_bdev_io_complete[_part]
 *
 * 실행 컨텍스트는 SPDK reactor 스레드. 모든 비동기 I/O 콜백은 발행 스레드에서 호출되어
 * lockless 갱신이 가능.
 *
 * === 타 모듈과의 연결 ===
 * - bdev_raid.h: RAID 코어 자료구조 (raid_bdev, raid_bdev_io, raid_base_bdev_info), 발행
 *   헬퍼 (raid_bdev_readv_blocks_ext, writev_blocks_ext, unmap_blocks, flush_blocks),
 *   완료 헬퍼 (raid_bdev_io_complete[_part]), DIF/DIX 검증 (raid_bdev_verify_dix_reftag).
 * - spdk/bdev_module.h: spdk_bdev_get_dif_type, spdk_bdev_notify_blockcnt_change 등.
 * - spdk/util.h: spdk_min, spdk_unlikely 등.
 *
 * 데이터 흐름: 가상 LBA → (strip 인덱스 = LBA >> strip_size_shift) → (disk = strip mod N,
 * disk-내 strip = strip / N) → (disk LBA = disk-내 strip * strip_size + offset_in_strip) →
 * 단일 base bdev에 발행. RAID 0는 redundancy 없음 → 한 base 실패 = 부모 raid_bdev_io 실패.
 *
 * === 주요 함수/구조체 요약 ===
 * - raid0_submit_rw_request : R/W를 단일 base bdev로 라우팅 (DIF/DIX REFTAG 검증 포함).
 * - raid0_submit_null_payload_request : FLUSH/UNMAP을 모든 관련 base bdev에 분할 발행.
 * - struct raid_bdev_io_range : 발행 범위가 차지하는 strip/disk 정보 (시작/끝 strip,
 *   디스크 인덱스, n_disks_involved 등).
 * - _raid0_get_io_range / _raid0_split_io_range : 위 구조체 채우기와 디스크별 슬라이스 산출.
 * - raid0_start : 모든 base bdev에 공통 data_size 결정 (min × strip 정렬), blockcnt 설정.
 * - raid0_resize: base bdev이 커졌을 때 RAID 가상 크기 갱신.
 * - g_raid0_module / RAID_MODULE_REGISTER : RAID 코어에 등록.
 */

/* [한국어] bdev_raid.h: RAID 코어 인터페이스. raid_bdev, raid_bdev_io, raid_base_bdev_info
 * 자료구조와 발행/완료 헬퍼 함수, RAID_MODULE_REGISTER 매크로. */
#include "bdev_raid.h"

/* [한국어] spdk/env.h: DPDK 기반 hugepage/DMA/PCI 추상화. 본 파일에선 직접 호출하지 않으나
 * RAID 모듈들이 일관되게 포함하는 SPDK 환경 헤더. */
#include "spdk/env.h"
/* [한국어] spdk/thread.h: SPDK thread/poller/메시지. 콜백 실행 스레드 컨텍스트 이해용. */
#include "spdk/thread.h"
/* [한국어] spdk/string.h: spdk_strerror 등 문자열 헬퍼 (본 파일은 직접 사용 안 하지만 일관성). */
#include "spdk/string.h"
/* [한국어] spdk/util.h: spdk_min, spdk_unlikely (likely/unlikely 분기 힌트 매크로). */
#include "spdk/util.h"

/* [한국어] spdk/log.h: 로깅 매크로 (SPDK_ERRLOG, SPDK_DEBUGLOG, SPDK_LOG_REGISTER_COMPONENT). */
#include "spdk/log.h"

/*
 * brief:
 * raid0_bdev_io_completion function is called by lower layers to notify raid
 * module that particular bdev_io is completed.
 * params:
 * bdev_io - pointer to bdev io submitted to lower layers, like child io
 * success - bdev_io status
 * cb_arg - function callback context (parent raid_bdev_io)
 * returns:
 * none
 */
/*
 * [한국어]
 * raid0_bdev_io_completion - R/W child bdev_io 완료 콜백.
 *
 * @bdev_io: 완료된 child bdev_io. 하위 base bdev이 보고.
 * @success: 성공 여부.
 * @cb_arg : 부모 raid_bdev_io.
 *
 * 동작: 성공이면 (READ + DIF가 활성화된 base bdev이고 REFTAG_CHECK 비트가 켜져 있을 때)
 * REFTAG 검증을 한 번 더 수행하여 RAID 가상 LBA 기준의 reftag 일관성을 보장한다. (각 base
 * bdev은 자신의 LBA에 맞춰 reftag를 검증하지만, RAID는 가상 LBA를 사용자에게 노출하므로
 * 가상 LBA 기준 reftag로 보정/검증해야 사용자 관점에서 일관됨.) 검증 실패 → FAILED, 아니면
 * SUCCESS로 부모 raid_bdev_io 종료. 실패는 즉시 FAILED.
 *
 * 실행 컨텍스트: child bdev_io를 발행한 동일 SPDK 스레드.
 *
 * 호출 체인:
 *   하위 base bdev 완료 → bdev 코어 → 본 콜백 → raid_bdev_io_complete → 사용자 cb
 */
static void
raid0_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;
	int rc;

	if (success) {
		/* [한국어] DIX/DIF 보호가 활성화된 READ 경로 + REFTAG 검증 플래그 ON 인 케이스만 검증.
		 * spdk_unlikely는 이 경로가 드물 것이라는 분기 힌트(컴파일러/CPU 분기 예측 최적화). */
		if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_READ &&
				  spdk_bdev_get_dif_type(bdev_io->bdev) != SPDK_DIF_DISABLE &&
				  bdev_io->bdev->dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK)) {

			/* [한국어] base bdev에 기록된 reftag를 RAID 가상 LBA(=bdev_io->u.bdev.offset_blocks
			 * 는 raid_bdev_io_complete 경로에서 가상 LBA로 대체됨) 기준으로 재검증. md_buf와
			 * iovs를 입력으로 PI 영역의 reftag 필드를 비교. */
			rc = raid_bdev_verify_dix_reftag(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
							 bdev_io->u.bdev.md_buf, bdev_io->u.bdev.num_blocks, bdev_io->bdev,
							 bdev_io->u.bdev.offset_blocks);
			if (rc != 0) {
				/* [한국어] reftag 불일치 → 데이터 무결성 위반. FAILED 보고. */
				SPDK_ERRLOG("Reftag verify failed.\n");
				raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}

		/* [한국어] 정상 완료 (또는 DIX 검증도 성공) → 사용자에게 SUCCESS 통지. */
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		/* [한국어] base bdev 실패 = RAID 0 실패 (redundancy 없음). */
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}

	/* [한국어] child bdev_io 반납. raid_bdev_io_complete 호출 후에 free 해도 안전 (raid_io는
	 * 별개의 객체). */
	spdk_bdev_free_io(bdev_io);
}

/* [한국어] 전방 선언: ENOMEM 재시도 콜백 _raid0_submit_rw_request 에서 호출. */
static void raid0_submit_rw_request(struct raid_bdev_io *raid_io);

/*
 * [한국어]
 * _raid0_submit_rw_request - bdev_io_wait 콜백 시그니처 어댑터.
 *
 * @_raid_io: void* 형태로 보관된 raid_bdev_io 포인터.
 *
 * R/W는 단일 base bdev에만 발행되므로 ENOMEM 재시도는 idempotent (동일 함수 재호출만으로 OK).
 */
static void
_raid0_submit_rw_request(void *_raid_io)
{
	/* [한국어] void* → raid_bdev_io* 복원. */
	struct raid_bdev_io *raid_io = _raid_io;

	/* [한국어] 동일한 발행 함수 재호출. 자원 회복 후 SPDK가 호출. */
	raid0_submit_rw_request(raid_io);
}

/*
 * brief:
 * raid0_submit_rw_request function is used to submit I/O to the correct
 * member disk for raid0 bdevs.
 * params:
 * raid_io
 * returns:
 * none
 */
/*
 * [한국어]
 * raid0_submit_rw_request - R/W를 적절한 base bdev로 단일 발행.
 *
 * @raid_io: 상위 raid_bdev_io. offset_blocks(가상 LBA), num_blocks, iovs, type 등 보유.
 * @return : 없음.
 *
 * 핵심 가정: RAID 코어가 split_on_optimal_io_boundary=true (strip_size 경계로 split) 로
 * 사전 분할해주므로, 들어오는 R/W는 항상 단일 strip 안에 들어간다 (단, num_base_bdevs > 1 일 때).
 * 따라서 start_strip == end_strip 이 보장되어야 하며, 어긋나면 코어 버그로 간주하고 abort.
 *
 * LBA 변환 단계 (상수 시간):
 *  - start_strip = offset_blocks >> strip_size_shift  (가상 LBA → 가상 strip 인덱스)
 *  - pd_strip    = start_strip / num_base_bdevs       (디스크 내부 strip 인덱스)
 *  - pd_idx      = start_strip % num_base_bdevs       (디스크 인덱스 0..N-1)
 *  - offset_in_strip = offset_blocks & (strip_size-1) (strip 내부 LBA, strip_size는 2의 거듭제곱)
 *  - pd_lba      = (pd_strip << strip_size_shift) + offset_in_strip
 *
 * WRITE 경로에선 DIX REFTAG_CHECK가 켜져 있으면 발행 전에 reftag 검증을 수행. 검증 실패 시
 * 즉시 FAILED 종료 (디스크에 잘못된 데이터가 기록되지 않도록).
 *
 * ENOMEM 시 bdev_io_wait 큐 등록, 그 외 비ENOMEM 에러는 즉시 FAILED.
 */
static void
raid0_submit_rw_request(struct raid_bdev_io *raid_io)
{
	/* [한국어] 확장 I/O 옵션 (memory_domain, metadata 패스스루). */
	struct spdk_bdev_ext_io_opts	io_opts = {};
	struct raid_bdev_io_channel	*raid_ch = raid_io->raid_ch;
	struct raid_bdev		*raid_bdev = raid_io->raid_bdev;
	/* [한국어] 디스크 내부 strip 인덱스 (= 가상 strip / num_base_bdevs). */
	uint64_t			pd_strip;
	/* [한국어] strip 내부 시작 오프셋 (블록 단위). 0 ≤ offset_in_strip < strip_size. */
	uint32_t			offset_in_strip;
	/* [한국어] 선택된 base bdev에서 실제 발행할 LBA. */
	uint64_t			pd_lba;
	/* [한국어] 발행 길이 = 입력 num_blocks (단일 strip 안에 다 들어감). */
	uint64_t			pd_blocks;
	/* [한국어] 선택된 base bdev 인덱스 (0..N-1). */
	uint8_t				pd_idx;
	int				ret = 0;
	/* [한국어] 입력 범위가 차지하는 시작/끝 가상 strip 인덱스. start==end 여야 정상. */
	uint64_t			start_strip;
	uint64_t			end_strip;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	/* [한국어] 가상 LBA → 가상 strip. shift는 strip_size = 2^strip_size_shift 가정. */
	start_strip = raid_io->offset_blocks >> raid_bdev->strip_size_shift;
	/* [한국어] 마지막 LBA (offset+num-1) 가 속한 strip. -1은 num=0 케이스 고려가 아니라
	 * "마지막 LBA"를 의미 (num은 항상 >0 이라고 RAID 코어가 보장). */
	end_strip = (raid_io->offset_blocks + raid_io->num_blocks - 1) >>
		    raid_bdev->strip_size_shift;
	if (start_strip != end_strip && raid_bdev->num_base_bdevs > 1) {
		/* [한국어] split이 제대로 적용되지 않은 입력. 단일 base bdev 케이스(num_base_bdevs==1)
		 * 에서는 strip 경계를 넘는 I/O가 정상이므로 검사를 건너뜀. */
		assert(false);
		SPDK_ERRLOG("I/O spans strip boundary!\n");
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	/* [한국어] 가상 strip → (디스크 내부 strip, 디스크 인덱스). RAID 0의 라운드 로빈 분배. */
	pd_strip = start_strip / raid_bdev->num_base_bdevs;
	pd_idx = start_strip % raid_bdev->num_base_bdevs;
	/* [한국어] strip 내부 오프셋 = LBA를 strip_size로 modulo. strip_size는 2^k 이므로 AND 가능. */
	offset_in_strip = raid_io->offset_blocks & (raid_bdev->strip_size - 1);
	/* [한국어] 디스크 LBA = 디스크 내부 strip 시작 + 내부 오프셋. */
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;
	pd_blocks = raid_io->num_blocks;
	base_info = &raid_bdev->base_bdev_info[pd_idx];
	if (base_info->desc == NULL) {
		/* [한국어] desc==NULL = 슬롯이 detach 또는 미설정. RAID 0는 redundancy 없으므로
		 * 안전망으로 abort (정상 동작에선 RAID 코어가 이 조건을 차단해야 함). */
		SPDK_ERRLOG("base bdev desc null for pd_idx %u\n", pd_idx);
		assert(0);
	}

	/*
	 * Submit child io to bdev layer with using base bdev descriptors, base
	 * bdev lba, base bdev child io length in blocks, buffer, completion
	 * function and function callback context
	 */
	assert(raid_ch != NULL);
	/* [한국어] 현재 SPDK 스레드의 base bdev I/O 채널 획득. */
	base_ch = raid_bdev_channel_get_base_channel(raid_ch, pd_idx);

	/* [한국어] ABI 호환을 위한 size 명시. */
	io_opts.size = sizeof(io_opts);
	/* [한국어] memory_domain 패스스루 (RDMA zero-copy 등). */
	io_opts.memory_domain = raid_io->memory_domain;
	io_opts.memory_domain_ctx = raid_io->memory_domain_ctx;
	/* [한국어] T10-PI/DIF 메타 버퍼 패스스루. */
	io_opts.metadata = raid_io->md_buf;

	if (raid_io->type == SPDK_BDEV_IO_TYPE_READ) {
		/* [한국어] READ 경로: 단일 base bdev에서 [pd_lba, pd_lba+pd_blocks) 만큼 읽기. */
		ret = raid_bdev_readv_blocks_ext(base_info, base_ch,
						 raid_io->iovs, raid_io->iovcnt,
						 pd_lba, pd_blocks, raid0_bdev_io_completion,
						 raid_io, &io_opts);
	} else if (raid_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		/* [한국어] DIX(Data Integrity Extension) 검증을 위해 RAID bdev 자체의 dif 설정 참조.
		 * base_info->raid_bdev 는 같은 raid_bdev 포인터이며, 사용자에게 노출되는 dif 속성을 사용. */
		struct spdk_bdev *bdev = &base_info->raid_bdev->bdev;

		if (spdk_unlikely(spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE &&
				  bdev->dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK)) {
			/* [한국어] WRITE 경로에선 발행 전에 사용자 데이터의 reftag가 RAID 가상 LBA와
			 * 일치하는지 검증. 잘못된 reftag로 디스크에 영구 기록되는 사고 방지. */
			ret = raid_bdev_verify_dix_reftag(raid_io->iovs, raid_io->iovcnt, io_opts.metadata,
							  pd_blocks, bdev, raid_io->offset_blocks);
			if (ret != 0) {
				/* [한국어] 검증 실패 → 즉시 FAILED. 일부 디스크에 부분 기록되는 일은 없음 (아직 발행 전). */
				SPDK_ERRLOG("bdev io submit error due to DIX verify failure\n");
				raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}

		/* [한국어] WRITE 경로: 단일 base bdev로 그대로 발행. */
		ret = raid_bdev_writev_blocks_ext(base_info, base_ch,
						  raid_io->iovs, raid_io->iovcnt,
						  pd_lba, pd_blocks, raid0_bdev_io_completion,
						  raid_io, &io_opts);
	} else {
		/* [한국어] FLUSH/UNMAP 등은 submit_null_payload_request로 가야 함. 코어 버그 표명. */
		SPDK_ERRLOG("Recvd not supported io type %u\n", raid_io->type);
		assert(0);
	}

	if (ret == -ENOMEM) {
		/* [한국어] 자원 부족 → wait 큐에 본 함수 어댑터 등록. base bdev이 자원 회복하면 재발행. */
		raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid0_submit_rw_request);
	} else if (ret != 0) {
		/* [한국어] 비ENOMEM 음수 반환 = 비정상 (인자 오류 등). 즉시 FAILED. */
		SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
		assert(false);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * struct raid_bdev_io_range
 * FLUSH/UNMAP 처럼 strip 경계를 넘어가는 큰 범위 입력의 base bdev별 분할 정보 캐시.
 *
 * 가상 LBA 범위를 분석해 시작/끝 가상 strip → (시작/끝 디스크 인덱스, 디스크 내부 시작/끝 strip,
 * strip 내부 시작/끝 오프셋, 관여한 디스크 수) 로 변환. 이후 각 disk_idx별 슬라이스를
 * _raid0_split_io_range 가 산출하는 데 사용.
 */
/* raid0 IO range */
struct raid_bdev_io_range {
	uint64_t	strip_size;
	/* [한국어] strip 크기(블록). 분할 산식의 상수 인자.
	 * 설정자: _raid0_get_io_range. 읽는 자: _raid0_split_io_range. */

	uint64_t	start_strip_in_disk;
	/* [한국어] 입력 범위가 시작하는 디스크 내부 strip 인덱스(가상 start_strip / num_base_bdevs).
	 * 디스크가 여러 개인 경우 disk_idx 가 start_disk 보다 작으면 +1 보정. */

	uint64_t	end_strip_in_disk;
	/* [한국어] 입력 범위가 끝나는 디스크 내부 strip 인덱스(가상 end_strip / num_base_bdevs).
	 * disk_idx 가 end_disk 보다 크면 -1 보정. */

	uint64_t	start_offset_in_strip;
	/* [한국어] 첫 strip 안의 시작 LBA 오프셋 (블록). 첫 strip만 부분일 수 있고 나머지는 0. */

	uint64_t	end_offset_in_strip;
	/* [한국어] 마지막 strip 안의 끝 LBA 오프셋 (블록, inclusive). 마지막 strip만 부분일 수 있고
	 * 그 외는 strip_size-1 (꽉 참). */

	uint8_t		start_disk;
	/* [한국어] 첫 가상 strip이 위치한 디스크 인덱스 (= start_strip % num_base_bdevs). */

	uint8_t		end_disk;
	/* [한국어] 마지막 가상 strip이 위치한 디스크 인덱스 (= end_strip % num_base_bdevs). */

	uint8_t		n_disks_involved;
	/* [한국어] 입력이 관여한 고유 base bdev 개수. 1 ≤ n_disks_involved ≤ num_base_bdevs.
	 * = min(end_strip - start_strip + 1, num_base_bdevs). 단일 strip이면 1, 큰 범위면 N. */
};

/*
 * [한국어]
 * _raid0_get_io_range - 입력 (offset_blocks, num_blocks)을 분석해 io_range를 채운다.
 *
 * @io_range          : 결과 출력 구조체.
 * @num_base_bdevs    : RAID 디스크 수.
 * @strip_size        : strip 크기(블록).
 * @strip_size_shift  : log2(strip_size).
 * @offset_blocks     : 입력 가상 LBA.
 * @num_blocks        : 입력 길이(블록).
 *
 * 모든 산식은 정수 연산이며, strip_size = 2^k 가정으로 시프트 사용 (RAID 코어가 보장).
 */
static inline void
_raid0_get_io_range(struct raid_bdev_io_range *io_range,
		    uint8_t num_base_bdevs, uint64_t strip_size, uint64_t strip_size_shift,
		    uint64_t offset_blocks, uint64_t num_blocks)
{
	uint64_t	start_strip;
	uint64_t	end_strip;
	/* [한국어] 마지막 LBA = offset + num - 1, 단 num=0 입력 보호를 위해 (num_blocks > 0) 만큼 빼기.
	 * num=0 이면 total_blocks = offset (range 자체가 비어있음). 일반적으로 RAID 코어가
	 * num_blocks > 0 만 보내므로 결국 offset+num-1 와 동일. */
	uint64_t	total_blocks;

	io_range->strip_size = strip_size;
	total_blocks = offset_blocks + num_blocks - (num_blocks > 0);

	/* The start and end strip index in raid0 bdev scope */
	/* [한국어] 가상 strip 인덱스로 변환. shift는 빠른 나눗셈. */
	start_strip = offset_blocks >> strip_size_shift;
	end_strip = total_blocks >> strip_size_shift;
	/* [한국어] 디스크 내부 strip 인덱스 (어떤 디스크에 속하는지는 별도 인덱스 사용). */
	io_range->start_strip_in_disk = start_strip / num_base_bdevs;
	io_range->end_strip_in_disk = end_strip / num_base_bdevs;

	/* The first strip may have unaligned start LBA offset.
	 * The end strip may have unaligned end LBA offset.
	 * Strips between them certainly have aligned offset and length to boundaries.
	 */
	/* [한국어] 첫/마지막 strip에서 부분만 닿을 수 있는 LBA 오프셋. modulo strip_size. */
	io_range->start_offset_in_strip = offset_blocks % strip_size;
	io_range->end_offset_in_strip = total_blocks % strip_size;

	/* The base bdev indexes in which start and end strips are located */
	/* [한국어] 시작/끝 가상 strip이 어떤 디스크에 매핑되는지 (라운드 로빈 modulo). */
	io_range->start_disk = start_strip % num_base_bdevs;
	io_range->end_disk = end_strip % num_base_bdevs;

	/* Calculate how many base_bdevs are involved in io operation.
	 * Number of base bdevs involved is between 1 and num_base_bdevs.
	 * It will be 1 if the first strip and last strip are the same one.
	 */
	/* [한국어] 관여 디스크 수: 가상 strip 폭과 N 중 작은 값. 가상 strip 폭이 N 이상이면
	 * 모든 디스크가 적어도 한 번씩은 등장하므로 N으로 캡 씌움. */
	io_range->n_disks_involved = spdk_min((end_strip - start_strip + 1), num_base_bdevs);
}

/*
 * [한국어]
 * _raid0_split_io_range - io_range로부터 특정 disk_idx 의 디스크 내부 [offset, num] 슬라이스 산출.
 *
 * @io_range        : 미리 채워진 분할 정보.
 * @disk_idx        : 디스크 인덱스 (0..N-1).
 * @_offset_in_disk : 출력 - 디스크 내부 시작 LBA (블록).
 * @_nblocks_in_disk: 출력 - 디스크 내부 발행 길이 (블록).
 *
 * 알고리즘:
 *  - 기본적으로 [start_strip_in_disk .. end_strip_in_disk] 의 strip을 disk_idx 에서 사용.
 *  - 단, disk_idx < start_disk 이면 start_strip_in_disk를 +1 (이 디스크는 첫 strip 사이클에서
 *    start_disk보다 늦게 등장하므로 첫 strip을 한 사이클 뒤에서 시작). end_disk 도 대칭으로 -1.
 *  - 첫 strip만 부분이고 나머지는 꽉 차며, 마지막 strip만 부분일 수 있다.
 */
static inline void
_raid0_split_io_range(struct raid_bdev_io_range *io_range, uint8_t disk_idx,
		      uint64_t *_offset_in_disk, uint64_t *_nblocks_in_disk)
{
	uint64_t n_strips_in_disk;
	uint64_t start_offset_in_disk;
	uint64_t end_offset_in_disk;
	uint64_t offset_in_disk;
	uint64_t nblocks_in_disk;
	uint64_t start_strip_in_disk;
	uint64_t end_strip_in_disk;

	/* [한국어] 기본 시작 strip은 io_range의 값. disk_idx가 start_disk보다 작으면 +1 (첫 사이클을
	 * 못 받았으므로 다음 사이클부터 시작). */
	start_strip_in_disk = io_range->start_strip_in_disk;
	if (disk_idx < io_range->start_disk) {
		start_strip_in_disk += 1;
	}

	/* [한국어] 기본 끝 strip은 io_range의 값. disk_idx가 end_disk보다 크면 -1 (마지막 사이클에서
	 * 자기 차례 전에 범위가 끝나므로 직전 사이클까지만 포함). */
	end_strip_in_disk = io_range->end_strip_in_disk;
	if (disk_idx > io_range->end_disk) {
		end_strip_in_disk -= 1;
	}

	/* [한국어] 보정 후 끝 ≥ 시작 이어야 정상. n_disks_involved 가 disk_idx 를 포함한다는 전제이므로
	 * 호출자가 _raid0_get_io_range에서 결정한 disk만 본 함수에 들어와야 한다. */
	assert(end_strip_in_disk >= start_strip_in_disk);
	/* [한국어] 디스크가 처리할 strip 수 (이 디스크에서). */
	n_strips_in_disk = end_strip_in_disk - start_strip_in_disk + 1;

	/* [한국어] 첫 strip만 부분 시작 가능, 나머지는 0부터. */
	if (disk_idx == io_range->start_disk) {
		start_offset_in_disk = io_range->start_offset_in_strip;
	} else {
		start_offset_in_disk = 0;
	}

	/* [한국어] 마지막 strip만 부분 끝 가능, 나머지는 끝까지. */
	if (disk_idx == io_range->end_disk) {
		end_offset_in_disk = io_range->end_offset_in_strip;
	} else {
		end_offset_in_disk = io_range->strip_size - 1;
	}

	/* [한국어] 디스크 LBA 시작 = (디스크 내부 시작 strip × strip_size) + 첫 strip 내부 오프셋. */
	offset_in_disk = start_offset_in_disk + start_strip_in_disk * io_range->strip_size;
	/* [한국어] 길이 = (n-1)개의 꽉 찬 strip + 마지막 strip의 [start..end] 구간 길이. */
	nblocks_in_disk = (n_strips_in_disk - 1) * io_range->strip_size
			  + end_offset_in_disk - start_offset_in_disk + 1;

	/* [한국어] 디버그 로그: 분할 결과 추적. RPC log_set_flag bdev_raid0 으로 활성화. */
	SPDK_DEBUGLOG(bdev_raid0,
		      "raid_bdev (strip_size 0x%" PRIx64 ") splits IO to base_bdev (%u) at (0x%" PRIx64 ", 0x%" PRIx64
		      ").\n",
		      io_range->strip_size, disk_idx, offset_in_disk, nblocks_in_disk);

	/* [한국어] 출력 인자 채움. 호출자는 이 (offset, num) 으로 base bdev에 발행. */
	*_offset_in_disk = offset_in_disk;
	*_nblocks_in_disk = nblocks_in_disk;
}

/* [한국어] 전방 선언: ENOMEM 재시도 콜백 _raid0_submit_null_payload_request 사용. */
static void raid0_submit_null_payload_request(struct raid_bdev_io *raid_io);

/*
 * [한국어]
 * _raid0_submit_null_payload_request - bdev_io_wait 콜백 어댑터.
 *
 * raid0_submit_null_payload_request 는 base_bdev_io_submitted 카운터로 진행상황을 추적해
 * 재진입에 안전하다.
 */
static void
_raid0_submit_null_payload_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid0_submit_null_payload_request(raid_io);
}

/*
 * [한국어]
 * raid0_base_io_complete - FLUSH/UNMAP child bdev_io 부분 완료 콜백.
 *
 * RAID 0의 FLUSH/UNMAP은 N개의 base bdev에 분할 발행되므로, 각 child가 끝날 때마다 1씩
 * 부분 보고. RAID 코어가 base_bdev_io_remaining을 감소시키고 0이 되면 자동으로
 * raid_bdev_io_complete 호출.
 */
static void
raid0_base_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	/* [한국어] 부분 완료 보고 (1 개 child). */
	raid_bdev_io_complete_part(raid_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);

	/* [한국어] child bdev_io 반납. */
	spdk_bdev_free_io(bdev_io);
}

/*
 * brief:
 * raid0_submit_null_payload_request function submits the next batch of
 * io requests with range but without payload, like FLUSH and UNMAP, to member disks;
 * it will submit as many as possible unless one base io request fails with -ENOMEM,
 * in which case it will queue itself for later submission.
 * params:
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * returns:
 * none
 */
/*
 * [한국어]
 * raid0_submit_null_payload_request - FLUSH/UNMAP을 관여 디스크들에 분할 발행.
 *
 * @raid_io: type=FLUSH 또는 UNMAP 의 부모 raid_bdev_io.
 *
 * 절차:
 *  1) _raid0_get_io_range로 입력 범위 분석 (strip 시작/끝, 디스크 인덱스, n_disks_involved).
 *  2) 첫 진입 시 base_bdev_io_remaining = n_disks_involved 설정 (재진입 시 보존).
 *  3) base_bdev_io_submitted 부터 시작해 라운드 로빈 순서로 (start_disk, start_disk+1, ...)
 *     n_disks_involved 만큼 발행. 각 디스크에 대해 _raid0_split_io_range로 (offset,num) 산출.
 *  4) ENOMEM 시 wait 큐 등록 후 return → 자원 회복 시 동일 함수가 submitted 부터 이어서.
 *
 * 실행 컨텍스트: 발행 스레드 (RAID 채널이 만들어진 스레드).
 */
static void
raid0_submit_null_payload_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev		*raid_bdev;
	struct raid_bdev_io_range	io_range;
	int				ret;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	raid_bdev = raid_io->raid_bdev;

	/* [한국어] 1차 분석: 입력 범위 분석. base_bdev_io_submitted/Remaining 과 무관하게
	 * 매번 동일 결과를 산출 (재진입 시에도 동일하므로 캐싱 불필요). */
	_raid0_get_io_range(&io_range, raid_bdev->num_base_bdevs,
			    raid_bdev->strip_size, raid_bdev->strip_size_shift,
			    raid_io->offset_blocks, raid_io->num_blocks);

	if (raid_io->base_bdev_io_remaining == 0) {
		/* [한국어] 첫 진입: 발행 예정 child 총 개수 = 관여 디스크 수. RAID 코어가
		 * raid_bdev_io_complete_part 호출마다 1씩 감소시켜 0이 되면 사용자에게 통지. */
		raid_io->base_bdev_io_remaining = io_range.n_disks_involved;
	}

	/* [한국어] 미발행 분량만 순서대로 발행. submitted는 누적 카운터. */
	while (raid_io->base_bdev_io_submitted < io_range.n_disks_involved) {
		uint8_t disk_idx;
		uint64_t offset_in_disk;
		uint64_t nblocks_in_disk;

		/* base_bdev is started from start_disk to end_disk.
		 * It is possible that index of start_disk is larger than end_disk's.
		 */
		/* [한국어] start_disk 부터 라운드 로빈으로 (start_disk + submitted) % N 의 디스크 선택.
		 * end_disk 가 start_disk 보다 작더라도 modulo로 자연스럽게 wrap. */
		disk_idx = (io_range.start_disk + raid_io->base_bdev_io_submitted) % raid_bdev->num_base_bdevs;
		base_info = &raid_bdev->base_bdev_info[disk_idx];
		base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, disk_idx);

		/* [한국어] 이 디스크의 [offset, num] 슬라이스 산출. */
		_raid0_split_io_range(&io_range, disk_idx, &offset_in_disk, &nblocks_in_disk);

		switch (raid_io->type) {
		case SPDK_BDEV_IO_TYPE_UNMAP:
			/* [한국어] UNMAP: 해당 슬라이스 LBA 구간을 할당 해제. NVMe Dataset Management 0x09. */
			ret = raid_bdev_unmap_blocks(base_info, base_ch,
						     offset_in_disk, nblocks_in_disk,
						     raid0_base_io_complete, raid_io);
			break;

		case SPDK_BDEV_IO_TYPE_FLUSH:
			/* [한국어] FLUSH: 해당 base bdev의 캐시를 영속 매체로 동기화. */
			ret = raid_bdev_flush_blocks(base_info, base_ch,
						     offset_in_disk, nblocks_in_disk,
						     raid0_base_io_complete, raid_io);
			break;

		default:
			/* [한국어] R/W 외 type이 들어오면 안 됨. */
			SPDK_ERRLOG("submit request, invalid io type with null payload %u\n", raid_io->type);
			assert(false);
			ret = -EIO;
		}

		if (ret == 0) {
			/* [한국어] 발행 성공: submitted 증가. */
			raid_io->base_bdev_io_submitted++;
		} else if (ret == -ENOMEM) {
			/* [한국어] 자원 부족: wait 큐 등록 후 즉시 return. submitted 유지 → 재진입 시 동일 disk부터. */
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
						base_ch, _raid0_submit_null_payload_request);
			return;
		} else {
			/* [한국어] 비ENOMEM 실패: 즉시 raid_bdev_io_complete(FAILED). */
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}
}

/*
 * [한국어]
 * raid0_start - RAID 0 인스턴스 시작 시 모듈 전용 초기화.
 *
 * @raid_bdev: 코어가 base bdev들을 검증·오픈한 후 호출. base_info[]에 데이터 영역 크기가
 *             채워진 상태.
 * @return   : 0 성공.
 *
 * 절차:
 *  - 모든 base bdev 의 data_size 중 최소값을 찾음 (RAID 0는 모든 디스크가 동일 크기여야
 *    완전 활용; 작은 디스크에 맞춰 큰 디스크의 자투리는 사용 안 함).
 *  - 최소값을 strip 경계로 내림 정렬한 값을 모든 base_info의 새 data_size로 통일.
 *  - 가상 RAID 크기 = data_size × N.
 *  - num_base_bdevs > 1 일 때만 split_on_optimal_io_boundary=true (단일 디스크면 split 불필요).
 */
static int
raid0_start(struct raid_bdev *raid_bdev)
{
	/* [한국어] 모든 base bdev의 data_size 최소값 후보. UINT64_MAX로 시작해 줄여 나감. */
	uint64_t min_blockcnt = UINT64_MAX;
	/* [한국어] strip 정렬을 거친 공통 data_size. */
	uint64_t base_bdev_data_size;
	struct raid_base_bdev_info *base_info;

	/* [한국어] 1차 패스: 최소 data_size 검색. */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		/* Calculate minimum block count from all base bdevs */
		min_blockcnt = spdk_min(min_blockcnt, base_info->data_size);
	}

	/* [한국어] 최소값을 strip 경계로 내림 (자투리 strip 잘라냄). 시프트 두 번으로 빠른 floor. */
	base_bdev_data_size = (min_blockcnt >> raid_bdev->strip_size_shift) << raid_bdev->strip_size_shift;

	/* [한국어] 2차 패스: 모든 base bdev의 data_size를 통일된 값으로 갱신. */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->data_size = base_bdev_data_size;
	}

	/*
	 * Take the minimum block count based approach where total block count
	 * of raid bdev is the number of base bdev times the minimum block count
	 * of any base bdev.
	 */
	SPDK_DEBUGLOG(bdev_raid0, "min blockcount %" PRIu64 ",  numbasedev %u, strip size shift %u\n",
		      min_blockcnt, raid_bdev->num_base_bdevs, raid_bdev->strip_size_shift);

	/* [한국어] 가상 RAID bdev 총 크기 = N * 디스크당 data_size. */
	raid_bdev->bdev.blockcnt = base_bdev_data_size * raid_bdev->num_base_bdevs;

	if (raid_bdev->num_base_bdevs > 1) {
		/* [한국어] R/W가 strip 경계 안에 머물도록 lib/bdev이 자동 split하게 요청.
		 * raid0_submit_rw_request 의 단일-strip 가정이 이로써 성립. */
		raid_bdev->bdev.optimal_io_boundary = raid_bdev->strip_size;
		raid_bdev->bdev.split_on_optimal_io_boundary = true;
	} else {
		/* Do not need to split reads/writes on single bdev RAID modules. */
		/* [한국어] 단일 디스크 RAID 0 은 그냥 패스스루이므로 split 불필요 → 성능 향상. */
		raid_bdev->bdev.optimal_io_boundary = 0;
		raid_bdev->bdev.split_on_optimal_io_boundary = false;
	}

	return 0;
}

/*
 * [한국어]
 * raid0_resize - base bdev 크기 변경 시 RAID 가상 크기 재계산.
 *
 * @raid_bdev: 대상 RAID bdev.
 * @return   : true 변경됨, false 변경 없음/실패.
 *
 * 호출 시점: base bdev 중 하나가 (보통 같은 양만큼) 커졌을 때 RAID 코어가 호출. 모든 디스크
 * 의 새 가용량 (block - data_offset) 중 최소값을 찾아 strip 경계로 정렬, 새 RAID blockcnt 산출.
 * 변경 시 spdk_bdev_notify_blockcnt_change 로 lib/bdev에 통지 (사용자 측에서 새 크기 확인 가능).
 */
static bool
raid0_resize(struct raid_bdev *raid_bdev)
{
	uint64_t blockcnt;
	int rc;
	uint64_t min_blockcnt = UINT64_MAX;
	struct raid_base_bdev_info *base_info;
	uint64_t base_bdev_data_size;

	/* [한국어] 1차: base bdev들의 새 가용량(블록 수 - data_offset) 최소값 산출. */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		struct spdk_bdev *base_bdev = spdk_bdev_desc_get_bdev(base_info->desc);

		min_blockcnt = spdk_min(min_blockcnt, base_bdev->blockcnt - base_info->data_offset);
	}

	/* [한국어] strip 경계로 내림 정렬한 디스크당 data_size, 그리고 가상 총 크기. */
	base_bdev_data_size = (min_blockcnt >> raid_bdev->strip_size_shift) << raid_bdev->strip_size_shift;
	blockcnt = base_bdev_data_size * raid_bdev->num_base_bdevs;

	if (blockcnt == raid_bdev->bdev.blockcnt) {
		/* [한국어] 변경 없음 → false 반환 (호출자는 실 변경 없음으로 인식). */
		return false;
	}

	/* [한국어] lib/bdev에 새 blockcnt 통지. 구독자(상위 레이어/사용자)에게 이벤트 전파. */
	rc = spdk_bdev_notify_blockcnt_change(&raid_bdev->bdev, blockcnt);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to notify blockcount change\n");
		return false;
	}

	/* [한국어] 통지 성공 후에야 모든 base_info의 data_size를 새 값으로 갱신 (롤백 불필요한 순서). */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->data_size = base_bdev_data_size;
	}

	return true;
}

/*
 * [한국어]
 * g_raid0_module - RAID 0 모듈 디스크립터. RAID_MODULE_REGISTER로 코어에 자동 등록.
 */
static struct raid_bdev_module g_raid0_module = {
	/* [한국어] 처리할 RAID 레벨 식별자. */
	.level = RAID0,
	/* [한국어] 최소 base bdev 수 = 1 (단일 디스크 RAID 0 = passthrough 형태로 허용). */
	.base_bdevs_min = 1,
	/* [한국어] memory_domain 패스스루 지원 (RAID 0는 데이터 변환 없음). */
	.memory_domains_supported = true,
	/* [한국어] DIF/T10-PI 지원 (REFTAG 검증 포함). */
	.dif_supported = true,
	/* [한국어] 시작 콜백: data_size 통일 + blockcnt 설정. */
	.start = raid0_start,
	/* [한국어] R/W 발행 콜백. */
	.submit_rw_request = raid0_submit_rw_request,
	/* [한국어] FLUSH/UNMAP 발행 콜백. */
	.submit_null_payload_request = raid0_submit_null_payload_request,
	/* [한국어] resize 콜백: base bdev 확장 시 RAID 가상 크기 갱신. concat은 미구현, RAID 0는 지원. */
	.resize = raid0_resize,
};
/* [한국어] g_raid0_module 을 RAID 코어 모듈 리스트에 자동 등록. constructor attribute 활용. */
RAID_MODULE_REGISTER(&g_raid0_module)

/* [한국어] "bdev_raid0" 디버그 로그 컴포넌트 등록. */
SPDK_LOG_REGISTER_COMPONENT(bdev_raid0)
