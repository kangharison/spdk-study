/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SCSI task 객체의 라이프사이클·데이터 버퍼·sense 데이터 빌더 (task.c)
 *
 * === 파일의 역할 ===
 * spdk_scsi_task는 SCSI Command(CDB) 한 개의 실행 단위를 표현하는 객체이며, 본 파일은:
 *   1) task의 reference counting / 해제(spdk_scsi_task_put).
 *   2) task 초기화(spdk_scsi_task_construct) — 완료/해제 콜백 설정.
 *   3) task 데이터 버퍼(iovec) 할당/해제, scatter(쓰기 데이터 채우기) / gather(읽기 데이터 모으기).
 *   4) SCSI sense data 빌드(spdk_scsi_task_build_sense_data) — CHECK CONDITION 응답 시 호출자의 표준 sense 포맷.
 *   5) task 상태 설정 helper(spdk_scsi_task_set_status / copy_status / process_null_lun / process_abort).
 * 본 파일은 SCSI 명령 디스패처가 아니다. 명령 처리는 lun.c → bdev_scsi_execute에서, PR 검사는 scsi_pr.c에서.
 *
 * === 전체 아키텍처에서의 위치 ===
 * iSCSI/vhost-scsi 같은 프론트엔드는 자체 task 풀에서 spdk_scsi_task를 임베드한 큰 구조체를 받아오고,
 * spdk_scsi_task_construct로 cpl_fn/free_fn을 등록한다. 명령 처리 후에는 spdk_scsi_task_put이
 * ref==0 시 free_fn 호출 → 프론트엔드 풀로 반환되거나 free된다.
 * SCSI 데이터 전송은 임베디드 iov(task->iov) 또는 외부 iovec 배열로 표현되며 본 파일이 변환을 돕는다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: scsi_internal.h(spdk_scsi_task 구조체), spdk/endian.h(from_be16),
 *         spdk/env.h(spdk_dma_zmalloc/free — DPDK hugepage DMA 메모리),
 *         spdk/util.h(spdk_round_up, spdk_min).
 * - 사용처: lun.c가 task 완료 시 cpl_fn 호출 → 프론트엔드 → spdk_scsi_task_put.
 *           PR 코드(scsi_pr.c)와 LUN 실행 로직(lun.c)이 build_sense_data/set_status를 호출.
 * - DMA: spdk_dma_zmalloc은 DPDK hugepage 영역에서 핀된 DMA-가능 메모리를 받는다 — bdev DMA 경로용.
 *
 * === 주요 함수/구조체 요약 ===
 * - scsi_task_free_data        : DMA 버퍼 해제, iov 리셋(static).
 * - spdk_scsi_task_put         : ref-- 후 0이면 bdev_io free → data free → free_fn.
 * - spdk_scsi_task_construct   : cpl/free 콜백 등록 + ref++ + iovs/iovcnt 초기.
 * - scsi_task_alloc_data       : 4바이트 정렬로 DMA 버퍼 할당(iSCSI 정렬 요구).
 * - spdk_scsi_task_scatter_data: 외부 src를 task의 iovec들로 분산 복사(쓰기 경로).
 * - spdk_scsi_task_gather_data : task iovec들을 단일 contig 버퍼로 모음(읽기 경로 헬퍼).
 * - spdk_scsi_task_set_data    : 외부 버퍼를 task에 부착(할당 없이).
 * - spdk_scsi_task_build_sense_data: SPC 표준 fixed-format sense data(0x70) 빌드.
 * - spdk_scsi_task_set_status  : status + sense를 한 번에 설정.
 * - spdk_scsi_task_copy_status : 다른 task의 status/sense를 복사.
 * - spdk_scsi_task_process_null_lun : 미존재 LUN 응답(INQUIRY는 PQ=3, 그 외는 ILLEGAL).
 * - spdk_scsi_task_process_abort    : ABORTED COMMAND sense로 종료.
 */

#include "scsi_internal.h"
/* [한국어] spdk_scsi_task 구조체 및 SPDK_SCSI_* 매크로 사용. */
#include "spdk/endian.h"
/* [한국어] from_be16() — INQUIRY allocation length 빅엔디안 디코드용. */
#include "spdk/env.h"
/* [한국어] spdk_dma_zmalloc/spdk_dma_free — DPDK hugepage 기반 DMA 메모리 할당. */
#include "spdk/util.h"
/* [한국어] spdk_round_up, spdk_min 매크로. */

