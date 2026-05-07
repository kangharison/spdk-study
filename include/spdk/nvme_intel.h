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
	/* [한국어] FID 0xC1 — 컨트롤러가 호스트에 노출하는 사용자 LBA 최대값(over-provisioning) 설정/조회.
	 * 역할: Set 시 CDW11에 새 max LBA를 인코딩해 SSD의 사용자 가시 용량을 줄여
	 *       NAND 여유분(spare)을 늘림으로써 WAF(Write Amplification Factor)와 P/E 수명 향상.
	 * 설정자: 호스트가 spdk_nvme_ctrlr_cmd_set_feature(ctrlr, 0xC1, cdw11=newMaxLba, ...) 호출.
	 *         또는 Get Feature 시에는 호스트가 LID로만 사용하고 응답 CDW0에 현재 값을 받음.
	 * 읽는 자: SSD 펌웨어가 FID를 디스패치해 namespace/translation layer 재구성.
	 *          호스트는 Get Feature 응답 CDW0에서 현재 max LBA 조회.
	 * 값 범위: 0보다 크고 native max LBA 이하. 너무 작으면 status code 0xC1 반환.
	 * 동기화: Admin SQ/CQ는 단일 SPDK 스레드 소유 — 별도 락 불필요.
	 *         실패 시 spdk_nvme_intel_set_max_lba_command_status_code 참조. */

	SPDK_NVME_INTEL_FEAT_NATIVE_MAX_LBA			= 0xC2,
	/* [한국어] FID 0xC2 — SSD의 물리적 native max LBA (공장 출하 시 정해진 LBA 상한) 조회 전용.
	 * 역할: MAX_LBA(0xC1)로 over-provisioning을 적용한 상태에서도 원래 한계를 알 수 있어
	 *       관리 도구가 max LBA 복구 또는 검증 시 활용한다.
	 * 설정자: SSD 펌웨어가 read-only 값을 채워 응답 CDW0로 반환.
	 * 읽는 자: 호스트가 Get Feature 응답에서 native max LBA 추출, 상한 검증에 사용.
	 * 값 범위: 펌웨어가 광고하는 절대 상한 — 변경 불가.
	 * 동기화: read-only이므로 캐싱 가능 (컨트롤러 초기화 후 변경되지 않음). */

	SPDK_NVME_INTEL_FEAT_POWER_GOVERNOR_SETTING		= 0xC6,
	/* [한국어] FID 0xC6 — 전력 거버너(thermal/power cap) 설정.
	 * 역할: SSD의 평균 전력 소비 상한을 펌웨어 모드로 제한해 thermal 또는 PSU 제약을 만족.
	 * 설정자: 호스트가 union spdk_nvme_intel_feat_power_governor::bits.power_governor_setting을
	 *         00h(25W)/01h(20W)/02h(10W) 중 하나로 채워 Set Feature 발행.
	 * 읽는 자: SSD 펌웨어가 controller-internal P-state limiter에 적용. Get Feature는 현재 모드 조회.
	 * 값 범위: 8비트, 정의된 값 00h/01h/02h만 유효. 그 외는 펌웨어 정의(거부 가능).
	 * 동기화: 단일 32비트 워드 단위 atomic 기록 — 여러 호스트 발행 경쟁은 펌웨어 큐잉으로 직렬화. */

	SPDK_NVME_INTEL_FEAT_SMBUS_ADDRESS			= 0xC8,
	/* [한국어] FID 0xC8 — SSD가 out-of-band SMBus(System Management Bus) 인터페이스에서
	 *           응답할 7비트 슬레이브 주소 설정/조회.
	 * 역할: 데이터센터에서 BMC(Baseboard Management Controller)가 NVMe-MI(Management Interface)로
	 *       SSD를 폴링/이벤트 수신할 때 BMC가 어드레싱할 주소를 호스트가 미리 등록.
	 * 설정자: 호스트가 union spdk_nvme_intel_feat_smbus_address::bits.smbus_controller_address에
	 *         BMC와 협의된 8비트 주소(7비트 + 1비트 패딩)를 기록.
	 * 읽는 자: SSD 펌웨어가 SMBus 컨트롤러 IP에 주소 적용. BMC가 이후 그 주소로 트랜잭션 전송.
	 * 값 범위: 표준 SMBus 7비트 주소 영역(0x08~0x77 등) — Intel 스펙 페이지 참조.
	 * 동기화: 한 번 부팅 후 고정되는 설정 — race 없음. 변경 시 펌웨어가 다음 트랜잭션부터 적용. */

	SPDK_NVME_INTEL_FEAT_LED_PATTERN			= 0xC9,
	/* [한국어] FID 0xC9 — 드라이브 베이의 식별 LED 점등 패턴(블링크/솔리드/오프) 설정/조회.
	 * 역할: 다수 SSD가 장착된 베이에서 특정 디바이스를 시각적으로 식별 (장애 교체, 구성 검증 등).
	 * 설정자: 운영자/스크립트가 union spdk_nvme_intel_feat_led_pattern::bits.value/feature_options에
	 *         패턴 코드를 채워 Set Feature 발행.
	 * 읽는 자: SSD 펌웨어가 LED PWM 컨트롤러에 패턴 적용 — 인접 베이 활성 신호와 다중화 가능.
	 * 값 범위: feature_options(24비트)는 펌웨어 정의 비트맵, value(8비트)는 모드 enum (0=off 등).
	 * 동기화: PWM 적용은 펌웨어 비동기 처리 — Set Feature 완료 시점과 LED 변화 시점은 불일치 가능. */

	SPDK_NVME_INTEL_FEAT_RESET_TIMED_WORKLOAD_COUNTERS	= 0xD5,
	/* [한국어] FID 0xD5 — 시간 기반 SMART 카운터(0xE2 media wear, 0xE3 host read %, 0xE4 timer) 리셋.
	 * 역할: 호스트가 새 워크로드 측정 베이스라인을 잡기 위해 누적 카운터를 0으로 클리어.
	 * 설정자: 호스트가 union spdk_nvme_intel_feat_reset_timed_workload_counters::bits.reset=1로
	 *         Set Feature 발행. Get은 미지원.
	 * 읽는 자: SSD 펌웨어가 SMART 페이지(LID 0xCA)의 해당 attribute raw_value를 0으로 초기화.
	 * 값 범위: bits.reset 1비트만 의미 — 나머지 31비트는 reserved=0.
	 * 동기화: 펌웨어가 즉시 atomic 클리어. 카운터 갱신은 펌웨어 백그라운드 — 호스트는 다음 SMART 응답에서 반영 확인. */

	SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING			= 0xE2,
	/* [한국어] FID 0xE2 — read/write latency histogram(LID 0xC1/0xC2) 수집 enable/disable.
	 * 역할: 펌웨어 내부 latency 통계 수집기 토글 — 비활성 시 LID 0xC1/0xC2 응답은 stale/0.
	 * 설정자: 호스트가 union spdk_nvme_intel_feat_latency_tracking::bits.enable에
	 *         0(disable, 기본) 또는 1(enable) 기록 후 Set Feature 발행.
	 * 읽는 자: SSD 펌웨어가 I/O completion path의 timestamp 측정·버킷 누적 토글.
	 *          이후 호스트가 LID 0xC1(read)/0xC2(write)로 spdk_nvme_intel_rw_latency_page 회수.
	 * 값 범위: 32비트 enable 필드 — 0/1 외 값은 펌웨어 정의(향후 확장용).
	 * 동기화: 토글은 atomic 32비트 쓰기 — 변경 시점 직전·직후 일부 I/O 통계는 누락/중복 가능. */
};

