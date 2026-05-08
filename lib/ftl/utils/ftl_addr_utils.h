/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 주소(ftl_addr)/LBA의 packed/unpacked 변환 헬퍼 (ftl_addr_utils.h)
 *
 * === 파일의 역할 ===
 * FTL의 L2P(Logical-to-Physical) 매핑 테이블이나 P2L 매핑 영역에서 한 슬롯이
 * 32비트(packed) 또는 64비트(unpacked) 두 가지 폭으로 저장될 수 있다.
 * 디바이스 전체 용량이 4KiB 블록 기준으로 32비트 LBA 범위(약 16TiB) 안에 들어가면
 * packed(32b) 모드로 매핑 테이블 메모리/저장 공간을 절반으로 줄일 수 있고, 그렇지 않으면
 * unpacked(64b) 모드로 동작한다. 이 헤더는 그런 분기를 ftl_addr_packed(dev) 한 줄로 추상화하여
 * 호출자가 buf+offset의 슬롯을 폭에 무관하게 read/write 할 수 있도록 4개의 인라인 헬퍼를 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * L2P/P2L 매핑 테이블의 모든 슬롯 접근은 이 헤더를 거친다.
 * 호출 체인: ftl_l2p.c / ftl_band.c / ftl_p2l_ckpt.c 등 → ftl_addr_load/store, ftl_lba_load/store →
 *   ftl_addr_packed(dev) (ftl_core.h 정의) → 폭에 따라 b32 또는 b64 인덱스 접근.
 * 실행 컨텍스트: 호스트 유저스페이스, FTL의 L2P 처리 스레드. 모두 인라인 함수이므로
 *   호출 비용은 거의 0이며 hot path에서도 안전하다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: ftl_core.h (struct spdk_ftl_dev 정의, ftl_addr 타입,
 *   FTL_ADDR_INVALID/FTL_LBA_INVALID 상수, ftl_addr_packed() 인라인).
 * 의존되는 모듈: ftl_l2p.c, ftl_l2p_cache.c, ftl_band.c (P2L 매핑 직렬화),
 *   ftl_p2l_ckpt.c (체크포인트 read/write), ftl_reloc.c.
 * 데이터 흐름: NV cache 또는 base bdev에서 4KiB 블록을 읽어와 buffer로 두면,
 *   각 슬롯이 packed 모드에서는 4바이트씩, unpacked에서는 8바이트씩 차지하므로
 *   호출자는 "슬롯 인덱스" offset만 알고 폭은 모르더라도 정확한 64비트 ftl_addr/LBA 값을 얻는다.
 * sentinel 처리: packed 모드에서 32비트 슬롯이 0xFFFFFFFF를 담고 있으면 64비트 sentinel
 *   FTL_ADDR_INVALID(=(uint64_t)-1)로 정상 확장해 호출자가 (uint64_t)-1 == addr 비교를 단순하게 할 수 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * 모두 인라인 함수이며 새 구조체는 정의하지 않는다.
 *   - ftl_addr_load(dev, buf, off): buf의 off번째 슬롯에서 ftl_addr 한 개 로드 (sentinel 보정 포함).
 *   - ftl_addr_store(dev, buf, off, addr): ftl_addr 값을 buf의 off번째 슬롯에 저장.
 *   - ftl_lba_load(dev, buf, off): buf에서 off번째 LBA 로드 (sentinel 보정 포함).
 *   - ftl_lba_store(dev, buf, off, lba): LBA 값을 buf의 off번째 슬롯에 저장.
 */

#ifndef FTL_ADDR_UTILS_H
#define FTL_ADDR_UTILS_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "ftl_core.h"
/* [한국어] struct spdk_ftl_dev, ftl_addr typedef, FTL_ADDR_INVALID, FTL_LBA_INVALID,
 * ftl_addr_packed(dev) 인라인 정의를 가져온다. 이 헤더의 모든 함수가 그 타입/매크로에 의존. */