/*
 * [한국어]
 * scsi_task_free_data - task가 동적으로 할당했던 DMA 데이터 버퍼를 해제
 *
 * @task: 대상 task. task->alloc_len > 0이면 spdk_dma_zmalloc된 버퍼가 있다는 뜻.
 *
 * scsi_task_alloc_data로 task가 직접 할당한 임베디드 iov 버퍼만 해제 대상이다.
 * 외부에서 spdk_scsi_task_set_data로 부착된 버퍼는 alloc_len==0이라 해제되지 않는다(소유자 책임).
 * 실행 컨텍스트: spdk_scsi_task_put의 마지막 단계 → task가 속한 SPDK 스레드.
 *
 * 호출 체인:
 *   spdk_scsi_task_put (ref==0) → [scsi_task_free_data] → spdk_dma_free
 */
static void
scsi_task_free_data(struct spdk_scsi_task *task)
{
	if (task->alloc_len != 0) {
		/* [한국어] alloc_len이 0이 아니면 본 파일이 직접 할당한 DMA 버퍼가 있음 → 해제 필요. */
		spdk_dma_free(task->iov.iov_base);
		/* [한국어] DPDK hugepage DMA 영역에서 받은 메모리 반환. */
		task->alloc_len = 0;
		/* [한국어] 다음 사용 시 재할당 가능 표시. */
	}

	task->iov.iov_base = NULL;
	/* [한국어] 임베디드 iov 리셋 — 외부 부착 버퍼였더라도 dangling 방지로 NULL화. */
	task->iov.iov_len = 0;
	/* [한국어] 길이도 0으로 초기화. */
}

/*
 * [한국어]
 * spdk_scsi_task_put - SCSI task의 reference count를 감소, 0이 되면 자원 해제
 *
 * @task: 대상 task. NULL이면 무시. ref가 1이면 본 호출이 마지막 ref drop이 된다.
 *
 * SPDK SCSI는 task를 ref-counted로 관리하여 ULP(iSCSI 등)와 백엔드(bdev) 간 비동기 완료 시점에
 * 안전한 해제를 보장한다. ref==0 도달 시 다음 순서로 정리한다:
 *   1) task->bdev_io가 있으면 spdk_bdev_free_io로 bdev I/O 객체 반환.
 *   2) scsi_task_free_data로 본 파일이 할당한 DMA 버퍼 해제.
 *   3) task->free_fn(task) 호출 — 보통 프론트엔드 풀(예: iSCSI PDU)로 반환.
 *
 * 실행 컨텍스트: task를 처리한 SPDK 스레드.
 * 호출자: lun.c의 cpl 경로, 프론트엔드의 명령 종료 경로, 에러 경로 등.
 *
 * 호출 체인:
 *   상위(완료 처리) → [spdk_scsi_task_put] → spdk_bdev_free_io / scsi_task_free_data / free_fn
 */
void
spdk_scsi_task_put(struct spdk_scsi_task *task)
{
	if (!task) {
		/* [한국어] NULL 안전성 — 부분 초기화 경로에서 정리 호출 시 안전. */
		return;
	}

	assert(task->ref > 0);
	/* [한국어] 0인 상태에서 put은 잘못된 라이프사이클 — 디버그 빌드에서 즉시 인지. */
	task->ref--;
	/* [한국어] 단일 스레드에서만 다뤄지므로 atomic 불필요. */

	if (task->ref == 0) {
		/* [한국어] 마지막 참조 drop — 전체 정리 진입. */
		struct spdk_bdev_io *bdev_io = task->bdev_io;
		/* [한국어] bdev I/O를 사용했으면 그 객체를 반환해야 함(없을 수도 있음). */

		if (bdev_io) {
			spdk_bdev_free_io(bdev_io);
			/* [한국어] bdev 모듈 mempool로 bdev_io 반환. */
		}

		scsi_task_free_data(task);
		/* [한국어] 임베디드 DMA 버퍼 해제. */

		task->free_fn(task);
		/* [한국어] 프론트엔드(예: iSCSI)에 task 메모리 반환. construct 시 등록된 콜백. */
	}
}

