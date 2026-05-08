/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright 2023 Solidigm All Rights Reserved
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 내부 공통 정의 + P2L 매핑/체크포인트/IO 로그 인터페이스 (ftl_internal.h)
 *
 * === 파일의 역할 ===
 * lib/ftl 내부에서 공유되는 핵심 상수, 타입, 자료구조, P2L(Physical-to-Logical) 매핑/체크포인트/
 * IO 로그/relocation 인터페이스를 한 곳에 모아둔 헤더이다. 정의되는 것:
 *   - FTL_ADDR_INVALID, FTL_LBA_INVALID, FTL_BLOCK_SIZE(=4 KiB) 등 기본 상수.
 *   - FTL_P2L_VERSION_*, FTL_P2L_LOG_VERSION_*, FTL_TRIM_LOG_VERSION_* 버전 상수.
 *   - typedef uint64_t ftl_addr — FTL 주소 공간(0..base_size = base lba, 그 위 = nv cache lba).
 *   - enum ftl_md_type/band_type/md_status — 메타데이터/밴드 타입과 상태.
 *   - struct ftl_p2l_map / map_entry — P2L 매핑 본체 + 한 엔트리.
 *   - struct ftl_p2l_sync_ctx, ftl_p2l_ckpt_page(_no_vss) — 체크포인트/동기화 자료구조.
 *   - struct ftl_trim_log — 트림 로그 페이지.
 *   - p2l_ckpt/log/reloc 함수 선언 — 체크포인트 init/issue/acquire/release, relocation API,
 *     P2L IO 로그 init/log/flush/acquire/release/read.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/ftl 내부 모듈들이 서로의 인터페이스를 참조하는 hub 헤더.
 * 호출 체인: ftl_band.c / ftl_l2p.c / ftl_writer.c / ftl_p2l_ckpt.c / ftl_p2l_log.c / ftl_reloc.c
 *   → 이 헤더의 자료구조와 함수들을 통해 협업.
 * 실행 컨텍스트: 모든 SPDK reactor 스레드. 자료구조는 thread affinity로 보호되며,
 *   p2l_log_acquire/release처럼 lock-free ring을 사용하는 경로는 명시적 동기화.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/stdinc.h, spdk/crc32.h, spdk/util.h, spdk/uuid.h, spdk/ftl.h(공개 API),
 *   utils/ftl_bitmap.h(p2l_map.valid), utils/ftl_md.h(union ftl_md_vss).
 * 의존되는 모듈: lib/ftl 거의 모든 .c 파일. 특히 ftl_l2p, ftl_band, ftl_writer, ftl_reloc,
 *   ftl_p2l_ckpt, ftl_p2l_log.
 * 데이터 흐름: P2L 매핑 자료구조가 GC relocation, dirty shutdown 복구의 중심. 체크포인트는
 *   주기적으로 디스크에 영속화되어 복구 시 재구성 시간 단축.
 *
 * === 주요 함수/구조체 요약 ===
 *   - ftl_addr: 64비트 FTL 통합 주소 — base bdev 또는 NV cache 어느 쪽인지 값 범위로 결정.
 *   - struct ftl_p2l_map: GC/복구의 핵심 — valid bitmap + P2L 엔트리 배열 + DMA 버퍼.
 *   - struct ftl_p2l_ckpt_page(_no_vss): 4 KiB 한 페이지에 채울 P2L 엔트리 배치.
 *   - ftl_p2l_ckpt_init/deinit/issue/acquire/release: 체크포인트 라이프사이클.
 *   - ftl_reloc_init/free/halt/resume/is_halted: GC relocation 엔진 라이프사이클.
 *   - ftl_p2l_log_init/deinit/log/flush/acquire/release/read: P2L IO 로그.
 */

#ifndef FTL_INTERNAL_H
#define FTL_INTERNAL_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "spdk/stdinc.h"
/* [한국어] 표준 타입(uint64_t/size_t/int 등). */
#include "spdk/crc32.h"
/* [한국어] CRC32 함수 — 메타데이터 무결성 검증에 사용(주로 .c에서 호출, 헤더는 타입 노출). */
#include "spdk/util.h"
/* [한국어] SPDK_STATIC_ASSERT, SPDK_COUNTOF_MEMBER 등 매크로. */
#include "spdk/uuid.h"
/* [한국어] spdk_uuid 정의 — 메타데이터 헤더에 UUID 포함될 수 있어 필요. */
#include "spdk/ftl.h"
/* [한국어] FTL 공개 API — spdk_ftl_fn 콜백 타입과 외부 노출 자료형. */

