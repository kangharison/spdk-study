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
 * 이 파일(원본 2604 라인, 주석 추가 후 약 3.5K+ 라인)은 매우 크다. 본 라운드에서:
 *  - 파일 상단 블록(필수 4섹션) 완성.
 *  - struct spdk_dif/_dif_sgl 모든 필드 §4 멀티라인 주석.
 *  - 모든 정적 헬퍼/공개 API 함수에 §2 멀티라인 주석.
 *  - 모든 함수 본문 실행 라인에 §3 인라인 주석.
 *  - #include / 매크로 / typedef에 인라인 주석.
 * 외부에 노출되는 API와 내부 라우팅 로직 모두 "이 파일만 보고도 PI 처리 흐름을
 * 완전히 이해" 가능한 수준으로 정비.
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

/*
 * [한국어]
 * _dif_sgl_init - _dif_sgl 커서를 주어진 iovec 배열의 시작에 위치시킨다.
 *
 * @s:      [out] 초기화 대상 SGL 커서.
 * @iovs:   사용자가 넘긴 iovec 배열(데이터/메타 버퍼).
 * @iovcnt: 배열 길이.
 *
 * 배경: dif.c의 모든 generate/verify/copy 경로는 처리 시작 전에 이 함수로 커서를
 * 만들고, 이후 _dif_sgl_advance / _dif_sgl_get_buf로 한 블록씩 진행한다.
 * 실행 컨텍스트: 단일 SPDK thread(한 ctx/한 I/O는 한 스레드에서만 처리).
 *
 * 호출 체인:
 *   spdk_dif_generate / spdk_dif_verify / spdk_dif_*_copy / spdk_dix_* → [_dif_sgl_init]
 */
static inline void
_dif_sgl_init(struct _dif_sgl *s, struct iovec *iovs, int iovcnt)
{
	s->iov = iovs;
	/* [한국어] 현재 가리키고 있는 iovec 포인터를 배열의 첫 원소로 설정. */
	s->iovcnt = iovcnt;
	/* [한국어] 남은 iovec 수 = 전체 길이로 초기화. _dif_sgl_advance가 0까지 줄임. */
	s->iov_offset = 0;
	/* [한국어] 첫 iovec 안의 바이트 오프셋을 0으로 시작. */
	s->total_size = 0;
	/* [한국어] 누적 append 크기를 0으로 시작(생성 모드 전용 통계). */
}

/*
 * [한국어]
 * _dif_sgl_advance - SGL 커서를 step 바이트만큼 전진시킨다.
 *
 * @s:    [in/out] 진행할 SGL 커서.
 * @step: 전진할 바이트 수(블록 크기, PI 크기, 임의 길이 등).
 *
 * 동작: iov_offset에 step을 더한 뒤, iov_len을 넘으면 다음 iovec으로 이동(빠져나간
 * 만큼 빼서 새 iovec의 오프셋으로 만든다). 한 step이 여러 iovec에 걸칠 수 있으므로
 * while로 반복.
 *
 * 호출 체인:
 *   _dif_sgl_append_split / dif_generate / dif_verify / _dif_*_split → [_dif_sgl_advance]
 *
 * 실행 컨텍스트: 단일 SPDK thread.
 * 주의: iovcnt가 0이 되면 SGL 끝이며, 이후 _dif_sgl_get_buf 호출은 정의되지 않은 동작.
 *       호출자가 _dif_sgl_is_valid로 미리 충분한 크기를 검증해야 한다.
 */
static void
_dif_sgl_advance(struct _dif_sgl *s, uint32_t step)
{
	s->iov_offset += step;
	/* [한국어] 현재 iovec 안에서 step만큼 전진(오프셋 누적). */
	while (s->iovcnt != 0) {
		/* [한국어] 남은 iovec이 있는 동안 iovec 경계 정리 반복. */
		if (s->iov_offset < s->iov->iov_len) {
			/* [한국어] 아직 현재 iovec 안에 머무르므로 정리 종료. */
			break;
		}

		s->iov_offset -= s->iov->iov_len;
		/* [한국어] 현재 iovec을 다 소비했으므로 그 길이만큼 오프셋에서 차감. */
		s->iov++;
		/* [한국어] 다음 iovec으로 포인터 이동. */
		s->iovcnt--;
		/* [한국어] 남은 iovec 수 1 감소. 0이 되면 SGL 종료. */
	}
}

/*
 * [한국어]
 * _dif_sgl_get_buf - 현재 커서가 가리키는 위치의 (포인터, 잔여 길이)를 반환.
 *
 * @s:        SGL 커서.
 * @_buf:     [out, NULL 허용] 현재 위치의 포인터.
 * @_buf_len: [out, NULL 허용] 현재 iovec에서 남은 바이트 수.
 *
 * 호출 체인:
 *   dif_generate / dif_verify / *_split / _dif_remap_ref_tag → [_dif_sgl_get_buf]
 * 실행 컨텍스트: 단일 SPDK thread.
 */
static inline void
_dif_sgl_get_buf(struct _dif_sgl *s, uint8_t **_buf, uint32_t *_buf_len)
{
	if (_buf != NULL) {
		/* [한국어] 호출자가 포인터를 원하면 iov_base + iov_offset 반환. */
		*_buf = (uint8_t *)s->iov->iov_base + s->iov_offset;
	}
	if (_buf_len != NULL) {
		/* [한국어] 호출자가 잔여 길이를 원하면 (iov_len - iov_offset) 반환.
		 * 이는 "현재 iovec에서 끝까지 남은 바이트". 다음 iovec까지 알기 위해선
		 * 호출자가 advance를 통해 경계를 넘어야 한다. */
		*_buf_len = s->iov->iov_len - s->iov_offset;
	}
}

/*
 * [한국어]
 * _dif_sgl_append - 출력 SGL을 "생성"하는 모드: 새 iovec 한 개를 채운다.
 *
 * @s:        [in/out] 출력 SGL 커서(append 모드).
 * @data:     채울 iov_base.
 * @data_len: 채울 iov_len.
 * @return:   true이면 아직 빈 iovec 슬롯 남음, false이면 마지막 슬롯을 채운 상태.
 *
 * 사용처: spdk_dif_set_md_interleave_iovs - 메타 인터리브 버퍼에서 데이터 영역만
 * 가리키는 새 iovec 배열을 만들 때 사용. 입력 배열을 전진하지 않고 새 출력 배열을 채운다.
 * 호출 체인: spdk_dif_set_md_interleave_iovs → _dif_sgl_append_split → [_dif_sgl_append].
 */
static inline bool
_dif_sgl_append(struct _dif_sgl *s, uint8_t *data, uint32_t data_len)
{
	assert(s->iovcnt > 0);
	/* [한국어] 빈 슬롯이 있어야 한다. 호출자가 보장하지 않으면 디버그 빌드에서 즉시 검출. */
	s->iov->iov_base = data;
	/* [한국어] 새 iovec 슬롯의 base를 채움. */
	s->iov->iov_len = data_len;
	/* [한국어] 새 iovec 슬롯의 길이를 채움. */
	s->total_size += data_len;
	/* [한국어] 누적 출력 크기 갱신(나중에 호출자에게 mapped_len으로 보고). */
	s->iov++;
	/* [한국어] 다음 빈 슬롯으로 이동. */
	s->iovcnt--;
	/* [한국어] 남은 슬롯 수 1 감소. */

	if (s->iovcnt > 0) {
		return true;
		/* [한국어] 아직 빈 슬롯 있음 - 호출자에게 더 append 가능 알림. */
	} else {
		return false;
		/* [한국어] 빈 슬롯 소진 - 호출자가 더 이상 append하면 안 됨. */
	}
}

/*
 * [한국어]
 * _dif_sgl_append_split - src SGL의 data_len 바이트를 dst SGL에 분할 append.
 *
 * @dst:      [out] 출력 iovec 배열(append 모드).
 * @src:      [in/out] 입력 SGL(읽으면서 advance).
 * @data_len: dst에 추가할 총 바이트 수.
 * @return:   dst 슬롯이 부족하면 false, 정상 완료면 true.
 *
 * src의 한 iovec이 data_len보다 작을 수 있으므로 여러 dst 슬롯에 나눠 추가한다.
 * 호출 체인: spdk_dif_set_md_interleave_iovs → [_dif_sgl_append_split].
 */
static inline bool
_dif_sgl_append_split(struct _dif_sgl *dst, struct _dif_sgl *src, uint32_t data_len)
{
	uint8_t *buf;
	/* [한국어] src의 현재 위치 포인터. */
	uint32_t buf_len;
	/* [한국어] src의 현재 iovec에서 남은 길이. */

	while (data_len != 0) {
		/* [한국어] 요청한 길이가 모두 처리될 때까지 반복. */
		_dif_sgl_get_buf(src, &buf, &buf_len);
		/* [한국어] src 현재 위치에서 (포인터, 잔여) 획득. */
		buf_len = spdk_min(buf_len, data_len);
		/* [한국어] 이번에 처리할 양은 src 잔여와 남은 요청 중 작은 쪽. */

		if (!_dif_sgl_append(dst, buf, buf_len)) {
			/* [한국어] dst에 append. dst가 마지막 슬롯이면 false 반환됨. */
			return false;
			/* [한국어] dst 슬롯 부족 - 호출자에게 부분 처리 알림. */
		}

		_dif_sgl_advance(src, buf_len);
		/* [한국어] src 커서를 처리한 만큼 전진. */
		data_len -= buf_len;
		/* [한국어] 남은 요청량 갱신. */
	}

	return true;
	/* [한국어] data_len 전부 처리 완료. */
}

/*
 * [한국어]
 * _dif_sgl_is_bytes_multiple - 모든 iovec 길이가 bytes의 배수인지 확인.
 *
 * @s:     검사할 SGL.
 * @bytes: 정렬 단위(보통 block_size).
 *
 * 의미: true이면 어떤 iovec도 블록 경계를 가로지르지 않음 - "fast path"(블록 단위
 * 단일 패스) 사용 가능. false이면 한 블록이 여러 iovec에 걸칠 수 있어 "split" 경로 필요.
 * 호출 시점: 처음 한 번만(반복 시작 전).
 */
/* This function must be used before starting iteration. */
static bool
_dif_sgl_is_bytes_multiple(struct _dif_sgl *s, uint32_t bytes)
{
	int i;
	/* [한국어] iovec 인덱스. */

	for (i = 0; i < s->iovcnt; i++) {
		/* [한국어] 모든 iovec 순회. */
		if (s->iov[i].iov_len % bytes) {
			/* [한국어] 한 개라도 bytes의 배수가 아니면 split 경로 필요. */
			return false;
		}
	}

	return true;
	/* [한국어] 전부 정렬됨 - fast path 가능. */
}

/*
 * [한국어]
 * _dif_sgl_is_valid - SGL의 총 가용 바이트가 요구한 크기 이상인지 확인.
 *
 * @s:     검사할 SGL.
 * @bytes: 필요한 총 바이트(예: block_size * num_blocks).
 *
 * 사용처: 모든 공개 API가 처음에 호출해 buffer overflow를 사전 차단.
 * 호출 시점: 반복 시작 전.
 */
/* This function must be used before starting iteration. */
static bool
_dif_sgl_is_valid(struct _dif_sgl *s, uint32_t bytes)
{
	uint64_t total = 0;
	/* [한국어] 누적 가용 바이트(uint32 오버플로 방지로 64비트). */
	int i;

	for (i = 0; i < s->iovcnt; i++) {
		total += s->iov[i].iov_len;
		/* [한국어] 모든 iovec 길이 합산. */
	}

	return total >= bytes;
	/* [한국어] 합이 요구치 이상이면 OK. */
}

/*
 * [한국어]
 * _dif_sgl_copy - SGL 커서를 복제(스냅샷). _dif_remap_ref_tag에서 두 위치를 동시에
 * 진행할 때 사용.
 */
static void
_dif_sgl_copy(struct _dif_sgl *to, struct _dif_sgl *from)
{
	memcpy(to, from, sizeof(struct _dif_sgl));
	/* [한국어] _dif_sgl 구조체 통째로 복사. iovec 배열 자체는 공유(포인터만 복제). */
}

/*
 * [한국어]
 * _dif_is_disabled - DIF가 비활성 상태(SPDK_DIF_DISABLE)인지 빠르게 확인.
 *
 * 사용처: 모든 공개 API가 본격 처리 전에 호출. DISABLE이면 즉시 0 반환하여 NOP 통과.
 */
static bool
_dif_is_disabled(enum spdk_dif_type dif_type)
{
	if (dif_type == SPDK_DIF_DISABLE) {
		/* [한국어] DIF 보호가 꺼진 namespace - PI 계산/검증 모두 건너뜀. */
		return true;
	} else {
		return false;
	}
}

/*
 * [한국어]
 * _dif_size - PI 형식에 해당하는 PI 영역 크기(바이트) 반환.
 * 16b Guard=8B(T10 표준), 32b Guard=16B(NVMe 32b PI), 64b Guard=16B(NVMe 64b PI).
 */
static inline size_t
_dif_size(enum spdk_dif_pi_format dif_pi_format)
{
	uint8_t size;
	/* [한국어] 반환할 PI 영역 크기. */

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		/* [한국어] 16비트 Guard 형식: T10 표준 PI 8바이트. */
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g16);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		/* [한국어] 32비트 Guard 형식: NVMe 확장 PI 16바이트. */
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g32);
	} else {
		/* [한국어] 64비트 Guard 형식: NVMe 확장 PI 16바이트. */
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g64);
	}

	return size;
}

/*
 * [한국어]
 * spdk_dif_pi_format_get_size - 외부에 PI 영역 크기를 노출하는 thin wrapper.
 * 호출자(bdev_nvme 등)가 메타 버퍼 크기를 산정할 때 사용.
 */
uint32_t
spdk_dif_pi_format_get_size(enum spdk_dif_pi_format dif_pi_format)
{
	return _dif_size(dif_pi_format);
	/* [한국어] 내부 헬퍼로 위임. */
}

/*
 * [한국어]
 * _get_guard_interval - "Guard CRC 산출 길이" = 블록 시작부터 PI 시작까지의 거리 계산.
 *
 * @block_size:    한 블록의 총 크기(데이터+메타 또는 데이터만).
 * @md_size:       메타데이터 크기.
 * @dif_loc:       PI 위치 - false=메타 끝(표준), true=메타 시작부.
 * @md_interleave: true면 메타가 블록 끝에 인터리브, false면 분리(DIX).
 * @dif_size:      PI 영역 자체 크기(8 또는 16).
 * @return:        Guard CRC가 커버할 길이(바이트). 즉 블록 내 PI 시작 오프셋.
 *
 * T10/NVMe 스펙:
 *  - PI가 메타 마지막 8/16B에 위치(dif_loc=false): CRC는 블록 데이터 + PI 앞 메타까지 커버.
 *  - PI가 메타 시작 8/16B에 위치(dif_loc=true): CRC는 메타를 일절 커버하지 않음.
 *
 * 호출 체인: spdk_dif_ctx_init → [_get_guard_interval] → ctx->guard_interval에 저장.
 */
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
		/* [한국어] PI가 메타 끝에 위치(표준 위치). CRC 커버 범위: 블록 끝 직전 PI까지. */
		if (md_interleave) {
			return block_size - dif_size;
			/* [한국어] 인터리브: 데이터+메타-PI 직전까지가 CRC 대상. */
		} else {
			return md_size - dif_size;
			/* [한국어] DIX: 메타에서 PI 직전까지가 CRC 대상(데이터는 별도 처리). */
		}
	} else {
		/* For metadata formats with more than 8/16 bytes (depending on
		 * the PI format), if the DIF is contained in the first 8/16 bytes
		 * of metadata, then the CRC does not cover any metadata.
		 */
		/* [한국어] PI가 메타 시작부에 위치(SCSI 일부 변형). CRC는 메타를 커버하지 않음. */
		if (md_interleave) {
			return block_size - md_size;
			/* [한국어] 인터리브: 데이터만 CRC 대상(메타 시작부에 PI). */
		} else {
			return 0;
			/* [한국어] DIX: 메타 시작부 PI - 메타 영역 자체가 CRC 대상이 아님. */
		}
	}
}

/*
 * [한국어]
 * _dif_guard_size - PI 형식에 따른 Guard CRC 크기(바이트) 반환.
 * 16b=2B, 32b=4B, 64b=8B.
 */
static inline uint8_t
_dif_guard_size(enum spdk_dif_pi_format dif_pi_format)
{
	uint8_t size;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g16.guard);
		/* [한국어] g16.guard는 uint16_t = 2바이트. */
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g32.guard);
		/* [한국어] g32.guard는 uint32_t = 4바이트. */
	} else {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g64.guard);
		/* [한국어] g64.guard는 uint64_t = 8바이트. */
	}

	return size;
}

