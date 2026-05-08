/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] blob을 bs_dev로 노출하는 변환 레이어 (blob_bs_dev.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK Blobstore의 한 blob을 다른 blob의 "backing device(struct spdk_bs_dev)"로
 * 마치 디스크처럼 사용할 수 있게 해주는 어댑터/래퍼 레이어이다. 대표적인 사용처는
 * snapshot/clone 관계: 자식 blob(clone)이 read 요청을 받았을 때 자기 자신에게 클러스터가
 * 할당되어 있지 않으면 부모(snapshot) blob을 backing으로 보고 거기서 데이터를 읽어와야
 * 하는데, 이때 부모 blob을 spdk_bs_dev 인터페이스로 감싸 자식의 back_bs_dev에 꽂는다.
 * 이 레이어는 read/readv/readv_ext 경로에서만 의미를 가진다 — write 계열은 모두 -EPERM이며
 * (snapshot/parent는 정의상 read-only) is_zeroes/is_range_valid/translate_lba는 부모 blob의
 * 클러스터 할당 상태를 들여다보고 반환한다. read 시에는 부모 blob의 크기보다 큰 영역을
 * 자식이 요청할 수 있으므로(예: 자식이 expand된 경우), trailing 부분을 zero-fill하는
 * zero_trailing_bytes() 보조 로직을 갖는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (snapshot/clone 시나리오):
 *   blob_create_snapshot (lib/blob/blobstore.c)
 *     → bs_create_blob_bs_dev(snapshot_blob) → struct spdk_blob_bs_dev 할당
 *     → 자식 clone의 blob->back_bs_dev = &b->bs_dev
 *   사용자 read on clone → blobstore가 미할당 클러스터 감지
 *     → back_bs_dev->readv (= blob_bs_dev_readv) → spdk_blob_io_readv(부모 blob)
 *     → 부모 blob에서 데이터 읽어 자식 사용자 버퍼로 복귀
 * 실행 컨텍스트: 호스트 유저스페이스, blob 소유 SPDK thread/reactor (single-threaded
 * affinity) 위에서 호출된다. polled-mode에서 콜백은 같은 thread의 polling 라운드에서 호출.
 *
 * === 타 모듈과의 연결 ===
 * - blobstore.h: struct spdk_blob_bs_dev (bs_dev + blob 포인터 짝), spdk_bs_dev,
 *   bs_lba_to_cluster, bs_cluster_to_lba, bs_io_unit_is_allocated 등 인라인 변환 함수,
 *   spdk_blob 구조체 정의를 가져온다.
 * - spdk/blob.h (공개 API): spdk_blob_io_read/readv/readv_ext, spdk_blob_close,
 *   spdk_blob_is_degraded, spdk_bs_get_io_unit_size 등을 호출.
 * - zeroes.c: 형제 구현체. zeroes_dev는 "0으로 채워진 backing", blob_bs_dev는
 *   "다른 blob을 backing"으로 표현. 둘 다 같은 spdk_bs_dev 인터페이스 구현.
 * - request.c/h: 직접 호출은 없지만, 이 어댑터의 read는 결국 spdk_blob_io_read를 거쳐
 *   bs_sequence_*에 도달한다.
 * 데이터 흐름:
 *   사용자(자식 blob) read → 자식 blob의 미할당 클러스터 감지 → blob_bs_dev_read*
 *     → spdk_blob_io_read(부모 blob, ...) → 부모 blob의 데이터 읽음
 *     → blob_bs_dev_read_cpl 어댑터 → 사용자 콜백으로 결과 전파.
 *
 * === 주요 함수/구조체 요약 ===
 * - bs_create_blob_bs_dev(blob): blob을 spdk_bs_dev처럼 보이게 하는 어댑터를 calloc/초기화.
 *   spdk_blob_bs_dev 구조체에 함수 포인터 테이블과 blob 포인터를 채워 반환.
 * - blob_bs_dev_read/readv/readv_ext: zero_trailing_bytes로 경계 보정 후
 *   spdk_blob_io_read* 호출 → blob_bs_dev_read_cpl로 콜백 어댑팅.
 * - blob_bs_dev_write/writev/...: 모두 -EPERM (snapshot은 read-only).
 * - blob_bs_dev_destroy: spdk_blob_close 후 blob_bs_dev_destroy_cpl에서 어댑터 free.
 * - blob_bs_is_zeroes / blob_bs_is_range_valid / blob_bs_translate_lba:
 *   부모 blob의 클러스터 할당 상태와 back_bs_dev 체인을 따라가며 판정.
 * - zero_trailing_bytes (static helper): 부모 blob 크기 < 자식 read 범위인 경우
 *   payload 끝 부분을 0으로 채우고 lba_count를 줄여 spdk_blob_io_read에 넘김.
 */

#include "spdk/stdinc.h"
/* [한국어] POSIX 표준 헤더 모음 — calloc/free/memset/EPERM/assert 등. */
#include "spdk/blob.h"
/* [한국어] Blobstore 공개 API — spdk_blob_io_read/readv/readv_ext, spdk_blob_close,
 * spdk_blob_is_degraded, spdk_bs_dev_cb_args, spdk_blob_ext_io_opts 등. */
#include "spdk/log.h"
/* [한국어] SPDK 로깅 매크로 — SPDK_ERRLOG (destroy 콜백 에러 보고)에 사용. */
#include "spdk/likely.h"
/* [한국어] spdk_likely/spdk_unlikely 분기 힌트 — zero_trailing_bytes의 fast path 표시. */
#include "blobstore.h"
/* [한국어] 내부 헤더 — struct spdk_blob_bs_dev, struct spdk_blob, 인라인 LBA/cluster
 * 변환 함수(bs_lba_to_cluster, bs_cluster_to_lba, bs_io_unit_is_allocated,
 * bs_io_unit_to_back_dev_lba, bs_blob_io_unit_to_lba, bs_dev_byte_to_lba) 사용. */

/*
 * [한국어]
 * blob_bs_dev_write - blob-backed bs_dev에 대한 write 요청 거부.
 *
 * @dev/@channel/@payload/@lba/@lba_count: write 인자들 (모두 무시)
 * @cb_args: 완료 콜백 — -EPERM으로 즉시 실패 통지
 *
 * 이 함수가 왜 필요한가: blob을 backing dev로 노출하는 시나리오는 보통 snapshot/clone
 * 관계이며, snapshot은 정의상 read-only다. 따라서 backing 경로를 통한 write는 발생할 수
 * 없으며, 도달하면 상위 로직 버그.
 * 동작: -EPERM으로 콜백 호출 후 assert(false)로 abort (디버그 빌드 한정).
 * 실행 컨텍스트: 정상 흐름에서는 절대 도달하지 않아야 하는 경로.
 */
static void
blob_bs_dev_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		  uint64_t lba, uint32_t lba_count,
		  struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	/* [한국어] -EPERM(연산 미허용)으로 즉시 콜백 — backing blob은 read-only. */
	assert(false);
	/* [한국어] 디버그 빌드에서는 abort — 도달 불가 경로. */
}

