/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] 일반 CRC-32 코어 루프 (테이블 초기화 + 갱신) (crc32.c)
 *
 * === 파일의 역할 ===
 * IEEE CRC-32(crc32_ieee.c)와 CRC-32C(crc32c.c)가 공통으로 쓰는 두 가지
 * 루틴을 제공한다:
 *   1) `crc32_table_init()` — 임의의 비트 반사 다항식으로 256-엔트리
 *      lookup table을 사전 계산.
 *   2) `crc32_update()` — table-driven byte-by-byte CRC 계산. ARM CRC32
 *      intrinsic이 활성화된 빌드에서는 하드웨어 가속 경로를 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 CRC 계산 스택의 가장 아래에 위치한다.
 *   상위: spdk_crc32_ieee_update / spdk_crc32c_update / spdk_crc32c_iov_update
 *         (소프트웨어 폴백 빌드에서)
 *   본 파일: 다항식 비종속 코어 루프 + ARM CRC32 분기.
 *   하위: ARM `__crc32b/__crc32d` 또는 lookup table 인덱싱.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `util_internal.h`(struct spdk_crc32_table 정의),
 *   `crc_internal.h`(SPDK_HAVE_ARM_CRC 매크로와 arm_acle.h 인클루드 분기),
 *   `spdk/crc32.h`(공개 prototype).
 * - 호출자: crc32_ieee.c(g_crc32_ieee_table 사용), crc32c.c의 SW 폴백 경로
 *   (g_crc32c_table 사용).
 * - 공유 상태: 호출자가 보유한 `struct spdk_crc32_table*` 한 개. 본 파일
 *   자체는 전역 상태를 갖지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - crc32_table_init(table, polynomial_reflect): 비트 반사 다항식으로
 *   256-엔트리 사전 계산 테이블 채움. 빌드 시 또는 constructor에서 1회 실행.
 * - crc32_update(table, buf, len, crc) [SW 폴백]: 매 바이트마다
 *   `crc = (crc >> 8) ^ table[(crc ^ byte) & 0xff]` 갱신.
 * - crc32_update(table, buf, len, crc) [ARM]: head/mid/tail 정렬 처리
 *   후 8B 단위 `__crc32d` 명령으로 가속.
 */

#include "util_internal.h"
/* [한국어] struct spdk_crc32_table, 다항식 매크로, prototype 정의. */
#include "crc_internal.h"
/* [한국어] SPDK_HAVE_ARM_CRC / SPDK_HAVE_SSE4_2 / SPDK_HAVE_ISAL 매크로와
 * 그에 따른 intrinsic 헤더(arm_acle.h, x86intrin.h, isa-l/crc.h) 인클루드. */
#include "spdk/crc32.h"
/* [한국어] 외부 공개 prototype. 본 파일은 helper 정의이므로 헤더 일관성을
 * 위해 포함한다. */

/*
 * [한국어]
 * crc32_table_init - 주어진 비트 반사 CRC-32 다항식으로 256-엔트리 lookup
 *                    table을 사전 계산.
 *
 * @table: 결과를 채울 spdk_crc32_table 구조체 포인터.
 * @polynomial_reflect: 비트 반사된 32비트 다항식. IEEE 802.3는 0xEDB88320,
 *                      Castagnoli(CRC-32C)는 0x82F63B78.
 * @return: void.
 *
 * 동기: 매 바이트마다 8회 시프트 루프를 도는 비테이블 알고리즘은 너무 느리다.
 * 한 번만 256개 엔트리를 만들어두면 이후 갱신이 1바이트당 lookup 1회로 끝난다.
 *
 * 실행 컨텍스트: 보통 GCC `__attribute__((constructor))` 안에서 호출되어
 * 프로세스 시작 시 1회 실행. 즉 단일 스레드 컨텍스트라 락 불필요.
 *
 * 호출 체인:
 *   <ELF loader> → crc32_ieee_init / crc32c_init → crc32_table_init.
 */
