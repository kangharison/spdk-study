/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iovec 헬퍼 함수 모음 (iov.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 전반에서 광범위하게 쓰이는 `struct iovec`(scatter-gather buffer)
 * 의 조작/순회/복사를 위한 공통 유틸리티를 제공한다. SPDK 의 bdev I/O 는
 * 단일 연속 버퍼가 아니라 다수의 iovec 으로 구성된 scatter-gather 형태로
 * 표현되며(spdk_bdev_io 의 u.bdev.iovs/iovcnt), 디바이스(NVMe PRP/SGL,
 * AIO, malloc, raid 등)와 상위 사용자(blob, fs, nvmf) 사이에서 길이가 다른
 * iovec 배열 간 복사·순회를 빈번하게 수행해야 한다. 본 파일은 그 핵심
 * 인프라(공통 iterator, 양방향 복사, 단일/이중 iov 비교 진행)를 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/util 의 leaf 유틸리티이며 thread-agnostic, stateless(상태는 호출자
 * 보유 iter/xfer 객체에만 존재). 호출자는 거의 모든 SPDK 서브시스템:
 *   bdev 코어/모듈 (lib/bdev, module/bdev/...) - bdev_io 의 iov 처리
 *   blobstore (lib/blob) - 블롭 R/W 시 사용자 iov ↔ cluster buffer 정렬
 *   nvmf (lib/nvmf) - 네트워크 PDU 페이로드 ↔ 호스트 iov 매핑
 *   fsdev/iSCSI/RPC - PDU/페이로드 처리
 * 따라서 본 파일은 데이터 이동 경로의 "공통 분모" 라 볼 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/util.h(공개 인터페이스 선언, struct spdk_ioviter 정의),
 *         spdk/log.h(에러 로그). 외부적으로는 표준 C 의 memcpy/memmove/memset.
 * - 사용처: bdev_io 분할/결합, NVMe-oF TCP/RDMA 의 페이로드 누적, blob R/W
 *           cluster 단위 처리, RPC 페이로드 직렬화 등.
 * - 데이터 흐름: scatter-gather 형태의 사용자 메모리(iovs[]) ↔ 단일 연속 버퍼(buf)
 *               또는 다른 iovs[] 사이의 양방향 복사. 본 파일은 어떤 메모리도
 *               할당/해제하지 않으며 호출자 메모리만 다룬다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_iov_memset      : 다중 iovec 전체를 단일 바이트값으로 채움(memset).
 * - spdk_ioviter_first/firstv: N개 iovec 배열에 대한 동기 진행 iterator 초기화.
 * - spdk_ioviter_next/nextv : iterator 의 다음 동기 청크(모든 배열에서 동일 길이)
 *                              를 반환. 가장 짧은 segment 에 맞춰 길이 정렬.
 * - spdk_iovcpy/iovmove  : 길이가 다른 src/dst iov 간 byte-단위 복사/이동.
 * - spdk_iov_xfer_init/from_buf/to_buf: 단일 연속 버퍼 ↔ iov 배열 간 복사를
 *                                       위한 단순 iterator (xfer 객체).
 * - spdk_copy_iovs_to_buf / spdk_copy_buf_to_iovs: xfer 의 1-shot 래퍼.
 * - struct spdk_ioviter / spdk_single_ioviter / spdk_iov_xfer (헤더에 정의):
 *   각 iovec 배열 내 현재 위치(idx, iov_base/iov_len 잔여 길이)를 보관하는
 *   상태 객체.
 */

/* [한국어] spdk_ioviter 등 iov 관련 공개 인터페이스 선언과 헬퍼 매크로. */
#include "spdk/util.h"
/* [한국어] 진단용 — 실제로 본 파일에서 SPDK_*LOG 가 직접 사용되지는 않지만,
 * util.h 가 의존하지 않는 로그 매크로가 향후 추가될 때를 대비해 포함. */
#include "spdk/log.h"

/*
 * [한국어]
 * spdk_iov_memset - iovec 배열의 모든 segment 를 동일 바이트값으로 채움.
 *
 * @iovs:   대상 iovec 배열.
 * @iovcnt: iovec 개수.
 * @c:      채울 바이트값(0~255). memset 과 동일한 의미.
 *
 * 사용 예: bdev I/O 의 read-zeroes 응답 시 사용자 버퍼를 0으로 채울 때,
 * 또는 secure-erase 류 동작에서 패턴을 일괄 적용할 때.
 *
 * 호출 체인: bdev/blob 등 → spdk_iov_memset → memset(libc)
 */
void
spdk_iov_memset(struct iovec *iovs, int iovcnt, int c)
{
	/* [한국어] 현재 처리 중인 iovec 의 인덱스(0부터 시작). */
	int iov_idx = 0;
	/* [한국어] 루프 내에서 현재 iovec 포인터를 보관하는 임시 변수. */
	struct iovec *iov;

	/* [한국어] 모든 segment 를 순회 — 각 segment 는 독립된 가상 메모리 블록. */
	while (iov_idx < iovcnt) {
		/* [한국어] 현재 segment 포인터 획득. */
		iov = &iovs[iov_idx];
		/* [한국어] 해당 segment 의 [iov_base, iov_base+iov_len) 를 c 로 채움. */
		memset(iov->iov_base, c, iov->iov_len);
		/* [한국어] 다음 segment 로 진행. */
		iov_idx++;
	}
}

/*
 * [한국어]
 * spdk_ioviter_first - 2-way iovec iterator 초기화 (src/dst 한 쌍).
 *
 * @iter:   호출자 보유 iterator 상태 객체.
 * @siov, @siovcnt: source iovec 배열과 개수.
 * @diov, @diovcnt: destination iovec 배열과 개수.
 * @src, @dst: 출력. 첫 동기 청크의 src/dst 주소 포인터(같은 길이).
 * @return: 첫 청크의 길이(바이트). 0 이면 둘 중 하나라도 비어 있음.
 *
 * 본 함수는 spdk_ioviter_firstv(2, ...) 의 편의 래퍼이다. iov_cpy/iovmove
 * 같은 두 배열 간 처리에서 가장 자주 쓰이는 패턴을 단순화해 준다.
 *
 * 호출 체인: spdk_iovcpy/iovmove 등 → spdk_ioviter_first → spdk_ioviter_firstv
 *           → spdk_ioviter_nextv (실제 길이 계산)
 */
size_t
spdk_ioviter_first(struct spdk_ioviter *iter,
		   struct iovec *siov, size_t siovcnt,
		   struct iovec *diov, size_t diovcnt,
		   void **src, void **dst)
{
	/* [한국어] firstv 가 받기 위한 배열 형태(2개) 로 모음. */
	struct iovec *iovs[2];
	size_t iovcnts[2];
	/* [한국어] firstv 가 채워줄 출력 포인터 슬롯(0=src, 1=dst). */
	void *out[2];
	/* [한국어] 첫 청크 길이 (둘 중 더 짧은 segment 에 맞춰짐). */
	size_t len;

	/* [한국어] 슬롯 0: source. */
	iovs[0] = siov;
	iovcnts[0] = siovcnt;

	/* [한국어] 슬롯 1: destination. */
	iovs[1] = diov;
	iovcnts[1] = diovcnt;

	/* [한국어] N=2 짜리 일반 firstv 호출 — out[]에 src/dst 가 채워짐. */
	len = spdk_ioviter_firstv(iter, 2, iovs, iovcnts, out);

	/* [한국어] 길이가 0 이면 한쪽이 비어있는 경우 — 출력 포인터는 의미 없음. */
	if (len > 0) {
		/* [한국어] 호출자가 받기 쉬운 형태로 풀어서 반환. */
		*src = out[0];
		*dst = out[1];
	}

	return len;
}

/*
 * [한국어]
 * spdk_ioviter_firstv - N-way iovec iterator 초기화 (일반화 버전).
 *
 * @iter:    호출자 보유 iterator.
 * @count:   동기 진행할 iovec 배열 개수 (N).
 * @iov:     iovec 배열들의 배열 (iov[i] 가 i번째 배열의 iovec[]).
 * @iovcnt:  각 iovec 배열의 segment 개수.
 * @out:     N 개의 첫 청크 시작 주소를 반환할 포인터 배열.
 * @return:  첫 청크의 길이(모두 동일). 0 이면 어느 하나라도 비어있음.
 *
 * iter 내부의 N 개 single iterator 를 각 배열의 [0]번 segment 위치로
 * 초기화한 뒤 곧바로 nextv 를 호출해 첫 동기 청크 길이를 계산한다.
 */
size_t
spdk_ioviter_firstv(struct spdk_ioviter *iter,
		    uint32_t count,
		    struct iovec **iov,
		    size_t *iovcnt,
		    void **out)
{
	/* [한국어] iter 내부 i번째 single iterator 를 가리킬 임시 변수. */
	struct spdk_single_ioviter *it;
	uint32_t i;

	/* [한국어] iterator 가 추적할 배열 개수 저장 — nextv 가 이 값만큼 순회. */
	iter->count = count;

	/* [한국어] N 개 single iterator 각각을 i번째 iovec 배열의 [0]번 segment 로 초기화. */
	for (i = 0; i < count; i++) {
		it = &iter->iters[i];
		/* [한국어] i 번째 iovec 배열 시작 포인터 저장. */
		it->iov = iov[i];
		/* [한국어] 해당 배열의 segment 총 개수 저장. */
		it->iovcnt = iovcnt[i];
		/* [한국어] 현재 segment 인덱스를 0 으로 초기화. */
		it->idx = 0;
		/* [한국어] 현재 segment 의 잔여 길이 = 초기에는 segment 전체 길이. */
		it->iov_len = iov[i][0].iov_len;
		/* [한국어] 현재 segment 의 잔여 시작 주소 = 초기에는 segment 베이스. */
		it->iov_base = iov[i][0].iov_base;
	}

	/* [한국어] 초기화 직후 nextv 를 호출해 "첫 번째" 청크 길이를 계산해 반환. */
	return spdk_ioviter_nextv(iter, out);
}

/*
 * [한국어]
 * spdk_ioviter_next - 2-way iterator 의 다음 동기 청크 진행.
 *
 * @iter, @src, @dst, @return: spdk_ioviter_first 와 동일한 의미.
 *
 * 본 함수는 spdk_ioviter_nextv(2, ...) 의 편의 래퍼.
 */
size_t
spdk_ioviter_next(struct spdk_ioviter *iter, void **src, void **dst)
{
	/* [한국어] nextv 출력 슬롯. */
	void *out[2];
	size_t len;

	/* [한국어] 일반화 nextv 호출 — N=iter->count(=2 이어야 함). */
	len = spdk_ioviter_nextv(iter, out);

	if (len > 0) {
		/* [한국어] 결과를 src/dst 로 풀어서 반환 — 호출자 코드 가독성 향상. */
		*src = out[0];
		*dst = out[1];
	}

	return len;
}

/*
 * [한국어]
 * spdk_ioviter_nextv - N-way iterator 의 다음 동기 청크 진행 (핵심 알고리즘).
 *
 * @iter: iterator.
 * @out:  N개 청크 시작 주소가 채워질 출력 포인터.
 * @return: 청크 길이(0 이면 어느 하나라도 끝남 → 종료).
 *
 * 알고리즘:
 *   1) 모든 single iterator 의 잔여 segment 길이 중 최솟값(len) 계산.
 *      → 모든 N개 배열에서 동시에 안전하게 사용할 수 있는 최대 청크 크기.
 *   2) 각 iterator 의 현재 잔여 시작주소를 out[i] 로 반환.
 *   3) iov_len 이 정확히 len 과 같으면 다음 segment 로 진행, 그렇지 않으면
 *      iov_base 를 +len, iov_len 을 -len 으로 부분 진행.
 *
 * 동시 실행 컨텍스트: 호출자 단일 스레드. iter 객체는 호출자 소유라
 * 동기화 불필요 (lock-free).
 */
size_t
spdk_ioviter_nextv(struct spdk_ioviter *iter, void **out)
{
	struct spdk_single_ioviter *it;
	size_t len;
	uint32_t i;

	/* Figure out the minimum size of each iovec's next segment */
	/* [한국어] 1단계: 모든 배열의 현재 segment 잔여 길이 중 최솟값을 구한다. */
	len = UINT32_MAX;
	for (i = 0; i < iter->count; i++) {
		it = &iter->iters[i];
		/* [한국어] 어느 한 배열이라도 끝(idx==iovcnt) 또는 잔여 0 이면 종료. */
		if (it->idx == it->iovcnt || it->iov_len == 0) {
			/* This element has 0 bytes remaining, so we're done. */
			return 0;
		}

		/* [한국어] 동기 청크 길이는 모든 배열의 잔여 길이의 최솟값으로 결정. */
		len = spdk_min(len, it->iov_len);
	}

	/* [한국어] 2-3단계: 각 iterator 에 대해 현재 청크 시작 주소를 출력하고, 진행. */
	for (i = 0; i < iter->count; i++) {
		it = &iter->iters[i];

		/* [한국어] 호출자에게 i 번째 배열의 현재 잔여 시작 주소를 알려줌. */
		out[i] = it->iov_base;

		if (it->iov_len == len) {
			/* Advance to next element */
			/* [한국어] 잔여 길이를 정확히 다 소비 → 다음 segment 로 이동. */
			it->idx++;
			if (it->idx != it->iovcnt) {
				/* Set up for next element */
				/* [한국어] 다음 segment 시작 주소/길이로 갱신. */
				it->iov_len = it->iov[it->idx].iov_len;
				it->iov_base = it->iov[it->idx].iov_base;
			}
			/* [한국어] (idx == iovcnt 이면 다음 nextv 호출 시 0 반환되어 종료) */
		} else {
			/* Partial buffer */
			/* [한국어] segment 일부만 소비 — base/len 을 부분 진행. */
			it->iov_base += len;
			it->iov_len -= len;
		}
	}

	return len;
}

/*
 * [한국어]
 * spdk_iovcpy - 길이가 다른 src/dst iovec 간 byte-단위 복사 (memcpy 의미).
 *
 * @siov, @siovcnt: source iov 배열.
 * @diov, @diovcnt: destination iov 배열.
 * @return: 실제로 복사된 총 바이트 수 (min(src_total_len, dst_total_len)).
 *
 * iter 를 사용해 src/dst 양쪽에서 동기 청크 단위로 진행하면서 memcpy.
 * src/dst 영역이 겹치는 경우 결과는 미정의(memcpy 와 동일 가정).
 */
size_t
spdk_iovcpy(struct iovec *siov, size_t siovcnt, struct iovec *diov, size_t diovcnt)
{
	/* [한국어] 호출자 stack 에 iterator 객체를 자동 변수로 둠 — 동시성 안전. */
	struct spdk_ioviter iter;
	size_t len, total_sz;
	void *src, *dst;

	/* [한국어] 누적 복사 바이트 수. */
	total_sz = 0;
	/* [한국어] for 루프 형태의 iterator 사용: first 로 초기 청크, 매회 next 로 진행. */
	for (len = spdk_ioviter_first(&iter, siov, siovcnt, diov, diovcnt, &src, &dst);
	     len != 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {
		/* [한국어] 현재 청크 만큼 byte-단위 복사 — 영역 비중첩 가정. */
		memcpy(dst, src, len);
		/* [한국어] 누적합 갱신. */
		total_sz += len;
	}

	return total_sz;
}

/*
 * [한국어]
 * spdk_iovmove - spdk_iovcpy 의 memmove 버전 (영역 중첩 허용).
 *
 * src/dst 영역이 겹쳐도 안전 — 실제 청크 단위 복사이므로 청크 내부에서만
 * 중첩되면 되며 memmove 가 그것을 보장한다.
 */
size_t
spdk_iovmove(struct iovec *siov, size_t siovcnt, struct iovec *diov, size_t diovcnt)
{
	struct spdk_ioviter iter;
	size_t len, total_sz;
	void *src, *dst;

	total_sz = 0;
	for (len = spdk_ioviter_first(&iter, siov, siovcnt, diov, diovcnt, &src, &dst);
	     len != 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {
		/* [한국어] memmove 사용 — src/dst 영역이 같은 객체 내에서 겹쳐도 OK. */
		memmove(dst, src, len);
		total_sz += len;
	}

	return total_sz;
}

/*
 * [한국어]
 * spdk_iov_xfer_init - 단일 iov 배열 ↔ 평면 buf 간 복사용 iterator 초기화.
 *
 * @ix:    호출자 보유 spdk_iov_xfer 상태 객체.
 * @iovs:  대상 iovec 배열.
 * @iovcnt: segment 개수.
 *
 * spdk_iov_xfer_from_buf / spdk_iov_xfer_to_buf 와 결합해 사용된다.
 * 한 번의 init 후 여러 번의 from_buf/to_buf 호출로 누적 진행 가능.
 */
void
spdk_iov_xfer_init(struct spdk_iov_xfer *ix, struct iovec *iovs, int iovcnt)
{
	/* [한국어] 대상 iov 배열 보관. */
	ix->iovs = iovs;
	ix->iovcnt = iovcnt;
	/* [한국어] 현재 처리 중인 segment 인덱스 — 시작은 0. */
	ix->cur_iov_idx = 0;
	/* [한국어] 현재 segment 내부의 byte 오프셋 — 시작은 0. */
	ix->cur_iov_offset = 0;
}

/*
 * [한국어]
 * iov_xfer (static) - iov 배열과 평면 buf 간 양방향 복사 핵심.
 *
 * @ix:      iterator (진행 상태가 갱신됨).
 * @buf:     평면 버퍼 (방향에 따라 read 또는 write 대상).
 * @buf_len: buf 길이(바이트).
 * @to_buf:  true 면 iov → buf, false 면 buf → iov 방향.
 * @return:  실제 이동된 바이트 수 (0~buf_len).
 *
 * 동작: 현재 segment 의 잔여 영역과 buf 의 잔여를 비교해 최소 길이만큼
 * memcpy 한 뒤 해당 segment 가 모두 소진되면 다음 segment 로 진행.
 */
static size_t
iov_xfer(struct spdk_iov_xfer *ix, const void *buf, size_t buf_len, bool to_buf)
{
	/* [한국어] 이번 청크 길이 / 현재 segment 의 잔여 길이 / 누적 이동 바이트. */
	size_t len, iov_remain_len, copied_len = 0;
	/* [한국어] 매 반복마다 현재 segment 포인터를 가리킬 임시 변수. */
	struct iovec *iov;

	/* [한국어] 0 길이 요청은 즉시 반환 — 무의미한 segment 진행 방지. */
	if (buf_len == 0) {
		return 0;
	}

	/* [한국어] 모든 segment 를 순회하며 buf 가 다 소비될 때까지 진행. */
	while (ix->cur_iov_idx < ix->iovcnt) {
		/* [한국어] 현재 처리 segment 포인터. */
		iov = &ix->iovs[ix->cur_iov_idx];
		/* [한국어] 현재 segment 잔여 길이 = (segment 길이 - 이미 처리한 오프셋). */
		iov_remain_len = iov->iov_len - ix->cur_iov_offset;
		if (iov_remain_len == 0) {
			/* [한국어] 현재 segment 가 0 이면 다음 segment 로 진행 후 재시도. */
			ix->cur_iov_idx++;
			ix->cur_iov_offset = 0;
			continue;
		}

		/* [한국어] 이번 청크 길이 = min(segment 잔여, buf 의 잔여). */
		len = spdk_min(iov_remain_len, buf_len - copied_len);

		if (to_buf) {
			/* [한국어] iov → buf 방향: iov(base+offset) 에서 읽어 buf(+copied) 에 쓰기. */
			memcpy((char *)buf + copied_len,
			       (char *)iov->iov_base + ix->cur_iov_offset, len);
		} else {
			/* [한국어] buf → iov 방향: buf(+copied) 에서 읽어 iov(base+offset) 에 쓰기. */
			memcpy((char *)iov->iov_base + ix->cur_iov_offset,
			       (const char *)buf + copied_len, len);
		}
		/* [한국어] 누적 이동량 갱신. */
		copied_len += len;
		/* [한국어] 현재 segment 내부 오프셋 진행 — 다음 호출에서 이어쓰기 가능. */
		ix->cur_iov_offset += len;

		/* [한국어] buf 측이 다 소진되었으면 더 진행할 필요 없음. */
		if (buf_len == copied_len) {
			return copied_len;
		}
	}

	/* [한국어] iov 측이 먼저 소진된 경우 — 부분 복사된 길이를 반환. */
	return copied_len;
}

/*
 * [한국어]
 * spdk_iov_xfer_from_buf - 평면 buf → iov 방향 (iov 측에 채우기).
 *
 * 사용 예: NVMe-oF TCP 가 네트워크에서 받은 PDU 페이로드를 호스트 iov 에 채울 때.
 */
size_t
spdk_iov_xfer_from_buf(struct spdk_iov_xfer *ix, const void *buf, size_t buf_len)
{
	/* [한국어] to_buf=false → buf 가 source, iov 가 destination. */
	return iov_xfer(ix, buf, buf_len, false);
}

/*
 * [한국어]
 * spdk_iov_xfer_to_buf - iov → 평면 buf 방향 (iov 에서 읽어내기).
 *
 * 사용 예: 호스트 iov 의 데이터를 단일 연속 PDU 페이로드 버퍼로 직렬화할 때.
 */
size_t
spdk_iov_xfer_to_buf(struct spdk_iov_xfer *ix, const void *buf, size_t buf_len)
{
	/* [한국어] to_buf=true → iov 가 source, buf 가 destination. */
	return iov_xfer(ix, buf, buf_len, true);
}

/*
 * [한국어]
 * spdk_copy_iovs_to_buf - iov → buf 1-shot 헬퍼.
 *
 * 내부적으로 xfer iter 를 임시 생성/초기화/사용한 뒤 폐기. 누적 진행이
 * 필요 없는 단발성 호출에 사용된다.
 */
void
spdk_copy_iovs_to_buf(void *buf, size_t buf_len, struct iovec *iovs, int iovcnt)
{
	/* [한국어] 자동 저장 영역(stack) 의 임시 iterator. */
	struct spdk_iov_xfer ix;

	/* [한국어] iter 를 대상 iov 로 초기화. */
	spdk_iov_xfer_init(&ix, iovs, iovcnt);
	/* [한국어] iov → buf 방향으로 buf_len 만큼 옮김. */
	spdk_iov_xfer_to_buf(&ix, buf, buf_len);
}

/*
 * [한국어]
 * spdk_copy_buf_to_iovs - buf → iov 1-shot 헬퍼.
 */
void
spdk_copy_buf_to_iovs(struct iovec *iovs, int iovcnt, void *buf, size_t buf_len)
{
	struct spdk_iov_xfer ix;

	/* [한국어] iter 를 대상 iov 로 초기화. */
	spdk_iov_xfer_init(&ix, iovs, iovcnt);
	/* [한국어] buf → iov 방향으로 buf_len 만큼 채워 넣음. */
	spdk_iov_xfer_from_buf(&ix, buf, buf_len);
}
