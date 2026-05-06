/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * VMD driver public interface
 */

/*
 * [한국어 설명] Intel VMD (Volume Management Device) 드라이버 공개 헤더 (vmd.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 의 VMD 라이브러리 (lib/vmd) 가 외부에 노출하는 공개 API
 * 를 선언한다. Intel VMD 는 CPU 의 Root Complex 안에 존재하는 일종의 PCIe
 * 가상 스위치/엔드포인트로, 그 하위에 연결된 NVMe SSD 들을 OS 에게는 단일
 * VMD 디바이스로 보이게 하고 그 enumerate/관리 책임을 호스트 드라이버에
 * 위임한다. SPDK 의 VMD 라이브러리는 유저스페이스에서 VMD 의 PCI Config
 * Space 와 MMIO 윈도우를 직접 매핑해 (1) VMD 하위에 매달린 NVMe 들을
 * SPDK PCI 서브시스템에 등록하고, (2) 핫플러그/핫리무브 이벤트를 폴링하며,
 * (3) VROC (Virtual RAID On CPU) 의 LED indicator 상태 (OFF/IDENTIFY/FAULT/
 * REBUILD) 를 읽고 쓰는 기능을 제공한다. 이 헤더는 그 중 init/fini, NVMe
 * 목록 enumerate, LED 제어, 핫플러그 모니터, 디바이스 제거/재스캔 함수를
 * 묶어 외부 모듈에 노출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 의 PCI 디바이스 스택은 [spdk_env_init (DPDK EAL)] -> [SPDK PCI
 * 추상화 (env_dpdk/pci)] -> [디바이스별 드라이버 (NVMe / Virtio 등)] 의
 * 순서로 동작한다. VMD 는 일반 PCIe 와 다르게 호스트 드라이버가 명시적으로
 * 하위 디바이스를 enumerate 해야 하기 때문에, spdk_vmd_init() 이 그 사이에
 * 끼어들어 VMD 엔드포인트의 BAR 를 mmap 하고 secondary bus 를 스캔하여
 * 발견된 NVMe 들을 SPDK PCI 서브시스템에 attach 한다. 이후 spdk_nvme_probe
 * 가 호출되면 마치 직결된 NVMe 처럼 그 디바이스들을 발견하고 제어할 수 있다.
 * 따라서 VMD 라이브러리는 "Bare-metal PCIe 와 NVMe 드라이버 사이에 끼어든
 * VMD 전용 enumerate 어댑터" 위치에 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/env.h (struct spdk_pci_addr / spdk_pci_device 등 PCI 추상화),
 * env_dpdk/pci (실제 BAR 매핑/MSI 처리), DPDK EAL (vfio/uio fd 관리).
 * 사용처: app/spdk_tgt 와 같은 SPDK 애플리케이션이 spdk_subsystem_init 단계
 * 에서 spdk_vmd_init() 을 호출하면 이후 NVMe 드라이버 (lib/nvme) 가 자동
 * 으로 VMD 하위 NVMe 를 인식한다. 또한 RPC (rpc/vmd) 가 spdk_vmd_pci_device_list
 * 와 LED state 함수를 노출해 외부 관리툴이 RAID 상태에 따른 LED 점등을
 * 제어할 수 있다. 데이터 흐름은 VMD 의 secondary PCIe 트리 (Config Space)
 * → SPDK 측 spdk_pci_device 객체 → 사용자 코드 (또는 NVMe 드라이버) 로
 * 흐른다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_vmd_init(): 시스템의 모든 VMD 엔드포인트를 enumerate 하고 SPDK PCI
 *   서브시스템에 그 하위 디바이스들을 등록한다.
 * - spdk_vmd_fini(): init 으로 할당된 모든 자원 해제.
 * - spdk_vmd_pci_device_list(addr, list): 특정 VMD BDF 하위에 매달린 NVMe
 *   디바이스 목록을 반환 (최대 MAX_VMD_TARGET = 24 개).
 * - spdk_vmd_set_led_state / spdk_vmd_get_led_state: VROC LED 상태 제어/조회.
 * - spdk_vmd_hotplug_monitor(): 핫플러그/핫리무브 폴링 (주기적 호출 필요).
 * - spdk_vmd_remove_device / spdk_vmd_rescan: 수동 detach/rescan API.
 * 핵심 enum: spdk_vmd_led_state — OFF / IDENTIFY / FAULT / REBUILD / UNKNOWN.
 */

