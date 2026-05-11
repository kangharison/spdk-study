/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] DPDK 버전별 PCI 백엔드 디스패처 (pci_dpdk.c)
 *
 * === 파일의 역할 ===
 * SPDK 의 env_dpdk 레이어는 여러 DPDK 릴리스(22.07, 22.11, 23.x, 24.x, 25.x …)를
 * 동시에 빌드/지원해야 한다. 각 DPDK 버전마다 `struct rte_pci_device`, `struct
 * rte_intr_handle`, 버스 스캔 API의 형태가 미묘하게 달라서 한 소스 파일에서
 * 모두 다루기는 어렵다. 이 파일은 런타임에 `rte_version()` 문자열을 파싱하여
 * 22.07 백엔드(`fn_table_2207`) 또는 22.11 호환 백엔드(`fn_table_2211`)를 골라
 * 전역 함수 포인터 테이블 `g_dpdk_fn_table` 에 바인딩하고, 그 뒤로 SPDK 코드가
 * 호출하는 `dpdk_pci_*` / `dpdk_bus_*` / `dpdk_device_*` 래퍼들이 이 테이블을
 * 통해 적절한 버전 구현으로 디스패치되도록 한다.
 * 즉, 본 파일은 "DPDK 버전 추상화 계층의 진입 디스패처"이며, 백엔드 구현은
 * 형제 파일 pci_dpdk_2207.c / pci_dpdk_2211.c 에 분리되어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택에서의 위치는 다음과 같다:
 *   [NVMe 드라이버 (lib/nvme)] ─ uses → [env_dpdk PCI API (lib/env_dpdk/pci.c)]
 *           │                                  │
 *           │                                  ▼
 *           │                           [본 파일: dpdk_pci_* 래퍼]
 *           │                                  │  g_dpdk_fn_table 포인터 호출
 *           │                                  ▼
 *           │              ┌────────────┴────────────┐
 *           │       [pci_dpdk_2207.c]         [pci_dpdk_2211.c]
 *           │       (DPDK 22.07 ABI)          (DPDK 22.11/23/24/25 호환)
 *           │              │                          │
 *           ▼              ▼                          ▼
 *      [rte_pci_register / rte_intr_* / rte_bus_* (DPDK 라이브러리)]
 *
 * 호출 시점: spdk_env_dpdk_post_init() 같은 EAL 초기화 직후 단계에서
 * dpdk_pci_init()이 호출되어 함수 테이블을 결정하고, 그 후에는 PCI/NVMe
 * probe 경로가 실행될 때마다 본 파일의 thin wrapper 들이 호출된다.
 *
 * 실행 컨텍스트: 호스트 유저스페이스. dpdk_pci_init() 자체는 단일 초기화
 * 스레드에서 1회 호출되며(전역 변수 초기화), 이후 래퍼들은 SPDK reactor
 * 스레드(또는 DPDK 라이브러리 내부 스레드)에서 호출될 수 있다. 함수 포인터
 * 테이블 자체는 init 이후 read-only 로 다뤄지므로 별도 락이 필요 없다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존하는 외부 라이브러리:
 *     · DPDK: rte_version() (DPDK 라이브러리 버전 문자열),
 *       그리고 fn_table_2207 / fn_table_2211 이 사용하는 rte_pci_*/rte_intr_*
 *       /rte_bus_* (실제 호출은 백엔드 .c 파일들이 수행).
 *     · SPDK 로깅: SPDK_ERRLOG / SPDK_NOTICELOG (lib/log).
 * - 의존하는 SPDK 내부 헤더:
 *     · pci_dpdk.h — 함수 포인터 테이블 `struct dpdk_fn_table` 와 외부 노출
 *       선언 (`dpdk_pci_*`, `dpdk_bus_*`, `dpdk_device_*`)을 제공.
 * - 이 파일에 의존하는 모듈:
 *     · lib/env_dpdk/pci.c, env.c — `dpdk_pci_*` API 를 통해 PCI 디바이스
 *       속성을 읽고 인터럽트 efd 를 다룬다. 이 흐름은 다시 lib/nvme/nvme_pcie.c
 *       에서 NVMe 컨트롤러 attach 단계에 사용된다.
 *
 * 데이터 흐름:
 *   rte_version() 문자열 → sscanf → (year, month, minor) → 분기 →
 *   g_dpdk_fn_table = &fn_table_22XX → 이후 모든 dpdk_* 호출이 이 테이블을 경유.
 *
 * === 주요 함수/구조체 요약 ===
 * - dpdk_pci_init() : DPDK 버전을 파싱하여 g_dpdk_fn_table 을 결정. 미지원
 *   버전이면 -EINVAL.
 * - dpdk_pci_device_get_mem_resource / get_name / get_devargs / get_addr /
 *   get_id / get_numa_node / read_config / write_config :
 *   PCI 디바이스 속성 접근(BAR/MMIO 자원 포함)을 위한 1-라인 디스패치 래퍼.
 *   NVMe 드라이버가 BAR0 의 NVMe 레지스터를 매핑할 때 mem_resource 가 사용된다.
 * - dpdk_pci_driver_register : SPDK 드라이버 구조체를 DPDK rte_pci_driver 로
 *   변환하고 등록 (실 구현은 백엔드의 pci_driver_register_2207/2211).
 * - dpdk_pci_device_enable/disable_interrupt, get_interrupt_efd,
 *   create_interrupt_efds, delete_interrupt_efds, get_interrupt_efd_by_index,
 *   interrupt_cap_multi : MSI/MSI-X eventfd 기반 인터럽트 관리 래퍼. SPDK 가
 *   polled-mode 외에 인터럽트 모드를 옵션으로 쓸 때 사용한다.
 * - dpdk_bus_probe / dpdk_bus_scan : DPDK 버스 스캔/probe 수행을 디스패치.
 * - dpdk_device_get_devargs / set_devargs / get_name / scan_allowed :
 *   `struct rte_device` (PCI/AUXILIARY 등 공통 베이스) 단위 속성 디스패치.
 *
 * 전역 상태:
 * - g_dpdk_fn_table : 본 파일의 정수. 모든 dpdk_* 래퍼가 이 포인터의 함수
 *   필드를 호출한다. dpdk_pci_init() 이후로는 변경되지 않으므로 lock-free.
 */