/*
 * [한국어]
 * spdk_scsi_task_construct - task를 사용 가능 상태로 초기화
 *
 * @task:    제로필이거나 부분 초기화된 task. 호출자가 메모리를 소유.
 * @cpl_fn:  완료 콜백 — lun.c의 scsi_lun_complete_task가 호출.
 * @free_fn: 해제 콜백 — spdk_scsi_task_put에서 ref==0일 때 호출.
 *
 * 콜백 등록 + ref=1로 만든 후 iov/iovs/iovcnt를 임베디드 iov 한 개를 가리키도록 설정한다.
 * 호출 시점에는 외부 iovec이 아직 부착되지 않았다는 가정(iov_base==NULL).
 * 실행 컨텍스트: task를 만들어내는 프론트엔드 스레드(보통 명령 수신 직후).
 *
 * 호출 체인:
 *   iSCSI/vhost-scsi 명령 수신 → [spdk_scsi_task_construct]
 */
void
spdk_scsi_task_construct(struct spdk_scsi_task *task,
			 spdk_scsi_task_cpl cpl_fn,
			 spdk_scsi_task_free free_fn)
{
	assert(task != NULL);
	/* [한국어] 호출자 책임 — NULL이면 즉시 abort(디버그). */
	assert(cpl_fn != NULL);
	/* [한국어] 완료 콜백은 필수 — 없으면 응답을 돌려줄 수 없음. */
	assert(free_fn != NULL);
	/* [한국어] 해제 콜백 필수 — task 메모리 회수 경로. */

	task->cpl_fn = cpl_fn;
	/* [한국어] 명령 완료 시 task->cpl_fn(task) 형태로 호출됨. */
	task->free_fn = free_fn;
	/* [한국어] put에서 ref==0일 때 task->free_fn(task)로 호출됨. */

	task->ref++;
	/* [한국어] 초기 참조 카운트 — 호출자가 명시적으로 put을 한 번 호출해야 0이 됨. */

	/*
	 * Pre-fill the iov_buffers to point to the embedded iov
	 */
	assert(task->iov.iov_base == NULL);
	/* [한국어] 임베디드 iov가 비어 있어야 정상 — 이전 사용 잔여 데이터가 있으면 버그. */
	task->iovs = &task->iov;
	/* [한국어] 외부 iovec 배열을 별도로 부착하지 않을 경우 임베디드 iov를 가리키도록 미리 세팅. */
	task->iovcnt = 1;
	/* [한국어] 임베디드 iov는 1개 — 단순 명령(INQUIRY 등)에 충분. */
}

/*
 * [한국어]
 * scsi_task_alloc_data - task의 임베디드 iov에 DMA 가능한 데이터 버퍼를 할당
 *
 * @task:      대상 task. alloc_len이 0이어야 함(이중 할당 금지).
 * @alloc_len: 호출자가 요청한 논리 길이.
 * @return:    할당된 버퍼의 시작 포인터(iov_base와 같음).
 *
 * iSCSI 등 일부 ULP는 4바이트 정렬을 요구하므로 실제 할당은 4바이트 라운드업.
 * spdk_dma_zmalloc은 DPDK hugepage 영역에서 핀된 DMA 메모리를 가져온다 — 이 메모리는
 * 그대로 bdev DMA 경로(NVMe PRP/SGL)에 사용 가능.
 * iov_len에는 alloc_len을 그대로 기록해 ULP가 보는 논리 길이를 유지한다.
 *
 * 호출 체인:
 *   spdk_scsi_task_scatter_data (iov 비어있을 때) → [scsi_task_alloc_data]
 */
