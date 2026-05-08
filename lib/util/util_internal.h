/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK util 모듈 내부용 공통 정의 (util_internal.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 lib/util/ 하위 .c 파일끼리 공유되는 "내부 전용" 정의를 담는다.
 * 외부 사용자(다른 라이브러리/애플리케이션)에게 노출되어서는 안 되는 CRC-32
 * 다항식 상수, 룩업 테이블 구조체(spdk_crc32_table), 그리고 그 테이블을 만들고
 * 부분 CRC를 갱신하는 두 함수의 프로토타입을 선언한다. 공개 API(spdk/crc32.h)에서
 * 호출하는 구현 본체가 이 내부 함수를 사용한다. SPDK의 일반 사용자는 이 파일을
 * 직접 인클루드하지 않는다(파일명에 _internal 접미사가 붙은 이유).
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK util 라이브러리 → CRC-32 표 기반 구현 보조 헤더.
 * 호출 체인:
 *   spdk_crc32_ieee/spdk_crc32c (공개 API, lib/util/crc32_ieee.c, crc32c.c)
 *     → crc32_table_init() / crc32_update() (이 헤더가 선언, lib/util/crc32.c가 정의)
 * 실행 컨텍스트: 호스트 유저스페이스. SPDK reactor 스레드 또는 일반 스레드에서
 * 동기적으로 호출되며, 락이나 SPDK thread 메시지를 사용하지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h(표준 헤더 모음).
 * - 의존하는 자: lib/util/crc32.c(테이블 빌더와 누적 함수의 정의), lib/util/crc32_ieee.c
 *   (IEEE 802.3 CRC-32 표 사용), lib/util/crc32c.c(Castagnoli 다항식 표 사용).
 * - 데이터 흐름: 다항식 상수(SPDK_CRC32*_POLYNOMIAL_REFLECT) → crc32_table_init이
 *   spdk_crc32_table을 채움 → crc32_update가 buf 데이터를 바이트 단위로 표 룩업하며
 *   crc 누적값을 갱신 → 호출자에게 갱신된 32비트 CRC 값 반환.
 *
 * === 주요 함수/구조체 요약 ===
 * - SPDK_CRC32_POLYNOMIAL_REFLECT  : IEEE 802.3 CRC-32(에서넷) 다항식의 비트 반전형.
 * - SPDK_CRC32C_POLYNOMIAL_REFLECT : Castagnoli CRC-32C(SCSI/iSCSI) 다항식의 비트 반전형.
 * - struct spdk_crc32_table        : 256 엔트리 표 룩업 자료구조 (slice-by-1 방식).
 * - crc32_table_init()             : 다항식으로부터 256 엔트리 표를 사전 계산.
 * - crc32_update()                 : 표를 사용하여 buf 길이만큼 CRC를 누적 갱신.
 */

#ifndef SPDK_UTIL_INTERNAL_H
/* [한국어] 헤더 가드 시작 - 다중 인클루드 시 재정의 오류 방지. */
#define SPDK_UTIL_INTERNAL_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 헤더 묶음(stdint, stddef, string 등)을 포함.
 * uint32_t 등 고정폭 정수 타입과 size_t를 사용하기 위해 필요. */

/**
 * IEEE CRC-32 polynomial (bit reflected)
 */
#define SPDK_CRC32_POLYNOMIAL_REFLECT 0xedb88320UL
/* [한국어] IEEE 802.3(에서넷, ZIP, PNG 등에서 사용하는) CRC-32 다항식
 *  x^32 + x^26 + ... + x + 1 의 비트 반전(bit-reflected) 표현.
 * 비트 반전형을 쓰는 이유: 입력 바이트를 최하위 비트(LSB)부터 처리하는 효율적인
 * 표 기반 알고리즘에서 시프트와 XOR 만으로 다항식 나눗셈을 수행할 수 있도록 하기 위함.
 * UL 접미사: unsigned long, 32비트 환경에서 안전한 32비트 정수 리터럴 보장. */

/**
 * CRC-32C (Castagnoli) polynomial (bit reflected)
 */
#define SPDK_CRC32C_POLYNOMIAL_REFLECT 0x82f63b78UL
/* [한국어] Castagnoli CRC-32C 다항식의 비트 반전 표현.
 * SCSI(T10), iSCSI 데이터 다이제스트, NVMe E2E 보호의 일부 구성, ext4 메타데이터,
 * SSE4.2 _mm_crc32_* 인스트럭션 등 광범위하게 쓰인다. IEEE 다항식보다 검출률이
 * 약간 더 높다고 알려져 있다. UL 접미사 이유는 위와 동일. */

struct spdk_crc32_table {
	/* [한국어] CRC-32 슬라이스-바이-1 알고리즘용 룩업 테이블 컨테이너.
	 * 다항식이 결정되면 256개 바이트 값에 대해 미리 CRC 갱신 결과를 계산해 둔다. */
	uint32_t table[256];
	/* [한국어] 256 엔트리 룩업 테이블. table[b]는 "현재 잔여 CRC의 하위 1바이트가
	 * b일 때 한 바이트를 처리한 후의 변화량"을 미리 계산한 값.
	 * 설정자: crc32_table_init() (정확히 한 번 초기화하는 것이 일반적).
	 * 읽는 자: crc32_update()가 데이터 바이트마다 table[(crc ^ data) & 0xFF]를 룩업.
	 * 값 범위: 32비트 부호 없는 정수 전 범위. 다항식에 따라 결정.
	 * 동기화: 초기화 후에는 read-only이므로 멀티스레드에서 락 없이 안전하게 공유. */
};

/**
 * Initialize a CRC32 lookup table for a given polynomial.
 *
 * \param table Table to fill with precalculated CRC-32 data.
 * \param polynomial_reflect Bit-reflected CRC-32 polynomial.
 */
/*
 * [한국어]
 * crc32_table_init - 주어진 다항식으로 CRC-32 슬라이스-바이-1 룩업 테이블을 채운다.
 *
 * @table:               결과를 받을 256엔트리 테이블 컨테이너 포인터(호출자 소유).
 * @polynomial_reflect:  IEEE 또는 Castagnoli 다항식의 비트 반전 표현.
 * @return:              없음(void). 실패 경로는 없으며 항상 성공.
 *
 * 호출 체인:
 *   crc32_ieee/c 모듈 초기화(전역 한 번 또는 lazy init) → [crc32_table_init]
 * 실행 컨텍스트: 어떤 스레드든 호출 가능하나, 표 자체는 한 번만 채워지면 read-only로
 * 모든 스레드가 공유한다. 동기화는 호출자(보통 한 번만 호출하는 패턴)에서 보장.
 */
void crc32_table_init(struct spdk_crc32_table *table,
		      uint32_t polynomial_reflect);
/* [한국어] 위 함수 프로토타입. 본체 정의는 lib/util/crc32.c. */


/**
 * Calculate a partial CRC-32 checksum.
 *
 * \param table CRC-32 table initialized with crc32_table_init().
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \param crc Previous CRC-32 value.
 * \return Updated CRC-32 value.
 */
/*
 * [한국어]
 * crc32_update - 표 룩업으로 주어진 버퍼에 대한 CRC를 부분 누적 갱신한다.
 *
 * @table:  사전에 crc32_table_init()로 초기화된 테이블(NULL 불가).
 * @buf:    체크섬 대상 데이터 버퍼(읽기 전용).
 * @len:    buf의 길이(바이트). 0도 허용되며 그 경우 crc를 그대로 반환.
 * @crc:    이전까지 누적된 CRC 값(연속 호출 가능, 첫 호출은 보통 0xFFFFFFFF).
 * @return: buf를 반영한 갱신된 CRC-32 값.
 *
 * 호출 체인:
 *   spdk_crc32_ieee/spdk_crc32c (공개 API) → [crc32_update] → 산술/룩업 연산만 수행
 * 실행 컨텍스트: 동기, 순수 계산. 어떤 SPDK thread/일반 스레드에서도 호출 가능.
 */
uint32_t crc32_update(const struct spdk_crc32_table *table,
		      const void *buf, size_t len,
		      uint32_t crc);
/* [한국어] 위 함수 프로토타입. 본체 정의는 lib/util/crc32.c.
 * 누적식 호출이 가능하도록 이전 crc를 입력으로 받는 설계이다. */

#endif /* SPDK_UTIL_INTERNAL_H */
/* [한국어] 헤더 가드 끝. */