enum spdk_nvme_intel_set_max_lba_command_status_code {
	/* [한국어] FID 0xC1 (MAX_LBA) Set 명령이 실패할 때 NVMe 완료 큐 엔트리(CQE) Status Field의
	 * Status Code 영역(SCT=Vendor Specific=0x7)에 반환되는 Intel 정의 코드.
	 * 설정자: SSD 펌웨어가 거부 사유에 따라 채움.
	 * 읽는 자: 호스트가 spdk_nvme_cpl::status.sc 값을 이 enum과 비교해 사용자에게 메시지 출력. */

	SPDK_NVME_INTEL_EXCEEDS_AVAILABLE_CAPACITY		= 0xC0,
	/* [한국어] SC 0xC0 — 요청한 max LBA가 드라이브의 가용 NAND 물리 용량을 초과.
	 * 역할: 사용자가 over-provisioning 모드에서 *증가* 방향으로 max LBA를 늘리려 했으나
	 *       NAND 칩이 광고하는 raw 용량보다 큰 값을 요청한 경우.
	 * 설정자: SSD 펌웨어가 CQE의 status.sc에 채움. SCT는 7(Vendor Specific).
	 * 읽는 자: 호스트 콜백이 spdk_nvme_cpl::status.sc와 비교해 사용자에게 "용량 초과" 보고.
	 * 값 범위: 0xC0 (8비트 status code field).
	 * 동기화: 단일 명령에 대한 일회성 응답 — 동기화 이슈 없음. */

	SPDK_NVME_INTEL_SMALLER_THAN_MIN_LIMIT			= 0xC1,
	/* [한국어] SC 0xC1 — 요청 값이 펌웨어가 허용하는 최소 max LBA(예: 80% over-provisioning) 미만.
	 * 역할: 펌웨어 정책상 너무 작은 max LBA는 정상 동작 보장 불가하므로 거부.
	 * 설정자: SSD 펌웨어 거부 응답.
	 * 읽는 자: 호스트가 사용자에게 "최소 한도 미만" 메시지 출력 후 더 큰 값으로 재시도 안내.
	 * 값 범위: 0xC1.
	 * 동기화: 일회성 응답. */

	SPDK_NVME_INTEL_SMALLER_THAN_NS_REQUIREMENTS		= 0xC2,
	/* [한국어] SC 0xC2 — 요청 값이 이미 생성된 namespace들의 LBA 합계를 수용하지 못함.
	 * 역할: namespace가 이미 데이터를 보유하므로 max LBA를 그 합계 미만으로 줄이면 데이터 유실 — 거부.
	 * 설정자: SSD 펌웨어 거부 응답.
	 * 읽는 자: 호스트가 "namespace 삭제 후 재시도" 안내 출력.
	 * 값 범위: 0xC2.
	 * 동기화: 일회성 응답. */
};

enum spdk_nvme_intel_log_page {
	/* [한국어] Intel Get Log Page (NVMe opcode 0x02)의 CDW10[LID] 필드 값 모음.
	 * 0xC0~0xFF는 vendor-specific. 각 LID는 고유한 페이지 크기와 레이아웃을 가진다.
	 * 설정자: 사용자가 spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, LID, ...) 호출 시 인자.
	 * 읽는 자: lib/nvme/nvme_ctrlr.c의 set_intel_supported_log_pages()는
	 * Intel VID(0x8086) 컨트롤러에 한해 이 LID들을 지원 비트맵에 등록한다. */

	SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY			= 0xC0,
	/* [한국어] LID 0xC0 — 어떤 Intel vendor LID가 현재 펌웨어에서 지원되는지를 길이 0/non-zero로
	 *           표시한 인덱스(디렉토리) 페이지.
	 * 역할: 컨트롤러 초기화 시 호스트가 가장 먼저 조회 — 이후 다른 0xCx/0xDx LID 사용 가능 여부 결정.
	 * 설정자: SSD 펌웨어가 응답 DMA로 struct spdk_nvme_intel_log_page_directory(512B) 채움.
	 * 읽는 자: lib/nvme/nvme_ctrlr.c::nvme_ctrlr_set_intel_supported_log_pages()가
	 *          이 페이지를 파싱해 ctrlr->log_page_supported 비트맵 갱신.
	 * 값 범위: 8비트 LID — 0xC0 고정.
	 * 동기화: 초기화 시 1회 회수 후 read-only 캐시. */

	SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY			= 0xC1,
	/* [한국어] LID 0xC1 — 읽기 명령 latency histogram (32us/1ms/32ms 폭 버킷, 380B).
	 * 역할: 호스트가 read latency 분포(p50/p99 등) 분석을 위해 회수.
	 * 설정자: 펌웨어 (FEAT_LATENCY_TRACKING(0xE2)이 enable일 때만 유효한 값 채움).
	 * 읽는 자: 호스트가 struct spdk_nvme_intel_rw_latency_page로 캐스팅해 버킷 순회.
	 * 값 범위: 8비트 LID — 0xC1 고정. tracking 비활성 시 응답은 stale/0.
	 * 동기화: 펌웨어 atomic snapshot — 호스트는 별도 락 불필요. */

	SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY			= 0xC2,
	/* [한국어] LID 0xC2 — 쓰기 명령 latency histogram. 페이로드 구조는 LID 0xC1과 동일.
	 * 역할: write 경로의 latency 분포 분석.
	 * 설정자/읽는 자/동기화: LID 0xC1과 동일.
	 * 값 범위: 8비트 LID — 0xC2 고정. */

	SPDK_NVME_INTEL_LOG_TEMPERATURE				= 0xC5,
	/* [한국어] LID 0xC5 — 온도 통계 페이지(112B). struct spdk_nvme_intel_temperature_page.
	 * 역할: 일반 SMART/Health(LID 0x02)에 없는 historical min/max, shutdown 카운트, 센서 오프셋 노출.
	 * 설정자: 펌웨어가 thermal sensor 폴링 결과를 누적해 응답.
	 * 읽는 자: 모니터링 도구가 K(Kelvin) 단위 온도를 °C로 변환하여 표시.
	 * 값 범위: 8비트 LID — 0xC5 고정.
	 * 동기화: 폴링 카운터 — 호스트 무락 read OK. */

	SPDK_NVME_INTEL_LOG_SMART				= 0xCA,
	/* [한국어] LID 0xCA — Intel 확장 SMART(156B 패킹) — 일반 NVMe SMART(LID 0x02)와 다름!
	 * 역할: NAND wear, host/NAND bytes written, thermal throttle 등 13개 attribute를 제공.
	 * 설정자: 펌웨어. 읽는 자: struct spdk_nvme_intel_smart_information_page::attributes[13] 순회.
	 * 값 범위: 8비트 LID — 0xCA 고정.
	 * 동기화: snapshot — atomic 응답. */

	SPDK_NVME_INTEL_MARKETING_DESCRIPTION			= 0xDD,
	/* [한국어] LID 0xDD — 제품 마케팅 문자열(4096B 페이지, 의미 있는 영역은 처음 512B).
	 * 역할: 호스트 도구가 SSD 모델명을 사용자 친화적 ASCII로 표시 (예: "INTEL SSDPE2KX040T8").
	 * 설정자: 펌웨어가 ASCII 문자열을 marketing_product[512]에 채움.
	 * 읽는 자: examples/nvme/identify 등 사용자 도구 — strncpy/strnlen 사용 권장(NULL-terminate 미보장).
	 * 값 범위: 8비트 LID — 0xDD 고정.
	 * 동기화: read-only — 일생 동안 변경되지 않음. */
};

