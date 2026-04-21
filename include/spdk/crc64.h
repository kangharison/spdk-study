/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * CRC-64 utility functions
 */

/*
 * [한국어 설명] CRC-64 (NVMe PI) 유틸리티 (crc64.h)
 *
 * === 파일의 역할 ===
 * NVMe 2.0 이후 도입된 CRC-64(Rocksoft 다항식 0xad93d23594c93659)를
 * Protection Information (PI) 필드 계산에 사용하기 위한 함수를 노출한다.
 * PI는 호스트-저장소 간 512/4096B 블록 외에 8B(혹은 16B) 보호 메타데이터를
 * 덧붙여 end-to-end data integrity를 보장한다. CRC-16(T10 DIF) 대비 더 강한
 * 보호를 원하는 최신 NVMe 드라이브가 CRC-64 PI를 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 구현: lib/util/crc64.c (또는 ISA-L 사용 빌드에서는 가속 구현).
 * 호출 경로: NVMe read/write 완료 시 PI 검증, 또는 호스트 생성 데이터에
 * PI 삽입(bdev DIF 모듈 경유).
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/stdinc.h  - 표준 타입
 *   - spdk/config.h  - 빌드 옵션 (ISA-L 가속 여부 등)
 * 의존하는 모듈: lib/bdev/bdev.c PI 경로, module/bdev/nvme NVMe PI 처리,
 *            lib/util/dif.c (T10 DIF 계산)
 * 공유 자료구조: 없음 (함수 호출).
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_crc64_nvme(buf, len, prev_crc):
 *       NVMe Rocksoft 다항식 기반 CRC-64 업데이트.
 *       스트리밍 방식 — 여러 버퍼를 이어서 해시할 때 이전 CRC를 인자로 전달.
 */

#ifndef SPDK_CRC64_H             /* [한국어] include 가드 */
#define SPDK_CRC64_H

#include "spdk/stdinc.h"         /* [한국어] uint64_t/size_t 등 */
#include "spdk/config.h"         /* [한국어] SPDK_CONFIG_ISAL 등 빌드 옵션 — 가속 구현 선택 시 참조 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Calculate a CRC-64 checksum (Rocksoft), for NVMe Protection Information
 *
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \param crc Previous CRC-64 value.
 * \return Updated CRC-64 value.
 */
uint64_t spdk_crc64_nvme(const void *buf, size_t len, uint64_t crc);
/*
 * [한국어]
 * spdk_crc64_nvme - NVMe PI(Rocksoft) CRC-64 갱신
 *
 * @buf: 체크섬할 데이터
 * @len: 바이트 수
 * @crc: 이전 CRC 값 (첫 호출: 초기값, 보통 0)
 * @return: 갱신된 CRC-64
 *
 * 다항식: 0xad93d23594c93659 (Rocksoft). NVMe 2.0 "Protection Information
 * Type 3 with 64-bit Guard"에서 규정.
 * 구현: 빌드 시 ISA-L 활성 여부에 따라 SW 테이블 또는 SSE/PCLMUL 가속 선택.
 * 스트리밍: 큰 데이터를 청크로 나누어 반복 호출하면 최종 CRC는 전체 CRC와 동일.
 */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_CRC64_H */         /* [한국어] include 가드 종료 */
