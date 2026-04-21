/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Memory-mapped I/O utility functions
 */

/*
 * [한국어 설명] MMIO(Memory-Mapped I/O) 액세스 래퍼 (mmio.h)
 *
 * === 파일의 역할 ===
 * PCIe 디바이스의 BAR(Base Address Register) 영역에 매핑된 레지스터에 대해
 * 1/2/4/8 바이트 단위 read/write를 안전하게 수행하는 inline 헬퍼를 제공한다.
 * MMIO는 일반 DRAM과 주소 공간은 같지만 동작은 완전히 다르며, SPDK는 다음
 * 두 위험을 이 헬퍼로 차단한다:
 *   (1) 컴파일러가 MMIO 접근을 재정렬·삭제·병합·중복 수행하는 것.
 *       → `volatile` 포인터 + `spdk_compiler_barrier()`로 방지.
 *   (2) 32비트 CPU에서 64비트 MMIO가 "atomic 64-bit transaction"으로 가지
 *       못하고 두 번에 나눠 나가는 문제.
 *       → 64비트 연산이 단일 트랜잭션임이 보장되는 x86_64에서는 직접 64비트
 *         접근, 그 외 아키텍처에서는 하위 32비트 먼저, 상위 32비트 나중의
 *         2-스텝 접근으로 분해. 순서는 I/OAT 등 Intel 디바이스가 기대하는
 *         순서를 따름.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 유저스페이스 NVMe 드라이버 및 PCI 기반 모든 장치 드라이버(I/OAT,
 * IDXD, VMD 등)에서 BAR 레지스터 접근 시 호출된다. 특히 NVMe의 경우:
 *   - Controller Config/Status 레지스터 read/write (lib/nvme/nvme_ctrlr.c)
 *   - SQ/CQ doorbell write (lib/nvme/nvme_pcie*.c)
 * 실행 컨텍스트: 유저스페이스 — 커널 mmap으로 받은 BAR 가상 주소를 전달받는다.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/stdinc.h       - uint8_t ~ uint64_t 등
 *   - spdk/barrier.h      - spdk_compiler_barrier
 * 의존하는 모듈:
 *   - lib/nvme/nvme_pcie*.c — CC/CSTS/AQA/ASQ/ACQ/doorbell 접근
 *   - module/bdev/nvme/* — NVMe bdev 모듈의 admin/IO 제출
 *   - lib/ioat, lib/idxd, lib/vmd 등 기타 PCI 드라이버
 * 공유 자료구조: 없음 — 포인터와 값만 주고받는 무상태 함수.
 * 데이터 흐름: CPU ↔ PCIe config/BAR 공간 (시스템 MMU + PCIe root complex
 * 경유). write는 write-posting 될 수 있어 완료 시점을 보장하려면 호출측이
 * 추가 barrier/read-back을 수행해야 한다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_mmio_read_1/2/4/8:  1/2/4/8 바이트 MMIO read
 *   - spdk_mmio_write_1/2/4/8: 1/2/4/8 바이트 MMIO write
 * 모두 static inline — 함수 호출 오버헤드 없이 volatile load/store 한 개로
 * 축소됨. 대부분의 경우 컴파일 후 단일 mov 명령과 동일하다.
 *
 * 중요 관례:
 *   - 파라미터는 반드시 `volatile` 포인터여야 함. 비volatile 포인터를
 *     넘기면 컴파일러가 read를 삭제하거나 write를 dead store로 제거할 수 있다.
 *   - 호출 직전 `spdk_compiler_barrier()`로 컴파일러 재정렬을 차단한다.
 *     CPU 재정렬 차단(즉 MMIO 순서 보장)까지 원하면 호출측에서 별도로
 *     spdk_wmb/rmb를 발행해야 한다.
 *   - 64비트 read는 하드웨어에 따라 하위-상위 순서 민감함 (I/OAT 디스크립터
 *     write-back 등) → x86_64가 아니면 2×32bit read로 분해.
 */

#ifndef SPDK_MMIO_H              /* [한국어] include 가드 시작 */
#define SPDK_MMIO_H              /* [한국어] 가드 심볼 */

#include "spdk/stdinc.h"         /* [한국어] 고정폭 정수 타입(uint8/16/32/64_t) 확보 */