enum spdk_nvme_intel_smart_attribute_code {
	/* [한국어] Intel 확장 SMART 페이지(LID 0xCA) 내부의 각 attribute 슬롯에 들어가는 code 값.
	 * struct spdk_nvme_intel_smart_attribute::code 필드와 매칭.
	 * 설정자: SSD 펌웨어가 각 attribute 슬롯에 코드+normalized+raw를 채워 응답.
	 * 읽는 자: 호스트가 13개 attribute를 순회하며 코드별 분기해 raw_value[6]를 해석. */

	SPDK_NVME_INTEL_SMART_PROGRAM_FAIL_COUNT		= 0xAB,
	/* [한국어] 0xAB — NAND program(쓰기 단계) 실패 누적 횟수.
	 * 역할: NAND 셀 결함/수명 임박 지표 — 증가가 가팔라지면 교체 시기 시그널.
	 * 설정자: SSD 펌웨어가 program 단계 실패 시 ECC 회복 후 카운터 증가.
	 * 읽는 자: 호스트 모니터링이 raw_value[6] little-endian 카운터로 해석.
	 * 값 범위: 0 이상 단조 증가 (FID 0xD5로 리셋 가능 여부는 펌웨어 정의).
	 * 동기화: snapshot snapshot — atomic 응답. */

	SPDK_NVME_INTEL_SMART_ERASE_FAIL_COUNT			= 0xAC,
	/* [한국어] 0xAC — NAND erase(블록 소거 단계) 실패 누적 횟수.
	 * 역할: 블록이 wear out에 가까워질수록 erase 실패가 증가 → 수명 임박 지표.
	 * 설정자: 펌웨어. 읽는 자: 호스트 모니터링.
	 * 값 범위: 0 이상 단조 증가. 동기화: atomic snapshot. */

	SPDK_NVME_INTEL_SMART_WEAR_LEVELING_COUNT		= 0xAD,
	/* [한국어] 0xAD — wear leveling 카운트 (P/E cycle 분포 통계: min/avg/max).
	 * 역할: NAND 블록 간 마모 균등화 정도 — 차이가 크면 wear leveling 알고리즘 미흡.
	 * 설정자: 펌웨어가 P/E cycle 통계를 raw_value 6바이트에 인코딩.
	 * 읽는 자: 호스트가 min/avg/max 분리 해석 (인코딩은 Intel 스펙 페이지 참조).
	 * 값 범위: 0 이상. 동기화: snapshot. */

	SPDK_NVME_INTEL_SMART_E2E_ERROR_COUNT			= 0xB8,
	/* [한국어] 0xB8 — End-to-End(호스트~NAND) CRC/ECC 보호로 검출된 데이터 경로 오류 누적.
	 * 역할: PI(Protection Information)와 함께 데이터 무결성 검증 결과 — 0이 정상.
	 * 설정자: 펌웨어가 데이터 경로 모든 stage(DRAM, controller, NAND interface)에서 mismatch 누적.
	 * 읽는 자: 호스트 모니터링이 silent corruption 가능성 평가.
	 * 값 범위: 0 이상 단조 증가. 동기화: snapshot. */

	SPDK_NVME_INTEL_SMART_CRC_ERROR_COUNT			= 0xC7,
	/* [한국어] 0xC7 — PCIe/NVMe 링크 계층에서 발생한 CRC 오류(LCRC/ECRC) 누적.
	 * 역할: PCIe 링크 신호 무결성 지표 — 케이블/슬롯 노이즈, signal integrity 문제 진단.
	 * 설정자: 펌웨어가 PCIe AER(Advanced Error Reporting) 카운터를 SMART에 노출.
	 * 읽는 자: 호스트 모니터링.
	 * 값 범위: 0 이상. 동기화: snapshot. */

	SPDK_NVME_INTEL_SMART_MEDIA_WEAR			= 0xE2,
	/* [한국어] 0xE2 — NAND 매체 마모율 (timed counter — 측정 기간 동안의 마모량).
	 * 역할: 평균 워크로드 강도 측정 — 0xE4 timer로 정규화 시 시간당 마모율 산출.
	 * 설정자: 펌웨어가 측정 기간 동안 누적 P/E delta를 raw_value에 인코딩.
	 * 읽는 자: 호스트가 FID 0xD5(RESET_TIMED_WORKLOAD_COUNTERS)로 베이스라인 리셋 후 측정.
	 * 값 범위: 정규화된 정수 — 펌웨어 정의 단위.
	 * 동기화: snapshot. timed counter는 0xD5로 호스트 트리거 리셋 가능. */

	SPDK_NVME_INTEL_SMART_HOST_READ_PERCENTAGE		= 0xE3,
	/* [한국어] 0xE3 — 측정 기간 동안 전체 I/O 중 host read가 차지한 비율(0~100).
	 * 역할: 워크로드 R/W 패턴 추정 — read-heavy vs write-heavy 분류.
	 * 설정자: 펌웨어가 timed window 동안 read/write count 비율 계산.
	 * 읽는 자: 호스트 모니터링.
	 * 값 범위: 0~100. 동기화: snapshot, FID 0xD5로 리셋. */

	SPDK_NVME_INTEL_SMART_TIMER				= 0xE4,
	/* [한국어] 0xE4 — 위 timed counter(0xE2/0xE3)들이 쌓인 시간(분 단위).
	 * 역할: 측정 기간 정규화에 사용 — wear/percentage / timer = 시간당 비율.
	 * 설정자: 펌웨어가 매분 +1.
	 * 읽는 자: 호스트 모니터링.
	 * 값 범위: 0 이상 — FID 0xD5로 0으로 리셋. 동기화: snapshot. */

	SPDK_NVME_INTEL_SMART_THERMAL_THROTTLE_STATUS		= 0xEA,
	/* [한국어] 0xEA — 현재 thermal throttle 활성 플래그 + 누적 throttle 시간.
	 * 역할: 발열 제한이 성능에 미친 영향 측정 — 누적 시간이 크면 cooling 보강 필요.
	 * 설정자: 펌웨어가 thermal sensor가 임계 초과할 때마다 throttle 진입 시간 누적.
	 * 읽는 자: 호스트 모니터링.
	 * 값 범위: raw_value[0]=현재 throttle %, raw_value[1..]=누적 시간 (정확한 인코딩은 펌웨어 정의).
	 * 동기화: snapshot. */

	SPDK_NVME_INTEL_SMART_RETRY_BUFFER_OVERFLOW_COUNTER	= 0xF0,
	/* [한국어] 0xF0 — 내부 retry buffer overflow 발생 횟수.
	 * 역할: NAND 재시도(read retry) 누적이 펌웨어 버퍼를 초과한 사건 수 — NAND 마모/품질 압박 지표.
	 * 설정자: 펌웨어. 읽는 자: 호스트 모니터링.
	 * 값 범위: 0 이상 단조 증가. 동기화: snapshot. */

	SPDK_NVME_INTEL_SMART_PLL_LOCK_LOSS_COUNT		= 0xF3,
	/* [한국어] 0xF3 — PCIe PHY PLL(Phase-Locked Loop) lock loss 발생 횟수.
	 * 역할: PCIe 클럭/링크 안정성 지표 — 슬롯 신호 품질 문제 진단.
	 * 설정자: 펌웨어가 PHY 인터럽트로 lock loss 이벤트 카운트.
	 * 읽는 자: 호스트 모니터링.
	 * 값 범위: 0 이상 단조 증가. 동기화: snapshot. */

	SPDK_NVME_INTEL_SMART_NAND_BYTES_WRITTEN		= 0xF4,
	/* [한국어] 0xF4 — NAND에 실제로 program된 누적 바이트 (펌웨어 GC/wear leveling 트래픽 포함).
	 * 역할: WAF(Write Amplification Factor) 계산의 분자 — WAF = NAND_BYTES / HOST_BYTES.
	 * 설정자: 펌웨어가 NAND 인터페이스로 흘러간 모든 바이트 누적.
	 * 읽는 자: 호스트가 32MiB 단위 등 펌웨어 정의 단위로 raw_value 디코딩.
	 * 값 범위: 0 이상 단조 증가. 동기화: snapshot. */

