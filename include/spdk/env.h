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
 * spdk_dma_malloc - DMA 안전 버퍼 할당 (NUMA 자동) - spdk_malloc(size, align, unused, ANY, DMA) 의 별칭
 *
 * @param size/align/unused: spdk_malloc 과 동일 의미.
 * @return: hugepage 가상주소 또는 NULL.
 *
 * SPDK 초창기 API 로, 신코드는 spdk_malloc + flags 권장. 동작 의미는 동일하다.
 *
 * 호출 체인: 사용자 → spdk_dma_malloc → spdk_malloc(... , SPDK_MALLOC_DMA)
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
 * spdk_dma_malloc_socket - DMA 안전 버퍼 + NUMA 노드 명시
 *
 * @param numa_id: 할당할 NUMA 노드. NVMe 디바이스가 붙은 노드와 일치시키는 것이 성능 최적.
 *
 * 호출 체인: 사용자/NVMe init → spdk_dma_malloc_socket → spdk_malloc(... , numa_id, DMA)
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
 * spdk_dma_zmalloc - DMA 안전 + 0초기화 버퍼 (NUMA ANY)
 * @return: 0 으로 채워진 hugepage 메모리.
 * 호출 체인: 사용자/드라이버 init → spdk_dma_zmalloc → spdk_zmalloc(...,ANY, DMA)
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
 * spdk_dma_zmalloc_socket - spdk_dma_zmalloc + NUMA 노드 명시
 * 호출 체인: 사용자 → spdk_dma_zmalloc_socket → spdk_zmalloc(... , numa_id, DMA)
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
 * spdk_dma_realloc - DMA 버퍼 크기 변경 (spdk_realloc 의 DMA 버전)
 * 주의: in-flight DMA 가 없는 상태여야 함 — IOVA 가 바뀌면 NVMe 가 잘못된 주소를 DMA 함.
 * 호출 체인: 사용자 → spdk_dma_realloc → spdk_realloc
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
 * spdk_dma_free - spdk_dma_*malloc 으로 받은 버퍼 해제 (사실상 spdk_free 와 동일)
 * "performance path 에서 호출되지 않음" 명시 — cleanup 단계에서만 사용 권장.
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
 * spdk_mempool_get_name - 풀의 이름 문자열 조회 (디버깅/RPC 표시용)
 * 반환된 포인터는 풀 lifetime 동안 유효. 호출자가 free 하지 말 것.
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
 * @return: 0 = 모두 받음, 음수 = 부족 (이 경우 ele_arr 는 미정, 호출자가 fallback 필요).
 * 사용처: NVMe-oF target 처럼 한 요청에 여러 객체 필요 시.
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
 * spdk_mempool_put_bulk - 여러 객체 일괄 반환 (atomic 절감)
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
 * spdk_mempool_count - 현재 풀 내에 "사용 가능한" 객체 수 (in-pool count)
 *
 * 주의: 코어 캐시 / 글로벌 ring 합계의 근사값 — 멀티스레드 환경에서 정확히 일치 보장 X.
 * 모니터링/디버깅 용도. 핫패스 분기 조건으로 사용하지 말 것.
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
 * spdk_mempool_obj_iter - 풀의 모든 객체에 obj_cb 호출 (in-use, in-pool 무관)
 *
 * 사용처: 디버깅, 통계, 외부 매핑 등록(예: RDMA MR 등록을 풀 전체 객체에 일괄 적용).
 * 컨텍스트: 핫패스 금지 — O(N) 비용.
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
 * spdk_mempool_mem_iter - 풀의 각 chunk(연속 IOVA 영역) 에 콜백
 *
 * 사용처: NVMe-oF RDMA target 이 RDMA MR(memory region) 을 풀 chunk 단위로 등록.
 * 풀이 여러 hugepage 에 흩어져 있을 수 있어 chunk 단위 처리가 필요.
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
 * spdk_mempool_lookup - 이름으로 풀 검색 (multi-process 또는 모듈 간 공유)
 */
struct spdk_mempool *spdk_mempool_lookup(const char *name);

/**
 * Get the number of dedicated CPU cores utilized by this env abstraction.
 *
 * \return the number of dedicated CPU cores.
 */
/*
 * [한국어]
 * spdk_env_get_core_count - SPDK 가 보유한 lcore 개수 (= core_mask 의 bit 수)
 * 사용처: reactor 개수 결정, mempool cache 총량 추정.
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
 * spdk_env_get_main_core - 메인 코어(앱 부팅 lcore) 의 ID
 *
 * spdk_env_opts.main_core 또는 자동 선택값. 보통 RPC/관리 작업이 이 코어에서 처리.
 */