/*
 * [한국어]
 * _dif_set_guard - PI 영역의 Guard 필드를 big-endian으로 직렬화하여 기록.
 *
 * @dif:           PI 구조체 포인터(메모리상 PI 영역).
 * @guard:         계산된 CRC 값(상위 비트는 형식에 맞춰 잘림).
 * @dif_pi_format: 16/32/64비트 형식 선택.
 *
 * T10 PI는 모두 BE로 직렬화. to_be16/to_be32/to_be64는 spdk/endian.h 매크로.
 * 호출 체인: _dif_generate / _dif_remap_ref_tag → [_dif_set_guard].
 */
static inline void
_dif_set_guard(struct spdk_dif *dif, uint64_t guard, enum spdk_dif_pi_format dif_pi_format)
{
	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		/* [한국어] 16비트 Guard: 하위 16비트만 잘라 BE 저장. */
		to_be16(&(dif->g16.guard), (uint16_t)guard);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		/* [한국어] 32비트 Guard: 하위 32비트만 잘라 BE 저장. */
		to_be32(&(dif->g32.guard), (uint32_t)guard);
	} else {
		/* [한국어] 64비트 Guard: 64비트 전체를 BE 저장. */
		to_be64(&(dif->g64.guard), guard);
	}
}

/*
 * [한국어]
 * _dif_get_guard - PI 영역에서 Guard 필드를 읽어 host endian uint64로 반환.
 *
 * 16/32비트의 경우 상위 비트는 0으로 패딩됨.
 * 호출 체인: _dif_verify → [_dif_get_guard] → 재계산한 guard와 비교.
 */
static inline uint64_t
_dif_get_guard(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t guard;
	/* [한국어] 통일된 64비트 컨테이너로 반환. */

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		/* [한국어] 16비트 Guard를 BE에서 host endian uint16으로, uint64로 zero-extend. */
		guard = (uint64_t)from_be16(&(dif->g16.guard));
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		/* [한국어] 32비트 Guard를 zero-extend. */
		guard = (uint64_t)from_be32(&(dif->g32.guard));
	} else {
		/* [한국어] 64비트 Guard 그대로. */
		guard = from_be64(&(dif->g64.guard));
	}

	return guard;
}

/*
 * [한국어]
 * _dif_generate_guard - 주어진 버퍼에 대한 Guard CRC를 계산(누적).
 *
 * @guard_seed:    CRC 초기값(첫 호출은 ctx->guard_seed, 후속 호출은 직전 결과).
 * @buf:           CRC 대상 데이터 시작.
 * @buf_len:       CRC 대상 길이.
 * @dif_pi_format: 16/32/64 - 사용할 다항식 결정.
 * @return:        누적 CRC 결과.
 *
 * 다항식:
 *  - 16비트: T10 DIF CRC (다항식 0x8BB7) - lib/util/crc16.c.
 *  - 32비트: NVMe CRC32C (다항식 0x1EDC6F41 reflected) - lib/util/crc32c.c.
 *  - 64비트: NVMe CRC64 (다항식 0xAD93D23594C93659) - lib/util/crc64.c.
 *
 * 호출 체인: dif_generate / dif_verify / dix_* → [_dif_generate_guard]
 *  → spdk_crc16_t10dif / spdk_crc32c_nvme / spdk_crc64_nvme.
 * 실행 컨텍스트: 단일 SPDK thread. ISA-L 가속 가능(빌드 시).
 */
static inline uint64_t
_dif_generate_guard(uint64_t guard_seed, void *buf, size_t buf_len,
		    enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t guard;
	/* [한국어] 누적 결과 컨테이너. */

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		/* [한국어] T10 표준 16비트 CRC - SCSI/NVMe 공통 보호 다항식. */
		guard = (uint64_t)spdk_crc16_t10dif((uint16_t)guard_seed, buf, buf_len);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		/* [한국어] CRC32C(NVMe). spdk_crc32c_nvme는 (data, len, seed) 시그니처. */
		guard = (uint64_t)spdk_crc32c_nvme(buf, buf_len, guard_seed);
	} else {
		/* [한국어] CRC64-NVMe(64b Guard). 64비트 누적 결과 그대로. */
		guard = spdk_crc64_nvme(buf, buf_len, guard_seed);
	}

	return guard;
}

/*
 * [한국어]
 * dif_generate_guard_split - 분할된 SGL에서 [start, start+len) 범위의 Guard CRC 계산.
 *
 * @guard_seed:   CRC 시드(누적 시작값).
 * @sgl:          [in/out] 데이터 SGL(읽으면서 advance).
 * @start:        블록 내 시작 오프셋(스트림 모드에서 부분 처리 시 사용).
 * @len:          이번에 처리할 길이.
 * @ctx:          ctx->guard_interval, dif_flags, dif_pi_format 참조.
 * @return:       업데이트된 CRC.
 *
 * 한 블록이 여러 iovec에 걸쳐 있을 때 사용. CRC 대상은 ctx->guard_interval까지로 클립.
 * dif_flags에 SPDK_DIF_FLAGS_GUARD_CHECK가 없으면 데이터 advance만 하고 CRC 계산 생략.
 * 호출 체인: _dif_generate_split / _dif_verify_split / dif_insert_copy_split → [dif_generate_guard_split].
 */
static uint64_t
dif_generate_guard_split(uint64_t guard_seed, struct _dif_sgl *sgl, uint32_t start,
			 uint32_t len, const struct spdk_dif_ctx *ctx)
{
	uint64_t guard = guard_seed;
	/* [한국어] 누적 CRC. seed로 시작. */
	uint32_t offset, end, buf_len;
	/* [한국어] offset=현재 진행 위치, end=종료 경계, buf_len=이번 청크 길이. */
	uint8_t *buf;
	/* [한국어] 현재 SGL 위치의 데이터 포인터. */

	offset = start;
	/* [한국어] 진행 시작점 = start. */
	end = start + spdk_min(len, ctx->guard_interval - start);
	/* [한국어] 종료 경계 = start + min(len, guard_interval-start).
	 * 즉 CRC는 ctx->guard_interval을 절대 넘지 않는다(PI 영역 자체는 CRC에 포함 안 됨). */

	while (offset < end) {
		/* [한국어] 종료 경계까지 청크 단위로 진행. */
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		/* [한국어] 현재 위치의 (포인터, iovec 잔여) 획득. */
		buf_len = spdk_min(buf_len, end - offset);
		/* [한국어] 이번 청크 = iovec 잔여와 종료까지 남은 길이 중 작은 쪽. */

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			/* [한국어] Guard 검사가 활성일 때만 CRC 누적(비활성이면 시간 낭비 회피). */
			guard = _dif_generate_guard(guard, buf, buf_len, ctx->dif_pi_format);
		}

		_dif_sgl_advance(sgl, buf_len);
		/* [한국어] SGL 커서를 처리한 만큼 전진. */
		offset += buf_len;
		/* [한국어] 진행 위치 갱신. */
	}

	return guard;
	/* [한국어] 누적된 CRC 반환(다음 split 호출의 seed로 이어짐). */
}

/*
 * [한국어]
 * _dif_generate_guard_copy - 한 패스로 src→dst 복사 + CRC 계산을 동시에.
 *
 * @guard_seed:    CRC 시드.
 * @dst:           복사 대상.
 * @src:           원본.
 * @buf_len:       길이.
 * @dif_pi_format: 16/32/64.
 * @return:        누적 CRC.
 *
 * 16비트 형식은 ISA-L의 spdk_crc16_t10dif_copy로 SIMD 한 패스 가속.
 * 32/64비트는 별도 가속 함수가 없어 memcpy + CRC 두 패스로 처리.
 * 호출 체인: dif_insert_copy / dif_verify_copy / dif_strip_copy 계열에서 사용.
 * 메모리 트래픽 절감(대형 블록에서 의미 있음).
 */
static inline uint64_t
_dif_generate_guard_copy(uint64_t guard_seed, void *dst, void *src, size_t buf_len,
			 enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t guard;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		/* [한국어] 16b: SIMD 1-pass copy+CRC(memcpy 불필요). */
		guard = (uint64_t)spdk_crc16_t10dif_copy((uint16_t)guard_seed, dst, src, buf_len);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		memcpy(dst, src, buf_len);
		/* [한국어] 32b: 데이터 복사 후 CRC32C 계산(2-pass). */
		guard = (uint64_t)spdk_crc32c_nvme(src, buf_len, guard_seed);
	} else {
		memcpy(dst, src, buf_len);
		/* [한국어] 64b: 데이터 복사 후 CRC64 계산(2-pass). */
		guard = spdk_crc64_nvme(src, buf_len, guard_seed);
	}

	return guard;
}

/*
 * [한국어]
 * _dif_generate_guard_copy_split - 분할 SGL에서 src→dst 복사+CRC를 청크 단위로.
 *
 * @guard:         [in] 누적 CRC seed, [return] 갱신된 CRC.
 * @dst_sgl:       출력 SGL(advance됨).
 * @src_sgl:       입력 SGL(advance됨).
 * @data_len:      처리할 총 길이.
 * @dif_pi_format: 16/32/64.
 * @return:        업데이트된 CRC.
 *
 * src/dst 두 SGL의 iovec 경계가 다를 수 있으므로, 매 청크는 둘 중 작은 잔여로 결정.
 * 호출 체인: dif_insert_copy_split / dif_overwrite_copy_split / dif_strip_copy_split 등.
 */
static uint64_t
_dif_generate_guard_copy_split(uint64_t guard, struct _dif_sgl *dst_sgl,
			       struct _dif_sgl *src_sgl, uint32_t data_len,
			       enum spdk_dif_pi_format dif_pi_format)
{
	uint32_t offset = 0, src_len, dst_len, buf_len;
	/* [한국어] offset=진행 위치, src/dst_len=각 SGL의 현재 iovec 잔여, buf_len=이번 청크. */
	uint8_t *src, *dst;
	/* [한국어] 각 SGL의 현재 위치 포인터. */

	while (offset < data_len) {
		/* [한국어] 요청한 총 길이까지 반복. */
		_dif_sgl_get_buf(src_sgl, &src, &src_len);
		/* [한국어] src 현재 위치/잔여 획득. */
		_dif_sgl_get_buf(dst_sgl, &dst, &dst_len);
		/* [한국어] dst 현재 위치/잔여 획득. */
		buf_len = spdk_min(src_len, dst_len);
		/* [한국어] 이번 청크는 두 잔여 중 작은 쪽. */
		buf_len = spdk_min(buf_len, data_len - offset);
		/* [한국어] 그리고 남은 요청량을 초과하지 않도록 클램프. */

		guard = _dif_generate_guard_copy(guard, dst, src, buf_len, dif_pi_format);
		/* [한국어] 청크 단위로 1-pass 복사+CRC 누적. */

		_dif_sgl_advance(src_sgl, buf_len);
		/* [한국어] src 전진. */
		_dif_sgl_advance(dst_sgl, buf_len);
		/* [한국어] dst 전진. */
		offset += buf_len;
		/* [한국어] 누적 진행 갱신. */
	}

	return guard;
}

/*
 * [한국어]
 * _data_copy_split - CRC 계산 없이 src→dst 데이터만 복사(분할 SGL 지원).
 *
 * 사용처: SPDK_DIF_FLAGS_GUARD_CHECK가 꺼진 경로 / DISABLE 경로 - 단순 복사만 필요.
 * memcpy를 청크별 반복하므로 일반적으로 일반 memcpy보다 느리다(SGL 경계 처리 비용).
 */
static void
_data_copy_split(struct _dif_sgl *dst_sgl, struct _dif_sgl *src_sgl, uint32_t data_len)
{
	uint32_t offset = 0, src_len, dst_len, buf_len;
	uint8_t *src, *dst;

	while (offset < data_len) {
		/* [한국어] 총 length까지 청크 반복. */
		_dif_sgl_get_buf(src_sgl, &src, &src_len);
		/* [한국어] src 위치/잔여. */
		_dif_sgl_get_buf(dst_sgl, &dst, &dst_len);
		/* [한국어] dst 위치/잔여. */
		buf_len = spdk_min(src_len, dst_len);
		/* [한국어] 두 잔여 중 작은 쪽이 이번 청크 한계. */
		buf_len = spdk_min(buf_len, data_len - offset);
		/* [한국어] 남은 요청량으로 추가 클램프. */

		memcpy(dst, src, buf_len);
		/* [한국어] 순수 복사. CRC는 계산하지 않는다. */

		_dif_sgl_advance(src_sgl, buf_len);
		/* [한국어] src 전진. */
		_dif_sgl_advance(dst_sgl, buf_len);
		/* [한국어] dst 전진. */
		offset += buf_len;
	}
}

/*
 * [한국어]
 * _dif_apptag_offset - PI 영역 시작부터 app_tag 필드까지의 오프셋(=guard 크기).
 * Layout: [Guard | AppTag | RefTag/StorageTag]
 */
static inline uint8_t
_dif_apptag_offset(enum spdk_dif_pi_format dif_pi_format)
{
	return _dif_guard_size(dif_pi_format);
	/* [한국어] AppTag는 Guard 직후에 위치하므로 Guard 크기와 동일. */
}

/*
 * [한국어]
 * _dif_apptag_size - app_tag 필드 크기. 모든 PI 형식에서 16비트(2B)로 동일.
 */
static inline uint8_t
_dif_apptag_size(void)
{
	return SPDK_SIZEOF_MEMBER(struct spdk_dif, g16.app_tag);
	/* [한국어] g16.app_tag = uint16_t = 2바이트. 16/32/64 모두 동일하므로 g16 기준. */
}

/*
 * [한국어]
 * _dif_set_apptag - PI 영역의 app_tag 필드를 BE로 직렬화하여 기록.
 * AppTag는 모든 형식에서 16비트(공통).
 */
static inline void
_dif_set_apptag(struct spdk_dif *dif, uint16_t app_tag, enum spdk_dif_pi_format dif_pi_format)
{
	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		/* [한국어] g16.app_tag에 BE로 저장. */
		to_be16(&(dif->g16.app_tag), app_tag);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		/* [한국어] g32.app_tag에 BE로 저장. */
		to_be16(&(dif->g32.app_tag), app_tag);
	} else {
		/* [한국어] g64.app_tag에 BE로 저장. */
		to_be16(&(dif->g64.app_tag), app_tag);
	}
}

/*
 * [한국어]
 * _dif_get_apptag - PI 영역에서 app_tag를 읽어 host endian uint16으로 반환.
 */
static inline uint16_t
_dif_get_apptag(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	uint16_t app_tag;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		app_tag = from_be16(&(dif->g16.app_tag));
		/* [한국어] g16.app_tag BE → host. */
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		app_tag = from_be16(&(dif->g32.app_tag));
		/* [한국어] g32.app_tag BE → host. */
	} else {
		app_tag = from_be16(&(dif->g64.app_tag));
		/* [한국어] g64.app_tag BE → host. */
	}

	return app_tag;
}

/*
 * [한국어]
 * _dif_apptag_ignore - app_tag가 0xFFFF(SPDK_DIF_APPTAG_IGNORE)인지 검사.
 *
 * T10 스펙: app_tag=0xFFFF는 "이 블록 PI 검증을 무시"를 의미.
 * Type 1/2: AppTag만 0xFFFF면 모든 검사 비활성. Type 3: AppTag+RefTag 모두 ignore일 때.
 */
static inline bool
_dif_apptag_ignore(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	return _dif_get_apptag(dif, dif_pi_format) == SPDK_DIF_APPTAG_IGNORE;
	/* [한국어] SPDK_DIF_APPTAG_IGNORE == 0xFFFF (include/spdk/dif.h). */
}

/*
 * [한국어]
 * _dif_reftag_offset - PI 영역 시작부터 reftag(stor_ref_space) 시작까지의 오프셋.
 *
 * 16b: Guard(2) + AppTag(2) = 4
 * 32b: Guard(4) + AppTag(2) + p1(2) = 8 (p2부터가 reftag 영역)
 * 64b: Guard(8) + AppTag(2) = 10 (p1부터가 reftag 영역, 6B)
 */
static inline uint8_t
_dif_reftag_offset(enum spdk_dif_pi_format dif_pi_format)
{
	uint8_t offset;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		/* [한국어] 16b: 단순히 apptag 직후. */
		offset = _dif_apptag_offset(dif_pi_format) + _dif_apptag_size();
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		/* [한국어] 32b: stor_ref_space_p1(2B)는 storage tag로 사용되어 reftag와 분리.
		 * reftag는 stor_ref_space_p2(8B)에 들어감. */
		offset = _dif_apptag_offset(dif_pi_format) + _dif_apptag_size()
			 + SPDK_SIZEOF_MEMBER(struct spdk_dif, g32.stor_ref_space_p1);
	} else {
		/* [한국어] 64b: apptag 직후가 reftag 영역(6B = p1 2B + p2 4B). */
		offset = _dif_apptag_offset(dif_pi_format) + _dif_apptag_size();
	}

	return offset;
}

