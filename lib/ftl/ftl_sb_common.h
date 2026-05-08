/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 슈퍼블록 모든 버전이 공유하는 공통 정의 (ftl_sb_common.h)
 *
 * === 파일의 역할 ===
 * 슈퍼블록 v2/v3/v5/current 모든 버전이 공통으로 사용하는 상수와 자료구조를 정의한다.
 * 핵심은: (1) FTL_SUPERBLOCK_SIZE = 128 KiB의 영구 영역 크기, (2) FTL_MAGIC 매크로와 매직 상수,
 * (3) ftl_superblock_header(magic+crc+version 24바이트) — 모든 버전이 동일하게 시작하는 헤더,
 * (4) ftl_superblock_gc_info(GC 진행 상태) — v2 이후 모든 버전에 임베드,
 * (5) ftl_superblock_v3_md_region — v3 chained list 노드,
 * (6) ftl_superblock_v5_md_blob_hdr — v5 blob 헤더,
 * (7) ftl_superblock_shm — 빠른 재시작용 공유 메모리 영역 자료구조.
 * 디스크 레이아웃과 정확히 일치해야 하므로 모든 디스크 영속 구조체는 packed + size 정적 어서션으로 강제.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 슈퍼블록 라이프사이클의 가장 하위 레이어. 모든 슈퍼블록 관련 헤더(ftl_sb.h, ftl_sb_current.h,
 * ftl_sb_prev.h, ftl_sb_v3.h, ftl_sb_v5.h)가 이 헤더를 직접 또는 간접 포함한다.
 * 호출 체인: 슈퍼블록 빌드/검증/직렬화 코드 → 이 헤더의 타입/매크로 사용.
 * 실행 컨텍스트: 자료구조 정의 + 매크로만 노출 — 컴파일 시간에 의미 있음. 인스턴스는 SPDK reactor
 *   스레드에서 사용된다(디바이스 시작/정지/업그레이드 단계).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/stdinc.h(uint64_t/bool 등), utils/ftl_defs.h(KiB 매크로),
 *   utils/ftl_df.h(ftl_df_obj_id 타입).
 * 의존되는 모듈: ftl_sb_current.h, ftl_sb_prev.h, ftl_sb_v3.h, ftl_sb_v5.h, ftl_sb.h,
 *   upgrade/ftl_sb_upgrade.h, 그 외 슈퍼블록을 다루는 모든 .c 파일.
 * 데이터 흐름: 디스크 슈퍼블록 영역 ↔ 인메모리 (struct ftl_superblock_*) ↔ 이 헤더의 packed 구조체.
 * 공유 상태: ftl_superblock_shm은 빠른 재시작 모드에서 hugepage shared memory에 매핑되어 dev 내부
 *   여러 컴포넌트가 trim 진행/dirty 상태/GC 상태를 공유.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수는 없으며 자료구조와 매크로만 정의한다.
 *   - FTL_SUPERBLOCK_SIZE: 128 KiB — NV cache 슈퍼블록 영역 크기(미래 확장 여지 큼).
 *   - FTL_MAGIC(a,b,c,d): 4개의 16비트 값을 64비트 magic으로 패킹.
 *   - FTL_SUPERBLOCK_MAGIC: 현재 버전 magic (FTL_MAGIC(0x1410,0x1683,0x1920,0x1989)).
 *   - struct ftl_superblock_header: magic/crc/version 24바이트 — 모든 버전 첫머리.
 *   - struct ftl_superblock_gc_info: GC 진행 상황 영구 기록 (band id high prio, current band, transaction valid 등).
 *   - struct ftl_superblock_v3_md_region: v3 MD 레이아웃 chained list 노드.
 *   - struct ftl_superblock_v5_md_blob_hdr: v5 blob 헤더(blob_sz + df_id).
 *   - struct ftl_superblock_shm: 빠른 재시작용 SHM 영역(shm_ready/clean, trim 진행 상태, gc_info 사본).
 */

#ifndef FTL_SB_COMMON_H
#define FTL_SB_COMMON_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "spdk/stdinc.h"
/* [한국어] uint64_t/uint32_t/uint16_t/bool 등 표준 타입과 SPDK_STATIC_ASSERT(spdk/util.h 또는 stdinc 경유). */
#include "utils/ftl_defs.h"
/* [한국어] KiB 매크로 (1ULL << 10) 사용 — FTL_SUPERBLOCK_SIZE 계산에 필요. */
#include "utils/ftl_df.h"
/* [한국어] ftl_df_obj_id 타입 — chained list/blob 안의 영속 오프셋 ID 표현. */

