/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK RAID 1 (미러링) 모듈 구현 (raid1.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 SPDK bdev RAID 프레임워크의 RAID 1 (mirroring) 레벨을 구현한다. RAID 1은 N개의
 * base bdev에 동일한 데이터를 복제(mirror)하여, READ는 N개 중 부하가 가장 낮은 한 곳에서
 * 가져오고, WRITE는 모든 N개에 fan-out 발행한다. 한 디스크가 실패해도 나머지 디스크가 살아
 * 있으면 데이터를 유지하므로 가용성을 제공한다 (CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL=1
 * 로 등록 — 한 개만 살아 있어도 RAID는 동작).
 *
 * 핵심 기능:
 *  - READ 분산: per-channel read_blocks_outstanding 카운터로 디스크별 부하 추적, 가장 적은
 *    디스크 선택 (간단한 LWL=Least Work Left 정책).
 *  - WRITE 미러링: 모든 base bdev에 동시 발행, 한 디스크 실패는 raid_bdev_fail_base_bdev로
 *    표시하고 나머지가 성공하면 SUCCESS 처리.
 *  - READ 오류 자동 복구: 한 디스크에서 READ 실패 시 다른 미러에서 재시도(다른 디스크들 순회).
 *    그 데이터를 실패한 디스크에 다시 write 시도(read repair). 다 실패하면 실패한 디스크를
 *    fail 처리하고 RAID는 SUCCESS로 종료 (사용자 데이터는 다른 미러에서 가져왔음).
 *  - 백그라운드 리빌드: submit_process_request로 새 미러 디스크에 데이터 복원.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   사용자 spdk_bdev_read/write
 *     → bdev_raid 코어
 *       → 본 파일 raid1_submit_rw_request
 *         (READ) raid1_submit_read_request → 단일 base bdev → 실패 시 raid1_read_other_base_bdev →
 *                            성공 시 raid1_correct_read_error (write back) → raid_bdev_io_complete
 *         (WRITE) raid1_submit_write_request → 모든 base bdev에 fan-out → raid1_write_bdev_io_completion
 *                            (1 fail = base fail mark, all done = part complete)
 *
 * 모듈 io_device 사용: 본 모듈은 자체 io_device(r1info)와 per-channel module ctx
 * (raid1_io_channel)를 등록하여 read 분산용 카운터를 보관. RAID 코어가 채널 생성 시
 * raid1_get_io_channel을 호출해 본 모듈 채널을 RAID 채널 안에 임베드.
 *
 * === 타 모듈과의 연결 ===
 * - bdev_raid.h: 코어 자료구조와 헬퍼 (raid_bdev_*_blocks_ext, raid_bdev_io_complete[_part],
 *   raid_bdev_channel_get_module_ctx, raid_bdev_fail_base_bdev, raid_bdev_io_init).
 * - spdk/likely.h: spdk_likely / spdk_unlikely 분기 예측 힌트.
 * - lib/bdev: spdk_bdev_*, spdk_io_device_register/unregister, spdk_get_io_channel.
 *
 * 데이터 흐름: WRITE는 1개의 사용자 I/O가 N개 base bdev write로 fan-out, READ는 1개의
 * 사용자 I/O가 1개 base bdev read로 라우팅 (실패 시 다른 미러로 retry). LBA 변환 없음
 * (RAID 1 가상 LBA = 모든 base bdev 로컬 LBA, 단 data_offset만 raid_bdev_*_blocks_ext에서 자동 보정).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct raid1_info       : 모듈 io_device 핸들 (raid_bdev 역참조용).
 * - struct raid1_io_channel : per-channel 카운터 (read_blocks_outstanding[N]).
 * - raid1_submit_rw_request : READ/WRITE 분기.
 * - raid1_submit_read_request : LWL 정책으로 디스크 선택 후 단일 base 발행.
 * - raid1_submit_write_request: 모든 base bdev에 미러 fan-out 발행.
 * - raid1_read_other_base_bdev: READ 실패 시 다른 미러로 재시도.
 * - raid1_correct_read_error  : 다른 미러에서 가져온 데이터를 실패 디스크에 write back.
 * - raid1_submit_null_payload_request: FLUSH/UNMAP을 모든 미러에 fan-out.
 * - raid1_submit_process_request: 백그라운드 리빌드 I/O (read 1, write 1).
 * - raid1_start / raid1_stop / raid1_resize: 라이프사이클과 동적 크기 조정.
 */

/* [한국어] bdev_raid.h: RAID 코어 인터페이스. raid_bdev, raid_bdev_io 등과 발행/완료 헬퍼. */
#include "bdev_raid.h"

/* [한국어] spdk/likely.h: spdk_likely(x), spdk_unlikely(x) — 분기 예측 힌트. ENOMEM/실패 같은
 * 드문 경로엔 unlikely, 정상 경로엔 likely 적용. */
#include "spdk/likely.h"
/* [한국어] spdk/log.h: SPDK_ERRLOG, SPDK_LOG_REGISTER_COMPONENT. */
#include "spdk/log.h"

/*
 * [한국어]
 * struct raid1_info - RAID 1 모듈의 io_device 핸들.
 *
 * 본 구조체 자체가 spdk_io_device 의 식별자(io_device 포인터)이며, 같은 RAID 인스턴스에 대해
 * 채널을 발급할 때 사용. raid_bdev 로의 역참조 용도.
 */
struct raid1_info {
	/* The parent raid bdev */
	struct raid_bdev *raid_bdev;
	/* [한국어] 부모 RAID bdev. raid1_io_device_unregister_done 에서 raid_bdev_module_stop_done
	 * 호출 시 사용.
	 * 설정자: raid1_start. 읽는 자: raid1_io_device_unregister_done.
	 * 동기화: app thread 단일 스레드 접근. */
};

/*
 * [한국어]
 * struct raid1_io_channel - per-thread 모듈 채널 컨텍스트.
 *
 * 각 SPDK 스레드(reactor) 별로 하나씩 생성되며, RAID 채널 안에 임베드된다. read 분산 정책이
 * "현재 진행 중인 read 블록 수가 가장 적은 디스크 선택" 이므로, 디스크별 outstanding 블록 수
 * 카운터를 보관한다. 길이는 가변이며 raid_bdev->num_base_bdevs 만큼.
 */
struct raid1_io_channel {
	/* Array of per-base_bdev counters of outstanding read blocks on this channel */
	uint64_t read_blocks_outstanding[0];
	/* [한국어] 디스크별 진행 중 read 블록 수 (이 채널 한정). zero-length array 로 가변 길이.
	 * 실제 길이는 raid1_start 의 spdk_io_device_register 시 sizeof(raid1_io_channel) +
	 * num_base_bdevs * sizeof(uint64_t) 로 할당.
	 * 설정자: raid1_channel_inc/dec_read_counters (read 발행/완료 시).
	 * 읽는 자: raid1_channel_next_read_base_bdev (read 디스크 선택).
	 * 값 범위: 0 ~ UINT64_MAX (assert로 오버/언더플로 가드).
	 * 동기화: 채널은 단일 스레드 소유 → lockless 갱신. */
};

/*
 * [한국어]
 * raid1_channel_inc_read_counters - read 발행 시 디스크 idx 의 outstanding 블록 수 증가.
 *
 * @raid_ch    : RAID I/O 채널 (모듈 ctx 추출용).
 * @idx        : 발행 대상 base bdev 슬롯 인덱스.
 * @num_blocks : 증가량 (= 발행한 블록 수).
 *
 * 채널은 발행 스레드 단일 소유이므로 lockless. assert로 오버플로 방지.
 */
static void
raid1_channel_inc_read_counters(struct raid_bdev_io_channel *raid_ch, uint8_t idx,
				uint64_t num_blocks)
{
	/* [한국어] RAID 채널에서 모듈 전용 컨텍스트 (raid1_io_channel) 추출. */
	struct raid1_io_channel *raid1_ch = raid_bdev_channel_get_module_ctx(raid_ch);

	/* [한국어] 오버플로 방지 단언. */
	assert(raid1_ch->read_blocks_outstanding[idx] <= UINT64_MAX - num_blocks);
	/* [한국어] 디스크별 부하 카운터 증가. 이후 LWL 정책에서 사용. */
	raid1_ch->read_blocks_outstanding[idx] += num_blocks;
}

/*
 * [한국어]
 * raid1_channel_dec_read_counters - read 완료 시 디스크 idx 의 outstanding 블록 수 감소.
 *
 * read 완료 콜백(raid1_read_bdev_io_completion)에서 항상 발행 시와 동일한 num_blocks 만큼
 * 감소. 발행/감소가 짝지어져야 카운터가 정합.
 */
static void
raid1_channel_dec_read_counters(struct raid_bdev_io_channel *raid_ch, uint8_t idx,
				uint64_t num_blocks)
{
	struct raid1_io_channel *raid1_ch = raid_bdev_channel_get_module_ctx(raid_ch);

	/* [한국어] 언더플로 방지 단언 (발행 없이 감소가 호출된 경우). */
	assert(raid1_ch->read_blocks_outstanding[idx] >= num_blocks);
	raid1_ch->read_blocks_outstanding[idx] -= num_blocks;
}

/*
 * [한국어]
 * raid1_init_ext_io_opts - spdk_bdev_ext_io_opts 를 raid_io 의 메타로 채워 초기화.
 *
 * memory_domain/메타 버퍼 패스스루를 위해 매 발행마다 동일 코드를 반복하지 않도록 유틸 분리.
 */
static void
raid1_init_ext_io_opts(struct spdk_bdev_ext_io_opts *opts, struct raid_bdev_io *raid_io)
{
	/* [한국어] 옵션 zero-init (모든 미사용 필드 0). */
	memset(opts, 0, sizeof(*opts));
	/* [한국어] ABI 호환을 위한 size 명시. */
	opts->size = sizeof(*opts);
	/* [한국어] memory_domain 패스스루 (RDMA zero-copy 등). */
	opts->memory_domain = raid_io->memory_domain;
	opts->memory_domain_ctx = raid_io->memory_domain_ctx;
	/* [한국어] T10-PI 메타 버퍼 패스스루. */
	opts->metadata = raid_io->md_buf;
}

/*
 * [한국어]
 * raid1_write_bdev_io_completion - WRITE 미러 fan-out 의 단일 child 완료 콜백.
 *
 * @bdev_io: 완료된 child bdev_io.
 * @success: 성공 여부.
 * @cb_arg : 부모 raid_bdev_io.
 *
 * 단일 미러 디스크 실패는 즉시 base bdev fail로 표시 (raid_bdev_fail_base_bdev). 그러나 부모
 * raid_io 는 part complete 만 보고하므로, 모든 미러가 실패해야 부모가 FAILED 로 종결됨
 * (default_status = FAILED 이고 일부라도 SUCCESS면 그 값으로 덮어씀).
 *
 * 즉 RAID 1의 WRITE 시맨틱: "하나라도 성공하면 SUCCESS, 모두 실패하면 FAILED, 실패한 디스크는
 * 즉시 fail 처리하여 이후 I/O가 그 디스크를 우회".
 */
static void
raid1_write_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	if (!success) {
		/* [한국어] 실패한 미러 디스크 식별 후 base bdev fail 표시. raid_bdev_channel_get_base_info
		 * 로 bdev_io 의 bdev → raid의 슬롯 정보 역검색. */
		struct raid_base_bdev_info *base_info;

		base_info = raid_bdev_channel_get_base_info(raid_io->raid_ch, bdev_io->bdev);
		if (base_info) {
			/* [한국어] 이 슬롯을 fail로 마킹. RAID 코어가 정책에 따라 슬롯을 떼어낼 수도, 유지할
			 * 수도 있음 (CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL=1 정책에 따라 살아있는 디스크가
			 * 1개 이상이면 RAID는 계속 동작). */
			raid_bdev_fail_base_bdev(base_info);
		}
	}

	/* [한국어] child bdev_io 반납. */
	spdk_bdev_free_io(bdev_io);

	/* [한국어] 부분 완료 보고. status는 SUCCESS 또는 FAILED 그대로 전달. RAID 코어가
	 * base_bdev_io_remaining 감소 + status 누적 + 0이 되면 부모 종료. */
	raid_bdev_io_complete_part(raid_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);
}

