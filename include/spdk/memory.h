/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] 페이지 크기 · 주소 정렬 매크로 (memory.h)
 *
 * === 파일의 역할 ===
 * SPDK가 DPDK hugepage 메모리를 다루면서 반복적으로 수행하는 주소 정렬 ·
 * 오프셋 계산을 매크로로 통일한다. 구체적으로:
 *   - 2MB hugepage 경계 정렬 (FLOOR_2MB, CEIL_2MB, _2MB_PAGE)
 *   - 2MB 내부 오프셋 추출 (_2MB_OFFSET)
 *   - 4KB 페이지(=NVMe PRP 단위) 경계 오프셋 (_4KB_OFFSET)
 *   - VFIO 활성 여부 빌드 상수 (VFIO_ENABLED)
 * 이 매크로들은 특히 (1) NVMe PRP(Physical Region Page) 리스트 생성 시
 * 4KB 경계 검사, (2) DPDK 2MB hugepage 테이블 lookup, (3) DMA 가능 메모리
 * 영역을 페이지 단위로 순회할 때 핵심적으로 사용된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 메모리 서브시스템의 최하단 상수 정의. lib/env_dpdk/memory.c의
 * 가상↔물리 주소 변환 테이블, lib/nvme/nvme_pcie.c의 PRP 리스트 생성,
 * 그리고 각종 DMA 버퍼 할당 경로에서 참조된다. 런타임 비용이 없는 순수
 * 컴파일 타임 상수·인라인 매크로.
 * 실행 컨텍스트: 모든 SPDK 컨텍스트에서 사용 가능 (hot path에서도 안전).
 *
 * === 타 모듈과의 연결 ===
 * 의존 대상:
 *   - spdk/stdinc.h — uintptr_t 등 표준 타입
 *   - linux/version.h — 리눅스 커널 버전 체크 (VFIO 활성화 판정용)
 * 의존하는 모듈:
 *   - lib/env_dpdk/memory.c — DPDK 가상→물리 주소 테이블 (2MB 엔트리)
 *   - lib/nvme/nvme_pcie*.c — PRP 리스트 빌드 시 4KB 경계 판정
 *   - lib/bdev/bdev.c / module/bdev/* — DMA 버퍼 정렬 확인
 *   - lib/blob/*, lib/ftl/* — 페이지 단위 블록 관리
 * 공유 자료구조: 없음 (상수·매크로만 제공).
 * 데이터 흐름: 없음.
 *
 * === 주요 함수/구조체 요약 ===
 * 매크로만 존재. 함수·구조체 없음.
 *   - SHIFT_2MB / VALUE_2MB / MASK_2MB: 2MB 페이지 shift · 값 · 하위 마스크
 *   - SHIFT_4KB / VALUE_4KB / MASK_4KB: 4KB 페이지 shift · 값 · 하위 마스크
 *   - _4KB_OFFSET(ptr): 포인터의 4KB 내부 오프셋 추출 (NVMe PRP 경계 판정)
 *   - _2MB_OFFSET(ptr): 포인터의 2MB 내부 오프셋 추출
 *   - _2MB_PAGE(ptr):   포인터가 속한 2MB 페이지 기준 주소
 *   - FLOOR_2MB(x):     x를 2MB 경계로 내림 정렬
 *   - CEIL_2MB(x):      x를 2MB 경계로 올림 정렬
 *   - VFIO_ENABLED:     VFIO 사용 가능 여부 (빌드 타임 플래그, 0/1)
 */

#ifndef SPDK_MEMORY_H            /* [한국어] include 가드 시작 — 중복 포함으로 인한 매크로 재정의 방지 */
#define SPDK_MEMORY_H            /* [한국어] 가드 심볼 정의 */

#include "spdk/stdinc.h"         /* [한국어] uintptr_t 등 표준 정수 타입 확보 — 포인터를 정수로 캐스팅하기 위해 필요 */

#ifndef __linux__                /* [한국어] 비(非)리눅스 빌드(예: FreeBSD, macOS) 분기
                                  *  - VFIO는 리눅스 전용 유저스페이스 장치 드라이버 프레임워크이므로, 타 OS에서는 기능을 비활성화 */
#define VFIO_ENABLED 0           /* [한국어] 비리눅스에서 VFIO 경로 자체가 없음을 빌드 타임에 확정 → DPDK PCI 드라이버 선택 로직이 uio 등 대체 경로로 분기 */
#else                            /* [한국어] 리눅스 빌드 — VFIO 가용성을 커널 버전으로 다시 판정 */
#include <linux/version.h>       /* [한국어] LINUX_VERSION_CODE 매크로 획득 — 커널 버전 비교용 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
                                 /* [한국어] VFIO는 리눅스 3.6에서 IOMMU 기반 유저스페이스 디바이스 접근 용도로 도입됨
                                  *  - 이보다 낮은 커널은 VFIO 관련 ioctl/파일이 없으므로 빌드에서 제거 */