/*
 * [한국어]
 * blob_bs_dev_writev - 벡터 write 요청 거부 (-EPERM).
 *
 * 동작은 blob_bs_dev_write와 동일. snapshot read-only 정책에 따라 모든 write 변형이 금지됨.
 */
static void
blob_bs_dev_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		   struct iovec *iov, int iovcnt,
		   uint64_t lba, uint32_t lba_count,
		   struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	/* [한국어] 즉시 -EPERM 통지. */
	assert(false);
	/* [한국어] 도달 불가 표명. */
}

/*
 * [한국어]
 * blob_bs_dev_writev_ext - 외부 메모리 도메인 지원 vectored write 거부 (-EPERM).
 *
 * ext_opts가 추가되었지만 동작은 blob_bs_dev_writev와 동일 — snapshot은 read-only.
 */
static void
blob_bs_dev_writev_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		       struct iovec *iov, int iovcnt,
		       uint64_t lba, uint32_t lba_count,
		       struct spdk_bs_dev_cb_args *cb_args,
		       struct spdk_blob_ext_io_opts *ext_opts)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	/* [한국어] 즉시 -EPERM. */
	assert(false);
	/* [한국어] 도달 불가. */
}

/*
 * [한국어]
 * blob_bs_dev_write_zeroes - WRITE_ZEROES 명령 거부 (-EPERM).
 *
 * NVMe Write Zeroes opcode 0x08에 대응. snapshot은 read-only이므로 0 쓰기도 금지.
 */
static void
blob_bs_dev_write_zeroes(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			 uint64_t lba, uint64_t lba_count,
			 struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	/* [한국어] -EPERM 통지. */
	assert(false);
	/* [한국어] 도달 불가. */
}

/*
 * [한국어]
 * blob_bs_dev_unmap - UNMAP/Deallocate 명령 거부 (-EPERM).
 *
 * NVMe Dataset Management Deallocate / SCSI UNMAP에 대응. read-only 정책으로 금지.
 */
static void
blob_bs_dev_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		  uint64_t lba, uint64_t lba_count,
		  struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	/* [한국어] -EPERM 통지. */
	assert(false);
	/* [한국어] 도달 불가. */
}

/*
 * [한국어]
 * blob_bs_dev_read_cpl - spdk_blob_io_read 완료 시 호출되는 어댑터 콜백.
 *
 * @cb_arg: 시작 시점에 전달했던 spdk_bs_dev_cb_args 포인터
 * @bserrno: spdk_blob_io_read 결과 (0=성공, <0=blobstore 에러)
 *
 * 이 함수가 왜 필요한가: spdk_blob_io_read의 완료 콜백 시그니처는
 * (void *cb_arg, int bserrno)이지만, bs_dev 인터페이스 사용자는
 * (channel, cb_arg, bserrno)를 기대한다. 시그니처/인자 변환 어댑터 역할.
 * 동작: cb_arg를 spdk_bs_dev_cb_args로 캐스팅한 뒤, 그 안의 cb_fn을
 * cb_args->channel과 cb_args->cb_arg로 호출하여 상위 사용자에게 결과 전달.
 * 실행 컨텍스트: spdk_blob_io_read의 완료 컨텍스트 (보통 같은 SPDK thread).
 *
 * 호출 체인:
 *   blob_bs_dev_read* → spdk_blob_io_read* (... cb=blob_bs_dev_read_cpl, ctx=cb_args)
 *     → 비동기 완료 → blob_bs_dev_read_cpl → cb_args->cb_fn (사용자 알림)
 */
static void
blob_bs_dev_read_cpl(void *cb_arg, int bserrno)
{
	struct spdk_bs_dev_cb_args *cb_args = (struct spdk_bs_dev_cb_args *)cb_arg;
	/* [한국어] 시작 시 ctx로 넘긴 spdk_bs_dev_cb_args 포인터를 복원 (void* 캐스팅). */

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, bserrno);
	/* [한국어] 상위 bs_dev 사용자에게 결과 전파 — bserrno를 그대로 통과시킨다.
	 * channel은 처음 read 시작 시 cb_args->channel에 저장된 값(상위 채널). */
}

