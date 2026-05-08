/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] zeroes 백킹 디바이스 — 항상 0을 반환하는 가상 bs_dev (zeroes.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK Blobstore에서 "thin-provisioned blob"의 미할당 영역을 표현하기 위한
 * 가상 backing bs_dev (struct spdk_bs_dev)를 정의한다. thin blob에서 아직 한 번도
 * 쓰이지 않은 클러스터를 읽으면 0으로 채워진 데이터를 돌려줘야 하는데, 이를 위해
 * 실제 디스크 I/O를 수행하지 않고 메모리 버퍼를 zero-fill한 뒤 즉시 콜백을 호출하는
 * 단일 전역 디바이스(g_zeroes_bs_dev)를 제공한다. read/readv는 0을 채우고 즉시 성공으로
 * 완료시키며, write 계열 연산은 모두 -EPERM으로 실패시킨다 (zero device는 read-only).
 * is_zeroes()는 항상 true를 반환하여 상위 레이어가 CoW 등에서 0 확인을 단축할 수 있도록
 * 한다. 외부 메모리 도메인(GPU/RDMA 등)을 사용하는 경우 spdk_memory_domain_memzero()를
 * 통해 비-CPU 메모리에도 0을 쓸 수 있도록 분기한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   blob_create (lib/blob/blobstore.c) → bs_create_zeroes_dev() [thin blob 초기화 시]
 *                                       → blob->back_bs_dev = &g_zeroes_bs_dev
 *   blob_io_read (사용자 I/O) → blob_request_submit_op_split → bs_sequence_read_bs_dev
 *                            → back_bs_dev->readv (= zeroes_readv) → memset → 콜백
 * 실행 컨텍스트: 호스트 유저스페이스, blob 소유 SPDK thread/reactor 위에서만 호출된다.
 * polled-mode 환경에서는 콜백이 즉시 동기적으로 호출되므로 추가 폴링 라운드가 필요 없다.
 *
 * === 타 모듈과의 연결 ===
 * - blobstore.h: struct spdk_bs_dev / spdk_bs_dev_cb_args / spdk_blob_ext_io_opts
 *   정의를 가져온다. blob->back_bs_dev 슬롯에 g_zeroes_bs_dev 포인터가 들어간다.
 * - blob_bs_dev.c: blob을 backing dev로 노출하는 변환 레이어. zeroes_dev와는
 *   "back_bs_dev 인터페이스의 다른 구현체"라는 형제 관계다.
 * - spdk/dma.h (spdk_memory_domain_memzero): GPU·RDMA NIC 등의 외부 메모리 도메인에
 *   zero를 쓰기 위한 추상화 — readv_ext 경로에서만 사용.
 * - blob_backed_with_zeroes_dev(): blobstore.c가 "이 blob이 zero-backed인가"를
 *   판별할 때 호출 (thin blob 최적화 분기).
 * 데이터 흐름: 사용자가 thin blob의 미할당 영역을 읽으면 → blobstore가 back_bs_dev에
 * read를 위임 → zeroes_read/zeroes_readv가 페이로드 버퍼를 0으로 채움 → 사용자 콜백.
 *
 * === 주요 함수/구조체 요약 ===
 * - bs_create_zeroes_dev(): 전역 g_zeroes_bs_dev의 포인터를 반환 (싱글톤 팩토리).
 * - blob_backed_with_zeroes_dev(): blob의 backing이 zeroes 디바이스인지 식별.
 * - zeroes_read / zeroes_readv: payload 버퍼를 memset(0)으로 채우고 즉시 콜백 호출.
 * - zeroes_readv_ext: 외부 메모리 도메인 지원 readv (GPU 메모리 등에 zero 작성).
 * - zeroes_write/writev/write_zeroes/unmap: 모두 -EPERM (assert false) — 쓰기 불가.
 * - zeroes_is_zeroes / zeroes_is_range_valid: 무조건 true.
 * - zeroes_translate_lba: 무조건 false — 물리 LBA 매핑이 없음을 의미.
 * - g_zeroes_bs_dev: 전역 싱글톤 spdk_bs_dev 인스턴스 — 모든 thin blob이 공유.
 */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 라이브러리 래퍼 — POSIX 헤더(stdint, string, errno 등)를 한 번에 가져와
 * 빌드 환경(Linux/FreeBSD)별 차이를 흡수한다. memset, EPERM 등이 여기서 가시화된다. */
#include "spdk/blob.h"
/* [한국어] Blobstore 공개 API — struct spdk_bs_dev, spdk_bs_dev_cb_args,
 * spdk_blob_ext_io_opts 등 backing dev 인터페이스 정의를 사용한다. */
#include "spdk/dma.h"
/* [한국어] 외부 메모리 도메인 추상화 — spdk_memory_domain_memzero() 호출 시 필요.
 * GPU/RDMA NIC 메모리처럼 CPU에서 직접 memset 불가한 영역에 zero를 채우기 위해 사용. */

#include "blobstore.h"
/* [한국어] 내부 헤더 — bs_create_zeroes_dev() / blob_backed_with_zeroes_dev()의 선언과
 * struct spdk_blob 등 내부 구조체 접근을 위해 포함한다. */

/*
 * [한국어]
 * zeroes_destroy - zeroes 디바이스의 destroy 콜백 (no-op).
 *
 * @bs_dev: 파괴 대상 bs_dev (실제로는 전역 싱글톤이므로 사용되지 않음)
 *
 * 이 함수가 왜 필요한가: spdk_bs_dev 인터페이스는 destroy 콜백을 필수로 요구한다.
 * zeroes 디바이스는 전역 정적 변수(g_zeroes_bs_dev)이므로 해제할 자원이 없으나,
 * 인터페이스 일관성을 위해 빈 함수를 둔다. 만약 NULL 포인터를 두면 상위에서
 * 호출 시 segfault가 발생한다.
 *
 * 동작: 아무것도 하지 않고 즉시 반환한다. 메모리 해제·콜백 호출 모두 없음.
 * 실행 컨텍스트: blob_close 등 blob 정리 경로에서 호출 — 호출 스레드는 blob 소유 thread.
 *
 * 호출 체인:
 *   blob_close → blob->back_bs_dev->destroy() → zeroes_destroy()
 */
static void
zeroes_destroy(struct spdk_bs_dev *bs_dev)
{
	return;
	/* [한국어] 명시적 no-op — zeroes 디바이스는 전역 정적 객체이므로 free 대상이 없다. */
}

/*
 * [한국어]
 * zeroes_read - 페이로드 버퍼를 0으로 채워 read 요청을 완료시킨다.
 *
 * @dev: bs_dev (g_zeroes_bs_dev). dev->blocklen=512가 사용됨
 * @channel: I/O 채널 (zeroes는 채널을 사용하지 않으므로 무시됨)
 * @payload: 0으로 채울 사용자 버퍼 (호출자가 lba_count*blocklen 만큼 할당해 둔 상태)
 * @lba: 시작 LBA (zeroes는 무시 — 어차피 모든 영역이 0)
 * @lba_count: 채울 블록 수
 * @cb_args: 완료 콜백과 인자를 담은 구조체. cb_fn(channel, cb_arg, errno)로 완료 통지
 *
 * 이 함수가 왜 필요한가: thin-provisioned blob의 미할당 영역을 사용자가 읽을 때,
 * 실제 디스크에는 데이터가 없지만 0으로 채워진 결과를 돌려줘야 한다 (POSIX-like 시맨틱).
 * 동작: payload를 dev->blocklen * lba_count 바이트만큼 memset(0)으로 채운 뒤
 * 즉시(동기적으로) 콜백을 0(성공)과 함께 호출한다. 디스크 I/O가 발생하지 않으므로
 * polled-mode 환경에서도 추가 polling 없이 바로 완료된다.
 * 실행 컨텍스트: blob 소유 SPDK thread/reactor에서 호출된다.
 *
 * 호출 체인:
 *   bs_sequence_read_bs_dev → bs_dev->read (= zeroes_read) → cb_args->cb_fn
 */
static void
zeroes_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	    uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	memset(payload, 0, dev->blocklen * lba_count);
	/* [한국어] payload 전체를 0으로 채운다 — 총 바이트 수 = blocklen(512) * lba_count.
	 * 이것이 zero-backing의 전부: thin blob 미할당 영역의 read는 0을 반환해야 한다. */
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
	/* [한국어] 동기적으로 완료 콜백 호출 — 0은 성공(에러 없음)을 의미.
	 * cb_args->channel은 상위(블롭스토어 채널 등)이며, 이 콜백은 보통
	 * bs_sequence_completion으로 이어져 사용자 시퀀스를 진전시킨다. */
}