/*
 * [한국어]
 * raid1_get_read_io_base_bdev - 현재 read 진행 중인 디스크의 base_info 반환.
 *
 * READ 경로에선 base_bdev_io_submitted 가 발행한 디스크 인덱스를 보관 (WRITE 처럼 진행 카운터
 * 가 아니라 마지막 발행 인덱스). raid1_correct_read_error / read_other 가 이 함수로 식별.
 */
static struct raid_base_bdev_info *
raid1_get_read_io_base_bdev(struct raid_bdev_io *raid_io)
{
	/* [한국어] READ에서만 의미 있음. */
	assert(raid_io->type == SPDK_BDEV_IO_TYPE_READ);
	/* [한국어] base_bdev_io_submitted 는 raid1_submit_read_request 가 idx 로 설정해 둔 값. */
	return &raid_io->raid_bdev->base_bdev_info[raid_io->base_bdev_io_submitted];
}

/*
 * [한국어]
 * raid1_correct_read_error_completion - read repair 의 write 완료 콜백.
 *
 * read 실패 디스크에 다른 미러에서 읽은 데이터를 다시 write back 한 결과를 처리. 이 write가
 * 실패해도 RAID 사용자에게는 SUCCESS (이미 다른 미러에서 데이터를 가져왔기 때문). write 실패
 * 디스크는 fail 처리.
 */