/* Size of superblock on NV cache, make it bigger for future fields */
#define FTL_SUPERBLOCK_SIZE (128ULL * KiB)
/* [한국어] NV cache에 예약된 슈퍼블록 영역 크기 = 128 KiB.
 * 실제 struct ftl_superblock 크기는 훨씬 작지만, 미래 확장 필드와 blob 가변 영역이 들어갈
 * 여유 공간을 미리 잡아둔 것. SPDK_STATIC_ASSERT로 sizeof(ftl_superblock) <= FTL_SUPERBLOCK_SIZE
 * 가 강제된다(ftl_sb_current.h). */

#define FTL_MAGIC(a, b, c, d) \
    ((UINT64_C(a) << 48) | (UINT64_C(b) << 32) | (UINT64_C(c) << 16) | \
     UINT64_C(d))
/* [한국어] 4개의 16비트 정수를 한 64비트 magic 값으로 패킹하는 헬퍼 매크로.
 *  - UINT64_C: stdint.h 매크로로 16/32/64비트 정수 리터럴을 uint64_t로 안전 표현.
 *  - 16/32/48 비트 시프트: 각 인자가 64비트 값의 16비트 슬롯을 차지하도록.
 *  v2 이전에는 같은 의미로 8비트씩 패킹하는 별도 매크로(FTL_MAGIC_V2)가 있었다(v2 버그 호환성). */

/**
 * Magic number identifies FTL superblock
 */
#define FTL_SUPERBLOCK_MAGIC FTL_MAGIC(0x1410, 0x1683, 0x1920, 0x1989)
/* [한국어] FTL 슈퍼블록 식별 magic 상수. (1410=Cervantes 사망, 1683=Hieronymus Brunschwig 사망 등
 * Intel/Solidigm 측 임의 선택값). 디스크에서 64비트 magic 필드를 읽고 이 값과 비교해 슈퍼블록임을 확인. */

struct ftl_superblock_gc_info {
	/* High priority band; if there's no free bands after dirty shutdown, don't restart GC from same id, or phys_id -
	 * pick actual lowest validity band to avoid being stuck and try to write it to the open band.
	 */
	uint64_t band_id_high_prio;
	/* [한국어] dirty shutdown 후 free band가 없을 때 GC가 다시 시도해야 할 우선순위 밴드.
	 * 설정자: GC 코드(ftl_reloc.c)가 GC 시작 시점에 갱신하고, 슈퍼블록 영속화 시 디스크에 기록.
	 * 읽는 자: 다음 부팅의 시작 mngt가 GC 재개 시 사용.
	 * 값 범위: 유효 밴드 ID 또는 FTL_BAND_ID_INVALID(GC 진행 없음).
	 * 동기화: 슈퍼블록 갱신은 단일 mngt 스레드에서만 발생하므로 락 불필요. */

	/* Currently relocated band (note it's just id, not seq_id ie. its actual location on disk) */
	uint64_t current_band_id;
	/* [한국어] 현재 GC가 relocate하고 있는 밴드의 ID(디스크 상의 물리 위치).
	 * 설정자: ftl_reloc.c가 relocation 시작 시.
	 * 읽는 자: 다음 부팅에서 dirty shutdown 후 GC 재개에 사용.
	 * 값 범위: 유효 밴드 ID 또는 FTL_BAND_ID_INVALID.
	 * 동기화: 슈퍼블록 갱신은 단일 스레드에서만 일어남. */

	/* Bands are grouped together into larger reclaim units; this is the band id translated to those units */
	uint64_t band_phys_id;
	/* [한국어] 여러 밴드를 묶은 큰 reclaim unit ID — SSD 내부 erase 단위에 정렬됨.
	 * 설정자: GC 시작 시 reloc 코드가 갱신.
	 * 읽는 자: dirty shutdown 후 GC 재개 시 사용.
	 * 값 범위: 유효 phys ID 또는 FTL_BAND_PHYS_ID_INVALID.
	 * 동기화: 단일 스레드 갱신. */