#include <rte_config.h>      /* [한국어] DPDK 빌드 시점 매크로(예: 엔디안, ABI 옵션). rte_version()의 의존성. */
#include <rte_version.h>     /* [한국어] rte_version() 선언과 RTE_VER_* 매크로 — 런타임 버전 분기에 필수. */
#include "pci_dpdk.h"        /* [한국어] struct dpdk_fn_table, dpdk_* 외부 선언, struct spdk_pci_driver 정의 (자매 백엔드 .c 와의 계약). */
#include "spdk/log.h"        /* [한국어] SPDK_ERRLOG / SPDK_NOTICELOG — 미지원 DPDK 발견 시 운용 메시지 출력용. */

/*
 * [한국어]
 * 외부 백엔드 함수 테이블 선언.
 *
 * fn_table_2207, fn_table_2211 은 각각 pci_dpdk_2207.c, pci_dpdk_2211.c 에서
 * 정의된 전역 변수이며, 이 파일의 g_dpdk_fn_table 이 가리킬 후보들이다.
 * extern 으로 선언함으로써 본 파일의 dpdk_pci_init() 가 두 백엔드 중 하나의
 * 주소를 그대로 g_dpdk_fn_table 에 대입할 수 있다.
 * 두 백엔드 모두 동일한 인터페이스(struct dpdk_fn_table)를 채우고 있어
 * SPDK 코드 입장에서는 어느 백엔드가 선택되었는지 구분할 필요가 없다.
 */
extern struct dpdk_fn_table fn_table_2207;
extern struct dpdk_fn_table fn_table_2211;

/*
 * [한국어]
 * g_dpdk_fn_table — 현재 활성 DPDK 백엔드를 가리키는 전역 함수 포인터 테이블.
 *
 * 설정자: dpdk_pci_init()에서 단 한 번 (초기화 단계).
 * 읽는 자: 본 파일의 모든 dpdk_pci_* / dpdk_bus_* / dpdk_device_* 래퍼.
 * 값 범위: NULL (init 이전), &fn_table_2207, &fn_table_2211 중 하나.
 *          init 실패 시 NULL 로 남고 후속 래퍼 호출은 NPE 가 되지만,
 *          dpdk_pci_init() 가 비-0 을 반환하면 SPDK env 초기화 자체가
 *          중단되므로 런타임에는 항상 유효한 포인터로 들어와 있어야 한다.
 * 동기화: init 이후 read-only 로 사용 → 멀티스레드에서 락 없이 읽어도 안전.
 *          (dpdk_pci_init()는 단일 스레드 EAL 초기화 컨텍스트에서 호출됨)
 */
static struct dpdk_fn_table *g_dpdk_fn_table;

