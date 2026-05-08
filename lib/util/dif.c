/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] T10 DIF (Data Integrity Field) / NVMe E2E Protection Information 처리 (dif.c)
 *
 * === 파일의 역할 ===
 * SCSI T10 DIF / NVMe E2E PI 보호 정보를 데이터 블록과 함께 처리(생성/검증/삽입/제거/리매핑)하는
 * 핵심 구현이다. T10 DIF는 각 블록(보통 512B/4096B) 끝에 8바이트의 PI 영역(또는 16바이트
 * 확장 PI)을 부착하여 데이터 무결성을 호스트-스토리지-디바이스 끝과 끝에서 보장한다. PI는
 * (1) Guard CRC(블록 데이터의 해시), (2) Application Tag(상위 응용용 자유 필드),
 * (3) Reference Tag(LBA와 연동된 식별자)로 구성된다.
 *
 * SPDK NVMe driver와 NVMe-oF target은 이 파일을 호출해 다음을 수행한다:
 *  - generate: 보호 없는 데이터에 PI를 계산해 부착(쓰기 경로)
 *  - verify: PI를 검사하여 무결성 확인(읽기 경로)
 *  - insert/strip(_copy): bdev 모듈 간 PI 추가/제거
 *  - update_crc32c: NVMe-oF에서 transport 보호용 추가 CRC32C 누적
 *  - remap_ref_tag: copy/clone 시 LBA가 바뀌면 reftag도 새 LBA에 맞춰 갱신
 *  - generate/verify_stream: 스트림 단위 처리(여러 호출에 걸친 누적)
 *  - DIX(Data Integrity Extension): metadata가 데이터 버퍼와 분리된 형태도 지원
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK util 라이브러리 → DIF/PI 처리 핵심.
 * 호출 체인:
 *   bdev_nvme/aio/etc → spdk_dif_ctx_init (블록/메타 크기, PI 형식 설정)
 *     → spdk_dif_generate / spdk_dif_verify / spdk_dif_*_copy / 등 (이 파일)
 *     → spdk_crc16_t10dif/spdk_crc32_ieee/spdk_crc64_nvme (lib/util/crc*.c)
 *     → 상위는 PI가 부착된 블록을 NVMe SQ에 PRACT/PRCHK 비트와 함께 제출
 * 실행 컨텍스트: 호스트 유저스페이스. 보통 SPDK reactor 스레드에서 동기 호출.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/dif.h(공개 API), spdk/crc16.h(T10 DIF CRC), spdk/crc32.h(64b Guard용
 *   alternative), spdk/crc64.h(NVMe 64b Guard용), spdk/endian.h(big-endian 변환:
 *   T10 PI 필드는 BE), spdk/log.h, spdk/util.h.
 * - 의존하는 자: lib/nvme(PRACT/PRCHK 처리), lib/nvmf(transport 검증, RDMA에서 추가 CRC32C),
 *   bdev_nvme/raid 등 PI를 다루는 모든 bdev 모듈.
 * - 데이터 흐름: 사용자 iovec(SGL) + dif_ctx → 블록 단위로 분할 → 각 블록에 대해
 *   guard/apptag/reftag 계산 → PI 영역(블록 끝 또는 메타 버퍼)에 기록 → 검증 시 역순.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_dif       : T10 PI/16b/32b/64b Guard 변형 union(8B 또는 16B 크기).
 * - struct _dif_sgl       : iovec 배열 위에서 바이트 단위 진행/생성을 추적하는 커서.
 * - spdk_dif_ctx_init      : 블록 크기, 메타 크기, PI 타입/형식, 초기 reftag/apptag 등 설정.
 * - spdk_dif_generate      : 보호 없는 블록에 PI를 계산해 끼워넣음(데이터 + 메타 인터리브).
 * - spdk_dif_verify        : 받은 블록의 PI를 재계산 비교, 불일치 시 err_blk에 위치/타입 기록.
 * - spdk_dif_*_copy        : src→dst 복사하면서 PI 추가/제거(메모리 한 번 패스).
 * - spdk_dif_inject_error  : 테스트용 PI 손상 주입.
 * - spdk_dif_update_crc32c : NVMe-oF RDMA 등에서 데이터 영역에 대한 추가 CRC32C 누적.
 * - spdk_dif_remap_ref_tag : 블록 이동 시 PI의 reference tag를 새 LBA에 맞춰 갱신.
 * - spdk_dix_*             : 데이터/메타가 분리된 DIX 변형(별도 메타 SGL 사용).
 *
 * === T10 DIF 핵심 개념 ===
 *  - Guard:  블록 데이터의 16비트 CRC(T10-CRC, 다항식 0x8BB7, 비반전).
 *            64b Guard는 NVMe 확장에서 CRC64-NVMe(0xAD93D23594C93659) 사용.
 *  - Apptag: 16비트 응용 태그. 0xFFFF는 "ignore"로 검증 면제.
 *  - Reftag: T10 Type 1/2/3에 따라 LBA와 연동(보통 LBA의 하위 32비트). NVMe 32b/64b
 *            확장 PI에서는 더 큰 비트 폭과 storage tag 결합 가능.
 *  - PI Format(NVMe): 16b Guard(8B PI), 32b Guard(16B PI), 64b Guard(16B PI).
 *  - 위치(dif_loc): 블록 끝에 두는 표준 위치 vs SCSI 일부에서의 시작부 위치.
 *  - md_interleave: 메타데이터를 블록 끝에 함께 배치하느냐, 별도 버퍼(DIX)로 두느냐.
 *
 * === 작업 진행 상황 ===
 * 이 파일(2604 라인)은 매우 크다. 본 라운드에서는 파일 상단 블록과 핵심 자료구조의
 * 모든 필드 주석, 그리고 가장 중요한 공개 API 함수의 함수 단위 주석을 작성한다.
 * 내부 헬퍼 함수(_dif_*_split, dif_generate, dif_verify, _dif_set_*, _dif_get_* 등)와
 * 모든 코드 라인의 인라인 주석은 다음 라운드에서 추가한다.
 */

#include "spdk/dif.h"
/* [한국어] 공개 API 선언(spdk_dif_ctx, spdk_dif_generate 등). */
#include "spdk/crc16.h"
/* [한국어] T10 DIF 16비트 Guard CRC 계산용. */
#include "spdk/crc32.h"
/* [한국어] NVMe-oF transport용 추가 CRC32C 누적용. */
#include "spdk/crc64.h"
/* [한국어] NVMe 64비트 Guard용 CRC64-NVMe. */
#include "spdk/endian.h"
/* [한국어] T10 PI 필드는 big-endian으로 직렬화 - to_be16/be64toh 등 사용. */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG 등. */
#include "spdk/util.h"
/* [한국어] SPDK_SIZEOF_MEMBER, spdk_min/max 등 매크로. */

#define REFTAG_MASK_16 0x00000000FFFFFFFF
/* [한국어] 16비트 Guard 형식의 reference tag 마스크 = 32비트(T10 Type 1 reftag 폭). */
#define REFTAG_MASK_32 0xFFFFFFFFFFFFFFFF
/* [한국어] 32비트 Guard 형식의 reference tag 마스크 = 64비트(NVMe 32b PI 확장). */
#define REFTAG_MASK_64 0x0000FFFFFFFFFFFF
/* [한국어] 64비트 Guard 형식의 reference tag 마스크 = 48비트(NVMe 64b PI 확장 - 6바이트). */

/* The variable size Storage Tag and Reference Tag is not supported yet,
 * so the maximum size of the Reference Tag is assumed.
 */
/* [한국어] T10/NVMe PI는 Guard 폭(16/32/64비트)에 따라 PI 영역 구조가 달라진다.
 * 이 union은 세 형식 모두를 표현하기 위한 일종의 "view" - 사용자는 ctx의 dif_pi_format에
 * 따라 g16/g32/g64 중 하나만 접근한다.
 * 동기화: 한 spdk_dif는 단일 블록에 속하므로 단일 스레드 액세스 가정.
 * 메모리 표현: 모든 필드는 빅엔디안으로 메모리에 저장(T10 스펙) - to_be*/be*toh로 변환. */
struct spdk_dif {
	union {
		struct {
			/* [한국어] 16비트 Guard PI: 8바이트 (T10 표준 PI). */
			uint16_t guard;
			/* [한국어] 블록 데이터의 16비트 T10 CRC.
			 * 설정자: _dif_set_guard, _dif_generate.
			 * 읽는 자: _dif_get_guard, _dif_verify.
			 * 값 범위: 16비트 전체 - "ignore" 의미 별도 없음(apptag와 다름). */
			uint16_t app_tag;
			/* [한국어] 응용 태그(자유 사용 16비트).
			 * 0xFFFF는 검증 시 "ignore" 의미로 해석되는 관례.
			 * 설정자: _dif_set_apptag.
			 * 읽는 자: _dif_get_apptag, apptag_check. */
			uint32_t stor_ref_space;
			/* [한국어] 32비트 reference tag 영역(T10 Type 1/2/3에서 의미가 달라짐).
			 * Type 1: LBA의 하위 32비트와 매치되어야 함.
			 * Type 2/3: 사용자 정의 또는 제약 다름.
			 * 설정자: _dif_set_reftag. 읽는 자: _dif_get_reftag, reftag_check. */
		} g16;
		struct {
			/* [한국어] 32비트 Guard PI: 16바이트 (NVMe 확장 PI 32b Guard 형식). */
			uint32_t guard;
			/* [한국어] 32비트 CRC. 더 큰 검출률 제공. */
			uint16_t app_tag;
			/* [한국어] 16비트 응용 태그. */
			uint16_t stor_ref_space_p1;
			/* [한국어] reference/storage tag 영역의 첫 16비트. */
			uint64_t stor_ref_space_p2;
			/* [한국어] reference/storage tag 영역의 나머지 64비트. p1+p2 = 80비트.
			 * REFTAG_MASK_32(=64비트)에 따라 storage tag와 분할된다. */
		} g32;
		struct {
			/* [한국어] 64비트 Guard PI: 16바이트 (NVMe 64b Guard 형식). */
			uint64_t guard;
			/* [한국어] 64비트 CRC(CRC64-NVMe). 큰 블록 시 더 강한 무결성 보장. */
			uint16_t app_tag;
			/* [한국어] 16비트 응용 태그. */
			uint16_t stor_ref_space_p1;
			/* [한국어] reference/storage tag의 첫 16비트. */
			uint32_t stor_ref_space_p2;
			/* [한국어] reference/storage tag의 다음 32비트. p1+p2 = 48비트(REFTAG_MASK_64). */
		} g64;
	};
};
SPDK_STATIC_ASSERT(SPDK_SIZEOF_MEMBER(struct spdk_dif, g16) == 8, "Incorrect size");
/* [한국어] g16은 정확히 8B(T10 표준 PI 크기) - 컴파일 타임 검증. */
SPDK_STATIC_ASSERT(SPDK_SIZEOF_MEMBER(struct spdk_dif, g32) == 16, "Incorrect size");
/* [한국어] g32는 정확히 16B(NVMe 32b Guard 확장 PI 크기). */
SPDK_STATIC_ASSERT(SPDK_SIZEOF_MEMBER(struct spdk_dif, g64) == 16, "Incorrect size");
/* [한국어] g64는 정확히 16B(NVMe 64b Guard 확장 PI 크기). */

/* Context to iterate or create a iovec array.
 * Each sgl is either iterated or created at a time.
 */