static void
raid1_correct_read_error_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	/* [한국어] write child bdev_io 반납. */
	spdk_bdev_free_io(bdev_io);

	if (!success) {
		struct raid_base_bdev_info *base_info = raid1_get_read_io_base_bdev(raid_io);

		/* Writing to the bdev that had the read error failed so fail the base bdev
		 * but complete the raid_io successfully. */
		/* [한국어] read 실패 디스크에 write 도 실패 = 디스크가 손상된 것으로 결론. fail 표시.
		 * 그러나 사용자 read 자체는 다른 미러에서 성공했으므로 SUCCESS 통지. */
		raid_bdev_fail_base_bdev(base_info);
	}

	/* [한국어] 사용자 raid_io 종료 (성공 통지). */
	raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

/*
 * [한국어]
 * raid1_correct_read_error - 다른 미러에서 가져온 데이터를 실패 디스크에 write back.
 *
 * @_raid_io: void* 형태 raid_bdev_io (bdev_io_wait 콜백 시그니처와 호환).
 *
 * 동기/배경: read 실패 후 다른 미러 디스크에서 데이터를 성공적으로 가져왔다면, 실패 디스크의
 * 해당 영역을 그 데이터로 덮어써서 데이터 불일치(silent corruption)를 자율 복구한다. 이는
 * 흔히 "read repair" 라 불린다.
 *
 * base_bdev_io_submitted 에는 read 실패 디스크 인덱스가 보관되어 있고, raid_io->iovs 에는
 * 다른 미러에서 가져온 정상 데이터가 이미 채워져 있다 (raid1_read_other_completion 시점).
 *
 * 호출 체인:
 *   raid1_read_other_completion (성공) → 본 함수 → raid_bdev_writev_blocks_ext →
 *     raid1_correct_read_error_completion → raid_bdev_io_complete(SUCCESS)
 */
static void
raid1_correct_read_error(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct spdk_bdev_ext_io_opts io_opts;
	struct raid_base_bdev_info *base_info;
	struct spdk_io_channel *base_ch;
	uint8_t i;
	int ret;

	/* [한국어] read 실패 디스크 인덱스. 발행/실패 직후 보관됨. */
	i = raid_io->base_bdev_io_submitted;
	base_info = &raid_bdev->base_bdev_info[i];
	base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, i);
	/* [한국어] 채널이 NULL이면 슬롯이 사라짐 = read 실패 후 detach 됨. 이 경우 호출되지 않아야
	 * 한다 (raid1_read_other_base_bdev 가 NULL 체크 후 다른 길로 보내주므로). */
	assert(base_ch != NULL);

	/* [한국어] write 옵션 준비 (메타/도메인 패스스루). */
	raid1_init_ext_io_opts(&io_opts, raid_io);
	/* [한국어] iovs 에 든 정상 데이터를 실패 디스크의 동일 LBA에 write back. */
	ret = raid_bdev_writev_blocks_ext(base_info, base_ch, raid_io->iovs, raid_io->iovcnt,
					  raid_io->offset_blocks, raid_io->num_blocks,
					  raid1_correct_read_error_completion, raid_io, &io_opts);
	if (spdk_unlikely(ret != 0)) {
		if (ret == -ENOMEM) {
			/* [한국어] 자원 부족 → wait 큐 등록 후 재시도. */
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
						base_ch, raid1_correct_read_error);
		} else {
			/* [한국어] write 발행 자체 실패 → 디스크 fail 처리하고 raid_io 는 SUCCESS 종결.
			 * (사용자 read는 이미 다른 미러에서 성공했으므로) */
			raid_bdev_fail_base_bdev(base_info);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		}
	}
}

/* [한국어] 전방 선언: read 실패 시 다른 미러 디스크 순회 함수. ENOMEM 콜백/완료 콜백에서 호출. */
static void raid1_read_other_base_bdev(void *_raid_io);

/*
 * [한국어]
 * raid1_read_other_completion - 다른 미러에서 read 시도 완료 콜백.
 *
 * 성공 시: 정상 데이터 확보 → raid1_correct_read_error로 실패 디스크 write back.
 * 실패 시: base_bdev_io_remaining-- 후 또 다른 미러로 raid1_read_other_base_bdev 재진입.
 */
static void
raid1_read_other_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	/* [한국어] read child 반납. */
	spdk_bdev_free_io(bdev_io);

	if (!success) {
		/* [한국어] 이 미러도 read 실패 → 다음 미러 시도. base_bdev_io_remaining 은
		 * "아직 시도하지 않은 디스크 수"로 사용 (남은 후보 카운터). */
		assert(raid_io->base_bdev_io_remaining > 0);
		raid_io->base_bdev_io_remaining--;
		raid1_read_other_base_bdev(raid_io);
		return;
	}

	/* try to correct the read error by writing data read from the other base bdev */
	/* [한국어] 다른 미러에서 정상 데이터 확보 → 실패 디스크에 write back 시도. */
	raid1_correct_read_error(raid_io);
}

/*
 * [한국어]
 * raid1_read_other_base_bdev - 다른 미러 디스크들 순회하며 read 재시도.
 *
 * @_raid_io: void* 형태 raid_io.
 *
 * base_bdev_io_remaining 은 첫 진입 시 num_base_bdevs로 설정되며, 매 시도마다 감소. 이미 시도
 * 했거나 채널이 없는 디스크는 건너뜀 (i == base_bdev_io_submitted 가 처음 실패 디스크). 모든
 * 디스크에서 실패하면 raid_io 를 FAILED로 종결하고 처음 실패 디스크는 fail 처리.
 *
 * 인덱스 산출: i = num_base_bdevs - base_bdev_io_remaining. 즉 N에서 시작해 매 시도마다 ++.
 * 단조증가하면서 자기 자신과 결손 슬롯을 skip.
 */