#include "utils/ftl_bitmap.h"
/* [한국어] ftl_p2l_map.valid의 ftl_bitmap 타입. */
#include "utils/ftl_md.h"
/* [한국어] union ftl_md_vss 정의 — VSS(Vendor Specific Signature, NVMe metadata-per-block)와
 * 같은 메타데이터 타입을 P2L 페이지 안에 임베드할 때 사용. */

/* Marks address as invalid */
#define FTL_ADDR_INVALID	((ftl_addr)-1)
/* [한국어] ftl_addr "유효하지 않음" sentinel = 0xFFFFFFFFFFFFFFFF.
 * L2P 슬롯이 매핑되지 않았거나 무효화된 경우 이 값으로 표시.
 * packed(32b) 모드에서는 (uint32_t)-1로 절단되어도 의미 보존(ftl_addr_utils.h 참고). */

/* Marks LBA as invalid */
#define FTL_LBA_INVALID		((uint64_t)-1)
/* [한국어] LBA "유효하지 않음" sentinel = 0xFFFFFFFFFFFFFFFF. P2L 슬롯에서도 동일 의미. */

/* Smallest data unit size */
#define FTL_BLOCK_SIZE		4096ULL
/* [한국어] FTL의 최소 데이터 단위 = 4 KiB(NVMe 표준 LBA 사이즈와 일치).
 * 모든 매핑/I/O는 이 단위로 정렬된다. */

#define FTL_P2L_VERSION_0	0
/* [한국어] P2L 매핑 영역의 버전 0(초기 형태). */
#define FTL_P2L_VERSION_1	1
/* [한국어] P2L 매핑 v1. */
#define FTL_P2L_VERSION_2	2
/* [한국어] P2L 매핑 v2 — 현재 사용 중. */

#define FTL_P2L_VERSION_CURRENT FTL_P2L_VERSION_2
/* [한국어] 컴파일 시점 최신 P2L 매핑 버전 — 새 빌드는 항상 이 버전으로 영역 생성. */

#define FTL_P2L_LOG_VERSION_0	0
/* [한국어] P2L IO 로그 버전 0 — 현재까지의 유일한 버전. */

#define FTL_P2L_LOG_VERSION_CURRENT FTL_P2L_LOG_VERSION_0
/* [한국어] 최신 P2L 로그 버전. */

/*
 * This type represents address in the ftl address space. Values from 0 to based bdev size are
 * mapped directly to base device lbas. Values above that represent nv cache lbas.
 */
typedef uint64_t ftl_addr;
/* [한국어] FTL 통합 주소 타입 — 64비트.
 *  - 0..base_bdev_size-1: base bdev의 LBA (영구 영역).
 *  - base_bdev_size 이상: NV cache LBA(쓰기 캐시). 두 디바이스를 한 주소 공간으로 추상화. */

struct spdk_ftl_dev;
/* [한국어] forward declaration — ftl_core.h가 정의. */

enum ftl_md_type {
	FTL_MD_TYPE_BAND,
	/* [한국어] 메타데이터 타입 = 밴드(base bdev 영역의 erase block 단위 그룹) 메타데이터. */
	FTL_MD_TYPE_CHUNK
	/* [한국어] 메타데이터 타입 = 청크(NV cache의 영역 단위) 메타데이터. */
};

enum ftl_band_type {
	FTL_BAND_TYPE_GC = 1,
	/* [한국어] GC(Garbage Collection)에 의해 회수 중인 밴드 타입.
	 * 값 1부터 시작 — 0은 미설정 sentinel로 두기 위함. */
	FTL_BAND_TYPE_COMPACTION
	/* [한국어] compaction(압축)에 의해 처리 중인 밴드 타입. */
};