/* [한국어] iovec(scatter-gather list) 위에서 바이트 단위 커서를 유지하는 내부 컨텍스트.
 * generate/verify는 입력/출력 SGL을 블록 경계가 아닌 바이트 단위로 다루므로
 * 매 블록 처리 후 정확한 양만큼 advance해야 한다. */
struct _dif_sgl {
	/* Current iovec in the iteration or creation */
	struct iovec *iov;
	/* [한국어] 현재 iovec 포인터. iov_offset과 함께 위치 결정.
	 * 설정자: _dif_sgl_init, _dif_sgl_advance.
	 * 읽는 자: _dif_sgl_get_buf 등. */

	/* Remaining count of iovecs in the iteration or creation. */
	int iovcnt;
	/* [한국어] 남은 iovec 수. 0이면 SGL 끝.
	 * 설정자: _dif_sgl_init, _dif_sgl_advance. */

	/* Current offset in the iovec */
	uint32_t iov_offset;
	/* [한국어] 현재 iovec 안에서의 바이트 오프셋(0 .. iov_len-1).
	 * 설정자: _dif_sgl_advance. iov_len에 도달하면 다음 iov로 이동. */

	/* Size of the created iovec array in bytes */
	uint32_t total_size;
	/* [한국어] _dif_sgl_append로 누적된 총 바이트(SGL 생성 모드용).
	 * 검증 모드에서는 사용되지 않음. */
};

static inline void
_dif_sgl_init(struct _dif_sgl *s, struct iovec *iovs, int iovcnt)
{
	s->iov = iovs;
	s->iovcnt = iovcnt;
	s->iov_offset = 0;
	s->total_size = 0;
}

static void
_dif_sgl_advance(struct _dif_sgl *s, uint32_t step)
{
	s->iov_offset += step;
	while (s->iovcnt != 0) {
		if (s->iov_offset < s->iov->iov_len) {
			break;
		}

		s->iov_offset -= s->iov->iov_len;
		s->iov++;
		s->iovcnt--;
	}
}

static inline void
_dif_sgl_get_buf(struct _dif_sgl *s, uint8_t **_buf, uint32_t *_buf_len)
{
	if (_buf != NULL) {
		*_buf = (uint8_t *)s->iov->iov_base + s->iov_offset;
	}
	if (_buf_len != NULL) {
		*_buf_len = s->iov->iov_len - s->iov_offset;
	}
}

static inline bool
_dif_sgl_append(struct _dif_sgl *s, uint8_t *data, uint32_t data_len)
{
	assert(s->iovcnt > 0);
	s->iov->iov_base = data;
	s->iov->iov_len = data_len;
	s->total_size += data_len;
	s->iov++;
	s->iovcnt--;

	if (s->iovcnt > 0) {
		return true;
	} else {
		return false;
	}
}

static inline bool
_dif_sgl_append_split(struct _dif_sgl *dst, struct _dif_sgl *src, uint32_t data_len)
{
	uint8_t *buf;
	uint32_t buf_len;

	while (data_len != 0) {
		_dif_sgl_get_buf(src, &buf, &buf_len);
		buf_len = spdk_min(buf_len, data_len);

		if (!_dif_sgl_append(dst, buf, buf_len)) {
			return false;
		}

		_dif_sgl_advance(src, buf_len);
		data_len -= buf_len;
	}

	return true;
}

/* This function must be used before starting iteration. */
static bool
_dif_sgl_is_bytes_multiple(struct _dif_sgl *s, uint32_t bytes)
{
	int i;

	for (i = 0; i < s->iovcnt; i++) {
		if (s->iov[i].iov_len % bytes) {
			return false;
		}
	}

	return true;
}

/* This function must be used before starting iteration. */
static bool
_dif_sgl_is_valid(struct _dif_sgl *s, uint32_t bytes)
{
	uint64_t total = 0;
	int i;

	for (i = 0; i < s->iovcnt; i++) {
		total += s->iov[i].iov_len;
	}

	return total >= bytes;
}

static void
_dif_sgl_copy(struct _dif_sgl *to, struct _dif_sgl *from)
{
	memcpy(to, from, sizeof(struct _dif_sgl));
}

static bool
_dif_is_disabled(enum spdk_dif_type dif_type)
{
	if (dif_type == SPDK_DIF_DISABLE) {
		return true;
	} else {
		return false;
	}
}

static inline size_t
_dif_size(enum spdk_dif_pi_format dif_pi_format)
{
	uint8_t size;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g16);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g32);
	} else {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g64);
	}

	return size;
}

uint32_t
spdk_dif_pi_format_get_size(enum spdk_dif_pi_format dif_pi_format)
{
	return _dif_size(dif_pi_format);
}

static uint32_t
_get_guard_interval(uint32_t block_size, uint32_t md_size, bool dif_loc, bool md_interleave,
		    size_t dif_size)
{
	if (!dif_loc) {
		/* For metadata formats with more than 8/16 bytes (depending on
		 * the PI format), if the DIF is contained in the last 8/16 bytes
		 * of metadata, then the CRC covers all metadata up to but excluding
		 * these last 8/16 bytes.
		 */
		if (md_interleave) {
			return block_size - dif_size;
		} else {
			return md_size - dif_size;
		}
	} else {
		/* For metadata formats with more than 8/16 bytes (depending on
		 * the PI format), if the DIF is contained in the first 8/16 bytes
		 * of metadata, then the CRC does not cover any metadata.
		 */
		if (md_interleave) {
			return block_size - md_size;
		} else {
			return 0;
		}
	}
}

static inline uint8_t
_dif_guard_size(enum spdk_dif_pi_format dif_pi_format)
{
	uint8_t size;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g16.guard);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g32.guard);
	} else {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g64.guard);
	}

	return size;
}

static inline void
_dif_set_guard(struct spdk_dif *dif, uint64_t guard, enum spdk_dif_pi_format dif_pi_format)
{
	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		to_be16(&(dif->g16.guard), (uint16_t)guard);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		to_be32(&(dif->g32.guard), (uint32_t)guard);
	} else {
		to_be64(&(dif->g64.guard), guard);
	}
}

static inline uint64_t
_dif_get_guard(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t guard;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		guard = (uint64_t)from_be16(&(dif->g16.guard));
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		guard = (uint64_t)from_be32(&(dif->g32.guard));
	} else {
		guard = from_be64(&(dif->g64.guard));
	}

	return guard;
}

static inline uint64_t
_dif_generate_guard(uint64_t guard_seed, void *buf, size_t buf_len,
		    enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t guard;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		guard = (uint64_t)spdk_crc16_t10dif((uint16_t)guard_seed, buf, buf_len);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		guard = (uint64_t)spdk_crc32c_nvme(buf, buf_len, guard_seed);
	} else {
		guard = spdk_crc64_nvme(buf, buf_len, guard_seed);
	}

	return guard;
}

static uint64_t
dif_generate_guard_split(uint64_t guard_seed, struct _dif_sgl *sgl, uint32_t start,
			 uint32_t len, const struct spdk_dif_ctx *ctx)
{
	uint64_t guard = guard_seed;
	uint32_t offset, end, buf_len;
	uint8_t *buf;

	offset = start;
	end = start + spdk_min(len, ctx->guard_interval - start);

	while (offset < end) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, end - offset);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(guard, buf, buf_len, ctx->dif_pi_format);
		}

		_dif_sgl_advance(sgl, buf_len);
		offset += buf_len;
	}

	return guard;
}

static inline uint64_t
_dif_generate_guard_copy(uint64_t guard_seed, void *dst, void *src, size_t buf_len,
			 enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t guard;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		guard = (uint64_t)spdk_crc16_t10dif_copy((uint16_t)guard_seed, dst, src, buf_len);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		memcpy(dst, src, buf_len);
		guard = (uint64_t)spdk_crc32c_nvme(src, buf_len, guard_seed);
	} else {
		memcpy(dst, src, buf_len);
		guard = spdk_crc64_nvme(src, buf_len, guard_seed);
	}

	return guard;
}

static uint64_t
_dif_generate_guard_copy_split(uint64_t guard, struct _dif_sgl *dst_sgl,
			       struct _dif_sgl *src_sgl, uint32_t data_len,
			       enum spdk_dif_pi_format dif_pi_format)
{
	uint32_t offset = 0, src_len, dst_len, buf_len;
	uint8_t *src, *dst;

	while (offset < data_len) {
		_dif_sgl_get_buf(src_sgl, &src, &src_len);
		_dif_sgl_get_buf(dst_sgl, &dst, &dst_len);
		buf_len = spdk_min(src_len, dst_len);
		buf_len = spdk_min(buf_len, data_len - offset);

		guard = _dif_generate_guard_copy(guard, dst, src, buf_len, dif_pi_format);

		_dif_sgl_advance(src_sgl, buf_len);
		_dif_sgl_advance(dst_sgl, buf_len);
		offset += buf_len;
	}

	return guard;
}

static void
_data_copy_split(struct _dif_sgl *dst_sgl, struct _dif_sgl *src_sgl, uint32_t data_len)
{
	uint32_t offset = 0, src_len, dst_len, buf_len;
	uint8_t *src, *dst;

	while (offset < data_len) {
		_dif_sgl_get_buf(src_sgl, &src, &src_len);
		_dif_sgl_get_buf(dst_sgl, &dst, &dst_len);
		buf_len = spdk_min(src_len, dst_len);
		buf_len = spdk_min(buf_len, data_len - offset);

		memcpy(dst, src, buf_len);

		_dif_sgl_advance(src_sgl, buf_len);
		_dif_sgl_advance(dst_sgl, buf_len);
		offset += buf_len;
	}
}

static inline uint8_t
_dif_apptag_offset(enum spdk_dif_pi_format dif_pi_format)
{
	return _dif_guard_size(dif_pi_format);
}

static inline uint8_t
_dif_apptag_size(void)
{
	return SPDK_SIZEOF_MEMBER(struct spdk_dif, g16.app_tag);
}

static inline void
_dif_set_apptag(struct spdk_dif *dif, uint16_t app_tag, enum spdk_dif_pi_format dif_pi_format)
{
	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		to_be16(&(dif->g16.app_tag), app_tag);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		to_be16(&(dif->g32.app_tag), app_tag);
	} else {
		to_be16(&(dif->g64.app_tag), app_tag);
	}
}

static inline uint16_t
_dif_get_apptag(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	uint16_t app_tag;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		app_tag = from_be16(&(dif->g16.app_tag));
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		app_tag = from_be16(&(dif->g32.app_tag));
	} else {
		app_tag = from_be16(&(dif->g64.app_tag));
	}

	return app_tag;
}

static inline bool
_dif_apptag_ignore(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	return _dif_get_apptag(dif, dif_pi_format) == SPDK_DIF_APPTAG_IGNORE;
}

static inline uint8_t
_dif_reftag_offset(enum spdk_dif_pi_format dif_pi_format)
{
	uint8_t offset;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		offset = _dif_apptag_offset(dif_pi_format) + _dif_apptag_size();
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		offset = _dif_apptag_offset(dif_pi_format) + _dif_apptag_size()
			 + SPDK_SIZEOF_MEMBER(struct spdk_dif, g32.stor_ref_space_p1);
	} else {
		offset = _dif_apptag_offset(dif_pi_format) + _dif_apptag_size();
	}

	return offset;
}

static inline uint8_t
_dif_reftag_size(enum spdk_dif_pi_format dif_pi_format)
{
	uint8_t size;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g16.stor_ref_space);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g32.stor_ref_space_p2);
	} else {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g64.stor_ref_space_p1) +
		       SPDK_SIZEOF_MEMBER(struct spdk_dif, g64.stor_ref_space_p2);
	}

	return size;
}