uint32_t spdk_env_get_main_core(void);

/**
 * Get the index of the first dedicated CPU core for this application.
 *
 * \return the index of the first dedicated CPU core.
 */
/*
 * [한국어]
 * spdk_env_get_first_core - core_mask 에서 가장 낮은 bit 의 코어 ID
 * SPDK_ENV_FOREACH_CORE 매크로의 시작점.
 */
uint32_t spdk_env_get_first_core(void);

/**
 * Get the index of the last dedicated CPU core for this application.
 *
 * \return the index of the last dedicated CPU core.
 */
/*
 * [한국어]
 * spdk_env_get_last_core - core_mask 에서 가장 높은 bit 의 코어 ID
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
/* [한국어] spdk_env_get_first_numa_id - 시스템의 가장 낮은 NUMA 노드 ID. FOREACH 시작점. */
int32_t spdk_env_get_first_numa_id(void);

/**
 * Get the ID of the last NUMA node on this system.
 *
 * \return the ID of the last NUMA node
 */
/* [한국어] spdk_env_get_last_numa_id - 가장 높은 NUMA 노드 ID. */
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
 * spdk_env_thread_wait_all - 모든 launch_pinned 스레드의 join
 * 종료 단계에서 메인 스레드가 호출. 각 스레드의 fn 이 return 할 때까지 블록.
 */
void spdk_env_thread_wait_all(void);

/**
 * Check whether the calling process is primary process.
 *
 * \return true if the calling process is primary process, or false otherwise.
 */
/*
 * [한국어]
 * spdk_process_is_primary - 현재 프로세스가 DPDK primary 인지 (vs secondary)
 *
 * Multi-process 모델: primary 가 hugepage 를 처음 만들고, secondary 는 attach 만 함.
 * memzone_reserve / mempool_create 는 primary 만 가능, secondary 는 lookup 만.
 * 사용처: bdev/모듈이 init 시 primary/secondary 구분 분기.
 */
bool spdk_process_is_primary(void);

/**
 * Get a monotonic timestamp counter.
 *
 * \return the monotonic timestamp counter.
 */
/*
 * [한국어]
 * spdk_get_ticks - 단조 증가 카운터 (DPDK rte_get_tsc_cycles, 사실상 rdtsc)
 *
 * 사용처: poller 의 deadline, latency 측정. spdk_get_ticks_hz 와 함께 ns/us 환산.
 * 컨텍스트: 어디서나 호출 가능 (시스템콜 없는 사용자 명령어).
 */
uint64_t spdk_get_ticks(void);

/**
 * Get the tick rate of spdk_get_ticks() per second.
 *
 * \return the tick rate of spdk_get_ticks() per second.
 */
/*
 * [한국어]
 * spdk_get_ticks_hz - 1초당 spdk_get_ticks 증가량 (TSC 주파수, Hz)
 *
 * 일반적으로 CPU base clock 에 대응. ns 환산: ns = ticks * 1e9 / hz.
 */
uint64_t spdk_get_ticks_hz(void);

/**
 * Delay the given number of microseconds.
 *
 * \param us Number of microseconds.
 */
/*
 * [한국어]
 * spdk_delay_us - busy-wait 으로 us 마이크로초 대기
 *
 * sleep() 가 아닌 busy-wait — reactor 모델에서 잠들면 polling 멈추므로 절대 X.
 * 사용처: NVMe reset 시퀀스의 spec 명시 대기 등 짧은 폴 대기.
 */
void spdk_delay_us(unsigned int us);

/**
 * Pause CPU execution for a short while
 */
/*
 * [한국어]
 * spdk_pause - x86 PAUSE 인스트럭션 (스핀 락 hint, hyperthread 양보)
 *
 * 짧은 spin loop 에서 SMT 형제에게 자원 양보 + 분기 예측 패널티 회피용.
 * NVMe CQE polling 의 inner loop 에서 자주 사용.
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
 * spdk_ring_free - ring 해제. 비어있어야 안전 (in-flight 메시지 손실 방지).
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
 * spdk_ring_count - 현재 ring 에 들어있는 엔트리 수 (근사값, MP/MC 일 때).
 * 분기 조건이 아닌 모니터링 용도.
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
 * spdk_iommu_is_enabled - 현재 SPDK 가 IOMMU(VFIO) 사용 중인지 보고
 *
 * IOMMU 활성 시: vtophys 가 IOVA(VA 직접) 반환, NVMe 가 직접 가상주소 DMA. 안전.
 * IOMMU 미활성 시: vtophys 가 실제 물리주소 반환 (CAP_SYS_ADMIN 또는 noiommu 모드 필요).
 * 사용처: 드라이버가 unsafe-mode 경고 출력, 또는 DMA 매핑 전략 선택.
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
/* [한국어] spdk_pci_vmd_get_driver - VMD(Volume Management Device) 드라이버 핸들 조회.
 * VMD = Intel CPU 의 PCIe 가상화 컨트롤러 (RAID 카드 같은 자식 NVMe 들을 하나의 PCI 디바이스로 노출).
 * spdk_pci_enumerate 의 driver 인자로 전달하여 VMD 디바이스만 enumerate 한다. */