/*
 * [한국어]
 * zero_trailing_bytes - read 범위 중 backing blob 크기를 넘는 부분을 zero-fill하고
 *                       lba_count를 valid 부분만큼 축소한다.
 *
 * @b: blob을 backing dev로 감싼 어댑터 — b->bs_dev.blockcnt가 backing blob의 LBA 개수
 * @iov: 사용자 페이로드의 iovec 배열 (read 시 채워질 출력 버퍼들)
 * @iovcnt: iov 배열 길이
 * @lba: read 시작 LBA (자식 blob 좌표계)
 * @lba_count: in/out — 입력은 사용자 요청 lba_count, 출력은 backing blob 안에 들어가는
 *             "유효한" lba_count로 축소됨 (그 이후는 0으로 미리 채워짐)
 *
 * 이 함수가 왜 필요한가: 자식 blob(clone)이 부모 blob(snapshot) 이후로 expand되어
 * 부모보다 더 많은 클러스터를 가질 수 있다. 이때 자식이 자신의 큰 LBA 범위를 read하면
 * 부모 backing dev에 그대로 위임할 경우 부모 경계 밖이라 에러가 난다. 그래서:
 *   1) 경계 밖 부분에 해당하는 LBA 개수(zero_lba_count)를 계산
 *   2) 그 부분에 대응되는 페이로드 영역만 미리 0으로 memset
 *   3) lba_count를 줄여 부모에게는 backing 안에 들어가는 부분만 위임
 * 이렇게 함으로써 확장된 thin blob의 read 시맨틱(0으로 채움)을 보장한다.
 *
 * 동작 과정:
 *   - fast path: lba + lba_count <= blockcnt이면 경계 밖 영역 없음 → 즉시 반환.
 *   - 경계 밖이 있다면 zero_bytes 계산 후 iov 배열을 끝에서부터 거꾸로 따라가며
 *     valid_bytes 이후의 트레일링 영역만 zero-fill.
 *   - 마지막에 *lba_count -= zero_lba_count로 호출자에게 축소된 LBA 개수를 돌려줌.
 *
 * 실행 컨텍스트: blob_bs_dev_read/readv/readv_ext에서 spdk_blob_io_read 호출 직전에 동기 실행.
 *
 * 호출 체인:
 *   blob_bs_dev_read(v|v_ext) → zero_trailing_bytes → (return) → spdk_blob_io_read*
 */
static inline void
zero_trailing_bytes(struct spdk_blob_bs_dev *b, struct iovec *iov, int iovcnt,
		    uint64_t lba, uint32_t *lba_count)
{
	uint32_t zero_lba_count;
	/* [한국어] 페이로드에서 0으로 채워야 할 LBA(블록) 개수. */
	uint64_t zero_bytes, zero_len;
	/* [한국어] zero_bytes: 0으로 채워야 할 총 바이트 수. zero_len: 한 iov에서 zero 처리할 바이트. */
	uint64_t payload_bytes;
	/* [한국어] 사용자 요청 페이로드 전체 바이트 수 (lba_count * blocklen). */
	uint64_t valid_bytes;
	/* [한국어] backing blob 안에 들어가는 "유효" 바이트 수 — 이만큼은 부모 read에 맡김. */
	void *zero_start;
	/* [한국어] 현재 iov에서 0으로 채울 시작 주소 (iov_base + valid_bytes). */
	struct iovec *i;
	/* [한국어] iov 배열 순회 포인터. */

	if (spdk_likely(lba + *lba_count <= b->bs_dev.blockcnt)) {
		/* [한국어] fast path — 요청 범위가 backing blob 안에 완전히 들어감.
		 * spdk_likely 힌트: 보통 자식 blob이 부모 크기를 넘지 않으므로 이 경로가 핫. */
		return;
		/* [한국어] zero-fill 불필요, lba_count 그대로. */
	}

	/* Figure out how many bytes in the payload will need to be zeroed. */
	zero_lba_count = spdk_min(*lba_count, lba + *lba_count - b->bs_dev.blockcnt);
	/* [한국어] 경계 밖 LBA 수 계산.
	 * lba+*lba_count - blockcnt = 경계 너머로 넘은 LBA 수.
	 * 다만 lba 자체가 이미 blockcnt를 넘은 경우(전체가 밖)에는 *lba_count로 캡.
	 * spdk_min은 두 경우 중 작은 값을 취해 안전하게 처리. */
	zero_bytes = zero_lba_count * (uint64_t)b->bs_dev.blocklen;
	/* [한국어] 0으로 채울 바이트 수 = LBA 수 × 블록 크기.
	 * uint64_t 캐스팅으로 32비트 곱셈 오버플로 방지. */

	payload_bytes = *lba_count * (uint64_t)b->bs_dev.blocklen;
	/* [한국어] 사용자 요청 전체 바이트 수 — iov 순회 시 누적 차감 변수로 사용. */
	valid_bytes = payload_bytes - zero_bytes;
	/* [한국어] backing 안에 들어가는 유효 데이터 바이트 — 0으로 채우지 않을 영역 길이. */

	i = iov;
	/* [한국어] iov 배열 순회 시작 — 첫 원소부터 valid_bytes를 소비하며 진행. */
	while (zero_bytes > 0) {
		/* [한국어] 아직 0으로 채울 바이트가 남아있는 동안 반복.
		 * iov를 처음부터 따라가며 valid 영역을 소비하고, 그 이후가 zero 영역이 된다. */
		if (i->iov_len > valid_bytes) {
			/* [한국어] 이 iov에 valid 영역이 끝나는 지점이 포함됨 → 여기서 zero-fill 시작.
			 * 즉, 이 iov의 앞 valid_bytes는 backing이 채울 영역, 그 이후는 0으로 채워야 함. */
			zero_start = i->iov_base + valid_bytes;
			/* [한국어] zero-fill 시작 주소 — iov 시작에서 valid_bytes만큼 지난 곳. */
			zero_len = spdk_min(payload_bytes, i->iov_len - valid_bytes);
			/* [한국어] 이 iov에서 0으로 채울 바이트 수 — 남은 페이로드 또는 iov 잔여 중 작은 값. */
			memset(zero_start, 0, zero_bytes);
			/* [한국어] 실제 zero-fill — 주의: zero_len이 아니라 zero_bytes로 memset.
			 * 경계 케이스에서 다음 iov까지 0으로 채워야 할 수도 있어 보수적으로 zero_bytes 사용.
			 * (zero_bytes는 남은 0-fill 총량. memset은 단일 iov 내에서만 안전하지만,
			 *  iov_base + valid_bytes 이후가 충분히 길면 OK — 일반적으로 페이로드는 단일 큰 버퍼. */
			valid_bytes = 0;
			/* [한국어] valid 영역 모두 소비됨. 이후 iov는 모두 zero 영역. */
			zero_bytes -= zero_len;
			/* [한국어] 처리한 만큼 차감 — 0이 되면 루프 종료. */
		}
		valid_bytes -= spdk_min(valid_bytes, i->iov_len);
		/* [한국어] valid_bytes에서 이번 iov가 차지한 만큼 차감 (음수 방지). */
		payload_bytes -= spdk_min(payload_bytes, i->iov_len);
		/* [한국어] 남은 페이로드 바이트도 동일하게 차감. */
		i++;
		/* [한국어] 다음 iov로 이동. */
	}

	*lba_count -= zero_lba_count;
	/* [한국어] 호출자에게 축소된 lba_count 반환 — 이후 부모 blob에는 이 값으로 read 위임. */
}