enum ftl_md_status {
	FTL_MD_SUCCESS,
	/* [한국어] 메타데이터 read 성공. */
	/* Metadata read failure */
	FTL_MD_IO_FAILURE,
	/* [한국어] bdev I/O 자체 실패. */
	/* Invalid version */
	FTL_MD_INVALID_VER,
	/* [한국어] 버전 필드가 알려진 값이 아님 — 다른 시스템 메타데이터 또는 손상. */
	/* UUID doesn't match */
	FTL_MD_NO_MD,
	/* [한국어] UUID 불일치 — 다른 FTL 인스턴스 데이터이거나 메타데이터 부재. */
	/* UUID and version matches but CRC doesn't */
	FTL_MD_INVALID_CRC,
	/* [한국어] UUID/버전은 맞지만 CRC 불일치 — 데이터 손상. */
	/* Vld or p2l map size doesn't match */
	FTL_MD_INVALID_SIZE
	/* [한국어] valid bitmap 또는 p2l map 크기가 기대와 불일치 — 디바이스 형상 변경 등. */
};

struct ftl_p2l_map_entry {
	uint64_t lba;
	/* [한국어] 이 슬롯에 저장된 LBA. FTL_LBA_INVALID이면 미사용.
	 * 설정자: write 발생 시 ftl_writer가 기록. 읽는 자: GC/복구 시 P2L 디시리얼라이즈. */
	uint64_t seq_id;
	/* [한국어] 해당 write의 시퀀스 번호 — dirty 복구 시 순서 결정에 사용.
	 * 설정자: write 시점. 읽는 자: 복구 디스패처. */
};

/* Number of p2l entries that could be stored in a single block for bands */
#define FTL_NUM_LBA_IN_BLOCK	(FTL_BLOCK_SIZE / sizeof(struct ftl_p2l_map_entry))
/* [한국어] 4 KiB 블록 한 개에 들어가는 P2L 엔트리 개수 = 4096 / 16 = 256.
 * 밴드용 P2L 페이지의 기본 용량. */

/*
 * Mapping of physical (actual location on disk) to logical (user's POV) addresses. Used in two main scenarios:
 * - during relocation FTL needs to pin L2P pages (this allows to check which pages to pin) and move still valid blocks
 * (valid map allows for preliminary elimination of invalid physical blocks, but user data could invalidate a location
 * during read/write operation, so actual comparison against L2P needs to be done)
 * - After dirty shutdown the state of the L2P is unknown and needs to be rebuilt - it is done by applying all P2L, taking
 * into account ordering of user writes
 */
struct ftl_p2l_map {
	/* Number of valid LBAs */
	size_t					num_valid;
	/* [한국어] valid 비트맵에서 set인 비트 수 — 빠른 조회용 캐시.
	 * 설정자: write/invalidate 시 갱신. 읽는 자: GC 후보 선정(낮은 num_valid 우선).
	 * 동기화: 한 밴드는 한 스레드에서만 다뤄지므로 비-원자적. */

	/* P2L map's reference count, prevents premature release of resources during dirty shutdown recovery for open bands */
	size_t					ref_cnt;
	/* [한국어] P2L 맵 참조 카운트 — open 밴드의 복구 중 자원 조기 해제 방지.
	 * 설정자: 복구 시 +1, 완료 시 -1. 읽는 자: free 결정 시 0인지 검사. */

	/* Bitmap of valid LBAs */
	struct ftl_bitmap			*valid;
	/* [한국어] 슬롯별 유효성 비트맵 — 1=유효, 0=무효(invalidate되었거나 미사용).
	 * 설정자: write 시 set, invalidate 시 clear. 읽는 자: GC 후보 검사, num_valid 재계산. */

	/* P2L map (only valid for open/relocating bands) */
	union {
		struct ftl_p2l_map_entry	*band_map;
		/* [한국어] 밴드용 P2L 엔트리 배열 — 슬롯 인덱스로 직접 접근. open/relocating 밴드만 메모리에 보유.
		 * 설정자: 밴드 open 시 alloc. 읽는 자: GC/relocation 시 lba/seq_id 조회. */

		struct ftl_p2l_map_entry	*chunk_map;
		/* [한국어] 청크용 P2L 슬롯 — band_map과 동일 슬롯이지만 NV cache 청크 단위 처리에 쓰임.
		 * union으로 같은 메모리를 공유 — 한 시점에 하나만 의미를 가짐. */
	};