/*
 * [한국어]
 * zeroes_write - zeroes 디바이스에 대한 write 요청을 거부한다 (-EPERM).
 *
 * @dev/@channel/@payload/@lba/@lba_count: write 요청 인자들 (모두 무시됨)
 * @cb_args: 완료 콜백 — -EPERM(연산 미허용)으로 즉시 실패 통지
 *
 * 이 함수가 왜 필요한가: zeroes 디바이스는 read-only 추상화이며, 만약 누군가가
 * 여기에 write를 시도한다면 그것은 상위 로직 버그이다. thin-provisioned blob의
 * write는 클러스터 할당과 동시에 back_bs_dev가 zeroes에서 실제 백엔드로 교체되어야
 * 하므로, 정상 경로에서 zeroes_write에 도달할 일이 없다.
 * 동작: -EPERM(Operation not permitted)으로 콜백을 호출하고, 디버그 빌드에서는
 * assert(false)로 즉시 abort하여 버그를 조기에 노출시킨다.
 * 실행 컨텍스트: 정상 흐름에서는 절대 도달하지 않아야 하는 경로.
 */
static void
zeroes_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	     uint64_t lba, uint32_t lba_count,
	     struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	/* [한국어] 즉시 -EPERM(연산 미허용)으로 완료 통지 — 사용자 콜백에 에러 전파. */
	assert(false);
	/* [한국어] 디버그 빌드에서는 abort — 정상 경로에서는 절대 호출되지 않아야 함을 표명.
	 * 릴리스 빌드(NDEBUG)에서는 단순히 에러 반환만 일어난다. */
}