void
crc32_table_init(struct spdk_crc32_table *table, uint32_t polynomial_reflect)
{
	int i, j;
	/* [한국어] i: 0..255 바이트 값 인덱스. j: 한 바이트의 8비트 시프트 카운터. */
	uint32_t val;
	/* [한국어] 현재 인덱스 i의 바이트가 CRC 마지막 단계에서 어떻게 변환되는지를
	 * 8회 시프트로 시뮬레이션해 누적할 임시 변수. */

	for (i = 0; i < 256; i++) {
		/* [한국어] 가능한 바이트 값 256개 모두에 대해 미리 계산. */
		val = i;
		/* [한국어] 초기값을 byte로 두고 8비트 시프트 + (반사 다항식 XOR)을 진행. */
		for (j = 0; j < 8; j++) {
			/* [한국어] 한 바이트당 8비트를 LSB부터 처리한다(반사 알고리즘). */
			if (val & 1) {
				/* [한국어] 최하위 비트가 1이면, 한 칸 우측 시프트 후
				 * 다항식과 XOR — 이는 표준 CRC 알고리즘에서
				 * "shift-out 1" 경우의 잔류분 합치기. */
				val = (val >> 1) ^ polynomial_reflect;
			} else {
				/* [한국어] 최하위 비트가 0이면 단순 시프트만. */
				val = (val >> 1);
			}
		}
		table->table[i] = val;
		/* [한국어] 256개 결과를 사전 저장. 이후 crc32_update가 매 바이트마다
		 * `table[(crc ^ byte) & 0xff]`로 즉시 결과를 얻는다. */
	}
}

#ifdef SPDK_HAVE_ARM_CRC
/* [한국어] ARMv8 CRC32 intrinsic(`__crc32b`, `__crc32d`)을 사용하는 빌드.
 * AArch64에서 -march=...+crc 또는 자동 검출(CRC feature 비트)로 활성화. */

/*
 * [한국어]
 * crc32_update (ARM 가속판) - 8B 단위 하드웨어 CRC32 명령으로 갱신.
 *
 * @table: 다항식별 lookup table. ARM 경로에서는 사실상 사용되지 않지만
 *         API 호환을 위해 인자로 받아둔다.
 * @buf, @len, @crc: byte buffer/길이/누적 CRC.
 * @return: 갱신된 CRC.
 *
 * 동기: ARM CRC32 명령은 8B 정렬된 64비트 워드 입력에서 가장 빠르다.
 * head(non-aligned 앞부분)와 tail(끝 잔여) 바이트는 byte 단위 명령(__crc32b)
 * 으로 처리하고, 가운데는 8B 단위 __crc32d로 한꺼번에 처리한다.
 *
 * 호출 체인:
 *   spdk_crc32_ieee_update / SW 폴백 경로 → 본 함수 → __crc32b/__crc32d.
 */
