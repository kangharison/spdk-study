/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2023 Samsung Electronics Co., Ltd. All rights reserved.
 */

/** \file
 * NVMe driver public API
 */

/*
 * [한국어 설명] SPDK 유저스페이스 NVMe 드라이버 공개 API (nvme.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 유저스페이스 NVMe 드라이버(lib/nvme)의 모든 외부 진입점을 정의한다.
 * 커널을 우회하여(VFIO/UIO 또는 vfio-user 통해 PCIe BAR을 user-space에 매핑) NVMe 컨트롤러를
 * polled-mode로 직접 제어하는 SPDK의 핵심 능력을 외부에 노출하는 단일 헤더이며, 다음 9개 영역을
 * 포괄한다: (1) transport_id / 컨트롤러 식별 (PCIe/RDMA/TCP/FC/vfio-user/Custom),
 * (2) probe/attach/connect/detach 라이프사이클 (sync + async 변형 모두),
 * (3) controller admin 명령 - identify/get_log_page/set_features/format/fw_download/security_send_recv/ns_attach/virt_mgmt,
 * (4) reset/disconnect/reconnect/fail 비동기 복구 경로, (5) namespace 정보 질의 + DSM/Compare/WriteZeroes 기능 지원 검사,
 * (6) I/O qpair 할당·연결·해제·재연결·process_completions, (7) NVM 명령 (read/write/compare/copy/write_zeroes/
 * dataset_management/uncorrectable + with_md/iov/ext 변형), (8) poll_group(여러 qpair을 한 스레드에서 묶어 폴링),
 * (9) NVMe-oF discovery/transport_register, ANA, CUSE 통합, statistics, hotplug callback.
 * NVMe spec 1.x/2.x admin·NVM·Fabrics 명령 집합과 SPDK 자체 추상화(qpair affinity, lockless polling, accel
 * sequence offload, memory domain DMA)를 한 파일에서 매개해 lib/bdev/module/bdev_nvme 와 사용자 앱 모두 이 헤더만으로
 * SPDK NVMe 드라이버를 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택의 '유저스페이스 블록 디바이스 드라이버' 계층에 해당한다. 호출 체인:
 *   [Application 또는 bdev_nvme module]
 *     -> spdk_nvme_probe(_ext|_async)            // 트랜스포트 enumerate
 *        -> probe_cb (사용자 필터)               // attach 여부 결정
 *           -> attach_cb (NVMe 컨트롤러 핸들 수령)
 *             -> spdk_nvme_ctrlr_alloc_io_qpair  // I/O queue pair 1개 (코어 affinity)
 *               -> spdk_nvme_ns_cmd_read/write   // NVMe NVM command 발행
 *                  -> [PCIe SQ doorbell (MMIO write32)] 또는 [RDMA SEND] 또는 [TCP PDU]
 *                     -> 디바이스가 처리 후 CQ에 entry 게시
 *                        -> spdk_nvme_qpair_process_completions()  // polled-mode
 *                           -> 사용자 cb_fn(ctx, cpl) 호출
 * 실행 컨텍스트는 호스트 유저스페이스, DPDK EAL이 핀(pinning)한 pthread 위. polled-mode이므로
 * 인터럽트 핸들러가 없으며, qpair는 1개 SPDK 스레드(=1 코어)에 affinity 고정되어 lockless 동작한다.
 * lib/bdev/module/bdev_nvme/ 가 이 API의 최대 사용자이며, bdev I/O를 NVMe ns_cmd로 1:1 또는 split하여
 * 발행한 뒤 spdk_nvme_poll_group_process_completions() 로 일괄 폴링한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 헤더(아래 #include): spdk/dma.h(memory_domain), spdk/env.h(spdk_pci_addr/dma alloc),
 * spdk/keyring.h(TLS PSK / DH-HMAC-CHAP key), spdk/nvme_spec.h(NVMe 1.4 wire format opcode/CDW/cpl 모든 비트),
 * spdk/nvmf_spec.h(NVMe-oF wire format - subnqn, TRTYPE/ADRFAM 코드), spdk/util.h(SPDK_SIZEOF, container_of).
 * 이 헤더에 의존하는 모듈: module/bdev/nvme/(가장 큰 사용자), examples/nvme/(perf/identify/hello_world),
 * lib/nvmf/(target 측에서도 init/spec 일부 공유), CUSE 통합 lib/nvme/nvme_cuse.c, vbdev_redirector,
 * test/nvme/* 자동 테스트, app/spdk_nvme_*, app/nvme_manage. 데이터 흐름: 사용자 buf -> spdk_nvme_ns_cmd_*
 * 가 (vtophys로 hugepage 물리주소 변환 -> PRP/SGL 작성 -> NVMe SQE에 임베드 -> SQ tail doorbell write32) ->
 * 컨트롤러가 DMA 수행 후 CQE 게시 -> process_completions() 가 CQ head doorbell + cb_fn 디스패치.
 * 공유 상태: spdk_nvme_ctrlr 핸들은 attach 후 해당 프로세스 전역, 동일 ctrlr 위 다수 qpair는 각자 다른 SPDK
 * 스레드에 배치 가능하나 한 qpair는 단일 스레드 전용 (lockless 핵심 규약).
 *
 * === 주요 함수/구조체 요약 ===
 *   spdk_nvme_probe / probe_async / probe_ext - 트랜스포트 enumerate + 콜백 기반 attach 트리거.
 *   spdk_nvme_connect / connect_async         - 단일 trid를 직접 연결 (다중 ctrlr 시 probe 권장).
 *   spdk_nvme_detach / detach_async           - 컨트롤러 해제 (다중 detach는 detach_ctx 누적).
 *   spdk_nvme_ctrlr_reset / disconnect / reconnect_async / reconnect_poll_async
 *                                              - 동기 reset 또는 비동기 disconnect→reconnect 두 가지 모델.
 *   spdk_nvme_ctrlr_get_data / get_regs_*     - identify controller data + CC/CSTS/CAP/VS/CMBSZ MMIO 레지스터.
 *   spdk_nvme_ctrlr_alloc_io_qpair / free_io_qpair / connect_io_qpair / reconnect_io_qpair
 *                                              - I/O qpair 라이프사이클 (한 qpair는 단일 스레드 전용).
 *   spdk_nvme_qpair_process_completions       - polled-mode CQ 비우기 + 사용자 cb_fn 호출 (반환=처리 개수).
 *   spdk_nvme_ns_cmd_read / write / compare / write_zeroes / dataset_management / copy / write_uncorrectable
 *                                              - NVMe NVM 명령 + with_md/iov/ext_io_opts 변형들.
 *   spdk_nvme_poll_group_create / add / remove / process_completions / destroy
 *                                              - 다수 qpair을 한 스레드에서 묶어 효율적으로 폴링.
 *   spdk_nvme_transport_id_parse / compare    - "trtype:PCIe traddr:0000:..." 같은 string<->trid 변환.
 *   spdk_nvme_cuse_register / unregister      - /dev/spdkcontrolN, /dev/spdknvmeNnM CUSE 노드 노출.
 *   주요 자료구조: spdk_nvme_ctrlr_opts (856B, opts_size ABI), spdk_nvme_io_qpair_opts (80B),
 *                spdk_nvme_transport_id (NVMe-oF 표준 endpoint key), spdk_nvme_ns_cmd_ext_io_opts
 *                (memory_domain/accel_sequence offload 옵션), spdk_nvme_ctrlr/qpair/ns 모두 opaque.
 */

#ifndef SPDK_NVME_H
/* [한국어] 헤더 가드 - 같은 컴파일 단위에서 nvme.h 다중 include 시 중복 선언을 방지한다.
 * SPDK 모든 공개 헤더의 SPDK_<NAME>_H_ 컨벤션이지만 본 파일은 마지막 _ 없이 SPDK_NVME_H 사용. */
#define SPDK_NVME_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 인클루드 (stdint, stddef, stdbool 등). uint32_t/uint64_t/bool/size_t 같은
 * 기본 타입과 NULL을 본 헤더 선언부에서 사용. */

#ifdef __cplusplus
/* [한국어] C++에서 이 헤더를 include할 때 모든 함수가 C linkage로 노출되도록 강제한다.
 * SPDK 라이브러리는 C로 컴파일되므로 C++ name mangling 방지 필요. */
