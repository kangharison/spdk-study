/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] RDMA 유틸리티 라이브러리 (rdma_utils.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK가 RDMA(libibverbs/librdmacm)를 사용할 때 여러 모듈에서 공통으로 필요한
 * 보조 자원들을 ref-count 기반 싱글톤으로 관리한다. 구체적으로 다음 4가지를 제공한다:
 *  (1) PD(Protection Domain) 풀 — ibv_context*당 하나의 PD를 alloc해 두고 여러 NVMe-oF qpair가
 *      공유. RDMA device hot-add/hot-remove를 rdma_get_devices()로 동기화하면서 안전한
 *      PD 라이프타임 관리.
 *  (2) Memory Map (MR 캐시) — PD+access_flags 조합당 하나의 spdk_mem_map을 만들어, 동일 PD에서
 *      I/O하는 모듈들이 같은 MR을 공유. spdk_mem_map은 SPDK 메모리 등록(hugepage alloc)
 *      이벤트를 받아 자동으로 ibv_reg_mr/ibv_dereg_mr을 수행하는 알림 콜백 모델이다.
 *  (3) Memory Domain 풀 — PD당 spdk_memory_domain(SPDK_DMA_DEVICE_TYPE_RDMA)을 ref-count 공유.
 *      bdev/accel 레이어가 zero-copy 결정을 내릴 때 같은 도메인의 메모리는 추가 복사 없이 사용 가능.
 *  (4) NUMA helper — rdma_cm_id의 local addr → 인터페이스 → /sys/class/net/.../device/numa_node로
 *      해당 RDMA 디바이스의 NUMA 노드 ID를 얻음. SPDK가 thread/buffer pool을 NUMA-affined하게 배치.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 콜 체인:
 *   lib/nvmf/rdma.c (NVMe-oF target) / module/bdev/nvme RDMA initiator / lib/rdma_provider
 *     → spdk_rdma_utils_*() (이 파일)
 *         → ibv_alloc_pd / ibv_reg_mr / spdk_mem_map_alloc / spdk_memory_domain_create
 *         → rdma_get_devices / rdma_free_devices (librdmacm device discovery)
 *         → ibv_dealloc_pd / ibv_dereg_mr 등 자원 회수
 *   destructor 후크(_rdma_utils_fini)에서 프로세스 종료 시 모든 자원 해제.
 * 이 모듈은 글로벌 상태(g_dev_list, g_rdma_utils_mr_maps, g_memory_domains)를 사용하므로
 * 모든 외부 API는 pthread_mutex로 보호된다 — RDMA core/cleanup 경로가 여러 스레드에서 호출될 수 있음.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(caller): rdma_provider(verbs/mlx5_dv), nvmf/rdma.c, nvme/nvme_rdma.c, bdev_nvme rdma initiator.
 *   특히 spdk_rdma_utils_get_pd / _create_mem_map / _get_memory_domain은 매우 빈번히 호출됨.
 * - 하위(callee): libibverbs(ibv_alloc_pd, ibv_reg_mr, ibv_dereg_mr), librdmacm(rdma_get_devices),
 *   SPDK 자체 라이브러리(spdk_mem_map_*, spdk_memory_domain_*, spdk_net_*, spdk_read_sysfs_*).
 * - 글로벌 상태:
 *     g_dev_list           : ibv_context*당 PD 1개를 ref-count로 공유.
 *     g_rdma_utils_mr_maps : (pd, access_flags)당 mem_map 1개를 ref-count로 공유.
 *     g_memory_domains     : pd당 spdk_memory_domain 1개를 ref-count로 공유.
 *   각각 별도 mutex로 보호.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rdma_utils_device          : ibv_context+pd+ref+removed 플래그 + tailq 링크.
 * - struct spdk_rdma_utils_mem_map    : (pd, access_flags)별 MR 캐시. spdk_mem_map과 NVMe RDMA hooks 보유.
 * - struct rdma_utils_memory_domain   : pd별 SPDK memory domain ref-count entry.
 * - rdma_utils_mem_notify()           : SPDK가 메모리 alloc/free 시 호출하는 콜백 → ibv_reg_mr/dereg_mr.
 * - rdma_check_contiguous_entries()   : mem_map 인접 페이지 통합 가능 여부 판단(같은 MR이면 합칠 수 있음).
 * - spdk_rdma_utils_create_mem_map()  : 새 mem_map alloc 또는 기존 공유. spdk_mem_map_alloc로 알림 등록.
 * - spdk_rdma_utils_free_mem_map()    : ref decrement. 0이면 mem_map 자체 해제.
 * - spdk_rdma_utils_get_translation() : virtual address → ibv_mr* 또는 rkey 변환(NVMe RDMA hooks 분기).
 * - rdma_add_dev/rdma_remove_dev/rdma_sync_dev_list : RDMA device hot-add/remove 동기화.
 * - spdk_rdma_utils_get_pd/_put_pd    : ibv_context당 PD ref-count 풀.
 * - _rdma_utils_fini() (destructor)   : 프로세스 종료 시 모든 자원 강제 해제.
 * - spdk_rdma_utils_get_memory_domain/_put_memory_domain : pd당 memory_domain 풀 관리.
 * - spdk_rdma_cm_id_get_numa_id()     : cm_id의 local addr → NUMA 노드 ID(/sys 파싱).
 */

#include "spdk_internal/rdma_utils.h"
/* [한국어] 이 파일이 노출하는 외부 API 헤더 — spdk_rdma_utils_* 함수와 mem_map/translation 타입 정의. */

#include "spdk/log.h"
/* [한국어] SPDK 로그 매크로. */
#include "spdk/string.h"
/* [한국어] spdk_strerror — errno → 메시지 변환. */
#include "spdk/likely.h"
/* [한국어] 분기 예측 힌트. */
#include "spdk/net.h"
/* [한국어] spdk_net_get_address_string / spdk_net_get_interface_name — sockaddr → 문자열 / IP → ifc 이름. */
#include "spdk/file.h"
/* [한국어] spdk_read_sysfs_attribute_uint32 — /sys/class/net/.../numa_node 같은 sysfs 속성 read 헬퍼. */

#include "spdk_internal/assert.h"
/* [한국어] SPDK_UNREACHABLE 등 내부 assert 매크로. */

#include <rdma/rdma_cma.h>
/* [한국어] librdmacm — rdma_cm_id, rdma_get_devices, rdma_free_devices, rdma_get_local_addr. */
#include <rdma/rdma_verbs.h>
/* [한국어] librdmacm verbs 도우미 — 일부 conv 함수 사용. */

/*
 * [한국어]
 * struct rdma_utils_device - ibv_context와 PD를 ref-count로 공유하기 위한 entry
 *
 * 한 RDMA 디바이스(ibv_context)당 PD 하나를 alloc하고 여러 caller가 공유한다. removed 플래그는
 * RDMA 디바이스가 hot-remove 되었을 때 마킹하고, ref가 0이 되는 시점에 실제로 PD를 해제한다.
 */
struct rdma_utils_device {
	struct ibv_pd			*pd;
	/* [한국어] 이 디바이스에 alloc한 Protection Domain — 모든 caller가 공유하는 단일 PD.
	 * 설정자: rdma_add_dev()의 ibv_alloc_pd 결과.
	 * 읽는 자: spdk_rdma_utils_get_pd가 caller에게 반환.
	 * 값 범위: 유효 ibv_pd*. 디바이스가 removed && ref==0이면 ibv_dealloc_pd 호출 후 free.
	 * 동기화: g_dev_mutex로 보호. */
	struct ibv_context		*context;
	/* [한국어] librdmacm/libibverbs가 노출하는 device 핸들 — 이 entry의 식별 키 역할.
	 * 설정자: rdma_add_dev에서 캡처(rdma_get_devices가 돌려준 컨텍스트).
	 * 읽는 자: get_pd가 caller가 준 context와 매치되는 entry를 찾을 때.
	 * 값 범위: 유효 ibv_context*. hot-remove 후엔 librdmacm이 무효화할 수 있으므로 removed 플래그로 표시.
	 * 동기화: g_dev_mutex. */
	int				ref;
	/* [한국어] 이 디바이스의 PD를 사용 중인 caller 수. 0이고 removed=true면 PD를 dealloc.
	 * 설정자: get_pd에서 ++, put_pd에서 --.
	 * 읽는 자: rdma_remove_dev(ref<=0 && removed면 실제 해제).
	 * 값 범위: 0 이상. 음수가 되면 put_pd 호출이 짝이 안 맞는 버그.
	 * 동기화: g_dev_mutex. */
	bool				removed;
	/* [한국어] 디바이스가 rdma_get_devices 결과에서 사라졌을 때(hot-remove) true로 마크.
	 * 설정자: rdma_sync_dev_list가 새 리스트와 비교 후 사라진 entry에 대해.
	 * 읽는 자: rdma_remove_dev / spdk_rdma_utils_get_pd(removed면 사용 안 함).
	 * 값 범위: false(살아 있음) / true(제거 마킹).
	 * 동기화: g_dev_mutex. */
	TAILQ_ENTRY(rdma_utils_device)	tailq;
	/* [한국어] g_dev_list에 연결되는 sys/queue.h의 TAILQ 포인터. forward+backward 더블 링크.
	 * 설정자: TAILQ_INSERT_TAIL/REMOVE.
	 * 동기화: g_dev_mutex. */
};

/*
 * [한국어]
 * struct spdk_rdma_utils_mem_map - PD+access_flags 조합당 MR 캐시
 *
 * SPDK는 hugepage 메모리를 alloc/free할 때마다 등록된 콜백을 호출한다(spdk_mem_map). 이 구조체는
 * 그 콜백을 받아 ibv_reg_mr/ibv_dereg_mr을 자동 수행해, 한 번 등록하면 모든 SPDK 메모리가
 * 자동으로 RDMA MR로 매핑되도록 한다. 같은 (pd, access_flags) 조합은 ref-count로 공유.
 */
struct spdk_rdma_utils_mem_map {
	struct spdk_mem_map			*map;
	/* [한국어] SPDK 메모리 알림 시스템에 등록된 map. spdk_mem_map_alloc 시 g_rdma_map_ops를
	 * 콜백으로 등록 → 이후 모든 spdk_malloc/free가 rdma_utils_mem_notify를 호출한다.
	 * 설정자: create_mem_map의 spdk_mem_map_alloc.
	 * 읽는 자: get_translation()이 spdk_mem_map_translate로 가상주소→ibv_mr/rkey 변환.
	 * 동기화: 내부적으로 spdk_mem_map이 RCU-like 동기화. */
	struct ibv_pd				*pd;
	/* [한국어] 이 mem_map이 묶인 Protection Domain.
	 * 설정자: create_mem_map의 caller 인자.
	 * 읽는 자: rdma_utils_mem_notify가 ibv_reg_mr 호출 시.
	 * 값 범위: 유효 ibv_pd*.
	 * 동기화: free 전까지 immutable. */
	struct spdk_nvme_rdma_hooks		*hooks;
	/* [한국어] NVMe RDMA initiator(SPDK NVMe 드라이버)가 ibv_reg_mr 대신 사용자 지정
	 * MR 등록 함수를 쓰고 싶을 때 제공하는 후크 — get_rkey 콜백이 있으면 그걸 사용.
	 * 설정자: create_mem_map의 caller 인자(initiator만 NULL이 아닐 수 있음).
	 * 읽는 자: rdma_utils_mem_notify가 alloc/free 분기 시 사용.
	 * 값 범위: NULL(표준 ibv_reg_mr 경로) 또는 사용자 후크 포인터.
	 * 동기화: free 전까지 immutable. */
	uint32_t				ref_count;
	/* [한국어] 이 mem_map을 공유하는 caller 수.
	 * 설정자: create_mem_map(++) / free_mem_map(--).
	 * 읽는 자: free_mem_map이 0이면 실제 free.
	 * 값 범위: 1 이상. 0이 되면 즉시 LIST_REMOVE 후 free.
	 * 동기화: g_rdma_mr_maps_mutex. */
	uint32_t				access_flags;
	/* [한국어] 이 mem_map의 모든 MR에 적용되는 access flags 비트마스크
	 * (IBV_ACCESS_LOCAL_WRITE | _REMOTE_READ | _REMOTE_WRITE 등).
	 * 설정자: create_mem_map. iWARP 디바이스는 RDMA_READ에 REMOTE_WRITE 권한이 추가로 필요해 자동 추가.
	 * 읽는 자: rdma_utils_mem_notify의 ibv_reg_mr 호출.
	 * 값 범위: libibverbs ACCESS 플래그의 비트마스크. */
	LIST_ENTRY(spdk_rdma_utils_mem_map)	link;
	/* [한국어] g_rdma_utils_mr_maps 글로벌 LIST에 연결되는 포인터.
	 * 동기화: g_rdma_mr_maps_mutex. */
};

/*
 * [한국어]
 * struct rdma_utils_memory_domain - PD당 spdk_memory_domain ref-count 풀 entry
 *
 * 같은 PD에 묶인 RDMA 메모리 도메인은 한 개만 만들어 ref-count로 공유한다. 이렇게 하면
 * accel/bdev이 도메인 동등성으로 zero-copy 가능 여부를 판단할 때 객체 비교가 간단해진다.
 */
struct rdma_utils_memory_domain {
	TAILQ_ENTRY(rdma_utils_memory_domain) link;
	/* [한국어] g_memory_domains TAILQ 링크. 동기화: g_memory_domains_lock. */
	uint32_t ref;
	/* [한국어] 이 도메인을 사용 중인 caller 수.
	 * 설정자: get_memory_domain(++) / put_memory_domain(--).
	 * 읽는 자: put이 0이 되면 spdk_memory_domain_destroy 후 free.
	 * 동기화: g_memory_domains_lock. */
	enum spdk_dma_device_type type;
	/* [한국어] 도메인 유형 — 항상 SPDK_DMA_DEVICE_TYPE_RDMA로 설정됨(이 파일 한정).
	 * 설정자: get_memory_domain (현재 코드는 명시적으로 세팅하지 않고 calloc의 0으로 시작 — 실질 사용 안 함).
	 * 동기화: 변경 불필요(immutable after init). */
	struct ibv_pd *pd;
	/* [한국어] 이 도메인이 묶인 PD — entry를 찾을 때의 키.
	 * 설정자: get_memory_domain의 caller 인자.
	 * 읽는 자: get/put이 PD 매치로 entry 검색.
	 * 동기화: free 전까지 immutable. */
	struct spdk_memory_domain *domain;
	/* [한국어] 실제 SPDK memory_domain 객체 — caller에게 노출되는 핸들.
	 * 설정자: spdk_memory_domain_create.
	 * 읽는 자: caller가 RDMA 전송 빌드 시 이 도메인을 식별자로 사용.
	 * 동기화: SPDK memory_domain framework이 내부 락. */
	struct spdk_memory_domain_rdma_ctx rdma_ctx;
	/* [한국어] 도메인 생성 시 user_ctx로 전달된 컨텍스트(ibv_pd 포함). framework이 콜백에서 되돌려줌.
	 * 설정자: get_memory_domain의 ctx 채움.
	 * 읽는 자: spdk_memory_domain framework 콜백.
	 * 값 범위: size=sizeof(rdma_ctx), ibv_pd=유효 PD.
	 * 동기화: immutable after init. */
};

static pthread_mutex_t g_dev_mutex = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] g_dev_list와 g_ctx_list를 보호. RDMA device hot-add/remove와 PD ref 변경 동기화용.
 * NVMe-oF target/initiator는 multi-thread 환경에서 동시에 get_pd를 호출할 수 있으므로 락 필수. */
