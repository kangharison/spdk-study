/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] VMD 슬롯 LED 제어 모듈 (led.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Intel VMD 뒤에 매달린 PCIe NVMe 디바이스의 *슬롯 LED*(IDENTIFY/FAULT/REBUILD/OFF)를
 * 사용자 공간에서 직접 제어하는 SPDK 공개 API를 구현한다. 일반 서버 섀시는 SES(SCSI Enclosure
 * Services) 또는 PCIe Slot Status/Control 레지스터를 통해 LED를 켜고 끄는데, VMD 환경에서는
 * 호스트 OS가 PCIe 슬롯 컨트롤러에 직접 MMIO write가 가능하므로 SES 없이도 LED를 제어할 수 있다.
 * 본 파일은 (attention indicator, power indicator) 비트 조합을 LED 상태에 매핑한 테이블과
 * 해당 비트를 PCIe slot_control 레지스터에 쓰는 로직, 그리고 endpoint→parent bridge로의
 * 슬롯 컨트롤러 역추적 로직을 담는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   사용자 RPC (vmd_set_led_state / vmd_get_led_state)
 *     → spdk_vmd_set_led_state(pci_device, state)  [공개 API in spdk/vmd.h]
 *     → vmd_get_led_device()           : endpoint → parent bridge 역추적 (LED는 부모 슬롯에 매달림)
 *     → vmd_led_set_indicator_control(): pcie_cap.slot_control MMIO write
 *     → 섀시의 SES/PCIe 슬롯 LED 점등
 * 실행 컨텍스트: 보통 RPC 처리 스레드 (SPDK mgmt thread). polled-mode 핫패스가 아님 - I/O 외부 경로.
 *
 * === 타 모듈과의 연결 ===
 * - vmd_internal.h : vmd_pci_device, pcie_cap, cached_slot_control 정의.
 * - spdk/vmd.h     : spdk_vmd_led_state enum (OFF/IDENTIFY/FAULT/REBUILD/UNKNOWN).
 * - spdk/log.h     : SPDK_ERRLOG.
 * - spdk/likely.h  : spdk_unlikely - branch prediction hint.
 * - spdk_pci_*    : 외부에서 spdk_pci_device로 호출하므로 vmd_find_device로 vmd 측 핸들 역검색.
 * 데이터 흐름:
 *   사용자 state(enum) → g_led_config 테이블 lookup → slot_control 비트필드 set
 *   → MMIO write (posted) → MMIO read back(=flush) → cached_slot_control 갱신
 *
 * === 주요 함수/구조체 요약 ===
 * - struct vmd_led_indicator_config : (attention_indicator, power_indicator) 비트 조합.
 * - g_led_config[]                  : LED 상태 → 비트 조합 매핑 테이블.
 * - vmd_led_set_indicator_control() : 비트 조합을 slot_control에 write + read-back으로 flush.
 * - vmd_led_get_state()             : cached_slot_control 비트를 LED 상태 enum으로 역변환.
 * - vmd_get_led_device()            : endpoint면 parent bridge로 올라가 슬롯 컨트롤러 디바이스 반환.
 * - spdk_vmd_set_led_state()        : 공개 API - state 검증 + 매핑 + write.
 * - spdk_vmd_get_led_state()        : 공개 API - 디바이스 lookup + 캐시 디코딩.
 */

/* [한국어] SPDK 표준 C 라이브러리 - assert/strcmp 등 사용. */
#include "spdk/stdinc.h"
/* [한국어] spdk_unlikely - 에러 경로(드물게 발생)를 분기예측에서 cold로 표시. */
#include "spdk/likely.h"
/* [한국어] SPDK_ERRLOG - 에러 로그 출력. */
#include "spdk/log.h"
/* [한국어] vmd_pci_device, pcie_cap, cached_slot_control 등 내부 자료구조 정의. */
#include "vmd_internal.h"

/* [한국어] PCIe slot_control 레지스터의 LED 비트필드를 표현하는 구조체.
 * PCIe 스펙(express slot control register, base spec 6.7.2)의 attention/power indicator 비트(2비트씩)에 대응. */
struct vmd_led_indicator_config {
	uint8_t attention_indicator	: 2;
	/* [한국어] Attention Indicator Control (2 bits): 00=Reserved, 01=On, 10=Blink, 11=Off.
	 * 설정자: g_led_config 테이블에 따라 vmd_led_set_indicator_control이 set.
	 * 읽는 자: HW가 slot_control 레지스터 write 시 이 비트를 보고 LED 제어.
	 * 값 의미: FAULT/REBUILD에서는 01(On 또는 Blink와 조합), OFF/IDENTIFY에서는 11(Off). */

	uint8_t power_indicator		: 2;
	/* [한국어] Power Indicator Control (2 bits): 00=Reserved, 01=On, 10=Blink, 11=Off.
	 * 값 의미: IDENTIFY에서는 01(Blink 4Hz와 조합), OFF/FAULT에서는 11(Off). */

	uint8_t reserved		: 4;
	/* [한국어] 비트필드 패딩 - attention(2)+power(2)=4비트를 8비트(uint8_t) 경계로 채우는 reserved 4비트.
	 * 설정자: designated initializer가 명시하지 않으므로 항상 0으로 초기화됨.
	 * 읽는 자: 누구도 읽지 않음 - 구조체 크기를 1바이트로 고정하기 위한 패딩 전용.
	 * 값 범위: 항상 0 (의미 없음).
	 * 동기화: 컴파일 타임 상수 테이블의 일부이므로 런타임 변경 없음 - 동기화 불필요. */
};

/*
 * VMD LED     Attn       Power       LED Amber
 * State       Indicator  Indicator
 *             Control    Control
 * ------------------------------------------------
 * Off         11b        11b         Off
 * Ident       11b        01b         Blink 4Hz
 * Fault       01b        11b         On
 * Rebuild     01b        01b         Blink 1Hz
 */
/* [한국어] LED 상태 enum → (attention, power) 비트 조합 매핑 테이블 (designated initializer 사용).
 * Intel VMD의 LED 펌웨어가 (attention, power) 입력 조합을 보고 amber LED의 패턴을 결정한다.
 *  - OFF       : (11b, 11b) = 둘 다 Off → LED 꺼짐.
 *  - IDENTIFY  : (11b, 01b) = attention Off, power On → 펌웨어가 4Hz blink 패턴 출력.
 *  - FAULT     : (01b, 11b) = attention On, power Off → solid On (continuous).
 *  - REBUILD   : (01b, 01b) = 둘 다 On → 펌웨어가 1Hz blink 패턴 출력.
 * 설정자: 컴파일 타임 상수 - 변경 불가.
 * 읽는 자: vmd_led_set_indicator_control이 state로 인덱싱, vmd_led_get_state가 역검색. */
static const struct vmd_led_indicator_config g_led_config[] = {
	/* [한국어] OFF: attention=3(11b=Off), power=3(11b=Off) → 두 indicator 모두 꺼짐 → amber LED 소등. */
	[SPDK_VMD_LED_STATE_OFF]	= { .attention_indicator = 3, .power_indicator = 3 },
	/* [한국어] IDENTIFY: attention=3(11b=Off), power=1(01b=On) → 펌웨어가 power On을 보고 4Hz blink 출력. */
	[SPDK_VMD_LED_STATE_IDENTIFY]	= { .attention_indicator = 3, .power_indicator = 1 },
	/* [한국어] FAULT: attention=1(01b=On), power=3(11b=Off) → attention On → solid(continuous) On. */
	[SPDK_VMD_LED_STATE_FAULT]	= { .attention_indicator = 1, .power_indicator = 3 },
	/* [한국어] REBUILD: attention=1(01b=On), power=1(01b=On) → 둘 다 On → 펌웨어가 1Hz blink 출력. */
	[SPDK_VMD_LED_STATE_REBUILD]	= { .attention_indicator = 1, .power_indicator = 1 },
};

/*
 * [한국어]
 * vmd_led_set_indicator_control - 슬롯 컨트롤러의 LED 비트를 지정 state에 맞춰 write
 *
 * @vmd_device: 슬롯을 노출하는 PCIe 브릿지(parent bridge) 디바이스. endpoint를 직접 넘기면 안 됨.
 * @state: 설정할 LED 상태 enum (OFF/IDENTIFY/FAULT/REBUILD).
 * @return: 없음 (void) - assert로 입력 검증.
 *
 * 왜 필요한가: PCIe slot_control 레지스터에 직접 비트 write하면 끝이지만, PCI config write는
 * "posted write"라 즉시 반영이 보장되지 않는다. 따라서 write 후 같은 레지스터를 read back하여
 * 강제 flush한 뒤 캐시(cached_slot_control)에 저장한다. read-back은 PCI 트랜잭션 ordering 규칙상
 * write completion을 강제한다.
 *
 * 동작 과정:
 *   1) g_led_config[state]에서 (attention, power) 비트 조합 lookup.
 *   2) 현재 slot_control 레지스터를 MMIO read로 가져와 비트필드 union에 담음.
 *   3) attention_indicator_control / power_indicator_control 비트만 덮어씀 (다른 비트 유지).
 *   4) slot_control에 write → read back → cached_slot_control 갱신.
 *
 * 실행 컨텍스트: RPC 처리 스레드. MMIO read/write는 단일 스레드에서 직렬 호출 가정.
 *
 * 호출 체인:
 *   spdk_vmd_set_led_state() → vmd_get_led_device() → [vmd_led_set_indicator_control]
 */
static void
vmd_led_set_indicator_control(struct vmd_pci_device *vmd_device, enum spdk_vmd_led_state state)
{
	const struct vmd_led_indicator_config *config;        /* [한국어] g_led_config 테이블 엔트리 포인터. */
	union express_slot_control_register slot_control;     /* [한국어] slot_control 32-bit + 비트필드 union. */

	/* [한국어] state 입력 검증 - g_led_config 배열 범위 보장(out-of-bounds read 방지).
	 * UNKNOWN/잘못된 값은 호출자(spdk_vmd_set_led_state)에서 미리 거부되지만 방어적 assert. */
	assert(state >= SPDK_VMD_LED_STATE_OFF && state <= SPDK_VMD_LED_STATE_REBUILD);
	config = &g_led_config[state];   /* [한국어] state에 해당하는 비트 조합 가져오기. */

	/* [한국어] PCIe capability 영역의 slot_control 레지스터를 MMIO read.
	 * volatile pcie_cap 포인터 → 컴파일러 캐시 회피, MMIO 트랜잭션 발생.
	 * 다른 비트(예: power controller, indicator events)를 보존하기 위해 read-modify-write 패턴 사용. */
	slot_control = vmd_device->pcie_cap->slot_control;
	/* [한국어] attention/power indicator 비트만 새 값으로 덮어쓰기 (다른 비트는 read한 그대로 유지). */
	slot_control.bit_field.attention_indicator_control = config->attention_indicator;
	slot_control.bit_field.power_indicator_control = config->power_indicator;

	/*
	 * Due to the fact that writes to the PCI config space are posted writes, we need to issue
	 * a read to the register we've just written to ensure it reached its destination.
	 * TODO: wrap all register writes with a function taking care of that.
	 */
	/* [한국어] slot_control에 새 값을 write - PCI posted write이므로 즉시 반영 미보장. */
	vmd_device->pcie_cap->slot_control = slot_control;
	/* [한국어] 같은 레지스터를 다시 read - PCI ordering 규칙상 read는 이전 write를 flush한다.
	 * 동시에 read 결과를 cached_slot_control에 저장하여 다음 get_state 호출 시 MMIO 절약. */
	vmd_device->cached_slot_control = vmd_device->pcie_cap->slot_control;
}

/*
 * [한국어]
 * vmd_led_get_state - cached_slot_control의 비트 조합을 LED 상태 enum으로 역변환
 *
 * @vmd_device: 슬롯 컨트롤러 디바이스(parent bridge).
 * @return: SPDK_VMD_LED_STATE_OFF/IDENTIFY/FAULT/REBUILD 또는 UNKNOWN(매핑 불가).
 *
 * 왜 필요한가: 사용자가 set_led_state 후 실제 비트가 들어갔는지 또는 외부 도구가 변경한 LED 상태를
 *   조회할 수 있도록 한다. cached_slot_control은 set_indicator_control이 read-back으로 갱신한
 *   최신 값이므로 추가 MMIO read 없이 즉시 반환 가능 (latency 절약).
 *
 * 동작: 4가지 state에 대해 g_led_config의 비트 조합과 비교, 일치하는 state 반환. 일치 없으면 UNKNOWN.
 *
 * 실행 컨텍스트: RPC 처리 스레드.
 *
 * 호출 체인:
 *   spdk_vmd_get_led_state() → [vmd_led_get_state]
 */
static unsigned int
vmd_led_get_state(struct vmd_pci_device *vmd_device)
{
	const struct vmd_led_indicator_config *config;        /* [한국어] 테이블 엔트리 포인터. */
	union express_slot_control_register slot_control;     /* [한국어] cached slot_control 임시 복사본. */
	unsigned int state;                                   /* [한국어] 순회용 state 인덱스. */

	/* [한국어] 마지막 write 시 read-back으로 갱신된 캐시값 사용 (MMIO read 회피). */
	slot_control = vmd_device->cached_slot_control;
	/* [한국어] 4개 state(OFF~REBUILD)에 대해 비트 조합 일치 검사. */
	for (state = SPDK_VMD_LED_STATE_OFF; state <= SPDK_VMD_LED_STATE_REBUILD; ++state) {
		config = &g_led_config[state];

		/* [한국어] attention과 power 두 비트가 모두 일치해야 해당 state로 인정. */
		if (slot_control.bit_field.attention_indicator_control == config->attention_indicator &&
		    slot_control.bit_field.power_indicator_control == config->power_indicator) {
			return state;  /* [한국어] 첫 매칭 state 반환. */
		}
	}

	/* [한국어] 어느 매핑에도 해당하지 않는 비트 조합 - 외부 도구가 변경했거나 펌웨어 차이 가능. */
	return SPDK_VMD_LED_STATE_UNKNOWN;
}

/*
 * The identifying device under VMD is located in the global list of VMD controllers.  If the BDF
 * identifies an endpoint, then the LED is attached to the endpoint's parent.  If the BDF identifies
 * a type 1 header, then this device has the corresponding LED. This may arise when a user wants to
 * identify a given empty slot under VMD.
 */
/*
 * [한국어]
 * vmd_get_led_device - 사용자 spdk_pci_device → LED를 가진 vmd_pci_device(슬롯 컨트롤러) 매핑
 *
 * @pci_device: 사용자가 RPC로 지정한 PCI 디바이스 - VMD 뒤의 endpoint(NVMe) 또는 brige 자체.
 * @return: LED 비트를 소유한 vmd_pci_device(반드시 type-1 bridge) 또는 NULL(못 찾음).
 *
 * 왜 필요한가: PCIe slot_control 레지스터는 슬롯 *상위* 브릿지에만 존재한다. 사용자가 NVMe endpoint
 * BDF를 넘기면 그 부모 브릿지를 찾아 그 LED를 조작해야 한다. 빈 슬롯의 LED를 직접 켜고 싶다면
 * 사용자는 brige BDF를 직접 넘길 수도 있다 - 이 함수는 두 케이스를 모두 처리한다.
 *
 * 동작:
 *   1) 디바이스가 "vmd" 타입인지 assert (vmd 트리에 속하지 않으면 호출 자체가 잘못).
 *   2) vmd_find_device(addr)로 vmd_pci_device 핸들 획득.
 *   3) header_type이 NORMAL(=type-0 endpoint)이면 parent의 self(=parent bridge) 반환.
 *      그 외(type-1)면 자기 자신 반환.
 *
 * 실행 컨텍스트: RPC 처리 스레드.
 *
 * 호출 체인:
 *   spdk_vmd_set_led_state()/get_led_state() → [vmd_get_led_device] → vmd_find_device
 */
static struct vmd_pci_device *
vmd_get_led_device(const struct spdk_pci_device *pci_device)
{
	struct vmd_pci_device *vmd_device;

	/* [한국어] 디바이스가 SPDK PCI 시스템에서 "vmd" 타입으로 등록되었는지 검증.
	 * VMD 도메인이 아닌 일반 PCI 디바이스에 대해 호출하면 의미 없으므로 assert. */
	assert(strcmp(spdk_pci_device_get_type(pci_device), "vmd") == 0);

	/* [한국어] BDF로 vmd_pci_device 핸들 역검색 - 모든 어댑터의 bus_list 순회. */
	vmd_device = vmd_find_device(&pci_device->addr);
	if (spdk_unlikely(vmd_device == NULL)) {
		/* [한국어] vmd 도메인에 존재해야 하는데 못 찾음 - race 또는 detached 디바이스. */
		return NULL;
	}

	/* [한국어] header_type=NORMAL(0)은 type-0 endpoint - LED는 부모 브릿지에 있음. */
	if (vmd_device->header_type == PCI_HEADER_TYPE_NORMAL) {
		/* [한국어] 부모 버스가 NULL이면 트리 root - 비정상 상태(VMD root는 type-1이어야 함). */
		if (spdk_unlikely(vmd_device->parent == NULL)) {
			return NULL;
		}

		/* [한국어] 부모 버스의 self 디바이스 = 슬롯 컨트롤러 브릿지. 여기에 slot_control 존재. */
		return vmd_device->parent->self;
	}

	/* [한국어] type-1 bridge면 자기 자신이 슬롯 컨트롤러 - 그대로 반환. */
	return vmd_device;
}

/*
 * [한국어]
 * spdk_vmd_set_led_state - VMD 뒤 디바이스 슬롯의 LED 상태를 설정(공개 API)
 *
 * @pci_device: 대상 PCI 디바이스 (endpoint 또는 슬롯 브릿지).
 * @state: 설정할 LED 상태 (OFF/IDENTIFY/FAULT/REBUILD). UNKNOWN은 거부.
 * @return: 0=성공, -EINVAL=잘못된 state, -ENODEV=VMD 뒤에 없음.
 *
 * 왜 필요한가: 핫스왑 NVMe 베이의 사용자 식별/오류 표시 - 시스템 관리자가 RPC로 LED를 점등해
 *   교체할 디스크의 물리적 위치를 시각적으로 확인할 수 있게 함.
 *
 * 호출 체인:
 *   사용자 RPC (vmd_set_led_state) → [spdk_vmd_set_led_state]
 *     → vmd_get_led_device → vmd_led_set_indicator_control → MMIO write
 */
int
spdk_vmd_set_led_state(struct spdk_pci_device *pci_device, enum spdk_vmd_led_state state)
{
	struct vmd_pci_device *vmd_device;

	/* [한국어] 입력 state 범위 검증 - UNKNOWN/음수/초과값 모두 거부. */
	if (state < SPDK_VMD_LED_STATE_OFF || state > SPDK_VMD_LED_STATE_REBUILD) {
		SPDK_ERRLOG("Invalid LED state\n");
		return -EINVAL;
	}

	/* [한국어] endpoint면 부모 브릿지로, bridge면 그대로 매핑하여 슬롯 컨트롤러 디바이스 획득. */
	vmd_device = vmd_get_led_device(pci_device);
	if (spdk_unlikely(vmd_device == NULL)) {
		/* [한국어] VMD 도메인에 속하지 않거나 트리 inconsistency - LED 제어 불가. */
		SPDK_ERRLOG("The PCI device is not behind the VMD\n");
		return -ENODEV;
	}

	/* [한국어] slot_control 비트 write + read-back flush. 반환값 없음(에러는 위에서 모두 처리). */
	vmd_led_set_indicator_control(vmd_device, state);
	return 0;
}

/*
 * [한국어]
 * spdk_vmd_get_led_state - 현재 LED 상태 조회 (공개 API)
 *
 * @pci_device: 대상 PCI 디바이스.
 * @state: [out] 현재 LED 상태가 채워질 포인터.
 * @return: 0=성공, -ENODEV=VMD 뒤에 없음.
 *
 * cached_slot_control 기반이므로 SPDK가 마지막에 set한 값을 반환한다. 외부 도구가 변경했다면
 * UNKNOWN이 들어갈 수 있다 (이 경우 캐시 갱신 위해 set_led_state를 한 번 호출해야 정확).
 *
 * 호출 체인:
 *   사용자 RPC (vmd_get_led_state) → [spdk_vmd_get_led_state]
 *     → vmd_get_led_device → vmd_led_get_state (캐시 디코딩)
 */
int
spdk_vmd_get_led_state(struct spdk_pci_device *pci_device, enum spdk_vmd_led_state *state)
{
	struct vmd_pci_device *vmd_device;

	/* [한국어] 슬롯 컨트롤러 디바이스 매핑 - endpoint면 parent bridge로 자동 추적. */
	vmd_device = vmd_get_led_device(pci_device);
	if (spdk_unlikely(vmd_device == NULL)) {
		SPDK_ERRLOG("The PCI device is not behind the VMD\n");
		return -ENODEV;
	}

	/* [한국어] cached_slot_control 비트 → enum으로 역변환 후 사용자 출력 인자에 저장. */
	*state = (enum spdk_vmd_led_state)vmd_led_get_state(vmd_device);
	return 0;
}
