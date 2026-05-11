/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright (c) Peng Yu yupeng0921@gmail.com.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK RAID concat (선형 이어붙이기) 모듈 구현 (concat.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 SPDK bdev RAID 프레임워크의 "concat" 레벨을 구현한다. concat은 RAID 0/1/5F와는
 * 달리 스트라이핑·미러링·패리티 같은 분산을 수행하지 않고, 여러 base bdev들을 LBA 공간에서
 * 순차적으로 이어붙여(linear append) 하나의 큰 가상 블록 디바이스로 노출한다. 즉 base bdev 0
 * 의 LBA 범위가 [0..L0)에 매핑되고, base bdev 1은 [L0..L0+L1)에, base bdev k는 [Σ_{i<k}Li ..
 * Σ_{i≤k}Li)에 매핑된다. 따라서 단일 I/O는 항상 정확히 하나의 base bdev로 라우팅되며,
 * (FLUSH/UNMAP 같은 range 기반 null-payload 요청만 다중 base bdev에 분할 발행될 수 있다).
 *
 * 이 모듈의 핵심 가치는 "여러 디스크를 단순 이어붙여 용량 합산만 제공"하는 가장 단순한
 * 형태의 RAID로, RAID 코어(bdev_raid)에서 제공하는 base bdev 관리/채널/RPC/superblock
 * 인프라를 그대로 재사용하면서, RAID 모듈 인터페이스(start/stop/submit_rw/submit_null)만
 * concat 시맨틱으로 채워 등록한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK bdev I/O 스택에서 본 모듈은 lib/bdev (공통 bdev 코어)와 base bdev 모듈
 * (예: bdev_nvme, bdev_malloc) 사이에 끼어 있다. 호출 체인은 다음과 같다:
 *   사용자 → spdk_bdev_read/write
 *     → lib/bdev (raid_bdev이 가상 bdev로 등록되어 있음)
 *       → bdev_raid 코어 (raid_bdev_submit_request, module/bdev/raid/bdev_raid.c)
 *         → 본 파일의 concat_submit_rw_request / concat_submit_null_payload_request
 *           → base bdev (raid_base_bdev_info::desc → 실제 NVMe/AIO/Malloc 등)
 *             → 완료 시 concat_bdev_io_completion / concat_base_io_complete 콜백
 *               → raid_bdev_io_complete[_part]로 RAID 코어에 완료 보고
 *                 → spdk_bdev_io_complete로 상위 사용자에게 완료 통지
 * 실행 컨텍스트는 SPDK reactor 스레드(폴드 모드 user thread)이다. 모든 콜백은 I/O를
 * 발행한 동일 스레드에서 호출되도록 SPDK가 보장한다.
 *
 * === 타 모듈과의 연결 ===
 * - bdev_raid.h: RAID 코어가 노출하는 모듈 등록 매크로(RAID_MODULE_REGISTER), 헬퍼 함수
 *   (raid_bdev_readv_blocks_ext, raid_bdev_writev_blocks_ext, raid_bdev_flush_blocks,
 *   raid_bdev_unmap_blocks, raid_bdev_io_complete[_part], raid_bdev_queue_io_wait), 그리고
 *   raid_bdev / raid_bdev_io / raid_base_bdev_info / raid_bdev_io_channel 자료구조 정의.
 * - spdk/bdev_module.h: 하위 base bdev에 대한 child I/O 발행/완료 처리 (간접적으로 사용).
 * - spdk/util.h: spdk_min 매크로 등 작은 유틸 함수.
 * - 본 모듈은 raid_bdev::module_private에 자체 정의 struct concat_block_range 배열을
 *   할당하여 "각 base bdev이 RAID LBA 공간 어디에 매핑되어 있는지"를 저장한다. 이 배열은
 *   concat_start에서 만들어 raid_bdev_module의 stop이 호출되는 concat_stop에서 free한다.
 * 데이터 흐름: 상위 raid_bdev_io::offset_blocks(가상 LBA) → block_range로 base bdev 인덱스
 * 결정 → 해당 base bdev의 LBA(pd_lba)로 변환 → child bdev_io 발행 → 완료 시 raid_bdev_io
 * 단위로 성공/실패 집계.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct concat_block_range : 한 base bdev이 차지하는 RAID LBA 구간 [start, start+length).
 * - concat_start()             : RAID 시작 시 base bdev 별 block_range 테이블 구성과 총 용량
 *                                계산. raid_bdev->bdev.blockcnt 설정 및 strip 경계로 split.
 * - concat_stop()              : RAID 종료 시 block_range 메모리 해제.
 * - concat_submit_rw_request() : READ/WRITE를 적절한 단일 base bdev로 라우팅.
 * - concat_submit_null_payload_request() : FLUSH/UNMAP을 여러 base bdev에 분할 발행.
 * - concat_bdev_io_completion(), concat_base_io_complete() : child bdev_io 완료 콜백.
 * - g_concat_module / RAID_MODULE_REGISTER : RAID 코어에 concat 모듈을 정적 등록.
 */

/* [한국어] bdev_raid.h: RAID 코어가 제공하는 자료구조/헬퍼 함수 선언. raid_bdev,
 * raid_bdev_io, raid_base_bdev_info, raid_bdev_module, RAID_MODULE_REGISTER 등을 사용. */
#include "bdev_raid.h"

/* [한국어] spdk/env.h: DPDK 기반 환경 추상화 (hugepage memory, DMA, PCI). 본 파일에서
 * 직접 함수를 호출하지 않더라도 RAID 코어 헤더에서 간접 의존되는 일반 SPDK 헤더이다. */
#include "spdk/env.h"
/* [한국어] spdk/thread.h: SPDK thread/poller/메시지 추상화. 본 파일은 spdk_thread API를
 * 직접 호출하지 않지만, 콜백이 어떤 스레드에서 실행되는지 이해하기 위해 포함한다. */
#include "spdk/thread.h"
/* [한국어] spdk/string.h: SPDK 문자열 헬퍼 (spdk_strerror 등). 진단용이며 본 모듈에선
 * 직접 호출하지 않지만 일관성을 위해 RAID 모듈들에 공통 포함되어 있다. */
#include "spdk/string.h"
/* [한국어] spdk/util.h: spdk_min 매크로 등 일반 유틸. concat_submit_null_payload_request
 * 에서 분할 길이 계산에 spdk_min을 사용한다. */
#include "spdk/util.h"

/* [한국어] spdk/log.h: SPDK 로깅 매크로 (SPDK_ERRLOG, SPDK_DEBUGLOG, SPDK_LOG_REGISTER_COMPONENT). */
#include "spdk/log.h"

/*
 * [한국어]
 * struct concat_block_range
 * 단일 base bdev이 RAID 가상 LBA 공간에서 점유하는 [start, start+length) 구간 기술자.
 *
 * 각 RAID concat 인스턴스는 num_base_bdevs 길이의 이 구조체 배열을 raid_bdev::module_private
 * 에 보관한다. concat_submit_rw_request/_null_payload_request는 이 배열을 선형 검색해
 * 상위 가상 LBA → (base bdev 인덱스, base LBA) 매핑을 수행한다.
 *
 * 용량 산정 시 strip_size 경계(strip_size_shift)로 잘라낸 결과를 length로 사용해
 * "디스크 끝의 자투리"를 잘라내며, 따라서 length는 항상 strip_size의 배수이다.
 */
struct concat_block_range {
	uint64_t start;
	/* [한국어] 이 base bdev이 RAID 가상 LBA 공간에서 시작하는 LBA(블록 인덱스).
	 * 설정자: concat_start()에서 base bdev 순회 중 누적 합으로 채움 (block_range[0].start=0,
	 *         block_range[k].start = Σ_{i<k} block_range[i].length).
	 * 읽는 자: concat_submit_rw_request(), concat_submit_null_payload_request().
	 * 값 범위: 0 ≤ start ≤ raid_bdev->bdev.blockcnt; 항상 strip_size의 배수.
	 * 동기화: RAID 시작 직후 한 번만 채워지고 이후 read-only이므로 락 불필요. */

	uint64_t length;
	/* [한국어] 이 base bdev이 제공하는 RAID 사용 가능한 블록 수.
	 * 설정자: concat_start()에서 base_info->data_size를 strip_size 경계로 내림한 값을 저장.
	 * 읽는 자: I/O 라우팅 시 [start, start+length) 구간에 포함되는지 판정하기 위해 사용.
	 * 값 범위: strip_size 의 양의 배수. 0이면 해당 base bdev은 사용 불가 상태이지만 concat은
	 *         base_bdevs_min=1 이상을 요구하므로 정상 가동 시엔 0이 아니어야 함.
	 * 동기화: read-only 이후라 별도 동기화 없음. */
};

/*
 * brief:
 * concat_bdev_io_completion function is called by lower layers to notify raid
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
 * concat_bdev_io_completion - READ/WRITE child bdev_io 완료 콜백.
 *
 * @bdev_io: 하위 base bdev이 완료한 child bdev_io 포인터. 완료 후 free 대상.
 * @success: 하위 layer가 보고한 성공 여부 (true=성공, false=실패/타임아웃).
 * @cb_arg : 부모 raid_bdev_io 포인터. concat_submit_rw_request에서 cb_arg로 등록됨.
 * @return : 없음.
 *
 * concat은 READ/WRITE를 단일 base bdev에 1:1로 발행하므로 child 1개 = parent 1개 완료이며,
 * 별도의 부분 완료 집계가 불필요하다. 따라서 raid_bdev_io_complete()를 한 번 호출해
 * 부모 raid_bdev_io를 즉시 종료한다. (FLUSH/UNMAP은 concat_base_io_complete를 사용해
 * raid_bdev_io_complete_part로 부분 집계하는 별도 경로를 사용.)
 *
 * 실행 컨텍스트: child bdev_io를 발행한 SPDK reactor 스레드. 같은 스레드에서 콜백되므로
 * lockless로 raid_bdev_io 상태를 갱신할 수 있다.
 *
 * 호출 체인:
 *   하위 base bdev (예: bdev_nvme의 NVMe 완료 폴링) → spdk_bdev_io_complete
 *     → bdev 코어 → 본 콜백 → raid_bdev_io_complete → 상위 spdk_bdev_io 완료
 */
static void
concat_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	/* [한국어] cb_arg에 저장해두었던 부모 raid_bdev_io 복원. concat_submit_rw_request에서
	 * 명시적으로 raid_io를 cb_arg로 넘겼기 때문에 그대로 캐스팅 가능. */
	struct raid_bdev_io *raid_io = cb_arg;

	/* [한국어] child bdev_io는 더 이상 필요 없으므로 bdev 코어에 반환. spdk_bdev_free_io는
	 * bdev_io를 발행한 스레드에서만 호출 가능 (per-thread 캐시 풀에 반환됨). */
	spdk_bdev_free_io(bdev_io);

	/* [한국어] 하위 결과를 RAID 코어 상태값으로 매핑하여 부모 raid_bdev_io 종료 통지. */
	if (success) {
		/* [한국어] 정상 완료 → 상위 사용자에게 SUCCESS 전달. raid_bdev_io_complete은
		 * 내부적으로 spdk_bdev_io_complete을 호출해 lib/bdev 레이어의 완료 큐에 진입. */
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		/* [한국어] 실패 시 FAILED 전달. concat은 redundancy가 없으므로 단일 base bdev
		 * 실패 = 전체 raid_bdev_io 실패 (RAID 1처럼 다른 미러로 retry할 여지 없음). */
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* [한국어] 전방 선언: concat_submit_rw_request는 _concat_submit_rw_request에서도 호출되므로
 * 컴파일러가 호출 시점에 시그니처를 알도록 미리 선언. ENOMEM 시 raid_bdev_queue_io_wait의
 * 콜백으로 _concat_submit_rw_request가 등록되며, 자원이 회복되면 다시 발행을 재시도한다. */
static void concat_submit_rw_request(struct raid_bdev_io *raid_io);

/*
 * [한국어]
 * _concat_submit_rw_request - raid_bdev_queue_io_wait 콜백 래퍼.
 *
 * @_raid_io: void* 형태로 큐에 보관되었던 raid_bdev_io 포인터.
 *
 * SPDK bdev_io_wait 콜백 시그니처는 void(*)(void*)이므로, 직접 concat_submit_rw_request를
 * 등록할 수 없다. 본 함수는 그 시그니처 어댑터 역할만 하며, 인자를 raid_bdev_io* 로 캐스팅해
 * concat_submit_rw_request를 다시 호출한다.
 *
 * 실행 컨텍스트: child bdev이 자원을 회복했을 때 SPDK가 호출하며, 원래 raid_io를 발행한
 * 스레드에서 실행된다 (bdev_io_wait_entry가 해당 thread에 enqueue 되었으므로).
 */
static void
_concat_submit_rw_request(void *_raid_io)
{
	/* [한국어] void* → 부모 raid_bdev_io* 로 복원. */
	struct raid_bdev_io *raid_io = _raid_io;

	/* [한국어] 실제 발행 함수 재호출. ENOMEM 상황은 동일 함수가 재진입에 안전하도록
	 * 작성되어 있어야 한다 (READ/WRITE는 base_bdev_io_submitted/_remaining 추적 없이
	 * 단일 child만 발행하므로 자연스럽게 idempotent). */
	concat_submit_rw_request(raid_io);
}

/*
 * brief:
 * concat_submit_rw_request function is used to submit I/O to the correct
 * member disk for concat bdevs.
 * params:
 * raid_io
 * returns:
 * none
 */
/*
 * [한국어]
 * concat_submit_rw_request - READ/WRITE I/O를 알맞은 단일 base bdev에 라우팅.
 *
 * @raid_io: 상위 사용자가 발행한 raid_bdev_io. offset_blocks(가상 LBA), num_blocks,
 *           iovs/iovcnt(데이터 버퍼), type(READ/WRITE), memory_domain 등을 담고 있음.
 * @return : 없음. 결과는 raid_bdev_io_complete 또는 ENOMEM 큐잉으로 비동기 통지.
 *
 * 동기/배경: concat은 가상 LBA 공간을 base bdev 단위로 직선적으로 분할하므로, 입력 LBA가
 * 어느 base bdev에 속하는지를 block_range[]에서 찾아 한 번의 child bdev_io로 그대로 forward
 * 하면 된다. RAID 코어가 split_on_optimal_io_boundary=true 와 optimal_io_boundary=strip_size
 * 를 통해 strip 경계를 넘는 I/O를 잘게 잘라주기 때문에 본 함수에선 분할이 필요 없다.
 *
 * 단계:
 *  1) block_range[]를 선형 스캔하여 raid_io->offset_blocks ≥ block_range[i].start 인 가장 큰
 *     i를 pd_idx로 결정. (정렬된 누적합이므로 첫 번째로 start가 더 큰 i 직전이 답.)
 *  2) pd_lba = offset_blocks - block_range[pd_idx].start, pd_blocks = num_blocks 로 변환.
 *  3) base_info, base_ch, io_opts 준비 후 type에 따라 readv/writev_blocks_ext 호출.
 *  4) -ENOMEM이면 bdev_io_wait 큐에 등록해 자원 회복 시 재호출, 그 외 비ENOMEM 에러는 즉시
 *     raid_bdev_io_complete(FAILED)로 종료 (assert(false)는 이 경로가 정상에서 안 와야 함을 표명).
 *
 * 실행 컨텍스트: 사용자가 spdk_bdev_writev/readv를 호출한 SPDK 스레드와 동일 스레드.
 *
 * 호출 체인:
 *   spdk_bdev_writev/readv → bdev 코어 → raid_bdev_submit_request →
 *     [본 함수] → raid_bdev_readv_blocks_ext / writev_blocks_ext → base bdev → 완료 콜백
 */
static void
concat_submit_rw_request(struct raid_bdev_io *raid_io)
{
	/* [한국어] RAID I/O 채널 (현재 reactor에서 base bdev들과 통신하기 위한 per-thread 채널 모음).
	 * 베이스 bdev별 child 채널은 raid_bdev_channel_get_base_channel()로 얻는다. */
	struct raid_bdev_io_channel	*raid_ch = raid_io->raid_ch;
	/* [한국어] 부모 raid_bdev (가상 디바이스) 포인터. block_range, base_bdev_info, num_base_bdevs
	 * 등 모든 라우팅 메타데이터의 출처. */
	struct raid_bdev		*raid_bdev = raid_io->raid_bdev;
	/* [한국어] concat_start에서 raid_bdev->module_private에 보관해둔 base bdev별 LBA 매핑 표. */
	struct concat_block_range	*block_range = raid_bdev->module_private;
	/* [한국어] 선택된 base bdev의 로컬 LBA. 가상 LBA에서 base bdev 시작 오프셋을 뺀 값. */
	uint64_t			pd_lba;
	/* [한국어] 발행할 block 수. concat은 한 base bdev로만 보내므로 raid_io->num_blocks와 동일. */
	uint64_t			pd_blocks;
	/* [한국어] 선택된 base bdev의 인덱스 (0..num_base_bdevs-1). -1이면 미발견 = 버그. */
	int				pd_idx;
	/* [한국어] 하위 발행 함수 반환값. 0=발행 성공, -ENOMEM=자원 부족, 그 외=즉시 실패. */
	int				ret = 0;
	/* [한국어] 선택된 base bdev의 정보 (descriptor, data_size, 상태 등). */
	struct raid_base_bdev_info	*base_info;
	/* [한국어] base bdev에 I/O를 발행하기 위한 SPDK I/O 채널 (per-thread). */
	struct spdk_io_channel		*base_ch;
	/* [한국어] 확장 I/O 옵션 구조체. memory_domain, metadata 등 RDMA/zero-copy 메타를 전달. */
	struct spdk_bdev_ext_io_opts	io_opts = {};
	/* [한국어] base bdev 순회 인덱스. */
	int i;

	/* [한국어] 미발견 표식으로 -1 초기화. 이후 루프 중 갱신되어 마지막 적합 인덱스를 보유. */
	pd_idx = -1;
	/* [한국어] block_range[]는 start 오름차순 정렬이므로 start ≤ offset 인 가장 큰 i를 찾는다.
	 * start > offset 이 되는 i 직전을 답으로 채택 → 그 시점에서 break. */
	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/* [한국어] 더 이상 적합한 후보가 없음 (start가 입력 오프셋보다 큼). 루프 탈출. */
		if (block_range[i].start > raid_io->offset_blocks) {
			break;
		}
		/* [한국어] 후보 갱신. 이후 더 큰 start가 나오면 갱신 안 되므로 마지막 갱신값이 답. */
		pd_idx = i;
	}
	/* [한국어] pd_idx == -1 이면 raid_io->offset_blocks < block_range[0].start 인 비정상 상황.
	 * RAID 코어가 0..blockcnt-1 범위로 정규화해 전달하므로 정상 경로에선 발생 불가. */
	assert(pd_idx >= 0);
	/* [한국어] 마찬가지로 선택된 base bdev의 시작 LBA보다 입력이 크거나 같아야 함을 단언. */
	assert(raid_io->offset_blocks >= block_range[pd_idx].start);
	/* [한국어] 가상 LBA → base bdev 로컬 LBA 변환 (오프셋만큼 빼기). */
	pd_lba = raid_io->offset_blocks - block_range[pd_idx].start;
	/* [한국어] concat은 strip_size 경계로 split되어 단일 strip 안에 들어오므로 그대로 사용. */
	pd_blocks = raid_io->num_blocks;
	/* [한국어] 선택된 base bdev의 정보 핸들. desc(=spdk_bdev_desc)가 NULL이면 detach 상태. */
	base_info = &raid_bdev->base_bdev_info[pd_idx];
	if (base_info->desc == NULL) {
		/* [한국어] 상위 RAID 코어가 일관되게 발행을 차단해야 하지만, 안전망으로 진단 후
		 * assert(0)으로 즉시 종료. concat은 redundancy가 없어 desc==NULL은 치명적. */
		SPDK_ERRLOG("base bdev desc null for pd_idx %u\n", pd_idx);
		assert(0);
	}

	/*
	 * Submit child io to bdev layer with using base bdev descriptors, base
	 * bdev lba, base bdev child io length in blocks, buffer, completion
	 * function and function callback context
	 */
	/* [한국어] raid_ch가 NULL이면 RAID 채널이 만들어지기 전에 호출된 비정상 상태. */
	assert(raid_ch != NULL);
	/* [한국어] 현재 SPDK reactor 스레드에 매핑된 base bdev의 spdk_io_channel 획득. */
	base_ch = raid_bdev_channel_get_base_channel(raid_ch, pd_idx);

	/* [한국어] ext_io_opts ABI 호환을 위해 사이즈 명시 (앞쪽 필드만 인식하도록). */
	io_opts.size = sizeof(io_opts);
	/* [한국어] 메모리 도메인 패스스루: RDMA 등에서 사용. concat은 변환 없이 그대로 전달만 함. */
	io_opts.memory_domain = raid_io->memory_domain;
	io_opts.memory_domain_ctx = raid_io->memory_domain_ctx;
	/* [한국어] DIF/T10-PI 등 메타데이터 버퍼 패스스루. concat은 메타데이터를 가공하지 않음. */
	io_opts.metadata = raid_io->md_buf;

	if (raid_io->type == SPDK_BDEV_IO_TYPE_READ) {
		/* [한국어] READ 경로: 선택된 base bdev에서 [pd_lba, pd_lba+pd_blocks) 만큼 읽기.
		 * raid_bdev_readv_blocks_ext는 내부적으로 spdk_bdev_readv_blocks_ext_with_md
		 * 호출. 완료 시 concat_bdev_io_completion이 raid_io를 cb_arg로 받아 호출됨. */
		ret = raid_bdev_readv_blocks_ext(base_info, base_ch,
						 raid_io->iovs, raid_io->iovcnt,
						 pd_lba, pd_blocks, concat_bdev_io_completion,
						 raid_io, &io_opts);
	} else if (raid_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		/* [한국어] WRITE 경로: 동일하게 단일 base bdev로 그대로 전달. */
		ret = raid_bdev_writev_blocks_ext(base_info, base_ch,
						  raid_io->iovs, raid_io->iovcnt,
						  pd_lba, pd_blocks, concat_bdev_io_completion,
						  raid_io, &io_opts);
	} else {
		/* [한국어] RAID 코어가 RW가 아닌 type을 본 함수에 라우팅해서는 안 된다. FLUSH/UNMAP은
		 * submit_null_payload_request로 가야 하고, RESET 등은 코어에서 처리. 진단 후 abort. */
		SPDK_ERRLOG("Recvd not supported io type %u\n", raid_io->type);
		assert(0);
	}

	if (ret == -ENOMEM) {
		/* [한국어] base bdev이 일시적으로 자원이 부족해 발행을 거부. SPDK는 자원 회복 시
		 * 콜백을 호출해주는 bdev_io_wait 메커니즘을 제공한다. 콜백으로 본 함수의 래퍼인
		 * _concat_submit_rw_request를 등록해 두면, base bdev이 여유가 생기면 다시 호출됨.
		 * 큐는 raid_io 단위로 한 번만 등록되므로 중복 등록은 안전하지 않지만, READ/WRITE
		 * 는 단일 child만 발행하므로 ENOMEM 시점에 다른 child가 이미 진행 중일 일이 없음. */
		raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _concat_submit_rw_request);
	} else if (ret != 0) {
		/* [한국어] ENOMEM이 아닌 음수 반환은 정상 운영 중 발생하면 안 되는 경로 (인자 오류,
		 * descriptor 손상 등). 디버그 빌드에선 abort 시키고 release 빌드에선 FAILED로 종료. */
		SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
		assert(false);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* [한국어] 전방 선언: ENOMEM 재시도 콜백에서 참조하기 위해 미리 선언. 함수는 base_bdev_io_remaining/
 * base_bdev_io_submitted 카운터를 보고 진행상황을 이어가도록 작성되어 재진입 안전. */
static void concat_submit_null_payload_request(struct raid_bdev_io *raid_io);

/*
 * [한국어]
 * _concat_submit_null_payload_request - bdev_io_wait 시그니처 어댑터.
 *
 * @_raid_io: void* 형태의 raid_bdev_io 포인터.
 *
 * concat_submit_null_payload_request는 raid_io의 base_bdev_io_submitted 진행값을 기준으로
 * 이미 발행된 child를 건너뛰며 이어서 발행하므로, 본 어댑터를 통해 ENOMEM 회복 시 재호출되어도
 * 동일한 발행을 중복 수행하지 않는다.
 */
static void
_concat_submit_null_payload_request(void *_raid_io)
{
	/* [한국어] void*에서 부모 raid_bdev_io* 복원. */
	struct raid_bdev_io *raid_io = _raid_io;

	/* [한국어] 재진입: 이전에 ENOMEM으로 중단된 시점부터 이어서 발행을 재시도. */
	concat_submit_null_payload_request(raid_io);
}

/*
 * [한국어]
 * concat_base_io_complete - FLUSH/UNMAP child bdev_io 부분 완료 콜백.
 *
 * @bdev_io: 완료된 child bdev_io. 여기서 free.
 * @success: 성공 여부.
 * @cb_arg : 부모 raid_bdev_io.
 *
 * FLUSH/UNMAP은 입력 LBA 범위가 여러 base bdev에 걸칠 수 있어 N개의 child가 발행된다.
 * 본 콜백은 raid_bdev_io_complete_part(raid_io, 1, status)를 통해 1개 child가 완료됐음을
 * 보고하고, RAID 코어가 base_bdev_io_remaining 카운터로 모든 자식이 완료되면 자동으로
 * 부모 raid_bdev_io를 종료한다 (성공/실패 누적 정책은 코어에서 처리).
 *
 * 실행 컨텍스트: 발행 스레드와 동일 스레드. lockless로 카운터 갱신 가능.
 */
static void
concat_base_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	/* [한국어] cb_arg에 들어 있던 부모 raid_bdev_io 복원. */
	struct raid_bdev_io *raid_io = cb_arg;

	/* [한국어] "자식 1개 완료"를 RAID 코어에 보고. 코어는 내부 카운터를 1 감소시키고,
	 * 0이 되면 누적된 최악 상태값을 사용해 부모 raid_bdev_io_complete를 자동 호출한다. */
	raid_bdev_io_complete_part(raid_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);

	/* [한국어] child bdev_io 반납. spdk_bdev_free_io는 발행 스레드에서만 호출 가능하므로
	 * 본 콜백 컨텍스트에서 안전. */
	spdk_bdev_free_io(bdev_io);
}

/*
 * brief:
 * concat_submit_null_payload_request function submits the next batch of
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
 * concat_submit_null_payload_request - FLUSH/UNMAP을 여러 base bdev에 분할 발행.
 *
 * @raid_io: 부모 raid_bdev_io. type은 SPDK_BDEV_IO_TYPE_FLUSH 또는 _UNMAP.
 * @return : 없음. 결과는 raid_bdev_io_complete[_part]로 비동기 통지.
 *
 * 동기/배경: FLUSH/UNMAP은 데이터 버퍼 없이 LBA 범위만 갖는 명령이며, 그 범위가 여러
 * base bdev에 걸칠 수 있다(예: 디스크 경계 가로지르는 UNMAP). 따라서 R/W와 달리 1개 입력이
 * 다수의 child를 만들어내며, 각 child는 자신이 점유한 base bdev의 [pd_lba, pd_lba+pd_blocks)
 * 만큼만 처리한다.
 *
 * 단계:
 *  1) base bdev들을 순회하며 입력 [offset, offset+num)와 교차하는 첫(start_idx)/마지막(stop_idx)
 *     인덱스를 식별. 각 base bdev에서 자를 길이도 미리 계산.
 *  2) 첫 진입 시 base_bdev_io_remaining = stop_idx - start_idx + 1 로 초기화 (재진입 시 보존).
 *  3) start_idx..stop_idx 를 순회하며 base_bdev_io_submitted 만큼은 건너뛰고(=이미 발행됨)
 *     나머지에 unmap/flush_blocks 발행. 발행 성공이면 submitted 증가, ENOMEM이면 큐 등록 후 return.
 *
 * 실행 컨텍스트: 발행 스레드. ENOMEM 회복 시 _concat_submit_null_payload_request로 재진입.
 *
 * 호출 체인:
 *   사용자 spdk_bdev_unmap/flush → raid_bdev_submit_request → [본 함수] → 각 base bdev →
 *     concat_base_io_complete (N회) → raid_bdev_io_complete_part 누적 → 최종 raid_bdev_io_complete
 */
static void
concat_submit_null_payload_request(struct raid_bdev_io *raid_io)
{
	/* [한국어] 부모 RAID bdev. block_range, num_base_bdevs, base_bdev_info 등을 사용. */
	struct raid_bdev		*raid_bdev;
	/* [한국어] 하위 발행 반환값. */
	int				ret;
	/* [한국어] 현재 순회 중인 base bdev 정보. */
	struct raid_base_bdev_info	*base_info;
	/* [한국어] 현재 base bdev에 대한 SPDK I/O 채널. */
	struct spdk_io_channel		*base_ch;
	/* [한국어] 현재 base bdev에서 시작할 로컬 LBA. */
	uint64_t			pd_lba;
	/* [한국어] 현재 base bdev에서 처리할 블록 수. */
	uint64_t			pd_blocks;
	/* [한국어] 입력 가상 LBA 진행 커서 (감소하지 않고 단조 증가). */
	uint64_t			offset_blocks;
	/* [한국어] 입력 잔여 블록 수 (감소). */
	uint64_t			num_blocks;
	/* [한국어] base bdev별 LBA 매핑 표 (concat_start에서 만든 모듈 전용 자료). */
	struct concat_block_range	*block_range;
	/* [한국어] 순회 인덱스 / 시작·종료 base bdev 인덱스. */
	int				i, start_idx, stop_idx;

	/* [한국어] raid_bdev / block_range 핸들 획득. */
	raid_bdev = raid_io->raid_bdev;
	block_range = raid_bdev->module_private;

	/* [한국어] 입력 LBA 범위를 지역 변수로 복사 (이후 분할 계산하면서 차감해 나감). */
	offset_blocks = raid_io->offset_blocks;
	num_blocks = raid_io->num_blocks;
	/* [한국어] start_idx / stop_idx -1 = 미식별 상태. 1차 패스에서 식별 후 assert. */
	start_idx = -1;
	stop_idx = -1;
	/*
	 * Go through all base bdevs, find the first bdev and the last bdev
	 */
	/* [한국어] 1차 패스: 입력 범위와 교차하는 base bdev들을 식별하고, 각 base bdev에서
	 * 차지할 길이를 계산하면서 num_blocks를 차감. 실제 child 발행은 2차 패스에서 수행. */
	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/* skip the bdevs before the offset_blocks */
		/* [한국어] base bdev 끝 < offset 이면 입력 범위가 아직 시작도 안 했으므로 skip. */
		if (offset_blocks >= block_range[i].start + block_range[i].length) {
			continue;
		}
		if (start_idx == -1) {
			/* [한국어] 입력과 처음으로 교차하는 base bdev. 시작 인덱스 기록. */
			start_idx = i;
		} else {
			/*
			 * The offset_blocks might be at the middle of the first bdev.
			 * Besides the first bdev, the offset_blocks should be always
			 * at the start of the bdev.
			 */
			/* [한국어] 두 번째 이후로 진입한 base bdev에서는 입력 커서가 정확히 base bdev
			 * 경계와 일치해야 한다 (이전 base bdev에서 끝까지 소진했기 때문). 어긋나면 버그. */
			assert(offset_blocks == block_range[i].start);
		}
		/* [한국어] 현재 base bdev의 로컬 LBA = 가상 커서 - base bdev 시작. */
		pd_lba = offset_blocks - block_range[i].start;
		/* [한국어] 이 base bdev에서 처리할 블록 수 = min(잔여 num_blocks, base bdev에 남은 길이). */
		pd_blocks = spdk_min(num_blocks, block_range[i].length - pd_lba);
		/* [한국어] 가상 커서 / 잔여를 갱신해 다음 base bdev에서 이어서 계산. */
		offset_blocks += pd_blocks;
		num_blocks -= pd_blocks;
		if (num_blocks == 0) {
			/* [한국어] 잔여 0 = 이 base bdev이 마지막 child가 됨. */
			stop_idx = i;
			break;
		}
	}
	/* [한국어] 1차 패스가 정상이면 두 인덱스 모두 결정되어야 한다. */
	assert(start_idx >= 0);
	assert(stop_idx >= 0);

	if (raid_io->base_bdev_io_remaining == 0) {
		/* [한국어] 첫 진입 (재시도 아님): 발행 예정 child 총 개수를 코어 카운터에 설정.
		 * RAID 코어는 raid_bdev_io_complete_part 호출마다 이 값을 감소시켜 0이 되면
		 * 자동으로 부모 raid_bdev_io_complete를 호출. */
		raid_io->base_bdev_io_remaining = stop_idx - start_idx + 1;
	}
	/* [한국어] 2차 패스를 위해 입력 LBA / 잔여를 다시 초기 상태로 복원. */
	offset_blocks = raid_io->offset_blocks;
	num_blocks = raid_io->num_blocks;
	for (i = start_idx; i <= stop_idx; i++) {
		/* [한국어] 단언: 현재 커서가 이 base bdev의 [start, start+length) 범위 내. */
		assert(offset_blocks >= block_range[i].start);
		assert(offset_blocks < block_range[i].start + block_range[i].length);
		/* [한국어] 1차 패스와 동일한 변환 (재시도 시에도 동일 분할이 재현됨). */
		pd_lba = offset_blocks -  block_range[i].start;
		pd_blocks = spdk_min(num_blocks, block_range[i].length - pd_lba);
		offset_blocks += pd_blocks;
		num_blocks -= pd_blocks;
		/*
		 * Skip the IOs we have submitted
		 */
		/* [한국어] 재진입 시 이미 발행된 만큼은 건너뜀. base_bdev_io_submitted는 누적값. */
		if (i < start_idx + raid_io->base_bdev_io_submitted) {
			continue;
		}
		/* [한국어] 발행 대상 base bdev 정보와 채널. */
		base_info = &raid_bdev->base_bdev_info[i];
		base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, i);
		switch (raid_io->type) {
		case SPDK_BDEV_IO_TYPE_UNMAP:
			/* [한국어] UNMAP: 해당 base bdev의 [pd_lba, pd_lba+pd_blocks) 구간을 할당 해제
			 * 요청. NVMe Dataset Management(0x09) 또는 백엔드별 trim에 매핑됨. */
			ret = raid_bdev_unmap_blocks(base_info, base_ch,
						     pd_lba, pd_blocks,
						     concat_base_io_complete, raid_io);
			break;
		case SPDK_BDEV_IO_TYPE_FLUSH:
			/* [한국어] FLUSH: 해당 base bdev의 캐시를 영속 매체로 동기화 요청.
			 * 일부 백엔드는 LBA 범위를 무시하고 디바이스 전체 flush를 수행할 수 있음. */
			ret = raid_bdev_flush_blocks(base_info, base_ch,
						     pd_lba, pd_blocks,
						     concat_base_io_complete, raid_io);
			break;
		default:
			/* [한국어] RAID 코어가 NULL-payload 외 type을 보내면 안 됨. 진단 후 실패 표식. */
			SPDK_ERRLOG("submit request, invalid io type with null payload %u\n", raid_io->type);
			assert(false);
			ret = -EIO;
		}
		if (ret == 0) {
			/* [한국어] 발행 성공: submitted 증가. 향후 ENOMEM 재진입 시 건너뛰는 기준이 됨. */
			raid_io->base_bdev_io_submitted++;
		} else if (ret == -ENOMEM) {
			/* [한국어] 자원 부족: bdev_io_wait 큐에 _concat_submit_null_payload_request를
			 * 등록하고 즉시 return. 자원 회복 시 SPDK가 다시 호출 → 동일 함수가 진행 카운터를
			 * 보고 남은 child부터 이어서 발행. raid_bdev_io_remaining은 그대로 유지. */
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
						base_ch, _concat_submit_null_payload_request);
			return;
		} else {
			/* [한국어] ENOMEM이 아닌 비정상 음수: 진단 후 즉시 FAILED로 raid_bdev_io 종료.
			 * 일부 child는 이미 발행됐을 수 있으므로 정확히 말하면 코어가 in-flight를 마무리
			 * 한 후 종료해야 하지만, 여기선 단순화해 즉시 종료를 호출. */
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}
}