	SPDK_NVME_INTEL_SMART_HOST_BYTES_WRITTEN		= 0xF5,
	/* [한국어] 0xF5 — 호스트가 발행한 누적 쓰기 바이트 (사용자 워크로드만, GC 미포함).
	 * 역할: WAF의 분모 — WAF = NAND_BYTES_WRITTEN / HOST_BYTES_WRITTEN.
	 *       정상 SSD에서 WAF는 워크로드별 1.x ~ 수 배 사이.
	 * 설정자: 펌웨어가 모든 host write 명령의 데이터 길이 합산.
	 * 읽는 자: 호스트가 raw_value를 펌웨어 정의 단위(예: 32MiB)로 곱해 바이트 환산.
	 * 값 범위: 0 이상 단조 증가. 동기화: snapshot. */
};

struct spdk_nvme_intel_log_page_directory {
	/* [한국어] LID 0xC0 (Log Page Directory) 응답 페이로드의 와이어 포맷.
	 * 펌웨어가 어떤 Intel LID를 지원하는지 길이로 보고하는 인덱스 페이지(512B 고정).
	 * 설정자: SSD 펌웨어가 Get Log Page(LID=0xC0) 응답으로 DMA 채움.
	 * 읽는 자: lib/nvme/nvme_ctrlr.c::set_intel_supported_log_pages()가 각 *_log_len을 검사해
	 *   0이 아닌 항목만 ctrlr->log_page_supported 비트맵에 등록.
	 * 동기화: 컨트롤러 초기화 단계에서 한 번만 채워지고 이후 read-only로 사용. */

	uint8_t		version[2];
	/* [한국어] 디렉토리 페이지 포맷의 메이저/마이너 버전 (Intel 정의, 와이어 오프셋 0~1).
	 * 역할: 펌웨어가 향후 새 LID/필드를 추가했을 때 호스트의 해석 가능 여부 판단.
	 * 설정자: SSD 펌웨어가 응답 DMA 시 채움.
	 * 읽는 자: 호스트가 자신이 알고 있는 디렉토리 포맷 버전과 비교하여 backward-compat 판정.
	 * 값 범위: version[0]=major, version[1]=minor. Intel 펌웨어 릴리스에 따라 단조 증가.
	 * 동기화: 응답 페이로드는 read-only — 호스트는 캐시 후 재참조 가능. */

	uint8_t		reserved[384];
	/* [한국어] 향후 확장을 위한 예약 영역 (와이어 오프셋 2~385, 384바이트).
	 * 역할: Intel이 새 길이 필드를 디렉토리에 추가할 슬롯 — 현재 펌웨어는 0으로 채움.
	 * 설정자: 펌웨어가 0으로 zero-fill.
	 * 읽는 자: 호스트는 0 검증 없이 무시 (forward-compat 차원에서 비-제로 허용).
	 * 값 범위: 0x00 (정의되지 않음).
	 * 동기화: read-only — 의미 없음. */

	uint8_t		read_latency_log_len;
	/* [한국어] LID 0xC1 (Read Latency) 페이지의 지원 여부/길이 (와이어 오프셋 386).
	 * 역할: 호스트가 LID 0xC1을 발행해도 안전한지 사전 점검.
	 * 설정자: 펌웨어가 0(미지원) 또는 페이지 길이 인코딩 값 채움.
	 * 읽는 자: lib/nvme/nvme_ctrlr.c::set_intel_supported_log_pages()가
	 *          이 값이 비-0이면 ctrlr->log_page_supported[0xC1] 비트 셋.
	 * 값 범위: 0=미지원, non-zero=지원 (Intel 정의 단위, 보통 페이지 크기/4 등).
	 * 동기화: 부팅 후 read-only. */

	uint8_t		reserved2;
	/* [한국어] 길이 필드 사이의 패딩 1바이트 (와이어 오프셋 387).
	 * 역할: 와이어 포맷 정렬 패딩. 설정자: 펌웨어 0으로 채움.
	 * 읽는 자: 호스트 무시. 값 범위: 0x00. 동기화: 무관. */

	uint8_t		write_latency_log_len;
	/* [한국어] LID 0xC2 (Write Latency) 페이지의 지원 여부/길이 (와이어 오프셋 388).
	 * 역할/설정자/읽는 자: read_latency_log_len과 동일하나 LID 0xC2 대상.
	 * 값 범위: 0=미지원, non-zero=지원.
	 * 동기화: 부팅 후 read-only. */

	uint8_t		reserved3[5];
	/* [한국어] 다음 길이 필드(0xC5)까지의 5바이트 패딩 (와이어 오프셋 389~393).
	 * 역할: LID 0xC2 다음 LID 0xC5 사이 미정의 LID(0xC3, 0xC4)를 위한 슬롯 예약.
	 * 설정자: 펌웨어 0. 읽는 자: 무시. 값 범위: 0x00. 동기화: 무관. */

	uint8_t		temperature_statistics_log_len;
	/* [한국어] LID 0xC5 (Temperature Statistics) 페이지의 지원 여부/길이 (와이어 오프셋 394).
	 * 역할: 호스트가 LID 0xC5 발행 가능 여부 판단.
	 * 설정자: 펌웨어. 읽는 자: 호스트가 비-0이면 spdk_nvme_intel_temperature_page 회수 가능.
	 * 값 범위: 0=미지원, non-zero=지원. 동기화: 부팅 후 read-only. */

	uint8_t		reserved4[9];
	/* [한국어] LID 0xC5 다음 슬롯부터 LID 0xCA까지의 9바이트 패딩 (와이어 오프셋 395~403).
	 * 역할: LID 0xC6~0xC9 슬롯(미사용 LID 또는 향후 확장)을 채우는 예약 패딩.
	 * 설정자: 펌웨어 0. 읽는 자: 무시. 값 범위: 0x00. 동기화: 무관. */

	uint8_t		smart_log_len;
	/* [한국어] LID 0xCA (Intel Extended SMART) 페이지의 지원 여부/길이 (와이어 오프셋 404).
	 * 역할: Intel 확장 SMART(13개 attribute) 회수 가능 여부.
	 * 주의: 일반 NVMe SMART/Health(LID 0x02)와 다른 별개 페이지 — 혼동 금지.
	 * 설정자: 펌웨어. 읽는 자: 호스트가 비-0이면 spdk_nvme_intel_smart_information_page 회수.
	 * 값 범위: 0=미지원, non-zero=지원. 동기화: 부팅 후 read-only. */

	uint8_t		reserved5[37];
	/* [한국어] LID 0xCA 다음부터 LID 0xDD 슬롯까지의 37바이트 패딩 (와이어 오프셋 405~441).
	 * 역할: LID 0xCB~0xDC 슬롯 예약. 설정자: 펌웨어 0. 읽는 자: 무시.
	 * 값 범위: 0x00. 동기화: 무관. */

	uint8_t		marketing_description_log_len;
	/* [한국어] LID 0xDD (Marketing Description) 페이지의 지원 여부/길이 (와이어 오프셋 442).
	 * 역할: 호스트가 LID 0xDD로 제품 마케팅 문자열 회수 가능 여부 점검.
	 * 설정자: 펌웨어. 읽는 자: examples/nvme/identify가 비-0일 때만 출력 시도.
	 * 값 범위: 0=미지원, non-zero=지원 (정상 펌웨어는 4096B 페이지 광고).
	 * 동기화: 부팅 후 read-only. */

