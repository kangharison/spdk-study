/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   Copyright (c) NetApp, Inc.
 *   Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/** \file
 * Encapsulated third-party dependencies
 */

/*
 * [한국어 설명] SPDK 환경 추상화 공개 헤더 (env.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK가 의존하는 "환경(env) 계층"의 모든 공개 API를 한 곳에 모은 진입점이다.
 * SPDK는 유저스페이스에서 NVMe/블록 디바이스를 polled-mode로 구동하기 위해 hugepage 기반
 * DMA 안전 메모리, NUMA-aware mempool/ring, PCI 디바이스 enumerate/attach, vtophys 변환,
 * 락이 없는 lcore 스레딩 등 OS/배포판이 직접 제공하지 않는 저수준 기능이 필요하다.
 * 이 파일은 그 기능들을 SPDK 상위 모듈(NVMe 드라이버, bdev, thread 라이브러리 등)에서
 * 구현체에 무관하게 호출할 수 있도록 추상화한다. 실제 구현은 lib/env_dpdk/(DPDK 백엔드)에
 * 있으나 헤더 자체는 백엔드 독립적이며, 다른 환경(예: SPDK no-DPDK 빌드)으로 교체 가능하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 전체 I/O 스택의 가장 아래(OS와 SPDK의 경계)에 위치한다. 호출 흐름은 다음과 같다:
 *   [Application/main] → spdk_env_opts_init/spdk_env_init (DPDK EAL 초기화 + hugepage·IOMMU 셋업)
 *     → spdk_app_start / spdk_thread_lib_init → 각 코어별 reactor 가동
 *     → NVMe 드라이버, bdev 모듈, 사용자 코드는 spdk_malloc/spdk_dma_zmalloc로 DMA 버퍼 확보
 *     → spdk_pci_enumerate(driver) 로 NVMe/IOAT/IDXD/VMD/Virtio PCI 디바이스 탐색·attach
 *     → spdk_vtophys/spdk_mem_map_translate 로 VA → IOVA 변환 (NVMe PRP/SGL 작성에 사용)
 *     → spdk_mempool/spdk_ring 으로 lockless 객체 풀과 메시지 큐 운용 (bdev_io, NVMe req 등)
 * 즉 이 파일은 "SPDK 런타임의 모든 다른 라이브러리가 의지하는 기반 layer"의 공개 계약이다.
 * 실행 컨텍스트는 호스트 유저스페이스이며, DPDK 의 EAL 초기화 시에만 시작 시점이고
 * 이후 전체 lifetime 동안 reactor/poller 들이 이 API들을 빈번히 호출한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h(표준 헤더 묶음), spdk/queue.h(BSD TAILQ 매크로 — spdk_pci_device 의 internal.tailq),
 *   spdk/pci_ids.h(SPDK_PCI_CLASS_ANY_ID 등 매크로), spdk/assert.h(SPDK_STATIC_ASSERT 로
 *   spdk_env_opts 크기를 ABI 안정화 검증).
 * - 구현체: lib/env_dpdk/* (DPDK 18.x+ 의 rte_eal_init/rte_malloc/rte_memzone/rte_mempool/
 *   rte_ring/rte_pci_*/rte_vfio API 위에 SPDK 의 얇은 래퍼를 얹음). DPDK 의 hugepage(2MB/1GB),
 *   IOMMU(VFIO 기반 IOVA), NUMA 지원이 그대로 이 헤더의 사양으로 노출된다.
 * - 사용자: lib/nvme/(qpair·SQ·CQ·PRP 작성에 spdk_vtophys, spdk_pci_enumerate),
 *   lib/bdev/(bdev_io 풀에 spdk_mempool), lib/thread/(코어별 spdk_ring 메시지 큐),
 *   lib/env_dpdk_rpc, app/spdk_tgt 등 거의 모든 SPDK 모듈.
 * - 데이터 흐름: 사용자 코드가 numa_id/flags 를 지정 → env 가 hugepage 위에 핀된 메모리 반환 →
 *   spdk_vtophys 로 IOVA 획득 → 그 IOVA 가 NVMe SQE 의 PRP1/PRP2 또는 SGL 엔트리로 들어가
 *   하드웨어 DMA 엔진이 직접 접근. 즉 "환경 계층이 발급한 메모리만이 DMA 가능"하다.
 * - 공유 자료구조: spdk_pci_device 는 등록된 PCI 디바이스 글로벌 TAILQ 의 노드이고,
 *   spdk_mem_map 은 vtophys 가 사용하는 VA→IOVA 페이지 테이블의 추상화이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_env_opts / spdk_env_opts_init / spdk_env_init / spdk_env_fini:
 *   DPDK EAL 초기화·종료. core_mask, mem_size, pci_allowed/blocked, iova_mode 등 부팅 옵션.
 * - spdk_malloc / spdk_zmalloc / spdk_realloc / spdk_free + spdk_dma_*:
 *   hugepage 기반 DMA 안전 메모리 할당 (NUMA-aware, cacheline-aligned, IOVA contiguous).
 * - spdk_memzone_reserve/lookup/free/dump: 프로세스 간 공유 가능한 named 메모리 영역.
 * - spdk_mempool_*: 코어별 캐시를 갖는 lockless 객체 풀 — bdev_io/NVMe req 등 핫패스 객체에 사용.
 * - spdk_ring (SP_SC/MP_SC/MP_MC) / enqueue / dequeue: lockless ring buffer (DPDK rte_ring 기반).
 *   reactor 간 메시지 전달 (spdk_thread_send_msg) 의 기반.
 * - spdk_env_get_*_core / spdk_env_get_*_numa_id / SPDK_ENV_FOREACH_CORE: 코어/NUMA 토폴로지 조회.
 * - spdk_vtophys / spdk_mem_map_*: 가상주소 → IOVA(VFIO 환경) 또는 물리주소 변환 — DMA 핵심.
 * - spdk_pci_driver_register / spdk_pci_enumerate / spdk_pci_device_*:
 *   PCI 드라이버 등록, BDF 탐색·attach, BAR 매핑(MMIO doorbell 접근), config space R/W,
 *   MSI-X 인터럽트 efd 발급 (interrupt-mode polled 보완용).
 * - spdk_pci_event_listen / spdk_pci_get_event: 핫플러그(uevent) 감시.
 * - 핵심 구조체:
 *   * spdk_env_opts — env_init 인자 (128 바이트 고정, 향후 확장은 끝쪽에 추가).
 *   * spdk_pci_addr / spdk_pci_id — PCI BDF 와 vendor/device/subsystem ID.
 *   * spdk_pci_device — 디바이스 핸들 + map_bar/cfg_read/write 함수 포인터 + 내부 상태.
 *   * spdk_pci_device_provider — 디바이스 attach/detach 백엔드 등록 (env_dpdk vs custom).
 *   * spdk_mem_map / spdk_mem_map_ops — VA→IOVA 변환 테이블의 일반화 추상.
 */

#ifndef SPDK_ENV_H
#define SPDK_ENV_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 가 정의한 표준 라이브러리 헤더 묶음 (stdint, stddef, stdbool, string 등).
 * 플랫폼별 차이를 흡수하고 SPDK 코드 전체에서 일관된 기본 타입(uint32_t, size_t, bool 등)을
 * 제공한다. env.h 의 모든 함수 시그니처가 이 타입들을 사용하므로 가장 먼저 포함되어야 한다. */
#include "spdk/queue.h"
/* [한국어] BSD 스타일 TAILQ/LIST 매크로 모음. 아래 spdk_pci_device 내부의
 * TAILQ_ENTRY(spdk_pci_device) 와 spdk_pci_device_provider 의 TAILQ_ENTRY 노드 정의에
 * 사용된다. SPDK 는 lockless TAILQ 를 광범위하게 활용하여 PCI 디바이스 리스트, mempool/ring
 * 글로벌 리스트 등을 관리한다. */
#include "spdk/pci_ids.h"
/* [한국어] PCI vendor/device/class ID 와 SPDK_PCI_CLASS_ANY_ID/SPDK_PCI_ANY_ID 매크로 정의.
 * spdk_pci_id 와 SPDK_PCI_DEVICE() 매크로 (드라이버 ID 테이블 작성용) 가 이 매크로들을 참조. */
#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로. 아래에서 sizeof(spdk_env_opts) == 128 을 컴파일 타임에
 * 강제하여 ABI/필드 구성이 의도와 어긋나면 빌드 실패하게 만든다 (out-of-tree consumer 보호). */