	/* DMA buffer for region's metadata entry */
	union {
		struct ftl_band_md		*band_dma_md;
		/* [한국어] 밴드 메타데이터의 DMA 버퍼 — bdev write 직접 가능한 hugepage 메모리.
		 * 설정자: 메타데이터 영역 alloc 시. 읽는 자: bdev_write_blocks 호출 시 buf 인자. */

		struct ftl_nv_cache_chunk_md	*chunk_dma_md;
		/* [한국어] NV cache 청크 메타데이터의 DMA 버퍼 (union으로 같은 슬롯 공유). */
	};

	/* P2L checkpointing region */
	struct ftl_p2l_ckpt			*p2l_ckpt;
	/* [한국어] 이 P2L 맵의 체크포인트 영역 핸들 — 주기적으로 영속화될 때 사용.
	 * 설정자: ftl_p2l_ckpt_acquire로 획득. 읽는 자: 체크포인트 issue/release. */
};

struct ftl_p2l_sync_ctx {
	struct ftl_band *band;
	/* [한국어] 동기화 대상 밴드 핸들. */
	uint64_t	xfer_start;
	/* [한국어] 동기화할 슬롯 시작 인덱스. */
	uint64_t	xfer_end;
	/* [한국어] 동기화할 슬롯 종료 인덱스(배타). */
	int		md_region;
	/* [한국어] 어느 메타데이터 영역에 영속화할지(layout region 인덱스). */
};
/* [한국어] P2L 동기 영속화 한 회차의 컨텍스트 — issue 시 콜백 인자로 사용. */

struct ftl_p2l_ckpt_page {
	struct ftl_p2l_map_entry map[FTL_NUM_LBA_IN_BLOCK];
	/* [한국어] 4 KiB 페이지 한 개에 256 엔트리(=FTL_NUM_LBA_IN_BLOCK) — VSS 미사용 단순 페이지. */
};

struct ftl_p2l_ckpt_page_no_vss {
	union ftl_md_vss metadata;
	/* [한국어] 페이지 헤더로 사용되는 VSS — 보통 시퀀스 ID/CRC 등 메타정보 담음.
	 * VSS가 별도 메타데이터 영역이 아닌 페이지 자체에 임베드되는 모드용. */
	struct ftl_p2l_map_entry map[FTL_NUM_LBA_IN_BLOCK - sizeof(union ftl_md_vss) / sizeof(
								  struct ftl_p2l_map_entry)];
	/* [한국어] VSS 차지분만큼 줄어든 엔트리 배열.
	 * 계산: 256 - sizeof(VSS)/16 = 256 - 1 = 255 (VSS=16바이트로 가정).
	 * 페이지 정확히 4 KiB가 되도록 정밀 산술. */
} __attribute__((packed));
/* [한국어] packed — 디스크 레이아웃과 1:1. */
SPDK_STATIC_ASSERT(sizeof(struct ftl_p2l_ckpt_page_no_vss) == FTL_BLOCK_SIZE,
		   "ftl_p2l_ckpt_page_no_vss incorrect size");
/* [한국어] 페이지 정확히 4 KiB임을 컴파일 시 강제 — 어긋나면 빌드 실패. */

#define FTL_NUM_P2L_ENTRIES_NO_VSS (SPDK_COUNTOF_MEMBER(struct ftl_p2l_ckpt_page_no_vss, map))
/* [한국어] no-vss 페이지의 엔트리 개수를 매크로로 노출 — SPDK_COUNTOF_MEMBER가 컴파일 타임 계산. */

#define FTL_TRIM_LOG_VERSION_0		0
/* [한국어] 트림 로그 버전 0(초기 형태). */
#define FTL_TRIM_LOG_VERSION_1		1
/* [한국어] 트림 로그 v1 — 현재 사용. */
#define FTL_TRIM_LOG_VERSION_CURRENT	FTL_TRIM_LOG_VERSION_1
/* [한국어] 컴파일 시점 최신 트림 로그 버전. */