uint32_t
crc32_update(const struct spdk_crc32_table *table, const void *buf, size_t len, uint32_t crc)
{
	size_t count_pre, count_post, count_mid;
	/* [한국어] 정렬 단계별 처리할 바이트 수: pre=8B 정렬 전, post=끝 잔여,
	 * mid=정렬된 8B 워드 수. */
	const uint64_t *dword_buf;
	/* [한국어] 8B 정렬 후 64비트 워드 단위 진행을 위한 포인터 별칭. */

	/* process the head and tail bytes separately to make the buf address
	 * passed to crc32_d is 8 byte aligned. This can avoid unaligned loads.
	 */
	/* [한국어] 8B 정렬 정책: AArch64에서 unaligned 64비트 로드는 대체로
	 * 동작하지만 마이크로아키텍처에 따라 페널티가 있을 수 있어 명시 정렬. */
	count_pre = ((uint64_t)buf & 7) == 0 ? 0 : 8 - ((uint64_t)buf & 7);
	/* [한국어] buf의 하위 3비트(0~7)가 0이 아니면, 정렬까지 남은 바이트 수
	 * 만큼 head로 분리. 이미 정렬되어 있으면 0. */
	count_post = (uint64_t)(buf + len) & 7;
	/* [한국어] 끝 주소의 하위 3비트 = 8B 정렬에서 남는 잔여 바이트 수. */
	count_mid = (len - count_pre - count_post) / 8;
	/* [한국어] 가운데 8B 워드의 개수. (총 len - head - tail) / 8. */

	while (count_pre--) {
		/* [한국어] 정렬 전 head 바이트들을 1B 단위 CRC32 명령으로 처리. */
		crc = __crc32b(crc, *(const uint8_t *)buf);
		/* [한국어] __crc32b: ARMv8 CRC32B 명령. (crc, byte) → 새 crc. */
		buf++;
	}

	dword_buf = (const uint64_t *)buf;
	/* [한국어] 이제 buf가 8B 정렬 → 64비트 포인터로 캐스팅해 안전하게 로드. */
	while (count_mid--) {
		crc = __crc32d(crc, *dword_buf);
		/* [한국어] __crc32d: 64비트 CRC32 명령. 한 명령으로 8B 처리 → 핫패스
		 * 처리량 극대화. */
		dword_buf++;
	}

	buf = dword_buf;
	/* [한국어] 가운데 처리 후 갱신된 위치를 다시 byte 포인터로 회수해 tail 처리. */
	while (count_post--) {
		crc = __crc32b(crc, *(const uint8_t *)buf);
		/* [한국어] tail 잔여 바이트도 1B 단위로 마저 처리. */
		buf++;
	}

	return crc;
	/* [한국어] head+mid+tail 모두 누적된 최종 CRC 반환. */
}

#else
/* [한국어] ARM CRC 명령이 없는 환경(또는 isa-l/SSE 가속을 끈 SW 폴백 빌드)
 * 에서 사용하는 일반 lookup-table 알고리즘. crc32_ieee.c와 crc32c.c가 공유. */

/*
 * [한국어]
 * crc32_update (SW 폴백판) - 256-엔트리 lookup table을 이용한 byte-by-byte
 *                            CRC-32 갱신.
 *
 * @table: 다항식별 사전 계산 테이블 (crc32_table_init이 채움).
 * @buf, @len: 입력 버퍼와 길이.
 * @crc: 누적 CRC.
 * @return: 갱신 결과.
 *
 * 매 바이트마다 (crc ^ byte)의 하위 8비트를 인덱스로 table을 lookup하여
 * 다음 8비트를 시프트한 결과와 XOR한다. 이 알고리즘은 표준 CRC 시프트
 * 회로의 8비트 압축 형태이다.
 *
 * 호출 체인:
 *   spdk_crc32_ieee_update / spdk_crc32c_update(SW) → 본 함수.
 */
uint32_t
crc32_update(const struct spdk_crc32_table *table, const void *buf, size_t len, uint32_t crc)
{
	const uint8_t *buf_u8 = buf;
	/* [한국어] void* 산술 회피를 위해 uint8_t 별칭 포인터로 변환. */
	size_t i;
	/* [한국어] 단순 인덱스 변수. */

	for (i = 0; i < len; i++) {
		/* [한국어] 입력 바이트별로 1회 lookup + 1회 XOR + 1회 시프트.
		 * (crc >> 8): 상위 24비트 보존 / 하위 8비트 폐기.
		 * (crc ^ buf_u8[i]) & 0xff: 다음 lookup 인덱스 = 새 입력 바이트와
		 *   현재 CRC 하위 바이트를 결합한 8비트 값.
		 * table[...]: 256개 사전 계산값 중 해당 인덱스 결과를 가져와 XOR. */
		crc = (crc >> 8) ^ table->table[(crc ^ buf_u8[i]) & 0xff];
	}

	return crc;
	/* [한국어] 모든 입력 바이트를 누적한 최종 CRC. */
}

#endif