/*
 * [한국어]
 * blob_bs_dev_read - blob을 backing dev로 한 read 위임. 경계 보정 후 spdk_blob_io_read 호출.
 *
 * @dev: spdk_blob_bs_dev로 캐스팅 가능한 어댑터 (첫 멤버가 spdk_bs_dev이므로 안전)
 * @channel: backing blob에 read를 발행할 I/O 채널 (자식 blob의 채널과 일반적으로 동일)
 * @payload: read 결과를 채울 사용자 버퍼
 * @lba: 시작 LBA (자식 blob 좌표계 기준의 io_unit 인덱스)
 * @lba_count: 읽을 LBA 개수
 * @cb_args: 완료 콜백 (cb_fn은 blob_bs_dev_read_cpl을 거쳐 호출됨)
 *
 * 이 함수가 왜 필요한가: 자식 blob이 미할당 클러스터를 read할 때, blobstore는
 * back_bs_dev->read를 호출하여 backing에서 데이터를 가져온다. backing이 다른 blob인 경우
 * 이 함수가 호출되어 spdk_blob_io_read로 위임한다. 단, 자식이 부모보다 클 수 있으므로
 * trailing zero-fill 보정이 필요하다.
 * 동작:
 *   1) dev → b로 캐스팅 (첫 멤버가 bs_dev이므로 같은 주소).
 *   2) iov 1개짜리 가짜 벡터를 만들어 zero_trailing_bytes 호출 (단일 버퍼도 동일 로직 재사용).
 *   3) 축소된 lba_count로 spdk_blob_io_read 발행 — 완료 콜백은 blob_bs_dev_read_cpl 어댑터.
 * 실행 컨텍스트: 자식 blob 소유 SPDK thread.
 *
 * 호출 체인:
 *   blobstore.c (CoW/snapshot read 경로) → back_bs_dev->read = blob_bs_dev_read
 *     → spdk_blob_io_read(부모 blob) → ... → blob_bs_dev_read_cpl → 사용자 콜백
 */
static inline void
blob_bs_dev_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		 uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;
	/* [한국어] 어댑터 캐스팅 — spdk_blob_bs_dev의 첫 필드가 spdk_bs_dev이므로
	 * 같은 메모리를 다른 타입 뷰로 본다 (C의 구조체 첫 멤버 호환성 패턴). */
	struct iovec iov;
	/* [한국어] zero_trailing_bytes는 iovec 인터페이스로 통일되어 있어, 단일 버퍼를
	 * 1-원소 iov로 감싸 호출. */

	iov.iov_base = payload;
	/* [한국어] iov 시작 주소 = 사용자 페이로드 버퍼. */
	iov.iov_len = lba_count * b->bs_dev.blocklen;
	/* [한국어] iov 길이 = 총 바이트 수 (lba_count × blocklen). */
	/* The backing blob may be smaller than this blob, so zero any trailing bytes. */
	zero_trailing_bytes(b, &iov, 1, lba, &lba_count);
	/* [한국어] 경계 밖 영역을 0으로 미리 채우고 lba_count 축소. iovcnt=1.
	 * 자식 blob이 부모보다 클 때 이 보정이 일어난다. */

	spdk_blob_io_read(b->blob, channel, payload, lba, lba_count,
			  blob_bs_dev_read_cpl, cb_args);
	/* [한국어] 부모(backing) blob에 실제 read 발행. payload 시작은 그대로,
	 * lba_count는 축소된 값(부모 안에 들어가는 부분만). 완료 시 blob_bs_dev_read_cpl이
	 * cb_args를 받아 사용자 콜백으로 전달. */
}