static inline void
_dif_set_reftag(struct spdk_dif *dif, uint64_t ref_tag, enum spdk_dif_pi_format dif_pi_format)
{
	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		to_be32(&(dif->g16.stor_ref_space), (uint32_t)ref_tag);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		to_be64(&(dif->g32.stor_ref_space_p2), ref_tag);
	} else {
		to_be16(&(dif->g64.stor_ref_space_p1), (uint16_t)(ref_tag >> 32));
		to_be32(&(dif->g64.stor_ref_space_p2), (uint32_t)ref_tag);
	}
}

static inline uint64_t
_dif_get_reftag(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t ref_tag;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		ref_tag = (uint64_t)from_be32(&(dif->g16.stor_ref_space));
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		ref_tag = from_be64(&(dif->g32.stor_ref_space_p2));
	} else {
		ref_tag = (uint64_t)from_be16(&(dif->g64.stor_ref_space_p1));
		ref_tag <<= 32;
		ref_tag |= (uint64_t)from_be32(&(dif->g64.stor_ref_space_p2));
	}

	return ref_tag;
}

static inline bool
_dif_reftag_match(struct spdk_dif *dif, uint64_t ref_tag,
		  enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t _ref_tag;
	bool match;

	_ref_tag = _dif_get_reftag(dif, dif_pi_format);

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		match = (_ref_tag == (ref_tag & REFTAG_MASK_16));
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		match = (_ref_tag == ref_tag);
	} else {
		match = (_ref_tag == (ref_tag & REFTAG_MASK_64));
	}

	return match;
}

static inline bool
_dif_reftag_ignore(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	return _dif_reftag_match(dif, REFTAG_MASK_32, dif_pi_format);
}

static bool
_dif_ignore(struct spdk_dif *dif, const struct spdk_dif_ctx *ctx)
{
	switch (ctx->dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		/* If Type 1 or 2 is used, then all DIF checks are disabled when
		 * the Application Tag is 0xFFFF.
		 */
		if (_dif_apptag_ignore(dif, ctx->dif_pi_format)) {
			return true;
		}
		break;
	case SPDK_DIF_TYPE3:
		/* If Type 3 is used, then all DIF checks are disabled when the
		 * Application Tag is 0xFFFF and the Reference Tag is 0xFFFFFFFF
		 * or 0xFFFFFFFFFFFFFFFF depending on the PI format.
		 */

		if (_dif_apptag_ignore(dif, ctx->dif_pi_format) &&
		    _dif_reftag_ignore(dif, ctx->dif_pi_format)) {
			return true;
		}
		break;
	default:
		break;
	}

	return false;
}

static bool
_dif_pi_format_is_valid(enum spdk_dif_pi_format dif_pi_format)
{
	switch (dif_pi_format) {
	case SPDK_DIF_PI_FORMAT_16:
	case SPDK_DIF_PI_FORMAT_32:
	case SPDK_DIF_PI_FORMAT_64:
		return true;
	default:
		return false;
	}
}

static bool
_dif_type_is_valid(enum spdk_dif_type dif_type)
{
	switch (dif_type) {
	case SPDK_DIF_DISABLE:
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
	case SPDK_DIF_TYPE3:
		return true;
	default:
		return false;
	}
}

/*
 * [한국어]
 * spdk_dif_ctx_init - DIF/PI 처리에 필요한 모든 파라미터를 ctx에 설정한다.
 *
 * @ctx:           [out] 초기화 대상 컨텍스트.
 * @block_size:    한 블록의 총 크기(데이터+메타). 보통 520, 528, 4104, 4160 등.
 * @md_size:       블록당 메타데이터 크기(8 또는 16 등).
 * @md_interleave: true=메타가 블록 끝에 인터리브(LBA format A 등),
 *                 false=메타가 별도 버퍼(DIX 모드, NVMe MPTR).
 * @dif_loc:       PI 위치 - false=메타 끝(표준), true=메타 시작부(SCSI 일부).
 * @dif_type:      SPDK_DIF_DISABLE/TYPE1/TYPE2/TYPE3 (T10 PI 타입).
 * @dif_flags:     검사 활성 비트(GUARD/APPTAG/REFTAG 검사 활성).
 * @init_ref_tag:  reference tag 초기값(보통 시작 LBA의 하위 32비트).
 * @apptag_mask:   apptag 검사 시 적용할 마스크.
 * @app_tag:       기대되는 apptag 값.
 * @data_offset:   data_block_size 단위 오프셋(스트림 모드에서 진행 위치).
 * @guard_seed:    Guard CRC 초기값(보통 0).
 * @opts:          확장 옵션(PI format = 16/32/64b Guard 등). NULL 시 16b 기본.
 * @return:        0 성공, -EINVAL 인자 오류.
 *
 * 호출 체인: bdev_nvme/raid 등 사용자 → [spdk_dif_ctx_init] → 이후 generate/verify에 전달.
 * 실행 컨텍스트: 단일 SPDK thread. 한 ctx는 한 I/O 처리에 묶여 다른 스레드와 공유 안 함.
 */
int
spdk_dif_ctx_init(struct spdk_dif_ctx *ctx, uint32_t block_size, uint32_t md_size,
		  bool md_interleave, bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
		  uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag,
		  uint32_t data_offset, uint64_t guard_seed, struct spdk_dif_ctx_init_ext_opts *opts)
{
	uint32_t data_block_size;
	enum spdk_dif_pi_format dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	if (opts != NULL) {
		if (!_dif_pi_format_is_valid(opts->dif_pi_format)) {
			SPDK_ERRLOG("No valid DIF PI format provided.\n");
			return -EINVAL;
		}

		dif_pi_format = opts->dif_pi_format;
	}

	if (!_dif_type_is_valid(dif_type)) {
		SPDK_ERRLOG("No valid DIF type was provided.\n");
		return -EINVAL;
	}

	if (md_size < _dif_size(dif_pi_format)) {
		SPDK_ERRLOG("Metadata size is smaller than DIF size.\n");
		return -EINVAL;
	}

	if (md_interleave) {
		if (block_size < md_size) {
			SPDK_ERRLOG("Block size is smaller than DIF size.\n");
			return -EINVAL;
		}
		data_block_size = block_size - md_size;
	} else {
		data_block_size = block_size;
	}

	if (data_block_size == 0) {
		SPDK_ERRLOG("Zero data block size is not allowed\n");
		return -EINVAL;
	}

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		if ((data_block_size % 512) != 0) {
			SPDK_ERRLOG("Data block size should be a multiple of 512B\n");
			return -EINVAL;
		}
	} else {
		if ((data_block_size % 4096) != 0) {
			SPDK_ERRLOG("Data block size should be a multiple of 4kB\n");
			return -EINVAL;
		}
	}

	ctx->block_size = block_size;
	ctx->md_size = md_size;
	ctx->md_interleave = md_interleave;
	ctx->dif_pi_format = dif_pi_format;
	ctx->guard_interval = _get_guard_interval(block_size, md_size, dif_loc, md_interleave,
			      _dif_size(ctx->dif_pi_format));
	ctx->dif_type = dif_type;
	ctx->dif_flags = dif_flags;
	ctx->init_ref_tag = init_ref_tag;
	ctx->apptag_mask = apptag_mask;
	ctx->app_tag = app_tag;
	ctx->data_offset = data_offset;
	ctx->ref_tag_offset = data_offset / data_block_size;
	ctx->last_guard = guard_seed;
	ctx->guard_seed = guard_seed;
	ctx->remapped_init_ref_tag = 0;

	return 0;
}

/*
 * [한국어]
 * spdk_dif_ctx_set_data_offset - 스트림 모드 진행 위치(data_offset) 갱신.
 *
 * 한 I/O를 여러 회차로 나눠 처리하는 경우(예: NVMe-oF에서 청크별 수신)에 사용.
 * data_offset / data_block_size = 처리한 블록 수 → ref_tag_offset 갱신.
 */
void
spdk_dif_ctx_set_data_offset(struct spdk_dif_ctx *ctx, uint32_t data_offset)
{
	uint32_t data_block_size;

	if (ctx->md_interleave) {
		data_block_size = ctx->block_size - ctx->md_size;
	} else {
		data_block_size = ctx->block_size;
	}

	ctx->data_offset = data_offset;
	ctx->ref_tag_offset = data_offset / data_block_size;
}

/*
 * [한국어]
 * spdk_dif_ctx_set_remapped_init_ref_tag - 블록 이동(remap) 시 새 시작 reftag 설정.
 *
 * spdk_dif_remap_ref_tag와 함께 사용. copy/clone 시 LBA가 바뀌면 PI의 reftag도
 * 새 LBA에 맞춰 갱신해야 한다.
 */
void
spdk_dif_ctx_set_remapped_init_ref_tag(struct spdk_dif_ctx *ctx,
				       uint32_t remapped_init_ref_tag)
{
	ctx->remapped_init_ref_tag = remapped_init_ref_tag;
}

static void
_dif_generate(void *_dif, uint64_t guard, uint32_t offset_blocks,
	      const struct spdk_dif_ctx *ctx)
{
	struct spdk_dif *dif = _dif;
	uint64_t ref_tag;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		_dif_set_guard(dif, guard, ctx->dif_pi_format);
	} else {
		_dif_set_guard(dif, 0, ctx->dif_pi_format);
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_APPTAG_CHECK) {
		_dif_set_apptag(dif, ctx->app_tag, ctx->dif_pi_format);
	} else {
		_dif_set_apptag(dif, 0, ctx->dif_pi_format);
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK) {
		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag
		 * remains the same as the initial reference tag.
		 */
		if (ctx->dif_type != SPDK_DIF_TYPE3) {
			ref_tag = ctx->init_ref_tag + ctx->ref_tag_offset + offset_blocks;
		} else {
			ref_tag = ctx->init_ref_tag + ctx->ref_tag_offset;
		}

		/* Overwrite reference tag if initialization reference tag is SPDK_DIF_REFTAG_IGNORE */
		if (ctx->init_ref_tag == SPDK_DIF_REFTAG_IGNORE) {
			if (ctx->dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
				ref_tag = REFTAG_MASK_16;
			} else if (ctx->dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
				ref_tag = REFTAG_MASK_32;
			} else {
				ref_tag = REFTAG_MASK_64;
			}
		}

		_dif_set_reftag(dif, ref_tag, ctx->dif_pi_format);
	} else {
		_dif_set_reftag(dif, 0, ctx->dif_pi_format);
	}
}

static void
dif_generate(struct _dif_sgl *sgl, uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;
	uint8_t *buf;
	uint64_t guard = 0;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_sgl_get_buf(sgl, &buf, NULL);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(ctx->guard_seed, buf, ctx->guard_interval, ctx->dif_pi_format);
		}

		_dif_generate(buf + ctx->guard_interval, guard, offset_blocks, ctx);

		_dif_sgl_advance(sgl, ctx->block_size);
	}
}

static void
dif_store_split(struct _dif_sgl *sgl, struct spdk_dif *dif,
		const struct spdk_dif_ctx *ctx)
{
	uint32_t offset = 0, rest_md_len, buf_len;
	uint8_t *buf;

	rest_md_len = ctx->block_size - ctx->guard_interval;

	while (offset < rest_md_len) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);

		if (offset < _dif_size(ctx->dif_pi_format)) {
			buf_len = spdk_min(buf_len, _dif_size(ctx->dif_pi_format) - offset);
			memcpy(buf, (uint8_t *)dif + offset, buf_len);
		} else {
			buf_len = spdk_min(buf_len, rest_md_len - offset);
		}

		_dif_sgl_advance(sgl, buf_len);
		offset += buf_len;
	}
}