/*
 * [한국어]
 * _dif_reftag_size - reference tag 영역 크기(바이트).
 *
 * 16b: 4B(32비트), 32b: 8B(64비트), 64b: 6B(48비트, MASK_64).
 */
static inline uint8_t
_dif_reftag_size(enum spdk_dif_pi_format dif_pi_format)
{
	uint8_t size;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g16.stor_ref_space);
		/* [한국어] T10 표준 4B reftag(LBA 하위 32비트). */
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g32.stor_ref_space_p2);
		/* [한국어] NVMe 32b PI: reftag는 p2 영역 8B. p1은 storage tag로 분리. */
	} else {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g64.stor_ref_space_p1) +
		       SPDK_SIZEOF_MEMBER(struct spdk_dif, g64.stor_ref_space_p2);
		/* [한국어] NVMe 64b PI: reftag는 p1+p2 = 6B(48비트). */
	}

	return size;
}

/*
 * [한국어]
 * _dif_set_reftag - PI 영역의 reference tag 필드를 BE로 직렬화하여 기록.
 *
 * 형식별 분할 저장:
 *  - 16b: g16.stor_ref_space(4B)에 하위 32비트만.
 *  - 32b: g32.stor_ref_space_p2(8B)에 64비트.
 *  - 64b: 48비트를 g64.stor_ref_space_p1(상위 16) + p2(하위 32)로 분할.
 *
 * 호출 체인: _dif_generate / _dif_remap_ref_tag → [_dif_set_reftag].
 */
static inline void
_dif_set_reftag(struct spdk_dif *dif, uint64_t ref_tag, enum spdk_dif_pi_format dif_pi_format)
{
	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		/* [한국어] 16b: 32비트로 잘라 BE 저장. */
		to_be32(&(dif->g16.stor_ref_space), (uint32_t)ref_tag);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		/* [한국어] 32b: 64비트 reftag를 p2(8B)에 BE 저장. */
		to_be64(&(dif->g32.stor_ref_space_p2), ref_tag);
	} else {
		/* [한국어] 64b: 48비트 reftag를 16+32로 분할 저장.
		 * p1에는 (ref_tag >> 32)의 하위 16비트, p2에는 ref_tag의 하위 32비트. */
		to_be16(&(dif->g64.stor_ref_space_p1), (uint16_t)(ref_tag >> 32));
		to_be32(&(dif->g64.stor_ref_space_p2), (uint32_t)ref_tag);
	}
}

/*
 * [한국어]
 * _dif_get_reftag - PI 영역의 reference tag 필드를 읽어 host endian uint64로 반환.
 *
 * 16b: 32비트 zero-extend / 32b: 64비트 그대로 / 64b: 16+32 → 48비트 결합.
 */
static inline uint64_t
_dif_get_reftag(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t ref_tag;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		ref_tag = (uint64_t)from_be32(&(dif->g16.stor_ref_space));
		/* [한국어] 16b: 32비트 BE → host, 64비트로 zero-extend. */
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		ref_tag = from_be64(&(dif->g32.stor_ref_space_p2));
		/* [한국어] 32b: p2의 64비트를 그대로 reftag로. */
	} else {
		ref_tag = (uint64_t)from_be16(&(dif->g64.stor_ref_space_p1));
		/* [한국어] 64b 1단계: p1(16비트) → 64비트 컨테이너. */
		ref_tag <<= 32;
		/* [한국어] 64b 2단계: 상위 16비트 위치로 이동(48비트 reftag의 상위 16). */
		ref_tag |= (uint64_t)from_be32(&(dif->g64.stor_ref_space_p2));
		/* [한국어] 64b 3단계: p2(32비트)와 OR해 48비트 reftag 완성. */
	}

	return ref_tag;
}

/*
 * [한국어]
 * _dif_reftag_match - PI에 저장된 reftag와 기대값(LBA 기반)이 일치하는지 검사.
 *
 * @dif:           PI 구조체.
 * @ref_tag:       기대값(보통 ctx->init_ref_tag + offset_blocks).
 * @dif_pi_format: 형식.
 * @return:        일치하면 true.
 *
 * 형식별 마스크 적용으로 비트 폭 차이를 흡수(16b는 32비트, 64b는 48비트).
 */
static inline bool
_dif_reftag_match(struct spdk_dif *dif, uint64_t ref_tag,
		  enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t _ref_tag;
	/* [한국어] PI에서 읽은 reftag 값. */
	bool match;
	/* [한국어] 일치 여부. */

	_ref_tag = _dif_get_reftag(dif, dif_pi_format);
	/* [한국어] PI에서 reftag 추출. */

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		/* [한국어] 16b: 기대값의 하위 32비트와 비교. */
		match = (_ref_tag == (ref_tag & REFTAG_MASK_16));
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		/* [한국어] 32b: 64비트 그대로 비교. */
		match = (_ref_tag == ref_tag);
	} else {
		/* [한국어] 64b: 기대값의 하위 48비트만 비교(REFTAG_MASK_64). */
		match = (_ref_tag == (ref_tag & REFTAG_MASK_64));
	}

	return match;
}

/*
 * [한국어]
 * _dif_reftag_ignore - reftag 영역이 모두 0xFF인지 검사("ignore" 표시).
 *
 * Type 3에서 reftag=0xFFFFFFFF / 0xFFFFFFFFFFFFFFFF는 "이 블록 검사 무시" 의미.
 * REFTAG_MASK_32 = 0xFFFFFFFFFFFFFFFF (full mask) - 64비트 모든 비트 1.
 */
static inline bool
_dif_reftag_ignore(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	return _dif_reftag_match(dif, REFTAG_MASK_32, dif_pi_format);
	/* [한국어] _dif_reftag_match는 형식별 마스크 적용 후 비교 - all-FF 매칭 검사. */
}

/*
 * [한국어]
 * _dif_ignore - 이 PI에 대해 모든 검사(GUARD/APPTAG/REFTAG)를 건너뛰어야 하는지 판단.
 *
 * T10/NVMe 스펙:
 *  - Type 1/2: AppTag=0xFFFF면 모든 검사 무시.
 *  - Type 3:   AppTag=0xFFFF AND RefTag=all-FF면 모든 검사 무시.
 *
 * 사용처: _dif_verify, _dif_remap_ref_tag 등에서 검사 진입 직전 판단.
 */
static bool
_dif_ignore(struct spdk_dif *dif, const struct spdk_dif_ctx *ctx)
{
	switch (ctx->dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		/* If Type 1 or 2 is used, then all DIF checks are disabled when
		 * the Application Tag is 0xFFFF.
		 */
		/* [한국어] Type 1/2: AppTag=0xFFFF만으로 ignore 결정. */
		if (_dif_apptag_ignore(dif, ctx->dif_pi_format)) {
			return true;
		}
		break;
	case SPDK_DIF_TYPE3:
		/* If Type 3 is used, then all DIF checks are disabled when the
		 * Application Tag is 0xFFFF and the Reference Tag is 0xFFFFFFFF
		 * or 0xFFFFFFFFFFFFFFFF depending on the PI format.
		 */
		/* [한국어] Type 3: AppTag와 RefTag 둘 다 all-FF 상태일 때만 ignore. */
		if (_dif_apptag_ignore(dif, ctx->dif_pi_format) &&
		    _dif_reftag_ignore(dif, ctx->dif_pi_format)) {
			return true;
		}
		break;
	default:
		/* [한국어] DISABLE 등은 여기서 처리되지 않음(상위에서 별도 처리). */
		break;
	}

	return false;
	/* [한국어] 위 조건에 해당 안 되면 정상 검사 진행. */
}

/*
 * [한국어]
 * _dif_pi_format_is_valid - opts->dif_pi_format이 알려진 enum 값인지 검사.
 * spdk_dif_ctx_init의 인자 검증용.
 */
static bool
_dif_pi_format_is_valid(enum spdk_dif_pi_format dif_pi_format)
{
	switch (dif_pi_format) {
	case SPDK_DIF_PI_FORMAT_16:
	case SPDK_DIF_PI_FORMAT_32:
	case SPDK_DIF_PI_FORMAT_64:
		/* [한국어] 세 형식 모두 유효. */
		return true;
	default:
		/* [한국어] enum 외 값은 잘못된 입력. */
		return false;
	}
}

/*
 * [한국어]
 * _dif_type_is_valid - dif_type이 알려진 enum 값인지 검사.
 * DISABLE/Type1/Type2/Type3만 허용.
 */
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
	/* [한국어] 데이터 영역만의 크기(메타 제외). 인터리브면 block_size-md_size, DIX면 block_size. */
	enum spdk_dif_pi_format dif_pi_format = SPDK_DIF_PI_FORMAT_16;
	/* [한국어] PI 형식 기본값은 16비트 Guard(T10 표준). opts로 16/32/64 선택 가능. */

	if (opts != NULL) {
		/* [한국어] 호출자가 ext opts를 제공하면 PI format 적용. NULL이면 기본 16비트. */
		if (!_dif_pi_format_is_valid(opts->dif_pi_format)) {
			SPDK_ERRLOG("No valid DIF PI format provided.\n");
			/* [한국어] 알려지지 않은 형식 - 즉시 실패. */
			return -EINVAL;
		}

		dif_pi_format = opts->dif_pi_format;
		/* [한국어] 검증된 형식 채택. */
	}

	if (!_dif_type_is_valid(dif_type)) {
		SPDK_ERRLOG("No valid DIF type was provided.\n");
		/* [한국어] DISABLE/Type1/2/3 외 값이면 실패. */
		return -EINVAL;
	}

	if (md_size < _dif_size(dif_pi_format)) {
		SPDK_ERRLOG("Metadata size is smaller than DIF size.\n");
		/* [한국어] 메타가 PI 영역(8/16B)보다 작으면 PI를 담을 공간 부족. */
		return -EINVAL;
	}

	if (md_interleave) {
		/* [한국어] 메타가 데이터 블록에 인터리브된 경우(NVMe LBA format A 등). */
		if (block_size < md_size) {
			SPDK_ERRLOG("Block size is smaller than DIF size.\n");
			/* [한국어] 블록이 메타보다 작으면 데이터 영역이 음수가 됨 - 불가. */
			return -EINVAL;
		}
		data_block_size = block_size - md_size;
		/* [한국어] 데이터 영역 = 전체 블록 - 메타. */
	} else {
		data_block_size = block_size;
		/* [한국어] DIX 모드: block_size 자체가 데이터 크기(메타는 별도 SGL). */
	}

	if (data_block_size == 0) {
		SPDK_ERRLOG("Zero data block size is not allowed\n");
		/* [한국어] 데이터 영역 0은 의미 없음. */
		return -EINVAL;
	}

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		/* [한국어] T10 16b PI: 전통적 섹터 크기인 512B의 배수만 허용. */
		if ((data_block_size % 512) != 0) {
			SPDK_ERRLOG("Data block size should be a multiple of 512B\n");
			return -EINVAL;
		}
	} else {
		/* [한국어] NVMe 32b/64b PI: 4KB 정렬 블록만 지원(스펙 권장). */
		if ((data_block_size % 4096) != 0) {
			SPDK_ERRLOG("Data block size should be a multiple of 4kB\n");
			return -EINVAL;
		}
	}

	ctx->block_size = block_size;
	/* [한국어] 블록 총 크기 저장. */
	ctx->md_size = md_size;
	/* [한국어] 메타 크기 저장. */
	ctx->md_interleave = md_interleave;
	/* [한국어] 인터리브 여부 저장 - 이후 모든 함수가 이 플래그로 분기. */
	ctx->dif_pi_format = dif_pi_format;
	/* [한국어] PI 형식 저장. */
	ctx->guard_interval = _get_guard_interval(block_size, md_size, dif_loc, md_interleave,
			      _dif_size(ctx->dif_pi_format));
	/* [한국어] CRC 커버 길이(블록 내 PI 시작 오프셋) 사전 계산해 캐시. */
	ctx->dif_type = dif_type;
	/* [한국어] T10 PI 타입 저장. */
	ctx->dif_flags = dif_flags;
	/* [한국어] GUARD/APPTAG/REFTAG 검사 활성 비트 + NVME_PRACT 플래그 저장. */
	ctx->init_ref_tag = init_ref_tag;
	/* [한국어] reftag 시작값(보통 시작 LBA의 하위 32비트). */
	ctx->apptag_mask = apptag_mask;
	/* [한국어] AppTag 검사 시 적용할 비트 마스크. */
	ctx->app_tag = app_tag;
	/* [한국어] 기대되는 AppTag 값. */
	ctx->data_offset = data_offset;
	/* [한국어] 스트림 모드용 - 이 ctx로 처리한 누적 데이터 오프셋. */
	ctx->ref_tag_offset = data_offset / data_block_size;
	/* [한국어] data_offset이 가리키는 시작 블록 번호 - reftag 계산에 가산. */
	ctx->last_guard = guard_seed;
	/* [한국어] 스트림 모드의 누적 Guard CRC 시드(부분 처리 후 다음 호출에 이어짐). */
	ctx->guard_seed = guard_seed;
	/* [한국어] 매 블록 CRC 시작값(보통 0). 호출자가 0 외 값을 원할 때 설정. */
	ctx->remapped_init_ref_tag = 0;
	/* [한국어] remap 시 새 시작 reftag - spdk_dif_ctx_set_remapped_init_ref_tag로 설정. */

	return 0;
	/* [한국어] 모든 검증 통과 - ctx 사용 가능. */
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
	/* [한국어] 데이터 영역 크기(인터리브 vs DIX 분기). */

	if (ctx->md_interleave) {
		data_block_size = ctx->block_size - ctx->md_size;
		/* [한국어] 인터리브: 블록에서 메타 빼면 데이터 영역. */
	} else {
		data_block_size = ctx->block_size;
		/* [한국어] DIX: 블록 자체가 데이터 영역. */
	}

	ctx->data_offset = data_offset;
	/* [한국어] 진행 위치 갱신. */
	ctx->ref_tag_offset = data_offset / data_block_size;
	/* [한국어] data_offset에 해당하는 블록 번호 = reftag 가산값. */
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
	/* [한국어] remap 후의 새 시작 reftag를 ctx에 저장. spdk_dif_remap_ref_tag가 사용. */
}

/*
 * [한국어]
 * _dif_generate - PI 영역(8/16B)에 Guard/AppTag/RefTag 세 필드를 채운다.
 *
 * @_dif:          PI 영역 메모리 주소(블록 끝/시작 또는 임시 버퍼).
 * @guard:         이미 계산된 Guard CRC 값.
 * @offset_blocks: 이번 블록의 인덱스(시작 블록 기준 0부터).
 * @ctx:           PI 형식, 검사 플래그, 시작 reftag/apptag 정보.
 *
 * 동작: dif_flags 비트별로 활성 필드만 채우고, 비활성 필드는 0으로 채움.
 * Type 1/2: 매 블록마다 reftag 1씩 증가. Type 3: 모든 블록에 동일 reftag.
 * SPDK_DIF_REFTAG_IGNORE 시작값 사용 시 PI에 all-FF 저장으로 ignore 표시.
 *
 * 호출 체인: dif_generate / _dif_generate_split / _dif_insert_copy 등 → [_dif_generate].
 * 실행 컨텍스트: 단일 SPDK thread.
 */