	/* May be updating multiple fields at the same time, clearing/setting this marks the transaction */
	uint64_t is_valid;
	/* [한국어] gc_info 다중 필드 갱신을 atomic하게 관찰하기 위한 트랜잭션 valid 플래그.
	 * 설정자: 갱신 시작 시 0(write begin), 모든 필드 기록 후 1(write end). 슈퍼블록 영속화 코드가 관리.
	 * 읽는 자: dirty shutdown 후 부팅에서 0이면 갱신이 중단됐던 것이라 신뢰 불가.
	 * 값 범위: 0(invalid/in-progress) 또는 1(valid/committed).
	 * 동기화: 슈퍼블록 영속화 시퀀스가 ordered write로 보장 — 마지막에 is_valid=1 기록. */
} __attribute__((packed));
/* [한국어] packed — 컴파일러 패딩 없이 디스크 레이아웃과 1:1. */
SPDK_STATIC_ASSERT(sizeof(struct ftl_superblock_gc_info) == 32,
		   "ftl_superblock_gc_info incorrect size");
/* [한국어] 32바이트(4 * uint64_t) 정확히 — 디스크 호환성을 위해 컴파일 시 강제 검증.
 * 누군가 필드를 추가하면 빌드가 실패해 사고를 사전 차단한다. */

struct ftl_superblock_header {
	uint64_t magic;
	/* [한국어] FTL 슈퍼블록 식별자 (FTL_SUPERBLOCK_MAGIC). 모든 버전의 첫 8바이트.
	 * 설정자: 슈퍼블록 빌드 시. 읽는 자: 마운트 시 디스패처가 매직 비교로 슈퍼블록 인식.
	 * 값 범위: FTL_SUPERBLOCK_MAGIC 또는 FTL_SUPERBLOCK_MAGIC_V2. 동기화: read-only after 빌드. */

	uint64_t crc;
	/* [한국어] 슈퍼블록 콘텐츠 CRC32 — header 외 모든 필드의 무결성 검증용.
	 * 설정자: 슈퍼블록 영속화 직전, 모든 필드 채운 후 CRC 계산해 채움.
	 * 읽는 자: 마운트 시점에 다시 계산해서 비교, 불일치면 슈퍼블록 손상으로 판정.
	 * 값 범위: spdk_crc32 결과(상위 32비트는 0). 동기화: 단일 스레드 갱신. */

	uint64_t version;
	/* [한국어] 슈퍼블록 버전 번호 — FTL_SB_VERSION_0 ~ FTL_SB_VERSION_CURRENT.
	 * 설정자: 슈퍼블록 빌드 시 현재 버전 기록. 읽는 자: 업그레이드 디스패처가 적절한 v2/v3/v5
	 * 처리 경로 선택. 값 범위: 0..5. 동기화: read-only after 빌드, 업그레이드 시에만 갱신. */
} __attribute__((packed));
/* [한국어] packed — 모든 버전 슈퍼블록 첫머리에 정확히 24바이트로 위치. */
SPDK_STATIC_ASSERT(sizeof(struct ftl_superblock_header) == 24,
		   "ftl_superblock_header incorrect size");
/* [한국어] 24바이트(3 * uint64_t) 정확히 — 절대 변경되어서는 안 되는 안정 인터페이스. */

struct ftl_superblock_v3_md_region {
	uint32_t		type;
	/* [한국어] MD 영역 타입 enum (ftl_layout_region_type 캐스팅). 어떤 종류의 메타데이터인지 식별
	 * (예: SB, L2P, P2L_CKPT, NVC chunk md 등).
	 * 설정자: v3 슈퍼블록 빌드 시. 읽는 자: 마운트 시 ftl_layout 매핑 코드. 값 범위: enum 정의 범위. */

	uint32_t		version;
	/* [한국어] 이 MD 영역 포맷의 버전. 영역별로 독립 버전 관리됨(전체 슈퍼블록 버전과 별도).
	 * 설정자: 영역 빌드 시. 읽는 자: 영역 업그레이드 디스패처가 verify/upgrade 함수 선택. */

	uint64_t		blk_offs;
	/* [한국어] 디바이스 시작점 기준 영역의 블록 오프셋(4 KiB 단위).
	 * 설정자: 레이아웃 빌더가 디바이스 용량을 고려해 결정. 읽는 자: bdev I/O 호출 시 시작 LBA로 사용.
	 * 동기화: 빌드 후 read-only. */

	uint64_t		blk_sz;
	/* [한국어] 영역 크기(블록 단위). 설정자: 레이아웃 빌더. 읽는 자: bdev I/O 길이로 사용. */

	ftl_df_obj_id		df_next;
	/* [한국어] chained list 다음 노드의 df 오프셋. FTL_DF_OBJ_ID_INVALID이면 끝.
	 * 설정자: 빌드 시 다음 노드 위치 결정 후 기록. 읽는 자: 마운트 시 ftl_df_get_obj_ptr로 환원해 순회.
	 * 동기화: 슈퍼블록 갱신은 단일 스레드. */
} __attribute__((packed));
/* [한국어] packed — chained list 노드의 디스크 레이아웃 안정성. */
SPDK_STATIC_ASSERT(sizeof(struct ftl_superblock_v3_md_region) == 32,
		   "ftl_superblock_v3_md_region incorrect size");