static uint64_t
_dif_generate_split(struct _dif_sgl *sgl, uint32_t offset_in_block, uint32_t data_len,
		    uint64_t guard, uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	struct spdk_dif dif = {};

	assert(offset_in_block < ctx->guard_interval);
	assert(offset_in_block + data_len < ctx->guard_interval ||
	       offset_in_block + data_len == ctx->block_size);

	/* Compute CRC over split logical block data. */
	guard = dif_generate_guard_split(guard, sgl, offset_in_block, data_len, ctx);

	if (offset_in_block + data_len < ctx->guard_interval) {
		return guard;
	}

	/* If a whole logical block data is parsed, generate DIF
	 * and save it to the temporary DIF area.
	 */
	_dif_generate(&dif, guard, offset_blocks, ctx);

	/* Copy generated DIF field to the split DIF field, and then
	 * skip metadata field after DIF field (if any).
	 */
	dif_store_split(sgl, &dif, ctx);

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}

	return guard;
}

static void
dif_generate_split(struct _dif_sgl *sgl, uint32_t num_blocks,
		   const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;
	uint64_t guard = 0;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_generate_split(sgl, 0, ctx->block_size, guard, offset_blocks, ctx);
	}
}

/*
 * [한국어]
 * spdk_dif_generate - in-place로 블록의 PI 영역(메타 끝의 8/16B)을 채운다.
 *
 * @iovs/iovcnt: 메타가 인터리브된 블록들의 iovec 배열(데이터+메타 모두 한 버퍼).
 * @num_blocks:  처리할 블록 수.
 * @ctx:         spdk_dif_ctx_init으로 초기화된 컨텍스트.
 * @return:      0 성공, -EINVAL 인자 오류.
 *
 * 동작: 각 블록에 대해 데이터 영역에 대한 Guard CRC 산출 → reftag 계산(LBA 기반) →
 * apptag 적용 → PI 영역(big-endian 직렬화)에 기록. ctx->dif_flags의 비트로 어떤 필드를
 * 실제 채울지 선택. SGL이 단일 iovec이면 dif_generate, 분리 iovec이면 dif_generate_split 사용.
 *
 * 호출 체인: bdev_nvme write 경로 → [spdk_dif_generate] → NVMe SQ 제출.
 * 실행 컨텍스트: 단일 SPDK thread.
 */
int
spdk_dif_generate(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
		  const struct spdk_dif_ctx *ctx)
{
	struct _dif_sgl sgl;

	_dif_sgl_init(&sgl, iovs, iovcnt);

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(&sgl, ctx->block_size)) {
		dif_generate(&sgl, num_blocks, ctx);
	} else {
		dif_generate_split(&sgl, num_blocks, ctx);
	}

	return 0;
}

static void
_dif_error_set(struct spdk_dif_error *err_blk, uint8_t err_type,
	       uint64_t expected, uint64_t actual, uint32_t err_offset)
{
	if (err_blk) {
		err_blk->err_type = err_type;
		err_blk->expected = expected;
		err_blk->actual = actual;
		err_blk->err_offset = err_offset;
	}
}

static bool
_dif_reftag_check(struct spdk_dif *dif, const struct spdk_dif_ctx *ctx,
		  uint64_t expected_reftag, uint32_t offset_blocks, struct spdk_dif_error *err_blk)
{
	uint64_t reftag;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK) {
		switch (ctx->dif_type) {
		case SPDK_DIF_TYPE1:
		case SPDK_DIF_TYPE2:
			/* Compare the DIF Reference Tag field to the passed Reference Tag.
			 * The passed Reference Tag will be the least significant 4 bytes
			 * or 8 bytes (depending on the PI format)
			 * of the LBA when Type 1 is used, and application specific value
			 * if Type 2 is used.
			 */
			if (!_dif_reftag_match(dif, expected_reftag, ctx->dif_pi_format)) {
				reftag = _dif_get_reftag(dif, ctx->dif_pi_format);
				_dif_error_set(err_blk, SPDK_DIF_REFTAG_ERROR, expected_reftag,
					       reftag, offset_blocks);
				SPDK_ERRLOG("Failed to compare Ref Tag: LBA=%" PRIu64 "," \
					    " Expected=%lx, Actual=%lx\n",
					    expected_reftag, expected_reftag, reftag);
				return false;
			}
			break;
		case SPDK_DIF_TYPE3:
			/* For Type 3, computed Reference Tag remains unchanged.
			 * Hence ignore the Reference Tag field.
			 */
			break;
		default:
			break;
		}
	}

	return true;
}

static int
_dif_verify(void *_dif, uint64_t guard, uint32_t offset_blocks,
	    const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	struct spdk_dif *dif = _dif;
	uint64_t _guard;
	uint16_t _app_tag;
	uint64_t ref_tag;

	if (_dif_ignore(dif, ctx)) {
		return 0;
	}

	/* For type 1 and 2, the reference tag is incremented for each
	 * subsequent logical block. For type 3, the reference tag
	 * remains the same as the initial reference tag.
	 */
	if (ctx->dif_type != SPDK_DIF_TYPE3) {
		ref_tag = ctx->init_ref_tag + ctx->ref_tag_offset + offset_blocks;
	} else {
		ref_tag = ctx->init_ref_tag + ctx->ref_tag_offset;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		/* Compare the DIF Guard field to the CRC computed over the logical
		 * block data.
		 */
		_guard = _dif_get_guard(dif, ctx->dif_pi_format);
		if (_guard != guard) {
			_dif_error_set(err_blk, SPDK_DIF_GUARD_ERROR, _guard, guard,
				       offset_blocks);
			SPDK_ERRLOG("Failed to compare Guard: LBA=%" PRIu64 "," \
				    "  Expected=%lx, Actual=%lx\n",
				    ref_tag, _guard, guard);
			return -1;
		}
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_APPTAG_CHECK) {
		/* Compare unmasked bits in the DIF Application Tag field to the
		 * passed Application Tag.
		 */
		_app_tag = _dif_get_apptag(dif, ctx->dif_pi_format);
		if ((_app_tag & ctx->apptag_mask) != (ctx->app_tag & ctx->apptag_mask)) {
			_dif_error_set(err_blk, SPDK_DIF_APPTAG_ERROR, ctx->app_tag,
				       (_app_tag & ctx->apptag_mask), offset_blocks);
			SPDK_ERRLOG("Failed to compare App Tag: LBA=%" PRIu64 "," \
				    "  Expected=%x, Actual=%x\n",
				    ref_tag, ctx->app_tag, (_app_tag & ctx->apptag_mask));
			return -1;
		}
	}

	if (!_dif_reftag_check(dif, ctx, ref_tag, offset_blocks, err_blk)) {
		return -1;
	}

	return 0;
}

static int
dif_verify(struct _dif_sgl *sgl, uint32_t num_blocks,
	   const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks;
	int rc;
	uint8_t *buf;
	uint64_t guard = 0;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_sgl_get_buf(sgl, &buf, NULL);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(ctx->guard_seed, buf, ctx->guard_interval, ctx->dif_pi_format);
		}

		rc = _dif_verify(buf + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
		if (rc != 0) {
			return rc;
		}

		_dif_sgl_advance(sgl, ctx->block_size);
	}

	return 0;
}

static void
dif_load_split(struct _dif_sgl *sgl, struct spdk_dif *dif,
	       const struct spdk_dif_ctx *ctx)
{
	uint32_t offset = 0, rest_md_len, buf_len;
	uint8_t *buf;

	rest_md_len = ctx->block_size - ctx->guard_interval;

	while (offset < rest_md_len) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);

		if (offset < _dif_size(ctx->dif_pi_format)) {
			buf_len = spdk_min(buf_len, _dif_size(ctx->dif_pi_format) - offset);
			memcpy((uint8_t *)dif + offset, buf, buf_len);
		} else {
			buf_len = spdk_min(buf_len, rest_md_len - offset);
		}

		_dif_sgl_advance(sgl, buf_len);
		offset += buf_len;
	}
}

static int
_dif_verify_split(struct _dif_sgl *sgl, uint32_t offset_in_block, uint32_t data_len,
		  uint64_t *_guard, uint32_t offset_blocks,
		  const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	uint64_t guard = *_guard;
	struct spdk_dif dif = {};
	int rc;

	assert(_guard != NULL);
	assert(offset_in_block < ctx->guard_interval);
	assert(offset_in_block + data_len < ctx->guard_interval ||
	       offset_in_block + data_len == ctx->block_size);

	guard = dif_generate_guard_split(guard, sgl, offset_in_block, data_len, ctx);

	if (offset_in_block + data_len < ctx->guard_interval) {
		*_guard = guard;
		return 0;
	}

	dif_load_split(sgl, &dif, ctx);

	rc = _dif_verify(&dif, guard, offset_blocks, ctx, err_blk);
	if (rc != 0) {
		return rc;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}

	*_guard = guard;
	return 0;
}

static int
dif_verify_split(struct _dif_sgl *sgl, uint32_t num_blocks,
		 const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks;
	uint64_t guard = 0;
	int rc;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dif_verify_split(sgl, 0, ctx->block_size, &guard, offset_blocks,
				       ctx, err_blk);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

/*
 * [한국어]
 * spdk_dif_verify - 받은 블록의 PI 영역을 검증한다.
 *
 * @iovs/iovcnt:  메타가 인터리브된 입력 블록 SGL.
 * @num_blocks:   검증할 블록 수.
 * @ctx:          PI 형식/플래그/예상 reftag 등이 설정된 컨텍스트.
 * @err_blk:      [out] 첫 검증 실패 시 어느 블록의 어떤 필드가 어떤 값으로 잘못됐는지 기록.
 * @return:       0 성공, -EINVAL 인자 오류, -EIO 검증 실패(err_blk 참고).
 *
 * 동작: 각 블록에 대해 데이터 Guard CRC 재계산 → PI에서 읽은 guard와 비교 →
 * apptag 검사(0xFFFF는 ignore) → reftag 검사(LBA와 일치). 어느 하나라도 불일치면
 * 즉시 -EIO 반환하고 err_blk에 위치/타입/expected/actual 기록.
 *
 * 호출 체인: bdev_nvme read 완료 콜백 → [spdk_dif_verify] → 사용자 데이터 신뢰성 보장.
 * 실행 컨텍스트: 단일 SPDK thread.
 */
int
spdk_dif_verify(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
		const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	struct _dif_sgl sgl;

	_dif_sgl_init(&sgl, iovs, iovcnt);

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(&sgl, ctx->block_size)) {
		return dif_verify(&sgl, num_blocks, ctx, err_blk);
	} else {
		return dif_verify_split(&sgl, num_blocks, ctx, err_blk);
	}
}

static uint32_t
dif_update_crc32c(struct _dif_sgl *sgl, uint32_t num_blocks,
		  uint32_t crc32c,  const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;
	uint8_t *buf;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_sgl_get_buf(sgl, &buf, NULL);

		crc32c = spdk_crc32c_update(buf, ctx->block_size - ctx->md_size, crc32c);

		_dif_sgl_advance(sgl, ctx->block_size);
	}

	return crc32c;
}

static uint32_t
_dif_update_crc32c_split(struct _dif_sgl *sgl, uint32_t offset_in_block, uint32_t data_len,
			 uint32_t crc32c, const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size, buf_len;
	uint8_t *buf;

	data_block_size = ctx->block_size - ctx->md_size;

	assert(offset_in_block + data_len <= ctx->block_size);

	while (data_len != 0) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, data_len);

		if (offset_in_block < data_block_size) {
			buf_len = spdk_min(buf_len, data_block_size - offset_in_block);
			crc32c = spdk_crc32c_update(buf, buf_len, crc32c);
		}

		_dif_sgl_advance(sgl, buf_len);
		offset_in_block += buf_len;
		data_len -= buf_len;
	}

	return crc32c;
}