static struct ibv_context **g_ctx_list = NULL;
/* [한국어] 마지막 rdma_get_devices 호출 결과 — NULL-terminated array. 이걸 보관해야 librdmacm이
 * 내부적으로 가지고 있는 PD 등이 free되지 않는다(rdma_free_devices 호출 시점 제어).
 * 새 sync 시 이전 g_ctx_list를 rdma_free_devices하고 새 결과로 갱신. */
static TAILQ_HEAD(, rdma_utils_device) g_dev_list = TAILQ_HEAD_INITIALIZER(g_dev_list);
/* [한국어] 활성 RDMA device entry들의 리스트. ibv_context 포인터 정렬 순으로 sync_dev_list가 유지. */

static LIST_HEAD(, spdk_rdma_utils_mem_map) g_rdma_utils_mr_maps = LIST_HEAD_INITIALIZER(
			&g_rdma_utils_mr_maps);
/* [한국어] (pd, access_flags) 조합별 mem_map 풀. 동일 조합 요청은 기존 entry 공유.
 * 보호: g_rdma_mr_maps_mutex. */
static pthread_mutex_t g_rdma_mr_maps_mutex = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] g_rdma_utils_mr_maps 보호 락. */

static TAILQ_HEAD(, rdma_utils_memory_domain) g_memory_domains = TAILQ_HEAD_INITIALIZER(
			g_memory_domains);