#ifdef __cplusplus               /* [한국어] C++ 링크 규약 보호 — inline 함수도 extern "C"로 감싸 mangled name 충돌 방지 */
extern "C" {
#endif

#include "spdk/barrier.h"        /* [한국어] spdk_compiler_barrier() 매크로 확보 — 아래 모든 함수가 사용 */

#ifdef __x86_64__                /* [한국어] x86_64는 aligned 8-byte MMIO가 PCIe 상에서 단일 트랜잭션으로 발행됨을 CPU가 보장 */
#define SPDK_MMIO_64BIT	1 /* Can do atomic 64-bit memory read/write (over PCIe) */
                                 /* [한국어] 64비트 단일 MMIO 가능 → 아래 read_8/write_8이 직접 *addr 접근 사용 */
#else
#define SPDK_MMIO_64BIT	0        /* [한국어] 32bit 또는 기타 아키텍처: 단일 64비트 트랜잭션 불가 → 32+32 2-step 분해 필요 */
#endif

static inline uint8_t
spdk_mmio_read_1(const volatile uint8_t *addr)
/*
 * [한국어]
 * spdk_mmio_read_1 - 1바이트 MMIO 레지스터 read
 *
 * @addr: BAR 내 대상 레지스터 주소 (mmap으로 얻은 유저스페이스 가상 주소, `volatile` 필수)
 * @return: 읽은 8비트 값
 *
 * NVMe 등 대부분의 장치에서는 1바이트 단위 접근이 드물지만, 일부 config
 * 스페이스나 벤더 전용 레지스터 접근을 위해 존재한다.
 * 실행 컨텍스트: 모든 SPDK thread.
 * 호출 체인: 장치 드라이버 (예: lib/nvme) → [이 함수] → PCIe root complex → device
 */
{
	spdk_compiler_barrier();     /* [한국어] 컴파일러 재정렬 차단 — 이 read가 주변 store/load보다 먼저/뒤로 이동하는 것 방지 */
	return *addr;                /* [한국어] volatile 포인터 역참조 → 컴파일러가 최적화로 제거·캐싱하지 못함. 실제 PCIe read transaction 1개 발행 */
}

static inline void
spdk_mmio_write_1(volatile uint8_t *addr, uint8_t val)
/*
 * [한국어]
 * spdk_mmio_write_1 - 1바이트 MMIO 레지스터 write
 *
 * @addr: 대상 주소 (volatile)
 * @val: 쓸 값
 *
 * 주의: write-posting 때문에 함수 리턴 시점에 장치가 값을 관찰했다는 보장은
 * 없다. 완료 보장을 위해서는 호출측에서 write 직후 read-back을 하거나
 * spdk_mb()를 발행해야 한다.
 */
{
	spdk_compiler_barrier();     /* [한국어] 이 write 전 컴파일러 재정렬 차단 */
	*addr = val;                 /* [한국어] 단일 8비트 PCIe write 발행 */
}

static inline uint16_t
spdk_mmio_read_2(const volatile uint16_t *addr)
/*
 * [한국어]
 * spdk_mmio_read_2 - 2바이트(16비트) MMIO read
 *
 * @addr: 대상 주소 (2바이트 정렬 필수)
 * @return: 읽은 16비트 값
 *
 * NVMe doorbell stride=0인 기본 구성에서 사용되지는 않지만, PCI config space
 * vendor/device ID 등 16비트 필드 접근에 사용.
 */
{
	spdk_compiler_barrier();     /* [한국어] 컴파일러 배리어 */
	return *addr;                /* [한국어] 16비트 volatile load → PCIe 2B read */
}

static inline void
spdk_mmio_write_2(volatile uint16_t *addr, uint16_t val)
/*
 * [한국어]
 * spdk_mmio_write_2 - 2바이트 MMIO write
 */
{
	spdk_compiler_barrier();     /* [한국어] 컴파일러 배리어 */
	*addr = val;                 /* [한국어] 16비트 volatile store */
}

static inline uint32_t
spdk_mmio_read_4(const volatile uint32_t *addr)
/*
 * [한국어]
 * spdk_mmio_read_4 - 4바이트(32비트) MMIO read
 *
 * @addr: 대상 주소 (4바이트 정렬)
 * @return: 읽은 32비트 값
 *
 * NVMe 스펙의 대부분 컨트롤러 레지스터(CC, CSTS, AQA, VS, INTMS 등)는 32비트
 * 폭이므로 이 함수가 hot path는 아니지만 가장 빈번히 호출된다.
 * doorbell은 기본 4바이트 폭이므로 SQ/CQ tail/head ring 알림에도 사용.
 */
{
	spdk_compiler_barrier();
	return *addr;                /* [한국어] 32비트 volatile load → 단일 PCIe 4B read */
}

static inline void
spdk_mmio_write_4(volatile uint32_t *addr, uint32_t val)
/*
 * [한국어]
 * spdk_mmio_write_4 - 4바이트 MMIO write
 *
 * NVMe SQ/CQ doorbell write가 이 경로로 나간다. hot path.
 * 호출자 책임: doorbell 직전 spdk_wmb()로 SQ 엔트리 쓰기 완료 보장 필요.
 */
{
	spdk_compiler_barrier();
	*addr = val;                 /* [한국어] 단일 32비트 volatile store — doorbell ring 등 */
}

static inline uint64_t
spdk_mmio_read_8(volatile uint64_t *addr)
/*
 * [한국어]
 * spdk_mmio_read_8 - 8바이트(64비트) MMIO read
 *
 * @addr: 대상 주소 (8바이트 정렬)
 * @return: 읽은 64비트 값
 *
 * x86_64에서는 단일 64비트 트랜잭션, 그 외에서는 하위→상위 순서의 두 번의
 * 32비트 read로 분해된다. 특정 HW(I/OAT)가 이 순서를 기대하므로 반대 순서가
 * 필요하면 호출자가 spdk_mmio_read_4() 쌍을 직접 사용해야 한다.
 * NVMe: CAP(Controller Capabilities) 레지스터(64비트), ASQ/ACQ 주소 레지스터
 * 등이 이 경로로 읽힌다.
 */
{
	uint64_t val;                /* [한국어] 반환할 조합 결과를 담는 로컬 변수 */
	volatile uint32_t *addr32 = (volatile uint32_t *)addr;
                                 /* [한국어] 동일 주소를 32비트 배열 뷰로 reinterpret — 2-step 분해를 위해
                                  *  - volatile 유지 필수: 컴파일러가 read 두 번을 삭제·병합하지 못하게 함 */

	spdk_compiler_barrier();     /* [한국어] 이 read 블록 진입 전 컴파일러 재정렬 차단 */

	if (SPDK_MMIO_64BIT) {       /* [한국어] 컴파일 타임 상수 — 최적화 시 해당 분기만 살아남아 dead branch 제거됨 */
		val = *addr;             /* [한국어] x86_64: 단일 PCIe 8B read — 원자성 보장 */
	} else {
		/*
		 * Read lower 4 bytes before upper 4 bytes.
		 * This particular order is required by I/OAT.
		 * If the other order is required, use a pair of spdk_mmio_read_4() calls.
		 */
		val = addr32[0];         /* [한국어] 하위 32비트 먼저 read — I/OAT 디스크립터 업데이트 감지 순서 준수
                                  *  - I/OAT는 디스크립터의 lower dword를 최신으로 쓰므로 하위를 먼저 읽어야 일관된 값 확보 */
		val |= (uint64_t)addr32[1] << 32;
                                 /* [한국어] 상위 32비트를 64비트로 확장 후 상위 워드 위치로 시프트, OR로 결합
                                  *  - (uint64_t) 캐스팅 없으면 32비트 시프트가 undefined behavior */
	}

	return val;                  /* [한국어] 조합된 64비트 값 반환 */
}

static inline void
spdk_mmio_write_8(volatile uint64_t *addr, uint64_t val)
/*
 * [한국어]
 * spdk_mmio_write_8 - 8바이트 MMIO write
 *
 * x86_64: 단일 트랜잭션. 그 외: 하위→상위 순서로 두 번의 32비트 write.
 * 사용처: NVMe ASQ/ACQ base address 레지스터(64비트) 설정, Controller
 * Reset 시퀀스 중 Admin Queue 주소 프로그래밍 등.
 */
{
	volatile uint32_t *addr32 = (volatile uint32_t *)addr;
                                 /* [한국어] 32비트 배열 뷰로 재해석 — 분해 write용 */

	spdk_compiler_barrier();     /* [한국어] 컴파일러 배리어 */

	if (SPDK_MMIO_64BIT) {
		*addr = val;             /* [한국어] x86_64: 단일 8B PCIe write */
	} else {
		addr32[0] = (uint32_t)val;
                                 /* [한국어] 하위 32비트 먼저 write — HW가 하위 dword 쓰기 이후 아직 상위가 업데이트 안 된 과도 상태를 볼 수 있으므로,
                                  *  값 일관성에 민감한 레지스터는 이 경로를 피하고 HW 스펙에 따라 별도 시퀀스를 사용해야 한다 */
		addr32[1] = (uint32_t)(val >> 32);
                                 /* [한국어] 상위 32비트 write — 32비트 시프트로 상위 워드 추출 */
	}
}

#ifdef __cplusplus
}
#endif

#endif                           /* [한국어] include 가드 종료 */