/*
 * [한국어]
 * concat_start - RAID concat 인스턴스 시작 시 모듈 전용 초기화.
 *
 * @raid_bdev: RAID 코어가 구성을 마친 raid_bdev 구조체. base_bdev_info[]에 모든 base bdev이
 *             configure된 상태. strip_size, strip_size_shift, num_base_bdevs도 채워져 있음.
 * @return   : 0 성공, 음수(-ENOMEM 등) 실패.
 *
 * 동기/배경: concat은 RAID 코어가 모든 base bdev을 검증/오픈한 후, 자기만의 LBA 매핑 표
 * (block_range[])를 만들어 module_private에 보관해야 한다. 동시에 base_info->data_size를
 * strip_size 경계로 정렬해 자투리를 잘라낸다 (R/W가 strip 경계를 넘지 않게 split되도록).
 *
 * 실행 컨텍스트: RPC bdev_raid_create / superblock 로드에서 호출되는 RAID 코어의 시작 경로.
 * 단일 스레드(주로 SPDK app thread)에서 한 번만 호출됨.
 *
 * 호출 체인:
 *   raid_bdev_configure → raid_bdev_module->start = [본 함수] → calloc + 매핑 산출
 *
 * 부수 효과:
 *  - base_info->data_size 가 strip_size 경계로 내림 정렬됨.
 *  - raid_bdev->module_private 에 block_range 배열 저장.
 *  - raid_bdev->bdev.blockcnt 가 모든 base bdev의 정렬된 합으로 설정됨.
 *  - optimal_io_boundary / split_on_optimal_io_boundary 가 strip_size로 설정되어, R/W가
 *    strip 경계를 넘지 않게 자동 분할되어 본 모듈로 들어옴.
 */