static void *
scsi_task_alloc_data(struct spdk_scsi_task *task, uint32_t alloc_len)
{
	uint32_t zmalloc_len;
	/* [한국어] 4바이트 라운드업된 실제 할당 길이. */

	assert(task->alloc_len == 0);
	/* [한국어] 이미 할당된 버퍼가 있으면 누수 — 디버그 빌드에서 잡는다. */

	/* Some ULPs (such as iSCSI) need to round len up to nearest
	 * 4 bytes. We can help those ULPs by allocating memory here
	 * up to next 4 byte boundary, so they don't have to worry
	 * about handling out-of-bounds errors.
	 */
	zmalloc_len = spdk_round_up(alloc_len, 4);
	/* [한국어] 4바이트 정렬 — iSCSI Data-Out / Data-In 처리 편의. */
	task->iov.iov_base = spdk_dma_zmalloc(zmalloc_len, 0, NULL);
	/* [한국어] DPDK hugepage 기반 zero-init DMA 메모리. align=0(자동), phys_addr 출력 안받음(NULL). */
	task->iov.iov_len = alloc_len;
	/* [한국어] iov_len은 ULP가 인식하는 논리 길이만 기록(라운드업 추가분 노출 X). */
	task->alloc_len = alloc_len;
	/* [한국어] 추후 free_data에서 "본 파일이 할당했음"을 식별할 플래그 겸 길이 기록. */

	return task->iov.iov_base;
	/* [한국어] 호출자는 보통 이 포인터를 통해 직접 데이터를 채워 넣을 수 있음. */
}

/*
 * [한국어]
 * spdk_scsi_task_scatter_data - 단일 src 버퍼를 task의 iovec 들에 분산 복사
 *
 * @task:    대상 task. iov 미할당 상태(iovcnt==1 && iov_base==NULL)면 자동 할당한다.
 * @src:     읽어 올 원본 버퍼.
 * @buf_len: 복사할 바이트 수.
 * @return:  buf_len(성공) 또는 -1(iov 총 길이가 buf_len보다 작아 못 담을 때 — sense 설정됨).
 *
 * iSCSI 등에서 Data-Out PDU로 받은 데이터를 task의 데이터 영역에 분산 저장할 때 사용.
 * 다중 iovec이 등록되어 있으면 순서대로 채우며, 각 iov의 iov_len을 초과하지 않도록 spdk_min 사용.
 * 길이 부족 시 SPDK_SCSI_SENSE_ILLEGAL_REQUEST + INVALID_FIELD_IN_CDB로 sense를 빌드해 응답에 반영.
 *
 * 호출 체인:
 *   iSCSI 데이터 입력 처리 → [spdk_scsi_task_scatter_data] → memcpy
 */
int
spdk_scsi_task_scatter_data(struct spdk_scsi_task *task, const void *src, size_t buf_len)
{
	size_t len = 0;
	/* [한국어] task iov들의 누적 길이 — 수용 가능 여부 판단에 사용. */
	size_t buf_left = buf_len;
	/* [한국어] 아직 복사하지 못한 src 바이트 수. */
	int i;
	/* [한국어] iov 인덱스. */
	struct iovec *iovs = task->iovs;
	/* [한국어] task가 가리키는 iovec 배열(임베디드 또는 외부) 시작점. */
	const uint8_t *pos;
	/* [한국어] src 내 현재 읽기 위치 포인터. */

	if (buf_len == 0) {
		/* [한국어] 빈 데이터 — 아무 일도 하지 않음(에러도 아님). */
		return 0;
	}

	if (task->iovcnt == 1 && iovs[0].iov_base == NULL) {
		/* [한국어] 임베디드 iov 한 개만 등록됐고 아직 버퍼가 없으면 본 파일에서 자동 할당. */
		scsi_task_alloc_data(task, buf_len);
		iovs[0] = task->iov;
		/* [한국어] iovs 포인터가 임베디드 iov 자체를 이미 가리키지만,
		 *          외부 배열 형태에 대비해 할당 결과를 다시 동기화. */
	}

	for (i = 0; i < task->iovcnt; i++) {
		assert(iovs[i].iov_base != NULL);
		/* [한국어] 모든 iov는 유효 버퍼를 가져야 함 — 자동 할당 후에도 보장. */
		len += iovs[i].iov_len;
		/* [한국어] 총 수용량 누적. */
	}

	if (len < buf_len) {
		/* [한국어] task가 받을 수 있는 양보다 src가 더 큼 — CDB invalid 처리. */
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -1;
	}

	pos = src;
	/* [한국어] src 포인터 시작점. */

	for (i = 0; i < task->iovcnt; i++) {
		/* [한국어] 각 iov마다 가능한 만큼 복사. */
		len = spdk_min(iovs[i].iov_len, buf_left);
		/* [한국어] 이번 iov에 들어갈 양 = min(iov 용량, 남은 src). */
		buf_left -= len;
		/* [한국어] 남은 src 바이트 수 갱신. */
		memcpy(iovs[i].iov_base, pos, len);
		/* [한국어] 실제 데이터 복사. */
		pos += len;
		/* [한국어] src 읽기 위치 진행. */
	}

	return buf_len;
	/* [한국어] 성공 시 복사한 바이트 수 반환. */
}

