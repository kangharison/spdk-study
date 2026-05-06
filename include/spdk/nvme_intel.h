/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Intel NVMe vendor-specific definitions
 *
 * Reference:
 * http://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/ssd-dc-p3700-spec.pdf
 */

/*
 * [한국어 설명] Intel NVMe 벤더 specific 정의 (nvme_intel.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 Intel SSD(특히 DC P3x00, P4x00, Optane 계열)의 NVMe **벤더 확장** 영역을
 * SPDK 사용자 공간 NVMe 드라이버가 인식·해석할 수 있도록 enum, 구조체, 비트필드 union을
 * 모은 공개 헤더이다. NVMe Base 1.x/2.x 스펙은 opcode/LID(Log Identifier)/FID(Feature Identifier)
 * 의 0xC0~0xFF 영역을 "Vendor Specific"으로 비워두는데, Intel은 이 영역을 사용해
 * 펌웨어 진단·전력 관리·SMART 확장 등을 노출한다.
 * 이 파일은 와이어 포맷(바이트 레이아웃, opcode 코드값) 그 자체이며, 어떤 함수도 정의하지 않는다.
 * 사용자는 spdk_nvme_ctrlr_cmd_get_log_page() 또는 spdk_nvme_ctrlr_cmd_get_feature() 같은
 * 일반 NVMe API에 여기 정의된 LID/FID 값을 넘기고, 응답 페이로드를 여기 정의된 구조체로 해석한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe 스택에서 이 파일은 가장 위(공개 API) 계층에 속한다.
 *   상위 호출자: app/spdk_nvme_perf, examples/nvme/identify, nvme-cli, NVMe-oF target 보안 모듈
 *      → spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_INTEL_LOG_SMART, ...) 호출
 *   해석 단계: 응답 버퍼를 struct spdk_nvme_intel_smart_information_page * 로 캐스팅해 사용
 *   하위 경로: lib/nvme/nvme_ctrlr.c::nvme_ctrlr_set_intel_supported_log_pages()는
 *      Intel 컨트롤러를 PCI VID(0x8086)로 식별 후, 여기 정의된 LID 0xC1/0xC2/0xC5/0xCA/0xDD를
 *      ctrlr->log_page_supported 비트맵에 마킹해서 사용자 질의에 대비한다.
 *   실행 컨텍스트: 호스트 유저스페이스의 SPDK 스레드(spdk_thread)에서 호출되며, 결과적으로
 *      NVMe Admin 큐(SQ0/CQ0)로 Get Log Page (opcode 0x02) / Get Feature (opcode 0x0A) /
 *      Set Feature (opcode 0x09) 명령이 발행된다. polled-mode 완료 통지로 응답이 도달한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h(uint*_t), spdk/assert.h(SPDK_STATIC_ASSERT 매크로 — 구조체 크기 강제 검증).
 * 의존되는 곳:
 *   - lib/nvme/nvme_ctrlr.c: Intel 컨트롤러용 log page/feature 지원 비트맵 설정
 *   - examples/nvme/identify/identify.c: --intel 옵션 시 Intel SMART, latency histogram, marketing name 출력
 *   - module/bdev/nvme/bdev_nvme.c: Intel SSD에서 PI(Protection Information) 등 부가 진단을 노출할 때
 * 데이터 흐름:
 *   호스트 메모리(struct spdk_nvme_intel_*_page) ← DMA ← SSD 펌웨어 로그 영역
 *   호스트 메모리(union spdk_nvme_intel_feat_*) → DMA → SSD 펌웨어 feature 레지스터
 * 공유 자료구조: 본 파일은 와이어 포맷 정의만 제공하며, ctrlr 핸들 등 런타임 상태는 보유하지 않는다.
 * NVMe 1.x 스펙과의 관계: Get Log Page (Section 5.14) 와 Get/Set Features (Section 5.15)에서
 * vendor-specific 영역(LID 0xC0-0xFF, FID 0xC0-0xFF)을 차용한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - enum spdk_nvme_intel_feat: Intel Set/Get Feature ID (0xC1 MAX_LBA, 0xC6 power governor,
 *   0xE2 latency tracking 등) — Get/Set Feature opcode의 CDW10[FID] 필드에 들어간다.
 * - enum spdk_nvme_intel_log_page: Intel Get Log Page LID (0xC0 directory, 0xC1/0xC2 latency,
 *   0xC5 temperature, 0xCA SMART 확장, 0xDD marketing description) — Get Log Page CDW10[LID]에 사용.
 * - enum spdk_nvme_intel_smart_attribute_code: Intel SMART 페이지 내부 attribute 코드 (0xAB~0xF5).
 * - struct spdk_nvme_intel_log_page_directory(512B): 펌웨어가 어떤 Intel LID를 지원하는지 길이 표.
 * - struct spdk_nvme_intel_rw_latency_page(380B): 32us/1ms/32ms 단위 히스토그램 버킷.
 * - struct spdk_nvme_intel_temperature_page(112B): current/highest/lowest/shutdown 온도 통계.
 * - struct spdk_nvme_intel_smart_attribute(12B): 코드+normalized+raw 6바이트 SMART 단일 항목.
 * - struct spdk_nvme_intel_smart_information_page(156B = 13×12, packed): SMART 13개 attribute 묶음.
 * - union spdk_nvme_intel_feat_*: 각 FID를 위한 32비트 CDW11 비트필드 인터프리터.
 * - struct spdk_nvme_intel_marketing_description_page(4096B): 펌웨어가 채우는 제품 마케팅 문자열.
 */

