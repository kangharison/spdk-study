/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Intel VMD(Volume Management Device) HW 스펙 및 PCIe 표준 정의 헤더 (vmd_spec.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 Intel VMD(Volume Management Device) PCIe controller가 노출하는
 * "가상 PCIe 트리"를 SPDK가 직접 enumerate하기 위해 필요한 모든 HW/PCI 표준
 * 정의를 모아둔다. VMD는 Xeon Scalable 이후의 PCH/Root Complex에 내장된 PCIe
 * 도메인 컨트롤러로, 일반적으로 OS가 BIOS/UEFI를 통해 enumerate하는 NVMe SSD
 * 들을 자신의 BAR 공간 안쪽의 별도 가상 PCIe 도메인으로 묶어둔다. SPDK는 OS
 * 의 PCI 서브시스템을 거치지 않고 VMD BAR을 mmap한 뒤, 표준 PCI Configuration
 * Space 매핑(CONFIG_OFFSET_ADDR 매크로의 bus/dev/func 인코딩)을 통해 그 안
 * 슬롯들을 직접 enumerate하고 NVMe SSD를 추가 attach한다.
 *
 * 본 헤더에는 PCI Type 0/Type 1(브리지) Configuration Header, PCI Capability
 * 헤더, MSI/MSI-X Capability 구조, PCI Express Capability 구조 전체(slot/link
 * /root 레지스터 + bit-field), serial number capability 등이 정의되어 있다.
 * 모두 표준 PCI 3.0 / PCIe 4.0 사양에 따라 비트-필드/오프셋이 결정된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [SPDK 사용자] → spdk_vmd_init() (공개 API)
 *     → vmd.c가 spdk_pci_enumerate로 VMD 컨트롤러(class 0x010802) 발견
 *     → vmd_enumerate_devices: VMD BAR을 mmap한 뒤 이 헤더의 매크로/구조체로
 *        가상 트리 안의 root-port/switch/endpoint를 traversal
 *     → 발견된 NVMe endpoint를 spdk_nvme_probe와 동일하게 attach
 * 실행 컨텍스트: 호스트 유저스페이스 (메인 init 스레드).
 *
 * === 타 모듈과의 연결 ===
 * - vmd.c / vmd_internal.h: 본 헤더의 모든 타입을 가져와 vmd_pci_device, vmd_adapter
 *   등 SPDK 측 추상화로 래핑하고, VMD BAR offset 계산에 매크로(CONFIG_OFFSET_ADDR
 *   등)를 사용한다.
 * - lib/env_dpdk: 실제 BAR mmap, vfio-pci/uio 바인딩 제공.
 * - lib/nvme: VMD가 enumerate한 NVMe endpoint에 대해 attach 콜백을 발화시킨다.
 * 데이터 흐름: BIOS가 VMD BAR0(VMCFG_BAR) 안에 가상 PCI configuration space를
 *   매핑 → 본 헤더의 매크로로 (bus,dev,func,reg) 4-tuple을 BAR offset으로 변환 →
 *   읽기로 vendor/device/class id, BAR 등 통상 PCI enumeration 수행.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct pci_header_common/zero/one : PCI Type 0(endpoint), Type 1(bridge),
 *   공통 16 바이트 헤더 구조. (1바이트, 2바이트, 4바이트 필드의 정확한 오프셋과 의미)
 * - struct pci_express_cap : PCIe Capability 전체 — device/link/slot/root 각각의
 *   capability/control/status 레지스터(비트필드 union).
 * - union express_slot_capabilities_register : 슬롯의 hotplug/MRL/attention/
 *   power-indicator capability 비트.
 * - union express_link_status_register : 링크 speed/width/training 상태.
 * - struct pci_msi_cap / pci_msix_cap / pci_msix_capability / pci_msix_table_entry :
 *   MSI/MSI-X interrupt capability와 vector table entry 정의.
 * - struct serial_number_capability : Device Serial Number Extended Capability.
 * - CONFIG_OFFSET_ADDR(bus,dev,func,reg) : VMD BAR 내부에서 한 endpoint의
 *   configuration space 한 reg에 접근하는 byte offset 계산 매크로.
 * - isHotPlugCapable(slotCap) : slot capability의 bit 6(=hotplug_capable) 검사.
 * - MAX_VMD_SUPPORTED(48) : 시스템에 동시에 존재할 수 있는 VMD 컨트롤러 상한.
 */

#ifndef VMD_SPEC_H
#define VMD_SPEC_H

/* [한국어] 한 시스템에 존재 가능한 VMD 컨트롤러 최대 수 (실제 Xeon SP는 보통 ≤3개,
 *  여유분 포함). 정적 배열 크기 한정에 사용. */
#define MAX_VMD_SUPPORTED 48  /* max number of vmd controllers in a system - */

/* [한국어] PCI config 공간 vendor_id 필드가 0xFFFF면 "디바이스 없음"을 의미 (PCI 표준). */
#define PCI_INVALID_VENDORID 0xFFFF
/* [한국어] 1 MiB 상수 — BAR 크기 계산 등에 사용. */
#define ONE_MB (1<<20)
/* [한국어] 구조체의 멤버 오프셋을 계산하는 표준 트릭(NULL 포인터를 캐스팅하여 멤버 주소 추출). */
#define PCI_OFFSET_OF(object, member)  ((uint32_t)&((object*)0)->member)
/* [한국어] 2의 보수 계산 — BAR mask로부터 BAR size 추출에 사용. */
#define TWOS_COMPLEMENT(value) (~(value) + 1)

/* [한국어] VMD 가상 트리에서 BAR base/limit이 "상위 4GB 너머"에 있음을 표시하는 signature 값.
 *  PCI spec에서는 BAR base/limit 레지스터의 일부 비트로 이런 sentinel을 부호화한다. */
#define VMD_UPPER_BASE_SIGNATURE  0xFFFFFFEF
#define VMD_UPPER_LIMIT_SIGNATURE 0xFFFFFFED

/* VMD Registers */
/* [한국어] VMD 자체의 vendor-specific config 레지스터 — VMCAP은 케이퍼빌리티,
 *  VMCONFIG는 enable 비트(0번 비트가 enable). spdk_pci_device_cfg_read*로 읽음. */
#define PCI_VMD_VMCAP		0x40
#define PCI_VMD_VMCONFIG	0x44

/*
 *  BAR assignment constants
 */
/* [한국어] BAR 비트필드/플래그 정의 — PCI Local Bus Spec 3.0의 BAR layout. */
#define  PCI_DWORD_SHIFT            32  /* [한국어] 64bit BAR에서 상위 32비트로 이동하는 shift. */
#define  PCI_BASE_ADDR_MASK         0xFFFFFFF0  /* [한국어] BAR의 base 주소 비트(하위 4비트는 flags). */
#define  PCI_BAR_MEMORY_MASK        0x0000000F  /* [한국어] BAR의 하위 flags 4비트. */
#define  PCI_BAR_MEMORY_MEM_IND     0x1         /* [한국어] BAR이 IO공간 인지(=1) 메모리(=0) 인지 indicator. */
#define  PCI_BAR_MEMORY_TYPE        0x6         /* [한국어] BAR type 비트(32/64bit). */
#define  PCI_BAR_MEMORY_PREFETCH    0x8         /* [한국어] prefetchable bit. */
#define  PCI_BAR_MEMORY_TYPE_32     0x0         /* [한국어] 32-bit BAR. */
#define  PCI_BAR_MEMORY_TYPE_64     0x4         /* [한국어] 64-bit BAR (2개 BAR 슬롯 사용). */
#define  PCI_BAR_MB_MASK            0xFFFFF     /* [한국어] 1MB 단위 정렬 마스크. */
#define  PCI_PCI_BRIDGE_ADDR_DEF    0xFFF0      /* [한국어] PCI-PCI 브리지의 memory base/limit 기본값. */
#define  PCI_BRIDGE_MEMORY_MASK     0xFFF0      /* [한국어] 브리지 memory window 16비트 마스크. */
#define  PCI_BRIDGE_PREFETCH_64     0x0001      /* [한국어] 브리지의 prefetch가 64bit임을 나타내는 비트. */
#define  PCI_BRIDGE_MEMORY_SHIFT    16          /* [한국어] 브리지 memory window 값의 좌측 시프트(16비트가 상위). */
#define  PCI_CONFIG_ACCESS_DELAY    500         /* [한국어] PCI config 쓰기 후 디바이스가 반영하는 대기 시간(usec). */

/* [한국어] PCI Type 0 헤더 안에서 BAR0가 시작되는 byte offset (=0x10). */
#define PCI_BAR0_OFFSET			0x10
/* [한국어] BAR 한 개 크기는 4바이트(32bit). 64bit BAR은 2개 슬롯을 차지. */
#define PCI_BAR_SIZE			4
/* [한국어] BAR 값의 하위 4비트(flags)를 제거하고 주소부만 남기는 마스크. */
#define PCI_BAR_MEMORY_ADDR_OFFSET	(~0xfull)

/* [한국어] 한 PCI(Express) 디바이스의 extended configuration space 크기 (4KiB). */
#define PCI_MAX_CFG_SIZE            0x1000

/* [한국어] PCI 헤더의 header_type 필드 offset(=0x0e). 하위 7비트는 type, MSB는 multi-function. */
#define PCI_HEADER_TYPE             0x0e
#define PCI_HEADER_TYPE_NORMAL   0  /* [한국어] Type 0 — endpoint (NVMe SSD 등). */
#define PCI_HEADER_TYPE_BRIDGE   1  /* [한국어] Type 1 — PCI-PCI 브리지(스위치 down/upstream port). */
#define PCI_MULTI_FUNCTION 0x80     /* [한국어] header_type MSB=1이면 multi-function 디바이스. */

/* [한국어] PCI command register 의 비트들. enumerate 후 디바이스를 enable할 때 OR. */
#define PCI_COMMAND_MEMORY 0x2   /* [한국어] memory space enable. */
#define PCI_COMMAND_MASTER 0x4   /* [한국어] bus master enable (DMA 가능). */

/* [한국어] PCIe Capability 의 device_type 필드 (express_cap.bit_field.device_type) 관련. */
#define PCIE_TYPE_FLAGS 0xf0       /* [한국어] capability_register의 device_type 비트 위치(상위 4비트). */
#define PCIE_TYPE_SHIFT 4
#define PCIE_TYPE_ROOT_PORT 0x4    /* [한국어] PCIe Root Port. */
#define PCIE_TYPE_DOWNSTREAM 0x6   /* [한국어] PCIe Switch Downstream Port. */

/* [한국어] NVMe 컨트롤러의 PCI class code. 24비트 (base=01 storage, sub=08 NVM, prog=02 NVMe). */
#define PCI_CLASS_STORAGE_EXPRESS   0x010802
/* [한국어] VMD enumerate 시 한 번에 처리할 디바이스 큐 크기 한정. */
#define ADDR_ELEM_COUNT 32
/* [한국어] PCI 버스 번호는 8비트이지만 VMD 내부 버스 공간 한도는 0x7F (계층 트리 깊이 제한). */
#define PCI_MAX_BUS_NUMBER 0x7F
/* [한국어] hotplug bridge용으로 예약된 버스 갯수 — 각 hotplug downstream 1개 슬롯에 1버스 예약. */
#define RESERVED_HOTPLUG_BUSES 1
/* [한국어] PCIe Slot Capability 레지스터의 bit 6(=hotplug_capable) 검사 매크로. */
#define isHotPlugCapable(slotCap)  ((slotCap) & (1<<6))
/* [한국어] VMD BAR 내부에서 (bus,device,function,reg)에 해당하는 config space byte offset 계산.
 *  레이아웃: bus[26:20] | device[19:15] | function[14:12] | reg[11:0].
 *  VMD BAR의 시작 + 이 offset = 해당 endpoint의 reg config 주소. */
#define CONFIG_OFFSET_ADDR(bus, device, function, reg) (((bus)<<20) | (device)<<15 | (function<<12) | (reg))
/* [한국어] PCI-PCI 브리지의 16비트 memory base/limit 레지스터에서 실제 주소를 추출. */
#define BRIDGE_BASEREG(reg)  (0xFFF0 & ((reg)>>16))

/* [한국어] VMD MISCCTRLSTS_0 vendor-specific register 오프셋과 ACPI hotplug enable 비트. */
#define MISCCTRLSTS_0_OFFSET  0x188
#define ENABLE_ACPI_MODE_FOR_HOTPLUG  (1 << 3)

/* Bit encodings for Command Register */
#define IO_SPACE_ENABLE               0x0001
#define MEMORY_SPACE_ENABLE           0x0002
#define BUS_MASTER_ENABLE             0x0004

/* Bit encodings for Status Register */
#define PCI_CAPABILITIES_LIST        0x0010
#define PCI_RECEIVED_TARGET_ABORT    0x1000
#define PCI_RECEIVED_MASTER_ABORT    0x2000
#define PCI_SIGNALED_SYSTEM_ERROR    0x4000
#define PCI_DETECTED_PARITY_ERROR    0x8000

/* Capability IDs */
#define CAPABILITY_ID_POWER_MANAGEMENT  0x01
#define CAPABILITY_ID_MSI   0x05
#define CAPABILITY_ID_PCI_EXPRESS   0x10
#define CAPABILITY_ID_MSIX  0x11

#define  PCI_MSIX_ENABLE (1 << 15)          /* bit 15 of MSIX Message Control */
#define  PCI_MSIX_FUNCTION_MASK (1 << 14)   /* bit 14 of MSIX Message Control */

/* extended capability */
#define EXTENDED_CAPABILITY_OFFSET 0x100
#define DEVICE_SERIAL_NUMBER_CAP_ID  0x3

/* [한국어] 각 디바이스의 가상 BAR 영역 기본 크기 (1MiB). VMD 트리 enumerate 시 사용. */
#define BAR_SIZE (1 << 20)

/*
 * [한국어] struct pci_enhanced_capability_header — PCIe 4.0의 Extended Capability
 *  헤더 (configuration space 0x100 이후 영역 chain). 각 ext capability는 이 헤더로 시작.
 */
struct pci_enhanced_capability_header {
	uint16_t capability_id;
	/* [한국어] Extended capability ID (예: 0x0003 = Device Serial Number). PCIe 표 7-x 참조. */

	uint16_t version: 4;
	/* [한국어] capability 버전 (현재 보통 1). */

	uint16_t next: 12;
	/* [한국어] 다음 ext capability의 byte offset (0이면 chain 끝). */
};

/*
 * [한국어] struct serial_number_capability — Device Serial Number Extended Capability
 *  (PCIe spec 7.16). 64bit serial number를 (lo,hi) 로 노출.
 */
struct serial_number_capability {
	struct pci_enhanced_capability_header hdr;
	/* [한국어] capability_id=0x3, version=1을 기대. */

	uint32_t sn_low;
	/* [한국어] 시리얼 넘버 하위 32비트. */

	uint32_t sn_hi;
	/* [한국어] 시리얼 넘버 상위 32비트. */
};

/*
 * [한국어] struct pci_header_common — PCI Configuration Space의 공통 상위 64바이트
 *  (Type 0/1 공통 부분 + Type별 분기 직전까지). offset 0x00 ~ 0x3F.
 *  실제 type 분기는 header_type 필드 (offset 0x0e)를 보고 pci_header_zero / one 으로 캐스팅.
 */
struct pci_header_common {
	uint16_t  vendor_id;
	/* [한국어] offset 0x00. 0xFFFF이면 디바이스 없음. */

	uint16_t  device_id;
	/* [한국어] offset 0x02. vendor 내부 디바이스 ID. */

	uint16_t  command;
	/* [한국어] offset 0x04. PCI command register (IO/MEM/MASTER enable 등). */

	uint16_t  status;
	/* [한국어] offset 0x06. PCI status (capabilities list, error 비트들). */

	uint32_t  rev_class;
	/* [한국어] offset 0x08. revision(8bit) + class code(24bit). 0x010802 = NVMe. */

	uint8_t   cache_line_size;
	/* [한국어] offset 0x0c. cache line 단위(DWORD 수). */

	uint8_t   master_lat_timer;
	/* [한국어] offset 0x0d. legacy PCI master latency timer (PCIe에서는 0). */

	uint8_t   header_type;
	/* [한국어] offset 0x0e. bit7=multifunction, bit[6:0]=0/1/2 type. */

	uint8_t   BIST;
	/* [한국어] offset 0x0f. Built-In Self-Test register (대부분 미사용). */

	uint8_t   rsvd12[36];
	/* [한국어] offset 0x10~0x33: type별 dependent area (BAR, bridge window 등) — 공통 view에서는 reserved. */

	uint8_t   cap_pointer;
	/* [한국어] offset 0x34. 첫 capability 의 byte offset (capability chain 시작점). */

	uint8_t   rsvd53[7];
	/* [한국어] offset 0x35~0x3B: 예약. */

	uint8_t   int_line;
	/* [한국어] offset 0x3c. legacy IRQ 라인 (PCIe MSI/MSI-X에서는 보통 0xFF). */

	uint8_t   int_pin;
	/* [한국어] offset 0x3d. INTx pin (A=1..D=4). */

	uint8_t   rsvd62[2];
	/* [한국어] offset 0x3e~0x3f: type 1 brigde의 경우 bridge_control이 위치하지만 공통 view에서는 reserved. */
};

struct pci_header_zero {
	uint16_t  vendor_id;
	uint16_t  device_id;
	uint16_t  command;
	uint16_t  status;
	uint32_t  rev_class;
	uint8_t   cache_line_size;
	uint8_t   master_lat_timer;
	uint8_t   header_type;
	uint8_t   BIST;
	uint32_t  BAR[6];
	uint32_t  carbus_cis_pointer;
	uint16_t  ssvid;
	uint16_t  ssid;
	uint32_t  exp_rom_base_addr;
	uint8_t   cap_pointer;
	uint8_t   rsvd53[7];
	uint8_t   intLine;
	uint8_t   int_pin;
	uint8_t   min_gnt;
	uint8_t   max_lat;
};

struct pci_header_one {
	uint16_t  vendor_id;
	uint16_t  device_id;
	uint16_t  command;
	uint16_t  status;
	uint32_t  rev_class;
	uint8_t   cache_line_size;
	uint8_t   master_lat_timer;
	uint8_t   header_type;
	uint8_t   BIST;
	uint32_t  BAR[2];
	uint8_t   primary;
	uint8_t   secondary;
	uint8_t   subordinate;
	uint8_t   secondary_lat_timer;
	uint8_t   io_base;
	uint8_t   io_limit;
	uint16_t  secondary_status;
	uint16_t  mem_base;
	uint16_t  mem_limit;
	uint16_t  prefetch_base;
	uint16_t  prefetch_limit;
	uint32_t  prefetch_base_upper;
	uint32_t  prefetch_limit_upper;
	uint16_t  io_base_upper;
	uint16_t  io_limit_upper;
	uint8_t   cap_pointer;
	uint8_t   rsvd53[3];
	uint32_t  exp_romBase_addr;
	uint8_t   int_line;
	uint8_t   int_pin;
	uint16_t  bridge_control;
};

struct pci_capabilities_header {
	uint8_t   capability_id;
	uint8_t   next;
};

/*
 * MSI capability structure for msi interrupt vectors
 */
#define MAX_MSIX_TABLE_SIZE 0x800
#define MSIX_ENTRY_VECTOR_CTRL_MASKBIT 1
#define PORT_INT_VECTOR  0;
#define CLEAR_MSIX_DESTINATION_ID 0xfff00fff
struct pci_msi_cap {
	struct pci_capabilities_header header;
	union _MsiControl {
		uint16_t as_uint16_t;
		struct _PCI_MSI_MESSAGE_CONTROL {
			uint16_t msi_enable : 1;
			uint16_t multiple_message_capable : 3;
			uint16_t multiple_message_enable : 3;
			uint16_t capable_of_64bits : 1;
			uint16_t per_vector_mask_capable : 1;
			uint16_t reserved : 7;
		} bit;
	} message_control;
	union {
		struct _PCI_MSI_MESSAGE_ADDRESS {
			uint32_t reserved : 2;
			uint32_t address : 30;
		} reg;
		uint32_t  raw;
	} message_address_lower;
	union {
		struct _Option32_bit {
			uint16_t message_data;
		} option32_bit;
		struct _Option64_bit {
			uint32_t  message_address_upper;
			uint16_t  message_data;
			uint16_t  reserved;
			uint32_t  mask_bits;
			uint32_t  pending_bits;
		} option64_bit;
	};
};

struct pcix_table_pointer {
	union {
		struct {
			uint32_t BaseIndexRegister : 3;
			uint32_t Reserved : 29;
		} TableBIR;
		uint32_t  TableOffset;
	};
};

struct pci_msix_capability {
	struct pci_capabilities_header header;
	union _MsixControl {
		uint16_t as_uint16_t;
		struct msg_ctrl {
			uint16_t table_size : 11;
			uint16_t reserved : 3;
			uint16_t function_mask : 1;
			uint16_t msix_enable : 1;
		} bit;
	} message_control;

	struct pcix_table_pointer message_table;
	struct pcix_table_pointer   pba_table;
};

struct pci_msix_table_entry {
	volatile uint32_t  message_addr_lo;
	volatile uint32_t  message_addr_hi;
	volatile uint32_t  message_data;
	volatile uint32_t  vector_control;
};

/*
 * Pci express capability
 */
enum PciExpressCapabilities {
	/* 0001b Legacy PCI Express Endpoint            */
	LegacyEndpoint       = 0x1,
	/* 0000b PCI Express Endpoint                   */
	ExpressEndpoint      = 0x0,
	/* 0100b Root Port of PCI Express Root Complex* */
	RootComplexRootPort  = 0x4,
	/* 0101b Upstream Port of PCI Express Switch*   */
	SwitchUpstreamPort   = 0x5,
	/* 0110b Downstream Port of PCI Express Switch* */
	SwitchDownStreamPort = 0x6,
	/* 0111b PCI Express to PCI/PCI-X Bridge*       */
	ExpressToPciBridge   = 0x7,
	/* 1000b PCI/PCI-X to PCI Express Bridge*       */
	PciToExpressBridge   = 0x8,
	/* 1001b Root Complex Integrated Endpoint       */
	RCIntegratedEndpoint = 0x9,
	/* 1010b Root Complex Event Collector           */
	RootComplexEventCollector = 0xa,
	InvalidCapability = 0xff
};

union express_capability_register {
	struct {
		uint16_t capability_version : 4;
		uint16_t device_type : 4;
		uint16_t slot_implemented : 1;
		uint16_t interrupt_message_number : 5;
		uint16_t rsv : 2;
	} bit_field;
	uint16_t as_uint16_t;
};

union express_slot_capabilities_register {
	struct {
		uint32_t attention_button_present : 1;
		uint32_t power_controller_present : 1;
		uint32_t MRL_sensor_present : 1;
		uint32_t attention_indicator_present : 1;
		uint32_t power_indicator_present : 1;
		uint32_t hotplug_surprise : 1;
		uint32_t hotplug_capable : 1;
		uint32_t slot_power_limit : 8;
		uint32_t slotPower_limit_scale : 2;
		uint32_t electromechanical_lock_present : 1;
		uint32_t no_command_completed_support : 1;
		uint32_t physical_slot_number : 13;
	} bit_field;
	uint32_t as_uint32_t;
};

union express_slot_control_register {
	struct {
		uint16_t attention_button_enable : 1;
		uint16_t power_fault_detect_enable : 1;
		uint16_t MRLsensor_enable : 1;
		uint16_t presence_detect_enable : 1;
		uint16_t command_completed_enable : 1;
		uint16_t hotplug_interrupt_enable : 1;
		uint16_t attention_indicator_control : 2;
		uint16_t power_indicator_control : 2;
		uint16_t power_controller_control : 1;
		uint16_t electromechanical_lockcontrol : 1;
		uint16_t datalink_state_change_enable : 1;
		uint16_t Rsvd : 3;
	} bit_field;
	uint16_t as_uint16_t;
};

union express_slot_status_register {
	struct {
		uint16_t attention_button_pressed : 1;
		uint16_t power_fault_detected : 1;
		uint16_t MRL_sensor_changed : 1;
		uint16_t presence_detect_changed : 1;
		uint16_t command_completed : 1;
		uint16_t MRL_sensor_state : 1;
		uint16_t presence_detect_state : 1;
		uint16_t electromechanical_lock_engaged : 1;
		uint16_t datalink_state_changed : 1;
		uint16_t rsvd : 7;
	} bit_field;
	uint16_t as_uint16_t;
};

union express_root_control_register {
	struct {
		uint16_t CorrectableSerrEnable : 1;
		uint16_t NonFatalSerrEnable : 1;
		uint16_t FatalSerrEnable : 1;
		uint16_t PMEInterruptEnable : 1;
		uint16_t CRSSoftwareVisibilityEnable : 1;
		uint16_t Rsvd : 11;
	} bit_field;
	uint16_t as_uint16_t;
};

union express_link_capability_register {
	struct {
		uint32_t maximum_link_speed : 4;
		uint32_t maximum_link_width : 6;
		uint32_t active_state_pms_support : 2;
		uint32_t l0_exit_latency : 3;
		uint32_t l1_exit_latency : 3;
		uint32_t clock_power_management : 1;
		uint32_t surprise_down_error_reporting_capable : 1;
		uint32_t datalink_layer_active_reporting_capable : 1;
		uint32_t link_bandwidth_notification_capability : 1;
		uint32_t aspm_optionality_compliance : 1;
		uint32_t rsvd : 1;
		uint32_t port_number : 8;
	} bit_field;
	uint32_t as_uint32_t;
};

union express_link_control_register {
	struct {
		uint16_t active_state_pm_control : 2;
		uint16_t rsvd1 : 1;
		uint16_t read_completion_boundary : 1;
		uint16_t link_disable : 1;
		uint16_t retrain_link : 1;
		uint16_t common_clock_config : 1;
		uint16_t extended_synch : 1;
		uint16_t enable_clock_power_management : 1;
		uint16_t rsvd2 : 7;
	} bit_field;
	uint16_t as_uint16_t;
};

union express_link_status_register {
	struct {
		uint16_t link_speed : 4;
		uint16_t link_width : 6;
		uint16_t undefined : 1;
		uint16_t link_training : 1;
		uint16_t slot_clock_config : 1;
		uint16_t datalink_layer_active : 1;
		uint16_t asvd : 2;
	} bit_field;
	uint16_t as_uint16_t;
};

struct pci_express_cap {
	uint8_t capid;
	uint8_t next_cap;
	union express_capability_register express_cap_register;
	uint32_t device_cap;
	uint16_t device_control;
	uint16_t device_status;
	union express_link_capability_register link_cap;
	union express_link_control_register link_control;
	union express_link_status_register link_status;
	union express_slot_capabilities_register slot_cap;
	union express_slot_control_register slot_control;
	union express_slot_status_register slot_status;
	uint32_t root_status;
	uint32_t deviceCap2;
	uint16_t deviceControl2;
	uint16_t deviceStatus2;
	uint32_t linkCap2;
	uint16_t linkControl2;
	uint16_t linkStatus2;
	uint32_t slotCap2;
	uint16_t slotControl2;
	uint16_t slotStatus2;
};

struct pci_msix_cap {
	uint8_t   cap_idd;
	uint8_t   next_cap;
	uint16_t  msg_control_reg;
	uint32_t  msix_table_offset;
	uint32_t  pba_offset;
};

struct pci_header {
	union {
		struct pci_header_common common;
		struct pci_header_zero zero;
		struct pci_header_one one;
	};
};

#endif /* VMD_SPEC_H */