static uint32_t
dif_update_crc32c_split(struct _dif_sgl *sgl, uint32_t num_blocks,
			uint32_t crc32c, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		crc32c = _dif_update_crc32c_split(sgl, 0, ctx->block_size, crc32c, ctx);
	}

	return crc32c;
}

/*
 * [한국어]
 * spdk_dif_update_crc32c - 블록의 데이터 영역(메타 제외)에 대한 CRC32C 누적 갱신.
 *
 * @iovs/iovcnt:  블록 SGL.
 * @num_blocks:   처리할 블록 수.
 * @_crc32c:      [in/out] 누적 CRC32C. NULL 거부.
 * @ctx:          블록/메타 크기 정보를 가진 컨텍스트.
 * @return:       0 성공, -EINVAL 인자 오류.
 *
 * 사용처: NVMe-oF RDMA에서 호스트가 전송한 데이터의 transport-level CRC32C 검증.
 * PI 영역은 제외하고 데이터 영역만 누적해야 호스트의 계산과 일치한다.
 */
int
spdk_dif_update_crc32c(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
		       uint32_t *_crc32c, const struct spdk_dif_ctx *ctx)
{
	struct _dif_sgl sgl;

	if (_crc32c == NULL) {
		return -EINVAL;
	}

	_dif_sgl_init(&sgl, iovs, iovcnt);

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_sgl_is_bytes_multiple(&sgl, ctx->block_size)) {
		*_crc32c = dif_update_crc32c(&sgl, num_blocks, *_crc32c, ctx);
	} else {
		*_crc32c = dif_update_crc32c_split(&sgl, num_blocks, *_crc32c, ctx);
	}

	return 0;
}

static void
_dif_insert_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		 uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size;
	uint8_t *src, *dst;
	uint64_t guard = 0;

	data_block_size = ctx->block_size - ctx->md_size;

	_dif_sgl_get_buf(src_sgl, &src, NULL);
	_dif_sgl_get_buf(dst_sgl, &dst, NULL);

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard_copy(ctx->guard_seed, dst, src, data_block_size,
						 ctx->dif_pi_format);
		guard = _dif_generate_guard(guard, dst + data_block_size,
					    ctx->guard_interval - data_block_size, ctx->dif_pi_format);
	} else {
		memcpy(dst, src, data_block_size);
	}

	_dif_generate(dst + ctx->guard_interval, guard, offset_blocks, ctx);

	_dif_sgl_advance(src_sgl, data_block_size);
	_dif_sgl_advance(dst_sgl, ctx->block_size);
}

static void
dif_insert_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_insert_copy(src_sgl, dst_sgl, offset_blocks, ctx);
	}
}

static void
_dif_insert_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		       uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size;
	uint64_t guard = 0;
	struct spdk_dif dif = {};

	data_block_size = ctx->block_size - ctx->md_size;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard_copy_split(ctx->guard_seed, dst_sgl, src_sgl,
						       data_block_size, ctx->dif_pi_format);
		guard = dif_generate_guard_split(guard, dst_sgl, data_block_size,
						 ctx->guard_interval - data_block_size, ctx);
	} else {
		_data_copy_split(dst_sgl, src_sgl, data_block_size);
		_dif_sgl_advance(dst_sgl, ctx->guard_interval - data_block_size);
	}

	_dif_generate(&dif, guard, offset_blocks, ctx);

	dif_store_split(dst_sgl, &dif, ctx);
}

static void
dif_insert_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		      uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_insert_copy_split(src_sgl, dst_sgl, offset_blocks, ctx);
	}
}

static void
_dif_disable_insert_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			 const struct spdk_dif_ctx *ctx)
{
	uint32_t offset = 0, src_len, dst_len, buf_len, data_block_size;
	uint8_t *src, *dst;

	data_block_size = ctx->block_size - ctx->md_size;

	while (offset < data_block_size) {
		_dif_sgl_get_buf(src_sgl, &src, &src_len);
		_dif_sgl_get_buf(dst_sgl, &dst, &dst_len);
		buf_len = spdk_min(src_len, dst_len);
		buf_len = spdk_min(buf_len, data_block_size - offset);

		memcpy(dst, src, buf_len);

		_dif_sgl_advance(src_sgl, buf_len);
		_dif_sgl_advance(dst_sgl, buf_len);
		offset += buf_len;
	}

	_dif_sgl_advance(dst_sgl, ctx->md_size);
}

static void
dif_disable_insert_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_disable_insert_copy(src_sgl, dst_sgl, ctx);
	}
}

static int
_spdk_dif_insert_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		      uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size;

	data_block_size = ctx->block_size - ctx->md_size;

	if (!_dif_sgl_is_valid(src_sgl, data_block_size * num_blocks) ||
	    !_dif_sgl_is_valid(dst_sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec arrays are not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		dif_disable_insert_copy(src_sgl, dst_sgl, num_blocks, ctx);
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(src_sgl, data_block_size) &&
	    _dif_sgl_is_bytes_multiple(dst_sgl, ctx->block_size)) {
		dif_insert_copy(src_sgl, dst_sgl, num_blocks, ctx);
	} else {
		dif_insert_copy_split(src_sgl, dst_sgl, num_blocks, ctx);
	}

	return 0;
}

static void
_dif_overwrite_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		    uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint8_t *src, *dst;
	uint64_t guard = 0;

	_dif_sgl_get_buf(src_sgl, &src, NULL);
	_dif_sgl_get_buf(dst_sgl, &dst, NULL);

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard_copy(ctx->guard_seed, dst, src, ctx->guard_interval,
						 ctx->dif_pi_format);
	} else {
		memcpy(dst, src, ctx->guard_interval);
	}

	_dif_generate(dst + ctx->guard_interval, guard, offset_blocks, ctx);

	_dif_sgl_advance(src_sgl, ctx->block_size);
	_dif_sgl_advance(dst_sgl, ctx->block_size);
}

static void
dif_overwrite_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		   uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_overwrite_copy(src_sgl, dst_sgl, offset_blocks, ctx);
	}
}

static void
_dif_overwrite_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			  uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint64_t guard = 0;
	struct spdk_dif dif = {};

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard_copy_split(ctx->guard_seed, dst_sgl, src_sgl,
						       ctx->guard_interval, ctx->dif_pi_format);
	} else {
		_data_copy_split(dst_sgl, src_sgl, ctx->guard_interval);
	}

	_dif_sgl_advance(src_sgl, ctx->block_size - ctx->guard_interval);

	_dif_generate(&dif, guard, offset_blocks, ctx);
	dif_store_split(dst_sgl, &dif, ctx);
}

static void
dif_overwrite_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			 uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_overwrite_copy_split(src_sgl, dst_sgl, offset_blocks, ctx);
	}
}

static void
dif_disable_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		 uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	_data_copy_split(dst_sgl, src_sgl, ctx->block_size * num_blocks);
}

static int
_spdk_dif_overwrite_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			 uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	if (!_dif_sgl_is_valid(src_sgl, ctx->block_size * num_blocks) ||
	    !_dif_sgl_is_valid(dst_sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec arrays are not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		dif_disable_copy(src_sgl, dst_sgl, num_blocks, ctx);
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(src_sgl, ctx->block_size) &&
	    _dif_sgl_is_bytes_multiple(dst_sgl, ctx->block_size)) {
		dif_overwrite_copy(src_sgl, dst_sgl, num_blocks, ctx);
	} else {
		dif_overwrite_copy_split(src_sgl, dst_sgl, num_blocks, ctx);
	}

	return 0;
}

/*
 * [한국어]
 * spdk_dif_generate_copy - 사용자 SGL → bounce SGL로 복사하면서 PI 삽입.
 *
 * @iovs/iovcnt:        원본 데이터(메타 없음).
 * @bounce_iovs/cnt:    출력 버퍼(데이터+메타 인터리브 형태).
 * @num_blocks:         처리할 블록 수.
 * @ctx:                DIF 컨텍스트.
 * @return:             0 성공, -EINVAL.
 *
 * NVMe PRACT(Protection Information Action) 플래그가 켜지면 PI를 NVMe controller가
 * 생성/제거 가능. 그렇지 않거나 metadata가 PI보다 크면 인터리브 삽입 경로 사용.
 * "bounce" 버퍼는 데이터+PI를 합친 형태로 하드웨어에 제출되며, 보통 hugepage DMA-able 메모리.
 */
int
spdk_dif_generate_copy(struct iovec *iovs, int iovcnt, struct iovec *bounce_iovs,
		       int bounce_iovcnt, uint32_t num_blocks,
		       const struct spdk_dif_ctx *ctx)
{
	struct _dif_sgl src_sgl, dst_sgl;

	_dif_sgl_init(&src_sgl, iovs, iovcnt);
	_dif_sgl_init(&dst_sgl, bounce_iovs, bounce_iovcnt);

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_NVME_PRACT) ||
	    ctx->md_size == _dif_size(ctx->dif_pi_format)) {
		return _spdk_dif_insert_copy(&src_sgl, &dst_sgl, num_blocks, ctx);
	} else {
		return _spdk_dif_overwrite_copy(&src_sgl, &dst_sgl, num_blocks, ctx);
	}
}

static int
_dif_strip_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		uint32_t offset_blocks, const struct spdk_dif_ctx *ctx,
		struct spdk_dif_error *err_blk)
{
	uint32_t data_block_size;
	uint8_t *src, *dst;
	int rc;
	uint64_t guard = 0;

	data_block_size = ctx->block_size - ctx->md_size;

	_dif_sgl_get_buf(src_sgl, &src, NULL);
	_dif_sgl_get_buf(dst_sgl, &dst, NULL);

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard_copy(ctx->guard_seed, dst, src, data_block_size,
						 ctx->dif_pi_format);
		guard = _dif_generate_guard(guard, src + data_block_size,
					    ctx->guard_interval - data_block_size, ctx->dif_pi_format);
	} else {
		memcpy(dst, src, data_block_size);
	}

	rc = _dif_verify(src + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
	if (rc != 0) {
		return rc;
	}

	_dif_sgl_advance(src_sgl, ctx->block_size);
	_dif_sgl_advance(dst_sgl, data_block_size);

	return 0;
}

static int
dif_strip_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
	       uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
	       struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks;
	int rc;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dif_strip_copy(src_sgl, dst_sgl, offset_blocks, ctx, err_blk);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int
_dif_strip_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		      uint32_t offset_blocks, const struct spdk_dif_ctx *ctx,
		      struct spdk_dif_error *err_blk)
{
	uint32_t data_block_size;
	uint64_t guard = 0;
	struct spdk_dif dif = {};

	data_block_size = ctx->block_size - ctx->md_size;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard_copy_split(ctx->guard_seed, dst_sgl, src_sgl,
						       data_block_size, ctx->dif_pi_format);
		guard = dif_generate_guard_split(guard, src_sgl, data_block_size,
						 ctx->guard_interval - data_block_size, ctx);
	} else {
		_data_copy_split(dst_sgl, src_sgl, data_block_size);
		_dif_sgl_advance(src_sgl, ctx->guard_interval - data_block_size);
	}

	dif_load_split(src_sgl, &dif, ctx);

	return _dif_verify(&dif, guard, offset_blocks, ctx, err_blk);
}