/*
 * [한국어]
 * dpdk_pci_init - 런타임 DPDK 버전을 파싱하여 g_dpdk_fn_table 백엔드를 선택.
 *
 * @return: 0 = 성공(g_dpdk_fn_table 설정 완료), -EINVAL = 버전 문자열 파싱
 *          실패 또는 해당 DPDK 버전이 SPDK가 지원하지 않는 경우.
 *          호출자는 비-0 반환 시 SPDK env 초기화를 중단해야 한다.
 *
 * 동기/배경: SPDK 는 빌드 시점이 아니라 "런타임"에 링크된 DPDK 의 실제
 * 버전을 검사한다. 시스템 패키지 매니저가 DPDK 를 업그레이드하더라도
 * SPDK 바이너리가 그대로 동작해야 하며, in-development(예: 26.03) DPDK
 * 도 부분적으로 검증할 수 있어야 하기 때문이다.
 *
 * 동작 단계:
 *   1) rte_version() 결과를 sscanf 로 파싱해 (YY, MM, minor[, suffix])를 추출.
 *   2) suffix 가 있으면 in-development 빌드(26.03 등)로 간주하고 22.11 호환
 *      백엔드를 잠정 사용. NOTICE 로그로 검증용임을 안내.
 *   3) 27.x 이상은 미지원(향후 ABI 변경 가능성)으로 즉시 실패.
 *   4) 22.11 LTS 의 경우 minor>4 면 ABI 변경 가능성 때문에 미지원.
 *   5) 23.x / 24.x / 25.x 의 화이트리스트(특정 minor=0 등)에 한해
 *      22.11 백엔드를 재사용. SPDK 가 검증한 조합만 허용한다.
 *   6) 21.11 미만은 미지원.
 *   7) 그 외(즉, 22.07 또는 비-22.11 22.x)는 22.07 백엔드 사용.
 *
 * 실행 컨텍스트: env 초기화 중 단일 스레드에서 1회만 호출. 재진입 X.
 * 호출 체인:
 *   spdk_env_dpdk_post_init() → ... → dpdk_pci_init() → (성공 시)
 *      이후 dpdk_pci_*/dpdk_bus_* 래퍼들이 g_dpdk_fn_table 을 통해
 *      백엔드 fn_table_2207/2211 의 함수를 호출.
 * 에러 경로: 비-0 반환 시 상위 init 가 실패하고 SPDK 앱은 시작하지 못한다.
 */