/* [한국어] PD별 memory_domain 풀.
 * 보호: g_memory_domains_lock. */
static pthread_mutex_t g_memory_domains_lock = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] g_memory_domains 보호 락. */

/*
 * [한국어]
 * rdma_utils_mem_notify - SPDK 메모리 alloc/free 콜백 → ibv_reg_mr/dereg_mr 자동 수행
 *
 * @cb_ctx: spdk_mem_map_alloc 시 등록한 caller 컨텍스트 — 여기서는 spdk_rdma_utils_mem_map*.
 * @map: 알림이 발생한 mem_map 객체.
 * @action: REGISTER(새 메모리 영역이 alloc됨) / UNREGISTER(free됨).
 * @vaddr: 영향 받은 메모리의 가상 주소.
 * @size: 영향 받은 영역 크기.
 * @return: 0 성공, 음수 실패. 실패 시 spdk_mem_map이 alloc을 abort.
 *
 * 동작:
 *  - REGISTER: NVMe RDMA hooks의 get_rkey가 있으면 그걸로 rkey만 받아 translation에 저장
 *    (initiator가 자체적으로 MR을 등록하는 경로). 그 외에는 ibv_reg_mr로 표준 등록 후 ibv_mr*를
 *    translation에 저장. translation은 해당 vaddr 범위에 대한 (ibv_mr* 또는 rkey)로 매핑된다.
 *  - UNREGISTER: 등록했던 ibv_mr을 ibv_dereg_mr 후 translation 제거.
 *
 * 실행 컨텍스트: spdk_malloc/free를 호출한 임의의 스레드. SPDK env가 알림 분배 시 자체 동기화.
 *
 * 호출 체인:
 *   spdk_malloc(SPDK_MALLOC_DMA) → spdk_mem_map의 알림 → 이 함수 → ibv_reg_mr → 커널 → HCA MR 등록
 */
static int
rdma_utils_mem_notify(void *cb_ctx, struct spdk_mem_map *map,
		      enum spdk_mem_map_notify_action action,
		      void *vaddr, size_t size)
{
	struct spdk_rdma_utils_mem_map *rmap = cb_ctx;
	/* [한국어] cb_ctx로 들어오는 것은 우리가 spdk_mem_map_alloc 시 넘긴 spdk_rdma_utils_mem_map*. */
	struct ibv_pd *pd = rmap->pd;
	/* [한국어] 등록 대상 PD. */
	struct ibv_mr *mr;
	/* [한국어] ibv_reg_mr 결과 보관용. */
	uint32_t access_flags;
	/* [한국어] access flags의 임시 복사본 — IBV_ACCESS_RELAXED_ORDERING을 conditional하게 추가. */
	int rc;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		/* [한국어] 새 메모리 영역 alloc — RDMA MR로 등록 필요. */
		if (rmap->hooks && rmap->hooks->get_rkey) {
			/* [한국어] NVMe RDMA initiator가 자체 MR 관리 후 rkey만 돌려주는 경로
			 * (예: 외부 메모리 풀에서 가져와 이미 등록된 메모리 사용). ibv_reg_mr 우회. */
			rc = spdk_mem_map_set_translation(map, (uint64_t)vaddr, size,
							  rmap->hooks->get_rkey(pd, vaddr, size));
			/* [한국어] translation 값으로 rkey(uint32 → 64bit 채움) 저장. get_translation에서
			 * mr_or_key.key로 꺼냄. */
		} else {
			/* [한국어] 표준 경로 — ibv_reg_mr로 새 MR 등록. */
			access_flags = rmap->access_flags;
#ifdef IBV_ACCESS_OPTIONAL_FIRST
			access_flags |= IBV_ACCESS_RELAXED_ORDERING;
			/* [한국어] libibverbs가 RELAXED_ORDERING(원자성 약화 허용)을 지원하면 추가.
			 * Mellanox HCA에서 PCIe TLP 순서를 완화해 throughput을 높일 수 있음.
			 * IBV_ACCESS_OPTIONAL_FIRST 매크로 정의 자체가 이 옵셔널 플래그군의 존재를 의미. */
#endif
			mr = ibv_reg_mr(pd, vaddr, size, access_flags);
			/* [한국어] libibverbs로 메모리 영역 등록 — 커널이 페이지 테이블 pin + HCA에 MR 컨텍스트
			 * 생성. 반환된 ibv_mr*에는 lkey/rkey가 들어 있어 이후 send_wr에서 사용. */
			if (mr == NULL) {
				/* [한국어] 등록 실패 — pin 가능한 페이지 부족 또는 HCA MR 자원 부족 등. */
				SPDK_ERRLOG("ibv_reg_mr() failed\n");
				return -1;
			} else {
				rc = spdk_mem_map_set_translation(map, (uint64_t)vaddr, size, (uint64_t)mr);
				/* [한국어] translation 값으로 ibv_mr* 저장(uint64_t로 cast). get_translation에서 복원. */
			}
		}
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		/* [한국어] 메모리 영역 free됨 — MR 해제 필요. */
		if (rmap->hooks == NULL || rmap->hooks->get_rkey == NULL) {
			/* [한국어] hooks 미사용 경로(=ibv_mr 직접 등록 경로)에서만 dereg 호출.
			 * hooks 경로는 caller가 자체 정리. */
			mr = (struct ibv_mr *)spdk_mem_map_translate(map, (uint64_t)vaddr, NULL);
			/* [한국어] vaddr에 매핑된 ibv_mr* 복원. NULL이면 등록되지 않았던 영역 — skip. */
			if (mr) {
				ibv_dereg_mr(mr);
				/* [한국어] HCA에서 MR 컨텍스트 제거 + 페이지 unpin. */
			}
		}
		rc = spdk_mem_map_clear_translation(map, (uint64_t)vaddr, size);
		/* [한국어] translation 테이블에서 해당 영역 제거 — 양쪽 경로 공통. */
		break;
	default:
		SPDK_UNREACHABLE();
		/* [한국어] enum 신규 값 추가 시 컴파일러가 잡아주지 못하는 케이스 — 런타임 fatal. */
	}

	return rc;
}