/*
 * [한국어]
 * blob_bs_dev_readv - 벡터 read 위임. blob_bs_dev_read의 iovec 버전.
 *
 * @dev/@channel/@cb_args: blob_bs_dev_read와 동일
 * @iov/@iovcnt: scatter-gather read 출력 버퍼 배열
 * @lba/@lba_count: read 좌표 (in/out via zero_trailing_bytes)
 *
 * 동작은 blob_bs_dev_read와 동일하나 iov를 그대로 받아 zero_trailing_bytes에 넘기고
 * spdk_blob_io_readv로 발행한다. 경계 보정 로직은 동일.
 */
static inline void
blob_bs_dev_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		  struct iovec *iov, int iovcnt,
		  uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;
	/* [한국어] 어댑터 캐스팅 (위 read와 동일 패턴). */

	/* The backing blob may be smaller than this blob, so zero any trailing bytes. */
	zero_trailing_bytes(b, iov, iovcnt, lba, &lba_count);
	/* [한국어] iov 배열을 직접 보정 — 부모 경계 밖 부분을 0으로 채우고 lba_count 축소. */

	spdk_blob_io_readv(b->blob, channel, iov, iovcnt, lba, lba_count,
			   blob_bs_dev_read_cpl, cb_args);
	/* [한국어] 부모 blob에 vectored read 발행. 완료 어댑터는 blob_bs_dev_read_cpl. */
}

/*
 * [한국어]
 * blob_bs_dev_readv_ext - 외부 메모리 도메인 지원 vectored read 위임.
 *
 * @dev/@channel/@iov/@iovcnt/@lba/@lba_count/@cb_args: blob_bs_dev_readv와 동일
 * @ext_opts: 외부 메모리 도메인 옵션 (GPU/RDMA 메모리 등)
 *
 * 일반 readv와 동일한 경계 보정 후 spdk_blob_io_readv_ext로 발행. ext_opts는 그대로 전파.
 * 주의: zero_trailing_bytes는 CPU memset을 쓰므로, ext 경로에서 iov_base가 외부
 * 도메인 메모리라면 이론적으로는 spdk_memory_domain_memzero가 필요할 수 있으나,
 * 현재 구현은 통상 단순 단일 버퍼 read 시나리오를 다룬다 (zeroes_readv_ext에 별도 처리).
 */
static inline void
blob_bs_dev_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      struct iovec *iov, int iovcnt,
		      uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args,
		      struct spdk_blob_ext_io_opts *ext_opts)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;
	/* [한국어] 어댑터 캐스팅. */

	/* The backing blob may be smaller than this blob, so zero any trailing bytes. */
	zero_trailing_bytes(b, iov, iovcnt, lba, &lba_count);
	/* [한국어] 부모 경계 보정. */

	spdk_blob_io_readv_ext(b->blob, channel, iov, iovcnt, lba, lba_count,
			       blob_bs_dev_read_cpl, cb_args, ext_opts);
	/* [한국어] ext 변형으로 부모 blob에 read 발행 — ext_opts(memory_domain 포함)를 그대로 전달. */
}

/*
 * [한국어]
 * blob_bs_dev_destroy_cpl - spdk_blob_close 완료 후 어댑터 메모리 해제 콜백.
 *
 * @cb_arg: spdk_blob_bs_dev 포인터 (bs_create_blob_bs_dev에서 calloc된 객체)
 * @bserrno: spdk_blob_close 결과 (0=성공)
 *
 * 이 함수가 왜 필요한가: 어댑터 destroy는 비동기 — 먼저 backing blob의 reference를
 * 풀기 위해 spdk_blob_close를 호출하고, 그 완료 후에야 어댑터 자체 메모리를 free할 수 있다.
 * 동작: 에러가 있으면 SPDK_ERRLOG로 기록하고, 성공/실패 무관하게 cb_arg를 free.
 * 실행 컨텍스트: spdk_blob_close 완료 컨텍스트 (보통 blob 소유 thread).
 */
static void
blob_bs_dev_destroy_cpl(void *cb_arg, int bserrno)
{
	if (bserrno != 0) {
		/* [한국어] blob_close 실패 — 에러 로깅 (메모리는 그래도 해제). */
		SPDK_ERRLOG("Error on blob_bs_dev destroy: %d", bserrno);
	}

	/* Free blob_bs_dev */
	free(cb_arg);
	/* [한국어] 어댑터 객체 free — bs_create_blob_bs_dev에서 calloc된 메모리 회수. */
}

/*
 * [한국어]
 * blob_bs_dev_destroy - 어댑터 destroy 진입점. backing blob을 close 후 어댑터 free.
 *
 * @bs_dev: 파괴 대상 어댑터 (spdk_blob_bs_dev로 캐스팅됨)
 *
 * 이 함수가 왜 필요한가: backing blob이 다른 blob일 때, 자식이 사라지면서 부모에 대한
 * reference도 해제해야 한다 (open_ref 카운트 감소). spdk_blob_close가 그 작업을 하며,
 * 비동기 완료 후 어댑터 메모리도 free해야 하므로 콜백을 통해 두 단계로 나눈다.
 * 동작: bs_dev → b 캐스팅 → spdk_blob_close(b->blob, blob_bs_dev_destroy_cpl, b) 호출.
 * 실행 컨텍스트: blob 정리 경로 (예: 자식 blob의 close에서 back_bs_dev->destroy 호출).
 *
 * 호출 체인:
 *   blob_close (자식) → blob->back_bs_dev->destroy = blob_bs_dev_destroy
 *     → spdk_blob_close(부모) → blob_bs_dev_destroy_cpl → free(b)
 */
