/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   Copyright (c) 2017, IBM Corporation.
 *   All rights reserved.
 */

/** \file
 * Memory barriers
 */

/*
 * [한국어 설명] 메모리 배리어 매크로 (barrier.h)
 *
 * === 파일의 역할 ===
 * CPU 아키텍처별 메모리 배리어 명령을 하나의 공통 인터페이스로 감싸 제공한다.
 * 메모리 배리어는 두 가지 종류의 재정렬을 막는다:
 *   (1) 컴파일러 재정렬 — 옵티마이저가 독립적인 메모리 접근의 순서를 바꾸는 것
 *   (2) CPU 재정렬     — out-of-order 실행기/store buffer/load queue에 의해
 *                        실제 메모리 버스에 나가는 순서가 소스 순서와 달라지는 것
 * SPDK는 유저스페이스 NVMe 드라이버·bdev hot path에서 장치 MMIO(doorbell)
 * 쓰기와 DMA 디스크립터(SQ/CQ 엔트리) 쓰기의 순서를 엄격히 지켜야 한다.
 * 예: "SQ 엔트리를 쓰고 → doorbell을 써야" 장치가 올바른 명령을 읽는다.
 * 이 순서가 보장되지 않으면 NVMe 컨트롤러가 반쪽 명령을 읽는 데이터 경합이
 * 발생하므로, 배리어는 단순 최적화가 아니라 정확성 필수 요소다.
 *
 * 제공 매크로 분류:
 *   - spdk_compiler_barrier(): 컴파일러 재정렬만 막음, CPU 명령은 생성 안 함
 *   - spdk_{r,w,}mb():         장치 MMIO/DMA 대상 강한 배리어 (CPU 재정렬 포함)
 *   - spdk_smp_{r,w,}mb():     다른 CPU 코어와의 공유 메모리용 약한 배리어
 *   - spdk_ivdt_dcache():      데이터 캐시 무효화 (주로 ARMv8에서 필요)
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 가장 하단의 아키텍처 포팅 레이어. 다음에서 사용됨:
 *   - lib/nvme/nvme_pcie*.c: SQ 엔트리 write → wmb → doorbell write 시퀀스
 *   - lib/nvme/nvme_qpair.c: CQ phase 비트 read → rmb → completion 해석
 *   - lib/thread/thread.c:   lockless message ring의 공유 상태 가시성 확보
 *   - 각종 ring/queue 자료구조(소스 공유)의 producer/consumer 동기화
 * 런타임 비용이 있는(=CPU fence 명령) hot path 요소이므로, 꼭 필요한
 * 위치에만 배치해야 한다.
 * 실행 컨텍스트: 모든 SPDK 컨텍스트에서 호출 가능. 인터럽트/시그널 안전.
 *
 * === 타 모듈과의 연결 ===
 * 의존 대상: spdk/stdinc.h — 실사용 타입은 없으나 관례상 포함.
 * 의존하는 모듈:
 *   - NVMe hot path (PRP 빌드/doorbell ring/CQ 폴링)
 *   - lib/thread lockless ring 연산
 *   - lib/sock 및 NVMe-oF 네트워크 경로의 공유 버퍼 동기화
 *   - lib/env_dpdk의 DMA 메모리 배리어 경로
 * 공유 자료구조: 없음 — 순수 instruction-level 동기화 원시.
 * 데이터 흐름: 없음.
 *
 * === 주요 함수/구조체 요약 ===
 * 매크로만 제공:
 *   - spdk_compiler_barrier(): 컴파일러 재정렬 방지용 빈 inline asm clobber
 *   - spdk_rmb / wmb / mb:     장치 대상(MMIO/DMA) strong fence
 *   - spdk_smp_rmb / wmb / mb: SMP 코어 간 weak fence (x86에선 대부분 컴파일러 배리어로 충분)
 *   - spdk_ivdt_dcache(ptr):   해당 주소 캐시 라인 invalidate (ARM에서만 실제 명령 생성)
 *
 * 지원 아키텍처: PowerPC64, ARM64, x86/x64, RISC-V, LoongArch.
 * 지원하지 않는 아키텍처는 #error로 빌드 실패.
 */

#ifndef SPDK_BARRIER_H           /* [한국어] include 가드 시작 */
#define SPDK_BARRIER_H           /* [한국어] 가드 심볼 정의 */

#include "spdk/stdinc.h"         /* [한국어] 표준 include 묶음 — 이 파일은 표준 타입 없이도 동작하나 SPDK 공개 헤더 관례 */