/*
 * [한국어]
 * rdma_check_contiguous_entries - mem_map 인접 페이지 통합 가능 여부 판단
 *
 * @addr_1, @addr_2: 두 인접 페이지 entry의 translation 값.
 * @return: 1 = 통합 가능(같은 ibv_mr*로 등록되어 있음), 0 = 통합 불가.
 *
 * spdk_mem_map은 페이지 단위 translation 테이블이지만, 같은 MR이 연속된 페이지에 매핑되면
 * 이 콜백이 1을 돌려줘 entry를 합쳐 메모리/lookup 효율을 높인다.
 *
 * 호출 체인:
 *   spdk_mem_map 내부 알림 처리 → 이 콜백
 */
static int
rdma_check_contiguous_entries(uint64_t addr_1, uint64_t addr_2)
{
	/* Two contiguous mappings will point to the same address which is the start of the RDMA MR. */
	/* [한국어] 동일 ibv_mr*에 매핑된 두 페이지는 translation 값(=ibv_mr* 또는 rkey)이 같다.
	 * 이 점을 이용해 단순 == 비교만으로 통합 가능 여부 판정. */
	return addr_1 == addr_2;
}

const struct spdk_mem_map_ops g_rdma_map_ops = {
	.notify_cb = rdma_utils_mem_notify,
	/* [한국어] alloc/free 알림 콜백 — 위 함수 등록. */
	.are_contiguous = rdma_check_contiguous_entries
	/* [한국어] 인접 통합 콜백. */
};
/* [한국어] spdk_mem_map_alloc 시 이 ops 묶음을 넘기면 SPDK env가 메모리 이벤트마다 콜백 호출. */

/*
 * [한국어]
 * _rdma_free_mem_map - mem_map의 alloc 출처에 따라 적절한 free 호출
 *
 * @map: 해제할 mem_map. NULL 비허용(assert).
 *
 * NVMe RDMA hooks가 있는 경로는 spdk_zmalloc(DMA-able hugepage)로 alloc되었고, 그 외는
 * 일반 calloc이라 짝맞춰 free. spdk_zmalloc/spdk_free와 calloc/free를 섞으면 heap corruption.
 *
 * 호출 체인:
 *   spdk_rdma_utils_create_mem_map(에러 경로) / spdk_rdma_utils_free_mem_map(refcount 0)
 *     → 이 함수
 */
static void
_rdma_free_mem_map(struct spdk_rdma_utils_mem_map *map)
{
	assert(map);

	if (map->hooks) {
		/* [한국어] hooks 경로 alloc(spdk_zmalloc) → spdk_free 짝맞춤. */
		spdk_free(map);
	} else {
		/* [한국어] 표준 경로 calloc → 일반 free. */
		free(map);
	}
}

/*
 * [한국어]
 * spdk_rdma_utils_create_mem_map - PD+access_flags 조합당 mem_map 생성/공유
 *
 * @pd: 등록할 PD. iWARP 디바이스면 access_flags에 자동으로 REMOTE_WRITE 추가.
 * @hooks: NVMe RDMA initiator가 자체 MR 관리 시 제공(NULL이면 표준 ibv_reg_mr 경로).
 * @access_flags: IBV_ACCESS_* 비트마스크.
 * @return: 새 또는 공유된 spdk_rdma_utils_mem_map*; NULL은 실패.
 *
 * 동작:
 *  1) g_rdma_utils_mr_maps 락 잡고 동일 (pd, access_flags) 검색 — 있으면 ref++ 후 반환.
 *  2) 없으면 새로 alloc. hooks가 있으면 DMA-able hugepage(spdk_zmalloc)에서, 없으면 calloc.
 *  3) 필드 초기화 + spdk_mem_map_alloc(g_rdma_map_ops)로 메모리 알림 콜백 등록.
 *  4) 글로벌 리스트에 INSERT_HEAD.
 *
 * 동기화: g_rdma_mr_maps_mutex.
 *
 * 호출 체인:
 *   nvmf_rdma_*_init / nvme_rdma_qpair_create → 이 함수 → spdk_mem_map_alloc → 메모리 알림 콜백 등록
 */