/*
 * [한국어]
 * zeroes_readv - 벡터(iov 배열) read 요청을 0으로 채워 완료시킨다.
 *
 * @dev: bs_dev (g_zeroes_bs_dev) — 사용되지 않음
 * @channel: I/O 채널 — 사용되지 않음
 * @iov: 사용자가 제공한 iovec 배열 (각 원소가 분산 버퍼 1개를 가리킴)
 * @iovcnt: iov 배열 원소 수
 * @lba/@lba_count: 무시됨 (zeroes는 LBA 위치와 무관하게 0을 반환)
 * @cb_args: 완료 콜백
 *
 * 이 함수가 왜 필요한가: scatter-gather read 경로 (예: zero-copy를 위해 NIC/디스크가
 * 바로 사용자 버퍼에 DMA하는 패턴)에서도 thin blob 미할당 영역은 0을 돌려줘야 한다.
 * 동작: iov 배열을 순회하며 각 원소의 iov_base를 iov_len 바이트만큼 memset(0).
 * 모두 끝나면 콜백을 0(성공)으로 호출.
 * 실행 컨텍스트: blob 소유 SPDK thread.
 */
static void
zeroes_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	     struct iovec *iov, int iovcnt,
	     uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int i;
	/* [한국어] iov 배열을 순회할 인덱스. */

	for (i = 0; i < iovcnt; i++) {
		/* [한국어] iov 배열의 모든 원소를 순서대로 처리. */
		memset(iov[i].iov_base, 0, iov[i].iov_len);
		/* [한국어] 각 분산 버퍼를 0으로 채운다. iov_base는 사용자 가상 주소이며,
		 * iov_len은 해당 세그먼트의 바이트 수 (LBA 정렬은 호출자가 보장). */
	}

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
	/* [한국어] 모든 세그먼트 zero-fill 완료 후 동기적으로 성공 콜백 호출. */
}

/*
 * [한국어]
 * zeroes_writev - 벡터 write 요청을 거부한다 (-EPERM).
 *
 * 이 함수의 모든 인자는 zeroes_write와 같은 의미이며, 동작도 동일 — 즉시 -EPERM과
 * assert(false)로 실패. zeroes는 read-only 추상화이므로 vectored write도 금지된다.
 */
