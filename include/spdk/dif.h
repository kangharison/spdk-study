/*   SPDX-License-Identifier: BSD-3-Clause
 *   All rights reserved.
 */

/*
 * [한국어 설명] T10 DIF/DIX 보호 정보 처리 공개 API (dif.h) — 약 492 라인
 *
 * === 파일의 역할 ===
 * T10 DIF (Data Integrity Field) 및 DIX (Data Integrity Extensions)의
 * 생성·검증·error injection·remap 등 보호 정보(Protection Information, PI)
 * 처리 API를 정의한다. T10 PI는 SCSI/SAS/SATA/NVMe 표준이 공유하는 호스트-
 * 디바이스 데이터 무결성 메커니즘으로, 각 LBA 블록 끝(또는 별도 메타데이터)
 * 에 다음 3종의 필드를 부착한다:
 *   - Guard      : CRC-16(T10-DIF) / CRC-32C(T10-PI Type 2/3) 체크섬.
 *   - Application Tag (16 bit): 호스트 정의 태그 (소유권/스트림 식별 등).
 *   - Reference Tag (32/48/64 bit): LBA를 해시한 위치 검증 태그 (Type 1=LBA 자체).
 * SPDK는 이 헤더를 통해 일반 데이터 버퍼에서 PI 필드를 생성·삽입하거나
 * 디바이스에서 받은 데이터의 PI를 검증해 호스트에 reject/passthrough 한다.
 *
 * DIF vs DIX:
 *   - DIF: 데이터와 PI가 인터리브된 "확장 LBA(Extended LBA)" — 예 512+8 = 520 바이트/블록.
 *   - DIX: 데이터와 PI가 별도의 메타데이터 버퍼로 분리 — 호스트는 데이터/메타 두 SGL 사용.
 * NVMe는 두 모드 모두 지원 (NSFEAT의 metadata pointer/extended LBA).
 *
 * PI Type:
 *   - Type 0: PI 비활성.
 *   - Type 1: Reference Tag = LBA의 lower bits (RW마다 자동 증가).
 *   - Type 2: Reference Tag = 호스트가 임의 지정 (NVMe Reservation/seq 사용).
 *   - Type 3: Reference Tag check 안 함 (Guard + Application Tag만).
 *
 * PI Format (NVMe 2.x 추가):
 *   - 16-bit  : 전통적인 8바이트 PI (Guard 16b + AppTag 16b + RefTag 32b).
 *   - 32-bit  : 16바이트 PI (Guard 32b + AppTag 16b + RefTag 80b 등).
 *   - 64-bit  : 16바이트 PI (Guard 64b + AppTag 16b + RefTag 48b).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 측: lib/bdev (PI 처리가 필요한 bdev), module/bdev/nvme (NVMe PI 활성
 *          네임스페이스), lib/nvmf (NVMe-oF target에서 호스트→디바이스 전달
 *          중 PI 변환), lib/nvme (NVMe initiator 측 PI 처리).
 * 호출 흐름:
 *   write 경로:  app 데이터 → spdk_dif_generate(...) → PI 부착된 확장 LBA →
 *                NVMe write 커맨드 → SSD가 자체 검증/저장.
 *   read 경로:   NVMe read 커맨드 → SSD가 PI 포함 반환 → spdk_dif_verify(...) →
 *                실패 시 spdk_dif_error로 호스트에 보고.
 * 구현부: lib/util/dif.c — 본 헤더의 모든 API의 실체. CRC 계산은 lib/util/crc16.c,
 *         crc32c.c가 담당.
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/stdinc.h(uint*_t/iovec/EINVAL), spdk/assert.h(SPDK_STATIC_ASSERT
 *            for ABI 검증).
 * 의존하는 모듈: lib/bdev/bdev.c (PI passthrough), module/bdev/nvme (NVMe PI 활성
 *              ns), lib/nvmf/ctrlr_bdev.c (oF target에서 PI 변환), lib/nvme
 *              (initiator 측 PI 처리), lib/iscsi (iSCSI Read/Write Protect 지원).
 * 데이터 흐름:
 *   write: 일반 SGL → spdk_dif_generate() → PI 부착 → NVMe SQE → SSD.
 *   read:  SSD → NVMe CQE → 확장 LBA SGL → spdk_dif_verify() → 일반 SGL → app.
 *   stream: 네트워크 수신과 PI 생성을 zero-copy 결합 (set_md_interleave_iovs +
 *           generate_stream/verify_stream).
 * 공유 자료구조: struct spdk_dif_ctx (PI 동작 파라미터 — 호출자가 한 번 init 후
 *              I/O마다 재사용), struct spdk_dif_error (검증 실패 시 출력 정보).
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_dif_ctx: PI 처리 파라미터 묶음 (block_size/md_size/dif_type/
 *     init_ref_tag/app_tag/guard_seed/...). spdk_dif_ctx_init()으로 초기화.
 *   - struct spdk_dif_error: 검증 실패 정보 (err_type/expected/actual/err_offset).
 *   - 매크로 SPDK_DIF_FLAGS_*: REFTAG/APPTAG/GUARD/PRACT check 활성 비트 (NVMe 스펙 호환).
 *   - 매크로 SPDK_DIF_*_ERROR: error type 비트 (verify 결과의 err_type 필드 값).
 *   - 매크로 SPDK_DIF_*_IGNORE: PI 검증 우회 매직 값 (스펙 §5.2.2.3).
 *   - enum spdk_dif_type: DISABLE/TYPE1/TYPE2/TYPE3.
 *   - enum spdk_dif_check_type: 개별 체크 종류 식별자.
 *   - enum spdk_dif_pi_format: 16/32/64비트 PI 포맷.
 *   - spdk_dif_ctx_init/_set_data_offset/_set_remapped_init_ref_tag.
 *   - DIF API: spdk_dif_generate/_verify/_update_crc32c/_generate_copy/_verify_copy/
 *             _inject_error/_remap_ref_tag — 인터리브 모드.
 *   - DIX API: spdk_dix_generate/_verify/_inject_error/_remap_ref_tag — 분리 메타.
 *   - Stream API: spdk_dif_set_md_interleave_iovs/_generate_stream/_verify_stream/
 *                _update_crc32c_stream — 부분 데이터 처리 (네트워크 수신 등).
 *   - Helper: spdk_dif_get_range_with_md/_get_length_with_md/_pi_format_get_size.
 */

#ifndef SPDK_DIF_H                   /* [한국어] 다중 포함 방지 가드 */
#define SPDK_DIF_H

