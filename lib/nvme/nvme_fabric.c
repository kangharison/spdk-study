/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe over Fabrics transport-independent functions
 */

/*
 * [한국어 설명] NVMe-oF 트랜스포트 공통 로직 (nvme_fabric.c)
 *
 * === 파일의 역할 ===
 * NVMe-over-Fabrics (NVMe-oF) — RDMA, TCP, Fibre Channel, vfio-user 등 PCIe가 아닌
 * 트랜스포트들이 공통으로 사용하는 **트랜스포트 독립적(transport-independent) 로직**을 모은 파일.
 * PCIe 컨트롤러는 BAR을 mmap하여 CC/CSTS/AQA 등을 직접 MMIO로 R/W하지만, NVMe-oF에서는 같은 일을
 * **Fabric Property Set/Get 커맨드**(`SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET/GET` opcode)로
 * 메시지 기반 처리한다 — 이 파일이 그 변환 계층이다.
 *
 * 5대 책임:
 *   1) **Property Set/Get** (= NVMe-oF용 레지스터 R/W): 컨트롤러 레지스터(CC/CSTS/AQA/CAP/...)를
 *      Fabric 커맨드로 변환. 동기(sync, busy-wait)·비동기(async, callback) 양쪽 인터페이스 제공.
 *      이로써 nvme_ctrlr.c의 상태머신이 PCIe든 NVMe-oF든 동일하게 `nvme_transport_set/get_reg_*`만
 *      호출하면 트랜스포트별로 BAR MMIO 또는 Fabric 커맨드로 분기됨.
 *   2) **Discovery**: NVMe-oF 표준의 Discovery 서비스 — `nqn.2014-08.org.nvmexpress.discovery`
 *      서브시스템에 일시 연결하여 Discovery Log Page를 읽고, 그 결과로 발견된 각 NVM 서브시스템에
 *      대해 `nvme_ctrlr_probe()`를 재호출. `nvme_fabric_ctrlr_scan` + `nvme_fabric_ctrlr_discover`
 *      + `nvme_fabric_discover_probe` 3종이 이 흐름을 구성.
 *   3) **Fabric CONNECT (큐페어 수립)**: NVMe-oF의 큐페어는 PCIe의 단순한 SQ/CQ와 달리
 *      **CONNECT 커맨드를 명시적으로 보내** 컨트롤러와 큐페어를 결합해야 사용 가능. Admin 큐는
 *      cntlid=0xFFFF로 보내 controller 할당받고, I/O 큐는 받은 cntlid를 다시 첨부하여 보냄.
 *      `nvme_fabric_qpair_connect_async/poll/connect` 3종이 이 핸드셰이크를 처리.
 *   4) **AUTH 진입 판단**: CONNECT 응답의 `authreq.atr/ascr` 비트를 해석하여 인증이 필요한 경우
 *      `qpair->auth.flags`에 기록. 실제 인증(DH-CHAP)은 nvme_auth.c가 담당하지만 그 진입 결정은
 *      여기서 내려진다(`nvme_fabric_qpair_auth_required`).
 *   5) **수명주기 정리**: CONNECT 핸드셰이크 중 사용한 임시 status/dma 버퍼 해제
 *      (`nvme_fabric_qpair_poll_cleanup`), 인증 콜백 호출(`nvme_fabric_qpair_auth_cleanup`).
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe 드라이버 레이어 스택에서 nvme_transport.c (vtable 디스패치) 바로 아래, 그러나 트랜스포트별
 * 구체 파일(nvme_rdma.c / nvme_tcp.c / nvme_vfio_user.c) 위에 위치. 즉 **트랜스포트 ops 구현체들이
 * 공통 헬퍼로 호출하는 파일**이며, 자기 자신은 vtable에 직접 등록되지 않는다(직접 ops를 export하지
 * 않음). 대신 각 트랜스포트의 ops 함수가 내부적으로 이 파일의 `nvme_fabric_*`를 호출하는 구조.
 *
 * 호출 체인 (Property R/W):
 *   [상위: nvme_ctrlr_process_init / 상태머신]
 *     → nvme_transport_set_reg_4(ctrlr, OFF, val)        [nvme_transport.c]
 *         → ops.set_reg_4 (트랜스포트별 RDMA/TCP)         [nvme_rdma.c / nvme_tcp.c]
 *             → nvme_fabric_ctrlr_set_reg_4              [이 파일]
 *                 → nvme_fabric_prop_set_cmd_sync
 *                     → nvme_fabric_prop_set_cmd  ─┐
 *                     → nvme_wait_for_adminq_completion
 *
 * 호출 체인 (Discovery):
 *   [Application]
 *     → spdk_nvme_probe(trid where subnqn=DISCOVERY_NQN)
 *     → nvme_transport_ctrlr_scan
 *         → ops.ctrlr_scan (트랜스포트별)
 *             → nvme_fabric_ctrlr_scan                   [이 파일]
 *                 → nvme_transport_ctrlr_construct (임시 discovery_ctrlr)
 *                 → nvme_ctrlr_process_init (READY까지)
 *                 → nvme_ctrlr_cmd_identify (cdata)
 *                 → nvme_fabric_ctrlr_discover
 *                     → nvme_fabric_get_discovery_log_page (4KB씩 페이징)
 *                     → nvme_fabric_discover_probe (각 엔트리)
 *                         → nvme_ctrlr_probe (실제 NVM 서브시스템 attach)
 *
 * 호출 체인 (CONNECT):
 *   [상위: nvme_ctrlr_process_init 또는 IO qpair 생성]
 *     → nvme_transport_ctrlr_connect_qpair               [nvme_transport.c]
 *         → ops.ctrlr_connect_qpair                       [트랜스포트별]
 *             → nvme_fabric_qpair_connect (또는 connect_async + connect_poll)
 *                 → SPDK_NVMF_FABRIC_COMMAND_CONNECT 발사
 *                 → 응답에서 cntlid 추출(admin) + atr/ascr 플래그 저장
 *
 * === 타 모듈과의 연결 ===
 *  - **상위 호출자**: nvme_rdma.c / nvme_tcp.c / nvme_vfio_user.c (각 트랜스포트의 ops 함수가
 *    직접 호출), nvme_ctrlr.c (Discovery 흐름 진입), nvme_qpair.c (큐페어 CONNECT 시).
 *  - **하위 의존**: spdk_nvme_ctrlr_cmd_admin_raw (Property/Discovery 커맨드 발사),
 *    nvme_qpair_submit_request (CONNECT 직접 제출 — Admin Path가 아닌 Reserved Request 경로),
 *    nvme_wait_for_adminq_completion / nvme_wait_for_completion_poll (sync busy-wait),
 *    nvme_completion_poll_cb (sync 완료 콜백), nvme_ctrlr_probe (Discovery 재귀),
 *    nvme_transport_ctrlr_construct + nvme_ctrlr_process_init (Discovery 임시 컨트롤러).
 *  - **공유 자료구조**:
 *      * struct spdk_nvme_qpair::reserved_req — CONNECT용 사전 예약된 nvme_request 슬롯.
 *        일반 풀이 비어 있어도 CONNECT는 무조건 가능해야 하므로 별도 보존.
 *      * struct spdk_nvme_qpair::fabric_poll_status — async CONNECT의 폴링 상태.
 *        connect_async가 설정 → connect_poll이 소비/해제.
 *      * struct nvme_auth::flags (atr, ascr) — CONNECT 응답에서 추출된 인증 요구 플래그.
 *  - **NVMe-oF 와이어 포맷**: include/spdk/nvmf_spec.h의 `spdk_nvmf_fabric_*` 구조체들이
 *    실제 SQE 64바이트 레이아웃을 정의 (`spdk_nvmf_fabric_prop_set_cmd`,
 *    `spdk_nvmf_fabric_connect_cmd`, `spdk_nvmf_fabric_connect_data`, ...).
 *
 * === 주요 함수/구조체 요약 ===
 *  - struct nvme_fabric_prop_ctx — async Property 커맨드의 진행 상태 컨텍스트(value/size/cb).
 *  - nvme_fabric_prop_set/get_cmd — Fabric 커맨드 SQE 빌드 + raw admin 제출 (1개 단위).
 *  - nvme_fabric_prop_*_cmd_sync — busy-wait 동기 래퍼 (status calloc + wait_for_adminq).
 *  - nvme_fabric_prop_*_cmd_async — calloc(ctx) 후 cmd_done 콜백에서 사용자 cb_fn 호출.
 *  - nvme_fabric_ctrlr_set/get_reg_4/8(_async) — 8개의 공개 Property R/W 진입점
 *    (nvme_transport.c의 ops 디스패치가 도착하는 곳).
 *  - nvme_fabric_discover_probe — 1개의 Discovery Log 엔트리를 해석하여 nvme_ctrlr_probe 재호출.
 *  - nvme_fabric_get_discovery_log_page — 4KB 버퍼로 GET LOG PAGE 페이징.
 *  - nvme_fabric_ctrlr_scan — Discovery NQN이면 임시 컨트롤러 생성 + discover 호출,
 *    아니면 직접 nvme_ctrlr_probe.
 *  - nvme_fabric_ctrlr_discover — 4KB씩 Discovery Log 읽으며 numrec 만큼 entry 순회.
 *  - nvme_fabric_qpair_connect_async — CONNECT SQE 빌드 + reserved_req로 제출, status 보존.
 *  - nvme_fabric_qpair_connect_poll — CONNECT 응답 수신 후 cntlid 추출 + auth 플래그 기록.
 *  - nvme_fabric_qpair_connect — 위 두 함수를 묶은 동기 진입점(busy-wait 루프).
 *  - nvme_fabric_qpair_poll_cleanup / auth_cleanup — 임시 자원 해제 + auth cb 통지.
 *  - nvme_fabric_qpair_auth_required — atr/ascr/dhchap_key/auth.cb_fn 중 하나라도 true면 인증 필요.
 */

#include "nvme_internal.h"
/* [한국어] NVMe 드라이버 내부 헤더 — spdk_nvme_ctrlr/qpair, nvme_request, NVME_INIT_REQUEST,
 * nvme_completion_poll_status, nvme_wait_for_*_completion 등 fabric 로직이 의존하는 모든
 * 내부 자료구조와 헬퍼 매크로의 선언을 한꺼번에 가져오기 위함. */

#include "spdk/endian.h"
/* [한국어] from_le16/to_le32 등 little-endian 변환 매크로 — Discovery Log Page의 recfmt가
 * 와이어상 little-endian이므로 호스트 엔디언으로 변환할 때 사용. */
#include "spdk/string.h"
/* [한국어] spdk_strerror, spdk_str_chomp, spdk_strlen_pad — Discovery 엔트리의 traddr/trsvcid가
 * 공백으로 패딩된 NVMe 스타일 문자열이므로 trim/null-terminate 처리에 필요. */

/*
 * [한국어] struct nvme_fabric_prop_ctx — 비동기 Property Set/Get 커맨드의 진행 상태.
 *
 * Property 커맨드 자체는 Admin queue로 1개 SQE를 제출하고 1개 CQE를 받는 단순한 패턴이지만,
 * 비동기 API에서는 콜백이 실행될 때 (1) 사용자가 set한 value (set의 경우)와 (2) 응답에서 추출할
 * 필드 크기(get의 경우)를 알아야 한다. 이 구조체가 그 컨텍스트를 SQE의 cb_arg로 보존한다.
 *
 * 수명주기:
 *   - 할당: nvme_fabric_prop_set/get_cmd_async에서 calloc.
 *   - 보존: req->cb_arg = ctx로 nvme_request에 첨부됨.
 *   - 소비/해제: nvme_fabric_prop_set/get_cmd_done 콜백 내부에서 사용자 cb_fn 호출 후 free(ctx).
 *
 * 사용 컨텍스트: Admin 큐 폴링 스레드(즉 컨트롤러 프로세스의 admin qpair를 소유한 스레드).
 * Lockless 보장은 컨트롤러 admin qpair 단일 스레드 접근 가정에 기반. */
struct nvme_fabric_prop_ctx {
	uint64_t		value;
	/* [한국어] PROPERTY_SET일 때 호스트가 설정하려 한 64-bit 값. PROPERTY_GET 콜백 경로에서는
	 * 사용되지 않지만(응답 CQE에서 읽음) struct를 set/get 양쪽이 공유하므로 필드는 존재.
	 * SET 콜백(nvme_fabric_prop_set_cmd_done)이 사용자 cb_fn에 그대로 전달 — 일종의 "에코". */