	uint8_t		reserved6[69];
	/* [한국어] 페이지 끝까지의 패딩 — 전체 sizeof()를 정확히 512B로 맞춤 (와이어 오프셋 443~511).
	 * 역할: NVMe 스펙이 디렉토리 페이지를 512B로 고정해서 wire 전송 길이 일치시킴.
	 * 설정자: 펌웨어 0. 읽는 자: 무시. 값 범위: 0x00. 동기화: 무관. */
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
	/* [한국어] 페이지 포맷 메이저 버전 (와이어 오프셋 0~1, 16비트 little-endian).
	 * 주의: "revison"은 Intel 스펙 원문 오타이며 SPDK는 와이어 호환을 위해 그대로 보존.
	 * 역할: 호스트가 페이지 레이아웃이 자신의 코드 버전과 호환되는지 검증.
	 * 설정자: SSD 펌웨어가 응답 DMA 시 채움.
	 * 읽는 자: 호스트 도구가 알고 있는 메이저 버전과 동일한지 확인 — 불일치 시 해석 거부.
	 * 값 범위: 0 이상 단조 증가 (메이저 버전 증가는 incompatible 변경).
	 * 동기화: 응답 데이터는 read-only — 락 불필요. */

	uint16_t		minor_revison;
	/* [한국어] 페이지 포맷 마이너 버전 (와이어 오프셋 2~3).
	 * 역할: 같은 메이저 내에서 호환을 보장하며 새 필드 추가 시 마이너 +1.
	 * 설정자: 펌웨어. 읽는 자: 호스트가 자신이 모르는 마이너 버전이라도 레이아웃 호환됨을 가정해 해석.
	 * 값 범위: 0 이상. 동기화: read-only. */

	uint32_t		buckets_32us[32];
	/* [한국어] 32us 폭 32개 버킷 카운터 (와이어 오프셋 4~131, 총 128B).
	 * 역할: 짧은 latency 영역(0~1024us)의 정밀 분포 — sub-ms 워크로드 분석에 핵심.
	 *       buckets_32us[0]=0~32us, buckets_32us[1]=32~64us, ..., buckets_32us[31]=992~1024us.
	 * 설정자: 펌웨어가 각 I/O completion 시점의 latency를 측정해 해당 버킷 +1.
	 *         FID 0xE2(LATENCY_TRACKING) enable일 때만 갱신.
	 * 읽는 자: 호스트가 모든 버킷을 순회해 분위수(p50/p99) 계산 또는 시각화.
	 * 값 범위: 32비트 unsigned 카운터 — 2^32(약 4G) 도달 시 펌웨어 정의에 따라 wrap 또는 saturate.
	 * 동기화: 펌웨어가 atomic snapshot — 호스트는 응답 시점의 일관된 분포를 받음. */

	uint32_t		buckets_1ms[31];
	/* [한국어] 1ms 폭 31개 버킷 (와이어 오프셋 132~255, 총 124B).
	 * 역할: 중간 영역(1~32ms)의 latency 분포 — 일반 워크로드 평균 영역.
	 *       buckets_1ms[0]=1~2ms, ..., buckets_1ms[30]=31~32ms.
	 * 설정자/읽는 자/값 범위/동기화: buckets_32us와 동일.
	 * 주의: 32us 마지막 버킷(992~1024us)과 1ms 첫 버킷(1~2ms) 사이에 1024us~1000us 미세 갭이
	 *       존재할 수 있으며 펌웨어가 매핑 정의(미세 차이는 무시되거나 가까운 버킷에 합산). */

	uint32_t		buckets_32ms[31];
	/* [한국어] 32ms 폭 31개 버킷 (와이어 오프셋 256~379, 총 124B).
	 * 역할: long-tail latency(32~1024ms) 추적 — 펌웨어 GC, 에러 복구로 인한 outlier 분석.
	 *       buckets_32ms[0]=32~64ms, ..., buckets_32ms[30]=1024ms 영역.
	 * 설정자/읽는 자/값 범위/동기화: buckets_32us와 동일.
	 * 주의: 가장 마지막 버킷은 ">= 상한"(saturation) 의미를 갖는 경우가 흔함 — 펌웨어 의존. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_intel_rw_latency_page) == 380, "Incorrect size");
/* [한국어] 4(2+2) + 32*4 + 31*4 + 31*4 = 4 + 128 + 124 + 124 = 380. 와이어 포맷 크기 검증. */

struct spdk_nvme_intel_temperature_page {
	/* [한국어] LID 0xC5 응답 페이로드(112B) — 일반 SMART(LID 0x02)에 없는 historical min/max 제공.
	 * 설정자: 펌웨어가 내부 thermal sensor를 주기 폴링해 누적.
	 * 읽는 자: 사용자/모니터링 도구가 thermal 트렌드 추적. */

	uint64_t		current_temperature;
	/* [한국어] 현재 컴포지트 온도 (와이어 오프셋 0~7, 단위는 K=Kelvin, NVMe 표준과 일치).
	 * 역할: SSD 내부 thermal sensor가 측정한 가장 뜨거운 지점의 현재 온도.
	 * 설정자: 펌웨어가 응답 직전 thermal sensor를 폴링해 갱신.
	 * 읽는 자: 호스트가 K→°C 변환(°C = K - 273)해 사용자에게 표시.
	 * 값 범위: 절대온도 K(부호 없는 64비트) — 일반적으로 273~400 범위 내.
	 * 동기화: 응답 시점의 즉석 측정값 — 호스트는 stale 가능성 인지 후 사용. */

	uint64_t		shutdown_flag_last;
	/* [한국어] 마지막 부팅 사이클에서 thermal shutdown(과열로 강제 종료) 발생 여부 (와이어 오프셋 8~15).
	 * 역할: 직전 power cycle이 정상 종료인지 thermal-induced 종료인지 호스트가 사후 확인.
	 * 설정자: 펌웨어가 부팅 시 NVRAM에 기록된 직전 종료 사유를 로드.
	 * 읽는 자: 호스트 모니터링 도구가 부팅 직후 점검.
	 * 값 범위: 0=정상 종료, 1=thermal shutdown (정확한 매핑은 펌웨어 정의).
	 * 동기화: 부팅 후 read-only. */

	uint64_t		shutdown_flag_life;
	/* [한국어] 디바이스 전체 수명 동안 누적된 thermal shutdown 발생 횟수 (와이어 오프셋 16~23).
	 * 역할: 디바이스가 일생 동안 몇 번 과열 종료되었는지 — 사용 환경(쿨링) 적정성 평가 지표.
	 * 설정자: 펌웨어가 thermal shutdown 발생 시 NVRAM 카운터 +1.
	 * 읽는 자: 호스트 모니터링.
	 * 값 범위: 0 이상 단조 증가 (NVRAM 영구 보존).
	 * 동기화: NVRAM 기록은 펌웨어가 atomic write 보장. */

	uint64_t		highest_temperature;
	/* [한국어] 디바이스 수명 동안 관측된 최고 온도(K) (와이어 오프셋 24~31).
	 * 역할: SSD가 일생 동안 경험한 최악의 thermal stress — wear와 wear leveling 정책 영향.
	 * 설정자: 펌웨어가 sensor 폴링 시 현재 값이 highest 초과면 갱신.
	 * 읽는 자: 호스트 모니터링.
	 * 값 범위: K 단위 절대온도. 동기화: NVRAM 보존. */