struct spdk_rdma_utils_mem_map *
spdk_rdma_utils_create_mem_map(struct ibv_pd *pd, struct spdk_nvme_rdma_hooks *hooks,
			       uint32_t access_flags)
{
	struct spdk_rdma_utils_mem_map *map;

	if (pd->context->device->transport_type == IBV_TRANSPORT_IWARP) {
		/* IWARP requires REMOTE_WRITE permission for RDMA_READ operation */
		/* [한국어] iWARP transport의 특이점 — RDMA READ도 peer가 REMOTE_WRITE 권한을 쥐고 있어야 한다.
		 * 이는 IB/RoCE와 다른 iWARP 프로토콜 정의로, SPDK가 자동 보정. */
		access_flags |= IBV_ACCESS_REMOTE_WRITE;
	}

	pthread_mutex_lock(&g_rdma_mr_maps_mutex);
	/* [한국어] 글로벌 mem_map 리스트 보호 락 진입. */
	/* Look up existing mem map registration for this pd */
	LIST_FOREACH(map, &g_rdma_utils_mr_maps, link) {
		/* [한국어] 동일 (pd, access_flags) entry 검색. */
		if (map->pd == pd && map->access_flags == access_flags) {
			/* [한국어] 매치 — ref-count 증가 후 즉시 반환. */
			map->ref_count++;
			pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
			return map;
		}
	}

	if (hooks) {
		/* [한국어] hooks 경로 — DMA-able hugepage에서 alloc(spdk_zmalloc).
		 * NVMe initiator의 MR 관리 콜백이 hugepage 영역에 alloc된 메타데이터를 직접 참조할 수 있도록.
		 * SPDK_ENV_NUMA_ID_ANY = 어느 NUMA든 무관, SPDK_MALLOC_DMA = DMA pinned. */
		map = spdk_zmalloc(sizeof(*map), 0, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	} else {
		/* [한국어] 일반 경로 — 표준 heap alloc. */
		map = calloc(1, sizeof(*map));
	}
	if (!map) {
		pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
		/* [한국어] alloc 실패 — 락 풀고 NULL 반환. */
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}
	map->pd = pd;
	/* [한국어] PD 저장 — entry 검색 키. */
	map->ref_count = 1;
	/* [한국어] 첫 ref 카운트 — 이번 호출자가 보유. */
	map->hooks = hooks;
	/* [한국어] hooks 보관 — notify 콜백에서 분기. */
	map->access_flags = access_flags;
	/* [한국어] iWARP 보정 후 최종 access flags. */
	map->map = spdk_mem_map_alloc(0, &g_rdma_map_ops, map);
	/* [한국어] SPDK 메모리 알림 시스템에 등록 — 첫 인자 0은 default translation value(아직 매핑 없음).
	 * 이 시점에 SPDK env가 이미 alloc해 둔 모든 hugepage 영역에 대해 즉시 NOTIFY_REGISTER 콜백을 발행.
	 * 즉 ibv_reg_mr이 기존 alloc된 메모리 전체에 대해 자동 호출됨. */
	if (!map->map) {
		SPDK_ERRLOG("Unable to create memory map\n");
		_rdma_free_mem_map(map);
		pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
		return NULL;
	}
	LIST_INSERT_HEAD(&g_rdma_utils_mr_maps, map, link);
	/* [한국어] 글로벌 풀에 등록 — 다음 호출자가 공유 가능. */

	pthread_mutex_unlock(&g_rdma_mr_maps_mutex);

	return map;
}

/*
 * [한국어]
 * spdk_rdma_utils_free_mem_map - mem_map 참조 해제. ref_count 0이면 실제 해제
 *
 * @_map: 해제할 mem_map의 포인터의 포인터(double pointer). 함수가 *_map을 NULL로 만들어
 *         caller의 더블 free를 방지.
 *
 * 동작: ref_count--. 0이 되면 LIST_REMOVE → spdk_mem_map_free → _rdma_free_mem_map.
 *
 * 동기화: g_rdma_mr_maps_mutex.
 *
 * 호출 체인:
 *   nvmf/nvme rdma teardown → 이 함수 → spdk_mem_map_free(자동 NOTIFY_UNREGISTER 발행)
 *     → rdma_utils_mem_notify → ibv_dereg_mr
 */
void
spdk_rdma_utils_free_mem_map(struct spdk_rdma_utils_mem_map **_map)
{
	struct spdk_rdma_utils_mem_map *map;

	if (!_map) {
		/* [한국어] caller가 NULL을 넘긴 경우 — no-op로 안전. */
		return;
	}

	map = *_map;
	if (!map) {
		/* [한국어] 이미 해제된 슬롯 — 더블 free 방지. */
		return;
	}
	*_map = NULL;
	/* [한국어] caller의 포인터 무효화 — 이후 caller가 같은 슬롯으로 접근 시 NULL이 됨. */

	pthread_mutex_lock(&g_rdma_mr_maps_mutex);
	assert(map->ref_count > 0);
	/* [한국어] 0 또는 음수면 짝이 안 맞는 free 호출 — 디버그 빌드에서 abort. */
	map->ref_count--;
	if (map->ref_count != 0) {
		/* [한국어] 아직 다른 caller가 사용 중 — 글로벌 리스트에 그대로 둠. */
		pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
		return;
	}

	LIST_REMOVE(map, link);
	/* [한국어] 마지막 ref였음 — 글로벌 풀에서 제거. */
	pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
	if (map->map) {
		spdk_mem_map_free(&map->map);
		/* [한국어] mem_map free → SPDK env가 모든 등록 영역에 NOTIFY_UNREGISTER 콜백 발행
		 * → rdma_utils_mem_notify가 ibv_dereg_mr 호출. */
	}
	_rdma_free_mem_map(map);
	/* [한국어] mem_map wrapper 해제(spdk_free 또는 free). */
}

/*
 * [한국어]
 * spdk_rdma_utils_get_translation - 가상 주소 → ibv_mr* 또는 rkey 변환
 *
 * @map: 검색할 mem_map.
 * @address: 변환할 가상 주소(send/recv WR이 가리키는 실제 데이터 버퍼).
 * @length: 요청한 변환 영역 길이. real_length는 spdk_mem_map_translate가 실제 매핑된 길이를 반환.
 * @translation: 출력 — translation_type(MR or KEY)과 해당 값.
 * @return: 0 성공, -EINVAL = MR 미등록.
 *
 * 동작: hooks가 있으면 KEY 타입(rkey)으로, 없으면 MR 타입(ibv_mr*)으로 변환 결과 채움.
 * caller(rdma_provider 등)는 이 결과를 send_wr.sg_list[i].lkey 등에 사용.
 *
 * 실행 컨텍스트: 핫패스(I/O 마다 호출). spdk_mem_map_translate는 lock-free O(1) lookup.
 *
 * 호출 체인:
 *   nvmf_rdma_request_fill_iovs / nvme_rdma_build_sgl → 이 함수 → spdk_mem_map_translate
 */
int
spdk_rdma_utils_get_translation(struct spdk_rdma_utils_mem_map *map, void *address,
				size_t length, struct spdk_rdma_utils_memory_translation *translation)
{
	uint64_t real_length = length;
	/* [한국어] spdk_mem_map_translate가 실제 매핑된 길이로 갱신해 줌(요청보다 클 수 있음). */

	assert(map);
	assert(address);
	assert(translation);

	if (map->hooks && map->hooks->get_rkey) {
		/* [한국어] hooks 경로 — translation 값은 rkey(uint64로 저장돼 있음). */
		translation->translation_type = SPDK_RDMA_UTILS_TRANSLATION_KEY;
		translation->mr_or_key.key = spdk_mem_map_translate(map->map, (uint64_t)address, &real_length);
	} else {
		/* [한국어] 표준 경로 — translation 값은 ibv_mr* 캐스팅된 uint64. */
		translation->translation_type = SPDK_RDMA_UTILS_TRANSLATION_MR;
		translation->mr_or_key.mr = (struct ibv_mr *)spdk_mem_map_translate(map->map, (uint64_t)address,
					    &real_length);
		if (spdk_unlikely(!translation->mr_or_key.mr)) {
			/* [한국어] 미등록 영역 — caller가 spdk_malloc(SPDK_MALLOC_DMA)로 alloc하지 않은
			 * 메모리를 RDMA로 보내려는 경우. 정책상 fatal로 처리. */
			SPDK_ERRLOG("No translation for ptr %p, size %zu\n", address, length);
			return -EINVAL;
		}
	}

	assert(real_length >= length);
	/* [한국어] 매핑 영역이 요청보다 작으면 caller가 잘못된 가정 하에 호출한 것 — 디버그 abort. */

	return 0;
}


/*
 * [한국어]
 * rdma_add_dev - g_dev_list에 새 ibv_context entry 추가 및 PD alloc
 *
 * @context: 새로 발견된 RDMA 디바이스의 ibv_context*.
 * @return: 새 entry; 실패 시 NULL.
 *
 * 호출 컨텍스트: rdma_sync_dev_list 내부에서만 호출(g_dev_mutex 보유 상태).
 *
 * 호출 체인:
 *   rdma_sync_dev_list → 이 함수 → ibv_alloc_pd → 커널 → HCA에 PD 컨텍스트 생성
 */
static struct rdma_utils_device *
rdma_add_dev(struct ibv_context *context)
{
	struct rdma_utils_device *dev;

	dev = calloc(1, sizeof(*dev));
	/* [한국어] entry zero-init alloc. ref=0, removed=false로 시작. */
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate RDMA device object.\n");
		return NULL;
	}

	dev->pd = ibv_alloc_pd(context);
	/* [한국어] 이 디바이스에 새 Protection Domain 할당 — HCA에 PD 컨텍스트 생성.
	 * 이후 모든 caller(get_pd 호출자)가 이 PD를 ref-count로 공유. */
	if (dev->pd == NULL) {
		SPDK_ERRLOG("ibv_alloc_pd() failed: %s (%d)\n", spdk_strerror(errno), errno);
		free(dev);
		return NULL;
	}

	dev->context = context;
	/* [한국어] entry 식별 키. */
	TAILQ_INSERT_TAIL(&g_dev_list, dev, tailq);
	/* [한국어] 글로벌 리스트 끝에 추가. caller가 락을 잡은 상태라 thread-safe. */

	return dev;
}

/*
 * [한국어]
 * rdma_remove_dev - 조건부 device entry 제거(removed=true && ref==0인 경우만)
 *
 * @dev: 제거 후보 entry.
 *
 * 동작: 조건 만족 시 TAILQ_REMOVE → ibv_dealloc_pd → free. 그렇지 않으면 no-op.
 * 이런 지연 정리 패턴은 hot-remove된 디바이스라도 아직 사용 중인 caller가 있을 수 있으므로
 * ref가 0이 될 때까지 PD를 유지하기 위함.
 *
 * 호출 컨텍스트: g_dev_mutex 보유 상태에서만 호출.
 *
 * 호출 체인:
 *   rdma_sync_dev_list / spdk_rdma_utils_put_pd / _rdma_utils_fini → 이 함수 → ibv_dealloc_pd
 */