/*
 * [한국어]
 * spdk_scsi_task_gather_data - task의 iovec 데이터들을 단일 contig 버퍼로 모음
 *
 * @task: 대상 task.
 * @len:  [out] 모은 총 바이트 수(>=0) 또는 -1(메모리 부족).
 * @return: malloc된 contig 버퍼(호출자가 free 책임). 없거나 실패 시 NULL.
 *
 * iSCSI 등 단일 contig 버퍼를 요구하는 ULP에서 task 데이터(scatter된 iov)를 합쳐서 반환할 때 사용.
 * 본 함수가 반환하는 버퍼는 일반 calloc 메모리 — DMA 용도가 아니므로 spdk_dma_free가 아닌 free 사용.
 *
 * 호출 체인:
 *   iSCSI 데이터 출력 처리 → [spdk_scsi_task_gather_data] → memcpy
 */
void *
spdk_scsi_task_gather_data(struct spdk_scsi_task *task, int *len)
{
	int i;
	/* [한국어] iov 순회 인덱스. */
	struct iovec *iovs = task->iovs;
	/* [한국어] task의 iovec 배열. */
	size_t buf_len = 0;
	/* [한국어] 모든 iov 길이 합산. */
	uint8_t *buf, *pos;
	/* [한국어] buf: 결과 버퍼; pos: 쓰기 위치. */

	for (i = 0; i < task->iovcnt; i++) {
		/* It is OK for iov_base to be NULL if iov_len is 0. */
		assert(iovs[i].iov_base != NULL || iovs[i].iov_len == 0);
		/* [한국어] 길이가 0이면 base가 NULL인 placeholder iov도 허용. */
		buf_len += iovs[i].iov_len;
		/* [한국어] 총 길이 누적. */
	}

	if (buf_len == 0) {
		/* [한국어] 데이터 없음 — len=0으로 알리고 NULL 반환. */
		*len = 0;
		return NULL;
	}

	buf = calloc(1, buf_len);
	/* [한국어] DMA가 필요 없는 일반 메모리(호출자가 wire로 보낼 임시 버퍼). */
	if (buf == NULL) {
		/* [한국어] 메모리 부족 — len=-1로 에러 표시. */
		*len = -1;
		return NULL;
	}

	pos = buf;
	/* [한국어] 결과 버퍼의 쓰기 시작점. */
	for (i = 0; i < task->iovcnt; i++) {
		memcpy(pos, iovs[i].iov_base, iovs[i].iov_len);
		/* [한국어] 각 iov의 데이터를 순서대로 이어붙임. */
		pos += iovs[i].iov_len;
		/* [한국어] 다음 iov 데이터가 들어갈 오프셋으로 이동. */
	}

	*len = buf_len;
	/* [한국어] 호출자에게 총 길이 통지. */
	return buf;
	/* [한국어] 호출자가 free()로 해제. */
}

/*
 * [한국어]
 * spdk_scsi_task_set_data - 외부 버퍼를 task에 부착(할당 없이)
 *
 * @task: 대상 task. 임베디드 iov 한 개만 등록된 상태(iovcnt==1, alloc_len==0)여야 함.
 * @data: 외부 버퍼 포인터. 호출자가 라이프사이클 관리 책임.
 * @len:  버퍼 길이.
 *
 * INQUIRY/REPORT LUNS 등 짧은 응답을 호출자가 stack/구조체 멤버에 미리 만들어 둔 경우,
 * 본 함수로 그 버퍼를 task에 부착해 별도 할당 없이 응답에 사용한다.
 * alloc_len==0을 유지하므로 spdk_scsi_task_put 시 free되지 않는다.
 *
 * 호출 체인:
 *   bdev_scsi_execute (INQUIRY/REPORT LUNS) → [spdk_scsi_task_set_data]
 */