/* [한국어] 32바이트 정확 — 32-bit type + 32-bit version + 64-bit*3. */

struct ftl_superblock_v5_md_blob_hdr {
	/* Blob size in bytes */
	uint16_t		blob_sz;
	/* [한국어] blob 영역 안의 한 객체 크기(바이트). 64KiB 미만이라 16비트로 충분.
	 * 설정자: blob 객체 직렬화 시 측정값 기록. 읽는 자: load 시 메모리 할당 크기 결정. */

	/* Reserved */
	uint16_t		reserved1;
	/* [한국어] 미래 확장 예약 16비트. 현재 0. 디스크 호환성을 위해 명시적으로 보존. */

	uint32_t		reserved2;
	/* [한국어] 미래 확장 예약 32비트. 현재 0. */

	/* DF pointer to the blob in a SB buf */
	ftl_df_obj_id	df_id;
	/* [한국어] 슈퍼블록 buf 시작 기준 blob 객체 시작 오프셋 — ftl_df_get_obj_ptr로 환원.
	 * 설정자: blob 직렬화 시. 읽는 자: load 시 ftl_df_get_obj_ptr(sb_buf, df_id)로 인메모리 포인터 복원.
	 * 값 범위: 유효 오프셋 또는 FTL_DF_OBJ_ID_INVALID. */
} __attribute__((packed));
/* [한국어] packed — 정확히 16바이트로 디스크에 매핑. */
SPDK_STATIC_ASSERT(sizeof(struct ftl_superblock_v5_md_blob_hdr) == 16,
		   "ftl_superblock_v5_md_blob_hdr incorrect size");
/* [한국어] 16바이트 정확 — 2*u16 + u32 + u64. */

struct ftl_superblock_shm {
	/* SHM initialization completed */
	bool				shm_ready;
	/* [한국어] SHM(공유 메모리) 영역 초기화 완료 플래그. 빠른 재시작 모드에서 다음 부팅이
	 * 이 플래그를 보고 SHM 데이터를 신뢰할지 판단.
	 * 설정자: 첫 부팅 시 init 코드가 true로. 읽는 자: 재시작 시점 빠른 경로 디스패처. */

	/* SHM status - fast restart */
	bool				shm_clean;
	/* [한국어] 직전 정상 종료 여부. true면 fast restart 가능, false면 dirty 복구 필요.
	 * 설정자: 정상 shutdown 시 true 기록, 시작 시 false로 클리어.
	 * 읽는 자: 재시작 디스패처가 빠른 경로/dirty 복구 경로 분기. */

	/* Used to continue trim after SHM recovery */
	struct {
		bool			in_progress;
		/* [한국어] trim(unmap) 작업이 진행 중이었는지 — 재시작 시 이어서 처리하기 위한 표식.
		 * 설정자: trim 시작 시 true, 완료 시 false. 읽는 자: 재시작 시 이 비트가 true면 아래 필드 사용. */
		uint64_t		start_lba;
		/* [한국어] 트림 시작 LBA. 설정자: trim 진입 시. 읽는 자: 재시작 시 이어서 트림. */
		uint64_t		num_blocks;
		/* [한국어] 트림 블록 수. 설정자: trim 진입 시. 읽는 자: 재시작 시 잔여 처리에 사용. */
		uint64_t		seq_id;
		/* [한국어] 트림이 속한 시퀀스 번호 — 다른 I/O와의 순서 비교에 사용. */
	} trim;
	/* [한국어] trim 진행 상태 익명 구조체 — SHM 회복 후 트림을 이어서 완료시키기 위한 컨텍스트. */

	struct ftl_superblock_gc_info	gc_info;
	/* [한국어] GC 진행 상황의 SHM 사본 — 디스크 슈퍼블록 갱신 전에도 메모리상에서 최신 상태를 유지.
	 * 설정자: GC 코드가 갱신 후 in-place 복사. 읽는 자: dirty 복구 시 마지막 GC 상태 참고. */
};
/* [한국어] 이 구조체는 packed가 아니다 — 디스크 영속이 아닌 hugepage SHM 영역에 매핑되며,
 * 같은 빌드의 프로세스끼리만 공유하므로 컴파일러 패딩이 있어도 무방. */

#endif /* FTL_SB_COMMON_H */
/* [한국어] 헤더 가드 종료. */