	int			size;
	/* [한국어] PROPERTY_GET 응답을 해석할 때 4바이트 vs 8바이트 분기에 사용
	 * (SPDK_NVMF_PROP_SIZE_4 = 0, SPDK_NVMF_PROP_SIZE_8 = 1). 응답 CQE의 dword0/1 union 중
	 * 어느 부분을 읽을지 결정. SET 경로는 size를 사용하지 않으므로 ctx에 저장하지 않음. */

	spdk_nvme_reg_cb	cb_fn;
	/* [한국어] 사용자가 set/get_reg_*_async 호출 시 전달한 최종 콜백 함수.
	 * 시그니처: `void cb_fn(void *cb_arg, uint64_t value, const struct spdk_nvme_cpl *cpl)`.
	 * 내부 cmd_done이 CQE를 해석한 후 이 함수를 호출하면 사용자 콜백 체인 완료. */

	void			*cb_arg;
	/* [한국어] cb_fn에 그대로 전달되는 사용자 컨텍스트(opaque). SPDK는 이 값의 의미를 모름 —
	 * 호출자(예: nvme_ctrlr_process_init 상태머신)가 자기 컨텍스트를 식별하는 용도. */
};

/*
 * [한국어]
 * nvme_fabric_prop_set_cmd - NVMe-oF Property Set 커맨드 1개를 빌드하여 Admin 큐로 발사.
 *
 * @ctrlr: 대상 컨트롤러. admin qpair를 통해 발사되며 컨트롤러의 트랜스포트(RDMA/TCP/...)는 무관.
 * @offset: 컨트롤러 레지스터 공간 내 오프셋 (예: CC=0x14, CSTS=0x1C, AQA=0x24, ASQ=0x28).
 *          NVMe Base Spec의 "Controller Registers" 표 그대로의 오프셋. PCIe라면 BAR0+offset,
 *          NVMe-oF라면 fctype + offset 필드로 전송됨.
 * @size: SPDK_NVMF_PROP_SIZE_4(0)=4바이트 / SPDK_NVMF_PROP_SIZE_8(1)=8바이트. 레지스터의 폭에 맞춤.
 * @value: 쓸 값. 4바이트면 하위 32비트만 의미가 있음.
 * @cb_fn / @cb_arg: 완료 시 호출될 raw nvme cmd 콜백 (sync 래퍼면 nvme_completion_poll_cb,
 *                   async 래퍼면 nvme_fabric_prop_set_cmd_done이 들어옴).
 * @return: spdk_nvme_ctrlr_cmd_admin_raw의 반환값. 0이면 SQE 제출 성공, 음수면 풀 부족 등 실패.
 *
 * **NVMe-oF에서 레지스터 R/W를 메시지로 하는 이유**:
 * RDMA/TCP 등은 호스트가 장치 BAR을 직접 mmap할 수 없다. 따라서 호스트가 컨트롤러의 CC.EN을
 * 1로 만들고 싶다면 PCIe처럼 BAR write를 할 수 없고, 대신 "내가 offset=0x14에 4바이트로 X를
 * 쓰고 싶다"는 의도를 NVMe Fabric 명령 0x00(PROPERTY_SET) opcode 0x7F(FABRIC)로 보낸다.
 * 컨트롤러(target)는 그 명령을 받아 자기 내부 레지스터를 갱신.
 *
 * **SQE 레이아웃 매핑** (NVMe-oF 1.x Figure: Property Set Command):
 *   Byte 0       opcode  = 0x7F (SPDK_NVME_OPC_FABRIC)
 *   Byte 4       fctype  = 0x00 (SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET)
 *   DWord 10     ofst    = offset
 *   DWord 11.0   attrib.size = size (0 or 1)
 *   DWord 12-15  value.u64 (LSB first)
 *
 * 실행 컨텍스트: 호출 스레드(보통 컨트롤러 admin qpair 소유 스레드). admin qpair에 SQE 등록만
 * 하고 즉시 반환. 완료는 polling 시점에 비동기로.
 *
 * 호출 체인:
 *   (sync)  nvme_fabric_prop_set_cmd_sync → [이 함수] → spdk_nvme_ctrlr_cmd_admin_raw
 *   (async) nvme_fabric_prop_set_cmd_async → [이 함수] → spdk_nvme_ctrlr_cmd_admin_raw
 */
static int
nvme_fabric_prop_set_cmd(struct spdk_nvme_ctrlr *ctrlr,
			 uint32_t offset, uint8_t size, uint64_t value,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_fabric_prop_set_cmd cmd = {};
	/* [한국어] SQE 64바이트를 0으로 초기화한 로컬 변수 (스택). 모든 필수 필드를 명시적으로
	 * 채우고 나머지는 0인 상태로 admin_raw에 넘기면 내부에서 nvme_request에 memcpy됨. */

	assert(size == SPDK_NVMF_PROP_SIZE_4 || size == SPDK_NVMF_PROP_SIZE_8);
	/* [한국어] NVMe-oF 스펙은 Property R/W의 size로 4B/8B 두 값만 허용. 호출자가 잘못된 값을
	 * 넘기면 디버그 빌드에서 즉시 abort — 디버깅 단순화 (런타임에는 NO-OP). */

	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	/* [한국어] Admin opcode 0x7F (Fabric Command). 일반 Admin 커맨드와 구분하기 위한 1차 dispatch.
	 * 이 opcode를 본 컨트롤러는 다음으로 fctype 필드를 읽어 세부 fabric subcommand를 결정. */

	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET;
	/* [한국어] Fabric Command Type = 0x00 (Property Set). 0x01=CONNECT, 0x04=PROPERTY_GET,
	 * 0x05/0x06=AUTH SEND/RECV. 즉 같은 opcode 아래의 2차 dispatch. */

	cmd.ofst = offset;
	/* [한국어] 컨트롤러 레지스터 공간 내 오프셋. CC=0x14처럼 NVMe Base Spec 그대로의 값. */

	cmd.attrib.size = size;
	/* [한국어] attrib는 비트필드 union — size가 LSB 3비트, 나머지는 reserved. */

	cmd.value.u64 = value;
	/* [한국어] value는 union {uint32_t u32[2]; uint64_t u64;}. 64비트 한 번에 채움 —
	 * 4바이트 모드에서도 상위 32b는 컨트롤러가 무시. */

	return spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, (struct spdk_nvme_cmd *)&cmd,
					     NULL, 0, cb_fn, cb_arg);
	/* [한국어] Property Set은 데이터 전송이 없는 커맨드 — payload=NULL, payload_size=0.
	 * spdk_nvme_ctrlr_cmd_admin_raw가 ctrlr->ctrlr_lock을 잡고 admin qpair에 SQE를 큐잉한다.
	 * cmd 캐스트는 spdk_nvmf_fabric_prop_set_cmd가 내부적으로 spdk_nvme_cmd 64B와 호환되기
	 * 때문 (둘 다 SQE 와이어 포맷). 반환값은 ctrlr_lock 풀 부족(-ENOMEM) 등의 실패만 가능. */
}

/*
 * [한국어]
 * nvme_fabric_prop_set_cmd_sync - Property Set의 동기(busy-wait) 래퍼.
 *
 * @ctrlr / @offset / @size / @value: nvme_fabric_prop_set_cmd와 동일.
 * @return: 0=성공, -ENOMEM=status tracker 할당 실패, -1=제출 후 대기 도중 실패(타임아웃·CQE 에러).
 *
 * 동작 흐름:
 *   1) status calloc — admin 큐에 SQE를 넣고 폴링하면서 완료를 기다리는 동안 콜백이 set할
 *      "완료 플래그"가 필요. nvme_completion_poll_status가 그 역할.
 *   2) nvme_fabric_prop_set_cmd(cb=nvme_completion_poll_cb, cb_arg=status)로 SQE 제출.
 *      `nvme_completion_poll_cb`는 단순히 `status->done = true` + status->cpl 복사.
 *   3) nvme_wait_for_adminq_completion(robust=true)로 admin qpair를 폴링하면서 status->done이
 *      true가 될 때까지 또는 타임아웃까지 busy-wait. robust=true는 timed_out 시에도 status를
 *      이 함수가 free하지 않도록 함(나중에 비동기로 완료될 가능성 보존).
 *
 * 실행 컨텍스트: 컨트롤러 초기화 상태머신 (nvme_ctrlr_process_init) — 즉 admin qpair 단일 소유
 * 스레드. busy-wait 루프 동안 reactor를 점유하지만, 컨트롤러 부팅 단계라 acceptable.
 *
 * 사용 예: nvme_fabric_ctrlr_set_reg_4 → 이 함수 → nvme_fabric_prop_set_cmd. 상위에서
 * 트랜스포트가 set_reg_4를 sync로만 지원할 때(트랜스포트가 set_reg_4_async를 미구현한 경우)
 * 이 경로가 호출됨.
 *
 * 호출 체인:
 *   nvme_fabric_ctrlr_set_reg_4/8 → [이 함수] → nvme_fabric_prop_set_cmd → admin_raw
 *                                            → nvme_wait_for_adminq_completion (busy-wait)
 */
static int
nvme_fabric_prop_set_cmd_sync(struct spdk_nvme_ctrlr *ctrlr,
			      uint32_t offset, uint8_t size, uint64_t value)
{
	struct nvme_completion_poll_status *status;
	/* [한국어] sync busy-wait의 "완료 슬롯". done 플래그·cpl 복사본·timed_out 플래그·timeout_tsc·
	 * dma_data 포함. 콜백이 결과를 기록하면 메인 루프가 done을 polling. */
	int rc;

	status = calloc(1, sizeof(*status));
	/* [한국어] status를 stack이 아닌 heap에 둬야 하는 이유: timed_out 발생 시 status는 콜백이
	 * 늦게라도 도착할 가능성을 위해 free되지 않고 보존되며, 그 경우 함수가 반환된 후에도 콜백이
	 * 안전하게 접근 가능해야 함 (스택은 함수 반환 시 무효). */
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		/* [한국어] heap 부족. 컨트롤러 초기화 도중이라면 치명적이지만 호출자가 -ENOMEM을 보고
		 * graceful하게 fail하도록 -ENOMEM 반환. */
		return -ENOMEM;
	}

	rc = nvme_fabric_prop_set_cmd(ctrlr, offset, size, value,
				      nvme_completion_poll_cb, status);
	/* [한국어] SQE 제출. 콜백은 nvme_completion_poll_cb (nvme.c) — `status->done=true; status->cpl=*cpl;`
	 * 만 하는 단순 복사 콜백. 이걸로 polling 루프에서 결과를 안전하게 읽을 수 있다. */
	if (rc < 0) {
		free(status);
		/* [한국어] SQE 제출 자체가 실패(예: nvme_request 풀 고갈) → 콜백이 절대 호출되지 않으므로
		 * status를 즉시 free해도 안전. 그렇지 않으면 leak. */
		return rc;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	/* [한국어] admin qpair를 spdk_nvme_qpair_process_completions로 직접 폴링하면서 status->done
	 * 또는 timed_out을 기다림. 세 번째 인자 robust=true → timed_out 시 status를 자동 free하지 않고
	 * 호출자에게 책임 이전(여기서는 timeout 시 -1만 반환하므로 사실상 leak이지만 컨트롤러 초기화
	 * 실패 = 종료 경로라 허용). */
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_fabric_prop_set_cmd offset=%x, size=%u failed: rc=%s\n",
				  offset, size, spdk_strerror(abs(rc)));
		/* [한국어] timeout(-ETIMEDOUT)·CQE 에러 등을 단일 -1로 단순화. nvme_ctrlr_process_init은
		 * 0이 아니면 모두 실패 처리하므로 세부 오류 코드는 의미가 없음. spdk_strerror로 사람이
		 * 읽을 수 있는 에러 문자열을 로그로만 남김. */
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * nvme_fabric_prop_set_cmd_done - async Property Set의 내부 완료 콜백.
 *
 * @ctx: 사전 calloc된 nvme_fabric_prop_ctx 포인터 (cmd_async에서 등록).
 * @cpl: NVMe-oF 컨트롤러로부터 받은 CQE 16바이트.
 *
 * 이 함수는 nvme_qpair_process_completions이 admin qpair에서 CQE를 발견하면 호출된다.
 * 역할: 보존된 사용자 cb_fn에 (cb_arg, value, cpl)을 전달하고 ctx를 free.
 *
 * 왜 분리된 콜백이 필요한가: spdk_nvme_cmd_cb의 시그니처는 `(cb_arg, cpl)`만 받지만,
 * Property R/W의 사용자 API(spdk_nvme_reg_cb)는 `(cb_arg, uint64_t value, cpl)`이 필요.
 * 즉 한 단계 트램펄린이 필요하므로 ctx에 value를 보존하고 이 함수가 시그니처를 변환.
 *
 * 실행 컨텍스트: admin qpair를 polling 중인 reactor 스레드 (process_completions가 SQE를 fetch한
 * 시점에 동기 호출). 즉 사용자의 cb_fn 또한 같은 스레드에서 실행됨.
 *
 * 호출 체인:
 *   spdk_nvme_qpair_process_completions → nvme_complete_request
 *     → req->cb_fn = nvme_fabric_prop_set_cmd_done
 *     → 사용자 spdk_nvme_reg_cb (예: 상태머신의 next-state 진입 함수)
 */