static int
concat_start(struct raid_bdev *raid_bdev)
{
	/* [한국어] 모든 base bdev의 정렬된 길이를 합산해 raid_bdev->bdev.blockcnt에 사용. */
	uint64_t total_blockcnt = 0;
	/* [한국어] base bdev 순회 포인터. RAID_FOR_EACH_BASE_BDEV 매크로의 루프 변수. */
	struct raid_base_bdev_info *base_info;
	/* [한국어] 새로 할당할 LBA 매핑 표. */
	struct concat_block_range *block_range;

	/* [한국어] base bdev 수만큼 0으로 초기화된 배열 할당. 실패 시 -ENOMEM 반환 → RAID 코어가
	 * 이 인스턴스 등록을 거부하고 정리. */
	block_range = calloc(raid_bdev->num_base_bdevs, sizeof(struct concat_block_range));
	if (!block_range) {
		SPDK_ERRLOG("Can not allocate block_range, num_base_bdevs: %u",
			    raid_bdev->num_base_bdevs);
		return -ENOMEM;
	}

	/* [한국어] block_range[] 인덱스 카운터 (RAID_FOR_EACH_BASE_BDEV는 인덱스를 노출하지 않음). */
	int idx = 0;
	/* [한국어] RAID 코어 매크로: base_bdev_info[]를 순회 (base_info는 각 항목의 포인터).
	 * 모든 base bdev은 RAID 코어가 미리 검증해 두었으므로 desc != NULL 보장 (단, 본 함수
	 * 시작 시점 기준; 이후 detach가 일어나면 별도 처리가 필요하나 그건 코어가 담당). */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		/* [한국어] base bdev이 보유한 데이터 영역을 strip 단위로 나눈 strip 개수.
		 * strip_size_shift = log2(strip_size). 우측 시프트로 빠른 나눗셈. */
		uint64_t strip_cnt = base_info->data_size >> raid_bdev->strip_size_shift;
		/* [한국어] strip 정수배만 사용. 자투리(strip_size 미만)는 잘라냄. */
		uint64_t pd_block_cnt = strip_cnt << raid_bdev->strip_size_shift;

		/* [한국어] base bdev의 사용 가능 영역을 strip 정수배로 갱신. 이후 RAID 코어/모듈은
		 * 이 값을 base bdev의 실제 한계로 간주. */
		base_info->data_size = pd_block_cnt;

		/* [한국어] 이 base bdev이 RAID LBA 공간에서 차지할 [start, start+length) 기록. */
		block_range[idx].start = total_blockcnt;
		block_range[idx].length = pd_block_cnt;
		/* [한국어] 다음 base bdev의 start 후보로 누적. */
		total_blockcnt += pd_block_cnt;
		idx++;
	}

	/* [한국어] 모듈 전용 데이터 슬롯에 block_range 배열 보관. submit 경로에서 매번 사용. */
	raid_bdev->module_private = block_range;

	/* [한국어] 디버그 로그: 총 블록 수, base bdev 수, strip shift 표시. SPDK_DEBUGLOG는
	 * "bdev_concat" 컴포넌트가 활성화된 경우에만 출력 (런타임 toggle 가능). */
	SPDK_DEBUGLOG(bdev_concat, "total blockcount %" PRIu64 ",  numbasedev %u, strip size shift %u\n",
		      total_blockcnt, raid_bdev->num_base_bdevs, raid_bdev->strip_size_shift);
	/* [한국어] 가상 RAID bdev의 총 블록 수 = 모든 base bdev의 정렬된 길이 합. */
	raid_bdev->bdev.blockcnt = total_blockcnt;

	/* [한국어] 상위 lib/bdev에 알려주는 "최적 I/O 경계". concat에서 R/W는 단일 base bdev에만
	 * 머물러야 하므로 strip_size 경계를 따라 자동 split되도록 요청. */
	raid_bdev->bdev.optimal_io_boundary = raid_bdev->strip_size;
	/* [한국어] true → bdev 코어가 큰 I/O를 strip 경계마다 child로 잘게 쪼개 본 모듈에 전달.
	 * 이 덕분에 concat_submit_rw_request에서 분할 로직이 불필요. */
	raid_bdev->bdev.split_on_optimal_io_boundary = true;

	return 0;
}