int
dpdk_pci_init(void)
{
	uint32_t year;                  /* [한국어] DPDK 버전의 연도(예: 22, 23, 24…) — 메이저 분기 키. */
	uint32_t month;                 /* [한국어] 릴리스 월(03/07/11) — DPDK는 3/7/11월 릴리스 사이클을 가짐. */
	uint32_t minor;                 /* [한국어] 패치 minor 번호(예: 22.11.4 의 4) — ABI 변동 가능성 검사용. */
	char release[32] = {0};         /* [한국어] 버전 뒤 suffix(rc1, -dev 등) 저장. in-development 판별용. /* Max size of DPDK version string */
	int count;                      /* [한국어] sscanf 가 채운 필드 수. 3 또는 4 이어야 정상. */

	/* [한국어] rte_version() 예시: "DPDK 22.11.0" 또는 "DPDK 26.03.0-rc1".
	 * sscanf 포맷 "DPDK %u.%u.%u%s" 로 (YY, MM, minor, [suffix]) 추출. */
	count = sscanf(rte_version(), "DPDK %u.%u.%u%s", &year, &month, &minor, release);
	if (count != 3 && count != 4) {                                       /* [한국어] 정상 형식이면 3(suffix 없음) 또는 4(suffix 있음). 그 외는 인식 불가. */
		SPDK_ERRLOG("Unrecognized DPDK version format '%s'\n", rte_version()); /* [한국어] 운영자 디버깅용 — 어떤 문자열 때문에 실패했는지 그대로 노출. */
		return -EINVAL;                                               /* [한국어] env 초기화 실패로 전파 → SPDK 앱 부팅 실패. */
	}

	/* Add support for DPDK main branch, should be updated after each new release.
	 * Only DPDK in development has additional suffix past minor version.
	 */
	/* [한국어] 위 영문 주석 보강: 안정 릴리스에는 suffix 가 없고, master 브랜치
	 * 빌드는 "-rc1", "-dev" 같은 suffix 를 가짐. SPDK 는 다음 LTS 가 등장하기 전까지
	 * 미리 in-development 버전을 검증할 수 있도록 화이트리스트로 한 점만 열어둔다. */
	if (strlen(release) != 0) {                                  /* [한국어] suffix 가 있으면 in-development 빌드. */
		if (year == 26 && month == 3 && minor == 0) {        /* [한국어] 다음 LTS 후보(26.03)는 22.11 ABI 호환으로 잠정 동작. */
			g_dpdk_fn_table = &fn_table_2211;            /* [한국어] 22.11 백엔드를 in-dev 검증용으로 임시 매핑. */
			SPDK_NOTICELOG("In-development %s is used. There is no support for it in SPDK. "
				       "Enabled only for validation.\n", rte_version()); /* [한국어] 운영자에게 비공식 빌드임을 명확히 알림. */
			return 0;                                    /* [한국어] init 성공 처리 — 후속 init 단계 진행. */
		}
	}

	/* Anything 27.x or higher is not supported. */
	if (year >= 27) {                /* [한국어] 향후 ABI 변동이 예측되므로 27 이상은 차단. */
		goto not_supported;      /* [한국어] 공통 에러 로그 후 -EINVAL 반환. */
	}

	if (year == 22 && month == 11) { /* [한국어] 22.11 LTS — 22.11.0~22.11.4 까지 ABI 동일성이 검증된 범위. */
		if (minor > 4) {
			/* It is possible that LTS minor release changed private ABI, so we
			 * cannot assume fn_table_2211 works for minor releases.  As 22.11
			 * minor releases occur, this will need to be updated to either affirm
			 * no ABI changes for the minor release, or add new header files and
			 * pci_dpdk_xxx.c implementation for the new minor release.
			 */
			/* [한국어] LTS 의 minor 릴리스도 내부 구조체 ABI 가 바뀔 수 있어
			 * 새 minor 가 나오면 SPDK 측에서 검증 후 이 if 의 상한을 올리거나
			 * 별도 pci_dpdk_2211_5.c 를 추가해야 한다. */
			goto not_supported;
		}
		g_dpdk_fn_table = &fn_table_2211;   /* [한국어] 검증된 22.11.x → 22.11 호환 백엔드 사용. */
	} else if (year == 23) {
		/* Only 23.11.0, 23.07.0 and 23.03.0 are supported. */
		/* [한국어] 23.x 시리즈 중 SPDK 가 검증한 .0 릴리스 3종만 허용. */
		if ((month != 11 || minor != 0) &&
		    (month != 7 || minor != 0) &&
		    (month != 3 || minor != 0)) {
			goto not_supported;             /* [한국어] 화이트리스트 외(예: 23.05, 23.11.1)는 차단. */
		}
		/* There were no changes between 22.11 and 23.11, so use the 22.11 implementation. */
		/* [한국어] 22.11 ABI와 동일성이 확인되어 동일 백엔드 재사용 — 코드 중복 회피. */
		g_dpdk_fn_table = &fn_table_2211;
	} else if (year == 24) {
		/* Only 24.11.0-2, 24.07.0 and 24.03.0 are supported. */
		/* [한국어] 24.x 화이트리스트. 24.11 은 minor 0~2 허용, 나머지는 .0 만 허용. */
		if ((month != 11 || minor > 2) &&
		    (month != 7 || minor != 0) &&
		    (month != 3 || minor != 0)) {
			goto not_supported;
		}
		/* There were no changes between 22.11 and 24.*, so use the 22.11 implementation. */
		/* [한국어] 22.11~24.x 범위는 SPDK가 보는 PCI/Bus ABI 동일 → 22.11 백엔드 재사용. */
		g_dpdk_fn_table = &fn_table_2211;
	} else if (year == 25) {
		/* Only 25.03.0, 25.07.0 and 25.11.0 are supported. */
		/* [한국어] 25.x 시리즈도 동일하게 .0 릴리스만 허용. */
		if ((month != 11 || minor != 0) &&
		    (month != 7 || minor != 0) &&
		    (month != 3 || minor != 0)) {
			goto not_supported;
		}
		/* There were no changes between 22.11 and 25.*, so use the 22.11 implementation. */
		g_dpdk_fn_table = &fn_table_2211;   /* [한국어] 22.11 ABI 호환 → 동일 백엔드 사용. */
	} else if (year < 21 || (year == 21 && month < 11)) {
		/* [한국어] 21.11 미만(21.05, 20.11 등)은 SPDK가 더 이상 검증하지 않음. */
		goto not_supported;
	} else {
		/* Everything else we use the 22.07 implementation. */
		/* [한국어] 위 모든 분기에 해당하지 않는 22.07 / 22.03 / 21.11 등은
		 * 22.07 백엔드(rte_dev.h 의 옛 ABI)를 사용. */
		g_dpdk_fn_table = &fn_table_2207;
	}
	return 0;       /* [한국어] 정상적으로 백엔드 결정 완료. */

not_supported:
	/* [한국어] 공통 실패 라벨 — 분기에서 미지원 버전을 만났을 때 한 곳에서 로깅 후 -EINVAL. */
	SPDK_ERRLOG("DPDK version %02d.%02d.%d is not supported.\n", year, month, minor);
	return -EINVAL;
}