static void
nvme_fabric_prop_set_cmd_done(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_fabric_prop_ctx *prop_ctx = ctx;
	/* [한국어] void * → 실제 타입으로 캐스트. SPDK 콜백 인터페이스는 모두 void *를 통해 컨텍스트를
	 * 운반 — 호출자와 콜백 사이의 type erasure. */

	prop_ctx->cb_fn(prop_ctx->cb_arg, prop_ctx->value, cpl);
	/* [한국어] 사용자 콜백 호출 — Property SET이므로 value는 호스트가 보낸 값을 그대로 전달
	 * (CQE에 포함되지 않으므로 ctx에 보존했던 값). cpl은 status field에 sct/sc 포함. 사용자는
	 * spdk_nvme_cpl_is_error로 성공/실패 판단. */
	free(prop_ctx);
	/* [한국어] async ctx 수명 종료 — calloc된 nvme_fabric_prop_ctx 해제. 사용자가 cb_fn 안에서
	 * 또 비동기를 걸어도 안전(prop_ctx 자체는 더 이상 누구도 참조 안 함). */
}

/*
 * [한국어]
 * nvme_fabric_prop_set_cmd_async - Property Set의 비동기 진입점.
 *
 * @ctrlr / @offset / @size / @value: nvme_fabric_prop_set_cmd와 동일.
 * @cb_fn: 사용자 콜백 (spdk_nvme_reg_cb 시그니처).
 * @cb_arg: 사용자 콜백 인자.
 * @return: 0=SQE 제출 성공(완료는 콜백으로), 음수=ctx 할당 실패 또는 admin_raw 실패.
 *
 * 비동기 Property Set은 sync와 달리 호출자를 즉시 반환시키므로, 사용자 cb_fn으로 결과를 통지하기
 * 위해 (1) 컨텍스트 calloc, (2) value/cb_fn/cb_arg 보존, (3) 내부 트램펄린 콜백 등록.
 *
 * 사용 사례: nvme_ctrlr_process_init이 비동기 트랜스포트(트랜스포트가 set_reg_*_async를
 * 정식 구현)에서 CC=...를 쓸 때. 이 경우 reactor가 다른 작업(다른 컨트롤러의 polling 등)을
 * 병행할 수 있어 멀티 컨트롤러 셋업이 빨라짐.
 *
 * 에러 경로:
 *   - calloc 실패 → 즉시 -ENOMEM 반환, 사용자 cb 호출 안 됨 (호출자 책임).
 *   - admin_raw 실패 → 콜백 미등록 → ctx free + 음수 반환, 사용자 cb 호출 안 됨.
 *
 * 호출 체인:
 *   nvme_fabric_ctrlr_set_reg_4/8_async → [이 함수] → nvme_fabric_prop_set_cmd → admin_raw
 *     (완료) → ... → process_completions → nvme_fabric_prop_set_cmd_done → 사용자 cb_fn
 */
static int
nvme_fabric_prop_set_cmd_async(struct spdk_nvme_ctrlr *ctrlr,
			       uint32_t offset, uint8_t size, uint64_t value,
			       spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	struct nvme_fabric_prop_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	/* [한국어] async 트램펄린 컨텍스트 할당. 콜백이 시작될 때까지 살아 있어야 함. */
	if (ctx == NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate fabrics property context\n");
		return -ENOMEM;
	}

	ctx->value = value;
	/* [한국어] SET이므로 사용자가 전달한 value를 그대로 콜백에 echo back. */
	ctx->cb_fn = cb_fn;
	/* [한국어] 사용자 spdk_nvme_reg_cb 보존. cmd_done이 이 함수를 호출할 것. */
	ctx->cb_arg = cb_arg;
	/* [한국어] 사용자 opaque 컨텍스트 보존. */

	rc = nvme_fabric_prop_set_cmd(ctrlr, offset, size, value,
				      nvme_fabric_prop_set_cmd_done, ctx);
	/* [한국어] SQE 제출. 콜백은 트램펄린(set_cmd_done) — 그 안에서 ctx->cb_fn을 호출. */
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to send Property Set fabrics command\n");
		free(ctx);
		/* [한국어] 제출 실패 → 콜백이 절대 호출되지 않음 → ctx free 책임은 여기 있음. */
	}

	return rc;
}

/*
 * [한국어]
 * nvme_fabric_prop_get_cmd - NVMe-oF Property Get 커맨드 1개를 빌드하여 Admin 큐로 발사.
 *
 * @ctrlr / @offset / @size: Set과 동일.
 * @cb_fn / @cb_arg: 완료 콜백. CQE의 dword0/1에 응답 값이 담겨 옴.
 * @return: spdk_nvme_ctrlr_cmd_admin_raw의 반환값.
 *
 * Set과 거의 동일한 SQE 빌드이지만 다음 차이:
 *   1) fctype = 0x04 (PROPERTY_GET)
 *   2) value 필드 사용 안 함(빈 채로 전송). 응답이 CQE의 result 필드(dword0/1)로 도달.
 *   3) 같은 spdk_nvmf_fabric_prop_set_cmd 구조체를 재사용 — 와이어 포맷이 동일하므로 별도
 *      get_cmd 구조체를 두지 않고 set_cmd 타입을 캐스트로 활용.
 *
 * 호출 체인:
 *   (sync)  nvme_fabric_prop_get_cmd_sync → [이 함수] → admin_raw
 *   (async) nvme_fabric_prop_get_cmd_async → [이 함수] → admin_raw
 */
static int
nvme_fabric_prop_get_cmd(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint8_t size,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_fabric_prop_set_cmd cmd = {};
	/* [한국어] Set과 동일한 64B SQE 구조체 재사용 — value 필드만 0으로 둠. NVMe-oF 스펙도
	 * Property Set/Get의 SQE 레이아웃이 거의 동일(value 사용 여부만 차이). */

	assert(size == SPDK_NVMF_PROP_SIZE_4 || size == SPDK_NVMF_PROP_SIZE_8);
	/* [한국어] 4B/8B만 허용 — Set과 동일한 spec 제약. */

	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	/* [한국어] Admin opcode 0x7F. */
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET;
	/* [한국어] Fabric subtype 0x04 — Property Get. Set(0x00)과의 유일한 차이점. */
	cmd.ofst = offset;
	/* [한국어] 읽을 레지스터 오프셋. */
	cmd.attrib.size = size;
	/* [한국어] 응답에서 4B/8B 중 어느 폭으로 보낼지 컨트롤러에 알림 — 응답 CQE의 result에서
	 * 클라이언트가 어느 부분을 사용할지 결정의 근거. */

	return spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, (struct spdk_nvme_cmd *)&cmd,
					     NULL, 0, cb_fn, cb_arg);
	/* [한국어] payload 없음. 응답 데이터(읽힌 레지스터 값)는 CQE의 dword0/1에 담겨 옴 —
	 * 별도 SGL/PRP 영역이 아니라 CQE 자체의 result 필드를 사용. */
}

/*
 * [한국어]
 * nvme_fabric_prop_get_cmd_sync - Property Get의 동기(busy-wait) 래퍼.
 *
 * @ctrlr / @offset / @size: Set과 동일.
 * @value: OUT — 성공 시 컨트롤러로부터 읽은 레지스터 값을 담는 포인터.
 * @return: 0=성공, -ENOMEM=tracker 할당 실패, -1=대기 중 실패.
 *
 * Set sync 래퍼와 큰 흐름은 같지만 **응답 처리** 단계에서 차이:
 *   - Set은 응답 CQE에 status.sct/sc만 보면 충분.
 *   - Get은 CQE의 status뿐 아니라 result 필드(dword0/1)에서 실제 레지스터 값을 추출해야 함 →
 *     status->cpl을 spdk_nvmf_fabric_prop_get_rsp로 캐스트해서 value union 접근.
 *
 * 주의 — robust=false 경로:
 *   nvme_wait_for_adminq_completion(robust=false) → timed_out 발생 시 status는 자동 free되지
 *   않고 호출자(이 함수)가 free 결정. 그래서 `if (!status->timed_out) free(status);`로
 *   timed_out일 때만 leak 허용. timed_out이면 콜백이 늦게라도 도착할 수 있고 그때 status에
 *   접근하므로 free하면 use-after-free.
 *
 * 호출 체인:
 *   nvme_fabric_ctrlr_get_reg_4/8 → [이 함수] → nvme_fabric_prop_get_cmd → admin_raw
 *                                            → nvme_wait_for_adminq_completion (busy-wait)
 *                                            → response 추출 → *value = ...
 */
static int
nvme_fabric_prop_get_cmd_sync(struct spdk_nvme_ctrlr *ctrlr,
			      uint32_t offset, uint8_t size, uint64_t *value)
{
	struct nvme_completion_poll_status *status;
	/* [한국어] sync busy-wait 트래커. set sync와 동일한 패턴. */
	struct spdk_nvmf_fabric_prop_get_rsp response;
	/* [한국어] CQE 16B를 응답 구조체로 재해석한 로컬 사본 — status->cpl을 캐스트한 결과를
	 * 담아 free 후에도 안전하게 읽을 수 있게 함. */
	int rc;

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	rc = nvme_fabric_prop_get_cmd(ctrlr, offset, size, nvme_completion_poll_cb, status);
	/* [한국어] Property Get SQE 제출 + nvme_completion_poll_cb 콜백 등록. */
	if (rc < 0) {
		free(status);
		return rc;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, false);
	/* [한국어] busy-wait. 마지막 인자 robust=false → timed_out에서 자동 free되지 않으므로
	 * 아래의 명시적 free 분기 필요. 이는 status->cpl을 free 전에 안전히 읽을 수 있도록 하기 위함. */
	response = *(struct spdk_nvmf_fabric_prop_get_rsp *)&status->cpl;
	/* [한국어] free 직전에 status->cpl을 응답 구조체로 캐스트하여 로컬 변수에 복사. CQE 16바이트가
	 * 그대로 prop_get_rsp의 와이어 포맷과 일치하므로 단순 type punning으로 가능. */
	if (!status->timed_out) {
		free(status);
		/* [한국어] timed_out=false면 콜백이 이미 다녀갔다는 의미 → 안전하게 free.
		 * timed_out=true면 콜백이 늦게 도착할 수 있으므로 누가 책임지고 free? — 이 코드 경로에서는
		 * 사실상 leak. 그러나 timeout = 컨트롤러 비정상 = 정리 안전성 우선이라는 스펙 절충. */
	}

	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_fabric_prop_get_cmd offset=%x, size=%u failed: rc=%s\n",
				  offset, size, spdk_strerror(abs(rc)));
		return -1;
	}

	if (size == SPDK_NVMF_PROP_SIZE_4) {
		*value = response.value.u32.low;
		/* [한국어] 4바이트 모드 → CQE의 result dword0(=value.u32.low)에 32비트 결과만 의미.
		 * 호출자는 uint32_t 결과만 받음(get_reg_4 래퍼가 (uint32_t)cast). */
	} else {
		*value = response.value.u64;
		/* [한국어] 8바이트 모드 → result dword0/1 합쳐서 64비트 결과. */
	}

	return 0;
}

/*
 * [한국어]
 * nvme_fabric_prop_get_cmd_done - async Property Get의 내부 완료 콜백.
 *
 * @ctx: nvme_fabric_prop_get_cmd_async에서 calloc한 prop_ctx (size 필드가 채워져 있음).
 * @cpl: NVMe-oF CQE 16바이트 (status + result 포함).
 *
 * Set의 cmd_done과 비슷한 트램펄린이지만 응답 추출 단계가 추가:
 *   - 성공 시: cpl을 prop_get_rsp로 캐스트 → ctx->size에 따라 4B/8B로 value 추출.
 *   - 실패 시: value=0으로 사용자 cb에 전달(에러는 cpl로 보고).
 *
 * 사용자 cb_fn 호출 후 ctx free.
 *
 * 실행 컨텍스트: admin qpair 폴링 스레드 (process_completions).
 */