#define VFIO_ENABLED 1           /* [한국어] VFIO 경로 활성 — lib/env_dpdk가 /dev/vfio/* 를 통한 DMA 매핑·인터럽트 제어 경로를 컴파일에 포함 */
#else
#define VFIO_ENABLED 0           /* [한국어] 오래된 커널에서는 VFIO 비활성 (대신 uio_pci_generic 등 레거시 경로 사용) */
#endif
#endif

#ifdef __cplusplus               /* [한국어] C++에서 포함될 때 C 링크 규약 적용 — 매크로 자체는 링크 규약 무관이나 관례 유지 */
extern "C" {
#endif

#define SHIFT_2MB		21 /* (1 << 21) == 2MB */
                                 /* [한국어] 2MB hugepage 크기의 shift 값.
                                  *   - DPDK의 기본 hugepage 크기가 2MB이므로 SPDK 전반이 이 상수를 가정한다
                                  *   - 1GB hugepage를 사용할 때도 2MB 하위 정렬은 동일하게 성립하므로 이 값이 기본 */
#define VALUE_2MB		(1ULL << SHIFT_2MB)
                                 /* [한국어] 2MB(=0x0020_0000) 값. ULL 접미사로 64비트 연산 보장
                                  *  - 32비트 시프트 오버플로를 피하기 위해 반드시 unsigned long long 리터럴 사용 */
#define MASK_2MB		(VALUE_2MB - 1)
                                 /* [한국어] 2MB 내부 오프셋 추출용 하위 21비트 마스크(=0x001F_FFFF)
                                  *  - `ptr & MASK_2MB` → 2MB 페이지 시작으로부터의 바이트 오프셋
                                  *  - `ptr & ~MASK_2MB` → 2MB 정렬된 페이지 시작 주소 */

#define SHIFT_4KB		12 /* (1 << 12) == 4KB */
                                 /* [한국어] 4KB 페이지 shift 값.
                                  *  - NVMe 스펙의 PRP(Physical Region Page) 엔트리 단위가 최소 4KB (MPS 설정에 따라 더 커짐)
                                  *  - CPU 페이지 테이블의 기본 페이지 크기와도 일치하므로 일반 virt↔phys 매핑에도 활용 */
#define VALUE_4KB		(1ULL << SHIFT_4KB)
                                 /* [한국어] 4KB(=0x0000_1000) 값 */
#define MASK_4KB		(VALUE_4KB - 1)
                                 /* [한국어] 4KB 내부 오프셋 마스크(=0x0000_0FFF)
                                  *  - NVMe PRP 규약: 첫 PRP는 임의 오프셋 허용, 이후 PRP들은 4KB 정렬 필수 → 이 마스크로 판정 */

#define _4KB_OFFSET(ptr)	(((uintptr_t)(ptr)) & MASK_4KB)
                                 /* [한국어] 포인터의 4KB 내부 오프셋 추출
                                  *  - ptr을 uintptr_t로 캐스팅하여 비트 연산 가능하게 함 (포인터에 직접 & 연산 불가)
                                  *  - 사용 예: PRP1에는 임의 오프셋의 시작 주소를 넣고, 이후 PRP2부터는
                                  *    `_4KB_OFFSET(addr) == 0` 이어야 함을 검증 */
#define _2MB_OFFSET(ptr)	(((uintptr_t)(ptr)) & MASK_2MB)
                                 /* [한국어] 포인터의 2MB hugepage 내부 오프셋 추출
                                  *  - DPDK 주소 변환 테이블은 2MB 단위 엔트리를 가지므로, 엔트리 내부 오프셋 계산에 사용 */
#define _2MB_PAGE(ptr)		FLOOR_2MB((uintptr_t)(ptr))
                                 /* [한국어] ptr이 속한 2MB 페이지의 기준(base) 주소 반환
                                  *  - `ptr`에 대해 2MB 경계로 내림 → 해당 hugepage의 가상 주소 시작점을 얻음
                                  *  - DPDK의 rte_mem_virt2phy()를 호출할 때 엔트리 키로 사용 */
#define FLOOR_2MB(x)		(((uintptr_t)(x)) & ~MASK_2MB)
                                 /* [한국어] x를 2MB 경계로 내림 정렬 (floor).
                                  *  - `~MASK_2MB`는 상위 비트만 남기는 마스크 → 하위 21비트를 0으로 밀어 버림
                                  *  - hugepage 경계 시작을 계산할 때 사용 */
#define CEIL_2MB(x)		FLOOR_2MB(((uintptr_t)(x)) + VALUE_2MB - 1)
                                 /* [한국어] x를 2MB 경계로 올림 정렬 (ceil).
                                  *  - (x + 2MB - 1)을 floor하면 올림과 동일한 효과 — 고전적 정렬 기법
                                  *  - 예: 메모리 영역의 끝 주소를 포함하는 마지막 hugepage 경계를 구할 때
                                  *  - 주의: x가 이미 2MB 정렬이면 그 자리 유지 (FLOOR(x + 2MB - 1) = x) */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_MEMORY_H */        /* [한국어] include 가드 종료 */