/*
 * [한국어]
 * dpdk_pci_device_get_mem_resource - PCI 디바이스의 BAR(Base Address Register)
 * 에 대응하는 메모리 자원(MMIO 매핑) 디스크립터를 가져온다.
 *
 * @dev: DPDK 가 enumerate 한 PCI 디바이스 핸들. probe 콜백에서 전달됨.
 * @bar: 0~PCI_MAX_RESOURCE-1 범위의 BAR 인덱스. NVMe 컨트롤러는 보통 BAR0
 *       을 NVMe 레지스터(NVMe Spec 3.x — Controller Registers)로 사용.
 * @return: 해당 BAR 의 `struct rte_mem_resource` 포인터. 이 안에 phys_addr,
 *          len, addr(가상 매핑) 가 들어 있다. bar 가 범위를 벗어나면 NULL.
 *
 * 호출 체인:
 *   nvme_pcie_ctrlr_construct() → spdk_pci_device_map_bar() →
 *      [본 함수] → g_dpdk_fn_table->pci_device_get_mem_resource()
 *      → pci_device_get_mem_resource_2207/2211 (백엔드)
 *      → &dev->mem_resource[bar] (DPDK 내부 배열 직접 액세스)
 *
 * 실행 컨텍스트: PCI probe / NVMe attach 단계 — 보통 SPDK 메인 reactor.
 */
struct rte_mem_resource *
dpdk_pci_device_get_mem_resource(struct rte_pci_device *dev, uint32_t bar)
{
	/* [한국어] 함수 포인터 디스패치 — 22.07 또는 22.11 백엔드 구현으로 위임. */
	return g_dpdk_fn_table->pci_device_get_mem_resource(dev, bar);
}

/*
 * [한국어]
 * dpdk_pci_device_get_name - DPDK 가 디바이스에 부여한 문자열 이름을 반환.
 *
 * @rte_dev: DPDK PCI 디바이스 핸들.
 * @return: "0000:81:00.0" 같은 BDF 형식 문자열(백엔드가 dev->name 또는
 *          rte_pci_device_name() 등을 호출해 반환). 호출자는 소유권을
 *          갖지 않으며 dev 의 라이프타임 동안만 유효.
 *
 * 호출 체인:
 *   spdk_pci_addr_parse / 로깅 → [본 함수] → 백엔드 pci_device_get_name_22XX.
 * 실행 컨텍스트: SPDK 초기화 / probe / RPC 응답 등 다양한 컨텍스트.
 */
const char *
dpdk_pci_device_get_name(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_get_name(rte_dev);   /* [한국어] 백엔드로 디스패치. */
}

/*
 * [한국어]
 * dpdk_pci_device_get_devargs - PCI 디바이스에 연결된 devargs(EAL 인자에서
 * 파싱된 디바이스별 옵션 문자열) 구조체를 반환.
 *
 * @rte_dev: DPDK PCI 디바이스.
 * @return: rte_dev->device.devargs 포인터(없으면 NULL).
 *
 * 호출 체인: SPDK 가 -a/-b EAL 옵션으로 전달된 디바이스 옵션을 읽을 때 사용.
 */
struct rte_devargs *
dpdk_pci_device_get_devargs(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_get_devargs(rte_dev);  /* [한국어] 디스패치 — 22.07/22.11 모두 동일하게 device.devargs 반환. */
}

/*
 * [한국어]
 * dpdk_pci_device_get_addr - PCI BDF(Bus/Device/Function) 주소 구조체 반환.
 *
 * @rte_dev: DPDK PCI 디바이스.
 * @return: rte_pci_addr (domain/bus/devid/function 4-tuple) 포인터.
 *
 * SPDK 측 spdk_pci_addr 와 BDF 변환에 사용된다.
 */
struct rte_pci_addr *
dpdk_pci_device_get_addr(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_get_addr(rte_dev);  /* [한국어] 백엔드의 &dev->addr 반환. */
}

/*
 * [한국어]
 * dpdk_pci_device_get_id - vendor_id/device_id/class_id 등을 담은 식별자
 * 구조체를 반환. NVMe 컨트롤러 매칭(class==Mass Storage / NVMe)에 사용.
 *
 * @rte_dev: DPDK PCI 디바이스.
 * @return: rte_pci_id 포인터. 라이프타임은 rte_dev 와 동일.
 */
struct rte_pci_id *
dpdk_pci_device_get_id(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_get_id(rte_dev);   /* [한국어] 백엔드 디스패치. */
}