static void
zeroes_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	      struct iovec *iov, int iovcnt,
	      uint64_t lba, uint32_t lba_count,
	      struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	/* [한국어] 연산 미허용 통지. */
	assert(false);
	/* [한국어] 정상 경로에서는 도달 불가 — 도달하면 상위 로직 버그. */
}

/*
 * [한국어]
 * _read_memory_domain_memzero_done - 외부 메모리 도메인 memzero 완료 콜백.
 *
 * @ctx: spdk_bs_dev_cb_args 포인터 (memzero 시작 시 cb_args를 그대로 전달)
 * @rc: spdk_memory_domain_memzero의 결과 (0=성공, <0=에러)
 *
 * 이 함수가 왜 필요한가: spdk_memory_domain_memzero()는 GPU/RDMA 메모리 등에 0을
 * 비동기적으로 쓰는 API이다. 그 비동기 완료 시점에 호출자(=상위 blobstore)에게
 * 결과를 전달해야 하므로 어댑터 콜백이 필요하다.
 * 동작: ctx를 spdk_bs_dev_cb_args로 캐스팅한 뒤 cb_fn(channel, cb_arg, rc) 호출.
 * 실행 컨텍스트: memory_domain 구현이 정한 컨텍스트(보통 호출자와 같은 SPDK thread).
 *
 * 호출 체인:
 *   zeroes_readv_ext → spdk_memory_domain_memzero → (비동기) → _read_memory_domain_memzero_done
 *                                                            → cb_args->cb_fn
 */
static void
_read_memory_domain_memzero_done(void *ctx, int rc)
{
	struct spdk_bs_dev_cb_args *cb_args = (struct spdk_bs_dev_cb_args *)ctx;
	/* [한국어] memzero 시작 시 ctx로 전달했던 cb_args 포인터를 복원.
	 * void* → 구조체 포인터 캐스팅 — SPDK 콜백 모델의 표준 패턴. */

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	/* [한국어] 상위 blobstore의 완료 콜백을 rc 그대로 전파.
	 * memzero가 성공이면 rc=0이 사용자에게 전달되어 read 성공으로 보인다. */
}

/*
 * [한국어]
 * zeroes_readv_ext - 외부 메모리 도메인 지원 readv (zero-fill 버전).
 *
 * @dev: bs_dev (g_zeroes_bs_dev) — 사용되지 않음
 * @channel: I/O 채널 — 사용되지 않음
 * @iov: 사용자 iovec 배열 (CPU 메모리 또는 외부 도메인 메모리를 가리킬 수 있음)
 * @iovcnt: iov 원소 수
 * @lba/@lba_count: 무시됨
 * @cb_args: 완료 콜백
 * @ext_io_opts: 외부 I/O 옵션 — memory_domain 필드가 NULL이 아니면 비-CPU 메모리 경로
 *
 * 이 함수가 왜 필요한가: SPDK의 ext I/O 경로는 GPU 메모리/RDMA NIC 메모리처럼
 * CPU에서 직접 접근 불가한 버퍼를 지원한다. iov_base가 그런 메모리라면 평범한
 * memset()으로는 안 되고 spdk_memory_domain_memzero()를 거쳐야 한다.
 * 동작:
 *   1) ext_io_opts->memory_domain이 설정되어 있으면 spdk_memory_domain_memzero()로
 *      비동기 zero-fill 요청 → 완료는 _read_memory_domain_memzero_done가 처리.
 *      만약 시작 자체가 실패(rc!=0)하면 즉시 에러 콜백.
 *   2) memory_domain이 NULL이면 일반 CPU 메모리이므로 직접 memset 루프.
 * 실행 컨텍스트: blob 소유 thread. memory_domain 경로는 비동기 완료 가능.
 */
static void
zeroes_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		 struct iovec *iov, int iovcnt,
		 uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args,
		 struct spdk_blob_ext_io_opts *ext_io_opts)
{
	int i, rc;
	/* [한국어] i = iov 순회 인덱스, rc = memzero API 반환 코드. */