struct ftl_trim_log {
	union ftl_md_vss hdr;
	/* [한국어] 트림 로그 페이지 헤더(VSS 형식 — 시퀀스/CRC 등). */
	char reserved[FTL_BLOCK_SIZE - sizeof(union ftl_md_vss)];
	/* [한국어] 페이지 나머지를 reserved로 채워 정확히 4 KiB 보장. 향후 확장 영역. */
};
SPDK_STATIC_ASSERT(sizeof(struct ftl_trim_log) == FTL_BLOCK_SIZE, "Invalid trim log page size");
/* [한국어] 트림 로그 페이지가 정확히 4 KiB임을 강제. */

struct ftl_p2l_ckpt;
/* [한국어] forward declaration — ftl_p2l_ckpt.c가 내부 정의. */
struct ftl_p2l_log;
/* [한국어] forward declaration — ftl_p2l_log.c가 내부 정의. */
struct ftl_band;
/* [한국어] forward declaration — ftl_band.h가 정의. */
struct spdk_ftl_dev;
/* [한국어] forward declaration — 위에서 이미 했으나 가독성용 재선언. */
struct ftl_mngt_process;
/* [한국어] forward declaration — mngt 파이프라인 핸들. */
struct ftl_io;
/* [한국어] forward declaration — FTL I/O 객체. */
struct ftl_rq;
/* [한국어] forward declaration — FTL 요청 객체. */

/*
 * [한국어]
 * ftl_p2l_ckpt_init - 디바이스의 P2L 체크포인트 시스템 초기화
 *
 * @param dev: FTL 디바이스.
 * @return: 0 = 성공, 음수 = 에러.
 *
 * 동기/배경: dirty shutdown 후 L2P 재구성 시간을 줄이기 위해 P2L 매핑을 주기적으로 영속화.
 *   이 함수가 체크포인트 영역 풀과 큐를 준비.
 * 실행 컨텍스트: 디바이스 init mngt 단계.
 */