/* [한국어] SPDK_VMD_H — 헤더 가드. 이 헤더는 SPDK PCI/NVMe 초기화 코드
 * 여러 곳에서 포함되므로 가드는 필수. */
#ifndef SPDK_VMD_H
#define SPDK_VMD_H

/* [한국어] spdk/stdinc.h - SPDK 표준 include 묶음. uint32_t/size_t/int 같은
 * 정수형 typedef 와 stdbool 등이 필요하므로 포함. */
#include "spdk/stdinc.h"

/* [한국어] C++ 컴파일러에서 이 헤더를 포함할 때 함수 심볼이 망글링되는
 * 것을 막는 extern "C" 가드. SPDK 공개 헤더의 표준 패턴. */
#ifdef __cplusplus
extern "C" {
#endif

/* [한국어] spdk/env.h - SPDK 의 환경 추상화 헤더 (DPDK 위에 얹은 thin
 * 레이어). 이 헤더는 struct spdk_pci_addr (B:D.F 표현) 와 struct
 * spdk_pci_device (PCI 디바이스 핸들) 정의를 가져오므로, vmd.h 가 이들을
 * 인자로 사용하기 위해 반드시 포함해야 한다. extern "C" 안에 위치한 것은
 * env.h 가 헤더 가드를 안전하게 처리하기 때문에 무방하다. */
#include "spdk/env.h"

/* [한국어] MAX_VMD_TARGET = 24 - 단일 VMD 도메인이 enumerate 할 수 있는
 * NVMe SSD 의 이론상 최대 개수. Intel 의 디자인 가이드에 따르면 한 CPU
 * 소켓당 최대 6 개의 VMD endpoint 가 있을 수 있고, 각 VMD 는 PCIe 재할당을
 * 통해 다수의 NVMe 를 호스트할 수 있다. 24 는 대표적인 4-way RAID 등을
 * 모두 수용할 수 있도록 잡은 보수적인 상한이며, spdk_vmd_pci_device_list
 * 의 nvme_list 인자는 호출자가 정확히 이 개수의 spdk_pci_device 배열을
 * 미리 할당해 전달해야 한다. */
/* Maximum VMD devices - up to 6 per cpu */
#define MAX_VMD_TARGET  24

/*
 * [한국어]
 * spdk_vmd_init - 시스템의 모든 VMD endpoint 를 enumerate 하고 그 하위
 *                 디바이스들을 SPDK PCI 서브시스템에 등록한다.
 *
 * @return: 0 성공, -1 실패.
 *
 * 동기/배경: 표준 PCIe enumerate 흐름 (Linux PCI 코어 또는 SPDK env_dpdk
 * 의 vfio probe) 만으로는 VMD 하위 디바이스가 보이지 않는다. VMD 는
 * "보호된 root port" 처럼 동작하면서 하위 PCIe 트리를 외부에 숨기기
 * 때문이다. 따라서 SPDK 가 VMD 하위 NVMe 를 사용하려면 사용자가 NVMe
 * probe 이전에 반드시 이 함수를 호출해 VMD 가 자체적으로 하위 트리를
 * scan 하고 그 결과를 SPDK PCI 추상 레이어에 주입하도록 해야 한다.
 *
 * 동작: (1) DPDK 가 vfio/uio 로 매핑한 모든 PCI 디바이스 중 vendor=Intel,
 * class=VMD 인 것을 찾는다. (2) 각 VMD endpoint 의 BAR0 (CFGBAR) 를
 * mmap 하여 secondary PCI Config Space 에 직접 접근한다. (3) Config Space
 * 를 walk 하면서 발견된 모든 endpoint(주로 NVMe)를 SPDK PCI 디바이스로
 * 등록한다. (4) MSI/MSI-X 라우팅을 VMD endpoint 에 매핑한다.
 *
 * 실행 컨텍스트: 애플리케이션 초기화 단계 (spdk_app_start 직후, NVMe probe
 * 이전). 단일 스레드에서 1 회만 호출. 폴링 컨텍스트가 아님.
 *
 * 호출 체인:
 *   app main / RPC bdev_nvme_attach_controller (vmd 옵션) →
 *   [spdk_vmd_init] → DPDK PCI bus walk → BAR mmap → 하위 device 등록
 */
/**
 * Enumerate VMD devices and hook them into the spdk pci subsystem
 *
 * \return 0 on success, -1 on failure
 */
int spdk_vmd_init(void);

/*
 * [한국어]
 * spdk_vmd_fini - spdk_vmd_init 으로 할당된 모든 자원을 해제.
 *
 * @return: void.
 *
 * 동기/배경: 종료 시 VMD 가 매핑한 BAR mmap, secondary PCI 디바이스 등록
 * 정보, 내부 자료구조를 정리해야 한다. 호출하지 않으면 hugepage 누수와
 * /dev/uio 핸들 누수가 발생할 수 있다.
 *
 * 동작: 등록한 모든 secondary 디바이스를 SPDK PCI 서브시스템에서 detach
 * 하고, BAR mmap 을 munmap, 내부 리스트와 락을 free 한다.
 *
 * 실행 컨텍스트: 애플리케이션 종료 시점. 단일 스레드에서 1 회만 호출.
 *
 * 호출 체인:
 *   app shutdown → spdk_subsystem_fini → [spdk_vmd_fini] → munmap, free
 */
/**
 * Release any resources allocated by the VMD library via spdk_vmd_init().
 */
void spdk_vmd_fini(void);

/*
 * [한국어]
 * spdk_vmd_pci_device_list - 특정 VMD BDF 의 하위 NVMe 디바이스 목록을 반환.
 *
 * @vmd_addr: 조회할 VMD endpoint 의 PCI BDF (Bus:Device.Function). 이 BDF 는
 *            VMD 자체의 root port BDF 이며, 하위 NVMe 가 아니다.
 * @nvme_list: 호출자가 미리 할당한 spdk_pci_device 배열. 정확히
 *             MAX_VMD_TARGET (24) 크기여야 함.
 * @return: 실제 매달려 있는 NVMe 개수 (0 이상). 음수 반환은 없음.
 *
 * 동기/배경: 사용자 코드(예: RPC) 가 어떤 NVMe 가 어느 VMD 에 속해
 * 있는지를 알아야 LED 제어, RAID grouping, 토폴로지 표시 등이 가능하다.
 * spdk_pci_for_each_device 로는 VMD 와 하위 NVMe 의 부모-자식 관계를
 * 알 수 없으므로 별도 API 가 필요하다.
 *
 * 동작: VMD 라이브러리가 init 시 구축해 둔 내부 (vmd_addr → device list)
 * 매핑을 룩업하여, 일치하는 VMD 의 하위 디바이스들을 nvme_list 에 복사한다.
 *
 * 실행 컨텍스트: 임의의 SPDK 스레드. 내부 자료구조는 init 시 구축된 후
 * 변경되지 않으므로 lock-free 로 읽을 수 있다 (단, 동시에 hotplug_monitor
 * 가 돌고 있다면 결과가 약간 stale 할 수 있음).
 *
 * 호출 체인:
 *   RPC vmd handler / 사용자 토폴로지 표시 코드 → [spdk_vmd_pci_device_list]
 */
/**
 * Returns a list of nvme devices found on the given vmd pci BDF.
 *
 * \param vmd_addr pci BDF of the vmd device to return end device list
 * \param nvme_list buffer of exactly MAX_VMD_TARGET to return spdk_pci_device array.
 *
 * \return Returns count of nvme device attached to input VMD.
 */
int spdk_vmd_pci_device_list(struct spdk_pci_addr vmd_addr, struct spdk_pci_device *nvme_list);

/** State of the LEDs */
/* [한국어] enum spdk_vmd_led_state - VMD 의 VROC (Virtual RAID On CPU)
 * 표시 LED 상태를 나타내는 열거형. Intel SSD 데이터센터 폼팩터는
 * 슬롯마다 멀티컬러 LED 를 갖고 있고, VMD 가 SES (SCSI Enclosure Services)
 * 유사 명령으로 그 점등 상태를 제어한다. */
enum spdk_vmd_led_state {
	SPDK_VMD_LED_STATE_OFF,
	/* [한국어] OFF — LED 소등.
	 * 설정자: spdk_vmd_set_led_state 가 명시적으로 OFF 를 줄 때.
	 * 읽는 자: spdk_vmd_get_led_state 가 현재 OFF 상태를 보고할 때.
	 * 의미: 정상 idle 또는 사용자가 시각적 식별을 끈 상태.
	 * 동기화: 단일 LED register 를 set/get 하는 atomic ioctl 형식이라
	 * VMD 라이브러리 내부 락만 있으면 충분. */

	SPDK_VMD_LED_STATE_IDENTIFY,
	/* [한국어] IDENTIFY — 점멸하여 식별 표시.
	 * 설정자: 관리자가 RPC 등으로 "이 슬롯이 어디 있는지 점멸로 보여줘"
	 * 라고 요청하면 set_led_state 가 IDENTIFY 로 전환.
	 * 읽는 자: get_led_state.
	 * 의미: 데이터센터 운영자가 물리적으로 디스크를 찾아내기 위한 보조 표시.
	 * 동기화: OFF 와 동일. */

	SPDK_VMD_LED_STATE_FAULT,
	/* [한국어] FAULT — 빨간색 점등 (보통).
	 * 설정자: bdev_raid 등 상위 레이어가 SSD 의 fatal error 를 감지하면
	 * set_led_state 로 FAULT 를 설정.
	 * 읽는 자: get_led_state.
	 * 의미: 해당 슬롯의 SSD 가 고장났음을 시각적으로 알림.
	 * 동기화: OFF 와 동일. */

	SPDK_VMD_LED_STATE_REBUILD,
	/* [한국어] REBUILD — 다른 색 점멸.
	 * 설정자: VROC RAID 가 spare 디스크를 사용해 재빌드 중일 때.
	 * 읽는 자: get_led_state.
	 * 의미: 해당 슬롯에서 데이터 복원이 진행 중이므로 빼지 말 것.
	 * 동기화: OFF 와 동일. */

	SPDK_VMD_LED_STATE_UNKNOWN,
	/* [한국어] UNKNOWN — 상태를 읽을 수 없거나 매핑이 없음.
	 * 설정자: 일반적으로 set 되지 않으며, get_led_state 가 LED register
	 * 의 비트 패턴이 정의된 4 가지 상태와 일치하지 않을 때 반환.
	 * 읽는 자: 상위 RPC/모니터링 코드.
	 * 의미: 사용자 측 진단 메시지로만 활용 (set 인자로 사용 시 -EINVAL).
	 * 동기화: 별도 sync 불필요 (보고 전용). */
};

/*
 * [한국어]
 * spdk_vmd_set_led_state - VMD 하위 PCI 디바이스의 슬롯 LED 상태를 설정.
 *
 * @pci_device: VMD 하위에 매달린 디바이스 핸들 (NVMe 등). VMD 하위가 아닌
 *              일반 PCIe 디바이스를 넘기면 -EINVAL 또는 비지원으로 반환.
 * @state: 설정할 LED 상태 (OFF/IDENTIFY/FAULT/REBUILD). UNKNOWN 은 비허용.
 * @return: 0 성공 / 음수 errno (예: -EINVAL VMD 하위 아님, -EIO 레지스터
 *          쓰기 실패).
 *
 * 동기/배경: VROC 에 익숙한 운영자는 RAID 컨트롤러의 SES 유사 LED 동작을
 * 그대로 기대한다. SPDK 의 RPC/관리툴이 그 기능을 노출하기 위해 이 API 를
 * 사용한다.
 *
 * 동작: pci_device 에서 부모 VMD endpoint 를 역참조하고, 해당 VMD 의
 * MMIO 레지스터 중 슬롯-N LED control register 를 찾아 state 비트 패턴으로
 * 기록한다 (Intel VMD 사양에 따른 비트 인코딩 사용).
 *
 * 실행 컨텍스트: 임의의 SPDK 스레드 — 단, 동일 VMD 에 동시 set 이 일어
 * 나도 race 가 없도록 VMD 라이브러리 내부 락이 보호한다.
 *
 * 호출 체인:
 *   RPC vmd_set_led / bdev_raid 의 fault 핸들러 → [spdk_vmd_set_led_state]
 *   → MMIO write
 */
/**
 * Sets the state of the LED on specified PCI device.  The device needs to be behind VMD.
 *
 * \param pci_device PCI device
 * \param state LED state to set
 *
 * \return 0 on success, negative errno otherwise
 */
int spdk_vmd_set_led_state(struct spdk_pci_device *pci_device, enum spdk_vmd_led_state state);

/*
 * [한국어]
 * spdk_vmd_get_led_state - VMD 하위 PCI 디바이스의 현재 LED 상태를 조회.
 *
 * @pci_device: VMD 하위 디바이스 핸들. 그 외 디바이스면 -EINVAL.
 * @state: (out) 현재 LED 상태가 기록될 포인터. 정의된 4 상태에 해당하지
 *         않는 비트 패턴이면 UNKNOWN 이 기록된다.
 * @return: 0 성공 / 음수 errno (예: -EIO 레지스터 읽기 실패).
 *
 * 동기/배경: 관리툴이 현재 슬롯 표시 상태를 조회/표시하기 위해 사용.
 * 또한 set_led_state 의 read-back 검증에도 활용 가능.
 *
 * 동작: pci_device 의 부모 VMD MMIO 슬롯 LED register 를 읽어 비트 패턴을
 * spdk_vmd_led_state 로 변환.
 *
 * 실행 컨텍스트: set 과 동일.
 *
 * 호출 체인:
 *   RPC vmd_get_led / 모니터링 → [spdk_vmd_get_led_state] → MMIO read
 */
/**
 * Retrieves the state of the LED on specified PCI device.  The device needs to be behind VMD.
 *
 * \param pci_device PCI device
 * \param state current LED state
 *
 * \return 0 on success, negative errno otherwise
 */
int spdk_vmd_get_led_state(struct spdk_pci_device *pci_device, enum spdk_vmd_led_state *state);

/*
 * [한국어]
 * spdk_vmd_hotplug_monitor - VMD 하위 디바이스의 핫플러그/핫리무브 이벤트를 폴링.
 *
 * @return: 처리한 이벤트 개수 (0 이상) 또는 음수 errno.
 *
 * 동기/배경: VMD 는 일반 OS PCIe 핫플러그 흐름을 우회하기 때문에, SPDK
 * VMD 라이브러리가 직접 VMD 의 hotplug status register 를 polling 해야
 * 한다. 인터럽트 기반이 아닌 polling 모델은 SPDK 의 lockless·인터럽트
 * 회피 철학과 맞물려 있다.
 *
 * 동작: 등록된 모든 VMD endpoint 의 hotplug status 비트를 읽고, 새로
 * 삽입된 디바이스가 있으면 SPDK PCI 서브시스템에 add, 빠진 디바이스가
 * 있으면 detach 를 호출한다.
 *
 * 실행 컨텍스트: 사용자가 직접 만든 spdk_poller 안에서 주기적으로 호출하는
 * 용도 (예: 1 초 주기). 호출이 안 되면 hotplug 가 인식되지 않는다.
 *
 * 호출 체인:
 *   사용자 spdk_poller_register 콜백 → [spdk_vmd_hotplug_monitor] → MMIO
 *   read → spdk_pci_device add/remove
 */
/**
 * Checks for hotplug/hotremove events of the devices behind the VMD.  Needs to be called
 * periodically to detect them.
 *
 * \return number of hotplug events detected or negative errno in case of errors
 */
int spdk_vmd_hotplug_monitor(void);

/*
 * [한국어]
 * spdk_vmd_remove_device - VMD 하위 PCI 디바이스를 강제로 detach (소프트
 *                          핫리무브 시뮬레이션).
 *
 * @addr: 제거할 PCI 디바이스의 BDF.
 * @return: 0 성공 / 음수 errno (-ENODEV 해당 BDF 없음, -EBUSY 사용 중).
 *
 * 동기/배경: 운영 중에 RAID 디스크 교체 등을 위해 사용자가 명시적으로
 * "이 SSD 를 SPDK 에서 떼어내고 싶다" 를 표현해야 할 때 사용. 하드웨어
 * 적인 power off 와 분리해 SPDK 측의 detach 만 수행한다. 다른 모듈이
 * 아직 그 디바이스를 잡고 있으면 deferred detach 가 일어나 release 후
 * 실제 분리된다.
 *
 * 실행 컨텍스트: RPC 핸들러 (관리 명령) 등.
 *
 * 호출 체인:
 *   RPC vmd_remove_device → [spdk_vmd_remove_device] → 내부 ref-count
 *   감소 / spdk_pci_device_detach
 */
/**
 * Removes a given device from the PCI subsystem simulating a hot-remove.  If the device is being
 * actively used by another module, the actual detach might be deferred.
 *
 * \param addr Address of a PCI device to remove.
 *
 * \return 0 if the device was successfully removed, negative errno otherwise.
 */
int spdk_vmd_remove_device(const struct spdk_pci_addr *addr);

/*
 * [한국어]
 * spdk_vmd_rescan - VMD 하위 PCIe 트리를 다시 스캔하여 새로 추가되었거나
 *                   spdk_vmd_remove_device 로 떼어낸 디바이스를 재발견.
 *
 * @return: 새롭게 발견·재등록된 디바이스 수 (0 이상) 또는 음수 errno.
 *
 * 동기/배경: 사용자가 remove_device 로 detach 한 디바이스를 다시 사용하고
 * 싶을 때, 또는 hotplug_monitor 가 미처 잡지 못한 새 디바이스를 강제로
 * 인식하고 싶을 때 사용.
 *
 * 동작: 등록된 모든 VMD endpoint 의 secondary PCI Config Space 를 다시
 * walk 하여, 현재 SPDK PCI 서브시스템에 없는 디바이스를 attach.
 *
 * 실행 컨텍스트: 관리 RPC 핸들러.
 *
 * 호출 체인:
 *   RPC vmd_rescan → [spdk_vmd_rescan] → BAR walk → 신규 등록
 */
/**
 * Forces a rescan of the devices behind the VMD.  If a device was previously removed through
 * spdk_vmd_remove_device() this will cause it to be reattached.
 *
 * \return number of new devices found during scanning or negative errno on failure.
 */
int spdk_vmd_rescan(void);

/* [한국어] extern "C" 블록 종료. */
#ifdef __cplusplus
}
#endif

#endif /* SPDK_VMD_H */
/* [한국어] 헤더 가드 종료 마커. */