static void
nvme_fabric_prop_get_cmd_done(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_fabric_prop_ctx *prop_ctx = ctx;
	struct spdk_nvmf_fabric_prop_get_rsp *response;
	uint64_t value = 0;
	/* [한국어] 기본값 0 — 에러 시에는 사용자 cb에 0을 전달. cpl로 에러 정보가 같이 가므로
	 * 0은 단순 placeholder(사용자가 cpl 검사 안 하고 value만 보면 안 됨). */

	if (spdk_nvme_cpl_is_success(cpl)) {
		/* [한국어] CQE의 status field(sct/sc)가 모두 0이어야 SUCCESS. 에러면 result는 무의미. */
		response = (struct spdk_nvmf_fabric_prop_get_rsp *)cpl;
		/* [한국어] CQE를 응답 구조체로 type pun. 와이어 포맷 호환성 덕분에 안전. */

		switch (prop_ctx->size) {
		case SPDK_NVMF_PROP_SIZE_4:
			value = response->value.u32.low;
			/* [한국어] 4B 폭 — 32비트 결과만 의미. */
			break;
		case SPDK_NVMF_PROP_SIZE_8:
			value = response->value.u64;
			/* [한국어] 8B 폭 — 64비트 결과 전체. */
			break;
		default:
			assert(0 && "Should never happen");
			/* [한국어] cmd_async에서 size 검증을 거쳤으므로 도달 불가능 — 디버그 빌드 trap.
			 * 릴리즈에서는 value=0 그대로 사용자 cb로 전달. */
		}
	}

	prop_ctx->cb_fn(prop_ctx->cb_arg, value, cpl);
	/* [한국어] 사용자 spdk_nvme_reg_cb 호출. 사용자는 cpl로 성공/실패 판단 후 value 사용. */
	free(prop_ctx);
	/* [한국어] async ctx 수명 종료. */
}

/*
 * [한국어]
 * nvme_fabric_prop_get_cmd_async - Property Get의 비동기 진입점.
 *
 * @ctrlr / @offset / @size: nvme_fabric_prop_get_cmd와 동일.
 * @cb_fn / @cb_arg: 사용자 콜백.
 * @return: 0=성공(완료는 콜백), 음수=ctx 할당/제출 실패.
 *
 * Set async와 거의 동일한 패턴이지만 ctx에 보존하는 정보가 다름:
 *   - Set async: ctx에 value 보존(콜백에서 echo back).
 *   - Get async: ctx에 size 보존(콜백에서 응답 폭 결정).
 *
 * 호출 체인:
 *   nvme_fabric_ctrlr_get_reg_4/8_async → [이 함수] → nvme_fabric_prop_get_cmd → admin_raw
 *     (완료) → ... → process_completions → nvme_fabric_prop_get_cmd_done → 사용자 cb_fn
 */
static int
nvme_fabric_prop_get_cmd_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint8_t size,
			       spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	struct nvme_fabric_prop_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate fabrics property context\n");
		return -ENOMEM;
	}

	ctx->size = size;
	/* [한국어] Get은 size를 콜백에서 응답 폭 분기에 사용 — value는 응답에서 추출하므로 보존 불필요. */
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	rc = nvme_fabric_prop_get_cmd(ctrlr, offset, size, nvme_fabric_prop_get_cmd_done, ctx);
	/* [한국어] SQE 제출 + 트램펄린 콜백 등록. */
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to send Property Get fabrics command\n");
		free(ctx);
		/* [한국어] 제출 실패 → 콜백 미실행 → ctx free. */
	}

	return rc;
}

/*
 * [한국어]
 * 이하 8개 함수는 NVMe-oF 트랜스포트(RDMA/TCP/vfio-user)의 ops 함수로부터 호출되는 공개 API
 * 4쌍(4B/8B × set/get × sync/async)이다. 모두 단순 어댑터 — size 인자만 PROP_SIZE_4/8로
 * 결정한 뒤 핵심 헬퍼(prop_*_cmd_sync/async)에 위임.
 *
 * 트랜스포트 ops 디스패치 경로:
 *   nvme_transport_set_reg_4(ctrlr, off, val)  [nvme_transport.c]
 *     → ops.set_reg_4 (RDMA/TCP/vfio-user)
 *         → nvme_fabric_ctrlr_set_reg_4         [이 함수]
 *
 * sync vs async 선택: 트랜스포트가 set_reg_*_async를 정식 구현했는가에 달림.
 * - 미구현이면 nvme_transport.c의 fallback 로직이 sync 변형을 호출 → 이 파일 sync 함수 → busy-wait.
 * - 구현이면 호출자(예: nvme_ctrlr_process_init 비동기 상태머신)가 직접 *_async를 호출 → 이 파일 async 함수.
 */

/*
 * [한국어]
 * nvme_fabric_ctrlr_set_reg_4 - 4바이트 레지스터 동기 쓰기. (예: CC, CSTS, AQA의 하위)
 *
 * @ctrlr / @offset / @value: 레지스터 위치와 쓸 값.
 * @return: 0=성공, 음수=실패.
 *
 * 사용 예: nvme_ctrlr_process_init이 CC.EN=1로 설정 (offset=0x14, 4바이트).
 */
int
nvme_fabric_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	return nvme_fabric_prop_set_cmd_sync(ctrlr, offset, SPDK_NVMF_PROP_SIZE_4, value);
	/* [한국어] PROP_SIZE_4(=0)로 sync 헬퍼 위임. value는 자동으로 uint32_t→uint64_t promotion. */
}

/*
 * [한국어]
 * nvme_fabric_ctrlr_set_reg_8 - 8바이트 레지스터 동기 쓰기. (예: ASQ, ACQ — 64-bit DMA 주소)
 *
 * 사용 예: ASQ(Admin SQ Base) offset=0x28에 admin SQ의 hugepage 물리 주소를 64비트로 기록.
 */
int
nvme_fabric_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	return nvme_fabric_prop_set_cmd_sync(ctrlr, offset, SPDK_NVMF_PROP_SIZE_8, value);
	/* [한국어] PROP_SIZE_8(=1)로 sync 헬퍼 위임. */
}

/*
 * [한국어]
 * nvme_fabric_ctrlr_get_reg_4 - 4바이트 레지스터 동기 읽기.
 *
 * @value: OUT — 성공 시 컨트롤러 레지스터의 32-bit 값.
 *
 * 내부 헬퍼는 64-bit를 다루므로 uint64_t tmp_value로 받은 후 uint32_t로 캐스트하여 반환.
 * 상위 32비트는 컨트롤러가 0으로 보내야 정상이지만 방어적으로 캐스트.
 */
int
nvme_fabric_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	uint64_t tmp_value;
	/* [한국어] sync 헬퍼는 uint64_t* 인터페이스이므로 임시 변수 필요. */
	int rc;
	rc = nvme_fabric_prop_get_cmd_sync(ctrlr, offset, SPDK_NVMF_PROP_SIZE_4, &tmp_value);
	/* [한국어] 4B 폭으로 sync 호출 — 헬퍼 내부에서 응답의 u32.low만 tmp_value에 담아 옴. */

	if (!rc) {
		*value = (uint32_t)tmp_value;
		/* [한국어] 성공 시에만 호출자 버퍼에 캐스트 저장. 실패 시 *value 미정의로 두어
		 * 호출자가 rc를 검사하도록 강제 (오염된 값 사용 방지). */
	}
	return rc;
}

/*
 * [한국어]
 * nvme_fabric_ctrlr_get_reg_8 - 8바이트 레지스터 동기 읽기. (예: CAP — 컨트롤러 capabilities)
 *
 * 사용 예: nvme_ctrlr_process_init이 부팅 시 CAP(offset=0x00) 64비트를 읽어 MQES, DSTRD, CSS,
 * MPSMIN 등을 확인. NVMe-oF에서도 CAP 의미는 동일하지만 DSTRD가 무의미(BAR 없음).
 */
int
nvme_fabric_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	return nvme_fabric_prop_get_cmd_sync(ctrlr, offset, SPDK_NVMF_PROP_SIZE_8, value);
	/* [한국어] 단순 위임 — 8B 폭 그대로 호출자 버퍼로. */
}

/*
 * [한국어]
 * nvme_fabric_ctrlr_set_reg_4_async - 4바이트 레지스터 비동기 쓰기.
 *
 * @cb_fn / @cb_arg: 완료 시 호출될 사용자 콜백.
 *
 * 비동기 상태머신(spdk_nvme_ctrlr_process_init_async)에서 사용 — 한 컨트롤러를 polling하면서도
 * 다른 컨트롤러나 reactor 작업과 병행 가능.
 */
int
nvme_fabric_ctrlr_set_reg_4_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
				  uint32_t value, spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	return nvme_fabric_prop_set_cmd_async(ctrlr, offset, SPDK_NVMF_PROP_SIZE_4, value,
					      cb_fn, cb_arg);
	/* [한국어] async 헬퍼에 위임 — 내부에서 ctx 할당 + 트램펄린 콜백 등록. */
}

/*
 * [한국어]
 * nvme_fabric_ctrlr_set_reg_8_async - 8바이트 레지스터 비동기 쓰기.
 */
int
nvme_fabric_ctrlr_set_reg_8_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
				  uint64_t value, spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	return nvme_fabric_prop_set_cmd_async(ctrlr, offset, SPDK_NVMF_PROP_SIZE_8, value,
					      cb_fn, cb_arg);
	/* [한국어] async 헬퍼에 위임. */
}

/*
 * [한국어]
 * nvme_fabric_ctrlr_get_reg_4_async - 4바이트 레지스터 비동기 읽기.
 *
 * 사용자 cb_fn은 spdk_nvme_reg_cb 시그니처(value, cpl) — 사용자는 value를 (uint32_t)로 truncate.
 * 4B vs 8B 분기는 ctx->size로 콜백에서 처리되므로 호출자는 신경 쓸 필요 없음.
 */
int
nvme_fabric_ctrlr_get_reg_4_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
				  spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	return nvme_fabric_prop_get_cmd_async(ctrlr, offset, SPDK_NVMF_PROP_SIZE_4, cb_fn, cb_arg);
	/* [한국어] async 헬퍼에 위임. */
}

/*
 * [한국어]
 * nvme_fabric_ctrlr_get_reg_8_async - 8바이트 레지스터 비동기 읽기.
 */
int
nvme_fabric_ctrlr_get_reg_8_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
				  spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	return nvme_fabric_prop_get_cmd_async(ctrlr, offset, SPDK_NVMF_PROP_SIZE_8, cb_fn, cb_arg);
	/* [한국어] async 헬퍼에 위임. */
}

/*
 * [한국어]
 * nvme_fabric_discover_probe - 1개의 Discovery Log 엔트리를 해석하여 nvme_ctrlr_probe 재호출.
 *
 * @entry: Discovery Log Page에서 가져온 1개 엔트리 (1024바이트 SPDK_NVMF_DISCOVERY_LOG_PAGE_ENTRY).
 * @probe_ctx: probe 결과를 누적하는 상위 컨텍스트 (사용자 probe_cb/attach_cb를 보유).
 * @discover_priority: discovery 컨트롤러의 trid.priority — 발견된 컨트롤러도 같은 우선순위 상속.
 *
 * 동작 요약:
 *   1) subtype 검사 — DISCOVERY/DISCOVERY_CURRENT는 referral(다른 discovery로 점프)이라 미지원이므로 skip.
 *      NVME(0x2) 서브시스템만 처리.
 *   2) trtype 검사 — 발견된 트랜스포트(RDMA/TCP/FC/...)가 현재 빌드에 컴파일되어 있는지(transport_available)
 *      확인. 없으면 skip.
 *   3) NQN/주소 정규화 — 와이어 포맷이 공백 패딩 또는 NUL termination이 모호한 경우가 있어
 *      strlen_pad/str_chomp/null-terminate로 정리. trid 구조체에는 항상 C 스타일 NUL-terminated.
 *   4) trid 완성 후 nvme_ctrlr_probe 재호출 → 일반 probe 경로로 이 NVM 서브시스템 attach 시도.
 *
 * 호출 컨텍스트: nvme_fabric_ctrlr_discover의 페이지 순회 루프에서 매 엔트리마다 호출 → 같은
 * 스레드(보통 application 스레드 또는 reactor)에서 실행.
 *
 * 호출 체인:
 *   nvme_fabric_ctrlr_discover (페이지 순회) → [이 함수] (엔트리 해석) → nvme_ctrlr_probe (attach 시도)
 */