static void
rdma_remove_dev(struct rdma_utils_device *dev)
{
	if (!dev->removed || dev->ref > 0) {
		/* [한국어] 아직 살아 있거나 사용 중 — 정리 보류. */
		return;
	}

	/* Deallocate protection domain only if the device is already removed and
	 * there is no reference.
	 */
	/* [한국어] 디바이스가 hot-remove되고 모든 caller가 put_pd를 호출해 ref==0인 시점에만 실제 해제.
	 * 이 시점에는 어떤 외부 코드도 PD를 안 잡고 있음이 보장. */
	TAILQ_REMOVE(&g_dev_list, dev, tailq);
	/* [한국어] 글로벌 리스트에서 분리. */
	ibv_dealloc_pd(dev->pd);
	/* [한국어] HCA에서 PD 컨텍스트 회수 + 커널 자원 정리. */
	free(dev);
	/* [한국어] entry 자체 free. */
}

/*
 * [한국어]
 * ctx_cmp - qsort 비교 함수: ibv_context* 포인터 주소 정렬
 *
 * @_c1, @_c2: 비교 대상(이중 포인터를 캐스팅).
 * @return: <0 / 0 / >0.
 *
 * rdma_sync_dev_list가 이전 g_ctx_list와 새 결과를 merge sort 비교로 빠르게 매칭하기 위해
 * 정렬을 사용. 포인터 비교라 stable하지만 실제 정렬 기준은 구현 정의(주소 비교)이므로
 * 의미론적 정렬이 아니라 단순 일관성 정렬.
 *
 * 호출 체인:
 *   qsort → 이 함수
 */
static int
ctx_cmp(const void *_c1, const void *_c2)
{
	struct ibv_context *c1 = *(struct ibv_context **)_c1;
	/* [한국어] qsort는 element 포인터를 넘겨주므로 한 번 dereference. */
	struct ibv_context *c2 = *(struct ibv_context **)_c2;

	return c1 < c2 ? -1 : c1 > c2;
	/* [한국어] 단순 주소 비교 — 같으면 0(자동, 두 조건 모두 false). */
}

/*
 * [한국어]
 * rdma_sync_dev_list - rdma_get_devices()로 현재 RDMA 디바이스 상태를 g_dev_list와 동기화
 *
 * @return: 0 성공, -ENODEV = 디바이스 없음 또는 rdma_get_devices 실패.
 *
 * 동작:
 *  1) rdma_get_devices(num_devs)로 현재 OS의 모든 RDMA 디바이스 목록 획득.
 *  2) num_devs==0이면 fatal — RDMA 환경 자체가 없음.
 *  3) qsort로 새 리스트를 포인터 정렬.
 *  4) g_ctx_list가 NULL이면 첫 호출 — 모든 디바이스를 add.
 *  5) 그렇지 않으면 두 정렬 리스트를 merge sort 식으로 비교:
 *     - new에만 있음 → rdma_add_dev (HCA hot-add).
 *     - old에만 있음 → 해당 entry를 removed=true 후 rdma_remove_dev(조건부 정리).
 *     - 양쪽 동일 → 변경 없음.
 *  6) 이전 g_ctx_list를 rdma_free_devices로 해제, 새 리스트를 g_ctx_list에 보관.
 *
 * 호출 컨텍스트: get_pd / put_pd 등 외부 API가 락 보유 상태에서 호출.
 *
 * 호출 체인:
 *   spdk_rdma_utils_get_pd / _put_pd → 이 함수 → rdma_get_devices(librdmacm)
 */
static int
rdma_sync_dev_list(void)
{
	struct ibv_context **new_ctx_list;
	int i, j;
	int num_devs = 0;

	/*
	 * rdma_get_devices() returns a NULL terminated array of opened RDMA devices,
	 * and sets num_devs to the number of the returned devices.
	 */
	/* [한국어] librdmacm이 OS에 보이는 RDMA 디바이스 목록을 NULL-terminated 포인터 배열로 반환.
	 * 호출자가 rdma_free_devices로 free해야 librdmacm 내부의 컨텍스트가 정리된다. */
	new_ctx_list = rdma_get_devices(&num_devs);
	if (new_ctx_list == NULL) {
		SPDK_ERRLOG("rdma_get_devices() failed: %s (%d)\n", spdk_strerror(errno), errno);
		return -ENODEV;
	}

	if (num_devs == 0) {
		/* [한국어] RDMA 디바이스 자체가 시스템에 없음 — 이 모듈을 사용할 수 없음. */
		rdma_free_devices(new_ctx_list);
		SPDK_ERRLOG("Returned RDMA device array was empty\n");
		return -ENODEV;
	}

	/*
	 * Sort new_ctx_list by addresses to update devices easily.
	 */
	/* [한국어] 두 리스트를 merge sort 식 비교로 효율적으로 동기화하기 위해 정렬. */
	qsort(new_ctx_list, num_devs, sizeof(struct ibv_context *), ctx_cmp);

	if (g_ctx_list == NULL) {
		/* If no old array, this is the first call. Add all devices. */
		/* [한국어] 첫 호출 — 비교 대상이 없으므로 모든 새 디바이스를 add. */
		for (i = 0; new_ctx_list[i] != NULL; i++) {
			rdma_add_dev(new_ctx_list[i]);
		}

		goto exit;
		/* [한국어] g_ctx_list 갱신만 남음 — exit 라벨로 점프. */
	}

	for (i = j = 0; new_ctx_list[i] != NULL || g_ctx_list[j] != NULL;) {
		/* [한국어] 두 정렬 리스트를 동시에 순회하는 merge 패턴 — 양쪽 모두 끝나야 종료. */
		struct ibv_context *new_ctx = new_ctx_list[i];
		struct ibv_context *old_ctx = g_ctx_list[j];
		bool add = false, remove = false;
		/* [한국어] 이번 iteration에서 add/remove 결정. 둘 다 false면 동일 — skip. */

		/*
		 * If a context exists only in the new array, create a device for it,
		 * or if a context exists only in the old array, try removing the
		 * corresponding device.
		 */

		if (old_ctx == NULL) {
			/* [한국어] old 리스트가 끝났는데 new에 남은 것 — 신규 디바이스. */
			add = true;
		} else if (new_ctx == NULL) {
			/* [한국어] new 리스트가 끝났는데 old에 남은 것 — 제거된 디바이스. */
			remove = true;
		} else if (new_ctx < old_ctx) {
			/* [한국어] 새 포인터가 더 작음 → new에만 있음(정렬 기준). add. */
			add = true;
		} else if (old_ctx < new_ctx) {
			/* [한국어] 옛 포인터가 더 작음 → old에만 있음. remove. */
			remove = true;
		}
		/* [한국어] 둘이 같으면 add=false, remove=false — 동일 디바이스 skip. */

		if (add) {
			rdma_add_dev(new_ctx_list[i]);
			/* [한국어] 새 디바이스 entry 생성 + PD alloc. */
			i++;
		} else if (remove) {
			struct rdma_utils_device *dev, *tmp;

			TAILQ_FOREACH_SAFE(dev, &g_dev_list, tailq, tmp) {
				/* [한국어] g_dev_list에서 사라진 ibv_context와 일치하는 entry 찾기. */
				if (dev->context == g_ctx_list[j]) {
					dev->removed = true;
					/* [한국어] hot-remove 마킹 — 이 시점부터 get_pd가 이 entry를 무시. */
					rdma_remove_dev(dev);
					/* [한국어] ref==0이면 즉시 정리, 아니면 보류(나중 put_pd가 마지막일 때 정리). */
				}
			}
			j++;
		} else {
			/* [한국어] 동일 — 양쪽 인덱스 모두 진행. */
			i++;
			j++;
		}
	}

	/* Free the old array. */
	/* [한국어] 더 이상 비교 안 쓸 이전 리스트는 librdmacm에 돌려줌(내부 자원 회수). */
	rdma_free_devices(g_ctx_list);

exit:
	/*
	 * Keep the newly returned array so that allocated protection domains
	 * are not freed unexpectedly.
	 */
	/* [한국어] librdmacm이 ibv_context를 살아 있게 유지하려면 이 array를 free하지 않고 들고 있어야 한다.
	 * 다음 sync 호출까지 caller(우리)가 owner. */
	g_ctx_list = new_ctx_list;
	return 0;
}