static void
raid1_read_other_base_bdev(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct spdk_bdev_ext_io_opts io_opts;
	struct raid_base_bdev_info *base_info;
	struct spdk_io_channel *base_ch;
	uint8_t i;
	int ret;

	/* [한국어] 시작 인덱스 = N - remaining. 첫 호출 시 remaining=N → i=0. 한 번 실패하면
	 * remaining=N-1 → i=1, ... 즉 단조 증가하며 시도. */
	for (i = raid_bdev->num_base_bdevs - raid_io->base_bdev_io_remaining; i < raid_bdev->num_base_bdevs;
	     i++) {
		base_info = &raid_bdev->base_bdev_info[i];
		base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, i);

		if (base_ch == NULL || i == raid_io->base_bdev_io_submitted) {
			/* [한국어] 결손 슬롯이거나 처음 read 실패한 디스크 자신 → 건너뜀. remaining 감소
			 * 후 다음 i로. */
			raid_io->base_bdev_io_remaining--;
			continue;
		}

		/* [한국어] 후보 미러에서 read 시도. */
		raid1_init_ext_io_opts(&io_opts, raid_io);
		ret = raid_bdev_readv_blocks_ext(base_info, base_ch, raid_io->iovs, raid_io->iovcnt,
						 raid_io->offset_blocks, raid_io->num_blocks,
						 raid1_read_other_completion, raid_io, &io_opts);
		if (spdk_unlikely(ret != 0)) {
			if (ret == -ENOMEM) {
				/* [한국어] 자원 부족 → wait 큐 등록 후 다시 본 함수 재호출. */
				raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, raid1_read_other_base_bdev);
			} else {
				/* [한국어] 비ENOMEM 실패 → 루프 탈출하여 fail 처리 경로로. */
				break;
			}
		}
		/* [한국어] 발행 성공 또는 ENOMEM 큐 등록. 콜백이 후속 처리. 본 호출은 여기서 종료. */
		return;
	}

	/* [한국어] 모든 미러에서 실패 → 처음 실패 디스크를 fail 처리하고 raid_io를 FAILED 통지. */
	base_info = raid1_get_read_io_base_bdev(raid_io);
	raid_bdev_fail_base_bdev(base_info);

	raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
}

/*
 * [한국어]
 * raid1_read_bdev_io_completion - 사용자 READ 의 1차 시도 완료 콜백.
 *
 * 성공: outstanding 카운터 감소 후 사용자에게 SUCCESS 통지 (단일 child 모델).
 * 실패: 카운터 감소 후 raid1_read_other_base_bdev 로 다른 미러 시도 (read repair 시작).
 */
static void
raid1_read_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	/* [한국어] read child 반납. */
	spdk_bdev_free_io(bdev_io);

	/* [한국어] LWL 카운터 감소 (발행 시 inc 와 짝). */
	raid1_channel_dec_read_counters(raid_io->raid_ch, raid_io->base_bdev_io_submitted,
					raid_io->num_blocks);

	if (!success) {
		/* [한국어] 1차 시도 실패 → 다른 미러로 retry. base_bdev_io_remaining 은 "남은 후보 수"로
		 * 재사용 (N 으로 초기화). */
		raid_io->base_bdev_io_remaining = raid_io->raid_bdev->num_base_bdevs;
		raid1_read_other_base_bdev(raid_io);
		return;
	}

	/* [한국어] 정상 완료 → 사용자에게 SUCCESS. */
	raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

/* [한국어] 전방 선언. ENOMEM wait 콜백에서 raid1_submit_rw_request 재진입 위해. */
static void raid1_submit_rw_request(struct raid_bdev_io *raid_io);

/*
 * [한국어]
 * _raid1_submit_rw_request - bdev_io_wait 콜백 시그니처 어댑터.
 *
 * READ는 단일 디스크에 발행, WRITE는 base_bdev_io_submitted를 보고 이어서 발행하므로 둘 다
 * 재진입에 안전.
 */
static void
_raid1_submit_rw_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid1_submit_rw_request(raid_io);
}

/*
 * [한국어]
 * raid1_channel_next_read_base_bdev - LWL(Least Work Left) 정책으로 다음 read 디스크 선택.
 *
 * @raid_bdev: RAID bdev.
 * @raid_ch  : RAID I/O 채널.
 * @return   : 선택된 슬롯 인덱스 (0..N-1) 또는 UINT8_MAX (모든 슬롯이 결손).
 *
 * 알고리즘: read_blocks_outstanding[] 의 최소값을 가진 디스크를 선택. 동률이면 작은 인덱스 우선
 * (반복문 < 비교라 같으면 갱신 안 함). 채널이 NULL인 슬롯은 결손이므로 후보에서 제외.
 *
 * 시간복잡도: O(N) per request. N이 작아 (보통 2~3) 비용 무시 가능.
 */
static uint8_t
raid1_channel_next_read_base_bdev(struct raid_bdev *raid_bdev, struct raid_bdev_io_channel *raid_ch)
{
	struct raid1_io_channel *raid1_ch = raid_bdev_channel_get_module_ctx(raid_ch);
	uint64_t read_blocks_min = UINT64_MAX;
	uint8_t idx = UINT8_MAX;
	uint8_t i;

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/* [한국어] 채널 NULL = 결손 슬롯, 건너뜀. 그리고 더 작은 outstanding 인 디스크만 갱신. */
		if (raid_bdev_channel_get_base_channel(raid_ch, i) != NULL &&
		    raid1_ch->read_blocks_outstanding[i] < read_blocks_min) {
			read_blocks_min = raid1_ch->read_blocks_outstanding[i];
			idx = i;
		}
	}

	/* [한국어] UINT8_MAX 면 모든 슬롯 결손 = read 불가. 호출자가 UINT8_MAX 처리. */
	return idx;
}

/*
 * [한국어]
 * raid1_submit_read_request - LWL로 디스크 선택 후 단일 base bdev에 read 발행.
 *
 * @raid_io: READ raid_bdev_io.
 * @return : 0 또는 음수 errno. 발행 성공/실패와 별개로 ENOMEM은 0 반환 (큐 등록되어 자동 재시도).
 *
 * 발행 성공 시 base_bdev_io_submitted = idx 보관 → 실패 시 어느 디스크에서 실패했는지 식별.
 * outstanding 카운터 ++ (read 완료 시 짝지어 --).
 */