/*
 * [한국어]
 * dpdk_pci_device_get_numa_node - 디바이스가 위치한 NUMA 노드 번호 반환.
 *
 * @_dev: DPDK PCI 디바이스.
 * @return: NUMA 노드 번호 (-1 = unknown). SPDK 는 hugepage 메모리 풀을
 *          디바이스 NUMA 노드와 같게 두어 DMA 지연을 줄인다.
 *
 * 호출 체인: bdev_nvme attach → spdk_nvme_ctrlr_alloc_io_qpair 시 NUMA-aware
 * 메모리 풀 선택 등.
 */
int
dpdk_pci_device_get_numa_node(struct rte_pci_device *_dev)
{
	return g_dpdk_fn_table->pci_device_get_numa_node(_dev);  /* [한국어] 백엔드의 _dev->device.numa_node 반환. */
}

/*
 * [한국어]
 * dpdk_pci_device_read_config - PCI configuration space (256B legacy / 4KB
 * extended) 의 임의 오프셋에서 len 바이트 읽기.
 *
 * @dev: PCI 디바이스 핸들.
 * @value: 읽은 데이터를 저장할 호출자 버퍼 (len 바이트 이상).
 * @len: 1/2/4 바이트 단위 (PCI 스펙 상 정렬된 액세스 권장).
 * @offset: PCI config space 내 바이트 오프셋 (예: 0x00=Vendor, 0x04=Status,
 *          MSI/MSI-X capability 위치 등).
 * @return: 0 = 정확히 len 바이트 읽음, -1 = 실패.
 *
 * 호출 체인:
 *   spdk_pci_device_cfg_read* (env_dpdk/pci.c) → [본 함수] →
 *      백엔드 → rte_pci_read_config() → DPDK PCI bus driver →
 *      sysfs config 노드 또는 VFIO/igb_uio ioctl.
 * 실행 컨텍스트: 유저스페이스 (PCI 설정 공간은 sysfs/VFIO 경유로 접근).
 */
int
dpdk_pci_device_read_config(struct rte_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	return g_dpdk_fn_table->pci_device_read_config(dev, value, len, offset);   /* [한국어] 디스패치 — 백엔드가 0/-1 정규화까지 처리. */
}

/*
 * [한국어]
 * dpdk_pci_device_write_config - PCI configuration space 에 len 바이트 쓰기.
 *
 * @dev: PCI 디바이스 핸들.
 * @value: 호출자가 준비한 데이터 버퍼.
 * @len: 1/2/4 바이트.
 * @offset: 쓸 PCI config 오프셋 (예: Command Register 0x04 의 BME bit 활성화).
 * @return: 0 = 성공, -1 = 실패.
 *
 * 사용 예: SPDK 가 PCI Bus Master Enable, Memory Space Enable 비트를 켜는
 * 등 디바이스 활성화 제어. 백엔드의 #ifdef __FreeBSD__ 분기는 BSD DPDK 가
 * 0/-1 만 반환하는 ABI 차이를 흡수하기 위한 것.
 */
int
dpdk_pci_device_write_config(struct rte_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	return g_dpdk_fn_table->pci_device_write_config(dev, value, len, offset);  /* [한국어] 백엔드 디스패치. */
}

/*
 * [한국어]
 * dpdk_pci_driver_register - SPDK PCI 드라이버를 DPDK 의 rte_pci_driver 로
 * 변환하여 등록.
 *
 * @driver: SPDK 드라이버 (id_table, name, drv_flags, cb_fn 등 포함).
 *          driver_buf[0..255] 영역이 rte_pci_driver 의 임베디드 저장소로 쓰인다.
 * @probe_fn: DPDK probe 콜백 — 새 디바이스 발견 시 호출.
 * @remove_fn: DPDK remove 콜백 — hot-remove 시 호출.
 * @return: 0 = 성공, -ENOMEM = id_table/name 메모리 할당 실패.
 *
 * 호출 체인:
 *   spdk_pci_driver_register() (env_dpdk/pci.c) → [본 함수] →
 *      백엔드 pci_driver_register_22XX → rte_pci_register().
 *
 * 동작 흐름(백엔드):
 *   1) SPDK id_table 의 엔트리 수 카운트.
 *   2) 같은 크기의 rte_pci_id 배열 calloc.
 *   3) class_id/vendor/device/subvendor/subdevice 필드를 1:1 복사.
 *   4) "spdk_<name>" 형식의 드라이버 이름 calloc.
 *   5) drv_flags 비트(SPDK_PCI_DRIVER_NEED_MAPPING 등)를 RTE_PCI_DRV_* 로 매핑.
 *   6) probe/remove 콜백 설치 후 rte_pci_register() 호출.
 *
 * 실행 컨텍스트: 모듈 초기화(SPDK_PCI_DRIVER_REGISTER 매크로 경유 또는 명시적
 * 등록 시점). 등록 후 콜백은 DPDK probe 스레드 컨텍스트에서 호출됨.
 */