static void
_dif_generate(void *_dif, uint64_t guard, uint32_t offset_blocks,
	      const struct spdk_dif_ctx *ctx)
{
	struct spdk_dif *dif = _dif;
	/* [한국어] void* 인자를 PI union 타입으로 캐스팅. */
	uint64_t ref_tag;
	/* [한국어] 이 블록에 기록할 reftag 값. */

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		/* [한국어] Guard 검사 활성: 계산된 CRC를 PI에 기록. */
		_dif_set_guard(dif, guard, ctx->dif_pi_format);
	} else {
		/* [한국어] 비활성: 0으로 채워 결정적 값 유지. */
		_dif_set_guard(dif, 0, ctx->dif_pi_format);
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_APPTAG_CHECK) {
		/* [한국어] AppTag 검사 활성: ctx->app_tag 값 기록. */
		_dif_set_apptag(dif, ctx->app_tag, ctx->dif_pi_format);
	} else {
		/* [한국어] 비활성: 0으로 채움. */
		_dif_set_apptag(dif, 0, ctx->dif_pi_format);
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK) {
		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag
		 * remains the same as the initial reference tag.
		 */
		/* [한국어] T10 스펙: Type 1/2는 블록마다 reftag 증가, Type 3는 고정. */
		if (ctx->dif_type != SPDK_DIF_TYPE3) {
			ref_tag = ctx->init_ref_tag + ctx->ref_tag_offset + offset_blocks;
			/* [한국어] Type 1/2: init + (data_offset 위치 블록 번호) + 이번 블록 인덱스. */
		} else {
			ref_tag = ctx->init_ref_tag + ctx->ref_tag_offset;
			/* [한국어] Type 3: 모든 블록에 동일 값(블록 인덱스 가산 안 함). */
		}

		/* Overwrite reference tag if initialization reference tag is SPDK_DIF_REFTAG_IGNORE */
		/* [한국어] 호출자가 reftag 검사 면제를 원하면 init=SPDK_DIF_REFTAG_IGNORE(0xFFFFFFFF)로
		 * 지정. PI에 형식별 max 값(all-FF)을 기록 → 검증 시 _dif_reftag_ignore가 ignore로 인식. */
		if (ctx->init_ref_tag == SPDK_DIF_REFTAG_IGNORE) {
			if (ctx->dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
				ref_tag = REFTAG_MASK_16;
				/* [한국어] 16b PI: 32비트 max(0xFFFFFFFF). */
			} else if (ctx->dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
				ref_tag = REFTAG_MASK_32;
				/* [한국어] 32b PI: 64비트 max. */
			} else {
				ref_tag = REFTAG_MASK_64;
				/* [한국어] 64b PI: 48비트 max. */
			}
		}

		_dif_set_reftag(dif, ref_tag, ctx->dif_pi_format);
		/* [한국어] PI 영역에 reftag 기록. */
	} else {
		_dif_set_reftag(dif, 0, ctx->dif_pi_format);
		/* [한국어] reftag 검사 비활성: 0 기록. */
	}
}

/*
 * [한국어]
 * dif_generate - "fast path": SGL의 모든 iovec이 블록 크기의 배수일 때.
 *
 * 한 블록이 항상 단일 iovec 안에 완전히 담겨 있으므로, 블록마다 한 번에
 * Guard CRC를 계산하고 PI를 기록할 수 있다.
 *
 * 호출 체인: spdk_dif_generate(SGL이 정렬된 경우) → [dif_generate].
 */
static void
dif_generate(struct _dif_sgl *sgl, uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;
	/* [한국어] 0..num_blocks-1 인덱스. reftag 계산에 사용. */
	uint8_t *buf;
	/* [한국어] 현재 블록 시작 포인터. */
	uint64_t guard = 0;
	/* [한국어] 매 블록마다 새로 계산하는 CRC. */

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		/* [한국어] 모든 블록 순회. */
		_dif_sgl_get_buf(sgl, &buf, NULL);
		/* [한국어] 현재 블록의 시작 주소 획득(잔여 길이는 사용 안 함). */

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			/* [한국어] Guard 검사 활성: 데이터+(있다면)앞쪽 메타까지 CRC 계산. */
			guard = _dif_generate_guard(ctx->guard_seed, buf, ctx->guard_interval, ctx->dif_pi_format);
		}

		_dif_generate(buf + ctx->guard_interval, guard, offset_blocks, ctx);
		/* [한국어] PI 영역(blockstart + guard_interval) 채우기. */

		_dif_sgl_advance(sgl, ctx->block_size);
		/* [한국어] 다음 블록으로 커서 이동. */
	}
}

/*
 * [한국어]
 * dif_store_split - 임시 buffer에 만든 PI(8/16B)를 분할 SGL의 PI 영역에 기록.
 *
 * 한 PI 영역이 여러 iovec에 걸칠 수 있으므로 청크 단위로 복사. PI 영역 이후 메타가
 * 더 있으면 그 부분은 건너뛰기만 함(쓰지 않음).
 *
 * 호출 체인: _dif_generate_split / dif_insert_copy_split / _dif_overwrite_copy_split → [dif_store_split].
 */
static void
dif_store_split(struct _dif_sgl *sgl, struct spdk_dif *dif,
		const struct spdk_dif_ctx *ctx)
{
	uint32_t offset = 0, rest_md_len, buf_len;
	/* [한국어] offset=현재 진행 위치, rest_md_len=PI영역+이후 메타 합계, buf_len=청크 길이. */
	uint8_t *buf;

	rest_md_len = ctx->block_size - ctx->guard_interval;
	/* [한국어] 블록 끝까지 = PI 영역 + (있을 수 있는) 그 뒤 메타. */

	while (offset < rest_md_len) {
		/* [한국어] 블록 끝까지 청크 반복. */
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		/* [한국어] 현재 위치/잔여 획득. */

		if (offset < _dif_size(ctx->dif_pi_format)) {
			/* [한국어] PI 영역 안: 임시 dif에서 복사해 기록. */
			buf_len = spdk_min(buf_len, _dif_size(ctx->dif_pi_format) - offset);
			memcpy(buf, (uint8_t *)dif + offset, buf_len);
		} else {
			/* [한국어] PI 이후 메타 영역: 건드리지 않고 advance만. */
			buf_len = spdk_min(buf_len, rest_md_len - offset);
		}

		_dif_sgl_advance(sgl, buf_len);
		/* [한국어] SGL 커서 전진. */
		offset += buf_len;
		/* [한국어] 진행 위치 갱신. */
	}
}

/*
 * [한국어]
 * _dif_generate_split - 블록 일부 또는 전부에 대해 CRC 누적; 블록 완성 시 PI 기록.
 *
 * @sgl:            데이터 SGL.
 * @offset_in_block: 이번 회차의 블록 내 시작 위치(스트림 모드 청크별로 누적).
 * @data_len:       이번 회차 처리 길이.
 * @guard:          누적 CRC 시드.
 * @offset_blocks:  이번 블록 인덱스.
 * @ctx:            PI 컨텍스트.
 * @return:         업데이트된 CRC(블록 완성 시 reset된 시드).
 *
 * 한 블록을 한 번에 처리하지 못할 때 사용. 누적 CRC는 호출 간 ctx->last_guard로 보존.
 * 블록 끝까지 도달하면 _dif_generate로 PI 채우고 dif_store_split로 SGL에 기록.
 */
static uint64_t
_dif_generate_split(struct _dif_sgl *sgl, uint32_t offset_in_block, uint32_t data_len,
		    uint64_t guard, uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	struct spdk_dif dif = {};
	/* [한국어] 임시 PI 버퍼(스택). 완성된 PI를 SGL에 기록하기 전 저장소. */

	assert(offset_in_block < ctx->guard_interval);
	/* [한국어] 시작 위치는 PI 영역 안일 수 없음(PI는 누적 끝에서 한 번에 기록). */
	assert(offset_in_block + data_len < ctx->guard_interval ||
	       offset_in_block + data_len == ctx->block_size);
	/* [한국어] 끝 위치는 PI 영역 직전이거나(부분 처리) 블록 끝(완성)이어야 함. */

	/* Compute CRC over split logical block data. */
	guard = dif_generate_guard_split(guard, sgl, offset_in_block, data_len, ctx);
	/* [한국어] 이번 청크의 데이터에 대해 CRC 누적. */

	if (offset_in_block + data_len < ctx->guard_interval) {
		return guard;
		/* [한국어] 아직 블록 미완성 - 누적 CRC를 호출자에게 돌려주고 다음 회차로. */
	}

	/* If a whole logical block data is parsed, generate DIF
	 * and save it to the temporary DIF area.
	 */
	/* [한국어] 블록 완성 - PI 영역에 들어갈 값 계산. */
	_dif_generate(&dif, guard, offset_blocks, ctx);

	/* Copy generated DIF field to the split DIF field, and then
	 * skip metadata field after DIF field (if any).
	 */
	/* [한국어] 임시 PI를 SGL의 PI 영역에 기록(분할된 메타 처리). */
	dif_store_split(sgl, &dif, ctx);

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
		/* [한국어] 다음 블록을 위해 CRC 시드 리셋. */
	}

	return guard;
	/* [한국어] 다음 블록을 위해 리셋된 시드 반환. */
}

/*
 * [한국어]
 * dif_generate_split - 분할 SGL 모드에서 num_blocks 블록 전체에 PI 생성.
 *
 * SGL의 iovec 경계가 블록 경계와 맞지 않을 때 사용. 매 블록을 _dif_generate_split로 처리.
 * 호출 체인: spdk_dif_generate(SGL 비정렬) → [dif_generate_split].
 */
static void
dif_generate_split(struct _dif_sgl *sgl, uint32_t num_blocks,
		   const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;
	uint64_t guard = 0;
	/* [한국어] 매 블록마다 새 시드로 시작. */

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
		/* [한국어] Guard 검사 활성 시 ctx의 시드(보통 0)로 시작. */
	}

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		/* [한국어] 매 블록을 0..block_size 한 번에 split 처리. */
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
	/* [한국어] 입력 SGL을 추적할 커서. */

	_dif_sgl_init(&sgl, iovs, iovcnt);
	/* [한국어] iovec 배열의 시작에서 커서 초기화. */

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		/* [한국어] 입력 버퍼가 num_blocks*block_size보다 작으면 오버플로 위험 - 거부. */
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
		/* [한국어] DIF 비활성 namespace - 처리 없음(OK). */
	}

	if (_dif_sgl_is_bytes_multiple(&sgl, ctx->block_size)) {
		/* [한국어] SGL이 블록 경계로 정렬: fast path. */
		dif_generate(&sgl, num_blocks, ctx);
	} else {
		/* [한국어] 비정렬: 한 블록이 여러 iovec에 걸쳐 있을 수 있어 split 경로. */
		dif_generate_split(&sgl, num_blocks, ctx);
	}

	return 0;
}

/*
 * [한국어]
 * _dif_error_set - 검증 실패 시 호출자에게 위치/타입/expected/actual을 보고할 err_blk를 채움.
 *
 * @err_blk:    [out, NULL 허용] 출력 구조체. NULL이면 무시.
 * @err_type:   SPDK_DIF_GUARD_ERROR / APPTAG_ERROR / REFTAG_ERROR 중 하나.
 * @expected:   기대했던 값.
 * @actual:     실제 PI에서 읽은 값.
 * @err_offset: 어느 블록에서 실패했는지(블록 인덱스).
 */
static void
_dif_error_set(struct spdk_dif_error *err_blk, uint8_t err_type,
	       uint64_t expected, uint64_t actual, uint32_t err_offset)
{
	if (err_blk) {
		/* [한국어] 호출자가 err_blk를 제공한 경우만 채움. */
		err_blk->err_type = err_type;
		/* [한국어] 어떤 필드(GUARD/APPTAG/REFTAG)에서 실패했는지. */
		err_blk->expected = expected;
		/* [한국어] 검증자가 기대한 값. */
		err_blk->actual = actual;
		/* [한국어] PI에서 실제 읽힌 값. */
		err_blk->err_offset = err_offset;
		/* [한국어] 실패 블록 인덱스(시작 LBA로부터의 오프셋). */
	}
}

/*
 * [한국어]
 * _dif_reftag_check - reftag 검사. 불일치 시 err_blk 채우고 false 반환.
 *
 * Type 1/2: PI의 reftag와 expected_reftag(LBA 기반)가 매치해야 함.
 * Type 3: reftag 검사 면제(스펙).
 */
static bool
_dif_reftag_check(struct spdk_dif *dif, const struct spdk_dif_ctx *ctx,
		  uint64_t expected_reftag, uint32_t offset_blocks, struct spdk_dif_error *err_blk)
{
	uint64_t reftag;
	/* [한국어] 실패 시 actual로 보고할 PI에서 읽은 reftag 값. */

	if (ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK) {
		/* [한국어] reftag 검사 활성일 때만 진행. */
		switch (ctx->dif_type) {
		case SPDK_DIF_TYPE1:
		case SPDK_DIF_TYPE2:
			/* Compare the DIF Reference Tag field to the passed Reference Tag.
			 * The passed Reference Tag will be the least significant 4 bytes
			 * or 8 bytes (depending on the PI format)
			 * of the LBA when Type 1 is used, and application specific value
			 * if Type 2 is used.
			 */
			/* [한국어] Type 1/2: expected는 LBA 하위 비트(또는 응용 정의). */
			if (!_dif_reftag_match(dif, expected_reftag, ctx->dif_pi_format)) {
				/* [한국어] 불일치 - 에러 정보 수집. */
				reftag = _dif_get_reftag(dif, ctx->dif_pi_format);
				_dif_error_set(err_blk, SPDK_DIF_REFTAG_ERROR, expected_reftag,
					       reftag, offset_blocks);
				SPDK_ERRLOG("Failed to compare Ref Tag: LBA=%" PRIu64 "," \
					    " Expected=%lx, Actual=%lx\n",
					    expected_reftag, expected_reftag, reftag);
				/* [한국어] 운영자 디버깅용 로그(SPDK_ERRLOG는 stderr 또는 syslog). */
				return false;
			}
			break;
		case SPDK_DIF_TYPE3:
			/* For Type 3, computed Reference Tag remains unchanged.
			 * Hence ignore the Reference Tag field.
			 */
			/* [한국어] Type 3는 reftag 검사 안 함. */
			break;
		default:
			break;
		}
	}

	return true;
	/* [한국어] 검사 통과 또는 검사 비활성. */
}

/*
 * [한국어]
 * _dif_verify - PI 영역의 세 필드를 모두 검증. 불일치 시 -1 + err_blk.
 *
 * @_dif:           PI 영역 메모리 주소(블록 끝).
 * @guard:          호출자가 미리 데이터에서 재계산한 Guard CRC.
 * @offset_blocks:  이 블록의 인덱스(reftag 산출용).
 * @ctx:            검증 파라미터.
 * @err_blk:        [out] 실패 시 정보 기록.
 * @return:         0 성공, -1 실패.
 *
 * 검사 순서: ignore 패턴 → Guard → AppTag(마스크 적용) → RefTag.
 * 어느 한 필드라도 실패하면 즉시 반환하므로 err_blk는 첫 실패 정보만 담음.
 *
 * 호출 체인: dif_verify / _dif_verify_split / _dif_strip_copy / _dif_verify_copy → [_dif_verify].
 */