	uint64_t		lowest_temperature;
	/* [한국어] 디바이스 수명 동안 관측된 최저 온도(K) (와이어 오프셋 32~39).
	 * 역할: cold-boot 환경/저온 데이터센터에서의 작동 한계 검증.
	 * 설정자: 펌웨어가 sensor 폴링 시 현재 값이 lowest 미만이면 갱신.
	 * 읽는 자: 호스트 모니터링.
	 * 값 범위: K 단위 절대온도. 동기화: NVRAM 보존. */

	uint64_t		reserved[5];
	/* [한국어] Intel 향후 확장 예약 영역 (와이어 오프셋 40~79, 5×8=40바이트).
	 * 역할: 새 thermal 통계 필드 추가 슬롯.
	 * 설정자: 펌웨어가 0으로 채움.
	 * 읽는 자: 호스트는 0 가정 없이 무시 (forward-compat).
	 * 값 범위: 0x00 (현재 미정의). 동기화: 무관. */

	uint64_t		specified_max_op_temperature;
	/* [한국어] 펌웨어가 광고하는 최대 동작 온도(K) (와이어 오프셋 80~87, 데이터시트 값과 일치).
	 * 역할: 호스트가 thermal headroom 계산 — current_temperature와 비교해 위험 임계 판단.
	 * 설정자: 펌웨어가 출하 시 정의된 상한을 응답에 채움.
	 * 읽는 자: 호스트 모니터링이 "current vs max" 차이로 alert 트리거.
	 * 값 범위: K 단위 — 일반적으로 343~358 (70~85°C).
	 * 동기화: 펌웨어 일생 read-only 값. */

	uint64_t		reserved2;
	/* [한국어] 8바이트 예약 (와이어 오프셋 88~95).
	 * 역할: max/min 사이 정렬·확장 슬롯. 설정자: 펌웨어 0. 읽는 자: 무시.
	 * 값 범위: 0x00. 동기화: 무관. */

	uint64_t		specified_min_op_temperature;
	/* [한국어] 펌웨어가 광고하는 최소 동작 온도(K) (와이어 오프셋 96~103).
	 * 역할: 저온 환경 작동 한계 — 호스트가 cold start 적정성 판단.
	 * 설정자: 펌웨어가 출하 시 정의된 하한을 응답에 채움. 읽는 자: 호스트 모니터링.
	 * 값 범위: K 단위 — 일반적으로 273 (0°C) 근처.
	 * 동기화: read-only. */

	uint64_t		estimated_offset;
	/* [한국어] 센서 오프셋 추정값 (와이어 오프셋 104~111, 보정용).
	 * 역할: thermal sensor와 실제 NAND junction 온도의 정적 오차 — 호스트는 보정용으로 차감.
	 *       실온도 ≈ current_temperature - estimated_offset.
	 * 설정자: 펌웨어가 공장 calibration 또는 런타임 추정 결과 채움.
	 * 읽는 자: 호스트가 정밀 모니터링 시 차감 적용.
	 * 값 범위: K 단위 부호 없음 — Intel 정의 인코딩으로 부호 표현 가능.
	 * 동기화: 부팅 후 가끔 갱신 — read-mostly. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_intel_temperature_page) == 112, "Incorrect size");
/* [한국어] 8 * 14 = 112 — 14개의 uint64_t로 구성. */

struct spdk_nvme_intel_smart_attribute {
	/* [한국어] Intel SMART 페이지의 단일 attribute 슬롯(12B). ATA SMART와 유사한 레이아웃.
	 * 설정자: 펌웨어. 읽는 자: 호스트가 attributes[13] 배열을 순회. */

	uint8_t			code;
	/* [한국어] attribute 식별 코드 (와이어 오프셋 0).
	 * 역할: 슬롯의 의미를 enum spdk_nvme_intel_smart_attribute_code (0xAB~0xF5)로 식별 —
	 *       슬롯 인덱스가 아닌 code 값으로 라우팅.
	 * 설정자: SSD 펌웨어가 슬롯에 채울 attribute 종류 결정.
	 * 읽는 자: 호스트가 switch(code) 분기로 raw_value[6] 해석 방식 결정.
	 * 값 범위: enum 값(0xAB, 0xAC, ..., 0xF5) 또는 0(빈 슬롯).
	 * 동기화: 응답 페이로드는 read-only — 락 불필요. */

	uint8_t			reserved[2];
	/* [한국어] 코드 뒤 2바이트 예약 (와이어 오프셋 1~2).
	 * 역할: 향후 status 비트(예: critical 플래그) 확장 슬롯.
	 * 설정자: 펌웨어 0. 읽는 자: 호스트 무시.
	 * 값 범위: 0x00. 동기화: 무관. */

	uint8_t			normalized_value;
	/* [한국어] 0~100 범위의 정규화 값 (와이어 오프셋 3, ATA SMART와 유사).
	 * 역할: attribute의 건강 점수 — 펌웨어가 raw_value를 정규화해 직관적 표시.
	 *       대개 100=새 것/정상, 0=수명 임박/위험.
	 *       MEDIA_WEAR(0xE2) 같은 마모 지표는 사용에 따라 100→0으로 감소.
	 * 설정자: 펌웨어가 attribute별 정규화 공식 적용해 채움.
	 * 읽는 자: 호스트 모니터링 도구가 단순 비교(< threshold)로 alert 트리거.
	 * 값 범위: 0~100 (그 외는 펌웨어 정의 — 예: 0xFF는 미지원 슬롯).
	 * 동기화: snapshot — read-only. */

	uint8_t			reserved2;
	/* [한국어] 1바이트 예약 (와이어 오프셋 4).
	 * 역할: normalized_value와 raw_value 사이 패딩/threshold 슬롯.
	 * 설정자: 펌웨어 0. 읽는 자: 무시. 값 범위: 0x00. 동기화: 무관. */

	uint8_t			raw_value[6];
	/* [한국어] 코드별 의미가 다른 raw 값 6바이트 (와이어 오프셋 5~10, 대개 little-endian 카운터).
	 * 역할: attribute의 실제 측정값 (정규화 전 원시 데이터).
	 *       해석 방식은 code별로 천차만별:
	 *         - PROGRAM_FAIL_COUNT(0xAB): 48비트 unsigned 카운터.
	 *         - HOST_BYTES_WRITTEN(0xF5): 32MiB 단위 카운터 (실제 바이트 = raw * 32MiB).
	 *         - WEAR_LEVELING_COUNT(0xAD): min/avg/max 3개 16비트 값으로 분할 인코딩.
	 *         - THERMAL_THROTTLE_STATUS(0xEA): [0]=현재 % + [1..]=누적 시간.
	 * 설정자: 펌웨어가 code별 정의된 인코딩으로 채움.
	 * 읽는 자: 호스트가 code 분기 후 정해진 인코딩으로 디코딩.
	 * 값 범위: 6바이트 (48비트) — code별 의미 다름.
	 * 동기화: snapshot — read-only. */