#ifndef SPDK_NVME_INTEL_H
/* [한국어] 다중 포함 방지 가드 — 이 헤더가 같은 번역 단위에 두 번 포함되어 enum/struct가
 * 중복 선언되는 것을 막는다. */
#define SPDK_NVME_INTEL_H

#include "spdk/stdinc.h"
/* [한국어] uint8_t/uint16_t/uint32_t/uint64_t 같은 고정폭 정수 타입을 가져온다.
 * NVMe 와이어 포맷은 폭이 정확히 정해져 있어 plain int 사용이 금지된다. */

#ifdef __cplusplus
/* [한국어] C++에서 이 C 헤더를 포함할 때 심볼 이름 맹글링을 방지하기 위한 extern "C" 가드.
 * SPDK는 nvmf/bdev 모듈 일부가 C++로 빌드될 수 있어 모든 공개 헤더에 이 가드가 있다. */
extern "C" {
#endif

#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로 정의를 가져온다 — 컴파일 타임에 sizeof(struct) 검증.
 * NVMe 스펙은 각 로그 페이지/feature 페이로드의 크기를 정확히 명시하므로
 * 구조체 패딩/정렬 오류가 발생하면 빌드 자체를 실패시켜야 한다. */

enum spdk_nvme_intel_feat {
	/* [한국어] Intel Set/Get Feature 명령(opcode Set=0x09, Get=0x0A)의 CDW10[FID] 필드에 들어가는
	 * 벤더 확장 Feature Identifier 모음. 0xC0~0xFF는 NVMe 스펙이 vendor-specific으로 비워둔 영역.
	 * 설정자: 사용자가 spdk_nvme_ctrlr_cmd_set_feature(ctrlr, FID, cdw11, ...) 호출 시 인자.
	 * 읽는 자: 컨트롤러 펌웨어가 FID를 디스패치해 해당 동작 수행.
	 * 동기화: SPDK Admin qpair는 한 스레드에서만 다루므로 별도 락 불필요. */

	SPDK_NVME_INTEL_FEAT_MAX_LBA				= 0xC1,
	/* [한국어] FID 0xC1 — 컨트롤러가 노출하는 사용자 LBA 최대값 설정/조회.
	 * Set 시 CDW11에 새 max LBA를 인코딩해 SSD를 over-provisioning 모드로 전환할 수 있다.
	 * 실패 시 status code는 spdk_nvme_intel_set_max_lba_command_status_code 참조. */

	SPDK_NVME_INTEL_FEAT_NATIVE_MAX_LBA			= 0xC2,
	/* [한국어] FID 0xC2 — SSD의 물리적 native max LBA(공장 출하 시 값) 조회 전용.
	 * MAX_LBA(0xC1)로 변경된 상태에서도 native 한계를 알 수 있어 복구 시 사용. */

	SPDK_NVME_INTEL_FEAT_POWER_GOVERNOR_SETTING		= 0xC6,
	/* [한국어] FID 0xC6 — 전력 거버너(thermal/power cap) 설정.
	 * CDW11 인코딩은 union spdk_nvme_intel_feat_power_governor 참조 (00h=25W/01h=20W/02h=10W). */

	SPDK_NVME_INTEL_FEAT_SMBUS_ADDRESS			= 0xC8,
	/* [한국어] FID 0xC8 — SSD가 SMBus(out-of-band BMC 통신)에서 응답할 7비트 슬레이브 주소.
	 * 데이터센터에서 NVMe-MI(Management Interface)로 모니터링할 때 BMC가 사용. */

	SPDK_NVME_INTEL_FEAT_LED_PATTERN			= 0xC9,
	/* [한국어] FID 0xC9 — 드라이브 식별 LED 패턴(블링크/솔리드 등) 설정.
	 * CDW11 비트필드는 union spdk_nvme_intel_feat_led_pattern 참조. */

	SPDK_NVME_INTEL_FEAT_RESET_TIMED_WORKLOAD_COUNTERS	= 0xD5,
	/* [한국어] FID 0xD5 — 시간 기반 워크로드 카운터(SMART E2/E3/E4) 리셋.
	 * Set 전용; Read는 미지원. CDW11.bit0=1로 설정 시 카운터들이 0으로 클리어. */

	SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING			= 0xE2,
	/* [한국어] FID 0xE2 — read/write latency histogram 추적 enable/disable.
	 * 활성화 후 LID 0xC1/0xC2 로그 페이지에 버킷화된 통계가 채워진다. */
};

enum spdk_nvme_intel_set_max_lba_command_status_code {
	/* [한국어] FID 0xC1 (MAX_LBA) Set 명령이 실패할 때 NVMe 완료 큐 엔트리(CQE) Status Field의
	 * Status Code 영역(SCT=Vendor Specific=0x7)에 반환되는 Intel 정의 코드.
	 * 설정자: SSD 펌웨어가 거부 사유에 따라 채움.
	 * 읽는 자: 호스트가 spdk_nvme_cpl::status.sc 값을 이 enum과 비교해 사용자에게 메시지 출력. */

	SPDK_NVME_INTEL_EXCEEDS_AVAILABLE_CAPACITY		= 0xC0,
	/* [한국어] 요청한 max LBA가 드라이브의 가용 NAND 용량을 초과 — 물리적으로 불가능. */

	SPDK_NVME_INTEL_SMALLER_THAN_MIN_LIMIT			= 0xC1,
	/* [한국어] 요청 값이 펌웨어가 허용하는 최소 max LBA보다 작음 — 너무 작은 over-provisioning. */

	SPDK_NVME_INTEL_SMALLER_THAN_NS_REQUIREMENTS		= 0xC2,
	/* [한국어] 이미 생성된 namespace의 합계보다 작은 값 — namespace 삭제 후 재시도 필요. */
};

enum spdk_nvme_intel_log_page {
	/* [한국어] Intel Get Log Page (NVMe opcode 0x02)의 CDW10[LID] 필드 값 모음.
	 * 0xC0~0xFF는 vendor-specific. 각 LID는 고유한 페이지 크기와 레이아웃을 가진다.
	 * 설정자: 사용자가 spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, LID, ...) 호출 시 인자.
	 * 읽는 자: lib/nvme/nvme_ctrlr.c의 set_intel_supported_log_pages()는
	 * Intel VID(0x8086) 컨트롤러에 한해 이 LID들을 지원 비트맵에 등록한다. */

	SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY			= 0xC0,
	/* [한국어] LID 0xC0 — 어떤 Intel LID가 펌웨어에서 지원되는지를 길이로 표시한 디렉토리.
	 * struct spdk_nvme_intel_log_page_directory(512B)로 해석. 길이가 0이면 미지원. */

	SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY			= 0xC1,
	/* [한국어] LID 0xC1 — 읽기 명령 latency histogram. struct spdk_nvme_intel_rw_latency_page.
	 * FEAT_LATENCY_TRACKING(0xE2)으로 활성화 후에만 의미 있는 값. */

	SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY			= 0xC2,
	/* [한국어] LID 0xC2 — 쓰기 명령 latency histogram. 구조는 LID 0xC1과 동일. */

	SPDK_NVME_INTEL_LOG_TEMPERATURE				= 0xC5,
	/* [한국어] LID 0xC5 — 온도 통계 페이지. struct spdk_nvme_intel_temperature_page(112B).
	 * NVMe 1.x 스펙의 SMART/Health(LID 0x02)에 비해 historical min/max를 추가 제공. */

	SPDK_NVME_INTEL_LOG_SMART				= 0xCA,
	/* [한국어] LID 0xCA — Intel 확장 SMART (NAND wear, host bytes written, thermal throttle 등).
	 * struct spdk_nvme_intel_smart_information_page(156B). 일반 SMART(LID 0x02)와 별개로 노출. */

	SPDK_NVME_INTEL_MARKETING_DESCRIPTION			= 0xDD,
	/* [한국어] LID 0xDD — 제품 마케팅 문자열 페이지(4096B 패딩, 실제 데이터는 처음 512B).
	 * struct spdk_nvme_intel_marketing_description_page. examples/nvme/identify에서 출력. */
};

enum spdk_nvme_intel_smart_attribute_code {
	/* [한국어] Intel 확장 SMART 페이지(LID 0xCA) 내부의 각 attribute 슬롯에 들어가는 code 값.
	 * struct spdk_nvme_intel_smart_attribute::code 필드와 매칭.
	 * 설정자: SSD 펌웨어가 각 attribute 슬롯에 코드+normalized+raw를 채워 응답.
	 * 읽는 자: 호스트가 13개 attribute를 순회하며 코드별 분기해 raw_value[6]를 해석. */

	SPDK_NVME_INTEL_SMART_PROGRAM_FAIL_COUNT		= 0xAB,
	/* [한국어] 0xAB — NAND program(쓰기) 실패 횟수. NAND 셀 수명/결함 지표. */

	SPDK_NVME_INTEL_SMART_ERASE_FAIL_COUNT			= 0xAC,
	/* [한국어] 0xAC — NAND erase(블록 소거) 실패 횟수. */

	SPDK_NVME_INTEL_SMART_WEAR_LEVELING_COUNT		= 0xAD,
	/* [한국어] 0xAD — wear leveling 카운트 (P/E cycle 분포 통계). */

	SPDK_NVME_INTEL_SMART_E2E_ERROR_COUNT			= 0xB8,
	/* [한국어] 0xB8 — End-to-end CRC/ECC 보호로 잡힌 데이터 경로 오류 누적. */

	SPDK_NVME_INTEL_SMART_CRC_ERROR_COUNT			= 0xC7,
	/* [한국어] 0xC7 — PCIe/NVMe 링크에서 발생한 CRC 오류 누적. */

	SPDK_NVME_INTEL_SMART_MEDIA_WEAR			= 0xE2,
	/* [한국어] 0xE2 — NAND 매체 마모율(0~정규화). FID 0xD5로 카운터 리셋 가능. */

	SPDK_NVME_INTEL_SMART_HOST_READ_PERCENTAGE		= 0xE3,
	/* [한국어] 0xE3 — 전체 I/O 중 host read 비율. R/W 워크로드 패턴 추정에 사용. */

	SPDK_NVME_INTEL_SMART_TIMER				= 0xE4,
	/* [한국어] 0xE4 — 위의 timed counter들이 쌓인 시간(분 단위 등). */

	SPDK_NVME_INTEL_SMART_THERMAL_THROTTLE_STATUS		= 0xEA,
	/* [한국어] 0xEA — 현재 thermal throttle이 활성화되었는지 + 누적 throttle 시간. */

	SPDK_NVME_INTEL_SMART_RETRY_BUFFER_OVERFLOW_COUNTER	= 0xF0,
	/* [한국어] 0xF0 — 내부 retry buffer overflow 발생 횟수 (NAND 재시도 압박 지표). */

	SPDK_NVME_INTEL_SMART_PLL_LOCK_LOSS_COUNT		= 0xF3,
	/* [한국어] 0xF3 — PCIe PHY PLL lock loss 횟수 (링크 안정성 지표). */

	SPDK_NVME_INTEL_SMART_NAND_BYTES_WRITTEN		= 0xF4,
	/* [한국어] 0xF4 — NAND에 실제로 기록된 누적 바이트 (write amplification 계산용 분자). */

	SPDK_NVME_INTEL_SMART_HOST_BYTES_WRITTEN		= 0xF5,
	/* [한국어] 0xF5 — 호스트가 발행한 누적 쓰기 바이트 (WAF의 분모).
	 * WAF = NAND_BYTES_WRITTEN / HOST_BYTES_WRITTEN. */
};

struct spdk_nvme_intel_log_page_directory {
	/* [한국어] LID 0xC0 (Log Page Directory) 응답 페이로드의 와이어 포맷.
	 * 펌웨어가 어떤 Intel LID를 지원하는지 길이로 보고하는 인덱스 페이지(512B 고정).
	 * 설정자: SSD 펌웨어가 Get Log Page(LID=0xC0) 응답으로 DMA 채움.
	 * 읽는 자: lib/nvme/nvme_ctrlr.c::set_intel_supported_log_pages()가 각 *_log_len을 검사해
	 *   0이 아닌 항목만 ctrlr->log_page_supported 비트맵에 등록.
	 * 동기화: 컨트롤러 초기화 단계에서 한 번만 채워지고 이후 read-only로 사용. */

	uint8_t		version[2];
	/* [한국어] 디렉토리 페이지 포맷의 메이저/마이너 버전 (Intel 정의).
	 * 설정자: 펌웨어. 읽는 자: 호스트가 향후 새 필드 해석 가능 여부 판단.
	 * 값 범위: Intel 펌웨어 릴리스에 따라 단조 증가. */

	uint8_t		reserved[384];
	/* [한국어] 향후 확장을 위한 예약 영역 — 호스트는 0인지 검사하지 않고 무시.
	 * 설정자: 펌웨어가 0으로 채움. 동기화: 의미 없음(read-only). */

	uint8_t		read_latency_log_len;
	/* [한국어] LID 0xC1 (Read Latency) 페이지의 길이(바이트 단위 비트 표현).
	 * 0이면 펌웨어가 LID 0xC1을 미지원. 설정자: 펌웨어. 읽는 자: log_page_supported 비트맵 채울 때. */

	uint8_t		reserved2;
	/* [한국어] 길이 필드 사이의 패딩 1바이트 — 호스트 무시. */

	uint8_t		write_latency_log_len;
	/* [한국어] LID 0xC2 (Write Latency) 페이지 지원 여부/길이. 0이면 미지원. */

	uint8_t		reserved3[5];
	/* [한국어] 다음 필드까지의 5바이트 패딩 — 와이어 포맷 정렬. */

	uint8_t		temperature_statistics_log_len;
	/* [한국어] LID 0xC5 (Temperature Statistics) 페이지 길이. 0이면 미지원. */

	uint8_t		reserved4[9];
	/* [한국어] 9바이트 패딩 — 후속 SMART 길이 필드의 오프셋(0xC0~0xCA 인덱스 거리)을 맞춤. */

	uint8_t		smart_log_len;
	/* [한국어] LID 0xCA (Intel Extended SMART) 페이지 길이. 0이면 미지원.
	 * 일반 SMART(LID 0x02)와 다름에 유의. */

	uint8_t		reserved5[37];
	/* [한국어] 37바이트 패딩 — marketing description(0xDD) 슬롯까지의 거리. */

	uint8_t		marketing_description_log_len;
	/* [한국어] LID 0xDD (Marketing Description) 페이지 길이. 0이면 미지원.
	 * 정상 펌웨어는 4096B를 보고. */

	uint8_t		reserved6[69];
	/* [한국어] 페이지 끝까지의 패딩 — 전체 sizeof()를 정확히 512로 맞추기 위함. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_intel_log_page_directory) == 512, "Incorrect size");
/* [한국어] 컴파일 타임 검증 — 디렉토리 페이지가 정확히 512바이트(NVMe 스펙 정의)인지 확인.
 * 패딩/정렬 오류로 크기가 어긋나면 빌드 실패. */

struct spdk_nvme_intel_rw_latency_page {
	/* [한국어] LID 0xC1(Read) / 0xC2(Write)의 응답 페이로드 (380B).
	 * latency 분포를 32us/1ms/32ms 단위 버킷 히스토그램으로 표현.
	 * 설정자: 펌웨어가 FID 0xE2(LATENCY_TRACKING) 활성화 상태에서만 채움.
	 * 읽는 자: 사용자가 모든 버킷을 순회해 분위수(p50/p99 등)를 계산.
	 * 동기화: 펌웨어가 atomic snapshot으로 응답 — 호스트는 별도 락 불필요. */

	uint16_t		major_revison;
	/* [한국어] 페이지 포맷 메이저 버전 (오타 "revison"은 Intel 스펙 원문 표기).
	 * 설정자: 펌웨어. 읽는 자: 호스트가 알고 있는 버전과 일치 여부 확인. */

	uint16_t		minor_revison;
	/* [한국어] 마이너 버전. 같은 메이저 내에서는 호환되도록 펌웨어 업데이트. */

	uint32_t		buckets_32us[32];
	/* [한국어] 0~32us, 32~64us, ... 식으로 32us 폭 32개 버킷 카운터 (총 0~1024us 영역).
	 * 짧은 latency 영역의 정밀 분포. 카운터는 32비트 → 4G 발생 시 wrap 가능. */

	uint32_t		buckets_1ms[31];
	/* [한국어] 1ms 폭 31개 버킷 (1~32ms 영역) — 중간 영역 분포.
	 * 32us 영역과 1ms 영역 사이는 펌웨어가 매핑 정의. */

	uint32_t		buckets_32ms[31];
	/* [한국어] 32ms 폭 31개 버킷 (32~1024ms 영역) — long-tail latency 추적.
	 * 가장 마지막 버킷은 ">= 상한"의 의미를 갖는 경우가 많음(펌웨어 의존). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_intel_rw_latency_page) == 380, "Incorrect size");
/* [한국어] 4(2+2) + 32*4 + 31*4 + 31*4 = 4 + 128 + 124 + 124 = 380. 와이어 포맷 크기 검증. */

struct spdk_nvme_intel_temperature_page {
	/* [한국어] LID 0xC5 응답 페이로드(112B) — 일반 SMART(LID 0x02)에 없는 historical min/max 제공.
	 * 설정자: 펌웨어가 내부 thermal sensor를 주기 폴링해 누적.
	 * 읽는 자: 사용자/모니터링 도구가 thermal 트렌드 추적. */

	uint64_t		current_temperature;
	/* [한국어] 현재 컴포지트 온도 (단위는 K, NVMe 표준과 일치).
	 * 설정자: 펌웨어가 매 응답마다 갱신. 읽는 자: 호스트가 K→°C 변환(-273). */

	uint64_t		shutdown_flag_last;
	/* [한국어] 마지막 부팅 사이클에서 thermal shutdown(과열 종료) 발생 여부 플래그. */

	uint64_t		shutdown_flag_life;
	/* [한국어] 디바이스 전체 수명 동안 누적된 thermal shutdown 발생 횟수. */

	uint64_t		highest_temperature;
	/* [한국어] 디바이스 수명 동안 관측된 최고 온도(K). */

	uint64_t		lowest_temperature;
	/* [한국어] 디바이스 수명 동안 관측된 최저 온도(K). */

	uint64_t		reserved[5];
	/* [한국어] Intel이 향후 확장을 위해 예약한 5×8=40바이트 — 호스트는 0으로 가정하지 않음. */

	uint64_t		specified_max_op_temperature;
	/* [한국어] 펌웨어가 광고하는 최대 동작 온도(K) — 데이터시트 값과 일치. */

	uint64_t		reserved2;
	/* [한국어] 8바이트 예약. */

	uint64_t		specified_min_op_temperature;
	/* [한국어] 펌웨어가 광고하는 최소 동작 온도(K). */

	uint64_t		estimated_offset;
	/* [한국어] 센서 오프셋 추정값(보정용). 실온도 ≈ current - estimated_offset. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_intel_temperature_page) == 112, "Incorrect size");
/* [한국어] 8 * 14 = 112 — 14개의 uint64_t로 구성. */

struct spdk_nvme_intel_smart_attribute {
	/* [한국어] Intel SMART 페이지의 단일 attribute 슬롯(12B). ATA SMART와 유사한 레이아웃.
	 * 설정자: 펌웨어. 읽는 자: 호스트가 attributes[13] 배열을 순회. */

	uint8_t			code;
	/* [한국어] attribute 식별 코드 — enum spdk_nvme_intel_smart_attribute_code 참조.
	 * 설정자: 펌웨어. 읽는 자: 호스트가 switch(code)로 raw_value 해석 분기. */

	uint8_t			reserved[2];
	/* [한국어] 코드 뒤 2바이트 예약 — 향후 status 비트로 확장될 수 있음. */

	uint8_t			normalized_value;
	/* [한국어] 0~100 범위의 정규화 값 (대개 100=새 것, 0=수명 임박).
	 * MEDIA_WEAR(0xE2) 같은 마모 지표는 100→0으로 감소. */

	uint8_t			reserved2;
	/* [한국어] 1바이트 예약. */

	uint8_t			raw_value[6];
	/* [한국어] 코드별 의미가 다른 raw 값 6바이트 (대개 little-endian 카운터).
	 * 예: HOST_BYTES_WRITTEN은 32MiB 단위 카운터로 인코딩. */

	uint8_t			reserved3;
	/* [한국어] 마지막 1바이트 예약 — 12B 정렬 맞춤. */
};

#pragma pack(push, 1)
/* [한국어] 다음 구조체에 한해 컴파일러의 자연 정렬 패딩을 끄고 1바이트 정렬 강제.
 * spdk_nvme_intel_smart_attribute(12B) 13개를 빈틈없이 156B로 패킹하기 위해 필요.
 * 이전 패킹 설정은 #pragma pack(pop)으로 복원. */

struct spdk_nvme_intel_smart_information_page {
	/* [한국어] LID 0xCA 응답 페이로드(packed, 156B) — Intel 확장 SMART 13개 attribute.
	 * 설정자: 펌웨어. 읽는 자: 사용자가 13개 슬롯을 순회하며 code 분기 처리.
	 * 동기화: snapshot 의미 — 펌웨어가 atomic하게 채워 응답. */

	struct spdk_nvme_intel_smart_attribute	attributes[13];
	/* [한국어] 13개의 12바이트 attribute. 인덱스 자체에는 의미가 없고 code 필드로 식별.
	 * 펌웨어는 모든 13슬롯을 채우며, 정의되지 않은 슬롯은 code=0으로 보낼 수 있다. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_intel_smart_information_page) == 156, "Incorrect size");
/* [한국어] 12 * 13 = 156. packed 적용으로 패딩 없이 정확히 156B. */
#pragma pack(pop)
/* [한국어] 이전 컴파일러 패킹 설정 복원 — 이후 구조체는 다시 자연 정렬 사용. */

union spdk_nvme_intel_feat_power_governor {
	/* [한국어] FID 0xC6 (POWER_GOVERNOR_SETTING)의 CDW11 인터프리터 (32비트 union).
	 * 설정자: 호스트가 Set Feature 호출 시 bits.power_governor_setting을 0/1/2로 설정.
	 * 읽는 자: 펌웨어가 raw 값을 해석하거나, Get Feature 응답에서 호스트가 bits로 디코딩.
	 * 동기화: 단일 32비트 워드 단위로 기록되므로 atomic. */

	uint32_t	raw;
	/* [한국어] 32비트 통째로 접근하기 위한 별칭 — Set/Get Feature CDW11에 직접 대입. */

	struct {
		/** power governor setting : 00h = 25W 01h = 20W 02h = 10W */
		uint32_t power_governor_setting		: 8;
		/* [한국어] 하위 8비트로 전력 캡 모드 선택.
		 * 00h = 25W (성능 최대), 01h = 20W (균형), 02h = 10W (저전력 모드).
		 * 데이터센터 SSD에서 PSU/thermal 제약에 맞춰 운용. */

		uint32_t reserved	: 24;
		/* [한국어] 상위 24비트 예약 — 호스트는 0으로 설정해야 함(미설정 시 정의 행동 없음). */
	} bits;
	/* [한국어] 비트필드로 디코딩한 뷰 — raw와 같은 메모리를 다른 시각으로 보는 union 멤버. */
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_intel_feat_power_governor) == 4, "Incorrect size");
/* [한국어] CDW11은 정확히 4바이트(32비트). union 크기 검증. */

union spdk_nvme_intel_feat_smbus_address {
	/* [한국어] FID 0xC8 (SMBUS_ADDRESS) CDW11 인터프리터.
	 * 설정자: 호스트가 BMC와 협의된 SMBus 슬레이브 주소를 기록.
	 * 읽는 자: SSD 펌웨어가 SMBus 인터페이스에 적용. */

	uint32_t	raw;
	/* [한국어] CDW11 raw 32비트. */

	struct {
		uint32_t reserved	: 1;
		/* [한국어] LSB 1비트 예약 — SMBus 주소가 7비트라 LSB는 R/W bit로 쓰이지 않도록 비움. */

		uint32_t smbus_controller_address	: 8;
		/* [한국어] 8비트 SMBus 슬레이브 주소(7비트 주소 + 1비트 패딩 형태).
		 * BMC가 NVMe-MI 명령으로 이 SSD를 어드레싱할 때 사용. */

		uint32_t reserved2	: 23;
		/* [한국어] 상위 23비트 예약 — 0으로 기록. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_intel_feat_smbus_address) == 4, "Incorrect size");
/* [한국어] 32비트 크기 검증. */

union spdk_nvme_intel_feat_led_pattern {
	/* [한국어] FID 0xC9 (LED_PATTERN) CDW11 인터프리터.
	 * 설정자: 데이터센터 운영자가 베이의 드라이브를 식별하기 위해 블링크 패턴 명령.
	 * 읽는 자: SSD 펌웨어가 식별 LED 컨트롤러에 패턴 적용. */

	uint32_t	raw;
	/* [한국어] CDW11 raw 32비트. */

	struct {
		uint32_t feature_options	: 24;
		/* [한국어] 하위 24비트 — 패턴 옵션 비트맵 (블링크 주기, 색상 등 펌웨어 정의). */

		uint32_t value	: 8;
		/* [한국어] 상위 8비트 — 패턴 강도/모드 enum (0=off, 1=solid, 2=blink 등). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_intel_feat_led_pattern) == 4, "Incorrect size");
/* [한국어] 32비트 크기 검증. */

union spdk_nvme_intel_feat_reset_timed_workload_counters {
	/* [한국어] FID 0xD5 (RESET_TIMED_WORKLOAD_COUNTERS) CDW11.
	 * Set 전용 — 호스트가 timed counter (E2 media wear, E3 host read %, E4 timer)를 리셋.
	 * Get은 미지원이라 응답에 의미 없음.
	 * 설정자: 호스트(주기적 SMART 베이스라인 리셋 용도). 읽는 자: SSD 펌웨어 리셋 트리거. */

	uint32_t	raw;
	/* [한국어] CDW11 raw 32비트. */

	struct {
		/**
		 * Write Usage: 00 = NOP, 1 = Reset E2, E3,E4 counters;
		 * Read Usage: Not Supported
		 */
		uint32_t reset	: 1;
		/* [한국어] 1비트 — 0=동작 없음, 1=E2/E3/E4 카운터 리셋 실행.
		 * 한 번 1로 Set Feature 발행하면 즉시 클리어되며 펌웨어가 자동으로 0으로 되돌리지 않음. */

		uint32_t reserved	: 31;
		/* [한국어] 상위 31비트 예약 — 0으로 기록. */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_intel_feat_reset_timed_workload_counters) == 4,
		   "Incorrect size");
/* [한국어] 32비트 크기 검증. */

union spdk_nvme_intel_feat_latency_tracking {
	/* [한국어] FID 0xE2 (LATENCY_TRACKING) CDW11.
	 * 설정자: 호스트가 latency histogram(LID 0xC1/0xC2) 수집을 enable/disable.
	 * 읽는 자: SSD 펌웨어가 내부 통계 수집 토글. */

	uint32_t	raw;
	/* [한국어] CDW11 raw 32비트. */

	struct {
		/**
		 * Write Usage:
		 * 00h = Disable Latency Tracking (Default)
		 * 01h = Enable Latency Tracking
		 */
		uint32_t enable	: 32;
		/* [한국어] 32비트 enable 플래그 — 0=비활성(기본), 0x01=활성.
		 * 다른 값은 펌웨어 정의 행동(향후 확장용). */
	} bits;
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_intel_feat_latency_tracking) == 4, "Incorrect size");
/* [한국어] 32비트 크기 검증. */

struct spdk_nvme_intel_marketing_description_page {
	/* [한국어] LID 0xDD (Marketing Description) 응답 페이로드.
	 * 설정자: SSD 펌웨어가 ASCII 제품명을 channel write.
	 * 읽는 자: examples/nvme/identify가 사용자에게 출력 (예: "INTEL SSDPE2KX040T8"). */

	uint8_t		marketing_product[512];
	/* [한국어] ASCII 제품 마케팅 문자열 — Intel 스펙상 정확히 512B만 의미 있음.
	 * 설정자: 펌웨어. 읽는 자: NULL-terminate 보장 없으므로 호스트가 strncpy/strnlen 사용 권장. */

	/* Spec says this log page will only write 512 bytes, but there are some older FW
	 * versions that accidentally write 516 instead.  So just pad this out to 4096 bytes
	 * to make sure users of this structure never end up overwriting unintended parts of
	 * memory.
	 */
	uint8_t		reserved[3584];
	/* [한국어] 3584B 패딩 — 펌웨어 버그(516B 기록) 방어용 안전 영역.
	 * 일부 구버전 펌웨어가 스펙을 어기고 512+4=516B를 기록하므로, 호스트 버퍼를
	 * 정확히 512B로 잡으면 stack/heap overflow 위험. 그래서 4096B(0xDD 페이지의
	 * NVMe 메모리 페이지 정렬 단위)로 패딩해 안전을 확보한다. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_intel_marketing_description_page) == 4096,
		   "Incorrect size");
/* [한국어] 512 + 3584 = 4096 (1 페이지). 크기 검증. */
#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 종료 — C++ 컴파일러 사용 시. */
#endif

#endif
/* [한국어] SPDK_NVME_INTEL_H 가드 종료. */