#ifdef __cplusplus
extern "C" {
/* [한국어] 이 헤더가 C++ 코드에서 #include 될 때 SPDK API 의 심볼이 C 링키지로 선언되도록
 * 강제. SPDK 는 C 로 작성되었으나 application/QEMU/RPC 코드 등에서 C++ 로 호출되는 경우가
 * 있어 name mangling 을 막아야 한다. 파일 끝의 #endif 와 짝을 이룬다. */
#endif

/* SPDK_ENV_NUMA_ID_ANY and SPDK_ENV_SOCKET_ID_ANY mean the same thing.
 * SOCKET_ID naming was inherited from DPDK, but the preferred use is
 * NUMA_ID to avoid confusion with TCP sockets. But keep the old #define
 * around since there is likely a lot of out-of-tree code using it.
 */
/* [한국어] "특정 NUMA 노드를 지정하지 않음 (어디든 OK)" 을 의미하는 sentinel.
 * spdk_malloc/spdk_memzone_reserve/spdk_mempool_create 등 numa_id 인자에 -1 을 넘기면
 * 환경 계층이 가장 적절한 NUMA 노드(일반적으로 호출 코어가 속한 노드)를 자동 선택한다.
 * 값이 -1 인 이유: NUMA 노드 ID 는 비음수이며, 음수 값은 "지정 안 함"의 관례. */
#define SPDK_ENV_NUMA_ID_ANY	(-1)
/* [한국어] DPDK 가 NUMA 노드를 socket 이라 부르던 레거시 명칭. SPDK_ENV_NUMA_ID_ANY 와
 * 동일한 값(-1)이며 out-of-tree 코드 호환을 위해 유지된다. 새 코드는 NUMA_ID 쓰기를 권장. */
#define SPDK_ENV_SOCKET_ID_ANY	(-1)

/* [한국어] "어떤 lcore 도 아님 / 현재 스레드는 SPDK lcore 가 아님" 을 의미하는 sentinel.
 * spdk_env_get_current_core() 가 SPDK 환경이 만든 스레드가 아닌 곳에서 호출되면 이 값을 반환.
 * UINT32_MAX 인 이유: 코어 ID 는 0..N 의 작은 값이고, UINT32_MAX 는 절대 유효 코어가 아님. */
#define SPDK_ENV_LCORE_ID_ANY	(UINT32_MAX)

/**
 * Memory is dma-safe.
 */
/* [한국어] spdk_malloc/zmalloc 의 flags 인자로 전달되는 비트.
 * 의미: 반환된 메모리가 DMA 안전 — 즉 hugepage 기반이며 핀(pin)되어 있어
 * NIC/NVMe 의 DMA 엔진이 spdk_vtophys 로 얻은 IOVA 로 직접 접근 가능.
 * 미설정 시: 일반 malloc 과 동등 (DMA 사용 불가). NVMe PRP/SGL 에 들어갈 버퍼는 반드시 이 플래그 필요. */
#define SPDK_MALLOC_DMA    0x01

/**
 * Memory is sharable across process boundaries.
 */
/* [한국어] DPDK multi-process 모델에서 primary/secondary 프로세스 간에 공유 가능한 메모리.
 * 보통 vhost-user 처럼 여러 프로세스가 같은 hugepage 를 mmap 해야 할 때 사용.
 * SPDK_MALLOC_DMA 와 함께 OR 가능. 미사용 시 단일 프로세스 내에서만 유효. */
#define SPDK_MALLOC_SHARE  0x02

/* [한국어] memzone 이름 최대 길이 (NULL 포함). DPDK rte_memzone 의 RTE_MEMZONE_NAMESIZE 와
 * 일치해야 하며, 더 길면 spdk_memzone_reserve() 가 실패. */
#define SPDK_MAX_MEMZONE_NAME_LEN 32
/* [한국어] mempool 이름 최대 길이. DPDK rte_mempool 이 내부 prefix("MP_" 등)를 붙이기 위해
 * 32 보다 작은 29 로 제한된다. */
#define SPDK_MAX_MEMPOOL_NAME_LEN 29

/**
 * Memzone flags
 */
/* [한국어] memzone 예약 시 IOVA 연속성 요구를 끄는 플래그. 기본값(플래그 없음)은
 * "물리/IOVA 가 연속" 보장이지만, 큰 영역(예: GB 단위)을 잡을 때 hugepage 가 부족해
 * 실패할 수 있어 이 플래그로 비연속 허용을 지시할 수 있다. NVMe PRP 처럼 IOVA 연속성이
 * 필요한 영역이 아니라면 사용 가능. 값 0x00100000 은 DPDK RTE_MEMZONE_IOVA_CONTIG 와
 * 의도가 반대인 비트로 매핑된다. */
#define SPDK_MEMZONE_NO_IOVA_CONTIG 0x00100000 /**< no iova contiguity */

/**
 * \brief Environment initialization options
 */
/* [한국어] spdk_env_init() 에 전달되는 부팅 옵션 구조체. DPDK EAL 의 argv 인자들을
 * 정형화한 형태로, 사용자는 spdk_env_opts_init() 으로 기본값을 채운 뒤 필요한 필드만
 * 덮어쓰고 spdk_env_init() 에 전달한다. 크기는 SPDK_STATIC_ASSERT 로 128 바이트 고정 —
 * 새 필드는 끝(reserved2 자리)부터 추가하여 ABI 호환을 지킨다. */
struct spdk_env_opts {
	const char		*name;
	/* [한국어] DPDK EAL 의 program name (argv[0]).
	 * 설정자: spdk_env_opts_init 이 "spdk" 기본값을 넣음. 사용자 코드에서 덮어쓸 수 있음.
	 * 읽는 자: DPDK 가 hugepage 파일 prefix 와 syslog 식별자 등으로 사용.
	 * 값 범위: NULL 불가의 짧은 ASCII 문자열. 멀티 SPDK 프로세스 공존 시 서로 달라야 함.
	 * 동기화: env_init 호출 전에만 변경, 이후 read-only. */

	const char		*core_mask;
	/* [한국어] 사용할 CPU 코어 마스크 (16진수 비트마스크 문자열 또는 "0x..." 형식).
	 * 설정자: 사용자 (CLI 인자 -m 과 매핑). 미설정 시 NULL — DPDK 는 코어 0만 사용.
	 * 읽는 자: env_dpdk 가 DPDK rte_eal_init 의 -c 인자로 전달.
	 * 값 범위: "0x3" 처럼 hex 문자열. lcore_map 과 상호 배타.
	 * 동기화: env_init 이전에만 유효. */

	const char		*lcore_map;
	/* [한국어] lcore → 물리 코어 매핑 문자열 (DPDK --lcores 인자, 예: "0@1,1@2").
	 * 설정자: 사용자. 미설정 시 NULL.
	 * 읽는 자: env_dpdk 가 DPDK 에 그대로 전달. core_mask 와 둘 중 하나만 사용해야 함.
	 * 값 범위: DPDK lcore-map 문법 문자열. */

	int			shm_id;
	/* [한국어] DPDK multi-process shm ID. primary/secondary 프로세스가 같은 ID 를 공유해야
	 * 같은 hugepage segment 를 보게 된다. 단일 프로세스라면 -1.
	 * 설정자: 사용자. 읽는 자: rte_eal_init -shm_id. */

	int			mem_channel;
	/* [한국어] 메모리 채널 수 (DDR 채널 인터리빙 힌트, DPDK -n 인자).
	 * 설정자: 사용자. 미설정 시 -1 (DPDK 가 하드웨어에서 자동 감지).
	 * 값 범위: 보통 1~4 (서버 RAM 채널 수). */

	int			main_core;
	/* [한국어] DPDK main lcore (= "초기화 스레드"가 매핑될 코어).
	 * 설정자: 사용자. 미설정 시 -1 → core_mask 의 가장 낮은 비트가 자동 선택됨.
	 * 읽는 자: rte_eal_init --main-lcore. spdk_env_get_main_core() 로 런타임 조회. */

	int			mem_size;
	/* [한국어] 예약할 hugepage 메모리 총량 (MiB). -1 = 가능한 만큼 모두 사용.
	 * 설정자: 사용자. 읽는 자: rte_eal_init -m.
	 * 값 범위: -1 또는 양수 MiB. 시스템에 마련된 hugepage 보다 크게 요청하면 init 실패. */

	bool			no_pci;
	/* [한국어] true 면 DPDK 가 PCI 버스 enumerate 를 건너뜀 — NVMe 등 PCI 디바이스를 쓰지 않는
	 * 순수 메모리 풀 용도(예: 단위 테스트, blobstore-only)에서 부팅 가속을 위해 사용.
	 * 설정자: 사용자. 읽는 자: env_dpdk 가 --no-pci 옵션으로 변환. */

	bool			hugepage_single_segments;
	/* [한국어] true 면 각 hugepage 를 단일 segment 로 다룸 (DPDK --single-file-segments).
	 * 설정자: 사용자. 읽는 자: env_dpdk. multi-process 공유나 IOMMU 디버깅에서 유용. */

	bool			unlink_hugepage;
	/* [한국어] true 면 init 종료 시 hugepage backing file 을 unlink 하여 디스크 잔재 제거.
	 * 설정자: 사용자. 잘못 종료된 이전 run 의 hugepage 가 남아있을 때 청소 용도. */

	bool			no_huge;
	/* [한국어] true 면 hugepage 미사용 — DPDK 의 --no-huge. 일반 anonymous 메모리로 동작하므로
	 * 실제 DMA 는 불가하나 단위 테스트/CI 환경에서 hugepage 가 없을 때 동작 가능.
	 * 설정자: 사용자. 핵심 운영(NVMe IO) 에는 부적합. */

	uint32_t		reserved;
	/* [한국어] 정렬용 패딩 + 향후 확장 예약. 현재 의미 없음.
	 * 설정자/읽는 자: 없음 (touched 시 ABI 깨짐 가능). 0 으로 유지. */

	size_t			num_pci_addr;
	/* [한국어] pci_blocked / pci_allowed 배열의 엔트리 개수.
	 * 설정자: 사용자가 두 배열을 동시에 설정. 읽는 자: env_dpdk PCI enumerate 단계.
	 * 값 범위: 0 = 제약 없음. */

	const char		*hugedir;
	/* [한국어] hugepage 마운트 디렉토리 (예: "/dev/hugepages"). NULL 이면 DPDK 자동 탐색.
	 * 설정자: 사용자. 읽는 자: rte_eal_init --huge-dir. */

	struct spdk_pci_addr	*pci_blocked;
	/* [한국어] enumerate 에서 제외할 PCI 디바이스 BDF 배열 (블록리스트).
	 * 설정자: 사용자. 길이는 num_pci_addr. 읽는 자: env_dpdk 가 -b 옵션으로 변환.
	 * pci_allowed 와 상호 배타 — 동시 설정 시 init 실패. */

	struct spdk_pci_addr	*pci_allowed;
	/* [한국어] enumerate 에서 허용할 PCI 디바이스 BDF 배열 (허용리스트).
	 * 설정자: 사용자. 읽는 자: env_dpdk -a 옵션. 비어있으면 모두 허용. */

	const char		*iova_mode;
	/* [한국어] IOVA 모드 강제 — "pa"(physical address) 또는 "va"(virtual address).
	 * NULL 이면 DPDK 가 IOMMU 가용성 보고 자동 선택.
	 * "va" = VFIO/IOMMU 사용 (안전·고성능 권장). "pa" = 직접 물리주소 (CAP_SYS_ADMIN 필요).
	 * 설정자: 사용자. 읽는 자: rte_eal_init --iova-mode. */

	uint64_t		base_virtaddr;
	/* [한국어] DPDK 가 hugepage 매핑을 시작할 가상주소 hint (--base-virtaddr).
	 * 설정자: 사용자. 0 이면 DPDK 자동 선택. 보통 multi-process 에서 매핑 주소 일치 목적. */

	/** Opaque context for use of the env implementation. */
	void			*env_context;
	/* [한국어] 환경 백엔드(env_dpdk 등) 가 자유롭게 쓰는 불투명 포인터. SPDK 코어는 건드리지 않음.
	 * 설정자/읽는 자: 백엔드 내부. 외부 사용자는 NULL 로 둘 것. */

	const char		*vf_token;
	/* [한국어] SR-IOV VF 사용 시 VFIO 인증 UUID (rte_eal_init --vfio-vf-token).
	 * 설정자: 사용자 (PF/VF 동일 토큰). 읽는 자: env_dpdk. NULL 이면 미사용. */

	size_t			opts_size;
	/* [한국어] 호출자가 컴파일된 시점에 알고 있던 spdk_env_opts 의 크기.
	 * 설정자: spdk_env_opts_init 이 sizeof(*opts) 로 채움.
	 * 읽는 자: spdk_env_init 이 이 값을 보고 신구 ABI 를 판별 — 새 필드는 opts_size 로
	 *   "옛 호출자에는 없는 영역" 을 안전하게 인식. ABI 진화 패턴 (Linux struct versioning). */

	bool			enforce_numa;
	/* [한국어] true 면 SPDK 가 NUMA-local 메모리만 쓰도록 강제 (cross-NUMA 할당 실패시 에러).
	 * 설정자: 사용자. 읽는 자: env_dpdk 가 numa-aware 할당 정책에 반영. */

	uint8_t			reserved2[7];
	/* [한국어] enforce_numa(1) 정렬 + 향후 새 bool/byte 필드 슬롯. 0 으로 유지.
	 * 새 필드 추가 시 이 영역을 갉아먹는 식으로 사용 — sizeof 128 유지가 강제 사항. */

	/* All new fields must be added at the end of this structure. */
	/* [한국어] ABI 호환성: 기존 필드 이동/제거 금지, 새 필드는 반드시 맨 끝에 추가하고
	 * opts_size 로 존재 여부를 식별. 그렇지 않으면 옛 바이너리/라이브러리가 깨진다. */
};
/* [한국어] 컴파일 타임 어서션: spdk_env_opts 가 정확히 128 바이트여야 함.
 * 누군가 필드를 잘못 추가/이동/제거하면 빌드가 멈춰 ABI 사고를 막는다.
 * SPDK_STATIC_ASSERT 매크로는 spdk/assert.h 에 정의되어 있고 _Static_assert 로 확장된다. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_env_opts) == 128, "Incorrect size");

/**
 * Allocate dma/sharable memory based on a given dma_flg. It is a memory buffer
 * with the given size, alignment and socket id.
 *
 * \param size Size in bytes.
 * \param align If non-zero, the allocated buffer is aligned to a multiple of
 * align. In this case, it must be a power of two. The returned buffer is always
 * aligned to at least cache line size.
 * \param unused **Invalid**. If not a NULL, the function will fail and return NULL.
 * \param numa_id NUMA node ID to allocate memory on, or SPDK_ENV_NUMA_ID_ANY
 * for any NUMA node.
 * \param flags Combination of SPDK_MALLOC flags (\ref SPDK_MALLOC_DMA, \ref SPDK_MALLOC_SHARE).
 * At least one flag must be specified.
 *
 * \return a pointer to the allocated memory buffer.
 */
/*
 * [한국어]
 * spdk_malloc - hugepage 기반 DMA/공유 가능 메모리 할당의 가장 일반적인 진입점
 *
 * @param size: 요청 바이트 수.
 * @param align: 정렬 (0=캐시라인 자동, 그 외엔 2의 거듭제곱).
 * @param unused: 호환성용 슬롯. NULL 이어야 함 — 비-NULL 이면 NULL 반환 (구버전 IOVA out-param 자리).
 * @param numa_id: NUMA 노드 (SPDK_ENV_NUMA_ID_ANY = 자동).
 * @param flags: SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE 조합. 최소 한 비트 필요.
 * @return: 성공 시 hugepage 기반 가상주소, 실패 시 NULL.
 *
 * NVMe SQE 의 PRP/SGL 에 들어갈 모든 사용자 데이터/메타 버퍼는 반드시 이 API 로 할당해야 한다.
 * 일반 malloc 은 swappable 이라 DMA 불가 — hugepage 위에 핀된 메모리만 IOVA 가 안정적이다.
 * 동작: env_dpdk 백엔드는 rte_malloc_socket 으로 hugepage memseg 에서 잘라 반환한다.
 * 컨텍스트: 어느 SPDK thread 에서도 호출 가능하지만 핫패스에서는 미리 mempool 로 풀링 권장.
 * caller: NVMe 드라이버, bdev_io 페이로드 준비 코드, 사용자 코드.
 * callee: env_dpdk → rte_malloc_socket / posix_memalign + mlock (no_huge 모드).
 *
 * 호출 체인: 사용자/NVMe 드라이버 → spdk_malloc → rte_malloc_socket → hugepage segment 컷
 */
void *spdk_malloc(size_t size, size_t align, uint64_t *unused, int numa_id, uint32_t flags);

/**
 * Allocate dma/sharable memory based on a given dma_flg. It is a memory buffer
 * with the given size, alignment and socket id. Also, the buffer will be zeroed.
 *
 * \param size Size in bytes.
 * \param align If non-zero, the allocated buffer is aligned to a multiple of
 * align. In this case, it must be a power of two. The returned buffer is always
 * aligned to at least cache line size.
 * \param unused **Invalid**. If not a NULL, the function will fail and return NULL.
 * \param numa_id NUMA node ID to allocate memory on, or SPDK_ENV_NUMA_ID_ANY
 * for any NUMA node.
 * \param flags Combination of SPDK_MALLOC flags (\ref SPDK_MALLOC_DMA, \ref SPDK_MALLOC_SHARE).
 *
 * \return a pointer to the allocated memory buffer.
 */
/*
 * [한국어]
 * spdk_zmalloc - spdk_malloc + memset(0). 주로 NVMe Identify 응답·페이로드 버퍼처럼
 *                초기 상태가 0 이어야 하는 경우 사용.
 *
 * @param size/align/unused/numa_id/flags: spdk_malloc 과 동일.
 * @return: 0 으로 채워진 hugepage 메모리 포인터 (실패 시 NULL).
 *
 * 동작: 내부적으로 spdk_malloc 후 memset 0 — 큰 영역에서는 페이지 폴트 비용이 들 수 있어
 *       핫패스 반복 할당에는 mempool 사용을 권장.
 * 컨텍스트: 어디서나 호출 가능. 주로 init/setup 단계.
 *
 * 호출 체인: 사용자 init 코드 → spdk_zmalloc → rte_zmalloc_socket
 */
void *spdk_zmalloc(size_t size, size_t align, uint64_t *unused, int numa_id, uint32_t flags);

/**
 * Resize a dma/sharable memory buffer with the given new size and alignment.
 * Existing contents are preserved.
 *
 * \param buf Buffer to resize.
 * \param size Size in bytes.
 * \param align If non-zero, the allocated buffer is aligned to a multiple of
 * align. In this case, it must be a power of two. The returned buffer is always
 * aligned to at least cache line size.
 *
 * \return a pointer to the resized memory buffer.
 */
/*
 * [한국어]
 * spdk_realloc - hugepage 버퍼 크기 변경 (기존 내용 유지)
 *
 * @param buf: spdk_malloc/zmalloc 으로 받은 버퍼 (NULL 이면 신규 할당과 동일).
 * @param size: 새 크기. 0 이면 free.
 * @param align: 새 정렬. 0 = 캐시라인.
 * @return: 새 (또는 같은) 가상주소 — 옛 buf 는 무효화될 수 있음.
 *
 * NVMe DMA 가 진행 중인 버퍼에 호출하면 IOVA 가 바뀔 수 있어 사고가 난다 — DMA 정지 후 호출.
 * 동작: env_dpdk 의 rte_realloc_socket 위에서 동작. 새 영역으로 옮겨지면 기존 IOVA 매핑 무효.
 *
 * 호출 체인: 사용자 코드 → spdk_realloc → rte_realloc_socket
 */
void *spdk_realloc(void *buf, size_t size, size_t align);

/**
 * Free buffer memory that was previously allocated with spdk_malloc() or spdk_zmalloc().
 *
 * \param buf Buffer to free.
 */
/*
 * [한국어]
 * spdk_free - spdk_malloc/zmalloc/realloc 가 반환한 버퍼 해제
 *
 * @param buf: 해제할 버퍼 (NULL 허용 — no-op).
 *
 * 일반 free() 와 절대 섞어 호출하면 안 됨. spdk_dma_free 와는 동등(둘 다 같은 백엔드 호출).
 * DMA in-flight 인 버퍼에 호출 금지 — 완료 후 해제.
 *
 * 호출 체인: 사용자/드라이버 cleanup → spdk_free → rte_free
 */
void spdk_free(void *buf);

/**
 * Initialize the default value of opts.
 *
 * \param opts Data structure where SPDK will initialize the default options.
 */
/*
 * [한국어]
 * spdk_env_opts_init - spdk_env_opts 를 안전한 기본값으로 채움
 *
 * @param opts: 사용자가 스택/힙에 잡은 빈 구조체 포인터 (NULL 불가).
 *
 * 사용자는 이 함수를 먼저 호출해 모든 필드를 디폴트로 채운 뒤, 자신이 바꾸고 싶은 필드만
 * 덮어쓰고 spdk_env_init 에 전달해야 한다 — 그래야 향후 새 필드가 추가되어도 ABI 안전.
 * 동작: name="spdk", core_mask=NULL, mem_size=-1, no_pci=false, opts_size=sizeof(*opts) 등.
 * 컨텍스트: 프로세스 시작 시 한 번. 멀티스레드 보호 불필요.
 *
 * 호출 체인: main → spdk_env_opts_init → (사용자 필드 수정) → spdk_env_init
 */
void spdk_env_opts_init(struct spdk_env_opts *opts);

/**
 * Initialize or reinitialize the environment library.
 * For initialization, this must be called prior to using any other functions
 * in this library. For reinitialization, the parameter `opts` must be set to
 * NULL and this must be called after the environment library was finished by
 * spdk_env_fini() within the same process.
 *
 * \param opts Environment initialization options.
 * \return 0 on success, or negative errno on failure.
 */
/*
 * [한국어]
 * spdk_env_init - DPDK EAL 초기화 + hugepage/IOMMU/PCI 셋업
 *
 * @param opts: spdk_env_opts_init 으로 채운 옵션. 재초기화 시 NULL 가능 (이전 opts 재사용).
 * @return: 0 = 성공, 음수 errno = 실패 (hugepage 부족, VFIO 권한, 충돌 shm_id 등).
 *
 * SPDK 의 모든 env_*/malloc/mempool/PCI/vtophys API 는 이 함수 호출 후에만 사용 가능.
 * 동작 단계: opts → DPDK argv 변환 → rte_eal_init (hugepage mmap, lcore pthread 생성,
 *   IOVA 모드 결정, IOMMU/VFIO 컨테이너 셋업) → SPDK 내부 mem_map/PCI 글로벌 리스트 초기화.
 * 컨텍스트: 프로세스 시작 시 메인 스레드에서 단 1회. 재진입 불가.
 * caller: spdk_app_start (앱 부팅) 또는 사용자 main.
 * callee: env_dpdk 의 rte_eal_init, vfio_setup_dma 등.
 *
 * 호출 체인: main → spdk_env_init → rte_eal_init → hugepage/VFIO/PCI bus probe
 */
int spdk_env_init(const struct spdk_env_opts *opts);

/**
 * Release any resources of the environment library that were allocated with
 * spdk_env_init(). After this call, no SPDK env function calls may be made.
 * It is expected that common usage of this function is to call it just before
 * terminating the process or before reinitializing the environment library
 * within the same process.
 */
/*
 * [한국어]
 * spdk_env_fini - DPDK EAL 종료 + 모든 env 자원 해제
 *
 * 호출 후에는 어떤 env API 도 사용 불가. 보통 프로세스 종료 직전 호출.
 * 동작: DPDK rte_eal_cleanup → hugepage unmap, VFIO 컨테이너 닫기, PCI 디바이스 detach,
 *       memzone/mempool/ring 글로벌 리스트 비움.
 * 컨텍스트: 메인 스레드에서 1회. 모든 reactor 가 정지된 상태여야 함.
 *
 * 호출 체인: spdk_app_stop_complete → spdk_env_fini → rte_eal_cleanup
 */
void spdk_env_fini(void);

/**
 * Allocate a pinned memory buffer with the given size and alignment.
 *
 * \param size Size in bytes.
 * \param align If non-zero, the allocated buffer is aligned to a multiple of
 * align. In this case, it must be a power of two. The returned buffer is always
 * aligned to at least cache line size.
 * \param unused **Invalid**. If not a NULL, the function will fail and return NULL.
 *
 * \return a pointer to the allocated memory buffer.
 */
/*
 * [한국어]
 * spdk_dma_malloc - DMA 안전 버퍼 할당 (NUMA 자동, spdk_malloc 의 편의 래퍼)
 *
 * @param size: 바이트 수.
 * @param align: 정렬 (0=캐시라인 자동, 2의 거듭제곱).
 * @param unused: NULL 이어야 함. legacy IOVA out-param 슬롯.
 * @return: hugepage 기반 가상주소 또는 NULL.
 *
 * 의미: spdk_malloc(size, align, unused, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA) 와 동일.
 * 역사: SPDK 초창기 API 로 신코드는 spdk_malloc + flags 사용 권장. 다만 광범위한 out-of-tree
 *   사용자 코드 호환을 위해 유지.
 * 반환된 버퍼는 spdk_vtophys 로 IOVA 변환 가능 — NVMe SQE PRP/SGL 에 그대로 사용.
 *
 * 호출 체인: 사용자/드라이버 → spdk_dma_malloc → spdk_malloc(... , ANY, DMA)
 */
void *spdk_dma_malloc(size_t size, size_t align, uint64_t *unused);

/**
 * Allocate a pinned, memory buffer with the given size, alignment and socket id.
 *
 * \param size Size in bytes.
 * \param align If non-zero, the allocated buffer is aligned to a multiple of
 * align. In this case, it must be a power of two. The returned buffer is always
 * aligned to at least cache line size.
 * \param unused **Invalid**. If not a NULL, the function will fail and return NULL.
 * \param numa_id NUMA node ID to allocate memory on, or SPDK_ENV_NUMA_ID_ANY
 * for any NUMA node.
 *
 * \return a pointer to the allocated memory buffer.
 */
/*
 * [한국어]
 * spdk_dma_malloc_socket - DMA 안전 버퍼 + 명시 NUMA 노드 할당
 *
 * @param size: 바이트 수.
 * @param align: 정렬.
 * @param unused: NULL 필수.
 * @param numa_id: 할당할 NUMA 노드 (또는 SPDK_ENV_NUMA_ID_ANY).
 * @return: hugepage 가상주소 또는 NULL.
 *
 * 의미: spdk_malloc(... , numa_id, SPDK_MALLOC_DMA). NUMA-local I/O 최적화 — NVMe 디바이스가
 *   numa_id 노드에 붙어있다면 같은 노드에 버퍼를 잡아 cross-NUMA 메모리 트래픽을 회피.
 * 사용처: NVMe req PRP 버퍼, RDMA NIC 의 send/recv 버퍼.
 *
 * 호출 체인: NVMe attach_cb → spdk_dma_malloc_socket(... , dev_numa_id, ...) → spdk_malloc
 */
void *spdk_dma_malloc_socket(size_t size, size_t align, uint64_t *unused, int numa_id);

/**
 * Allocate a pinned memory buffer with the given size and alignment. The buffer
 * will be zeroed.
 *
 * \param size Size in bytes.
 * \param align If non-zero, the allocated buffer is aligned to a multiple of
 * align. In this case, it must be a power of two. The returned buffer is always
 * aligned to at least cache line size.
 * \param unused **Invalid**. If not a NULL, the function will fail and return NULL.
 *
 * \return a pointer to the allocated memory buffer.
 */
/*
 * [한국어]
 * spdk_dma_zmalloc - DMA 안전 + 0 초기화 버퍼 (NUMA ANY)
 *
 * @param size: 바이트 수.
 * @param align: 정렬 (0=캐시라인, 그 외 2^n).
 * @param unused: NULL 이어야 함 (legacy IOVA out-param 슬롯).
 * @return: 0 으로 채워진 hugepage 메모리 포인터 또는 NULL.
 *
 * 의미적으로 spdk_zmalloc(size, align, unused, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA) 와 동일.
 * 가장 흔히 사용되는 SPDK DMA 할당 API — NVMe Identify 응답, log page 페이로드, NVMe Admin queue 등
 * "init 시 한 번 잡고 lifetime 동안 유지" 하는 0-init 버퍼에 사용된다.
 *
 * 호출 체인: NVMe init / 사용자 → spdk_dma_zmalloc → spdk_zmalloc(... , ANY, DMA)
 */
void *spdk_dma_zmalloc(size_t size, size_t align, uint64_t *unused);

/**
 * Allocate a pinned memory buffer with the given size, alignment and socket id.
 * The buffer will be zeroed.
 *
 * \param size Size in bytes.
 * \param align If non-zero, the allocated buffer is aligned to a multiple of
 * align. In this case, it must be a power of two. The returned buffer is always
 * aligned to at least cache line size.
 * \param unused **Invalid**. If not a NULL, the function will fail and return NULL.
 * \param numa_id NUMA node ID to allocate memory on, or SPDK_ENV_NUMA_ID_ANY
 * for any NUMA node.
 *
 * \return a pointer to the allocated memory buffer.
 */
/*
 * [한국어]
 * spdk_dma_zmalloc_socket - DMA 안전 + 0 초기화 버퍼 + NUMA 노드 명시
 *
 * @param size: 바이트 수.
 * @param align: 정렬.
 * @param unused: NULL 필수.
 * @param numa_id: 할당할 NUMA 노드 (또는 SPDK_ENV_NUMA_ID_ANY).
 * @return: 0 으로 채워진 hugepage 메모리 포인터 또는 NULL.
 *
 * spdk_zmalloc(... , numa_id, SPDK_MALLOC_DMA) 와 동일. 디바이스가 붙은 NUMA 노드와 같은 곳에
 * 버퍼를 잡아 cross-NUMA DMA 대역폭 손실을 회피하는 패턴 (spdk_pci_device_get_numa_id 와 짝).
 *
 * 호출 체인: NVMe attach_cb → spdk_dma_zmalloc_socket(... , dev_numa_id, ...) → spdk_zmalloc
 */
void *spdk_dma_zmalloc_socket(size_t size, size_t align, uint64_t *unused, int numa_id);

/**
 * Resize the allocated and pinned memory buffer with the given new size and
 * alignment. Existing contents are preserved.
 *
 * \param buf Buffer to resize.
 * \param size Size in bytes.
 * \param align If non-zero, the allocated buffer is aligned to a multiple of
 * align. In this case, it must be a power of two. The returned buffer is always
 * aligned to at least cache line size.
 * \param unused **Invalid**. If not a NULL, the function will fail and return NULL.
 *
 * \return a pointer to the resized memory buffer.
 */
/*
 * [한국어]
 * spdk_dma_realloc - DMA 안전 버퍼 크기 변경 (spdk_realloc 의 DMA 버전)
 *
 * @param buf: spdk_dma_*malloc 으로 받은 기존 버퍼 (NULL = 신규 할당).
 * @param size: 새 크기 (0 = free).
 * @param align: 새 정렬.
 * @param unused: NULL 필수.
 * @return: 새 (혹은 같은) hugepage 가상주소. 옛 buf 는 무효일 수 있음.
 *
 * 위험: in-flight DMA 가 진행 중인 버퍼에 호출 금지. realloc 이 새 영역으로 옮기면 IOVA 가 바뀌고,
 *   NVMe 컨트롤러는 옛 IOVA 로 DMA 를 계속하다가 잘못된 메모리에 쓰기/읽기 → 데이터 손상.
 *   반드시 spdk_nvme_qpair_process_completions / spdk_bdev_io 완료를 보장한 뒤 호출.
 * 사용처: 동적 크기 버퍼 (사용자가 size 를 늘리면서 데이터 유지 필요) — 드물지만 사용자 코드용.
 *
 * 호출 체인: 사용자 코드 → spdk_dma_realloc → spdk_realloc → rte_realloc_socket
 */
void *spdk_dma_realloc(void *buf, size_t size, size_t align, uint64_t *unused);

/**
 * Free a memory buffer previously allocated, for example from spdk_dma_zmalloc().
 * This call is never made from the performance path.
 *
 * \param buf Buffer to free.
 */
/*
 * [한국어]
 * spdk_dma_free - spdk_dma_*malloc 으로 받은 버퍼 해제 (spdk_free 와 동일)
 *
 * @param buf: 해제할 버퍼 (NULL 허용 — no-op).
 *
 * 의미: spdk_free 의 별칭 (역사적 이유로 분리 유지). 일반 free() 와 절대 섞으면 안 됨.
 * 위험: in-flight DMA 가 진행 중인 버퍼에 호출 금지 — 완료 콜백 도착 후 해제.
 * 헤더 docstring 의 "never made from the performance path" 는 hugepage free 가 메타데이터 정리
 *   비용이 들어 핫패스에 부적합함을 알림 — 빈번한 alloc/free 는 mempool 로 대체.
 *
 * 호출 체인: NVMe shutdown / 사용자 cleanup → spdk_dma_free → spdk_free → rte_free
 */
void spdk_dma_free(void *buf);

/**
 * Reserve a named, process shared memory zone with the given size, numa_id
 * and flags. Unless `SPDK_MEMZONE_NO_IOVA_CONTIG` flag is provided, the returned
 * memory will be IOVA contiguous.
 *
 * \param name Name to set for this memory zone.
 * \param len Length in bytes.
 * \param numa_id NUMA node ID to allocate memory on, or SPDK_ENV_NUMA_ID_ANY
 * for any NUMA node.
 * \param flags Flags to set for this memory zone.
 *
 * \return a pointer to the allocated memory address on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_memzone_reserve - 이름표가 붙은 프로세스 공유 메모리 영역 예약
 *
 * @param name: 전역적으로 고유한 이름 (SPDK_MAX_MEMZONE_NAME_LEN 미만).
 * @param len: 바이트 길이.
 * @param numa_id: NUMA 노드 (또는 ANY).
 * @param flags: SPDK_MEMZONE_NO_IOVA_CONTIG 등.
 * @return: 가상주소 (성공) 또는 NULL.
 *
 * memzone vs malloc: memzone 은 "이름표"로 lookup 가능, multi-process 에서 shared-by-name,
 * 그리고 기본적으로 IOVA contiguous (DMA descriptor 사용에 적합). NVMe Admin queue 처럼
 * 한 프로세스에서 한 번만 잡고 lifetime 동안 유지하는 영역에 사용.
 *
 * 호출 체인: 드라이버 init → spdk_memzone_reserve → rte_memzone_reserve
 */
void *spdk_memzone_reserve(const char *name, size_t len, int numa_id, unsigned flags);

/**
 * Reserve a named, process shared memory zone with the given size, numa_id,
 * flags and alignment. Unless `SPDK_MEMZONE_NO_IOVA_CONTIG` flag is provided,
 * the returned memory will be IOVA contiguous.
 *
 * \param name Name to set for this memory zone.
 * \param len Length in bytes.
 * \param numa_id NUMA node ID to allocate memory on, or SPDK_ENV_NUMA_ID_ANY
 * for any NUMA node.
 * \param flags Flags to set for this memory zone.
 * \param align Alignment for resulting memzone. Must be a power of 2.
 *
 * \return a pointer to the allocated memory address on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_memzone_reserve_aligned - spdk_memzone_reserve + 정렬 강제 (4KiB/2MiB 등)
 *
 * @param align: 2의 거듭제곱. NVMe SQ/CQ 는 4KiB 정렬 필요.
 * 호출 체인: 드라이버 init → spdk_memzone_reserve_aligned → rte_memzone_reserve_aligned
 */
void *spdk_memzone_reserve_aligned(const char *name, size_t len, int numa_id,
				   unsigned flags, unsigned align);

/**
 * Lookup the memory zone identified by the given name.
 *
 * \param name Name of the memory zone.
 *
 * \return a pointer to the reserved memory address on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_memzone_lookup - 이름으로 기존 memzone 검색
 *
 * Multi-process 에서 secondary 가 primary 가 만든 영역을 attach 할 때 사용.
 * 호출 체인: secondary process init → spdk_memzone_lookup → rte_memzone_lookup
 */
void *spdk_memzone_lookup(const char *name);

/**
 * Free the memory zone identified by the given name.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_memzone_free - 이름으로 memzone 해제
 *
 * 0 성공, -1 실패. 다른 프로세스가 아직 attach 중이면 의도와 다르게 동작 가능.
 * 호출 체인: 드라이버 cleanup → spdk_memzone_free → rte_memzone_free
 */
int spdk_memzone_free(const char *name);

/**
 * Dump debug information about all memzones.
 *
 * \param f File to write debug information to.
 */
/*
 * [한국어]
 * spdk_memzone_dump - 디버깅용. 등록된 모든 memzone 의 이름/크기/IOVA 를 f 에 기록
 *
 * @param f: 보통 stderr 또는 RPC 응답 스트림.
 * 호출 체인: 디버그/RPC → spdk_memzone_dump → rte_memzone_dump
 */
void spdk_memzone_dump(FILE *f);

/* [한국어] mempool 의 불투명(opaque) 구조체 전방 선언. 실제 정의는 env_dpdk 내부 (rte_mempool 래퍼).
 * 사용자는 포인터로만 다루며 spdk_mempool_* 함수들로만 조작한다. */
struct spdk_mempool;

/* [한국어] spdk_mempool_create 의 cache_size 인자에 SIZE_MAX 를 넘기면
 * "DPDK 기본 코어별 캐시 크기 사용" 의미. 0 을 넘기면 코어 캐시 비활성 (lockless 효과 감소).
 * SPDK 핫패스에서는 코어 캐시가 critical — bdev_io 풀 등은 디폴트 사용. */
#define SPDK_MEMPOOL_DEFAULT_CACHE_SIZE	SIZE_MAX

/**
 * Create a thread-safe memory pool.
 *
 * \param name Name for the memory pool.
 * \param count Count of elements.
 * \param ele_size Element size in bytes.
 * \param cache_size How many elements may be cached in per-core caches. Use
 * SPDK_MEMPOOL_DEFAULT_CACHE_SIZE for a reasonable default, or 0 for no per-core cache.
 * \param numa_id NUMA node ID to allocate memory on, or SPDK_ENV_NUMA_ID_ANY
 * for any NUMA node.
 *
 * \return a pointer to the created memory pool.
 */
/*
 * [한국어]
 * spdk_mempool_create - lockless 객체 풀 생성 (DPDK rte_mempool 래퍼)
 *
 * @param name: 전역 고유 이름 (lookup 가능). SPDK_MAX_MEMPOOL_NAME_LEN 미만.
 * @param count: 풀에 들어갈 객체 개수.
 * @param ele_size: 객체 1개의 크기.
 * @param cache_size: 각 lcore 가 자체 캐시할 객체 수 (SPDK_MEMPOOL_DEFAULT_CACHE_SIZE 권장).
 *                    0 이면 캐시 끔 → 매번 글로벌 ring 에 atomic 접근 (느림).
 * @param numa_id: 풀 메모리가 위치할 NUMA 노드.
 * @return: 생성된 풀 핸들 또는 NULL.
 *
 * SPDK 핫패스의 단골: bdev_io, NVMe req, scsi task 등 빈번한 alloc/free 객체에 사용.
 * lockless 의 비결: per-lcore 캐시(첫 N개) → 코어가 자기 캐시에서 빼면 atomic 불필요.
 * 캐시가 비면 글로벌 ring (MP_MC) 에서 cache_size 만큼 한 번에 가져온다 → atomic CAS 1회로 N개.
 *
 * 호출 체인: bdev/NVMe init → spdk_mempool_create → rte_mempool_create_empty + populate + obj_init
 */
struct spdk_mempool *spdk_mempool_create(const char *name, size_t count,
		size_t ele_size, size_t cache_size, int numa_id);

/**
 * An object callback function for memory pool.
 *
 * Used by spdk_mempool_create_ctor().
 */
/* [한국어] mempool 의 각 객체에 대해 호출되는 사용자 콜백 시그니처.
 * @param mp: 대상 풀.
 * @param opaque: 사용자가 create_ctor 에 넘긴 컨텍스트.
 * @param obj: 현재 콜백이 다루는 객체 포인터.
 * @param obj_idx: 0..count-1 인덱스.
 * 사용처: spdk_mempool_create_ctor 가 풀 생성 시 모든 객체에 1회 호출 (필드 초기화 등),
 *         또는 spdk_mempool_obj_iter 가 디버깅/통계용으로 모든 객체 순회. */
typedef void (spdk_mempool_obj_cb_t)(struct spdk_mempool *mp,
				     void *opaque, void *obj, unsigned obj_idx);

/**
 * A memory chunk callback function for memory pool.
 *
 * Used by spdk_mempool_mem_iter().
 */
/* [한국어] mempool 의 각 메모리 chunk(연속된 hugepage 영역)에 대한 콜백.
 * @param addr: chunk 의 가상주소 시작.
 * @param iova: chunk 의 IOVA 시작 (DMA 매핑 시 사용).
 * @param len: chunk 길이.
 * @param mem_idx: chunk 인덱스 (풀이 여러 chunk 로 분리될 수 있음 — IOVA 비연속 hugepage 시).
 * 사용처: 외부 DMA 엔진(예: RDMA NIC HW reg)에 풀 전체 메모리를 일괄 매핑할 때. */
typedef void (spdk_mempool_mem_cb_t)(struct spdk_mempool *mp, void *opaque, void *addr,
				     uint64_t iova, size_t len, unsigned mem_idx);

/**
 * Create a thread-safe memory pool with user provided initialization function
 * and argument.
 *
 * \param name Name for the memory pool.
 * \param count Count of elements.
 * \param ele_size Element size in bytes.
 * \param cache_size How many elements may be cached in per-core caches. Use
 * SPDK_MEMPOOL_DEFAULT_CACHE_SIZE for a reasonable default, or 0 for no per-core cache.
 * \param numa_id NUMA node ID to allocate memory on, or SPDK_ENV_NUMA_ID_ANY
 * for any NUMA node.
 * \param obj_init User provided object callback initialization function.
 * \param obj_init_arg User provided callback initialization function argument.
 *
 * \return a pointer to the created memory pool.
 */
/*
 * [한국어]
 * spdk_mempool_create_ctor - spdk_mempool_create + 모든 객체에 obj_init 콜백 1회 실행
 *
 * 풀 생성 직후 각 객체의 내부 필드(예: TAILQ_ENTRY, mutex, qpair 백포인터)를
 * 미리 셋팅해두어 핫패스의 spdk_mempool_get 이 즉시 사용 가능한 객체를 반환하게 한다.
 *
 * 호출 체인: bdev_io 풀 init → spdk_mempool_create_ctor → rte_mempool_create + obj_init loop
 */
struct spdk_mempool *spdk_mempool_create_ctor(const char *name, size_t count,
		size_t ele_size, size_t cache_size, int numa_id,
		spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg);

/**
 * Get the name of a memory pool.
 *
 * \param mp Memory pool to query.
 *
 * \return the name of the memory pool.
 */
/*
 * [한국어]
 * spdk_mempool_get_name - 풀의 이름 문자열 조회
 *
 * @param mp: 대상 풀.
 * @return: spdk_mempool_create 시 사용한 이름 문자열 포인터. 풀 lifetime 동안 유효.
 *
 * 주의: 호출자가 free 하면 안 됨 (내부 정적/풀 내부 버퍼). 풀이 free 되면 dangling pointer 가
 *   되므로 사용 전후로 풀의 lifetime 을 보장해야 한다.
 * 사용처: 디버그 로깅, RPC 통계 응답의 풀 식별자.
 *
 * 호출 체인: RPC/디버그 코드 → spdk_mempool_get_name → rte_mempool->name
 */
char *spdk_mempool_get_name(struct spdk_mempool *mp);

/**
 * Free a memory pool.
 */
/*
 * [한국어]
 * spdk_mempool_free - 풀과 모든 객체 메모리 해제
 *
 * 모든 객체가 풀에 반환된 상태(in-use 0)여야 안전. cleanup 단계에서만 호출.
 * 호출 체인: 모듈 cleanup → spdk_mempool_free → rte_mempool_free
 */
void spdk_mempool_free(struct spdk_mempool *mp);

/**
 * Get an element from a memory pool. If no elements remain, return NULL.
 *
 * \param mp Memory pool to query.
 *
 * \return a pointer to the element.
 */
/*
 * [한국어]
 * spdk_mempool_get - 풀에서 객체 1개 꺼냄 (lockless 핫패스)
 *
 * @return: 객체 포인터 또는 NULL (풀 고갈).
 *
 * 동작: 호출 코어의 캐시에서 먼저 시도 (lock-free), 캐시 비면 글로벌 ring 에서 cache_size 만큼
 *       한 번에 보충. 보충 자체는 atomic CAS 한 번. 핫패스 (bdev I/O 시작 시 bdev_io 할당) 의 핵심.
 * 컨텍스트: SPDK thread (lcore) 에서만 호출 권장 — 비-lcore 스레드는 코어 캐시 미존재.
 *
 * 호출 체인: spdk_bdev_*_io 시작 → spdk_mempool_get → rte_mempool_get
 */
void *spdk_mempool_get(struct spdk_mempool *mp);

/**
 * Get multiple elements from a memory pool.
 *
 * \param mp Memory pool to get multiple elements from.
 * \param ele_arr Array of the elements to fill.
 * \param count Count of elements to get.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_mempool_get_bulk - count 개 객체를 한 번에 꺼냄 (atomic 횟수 절감)
 *
 * @param mp: 대상 풀.
 * @param ele_arr: count 개 포인터를 받을 호출자 배열.
 * @param count: 요청 개수.
 * @return: 0 = 모두 받음, 음수 errno = 부족 (이 경우 ele_arr 는 부분적이거나 미정의 상태,
 *          호출자는 받은 0 개 가정하고 fallback 처리 필요 — 즉 atomic all-or-nothing 의미).
 *
 * 동작: 코어 캐시에서 우선 빼고, 부족하면 글로벌 ring 에서 보충 시도. 한 번의 atomic CAS 로 N 개
 *   가져오므로 spdk_mempool_get 을 N 번 호출하는 것보다 효율적.
 * 사용처: NVMe-oF target 의 RDMA WR 묶음 처리, 한 bdev_io 가 여러 child IO 를 동시에 띄울 때.
 * 컨텍스트: SPDK thread 권장 (코어 캐시 사용).
 *
 * 호출 체인: NVMe-oF / bdev split → spdk_mempool_get_bulk → rte_mempool_get_bulk
 */
int spdk_mempool_get_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count);