#ifdef __cplusplus               /* [한국어] C++ 링크 규약 가드 — 매크로 자체는 규약 무관, 관례 유지 */
extern "C" {
#endif

/** Compiler memory barrier */
#define spdk_compiler_barrier() __asm volatile("" ::: "memory")
                                 /* [한국어] 컴파일러 전용 배리어
                                  *  - 빈 inline asm + "memory" clobber로 컴파일러에 "이 지점 전후로 메모리 읽기/쓰기를 재배치하지 말라" 지시
                                  *  - CPU 명령은 전혀 생성되지 않음 (런타임 비용 0)
                                  *  - 사용처: 컴파일러 옵티마이저가 공유 변수 접근을 레지스터 캐싱하거나 재정렬하는 것만 막으면 되는 경우
                                  *    예: 같은 코어 내 시그널 핸들러와의 가시성 */

/** Read memory barrier */
#define spdk_rmb()	_spdk_rmb()
                                 /* [한국어] 읽기 배리어 — 이 지점 이전의 모든 load가 이후 load보다 먼저 관찰되도록 보장
                                  *  - 장치(NVMe/HW)가 쓴 데이터를 읽을 때 순서 보장 필요 시 사용
                                  *  - 예: CQE의 phase 비트를 먼저 읽은 뒤에 payload를 읽어야 완료된 커맨드 데이터가 올바름
                                  *  - 내부적으로 아키텍처별 구현(_spdk_rmb)으로 디스패치 */
/** Write memory barrier */
#define spdk_wmb()	_spdk_wmb()
                                 /* [한국어] 쓰기 배리어 — 이 지점 이전의 모든 store가 이후 store보다 먼저 관찰되도록 보장
                                  *  - 장치가 읽기 전에 버퍼·디스크립터가 완전히 기록됐음을 확인해야 할 때 사용
                                  *  - 예: NVMe SQ 엔트리 전체 field를 write → wmb → tail doorbell write
                                  *    (wmb 없으면 doorbell 쓴 뒤 장치가 SQ를 읽을 때 반쪽 엔트리를 볼 수 있음) */
/** Full read/write memory barrier */
#define spdk_mb()	_spdk_mb()
                                 /* [한국어] 전체 배리어 — read+write 양방향 재정렬 모두 방지
                                  *  - rmb+wmb의 상위 집합. 더 비쌈 (x86: mfence, ARM: dsb sy)
                                  *  - 장치-메모리와의 강한 순서 보장 필요 시 사용 */

/** SMP read memory barrier. */
#define spdk_smp_rmb()	_spdk_smp_rmb()
                                 /* [한국어] SMP 코어 간 read 배리어
                                  *  - 다른 CPU 코어가 쓴 값을 읽을 때 순서 보장. 장치 대상이 아닌 메모리↔메모리 경로
                                  *  - x86은 강한 메모리 모델(TSO)이므로 컴파일러 배리어로 충분, ARM/PPC는 lwsync/dmb 등 실제 명령 필요
                                  *  - 사용처: lockless ring 소비자 측에서 producer 인덱스 읽기 */
/** SMP write memory barrier. */
#define spdk_smp_wmb()	_spdk_smp_wmb()
                                 /* [한국어] SMP 코어 간 write 배리어
                                  *  - 다른 CPU가 "먼저 데이터 슬롯 → 그 다음 인덱스 증가" 순서로 관찰하도록 보장
                                  *  - 사용처: lockless ring producer가 엔트리 write 후 producer head 업데이트 */
/** SMP read/write memory barrier. */
#define spdk_smp_mb()	_spdk_smp_mb()
                                 /* [한국어] SMP 전체 배리어 (양방향). 가장 비싼 smp 배리어 */

/** Invalidate data cache, input is data pointer */
#define spdk_ivdt_dcache(pdata)	_spdk_ivdt_dcache(pdata)
                                 /* [한국어] 해당 주소의 데이터 캐시 라인 invalidate
                                  *  - 비캐시 일관성(non-coherent) 영역을 장치가 DMA로 갱신한 경우, CPU가 stale cache를 읽지 않게 강제
                                  *  - ARMv8에서는 `dc civac` (clean + invalidate by VA, to PoC)으로 실제 명령 생성
                                  *  - x86/PPC/RISC-V/LoongArch는 cache coherent이므로 빈 매크로 */

#ifdef __PPC64__                 /* [한국어] PowerPC 64-bit 분기 — IBM POWER 아키텍처 */

#define _spdk_rmb()	__asm volatile("sync" ::: "memory")
                                 /* [한국어] POWER: `sync` — heavyweight sync, 장치/메모리 양방향 강한 순서 보장 */
#define _spdk_wmb()	__asm volatile("sync" ::: "memory")
                                 /* [한국어] POWER: 동일 sync 재사용. eieio보다 강하게 장치와의 순서까지 보장 */
#define _spdk_mb()	__asm volatile("sync" ::: "memory")
                                 /* [한국어] POWER 전체 배리어 — sync 하나로 충분 (POWER는 relaxed 모델이라 강한 배리어 필요) */
#define _spdk_smp_rmb()	__asm volatile("lwsync" ::: "memory")
                                 /* [한국어] POWER: `lwsync` — lightweight sync. 메모리↔메모리 순서만 보장(장치 제외) → SMP용으로 sync보다 저렴 */
#define _spdk_smp_wmb()	__asm volatile("lwsync" ::: "memory")
                                 /* [한국어] POWER SMP 쓰기 배리어 — lwsync로 코어 간 쓰기 순서 보장 */
#define _spdk_smp_mb()	spdk_mb()
                                 /* [한국어] POWER SMP 전체 배리어 — lwsync는 store-load 순서를 보장하지 못하므로 heavyweight sync 사용 */
#define _spdk_ivdt_dcache(pdata)
                                 /* [한국어] POWER는 cache coherent DMA가 일반적이므로 캐시 invalidate 불필요 */

#elif defined(__aarch64__)       /* [한국어] ARM64(AArch64) 분기 */

#define _spdk_rmb()	__asm volatile("dsb ld" ::: "memory")
                                 /* [한국어] ARM64: `dsb ld` — Data Synchronization Barrier, load 대상. 이전 모든 load 완료 보장 */
#define _spdk_wmb()	__asm volatile("dsb st" ::: "memory")
                                 /* [한국어] ARM64: `dsb st` — store 대상 DSB. MMIO/DMA 쓰기 순서 보장에 사용 */
#define _spdk_mb()	__asm volatile("dsb sy" ::: "memory")
                                 /* [한국어] ARM64: `dsb sy` — system scope 전체 DSB. 가장 강한 배리어 */
#define _spdk_smp_rmb()	__asm volatile("dmb ishld" ::: "memory")
                                 /* [한국어] ARM64: `dmb ishld` — Data Memory Barrier, Inner Shareable, Load
                                  *  - 내부 공유 도메인(동일 SoC 내 CPU 클러스터) 범위의 load-load 순서만 보장 → SMP용으로 dsb보다 저렴 */
#define _spdk_smp_wmb()	__asm volatile("dmb ishst" ::: "memory")
                                 /* [한국어] ARM64: `dmb ishst` — Inner Shareable, Store 대상 DMB */
#define _spdk_smp_mb()	__asm volatile("dmb ish" ::: "memory")
                                 /* [한국어] ARM64: `dmb ish` — Inner Shareable 전체 DMB (양방향) */
#define _spdk_ivdt_dcache(pdata)	asm volatile("dc civac, %0" : : "r"(pdata) : "memory");
                                 /* [한국어] ARM64: `dc civac, Xn` — Data cache Clean & Invalidate by VA to Point of Coherency
                                  *  - 해당 주소의 캐시 라인을 PoC까지 방출+무효화 → non-coherent DMA 영역에 필수 */

#elif defined(__i386__) || defined(__x86_64__)
                                 /* [한국어] x86 / x86_64 분기 — 강한 메모리 모델(TSO) 덕분에 SMP 배리어는 대부분 컴파일러 힌트로 충분 */

#define _spdk_rmb()	__asm volatile("lfence" ::: "memory")
                                 /* [한국어] x86: `lfence` — load fence. 장치/WC 메모리 대상 로드 순서 보장
                                  *  - TSO는 load-load 재정렬 자체가 드물지만, NT load나 WC MMIO에서 필요 */
#define _spdk_wmb()	__asm volatile("sfence" ::: "memory")
                                 /* [한국어] x86: `sfence` — store fence. NT store/WC 버퍼 플러시와 쓰기 순서 보장
                                  *  - MMIO doorbell 쓰기 전 SQ 엔트리 flush 용도로 NVMe 드라이버에서 필수 */
#define _spdk_mb()	__asm volatile("mfence" ::: "memory")
                                 /* [한국어] x86: `mfence` — memory fence. 전체 load/store 순서 보장 (가장 비쌈) */
#define _spdk_smp_rmb()	spdk_compiler_barrier()
                                 /* [한국어] x86 TSO: load-load 재정렬 없음 → 컴파일러 배리어로 충분 (CPU 명령 0) */
#define _spdk_smp_wmb()	spdk_compiler_barrier()
                                 /* [한국어] x86 TSO: store-store 재정렬 없음 → 컴파일러 배리어로 충분 */
#if defined(__x86_64__)
#define _spdk_smp_mb()	__asm volatile("lock addl $0, -128(%%rsp); " ::: "memory");
                                 /* [한국어] x86_64 smp_mb 관용구: mfence 대신 `lock addl $0, -128(%rsp)` 사용
                                  *  - 이유: mfence는 SFENCE+LFENCE 합쳐진 비싼 명령. lock-prefixed RMW는 full fence 효과이며 더 저렴
                                  *  - -128(%rsp)는 red zone(System V AMD64 ABI의 128바이트 preserved) 내부 주소 → 다른 데이터 훼손 없음
                                  *  - 같은 주소에 +0 더하기라 값은 불변, 순수 memory ordering 효과만 */
#elif defined(__i386__)
#define _spdk_smp_mb()	__asm volatile("lock addl $0, -128(%%esp); " ::: "memory");
                                 /* [한국어] x86 32bit 버전. %esp 기반 주소. 32bit ABI에는 red zone이 없지만 -128은 관례적으로 안전 영역 */
#endif
#define _spdk_ivdt_dcache(pdata)
                                 /* [한국어] x86은 cache coherent DMA → invalidate 불필요 */

#elif defined(__riscv)           /* [한국어] RISC-V 분기 */

#define _spdk_rmb()	__asm__ __volatile__("fence ir, ir" ::: "memory")
                                 /* [한국어] RISC-V: `fence ir, ir` — 이전의 input(device read)+read가 이후 input+read보다 앞 순서 보장
                                  *  - ir = I/O read + Mem read, 장치 I/O까지 포함하는 강한 read 배리어 */
#define _spdk_wmb()	__asm__ __volatile__("fence ow, ow" ::: "memory")
                                 /* [한국어] RISC-V: `fence ow, ow` — I/O output + Memory write 순서 보장 (MMIO 쓰기 포함) */
#define _spdk_mb()	__asm__ __volatile__("fence iorw, iorw" ::: "memory")
                                 /* [한국어] RISC-V: `fence iorw, iorw` — I/O + memory의 read+write 전방향 배리어 (최강) */
#define _spdk_smp_rmb()	__asm__ __volatile__("fence r, r" ::: "memory")
                                 /* [한국어] RISC-V SMP: 메모리만 대상 read-read 배리어 */
#define _spdk_smp_wmb()	__asm__ __volatile__("fence w, w" ::: "memory")
                                 /* [한국어] RISC-V SMP: 메모리만 대상 write-write 배리어 */
#define _spdk_smp_mb()	__asm__ __volatile__("fence rw, rw" ::: "memory")
                                 /* [한국어] RISC-V SMP: 메모리만 대상 rw 전방향 배리어 */
#define _spdk_ivdt_dcache(pdata)
                                 /* [한국어] RISC-V는 대부분 cache coherent, 여기서는 무동작 */

#elif defined(__loongarch__)     /* [한국어] LoongArch 분기 */

#define _spdk_rmb()	__asm volatile("dbar 0" ::: "memory")
                                 /* [한국어] LoongArch: `dbar 0` — Data Barrier, hint 0 = 완전한 순서 보장. 세분화 hint(lwsync류)는 CPU별 지원 편차로 0 고정 */
#define _spdk_wmb()	__asm volatile("dbar 0" ::: "memory")
                                 /* [한국어] LoongArch 쓰기 배리어 (동일 dbar 0) */
#define _spdk_mb()	__asm volatile("dbar 0" ::: "memory")
                                 /* [한국어] LoongArch 전체 배리어 */
#define _spdk_smp_rmb()	__asm volatile("dbar 0" ::: "memory")
                                 /* [한국어] LoongArch SMP — 같은 dbar 0 사용 */
#define _spdk_smp_wmb()	__asm volatile("dbar 0" ::: "memory")
#define _spdk_smp_mb()	__asm volatile("dbar 0" ::: "memory")
#define _spdk_ivdt_dcache(pdata)
                                 /* [한국어] LoongArch 캐시 coherent 가정 → invalidate 무동작 */

#else                            /* [한국어] 지원하지 않는 아키텍처 — 빈 매크로 선언 후 빌드 실패 처리
                                  *  - 빈 정의는 #error 직전이라 실제로는 도달 못함. 단, 에디터/정적 분석기가 매크로 미정의로 오류 내는 것 방지 차원 */

#define _spdk_rmb()
#define _spdk_wmb()
#define _spdk_mb()
#define _spdk_smp_rmb()
#define _spdk_smp_wmb()
#define _spdk_smp_mb()
#define _spdk_ivdt_dcache(pdata)
#error Unknown architecture       /* [한국어] 포팅되지 않은 아키텍처에서 빌드 중단 — 새 아키텍처 추가 시 여기에 case 추가 필수 */

#endif

#ifdef __cplusplus               /* [한국어] C++ 가드 닫기 */
}
#endif

#endif                           /* [한국어] include 가드 종료 */