static int
_dif_verify(void *_dif, uint64_t guard, uint32_t offset_blocks,
	    const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	struct spdk_dif *dif = _dif;
	/* [한국어] void* → PI union 캐스팅. */
	uint64_t _guard;
	/* [한국어] PI에서 읽은 Guard. */
	uint16_t _app_tag;
	/* [한국어] PI에서 읽은 AppTag. */
	uint64_t ref_tag;
	/* [한국어] LBA 기반으로 계산된 기대 reftag. */

	if (_dif_ignore(dif, ctx)) {
		return 0;
		/* [한국어] PI에 ignore 패턴(0xFFFF 등)이 기록되어 있으면 검사 면제. */
	}

	/* For type 1 and 2, the reference tag is incremented for each
	 * subsequent logical block. For type 3, the reference tag
	 * remains the same as the initial reference tag.
	 */
	/* [한국어] Type 1/2: 블록마다 증가, Type 3: 고정. _dif_generate와 동일 규칙. */
	if (ctx->dif_type != SPDK_DIF_TYPE3) {
		ref_tag = ctx->init_ref_tag + ctx->ref_tag_offset + offset_blocks;
	} else {
		ref_tag = ctx->init_ref_tag + ctx->ref_tag_offset;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		/* Compare the DIF Guard field to the CRC computed over the logical
		 * block data.
		 */
		/* [한국어] Guard 검사 활성: PI에 기록된 CRC와 새로 계산한 CRC 비교. */
		_guard = _dif_get_guard(dif, ctx->dif_pi_format);
		if (_guard != guard) {
			/* [한국어] 데이터 변조 또는 매체 오류 가능성 - 즉시 실패 반환. */
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
		/* [한국어] AppTag 검사: 마스크 적용 후 비교(특정 비트만 검사 가능). */
		_app_tag = _dif_get_apptag(dif, ctx->dif_pi_format);
		if ((_app_tag & ctx->apptag_mask) != (ctx->app_tag & ctx->apptag_mask)) {
			/* [한국어] 마스크된 비트가 expected와 다르면 실패. */
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
		/* [한국어] reftag 검사 실패 - err_blk는 _dif_reftag_check 내부에서 채워짐. */
	}

	return 0;
	/* [한국어] 모든 검사 통과. */
}

/*
 * [한국어]
 * dif_verify - "fast path" 검증: 정렬된 SGL에서 블록 단위 한 번에 처리.
 *
 * 호출 체인: spdk_dif_verify(SGL 정렬) → [dif_verify] → _dif_verify.
 * 첫 실패 시 즉시 반환(이후 블록은 처리하지 않음).
 */
static int
dif_verify(struct _dif_sgl *sgl, uint32_t num_blocks,
	   const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks;
	int rc;
	/* [한국어] 검증 결과 코드. 0 또는 -1. */
	uint8_t *buf;
	uint64_t guard = 0;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		/* [한국어] 블록별 순회. */
		_dif_sgl_get_buf(sgl, &buf, NULL);
		/* [한국어] 현재 블록의 시작 주소. */

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			/* [한국어] Guard 검사 활성: 데이터 영역 CRC 재계산. */
			guard = _dif_generate_guard(ctx->guard_seed, buf, ctx->guard_interval, ctx->dif_pi_format);
		}

		rc = _dif_verify(buf + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
		/* [한국어] PI 영역(blockstart + guard_interval)에서 검증 수행. */
		if (rc != 0) {
			return rc;
			/* [한국어] 첫 실패 시 즉시 반환 - err_blk는 이미 채워짐. */
		}

		_dif_sgl_advance(sgl, ctx->block_size);
		/* [한국어] 다음 블록으로 커서 이동. */
	}

	return 0;
}

/*
 * [한국어]
 * dif_load_split - 분할 SGL의 PI 영역(8/16B)을 읽어 임시 buffer에 모음.
 *
 * dif_store_split의 역방향. 분할 SGL의 PI 영역이 여러 iovec에 걸쳐 있을 수 있으므로
 * 청크별로 모아 결합. PI 이후 메타는 advance만 하고 읽지 않음.
 */
static void
dif_load_split(struct _dif_sgl *sgl, struct spdk_dif *dif,
	       const struct spdk_dif_ctx *ctx)
{
	uint32_t offset = 0, rest_md_len, buf_len;
	uint8_t *buf;

	rest_md_len = ctx->block_size - ctx->guard_interval;
	/* [한국어] 블록 끝까지(PI + 그 뒤 메타). */

	while (offset < rest_md_len) {
		/* [한국어] 블록 끝까지 청크 반복. */
		_dif_sgl_get_buf(sgl, &buf, &buf_len);

		if (offset < _dif_size(ctx->dif_pi_format)) {
			/* [한국어] PI 영역 안: 임시 dif에 데이터 모으기. */
			buf_len = spdk_min(buf_len, _dif_size(ctx->dif_pi_format) - offset);
			memcpy((uint8_t *)dif + offset, buf, buf_len);
		} else {
			/* [한국어] PI 이후 메타 영역: 읽지 않고 건너뛰기. */
			buf_len = spdk_min(buf_len, rest_md_len - offset);
		}

		_dif_sgl_advance(sgl, buf_len);
		offset += buf_len;
	}
}

/*
 * [한국어]
 * _dif_verify_split - 분할 SGL에서 블록 일부/전체를 검증. 누적 CRC를 호출자에게 반영.
 *
 * @sgl:             데이터 SGL.
 * @offset_in_block: 이번 회차 블록 내 시작 위치.
 * @data_len:        이번 회차 처리 길이.
 * @_guard:          [in/out] 누적 CRC. 블록 완성 시 reset.
 * @offset_blocks:   블록 인덱스.
 * @ctx:             컨텍스트.
 * @err_blk:         [out] 실패 정보.
 * @return:          0 성공, -1 실패.
 *
 * 블록 미완성: CRC만 누적해 *_guard에 반영하고 0 반환.
 * 블록 완성: PI 영역을 dif_load_split으로 읽고 _dif_verify로 검증.
 */
static int
_dif_verify_split(struct _dif_sgl *sgl, uint32_t offset_in_block, uint32_t data_len,
		  uint64_t *_guard, uint32_t offset_blocks,
		  const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	uint64_t guard = *_guard;
	/* [한국어] 입력 누적 CRC를 로컬로 가져와 작업. */
	struct spdk_dif dif = {};
	/* [한국어] PI 임시 buffer(스택). */
	int rc;

	assert(_guard != NULL);
	/* [한국어] 누적 CRC 포인터는 NULL 불가. */
	assert(offset_in_block < ctx->guard_interval);
	/* [한국어] 시작점은 PI 영역 안일 수 없음. */
	assert(offset_in_block + data_len < ctx->guard_interval ||
	       offset_in_block + data_len == ctx->block_size);
	/* [한국어] 끝점은 PI 직전(누적) 또는 블록 끝(완성). */

	guard = dif_generate_guard_split(guard, sgl, offset_in_block, data_len, ctx);
	/* [한국어] 데이터 영역 청크에 대해 CRC 누적. */

	if (offset_in_block + data_len < ctx->guard_interval) {
		*_guard = guard;
		/* [한국어] 블록 미완성 - 누적값을 호출자에게 돌려주고 다음 호출로. */
		return 0;
	}

	dif_load_split(sgl, &dif, ctx);
	/* [한국어] 블록 완성 - PI 영역 읽기. */

	rc = _dif_verify(&dif, guard, offset_blocks, ctx, err_blk);
	/* [한국어] 임시 PI 버퍼와 누적 CRC로 검증. */
	if (rc != 0) {
		return rc;
		/* [한국어] 검증 실패 - 즉시 반환. */
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
		/* [한국어] 다음 블록을 위해 시드 리셋. */
	}

	*_guard = guard;
	/* [한국어] 호출자가 다음 블록에 사용할 시드를 반영. */
	return 0;
}

/*
 * [한국어]
 * dif_verify_split - 분할 SGL 모드 num_blocks 검증.
 * 호출 체인: spdk_dif_verify(SGL 비정렬) → [dif_verify_split].
 */
static int
dif_verify_split(struct _dif_sgl *sgl, uint32_t num_blocks,
		 const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks;
	uint64_t guard = 0;
	int rc;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
		/* [한국어] Guard 검사 활성: 매 블록 시작 시 ctx 시드로 리셋. */
	}

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		/* [한국어] 매 블록을 0..block_size로 split 처리. */
		rc = _dif_verify_split(sgl, 0, ctx->block_size, &guard, offset_blocks,
				       ctx, err_blk);
		if (rc != 0) {
			return rc;
			/* [한국어] 첫 실패 시 즉시 반환. */
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
	/* [한국어] 입력 SGL 커서. */

	_dif_sgl_init(&sgl, iovs, iovcnt);
	/* [한국어] iovec 배열의 시작에서 커서 초기화. */

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		/* [한국어] 입력 버퍼 크기 부족 - 오버플로 위험. */
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
		/* [한국어] DIF 비활성 - 검증 없이 통과. */
	}

	if (_dif_sgl_is_bytes_multiple(&sgl, ctx->block_size)) {
		/* [한국어] 정렬된 SGL: fast path. */
		return dif_verify(&sgl, num_blocks, ctx, err_blk);
	} else {
		/* [한국어] 비정렬: split 경로. */
		return dif_verify_split(&sgl, num_blocks, ctx, err_blk);
	}
}

/*
 * [한국어]
 * dif_update_crc32c - 정렬된 SGL에서 데이터 영역(메타 제외)의 CRC32C 누적.
 *
 * @sgl:        데이터 SGL.
 * @num_blocks: 블록 수.
 * @crc32c:     누적 시드.
 * @ctx:        block_size/md_size 참조.
 * @return:     누적 결과.
 *
 * NVMe-oF RDMA에서 transport-level CRC32C 검증용. PI/메타 영역은 제외.
 */
static uint32_t
dif_update_crc32c(struct _dif_sgl *sgl, uint32_t num_blocks,
		  uint32_t crc32c,  const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;
	uint8_t *buf;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		/* [한국어] 매 블록 순회. */
		_dif_sgl_get_buf(sgl, &buf, NULL);
		/* [한국어] 현재 블록 시작 주소. */

		crc32c = spdk_crc32c_update(buf, ctx->block_size - ctx->md_size, crc32c);
		/* [한국어] 데이터 영역(블록-메타)만 누적. lib/util/crc32c.c. */

		_dif_sgl_advance(sgl, ctx->block_size);
		/* [한국어] 다음 블록(메타 포함)으로 점프. */
	}

	return crc32c;
}

/*
 * [한국어]
 * _dif_update_crc32c_split - 분할 SGL에서 [offset_in_block, +data_len) 범위의 데이터 CRC32C 누적.
 *
 * 메타 영역은 CRC에 포함하지 않음(스트림 모드에서 한 청크가 메타에 걸쳐 있어도 데이터 부분만 누적).
 */
static uint32_t
_dif_update_crc32c_split(struct _dif_sgl *sgl, uint32_t offset_in_block, uint32_t data_len,
			 uint32_t crc32c, const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size, buf_len;
	uint8_t *buf;

	data_block_size = ctx->block_size - ctx->md_size;
	/* [한국어] 데이터 영역 크기. */

	assert(offset_in_block + data_len <= ctx->block_size);
	/* [한국어] 한 블록을 넘어가지 않아야 함. */

	while (data_len != 0) {
		/* [한국어] 요청 길이까지 청크 반복. */
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		/* [한국어] 현재 SGL 위치/잔여. */
		buf_len = spdk_min(buf_len, data_len);
		/* [한국어] 이번 청크 = iovec 잔여와 남은 요청 중 작은 쪽. */

		if (offset_in_block < data_block_size) {
			/* [한국어] 데이터 영역 안: 메타 시작 직전까지 클램프 후 CRC 누적. */
			buf_len = spdk_min(buf_len, data_block_size - offset_in_block);
			crc32c = spdk_crc32c_update(buf, buf_len, crc32c);
		}
		/* [한국어] else: 메타 영역이면 CRC 갱신 없이 advance만. */

		_dif_sgl_advance(sgl, buf_len);
		offset_in_block += buf_len;
		data_len -= buf_len;
	}

	return crc32c;
}

/*
 * [한국어]
 * dif_update_crc32c_split - 분할 SGL에서 num_blocks 블록의 CRC32C 누적.
 */
static uint32_t
dif_update_crc32c_split(struct _dif_sgl *sgl, uint32_t num_blocks,
			uint32_t crc32c, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		/* [한국어] 매 블록을 0..block_size로 split 처리(데이터만 CRC). */
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
		/* [한국어] CRC 포인터 NULL은 호출자 실수 - 거부. */
	}

	_dif_sgl_init(&sgl, iovs, iovcnt);
	/* [한국어] SGL 커서 초기화. */

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_sgl_is_bytes_multiple(&sgl, ctx->block_size)) {
		/* [한국어] 정렬된 SGL: 블록 단위 fast path. */
		*_crc32c = dif_update_crc32c(&sgl, num_blocks, *_crc32c, ctx);
	} else {
		/* [한국어] 비정렬: split 경로. */
		*_crc32c = dif_update_crc32c_split(&sgl, num_blocks, *_crc32c, ctx);
	}

	return 0;
}

/*
 * [한국어]
 * _dif_insert_copy - 정렬 SGL fast path: src(데이터만) → dst(데이터+메타+PI) 한 블록 처리.
 *
 * src 블록은 data_block_size, dst 블록은 block_size(=data_block_size+md_size).
 * GUARD_CHECK 활성: 1-pass copy+CRC. 비활성: 단순 memcpy.
 *
 * 메모리 레이아웃 변환: [DATA] → [DATA | META(앞부분) | PI(끝)]
 */
static void
_dif_insert_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		 uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size;
	/* [한국어] 데이터 영역 크기. */
	uint8_t *src, *dst;
	uint64_t guard = 0;

	data_block_size = ctx->block_size - ctx->md_size;
	/* [한국어] 메타 제외 데이터 크기. */

	_dif_sgl_get_buf(src_sgl, &src, NULL);
	/* [한국어] src 현재 블록 시작. */
	_dif_sgl_get_buf(dst_sgl, &dst, NULL);
	/* [한국어] dst 현재 블록 시작. */

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		/* [한국어] 1단계: src 데이터를 dst의 데이터 영역에 복사하면서 CRC 누적. */
		guard = _dif_generate_guard_copy(ctx->guard_seed, dst, src, data_block_size,
						 ctx->dif_pi_format);
		/* [한국어] 2단계: dst의 PI 직전 메타 영역(있다면)도 CRC에 포함.
		 * dst 메타는 아직 비어있지만 0으로 초기화되어 있다고 가정 - 호출자가 NVMe write 전 0 채움. */
		guard = _dif_generate_guard(guard, dst + data_block_size,
					    ctx->guard_interval - data_block_size, ctx->dif_pi_format);
	} else {
		memcpy(dst, src, data_block_size);
		/* [한국어] CRC 검사 비활성: 데이터만 단순 복사. */
	}

	_dif_generate(dst + ctx->guard_interval, guard, offset_blocks, ctx);
	/* [한국어] dst의 PI 영역에 Guard/AppTag/RefTag 기록. */

	_dif_sgl_advance(src_sgl, data_block_size);
	/* [한국어] src는 데이터 크기만큼 전진(메타 없음). */
	_dif_sgl_advance(dst_sgl, ctx->block_size);
	/* [한국어] dst는 블록 크기 전체 전진(데이터+메타+PI). */
}

/*
 * [한국어]
 * dif_insert_copy - 정렬 SGL fast path: num_blocks 처리.
 */
static void
dif_insert_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		/* [한국어] 매 블록을 _dif_insert_copy로 처리. */
		_dif_insert_copy(src_sgl, dst_sgl, offset_blocks, ctx);
	}
}

/*
 * [한국어]
 * _dif_insert_copy_split - 분할 SGL: src→dst 복사+CRC+PI 삽입(한 블록).
 *
 * dst의 메타+PI 영역도 분할되어 있을 수 있으므로 dif_store_split로 처리.
 */
static void
_dif_insert_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		       uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size;
	uint64_t guard = 0;
	struct spdk_dif dif = {};
	/* [한국어] 임시 PI buffer. */

	data_block_size = ctx->block_size - ctx->md_size;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		/* [한국어] 1단계: 분할 SGL 청크별로 src→dst 복사+CRC. */
		guard = _dif_generate_guard_copy_split(ctx->guard_seed, dst_sgl, src_sgl,
						       data_block_size, ctx->dif_pi_format);
		/* [한국어] 2단계: dst의 메타(앞부분)에 대해 CRC 추가 누적. */
		guard = dif_generate_guard_split(guard, dst_sgl, data_block_size,
						 ctx->guard_interval - data_block_size, ctx);
	} else {
		_data_copy_split(dst_sgl, src_sgl, data_block_size);
		/* [한국어] CRC 비활성: 데이터만 분할 복사. */
		_dif_sgl_advance(dst_sgl, ctx->guard_interval - data_block_size);
		/* [한국어] dst의 메타(앞부분) 영역 건너뛰기(PI 직전까지). */
	}

	_dif_generate(&dif, guard, offset_blocks, ctx);
	/* [한국어] 임시 PI buffer 채우기. */

	dif_store_split(dst_sgl, &dif, ctx);
	/* [한국어] dst의 PI 영역(분할 가능)에 PI 기록 + 이후 메타 건너뛰기. */
}

/*
 * [한국어]
 * dif_insert_copy_split - 분할 SGL: num_blocks 삽입+복사.
 */
static void
dif_insert_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		      uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_insert_copy_split(src_sgl, dst_sgl, offset_blocks, ctx);
	}
}

/*
 * [한국어]
 * _dif_disable_insert_copy - DISABLE 모드: src(데이터) → dst(데이터+메타) 복사. PI 미생성.
 *
 * dst의 메타 영역은 0으로 채워지지 않음(주의 - 호출자가 사전 zero 필요할 수 있음).
 * 분할 SGL을 직접 처리.
 */
static void
_dif_disable_insert_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			 const struct spdk_dif_ctx *ctx)
{
	uint32_t offset = 0, src_len, dst_len, buf_len, data_block_size;
	uint8_t *src, *dst;

	data_block_size = ctx->block_size - ctx->md_size;

	while (offset < data_block_size) {
		/* [한국어] 데이터 영역 전체를 청크별로 복사. */
		_dif_sgl_get_buf(src_sgl, &src, &src_len);
		_dif_sgl_get_buf(dst_sgl, &dst, &dst_len);
		buf_len = spdk_min(src_len, dst_len);
		/* [한국어] 두 SGL의 잔여 중 작은 쪽이 청크 한계. */
		buf_len = spdk_min(buf_len, data_block_size - offset);
		/* [한국어] 남은 데이터 길이로도 클램프. */

		memcpy(dst, src, buf_len);
		/* [한국어] 데이터 복사. */

		_dif_sgl_advance(src_sgl, buf_len);
		_dif_sgl_advance(dst_sgl, buf_len);
		offset += buf_len;
	}

	_dif_sgl_advance(dst_sgl, ctx->md_size);
	/* [한국어] dst의 메타 영역만큼 전진(쓰지 않고 건너뜀). */
}