/**
 * Put an element back into the memory pool.
 *
 * \param mp Memory pool to put element back into.
 * \param ele Element to put.
 */
/*
 * [한국어]
 * spdk_mempool_put - 객체를 풀에 반환 (핫패스, lockless)
 *
 * 동작: 코어 캐시가 가득 차면 절반을 글로벌 ring 으로 flush.
 * 호출 체인: bdev_io 완료 콜백 → spdk_mempool_put → rte_mempool_put
 */
void spdk_mempool_put(struct spdk_mempool *mp, void *ele);

/**
 * Put multiple elements back into the memory pool.
 *
 * \param mp Memory pool to put multiple elements back into.
 * \param ele_arr Array of the elements to put.
 * \param count Count of elements to put.
 */
/*
 * [한국어]
 * spdk_mempool_put_bulk - 여러 객체를 한 번에 풀에 반환 (atomic 횟수 절감)
 *
 * @param mp: 대상 풀.
 * @param ele_arr: 반환할 객체 포인터 배열.
 * @param count: 반환 개수.
 *
 * 동작: 코어 캐시에 우선 채우고, 캐시가 가득 차면 절반을 글로벌 ring 으로 flush. 1번 atomic CAS 로 N 개.
 * 사용처: NVMe-oF target 의 RDMA WR 묶음 완료, bdev split child 완료 시 일괄 free.
 * 컨텍스트: SPDK thread 권장.
 *
 * 호출 체인: 완료 콜백/배치 free → spdk_mempool_put_bulk → rte_mempool_put_bulk
 */
void spdk_mempool_put_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count);

/**
 * Get the number of entries in the memory pool.
 *
 * \param pool Memory pool to query.
 *
 * \return the number of entries in the memory pool.
 */
/*
 * [한국어]
 * spdk_mempool_count - 현재 풀 안에 "사용 가능한"(in-pool) 객체 수 조회
 *
 * @param pool: 대상 풀.
 * @return: 모든 lcore 캐시 + 글로벌 ring 의 잔여 객체 수 합산 (근사값).
 *
 * 정확성: 멀티 lcore 환경에서 다른 코어가 동시에 get/put 중이면 결과는 즉시 stale 가능 — 정확한
 *   "in-use 객체 수" = create 시 count - 이 함수 반환값 도 race-prone.
 * 사용처: 모니터링, RPC bdev_get_iostat 의 pool 통계, 디버깅. 핫패스 분기 조건으로는 사용 X
 *   (실패는 spdk_mempool_get 이 NULL 반환할 때 처리).
 *
 * 호출 체인: RPC stats → spdk_mempool_count → rte_mempool_avail_count
 */
size_t spdk_mempool_count(const struct spdk_mempool *pool);

/**
 * Iterate through all elements of the pool and call a function on each one.
 *
 * \param mp Memory pool to iterate on.
 * \param obj_cb Function to call on each element.
 * \param obj_cb_arg Opaque pointer passed to the callback function.
 *
 * \return Number of elements iterated.
 */
/*
 * [한국어]
 * spdk_mempool_obj_iter - 풀의 모든 객체에 obj_cb 호출 (in-use/in-pool 무관 전체 순회)
 *
 * @param mp: 대상 풀.
 * @param obj_cb: 각 객체에 호출될 콜백 (spdk_mempool_obj_cb_t 시그니처).
 * @param obj_cb_arg: 콜백에 전달되는 opaque 컨텍스트.
 * @return: 순회된 객체 수.
 *
 * 동작: 풀 내부의 메모리 chunk 들을 ele_size 단위로 슬라이스하여 obj_cb 를 N 번 호출.
 *   in-use(현재 다른 코드가 들고 있는) 객체도 포함되므로 콜백은 객체의 일관성을 가정하면 안 됨
 *   (사용 중일 수 있음 → read-only 메타정보 출력 정도가 안전).
 * 사용처: 디버깅 dump, 통계, 외부 DMA 매핑 (RDMA MR 등록을 풀 전체 객체에 일괄 적용 — 이때는
 *   풀이 아직 in-use 0 인 init 직후가 안전).
 * 컨텍스트: 핫패스 금지 (O(N) 비용). init / cleanup / RPC 핸들러 등 cold path.
 *
 * 호출 체인: RDMA MR init / RPC stats → spdk_mempool_obj_iter → rte_mempool_obj_iter
 */
uint32_t spdk_mempool_obj_iter(struct spdk_mempool *mp, spdk_mempool_obj_cb_t obj_cb,
			       void *obj_cb_arg);

/**
 * Iterate through all memory chunks of the pool and call a function on each one.
 *
 * \param mp Memory pool to iterate on.
 * \param mem_cb Function to call on each memory chunk.
 * \param mem_cb_arg Opaque pointer passed to the callback function.
 *
 * \return Number of memory chunks iterated.
 */
/*
 * [한국어]
 * spdk_mempool_mem_iter - 풀의 각 메모리 chunk(연속 IOVA 영역) 단위로 콜백 호출
 *
 * @param mp: 대상 풀.
 * @param mem_cb: 각 chunk 에 호출될 콜백 (spdk_mempool_mem_cb_t).
 * @param mem_cb_arg: 콜백 컨텍스트.
 * @return: 순회된 chunk 수.
 *
 * 동작: 풀의 백킹 메모리는 IOVA-연속 hugepage 가 부족하면 여러 chunk 로 분할 저장됨. 이 함수는 각
 *   chunk 의 (addr, iova, len) 을 콜백에 전달.
 * 사용처: NVMe-oF RDMA target 이 풀 메모리를 RDMA MR(memory region) 로 등록할 때 — MR 은 IOVA-
 *   연속 영역 단위로만 가능하므로 chunk 단위로 ibv_reg_mr 호출. obj 단위(spdk_mempool_obj_iter)
 *   보다 효율적 (한 chunk = 수많은 obj).
 * 컨텍스트: init / RDMA poll group 생성 시. 핫패스 금지.
 *
 * 호출 체인: NVMe-oF RDMA init → spdk_mempool_mem_iter → rte_mempool_mem_iter → ibv_reg_mr
 */
uint32_t spdk_mempool_mem_iter(struct spdk_mempool *mp, spdk_mempool_mem_cb_t mem_cb,
			       void *mem_cb_arg);

/**
 * Lookup the memory pool identified by the given name.
 *
 * \param name Name of the memory pool.
 *
 * \return a pointer to the memory pool on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_mempool_lookup - 이름으로 등록된 mempool 검색
 *
 * @param name: spdk_mempool_create 시 사용된 고유 이름.
 * @return: 매칭된 풀 핸들, 없으면 NULL.
 *
 * 동작: 글로벌 mempool 리스트(DPDK rte_mempool 의 글로벌 TAILQ)에서 strcmp 매칭.
 * 사용처:
 *   - Multi-process: secondary 가 primary 가 만든 풀에 attach (객체 데이터는 hugepage 공유).
 *   - 모듈 간 공유: bdev_io 풀처럼 한 라이브러리가 만든 풀을 다른 모듈이 lookup 으로 접근.
 *
 * 호출 체인: secondary init / 모듈 lookup → spdk_mempool_lookup → rte_mempool_lookup
 */
struct spdk_mempool *spdk_mempool_lookup(const char *name);

/**
 * Get the number of dedicated CPU cores utilized by this env abstraction.
 *
 * \return the number of dedicated CPU cores.
 */
/*
 * [한국어]
 * spdk_env_get_core_count - SPDK 가 사용할 lcore 의 개수 조회
 *
 * @return: spdk_env_opts.core_mask 또는 lcore_map 에 set 된 lcore 비트 수와 동일.
 *          spdk_env_init 이 한 번 결정한 뒤로 변하지 않음.
 *
 * 사용처:
 *   - reactor 개수 결정 (lib/thread): 정확히 이 수만큼 reactor 스레드 생성.
 *   - mempool cache size 추정 (총 캐시 메모리 = cache_size * 코어수).
 *   - bdev_io 풀 크기 산정 (코어당 N 개 가정).
 * 컨텍스트: 어디서나 호출 가능 (init 후).
 *
 * 호출 체인: reactor init / bdev init → spdk_env_get_core_count → 내부 lcore 비트맵 popcount
 */
uint32_t spdk_env_get_core_count(void);

/**
 * Get the CPU core index of the current thread.
 *
 * This will only function when called from threads set up by
 * this environment abstraction. For any other threads \c SPDK_ENV_LCORE_ID_ANY
 * will be returned.
 *
 * \return the CPU core index of the current thread.
 */
/*
 * [한국어]
 * spdk_env_get_current_core - 현재 스레드가 실행 중인 lcore ID
 *
 * SPDK lcore (DPDK 가 만든 pinned pthread) 가 아니면 SPDK_ENV_LCORE_ID_ANY 반환.
 * 동작: pthread TLS 의 RTE_PER_LCORE(_lcore_id) 를 읽음.
 * 사용처: 코어별 통계, mempool cache 라우팅, thread affinity 검증.
 */
uint32_t spdk_env_get_current_core(void);

/**
 * Get the index of the main dedicated CPU core for this application.
 *
 * \return the index of the main dedicated CPU core.
 */
/*
 * [한국어]
 * spdk_env_get_main_core - 메인(main) lcore ID 조회
 *
 * @return: spdk_env_opts.main_core 가 지정한 값 또는 미지정시 자동 선택된 lcore (보통 core_mask 의
 *          가장 낮은 비트). spdk_env_init 이후 불변.
 *
 * 의미: "main lcore" = DPDK 가 rte_eal_init 후 가장 먼저 진입하는 lcore. SPDK 는 보통 이 코어에서
 *   초기화 작업(메모리 풀 생성, RPC 서버 부팅)을 수행하고 다른 reactor 들에 I/O 분배.
 * 사용처: spdk_thread_get_app_thread() 가 일반적으로 이 코어에 매핑됨, RPC 핸들러 라우팅 기준.
 *
 * 호출 체인: app init / RPC dispatcher → spdk_env_get_main_core → 내부 변수
 */
uint32_t spdk_env_get_main_core(void);

/**
 * Get the index of the first dedicated CPU core for this application.
 *
 * \return the index of the first dedicated CPU core.
 */
/*
 * [한국어]
 * spdk_env_get_first_core - core_mask 에서 가장 낮은 비트의 lcore ID 조회
 *
 * @return: 가장 낮은 lcore 인덱스. core_mask 가 비어있으면 SPDK_ENV_LCORE_ID_ANY (UINT32_MAX).
 *
 * 동작: env_dpdk 가 보관한 lcore 비트맵에서 첫 set bit 의 위치 반환.
 * 사용처: SPDK_ENV_FOREACH_CORE 매크로의 시작점, 디폴트 reactor 코어 결정 (main_core 가 -1 일 때).
 *
 * 호출 체인: SPDK_ENV_FOREACH_CORE / 사용자 → spdk_env_get_first_core → 비트맵 ffs
 */
uint32_t spdk_env_get_first_core(void);

/**
 * Get the index of the last dedicated CPU core for this application.
 *
 * \return the index of the last dedicated CPU core.
 */