	if (ext_io_opts->memory_domain) {
		/* [한국어] 외부 메모리 도메인 경로 — iov가 GPU/RDMA 메모리 등 CPU 직접
		 * 접근 불가한 영역을 가리킴. 도메인 드라이버에 zero 작업을 위임. */
		rc = spdk_memory_domain_memzero(ext_io_opts->memory_domain, ext_io_opts->memory_domain_ctx, iov,
						iovcnt, _read_memory_domain_memzero_done, cb_args);
		/* [한국어] 메모리 도메인에 zero-fill 비동기 요청.
		 * - memory_domain: 도메인 핸들 (어떤 종류의 메모리인지 식별)
		 * - memory_domain_ctx: 도메인 별 컨텍스트 (예: GPU stream)
		 * - iov/iovcnt: 채울 버퍼 목록
		 * - 완료 콜백/인자: _read_memory_domain_memzero_done(cb_args)
		 * 반환값: 0이면 비동기 시작 성공(콜백이 별도 호출됨), <0이면 시작 실패. */
		if (rc) {
			/* [한국어] 비동기 시작 자체 실패 — 콜백이 호출되지 않으므로 여기서 직접 에러 통지. */
			cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
		}
		return;
		/* [한국어] memory_domain 경로는 여기서 종료. 일반 memset 루프로 떨어지지 않도록. */
	}

	for (i = 0; i < iovcnt; i++) {
		/* [한국어] memory_domain이 없는 일반 CPU 메모리 경로 — 직접 memset. */
		memset(iov[i].iov_base, 0, iov[i].iov_len);
		/* [한국어] 각 iov 세그먼트를 0으로 채움 (zeroes_readv와 동일 로직). */
	}

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
	/* [한국어] CPU 경로는 동기 완료이므로 즉시 성공 콜백. */
}

/*
 * [한국어]
 * zeroes_writev_ext - 외부 메모리 도메인 지원 vectored write 거부.
 *
 * 동작은 zeroes_writev와 동일 — 즉시 -EPERM 반환 + assert(false). zeroes는
 * read-only이므로 ext 변형 write도 금지된다.
 */
static void
zeroes_writev_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		  struct iovec *iov, int iovcnt,
		  uint64_t lba, uint32_t lba_count,
		  struct spdk_bs_dev_cb_args *cb_args,
		  struct spdk_blob_ext_io_opts *ext_io_opts)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	/* [한국어] 연산 미허용으로 실패 통지. */
	assert(false);
	/* [한국어] 정상 경로에서는 도달 불가. */
}

/*
 * [한국어]
 * zeroes_write_zeroes - "특정 LBA 범위에 0을 쓰기" 명령 거부.
 *
 * 일반 블록 디바이스의 WRITE_ZEROES 명령(NVMe Write Zeroes opcode 0x08 등)에 대응되나,
 * zeroes 디바이스는 모든 영역이 이미 0이므로 의미가 없고, 또한 read-only이므로
 * 호출되어서는 안 된다. -EPERM + assert(false).
 */
static void
zeroes_write_zeroes(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		    uint64_t lba, uint64_t lba_count,
		    struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	/* [한국어] 연산 미허용 통지. */
	assert(false);
	/* [한국어] 도달 불가 표명. */
}

/*
 * [한국어]
 * zeroes_unmap - DEALLOCATE/UNMAP(TRIM) 명령 거부.
 *
 * NVMe Dataset Management의 Deallocate (opcode 0x09 with AD bit) 또는 SCSI UNMAP에
 * 해당하는 연산. zeroes 디바이스는 물리 매핑이 없으므로 unmap도 의미 없고 read-only.
 * -EPERM + assert(false).
 */
static void
zeroes_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	     uint64_t lba, uint64_t lba_count,
	     struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	/* [한국어] 연산 미허용 통지. */
	assert(false);
	/* [한국어] 도달 불가 표명. */
}

/*
 * [한국어]
 * zeroes_is_zeroes - 주어진 LBA 범위가 모두 0인지 질의 — 항상 true.
 *
 * @dev/@lba/@lba_count: 무시됨 — zeroes 디바이스는 정의상 모든 영역이 0
 * @return: 항상 true
 *
 * 이 함수가 왜 필요한가: blobstore CoW(Copy-on-Write) 경로에서 "원본이 모두 0이라면
 * 새 클러스터 할당 시 zero-fill 단계를 생략 가능"하다는 최적화가 있다.
 * is_zeroes() == true이면 상위 레이어가 그 최적화를 발동시킨다.
 */