	uint8_t			reserved3;
	/* [한국어] 마지막 1바이트 예약 (와이어 오프셋 11) — 12B 슬롯 크기 정렬 맞춤.
	 * 역할: 다음 attribute 슬롯이 12B 경계에 시작하도록 보장.
	 * 설정자: 펌웨어 0. 읽는 자: 무시. 값 범위: 0x00. 동기화: 무관. */
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
	/* [한국어] 13개의 12바이트 attribute 슬롯 (와이어 오프셋 0~155, 156B packed).
	 * 역할: Intel 확장 SMART의 모든 통계를 담는 가변 의미 슬롯 배열.
	 *       슬롯 인덱스 자체에는 의미가 없고 attributes[i].code로 식별.
	 * 설정자: 펌웨어가 모든 13슬롯을 빠짐없이 채움 (code 순서는 펌웨어/모델별 다를 수 있음).
	 *         정의되지 않은 슬롯은 code=0으로 보내며, 호스트는 이를 skip 처리.
	 * 읽는 자: 호스트가 for(i=0..12) 루프로 attributes[i].code 분기 후 raw_value 해석.
	 * 값 범위: 각 슬롯은 12B로 packed — 패딩 없이 156B 정확.
	 * 동기화: 펌웨어가 응답 시 모든 슬롯을 atomic snapshot으로 채움 — 호스트 락 불필요. */
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
	/* [한국어] 32비트 통째 접근용 별칭 (와이어: NVMe Set/Get Feature CDW11 그대로).
	 * 역할: 호스트가 비트필드 사용 없이 미리 인코딩된 32비트 값을 그대로 전송할 때 사용.
	 * 설정자: 호스트가 cdw11 = power_gov_value 형태로 대입.
	 * 읽는 자: 펌웨어가 CDW11 raw로 수신.
	 * 값 범위: 0x00000000 ~ 0xFFFFFFFF (의미 있는 비트는 하위 8비트만).
	 * 동기화: 32비트 단위 atomic 쓰기. */

	struct {
		/** power governor setting : 00h = 25W 01h = 20W 02h = 10W */
		uint32_t power_governor_setting		: 8;
		/* [한국어] 하위 8비트(CDW11[7:0]) — 전력 캡 모드 선택.
		 * 역할: 평균 전력 상한을 펌웨어 정의 모드로 제한 →
		 *       00h = 25W (성능 최대), 01h = 20W (균형), 02h = 10W (저전력 모드).
		 *       데이터센터 SSD에서 PSU/thermal 제약 또는 dense rack 운용 시 사용.
		 * 설정자: 호스트가 Set Feature 시 0/1/2 중 선택.
		 * 읽는 자: 펌웨어가 controller P-state limiter에 적용. Get Feature는 현재 모드 반환.
		 * 값 범위: 0x00, 0x01, 0x02 (그 외는 펌웨어 정의 — 거부 가능).
		 * 동기화: atomic 32비트 쓰기 — 동시 발행 시 펌웨어 큐가 직렬화. */

		uint32_t reserved	: 24;
		/* [한국어] 상위 24비트(CDW11[31:8]) 예약 — 호스트는 반드시 0으로 설정.
		 * 역할: 향후 power governor 옵션 확장 슬롯.
		 * 설정자: 호스트가 0으로 채움 (비-0 시 펌웨어 거부 또는 정의되지 않은 동작).
		 * 읽는 자: 펌웨어가 0 검증 후 무시.
		 * 값 범위: 0x000000. 동기화: 32비트 단위 atomic. */
	} bits;
	/* [한국어] 비트필드 뷰 — raw와 같은 메모리를 비트 단위로 디코딩하는 union 멤버.
	 * 역할: 호스트 코드의 가독성 제고 — feat.bits.power_governor_setting = 1 형태로 사용.
	 * 동기화: union 자체는 단순 접근 방식 차이 — 동시성 의미 없음. */
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_intel_feat_power_governor) == 4, "Incorrect size");
/* [한국어] CDW11은 정확히 4바이트(32비트). union 크기 검증. */

union spdk_nvme_intel_feat_smbus_address {
	/* [한국어] FID 0xC8 (SMBUS_ADDRESS) CDW11 인터프리터.
	 * 설정자: 호스트가 BMC와 협의된 SMBus 슬레이브 주소를 기록.
	 * 읽는 자: SSD 펌웨어가 SMBus 인터페이스에 적용. */

	uint32_t	raw;
	/* [한국어] CDW11 raw 32비트 통째 접근용 별칭.
	 * 역할: 호스트가 미리 인코딩된 SMBus 주소 워드를 그대로 전송.
	 * 설정자: 호스트. 읽는 자: 펌웨어가 CDW11 raw로 수신.
	 * 값 범위: 32비트. 동기화: atomic 32비트 쓰기. */

	struct {
		uint32_t reserved	: 1;
		/* [한국어] LSB 1비트(CDW11[0]) 예약.
		 * 역할: SMBus 주소가 7비트이므로 LSB는 R/W bit 위치에 해당하는 자리 — 호스트가 0으로 비움.
		 * 설정자: 호스트가 0. 읽는 자: 펌웨어가 무시.
		 * 값 범위: 0. 동기화: 32비트 워드 일부 — 펌웨어가 비트 마스킹으로 추출. */

		uint32_t smbus_controller_address	: 8;
		/* [한국어] 8비트 SMBus 슬레이브 주소(CDW11[8:1], 7비트 주소를 한 비트 좌측 시프트한 형태).
		 * 역할: BMC가 NVMe-MI 명령으로 이 SSD를 어드레싱할 때 사용할 주소.
		 * 설정자: 호스트가 BMC와 협의된 7비트 주소를 << 1 형태로 기록.
		 * 읽는 자: SSD 펌웨어가 SMBus 컨트롤러 IP에 적용 → 다음 BMC 트랜잭션부터 응답.
		 * 값 범위: 표준 SMBus 7비트 주소 영역(0x08~0x77 범위 등) — Intel 스펙 페이지 참조.
		 * 동기화: 한 번 부팅 후 고정되는 설정. */

		uint32_t reserved2	: 23;
		/* [한국어] 상위 23비트(CDW11[31:9]) 예약 — 호스트는 0으로 기록.
		 * 역할: 향후 SMBus 추가 설정(예: 폴링 주기) 확장 슬롯.
		 * 설정자: 호스트 0. 읽는 자: 펌웨어 무시.
		 * 값 범위: 0. 동기화: 무관. */
	} bits;
	/* [한국어] 비트필드 뷰. 동기화: 단순 접근 방식 차이. */
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_intel_feat_smbus_address) == 4, "Incorrect size");
/* [한국어] 32비트 크기 검증. */

union spdk_nvme_intel_feat_led_pattern {
	/* [한국어] FID 0xC9 (LED_PATTERN) CDW11 인터프리터.
	 * 설정자: 데이터센터 운영자가 베이의 드라이브를 식별하기 위해 블링크 패턴 명령.
	 * 읽는 자: SSD 펌웨어가 식별 LED 컨트롤러에 패턴 적용. */

	uint32_t	raw;
	/* [한국어] CDW11 raw 32비트 통째 접근용.
	 * 역할: 호스트가 비트필드 우회해 직접 인코딩 값 전송.
	 * 설정자/읽는 자/값 범위/동기화: 다른 feat union과 동일. */

	struct {
		uint32_t feature_options	: 24;
		/* [한국어] 하위 24비트(CDW11[23:0]) — 패턴 옵션 비트맵 (블링크 주기, 색상, 폴링 인터벌 등).
		 * 역할: value(상위 8비트)가 정한 모드의 세부 파라미터.
		 * 설정자: 호스트가 펌웨어 스펙 페이지에 따라 비트별 옵션 설정.
		 * 읽는 자: SSD 펌웨어가 LED PWM 컨트롤러에 적용.
		 * 값 범위: 펌웨어 정의 비트맵 — Intel 스펙 페이지 참조.
		 * 동기화: atomic 32비트 워드 일부 — race 없음(단일 호스트 발행). */

		uint32_t value	: 8;
		/* [한국어] 상위 8비트(CDW11[31:24]) — 패턴 강도/모드 enum.
		 * 역할: LED 동작 모드를 결정 — 0=off, 1=solid, 2=blink 등 (정확한 값은 펌웨어 정의).
		 * 설정자: 호스트가 운영 의도에 따라 모드 코드 기록.
		 * 읽는 자: SSD 펌웨어 LED 드라이버.
		 * 값 범위: 0x00 ~ 0xFF (펌웨어 정의 enum).
		 * 동기화: atomic 워드 일부. */
	} bits;
	/* [한국어] 비트필드 뷰. */
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_intel_feat_led_pattern) == 4, "Incorrect size");
/* [한국어] 32비트 크기 검증. */

