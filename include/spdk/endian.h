/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Endian conversion functions
 */

/*
 * [한국어 설명] 엔디언 변환 유틸리티 (endian.h)
 *
 * === 파일의 역할 ===
 * 호스트 메모리와 네트워크/디스크 프로토콜 간 바이트 오더 변환을 위한 안전한
 * byte-wise 변환 함수들을 제공한다. 제공하는 함수 쌍은 다음과 같다:
 *   - from_be16 / from_be32 / from_be64 — 빅엔디언 바이트 스트림을 호스트 정수로
 *   - to_be16   / to_be32   / to_be64   — 호스트 정수를 빅엔디언 바이트 스트림으로
 *   - from_le16 / from_le32 / from_le64 — 리틀엔디언 바이트 스트림 → 호스트 정수
 *   - to_le16   / to_le32   / to_le64   — 호스트 정수 → 리틀엔디언 바이트 스트림
 *
 * 구현 원칙: **byte-by-byte 접근을 통해 정렬(alignment) 요구와 호스트
 * 엔디언에 관계없이 항상 동일하게 동작**한다. 즉 ptr이 unaligned 이어도
 * bus error/SIGBUS가 나지 않고, 호스트 머신이 빅/리틀 어느 쪽이든
 * 결과가 동일하다. 이는 프로토콜 패킷/디스크 레이아웃이 unaligned 위치에
 * 다바이트 필드를 둘 수 있기 때문에 중요하다.
 *
 * 사용 맥락:
 *   - NVMe는 호스트 메모리 레이아웃을 리틀엔디언으로 정의하므로 대부분 le 함수 사용
 *   - iSCSI/SCSI, NVMe-oF TCP discovery log, 네트워크 프로토콜은 빅엔디언(network order)
 *   - GPT 파티션 테이블: 리틀엔디언
 *   - iSCSI PDU 헤더: 빅엔디언
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 최하단 유틸리티. 특별한 실행 컨텍스트 없이 어디서든 호출 가능.
 * 주로 프로토콜 파싱/빌드 경로(lib/iscsi, lib/scsi, lib/nvme/nvme_fabric.c,
 * module/bdev/gpt, module/bdev/nvme 등)에서 사용.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h — uintN_t 확보.
 * 의존하는 모듈: 프로토콜 파서·빌더 전반.
 * 공유 자료구조: 없음.
 * 데이터 흐름: 메모리 ↔ 메모리 (순수 비트 조작, I/O 없음).
 *
 * === 주요 함수/구조체 요약 ===
 * 8쌍의 static inline 함수. 모두 O(1), 런타임 비용 수 사이클 이내.
 * byte-pointer 캐스팅을 통해 strict aliasing 위반 없이 안전하게 접근.
 */

#ifndef SPDK_ENDIAN_H            /* [한국어] include 가드 시작 */
#define SPDK_ENDIAN_H            /* [한국어] 가드 심볼 */

#include "spdk/stdinc.h"         /* [한국어] uintN_t 확보 */