void
spdk_scsi_task_set_data(struct spdk_scsi_task *task, void *data, uint32_t len)
{
	assert(task->iovcnt == 1);
	/* [한국어] 단일 iov 가정 — 다중 iov 등록 후 set_data는 의미가 모호함. */
	assert(task->alloc_len == 0);
	/* [한국어] 본 파일이 할당한 버퍼가 있다면 set_data로 덮어쓰면 누수 — 디버그 빌드에서 잡음. */

	task->iovs[0].iov_base = data;
	/* [한국어] 외부 버퍼를 그대로 부착(소유권 이전 없음). */
	task->iovs[0].iov_len = len;
	/* [한국어] 길이만 task에 반영. */
}

/*
 * [한국어]
 * spdk_scsi_task_build_sense_data - SCSI sense data(fixed format, response code 0x70)를 빌드
 *
 * @task: 대상 task. task->sense_data 버퍼에 18바이트의 fixed format sense를 직접 기록.
 * @sk:   Sense Key (SPC-4 4.5.6, 4비트). 예: 0x05 ILLEGAL_REQUEST, 0x06 UNIT_ATTENTION 등.
 * @asc:  Additional Sense Code (1바이트).
 * @ascq: Additional Sense Code Qualifier (1바이트).
 *
 * SPC-4 4.5.3 Sense data formats — "Current errors, Fixed format" (response code 0x70)을 채운다.
 * VALID 비트(상위 1비트)를 1로 세팅(INFORMATION 필드 미사용이지만 관습적으로 0x80 OR).
 * 본 함수만으로는 sense를 응답에 포함시키지 않으며, 호출자가 task->status를 CHECK CONDITION으로
 * 설정해야 ULP 응답 단계에서 sense_data가 ULP에 의해 송신된다.
 *
 * 호출 체인:
 *   spdk_scsi_task_set_status → [spdk_scsi_task_build_sense_data]
 *   또는 직접: PR 코드, LUN reset 등 → [spdk_scsi_task_build_sense_data]
 */
void
spdk_scsi_task_build_sense_data(struct spdk_scsi_task *task, int sk, int asc, int ascq)
{
	uint8_t *cp;
	/* [한국어] sense_data 버퍼 시작 포인터(작업 편의용). */
	int resp_code;
	/* [한국어] response code(0x70 = current+fixed). */

	resp_code = 0x70; /* Current + Fixed format */
	/* [한국어] SPC-4 Table 53: 0x70 = current errors, fixed format. (deferred=0x71, descriptor=0x72/0x73 별도) */

	/* Sense Data */
	cp = task->sense_data;
	/* [한국어] task 임베디드 sense 버퍼(SPDK_SCSI_SENSE_BUF_SIZE 이상)에 직접 작성. */

	/* VALID(7) RESPONSE CODE(6-0) */
	cp[0] = 0x80 | resp_code;
	/* [한국어] VALID=1 (INFORMATION 필드는 비록 0이지만 표준 관습) + RESP=0x70 → 0xF0. */
	/* Obsolete */
	cp[1] = 0;
	/* [한국어] SPC-4에서 Obsolete된 segment number 필드 — 0 채움. */
	/* FILEMARK(7) EOM(6) ILI(5) SENSE KEY(3-0) */
	cp[2] = sk & 0xf;
	/* [한국어] FILEMARK/EOM/ILI=0; 하위 4비트에 sense key. (FILEMARK 등은 sequential 디바이스에서 사용) */
	/* INFORMATION */
	memset(&cp[3], 0, 4);
	/* [한국어] INFORMATION 필드(LBA 등) — 본 구현에서는 미사용으로 0. */

	/* ADDITIONAL SENSE LENGTH */
	cp[7] = 10;
	/* [한국어] cp[8]부터 cp[17]까지 10바이트 추가 sense 영역의 길이. */

	/* COMMAND-SPECIFIC INFORMATION */
	memset(&cp[8], 0, 4);
	/* [한국어] command-specific information 4바이트 — 본 구현 미사용. */
	/* ADDITIONAL SENSE CODE */
	cp[12] = asc;
	/* [한국어] ASC 1바이트 — 호출자가 전달한 코드. (예: 0x24 = INVALID_FIELD_IN_CDB) */
	/* ADDITIONAL SENSE CODE QUALIFIER */
	cp[13] = ascq;
	/* [한국어] ASCQ 1바이트 — ASC를 보조하는 한정자. */
	/* FIELD REPLACEABLE UNIT CODE */
	cp[14] = 0;
	/* [한국어] FRU 코드 — 본 구현에서는 항상 0(미사용). */

	/* SKSV(7) SENSE KEY SPECIFIC(6-0,7-0,7-0) */
	cp[15] = 0;
	/* [한국어] SKSV(Sense Key Specific Valid)=0 → 다음 2바이트도 무의미. */
	cp[16] = 0;
	/* [한국어] sense key specific 1/3 — 0. */
	cp[17] = 0;
	/* [한국어] sense key specific 2/3 — 0. */

	/* SenseLength */
	task->sense_data_len = 18;
	/* [한국어] 총 18바이트(헤더 8 + 추가 10)를 ULP에게 알린다. */
}