/*
 * [한국어]
 * spdk_rdma_utils_get_pd - ibv_context에 매핑된 PD를 ref-count로 획득
 *
 * @context: 대상 디바이스의 ibv_context*.
 * @return: 공유 PD 포인터; NULL = 실패.
 *
 * 동작:
 *  1) g_dev_mutex 락.
 *  2) rdma_sync_dev_list — 디바이스 hot-add/remove 반영.
 *  3) g_dev_list에서 context 매치되고 removed=false인 entry 찾기 → ref++ → PD 반환.
 *  4) 없으면 에러.
 *
 * 호출 체인:
 *   nvmf_rdma_listener_init / nvme_rdma_qpair_create → 이 함수
 */
struct ibv_pd *
spdk_rdma_utils_get_pd(struct ibv_context *context)
{
	struct rdma_utils_device *dev;
	int rc;

	pthread_mutex_lock(&g_dev_mutex);
	/* [한국어] 글로벌 dev/ctx 리스트 보호 락 진입. */

	rc = rdma_sync_dev_list();
	/* [한국어] 매 get_pd마다 hot-add/remove 동기화 — 약간의 비용이 있지만 정확성 우선. */
	if (rc != 0) {
		pthread_mutex_unlock(&g_dev_mutex);

		SPDK_ERRLOG("Failed to sync RDMA device list\n");
		return NULL;
	}

	TAILQ_FOREACH(dev, &g_dev_list, tailq) {
		/* [한국어] context 매치 + removed 아닌 entry 찾기. */
		if (dev->context == context && !dev->removed) {
			dev->ref++;
			/* [한국어] ref-count 증가 — 이 caller가 사용 중임을 표시. */
			pthread_mutex_unlock(&g_dev_mutex);

			return dev->pd;
		}
	}

	pthread_mutex_unlock(&g_dev_mutex);

	/* [한국어] 매치 entry 없음 — caller가 우리 모듈이 모르는 context를 넘겼거나 hot-remove 중. */
	SPDK_ERRLOG("Failed to get PD\n");
	return NULL;
}

/*
 * [한국어]
 * spdk_rdma_utils_put_pd - PD 참조 해제. ref==0이고 removed=true면 PD 실제 해제
 *
 * @pd: 해제할 PD 포인터.
 *
 * 동작:
 *  1) g_dev_mutex 락.
 *  2) g_dev_list에서 dev->pd == pd entry 찾기 → ref-- → rdma_remove_dev로 조건부 정리.
 *  3) 마지막에 rdma_sync_dev_list로 hot-remove 반영(다른 디바이스가 사라졌을 수 있음).
 *
 * 호출 체인:
 *   nvmf/nvme rdma teardown → 이 함수 → 조건부 ibv_dealloc_pd
 */
void
spdk_rdma_utils_put_pd(struct ibv_pd *pd)
{
	struct rdma_utils_device *dev, *tmp;

	pthread_mutex_lock(&g_dev_mutex);

	TAILQ_FOREACH_SAFE(dev, &g_dev_list, tailq, tmp) {
		/* [한국어] _SAFE 순회 — rdma_remove_dev가 entry를 free할 수 있어 tmp로 다음 노드 미리 보관. */
		if (dev->pd == pd) {
			assert(dev->ref > 0);
			/* [한국어] 0이면 짝 안 맞는 put — fatal. */
			dev->ref--;

			rdma_remove_dev(dev);
			/* [한국어] ref==0 + removed=true면 즉시 정리. 아니면 no-op. */
		}
	}

	rdma_sync_dev_list();
	/* [한국어] 다른 디바이스가 hot-remove됐는지 확인 — 정리 가능한 entry가 더 있을 수 있음. */

	pthread_mutex_unlock(&g_dev_mutex);
}

/*
 * [한국어]
 * _rdma_utils_fini - 프로세스 종료 시 모든 RDMA 자원 강제 해제 (destructor)
 *
 * GCC __attribute__((destructor)): main이 끝나거나 exit() 직후 자동 호출.
 *
 * 동작: g_dev_list의 모든 entry를 ref=0, removed=true로 강제 마킹 → rdma_remove_dev로 정리.
 * 이후 g_ctx_list도 rdma_free_devices로 회수.
 *
 * 호출 체인:
 *   exit() / 프로세스 종료 → libc가 destructor 호출 → 이 함수
 */
__attribute__((destructor)) static void
_rdma_utils_fini(void)
{
	struct rdma_utils_device *dev, *tmp;

	TAILQ_FOREACH_SAFE(dev, &g_dev_list, tailq, tmp) {
		dev->removed = true;
		/* [한국어] 모든 entry hot-remove 마킹. */
		dev->ref = 0;
		/* [한국어] caller가 put을 안 했더라도 강제 0으로 — 어차피 프로세스 종료 시점이라 상관없음. */
		rdma_remove_dev(dev);
		/* [한국어] 즉시 정리(ibv_dealloc_pd + free). */
	}

	if (g_ctx_list != NULL) {
		rdma_free_devices(g_ctx_list);
		/* [한국어] librdmacm 측 컨텍스트 자원 회수. */
		g_ctx_list = NULL;
	}
}

/*
 * [한국어]
 * spdk_rdma_utils_get_memory_domain - PD당 spdk_memory_domain ref-count 획득
 *
 * @pd: 도메인을 묶을 PD.
 * @return: spdk_memory_domain* — caller에게 노출되는 핸들; NULL = 실패.
 *
 * 동작:
 *  1) g_memory_domains_lock 잡고 PD 매치 entry 검색 — 있으면 ref++ 후 반환.
 *  2) 없으면 entry alloc + spdk_memory_domain_create(SPDK_DMA_DEVICE_TYPE_RDMA, ctx,
 *     SPDK_RDMA_DMA_DEVICE)로 도메인 생성.
 *  3) ctx에 ibv_pd를 user_ctx로 실어 framework이 콜백 시 PD를 알 수 있게 함.
 *  4) g_memory_domains에 추가.
 *
 * 호출 체인:
 *   rdma_provider_verbs_qp_create / 기타 → 이 함수 → spdk_memory_domain_create
 */