/*
 * [한국어]
 * dif_disable_insert_copy - DISABLE 모드 num_blocks 처리.
 */
static void
dif_disable_insert_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_disable_insert_copy(src_sgl, dst_sgl, ctx);
	}
}

/*
 * [한국어]
 * _spdk_dif_insert_copy - "메타 없는 데이터" + PI 삽입 경로의 라우터.
 *
 * 입력: src=데이터만, dst=데이터+메타+PI.
 * SGL 정렬 여부와 DISABLE 여부에 따라 fast/split/disable 경로로 분기.
 */
static int
_spdk_dif_insert_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		      uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size;

	data_block_size = ctx->block_size - ctx->md_size;

	if (!_dif_sgl_is_valid(src_sgl, data_block_size * num_blocks) ||
	    !_dif_sgl_is_valid(dst_sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec arrays are not valid.\n");
		/* [한국어] src는 데이터만큼, dst는 블록 전체만큼 충분해야 함. */
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		dif_disable_insert_copy(src_sgl, dst_sgl, num_blocks, ctx);
		/* [한국어] DISABLE: 데이터만 복사하고 메타 건너뛰기. */
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(src_sgl, data_block_size) &&
	    _dif_sgl_is_bytes_multiple(dst_sgl, ctx->block_size)) {
		/* [한국어] 양쪽 모두 정렬: fast path. */
		dif_insert_copy(src_sgl, dst_sgl, num_blocks, ctx);
	} else {
		/* [한국어] 한쪽이라도 비정렬: split 경로. */
		dif_insert_copy_split(src_sgl, dst_sgl, num_blocks, ctx);
	}

	return 0;
}

/*
 * [한국어]
 * _dif_overwrite_copy - 정렬 fast path: src(데이터+메타) → dst(데이터+메타) 복사 후 PI 덮어쓰기.
 *
 * NVMe PRACT가 켜진 경우, 호스트는 이미 메타가 채워진 블록을 가지고 있고 PI만 새로 쓰면 됨.
 * src와 dst 둘 다 block_size 단위.
 */
static void
_dif_overwrite_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		    uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint8_t *src, *dst;
	uint64_t guard = 0;

	_dif_sgl_get_buf(src_sgl, &src, NULL);
	_dif_sgl_get_buf(dst_sgl, &dst, NULL);

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		/* [한국어] PI 직전까지(데이터+메타 앞부분)를 dst에 복사하면서 CRC. */
		guard = _dif_generate_guard_copy(ctx->guard_seed, dst, src, ctx->guard_interval,
						 ctx->dif_pi_format);
	} else {
		memcpy(dst, src, ctx->guard_interval);
		/* [한국어] CRC 비활성: 단순 복사. */
	}

	_dif_generate(dst + ctx->guard_interval, guard, offset_blocks, ctx);
	/* [한국어] dst PI 영역에 새 PI 기록(src의 기존 PI는 무시되고 덮어쓰기). */

	_dif_sgl_advance(src_sgl, ctx->block_size);
	_dif_sgl_advance(dst_sgl, ctx->block_size);
}

/*
 * [한국어]
 * dif_overwrite_copy - 정렬 fast path overwrite num_blocks 처리.
 */
static void
dif_overwrite_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		   uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_overwrite_copy(src_sgl, dst_sgl, offset_blocks, ctx);
	}
}

/*
 * [한국어]
 * _dif_overwrite_copy_split - 분할 SGL: src(데이터+메타) → dst(데이터+메타) 복사 + PI 덮어쓰기.
 */
static void
_dif_overwrite_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			  uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint64_t guard = 0;
	struct spdk_dif dif = {};

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		/* [한국어] PI 직전까지 분할 복사 + CRC. */
		guard = _dif_generate_guard_copy_split(ctx->guard_seed, dst_sgl, src_sgl,
						       ctx->guard_interval, ctx->dif_pi_format);
	} else {
		_data_copy_split(dst_sgl, src_sgl, ctx->guard_interval);
		/* [한국어] CRC 비활성: 단순 분할 복사. */
	}

	_dif_sgl_advance(src_sgl, ctx->block_size - ctx->guard_interval);
	/* [한국어] src의 PI+이후 메타 건너뛰기(dst와 다른 점: src의 PI는 사용 안 함). */

	_dif_generate(&dif, guard, offset_blocks, ctx);
	/* [한국어] 새 PI 계산. */
	dif_store_split(dst_sgl, &dif, ctx);
	/* [한국어] dst의 PI 영역에 기록(분할 가능). */
}

/*
 * [한국어]
 * dif_overwrite_copy_split - 분할 SGL overwrite num_blocks.
 */
static void
dif_overwrite_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			 uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_overwrite_copy_split(src_sgl, dst_sgl, offset_blocks, ctx);
	}
}

/*
 * [한국어]
 * dif_disable_copy - DISABLE 모드 src→dst 단순 복사. PI 무시.
 * src와 dst 둘 다 block_size 크기 SGL을 가정.
 */
static void
dif_disable_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		 uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	_data_copy_split(dst_sgl, src_sgl, ctx->block_size * num_blocks);
	/* [한국어] 한 번에 모든 블록 분량 복사(블록 단위 분기 불필요). */
}

/*
 * [한국어]
 * _spdk_dif_overwrite_copy - "데이터+메타" + PI 덮어쓰기 경로의 라우터.
 *
 * NVMe PRACT 비활성이거나 메타 크기 > PI 크기일 때 사용. src/dst 모두 block_size.
 */
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
		/* [한국어] DISABLE: 단순 복사. */
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(src_sgl, ctx->block_size) &&
	    _dif_sgl_is_bytes_multiple(dst_sgl, ctx->block_size)) {
		/* [한국어] 양쪽 정렬: fast path. */
		dif_overwrite_copy(src_sgl, dst_sgl, num_blocks, ctx);
	} else {
		/* [한국어] 비정렬: split 경로. */
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
	/* [한국어] src=사용자 데이터, dst=bounce 버퍼. */

	_dif_sgl_init(&src_sgl, iovs, iovcnt);
	_dif_sgl_init(&dst_sgl, bounce_iovs, bounce_iovcnt);

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_NVME_PRACT) ||
	    ctx->md_size == _dif_size(ctx->dif_pi_format)) {
		/* [한국어] PRACT 비활성 또는 메타 크기 == PI 크기:
		 * 호스트는 메타 없는 데이터만 가지고 있으며, bounce에 PI를 새로 삽입해야 함. */
		return _spdk_dif_insert_copy(&src_sgl, &dst_sgl, num_blocks, ctx);
	} else {
		/* [한국어] PRACT 활성 + md_size > PI 크기:
		 * 호스트는 데이터+메타를 가지고 있고, bounce에는 PI만 새로 덮어씀. */
		return _spdk_dif_overwrite_copy(&src_sgl, &dst_sgl, num_blocks, ctx);
	}
}

/*
 * [한국어]
 * _dif_strip_copy - 정렬 fast path: src(데이터+메타+PI) → dst(데이터만) 복사 + PI 검증.
 *
 * 읽기 경로의 핵심. HW에서 받은 bounce(블록 전체)를 사용자 영역(데이터만)으로 옮기면서
 * PI를 검증한다. 검증 실패 시 -1 반환, dst에는 일부 데이터가 이미 복사된 상태.
 */
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
		/* [한국어] 1단계: src 데이터를 dst에 복사하면서 CRC 누적. */
		guard = _dif_generate_guard_copy(ctx->guard_seed, dst, src, data_block_size,
						 ctx->dif_pi_format);
		/* [한국어] 2단계: src의 PI 직전 메타 영역까지 CRC에 추가(dst엔 복사 안 함). */
		guard = _dif_generate_guard(guard, src + data_block_size,
					    ctx->guard_interval - data_block_size, ctx->dif_pi_format);
	} else {
		memcpy(dst, src, data_block_size);
		/* [한국어] CRC 비활성: 데이터만 복사. */
	}

	rc = _dif_verify(src + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
	/* [한국어] src의 PI 영역에서 검증. 실패 시 err_blk 채워짐. */
	if (rc != 0) {
		return rc;
		/* [한국어] 검증 실패 - 호출자에게 -1 보고. */
	}

	_dif_sgl_advance(src_sgl, ctx->block_size);
	/* [한국어] src는 블록 전체 전진. */
	_dif_sgl_advance(dst_sgl, data_block_size);
	/* [한국어] dst는 데이터 영역만 전진(메타 없음). */

	return 0;
}

/*
 * [한국어]
 * dif_strip_copy - 정렬 fast path strip num_blocks. 첫 실패 시 즉시 반환.
 */
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
			/* [한국어] 검증 실패 시 즉시 중단(이후 블록 처리 안 함). */
		}
	}

	return 0;
}

/*
 * [한국어]
 * _dif_strip_copy_split - 분할 SGL: src(데이터+메타+PI) → dst(데이터) 복사 + PI 검증.
 */
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
		/* [한국어] 1단계: src 데이터를 dst에 분할 복사 + CRC. */
		guard = _dif_generate_guard_copy_split(ctx->guard_seed, dst_sgl, src_sgl,
						       data_block_size, ctx->dif_pi_format);
		/* [한국어] 2단계: src의 메타(PI 직전)도 CRC에 추가(dst 안 씀). */
		guard = dif_generate_guard_split(guard, src_sgl, data_block_size,
						 ctx->guard_interval - data_block_size, ctx);
	} else {
		_data_copy_split(dst_sgl, src_sgl, data_block_size);
		/* [한국어] CRC 비활성: 데이터만 분할 복사. */
		_dif_sgl_advance(src_sgl, ctx->guard_interval - data_block_size);
		/* [한국어] src의 메타(앞부분) 건너뛰기. */
	}

	dif_load_split(src_sgl, &dif, ctx);
	/* [한국어] src의 PI 영역(분할)을 임시 buffer에 모음. */

	return _dif_verify(&dif, guard, offset_blocks, ctx, err_blk);
	/* [한국어] 검증 결과 그대로 반환. */
}

/*
 * [한국어]
 * dif_strip_copy_split - 분할 SGL strip num_blocks.
 */
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

/*
 * [한국어]
 * _dif_disable_strip_copy - DISABLE 모드 read: src(데이터+메타) → dst(데이터) 복사. PI 검증 없음.
 */
static void
_dif_disable_strip_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			const struct spdk_dif_ctx *ctx)
{
	uint32_t offset = 0, src_len, dst_len, buf_len, data_block_size;
	uint8_t *src, *dst;

	data_block_size = ctx->block_size - ctx->md_size;

	while (offset < data_block_size) {
		/* [한국어] 데이터 영역 전체 분할 복사. */
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
	/* [한국어] src의 메타 영역 건너뛰기(읽지 않고 advance만). */
}

/*
 * [한국어]
 * dif_disable_strip_copy - DISABLE 모드 read num_blocks 처리.
 */
static void
dif_disable_strip_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		       uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_disable_strip_copy(src_sgl, dst_sgl, ctx);
	}
}

/*
 * [한국어]
 * _spdk_dif_strip_copy - "데이터+메타+PI" → "데이터만" 검증+제거 경로의 라우터.
 *
 * 입력: src=block_size, dst=data_block_size.
 */
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
		/* [한국어] dst는 데이터만, src는 블록 전체 충분해야 함. */
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		dif_disable_strip_copy(src_sgl, dst_sgl, num_blocks, ctx);
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(dst_sgl, data_block_size) &&
	    _dif_sgl_is_bytes_multiple(src_sgl, ctx->block_size)) {
		/* [한국어] 양쪽 정렬: fast path. */
		return dif_strip_copy(src_sgl, dst_sgl, num_blocks, ctx, err_blk);
	} else {
		/* [한국어] 비정렬: split. */
		return dif_strip_copy_split(src_sgl, dst_sgl, num_blocks, ctx, err_blk);
	}
}

/*
 * [한국어]
 * _dif_verify_copy - 정렬 fast path: src(데이터+메타+PI) → dst(데이터+메타) 복사 + PI 검증.
 *
 * NVMe PRACT가 켜진 경우의 read에서 호스트가 메타까지 받기를 원할 때.
 * dst는 block_size이며 PI 영역은 미정의(검증만 함).
 */
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
		/* [한국어] PI 직전까지 복사 + CRC. */
		guard = _dif_generate_guard_copy(ctx->guard_seed, dst, src, ctx->guard_interval,
						 ctx->dif_pi_format);
	} else {
		memcpy(dst, src, ctx->guard_interval);
		/* [한국어] CRC 비활성: 단순 복사. */
	}

	rc = _dif_verify(src + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
	/* [한국어] src의 PI 검증. */
	if (rc != 0) {
		return rc;
	}

	_dif_sgl_advance(src_sgl, ctx->block_size);
	/* [한국어] src 블록 전체 전진. */
	_dif_sgl_advance(dst_sgl, ctx->block_size);
	/* [한국어] dst도 블록 전체 전진(PI 영역은 비어 있음 - 호출자가 미정의). */

	return 0;
}

/*
 * [한국어]
 * dif_verify_copy - 정렬 fast path verify_copy num_blocks.
 */
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

/*
 * [한국어]
 * _dif_verify_copy_split - 분할 SGL verify_copy(검증 + 데이터/메타 복사).
 */
static int
_dif_verify_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		       uint32_t offset_blocks, const struct spdk_dif_ctx *ctx,
		       struct spdk_dif_error *err_blk)
{
	uint64_t guard = 0;
	struct spdk_dif dif = {};

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		/* [한국어] PI 직전까지 분할 복사 + CRC. */
		guard = _dif_generate_guard_copy_split(ctx->guard_seed, dst_sgl, src_sgl,
						       ctx->guard_interval, ctx->dif_pi_format);
	} else {
		_data_copy_split(dst_sgl, src_sgl, ctx->guard_interval);
	}

	dif_load_split(src_sgl, &dif, ctx);
	/* [한국어] src의 PI 영역을 임시 buffer로 모음. */
	_dif_sgl_advance(dst_sgl, ctx->block_size - ctx->guard_interval);
	/* [한국어] dst의 PI+이후 메타 영역 건너뛰기(쓰지 않음). */

	return _dif_verify(&dif, guard, offset_blocks, ctx, err_blk);
}

/*
 * [한국어]
 * dif_verify_copy_split - 분할 SGL verify_copy num_blocks.
 */
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

/*
 * [한국어]
 * _spdk_dif_verify_copy - "데이터+메타+PI" → "데이터+메타" 검증+복사 라우터.
 * src/dst 모두 block_size이며 dst의 PI 영역은 비어 있음.
 */
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
		/* [한국어] DISABLE: 검증 없이 단순 복사. */
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(dst_sgl, ctx->block_size) &&
	    _dif_sgl_is_bytes_multiple(src_sgl, ctx->block_size)) {
		/* [한국어] 양쪽 정렬: fast path. */
		return dif_verify_copy(src_sgl, dst_sgl, num_blocks, ctx, err_blk);
	} else {
		/* [한국어] 비정렬: split. */
		return dif_verify_copy_split(src_sgl, dst_sgl, num_blocks, ctx, err_blk);
	}
}

/*
 * [한국어]
 * spdk_dif_verify_copy - bounce SGL(데이터+PI)을 사용자 SGL(데이터만)로 복사+검증.
 *
 * 읽기 경로의 짝: HW로부터 받은 bounce 버퍼의 PI를 검증한 뒤 데이터 영역만 사용자에게 전달.
 * 검증 실패 시 -EIO 및 err_blk 채움.
 *
 * 호출 체인: bdev_nvme read 완료 → [spdk_dif_verify_copy] → 사용자 콜백.
 */
int
spdk_dif_verify_copy(struct iovec *iovs, int iovcnt, struct iovec *bounce_iovs,
		     int bounce_iovcnt, uint32_t num_blocks,
		     const struct spdk_dif_ctx *ctx,
		     struct spdk_dif_error *err_blk)
{
	struct _dif_sgl src_sgl, dst_sgl;
	/* [한국어] src=HW가 채운 bounce(PI 포함), dst=사용자 영역. */

	_dif_sgl_init(&src_sgl, bounce_iovs, bounce_iovcnt);
	_dif_sgl_init(&dst_sgl, iovs, iovcnt);

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_NVME_PRACT) ||
	    ctx->md_size == _dif_size(ctx->dif_pi_format)) {
		/* [한국어] PRACT 비활성 또는 메타==PI: 사용자에게 데이터만 보내고 PI 검증 후 제거. */
		return _spdk_dif_strip_copy(&src_sgl, &dst_sgl, num_blocks, ctx, err_blk);
	} else {
		/* [한국어] PRACT 활성 + md_size>PI: 사용자에게 데이터+메타까지 보내고 PI만 검증. */
		return _spdk_dif_verify_copy(&src_sgl, &dst_sgl, num_blocks, ctx, err_blk);
	}
}