#include "spdk/stdinc.h"             /* [한국어] 표준 타입 (uint*_t, struct iovec, ENOMEM 등) */
#include "spdk/assert.h"             /* [한국어] SPDK_STATIC_ASSERT — struct spdk_dif_ctx_init_ext_opts ABI 크기 검증 */

#ifdef __cplusplus
extern "C" {                         /* [한국어] C++ 호환을 위한 C 링키지 강제 */
#endif

/**
 * Use `SPDK_DIF_APPTAG_IGNORE` and `SPDK_DIF_REFTAG_IGNORE`
 * as the special values when creating DIF context, when the two
 * values are used for Application Tag and Initialization Reference Tag,
 * DIF library will fill the protection information fields with above
 * values, based on the specification, when doing verify with
 * above values in protection information field, the checking
 * will be ignored.
 */
/* [한국어] === PI 검증 우회 매직 값 (T10/NVMe 스펙) ===
 * 디바이스가 PI 필드에서 이 값을 발견하면 해당 체크를 우회하도록 표준이 규정.
 * NVMe Base Spec §5.2 / T10-PI §5.2.2.3 참조. */

#define SPDK_DIF_REFTAG_IGNORE		0xFFFFFFFF
/* [한국어] Reference Tag가 0xFFFFFFFF면 reftag check 무시 (Type 1/2 모두).
 * 사용 사례: PI 활성 ns에 첫 write 시 알 수 없는 LBA로 일관된 PI를 만들 때. */

#define SPDK_DIF_APPTAG_IGNORE		0xFFFF
/* [한국어] Application Tag가 0xFFFF면 apptag check 무시.
 * 사용 사례: 호스트가 application tag를 사용하지 않는 경우. */

/* [한국어] === DIF 동작 활성 플래그 (NVMe Format NSID DPS와 호환) ===
 * spdk_dif_ctx.dif_flags에 OR로 조합. NVMe 스펙의 PRINFO 필드와 호환성을 위해
 * 비트 위치(26~29)가 NVMe Read/Write 커맨드의 CDW12 PRINFO 비트와 동일하다. */

#define SPDK_DIF_FLAGS_REFTAG_CHECK	(1U << 26)
/* [한국어] Reference Tag 검증 활성 (NVMe PRCHK_REF, bit 26).
 * Type 1: SSD가 LBA 자동 증가 검증, Type 2: 호스트 지정값 검증, Type 3: 무시. */

#define SPDK_DIF_FLAGS_APPTAG_CHECK	(1U << 27)
/* [한국어] Application Tag 검증 활성 (NVMe PRCHK_APP, bit 27).
 * apptag_mask & received_apptag == apptag_mask & expected_apptag 형태로 비교. */

#define SPDK_DIF_FLAGS_GUARD_CHECK	(1U << 28)
/* [한국어] Guard(CRC) 검증 활성 (NVMe PRCHK_GUARD, bit 28).
 * 가장 핵심 — 데이터 비트 손상을 직접 검출. */

#define SPDK_DIF_FLAGS_NVME_PRACT	(1U << 29)
/* [한국어] NVMe PRACT(Protection Action) 활성 (NVMe PRACT, bit 29).
 * write: SSD가 자동으로 PI 생성/제거. read: SSD가 검증 후 PI 제거하여 데이터만 반환.
 * SPDK가 PI 생성을 직접 하지 않고 SSD에 위임할 때 사용. */

/* [한국어] === verify/inject 결과의 error type 비트 ===
 * spdk_dif_error.err_type에 OR로 조합되어, 어떤 체크가 실패했는지를 표시.
 * spdk_dif_inject_error의 inject_flags 인자에도 동일 비트 사용. */

#define SPDK_DIF_REFTAG_ERROR	0x1
/* [한국어] Reference Tag 불일치 (예상 LBA와 실제 reftag 다름). */

#define SPDK_DIF_APPTAG_ERROR	0x2
/* [한국어] Application Tag 불일치 (apptag_mask로 마스킹 후 비교 결과 다름). */

#define SPDK_DIF_GUARD_ERROR	0x4
/* [한국어] Guard(CRC) 불일치 — 데이터 또는 PI 자체가 손상됨. */

#define SPDK_DIF_DATA_ERROR	0x8
/* [한국어] inject_error 전용 — 데이터 영역(non-PI)에 비트 플립을 가했을 때 표시.
 * verify가 보고하는 게 아니라 호출자가 inject 의도를 표현하는 비트. */

/* [한국어] === DIF Type (T10-PI Type 0/1/2/3) ===
 * NVMe Identify Namespace의 DPS(Data Protection Settings) 필드와 매핑.
 * Type 1이 가장 흔하며 (LBA 기반 reftag), Type 3는 reftag 검증을 끔. */
enum spdk_dif_type {
	SPDK_DIF_DISABLE = 0,
	/* [한국어] Type 0 — PI 비활성. block_size에 PI 필드 없음. */

	SPDK_DIF_TYPE1 = 1,
	/* [한국어] Type 1 — Reference Tag = LBA의 하위 비트 (자동 증가).
	 * SSD가 모든 PI 필드 검증, 가장 흔한 모드. */

	SPDK_DIF_TYPE2 = 2,
	/* [한국어] Type 2 — Reference Tag = 호스트 지정값 (NVMe ILBRT/EILBRT 사용).
	 * 호스트가 매 write마다 reftag 명시 — sequential 기록 외에도 활용. */

	SPDK_DIF_TYPE3 = 3,
	/* [한국어] Type 3 — Reference Tag check 비활성. Guard + Application Tag만. */
};

/* [한국어] 개별 체크 종류 식별자 — 보고/RPC에서 어떤 종류의 체크인지 구분 용도.
 * SPDK_DIF_FLAGS_*_CHECK 비트와 별개로, 단일 enum 값으로 한 번에 한 종류를 지정. */
enum spdk_dif_check_type {
	SPDK_DIF_CHECK_TYPE_REFTAG = 1,
	/* [한국어] Reference Tag 체크 종류 식별자. */

	SPDK_DIF_CHECK_TYPE_APPTAG = 2,
	/* [한국어] Application Tag 체크 종류 식별자. */

	SPDK_DIF_CHECK_TYPE_GUARD = 3,
	/* [한국어] Guard(CRC) 체크 종류 식별자. */
};

/* [한국어] PI 포맷 — NVMe 2.x에서 추가된 32/64비트 PI 지원.
 * 16비트는 전통적인 T10-DIF 8바이트 PI, 32/64비트는 NVMe 2.x의 16바이트 PI. */
enum spdk_dif_pi_format {
	SPDK_DIF_PI_FORMAT_16 = 0,
	/* [한국어] 16비트 Guard CRC, 8바이트 PI 필드. T10-DIF/NVMe 1.x 표준. */

	SPDK_DIF_PI_FORMAT_32 = 1,
	/* [한국어] 32비트 Guard CRC-32C, 16바이트 PI 필드. NVMe 2.x. */

	SPDK_DIF_PI_FORMAT_64 = 2,
	/* [한국어] 64비트 Guard CRC-64, 16바이트 PI 필드. NVMe 2.x — 가장 강력한 검출. */
};

/* [한국어] spdk_dif_ctx_init()에 전달하는 확장 옵션 구조체.
 * size 필드를 통한 ABI 호환 패턴 — 새 필드는 항상 끝에 추가, 라이브러리는
 * size로 호출자 버전을 식별해 신규 필드를 기본값으로 채울 수 있다. */
struct spdk_dif_ctx_init_ext_opts {
	/** Size of this structure in bytes, use SPDK_SIZEOF() to calculate it */
	size_t size;
	/* [한국어] 호출자가 인지한 본 구조체의 sizeof 값 (ABI 호환용).
	 * 설정자: 사용자가 SPDK_SIZEOF() 또는 sizeof(struct ...)로 채움.
	 * 읽는 자: lib/util/dif.c가 옛 ABI 사용 여부 판별.
	 * 값 범위: 양수, sizeof(struct spdk_dif_ctx_init_ext_opts) 이하. */

	uint32_t dif_pi_format;
	/* [한국어] PI 포맷 선택 — enum spdk_dif_pi_format(16/32/64bit).
	 * 설정자: 사용자가 명시. NVMe Identify Namespace의 NPDA/NPDG로부터 결정.
	 * 읽는 자: dif_ctx_init이 spdk_dif_ctx.dif_pi_format으로 복사.
	 * 값 범위: 0~2 (SPDK_DIF_PI_FORMAT_16/_32/_64). */
};
/* [한국어] ABI 안정성 가드 — size_t(8) + uint32_t(4) + padding(4) = 16 바이트. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_dif_ctx_init_ext_opts) == 16, "Incorrect size");

/** DIF context information */
/* [한국어] DIF/DIX 처리에 필요한 모든 파라미터를 담는 컨텍스트.
 * 사용 패턴: 한 번 spdk_dif_ctx_init()으로 채운 후, 같은 ns/I/O 세션의
 * 여러 generate/verify 호출에서 재사용 (값은 mostly read-only).
 * 변경되는 필드: data_offset(stream API에서 갱신), last_guard(부분 블록 처리),
 * remapped_init_ref_tag(virtual bdev remap 시).
 * 동기화: 한 spdk_thread/I/O context에 종속 — cross-thread 공유 금지. */
struct spdk_dif_ctx {
	/** Block size */
	uint32_t		block_size;
	/* [한국어] 확장 LBA 한 블록의 총 크기 (데이터 + 메타데이터 포함).
	 * 예: 512+8=520, 4096+8=4104, 4096+64=4160.
	 * 설정자: dif_ctx_init이 호출자 인자로부터 채움.
	 * 읽는 자: 모든 generate/verify가 블록 경계 계산에 사용. */

	/** Interval for guard computation for DIF */
	uint32_t		guard_interval;
	/* [한국어] Guard CRC 계산 대상이 되는 바이트 수 (보통 데이터 영역만).
	 * 설정자: dif_ctx_init이 md_interleave/md_size/dif_loc로부터 도출.
	 * 읽는 자: CRC 계산 루프 — 0..guard_interval-1 바이트로 CRC 산출.
	 * 값 범위: block_size 이하 (interleaved일 때 데이터+이전 메타). */

	/** Metadata size */
	uint32_t		md_size;
	/* [한국어] 한 블록당 메타데이터(PI 포함) 크기.
	 * 예: T10-DIF는 8, NVMe 2.x 32/64bit PI는 16, 4K+64는 64.
	 * 설정자: dif_ctx_init.
	 * 읽는 자: PI 위치 계산, md_iov 길이 검증. */

	/** Metadata location */
	bool			md_interleave;
	/* [한국어] true: DIF 모드(데이터+메타가 한 블록 안에 인터리브, 확장 LBA).
	 *          false: DIX 모드(데이터와 메타가 별도 SGL).
	 * 설정자: dif_ctx_init.
	 * 읽는 자: spdk_dif_*는 true 가정, spdk_dix_*는 false 가정. */

	/** DIF type */
	uint8_t			dif_type; /* ref spdk_dif_ctx */
	/* [한국어] T10-PI Type (enum spdk_dif_type 값).
	 * 설정자: dif_ctx_init.
	 * 읽는 자: reftag 검증/생성 로직이 Type 1/2/3 분기.
	 * 값 범위: 0~3 (DISABLE/TYPE1/TYPE2/TYPE3). */

	/** DIF Protection Information format */
	uint8_t			dif_pi_format; /* ref spdk_dif_pi_format */
	/* [한국어] PI 필드 포맷 (16/32/64bit Guard).
	 * 설정자: dif_ctx_init이 opts->dif_pi_format에서 복사.
	 * 읽는 자: CRC 알고리즘 분기 (CRC-16 vs CRC-32C vs CRC-64). */

	uint8_t			rsvd[1];
	/* [한국어] 정렬을 위한 예약 필드 (uint8 + uint8 + uint8 + uint32 정렬). */

	/* Flags to specify the DIF action */
	uint32_t		dif_flags;
	/* [한국어] SPDK_DIF_FLAGS_REFTAG_CHECK/APPTAG_CHECK/GUARD_CHECK/NVME_PRACT 비트 OR.
	 * 설정자: dif_ctx_init.
	 * 읽는 자: verify가 각 체크의 활성 여부 판별. */

	uint8_t			rsvd2[4];
	/* [한국어] uint32 + uint64 정렬을 맞추기 위한 예약 영역. */

	/* Initial reference tag */
	uint64_t		init_ref_tag;
	/* [한국어] 첫 블록의 Reference Tag 시작 값 (Type 1: 시작 LBA).
	 * 설정자: dif_ctx_init이 호출자 인자로부터 받음.
	 * 읽는 자: generate/verify가 블록 i의 reftag = init_ref_tag + i로 사용.
	 * 값 범위: PI 포맷에 따라 32/48/80비트 의미 폭 다름. */

	/** Application tag */
	uint16_t		app_tag;
	/* [한국어] 모든 블록에 동일하게 부착될 Application Tag (16비트).
	 * 설정자: dif_ctx_init.
	 * 읽는 자: generate가 PI 필드에 기록, verify가 (received & mask)와 비교. */

	/* Application tag mask */
	uint16_t		apptag_mask;
	/* [한국어] apptag 비교 시 사용할 비트마스크.
	 * 0xFFFF = 전체 비트 비교, 0x0000 = 비교 안 함.
	 * 사용 사례: 일부 비트만 의미 있는 경우 (호스트별 일부 비트 사용). */

	/* Byte offset from the start of the whole data buffer. */
	uint32_t		data_offset;
	/* [한국어] 전체 LBA 페이로드의 시작에서 현재 처리 위치(바이트) 오프셋.
	 * 설정자: dif_ctx_set_data_offset()로 stream API가 부분 처리 시 갱신.
	 * 읽는 자: stream 함수들이 어느 블록부터 처리할지 판단. */

	/* Offset to initial reference tag */
	uint32_t		ref_tag_offset;
	/* [한국어] data_offset로부터 도출된 reftag 증가량 — i번째 블록은 init_ref_tag + ref_tag_offset + i.
	 * 설정자: 내부적으로 data_offset 변경 시 갱신.
	 * 읽는 자: stream 함수의 reftag 계산. */

	/* Remapped initial reference tag. */
	uint32_t		remapped_init_ref_tag;
	/* [한국어] virtual bdev(예: split bdev)에서 LBA 주소 변환 후 새 reftag.
	 * 설정자: spdk_dif_ctx_set_remapped_init_ref_tag().
	 * 읽는 자: spdk_dif_remap_ref_tag/spdk_dix_remap_ref_tag가 PI를 갱신. */

	/** Guard value of the last data block.
	 *
	 * Interim guard value is set if the last data block is partial, or
	 * seed value is set otherwise.
	 */
	uint64_t		last_guard;
	/* [한국어] 마지막 처리한 블록의 Guard 중간값 (CRC 누적).
	 * 부분 블록(stream API)일 때: 다음 호출에서 이어서 CRC 계산할 시드.
	 * 완전 블록 처리 후: guard_seed로 리셋되어 다음 블록의 CRC 시작점으로 사용.
	 * 설정자: generate_stream/verify_stream이 매 호출마다 갱신. */

	/* Seed value for guard computation */
	uint64_t		guard_seed;
	/* [한국어] CRC 계산 초기값 (보통 0). NVMe spec은 0을 권장.
	 * 설정자: dif_ctx_init.
	 * 읽는 자: 각 블록의 CRC 계산 시작 시. */

};

/** DIF error information */
/* [한국어] verify 실패 시 호출자에게 반환되는 에러 정보 묶음.
 * 어떤 종류의 검사가 어떤 블록에서 실패했고, expected/actual이 무엇인지 보고. */
struct spdk_dif_error {
	/** Error type */
	uint8_t		err_type;
	/* [한국어] SPDK_DIF_REFTAG/APPTAG/GUARD_ERROR 비트 OR.
	 * 설정자: verify 함수가 실패 검출 시 채움.
	 * 읽는 자: 호출자가 RPC/log로 어떤 종류의 손상인지 보고. */

	uint8_t		rsvd[7];
	/* [한국어] uint64 정렬을 위한 예약 패딩. */

	/** Expected value */
	uint64_t	expected;
	/* [한국어] 호스트가 기대했던 값 (예: 계산된 CRC, init_ref_tag + block_idx).
	 * 설정자: verify가 실패 시 자동 계산 후 저장. */

	/** Actual value */
	uint64_t	actual;
	/* [한국어] 디바이스/버퍼에서 실제로 읽은 값.
	 * expected != actual인 필드의 값. 디버깅에 유용. */

	/** Offset the error occurred at, block based */
	uint32_t	err_offset;
	/* [한국어] 에러가 발생한 블록의 인덱스 (0-base, 페이로드 내부).
	 * 설정자: verify가 실패한 블록 번호로 채움.
	 * 읽는 자: 호출자가 어느 LBA에서 에러가 났는지 LBA 변환 시 사용. */
};

/**
 * Initialize DIF context.
 *
 * \param ctx DIF context.
 * \param block_size Block size in a block.
 * \param md_size Metadata size in a block.
 * \param md_interleave If true, metadata is interleaved with block data.
 * If false, metadata is separated with block data.
 * \param dif_loc DIF location. If true, DIF is set in the first 8/16 bytes of metadata.
 * If false, DIF is in the last 8/16 bytes of metadata.
 * \param dif_type Type of DIF.
 * \param dif_flags Flag to specify the DIF action.
 * \param init_ref_tag Initial reference tag. For type 1, this is the
 * starting block address.
 * \param apptag_mask Application tag mask.
 * \param app_tag Application tag.
 * \param data_offset Byte offset from the start of the whole data buffer.
 * \param guard_seed Seed value for guard computation.
 * \param opts Extended options for DIF context.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dif_ctx_init - DIF 컨텍스트를 초기화 (모든 PI 처리 호출의 전제조건).
 *
 * @ctx: 초기화할 컨텍스트 (호출자 소유 메모리, 보통 stack 또는 I/O cmd 내부).
 * @block_size: 확장 LBA 블록 크기 (데이터 + 메타데이터 포함, 예: 520, 4104).
 * @md_size: 한 블록당 메타데이터 크기 (예: 8, 16, 64).
 * @md_interleave: true=DIF(인터리브)/false=DIX(분리 메타).
 * @dif_loc: true=PI가 메타데이터 앞 8/16바이트, false=뒤 8/16바이트 (DPS bit 4).
 * @dif_type: T10-PI Type (DISABLE/TYPE1/TYPE2/TYPE3).
 * @dif_flags: SPDK_DIF_FLAGS_*_CHECK 비트 OR (활성 검사 종류).
 * @init_ref_tag: 첫 블록의 reference tag (Type 1: 시작 LBA).
 * @apptag_mask: apptag 비교 시 마스크.
 * @app_tag: 모든 블록의 application tag.
 * @data_offset: 페이로드 내 시작 바이트 오프셋 (보통 0, stream에서만 의미).
 * @guard_seed: CRC 초기 시드 값 (NVMe 권장 0).
 * @opts: 확장 옵션 (PI 포맷 등). NULL이면 기본 16비트 PI.
 * @return: 0 성공 / -EINVAL 등 음수 errno 실패.
 *
 * 호출자: bdev 모듈, NVMe 드라이버, NVMe-oF target.
 * 호출 컨텍스트: I/O 시작 시점 — 같은 컨텍스트를 같은 ns의 여러 generate/verify에 재사용 가능.
 */
int spdk_dif_ctx_init(struct spdk_dif_ctx *ctx, uint32_t block_size, uint32_t md_size,
		      bool md_interleave, bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
		      uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag,
		      uint32_t data_offset, uint64_t guard_seed, struct spdk_dif_ctx_init_ext_opts *opts);

/**
 * Update date offset of DIF context.
 *
 * \param ctx DIF context.
 * \param data_offset Byte offset from the start of the whole data buffer.
 */
/*
 * [한국어]
 * spdk_dif_ctx_set_data_offset - 컨텍스트의 data_offset 필드를 갱신.
 *
 * @ctx: 갱신할 컨텍스트.
 * @data_offset: 새 바이트 오프셋 (전체 LBA 페이로드 시작 기준).
 *
 * stream API 사용 시 매 부분 처리마다 호출 — 어디서 이어 처리할지 알리기 위함.
 * 내부적으로 ref_tag_offset도 함께 재계산된다.
 */
void spdk_dif_ctx_set_data_offset(struct spdk_dif_ctx *ctx, uint32_t data_offset);

/**
 * Set remapped initial reference tag of DIF context.
 *
 * \param ctx DIF context.
 * \param remapped_init_ref_tag Remapped initial reference tag. For type 1, this is the
 * starting block address.
 */
/*
 * [한국어]
 * spdk_dif_ctx_set_remapped_init_ref_tag - 가상 bdev에서 LBA 변환 후의 새 reftag 설정.
 *
 * @ctx: 갱신할 컨텍스트.
 * @remapped_init_ref_tag: 변환된 가상 LBA에 대응하는 새 reference tag.
 *
 * 사용 사례: split bdev가 자식 bdev에 I/O를 위임할 때, 부모 LBA 기반 reftag를
 * 자식 LBA 기반 reftag로 변환해야 디바이스가 올바르게 검증 가능. 본 함수로
 * 새 값을 설정한 후 spdk_dif_remap_ref_tag/spdk_dix_remap_ref_tag로 PI 갱신.
 */
void spdk_dif_ctx_set_remapped_init_ref_tag(struct spdk_dif_ctx *ctx,
		uint32_t remapped_init_ref_tag);

/**
 * Generate DIF for extended LBA payload.
 *
 * \param iovs iovec array describing the extended LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param num_blocks Number of blocks of the payload.
 * \param ctx DIF context.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dif_generate - 확장 LBA 페이로드(데이터+메타 인터리브)에 PI 생성·삽입.
 *
 * @iovs: 확장 LBA 페이로드 SGL (데이터+메타 자리가 이미 확보되어 있어야 함).
 * @iovcnt: SGL 엔트리 수.
 * @num_blocks: 페이로드의 블록 수.
 * @ctx: PI 처리 파라미터.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * 동작:
 *   - 각 블록에 대해 Guard CRC 계산, AppTag/RefTag 부착.
 *   - Type 1: reftag = init_ref_tag + i. Type 2: ctx값 그대로. Type 3: 무시.
 * 호출 컨텍스트: write 경로 — bdev 모듈이 디바이스에 보내기 전 호출.
 */
int spdk_dif_generate(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
		      const struct spdk_dif_ctx *ctx);

/**
 * Verify DIF for extended LBA payload.
 *
 * \param iovs iovec array describing the extended LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param num_blocks Number of blocks of the payload.
 * \param ctx DIF context.
 * \param err_blk Error information of the block in which DIF error is found.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dif_verify - 확장 LBA 페이로드의 PI를 검증.
 *
 * @iovs: 검증할 확장 LBA 페이로드 SGL (디바이스에서 받은 데이터).
 * @iovcnt: SGL 엔트리 수.
 * @num_blocks: 블록 수.
 * @ctx: PI 처리 파라미터 (dif_flags의 *_CHECK 비트가 활성된 검사만 수행).
 * @err_blk: 출력 — 첫 번째로 실패한 블록의 정보 (NULL이면 정보 미수집).
 * @return: 0 성공 (또는 모든 블록 PASS) / 음수 errno 실패.
 *
 * 동작:
 *   - 각 블록의 PI를 ctx 기준으로 비교.
 *   - SPDK_DIF_*_IGNORE 매직 값을 만나면 해당 체크 우회.
 *   - 첫 실패에서 즉시 중단 후 err_blk에 expected/actual/err_offset 채움.
 * 호출 컨텍스트: read 경로 — bdev 모듈이 디바이스 응답 처리 시.
 */
int spdk_dif_verify(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
		    const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk);

/**
 * Calculate CRC-32C checksum for extended LBA payload.
 *
 * \param iovs iovec array describing the extended LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param num_blocks Number of blocks of the payload.
 * \param crc32c Initial and updated CRC-32C value.
 * \param ctx DIF context.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dif_update_crc32c - 확장 LBA 페이로드의 CRC-32C 누적 계산 (T10-DIF 검증과 별개의 end-to-end CRC).
 *
 * @iovs/@iovcnt/@num_blocks: 입력 페이로드.
 * @crc32c: in-out — 시드 값을 입력하고 누적된 CRC를 출력.
 * @ctx: 블록 형상 정보 (md_size 등).
 * @return: 0 성공 / 음수 errno 실패.
 *
 * 사용 사례: NVMe-oF transport(특히 TCP)에서 디지스트 검증 또는 별도 무결성
 *           체크에 활용. PI 자체를 다루지 않고 데이터 부분만 CRC 계산.
 */
int spdk_dif_update_crc32c(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
			   uint32_t *crc32c, const struct spdk_dif_ctx *ctx);

/**
 * Copy data and generate DIF for extended LBA payload.
 *
 * NOTE: If PRACT is set in the DIF context, this function simulates the NVMe PRACT feature.
 * If metadata size is larger than DIF size, not only bounce buffer but also source buffer
 * should be extended LBA payload.

 * \param iovs iovec array describing the LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param bounce_iovs A contiguous buffer forming extended LBA payload.
 * \param bounce_iovcnt Number of elements in the bounce_iovs array.
 * \param num_blocks Number of blocks of the LBA payload.
 * \param ctx DIF context.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dif_generate_copy - 데이터를 bounce 버퍼로 복사하면서 동시에 PI 생성.
 *
 * @iovs/@iovcnt: 원본 데이터 SGL (PI 자리 없음, 순수 데이터).
 * @bounce_iovs/@bounce_iovcnt: 출력 bounce 버퍼 SGL (확장 LBA 형태).
 * @num_blocks: 블록 수.
 * @ctx: PI 처리 파라미터.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * NVMe PRACT 시뮬레이션 — 호스트 데이터에 PI를 부착하면서 한 번의 메모리
 * 복사에 묶어 처리. PRACT가 set이고 메타가 PI보다 큰 경우 원본도 확장 LBA 형태여야 함.
 * 사용 사례: HW가 PRACT 미지원 시 SW 보완.
 */
int spdk_dif_generate_copy(struct iovec *iovs, int iovcnt, struct iovec *bounce_iovs,
			   int bounce_iovcnt,  uint32_t num_blocks,
			   const struct spdk_dif_ctx *ctx);

/**
 * Verify DIF and copy data for extended LBA payload.
 *
 * NOTE: If PRACT is set in the DIF context, this function simulates the NVMe PRACT feature.
 * If metadata size is larger than DIF size, not only bounce buffer but also destination buffer
 * should be extended LBA payload.
 *
 * \param iovs iovec array describing the LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param bounce_iovs A contiguous buffer forming extended LBA payload.
 * \param bounce_iovcnt Number of elements in the bounce_iovs array.
 * \param num_blocks Number of blocks of the LBA payload.
 * \param ctx DIF context.
 * \param err_blk Error information of the block in which DIF error is found.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dif_verify_copy - bounce 버퍼의 PI를 검증하면서 데이터 부분만 호스트 SGL로 복사.
 *
 * @iovs/@iovcnt: 출력 — 복사 대상 호스트 SGL (PI 제거된 순수 데이터).
 * @bounce_iovs/@bounce_iovcnt: 입력 — 디바이스가 채운 확장 LBA 페이로드.
 * @num_blocks: 블록 수.
 * @ctx: PI 처리 파라미터.
 * @err_blk: 출력 에러 정보.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * generate_copy의 read 짝 — verify와 PI 제거 복사를 한 번에 수행. PRACT 시뮬레이션.
 */
int spdk_dif_verify_copy(struct iovec *iovs, int iovcnt, struct iovec *bounce_iovs,
			 int bounce_iovcnt,  uint32_t num_blocks,
			 const struct spdk_dif_ctx *ctx,
			 struct spdk_dif_error *err_blk);

/**
 * Inject bit flip error to extended LBA payload.
 *
 * \param iovs iovec array describing the extended LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param num_blocks Number of blocks of the payload.
 * \param ctx DIF context.
 * \param inject_flags Flags to specify the action of error injection.
 * \param inject_offset Offset, in blocks, to which error is injected.
 * If multiple error is injected, only the last injection is stored.
 *
 * \return 0 on success and negated errno otherwise including no metadata.
 */
/*
 * [한국어]
 * spdk_dif_inject_error - 확장 LBA 페이로드에 비트 플립 에러를 의도적으로 주입 (테스트 용).
 *
 * @iovs/@iovcnt/@num_blocks: 입력 페이로드.
 * @ctx: PI 처리 파라미터.
 * @inject_flags: SPDK_DIF_REFTAG/APPTAG/GUARD/DATA_ERROR 비트 OR — 어떤 영역에 주입할지.
 * @inject_offset: 출력 — 마지막으로 주입한 블록 오프셋 (여러 주입 시 마지막 위치만 보존).
 * @return: 0 성공 / 음수 errno 실패 (예: 메타 없음 -ENOTSUP).
 *
 * 사용 사례: SPDK 테스트 슈트가 verify 경로의 에러 검출 로직을 검증할 때.
 *           QA 외에는 호출하지 않는 게 정상.
 */
int spdk_dif_inject_error(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
			  const struct spdk_dif_ctx *ctx, uint32_t inject_flags,
			  uint32_t *inject_offset);

/**
 * Generate DIF for separate metadata payload.
 *
 * \param iovs iovec array describing the LBA payload.
 * \params iovcnt Number of elements in iovs.
 * \param md_iov A contiguous buffer for metadata.
 * \param num_blocks Number of blocks of the separate metadata payload.
 * \param ctx DIF context.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dix_generate - DIX 모드(메타 분리) 페이로드에 PI 생성·삽입.
 *
 * @iovs/@iovcnt: 데이터 SGL (PI 포함 안 함, 순수 데이터).
 * @md_iov: 별도 메타데이터 버퍼 (모든 블록의 메타가 인접).
 * @num_blocks: 블록 수.
 * @ctx: PI 처리 파라미터 (md_interleave=false여야 함).
 * @return: 0 성공 / 음수 errno 실패.
 *
 * DIF의 분리 메타 버전 — 호스트가 데이터/메타를 별도로 보관할 때 사용.
 * NVMe DIX (Metadata Pointer 모드) 또는 SCSI DIX와 매핑.
 */
int spdk_dix_generate(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
		      uint32_t num_blocks, const struct spdk_dif_ctx *ctx);

/**
 * Verify DIF for separate metadata payload.
 *
 * \param iovs iovec array describing the LBA payload.
 * \params iovcnt Number of elements in iovs.
 * \param md_iov A contiguous buffer for metadata.
 * \param num_blocks Number of blocks of the separate metadata payload.
 * \param ctx DIF context.
 * \param err_blk Error information of the block in which DIF error is found.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dix_verify - DIX 모드 페이로드의 PI를 검증.
 *
 * @iovs/@iovcnt: 데이터 SGL.
 * @md_iov: 별도 메타데이터 버퍼 (모든 블록의 PI 포함).
 * @num_blocks: 블록 수.
 * @ctx: PI 처리 파라미터.
 * @err_blk: 출력 에러 정보.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * spdk_dif_verify의 DIX 버전. 동일한 검증 로직이지만 PI를 md_iov에서 분리 추출.
 */
int spdk_dix_verify(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
		    uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		    struct spdk_dif_error *err_blk);

/**
 * Inject bit flip error to separate metadata payload.
 *
 * \param iovs iovec array describing the extended LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param md_iov A contiguous buffer for metadata.
 * \param num_blocks Number of blocks of the payload.
 * \param ctx DIF context.
 * \param inject_flags Flag to specify the action of error injection.
 * \param inject_offset Offset, in blocks, to which error is injected.
 * If multiple error is injected, only the last injection is stored.
 *
 * \return 0 on success and negated errno otherwise including no metadata.
 */
/*
 * [한국어]
 * spdk_dix_inject_error - DIX 모드 페이로드에 비트 플립 에러 주입 (테스트 용).
 *
 * @iovs/@iovcnt/@md_iov/@num_blocks/@ctx: DIX 페이로드와 컨텍스트.
 * @inject_flags: 주입 종류 비트 (SPDK_DIF_*_ERROR).
 * @inject_offset: 출력 — 마지막 주입 위치.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * spdk_dif_inject_error의 DIX 버전. 데이터/메타 어느 쪽이든 inject_flags에 따라 주입.
 */
int spdk_dix_inject_error(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
			  uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
			  uint32_t inject_flags, uint32_t *inject_offset);

/**
 * Setup iovec array to leave a space for metadata for each block.
 *
 * This function is used to leave a space for metadata for each block when
 * the network socket reads data, or to make the network socket ignore a
 * space for metadata for each block when the network socket writes data.
 * This function removes the necessity of data copy in the SPDK application
 * during DIF insertion and strip.
 *
 * When the extended LBA payload is split into multiple data segments,
 * start of each data segment is passed through the DIF context. data_offset
 * and data_len is within a data segment.
 *
 * \param iovs iovec array set by this function.
 * \param iovcnt Number of elements in the iovec array.
 * \param buf_iovs SGL for the buffer to create extended LBA payload.
 * \param buf_iovcnt Size of the SGL for the buffer to create extended LBA payload.
 * \param data_offset Offset to store the next incoming data in the current data segment.
 * \param data_len Expected length of the newly read data in the current data segment of
 * the extended LBA payload.
 * \param mapped_len Output parameter that will contain data length mapped by
 * the iovec array.
 * \param ctx DIF context.
 *
 * \return Number of used elements in the iovec array on success or negated
 * errno otherwise.
 */
/*
 * [한국어]
 * spdk_dif_set_md_interleave_iovs - 네트워크 read/write 시 메타 자리를 비워둔 SGL 구성.
 *
 * @iovs: 출력 — 본 함수가 채울 iovec 배열 (메타 영역을 건너뛴 데이터 조각들).
 * @iovcnt: iovs 배열 크기.
 * @buf_iovs: 입력 — 확장 LBA 형태의 실제 버퍼 SGL.
 * @buf_iovcnt: buf_iovs 엔트리 수.
 * @data_offset: 현재 segment 내에서 다음 들어올 데이터의 오프셋.
 * @data_len: 새로 읽힐 데이터 길이.
 * @mapped_len: 출력 — 실제로 매핑된 데이터 길이.
 * @ctx: PI 처리 파라미터.
 * @return: 사용된 iovs 엔트리 수 / 음수 errno 실패.
 *
 * 핵심: 네트워크가 데이터 부분만 직접 buf_iovs로 zero-copy 수신/전송하도록
 * 메타 자리를 건너뛴 SGL을 만들어 준다. 분할 segment 시 data_offset/data_len이
 * 현재 segment 내부 좌표.
 * 사용 사례: NVMe-oF/TCP transport가 호스트 데이터만 네트워크로 전달, PI는
 *           SPDK가 별도로 채움/검증.
 */
int spdk_dif_set_md_interleave_iovs(struct iovec *iovs, int iovcnt,
				    struct iovec *buf_iovs, int buf_iovcnt,
				    uint32_t data_offset, uint32_t data_len,
				    uint32_t *mapped_len,
				    const struct spdk_dif_ctx *ctx);

/**
 * Generate and insert DIF into metadata space for newly read data block.
 *
 * When the extended LBA payload is split into multiple data segments,
 * start of each data segment is passed through the DIF context. data_offset
 * and data_len is within a data segment.
 *
 * \param iovs iovec array describing the extended LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param data_offset Offset to the newly read data in the current data segment of
 * the extended LBA payload.
 * \param data_len Length of the newly read data in the current data segment of
 * the extended LBA payload.
 * \param ctx DIF context.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dif_generate_stream - 새로 수신된 데이터 블록에 대해 PI를 즉시 생성·삽입 (스트림).
 *
 * @iovs/@iovcnt: 확장 LBA 페이로드 SGL.
 * @data_offset: 새 데이터의 segment 내 오프셋.
 * @data_len: 새 데이터 길이.
 * @ctx: PI 컨텍스트 (in-out — last_guard/data_offset 갱신).
 * @return: 0 성공 / 음수 errno 실패.
 *
 * 네트워크에서 데이터가 부분적으로 들어올 때마다 호출. 부분 블록은 last_guard에
 * CRC 중간값을 누적해 두어 다음 호출에서 이어서 계산. NVMe-oF/iSCSI 용도.
 */
int spdk_dif_generate_stream(struct iovec *iovs, int iovcnt,
			     uint32_t data_offset, uint32_t data_len,
			     struct spdk_dif_ctx *ctx);

/**
 * Verify DIF for the to-be-written block of the extended LBA payload.
 *
 * \param iovs iovec array describing the extended LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param data_offset Offset to the to-be-written data in the extended LBA payload.
 * \param data_len Length of the to-be-written data in the extended LBA payload.
 * \param ctx DIF context.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dif_verify_stream - 디바이스에 보낼 예정인 부분 페이로드의 PI를 검증 (스트림).
 *
 * @iovs/@iovcnt: 확장 LBA 페이로드.
 * @data_offset: 검증 대상 데이터의 페이로드 내 오프셋.
 * @data_len: 검증 대상 길이.
 * @ctx: PI 컨텍스트 (in-out).
 * @err_blk: 출력 에러 정보.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * generate_stream의 짝 — write 경로에서 호스트가 PI 포함 데이터를 부분적으로
 * 모을 때, segment마다 검증해 손상 조기 발견.
 */
int spdk_dif_verify_stream(struct iovec *iovs, int iovcnt,
			   uint32_t data_offset, uint32_t data_len,
			   struct spdk_dif_ctx *ctx,
			   struct spdk_dif_error *err_blk);

/**
 * Calculate CRC-32C checksum of the specified range in the extended LBA payload.
 *
 * \param iovs iovec array describing the extended LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param data_offset Offset to the range
 * \param data_len Length of the range
 * \param crc32c Initial and updated CRC-32C value.
 * \param ctx DIF context.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dif_update_crc32c_stream - 부분 범위에 대한 CRC-32C 누적 계산 (스트림).
 *
 * @iovs/@iovcnt: 확장 LBA 페이로드.
 * @data_offset/@data_len: 계산 범위.
 * @crc32c: in-out — 누적 CRC.
 * @ctx: 블록 형상 정보.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * spdk_dif_update_crc32c의 부분 처리 버전. NVMe-oF/TCP digest 같은 segment 단위
 * CRC 계산에 사용.
 */
int spdk_dif_update_crc32c_stream(struct iovec *iovs, int iovcnt,
				  uint32_t data_offset, uint32_t data_len,
				  uint32_t *crc32c, const struct spdk_dif_ctx *ctx);
/**
 * Convert offset and size from LBA based to extended LBA based.
 *
 * \param data_offset Data offset
 * \param data_len Data length
 * \param buf_offset Buffer offset converted from data offset.
 * \param buf_len Buffer length converted from data length
 * \param ctx DIF context.
 */
/*
 * [한국어]
 * spdk_dif_get_range_with_md - LBA 기반 (offset, len)을 확장 LBA 기반으로 변환.
 *
 * @data_offset: LBA(데이터만) 기준 오프셋.
 * @data_len: LBA 기준 길이.
 * @buf_offset: 출력 — 확장 LBA(데이터+메타) 기준 오프셋.
 * @buf_len: 출력 — 확장 LBA 기준 길이.
 * @ctx: 블록 형상 정보 (block_size/md_size).
 *
 * 헬퍼 — 메타 영역만큼 부풀려진 좌표를 계산. 호출자가 PI 포함 버퍼를 할당·접근할 때 사용.
 */
void spdk_dif_get_range_with_md(uint32_t data_offset, uint32_t data_len,
				uint32_t *buf_offset, uint32_t *buf_len,
				const struct spdk_dif_ctx *ctx);

/**
 * Convert length from LBA based to extended LBA based.
 *
 * \param data_len Data length
 * \param ctx DIF context.
 *
 * \return Extended LBA based data length.
 */
/*
 * [한국어]
 * spdk_dif_get_length_with_md - LBA 기준 길이를 확장 LBA 기준 길이로 변환.
 *
 * @data_len: LBA(데이터만) 기준 바이트 길이.
 * @ctx: 블록 형상.
 * @return: 메타 영역을 포함한 확장 LBA 기준 길이.
 *
 * get_range_with_md의 길이만 필요한 경우의 단순화 버전.
 */
uint32_t spdk_dif_get_length_with_md(uint32_t data_len, const struct spdk_dif_ctx *ctx);

/**
 * Remap reference tag for extended LBA payload.
 *
 * When using stacked virtual bdev (e.g. split virtual bdev), block address space for I/O
 * will be remapped during I/O processing and so reference tag will have to be remapped
 * accordingly. This patch is for that case.
 *
 * \param iovs iovec array describing the extended LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param num_blocks Number of blocks of the payload.
 * \param ctx DIF context.
 * \param err_blk Error information of the block in which DIF error is found.
 * \param check_ref_tag If true, check the reference tag before updating.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dif_remap_ref_tag - 확장 LBA 페이로드의 reference tag를 새 값으로 갱신.
 *
 * @iovs/@iovcnt: 확장 LBA 페이로드.
 * @num_blocks: 블록 수.
 * @dif_ctx: PI 컨텍스트 (remapped_init_ref_tag 필드가 미리 set 되어야 함).
 * @err_blk: 출력 — check_ref_tag=true일 때 검증 실패 정보.
 * @check_ref_tag: true면 갱신 전에 기존 reftag를 검증, false면 무조건 덮어쓰기.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * 사용 사례: split bdev 같은 가상 bdev가 부모 LBA를 자식 LBA로 변환할 때
 *           PI 영역의 reftag도 함께 갱신해야 디바이스가 올바른 검증을 할 수 있음.
 *           Type 1 ns에서 주로 사용.
 */
int spdk_dif_remap_ref_tag(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
			   const struct spdk_dif_ctx *dif_ctx,
			   struct spdk_dif_error *err_blk,
			   bool check_ref_tag);

/**
 * Remap reference tag for separate metadata payload.
 *
 * When using stacked virtual bdev (e.g. split virtual bdev), block address space for I/O
 * will be remapped during I/O processing and so reference tag will have to be remapped
 * accordingly. This patch is for that case.
 *
 * \param md_iov A contiguous buffer for metadata.
 * \param num_blocks Number of blocks of the payload.
 * \param ctx DIF context.
 * \param err_blk Error information of the block in which DIF error is found.
 * \param check_ref_tag If true, check the reference tag before updating.
 *
 * \return 0 on success and negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_dix_remap_ref_tag - DIX 모드 메타데이터 버퍼의 reference tag를 새 값으로 갱신.
 *
 * @md_iov: 별도 메타데이터 버퍼 (PI 포함).
 * @num_blocks: 블록 수.
 * @dif_ctx: PI 컨텍스트 (remapped_init_ref_tag set 필요).
 * @err_blk: 출력 에러 정보.
 * @check_ref_tag: true면 갱신 전 검증.
 * @return: 0 성공 / 음수 errno 실패.
 *
 * spdk_dif_remap_ref_tag의 DIX 버전. 데이터 SGL은 건드리지 않고 md_iov만 갱신.
 */
int spdk_dix_remap_ref_tag(struct iovec *md_iov, uint32_t num_blocks,
			   const struct spdk_dif_ctx *dif_ctx,
			   struct spdk_dif_error *err_blk,
			   bool check_ref_tag);

/**
 * Get PI field size for the PI format
 *
 * \param dif_pi_format DIF PI format type
 *
 * \return Size of the PI field.
 */
/*
 * [한국어]
 * spdk_dif_pi_format_get_size - PI 포맷에 해당하는 PI 필드 바이트 크기 반환.
 *
 * @dif_pi_format: enum spdk_dif_pi_format 값.
 * @return: PI 필드의 바이트 크기 (16=8바이트, 32/64=16바이트).
 *
 * 메타 영역 내 PI 위치 계산, NVMe Identify Namespace의 NPDA/NPDG 검증 등에 사용.
 */
uint32_t spdk_dif_pi_format_get_size(enum spdk_dif_pi_format dif_pi_format);

#ifdef __cplusplus
}
#endif
#endif /* SPDK_DIF_H */                /* [한국어] include 가드 닫기 */