struct spdk_memory_domain *
spdk_rdma_utils_get_memory_domain(struct ibv_pd *pd)
{
	struct rdma_utils_memory_domain *domain = NULL;
	struct spdk_memory_domain_ctx ctx = {};
	int rc;

	pthread_mutex_lock(&g_memory_domains_lock);

	TAILQ_FOREACH(domain, &g_memory_domains, link) {
		/* [한국어] PD 매치 entry 검색 — 있으면 ref-count로 공유. */
		if (domain->pd == pd) {
			domain->ref++;
			pthread_mutex_unlock(&g_memory_domains_lock);
			return domain->domain;
		}
	}

	domain = calloc(1, sizeof(*domain));
	/* [한국어] 새 entry alloc(없을 때만 도달). */
	if (!domain) {
		SPDK_ERRLOG("Memory allocation failed\n");
		pthread_mutex_unlock(&g_memory_domains_lock);
		return NULL;
	}

	domain->rdma_ctx.size = sizeof(domain->rdma_ctx);
	/* [한국어] forward/backward compat용 size — framework이 sizeof 비교로 버전 식별. */
	domain->rdma_ctx.ibv_pd = pd;
	/* [한국어] PD 보존 — 도메인 콜백에서 PD를 다시 사용. */
	ctx.size = sizeof(ctx);
	/* [한국어] 외부 wrapper size. */
	ctx.user_ctx = &domain->rdma_ctx;
	/* [한국어] framework 콜백 시 다시 돌려받을 user 컨텍스트. */
	ctx.user_ctx_size = domain->rdma_ctx.size;

	rc = spdk_memory_domain_create(&domain->domain, SPDK_DMA_DEVICE_TYPE_RDMA, &ctx,
				       SPDK_RDMA_DMA_DEVICE);
	/* [한국어] SPDK memory domain 생성 — RDMA 타입으로. accel/bdev이 이 도메인을 인식하면
	 * "이 메모리는 HCA가 직접 DMA 가능"으로 판단 → zero-copy 경로 결정.
	 * SPDK_RDMA_DMA_DEVICE는 도메인 식별 문자열. */
	if (rc) {
		SPDK_ERRLOG("Failed to create memory domain\n");
		free(domain);
		pthread_mutex_unlock(&g_memory_domains_lock);
		return NULL;
	}

	domain->pd = pd;
	/* [한국어] entry 키 저장. */
	domain->ref = 1;
	/* [한국어] 첫 ref. */
	TAILQ_INSERT_TAIL(&g_memory_domains, domain, link);
	/* [한국어] 글로벌 풀에 등록 — 다음 caller가 공유. */

	pthread_mutex_unlock(&g_memory_domains_lock);

	return domain->domain;
}

/*
 * [한국어]
 * spdk_rdma_utils_put_memory_domain - 도메인 ref decrement, 0이면 실제 해제
 *
 * @_domain: 해제할 도메인. NULL 허용(no-op).
 * @return: 0 성공, -ENODEV = 매치 entry 없음.
 *
 * 호출 체인:
 *   rdma_provider_verbs_qp_destroy → 이 함수 → spdk_memory_domain_destroy(ref==0 시)
 */
int
spdk_rdma_utils_put_memory_domain(struct spdk_memory_domain *_domain)
{
	struct rdma_utils_memory_domain *domain = NULL;

	if (!_domain) {
		/* [한국어] NULL은 no-op — caller의 정리 코드 단순화. */
		return 0;
	}

	pthread_mutex_lock(&g_memory_domains_lock);

	TAILQ_FOREACH(domain, &g_memory_domains, link) {
		/* [한국어] domain 핸들 매치 entry 검색. */
		if (domain->domain == _domain) {
			break;
		}
	}

	if (!domain) {
		/* [한국어] 매치 entry 없음 — caller가 우리 풀에 없는 핸들을 넘긴 경우. */
		pthread_mutex_unlock(&g_memory_domains_lock);
		return -ENODEV;
	}
	assert(domain->ref > 0);
	/* [한국어] 짝 안 맞는 put 방지. */

	domain->ref--;

	if (domain->ref == 0) {
		/* [한국어] 마지막 ref — 실제 해제. */
		spdk_memory_domain_destroy(domain->domain);
		/* [한국어] SPDK framework에서 도메인 자원 회수. */
		TAILQ_REMOVE(&g_memory_domains, domain, link);
		free(domain);
	}

	pthread_mutex_unlock(&g_memory_domains_lock);

	return 0;
}

/*
 * [한국어]
 * spdk_rdma_cm_id_get_numa_id - cm_id의 local addr → 인터페이스 → NUMA 노드 ID 조회
 *
 * @cm_id: 조회 대상 cm_id(주소 해상도 완료 상태여야 의미 있음).
 * @return: NUMA 노드 ID(0 이상) 또는 SPDK_ENV_NUMA_ID_ANY(미정/실패).
 *
 * 동작:
 *  1) rdma_get_local_addr — cm_id가 바인딩된 sockaddr*.
 *  2) sockaddr → IP 문자열(spdk_net_get_address_string).
 *  3) IP → 인터페이스 이름(spdk_net_get_interface_name, /proc/net/route 또는 ioctl 사용).
 *  4) /sys/class/net/<ifc>/device/numa_node를 spdk_read_sysfs_attribute_uint32로 read.
 *  5) 성공 시 numa_id 반환, 실패 어느 단계든 SPDK_ENV_NUMA_ID_ANY.
 *
 * SPDK는 이 정보를 사용해 thread/buffer pool을 RDMA HCA와 같은 NUMA 노드에 배치 — cross-NUMA
 * 메모리 접근을 줄여 latency/throughput을 향상.
 *
 * 호출 체인:
 *   nvmf_rdma_listener_init / nvme_rdma_qpair_create → 이 함수 → /sys 파싱
 */
int32_t
spdk_rdma_cm_id_get_numa_id(struct rdma_cm_id *cm_id)
{
	struct sockaddr	*sa;
	/* [한국어] librdmacm이 보관 중인 local 주소(IPv4/IPv6 모두 가능). */
	char		addr[64];
	/* [한국어] IP 문자열 버퍼(IPv6 textual max ~46자 + 여유). */
	char		ifc[64];
	/* [한국어] 네트워크 인터페이스 이름 버퍼(예: "ib0", "ens1f0np0"). */
	uint32_t	numa_id;
	/* [한국어] sysfs에서 읽은 NUMA 노드 ID(uint32). 음수(-1)는 sysfs가 문자열로 표시하지만
	 * spdk_read_sysfs_attribute_uint32는 음수면 실패로 처리. */
	int		rc;

	sa = rdma_get_local_addr(cm_id);
	/* [한국어] cm_id가 바인딩된 local sockaddr* 가져오기. 주소 해상도 전이면 NULL 가능. */
	if (sa == NULL) {
		return SPDK_ENV_NUMA_ID_ANY;
		/* [한국어] 주소 미바인딩 — 어느 NUMA로도 결정 못 함 → ANY. */
	}
	rc = spdk_net_get_address_string(sa, addr, sizeof(addr));
	/* [한국어] sockaddr → presentation form IP 문자열. inet_ntop 래퍼. */
	if (rc) {
		return SPDK_ENV_NUMA_ID_ANY;
	}
	rc = spdk_net_get_interface_name(addr, ifc, sizeof(ifc));
	/* [한국어] IP에 바인딩된 NIC 이름 검색 — getifaddrs 또는 /proc/net/route 등 사용. */
	if (rc) {
		return SPDK_ENV_NUMA_ID_ANY;
	}
	rc = spdk_read_sysfs_attribute_uint32(&numa_id,
					      "/sys/class/net/%s/device/numa_node", ifc);
	/* [한국어] sysfs 경로 — 커널이 노출하는 PCI device의 NUMA 노드 ID. printf 포맷으로 ifc 삽입.
	 * 일부 가상 인터페이스는 -1을 보고하는데, 이 경우 spdk_read_sysfs_attribute_uint32가 실패 처리. */
	if (rc || numa_id > INT32_MAX) {
		/* [한국어] read 실패 또는 비정상 큰 값(int32 cast 시 음수 됨) → ANY. */
		return SPDK_ENV_NUMA_ID_ANY;
	}
	return (int32_t)numa_id;
	/* [한국어] 정상 NUMA 노드 ID(보통 0 ~ socket_count-1). */
}