/*
 * [한국어]
 * _bit_flip - 한 바이트의 특정 비트를 토글.
 * 사용처: PI/데이터에 의도적 단일 비트 오류 주입(테스트).
 */
static void
_bit_flip(uint8_t *buf, uint32_t flip_bit)
{
	uint8_t byte;

	byte = *buf;
	/* [한국어] 원본 바이트 읽기. */
	byte ^= 1 << flip_bit;
	/* [한국어] flip_bit 위치(0..7) 비트만 XOR로 토글. */
	*buf = byte;
	/* [한국어] 변경된 바이트 다시 쓰기. */
}

/*
 * [한국어]
 * _dif_inject_error - 지정한 블록의 지정한 바이트의 지정한 비트를 1개 토글.
 *
 * @sgl:                  대상 SGL(advance됨).
 * @block_size:           블록 크기(여기는 ctx->block_size 또는 md_size).
 * @num_blocks:           전체 블록 수(검증용).
 * @inject_offset_blocks: 주입할 블록 인덱스.
 * @inject_offset_bytes:  주입할 블록 내 바이트 오프셋.
 * @inject_offset_bits:   주입할 비트(0..7).
 * @return:               0 성공, -1 SGL 부족.
 */
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
	/* [한국어] 대상 블록까지 점프. */

	offset_in_block = 0;
	/* [한국어] 블록 안 진행 위치. */

	while (offset_in_block < block_size) {
		/* [한국어] 블록 안에서 청크 단위 진행. */
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, block_size - offset_in_block);
		/* [한국어] 이번 청크 길이. */

		if (inject_offset_bytes >= offset_in_block &&
		    inject_offset_bytes < offset_in_block + buf_len) {
			/* [한국어] 목표 바이트가 이번 청크 안에 있음. */
			buf += inject_offset_bytes - offset_in_block;
			/* [한국어] 청크 내 정확한 위치로 포인터 조정. */
			_bit_flip(buf, inject_offset_bits);
			/* [한국어] 비트 토글로 손상 주입. */
			return 0;
		}

		_dif_sgl_advance(sgl, buf_len);
		offset_in_block += buf_len;
	}

	return -1;
	/* [한국어] 블록 안에서 못 찾음(인자 오류). */
}

/*
 * [한국어]
 * dif_inject_error - 무작위 위치(블록/바이트/비트)에 단일 비트 오류 주입.
 *
 * @sgl:                대상 SGL.
 * @block_size:         블록 크기.
 * @num_blocks:         블록 수.
 * @start_inject_bytes: 주입 가능한 바이트 시작 오프셋(예: PI 영역의 reftag 시작).
 * @inject_range_bytes: 주입 가능한 바이트 범위 길이(예: reftag 크기).
 * @inject_offset:      [out] 어느 블록에 주입했는지.
 *
 * srand(time(0))로 시드 초기화 - 테스트 재현 시 같은 시간엔 같은 결과.
 * 사용처: SPDK 회귀 테스트(test/dif).
 */
static int
dif_inject_error(struct _dif_sgl *sgl, uint32_t block_size, uint32_t num_blocks,
		 uint32_t start_inject_bytes, uint32_t inject_range_bytes,
		 uint32_t *inject_offset)
{
	uint32_t inject_offset_blocks, inject_offset_bytes, inject_offset_bits;
	uint32_t offset_blocks;
	int rc;

	srand(time(0));
	/* [한국어] 매 호출 시드 초기화 - 다른 시간엔 다른 위치 주입. */

	inject_offset_blocks = rand() % num_blocks;
	/* [한국어] 무작위 블록 선택. */
	inject_offset_bytes = start_inject_bytes + (rand() % inject_range_bytes);
	/* [한국어] 지정 범위 내 무작위 바이트 선택. */
	inject_offset_bits = rand() % 8;
	/* [한국어] 무작위 비트 위치(0..7). */

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		/* [한국어] 블록 순회하다 목표 블록에서만 주입. */
		if (offset_blocks == inject_offset_blocks) {
			rc = _dif_inject_error(sgl, block_size, num_blocks,
					       inject_offset_blocks,
					       inject_offset_bytes,
					       inject_offset_bits);
			if (rc == 0) {
				*inject_offset = inject_offset_blocks;
				/* [한국어] 호출자에게 어느 블록에 주입했는지 보고. */
			}
			return rc;
		}
	}

	return -1;
	/* [한국어] 도달 불가(num_blocks % num_blocks 보장 - 안전망). */
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
		/* [한국어] reftag 영역에 오류 주입.
		 * 위치: 블록 시작 + guard_interval(PI 시작) + reftag_offset. */
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
		/* [한국어] apptag 영역에 오류 주입.
		 * 위치: PI 시작 + apptag_offset(=guard 크기). */
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
		/* [한국어] Guard CRC 영역에 오류 주입. 위치: PI 시작(guard_interval). */
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
		/* [한국어] 데이터 영역에 오류 주입. 검증 시 Guard 불일치로 검출 예상. */
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

/*
 * [한국어]
 * dix_generate - DIX 모드 정렬 fast path: 데이터/메타가 분리된 SGL에서 PI 생성.
 *
 * @data_sgl:   데이터 SGL(블록 크기 단위).
 * @md_sgl:     메타데이터 SGL(md_size 단위, 단일 iovec 가정).
 * @num_blocks: 블록 수.
 *
 * NVMe MPTR(Metadata Pointer)이 별도로 지정된 경우. 데이터 영역 전체와 메타 영역의
 * PI 직전까지를 모두 CRC에 포함한 뒤, 메타의 PI 영역에 PI 기록.
 */
static void
dix_generate(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
	     uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks = 0;
	uint8_t *data_buf, *md_buf;
	uint64_t guard;

	while (offset_blocks < num_blocks) {
		/* [한국어] 매 블록 처리. */
		_dif_sgl_get_buf(data_sgl, &data_buf, NULL);
		/* [한국어] 현재 블록의 데이터 시작. */
		_dif_sgl_get_buf(md_sgl, &md_buf, NULL);
		/* [한국어] 현재 블록의 메타 시작. */

		guard = 0;
		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			/* [한국어] 1단계: 데이터 영역 전체에 대한 CRC 계산. */
			guard = _dif_generate_guard(ctx->guard_seed, data_buf, ctx->block_size,
						    ctx->dif_pi_format);
			/* [한국어] 2단계: 메타의 PI 직전까지 CRC 누적(있다면). */
			guard = _dif_generate_guard(guard, md_buf, ctx->guard_interval,
						    ctx->dif_pi_format);
		}

		_dif_generate(md_buf + ctx->guard_interval, guard, offset_blocks, ctx);
		/* [한국어] 메타 버퍼의 PI 영역에 PI 기록. */

		_dif_sgl_advance(data_sgl, ctx->block_size);
		/* [한국어] 데이터 SGL 한 블록 전진. */
		_dif_sgl_advance(md_sgl, ctx->md_size);
		/* [한국어] 메타 SGL 한 메타 단위 전진. */
		offset_blocks++;
	}
}

/*
 * [한국어]
 * _dix_generate_split - DIX 분할 SGL: 한 블록 데이터를 여러 청크로 받아 PI 생성.
 *
 * 데이터 SGL이 비정렬일 수 있으므로 블록 안에서 청크별로 CRC 누적.
 * 메타 SGL은 단일 iovec 가정(호출 측에서 보장).
 */
static void
_dix_generate_split(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
		    uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_in_block, data_buf_len;
	uint8_t *data_buf, *md_buf;
	uint64_t guard = 0;

	_dif_sgl_get_buf(md_sgl, &md_buf, NULL);
	/* [한국어] 메타 시작 포인터. */

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
		/* [한국어] CRC 활성: 시드에서 시작. */
	}
	offset_in_block = 0;

	while (offset_in_block < ctx->block_size) {
		/* [한국어] 블록 끝까지 청크 반복. */
		_dif_sgl_get_buf(data_sgl, &data_buf, &data_buf_len);
		data_buf_len = spdk_min(data_buf_len, ctx->block_size - offset_in_block);
		/* [한국어] 블록 경계 안으로 클램프. */

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(guard, data_buf, data_buf_len,
						    ctx->dif_pi_format);
			/* [한국어] 데이터 청크에 대해 CRC 누적. */
		}

		_dif_sgl_advance(data_sgl, data_buf_len);
		offset_in_block += data_buf_len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		/* [한국어] 메타의 PI 직전까지 CRC 누적(메타는 단일 iovec). */
		guard = _dif_generate_guard(guard, md_buf, ctx->guard_interval,
					    ctx->dif_pi_format);
	}

	_dif_sgl_advance(md_sgl, ctx->md_size);
	/* [한국어] 메타 SGL 한 단위 전진. */

	_dif_generate(md_buf + ctx->guard_interval, guard, offset_blocks, ctx);
	/* [한국어] 메타 버퍼의 PI 영역에 PI 기록. */
}

/*
 * [한국어]
 * dix_generate_split - DIX 분할 SGL num_blocks 처리.
 */
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
	/* [한국어] data와 메타용 분리 SGL. md_iov는 단일 iovec(iovcnt=1). */

	_dif_sgl_init(&data_sgl, iovs, iovcnt);
	_dif_sgl_init(&md_sgl, md_iov, 1);

	if (!_dif_sgl_is_valid(&data_sgl, ctx->block_size * num_blocks) ||
	    !_dif_sgl_is_valid(&md_sgl, ctx->md_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		/* [한국어] 데이터/메타 양쪽 모두 충분한 크기를 가져야 함. */
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(&data_sgl, ctx->block_size)) {
		/* [한국어] 데이터 SGL 정렬: fast path. */
		dix_generate(&data_sgl, &md_sgl, num_blocks, ctx);
	} else {
		/* [한국어] 데이터 SGL 비정렬: split path. */
		dix_generate_split(&data_sgl, &md_sgl, num_blocks, ctx);
	}

	return 0;
}

/*
 * [한국어]
 * dix_verify - DIX 정렬 fast path 검증.
 * 데이터 영역 + 메타 PI 직전까지에 대해 CRC 재계산 → 메타의 PI와 비교.
 */
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
			/* [한국어] 1단계: 데이터 영역 전체 CRC. */
			guard = _dif_generate_guard(ctx->guard_seed, data_buf, ctx->block_size,
						    ctx->dif_pi_format);
			/* [한국어] 2단계: 메타 PI 앞부분 CRC. */
			guard = _dif_generate_guard(guard, md_buf, ctx->guard_interval,
						    ctx->dif_pi_format);
		}

		rc = _dif_verify(md_buf + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
		/* [한국어] 메타의 PI 영역과 비교 검증. */
		if (rc != 0) {
			return rc;
		}

		_dif_sgl_advance(data_sgl, ctx->block_size);
		_dif_sgl_advance(md_sgl, ctx->md_size);
		offset_blocks++;
	}

	return 0;
}

/*
 * [한국어]
 * _dix_verify_split - DIX 분할 SGL 한 블록 검증.
 */
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
		/* [한국어] 데이터 영역 청크별 CRC 누적. */
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
		/* [한국어] 메타의 PI 앞부분도 CRC에 누적. */
		guard = _dif_generate_guard(guard, md_buf, ctx->guard_interval,
					    ctx->dif_pi_format);
	}

	_dif_sgl_advance(md_sgl, ctx->md_size);
	/* [한국어] 메타 SGL 한 단위 전진. */

	return _dif_verify(md_buf + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
	/* [한국어] PI 영역과 누적 CRC 비교. */
}

/*
 * [한국어]
 * dix_verify_split - DIX 분할 SGL num_blocks 검증.
 */
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
		/* [한국어] DIX 모드는 메타 버퍼가 필수. */
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
		/* [한국어] 데이터 정렬: fast path. */
		return dix_verify(&data_sgl, &md_sgl, num_blocks, ctx, err_blk);
	} else {
		/* [한국어] 데이터 비정렬: split. */
		return dix_verify_split(&data_sgl, &md_sgl, num_blocks, ctx, err_blk);
	}
}

/*
 * [한국어]
 * spdk_dix_inject_error - DIX 모드 테스트 오류 주입.
 *
 * PI 영역(reftag/apptag/guard)에 주입할 때는 메타 SGL에 주입,
 * 데이터 영역에 주입할 때는 데이터 SGL에 주입.
 * 사용처: SPDK 회귀 테스트(test/dif).
 */
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
		/* [한국어] 메타 SGL의 reftag 위치에 오류 주입. */
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
		/* [한국어] 메타 SGL의 apptag 위치에 오류 주입. */
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
		/* [한국어] 메타 SGL의 Guard 위치에 오류 주입. */
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
		/* [한국어] 데이터 SGL 전체 범위에 오류 주입. 검증 시 Guard로 검출. */
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

/*
 * [한국어]
 * _to_next_boundary - offset부터 boundary 정렬 다음 위치까지의 거리 반환.
 *
 * 예: offset=520, boundary=512 → 504(다음 512 배수까지). 한 블록 안에 남은 바이트 계산용.
 * 사용처: 스트림 모드에서 한 청크가 블록 경계를 넘지 않도록 분할.
 */
static uint32_t
_to_next_boundary(uint32_t offset, uint32_t boundary)
{
	return boundary - (offset % boundary);
	/* [한국어] (boundary - 잉여) = 다음 정렬 위치까지의 거리. */
}

/*
 * [한국어]
 * _to_size_with_md - "데이터 좌표 size"를 "메타 포함 버퍼 좌표"로 변환.
 *
 * @size:            데이터 좌표(메타 제외)에서의 길이/오프셋.
 * @data_block_size: 한 블록의 데이터 크기.
 * @block_size:      한 블록의 총 크기(데이터+메타).
 * @return:          메타 포함 버퍼 좌표.
 *
 * 예: data_block_size=512, block_size=520, size=1024 → 2*520 = 1040.
 * 잉여 부분은 그대로 더해 부분 블록을 처리.
 */
static uint32_t
_to_size_with_md(uint32_t size, uint32_t data_block_size, uint32_t block_size)
{
	return (size / data_block_size) * block_size + (size % data_block_size);
	/* [한국어] 완전 블록 수 * block_size + 부분 블록의 데이터 잉여. */
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
	/* [한국어] data_block_size=블록의 데이터 크기, data_unalign=ctx 위치의 블록 내 정렬 어긋남,
	 *         buf_len=메타 포함 버퍼 길이, buf_offset=메타 포함 버퍼 시작점, len=청크 길이. */
	struct _dif_sgl dif_sgl;
	/* [한국어] 출력 iovec 배열용 SGL(append 모드). */
	struct _dif_sgl buf_sgl;
	/* [한국어] 입력 메타 인터리브 buffer SGL(읽기 모드). */

	if (iovs == NULL || iovcnt == 0 || buf_iovs == NULL || buf_iovcnt == 0) {
		return -EINVAL;
		/* [한국어] 인자 무효 - 거부. */
	}

	data_block_size = ctx->block_size - ctx->md_size;
	/* [한국어] 데이터 영역 크기. */

	data_unalign = ctx->data_offset % data_block_size;
	/* [한국어] ctx의 누적 위치가 블록 경계와 어긋난 정도. */

	buf_len = _to_size_with_md(data_unalign + data_offset + data_len, data_block_size,
				   ctx->block_size);
	buf_len -= data_unalign;
	/* [한국어] 처리해야 할 데이터 영역(unalign 보정 포함)에 해당하는 메타 포함 버퍼 길이. */

	_dif_sgl_init(&dif_sgl, iovs, iovcnt);
	_dif_sgl_init(&buf_sgl, buf_iovs, buf_iovcnt);

	if (!_dif_sgl_is_valid(&buf_sgl, buf_len)) {
		SPDK_ERRLOG("Buffer overflow will occur.\n");
		/* [한국어] 입력 메타 인터리브 버퍼가 부족하면 거부. */
		return -ERANGE;
	}

	buf_offset = _to_size_with_md(data_unalign + data_offset, data_block_size, ctx->block_size);
	buf_offset -= data_unalign;
	/* [한국어] 메타 포함 버퍼 안에서 시작 오프셋(data_offset에 해당). */

	_dif_sgl_advance(&buf_sgl, buf_offset);
	/* [한국어] 입력 SGL을 시작 위치까지 전진. */

	while (data_len != 0) {
		/* [한국어] 데이터를 블록 경계 단위로 나누어 출력 iovec 배열을 만든다. */
		len = spdk_min(data_len, _to_next_boundary(ctx->data_offset + data_offset, data_block_size));
		/* [한국어] 이번 청크 길이 = 남은 길이와 다음 블록 경계까지 중 작은 쪽. */
		if (!_dif_sgl_append_split(&dif_sgl, &buf_sgl, len)) {
			/* [한국어] 출력 iovec 슬롯이 부족하면 부분 매핑된 상태로 종료. */
			break;
		}
		_dif_sgl_advance(&buf_sgl, ctx->md_size);
		/* [한국어] 한 데이터 블록을 매핑한 후, 입력 buffer의 메타 영역은 건너뛴다. */
		data_offset += len;
		data_len -= len;
	}

	if (_mapped_len != NULL) {
		*_mapped_len = dif_sgl.total_size;
		/* [한국어] 호출자에게 실제 매핑된 데이터 길이 보고. */
	}

	return iovcnt - dif_sgl.iovcnt;
	/* [한국어] 사용한 출력 iovec 수 반환(= 원래 - 남은). */
}