static int
dif_strip_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		     uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		     struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks;
	int rc;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dif_strip_copy_split(src_sgl, dst_sgl, offset_blocks, ctx, err_blk);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static void
_dif_disable_strip_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			const struct spdk_dif_ctx *ctx)
{
	uint32_t offset = 0, src_len, dst_len, buf_len, data_block_size;
	uint8_t *src, *dst;

	data_block_size = ctx->block_size - ctx->md_size;

	while (offset < data_block_size) {
		_dif_sgl_get_buf(src_sgl, &src, &src_len);
		_dif_sgl_get_buf(dst_sgl, &dst, &dst_len);
		buf_len = spdk_min(src_len, dst_len);
		buf_len = spdk_min(buf_len, data_block_size - offset);

		memcpy(dst, src, buf_len);

		_dif_sgl_advance(src_sgl, buf_len);
		_dif_sgl_advance(dst_sgl, buf_len);
		offset += buf_len;
	}

	_dif_sgl_advance(src_sgl, ctx->md_size);
}

static void
dif_disable_strip_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		       uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_disable_strip_copy(src_sgl, dst_sgl, ctx);
	}
}

static int
_spdk_dif_strip_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		     uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		     struct spdk_dif_error *err_blk)
{
	uint32_t data_block_size;

	data_block_size = ctx->block_size - ctx->md_size;

	if (!_dif_sgl_is_valid(dst_sgl, data_block_size * num_blocks) ||
	    !_dif_sgl_is_valid(src_sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec arrays are not valid\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		dif_disable_strip_copy(src_sgl, dst_sgl, num_blocks, ctx);
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(dst_sgl, data_block_size) &&
	    _dif_sgl_is_bytes_multiple(src_sgl, ctx->block_size)) {
		return dif_strip_copy(src_sgl, dst_sgl, num_blocks, ctx, err_blk);
	} else {
		return dif_strip_copy_split(src_sgl, dst_sgl, num_blocks, ctx, err_blk);
	}
}

static int
_dif_verify_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		 uint32_t offset_blocks, const struct spdk_dif_ctx *ctx,
		 struct spdk_dif_error *err_blk)
{
	uint8_t *src, *dst;
	int rc;
	uint64_t guard = 0;

	_dif_sgl_get_buf(src_sgl, &src, NULL);
	_dif_sgl_get_buf(dst_sgl, &dst, NULL);

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard_copy(ctx->guard_seed, dst, src, ctx->guard_interval,
						 ctx->dif_pi_format);
	} else {
		memcpy(dst, src, ctx->guard_interval);
	}

	rc = _dif_verify(src + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
	if (rc != 0) {
		return rc;
	}

	_dif_sgl_advance(src_sgl, ctx->block_size);
	_dif_sgl_advance(dst_sgl, ctx->block_size);

	return 0;
}

static int
dif_verify_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks;
	int rc;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dif_verify_copy(src_sgl, dst_sgl, offset_blocks, ctx, err_blk);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int
_dif_verify_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		       uint32_t offset_blocks, const struct spdk_dif_ctx *ctx,
		       struct spdk_dif_error *err_blk)
{
	uint64_t guard = 0;
	struct spdk_dif dif = {};

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard_copy_split(ctx->guard_seed, dst_sgl, src_sgl,
						       ctx->guard_interval, ctx->dif_pi_format);
	} else {
		_data_copy_split(dst_sgl, src_sgl, ctx->guard_interval);
	}

	dif_load_split(src_sgl, &dif, ctx);
	_dif_sgl_advance(dst_sgl, ctx->block_size - ctx->guard_interval);

	return _dif_verify(&dif, guard, offset_blocks, ctx, err_blk);
}

static int
dif_verify_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		      uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		      struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks;
	int rc;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dif_verify_copy_split(src_sgl, dst_sgl, offset_blocks, ctx, err_blk);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int
_spdk_dif_verify_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		      uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		      struct spdk_dif_error *err_blk)
{
	if (!_dif_sgl_is_valid(dst_sgl, ctx->block_size * num_blocks) ||
	    !_dif_sgl_is_valid(src_sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec arrays are not valid\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		dif_disable_copy(src_sgl, dst_sgl, num_blocks, ctx);
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(dst_sgl, ctx->block_size) &&
	    _dif_sgl_is_bytes_multiple(src_sgl, ctx->block_size)) {
		return dif_verify_copy(src_sgl, dst_sgl, num_blocks, ctx, err_blk);
	} else {
		return dif_verify_copy_split(src_sgl, dst_sgl, num_blocks, ctx, err_blk);
	}
}

/*
 * [한국어]
 * spdk_dif_verify_copy - bounce SGL(데이터+PI)을 사용자 SGL(데이터만)로 복사+검증.
 *
 * 읽기 경로의 짝: HW로부터 받은 bounce 버퍼의 PI를 검증한 뒤 데이터 영역만 사용자에게 전달.
 * 검증 실패 시 -EIO 및 err_blk 채움.
 */
int
spdk_dif_verify_copy(struct iovec *iovs, int iovcnt, struct iovec *bounce_iovs,
		     int bounce_iovcnt, uint32_t num_blocks,
		     const struct spdk_dif_ctx *ctx,
		     struct spdk_dif_error *err_blk)
{
	struct _dif_sgl src_sgl, dst_sgl;

	_dif_sgl_init(&src_sgl, bounce_iovs, bounce_iovcnt);
	_dif_sgl_init(&dst_sgl, iovs, iovcnt);

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_NVME_PRACT) ||
	    ctx->md_size == _dif_size(ctx->dif_pi_format)) {
		return _spdk_dif_strip_copy(&src_sgl, &dst_sgl, num_blocks, ctx, err_blk);
	} else {
		return _spdk_dif_verify_copy(&src_sgl, &dst_sgl, num_blocks, ctx, err_blk);
	}
}

static void
_bit_flip(uint8_t *buf, uint32_t flip_bit)
{
	uint8_t byte;

	byte = *buf;
	byte ^= 1 << flip_bit;
	*buf = byte;
}

static int
_dif_inject_error(struct _dif_sgl *sgl,
		  uint32_t block_size, uint32_t num_blocks,
		  uint32_t inject_offset_blocks,
		  uint32_t inject_offset_bytes,
		  uint32_t inject_offset_bits)
{
	uint32_t offset_in_block, buf_len;
	uint8_t *buf;

	_dif_sgl_advance(sgl, block_size * inject_offset_blocks);

	offset_in_block = 0;

	while (offset_in_block < block_size) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, block_size - offset_in_block);

		if (inject_offset_bytes >= offset_in_block &&
		    inject_offset_bytes < offset_in_block + buf_len) {
			buf += inject_offset_bytes - offset_in_block;
			_bit_flip(buf, inject_offset_bits);
			return 0;
		}

		_dif_sgl_advance(sgl, buf_len);
		offset_in_block += buf_len;
	}

	return -1;
}

static int
dif_inject_error(struct _dif_sgl *sgl, uint32_t block_size, uint32_t num_blocks,
		 uint32_t start_inject_bytes, uint32_t inject_range_bytes,
		 uint32_t *inject_offset)
{
	uint32_t inject_offset_blocks, inject_offset_bytes, inject_offset_bits;
	uint32_t offset_blocks;
	int rc;

	srand(time(0));

	inject_offset_blocks = rand() % num_blocks;
	inject_offset_bytes = start_inject_bytes + (rand() % inject_range_bytes);
	inject_offset_bits = rand() % 8;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		if (offset_blocks == inject_offset_blocks) {
			rc = _dif_inject_error(sgl, block_size, num_blocks,
					       inject_offset_blocks,
					       inject_offset_bytes,
					       inject_offset_bits);
			if (rc == 0) {
				*inject_offset = inject_offset_blocks;
			}
			return rc;
		}
	}

	return -1;
}

/*
 * [한국어]
 * spdk_dif_inject_error - 테스트용으로 PI 영역(reftag/apptag/guard) 또는 데이터에 비트 손상 주입.
 *
 * @inject_flags:    SPDK_DIF_REFTAG_ERROR/APPTAG_ERROR/GUARD_ERROR/DATA_ERROR 중 하나 이상.
 * @inject_offset:   [out] 손상이 주입된 블록 오프셋.
 *
 * 사용처: SPDK 테스트(test/dif). 실제 운영에서는 호출하지 않는다. 검증 경로의 회귀 테스트와
 * 에러 핸들링 코드의 도달성 검증에 사용.
 */
int
spdk_dif_inject_error(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
		      const struct spdk_dif_ctx *ctx, uint32_t inject_flags,
		      uint32_t *inject_offset)
{
	struct _dif_sgl sgl;
	int rc;

	_dif_sgl_init(&sgl, iovs, iovcnt);

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (inject_flags & SPDK_DIF_REFTAG_ERROR) {
		rc = dif_inject_error(&sgl, ctx->block_size, num_blocks,
				      ctx->guard_interval + _dif_reftag_offset(ctx->dif_pi_format),
				      _dif_reftag_size(ctx->dif_pi_format),
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Reference Tag.\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_APPTAG_ERROR) {
		rc = dif_inject_error(&sgl, ctx->block_size, num_blocks,
				      ctx->guard_interval + _dif_apptag_offset(ctx->dif_pi_format),
				      _dif_apptag_size(),
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Application Tag.\n");
			return rc;
		}
	}
	if (inject_flags & SPDK_DIF_GUARD_ERROR) {
		rc = dif_inject_error(&sgl, ctx->block_size, num_blocks,
				      ctx->guard_interval,
				      _dif_guard_size(ctx->dif_pi_format),
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Guard.\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_DATA_ERROR) {
		/* If the DIF information is contained within the last 8/16 bytes of
		 * metadata (depending on the PI format), then the CRC covers all metadata
		 * bytes up to but excluding the last 8/16 bytes. But error injection does not
		 * cover these metadata because classification is not determined yet.
		 *
		 * Note: Error injection to data block is expected to be detected as
		 * guard error.
		 */
		rc = dif_inject_error(&sgl, ctx->block_size, num_blocks,
				      0,
				      ctx->block_size - ctx->md_size,
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to data block.\n");
			return rc;
		}
	}

	return 0;
}

static void
dix_generate(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
	     uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks = 0;
	uint8_t *data_buf, *md_buf;
	uint64_t guard;

	while (offset_blocks < num_blocks) {
		_dif_sgl_get_buf(data_sgl, &data_buf, NULL);
		_dif_sgl_get_buf(md_sgl, &md_buf, NULL);

		guard = 0;
		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(ctx->guard_seed, data_buf, ctx->block_size,
						    ctx->dif_pi_format);
			guard = _dif_generate_guard(guard, md_buf, ctx->guard_interval,
						    ctx->dif_pi_format);
		}

		_dif_generate(md_buf + ctx->guard_interval, guard, offset_blocks, ctx);

		_dif_sgl_advance(data_sgl, ctx->block_size);
		_dif_sgl_advance(md_sgl, ctx->md_size);
		offset_blocks++;
	}
}

static void
_dix_generate_split(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
		    uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_in_block, data_buf_len;
	uint8_t *data_buf, *md_buf;
	uint64_t guard = 0;

	_dif_sgl_get_buf(md_sgl, &md_buf, NULL);

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}
	offset_in_block = 0;

	while (offset_in_block < ctx->block_size) {
		_dif_sgl_get_buf(data_sgl, &data_buf, &data_buf_len);
		data_buf_len = spdk_min(data_buf_len, ctx->block_size - offset_in_block);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(guard, data_buf, data_buf_len,
						    ctx->dif_pi_format);
		}

		_dif_sgl_advance(data_sgl, data_buf_len);
		offset_in_block += data_buf_len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard(guard, md_buf, ctx->guard_interval,
					    ctx->dif_pi_format);
	}

	_dif_sgl_advance(md_sgl, ctx->md_size);

	_dif_generate(md_buf + ctx->guard_interval, guard, offset_blocks, ctx);
}