/*
 * [한국어]
 * concat_stop - RAID concat 인스턴스 종료 시 모듈 전용 자원 해제.
 *
 * @raid_bdev: 종료될 raid_bdev.
 * @return   : 항상 true. RAID 코어 인터페이스 상 false면 "비동기 stop 진행 중"을 의미하지만,
 *             concat은 즉시 동기 종료가 가능하므로 true (= 즉시 완료).
 *
 * 호출 체인:
 *   raid_bdev_destruct → raid_bdev_module->stop = [본 함수] → free
 */
static bool
concat_stop(struct raid_bdev *raid_bdev)
{
	/* [한국어] concat_start에서 할당한 매핑 표 회수. */
	struct concat_block_range *block_range = raid_bdev->module_private;

	/* [한국어] calloc과 짝을 이루는 free. RAID 코어는 module_private을 NULL로 정리하지
	 * 않으므로 모듈 책임. */
	free(block_range);

	/* [한국어] 즉시 stop 완료 보고. RAID 코어가 raid_bdev 자체를 마저 정리. */
	return true;
}

/*
 * [한국어]
 * g_concat_module - RAID concat 모듈 디스크립터.
 *
 * RAID 코어가 모듈 등록 시 보관하는 정적 객체. RAID_MODULE_REGISTER로 컴파일 시점에 RAID
 * 코어의 모듈 리스트에 자동 등록된다 (constructor attribute 기반).
 */
