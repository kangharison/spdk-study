/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] NVMe 디바이스별 비표준 동작 보정 테이블 및 PCIe ID 매칭 로직 (nvme_quirks.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK NVMe 드라이버가 시중의 다양한 NVMe SSD/컨트롤러를 다룰 때 마주치는
 * "스펙 위반·해석 차이·하드웨어 버그" 같은 비표준 동작을 사전에 인지하고 우회하기 위한
 * **벤더/디바이스별 quirk(특이 동작) 테이블**과 그 테이블을 PCI ID로 룩업하는 함수를
 * 제공한다. 핵심 자료구조는 `static const nvme_quirks[]` 배열로, 각 행은 (PCI class,
 * vendor, device, subvendor, subdevice) 5튜플과 그에 대응하는 quirk 비트마스크
 * (`NVME_QUIRK_*` / `NVME_INTEL_QUIRK_*`)를 묶는다. 룩업이 성공하면 64비트 quirk
 * 플래그가 반환되어 컨트롤러 초기화/I/O 경로 곳곳에서 분기에 활용된다 (예:
 * `NVME_QUIRK_DELAY_BEFORE_INIT`은 초기화 직전 강제 슬립, `NVME_QUIRK_MINIMUM_ADMIN_QUEUE_SIZE`는
 * 최소 admin 큐 깊이 강제, `NVME_QUIRK_NOT_USE_SGL`은 SGL 대신 PRP 강제).
 * SPDK_PCI_ANY_ID(0xFFFF) 와일드카드를 통해 "벤더 전체" 또는 "특정 벤더의 특정 device 만"
 * 같은 정밀도 조절이 가능하도록 매칭 함수가 만들어져 있다.
 *
 * **본 파일은 코드 수정 없이 한국어 주석만 추가/보강된 학습 사본**이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 호스트 NVMe 컨트롤러 초기화 흐름의 매우 이른 단계에서 호출된다. PCIe 트랜스포트
 * 드라이버(`lib/nvme/nvme_pcie.c`)가 새 컨트롤러를 attach 하면서 `struct spdk_pci_id`를
 * 채우면, `lib/nvme/nvme_ctrlr.c`의 컨트롤러 생성 경로(예: `nvme_ctrlr_construct` 직후)와
 * `lib/nvme/nvme_pcie.c`의 PCIe 어댑터 초기화 경로에서 `nvme_get_quirks(&pci_id)`를 호출해
 * `ctrlr->quirks` 필드를 채운다. 이후 컨트롤러 라이프사이클의 모든 분기점
 * (큐 생성, Identify 핸들링, shutdown, log page 조회 등)에서 이 비트마스크를 참조한다.
 * 호출 체인:
 *   nvme_pcie_ctrlr_construct (lib/nvme/nvme_pcie.c)
 *     → nvme_ctrlr_construct (lib/nvme/nvme_ctrlr.c)
 *       → nvme_get_quirks (이 파일) → ctrlr->quirks 저장
 *         → 이후 nvme_ctrlr_process_init 단계마다 if (quirks & NVME_QUIRK_*) 분기
 *
 * 실행 컨텍스트: 컨트롤러 attach 시점에 호출자(보통 메인 또는 init 스레드)에서 동기적으로
 * 1회 실행. 룩업 결과(64비트 정수)는 컨트롤러 객체에 캐싱되어 이후로는 추가 룩업 없이 사용.
 * 이 파일에 락이 필요 없는 이유는 (1) `nvme_quirks[]`가 `static const`로 read-only이고,
 * (2) `nvme_get_quirks()`가 순수 함수(부수 효과 없음, 디버그 로그 제외)이기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 *  - **include/spdk/pci_ids.h** — `SPDK_PCI_VID_INTEL` 등 16비트 vendor ID 상수와
 *    `SPDK_PCI_ANY_ID`(0xFFFF), `SPDK_PCI_CLASS_NVME`(0x010802) 같은 PCI 클래스 코드를
 *    제공. 이 파일의 테이블은 그 매크로들을 5튜플로 조합해 디바이스를 식별한다.
 *  - **include/spdk/env.h** — `struct spdk_pci_id` (5필드: class_id, vendor_id, device_id,
 *    subvendor_id, subdevice_id) 정의. PCIe configuration space에서 추출된 식별자.
 *  - **lib/nvme/nvme_internal.h** — `NVME_INTEL_QUIRK_READ_LATENCY`(0x1) 부터
 *    `NVME_QUIRK_MSIX_VECTOR_COUNT`(0x40000) 까지의 비트마스크 정의(line 131~244).
 *    각 비트의 의미 주석이 그 헤더에 기술되어 있다.
 *  - **lib/nvme/nvme_pcie.c / nvme_ctrlr.c** — `nvme_get_quirks()`를 호출하는 유일한
 *    상위 모듈. 반환값은 `struct spdk_nvme_ctrlr::quirks` (64비트) 필드에 저장.
 *  - **SPDK_DEBUGLOG (include/spdk/log.h)** — `nvme` 카테고리로 매칭/플래그 디버그 로그
 *    출력. `--logflag nvme` 옵션으로 런타임 활성화.
 *
 * === 주요 함수/구조체 요약 ===
 *  - `struct nvme_quirk` — (PCI ID 패턴, quirk 비트마스크) 쌍을 담는 테이블 행.
 *  - `nvme_quirks[]` — sentinel(전체 0)으로 끝나는 정적 quirk 테이블. 새 SSD 추가 시
 *    이 배열 끝에 행을 한 줄 추가하면 됨.
 *  - `pci_id_match()` — 5튜플 비교 헬퍼. `SPDK_PCI_ANY_ID`/`SPDK_PCI_CLASS_ANY_ID`는
 *    와일드카드로 처리, 나머지는 정확히 일치해야 함.
 *  - `nvme_get_quirks()` — 외부 진입점. PCI ID로 테이블을 선형 탐색하여 첫 매칭의
 *    flags를 반환. 미매칭 시 0 반환(quirk 없음).
 *
 * === Quirk 플래그 의미 요약 (nvme_internal.h 참조) ===
 *  - NVME_INTEL_QUIRK_READ_LATENCY/WRITE_LATENCY: Intel 전용 latency log page 사용
 *  - NVME_INTEL_QUIRK_STRIPING: Intel 디바이스의 stripe 경계 정렬 필요
 *  - NVME_QUIRK_DELAY_BEFORE_CHK_RDY: CSTS.RDY 검사 전 지연 (전원 안정화 대기)
 *  - NVME_QUIRK_DELAY_AFTER_QUEUE_ALLOC: 큐 할당 후 지연 (VirtualBox 에뮬레이션 한계)
 *  - NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE: Deallocate(TRIM) 후 0 읽힘 보장 (Intel 특성)
 *  - NVME_QUIRK_IDENTIFY_CNS / OCSSD: OpenChannel SSD 식별
 *  - NVME_QUIRK_DELAY_BEFORE_INIT: 디바이스 attach 후 init 전 강제 sleep (워크어라운드)
 *  - NVME_QUIRK_MINIMUM_IO_QUEUE_SIZE: I/O 큐 최소 크기 강제 (펌웨어 버그 회피)
 *  - NVME_QUIRK_MAXIMUM_PCI_ACCESS_WIDTH: PCIe MMIO 접근 폭 제한 (32비트 강제 등)
 *  - NVME_QUIRK_OACS_SECURITY: OACS의 Security 비트 무시
 *  - NVME_QUIRK_NO_SGL_FOR_DSM: DSM 명령에서 SGL 비활성 (PRP 강제)
 *  - NVME_QUIRK_MDTS_EXCLUDE_MD: MDTS 계산 시 metadata 길이 제외
 *  - NVME_QUIRK_NOT_USE_SGL: SGL 전체 비활성 (Huawei 호환)
 *  - NVME_QUIRK_MINIMUM_ADMIN_QUEUE_SIZE: admin 큐 최소 크기 강제 (Microsoft Azure)
 *  - NVME_QUIRK_MSIX_VECTOR_COUNT: MSI-X 벡터 수 보정 (Micron 워크어라운드)
 *  - NVME_QUIRK_SHST_COMPLETE: Shutdown Status가 항상 COMPLETE인 것처럼 동작 (VMware)
 *  - NVME_INTEL_QUIRK_NO_LOG_PAGES: Intel-specific log page 미지원
 */

/* [한국어] nvme_internal.h: SPDK NVMe 드라이버 내부 헤더 — NVME_QUIRK_* 매크로,
 * struct spdk_nvme_ctrlr 정의, NVMe 트랜스포트 vtable 등을 모두 모아 두는 단일 진입점.
 * 이 quirk 테이블은 외부 사용자에게 노출되지 않으므로 public 헤더 대신 internal만 include. */
#include "nvme_internal.h"

/*
 * [한국어]
 * struct nvme_quirk - quirk 테이블의 한 행을 표현하는 내부 구조체.
 *
 * 이 파일 안에서만 사용되며, `nvme_quirks[]` 정적 배열의 element 타입이다.
 * `id`로 매칭한 뒤 매칭 성공 시 `flags`를 컨트롤러로 반환하는 단순한 (key, value) 쌍.
 */
struct nvme_quirk {
	struct spdk_pci_id	id;
	/* [한국어] 매칭 키. 5튜플 (class_id, vendor_id, device_id, subvendor_id, subdevice_id).
	 * 설정자: 컴파일 타임 상수로만 채워짐 (테이블 정의부).
	 * 읽는 자: pci_id_match()가 디바이스의 실제 PCI ID와 비교.
	 * 값 범위: 16비트(서브)벤더/디바이스 ID 또는 SPDK_PCI_ANY_ID(0xFFFF) 와일드카드.
	 *           class_id는 24비트(0x010802 = NVMe) 또는 SPDK_PCI_CLASS_ANY_ID(0xFFFFFF).
	 * 동기화: `static const`이므로 read-only — 락 불필요. */

	uint64_t		flags;
	/* [한국어] 매칭 성공 시 적용할 quirk 비트마스크 (NVME_QUIRK_* / NVME_INTEL_QUIRK_*).
	 * 설정자: 테이블 정의부에서 OR 조합으로 설정.
	 * 읽는 자: nvme_get_quirks() 반환 → ctrlr->quirks에 저장 → 컨트롤러 라이프사이클 분기.
	 * 값 범위: 0 (no quirk, sentinel 행에서만) ~ 19개 비트의 임의 OR 조합.
	 *           현재 사용되는 최상위 비트는 0x40000 (NVME_QUIRK_MSIX_VECTOR_COUNT).
	 * 동기화: read-only — 컨트롤러별로 1회 복사 후 사용. */
};

/* [한국어] 정적 quirk 테이블 — 컴파일 타임 상수.
 * 각 행은 {PCI 5튜플, quirk 비트마스크} 쌍. 마지막 sentinel 행({0,...,0}, 0)에서
 * vendor_id == 0 검사로 룩업 루프가 종료된다. 새 모델 추가 시 sentinel 위에 한 줄을
 * 추가하면 된다. 순서는 무관하나 와일드카드(SPDK_PCI_ANY_ID) 행이 먼저 매치하므로
 * 더 좁은 패턴을 위에 두는 편이 안전. (현재 테이블은 동일 vendor 안에서 중첩이 없음.) */
static const struct nvme_quirk nvme_quirks[] = {
	/* [한국어] Intel 0x0953 — Intel SSD DC P3500/P3600/P3700 (Fultondale) 패밀리.
	 * READ/WRITE_LATENCY: 벤더 전용 latency log page 활성. STRIPING: stripe 정렬 IO 우대.
	 * READ_ZERO_AFTER_DEALLOCATE: TRIM 후 0 보장 (DZ bit 비신뢰 → quirk 강제).
	 * DELAY_BEFORE_INIT: 펌웨어 안정화 대기. MINIMUM_IO_QUEUE_SIZE: 너무 작은 큐 거부. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_INTEL_QUIRK_READ_LATENCY |
		NVME_INTEL_QUIRK_WRITE_LATENCY |
		NVME_INTEL_QUIRK_STRIPING |
		NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE |
		NVME_QUIRK_DELAY_BEFORE_INIT |
		NVME_QUIRK_MINIMUM_IO_QUEUE_SIZE
	},
	/* [한국어] Intel 0x0A53 — DC P3520/P4500/P4600 후속 모델. 위와 동일한 quirk 세트. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_INTEL, 0x0A53, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_INTEL_QUIRK_READ_LATENCY |
		NVME_INTEL_QUIRK_WRITE_LATENCY |
		NVME_INTEL_QUIRK_STRIPING |
		NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE |
		NVME_QUIRK_DELAY_BEFORE_INIT |
		NVME_QUIRK_MINIMUM_IO_QUEUE_SIZE
	},
	/* [한국어] Intel 0x0A54 — P4500/P4600 변종. 동일 quirk 세트. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_INTEL, 0x0A54, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_INTEL_QUIRK_READ_LATENCY |
		NVME_INTEL_QUIRK_WRITE_LATENCY |
		NVME_INTEL_QUIRK_STRIPING |
		NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE |
		NVME_QUIRK_DELAY_BEFORE_INIT |
		NVME_QUIRK_MINIMUM_IO_QUEUE_SIZE
	},
	/* [한국어] Intel 0x0A55 — DELAY_BEFORE_INIT 만 빠진 변종(펌웨어 개선분). */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_INTEL, 0x0A55, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_INTEL_QUIRK_READ_LATENCY |
		NVME_INTEL_QUIRK_WRITE_LATENCY |
		NVME_INTEL_QUIRK_STRIPING |
		NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE |
		NVME_QUIRK_MINIMUM_IO_QUEUE_SIZE
	},
	/* [한국어] Intel 0x0B60 — DC P5510/P5610. NO_SGL_FOR_DSM 추가: Dataset Mgmt(TRIM)에
	 * SGL을 쓰면 펌웨어가 잘못 처리하므로 PRP로 강제. READ_ZERO_AFTER_DEALLOCATE 제거. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_INTEL, 0x0B60, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_INTEL_QUIRK_READ_LATENCY |
		NVME_INTEL_QUIRK_WRITE_LATENCY |
		NVME_INTEL_QUIRK_STRIPING |
		NVME_QUIRK_MINIMUM_IO_QUEUE_SIZE |
		NVME_QUIRK_NO_SGL_FOR_DSM
	},
	/* [한국어] Memblaze PBlaze 시리즈 — 컨트롤러 reset 후 CSTS.RDY가 천천히 1로 가므로
	 * RDY 폴링 전에 sleep 필요. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_MEMBLAZE, 0x0540, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_DELAY_BEFORE_CHK_RDY
	},
	/* [한국어] Samsung 0xa821 — PM173X/983/1725 등. CSTS.RDY 지연 동일 이슈. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_SAMSUNG, 0xa821, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_DELAY_BEFORE_CHK_RDY
	},
	/* [한국어] Samsung 0xa822 — 동일 패밀리 변종. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_SAMSUNG, 0xa822, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_DELAY_BEFORE_CHK_RDY
	},
	/* [한국어] Samsung 0xa826 — 후속 모델. RDY 지연 동일. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_SAMSUNG, 0xa826, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_DELAY_BEFORE_CHK_RDY
	},
	/* [한국어] VirtualBox 가상 NVMe — 큐 할당 직후 doorbell write 시 race가 있어
	 * DELAY_AFTER_QUEUE_ALLOC 적용. 가상화 환경 특유의 워크어라운드. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_VIRTUALBOX, 0x4e56, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_DELAY_AFTER_QUEUE_ALLOC
	},
	/* [한국어] Intel 0x5845 — 가상화/SR-IOV용 NVMe controller(QEMU 표준 NVMe 등에 사용).
	 * NO_LOG_PAGES: Intel-specific log page 미지원.
	 * MAXIMUM_PCI_ACCESS_WIDTH: 64bit MMIO 분할 시 PCIe 버스가 분리 처리하지 않으므로
	 * 32bit access로 강제. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_INTEL, 0x5845, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_INTEL_QUIRK_NO_LOG_PAGES |
		NVME_QUIRK_MAXIMUM_PCI_ACCESS_WIDTH
	},
	/* [한국어] Red Hat virtio-NVMe — QEMU 에뮬레이션 컨트롤러. 동일 PCI access width 이슈. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_REDHAT, 0x0010, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_MAXIMUM_PCI_ACCESS_WIDTH
	},
	/* [한국어] CNEX Labs 0x1f1f — OpenChannel SSD. IDENTIFY_CNS: Identify CNS field 비표준.
	 * OCSSD: OpenChannel SSD 코드 경로 활성. (현재 OCSSD 지원은 점진적 deprecated 진행.) */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_CNEXLABS, 0x1f1f, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_IDENTIFY_CNS |
		NVME_QUIRK_OCSSD
	},
	/* [한국어] VMware ESXi 가상 NVMe — Shutdown Status가 항상 COMPLETE인 것처럼 보고하므로
	 * shutdown 절차에서 SHST 폴링을 건너뛰어 무한 대기 방지. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_VMWARE, 0x07f0, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_SHST_COMPLETE
	},
	/* [한국어] Intel 0x2700 — 특정 모델에서 OACS의 Security Send/Recv 비트가 잘못 보고됨.
	 * OACS_SECURITY: SPDK가 자체적으로 security 명령 지원 가정 무시. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_INTEL, 0x2700, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_OACS_SECURITY
	},
	/* [한국어] Intel 0x4140 — MDTS(Maximum Data Transfer Size) 계산 시 metadata 길이를
	 * 포함하지 않도록 quirk. 펌웨어 해석 차이 보정. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_INTEL, 0x4140, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_MDTS_EXCLUDE_MD
	},
	/* [한국어] Huawei 전체(device_id 와일드카드) — SGL 사용 시 컨트롤러가 hang/오답 →
	 * NOT_USE_SGL 로 PRP only 강제. Huawei OEM SSD 전반 적용. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_HUAWEI, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_NOT_USE_SGL
	},
	/* [한국어] Microsoft Azure NVMe (device_id 0xb111) — admin 큐 깊이가 너무 작으면 reject →
	 * MINIMUM_ADMIN_QUEUE_SIZE 로 안전한 최소값 강제. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_MICROSOFT, 0xb111, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_MINIMUM_ADMIN_QUEUE_SIZE
	},
	/* [한국어] Micron 전체(device 와일드카드) — MSI-X 벡터 카운트가 잘못 보고되어 보정 필요. */
	{	{SPDK_PCI_CLASS_NVME, SPDK_PCI_VID_MICRON, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID},
		NVME_QUIRK_MSIX_VECTOR_COUNT
	},
	/* [한국어] Sentinel — 종료 마커. nvme_get_quirks()의 while 루프가 vendor_id == 0 으로 탈출. */
	{	{0x000000, 0x0000, 0x0000, 0x0000, 0x0000}, 0}
};