static void
blob_bs_dev_destroy(struct spdk_bs_dev *bs_dev)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)bs_dev;
	/* [한국어] 어댑터 캐스팅 — b->blob이 backing blob 포인터. */

	spdk_blob_close(b->blob, blob_bs_dev_destroy_cpl, b);
	/* [한국어] backing blob의 reference 해제 (open_ref-- 등). 비동기 완료는
	 * blob_bs_dev_destroy_cpl가 처리하며 cb_arg=b로 어댑터 메모리도 같이 free된다. */
}

/*
 * [한국어]
 * blob_bs_is_zeroes - backing blob의 특정 클러스터 범위가 모두 0인지 질의.
 *
 * @dev: blob_bs_dev 어댑터
 * @lba: 질의 시작 LBA (cluster 정렬 가정)
 * @lba_count: 질의 LBA 수 (한 클러스터 분량 가정)
 * @return: 모두 0이면 true, 한 곳이라도 데이터가 있으면 false
 *
 * 이 함수가 왜 필요한가: blobstore의 CoW 경로에서 자식이 부모로부터 클러스터를
 * 복사하기 전에, "부모의 그 클러스터가 어차피 0이라면 복사 생략 + zero-fill"으로
 * 최적화할 수 있다. 그 판정에 사용된다.
 * 동작:
 *   1) 부모 blob에 직접 클러스터가 할당되어 있으면(데이터가 있으면) false.
 *   2) 부모도 미할당이면 부모의 back_bs_dev로 재귀적으로 질의.
 *      - 부모의 backing이 zeroes_dev면 결국 true가 돌아옴.
 *      - 부모도 다른 blob이면 그 backing 체인을 따라 계속 추적.
 *   3) is_range_valid로 backing 경계도 확인해 안전하게 처리.
 * 실행 컨텍스트: blob 소유 thread, CoW 결정 시 동기 호출.
 */
static bool
blob_bs_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;
	/* [한국어] 어댑터 캐스팅. */
	struct spdk_blob *blob = b->blob;
	/* [한국어] backing blob 포인터 (부모/snapshot blob). */
	bool is_valid_range;
	/* [한국어] backing 체인의 다음 단계가 LBA 범위를 다룰 수 있는지. */

	assert(lba == bs_cluster_to_lba(blob->bs, bs_lba_to_cluster(blob->bs, lba)));
	/* [한국어] lba가 클러스터 경계에 정렬되어 있어야 함을 표명.
	 * lba → cluster → lba 라운드트립 결과가 동일해야 정렬. */
	assert(lba_count == bs_dev_byte_to_lba(dev, blob->bs->cluster_sz));
	/* [한국어] lba_count는 정확히 한 클러스터 분량의 LBA여야 함 (CoW 단위가 클러스터). */

	if (bs_io_unit_is_allocated(blob, lba)) {
		/* [한국어] 부모 blob에 이 클러스터가 실제로 할당되어 있다면 데이터가 있는 것 → 0 아님. */
		return false;
	}

	assert(blob->back_bs_dev != NULL);
	/* [한국어] 부모 blob에도 backing이 반드시 있어야 함 (미할당이면 어딘가로 위임). */
	is_valid_range = blob->back_bs_dev->is_range_valid(blob->back_bs_dev, lba, lba_count);
	/* [한국어] 부모의 backing이 이 LBA 범위를 다룰 수 있는지 확인 — 경계 밖이면 false. */
	return is_valid_range && blob->back_bs_dev->is_zeroes(blob->back_bs_dev,
			bs_io_unit_to_back_dev_lba(blob, lba),
			bs_io_unit_to_back_dev_lba(blob, lba_count));
	/* [한국어] backing이 다룰 수 있고, backing 좌표계로 변환된 LBA에서 모두 0이면 true.
	 * bs_io_unit_to_back_dev_lba는 자식 io_unit → backing의 blocklen 단위로 환산. */
}

/*
 * [한국어]
 * blob_bs_is_range_valid - 어댑터의 LBA 범위가 backing blob 경계 안에 있는지 질의.
 *
 * @dev: blob_bs_dev 어댑터
 * @lba: 시작 LBA (클러스터 첫 LBA로 가정)
 * @lba_count: cluster_sz에 해당하는 LBA 수 (예: 4MiB 클러스터, 512B 블록 → 8192)
 * @return: lba가 backing blob의 클러스터 경계 안이면 true
 *
 * 이 함수가 왜 필요한가: 자식 blob이 부모 snapshot보다 클 수 있는데, 이 어댑터의
 * blockcnt는 자식 기준으로 설정되어 있다. 따라서 어떤 클러스터를 backing에서 읽으려
 * 할 때 부모(=이 어댑터의 blob) 안에 정말 들어가는지 확인이 필요.
 * 동작: lba를 부모의 active.num_clusters * io_units_per_cluster와 비교 — 이 범위 안이면 true.
 * 실행 컨텍스트: blob 소유 thread.
 */