static void
nvme_fabric_discover_probe(struct spdk_nvmf_discovery_log_page_entry *entry,
			   struct spdk_nvme_probe_ctx *probe_ctx,
			   int discover_priority)
{
	struct spdk_nvme_transport_id trid;
	/* [한국어] probe에 넘길 트랜스포트 ID 구조체 — trtype/adrfam/traddr/trsvcid/subnqn 등. */
	uint8_t *end;
	size_t len;

	memset(&trid, 0, sizeof(trid));
	/* [한국어] trid 0 초기화 — 미사용 필드(예: trtype_str, hostsvcid 등)를 NUL/0으로 보장. */

	if (entry->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT ||
	    entry->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		/* [한국어] DISCOVERY_CURRENT(0x3, NVMe-oF 1.1 추가) / DISCOVERY(0x1) — 이 엔트리가
		 * "다른 discovery 서비스를 가리키는 referral"이라는 의미. SPDK는 다단계 referral을 지원하지
		 * 않으므로 skip + WARN. */
		SPDK_WARNLOG("Skipping unsupported current discovery service or"
			     " discovery service referral\n");
		return;
	} else if (entry->subtype != SPDK_NVMF_SUBTYPE_NVME) {
		/* [한국어] 0x2(NVME) 외 알 수 없는 서브타입 — 향후 스펙 확장 대비 안전한 skip. */
		SPDK_WARNLOG("Skipping unknown subtype %u\n", entry->subtype);
		return;
	}

	trid.trtype = entry->trtype;
	/* [한국어] 발견된 NVM 서브시스템의 트랜스포트 enum (RDMA/TCP/FC/PCIE 등). */
	spdk_nvme_transport_id_populate_trstring(&trid, spdk_nvme_transport_id_trtype_str(entry->trtype));
	/* [한국어] enum → 문자열("RDMA"/"TCP"/...) 변환 후 trid.trstring에 채움. trstring은
	 * nvme_transport.c가 ops를 조회할 때 사용하는 키. */
	if (!spdk_nvme_transport_available_by_name(trid.trstring)) {
		/* [한국어] 컴파일 시 해당 트랜스포트가 빌드되었는지 확인 — 없으면 ops가 등록되지 않았으므로
		 * probe 시도해도 실패. 미리 skip + WARN. */
		SPDK_WARNLOG("NVMe transport type %u not available; skipping probe\n",
			     trid.trtype);
		return;
	}

	trid.adrfam = entry->adrfam;
	/* [한국어] 주소 패밀리 (IPv4/IPv6/IB/FC). transport별로 의미가 다름. */

	/* Ensure that subnqn is null terminated. */
	end = memchr(entry->subnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1);
	/* [한국어] NQN(NVMe Qualified Name)은 최대 223바이트 + NUL — 와이어상 NUL 종결 보장이 약하므로
	 * memchr로 NUL 위치 탐색. NUL이 없으면 잘못된 엔트리이므로 skip. */
	if (!end) {
		SPDK_ERRLOG("Discovery entry SUBNQN is not null terminated\n");
		return;
	}
	len = end - entry->subnqn;
	/* [한국어] 실제 NQN 길이 (NUL 제외). */
	memcpy(trid.subnqn, entry->subnqn, len);
	trid.subnqn[len] = '\0';
	/* [한국어] 잘라서 trid에 복사 + 안전한 NUL 종결. */

	/* Convert traddr to a null terminated string. */
	len = spdk_strlen_pad(entry->traddr, sizeof(entry->traddr), ' ');
	/* [한국어] traddr는 우측 공백 패딩 가능(NVMe 와이어 표준) — strlen_pad로 trailing space를 제거한
	 * 실제 길이 산출. */
	memcpy(trid.traddr, entry->traddr, len);
	/* [한국어] 패딩 제거된 실제 부분만 trid에 복사. (sizeof(trid.traddr)는 자동 NUL 보장하지 않으므로
	 * len 이후에 0이 들어 있어야 — trid를 처음에 memset한 덕분에 안전.) */
	if (spdk_str_chomp(trid.traddr) != 0) {
		SPDK_DEBUGLOG(nvme, "Trailing newlines removed from discovery TRADDR\n");
		/* [한국어] 일부 타깃이 \r\n을 끝에 붙여 보내는 경우 정리 — chomp가 제거 시 리턴 != 0. */
	}

	/* Convert trsvcid to a null terminated string. */
	len = spdk_strlen_pad(entry->trsvcid, sizeof(entry->trsvcid), ' ');
	/* [한국어] trsvcid(서비스 ID — TCP/IP의 포트, FC의 N_Port_ID 등)도 동일한 패턴으로 정리. */
	memcpy(trid.trsvcid, entry->trsvcid, len);
	if (spdk_str_chomp(trid.trsvcid) != 0) {
		SPDK_DEBUGLOG(nvme, "Trailing newlines removed from discovery TRSVCID\n");
	}

	SPDK_DEBUGLOG(nvme, "subnqn=%s, trtype=%u, traddr=%s, trsvcid=%s\n",
		      trid.subnqn, trid.trtype,
		      trid.traddr, trid.trsvcid);
	/* [한국어] 디버그 트레이스 — discovery로 발견된 attachable 컨트롤러의 전체 식별자 출력. */

	/* Copy the priority from the discovery ctrlr */
	trid.priority = discover_priority;
	/* [한국어] discovery 컨트롤러에 설정된 priority가 발견된 컨트롤러로 상속.
	 * priority는 multipath 정책에서 path 선택의 가중치로 사용. */

	nvme_ctrlr_probe(&trid, probe_ctx, NULL);
	/* [한국어] 일반 probe 경로 진입 — 트랜스포트 ops.ctrlr_construct를 거쳐 실제 컨트롤러 attach.
	 * 마지막 인자 NULL = devhandle 없음 (NVMe-oF에선 PCIe 디바이스 핸들이 의미 없음). */
}

/*
 * [한국어]
 * nvme_fabric_get_discovery_log_page - Discovery Log Page를 동기로 size바이트만큼 읽어옴.
 *
 * @ctrlr: discovery 컨트롤러 (subnqn=DISCOVERY_NQN인 임시 컨트롤러).
 * @log_page: OUT 버퍼 (호출자 스택 buffer[4096]).
 * @size: 읽을 바이트 수.
 * @offset: Discovery Log Page 내 읽기 시작 오프셋. 큰 페이지를 4KB씩 페이징 읽기 가능.
 * @return: 0=성공, -ENOMEM=tracker 할당 실패, -1=제출/대기 실패.
 *
 * GET LOG PAGE Admin 커맨드 (LID=0x70 = SPDK_NVME_LOG_DISCOVERY) 발사 → 동기 대기.
 * Discovery Log Page는 첫 16바이트 헤더(genctr, numrec, recfmt, ...) 뒤에 entries 배열이 옴.
 *
 * 호출자 nvme_fabric_ctrlr_discover가 4KB 단위로 반복 호출하면서 numrec 만큼의 엔트리를 모두
 * 가져올 때까지 페이징.
 */
static int
nvme_fabric_get_discovery_log_page(struct spdk_nvme_ctrlr *ctrlr,
				   void *log_page, uint32_t size, uint64_t offset)
{
	struct nvme_completion_poll_status *status;
	int rc;

	status = calloc(1, sizeof(*status));
	/* [한국어] sync busy-wait의 완료 슬롯. */
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_DISCOVERY, 0, log_page, size, offset,
					      nvme_completion_poll_cb, status);
	/* [한국어] GET LOG PAGE 제출 — LID=DISCOVERY, NSID=0(컨트롤러 글로벌), payload=log_page,
	 * length=size, offset=offset. 콜백은 sync 완료 트래커. */
	if (rc < 0) {
		free(status);
		return -1;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	/* [한국어] busy-wait — robust=true는 위 prop_set과 같은 의미 (timed_out 시 자동 free 안 함). */
	if (rc) {
		SPDK_ERRLOG("wait for spdk_nvme_ctrlr_cmd_get_log_page failed: rc=%s\n", spdk_strerror(abs(rc)));
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * nvme_fabric_ctrlr_scan - NVMe-oF 트랜스포트의 ctrlr_scan ops 구현 (트랜스포트별 ops가 위임).
 *
 * @probe_ctx: 사용자가 spdk_nvme_probe()로 시작한 probe 세션. probe_ctx->trid가 검색 대상.
 * @direct_connect: true=spdk_nvme_connect() 경로(특정 컨트롤러를 바로 attach), false=spdk_nvme_probe()
 *                  경로(discovery 후 entries 순회).
 * @return: 0=성공, 음수=실패.
 *
 * **두 가지 모드**:
 *   1) trid.subnqn != DISCOVERY_NQN → 사용자가 직접 NVM 컨트롤러를 지정 → nvme_ctrlr_probe로 즉시 attach.
 *   2) trid.subnqn == DISCOVERY_NQN ("nqn.2014-08.org.nvmexpress.discovery") → Discovery 모드:
 *       (a) 임시 discovery 컨트롤러 만들기 (nvme_transport_ctrlr_construct).
 *       (b) 그 컨트롤러를 READY까지 초기화 (process_init 루프).
 *       (c) Identify Controller로 cdata 가져오기 (대부분 안 쓰지만 일관성 유지).
 *       (d) direct_connect=true면 이 discovery 컨트롤러 자체를 사용자에게 attach 후 종료.
 *           direct_connect=false면 nvme_fabric_ctrlr_discover로 Discovery Log를 읽어 다른 NVM
 *           컨트롤러들을 발견 후, discovery 컨트롤러는 destruct(임시였으므로).
 *
 * **NVMe-oF Discovery NQN의 의미**:
 * NVMe-oF는 "discovery 컨트롤러"라는 가상 서브시스템을 둔다 — 실제 NVM 데이터를 제공하지 않고
 * Discovery Log Page만 제공. 호스트는 먼저 이 discovery 컨트롤러에 연결해서 어떤 NVM 컨트롤러들이
 * 있는지 목록을 받고, 그 후 진짜 NVM 컨트롤러에 연결.
 *
 * 호출 체인:
 *   spdk_nvme_probe → nvme_transport_ctrlr_scan → ops.ctrlr_scan (트랜스포트별)
 *     → [이 함수] → 분기:
 *         (NVM)       nvme_ctrlr_probe
 *         (DISC)      nvme_transport_ctrlr_construct + process_init + Identify + discover
 */