/* Compare each field. SPDK_PCI_ANY_ID in s1 matches everything */
/*
 * [한국어]
 * pci_id_match - quirk 패턴(s1)과 실제 디바이스 PCI ID(s2)를 비교하는 헬퍼.
 *
 * @s1: quirk 테이블의 패턴 (와일드카드 SPDK_PCI_ANY_ID/SPDK_PCI_CLASS_ANY_ID 가능).
 * @s2: 실제 발견된 NVMe 디바이스의 PCI ID (와일드카드 의미 없음, 실제 값).
 * @return: 5필드가 모두 매치하면 true(0이 아닌 값), 하나라도 불일치하면 false.
 *
 * 와일드카드 의미:
 *  - class_id 의 와일드카드: SPDK_PCI_CLASS_ANY_ID (0xFFFFFF, 24비트 풀)
 *  - 나머지 4필드의 와일드카드: SPDK_PCI_ANY_ID (0xFFFF, 16비트 풀)
 * 즉 s1의 어느 한 필드라도 와일드카드면 그 필드는 무조건 통과시킨다.
 *
 * 실행 컨텍스트: nvme_get_quirks() 내부 루프에서만 호출되는 순수 함수. 부수 효과 없음.
 * 매칭 비교가 짧고 분기 예측이 쉬워 인라인 최적화에 적합 (static + 단일 호출자).
 *
 * 호출 체인:
 *   nvme_get_quirks → [pci_id_match] → return bool
 */