static void
dix_generate_split(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
		   uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dix_generate_split(data_sgl, md_sgl, offset_blocks, ctx);
	}
}

/*
 * [한국어]
 * spdk_dix_generate - DIX(Data Integrity Extension): 데이터/메타가 분리된 경우의 PI 생성.
 *
 * @iovs:    데이터 SGL (data_block_size 단위 블록).
 * @md_iov:  메타데이터 단일 iovec (블록 수 × md_size 크기).
 * @ctx:     md_interleave=false로 설정된 컨텍스트.
 *
 * NVMe MPTR(Metadata Pointer)가 별도로 지정된 경우 등 메타가 데이터와 분리된 형태에서 사용.
 * 데이터 영역에서 Guard CRC 산출 → 메타 버퍼의 PI 영역에 PI 기록.
 */
int
spdk_dix_generate(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
		  uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	struct _dif_sgl data_sgl, md_sgl;

	_dif_sgl_init(&data_sgl, iovs, iovcnt);
	_dif_sgl_init(&md_sgl, md_iov, 1);

	if (!_dif_sgl_is_valid(&data_sgl, ctx->block_size * num_blocks) ||
	    !_dif_sgl_is_valid(&md_sgl, ctx->md_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(&data_sgl, ctx->block_size)) {
		dix_generate(&data_sgl, &md_sgl, num_blocks, ctx);
	} else {
		dix_generate_split(&data_sgl, &md_sgl, num_blocks, ctx);
	}

	return 0;
}

static int
dix_verify(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
	   uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
	   struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks = 0;
	uint8_t *data_buf, *md_buf;
	uint64_t guard;
	int rc;

	while (offset_blocks < num_blocks) {
		_dif_sgl_get_buf(data_sgl, &data_buf, NULL);
		_dif_sgl_get_buf(md_sgl, &md_buf, NULL);

		guard = 0;
		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(ctx->guard_seed, data_buf, ctx->block_size,
						    ctx->dif_pi_format);
			guard = _dif_generate_guard(guard, md_buf, ctx->guard_interval,
						    ctx->dif_pi_format);
		}

		rc = _dif_verify(md_buf + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
		if (rc != 0) {
			return rc;
		}

		_dif_sgl_advance(data_sgl, ctx->block_size);
		_dif_sgl_advance(md_sgl, ctx->md_size);
		offset_blocks++;
	}

	return 0;
}

static int
_dix_verify_split(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
		  uint32_t offset_blocks, const struct spdk_dif_ctx *ctx,
		  struct spdk_dif_error *err_blk)
{
	uint32_t offset_in_block, data_buf_len;
	uint8_t *data_buf, *md_buf;
	uint64_t guard = 0;

	_dif_sgl_get_buf(md_sgl, &md_buf, NULL);

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}
	offset_in_block = 0;

	while (offset_in_block < ctx->block_size) {
		_dif_sgl_get_buf(data_sgl, &data_buf, &data_buf_len);
		data_buf_len = spdk_min(data_buf_len, ctx->block_size - offset_in_block);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(guard, data_buf, data_buf_len,
						    ctx->dif_pi_format);
		}

		_dif_sgl_advance(data_sgl, data_buf_len);
		offset_in_block += data_buf_len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard(guard, md_buf, ctx->guard_interval,
					    ctx->dif_pi_format);
	}

	_dif_sgl_advance(md_sgl, ctx->md_size);

	return _dif_verify(md_buf + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
}