static bool
zeroes_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	return true;
	/* [한국어] zeroes 디바이스는 정의상 모든 LBA가 0 — 어떤 범위 질의에도 true. */
}

/*
 * [한국어]
 * zeroes_is_range_valid - LBA 범위가 디바이스 경계 안에 있는지 — 항상 true.
 *
 * zeroes 디바이스는 blockcnt = UINT64_MAX이므로 모든 LBA 범위가 유효하다.
 * 상위 레이어(blob_bs_dev.c의 blob_bs_is_zeroes 등)가 backing dev의 경계를 확인할 때 사용.
 */
static bool
zeroes_is_range_valid(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	return true;
	/* [한국어] blockcnt=UINT64_MAX이므로 모든 범위가 유효. */
}

/*
 * [한국어]
 * zeroes_translate_lba - 가상 LBA → 백엔드 물리 LBA 변환 — 항상 false (매핑 없음).
 *
 * @dev/@lba: 무시됨
 * @base_lba: 변환 결과 출력 위치 (호출되더라도 false 반환 시 사용 안 됨)
 * @return: 항상 false — "이 디바이스는 물리적 LBA 매핑을 갖지 않음"
 *
 * 이 함수가 왜 필요한가: blobstore가 "이 LBA는 진짜 디스크의 어디로 가는가?"를
 * 묻는 경로에서 zeroes 디바이스는 가상이므로 변환이 불가능함을 신호한다.
 * 호출자는 이 false를 보고 "물리 매핑 없음"으로 처리한다.
 */
static bool
zeroes_translate_lba(struct spdk_bs_dev *dev, uint64_t lba, uint64_t *base_lba)
{
	return false;
	/* [한국어] zeroes 디바이스는 물리 LBA가 없음 — 변환 불가 의미로 false 반환. */
}

/*
 * [한국어]
 * g_zeroes_bs_dev - 전역 싱글톤 zero-backing 디바이스.
 *
 * 모든 thin-provisioned blob의 미할당 영역이 공유하는 단일 객체이다.
 * 정적 const-like 초기화로 함수 포인터 테이블 구성 — bs_create_zeroes_dev()는
 * 이 객체의 주소를 반환할 뿐이다.
 *
 * 필드 의미:
 * - blockcnt = UINT64_MAX: 무한대 크기로 표시 → is_range_valid가 모든 범위 허용 가능
 * - blocklen = 512: 가상 블록 크기 (실제 디스크 I/O는 안 일어나므로 임의값)
 * - create/destroy_channel = NULL: 채널 컨텍스트가 필요 없음 (상태 없음)
 * - read/readv/readv_ext: zero-fill 동작
 * - write/writev/writev_ext/write_zeroes/unmap: 모두 -EPERM
 * - is_zeroes/is_range_valid: true 상수
 * - translate_lba: false (물리 매핑 없음)
 *
 * 동기화: 이 객체는 함수 포인터만 담고 있고 mutable state가 없으므로 lock 불필요.
 * 모든 SPDK thread가 동시에 안전하게 사용 가능.
 */