static bool
pci_id_match(const struct spdk_pci_id *s1, const struct spdk_pci_id *s2)
{
	/* [한국어] 5필드 AND 비교: 각 필드가 (와일드카드) OR (정확 일치) 둘 중 하나면 통과.
	 * class_id만 SPDK_PCI_CLASS_ANY_ID(24비트), 나머지는 SPDK_PCI_ANY_ID(16비트) 사용.
	 * 모든 조건 통과 시 true 반환 — 호출자(nvme_get_quirks)가 quirk flags 채택. */
	if ((s1->class_id == SPDK_PCI_CLASS_ANY_ID || s1->class_id == s2->class_id) &&
	    (s1->vendor_id == SPDK_PCI_ANY_ID || s1->vendor_id == s2->vendor_id) &&
	    (s1->device_id == SPDK_PCI_ANY_ID || s1->device_id == s2->device_id) &&
	    (s1->subvendor_id == SPDK_PCI_ANY_ID || s1->subvendor_id == s2->subvendor_id) &&
	    (s1->subdevice_id == SPDK_PCI_ANY_ID || s1->subdevice_id == s2->subdevice_id)) {
		return true; /* [한국어] 모든 필드 매치 — quirk 적용 결정. */
	}
	return false; /* [한국어] 하나라도 불일치 — 다음 quirk 행으로 이동 권유. */
}