int
dpdk_pci_driver_register(struct spdk_pci_driver *driver,
			 int (*probe_fn)(struct rte_pci_driver *driver, struct rte_pci_device *device),
			 int (*remove_fn)(struct rte_pci_device *device))

{
	return g_dpdk_fn_table->pci_driver_register(driver, probe_fn, remove_fn);  /* [한국어] 두 백엔드 모두 동일 본체 — 단지 헤더 경로만 다름. */
}

/*
 * [한국어]
 * dpdk_pci_device_enable_interrupt - 디바이스의 MSI/MSI-X 인터럽트 활성화.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @return: 0 = 성공, 음수 = 실패.
 *
 * SPDK 의 polled-mode 가 아닌, 인터럽트 모드 옵션(예: 큰 idle 시간을 가진
 * 워크로드)을 활성화할 때 사용. 내부적으로 백엔드는 rte_intr_enable() 을
 * 호출하여 VFIO/uio 의 MSI/MSI-X 매핑을 활성화한다.
 *
 * 실행 컨텍스트: 디바이스 attach 후 1회. 이후 efd 는 epoll 등에 등록.
 */
int
dpdk_pci_device_enable_interrupt(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_enable_interrupt(rte_dev);   /* [한국어] 디스패치 — rte_intr_enable() 까지 도달. */
}

/*
 * [한국어]
 * dpdk_pci_device_disable_interrupt - MSI/MSI-X 인터럽트 비활성화.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @return: 0 = 성공, 음수 = 실패.
 *
 * detach 또는 polled-mode 로 전환할 때 호출. 백엔드는 rte_intr_disable().
 */
int
dpdk_pci_device_disable_interrupt(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_disable_interrupt(rte_dev);   /* [한국어] 백엔드 디스패치. */
}

/*
 * [한국어]
 * dpdk_pci_device_get_interrupt_efd - 디바이스의 인터럽트 eventfd(파일
 * 디스크립터)를 반환. SPDK 가 epoll/poller 에 등록하여 이벤트 기반 처리에
 * 활용.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @return: 비음수 fd, 음수 = 실패.
 *
 * 백엔드: rte_intr_fd_get(rte_dev->intr_handle).
 */
int
dpdk_pci_device_get_interrupt_efd(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_get_interrupt_efd(rte_dev);   /* [한국어] 디스패치. */
}

/*
 * [한국어]
 * dpdk_pci_device_create_interrupt_efds - MSI-X 의 다중 벡터에 대해 count 개의
 * eventfd 를 생성.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @count: 생성할 efd 개수 (보통 큐페어 수).
 * @return: 0 또는 음수 errno.
 *
 * 백엔드: rte_intr_efd_enable(intr_handle, count). SPDK가 NVMe IO 큐 별로
 * 별도의 eventfd 를 epoll 등록하여 멀티 큐 인터럽트 처리를 가능하게 한다.
 */
int
dpdk_pci_device_create_interrupt_efds(struct rte_pci_device *rte_dev, uint32_t count)
{
	return g_dpdk_fn_table->pci_device_create_interrupt_efds(rte_dev, count);   /* [한국어] 디스패치. */
}

/*
 * [한국어]
 * dpdk_pci_device_delete_interrupt_efds - create 의 짝. 모든 eventfd 해제.
 *
 * @rte_dev: PCI 디바이스 핸들.
 *
 * 호출 컨텍스트: 디바이스 detach 단계. 반환값 없음 — 실패해도 진행.
 */
void
dpdk_pci_device_delete_interrupt_efds(struct rte_pci_device *rte_dev)
{
	g_dpdk_fn_table->pci_device_delete_interrupt_efds(rte_dev);   /* [한국어] 디스패치 (void 반환). */
}

/*
 * [한국어]
 * dpdk_pci_device_get_interrupt_efd_by_index - 큐페어 인덱스에 해당하는
 * eventfd 를 반환.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @index: create_interrupt_efds 로 만든 efd 배열의 인덱스 (0부터).
 * @return: 비음수 fd / 음수 errno.
 */
int
dpdk_pci_device_get_interrupt_efd_by_index(struct rte_pci_device *rte_dev, uint32_t index)
{
	return g_dpdk_fn_table->pci_device_get_interrupt_efd_by_index(rte_dev, index);  /* [한국어] 디스패치. */
}