int
nvme_fabric_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
		       bool direct_connect)
{
	struct spdk_nvme_ctrlr_opts discovery_opts;
	/* [한국어] discovery 컨트롤러 전용 opts(default 값). 사용자 opts와 분리하여 discovery 컨트롤러는
	 * 항상 default로 만듦 — discovery는 짧은 수명이고 특별한 튜닝 불필요. */
	struct spdk_nvme_ctrlr *discovery_ctrlr;
	int rc;
	struct nvme_completion_poll_status *status;

	if (strcmp(probe_ctx->trid.subnqn, SPDK_NVMF_DISCOVERY_NQN) != 0) {
		/* It is not a discovery_ctrlr info and try to directly connect it */
		/* [한국어] 사용자가 명시한 NQN이 표준 discovery NQN과 다르면 → 사용자가 직접 attach 의도 →
		 * 일반 ctrlr_probe 경로로 즉시 진입. discovery 단계 생략. */
		rc = nvme_ctrlr_probe(&probe_ctx->trid, probe_ctx, NULL);
		return rc;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&discovery_opts, sizeof(discovery_opts));
	/* [한국어] discovery_opts를 default로 채움 (admin queue 사이즈, KATO 등 표준값). */
	if (direct_connect && probe_ctx->probe_cb) {
		probe_ctx->probe_cb(probe_ctx->cb_ctx, &probe_ctx->trid, &discovery_opts);
		/* [한국어] direct_connect 모드 + 사용자 probe_cb 등록 시 → 사용자가 opts를 수정할 기회 제공
		 * (예: keep_alive_timeout 변경). 이 콜백 안에서 사용자가 attach 여부도 결정 가능 — 그러나
		 * direct_connect는 항상 attach 의도이므로 false 반환은 무시되는 셈. */
	}

	discovery_ctrlr = nvme_transport_ctrlr_construct(&probe_ctx->trid, &discovery_opts, NULL);
	/* [한국어] 트랜스포트 vtable의 ctrlr_construct 호출 — RDMA/TCP 등 ops가 디스패치됨.
	 * NVMe-oF의 경우 connection setup(TCP socket / RDMA QP), admin qpair 생성까지 진행. */
	if (discovery_ctrlr == NULL) {
		return -1;
	}

	while (discovery_ctrlr->state != NVME_CTRLR_STATE_READY) {
		/* [한국어] 컨트롤러 상태머신을 READY까지 동기로 진행. process_init은 1 step씩 진행하므로
		 * busy-wait 루프로 모든 단계(CONNECT_ADMINQ → READ_VS/CAP → CHECK_EN → ENABLE → IDENTIFY →
		 * SET_NUM_QUEUES → SET_KEEP_ALIVE → ... → READY)를 거침. */
		if (nvme_ctrlr_process_init(discovery_ctrlr) != 0) {
			nvme_ctrlr_destruct(discovery_ctrlr);
			/* [한국어] 단계 중 실패 → 임시 컨트롤러 정리 후 실패 반환. resource leak 방지. */
			return -1;
		}
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(discovery_ctrlr, "Failed to allocate status tracker\n");
		nvme_ctrlr_destruct(discovery_ctrlr);
		return -ENOMEM;
	}

	/* get the cdata info */
	rc = nvme_ctrlr_cmd_identify(discovery_ctrlr, SPDK_NVME_IDENTIFY_CTRLR, 0, 0, 0,
				     &discovery_ctrlr->cdata, sizeof(discovery_ctrlr->cdata),
				     nvme_completion_poll_cb, status);
	/* [한국어] Identify Controller (CNS=0x01) 발사 — discovery 컨트롤러의 cdata 채움.
	 * discovery 컨트롤러도 cdata에 vid/ssvid/sn/mn/fr 등 기본 정보를 보고함 — 일부 사용자가 이 정보를
	 * direct_connect 시 활용 가능. */
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(discovery_ctrlr, "Failed to identify cdata\n");
		nvme_ctrlr_destruct(discovery_ctrlr);
		free(status);
		return rc;
	}

	rc = nvme_wait_for_adminq_completion(discovery_ctrlr, status, true);
	if (rc) {
		NVME_CTRLR_ERRLOG(discovery_ctrlr, "wait for nvme_identify_controller failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
		nvme_ctrlr_destruct(discovery_ctrlr);
		return rc;
		/* [한국어] 주의: status는 robust=true 경로이므로 timed_out=true면 free되지 않음.
		 * 함수 반환 시점에 leak이지만 컨트롤러 destruct를 거치므로 메모리 정리 측면에서 비교적
		 * 무해 (프로세스 종료 시 OS가 회수). */
	}

	/* Direct attach through spdk_nvme_connect() API */
	if (direct_connect == true) {
		/* Set the ready state to skip the normal init process */
		/* [한국어] direct_connect 모드 — 사용자가 spdk_nvme_connect()로 discovery 컨트롤러 자체를
		 * "그대로 attach"하길 원함. 이 경우 별도의 NVM 컨트롤러를 찾지 않고 discovery_ctrlr를
		 * 사용자 attach_cb에 그대로 전달. */
		discovery_ctrlr->state = NVME_CTRLR_STATE_READY;
		/* [한국어] 이미 READY지만 명시 — connected 처리 후 process_init이 다시 돌지 않게 보장. */
		nvme_ctrlr_connected(probe_ctx, discovery_ctrlr);
		/* [한국어] probe_ctx 에 attached_ctrlr로 등록 — 사용자 attach_cb가 호출됨. */
		nvme_ctrlr_add_process(discovery_ctrlr, 0);
		/* [한국어] 컨트롤러의 active_proc 리스트에 현재 프로세스(pid=0 sentinel) 추가 — multi-process
		 * 시 자기 컨텍스트 등록. 같은 컨트롤러를 다른 프로세스가 사용해도 충돌 없도록. */
		return 0;
	}

	rc = nvme_fabric_ctrlr_discover(discovery_ctrlr, probe_ctx);
	/* [한국어] 표준 discovery 모드 — Discovery Log Page를 페이징 읽으며 발견된 각 NVM 서브시스템에
	 * 대해 nvme_ctrlr_probe 재귀 호출. 이 함수가 끝나면 모든 발견된 컨트롤러는 probe_ctx에 등록됨. */
	nvme_ctrlr_destruct(discovery_ctrlr);
	/* [한국어] discovery 컨트롤러는 임시 — 발견 작업 완료 후 destruct (TCP 연결 끊기, 자원 해제).
	 * 이후 probe_ctx에 추가된 NVM 컨트롤러들만 사용자에게 attach됨. */
	return rc;
}

/*
 * [한국어]
 * nvme_fabric_ctrlr_discover - Discovery Log Page를 4KB 버퍼로 페이징 읽으며 모든 entry 처리.
 *
 * @ctrlr: 이미 READY 상태인 discovery 컨트롤러.
 * @probe_ctx: 발견된 NVM 컨트롤러를 누적할 probe 세션.
 * @return: 0=모든 entry 처리 완료, 음수=실패.
 *
 * **Discovery Log Page 와이어 포맷**:
 *   [헤더 16바이트: genctr(8B) + numrec(8B) + recfmt(2B) + reserved]
 *   [entries[0] 1024B]
 *   [entries[1] 1024B]
 *   ...
 *   [entries[numrec-1]]
 *
 * 4KB 버퍼 1번에 들어가는 엔트리 수:
 *   - 첫 페이지(헤더 포함): (4096 - 16) / 1024 = 3.97... → 3개
 *   - 이후 페이지(entry만): 4096 / 1024 = 4개
 *
 * **알고리즘**:
 *   1) buffer_max_entries_first(=3) / buffer_max_entries(=4) 미리 계산.
 *   2) loop {
 *        get_discovery_log_page(buffer, 4096, log_page_offset)
 *        if 첫 호출: 헤더 검증(recfmt=0) + numrec 추출 + entries[0] 시작 주소 + 첫 페이지 entry 수
 *        else: entry 수만 결정
 *        for each entry: nvme_fabric_discover_probe (위에서 정의)
 *        remaining_num_rec -= numrec; log_page_offset += numrec * 1024
 *      } until remaining_num_rec == 0
 *
 * 이 함수 한 번 호출로 numrec=수백 개의 컨트롤러를 모두 발견 가능 (4KB씩 여러 번 GET LOG PAGE).
 *
 * 호출 컨텍스트: nvme_fabric_ctrlr_scan의 마지막 단계. discovery 컨트롤러는 이 함수 종료 후
 * destruct.
 */
int
nvme_fabric_ctrlr_discover(struct spdk_nvme_ctrlr *ctrlr,
			   struct spdk_nvme_probe_ctx *probe_ctx)
{
	struct spdk_nvmf_discovery_log_page *log_page;
	/* [한국어] Discovery Log Page 헤더 + 첫 페이지 entries 영역에 대한 view 포인터. */
	struct spdk_nvmf_discovery_log_page_entry *log_page_entry;
	/* [한국어] 현재 처리 중인 entry 시작 위치(첫 페이지에서는 헤더 다음 entries[0]을 가리키고,
	 * 이후 페이지에서는 buffer 시작점을 가리킴). */
	char buffer[4096];
	/* [한국어] 4KB 스택 버퍼 — DPDK hugepage가 아니어도 됨(GET LOG PAGE는 PRP/SGL로 임의 메모리
	 * 사용 가능). 4KB는 NVMe 페이지 경계와 정확히 일치하여 PRP1만으로 처리 가능. */
	int rc;
	uint64_t i, numrec, buffer_max_entries_first, buffer_max_entries, log_page_offset = 0;
	/* [한국어] log_page_offset: discovery log page 내 다음에 읽을 위치(0부터 시작 — 헤더부터). */
	uint64_t remaining_num_rec = 0;
	/* [한국어] 아직 처리하지 않은 entry 수. 첫 GET 후 헤더에서 numrec를 읽어 초기화. */
	uint16_t recfmt;
	/* [한국어] Record Format — 현재 NVMe-oF 1.x에서는 0만 정의됨. 0이 아니면 호환되지 않는 신
	 * 포맷이라 호스트가 처리 불가. */

	memset(buffer, 0x0, 4096);
	/* [한국어] 스택 버퍼 0 초기화 — 컨트롤러 응답이 4KB 미만이어도 잔여 영역을 NUL로 보장. */
	buffer_max_entries_first = (sizeof(buffer) - offsetof(struct spdk_nvmf_discovery_log_page,
				    entries[0])) /
				   sizeof(struct spdk_nvmf_discovery_log_page_entry);
	/* [한국어] 첫 번째 GET 시 4KB에서 헤더(=offsetof entries[0] = 16바이트 가량)를 뺀 영역에
	 * 들어가는 1024바이트 entry 수. 보통 3개. offsetof는 컴파일 타임에 결정. */
	buffer_max_entries = sizeof(buffer) / sizeof(struct spdk_nvmf_discovery_log_page_entry);
	/* [한국어] 두 번째 GET부터 4KB 전체에 entry만 들어옴 — 헤더는 첫 페이지에서 이미 처리.
	 * 4096 / 1024 = 정확히 4개. */
	do {
		rc = nvme_fabric_get_discovery_log_page(ctrlr, buffer, sizeof(buffer), log_page_offset);
		/* [한국어] 4KB 한 번 받기 — log_page_offset 위치부터 4096바이트. 동기 대기 함수. */
		if (rc < 0) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Get Log Page - Discovery error\n");
			return rc;
		}

		if (!remaining_num_rec) {
			/* [한국어] 첫 호출 — 아직 numrec 모름. 헤더 검증 후 추출. */
			log_page = (struct spdk_nvmf_discovery_log_page *)buffer;
			/* [한국어] buffer 시작 = 헤더 시작. */
			recfmt = from_le16(&log_page->recfmt);
			/* [한국어] little-endian 와이어 → 호스트 endian. NVMe-oF 와이어는 LE 강제. */
			if (recfmt != 0) {
				NVME_CTRLR_ERRLOG(ctrlr, "Unrecognized discovery log record format %" PRIu16 "\n", recfmt);
				return -EPROTO;
				/* [한국어] 호환되지 않는 포맷 → 즉시 실패. */
			}
			remaining_num_rec = log_page->numrec;
			/* [한국어] 전체 entry 수. 0이면 발견된 NVM 서브시스템 없음 → 루프 1회로 종료. */
			log_page_offset = offsetof(struct spdk_nvmf_discovery_log_page, entries[0]);
			/* [한국어] 다음 GET은 헤더를 건너뛴 위치부터 — 즉 entries[0]의 절대 오프셋. 첫 페이지에
			 * 들어간 entry는 그 다음 GET 시작점 계산에 영향. */
			log_page_entry = &log_page->entries[0];
			/* [한국어] 첫 페이지에서 처리할 entry 시작 주소 = 헤더 바로 뒤. */
			numrec = spdk_min(remaining_num_rec, buffer_max_entries_first);
			/* [한국어] 이번 페이지에 들어 있는 실제 entry 수 = 전체 numrec와 max(=3) 중 min. */
		} else {
			/* [한국어] 두 번째 GET 이후 — 헤더 없이 entry만. */
			numrec = spdk_min(remaining_num_rec, buffer_max_entries);
			/* [한국어] 이번 페이지의 entry 수 = 잔여와 max(=4) 중 min. 마지막 페이지는 max보다
			 * 작을 수 있음. */
			log_page_entry = (struct spdk_nvmf_discovery_log_page_entry *)buffer;
			/* [한국어] buffer 시작이 곧 entry 시작. */
		}

		for (i = 0; i < numrec; i++) {
			nvme_fabric_discover_probe(log_page_entry++, probe_ctx, ctrlr->trid.priority);
			/* [한국어] 현재 entry로 probe → 다음 entry로 포인터 이동(post-increment).
			 * 발견된 각 NVM 컨트롤러에 대해 nvme_ctrlr_probe가 재귀적으로 attach 시도. */
		}
		remaining_num_rec -= numrec;
		/* [한국어] 잔여 entry 수 차감. */
		log_page_offset += numrec * sizeof(struct spdk_nvmf_discovery_log_page_entry);
		/* [한국어] 다음 GET 오프셋 = 이번에 읽은 마지막 entry 끝의 다음 위치.
		 * (첫 페이지에서는 위에서 entries[0]의 오프셋으로 초기화한 후, 첫 페이지의 entry 수만큼 증가.
		 * 이후 페이지에서는 read한 numrec * 1024씩 증가.) */
	} while (remaining_num_rec != 0);
	/* [한국어] 모든 entry 처리할 때까지 4KB씩 페이징 GET. */

	return 0;
}