/*
 * [한국어]
 * nvme_get_quirks - 주어진 NVMe 디바이스에 적용할 quirk 비트마스크 룩업.
 *
 * @id: PCIe configuration space에서 추출한 실제 디바이스의 5튜플 ID.
 * @return: 첫 매칭 행의 quirk 플래그(64비트 OR 비트마스크). 매치 실패 시 0.
 *
 * 이 함수가 필요한 이유:
 *   NVMe 1.x/2.x 스펙은 ICA(Implementation Choice Allowed) 영역이 넓어 벤더마다
 *   해석/타이밍/지원 기능이 미묘하게 다르다. SPDK는 컨트롤러 attach 시점에 본 함수를
 *   호출하여 알려진 비표준 동작을 컨트롤러 객체에 캐싱하고, 이후 모든 코드 경로에서
 *   quirks 비트마스크를 분기 조건으로 사용해 "디바이스별 최선 동작"을 보장한다.
 *
 * 동작 단계:
 *   1) 디버그 로그로 검색 시작 알림 (vendor:device [subv:subd]).
 *   2) sentinel(vendor_id == 0)을 만날 때까지 nvme_quirks[] 선형 탐색.
 *   3) pci_id_match() 통과한 첫 행이 나오면, PRINT_QUIRK 매크로로 활성화된 비트들을
 *      차례로 디버그 로그에 출력하고 quirk->flags 반환 → 함수 종료.
 *   4) 매칭 실패하면 quirk++ 후 다음 행으로. sentinel 도달 시 0 반환.
 *
 * 실행 컨텍스트: 컨트롤러 attach/init 단계에서 호출되며, 이는 SPDK 메인 init 스레드
 * (또는 컨트롤러 attach 작업 스레드)이다. 부수 효과는 디버그 로그 출력 뿐이고, 테이블이
 * read-only이므로 동시에 여러 컨트롤러를 attach 해도 안전. 시간 복잡도는 O(N) 선형
 * 탐색이지만 N이 작아(<25행) 사실상 무시.
 *
 * 호출자: nvme_pcie.c / nvme_ctrlr.c 의 컨트롤러 생성 경로 (단 한 번 호출, 결과는 ctrlr->quirks).
 * 콜리: pci_id_match (이 파일), SPDK_DEBUGLOG (include/spdk/log.h).
 *
 * 호출 체인:
 *   nvme_ctrlr_construct → [nvme_get_quirks] → pci_id_match → return uint64_t
 */