int ftl_p2l_ckpt_init(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_p2l_ckpt_deinit - P2L 체크포인트 시스템 해제
 */
void ftl_p2l_ckpt_deinit(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_p2l_ckpt_issue - 요청에 묶인 P2L 데이터를 체크포인트 영역에 비동기 영속화
 *
 * @param rq: 발급할 ftl_rq — 안에 ckpt 핸들과 P2L 데이터 포함.
 *
 * 호출 체인: write 완료 후 → ftl_writer → [이 함수] → bdev write → 콜백.
 */
void ftl_p2l_ckpt_issue(struct ftl_rq *rq);

/*
 * [한국어]
 * ftl_p2l_ckpt_acquire - free 풀에서 체크포인트 핸들 1개 획득
 *
 * @param dev: FTL 디바이스. @return: 핸들 또는 NULL(풀 비어있음).
 */
struct ftl_p2l_ckpt *ftl_p2l_ckpt_acquire(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_p2l_ckpt_acquire_region_type - 특정 layout region type에 묶인 체크포인트 획득
 *
 * @param dev: FTL 디바이스.
 * @param region_type: ftl_layout_region_type 값(uint32_t).
 * @return: 해당 영역에 매핑된 체크포인트 핸들.
 *
 * 사용 예: NV cache 청크 P2L과 base bdev band P2L이 별도 영역을 쓰므로 영역 타입으로 구별.
 */
struct ftl_p2l_ckpt *ftl_p2l_ckpt_acquire_region_type(struct spdk_ftl_dev *dev,
		uint32_t region_type);

/*
 * [한국어]
 * ftl_p2l_ckpt_release - 체크포인트 핸들을 free 풀로 반환
 */
void ftl_p2l_ckpt_release(struct spdk_ftl_dev *dev, struct ftl_p2l_ckpt *ckpt);

/*
 * [한국어]
 * ftl_p2l_ckpt_region_type - 체크포인트가 매핑된 layout region 타입 조회
 *
 * @param ckpt: 체크포인트 핸들.
 * @return: 해당 영역의 ftl_layout_region_type.
 */
enum ftl_layout_region_type ftl_p2l_ckpt_region_type(const struct ftl_p2l_ckpt *ckpt);

#if defined(DEBUG)
/*
 * [한국어]
 * ftl_p2l_validate_ckpt - DEBUG 빌드에서 체크포인트 영역의 일관성을 검증
 *
 * @param band: 검증 대상 밴드.
 *
 * 동기/배경: 디버그 모드에서 P2L과 체크포인트 사이 정합성 검사 — 릴리즈에서는 인라인 빈 함수.
 */
void ftl_p2l_validate_ckpt(struct ftl_band *band);
#else
/*
 * [한국어]
 * ftl_p2l_validate_ckpt - 릴리즈 빌드에서는 빈 인라인 함수 (no-op)
 *
 * 호출자가 #ifdef DEBUG 분기 없이 항상 호출할 수 있게 하면서 릴리즈에선 비용 0.
 */
static inline void
ftl_p2l_validate_ckpt(struct ftl_band *band)
{
	/* [한국어] 릴리즈 모드: 의도적으로 빈 함수 — 호출 비용 제거. */
}
#endif

/*
 * [한국어]
 * ftl_mngt_p2l_ckpt_get_seq_id - 특정 layout region의 체크포인트 시퀀스 ID 조회
 *
 * @param dev: FTL 디바이스.
 * @param md_region: layout region 인덱스.
 * @return: 시퀀스 ID(영속화된 마지막 시점) 또는 0(체크포인트 없음).
 */
uint64_t ftl_mngt_p2l_ckpt_get_seq_id(struct spdk_ftl_dev *dev, int md_region);

/*
 * [한국어]
 * ftl_mngt_p2l_ckpt_restore - 체크포인트로부터 밴드의 P2L 매핑 복원 (dirty 복구)
 *
 * @param band: 대상 밴드.
 * @param md_region: 복원에 사용할 layout region.
 * @param seq_id: 해당 시퀀스 번호 이후의 데이터를 복원.
 * @return: 0 = 성공, 음수 = 실패.
 */
int ftl_mngt_p2l_ckpt_restore(struct ftl_band *band, uint32_t md_region, uint64_t seq_id);

/*
 * [한국어]
 * ftl_mngt_p2l_ckpt_restore_clean - 정상 종료 후 빠른 P2L 복원
 *
 * @param band: 대상 밴드.
 * @return: 0 = 성공, 음수 = 실패.
 *
 * 동기/배경: 정상 종료 시 모든 P2L이 깔끔히 영속화되어 있으므로 단순 read로 복원 가능.
 */
int ftl_mngt_p2l_ckpt_restore_clean(struct ftl_band *band);

/*
 * [한국어]
 * ftl_mngt_p2l_ckpt_restore_shm_clean - SHM clean 상태에서의 P2L 복원 (재시작 가속)
 *
 * @param band: 대상 밴드.
 *
 * 동기/배경: 빠른 재시작 모드에서 hugepage SHM에 남아있는 P2L 사본으로 즉시 복원.
 */
void ftl_mngt_p2l_ckpt_restore_shm_clean(struct ftl_band *band);

/*
 * [한국어]
 * ftl_mngt_persist_bands_p2l - 모든 밴드의 P2L 매핑을 디스크에 영속화 후 mngt next_step
 *
 * @param mngt: ftl_mngt 파이프라인 핸들.
 *
 * 동기/배경: 정상 종료 시 호출되어 모든 P2L을 깨끗이 영속화. 비동기 — 완료 시 mngt 진행.
 */
void ftl_mngt_persist_bands_p2l(struct ftl_mngt_process *mngt);

/*
 * [한국어]
 * ftl_reloc_init - GC relocation 엔진 초기화
 *
 * @param dev: FTL 디바이스.
 * @return: relocation 엔진 핸들 또는 NULL.
 */
struct ftl_reloc *ftl_reloc_init(struct spdk_ftl_dev *dev);

/*
 * [한국어]
 * ftl_reloc_free - GC relocation 엔진 해제
 */
void ftl_reloc_free(struct ftl_reloc *reloc);

/*
 * [한국어]
 * ftl_reloc - relocation 엔진의 한 사이클 실행 (poller에서 주기적 호출)
 *
 * @param reloc: relocation 엔진.
 *
 * 동기/배경: SPDK poller가 무한 루프로 호출하며 진행 가능한 한 단계씩 처리.
 *   GC 후보 선정 → 유효 데이터 read → write 큐 → 새 위치에 쓰기.
 * 실행 컨텍스트: SPDK reactor 스레드, poller 콜백.
 */
void ftl_reloc(struct ftl_reloc *reloc);

/*
 * [한국어]
 * ftl_reloc_halt - relocation 엔진을 일시 정지 (디바이스 정지 등)
 */
void ftl_reloc_halt(struct ftl_reloc *reloc);

/*
 * [한국어]
 * ftl_reloc_resume - relocation 엔진 재개
 */
void ftl_reloc_resume(struct ftl_reloc *reloc);

/*
 * [한국어]
 * ftl_reloc_is_halted - relocation 엔진이 정지 상태인지 조회
 *
 * @param reloc: 엔진.
 * @return: true = 정지, false = 동작 중.
 */
bool ftl_reloc_is_halted(const struct ftl_reloc *reloc);

/**
 * P2L IO log
 */
/*
 * @brief Initialize P2L IO log
 *
 * @param dev FTL device
 *
 * @return Initialization result
 */
/*
 * [한국어]
 * ftl_p2l_log_init - P2L IO 로그 시스템 초기화
 *
 * @param dev: FTL 디바이스.
 * @return: 0 = 성공, 음수 = 에러.
 *
 * 동기/배경: P2L 체크포인트와는 별개로, 모든 write를 IO 단위로 로그에 기록해 dirty 복구 시
 *   체크포인트 + 로그 재생으로 정확한 L2P 재구성. 이 함수가 로그 풀과 ring을 준비.
 */
int ftl_p2l_log_init(struct spdk_ftl_dev *dev);

/**
 * @brief Deinitialize P2L IO log
 *
 * @param dev FTL device
 */
/*
 * [한국어]
 * ftl_p2l_log_deinit - P2L IO 로그 시스템 해제
 */
void ftl_p2l_log_deinit(struct spdk_ftl_dev *dev);

/**
 * @brief Get number of blocks required in FTL MD object to store P2L IO log
 *
 * @param dev FTL device
 * @param write_unit_blocks number of blocks in a write unit
 * @param max_user_data_blocks maximum number of user data blocks within a chunk/band
 *
 * @return Number of blocks required
 */
/*
 * [한국어]
 * ftl_p2l_log_get_md_blocks_required - P2L IO 로그를 저장할 메타데이터 영역 크기(블록 수) 산출
 *
 * @param dev: FTL 디바이스.
 * @param write_unit_blocks: 한 write unit의 블록 수.
 * @param max_user_data_blocks: 청크/밴드 내 최대 사용자 데이터 블록 수.
 * @return: 필요한 4 KiB 블록 개수 — 메타데이터 영역 alloc 시 사용.
 */
uint64_t ftl_p2l_log_get_md_blocks_required(struct spdk_ftl_dev *dev,
		uint64_t write_unit_blocks,
		uint64_t max_user_data_blocks);

/**
 * @brief Add an IO to the P2L IO log
 *
 * @param p2l P2L IO log
 * @param io The IO to be logged
 */
/*
 * [한국어]
 * ftl_p2l_log_io - 한 IO를 P2L IO 로그에 추가
 *
 * @param p2l: 로그 핸들.
 * @param io: 로그할 IO 객체 — lba, addr, seq_id 등이 추출되어 기록.
 *
 * 호출 체인: ftl_writer write 완료 후 → [이 함수] → 로그 entry 인메모리 추가 → 주기적으로 flush.
 */
void ftl_p2l_log_io(struct ftl_p2l_log *p2l, struct ftl_io *io);

/**
 * @brief Flush the P2L IO logs
 *
 * @param dev FTL device
 */
/*
 * [한국어]
 * ftl_p2l_log_flush - 모든 인메모리 P2L IO 로그를 디스크에 영속화
 *
 * @param dev: FTL 디바이스.
 */
void ftl_p2l_log_flush(struct spdk_ftl_dev *dev);

/**
 * @brief Callback function invoked when IO is logged
 *
 * @param io IO that P2L logging was finished
 */
typedef void (*ftl_p2l_log_cb)(struct ftl_io *io);
/* [한국어] IO 로그 완료 콜백 타입 — log_acquire 시 등록.
 * 한 IO에 대한 로깅이 끝나면 호출되어 호출자가 IO를 다음 단계로 진행. */

/**
 * @brief Get layout region type corresponding to the specific P2L log
 *
 * @param p2l P2L log
 *
 * @return Layout region type
 */
/*
 * [한국어]
 * ftl_p2l_log_type - P2L 로그가 매핑된 layout region 타입 조회
 */
enum ftl_layout_region_type ftl_p2l_log_type(struct ftl_p2l_log *p2l);

/**
 * @brief Acquire P2L IO log
 *
 * @param dev FTL device
 * @param seq_id Sequence ID of the P2L IO log
 * @param cb Callback function invoked when IO logging is finished
 *
 * @return The P2L IO log
 */
/*
 * [한국어]
 * ftl_p2l_log_acquire - 시퀀스 ID에 해당하는 P2L IO 로그 핸들 획득
 *
 * @param dev: FTL 디바이스.
 * @param seq_id: 로그가 속할 시퀀스 번호.
 * @param cb: 로깅 완료 콜백.
 * @return: 로그 핸들 또는 NULL(풀 비어있음).
 */
struct ftl_p2l_log *ftl_p2l_log_acquire(struct spdk_ftl_dev *dev,
					uint64_t seq_id,
					ftl_p2l_log_cb cb);

/**
 * @brief Release P2L IO log
 *
 * @param dev FTL device
 * @param p2l P2L IO log to be released
 */
/*
 * [한국어]
 * ftl_p2l_log_release - P2L IO 로그 핸들을 풀로 반환
 */
void ftl_p2l_log_release(struct spdk_ftl_dev *dev, struct ftl_p2l_log *p2l);

/**
 * @brief P2L Log read callback
 *
 * @param dev FTL device
 * @param cb_arg The callback argument
 * @param lba LBA value of P2L log entry
 * @param addr Physical address of P2L log entry
 * @param seq_id Sequence ID of P2L log entry
 *
 * @retval 0 Continue reading
 * @retval Non-zero Stop reading
 */
typedef int (*ftl_p2l_log_rd_cb)(struct spdk_ftl_dev *dev, void *cb_arg,
				 uint64_t lba, ftl_addr addr, uint64_t seq_id);
/* [한국어] 로그 read 시 항목별 콜백 타입 — 로그를 읽으면서 발견되는 매 entry마다 호출.
 * 반환 0이면 다음 항목 계속 읽기, 비-0이면 읽기 중단(예: 충분한 정보 수집됨). */

/**
 * @brief Read P2L IO log
 *
 * @param dev FTL device
 * @param type The P2L IO log layout region type
 * @param seq_id The sequence number of the log
 * @param cb_fn The callback function which will be invoked when reading process finished
 * @param cb_arg The callback argument
 * @param cb_rd The callback function to report items found in the P2L log
 *
 * @return Operation result
 * @retval 0 - The reading procedure started successfully
 * @retval non-zero - An error occurred and the reading did not started
 */
/*
 * [한국어]
 * ftl_p2l_log_read - 영속 P2L IO 로그를 비동기로 읽으며 항목별 콜백 호출
 *
 * @param dev: FTL 디바이스.
 * @param type: 로그가 위치한 layout region 타입.
 * @param seq_id: 시작 시퀀스 번호.
 * @param cb_fn: 전체 읽기 완료 시 호출될 콜백(spdk_ftl_fn).
 * @param cb_arg: 콜백 컨텍스트.
 * @param cb_rd: 항목별로 호출될 콜백 — 비-0 반환으로 조기 종료 가능.
 * @return: 0 = 비동기 시작 성공, 음수 = 즉시 실패.
 *
 * 동기/배경: dirty 복구 시 체크포인트 + 로그 재생으로 정확한 L2P 재구성에 사용.
 */
int ftl_p2l_log_read(struct spdk_ftl_dev *dev, enum ftl_layout_region_type type, uint64_t seq_id,
		     spdk_ftl_fn cb_fn, void *cb_arg, ftl_p2l_log_rd_cb cb_rd);


#endif /* FTL_INTERNAL_H */
/* [한국어] 헤더 가드 종료. */