static int
raid1_submit_read_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid_bdev_io_channel *raid_ch = raid_io->raid_ch;
	struct spdk_bdev_ext_io_opts io_opts;
	struct raid_base_bdev_info *base_info;
	struct spdk_io_channel *base_ch;
	uint8_t idx;
	int ret;

	/* [한국어] LWL로 가장 한가한 디스크 선택. */
	idx = raid1_channel_next_read_base_bdev(raid_bdev, raid_ch);
	if (spdk_unlikely(idx == UINT8_MAX)) {
		/* [한국어] 모든 슬롯이 결손 → 즉시 FAILED. 0 반환은 "내가 처리 완료, 호출자는 추가 처리
		 * 하지 말라"는 의미. */
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return 0;
	}

	base_info = &raid_bdev->base_bdev_info[idx];
	base_ch = raid_bdev_channel_get_base_channel(raid_ch, idx);

	raid1_init_ext_io_opts(&io_opts, raid_io);
	/* [한국어] 단일 base bdev에 readv 발행. RAID 1는 LBA 변환 없이 그대로 (data_offset만 헬퍼가 보정). */
	ret = raid_bdev_readv_blocks_ext(base_info, base_ch, raid_io->iovs, raid_io->iovcnt,
					 raid_io->offset_blocks, raid_io->num_blocks,
					 raid1_read_bdev_io_completion, raid_io, &io_opts);

	if (spdk_likely(ret == 0)) {
		/* [한국어] 발행 성공: outstanding 증가, 실패 시 디스크 식별을 위해 idx 보관. */
		raid1_channel_inc_read_counters(raid_ch, idx, raid_io->num_blocks);
		raid_io->base_bdev_io_submitted = idx;
	} else if (spdk_unlikely(ret == -ENOMEM)) {
		/* [한국어] 자원 부족 → wait 큐 등록. 자원 회복 시 _raid1_submit_rw_request 재진입.
		 * 0 반환으로 호출자(raid1_submit_rw_request)에게 "성공처럼 처리"하라고 알림. */
		raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid1_submit_rw_request);
		return 0;
	}

	/* [한국어] 비ENOMEM 비0 음수 시 호출자가 ret을 보고 raid_bdev_io_complete(FAILED) 호출. */
	return ret;
}

/*
 * [한국어]
 * raid1_submit_write_request - 모든 base bdev에 미러 fan-out write 발행.
 *
 * @raid_io: WRITE raid_bdev_io.
 * @return : 0 정상, -ENODEV (모든 슬롯 결손).
 *
 * 첫 진입 시 base_bdev_io_remaining = N, default_status = FAILED 설정. 이후 각 child 완료 시
 * raid1_write_bdev_io_completion 가 part complete 호출 → 한 child라도 SUCCESS면 누적 status가
 * SUCCESS로 갱신되어 최종 사용자에게 SUCCESS 통지. (FAILED는 모두 실패한 경우만.)
 *
 * ENOMEM 시 wait 큐 등록 + 0 반환. submitted 인덱스 보존하므로 재진입 시 이어서 발행.
 *
 * 실패한 디스크의 경우 raid1_write_bdev_io_completion 가 raid_bdev_fail_base_bdev로 슬롯을 fail
 * 처리하므로, 다음 WRITE에서 그 슬롯은 자동으로 채널 NULL → skip.
 */
static int
raid1_submit_write_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct spdk_bdev_ext_io_opts io_opts;
	struct raid_base_bdev_info *base_info;
	struct spdk_io_channel *base_ch;
	uint8_t idx;
	uint64_t base_bdev_io_not_submitted;
	int ret = 0;

	if (raid_io->base_bdev_io_submitted == 0) {
		/* [한국어] 첫 진입: 카운터 N, default = FAILED (한 child라도 SUCCESS면 SUCCESS로 덮음). */
		raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs;
		raid_bdev_io_set_default_status(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}

	raid1_init_ext_io_opts(&io_opts, raid_io);
	for (idx = raid_io->base_bdev_io_submitted; idx < raid_bdev->num_base_bdevs; idx++) {
		base_info = &raid_bdev->base_bdev_info[idx];
		base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, idx);

		if (base_ch == NULL) {
			/* skip a missing base bdev's slot */
			/* [한국어] 결손 슬롯: 발행 시도 없이 part complete (FAILED) 보고. 누적 status에는
			 * 영향이 없으나 (이미 default가 FAILED) remaining 감소가 필요함. submitted는 ++. */
			raid_io->base_bdev_io_submitted++;
			raid_bdev_io_complete_part(raid_io, 1, SPDK_BDEV_IO_STATUS_FAILED);
			continue;
		}

		/* [한국어] 미러 발행. 동일 LBA, 동일 데이터, 동일 메타. */
		ret = raid_bdev_writev_blocks_ext(base_info, base_ch, raid_io->iovs, raid_io->iovcnt,
						  raid_io->offset_blocks, raid_io->num_blocks,
						  raid1_write_bdev_io_completion, raid_io, &io_opts);
		if (spdk_unlikely(ret != 0)) {
			if (spdk_unlikely(ret == -ENOMEM)) {
				/* [한국어] 자원 부족 → wait 큐 등록 후 즉시 return. 자원 회복 시 submitted
				 * 부터 이어서 발행. */
				raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid1_submit_rw_request);
				return 0;
			}

			/* [한국어] 비ENOMEM 즉시 실패: 미발행분 만큼 part complete (FAILED) 일괄 보고 후
			 * return. 0 반환으로 호출자에게 추가 처리 안 하도록. */
			base_bdev_io_not_submitted = raid_bdev->num_base_bdevs -
						     raid_io->base_bdev_io_submitted;
			raid_bdev_io_complete_part(raid_io, base_bdev_io_not_submitted,
						   SPDK_BDEV_IO_STATUS_FAILED);
			return 0;
		}

		/* [한국어] 발행 성공: 다음 디스크로. */
		raid_io->base_bdev_io_submitted++;
	}

	if (raid_io->base_bdev_io_submitted == 0) {
		/* [한국어] 모든 슬롯이 결손이고 한 번도 발행되지 않음 → -ENODEV. 호출자가 FAILED 통지. */
		ret = -ENODEV;
	}

	return ret;
}

/*
 * [한국어]
 * raid1_submit_rw_request - READ/WRITE 분기 디스패치.
 *
 * RAID 코어가 sumbit_rw_request 콜백으로 호출. type 에 따라 read/write 분기 후 ret 처리.
 */