/*
 * [한국어]
 * nvme_fabric_qpair_connect_async - Fabric CONNECT 커맨드를 비동기로 발사 (큐페어 핸드셰이크 시작).
 *
 * @qpair: CONNECT를 발사할 큐페어. admin 큐 또는 I/O 큐.
 * @num_entries: 큐페어 SQ 크기 (0-based wire 표현은 sqsize = num_entries - 1).
 * @return: 0=SQE 제출 성공(완료는 connect_poll로 폴링), 음수=인자/메모리/제출 실패.
 *
 * **NVMe-oF에서 CONNECT가 필요한 이유**:
 * PCIe NVMe는 SQ/CQ가 BAR + AQA/ASQ/ACQ 레지스터로 자동 활성화. NVMe-oF는 트랜스포트 추상화 위에서
 * 호스트가 명시적으로 "이 큐페어를 컨트롤러와 묶고 싶다"는 의도를 표현해야 → CONNECT 커맨드.
 *
 * **CONNECT 데이터 구조** (1024바이트 페이로드):
 *   - hostid[16]      = 호스트 식별자 (RFC4122 UUID)
 *   - cntlid          = admin 큐: 0xFFFF(=요청), I/O 큐: admin이 받은 cntlid
 *   - subnqn[256]     = 대상 NVM 서브시스템 NQN
 *   - hostnqn[256]    = 호스트 자신의 NQN (인증/감사용)
 *
 * **CONNECT SQE 필드** (64바이트):
 *   - opcode = 0x7F (FABRIC), fctype = 0x01 (CONNECT)
 *   - qid = qpair->id (admin=0, IO=1..N)
 *   - sqsize = num_entries - 1 (NVMe-oF 와이어는 0-based)
 *   - kato = keep alive timeout in ms (admin 큐만 의미)
 *
 * **동작 흐름**:
 *   1) 인자 검증 (num_entries, ctrlr 존재).
 *   2) DMA hugepage에 nvmf_data 1024B 할당 (트랜스포트가 host→target으로 보낼 페이로드).
 *   3) status tracker calloc + dma_data 보존 (정리 시 함께 free).
 *   4) SQE 빌드 (opcode/fctype/qid/sqsize/kato).
 *   5) qpair->reserved_req 사용 — 일반 풀이 비어 있어도 CONNECT는 무조건 가능해야 하므로
 *      큐페어 생성 시 미리 1슬롯 예약. NVME_INIT_REQUEST로 콜백/페이로드/완료타입 셋업.
 *   6) 페이로드 채우기 (cntlid 결정 + hostid + hostnqn + subnqn).
 *   7) nvme_qpair_submit_request → 트랜스포트가 와이어로 송신.
 *   8) 타임아웃 timestamp 계산(옵션).
 *   9) qpair->fabric_poll_status에 status 저장 → connect_poll이 소비.
 *
 * 호출자는 이 함수 후 nvme_fabric_qpair_connect_poll을 -EAGAIN이 사라질 때까지 호출하여 완료 대기.
 *
 * 호출 체인:
 *   ops.ctrlr_connect_qpair (트랜스포트별)
 *     → nvme_fabric_qpair_connect 또는 connect_async + connect_poll 직접 호출
 *     → [이 함수]: SQE 제출 + status 보존 → return 0 (-EAGAIN 후속)
 */
int
nvme_fabric_qpair_connect_async(struct spdk_nvme_qpair *qpair, uint32_t num_entries)
{
	struct nvme_completion_poll_status *status;
	struct spdk_nvmf_fabric_connect_cmd cmd;
	/* [한국어] CONNECT SQE 64바이트 — 스택 임시 변수. submit 직전에 req->cmd로 memcpy됨. */
	struct spdk_nvmf_fabric_connect_data *nvmf_data;
	/* [한국어] CONNECT 페이로드 1024바이트 (DMA-able 영역 필요). 트랜스포트가 SGL/PRP로
	 * 가리키므로 hugepage에서 할당. */
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_request *req;
	int rc;

	if (num_entries == 0 || num_entries > SPDK_NVME_IO_QUEUE_MAX_ENTRIES) {
		return -EINVAL;
		/* [한국어] num_entries 유효 범위 (1 ≤ N ≤ MAX). 0은 NVMe 와이어상으론 1을 의미할
		 * 수도 있으나, SPDK는 명시적 0 거부로 호출자 버그 발견. */
	}

	ctrlr = qpair->ctrlr;
	if (!ctrlr) {
		return -EINVAL;
		/* [한국어] qpair는 항상 ctrlr와 짝이지만 방어적 검사 — qpair가 destruct 직후 잘못 사용되는
		 * 경로 차단. */
	}

	nvmf_data = spdk_zmalloc(sizeof(*nvmf_data), 0, NULL,
				 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	/* [한국어] DPDK hugepage에서 1024B DMA 메모리 할당 + 0 초기화 — RDMA 등 트랜스포트가 호스트→
	 * 타깃 송신 시 물리 주소 등록 필요. SPDK_ENV_LCORE_ID_ANY = NUMA 노드 미지정 (default). */
	if (!nvmf_data) {
		NVME_QPAIR_ERRLOG(qpair, "nvmf_data allocation error\n");
		return -ENOMEM;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to allocate status tracker\n");
		spdk_free(nvmf_data);
		/* [한국어] tracker 할당 실패 시 nvmf_data 누수 방지. */
		return -ENOMEM;
	}

	status->dma_data = nvmf_data;
	/* [한국어] tracker가 dma 페이로드를 함께 보유 → cleanup이 한 번에 해제 가능. */

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	/* [한국어] Admin opcode 0x7F. CONNECT도 fabric 커맨드 카테고리. */
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	/* [한국어] Fabric subtype 0x01. */
	cmd.qid = qpair->id;
	/* [한국어] 큐페어 ID — admin=0, I/O=1..N. NVMe-oF의 controller association 핵심 키. */
	cmd.sqsize = num_entries - 1;
	/* [한국어] NVMe 와이어 컨벤션 0-based — 32 entry 큐면 sqsize=31. */
	cmd.kato = ctrlr->opts.keep_alive_timeout_ms;
	/* [한국어] Keep Alive Timeout (ms). admin 큐만 의미 — 컨트롤러가 이 시간 동안 keep_alive를
	 * 못 받으면 host disconnect 처리. NVMe-oF 1.x 기본값은 120초. I/O 큐에서는 무시되지만
	 * 일관성 위해 항상 채움. */

	assert(qpair->reserved_req != NULL);
	/* [한국어] reserved_req는 nvme_qpair_init에서 미리 1개 예약된 nvme_request 슬롯.
	 * 일반 풀(free_req)이 비어 있어도 CONNECT/AUTH는 반드시 발사 가능하도록 격리 보존. */
	req = qpair->reserved_req;
	NVME_INIT_REQUEST(req, nvme_completion_poll_cb, status, NVME_PAYLOAD_CONTIG(nvmf_data, NULL),
			  sizeof(*nvmf_data), 0);
	/* [한국어] req 슬롯 초기화 — 콜백/콜백인자/페이로드(CONTIG=연속 buffer)/길이/메타데이터 길이.
	 * NVME_PAYLOAD_CONTIG는 nvme_payload union을 contiguous 버퍼 모드로 셋업. 메타데이터=0 — CONNECT는
	 * end-to-end protection 없음. */

	memcpy(&req->cmd, &cmd, sizeof(cmd));
	/* [한국어] 스택의 cmd를 req->cmd(SQE 슬롯)로 복사. 이후 트랜스포트 submit이 req->cmd를 와이어에
	 * encapsulate(RDMA SEND / TCP CapsuleCmd 등). */

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvmf_data->cntlid = 0xFFFF;
		/* [한국어] admin 큐 CONNECT: cntlid=0xFFFF로 "할당 요청" 의미. 컨트롤러가 응답에서 실제 cntlid
		 * 부여 → connect_poll에서 ctrlr->cntlid에 저장. */
	} else {
		nvmf_data->cntlid = ctrlr->cntlid;
		/* [한국어] I/O 큐 CONNECT: 이미 admin 큐에서 부여받은 cntlid를 첨부 → 컨트롤러가 이 큐페어를
		 * 어느 컨트롤러 association에 묶을지 결정 근거. */
	}

	SPDK_STATIC_ASSERT(sizeof(nvmf_data->hostid) == sizeof(ctrlr->opts.extended_host_id),
			   "host ID size mismatch");
	/* [한국어] 컴파일 타임 assert — 와이어 hostid는 16바이트, ctrlr_opts의 extended_host_id도 16바이트.
	 * 두 사이즈가 어긋나면 빌드 실패 → 사일런트 truncation 방지. */
	memcpy(nvmf_data->hostid, ctrlr->opts.extended_host_id, sizeof(nvmf_data->hostid));
	/* [한국어] 호스트 식별자(UUID 16바이트) — 사용자가 spdk_nvme_ctrlr_opts에 설정. 컨트롤러가
	 * reservation/multi-host 정책에서 호스트 구별에 사용. */
	snprintf(nvmf_data->hostnqn, sizeof(nvmf_data->hostnqn), "%s", ctrlr->opts.hostnqn);
	/* [한국어] 호스트 NQN — 와이어 256바이트, 자동 NUL 종결. 인증 시 호스트 식별 + 감사 로그용. */
	snprintf(nvmf_data->subnqn, sizeof(nvmf_data->subnqn), "%s", ctrlr->trid.subnqn);
	/* [한국어] 대상 NVM 서브시스템 NQN — 트랜스포트 라우팅 후 컨트롤러가 자기 NQN과 일치 검증. */

	rc = nvme_qpair_submit_request(qpair, req);
	/* [한국어] 큐페어로 직접 submit — 일반 IO submit과 동일한 경로(트랜스포트 ops.qpair_submit_request)지만,
	 * 큐페어 자체가 아직 ENABLED 상태가 아니므로 nvme_qpair_submit_request의 state 체크에서
	 * FABRIC-CONNECTING 상태일 때만 허용되는 경로를 통과. */
	if (rc < 0) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to allocate/submit FABRIC_CONNECT command, rc %d\n", rc);
		spdk_free(status->dma_data);
		free(status);
		/* [한국어] 제출 실패 → 콜백 미실행 → 자원 직접 해제 (페이로드 + tracker). */
		return rc;
	}

	/* If we time out, the qpair will abort the request upon destruction. */
	if (ctrlr->opts.fabrics_connect_timeout_us > 0) {
		status->timeout_tsc = spdk_get_ticks() + ctrlr->opts.fabrics_connect_timeout_us *
				      spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
		/* [한국어] 타임아웃 deadline 계산 — current TSC + (us → ticks). connect_poll이 매번 비교하여
		 * 초과 시 timed_out=true로 마킹. 0이면 무한 대기 (옵션이 명시되지 않은 경우). */
	}

	qpair->auth.flags.raw = 0;
	/* [한국어] 인증 플래그 초기화 — atr/ascr/in_auth_poll 모두 0. CONNECT 응답이 도착하면 atr/ascr이
	 * 채워질 수 있음 (connect_poll에서). */
	qpair->fabric_poll_status = status;
	/* [한국어] connect_poll이 -EAGAIN 동안 폴링하며 사용할 status. 동시에 큐페어가 destruct되면
	 * 미완료 CONNECT를 abort할 수 있도록 정리 경로의 핸들. */
	return 0;
}

/*
 * [한국어]
 * nvme_fabric_qpair_poll_cleanup - CONNECT 완료/실패 후 fabric_poll_status 자원 정리.
 *
 * @qpair: connect_async에서 fabric_poll_status를 설정한 큐페어.
 *
 * 동작:
 *   - qpair->fabric_poll_status를 NULL로 (idempotency 보장 — 두 번 호출되어도 안전).
 *   - status->timed_out이 false면 dma_data + status 해제.
 *   - timed_out=true면 메모리를 해제하지 않음 → 늦게 도착할 콜백이 use-after-free하지 않도록.
 *     (대신 트랜스포트 destruct 경로가 결국 abort로 콜백을 강제 호출하므로 그때 해제.)
 *
 * 호출 컨텍스트: connect_poll 종료 시 (성공/타임아웃/CQE 에러 모두), 또는 큐페어 destruct 시.
 */
void
nvme_fabric_qpair_poll_cleanup(struct spdk_nvme_qpair *qpair)
{
	struct nvme_completion_poll_status *status = qpair->fabric_poll_status;
	/* [한국어] 큐페어에서 status 핸들 회수. */

	qpair->fabric_poll_status = NULL;
	/* [한국어] 즉시 NULL로 — 이후 connect_poll이 다시 호출되어도 fabric_poll_status에서 다른
	 * 작업과 혼동되지 않도록. */
	if (!status->timed_out) {
		spdk_free(status->dma_data);
		/* [한국어] DMA hugepage(nvmf_data 1024B) 해제. */
		free(status);
		/* [한국어] tracker 해제. */
	}
	/* [한국어] timed_out=true면 의도적 leak — 큐페어가 결국 abort_all_queued_reqs 등으로 콜백을
	 * 강제 호출할 때 status에 안전하게 접근하도록 보존. */
}

