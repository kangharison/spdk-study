/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * CRC-16 utility functions
 */

/*
 * [한국어 설명] CRC-16 (T10 DIF) 유틸리티 (crc16.h)
 *
 * === 파일의 역할 ===
 * SCSI T10 DIF(Data Integrity Field) 표준에 사용되는 CRC-16 계산 함수를
 * 제공한다. T10 DIF는 블록 디바이스 I/O 경로에서 각 블록에 8바이트 보호
 * 필드를 덧붙여 저장 매체와 호스트 사이의 비트 플립 등을 검출한다:
 *   - Guard    : 2B CRC-16 (이 파일이 제공)
 *   - App Tag  : 2B
 *   - Ref Tag  : 4B (LBA 파생)
 *
 * SPDK의 `lib/util/dif.c`가 이 함수를 사용해 DIF 블록의 Guard 필드를 계산·
 * 검증한다. NVMe-oF 타겟에서 호스트 트랜스포트와 백엔드 미디어 간 PI 유지
 * 여부에 영향.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 구현: lib/util/crc16.c. ISA-L 지원 빌드에서는 최적화 버전 사용.
 * 실행 컨텍스트: 블록 I/O hot path. ISA-L PCLMUL 가속이 있으면 수 GB/s 처리.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h — 표준 타입.
 * 의존하는 모듈: lib/util/dif.c, lib/bdev PI 경로, module/bdev/nvme PI 처리.
 * 공유 자료구조: 없음.
 *
 * === 주요 함수/구조체 요약 ===
 *   - SPDK_T10DIF_CRC16_POLYNOMIAL : 0x8bb7 (T10 DIF 규정 다항식)
 *   - spdk_crc16_t10dif(init, buf, len): 단순 CRC 계산
 *   - spdk_crc16_t10dif_copy(...):      복사와 CRC 계산을 fused 수행
 *                                        → 한 번의 메모리 순회로 두 일 처리
 *                                          (DMA 복사 경로에서 bandwidth 절감)
 */

#ifndef SPDK_CRC16_H             /* [한국어] include 가드 */
#define SPDK_CRC16_H

#include "spdk/stdinc.h"         /* [한국어] uint16_t/size_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * T10-DIF CRC-16 polynomial
 */
#define SPDK_T10DIF_CRC16_POLYNOMIAL 0x8bb7u
                                 /* [한국어] T10 SBC-3 §4.22 규정 CRC-16 다항식 x^16 + x^15 + x^11 + x^9 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1
                                  *  - u 접미사로 unsigned int 확정 (비트 연산 안전성) */

/**
 * Calculate T10-DIF CRC-16 checksum.
 *
 * \param init_crc Initial CRC-16 value.
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \return CRC-16 value.
 */
uint16_t spdk_crc16_t10dif(uint16_t init_crc, const void *buf, size_t len);
/*
 * [한국어]
 * spdk_crc16_t10dif - T10 DIF CRC-16 계산
 *
 * @init_crc: 이전 CRC (첫 호출 시 0 또는 사양이 요구하는 초기값)
 * @buf: 체크섬 대상 데이터
 * @len: 바이트 길이
 * @return: 계산된 CRC-16
 *
 * 호출 예: PI의 Guard 필드 검증/생성. 블록 단위(512/4096B)로 반복.
 */

/**
 * Calculate T10-DIF CRC-16 checksum and copy data.
 *
 * \param init_crc Initial CRC-16 value.
 * \param dst Destination data buffer for copy.
 * \param src Source data buffer for CRC calculation and copy.
 * \param len Length of buffer in bytes.
 * \return CRC-16 value.
 */
uint16_t spdk_crc16_t10dif_copy(uint16_t init_crc, uint8_t *dst, uint8_t *src,
				size_t len);
/*
 * [한국어]
 * spdk_crc16_t10dif_copy - 복사 + CRC 계산 fused 버전
 *
 * @dst: 목적지 버퍼
 * @src: 원본 버퍼 (복사 대상이자 CRC 계산 대상)
 * @return: src 내용의 CRC-16
 *
 * 장점: 동일 바이트를 memcpy 한 번, CRC 계산 한 번으로 각각 순회하는 대신
 *       fused 한 번 순회로 처리 → 캐시 효율/bandwidth 향상. hot path 최적화.
 */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_CRC16_H */         /* [한국어] include 가드 종료 */