static struct raid_bdev_module g_concat_module = {
	/* [한국어] 이 모듈이 처리하는 RAID 레벨 식별자. enum raid_level 의 CONCAT 값. */
	.level = CONCAT,
	/* [한국어] 최소 base bdev 수. concat은 1개부터 동작 (단일 디스크 = 그 디스크 그대로 노출). */
	.base_bdevs_min = 1,
	/* [한국어] memory_domain 패스스루 지원 여부. concat은 변환 없이 그대로 전달하므로 true. */
	.memory_domains_supported = true,
	/* [한국어] 시작 콜백: block_range 구축 + blockcnt/optimal_io_boundary 설정. */
	.start = concat_start,
	/* [한국어] 종료 콜백: block_range 해제. */
	.stop = concat_stop,
	/* [한국어] R/W 발행 콜백: 단일 base bdev 라우팅. */
	.submit_rw_request = concat_submit_rw_request,
	/* [한국어] FLUSH/UNMAP 발행 콜백: 다중 base bdev 분할. */
	.submit_null_payload_request = concat_submit_null_payload_request,
};
/* [한국어] g_concat_module을 RAID 코어의 모듈 리스트에 등록. 매크로 내부에 __attribute__
 * ((constructor)) 함수가 정의되어 main() 이전에 자동 실행됨. 이로써 사용자가 RPC로
 * "raid_level": "concat" 으로 RAID를 만들 때 본 모듈이 선택된다. */
RAID_MODULE_REGISTER(&g_concat_module)

/* [한국어] "bdev_concat" 디버그 로그 컴포넌트 등록. SPDK_DEBUGLOG 매크로의 첫 인자에 사용되며,
 * `spdk_log_set_flag("bdev_concat")` 또는 RPC `log_set_flag` 로 런타임 활성화 가능. */
SPDK_LOG_REGISTER_COMPONENT(bdev_concat)