static struct spdk_bs_dev g_zeroes_bs_dev = {
	.blockcnt = UINT64_MAX,
	/* [한국어] 가상 디바이스의 총 블록 수 — 무한대로 표시하여 어떤 LBA 범위에도
	 * is_range_valid가 true를 반환할 수 있게 한다. */
	.blocklen = 512,
	/* [한국어] 가상 블록 크기 (512B) — zero-fill 시 lba_count*blocklen 계산에 사용.
	 * 실제 디스크 I/O가 없으므로 정렬 외에 의미 있는 값은 아님. */
	.create_channel = NULL,
	/* [한국어] 채널 생성 콜백 없음 — 상태 없는 디바이스이므로 채널 컨텍스트 불필요. */
	.destroy_channel = NULL,
	/* [한국어] 채널 파괴 콜백 없음 — 위와 같은 이유. */
	.destroy = zeroes_destroy,
	/* [한국어] 디바이스 파괴 콜백 — 빈 함수 (전역 객체이므로 해제 없음). */
	.read = zeroes_read,
	/* [한국어] 단순 read — payload를 0으로 memset 후 즉시 콜백. */
	.write = zeroes_write,
	/* [한국어] 단순 write — -EPERM (read-only 디바이스). */
	.readv = zeroes_readv,
	/* [한국어] 벡터 read — iov 모든 세그먼트를 0으로 memset. */
	.writev = zeroes_writev,
	/* [한국어] 벡터 write — -EPERM. */
	.readv_ext = zeroes_readv_ext,
	/* [한국어] 외부 메모리 도메인 지원 readv — GPU/RDMA 메모리에도 zero 가능. */
	.writev_ext = zeroes_writev_ext,
	/* [한국어] 외부 도메인 vectored write — -EPERM. */
	.write_zeroes = zeroes_write_zeroes,
	/* [한국어] WRITE_ZEROES 명령 — -EPERM (이미 0인데다 read-only). */
	.unmap = zeroes_unmap,
	/* [한국어] UNMAP/TRIM 명령 — -EPERM (물리 매핑 없음 + read-only). */
	.is_zeroes = zeroes_is_zeroes,
	/* [한국어] 범위가 모두 0인지 질의 — 항상 true (CoW 최적화 경로에서 활용). */
	.is_range_valid = zeroes_is_range_valid,
	/* [한국어] LBA 범위가 디바이스 경계 안인지 — 항상 true (blockcnt=UINT64_MAX). */
	.translate_lba = zeroes_translate_lba,
	/* [한국어] 가상→물리 LBA 변환 — 항상 false (물리 LBA 없음). */
};

/*
 * [한국어]
 * bs_create_zeroes_dev - 전역 zeroes 디바이스 싱글톤 포인터 반환 (팩토리).
 *
 * @return: &g_zeroes_bs_dev (NULL 아님)
 *
 * 이 함수가 왜 필요한가: 이름은 "create"이지만 실제로는 메모리 할당 없이 전역 객체의
 * 주소만 돌려준다. 호출자(blobstore.c의 thin blob 초기화 경로)는 이 포인터를
 * blob->back_bs_dev에 저장하여 "미할당 영역 read 시 0을 반환"하는 동작을 얻는다.
 * 모든 thin blob이 같은 g_zeroes_bs_dev를 공유하므로 메모리 효율적이다.
 *
 * 동기화: 전역 객체는 mutable state가 없어 lock 없이 안전하게 공유.
 *
 * 호출 체인:
 *   blob_create / blob_open (thin blob) → bs_create_zeroes_dev() → blob->back_bs_dev 저장
 */
struct spdk_bs_dev *
bs_create_zeroes_dev(void)
{
	return &g_zeroes_bs_dev;
	/* [한국어] 싱글톤 포인터 반환 — 할당이 없으므로 destroy 시에도 free하지 않는다. */
}

/*
 * [한국어]
 * blob_backed_with_zeroes_dev - blob의 backing이 zeroes 싱글톤인지 식별.
 *
 * @blob: 검사 대상 blob (NULL 가능)
 * @return: blob != NULL && blob->back_bs_dev == &g_zeroes_bs_dev이면 true
 *
 * 이 함수가 왜 필요한가: blobstore.c의 여러 최적화 경로에서 "이 blob은 backing이
 * zero라서 별도 read 없이 0으로 처리 가능"한 분기를 결정해야 한다. 단순 포인터 비교로
 * 빠르게 판별 가능 (싱글톤이라 가능).
 *
 * 호출 체인:
 *   blobstore.c (CoW/snapshot 경로) → blob_backed_with_zeroes_dev → 최적화 분기
 */
bool
blob_backed_with_zeroes_dev(struct spdk_blob *blob)
{
	return blob != NULL && blob->back_bs_dev == &g_zeroes_bs_dev;
	/* [한국어] short-circuit AND — blob이 유효해야 하고, backing이 zeroes 싱글톤과
	 * 동일 포인터여야 true. 다른 zero-like 디바이스(예: 사용자 정의)는 false 반환. */
}