static void
raid1_submit_rw_request(struct raid_bdev_io *raid_io)
{
	int ret;

	switch (raid_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		ret = raid1_submit_read_request(raid_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		ret = raid1_submit_write_request(raid_io);
		break;
	default:
		/* [한국어] FLUSH/UNMAP 등은 submit_null_payload_request로 가야 함. */
		ret = -EINVAL;
		break;
	}

	if (spdk_unlikely(ret != 0)) {
		/* [한국어] 비0 반환 (-EINVAL/-ENODEV 등) → FAILED 통지. ENOMEM 경로는 0을 반환하므로 여기에 안 옴. */
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* [한국어] 전방 선언: FLUSH/UNMAP fan-out 함수. */
static void raid1_submit_null_payload_request(struct raid_bdev_io *raid_io);

/*
 * [한국어]
 * _raid1_submit_null_payload_request - bdev_io_wait 콜백 어댑터.
 */
static void
_raid1_submit_null_payload_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid1_submit_null_payload_request(raid_io);
}

/*
 * [한국어]
 * raid1_null_payload_request_io_completion - FLUSH/UNMAP child 완료 콜백.
 *
 * 본질적으로 WRITE 완료 콜백과 동일한 시맨틱 (실패 디스크 fail mark + part complete) 이므로
 * raid1_write_bdev_io_completion 을 그대로 위임.
 */
static inline void
raid1_null_payload_request_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	raid1_write_bdev_io_completion(bdev_io, success, cb_arg);
}

/*
 * [한국어]
 * raid1_submit_null_payload_request - FLUSH/UNMAP을 모든 미러에 fan-out.
 *
 * raid1_submit_write_request 와 거의 동일한 로직. type에 따라 unmap/flush 헬퍼만 분기.
 */
static void
raid1_submit_null_payload_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct spdk_bdev_ext_io_opts io_opts;
	struct raid_base_bdev_info *base_info;
	struct spdk_io_channel *base_ch;
	uint8_t idx;
	uint64_t base_bdev_io_not_submitted;
	int ret = 0;

	if (raid_io->base_bdev_io_submitted == 0) {
		/* [한국어] 첫 진입: N개 child 예정, default FAILED. */
		raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs;
		raid_bdev_io_set_default_status(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}

	raid1_init_ext_io_opts(&io_opts, raid_io);
	for (idx = raid_io->base_bdev_io_submitted; idx < raid_bdev->num_base_bdevs; idx++) {
		base_info = &raid_bdev->base_bdev_info[idx];
		base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, idx);

		if (base_ch == NULL) {
			/* skip a missing base bdev's slot */
			/* [한국어] 결손 슬롯 skip: part complete(FAILED). */
			raid_io->base_bdev_io_submitted++;
			raid_bdev_io_complete_part(raid_io, 1, SPDK_BDEV_IO_STATUS_FAILED);
			continue;
		}

		switch (raid_io->type) {
		case SPDK_BDEV_IO_TYPE_UNMAP:
			/* [한국어] UNMAP 발행 (NVMe Dataset Management 매핑). */
			ret = raid_bdev_unmap_blocks(base_info, base_ch,
						     raid_io->offset_blocks, raid_io->num_blocks,
						     raid1_null_payload_request_io_completion, raid_io);
			break;

		case SPDK_BDEV_IO_TYPE_FLUSH:
			/* [한국어] FLUSH 발행. */
			ret = raid_bdev_flush_blocks(base_info, base_ch,
						     raid_io->offset_blocks, raid_io->num_blocks,
						     raid1_null_payload_request_io_completion, raid_io);
			break;

		default:
			/* [한국어] 잘못된 type. 코어 버그. */
			SPDK_ERRLOG("submit request, invalid io type with null payload %u\n", raid_io->type);
			assert(false);
			ret = -EIO;
		}

		if (spdk_unlikely(ret != 0)) {
			if (spdk_unlikely(ret == -ENOMEM)) {
				/* [한국어] wait 등록 후 return. */
				raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid1_submit_null_payload_request);
				return;
			}

			/* [한국어] 비ENOMEM 실패 → 남은 부분 일괄 FAILED part complete. */
			base_bdev_io_not_submitted = raid_bdev->num_base_bdevs -
						     raid_io->base_bdev_io_submitted;
			raid_bdev_io_complete_part(raid_io, base_bdev_io_not_submitted,
						   SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		raid_io->base_bdev_io_submitted++;
	}

	/* [한국어] 적어도 하나는 발행되었어야 함 (write request와 달리 ENODEV 분기는 없고 단언으로
	 * 안전 검증). RAID 1는 base_bdevs_min=2 이고 모두 결손은 RAID 코어가 차단. */
	assert(raid_io->base_bdev_io_submitted != 0);
}

/*
 * [한국어]
 * raid1_ioch_destroy / raid1_ioch_create - 모듈 io_device 채널 콜백.
 *
 * 모듈은 채널별 read_blocks_outstanding[N] 카운터를 ctx_buf 안에 두며, ctx_buf 는 SPDK가
 * spdk_io_device_register 시 등록한 size 만큼 0으로 잡아 준다 (별도 초기화 불필요). destroy
 * 시에도 별도 정리가 없으므로 둘 다 빈 함수.
 */
static void
raid1_ioch_destroy(void *io_device, void *ctx_buf)
{
	/* [한국어] 별도 자원 없음. ctx_buf 자체는 SPDK가 자동 free. */
}

static int
raid1_ioch_create(void *io_device, void *ctx_buf)
{
	/* [한국어] ctx_buf 는 zero-init 되어 있으므로 초기화 불요. */
	return 0;
}

/*
 * [한국어]
 * raid1_io_device_unregister_done - spdk_io_device_unregister 완료 콜백.
 *
 * 비동기 unregister가 끝난 시점에 RAID 코어에 stop 완료 통지(raid_bdev_module_stop_done) 후
 * r1info 메모리 free.
 */
static void
raid1_io_device_unregister_done(void *io_device)
{
	struct raid1_info *r1info = io_device;

	/* [한국어] 코어에 비동기 stop 완료를 통지 → 코어가 RAID 인스턴스 정리 진행. */
	raid_bdev_module_stop_done(r1info->raid_bdev);

	/* [한국어] 모듈 자체 핸들 free. */
	free(r1info);
}

/*
 * [한국어]
 * raid1_start - RAID 1 인스턴스 시작.
 *
 * 절차:
 *  1) r1info 할당 (모듈 io_device 핸들).
 *  2) base bdev들 중 최소 data_size 산출, 모든 슬롯의 data_size 통일 → blockcnt 설정.
 *  3) spdk_io_device_register 로 모듈 io_device 등록 (채널 ctx_size = sizeof(raid1_io_channel)
 *     + N * sizeof(uint64_t)). 이 크기에 맞춰 read_blocks_outstanding[N] 가 자동 잡힘.
 *  4) raid_bdev->module_private = r1info.
 *
 * RAID 1는 LBA 변환 없이 모든 미러가 동일하므로 strip 정렬 불요 (단, min_blockcnt 만 통일).
 */
static int
raid1_start(struct raid_bdev *raid_bdev)
{
	uint64_t min_blockcnt = UINT64_MAX;
	struct raid_base_bdev_info *base_info;
	struct raid1_info *r1info;
	char name[256];

	r1info = calloc(1, sizeof(*r1info));
	if (!r1info) {
		SPDK_ERRLOG("Failed to allocate RAID1 info device structure\n");
		return -ENOMEM;
	}
	r1info->raid_bdev = raid_bdev;

	/* [한국어] 1차 패스: 모든 base bdev의 data_size 최소값 산출. */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		min_blockcnt = spdk_min(min_blockcnt, base_info->data_size);
	}

	/* [한국어] 2차 패스: 모든 슬롯을 통일된 크기로 (자투리 잘라냄). */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->data_size = min_blockcnt;
	}

	/* [한국어] 가상 RAID 크기 = 단일 미러 크기 (RAID 1는 N배 아님). */
	raid_bdev->bdev.blockcnt = min_blockcnt;
	raid_bdev->module_private = r1info;

	/* [한국어] io_device 등록명 (디버그 용). */
	snprintf(name, sizeof(name), "raid1_%s", raid_bdev->bdev.name);
	/* [한국어] 모듈 io_device 등록. ctx_size 에는 raid1_io_channel 헤더 + N개의 uint64_t 카운터.
	 * SPDK가 채널 생성 시 이 크기만큼 ctx_buf를 zero-alloc 해 raid1_ioch_create 에 전달. */
	spdk_io_device_register(r1info, raid1_ioch_create, raid1_ioch_destroy,
				sizeof(struct raid1_io_channel) + raid_bdev->num_base_bdevs * sizeof(uint64_t),
				name);

	return 0;
}