/*
 * [한국어]
 * spdk_env_get_last_core - core_mask 에서 가장 높은 비트의 lcore ID 조회
 *
 * @return: 가장 높은 lcore 인덱스 (SPDK_ENV_FOREACH_CORE 의 종료 비교에는 사용 X — next 가
 *          UINT32_MAX 반환으로 종료 처리). core_mask 비어있으면 SPDK_ENV_LCORE_ID_ANY.
 *
 * 사용처: 코어 범위 사전 할당 (예: lcore 인덱스 기반 배열 크기 = last_core + 1), 마지막 코어에만
 *   특별 역할 부여.
 *
 * 호출 체인: 사용자/모듈 init → spdk_env_get_last_core → 비트맵 fls
 */
uint32_t spdk_env_get_last_core(void);

/**
 * Get the index of the next dedicated CPU core for this application.
 *
 * If there is no next core, return UINT32_MAX.
 *
 * \param prev_core Index of previous core.
 *
 * \return the index of the next dedicated CPU core.
 */
/*
 * [한국어]
 * spdk_env_get_next_core - prev_core 다음으로 켜진 lcore ID
 *
 * @return: 다음 코어 ID 또는 UINT32_MAX(=SPDK_ENV_LCORE_ID_ANY) 종료.
 * SPDK_ENV_FOREACH_CORE 매크로 내부에서 사용. core_mask 가 sparse 일 때도 안전.
 */
uint32_t spdk_env_get_next_core(uint32_t prev_core);

/* [한국어] 모든 lcore 를 순회하는 매크로. 일반적인 사용 패턴:
 *   uint32_t i; SPDK_ENV_FOREACH_CORE(i) { ... }
 * 종료 조건은 i < UINT32_MAX (== get_next_core 가 UINT32_MAX 반환 = 더 이상 없음).
 * 사용처: 모든 reactor 에 메시지 보내기, 코어별 통계 집계 등. */
#define SPDK_ENV_FOREACH_CORE(i)		\
	for (i = spdk_env_get_first_core();	\
	     i < UINT32_MAX;			\
	     i = spdk_env_get_next_core(i))

/**
 * Get the NUMA node ID for the given core.
 *
 * \param core CPU core to query.
 *
 * \return the NUMA node ID for the given core.
 */
/*
 * [한국어]
 * spdk_env_get_numa_id - 주어진 lcore 가 속한 NUMA 노드
 * 사용처: PCI 디바이스가 속한 NUMA 와 일치하는 코어를 reactor 로 배치 (NUMA-local I/O).
 */
int32_t spdk_env_get_numa_id(uint32_t core);

/**
 * Get the ID of the first NUMA node on this system.
 *
 * \return the ID of the first NUMA node
 */
/*
 * [한국어]
 * spdk_env_get_first_numa_id - 시스템의 가장 낮은 NUMA 노드 ID 조회
 *
 * @return: 시스템이 보유한 NUMA 노드 중 가장 낮은 ID (보통 0). NUMA 노드 정보가 없으면 -1.
 *
 * 동작: env_dpdk 가 부팅 시 sysfs(/sys/devices/system/node/) 또는 hwloc 으로 토폴로지를 스캔하여
 *       만들어둔 내부 비트맵에서 첫 set bit 의 인덱스를 반환. SPDK_ENV_FOREACH_NUMA_ID 매크로의
 *       시작점으로 사용된다.
 * 컨텍스트: 어디서나 호출 가능. read-only 조회로 동기화 불필요.
 * caller: 시스템 토폴로지 순회가 필요한 모든 모듈 (예: bdev_io 풀 NUMA 별 생성, RPC 통계).
 * callee: env_dpdk 내부 NUMA 비트맵 lookup.
 *
 * 호출 체인: SPDK_ENV_FOREACH_NUMA_ID 매크로 / 사용자 코드 → spdk_env_get_first_numa_id
 */
int32_t spdk_env_get_first_numa_id(void);

/**
 * Get the ID of the last NUMA node on this system.
 *
 * \return the ID of the last NUMA node
 */
/*
 * [한국어]
 * spdk_env_get_last_numa_id - 시스템의 가장 높은 NUMA 노드 ID 조회
 *
 * @return: NUMA 노드 비트맵의 마지막 set bit 인덱스. 단일 NUMA 시스템은 0.
 *
 * 동작: env_dpdk 의 내부 NUMA 비트맵에서 마지막 set bit 의 인덱스를 반환. SPDK_ENV_FOREACH_NUMA_ID
 *       매크로의 종료 비교에는 사용되지 않고 (next 가 INT32_MAX 반환으로 끝남), 노드 개수 산정이나
 *       마지막 노드 특별 처리 (예: 마지막 노드에만 RPC 콜백 부착) 시 사용한다.
 * 컨텍스트: 어디서나 호출 가능. read-only.
 *
 * 호출 체인: 사용자 코드 → spdk_env_get_last_numa_id
 */
int32_t spdk_env_get_last_numa_id(void);

/**
 * Get the index of the next NUMA node on this system.
 *
 * If there is no next NUMA ID, or the passed prev_numa_id is not a
 * valid NUMA ID, return INT32_MAX.
 *
 * \param prev_numa_id Index of previous NUMA ID.
 *
 * \return the index of the next NUMA ID, or INT32_MAX if there is no next one
 */
/*
 * [한국어]
 * spdk_env_get_next_numa_id - prev 다음 NUMA 노드 ID. 종료시 INT32_MAX.
 * SPDK_ENV_FOREACH_NUMA_ID 매크로 내부에서 사용.
 */
int32_t spdk_env_get_next_numa_id(int32_t prev_numa_id);

/* [한국어] 시스템의 모든 NUMA 노드를 순회. 사용 예: 노드별 mempool 생성, 통계 집계. */
#define SPDK_ENV_FOREACH_NUMA_ID(i)			\
	for (i = spdk_env_get_first_numa_id();		\
	     i < INT32_MAX;				\
	     i = spdk_env_get_next_numa_id(i))

/* [한국어] CPU 비트마스크를 다루는 SPDK 추상화의 전방 선언.
 * 정의는 spdk/cpuset.h 에 있음. 이 헤더에서는 포인터로만 사용. */
struct spdk_cpuset;

/**
 * Create a cpuset with each dedicated core's bit set to true.
 *
 * This function will first zero the cpuset and then set the
 * bit for each core dedicated to this application to true.
 *
 * \param cpuset spdk_cpuset to initialize
 */
/*
 * [한국어]
 * spdk_env_get_cpuset - 앱이 보유한 모든 lcore 비트를 cpuset 에 채움 (먼저 0 클리어).
 *
 * @param cpuset: 호출자가 잡은 빈 spdk_cpuset (스택/힙).
 * 사용처: spdk_thread 생성 시 "어디든 가능" cpumask 로 초기화.
 */
void spdk_env_get_cpuset(struct spdk_cpuset *cpuset);

/**
 * Create a cpuset with each SMT sibling core's bit set to true.
 *
 * This function will first zero the cpuset and then set the bit for each
 * SMT sibling core to true.
 *
 * If the specified core has no SMT siblings, then only the specified
 * core's bit will be set.
 *
 * If the specified core has SMT siblings, then all of the siblings, including
 * the specified core, will be set. Note: this will set bits for all siblings,
 * even ones not part of the application's core mask.
 *
 * If the specified core is UINT32_MAX, then bits will be set for all SMT
 * siblings of all cores in the application's core mask.
 *
 * \param cpuset spdk_cpuset for SMT sibling cores
 * \param core core to get siblings for (UINT32_MAX for all cores in app
 *             core mask)
 * \return true if environment supports SMT detection, false otherwise (in
 *         which case the spdk_cpuset will be invalid)
 */
/*
 * [한국어]
 * spdk_env_core_get_smt_cpuset - SMT(하이퍼스레딩) 형제 코어 비트 조회
 *
 * SMT 형제(같은 물리 코어를 공유하는 logical CPU)는 캐시/실행유닛을 공유하므로
 * 같은 NVMe qpair 를 두 SMT 에 배치하면 서로 캐시 라인 핑퐁 발생 — 이를 회피하기 위한 정보.
 * @return: SMT 감지 가능 환경(Linux + sysfs) true, 아니면 false (cpuset 무효).
 */
bool spdk_env_core_get_smt_cpuset(struct spdk_cpuset *cpuset, uint32_t core);

/* [한국어] spdk_env_thread_launch_pinned 의 진입점 시그니처.
 * @param void*: spdk_env_thread_launch_pinned 의 arg 인자.
 * @return: 스레드 종료 시 종료 코드. */
typedef int (*thread_start_fn)(void *);

/**
 * Launch a thread pinned to the given core. Only a single pinned thread may be
 * launched per core. Subsequent attempts to launch pinned threads on that core
 * will fail.
 *
 * \param core The core to pin the thread to.
 * \param fn Entry point on the new thread.
 * \param arg Argument passed to thread_start_fn
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_env_thread_launch_pinned - 지정 코어에 핀된 pthread 시작
 *
 * @param core: 핀할 lcore ID. 이미 다른 pinned thread 가 있으면 실패.
 * @param fn: 스레드 진입 함수.
 * @param arg: fn 에 전달할 인자.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 동작: pthread_create + sched_setaffinity 로 단일 코어에 고정. SPDK 의 reactor 모델은
 * 이 위에서 빌드된다 — 1 코어 1 스레드 1 reactor 가 곧 polled-mode 의 토대.
 * 컨텍스트: 메인 스레드에서 init 시. 핫패스 호출 금지.
 *
 * 호출 체인: spdk_app_start / spdk_thread_lib_init → spdk_env_thread_launch_pinned → pthread_create
 */
int spdk_env_thread_launch_pinned(uint32_t core, thread_start_fn fn, void *arg);

/**
 * Wait for all threads to exit before returning.
 */
/*
 * [한국어]
 * spdk_env_thread_wait_all - launch_pinned 로 생성된 모든 lcore 스레드의 종료 대기 (join)
 *
 * 동작: DPDK rte_eal_mp_wait_lcore 호출 — 모든 lcore 의 thread_start_fn 이 return 할 때까지
 *   메인 스레드를 블록. lcore 스레드가 무한 polling 루프인 경우 외부 종료 신호로 루프를 빠져나오게
 *   먼저 만들어야 한다 (그렇지 않으면 영원히 대기).
 * 컨텍스트: 메인 스레드 종료 단계에서 1회.
 * caller: spdk_app_stop → spdk_app_fini → spdk_env_thread_wait_all → spdk_env_fini.
 *
 * 호출 체인: main → spdk_env_thread_wait_all → rte_eal_mp_wait_lcore
 */
void spdk_env_thread_wait_all(void);

/**
 * Check whether the calling process is primary process.
 *
 * \return true if the calling process is primary process, or false otherwise.
 */
/*
 * [한국어]
 * spdk_process_is_primary - 현재 프로세스가 DPDK primary 인지 secondary 인지 조회
 *
 * @return: true = primary (= 최초로 EAL init 한 프로세스), false = secondary (= 같은 shm_id 로 attach).
 *
 * Multi-process 모델 (DPDK):
 *   - Primary: hugepage 를 처음 mmap 하고 memzone/mempool 의 메타데이터를 만든다. secondary 들이
 *     붙기 전에 모든 자원을 미리 생성해두는 패턴.
 *   - Secondary: 같은 hugepage segment 를 attach 하여 primary 가 만든 자원을 lookup-only 로 사용.
 *     memzone_reserve / mempool_create 같은 "생성" API 는 호출 불가 (errno=ENOTSUP).
 *
 * 사용처:
 *   - bdev/모듈이 init 시 분기: primary 는 풀 생성, secondary 는 lookup.
 *   - vhost target 의 primary, vhost-user client 의 secondary 패턴.
 * 컨텍스트: 어디서나. 결과는 init 후 불변.
 *
 * 호출 체인: 모듈 init → spdk_process_is_primary → rte_eal_process_type == RTE_PROC_PRIMARY
 */
bool spdk_process_is_primary(void);

/**
 * Get a monotonic timestamp counter.
 *
 * \return the monotonic timestamp counter.
 */
/*
 * [한국어]
 * spdk_get_ticks - 단조 증가 타임스탬프 카운터 (TSC 기반)
 *
 * @return: 64-bit monotonic tick 카운터. 단위는 spdk_get_ticks_hz() 의 Hz 로 ns/us 환산.
 *
 * 동작: DPDK rte_get_tsc_cycles → 내부적으로 x86 의 RDTSC 인스트럭션 (또는 ARM CNTVCT_EL0).
 *   시스템콜 없음 — 매우 빠르게 (수 사이클) ns-급 해상도 시간 측정 가능.
 *   주의: TSC 가 invariant 아니면 (구형 CPU, 일부 VM) 값이 코어/주파수 변경 따라 흔들릴 수 있음 —
 *   현대 서버 CPU 는 invariant TSC 보장 (CPUID.80000007h:EDX[8]).
 * 사용처: poller deadline 계산, I/O latency 측정, statistic histogram (lib/util/histogram).
 *
 * 호출 체인: poller / latency stats → spdk_get_ticks → RDTSC
 */
uint64_t spdk_get_ticks(void);

/**
 * Get the tick rate of spdk_get_ticks() per second.
 *
 * \return the tick rate of spdk_get_ticks() per second.
 */
/*
 * [한국어]
 * spdk_get_ticks_hz - spdk_get_ticks() 의 초당 증가량 (TSC 주파수, Hz)
 *
 * @return: tick rate (Hz). 예: 3.0 GHz CPU 면 3000000000 근사값.
 *
 * 동작: DPDK 가 init 시점에 CPUID 또는 calibration 으로 측정해둔 값을 반환. invariant TSC 면
 *   런타임 내내 변하지 않음.
 * 환산:
 *   - ns = ticks * 1e9 / hz
 *   - us = ticks * 1e6 / hz
 *   - sec = ticks / hz
 * 사용처: spdk_get_ticks 결과를 사람이 읽을 수 있는 단위로 변환, poller period 설정.
 *
 * 호출 체인: latency 계산 / poller 등록 → spdk_get_ticks_hz → DPDK 캐시값
 */
uint64_t spdk_get_ticks_hz(void);

/**
 * Delay the given number of microseconds.
 *
 * \param us Number of microseconds.
 */
/*
 * [한국어]
 * spdk_delay_us - busy-wait 으로 지정 마이크로초만큼 대기
 *
 * @param us: 대기 시간 (마이크로초).
 *
 * 동작: spdk_get_ticks 를 폴링하며 us * hz / 1e6 만큼 경과할 때까지 busy-loop.
 *   usleep / nanosleep 같은 sleep 시스템 콜이 아닌 이유: reactor 가 잠들면 다른 poller 가 정지하고
 *   polled-mode I/O latency 가 깨진다. busy-wait 은 CPU 를 100% 점유하지만 reactor 의 polling
 *   순환은 유지된다.
 * 사용처:
 *   - NVMe reset 시퀀스의 spec 명시 대기 (예: CC.EN 토글 후 CSTS.RDY bit 폴링 사이의 짧은 지연).
 *   - 디바이스 power state 전환 후 settle 시간.
 *   - 단위 테스트의 결정적 타이밍.
 * 주의: 긴 us 값(>1ms)은 비효율 — poller deadline 기반 비동기 패턴으로 대체 권장.
 *
 * 호출 체인: NVMe reset / power mgmt → spdk_delay_us → spdk_get_ticks 폴링 루프
 */
void spdk_delay_us(unsigned int us);

/**
 * Pause CPU execution for a short while
 */
/*
 * [한국어]
 * spdk_pause - 짧은 spin 대기를 위한 CPU 힌트 (x86 PAUSE 인스트럭션 등)
 *
 * 동작:
 *   - x86: PAUSE (REP NOP) — SMT 형제 코어에 파이프라인 자원 양보 + memory ordering 위반 회피로
 *     분기 예측 패널티 절감. 약 5~140 사이클 idle.
 *   - ARM: YIELD 인스트럭션 (유사 의미).
 *   - 그 외: no-op.
 * 사용처: NVMe CQE polling 의 tight inner loop, atomic CAS 의 spin retry 사이, ring contention 시.
 *   목적은 "내가 잠시 진전할 게 없으니 SMT 형제 코어가 progress 하도록 양보" + "memory snoop 트래픽 절감".
 *
 * 호출 체인: NVMe poller / spin lock → spdk_pause → rte_pause (PAUSE/YIELD)
 */
void spdk_pause(void);

/* [한국어] lockless ring buffer 의 불투명 구조 (DPDK rte_ring 래퍼).
 * SPDK 의 reactor 간 메시지 전달 (spdk_thread_send_msg) 의 기반 자료구조. */
struct spdk_ring;

/* [한국어] ring 의 producer/consumer 동시성 모델.
 * 잘못된 타입 선택은 데이터 손상을 유발 — 호출자는 자기 모델을 정확히 알아야 한다. */
enum spdk_ring_type {
	SPDK_RING_TYPE_SP_SC,		/* Single-producer, single-consumer */
	/* [한국어] 1 producer × 1 consumer. atomic 최소 (쓰기/읽기 인덱스가 단일 소유).
	 * 가장 빠르며 reactor → reactor 메시지(고정 페어) 등에 쓸 수 있으나 SPDK 는 보통 MP_SC 사용. */

	SPDK_RING_TYPE_MP_SC,		/* Multi-producer, single-consumer */
	/* [한국어] N producer → 1 consumer. spdk_thread_send_msg 의 표준 타입 — 누구든 코어 X 에
	 * 메시지 보낼 수 있고 코어 X 만 자기 큐를 dequeue. enqueue 만 atomic CAS. */

	SPDK_RING_TYPE_MP_MC,		/* Multi-producer, multi-consumer */
	/* [한국어] N×M. mempool 글로벌 ring 이 이 타입. enqueue/dequeue 모두 atomic 필요 → 가장 느림. */
};

/**
 * Create a ring.
 *
 * \param type Type for the ring. (SPDK_RING_TYPE_SP_SC or SPDK_RING_TYPE_MP_SC).
 * \param count Size of the ring in elements.
 * \param numa_id NUMA node ID to allocate memory on, or SPDK_ENV_NUMA_ID_ANY
 * for any NUMA node.
 *
 * \return a pointer to the created ring.
 */
/*
 * [한국어]
 * spdk_ring_create - lockless ring 생성
 *
 * @param type: SP_SC / MP_SC / MP_MC 중 하나.
 * @param count: 엔트리 개수. 2의 거듭제곱이어야 함 (rte_ring 제약 — 마스크 연산 위해).
 * @param numa_id: 할당 NUMA 노드.
 * @return: 생성된 ring 또는 NULL.
 *
 * 사용처: spdk_thread 의 메시지 큐(MP_SC), reactor 간 작업 분배.
 * 호출 체인: spdk_thread_create → spdk_ring_create → rte_ring_create
 */
struct spdk_ring *spdk_ring_create(enum spdk_ring_type type, size_t count, int numa_id);

/**
 * Free the ring.
 *
 * \param ring Ring to free.
 */
/*
 * [한국어]
 * spdk_ring_free - lockless ring 해제 (DPDK rte_ring 래퍼 free)
 *
 * @param ring: spdk_ring_create 가 반환한 핸들. NULL 호출 금지.
 *
 * 안전 조건: ring 이 비어있어야 함 — 안에 남아있는 객체 포인터들은 free 되지 않고 손실된다.
 *   spdk_thread 의 메시지 큐라면 모든 send_msg 완료 + dequeue 0 인 상태에서만 호출.
 * 동작: 내부 메타데이터(producer/consumer 인덱스, lcore 캐시 정보) 및 백킹 메모리 해제.
 * 컨텍스트: cleanup 단계 — 메인 스레드에서 1회. 핫패스 금지.
 * caller: spdk_thread 해체 시퀀스, reactor cleanup.
 * callee: rte_ring_free.
 *
 * 호출 체인: spdk_thread_destroy → spdk_ring_free → rte_ring_free
 */
void spdk_ring_free(struct spdk_ring *ring);

/**
 * Get the number of objects in the ring.
 *
 * \param ring the ring.
 *
 * \return the number of objects in the ring.
 */
/*
 * [한국어]
 * spdk_ring_count - 현재 ring 에 enqueued 된 객체 수 (근사값)
 *
 * @param ring: 대상 ring.
 * @return: producer_tail - consumer_head 의 unsigned 차이 (atomic read 한 스냅샷).
 *
 * 정확성: SP_SC ring 은 정확, MP_SC/MP_MC 는 다른 lcore 가 동시에 enqueue/dequeue 하면 즉시
 *   stale 해질 수 있어 "근사값" 으로만 신뢰. 따라서 핫패스의 분기 조건 (예: ring 비어있으면 skip)
 *   으로 사용하면 안 됨 — 실제 비어있음 확인은 spdk_ring_dequeue 의 반환값 0 으로 판정.
 * 사용처: 모니터링, RPC 통계, 백프레셔 추정 (정확도 요구가 낮은 영역).
 * 컨텍스트: 어디서나. 그러나 결과는 항상 race-prone 임을 인지.
 *
 * 호출 체인: RPC/stats 코드 → spdk_ring_count → rte_ring_count
 */
size_t spdk_ring_count(struct spdk_ring *ring);

/**
 * Queue the array of objects (with length count) on the ring.
 *
 * \param ring A pointer to the ring.
 * \param objs A pointer to the array to be queued.
 * \param count Length count of the array of objects.
 * \param free_space If non-NULL, amount of free space after the enqueue has finished.
 *
 * \return the number of objects enqueued.
 */
/*
 * [한국어]
 * spdk_ring_enqueue - ring 에 count 개 객체 push (lockless atomic CAS)
 *
 * @return: 실제 enqueue 된 개수 (ring full 이면 count 보다 작을 수 있음).
 * @param free_space: NULL 아니면 enqueue 후 남은 자리 수 — back-pressure 결정에 사용.
 *
 * 동작: producer head 를 CAS 로 점유 → memcpy → producer tail 업데이트 (linearizable).
 * 컨텍스트: ring type 이 허용하는 producer 컨텍스트에서만 호출.
 */