/*
 * [한국어]
 * spdk_scsi_task_set_status - SCSI status code + sense data를 한 번에 설정
 *
 * @task: 대상 task.
 * @sc:   SCSI status (예: GOOD, CHECK CONDITION, RESERVATION CONFLICT).
 * @sk:   sense key (sc==CHECK CONDITION인 경우만 의미). 그 외에는 무시.
 * @asc:  ASC.
 * @ascq: ASCQ.
 *
 * sc가 CHECK CONDITION일 때만 sense data를 빌드한다(다른 status는 sense 무관).
 * SCSI 명령 처리 코드에서 가장 많이 호출되는 헬퍼.
 *
 * 호출 체인:
 *   bdev_scsi_execute / scsi_pr_* / lun.c 등 → [spdk_scsi_task_set_status]
 */
void
spdk_scsi_task_set_status(struct spdk_scsi_task *task, int sc, int sk,
			  int asc, int ascq)
{
	if (sc == SPDK_SCSI_STATUS_CHECK_CONDITION) {
		/* [한국어] CHECK CONDITION일 때만 sense data가 의미를 가짐. */
		spdk_scsi_task_build_sense_data(task, sk, asc, ascq);
	}
	task->status = sc;
	/* [한국어] ULP가 응답 송신 시 보는 1바이트 SCSI status. */
}

/*
 * [한국어]
 * spdk_scsi_task_copy_status - 다른 task의 status/sense를 복사
 *
 * @dst: 복사 대상 task.
 * @src: 복사 원본 task.
 *
 * 명령 처리 중 새 task를 만들거나 부모/자식 task로 분할한 경우, 자식 task의 결과를
 * 부모 task로 옮길 때 사용. sense_data와 sense_data_len, status 세 가지를 복사.
 */
void
spdk_scsi_task_copy_status(struct spdk_scsi_task *dst,
			   struct spdk_scsi_task *src)
{
	memcpy(dst->sense_data, src->sense_data, src->sense_data_len);
	/* [한국어] sense 바이트열 그대로 복사 — src 길이만큼만 안전하게. */
	dst->sense_data_len = src->sense_data_len;
	/* [한국어] 길이도 복사. */
	dst->status = src->status;
	/* [한국어] SCSI status 복사. */
}

/*
 * [한국어]
 * spdk_scsi_task_process_null_lun - 존재하지 않는 LUN 대상 명령에 대한 표준 응답 처리
 *
 * @task: 대상 task. task->lun이 NULL이거나 매핑된 LUN이 없을 때 사용.
 *
 * SPC-4 6.4.2: INQUIRY 명령은 미존재 LUN에 대해서도 PERIPHERAL_QUALIFIER=0x3,
 * PERIPHERAL_DEVICE_TYPE=0x1F(Unknown)로 GOOD status 응답해야 한다(이니시에이터의 LUN 탐색을 위함).
 * 그 외 명령은 LOGICAL UNIT NOT SUPPORTED(ASC=0x25)로 CHECK CONDITION 반환.
 *
 * 호출 체인:
 *   iSCSI/vhost-scsi가 task->lun==NULL 감지 → [spdk_scsi_task_process_null_lun]
 */