/*
 * [한국어]
 * ftl_addr_load - L2P/P2L 매핑 슬롯에서 ftl_addr 한 개를 폭 무관하게 로드
 *
 * @param dev: FTL 디바이스 핸들 — packed/unpacked 모드 결정에 사용.
 * @param buffer: 매핑 영역의 시작 포인터 (4KiB 블록을 NV cache 등에서 읽어온 메모리).
 * @param offset: 슬롯 인덱스(요소 단위, 바이트 아님). 캐스팅된 b32/b64 배열에 그대로 인덱스됨.
 * @return: 64비트 ftl_addr 값. packed 모드에서 0xFFFFFFFF 슬롯은 64비트 FTL_ADDR_INVALID로 확장.
 *
 * 동기/배경: FTL은 디바이스 용량에 따라 매핑 슬롯을 32b 또는 64b로 압축한다.
 *   호출자가 매번 if/else를 쓰지 않도록 통합 인터페이스가 필요.
 * 동작: ftl_addr_packed(dev)가 참이면 buffer를 uint32_t* 로 보고 인덱스 접근, sentinel 보정.
 *   거짓이면 uint64_t*로 보고 그대로 반환.
 * 실행 컨텍스트: L2P 처리 스레드(SPDK reactor) — hot path지만 인라인이라 비용 낮음.
 * caller: ftl_l2p.c, ftl_l2p_cache.c, ftl_band.c (P2L 매핑 디시리얼라이즈) 등.
 * callee: ftl_addr_packed (인라인) 외엔 없음.
 * 에러 경로: 없음 (입력 신뢰). 잘못된 offset은 호출자 책임.
 *
 * 호출 체인:
 *   L2P 캐시 페이지 디코드 / P2L 체크포인트 적용 → [이 함수]
 */
static inline ftl_addr
ftl_addr_load(struct spdk_ftl_dev *dev, void *buffer, uint64_t offset)
{
	if (ftl_addr_packed(dev)) {
		/* [한국어] packed 모드: 디바이스 용량이 32b 표현 가능 범위 내라서 슬롯이 4바이트씩 저장됨. */
		uint32_t *b32 = buffer;
		/* [한국어] 호출자가 넘긴 buffer를 32비트 배열로 재해석. 캐스팅 비용은 0. */
		ftl_addr addr = b32[offset];
		/* [한국어] offset번째 슬롯을 읽어 64비트 ftl_addr로 zero-extend.
		 * 정상 주소라면 그대로 64b 표현으로 의미가 보존된다. */

		if (addr == (uint32_t)FTL_ADDR_INVALID) {
			/* [한국어] 32b sentinel(0xFFFFFFFF)인지 검사. (uint32_t) 캐스팅으로
			 * 64b sentinel(0xFFFFFFFFFFFFFFFF)의 하위 32b만 비교. */
			return FTL_ADDR_INVALID;
			/* [한국어] sentinel은 64비트 폭으로 정상화하여 반환 — 호출자가 64b 비교만 쓸 수 있게 함. */
		} else {
			return addr;
			/* [한국어] 일반 주소는 그대로 반환 (zero-extend된 64b 값). */
		}
	} else {
		/* [한국어] unpacked 모드: 슬롯이 8바이트씩. */
		uint64_t *b64 = buffer;
		/* [한국어] buffer를 64b 배열로 재해석. */
		return b64[offset];
		/* [한국어] 64b 슬롯이 이미 ftl_addr 그대로이므로 sentinel 보정 불필요. */
	}
}

/*
 * [한국어]
 * ftl_addr_store - L2P/P2L 매핑 슬롯에 ftl_addr 값을 폭 무관하게 저장
 *
 * @param dev: FTL 디바이스 핸들 — packed/unpacked 결정.
 * @param buffer: 매핑 영역 시작 포인터.
 * @param offset: 슬롯 인덱스.
 * @param addr: 저장할 ftl_addr (64b). packed 모드에서는 하위 32b만 저장됨 — 호출자가 범위 보장 책임.
 *
 * 동기/배경: load의 대칭 함수. L2P 갱신, P2L 체크포인트 작성 시 사용.
 * 동작: 폭에 따라 b32/b64 인덱스에 대입. packed에서 64b → 32b 암시적 절단이 일어나며,
 *   주소가 32b 범위에 들어간다는 사전 보장이 필요하다(디바이스가 packed 모드라면 자연히 보장됨).
 * 실행 컨텍스트: L2P 갱신 스레드, FTL 메타데이터 작성 경로.
 *
 * 호출 체인:
 *   L2P 갱신 / P2L 체크포인트 빌드 → [이 함수]
 */