struct spdk_pci_driver *spdk_pci_vmd_get_driver(void);

/**
 * Get the I/OAT PCI driver object.
 *
 * \return PCI driver.
 */
/* [한국어] I/OAT (Intel I/O Acceleration Technology) DMA 엔진 드라이버 핸들. CPU 메모리 복사 오프로드. */
struct spdk_pci_driver *spdk_pci_ioat_get_driver(void);

/**
 * Get the IDXD PCI driver object.
 *
 * \return PCI driver.
 */
/* [한국어] IDXD (Intel Data Streaming Accelerator) 드라이버 핸들. 차세대 DSA — copy/CRC/compress 가속. */
struct spdk_pci_driver *spdk_pci_idxd_get_driver(void);

/**
 * Get the AE4DMA PCI driver object.
 *
 * \return PCI driver.
 */
/* [한국어] AMD AE4DMA (Engine for DMA) 드라이버 핸들. AMD의 DSA 동등 가속기. */
struct spdk_pci_driver *spdk_pci_ae4dma_get_driver(void);

/**
 * Get the Virtio PCI driver object.
 *
 * \return PCI driver.
 */
/* [한국어] Virtio PCI (virtio-blk/scsi) 드라이버 핸들. QEMU/KVM 게스트의 가상 디바이스 attach 용. */
struct spdk_pci_driver *spdk_pci_virtio_get_driver(void);

/**
 * Get PCI driver by name (e.g. "nvme", "vmd", "ioat").
 */
/* [한국어] 이름 문자열로 등록된 드라이버 검색 — RPC 등 동적 환경에서 사용. */
struct spdk_pci_driver *spdk_pci_get_driver(const char *name);

/**
 * Get the NVMe PCI driver object.
 *
 * \return PCI driver.
 */
/* [한국어] NVMe PCI 드라이버 핸들 — SPDK 의 가장 핵심. lib/nvme 가 등록한 ID 테이블 (Intel/Samsung 등
 * NVMe vendor 들 + class match) 을 가짐. spdk_nvme_probe 가 이 드라이버로 enumerate 호출. */
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
 * spdk_pci_device_unmap_bar - BAR 매핑 해제. detach 시 자동 호출되므로 사용자가 직접 부르는 경우는 드뭄.
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
/* [한국어] spdk_pci_device_disable_interrupt - 인터럽트 비활성. enable 의 짝. */
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
/* [한국어] spdk_pci_device_disable_interrupts - 다중 MSI-X 비활성 + efd close. enable 짝. */
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
 * spdk_pci_device_get_interrupt_efd_by_index - 특정 MSI-X 벡터의 efd 조회
 *
 * NVMe qpair N 의 인터럽트 = index N 의 efd → reactor 의 epoll 셋에 등록.
 */
int spdk_pci_device_get_interrupt_efd_by_index(struct spdk_pci_device *dev, uint32_t index);

/**
 * Get the domain of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return PCI device domain.
 */
/* [한국어] spdk_pci_device_get_domain - dev->addr.domain 조회 (PCI segment 번호). */
uint32_t spdk_pci_device_get_domain(struct spdk_pci_device *dev);

/**
 * Get the bus number of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return PCI bus number.
 */
/* [한국어] spdk_pci_device_get_bus - dev->addr.bus 조회. */
uint8_t spdk_pci_device_get_bus(struct spdk_pci_device *dev);

/**
 * Get the device number within the PCI bus the device is on.
 *
 * \param dev PCI device.
 *
 * \return PCI device number.
 */
/* [한국어] spdk_pci_device_get_dev - dev->addr.dev 조회. */
uint8_t spdk_pci_device_get_dev(struct spdk_pci_device *dev);

/**
 * Get the particular function number represented by struct spdk_pci_device.
 *
 * \param dev PCI device.
 *
 * \return PCI function number.
 */