/*
 * [한국어]
 * raid1_stop - RAID 1 인스턴스 종료.
 *
 * @return: false (비동기 — io_device unregister 완료 후 raid_bdev_module_stop_done 호출).
 *
 * 채널 정리는 SPDK가 알아서 진행. 본 함수는 unregister만 트리거.
 */
static bool
raid1_stop(struct raid_bdev *raid_bdev)
{
	struct raid1_info *r1info = raid_bdev->module_private;

	/* [한국어] 비동기 unregister 트리거. 모든 채널 정리 + 콜백 후 자원 해제. */
	spdk_io_device_unregister(r1info, raid1_io_device_unregister_done);

	/* [한국어] false = 비동기 종료. 코어는 raid_bdev_module_stop_done 호출을 대기. */
	return false;
}

/*
 * [한국어]
 * raid1_get_io_channel - 모듈 io_device 의 채널을 반환.
 *
 * RAID 코어가 RAID 채널을 만들 때 모듈 채널을 임베드하기 위해 호출. spdk_get_io_channel 은
 * 현재 SPDK thread에 매핑된 채널을 반환 (없으면 새로 생성하여 raid1_ioch_create 호출).
 */
static struct spdk_io_channel *
raid1_get_io_channel(struct raid_bdev *raid_bdev)
{
	struct raid1_info *r1info = raid_bdev->module_private;

	return spdk_get_io_channel(r1info);
}

/*
 * [한국어]
 * raid1_process_write_completed - 백그라운드 리빌드 write 완료 콜백.
 *
 * 리빌드 = 새 미러 디스크에 데이터를 한 LBA 윈도우씩 복사. read 한 후 그 데이터를 target에
 * write. 이 콜백은 write 단계 종료 시 호출되며, 결과를 raid_bdev_process_request_complete 로
 * 코어에 통지하면 코어가 다음 윈도우로 진행.
 */
static void
raid1_process_write_completed(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_process_request *process_req = cb_arg;

	spdk_bdev_free_io(bdev_io);

	/* [한국어] success → 0, fail → -EIO. RAID 코어가 누적해 리빌드 진행/실패 결정. */
	raid_bdev_process_request_complete(process_req, success ? 0 : -EIO);
}

/* [한국어] 전방 선언. ENOMEM wait 콜백에서 호출. */
static void raid1_process_submit_write(struct raid_bdev_process_request *process_req);

/*
 * [한국어]
 * _raid1_process_submit_write - bdev_io_wait 콜백 어댑터.
 */
static void
_raid1_process_submit_write(void *ctx)
{
	struct raid_bdev_process_request *process_req = ctx;

	raid1_process_submit_write(process_req);
}

/*
 * [한국어]
 * raid1_process_submit_write - 리빌드 write 단계 발행.
 *
 * 이미 read가 완료되어 raid_io->iovs 에 데이터가 있는 상태. target(=새 미러)에 write 발행.
 */
static void
raid1_process_submit_write(struct raid_bdev_process_request *process_req)
{
	struct raid_bdev_io *raid_io = &process_req->raid_io;
	struct spdk_bdev_ext_io_opts io_opts;
	int ret;

	raid1_init_ext_io_opts(&io_opts, raid_io);
	/* [한국어] target 슬롯에 write. process_req->target/target_ch 가 새 미러 디스크와 그 채널. */
	ret = raid_bdev_writev_blocks_ext(process_req->target, process_req->target_ch,
					  raid_io->iovs, raid_io->iovcnt,
					  raid_io->offset_blocks, raid_io->num_blocks,
					  raid1_process_write_completed, process_req, &io_opts);
	if (spdk_unlikely(ret != 0)) {
		if (ret == -ENOMEM) {
			/* [한국어] 자원 부족 → wait 등록. raid_io->waitq_entry 사용 (process_req 안의
			 * raid_io 임베딩). */
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(process_req->target->desc),
						process_req->target_ch, _raid1_process_submit_write);
		} else {
			/* [한국어] 즉시 실패 → 코어에 errno 보고. */
			raid_bdev_process_request_complete(process_req, ret);
		}
	}
}

/*
 * [한국어]
 * raid1_process_read_completed - 리빌드 read 단계 완료 콜백.
 *
 * 사용자 read 완료 콜백과 시그니처가 다름: raid_io->completion_cb 가 호출되며 인자는
 * (raid_io, status). 즉 raid_bdev_io_complete 가 spdk_bdev_io_complete 대신 본 콜백을 부르도록
 * raid_io->completion_cb를 설정해 두었기 때문 (raid1_submit_process_request에서). 이로써 process
 * I/O는 사용자에게 노출되지 않고 코어 안에서만 흐른다.
 *
 * 성공: write 단계 발행. 실패: 코어에 EIO 통지.
 */