static int
dix_verify_split(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
		 uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		 struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks;
	int rc;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dix_verify_split(data_sgl, md_sgl, offset_blocks, ctx, err_blk);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

/*
 * [한국어]
 * spdk_dix_verify - DIX 모드에서 데이터/메타 분리 SGL을 사용해 PI 검증.
 *
 * 데이터 SGL에서 Guard CRC 재계산 → 메타 SGL의 PI 영역과 비교 → 불일치 시 -EIO + err_blk.
 */
int
spdk_dix_verify(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
		uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		struct spdk_dif_error *err_blk)
{
	struct _dif_sgl data_sgl, md_sgl;

	if (md_iov->iov_base == NULL) {
		SPDK_ERRLOG("Metadata buffer is NULL.\n");
		return -EINVAL;
	}

	_dif_sgl_init(&data_sgl, iovs, iovcnt);
	_dif_sgl_init(&md_sgl, md_iov, 1);

	if (!_dif_sgl_is_valid(&data_sgl, ctx->block_size * num_blocks) ||
	    !_dif_sgl_is_valid(&md_sgl, ctx->md_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(&data_sgl, ctx->block_size)) {
		return dix_verify(&data_sgl, &md_sgl, num_blocks, ctx, err_blk);
	} else {
		return dix_verify_split(&data_sgl, &md_sgl, num_blocks, ctx, err_blk);
	}
}

int
spdk_dix_inject_error(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
		      uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		      uint32_t inject_flags, uint32_t *inject_offset)
{
	struct _dif_sgl data_sgl, md_sgl;
	int rc;

	_dif_sgl_init(&data_sgl, iovs, iovcnt);
	_dif_sgl_init(&md_sgl, md_iov, 1);

	if (!_dif_sgl_is_valid(&data_sgl, ctx->block_size * num_blocks) ||
	    !_dif_sgl_is_valid(&md_sgl, ctx->md_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (inject_flags & SPDK_DIF_REFTAG_ERROR) {
		rc = dif_inject_error(&md_sgl, ctx->md_size, num_blocks,
				      ctx->guard_interval + _dif_reftag_offset(ctx->dif_pi_format),
				      _dif_reftag_size(ctx->dif_pi_format),
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Reference Tag.\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_APPTAG_ERROR) {
		rc = dif_inject_error(&md_sgl, ctx->md_size, num_blocks,
				      ctx->guard_interval + _dif_apptag_offset(ctx->dif_pi_format),
				      _dif_apptag_size(),
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Application Tag.\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_GUARD_ERROR) {
		rc = dif_inject_error(&md_sgl, ctx->md_size, num_blocks,
				      ctx->guard_interval,
				      _dif_guard_size(ctx->dif_pi_format),
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Guard.\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_DATA_ERROR) {
		/* Note: Error injection to data block is expected to be detected
		 * as guard error.
		 */
		rc = dif_inject_error(&data_sgl, ctx->block_size, num_blocks,
				      0,
				      ctx->block_size,
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Guard.\n");
			return rc;
		}
	}

	return 0;
}

static uint32_t
_to_next_boundary(uint32_t offset, uint32_t boundary)
{
	return boundary - (offset % boundary);
}

static uint32_t
_to_size_with_md(uint32_t size, uint32_t data_block_size, uint32_t block_size)
{
	return (size / data_block_size) * block_size + (size % data_block_size);
}

/*
 * [한국어]
 * spdk_dif_set_md_interleave_iovs - 메타-인터리브된 buf_iovs에서 데이터 영역만 가리키는 iovs 생성.
 *
 * @iovs/iovcnt:        [out] 데이터 영역만 가리킬 iovec(메타 영역은 건너뜀).
 * @buf_iovs/buf_iovcnt: 메타가 인터리브된 원본 버퍼 SGL.
 * @data_offset:        데이터 영역 기준 시작 오프셋.
 * @data_len:           읽을 데이터 길이.
 * @_mapped_len:        [out] 실제로 매핑된 데이터 길이.
 *
 * 사용처: 사용자 read 콜백이 메타 없는 데이터만 받기를 원할 때, bounce 버퍼를 복사하지 않고
 * iovec만 재구성해 zero-copy로 전달.
 */
int
spdk_dif_set_md_interleave_iovs(struct iovec *iovs, int iovcnt,
				struct iovec *buf_iovs, int buf_iovcnt,
				uint32_t data_offset, uint32_t data_len,
				uint32_t *_mapped_len,
				const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size, data_unalign, buf_len, buf_offset, len;
	struct _dif_sgl dif_sgl;
	struct _dif_sgl buf_sgl;

	if (iovs == NULL || iovcnt == 0 || buf_iovs == NULL || buf_iovcnt == 0) {
		return -EINVAL;
	}

	data_block_size = ctx->block_size - ctx->md_size;

	data_unalign = ctx->data_offset % data_block_size;

	buf_len = _to_size_with_md(data_unalign + data_offset + data_len, data_block_size,
				   ctx->block_size);
	buf_len -= data_unalign;

	_dif_sgl_init(&dif_sgl, iovs, iovcnt);
	_dif_sgl_init(&buf_sgl, buf_iovs, buf_iovcnt);

	if (!_dif_sgl_is_valid(&buf_sgl, buf_len)) {
		SPDK_ERRLOG("Buffer overflow will occur.\n");
		return -ERANGE;
	}

	buf_offset = _to_size_with_md(data_unalign + data_offset, data_block_size, ctx->block_size);
	buf_offset -= data_unalign;

	_dif_sgl_advance(&buf_sgl, buf_offset);

	while (data_len != 0) {
		len = spdk_min(data_len, _to_next_boundary(ctx->data_offset + data_offset, data_block_size));
		if (!_dif_sgl_append_split(&dif_sgl, &buf_sgl, len)) {
			break;
		}
		_dif_sgl_advance(&buf_sgl, ctx->md_size);
		data_offset += len;
		data_len -= len;
	}

	if (_mapped_len != NULL) {
		*_mapped_len = dif_sgl.total_size;
	}

	return iovcnt - dif_sgl.iovcnt;
}

static int
_dif_sgl_setup_stream(struct _dif_sgl *sgl, uint32_t *_buf_offset, uint32_t *_buf_len,
		      uint32_t data_offset, uint32_t data_len,
		      const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size, data_unalign, buf_len, buf_offset;

	data_block_size = ctx->block_size - ctx->md_size;

	data_unalign = ctx->data_offset % data_block_size;

	/* If the last data block is complete, DIF of the data block is
	 * inserted or verified in this turn.
	 */
	buf_len = _to_size_with_md(data_unalign + data_offset + data_len, data_block_size,
				   ctx->block_size);
	buf_len -= data_unalign;

	if (!_dif_sgl_is_valid(sgl, buf_len)) {
		return -ERANGE;
	}

	buf_offset = _to_size_with_md(data_unalign + data_offset, data_block_size, ctx->block_size);
	buf_offset -= data_unalign;

	_dif_sgl_advance(sgl, buf_offset);
	buf_len -= buf_offset;

	buf_offset += data_unalign;

	*_buf_offset = buf_offset;
	*_buf_len = buf_len;

	return 0;
}

/*
 * [한국어]
 * spdk_dif_generate_stream - PI 생성을 여러 회차에 걸쳐 누적 처리(스트림 모드).
 *
 * @data_offset:  이번 회차 데이터의 시작(전체 I/O 기준).
 * @data_len:     이번 회차 처리 길이.
 * @ctx:          ctx->last_guard 등 진행 상태 업데이트.
 *
 * NVMe-oF에서 데이터를 청크로 받을 때, 한 블록의 일부만 도착해도 가능한 만큼 PI 처리를
 * 진행하고 나머지는 다음 호출에 누적. ctx 안에 last_guard, last_remainder_data_size 등이 보관.
 */
int
spdk_dif_generate_stream(struct iovec *iovs, int iovcnt,
			 uint32_t data_offset, uint32_t data_len,
			 struct spdk_dif_ctx *ctx)
{
	uint32_t buf_len = 0, buf_offset = 0;
	uint32_t len, offset_in_block, offset_blocks;
	uint64_t guard = 0;
	struct _dif_sgl sgl;
	int rc;

	if (iovs == NULL || iovcnt == 0) {
		return -EINVAL;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->last_guard;
	}

	_dif_sgl_init(&sgl, iovs, iovcnt);

	rc = _dif_sgl_setup_stream(&sgl, &buf_offset, &buf_len, data_offset, data_len, ctx);
	if (rc != 0) {
		return rc;
	}

	while (buf_len != 0) {
		len = spdk_min(buf_len, _to_next_boundary(buf_offset, ctx->block_size));
		offset_in_block = buf_offset % ctx->block_size;
		offset_blocks = buf_offset / ctx->block_size;

		guard = _dif_generate_split(&sgl, offset_in_block, len, guard, offset_blocks, ctx);

		buf_len -= len;
		buf_offset += len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		ctx->last_guard = guard;
	}

	return 0;
}

/*
 * [한국어]
 * spdk_dif_verify_stream - 스트림 모드 PI 검증. generate_stream의 짝.
 */
int
spdk_dif_verify_stream(struct iovec *iovs, int iovcnt,
		       uint32_t data_offset, uint32_t data_len,
		       struct spdk_dif_ctx *ctx,
		       struct spdk_dif_error *err_blk)
{
	uint32_t buf_len = 0, buf_offset = 0;
	uint32_t len, offset_in_block, offset_blocks;
	uint64_t guard = 0;
	struct _dif_sgl sgl;
	int rc = 0;

	if (iovs == NULL || iovcnt == 0) {
		return -EINVAL;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->last_guard;
	}

	_dif_sgl_init(&sgl, iovs, iovcnt);

	rc = _dif_sgl_setup_stream(&sgl, &buf_offset, &buf_len, data_offset, data_len, ctx);
	if (rc != 0) {
		return rc;
	}

	while (buf_len != 0) {
		len = spdk_min(buf_len, _to_next_boundary(buf_offset, ctx->block_size));
		offset_in_block = buf_offset % ctx->block_size;
		offset_blocks = buf_offset / ctx->block_size;

		rc = _dif_verify_split(&sgl, offset_in_block, len, &guard, offset_blocks,
				       ctx, err_blk);
		if (rc != 0) {
			goto error;
		}

		buf_len -= len;
		buf_offset += len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		ctx->last_guard = guard;
	}
error:
	return rc;
}

/*
 * [한국어]
 * spdk_dif_update_crc32c_stream - 스트림 모드 데이터 영역 CRC32C 누적.
 *
 * NVMe-oF transport CRC32C 검증을 청크별로 누적할 때 사용.
 */
int
spdk_dif_update_crc32c_stream(struct iovec *iovs, int iovcnt,
			      uint32_t data_offset, uint32_t data_len,
			      uint32_t *_crc32c, const struct spdk_dif_ctx *ctx)
{
	uint32_t buf_len = 0, buf_offset = 0, len, offset_in_block;
	uint32_t crc32c;
	struct _dif_sgl sgl;
	int rc;

	if (iovs == NULL || iovcnt == 0) {
		return -EINVAL;
	}

	crc32c = *_crc32c;
	_dif_sgl_init(&sgl, iovs, iovcnt);

	rc = _dif_sgl_setup_stream(&sgl, &buf_offset, &buf_len, data_offset, data_len, ctx);
	if (rc != 0) {
		return rc;
	}

	while (buf_len != 0) {
		len = spdk_min(buf_len, _to_next_boundary(buf_offset, ctx->block_size));
		offset_in_block = buf_offset % ctx->block_size;

		crc32c = _dif_update_crc32c_split(&sgl, offset_in_block, len, crc32c, ctx);

		buf_len -= len;
		buf_offset += len;
	}

	*_crc32c = crc32c;

	return 0;
}

/*
 * [한국어]
 * spdk_dif_get_range_with_md - 데이터 오프셋/길이를 메타 포함 버퍼 오프셋/길이로 변환.
 *
 * data_offset(데이터 좌표)와 data_len이 주어지면, 메타가 인터리브된 실제 버퍼에서의
 * (시작 오프셋, 길이)를 계산해준다. md_interleave=false면 그대로 반환.
 */
void
spdk_dif_get_range_with_md(uint32_t data_offset, uint32_t data_len,
			   uint32_t *_buf_offset, uint32_t *_buf_len,
			   const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size, data_unalign, buf_offset, buf_len;

	if (!ctx->md_interleave) {
		buf_offset = data_offset;
		buf_len = data_len;
	} else {
		data_block_size = ctx->block_size - ctx->md_size;

		data_unalign = data_offset % data_block_size;

		buf_offset = _to_size_with_md(data_offset, data_block_size, ctx->block_size);
		buf_len = _to_size_with_md(data_unalign + data_len, data_block_size, ctx->block_size) -
			  data_unalign;
	}

	if (_buf_offset != NULL) {
		*_buf_offset = buf_offset;
	}

	if (_buf_len != NULL) {
		*_buf_len = buf_len;
	}
}

/*
 * [한국어]
 * spdk_dif_get_length_with_md - 데이터 길이만큼의 메타 인터리브 버퍼 총 길이 계산.
 *
 * 사용처: 사용자가 N바이트 데이터를 위해 얼마의 bounce 버퍼를 할당해야 하는지 결정.
 */
uint32_t
spdk_dif_get_length_with_md(uint32_t data_len, const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size;

	if (!ctx->md_interleave) {
		return data_len;
	} else {
		data_block_size = ctx->block_size - ctx->md_size;

		return _to_size_with_md(data_len, data_block_size, ctx->block_size);
	}
}

static int
_dif_remap_ref_tag(struct _dif_sgl *sgl, uint32_t offset_blocks,
		   const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk,
		   bool check_ref_tag)
{
	uint32_t offset, buf_len;
	uint64_t expected = 0, remapped;
	uint8_t *buf;
	struct _dif_sgl tmp_sgl;
	struct spdk_dif dif;

	/* Fast forward to DIF field. */
	_dif_sgl_advance(sgl, ctx->guard_interval);
	_dif_sgl_copy(&tmp_sgl, sgl);

	/* Copy the split DIF field to the temporary DIF buffer */
	offset = 0;
	while (offset < _dif_size(ctx->dif_pi_format)) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, _dif_size(ctx->dif_pi_format) - offset);

		memcpy((uint8_t *)&dif + offset, buf, buf_len);

		_dif_sgl_advance(sgl, buf_len);
		offset += buf_len;
	}

	if (_dif_ignore(&dif, ctx)) {
		goto end;
	}

	/* For type 1 and 2, the Reference Tag is incremented for each
	 * subsequent logical block. For type 3, the Reference Tag
	 * remains the same as the initial Reference Tag.
	 */
	if (ctx->dif_type != SPDK_DIF_TYPE3) {
		expected = ctx->init_ref_tag + ctx->ref_tag_offset + offset_blocks;
		remapped = ctx->remapped_init_ref_tag + ctx->ref_tag_offset + offset_blocks;
	} else {
		remapped = ctx->remapped_init_ref_tag;
	}

	/* Verify the stored Reference Tag. */
	if (check_ref_tag && !_dif_reftag_check(&dif, ctx, expected, offset_blocks, err_blk)) {
		return -1;
	}

	/* Update the stored Reference Tag to the remapped one. */
	_dif_set_reftag(&dif, remapped, ctx->dif_pi_format);

	offset = 0;
	while (offset < _dif_size(ctx->dif_pi_format)) {
		_dif_sgl_get_buf(&tmp_sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, _dif_size(ctx->dif_pi_format) - offset);

		memcpy(buf, (uint8_t *)&dif + offset, buf_len);

		_dif_sgl_advance(&tmp_sgl, buf_len);
		offset += buf_len;
	}

end:
	_dif_sgl_advance(sgl, ctx->block_size - ctx->guard_interval - _dif_size(ctx->dif_pi_format));

	return 0;
}

/*
 * [한국어]
 * spdk_dif_remap_ref_tag - 블록 이동(copy/clone) 시 PI의 reference tag를 새 LBA에 맞춰 갱신.
 *
 * @check_ref_tag:  true이면 갱신 전에 기존 reftag가 예상값과 일치하는지 검증.
 *
 * spdk_dif_ctx_set_remapped_init_ref_tag로 새 시작 reftag를 ctx에 미리 설정해 두고
 * 호출. PI 영역을 in-place로 다시 쓰며, Guard CRC는 reftag 변경에 영향받지 않으므로 유지.
 */
int
spdk_dif_remap_ref_tag(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
		       const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk,
		       bool check_ref_tag)
{
	struct _dif_sgl sgl;
	uint32_t offset_blocks;
	int rc;

	_dif_sgl_init(&sgl, iovs, iovcnt);

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK)) {
		return 0;
	}

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dif_remap_ref_tag(&sgl, offset_blocks, ctx, err_blk, check_ref_tag);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int
_dix_remap_ref_tag(struct _dif_sgl *md_sgl, uint32_t offset_blocks,
		   const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk,
		   bool check_ref_tag)
{
	uint64_t expected = 0, remapped;
	uint8_t *md_buf;
	struct spdk_dif *dif;

	_dif_sgl_get_buf(md_sgl, &md_buf, NULL);

	dif = (struct spdk_dif *)(md_buf + ctx->guard_interval);

	if (_dif_ignore(dif, ctx)) {
		goto end;
	}

	/* For type 1 and 2, the Reference Tag is incremented for each
	 * subsequent logical block. For type 3, the Reference Tag
	 * remains the same as the initialReference Tag.
	 */
	if (ctx->dif_type != SPDK_DIF_TYPE3) {
		expected = ctx->init_ref_tag + ctx->ref_tag_offset + offset_blocks;
		remapped = ctx->remapped_init_ref_tag + ctx->ref_tag_offset + offset_blocks;
	} else {
		remapped = ctx->remapped_init_ref_tag;
	}

	/* Verify the stored Reference Tag. */
	if (check_ref_tag && !_dif_reftag_check(dif, ctx, expected, offset_blocks, err_blk)) {
		return -1;
	}

	/* Update the stored Reference Tag to the remapped one. */
	_dif_set_reftag(dif, remapped, ctx->dif_pi_format);

end:
	_dif_sgl_advance(md_sgl, ctx->md_size);

	return 0;
}

/*
 * [한국어]
 * spdk_dix_remap_ref_tag - DIX 모드에서 메타 SGL의 PI reference tag만 갱신.
 *
 * 데이터는 건드리지 않고 메타 버퍼만 재기록. blob copy 등에서 사용.
 */
int
spdk_dix_remap_ref_tag(struct iovec *md_iov, uint32_t num_blocks,
		       const struct spdk_dif_ctx *ctx,
		       struct spdk_dif_error *err_blk,
		       bool check_ref_tag)
{
	struct _dif_sgl md_sgl;
	uint32_t offset_blocks;
	int rc;

	_dif_sgl_init(&md_sgl, md_iov, 1);

	if (!_dif_sgl_is_valid(&md_sgl, ctx->md_size * num_blocks)) {
		SPDK_ERRLOG("Size of metadata iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK)) {
		return 0;
	}

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dix_remap_ref_tag(&md_sgl, offset_blocks, ctx, err_blk, check_ref_tag);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}
