/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] IEEE 802.3 CRC-32 (다항식 0xEDB88320) 계산 진입점 (crc32_ieee.c)
 *
 * === 파일의 역할 ===
 * IEEE 802.3 표준 CRC-32(흔히 "Ethernet/zlib/PNG CRC-32"라 불리는 다항식)
 * 을 SPDK가 사용할 때의 진입점을 제공한다. 본 파일은 256-엔트리 lookup
 * table을 프로세스 시작 시 한 번 생성(`__attribute__((constructor))`)하고,
 * 그 테이블을 공유하는 `spdk_crc32_ieee_update()` API를 제공한다. 실제
 * byte-by-byte 갱신 로직은 `crc32_update()`(crc32.c)에 위임한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IEEE CRC-32는 SPDK 내부에서 다음 위치들에 쓰인다:
 *   - blobstore metadata 페이지 무결성 (crc32_ieee_update on blob page)
 *   - reduce 압축 모듈 등 일부 영구 메타데이터 검증
 * (NOTE: NVMe DIF guard·NVMe-oF DDGST/HDGST는 CRC-32C(Castagnoli)이며
 * crc32c.c에서 처리. 이 IEEE 변형과 혼동 금지.)
 * 호출 흐름:
 *   blob 페이지 write/read → blob 모듈에서 spdk_crc32_ieee_update() →
 *   공유 g_crc32_ieee_table 사용 → table-driven CRC 계산.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `util_internal.h`(SPDK_CRC32_POLYNOMIAL_REFLECT 0xEDB88320 정의),
 *   `spdk/crc32.h`(공개 prototype). 실제 갱신 함수 `crc32_update`는
 *   crc32.c에 정의됨 (ARM CRC32 instrinsic 분기 포함).
 * - 호출자: lib/blob/* 등에서 spdk_crc32_ieee_update를 호출.
 * - 공유 상태: `g_crc32_ieee_table` — constructor에서 초기화 후 read-only.
 *   초기화는 단일 스레드에서 한 번만 일어나고 이후 모든 reactor가 락 없이
 *   읽기만 하므로 race가 없다 (lockless table sharing).
 *
 * === 주요 함수/구조체 요약 ===
 * - crc32_ieee_init(): 프로세스 시작 시 자동 호출되는 GCC constructor —
 *   IEEE 다항식 0xEDB88320으로 256-엔트리 lookup table 채움.
 * - spdk_crc32_ieee_update(buf, len, crc): 누적 CRC 갱신. 초깃값과 최종
 *   complement는 호출자가 책임진다(예: blob 코드).
 * - g_crc32_ieee_table: 파일 정적, 256×uint32_t 테이블, 모든 호출이 공유.
 */

#include "util_internal.h"
/* [한국어] SPDK_CRC32_POLYNOMIAL_REFLECT(0xedb88320) 매크로와
 * struct spdk_crc32_table, crc32_table_init/crc32_update prototype 정의. */
#include "spdk/crc32.h"
/* [한국어] 공개 API spdk_crc32_ieee_update() prototype. blob/reduce 등이
 * 이 헤더만 보고 호출한다. */

static struct spdk_crc32_table g_crc32_ieee_table;
/* [한국어] IEEE CRC-32용 256-엔트리 lookup table.
 * - 설정자: 아래 crc32_ieee_init()이 프로세스 시작 시 한 번 채움.
 * - 읽는 자: spdk_crc32_ieee_update() 호출 시 매 바이트마다 인덱싱.
 * - 동기화: 초기화 이후 read-only이므로 락/배리어 불필요. 모든 SPDK
 *   스레드가 안전하게 공유 (lockless). 약 1KB(256×4B). */

/*
 * [한국어]
 * crc32_ieee_init - 프로세스 시작 시 IEEE CRC-32 lookup table을 자동 생성.
 *
 * @return: void.
 *
 * `__attribute__((constructor))` 덕분에 main() 진입 전, 동적 로더 단계에서
 * 자동 호출된다. 이 시점은 프로세스가 단일 스레드 상태이므로 g_crc32_ieee_table
 * 초기화에 동기화가 필요 없다. 한 번 채운 뒤로는 read-only로 사용된다.
 *
 * 호출 체인:
 *   <ELF dynamic loader> → crc32_ieee_init() → crc32_table_init() (crc32.c)
 *
 * 실행 컨텍스트: 메인 스레드 단독, 다른 SPDK 스레드/poller 생성 이전.
 */
__attribute__((constructor)) static void
crc32_ieee_init(void)
{
	crc32_table_init(&g_crc32_ieee_table, SPDK_CRC32_POLYNOMIAL_REFLECT);
	/* [한국어] util_internal.h 정의 SPDK_CRC32_POLYNOMIAL_REFLECT(0xedb88320,
	 * 비트 반사 IEEE 802.3 다항식 x^32+x^26+x^23+...+x+1)로 256개 엔트리를
	 * 사전 계산. 이후 갱신 루프는 한 바이트당 1회 인덱스 룩업+XOR로 처리됨. */
}

/*
 * [한국어]
 * spdk_crc32_ieee_update - IEEE CRC-32을 누적 갱신.
 *
 * @buf: 입력 데이터의 시작 주소.
 * @len: buf의 바이트 길이.
 * @crc: 이전까지의 누적 CRC. 호출자가 첫 호출에서는 보통 ~0u(0xFFFFFFFF)로
 *       시작하고, 마지막에 다시 보수(complement)를 취하는 컨벤션을 사용한다.
 *       이 함수 자체는 초깃값 보정/최종 보수를 자동으로 하지 않는다.
 * @return: buf 처리 후의 누적 CRC.
 *
 * blob 페이지 같은 영속 데이터의 무결성 검증용으로 호출된다. 실제 계산은
 * 공통 헬퍼 crc32_update()에 위임 — 빌드 환경에 따라 ARM CRC32 intrinsic이
 * 활성화되었으면 하드웨어 가속 경로가 쓰일 수 있다(crc32.c 참조).
 *
 * 실행 컨텍스트: 임의의 SPDK 스레드. 테이블이 read-only이므로 동시 호출 안전.
 * 호출 체인:
 *   <blob/reduce 등 호출자> → spdk_crc32_ieee_update() → crc32_update()
 */
uint32_t
spdk_crc32_ieee_update(const void *buf, size_t len, uint32_t crc)
{
	return crc32_update(&g_crc32_ieee_table, buf, len, crc);
	/* [한국어] 공유 lookup table을 넘겨 실제 byte-by-byte CRC 계산을 수행하는
	 * 일반 헬퍼로 위임. 이 위임 구조 덕분에 IEEE/CRC32C 두 다항식이 동일한
	 * 코어 루프를 공유한다 (DRY 원칙). */
}