void
spdk_scsi_task_process_null_lun(struct spdk_scsi_task *task)
{
	uint8_t buffer[36];
	/* [한국어] INQUIRY 표준 응답 36바이트(SPC-4 Table 142 Standard Inquiry data) 임시 버퍼. */
	uint32_t allocation_len;
	/* [한국어] CDB[3..4]의 INQUIRY allocation length — 이니시에이터가 받겠다는 최대 길이. */
	uint32_t data_len;
	/* [한국어] 본 함수가 만든 응답 데이터의 실제 길이. */

	task->length = task->transfer_len;
	/* [한국어] task->length = 전송 길이로 동기화 — ULP가 이 값을 응답 길이로 본다. */
	if (task->cdb[0] == SPDK_SPC_INQUIRY) {
		/*
		 * SPC-4 states that INQUIRY commands to an unsupported LUN
		 *  must be served with PERIPHERAL QUALIFIER = 0x3 and
		 *  PERIPHERAL DEVICE TYPE = 0x1F.
		 */
		/* [한국어] INQUIRY는 항상 응답을 돌려줘야 함 — 미존재 LUN 표시 응답을 만든다. */
		data_len = sizeof(buffer);
		/* [한국어] 36바이트 표준 INQUIRY 응답 길이. */

		memset(buffer, 0, data_len);
		/* [한국어] 모든 필드 0 초기화 후 필요한 비트만 셋. */
		/* PERIPHERAL QUALIFIER(7-5) PERIPHERAL DEVICE TYPE(4-0) */
		buffer[0] = 0x03 << 5 | 0x1f;
		/* [한국어] PQ=0x3(Not Capable), PDT=0x1F(Unknown) — 미존재 LUN 의미. */
		/* ADDITIONAL LENGTH */
		buffer[4] = data_len - 5;
		/* [한국어] 첫 5바이트 헤더 이후의 추가 길이 = 36-5 = 31. */

		allocation_len = from_be16(&task->cdb[3]);
		/* [한국어] CDB allocation length(BE16). 이니시에이터가 받을 수 있는 최대 응답 바이트. */
		if (spdk_scsi_task_scatter_data(task, buffer, spdk_min(allocation_len, data_len)) >= 0) {
			/* [한국어] 응답을 task iov로 복사 — alloc/data 중 작은 쪽까지만(SPC: 응답은 alloc_len 초과 금지). */
			task->data_transferred = data_len;
			/* [한국어] 실제 채운 응답 데이터 길이를 ULP에 알림. */
			task->status = SPDK_SCSI_STATUS_GOOD;
			/* [한국어] INQUIRY는 GOOD 상태로 응답(미존재 LUN이어도). */
		}
	} else {
		/* LOGICAL UNIT NOT SUPPORTED */
		/* [한국어] INQUIRY 외 명령은 즉시 거부 — 표준 sense ASC=0x25(LU NOT SUPPORTED). */
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_LOGICAL_UNIT_NOT_SUPPORTED,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		task->data_transferred = 0;
		/* [한국어] 데이터 응답 없음. */
	}
}

/*
 * [한국어]
 * spdk_scsi_task_process_abort - task를 ABORTED COMMAND sense로 종료
 *
 * @task: 대상 task. CHECK CONDITION + sense=ABORTED COMMAND로 설정.
 *
 * LUN이 hot-removed 상태(lun->removed)에서 새로 도착한 task를 즉시 거부할 때 사용된다(lun.c).
 * 이니시에이터에게 "이 명령은 abort되었음"을 표준 방식으로 통지.
 *
 * 호출 체인:
 *   _scsi_lun_execute_task (lun->removed) → [spdk_scsi_task_process_abort]
 */
void
spdk_scsi_task_process_abort(struct spdk_scsi_task *task)
{
	spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
				  SPDK_SCSI_SENSE_ABORTED_COMMAND,
				  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
				  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	/* [한국어] sense key=0x0B(ABORTED_COMMAND), ASC=0x00(No additional sense). */
}