/*
 * [한국어]
 * dpdk_pci_device_interrupt_cap_multi - 디바이스가 다중 인터럽트(MSI-X 다중
 * 벡터)를 지원하는지 조회.
 *
 * @rte_dev: PCI 디바이스 핸들.
 * @return: 0 = 비지원, 1 = 지원.
 *
 * 호출 체인: SPDK가 인터럽트 모드를 켤 때, MSI-X 다중 벡터 가능 여부를 보고
 * efd 개수를 결정하는 데 활용.
 */
int
dpdk_pci_device_interrupt_cap_multi(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_interrupt_cap_multi(rte_dev);   /* [한국어] rte_intr_cap_multiple() 까지 디스패치. */
}

/*
 * [한국어]
 * dpdk_bus_probe - DPDK 의 모든 등록된 버스(PCI/VDEV/AUXILIARY 등)에 대해
 * probe 를 트리거. 등록된 드라이버의 probe 콜백이 매칭된 디바이스마다 호출됨.
 *
 * @return: 0 = 성공, 음수 = 실패.
 *
 * 호출 체인:
 *   spdk_env_dpdk_post_init() → ... → [본 함수] → 백엔드 → rte_bus_probe().
 * 실행 컨텍스트: 환경 초기화 단일 스레드.
 */
int
dpdk_bus_probe(void)
{
	return g_dpdk_fn_table->bus_probe();   /* [한국어] 디스패치 — 백엔드가 rte_bus_probe() 호출. */
}

/*
 * [한국어]
 * dpdk_bus_scan - DPDK 의 모든 버스에 대해 디바이스 스캔 수행.
 * 시스템에 존재하는 디바이스 목록을 채우지만, 드라이버 매칭은 probe 단계에서.
 *
 * 실행 컨텍스트: 환경 초기화 단일 스레드.
 */
void
dpdk_bus_scan(void)
{
	g_dpdk_fn_table->bus_scan();   /* [한국어] rte_bus_scan() 디스패치. */
}

/*
 * [한국어]
 * dpdk_device_get_devargs - PCI/AUX 등 공통 베이스 `struct rte_device` 의
 * devargs 포인터 반환.
 *
 * @dev: 공통 디바이스 베이스.
 * @return: dev->devargs 또는 NULL.
 *
 * dpdk_pci_device_get_devargs() 와 다른 점: 이쪽은 PCI 가 아닌 다른 버스
 * 디바이스(VDEV 등) 까지 대상으로 한다.
 */
struct rte_devargs *
dpdk_device_get_devargs(struct rte_device *dev)
{
	return g_dpdk_fn_table->device_get_devargs(dev);   /* [한국어] 디스패치. */
}

/*
 * [한국어]
 * dpdk_device_set_devargs - 공통 베이스 디바이스에 devargs 를 설치.
 *
 * @dev: 대상 디바이스.
 * @devargs: 설치할 devargs (소유권은 DPDK 측이 관리).
 *
 * SPDK 가 동적으로 디바이스를 등록할 때 옵션 파라미터를 주입하기 위해 사용.
 */
void
dpdk_device_set_devargs(struct rte_device *dev, struct rte_devargs *devargs)
{
	g_dpdk_fn_table->device_set_devargs(dev, devargs);   /* [한국어] 디스패치. */
}

/*
 * [한국어]
 * dpdk_device_get_name - 공통 베이스 디바이스의 이름 반환.
 *
 * @dev: 대상 디바이스.
 * @return: dev->name 문자열 (라이프타임은 dev 와 동일).
 */
const char *
dpdk_device_get_name(struct rte_device *dev)
{
	return g_dpdk_fn_table->device_get_name(dev);   /* [한국어] 디스패치. */
}

/*
 * [한국어]
 * dpdk_device_scan_allowed - 디바이스가 속한 버스의 스캔 모드가 ALLOWLIST
 * 인지 확인. SPDK가 -a 옵션으로 화이트리스트 모드를 사용하는지를 판별하는
 * 데에 활용.
 *
 * @dev: 대상 디바이스.
 * @return: true = 버스가 ALLOWLIST 모드 (특정 디바이스만 명시적 허용),
 *          false = BLOCKLIST 모드(기본 허용, 일부만 차단).
 *
 * 백엔드 구현: dev->bus->conf.scan_mode == RTE_BUS_SCAN_ALLOWLIST.
 */
bool
dpdk_device_scan_allowed(struct rte_device *dev)
{
	return g_dpdk_fn_table->device_scan_allowed(dev);   /* [한국어] 디스패치. */
}