size_t spdk_ring_enqueue(struct spdk_ring *ring, void **objs, size_t count,
			 size_t *free_space);

/**
 * Dequeue count objects from the ring into the array objs.
 *
 * \param ring A pointer to the ring.
 * \param objs A pointer to the array to be dequeued.
 * \param count Maximum number of elements to be dequeued.
 *
 * \return the number of objects dequeued, which is less than or equal to count.
 */
/*
 * [한국어]
 * spdk_ring_dequeue - ring 에서 최대 count 개 pop
 *
 * @return: 실제 dequeue 개수 (ring 비어있으면 0).
 * 컨텍스트: SC ring 이면 단일 consumer 만, MC 면 어느 컨텍스트에서나 OK.
 * 사용처: reactor 의 메시지 처리 루프 — 매 polling 마다 dequeue 후 핸들러 호출.
 */
size_t spdk_ring_dequeue(struct spdk_ring *ring, void **objs, size_t count);

/**
 * Reports whether the SPDK application is using the IOMMU for DMA
 *
 * \return True if we are using the IOMMU, false otherwise.
 */
/*
 * [한국어]
 * spdk_iommu_is_enabled - 현재 SPDK 프로세스가 IOMMU(VFIO) 모드로 동작 중인지 조회
 *
 * @return: true = IOMMU(VFIO container) 활성, false = no-IOMMU (uio_pci_generic / vfio noiommu).
 *
 * IOMMU 활성:
 *   - spdk_vtophys 가 IOVA(보통 가상주소 = IOVA) 반환. NVMe 는 가상주소를 IOVA 로 그대로 DMA.
 *   - VFIO container 가 IOVA→PA 변환을 처리 → 안전 (다른 메모리에 접근 불가).
 *   - 일반 사용자도 권한 부여만 받으면 사용 가능 (root 불필요).
 *   - iova_mode = "va" 가 디폴트 결정.
 * IOMMU 미활성:
 *   - vtophys 가 실제 PA 반환 (Linux /proc/self/pagemap 으로 PA 조회 필요 → CAP_SYS_ADMIN 또는 root).
 *   - 디바이스가 임의 PA 에 DMA 가능 → 보안 취약 (rogue NIC 이 커널 메모리 손상 가능).
 *   - iova_mode = "pa".
 * 사용처: 드라이버가 unsafe-mode 경고 출력, 외부 DMA 등록(RDMA MR) 전략 분기, 보안 정책 결정.
 *
 * 호출 체인: NVMe init / RDMA init / RPC stats → spdk_iommu_is_enabled → 내부 플래그
 */
bool spdk_iommu_is_enabled(void);

/* [한국어] vtophys 가 변환 실패 시 반환하는 sentinel. 모든 비트 1 = 절대 유효한 물리주소가 아님.
 * 호출자는 == SPDK_VTOPHYS_ERROR 비교로 실패 처리. */
#define SPDK_VTOPHYS_ERROR	(0xFFFFFFFFFFFFFFFFULL)

/**
 * Get the physical address of a buffer.
 *
 * \param buf A pointer to a buffer.
 * \param size Contains the size of the memory region pointed to by vaddr.
 * If vaddr is successfully translated, then this is updated with the size of
 * the memory region for which the translation is valid.
 *
 * \return the physical address of this buffer on success, or SPDK_VTOPHYS_ERROR
 * on failure.
 */
/*
 * [한국어]
 * spdk_vtophys - 가상주소 → IOVA(또는 물리주소) 변환 (DMA 의 핵심)
 *
 * @param buf: spdk_malloc/zmalloc/dma_* 또는 memzone 으로 잡은 버퍼.
 * @param size: in/out — 입력은 호출자 관심 영역 크기, 출력은 같은 IOVA 매핑이 유효한
 *              연속 영역 크기. 페이지 경계에서 줄어들 수 있어 DMA 시 page-by-page 처리 필요.
 * @return: IOVA (성공) 또는 SPDK_VTOPHYS_ERROR (실패).
 *
 * NVMe PRP 작성 흐름: 사용자 버퍼 → spdk_vtophys 로 IOVA 획득 → PRP1/PRP2/PRP_LIST 에 기록 →
 * NVMe SQE 발행 → 컨트롤러가 IOVA 로 DMA → IOMMU(VFIO) 가 IOVA→PA 변환 → DRAM 접근.
 * IOMMU 비활성 시에는 직접 PA 가 반환되며 그대로 PCI 버스 주소로 사용된다.
 *
 * 동작: 내부 spdk_mem_map 에서 vaddr 의 페이지 매핑을 lookup. 매핑되지 않은 영역(일반 malloc 등)
 *       은 SPDK_VTOPHYS_ERROR. spdk_mem_register 로 등록된 외부 메모리도 변환 가능.
 * 컨텍스트: 핫패스에서 매 I/O 마다 호출 — O(1) (radix tree lookup). 캐시 친화적.
 *
 * 호출 체인: NVMe req 빌드 → spdk_vtophys → spdk_mem_map_translate → IOVA
 */
uint64_t spdk_vtophys(const void *buf, uint64_t *size);

/* [한국어] PCI BDF(Bus/Device/Function) + Domain 4튜플. PCI Express 식별자 표준. */
struct spdk_pci_addr {
	uint32_t			domain;
	/* [한국어] PCI domain (segment) 번호. 일반 데스크톱은 0, NUMA 다수 도메인 서버에서 0..N.
	 * 설정자: spdk_pci_addr_parse / DPDK enum. 읽는 자: 식별/매칭. */

	uint8_t				bus;
	/* [한국어] PCI bus 번호 0..255. lspci 의 첫 두 자리. */

	uint8_t				dev;
	/* [한국어] device 번호 0..31 (5비트만 유효, 나머지는 0). */

	uint8_t				func;
	/* [한국어] function 번호 0..7. SR-IOV VF 는 같은 dev 의 다른 func 으로 구분. */
};

/* [한국어] PCI 디바이스 식별자 — 드라이버의 ID 테이블에 사용 (vendor:device 매칭). */
struct spdk_pci_id {
	uint32_t	class_id;	/**< Class ID or SPDK_PCI_CLASS_ANY_ID. */
	/* [한국어] PCI Class code (예: 0x010802 = NVMe). SPDK_PCI_CLASS_ANY_ID 면 any.
	 * 보통 드라이버는 vendor/device 만 매칭하고 class 는 ANY. */

	uint16_t	vendor_id;	/**< Vendor ID or SPDK_PCI_ANY_ID. */
	/* [한국어] PCI vendor ID (예: 0x8086 = Intel, 0x144d = Samsung). */

	uint16_t	device_id;	/**< Device ID or SPDK_PCI_ANY_ID. */
	/* [한국어] vendor 별 device ID. ANY 면 vendor 전체 매칭. */

	uint16_t	subvendor_id;	/**< Subsystem vendor ID or SPDK_PCI_ANY_ID. */
	/* [한국어] subsystem vendor (보드 제조사). 보통 ANY. */

	uint16_t	subdevice_id;	/**< Subsystem device ID or SPDK_PCI_ANY_ID. */
	/* [한국어] subsystem device. 보통 ANY. */
};

/** Device needs PCI BAR mapping (done with either IGB_UIO or VFIO) */
/* [한국어] 드라이버 등록 시 플래그: BAR(MMIO) 영역의 사용자공간 매핑 필요 — NVMe 는 doorbell
 * 접근에 BAR 매핑이 필수이므로 설정. UIO/VFIO 둘 중 하나로 매핑 가능. */
#define SPDK_PCI_DRIVER_NEED_MAPPING 0x0001
/** Device needs PCI BAR mapping with enabled write combining (wc) */
/* [한국어] BAR 매핑 시 write-combining(WC) 활성 — 다중 인접 32-bit 쓰기를 burst 로 합쳐
 * PCI 효율 극대화. NVMe doorbell 처럼 빈번한 인접 쓰기 영역에 유리. */
#define SPDK_PCI_DRIVER_WC_ACTIVATE 0x0002

/*
 * [한국어]
 * spdk_pci_driver_register - SPDK 에 PCI 드라이버 등록 (이름 + ID 테이블 + 플래그)
 *
 * 보통 직접 호출하지 않고 SPDK_PCI_DRIVER_REGISTER 매크로 (constructor 속성) 로 등록.
 * 등록된 드라이버는 spdk_pci_enumerate(driver, ...) 의 매칭 대상이 되며, ID 테이블의 모든
 * vendor/device/class 패턴이 BDF 디바이스에 매칭되면 attach 시도.
 *
 * 호출 체인: __attribute__((constructor)) → spdk_pci_driver_register → 글로벌 driver list TAILQ
 */
void spdk_pci_driver_register(const char *name, struct spdk_pci_id *id_table, uint32_t flags);

/* [한국어] SPDK 의 PCI 디바이스 핸들 — 환경 백엔드(env_dpdk 또는 custom provider) 가 채워
 * 드라이버에 전달하는 포터블 디스크립터. NVMe 드라이버는 이 구조체를 받아 BAR 매핑/cfg R/W 한다. */
struct spdk_pci_device {
	struct spdk_pci_device		*parent;
	/* [한국어] VMD(Volume Management Device) 같이 부모 PCI 디바이스 아래에 가상 자식 디바이스가
	 * 있을 때의 부모 포인터. 일반 NVMe 면 NULL.
	 * 설정자: env_dpdk 또는 VMD 드라이버. 읽는 자: detach 시 부모 정리 순서 결정. */

	void				*dev_handle;
	/* [한국어] 백엔드 고유 핸들 (env_dpdk 에서는 struct rte_pci_device*).
	 * 외부에서 직접 디리퍼런스 금지 — 백엔드 함수 포인터를 통해서만 접근. */

	struct spdk_pci_addr		addr;
	/* [한국어] 이 디바이스의 BDF. 식별과 로깅에 사용. enumerate 시 채워짐. */

	struct spdk_pci_id		id;
	/* [한국어] vendor/device/class. 드라이버 ID 테이블 매칭 결과. */

	union {
		int			numa_id;
		/* [한국어] 디바이스가 물리적으로 연결된 NUMA 노드 (보통 sysfs/numa_node 에서 읽음).
		 * I/O 성능 최적화: 같은 NUMA 의 코어를 reactor 로 배치. */
		int			socket_id; /* Legacy name for this field */
		/* [한국어] DPDK 레거시 명칭 (== numa_id). 두 이름은 union 으로 같은 메모리 공유. */
	};
	const char			*type;
	/* [한국어] 디바이스 type 문자열 (provider 이름). "pci" (env_dpdk 기본) 또는 custom provider 이름.
	 * detach 시 어느 provider 가 처리할지 결정 (dev->type == provider->name 매칭). */

	int (*map_bar)(struct spdk_pci_device *dev, uint32_t bar,
		       void **mapped_addr, uint64_t *phys_addr, uint64_t *size);
	/* [한국어] BAR 매핑 함수 포인터. env_dpdk 기본은 rte_pci_map_device 기반 매핑.
	 * custom provider 가 자체 매핑 로직 (예: emulated device) 을 넣을 수 있어 함수 포인터화. */

	int (*unmap_bar)(struct spdk_pci_device *dev, uint32_t bar,
			 void *addr);
	/* [한국어] map_bar 의 짝. detach 시 자동 호출. */

	int (*cfg_read)(struct spdk_pci_device *dev, void *value,
			uint32_t len, uint32_t offset);
	/* [한국어] PCI config space 읽기 (4096 바이트 영역). VFIO 면 ioctl(VFIO_DEVICE_GET_REGION) 경유.
	 * provider 별 구현으로 emulated device 지원. */

	int (*cfg_write)(struct spdk_pci_device *dev, void *value,
			 uint32_t len, uint32_t offset);
	/* [한국어] config space 쓰기. NVMe Reset, MSI-X 활성, BME(Bus Master Enable) 등에 사용. */

	struct _spdk_pci_device_internal {
		/* [한국어] 외부 코드가 직접 만질 수 없는 SPDK 내부 상태. 이름 prefix _spdk 로 사인. */

		struct spdk_pci_driver		*driver;
		/* [한국어] 이 디바이스를 attach 한 SPDK 드라이버 포인터. detach 시 unwind 에 사용. */

		bool				attached;
		/* [한국어] true 면 사용자 콜백이 0 반환하여 attach 성공한 상태. detach 가 false 로 변경. */

		/* optional fd for exclusive access to this device on this process */
		int				claim_fd;
		/* [한국어] spdk_pci_device_claim 으로 받은 lockfile fd. F_SETLK 가 걸려있어 다른 프로세스의
		 * claim 차단. -1 = 미점유. unclaim/detach 시 close 되어 락 해제. */

		bool				pending_removal;
		/* [한국어] 핫리무브 신호 (uevent) 가 감지되어 곧 제거 예정. 드라이버는 새 IO 발행 중단해야 함. */

		/* The device was successfully removed on a DPDK interrupt thread,
		 * but to prevent data races we couldn't remove it from the global
		 * device list right away. It'll be removed as soon as possible
		 * on a regular thread when any public pci function is called.
		 */
		bool				removed;
		/* [한국어] 실제 제거 완료 — 안전한 시점에 글로벌 리스트에서 unlink 예정.
		 * 인터럽트 스레드에서 직접 unlink 하면 다른 reactor 와 데이터 레이스 가능 → 지연 제거 패턴. */

		TAILQ_ENTRY(spdk_pci_device)	tailq;
		/* [한국어] 글로벌 PCI 디바이스 TAILQ 의 노드. spdk_pci_for_each_device 가 순회.
		 * 동기화: 등록/제거는 init/cleanup 또는 main 스레드에서, 순회는 lock-free 가정. */
	} internal;
};

/**
 * Callback for device attach handling.
 *
 * \param enum_ctx Opaque value.
 * \param dev PCI device.
 *
 * \return -1 if an error occurred,
 *          0 if device attached successfully,
 *          1 if device not attached.
 */
/* [한국어] spdk_pci_enumerate / spdk_pci_device_attach 가 매칭된 디바이스마다 호출하는 콜백.
 * 드라이버는 이 콜백 안에서 BAR 매핑·NVMe controller 객체 생성 등 attach 작업을 수행한다.
 * @return: -1 = 치명 오류 (enumerate 중단), 0 = attach 성공 (dev 보존), 1 = pass (다음 디바이스). */
typedef int (*spdk_pci_enum_cb)(void *enum_ctx, struct spdk_pci_device *dev);

/* [한국어] 드라이버 ID 테이블 한 엔트리를 간결히 작성하는 매크로.
 * 사용 예: { SPDK_PCI_DEVICE(SPDK_PCI_VID_INTEL, 0x0953) } — class/sub*는 ANY 로 채움.
 * struct spdk_pci_id 의 designated initializer 로 확장된다. */
#define SPDK_PCI_DEVICE(vend, dev)          \
	.class_id = SPDK_PCI_CLASS_ANY_ID,      \
	.vendor_id = (vend),                    \
	.device_id = (dev),                     \
	.subvendor_id = SPDK_PCI_ANY_ID,        \
	.subdevice_id = SPDK_PCI_ANY_ID

/* [한국어] PCI 드라이버 자동 등록 매크로. __attribute__((constructor)) 로 main 진입 전 실행되어
 * 사용자가 spdk_pci_driver_register 를 명시적으로 호출하지 않아도 된다.
 * #name 으로 stringify, ## 토큰 결합으로 unique 함수명 생성 — 여러 드라이버가 같은 TU 에 있어도
 * 심볼 충돌 없음. */
#define SPDK_PCI_DRIVER_REGISTER(name, id_table, flags) \
__attribute__((constructor)) static void _spdk_pci_driver_register_##name(void) \
{ \
	spdk_pci_driver_register(#name, id_table, flags); \
}

/**
 * Get the VMD PCI driver object.
 *
 * \return PCI driver.
 */
/*
 * [한국어]
 * spdk_pci_vmd_get_driver - VMD(Volume Management Device) PCI 드라이버 핸들 조회
 *
 * @return: lib/vmd 가 register 한 spdk_pci_driver 포인터.
 *
 * VMD = Intel CPU 가 보유한 PCIe 가상화 컨트롤러로, 아래에 달린 자식 NVMe 들을 단일 PCI 디바이스
 *   처럼 호스트에 노출. 핫플러그/관리 일원화의 이점이 있으나 SPDK 가 자식 NVMe 에 접근하려면
 *   VMD 드라이버로 한 번 enumerate 한 뒤 그 자식들을 다시 NVMe 드라이버로 enumerate 해야 한다.
 *
 * 사용처: spdk_pci_enumerate(spdk_pci_vmd_get_driver(), ...) — VMD 디바이스만 attach.
 *
 * 호출 체인: NVMe init (VMD 옵션 활성) → spdk_pci_vmd_get_driver → spdk_pci_enumerate
 */
struct spdk_pci_driver *spdk_pci_vmd_get_driver(void);

/**
 * Get the I/OAT PCI driver object.
 *
 * \return PCI driver.
 */
/*
 * [한국어]
 * spdk_pci_ioat_get_driver - I/OAT(Intel I/O Acceleration Technology) DMA 엔진 PCI 드라이버 핸들
 *
 * @return: lib/ioat 가 register 한 spdk_pci_driver 포인터.
 *
 * I/OAT = Intel Xeon CPU 의 내장 DMA 엔진 (Crystal Beach). CPU 코어를 사용하지 않고 메모리-메모리
 *   복사를 오프로드 — bdev_copy, blob CoW, NVMe-oF target 의 데이터 복사 경로 가속에 사용.
 *
 * 사용처: spdk_pci_enumerate(spdk_pci_ioat_get_driver(), ...) — I/OAT 채널 attach.
 *
 * 호출 체인: copy accelerator init → spdk_pci_ioat_get_driver → spdk_pci_enumerate
 */
struct spdk_pci_driver *spdk_pci_ioat_get_driver(void);

/**
 * Get the IDXD PCI driver object.
 *
 * \return PCI driver.
 */
/*
 * [한국어]
 * spdk_pci_idxd_get_driver - IDXD(Intel Data Streaming Accelerator, "DSA") PCI 드라이버 핸들
 *
 * @return: lib/idxd 가 register 한 spdk_pci_driver 포인터.
 *
 * IDXD = Intel Xeon Sapphire Rapids 이후의 차세대 가속기. I/OAT 의 후속으로 memcpy/CRC/compress/
 *   fill/compare 를 SVA(Shared Virtual Addressing) 기반으로 오프로드. 사용자 공간에서 ENQCMD(S)
 *   인스트럭션으로 work submission descriptor 를 디바이스에 직접 푸시.
 *
 * 사용처: bdev/blob 의 dataintegrity(CRC), copy 가속, NVMe-oF crc32 계산 등에 활용.
 *
 * 호출 체인: accel module init → spdk_pci_idxd_get_driver → spdk_pci_enumerate
 */
struct spdk_pci_driver *spdk_pci_idxd_get_driver(void);

/**
 * Get the AE4DMA PCI driver object.
 *
 * \return PCI driver.
 */
/*
 * [한국어]
 * spdk_pci_ae4dma_get_driver - AMD AE4DMA(AMD Engine for DMA) PCI 드라이버 핸들
 *
 * @return: lib/ae4dma 가 register 한 spdk_pci_driver 포인터.
 *
 * AE4DMA = AMD EPYC 세대의 DMA/가속 엔진. Intel IDXD/I/OAT 와 유사한 메모리-메모리 복사 오프로드
 *   기능 제공. SPDK accel framework 가 플랫폼에 따라 IDXD 또는 AE4DMA 를 선택 사용.
 *
 * 사용처: AMD 플랫폼에서 copy/CRC 가속.
 *
 * 호출 체인: accel module init (AMD) → spdk_pci_ae4dma_get_driver → spdk_pci_enumerate
 */
struct spdk_pci_driver *spdk_pci_ae4dma_get_driver(void);

/**
 * Get the Virtio PCI driver object.
 *
 * \return PCI driver.
 */
/*
 * [한국어]
 * spdk_pci_virtio_get_driver - Virtio PCI 드라이버 핸들 (virtio-blk/scsi)
 *
 * @return: lib/virtio 가 register 한 spdk_pci_driver 포인터.
 *
 * Virtio = QEMU/KVM 가상화 환경의 paravirtual 디바이스 표준. SPDK 가 게스트 VM 안에서 동작할 때
 *   호스트가 제공한 virtio-blk/virtio-scsi 디바이스를 polled-mode 로 attach 하기 위함.
 *   bdev_virtio 백엔드 모듈의 기반.
 *
 * 사용처: VM 안에서 SPDK 가 실행되며 virtio-blk 를 bdev 로 노출하는 시나리오.
 *
 * 호출 체인: bdev_virtio init → spdk_pci_virtio_get_driver → spdk_pci_enumerate
 */
struct spdk_pci_driver *spdk_pci_virtio_get_driver(void);

/**
 * Get PCI driver by name (e.g. "nvme", "vmd", "ioat").
 */
/*
 * [한국어]
 * spdk_pci_get_driver - 이름 문자열로 등록된 PCI 드라이버 검색
 *
 * @param name: spdk_pci_driver_register 시 사용한 이름 ("nvme", "vmd", "ioat", "idxd", "virtio" 등).
 * @return: 매칭된 드라이버 포인터, 없으면 NULL.
 *
 * 동작: 글로벌 드라이버 TAILQ 를 선형 검색하여 strcmp 매칭.
 * 사용처: RPC 핸들러가 사용자 인자(문자열)로 드라이버를 동적 선택할 때.
 *   하드코딩된 spdk_pci_nvme_get_driver() 와 달리 런타임 lookup.
 *
 * 호출 체인: RPC handler → spdk_pci_get_driver(name) → 드라이버 TAILQ 검색
 */
struct spdk_pci_driver *spdk_pci_get_driver(const char *name);

/**
 * Get the NVMe PCI driver object.
 *
 * \return PCI driver.
 */
/*
 * [한국어]
 * spdk_pci_nvme_get_driver - NVMe PCI 드라이버 핸들 (SPDK 의 가장 핵심 드라이버)
 *
 * @return: lib/nvme 가 register 한 spdk_pci_driver 포인터.
 *
 * NVMe 드라이버: lib/nvme 가 부팅 시 NVMe vendor/class (PCI class 0x010802 = NVM Express)
 *   ID 테이블을 가지고 spdk_pci_driver_register 호출. spdk_nvme_probe 는 이 핸들로
 *   spdk_pci_enumerate 를 호출하여 모든 NVMe SSD 를 attach 한다.
 * NEED_MAPPING 플래그가 설정되어 있어 enumerate 단계에서 자동으로 BAR0(controller register) 매핑.
 *
 * 사용처: spdk_nvme_probe / spdk_nvme_connect / RPC nvme_attach_controller 등.
 *
 * 호출 체인: spdk_nvme_probe → spdk_pci_nvme_get_driver → spdk_pci_enumerate
 */
struct spdk_pci_driver *spdk_pci_nvme_get_driver(void);

/**
 * Enumerate all PCI devices supported by the provided driver and try to
 * attach those that weren't attached yet. The provided callback will be
 * called for each such device and its return code will decide whether that
 * device is attached or not. Attached devices have to be manually detached
 * with spdk_pci_device_detach() to be attach-able again.
 *
 * During enumeration all registered pci devices with exposed access to
 * userspace are getting probed internally unless not explicitly specified
 * on denylist. Because of that it becomes not possible to either use such
 * devices with another application or unbind the driver (e.g. vfio).
 *
 * 2s asynchronous delay is introduced to avoid race conditions between
 * user space software initialization and in-kernel device handling for
 * newly inserted devices. Subsequent enumerate call after the delay
 * shall allow for a successful device attachment.
 *
 * \param driver Driver for a specific device type.
 * \param enum_cb Callback to be called for each non-attached PCI device.
 * \param enum_ctx Additional context passed to the callback function.
 *
 * \return -1 if an internal error occurred or the provided callback returned -1,
 *         0 otherwise
 */
/*
 * [한국어]
 * spdk_pci_enumerate - 드라이버에 매칭되는 모든 PCI 디바이스 탐색 + attach 시도
 *
 * @param driver: 어떤 드라이버 ID 테이블로 매칭할지 (예: spdk_pci_nvme_get_driver()).
 * @param enum_cb: 각 매칭 디바이스마다 호출, 0 반환하면 attach 보존.
 * @param enum_ctx: enum_cb 에 전달되는 컨텍스트.
 * @return: 0 성공, -1 내부 오류 또는 cb 가 -1 반환.
 *
 * 동작 단계:
 *   1) DPDK PCI bus 의 등록 디바이스를 모두 스캔
 *   2) 드라이버 ID 테이블과 BDF 매칭 + pci_allowed/blocked 필터
 *   3) 매칭된 각 디바이스에 대해 BAR 매핑 (NEED_MAPPING 플래그) → enum_cb 호출
 *   4) 0 반환 디바이스는 attached=true 로 보존, 그 외는 BAR unmap
 *   5) 처음 호출 시 신규 디바이스에는 2초 비동기 지연이 적용 (in-kernel device probing 충돌 회피)
 *
 * 컨텍스트: 보통 init 단계 메인 스레드. 핫패스 호출 X.
 * caller: spdk_nvme_probe / IOAT init.
 * callee: env_dpdk 가 rte_pci_probe 와 ID 매칭 루프 + spdk_pci_device_map_bar.
 *
 * 호출 체인: spdk_nvme_probe → spdk_pci_enumerate(nvme_driver, attach_cb) → 디바이스별 attach_cb →
 *           NVMe controller 객체 생성 + qpair 셋업
 */
int spdk_pci_enumerate(struct spdk_pci_driver *driver, spdk_pci_enum_cb enum_cb, void *enum_ctx);

/**
 * Call the provided function pointer for every enumerated PCI device.
 *
 * \param ctx Context parameter to pass to fn.
 * \param fn Function to call for each PCI device
 */
/*
 * [한국어]
 * spdk_pci_for_each_device - 등록된 모든 PCI 디바이스 순회 (드라이버 무관, 단순 iterate)
 *
 * 사용처: RPC "list pci devices", 디버그 dump, 통계 수집.
 * 컨텍스트: lock-free 순회 (글로벌 TAILQ) — 동시 attach/detach 가능성에 주의.
 */
void spdk_pci_for_each_device(void *ctx, void (*fn)(void *ctx, struct spdk_pci_device *dev));

/**
 * Map a PCI BAR in the current process.
 *
 * \param dev PCI device.
 * \param bar BAR number.
 * \param mapped_addr A variable to store the virtual address of the mapping.
 * \param phys_addr A variable to store the physical address of the mapping.
 * \param size A variable to store the size of the bar (in bytes).
 *
 * \return 0 on success.
 */
/*
 * [한국어]
 * spdk_pci_device_map_bar - PCI BAR (MMIO 영역) 을 현재 프로세스 가상주소공간에 매핑
 *
 * @param bar: BAR 번호 (0..5). NVMe 는 BAR0 가 controller register 영역(NVMe 1.x §3).
 * @param mapped_addr: 출력 — 매핑된 가상주소 (이후 일반 포인터 dereference 로 MMIO 가능).
 * @param phys_addr: 출력 — BAR 물리주소 (디버그/외부 매핑용).
 * @param size: 출력 — BAR 크기.
 * @return: 0 성공.
 *
 * 동작: VFIO 면 ioctl(VFIO_DEVICE_GET_REGION_INFO) + mmap; UIO 면 /dev/uioN 의 MMIO mmap.
 * 결과: NVMe 드라이버는 mapped_addr 에서 doorbell 오프셋(SQyTDBL/CQyHDBL) 에 32-bit write 로
 *       SQ tail / CQ head 갱신 — 이게 polled-mode 의 "I/O submit/complete" 의 본질.
 *
 * 호출 체인: NVMe attach_cb → spdk_pci_device_map_bar → dev->map_bar (env_dpdk: rte_pci_map_device)
 */
int spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			    void **mapped_addr, uint64_t *phys_addr, uint64_t *size);