extern "C" {
#endif

#include "spdk/dma.h"
/* [한국어] DMA memory_domain 추상화. spdk_nvme_ns_cmd_ext_io_opts.memory_domain과
 * spdk_nvme_ctrlr_get_memory_domains()에서 사용 - GPU/RDMA-registered 메모리에서 zero-copy
 * I/O를 발행할 때 페이로드 위치를 표현한다. */
#include "spdk/env.h"
/* [한국어] DPDK EAL 추상화. spdk_pci_device/spdk_pci_addr (PCIe ctrlr 식별), spdk_dma_*
 * (hugepage 기반 vfio 매핑 메모리 할당), spdk_event_handler_opts(인터럽트 모드 fd 통합)에 필요. */
#include "spdk/keyring.h"
/* [한국어] SPDK keyring 인터페이스. spdk_nvme_ctrlr_opts.tls_psk(NVMe/TCP TLS pre-shared key),
 * dhchap_key/dhchap_ctrlr_key(in-band DH-HMAC-CHAP authentication, NVMe 2.0 spec)에 사용. */
#include "spdk/nvme_spec.h"
/* [한국어] NVMe 1.4 base spec wire format. spdk_nvme_cmd(64B SQE), spdk_nvme_cpl(16B CQE),
 * spdk_nvme_ctrlr_data(4096B identify), CC/CSTS/CAP register union, opcode/CDW 비트필드 모두 정의.
 * 본 헤더의 거의 모든 함수가 이 헤더의 타입에 의존한다. */
#include "spdk/nvmf_spec.h"
/* [한국어] NVMe-oF spec wire format. SPDK_NVMF_TRTYPE_RDMA/FC/TCP, SPDK_NVMF_NQN_MAX_LEN(223),
 * SPDK_NVMF_TRADDR_MAX_LEN(256), discovery_log_page 구조체. spdk_nvme_transport_id 의 길이
 * 상한과 spdk_nvme_transport_type enum 값 매핑에 사용된다. */
#include "spdk/util.h"
/* [한국어] SPDK_STATIC_ASSERT, SPDK_SIZEOF, SPDK_COUNTOF 매크로. struct spdk_nvme_ctrlr_opts/
 * io_qpair_opts/ns_cmd_ext_io_opts 의 ABI 크기 컴파일 타임 검증에 사용. */

#define SPDK_NVME_TRANSPORT_NAME_FC		"FC"
/* [한국어] Fibre Channel 트랜스포트 문자열 식별자 - struct spdk_nvme_transport_id.trstring 에
 * 채워진다. lib/nvme/nvme_transport.c 의 transport_register 시 키로 사용 (대소문자 구분). */
#define SPDK_NVME_TRANSPORT_NAME_PCIE		"PCIE"
/* [한국어] 로컬 PCIe (NVMe over PCIe) 식별자. PCIE 트랜스포트는 NVMe-oF가 아니므로
 * SPDK_NVME_TRANSPORT_PCIE = 256 (TRTYPE 8bit 범위 밖)으로 의도적 분리. */
#define SPDK_NVME_TRANSPORT_NAME_RDMA		"RDMA"
/* [한국어] RDMA 트랜스포트 (RoCE v2/iWARP/InfiniBand). NVMe-oF spec TRTYPE=0x01 매핑. */
#define SPDK_NVME_TRANSPORT_NAME_TCP		"TCP"
/* [한국어] TCP 트랜스포트 (NVMe/TCP, TRTYPE=0x03). lib/nvme/nvme_tcp.c 가 PDU 인코딩 담당. */
#define SPDK_NVME_TRANSPORT_NAME_VFIOUSER	"VFIOUSER"
/* [한국어] vfio-user 트랜스포트 (커스텀, NVMe-oF spec 외). QEMU/SPDK 사이를 UNIX 소켓으로
 * 잇는 vfio 프로토콜 변형. lib/nvme/nvme_vfio_user.c 사용. */
#define SPDK_NVME_TRANSPORT_NAME_CUSTOM		"CUSTOM"
/* [한국어] 사용자 정의 트랜스포트. spdk_nvme_transport_register() 로 외부에서 추가한
 * 커스텀 백엔드(예: 시뮬레이터)를 식별할 때 사용. */

#define SPDK_NVMF_PRIORITY_MAX_LEN 4
/* [한국어] spdk_nvme_transport_id 의 priority 필드 문자열 표현 최대 길이.
 * "0".."31" 범위 정수를 문자열로 직렬화할 때 NULL 종료 포함 4바이트. */

/**
 * Opaque handle to a controller. Returned by spdk_nvme_probe()'s attach_cb.
 */
/*
 * [한국어]
 * struct spdk_nvme_ctrlr - 불투명(opaque) NVMe 컨트롤러 핸들 전방 선언.
 *
 * 실제 정의는 lib/nvme/nvme_internal.h 에 있다. 외부 코드는 포인터로만 다루며
 * spdk_nvme_ctrlr_*() 접근자를 통해서만 필드에 접근한다. 한 PCIe NVMe SSD 또는 한
 * NVMe-oF 컨트롤러가 정확히 1개의 spdk_nvme_ctrlr 핸들에 대응한다.
 *
 * 라이프사이클: spdk_nvme_probe()의 attach_cb 가 핸들을 발급 -> 사용자가 qpair 할당·I/O 발행 ->
 * spdk_nvme_detach() 로 해제. 컨트롤러는 reset 중에도 핸들이 유지되며, set_trid 로 같은 핸들을
 * 다른 endpoint에 재바인딩할 수 있다(NVMe-oF failover).
 */
struct spdk_nvme_ctrlr;

/**
 * NVMe controller initialization options.
 *
 * A pointer to this structure will be provided for each probe callback from spdk_nvme_probe() to
 * allow the user to request non-default options, and the actual options enabled on the controller
 * will be provided during the attach callback.
 */
/*
 * [한국어]
 * struct spdk_nvme_ctrlr_opts - 컨트롤러 attach 시 적용할 옵션 묶음 (856B, opts_size ABI).
 *
 * 사용 패턴: spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts)) 로 기본값 채우고,
 * probe_cb 안에서 사용자가 필드를 수정한 뒤, attach_cb 로 실제 적용된 값을 다시 받는다.
 * 컨트롤러가 요청 옵션을 모두 지원한다는 보장은 없으며, lib/nvme 가 협상 후 attach_cb 의 const
 * 버전으로 최종값을 전달한다.
 *
 * ABI 안전: opts_size 필드(byte 824 부근) + reserved 패딩 + sizeof()=856 STATIC_ASSERT 4중 안전장치.
 * 새 필드는 항상 끝에 추가하고 reserved 슬롯을 줄이는 방식으로 SPDK 버전 간 호환성 유지.
 */
struct spdk_nvme_ctrlr_opts {
	/**
	 * Number of I/O queues to request (used to set Number of Queues feature)
	 */
	uint32_t num_io_queues;
	/* [한국어] attach 직후 NVMe Set Features (FID=0x07, Number of Queues) 명령으로 컨트롤러에
	 * 요청할 I/O queue 개수. 컨트롤러는 요청보다 적은 수를 반환할 수 있으며, 사용자는
	 * spdk_nvme_ctrlr_alloc_io_qpair() 호출 횟수가 이 값을 넘을 수 없다.
	 * 설정자: 기본 32(get_default_ctrlr_opts), 사용자가 probe_cb에서 덮어씀.
	 * 읽는 자: lib/nvme/nvme_ctrlr.c 의 nvme_ctrlr_set_num_queues() 가 admin 명령에 사용.
	 * 값 범위: 1..65535 (NVMe spec NCQR/NSQR 16bit). 0은 의미 없음.
	 * 동기화: attach 시 1회 설정, 이후 변경 불가. */

	/**
	 * Enable submission queue in controller memory buffer
	 */
	bool use_cmb_sqs;
	/* [한국어] PCIe Controller Memory Buffer(CMB)에 SQ를 배치할지 여부. CMBSZ 레지스터에
	 * 의해 컨트롤러가 CMB를 노출하면 SQ를 호스트 RAM 대신 디바이스 BAR 영역에 두어
	 * doorbell write 후 곧장 PCIe peer-to-peer로 entry가 인식되게 만든다(latency 개선).
	 * 설정자: 사용자, 기본 false. 읽는 자: nvme_pcie_qpair_construct_io_qpair().
	 * 값 범위: true(CMB 가용시 SQ를 CMB에) / false(호스트 RAM SQ).
	 * 동기화: PCIe 트랜스포트 전용, fabrics 무시. */

	/**
	 * Don't initiate shutdown processing
	 */
	bool no_shn_notification;
	/* [한국어] detach 시 NVMe Shutdown Notification(CC.SHN=0x01 Normal) 절차를 건너뛸지.
	 * 정상적으로는 SPDK가 detach 전에 컨트롤러에 정상 종료를 알려 메타데이터 flush를
	 * 보장하지만, 핫리무브/FW crash 등에서 빠른 종료가 필요하면 true로 우회한다.
	 * 설정자: 사용자, 기본 false. 읽는 자: nvme_ctrlr_destruct() shutdown 분기. */

	/**
	 * Enable interrupts for completion notification. This is only supported within a primary
	 * SPDK process, and if enabled SPDK will not support secondary processes.
	 */
	bool enable_interrupts;
	/* [한국어] 인터럽트 모드 활성화 (SPDK 21.07+). polled-mode 대신 MSI-X 또는 fabrics fd EPOLLIN
	 * 으로 완료 통지를 받는다. spdk_nvme_qpair_get_fd() 와 spdk_thread_get_interrupt_fd_group()
	 * 통합용. 단점: 인터럽트 라우팅 비용 + secondary process 비호환.
	 * 설정자: 사용자(기본 false). 읽는 자: nvme_pcie_ctrlr_construct() 가 MSI-X vector 매핑. */

	/* Hole at byte 7. */
	uint8_t	reserved7;
	/* [한국어] 7번째 바이트 패딩. 이전 버전이 bool 4개 후 enum 시작점을 4-byte align하기 위해 둠. */

	/**
	 * Type of arbitration mechanism
	 */
	enum spdk_nvme_cc_ams arb_mechanism;
	/* [한국어] CC.AMS (Arbitration Mechanism Selected) 필드. 컨트롤러가 다중 SQ에서 명령을 어떤
	 * 우선순위로 dispatch할지 결정. RR(round-robin, 기본) / WRR(weighted round-robin) / Vendor.
	 * 설정자: 사용자. 읽는 자: nvme_ctrlr_enable() 가 CC 레지스터 작성 시 사용.
	 * 값 범위: SPDK_NVME_CC_AMS_RR=0, _WRR=1, _VS=7. WRR 선택 시 low/medium/high_priority_weight 필요. */

	/**
	 * Maximum number of commands that the controller may launch at one time.  The
	 * value is expressed as a power of two, valid values are from 0-7, and 7 means
	 * unlimited.
	 */
	uint8_t arbitration_burst;
	/* [한국어] AB(Arbitration Burst) - 한 라운드당 dispatch 가능한 명령 수의 2^n 표현.
	 * 값 0..6 = 2^0..2^6, 7 = unlimited. NVMe spec 5.21.1.1.
	 * 설정자: 사용자, 기본 0. 읽는 자: Set Features FID=0x01(Arbitration). */

	/**
	 * Number of commands that may be executed from the low priority queue in each
	 * arbitration round.  This field is only valid when arb_mechanism is set to
	 * SPDK_NVME_CC_AMS_WRR (weighted round robin).
	 */
	uint8_t low_priority_weight;
	/* [한국어] WRR low priority 가중치(0-255). arb_mechanism이 WRR일 때만 유효. */

	/**
	 * Number of commands that may be executed from the medium priority queue in each
	 * arbitration round.  This field is only valid when arb_mechanism is set to
	 * SPDK_NVME_CC_AMS_WRR (weighted round robin).
	 */
	uint8_t medium_priority_weight;
	/* [한국어] WRR medium priority 가중치(0-255). WRR 모드에서만 의미. */

	/**
	 * Number of commands that may be executed from the high priority queue in each
	 * arbitration round.  This field is only valid when arb_mechanism is set to
	 * SPDK_NVME_CC_AMS_WRR (weighted round robin).
	 */
	uint8_t high_priority_weight;
	/* [한국어] WRR high priority 가중치(0-255). qpair 생성 시 qprio 필드와 결합되어
	 * 해당 SQ의 dispatch 빈도가 결정된다. */

	/**
	 * Keep alive timeout in milliseconds (0 = disabled).
	 *
	 * The NVMe library will set the Keep Alive Timer feature to this value and automatically
	 * send Keep Alive commands as needed.  The library user must call
	 * spdk_nvme_ctrlr_process_admin_completions() periodically to ensure Keep Alive commands
	 * are sent.
	 */
	uint32_t keep_alive_timeout_ms;
	/* [한국어] NVMe-oF Keep Alive timeout(ms). 컨트롤러가 이 시간 내 Keep Alive 명령을 받지
	 * 못하면 호스트 연결을 죽었다고 판단하고 정리. 0=비활성. SPDK가 KATO/2 주기로 자동 발행.
	 * 설정자: 사용자(fabrics 권장 10000ms 이상). 읽는 자: nvme_ctrlr_keep_alive 타이머. */

	/**
	 * Specify the retry number when there is issue with the transport
	 */
	uint8_t transport_retry_count;
	/* [한국어] 트랜스포트 레벨 재시도 횟수. PCIe는 보통 4, RDMA/TCP는 더 큰 값.
	 * SQE 발행 후 timeout 또는 transport error 시 같은 명령을 재발행하는 최대 횟수. */

	/* Hole at bytes 21-23. */
	uint8_t reserved21[3];
	/* [한국어] 패딩. 다음 필드 io_queue_size(uint32)를 4-byte align 시키기 위해 둠. */

	/**
	 * The queue depth of each NVMe I/O queue.
	 */
	uint32_t io_queue_size;
	/* [한국어] 각 I/O queue의 SQ/CQ 엔트리 수(=queue depth). NVMe spec 상 1..65536.
	 * 컨트롤러의 MQES(CAP[15:0])+1 값을 초과할 수 없다. SPDK 기본 256.
	 * 설정자: 사용자(probe_cb). 읽는 자: alloc_io_qpair() 가 SQ/CQ DMA 메모리 크기 결정.
	 * 동기화: attach 시 1회 결정 후 변경 불가, qpair마다 io_qpair_opts.io_queue_size로 override 가능. */

	/**
	 * The host NQN to use when connecting to NVMe over Fabrics controllers.
	 *
	 * If empty, a default value will be used.
	 */
	char hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	/* [한국어] NVMe-oF Host NQN(NVMe Qualified Name) - "nqn.YYYY-MM.<reverse-DN>:..." 형식,
	 * 최대 223+1=224 byte. 빈 문자열이면 "nqn.2014-08.org.nvmexpress:uuid:<random>" 자동생성.
	 * fabrics Connect 명령(CDW10/11)에 임베드되어 target subsystem이 host를 식별. */

	/**
	 * The number of requests to allocate for each NVMe I/O queue.
	 *
	 * This should be at least as large as io_queue_size.
	 *
	 * A single I/O may allocate more than one request, since splitting may be necessary to
	 * conform to the device's maximum transfer size, PRP list compatibility requirements,
	 * or driver-assisted striping.
	 */
	uint32_t io_queue_requests;
	/* [한국어] qpair마다 사전 할당할 spdk_nvme_request 객체 수. SQE보다 많아야 split I/O 흡수.
	 * 큰 I/O가 MDTS/PRP 제약으로 N개로 split되면 N개 request 슬롯을 동시에 차지.
	 * 기본 io_queue_size*2 정도. 부족하면 ns_cmd_*가 -ENOMEM 반환. */

	/**
	 * Source address for NVMe-oF connections.
	 * Set src_addr and src_svcid to empty strings if no source address should be
	 * specified.
	 */
	char src_addr[SPDK_NVMF_TRADDR_MAX_LEN + 1];
	/* [한국어] fabrics 클라이언트 측 source IP(또는 WWN). 멀티홈 호스트에서 특정 NIC을 강제할 때 사용.
	 * 빈 문자열이면 OS가 라우팅 테이블로 자동 선택. */

	/**
	 * Source service ID (port) for NVMe-oF connections.
	 * Set src_addr and src_svcid to empty strings if no source address should be
	 * specified.
	 */
	char src_svcid[SPDK_NVMF_TRSVCID_MAX_LEN + 1];
	/* [한국어] fabrics source 포트번호 문자열. 일반적으로 비워두며 ephemeral port 사용. */

	/**
	 * The host identifier to use when connecting to controllers with 64-bit host ID support.
	 *
	 * Set to all zeroes to specify that no host ID should be provided to the controller.
	 */
	uint8_t host_id[8];
	/* [한국어] NVMe Host Identifier (Set Features FID=0x81, 64bit). 일부 컨트롤러가 reservation/
	 * persistent state 추적에 사용. 모든 0이면 미설정 의미로 SPDK가 명령 생략. */

	/**
	 * The host identifier to use when connecting to controllers with extended (128-bit) host ID support.
	 *
	 * Set to all zeroes to specify that no host ID should be provided to the controller.
	 */
	uint8_t extended_host_id[16];
	/* [한국어] 128bit 확장 Host ID. 컨트롤러가 EXHID(Extended Host Identifier) 지원시 사용,
	 * 미지원이면 64bit host_id 만 전송. */

	/* Hole at bytes 570-571. */
	uint8_t reserved570[2];
	/* [한국어] 패딩. 다음 enum 필드 정렬을 위함. */

	/**
	 * The I/O command set to select.
	 *
	 * If the requested command set is not supported, the controller
	 * initialization process will not proceed. By default, the NVM
	 * command set is used.
	 */
	enum spdk_nvme_cc_css command_set;
	/* [한국어] CC.CSS (Controller Configuration - I/O Command Set Selected). NVMe 1.4 spec
	 * 의 NVM(0)/Admin only/I/O Command Set(0x06, ZNS·KV 같은 다중 command set 활성화).
	 * 컨트롤러가 CAP.CSS 비트맵으로 지원 가능 set을 광고하며 미지원이면 attach 실패. */

	/**
	 * Admin commands timeout in milliseconds (0 = no timeout).
	 *
	 * The timeout value is used for admin commands submitted internally
	 * by the nvme driver during initialization, before the user is able
	 * to call spdk_nvme_ctrlr_register_timeout_callback(). By default,
	 * this is set to 120 seconds, users can change it in the probing
	 * callback.
	 */
	uint32_t admin_timeout_ms;
	/* [한국어] init phase admin 명령 timeout(ms). probe/identify/set_features 등 SPDK 내부
	 * admin 명령에 적용. user-level register_timeout_callback() 등록 전 init 단계에만 의미.
	 * 기본 120000 (120초). NVMe-oF reconnect 시간을 넉넉히 보려면 큰 값 권장. */

	/**
	 * It is used for TCP transport.
	 *
	 * Set to true, means having header digest for the header in the NVMe/TCP PDU
	 */
	bool header_digest;
	/* [한국어] NVMe/TCP PDU header에 CRC32C digest 추가 여부. 와이어 무결성 보호.
	 * 양 끝(target/initiator) 모두 활성화해야 connect 성공. CPU 비용 trade-off. */

	/**
	 * It is used for TCP transport.
	 *
	 * Set to true, means having data digest for the data in the NVMe/TCP PDU
	 */
	bool data_digest;
	/* [한국어] NVMe/TCP PDU data 영역 CRC32C digest 추가 여부. header_digest와 독립 협상. */

	/**
	 * Disable logging of requests that are completed with error status.
	 *
	 * Defaults to 'false' (errors are logged).
	 */
	bool disable_error_logging;
	/* [한국어] cpl.status가 0 아닌 완료에 대한 SPDK_ERRLOG 출력 억제. fio 워크로드처럼 알려진
	 * 에러를 의도적으로 발생시키는 경우 로그 폭주 방지. 기본 false. */

	/**
	 * It is used for both RDMA & TCP transport
	 * Specify the transport ACK timeout. The value should be in range 0-31 where 0 means
	 * use driver-specific default value.
	 * RDMA: The value is applied to each qpair
	 * and affects the time that qpair waits for transport layer acknowledgement
	 * until it retransmits a packet. The value should be chosen empirically
	 * to meet the needs of a particular application. A low value means less time
	 * the qpair waits for ACK which can increase the number of retransmissions.
	 * A large value can increase the time the connection is closed.
	 * The value of ACK timeout is calculated according to the formula
	 * 4.096 * 2^(transport_ack_timeout) usec.
	 * TCP: The value is applied to each qpair
	 * and affects the time that qpair waits for transport layer acknowledgement
	 * until connection is closed forcefully.
	 * The value of ACK timeout is calculated according to the formula
	 * 2^(transport_ack_timeout) msec.
	 */
	uint8_t transport_ack_timeout;
	/* [한국어] RDMA/TCP transport ACK timeout. 0..31 (0=드라이버 기본).
	 * RDMA: 4.096*2^val usec, TCP: 2^val msec. NVMe-oF 5.20.1.18 spec. */

	/**
	 * The queue depth of NVMe Admin queue.
	 */
	uint16_t admin_queue_size;
	/* [한국어] Admin Queue depth. 기본 32, 최소 16(spec). I/O qpair와 분리되어 별도 SQ/CQ.
	 * 너무 작으면 init 시 동시 admin 명령 부족, 너무 크면 메모리 낭비. */

	/* Hole at bytes 586-591. */
	uint8_t reserved586[6];
	/* [한국어] 패딩. opts_size(size_t=8B) align 위함. */

	/**
	 * The size of spdk_nvme_ctrlr_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 */
	size_t opts_size;
	/* [한국어] ★ ABI 호환성 핵심 필드. 호출자가 컴파일된 sizeof(spdk_nvme_ctrlr_opts) 값을 적어
	 * 라이브러리가 어디까지 유효한지 식별 -> 그 이후 필드는 기본값 충전. SPDK 버전 갭에서도
	 * 새 필드 추가에 ABI 깨짐 방지. SPDK_SIZEOF(opts, last_member) 매크로 사용 권장. */

	/**
	 * The amount of time to spend before timing out during fabric connect on qpairs associated with
	 * this controller in microseconds.
	 */
	uint64_t fabrics_connect_timeout_us;
	/* [한국어] fabrics Connect 명령 timeout(us). NVMe-oF qpair 연결 시 응답 대기 시간.
	 * 너무 짧으면 부하 큰 target에서 false-fail. 기본 30초. */

	/**
	 * Disable reading ANA log page. The upper layer should reading ANA log page instead
	 * if set to true.
	 *
	 * Default is `false` (ANA log page is read).
	 */
	bool disable_read_ana_log_page;
	/* [한국어] ANA(Asymmetric Namespace Access) log page를 SPDK가 자동으로 읽지 않게 함.
	 * bdev_nvme 멀티패스 모듈처럼 상위에서 직접 ANA 로그를 관리할 때 true.
	 * 기본 false: SPDK가 NS_ATTR_CHANGED AEN 시 자동 갱신. */

	/* Hole at bytes 610-616. */
	uint8_t reserved610[7];
	/* [한국어] 패딩. 다음 disable_read_changed_ns_list_log_page (1B) 위치까지 정렬. */

	/**
	 * Disable reading CHANGED_NS_LIST log page in response to an NS_ATTR_CHANGED AEN
	 * The upper layer should reading CHANGED_NS_LIST log page instead if set to true.
	 *
	 * Default is `false` (CHANGED_NS_LIST log page is read).
	 */
	uint8_t disable_read_changed_ns_list_log_page;
	/* [한국어] NS_ATTR_CHANGED AEN(Asynchronous Event Notification) 수신 시 SPDK가 자동으로
	 * CHANGED_NS_LIST log page(LID=0x70 등)를 읽는 동작을 끔. 멀티패스 상위가 직접 폴링할 때 사용. */

	/* Hole at bytes 617-816. */
	uint8_t reserved617[200];
	/* [한국어] 큰 패딩 슬롯. 향후 SPDK 버전이 새 옵션을 추가할 때 ABI 깨지 않고 채울 영역. */

	/**
	 * It is used for RDMA transport.
	 *
	 * Set the IP protocol type of service value for RDMA transport. Default is 0, which means that the TOS will not be set.
	 */
	uint8_t transport_tos;
	/* [한국어] RDMA용 IP TOS(Type of Service) 값(DSCP). 0=기본(설정 안함). DCB/QoS 환경에서
	 * RoCE 트래픽을 우선순위 지정할 때 사용. */

	/**
	 * Pre-shared key for NVMe/TCP's TLS connection.
	 */
	struct spdk_key *tls_psk;
	/* [한국어] NVMe/TCP TLS 1.3 PSK(Pre-Shared Key). spdk/keyring.h 의 spdk_key 핸들.
	 * NULL이면 plain TCP, 비-NULL이면 TLS 핸드셰이크. NVMe Boot spec PSK identity 형식 준수. */

	/**
	 * In-band authentication DH-HMAC-CHAP host key.
	 */
	struct spdk_key *dhchap_key;
	/* [한국어] 호스트 측 DH-HMAC-CHAP 키 (NVMe 2.0 in-band auth). Connect 후 별도 auth send/recv
	 * 명령으로 challenge-response 인증. NULL이면 unauthenticated. */

	/**
	 * In-band authentication DH-HMAC-CHAP controller key.
	 */
	struct spdk_key *dhchap_ctrlr_key;
	/* [한국어] 컨트롤러 측 DH-HMAC-CHAP 키. 양방향 인증(bidirectional)일 때 컨트롤러를 호스트가
	 * 검증하기 위해 필요. NULL=일방향 인증. */

	/**
	 * Allowed digests in in-band authentication.  Each bit corresponds to one of the
	 * spdk_nvmf_dhchap_hash values.
	 */
	uint32_t dhchap_digests;
	/* [한국어] DH-HMAC-CHAP 협상에서 호스트가 허용하는 hash algorithm 비트맵.
	 * SHA-256(bit 0)/SHA-384(bit 1)/SHA-512(bit 2). 0=라이브러리 기본. */

	/**
	 * Allowed Diffie-Hellman groups in in-band authentication.  Each bit corresponds to one of
	 * the spdk_nvmf_dhchap_dhgroup values.
	 */
	uint32_t dhchap_dhgroups;
	/* [한국어] DH-HMAC-CHAP 협상에서 허용 DH group(2048/3072/4096/6144/8192-bit MODP) 비트맵.
	 * 0=라이브러리 기본. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ctrlr_opts) == 856, "Incorrect size");
/* [한국어] 컴파일 타임 ABI 검증 - struct spdk_nvme_ctrlr_opts 가 정확히 856B여야 함.
 * 새 필드 추가 시 reserved 패딩에서만 가져오고 sizeof를 절대 키우지 않는 SPDK 컨벤션. */

/**
 * NVMe acceleration operation callback.
 *
 * \param cb_arg The user provided arg which is passed to the corresponding accelerated function call
 * defined in struct spdk_nvme_accel_fn_table.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_nvme_accel_completion_cb - accel sequence 전체 완료 콜백 typedef.
 *
 * @cb_arg: append/finish 시 사용자가 넘긴 컨텍스트.
 * @status: 0=정상, 음수 errno=실패.
 *
 * 호출 컨텍스트: accel sequence를 등록한 spdk_thread (즉 qpair 소속 스레드)에서 호출.
 * lib/accel 의 backend(IOAT/DSA/SW)가 비동기로 작업을 마치고 완료 콜백을 dispatch한다.
 * accel offload는 NVMe drvier가 sequence(crc32/copy 등)를 lib/accel 에 위임해 CPU 절감.
 */
typedef void (*spdk_nvme_accel_completion_cb)(void *cb_arg, int status);

/**
 * Completion callback for a single operation in a sequence.
 *
 * \param cb_arg Argument provided by the user when appending an operation to a sequence.
 */
/*
 * [한국어]
 * spdk_nvme_accel_step_cb - sequence 내 개별 step 완료 콜백.
 *
 * append_crc32c/append_copy 마다 별도 콜백을 등록해 step 완료를 추적할 수 있다. 보통 NULL
 * (전체 finish_sequence 콜백만 사용). status 인자가 없는 이유는 step 실패는 sequence 전체 실패로
 * 묶여 finish 콜백의 status로 보고되기 때문.
 */
typedef void (*spdk_nvme_accel_step_cb)(void *cb_arg);

/**
 * Function table for the NVMe accelerator device.
 *
 * This table provides a set of APIs to allow user to leverage
 * accelerator functions.
 */
/*
 * [한국어]
 * struct spdk_nvme_accel_fn_table - poll group이 accel offload 능력을 외부에서 받는 vtable.
 *
 * 사용자(예: bdev_nvme)가 spdk_nvme_poll_group_create(opts.accel_fn_table=...) 시 채워서 전달.
 * NVMe 드라이버는 데이터 복사/CRC32C 같은 일반 연산을 직접 수행하지 않고 lib/accel 백엔드(IOAT/
 * DSA/SW)에 위임 -> CPU 절감 + 메모리 대역폭 절감. SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED
 * 플래그가 켜진 컨트롤러에서만 의미.
 *
 * ABI: table_size + reserved 패딩으로 새 step 추가에도 호환성 유지.
 */
struct spdk_nvme_accel_fn_table {
	/**
	 * The size of spdk_nvme_accel_fun_table according to the caller of
	 * this library is used for ABI compatibility. The library uses this
	 * field to know how many fields in this structure are valid.
	 * And the library will populate any remaining fields with default values.
	 * Newly added fields should be put at the end of the struct.
	 */
	size_t table_size;
	/* [한국어] 호출자가 컴파일된 sizeof(struct spdk_nvme_accel_fn_table). 라이브러리는 이 값을
	 * 보고 어디까지 fn 포인터가 유효한지 결정. 새 step 추가에 대비한 ABI 가드. */

	/* Hole at bytes 8-15. */
	uint8_t reserved8[8];
	/* [한국어] 패딩. 다음 함수 포인터들을 8-byte align 시키기 위함. */

	/** Finish an accel sequence */
	void (*finish_sequence)(void *seq, spdk_nvme_accel_completion_cb cb_fn, void *cb_arg);
	/* [한국어] 빌드된 sequence를 백엔드 큐에 넣고 실행 시작. cb_fn(cb_arg, status)는 모든 step이
	 * 끝난 후 호출. NVMe driver가 ns_cmd 발행 직전 호출하여 호스트 메모리 정리/CRC 계산을 마침. */

	/** Reverse an accel sequence */
	void (*reverse_sequence)(void *seq);
	/* [한국어] sequence step 순서 뒤집기. 보통 read 경로에서 사용 - DMA 후 CRC 검증을 위해
	 * NVMe 완료 후 sequence를 reverse하여 실행. */

	/** Abort an accel sequence */
	void (*abort_sequence)(void *seq);
	/* [한국어] 발행 안 된 sequence 폐기. NVMe 명령 발행 실패 시 메모리 누수 방지. */

	/** Append a crc32c operation to a sequence */
	int (*append_crc32c)(void *ctx, void **seq, uint32_t *dst, struct iovec *iovs, uint32_t iovcnt,
			     struct spdk_memory_domain *memory_domain, void *domain_ctx,
			     uint32_t seed, spdk_nvme_accel_step_cb cb_fn, void *cb_arg);
	/* [한국어] sequence에 CRC32C 계산 step 추가. iovs[]를 seed로 시작해 *dst에 저장.
	 * memory_domain != NULL이면 GPU/RDMA 메모리 직접 처리. T10 DIF guard 계산 등에 사용. */

	/** Append a copy operation to a sequence */
	int (*append_copy)(void *ctx, void **seq, struct iovec *dst_iovs, uint32_t dst_iovcnt,
			   struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			   struct iovec *src_iovs, uint32_t src_iovcnt,
			   struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			   spdk_nvme_accel_step_cb cb_fn, void *cb_arg);
	/* [한국어] sequence에 메모리 복사 step 추가. src/dst 각각 다른 memory_domain 가능 ->
	 * cross-domain copy(예: GPU mem -> NVMe BAR). 큰 I/O bounce buffer 회피에 핵심. */
};

/**
 * Indicate whether a ctrlr handle is associated with a Discovery controller.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return true if a discovery controller, else false.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_is_discovery - 핸들이 NVMe-oF Discovery controller인지 검사.
 *
 * @ctrlr: 컨트롤러 핸들.
 * @return: true=discovery, false=일반 I/O 컨트롤러.
 *
 * Discovery controller는 NVMe-oF spec 의 SUBNQN="nqn.2014-08.org.nvmexpress.discovery" 인
 * 특수 컨트롤러로, I/O qpair를 가지지 않고 GET_LOG_PAGE LID=0x70(Discovery) 만 지원한다.
 * 호출자: bdev_nvme 가 connect 결과를 분기할 때 사용.
 */
bool spdk_nvme_ctrlr_is_discovery(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Indicate whether a ctrlr handle is associated with a fabrics controller.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return true if a fabrics controller, else false.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_is_fabrics - 컨트롤러가 fabrics(RDMA/TCP/FC/vfio-user) 트랜스포트인지.
 *
 * @return: true=fabrics, false=PCIe(또는 CUSTOM 비-fabrics).
 * PCIe 전용 호출(set_hotplug_filter, get_pci_device 등) 분기에 사용.
 */
bool spdk_nvme_ctrlr_is_fabrics(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the default options for the creation of a specific NVMe controller.
 *
 * \param[out] opts Will be filled with the default option.
 * \param opts_size Must be set to sizeof(struct spdk_nvme_ctrlr_opts).
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_default_ctrlr_opts - 옵션 구조체를 SPDK 기본값으로 채움.
 *
 * @opts: [out] 호출자가 미리 할당. 라이브러리가 모든 필드를 채우고 opts_size도 기록.
 * @opts_size: 호출자 컴파일 시 sizeof(struct spdk_nvme_ctrlr_opts). ABI 호환성용.
 *
 * 사용 패턴: 사용자가 probe_cb 안에서 또는 connect 전에 이 함수 호출 -> 일부 필드 사용자가 수정 ->
 * 그 opts를 spdk_nvme_connect()/probe_cb 반환값에 사용. opts_size 보다 작은 SPDK가 아는 영역만
 * 채워지고 나머지는 그대로(0).
 *
 * 호출 체인: [Application] → spdk_nvme_ctrlr_get_default_ctrlr_opts() → memset+상수 채움.
 */
void spdk_nvme_ctrlr_get_default_ctrlr_opts(struct spdk_nvme_ctrlr_opts *opts,
		size_t opts_size);

/*
 * Get the options in use for a given controller.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_opts - attach 후 컨트롤러에 실제로 적용된 opts를 const 포인터로 반환.
 *
 * 컨트롤러 내부 spdk_nvme_ctrlr_opts 구조체를 직접 가리키므로 수정 금지. 사용자가 probe_cb에서
 * 요청한 값과 컨트롤러가 협상한 결과(예: num_io_queues가 32 요청 -> 16 부여)를 비교할 때 유용.
 * 동시성: 핸들이 살아있는 동안만 유효, attach/detach 사이에서만 호출.
 */
const struct spdk_nvme_ctrlr_opts *spdk_nvme_ctrlr_get_opts(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Reason for qpair disconnect at the transport layer.
 *
 * NONE implies that the qpair is still connected while UNKNOWN means that the
 * qpair is disconnected, but the cause was not apparent.
 */
/*
 * [한국어]
 * enum spdk_nvme_qp_failure_reason - qpair 단절 원인 분류.
 *
 * spdk_nvme_qpair_get_failure_reason()/spdk_nvme_ctrlr_get_admin_qp_failure_reason() 반환값.
 * 상위 계층(bdev_nvme 멀티패스)이 fail-over 정책 결정 시 LOCAL(이 호스트 문제) vs REMOTE
 * (target 측) 구분에 사용한다.
 */
enum spdk_nvme_qp_failure_reason {
	SPDK_NVME_QPAIR_FAILURE_NONE = 0,
	/* [한국어] qpair가 정상 연결 상태 - 단절 아님. 값 0(default). */
	SPDK_NVME_QPAIR_FAILURE_LOCAL,
	/* [한국어] 호스트 측 원인 (PCIe AER/RDMA QP error/TCP socket close 등). */
	SPDK_NVME_QPAIR_FAILURE_REMOTE,
	/* [한국어] target 측 원인 (NVMe-oF abort, controller reset 통보 등). */
	SPDK_NVME_QPAIR_FAILURE_UNKNOWN,
	/* [한국어] 단절은 확실하나 원인 식별 불가. 보통 transport-specific recovery 후 분류. */
	SPDK_NVME_QPAIR_FAILURE_RESET,
	/* [한국어] 사용자 또는 SPDK가 ctrlr_reset/disconnect를 명시적으로 호출해 의도된 단절. */
};

typedef enum spdk_nvme_qp_failure_reason spdk_nvme_qp_failure_reason;
/* [한국어] coding style 일관성을 위한 typedef - 함수 반환 타입에서 enum 키워드 생략 가능. */

/**
 * NVMe library transports
 *
 * NOTE: These are mapped directly to the NVMe over Fabrics TRTYPE values, except for PCIe,
 * which is a special case since NVMe over Fabrics does not define a TRTYPE for local PCIe.
 *
 * Currently, this uses 256 for PCIe which is intentionally outside of the 8-bit range of TRTYPE.
 * If the NVMe-oF specification ever defines a PCIe TRTYPE, this should be updated.
 */
/*
 * [한국어]
 * enum spdk_nvme_transport_type - SPDK NVMe 라이브러리가 지원하는 트랜스포트 종류.
 *
 * RDMA/FC/TCP 값은 NVMe-oF spec TRTYPE(8bit)에 1:1 매핑. PCIe와 비-spec 트랜스포트는 8bit 밖
 * (256/1024/4096)에 배치하여 spdk_nvme_trtype_is_fabrics() 가 단순 비교로 분류 가능.
 */
enum spdk_nvme_transport_type {
	/**
	 * PCIe Transport (locally attached devices)
	 */
	SPDK_NVME_TRANSPORT_PCIE = 256,
	/* [한국어] 로컬 PCIe NVMe SSD. lib/nvme/nvme_pcie.c, VFIO/UIO를 통해 BAR를 user space에 매핑. */

	/**
	 * RDMA Transport (RoCE, iWARP, etc.)
	 */
	SPDK_NVME_TRANSPORT_RDMA = SPDK_NVMF_TRTYPE_RDMA,
	/* [한국어] RDMA = NVMe-oF TRTYPE 0x01. ibv_post_send/recv 기반. lib/nvme/nvme_rdma.c. */

	/**
	 * Fibre Channel (FC) Transport
	 */
	SPDK_NVME_TRANSPORT_FC = SPDK_NVMF_TRTYPE_FC,
	/* [한국어] FC-NVMe = TRTYPE 0x02. 외부 fc-nvme 라이브러리(libfc) 의존. */

	/**
	 * TCP Transport
	 */
	SPDK_NVME_TRANSPORT_TCP = SPDK_NVMF_TRTYPE_TCP,
	/* [한국어] NVMe/TCP = TRTYPE 0x03. spdk/sock 기반 PDU 송수신. lib/nvme/nvme_tcp.c. */

	/**
	 * Custom VFIO User Transport (Not spec defined)
	 */
	SPDK_NVME_TRANSPORT_VFIOUSER = 1024,
	/* [한국어] vfio-user (libvfio-user). QEMU/SPDK 사이 UNIX 소켓 기반 vfio 프로토콜. spec 외. */

	/**
	 * Custom Transport (Not spec defined)
	 */
	SPDK_NVME_TRANSPORT_CUSTOM = 4096,
	/* [한국어] 사용자가 spdk_nvme_transport_register() 로 추가한 비-fabrics 트랜스포트(예: 시뮬레이터). */

	/**
	 * Custom Fabric Transport (Not spec defined)
	 */
	SPDK_NVME_TRANSPORT_CUSTOM_FABRICS = 4097,
	/* [한국어] 사용자 정의 fabrics 트랜스포트. is_fabrics() 검사 시 true 반환되도록 별도 값. */
};

/*
 * [한국어]
 * spdk_nvme_trtype_is_fabrics - trtype이 fabrics인지 판별 (인라인).
 *
 * @trtype: 검사할 트랜스포트 타입.
 * @return: true=fabrics(RDMA/FC/TCP/CUSTOM_FABRICS), false=로컬(PCIe/VFIOUSER/CUSTOM).
 *
 * 구현 트릭: 비-fabrics trtype을 의도적으로 256 이상에 배치 → 8bit 범위(<=UINT8_MAX)면 fabrics.
 * 단, CUSTOM_FABRICS(4097)는 8bit 밖이므로 별도 OR 조건. 호출자: PCIe vs fabrics 분기 모든 곳.
 */
static inline bool spdk_nvme_trtype_is_fabrics(enum spdk_nvme_transport_type trtype)
{
	/* We always define non-fabrics trtypes outside of the 8-bit range
	 * of NVMe-oF trtype.
	 */
	/* [한국어] PCIe(256) 같은 비-fabrics 값은 의도적으로 8bit 밖에 둠 → 단순 비교로 분류 가능. */
	return trtype <= UINT8_MAX || trtype == SPDK_NVME_TRANSPORT_CUSTOM_FABRICS;
	/* [한국어] 8bit 이내면 NVMe-oF spec 정의 트랜스포트(반드시 fabrics) + custom fabrics 명시 추가. */
}

/* typedef added for coding style reasons */
typedef enum spdk_nvme_transport_type spdk_nvme_transport_type_t;
/* [한국어] enum 키워드 없이 사용하기 위한 typedef. struct spdk_nvme_transport_poll_group_stat
 * 같은 멤버 타입 선언에서 더 간결한 형태로 쓰임. */

/**
 * NVMe transport identifier.
 *
 * This identifies a unique endpoint on an NVMe fabric.
 *
 * A string representation of a transport ID may be converted to this type using
 * spdk_nvme_transport_id_parse().
 */
/*
 * [한국어]
 * struct spdk_nvme_transport_id - NVMe 컨트롤러 endpoint를 유일하게 식별하는 키.
 *
 * SPDK 전반에서 "어느 NVMe 컨트롤러"를 가리킬 때 사용하는 표준 키. PCIe는 (trtype, traddr=BDF)로
 * 식별, fabrics는 (trtype, traddr=IP, trsvcid=port, subnqn) 4-tuple로 식별. 사용자 입력은 보통
 * "trtype:RDMA traddr:192.168.1.10 trsvcid:4420 subnqn:nqn..." 형태의 문자열을
 * spdk_nvme_transport_id_parse() 로 이 구조체에 변환.
 *
 * 사용 위치: probe/connect API의 trid 인자, ANA log 항목, bdev_nvme RPC 입력, multipath 정책.
 */
struct spdk_nvme_transport_id {
	/**
	 * NVMe transport string.
	 */
	char trstring[SPDK_NVMF_TRSTRING_MAX_LEN + 1];
	/* [한국어] 트랜스포트 이름 문자열("PCIE"/"RDMA"/"TCP"/"FC"/"VFIOUSER"/"CUSTOM").
	 * 설정자: spdk_nvme_transport_id_populate_trstring() 또는 사용자 직접.
	 * 읽는 자: lib/nvme 가 transport vtable lookup 시 키로 사용. trtype과 항상 일치해야 함. */

	/**
	 * NVMe transport type.
	 */
	enum spdk_nvme_transport_type trtype;
	/* [한국어] 트랜스포트 타입 enum. trstring과 의미적으로 중복이지만 비교 성능과 NVMe-oF spec
	 * TRTYPE 와이어 호환성을 위해 둘 다 보관. SPDK 내부 함수는 주로 trtype을 본다. */

	/**
	 * Address family of the transport address.
	 *
	 * For PCIe, this value is ignored.
	 */
	enum spdk_nvmf_adrfam adrfam;
	/* [한국어] 주소 패밀리(IPv4/IPv6/IB/FC/Intra-host). NVMe-oF spec 의 ADRFAM 필드.
	 * PCIe는 의미 없음. */

	/**
	 * Transport address of the NVMe-oF endpoint. For transports which use IP
	 * addressing (e.g. RDMA), this should be an IP address. For PCIe, this
	 * can either be a zero length string (the whole bus) or a PCI address
	 * in the format DDDD:BB:DD.FF or DDDD.BB.DD.FF. For FC the string is
	 * formatted as: nn-0xWWNN:pn-0xWWPN” where WWNN is the Node_Name of the
	 * target NVMe_Port and WWPN is the N_Port_Name of the target NVMe_Port.
	 */
	char traddr[SPDK_NVMF_TRADDR_MAX_LEN + 1];
	/* [한국어] 트랜스포트 주소 문자열. PCIe="0000:04:00.0" BDF, RDMA/TCP="192.168.1.10",
	 * FC="nn-0xWWNN:pn-0xWWPN". probe 시 빈 문자열이면 PCIe는 전체 버스 enumerate. */

	/**
	 * Transport service id of the NVMe-oF endpoint.  For transports which use
	 * IP addressing (e.g. RDMA), this field should be the port number. For PCIe,
	 * and FC this is always a zero length string.
	 */
	char trsvcid[SPDK_NVMF_TRSVCID_MAX_LEN + 1];
	/* [한국어] TCP/RDMA 포트번호 문자열("4420"). NVMe-oF 표준 well-known port 4420.
	 * PCIe/FC에서는 빈 문자열. */

	/**
	 * Subsystem NQN of the NVMe over Fabrics endpoint. May be a zero length string.
	 */
	char subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	/* [한국어] target subsystem NQN. fabrics에서 한 IP:port 뒤에 다수 subsystem이 있을 때
	 * 어느 subsystem에 connect할지 지정. discovery service면 "nqn.2014-08.org.nvmexpress.discovery". */

	/**
	 * The Transport connection priority of the NVMe-oF endpoint. Currently this is
	 * only supported by posix based sock implementation on Kernel TCP stack. More
	 * information of this field can be found from the socket(7) man page.
	 */
	int priority;
	/* [한국어] TCP socket priority(SO_PRIORITY). 현재 posix sock backend에서만 적용.
	 * QoS 멀티큐 환경에서 다른 트래픽과 우선순위 차별화. */
};

/**
 * NVMe host identifier
 *
 * Used for defining the host identity for an NVMe-oF connection.
 *
 * In terms of configuration, this object can be considered a subtype of TransportID
 * Please see etc/spdk/nvmf.conf.in for more details.
 *
 * A string representation of this type may be converted to this type using
 * spdk_nvme_host_id_parse().
 */
/*
 * [한국어]
 * struct spdk_nvme_host_id - 호스트 측 source address (NVMe-oF 클라이언트).
 *
 * 멀티홈 호스트에서 특정 NIC을 강제할 때 사용. transport_id가 target endpoint를 가리킨다면
 * host_id는 클라이언트가 어느 source IP/port로 connect 할지 지정. 보통 비워두며 OS의
 * 라우팅 테이블이 자동 결정.
 */
struct spdk_nvme_host_id {
	/**
	 * Transport address to be used by the host when connecting to the NVMe-oF endpoint.
	 * May be an IP address or a zero length string for transports which
	 * use IP addressing (e.g. RDMA).
	 * For PCIe and FC this is always a zero length string.
	 */
	char hostaddr[SPDK_NVMF_TRADDR_MAX_LEN + 1];
	/* [한국어] 호스트 source 주소(IP). 빈 문자열=OS 자동선택. RDMA/TCP에서만 의미. */

	/**
	 * Transport service ID used by the host when connecting to the NVMe.
	 * May be a port number or a zero length string for transports which
	 * use IP addressing (e.g. RDMA).
	 * For PCIe and FC this is always a zero length string.
	 */
	char hostsvcid[SPDK_NVMF_TRSVCID_MAX_LEN + 1];
	/* [한국어] 호스트 source 포트. 빈 문자열=ephemeral port. */
};

/*
 * [한국어]
 * struct spdk_nvme_rdma_device_stat - RDMA 트랜스포트 디바이스(RDMA NIC)당 통계.
 *
 * spdk_nvme_poll_group_get_stats() 가 채워서 반환. 한 poll group이 다수 RDMA NIC을 거치면
 * 각 NIC마다 한 인스턴스 생성. ibv send/recv WR 수 추적으로 NIC별 부하 파악.
 */
struct spdk_nvme_rdma_device_stat {
	const char *name;
	/* [한국어] RDMA 디바이스 이름(ibv_device->name, 예: "mlx5_0"). 정적 문자열. */
	uint64_t polls;
	/* [한국어] 이 NIC poll 호출 누적 횟수. */
	uint64_t idle_polls;
	/* [한국어] CQ가 비어있어 아무 처리 못 한 poll 횟수. busy_ratio 계산 분모. */
	uint64_t completions;
	/* [한국어] 처리한 ibv_wc 누적. */
	uint64_t queued_requests;
	/* [한국어] WR 자원 부족 등으로 internal queue에 대기 중인 요청 수. */
	uint64_t total_send_wrs;
	/* [한국어] post_send 누적 WR 수. NVMe SQE 발행 + RDMA write 모두 포함. */
	uint64_t send_doorbell_updates;
	/* [한국어] send queue doorbell ring(ibv_post_send) 호출 수. WR 묶음 처리 효율 지표. */
	uint64_t total_recv_wrs;
	/* [한국어] post_recv 누적 WR 수. NVMe CQE 수신용 + RDMA read response. */
	uint64_t recv_doorbell_updates;
	/* [한국어] receive queue doorbell ring 호출 수. */
};

/*
 * [한국어]
 * struct spdk_nvme_pcie_stat - PCIe 트랜스포트 통계.
 *
 * MMIO doorbell 갱신 횟수와 shadow doorbell(NVMe 1.3 spec에 추가된 호스트 메모리 사본) 갱신
 * 횟수를 분리 추적해 doorbell 효율성 분석에 사용. shadow doorbell은 PCIe write를 줄여 latency 감소.
 */
struct spdk_nvme_pcie_stat {
	uint64_t polls;
	/* [한국어] qpair_process_completions 누적 호출. */
	uint64_t idle_polls;
	/* [한국어] CQ phase mismatch(완료 entry 없음)로 빈 손 폴링 횟수. */
	uint64_t completions;
	/* [한국어] 처리한 CQE 수. 디바이스의 throughput 지표. */
	uint64_t cq_mmio_doorbell_updates;
	/* [한국어] CQ head doorbell MMIO write 횟수 (shadow 비활성 시). */
	uint64_t cq_shadow_doorbell_updates;
	/* [한국어] CQ head shadow doorbell update (host RAM write only, MMIO 없음). */
	uint64_t submitted_requests;
	/* [한국어] 사용자가 ns_cmd로 발행한 요청 수. */
	uint64_t queued_requests;
	/* [한국어] split/대기 등으로 internal queue에 임시 보관된 요청 수. */
	uint64_t sq_mmio_doorbell_updates;
	/* [한국어] SQ tail doorbell MMIO write 횟수. delay_cmd_submit 옵션 켜면 batching으로 감소. */
	uint64_t sq_shadow_doorbell_updates;
	/* [한국어] SQ tail shadow doorbell update. CMB SQE+shadow doorbell 시 PCIe round-trip 회피. */
};

/*
 * [한국어]
 * struct spdk_nvme_tcp_stat - NVMe/TCP 트랜스포트 통계.
 *
 * NVMe/TCP는 PDU 단위로 동작하므로 socket completion(send/recv 진척)과 nvme completion
 * (CQE 처리)을 분리해 본다. queued_requests 가 크면 TCP send buffer 포화 의심.
 */
struct spdk_nvme_tcp_stat {
	uint64_t polls;
	/* [한국어] poll 호출 횟수. */
	uint64_t idle_polls;
	/* [한국어] 처리할 PDU 없이 빈손으로 돌아온 poll. */
	uint64_t socket_completions;
	/* [한국어] socket level 송수신 완료 이벤트(EPOLLIN/OUT) 수. */
	uint64_t nvme_completions;
	/* [한국어] NVMe CQE 디스패치 횟수. */
	uint64_t submitted_requests;
	/* [한국어] 사용자 ns_cmd 발행 누적. */
	uint64_t queued_requests;
	/* [한국어] socket buffer poll로 대기 중인 요청 수. */
};

/*
 * [한국어]
 * struct spdk_nvme_transport_poll_group_stat - 한 트랜스포트의 poll group 통계 컨테이너.
 *
 * trtype에 따라 union으로 RDMA/PCIe/TCP 중 하나만 활성. 한 poll group은 여러 트랜스포트를
 * 동시에 폴링할 수 있으므로 이 구조체가 트랜스포트당 1개씩 모여 spdk_nvme_poll_group_stat에 들어감.
 */
struct spdk_nvme_transport_poll_group_stat {
	spdk_nvme_transport_type_t trtype;
	/* [한국어] 어느 트랜스포트의 통계인지 식별. union 분기 결정 키. */
	union {
		struct {
			uint32_t num_devices;
			/* [한국어] 이 poll group이 사용 중인 RDMA 디바이스 수. */
			struct spdk_nvme_rdma_device_stat *device_stats;
			/* [한국어] num_devices 개의 device 통계 배열. SPDK가 할당, 사용자가 free. */
		} rdma;
		struct spdk_nvme_pcie_stat pcie;
		/* [한국어] PCIe poll group 통계. PCIe는 디바이스 단위 통계가 ctrlr에 종속. */
		struct spdk_nvme_tcp_stat tcp;
		/* [한국어] TCP poll group 통계. */
	};
};

/*
 * [한국어]
 * struct spdk_nvme_poll_group_stat - poll group 전체 통계 묶음.
 *
 * spdk_nvme_poll_group_get_stats() 가 할당해서 반환. spdk_nvme_poll_group_free_stats() 로 해제.
 * 여러 트랜스포트가 한 poll group에 섞여 있으면 transport_stat 배열에 트랜스포트별로 분리.
 */
struct spdk_nvme_poll_group_stat {
	uint32_t num_transports;
	/* [한국어] 활성화된 트랜스포트 수. transport_stat 배열 길이. */
	struct spdk_nvme_transport_poll_group_stat **transport_stat;
	/* [한국어] 트랜스포트별 통계 포인터 배열. SPDK 소유 메모리 - free_stats로 해제. */
};

/*
 * Controller support flags
 *
 * Used for identifying if the controller supports these flags.
 */
/*
 * [한국어]
 * enum spdk_nvme_ctrlr_flags - 컨트롤러 능력 비트마스크.
 *
 * spdk_nvme_ctrlr_get_flags() 반환값에서 비트 검사. SPDK가 attach 후 identify ctrlr +
 * get_log_page 결과를 종합해 결정. 사용자(bdev_nvme 등)가 어떤 NVM 명령/SGL/Compare 등
 * 기능을 쓸지 fast path 분기에 사용.
 */
enum spdk_nvme_ctrlr_flags {
	SPDK_NVME_CTRLR_SGL_SUPPORTED			= 1 << 0, /**< SGL is supported */
	/* [한국어] 컨트롤러가 SGL(Scatter-Gather List)을 지원. PRP 대신 SGL로 큰 I/O 분산 가능. */
	SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED	= 1 << 1, /**< security send/receive is supported */
	/* [한국어] Security Send(0x81)/Receive(0x82) admin 명령 지원. TCG Opal 등 운반에 필요. */
	SPDK_NVME_CTRLR_WRR_SUPPORTED			= 1 << 2, /**< Weighted Round Robin is supported */
	/* [한국어] CC.AMS WRR(Weighted Round Robin) arbitration 지원. qpair 우선순위 활용 가능. */
	SPDK_NVME_CTRLR_COMPARE_AND_WRITE_SUPPORTED	= 1 << 3, /**< Compare and write fused operations supported */
	/* [한국어] Compare(0x05) + Write(0x01) Fused operation 지원. 원자적 CAS-on-NVMe 가능. */
	SPDK_NVME_CTRLR_SGL_REQUIRES_DWORD_ALIGNMENT	= 1 << 4, /**< Dword alignment is required for SGL */
	/* [한국어] SGL element들이 4-byte align 되어야 함. 일부 컨트롤러 quirk. */
	SPDK_NVME_CTRLR_ZONE_APPEND_SUPPORTED		= 1 << 5, /**< Zone Append is supported (within Zoned Namespaces) */
	/* [한국어] ZNS Zone Append(opcode 0x7D) 지원. write pointer 추적 없이 multi-producer write 가능. */
	SPDK_NVME_CTRLR_DIRECTIVES_SUPPORTED		= 1 << 6, /**< The Directives is supported */
	/* [한국어] Directive Send/Receive 명령 지원 (Stream/Identify Directive). */
	SPDK_NVME_CTRLR_MPTR_SGL_SUPPORTED		= 1 << 7, /**< MPTR containing SGL descriptor is supported */
	/* [한국어] Metadata pointer(MPTR) 자리에 SGL descriptor 사용 가능. metadata도 분산 저장. */
	SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED	= 1 << 8, /**< Support for sending I/O requests with accel sequence */
	/* [한국어] poll group의 accel_fn_table 와 결합하여 ns_cmd_ext_io_opts.accel_sequence 활용 가능. */
};

/**
 * Structure with optional IO request parameters
 */
/*
 * [한국어]
 * struct spdk_nvme_ns_cmd_ext_io_opts - ns_cmd_*_ext 변형의 확장 옵션 (56B, ABI guarded).
 *
 * 기존 ns_cmd_read/write가 인자가 너무 많아진 문제를 해결하기 위해 도입된 확장 옵션 묶음.
 * memory_domain(GPU/RDMA mem), accel_sequence(offload), metadata(T10 DIF), apptag(end-to-end
 * protection) 등을 한 구조체로 모아 ABI 안전하게 확장. SPDK_SIZEOF(opts, last_member)로 size
 * 채우는 패턴 사용.
 */
struct spdk_nvme_ns_cmd_ext_io_opts {
	/** size of this structure in bytes, use SPDK_SIZEOF(opts, last_member) to obtain it */
	size_t size;
	/* [한국어] ABI guard - 호출자가 컴파일된 구조체 크기. 라이브러리가 어디까지 유효한지 결정. */
	/** Memory domain which describes data payload in IO request. The controller must support
	 * the corresponding memory domain type, refer to \ref spdk_nvme_ctrlr_get_memory_domains */
	struct spdk_memory_domain *memory_domain;
	/* [한국어] 페이로드의 메모리 도메인. NULL=일반 호스트 RAM, 비-NULL=GPU/RDMA registered/CMB 등.
	 * 컨트롤러가 이 도메인 타입 지원해야 함(get_memory_domains 검사). zero-copy I/O 핵심. */
	/** User context to be passed to memory domain operations */
	void *memory_domain_ctx;
	/* [한국어] memory_domain 콜백에 전달되는 컨텍스트 포인터. translate/pull/push 시 사용. */
	/** Flags for this IO, defined in nvme_spec.h */
	uint32_t io_flags;
	/* [한국어] NVMe spec 의 명령 dword 12 상위 비트 - LR(Limited Retry), FUA(Force Unit Access),
	 * PRCHK(Protection Information check) 등. SPDK_NVME_IO_FLAGS_* 상수 OR. */
	/* Hole at bytes 28-31. */
	uint8_t reserved28[4];
	/* [한국어] 패딩. 다음 metadata(void*=8B) align. */
	/** Virtual address pointer to the metadata payload, the length of metadata is specified by \ref spdk_nvme_ns_get_md_size */
	void *metadata;
	/* [한국어] T10 DIF/DIX 메타데이터 버퍼 가상 주소. extended sector format(LBA+MD interleaved)이
	 * 아닌 별도 buffer 모드에서만 사용. 길이는 ns_get_md_size() * lba_count. */
	/** Application tag mask to use end-to-end protection information. */
	uint16_t apptag_mask;
	/* [한국어] 비트마스크 - 어느 비트의 apptag를 검사할지. PI Type 1/2/3 모두 사용. */
	/** Application tag to use end-to-end protection information. */
	uint16_t apptag;
	/* [한국어] T10 DIF Application Tag 기대값. 컨트롤러가 PI 검사 시 이 값과 비교. */
	/** Command dword 13 specific field. */
	uint32_t cdw13;
	/* [한국어] NVMe SQE의 dword 13. command-specific field (예: Copy 명령의 Source Range Count). */
	/** Accel sequence (only valid if SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED is set and the
	 *  qpair is part of a poll group).
	 */
	void *accel_sequence;
	/* [한국어] 미리 빌드한 accel sequence(crc32c+copy 등) 포인터. ns_cmd 발행 시 SPDK가
	 * finish_sequence를 호출해 sequence 처리 후 NVMe 명령 발행 → CPU 절감. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ns_cmd_ext_io_opts) == 56, "Incorrect size");
/* [한국어] ABI guard - 56B 고정. 새 옵션은 size 필드를 보고 라이브러리가 미설정 영역을 0 처리. */

/**
 * Parse the string representation of a transport ID.
 *
 * \param trid Output transport ID structure (must be allocated and initialized by caller).
 * \param str Input string representation of a transport ID to parse.
 *
 * str must be a zero-terminated C string containing one or more key:value pairs
 * separated by whitespace.
 *
 * Key          | Value
 * ------------ | -----
 * trtype       | Transport type (e.g. PCIe, RDMA)
 * adrfam       | Address family (e.g. IPv4, IPv6)
 * traddr       | Transport address (e.g. 0000:04:00.0 for PCIe, 192.168.100.8 for RDMA, or WWN for FC)
 * trsvcid      | Transport service identifier (e.g. 4420)
 * subnqn       | Subsystem NQN
 *
 * Unspecified fields of trid are left unmodified, so the caller must initialize
 * trid (for example, memset() to 0) before calling this function.
 *
 * \return 0 if parsing was successful and trid is filled out, or negated errno
 * values on failure.
 */
/*
 * [한국어]
 * spdk_nvme_transport_id_parse - "trtype:RDMA traddr:..." 문자열 → spdk_nvme_transport_id 변환.
 *
 * @trid: [out] 호출자가 미리 memset(0)한 구조체. 함수가 파싱 결과로 채움.
 * @str: 입력 문자열 (whitespace로 구분된 key:value 쌍). NULL 종료 필요.
 * @return: 0=성공, 음수 errno=실패(잘못된 형식/알 수 없는 키).
 *
 * 사용 시점: bdev_nvme RPC 핸들러, NVMe-oF discovery, 사용자 앱(perf, identify)이 CLI 인자에서 trid 추출.
 * 호출자: bdev_nvme_create_ctrlr, examples/nvme/discovery_aer.c 등.
 * 동작: 키워드 파싱 → trstring/trtype/adrfam/traddr/trsvcid/subnqn/priority 채움. 미지정 필드는 그대로.
 */
int spdk_nvme_transport_id_parse(struct spdk_nvme_transport_id *trid, const char *str);


/**
 * Fill in the trtype and trstring fields of this trid based on a known transport type.
 *
 * \param trid The trid to fill out.
 * \param trtype The transport type to use for filling the trid fields. Only valid for
 * transport types referenced in the NVMe-oF spec.
 */
/*
 * [한국어]
 * spdk_nvme_trid_populate_transport - trtype을 받아 trid의 trtype/trstring 두 필드 동시 설정.
 *
 * @trid: [out] 다른 필드는 건드리지 않음.
 * @trtype: 설정할 트랜스포트 타입.
 *
 * trstring과 trtype을 일관성 있게 유지하기 위한 헬퍼. NVMe-oF spec 트랜스포트만 유효 - PCIe 등은
 * 사용자가 직접 설정. 코드 중복 회피.
 */
void spdk_nvme_trid_populate_transport(struct spdk_nvme_transport_id *trid,
				       enum spdk_nvme_transport_type trtype);

/**
 * Parse the string representation of a host ID.
 *
 * \param hostid Output host ID structure (must be allocated and initialized by caller).
 * \param str Input string representation of a transport ID to parse (hostid is a sub-configuration).
 *
 * str must be a zero-terminated C string containing one or more key:value pairs
 * separated by whitespace.
 *
 * Key            | Value
 * -------------- | -----
 * hostaddr       | Transport address (e.g. 192.168.100.8 for RDMA)
 * hostsvcid      | Transport service identifier (e.g. 4420)
 *
 * Unspecified fields of trid are left unmodified, so the caller must initialize
 * hostid (for example, memset() to 0) before calling this function.
 *
 * This function should not be used with Fiber Channel or PCIe as these transports
 * do not require host information for connections.
 *
 * \return 0 if parsing was successful and hostid is filled out, or negated errno
 * values on failure.
 */
/*
 * [한국어]
 * spdk_nvme_host_id_parse - "hostaddr:... hostsvcid:..." 문자열 → spdk_nvme_host_id 변환.
 *
 * transport_id_parse의 호스트 측 변형. 사용 빈도 적음 - 보통 host_id는 비워둠.
 * 동작 방식과 에러 처리는 transport_id_parse와 동일.
 */
int spdk_nvme_host_id_parse(struct spdk_nvme_host_id *hostid, const char *str);

/**
 * Parse the string representation of a transport ID transport type into the trid struct.
 *
 * \param trid The trid to write to
 * \param trstring Input string representation of transport type (e.g. "PCIe", "RDMA").
 *
 * \return 0 if parsing was successful and trtype is filled out, or negated errno
 * values if the provided string was an invalid transport string.
 */
/*
 * [한국어]
 * spdk_nvme_transport_id_populate_trstring - trstring 인자만 받아 trid의 trstring 필드 채움.
 *
 * trtype 별도 미설정. 사용자가 trstring만 알고 있을 때(예: RPC 입력) trid 일부 채우고
 * 나머지(traddr 등)는 별도 입력. 대소문자 정규화 포함.
 */
int spdk_nvme_transport_id_populate_trstring(struct spdk_nvme_transport_id *trid,
		const char *trstring);

/**
 * Parse the string representation of a transport ID transport type.
 *
 * \param trtype Output transport type (allocated by caller).
 * \param str Input string representation of transport type (e.g. "PCIe", "RDMA").
 *
 * \return 0 if parsing was successful and trtype is filled out, or negated errno
 * values on failure.
 */
/*
 * [한국어]
 * spdk_nvme_transport_id_parse_trtype - "PCIe"/"RDMA"/"TCP" 등 문자열 → enum trtype 단독 변환.
 *
 * trid 통째 변환이 아니라 trtype 한 필드만 추출. transport register lookup 등에 사용.
 */
int spdk_nvme_transport_id_parse_trtype(enum spdk_nvme_transport_type *trtype, const char *str);

/**
 * Look up the string representation of a transport ID transport type.
 *
 * \param trtype Transport type to convert.
 *
 * \return static string constant describing trtype, or NULL if trtype not found.
 */
/*
 * [한국어]
 * spdk_nvme_transport_id_trtype_str - enum trtype → 정적 문자열 (역변환).
 *
 * 로그/RPC 응답 출력용. 정적 문자열이므로 free 금지. 알 수 없는 trtype이면 NULL.
 */
const char *spdk_nvme_transport_id_trtype_str(enum spdk_nvme_transport_type trtype);

/**
 * Look up the string representation of a transport ID address family.
 *
 * \param adrfam Address family to convert.
 *
 * \return static string constant describing adrfam, or NULL if adrfam not found.
 */
/*
 * [한국어]
 * spdk_nvme_transport_id_adrfam_str - enum adrfam → 정적 문자열.
 *
 * 예: SPDK_NVMF_ADRFAM_IPV4 → "IPv4". RPC 출력용.
 */
const char *spdk_nvme_transport_id_adrfam_str(enum spdk_nvmf_adrfam adrfam);

/**
 * Parse the string representation of a transport ID address family.
 *
 * \param adrfam Output address family (allocated by caller).
 * \param str Input string representation of address family (e.g. "IPv4", "IPv6").
 *
 * \return 0 if parsing was successful and adrfam is filled out, or negated errno
 * values on failure.
 */
/*
 * [한국어]
 * spdk_nvme_transport_id_parse_adrfam - "IPv4"/"IPv6"/"IB"/"FC" 문자열 → enum adrfam.
 *
 * trid 안의 adrfam 필드 단독 추출용. discovery service 응답 파싱에 사용.
 */
int spdk_nvme_transport_id_parse_adrfam(enum spdk_nvmf_adrfam *adrfam, const char *str);

/**
 * Compare two transport IDs.
 *
 * The result of this function may be used to sort transport IDs in a consistent
 * order; however, the comparison result is not guaranteed to be consistent across
 * library versions.
 *
 * This function uses a case-insensitive comparison for string fields, but it does
 * not otherwise normalize the transport ID. It is the caller's responsibility to
 * provide the transport IDs in a consistent format.
 *
 * \param trid1 First transport ID to compare.
 * \param trid2 Second transport ID to compare.
 *
 * \return 0 if trid1 == trid2, less than 0 if trid1 < trid2, greater than 0 if
 * trid1 > trid2.
 */
/*
 * [한국어]
 * spdk_nvme_transport_id_compare - 두 trid를 정렬 가능한 형태로 비교.
 *
 * @trid1, trid2: 비교할 trid 두 개.
 * @return: <0 if trid1 < trid2, 0 if 같음, >0 if trid1 > trid2.
 *
 * 사용처: ctrlr lookup(같은 trid면 같은 ctrlr), TAILQ 정렬, 멀티패스 fail-over.
 * 문자열 필드는 case-insensitive 비교. SPDK 버전 간 결과 일관성은 보장 안 됨.
 * 호출자: bdev_nvme 가 같은 trid 중복 attach 방지에 사용.
 */
int spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
				   const struct spdk_nvme_transport_id *trid2);

/**
 * Parse the string representation of PI check settings (prchk:guard|reftag)
 *
 * \param prchk_flags Output PI check flags.
 * \param str Input string representation of PI check settings.
 *
 * \return 0 if parsing was successful and prchk_flags is set, or negated errno
 * values on failure.
 */
/*
 * [한국어]
 * spdk_nvme_prchk_flags_parse - "guard|reftag" 형태 문자열 → PI check 비트마스크.
 *
 * @prchk_flags: [out] SPDK_NVME_IO_FLAGS_PRCHK_GUARD/REFTAG/APPTAG OR 결과.
 * NVMe T10 DIF end-to-end protection 검사 항목 선택. RPC 입력 파싱용.
 */
int spdk_nvme_prchk_flags_parse(uint32_t *prchk_flags, const char *str);

/**
 * Look up the string representation of PI check settings  (prchk:guard|reftag)
 *
 * \param prchk_flags PI check flags to convert.
 *
 * \return static string constant describing PI check settings. If prchk_flags is 0,
 * NULL is returned.
 */
/*
 * [한국어]
 * spdk_nvme_prchk_flags_str - PI check 비트마스크 → "guard|reftag" 문자열.
 *
 * 0이면 NULL. 정적 문자열이므로 free 금지. RPC 출력에서 사용.
 */
const char *spdk_nvme_prchk_flags_str(uint32_t prchk_flags);

/**
 * Determine whether the NVMe library can handle a specific NVMe over Fabrics
 * transport type.
 *
 * \param trtype NVMe over Fabrics transport type to check.
 *
 * \return true if trtype is supported or false if it is not supported or if
 * SPDK_NVME_TRANSPORT_CUSTOM is supplied as trtype since it can represent multiple
 * transports.
 */
/*
 * [한국어]
 * spdk_nvme_transport_available - 지정 trtype을 SPDK 라이브러리가 핸들 가능한지.
 *
 * 빌드 시 비활성화된 트랜스포트(예: --without-rdma)면 false. CUSTOM은 모호하므로 false.
 * 사용자가 fabrics initiator 시작 전 미리 능력 체크용.
 */
bool spdk_nvme_transport_available(enum spdk_nvme_transport_type trtype);

/**
 * Determine whether the NVMe library can handle a specific NVMe over Fabrics
 * transport type.
 *
 * \param transport_name Name of the NVMe over Fabrics transport type to check.
 *
 * \return true if transport_name is supported or false if it is not supported.
 */
/*
 * [한국어]
 * spdk_nvme_transport_available_by_name - 트랜스포트 이름 문자열 버전 _available.
 *
 * 사용자 정의 CUSTOM 트랜스포트도 등록되어 있으면 true. RPC에서 직접 사용.
 */
bool spdk_nvme_transport_available_by_name(const char *transport_name);

/**
 * Callback for spdk_nvme_probe() enumeration.
 *
 * \param cb_ctx Opaque value passed to spdk_nvme_probe().
 * \param trid NVMe transport identifier.
 * \param opts NVMe controller initialization options. This structure will be
 * populated with the default values on entry, and the user callback may update
 * any options to request a different value. The controller may not support all
 * requested parameters, so the final values will be provided during the attach
 * callback.
 *
 * \return true to attach to this device.
 */
/*
 * [한국어]
 * spdk_nvme_probe_cb - probe 단계에서 발견한 컨트롤러마다 호출되는 사용자 콜백.
 *
 * @cb_ctx: spdk_nvme_probe() 호출 시 사용자가 넘긴 컨텍스트.
 * @trid: 발견된 컨트롤러의 transport ID. 이 값으로 attach 여부 결정 (예: 특정 BDF만 선택).
 * @opts: [in/out] 라이브러리가 기본값으로 채워서 전달. 콜백이 수정한 값으로 attach 시도.
 * @return: true=이 디바이스에 attach 진행, false=skip.
 *
 * 호출 컨텍스트: spdk_nvme_probe()/probe_async() 가 트랜스포트 enumerate 중 호출.
 * 보통 single thread에서 직렬 호출. 콜백 안에서 opts를 자유롭게 수정 가능 - opts_size 보존 필수.
 * 호출 체인: 사용자 → spdk_nvme_probe() → [transport enumerate] → probe_cb() → (true면) attach 진행.
 */
typedef bool (*spdk_nvme_probe_cb)(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
				   struct spdk_nvme_ctrlr_opts *opts);

/**
 * Callback for spdk_nvme_attach() to report a device that has been attached to
 * the userspace NVMe driver.
 *
 * \param cb_ctx Opaque value passed to spdk_nvme_attach_cb().
 * \param trid NVMe transport identifier.
 * \param ctrlr Opaque handle to NVMe controller.
 * \param opts NVMe controller initialization options that were actually used.
 * Options may differ from the requested options from the attach call depending
 * on what the controller supports.
 */
/*
 * [한국어]
 * spdk_nvme_attach_cb - attach 성공 시 컨트롤러 핸들과 최종 적용 opts를 사용자에게 전달.
 *
 * @ctrlr: ★ 새로 attach된 컨트롤러 핸들 - 이 시점부터 사용자가 소유. detach 호출 책임 있음.
 * @opts: 협상 후 실제 적용된 옵션 (probe_cb 요청과 다를 수 있음 - num_io_queues 등).
 *
 * 호출 컨텍스트: probe 진행 중 단일 스레드. 이 콜백에서 ctrlr를 사용자 자료구조에 등록 후
 * spdk_nvme_ctrlr_alloc_io_qpair() 로 I/O 시작 가능. 콜백 내부는 가벼운 등록만 권장 (긴 작업은
 * spdk_thread_send_msg로 외부 위임).
 */
typedef void (*spdk_nvme_attach_cb)(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
				    struct spdk_nvme_ctrlr *ctrlr,
				    const struct spdk_nvme_ctrlr_opts *opts);

/**
 * Callback for spdk_nvme_probe*_ext() to report a device that has been probed but
 * unable to attach to the userspace NVMe driver.
 *
 * \param cb_ctx Opaque value passed to spdk_nvme_probe*_ext().
 * \param trid NVMe transport identifier.
 * \param rc Negative error code that provides information about the failure.
 */
/*
 * [한국어]
 * spdk_nvme_attach_fail_cb - probe_cb=true 였으나 attach 실패 시 호출 (probe_ext 변형 전용).
 *
 * @rc: 실패 원인 음수 errno (-ENOMEM/-EIO/-EINVAL 등).
 * 기존 probe API에서는 attach 실패가 silent였으나 _ext 추가로 명시적 통지 가능.
 * 사용자(bdev_nvme)가 hot_plug 실패 알림에 사용.
 */
typedef void (*spdk_nvme_attach_fail_cb)(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		int rc);

/**
 * Callback for spdk_nvme_remove() to report that a device attached to the userspace
 * NVMe driver has been removed from the system.
 *
 * The controller will remain in a failed state (any new I/O submitted will fail).
 *
 * The controller must be detached from the userspace driver by calling spdk_nvme_detach()
 * once the controller is no longer in use. It is up to the library user to ensure
 * that no other threads are using the controller before calling spdk_nvme_detach().
 *
 * \param cb_ctx Opaque value passed to spdk_nvme_remove_cb().
 * \param ctrlr NVMe controller instance that was removed.
 */
/*
 * [한국어]
 * spdk_nvme_remove_cb - 핫 리무브(또는 NVMe-oF 단절) 시 사용자에게 통지.
 *
 * 핸들은 invalid 직전 - 모든 신규 I/O는 -ENXIO 반환. 사용자는 진행 중 I/O 안전 종료 후
 * spdk_nvme_detach()로 핸들 정리해야 함. SPDK 자체는 자동 detach하지 않음.
 * 호출 컨텍스트: probe poll loop 또는 hot-plug 감시 스레드.
 */
typedef void (*spdk_nvme_remove_cb)(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr);

/*
 * [한국어]
 * spdk_nvme_pcie_hotplug_filter_cb - PCIe 핫 인서트 필터 콜백 typedef.
 *
 * @addr: 새로 발견된 PCIe BDF.
 * @return: true=이 디바이스 probe 진행, false=무시.
 *
 * spdk_nvme_pcie_set_hotplug_filter() 로 등록. SPDK env layer가 udev 같은 채널로 새 PCIe device를
 * 감지했을 때 NVMe 드라이버가 attach 시도 전 사용자 필터를 거친다. NVMe SSD 외 다른 PCIe device
 * 무시하거나 특정 BDF만 허용할 때 사용.
 */
typedef bool (*spdk_nvme_pcie_hotplug_filter_cb)(const struct spdk_pci_addr *addr);

/**
 * Register the associated function to allow filtering of hot-inserted PCIe SSDs.
 *
 * If an application is using spdk_nvme_probe() to detect hot-inserted SSDs,
 * this function may be used to register a function to filter those SSDs.
 * If the filter function returns true, the nvme library will notify the SPDK
 * env layer to allow probing of the device.
 *
 * Registering a filter function is optional.  If none is registered, the nvme
 * library will allow probing of all hot-inserted SSDs.
 *
 * \param filter_cb Filter function callback routine
 */
/*
 * [한국어]
 * spdk_nvme_pcie_set_hotplug_filter - 핫플러그 필터 콜백 등록 (전역, PCIe 전용).
 *
 * @filter_cb: 필터 함수. NULL 허용 (모두 통과).
 *
 * 미등록 시 모든 핫 인서트 SSD가 probe 대상. 호출 빈도는 device 발견 시 1회/디바이스.
 * thread-safe (단일 글로벌 변수, atomic 갱신). 호출자: app init 단계에서 1회.
 */
void
spdk_nvme_pcie_set_hotplug_filter(spdk_nvme_pcie_hotplug_filter_cb filter_cb);

/**
 * Enumerate the bus indicated by the transport ID and attach the userspace NVMe
 * driver to each device found if desired.
 *
 * This function is not thread safe and should only be called from one thread at
 * a time while no other threads are actively using any NVMe devices.
 *
 * If called from a secondary process, only devices that have been attached to
 * the userspace driver in the primary process will be probed.
 *
 * If called more than once, only devices that are not already attached to the
 * SPDK NVMe driver will be reported.
 *
 * To stop using the the controller and release its associated resources,
 * call spdk_nvme_detach() with the spdk_nvme_ctrlr instance from the attach_cb()
 * function.
 *
 * \param trid The transport ID indicating which bus to enumerate. If the trtype
 * is PCIe or trid is NULL, this will scan the local PCIe bus. If the trtype is
 * fabrics (e.g. RDMA, TCP), the traddr and trsvcid must point at the location of an
 * NVMe-oF discovery service.
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of
 * the callbacks.
 * \param probe_cb will be called once per NVMe device found in the system.
 * \param attach_cb will be called for devices for which probe_cb returned true
 * once that NVMe controller has been attached to the userspace driver.
 * \param remove_cb will be called for devices that were attached in a previous
 * spdk_nvme_probe() call but are no longer attached to the system. Optional;
 * specify NULL if removal notices are not desired.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_nvme_probe - 트랜스포트 enumerate + 콜백 기반 attach 구동 (동기, 블로킹).
 *
 * @trid: PCIe면 NULL/empty=전체 버스, BDF 지정 시 해당 device. fabrics면 discovery service endpoint.
 * @cb_ctx: probe_cb/attach_cb/remove_cb 모두에 전달되는 사용자 컨텍스트.
 * @probe_cb: 발견 시 호출 - true 반환하면 attach 진행, false면 skip.
 * @attach_cb: attach 성공 시 ctrlr 핸들 전달.
 * @remove_cb: 이전 probe로 attach된 디바이스가 핫리무브 됐을 때 통지 (NULL 허용).
 * @return: 0=enumerate 완료(attach 성공/실패와 무관), -1=enumerate 자체 실패.
 *
 * 동기 함수 - 모든 attach가 끝날 때까지 블록. 비동기 진행은 spdk_nvme_probe_async() 사용.
 * thread-safe하지 않음 - 다른 스레드가 NVMe 디바이스 사용 중이면 호출 금지.
 * 호출 체인: 사용자 → spdk_nvme_probe() → 트랜스포트 vtable의 ctrlr_construct → identify → attach_cb.
 */
int spdk_nvme_probe(const struct spdk_nvme_transport_id *trid,
		    void *cb_ctx,
		    spdk_nvme_probe_cb probe_cb,
		    spdk_nvme_attach_cb attach_cb,
		    spdk_nvme_remove_cb remove_cb);

/**
 * Enumerate the bus indicated by the transport ID and attach the userspace NVMe
 * driver to each device found if desired.
 *
 * This works just the same as spdk_nvme_probe(), except that it calls attach_fail_cb
 * for devices that are probed but unabled to attach.
 *
 * \param trid The transport ID indicating which bus to enumerate. If the trtype
 * is PCIe or trid is NULL, this will scan the local PCIe bus. If the trtype is
 * fabrics (e.g. RDMA, TCP), the traddr and trsvcid must point at the location of an
 * NVMe-oF discovery service.
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of
 * the callbacks.
 * \param probe_cb will be called once per NVMe device found in the system.
 * \param attach_cb will be called for devices for which probe_cb returned true
 * once that NVMe controller has been attached to the userspace driver.
 * \param attach_fail_cb will be called for devices which probe_cb returned true
 * but failed to attach to the userspace driver.
 * \param remove_cb will be called for devices that were attached in a previous
 * spdk_nvme_probe() call but are no longer attached to the system. Optional;
 * specify NULL if removal notices are not desired.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_nvme_probe_ext - probe 확장 변형: attach 실패도 명시적으로 통지받음.
 *
 * 일반 probe와 동일하지만 attach_fail_cb 추가 - probe_cb=true 였으나 init 중 실패한 device의
 * trid와 errno를 사용자에게 통지. silent 실패가 문제되는 멀티패스/관리 RPC에서 유용.
 * 다른 인자/동작은 spdk_nvme_probe()와 같음.
 */
int spdk_nvme_probe_ext(const struct spdk_nvme_transport_id *trid,
			void *cb_ctx,
			spdk_nvme_probe_cb probe_cb,
			spdk_nvme_attach_cb attach_cb,
			spdk_nvme_attach_fail_cb attach_fail_cb,
			spdk_nvme_remove_cb remove_cb);

/**
 * Connect the NVMe driver to the device located at the given transport ID.
 *
 * This function is not thread safe and should only be called from one thread at
 * a time while no other threads are actively using this NVMe device.
 *
 * If called from a secondary process, only the device that has been attached to
 * the userspace driver in the primary process will be connected.
 *
 * If connecting to multiple controllers, it is suggested to use spdk_nvme_probe()
 * and filter the requested controllers with the probe callback. For PCIe controllers,
 * spdk_nvme_probe() will be more efficient since the controller resets will happen
 * in parallel.
 *
 * To stop using the the controller and release its associated resources, call
 * spdk_nvme_detach() with the spdk_nvme_ctrlr instance returned by this function.
 *
 * \param trid The transport ID indicating which device to connect. If the trtype
 * is PCIe, this will connect the local PCIe bus. If the trtype is fabrics
 * (e.g. RDMA, TCP), the traddr and trsvcid must point at the location of an NVMe-oF
 * service.
 * \param opts NVMe controller initialization options. Default values will be used
 * if the user does not specify the options. The controller may not support all
 * requested parameters.
 * \param opts_size Must be set to sizeof(struct spdk_nvme_ctrlr_opts), or 0 if
 * opts is NULL.
 *
 * \return pointer to the connected NVMe controller or NULL if there is any failure.
 *
 */
/*
 * [한국어]
 * spdk_nvme_connect - 단일 trid에 직접 연결 (probe/attach 콜백 없는 간소화 진입점).
 *
 * @trid: 연결할 컨트롤러의 transport ID. 명확한 1개 endpoint 필요.
 * @opts: 사용자 옵션 (NULL이면 default).
 * @opts_size: sizeof(struct spdk_nvme_ctrlr_opts), opts=NULL이면 0.
 * @return: 성공 시 ctrlr 핸들, 실패 시 NULL.
 *
 * 동기 동작 - 연결 완료까지 블록. 다중 ctrlr 연결 시는 probe + filter가 효율적 (PCIe reset 병렬화).
 * 호출 체인: 사용자 → spdk_nvme_connect() → 트랜스포트 connect → identify → 핸들 반환.
 */
struct spdk_nvme_ctrlr *spdk_nvme_connect(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		size_t opts_size);

struct spdk_nvme_probe_ctx;
/* [한국어] probe/connect 비동기 진행 상태를 담는 불투명 구조체 전방 선언.
 * spdk_nvme_probe_async() / connect_async() 가 반환, probe_poll_async() 가 polling.
 * 라이브러리 내부에서 attach 중인 ctrlr들의 state machine과 사용자 콜백 정보를 보관. */

/**
 * Connect the NVMe driver to the device located at the given transport ID.
 *
 * The function will return a probe context on success, controller associates with
 * the context is not ready for use, user must call spdk_nvme_probe_poll_async()
 * until spdk_nvme_probe_poll_async() returns 0.
 *
 * \param trid The transport ID indicating which device to connect. If the trtype
 * is PCIe, this will connect the local PCIe bus. If the trtype is fabrics
 * (e.g. RDMA, TCP), the traddr and trsvcid must point at the location of an NVMe-oF
 * service.
 * \param opts NVMe controller initialization options. Default values will be used
 * if the user does not specify the options. The controller may not support all
 * requested parameters.
 * \param attach_cb will be called once the NVMe controller has been attached
 * to the userspace driver.
 *
 * \return probe context on success, NULL on failure.
 *
 */
/*
 * [한국어]
 * spdk_nvme_connect_async - 비동기 connect 시작. 컨트롤러 핸들은 attach_cb로 나중에 도착.
 *
 * @trid: 대상 endpoint.
 * @opts: 사용자 옵션 (NULL=default, opts_size 검증).
 * @attach_cb: attach 완료 시 호출 - ctrlr 핸들 수령 위치.
 * @return: probe context (반복 polling용), 실패 시 NULL.
 *
 * 사용 패턴: connect_async → 반복 spdk_nvme_probe_poll_async() 호출 → 0 반환 시 완료.
 * polled-mode 앱에서 connect로 reactor 블로킹 방지에 사용. fabrics 환경 권장.
 */
struct spdk_nvme_probe_ctx *spdk_nvme_connect_async(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		spdk_nvme_attach_cb attach_cb);

/**
 * Probe and add controllers to the probe context list.
 *
 * Users must call spdk_nvme_probe_poll_async() to initialize
 * controllers in the probe context list to the READY state.
 *
 * \param trid The transport ID indicating which bus to enumerate. If the trtype
 * is PCIe or trid is NULL, this will scan the local PCIe bus. If the trtype is
 * fabrics (e.g. RDMA, TCP), the traddr and trsvcid must point at the location of an
 * NVMe-oF discovery service.
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of
 * the callbacks.
 * \param probe_cb will be called once per NVMe device found in the system.
 * \param attach_cb will be called for devices for which probe_cb returned true
 * once that NVMe controller has been attached to the userspace driver.
 * \param remove_cb will be called for devices that were attached in a previous
 * spdk_nvme_probe() call but are no longer attached to the system. Optional;
 * specify NULL if removal notices are not desired.
 *
 * \return probe context on success, NULL on failure.
 */
/*
 * [한국어]
 * spdk_nvme_probe_async - 비동기 probe 시작. enumerate는 즉시 끝나지만 attach는 polling 진행.
 *
 * spdk_nvme_probe()의 비동기 버전. enumerate에서 발견된 컨트롤러 목록을 probe_ctx에 누적,
 * 사용자가 spdk_nvme_probe_poll_async()를 호출할 때마다 init state machine 1 step 진행.
 * 한 SPDK reactor가 다른 일도 하면서 NVMe init을 병행하고 싶을 때 사용.
 */
struct spdk_nvme_probe_ctx *spdk_nvme_probe_async(const struct spdk_nvme_transport_id *trid,
		void *cb_ctx,
		spdk_nvme_probe_cb probe_cb,
		spdk_nvme_attach_cb attach_cb,
		spdk_nvme_remove_cb remove_cb);

/**
 * Probe and add controllers to the probe context list.
 *
 * Users must call spdk_nvme_probe_poll_async() to initialize
 * controllers in the probe context list to the READY state.
 *
 * This works just the same as spdk_nvme_probe_async(), except that it calls
 * attach_fail_cb for devices that are probed but unabled to attach.
 *
 * \param trid The transport ID indicating which bus to enumerate. If the trtype
 * is PCIe or trid is NULL, this will scan the local PCIe bus. If the trtype is
 * fabrics (e.g. RDMA, TCP), the traddr and trsvcid must point at the location of an
 * NVMe-oF discovery service.
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of
 * the callbacks.
 * \param probe_cb will be called once per NVMe device found in the system.
 * \param attach_cb will be called for devices for which probe_cb returned true
 * once that NVMe controller has been attached to the userspace driver.
 * \param attach_fail_cb will be called for devices which probe_cb returned true
 * but failed to attach to the userspace driver.
 * \param remove_cb will be called for devices that were attached in a previous
 * spdk_nvme_probe() call but are no longer attached to the system. Optional;
 * specify NULL if removal notices are not desired.
 *
 * \return probe context on success, NULL on failure.
 */
/*
 * [한국어]
 * spdk_nvme_probe_async_ext - probe_async + attach_fail_cb (실패 통지 추가 비동기 변형).
 *
 * spdk_nvme_probe_async와 같지만 attach_fail_cb를 받는다. NVMe-oF discovery에서 일부 subsystem이
 * 다운되어 있어도 나머지는 정상 attach되도록 하면서 실패 명단을 받고 싶은 경우 사용.
 */
struct spdk_nvme_probe_ctx *spdk_nvme_probe_async_ext(const struct spdk_nvme_transport_id *trid,
		void *cb_ctx,
		spdk_nvme_probe_cb probe_cb,
		spdk_nvme_attach_cb attach_cb,
		spdk_nvme_attach_fail_cb attach_fail_cb,
		spdk_nvme_remove_cb remove_cb);

/**
 * Proceed with attaching controllers associated with the probe context.
 *
 * The probe context is one returned from a previous call to
 * spdk_nvme_probe_async().  Users must call this function on the
 * probe context until it returns 0.
 *
 * If any controllers fail to attach, there is no explicit notification.
 * Users can detect attachment failure by comparing attach_cb invocations
 * with the number of times where the user returned true for the
 * probe_cb.
 *
 * \param probe_ctx Context used to track probe actions.
 *
 * \return 0 if all probe operations are complete; the probe_ctx
 * is also freed and no longer valid.
 * \return -EAGAIN if there are still pending probe operations; user must call
 * spdk_nvme_probe_poll_async again to continue progress.
 */
/*
 * [한국어]
 * spdk_nvme_probe_poll_async - 비동기 probe context의 attach state machine을 한 step 진행.
 *
 * @probe_ctx: probe_async/connect_async 반환값.
 * @return: 0=모든 attach 완료(probe_ctx 자동 free), -EAGAIN=아직 진행 중 다시 호출 필요.
 *
 * 사용 패턴: do { rc = probe_poll_async(ctx); } while (rc == -EAGAIN);
 * 또는 SPDK poller로 등록해 reactor가 매 라운드마다 호출. 호출 컨텍스트는 probe_async 호출한
 * 동일 SPDK 스레드여야 함 (cross-thread 금지).
 */
int spdk_nvme_probe_poll_async(struct spdk_nvme_probe_ctx *probe_ctx);

/**
 * Detach specified device returned by spdk_nvme_probe()'s attach_cb from the
 * NVMe driver.
 *
 * On success, the spdk_nvme_ctrlr handle is no longer valid.
 *
 * This function should be called from a single thread while no other threads
 * are actively using the NVMe device.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_nvme_detach - 컨트롤러를 SPDK 드라이버에서 분리하고 자원 해제 (동기, 블로킹).
 *
 * @ctrlr: 분리할 컨트롤러 (probe/connect로 받은 핸들).
 * @return: 0 성공, -1 실패.
 *
 * 호출 후 핸들 invalid - 어떤 ctrlr_*/qpair_*/ns_* 호출도 금지. 사용자가 모든 qpair를 free하고
 * I/O가 더 이상 진행 중이지 않음을 보장한 뒤 단일 스레드에서 호출. PCIe shutdown notification(CC.SHN=
 * Normal) 후 BAR unmap. fabrics는 connection close.
 */
int spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr);

struct spdk_nvme_detach_ctx;
/* [한국어] 다중 컨트롤러 비동기 detach를 누적/추적하는 불투명 구조체. */

/**
 * Allocate a context to track detachment of multiple controllers if this call is the
 * first successful start of detachment in a sequence, or use the passed context otherwise.
 *
 * Then, start detaching the specified device returned by spdk_nvme_probe()'s attach_cb
 * from the NVMe driver, and append this detachment to the context.
 *
 * User must call spdk_nvme_detach_poll_async() to complete the detachment.
 *
 * If the context is not allocated before this call, and if the specified device is detached
 * locally from the caller process but any other process still attaches it or failed to be
 * detached, context is not allocated.
 *
 * This function should be called from a single thread while no other threads are
 * actively using the NVMe device.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param detach_ctx Reference to the context in a sequence. An new context is allocated
 * if this call is the first successful start of detachment in a sequence, or use the
 * passed context.
 */
/*
 * [한국어]
 * spdk_nvme_detach_async - 비동기 detach 시작 + 다중 컨트롤러 detach 누적.
 *
 * @ctrlr: 분리할 컨트롤러.
 * @detach_ctx: [in/out] 첫 호출 시 *detach_ctx==NULL 이면 새 컨텍스트 할당, 이후 호출 시 같은
 *              포인터를 넘기면 다중 detach가 한 컨텍스트에 누적되어 spdk_nvme_detach_poll_async() 한
 *              번에 폴링 가능. multi-process에서 다른 프로세스가 여전히 attach 중이면 ctx는 할당 안됨.
 *
 * 다중 detach 사용 패턴: 여러 ctrlr 각각 detach_async(&ctx) → 모두 호출 후 detach_poll_async(ctx)로
 * 일괄 폴링. SPDK_APP shutdown 시 효율적 종료에 핵심.
 */
int spdk_nvme_detach_async(struct spdk_nvme_ctrlr *ctrlr,
			   struct spdk_nvme_detach_ctx **detach_ctx);

/**
 * Poll detachment of multiple controllers until they complete.
 *
 * User must call this function until it returns 0.
 *
 * \param detach_ctx Context to track the detachment.
 *
 * \return 0 if all detachments complete; the context is also freed and no longer valid.
 * \return -EAGAIN if any detachment is still in progress; users must call
 * spdk_nvme_detach_poll_async() again to continue progress.
 */
/*
 * [한국어]
 * spdk_nvme_detach_poll_async - detach_ctx 진행 (한 step 폴링).
 *
 * @return: 0=모든 detach 완료(ctx 자동 free), -EAGAIN=아직 진행 중.
 * 사용자는 0 반환 때까지 반복 호출. detach_async와 동일 스레드에서 호출 필수.
 */
int spdk_nvme_detach_poll_async(struct spdk_nvme_detach_ctx *detach_ctx);

/**
 * Continue calling spdk_nvme_detach_poll_async() internally until it returns 0.
 *
 * \param detach_ctx Context to track the detachment.
 */
/*
 * [한국어]
 * spdk_nvme_detach_poll - detach_poll_async를 내부에서 0 반환할 때까지 자동 반복.
 *
 * 블로킹 헬퍼 - shutdown 경로처럼 그냥 끝까지 기다리면 되는 곳에서 사용. 비동기 처리 필요 없으면 이걸로 단순화.
 */
void spdk_nvme_detach_poll(struct spdk_nvme_detach_ctx *detach_ctx);

/**
 * Scan attached controllers for events.
 *
 * This function lets user act on events such as hot-remove without a need to
 * enable hotplug explicitly. Only attached devices will be checked.
 *
 * \param trid Transport ID.
 *
 * \returns 0 on success, negative on failure.
 */
/*
 * [한국어]
 * spdk_nvme_scan_attached - 기존 attach된 컨트롤러에 대해 hot-remove 같은 이벤트 스캔.
 *
 * @trid: 스캔 대상 트랜스포트 (PCIe면 핫리무브 감지).
 * @return: 0 성공, 음수 실패.
 *
 * 사용자가 spdk_nvme_probe 의 enable_hotplug 없이도 핫이벤트 처리하고 싶을 때 명시 호출.
 * 새 디바이스는 검사하지 않고 기존 attach된 디바이스의 상태만 갱신.
 */
int spdk_nvme_scan_attached(const struct spdk_nvme_transport_id *trid);

/**
 * Update the transport ID for a given controller.
 *
 * This function allows the user to set a new trid for a controller only if the
 * controller is failed. The controller's failed state can be obtained from
 * spdk_nvme_ctrlr_is_failed(). The controller can also be forced to the failed
 * state using spdk_nvme_ctrlr_fail().
 *
 * This function also requires that the transport type and subnqn of the new trid
 * be the same as the old trid.
 *
 * \param ctrlr Opaque handle to an NVMe controller.
 * \param trid The new transport ID.
 *
 * \return 0 on success, -EINVAL if the trid is invalid,
 * -EPERM if the ctrlr is not failed.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_set_trid - failed 컨트롤러의 trid를 다른 endpoint로 교체 (NVMe-oF 페일오버).
 *
 * @ctrlr: 변경할 컨트롤러 (반드시 failed 상태여야 함 - is_failed()=true).
 * @trid: 새 transport ID (트랜스포트 타입과 subnqn은 기존과 동일해야 함).
 * @return: 0 성공, -EINVAL trid 부적합, -EPERM ctrlr가 failed 아님.
 *
 * 사용 시나리오: NVMe-oF target이 죽었는데 같은 subsystem이 다른 IP로 재기동 → 같은 ctrlr 핸들로
 * 새 IP에 reconnect하여 상위(bdev_nvme)가 핸들 변경 없이 자동 페일오버. 호출 후 reset 필요.
 */
int spdk_nvme_ctrlr_set_trid(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_transport_id *trid);

/**
 * Set the remove callback and context to be invoked if the controller is removed.
 *
 * This will override any remove_cb and/or ctx specified when the controller was
 * probed.
 *
 * This function may only be called from the primary process.  This function has
 * no effect if called from a secondary process.
 *
 * \param ctrlr Opaque handle to an NVMe controller.
 * \param remove_cb remove callback
 * \param remove_ctx remove callback context
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_set_remove_cb - probe 시 등록한 remove_cb를 다른 콜백으로 교체.
 *
 * primary process에서만 동작 (secondary process 호출은 무시). 동적으로 hot-remove 처리 정책을
 * 바꾸고 싶을 때 사용 - 예: 정상 작동 중에는 reconnect 시도, shutdown 단계에서는 즉시 detach.
 */
void spdk_nvme_ctrlr_set_remove_cb(struct spdk_nvme_ctrlr *ctrlr,
				   spdk_nvme_remove_cb remove_cb, void *remove_ctx);

/*
 * [한국어]
 * struct spdk_nvme_ctrlr_key_opts - DH-HMAC-CHAP 키 동적 갱신 옵션.
 *
 * 일반적으로 ctrlr_opts.dhchap_key/dhchap_ctrlr_key 는 attach 시 1회 설정. 키를 런타임에 교체할
 * 때 spdk_nvme_ctrlr_set_keys()에 이 구조체를 넘긴다. ABI 안전을 위해 size 필드 포함.
 */
struct spdk_nvme_ctrlr_key_opts {
	/** Size of this structure */
	size_t size;
	/* [한국어] sizeof(struct spdk_nvme_ctrlr_key_opts) - 향후 키 종류 추가에 대비한 ABI guard. */
	/** DH-HMAC-CHAP host key */
	struct spdk_key *dhchap_key;
	/* [한국어] 새 호스트 키 (NULL 허용 - 키 제거). attach 후 발급된 spdk_key 핸들. */
	/** DH-HMAC-CHAP controller key */
	struct spdk_key *dhchap_ctrlr_key;
	/* [한국어] 새 컨트롤러 키 (양방향 인증용, NULL 허용). */
};

/**
 * Set keys for a given NVMe controller.  These keys will override the keys specified in
 * `spdk_nvme_ctrlr_opts` when attaching the controller and will be used from now on to authenticate
 * all qpairs associated with this controller.
 *
 * This function only sets the keys, it doesn't force existing qpairs to use them.  To do that,
 * users need to call `spdk_nvme_ctrlr_authenticate()` to authenticate the admin queue and
 * `spdk_nvme_qpair_authenticate()` to authenticate IO queues.
 *
 * \param ctrlr NVMe controller.
 * \param opts Key options.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_set_keys - 컨트롤러 인증 키를 런타임에 교체.
 *
 * @opts: 새 키 설정 (size 필드 필수).
 * @return: 0 성공, 음수 errno 실패.
 *
 * 키만 갱신할 뿐 기존 qpair 인증 상태는 변경 안 함. 새 키로 재인증 하려면 추가로
 * spdk_nvme_ctrlr_authenticate(admin)와 spdk_nvme_qpair_authenticate(I/O) 명시 호출 필요.
 * 키 회전(key rotation) 정책 구현에 사용.
 */
int spdk_nvme_ctrlr_set_keys(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ctrlr_key_opts *opts);

/**
 * Perform a full hardware reset of the NVMe controller.
 *
 * This function should be called from a single thread while no other threads
 * are actively using the NVMe device.
 *
 * Any pointers returned from spdk_nvme_ctrlr_get_ns(), spdk_nvme_ns_get_data(),
 * spdk_nvme_zns_ns_get_data(), and spdk_nvme_zns_ctrlr_get_data()
 * may be invalidated by calling this function. The number of namespaces as returned
 * by spdk_nvme_ctrlr_get_num_ns() may also change.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_reset - 동기 컨트롤러 리셋 (블로킹).
 *
 * @ctrlr: 리셋할 컨트롤러.
 * @return: 0 성공, -1 실패.
 *
 * PCIe: CC.EN=0 → CSTS.RDY=0 대기 → CC.EN=1 → CSTS.RDY=1 대기 (NVMe spec 7.3 reset sequence).
 * Fabrics: 모든 qpair disconnect 후 admin queue connect 재시도.
 *
 * 단일 스레드에서 호출 + 다른 스레드가 ctrlr 사용 중이면 안 됨. ★ 부작용:
 * spdk_nvme_ns_get_data, spdk_nvme_ctrlr_get_ns 등이 반환한 모든 포인터가 invalidate.
 * 네임스페이스 수도 변경 가능. 호출 후 모든 관련 데이터 재조회 필요.
 *
 * 비동기 모델 원하면 disconnect → reconnect_async → reconnect_poll_async 사용.
 */
int spdk_nvme_ctrlr_reset(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Disconnect the given NVMe controller.
 *
 * This function is used as the first operation of a full reset sequence of the given NVMe
 * controller. The NVMe controller is ready to reconnect after completing this function.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return 0 on success, -EBUSY if controller is already resetting, or -ENXIO if controller
 * has been removed.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_disconnect - 비동기 reset 시퀀스의 첫 단계: 컨트롤러 단절.
 *
 * @return: 0 성공, -EBUSY 이미 리셋 중, -ENXIO 핫리무브된 상태.
 *
 * 모든 qpair disconnect + admin shutdown. 호출 후 reconnect_async/poll로 다시 살릴 수 있음.
 * 비동기 reset 패턴: disconnect() → reconnect_async() → 반복 reconnect_poll_async() 까지 -EAGAIN 아닐 때까지.
 */
int spdk_nvme_ctrlr_disconnect(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Start re-enabling the given NVMe controller in a full reset sequence
 *
 * \param ctrlr Opaque handle to NVMe controller.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_reconnect_async - disconnect 후 비동기 re-enable 시작.
 *
 * 반환값 없음 - 시작만 표시. 이후 reconnect_poll_async() 반복 호출이 진행 책임.
 * fabrics: connect 재시도 + identify 재실행. PCIe: CC.EN=1 + 큐 재구축.
 */
void spdk_nvme_ctrlr_reconnect_async(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Proceed with re-enabling the given NVMe controller.
 *
 * Users must call this function in a full reset sequence until it returns a value other
 * than -EAGAIN.
 *
 * \return 0 if the given NVMe controller is enabled, or -EBUSY if there are still
 * pending operations to enable it.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_reconnect_poll_async - reconnect state machine 1 step 진행.
 *
 * @return: 0=enable 완료, -EBUSY=아직 진행 중 다시 호출 필요, 음수 errno=실패.
 *
 * 호출 컨텍스트: ctrlr를 소유한 단일 SPDK 스레드에서만. NVMe-oF 환경에서 transient network failure
 * 시 reconnect 루프 구현에 사용. SPDK 자체 retry 로직(transport_retry_count) 외부에서 정책 제어.
 */
int spdk_nvme_ctrlr_reconnect_poll_async(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Perform a NVMe subsystem reset.
 *
 * This function should be called from a single thread while no other threads
 * are actively using the NVMe device.
 * A subsystem reset is typically seen by the OS as a hot remove, followed by a
 * hot add event.
 *
 * Any pointers returned from spdk_nvme_ctrlr_get_ns(), spdk_nvme_ns_get_data(),
 * spdk_nvme_zns_ns_get_data(), and spdk_nvme_zns_ctrlr_get_data()
 * may be invalidated by calling this function. The number of namespaces as returned
 * by spdk_nvme_ctrlr_get_num_ns() may also change.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return 0 on success, -1 on failure, -ENOTSUP if subsystem reset is not supported.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_reset_subsystem - NVMe Subsystem Reset (전체 subsystem hot remove + add 효과).
 *
 * @return: 0 성공, -1 실패, -ENOTSUP 미지원.
 *
 * NVMe spec 5.27 NSSR (NVM Subsystem Reset). NSSC.NSSRC=0x4E564D65 매직 넘버 write로 트리거.
 * 같은 subsystem의 모든 컨트롤러가 함께 리셋. CAP.NSSRS=1 디바이스만 지원. ctrlr_reset보다 강력.
 * OS 입장에서 핫리무브 + 핫애드처럼 보이므로 사용자도 모든 ns/qpair 자료를 재조회해야 함.
 */
int spdk_nvme_ctrlr_reset_subsystem(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Fail the given NVMe controller.
 *
 * This function gives the application the opportunity to fail a controller
 * at will. When a controller is failed, any calls to process completions or
 * submit I/O on qpairs associated with that controller will fail with an error
 * code of -ENXIO.
 * The controller can only be taken from the failed state by
 * calling spdk_nvme_ctrlr_reset. After the controller has been successfully
 * reset, any I/O pending when the controller was moved to failed will be
 * aborted back to the application and can be resubmitted. I/O can then resume.
 *
 * \param ctrlr Opaque handle to an NVMe controller.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_fail - 사용자 명시적으로 컨트롤러를 failed 상태로 강제 전환.
 *
 * 이후 ns_cmd_*/process_completions 모두 -ENXIO 반환. failed에서 빠져나오려면 ctrlr_reset 필요.
 * 사용 시나리오: 상위 모듈이 컨트롤러 동작 이상을 감지했을 때 빠르게 차단하고 fail-over 트리거.
 * pending I/O는 reset 후 일제히 abort되어 사용자에게 반환.
 */
void spdk_nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr);

/**
 * This function returns the failed status of a given controller.
 *
 * \param ctrlr Opaque handle to an NVMe controller.
 *
 * \return True if the controller is failed, false otherwise.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_is_failed - failed 상태 단순 조회. 멀티패스가 빠른 분기에 사용.
 */
bool spdk_nvme_ctrlr_is_failed(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the identify controller data as defined by the NVMe specification.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return pointer to the identify controller data.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_data - NVMe Identify Controller(opcode 0x06, CNS=0x01) 데이터 4096B 반환.
 *
 * 컨트롤러 attach 중 SPDK가 캐시한 spdk_nvme_ctrlr_data 구조체 포인터 반환. NVMe spec Figure 245
 * (NVMe 2.0 기준)의 모든 필드 - VID/SSVID/SN/MN/FR/OACS/NN/CTRATT/SGLS/PSDS 포함. ★ ctrlr_reset 호출
 * 시 무효화. thread-safe (read-only). 호출자: bdev_nvme의 metadata 채우기.
 */
const struct spdk_nvme_ctrlr_data *spdk_nvme_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NVMe controller CSTS (Status) register.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the NVMe controller CSTS (Status) register.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_regs_csts - NVMe spec CSTS(Controller Status, offset 0x1C) MMIO 레지스터 읽기.
 * RDY/CFS(Controller Fatal Status)/SHST/NSSRO/PP/ST 비트 확인. PCIe는 BAR0+0x1C MMIO read,
 * fabrics는 fabric property fetch 명령. 호출자: 디버깅, reset 진행 추적.
 */
union spdk_nvme_csts_register spdk_nvme_ctrlr_get_regs_csts(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NVMe controller CC (Configuration) register.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the NVMe controller CC (Configuration) register.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_regs_cc - CC(Controller Configuration, offset 0x14) 읽기.
 * EN(enable)/CSS(command set)/MPS(memory page size)/AMS/SHN/IOSQES/IOCQES 비트.
 * 호출자: 컨트롤러 enable 상태 확인, log/debug 출력.
 */
union spdk_nvme_cc_register spdk_nvme_ctrlr_get_regs_cc(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NVMe controller CAP (Capabilities) register.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the NVMe controller CAP (Capabilities) register.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_regs_cap - CAP(Controller Capabilities, offset 0x00, 64bit) 읽기.
 * MQES(max queue entries supported)/CQR/AMS/TO(timeout)/DSTRD(doorbell stride)/NSSRS/CSS(command set support)/
 * BPS/MPSMIN/MPSMAX/PMRS/CMBS 비트. 컨트롤러 능력 광고. SPDK init이 가장 먼저 읽음.
 */
union spdk_nvme_cap_register spdk_nvme_ctrlr_get_regs_cap(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NVMe controller VS (Version) register.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the NVMe controller VS (Version) register.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_regs_vs - VS(Version, offset 0x08) 레지스터. NVMe spec major.minor.tertiary
 * 인코딩. SPDK가 1.4 이상이면 ZNS/Boot Partition 등 활용 가능 분기에 사용.
 */
union spdk_nvme_vs_register spdk_nvme_ctrlr_get_regs_vs(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NVMe controller CMBSZ (Controller Memory Buffer Size) register
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the NVMe controller CMBSZ (Controller Memory Buffer Size) register.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_regs_cmbsz - CMBSZ(Controller Memory Buffer Size, offset 0x3C) 레지스터.
 * SQS(SQ supported)/CQS/LISTS/RDS/WDS/SZU(size unit)/SZ. 0이면 CMB 없음. ctrlr_opts.use_cmb_sqs와 결합.
 */
union spdk_nvme_cmbsz_register spdk_nvme_ctrlr_get_regs_cmbsz(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NVMe controller PMRCAP (Persistent Memory Region Capabilities) register.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the NVMe controller PMRCAP (Persistent Memory Region Capabilities) register.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_regs_pmrcap - PMRCAP(Persistent Memory Region Capabilities, offset 0xE00) 레지스터.
 * RDS/WDS/BIR(BAR indicator register)/PMRTU(time unit)/PMRWBM/PMRTO/CMSS. PMR은 NVMe 1.4의 NVDIMM-like 영역.
 */
union spdk_nvme_pmrcap_register spdk_nvme_ctrlr_get_regs_pmrcap(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NVMe controller BPINFO (Boot Partition Information) register.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the NVMe controller BPINFO (Boot Partition Information) register.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_regs_bpinfo - BPINFO(Boot Partition Information, offset 0x40) 레지스터.
 * BPSZ(boot partition size)/BRS(boot read status)/ABPID(active boot partition ID).
 * NVMe spec 7.7 Boot Partition - SPDK가 SSD 펌웨어에서 boot 이미지를 읽어들이는 데 사용.
 */
union spdk_nvme_bpinfo_register spdk_nvme_ctrlr_get_regs_bpinfo(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NVMe controller PMR size.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the NVMe controller PMR size or 0 if PMR is not supported.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_pmrsz - PMR 크기 byte 단위 반환. 0=PMR 미지원.
 * PMRCAP.PMRTU/PMRSZ를 합쳐 byte 단위로 환산. PMR write/read API 사용 전 사이즈 검사용.
 */
uint64_t spdk_nvme_ctrlr_get_pmrsz(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the maximum NSID value that will ever be used for the given controller
 *
 * This function is thread safe and can be called at any point while the
 * controller is attached to the SPDK NVMe driver.
 *
 * This is equivalent to calling spdk_nvme_ctrlr_get_data() to get the
 * spdk_nvme_ctrlr_data and then reading the nn field.
 *
 * The NN field in the NVMe specification represents the maximum value that a
 * namespace ID can ever have. Prior to NVMe 1.2, this was also the number of
 * active namespaces, but from 1.2 onward the list of namespaces may be
 * sparsely populated. Unfortunately, the meaning of this field is often
 * misinterpreted by drive manufacturers and NVMe-oF implementers so it is
 * not considered reliable. AVOID USING THIS FUNCTION WHENEVER POSSIBLE.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the number of namespaces.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_num_ns - identify ctrlr.NN 필드 반환 (max nsid 값).
 *
 * ★ 주의: NVMe 1.2 이후 ns 리스트는 sparse 가능 - NN이 곧 active ns 수가 아니다. 또한 일부
 * 펌웨어는 NN 의미를 잘못 구현. 일반적으로 spdk_nvme_ctrlr_get_first/next_active_ns()로 enumerate
 * 권장. 본 함수는 backward compat용. AVOID USING THIS FUNCTION WHENEVER POSSIBLE.
 */
uint32_t spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the PCI device of a given NVMe controller.
 *
 * This only works for local (PCIe-attached) NVMe controllers; other transports
 * will return NULL.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return PCI device of the NVMe controller, or NULL if not available.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_pci_device - PCIe ctrlr의 spdk_pci_device 핸들 반환. fabrics면 NULL.
 * 사용자가 PCIe BAR/config space/이름 등 추가 메타데이터에 접근할 때.
 */
struct spdk_pci_device *spdk_nvme_ctrlr_get_pci_device(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NUMA ID for the given NVMe controller.
 *
 * For network-based transports, the NUMA ID will be correlated to the
 * network interface.
 *
 * \param ctrlr Opaque handle to NVMe controller
 *
 * \return NUMA ID of the NVMe controller, or SPDK_ENV_NUMA_ID_ANY if
 *         the NUMA ID is unknown
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_numa_id - 컨트롤러가 속한 NUMA 노드 ID. PCIe면 PCIe root complex,
 * fabrics면 사용 NIC의 NUMA. 미상이면 SPDK_ENV_NUMA_ID_ANY. NUMA-aware buffer 할당 정책에 사용.
 */
int32_t spdk_nvme_ctrlr_get_numa_id(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NVMe controller ID for the given controller.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return ID of the NVMe controller.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_id - NVMe spec의 Controller ID(CNTLID, identify ctrlr.cntlid 필드, 16bit).
 * NVMe-oF target에서 다중 컨트롤러 인스턴스를 구분. 같은 subsystem 내 유일.
 */
uint16_t spdk_nvme_ctrlr_get_id(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the maximum data transfer size of a given NVMe controller.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return Maximum data transfer size of the NVMe controller in bytes.
 *
 * The I/O command helper functions, such as spdk_nvme_ns_cmd_read(), will split
 * large I/Os automatically; however, it is up to the user to obey this limit for
 * commands submitted with the raw command functions, such as spdk_nvme_ctrlr_cmd_io_raw().
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_max_xfer_size - MDTS(Maximum Data Transfer Size, identify ctrlr.mdts) 기반
 * byte 단위 최대 전송 크기 반환. ns_cmd_*는 자동 split하지만 cmd_io_raw 사용자는 직접 준수해야 함.
 */
uint32_t spdk_nvme_ctrlr_get_max_xfer_size(const struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the maximum number of SGEs per request for the given NVMe controller.
 *
 * Controllers that do not support SGL will return UINT16_MAX.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return Maximum number of SGEs per request
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_max_sges - 한 명령에 첨부 가능한 최대 SGE(Scatter-Gather Entry) 수.
 * SGL 미지원 컨트롤러는 UINT16_MAX 반환. iovec 기반 readv/writev 발행 전 split 결정에 사용.
 */
uint16_t spdk_nvme_ctrlr_get_max_sges(const struct spdk_nvme_ctrlr *ctrlr);

/**
 * Check whether the nsid is an active nv for the given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param nsid Namespace id.
 *
 * \return true if nsid is an active ns, or false otherwise.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_is_active_ns - 주어진 nsid가 active(접근 가능한) ns인지 검사.
 * SPDK가 캐시한 active ns list와 비교. ns 1..NN 범위라도 inactive일 수 있음 (sparse).
 */
bool spdk_nvme_ctrlr_is_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid);

/**
 * Get the nsid of the first active namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the nsid of the first active namespace, 0 if there are no active namespaces.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_first_active_ns - active ns 순회 시작점 nsid 반환. 없으면 0.
 * 사용 패턴: nsid = first_active(); while (nsid) { ... ; nsid = next_active(nsid); }
 */
uint32_t spdk_nvme_ctrlr_get_first_active_ns(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get next active namespace given the previous nsid.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param prev_nsid Namespace id.
 *
 * \return a next active namespace given the previous nsid, 0 when there are no
 * more active namespaces.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_next_active_ns - prev_nsid 이후 첫 active nsid. 끝이면 0.
 * NVMe Identify Active Namespace List(CNS=0x02) 캐시를 순회. ctrlr_reset 시 무효화.
 */
uint32_t spdk_nvme_ctrlr_get_next_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t prev_nsid);

/**
 * Determine if a particular log page is supported by the given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \sa spdk_nvme_ctrlr_cmd_get_log_page().
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param log_page Log page to query.
 *
 * \return true if supported, or false otherwise.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_is_log_page_supported - 컨트롤러가 특정 LID(0x01..0xFF)의 Get Log Page를 지원하는지.
 * identify ctrlr.LPA 비트와 vendor-specific log page 캐시를 종합해 결정. 호출 전 spec 검사 패턴.
 */
bool spdk_nvme_ctrlr_is_log_page_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page);

/**
 * Determine if a particular feature is supported by the given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \sa spdk_nvme_ctrlr_cmd_get_feature().
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param feature_code Feature to query.
 *
 * \return true if supported, or false otherwise.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_is_feature_supported - Get/Set Features의 FID 지원 여부.
 * NVMe spec의 Feature Identifiers Supported and Effects log page(LID=0x12)를 SPDK가 캐시.
 */
bool spdk_nvme_ctrlr_is_feature_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature_code);

/**
 * Signature for callback function invoked when a command is completed.
 *
 * \param ctx Callback context provided when the command was submitted.
 * \param cpl Completion queue entry that contains the completion status.
 */
/*
 * [한국어]
 * spdk_nvme_cmd_cb - ★ NVMe 명령 완료 콜백 typedef (SPDK NVMe 드라이버의 핵심 콜백).
 *
 * @ctx: ns_cmd_*/ctrlr_cmd_* 호출 시 사용자가 넘긴 컨텍스트.
 * @cpl: 16B Completion Queue Entry. cpl.status.sc(status code), cpl.status.sct(status type),
 *       cpl.cdw0(명령별 결과 데이터), cpl.sqid/cid 포함. spdk_nvme_cpl_is_error()로 빠른 검사.
 *
 * 호출 컨텍스트: spdk_nvme_qpair_process_completions() 가 CQ에서 entry를 pull하면서 디스패치.
 * 즉 사용자가 process_completions를 호출한 그 SPDK 스레드에서 실행됨. 콜백 안에서 같은 qpair에
 * 추가 ns_cmd 발행 가능 (재진입 안전). 호출 체인: 사용자 → qpair_process_completions → CQE 디스패치 → cb_fn.
 */
typedef void (*spdk_nvme_cmd_cb)(void *ctx, const struct spdk_nvme_cpl *cpl);

/**
 * Signature for callback function invoked when an asynchronous event request
 * command is completed.
 *
 * \param aer_cb_arg Context specified by spdk_nvme_register_aer_callback().
 * \param cpl Completion queue entry that contains the completion status
 * of the asynchronous event request that was completed.
 */
/*
 * [한국어]
 * spdk_nvme_aer_cb - AER(Asynchronous Event Request, opcode 0x0C) 완료 콜백.
 *
 * SPDK가 admin queue에 AER 명령을 항상 N개 띄워둠. 컨트롤러 측 이벤트(Smart/Health 임계,
 * 네임스페이스 변경, FW activation, ANA change 등) 발생 시 그 중 하나가 완료되며 콜백 호출.
 * cpl.cdw0의 Async Event Type/Information으로 이벤트 종류 식별. 호출 컨텍스트: process_admin_completions.
 */
typedef void (*spdk_nvme_aer_cb)(void *aer_cb_arg,
				 const struct spdk_nvme_cpl *cpl);

/**
 * Register callback function invoked when an AER command is completed for the
 * given NVMe controller.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param aer_cb_fn Callback function invoked when an asynchronous event request
 * command is completed.
 * \param aer_cb_arg Argument passed to callback function.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_register_aer_callback - AER 이벤트 사용자 콜백 등록.
 *
 * 한 컨트롤러당 한 콜백. NULL로 호출하면 등록 해제. 사용자는 등록 후 ns 추가/삭제,
 * SMART/Health 경고, FW commit 알림 등을 자동 수신. 미등록 시 SPDK가 기본 핸들러로 자체 처리.
 */
void spdk_nvme_ctrlr_register_aer_callback(struct spdk_nvme_ctrlr *ctrlr,
		spdk_nvme_aer_cb aer_cb_fn,
		void *aer_cb_arg);

/**
 * Disable reading the CHANGED_NS_LIST log page for the specified controller.
 *
 * Applications that register an AER callback may wish to read the CHANGED_NS_LIST
 * log page itself, rather than relying on the driver to do it.  Calling this
 * function will ensure that the driver does not read this log page if the
 * controller returns a NS_ATTR_CHANGED AEN.
 *
 * Reading of this log page can alternatively be disabled by setting the
 * disable_read_changed_ns_list_log_page flag in the spdk_nvme_ctrlr_opts
 * when attaching the controller.
 *
 * \param ctrlr NVMe controller on which to disable the log page read.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_disable_read_changed_ns_list_log_page - NS_ATTR_CHANGED AEN 발생 시 SPDK가
 * 자동으로 LID=0x70(Changed Namespace List) 로그를 읽지 않게 함. 상위가 직접 로그 파싱할 때 사용.
 * ctrlr_opts.disable_read_changed_ns_list_log_page=1 과 동일 효과를 attach 후에도 적용.
 */
void spdk_nvme_ctrlr_disable_read_changed_ns_list_log_page(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Opaque handle to a queue pair.
 *
 * I/O queue pairs may be allocated using spdk_nvme_ctrlr_alloc_io_qpair().
 */
/*
 * [한국어]
 * struct spdk_nvme_qpair - 불투명 NVMe queue pair (SQ + CQ 한 쌍) 핸들.
 *
 * 실제 정의는 lib/nvme/nvme_internal.h. 한 컨트롤러는 다수 qpair를 가질 수 있고 각 qpair는
 * 독립 SQ(submission)/CQ(completion) ring을 가진다. SQE는 64B, CQE는 16B. SQ tail doorbell write로
 * 디바이스에 통지, CQ는 디바이스가 phase bit를 토글한 entry를 pull.
 *
 * ★ thread affinity: 한 qpair는 정확히 한 SPDK 스레드 전용 (process_completions, ns_cmd_* 모두 동일
 * 스레드에서 호출). 다른 스레드 사용 금지 - lockless 설계의 핵심 규약.
 */
struct spdk_nvme_qpair;

/**
 * Signature for the callback function invoked when a timeout is detected on a
 * request.
 *
 * For timeouts detected on the admin queue pair, the qpair returned here will
 * be NULL.  If the controller has a serious error condition and is unable to
 * communicate with driver via completion queue, the controller can set Controller
 * Fatal Status field to 1, then reset is required to recover from such error.
 * Users may detect Controller Fatal Status when timeout happens.
 *
 * \param cb_arg Argument passed to callback function.
 * \param ctrlr Opaque handle to NVMe controller.
 * \param qpair Opaque handle to a queue pair.
 * \param cid Command ID.
 */
/*
 * [한국어]
 * spdk_nvme_timeout_cb - 명령 timeout 감지 콜백.
 *
 * @qpair: timeout 발생 qpair. admin queue timeout이면 NULL.
 * @cid: 16bit Command ID (SQE의 cdw0[31:16]).
 *
 * SPDK가 매 ms 단위로 outstanding 명령들의 발행 시각을 검사 - 등록된 timeout_us 초과면 호출.
 * 사용자는 ctrlr_fail/reset/abort 중 정책 결정. CSTS.CFS=1이면 컨트롤러가 fatal 상태 - reset 필수.
 * 콜백 컨텍스트: process_completions가 폴링 중 timeout 검출.
 */
typedef void (*spdk_nvme_timeout_cb)(void *cb_arg,
				     struct spdk_nvme_ctrlr *ctrlr,
				     struct spdk_nvme_qpair *qpair,
				     uint16_t cid);

/**
 * Register for timeout callback on a controller.
 *
 * The application can choose to register for timeout callback or not register
 * for timeout callback.
 *
 * \param ctrlr NVMe controller on which to monitor for timeout.
 * \param timeout_io_us Timeout value in microseconds for io commands.
 * \param timeout_admin_us Timeout value in microseconds for admin commands.
 * \param cb_fn A function pointer that points to the callback function.
 * \param cb_arg Argument to the callback function.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_register_timeout_callback - 명령 timeout 임계값과 콜백 등록.
 *
 * @timeout_io_us: I/O 명령 timeout 마이크로초 (0=무제한).
 * @timeout_admin_us: admin 명령 timeout. 0=무제한.
 * @cb_fn: timeout 발생 시 호출되는 사용자 콜백.
 *
 * 등록된 timeout보다 먼저 ctrlr_opts.admin_timeout_ms가 init 단계에 적용. 사용자 콜백은 attach 후
 * 활성화. NVMe-oF 환경에서 keep-alive와 별개로 명령 단위 latency 가드 구현에 사용.
 */
void spdk_nvme_ctrlr_register_timeout_callback(struct spdk_nvme_ctrlr *ctrlr,
		uint64_t timeout_io_us, uint64_t timeout_admin_us,
		spdk_nvme_timeout_cb cb_fn, void *cb_arg);

/**
 * Signature for the callback function when a
 * \ref spdk_nvme_ctrlr_get_discovery_log_page operation is completed.
 *
 * \param cb_arg Argument passed to callback function.
 * \param rc Status of operation. 0 means success, and that the cpl argument is valid.
 *           Failure indicated by negative errno value.
 * \param cpl NVMe completion status of the operation. NULL if rc != 0. If multiple
 *            completions with error status occurred during the operation, the cpl
 *            value for the first error will be used here.
 * \param log_page Pointer to the full discovery log page. The application is
 *                 responsible for freeing this buffer using free().
 */
/*
 * [한국어]
 * spdk_nvme_discovery_cb - NVMe-oF Discovery Log Page 수신 완료 콜백.
 *
 * @rc: 0=성공, 음수=실패(cpl 무효).
 * @cpl: NVMe completion. 첫 에러의 status 보관 (다중 명령 중 첫 실패).
 * @log_page: 전체 discovery log 데이터 (header + entries 배열). ★ 사용자가 free() 책임.
 *
 * 호출 시점: spdk_nvme_ctrlr_get_discovery_log_page 가 내부적으로 여러 Get Log Page 호출 후 모두
 * 완료되면 통합 결과를 단일 콜백으로 사용자에게 전달. discovery service에서 active subsystem 목록 획득.
 */
typedef void (*spdk_nvme_discovery_cb)(void *cb_arg, int rc,
				       const struct spdk_nvme_cpl *cpl,
				       struct spdk_nvmf_discovery_log_page *log_page);

/**
 * Get a full discovery log page from the specified controller.
 *
 * This function will first read the discovery log header to determine the
 * total number of valid entries in the discovery log, then it will allocate
 * a buffer to hold the entire log and issue multiple GET_LOG_PAGE commands to
 * get all of the entries.
 *
 * The application is responsible for calling
 * \ref spdk_nvme_ctrlr_process_admin_completions to trigger processing of
 * completions submitted by this function.
 *
 * \param ctrlr Pointer to the discovery controller.
 * \param cb_fn Function to call when the operation is complete.
 * \param cb_arg Argument to pass to cb_fn.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_discovery_log_page - NVMe-oF discovery log 자동 fetch.
 *
 * Discovery controller에 (1) header 읽어 num entries 확인 → (2) 전체 크기 buffer alloc →
 * (3) 다중 Get Log Page(LID=0x70)로 page 분할 fetch → (4) 통합 후 cb_fn 호출.
 * 사용자는 spdk_nvme_ctrlr_process_admin_completions()를 주기 호출해야 진행. 시스템 fabrics
 * subsystem enumerate에 사용.
 */
int spdk_nvme_ctrlr_get_discovery_log_page(struct spdk_nvme_ctrlr *ctrlr,
		spdk_nvme_discovery_cb cb_fn, void *cb_arg);

/**
 * NVMe I/O queue pair initialization options.
 *
 * These options may be passed to spdk_nvme_ctrlr_alloc_io_qpair() to configure queue pair
 * options at queue creation time.
 *
 * The user may retrieve the default I/O queue pair creation options for a controller using
 * spdk_nvme_ctrlr_get_default_io_qpair_opts().
 */
/*
 * [한국어]
 * struct spdk_nvme_io_qpair_opts - I/O qpair 생성 옵션 (80B, ABI guarded).
 *
 * spdk_nvme_ctrlr_alloc_io_qpair() 인자로 사용. 컨트롤러의 ctrlr_opts와 별개로 qpair마다 다른
 * queue depth/priority/SQ-CQ 메모리 위치/SGE merge 등 세부 동작 제어. user-supplied SQ/CQ 메모리
 * 전달로 RDMA registered memory 등 고급 활용 가능.
 */
struct spdk_nvme_io_qpair_opts {
	/**
	 * Queue priority for weighted round robin arbitration.  If a different arbitration
	 * method is in use, pass 0.
	 */
	enum spdk_nvme_qprio qprio;
	/* [한국어] WRR 모드에서의 우선순위 - URGENT/HIGH/MEDIUM/LOW 4단계.
	 * ctrlr_opts.arb_mechanism=WRR 일 때만 의미. 다른 모드에서는 0 전달. */

	/**
	 * The queue depth of this NVMe I/O queue. Overrides spdk_nvme_ctrlr_opts::io_queue_size.
	 */
	uint32_t io_queue_size;
	/* [한국어] 이 qpair만의 SQ/CQ 깊이 (ctrlr_opts.io_queue_size override). 컨트롤러 MQES+1 한도.
	 * 작은 큐 = 낮은 latency/높은 throughput trade-off, 큰 큐 = batching 효율. */

	/**
	 * The number of requests to allocate for this NVMe I/O queue.
	 *
	 * Overrides spdk_nvme_ctrlr_opts::io_queue_requests.
	 *
	 * This should be at least as large as io_queue_size.
	 *
	 * A single I/O may allocate more than one request, since splitting may be
	 * necessary to conform to the device's maximum transfer size, PRP list
	 * compatibility requirements, or driver-assisted striping.
	 */
	uint32_t io_queue_requests;
	/* [한국어] 이 qpair에 사전 할당할 spdk_nvme_request 슬롯 수. split I/O가 N슬롯을 동시 점유하므로
	 * io_queue_size 보다 충분히 커야 -ENOMEM 회피 가능. */

	/**
	 * When submitting I/O via spdk_nvme_ns_read/write and similar functions,
	 * don't immediately submit it to hardware. Instead, queue up new commands
	 * and submit them to the hardware inside spdk_nvme_qpair_process_completions().
	 *
	 * This results in better batching of I/O commands. Often, it is more efficient
	 * to submit batches of commands to the underlying hardware than each command
	 * individually.
	 *
	 * This only applies to PCIe and RDMA transports.
	 *
	 * The flag was originally named delay_pcie_doorbell. To allow backward compatibility
	 * both names are kept in unnamed union.
	 */
	union {
		bool delay_cmd_submit;
		/* [한국어] true=ns_cmd_*가 즉시 doorbell write하지 않고 SQE를 모아두었다가 다음
		 * process_completions에서 한 번에 발행 (batching). PCIe doorbell write 비용 절감.
		 * 단점: latency 증가. high-throughput 워크로드에서 켬. */
		bool delay_pcie_doorbell;
		/* [한국어] 동의어 (이전 이름). 호환성을 위해 union으로 유지. delay_cmd_submit과 같은 비트. */
	};

	/* Hole at bytes 13-15. */
	uint8_t reserved13[3];
	/* [한국어] 패딩. 다음 sq/cq 구조체 8-byte align. */

	/**
	 * These fields allow specifying the memory buffers for the submission and/or
	 * completion queues.
	 * By default, vaddr is set to NULL meaning SPDK will allocate the memory to be used.
	 * If vaddr is NULL then paddr must be set to 0.
	 * If vaddr is non-NULL, and paddr is zero, SPDK derives the physical
	 * address for the NVMe device, in this case the memory must be registered.
	 * If a paddr value is non-zero, SPDK uses the vaddr and paddr as passed
	 * SPDK assumes that the memory passed is both virtually and physically
	 * contiguous.
	 * If these fields are used, SPDK will NOT impose any restriction
	 * on the number of elements in the queues.
	 * The buffer sizes are in number of bytes, and are used to confirm
	 * that the buffers are large enough to contain the appropriate queue.
	 * These fields are only used by PCIe attached NVMe devices.  They
	 * are presently ignored for other transports.
	 */
	struct {
		struct spdk_nvme_cmd *vaddr;
		/* [한국어] 사용자 지정 SQ 가상주소. NULL이면 SPDK가 자동 할당. PCIe 전용. */
		uint64_t paddr;
		/* [한국어] 사용자 지정 SQ 물리주소. 0이면 vtophys로 SPDK가 도출 (vaddr 메모리는 hugepage
		 * 등록 필수). 비-0이면 vaddr+paddr 그대로 신뢰 - DMA 가능 메모리 보장 사용자 책임. */
		uint64_t buffer_size;
		/* [한국어] SQ 메모리 영역 byte 크기. 큐 깊이*64 보다 커야 함. */
	} sq;
	struct {
		struct spdk_nvme_cpl *vaddr;
		/* [한국어] 사용자 지정 CQ 가상주소. NULL=자동 할당. */
		uint64_t paddr;
		/* [한국어] CQ 물리주소. 0=자동, 비-0=명시. */
		uint64_t buffer_size;
		/* [한국어] CQ 메모리 영역 byte 크기. 큐 깊이*16 보다 커야 함. */
	} cq;

	/**
	 * This flag indicates to the alloc_io_qpair function that it should not perform
	 * the connect portion on this qpair. This allows the user to add the qpair to a
	 * poll group and then connect it later.
	 */
	bool create_only;
	/* [한국어] true=alloc_io_qpair가 connect 단계 생략 (qpair 메모리만 만듦). 사용자가 poll group
	 * 추가 후 connect_io_qpair 호출하는 패턴. NVMe-oF 비동기 connect에 유용. */

	/**
	 * This flag if set to true enables the creation of submission and completion queue
	 * asynchronously. Default mode is set to false to create io qpair synchronously.
	 */
	bool async_mode;
	/* [한국어] true=Create I/O Submission/Completion Queue admin 명령을 비동기 발행.
	 * 기본 false (동기). 다수 qpair 동시 생성 시 init 가속에 사용. */

	/**
	 * This flag if set to true disables the merging of physically
	 * contiguous SGL elements. Default mode is set to false to allow
	 * merging of physically contiguous SGL elements.
	 */
	bool disable_pcie_sgl_merge;
	/* [한국어] iovec 인접 요소가 물리주소 연속이면 SPDK가 SGL element 1개로 병합 (true면 비활성).
	 * 일부 컨트롤러 quirk 회피용. 일반적으로 false 유지. */

	/* Hole at bytes 67-71. */
	uint8_t reserved67[5];
	/* [한국어] 패딩. opts_size(8B) align. */

	/**
	 * The size of spdk_nvme_io_qpair_opts according to the caller of this library is used for
	 * ABI compatibility. The library uses this field to know how many fields in this structure
	 * are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	/* [한국어] ABI guard - 컴파일된 sizeof. ctrlr_opts와 동일한 ABI 패턴. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_io_qpair_opts) == 80, "Incorrect size");
/* [한국어] ABI guard 80B 검증. 새 옵션은 reserved 슬롯에서만 추가. */

/**
 * Get the default options for I/O qpair creation for a specific NVMe controller.
 *
 * \param ctrlr NVMe controller to retrieve the defaults from.
 * \param[out] opts Will be filled with the default options for
 * spdk_nvme_ctrlr_alloc_io_qpair().
 * \param opts_size Must be set to sizeof(struct spdk_nvme_io_qpair_opts).
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_default_io_qpair_opts - qpair 생성용 기본 옵션 채움.
 *
 * @ctrlr: 컨트롤러 (트랜스포트별 권장 default 결정).
 * @opts: [out] 기본값으로 채움.
 * @opts_size: 호출자 컴파일 sizeof.
 */
void spdk_nvme_ctrlr_get_default_io_qpair_opts(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_io_qpair_opts *opts,
		size_t opts_size);

/**
 * Allocate an I/O queue pair (submission and completion queue).
 *
 * This function by default also performs any connection activities required for
 * a newly created qpair. To avoid that behavior, the user should set the create_only
 * flag in the opts structure to true.
 *
 * Each queue pair should only be used from a single thread at a time (mutual
 * exclusion must be enforced by the user).
 *
 * \param ctrlr NVMe controller for which to allocate the I/O queue pair.
 * \param opts I/O qpair creation options, or NULL to use the defaults as returned
 * by spdk_nvme_ctrlr_get_default_io_qpair_opts().
 * \param opts_size Must be set to sizeof(struct spdk_nvme_io_qpair_opts), or 0
 * if opts is NULL.
 *
 * \return a pointer to the allocated I/O queue pair.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_alloc_io_qpair - I/O qpair 할당 + (기본) 자동 connect.
 *
 * @ctrlr: 컨트롤러 핸들.
 * @opts: 옵션 (NULL=default). create_only=true면 connect 단계 건너뜀.
 * @opts_size: sizeof, opts=NULL이면 0.
 * @return: qpair 핸들, NULL=실패.
 *
 * 동작: SQ/CQ 메모리 할당 + Create I/O CQ admin 명령 + Create I/O SQ admin 명령 → ready.
 * ★ qpair는 단일 SPDK 스레드 전용 - 사용자가 mutual exclusion 보장. 일반적으로 alloc 시점의
 * 스레드가 owner. 다른 코어에서 사용하려면 spdk_thread_send_msg로 그 스레드에 작업 위임.
 *
 * 호출 체인: 사용자(보통 attach_cb 안에서) → alloc_io_qpair → 트랜스포트 vtable construct.
 */
struct spdk_nvme_qpair *spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
		const struct spdk_nvme_io_qpair_opts *opts,
		size_t opts_size);

/**
 * Connect a newly created I/O qpair.
 *
 * This function does any connection activities required for a newly created qpair.
 * It should be called after spdk_nvme_ctrlr_alloc_io_qpair has been called with the
 * create_only flag set to true in the spdk_nvme_io_qpair_opts structure.
 *
 * This call will fail if performed on a qpair that is already connected.
 * For reconnecting qpairs, see spdk_nvme_ctrlr_reconnect_io_qpair.
 *
 * For fabrics like TCP and RDMA, this function actually sends the commands over the wire
 * that connect the qpair. For PCIe, this function performs some internal state machine operations.
 *
 * \param ctrlr NVMe controller for which to allocate the I/O queue pair.
 * \param qpair Opaque handle to the qpair to connect.
 *
 * return 0 on success or negated errno on failure. Specifically -EISCONN if the qpair is already connected.
 *
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_connect_io_qpair - create_only=true로 만든 qpair를 실제로 연결.
 *
 * @return: 0 성공, -EISCONN 이미 연결됨, 음수 errno 기타 실패.
 *
 * fabrics(TCP/RDMA)는 fabric Connect 명령 실제 송수신, PCIe는 내부 state 전이만. 미연결 qpair에
 * I/O 발행하면 에러. reconnect용은 별도 spdk_nvme_ctrlr_reconnect_io_qpair 사용.
 */
int spdk_nvme_ctrlr_connect_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);

/**
 * Disconnect the given I/O qpair.
 *
 * This function must be called from the same thread as spdk_nvme_qpair_process_completions
 * and the spdk_nvme_ns_cmd_* functions.
 *
 * After disconnect, calling spdk_nvme_qpair_process_completions or one of the
 * spdk_nvme_ns_cmd* on a qpair will result in a return value of -ENXIO. A
 * disconnected qpair may be reconnected with either the spdk_nvme_ctrlr_connect_io_qpair
 * or spdk_nvme_ctrlr_reconnect_io_qpair APIs.
 *
 * \param qpair The qpair to disconnect.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_disconnect_io_qpair - I/O qpair 단절 (free 아님, 재연결 가능).
 *
 * 호출 후 같은 qpair에 ns_cmd_*/process_completions는 -ENXIO 반환. reconnect_io_qpair로 부활 가능.
 * ★ qpair owner 스레드와 동일 스레드에서 호출 필수.
 */
void spdk_nvme_ctrlr_disconnect_io_qpair(struct spdk_nvme_qpair *qpair);

/**
 * Attempt to reconnect the given qpair.
 *
 * This function is intended to be called on qpairs that have already been connected,
 * but have since entered a failed state as indicated by a return value of -ENXIO from
 * either spdk_nvme_qpair_process_completions or one of the spdk_nvme_ns_cmd_* functions.
 * This function must be called from the same thread as spdk_nvme_qpair_process_completions
 * and the spdk_nvme_ns_cmd_* functions.
 *
 * Calling this function has the same effect as calling spdk_nvme_ctrlr_disconnect_io_qpair
 * followed by spdk_nvme_ctrlr_connect_io_qpair.
 *
 * This function may be called on newly created qpairs, but it does extra checks and attempts
 * to disconnect the qpair before connecting it. The recommended API for newly created qpairs
 * is spdk_nvme_ctrlr_connect_io_qpair.
 *
 * \param qpair The qpair to reconnect.
 *
 * \return 0 on success, or if the qpair was already connected.
 * -EAGAIN if the driver was unable to reconnect during this call,
 * but the controller is still connected and is either resetting or enabled.
 * -ENODEV if the controller is removed. In this case, the controller cannot be recovered
 * and the application will have to destroy it and the associated qpairs.
 * -ENXIO if the controller is in a failed state but is not yet resetting. In this case,
 * the application should call spdk_nvme_ctrlr_reset to reset the entire controller.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_reconnect_io_qpair - 단절된 qpair를 재연결 시도.
 *
 * @return: 0=성공/이미 연결, -EAGAIN=재시도 권장(컨트롤러 reset 중), -ENODEV=핫리무브 복구 불가,
 *          -ENXIO=ctrlr가 failed but not resetting (사용자가 ctrlr_reset 호출 필요).
 *
 * disconnect+connect의 합. 이미 정상 연결된 qpair는 추가 검사+disconnect 후 재연결 시도하므로
 * 신규 qpair는 connect_io_qpair 권장. 호출 컨텍스트는 qpair owner 스레드.
 */
int spdk_nvme_ctrlr_reconnect_io_qpair(struct spdk_nvme_qpair *qpair);

/**
 * Opaque extended event handler options.
 */
struct spdk_event_handler_opts;
/* [한국어] spdk/env.h 의 인터럽트 모드 fd 등록 옵션 확장체 (불투명 전방 선언).
 * spdk_nvme_qpair_get_fd / get_admin_qp_fd 의 [out] 인자에 사용. */

/**
 * Get file descriptor for the admin queue pair of a controller.
 *
 * Applications that enable interrupts for completion notification will register and unregister
 * interrupt event source on its queue pair file descriptor. This function returns file descriptor
 * of the admin queue pair.
 * This function also allows the transport layer to fill out event handler opts required by the
 * application during interrupt registration phase.
 *
 * \param ctrlr Controller for which fd has to be fetched.
 * \param[out] opts Event handler options to be filled by the transport, or NULL.
 *
 * \return a valid fd on success, with opts filled out if specified.
 * -ENOTSUP if transport does not support fetching fd for this controller.
 * -EINVAL if opts is specified, but its size is incorrect.
 * -EINVAL if fds are not reserved, -1 if interrupts are not enabled for this controller.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_admin_qp_fd - admin qpair의 fd를 반환 (인터럽트 모드용).
 *
 * @opts: [out] (NULL 허용) 트랜스포트가 채울 epoll 등록 옵션.
 * @return: fd>=0 성공, -ENOTSUP/-EINVAL/-1.
 *
 * spdk_thread_get_interrupt_fd_group()에 합성하여 SPDK reactor의 epoll 통합 인터럽트 처리에 사용.
 * polled-mode가 아닌 인터럽트 모드 사용 시 ctrlr_opts.enable_interrupts=true 필수.
 */
int spdk_nvme_ctrlr_get_admin_qp_fd(struct spdk_nvme_ctrlr *ctrlr,
				    struct spdk_event_handler_opts *opts);

/**
 * Returns the reason the admin qpair for a given controller is disconnected.
 *
 * \param ctrlr The controller to check.
 *
 * \return a valid spdk_nvme_qp_failure_reason.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_admin_qp_failure_reason - admin qpair의 단절 원인 enum 반환.
 * fabrics 환경에서 keep-alive timeout 등 admin 단절 원인을 진단할 때 사용.
 */
spdk_nvme_qp_failure_reason spdk_nvme_ctrlr_get_admin_qp_failure_reason(
	struct spdk_nvme_ctrlr *ctrlr);

/**
 * Free an I/O queue pair that was allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 *
 * The qpair must not be accessed after calling this function.
 *
 * \param qpair I/O queue pair to free.
 *
 * \return 0 on success.  This function will never return any value other than 0.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_free_io_qpair - qpair 해제 (Delete I/O SQ + CQ admin 명령 + 메모리 free).
 *
 * 호출 후 qpair invalid - 어떤 호출도 금지. outstanding I/O가 있으면 abort 처리 후 free.
 * 항상 0 반환 (실패 케이스 없음). 항상 alloc 짝지어 호출.
 */
int spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair);

/**
 * Send the given NVM I/O command, I/O buffers, lists and all to the NVMe controller.
 *
 * This is a low level interface for submitting I/O commands directly.
 *
 * This function allows a caller to submit an I/O request that is
 * COMPLETELY pre-defined, right down to the "physical" memory buffers.
 * It is intended for testing hardware, specifying exact buffer location,
 * alignment, and offset.  It also allows for specific choice of PRP
 * and SGLs.
 *
 * The driver sets the CID.  EVERYTHING else is assumed set by the caller.
 * Needless to say, this is potentially extremely dangerous for both the host
 * (accidental/malicious storage usage/corruption), and the device.
 * Thus its intent is for very specific hardware testing and environment
 * reproduction.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * This function can only be used on PCIe controllers and qpairs.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param qpair I/O qpair to submit command.
 * \param cmd NVM I/O command to submit.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */

/*
 * [한국어]
 * spdk_nvme_ctrlr_io_cmd_raw_no_payload_build - 모든 필드 사용자 채움(PRP/SGL 포함) 저수준 raw 발행.
 *
 * 매우 위험 - 사용자가 PRP 리스트, 메타데이터 포인터, 모든 cdw를 직접 작성. 드라이버는 cid만 채움.
 * NVMe 호환성 테스트, 펌웨어 디버깅, 정확한 메모리 패턴 재현용. PCIe 전용. 일반 사용은 ns_cmd_* 권장.
 */
int spdk_nvme_ctrlr_io_cmd_raw_no_payload_build(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cmd *cmd,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Send the given NVM I/O command to the NVMe controller.
 *
 * This is a low level interface for submitting I/O commands directly. Prefer
 * the spdk_nvme_ns_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * When constructing the nvme_command it is not necessary to fill out the PRP
 * list/SGL or the CID. The driver will handle both of those for you.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param qpair I/O qpair to submit command.
 * \param cmd NVM I/O command to submit.
 * \param buf Virtual memory address of a single physically contiguous buffer.
 * \param len Size of buffer.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_io_raw - 저수준 raw I/O 명령 발행 (드라이버가 PRP/SGL/CID 자동 채움).
 *
 * @buf: 단일 물리 연속 버퍼 가상주소.
 * @len: 버퍼 byte 길이.
 *
 * cmd.opc/nsid/cdw10-15 등 명령 의미 부분은 사용자가 작성, 메모리 매핑(PRP)은 드라이버가 책임.
 * 검증 없이 발행 - 잘못된 nsid/opcode면 컨트롤러가 에러 반환. ns_cmd_* 사용 권장.
 */
int spdk_nvme_ctrlr_cmd_io_raw(struct spdk_nvme_ctrlr *ctrlr,
			       struct spdk_nvme_qpair *qpair,
			       struct spdk_nvme_cmd *cmd,
			       void *buf, uint32_t len,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Send the given NVM I/O command with metadata to the NVMe controller.
 *
 * This is a low level interface for submitting I/O commands directly. Prefer
 * the spdk_nvme_ns_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * When constructing the nvme_command it is not necessary to fill out the PRP
 * list/SGL or the CID. The driver will handle both of those for you.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param qpair I/O qpair to submit command.
 * \param cmd NVM I/O command to submit.
 * \param buf Virtual memory address of a single physically contiguous buffer.
 * \param len Size of buffer.
 * \param md_buf Virtual memory address of a single physically contiguous metadata
 * buffer.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_io_raw_with_md - cmd_io_raw + 별도 metadata 버퍼 변형.
 *
 * @md_buf: T10 DIF/DIX 메타데이터 단일 물리 연속 버퍼. ns의 metadata size 만큼.
 * extended LBA(LBA+MD interleaved) 미지원 ns에서 분리 metadata 모드 사용.
 */
int spdk_nvme_ctrlr_cmd_io_raw_with_md(struct spdk_nvme_ctrlr *ctrlr,
				       struct spdk_nvme_qpair *qpair,
				       struct spdk_nvme_cmd *cmd,
				       void *buf, uint32_t len, void *md_buf,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Restart the SGL walk to the specified offset when the command has scattered
 * payloads.
 *
 * \param cb_arg Argument passed to readv/writev.
 * \param offset Offset for SGL.
 */
/*
 * [한국어]
 * spdk_nvme_req_reset_sgl_cb - SGL 순회 재시작 콜백 (재시도/split 시 SPDK가 호출).
 *
 * @offset: 사용자 iovec[]의 누적 byte 위치를 이 값으로 되돌리라는 요청.
 *
 * SPDK는 큰 I/O를 split하거나 재시도할 때 사용자에게 "특정 offset부터 SGE를 다시 알려달라"고 요청한다.
 * 사용자는 자신의 iovec 배열 인덱스/내부 offset을 그 위치로 reset.
 */
typedef void (*spdk_nvme_req_reset_sgl_cb)(void *cb_arg, uint32_t offset);

/**
 * Fill out *address and *length with the current SGL entry and advance to the
 * next entry for the next time the callback is invoked.
 *
 * The described segment must be physically contiguous.
 *
 * \param cb_arg Argument passed to readv/writev.
 * \param address Virtual address of this segment, a value of UINT64_MAX
 * means the segment should be described via Bit Bucket SGL.
 * \param length Length of this physical segment.
 */
/*
 * [한국어]
 * spdk_nvme_req_next_sge_cb - 다음 SGE 정보를 사용자에게 묻는 콜백.
 *
 * @address: [out] 현재 segment 가상주소. UINT64_MAX=Bit Bucket SGL(컨트롤러가 데이터 버림).
 * @length: [out] 이 segment의 byte 길이. 물리 연속이어야 함.
 *
 * SPDK가 I/O 발행 중 SGL 빌드를 위해 반복 호출. iovec 기반 readv/writev 인자에 등록.
 * 호출자가 next 호출을 위해 자체 인덱스 advance 필요.
 */
typedef int (*spdk_nvme_req_next_sge_cb)(void *cb_arg, void **address,
		uint32_t *length);

/**
 * Send the given NVM I/O command with metadata to the NVMe controller.
 *
 * This is a low level interface for submitting I/O commands directly. Prefer
 * the spdk_nvme_ns_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * The command is submitted to a qpair allocated by  spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param qpair I/O qpair to submit command.
 * \param cmd NVM I/O command to submit.
 * \param len Size of buffer.
 * \param md_buf Virtual memory address of a single physically contiguous metadata buffer.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 * \param reset_sgl_fn Callback function to reset scattered payload.
 * \param next_sge_fn Callback function to iterate each scattered payload memory segment.
 *
 * \return 0 if successfully submitted, negated errnos on the following error
 conditions:
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_iov_raw_with_md - raw cmd + iovec(SGL) + metadata 변형.
 *
 * @reset_sgl_fn / next_sge_fn: SGL 빌드용 콜백. SPDK가 split/재시도 시 사용.
 * 단일 buf 대신 iovec scatter-gather 페이로드 + metadata 분리 처리. 가장 일반적인 raw 변형.
 */
int spdk_nvme_ctrlr_cmd_iov_raw_with_md(struct spdk_nvme_ctrlr *ctrlr,
					struct spdk_nvme_qpair *qpair,
					struct spdk_nvme_cmd *cmd, uint32_t len,
					void *md_buf, spdk_nvme_cmd_cb cb_fn,
					void *cb_arg,
					spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
					spdk_nvme_req_next_sge_cb next_sge_fn);

/**
 * Get file descriptor for an I/O queue pair.
 *
 * Applications that enable interrupts for completion notification will register and unregister
 * interrupt event source on its queue pair file descriptor. This function returns file descriptor
 * for an I/O queue pair.
 * This function also allows the transport layer to fill out event handler opts required by the
 * application during interrupt registration phase.
 *
 * \param qpair Opaque handle of the queue pair for which fd has to be fetched.
 * \param[out] opts Opaque event handler options to be filled by the transport, or NULL.
 *
 * \return a valid fd on success, with opts filled out if specified
 * -ENOTSUP if transport does not support fetching fd for the queue pair.
 * -EINVAL if opts is specified but its size is incorrect.
 * -EINVAL if fds are not reserved, -1 if interrupts are not enabled for the qpair controller.
 */
/*
 * [한국어]
 * spdk_nvme_qpair_get_fd - I/O qpair fd 반환 (인터럽트 모드용).
 *
 * @opts: [out] 트랜스포트별 epoll 등록 옵션 (NULL 허용).
 * @return: fd>=0, -ENOTSUP/-EINVAL.
 *
 * spdk_thread_get_interrupt_fd_group()에 추가하여 reactor의 epoll에 통합. polled-mode와 인터럽트
 * 모드 혼합 사용에 핵심.
 */
int spdk_nvme_qpair_get_fd(struct spdk_nvme_qpair *qpair,
			   struct spdk_event_handler_opts *opts);

/**
 * Process any outstanding completions for I/O submitted on a queue pair.
 *
 * This call is non-blocking, i.e. it only processes completions that are ready
 * at the time of this function call. It does not wait for outstanding commands
 * to finish.
 *
 * For each completed command, the request's callback function will be called if
 * specified as non-NULL when the request was submitted.
 *
 * The caller must ensure that each queue pair is only used from one thread at a
 * time.
 *
 * This function may be called at any point while the controller is attached to
 * the SPDK NVMe driver.
 *
 * \sa spdk_nvme_cmd_cb
 *
 * \param qpair Queue pair to check for completions.
 * \param max_completions Limit the number of completions to be processed in one
 * call, or 0 for unlimited.
 *
 * \return number of completions processed (may be 0) or negated on error. -ENXIO
 * in the special case that the qpair is failed at the transport layer.
 */
/*
 * [한국어]
 * spdk_nvme_qpair_process_completions - ★ polled-mode 핵심 함수: CQ 폴링 + 사용자 cb_fn 호출.
 *
 * @max_completions: 한 호출에서 처리할 최대 CQE 수, 0=무제한.
 * @return: 처리한 CQE 수(0 가능), -ENXIO=qpair가 transport 단절.
 *
 * 동작: CQ head에서 phase bit 일치 entry pull → cid로 발행시 등록한 cb_fn 룩업 → cb_fn(ctx, cpl) 호출 →
 * CQ head doorbell 갱신. 비블로킹 - 즉시 가용한 CQE만 처리.
 *
 * ★ 호출 컨텍스트: qpair owner SPDK 스레드 전용. SPDK reactor의 poller가 매 라운드 호출하는 것이
 * 표준 패턴. delay_cmd_submit=true 면 이 함수가 SQE batch도 발행 (doorbell write).
 *
 * 호출 체인: spdk_thread_poll → 사용자 poller → qpair_process_completions → cb_fn → 사용자 코드.
 */
int32_t spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair,
		uint32_t max_completions);

/**
 * Returns the reason the qpair is disconnected.
 *
 * \param qpair The qpair to check.
 *
 * \return a valid spdk_nvme_qp_failure_reason.
 */
/*
 * [한국어]
 * spdk_nvme_qpair_get_failure_reason - 단절된 qpair의 원인 반환 (NONE이면 정상).
 * 멀티패스/재연결 정책 결정에 사용.
 */
spdk_nvme_qp_failure_reason spdk_nvme_qpair_get_failure_reason(struct spdk_nvme_qpair *qpair);

/**
 * Control if DNR is set or not for aborted commands.
 *
 * The default value is false.
 *
 * \param qpair The qpair to set.
 * \param dnr Set the DNR bit to 1 if true or 0 if false for aborted commands.
 */
/*
 * [한국어]
 * spdk_nvme_qpair_set_abort_dnr - abort 처리 명령의 cpl.status DNR(Do Not Retry) 비트 제어.
 *
 * @dnr: true=DNR=1(상위 계층에 retry 금지 통지), false=DNR=0(retry 허용).
 *
 * SPDK가 자체 abort 처리할 때(예: ctrlr_fail 후 정리)의 정책. bdev_nvme 같은 상위가 DNR 보고
 * 더 이상 재시도 안 할지 결정.
 */
void spdk_nvme_qpair_set_abort_dnr(struct spdk_nvme_qpair *qpair, bool dnr);

/**
 * Return the connection status of a given qpair.
 *
 * \param qpair The qpair to check.
 *
 * \return true if the qpair is connected, or false otherwise.
 */
/*
 * [한국어]
 * spdk_nvme_qpair_is_connected - qpair connect 상태 단순 검사. 멀티패스 분기에 사용.
 */
bool spdk_nvme_qpair_is_connected(struct spdk_nvme_qpair *qpair);

/*
 * [한국어]
 * spdk_nvme_authenticate_cb - DH-HMAC-CHAP 인증 완료 콜백.
 * @status: 0=성공, 음수 errno=실패. NVMe 2.0 in-band authentication 결과.
 */
typedef void (*spdk_nvme_authenticate_cb)(void *ctx, int status);

/**
 * Force a qpair to authenticate.  As part of initialization, qpairs are authenticated automatically
 * if the controller is configured with DH-HMAC-CHAP keys.  However, this function can be used to
 * force authentication after a connection has already been established.
 *
 * This function doesn't disconnect the qpair if the authentication is successful.
 *
 * \param qpair The qpair to authenticate.
 * \param cb_fn Callback to be executed after the authentication is done.
 * \param cb_ctx Context passed to `cb_fn`.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_qpair_authenticate - I/O qpair 강제 재인증 (NVMe 2.0 in-band auth).
 *
 * 보통 connect 시 자동 인증되지만 set_keys 후 수동 재인증 트리거. 인증 성공 시 단절 안 함.
 * 비동기 - cb_fn으로 결과 통지.
 */
int spdk_nvme_qpair_authenticate(struct spdk_nvme_qpair *qpair,
				 spdk_nvme_authenticate_cb cb_fn, void *cb_ctx);

/**
 * Force authentication on the admin qpair of a controller.  As part of initialization, the admin
 * qpair is authenticated automatically if the controller is configured with DH-HMAC-CHAP keys.
 * However, this function can be used to force authentication after a connection has already been
 * established.
 *
 * This function doesn't disconnect the admin qpair if the authentication is successful.
 *
 * \param ctrlr Controller to authenticate.
 * \param cb_fn Callback to be executed after the authentication is done.
 * \param cb_ctx Context passed to `cb_fn`.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_authenticate - admin qpair 강제 재인증. set_keys 후 새 키로 재인증 시 사용.
 */
int spdk_nvme_ctrlr_authenticate(struct spdk_nvme_ctrlr *ctrlr,
				 spdk_nvme_authenticate_cb cb_fn, void *cb_ctx);

/**
 * Send the given admin command to the NVMe controller.
 *
 * This is a low level interface for submitting admin commands directly. Prefer
 * the spdk_nvme_ctrlr_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * When constructing the nvme_command it is not necessary to fill out the PRP
 * list/SGL or the CID. The driver will handle both of those for you.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param cmd NVM admin command to submit.
 * \param buf Virtual memory address of a single physically contiguous buffer.
 * \param len Size of buffer.
 * \param cb_fn Callback function invoked when the admin command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be
 * allocated for this request, -ENXIO if the admin qpair is failed at the transport layer.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_admin_raw - 저수준 admin 명령 raw 발행. ns_cmd_*_admin 변형의 admin queue 버전.
 *
 * 사용자가 cmd.opc/cdw10-15 등 직접 작성, PRP/CID는 SPDK가 채움. 검증 없이 발행 - vendor-specific
 * admin 명령이나 신규 spec 명령 시험에 사용. 일반 사용은 spdk_nvme_ctrlr_cmd_* 헬퍼 권장.
 */
int spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
				  struct spdk_nvme_cmd *cmd,
				  void *buf, uint32_t len,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Check if the controller supports NSSR
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return true if the controller supports NSSR, false otherwise
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_is_nssr_supported - NSSR(NVM Subsystem Reset) 지원 여부 (CAP.NSSRS 비트).
 * spdk_nvme_ctrlr_reset_subsystem 호출 가능 검사용.
 */
bool spdk_nvme_ctrlr_is_nssr_supported(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Process any outstanding completions for admin commands.
 *
 * This will process completions for admin commands submitted on any thread.
 *
 * This call is non-blocking, i.e. it only processes completions that are ready
 * at the time of this function call. It does not wait for outstanding commands
 * to finish.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return number of completions processed (may be 0) or negated on error. -ENXIO
 * in the special case that the qpair is failed at the transport layer.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_process_admin_completions - admin queue 폴링 (I/O qpair용 process_completions의
 * admin 버전).
 *
 * @return: 처리한 admin CQE 수, 또는 -ENXIO admin qpair fail.
 *
 * Identify/Get Log Page/Set Features/Format/FW Download/Security Send/Receive 등 admin 명령의 완료를
 * 폴링. SPDK가 자체적으로 keep-alive와 AER을 admin queue에 띄워두므로 사용자가 명시적 admin 명령을
 * 발행하지 않더라도 주기적 호출 권장 (보통 100ms 주기 poller로 등록). 호출 컨텍스트: 컨트롤러 owner 스레드.
 */
int32_t spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr);


/**
 * Opaque handle to a namespace. Obtained by calling spdk_nvme_ctrlr_get_ns().
 */
/*
 * [한국어]
 * struct spdk_nvme_ns - 불투명 NVMe namespace 핸들.
 *
 * 한 컨트롤러는 다수 ns를 가질 수 있고 각 ns는 독립 LBA 주소공간/크기/PI 모드를 가진다.
 * 정의는 lib/nvme/nvme_internal.h. spdk_nvme_ns_cmd_*의 첫 인자로 사용.
 * ★ ctrlr_reset 호출 시 무효화 - 재조회 필요.
 */
struct spdk_nvme_ns;

/**
 * Get a handle to a namespace for the given controller.
 *
 * Namespaces are numbered from 1 to the total number of namespaces. There will
 * never be any gaps in the numbering. The number of namespaces is obtained by
 * calling spdk_nvme_ctrlr_get_num_ns().
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param ns_id Namespace id.
 *
 * \return a pointer to the namespace.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_ns - nsid로 namespace 핸들 조회.
 *
 * @ns_id: 1..NN 범위. 0은 무효, NN+1 이상은 NULL 반환.
 *
 * SPDK가 attach 시 SPDK_NVME_GLOBAL_NS_TAG로 enumerate한 ns 캐시에서 반환. inactive ns에 대해서도
 * 핸들 자체는 반환되며 ns_is_active() 별도 검사 필요. ★ ctrlr_reset 시 invalidate.
 */
struct spdk_nvme_ns *spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t ns_id);

/**
 * Get a specific log page from the NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_is_log_page_supported()
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param log_page The log page identifier.
 * \param nsid Depending on the log page, this may be 0, a namespace identifier,
 * or SPDK_NVME_GLOBAL_NS_TAG.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param offset Offset in bytes within the log page to start retrieving log page
 * data. May only be non-zero if the controller supports extended data for Get Log
 * Page as reported in the controller data log page attributes.
 * \param cb_fn Callback function to invoke when the log page has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be
 * allocated for this request, -ENXIO if the admin qpair is failed at the transport layer.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_get_log_page - NVMe Get Log Page admin 명령 발행 (opcode 0x02).
 *
 * @log_page: LID(0x01..0xFF). SMART(0x02), FW Slot(0x03), Error Info(0x01), Sanitize(0x81) 등.
 * @nsid: 0 또는 SPDK_NVME_GLOBAL_NS_TAG(0xFFFFFFFF) 또는 ns별.
 * @payload: 결과 받을 버퍼 (DMA 가능 메모리, vtophys로 매핑됨).
 * @offset: extended log data 지원 컨트롤러에서만 비-0 가능.
 *
 * 비동기 - cb_fn으로 완료 통지. process_admin_completions() 폴링 필요. spec 섹션 5.16.
 */
int spdk_nvme_ctrlr_cmd_get_log_page(struct spdk_nvme_ctrlr *ctrlr,
				     uint8_t log_page, uint32_t nsid,
				     void *payload, uint32_t payload_size,
				     uint64_t offset,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Get a specific log page from the NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * This function allows specifying extra fields in cdw10 and cdw11 such as
 * Retain Asynchronous Event and Log Specific Field.
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_is_log_page_supported()
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param log_page The log page identifier.
 * \param nsid Depending on the log page, this may be 0, a namespace identifier,
 * or SPDK_NVME_GLOBAL_NS_TAG.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param offset Offset in bytes within the log page to start retrieving log page
 * data. May only be non-zero if the controller supports extended data for Get Log
 * Page as reported in the controller data log page attributes.
 * \param cdw10 Value to specify for cdw10.  Specify 0 for numdl - it will be
 * set by this function based on the payload_size parameter.  Specify 0 for lid -
 * it will be set by this function based on the log_page parameter.
 * \param cdw11 Value to specify for cdw11.  Specify 0 for numdu - it will be
 * set by this function based on the payload_size.
 * \param cdw14 Value to specify for cdw14.
 * \param cb_fn Callback function to invoke when the log page has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be
 * allocated for this request, -ENXIO if the admin qpair is failed at the transport layer.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_get_log_page_ext - get_log_page 확장 변형: cdw10/11/14 사용자 지정.
 *
 * RAE(Retain Asynchronous Event), LSF(Log Specific Field) 같은 비트를 사용자가 직접 셋팅.
 * SPDK는 numdl/numdu(데이터 길이)와 lid는 자동 계산하여 사용자 cdw10/11에 OR. 고급 사용자용.
 */
int spdk_nvme_ctrlr_cmd_get_log_page_ext(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page,
		uint32_t nsid, void *payload, uint32_t payload_size,
		uint64_t offset, uint32_t cdw10, uint32_t cdw11,
		uint32_t cdw14, spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Abort a specific previously-submitted NVMe command.
 *
 * \sa spdk_nvme_ctrlr_register_timeout_callback()
 *
 * \param ctrlr NVMe controller to which the command was submitted.
 * \param qpair NVMe queue pair to which the command was submitted. For admin
 *  commands, pass NULL for the qpair.
 * \param cid Command ID of the command to abort.
 * \param cb_fn Callback function to invoke when the abort has completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be
 * allocated for this request, -ENXIO if the admin qpair is failed at the transport layer.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_abort - 특정 CID의 outstanding 명령 abort 시도 (opcode 0x08).
 *
 * @qpair: 대상 qpair, admin 명령 abort면 NULL.
 * @cid: SQE의 16bit Command ID. 사용자가 발행한 명령에 SPDK가 자동 부여한 cid.
 *
 * 베스트-에포트 - 컨트롤러가 이미 처리 중이면 abort 실패. 결과는 이 abort 명령의 cb_fn에서.
 * 원래 명령의 cb_fn은 abort 성공 시 SPDK_NVME_SC_ABORTED_BY_REQUEST 상태로 호출됨.
 */
int spdk_nvme_ctrlr_cmd_abort(struct spdk_nvme_ctrlr *ctrlr,
			      struct spdk_nvme_qpair *qpair,
			      uint16_t cid,
			      spdk_nvme_cmd_cb cb_fn,
			      void *cb_arg);

/**
 * Abort previously submitted commands which have cmd_cb_arg as its callback argument.
 *
 * \param ctrlr NVMe controller to which the commands were submitted.
 * \param qpair NVMe queue pair to which the commands were submitted. For admin
 * commands, pass NULL for the qpair.
 * \param cmd_cb_arg Callback argument for the NVMe commands which this function
 * attempts to abort.
 * \param cb_fn Callback function to invoke when this function has completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno otherwise.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_abort_ext - 콜백 인자(cb_arg) 일치 명령 일괄 abort.
 *
 * cid 대신 명령 발행 시 사용자가 넘긴 cb_arg 포인터로 매칭. 한 사용자 컨텍스트가 발행한 모든 명령을
 * 한 번에 정리할 때 유용. 매칭된 각 명령마다 abort 시도 후 통합 cb_fn 호출.
 */
int spdk_nvme_ctrlr_cmd_abort_ext(struct spdk_nvme_ctrlr *ctrlr,
				  struct spdk_nvme_qpair *qpair,
				  void *cmd_cb_arg,
				  spdk_nvme_cmd_cb cb_fn,
				  void *cb_arg);

/**
 * Set specific feature for the given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_cmd_get_feature().
 *
 * \param ctrlr NVMe controller to manipulate.
 * \param feature The feature identifier.
 * \param cdw11 as defined by the specification for this command.
 * \param cdw12 as defined by the specification for this command.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the feature has been set.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be
 * allocated for this request, -ENXIO if the admin qpair is failed at the transport layer.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_set_feature - Set Features admin 명령 (opcode 0x09).
 *
 * @feature: FID(0x01..0xFF) - Arbitration/Power Mgmt/Temperature/Number of Queues/Async Event Config 등.
 * @cdw11/cdw12: spec에서 정의한 feature별 값.
 * 비동기. 일부 feature는 payload(예: Host Identifier 0x81의 경우 16B)에 추가 데이터 필요.
 */
int spdk_nvme_ctrlr_cmd_set_feature(struct spdk_nvme_ctrlr *ctrlr,
				    uint8_t feature, uint32_t cdw11, uint32_t cdw12,
				    void *payload, uint32_t payload_size,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Get specific feature from given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_cmd_set_feature()
 *
 * \param ctrlr NVMe controller to query.
 * \param feature The feature identifier.
 * \param cdw11 as defined by the specification for this command.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the feature has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, -ENOMEM if resources could not be allocated
 * for this request, -ENXIO if the admin qpair is failed at the transport layer.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_get_feature - Get Features admin 명령 (opcode 0x0A).
 *
 * 결과는 cpl.cdw0(scalar feature)와 payload(structured feature) 두 곳. SEL 비트(cdw11)로
 * Current/Default/Saved/Supported Capabilities 중 선택. 비동기.
 */
int spdk_nvme_ctrlr_cmd_get_feature(struct spdk_nvme_ctrlr *ctrlr,
				    uint8_t feature, uint32_t cdw11,
				    void *payload, uint32_t payload_size,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Get specific feature from given NVMe controller.
 *
 * \param ctrlr NVMe controller to query.
 * \param feature The feature identifier.
 * \param cdw11 as defined by the specification for this command.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the feature has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 * \param ns_id The namespace identifier.
 *
 * \return 0 if successfully submitted, -ENOMEM if resources could not be allocated
 * for this request, -ENXIO if the admin qpair is failed at the transport layer.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * Call \ref spdk_nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_cmd_set_feature_ns()
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_get_feature_ns - get_feature의 ns 단위 변형. cdw0/cdw1 nsid 채움.
 * 일부 feature(LBA Range Type 등)는 ns 컨텍스트 필요.
 */
int spdk_nvme_ctrlr_cmd_get_feature_ns(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				       uint32_t cdw11, void *payload, uint32_t payload_size,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t ns_id);

/**
 * Set specific feature for the given NVMe controller and namespace ID.
 *
 * \param ctrlr NVMe controller to manipulate.
 * \param feature The feature identifier.
 * \param cdw11 as defined by the specification for this command.
 * \param cdw12 as defined by the specification for this command.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the feature has been set.
 * \param cb_arg Argument to pass to the callback function.
 * \param ns_id The namespace identifier.
 *
 * \return 0 if successfully submitted, -ENOMEM if resources could not be allocated
 * for this request, -ENXIO if the admin qpair is failed at the transport layer.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * Call \ref spdk_nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_cmd_get_feature_ns()
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_set_feature_ns - set_feature의 ns 단위 변형.
 */
int spdk_nvme_ctrlr_cmd_set_feature_ns(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				       uint32_t cdw11, uint32_t cdw12, void *payload,
				       uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
				       void *cb_arg, uint32_t ns_id);

/**
 * Receive security protocol data from controller.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to use for security receive command submission.
 * \param secp Security Protocol that is used.
 * \param spsp Security Protocol Specific field.
 * \param nssf NVMe Security Specific field. Indicate RPMB target when using Security
 * Protocol EAh.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the command has been completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be allocated
 * for this request.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_security_receive - Security Receive admin 명령 (opcode 0x82, 비동기).
 *
 * @secp: Security Protocol ID (0x01=TCG Opal, 0xEA=NVMe RPMB).
 * @spsp: Protocol-specific 16bit param.
 * @nssf: NVMe-specific (RPMB target).
 *
 * 사용처: TCG Opal lib/opal, RPMB(Replay Protected Memory Block). 비동기 + cb_fn.
 */
int spdk_nvme_ctrlr_cmd_security_receive(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
		uint16_t spsp, uint8_t nssf, void *payload,
		uint32_t payload_size,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Send security protocol data to controller.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to use for security send command submission.
 * \param secp Security Protocol that is used.
 * \param spsp Security Protocol Specific field.
 * \param nssf NVMe Security Specific field. Indicate RPMB target when using Security
 * Protocol EAh.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the command has been completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be allocated
 * for this request.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_security_send - Security Send admin 명령 (opcode 0x81, 비동기).
 *
 * 동작은 _receive와 대칭. TCG Opal session 시작/lock/unlock/key 명령 운반.
 */
int spdk_nvme_ctrlr_cmd_security_send(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
				      uint16_t spsp, uint8_t nssf, void *payload,
				      uint32_t payload_size, spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Receive security protocol data from controller.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to use for security receive command submission.
 * \param secp Security Protocol that is used.
 * \param spsp Security Protocol Specific field.
 * \param nssf NVMe Security Specific field. Indicate RPMB target when using Security
 * Protocol EAh.
 * \param payload The pointer to the payload buffer.
 * \param size The size of payload buffer.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be allocated
 * for this request.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_security_receive - 동기 wrapper. cmd_security_receive 발행 후 완료까지 polling.
 * 콜백 없는 단순 호출 - 사용자 코드 단순화. lib/opal에서 사용.
 */
int spdk_nvme_ctrlr_security_receive(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
				     uint16_t spsp, uint8_t nssf, void *payload, size_t size);

/**
 * Send security protocol data to controller.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to use for security send command submission.
 * \param secp Security Protocol that is used.
 * \param spsp Security Protocol Specific field.
 * \param nssf NVMe Security Specific field. Indicate RPMB target when using Security
 * Protocol EAh.
 * \param payload The pointer to the payload buffer.
 * \param size The size of payload buffer.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be allocated
 * for this request.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_security_send - 동기 wrapper. lib/opal/opal.c가 모든 Opal 호출에 사용.
 */
int spdk_nvme_ctrlr_security_send(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
				  uint16_t spsp, uint8_t nssf, void *payload, size_t size);

/**
 * Receive data related to a specific Directive Type from the controller.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \param ctrlr NVMe controller to use for directive receive command submission.
 * \param nsid Specific Namespace Identifier.
 * \param doper Directive Operation defined in nvme_spec.h.
 * \param dtype Directive Type defined in nvme_spec.h.
 * \param dspec Directive Specific defined in nvme_spec.h.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cdw12 Command dword 12.
 * \param cdw13 Command dword 13.
 * \param cb_fn Callback function to invoke when the command has been completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be allocated
 * for this request.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_directive_receive - Directive Receive admin 명령 (opcode 0x1A).
 *
 * @doper: Directive Operation. @dtype: Identify(0x00)/Streams(0x01) 등. @dspec: dtype별 추가 인자.
 * Streams Directive는 host hint를 컨트롤러에 전달해 SSD GC 효율 개선.
 */
int spdk_nvme_ctrlr_cmd_directive_receive(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
		uint32_t doper, uint32_t dtype, uint32_t dspec,
		void *payload, uint32_t payload_size, uint32_t cdw12,
		uint32_t cdw13, spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Send data related to a specific Directive Type to the controller.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \param ctrlr NVMe controller to use for directive send command submission.
 * \param nsid Specific Namespace Identifier.
 * \param doper Directive Operation defined in nvme_spec.h.
 * \param dtype Directive Type defined in nvme_spec.h.
 * \param dspec Directive Specific defined in nvme_spec.h.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cdw12 Command dword 12.
 * \param cdw13 Command dword 13.
 * \param cb_fn Callback function to invoke when the command has been completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be allocated
 * for this request.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_directive_send - Directive Send admin 명령 (opcode 0x19).
 *
 * Streams Directive enable, Identify directive list 등록 등. SPDK_NVME_CTRLR_DIRECTIVES_SUPPORTED 검사.
 */
int spdk_nvme_ctrlr_cmd_directive_send(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				       uint32_t doper, uint32_t dtype, uint32_t dspec,
				       void *payload, uint32_t payload_size, uint32_t cdw12,
				       uint32_t cdw13, spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Get supported flags of the controller.
 *
 * \param ctrlr NVMe controller to get flags.
 *
 * \return supported flags of this controller.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_flags - 컨트롤러의 능력 비트마스크 (enum spdk_nvme_ctrlr_flags 조합).
 * SGL/SECURITY/WRR/COMPARE_AND_WRITE/ZONE_APPEND/DIRECTIVES/MPTR_SGL/ACCEL_SEQUENCE 지원 여부.
 */
uint64_t spdk_nvme_ctrlr_get_flags(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Attach the specified namespace to controllers.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to use for command submission.
 * \param nsid Namespace identifier for namespace to attach.
 * \param payload The pointer to the controller list.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated
 * for this request.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_attach_ns - Namespace Attachment admin 명령 (opcode 0x15) - 컨트롤러에 ns 부착.
 *
 * @payload: SPDK_NVME_CTRLR_LIST 형식 (16bit count + 2047 controller IDs). 부착할 컨트롤러 명단.
 * NVMe Multi-Path/Multi-Tenant 환경에서 한 ns를 여러 ctrlr에 공유시킬 때 사용.
 */
int spdk_nvme_ctrlr_attach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			      struct spdk_nvme_ctrlr_list *payload);

/**
 * Detach the specified namespace from controllers.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to use for command submission.
 * \param nsid Namespace ID to detach.
 * \param payload The pointer to the controller list.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated
 * for this request
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_detach_ns - ns를 컨트롤러 리스트에서 detach.
 * 양방향 연결 해제 - 다른 controller가 같은 ns를 보유 중이면 그쪽 ctrlr는 영향 없음.
 */
int spdk_nvme_ctrlr_detach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			      struct spdk_nvme_ctrlr_list *payload);

/**
 * Create a namespace.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to create namespace on.
 * \param payload The pointer to the NVMe namespace data.
 *
 * \return Namespace ID (>= 1) if successfully created, or 0 if the request failed.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_create_ns - Namespace Management Create (opcode 0x0D, SEL=0).
 *
 * @payload: 새 ns의 spdk_nvme_ns_data (size, capacity, LBA format, NMIC 등).
 * @return: 새 nsid (>=1) 또는 0 실패.
 *
 * SSD가 ns 동적 생성 지원할 때만 (identify ctrlr.OACS bit 3). 동기 호출.
 */
uint32_t spdk_nvme_ctrlr_create_ns(struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_nvme_ns_data *payload);

/**
 * Delete a namespace.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to delete namespace from.
 * \param nsid The namespace identifier.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be
 * allocated
 * for this request
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_delete_ns - Namespace Management Delete (opcode 0x0D, SEL=1).
 * ns 영구 삭제 - 데이터도 함께 폐기. 동기 호출.
 */
int spdk_nvme_ctrlr_delete_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid);

/**
 * Format NVM.
 *
 * This function requests a low-level format of the media.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to format.
 * \param nsid The namespace identifier. May be SPDK_NVME_GLOBAL_NS_TAG to format
 * all namespaces.
 * \param format The format information for the command.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be
 * allocated for this request
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_format - Format NVM admin 명령 (opcode 0x80) - low-level 포맷.
 *
 * @nsid: 특정 ns 또는 SPDK_NVME_GLOBAL_NS_TAG(전체).
 * @format: LBAF(LBA format index)/MS(metadata setting)/PI(protection info)/PIL/SES(secure erase) 정보.
 *
 * 디스크 모든 데이터 즉시 소실. 시간이 오래 걸림 - 동기 호출이지만 timeout 길게 설정 필요.
 */
int spdk_nvme_ctrlr_format(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			   struct spdk_nvme_format *format);

/**
 * Download a new firmware image.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to perform firmware operation on.
 * \param payload The data buffer for the firmware image.
 * \param size The data size will be downloaded.
 * \param slot The slot that the firmware image will be committed to.
 * \param commit_action The action to perform when firmware is committed.
 * \param completion_status output parameter. Contains the completion status of
 * the firmware commit operation.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated
 * for this request, -1 if the size is not multiple of 4.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_update_firmware - FW Image Download(opcode 0x11) + FW Commit(0x10) 통합 수행.
 *
 * @payload: FW 이미지 (4-byte aligned size).
 * @slot: commit 대상 슬롯 (1..7).
 * @commit_action: REPLACE/ACTIVATE_NEXT_RESET 등 NVMe spec 5.13.
 * @completion_status: [out] commit 결과 status code.
 *
 * 동기 - download chunks → commit → 결과 status 수신까지 블록. 큰 펌웨어는 분할 download 자동.
 */
int spdk_nvme_ctrlr_update_firmware(struct spdk_nvme_ctrlr *ctrlr, void *payload, uint32_t size,
				    int slot, enum spdk_nvme_fw_commit_action commit_action,
				    struct spdk_nvme_status *completion_status);

/**
 * Start the Read from a Boot Partition.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to perform the Boot Partition read.
 * \param payload The data buffer for Boot Partition read.
 * \param bprsz Read size in multiples of 4 KiB to copy into the Boot Partition Memory Buffer.
 * \param bprof Boot Partition offset to read from in 4 KiB units.
 * \param bpid Boot Partition identifier for the Boot Partition read operation.
 *
 * \return 0 if Boot Partition read is successful. Negated errno on the following error conditions:
 * -ENOMEM: if resources could not be allocated.
 * -ENOTSUP: Boot Partition is not supported by the Controller.
 * -EIO: Registers access failure.
 * -EINVAL: Parameters are invalid.
 * -EFAULT: Invalid address was specified as part of payload.
 * -EALREADY: Boot Partition read already initiated.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_read_boot_partition_start - Boot Partition 읽기 시작 (NVMe spec 7.7).
 *
 * @bprsz: 4KiB 단위 read size.
 * @bprof: 4KiB 단위 offset in boot partition.
 * @bpid: 0 또는 1 (boot partition ID).
 *
 * BPMBL 레지스터에 paddr write → BPRSEL.BPID/BPROF/BPRSZ → 비동기 시작. read_boot_partition_poll로 완료 확인.
 */
int spdk_nvme_ctrlr_read_boot_partition_start(struct spdk_nvme_ctrlr *ctrlr, void *payload,
		uint32_t bprsz, uint32_t bprof, uint32_t bpid);

/**
 * Poll the status of the Read from a Boot Partition.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to perform the Boot Partition read.
 *
 * \return 0 if Boot Partition read is successful. Negated errno on the following error conditions:
 * -EIO: Registers access failure.
 * -EINVAL: Invalid read status or the Boot Partition read is not initiated yet.
 * -EAGAIN: If the read is still in progress; users must call
 * spdk_nvme_ctrlr_read_boot_partition_poll again to check the read status.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_read_boot_partition_poll - Boot Partition read 진행 폴링.
 * BPINFO.BRS(Boot Read Status) 비트 확인. -EAGAIN=진행 중, 0=완료, 음수 errno=실패.
 */
int spdk_nvme_ctrlr_read_boot_partition_poll(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Write to a Boot Partition.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 * Users will get the completion after the data is downloaded, image is replaced and
 * Boot Partition is activated or when the sequence encounters an error.
 *
 * \param ctrlr NVMe controller to perform the Boot Partition write.
 * \param payload The data buffer for Boot Partition write.
 * \param size Data size to write to the Boot Partition.
 * \param bpid Boot Partition identifier for the Boot Partition write operation.
 * \param cb_fn Callback function to invoke when the operation is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if Boot Partition write submit is successful. Negated errno on the following error conditions:
 * -ENOMEM: if resources could not be allocated.
 * -ENOTSUP: Boot Partition is not supported by the Controller.
 * -EIO: Registers access failure.
 * -EINVAL: Parameters are invalid.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_write_boot_partition - Boot Partition 쓰기 (FW Image Download + Commit_BP).
 *
 * 비동기 - 데이터 download → boot image 교체 → activation 시퀀스 완료 시 cb_fn 호출.
 * 새 BIOS/펌웨어 업로드용. SSD가 OACS.boot_partition_supported(bit 13)일 때만.
 */
int spdk_nvme_ctrlr_write_boot_partition(struct spdk_nvme_ctrlr *ctrlr, void *payload,
		uint32_t size, uint32_t bpid, spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Return virtual address of PCIe NVM I/O registers
 *
 * This function returns a pointer to the PCIe I/O registers for a controller
 * or NULL if unsupported for this transport.
 *
 * \param ctrlr Controller whose registers are to be accessed.
 *
 * \return Pointer to virtual address of register bank, or NULL.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_registers - PCIe BAR 매핑된 NVMe register 영역 가상주소 반환.
 *
 * fabrics는 NULL. volatile 포인터 - MMIO read/write 시 컴파일러 최적화 차단. 직접 사용은 위험.
 * 보통 spdk_nvme_ctrlr_get_regs_*() 헬퍼로 추상화. 디버깅 도구가 raw access 필요할 때만 사용.
 */
volatile struct spdk_nvme_registers *spdk_nvme_ctrlr_get_registers(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Reserve the controller memory buffer for data transfer use.
 *
 * This function reserves the full size of the controller memory buffer
 * for use in data transfers. If submission queues or completion queues are
 * already placed in the controller memory buffer, this call will fail.
 *
 * \param ctrlr Controller from which to allocate memory buffer
 *
 * \return The size of the controller memory buffer on success. Negated errno
 * on failure.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_reserve_cmb - Controller Memory Buffer 전체 영역을 데이터 전송용으로 예약.
 *
 * @return: CMB 크기(byte), 음수 errno 실패. 이미 SQ/CQ가 CMB 점유 중이면 실패.
 *
 * 사용자 데이터 버퍼를 CMB에 할당하면 PCIe peer-to-peer 가능 (CPU 우회 데이터 이동).
 * GPUDirect Storage / xfer between two NVMe SSDs 등에 활용.
 */
int spdk_nvme_ctrlr_reserve_cmb(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Map a previously reserved controller memory buffer so that it's data is
 * visible from the CPU. This operation is not always possible.
 *
 * \param ctrlr Controller that contains the memory buffer
 * \param size Size of buffer that was mapped.
 *
 * \return Pointer to controller memory buffer, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_map_cmb - reserve된 CMB를 CPU 가시 메모리에 매핑.
 *
 * @size: [out] 매핑된 크기.
 * @return: 가상주소, 실패시 NULL. 일부 컨트롤러는 CMB CPU map 미지원.
 */
void *spdk_nvme_ctrlr_map_cmb(struct spdk_nvme_ctrlr *ctrlr, size_t *size);

/**
 * Free a controller memory I/O buffer.
 *
 * \param ctrlr Controller from which to unmap the memory buffer.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_unmap_cmb - map_cmb로 매핑한 CMB 영역 해제.
 */
void spdk_nvme_ctrlr_unmap_cmb(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Enable the Persistent Memory Region
 *
 * \param ctrlr Controller that contains the Persistent Memory Region
 *
 * \return 0 on success. Negated errno on the following error conditions:
 * -ENOTSUP: PMR is not supported by the Controller.
 * -EIO: Registers access failure.
 * -EINVAL: PMR Time Units Invalid or PMR is already enabled.
 * -ETIMEDOUT: Timed out to Enable PMR.
 * -ENOSYS: Transport does not support Enable PMR function.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_enable_pmr - Persistent Memory Region 활성화 (PMRCTL.EN=1 write).
 *
 * @return: 0 성공, -ENOTSUP/-EIO/-EINVAL/-ETIMEDOUT/-ENOSYS 등.
 *
 * PMR은 NVMe 1.4의 NVDIMM-like 영역 - 전원 잃어도 보존되는 디바이스 RAM. 사용자가 직접 store/load
 * 수행 가능 (NVMe 명령 거치지 않음). 트랜스포트가 PMR 미지원이면 -ENOSYS.
 */
int spdk_nvme_ctrlr_enable_pmr(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Disable the Persistent Memory Region
 *
 * \param ctrlr Controller that contains the Persistent Memory Region
 *
 * \return 0 on success. Negated errno on the following error conditions:
 * -ENOTSUP: PMR is not supported by the Controller.
 * -EIO: Registers access failure.
 * -EINVAL: PMR Time Units Invalid or PMR is already disabled.
 * -ETIMEDOUT: Timed out to Disable PMR.
 * -ENOSYS: Transport does not support Disable PMR function.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_disable_pmr - PMR 비활성화 (PMRCTL.EN=0). 모든 outstanding 사용 정리 후 호출.
 */
int spdk_nvme_ctrlr_disable_pmr(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Map the Persistent Memory Region so that it's data is
 * visible from the CPU.
 *
 * \param ctrlr Controller that contains the Persistent Memory Region
 * \param size Size of the region that was mapped.
 *
 * \return Pointer to Persistent Memory Region, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_map_pmr - PMR 영역을 CPU 가시 메모리에 매핑. NULL=실패.
 * 매핑된 영역에 일반 store/load - 자동 영구 저장. NVMe 명령 우회.
 */
void *spdk_nvme_ctrlr_map_pmr(struct spdk_nvme_ctrlr *ctrlr, size_t *size);

/**
 * Free the Persistent Memory Region.
 *
 * \param ctrlr Controller from which to unmap the Persistent Memory Region.
 *
 * \return 0 on success, negative errno on failure.
 * -ENXIO: Either PMR is not supported by the Controller or the PMR is already unmapped.
 * -ENOSYS: Transport does not support Unmap PMR function.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_unmap_pmr - PMR 매핑 해제.
 */
int spdk_nvme_ctrlr_unmap_pmr(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the transport ID for a given NVMe controller.
 *
 * \param ctrlr Controller to get the transport ID.
 * \return Pointer to the controller's transport ID.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_transport_id - 이 컨트롤러의 trid (const 포인터). attach 시 캐시값.
 * set_trid로 변경된 후에는 새 endpoint를 가리킴. 멀티패스 추적/디버깅 출력에 사용.
 */
const struct spdk_nvme_transport_id *spdk_nvme_ctrlr_get_transport_id(
	struct spdk_nvme_ctrlr *ctrlr);

/**
 * \brief Alloc NVMe I/O queue identifier.
 *
 * This function is only needed for the non-standard case of allocating queues using the raw
 * command interface. In most cases \ref spdk_nvme_ctrlr_alloc_io_qpair should be sufficient.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \return qid on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_alloc_qid - 사용 가능한 NVMe queue ID(QID) 할당.
 *
 * 일반 사용자는 alloc_io_qpair만 사용하면 충분. raw command interface로 직접 큐 생성하는 고급 사용자가
 * 사용할 명시 API. 1..num_io_queues 범위 사용 가능 QID 중 하나 반환.
 */
int32_t spdk_nvme_ctrlr_alloc_qid(struct spdk_nvme_ctrlr *ctrlr);

/**
 * \brief Free NVMe I/O queue identifier.
 *
 * This function must only be called with qids previously allocated with \ref spdk_nvme_ctrlr_alloc_qid.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param qid NVMe Queue Identifier.
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_free_qid - alloc_qid로 받은 QID 반납. 사용자 직접 큐 관리 시에만.
 */
void spdk_nvme_ctrlr_free_qid(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid);

/**
 * Opaque handle for a poll group. A poll group is a collection of spdk_nvme_qpair
 * objects that are polled for completions as a unit.
 *
 * Returned by spdk_nvme_poll_group_create().
 */
/*
 * [한국어]
 * struct spdk_nvme_poll_group - 다수 qpair을 한 단위로 묶어 폴링하는 불투명 핸들.
 *
 * SPDK 19.10+에서 도입된 개념. bdev_nvme 같은 사용자가 한 SPDK 스레드에 다수 qpair(여러 컨트롤러,
 * 여러 트랜스포트)를 배치할 때 효율적 폴링을 위함. RDMA의 shared CQ, TCP의 epoll 한 번 등 트랜스포트가
 * 자체 최적화로 group 단위 batching 가능.
 */
struct spdk_nvme_poll_group;


/**
 * This function alerts the user to disconnected qpairs when calling
 * spdk_nvme_poll_group_process_completions.
 */
/*
 * [한국어]
 * spdk_nvme_disconnected_qpair_cb - poll group 폴링 중 단절된 qpair 발견 시 호출 콜백.
 *
 * @qpair: 단절된 qpair. 사용자는 reconnect 또는 destroy 결정.
 * @poll_group_ctx: poll_group_create 시 등록한 ctx.
 *
 * process_completions/wait 호출마다 단절된 qpair 각각에 대해 호출 - 사용자가 NULL 넘기면 -EINVAL.
 */
typedef void (*spdk_nvme_disconnected_qpair_cb)(struct spdk_nvme_qpair *qpair,
		void *poll_group_ctx);

/**
 * Create a new poll group.
 *
 * \param ctx A user supplied context that can be retrieved later with spdk_nvme_poll_group_get_ctx
 * \param table The call back table defined by users which contains the accelerated functions
 * which can be used to accelerate some operations such as crc32c.
 *
 * \return Pointer to the new poll group, or NULL on error.
 */
/*
 * [한국어]
 * spdk_nvme_poll_group_create - 새 poll group 생성.
 *
 * @ctx: 사용자 컨텍스트 (poll_group_get_ctx로 회수).
 * @table: ★ accel offload 함수 테이블 (NULL 허용 - offload 비활성). bdev_nvme가 lib/accel과 연결.
 * @return: poll group 핸들, 실패 시 NULL.
 *
 * 호출자(보통 SPDK 스레드 단위)가 한 번씩 만들고 그 group에 qpair를 add. 한 group 한 thread.
 */
struct spdk_nvme_poll_group *spdk_nvme_poll_group_create(void *ctx,
		struct spdk_nvme_accel_fn_table *table);

/**
 * Add an spdk_nvme_qpair to a poll group. qpairs may only be added to
 * a poll group if they are in the disconnected state; i.e. either they were
 * just allocated and not yet connected or they have been disconnected with a call
 * to spdk_nvme_ctrlr_disconnect_io_qpair.
 *
 * \param group The group to which the qpair will be added.
 * \param qpair The qpair to add to the poll group.
 *
 * return 0 on success, -EINVAL if the qpair is not in the disabled state, -ENODEV if the transport
 * doesn't exist, -ENOMEM on memory allocation failures, or -EPROTO on a protocol (transport) specific failure.
 */
/*
 * [한국어]
 * spdk_nvme_poll_group_add - qpair를 poll group에 추가.
 *
 * ★ qpair는 disconnected 상태여야 함 (alloc_io_qpair(create_only=true) 또는 disconnect_io_qpair 후).
 * @return: 0=성공, -EINVAL=qpair가 disconnected 아님, -ENOMEM/-ENODEV/-EPROTO.
 *
 * 추가 후 connect_io_qpair → process_completions 사이클로 사용. 동일 group에 다수 qpair가 모이면
 * RDMA shared CQ 같은 트랜스포트 최적화 활용 가능.
 */
int spdk_nvme_poll_group_add(struct spdk_nvme_poll_group *group, struct spdk_nvme_qpair *qpair);

/**
 * Remove a disconnected spdk_nvme_qpair from a poll group.
 *
 * \param group The group from which to remove the qpair.
 * \param qpair The qpair to remove from the poll group.
 *
 * return 0 on success, -ENOENT if the qpair is not found in the group, -EINVAL if the qpair is not
 * disconnected in the group, or -EPROTO on a protocol (transport) specific failure.
 */
/*
 * [한국어]
 * spdk_nvme_poll_group_remove - qpair를 group에서 제거. ★ qpair가 disconnected 상태여야 함.
 * @return: 0=성공, -ENOENT 없음, -EINVAL connected 상태, -EPROTO transport 실패.
 */
int spdk_nvme_poll_group_remove(struct spdk_nvme_poll_group *group, struct spdk_nvme_qpair *qpair);

/**
 * Wait for interrupt events on file descriptors of all the qpairs in this poll group.
 *
 * In interrupt mode all the file descriptors of qpairs are registered to the fd group within the
 * poll group. This can collectively wait for interrupt events on all those file descriptors.
 *
 * the disconnected_qpair_cb will be called for all disconnected qpairs in the poll group
 * including qpairs which fail within the context of this call.
 * The user is responsible for trying to reconnect or destroy those qpairs.
 *
 * \param group The poll group on which to wait for interrupt events.
 * \param disconnected_qpair_cb A callback function of type spdk_nvme_disconnected_qpair_cb. Must
 * be non-NULL.
 *
 * \return number of events processed on success, -EINVAL if no disconnected_qpair_cb is passed, or
 * -errno in case of failure.
 */
/*
 * [한국어]
 * spdk_nvme_poll_group_wait - 인터럽트 모드에서 group 내 모든 qpair fd에 대해 epoll_wait.
 *
 * @disconnected_qpair_cb: NULL 금지. 단절된 qpair마다 호출.
 * @return: 처리 이벤트 수, -EINVAL/-errno.
 *
 * 인터럽트 모드 + poll group 결합 진입점. process_completions와 달리 fd 이벤트 도달까지 블록.
 */
int spdk_nvme_poll_group_wait(struct spdk_nvme_poll_group *group,
			      spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);

/**
 * Return the fd_group associated with this poll group.
 *
 * \param group Poll group.
 *
 * \return fd_group or NULL if there's no fd_group associated with this poll group.
 */
/*
 * [한국어]
 * spdk_nvme_poll_group_get_fd_group - poll group 내부 spdk_fd_group 반환 (NULL 가능).
 *
 * 외부 epoll에 합성하여 SPDK reactor의 인터럽트 모드 통합 처리에 사용. spdk_thread_get_interrupt_fd_group과
 * 결합해 reactor 단위로 모든 NVMe fd를 한 epoll로 통합.
 */
struct spdk_fd_group *spdk_nvme_poll_group_get_fd_group(struct spdk_nvme_poll_group *group);

/*
 * [한국어]
 * spdk_nvme_poll_group_interrupt_cb - 인터럽트 모드에서 poll group이 polling 필요한 시점 통지 콜백.
 *
 * 단순 I/O 완료는 fd EPOLLIN으로 알지만 qpair 단절 같은 이벤트는 별도 트리거 필요. SPDK가 set_interrupt_callback
 * 등록 시 이 콜백으로 사용자에게 process_completions 호출 요청.
 */
typedef void (*spdk_nvme_poll_group_interrupt_cb)(struct spdk_nvme_poll_group *group, void *ctx);

/**
 * Register a callback to notify that a poll group needs to be polled when in interrupt mode.  This
 * isn't required to poll IO completions, but is necessary to process some events, e.g. qpair
 * disconnection.
 *
 * \param group Poll group.
 * \param cb_fn Callback to be executed when the poll group needs to be polled.
 * \param cb_ctx Callback's context.
 *
 * \return 0 on success or negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_nvme_poll_group_set_interrupt_callback - 인터럽트 모드 알림 콜백 등록.
 * 인터럽트 모드 사용 시 일부 이벤트(qpair disconnect 등)는 fd 이벤트로 안 올 수 있어 이 콜백으로 보정.
 */
int spdk_nvme_poll_group_set_interrupt_callback(struct spdk_nvme_poll_group *group,
		spdk_nvme_poll_group_interrupt_cb cb_fn, void *cb_ctx);

/**
 * Destroy an empty poll group.
 *
 * \param group The group to destroy.
 *
 * return 0 on success, -EBUSY if the poll group is not empty.
 */
/*
 * [한국어]
 * spdk_nvme_poll_group_destroy - 빈 poll group 해제. -EBUSY=qpair 남아있으면 실패.
 * 사용자가 모든 qpair를 remove한 뒤 호출.
 */
int spdk_nvme_poll_group_destroy(struct spdk_nvme_poll_group *group);

/**
 * Poll for completions on all qpairs in this poll group.
 *
 * the disconnected_qpair_cb will be called for all disconnected qpairs in the poll group
 * including qpairs which fail within the context of this call.
 * The user is responsible for trying to reconnect or destroy those qpairs.
 *
 * \param group The group on which to poll for completions.
 * \param completions_per_qpair The maximum number of completions per qpair.
 * \param disconnected_qpair_cb A callback function of type spdk_nvme_disconnected_qpair_cb. Must be non-NULL.
 *
 * return The number of completions across all qpairs, -EINVAL if no disconnected_qpair_cb is passed, or
 * -EIO if the shared completion queue cannot be polled for the RDMA transport.
 */
/*
 * [한국어]
 * spdk_nvme_poll_group_process_completions - ★ poll group 내 모든 qpair에 대해 batch CQ 폴링.
 *
 * @completions_per_qpair: qpair당 처리 상한.
 * @disconnected_qpair_cb: 단절 qpair 발견 시 호출. NULL 금지.
 * @return: 모든 qpair 합산 처리 수, -EINVAL=cb NULL, -EIO=RDMA shared CQ poll 실패.
 *
 * 한 호출로 다수 qpair 처리. 트랜스포트가 group-level 최적화(예: RDMA shared receive queue) 적용 가능 →
 * qpair_process_completions를 N번 부르는 것보다 효율적. bdev_nvme의 핵심 hot path.
 */
int64_t spdk_nvme_poll_group_process_completions(struct spdk_nvme_poll_group *group,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);

/**
 * Check if all qpairs in the poll group are connected.
 *
 * This function allows the caller to check if all qpairs in a poll group are
 * connected. This API is generally only suitable during application startup,
 * to check when a large number of async connections have completed.
 *
 * It is useful for applications like benchmarking tools to create
 * a large number of qpairs, but then ensuring they are all fully connected before
 * proceeding with I/O.
 *
 * \param group The group on which to poll connecting qpairs.
 *
 * return 0 if all qpairs are in CONNECTED state, -EIO if any connections failed to connect, -EAGAIN if
 * any qpairs are still trying to connected.
 */
/*
 * [한국어]
 * spdk_nvme_poll_group_all_connected - group 모든 qpair가 CONNECTED인지 검사.
 * 0=모두 connected, -EAGAIN=일부 진행 중, -EIO=일부 실패. 다수 비동기 connect 완료 대기에 사용.
 */
int spdk_nvme_poll_group_all_connected(struct spdk_nvme_poll_group *group);

/**
 * Retrieve the user context for this specific poll group.
 *
 * \param group The poll group from which to retrieve the context.
 *
 * \return A pointer to the user provided poll group context.
 */
/*
 * [한국어]
 * spdk_nvme_poll_group_get_ctx - poll_group_create 시 등록한 ctx 회수.
 */
void *spdk_nvme_poll_group_get_ctx(struct spdk_nvme_poll_group *group);

/**
 * Retrieves transport statistics for the given poll group.
 *
 * Note: the structure returned by this function should later be freed with
 * @b spdk_nvme_poll_group_free_stats function
 *
 * \param group Pointer to NVME poll group
 * \param stats Double pointer to statistics to be filled by this function
 * \return 0 on success or negated errno on failure
 */
/*
 * [한국어]
 * spdk_nvme_poll_group_get_stats - poll group의 트랜스포트별 통계 수집.
 * @stats: [out] SPDK가 할당한 통계 구조체. ★ free_stats로 반드시 해제 필요.
 */
int spdk_nvme_poll_group_get_stats(struct spdk_nvme_poll_group *group,
				   struct spdk_nvme_poll_group_stat **stats);

/**
 * Frees poll group statistics retrieved using @b spdk_nvme_poll_group_get_stats function
 *
 * @param group Pointer to a poll group
 * @param stat Pointer to statistics to be released
 */
/*
 * [한국어]
 * spdk_nvme_poll_group_free_stats - get_stats로 받은 통계 메모리 해제. SPDK 소유 메모리이므로 직접 free 금지.
 */
void spdk_nvme_poll_group_free_stats(struct spdk_nvme_poll_group *group,
				     struct spdk_nvme_poll_group_stat *stat);

/**
 * Get the identify namespace data as defined by the NVMe specification.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace.
 *
 * \return a pointer to the namespace data.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_data - Identify Namespace data 구조체 반환 (NSZE/NCAP/NUSE/LBAF/NMIC 등).
 * SPDK가 attach 시 캐시. ctrlr_reset 시 무효화.
 */
const struct spdk_nvme_ns_data *spdk_nvme_ns_get_data(struct spdk_nvme_ns *ns);

/**
 * Get the I/O command set specific identify namespace data for NVM command set
 * as defined by the NVMe specification.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace.
 *
 * \return a pointer to the identify namespace data.
 */
/*
 * [한국어]
 * spdk_nvme_nvm_ns_get_data - I/O Command Set Specific(CNS=0x05) NVM ns data 반환.
 * NVMe 1.4 Multi-Command Set 지원 컨트롤러에서 NVM-set 고유 필드(LBSTM 등) 접근.
 */
const struct spdk_nvme_nvm_ns_data *spdk_nvme_nvm_ns_get_data(struct spdk_nvme_ns *ns);

/**
 * Get the namespace id (index number) from the given namespace handle.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace.
 *
 * \return namespace id.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_id - 핸들의 nsid 반환 (1..NN).
 */
uint32_t spdk_nvme_ns_get_id(struct spdk_nvme_ns *ns);

/**
 * Get the controller with which this namespace is associated.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace.
 *
 * \return a pointer to the controller.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_ctrlr - 이 ns가 속한 컨트롤러 핸들 반환. ns에서 ctrlr 거꾸로 찾기.
 */
struct spdk_nvme_ctrlr *spdk_nvme_ns_get_ctrlr(struct spdk_nvme_ns *ns);

/**
 * Determine whether a namespace is active.
 *
 * Inactive namespaces cannot be the target of I/O commands.
 *
 * \param ns Namespace to query.
 *
 * \return true if active, or false if inactive.
 */
/*
 * [한국어]
 * spdk_nvme_ns_is_active - inactive ns는 I/O 명령 대상이 될 수 없음.
 * inactive ns에 ns_cmd_*는 -EINVAL/Invalid Namespace error.
 */
bool spdk_nvme_ns_is_active(struct spdk_nvme_ns *ns);

/**
 * Get the maximum transfer size, in bytes, for an I/O sent to the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the maximum transfer size in bytes.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_max_io_xfer_size - 이 ns에 한 명령으로 보낼 수 있는 최대 byte 수.
 * MDTS와 PRP/SGL 제약, ns별 sector size를 종합해 도출. ns_cmd_*는 자동 split, raw는 사용자가 준수.
 */
uint32_t spdk_nvme_ns_get_max_io_xfer_size(struct spdk_nvme_ns *ns);

/**
 * Get the sector size, in bytes, of the given namespace.
 *
 * This function returns the size of the data sector only.  It does not
 * include metadata size.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * /return the sector size in bytes.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_sector_size - LBA 데이터 부분 크기 byte (메타데이터 제외).
 * 보통 512 또는 4096. LBAF format index가 결정. ns_cmd_*의 byte 길이 변환 시 핵심 단위.
 */
uint32_t spdk_nvme_ns_get_sector_size(struct spdk_nvme_ns *ns);

/**
 * Get the extended sector size, in bytes, of the given namespace.
 *
 * This function returns the size of the data sector plus metadata.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * /return the extended sector size in bytes.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_extended_sector_size - 데이터+메타데이터 통합 byte (extended LBA format).
 * MS bit 0 + extended LBA 모드면 sector_size + md_size. T10 DIF interleave 모드 사용 시.
 */
uint32_t spdk_nvme_ns_get_extended_sector_size(struct spdk_nvme_ns *ns);

/**
 * Get the number of sectors for the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the number of sectors.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_num_sectors - ns의 LBA 총 개수 (NSZE 필드).
 */
uint64_t spdk_nvme_ns_get_num_sectors(struct spdk_nvme_ns *ns);

/**
 * Get the size, in bytes, of the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the size of the given namespace in bytes.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_size - ns 크기 byte 단위 (num_sectors * sector_size).
 */
uint64_t spdk_nvme_ns_get_size(struct spdk_nvme_ns *ns);

/**
 * Get the end-to-end data protection information type of the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the end-to-end data protection information type.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_pi_type - T10 DIF Protection Information Type 0(disabled)/1/2/3 반환.
 */
enum spdk_nvme_pi_type spdk_nvme_ns_get_pi_type(struct spdk_nvme_ns *ns);

/**
 * Get the end-to-end data protection information format of the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the end-to-end data protection information format.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_pi_format - PI format - 16/32/64bit guard tag size (NVMe 2.0 PIFA 필드).
 */
enum spdk_nvme_pi_format spdk_nvme_ns_get_pi_format(struct spdk_nvme_ns *ns);

/**
 * Get the metadata size, in bytes, of the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the metadata size of the given namespace in bytes.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_md_size - 메타데이터 byte 크기 (LBA당). T10 DIF guard+apptag+reftag = 8B(PI Type 1/2/3).
 */
uint32_t spdk_nvme_ns_get_md_size(struct spdk_nvme_ns *ns);

/**
 * Get the format index of the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param nsdata pointer to the NVMe namespace data.
 *
 * \return the format index of the given namespace.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_format_index - 현재 ns의 LBA Format index (FLBAS 비트, 0..15).
 * 가용 LBA format은 nsdata.lbaf[]에 정의. 다중 sector size 지원 SSD에서 활성 format 식별.
 */
uint32_t spdk_nvme_ns_get_format_index(const struct spdk_nvme_ns_data *nsdata);

/**
 * Check whether if the namespace can support extended LBA when end-to-end data
 * protection enabled.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return true if the namespace can support extended LBA when end-to-end data
 * protection enabled, or false otherwise.
 */
/*
 * [한국어]
 * spdk_nvme_ns_supports_extended_lba - DIF Protection 활성 시 metadata interleave LBA 지원 여부.
 */
bool spdk_nvme_ns_supports_extended_lba(struct spdk_nvme_ns *ns);

/**
 * Check whether if the namespace supports write uncorrectable operation
 */
/*
 * [한국어]
 * spdk_nvme_ns_supports_write_uncorrectable - Write Uncorrectable(opcode 0x04) 지원 여부.
 * NVMe ONCS bit 1. ns_cmd_write_uncorrectable 호출 가능 검사.
 */
bool spdk_nvme_ns_supports_write_uncorrectable(struct spdk_nvme_ns *ns);

/**
 * Check whether if the namespace supports compare operation
 */
/*
 * [한국어]
 * spdk_nvme_ns_supports_compare - Compare(opcode 0x05) 지원 여부 (ONCS bit 0).
 */
bool spdk_nvme_ns_supports_compare(struct spdk_nvme_ns *ns);

/**
 * Determine the value returned when reading deallocated blocks.
 *
 * If deallocated blocks return 0, the deallocate command can be used as a more
 * efficient alternative to the write_zeroes command, especially for large requests.
 *
 * \param ns Namespace.
 *
 * \return the logical block read value.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_dealloc_logical_block_read_value - dealloc된 LBA 읽기 결과 동작.
 * READ_NOT_REPORTED/READ_00/READ_FF. 0이면 dealloc을 write_zeroes 대안으로 활용 가능.
 */
enum spdk_nvme_dealloc_logical_block_read_value spdk_nvme_ns_get_dealloc_logical_block_read_value(
	struct spdk_nvme_ns *ns);

/**
 * Get the optimal I/O boundary, in blocks, for the given namespace.
 *
 * Read and write commands should not cross the optimal I/O boundary for best
 * performance.
 *
 * \param ns Namespace to query.
 *
 * \return Optimal granularity of I/O commands, in blocks, or 0 if no optimal
 * granularity is reported.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_optimal_io_boundary - NOIOB 필드 기반 최적 I/O 경계 (블록 단위, 0=권고 없음).
 * 이 경계 cross하면 SSD 내부 split → 지연. bdev_nvme의 io_boundary 정책에 사용.
 */
uint32_t spdk_nvme_ns_get_optimal_io_boundary(struct spdk_nvme_ns *ns);

/**
 * Get the NGUID for the given namespace.
 *
 * \param ns Namespace to query.
 *
 * \return a pointer to namespace NGUID, or NULL if ns does not have a NGUID.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_nguid - 16-byte NGUID 포인터 (모두 0이면 NULL). 글로벌 ns 식별자.
 */
const uint8_t *spdk_nvme_ns_get_nguid(const struct spdk_nvme_ns *ns);

/**
 * Get the UUID for the given namespace.
 *
 * \param ns Namespace to query.
 *
 * \return a pointer to namespace UUID, or NULL if ns does not have a UUID.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_uuid - ns UUID 포인터 (RFC 4122 16바이트). 미설정이면 NULL.
 * Identify Namespace Identification Descriptor List(CNS=0x03)에서 추출.
 */
const struct spdk_uuid *spdk_nvme_ns_get_uuid(const struct spdk_nvme_ns *ns);

/**
 * Get the Command Set Identifier for the given namespace.
 *
 * \param ns Namespace to query.
 *
 * \return the namespace Command Set Identifier.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_csi - Command Set Identifier (NVM=0x00, KV=0x01, ZNS=0x02). NVMe 1.4+ 다중 command set.
 */
enum spdk_nvme_csi spdk_nvme_ns_get_csi(const struct spdk_nvme_ns *ns);

/**
 * \brief Namespace command support flags.
 */
/*
 * [한국어]
 * enum spdk_nvme_ns_flags - ns가 지원하는 명령 비트마스크 (ONCS, ns identify 종합).
 *
 * spdk_nvme_ns_get_flags() 반환값. bdev_nvme가 어떤 NVM 명령을 매핑할지 결정.
 */
enum spdk_nvme_ns_flags {
	SPDK_NVME_NS_DEALLOCATE_SUPPORTED	= 1 << 0, /**< The deallocate command is supported */
	/* [한국어] Dataset Management Deallocate (=trim/unmap) 지원. ns_cmd_dataset_management 가능. */
	SPDK_NVME_NS_FLUSH_SUPPORTED		= 1 << 1, /**< The flush command is supported */
	/* [한국어] Flush(0x00) 명령 지원. 휘발성 캐시 영구화 보장. */
	SPDK_NVME_NS_RESERVATION_SUPPORTED	= 1 << 2, /**< The reservation command is supported */
	/* [한국어] Reservation Acquire/Release/Register/Report 지원. 멀티호스트 락 메커니즘. */
	SPDK_NVME_NS_WRITE_ZEROES_SUPPORTED	= 1 << 3, /**< The write zeroes command is supported */
	/* [한국어] Write Zeroes(0x08) 지원. 데이터 0으로 채우기 (메모리 미사용 즉시). */
	SPDK_NVME_NS_DPS_PI_SUPPORTED		= 1 << 4, /**< The end-to-end data protection is supported */
	/* [한국어] T10 DIF/DIX end-to-end protection 활성. Type 1/2/3 모드 ns. */
	SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED	= 1 << 5, /**< The extended lba format is supported,
							      metadata is transferred as a contiguous
							      part of the logical block that it is associated with */
	/* [한국어] Extended LBA(LBA+MD interleaved) 모드 지원. 별도 metadata buffer 없이 한 buf 전송. */
	SPDK_NVME_NS_WRITE_UNCORRECTABLE_SUPPORTED	= 1 << 6, /**< The write uncorrectable command is supported */
	/* [한국어] Write Uncorrectable(0x04) 지원. 의도적 손상 마킹(테스트/scrubbing). */
	SPDK_NVME_NS_COMPARE_SUPPORTED		= 1 << 7, /**< The compare command is supported */
	/* [한국어] Compare(0x05) 지원. host buffer와 LBA 데이터 비교. fused write와 결합 시 CAS. */
};

/**
 * Get the flags for the given namespace.
 *
 * See spdk_nvme_ns_flags for the possible flags returned.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the flags for the given namespace.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_flags - 위 enum의 OR 비트마스크 반환. attach 시 SPDK가 캐시.
 */
uint32_t spdk_nvme_ns_get_flags(struct spdk_nvme_ns *ns);

/**
 * Get the ANA group ID for the given namespace.
 *
 * This function should be called only if spdk_nvme_ctrlr_is_log_page_supported() returns
 * true for the controller and log page ID SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the ANA group ID for the given namespace.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_ana_group_id - ns의 ANA(Asymmetric Namespace Access) Group ID.
 *
 * NVMe-oF 멀티패스에서 같은 ANA group은 path 변경 시 함께 전이. ANA log 활성 시에만 의미. */
uint32_t spdk_nvme_ns_get_ana_group_id(const struct spdk_nvme_ns *ns);

/**
 * Get the ANA state for the given namespace.
 *
 * This function should be called only if spdk_nvme_ctrlr_is_log_page_supported() returns
 * true for the controller and log page ID SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the ANA state for the given namespace.
 */
/*
 * [한국어]
 * spdk_nvme_ns_get_ana_state - ANA state: OPTIMIZED/NON_OPTIMIZED/INACCESSIBLE/PERSISTENT_LOSS/CHANGE.
 * 멀티패스 라우팅에서 OPTIMIZED 경로 우선 선택, INACCESSIBLE은 fail-over.
 */
enum spdk_nvme_ana_state spdk_nvme_ns_get_ana_state(const struct spdk_nvme_ns *ns);

/**
 * Submit a write I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to the data payload.
 * \param lba Starting LBA to write the data.
 * \param lba_count Length (in sectors) for the write operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_write - ★ NVM Write 명령 (opcode 0x01) 발행.
 *
 * @payload: 단일 물리연속 또는 hugepage 등록된 가상주소.
 * @lba: 시작 LBA (sector 단위, byte 아님).
 * @lba_count: 길이(sector 수). 0 = 1 sector(NVMe spec 0's based).
 * @io_flags: SPDK_NVME_IO_FLAGS_FUA/LR/PRCHK_GUARD/REFTAG/APPTAG 등 OR.
 * @cb_fn: 완료 콜백 (process_completions에서 호출).
 *
 * 동작: SPDK가 lba/lba_count 검증 → MDTS 초과 시 자동 split → vtophys → PRP/SGL 빌드 → SQE 작성 →
 * SQ tail doorbell write (delay_cmd_submit 모드면 batching).
 *
 * @return: 0=발행 성공, -EINVAL=잘못된 인자, -ENOMEM=request 슬롯 없음, -ENXIO=qpair fail.
 *
 * ★ 호출 컨텍스트: qpair owner SPDK 스레드 전용. process_completions 호출 후 cb_fn 실행됨.
 * 호출 체인: 사용자/bdev_nvme → ns_cmd_write → SPDK PRP 빌드 → MMIO doorbell → 완료 폴링 → cb_fn.
 */
int spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			   uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			   void *cb_arg, uint32_t io_flags);

/**
 * Submit a write I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param lba Starting LBA to write the data.
 * \param lba_count Length (in sectors) for the write operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param reset_sgl_fn Callback function to reset scattered payload.
 * \param next_sge_fn Callback function to iterate each scattered payload memory
 * segment.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_writev - scatter-gather Write 변형. 단일 buf 대신 reset_sgl/next_sge 콜백으로 SGE 제공.
 *
 * 사용자 iovec 등을 SPDK가 콜백으로 순회하면서 NVMe SGL element 작성. 비연속 버퍼 페이로드 가능.
 * 큰 사용자 buffer를 split하여 SGE 다수 만들 때 효율적. SGL 미지원 컨트롤러는 PRP로 변환.
 */
int spdk_nvme_ns_cmd_writev(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			    uint64_t lba, uint32_t lba_count,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			    spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			    spdk_nvme_req_next_sge_cb next_sge_fn);

/**
 * Submit a write I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O
 * \param qpair I/O queue pair to submit the request
 * \param lba starting LBA to write the data
 * \param lba_count length (in sectors) for the write operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined in nvme_spec.h, for this I/O
 * \param reset_sgl_fn callback function to reset scattered payload
 * \param next_sge_fn callback function to iterate each scattered
 * payload memory segment
 * \param metadata virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size()
 * \param apptag_mask application tag mask.
 * \param apptag application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_writev_with_md - writev + 별도 metadata 버퍼 + apptag.
 *
 * @metadata: T10 DIF 메타데이터 단일 buf (분리 모드). md_size*lba_count byte 필요.
 * @apptag/apptag_mask: end-to-end protection의 PI Type 1/2/3 application tag.
 */
int spdk_nvme_ns_cmd_writev_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				    uint64_t lba, uint32_t lba_count,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				    spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				    spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				    uint16_t apptag_mask, uint16_t apptag);

/**
 * Submit a write I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O
 * \param qpair I/O queue pair to submit the request
 * \param lba starting LBA to write the data
 * \param lba_count length (in sectors) for the write operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param reset_sgl_fn callback function to reset scattered payload
 * \param next_sge_fn callback function to iterate each scattered
 * payload memory segment
 * \param opts Optional structure with extended IO request options. If provided, the caller must
 * guarantee that this structure is accessible until IO completes
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 * -EFAULT: Invalid address was specified as part of payload.  cb_fn is also called
 *          with error status including dnr=1 in this case.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_writev_ext - writev + ext_io_opts (memory_domain/accel_sequence/apptag 통합).
 *
 * @opts: spdk_nvme_ns_cmd_ext_io_opts (size 필드 필수, GPU/RDMA/accel offload 등 모든 확장 옵션).
 * 가장 일반적인 modern API. memory_domain 활용으로 zero-copy GPU I/O 가능.
 */
int spdk_nvme_ns_cmd_writev_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				uint64_t lba, uint32_t lba_count,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				spdk_nvme_req_next_sge_cb next_sge_fn,
				struct spdk_nvme_ns_cmd_ext_io_opts *opts);

/**
 * Submit a write I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O
 * \param qpair I/O queue pair to submit the request
 * \param payload Virtual address pointer to the data payload.
 * \param lba starting LBA to write the data
 * \param lba_count length (in sectors) for the write operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param opts Optional structure with extended IO request options. If provided, the caller must
 * guarantee that this structure is accessible until IO completes
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 * -EFAULT: Invalid address was specified as part of payload.  cb_fn is also called
 *          with error status including dnr=1 in this case.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_write_ext - 단일 buffer write + ext_io_opts. writev_ext의 single-buf 변형.
 */
int spdk_nvme_ns_cmd_write_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       void *payload, uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			       struct spdk_nvme_ns_cmd_ext_io_opts *opts);

/**
 * Submit a write I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to the data payload.
 * \param metadata Virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size().
 * \param lba Starting LBA to write the data.
 * \param lba_count Length (in sectors) for the write operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 * \param apptag_mask Application tag mask.
 * \param apptag Application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_write_with_md - write + metadata 분리 모드 + apptag.
 * extended LBA 미지원 ns에서 metadata buffer 별도 전달. T10 DIF Type 1/2/3 사용 시.
 */
int spdk_nvme_ns_cmd_write_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				   void *payload, void *metadata,
				   uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
				   void *cb_arg, uint32_t io_flags,
				   uint16_t apptag_mask, uint16_t apptag);

/**
 * Submit a write zeroes I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write zeroes I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param lba Starting LBA for this command.
 * \param lba_count Length (in sectors) for the write zero operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_write_zeroes - Write Zeroes 명령 (opcode 0x08). LBA 범위를 0으로 채움.
 * 페이로드 없음 - 메모리 대역폭 절약. 미지원 ns(SPDK_NVME_NS_WRITE_ZEROES_SUPPORTED 비트)면 -EINVAL.
 */
int spdk_nvme_ns_cmd_write_zeroes(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				  uint64_t lba, uint32_t lba_count,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				  uint32_t io_flags);

/**
 * Submit a verify I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the verify I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param lba Starting LBA to verify the data.
 * \param lba_count Length (in sectors) for the verify operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_verify - Verify 명령 (opcode 0x0C). LBA 데이터를 컨트롤러 내부에서 ECC/CRC 검증만 수행.
 * 호스트로 데이터 전송 없음. 매체 무결성 검사. 페이로드 없는 명령.
 */
int spdk_nvme_ns_cmd_verify(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			    uint64_t lba, uint32_t lba_count,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			    uint32_t io_flags);

/**
 * Submit a write uncorrectable I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write uncorrectable I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param lba Starting LBA for this command.
 * \param lba_count Length (in sectors) for the write uncorrectable operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_write_uncorrectable - Write Uncorrectable (opcode 0x04).
 * 의도적으로 LBA에 uncorrectable error 마킹. 이후 read 시 error 반환. 테스트/scrubbing에 사용.
 */
int spdk_nvme_ns_cmd_write_uncorrectable(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		uint64_t lba, uint32_t lba_count,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Submits a read I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the read I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to the data payload.
 * \param lba Starting LBA to read the data.
 * \param lba_count Length (in sectors) for the read operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_read - ★ NVM Read 명령 (opcode 0x02) 발행. ns_cmd_write의 read 대응.
 *
 * @payload: 읽은 데이터를 담을 버퍼 (DMA 가능 메모리).
 * 동작은 write와 동일하지만 데이터 흐름은 컨트롤러 → 호스트. PRP/SGL은 호스트 destination 표현.
 * 비동기 - cb_fn이 호출될 때까지 payload 메모리 생존 보장 책임.
 */
int spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			  uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			  void *cb_arg, uint32_t io_flags);

/**
 * Submit a read I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the read I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param lba Starting LBA to read the data.
 * \param lba_count Length (in sectors) for the read operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param reset_sgl_fn Callback function to reset scattered payload.
 * \param next_sge_fn Callback function to iterate each scattered payload memory
 * segment.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_readv - scatter-gather Read. writev의 read 대응. iovec destination 지원.
 */
int spdk_nvme_ns_cmd_readv(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   uint64_t lba, uint32_t lba_count,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			   spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			   spdk_nvme_req_next_sge_cb next_sge_fn);

/**
 * Submit a read I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 *
 * \param ns NVMe namespace to submit the read I/O
 * \param qpair I/O queue pair to submit the request
 * \param lba starting LBA to read the data
 * \param lba_count length (in sectors) for the read operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined in nvme_spec.h, for this I/O
 * \param reset_sgl_fn callback function to reset scattered payload
 * \param next_sge_fn callback function to iterate each scattered
 * payload memory segment
 * \param metadata virtual address pointer to the metadata payload, the length
 *	           of metadata is specified by spdk_nvme_ns_get_md_size()
 * \param apptag_mask application tag mask.
 * \param apptag application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_readv_with_md - readv + 별도 metadata + apptag. T10 DIF 분리 모드 read.
 */
int spdk_nvme_ns_cmd_readv_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				   uint64_t lba, uint32_t lba_count,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				   spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				   spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				   uint16_t apptag_mask, uint16_t apptag);

/**
 * Submit a read I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 *
 * \param ns NVMe namespace to submit the read I/O
 * \param qpair I/O queue pair to submit the request
 * \param lba starting LBA to read the data
 * \param lba_count length (in sectors) for the read operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param reset_sgl_fn callback function to reset scattered payload
 * \param next_sge_fn callback function to iterate each scattered
 * payload memory segment
 * \param opts Optional structure with extended IO request options. If provided, the caller must
 * guarantee that this structure is accessible until IO completes
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 * -EFAULT: Invalid address was specified as part of payload.  cb_fn is also called
 *          with error status including dnr=1 in this case.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_readv_ext - readv + ext_io_opts (memory_domain/accel_sequence/apptag). 가장 일반적 modern API.
 */
int spdk_nvme_ns_cmd_readv_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			       void *cb_arg, spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			       spdk_nvme_req_next_sge_cb next_sge_fn,
			       struct spdk_nvme_ns_cmd_ext_io_opts *opts);

/**
 * Submit a read I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 *
 * \param ns NVMe namespace to submit the read I/O
 * \param qpair I/O queue pair to submit the request
 * \param payload virtual address pointer to the data payload
 * \param lba starting LBA to read the data
 * \param lba_count length (in sectors) for the read operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param opts Optional structure with extended IO request options. If provided, the caller must
 * guarantee that this structure is accessible until IO completes
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 * -EFAULT: Invalid address was specified as part of payload.  cb_fn is also called
 *          with error status including dnr=1 in this case.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_read_ext - 단일 buf read + ext_io_opts. read_ext의 single-buf 변형.
 */
int spdk_nvme_ns_cmd_read_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			      uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			      struct spdk_nvme_ns_cmd_ext_io_opts *opts);

/**
 * Submits a read I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the read I/O
 * \param qpair I/O queue pair to submit the request
 * \param payload virtual address pointer to the data payload
 * \param metadata virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size().
 * \param lba starting LBA to read the data.
 * \param lba_count Length (in sectors) for the read operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param apptag_mask Application tag mask.
 * \param apptag Application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_read_with_md - read + metadata 분리. write_with_md의 read 대응.
 */
int spdk_nvme_ns_cmd_read_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				  void *payload, void *metadata,
				  uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
				  void *cb_arg, uint32_t io_flags,
				  uint16_t apptag_mask, uint16_t apptag);

/**
 * Submit a data set management request to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * This is a convenience wrapper that will automatically allocate and construct
 * the correct data buffers. Therefore, ranges does not need to be allocated from
 * pinned memory and can be placed on the stack. If a higher performance, zero-copy
 * version of DSM is required, simply build and submit a raw command using
 * spdk_nvme_ctrlr_cmd_io_raw().
 *
 * \param ns NVMe namespace to submit the DSM request
 * \param type A bit field constructed from \ref spdk_nvme_dsm_attribute.
 * \param qpair I/O queue pair to submit the request
 * \param ranges An array of \ref spdk_nvme_dsm_range elements describing the LBAs
 * to operate on.
 * \param num_ranges The number of elements in the ranges array.
 * \param cb_fn Callback function to invoke when the I/O is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_dataset_management - DSM 명령 (opcode 0x09). trim/IDR/IDW/AD attribute로 hint 전달.
 *
 * @type: SPDK_NVME_DSM_ATTR_DEALLOCATE/IDR/IDW/AD OR (NVMe spec 5.10).
 * @ranges: LBA 범위 배열 (각 spdk_nvme_dsm_range = 16B). 최대 256 entries.
 * @num_ranges: ranges[] 개수 (1..256).
 *
 * SPDK가 ranges를 자동 DMA 가능 메모리에 복사 - 사용자가 stack 변수 사용 가능. 가장 흔한 용도는
 * deallocate(=trim/unmap). bdev_nvme의 BDEV_IO_TYPE_UNMAP 매핑 대상.
 */
int spdk_nvme_ns_cmd_dataset_management(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
					uint32_t type,
					const struct spdk_nvme_dsm_range *ranges,
					uint16_t num_ranges,
					spdk_nvme_cmd_cb cb_fn,
					void *cb_arg);

/**
 * Submit a simple copy command request to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * This is a convenience wrapper that will automatically allocate and construct
 * the correct data buffers. Therefore, ranges does not need to be allocated from
 * pinned memory and can be placed on the stack. If a higher performance, zero-copy
 * version of SCC is required, simply build and submit a raw command using
 * spdk_nvme_ctrlr_cmd_io_raw().
 *
 * \param ns NVMe namespace to submit the SCC request
 * \param qpair I/O queue pair to submit the request
 * \param ranges An array of \ref spdk_nvme_scc_source_range elements describing the LBAs
 * to operate on.
 * \param num_ranges The number of elements in the ranges array.
 * \param dest_lba Destination LBA to copy the data.
 * \param cb_fn Callback function to invoke when the I/O is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -ENOMEM: The request cannot be allocated.
 * -EINVAL: Invalid ranges.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_copy - Simple Copy Command (opcode 0x19). NVMe 1.4+ 컨트롤러 내부 LBA→LBA 복사.
 *
 * 호스트 메모리 거치지 않음 - 컨트롤러가 directly LBA 간 복사. multi-source → single-dest 패턴.
 * @ranges: 소스 LBA 범위 배열 (스택 변수 가능, SPDK가 DMA 메모리로 복사).
 * @dest_lba: 목적지 시작 LBA (모든 source 데이터 연속 배치).
 */
int spdk_nvme_ns_cmd_copy(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			  const struct spdk_nvme_scc_source_range *ranges,
			  uint16_t num_ranges,
			  uint64_t dest_lba,
			  spdk_nvme_cmd_cb cb_fn,
			  void *cb_arg);

/**
 * Submit a flush request to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the flush request.
 * \param qpair I/O queue pair to submit the request.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_flush - Flush 명령 (opcode 0x00). 휘발성 캐시(VWC)를 영구 매체로 동기화.
 * 비용 큰 명령이므로 fsync()처럼 신중히 사용. NVMe spec ONCS bit 1 (Volatile Write Cache).
 */
int spdk_nvme_ns_cmd_flush(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a reservation register to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the reservation register request.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to the reservation register data.
 * \param ignore_key '1' the current reservation key check is disabled.
 * \param action Specifies the registration action.
 * \param cptpl Change the Persist Through Power Loss state.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_reservation_register - Reservation Register (opcode 0x0D).
 *
 * 멀티호스트 환경에서 ns 접근 권한 (예: SCSI Persistent Reservations 대응) 등록.
 * @action: REGISTER_KEY/UNREGISTER_KEY/REPLACE_KEY.
 * @cptpl: persist through power loss 정책. clustered 스토리지에서 활용.
 */
int spdk_nvme_ns_cmd_reservation_register(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_reservation_register_data *payload,
		bool ignore_key,
		enum spdk_nvme_reservation_register_action action,
		enum spdk_nvme_reservation_register_cptpl cptpl,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submits a reservation release to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the reservation release request.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to current reservation key.
 * \param ignore_key '1' the current reservation key check is disabled.
 * \param action Specifies the reservation release action.
 * \param type Reservation type for the namespace.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_reservation_release - Reservation Release. 보유 reservation 해제.
 */
int spdk_nvme_ns_cmd_reservation_release(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_reservation_key_data *payload,
		bool ignore_key,
		enum spdk_nvme_reservation_release_action action,
		enum spdk_nvme_reservation_type type,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submits a reservation acquire to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the reservation acquire request.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to reservation acquire data.
 * \param ignore_key '1' the current reservation key check is disabled.
 * \param action Specifies the reservation acquire action.
 * \param type Reservation type for the namespace.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_reservation_acquire - Reservation Acquire. ns 독점/공유 권한 취득.
 *
 * @action: ACQUIRE/PREEMPT/PREEMPT_AND_ABORT (다른 호스트의 reservation 강제 회수).
 * @type: WRITE_EXCLUSIVE/EXCLUSIVE_ACCESS 변형들 (NVMe spec 8.8).
 */
int spdk_nvme_ns_cmd_reservation_acquire(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_reservation_acquire_data *payload,
		bool ignore_key,
		enum spdk_nvme_reservation_acquire_action action,
		enum spdk_nvme_reservation_type type,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a reservation report to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the reservation report request.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer for reservation status data.
 * \param len Length bytes for reservation status data structure.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_reservation_report - 현재 ns의 reservation 상태 조회 (opcode 0x0E).
 * 등록된 controller 목록과 active reservation type 보고.
 */
int spdk_nvme_ns_cmd_reservation_report(struct spdk_nvme_ns *ns,
					struct spdk_nvme_qpair *qpair,
					void *payload, uint32_t len,
					spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit an I/O management receive command to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the I/O mgmt receive request.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer for I/O mgmt receive data.
 * \param len Length bytes for I/O mgmt receive data structure.
 * \param mo Management operation to perform.
 * \param mos Management operation specific field for the mo.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_io_mgmt_recv - I/O Management Receive (opcode 0x12). NVMe FDP/Endurance Group
 * 관리 정보 read. mo=management operation type, mos=mo-specific.
 */
int spdk_nvme_ns_cmd_io_mgmt_recv(struct spdk_nvme_ns *ns,
				  struct spdk_nvme_qpair *qpair, void *payload,
				  uint32_t len, uint8_t mo, uint16_t mos,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit an I/O management send command to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the I/O mgmt send request.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer for I/O mgmt send data.
 * \param len Length bytes for I/O mgmt send data structure.
 * \param mo Management operation to perform.
 * \param mos Management operation specific field for the mo.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_io_mgmt_send - I/O Management Send (opcode 0x1D). FDP placement directive 등 설정.
 */
int spdk_nvme_ns_cmd_io_mgmt_send(struct spdk_nvme_ns *ns,
				  struct spdk_nvme_qpair *qpair, void *payload,
				  uint32_t len, uint8_t mo, uint16_t mos,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a compare I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the compare I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to the data payload.
 * \param lba Starting LBA to compare the data.
 * \param lba_count Length (in sectors) for the compare operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_compare - Compare 명령 (opcode 0x05). LBA 데이터와 host buffer 비교.
 *
 * 일치 안 하면 cpl.status=COMPARE_FAILURE. fused write(0x01)와 결합 시 NVMe spec 6.2 fused operation
 * 으로 atomic compare-and-write 가능. 멀티호스트 동시성 제어에 사용.
 */
int spdk_nvme_ns_cmd_compare(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			     uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			     void *cb_arg, uint32_t io_flags);

/**
 * Submit a compare I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the compare I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param lba Starting LBA to compare the data.
 * \param lba_count Length (in sectors) for the compare operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param reset_sgl_fn Callback function to reset scattered payload.
 * \param next_sge_fn Callback function to iterate each scattered payload memory
 * segment.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_comparev - scatter-gather compare. SGL 사용한 비교용 페이로드.
 */
int spdk_nvme_ns_cmd_comparev(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      uint64_t lba, uint32_t lba_count,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			      spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			      spdk_nvme_req_next_sge_cb next_sge_fn);

/**
 * Submit a compare I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the compare I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param lba Starting LBA to compare the data.
 * \param lba_count Length (in sectors) for the compare operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param reset_sgl_fn Callback function to reset scattered payload.
 * \param next_sge_fn Callback function to iterate each scattered payload memory
 * segment.
 * \param metadata Virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size()
 * \param apptag_mask Application tag mask.
 * \param apptag Application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int
/*
 * [한국어]
 * spdk_nvme_ns_cmd_comparev_with_md - comparev + metadata 분리 + apptag.
 */
spdk_nvme_ns_cmd_comparev_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				  uint64_t lba, uint32_t lba_count,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				  spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				  spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				  uint16_t apptag_mask, uint16_t apptag);

/**
 * Submit a compare I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the compare I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to the data payload.
 * \param metadata Virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size().
 * \param lba Starting LBA to compare the data.
 * \param lba_count Length (in sectors) for the compare operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param apptag_mask Application tag mask.
 * \param apptag Application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_ns_cmd_compare_with_md - compare + metadata 분리 모드.
 */
int spdk_nvme_ns_cmd_compare_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				     void *payload, void *metadata,
				     uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
				     void *cb_arg, uint32_t io_flags,
				     uint16_t apptag_mask, uint16_t apptag);

/**
 * \brief Inject an error for the next request with a given opcode.
 *
 * \param ctrlr NVMe controller.
 * \param qpair I/O queue pair to add the error command,
 *              NULL for Admin queue pair.
 * \param opc Opcode for Admin or I/O commands.
 * \param do_not_submit True if matching requests should not be submitted
 *                      to the controller, but instead completed manually
 *                      after timeout_in_us has expired.  False if matching
 *                      requests should be submitted to the controller and
 *                      have their completion status modified after the
 *                      controller completes the request.
 * \param timeout_in_us Wait specified microseconds when do_not_submit is true.
 * \param err_count Number of matching requests to inject errors.
 * \param sct Status code type.
 * \param sc Status code.
 *
 * \return 0 if successfully enabled, ENOMEM if an error command
 *	     structure cannot be allocated.
 *
 * The function can be called multiple times to inject errors for different
 * commands.  If the opcode matches an existing entry, the existing entry
 * will be updated with the values specified.
 */
/*
 * [한국어]
 * spdk_nvme_qpair_add_cmd_error_injection - 특정 opcode 명령에 인위적 에러 주입 (테스트용).
 *
 * @do_not_submit: true=실제 컨트롤러에 안 보내고 timeout 후 사용자 cb_fn에 가짜 cpl로 응답.
 *                 false=컨트롤러에 보내고 정상 완료 후 cpl만 수정.
 * @err_count: 매칭 명령 N개에 에러 주입 후 자동 비활성화.
 * @sct/sc: NVMe spec status code type/code.
 *
 * 페일오버, retry 정책, error handling path 테스트에 사용. Production 사용 금지.
 */
int spdk_nvme_qpair_add_cmd_error_injection(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair,
		uint8_t opc,
		bool do_not_submit,
		uint64_t timeout_in_us,
		uint32_t err_count,
		uint8_t sct, uint8_t sc);

/**
 * \brief Clear the specified NVMe command with error status.
 *
 * \param ctrlr NVMe controller.
 * \param qpair I/O queue pair to remove the error command,
 * \            NULL for Admin queue pair.
 * \param opc Opcode for Admin or I/O commands.
 *
 * The function will remove specified command in the error list.
 */
/*
 * [한국어]
 * spdk_nvme_qpair_remove_cmd_error_injection - error injection 항목 제거.
 */
void spdk_nvme_qpair_remove_cmd_error_injection(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair,
		uint8_t opc);

/**
 * \brief Given NVMe status, return ASCII string for that error.
 *
 * \param status Status from NVMe completion queue element.
 * \return Returns status as an ASCII string.
 */
/*
 * [한국어]
 * spdk_nvme_cpl_get_status_string - cpl.status를 사람용 문자열로 변환 (예: "INVALID NAMESPACE OR FORMAT").
 * NVMe spec Figure 95/96 (status code) 매핑 테이블. 정적 문자열 반환.
 */
const char *spdk_nvme_cpl_get_status_string(const struct spdk_nvme_status *status);

/**
 * \brief Given NVMe status, return ASCII string for the type of that error.
 *
 * \param status Status from NVMe completion queue element.
 * \return Returns status type as an ASCII string.
 */
/*
 * [한국어]
 * spdk_nvme_cpl_get_status_type_string - status type(SCT) 카테고리 문자열 (Generic/Cmd Specific/Media).
 */
const char *spdk_nvme_cpl_get_status_type_string(const struct spdk_nvme_status *status);

/**
 * \brief Prints (SPDK_NOTICELOG) the contents of an NVMe submission queue entry (command).
 *
 * \param qpair Pointer to the NVMe queue pair - used to determine admin versus I/O queue.
 * \param cmd Pointer to the submission queue command to be formatted.
 */
/*
 * [한국어]
 * spdk_nvme_qpair_print_command - SQE를 디버그 형식으로 NOTICELOG 출력. opcode/cdw/PRP/SGL 사람용.
 */
void spdk_nvme_qpair_print_command(struct spdk_nvme_qpair *qpair,
				   struct spdk_nvme_cmd *cmd);

/**
 * \brief Prints (SPDK_NOTICELOG) the contents of an NVMe completion queue entry.
 *
 * \param qpair Pointer to the NVMe queue pair - presently unused.
 * \param cpl Pointer to the completion queue element to be formatted.
 */
/*
 * [한국어]
 * spdk_nvme_qpair_print_completion - CQE를 디버그 형식으로 NOTICELOG 출력 (cdw0/sct/sc/sqid/cid).
 */
void spdk_nvme_qpair_print_completion(struct spdk_nvme_qpair *qpair,
				      struct spdk_nvme_cpl *cpl);

/**
 * \brief Gets the NVMe qpair ID for the specified qpair.
 *
 * \param qpair Pointer to the NVMe queue pair.
 * \returns ID for the specified qpair.
 */
/*
 * [한국어]
 * spdk_nvme_qpair_get_id - qpair의 NVMe QID 반환 (admin=0, I/O=1..N).
 */
uint16_t spdk_nvme_qpair_get_id(struct spdk_nvme_qpair *qpair);

/**
 * Gets the number of outstanding requests for the specified qpair.
 *
 * This number is not decremented until after a request's callback function is completed.
 *
 * This number is not matched necessarily to the number of NVMe commands submitted by the
 * user. For example, nvme driver may split a request due to MDTS limitations, that will
 * also allocate a request for the parent, etc.
 *
 * \param qpair Pointer to the NVMe queue pair.
 * \returns number of outstanding requests for the specified qpair.
 */
/*
 * [한국어]
 * spdk_nvme_qpair_get_num_outstanding_reqs - 진행 중 SPDK request 슬롯 수.
 *
 * cb_fn 완료 후에야 감소. 하나의 사용자 명령이 split되면 N개로 카운트. 부하 모니터링/throttling용.
 */
uint32_t spdk_nvme_qpair_get_num_outstanding_reqs(struct spdk_nvme_qpair *qpair);

/**
 * \brief Prints (SPDK_NOTICELOG) the contents of an NVMe submission queue entry (command).
 *
 * \param qid Queue identifier.
 * \param cmd Pointer to the submission queue command to be formatted.
 */
/*
 * [한국어]
 * spdk_nvme_print_command - qpair_print_command의 qid-only 변형. qpair 핸들 없이 raw SQE 디버깅에 사용.
 */
void spdk_nvme_print_command(uint16_t qid, struct spdk_nvme_cmd *cmd);

/**
 * \brief Prints (SPDK_NOTICELOG) the contents of an NVMe completion queue entry.
 *
 * \param qid Queue identifier.
 * \param cpl Pointer to the completion queue element to be formatted.
 */
/*
 * [한국어]
 * spdk_nvme_print_completion - qpair_print_completion의 qid-only 변형. raw CQE 디버깅.
 */
void spdk_nvme_print_completion(uint16_t qid, struct spdk_nvme_cpl *cpl);

/**
 * Return the name of a digest.
 *
 * \param id Digest identifier (see `enum spdk_nvmf_dhchap_hash`).
 *
 * \return Name of the digest.
 */
/*
 * [한국어]
 * spdk_nvme_dhchap_get_digest_name - DH-HMAC-CHAP digest enum → 이름("sha256"/"sha384"/"sha512").
 */
const char *spdk_nvme_dhchap_get_digest_name(int id);

/**
 * Return the id of a digest.
 *
 * \param name Name of a digest.
 *
 * \return Digest id (see `enum spdk_nvmf_dhchap_hash`) or negative errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_dhchap_get_digest_id - 이름→enum digest. ctrlr_opts.dhchap_digests 비트 셋팅에 사용.
 */
int spdk_nvme_dhchap_get_digest_id(const char *name);

/**
 * Return the length of a digest.
 *
 * \param id Digest identifier (see `enum spdk_nvmf_dhchap_hash`).
 *
 * \return Length of a digest or 0 if the id is unknown.
 */
/*
 * [한국어]
 * spdk_nvme_dhchap_get_digest_length - digest hash 출력 byte 길이 (32/48/64).
 */
uint8_t spdk_nvme_dhchap_get_digest_length(int id);

/**
 * Return the name of a Diffie-Hellman group.
 *
 * \param id Diffie-Hellman group identifier (see `enum spdk_nvmf_dhchap_dhgroup`).
 *
 * \return Name of the Diffie-Hellman group.
 */
/*
 * [한국어]
 * spdk_nvme_dhchap_get_dhgroup_name - DH group enum→이름 ("ffdhe2048".."ffdhe8192").
 */
const char *spdk_nvme_dhchap_get_dhgroup_name(int id);

/**
 * Return the id of a Diffie-Hellman group.
 *
 * \param name Name of a Diffie-Hellman group.
 *
 * \return Diffie-Hellman group id (see `enum spdk_nvmf_dhchap_dhgroup`) or negative errno
 * on failure.
 */
/*
 * [한국어]
 * spdk_nvme_dhchap_get_dhgroup_id - 이름→enum DH group. ctrlr_opts.dhchap_dhgroups 비트 셋팅.
 */
int spdk_nvme_dhchap_get_dhgroup_id(const char *name);

/* [한국어] libibverbs 전방 선언 - RDMA 트랜스포트 hook에서 사용. ibv_context=verbs handle,
 * ibv_pd=Protection Domain, ibv_mr=Memory Region. RDMA 미사용 빌드에서도 헤더 컴파일 가능. */
struct ibv_context;
struct ibv_pd;
struct ibv_mr;

/**
 * RDMA Transport Hooks
 */
/*
 * [한국어]
 * struct spdk_nvme_rdma_hooks - RDMA 트랜스포트 사용자 정의 훅.
 *
 * 보통 SPDK가 자체 ibv_alloc_pd + ibv_reg_mr 수행하지만, 외부에서 이미 PD/MR을 등록해 둔 경우
 * 사용자 콜백으로 protection domain/rkey 공급 가능. 한 프로세스당 1회 init_hooks 호출.
 */
struct spdk_nvme_rdma_hooks {
	/**
	 * \brief Get an InfiniBand Verbs protection domain.
	 */
	struct ibv_pd *(*get_ibv_pd)(const struct spdk_nvme_transport_id *trid,
				     struct ibv_context *verbs);
	/* [한국어] 사용자가 미리 만든 ibv_pd를 SPDK에 제공 (ibv_alloc_pd 우회). NULL 반환 시 SPDK 기본 동작. */

	/**
	 * \brief Get an InfiniBand Verbs memory region for a buffer.
	 */
	uint64_t (*get_rkey)(struct ibv_pd *pd, void *buf, size_t size);
	/* [한국어] buf의 RDMA remote key 반환. 외부에서 ibv_reg_mr로 등록된 메모리 활용 가능. */

	/**
	 * \brief Put back keys got from get_rkey.
	 */
	void (*put_rkey)(uint64_t key);
	/* [한국어] get_rkey로 받은 키 반환. 사용자 측 reference count 관리에 사용. */
};

/**
 * \brief Set the global hooks for the RDMA transport, if necessary.
 *
 * This call is optional and must be performed prior to probing for
 * any devices. By default, the RDMA transport will use the ibverbs
 * library to create protection domains and register memory. This
 * is a mechanism to subvert that and use an existing registration.
 *
 * This function may only be called one time per process.
 *
 * \param hooks for initializing global hooks
 */
/*
 * [한국어]
 * spdk_nvme_rdma_init_hooks - RDMA hooks 글로벌 등록 (한 프로세스당 1회). probe 이전 호출.
 */
void spdk_nvme_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks);

/**
 * Get name of cuse device associated with NVMe controller.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param name	Buffer of be filled with cuse device name.
 * \param size	Size of name buffer.
 *
 * \return 0 on success. Negated errno on the following error conditions:
 * -ENODEV: No cuse device registered for the controller.
 * -ENSPC: Too small buffer size passed. Value of size pointer changed to the required length.
 */
/*
 * [한국어]
 * spdk_nvme_cuse_get_ctrlr_name - CUSE controller device 이름 조회 ("/dev/spdkX" 같은 device path).
 * size가 부족하면 -ENSPC + 필요한 크기 *size에 기록.
 */
int spdk_nvme_cuse_get_ctrlr_name(struct spdk_nvme_ctrlr *ctrlr, char *name, size_t *size);

/**
 * Get name of cuse device associated with NVMe namespace.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param nsid	Namespace id.
 * \param name	Buffer of be filled with cuse device name.
 * \param size	Size of name buffer.
 *
 * \return 0 on success. Negated errno on the following error conditions:
 * -ENODEV: No cuse device registered for the namespace.
 * -ENSPC: Too small buffer size passed. Value of size pointer changed to the required length.
 */
/*
 * [한국어]
 * spdk_nvme_cuse_get_ns_name - 특정 ns의 CUSE namespace device 이름 ("/dev/spdkXnY").
 */
int spdk_nvme_cuse_get_ns_name(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			       char *name, size_t *size);

/**
 * Create a character device at the path specified
 *
 * The character device can handle ioctls and is compatible with a standard
 * Linux kernel NVMe device. Tools such as nvme-cli can be used to configure
 * SPDK devices through this interface.
 *
 * The user is expected to be polling the admin qpair for this controller periodically
 * for the CUSE device to function.
 *
 * \param ctrlr Opaque handle to the NVMe controller.
 *
 * \return 0 on success. Negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_cuse_register - ★ CUSE(Character device in USEr space) 노드 생성.
 *
 * /dev/spdkcontrolN(컨트롤러)와 /dev/spdknvmeNnM(ns)을 생성해 표준 Linux NVMe 캐릭터 디바이스처럼
 * 노출. nvme-cli, smartctl 같은 일반 도구로 SPDK SSD 관리 가능. 사용자가 admin qpair 폴링 책임.
 *
 * 호출 체인: 사용자 → cuse_register → libfuse cuse 세션 생성 → ioctl 라우팅 → SPDK admin 명령.
 */
int spdk_nvme_cuse_register(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Remove a previously created character device
 *
 * \param ctrlr Opaque handle to the NVMe controller.
 *
 * \return 0 on success. Negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_cuse_unregister - CUSE 노드 제거. 사용자가 detach 전 호출.
 */
int spdk_nvme_cuse_unregister(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get SPDK memory domains used by the given nvme controller.
 *
 * The user can call this function with \b domains set to NULL and \b array_size set to 0 to get the
 * number of memory domains used by nvme controller
 *
 * \param ctrlr Opaque handle to the NVMe controller.
 * \param domains Pointer to an array of memory domains to be filled by this function. The user should allocate big enough
 * array to keep all memory domains used by nvme controller
 * \param array_size size of \b domains array
 * \return the number of entries in \b domains array or negated errno. If returned value is bigger than \b array_size passed by the user
 * then the user should increase the size of \b domains array and call this function again. There is no guarantees that
 * the content of \b domains array is valid in that case.
 *         -EINVAL if input parameters were invalid

 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_get_memory_domains - 컨트롤러가 지원하는 memory domain 배열 조회.
 *
 * @domains: [out] NULL+array_size=0 호출 시 실제 개수만 반환 (size discovery 패턴).
 * @return: 채워진 개수, 또는 array_size보다 크면 사용자가 재할당 후 재호출 필요. -EINVAL=잘못된 인자.
 *
 * ns_cmd_*_ext의 memory_domain 옵션 사용 전 컨트롤러 지원 확인. RDMA/PCIe + GPU memory 등 매핑.
 */
int spdk_nvme_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				       struct spdk_memory_domain **domains, int array_size);

/**
 * Opaque handle for a transport poll group. Used by the transport function table.
 */
/* [한국어] 트랜스포트별 poll group 내부 표현 (불투명). spdk_nvme_poll_group이 트랜스포트마다
 * 1개씩 보유. 트랜스포트 vtable의 콜백 인자로 사용. */
struct spdk_nvme_transport_poll_group;

/**
 * Update and populate namespace CUSE devices (Experimental)
 *
 * \param ctrlr Opaque handle to the NVMe controller.
 *
 */
/*
 * [한국어]
 * spdk_nvme_cuse_update_namespaces - ns 추가/삭제 후 CUSE namespace device 갱신 (실험적).
 * 핫플러그된 ns에 대해 /dev/spdknvmeNnM 노드 동기화.
 */
void spdk_nvme_cuse_update_namespaces(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Signature for callback invoked after completing a register read/write operation.
 *
 * \param ctx Context passed by the user.
 * \param value Value of the register, undefined in case of a failure.
 * \param cpl Completion queue entry that contains the status of the command.
 */
/*
 * [한국어]
 * spdk_nvme_reg_cb - register read/write 비동기 완료 콜백.
 * @value: read 결과 값 (write는 무의미). @cpl: NVMe completion (fabrics fabric property command).
 * 트랜스포트가 register access를 비동기로 처리할 때(fabrics) 사용. PCIe MMIO는 보통 동기.
 */
typedef void (*spdk_nvme_reg_cb)(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl);

struct nvme_request;
/* [한국어] lib/nvme 내부 spdk_nvme_request 전방 선언. transport_ops vtable에서 인자 타입으로만 사용,
 * 외부 코드는 직접 다루지 않음. */

struct spdk_nvme_transport;
/* [한국어] 등록된 트랜스포트 인스턴스 (불투명). transport_register로 vtable 등록 시 SPDK 내부에서 생성. */

/*
 * [한국어]
 * struct spdk_nvme_transport_ops - 트랜스포트 backend가 SPDK NVMe core에 제공하는 vtable.
 *
 * SPDK_NVME_TRANSPORT_REGISTER(name, &ops) constructor로 lib/nvme 부팅 시 자동 등록.
 * PCIe(lib/nvme/nvme_pcie.c)/RDMA/TCP/FC/vfio-user 모두 이 vtable 한 인스턴스씩 export.
 * 사용자 정의 트랜스포트(시뮬레이터/시험용) 작성 시 이 구조체를 채워 spdk_nvme_transport_register() 호출.
 */
struct spdk_nvme_transport_ops {
	char name[SPDK_NVMF_TRSTRING_MAX_LEN + 1];
	/* [한국어] 트랜스포트 이름("PCIE"/"RDMA"/"TCP" 등). transport_id.trstring과 매칭에 사용. */

	enum spdk_nvme_transport_type type;
	/* [한국어] 트랜스포트 타입 enum. spdk_nvme_transport_type 매핑. */

	struct spdk_nvme_ctrlr *(*ctrlr_construct)(const struct spdk_nvme_transport_id *trid,
			const struct spdk_nvme_ctrlr_opts *opts,
			void *devhandle);
	/* [한국어] 트랜스포트별 spdk_nvme_ctrlr 인스턴스 생성. PCIe면 BAR 매핑, fabrics면 connect.
	 * @devhandle: PCIe spdk_pci_device 등 트랜스포트 specific. */

	int (*ctrlr_scan)(struct spdk_nvme_probe_ctx *probe_ctx, bool direct_connect);
	/* [한국어] 트랜스포트 enumerate. probe_ctx 의 trid에 맞춰 발견된 ctrlr를 ctx에 누적.
	 * direct_connect=true면 단일 endpoint connect (probe_async vs connect_async 분기). */

	int (*ctrlr_destruct)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] ctrlr 소멸 (Shutdown Notification 후 자원 해제). detach 경로에서 호출. */

	int (*ctrlr_enable)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] CC.EN=1 → CSTS.RDY=1 대기. NVMe spec 7.3 enable sequence. */

	int (*ctrlr_enable_interrupts)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] 인터럽트 모드 활성화 (MSI-X 매핑 또는 fabrics fd EPOLLIN). */

	int (*ctrlr_set_reg_4)(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value);
	/* [한국어] 32bit register write. PCIe=MMIO write32, fabrics=Property Set capsule. 동기. */

	int (*ctrlr_set_reg_8)(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value);
	/* [한국어] 64bit register write (CAP/ASQ/ACQ 같은 8B 영역). */

	int (*ctrlr_get_reg_4)(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value);
	/* [한국어] 32bit register read. */

	int (*ctrlr_get_reg_8)(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value);
	/* [한국어] 64bit register read. */

	int (*ctrlr_set_reg_4_async)(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value,
				     spdk_nvme_reg_cb cb_fn, void *cb_arg);
	/* [한국어] 32bit register write 비동기. fabrics에서 reactor 블로킹 방지에 유용. */

	int (*ctrlr_set_reg_8_async)(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value,
				     spdk_nvme_reg_cb cb_fn, void *cb_arg);
	/* [한국어] 64bit register write 비동기. */

	int (*ctrlr_get_reg_4_async)(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
				     spdk_nvme_reg_cb cb_fn, void *cb_arg);
	/* [한국어] 32bit register read 비동기. cb_fn에 value 전달. */

	int (*ctrlr_get_reg_8_async)(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
				     spdk_nvme_reg_cb cb_fn, void *cb_arg);
	/* [한국어] 64bit register read 비동기. */

	uint32_t (*ctrlr_get_max_xfer_size)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] 트랜스포트별 MDTS 환산 max byte. RDMA는 MR size 등 추가 제약. */

	uint16_t (*ctrlr_get_max_sges)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] 트랜스포트가 지원하는 최대 SGE 수. PCIe는 MAX_SGL_DESC, RDMA는 MR limit. */

	int (*ctrlr_reserve_cmb)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] PCIe 전용. CMB 영역 사용자 데이터 전송용 예약. fabrics는 -ENOTSUP. */

	void *(*ctrlr_map_cmb)(struct spdk_nvme_ctrlr *ctrlr, size_t *size);
	/* [한국어] CMB CPU 가시 매핑. */

	int (*ctrlr_unmap_cmb)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] CMB unmap. */

	int (*ctrlr_enable_pmr)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] PMR enable. */

	int (*ctrlr_disable_pmr)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] PMR disable. */

	void *(*ctrlr_map_pmr)(struct spdk_nvme_ctrlr *ctrlr, size_t *size);
	/* [한국어] PMR CPU 매핑. */

	int (*ctrlr_unmap_pmr)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] PMR unmap. */

	struct spdk_nvme_qpair *(*ctrlr_create_io_qpair)(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
			const struct spdk_nvme_io_qpair_opts *opts);
	/* [한국어] qpair 메모리 할당 + 트랜스포트별 자원 (RDMA QP, TCP socket, PCIe SQ/CQ ring). */

	int (*ctrlr_delete_io_qpair)(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);
	/* [한국어] qpair 자원 해제. Delete I/O SQ/CQ admin 명령 + 메모리 free. */

	int (*ctrlr_connect_qpair)(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);
	/* [한국어] qpair connect. fabrics fabric Connect 명령 발행, PCIe 내부 state 전이. */

	void (*ctrlr_disconnect_qpair)(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);
	/* [한국어] qpair 단절. RDMA QP modify→ERR, TCP close, PCIe drain. */

	void (*qpair_abort_reqs)(struct spdk_nvme_qpair *qpair, uint32_t dnr);
	/* [한국어] outstanding request 일괄 abort - 사용자 cb_fn에 ABORTED status 전달. */

	int (*qpair_reset)(struct spdk_nvme_qpair *qpair);
	/* [한국어] qpair 내부 state 초기화 (SQ/CQ pointer, request slot 비우기). */

	int (*qpair_submit_request)(struct spdk_nvme_qpair *qpair, struct nvme_request *req);
	/* [한국어] ★ 한 NVMe request를 트랜스포트에 발행. PCIe=SQE 작성+doorbell, RDMA=SEND, TCP=PDU.
	 * ns_cmd_*가 결국 이 함수 호출. hot path. */

	int (*qpair_authenticate)(struct spdk_nvme_qpair *qpair);
	/* [한국어] DH-HMAC-CHAP 인증 시퀀스 시작 (NVMe-oF 2.0). */

	int32_t (*qpair_process_completions)(struct spdk_nvme_qpair *qpair, uint32_t max_completions);
	/* [한국어] ★ 트랜스포트 CQ 폴링 + 사용자 cb_fn 디스패치. spdk_nvme_qpair_process_completions의
	 * 백엔드 구현. PCIe=phase bit 체크, RDMA=ibv_poll_cq, TCP=PDU 파싱. */

	int (*qpair_iterate_requests)(struct spdk_nvme_qpair *qpair,
				      int (*iter_fn)(struct nvme_request *req, void *arg),
				      void *arg);
	/* [한국어] qpair 의 모든 outstanding request를 순회. timeout 검사, abort_ext 매칭에 사용. */

	int (*qpair_get_fd)(struct spdk_nvme_qpair *qpair, struct spdk_event_handler_opts *opts);
	/* [한국어] qpair fd 반환 (인터럽트 모드). PCIe MSI-X eventfd, fabrics socket fd. */

	void (*admin_qpair_abort_aers)(struct spdk_nvme_qpair *qpair);
	/* [한국어] admin queue의 AER 명령 abort. detach 시 stuck AER 정리. */

	struct spdk_nvme_transport_poll_group *(*poll_group_create)(void);
	/* [한국어] 트랜스포트별 poll group 인스턴스 생성. RDMA=shared receive queue, TCP=epoll fd_group. */

	int (*poll_group_add)(struct spdk_nvme_transport_poll_group *tgroup, struct spdk_nvme_qpair *qpair);
	/* [한국어] qpair를 트랜스포트 poll group에 추가. */

	int (*poll_group_remove)(struct spdk_nvme_transport_poll_group *tgroup,
				 struct spdk_nvme_qpair *qpair);
	/* [한국어] qpair를 그룹에서 제거. */

	int (*poll_group_connect_qpair)(struct spdk_nvme_qpair *qpair);
	/* [한국어] poll group 컨텍스트에서 qpair connect. group-aware connect 동작. */

	int (*poll_group_disconnect_qpair)(struct spdk_nvme_qpair *qpair);
	/* [한국어] poll group 안에서 qpair disconnect. */

	int64_t (*poll_group_process_completions)(struct spdk_nvme_transport_poll_group *tgroup,
			uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
	/* [한국어] ★ poll group 단위 CQ 폴링 (per-qpair process_completions 합산보다 효율적).
	 * 트랜스포트가 batched poll 최적화 적용. */

	void (*poll_group_check_disconnected_qpairs)(struct spdk_nvme_transport_poll_group *tgroup,
			spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
	/* [한국어] 단절된 qpair만 빠르게 체크 (process_completions 없이 disconnect 정리만). */

	int (*poll_group_destroy)(struct spdk_nvme_transport_poll_group *tgroup);
	/* [한국어] poll group 자원 해제. */

	int (*poll_group_get_stats)(struct spdk_nvme_transport_poll_group *tgroup,
				    struct spdk_nvme_transport_poll_group_stat **stats);
	/* [한국어] 트랜스포트별 통계 채움 (RDMA/PCIe/TCP union). */

	void (*poll_group_free_stats)(struct spdk_nvme_transport_poll_group *tgroup,
				      struct spdk_nvme_transport_poll_group_stat *stats);
	/* [한국어] get_stats 메모리 해제. */

	int (*ctrlr_get_memory_domains)(const struct spdk_nvme_ctrlr *ctrlr,
					struct spdk_memory_domain **domains,
					int array_size);
	/* [한국어] 트랜스포트가 지원하는 memory domain 배열. PCIe=PCIe BAR/CMB, RDMA=registered memory. */

	int (*ctrlr_ready)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] 트랜스포트가 ctrlr를 사용 가능 상태로 만드는 마무리 단계. fabrics fabric Connect 후 호출. */

	volatile struct spdk_nvme_registers *(*ctrlr_get_registers)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] BAR 매핑된 register 영역 직접 접근 (PCIe만). */

	/* Optional callback for transports to process removal events of attached controllers. */
	int (*ctrlr_scan_attached)(struct spdk_nvme_probe_ctx *probe_ctx);
	/* [한국어] 옵션. attach된 ctrlr에 대해 hot-remove 같은 이벤트 스캔. spdk_nvme_scan_attached 백엔드. */

	/* Optional callback for transports to process transport-specific events. E.g. poll RDMA_CM event channel */
	int (*ctrlr_process_transport_events)(struct spdk_nvme_ctrlr *ctrlr);
	/* [한국어] 옵션. 트랜스포트 specific 이벤트 처리 (RDMA_CM event channel 폴링 등). */
};

/**
 * Register the operations for a given transport type.
 *
 * This function should be invoked by referencing the macro
 * SPDK_NVME_TRANSPORT_REGISTER macro in the transport's .c file.
 *
 * \param ops The operations associated with an NVMe-oF transport.
 */
/*
 * [한국어]
 * spdk_nvme_transport_register - vtable 등록 (보통 SPDK_NVME_TRANSPORT_REGISTER 매크로 통해 자동).
 *
 * lib/nvme/nvme_pcie.c, nvme_rdma.c, nvme_tcp.c, nvme_fc.c, nvme_vfio_user.c 가 이 함수 호출하여
 * 자기 vtable 등록. 외부 사용자도 customer transport 작성 가능.
 */
void spdk_nvme_transport_register(const struct spdk_nvme_transport_ops *ops);

/*
 * Macro used to register new transports.
 */
/*
 * [한국어]
 * SPDK_NVME_TRANSPORT_REGISTER - constructor 자동 등록 매크로.
 *
 * GCC __attribute__((constructor))로 main() 진입 전 자동 호출되는 _spdk_nvme_transport_register_<name>
 * 함수를 만들어 transport_ops 등록. 사용자는 매크로 한 줄로 트랜스포트 등록 가능. SPDK_RPC_REGISTER /
 * SPDK_LOG_REGISTER_COMPONENT와 같은 패턴.
 */
#define SPDK_NVME_TRANSPORT_REGISTER(name, transport_ops) \
static void __attribute__((constructor)) _spdk_nvme_transport_register_##name(void) \
{ \
	spdk_nvme_transport_register(transport_ops); \
}

/**
 * NVMe transport options.
 */
/*
 * [한국어]
 * struct spdk_nvme_transport_opts - 트랜스포트 글로벌 옵션 (32B).
 *
 * 컨트롤러별 ctrlr_opts와 다르게 process 전체에 영향. transport_set_opts로 한 번 설정.
 * RDMA SRQ size, CM event timeout, TCP connect timeout 등 fabrics specific 튜닝.
 */
struct spdk_nvme_transport_opts {
	/**
	 * It is used for RDMA transport.
	 *
	 * The queue depth of a shared rdma receive queue.
	 */
	uint32_t rdma_srq_size;
	/* [한국어] RDMA Shared Receive Queue 깊이. 다수 qpair가 한 SRQ 공유 시 메모리 절감.
	 * 0=SRQ 미사용 (qpair마다 별도 RQ). 너무 작으면 receive WR 부족으로 stall. */

	/* Hole at bytes 4-7. */
	uint8_t reserved4[4];
	/* [한국어] 패딩. opts_size(8B) align. */

	/**
	 * The size of spdk_nvme_transport_opts according to the caller of this library is used for ABI
	 * compatibility.
	 */
	size_t opts_size;
	/* [한국어] ABI guard - 호출자 컴파일 sizeof. 새 옵션 추가에 호환성 유지. */

	/**
	 * It is used for RDMA transport.
	 */
	uint32_t rdma_max_cq_size;
	/* [한국어] RDMA Completion Queue 최대 깊이. 0=무제한(기본). 너무 크면 메모리 낭비. */

	/**
	 * It is used for RDMA transport.
	 *
	 * RDMA CM event timeout in milliseconds.
	 */
	uint16_t rdma_cm_event_timeout_ms;
	/* [한국어] rdma_resolve_addr 등 CM 이벤트 timeout. 네트워크 RTT 큰 환경에서 키워야 함. */

	/**
	 * It is used for RDMA transport.
	 *
	 * Configure UMR per IO request if supported by the system
	 */
	bool rdma_umr_per_io;
	/* [한국어] User Memory Region per-IO 모드 (Mellanox UMR 기능). I/O마다 메모리 재등록 - 동적
	 * 메모리 사용 패턴에서 사전 등록 부담 회피. */

	/* Hole at byte 23. */
	uint8_t reserved23;
	/* [한국어] 패딩. */

	/**
	 * Time in msec to wait until connection is done (0 = no timeout).
	 */
	uint32_t tcp_connect_timeout_ms;
	/* [한국어] NVMe/TCP connect() socket 시도 timeout. 0=무제한 (잘못 사용 시 connect hang). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_transport_opts) == 32, "Incorrect size");
/* [한국어] ABI guard 32B 검증. */

/**
 * Get the current NVMe transport options.
 *
 * \param[out] opts Will be filled with the current options for spdk_nvme_transport_set_opts().
 * \param opts_size Must be set to sizeof(struct spdk_nvme_transport_opts).
 */
/*
 * [한국어]
 * spdk_nvme_transport_get_opts - 현재 글로벌 트랜스포트 옵션 읽기.
 */
void spdk_nvme_transport_get_opts(struct spdk_nvme_transport_opts *opts, size_t opts_size);

/**
 * Set the NVMe transport options.
 */
/*
 * [한국어]
 * spdk_nvme_transport_set_opts - 글로벌 트랜스포트 옵션 적용.
 *
 * 보통 spdk_app_start 직후, probe 호출 전 한 번만 호출. 이후 변경은 일부 옵션 reset 필요.
 * RDMA/TCP 환경에서 tuning 필수.
 */
int spdk_nvme_transport_set_opts(const struct spdk_nvme_transport_opts *opts, size_t opts_size);

#ifdef __cplusplus
}
#endif

#endif