uint64_t
nvme_get_quirks(const struct spdk_pci_id *id)
{
	/* [한국어] 테이블 헤드 포인터 — 루프 내에서 ++로 전진. 원본 nvme_quirks[]는 const라
	 * 수정 위험 없음. 포인터 자체는 로컬이므로 reentrant 안전. */
	const struct nvme_quirk *quirk = nvme_quirks;

	/* [한국어] 검색 시작을 nvme 디버그 로그 카테고리에 기록. 사용자/개발자가
	 * SPDK_LOG_REGISTER_COMPONENT(nvme) + --logflag nvme 로 활성화 시 출력. */
	SPDK_DEBUGLOG(nvme, "Searching for %04x:%04x [%04x:%04x]...\n",
		      id->vendor_id, id->device_id,
		      id->subvendor_id, id->subdevice_id);

	/* [한국어] sentinel(vendor_id == 0) 만날 때까지 선형 탐색.
	 * 0 vendor_id는 PCI-SIG 미할당 값이므로 정상 디바이스와 충돌 없음 → 종료 마커로 안전. */
	while (quirk->id.vendor_id) {
		/* [한국어] 현재 행의 패턴이 디바이스와 매치되는지 확인. */
		if (pci_id_match(&quirk->id, id)) {
			/* [한국어] 매치 성공: 어느 패턴 행에 매치됐는지 디버그 로그에 기록. */
			SPDK_DEBUGLOG(nvme, "Matched quirk %04x:%04x [%04x:%04x]:\n",
				      quirk->id.vendor_id, quirk->id.device_id,
				      quirk->id.subvendor_id, quirk->id.subdevice_id);

/* [한국어] PRINT_QUIRK: 단일 비트가 활성화돼 있으면 그 비트의 매크로 이름 그대로
 * (#quirk_flag stringification) 디버그 로그에 출력. do{}while(0) 관용구로 매크로
 * 안전하게 단일 statement 화. 모든 19개 quirk 플래그를 수동으로 나열하므로 새 quirk
 * 추가 시 이 목록도 함께 갱신해야 함 (누락돼도 동작에는 영향 없고 디버그 가독성만 손상). */
#define PRINT_QUIRK(quirk_flag) \
			do { \
				if (quirk->flags & (quirk_flag)) { \
					SPDK_DEBUGLOG(nvme, "Quirk enabled: %s\n", #quirk_flag); \
				} \
			} while (0)

			/* [한국어] Intel-specific: 벤더 latency log page 활성. */
			PRINT_QUIRK(NVME_INTEL_QUIRK_READ_LATENCY);
			PRINT_QUIRK(NVME_INTEL_QUIRK_WRITE_LATENCY);
			/* [한국어] CSTS.RDY 폴링 전 대기 (펌웨어 안정화 시간). */
			PRINT_QUIRK(NVME_QUIRK_DELAY_BEFORE_CHK_RDY);
			/* [한국어] Intel stripe 정렬 우대. */
			PRINT_QUIRK(NVME_INTEL_QUIRK_STRIPING);
			/* [한국어] 큐 할당 후 doorbell write 전 sleep (가상화 race 회피). */
			PRINT_QUIRK(NVME_QUIRK_DELAY_AFTER_QUEUE_ALLOC);
			/* [한국어] Deallocate(TRIM) 후 0 read 보장 (Intel 특성). */
			PRINT_QUIRK(NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE);
			/* [한국어] Identify CNS field 비표준 해석. */
			PRINT_QUIRK(NVME_QUIRK_IDENTIFY_CNS);
			/* [한국어] OpenChannel SSD 코드 경로 활성. */
			PRINT_QUIRK(NVME_QUIRK_OCSSD);
			/* [한국어] Intel-specific log page 미지원 표시. */
			PRINT_QUIRK(NVME_INTEL_QUIRK_NO_LOG_PAGES);
			/* [한국어] Shutdown Status 항상 COMPLETE 처럼 동작 (VMware). */
			PRINT_QUIRK(NVME_QUIRK_SHST_COMPLETE);
			/* [한국어] init 시작 전 강제 sleep. */
			PRINT_QUIRK(NVME_QUIRK_DELAY_BEFORE_INIT);
			/* [한국어] I/O 큐 최소 깊이 강제. */
			PRINT_QUIRK(NVME_QUIRK_MINIMUM_IO_QUEUE_SIZE);
			/* [한국어] PCIe MMIO 접근 폭 32bit 강제. */
			PRINT_QUIRK(NVME_QUIRK_MAXIMUM_PCI_ACCESS_WIDTH);
			/* [한국어] OACS의 Security 비트 무시. */
			PRINT_QUIRK(NVME_QUIRK_OACS_SECURITY);
			/* [한국어] DSM(Dataset Mgmt) 명령에서만 SGL 비활성. */
			PRINT_QUIRK(NVME_QUIRK_NO_SGL_FOR_DSM);
			/* [한국어] MDTS 계산 시 metadata 길이 제외. */
			PRINT_QUIRK(NVME_QUIRK_MDTS_EXCLUDE_MD);
			/* [한국어] SGL 전체 비활성 (Huawei). */
			PRINT_QUIRK(NVME_QUIRK_NOT_USE_SGL);
			/* [한국어] admin 큐 최소 크기 강제 (Microsoft Azure). */
			PRINT_QUIRK(NVME_QUIRK_MINIMUM_ADMIN_QUEUE_SIZE);
			/* [한국어] MSI-X 벡터 카운트 보정 (Micron). */
			PRINT_QUIRK(NVME_QUIRK_MSIX_VECTOR_COUNT);

			/* [한국어] 첫 매치만 적용하고 즉시 반환 (early exit).
			 * 같은 디바이스가 여러 행에 매치될 가능성을 차단 — 테이블 작성자가
			 * 위에 더 좁은 패턴을 두는 책임을 지도록 한 설계. */
			return quirk->flags;
		}
		quirk++; /* [한국어] 매치 실패 — 다음 quirk 행으로 전진. */
	}

	/* [한국어] sentinel 도달 = 매치 실패. 표준 동작 가정 — quirk 0 반환. */
	SPDK_DEBUGLOG(nvme, "No quirks found.\n");

	return 0; /* [한국어] 0 = NVME_QUIRK 비트가 하나도 set되지 않음 → 모든 분기에서 표준 경로. */
}