/*
 * [한국어]
 * nvme_fabric_qpair_auth_cleanup - 인증 콜백을 필요 시 호출하여 사용자에게 결과 통지.
 *
 * @qpair: 인증 콜백을 등록한 큐페어 (qpair->auth.cb_fn).
 * @status: 사용자 콜백에 전달할 결과 (0=성공, 음수=실패).
 *
 * 인증이 필요 없거나 비동기 인증 시작 후 정리 시점에 호출.
 * cb_fn=NULL이면 이미 호출되었거나 등록 안 된 경우 → no-op.
 *
 * 호출 컨텍스트:
 *   - 인증 미수행 케이스: connect_poll 직후 nvme_fabric_qpair_auth_required가 false면 status=0으로 호출.
 *   - 인증 실패: nvme_auth.c의 인증 상태머신이 실패 시.
 *   - qpair destruct 도중 미완료 인증 정리.
 */
void
nvme_fabric_qpair_auth_cleanup(struct spdk_nvme_qpair *qpair, int status)
{
	if (qpair->auth.cb_fn != NULL) {
		/* [한국어] 사용자가 spdk_nvme_qpair_authenticate_async로 등록한 콜백이 있으면. */
		qpair->auth.cb_fn(qpair->auth.cb_ctx, status);
		/* [한국어] 사용자 콜백 호출 → status로 결과 통지. */
		qpair->auth.cb_fn = NULL;
		/* [한국어] cb_fn을 NULL로 — idempotent. 두 번 호출되어도 사용자 cb는 1번만 실행. */
	}
}

/*
 * [한국어]
 * nvme_fabric_qpair_connect_poll - CONNECT 응답을 폴링하며 도착 시 cntlid/auth 플래그 추출.
 *
 * @qpair: connect_async로 CONNECT를 발사한 큐페어 (fabric_poll_status가 set 되어 있어야).
 * @return: -EAGAIN=아직 응답 없음(다시 호출 필요), 0=성공, -ECANCELED=타임아웃, -EIO=CQE 에러.
 *
 * 동작:
 *   1) nvme_wait_for_completion_poll로 폴링 — 1번의 process_completions만 수행하고 반환.
 *      반환값 -EAGAIN이면 아직 응답 없음 → 호출자가 다시 호출.
 *   2) 응답 도착 → status->cpl 검사:
 *      - status->timed_out = true → -ECANCELED.
 *      - status->cpl.status.sct/sc != 0 → -EIO + sct/sc 로깅.
 *      - 둘 다 정상 → SUCCESS 처리:
 *          (a) admin 큐였으면 응답에서 cntlid 추출하여 ctrlr->cntlid에 저장. 이 cntlid는 이후
 *              I/O 큐 CONNECT의 페이로드에 들어감.
 *          (b) authreq.atr=1 → qpair->auth.flags.atr=1 (인증 트랜잭션 필요).
 *          (c) authreq.ascr=1 → qpair->auth.flags.ascr=1 (인증 + secure channel 필요).
 *   3) 결과와 관계없이 finish: 자원 정리.
 *
 * **CONNECT 응답 파싱 패턴**:
 * NVMe-oF CONNECT 응답 16바이트(spdk_nvmf_fabric_connect_rsp)는 status_code_specific union을 가짐.
 * SUCCESS 시에는 success 서브구조체 — cntlid + authreq(atr/ascr 비트필드).
 * FAILURE 시에는 invalid 서브구조체 — ipo + iattr (어느 필드가 잘못됐는지). 여기서는 SUCCESS 경로만
 * 파싱하고 FAILURE는 sct/sc 로깅만.
 *
 * 호출 패턴 (busy-wait):
 *   do { rc = nvme_fabric_qpair_connect_poll(qpair); } while (rc == -EAGAIN);
 */
int
nvme_fabric_qpair_connect_poll(struct spdk_nvme_qpair *qpair)
{
	struct nvme_completion_poll_status *status;
	struct spdk_nvmf_fabric_connect_rsp *rsp;
	/* [한국어] CQE 16B를 CONNECT 응답 구조체로 type pun할 포인터. */
	struct spdk_nvme_ctrlr *ctrlr;
	int rc = 0;

	ctrlr = qpair->ctrlr;
	status = qpair->fabric_poll_status;
	/* [한국어] connect_async가 보존한 tracker 회수. */

	if (nvme_wait_for_completion_poll(qpair, status) == -EAGAIN) {
		return -EAGAIN;
		/* [한국어] 아직 응답 없음 — 호출자가 다시 호출하도록 빠르게 반환. busy-wait 루프의 한 라운드. */
	}

	if (status->timed_out || spdk_nvme_cpl_is_error(&status->cpl)) {
		/* [한국어] 타임아웃이거나 CQE에서 에러 보고 → 에러 경로. */
		NVME_QPAIR_ERRLOG(qpair, "Connect command failed, rc %d, trtype:%s adrfam:%s "
				  "traddr:%s trsvcid:%s subnqn:%s\n",
				  status->timed_out ? -ECANCELED : -EIO,
				  spdk_nvme_transport_id_trtype_str(ctrlr->trid.trtype),
				  spdk_nvme_transport_id_adrfam_str(ctrlr->trid.adrfam),
				  ctrlr->trid.traddr,
				  ctrlr->trid.trsvcid,
				  ctrlr->trid.subnqn);
		/* [한국어] 진단 로그 — 어느 트랜스포트/주소/NQN으로 가던 CONNECT가 실패했는지. 운영 환경에서
		 * 멀티 컨트롤러 셋업 시 어느 타깃이 문제인지 식별 핵심. */
		if (status->timed_out) {
			rc = -ECANCELED;
			/* [한국어] 타임아웃 코드로 -ECANCELED 사용 (POSIX 협약 — 실제 디스커넥트는 큐페어 destruct
			 * 시 일어나며, 그 전에는 abort된 상태로 표현). */
		} else {
			NVME_QPAIR_ERRLOG(qpair, "Connect command completed with error: sct %d, sc %d\n",
					  status->cpl.status.sct, status->cpl.status.sc);
			/* [한국어] CQE의 sct(Status Code Type)/sc(Status Code) 추출 — 예: sct=1(Command Specific)
			 * + sc=0x83(Connect Invalid Parameters) 등 NVMe-oF 1.x 정의된 코드. */
			rc = -EIO;
		}

		goto finish;
	}

	rsp = (struct spdk_nvmf_fabric_connect_rsp *)&status->cpl;
	/* [한국어] CQE 16B를 CONNECT 응답 구조체로 캐스트. status_code_specific union이 16바이트 영역의
	 * dword0/1 위치에 매핑. */
	if (nvme_qpair_is_admin_queue(qpair)) {
		ctrlr->cntlid = rsp->status_code_specific.success.cntlid;
		/* [한국어] admin 큐 CONNECT 성공 시 컨트롤러가 부여한 cntlid 추출 → ctrlr 객체에 영구 보존.
		 * 이후 같은 컨트롤러의 모든 I/O 큐 CONNECT가 이 값을 페이로드에 첨부 → controller association
		 * 의 핵심 키. */
		NVME_CTRLR_DEBUGLOG(ctrlr, "cntlid set\n");
	}
	if (rsp->status_code_specific.success.authreq.atr) {
		qpair->auth.flags.atr = true;
		/* [한국어] atr (authreq Authentication Transactions Required) — 컨트롤러가 인증 트랜잭션을
		 * 요구. 호스트는 다음 단계로 nvme_auth.c의 DH-CHAP 핸드셰이크를 시작해야 큐페어가 사용 가능. */
	}
	if (rsp->status_code_specific.success.authreq.ascr) {
		qpair->auth.flags.ascr = true;
		/* [한국어] ascr (Authentication and Secure Channel Required) — 인증 + secure channel(TLS 등)
		 * 둘 다 요구. SPDK의 NVMe-oF TCP 트랜스포트가 TLS 협상을 별도로 수행. */
	}
finish:
	nvme_fabric_qpair_poll_cleanup(qpair);
	/* [한국어] 성공/실패 어느 경로든 fabric_poll_status 자원 정리 — 리소스 leak 방지의 단일 진입점. */
	return rc;
}

/*
 * [한국어]
 * nvme_fabric_qpair_auth_required - 큐페어가 인증 단계로 진입해야 하는지 판단.
 *
 * @qpair: CONNECT 응답 처리가 끝난 큐페어 (auth.flags 채워진 상태).
 * @return: true=인증 필요, false=인증 불필요(즉시 사용 가능).
 *
 * 인증이 필요한 4가지 경우 (OR):
 *   1) qpair->auth.flags.atr — 컨트롤러가 응답에서 인증 트랜잭션 요구 (atr=1).
 *   2) qpair->auth.flags.ascr — 컨트롤러가 인증+secure channel 요구 (ascr=1).
 *   3) ctrlr->opts.dhchap_ctrlr_key != NULL — 호스트가 ctrlr 인증을 요구하기 위해 DH-CHAP 키를
 *      설정해 둠. 컨트롤러가 atr/ascr을 안 주더라도 호스트 정책상 강제 인증.
 *   4) qpair->auth.cb_fn != NULL — 사용자가 명시적으로 spdk_nvme_qpair_authenticate_async를 호출
 *      하여 인증 콜백 등록 → 인증 수행 후 사용자에게 결과 통지 의무.
 *
 * 호출 컨텍스트: connect_poll 성공 후 트랜스포트별 ctrlr_connect_qpair 끝부분에서 분기 결정.
 * true면 nvme_fabric_qpair_authenticate_async 진입, false면 큐페어 ENABLED 상태로 전이.
 */
bool
nvme_fabric_qpair_auth_required(struct spdk_nvme_qpair *qpair)
{
	return qpair->auth.flags.atr || qpair->auth.flags.ascr ||
	       qpair->ctrlr->opts.dhchap_ctrlr_key != NULL || qpair->auth.cb_fn != NULL;
	/* [한국어] 4가지 조건의 OR. 어느 하나라도 true면 인증 필요. 평가는 lazy 평가이므로 atr만 true이면
	 * 나머지는 검사하지 않음. */
}

/*
 * [한국어]
 * nvme_fabric_qpair_connect - CONNECT의 동기 진입점 (busy-wait 래퍼).
 *
 * @qpair / @num_entries: connect_async와 동일.
 * @return: 0=성공, 음수=실패 (connect_async 또는 connect_poll의 실패 코드).
 *
 * 단순한 do-while 패턴 — connect_async로 발사 후 connect_poll을 -EAGAIN이 아닐 때까지 반복.
 * 폴링 루프 동안 같은 큐페어의 모든 작업이 정지되므로 admin/IO 큐의 setup 단계에서만 사용.
 *
 * 호출 컨텍스트: 트랜스포트별 ctrlr_connect_qpair가 동기 모드일 때 (대부분의 경로). 비동기 모드는
 * 직접 connect_async + connect_poll을 사용.
 *
 * 호출 체인:
 *   ops.ctrlr_connect_qpair → [이 함수]
 *     → nvme_fabric_qpair_connect_async (SQE 발사)
 *     → busy-wait { nvme_fabric_qpair_connect_poll } (응답 도착 대기)
 */
int
nvme_fabric_qpair_connect(struct spdk_nvme_qpair *qpair, uint32_t num_entries)
{
	int rc;

	rc = nvme_fabric_qpair_connect_async(qpair, num_entries);
	/* [한국어] 비동기 연결 시작 — SQE 발사 + status 보존. */
	if (rc) {
		return rc;
		/* [한국어] 발사 자체가 실패면 즉시 반환 (자원은 connect_async에서 이미 정리됨). */
	}

	do {
		/* Wait until the command completes or times out */
		rc = nvme_fabric_qpair_connect_poll(qpair);
		/* [한국어] 매 라운드에 1번의 process_completions 수행. -EAGAIN이면 다시. busy-wait이므로 다른
		 * 작업과 reactor 공유 안 함 — 부팅 단계에만 적합. */
	} while (rc == -EAGAIN);

	return rc;
	/* [한국어] -EAGAIN이 아닌 첫 결과 = 0(성공) / -ECANCELED(타임아웃) / -EIO(에러) 중 하나. */
}