union spdk_nvme_intel_feat_reset_timed_workload_counters {
	/* [한국어] FID 0xD5 (RESET_TIMED_WORKLOAD_COUNTERS) CDW11.
	 * Set 전용 — 호스트가 timed counter (E2 media wear, E3 host read %, E4 timer)를 리셋.
	 * Get은 미지원이라 응답에 의미 없음.
	 * 설정자: 호스트(주기적 SMART 베이스라인 리셋 용도). 읽는 자: SSD 펌웨어 리셋 트리거. */

	uint32_t	raw;
	/* [한국어] CDW11 raw 32비트 통째 접근용 — Set Feature 0xD5 트리거.
	 * 역할: 호스트가 미리 인코딩된 32비트 값 그대로 발행.
	 * 설정자: 호스트. 읽는 자: 펌웨어. 값 범위: 32비트. 동기화: atomic. */

	struct {
		/**
		 * Write Usage: 00 = NOP, 1 = Reset E2, E3,E4 counters;
		 * Read Usage: Not Supported
		 */
		uint32_t reset	: 1;
		/* [한국어] LSB 1비트(CDW11[0]) — timed counter 리셋 트리거.
		 * 역할: Set Feature 0xD5 발행 시 0=NOP, 1=E2(media wear)/E3(host read %)/E4(timer) 즉시 리셋.
		 *       Get Feature는 미지원 (스펙 정의 — Read Usage: Not Supported).
		 * 설정자: 호스트가 새 워크로드 측정 베이스라인을 잡기 위해 1로 기록 후 발행.
		 * 읽는 자: SSD 펌웨어가 SMART 페이지(LID 0xCA)의 해당 attribute raw_value를 0으로 클리어.
		 * 값 범위: 0 또는 1.
		 * 동기화: 펌웨어가 트리거를 1회성으로 처리 — 카운터는 클리어 후 자체 누적 재개 (펌웨어가
		 *        bit를 자동으로 0으로 되돌리지는 않음 — 호스트 발행한 워드 자체가 일회성). */

		uint32_t reserved	: 31;
		/* [한국어] 상위 31비트(CDW11[31:1]) 예약 — 호스트는 0으로 기록.
		 * 역할: 향후 부분 리셋(개별 카운터 선택) 등 확장 슬롯.
		 * 설정자: 호스트 0. 읽는 자: 펌웨어 무시.
		 * 값 범위: 0. 동기화: 무관. */
	} bits;
	/* [한국어] 비트필드 뷰. */
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_intel_feat_reset_timed_workload_counters) == 4,
		   "Incorrect size");
/* [한국어] 32비트 크기 검증. */

union spdk_nvme_intel_feat_latency_tracking {
	/* [한국어] FID 0xE2 (LATENCY_TRACKING) CDW11.
	 * 설정자: 호스트가 latency histogram(LID 0xC1/0xC2) 수집을 enable/disable.
	 * 읽는 자: SSD 펌웨어가 내부 통계 수집 토글. */

	uint32_t	raw;
	/* [한국어] CDW11 raw 32비트 통째 접근용 — bits.enable과 동일 메모리.
	 * 역할: 호스트가 0/1 단순 토글 시 raw 사용이 더 직관적.
	 * 설정자: 호스트. 읽는 자: 펌웨어. 값 범위: 32비트. 동기화: atomic 워드 쓰기. */

	struct {
		/**
		 * Write Usage:
		 * 00h = Disable Latency Tracking (Default)
		 * 01h = Enable Latency Tracking
		 */
		uint32_t enable	: 32;
		/* [한국어] 32비트 enable 플래그 — latency 추적 토글 (CDW11[31:0] 전체 사용).
		 * 역할: SSD 펌웨어 내부 latency 통계 수집기 활성/비활성 결정.
		 *       비활성 시 LID 0xC1(read)/0xC2(write) 응답은 stale 값 또는 0.
		 * 설정자: 호스트가 측정 시작 전 1로 enable, 측정 종료 후 0으로 disable.
		 * 읽는 자: SSD 펌웨어가 I/O completion path의 timestamp 측정·버킷 누적 토글.
		 *          이후 호스트는 LID 0xC1/0xC2로 spdk_nvme_intel_rw_latency_page 회수.
		 * 값 범위: 0x00=Disable(기본), 0x01=Enable. 그 외 값은 펌웨어 정의 행동(향후 확장용).
		 * 동기화: atomic 32비트 쓰기 — 토글 직전·직후 일부 I/O 통계가 누락/중복될 수 있어
		 *        호스트는 측정 데이터 신뢰 구간을 토글 시점 ±수 msec 후로 제한 권장. */
	} bits;
	/* [한국어] 비트필드 뷰. 한 비트필드(32비트)뿐이라 raw와 동등. */
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvme_intel_feat_latency_tracking) == 4, "Incorrect size");
/* [한국어] 32비트 크기 검증. */

struct spdk_nvme_intel_marketing_description_page {
	/* [한국어] LID 0xDD (Marketing Description) 응답 페이로드.
	 * 설정자: SSD 펌웨어가 ASCII 제품명을 channel write.
	 * 읽는 자: examples/nvme/identify가 사용자에게 출력 (예: "INTEL SSDPE2KX040T8"). */

	uint8_t		marketing_product[512];
	/* [한국어] ASCII 제품 마케팅 문자열 (와이어 오프셋 0~511, 정확히 512B만 의미 있음).
	 * 역할: 사용자에게 표시되는 제품 식별 문자열 (예: "INTEL SSDPE2KX040T8 ").
	 *       NVMe Identify Controller의 MN(Model Number, 40B)과 별개로 더 긴 마케팅명 노출.
	 * 설정자: SSD 펌웨어가 출하 시 정의된 ASCII 문자열을 응답 DMA로 채움.
	 * 읽는 자: examples/nvme/identify가 --intel 옵션 시 출력. nvme-cli 등의 도구도 활용.
	 * 값 범위: 7-bit ASCII 문자(예: A-Z, 0-9, 공백). NULL 종료 보장 없음 — 호스트는
	 *         반드시 strncpy/strnlen(.., 512) 사용해 buffer overflow 방지.
	 * 동기화: 펌웨어 일생 read-only — 한 번 회수 후 캐시 가능. */

	/* Spec says this log page will only write 512 bytes, but there are some older FW
	 * versions that accidentally write 516 instead.  So just pad this out to 4096 bytes
	 * to make sure users of this structure never end up overwriting unintended parts of
	 * memory.
	 */
	uint8_t		reserved[3584];
	/* [한국어] 3584B 패딩 (와이어 오프셋 512~4095) — 펌웨어 버그 방어용 안전 영역.
	 * 역할: 일부 구버전 Intel 펌웨어가 스펙을 어기고 512B 대신 516B(또는 그 이상)를 DMA 기록하는
	 *       알려진 버그 회피. 호스트 버퍼가 정확히 512B면 stack/heap에 인접 메모리를 덮어쓸 위험.
	 *       구조체를 4096B(NVMe 메모리 페이지 정렬 단위)로 패딩해 어떤 펌웨어 버전이라도 안전.
	 * 설정자: 정상 펌웨어는 0으로 zero-fill 또는 미터치. 버그 있는 펌웨어는 처음 4B에 추가 데이터.
	 * 읽는 자: 호스트는 reserved 영역을 의미 있는 값으로 해석하지 않음 — 단순 안전 패딩.
	 * 값 범위: 정의되지 않음 (펌웨어 의존).
	 * 동기화: read-only 응답 페이로드 — 락 불필요. */
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