static void
raid1_process_read_completed(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	/* [한국어] 임베딩된 raid_io → 부모 process_req 복원. SPDK_CONTAINEROF 매크로. */
	struct raid_bdev_process_request *process_req = SPDK_CONTAINEROF(raid_io,
			struct raid_bdev_process_request, raid_io);

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		/* [한국어] read 실패 → 리빌드 한 윈도우 실패. 코어가 정책에 따라 재시도/중단 결정. */
		raid_bdev_process_request_complete(process_req, -EIO);
		return;
	}

	/* [한국어] read 성공 → write 발행. */
	raid1_process_submit_write(process_req);
}

/*
 * [한국어]
 * raid1_submit_process_request - 리빌드 한 윈도우 발행 진입점.
 *
 * @process_req: 코어가 만든 리빌드 요청 (target, offset, num, iov 등 채워진 상태).
 * @raid_ch    : 리빌드용 RAID 채널 (보통 백그라운드 전용).
 * @return     : 발행한 블록 수 (양수) 또는 음수 errno.
 *
 * raid1_submit_read_request 를 그대로 재사용해 read를 발행. completion_cb 를 process 전용으로
 * 설정해 사용자 경로(spdk_bdev_io_complete)를 우회.
 */
static int
raid1_submit_process_request(struct raid_bdev_process_request *process_req,
			     struct raid_bdev_io_channel *raid_ch)
{
	struct raid_bdev_io *raid_io = &process_req->raid_io;
	int ret;

	/* [한국어] process_req 안의 raid_io 를 READ 형태로 초기화. type=READ, offset/num/iov/md_buf
	 * 는 process_req 가 미리 채워둔 값을 사용. */
	raid_bdev_io_init(raid_io, raid_ch, SPDK_BDEV_IO_TYPE_READ,
			  process_req->offset_blocks, process_req->num_blocks,
			  &process_req->iov, 1, process_req->md_buf, NULL, NULL);
	/* [한국어] 사용자 완료 경로 우회: raid_bdev_io_complete 시 spdk_bdev_io_complete 대신 본 콜백. */
	raid_io->completion_cb = raid1_process_read_completed;

	/* [한국어] 일반 read 발행 함수 그대로 사용 (LWL로 가장 한가한 미러에서 읽기). */
	ret = raid1_submit_read_request(raid_io);
	if (spdk_likely(ret == 0)) {
		/* [한국어] 발행 성공 (또는 ENOMEM 큐 등록): 진행한 블록 수 반환 → 코어가 다음 윈도우 진행. */
		return process_req->num_blocks;
	} else if (ret < 0) {
		/* [한국어] 음수 errno 그대로 반환. */
		return ret;
	} else {
		return -EINVAL;
	}
}

/*
 * [한국어]
 * raid1_resize - base bdev 크기 변경 시 RAID 가상 크기 갱신.
 *
 * 모든 슬롯의 (blockcnt - data_offset) 중 최소값을 새 RAID blockcnt로 설정. 결손 슬롯은 skip.
 * 변경이 있으면 spdk_bdev_notify_blockcnt_change 통지.
 */
static bool
raid1_resize(struct raid_bdev *raid_bdev)
{
	int rc;
	uint64_t min_blockcnt = UINT64_MAX;
	struct raid_base_bdev_info *base_info;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		struct spdk_bdev *base_bdev;

		/* [한국어] 결손 슬롯은 크기 산정에 영향 없음. */
		if (base_info->desc == NULL) {
			continue;
		}
		base_bdev = spdk_bdev_desc_get_bdev(base_info->desc);
		min_blockcnt = spdk_min(min_blockcnt, base_bdev->blockcnt - base_info->data_offset);
	}

	if (min_blockcnt == raid_bdev->bdev.blockcnt) {
		/* [한국어] 변경 없음. */
		return false;
	}

	/* [한국어] lib/bdev에 새 blockcnt 통지. */
	rc = spdk_bdev_notify_blockcnt_change(&raid_bdev->bdev, min_blockcnt);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to notify blockcount change\n");
		return false;
	}

	/* [한국어] 통지 성공 후 모든 슬롯 data_size 갱신. */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->data_size = min_blockcnt;
	}
	return true;
}

/*
 * [한국어]
 * g_raid1_module - RAID 1 모듈 디스크립터.
 *
 * RAID_MODULE_REGISTER로 코어에 자동 등록. 사용자가 RPC bdev_raid_create raid_level=raid1로
 * 만들 때 본 모듈이 선택됨.
 */
static struct raid_bdev_module g_raid1_module = {
	/* [한국어] RAID 레벨 식별자. */
	.level = RAID1,
	/* [한국어] 최소 미러 디스크 수 = 2. (1개 미러는 RAID 0 단일 디스크와 동일하므로 의미 없음). */
	.base_bdevs_min = 2,
	/* [한국어] 최소 동작 슬롯 = 1 (한 디스크만 살아 있어도 RAID는 동작). N-1개까지 결손 허용. */
	.base_bdevs_constraint = {CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL, 1},
	/* [한국어] memory_domain 패스스루 지원 (모든 미러에 동일 데이터 그대로 발행). */
	.memory_domains_supported = true,
	/* [한국어] 시작 콜백: r1info 할당, blockcnt 설정, io_device 등록. */
	.start = raid1_start,
	/* [한국어] 종료 콜백: io_device unregister (비동기). */
	.stop = raid1_stop,
	/* [한국어] R/W 디스패치. */
	.submit_rw_request = raid1_submit_rw_request,
	/* [한국어] FLUSH/UNMAP 발행. */
	.submit_null_payload_request = raid1_submit_null_payload_request,
	/* [한국어] 모듈 채널 발급 (read 분산 카운터 포함). */
	.get_io_channel = raid1_get_io_channel,
	/* [한국어] 백그라운드 리빌드 발행. */
	.submit_process_request = raid1_submit_process_request,
	/* [한국어] base bdev 크기 변경 시 갱신. */
	.resize = raid1_resize,
};
/* [한국어] g_raid1_module 자동 등록. */
RAID_MODULE_REGISTER(&g_raid1_module)

/* [한국어] "bdev_raid1" 디버그 컴포넌트 등록. */
SPDK_LOG_REGISTER_COMPONENT(bdev_raid1)