/*
 * [한국어]
 * _dif_sgl_setup_stream - 스트림 모드용 SGL 커서 설정.
 *
 * @sgl:         [in/out] 입력 SGL 커서. data_offset 위치까지 advance됨.
 * @_buf_offset: [out] 메타 포함 좌표의 현재 회차 시작 오프셋.
 * @_buf_len:    [out] 메타 포함 좌표의 현재 회차 길이.
 * @data_offset: 데이터 좌표 회차 시작.
 * @data_len:    데이터 좌표 회차 길이.
 * @ctx:         컨텍스트.
 *
 * 핵심: 스트림 청크가 블록 중간에 시작/끝날 수 있으므로, 각 회차에서
 * "정확히 처리해야 할 메타 포함 좌표 범위"를 계산해야 한다. data_unalign으로
 * ctx의 누적 위치 보정.
 */
static int
_dif_sgl_setup_stream(struct _dif_sgl *sgl, uint32_t *_buf_offset, uint32_t *_buf_len,
		      uint32_t data_offset, uint32_t data_len,
		      const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size, data_unalign, buf_len, buf_offset;

	data_block_size = ctx->block_size - ctx->md_size;

	data_unalign = ctx->data_offset % data_block_size;
	/* [한국어] ctx의 누적 시작 위치 정렬 보정. */

	/* If the last data block is complete, DIF of the data block is
	 * inserted or verified in this turn.
	 */
	/* [한국어] 마지막 데이터 블록이 이번 회차에서 완성되면, 그 블록의 PI 처리도 이번에 한다. */
	buf_len = _to_size_with_md(data_unalign + data_offset + data_len, data_block_size,
				   ctx->block_size);
	buf_len -= data_unalign;
	/* [한국어] 회차 끝까지 메타 포함 버퍼 길이 계산. */

	if (!_dif_sgl_is_valid(sgl, buf_len)) {
		return -ERANGE;
		/* [한국어] SGL 부족 - 거부. */
	}

	buf_offset = _to_size_with_md(data_unalign + data_offset, data_block_size, ctx->block_size);
	buf_offset -= data_unalign;
	/* [한국어] 회차 시작점의 메타 포함 좌표 계산. */

	_dif_sgl_advance(sgl, buf_offset);
	/* [한국어] SGL을 회차 시작점까지 전진. */
	buf_len -= buf_offset;
	/* [한국어] 시작점 이후의 처리 길이. */

	buf_offset += data_unalign;
	/* [한국어] data_unalign을 다시 더해 절대 좌표(블록 내 위치 계산용)로 만든다. */

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
	/* [한국어] 메타 포함 좌표의 회차 길이/시작. */
	uint32_t len, offset_in_block, offset_blocks;
	/* [한국어] 청크 길이/블록 내 위치/블록 인덱스. */
	uint64_t guard = 0;
	/* [한국어] 누적 CRC. ctx->last_guard로 회차 간 보존. */
	struct _dif_sgl sgl;
	int rc;

	if (iovs == NULL || iovcnt == 0) {
		return -EINVAL;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->last_guard;
		/* [한국어] 직전 회차의 누적 CRC를 가져옴. */
	}

	_dif_sgl_init(&sgl, iovs, iovcnt);

	rc = _dif_sgl_setup_stream(&sgl, &buf_offset, &buf_len, data_offset, data_len, ctx);
	if (rc != 0) {
		return rc;
		/* [한국어] SGL 부족 - 회차 처리 불가. */
	}

	while (buf_len != 0) {
		/* [한국어] 회차 분량을 블록 경계 단위 청크로 나눠 처리. */
		len = spdk_min(buf_len, _to_next_boundary(buf_offset, ctx->block_size));
		/* [한국어] 청크 = min(남은 회차 분량, 다음 블록 경계까지 거리). */
		offset_in_block = buf_offset % ctx->block_size;
		/* [한국어] 청크의 블록 내 시작 위치. */
		offset_blocks = buf_offset / ctx->block_size;
		/* [한국어] 청크가 속한 블록 인덱스. */

		guard = _dif_generate_split(&sgl, offset_in_block, len, guard, offset_blocks, ctx);
		/* [한국어] 청크 처리 - 블록 완성 시 PI 기록, 미완성 시 CRC만 누적. */

		buf_len -= len;
		buf_offset += len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		ctx->last_guard = guard;
		/* [한국어] 다음 회차에 이어서 사용할 누적 CRC를 ctx에 저장. */
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
		/* [한국어] 직전 회차의 누적 CRC. */
	}

	_dif_sgl_init(&sgl, iovs, iovcnt);

	rc = _dif_sgl_setup_stream(&sgl, &buf_offset, &buf_len, data_offset, data_len, ctx);
	if (rc != 0) {
		return rc;
	}

	while (buf_len != 0) {
		/* [한국어] 회차 분량을 블록 경계 청크로 분할. */
		len = spdk_min(buf_len, _to_next_boundary(buf_offset, ctx->block_size));
		offset_in_block = buf_offset % ctx->block_size;
		offset_blocks = buf_offset / ctx->block_size;

		rc = _dif_verify_split(&sgl, offset_in_block, len, &guard, offset_blocks,
				       ctx, err_blk);
		/* [한국어] 청크 검증. 블록 완성 시 PI 비교, 미완성 시 CRC만 누적. */
		if (rc != 0) {
			goto error;
			/* [한국어] 검증 실패 - last_guard 저장 없이 즉시 종료. */
		}

		buf_len -= len;
		buf_offset += len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		ctx->last_guard = guard;
		/* [한국어] 다음 회차용 CRC 보존. */
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
	/* [한국어] 누적 CRC32C(Guard용 CRC와 별개 - transport CRC). */
	struct _dif_sgl sgl;
	int rc;

	if (iovs == NULL || iovcnt == 0) {
		return -EINVAL;
	}

	crc32c = *_crc32c;
	/* [한국어] 호출자의 누적 시드를 가져옴. */
	_dif_sgl_init(&sgl, iovs, iovcnt);

	rc = _dif_sgl_setup_stream(&sgl, &buf_offset, &buf_len, data_offset, data_len, ctx);
	if (rc != 0) {
		return rc;
	}

	while (buf_len != 0) {
		/* [한국어] 블록 경계 청크로 분할 처리. */
		len = spdk_min(buf_len, _to_next_boundary(buf_offset, ctx->block_size));
		offset_in_block = buf_offset % ctx->block_size;

		crc32c = _dif_update_crc32c_split(&sgl, offset_in_block, len, crc32c, ctx);
		/* [한국어] 데이터 영역만 CRC32C 누적(메타는 제외). */

		buf_len -= len;
		buf_offset += len;
	}

	*_crc32c = crc32c;
	/* [한국어] 다음 회차용으로 누적값 호출자에 반영. */

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
		/* [한국어] DIX 모드: 데이터 좌표 = 메타 포함 좌표(메타가 별도 SGL이므로 동일). */
		buf_offset = data_offset;
		buf_len = data_len;
	} else {
		/* [한국어] 인터리브: 메타가 데이터 사이에 끼어 있으므로 좌표 변환 필요. */
		data_block_size = ctx->block_size - ctx->md_size;

		data_unalign = data_offset % data_block_size;
		/* [한국어] data_offset의 블록 내 정렬 어긋남. */

		buf_offset = _to_size_with_md(data_offset, data_block_size, ctx->block_size);
		/* [한국어] data_offset에 해당하는 메타 포함 버퍼 시작 오프셋. */
		buf_len = _to_size_with_md(data_unalign + data_len, data_block_size, ctx->block_size) -
			  data_unalign;
		/* [한국어] data_len에 해당하는 메타 포함 버퍼 길이(unalign 보정). */
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
		/* [한국어] DIX: 메타가 별도이므로 그대로 반환. */
	} else {
		/* [한국어] 인터리브: 데이터 길이를 메타 포함 좌표로 변환. */
		data_block_size = ctx->block_size - ctx->md_size;

		return _to_size_with_md(data_len, data_block_size, ctx->block_size);
	}
}

/*
 * [한국어]
 * _dif_remap_ref_tag - 한 블록의 PI 영역에서 reftag만 새 LBA에 맞춰 갱신.
 *
 * @sgl:           [in/out] 인터리브된 데이터+메타 SGL. PI 영역까지 advance한 뒤 다시 읽고 쓴다.
 * @offset_blocks: 블록 인덱스.
 * @ctx:           init_ref_tag(원본 시작) + remapped_init_ref_tag(새 시작) 모두 가짐.
 * @err_blk:       [out] check_ref_tag=true일 때 검증 실패 정보.
 * @check_ref_tag: true이면 새 값을 쓰기 전 기존 reftag가 expected와 일치하는지 검증.
 *
 * 동작: 데이터 영역 건너뛰기 → PI 영역 읽기(분할) → ignore면 패스, 아니면 기존 reftag 검증
 * → reftag 새 값으로 재기록(분할) → 블록 끝까지 advance.
 *
 * Guard CRC는 reftag 변경에 영향받지 않으므로(ref tag는 PI 영역만 차지, CRC 대상은 데이터 + 가능한 메타 앞부분) 갱신 안 함.
 *
 * 호출 체인: spdk_dif_remap_ref_tag → [_dif_remap_ref_tag] (블록마다 호출).
 */
static int
_dif_remap_ref_tag(struct _dif_sgl *sgl, uint32_t offset_blocks,
		   const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk,
		   bool check_ref_tag)
{
	uint32_t offset, buf_len;
	uint64_t expected = 0, remapped;
	/* [한국어] expected=원래 LBA에서 기대되는 reftag, remapped=새 LBA의 reftag. */
	uint8_t *buf;
	struct _dif_sgl tmp_sgl;
	/* [한국어] PI 영역 시작 위치를 보존하기 위한 SGL 스냅샷(쓰기 시 다시 사용). */
	struct spdk_dif dif;
	/* [한국어] PI 임시 buffer(스택). */

	/* Fast forward to DIF field. */
	_dif_sgl_advance(sgl, ctx->guard_interval);
	/* [한국어] 데이터+(있다면)앞쪽 메타까지 건너뛰고 PI 시작 위치로. */
	_dif_sgl_copy(&tmp_sgl, sgl);
	/* [한국어] PI 시작 위치 스냅샷 - 쓰기 단계에서 동일 위치를 다시 가리키기 위함. */

	/* Copy the split DIF field to the temporary DIF buffer */
	/* [한국어] 1단계: PI 영역(분할 가능)을 임시 buffer로 모음. */
	offset = 0;
	while (offset < _dif_size(ctx->dif_pi_format)) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, _dif_size(ctx->dif_pi_format) - offset);

		memcpy((uint8_t *)&dif + offset, buf, buf_len);
		/* [한국어] PI 청크를 임시 buffer의 해당 위치에 복사. */

		_dif_sgl_advance(sgl, buf_len);
		offset += buf_len;
	}

	if (_dif_ignore(&dif, ctx)) {
		goto end;
		/* [한국어] PI에 ignore 패턴(0xFFFF 등) - 갱신 없이 통과. */
	}

	/* For type 1 and 2, the Reference Tag is incremented for each
	 * subsequent logical block. For type 3, the Reference Tag
	 * remains the same as the initialReference Tag.
	 */
	/* [한국어] Type 1/2: 블록 인덱스 가산. Type 3: 고정. */
	if (ctx->dif_type != SPDK_DIF_TYPE3) {
		expected = ctx->init_ref_tag + ctx->ref_tag_offset + offset_blocks;
		remapped = ctx->remapped_init_ref_tag + ctx->ref_tag_offset + offset_blocks;
	} else {
		remapped = ctx->remapped_init_ref_tag;
		/* [한국어] Type 3는 expected가 사용되지 않음(check_ref_tag도 의미 없음). */
	}

	/* Verify the stored Reference Tag. */
	if (check_ref_tag && !_dif_reftag_check(&dif, ctx, expected, offset_blocks, err_blk)) {
		return -1;
		/* [한국어] 기존 reftag 검증 요구 + 실패 시 즉시 -1. */
	}

	/* Update the stored Reference Tag to the remapped one. */
	_dif_set_reftag(&dif, remapped, ctx->dif_pi_format);
	/* [한국어] 임시 buffer의 reftag만 새 값으로 변경. Guard/AppTag는 그대로. */

	/* [한국어] 2단계: 임시 buffer를 PI 영역(분할)에 다시 기록. */
	offset = 0;
	while (offset < _dif_size(ctx->dif_pi_format)) {
		_dif_sgl_get_buf(&tmp_sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, _dif_size(ctx->dif_pi_format) - offset);

		memcpy(buf, (uint8_t *)&dif + offset, buf_len);
		/* [한국어] 갱신된 PI를 SGL의 PI 영역에 청크 단위로 기록. */

		_dif_sgl_advance(&tmp_sgl, buf_len);
		offset += buf_len;
	}

end:
	_dif_sgl_advance(sgl, ctx->block_size - ctx->guard_interval - _dif_size(ctx->dif_pi_format));
	/* [한국어] 블록 끝까지 advance(PI 이후 메타 영역이 있으면 건너뛰기). */

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
		/* [한국어] DIF 비활성 - 처리 없음. */
	}

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK)) {
		return 0;
		/* [한국어] reftag 검사가 비활성이면 reftag 자체가 의미 없음 - 갱신 안 함. */
	}

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		/* [한국어] 매 블록의 PI 영역에서 reftag 갱신. */
		rc = _dif_remap_ref_tag(&sgl, offset_blocks, ctx, err_blk, check_ref_tag);
		if (rc != 0) {
			return rc;
			/* [한국어] check_ref_tag 모드에서 검증 실패 시 즉시 반환. */
		}
	}

	return 0;
}

/*
 * [한국어]
 * _dix_remap_ref_tag - DIX 모드에서 한 블록의 메타 PI 영역의 reftag만 갱신.
 *
 * 인터리브 모드와 달리 메타 SGL이 단일 buffer라 가정 - 청크 분할 없이 직접 포인터 접근.
 * 데이터 SGL은 건드리지 않으므로 호출자가 안 넘김.
 *
 * 호출 체인: spdk_dix_remap_ref_tag → [_dix_remap_ref_tag] (블록마다).
 */
static int
_dix_remap_ref_tag(struct _dif_sgl *md_sgl, uint32_t offset_blocks,
		   const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk,
		   bool check_ref_tag)
{
	uint64_t expected = 0, remapped;
	uint8_t *md_buf;
	struct spdk_dif *dif;
	/* [한국어] 메타 buffer 안의 PI 영역 직접 포인터(분할 처리 불필요). */

	_dif_sgl_get_buf(md_sgl, &md_buf, NULL);
	/* [한국어] 현재 메타 단위의 시작 주소. */

	dif = (struct spdk_dif *)(md_buf + ctx->guard_interval);
	/* [한국어] 메타 안의 PI 시작 위치 - in-place 접근. */

	if (_dif_ignore(dif, ctx)) {
		goto end;
		/* [한국어] ignore 패턴 - 갱신 없이 통과. */
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
		/* [한국어] 기존 reftag 검증 실패 시 -1. */
	}

	/* Update the stored Reference Tag to the remapped one. */
	_dif_set_reftag(dif, remapped, ctx->dif_pi_format);
	/* [한국어] 메타 buffer 안의 PI에서 reftag만 in-place 갱신. */

end:
	_dif_sgl_advance(md_sgl, ctx->md_size);
	/* [한국어] 다음 블록의 메타로 advance. */

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
	/* [한국어] DIX 메타는 단일 iovec(iovcnt=1). */

	if (!_dif_sgl_is_valid(&md_sgl, ctx->md_size * num_blocks)) {
		SPDK_ERRLOG("Size of metadata iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
		/* [한국어] DIF 비활성 - 처리 없음. */
	}

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK)) {
		return 0;
		/* [한국어] reftag 검사 비활성 - reftag 의미 없음. */
	}

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		/* [한국어] 매 블록의 메타 PI 영역에서 reftag 갱신. */
		rc = _dix_remap_ref_tag(&md_sgl, offset_blocks, ctx, err_blk, check_ref_tag);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}
