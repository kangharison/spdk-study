/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * XOR utility functions
 */

/*
 * [한국어 설명] XOR 연산 유틸리티 (xor.h)
 *
 * === 파일의 역할 ===
 * 여러 소스 버퍼를 바이트 단위 XOR해 단일 destination에 쓰는 함수와, 최적
 * 벡터화(AVX2/AVX-512)를 위한 권장 alignment 조회 함수를 제공한다. SPDK의
 * RAID-5/6 패리티 계산(Q/P 블록 생성), 데이터 축약(hash merge) 등에서 사용.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 구현: lib/util/xor.c. AVX2/AVX-512 활성 CPU에서는 벡터화된 XOR 루프 사용,
 * 아니면 스칼라 폴백. DPDK의 rte_xor 대체 목적이 아니며 SPDK 자체 구현.
 * 실행 컨텍스트: hot path (RAID-5 쓰기 시 매 스트립마다 호출).
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h — 표준 타입.
 * 의존하는 모듈: module/bdev/raid (RAID-5 parity generation).
 * 공유 자료구조: 없음.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_xor_gen(dest, sources, n, len):
 *       dest[i] = sources[0][i] ^ sources[1][i] ^ ... (i=0..len-1, n 소스)
 *   - spdk_xor_get_optimal_alignment():
 *       내부 벡터 명령 폭에 맞는 권장 정렬 (보통 32 또는 64바이트).
 *       호출자는 이 정렬로 버퍼 할당 시 최고 성능.
 */

#ifndef SPDK_XOR_H               /* [한국어] include 가드 */
#define SPDK_XOR_H

#include "spdk/stdinc.h"         /* [한국어] uint32_t/size_t/void* 등 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Generate XOR from multiple source buffers.
 *
 * \param dest Destination buffer.
 * \param sources Array of source buffers.
 * \param n Number of source buffers in the array.
 * \param len Length of each buffer in bytes.
 * \return 0 on success, negative error code otherwise.
 */
int spdk_xor_gen(void *dest, void **sources, uint32_t n, uint32_t len);
/*
 * [한국어]
 * spdk_xor_gen - n개 소스 버퍼를 XOR해 dest에 기록
 *
 * @dest: 결과 버퍼 (len 바이트 쓰기 가능)
 * @sources: 소스 버퍼 포인터 배열 (길이 n)
 * @n: 소스 수 (2 이상 권장; 1이면 단순 복사와 동등)
 * @len: 각 버퍼 길이(바이트). 모든 소스/dest가 동일 길이여야 함
 * @return: 0 성공, 음수 errno 실패 (정렬 불일치 등)
 *
 * 사용처: RAID-5 parity = D0 ^ D1 ^ ... ^ Dk. 매 스트라이프 write 시 호출.
 */

/**
 * Get the optimal buffer alignment for XOR functions.
 *
 * \return The alignment in bytes.
 */
size_t spdk_xor_get_optimal_alignment(void);
/*
 * [한국어]
 * spdk_xor_get_optimal_alignment - XOR 함수 최적 버퍼 정렬 반환
 *
 * @return: 벡터 명령 폭에 맞춘 정렬(바이트). AVX-512이면 64, AVX2는 32, 스칼라는 8.
 *
 * 정렬되지 않은 버퍼에서도 동작하지만 성능 저하. posix_memalign 등으로
 * 이 값에 맞춰 버퍼를 할당하면 최고 처리량 확보.
 */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_XOR_H */           /* [한국어] include 가드 종료 */