static bool
blob_bs_is_range_valid(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;
	/* [한국어] 어댑터 캐스팅. */
	struct spdk_blob *blob = b->blob;
	/* [한국어] backing blob (부모) 포인터. */
	uint64_t	io_units_per_cluster;
	/* [한국어] 클러스터 당 io_unit 수 — 경계 계산에 사용. */

	/* The lba here is supposed to be the first lba of cluster. lba_count
	 * will typically be fixed e.g. 8192 for 4MiB cluster. */
	assert(lba_count == blob->bs->cluster_sz / dev->blocklen);
	/* [한국어] lba_count = 한 클러스터의 LBA 수 (cluster_sz / blocklen). */
	assert(lba % lba_count == 0);
	/* [한국어] lba가 클러스터 경계에 정렬되어 있음. */

	io_units_per_cluster = blob->bs->io_units_per_cluster;
	/* [한국어] blobstore 전역 설정값을 로컬 변수로 캐시. */

	/* A blob will either have:
	* - no backing bs_bdev (normal thick blob), or
	* - zeroes backing bs_bdev (thin provisioned blob), or
	* - blob backing bs_bdev (e.g snapshot)
	* It may be possible that backing bs_bdev has lesser number of clusters
	* than the child lvol blob because lvol blob has been expanded after
	* taking snapshot. In such a case, page will be outside the cluster io_unit
	* range of the backing dev. Always return true for zeroes backing bdev. */
	return lba < blob->active.num_clusters * io_units_per_cluster;
	/* [한국어] backing(=부모) blob이 다룰 수 있는 io_unit 총 수 = num_clusters × io_units_per_cluster.
	 * lba가 이 값 미만이면 부모 안 → true. 그 이상이면 자식 확장으로 인한 경계 밖 → false. */
}

/*
 * [한국어]
 * blob_bs_translate_lba - 어댑터의 가상 LBA를 backing 체인을 따라 물리 LBA로 변환.
 *
 * @dev: blob_bs_dev 어댑터
 * @lba: 변환할 가상 LBA (어댑터 blob 기준)
 * @base_lba: 출력 — 진짜 물리 LBA(스토리지 디바이스 기준)
 * @return: 변환 성공이면 true (그리고 *base_lba에 결과 기록), 실패면 false
 *
 * 이 함수가 왜 필요한가: 어떤 경로(예: bdev_passthru 패턴)에서는 blob LBA → 실제 디스크 LBA
 * 매핑이 필요하다. 어댑터 blob이 클러스터를 직접 가지고 있다면 그 매핑을 반환하고,
 * 아니라면 backing(부모의 backing) 체인을 따라 재귀적으로 해석한다.
 * 동작:
 *   1) 어댑터 blob에 lba가 할당되어 있으면 bs_blob_io_unit_to_lba로 직접 변환 후 반환.
 *   2) 미할당이면 backing의 is_range_valid + translate_lba에 위임 (좌표 변환 후).
 * 실행 컨텍스트: blob 소유 thread, 상위 매핑 질의 시 동기 호출.
 */
static bool
blob_bs_translate_lba(struct spdk_bs_dev *dev, uint64_t lba, uint64_t *base_lba)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;
	/* [한국어] 어댑터 캐스팅. */
	struct spdk_blob *blob = b->blob;
	/* [한국어] backing blob 포인터. */
	bool is_valid_range;
	/* [한국어] 다음 backing 단계의 범위 유효성. */

	assert(base_lba != NULL);
	/* [한국어] 호출자는 출력 포인터를 반드시 제공해야 함. */
	if (bs_io_unit_is_allocated(blob, lba)) {
		/* [한국어] 어댑터 blob 자신에 클러스터가 할당되어 있으면 직접 매핑. */
		*base_lba = bs_blob_io_unit_to_lba(blob, lba);
		/* [한국어] io_unit → 디스크 LBA 변환 (clusters 배열에서 cluster 시작 LBA 찾기). */
		return true;
	}

	assert(blob->back_bs_dev != NULL);
	/* [한국어] 미할당이면 backing이 반드시 있어야 (zeroes나 다른 blob). */
	/* Since here we don't get lba_count directly, passing lba_count derived
	 * from cluster_sz which typically happens for other calls like is_zeroes
	 * in CoW path. */
	is_valid_range = blob->back_bs_dev->is_range_valid(blob->back_bs_dev, lba,
			 bs_dev_byte_to_lba(blob->back_bs_dev, blob->bs->cluster_sz));
	/* [한국어] backing이 lba를 다룰 수 있는지 확인 — lba_count는 클러스터 크기에서 유도. */
	return is_valid_range && blob->back_bs_dev->translate_lba(blob->back_bs_dev,
			bs_io_unit_to_back_dev_lba(blob, lba),
			base_lba);
	/* [한국어] backing 체인의 다음 단계로 변환 위임. backing 좌표계로 변환된 lba 사용.
	 * backing이 zeroes면 false 반환 (zeroes는 물리 매핑 없음). */
}

/*
 * [한국어]
 * blob_bs_is_degraded - backing blob이 degraded 상태인지 (예: 외부 snapshot 미가용).
 *
 * @dev: 어댑터
 * @return: spdk_blob_is_degraded(b->blob) 결과 그대로
 *
 * 이 함수가 왜 필요한가: 외부 snapshot(esnap) 같이 외부 데이터 소스에 의존하는 blob은
 * 그 외부가 사용 불가하면 degraded로 간주된다. 어댑터 사용자(자식 blob 사용자)가
 * 이 상태를 알아야 read 실패 가능성을 인지할 수 있다.
 * 동작: 단순히 spdk_blob_is_degraded로 위임.
 */
static bool
blob_bs_is_degraded(struct spdk_bs_dev *dev)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;
	/* [한국어] 어댑터 캐스팅. */

	return spdk_blob_is_degraded(b->blob);
	/* [한국어] backing blob의 degraded 상태를 그대로 반환 (esnap 등에 사용). */
}