#ifdef __cplusplus               /* [한국어] C++ 링크 규약 가드 */
extern "C" {
#endif

static inline uint16_t
from_be16(const void *ptr)
/*
 * [한국어]
 * from_be16 - 빅엔디언 2바이트 스트림을 호스트 16비트 정수로 변환
 *
 * @ptr: 빅엔디언 2바이트 영역의 시작 주소 (unaligned OK)
 * @return: 호스트 네이티브 표현의 16비트 정수
 *
 * 사용 예: 네트워크 패킷 헤더에서 u16 필드 파싱, iSCSI/SCSI CDB의 길이 필드 등
 */
{
	const uint8_t *tmp = (const uint8_t *)ptr;
                                 /* [한국어] void*를 uint8_t*로 명시 캐스팅 — byte-wise 접근이 가능하도록 강타입화
                                  *  - const 유지로 원본 변경 방지 보증 */
	return (((uint16_t)tmp[0] << 8) | tmp[1]);
                                 /* [한국어] 빅엔디언: 상위 바이트가 먼저 등장
                                  *  - tmp[0] = MSB, tmp[1] = LSB
                                  *  - (uint16_t) 캐스팅으로 8비트 시프트 시 프로모션 정확성 보장 */
}

static inline void
to_be16(void *out, uint16_t in)
/*
 * [한국어]
 * to_be16 - 호스트 16비트 정수를 빅엔디언 2바이트로 직렬화
 *
 * @out: 2바이트 이상 쓰기 가능한 출력 버퍼
 * @in: 직렬화할 값
 */
{
	uint8_t *tmp = (uint8_t *)out;
                                 /* [한국어] 출력 버퍼 byte-pointer 뷰 */
	tmp[0] = (in >> 8) & 0xFF;   /* [한국어] MSB 먼저 (빅엔디언) — 0xFF 마스크는 상위 바이트를 취한다는 의도 명시 */
	tmp[1] = in & 0xFF;          /* [한국어] LSB 저장 */
}

static inline uint32_t
from_be32(const void *ptr)
/*
 * [한국어]
 * from_be32 - 빅엔디언 4바이트 → 호스트 32비트
 * @ptr: 4바이트 빅엔디언 스트림 (unaligned OK)
 */
{
	const uint8_t *tmp = (const uint8_t *)ptr;
                                 /* [한국어] byte-wise 뷰 */
	return (((uint32_t)tmp[0] << 24) |
		((uint32_t)tmp[1] << 16) |
		((uint32_t)tmp[2] << 8) |
		((uint32_t)tmp[3]));
                                 /* [한국어] MSB→LSB 순서로 OR 결합. 각 바이트를 uint32_t로 프로모션 후 시프트 → 16비트 연산에서의 오버플로 방지 */
}

static inline void
to_be32(void *out, uint32_t in)
/*
 * [한국어]
 * to_be32 - 호스트 32비트 → 빅엔디언 4바이트
 */
{
	uint8_t *tmp = (uint8_t *)out;
	tmp[0] = (in >> 24) & 0xFF;  /* [한국어] MSB */
	tmp[1] = (in >> 16) & 0xFF;
	tmp[2] = (in >> 8) & 0xFF;
	tmp[3] = in & 0xFF;          /* [한국어] LSB */
}

static inline uint64_t
from_be64(const void *ptr)
/*
 * [한국어]
 * from_be64 - 빅엔디언 8바이트 → 호스트 64비트
 * 사용 예: NVMe-oF discovery log의 endpoint ID, iSCSI ISID 등 64비트 필드
 */
{
	const uint8_t *tmp = (const uint8_t *)ptr;
	return (((uint64_t)tmp[0] << 56) |
		((uint64_t)tmp[1] << 48) |
		((uint64_t)tmp[2] << 40) |
		((uint64_t)tmp[3] << 32) |
		((uint64_t)tmp[4] << 24) |
		((uint64_t)tmp[5] << 16) |
		((uint64_t)tmp[6] << 8) |
		((uint64_t)tmp[7]));
                                 /* [한국어] 8바이트를 MSB→LSB 순으로 조합. 모든 시프트 대상이 uint64_t임을 캐스팅으로 명시 (32비트 오버플로 방지) */
}

static inline void
to_be64(void *out, uint64_t in)
/*
 * [한국어]
 * to_be64 - 호스트 64비트 → 빅엔디언 8바이트
 */
{
	uint8_t *tmp = (uint8_t *)out;
	tmp[0] = (in >> 56) & 0xFF;  /* [한국어] 최상위 바이트 */
	tmp[1] = (in >> 48) & 0xFF;
	tmp[2] = (in >> 40) & 0xFF;
	tmp[3] = (in >> 32) & 0xFF;
	tmp[4] = (in >> 24) & 0xFF;
	tmp[5] = (in >> 16) & 0xFF;
	tmp[6] = (in >> 8) & 0xFF;
	tmp[7] = in & 0xFF;          /* [한국어] 최하위 바이트 */
}

static inline uint16_t
from_le16(const void *ptr)
/*
 * [한국어]
 * from_le16 - 리틀엔디언 2바이트 → 호스트 16비트
 * NVMe SQE/CQE, GPT 엔트리 등 리틀엔디언 프로토콜 파싱에 사용.
 */
{
	const uint8_t *tmp = (const uint8_t *)ptr;
	return (((uint16_t)tmp[1] << 8) | tmp[0]);
                                 /* [한국어] 리틀엔디언: LSB가 먼저 오므로 tmp[0]=LSB, tmp[1]=MSB → 조합 순서만 반대 */
}

static inline void
to_le16(void *out, uint16_t in)
{
	uint8_t *tmp = (uint8_t *)out;
	tmp[1] = (in >> 8) & 0xFF;   /* [한국어] 리틀엔디언은 상위 바이트를 뒤에 */
	tmp[0] = in & 0xFF;          /* [한국어] LSB가 처음 */
}

static inline uint32_t
from_le32(const void *ptr)
/*
 * [한국어]
 * from_le32 - 리틀엔디언 4바이트 → 호스트 32비트
 */
{
	const uint8_t *tmp = (const uint8_t *)ptr;
	return (((uint32_t)tmp[3] << 24) |
		((uint32_t)tmp[2] << 16) |
		((uint32_t)tmp[1] << 8) |
		((uint32_t)tmp[0]));
                                 /* [한국어] 리틀엔디언: 인덱스 0이 LSB. 비트 위치만 뒤집어서 조합 */
}

static inline void
to_le32(void *out, uint32_t in)
{
	uint8_t *tmp = (uint8_t *)out;
	tmp[3] = (in >> 24) & 0xFF;
	tmp[2] = (in >> 16) & 0xFF;
	tmp[1] = (in >> 8) & 0xFF;
	tmp[0] = in & 0xFF;          /* [한국어] LSB → tmp[0] */
}

static inline uint64_t
from_le64(const void *ptr)
/*
 * [한국어]
 * from_le64 - 리틀엔디언 8바이트 → 호스트 64비트
 */
{
	const uint8_t *tmp = (const uint8_t *)ptr;
	return (((uint64_t)tmp[7] << 56) |
		((uint64_t)tmp[6] << 48) |
		((uint64_t)tmp[5] << 40) |
		((uint64_t)tmp[4] << 32) |
		((uint64_t)tmp[3] << 24) |
		((uint64_t)tmp[2] << 16) |
		((uint64_t)tmp[1] << 8) |
		((uint64_t)tmp[0]));
}

static inline void
to_le64(void *out, uint64_t in)
{
	uint8_t *tmp = (uint8_t *)out;
	tmp[7] = (in >> 56) & 0xFF;
	tmp[6] = (in >> 48) & 0xFF;
	tmp[5] = (in >> 40) & 0xFF;
	tmp[4] = (in >> 32) & 0xFF;
	tmp[3] = (in >> 24) & 0xFF;
	tmp[2] = (in >> 16) & 0xFF;
	tmp[1] = (in >> 8) & 0xFF;
	tmp[0] = in & 0xFF;          /* [한국어] LSB가 인덱스 0 */
}

#ifdef __cplusplus
}
#endif

#endif                           /* [한국어] include 가드 종료 */