/* [한국어] spdk_pci_device_get_func - dev->addr.func 조회. */
uint8_t spdk_pci_device_get_func(struct spdk_pci_device *dev);

/**
 * Get the full DomainBDF address of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return PCI address.
 */
/* [한국어] spdk_pci_device_get_addr - 4튜플 BDF 통째로 반환 (값 복사). */
struct spdk_pci_addr spdk_pci_device_get_addr(struct spdk_pci_device *dev);

/**
 * Get the vendor ID of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return vendor ID.
 */
/* [한국어] spdk_pci_device_get_vendor_id - dev->id.vendor_id 조회. */
uint16_t spdk_pci_device_get_vendor_id(struct spdk_pci_device *dev);

/**
 * Get the device ID of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return device ID.
 */
/* [한국어] spdk_pci_device_get_device_id - dev->id.device_id 조회. */
uint16_t spdk_pci_device_get_device_id(struct spdk_pci_device *dev);

/**
 * Get the subvendor ID of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return subvendor ID.
 */
/* [한국어] spdk_pci_device_get_subvendor_id - 서브시스템 vendor 조회. */
uint16_t spdk_pci_device_get_subvendor_id(struct spdk_pci_device *dev);

/**
 * Get the subdevice ID of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return subdevice ID.
 */
/* [한국어] spdk_pci_device_get_subdevice_id - 서브시스템 device 조회. */
uint16_t spdk_pci_device_get_subdevice_id(struct spdk_pci_device *dev);

/**
 * Get the PCI ID of a PCI device.
 *
 * \param dev PCI device.
 *
 * \return PCI ID.
 */
/* [한국어] spdk_pci_device_get_id - 5튜플 ID 통째로 반환. */
struct spdk_pci_id spdk_pci_device_get_id(struct spdk_pci_device *dev);

/**
 * Get the NUMA node the PCI device is on.
 *
 * \param dev PCI device.
 *
 * \return NUMA node index (>= 0).
 */
/* [한국어] spdk_pci_device_get_numa_id - 디바이스가 붙은 NUMA 노드.
 * 사용처: NUMA-local 한 코어를 reactor 로 선택해 cross-NUMA DMA 대역폭 손실 회피. */
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
/* [한국어] spdk_pci_device_unclaim - claim 해제. lock 파일 close. */
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
/* [한국어] spdk_pci_device_cfg_write - cfg_read 의 짝. NVMe 컨트롤러 초기화 시 BME 활성 등에 사용. */
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
/* [한국어] spdk_pci_device_cfg_read8 - 1바이트 편의 래퍼. 정렬 문제를 피하기 위한 타입 분리. */
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
/* [한국어] spdk_pci_device_cfg_write8 - 1바이트 쓰기. */
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
/* [한국어] spdk_pci_device_cfg_read16 - 2바이트 편의 래퍼. Command(0x04)/Status(0x06) 등에 사용. */
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
/* [한국어] spdk_pci_device_cfg_write16 - 2바이트 쓰기. BME 활성: read16(0x04) | 0x4 → write16. */
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
/* [한국어] spdk_pci_device_cfg_read32 - 4바이트 편의 래퍼. Vendor:Device(0x00) 4바이트 한번에 읽기 등. */
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
/* [한국어] spdk_pci_device_cfg_write32 - 4바이트 쓰기. */
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
/* [한국어] spdk_pci_addr_compare - BDF 두 개 lex 비교. allowed/blocked 리스트 검색에 사용. */
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
/* [한국어] spdk_pci_unhook_device - hook 해제. attached 상태 아닌 디바이스에만 호출 가능. */
void spdk_pci_unhook_device(struct spdk_pci_device *dev);

/**
 * Return the type of the PCI device.
 *
 * \param dev PCI device
 *
 * \return string representing the type of the device
 */
/* [한국어] spdk_pci_device_get_type - dev->type 문자열 조회 ("pci", "vfio-user" 등 provider 이름). */
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
/* [한국어] spdk_pci_register_device_provider - provider 글로벌 리스트에 추가.
 * 보통 SPDK_PCI_REGISTER_DEVICE_PROVIDER 매크로를 통해 constructor 단계에서 자동 등록. */
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
/* [한국어] spdk_mem_map_free - map 해제. **pmap 더블 포인터: free 후 NULL 로 자동 클리어. */
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
/* [한국어] spdk_mem_map_clear_translation - set_translation 의 짝. 영역을 default_translation 으로 되돌림. */
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
/* [한국어] spdk_pci_unregister_error_handler - register 의 짝. 핸들러 함수 포인터로 매칭 제거. */
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