/**
 * Unmap a PCI BAR from the current process. This happens automatically when
 * the PCI device is detached.
 *
 * \param dev PCI device.
 * \param bar BAR number.
 * \param mapped_addr Virtual address of the bar.
 *
 * \return 0 on success.
 */
/*
 * [한국어]
 * spdk_pci_device_unmap_bar - PCI BAR 매핑 해제 (map_bar 의 짝)
 *
 * @param dev: PCI 디바이스.
 * @param bar: BAR 번호.
 * @param mapped_addr: map_bar 가 반환했던 가상주소.
 * @return: 0 성공.
 *
 * 동작: dev->unmap_bar 호출 — env_dpdk 면 munmap + VFIO region 해제.
 * 호출 시점: spdk_pci_device_detach 가 attached BAR 들을 자동 unmap 하므로 사용자가 직접 부를 일은
 *   거의 없다. 단, BAR 를 일시적으로 unmap 했다가 다시 map 하는 특수 시나리오에서 명시 호출.
 *
 * 호출 체인: spdk_pci_device_detach → spdk_pci_device_unmap_bar → dev->unmap_bar (munmap)
 */
int spdk_pci_device_unmap_bar(struct spdk_pci_device *dev, uint32_t bar,
			      void *mapped_addr);

/**
 * Enable PCI device interrupts. (Experimental)
 *
 * \param dev PCI device.
 *
 * \return 0 on success, negative value on error.
 */
/*
 * [한국어]
 * spdk_pci_device_enable_interrupt (실험적)
 *
 * 디바이스의 INTx (legacy) 또는 MSI 인터럽트를 활성. 보통 SPDK 는 polled-mode 라 사용 안 함이지만,
 * idle 시 절전을 위해 interrupt-mode 로 전환할 때 사용 (lib/nvme 의 interrupt-mode 옵션).
 * 호출 체인: NVMe init 에서 interrupt-mode 옵션 시 → spdk_pci_device_enable_interrupt → VFIO_DEVICE_SET_IRQS
 */
int spdk_pci_device_enable_interrupt(struct spdk_pci_device *dev);

/**
 * Disable PCI device interrupts. (Experimental)
 *
 * \param dev PCI device.
 *
 * \return 0 on success, negative value on error.
 */
/*
 * [한국어]
 * spdk_pci_device_disable_interrupt - PCI 인터럽트 비활성 (enable 의 짝, 실험적)
 *
 * @param dev: PCI 디바이스.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 동작: VFIO_DEVICE_SET_IRQS 로 INTx/MSI 비활성. 등록된 eventfd 도 자동 close.
 * 사용처: polled-mode 로 복귀하거나 디바이스 종료 직전. 핫패스에서는 호출 X.
 *
 * 호출 체인: NVMe shutdown / mode 전환 → spdk_pci_device_disable_interrupt → VFIO_DEVICE_SET_IRQS
 */
int spdk_pci_device_disable_interrupt(struct spdk_pci_device *dev);

/**
 * Get an event file descriptor associated with a PCI device interrupt.
 * (Experimental)
 *
 * \param dev PCI device.
 *
 * \return Event file descriptor on success, negative value on error.
 */
/*
 * [한국어]
 * spdk_pci_device_get_interrupt_efd - 인터럽트와 연결된 eventfd 반환
 *
 * 사용처: epoll/poll 루프에 efd 등록 → IRQ 발생 시 read 가능 → interrupt-mode 폴 대기.
 * VFIO 가 IRQ → eventfd write 로 변환해주는 메커니즘 활용.
 */
int spdk_pci_device_get_interrupt_efd(struct spdk_pci_device *dev);

/**
 * Enable PCI device interrupts, only if VFIO MSI-X is supported.
 * This creates a bunch of event file descriptors, for which VFIO IRQ are set,
 * which then can be used to enable interrupts.
 *
 * \param dev PCI device.
 * \param efd_count Number of event fds to create.
 *
 * \return 0 on success, negative value on error.
 */
/*
 * [한국어]
 * spdk_pci_device_enable_interrupts - MSI-X 다중 벡터 활성 (VFIO 전용)
 *
 * NVMe 는 큐별 MSI-X 벡터를 가질 수 있어 efd_count = qpair 수만큼 생성 (qpair 별 인터럽트 분리).
 * 동작: VFIO_DEVICE_SET_IRQS 로 efd_count 개의 eventfd 를 IRQ 와 매핑.
 */
int spdk_pci_device_enable_interrupts(struct spdk_pci_device *dev, uint32_t efd_count);

/**
 * Disable PCI device interrupts.
 * This disables the MSI-X interrupts for all the created event file descriptors
 * and frees them.
 *
 * \param dev PCI device.
 *
 * \return 0 on success, negative value on error.
 */
/*
 * [한국어]
 * spdk_pci_device_disable_interrupts - MSI-X 다중 벡터 비활성 (enable_interrupts 의 짝)
 *
 * @param dev: PCI 디바이스.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 동작: VFIO_DEVICE_SET_IRQS (count=0) 호출 → 모든 efd close 및 IRQ 라우팅 해제.
 *   enable_interrupts 가 생성한 efd 들이 자동 정리되므로 호출자가 따로 close 할 필요 X.
 * 사용처: NVMe qpair 들의 일괄 종료, interrupt-mode → polled-mode 전환.
 *
 * 호출 체인: NVMe controller shutdown → spdk_pci_device_disable_interrupts → VFIO_DEVICE_SET_IRQS
 */
int spdk_pci_device_disable_interrupts(struct spdk_pci_device *dev);

/**
 * Get an event file descriptor associated with a PCI device, for a particular
 * MSI-X index.
 *
 * \param dev PCI device.
 * \param index event file descriptor index.
 *
 * \return Event file descriptor on success, negative value on error.
 */
/*
 * [한국어]
 * spdk_pci_device_get_interrupt_efd_by_index - 특정 MSI-X 벡터 인덱스의 eventfd 조회
 *
 * @param dev: PCI 디바이스.
 * @param index: MSI-X 벡터 인덱스 (spdk_pci_device_enable_interrupts 의 efd_count 범위 내).
 * @return: 해당 인덱스 eventfd (>= 0) 또는 음수 errno.
 *
 * 동작: VFIO 가 enable 시 생성해둔 efd 배열에서 index 번째 fd 반환.
 * 사용처: NVMe qpair N 의 인터럽트 신호를 받기 위해 index=N 의 efd 를 reactor 의 epoll set 에
 *   등록 → IRQ 발생 시 epoll 이 깨어나 해당 qpair 의 CQ 를 폴링. polled-mode 의 보완형 idle 절전.
 *
 * 호출 체인: NVMe qpair init (interrupt-mode) → spdk_pci_device_get_interrupt_efd_by_index → epoll_ctl
 */
int spdk_pci_device_get_interrupt_efd_by_index(struct spdk_pci_device *dev, uint32_t index);

/**
 * Get the domain of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return PCI device domain.
 */
/*
 * [한국어]
 * spdk_pci_device_get_domain - PCI 디바이스의 domain(segment) 번호 조회
 *
 * @param dev: attached PCI 디바이스 핸들.
 * @return: 16/32-bit PCI domain. 일반 시스템은 0, NUMA 다수 도메인 서버에서 0..N.
 *
 * 동작: dev->addr.domain 단순 반환. struct 접근을 함수 호출로 감싸 ABI/구현 캡슐화.
 * 사용처: 로깅("0000:5e:00.0"), BDF 비교, RPC 응답 직렬화.
 * 컨텍스트: 어디서나. dev 가 valid 한 동안 안전.
 *
 * 호출 체인: 로깅/RPC 핸들러 → spdk_pci_device_get_domain → dev->addr.domain
 */
uint32_t spdk_pci_device_get_domain(struct spdk_pci_device *dev);

/**
 * Get the bus number of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return PCI bus number.
 */
/*
 * [한국어]
 * spdk_pci_device_get_bus - PCI bus 번호 조회
 *
 * @param dev: attached PCI 디바이스.
 * @return: PCI bus 번호 0..255 (BDF 의 B).
 *
 * 동작: dev->addr.bus 반환. PCIe 토폴로지에서 root complex 직속 또는 bridge 하위 bus 번호.
 * 사용처: BDF 비교, RPC 응답 직렬화, NUMA-local 디바이스 탐색.
 *
 * 호출 체인: 사용자/RPC → spdk_pci_device_get_bus → dev->addr.bus
 */
uint8_t spdk_pci_device_get_bus(struct spdk_pci_device *dev);

/**
 * Get the device number within the PCI bus the device is on.
 *
 * \param dev PCI device.
 *
 * \return PCI device number.
 */
/*
 * [한국어]
 * spdk_pci_device_get_dev - PCI device 번호 조회 (BDF 의 D)
 *
 * @param dev: attached PCI 디바이스.
 * @return: 0..31 (PCI spec: 5비트만 유효). 같은 bus 내 슬롯 식별자.
 *
 * 동작: dev->addr.dev 반환.
 * 사용처: BDF 비교, RPC, 슬롯 단위 정책 (예: 슬롯별 다른 timeout).
 *
 * 호출 체인: 사용자/RPC → spdk_pci_device_get_dev → dev->addr.dev
 */
uint8_t spdk_pci_device_get_dev(struct spdk_pci_device *dev);

/**
 * Get the particular function number represented by struct spdk_pci_device.
 *
 * \param dev PCI device.
 *
 * \return PCI function number.
 */
/*
 * [한국어]
 * spdk_pci_device_get_func - PCI function 번호 조회 (BDF 의 F)
 *
 * @param dev: attached PCI 디바이스.
 * @return: 0..7 (PCI multi-function 디바이스의 function index).
 *
 * 동작: dev->addr.func 반환. SR-IOV 환경에서는 PF 가 func=0, VF 가 그 외 func.
 *   같은 dev 의 다른 func 은 logical 디바이스로 별개 BDF 를 부여받는다.
 * 사용처: SR-IOV VF 구분, multi-function NIC/NVMe 식별.
 *
 * 호출 체인: 사용자/RPC → spdk_pci_device_get_func → dev->addr.func
 */
uint8_t spdk_pci_device_get_func(struct spdk_pci_device *dev);

/**
 * Get the full DomainBDF address of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return PCI address.
 */
/*
 * [한국어]
 * spdk_pci_device_get_addr - 4튜플 BDF 구조체 통째 반환 (값 복사)
 *
 * @param dev: attached PCI 디바이스.
 * @return: { domain, bus, dev, func } 4필드를 값으로 복사한 spdk_pci_addr.
 *
 * 동작: dev->addr 를 값으로 반환. 호출자는 결과를 spdk_pci_addr_fmt 로 문자열화하거나
 *   spdk_pci_addr_compare 로 다른 BDF 와 비교할 수 있다.
 *   값 반환이라 dev 가 detach 되어도 호출자가 들고 있는 복사본은 유효.
 * 사용처: RPC 응답, 로깅, allowed/blocked 리스트 검색.
 *
 * 호출 체인: NVMe driver/RPC → spdk_pci_device_get_addr → dev->addr (값 복사)
 */
struct spdk_pci_addr spdk_pci_device_get_addr(struct spdk_pci_device *dev);

/**
 * Get the vendor ID of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return vendor ID.
 */
/*
 * [한국어]
 * spdk_pci_device_get_vendor_id - PCI vendor ID 조회 (드라이버 ID 매칭의 기본)
 *
 * @param dev: attached PCI 디바이스.
 * @return: 16-bit vendor ID (예: 0x8086 = Intel, 0x144d = Samsung, 0x1b36 = Red Hat/QEMU virtio).
 *
 * 동작: dev->id.vendor_id 반환. PCI config space offset 0x00 의 첫 2바이트가 vendor ID 이며,
 *   env_dpdk 가 attach 시점에 cfg_read16(0x00) 으로 채워둔다.
 * 사용처: vendor-specific quirk 분기 (예: Samsung NVMe 특정 reset 시퀀스), 로깅, RPC.
 *
 * 호출 체인: NVMe quirk 분기 / RPC → spdk_pci_device_get_vendor_id → dev->id.vendor_id
 */
uint16_t spdk_pci_device_get_vendor_id(struct spdk_pci_device *dev);

/**
 * Get the device ID of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return device ID.
 */
/*
 * [한국어]
 * spdk_pci_device_get_device_id - vendor 내 device ID 조회 (제품/모델 식별)
 *
 * @param dev: attached PCI 디바이스.
 * @return: 16-bit device ID. vendor 별로 의미가 다르며 같은 vendor 의 다른 모델 구분에 사용.
 *
 * 동작: dev->id.device_id 반환. PCI config space offset 0x02 의 2바이트.
 * 사용처: 특정 모델만 적용되는 firmware bug workaround, RPC 의 디바이스 카탈로그.
 *
 * 호출 체인: NVMe quirk / RPC → spdk_pci_device_get_device_id → dev->id.device_id
 */
uint16_t spdk_pci_device_get_device_id(struct spdk_pci_device *dev);

/**
 * Get the subvendor ID of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return subvendor ID.
 */
/*
 * [한국어]
 * spdk_pci_device_get_subvendor_id - 서브시스템 vendor ID 조회 (보드/OEM 식별)
 *
 * @param dev: attached PCI 디바이스.
 * @return: 16-bit subsystem vendor ID. 동일 칩셋이 여러 OEM(예: Dell, HP) 보드로 재포장될 때 구분.
 *
 * 동작: dev->id.subvendor_id 반환. PCI config space offset 0x2C 의 2바이트.
 *   대부분 드라이버 매칭은 vendor/device 만 보고 sub* 는 ANY 로 두므로 식별 정보용.
 * 사용처: OEM 별 quirk, 인벤토리/관리 정보.
 *
 * 호출 체인: 사용자/RPC → spdk_pci_device_get_subvendor_id → dev->id.subvendor_id
 */
uint16_t spdk_pci_device_get_subvendor_id(struct spdk_pci_device *dev);

/**
 * Get the subdevice ID of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return subdevice ID.
 */
/*
 * [한국어]
 * spdk_pci_device_get_subdevice_id - 서브시스템 device ID 조회
 *
 * @param dev: attached PCI 디바이스.
 * @return: 16-bit subsystem device ID. subvendor 별로 OEM 보드 모델 구분.
 *
 * 동작: dev->id.subdevice_id 반환. PCI config space offset 0x2E 의 2바이트.
 * 사용처: 서브벤더가 같은 OEM 의 보드 모델별 firmware/quirk 분기.
 *
 * 호출 체인: 사용자/RPC → spdk_pci_device_get_subdevice_id → dev->id.subdevice_id
 */
uint16_t spdk_pci_device_get_subdevice_id(struct spdk_pci_device *dev);

/**
 * Get the PCI ID of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return PCI ID.
 */
/*
 * [한국어]
 * spdk_pci_device_get_id - 5튜플 PCI ID 구조체 통째 반환 (값 복사)
 *
 * @param dev: attached PCI 디바이스.
 * @return: { class_id, vendor_id, device_id, subvendor_id, subdevice_id } 값 복사본.
 *
 * 동작: dev->id 를 값으로 반환. 호출자는 결과로 ID 테이블 매칭이나 quirk 분기 작성 가능.
 * 사용처: 한 함수 안에서 여러 ID 필드 동시 참조할 때 (각각 호출보다 1번 복사가 효율적).
 *
 * 호출 체인: NVMe driver/RPC → spdk_pci_device_get_id → dev->id (값 복사)
 */
struct spdk_pci_id spdk_pci_device_get_id(struct spdk_pci_device *dev);

/**
 * Get the NUMA node the PCI device is on.
 *
 * \param dev PCI device.
 *
 * \return NUMA node index (>= 0).
 */