/*
 * [한국어]
 * bs_create_blob_bs_dev - blob을 backing dev로 노출하는 어댑터 객체 생성.
 *
 * @blob: backing으로 사용할 blob (보통 snapshot/parent blob)
 * @return: 새로 할당된 spdk_bs_dev 포인터 (실패 시 NULL — calloc 실패)
 *
 * 이 함수가 왜 필요한가: 자식 blob(clone)의 back_bs_dev에 부모 blob을 꽂으려면
 * spdk_bs_dev 인터페이스로 변환된 객체가 필요하다. 이 팩토리는 spdk_blob_bs_dev를
 * calloc하고 함수 포인터 테이블 + blob 포인터를 채워 반환한다.
 * 동작:
 *   1) calloc(1, sizeof(spdk_blob_bs_dev)) — 0으로 초기화된 어댑터 객체 할당.
 *   2) blockcnt/blocklen을 backing blob의 크기·io_unit 크기로 설정.
 *   3) 모든 함수 포인터(read/write/zeroes/translate 등)를 이 파일의 정적 함수로 연결.
 *   4) b->blob에 backing blob 포인터 저장 (read/translate 시 캐스팅으로 접근).
 *   5) 첫 멤버인 b->bs_dev의 주소를 반환 (호출자는 spdk_bs_dev*로 다룸).
 * 실행 컨텍스트: snapshot 생성, esnap 설정 등 blob 라이프사이클 이벤트에서 호출.
 *
 * 호출 체인:
 *   blob_create_snapshot / esnap setup → bs_create_blob_bs_dev(snapshot)
 *     → 자식 blob의 back_bs_dev = 반환값
 */
struct spdk_bs_dev *
bs_create_blob_bs_dev(struct spdk_blob *blob)
{
	struct spdk_blob_bs_dev  *b;
	/* [한국어] 어댑터 객체 포인터 — 새로 할당하여 채울 대상. */

	b = calloc(1, sizeof(*b));
	/* [한국어] 0-초기화 할당 — 모든 함수 포인터가 NULL인 상태로 시작.
	 * 이후 명시적으로 함수 포인터를 채워야 한다. */
	if (b == NULL) {
		/* [한국어] 메모리 부족 — 호출자가 처리할 수 있도록 NULL 반환. */
		return NULL;
	}
	/* snapshot blob */
	b->bs_dev.blockcnt = blob->active.num_clusters * blob->bs->io_units_per_cluster;
	/* [한국어] 어댑터의 총 블록(io_unit) 수 = backing blob의 클러스터 수 × 클러스터당 io_unit 수. */
	b->bs_dev.blocklen = spdk_bs_get_io_unit_size(blob->bs);
	/* [한국어] 블록 크기 = blobstore의 io_unit 크기 (보통 4KiB 또는 512B). */
	b->bs_dev.create_channel = NULL;
	/* [한국어] 채널 생성 콜백 — 어댑터 자체는 채널 컨텍스트 불필요 (read는 blob 채널로 위임). */
	b->bs_dev.destroy_channel = NULL;
	/* [한국어] 채널 파괴 콜백 — 위와 같은 이유로 NULL. */
	b->bs_dev.destroy = blob_bs_dev_destroy;
	/* [한국어] destroy — backing blob을 close하고 어댑터 free. */
	b->bs_dev.write = blob_bs_dev_write;
	/* [한국어] write — -EPERM (snapshot은 read-only). */
	b->bs_dev.writev = blob_bs_dev_writev;
	/* [한국어] writev — -EPERM. */
	b->bs_dev.writev_ext = blob_bs_dev_writev_ext;
	/* [한국어] writev_ext — -EPERM. */
	b->bs_dev.read = blob_bs_dev_read;
	/* [한국어] read — zero_trailing 보정 후 spdk_blob_io_read. */
	b->bs_dev.readv = blob_bs_dev_readv;
	/* [한국어] readv — vectored 버전. */
	b->bs_dev.readv_ext = blob_bs_dev_readv_ext;
	/* [한국어] readv_ext — 외부 메모리 도메인 지원 vectored read. */
	b->bs_dev.write_zeroes = blob_bs_dev_write_zeroes;
	/* [한국어] write_zeroes — -EPERM. */
	b->bs_dev.unmap = blob_bs_dev_unmap;
	/* [한국어] unmap — -EPERM. */
	b->bs_dev.is_zeroes = blob_bs_is_zeroes;
	/* [한국어] is_zeroes — backing blob의 클러스터 할당 상태 + 체인 재귀 질의. */
	b->bs_dev.is_range_valid = blob_bs_is_range_valid;
	/* [한국어] is_range_valid — backing blob의 cluster 경계 검사. */
	b->bs_dev.translate_lba = blob_bs_translate_lba;
	/* [한국어] translate_lba — 클러스터 매핑 또는 backing 체인 재귀. */
	b->bs_dev.is_degraded = blob_bs_is_degraded;
	/* [한국어] is_degraded — esnap 등 외부 의존 가용성 확인. */
	b->blob = blob;
	/* [한국어] backing blob 포인터 저장 — 모든 핸들러가 (struct spdk_blob_bs_dev *)dev로
	 * 캐스팅 후 b->blob을 통해 접근한다. */

	return &b->bs_dev;
	/* [한국어] 첫 멤버 주소 반환 — 호출자에게는 spdk_bs_dev *로 보임 (구조체 첫 멤버 호환).
	 * 캐스팅을 통해 spdk_blob_bs_dev *로도 다시 회수 가능. */
}