static inline void
ftl_addr_store(struct spdk_ftl_dev *dev, void *buffer, uint64_t offset, ftl_addr addr)
{
	if (ftl_addr_packed(dev)) {
		/* [한국어] packed 모드 — 32b 슬롯에 저장. */
		uint32_t *b32 = buffer;
		/* [한국어] buffer를 32b 배열로 재해석. */
		b32[offset] = addr;
		/* [한국어] 64b → 32b 암시적 절단. packed 디바이스는 주소 범위가
		 * 32b를 넘지 않도록 디자인되므로 손실 없음. sentinel(uint64_t)-1도 (uint32_t)0xFFFFFFFF로 보존. */
	} else {
		/* [한국어] unpacked 모드 — 64b 슬롯에 그대로 저장. */
		uint64_t *b64 = buffer;
		/* [한국어] 64b 배열로 재해석. */
		b64[offset] = addr;
		/* [한국어] 64b 그대로 저장 — 어떤 변환도 없음. */
	}
}

/*
 * [한국어]
 * ftl_lba_load - 매핑 슬롯에서 LBA(논리 블록 주소)를 폭 무관하게 로드
 *
 * @param dev: FTL 디바이스 핸들.
 * @param buffer: 슬롯 배열 시작 포인터.
 * @param offset: 슬롯 인덱스.
 * @return: 64b LBA. packed 모드에서 0xFFFFFFFF는 FTL_LBA_INVALID(64b)로 확장.
 *
 * 동기/배경: ftl_addr_load와 동일한 패턴이지만, P2L 영역(밴드의 "이 PPA가 어떤 LBA였나"
 *   역매핑)을 다룰 때 LBA(uint64_t) 폭으로 다룬다. 압축 정책은 ftl_addr와 동일.
 * 동작: packed면 b32, unpacked면 b64 슬롯 접근. sentinel 보정 포함.
 * 실행 컨텍스트: P2L 적용 스레드 — dirty shutdown 복구나 GC relocation 시 사용.
 *
 * 호출 체인:
 *   ftl_band P2L 디코드 / 복구 경로 → [이 함수]
 */
static inline uint64_t
ftl_lba_load(struct spdk_ftl_dev *dev, void *buffer, uint64_t offset)
{
	if (ftl_addr_packed(dev)) {
		/* [한국어] packed: LBA도 32b 슬롯에 저장됨. */
		uint32_t *b32 = buffer;
		/* [한국어] 32b 배열 재해석. */
		uint32_t lba = b32[offset];
		/* [한국어] 32b LBA 로드. */

		if (lba == (uint32_t)FTL_LBA_INVALID) {
			/* [한국어] 32b sentinel(0xFFFFFFFF) 체크. */
			return FTL_LBA_INVALID;
			/* [한국어] 64b sentinel로 확장 반환. */
		} else {
			return lba;
			/* [한국어] 일반 LBA는 zero-extend되어 64b로 반환. */
		}
	} else {
		/* [한국어] unpacked: 64b 슬롯 그대로. */
		uint64_t *b64 = buffer;
		/* [한국어] 64b 배열 재해석. */
		return b64[offset];
		/* [한국어] 64b LBA 그대로 반환. */
	}
}

/*
 * [한국어]
 * ftl_lba_store - LBA를 매핑 슬롯에 폭 무관하게 저장
 *
 * @param dev: FTL 디바이스 핸들.
 * @param buffer: 슬롯 배열 시작 포인터.
 * @param offset: 슬롯 인덱스.
 * @param lba: 저장할 LBA (64b). packed 모드에서는 하위 32b만 저장됨.
 *
 * 동기/배경: ftl_lba_load의 짝. P2L 매핑 작성 시 사용.
 *
 * 호출 체인:
 *   ftl_band P2L 작성 / 체크포인트 → [이 함수]
 */
static inline void
ftl_lba_store(struct spdk_ftl_dev *dev, void *buffer, uint64_t offset, uint64_t lba)
{
	if (ftl_addr_packed(dev)) {
		/* [한국어] packed: 32b 슬롯에 저장. */
		uint32_t *b32 = buffer;
		/* [한국어] 32b 배열 재해석. */
		b32[offset] = lba;
		/* [한국어] 64b → 32b 절단 저장. sentinel도 (uint32_t)0xFFFFFFFF로 보존. */
	} else {
		/* [한국어] unpacked: 64b 슬롯에 저장. */
		uint64_t *b64 = buffer;
		/* [한국어] 64b 배열 재해석. */
		b64[offset] = lba;
		/* [한국어] 64b 그대로 저장. */
	}
}

#endif /* FTL_ADDR_UTILS_H */
/* [한국어] 헤더 가드 종료. */