/*
 * [한국어]
 * spdk_pci_device_get_numa_id - 디바이스가 물리적으로 연결된 NUMA 노드 조회
 *
 * @param dev: attached PCI 디바이스.
 * @return: NUMA 노드 ID (>= 0) 또는 SPDK_ENV_NUMA_ID_ANY(=-1) — 정보 없음/단일 노드.
 *
 * 동작: dev->numa_id 반환 (env_dpdk 가 sysfs /sys/bus/pci/devices/.../numa_node 에서 읽어둠).
 *   PCIe 디바이스는 특정 CPU socket 의 root complex 에 연결되어 있어 해당 socket 의 NUMA 노드와
 *   짝지어진다. Cross-NUMA DMA 는 QPI/UPI 링크를 거치므로 대역폭 손실 + 지연이 발생.
 * 사용처: NVMe qpair 를 처리할 reactor 를 디바이스와 같은 NUMA 의 lcore 에 배치 (NUMA-local I/O).
 *   spdk_dma_zmalloc_socket(... , numa_id, ...) 의 numa_id 로도 전달되어 버퍼도 NUMA-local 화.
 * 컨텍스트: init 단계 또는 RPC 응답. 핫패스에서는 attach 시 캐시해두고 매번 호출하지 않음이 일반적.
 *
 * 호출 체인: NVMe init → spdk_pci_device_get_numa_id → dev->numa_id → reactor cpumask 선택
 */
int spdk_pci_device_get_numa_id(struct spdk_pci_device *dev);

/**
 * Serialize the PCIe Device Serial Number into the provided buffer.
 * The buffer will contain a 16-character-long serial number followed by
 * a NULL terminator.
 *
 * \param dev PCI device.
 * \param sn Buffer to store the serial number in.
 * \param len Length of buffer. Must be at least 17.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_pci_device_get_serial_number - PCIe Device Serial Number 확장 capability 에서 16자 SN 추출
 *
 * 동작: PCI config space 의 PCIe extended capability 검색 → DSN cap → 8 byte 를 hex 16자로 직렬화.
 * 사용처: NVMe 다중 장치 환경에서 디바이스 식별자, 로깅.
 */
int spdk_pci_device_get_serial_number(struct spdk_pci_device *dev, char *sn, size_t len);

/**
 * Claim a PCI device for exclusive SPDK userspace access.
 *
 * Uses F_SETLK on a shared memory file with the PCI address embedded in its name.
 * As long as this file remains open with the lock acquired, other processes will
 * not be able to successfully call this function on the same PCI device.
 *
 * The device can be un-claimed by the owning process with spdk_pci_device_unclaim().
 * It will be also unclaimed automatically when detached.
 *
 * \param dev PCI device to claim.
 *
 * \return -EACCES if the device has already been claimed,
 *	   negative errno on unexpected errors,
 *	   0 on success.
 */
/*
 * [한국어]
 * spdk_pci_device_claim - 다른 프로세스가 동일 PCI 디바이스를 attach 하지 못하도록 lock
 *
 * 동작: /var/tmp 등에 BDF 가 이름에 들어간 빈 파일 생성 → F_SETLK 으로 advisory lock.
 * 같은 시스템에 SPDK 인스턴스가 둘 이상 떠있을 때 같은 NVMe 를 동시에 driver attach 하면
 * 데이터 손상 위험 — 이 lock 으로 first-come-first-served 보호. 자동 unclaim: detach 시 close→해제.
 *
 * @return: -EACCES = 이미 다른 프로세스 점유, 0 = 점유 획득.
 */
int spdk_pci_device_claim(struct spdk_pci_device *dev);

/**
 * Undo spdk_pci_device_claim().
 *
 * \param dev PCI device to unclaim.
 */
/*
 * [한국어]
 * spdk_pci_device_unclaim - spdk_pci_device_claim 으로 점유한 lock 해제
 *
 * @param dev: 점유 중인 PCI 디바이스.
 *
 * 동작: claim 시 잡아둔 lockfile fd 를 close → 커널이 F_SETLK 락을 자동 해제 → 다른 SPDK
 *   프로세스가 같은 BDF 에 대해 claim 가능. dev->internal.claim_fd 는 -1 로 리셋.
 * 자동 호출: spdk_pci_device_detach 가 attached 디바이스의 claim 을 자동 해제하므로 명시 호출은
 *   "detach 없이 claim 만 해제하고 싶을 때" 에 한정 (드문 케이스).
 * 컨텍스트: cleanup. 핫패스 호출 X.
 *
 * 호출 체인: NVMe cleanup / 사용자 → spdk_pci_device_unclaim → close(claim_fd)
 */
void spdk_pci_device_unclaim(struct spdk_pci_device *dev);

/**
 * Release all resources associated with the given device and detach it. As long
 * as the PCI device is physically available, it will attachable again.
 *
 * \param device PCI device.
 */
/*
 * [한국어]
 * spdk_pci_device_detach - 디바이스 분리: BAR unmap, claim 해제, 글로벌 리스트 제거
 *
 * 사용처: NVMe controller 종료 시 spdk_nvme_detach 가 내부적으로 호출.
 * detach 후 같은 BDF 를 다시 spdk_pci_device_attach 하면 재 attach 가능 (디바이스가 물리적으로
 * 살아있는 한). 핫리무브 후에는 BDF 재 enumerate 가 필요.
 *
 * 호출 체인: spdk_nvme_detach → spdk_pci_device_detach → unmap_bar/cfg_close/TAILQ_REMOVE
 */
void spdk_pci_device_detach(struct spdk_pci_device *device);

/**
 * Attach a PCI device. This will bypass all blocked list rules and explicitly
 * attach a device at the provided address. The return code of the provided
 * callback will decide whether that device is attached or not. Attached
 * devices have to be manually detached with spdk_pci_device_detach() to be
 * attach-able again.
 *
 * \param driver Driver for a specific device type. The device will only be
 * attached if it's supported by this driver.
 * \param enum_cb Callback to be called for the PCI device once it's found.
 * \param enum_ctx Additional context passed to the callback function.
 * \param pci_address Address of the device to attach.
 *
 * \return -1 if a device at the provided PCI address couldn't be found,
 *         -1 if an internal error happened or the provided callback returned non-zero,
 *         0 otherwise
 */
/*
 * [한국어]
 * spdk_pci_device_attach - 특정 BDF 디바이스 1개만 명시적 attach (블록리스트 우회)
 *
 * spdk_pci_enumerate 가 "전체 스캔"이라면 이 함수는 "이 디바이스 하나만". 사용자가 RPC 등으로
 * 특정 NVMe BDF 를 동적으로 attach 할 때 사용. enum_cb 가 0 반환해야 attach 가 보존된다.
 *
 * 호출 체인: RPC nvme_attach_controller → spdk_pci_device_attach → 매칭 후 enum_cb
 */
int spdk_pci_device_attach(struct spdk_pci_driver *driver, spdk_pci_enum_cb enum_cb,
			   void *enum_ctx, struct spdk_pci_addr *pci_address);

/**
 * Allow the specified PCI device to be probed by the calling process.
 *
 * When using spdk_pci_enumerate(), only devices with allowed PCI addresses will
 * be probed.  By default, this is all PCI addresses, but the pci_allowed
 * and pci_blocked environment options can override this behavior.
 * This API enables the caller to allow a new PCI address that may have previously
 * been blocked.
 *
 * \param pci_addr PCI address to allow
 * \return 0 if successful
 * \return -ENOMEM if environment-specific data structures cannot be allocated
 * \return -EINVAL if specified PCI address is not valid
 */
/*
 * [한국어]
 * spdk_pci_device_allow - PCI 허용리스트에 BDF 동적 추가
 *
 * env_opts 의 pci_allowed/blocked 는 init 시 한 번 결정되지만, 런타임에 RPC 로 새 BDF 를
 * 추가하고 싶을 때 사용. 이후 spdk_pci_enumerate 가 이 BDF 도 매칭한다.
 */
int spdk_pci_device_allow(struct spdk_pci_addr *pci_addr);

/**
 * Read \c len bytes from the PCI configuration space.
 *
 * \param dev PCI device.
 * \param buf A buffer to copy the data into.
 * \param len Number of bytes to read.
 * \param offset Offset (in bytes) in the PCI config space to start reading from.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_pci_device_cfg_read - PCI config space 가변 길이 읽기 (PCIe 4096B 영역)
 *
 * config space 영역 = type0/type1 헤더(0x00..0x3F, vendor/device/command/status/BAR…)
 *   + capability list (0x40..0xFF, MSI-X 등) + PCIe extended capability (0x100..0xFFF, AER, DSN 등).
 * 사용처: 드라이버가 capability discovery, BME(0x04 Command 의 bit 2) 설정, PM state 변경.
 *
 * 호출 체인: NVMe init → spdk_pci_device_cfg_read* → dev->cfg_read (env_dpdk: VFIO_DEVICE_CFG ioctl)
 */
int spdk_pci_device_cfg_read(struct spdk_pci_device *dev, void *buf, uint32_t len,
			     uint32_t offset);

/**
 * Write \c len bytes into the PCI configuration space.
 *
 * \param dev PCI device.
 * \param buf A buffer to copy the data from.
 * \param len Number of bytes to write.
 * \param offset Offset (in bytes) in the PCI config space to start writing to.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_pci_device_cfg_write - PCI config space 가변 길이 쓰기 (cfg_read 의 짝)
 *
 * @param dev: attached PCI 디바이스.
 * @param buf: 쓸 데이터의 소스 버퍼.
 * @param len: 바이트 수.
 * @param offset: config space 시작 오프셋.
 * @return: 0 성공, -1 실패.
 *
 * 동작: dev->cfg_write 함수 포인터를 호출 — env_dpdk 면 VFIO 의 pwrite(VFIO_DEVICE_CFG region) 경유.
 * 사용처:
 *   - BME(Bus Master Enable) 활성: cfg_read16(0x04) | 0x4 → cfg_write16 — DMA 전제 조건.
 *   - MSI-X capability 활성, PM state 변경, PCIe link retrain 등.
 *   - NVMe controller reset 후 reconfiguration.
 * 컨텍스트: init/reset 단계. 핫패스 호출 X.
 *
 * 호출 체인: NVMe init/reset → spdk_pci_device_cfg_write → dev->cfg_write (VFIO ioctl)
 */
int spdk_pci_device_cfg_write(struct spdk_pci_device *dev, void *buf, uint32_t len,
			      uint32_t offset);

/**
 * Read 1 byte from the PCI configuration space.
 *
 * \param dev PCI device.
 * \param value A buffer to copy the data into.
 * \param offset Offset (in bytes) in the PCI config space to start reading from.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_pci_device_cfg_read8 - PCI config space 1바이트 읽기 편의 래퍼
 *
 * @param dev: PCI 디바이스.
 * @param value: 1바이트 출력 버퍼.
 * @param offset: config space 오프셋.
 * @return: 0 성공, -1 실패.
 *
 * 동작: 내부적으로 cfg_read(dev, value, 1, offset). 1바이트 access 가 PCI 스펙상 항상 안전한
 *   필드(예: Revision ID, Interrupt Line)에 사용. 정렬을 신경 쓰지 않아도 됨.
 * 사용처: capability ID byte, programming interface byte 등 1바이트 필드 조회.
 *
 * 호출 체인: NVMe init → spdk_pci_device_cfg_read8 → cfg_read (1B)
 */
int spdk_pci_device_cfg_read8(struct spdk_pci_device *dev, uint8_t *value, uint32_t offset);

/**
 * Write 1 byte into the PCI configuration space.
 *
 * \param dev PCI device.
 * \param value A value to write.
 * \param offset Offset (in bytes) in the PCI config space to start writing to.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_pci_device_cfg_write8 - PCI config space 1바이트 쓰기
 *
 * @param dev: PCI 디바이스.
 * @param value: 쓸 1바이트 값.
 * @param offset: 시작 오프셋.
 * @return: 0 성공, -1 실패.
 *
 * 동작: cfg_write(dev, &value, 1, offset). 보통 capability enable/disable 비트 토글에 사용.
 * 사용처: Cache Line Size (offset 0x0C), Latency Timer 등 1바이트 단위 설정.
 *
 * 호출 체인: NVMe init → spdk_pci_device_cfg_write8 → cfg_write (1B)
 */
int spdk_pci_device_cfg_write8(struct spdk_pci_device *dev, uint8_t value, uint32_t offset);

/**
 * Read 2 bytes from the PCI configuration space.
 *
 * \param dev PCI device.
 * \param value A buffer to copy the data into.
 * \param offset Offset (in bytes) in the PCI config space to start reading from.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_pci_device_cfg_read16 - PCI config space 2바이트 읽기 편의 래퍼
 *
 * @param dev: PCI 디바이스.
 * @param value: 2바이트 출력 버퍼.
 * @param offset: 시작 오프셋 (2바이트 정렬 권장).
 * @return: 0 성공, -1 실패.
 *
 * 동작: cfg_read(dev, value, 2, offset).
 * 사용처: Command(0x04)/Status(0x06), Vendor/Device/Subsystem ID 필드 등 2바이트 단위 표준 필드.
 *   PCI 표준 헤더의 대부분 16-bit 필드는 이 함수로 읽는다.
 *
 * 호출 체인: NVMe init → spdk_pci_device_cfg_read16 → cfg_read (2B)
 */
int spdk_pci_device_cfg_read16(struct spdk_pci_device *dev, uint16_t *value, uint32_t offset);

/**
 * Write 2 bytes into the PCI configuration space.
 *
 * \param dev PCI device.
 * \param value A value to write.
 * \param offset Offset (in bytes) in the PCI config space to start writing to.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_pci_device_cfg_write16 - PCI config space 2바이트 쓰기
 *
 * @param dev: PCI 디바이스.
 * @param value: 쓸 2바이트 값.
 * @param offset: 시작 오프셋.
 * @return: 0 성공, -1 실패.
 *
 * 동작: cfg_write(dev, &value, 2, offset).
 * 핵심 사용 예 — BME(Bus Master Enable) 활성:
 *   uint16_t cmd; cfg_read16(dev, &cmd, 0x04); cmd |= 0x4; cfg_write16(dev, cmd, 0x04);
 *   BME 비트가 1 이어야 디바이스가 호스트 메모리에 DMA 가능. NVMe 동작의 전제.
 * 사용처: Command 레지스터, MSI-X capability message control 등 2바이트 단위 control.
 *
 * 호출 체인: NVMe init → spdk_pci_device_cfg_write16 → cfg_write (2B)
 */
int spdk_pci_device_cfg_write16(struct spdk_pci_device *dev, uint16_t value, uint32_t offset);

/**
 * Read 4 bytes from the PCI configuration space.
 *
 * \param dev PCI device.
 * \param value A buffer to copy the data into.
 * \param offset Offset (in bytes) in the PCI config space to start reading from.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_pci_device_cfg_read32 - PCI config space 4바이트 읽기 편의 래퍼
 *
 * @param dev: PCI 디바이스.
 * @param value: 4바이트 출력 버퍼.
 * @param offset: 시작 오프셋 (4바이트 정렬 권장).
 * @return: 0 성공, -1 실패.
 *
 * 동작: cfg_read(dev, value, 4, offset). 4바이트 access 는 PCI/PCIe 표준에서 항상 안전한 atomic access.
 * 사용처: Vendor:Device(0x00) 4B 동시 읽기, BAR 레지스터(0x10..0x24) 읽기, PCIe extended capability
 *   header (offset 0x100~) 4바이트 단위 파싱 (next cap pointer + cap ID).
 *
 * 호출 체인: NVMe init / capability discovery → spdk_pci_device_cfg_read32 → cfg_read (4B)
 */
int spdk_pci_device_cfg_read32(struct spdk_pci_device *dev, uint32_t *value, uint32_t offset);

/**
 * Write 4 bytes into the PCI configuration space.
 *
 * \param dev PCI device.
 * \param value A value to write.
 * \param offset Offset (in bytes) in the PCI config space to start writing to.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_pci_device_cfg_write32 - PCI config space 4바이트 쓰기
 *
 * @param dev: PCI 디바이스.
 * @param value: 쓸 4바이트 값.
 * @param offset: 시작 오프셋.
 * @return: 0 성공, -1 실패.
 *
 * 동작: cfg_write(dev, &value, 4, offset). 32-bit access 는 atomic 한 PCI 트랜잭션.
 * 사용처: BAR base address 재프로그래밍 (보통 OS 가 함, SPDK 는 거의 안 함), AER status 클리어
 *   (rw1c 비트에 1 을 써서 클리어), VFIO 디바이스 reset.
 *
 * 호출 체인: NVMe init/error handling → spdk_pci_device_cfg_write32 → cfg_write (4B)
 */
int spdk_pci_device_cfg_write32(struct spdk_pci_device *dev, uint32_t value, uint32_t offset);

/**
 * Check if device was requested to be removed from the process. This can be
 * caused either by physical device hotremoval or OS-triggered removal. In the
 * latter case, the device may continue to function properly even if this
 * function returns \c true . The upper-layer driver may check this function
 * periodically and eventually detach the device.
 *
 * \param dev PCI device.
 *
 * \return if device was requested to be removed
 */
/*
 * [한국어]
 * spdk_pci_device_is_removed - 핫리무브/OS-triggered removal 요청 여부
 *
 * true 반환 시 NVMe 드라이버는 새 IO 발행 중단 + 기존 IO 안전 완료 처리 + spdk_nvme_detach 시작 필요.
 * 단순히 OS-triggered 인 경우엔 디바이스가 아직 동작 중일 수도 있어 즉시 detach 가 필수는 아님.
 * 사용처: NVMe poller 가 매 polling 주기에 이 값을 확인.
 */
bool spdk_pci_device_is_removed(struct spdk_pci_device *dev);

/**
 * Compare two PCI addresses.
 *
 * \param a1 PCI address 1.
 * \param a2 PCI address 2.
 *
 * \return 0 if a1 == a2, less than 0 if a1 < a2, greater than 0 if a1 > a2
 */
/*
 * [한국어]
 * spdk_pci_addr_compare - 두 PCI BDF 주소의 사전식(lexicographic) 비교
 *
 * @param a1: 비교 대상 1.
 * @param a2: 비교 대상 2.
 * @return: 0 = 같음, <0 = a1 < a2, >0 = a1 > a2 (domain → bus → dev → func 순 비교).
 *
 * 사용처:
 *   - allowed/blocked PCI 리스트 검색 (env_opts.pci_allowed[i] 와 enumerate 대상 BDF 비교).
 *   - BDF 정렬 (RPC 응답에 정렬된 디바이스 리스트 반환).
 *   - 디바이스 식별 (NVMe Probe 콜백이 특정 BDF 만 attach 하고 싶을 때).
 * 컨텍스트: 어디서나. 순수 함수 (인자만 봄, 부수효과 없음).
 *
 * 호출 체인: env_dpdk 의 enum filter / 사용자 코드 → spdk_pci_addr_compare → 4필드 정수 비교
 */
int spdk_pci_addr_compare(const struct spdk_pci_addr *a1, const struct spdk_pci_addr *a2);

/**
 * Convert a string representation of a PCI address into a struct spdk_pci_addr.
 *
 * \param addr PCI address output on success.
 * \param bdf PCI address in domain:bus:device.function format or
 *	domain.bus.device.function format.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_pci_addr_parse - "0000:5e:00.0" 또는 "0000.5e.00.0" 형식 → struct spdk_pci_addr
 *
 * 사용처: RPC, CLI 인자, config 파일에서 BDF 입력 받기.
 */
int spdk_pci_addr_parse(struct spdk_pci_addr *addr, const char *bdf);

/**
 * Convert a struct spdk_pci_addr to a string.
 *
 * \param bdf String into which a string will be output in the format
 *  domain:bus:device.function. The string must be at least 14 characters in size.
 * \param sz Size of bdf in bytes. Must be at least 14.
 * \param addr PCI address.
 *
 * \return 0 on success, or a negated errno on failure.
 */
/*
 * [한국어]
 * spdk_pci_addr_fmt - struct → "DDDD:BB:DD.F" 문자열 (최소 14자 버퍼).
 *
 * 사용처: 로그 메시지, RPC 응답 직렬화, lockfile 이름 생성.
 */
int spdk_pci_addr_fmt(char *bdf, size_t sz, const struct spdk_pci_addr *addr);

/**
 * Hook a custom PCI device into the PCI layer. The device will be attachable,
 * enumerable, and will call provided callbacks on each PCI resource access
 * request.
 *
 * \param drv driver that will be able to attach the device
 * \param dev fully initialized PCI device struct
 *
 * \return 0 on success, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_pci_hook_device - "가상" PCI 디바이스를 SPDK PCI 레이어에 끼워넣기
 *
 * 실제 PCI 버스에 없는 디바이스 (단위 테스트 mock NVMe, emulated controller, vfio-user) 를
 * 마치 PCI 디바이스인 것처럼 enumerate/attach 가능하게 한다. dev 의 map_bar/cfg_read/write
 * 함수 포인터를 호출자가 직접 채워서 전달.
 *
 * 사용처: lib/nvme 의 vfio-user transport, 테스트 fixture.
 */
int spdk_pci_hook_device(struct spdk_pci_driver *drv, struct spdk_pci_device *dev);

/**
 * Un-hook a custom PCI device from the PCI layer. The device must not be attached.
 *
 * \param dev fully initialized PCI device struct
 */
/*
 * [한국어]
 * spdk_pci_unhook_device - hook_device 의 짝, 커스텀 PCI 디바이스 등록 해제
 *
 * @param dev: spdk_pci_hook_device 로 등록된 디바이스 핸들.
 *
 * 사전 조건: dev 가 attached 상태가 아니어야 함 (attached 라면 먼저 detach 호출).
 * 동작: 글로벌 PCI 디바이스 TAILQ 에서 dev 제거. dev 메모리 자체는 호출자가 해제해야 함.
 * 사용처: vfio-user transport 종료, 단위 테스트 mock 디바이스 cleanup.
 *
 * 호출 체인: 테스트/transport cleanup → spdk_pci_unhook_device → TAILQ_REMOVE
 */
void spdk_pci_unhook_device(struct spdk_pci_device *dev);

/**
 * Return the type of the PCI device.
 *
 * \param dev PCI device
 *
 * \return string representing the type of the device
 */
/*
 * [한국어]
 * spdk_pci_device_get_type - PCI 디바이스의 provider 타입 문자열 조회
 *
 * @param dev: PCI 디바이스 핸들.
 * @return: dev->type 포인터 (예: "pci" = env_dpdk 기본, "vfio-user" = vfio-user transport).
 *          NULL 반환 없음 — 항상 valid 문자열.
 *
 * 동작: dev->type 단순 반환. detach 시 어느 provider 의 detach_cb 를 호출할지 결정하는 키.
 * 사용처: provider 별 로직 분기 (예: "vfio-user" 디바이스에는 BAR 매핑을 다르게 처리),
 *   RPC 응답의 디바이스 타입 표시.
 *
 * 호출 체인: PCI provider lookup / RPC → spdk_pci_device_get_type → dev->type
 */
