/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * CRC-32 utility functions
 */

/*
 * [한국어 설명] CRC-32 / CRC-32C 유틸리티 (crc32.h)
 *
 * === 파일의 역할 ===
 * 두 가지 CRC-32 방식과 그 변종을 제공한다:
 *   - CRC-32 IEEE  (다항식 0xedb88320, 일반적인 ZIP/Ethernet 호환)
 *     → GPT 파티션 테이블 헤더/엔트리 CRC 계산 등에 사용
 *   - CRC-32C     (Castagnoli 0x82f63b78, SSE4.2 CRC32 명령 지원)
 *     → iSCSI digest, NVMe-oF 디스커버리 로그, NVMe 32-bit PI Guard,
 *       BIC-TCP, SCTP 등에서 사용
 *   - CRC-32C iovec: 분산 버퍼 대상 스트리밍 계산
 *   - CRC-32C "nvme": NVMe PI용 초기/종결 처리를 반영한 래퍼
 *
 * === 전체 아키텍처에서의 위치 ===
 * 구현: lib/util/crc32.c + crc32_ieee.c + crc32c.c.
 * CPU가 SSE4.2 `crc32` 명령을 지원하면 하드웨어 가속, 없으면 테이블 기반 SW.
 * ISA-L 활성 시 PCLMUL 가속으로 수 GB/s 처리.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/stdinc.h - uint32_t/size_t/iovec
 *   - spdk/config.h - ISA-L/가속 옵션
 * 의존하는 모듈:
 *   - lib/iscsi (digest 검증)
 *   - lib/nvmf / lib/nvme (discovery log CRC, 32-bit PI)
 *   - module/bdev/gpt (파티션 테이블 CRC)
 * 공유 자료구조: 없음.
 *
 * === 주요 함수/구조체 요약 ===
 *   - SPDK_CRC32_SIZE_BYTES = 4
 *   - spdk_crc32_ieee_update:      IEEE 802.3 다항식 CRC-32
 *   - spdk_crc32c_update:          Castagnoli CRC-32C (단일 버퍼)
 *   - spdk_crc32c_iov_update:      iovec 배열 CRC-32C (분산 버퍼)
 *   - spdk_crc32c_nvme:            NVMe PI용 CRC-32C (반전/초기값 처리)
 */

#ifndef SPDK_CRC32_H             /* [한국어] include 가드 */
#define SPDK_CRC32_H

#include "spdk/stdinc.h"         /* [한국어] uint32_t/size_t/iovec */
#include "spdk/config.h"         /* [한국어] 빌드 옵션 (ISA-L 여부 등) */

#define SPDK_CRC32_SIZE_BYTES 4
                                 /* [한국어] CRC-32 결과 크기 (바이트) — buffer 길이 계산 편의용 상수 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Calculate a partial CRC-32 IEEE checksum.
 *
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \param crc Previous CRC-32 value.
 * \return Updated CRC-32 value.
 */
uint32_t spdk_crc32_ieee_update(const void *buf, size_t len, uint32_t crc);
/*
 * [한국어]
 * spdk_crc32_ieee_update - IEEE CRC-32 스트리밍 업데이트
 *
 * 다항식 0xedb88320(reflected form). Ethernet/ZIP/PNG 호환.
 * 초기/종결 XOR 관례: 첫 호출 시 crc=~0, 마지막에 결과 ~ 적용 (호출자 책임).
 */

/**
 * Calculate a partial CRC-32C checksum.
 *
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \param crc Previous CRC-32C value.
 * \return Updated CRC-32C value.
 */
uint32_t spdk_crc32c_update(const void *buf, size_t len, uint32_t crc);
/*
 * [한국어]
 * spdk_crc32c_update - Castagnoli CRC-32C 스트리밍
 *
 * 다항식 0x82f63b78 (reflected). Intel SSE4.2 crc32 명령어로 하드웨어 가속 가능.
 * SPDK는 CPU feature detection 후 최적 경로 선택.
 */

/**
 * Calculate a partial CRC-32C checksum.
 *
 * \param iov Data buffer vectors to checksum.
 * \param iovcnt size of iov parameter.
 * \param crc32c Previous CRC-32C value.
 * \return Updated CRC-32C value.
 */
uint32_t spdk_crc32c_iov_update(struct iovec *iov, int iovcnt, uint32_t crc32c);
/*
 * [한국어]
 * spdk_crc32c_iov_update - iovec 배열에 대한 CRC-32C
 *
 * scatter-gather 버퍼에 대해 각 세그먼트를 순차적으로 CRC에 반영.
 * NVMe-oF 등 non-contiguous 버퍼 처리에 사용.
 */

/**
 * Calculate a CRC-32C checksum, for NVMe Protection Information
 *
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \param crc Previous CRC-32C value.
 * \return Updated CRC-32C value.
 */
uint32_t spdk_crc32c_nvme(const void *buf, size_t len, uint32_t crc);
/*
 * [한국어]
 * spdk_crc32c_nvme - NVMe 32-bit PI Guard용 CRC-32C
 *
 * NVMe 스펙은 CRC-32C를 기반으로 하되 초기/종결 값 처리가 일반 CRC-32C와
 * 약간 다름. 이 래퍼가 그 차이를 흡수하여 PI Guard 필드와 일치하는 결과 반환.
 */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_CRC32_H */         /* [한국어] include 가드 종료 */