const char *spdk_pci_device_get_type(const struct spdk_pci_device *dev);

/* [한국어] PCI device provider 추상화 — 같은 SPDK 안에서 여러 PCI 백엔드(env_dpdk, vfio-user 등)를
 * 공존시키기 위한 메커니즘. 각 provider 는 attach/detach 콜백과 unique name 으로 식별된다.
 * dev->type == provider->name 매칭으로 detach 시 올바른 provider 가 선택된다. */
struct spdk_pci_device_provider {
	const char *name;
	/* [한국어] provider 이름 (dev->type 과 매칭). 예: "pci"(DPDK), "vfio-user". */

	/**
	 * Callback executed to attach a PCI device on a given address.
	 *
	 * \param addr address of the device.
	 *
	 * \return 0 if the device was attached successfully, negative errno otherwise.
	 */
	int (*attach_cb)(const struct spdk_pci_addr *addr);
	/* [한국어] BDF 가 주어졌을 때 해당 디바이스를 이 provider 가 가진 메커니즘으로 attach.
	 * 성공 시 0 반환 + 디바이스를 글로벌 리스트에 등록 (spdk_pci_hook_device 등 사용). */

	/**
	 * Callback executed to detach a given PCI device.  The provider to detach the device is
	 * selected based on the type of the device and the name of the provider (i.e. dev->type ==
	 * provider->name).
	 *
	 * \param dev PCI device to detach.
	 */
	void (*detach_cb)(struct spdk_pci_device *dev);
	/* [한국어] detach 시 호출. provider 별 자원(emulated 디바이스의 backing fd 등) 정리. */

	TAILQ_ENTRY(spdk_pci_device_provider) tailq;
	/* [한국어] 글로벌 provider 리스트의 노드. 등록 순으로 attach 시도. */
};

/**
 * Register a PCI device provdier.
 *
 * \param provider PCI device provider.
 */
/*
 * [한국어]
 * spdk_pci_register_device_provider - PCI device provider 를 글로벌 리스트에 추가
 *
 * @param provider: name + attach_cb + detach_cb 가 채워진 spdk_pci_device_provider 포인터.
 *                  메모리는 호출자가 lifetime 동안 유지해야 함 (포인터만 등록).
 *
 * 동작: provider 의 TAILQ_ENTRY 를 글로벌 provider 리스트에 추가. 등록 후에는 spdk_pci_device_attach
 *   가 매칭되는 BDF 에 대해 이 provider 의 attach_cb 를 호출할 수 있게 된다.
 * 자동 등록: 보통 SPDK_PCI_REGISTER_DEVICE_PROVIDER 매크로가 __attribute__((constructor)) 로
 *   main 진입 전에 자동 등록 — 사용자가 명시 호출할 일은 거의 없다.
 * 컨텍스트: init 단계 (constructor 또는 명시 init). 핫패스 호출 X.
 *
 * 호출 체인: __attribute__((constructor)) → spdk_pci_register_device_provider → 글로벌 TAILQ
 */
void spdk_pci_register_device_provider(struct spdk_pci_device_provider *provider);

/* [한국어] PCI device provider 자동 등록 매크로 — main 진입 전 constructor 로 실행되어
 * 사용자 코드의 명시적 init 호출 불필요. ## 토큰 결합으로 unique 함수명 생성. */
#define SPDK_PCI_REGISTER_DEVICE_PROVIDER(name, provider) \
	static void __attribute__((constructor)) _spdk_pci_register_device_provider_##name(void) \
	{ \
		spdk_pci_register_device_provider(provider); \
	}

/**
 * Remove any CPU affinity from the current thread.
 */
/*
 * [한국어]
 * spdk_unaffinitize_thread - 현재 pthread 의 CPU affinity 해제 (모든 코어 허용)
 *
 * lcore 에 핀된 reactor 스레드가 자식 스레드를 만들 때, 자식이 같은 핀을 상속받지 않도록
 * 호출자 스레드의 affinity 를 임시 해제. 사용처: 보조 작업 스레드 생성 시.
 */
void spdk_unaffinitize_thread(void);

/**
 * Call a function with CPU affinity unset.
 *
 * This can be used to run a function that creates other threads without inheriting the calling
 * thread's CPU affinity.
 *
 * \param cb Function to call
 * \param arg Parameter to the function cb().
 *
 * \return the return value of cb().
 */
/*
 * [한국어]
 * spdk_call_unaffinitized - cb 를 affinity 없이 실행 후 원복
 *
 * 동작: 현재 affinity 저장 → 모든 코어 허용 → cb(arg) 실행 → affinity 복원 → cb 반환값 리턴.
 * 사용처: SPDK 내부에서 unpinned 작업 스레드 생성 (DPDK init thread 등) 시.
 */
void *spdk_call_unaffinitized(void *cb(void *arg), void *arg);

/**
 * Page-granularity memory address translation table.
 */
/* [한국어] VA→IOVA(또는 임의 64-bit 값) 변환 테이블의 불투명 핸들. spdk_vtophys 의 백엔드.
 * 페이지 단위(2MB) radix tree 로 빠른 lookup 제공. 외부 DMA 매핑(RDMA MR ID 등)을 keyed by VA
 * 보관할 때도 재사용 가능 — translation 값이 임의 64bit 이므로 일반화된 mapping. */
struct spdk_mem_map;

/* [한국어] mem_map 의 notify 콜백이 받는 액션 — 어떤 메모리 영역이 등록/해제되었는지 알림. */
enum spdk_mem_map_notify_action {
	SPDK_MEM_MAP_NOTIFY_REGISTER,
	/* [한국어] 새 메모리 영역이 등록됨 — 콜백은 외부 DMA(예: RDMA MR) 매핑을 함께 등록. */
	SPDK_MEM_MAP_NOTIFY_UNREGISTER,
	/* [한국어] 영역 해제 — 콜백은 외부 매핑도 해제 필요. */
};

/* [한국어] mem_map 의 영역 변경 통지 콜백.
 * 모든 등록된 mem_map 에 대해 spdk_mem_register/unregister 시 호출된다.
 * @param cb_ctx: spdk_mem_map_alloc 의 cb_ctx.
 * @param map: 호출 대상 map.
 * @param action: REGISTER/UNREGISTER.
 * @param vaddr/size: 영향 받은 영역.
 * @return: 음수 errno 면 register 실패. */
typedef int (*spdk_mem_map_notify_cb)(void *cb_ctx, struct spdk_mem_map *map,
				      enum spdk_mem_map_notify_action action,
				      void *vaddr, size_t size);

/* [한국어] 두 translation 값이 "연속(contiguous)" 한지 판정하는 사용자 정의 함수.
 * 사용처: spdk_vtophys 가 size out 을 채울 때 인접 페이지가 한 IOVA 블록인지 확인. */
typedef int (*spdk_mem_map_contiguous_translations)(uint64_t addr_1, uint64_t addr_2);

/**
 * A function table to be implemented by each memory map.
 */
/* [한국어] mem_map 사용자가 제공해야 할 콜백 테이블. */
struct spdk_mem_map_ops {
	spdk_mem_map_notify_cb notify_cb;
	/* [한국어] register/unregister 통지. NULL 가능 (단순 lookup-only mem_map). */
	spdk_mem_map_contiguous_translations are_contiguous;
	/* [한국어] 인접 페이지 translation 의 연속성 판정. NULL 이면 항상 비연속 가정 (size=4KiB 반환). */
};

/**
 * Allocate a virtual memory address translation map.
 *
 * \param default_translation Default translation for the map.
 * \param ops Table of callback functions for map operations.
 * \param cb_ctx Argument passed to the callback function.
 *
 * \return a pointer to the allocated virtual memory address translation map.
 */
/*
 * [한국어]
 * spdk_mem_map_alloc - 새 VA-keyed translation 테이블 생성
 *
 * @param default_translation: lookup 실패 시 반환할 기본값 (예: SPDK_VTOPHYS_ERROR).
 * @param ops: notify/are_contiguous 콜백 (NULL 허용 — passive map).
 * @param cb_ctx: notify_cb 에 전달.
 * @return: 새 map 핸들 또는 NULL.
 *
 * 새로 만든 map 은 자동으로 글로벌 mem 등록 이벤트 (이미 등록된 영역) 를 colaesce 적용받아
 * notify_cb 가 SPDK_MEM_MAP_NOTIFY_REGISTER 로 한 번씩 호출됨 — 즉시 일관 상태에서 시작.
 *
 * 사용처: NVMe 드라이버 내부의 vtophys, RDMA NIC 의 MR 캐시.
 */
struct spdk_mem_map *spdk_mem_map_alloc(uint64_t default_translation,
					const struct spdk_mem_map_ops *ops, void *cb_ctx);

/**
 * Free a memory map previously allocated by spdk_mem_map_alloc().
 *
 * \param pmap Memory map to free.
 */
/*
 * [한국어]
 * spdk_mem_map_free - spdk_mem_map_alloc 으로 생성한 mem_map 해제
 *
 * @param pmap: spdk_mem_map* 변수의 주소 (더블 포인터). 함수 종료 시 *pmap = NULL 로 자동 초기화되어
 *              double-free 방지.
 *
 * 동작: map 의 radix tree 해제 + 글로벌 mem_map 리스트에서 제거. 등록 콜백은 호출되지 않음
 *   (REGISTER 의 대칭으로 UNREGISTER 가 자동 발생하지는 않음 — map 자체가 사라지므로).
 * 컨텍스트: cleanup 단계. 핫패스 호출 X.
 *
 * 호출 체인: 사용자/RDMA cleanup → spdk_mem_map_free → 내부 radix tree free + *pmap=NULL
 */
void spdk_mem_map_free(struct spdk_mem_map **pmap);

/**
 * Register an address translation for a range of virtual memory.
 *
 * \param map Memory map.
 * \param vaddr Virtual address of the region to register - must be 2 MB aligned.
 * \param size Size of the region in bytes - must be multiple of 2 MB in the
 *  current implementation.
 * \param translation Translation to store in the map for this address range.
 *
 * \sa spdk_mem_map_clear_translation().
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_mem_map_set_translation - VA 영역에 translation 값 저장 (2MB 정렬 강제)
 *
 * 정렬 강제 이유: hugepage size = 2MB 가 SPDK 내부 page 단위. radix tree 의 leaf 가 2MB 페이지.
 * 사용처: 일반 사용자 호출은 드물고, env_dpdk 가 hugepage 매핑 시점에 자동 호출.
 */
int spdk_mem_map_set_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size,
				 uint64_t translation);

/**
 * Unregister an address translation.
 *
 * \param map Memory map.
 * \param vaddr Virtual address of the region to unregister - must be 2 MB aligned.
 * \param size Size of the region in bytes - must be multiple of 2 MB in the
 *  current implementation.
 *
 * \sa spdk_mem_map_set_translation().
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_mem_map_clear_translation - 영역의 translation 값을 default 로 되돌림 (set 의 짝)
 *
 * @param map: 대상 mem_map.
 * @param vaddr: 클리어 시작 주소 (2MB 정렬).
 * @param size: 길이 (2MB 배수).
 * @return: 0 성공, 음수 errno 실패.
 *
 * 동작: radix tree 의 해당 2MB 페이지 노드들을 default_translation 값으로 덮어쓰기. tree 노드 자체는
 *   해제하지 않을 수 있어 메모리 절약보다는 lookup 결과 의미 변경이 목적.
 * 사용처: spdk_mem_unregister 가 내부적으로 호출, 또는 외부 DMA 매핑 (RDMA MR) 캐시 무효화.
 *
 * 호출 체인: spdk_mem_unregister / 사용자 → spdk_mem_map_clear_translation → radix tree update
 */
int spdk_mem_map_clear_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size);

/**
 * Look up the translation of a virtual address in a memory map.
 *
 * \param map Memory map.
 * \param vaddr Virtual address.
 * \param size Contains the size of the memory region pointed to by vaddr.
 * If vaddr is successfully translated, then this is updated with the size of
 * the memory region for which the translation is valid.
 *
 * \return the translation of vaddr stored in the map, or default_translation
 * as specified in spdk_mem_map_alloc() if vaddr is not present in the map.
 */
/*
 * [한국어]
 * spdk_mem_map_translate - VA → translation lookup (spdk_vtophys 의 일반화 버전)
 *
 * @param size: in/out — 입력은 호출자 관심 길이, 출력은 같은 translation 이 유효한 연속 길이.
 * 핫패스에서 매 IO 마다 호출되므로 O(1) radix tree lookup.
 *
 * 호출 체인: spdk_vtophys → spdk_mem_map_translate → 내부 radix tree
 */
uint64_t spdk_mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr, uint64_t *size);

/**
 * Register the specified memory region for address translation.
 *
 * The memory region must map to pinned huge pages (2MB or greater).
 *
 * \param vaddr Virtual address to register.
 * \param len Length in bytes of the vaddr.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_mem_register - 외부에서 잡은 hugepage 메모리를 SPDK 의 vtophys 매핑에 등록
 *
 * 사용처: 사용자가 직접 mmap(MAP_HUGETLB) 으로 잡은 영역, 또는 다른 라이브러리가 제공한
 * pinned 메모리를 NVMe/RDMA DMA 대상으로 만들고 싶을 때. 등록 후 spdk_vtophys 로 변환 가능.
 * 모든 등록된 spdk_mem_map 에 notify_cb(REGISTER) 가 호출되어 외부 DMA(MR) 도 자동 매핑.
 *
 * 제약: 2MB 정렬 + 2MB 배수 길이, hugepage 위에 핀된 메모리.
 */
int spdk_mem_register(void *vaddr, size_t len);

/**
 * Unregister the specified memory region from vtophys address translation.
 *
 * The caller must ensure all in-flight DMA operations to this memory region
 * are completed or cancelled before calling this function.
 *
 * \param vaddr Virtual address to unregister.
 * \param len Length in bytes of the vaddr.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_mem_unregister - register 의 짝. 모든 mem_map 에 notify_cb(UNREGISTER) 호출.
 * 주의: in-flight DMA 가 끝났음을 호출자가 보장해야 함 — 안 그러면 NVMe 가 잘못된 IOVA 접근.
 */
int spdk_mem_unregister(void *vaddr, size_t len);

/**
 * Reserve the address space specified in all memory maps.
 *
 * This pre-allocates the necessary space in the memory maps such that
 * future calls to spdk_mem_register() on that region require no
 * internal memory allocations.
 *
 * \param vaddr Virtual address to reserve
 * \param len Length in bytes of vaddr
 *
 * \return 0 on success, negated errno on failure.
 */
/*
 * [한국어]
 * spdk_mem_reserve - mem_map 내부 자료구조의 사전 할당 (radix tree 노드 미리 마련)
 *
 * 사용처: 핫패스에서 첫 spdk_mem_register 가 malloc 하는 비용을 init 시점으로 옮기고 싶을 때.
 * RT-friendly 패턴.
 */
int spdk_mem_reserve(void *vaddr, size_t len);

/**
 * Get the NUMA node ID for the specified memory buffer.
 *
 * Note: this only works for memory allocated via the environment layer.
 *
 * \param buf A pointer to a buffer.
 * \param size Contains the size of the memory region pointed to by vaddr.
 * If vaddr is successfully translated, then this is updated with the size of
 * the memory region for which the translation is valid.
 *
 * \return NUMA ID of the buffer if known, SPDK_ENV_NUMA_ID_ANY otherwise
 */
/*
 * [한국어]
 * spdk_mem_get_numa_id - hugepage 버퍼의 NUMA 노드 조회
 *
 * 사용처: NVMe req 가 제출될 코어가 버퍼 NUMA 와 다른지 검사 (cross-NUMA 경고/통계).
 * 일반 malloc 메모리는 SPDK_ENV_NUMA_ID_ANY 반환.
 */
int32_t spdk_mem_get_numa_id(const void *buf, uint64_t *size);

/**
 * Get the address's file descriptor and offset, it works with spdk memory allocation APIs
 *
 * \param vaddr Virtual address to get
 * \param offset Virtual address's map offset to the file descriptor
 *
 * \return negative errno on failure, otherwise return the file descriptor
 */
/*
 * [한국어]
 * spdk_mem_get_fd_and_offset - 가상주소 → 그것을 backing 하는 hugepage 파일 fd + 오프셋
 *
 * 사용처: vhost-user 가 게스트에게 메모리 region 을 전달할 때 fd 기반 SCM_RIGHTS 패스.
 * @return: fd (성공) 또는 음수 errno (실패).
 */
int spdk_mem_get_fd_and_offset(void *vaddr, uint64_t *offset);

/* [한국어] PCI 핫플러그 이벤트 타입 — Linux uevent 의 ADD/REMOVE 에 매핑. */
enum spdk_pci_event_type {
	SPDK_UEVENT_ADD = 0,
	/* [한국어] 새 PCI 디바이스 hot-insert. spdk_pci_enumerate 재호출로 attach 가능. */
	SPDK_UEVENT_REMOVE = 1,
	/* [한국어] hot-remove. attached 디바이스라면 즉시 detach 처리 시작 권장. */
};

/* [한국어] uevent 한 건의 페이로드: 동작 타입 + 대상 BDF. */
struct spdk_pci_event {
	enum spdk_pci_event_type action;
	/* [한국어] ADD 또는 REMOVE. */
	struct spdk_pci_addr traddr;
	/* [한국어] 이벤트가 발생한 디바이스 BDF (transport address). */
};

/* [한국어] PCI 버스 SIGBUS 등의 에러 시그널 핸들러 시그니처.
 * @param failure_addr: 폴트가 발생한 가상주소 (fault SIGBUS 의 si_addr).
 * @param ctx: 등록 시 전달한 컨텍스트.
 * 사용처: 핫리무브 직후 옛 BAR 매핑 접근으로 SIGBUS 가 발생할 때 longjmp/recovery. */
typedef void (*spdk_pci_error_handler)(const void *failure_addr, void *ctx);

/**
 * Begin listening for PCI bus events. This is used to detect hot-insert and
 * hot-remove events. Once the system is listening, events may be retrieved
 * by calling spdk_pci_get_event() periodically.
 *
 * \return negative errno on failure, otherwise,  return a file descriptor
 * that may be later passed to spdk_pci_get_event().
 */
/*
 * [한국어]
 * spdk_pci_event_listen - PCI 핫플러그 이벤트 수신 시작
 *
 * 동작: Linux 의 NETLINK_KOBJECT_UEVENT 소켓 연결 → fd 반환.
 * 반환된 fd 를 reactor 의 epoll 이나 poller 가 주기적으로 read 하여 이벤트 수령.
 *
 * @return: fd 또는 음수 errno.
 */
int spdk_pci_event_listen(void);

/**
 * Get the next PCI bus event.
 *
 * \param fd A file descriptor returned by spdk_pci_event_listen()
 * \param event An event on the PCI bus
 *
 * \return Negative errno on failure. 0 for no event. A positive number
 * when an event has been returned
 */
/*
 * [한국어]
 * spdk_pci_get_event - listen fd 에서 다음 이벤트 1건 비동기 read
 *
 * @return: >0 = 이벤트 1건 채워짐, 0 = 아직 없음, <0 = 오류.
 * non-blocking — poller 패턴에서 호출.
 */
int spdk_pci_get_event(int fd, struct spdk_pci_event *event);

/**
 * Register a signal handler to handle bus errors on the PCI bus
 *
 * \param sighandler Signal bus handler of the PCI bus
 * \param ctx The arg pass to the registered signal bus handler.
 *
 * \return negative errno on failure, otherwise it means successful
 */
/*
 * [한국어]
 * spdk_pci_register_error_handler - SIGBUS 시 호출될 사용자 핸들러 등록
 *
 * 핫리무브 후 옛 BAR vaddr 접근 시 SIGBUS 발생 → SPDK 의 시그널 디스패처가 등록된 모든
 * 핸들러를 호출. 핸들러는 보통 setjmp/longjmp 로 안전 위치로 점프.
 */
int spdk_pci_register_error_handler(spdk_pci_error_handler sighandler, void *ctx);

/**
 * Register a signal handler to handle bus errors on the PCI bus
 *
 * \param sighandler Signal bus handler of the PCI bus
 */
/*
 * [한국어]
 * spdk_pci_unregister_error_handler - PCI bus 에러 시그널 핸들러 제거 (register 의 짝)
 *
 * @param sighandler: register 시 사용한 함수 포인터. 동일 포인터로 매칭하여 제거.
 *
 * 동작: SPDK 의 SIGBUS 핸들러 체인에서 sighandler 와 일치하는 엔트리를 unlink. ctx 도 함께 제거.
 *   동일 sighandler 가 여러 ctx 로 등록되어 있으면 첫 매칭만 제거되는 점에 주의.
 * 사용처: NVMe controller detach 시퀀스 — 옛 BAR vaddr 접근으로 SIGBUS 발생 가능 구간이 끝나면 해제.
 *
 * 호출 체인: NVMe controller cleanup → spdk_pci_unregister_error_handler → 내부 핸들러 체인 제거
 */
void spdk_pci_unregister_error_handler(spdk_pci_error_handler sighandler);

/**
 * Get the tid of the current thread
 */
/*
 * [한국어]
 * spdk_get_tid - 현재 OS 스레드의 TID (Linux gettid()) 반환
 *
 * 사용처: 로깅, 디버깅, 스레드 식별. spdk_thread (논리 스레드) 와는 별개로 OS 레벨 식별자.
 */
int spdk_get_tid(void);

#ifdef __cplusplus
}
/* [한국어] extern "C" { 의 짝. C++ 컨슈머에서 SPDK 심볼이 C 링키지로 인식되도록 닫음. */
#endif

#endif
/* [한국어] #ifndef SPDK_ENV_H 가드 종료. 헤더 다중 포함 보호. */
