/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2024 Samsung Electronics Co., Ltd. All rights reserved.
 *   Copyright (c) 2025, Oracle and/or its affiliates.
 */

/*
 * [한국어 설명] NVMe-oF Target 컨트롤러(ctrlr) 라이프사이클 및 명령 디스패처 (ctrlr.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 SPDK NVMe-over-Fabrics(NVMf) Target 측에서 "컨트롤러(spdk_nvmf_ctrlr)"
 * 객체의 라이프사이클(생성/Connect/Property Get/Set/AER/Keep-Alive/Shutdown/소멸)과
 * 호스트가 보낸 NVMe Admin/Fabric/I/O 명령을 적절한 핸들러로 디스패치하는 핵심
 * 로직을 구현한다. 호스트가 Fabric Connect Capsule(qid=0)을 보내면 본 파일의
 * nvmf_ctrlr_create()가 컨트롤러 객체를 만들고, 후속 Connect(qid>0)는 IO QPair를
 * 동일 컨트롤러에 등록하여 멀티 큐 페어 NVMe 세션을 구성한다.
 * 이후 호스트가 보낸 Admin 명령(Identify/Get Log Page/Set Features/AER 등)은
 * nvmf_ctrlr_process_admin_cmd()에서, IO 명령은 nvmf_ctrlr_process_io_cmd()에서
 * 처리되며, Fabric 캡슐 명령(Property Get/Set/Authentication Send/Recv)은
 * nvmf_ctrlr_process_fabrics_cmd()에서 처리된다. 컨트롤러 라이프사이클의
 * 종결(Keep-Alive 만료, Shutdown 요청, 명시적 disconnect)도 본 파일의 타이머
 * 폴러와 disconnect 헬퍼에서 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (수신 경로):
 *   transport(rdma/tcp/fc/vfio) → spdk_nvmf_request_exec() → 본 파일의
 *   nvmf_ctrlr_process_{fabrics,admin,io}_cmd → 개별 핸들러 → nvmf_bdev_ctrlr_*
 *   → bdev 레이어 → bdev 모듈 (예: bdev_nvme/bdev_malloc) → 백엔드
 * 호출 체인 (응답 경로):
 *   bdev 완료 콜백 → spdk_nvmf_request_complete() → _nvmf_request_complete()
 *   (해당 컨트롤러를 소유한 SPDK thread로 메시지 전송)
 *   → nvmf_transport_req_complete() → transport가 RDMA/TCP/FC로 CQE/Capsule 송신
 * 실행 컨텍스트:
 *   - 컨트롤러 객체는 admin qpair를 처음 받은 SPDK thread(=poll group)에
 *     "고정(thread affinity)"되며 이후 모든 컨트롤러 상태 변경은 ctrlr->thread에서만
 *     수행된다. 다른 스레드에서의 작업은 spdk_thread_send_msg()로 위임된다.
 *   - polled-mode: keep_alive_poller, association_timer, cc_timeout_timer 등은
 *     모두 SPDK_POLLER_REGISTER로 등록되어 reactor 루프에서 주기적으로 호출된다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/nvmf/subsystem.c: spdk_nvmf_subsystem 객체와 호스트/리스너 ACL을 관리한다.
 *   본 파일은 nvmf_subsystem_add_ctrlr/remove_ctrlr/host_allowed/listener_allowed
 *   등을 호출하여 subsystem과 컨트롤러를 결합한다.
 * - lib/nvmf/transport.c, rdma.c, tcp.c, fc.c: transport-specific qpair을 생성하고,
 *   본 파일의 nvmf_ctrlr_add_qpair()로 컨트롤러에 등록한다. 응답 송신은
 *   nvmf_transport_req_complete()를 통해 transport에 위임된다.
 * - lib/nvmf/ctrlr_bdev.c: nvmf_bdev_ctrlr_read/write/flush/identify 등 NVMe ↔ bdev
 *   I/O 변환 함수가 정의되어 있으며, 본 파일이 호출자(caller)이다.
 * - lib/nvmf/auth.c: nvmf_qpair_auth_init/nvmf_auth_request_exec 등 DH-HMAC-CHAP
 *   인증 흐름을 제공하며, Connect 응답 시 인증 필요 여부에 따라 본 파일이 위임한다.
 * - lib/nvmf/ctrlr_discovery.c: Discovery Subsystem 전용 Get Log Page(LID=0x70)를
 *   처리하는 nvmf_get_discovery_log_page_async()를 본 파일이 호출한다.
 * - include/spdk/nvme_spec.h: NVMe 1.x/2.x 스펙의 명령/완료/레지스터 비트필드 정의.
 *   본 파일은 spec 정의를 직접 사용하여 호스트와 wire-compatible한 응답을 만든다.
 *
 * === 주요 함수/구조체 요약 ===
 * - nvmf_ctrlr_create(): qid=0(admin) Connect 처리 중 호출되어 spdk_nvmf_ctrlr를
 *   할당하고 vcprop(가상 컨트롤러 레지스터)/cdata(Identify Controller 데이터)를
 *   초기화한다. KAS, Number of Queues, ANA, Discovery 여부에 따라 다른 기본값을
 *   적용하고, qpair_mask/visible_ns 등 비트맵을 만든다.
 * - nvmf_ctrlr_destruct(): subsystem에서 컨트롤러를 제거한 뒤 ctrlr->thread로
 *   메시지를 보내 _nvmf_ctrlr_destruct()에서 실제 free를 수행한다.
 * - nvmf_ctrlr_cmd_connect() / _nvmf_ctrlr_connect() / _nvmf_ctrlr_add_io_qpair():
 *   Fabric Connect 명령 처리. qid=0이면 새 컨트롤러 생성, qid>0이면 기존
 *   컨트롤러에 IO qpair를 추가한다.
 * - nvmf_property_get() / nvmf_property_set(): NVMe-oF Property Get/Set Capsule.
 *   호스트가 가상 컨트롤러 레지스터(CC/CSTS/CAP/VS/AQA/ASQ/ACQ/CRTO)를 읽거나
 *   쓰는 wire 진입점. nvmf_props[] 테이블이 매핑을 담당한다.
 * - nvmf_prop_set_cc(): CC 레지스터 변경 처리 (EN/SHN/IOSQES/IOCQES). Enable이
 *   1이면 CSTS.RDY=1로 만들고, Shutdown/Disable이면 IO qpair들을 끊고 association
 *   타이머를 시작한다.
 * - nvmf_ctrlr_keep_alive_poll() / nvmf_ctrlr_async_event_*(): Keep-Alive 만료
 *   감시와 AER(Async Event Request) 통지(NS Attribute Change/ANA Change/Discovery
 *   Log Change/Reservation Notice/Error 등) 발송.
 * - nvmf_ctrlr_get_log_page(): Get Log Page 명령. SUPPORTED_LOG_PAGES, ERROR,
 *   FIRMWARE_SLOT, ANA, COMMAND_EFFECTS, CHANGED_NS_LIST, RESERVATION_NOTIFICATION,
 *   FEATURE_IDS_EFFECTS, NVME_MI_EFFECTS, DISCOVERY 등을 분기한다.
 * - nvmf_ctrlr_identify(): Identify 명령. CNS 값에 따라 NS/CTRLR/Active NS List/
 *   NS ID Descriptor/IOCS-specific 데이터를 만든다.
 * - nvmf_ctrlr_process_admin_cmd() / nvmf_ctrlr_process_io_cmd():
 *   각각 Admin/IO 명령의 최상위 디스패처. opcode별 핸들러 분기 + 디스커버리
 *   컨트롤러 한정 검사 + ANA 상태 검사 + Reservation 충돌 검사 등을 수행.
 * - spdk_nvmf_request_exec(): transport가 호출하는 본 파일의 진입점.
 *   subsystem/qpair 활성 검사 후 위 디스패처들로 분기.
 * - spdk_nvmf_request_complete() / _nvmf_request_complete(): 모든 NVMe 응답이
 *   거치는 중앙 완료 경로. outstanding 큐에서 빼고, mgmt_io_outstanding/
 *   io_outstanding 카운터를 감소시키고, subsystem이 PAUSING이면 일괄 PAUSED 전환.
 * - struct spdk_nvmf_custom_admin_cmd: bdev 모듈이 등록한 사용자 정의 admin 핸들러.
 * - struct nvmf_prop: vcprop 가상 레지스터의 offset/size/get_cb/set_cb 매핑 테이블.
 */

#include "spdk/stdinc.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/bdev.h"
#include "spdk/bdev_zone.h"
#include "spdk/bit_array.h"
#include "spdk/endian.h"
#include "spdk/thread.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf_cmd.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/version.h"
#include "spdk/log.h"
#include "spdk_internal/usdt.h"

/* [한국어] CC.SHN(Shutdown Notification)/CC.EN=0(Reset) 처리 후 IO qpair가 모두
 * 정리되기를 기다리는 자체 타임아웃(밀리초). NVMe 호스트 측 reset/shutdown
 * 타임아웃보다 짧게 잡아, 호스트가 timeout 으로 강제 reset하기 전에 컨트롤러가
 * 깔끔하게 fatal status 표시 등을 할 수 있게 한다. */
#define NVMF_CC_RESET_SHN_TIMEOUT_IN_MS	10000

/* [한국어] 위 타임아웃 + 5초 여유. 호스트 측이 컨트롤러 ready/shutdown complete를
 * 기다리는 최대 시간으로 사용되며, vcprop.cap.bits.to (500ms 단위)에도 반영된다. */
#define NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS	(NVMF_CC_RESET_SHN_TIMEOUT_IN_MS + 5000)

/* [한국어] IO Connect 시 동일 QID가 비트맵에 이미 set되어 있을 때 재시도 간격(μs).
 * 호스트가 같은 QID로 재접속을 시도하는 동안 이전 qpair 정리가 비동기로 끝나기를
 * 기다리는 폴러 주기. 너무 짧으면 비트맵 race가, 너무 길면 호스트가 connect timeout
 * 을 보게 된다. */
#define DUPLICATE_QID_RETRY_US 1000

/*
 * Report the SPDK version as the firmware revision.
 * SPDK_VERSION_STRING won't fit into FR (only 8 bytes), so try to fit the most important parts.
 */
/* [한국어] Identify Controller 응답의 FR(Firmware Revision) 8바이트에 들어갈 SPDK
 * 버전 문자열. SPDK_VERSION_STRING 전체는 8바이트를 초과하므로 major.minor.patch
 * 부분만 연결한다. NVMe 호스트의 nvme-cli가 "nvme list" 등에서 표시한다. */
#define FW_VERSION SPDK_VERSION_MAJOR_STRING SPDK_VERSION_MINOR_STRING SPDK_VERSION_PATCH_STRING

/* [한국어] ANA(Asymmetric Namespace Access) 상태 전이 시 호스트가 기다려야 할
 * 최대 시간(초). Identify Controller의 anatt 필드로 보고된다 (NVMe 1.4 8.18). */
#define ANA_TRANSITION_TIME_IN_SEC 10

/* [한국어] Identify Controller의 ACL(Abort Command Limit) 필드 값 (0-based).
 * 동시에 처리할 수 있는 abort 명령 수 = 이 값 + 1. SPDK는 실제 제한이 없지만
 * 스펙 권장값을 따른다 (NVMe 1.4 5.15). */
#define NVMF_ABORT_COMMAND_LIMIT 3

/*
 * Support for custom admin command handlers
 */
/* [한국어] bdev 모듈(예: bdev_nvme)이 특정 admin opcode를 가로채서 직접 처리할 수
 * 있게 해주는 후크 등록 테이블의 단위 항목. spdk_nvmf_set_custom_admin_cmd_hdlr()/
 * spdk_nvmf_set_passthru_admin_cmd()로 등록되며, nvmf_ctrlr_process_admin_cmd()
 * 안에서 표준 디스패치 직전에 hdlr가 우선 호출된다. */
struct spdk_nvmf_custom_admin_cmd {
	spdk_nvmf_custom_cmd_hdlr hdlr;
	/* [한국어] 등록된 사용자 콜백.
	 * 설정자: spdk_nvmf_set_custom_admin_cmd_hdlr()/spdk_nvmf_set_passthru_admin_cmd().
	 * 읽는 자: nvmf_ctrlr_process_admin_cmd()가 cmd->opc 진입 직후 비교.
	 * 값 범위: NULL이면 표준 처리, non-NULL이면 호출되어 SPDK_NVMF_REQUEST_EXEC_*
	 * 상태를 반환해야 한다.
	 * 동기화: 단일 ctrlr->thread에서만 읽히므로 락 없음 (하지만 등록은 보통
	 * RPC 콜백/초기화 시점에 한 번만). */

	uint32_t nsid; /* nsid to forward */
	/* [한국어] passthru가 enable일 때 명령을 어느 NSID의 bdev로 forward할지.
	 * 0이면 cmd->nsid를 그대로 사용. spdk_nvmf_set_passthru_admin_cmd()가 설정.
	 * nvmf_passthru_admin_cmd()에서 읽는다. */
};

/* [한국어] opcode(0~SPDK_NVME_MAX_OPC) 별 사용자 admin 핸들러 전역 테이블.
 * NVMe admin opcode는 1바이트(0~255). bdev_nvme 모듈이 startup 시 등록한 후
 * 모든 ctrlr 객체가 공유한다. 동기화: 등록은 초기화 단계에서만 발생한다고 가정. */
static struct spdk_nvmf_custom_admin_cmd g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_MAX_OPC + 1];

/* [한국어] 전방 선언들. _nvmf_request_complete: 응답 송신 + 카운터 정리 (스레드
 * 메시지 콜백). nvmf_passthru_admin_cmd_for_ctrlr: 컨트롤러 첫 NS로 admin 명령
 * 우회. nvmf_passthru_admin_cmd: cmd->nsid 또는 hdlrs[opc].nsid로 우회. */
static void _nvmf_request_complete(void *ctx);
int nvmf_passthru_admin_cmd_for_ctrlr(struct spdk_nvmf_request *req, struct spdk_nvmf_ctrlr *ctrlr);
static int nvmf_passthru_admin_cmd(struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_invalid_connect_response - Connect Capsule에 INVALID_PARAM 상태를 채운다
 *
 * @rsp: Connect 응답 캡슐 포인터.
 * @iattr: 0=command capsule 내 잘못된 필드, 1=data capsule(connect_data) 내 잘못된 필드.
 * @ipo: 잘못된 필드의 byte offset (connect_cmd 또는 connect_data 구조체 기준).
 *
 * NVMe-oF 1.1 스펙 (Figure 28 Fabric Status) 의 INVALID_PARAM (sct=COMMAND_SPECIFIC,
 * sc=02h) 형식으로 응답을 만들고, 어느 필드가 문제인지 호스트에 알려준다.
 * SPDK_NVMF_INVALID_CONNECT_CMD/DATA 매크로의 공통 헬퍼.
 */
static inline void
nvmf_invalid_connect_response(struct spdk_nvmf_fabric_connect_rsp *rsp,
			      uint8_t iattr, uint16_t ipo)
{
	rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC; /* [한국어] SCT=01h Command Specific */
	rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM; /* [한국어] SC=02h Invalid Parameter */
	rsp->status_code_specific.invalid.iattr = iattr; /* [한국어] cmd vs data 구분 비트 */
	rsp->status_code_specific.invalid.ipo = ipo; /* [한국어] 잘못된 필드 오프셋 */
}

/* [한국어] Connect Command Capsule(SQE 본문) 내 잘못된 필드를 보고할 때 사용.
 * iattr=0, ipo=struct spdk_nvmf_fabric_connect_cmd 의 해당 필드 offset. */
#define SPDK_NVMF_INVALID_CONNECT_CMD(rsp, field)	\
	nvmf_invalid_connect_response(rsp, 0, offsetof(struct spdk_nvmf_fabric_connect_cmd, field))
/* [한국어] Connect Data Capsule(이어붙은 in-capsule 데이터) 내 잘못된 필드를 보고.
 * iattr=1, ipo=struct spdk_nvmf_fabric_connect_data 의 해당 필드 offset. */
#define SPDK_NVMF_INVALID_CONNECT_DATA(rsp, field)	\
	nvmf_invalid_connect_response(rsp, 1, offsetof(struct spdk_nvmf_fabric_connect_data, field))


/*
 * [한국어]
 * nvmf_ctrlr_stop_keep_alive_timer - Keep-Alive 만료 감시 폴러를 해제한다
 *
 * @ctrlr: 컨트롤러 객체. NULL이면 ERRLOG만 찍고 반환.
 *
 * Keep-Alive(Admin opcode 18h, NVMe 1.4 Section 5.18)는 호스트가 KATO 시간 안에
 * 이 명령을 다시 보내지 않으면 컨트롤러가 association을 끊도록 하는 watchdog이다.
 * 본 함수는 association을 끊거나 컨트롤러가 destruct될 때 호출되어 폴러를 unregister한다.
 * 호출 컨텍스트: ctrlr->thread (ctrlr 라이프사이클을 관리하는 스레드).
 *
 * 호출 체인:
 *   nvmf_ctrlr_keep_alive_poll(만료 감지) → 본 함수
 *   _nvmf_ctrlr_destruct → 본 함수
 *   nvmf_prop_set_cc(SHN 진입) → 본 함수
 */
static void
nvmf_ctrlr_stop_keep_alive_timer(struct spdk_nvmf_ctrlr *ctrlr)
{
	if (!ctrlr) { /* [한국어] 방어적 NULL 체크 - 호출자가 항상 valid를 줘야 정상 */
		SPDK_ERRLOG("Controller is NULL\n");
		return;
	}

	if (ctrlr->keep_alive_poller == NULL) { /* [한국어] 이미 멈춰있거나 KATO=0이라 등록되지 않았을 수 있음 */
		return;
	}

	SPDK_DEBUGLOG(nvmf, "Stop keep alive poller\n");
	spdk_poller_unregister(&ctrlr->keep_alive_poller); /* [한국어] reactor 폴러 리스트에서 제거 (포인터는 NULL로 set됨) */
}

/*
 * [한국어]
 * nvmf_ctrlr_stop_association_timer - "association preserve" 타임아웃 폴러 해제
 *
 * @ctrlr: 컨트롤러 객체.
 *
 * NVMe-oF 스펙은 CC.EN이 1→0으로 전이된 후에도 일정 시간(association_timeout)
 * 동안 호스트-컨트롤러 association을 유지해야 한다고 규정한다 (스펙 Figure 26 등).
 * 호스트가 이 시간 안에 다시 CC.EN=1을 쓰면 동일 ctrlr 객체가 재사용된다.
 * 호스트가 다시 enable하지 않으면 nvmf_ctrlr_association_remove() 폴러가 발화하여
 * admin qpair을 disconnect한다. 본 함수는 그 폴러를 정리한다.
 */
static void
nvmf_ctrlr_stop_association_timer(struct spdk_nvmf_ctrlr *ctrlr)
{
	if (!ctrlr) {
		SPDK_ERRLOG("Controller is NULL\n");
		assert(false); /* [한국어] 정상 흐름에서는 절대 NULL이 와선 안 됨 */
		return;
	}

	if (ctrlr->association_timer == NULL) { /* [한국어] CC.EN=0 상태가 아니거나 이미 해제됨 */
		return;
	}

	SPDK_DEBUGLOG(nvmf, "Stop association timer\n");
	spdk_poller_unregister(&ctrlr->association_timer);
}

/*
 * [한국어]
 * nvmf_ctrlr_disconnect_qpairs_done - spdk_for_each_channel 종료 콜백 (로깅용)
 *
 * @i: 채널 iterator (사용 안 함).
 * @status: 0=모든 poll group 순회 성공, <0=실패.
 *
 * spdk_for_each_channel(target, fn, ctx, done)에서 모든 reactor의 poll group을
 * 순회한 뒤 호출되는 종료 후크. 본 함수는 결과 로깅만 하고 추가 작업은 없다.
 * (Keep-Alive 만료 시점의 IO/Admin qpair 일괄 disconnect 흐름의 마무리.)
 */
static void
nvmf_ctrlr_disconnect_qpairs_done(struct spdk_io_channel_iter *i, int status)
{
	if (status == 0) {
		SPDK_DEBUGLOG(nvmf, "ctrlr disconnect qpairs complete successfully\n");
	} else {
		SPDK_ERRLOG("Fail to disconnect ctrlr qpairs\n");
	}
}

/*
 * [한국어]
 * _nvmf_ctrlr_disconnect_qpairs_on_pg - 한 poll group의 qpair 중 ctrlr 소유분 끊기
 *
 * @i: spdk_for_each_channel iterator. ctx에 spdk_nvmf_ctrlr*가 들어 있다.
 * @include_admin: true면 admin qpair도 끊고, false면 IO qpair만 끊는다.
 * @return: 0 또는 spdk_nvmf_qpair_disconnect()의 음수 에러.
 *
 * 각 reactor에 배치된 poll group을 순회하면서 해당 group이 들고 있는 qpair 중
 * 본 컨트롤러 소유(qpair->ctrlr == ctrlr)인 것들을 disconnect한다. EINPROGRESS는
 * "비동기 진행 중"의 정상 흐름이라 0으로 정규화한다.
 *
 * 호출 컨텍스트: 각 poll group의 SPDK thread (spdk_for_each_channel가 메시지로
 * 디스패치). qpair는 그 thread에 소속되므로 안전하게 disconnect 가능.
 */
static int
_nvmf_ctrlr_disconnect_qpairs_on_pg(struct spdk_io_channel_iter *i, bool include_admin)
{
	int rc = 0; /* [한국어] 반환 코드. 오류 발생 시 음수, 성공/EINPROGRESS 정규화 시 0 */
	struct spdk_nvmf_ctrlr *ctrlr; /* [한국어] disconnect 대상 컨트롤러 (iter ctx에서 추출) */
	struct spdk_nvmf_qpair *qpair, *temp_qpair; /* [한국어] 순회 qpair 포인터. SAFE: 삭제하면서 순회 가능 */
	struct spdk_io_channel *ch; /* [한국어] 현재 reactor의 I/O 채널 */
	struct spdk_nvmf_poll_group *group; /* [한국어] 채널에 연결된 poll group (qpairs 리스트 보유) */

	ctrlr = spdk_io_channel_iter_get_ctx(i); /* [한국어] spdk_for_each_channel에 전달된 ctx (컨트롤러 포인터) 추출 */
	ch = spdk_io_channel_iter_get_channel(i); /* [한국어] 이 reactor의 I/O 채널 (nvmf poll group 채널) */
	group = spdk_io_channel_get_ctx(ch); /* [한국어] 채널 ctx에서 poll group 구조체 추출 */

	/* [한국어] 이 poll group의 모든 qpair를 순회하면서 대상 ctrlr 소속인 것을 찾아 disconnect.
	 * FOREACH_SAFE: disconnect 중 qpair가 리스트에서 제거될 수 있으므로 temp_qpair로 다음 저장 */
	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, temp_qpair) {
		if (qpair->ctrlr == ctrlr && (include_admin || !nvmf_qpair_is_admin_queue(qpair))) {
			/* [한국어] 이 ctrlr 소속이고 (admin 포함 또는 IO만 대상)인 qpair → disconnect */
			rc = spdk_nvmf_qpair_disconnect(qpair);
			if (rc) {
				if (rc == -EINPROGRESS) {
					/* [한국어] EINPROGRESS: 이미 비동기 disconnect 진행 중 → 정상 경로, 0으로 정규화 */
					rc = 0;
				} else {
					/* [한국어] 실제 오류: 로그 출력 후 순회 중단 */
					SPDK_ERRLOG("Qpair disconnect failed\n");
					return rc;
				}
			}
		}
	}

	return rc; /* [한국어] 성공 시 0, 오류 시 음수 (-EINPROGRESS는 이미 0으로 변환됨) */
}

/*
 * [한국어]
 * nvmf_ctrlr_disconnect_qpairs_on_pg - per-pg 콜백: admin 포함 모든 qpair 해제
 * Keep-Alive 만료 시 호스트와의 연결을 완전히 끊을 때 사용된다.
 */
static void
nvmf_ctrlr_disconnect_qpairs_on_pg(struct spdk_io_channel_iter *i)
{
	spdk_for_each_channel_continue(i, _nvmf_ctrlr_disconnect_qpairs_on_pg(i, true));
}

/*
 * [한국어]
 * nvmf_ctrlr_disconnect_io_qpairs_on_pg - per-pg 콜백: IO qpair만 해제 (admin 보존)
 * CC.EN=1→0 또는 SHN(Shutdown) 진입 시 사용된다. admin qpair는 호스트가 다시
 * 사용할 수 있도록 보존하고, IO qpair만 닫아 in-flight IO를 중단한다.
 */
static void
nvmf_ctrlr_disconnect_io_qpairs_on_pg(struct spdk_io_channel_iter *i)
{
	spdk_for_each_channel_continue(i, _nvmf_ctrlr_disconnect_qpairs_on_pg(i, false));
}

/*
 * [한국어]
 * nvmf_ctrlr_keep_alive_poll - KATO 만료 감시 폴러 (주기 = KATO ms)
 *
 * @ctx: 컨트롤러 객체 포인터 (poller 등록 시 전달됨).
 * @return: SPDK_POLLER_IDLE/BUSY (work 수행 여부). reactor의 통계용.
 *
 * 마지막 Keep-Alive 명령 이후 KATO 시간이 흐르면 호스트가 죽었다고 간주하여
 * CSTS.CFS(Controller Fatal Status)=1을 set하고 모든 qpair를 끊는다. 정상
 * 호스트는 KATO 절반 정도마다 Keep-Alive를 보내므로 last_keep_alive_tick이
 * 자주 갱신된다.
 *
 * 호출 컨텍스트: ctrlr->thread (Keep-Alive Admin 명령도 같은 스레드에서
 * last_keep_alive_tick을 갱신하므로 락 불필요).
 *
 * 호출 체인:
 *   reactor 폴 루프 → 본 함수 → nvmf_ctrlr_set_fatal_status + spdk_for_each_channel
 *     → nvmf_ctrlr_disconnect_qpairs_on_pg → spdk_nvmf_qpair_disconnect
 */
static int
nvmf_ctrlr_keep_alive_poll(void *ctx)
{
	uint64_t keep_alive_timeout_tick; /* [한국어] 만료 절대 tick값 = last_keep_alive_tick + KATO ticks */
	uint64_t now = spdk_get_ticks(); /* [한국어] 현재 시각 (TSC 기반 절대 tick) */
	struct spdk_nvmf_ctrlr *ctrlr = ctx; /* [한국어] 이 폴러를 소유한 컨트롤러 */

	if (ctrlr->in_destruct) {
		/* [한국어] 컨트롤러 소멸 중: 폴러를 즉시 해제하고 IDLE 반환 */
		nvmf_ctrlr_stop_keep_alive_timer(ctrlr);
		return SPDK_POLLER_IDLE;
	}

	SPDK_DEBUGLOG(nvmf, "Polling ctrlr keep alive timeout\n");

	/* If the Keep alive feature is in use and the timer expires */
	/* [한국어] 만료 시각 계산: last_keep_alive_tick + KATO(ms) × (ticks_hz / 1000)
	 * KATO는 ms 단위이고 ticks_hz는 1초당 tick 수이므로 나눗셈으로 ms→ticks 변환 */
	keep_alive_timeout_tick = ctrlr->last_keep_alive_tick +
				  ctrlr->feat.keep_alive_timer.bits.kato * spdk_get_ticks_hz() / UINT64_C(1000);
	if (now > keep_alive_timeout_tick) {
		/* [한국어] KATO 경과: 호스트가 응답하지 않음 → 연결 강제 종료 */
		SPDK_NOTICELOG("Disconnecting host %s from subsystem %s due to keep alive timeout.\n",
			       ctrlr->hostnqn, ctrlr->subsys->subnqn);
		/* set the Controller Fatal Status bit to '1' */
		/* [한국어] CSTS.CFS=0인 경우에만 처리 (이미 fatal이면 재처리 불필요) */
		if (ctrlr->vcprop.csts.bits.cfs == 0) {
			nvmf_ctrlr_set_fatal_status(ctrlr); /* [한국어] CSTS.CFS=1 설정: 호스트에 컨트롤러 치명 오류 표시 */

			/*
			 * disconnect qpairs, terminate Transport connection
			 * destroy ctrlr, break the host to controller association
			 * disconnect qpairs with qpair->ctrlr == ctrlr
			 */
			/* [한국어] 모든 reactor의 poll group을 순회하며 이 ctrlr 소속 qpair를 모두 disconnect.
			 * 완료 후 nvmf_ctrlr_disconnect_qpairs_done이 호출되고, 각 qpair disconnect는
			 * 연쇄적으로 ctrlr destruct를 유발한다 */
			spdk_for_each_channel(ctrlr->subsys->tgt,
					      nvmf_ctrlr_disconnect_qpairs_on_pg, /* [한국어] per-poll-group 콜백 */
					      ctrlr, /* [한국어] ctx로 ctrlr 포인터 전달 */
					      nvmf_ctrlr_disconnect_qpairs_done); /* [한국어] 모든 순회 완료 후 로깅 */
			return SPDK_POLLER_BUSY; /* [한국어] 이 사이클에서 disconnect 작업을 시작했음 */
		}
	}

	return SPDK_POLLER_IDLE; /* [한국어] KATO 아직 만료 안 됨 또는 이미 CFS=1 */
}

/*
 * [한국어]
 * nvmf_ctrlr_start_keep_alive_timer - KATO 폴러를 등록한다
 *
 * @ctrlr: 컨트롤러 객체. KATO=0이면 아무 일도 하지 않음.
 *
 * Keep-Alive 기능이 enable(KATO≠0)된 경우 last_keep_alive_tick를 현재 ticks로
 * 초기화하고, KATO ms 주기의 폴러를 등록한다. KATO 단위는 ms이며 SPDK_POLLER_REGISTER
 * 의 마지막 인자는 μs이므로 *1000을 곱한다.
 *
 * 호출 컨텍스트: ctrlr->thread. _nvmf_ctrlr_add_admin_qpair() 또는
 * nvmf_ctrlr_set_features_keep_alive_timer()에서 호출된다.
 */
static void
nvmf_ctrlr_start_keep_alive_timer(struct spdk_nvmf_ctrlr *ctrlr)
{
	if (!ctrlr) {
		SPDK_ERRLOG("Controller is NULL\n");
		return;
	}

	/* if cleared to 0 then the Keep Alive Timer is disabled */
	if (ctrlr->feat.keep_alive_timer.bits.kato != 0) { /* [한국어] KATO=0이면 KA 비활성 (스펙 5.18) */

		ctrlr->last_keep_alive_tick = spdk_get_ticks(); /* [한국어] 시계 origin = 지금. 첫 만료 = now+KATO */

		SPDK_DEBUGLOG(nvmf, "Ctrlr add keep alive poller\n");
		ctrlr->keep_alive_poller = SPDK_POLLER_REGISTER(nvmf_ctrlr_keep_alive_poll, ctrlr,
					   ctrlr->feat.keep_alive_timer.bits.kato * 1000); /* [한국어] ms→μs */
	}
}

/*
 * [한국어]
 * nvmf_qpair_set_ctrlr - qpair에 컨트롤러 포인터를 결합한다 (idempotent)
 *
 * @qpair: NVMe-oF qpair (admin or IO).
 * @ctrlr: 결합 대상 컨트롤러.
 *
 * Connect 처리 도중 qpair->ctrlr를 set하는 헬퍼. admin qpair의 경우 _nvmf_subsystem_add_ctrlr
 * 와 _nvmf_ctrlr_add_admin_qpair 양쪽에서 set 시도가 발생하므로 두 번 호출되어도
 * 무해하도록 idempotent로 작성됨. trace_id에 subsystem NQN 문자열을 추가하여
 * SPDK trace 분석 시 구분되게 한다.
 */
static void
nvmf_qpair_set_ctrlr(struct spdk_nvmf_qpair *qpair, struct spdk_nvmf_ctrlr *ctrlr)
{
	if (qpair->ctrlr != NULL) {
		/* Admin queues will call this function twice. */
		assert(qpair->ctrlr == ctrlr); /* [한국어] 두 번째 호출이라면 같은 ctrlr여야 함 */
		return;
	}

	qpair->ctrlr = ctrlr; /* [한국어] 이후 모든 dispatch는 qpair->ctrlr를 통해 ctrlr 접근 */
	spdk_trace_owner_append_description(qpair->trace_id,
					    spdk_nvmf_subsystem_get_nqn(ctrlr->subsys));
}

static int _retry_qid_check(void *ctx);

/*
 * [한국어]
 * nvmf_ctrlr_send_connect_rsp - Connect 명령에 대한 최종 응답 캡슐을 송신한다
 *
 * @ctx: spdk_nvmf_request* (메시지 콜백 인자).
 *
 * Connect 처리의 마지막 단계. 인증이 필요한 subsystem이면 qpair을
 * AUTHENTICATING 상태로 두고 응답의 ATR(authreq) 비트를 1로 표시하여 호스트가
 * Authentication Send/Recv를 시작하도록 알린다. 인증이 필요 없으면 바로 ENABLED
 * 로 전환한다. 응답에는 새로 할당된 cntlid를 채워 넣는다 (admin Connect의 경우).
 *
 * 호출 컨텍스트: qpair->group->thread (qpair_set_state, request_complete가
 * 그 스레드에서 수행되어야 하므로 spdk_thread_send_msg로 진입).
 *
 * 호출 체인:
 *   nvmf_ctrlr_add_qpair → spdk_thread_send_msg(qpair->group->thread, 본 함수)
 *   → spdk_nvmf_request_complete → transport가 Connect Response Capsule 송신
 */
static void
nvmf_ctrlr_send_connect_rsp(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	int rc;

	/* The qpair might have been disconnected in the meantime */
	assert(qpair->state == SPDK_NVMF_QPAIR_CONNECTING ||
	       qpair->state == SPDK_NVMF_QPAIR_DEACTIVATING);
	if (qpair->state == SPDK_NVMF_QPAIR_CONNECTING) {
		if (nvmf_subsystem_host_auth_required(ctrlr->subsys, ctrlr->hostnqn)) {
			rc = nvmf_qpair_auth_init(qpair);
			if (rc != 0) {
				rsp->status.sct = SPDK_NVME_SCT_GENERIC;
				rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				spdk_nvmf_request_complete(req);
				spdk_nvmf_qpair_disconnect(qpair);
				return;
			}
			rsp->status_code_specific.success.authreq.atr = 1;
			nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_AUTHENTICATING);
		} else {
			nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_ENABLED);
		}
	}

	SPDK_DEBUGLOG(nvmf, "connect capsule response: cntlid = 0x%04x\n", ctrlr->cntlid);

	assert(spdk_get_thread() == qpair->group->thread);
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	rsp->status_code_specific.success.cntlid = ctrlr->cntlid;
	spdk_nvmf_request_complete(req);
}

/*
 * [한국어]
 * nvmf_ctrlr_add_qpair - 컨트롤러의 qpair 비트맵에 새 qpair를 등록한다
 *
 * @qpair: 등록할 qpair.
 * @ctrlr: 소속 컨트롤러.
 * @req: Connect 요청 객체. 처리 결과(success/error)를 채워 응답한다.
 *
 * Connect 흐름의 핵심 단계로, 다음을 수행한다:
 * 1) admin_qpair가 사라졌으면 INVALID_PARAM으로 거절.
 * 2) 동일 QID가 이미 비트맵에 set이면 (a) 진행 중인 connect_req이 없으면 즉시
 *    INVALID_QUEUE_IDENTIFIER로 거절, (b) 있으면 DUPLICATE_QID_RETRY_US 폴러를
 *    걸어 잠시 후 재시도 (이전 qpair의 disconnect가 진행 중일 가능성).
 * 3) 비트맵에 set하고 성공 응답을 admin qpair의 thread로 보낸다.
 *
 * 호출 컨텍스트: ctrlr->thread (admin qpair가 소속된 SPDK thread). admin이
 * 사라진 경우의 보호 로직이 들어 있다.
 */
static void
nvmf_ctrlr_add_qpair(struct spdk_nvmf_qpair *qpair,
		     struct spdk_nvmf_ctrlr *ctrlr,
		     struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;

	if (!ctrlr->admin_qpair) {
		SPDK_ERRLOG("Inactive admin qpair\n");
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		qpair->connect_req = NULL;
		qpair->ctrlr = NULL;
		spdk_nvmf_request_complete(req);
		return;
	}

	assert(ctrlr->admin_qpair->group->thread == spdk_get_thread());

	if (spdk_bit_array_get(ctrlr->qpair_mask, qpair->qid)) {
		if (qpair->connect_req != NULL) {
			SPDK_ERRLOG("Got I/O connect with duplicate QID %u (cntlid:%u)\n",
				    qpair->qid, ctrlr->cntlid);
			rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			rsp->status.sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
			qpair->connect_req = NULL;
			qpair->ctrlr = NULL;
			spdk_nvmf_request_complete(req);
		} else {
			SPDK_WARNLOG("Duplicate QID detected (cntlid:%u, qid:%u), re-check in %dus\n",
				     ctrlr->cntlid, qpair->qid, DUPLICATE_QID_RETRY_US);
			qpair->connect_req = req;
			/* Set qpair->ctrlr here so that we'll have it when the poller expires. */
			nvmf_qpair_set_ctrlr(qpair, ctrlr);
			req->poller = SPDK_POLLER_REGISTER(_retry_qid_check, qpair,
							   DUPLICATE_QID_RETRY_US);
		}
		return;
	}

	qpair->connect_req = NULL;

	SPDK_DTRACE_PROBE4_TICKS(nvmf_ctrlr_add_qpair, qpair, qpair->qid, ctrlr->subsys->subnqn,
				 ctrlr->hostnqn);
	nvmf_qpair_set_ctrlr(qpair, ctrlr);
	spdk_bit_array_set(ctrlr->qpair_mask, qpair->qid);
	SPDK_DEBUGLOG(nvmf, "qpair_mask set, qid %u\n", qpair->qid);

	spdk_thread_send_msg(qpair->group->thread, nvmf_ctrlr_send_connect_rsp, req);
}

/*
 * [한국어]
 * _retry_qid_check - 이전 qpair의 disconnect 완료를 기다린 후 qpair 등록 재시도
 *
 * @ctx: 재시도 대상 spdk_nvmf_qpair*.
 * @return: SPDK_POLLER_BUSY (폴러가 발화했음을 나타냄).
 *
 * DUPLICATE_QID_RETRY_US 후에 한 번 실행되는 one-shot 폴러 콜백.
 * 이전 동일 QID를 가진 qpair의 비동기 disconnect가 완료되었다고 기대하고
 * nvmf_ctrlr_add_qpair를 다시 시도한다. 여전히 duplicate이면 오류 응답한다.
 *
 * 호출 체인:
 *   nvmf_ctrlr_add_qpair(중복 QID + connect_req != NULL) → SPDK_POLLER_REGISTER(본 함수)
 *   → [DUPLICATE_QID_RETRY_US 후] 본 함수 → nvmf_ctrlr_add_qpair
 */
static int
_retry_qid_check(void *ctx)
{
	struct spdk_nvmf_qpair *qpair = ctx; /* [한국어] 재시도할 IO qpair */
	struct spdk_nvmf_request *req = qpair->connect_req; /* [한국어] 보류 중인 Connect 요청 */
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 등록 대상 컨트롤러 */

	spdk_poller_unregister(&req->poller); /* [한국어] one-shot 폴러 해제 - 다음 폴 방지 */
	SPDK_WARNLOG("Retrying adding qpair, qid:%d\n", qpair->qid);
	nvmf_ctrlr_add_qpair(qpair, ctrlr, req); /* [한국어] 비트맵 재확인 + 등록 시도 */
	return SPDK_POLLER_BUSY; /* [한국어] 이 사이클에서 work 수행했음을 reactor에 알림 */
}

/*
 * [한국어]
 * _nvmf_ctrlr_add_admin_qpair - 신규 컨트롤러의 admin qpair를 설정하고 비트맵에 등록
 *
 * @ctx: spdk_nvmf_request* (메시지 콜백 인자).
 *
 * _nvmf_subsystem_add_ctrlr()가 subsystem 등록에 성공한 뒤 ctrlr->thread로
 * 전달하는 메시지 콜백. ctrlr->admin_qpair를 fix하고 Keep-Alive 폴러를 시작하며,
 * nvmf_ctrlr_add_qpair로 비트맵에 QID 0을 set한다.
 *
 * 실행 컨텍스트: ctrlr->thread (admin qpair의 poll group 스레드).
 *
 * 호출 체인:
 *   _nvmf_subsystem_add_ctrlr → spdk_thread_send_msg(ctrlr->thread, 본 함수)
 *   → nvmf_ctrlr_add_qpair → spdk_thread_send_msg(qpair->group->thread, nvmf_ctrlr_send_connect_rsp)
 */
static void
_nvmf_ctrlr_add_admin_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx; /* [한국어] admin Connect 요청 */
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] admin qpair (qid=0) */
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr; /* [한국어] 신규 컨트롤러 */

	ctrlr->admin_qpair = qpair; /* [한국어] admin qpair 포인터 고정 - IO Connect 검증에서 사용 */
	ctrlr->association_timeout = qpair->transport->opts.association_timeout; /* [한국어] CC.EN=0 후 association 보존 시간 (ms) */
	nvmf_ctrlr_start_keep_alive_timer(ctrlr); /* [한국어] KATO≠0이면 keep_alive_poller 등록 */
	nvmf_ctrlr_add_qpair(qpair, ctrlr, req); /* [한국어] qpair_mask[0] set + Connect RSP 전송 */
}

/*
 * [한국어]
 * _nvmf_subsystem_add_ctrlr - subsystem의 ctrlrs 리스트에 새 컨트롤러를 등록
 *
 * @ctx: spdk_nvmf_request* (메시지 콜백 인자).
 *
 * nvmf_ctrlr_create()가 subsystem->thread로 전달하는 메시지 콜백. cntlid 할당과
 * ctrlrs TAILQ 삽입을 subsystem->thread에서 안전하게 수행한다. 실패하면 ctrlr과
 * qpair_mask를 free하고 INTERNAL_DEVICE_ERROR 응답. 성공하면 ctrlr->thread로
 * _nvmf_ctrlr_add_admin_qpair 메시지를 전송해 admin qpair 등록을 계속한다.
 *
 * 실행 컨텍스트: subsystem->thread (subsystem 상태 변경의 단일 소유자).
 *
 * 호출 체인:
 *   nvmf_ctrlr_create → spdk_thread_send_msg(subsystem->thread, 본 함수)
 *   → spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_add_admin_qpair)
 */
static void
_nvmf_subsystem_add_ctrlr(void *ctx)
{
	struct spdk_nvmf_request *req = ctx; /* [한국어] admin Connect 요청 */
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] admin qpair */
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp; /* [한국어] Connect 응답 필드 */
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr; /* [한국어] 신규 컨트롤러 (cntlid 미할당 상태) */

	if (nvmf_subsystem_add_ctrlr(ctrlr->subsys, ctrlr)) {
		/* [한국어] cntlid 할당 실패 또는 subsystem이 accept 불가 상태 */
		SPDK_ERRLOG("Unable to add controller to subsystem\n");
		spdk_bit_array_free(&ctrlr->qpair_mask); /* [한국어] 비트맵 해제 */
		free(ctrlr); /* [한국어] ctrlr 자체 해제 */
		qpair->ctrlr = NULL; /* [한국어] qpair→ctrlr 참조 해제 */
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR; /* [한국어] 호스트에 내부 오류 알림 */
		spdk_nvmf_request_complete(req); /* [한국어] Connect RSP 송신 (오류) */
		return;
	}

	/* [한국어] 성공: ctrlr->thread로 메시지를 보내 admin qpair 등록 계속 */
	spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_add_admin_qpair, req);
}

/*
 * [한국어]
 * nvmf_ctrlr_cdata_init - 컨트롤러의 Identify Controller 응답 데이터(cdata)를 초기화
 *
 * @transport: 이 컨트롤러가 연결된 transport.
 * @subsystem: 컨트롤러가 속한 NVM subsystem.
 * @cdata: 초기화할 spdk_nvmf_ctrlr_data 구조체 포인터 (이미 calloc으로 zeroed).
 *
 * Identify Controller (CNS=01h) 명령 응답의 기본 필드를 transport/subsystem 설정값으로
 * 채운다. 이후 spdk_nvmf_ctrlr_identify_ctrlr()이 더 많은 필드를 추가한다.
 * transport별 cdata_init 콜백이 있으면 추가적으로 호출된다 (예: TCP, RDMA 특화 필드).
 *
 * 실행 컨텍스트: Connect를 받은 qpair->group->thread (nvmf_ctrlr_create 내부).
 *
 * 호출 체인:
 *   nvmf_ctrlr_create → 본 함수 → transport->ops->cdata_init (optional)
 */
static void
nvmf_ctrlr_cdata_init(struct spdk_nvmf_transport *transport, struct spdk_nvmf_subsystem *subsystem,
		      struct spdk_nvmf_ctrlr_data *cdata)
{
	cdata->aerl = SPDK_NVMF_MAX_ASYNC_EVENTS - 1; /* [한국어] Async Event Request Limit (0-based). AERL+1개 AER 허용 */
	cdata->kas = transport->opts.kas; /* [한국어] Keep Alive Granularity (100ms 단위). 0이면 KA 미지원 */
	cdata->vid = SPDK_PCI_VID_INTEL; /* [한국어] Vendor ID: Intel (0x8086) */
	cdata->ssvid = SPDK_PCI_VID_INTEL; /* [한국어] Subsystem Vendor ID: Intel (0x8086) */
	/* INTEL OUI */
	cdata->ieee[0] = 0xe4; /* [한국어] IEEE OUI 바이트0 (Intel OUI: e4:d2:5c) */
	cdata->ieee[1] = 0xd2; /* [한국어] IEEE OUI 바이트1 */
	cdata->ieee[2] = 0x5c; /* [한국어] IEEE OUI 바이트2 */

	/* When adding support for a new ONCS feature, ensure it follows the pattern below. */
	/* [한국어] ONCS(Optional NVM Command Support) 비트들을 transport 설정에서 복사.
	 * transport->opts.oncs는 RPC/설정으로 제어되며 개별 비트가 각 선택적 명령 지원을 나타냄 */
	cdata->oncs.nvmcmps = transport->opts.oncs.nvmcmps; /* [한국어] Compare 명령 지원 */
	cdata->oncs.nvmdsmsv = transport->opts.oncs.nvmdsmsv; /* [한국어] Dataset Management 명령 지원 */
	cdata->oncs.nvmwzsv = transport->opts.oncs.nvmwzsv; /* [한국어] Write Zeroes 명령 지원 */
	cdata->oncs.reservs = transport->opts.oncs.reservs; /* [한국어] Reservations 명령 지원 */
	cdata->oncs.nvmcpys = transport->opts.oncs.nvmcpys; /* [한국어] Copy 명령 지원 */

	/* When adding support for a new FUSES feature, ensure it follows the pattern below. */
	cdata->fuses.fcws = transport->opts.fuses.fcws; /* [한국어] Fused Compare and Write 지원 */

	/* [한국어] SGL(Scatter-Gather List) 지원 선언 - NVMe-oF는 SGL 필수 */
	cdata->sgls.supported = 1; /* [한국어] SGL supported in NVM Command Set */
	cdata->sgls.keyed_sgl = 1; /* [한국어] Keyed SGL Data Block Descriptor 지원 (RDMA 필수) */
	cdata->sgls.sgl_offset = 1; /* [한국어] SGL Offset 사용 지원 */
	cdata->cntrltype = spdk_nvmf_subsystem_is_discovery(subsystem) ?
			   SPDK_NVME_CTRLR_DISCOVERY : SPDK_NVME_CTRLR_IO; /* [한국어] Discovery=2h, I/O=1h */

	/* [한국어] NVMe-oF 특화 필드: ioccsz = I/O Command Capsule Size (16바이트 단위)
	 * = SQE(64B) + in-capsule data (in_capsule_data_size). 호스트는 이 값만큼 캡슐을 만듦. */
	cdata->nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	cdata->nvmf_specific.ioccsz += transport->opts.in_capsule_data_size / 16;
	cdata->nvmf_specific.iorcsz = sizeof(struct spdk_nvme_cpl) / 16; /* [한국어] I/O Response Capsule Size (16B 단위) = CQE 4×16B = 4 */
	cdata->nvmf_specific.icdoff = 0; /* offset starts directly after SQE */ /* [한국어] In-capsule Data Offset = 0: SQE 바로 뒤에 데이터 */
	cdata->nvmf_specific.ctrattr.ctrlr_model = SPDK_NVMF_CTRLR_MODEL_DYNAMIC; /* [한국어] Dynamic 컨트롤러 모델 (cntlid를 target이 할당) */
	cdata->nvmf_specific.msdbd = 1; /* [한국어] Maximum SGL Data Block Descriptors = 1 (단일 SGL 블록) */

	if (transport->ops->cdata_init) {
		/* [한국어] transport 종류별 추가 초기화 (예: RDMA이면 IBMR, TCP이면 PDU 관련 필드) */
		transport->ops->cdata_init(transport, subsystem, cdata);
	}
}

/*
 * [한국어]
 * nvmf_subsystem_has_zns_iocs - subsystem에 ZNS(Zoned Namespace) 네임스페이스가 있는지 확인
 *
 * @subsystem: 검사할 NVM subsystem.
 * @return: true이면 ZNS bdev를 보유한 NS가 1개 이상 존재, false이면 전부 비ZNS.
 *
 * cdata->iocs 필드(I/O Command Set 지원 비트맵) 초기화 시 ZNS Command Set
 * (CSS=0x2) 지원 여부를 결정하기 위해 nvmf_ctrlr_create()에서 호출된다.
 * NS가 하나라도 ZNS bdev를 가리키면 해당 subsystem은 ZNS를 광고한다.
 *
 * 실행 컨텍스트: qpair->group->thread (nvmf_ctrlr_create 내부).
 *
 * 호출 체인:
 *   nvmf_ctrlr_create → 본 함수 → spdk_bdev_is_zoned
 */
static bool
nvmf_subsystem_has_zns_iocs(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_ns *ns; /* [한국어] 순회 중인 네임스페이스 포인터 */
	uint32_t i; /* [한국어] nsid 0-based 인덱스 */

	for (i = 0; i < subsystem->max_nsid; i++) { /* [한국어] 가능한 모든 nsid 슬롯 순회 */
		ns = subsystem->ns[i]; /* [한국어] 이 슬롯의 NS 포인터 (NULL이면 미사용) */
		if (ns && ns->bdev && spdk_bdev_is_zoned(ns->bdev)) {
			/* [한국어] NS가 존재하고 그 bdev가 Zoned Device이면 즉시 true 반환 */
			return true;
		}
	}
	return false; /* [한국어] ZNS NS 없음 */
}

/*
 * [한국어]
 * nvmf_ctrlr_init_visible_ns - 컨트롤러에게 보이는 네임스페이스 비트맵을 초기화
 *
 * @ctrlr: 초기화할 컨트롤러 (이미 hostnqn 필드 설정 완료).
 *
 * 각 NS를 순회하여 ns->always_visible=true이거나 hostnqn이 ns의 host 접근 목록에
 * 있는 경우 visible_ns 비트맵의 해당 비트를 set한다.
 * Identify Namespace List, ANA Log Page 등 NS 목록 반환 시 이 비트맵을 기준으로 필터링한다.
 *
 * 실행 컨텍스트: qpair->group->thread (nvmf_ctrlr_create 내부).
 *
 * 호출 체인:
 *   nvmf_ctrlr_create → 본 함수 → spdk_nvmf_subsystem_get_first_ns/get_next_ns,
 *                                  nvmf_ns_find_host, nvmf_ctrlr_ns_set_visible
 */
static void
nvmf_ctrlr_init_visible_ns(struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys; /* [한국어] 이 컨트롤러가 속한 subsystem */
	struct spdk_nvmf_ns *ns; /* [한국어] 순회 중인 네임스페이스 포인터 */

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		/* [한국어] always_visible이거나 hostnqn이 NS의 host ACL에 등록된 경우 비트 set */
		if (ns->always_visible || nvmf_ns_find_host(ns, ctrlr->hostnqn) != NULL) {
			nvmf_ctrlr_ns_set_visible(ctrlr, ns->nsid, true); /* [한국어] visible_ns 비트맵에서 nsid 비트를 set */
		}
	}
}

/*
 * [한국어]
 * nvmf_ctrlr_create - 호스트의 admin Connect(qid=0)에 대해 새 컨트롤러를 만든다
 *
 * @subsystem: 호스트가 접속하려는 NVM subsystem (subnqn으로 룩업됨).
 * @req: Connect 요청 객체 (응답 채우기에 사용).
 * @connect_cmd: Connect SQE (KATO, qid, sqsize 등).
 * @connect_data: Connect 데이터 캡슐 (hostnqn, hostid, cntlid 등 NQN/UUID).
 * @return: 신규 spdk_nvmf_ctrlr* (성공) 또는 NULL (할당/리스너 검색 실패).
 *
 * 흐름 요약:
 *  1) calloc으로 ctrlr 객체 할당.
 *  2) Fabrics(rdma/tcp/fc)이면 dynamic ctrlr (cntlid는 후속 add_ctrlr에서 할당).
 *  3) qpair_mask, visible_ns 비트맵 생성. nvmf_ctrlr_cdata_init()으로
 *     Identify Controller 데이터(cdata) 기본값 채움.
 *  4) KATO 정규화 (transport->opts.min_kato 보정 + KAS unit 라운드업).
 *  5) Discovery subsystem이면 KATO=0일 때 NVMF_DISC_KATO_IN_MS 기본값을 부여.
 *  6) vcprop(가상 NVMe 레지스터) 초기화: CAP/VS/CC/CSTS/CRTO. NVMe 2.0 호환을 위해
 *     vs.mjr=2, cap.crwms=1, cap.cqr=1 (NVMe-oF 필수) 등을 set.
 *  7) NVM subsystem이면 listener 정보를 찾아 둠 (ANA reporting을 위해).
 *  8) qpair에 ctrlr 결합 후 subsystem->thread로 _nvmf_subsystem_add_ctrlr 메시지 송신.
 *
 * 호출 컨텍스트: qpair->group->thread (Connect 명령을 처음 받은 곳).
 * 이후 라이프사이클은 ctrlr->thread = qpair->group->thread로 고정된다.
 */
static struct spdk_nvmf_ctrlr *
nvmf_ctrlr_create(struct spdk_nvmf_subsystem *subsystem,
		  struct spdk_nvmf_request *req,
		  struct spdk_nvmf_fabric_connect_cmd *connect_cmd,
		  struct spdk_nvmf_fabric_connect_data *connect_data)
{
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_transport *transport = req->qpair->transport;
	struct spdk_nvme_transport_id listen_trid = {};
	bool subsys_has_multi_iocs = false;

	ctrlr = calloc(1, sizeof(*ctrlr)); /* [한국어] zero-init된 ctrlr 객체 할당 */
	if (ctrlr == NULL) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}

	if (spdk_nvme_trtype_is_fabrics(transport->ops->type)) {
		/* [한국어] RDMA/TCP/FC 등 Fabrics transport는 dynamic ctrlr 모드만 지원.
		 * cntlid는 _nvmf_subsystem_add_ctrlr()에서 할당된다 (호스트가 0xFFFF 보냈음). */
		ctrlr->dynamic_ctrlr = true;
	} else {
		/* [한국어] Local(PCIe-style) transport는 호스트가 명시한 cntlid 사용 (vfio-user 등). */
		ctrlr->cntlid = connect_data->cntlid;
	}

	SPDK_DTRACE_PROBE3_TICKS(nvmf_ctrlr_create, ctrlr, subsystem->subnqn,
				 spdk_thread_get_id(req->qpair->group->thread)); /* [한국어] DTrace 프로브 - 성능분석용 */

	STAILQ_INIT(&ctrlr->async_events); /* [한국어] AER 통지 큐 초기화 (호스트가 AER 안 걸어둔 동안 누적) */
	TAILQ_INIT(&ctrlr->log_head); /* [한국어] Reservation Notification 로그 페이지 큐 초기화 */
	ctrlr->subsys = subsystem; /* [한국어] 모든 ctrlr는 정확히 하나의 subsystem에 속함 */
	ctrlr->thread = req->qpair->group->thread; /* [한국어] thread affinity 고정 - 이후 모든 라이프사이클은 이 thread에서만 */
	ctrlr->disconnect_in_progress = false;
	ctrlr->executing_nssr = false; /* [한국어] NSSR(NVM Subsystem Reset) 진행 중 표지 */

	/* [한국어] qpair_mask는 transport 설정의 max_qpairs_per_ctrlr 길이 비트맵.
	 * QID 1..max-1의 점유 여부를 1비트씩 표시하여 IO Connect 중복을 검출한다. */
	ctrlr->qpair_mask = spdk_bit_array_create(transport->opts.max_qpairs_per_ctrlr);
	if (!ctrlr->qpair_mask) {
		SPDK_ERRLOG("Failed to allocate controller qpair mask\n");
		goto err_qpair_mask;
	}

	/* [한국어] Identify Controller 응답으로 호스트에 보낼 cdata를 채운다.
	 * (kas, vendor id, sgls, ioccsz/iorcsz/icdoff/msdbd 등 NVMe-oF specific 포함) */
	nvmf_ctrlr_cdata_init(transport, subsystem, &ctrlr->cdata);

	/*
	 * KAS: This field indicates the granularity of the Keep Alive Timer in 100ms units.
	 * If this field is cleared to 0h, then Keep Alive is not supported.
	 */
	if (ctrlr->cdata.kas) {
		/* [한국어] KAS(Keep Alive Support) = 0이면 KA 미지원. 지원 시 KATO 정규화:
		 *   0: KA 비활성 요청 → 그대로 0
		 *   <= min_kato: 최소값 보정 (너무 짧은 KATO 방지)
		 *   else: KAS 단위(100ms)로 올림 (스펙: KATO는 KAS 배수여야 함) */
		if (connect_cmd->kato == 0) {
			ctrlr->feat.keep_alive_timer.bits.kato = 0; /* [한국어] KA 비활성 */
		} else if (connect_cmd->kato <= transport->opts.min_kato) {
			ctrlr->feat.keep_alive_timer.bits.kato = transport->opts.min_kato; /* [한국어] 최소 허용 KATO로 상향 보정 */
		} else {
			/* [한국어] KAS*NVMF_KAS_TIME_UNIT_IN_MS(100ms)의 배수로 올림 정규화 */
			ctrlr->feat.keep_alive_timer.bits.kato = spdk_round_up(connect_cmd->kato,
					ctrlr->cdata.kas * NVMF_KAS_TIME_UNIT_IN_MS);
		}
	}

	ctrlr->feat.async_event_configuration.bits.ns_attr_notice = 1; /* [한국어] NS Attribute Change 통지 기본 활성 */
	if (ctrlr->subsys->flags.ana_reporting) {
		/* [한국어] subsystem이 ANA 보고를 지원하면 ANA Change 통지도 활성 */
		ctrlr->feat.async_event_configuration.bits.ana_change_notice = 1;
	}
	ctrlr->feat.volatile_write_cache.bits.wce = 1; /* [한국어] WCE(Write Cache Enable) 기본값 1 (Volatile Write Cache 활성) */
	/* Coalescing Disable */
	/* [한국어] 인터럽트 코어링 비활성(CD=1): NVMe-oF는 인터럽트 대신 폴링 기반이므로 코어링 불필요 */
	ctrlr->feat.interrupt_vector_configuration.bits.cd = 1;

	if (spdk_nvmf_subsystem_is_discovery(ctrlr->subsys)) {
		/*
		 * If keep-alive timeout is not set, discovery controllers use some
		 * arbitrary high value in order to cleanup stale discovery sessions
		 *
		 * From the 1.0a nvme-of spec:
		 * "The Keep Alive command is reserved for
		 * Discovery controllers. A transport may specify a
		 * fixed Discovery controller activity timeout value
		 * (e.g., 2 minutes). If no commands are received
		 * by a Discovery controller within that time
		 * period, the controller may perform the
		 * actions for Keep Alive Timer expiration".
		 *
		 * From the 1.1 nvme-of spec:
		 * "A host requests an explicit persistent connection
		 * to a Discovery controller and Asynchronous Event Notifications from
		 * the Discovery controller on that persistent connection by specifying
		 * a non-zero Keep Alive Timer value in the Connect command."
		 *
		 * In case non-zero KATO is used, we enable discovery_log_change_notice
		 * otherwise we disable it and use default discovery controller KATO.
		 * KATO is in millisecond.
		 */
		/* [한국어] Discovery 컨트롤러: KATO=0이면 오래된 세션 정리를 위해 기본값 설정.
		 * KATO≠0이면 호스트가 AEN을 원하는 영구 연결 → discovery_log_change_notice 활성 */
		if (ctrlr->feat.keep_alive_timer.bits.kato == 0) {
			ctrlr->feat.keep_alive_timer.bits.kato = NVMF_DISC_KATO_IN_MS; /* [한국어] Discovery 기본 KATO (2분 = 120000ms) */
			ctrlr->feat.async_event_configuration.bits.discovery_log_change_notice = 0; /* [한국어] 단순 연결은 discovery AEN 비활성 */
		} else {
			ctrlr->feat.async_event_configuration.bits.discovery_log_change_notice = 1; /* [한국어] 영구 연결: discovery log 변경 통지 활성 */
		}
	}

	/* Subtract 1 for admin queue, 1 for 0's based */
	/* [한국어] Number of Queues 기본값: max_qpairs_per_ctrlr에서 admin용 1개와 0-based 보정 1개를 빼 IO 큐 최대 수 계산 */
	ctrlr->feat.number_of_queues.bits.ncqr = transport->opts.max_qpairs_per_ctrlr - 1 -
			1; /* [한국어] NCQR: IO CQ 개수 최대값 (0-based). 호스트가 Set Features로 줄일 수 있음 */
	ctrlr->feat.number_of_queues.bits.nsqr = transport->opts.max_qpairs_per_ctrlr - 1 -
			1; /* [한국어] NSQR: IO SQ 개수 최대값 (0-based). NCQR와 동일 설정 */

	spdk_uuid_copy(&ctrlr->hostid, (struct spdk_uuid *)connect_data->hostid); /* [한국어] 호스트 UUID 복사 (Reservation 등록에서 호스트 식별에 사용) */
	memcpy(ctrlr->hostnqn, connect_data->hostnqn, SPDK_NVMF_NQN_MAX_LEN); /* [한국어] 호스트 NQN 복사 (로그/ANA/host ACL 비교에서 사용) */

	ctrlr->visible_ns = spdk_bit_array_create(subsystem->max_nsid);
	if (!ctrlr->visible_ns) {
		SPDK_ERRLOG("Failed to allocate visible namespace array\n");
		goto err_visible_ns;
	}
	nvmf_ctrlr_init_visible_ns(ctrlr);

	/* [한국어] CAP(Controller Capabilities) 가상 레지스터 초기화.
	 * PCIe NVMe라면 BAR0 MMIO에 있지만 NVMe-oF는 메모리 내 vcprop.cap에 저장하고
	 * Property Get Capsule에 응답할 때 반환한다 (NVMe-oF 1.1 §4.6). */
	ctrlr->vcprop.cap.raw = 0; /* [한국어] 초기화: 모든 비트 클리어 후 필드별 설정 */
	ctrlr->vcprop.cap.bits.cqr = 1; /* NVMe-oF specification required */ /* [한국어] CQR=1: Contiguous Queues Required. NVMe-oF 스펙 필수 */
	ctrlr->vcprop.cap.bits.mqes = transport->opts.max_queue_depth -
				      1; /* max queue depth */ /* [한국어] MQES: Maximum Queue Entries Supported (0-based). max_queue_depth-1 */
	ctrlr->vcprop.cap.bits.ams = 0; /* optional arb mechanisms */ /* [한국어] AMS=0: RR 중재만 지원 (weight-based, vendor-specific 미지원) */
	/* ready timeout - 500 msec units */
	/* [한국어] TO: Timeout (500ms 단위). 컨트롤러가 CC.EN=1 후 CSTS.RDY=1이 되는 최대 시간 */
	ctrlr->vcprop.cap.bits.to = NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS / 500;
	ctrlr->vcprop.cap.bits.dstrd = 0; /* fixed to 0 for NVMe-oF */ /* [한국어] DSTRD=0: Doorbell Stride. NVMe-oF는 doorbell 없으므로 항상 0 */
	subsys_has_multi_iocs = nvmf_subsystem_has_zns_iocs(subsystem); /* [한국어] subsystem에 ZNS NS가 있으면 multi-IOCS 지원 광고 */
	if (subsys_has_multi_iocs) {
		/* [한국어] CSS: Command Set Supported. ZNS NS 존재 → IOCS 비트 설정 (NVM + ZNS) */
		ctrlr->vcprop.cap.bits.css =
			SPDK_NVME_CAP_CSS_IOCS; /* One or more I/O command sets supported */
	} else {
		/* [한국어] ZNS NS 없음 → NVM Command Set만 지원 */
		ctrlr->vcprop.cap.bits.css = SPDK_NVME_CAP_CSS_NVM; /* NVM command set */
	}

	ctrlr->vcprop.cap.bits.mpsmin = 0; /* 2 ^ (12 + mpsmin) == 4k */ /* [한국어] MPSMIN=0: 최소 메모리 페이지 크기 = 2^(12+0) = 4KB */
	ctrlr->vcprop.cap.bits.mpsmax = 0; /* 2 ^ (12 + mpsmax) == 4k */ /* [한국어] MPSMAX=0: 최대 메모리 페이지 크기 = 4KB (NVMe-oF는 고정) */

	if (subsystem->nssr_enabled == 1) {
		/* [한국어] NSSRS=1: NVM Subsystem Reset Supported. 0x4E564D65 magic 쓰기로 reset 가능 */
		ctrlr->vcprop.cap.bits.nssrs = 1;
	}

	/* NVMe 2.0 specification required */
	/* [한국어] CRWMS=1: Controller Ready With Media Support (NVMe 2.0). CRTO 레지스터 사용을 알림 */
	ctrlr->vcprop.cap.bits.crwms = 1;

	/* Version Supported: 2.0 */
	/* [한국어] VS(Version) 레지스터: 이 컨트롤러가 구현하는 NVMe 스펙 버전 = 2.0.0 */
	ctrlr->vcprop.vs.bits.mjr = 2; /* [한국어] Major version = 2 */
	ctrlr->vcprop.vs.bits.mnr = 0; /* [한국어] Minor version = 0 */
	ctrlr->vcprop.vs.bits.ter = 0; /* [한국어] Tertiary version = 0 */

	/* [한국어] CC(Controller Configuration) 레지스터 초기값: EN=0, CSS는 multi-iocs이면 IOCS */
	ctrlr->vcprop.cc.raw = 0; /* [한국어] 모든 비트 초기화 */
	ctrlr->vcprop.cc.bits.en = 0; /* Init controller disabled */ /* [한국어] EN=0: 컨트롤러 비활성. 호스트가 Property Set으로 1로 설정해야 사용 가능 */
	if (subsys_has_multi_iocs) {
		/* [한국어] CC.CSS: Command Set Selected. multi-iocs 광고 시 CSS=111b (All Supported IOCS)로 초기화 */
		ctrlr->vcprop.cc.bits.css =
			SPDK_NVME_CC_CSS_IOCS; /* All supported I/O Command Sets */
	}

	/* [한국어] CSTS(Controller Status) 레지스터 초기값: RDY=0 (아직 준비 안 됨) */
	ctrlr->vcprop.csts.raw = 0; /* [한국어] 모든 비트 초기화 */
	ctrlr->vcprop.csts.bits.rdy = 0; /* Init controller as not ready */ /* [한국어] RDY=0: 호스트가 EN=1 후 폴링하는 값. EN 처리 후 RDY=1로 변경됨 */

	/* set controller ready with media timeout to CAP.TO value */
	/* [한국어] CRTO(Controller Ready Timeouts): CRWMT=CAP.TO. "with media" 준비 최대 시간 (500ms 단위) */
	ctrlr->vcprop.crto.bits.crwmt = ctrlr->vcprop.cap.bits.to;

	SPDK_DEBUGLOG(nvmf, "cap 0x%" PRIx64 "\n", ctrlr->vcprop.cap.raw);
	SPDK_DEBUGLOG(nvmf, "vs 0x%x\n", ctrlr->vcprop.vs.raw);
	SPDK_DEBUGLOG(nvmf, "cc 0x%x\n", ctrlr->vcprop.cc.raw);
	SPDK_DEBUGLOG(nvmf, "csts 0x%x\n", ctrlr->vcprop.csts.raw);

	ctrlr->dif_insert_or_strip = transport->opts.dif_insert_or_strip; /* [한국어] DIF 삽입/제거 여부: transport 설정에서 상속 (T10 Protection Info) */

	if (ctrlr->subsys->subtype == SPDK_NVMF_SUBTYPE_NVME) {
		/* [한국어] NVM subsystem(Discovery 아님)일 때만 listener 정보를 저장.
		 * ANA 상태 보고(nvmf_ctrlr_get_ana_state)가 listener->ana_state[]를 참조하기 때문 */
		if (spdk_nvmf_qpair_get_listen_trid(req->qpair, &listen_trid) != 0) {
			/* [한국어] qpair가 연결된 listener의 transport ID 조회 실패 → 에러 */
			SPDK_ERRLOG("Could not get listener transport ID\n");
			goto err_listener;
		}

		ctrlr->listener = nvmf_subsystem_find_listener(ctrlr->subsys, &listen_trid); /* [한국어] trid로 subsystem의 listener 목록 검색 */
		if (!ctrlr->listener) {
			/* [한국어] 해당 trid의 listener 없음 → Connect 거절 (listener ACL이 변경됐을 수도 있음) */
			SPDK_ERRLOG("Listener was not found\n");
			goto err_listener;
		}
	}

	nvmf_qpair_set_ctrlr(req->qpair, ctrlr);
	spdk_thread_send_msg(subsystem->thread, _nvmf_subsystem_add_ctrlr, req);

	return ctrlr;
err_listener:
	spdk_bit_array_free(&ctrlr->visible_ns);
err_visible_ns:
	spdk_bit_array_free(&ctrlr->qpair_mask);
err_qpair_mask:
	free(ctrlr);
	return NULL;
}

/*
 * [한국어]
 * _nvmf_ctrlr_destruct - 컨트롤러 객체의 실제 free (ctrlr->thread에서 실행)
 *
 * @ctx: 해제할 spdk_nvmf_ctrlr*.
 *
 * subsystem 등록 해제와 disconnect 처리가 모두 끝난 후 호출되는 마지막 단계.
 * - keep_alive/association 폴러 정리
 * - qpair_mask, visible_ns 비트맵 free
 * - log_head(reservation log) 큐 비우기
 * - async_events 큐 비우기 (호스트가 가져가지 못한 AER 통지들)
 * - ctrlr 자체 free
 *
 * 만약 disconnect_in_progress가 아직 true이면 자기 자신을 다시 메시지로 큐잉해
 * 다음 폴 사이클에 재시도한다 (race 방지).
 *
 * 호출 컨텍스트: ctrlr->thread (assert로 강제). nvmf_ctrlr_destruct()의
 * spdk_thread_send_msg에 의해 진입됨.
 */
static void
_nvmf_ctrlr_destruct(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx;
	struct spdk_nvmf_reservation_log *log, *log_tmp;
	struct spdk_nvmf_async_event_completion *event, *event_tmp;

	SPDK_DTRACE_PROBE3_TICKS(nvmf_ctrlr_destruct, ctrlr, ctrlr->subsys->subnqn,
				 spdk_thread_get_id(ctrlr->thread));

	assert(spdk_get_thread() == ctrlr->thread);
	assert(ctrlr->in_destruct);

	SPDK_DEBUGLOG(nvmf, "Destroy ctrlr 0x%hx\n", ctrlr->cntlid);
	if (ctrlr->disconnect_in_progress) {
		SPDK_ERRLOG("freeing ctrlr with disconnect in progress\n");
		spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_destruct, ctrlr);
		return;
	}

	nvmf_ctrlr_stop_keep_alive_timer(ctrlr);
	nvmf_ctrlr_stop_association_timer(ctrlr);
	spdk_bit_array_free(&ctrlr->qpair_mask);

	TAILQ_FOREACH_SAFE(log, &ctrlr->log_head, link, log_tmp) {
		TAILQ_REMOVE(&ctrlr->log_head, log, link);
		free(log);
	}
	STAILQ_FOREACH_SAFE(event, &ctrlr->async_events, link, event_tmp) {
		STAILQ_REMOVE(&ctrlr->async_events, event, spdk_nvmf_async_event_completion, link);
		free(event);
	}
	spdk_bit_array_free(&ctrlr->visible_ns);
	free(ctrlr);
}

/*
 * [한국어]
 * nvmf_ctrlr_destruct - 컨트롤러 소멸 진입점 (다른 스레드에서 호출 가능)
 *
 * @ctrlr: 소멸시킬 컨트롤러.
 *
 * subsystem의 ctrlrs 리스트에서 ctrlr를 즉시 제거하고 (subsystem->thread에서
 * 안전하게 처리되도록 nvmf_subsystem_remove_ctrlr가 처리), 실제 해제는
 * ctrlr->thread로 메시지를 보내 _nvmf_ctrlr_destruct()에서 수행한다. 이로써
 * cross-thread 호출 안전성이 보장된다.
 *
 * 호출자: subsystem 정지/삭제 RPC, qpair disconnect 마지막 단계 등.
 */
void
nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
	nvmf_subsystem_remove_ctrlr(ctrlr->subsys, ctrlr); /* [한국어] subsystem->ctrlrs 리스트에서 제거 */

	spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_destruct, ctrlr); /* [한국어] 실제 free는 ctrlr 소유 스레드에서 */
}

/*
 * [한국어]
 * nvmf_ctrlr_add_io_qpair - IO qpair를 컨트롤러에 등록 (ctrlr->thread에서 실행)
 *
 * @ctx: spdk_nvmf_request* (IO Connect 요청).
 *
 * _nvmf_ctrlr_add_io_qpair()가 subsystem->thread에서 선행 검증을 마친 뒤
 * admin_qpair_group->thread(=ctrlr->thread)로 메시지를 보내 본 함수를 실행한다.
 * ctrlr->thread에서 실행되므로 ctrlr 상태를 안전하게 읽고 수정할 수 있다.
 *
 * 검증 단계:
 *  1) ctrlr->in_destruct: 소멸 중이면 거절.
 *  2) Discovery subsystem에는 IO qpair 불가.
 *  3) CC.EN=0이면 아직 초기화 안 됨 → 거절.
 *  4) CC.IOSQES, CC.IOCQES가 스펙 크기(64B/16B)와 맞는지 검증.
 *  5) admin_qpair 활성 상태 확인 (DEACTIVATING이거나 group==NULL이면 거절).
 *  6) QID가 qpair_mask 용량 초과이면 거절 (INVALID_QUEUE_IDENTIFIER).
 *  7) 통과하면 nvmf_ctrlr_add_qpair()를 호출해 qpair_mask에 비트 set 후 Connect RSP 송신.
 *
 * 실행 컨텍스트: ctrlr->thread (admin_qpair_group->thread와 동일).
 *
 * 호출 체인:
 *   _nvmf_ctrlr_add_io_qpair [subsystem->thread]
 *     → spdk_thread_send_msg(admin_qpair_group->thread, nvmf_ctrlr_add_io_qpair)
 *     → [본 함수] → nvmf_ctrlr_add_qpair → nvmf_ctrlr_send_connect_rsp
 */
static void
nvmf_ctrlr_add_io_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx; /* [한국어] IO Connect 요청 (qpair, rsp 포함) */
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp; /* [한국어] Connect 응답 버퍼 */
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] 등록할 IO qpair */
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr; /* [한국어] 대상 컨트롤러 (선행 함수가 설정) */
	struct spdk_nvmf_qpair *admin_qpair = ctrlr->admin_qpair; /* [한국어] 동일 ctrlr의 admin qpair */
	struct spdk_nvmf_poll_group *admin_qpair_group = NULL; /* [한국어] admin qpair가 속한 poll group */
	enum spdk_nvmf_qpair_state admin_qpair_state = SPDK_NVMF_QPAIR_UNINITIALIZED; /* [한국어] admin qpair 현재 상태 (로그용) */
	bool admin_qpair_active = false; /* [한국어] admin qpair 활성 여부 플래그 */

	SPDK_DTRACE_PROBE4_TICKS(nvmf_ctrlr_add_io_qpair, ctrlr, req->qpair, req->qpair->qid,
				 spdk_thread_get_id(ctrlr->thread));

	/* For error case, the value should be NULL. So set it to NULL at first. */
	/* [한국어] 오류 경로에서 qpair->ctrlr이 잘못된 포인터를 유지하지 않도록 먼저 NULL로 초기화 */
	qpair->ctrlr = NULL;

	/* Make sure the controller is not being destroyed. */
	if (ctrlr->in_destruct) {
		/* [한국어] 컨트롤러가 이미 소멸 중 → IO Connect 거절 */
		SPDK_ERRLOG("Got I/O connect while ctrlr was being destroyed.\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid); /* [한국어] SC=INVALID_QID로 rsp 설정 */
		goto end;
	}

	if (spdk_nvmf_subsystem_is_discovery(ctrlr->subsys)) {
		/* [한국어] Discovery 컨트롤러는 admin qpair만 허용; IO qpair 금지 */
		SPDK_ERRLOG("I/O connect not allowed on discovery controller\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	if (!ctrlr->vcprop.cc.bits.en) {
		/* [한국어] CC.EN=0: 아직 Property Set으로 컨트롤러 활성화가 안 됨 → 거절 */
		SPDK_ERRLOG("Got I/O connect before ctrlr was enabled\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	if (1u << ctrlr->vcprop.cc.bits.iosqes != sizeof(struct spdk_nvme_cmd)) {
		/* [한국어] CC.IOSQES: SQ Entry Size 지수(log2). 2^IOSQES가 64B(NVMe SQE)이어야 함 */
		SPDK_ERRLOG("Got I/O connect with invalid IOSQES %u\n",
			    ctrlr->vcprop.cc.bits.iosqes);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	if (1u << ctrlr->vcprop.cc.bits.iocqes != sizeof(struct spdk_nvme_cpl)) {
		/* [한국어] CC.IOCQES: CQ Entry Size 지수. 2^IOCQES가 16B(NVMe CQE)이어야 함 */
		SPDK_ERRLOG("Got I/O connect with invalid IOCQES %u\n",
			    ctrlr->vcprop.cc.bits.iocqes);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	/* There is a chance that admin qpair was destroyed. This is an issue that was observed only with ESX initiators */
	if (admin_qpair) {
		/* [한국어] admin_qpair가 아직 살아있으면 활성 상태와 group 포인터를 읽음 */
		admin_qpair_active = spdk_nvmf_qpair_is_active(admin_qpair); /* [한국어] ACTIVE 상태인지 확인 */
		admin_qpair_group = admin_qpair->group; /* [한국어] admin qpair가 속한 poll group */
		admin_qpair_state = admin_qpair->state; /* [한국어] 오류 로그 출력용 현재 상태값 */
	}

	if (!admin_qpair_active || admin_qpair_group == NULL) {
		/* There is a chance that admin qpair was destroyed or is being destroyed at this moment due to e.g.
		 * expired keep alive timer. Part of the qpair destruction process is change of qpair's
		 * state to DEACTIVATING and removing it from poll group */
		/* [한국어] KATO 만료 등으로 admin qpair가 DEACTIVATING이거나 이미 group에서 제거된 경우.
		 * 이 상태에서 IO qpair를 추가하면 ctrlr 상태가 일관성을 잃으므로 거절 */
		SPDK_ERRLOG("Inactive admin qpair (state %d, group %p)\n", admin_qpair_state, admin_qpair_group);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	/* check if we would exceed ctrlr connection limit */
	if (qpair->qid >= spdk_bit_array_capacity(ctrlr->qpair_mask)) {
		/* [한국어] QID가 qpair_mask 비트맵 용량 이상이면 최대 큐 수 초과 */
		SPDK_ERRLOG("Requested QID %u but Max QID is %u\n",
			    qpair->qid, spdk_bit_array_capacity(ctrlr->qpair_mask) - 1);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC; /* [한국어] Command Specific 오류 타입 */
		rsp->status.sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER; /* [한국어] QID 범위 초과 SC */
		goto end;
	}

	nvmf_ctrlr_add_qpair(qpair, ctrlr, req); /* [한국어] qpair_mask 비트 set + 중복 QID 처리 + Connect RSP 전송 */
	return;
end:
	spdk_nvmf_request_complete(req); /* [한국어] 오류 응답 전송 후 요청 완료 처리 */
}

/*
 * [한국어]
 * _nvmf_ctrlr_add_io_qpair - IO qpair Connect의 1단계 검증 (subsystem->thread에서 실행)
 *
 * @ctx: spdk_nvmf_request* (IO Connect 요청).
 *
 * IO Connect(qid>0)는 2단계로 처리된다:
 *  1) 본 함수: subsystem->thread에서 subsystem/ctrlr 레벨 검증
 *     (cntlid 유효성, in_destruct, ANA listener 일치 여부, admin qpair 상태).
 *  2) nvmf_ctrlr_add_io_qpair: ctrlr->thread에서 CC/IOSQES/IOCQES/QID 범위 재검증 + 등록.
 *
 * subsystem->thread에서 실행되므로 subsystem->ctrlrs 리스트 접근이 안전하다.
 * 검증을 통과하면 qpair->ctrlr을 설정하고 admin_qpair_group->thread로 메시지를 전달한다.
 *
 * 실행 컨텍스트: subsystem->thread (nvmf_ctrlr_cmd_connect에서 send_msg).
 *
 * 호출 체인:
 *   nvmf_ctrlr_cmd_connect [poll group thread]
 *     → spdk_thread_send_msg(subsystem->thread, _nvmf_ctrlr_add_io_qpair)
 *     → [본 함수] → spdk_thread_send_msg(admin_qpair_group->thread, nvmf_ctrlr_add_io_qpair)
 */
static void
_nvmf_ctrlr_add_io_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx; /* [한국어] IO Connect 요청 객체 */
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp; /* [한국어] Connect 응답 버퍼 */
	struct spdk_nvmf_fabric_connect_data *data; /* [한국어] Connect 데이터 캡슐 (cntlid, subnqn 등) */
	struct spdk_nvmf_ctrlr *ctrlr; /* [한국어] cntlid로 찾은 기존 컨트롤러 */
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] 추가할 IO qpair */
	struct spdk_nvmf_qpair *admin_qpair; /* [한국어] ctrlr의 admin qpair */
	struct spdk_nvmf_tgt *tgt = qpair->transport->tgt; /* [한국어] 전역 target (subsystem 룩업용) */
	struct spdk_nvmf_subsystem *subsystem; /* [한국어] subnqn으로 찾은 subsystem */
	struct spdk_nvme_transport_id listen_trid = {}; /* [한국어] ANA 검증용 리스너 transport ID */
	const struct spdk_nvmf_subsystem_listener *listener; /* [한국어] IO qpair의 리스너 */
	struct spdk_nvmf_poll_group *admin_qpair_group = NULL; /* [한국어] admin qpair의 poll group */
	enum spdk_nvmf_qpair_state admin_qpair_state = SPDK_NVMF_QPAIR_UNINITIALIZED; /* [한국어] 로그용 admin 상태 */
	bool admin_qpair_active = false; /* [한국어] admin qpair 활성 여부 */

	assert(req->iovcnt == 1); /* [한국어] Connect 데이터 캡슐은 단일 iov에 들어 있어야 함 */

	data = req->iov[0].iov_base; /* [한국어] Connect 데이터 캡슐 포인터 추출 */

	SPDK_DEBUGLOG(nvmf, "Connect I/O Queue for controller id 0x%x\n", data->cntlid);

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, data->subnqn); /* [한국어] subnqn → subsystem 룩업 */
	/* We already checked this in _nvmf_ctrlr_connect */
	assert(subsystem != NULL); /* [한국어] _nvmf_ctrlr_connect에서 이미 검증됨 */

	ctrlr = nvmf_subsystem_get_ctrlr(subsystem, data->cntlid); /* [한국어] cntlid → ctrlr 룩업 */
	if (ctrlr == NULL) {
		/* [한국어] 해당 cntlid를 가진 컨트롤러가 subsystem에 없음 */
		SPDK_ERRLOG("Unknown controller ID 0x%x\n", data->cntlid);
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, cntlid); /* [한국어] cntlid 필드에 Invalid 오류 표시 */
		spdk_nvmf_request_complete(req); /* [한국어] 오류 응답 전송 */
		return;
	}

	/* fail before passing a message to the controller thread. */
	if (ctrlr->in_destruct) {
		/* [한국어] ctrlr이 소멸 중이면 더 이상 IO qpair를 추가할 수 없음 */
		SPDK_ERRLOG("Got I/O connect while ctrlr was being destroyed.\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		spdk_nvmf_request_complete(req);
		return;
	}

	/* If ANA reporting is enabled, check if I/O connect is on the same listener. */
	if (subsystem->flags.ana_reporting) {
		/* [한국어] ANA(Asymmetric Namespace Access) 리포팅이 활성화된 경우
		 * IO qpair의 리스너가 admin qpair와 동일한지 검증해야 ANA 그룹이 일관성을 유지함 */
		if (spdk_nvmf_qpair_get_listen_trid(req->qpair, &listen_trid) != 0) {
			/* [한국어] qpair에서 리스너 TID를 가져올 수 없음 (내부 오류) */
			SPDK_ERRLOG("Could not get listener transport ID\n");
			SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
			spdk_nvmf_request_complete(req);
			return;
		}

		listener = nvmf_subsystem_find_listener(subsystem, &listen_trid); /* [한국어] TID → 리스너 매핑 */
		if (listener != ctrlr->listener) {
			/* [한국어] IO qpair의 리스너가 admin qpair 시절 기록한 리스너와 다름 → 거절
			 * NVMe-oF 스펙상 모든 qpair는 동일 fabric 인터페이스를 통해야 함 */
			SPDK_ERRLOG("I/O connect is on a listener different from admin connect\n");
			SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
			spdk_nvmf_request_complete(req);
			return;
		}
	}

	admin_qpair = ctrlr->admin_qpair; /* [한국어] 기존 admin qpair 포인터 (NULL 가능) */

	/* There is a chance that admin qpair was destroyed. This is an issue that was observed only with ESX initiators */
	if (admin_qpair) {
		/* [한국어] admin_qpair가 아직 있으면 활성/group 상태를 스냅샷 */
		admin_qpair_active = spdk_nvmf_qpair_is_active(admin_qpair);
		admin_qpair_group = admin_qpair->group;
		admin_qpair_state = admin_qpair->state;
	}

	if (!admin_qpair_active || admin_qpair_group == NULL) {
		/* There is a chance that admin qpair was destroyed or is being destroyed at this moment due to e.g.
		 * expired keep alive timer. Part of the qpair destruction process is change of qpair's
		 * state to DEACTIVATING and removing it from poll group */
		/* [한국어] admin qpair가 DEACTIVATING 상태이거나 이미 poll group에서 제거된 경우.
		 * 이때 IO qpair를 추가하면 비정상 상태가 될 수 있으므로 거절 */
		SPDK_ERRLOG("Inactive admin qpair (state %d, group %p)\n", admin_qpair_state, admin_qpair_group);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		spdk_nvmf_request_complete(req);
		return;
	}
	qpair->ctrlr = ctrlr; /* [한국어] IO qpair에 ctrlr 연결 (다음 단계에서 NULL로 재초기화됨) */
	spdk_thread_send_msg(admin_qpair_group->thread, nvmf_ctrlr_add_io_qpair, req); /* [한국어] ctrlr->thread로 2단계 검증 위임 */
}

/*
 * [한국어]
 * nvmf_qpair_access_allowed - hostnqn + listener 기반 ACL(접근 제어) 검사
 *
 * @qpair: 접속 중인 qpair (리스너 TID를 얻기 위해 사용).
 * @subsystem: 접속 대상 subsystem (ACL 목록 보유).
 * @hostnqn: 호스트가 제공한 Host NQN 문자열.
 * @return: true이면 접속 허용, false이면 거절.
 *
 * 두 가지 ACL 검사를 순서대로 수행한다:
 *  1) hostnqn 기반: subsystem 설정에 허용된 host NQN 목록이 있으면 그에 포함되는지 확인.
 *  2) 리스너(listen_trid) 기반: 특정 포트/주소에서만 허용하는 경우 qpair의 리스너가
 *     subsystem의 허용 리스너 목록에 있는지 확인.
 *
 * 실행 컨텍스트: _nvmf_ctrlr_connect [subsystem->thread].
 *
 * 호출 체인:
 *   _nvmf_ctrlr_connect → 본 함수 → spdk_nvmf_subsystem_host_allowed,
 *                                    spdk_nvmf_qpair_get_listen_trid,
 *                                    spdk_nvmf_subsystem_listener_allowed
 */
static bool
nvmf_qpair_access_allowed(struct spdk_nvmf_qpair *qpair, struct spdk_nvmf_subsystem *subsystem,
			  const char *hostnqn)
{
	struct spdk_nvme_transport_id listen_trid = {}; /* [한국어] qpair가 연결된 리스너의 transport ID */

	if (!spdk_nvmf_subsystem_host_allowed(subsystem, hostnqn)) {
		/* [한국어] subsystem의 host ACL에 hostnqn이 없음 → 거절
		 * 빈 ACL 목록이면 모든 host 허용, 목록이 있으면 명시된 NQN만 허용 */
		SPDK_ERRLOG("Subsystem '%s' does not allow host '%s'\n", subsystem->subnqn, hostnqn);
		return false;
	}

	if (spdk_nvmf_qpair_get_listen_trid(qpair, &listen_trid)) {
		/* [한국어] qpair에서 리스너 TID를 가져올 수 없음 (transport 내부 오류) */
		SPDK_ERRLOG("Subsystem '%s' is unable to enforce access control due to an internal error.\n",
			    subsystem->subnqn);
		return false;
	}

	if (!spdk_nvmf_subsystem_listener_allowed(subsystem, &listen_trid)) {
		/* [한국어] 이 리스너(포트/주소)는 subsystem에 허가된 리스너 목록에 없음 → 거절 */
		SPDK_ERRLOG("Subsystem '%s' does not allow host '%s' to connect at this address.\n",
			    subsystem->subnqn, hostnqn);
		return false;
	}

	return true; /* [한국어] 모든 ACL 검사 통과 */
}

/*
 * [한국어]
 * _nvmf_ctrlr_connect - Connect 명령의 SQ size/QID 검증 + qid=0/>0 분기
 *
 * @req: Connect 요청.
 * @return: COMPLETE 또는 ASYNCHRONOUS.
 *
 * - sqsize=0 거절 (스펙: SQSIZE는 0-based, 최소 1).
 * - admin queue: sqsize < max_aq_depth, IO queue: sqsize < max_queue_depth.
 * - sq_head_max/qid 설정, current_unassociated_qpairs 감소, admin/io stat++.
 * - qid=0 (admin):
 *     Fabrics + cntlid != 0xFFFF면 거절 (SPDK는 dynamic 모드만 지원).
 *     nvmf_ctrlr_create로 새 ctrlr 생성 → ASYNCHRONOUS.
 * - qid>0 (IO): subsystem->thread로 _nvmf_ctrlr_add_io_qpair 메시지 송신.
 */
static int
_nvmf_ctrlr_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_data *data = req->iov[0].iov_base; /* [한국어] Connect 데이터 캡슐 (cntlid/hostid/subnqn/hostnqn 포함) */
	struct spdk_nvmf_fabric_connect_cmd *cmd = &req->cmd->connect_cmd; /* [한국어] Connect SQE (recfmt, qid, sqsize, kato 포함) */
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp; /* [한국어] Connect 응답 캡슐 (cntlid, authreq 반환) */
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] 연결 중인 qpair */
	struct spdk_nvmf_transport *transport = qpair->transport; /* [한국어] qpair의 transport (RDMA/TCP 등) */
	struct spdk_nvmf_ctrlr *ctrlr; /* [한국어] 신규 admin Connect 시 생성될 컨트롤러 */
	struct spdk_nvmf_subsystem *subsystem; /* [한국어] subnqn으로 찾은 NVM subsystem */

	SPDK_DEBUGLOG(nvmf, "recfmt 0x%x qid %u sqsize %u\n",
		      cmd->recfmt, cmd->qid, cmd->sqsize);

	SPDK_DEBUGLOG(nvmf, "Connect data:\n");
	SPDK_DEBUGLOG(nvmf, "  cntlid:  0x%04x\n", data->cntlid);
	SPDK_DEBUGLOG(nvmf, "  hostid: %08x-%04x-%04x-%02x%02x-%04x%08x ***\n",
		      ntohl(*(uint32_t *)&data->hostid[0]),
		      ntohs(*(uint16_t *)&data->hostid[4]),
		      ntohs(*(uint16_t *)&data->hostid[6]),
		      data->hostid[8],
		      data->hostid[9],
		      ntohs(*(uint16_t *)&data->hostid[10]),
		      ntohl(*(uint32_t *)&data->hostid[12]));
	SPDK_DEBUGLOG(nvmf, "  subnqn: \"%s\"\n", data->subnqn);
	SPDK_DEBUGLOG(nvmf, "  hostnqn: \"%s\"\n", data->hostnqn);

	subsystem = spdk_nvmf_tgt_find_subsystem(transport->tgt, data->subnqn); /* [한국어] target의 subsystem 리스트에서 subnqn 일치하는 항목 검색 */
	if (!subsystem) {
		/* [한국어] 해당 subnqn의 subsystem 없음 → INVALID_PARAM(subnqn 필드) 응답 */
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, subnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->recfmt != 0) {
		/* [한국어] RECFMT(Record Format) ≠ 0은 미지원 포맷 → INCOMPATIBLE_FORMAT 오류 */
		SPDK_ERRLOG("Connect command unsupported RECFMT %u\n", cmd->recfmt);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT; /* [한국어] Fabric SC: Incompatible Format */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/*
	 * SQSIZE is a 0-based value, so it must be at least 1 (minimum queue depth is 2) and
	 * strictly less than max_aq_depth (admin queues) or max_queue_depth (io queues).
	 */
	/* [한국어] SQSIZE는 0-based: 0이면 queue depth=1인데 NVMe 스펙은 최소 2를 요구 → 거절 */
	if (cmd->sqsize == 0) {
		SPDK_ERRLOG("Invalid SQSIZE = 0\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, sqsize); /* [한국어] sqsize 필드를 잘못된 필드로 보고 */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->qid == 0) {
		/* [한국어] Admin qpair: SQSIZE가 max_aq_depth 이상이면 거절 (0-based이므로 < max_aq_depth여야 함) */
		if (cmd->sqsize >= transport->opts.max_aq_depth) {
			SPDK_ERRLOG("Invalid SQSIZE for admin queue %u (min 1, max %u)\n",
				    cmd->sqsize, transport->opts.max_aq_depth - 1);
			SPDK_NVMF_INVALID_CONNECT_CMD(rsp, sqsize);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else if (cmd->sqsize >= transport->opts.max_queue_depth) {
		/* [한국어] IO qpair: SQSIZE가 max_queue_depth 이상이면 거절 */
		SPDK_ERRLOG("Invalid SQSIZE %u (min 1, max %u)\n",
			    cmd->sqsize, transport->opts.max_queue_depth - 1);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, sqsize);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	qpair->sq_head_max = cmd->sqsize; /* [한국어] qpair SQ 최대 깊이 설정 (0-based = SQSIZE 값 그대로 저장) */
	qpair->qid = cmd->qid; /* [한국어] qpair의 QID 확정 (이후 qpair_mask 비트맵 인덱스로 사용) */
	qpair->connect_received = true; /* [한국어] 이 qpair에서 Connect 명령을 정상 수신했음을 표시 */

	/* [한국어] 이 qpair는 더 이상 "unassociated"가 아님: poll group 통계 감소
	 * mutex: current_unassociated_qpairs는 여러 qpair에서 동시에 수정될 수 있음 */
	pthread_mutex_lock(&qpair->group->mutex);
	assert(qpair->group->current_unassociated_qpairs > 0); /* [한국어] Connect 이전 unassociated 상태여야 함 */
	qpair->group->current_unassociated_qpairs--; /* [한국어] 연결 진행 중이므로 unassociated 카운트 감소 */
	pthread_mutex_unlock(&qpair->group->mutex);

	if (0 == qpair->qid) {
		/* [한국어] admin qpair가 추가됨: poll group 통계 갱신 */
		qpair->group->stat.admin_qpairs++; /* [한국어] 누적 admin qpair 생성 카운트 */
		qpair->group->stat.current_admin_qpairs++; /* [한국어] 현재 활성 admin qpair 수 */
	} else {
		/* [한국어] IO qpair가 추가됨: poll group 통계 갱신 */
		qpair->group->stat.io_qpairs++; /* [한국어] 누적 IO qpair 생성 카운트 */
		qpair->group->stat.current_io_qpairs++; /* [한국어] 현재 활성 IO qpair 수 */
	}

	if (cmd->qid == 0) {
		SPDK_DEBUGLOG(nvmf, "Connect Admin Queue for controller ID 0x%x\n", data->cntlid);

		if (spdk_nvme_trtype_is_fabrics(transport->ops->type) && data->cntlid != 0xFFFF) {
			/* This NVMf target only supports dynamic mode. */
			/* [한국어] Fabrics transport는 dynamic ctrlr 모드만 지원.
			 * dynamic 모드에서 호스트는 cntlid=0xFFFF로 보내 target이 할당하도록 해야 함 */
			SPDK_ERRLOG("The NVMf target only supports dynamic mode (CNTLID = 0x%x).\n", data->cntlid);
			SPDK_NVMF_INVALID_CONNECT_DATA(rsp, cntlid); /* [한국어] cntlid 필드를 잘못된 필드로 보고 */
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		/* Establish a new ctrlr */
		/* [한국어] 신규 컨트롤러 객체 생성: vcprop/cdata/qpair_mask/visible_ns 초기화.
		 * 성공하면 비동기 흐름(subsystem→ctrlr thread로 메시지) 시작 */
		ctrlr = nvmf_ctrlr_create(subsystem, req, cmd, data);
		if (!ctrlr) {
			/* [한국어] 컨트롤러 할당 실패 (메모리 부족, listener 없음 등) */
			SPDK_ERRLOG("nvmf_ctrlr_create() failed\n");
			rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else {
			/* [한국어] 비동기: _nvmf_subsystem_add_ctrlr → _nvmf_ctrlr_add_admin_qpair
			 * → nvmf_ctrlr_send_connect_rsp 에서 실제 응답 전송 */
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
	} else {
		/* [한국어] IO qpair(qid>0): subsystem->thread로 전달해 cntlid 유효성/in_destruct/
		 * ANA listener 검증 후 ctrlr->thread로 추가 전달 */
		spdk_thread_send_msg(subsystem->thread, _nvmf_ctrlr_add_io_qpair, req);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS; /* [한국어] 비동기: IO qpair 등록 완료 시 Connect RSP 전송 */
	}
}

/*
 * [한국어]
 * nvmf_subsystem_pg_from_connect_cmd - Connect 요청에서 subsystem poll group 포인터 반환
 *
 * @req: Fabric Connect 요청 (iovcnt==1, ctrlr==NULL 보장).
 * @return: &req->qpair->group->sgroups[subsystem->id] 또는 NULL (subnqn 룩업 실패).
 *
 * 각 poll group은 subsystem별로 sgroups[] 배열을 가지며, Connect 처리 시
 * mgmt_io_outstanding를 증가시킬 올바른 sgroup 슬롯을 찾는 데 사용된다.
 * subsystem->id는 tgt에 등록될 때 할당되는 고유 인덱스이다.
 *
 * 실행 컨텍스트: qpair->group->thread (poll group 소유 스레드).
 *
 * 호출 체인:
 *   nvmf_ctrlr_cmd_connect / spdk_nvmf_ctrlr_connect → 본 함수 → spdk_nvmf_tgt_find_subsystem
 */
static struct spdk_nvmf_subsystem_poll_group *
nvmf_subsystem_pg_from_connect_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_data *data; /* [한국어] Connect 데이터 캡슐 (subnqn 포함) */
	struct spdk_nvmf_subsystem *subsystem; /* [한국어] subnqn → subsystem 룩업 결과 */
	struct spdk_nvmf_tgt *tgt; /* [한국어] 전역 NVMe-oF target */

	assert(nvmf_request_is_fabric_connect(req)); /* [한국어] Fabric Connect 명령인지 확인 */
	assert(req->qpair->ctrlr == NULL); /* [한국어] admin Connect 단계에서 ctrlr 미할당 상태 */
	assert(req->iovcnt == 1); /* [한국어] Connect 데이터 캡슐은 단일 iov */

	data = req->iov[0].iov_base; /* [한국어] Connect 데이터 캡슐 추출 */
	tgt = req->qpair->transport->tgt; /* [한국어] transport를 통해 tgt 획득 */

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, data->subnqn); /* [한국어] subnqn → subsystem 룩업 */
	if (subsystem == NULL) {
		/* [한국어] 해당 subnqn의 subsystem이 없음 → NULL 반환 (호출자가 오류 응답 처리) */
		return NULL;
	}

	return &req->qpair->group->sgroups[subsystem->id]; /* [한국어] 이 poll group에서 subsystem에 해당하는 sgroup 슬롯 */
}

SPDK_LOG_DEPRECATION_REGISTER(spdk_nvmf_ctrlr_connect, "", "v26.05",
			      SPDK_LOG_DEPRECATION_EVERY_24H);

/*
 * [한국어]
 * spdk_nvmf_ctrlr_connect - deprecated 레거시 Connect 진입점 (v26.05에서 제거 예정)
 *
 * @req: Fabric Connect 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE 또는 ASYNCHRONOUS.
 *
 * nvmf_ctrlr_cmd_connect()로 이전된 Connect 처리를 직접 노출하던 구 공개 API.
 * 현재는 deprecated 경고를 발행한 뒤 내부 함수를 호출하는 래퍼 역할만 한다.
 * 외부 transport driver가 직접 호출하던 코드의 이전 호환성을 위해 잠시 유지됨.
 *
 * 실행 컨텍스트: qpair->group->thread.
 *
 * 호출 체인:
 *   [외부 transport driver] → 본 함수 → nvmf_subsystem_pg_from_connect_cmd,
 *                                        _nvmf_ctrlr_connect, _nvmf_request_complete
 */
int
spdk_nvmf_ctrlr_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp; /* [한국어] Connect 응답 버퍼 */
	struct spdk_nvmf_subsystem_poll_group *sgroup; /* [한국어] 이 Connect가 속할 sgroup */
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] 연결 중인 qpair */
	enum spdk_nvmf_request_exec_status status; /* [한국어] 처리 결과 상태 */

	SPDK_LOG_DEPRECATED(spdk_nvmf_ctrlr_connect); /* [한국어] 호출 빈도 제한 방식(24h당 1번)으로 deprecated 경고 출력 */

	if (req->iovcnt > 1) {
		/* [한국어] Connect 데이터는 단일 iov에 있어야 함; 복수 iov는 비정상 */
		SPDK_ERRLOG("Connect command invalid iovcnt: %d\n", req->iovcnt);
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		goto out;
	}

	sgroup = nvmf_subsystem_pg_from_connect_cmd(req); /* [한국어] subnqn으로 sgroup 찾기 */
	if (!sgroup) {
		/* [한국어] subnqn에 해당하는 subsystem 없음 */
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, subnqn);
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		goto out;
	}

	sgroup->mgmt_io_outstanding++; /* [한국어] Connect는 management I/O로 계산; 완료 시 감소 */
	TAILQ_INSERT_TAIL(&qpair->outstanding, req, link); /* [한국어] qpair 미완료 큐에 요청 등록 */

	status = _nvmf_ctrlr_connect(req); /* [한국어] SQSIZE/QID/cntlid 검증 후 admin/IO 분기 처리 */

out:
	if (status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		_nvmf_request_complete(req); /* [한국어] 동기 완료 경로: 응답 전송 + outstanding 큐에서 제거 */
	}

	return status;
}

/*
 * [한국어]
 * nvmf_ctrlr_cmd_connect - Fabric Connect 명령(fctype=01h)의 진입점
 *
 * @req: Connect 요청.
 * @return: COMPLETE/ASYNCHRONOUS.
 *
 * 호스트가 admin(qid=0) 또는 IO(qid>0) qpair을 만들 때 보내는 명령.
 * 검증:
 *  - in-capsule data 길이 = sizeof(spdk_nvmf_fabric_connect_data) (cntlid/hostid/subnqn/hostnqn).
 *  - subsystem 룩업 (subnqn). 없으면 INVALID_PARAM.
 *  - subsystem이 INACTIVE/PAUSING/PAUSED/DEACTIVATING이면 sgroup->queued로 큐잉(나중에 재시도).
 *  - hostnqn null-terminated 검사.
 *  - subsystem ACL (host_allowed) + listener ACL.
 * 통과하면 _nvmf_ctrlr_connect로 진행.
 */
static int
nvmf_ctrlr_cmd_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_data *data = req->iov[0].iov_base; /* [한국어] Connect 데이터 캡슐 (cntlid/hostid/subnqn/hostnqn) */
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp; /* [한국어] Fabric Connect 응답 캡슐 */
	struct spdk_nvmf_transport *transport = req->qpair->transport; /* [한국어] 이 qpair의 transport (subnqn 룩업을 위해 tgt 접근) */
	struct spdk_nvmf_subsystem *subsystem; /* [한국어] subnqn으로 찾은 대상 subsystem */

	if (req->length < sizeof(struct spdk_nvmf_fabric_connect_data)) {
		/* [한국어] Connect 데이터 캡슐이 너무 작음: cntlid/hostid/subnqn/hostnqn 필드가 모두 있어야 함 */
		SPDK_ERRLOG("Connect command data length 0x%x too small\n", req->length);
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD; /* [한국어] INVALID_FIELD: 데이터 길이 부족 */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->iovcnt > 1) {
		/* [한국어] Connect 데이터는 단일 iov에 들어와야 함; 분할된 경우 비정상 */
		SPDK_ERRLOG("Connect command invalid iovcnt: %d\n", req->iovcnt);
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(transport->tgt, data->subnqn); /* [한국어] target에 등록된 subsystem 중 subnqn이 일치하는 항목 검색 */
	if (!subsystem) {
		/* [한국어] 알 수 없는 subnqn → INVALID_PARAM(subnqn 필드) 반환 */
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, subnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if ((subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE) ||
	    (subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSING) ||
	    (subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED) ||
	    (subsystem->state == SPDK_NVMF_SUBSYSTEM_DEACTIVATING)) {
		struct spdk_nvmf_subsystem_poll_group *sgroup;

		/* Subsystem is not ready to handle a connect. Decrement
		 * the mgmt_io_outstanding to avoid the subsystem waiting
		 * for this command to complete before unpausing. Queued
		 * requests get retried when subsystem resumes.
		 */
		/* [한국어] subsystem이 INACTIVE/PAUSING/PAUSED/DEACTIVATING 상태: Connect 처리 불가.
		 * mgmt_io_outstanding을 다시 줄이고 outstanding에서 제거한 뒤 queued로 이동.
		 * subsystem이 ACTIVE로 전환되면 sgroup->queued 큐에서 꺼내 재시도됨. */
		sgroup = nvmf_subsystem_pg_from_connect_cmd(req); /* [한국어] 이 Connect가 속할 sgroup 찾기 */
		assert(sgroup != NULL); /* [한국어] subsystem이 존재하면 sgroup도 있어야 함 */
		sgroup->mgmt_io_outstanding--; /* [한국어] queued로 이동하므로 outstanding 카운트 감소 (PAUSING 중 체크와 레이스 방지) */
		TAILQ_REMOVE(&req->qpair->outstanding, req, link); /* [한국어] qpair outstanding 큐에서 제거 (queued로 이전) */
		TAILQ_INSERT_TAIL(&sgroup->queued, req, link); /* [한국어] subsystem 재개 시 재처리할 큐에 삽입 */
		SPDK_DEBUGLOG(nvmf, "Subsystem '%s' is not ready for connect, retrying...\n", subsystem->subnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS; /* [한국어] 비동기: subsystem 재개 시 자동 재실행 */
	}

	/* Ensure that hostnqn is null terminated */
	/* [한국어] hostnqn은 반드시 null-terminated 문자열이어야 함. 없으면 메모리 over-read 위험 */
	if (!memchr(data->hostnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1)) {
		SPDK_ERRLOG("Connect HOSTNQN is not null terminated\n");
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, hostnqn); /* [한국어] hostnqn 필드를 잘못된 필드로 보고 */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (!nvmf_qpair_access_allowed(req->qpair, subsystem, data->hostnqn)) {
		/* [한국어] hostnqn이 subsystem의 host ACL에 없거나 listener 접근이 차단됨
		 * INVALID_HOST: 이 호스트는 해당 subsystem에 접근 권한 없음 */
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_HOST; /* [한국어] Fabric SC=04h: Invalid Host Identifier */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return _nvmf_ctrlr_connect(req); /* [한국어] 검증 통과 → SQSIZE/QID/cntlid 최종 검증 + admin/IO 분기 처리 */
}

/*
 * [한국어]
 * nvmf_ctrlr_association_remove - Association 타임아웃 폴러 콜백 (CC.EN=0 후 일정 시간 경과)
 *
 * @ctx: spdk_nvmf_ctrlr* (타임아웃이 발생한 컨트롤러).
 * @return: SPDK_POLLER_IDLE (소멸 중) 또는 SPDK_POLLER_BUSY (disconnect 발동).
 *
 * CC.EN이 0으로 전환된 후(리셋 또는 셧다운) association_timeout ms가 경과하면
 * 본 폴러가 발화하여 admin qpair를 강제로 disconnect한다. NVMe 스펙상
 * CC.EN=0 후에도 호스트는 association_timeout 시간 내에 재접속할 수 있으나,
 * 그 시간이 지나면 target은 자원을 해제할 수 있다.
 *
 * 실행 컨텍스트: ctrlr->thread (SPDK_POLLER_REGISTER로 등록된 폴러 콜백).
 *
 * 호출 체인:
 *   SPDK 폴러 프레임워크 → 본 함수 → nvmf_ctrlr_stop_association_timer,
 *                                      spdk_nvmf_qpair_disconnect(admin_qpair)
 */
static int
nvmf_ctrlr_association_remove(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx; /* [한국어] 타임아웃이 발생한 컨트롤러 */
	int rc; /* [한국어] disconnect 반환값 */

	nvmf_ctrlr_stop_association_timer(ctrlr); /* [한국어] 폴러를 먼저 해제해 재발화 방지 */

	if (ctrlr->in_destruct) {
		/* [한국어] 이미 소멸 중이면 추가 조작 불필요 */
		return SPDK_POLLER_IDLE;
	}
	SPDK_DEBUGLOG(nvmf, "Disconnecting host from subsystem %s due to association timeout.\n",
		      ctrlr->subsys->subnqn);

	if (ctrlr->admin_qpair) {
		/* [한국어] admin qpair를 강제 disconnect → 연쇄적으로 ctrlr destruct 트리거 */
		rc = spdk_nvmf_qpair_disconnect(ctrlr->admin_qpair);
		if (rc < 0 && rc != -EINPROGRESS) {
			/* [한국어] -EINPROGRESS는 이미 disconnect 중인 정상 상태; 그 외는 비정상 */
			SPDK_ERRLOG("Fail to disconnect admin ctrlr qpair\n");
			assert(false);
		}
	}

	return SPDK_POLLER_BUSY; /* [한국어] disconnect 발동됨 */
}

/*
 * [한국어]
 * _nvmf_ctrlr_cc_reset_shn_done - CC 리셋/셧다운 완료 대기 폴러
 *
 * @ctx: spdk_nvmf_ctrlr*.
 * @return: SPDK_POLLER_IDLE (아직 대기 중) 또는 SPDK_POLLER_BUSY (완료 처리됨).
 *
 * CC.EN=0(리셋) 또는 CC.SHN≠0(셧다운)이 발동된 후 모든 IO qpair들이 disconnect
 * 완료되기를 기다리는 폴러. qpair_mask의 남은 비트 수(count)가 1이면 admin qpair만
 * 남은 것이므로 완료로 판단한다.
 *
 * 완료 시 동작:
 *  - cc_timeout_timer 해제.
 *  - 셧다운이면 CSTS.SHST=SHST_COMPLETE(2h).
 *  - 리셋이면 CC.raw=0, CSTS.raw=0 (레지스터 클리어).
 *  - NSSR(NVM Subsystem Reset)로 발동됐으면 CSTS.NSSRO=1 설정.
 *  - association_timer 재등록 (2분 내 재접속 허용).
 *  - disconnect_in_progress=false.
 *
 * 타임아웃(cc_timeout_tsc 경과) 시에도 IO가 남아 있으면 CFS=1(Fatal Status) 설정.
 *
 * 실행 컨텍스트: ctrlr->thread (100ms SPDK 폴러).
 *
 * 호출 체인:
 *   nvmf_prop_set_cc(EN→0 또는 SHN≠0)
 *     → for_each_channel(nvmf_ctrlr_disconnect_io_qpairs_on_pg, nvmf_ctrlr_cc_reset_shn_done)
 *     → nvmf_ctrlr_cc_reset_shn_done → _nvmf_ctrlr_cc_reset_shn_done [폴러 등록]
 *     → SPDK 폴러 → [본 함수]
 */
static int
_nvmf_ctrlr_cc_reset_shn_done(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx; /* [한국어] 리셋/셧다운 중인 컨트롤러 */
	uint64_t now = spdk_get_ticks(); /* [한국어] 현재 타임스탬프 (tsc 단위) */
	uint32_t count; /* [한국어] 현재 qpair_mask에 set된 비트 수 (활성 qpair 수) */

	if (ctrlr->cc_timer) {
		spdk_poller_unregister(&ctrlr->cc_timer); /* [한국어] 이전 100ms 재시도 폴러 해제 */
	}

	count = spdk_bit_array_count_set(ctrlr->qpair_mask); /* [한국어] 남은 활성 qpair 수 조회 */
	SPDK_DEBUGLOG(nvmf, "ctrlr %p active queue count %u\n", ctrlr, count);

	if (count > 1) {
		/* [한국어] 아직 IO qpair(들)이 disconnect 완료되지 않음 (count=1이면 admin만 남은 것) */
		if (now < ctrlr->cc_timeout_tsc) {
			/* restart cc timer */
			/* [한국어] 타임아웃 전이면 100ms 후 재폴링 */
			ctrlr->cc_timer = SPDK_POLLER_REGISTER(_nvmf_ctrlr_cc_reset_shn_done, ctrlr, 100 * 1000);
			return SPDK_POLLER_IDLE;
		} else {
			/* controller fatal status */
			/* [한국어] 타임아웃 경과 후에도 IO가 미완료 → CSTS.CFS=1 설정 (치명 오류) */
			SPDK_WARNLOG("IO timeout, ctrlr %p is in fatal status\n", ctrlr);
			nvmf_ctrlr_set_fatal_status(ctrlr); /* [한국어] CSTS.CFS=1: 호스트가 다음 명령에서 인지 */
		}
	}

	spdk_poller_unregister(&ctrlr->cc_timeout_timer); /* [한국어] CC 타임아웃 감시 폴러 해제 */

	if (ctrlr->disconnect_is_shn) {
		/* [한국어] 셧다운으로 발동된 경우: CSTS.SHST=SHST_COMPLETE(2h)로 완료 알림 */
		ctrlr->vcprop.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
		ctrlr->disconnect_is_shn = false; /* [한국어] 셧다운 플래그 클리어 */
	} else {
		/* Only a subset of the registers are cleared out on a reset */
		/* [한국어] 리셋(EN=0)의 경우: CC와 CSTS를 0으로 초기화 (재접속 시 재설정) */
		ctrlr->vcprop.cc.raw = 0; /* [한국어] CC 레지스터 전체 클리어 */
		ctrlr->vcprop.csts.raw = 0; /* [한국어] CSTS 레지스터 전체 클리어 */
	}

	if (ctrlr->executing_nssr) {
		/* [한국어] NVM Subsystem Reset(NSSR 레지스터 4핫도그 값 쓰기)으로 리셋된 경우
		 * CSTS.NSSRO=1로 NVM Subsystem Reset Occurred 알림 */
		ctrlr->executing_nssr = false;
		ctrlr->vcprop.csts.bits.nssro = 1; /* [한국어] NVM Subsystem Reset Occurred 비트 set */
	}

	/* After CC.EN transitions to 0 (due to shutdown or reset), the association
	 * between the host and controller shall be preserved for at least 2 minutes */
	if (ctrlr->association_timer) {
		/* [한국어] 이미 association 타이머가 실행 중이면 중지 후 재등록 */
		SPDK_DEBUGLOG(nvmf, "Association timer already set\n");
		nvmf_ctrlr_stop_association_timer(ctrlr);
	}
	if (ctrlr->association_timeout) {
		/* [한국어] association_timeout ms 후 nvmf_ctrlr_association_remove 발동 */
		ctrlr->association_timer = SPDK_POLLER_REGISTER(nvmf_ctrlr_association_remove, ctrlr,
					   ctrlr->association_timeout * 1000);
	}
	ctrlr->disconnect_in_progress = false; /* [한국어] disconnect 처리 완료 표시 */
	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * nvmf_ctrlr_cc_reset_shn_done - for_each_channel 완료 콜백 (IO qpair disconnect 순회 완료 후)
 *
 * @i: spdk_io_channel_iter (순회 상태 + ctx=ctrlr 포함).
 * @status: 0이면 성공, 음수면 오류.
 *
 * nvmf_prop_set_cc()에서 spdk_for_each_channel()을 통해 모든 poll group의
 * IO qpair를 disconnect한 뒤 최종적으로 호출되는 완료 콜백.
 * 실패 시 assert(false)로 비정상 종료하고, 성공 시 _nvmf_ctrlr_cc_reset_shn_done()을
 * 즉시 호출하여 qpair 잔여 확인 및 레지스터 갱신을 수행한다.
 *
 * 실행 컨텍스트: ctrlr->thread (for_each_channel 완료는 원래 스레드로 돌아옴).
 *
 * 호출 체인:
 *   spdk_for_each_channel 완료 → 본 함수 → _nvmf_ctrlr_cc_reset_shn_done
 */
static void
nvmf_ctrlr_cc_reset_shn_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_ctrlr *ctrlr = spdk_io_channel_iter_get_ctx(i); /* [한국어] 순회 ctx에서 ctrlr 추출 */

	if (status < 0) {
		/* [한국어] poll group 순회 중 오류 발생 → 비정상 종료 */
		SPDK_ERRLOG("Fail to disconnect io ctrlr qpairs\n");
		assert(false);
	}

	_nvmf_ctrlr_cc_reset_shn_done((void *)ctrlr); /* [한국어] 잔여 qpair 확인 + 레지스터/타이머 정리 */
}

/*
 * [한국어]
 * nvmf_bdev_complete_reset - bdev 강제 리셋 완료 콜백
 *
 * @bdev_io: 완료된 bdev I/O 핸들.
 * @success: true이면 리셋 성공, false이면 실패.
 * @cb_arg: 미사용 (NULL).
 *
 * nvmf_ctrlr_cc_timeout()에서 CC 리셋/셧다운 타임아웃 시 강제 bdev reset을
 * 발동한 뒤 완료 알림을 받는 콜백. 로그만 출력하고 bdev_io를 해제한다.
 *
 * 실행 컨텍스트: bdev I/O 완료를 처리하는 poll group 스레드.
 *
 * 호출 체인:
 *   nvmf_ctrlr_cc_timeout → spdk_bdev_reset → [bdev 처리] → 본 함수 → spdk_bdev_free_io
 */
static void
nvmf_bdev_complete_reset(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	SPDK_NOTICELOG("Resetting bdev done with %s\n", success ? "success" : "failure");

	spdk_bdev_free_io(bdev_io); /* [한국어] bdev I/O 자원 해제 (메모리 반환) */
}


/*
 * [한국어]
 * nvmf_ctrlr_cc_timeout - CC 리셋/셧다운 타임아웃 폴러 콜백 (강제 bdev 리셋)
 *
 * @ctx: spdk_nvmf_ctrlr*.
 * @return: SPDK_POLLER_IDLE (admin qpair 없음) 또는 SPDK_POLLER_BUSY (bdev 리셋 발동).
 *
 * CC.EN=0 또는 CC.SHN≠0 발동 후 일정 시간(NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS) 내에
 * IO qpair들이 모두 disconnect되지 않으면 본 폴러가 발화하여 모든 NS의 bdev를
 * 강제 reset한다. 이는 bdev IO가 block된 상황에서 컨트롤러가 영원히 대기하는 것을
 * 방지하기 위한 안전 장치이다.
 *
 * bdev 리셋 완료 후에는 nvmf_bdev_complete_reset()이 호출된다. 실제 IO qpair
 * disconnect 완료 폴링은 _nvmf_ctrlr_cc_reset_shn_done()이 계속 수행한다.
 *
 * 실행 컨텍스트: ctrlr->thread (SPDK 폴러).
 *
 * 호출 체인:
 *   SPDK 폴러 → 본 함수 → spdk_bdev_reset → [bdev 처리] → nvmf_bdev_complete_reset
 */
static int
nvmf_ctrlr_cc_timeout(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx; /* [한국어] 타임아웃이 발생한 컨트롤러 */
	struct spdk_nvmf_poll_group *group; /* [한국어] admin qpair의 poll group (bdev channel 보유) */
	struct spdk_nvmf_ns *ns; /* [한국어] 순회 중인 네임스페이스 */
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info; /* [한국어] NS별 bdev channel 정보 */

	spdk_poller_unregister(&ctrlr->cc_timeout_timer); /* [한국어] 본 폴러 해제 (one-shot) */
	SPDK_DEBUGLOG(nvmf, "Ctrlr %p reset or shutdown timeout\n", ctrlr);

	if (!ctrlr->admin_qpair) {
		/* [한국어] admin qpair가 이미 disconnect됐으면 bdev 리셋 불필요 */
		SPDK_NOTICELOG("Ctrlr %p admin qpair disconnected\n", ctrlr);
		return SPDK_POLLER_IDLE;
	}

	group = ctrlr->admin_qpair->group; /* [한국어] admin qpair가 속한 poll group */
	assert(group != NULL && group->sgroups != NULL); /* [한국어] group은 NULL이 아니어야 함 */

	for (ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(ctrlr->subsys, ns)) {
		/* [한국어] 모든 NS를 순회하며 bdev가 있는 것만 강제 리셋 */
		if (ns->bdev == NULL) {
			continue; /* [한국어] bdev 없는 NS(빈 슬롯)은 건너뜀 */
		}
		ns_info = &group->sgroups[ctrlr->subsys->id].ns_info[ns->opts.nsid - 1]; /* [한국어] nsid(1-based) → 0-based 인덱스로 sgroup NS 정보 접근 */
		SPDK_NOTICELOG("Ctrlr %p resetting NSID %u\n", ctrlr, ns->opts.nsid);
		spdk_bdev_reset(ns->desc, ns_info->channel, nvmf_bdev_complete_reset, NULL); /* [한국어] bdev 강제 리셋 발동 (inflight I/O 중단) */
	}

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * spdk_nvmf_ctrlr_get_regs - 컨트롤러의 가상 NVMe 레지스터 집합 포인터 반환
 *
 * @ctrlr: 조회할 컨트롤러.
 * @return: &ctrlr->vcprop (읽기 전용 포인터).
 *
 * vcprop은 NVMe-oF에서 실제 PCIe BAR 대신 사용하는 인메모리 레지스터 집합이다.
 * 호스트는 Property Get/Set Fabrics 명령으로 이 레지스터들을 읽고 쓴다.
 * 이 함수는 외부 모듈(transport, RPC 등)이 레지스터 값을 조회할 수 있도록 노출한다.
 *
 * 실행 컨텍스트: 모든 스레드 (읽기 전용, 동기화 불필요).
 */
const struct spdk_nvmf_registers *
spdk_nvmf_ctrlr_get_regs(struct spdk_nvmf_ctrlr *ctrlr)
{
	return &ctrlr->vcprop; /* [한국어] 가상 NVMe 레지스터 구조체 포인터 반환 */
}

/*
 * [한국어]
 * nvmf_ctrlr_set_fatal_status - 컨트롤러의 Fatal Status(CSTS.CFS) 비트를 set
 *
 * @ctrlr: 치명 오류 상태로 전환할 컨트롤러.
 *
 * CSTS.CFS=1로 설정하면 호스트는 다음 Property Get 또는 명령 완료 시
 * 컨트롤러가 치명 오류 상태임을 인지하고 리셋을 수행해야 한다.
 * KATO 만료, CC 타임아웃, AER Critical Warning 등의 상황에서 호출된다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 */
void
nvmf_ctrlr_set_fatal_status(struct spdk_nvmf_ctrlr *ctrlr)
{
	ctrlr->vcprop.csts.bits.cfs = 1; /* [한국어] CSTS(Controller Status) Fatal Status 비트 set */
}

/*
 * [한국어]
 * nvmf_prop_get_cap - CAP(Controller Capabilities) 레지스터 읽기
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 64비트 CAP 레지스터 원시값.
 *
 * CAP는 읽기 전용 레지스터로 nvmf_ctrlr_create()에서 한 번 초기화되며
 * 이후 변경되지 않는다. NVMe-oF는 물리 BAR 없이 vcprop.cap에 저장한다.
 * Property Get 명령의 cap 오프셋(0x0000)에 대응한다.
 *
 * 호출 체인: find_prop → nvmf_property_get → 본 함수
 */
static uint64_t
nvmf_prop_get_cap(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.cap.raw; /* [한국어] CAP 레지스터 64비트 원시값 반환 */
}

/*
 * [한국어]
 * nvmf_prop_get_vs - VS(Version) 레지스터 읽기
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 32비트 VS 레지스터 원시값.
 *
 * VS는 NVMe 스펙 버전을 나타내며 nvmf_ctrlr_create()에서 vs.mjr=2, vs.mnr=0으로
 * 초기화된다. Property Get 오프셋 0x0008에 대응한다.
 *
 * 호출 체인: find_prop → nvmf_property_get → 본 함수
 */
static uint64_t
nvmf_prop_get_vs(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.vs.raw; /* [한국어] VS(Version) 레지스터 32비트 원시값 반환 */
}

/*
 * [한국어]
 * nvmf_prop_get_cc - CC(Controller Configuration) 레지스터 읽기
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 32비트 CC 레지스터 원시값.
 *
 * 호스트는 Property Get으로 CC를 읽어 자신이 쓴 값이 반영됐는지 확인하거나
 * 현재 설정값을 조회한다. Property Get 오프셋 0x0014에 대응한다.
 *
 * 호출 체인: find_prop → nvmf_property_get → 본 함수
 */
static uint64_t
nvmf_prop_get_cc(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.cc.raw; /* [한국어] CC(Controller Configuration) 레지스터 32비트 원시값 반환 */
}

/*
 * [한국어]
 * nvmf_prop_get_nssr - NSSR(NVM Subsystem Reset) 레지스터 읽기 (항상 0 반환)
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 항상 0.
 *
 * NSSR은 쓰기 전용 레지스터이다. 호스트가 0x4E564D65 ('NVMe')를 쓰면
 * NVM Subsystem Reset이 트리거된다. 읽으면 항상 0을 반환하는 것이 스펙이다.
 * Property Get 오프셋 0x0020에 대응한다.
 *
 * 호출 체인: find_prop → nvmf_property_get → 본 함수
 */
static uint64_t
nvmf_prop_get_nssr(struct spdk_nvmf_ctrlr *ctrlr)
{
	return 0; /* [한국어] NSSR은 쓰기 전용 — 읽기 시 항상 0 반환 (NVMe 스펙 §3.1) */
}

/*
 * [한국어]
 * nvmf_prop_set_cc - CC(Controller Configuration) 레지스터 쓰기 처리
 *
 * @ctrlr: 컨트롤러.
 * @value: 호스트가 쓴 32비트 값.
 * @return: true=수용, false=거절(INVALID_PARAM).
 *
 * NVMe 호스트의 정상 시퀀스:
 *   1) Connect(qid=0) → AQA/ASQ/ACQ Property Set → CC.EN=1 set
 *      → CSTS.RDY=1 폴링 → IO Connect(qid>0) → 정상 IO
 *   2) 종료 시 CC.SHN=01b/10b → CSTS.SHST=10b 폴링 → CC.EN=0
 *
 * 본 함수는 비트별 변경(diff) 분석 후 다음을 처리한다:
 *  - EN 1→0: nvmf_ctrlr_disconnect_io_qpairs_on_pg를 모든 PG에 디스패치하고
 *    cc_timeout_timer를 걸어 fatal status 처리. 끝나면 association_timer 시작.
 *  - EN 0→1: association_timer 정지, CSTS.RDY=1 (호스트는 이걸 폴링하다 IO Connect로 진행).
 *  - SHN: NORMAL/ABRUPT shutdown 처리. EN=0과 거의 동일하지만 CSTS.SHST를 셋팅.
 *  - IOSQES/IOCQES: SQ/CQ entry size 갱신 (호스트가 16/64B로 보고).
 *  - AMS/MPS/CSS: SPDK 비지원 또는 제한값 검사.
 *
 * 호출 컨텍스트: ctrlr->thread (Property Set capsule 처리 경로).
 */
static bool
nvmf_prop_set_cc(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	union spdk_nvme_cc_register cc, diff; /* [한국어] cc=새 값, diff=비트 변경 마스크 (XOR 결과) */
	uint32_t cc_timeout_ms; /* [한국어] cc_timeout_timer와 호스트 타임아웃 사이 중간값 계산용 */

	cc.raw = value; /* [한국어] 호스트가 쓴 새 CC 값 (EN/SHN/IOSQES/IOCQES/CSS/MPS/AMS 포함) */

	SPDK_DEBUGLOG(nvmf, "cur CC: 0x%08x\n", ctrlr->vcprop.cc.raw);
	SPDK_DEBUGLOG(nvmf, "new CC: 0x%08x\n", cc.raw);

	/*
	 * Calculate which bits changed between the current and new CC.
	 * Mark each bit as 0 once it is handled to determine if any unhandled bits were changed.
	 */
	/* [한국어] 변경된 비트만 추출: XOR(새값, 현재값).
	 * 각 비트를 처리할 때마다 diff의 해당 비트를 0으로 클리어하여 처리되지 않은 비트를 추적 */
	diff.raw = cc.raw ^ ctrlr->vcprop.cc.raw;

	if (diff.bits.en) {
		/* [한국어] EN 비트가 변경됨: Enable(0→1) 또는 Disable/Reset(1→0) */
		if (cc.bits.en) {
			/* [한국어] CC.EN = 0→1: 컨트롤러 활성화. association timer 중지 후 RDY=1 설정 */
			SPDK_DEBUGLOG(nvmf, "Property Set CC Enable!\n");
			nvmf_ctrlr_stop_association_timer(ctrlr); /* [한국어] 재활성화이면 이전 association 타이머 취소 */

			ctrlr->vcprop.cc.bits.en = 1; /* [한국어] vcprop.cc.en=1 (이후 IO Connect 허용) */
			ctrlr->vcprop.csts.bits.rdy = 1; /* [한국어] CSTS.RDY=1: 컨트롤러 준비 완료 표시 */
		} else {
			/* [한국어] CC.EN = 1→0: 컨트롤러 리셋(Disable). IO qpair들을 모두 끊고
			 * cc_timeout_timer를 시작해 일정 시간 안에 완료 안 되면 강제 에러 처리 */
			SPDK_DEBUGLOG(nvmf, "Property Set CC Disable!\n");
			if (ctrlr->disconnect_in_progress) {
				/* [한국어] 이미 이전 SHN 또는 다른 disconnect가 진행 중 → 무시 */
				SPDK_DEBUGLOG(nvmf, "Disconnect in progress\n");
				return true;
			}

			/* [한국어] cc_timeout_timer: NVMF_CC_RESET_SHN_TIMEOUT_IN_MS(10s) 후 강제 에러 처리 */
			ctrlr->cc_timeout_timer = SPDK_POLLER_REGISTER(nvmf_ctrlr_cc_timeout, ctrlr,
						  NVMF_CC_RESET_SHN_TIMEOUT_IN_MS * 1000);
			/* Make sure cc_timeout_ms is between cc_timeout_timer and Host reset/shutdown timeout */
			/* [한국어] cc_timeout_tsc: (타이머 만료 시각 + 호스트 타임아웃) / 2 지점. 중간값으로
			 * 타이머 발화 전에 CSTS.RDY=0이 완료되지 않으면 강제 조치 발동 */
			cc_timeout_ms = (NVMF_CC_RESET_SHN_TIMEOUT_IN_MS + NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS) / 2;
			ctrlr->cc_timeout_tsc = spdk_get_ticks() + cc_timeout_ms * spdk_get_ticks_hz() / (uint64_t)1000;

			ctrlr->vcprop.cc.bits.en = 0; /* [한국어] EN=0 즉시 반영 */
			ctrlr->disconnect_in_progress = true; /* [한국어] 이중 진입 방지 플래그 */
			ctrlr->disconnect_is_shn = false; /* [한국어] 이번은 SHN이 아닌 Reset(EN→0) */
			/* [한국어] 모든 reactor의 poll group에서 IO qpair(admin 제외) disconnect 시작 */
			spdk_for_each_channel(ctrlr->subsys->tgt,
					      nvmf_ctrlr_disconnect_io_qpairs_on_pg, /* [한국어] IO qpair만 끊음 */
					      ctrlr,
					      nvmf_ctrlr_cc_reset_shn_done); /* [한국어] 모든 pg 순회 완료 콜백 */
		}
		diff.bits.en = 0; /* [한국어] EN 비트 처리 완료 표시 */
	}

	if (diff.bits.shn) {
		/* [한국어] SHN(Shutdown Notification) 비트 변경:
		 * 01b=Normal, 10b=Abrupt shutdown 요청. 00b=클리어 */
		if (cc.bits.shn == SPDK_NVME_SHN_NORMAL ||
		    cc.bits.shn == SPDK_NVME_SHN_ABRUPT) {
			/* [한국어] SHN=Normal(01b) 또는 Abrupt(10b): 셧다운 시퀀스 시작 */
			SPDK_DEBUGLOG(nvmf, "Property Set CC Shutdown %u%ub!\n",
				      cc.bits.shn >> 1, cc.bits.shn & 1);
			if (ctrlr->disconnect_in_progress) {
				/* [한국어] 이미 이전 Reset 또는 다른 SHN이 진행 중 → 무시 */
				SPDK_DEBUGLOG(nvmf, "Disconnect in progress\n");
				return true;
			}

			/* [한국어] cc_timeout_timer 및 cc_timeout_tsc 설정 (Reset과 동일 메커니즘) */
			ctrlr->cc_timeout_timer = SPDK_POLLER_REGISTER(nvmf_ctrlr_cc_timeout, ctrlr,
						  NVMF_CC_RESET_SHN_TIMEOUT_IN_MS * 1000);
			/* Make sure cc_timeout_ms is between cc_timeout_timer and Host reset/shutdown timeout */
			cc_timeout_ms = (NVMF_CC_RESET_SHN_TIMEOUT_IN_MS + NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS) / 2;
			ctrlr->cc_timeout_tsc = spdk_get_ticks() + cc_timeout_ms * spdk_get_ticks_hz() / (uint64_t)1000;

			ctrlr->vcprop.cc.bits.shn = cc.bits.shn; /* [한국어] SHN 값 기록 (CSTS.SHST 전환 추적에 사용) */
			ctrlr->disconnect_in_progress = true; /* [한국어] 이중 진입 방지 */
			ctrlr->disconnect_is_shn = true; /* [한국어] 이번은 SHN(셧다운)임을 표시 */
			/* [한국어] IO qpair만 끊기 시작. 완료 후 nvmf_ctrlr_cc_reset_shn_done이 CSTS.SHST=완료로 변경 */
			spdk_for_each_channel(ctrlr->subsys->tgt,
					      nvmf_ctrlr_disconnect_io_qpairs_on_pg,
					      ctrlr,
					      nvmf_ctrlr_cc_reset_shn_done);

			/* From the time a shutdown is initiated the controller shall disable
			 * Keep Alive timer */
			/* [한국어] SHN 시작 시점부터 KA 타이머 비활성 (스펙 §5.18: SHN 시 KA 불필요) */
			nvmf_ctrlr_stop_keep_alive_timer(ctrlr);
		} else if (cc.bits.shn == 0) {
			/* [한국어] SHN=00b(클리어): 호스트가 셧다운 상태를 초기화. 현재 vcprop에 반영 */
			ctrlr->vcprop.cc.bits.shn = 0;
		} else {
			/* [한국어] 예약된 SHN 값(11b): 무효 → false 반환으로 INVALID_PARAM */
			SPDK_ERRLOG("Prop Set CC: Invalid SHN value %u%ub\n",
				    cc.bits.shn >> 1, cc.bits.shn & 1);
			return false;
		}
		diff.bits.shn = 0; /* [한국어] SHN 비트 처리 완료 */
	}

	if (diff.bits.iosqes) {
		/* [한국어] IOSQES(I/O SQ Entry Size) 변경: log2(SQ entry size). 64B이면 6 */
		SPDK_DEBUGLOG(nvmf, "Prop Set IOSQES = %u (%u bytes)\n",
			      cc.bits.iosqes, 1u << cc.bits.iosqes);
		ctrlr->vcprop.cc.bits.iosqes = cc.bits.iosqes; /* [한국어] 새 IOSQES 저장 */
		diff.bits.iosqes = 0; /* [한국어] 처리 완료 */
	}

	if (diff.bits.iocqes) {
		/* [한국어] IOCQES(I/O CQ Entry Size) 변경: log2(CQ entry size). 16B이면 4 */
		SPDK_DEBUGLOG(nvmf, "Prop Set IOCQES = %u (%u bytes)\n",
			      cc.bits.iocqes, 1u << cc.bits.iocqes);
		ctrlr->vcprop.cc.bits.iocqes = cc.bits.iocqes; /* [한국어] 새 IOCQES 저장 */
		diff.bits.iocqes = 0; /* [한국어] 처리 완료 */
	}

	if (diff.bits.ams) {
		/* [한국어] AMS(Arbitration Mechanism Selected) 변경 시도: NVMe-oF는 RR(00b)만 지원 → 거절 */
		SPDK_ERRLOG("Arbitration Mechanism Selected (AMS) 0x%x not supported!\n", cc.bits.ams);
		return false;
	}

	if (diff.bits.mps) {
		/* [한국어] MPS(Memory Page Size) 변경 시도: NVMe-oF는 4KB(00b)만 지원 → 거절 */
		SPDK_ERRLOG("Memory Page Size (MPS) %u KiB not supported!\n", (1 << (2 + cc.bits.mps)));
		return false;
	}

	if (diff.bits.css) {
		/* [한국어] CSS(I/O Command Set Selected) 변경: NVM(000b)~IOCS(111b) 범위만 허용 */
		if (cc.bits.css > SPDK_NVME_CC_CSS_IOCS) {
			/* [한국어] 스펙에 없는 CSS 값 → 거절 */
			SPDK_ERRLOG("I/O Command Set Selected (CSS) 0x%x not supported!\n", cc.bits.css);
			return false;
		}
		diff.bits.css = 0; /* [한국어] CSS 처리 완료 (현재는 vcprop.cc에 저장하지 않음 - 초기화 시 이미 설정) */
	}

	if (diff.bits.crime) {
		/* [한국어] CRIME(Controller Ready Independent of Media Enable): NVMe 2.0 RO 비트.
		 * 호스트가 쓰려 하면 경고만 출력하고 무시 */
		SPDK_WARNLOG("Property Set for read only property CC.CRIME\n");
		diff.bits.crime = 0; /* [한국어] 처리 완료 (실제 변경 없음) */
	}

	if (diff.raw != 0) {
		/* Print an error message, but don't fail the command in this case.
		 * If we did want to fail in this case, we'd need to ensure we acted
		 * on no other bits or the initiator gets confused. */
		/* [한국어] 위에서 처리하지 않은 예약 비트가 바뀐 경우: 오류 로그만 찍고 성공 반환
		 * (실패하면 호스트가 혼란에 빠질 수 있어 관대하게 처리) */
		SPDK_ERRLOG("Prop Set CC toggled reserved bits 0x%x!\n", diff.raw);
	}

	return true; /* [한국어] CC 레지스터 변경 성공 */
}

/*
 * [한국어]
 * nvmf_prop_set_nssr - NSSR(NVM Subsystem Reset) 레지스터 쓰기 처리
 *
 * @ctrlr: Property Set을 수신한 컨트롤러.
 * @value: 호스트가 쓴 32비트 값.
 * @return: true이면 처리됨, false이면 CAP.NSSRS=0이라 지원 안 됨.
 *
 * NVMe 스펙 §3.3.4: NSSR 오프셋에 0x4E564D65("NVMe")를 쓰면 NVM Subsystem Reset
 * 이 발동된다. 다른 값은 효과 없다.
 * 리셋 동작:
 *  1) 모든 NS의 bdev에 spdk_bdev_nvme_nssr() 또는 spdk_bdev_reset() 발동.
 *  2) subsystem의 모든 ctrlr에 CC.EN=0을 적용하여 IO disconnect 시작.
 *  3) 각 ctrlr에 executing_nssr=true 설정 → 완료 시 CSTS.NSSRO=1 알림.
 *
 * 실행 컨텍스트: ctrlr->thread (nvmf_property_set 내부).
 *
 * 호출 체인:
 *   nvmf_property_set → find_prop(ofst=NSSR) → nvmf_prop_set_nssr
 *     → spdk_bdev_nvme_nssr / spdk_bdev_reset
 *     → nvmf_prop_set_cc(EN=0) [각 ctrlr에]
 */
static bool
nvmf_prop_set_nssr(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	struct spdk_nvmf_poll_group *group; /* [한국어] admin qpair의 poll group (bdev channel 보유) */
	struct spdk_nvmf_ns *ns; /* [한국어] 순회 중인 네임스페이스 */
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info; /* [한국어] NS별 bdev channel */
	struct spdk_nvmf_ctrlr *ctrlr_iter; /* [한국어] subsystem 내 모든 ctrlr 순회용 */
	union spdk_nvme_cc_register cc_new; /* [한국어] EN=0으로 설정할 임시 CC 값 */
	int rc = 0; /* [한국어] bdev_nvme_nssr 반환값 */

	if (ctrlr->vcprop.cap.bits.nssrs != 1) {
		/* [한국어] CAP.NSSRS=0: NVM Subsystem Reset 미지원 컨트롤러 */
		return false;
	}

	/* A write of the value 4E564D65h ("NVMe")
	 * to this field initiates an NVM Subsystem Reset.
	 * A write of any other value has no
	 * functional effect on the operation of the NVM subsystem. */
	if (value != SPDK_NVME_NSSR_VALUE) {
		/* [한국어] "NVMe"(0x4E564D65) 이외의 값은 무시 */
		return true;
	}

	group = ctrlr->admin_qpair->group; /* [한국어] bdev channel 접근을 위해 poll group 필요 */
	assert(group != NULL && group->sgroups != NULL);

	for (ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(ctrlr->subsys, ns)) {
		/* [한국어] 모든 bdev NS에 NSSR 또는 대체 리셋 발동 */
		if (ns->bdev == NULL) {
			continue; /* [한국어] bdev 없는 NS 건너뜀 */
		}

		ns_info = &group->sgroups[ctrlr->subsys->id].ns_info[ns->opts.nsid - 1]; /* [한국어] nsid(1-based) → 0-based 인덱스 */

		SPDK_DEBUGLOG(nvmf, "Ctrlr %p setting NSSR to NSID %u\n", ctrlr, ns->opts.nsid);
		rc = spdk_bdev_nvme_nssr(ns->desc, ns_info->channel, nvmf_bdev_complete_reset, NULL); /* [한국어] bdev가 NVMe NSSR 명령 지원 여부 확인 + 발동 */
		if (rc == -ENOTSUP) {
			/* [한국어] NVMe NSSR 미지원 bdev는 일반 reset으로 대체 */
			SPDK_DEBUGLOG(nvmf, "Ctrlr %p NSSR not supported, performing reset\n", ctrlr);
			spdk_bdev_reset(ns->desc, ns_info->channel, nvmf_bdev_complete_reset, NULL);
		}
	}

	TAILQ_FOREACH(ctrlr_iter, &ctrlr->subsys->ctrlrs, link) {
		/* [한국어] subsystem의 모든 컨트롤러에 CC.EN=0 적용하여 IO qpair disconnect 시작 */
		cc_new = ctrlr_iter->vcprop.cc; /* [한국어] 현재 CC 값 복사 */
		cc_new.bits.en = 0; /* [한국어] EN 비트 클리어 */
		ctrlr_iter->executing_nssr = true; /* [한국어] 완료 시 CSTS.NSSRO=1 설정하도록 표시 */
		nvmf_prop_set_cc(ctrlr_iter, cc_new.raw); /* [한국어] CC 쓰기 처리 → disconnect 시작 */
	}

	return true;
}

/*
 * [한국어]
 * nvmf_prop_set_csts - CSTS(Controller Status) 레지스터 쓰기 처리
 *
 * @ctrlr: 대상 컨트롤러.
 * @value: 호스트가 쓴 32비트 값.
 * @return: 항상 true.
 *
 * CSTS는 대부분 읽기 전용(RO)이나 NSSRO 비트만 RWC(Write ‘1’ to Clear)이다.
 * 호스트가 NSSRO=1을 쓰면 target은 CSTS.NSSRO를 0으로 클리어한다.
 * NVMe 스펙 §3.1.7 참조.
 *
 * 실행 컨텍스트: ctrlr->thread (nvmf_property_set 내부).
 */
static bool
nvmf_prop_set_csts(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	union spdk_nvme_csts_register csts; /* [한국어] 호스트가 쓴 값을 CSTS 구조체로 해석 */
	csts.raw = value;

	SPDK_DEBUGLOG(nvmf, "cur CSTS: 0x%08x\n", ctrlr->vcprop.csts.raw);
	SPDK_DEBUGLOG(nvmf, "new CSTS: 0x%08x\n", csts.raw);

	/* only NSSRO bit is RWC (Read/Write ‘1’ to clear),
	 * rest of CSTS is RO, so ignore it */
	if (csts.bits.nssro == 1) {
		/* [한국어] NSSRO=1 쓰기 → 비트 클리어 (NVM Subsystem Reset Occurred 인지 처리) */
		ctrlr->vcprop.csts.bits.nssro = 0;
	}

	return true;
}

/*
 * [한국어]
 * nvmf_prop_get_csts - CSTS(Controller Status) 레지스터 읽기
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 32비트 CSTS 레지스터 원시값.
 *
 * CSTS.RDY(=1이면 CC.EN=1 후 준비 완료), CSTS.CFS(Fatal Status), CSTS.NSSRO,
 * CSTS.SHST(Shutdown Status) 등을 포함한다.
 * Property Get 오프셋 0x001C에 대응한다.
 */
static uint64_t
nvmf_prop_get_csts(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.csts.raw; /* [한국어] CSTS 레지스터 32비트 원시값 반환 */
}

/*
 * [한국어]
 * nvmf_prop_get_aqa - AQA(Admin Queue Attributes) 레지스터 읽기
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 32비트 AQA 레지스터 원시값.
 *
 * AQA는 Admin SQ 크기(ASQS, 0-based)와 Admin CQ 크기(ACQS, 0-based)를 담는다.
 * Property Get 오프셋 0x0024에 대응한다.
 */
static uint64_t
nvmf_prop_get_aqa(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.aqa.raw; /* [한국어] AQA 레지스터 32비트 원시값 반환 */
}

/*
 * [한국어]
 * nvmf_prop_set_aqa - AQA(Admin Queue Attributes) 레지스터 쓰기 처리
 *
 * @ctrlr: 대상 컨트롤러.
 * @value: 호스트가 쓴 32비트 값.
 * @return: true이면 유효한 값, false이면 검증 실패.
 *
 * 호스트가 CC.EN=1 전에 admin queue 크기를 지정하는 레지스터.
 * ASQS, ACQS는 0-based이므로 최소값은 min_entries - 1이다.
 * Reserved 비트가 0이 아니면 거절한다.
 * Property Set 오프셋 0x0024에 대응한다.
 */
static bool
nvmf_prop_set_aqa(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	union spdk_nvme_aqa_register aqa; /* [한국어] 호스트가 쓴 값을 AQA 구조체로 해석 */

	aqa.raw = value;

	/*
	 * We don’t need to explicitly check for maximum size, as the fields are
	 * limited to 12 bits (4096).
	 */
	/* [한국어] ASQS/ACQS 최소값(0-based, 최소 2개 - 1 = 1), reserved 비트 검증 */
	if (aqa.bits.asqs < SPDK_NVME_ADMIN_QUEUE_MIN_ENTRIES - 1 ||
	    aqa.bits.acqs < SPDK_NVME_ADMIN_QUEUE_MIN_ENTRIES - 1 ||
	    aqa.bits.reserved1 != 0 || aqa.bits.reserved2 != 0) {
		return false; /* [한국어] 검증 실패 → INVALID_PARAM 응답 */
	}

	ctrlr->vcprop.aqa.raw = value; /* [한국어] AQA 저장 */

	return true;
}

/*
 * [한국어]
 * nvmf_prop_get_asq - ASQ(Admin Submission Queue Base Address) 레지스터 읽기
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 64비트 ASQ 레지스터 값 (Admin SQ 물리 기저주소).
 *
 * NVMe-oF에서 ASQ는 호스트가 쓴 값을 반영하기 위해 존재하나
 * 실제 메모리 매핑에는 사용되지 않는다.
 * Property Get 오프셋 0x0028에 대응한다.
 */
static uint64_t
nvmf_prop_get_asq(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.asq; /* [한국어] ASQ 64비트 레지스터 값 반환 */
}

/*
 * [한국어]
 * nvmf_prop_set_asq_lower - ASQ 레지스터 하위 32비트 쓰기
 *
 * @ctrlr: 대상 컨트롤러.
 * @value: 하위 32비트 값.
 * @return: 항상 true.
 *
 * 64비트 ASQ를 두 번의 32비트 Property Set으로 쓰는 경우의 하위 워드 처리.
 * 상위 32비트는 보존하고 하위 32비트만 교체한다.
 */
static bool
nvmf_prop_set_asq_lower(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	/* [한국어] 상위 32비트 마스크 보존 + 하위 32비트 교체 */
	ctrlr->vcprop.asq = (ctrlr->vcprop.asq & (0xFFFFFFFFULL << 32ULL)) | value;

	return true;
}

/*
 * [한국어]
 * nvmf_prop_set_asq_upper - ASQ 레지스터 상위 32비트 쓰기
 *
 * @ctrlr: 대상 컨트롤러.
 * @value: 상위 32비트 값.
 * @return: 항상 true.
 *
 * 64비트 ASQ를 두 번의 32비트 Property Set으로 쓰는 경우의 상위 워드 처리.
 * 하위 32비트는 보존하고 상위 32비트만 교체한다.
 */
static bool
nvmf_prop_set_asq_upper(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	/* [한국어] 하위 32비트 마스크 보존 + 상위 32비트 교체 */
	ctrlr->vcprop.asq = (ctrlr->vcprop.asq & 0xFFFFFFFFULL) | ((uint64_t)value << 32ULL);

	return true;
}

/*
 * [한국어]
 * nvmf_prop_get_acq - ACQ(Admin Completion Queue Base Address) 레지스터 읽기
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 64비트 ACQ 레지스터 값 (Admin CQ 물리 기저주소).
 *
 * Property Get 오프셋 0x0030에 대응한다.
 */
static uint64_t
nvmf_prop_get_acq(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.acq; /* [한국어] ACQ 64비트 레지스터 값 반환 */
}

/*
 * [한국어]
 * nvmf_prop_set_acq_lower - ACQ 레지스터 하위 32비트 쓰기
 *
 * @ctrlr: 대상 컨트롤러.
 * @value: 하위 32비트 값.
 * @return: 항상 true.
 */
static bool
nvmf_prop_set_acq_lower(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	/* [한국어] 상위 32비트 보존 + 하위 32비트 교체 */
	ctrlr->vcprop.acq = (ctrlr->vcprop.acq & (0xFFFFFFFFULL << 32ULL)) | value;

	return true;
}

/*
 * [한국어]
 * nvmf_prop_set_acq_upper - ACQ 레지스터 상위 32비트 쓰기
 *
 * @ctrlr: 대상 컨트롤러.
 * @value: 상위 32비트 값.
 * @return: 항상 true.
 */
static bool
nvmf_prop_set_acq_upper(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	/* [한국어] 하위 32비트 보존 + 상위 32비트 교체 */
	ctrlr->vcprop.acq = (ctrlr->vcprop.acq & 0xFFFFFFFFULL) | ((uint64_t)value << 32ULL);

	return true;
}

/*
 * [한국어]
 * nvmf_prop_get_crto - CRTO(Controller Ready Timeouts) 레지스터 읽기
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 32비트 CRTO 레지스터 원시값.
 *
 * CRTO는 NVMe 2.0에서 추가된 레지스터로 CRWMS(Controller Ready With Media Supported)와
 * CRIMS(Controller Ready Independent of Media Supported) 필드를 포함한다.
 * 각각 CC.EN=1 후 CSTS.RDY=1이 되기까지의 최대 대기 시간(500ms 단위)을 나타낸다.
 * Property Get 오프셋 0x0068에 대응한다.
 */
static uint64_t
nvmf_prop_get_crto(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.crto.raw; /* [한국어] CRTO 레지스터 32비트 원시값 반환 */
}

/* [한국어] NVMe-oF Property Get/Set Capsule이 다루는 가상 NVMe 컨트롤러 레지스터의
 * 메타테이블 항목. 호스트는 PCIe NVMe라면 BAR0의 컨트롤러 레지스터를 MMIO로
 * 읽고/쓰지만, NVMe-oF는 transport 캡슐(Property Get/Set, fctype=04h/00h)로 대신
 * 한다. SPDK는 진짜 BAR가 없으므로 ctrlr->vcprop에 가상 레지스터 값을 두고
 * 본 테이블의 get_cb/set_cb를 통해 호스트 요청에 응답한다. */
struct nvmf_prop {
	uint32_t ofst;
	/* [한국어] spdk_nvme_registers 구조체 안에서의 byte offset.
	 * 호스트가 보낸 cmd->ofst와 일치하는 항목을 find_prop()로 찾는다. */

	uint8_t size;
	/* [한국어] 레지스터 폭(바이트). 4 또는 8. CAP/ASQ/ACQ는 8, 그 외는 4. */

	char name[11];
	/* [한국어] 디버그 로그 출력용 짧은 이름 ("cap", "vs", "cc", "csts", ...). */

	uint64_t (*get_cb)(struct spdk_nvmf_ctrlr *ctrlr);
	/* [한국어] Property Get 처리. 호스트가 SET만 한 NSSR 같은 경우 NULL이 가능하며,
	 * 그러면 find_prop가 매치하지 못한 것처럼 INVALID_PARAM으로 응답된다. */

	bool (*set_cb)(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value);
	/* [한국어] Property Set 처리 (8B의 경우 하위 4B 또는 4B 전체). NULL이면 RO.
	 * true 반환 = 성공, false = INVALID_PARAM 응답 유발. */

	bool (*set_upper_cb)(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value);
	/* [한국어] 8B 레지스터(ASQ/ACQ)의 상위 4B 쓰기. 4B 레지스터에서는 NULL. */
};

/* [한국어] PROP 매크로: nvmf_prop 항목을 spec 레지스터의 field 이름과 size로
 * 한 줄에 선언한다. offsetof로 spdk_nvme_registers 안에서의 위치를 자동 추출. */
#define PROP(field, size, get_cb, set_cb, set_upper_cb) \
	{ \
		offsetof(struct spdk_nvme_registers, field), \
		size, \
		#field, \
		get_cb, set_cb, set_upper_cb \
	}

/* [한국어] NVMe-oF가 지원하는 가상 컨트롤러 레지스터 목록.
 * - cap (RO): Controller Capabilities. mqes, to, dstrd, css, mpsmin/max 등.
 * - vs (RO): Version. SPDK는 NVMe 2.0 보고.
 * - cc (RW): Controller Configuration. EN/SHN/IOSQES/IOCQES/CSS/MPS/AMS.
 * - csts (R/W1C): Controller Status. RDY/CFS/SHST/NSSRO 비트.
 * - nssr (WO): NVM Subsystem Reset. 4E564D65h ("NVMe") 쓰면 reset 트리거.
 * - aqa (RW): Admin Queue Attributes. ASQS/ACQS.
 * - asq (RW, 8B): Admin SQ base address (NVMe-oF에서는 의미 약함).
 * - acq (RW, 8B): Admin CQ base address.
 * - crto (RO): Controller Ready Timeouts. CRWMT/CRIMT.
 */
static const struct nvmf_prop nvmf_props[] = {
	PROP(cap,  8, nvmf_prop_get_cap,  NULL,                    NULL),
	PROP(vs,   4, nvmf_prop_get_vs,   NULL,                    NULL),
	PROP(cc,   4, nvmf_prop_get_cc,   nvmf_prop_set_cc,        NULL),
	PROP(csts, 4, nvmf_prop_get_csts, nvmf_prop_set_csts,      NULL),
	PROP(nssr, 4, nvmf_prop_get_nssr, nvmf_prop_set_nssr,      NULL),
	PROP(aqa,  4, nvmf_prop_get_aqa,  nvmf_prop_set_aqa,       NULL),
	PROP(asq,  8, nvmf_prop_get_asq,  nvmf_prop_set_asq_lower, nvmf_prop_set_asq_upper),
	PROP(acq,  8, nvmf_prop_get_acq,  nvmf_prop_set_acq_lower, nvmf_prop_set_acq_upper),
	PROP(crto, 4, nvmf_prop_get_crto, NULL,                    NULL)
};

/*
 * [한국어]
 * find_prop - cmd->ofst/size에 매치되는 nvmf_prop 항목을 룩업한다
 *
 * @ofst: 호스트가 보낸 byte offset.
 * @size: 호스트가 요청한 폭 (4 또는 8).
 * @return: 매치 항목 포인터 또는 NULL.
 *
 * 8B 레지스터(CAP/ASQ/ACQ)는 4B 단위로 나눠 읽기/쓰기될 수 있으므로
 * "[ofst, ofst+size) ⊆ [prop->ofst, prop->ofst+prop->size)" 포함 관계로 매치한다.
 */
static const struct nvmf_prop *
find_prop(uint32_t ofst, uint8_t size)
{
	size_t i; /* [한국어] nvmf_props[] 순회 인덱스 */

	for (i = 0; i < SPDK_COUNTOF(nvmf_props); i++) { /* [한국어] 테이블 전체 선형 탐색 (항목 수 적으므로 충분) */
		const struct nvmf_prop *prop = &nvmf_props[i]; /* [한국어] 현재 검사 중인 레지스터 항목 */

		if ((ofst >= prop->ofst) && (ofst + size <= prop->ofst + prop->size)) {
			/* [한국어] 포함 관계 매치: 요청 범위 [ofst, ofst+size)가 레지스터 범위 안에 있음.
			 * 8B 레지스터의 하위 4B(ofst==prop->ofst) 또는 상위 4B(ofst==prop->ofst+4)도 허용 */
			return prop;
		}
	}

	return NULL; /* [한국어] 매치 없음: 호출자가 INVALID_PARAM 응답 처리 */
}

/*
 * [한국어]
 * nvmf_property_get - Fabric Property Get capsule(fctype=04h) 처리
 *
 * @req: 요청 객체. cmd->prop_get_cmd, rsp->prop_get_rsp 사용.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 호스트가 가상 컨트롤러 레지스터를 읽기 위해 보낸 capsule. PCIe NVMe라면
 * BAR0 MMIO read에 해당. cmd->attrib.size로 폭(4 또는 8B)을 받아 find_prop()로
 * 매핑 후 get_cb() 호출. 8B 레지스터를 4B로 부분 읽기하는 경우 상하위 절반을
 * 마스크/시프트로 추출하여 응답한다.
 *
 * 호출 컨텍스트: nvmf_ctrlr_process_fabrics_cmd()에서 호출 → ctrlr->thread.
 */
static int
nvmf_property_get(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] admin qpair에 결합된 컨트롤러 (vcprop 가상 레지스터 저장소) */
	struct spdk_nvmf_fabric_prop_get_cmd *cmd = &req->cmd->prop_get_cmd; /* [한국어] Property Get Capsule의 SQE (attrib.size, ofst 포함) */
	struct spdk_nvmf_fabric_prop_get_rsp *response = &req->rsp->prop_get_rsp; /* [한국어] Property Get 응답 (value.u64 반환) */
	const struct nvmf_prop *prop; /* [한국어] nvmf_props[] 테이블에서 찾은 레지스터 매핑 항목 */
	uint8_t size; /* [한국어] 읽기 폭 바이트 수 (4 또는 8) */

	response->status.sc = 0; /* [한국어] 기본 성공 상태로 초기화 (오류 경로에서 덮어씀) */
	response->value.u64 = 0; /* [한국어] 읽기 값 초기화 (get_cb()가 실제 값을 채움) */

	SPDK_DEBUGLOG(nvmf, "size %d, offset 0x%x\n",
		      cmd->attrib.size, cmd->ofst);

	switch (cmd->attrib.size) {
	case SPDK_NVMF_PROP_SIZE_4:
		size = 4; /* [한국어] 4바이트 폭 읽기: VS, CC, CSTS 등 32-bit 레지스터 */
		break;
	case SPDK_NVMF_PROP_SIZE_8:
		size = 8; /* [한국어] 8바이트 폭 읽기: CAP, ASQ, ACQ 등 64-bit 레지스터 */
		break;
	default:
		SPDK_DEBUGLOG(nvmf, "Invalid size value %d\n", cmd->attrib.size);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC; /* [한국어] 잘못된 attrib.size → Command Specific 오류 */
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM; /* [한국어] Fabric SC: Invalid Parameter */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	prop = find_prop(cmd->ofst, size); /* [한국어] nvmf_props[] 테이블에서 offset과 size로 해당 레지스터 항목 검색 */
	if (prop == NULL || prop->get_cb == NULL) {
		/* [한국어] 알 수 없는 레지스터 오프셋이거나 읽기 불가 레지스터 → INVALID_PARAM */
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	SPDK_DEBUGLOG(nvmf, "name: %s\n", prop->name);

	response->value.u64 = prop->get_cb(ctrlr); /* [한국어] 레지스터별 get 콜백 호출: CAP이면 nvmf_prop_get_cap(), CC이면 nvmf_prop_get_cc() 등 */

	if (size != prop->size) {
		/* The size must be 4 and the prop->size is 8. Figure out which part of the property to read. */
		/* [한국어] 8B 레지스터를 4B로 부분 읽기하는 경우 (NVMe spec 허용): 하위/상위 절반 추출 */
		assert(size == 4); /* [한국어] 이 경로는 반드시 4B 요청 */
		assert(prop->size == 8); /* [한국어] 실제 레지스터는 8B (예: ASQ/ACQ/CAP) */

		if (cmd->ofst == prop->ofst) {
			/* Keep bottom 4 bytes only */
			/* [한국어] 요청 offset이 레지스터 시작과 일치 → 하위 32비트 반환 */
			response->value.u64 &= 0xFFFFFFFF;
		} else {
			/* Keep top 4 bytes only */
			/* [한국어] 요청 offset이 레지스터+4 → 상위 32비트를 하위로 시프트하여 반환 */
			response->value.u64 >>= 32;
		}
	}

	SPDK_DEBUGLOG(nvmf, "response value: 0x%" PRIx64 "\n", response->value.u64);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE; /* [한국어] 동기 완료: 호출자가 즉시 응답 캡슐 전송 */
}

/*
 * [한국어]
 * nvmf_property_set - Fabric Property Set capsule(fctype=00h) 처리
 *
 * @req: 요청 객체. cmd->prop_set_cmd, rsp->nvme_cpl 사용.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 호스트가 가상 컨트롤러 레지스터에 값을 쓰기 위한 capsule. PCIe NVMe라면
 * BAR0 MMIO write에 해당. find_prop()로 항목을 찾고 set_cb 또는
 * set_upper_cb를 호출. 8B 레지스터를 4B로 부분 쓰기하면 하위/상위 절반에
 * 맞는 콜백을 분기 호출한다. set_cb가 false 반환 시 INVALID_PARAM 응답.
 *
 * 가장 흔한 변경은 CC 레지스터 (호스트 enable/shutdown 시퀀스).
 */
static int
nvmf_property_set(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] admin qpair에 결합된 컨트롤러 (vcprop 가상 레지스터 소유자) */
	struct spdk_nvmf_fabric_prop_set_cmd *cmd = &req->cmd->prop_set_cmd; /* [한국어] Property Set Capsule SQE (attrib.size, ofst, value.u64 포함) */
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl; /* [한국어] 일반 NVMe CQE 응답 (Property Set은 data 없이 status만 반환) */
	const struct nvmf_prop *prop; /* [한국어] nvmf_props[] 테이블에서 찾은 레지스터 항목 */
	uint64_t value; /* [한국어] 쓰기 요청 값 (u64, 4B 요청이면 상위 절반 무시) */
	uint8_t size; /* [한국어] 쓰기 폭 바이트 수 (4 또는 8) */
	bool ret; /* [한국어] set_cb/set_upper_cb 콜백 성공 여부 */

	SPDK_DEBUGLOG(nvmf, "size %d, offset 0x%x, value 0x%" PRIx64 "\n",
		      cmd->attrib.size, cmd->ofst, cmd->value.u64);

	switch (cmd->attrib.size) {
	case SPDK_NVMF_PROP_SIZE_4:
		size = 4; /* [한국어] 4바이트 쓰기 요청: CC, CSTS, AQA 등 32-bit 레지스터 또는 64-bit 레지스터 하위/상위 절반 */
		break;
	case SPDK_NVMF_PROP_SIZE_8:
		size = 8; /* [한국어] 8바이트 쓰기 요청: ASQ/ACQ 같은 64-bit 기저 주소 레지스터 */
		break;
	default:
		SPDK_DEBUGLOG(nvmf, "Invalid size value %d\n", cmd->attrib.size);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC; /* [한국어] 잘못된 attrib.size → Command Specific 오류 */
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM; /* [한국어] Fabric SC: Invalid Parameter */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	prop = find_prop(cmd->ofst, size); /* [한국어] nvmf_props[] 테이블에서 offset+size 매칭 레지스터 항목 검색 */
	if (prop == NULL || prop->set_cb == NULL) {
		/* [한국어] 알 수 없는 offset이거나 read-only(set_cb=NULL) 레지스터 → 거절 */
		SPDK_INFOLOG(nvmf, "Invalid offset 0x%x\n", cmd->ofst);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	SPDK_DEBUGLOG(nvmf, "name: %s\n", prop->name);

	value = cmd->value.u64; /* [한국어] 호스트가 보낸 raw 64-bit 쓰기 값 추출 */

	if (prop->size == 4) {
		/* [한국어] 4B 레지스터(CC, CSTS, AQA, VS): 하위 32비트만 전달 */
		ret = prop->set_cb(ctrlr, (uint32_t)value);
	} else if (size != prop->size) {
		/* The size must be 4 and the prop->size is 8. Figure out which part of the property to write. */
		/* [한국어] 8B 레지스터를 4B씩 분할 쓰기: ASQ(64B)를 2번의 4B Property Set으로 쓰는 경우 */
		assert(size == 4); /* [한국어] 요청이 4B 폭임 (8B 레지스터의 절반을 씀) */
		assert(prop->size == 8); /* [한국어] 대상 레지스터는 8B (ASQ, ACQ 등) */

		if (cmd->ofst == prop->ofst) {
			/* [한국어] 요청 offset == 레지스터 시작 → 하위 32비트 쓰기 */
			ret = prop->set_cb(ctrlr, (uint32_t)value);
		} else {
			/* [한국어] 요청 offset == 레지스터+4 → 상위 32비트 쓰기 (set_upper_cb 사용) */
			ret = prop->set_upper_cb(ctrlr, (uint32_t)value);
		}
	} else {
		/* [한국어] 8B 레지스터를 8B 한 번에 쓰기: 하위 → 상위 순으로 두 콜백 순차 호출 */
		ret = prop->set_cb(ctrlr, (uint32_t)value); /* [한국어] 하위 32비트 먼저 적용 */
		if (ret) {
			ret = prop->set_upper_cb(ctrlr, (uint32_t)(value >> 32)); /* [한국어] 하위가 성공한 경우에만 상위 32비트 적용 */
		}
	}

	if (!ret) {
		/* [한국어] set_cb가 false 반환 = 유효하지 않은 값(예: reserved 비트 오염, 잘못된 상태 전이) */
		SPDK_ERRLOG("prop set_cb failed\n");
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_set_features_arbitration - Set Features: Arbitration (Feature ID 01h)
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 중재 메커니즘(RR, 우선순위 가중치 등)을 CDW11 값으로 설정한다.
 * NVMe-oF는 실제로 중재를 구현하지 않지만 Feature 값을 저장하여
 * Get Features 응답에 반환할 수 있도록 한다.
 */
static int
nvmf_ctrlr_set_features_arbitration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */

	SPDK_DEBUGLOG(nvmf, "Set Features - Arbitration (cdw11 = 0x%0x)\n", cmd->cdw11);

	ctrlr->feat.arbitration.raw = cmd->cdw11; /* [한국어] CDW11 전체를 arbitration 필드에 저장 */
	ctrlr->feat.arbitration.bits.reserved = 0; /* [한국어] reserved 비트는 항상 0으로 클리어 */

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_set_features_power_management - Set Features: Power Management (Feature ID 02h)
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 전원 상태(PS)를 설정한다. SPDK는 NPSS=0(power state 0만 지원)으로 광고하므로
 * PS≠0 요청은 INVALID_FIELD 오류로 거절한다.
 */
static int
nvmf_ctrlr_set_features_power_management(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */

	SPDK_DEBUGLOG(nvmf, "Set Features - Power Management (cdw11 = 0x%0x)\n", cmd->cdw11);

	/* Only PS = 0 is allowed, since we report NPSS = 0 */
	if (cmd->cdw11_bits.feat_power_management.bits.ps != 0) {
		/* [한국어] NPSS=0으로 광고했으므로 PS=0만 허용 */
		SPDK_ERRLOG("Invalid power state %u\n", cmd->cdw11_bits.feat_power_management.bits.ps);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr->feat.power_management.raw = cmd->cdw11; /* [한국어] PS 값 저장 */
	ctrlr->feat.power_management.bits.reserved = 0; /* [한국어] reserved 클리어 */

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * temp_threshold_opts_valid - Temperature Threshold Feature의 CDW11 옵션 유효성 검사
 *
 * @opts: CDW11에서 추출한 temperature threshold 옵션 구조체 포인터.
 * @return: true이면 유효, false이면 reserved/잘못된 값.
 *
 * NVMe 스펙 §5.27.1에 따라 TMPSEL(온도 센서 선택)과 THSEL(임계값 유형) 필드를 검증한다.
 * TMPSEL 9-14는 reserved이고, THSEL 2-3도 reserved이다.
 */
static bool
temp_threshold_opts_valid(const union spdk_nvme_feat_temperature_threshold *opts)
{
	/*
	 * Valid TMPSEL values:
	 *  0000b - 1000b: temperature sensors
	 *  1111b: set all implemented temperature sensors
	 */
	if (opts->bits.tmpsel >= 9 && opts->bits.tmpsel != 15) {
		/* 1001b - 1110b: reserved */
		/* [한국어] TMPSEL 9~14는 reserved 값 → 거절 */
		SPDK_ERRLOG("Invalid TMPSEL %u\n", opts->bits.tmpsel);
		return false;
	}

	/*
	 * Valid THSEL values:
	 *  00b: over temperature threshold
	 *  01b: under temperature threshold
	 */
	if (opts->bits.thsel > 1) {
		/* 10b - 11b: reserved */
		/* [한국어] THSEL 2, 3은 reserved 값 → 거절 */
		SPDK_ERRLOG("Invalid THSEL %u\n", opts->bits.thsel);
		return false;
	}

	return true;
}

/*
 * [한국어]
 * nvmf_ctrlr_set_features_temperature_threshold - Set Features: Temperature Threshold (Feature ID 04h)
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 온도 임계값을 설정한다. SPDK는 실제 온도 센서를 구현하지 않으므로
 * 옵션 검증만 하고 값을 저장하지 않는다.
 */
static int
nvmf_ctrlr_set_features_temperature_threshold(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */

	SPDK_DEBUGLOG(nvmf, "Set Features - Temperature Threshold (cdw11 = 0x%0x)\n", cmd->cdw11);

	if (!temp_threshold_opts_valid(&cmd->cdw11_bits.feat_temp_threshold)) {
		/* [한국어] TMPSEL/THSEL 검증 실패 */
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* TODO: no sensors implemented - ignore new values */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE; /* [한국어] 값은 무시 (센서 미구현) */
}

/*
 * [한국어]
 * nvmf_ctrlr_get_features_temperature_threshold - Get Features: Temperature Threshold (Feature ID 04h)
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 온도 임계값을 반환한다. SPDK는 센서가 없으므로 항상 0을 반환한다.
 */
static int
nvmf_ctrlr_get_features_temperature_threshold(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */

	SPDK_DEBUGLOG(nvmf, "Get Features - Temperature Threshold (cdw11 = 0x%0x)\n", cmd->cdw11);

	if (!temp_threshold_opts_valid(&cmd->cdw11_bits.feat_temp_threshold)) {
		/* [한국어] 잘못된 TMPSEL/THSEL 옵션 → 거절 */
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* TODO: no sensors implemented - return 0 for all thresholds */
	rsp->cdw0 = 0; /* [한국어] 임계값 0 반환 (센서 미구현) */

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_get_features_interrupt_vector_configuration - Get Features: Interrupt Vector Configuration (Feature ID 09h)
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * NVMe-oF는 MSI-X 인터럽트가 없으므로 Coalescing Disable(CD) 비트만 의미 있다.
 * CDW11의 IV 필드를 그대로 에코하고, CD는 저장된 값을 반환한다.
 */
static int
nvmf_ctrlr_get_features_interrupt_vector_configuration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */
	union spdk_nvme_feat_interrupt_vector_configuration iv_conf = {}; /* [한국어] 응답할 IV 설정값 */

	SPDK_DEBUGLOG(nvmf, "Get Features - Interrupt Vector Configuration (cdw11 = 0x%0x)\n", cmd->cdw11);

	iv_conf.bits.iv = cmd->cdw11_bits.feat_interrupt_vector_configuration.bits.iv; /* [한국어] 요청된 IV 번호 에코 */
	iv_conf.bits.cd = ctrlr->feat.interrupt_vector_configuration.bits.cd; /* [한국어] 저장된 Coalescing Disable 값 */
	rsp->cdw0 = iv_conf.raw; /* [한국어] 응답 CDW0에 IV 설정값 반환 */

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_set_features_error_recovery - Set Features: Error Recovery (Feature ID 05h)
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 오류 복구 타임아웃(TLER) 설정. DULBE(Deallocated or Unwritten Logical Block Error) 비트는
 * Identify Namespace에서 광고하지 않으므로 호스트가 set하면 INVALID_FIELD 오류.
 */
static int
nvmf_ctrlr_set_features_error_recovery(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */

	SPDK_DEBUGLOG(nvmf, "Set Features - Error Recovery (cdw11 = 0x%0x)\n", cmd->cdw11);

	if (cmd->cdw11_bits.feat_error_recovery.bits.dulbe) {
		/*
		 * Host is not allowed to set this bit, since we don't advertise it in
		 * Identify Namespace.
		 */
		/* [한국어] DULBE는 Identify Namespace에서 광고하지 않으므로 호스트가 set 불가 */
		SPDK_ERRLOG("Host set unsupported DULBE bit\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr->feat.error_recovery.raw = cmd->cdw11; /* [한국어] TLER 등 오류 복구 설정값 저장 */
	ctrlr->feat.error_recovery.bits.reserved = 0; /* [한국어] reserved 클리어 */

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_set_features_volatile_write_cache - Set Features: Volatile Write Cache (Feature ID 06h)
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 휘발성 쓰기 캐시(WCE) 활성화/비활성화를 설정한다.
 * 실제 bdev 캐시 정책 변경은 여기서 하지 않고 값만 저장한다.
 */
static int
nvmf_ctrlr_set_features_volatile_write_cache(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */

	SPDK_DEBUGLOG(nvmf, "Set Features - Volatile Write Cache (cdw11 = 0x%0x)\n", cmd->cdw11);

	ctrlr->feat.volatile_write_cache.raw = cmd->cdw11; /* [한국어] WCE 비트 포함 raw 값 저장 */
	ctrlr->feat.volatile_write_cache.bits.reserved = 0; /* [한국어] reserved 클리어 */

	SPDK_DEBUGLOG(nvmf, "Set Features - Volatile Write Cache %s\n",
		      ctrlr->feat.volatile_write_cache.bits.wce ? "Enabled" : "Disabled");
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_set_features_write_atomicity - Set Features: Write Atomicity Normal (Feature ID 0Ah)
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 쓰기 원자성 일반 수준(AWUN/AWUPF 기준) 설정. 값 저장 후 Get Features로 반환.
 */
static int
nvmf_ctrlr_set_features_write_atomicity(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */

	SPDK_DEBUGLOG(nvmf, "Set Features - Write Atomicity (cdw11 = 0x%0x)\n", cmd->cdw11);

	ctrlr->feat.write_atomicity.raw = cmd->cdw11; /* [한국어] Write Atomicity 설정값 저장 */
	ctrlr->feat.write_atomicity.bits.reserved = 0; /* [한국어] reserved 클리어 */

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_set_features_host_identifier - Set Features: Host Identifier (Feature ID 81h) — 거절
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE (오류 응답).
 *
 * NVMe-oF에서 Host Identifier는 Fabric Connect 데이터에서 이미 설정된다.
 * Set Features로의 변경은 COMMAND_SEQUENCE_ERROR(CSE)로 거절한다.
 */
static int
nvmf_ctrlr_set_features_host_identifier(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */

	SPDK_ERRLOG("Set Features - Host Identifier not allowed\n");
	response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR; /* [한국어] NVMe-oF에서 HID 변경 불가 */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_get_features_host_identifier - Get Features: Host Identifier (Feature ID 81h)
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 128비트(16B) Host Identifier를 데이터 버퍼에 복사해 반환한다.
 * NVMe-oF는 반드시 EXHID=1(Extended 128-bit Host ID)을 요구한다.
 * EXHID=0 요청은 INVALID_FIELD 오류로 거절한다.
 */
static int
nvmf_ctrlr_get_features_host_identifier(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 (hostid 보유) */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */
	struct spdk_iov_xfer ix; /* [한국어] iov 기반 데이터 복사 헬퍼 */

	SPDK_DEBUGLOG(nvmf, "Get Features - Host Identifier\n");

	if (!cmd->cdw11_bits.feat_host_identifier.bits.exhid) {
		/* NVMe over Fabrics requires EXHID=1 (128-bit/16-byte host ID) */
		/* [한국어] NVMe-oF에서는 EXHID=1(128비트 Host ID)만 허용. EXHID=0은 거절 */
		SPDK_ERRLOG("Get Features - Host Identifier with EXHID=0 not allowed\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->iovcnt < 1 || req->length < sizeof(ctrlr->hostid)) {
		/* [한국어] 데이터 버퍼가 없거나 너무 작으면 복사 불가 */
		SPDK_ERRLOG("Invalid data buffer for Get Features - Host Identifier\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt); /* [한국어] iov 전송 컨텍스트 초기화 */
	spdk_iov_xfer_from_buf(&ix, &ctrlr->hostid, sizeof(ctrlr->hostid)); /* [한국어] 16B hostid를 iov 버퍼에 복사 */

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_get_features_reservation_notification_mask(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_ns *ns;

	SPDK_DEBUGLOG(nvmf, "get Features - Reservation Notification Mask\n");

	if (cmd->nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		SPDK_ERRLOG("get Features - Invalid Namespace ID\n");
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = nvmf_ctrlr_get_ns(ctrlr, cmd->nsid);
	if (ns == NULL) {
		SPDK_ERRLOG("get Features - Invalid Namespace ID\n");
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	rsp->cdw0 = ns->mask;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_reservation_notification_mask(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_ns *ns;

	SPDK_DEBUGLOG(nvmf, "Set Features - Reservation Notification Mask\n");

	if (cmd->nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
		     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
			ns->mask = cmd->cdw11;
		}
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = nvmf_ctrlr_get_ns(ctrlr, cmd->nsid);
	if (ns == NULL) {
		SPDK_ERRLOG("Set Features - Invalid Namespace ID\n");
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	ns->mask = cmd->cdw11;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_get_features_reservation_persistence(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvmf_ns *ns;

	SPDK_DEBUGLOG(nvmf, "Get Features - Reservation Persistence\n");

	ns = nvmf_ctrlr_get_ns(ctrlr, cmd->nsid);
	/* NSID with SPDK_NVME_GLOBAL_NS_TAG (=0xffffffff) also included */
	if (ns == NULL) {
		SPDK_ERRLOG("Get Features - Invalid Namespace ID\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	response->cdw0 = ns->ptpl_activated;

	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_reservation_persistence(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvmf_ns *ns;
	bool ptpl;

	SPDK_DEBUGLOG(nvmf, "Set Features - Reservation Persistence\n");

	ns = nvmf_ctrlr_get_ns(ctrlr, cmd->nsid);
	ptpl = cmd->cdw11_bits.feat_rsv_persistence.bits.ptpl;

	if (cmd->nsid != SPDK_NVME_GLOBAL_NS_TAG && ns && nvmf_ns_is_ptpl_capable(ns)) {
		ns->ptpl_activated = ptpl;
	} else if (cmd->nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		for (ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); ns;
		     ns = spdk_nvmf_subsystem_get_next_ns(ctrlr->subsys, ns)) {
			if (nvmf_ns_is_ptpl_capable(ns)) {
				ns->ptpl_activated = ptpl;
			}
		}
	} else {
		SPDK_ERRLOG("Set Features - Invalid Namespace ID or Reservation Configuration\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* TODO: Feature not changeable for now */
	response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	response->status.sc = SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_get_features_host_behavior_support - Get Features: Host Behavior Support (Feature ID 16h)
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * ACRE(Asymmetric Command Retry Enable)와 LBAFEE(LBA Format Extension Enable)
 * 활성화 상태를 반환한다. 데이터 버퍼에 spdk_nvme_host_behavior 구조체를 복사한다.
 */
static int
nvmf_ctrlr_get_features_host_behavior_support(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */
	struct spdk_nvme_host_behavior host_behavior = {}; /* [한국어] 반환할 host behavior 데이터 (zero-init) */
	struct spdk_iov_xfer ix; /* [한국어] iov 기반 데이터 복사 헬퍼 */

	SPDK_DEBUGLOG(nvmf, "Get Features - Host Behavior Support\n");

	if (req->iovcnt < 1 || req->length < sizeof(struct spdk_nvme_host_behavior)) {
		/* [한국어] 데이터 버퍼가 없거나 구조체보다 작음 */
		SPDK_ERRLOG("invalid data buffer for Host Behavior Support\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	host_behavior.acre = ctrlr->acre_enabled; /* [한국어] ACRE 활성화 여부 (0 또는 1) */
	host_behavior.lbafee = ctrlr->lbafee_enabled; /* [한국어] LBAFEE 활성화 여부 (0 또는 1) */

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt); /* [한국어] iov 전송 컨텍스트 초기화 */
	spdk_iov_xfer_from_buf(&ix, &host_behavior, sizeof(host_behavior)); /* [한국어] host_behavior 구조체를 iov 버퍼에 복사 */

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_set_features_host_behavior_support - Set Features: Host Behavior Support (Feature ID 16h)
 *
 * @req: Admin 명령 요청 (iovcnt=1, iov에 spdk_nvme_host_behavior 구조체).
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * ACRE(Asymmetric Command Retry Enable)와 LBAFEE(LBA Format Extension Enable)
 * 활성화 여부를 설정한다. 값은 0 또는 1만 허용되며 다른 값은 INVALID_FIELD 오류.
 */
static int
nvmf_ctrlr_set_features_host_behavior_support(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */
	struct spdk_nvme_host_behavior *host_behavior; /* [한국어] iov 버퍼에서 직접 참조하는 구조체 */

	SPDK_DEBUGLOG(nvmf, "Set Features - Host Behavior Support\n");
	if (req->iovcnt != 1) {
		/* [한국어] 정확히 1개의 iov가 있어야 함 */
		SPDK_ERRLOG("Host Behavior Support invalid iovcnt: %d\n", req->iovcnt);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	if (req->iov[0].iov_len != sizeof(struct spdk_nvme_host_behavior)) {
		/* [한국어] iov 길이가 host_behavior 구조체 크기와 정확히 일치해야 함 */
		SPDK_ERRLOG("Host Behavior Support invalid iov_len: %zd\n", req->iov[0].iov_len);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	host_behavior = (struct spdk_nvme_host_behavior *)req->iov[0].iov_base; /* [한국어] iov에서 직접 구조체 참조 */
	if (host_behavior->acre == 0) {
		ctrlr->acre_enabled = false; /* [한국어] ACRE 비활성화 */
	} else if (host_behavior->acre == 1) {
		ctrlr->acre_enabled = true; /* [한국어] ACRE 활성화 */
	} else {
		/* [한국어] acre 필드에 0/1 외 값은 허용 안 됨 */
		SPDK_ERRLOG("Host Behavior Support invalid acre: 0x%02x\n", host_behavior->acre);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	if (host_behavior->lbafee == 0) {
		ctrlr->lbafee_enabled = false; /* [한국어] LBAFEE 비활성화 */
	} else if (host_behavior->lbafee == 1) {
		ctrlr->lbafee_enabled = true; /* [한국어] LBAFEE 활성화 */
	} else {
		/* [한국어] lbafee 필드에 0/1 외 값은 허용 안 됨 */
		SPDK_ERRLOG("Host Behavior Support invalid lbafee: 0x%02x\n", host_behavior->lbafee);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_set_features_keep_alive_timer - Set Features: Keep Alive Timer (Feature ID 0Fh)
 *
 * @req: Admin 명령 요청. CDW11=KATO 값 (ms 단위).
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * Keep Alive 타임아웃(KATO)을 갱신하고 keep_alive_poller를 재등록한다.
 * - KATO=0: 타임아웃 비활성화 → KEEP_ALIVE_INVALID 오류 (NVMe-oF에서 KA 필수).
 * - KATO ≤ min_kato: transport 최소값으로 클램프.
 * - 그 외: KAS 단위로 올림(round_up)하여 저장.
 * 값이 변경되면 기존 keep_alive_poller를 해제하고 새 KATO 주기로 재등록한다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 */
static int
nvmf_ctrlr_set_features_keep_alive_timer(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvmf_transport *transport = req->qpair->transport; /* [한국어] transport (min_kato, kas 보유) */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE (CDW11=KATO ms) */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */

	SPDK_DEBUGLOG(nvmf, "Set Features - Keep Alive Timer (%u ms)\n", cmd->cdw11);

	/*
	 * if attempts to disable keep alive by setting kato to 0h
	 * a status value of keep alive invalid shall be returned
	 */
	if (cmd->cdw11_bits.feat_keep_alive_timer.bits.kato == 0) {
		/* [한국어] KATO=0은 타임아웃 비활성화 시도 → NVMe-oF에서 허용 안 됨 */
		rsp->status.sc = SPDK_NVME_SC_KEEP_ALIVE_INVALID;
	} else if (cmd->cdw11_bits.feat_keep_alive_timer.bits.kato <= transport->opts.min_kato) {
		/* [한국어] 요청값이 transport 최소 KATO 이하이면 최소값으로 클램프 */
		ctrlr->feat.keep_alive_timer.bits.kato = transport->opts.min_kato;
	} else {
		/* round up to milliseconds */
		/* [한국어] KAS 단위(NVMF_KAS_TIME_UNIT_IN_MS=100ms)로 올림하여 저장
		 * KATO 타이머는 KAS granularity 배수여야 함 */
		ctrlr->feat.keep_alive_timer.bits.kato = spdk_round_up(
					cmd->cdw11_bits.feat_keep_alive_timer.bits.kato,
					transport->opts.kas * NVMF_KAS_TIME_UNIT_IN_MS);
	}

	/*
	 * if change the keep alive timeout value successfully
	 * update the keep alive poller.
	 */
	if (cmd->cdw11_bits.feat_keep_alive_timer.bits.kato != 0) {
		/* [한국어] KATO가 유효하면 기존 폴러 해제 후 새 주기로 재등록 */
		if (ctrlr->keep_alive_poller != NULL) {
			spdk_poller_unregister(&ctrlr->keep_alive_poller); /* [한국어] 기존 폴러 해제 */
		}
		ctrlr->keep_alive_poller = SPDK_POLLER_REGISTER(nvmf_ctrlr_keep_alive_poll, ctrlr,
					   ctrlr->feat.keep_alive_timer.bits.kato * 1000); /* [한국어] KATO ms → us 변환 후 등록 */
	}

	SPDK_DEBUGLOG(nvmf, "Set Features - Keep Alive Timer set to %u ms\n",
		      ctrlr->feat.keep_alive_timer.bits.kato);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_set_features_number_of_queues - Set Features: Number of Queues (Feature ID 07h)
 *
 * @req: Admin 명령 요청. CDW11=NSQR/NCQR (0-based 요청 큐 수).
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 호스트가 사용할 SQ/CQ 개수를 협상한다. SPDK는 pre-configured max_qpairs_allowed를
 * 항상 반환하고 호스트의 요청 값은 무시한다. IO qpair가 이미 활성화된 후에 이
 * 명령을 보내면 COMMAND_SEQUENCE_ERROR(CSE) 오류 (CC.EN=1 후 최초 1회만 허용).
 * NCQR=0xFFFF 또는 NSQR=0xFFFF는 invalid이다 (0-based 최대값은 65534).
 *
 * 실행 컨텍스트: ctrlr->thread.
 */
static int
nvmf_ctrlr_set_features_number_of_queues(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */
	uint32_t count; /* [한국어] 현재 활성 qpair 수 */

	SPDK_DEBUGLOG(nvmf, "Set Features - Number of Queues, cdw11 0x%x\n",
		      req->cmd->nvme_cmd.cdw11);

	if (cmd->cdw11_bits.feat_num_of_queues.bits.ncqr == UINT16_MAX ||
	    cmd->cdw11_bits.feat_num_of_queues.bits.nsqr == UINT16_MAX) {
		/* [한국어] 0xFFFF는 invalid (0-based이므로 65535개 큐는 불가) */
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	count = spdk_bit_array_count_set(ctrlr->qpair_mask); /* [한국어] 활성 qpair 수 조회 */
	/* verify that the controller is ready to process commands */
	if (count > 1) {
		/* [한국어] admin qpair(1개) 외에 IO qpair가 이미 있으면 순서 오류 */
		SPDK_DEBUGLOG(nvmf, "Queue pairs already active!\n");
		rsp->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
	} else {
		/*
		 * Ignore the value requested by the host -
		 * always return the pre-configured value based on max_qpairs_allowed.
		 */
		/* [한국어] 호스트 요청값 무시; 항상 pre-configured 큐 수를 CDW0에 반환 */
		rsp->cdw0 = ctrlr->feat.number_of_queues.raw;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/* [한국어] 컨트롤러 구조체 크기 정적 검증.
 * spdk_nvmf_ctrlr에 마이그레이션 관련 필드를 추가/삭제할 때 이 값을 업데이트해야 한다. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ctrlr) == 4936,
		   "Please check migration fields that need to be added or not");

/*
 * [한국어]
 * nvmf_ctrlr_migr_data_copy - 마이그레이션 데이터를 버전 안전하게 복사
 *
 * @data: 복사 대상 마이그레이션 데이터 구조체.
 * @data_src: 복사 원본 마이그레이션 데이터 구조체.
 * @data_size: 복사할 최대 크기 (버전 호환성을 위해 사용).
 *
 * spdk_nvmf_ctrlr_migr_data 구조체를 원본에서 대상으로 복사한다.
 * regs/feat은 각각 regs_size/feat_size 중 작은 값만큼 복사하여 버전 불일치에 대응한다.
 * 개별 필드는 SET_FIELD 매크로로 data_size 범위 내에서만 복사한다.
 * 배열(async_events, aer_cids)은 SET_ARRAY 매크로로 처리한다.
 *
 * 실행 컨텍스트: ctrlr->thread (save/restore 함수 내부).
 */
static void
nvmf_ctrlr_migr_data_copy(struct spdk_nvmf_ctrlr_migr_data *data,
			  const struct spdk_nvmf_ctrlr_migr_data *data_src, size_t data_size)
{
	assert(data); /* [한국어] 대상 포인터 NULL 금지 */
	assert(data_src); /* [한국어] 원본 포인터 NULL 금지 */
	assert(data_size); /* [한국어] 복사 크기 0 금지 */

	/* [한국어] vcprop(가상 레지스터) 복사: 더 작은 버전 크기만큼만 복사 */
	memcpy(&data->regs, &data_src->regs, spdk_min(data->regs_size, data_src->regs_size));
	/* [한국어] feat(Feature 설정값) 복사: 더 작은 버전 크기만큼만 복사 */
	memcpy(&data->feat, &data_src->feat, spdk_min(data->feat_size, data_src->feat_size));

/* [한국어] SET_FIELD: data_size 범위 내의 필드만 조건부 복사 (버전 호환성 보장) */
#define SET_FIELD(field) \
    if (offsetof(struct spdk_nvmf_ctrlr_migr_data, field) + sizeof(data->field) <= data_size) { \
        data->field = data_src->field; \
    } \

	SET_FIELD(cntlid); /* [한국어] 컨트롤러 ID (호스트에게 알려진 ID) */
	SET_FIELD(acre); /* [한국어] ACRE 활성화 여부 */
	SET_FIELD(num_aer_cids); /* [한국어] 미완료 AER 요청 수 */
	SET_FIELD(num_async_events); /* [한국어] 보류 중인 비동기 이벤트 수 */
	SET_FIELD(notice_aen_mask); /* [한국어] AEN(Async Event Notification) 마스크 */
#undef SET_FIELD

/* [한국어] SET_ARRAY: 배열 전체를 data_size 범위 내에서만 복사 */
#define SET_ARRAY(arr) \
    if (offsetof(struct spdk_nvmf_ctrlr_migr_data, arr) + sizeof(data->arr) <= data_size) { \
        memcpy(&data->arr, &data_src->arr, sizeof(data->arr)); \
    } \

	SET_ARRAY(async_events); /* [한국어] 보류 중인 비동기 이벤트 배열 */
	SET_ARRAY(aer_cids); /* [한국어] 미완료 AER 명령의 CID(Command ID) 배열 */
#undef SET_ARRAY
}

/*
 * [한국어]
 * spdk_nvmf_ctrlr_save_migr_data - 컨트롤러 상태를 마이그레이션 데이터로 저장
 *
 * @ctrlr: 저장할 컨트롤러.
 * @data: 저장 대상 마이그레이션 데이터 버퍼 (호출자가 제공, data_size 설정 필요).
 * @return: 항상 0.
 *
 * 컨트롤러의 현재 상태(레지스터, Feature 값, cntlid, ACRE, 미완료 AER, AEN 마스크 등)를
 * spdk_nvmf_ctrlr_migr_data 구조체에 복사한다. 이를 통해 라이브 마이그레이션 시
 * 컨트롤러 상태를 다른 노드/프로세스로 전달할 수 있다.
 *
 * 실행 컨텍스트: ctrlr->thread (assert로 강제).
 *
 * 호출 체인:
 *   마이그레이션 RPC → 본 함수 → nvmf_ctrlr_migr_data_copy
 */
int
spdk_nvmf_ctrlr_save_migr_data(struct spdk_nvmf_ctrlr *ctrlr,
			       struct spdk_nvmf_ctrlr_migr_data *data)
{
	struct spdk_nvmf_async_event_completion *event, *event_tmp; /* [한국어] 비동기 이벤트 순회용 */
	uint32_t i; /* [한국어] AER CID 복사 루프 인덱스 */
	/* [한국어] 로컬 마이그레이션 데이터 구조체 초기화 (현재 버전 크기로) */
	struct spdk_nvmf_ctrlr_migr_data data_local = {
		.data_size = offsetof(struct spdk_nvmf_ctrlr_migr_data, unused),
		.regs_size = sizeof(struct spdk_nvmf_registers),
		.feat_size = sizeof(struct spdk_nvmf_ctrlr_feat)
	};

	assert(data->data_size <= sizeof(data_local)); /* [한국어] 호출자 버퍼가 로컬보다 클 수 없음 */
	assert(spdk_get_thread() == ctrlr->thread); /* [한국어] ctrlr->thread에서만 실행 가능 */

	memcpy(&data_local.regs, &ctrlr->vcprop, sizeof(struct spdk_nvmf_registers)); /* [한국어] 가상 레지스터 복사 */
	memcpy(&data_local.feat, &ctrlr->feat, sizeof(struct spdk_nvmf_ctrlr_feat)); /* [한국어] Feature 값 복사 */

	data_local.cntlid = ctrlr->cntlid; /* [한국어] 컨트롤러 ID 저장 */
	data_local.acre = ctrlr->acre_enabled; /* [한국어] ACRE 활성화 여부 저장 */
	data_local.num_aer_cids = ctrlr->nr_aer_reqs; /* [한국어] 미완료 AER 수 저장 */

	/* [한국어] 보류 중인 비동기 이벤트를 배열에 복사 (최대 SPDK_NVMF_MIGR_MAX_PENDING_AERS개) */
	STAILQ_FOREACH_SAFE(event, &ctrlr->async_events, link, event_tmp) {
		if (data_local.num_async_events + 1 > SPDK_NVMF_MIGR_MAX_PENDING_AERS) {
			/* [한국어] 배열 크기 초과 → 경고 후 중단 */
			SPDK_ERRLOG("ctrlr %p has too many pending AERs\n", ctrlr);
			break;
		}

		data_local.async_events[data_local.num_async_events++].raw = event->event.raw; /* [한국어] 이벤트 raw 값 복사 */
	}

	for (i = 0; i < ctrlr->nr_aer_reqs; i++) {
		/* [한국어] 미완료 AER 요청의 CID를 배열에 저장 (복원 시 재발행에 사용) */
		struct spdk_nvmf_request *req = ctrlr->aer_req[i];
		data_local.aer_cids[i] = req->cmd->nvme_cmd.cid; /* [한국어] AER Command ID 저장 */
	}
	data_local.notice_aen_mask = ctrlr->notice_aen_mask; /* [한국어] AEN 마스크 저장 */

	nvmf_ctrlr_migr_data_copy(data, &data_local, spdk_min(data->data_size, data_local.data_size)); /* [한국어] 로컬 → 호출자 버퍼로 버전 안전 복사 */
	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_ctrlr_restore_migr_data - 마이그레이션 데이터에서 컨트롤러 상태 복원
 *
 * @ctrlr: 복원 대상 컨트롤러 (새로 생성된 빈 컨트롤러).
 * @data: 원본 노드에서 save_migr_data로 저장된 마이그레이션 데이터.
 * @return: 0(성공) 또는 -ENOMEM (AER 이벤트 할당 실패).
 *
 * spdk_nvmf_ctrlr_save_migr_data()가 저장한 컨트롤러 상태를 새 컨트롤러 객체에
 * 복원한다. vcprop(레지스터), feat(Feature), cntlid, ACRE, 미완료 비동기 이벤트,
 * AEN 마스크를 복원한다.
 *
 * 실행 컨텍스트: ctrlr->thread (assert로 강제).
 *
 * 호출 체인:
 *   마이그레이션 RPC → 본 함수 → nvmf_ctrlr_migr_data_copy
 */
int
spdk_nvmf_ctrlr_restore_migr_data(struct spdk_nvmf_ctrlr *ctrlr,
				  const struct spdk_nvmf_ctrlr_migr_data *data)
{
	uint32_t i; /* [한국어] 비동기 이벤트 복원 루프 인덱스 */
	/* [한국어] 현재 버전 기본값으로 로컬 구조체 초기화 */
	struct spdk_nvmf_ctrlr_migr_data data_local = {
		.data_size = offsetof(struct spdk_nvmf_ctrlr_migr_data, unused),
		.regs_size = sizeof(struct spdk_nvmf_registers),
		.feat_size = sizeof(struct spdk_nvmf_ctrlr_feat)
	};

	assert(data->data_size <= sizeof(data_local)); /* [한국어] 원본 버퍼가 로컬보다 클 수 없음 */
	assert(spdk_get_thread() == ctrlr->thread); /* [한국어] ctrlr->thread에서만 실행 */

	/* local version of data should have defaults set before copy */
	/* [한국어] 원본 → 로컬로 버전 안전 복사 (기본값 유지 + 구버전 호환) */
	nvmf_ctrlr_migr_data_copy(&data_local, data, spdk_min(data->data_size, data_local.data_size));
	memcpy(&ctrlr->vcprop, &data_local.regs, sizeof(struct spdk_nvmf_registers)); /* [한국어] 가상 레지스터 복원 */
	memcpy(&ctrlr->feat, &data_local.feat, sizeof(struct spdk_nvmf_ctrlr_feat)); /* [한국어] Feature 값 복원 */

	ctrlr->cntlid = data_local.cntlid; /* [한국어] 컨트롤러 ID 복원 */
	ctrlr->acre_enabled = data_local.acre; /* [한국어] ACRE 활성화 여부 복원 */

	/* [한국어] 보류 중인 비동기 이벤트 목록 복원 */
	for (i = 0; i < data_local.num_async_events; i++) {
		struct spdk_nvmf_async_event_completion *event;

		event = calloc(1, sizeof(*event)); /* [한국어] AER 완료 구조체 할당 */
		if (!event) {
			return -ENOMEM; /* [한국어] 메모리 부족 */
		}

		event->event.raw = data_local.async_events[i].raw; /* [한국어] 이벤트 raw 값 복원 */
		STAILQ_INSERT_TAIL(&ctrlr->async_events, event, link); /* [한국어] async_events 큐에 추가 */
	}
	ctrlr->notice_aen_mask = data_local.notice_aen_mask; /* [한국어] AEN 마스크 복원 */

	return 0;
}

/*
 * [한국어]
 * nvmf_ctrlr_set_features_async_event_configuration - Set Features: Async Event Configuration (Feature ID 0Bh)
 *
 * @req: Admin 명령 요청. CDW11에 이벤트 마스크(SMART/Health Critical Warning 비트 등) 포함.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 어떤 종류의 비동기 이벤트를 발생시킬지 설정한다.
 * cdw11의 비트 중 Namespace Attribute Notice, Firmware Activation Notice, ANA Change 등을
 * 포함하며, reserved 비트는 클리어된다.
 * 이 Feature 값은 나중에 이벤트 발생 시 notice_aen_mask와 함께 필터링에 사용된다.
 */
static int
nvmf_ctrlr_set_features_async_event_configuration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */

	SPDK_DEBUGLOG(nvmf, "Set Features - Async Event Configuration, cdw11 0x%08x\n",
		      cmd->cdw11);
	ctrlr->feat.async_event_configuration.raw = cmd->cdw11; /* [한국어] AEC 비트맵 raw 값 저장 */
	ctrlr->feat.async_event_configuration.bits.reserved1 = 0; /* [한국어] reserved1 클리어 */
	ctrlr->feat.async_event_configuration.bits.reserved2 = 0; /* [한국어] reserved2 클리어 */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_async_event_request - AER(Async Event Request, opcode 0Ch) 처리
 *
 * @req: AER 요청.
 * @return: COMPLETE(즉시 응답 가능한 pending 이벤트 있음 또는 한도 초과 에러)
 *          또는 ASYNCHRONOUS(슬롯에 보관해 두고 이벤트 발생 시 응답).
 *
 * 호스트는 사전에 N개의 AER을 보내두고 컨트롤러 측에서 비동기 이벤트(NS 변경,
 * ANA 변경, Discovery 변경, Error, Reservation 등)가 발생하면 그 AER에 응답이
 * 채워져 돌아온다. NVMe 1.4 7.1.2.
 * - 한도(SPDK_NVMF_MAX_ASYNC_EVENTS=4) 초과 시 즉시 AERL EXCEEDED 응답.
 * - 보관해둔 이벤트(async_events 큐)가 있으면 즉시 그것을 cdw0로 응답.
 * - 그 외에는 aer_req[]에 슬롯 저장. 이벤트 발생 시 nvmf_ctrlr_async_event_notification에서 응답.
 */
static int
nvmf_ctrlr_async_event_request(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_async_event_completion *pending_event;

	SPDK_DEBUGLOG(nvmf, "Async Event Request\n");

	/* Four asynchronous events are supported for now */
	if (ctrlr->nr_aer_reqs >= SPDK_NVMF_MAX_ASYNC_EVENTS) {
		SPDK_DEBUGLOG(nvmf, "AERL exceeded\n");
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (!STAILQ_EMPTY(&ctrlr->async_events)) {
		pending_event = STAILQ_FIRST(&ctrlr->async_events);
		rsp->cdw0 = pending_event->event.raw;
		STAILQ_REMOVE(&ctrlr->async_events, pending_event, spdk_nvmf_async_event_completion, link);
		free(pending_event);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr->aer_req[ctrlr->nr_aer_reqs++] = req;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static int
nvmf_get_firmware_slot_log_page(struct iovec *iovs, int iovcnt, uint64_t offset, uint32_t length)
{
	struct spdk_nvme_firmware_page fw_page;
	size_t copy_len;
	struct spdk_iov_xfer ix;

	spdk_iov_xfer_init(&ix, iovs, iovcnt);

	memset(&fw_page, 0, sizeof(fw_page));
	fw_page.afi.active_slot = 1;
	fw_page.afi.next_reset_slot = 0;
	spdk_strcpy_pad(fw_page.revision[0], FW_VERSION, sizeof(fw_page.revision[0]), ' ');

	if (offset < sizeof(fw_page)) {
		copy_len = spdk_min(sizeof(fw_page) - offset, length);
		if (copy_len > 0) {
			spdk_iov_xfer_from_buf(&ix, (const char *)&fw_page + offset, copy_len);
		}
	} else {
		SPDK_ERRLOG("Invalid Get log page firmware slot offset: (%" PRIu64 "), log page size (%zu)\n",
			    offset, sizeof(fw_page));
		return -EINVAL;
	}

	return 0;
}

void
nvmf_ctrlr_unmask_aen(struct spdk_nvmf_ctrlr *ctrlr,
		      enum spdk_nvme_async_event_mask_bit mask)
{
	ctrlr->notice_aen_mask &= ~(1 << mask);
}

static inline bool
nvmf_ctrlr_mask_aen(struct spdk_nvmf_ctrlr *ctrlr,
		    enum spdk_nvme_async_event_mask_bit mask)
{
	if (ctrlr->notice_aen_mask & (1 << mask)) {
		return false;
	} else {
		ctrlr->notice_aen_mask |= (1 << mask);
		return true;
	}
}

/* we have to use the typedef in the function declaration to appease astyle. */
typedef enum spdk_nvme_ana_state spdk_nvme_ana_state_t;

static inline spdk_nvme_ana_state_t
nvmf_ctrlr_get_ana_state(struct spdk_nvmf_ctrlr *ctrlr, uint32_t anagrpid)
{
	if (!ctrlr->subsys->flags.ana_reporting) {
		return SPDK_NVME_ANA_OPTIMIZED_STATE;
	}

	if (spdk_unlikely(!nvmf_subsystem_listener_is_active(ctrlr->listener))) {
		return SPDK_NVME_ANA_INACCESSIBLE_STATE;
	}

	assert(anagrpid - 1 < ctrlr->subsys->max_nsid);
	return ctrlr->listener->ana_state[anagrpid - 1];
}

static spdk_nvme_ana_state_t
nvmf_ctrlr_get_ana_state_from_nsid(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvmf_ns *ns;

	/* We do not have NVM subsystem specific ANA state. Hence if NSID is either
	 * SPDK_NVMF_GLOBAL_NS_TAG, invalid, or for inactive namespace, return
	 * the optimized state.
	 */
	ns = nvmf_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		return SPDK_NVME_ANA_OPTIMIZED_STATE;
	}

	return nvmf_ctrlr_get_ana_state(ctrlr, ns->anagrpid);
}

static void
nvmf_get_error_log_page(struct spdk_nvmf_ctrlr *ctrlr, struct iovec *iovs, int iovcnt,
			uint64_t offset, uint32_t length, uint32_t rae)
{
	if (!rae) {
		nvmf_ctrlr_unmask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_ERROR_MASK_BIT);
	}

	/* TODO: actually fill out log page data */
}

static int
nvmf_get_ana_log_page(struct spdk_nvmf_ctrlr *ctrlr, struct iovec *iovs, int iovcnt,
		      uint64_t offset, uint32_t length, uint32_t rae, uint32_t rgo)
{
	struct spdk_nvme_ana_page ana_hdr;
	struct spdk_nvme_ana_group_descriptor ana_desc;
	size_t copy_len, copied_len;
	uint32_t num_anagrp = 0, anagrpid;
	size_t total_page_size = sizeof(ana_hdr);
	struct spdk_nvmf_ns *ns;
	struct spdk_iov_xfer ix;

	spdk_iov_xfer_init(&ix, iovs, iovcnt);

	if (length == 0) {
		goto done;
	}

	for (anagrpid = 1; anagrpid <= ctrlr->subsys->max_nsid; anagrpid++) {
		if (ctrlr->subsys->ana_group[anagrpid - 1] == 0) {
			continue;
		}

		total_page_size += sizeof(ana_desc);

		if (rgo) {
			continue;
		}

		for (ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); ns != NULL;
		     ns = spdk_nvmf_subsystem_get_next_ns(ctrlr->subsys, ns)) {
			if (ns->anagrpid == anagrpid) {
				total_page_size += sizeof(uint32_t);
			}
		}
	}

	if (offset >= total_page_size) {
		SPDK_ERRLOG("Invalid Get log page ana offset: (%" PRIu64 "), log page size (%zu)\n",
			    offset, total_page_size);
		return -EINVAL;
	}

	if (offset >= sizeof(ana_hdr)) {
		offset -= sizeof(ana_hdr);
	} else {
		for (anagrpid = 1; anagrpid <= ctrlr->subsys->max_nsid; anagrpid++) {
			if (ctrlr->subsys->ana_group[anagrpid - 1] > 0) {
				num_anagrp++;
			}
		}

		memset(&ana_hdr, 0, sizeof(ana_hdr));

		ana_hdr.num_ana_group_desc = num_anagrp;
		/* TODO: Support Change Count. */
		ana_hdr.change_count = 0;

		copy_len = spdk_min(sizeof(ana_hdr) - offset, length);
		copied_len = spdk_iov_xfer_from_buf(&ix, (const char *)&ana_hdr + offset, copy_len);
		assert(copied_len == copy_len);
		length -= copied_len;
		offset = 0;
	}

	if (length == 0) {
		goto done;
	}

	for (anagrpid = 1; anagrpid <= ctrlr->subsys->max_nsid; anagrpid++) {
		if (ctrlr->subsys->ana_group[anagrpid - 1] == 0) {
			continue;
		}

		if (offset >= sizeof(ana_desc)) {
			offset -= sizeof(ana_desc);
		} else {
			memset(&ana_desc, 0, sizeof(ana_desc));

			ana_desc.ana_group_id = anagrpid;
			if (rgo) {
				ana_desc.num_of_nsid = 0;
			} else {
				ana_desc.num_of_nsid = ctrlr->subsys->ana_group[anagrpid - 1];
			}
			ana_desc.ana_state = nvmf_ctrlr_get_ana_state(ctrlr, anagrpid);

			copy_len = spdk_min(sizeof(ana_desc) - offset, length);
			copied_len = spdk_iov_xfer_from_buf(&ix, (const char *)&ana_desc + offset,
							    copy_len);
			assert(copied_len == copy_len);
			length -= copied_len;
			offset = 0;

			if (length == 0) {
				goto done;
			}
		}

		if (rgo) {
			continue;
		}

		/* TODO: Revisit here about O(n^2) cost if we have subsystem with
		 * many namespaces in the future.
		 */
		for (ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); ns != NULL;
		     ns = spdk_nvmf_subsystem_get_next_ns(ctrlr->subsys, ns)) {
			if (ns->anagrpid != anagrpid) {
				continue;
			}

			if (offset >= sizeof(uint32_t)) {
				offset -= sizeof(uint32_t);
				continue;
			}

			copy_len = spdk_min(sizeof(uint32_t) - offset, length);
			copied_len = spdk_iov_xfer_from_buf(&ix, (const char *)&ns->nsid + offset,
							    copy_len);
			assert(copied_len == copy_len);
			length -= copied_len;
			offset = 0;

			if (length == 0) {
				goto done;
			}
		}
	}

done:
	if (!rae) {
		nvmf_ctrlr_unmask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_ANA_CHANGE_MASK_BIT);
	}

	return 0;
}

void
nvmf_ctrlr_ns_changed(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
	uint16_t max_changes = SPDK_COUNTOF(ctrlr->changed_ns_list.ns_list);
	uint16_t i;
	bool found = false;

	for (i = 0; i < ctrlr->changed_ns_list_count; i++) {
		if (ctrlr->changed_ns_list.ns_list[i] == nsid) {
			/* nsid is already in the list */
			found = true;
			break;
		}
	}

	if (!found) {
		if (ctrlr->changed_ns_list_count == max_changes) {
			/* Out of space - set first entry to FFFFFFFFh and zero-fill the rest. */
			ctrlr->changed_ns_list.ns_list[0] = 0xFFFFFFFFu;
			for (i = 1; i < max_changes; i++) {
				ctrlr->changed_ns_list.ns_list[i] = 0;
			}
		} else {
			ctrlr->changed_ns_list.ns_list[ctrlr->changed_ns_list_count++] = nsid;
		}
	}
}

static int
nvmf_get_changed_ns_list_log_page(struct spdk_nvmf_ctrlr *ctrlr,
				  struct iovec *iovs, int iovcnt, uint64_t offset, uint32_t length, uint32_t rae)
{
	size_t copy_length;
	struct spdk_iov_xfer ix;
	size_t page_size = sizeof(ctrlr->changed_ns_list);


	spdk_iov_xfer_init(&ix, iovs, iovcnt);

	if (offset < page_size) {
		copy_length = spdk_min(length, page_size - offset);
		if (copy_length) {
			spdk_iov_xfer_from_buf(&ix, (char *)&ctrlr->changed_ns_list + offset, copy_length);
		}
	} else {
		SPDK_ERRLOG("Invalid Get log page changed ns list offset: (%" PRIu64 "), log page size (%zu)\n",
			    offset, page_size);
		return -EINVAL;
	}

	/* Clear log page each time it is read */
	ctrlr->changed_ns_list_count = 0;
	memset(&ctrlr->changed_ns_list, 0, page_size);

	if (!rae) {
		nvmf_ctrlr_unmask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGE_MASK_BIT);
	}

	return 0;
}

/* The structure can be modified if we provide support for other commands in future */
static const struct spdk_nvme_cmds_and_effect_log_page g_cmds_and_effect_log_page = {
	.admin_cmds_supported = {
		/* Get Log Page */
		[SPDK_NVME_OPC_GET_LOG_PAGE]		= { .csupp = 1, .nscpe = 1, .cscpe = 1 },
		/* Identify */
		[SPDK_NVME_OPC_IDENTIFY]		= { .csupp = 1, .nscpe = 1, .cscpe = 1 },
		/* Abort */
		[SPDK_NVME_OPC_ABORT]			= { .csupp = 1, .cscpe = 1 },
		/* Set Features */
		[SPDK_NVME_OPC_SET_FEATURES]		= { .csupp = 1, .nscpe = 1, .cscpe = 1 },
		/* Get Features */
		[SPDK_NVME_OPC_GET_FEATURES]		= { .csupp = 1, .nscpe = 1, .cscpe = 1 },
		/* Async Event Request */
		[SPDK_NVME_OPC_ASYNC_EVENT_REQUEST]	= { .csupp = 1, .cscpe = 1 },
		/* Keep Alive */
		[SPDK_NVME_OPC_KEEP_ALIVE]		= { .csupp = 1, .cscpe = 1 },
	},
	.io_cmds_supported = {
		/* FLUSH */
		[SPDK_NVME_OPC_FLUSH]			= { .csupp = 1, .lbcc = 1, .nscpe = 1 },
		/* WRITE */
		[SPDK_NVME_OPC_WRITE]			= { .csupp = 1, .lbcc = 1, .nscpe = 1 },
		/* READ */
		[SPDK_NVME_OPC_READ]			= { .csupp = 1, .nscpe = 1 },
		/* WRITE ZEROES */
		[SPDK_NVME_OPC_WRITE_ZEROES]		= { .csupp = 1, .lbcc = 1, .nscpe = 1 },
		/* DATASET MANAGEMENT */
		[SPDK_NVME_OPC_DATASET_MANAGEMENT]	= { .csupp = 1, .lbcc = 1, .nscpe = 1 },
		/* COMPARE */
		[SPDK_NVME_OPC_COMPARE]			= { .csupp = 1, .nscpe = 1 },
		/* RESERVATION REGISTER */
		[SPDK_NVME_OPC_RESERVATION_REGISTER]	= { .csupp = 1, .nscpe = 1 },
		/* RESERVATION REPORT */
		[SPDK_NVME_OPC_RESERVATION_REPORT]	= { .csupp = 1, .nscpe = 1 },
		/* RESERVATION ACQUIRE */
		[SPDK_NVME_OPC_RESERVATION_ACQUIRE]	= { .csupp = 1, .nscpe = 1 },
		/* RESERVATION RELEASE */
		[SPDK_NVME_OPC_RESERVATION_RELEASE]	= { .csupp = 1, .nscpe = 1 },
		/* ZONE MANAGEMENT SEND */
		[SPDK_NVME_OPC_ZONE_MGMT_SEND]		= { .csupp = 1, .lbcc = 1, .nscpe = 1 },
		/* ZONE MANAGEMENT RECEIVE */
		[SPDK_NVME_OPC_ZONE_MGMT_RECV]		= { .csupp = 1, .nscpe = 1 },
		/* ZONE APPEND */
		[SPDK_NVME_OPC_ZONE_APPEND]		= { .csupp = 1, .lbcc = 1, .nscpe = 1 },
		/* COPY */
		[SPDK_NVME_OPC_COPY]			= { .csupp = 1, .lbcc = 1, .nscpe = 1 },
	},
};

/*
 * [한국어]
 * spdk_nvmf_get_cmds_and_effects_log_page - Commands and Effects Log Page(LID=05h) 데이터 생성
 *
 * @ctrlr: 대상 컨트롤러.
 * @log_page: 결과를 저장할 Commands and Effects Log Page 구조체 포인터.
 *
 * g_cmds_and_effect_log_page 전역 테이블을 기반으로 복사한 뒤, 이 컨트롤러가
 * 지원하지 않는 선택적 명령들(Write Zeroes, DSM, Compare, ZNS, Copy, Reservation)의
 * 항목을 0으로 초기화한다. ONCS/ZNS/ANA 필드에 따라 동적으로 지원 여부를 반영한다.
 *
 * 호출 컨텍스트: nvmf_get_cmds_and_effects_log_page() 내부. ctrlr->thread에서 실행.
 *
 * 호출 체인:
 *   nvmf_get_cmds_and_effects_log_page → [이 함수]
 */
void
spdk_nvmf_get_cmds_and_effects_log_page(struct spdk_nvmf_ctrlr *ctrlr,
					struct spdk_nvme_cmds_and_effect_log_page *log_page)
{
	struct spdk_nvme_cmds_and_effect_entry *entry; /* [한국어] 특정 opcode의 cmds_and_effects 항목 포인터 */

	*log_page = g_cmds_and_effect_log_page; /* [한국어] 전역 기본 테이블을 로컬로 복사 (모든 지원 명령이 csupp=1로 설정됨) */
	if (!ctrlr->cdata.oncs.nvmwzsv || !nvmf_ctrlr_write_zeroes_supported(ctrlr)) {
		/* [한국어] Write Zeroes 미지원(ONCS.nvmwzsv=0 또는 bdev 미지원) → 해당 항목 클리어 */
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_WRITE_ZEROES];
		memset(entry, 0, sizeof(*entry));
	}
	if (!ctrlr->cdata.oncs.nvmdsmsv || !nvmf_ctrlr_dsm_supported(ctrlr)) {
		/* [한국어] Dataset Management(TRIM) 미지원 → 항목 클리어 */
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_DATASET_MANAGEMENT];
		memset(entry, 0, sizeof(*entry));
	}
	if (!ctrlr->cdata.oncs.nvmcmps) {
		/* [한국어] Compare 명령 미지원(ONCS.nvmcmps=0) → 항목 클리어 */
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_COMPARE];
		memset(entry, 0, sizeof(*entry));
	}
	if (!nvmf_subsystem_has_zns_iocs(ctrlr->subsys)) {
		/* [한국어] subsystem에 ZNS NS가 없으면 Zone Management Send/Recv 항목 클리어 */
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_ZONE_MGMT_SEND];
		memset(entry, 0, sizeof(*entry));
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_ZONE_MGMT_RECV];
		memset(entry, 0, sizeof(*entry));
	}
	if (!nvmf_subsystem_zone_append_supported(ctrlr->subsys)) {
		/* [한국어] Zone Append 미지원 subsystem → 항목 클리어 */
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_ZONE_APPEND];
		memset(entry, 0, sizeof(*entry));
	}
	if (!ctrlr->cdata.oncs.nvmcpys) {
		/* [한국어] Copy 명령 미지원(ONCS.nvmcpys=0) → 항목 클리어 */
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_COPY];
		memset(entry, 0, sizeof(*entry));
	}
	if (!ctrlr->cdata.oncs.reservs) {
		/* [한국어] Reservation 명령 미지원(ONCS.reservs=0) → 모든 Reservation 항목 클리어 */
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_RESERVATION_REGISTER];
		memset(entry, 0, sizeof(*entry));
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_RESERVATION_REPORT];
		memset(entry, 0, sizeof(*entry));
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_RESERVATION_ACQUIRE];
		memset(entry, 0, sizeof(*entry));
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_RESERVATION_RELEASE];
		memset(entry, 0, sizeof(*entry));
	}
}

/*
 * [한국어]
 * nvmf_get_cmds_and_effects_log_page - Get Log Page(LID=05h, Commands and Effects) 처리
 *
 * @ctrlr: 대상 컨트롤러.
 * @iovs: 결과 데이터를 담을 iov 배열 (호스트 측 DMA 버퍼).
 * @iovcnt: iov 개수.
 * @offset: 로그 페이지 내 byte offset (호스트가 요청한 시작 위치).
 * @length: 요청한 데이터 길이(바이트).
 * @return: 0(성공) 또는 -EINVAL(offset 범위 초과).
 *
 * spdk_nvmf_get_cmds_and_effects_log_page()로 데이터 생성 후 offset/length 범위에
 * 해당하는 부분만 iov 버퍼에 복사한다. iov_xfer API를 사용하여 scatter-gather 처리.
 *
 * 호출 컨텍스트: ctrlr->thread (nvmf_ctrlr_get_log_page 내부).
 *
 * 호출 체인:
 *   nvmf_ctrlr_get_log_page → [이 함수] → spdk_nvmf_get_cmds_and_effects_log_page
 */
static int
nvmf_get_cmds_and_effects_log_page(struct spdk_nvmf_ctrlr *ctrlr, struct iovec *iovs, int iovcnt,
				   uint64_t offset, uint32_t length)
{
	uint32_t page_size = sizeof(struct spdk_nvme_cmds_and_effect_log_page); /* [한국어] Commands and Effects 로그 페이지 전체 크기 */
	size_t copy_len = 0; /* [한국어] 실제 복사할 바이트 수 */
	struct spdk_iov_xfer ix; /* [한국어] scatter-gather iov 복사 컨텍스트 */
	struct spdk_nvme_cmds_and_effect_log_page cmds_and_effects_log_page = {}; /* [한국어] 스택에 임시 생성할 로그 페이지 데이터 */

	spdk_nvmf_get_cmds_and_effects_log_page(ctrlr, &cmds_and_effects_log_page); /* [한국어] ONCS/ZNS/Reservation 지원에 따라 데이터 생성 */

	spdk_iov_xfer_init(&ix, iovs, iovcnt); /* [한국어] iov xfer 컨텍스트 초기화 (scatter-gather 순차 쓰기 준비) */
	if (offset < page_size) {
		/* [한국어] offset이 페이지 크기 미만: 요청 범위만큼 복사 */
		copy_len = spdk_min(page_size - offset, length); /* [한국어] 남은 페이지 데이터와 요청 길이 중 작은 값 */
		spdk_iov_xfer_from_buf(&ix, (char *)(&cmds_and_effects_log_page) + offset, copy_len); /* [한국어] 로그 페이지의 offset 위치에서 copy_len 바이트를 iov로 전송 */
	} else {
		/* [한국어] offset이 페이지 크기 이상: 요청 범위가 유효하지 않음 */
		SPDK_ERRLOG("Invalid Get log page cmds effects offset: (%" PRIu64 "), log page size (%" PRIu32")\n",
			    offset, page_size);
		return -EINVAL;
	}

	return 0; /* [한국어] 성공 */
}

/*
 * [한국어]
 * nvmf_get_reservation_notification_log_page - Get Log Page(LID=70h, Reservation Notification) 처리
 *
 * @ctrlr: 대상 컨트롤러 (log_head 큐와 num_avail_log_pages 보유).
 * @iovs: 결과 데이터를 담을 iov 배열.
 * @iovcnt: iov 개수.
 * @offset: 로그 페이지 내 byte offset.
 * @length: 요청한 데이터 길이(바이트).
 * @rae: Retain Asynchronous Event(RAE)=1이면 AEN 마스크 클리어 안 함.
 * @return: 0(성공) 또는 -EINVAL(offset 범위 초과).
 *
 * log_head TAILQ에서 항목을 순서대로 꺼내 iov로 복사한다. 각 항목은
 * _nvmf_ctrlr_add_reservation_log()에서 추가되었으며, 읽기 후 해제된다.
 * RAE=0이면 AEN 마스크를 클리어하여 다음 Reservation 이벤트에 다시 AER을 보낼 수 있게 한다.
 *
 * 호출 컨텍스트: ctrlr->thread (nvmf_ctrlr_get_log_page 내부).
 *
 * 호출 체인:
 *   nvmf_ctrlr_get_log_page → [이 함수] → nvmf_ctrlr_unmask_aen (if RAE=0)
 */
static int
nvmf_get_reservation_notification_log_page(struct spdk_nvmf_ctrlr *ctrlr,
		struct iovec *iovs, int iovcnt, uint64_t offset, uint32_t length, uint32_t rae)
{
	uint32_t unit_log_len, avail_log_len, next_pos, copy_len; /* [한국어] 로그 항목당 크기, 전체 크기, 순회 위치, 복사 길이 */
	struct spdk_nvmf_reservation_log *log, *log_tmp; /* [한국어] 순회 중인 log_head 항목 (SAFE: 삭제하면서 순회) */
	struct spdk_iov_xfer ix; /* [한국어] scatter-gather iov 복사 컨텍스트 */

	spdk_iov_xfer_init(&ix, iovs, iovcnt); /* [한국어] iov xfer 초기화 */

	unit_log_len = sizeof(struct spdk_nvme_reservation_notification_log); /* [한국어] 항목 하나의 크기 (로그 페이지 단위) */
	/* No available log, return zeroed log pages */
	/* [한국어] 현재 보고할 Reservation 로그가 없으면 0-fill된 버퍼를 그대로 반환 */
	if (!ctrlr->num_avail_log_pages) {
		return 0;
	}

	avail_log_len = ctrlr->num_avail_log_pages * unit_log_len; /* [한국어] 전체 이용 가능한 로그 데이터 크기 */
	if (offset >= avail_log_len) {
		/* [한국어] 요청 offset이 유효 범위를 초과 → 오류 */
		SPDK_ERRLOG("Invalid Get log page reservation notification offset: (%" PRIu64"), log page size (%"
			    PRIu32")\n", offset, avail_log_len);
		return -EINVAL;
	}

	next_pos = 0; /* [한국어] 현재까지 처리한 로그 데이터 누적 위치 */
	TAILQ_FOREACH_SAFE(log, &ctrlr->log_head, link, log_tmp) {
		TAILQ_REMOVE(&ctrlr->log_head, log, link); /* [한국어] 읽힌 항목을 큐에서 제거 (소비) */
		ctrlr->num_avail_log_pages--; /* [한국어] 잔여 로그 수 감소 */

		next_pos += unit_log_len; /* [한국어] 이 항목의 끝 위치 */
		if (next_pos > offset) {
			/* [한국어] 이 항목이 요청 offset 이후에 있으면 복사 */
			copy_len = spdk_min(next_pos - offset, length); /* [한국어] 이 항목에서 복사할 바이트 수 */
			spdk_iov_xfer_from_buf(&ix, &log->log, copy_len); /* [한국어] 로그 항목 데이터를 iov로 전송 */
			length -= copy_len; /* [한국어] 남은 복사 길이 갱신 */
			offset += copy_len; /* [한국어] 현재 소스 offset 갱신 */
		}
		free(log); /* [한국어] 로그 항목 메모리 해제 */

		if (length == 0) {
			/* [한국어] 요청한 길이만큼 복사 완료 → 순회 중단 */
			break;
		}
	}

	if (!rae) {
		/* [한국어] RAE=0: Retain 모드 아님 → AEN 마스크 클리어하여 다음 Reservation 이벤트에 AER 허용 */
		nvmf_ctrlr_unmask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL_MASK_BIT);
	}
	return 0; /* [한국어] 성공 */
}

/*
 * [한국어]
 * is_log_page_ctrlr_nvm_scope - 주어진 Log Page ID가 Controller/NVM scope인지 판별
 *
 * @lid: Log Page ID (SPDK_NVME_LOG_* 상수).
 * @return: true이면 Controller 또는 NVM scope log page, false이면 NS-specific.
 *
 * NVMe 스펙에서 일부 Log Page는 Controller/NVM 전체 범위(Ctrlr Scope)에 적용되고
 * 일부는 특정 Namespace에 적용된다. 이 함수는 NSID가 GLOBAL_NS_TAG여야 하는
 * Controller/NVM scope 페이지를 식별한다. 이를 통해 Get Log Page 처리에서
 * NS별 접근 권한 검사를 건너뛸 수 있다.
 *
 * 실행 컨텍스트: nvmf_ctrlr_get_log_page [ctrlr->thread].
 */
static bool
is_log_page_ctrlr_nvm_scope(uint8_t lid)
{
	switch (lid) {
	case SPDK_NVME_LOG_SUPPORTED_LOG_PAGES: /* [한국어] Supported Log Pages: 지원되는 로그 페이지 목록 */
	case SPDK_NVME_LOG_ERROR: /* [한국어] Error Information: 오류 로그 */
	case SPDK_NVME_LOG_FIRMWARE_SLOT: /* [한국어] Firmware Slot Information: FW 슬롯 정보 */
	case SPDK_NVME_LOG_CHANGED_NS_LIST: /* [한국어] Changed Namespace List: 변경된 NS 목록 */
	case SPDK_NVME_LOG_COMMAND_EFFECTS_LOG: /* [한국어] Commands Supported and Effects: 명령 효과 */
	case SPDK_NVME_LOG_DEVICE_SELF_TEST: /* [한국어] Device Self-test: 자가 진단 */
	case SPDK_NVME_LOG_TELEMETRY_HOST_INITIATED: /* [한국어] Telemetry Host-Initiated */
	case SPDK_NVME_LOG_TELEMETRY_CTRLR_INITIATED: /* [한국어] Telemetry Controller-Initiated */
	case SPDK_NVME_LOG_ENDURANCE_GROUP_INFORMATION: /* [한국어] Endurance Group Information */
	case SPDK_NVME_LOG_PREDICATBLE_LATENCY: /* [한국어] Predictable Latency Per NVM Set */
	case SPDK_NVME_LOG_PREDICTABLE_LATENCY_EVENT: /* [한국어] Predictable Latency Event Aggregate */
	case SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS: /* [한국어] Asymmetric Namespace Access (ANA): 멀티패스 상태 */
	case SPDK_NVME_LOG_PERSISTENT_EVENT_LOG: /* [한국어] Persistent Event Log */
	case SPDK_NVME_LOG_ENDURANCE_GROUP_EVENT: /* [한국어] Endurance Group Event Aggregate */
		return true; /* [한국어] Controller/NVM scope: NSID=0xFFFFFFFF(GLOBAL_NS_TAG) 사용 */
	default:
		return false; /* [한국어] NS-specific scope: 특정 NSID 필요 */
	}
}

/* [한국어]
 * g_supported_feat_id_effects_log_pages_discovery - Discovery 컨트롤러용 Feature IDs Effects 테이블.
 * Discovery subsystem은 KATO(Keep Alive)와 AEC(Async Event Config)만 지원한다.
 * fsupp=1: 지원함, cscpe=1: 컨트롤러 범위(Controller Scope) 변경 가능.
 */
static const struct spdk_nvme_feature_ids_effects_log_page
	g_supported_feat_id_effects_log_pages_discovery = {
	.fis = {
		[SPDK_NVME_FEAT_KEEP_ALIVE_TIMER] =	     { .fsupp = 1, .cscpe = 1}, /* [한국어] KATO: Keep Alive 타임아웃 설정 */
		[SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION] = { .fsupp = 1, .cscpe = 1}, /* [한국어] AEC: 비동기 이벤트 마스크 설정 */
	}
};

/* [한국어]
 * g_supported_feat_id_effects_log_pages - IO 컨트롤러용 Feature IDs Effects 테이블.
 * NVMe Get Log Page(LID=12h, Feature Identifiers Supported and Effects)에 반환할 항목들.
 * fsupp=1: 해당 Feature 지원, cscpe=1: 컨트롤러 범위 변경 가능, nscpe=1: 네임스페이스 범위 변경 가능.
 */
static const struct spdk_nvme_feature_ids_effects_log_page g_supported_feat_id_effects_log_pages = {
	.fis = {
		[SPDK_NVME_FEAT_KEEP_ALIVE_TIMER] =		  { .fsupp = 1, .cscpe = 1}, /* [한국어] KATO(Feature ID=0Fh): Keep Alive 타임아웃(ms) */
		[SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION] =	  { .fsupp = 1, .cscpe = 1}, /* [한국어] AEC(Feature ID=0Bh): AER 마스크 비트 설정 */
		[SPDK_NVME_FEAT_ARBITRATION] =			  { .fsupp = 1, .cscpe = 1}, /* [한국어] Arbitration(Feature ID=01h): 커맨드 중재 방식(RR/WRR/Vendor) */
		[SPDK_NVME_FEAT_POWER_MANAGEMENT] =		  { .fsupp = 1, .cscpe = 1}, /* [한국어] Power Management(Feature ID=02h): 전원 상태 설정 */
		[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD] =	  { .fsupp = 1, .cscpe = 1}, /* [한국어] Temperature Threshold(Feature ID=04h): 온도 임계값 */
		[SPDK_NVME_FEAT_ERROR_RECOVERY] =		  { .fsupp = 1, .nscpe = 1}, /* [한국어] Error Recovery(Feature ID=05h): 오류 복구 설정 — NS 범위 */
		[SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE] =		  { .fsupp = 1, .cscpe = 1}, /* [한국어] Volatile Write Cache(Feature ID=06h): WCE(Write Cache Enable) */
		[SPDK_NVME_FEAT_NUMBER_OF_QUEUES] =		  { .fsupp = 1, .cscpe = 1}, /* [한국어] Number of Queues(Feature ID=07h): IO SQ/CQ 개수 협상 */
		[SPDK_NVME_FEAT_INTERRUPT_COALESCING] =		  { .fsupp = 1, .cscpe = 1}, /* [한국어] Interrupt Coalescing(Feature ID=08h): MSI-X 코얼레싱 설정 */
		[SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION] = { .fsupp = 1, .cscpe = 1}, /* [한국어] Interrupt Vector Config(Feature ID=09h): 인터럽트 벡터 */
		[SPDK_NVME_FEAT_WRITE_ATOMICITY] =		  { .fsupp = 1, .cscpe = 1}, /* [한국어] Write Atomicity Normal(Feature ID=0Ah): 원자적 쓰기 크기 */
		[SPDK_NVME_FEAT_HOST_IDENTIFIER] =		  { .fsupp = 1, .cscpe = 1}, /* [한국어] Host Identifier(Feature ID=81h): 호스트 UUID 설정 */
		[SPDK_NVME_FEAT_HOST_RESERVE_MASK] =		  { .fsupp = 1, .nscpe = 1}, /* [한국어] Reservation Notification Mask(Feature ID=83h): Reservation 이벤트 마스크 — NS 범위 */
		[SPDK_NVME_FEAT_HOST_RESERVE_PERSIST] =		  { .fsupp = 1, .nscpe = 1}, /* [한국어] Reservation Persistence(Feature ID=84h): 전원 사이클 후 Reservation 유지 여부 — NS 범위 */
		[SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT] =	  { .fsupp = 1, .cscpe = 1}, /* [한국어] Host Behavior Support(Feature ID=16h): 호스트 기능 플래그(ACRE 등) */
	}
};

/*
 * [한국어]
 * spdk_nvmf_get_feature_ids_effects_log_page - Feature IDs Effects Log Page 데이터 채우기
 *
 * @ctrlr: 대상 컨트롤러.
 * @log_page: 결과를 저장할 Feature IDs Effects Log Page 구조체.
 *
 * Discovery subsystem이면 KAT/AEC만 포함하는 축약된 페이지를, IO subsystem이면
 * 모든 지원 Feature를 포함하는 전체 페이지를 반환한다.
 * Get Log Page(LID=12h)에 대한 응답에 사용된다.
 */
void
spdk_nvmf_get_feature_ids_effects_log_page(struct spdk_nvmf_ctrlr *ctrlr,
		struct spdk_nvme_feature_ids_effects_log_page *log_page)
{
	if (spdk_nvmf_subsystem_is_discovery(ctrlr->subsys)) {
		/* [한국어] Discovery subsystem은 KATO, AEC Feature만 지원 */
		*log_page = g_supported_feat_id_effects_log_pages_discovery;
	} else {
		/* [한국어] IO subsystem은 모든 지원 Feature Effects를 반환 */
		*log_page = g_supported_feat_id_effects_log_pages;
	}
}

/*
 * [한국어]
 * nvmf_get_feature_ids_effects_log_page - Get Log Page: Feature IDs Effects (LID=12h) 처리
 *
 * @ctrlr: 대상 컨트롤러.
 * @iovs: 데이터를 복사할 iov 배열.
 * @iovcnt: iov 개수.
 * @offset: 로그 페이지 내 읽기 시작 오프셋.
 * @length: 읽을 바이트 수.
 * @return: 0(성공) 또는 -EINVAL (오프셋 범위 초과).
 *
 * Feature IDs Effects Log Page(LID=12h)를 iovs 버퍼에 복사한다.
 * Discovery/IO subsystem 여부에 따라 다른 페이지 내용이 반환된다.
 */
static int
nvmf_get_feature_ids_effects_log_page(struct spdk_nvmf_ctrlr *ctrlr, struct iovec *iovs, int iovcnt,
				      uint64_t offset, uint32_t length)
{
	uint32_t page_size = sizeof(struct spdk_nvme_feature_ids_effects_log_page); /* [한국어] 전체 로그 페이지 크기 */
	size_t copy_len = 0; /* [한국어] 실제 복사할 바이트 수 */
	struct spdk_iov_xfer ix; /* [한국어] iov 전송 컨텍스트 */
	struct spdk_nvme_feature_ids_effects_log_page supported_feature_ids_effects_log_page = {}; /* [한국어] 반환할 로그 페이지 (zero-init) */

	spdk_nvmf_get_feature_ids_effects_log_page(ctrlr, &supported_feature_ids_effects_log_page); /* [한국어] 컨트롤러 타입에 맞는 페이지 내용 채우기 */

	spdk_iov_xfer_init(&ix, iovs, iovcnt); /* [한국어] iov 전송 컨텍스트 초기화 */
	if (offset < page_size) {
		copy_len = spdk_min(page_size - offset, length); /* [한국어] 복사 가능한 최대 바이트 수 계산 */
		spdk_iov_xfer_from_buf(&ix, (char *)(&supported_feature_ids_effects_log_page) + offset, copy_len); /* [한국어] offset부터 copy_len 바이트 복사 */
	} else {
		SPDK_ERRLOG("Invalid Get feat id effects log page offset: (%" PRIu64 "),"
			    " log page size (%" PRIu32")\n", offset, page_size);
		return -EINVAL;
	}

	return 0;
}

/* [한국어]
 * g_supported_log_pages_discover - Discovery 컨트롤러가 지원하는 Log Page ID 목록.
 * Get Log Page(LID=00h, Supported Log Pages)에 반환할 테이블.
 * Discovery는 로그 종류가 제한적이다.
 */
static const struct spdk_nvme_supported_log_pages g_supported_log_pages_discover = {
	.lids = {
		[SPDK_NVME_LOG_SUPPORTED_LOG_PAGES] = { .lsupp = 1 }, /* [한국어] LID=00h: Supported Log Pages 자체 */
		[SPDK_NVME_LOG_DISCOVERY] =	      { .lsupp = 1 }, /* [한국어] LID=70h: Discovery Log Page (NVMe-oF 특화) */
		[SPDK_NVME_LOG_FEATURE_IDS_EFFECTS] = { .lsupp = 1 }, /* [한국어] LID=12h: Feature IDs Effects */
	}
};

/* [한국어]
 * g_supported_log_pages - IO 컨트롤러가 지원하는 Log Page ID 목록.
 * Get Log Page(LID=00h, Supported Log Pages)에 반환할 전체 테이블.
 * lsupp=1: 해당 LID를 지원함.
 */
static const struct spdk_nvme_supported_log_pages g_supported_log_pages = {
	.lids = {
		[SPDK_NVME_LOG_SUPPORTED_LOG_PAGES] =	      { .lsupp = 1 }, /* [한국어] LID=00h: Supported Log Pages */
		[SPDK_NVME_LOG_ERROR] =			      { .lsupp = 1 }, /* [한국어] LID=01h: Error Information */
		[SPDK_NVME_LOG_HEALTH_INFORMATION] =	      { .lsupp = 1 }, /* [한국어] LID=02h: SMART / Health Information */
		[SPDK_NVME_LOG_FIRMWARE_SLOT] =		      { .lsupp = 1 }, /* [한국어] LID=03h: Firmware Slot Information */
		[SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS] = { .lsupp = 1 }, /* [한국어] LID=0Ch: ANA (Asymmetric Namespace Access) */
		[SPDK_NVME_LOG_COMMAND_EFFECTS_LOG] =	      { .lsupp = 1 }, /* [한국어] LID=05h: Commands Supported and Effects */
		[SPDK_NVME_LOG_CHANGED_NS_LIST] =	      { .lsupp = 1 }, /* [한국어] LID=04h: Changed Namespace List */
		[SPDK_NVME_LOG_RESERVATION_NOTIFICATION] =    { .lsupp = 1 }, /* [한국어] LID=80h: Reservation Notification */
		[SPDK_NVME_LOG_FEATURE_IDS_EFFECTS] =	      { .lsupp = 1 }, /* [한국어] LID=12h: Feature Identifiers Supported and Effects */
		[SPDK_NVME_LOG_NVME_MI_COMMANDS_EFFECTS] =    { .lsupp = 1 }, /* [한국어] LID=13h: NVMe-MI Commands Supported and Effects */
	}
};

void
spdk_nvmf_get_supported_log_pages(struct spdk_nvmf_ctrlr *ctrlr,
				  struct spdk_nvme_supported_log_pages *log_page)
{
	if (spdk_nvmf_subsystem_is_discovery(ctrlr->subsys)) {
		*log_page = g_supported_log_pages_discover;
	} else {
		*log_page = g_supported_log_pages;
		if (ctrlr->subsys->flags.ana_reporting != 1) {
			log_page->lids[SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS].lsupp = 0;
		}
	}
}

static int
nvmf_get_supported_log_pages(struct spdk_nvmf_ctrlr *ctrlr, struct iovec *iovs, int iovcnt,
			     uint64_t offset, uint32_t length)
{
	uint32_t page_size = sizeof(struct spdk_nvme_supported_log_pages);
	size_t copy_len = 0;
	struct spdk_iov_xfer ix;
	struct spdk_nvme_supported_log_pages supported_log_pages = {};

	spdk_nvmf_get_supported_log_pages(ctrlr, &supported_log_pages);

	spdk_iov_xfer_init(&ix, iovs, iovcnt);
	if (offset < page_size) {
		copy_len = spdk_min(page_size - offset, length);
		spdk_iov_xfer_from_buf(&ix, (char *)(&supported_log_pages) + offset, copy_len);
	} else {
		SPDK_ERRLOG("Invalid Get supported log pages offset: (%" PRIu64 "), log page size (%" PRIu32")\n",
			    offset, page_size);
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * nvmf_ctrlr_get_log_page - Get Log Page admin 명령(opcode 02h) 디스패처
 *
 * @req: 요청.
 * @return: COMPLETE 또는 ASYNCHRONOUS(Discovery Log의 비동기 경로).
 *
 * cmd->cdw10/cdw11/cdw12/cdw13에서 lid/numdl/numdu/offset(64-bit)을 추출하고,
 * 호스트가 요구한 길이(len = (numdu<<16)+numdl+1)*4 바이트를 검증한다.
 * Discovery subsystem이면 SUPPORTED_LOG_PAGES/DISCOVERY/FEATURE_IDS_EFFECTS만,
 * NVM subsystem이면 ERROR/HEALTH/FIRMWARE_SLOT/ANA/COMMAND_EFFECTS/CHANGED_NS_LIST
 * /RESERVATION_NOTIFICATION/FEATURE_IDS_EFFECTS/NVME_MI_EFFECTS를 분기 처리.
 *
 * 데이터는 대부분 req->iov로 직접 채워지며, RAE(Retain Async Event)=0이면 해당
 * AEN mask bit이 풀려서 다음 이벤트를 다시 트리거할 수 있게 된다 (NVMe 1.4 5.14).
 */
static int
nvmf_ctrlr_get_log_page(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvme_transport_id cmd_source_trid;
	uint64_t offset, len;
	uint32_t rae, numdl, numdu;
	uint8_t lid = cmd->cdw10_bits.get_log_page.lid;
	int rc = 0;

	if (req->iovcnt < 1) {
		SPDK_DEBUGLOG(nvmf, "get log command with no buffer\n");
		goto invalid_field_log_page;
	}

	if (is_log_page_ctrlr_nvm_scope(lid) &&
	    ((cmd->nsid != 0) && (cmd->nsid != SPDK_NVME_GLOBAL_NS_TAG))) {
		SPDK_ERRLOG("Invalid NSID: %u for log pages with a scope of NVM subsystem or controller: LID=0x%02X\n",
			    cmd->nsid, lid);
		goto invalid_field_log_page;
	}


	offset = (uint64_t)cmd->cdw12 | ((uint64_t)cmd->cdw13 << 32);
	if (offset & 3) {
		SPDK_ERRLOG("Invalid log page offset 0x%" PRIx64 ", Offset must be 4-byte aligned\n", offset);
		goto invalid_field_log_page;
	}

	rae = cmd->cdw10_bits.get_log_page.rae;
	numdl = cmd->cdw10_bits.get_log_page.numdl;
	numdu = cmd->cdw11_bits.get_log_page.numdu;
	len = ((numdu << 16) + numdl + (uint64_t)1) * 4;
	if (len > req->length) {
		SPDK_ERRLOG("Get log page: len (%" PRIu64 ") > buf size (%u)\n",
			    len, req->length);
		goto invalid_field_log_page;
	}

	lid = cmd->cdw10_bits.get_log_page.lid;
	SPDK_DEBUGLOG(nvmf, "Get log page: LID=0x%02X offset=0x%" PRIx64 " len=0x%" PRIx64 " rae=%u\n",
		      lid, offset, len, rae);

	if (spdk_nvmf_subsystem_is_discovery(subsystem)) {
		switch (lid) {
		/* If you are adding support for new log pages, please update g_supported_log_pages_discover[] to reflect it. */
		case SPDK_NVME_LOG_SUPPORTED_LOG_PAGES:
			rc = nvmf_get_supported_log_pages(ctrlr, req->iov, req->iovcnt, offset, len);
			break;
		case SPDK_NVME_LOG_DISCOVERY:
			if (spdk_nvmf_qpair_get_listen_trid(req->qpair, &cmd_source_trid)) {
				SPDK_ERRLOG("Failed to get LOG_DISCOVERY source trid\n");
				response->status.sct = SPDK_NVME_SCT_GENERIC;
				response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			}

			nvmf_get_discovery_log_page_async(req, offset, len, &cmd_source_trid, rae);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		case SPDK_NVME_LOG_FEATURE_IDS_EFFECTS:
			rc = nvmf_get_feature_ids_effects_log_page(ctrlr, req->iov, req->iovcnt, offset, len);
			break;
		default:
			SPDK_INFOLOG(nvmf, "Unsupported Get Log Page Identifier for discovery subsystem 0x%02X\n", lid);
			goto invalid_field_log_page;
		}

		if (rc) {
			goto invalid_field_log_page;
		}
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	switch (lid) {
	/* If you are adding support for new log pages, please update g_supported_log_pages[] to reflect it. */
	/* [한국어] 새 LID를 추가할 때는 반드시 g_supported_log_pages[] 테이블도 업데이트할 것 */
	case SPDK_NVME_LOG_SUPPORTED_LOG_PAGES:
		/* [한국어] LID=00h: 지원하는 Log Page 목록 반환 */
		rc = nvmf_get_supported_log_pages(ctrlr, req->iov, req->iovcnt, offset, len);
		break;
	case SPDK_NVME_LOG_ERROR:
		/* [한국어] LID=01h: 오류 정보 로그 반환 */
		nvmf_get_error_log_page(ctrlr, req->iov, req->iovcnt, offset, len, rae);
		break;
	case SPDK_NVME_LOG_HEALTH_INFORMATION:
		/* TODO: actually fill out log page data */
		/* [한국어] LID=02h: SMART/Health 정보 — 현재 미구현, 0-fill 버퍼 반환 */
		break;
	case SPDK_NVME_LOG_FIRMWARE_SLOT:
		/* [한국어] LID=03h: 펌웨어 슬롯 정보 반환 */
		rc = nvmf_get_firmware_slot_log_page(req->iov, req->iovcnt, offset, len);
		break;
	case SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS:
		/* [한국어] LID=0Ch: ANA(Asymmetric Namespace Access) 로그 — 멀티패스 상태 */
		if (subsystem->flags.ana_reporting) {
			/* [한국어] ANA reporting이 활성화된 경우만 ANA 로그 반환 */
			uint32_t rgo = cmd->cdw10_bits.get_log_page.lsp & 1; /* [한국어] RGO(Return Groups Only): LSP 비트0=1이면 그룹만 반환 */
			rc = nvmf_get_ana_log_page(ctrlr, req->iov, req->iovcnt, offset, len, rae, rgo);
			break;
		} else {
			/* [한국어] ANA reporting 비활성화 시 → invalid_field 오류 반환 */
			SPDK_INFOLOG(nvmf, "Get Log Page for Asymmetric Namespace Access is not supported\n");
			goto invalid_field_log_page;
		}
	case SPDK_NVME_LOG_COMMAND_EFFECTS_LOG:
		/* [한국어] LID=05h: Commands Supported and Effects 로그 반환 */
		rc = nvmf_get_cmds_and_effects_log_page(ctrlr, req->iov, req->iovcnt, offset, len);
		break;
	case SPDK_NVME_LOG_CHANGED_NS_LIST:
		/* [한국어] LID=04h: 변경된 Namespace 목록 반환 — AEN 후 호스트가 읽음 */
		rc = nvmf_get_changed_ns_list_log_page(ctrlr, req->iov, req->iovcnt, offset, len, rae);
		break;
	case SPDK_NVME_LOG_RESERVATION_NOTIFICATION:
		/* [한국어] LID=80h: Reservation Notification 로그 반환 — Reservation 이벤트 후 호스트가 읽음 */
		rc = nvmf_get_reservation_notification_log_page(ctrlr, req->iov, req->iovcnt, offset, len, rae);
		break;
	case SPDK_NVME_LOG_FEATURE_IDS_EFFECTS:
		/* [한국어] LID=12h: Feature IDs Supported and Effects 로그 반환 */
		rc = nvmf_get_feature_ids_effects_log_page(ctrlr, req->iov, req->iovcnt, offset, len);
		break;
	case SPDK_NVME_LOG_NVME_MI_COMMANDS_EFFECTS:
		/* To comply with id-ctrl LPA bit 5, don't fail this log but return zeroed buffer instead. */
		/* [한국어] LID=13h: NVMe-MI 명령 효과 로그 — LPA bit5 준수를 위해 0-fill 버퍼 반환(오류 아님) */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	default:
		/* [한국어] 미지원 LID → invalid_field 오류 응답 */
		SPDK_INFOLOG(nvmf, "Unsupported Get Log Page Identifier 0x%02X\n", lid);
		goto invalid_field_log_page;
	}

	if (!rc) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

invalid_field_log_page:
	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * _nvmf_ctrlr_get_ns_safe - NSID 유효성 검사 후 NS 포인터 반환
 *
 * @ctrlr: 대상 컨트롤러.
 * @nsid: 요청된 Namespace ID (1-based).
 * @rsp: 오류 발생 시 상태 코드를 설정할 CQE 응답 포인터.
 * @return: 유효한 spdk_nvmf_ns* 또는 NULL (비활성/오류).
 *
 * nsid=0 또는 max_nsid 초과이면 INVALID_NAMESPACE_OR_FORMAT.
 * nsid가 범위 내지만 NS가 없거나 bdev가 없으면 비활성 NS로 SUCCESS + NULL 반환
 * (NVMe 스펙상 비활성 NS는 zero-filled 응답이 정상이므로).
 * 버퍼는 이미 nvmf_ctrlr_process_admin_cmd()에서 zeroed되어 있어 NULL 반환만으로 충분.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify_ns → 본 함수 → nvmf_ctrlr_get_ns
 */
static struct spdk_nvmf_ns *
_nvmf_ctrlr_get_ns_safe(struct spdk_nvmf_ctrlr *ctrlr,
			uint32_t nsid,
			struct spdk_nvme_cpl *rsp)
{
	struct spdk_nvmf_ns *ns; /* [한국어] 반환할 네임스페이스 포인터 */
	if (nsid == 0 || nsid > ctrlr->subsys->max_nsid) {
		/* [한국어] nsid=0은 예약값, max_nsid 초과는 범위 오류 */
		SPDK_ERRLOG("Identify Namespace for invalid NSID %u\n", nsid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return NULL;
	}

	ns = nvmf_ctrlr_get_ns(ctrlr, nsid); /* [한국어] subsystem->ns[nsid-1] 접근 */
	if (ns == NULL || ns->bdev == NULL) {
		/*
		 * Inactive namespaces should return a zero filled data structure.
		 * The data buffer is already zeroed by nvmf_ctrlr_process_admin_cmd(),
		 * so we can just return early here.
		 */
		/* [한국어] 비활성 NS는 SUCCESS + zero-filled 버퍼를 반환하는 것이 NVMe 스펙
		 * 버퍼는 이미 zeroed이므로 NULL 반환만으로 처리 완료 */
		SPDK_DEBUGLOG(nvmf, "Identify Namespace for inactive NSID %u\n", nsid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_SUCCESS;
		return NULL;
	}
	return ns; /* [한국어] 유효한 활성 NS 포인터 반환 */
}

/*
 * [한국어]
 * nvmf_ctrlr_identify_ns - Identify Namespace(CNS=00h) 응답 데이터 채우기
 *
 * @ctrlr: 요청한 컨트롤러.
 * @cmd: NVMe SQE (nsid 포함).
 * @rsp: CQE 응답.
 * @nsdata: 결과를 저장할 Identify Namespace 데이터 구조체.
 * @nsid: 조회할 Namespace ID.
 *
 * bdev에서 NS 정보를 채우고, ANA reporting이 활성화된 경우 anagrpid와
 * ANA 상태도 반영한다. INACCESSIBLE 또는 PERSISTENT_LOSS 상태이면 nuse=0으로 설정한다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify → 본 함수 → _nvmf_ctrlr_get_ns_safe,
 *                                    nvmf_bdev_ctrlr_identify_ns,
 *                                    nvmf_ctrlr_get_ana_state
 */
static void
nvmf_ctrlr_identify_ns(struct spdk_nvmf_ctrlr *ctrlr,
		       struct spdk_nvme_cmd *cmd,
		       struct spdk_nvme_cpl *rsp,
		       struct spdk_nvme_ns_data *nsdata,
		       uint32_t nsid)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys; /* [한국어] NS가 속한 subsystem */
	struct spdk_nvmf_ns *ns; /* [한국어] 조회 대상 NS */
	enum spdk_nvme_ana_state ana_state; /* [한국어] NS의 ANA 상태 */

	ns = _nvmf_ctrlr_get_ns_safe(ctrlr, nsid, rsp); /* [한국어] NSID 검증 + NS 포인터 획득 */
	if (ns == NULL) {
		return; /* [한국어] 비활성 NS이거나 오류 → zero-filled 버퍼 그대로 반환 */
	}

	/* [한국어] bdev에서 Identify Namespace 데이터 채우기
	 * (NSZE, NCAP, NLBAF, LBA Format 등의 bdev 특화 필드 반영) */
	nvmf_bdev_ctrlr_identify_ns(ns, nsdata, ctrlr->dif_insert_or_strip,
				    ctrlr->admin_qpair->transport->opts.max_io_size);

	assert(ctrlr->admin_qpair); /* [한국어] admin_qpair는 항상 유효해야 함 */

	if (subsystem->flags.ana_reporting) {
		/* [한국어] ANA 멀티패스 기능이 활성화된 경우 anagrpid와 ANA 상태 반영 */
		assert(ns->anagrpid - 1 < subsystem->max_nsid); /* [한국어] ANA 그룹 ID는 1-based, max_nsid 이내여야 함 */
		nsdata->anagrpid = ns->anagrpid; /* [한국어] NS가 속한 ANA Group ID 설정 */

		ana_state = nvmf_ctrlr_get_ana_state(ctrlr, ns->anagrpid); /* [한국어] 현재 ANA 상태 조회 */
		if (ana_state == SPDK_NVME_ANA_INACCESSIBLE_STATE ||
		    ana_state == SPDK_NVME_ANA_PERSISTENT_LOSS_STATE) {
			/* [한국어] INACCESSIBLE/PERSISTENT_LOSS: 이 경로에서 NS 사용 불가 → nuse=0 */
			nsdata->nuse = 0;
		}
	}
}

/*
 * [한국어]
 * spdk_nvmf_ctrlr_identify_ns - Identify Namespace(CNS=00h) 공개 API
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: NVMe SQE.
 * @rsp: CQE 응답.
 * @nsdata: 결과 저장 버퍼.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * nvmf_ctrlr_identify_ns()의 래퍼로 cmd->nsid를 전달한다.
 * transport 또는 passthrough 경로에서 호출된다.
 */
int
spdk_nvmf_ctrlr_identify_ns(struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvme_cmd *cmd,
			    struct spdk_nvme_cpl *rsp,
			    struct spdk_nvme_ns_data *nsdata)
{
	nvmf_ctrlr_identify_ns(ctrlr, cmd, rsp, nsdata, cmd->nsid); /* [한국어] cmd->nsid로 NS Identify 수행 */

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * identify_ns_passthru_cb - Identify Namespace passthrough 완료 콜백
 *
 * @req: Identify Namespace 요청.
 *
 * NVMe 드라이브에서 받은 Identify NS 데이터(nvme_nsdata)와
 * NVMe-oF fabric 레이어에서 계산한 nsdata(nvmf_nsdata)를 병합하여
 * 호스트에게 전달할 최종 응답을 생성한다.
 *
 * 병합 규칙:
 *   - optperf 관련 필드(npwg/npwa/npdg/npda/nows): SSD 값 우선
 *   - 원자 쓰기 단위(nawun/nawupf/nacwu): SSD 값 우선
 *   - 경계 정렬 필드(nabsn/nabo/nabspf): SSD 값 사용
 *   - 그 외(NSZE, NCAP, LBA format 등): nvmf 계산값 유지
 *
 * 실행 컨텍스트: bdev 완료 콜백 → ctrlr->thread.
 *
 * 호출 체인:
 *   spdk_nvmf_ctrlr_identify_ns_ext → spdk_nvmf_bdev_ctrlr_nvme_passthru_admin → [이 함수]
 */
static void
identify_ns_passthru_cb(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req); /* [한국어] NVMe 명령 (orig_nsid 확인용) */
	struct spdk_nvmf_ctrlr *ctrlr = spdk_nvmf_request_get_ctrlr(req); /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cpl *rsp = spdk_nvmf_request_get_response(req); /* [한국어] CQE 응답 */
	struct spdk_nvme_ns_data nvmf_nsdata = {}; /* [한국어] fabric에서 계산한 NS 데이터 */
	struct spdk_nvme_ns_data nvme_nsdata = {}; /* [한국어] NVMe 드라이브에서 받은 NS 데이터 */
	size_t datalen; /* [한국어] 실제 복사된 바이트 수 */

	/* This is the identify data from the NVMe drive */
	/* [한국어] 드라이브에서 받은 Identify NS 데이터를 nvme_nsdata로 복사 */
	datalen = spdk_nvmf_request_copy_to_buf(req, &nvme_nsdata,
						sizeof(nvme_nsdata));
	/* [한국어] orig_nsid를 사용해 fabric 레이어 NS 데이터 계산 */
	nvmf_ctrlr_identify_ns(ctrlr, cmd, rsp, &nvmf_nsdata, req->orig_nsid);

	/* Update fabric's namespace according to SSD's namespace */
	if (nvme_nsdata.nsfeat.optperf) {
		/* [한국어] SSD가 최적 성능 힌트를 지원하면 SSD 값으로 덮어씀 */
		nvmf_nsdata.nsfeat.optperf = nvme_nsdata.nsfeat.optperf; /* [한국어] optperf 플래그 */
		nvmf_nsdata.npwg = nvme_nsdata.npwg; /* [한국어] 최적 쓰기 단위 (Namespace Preferred Write Granularity) */
		nvmf_nsdata.npwa = nvme_nsdata.npwa; /* [한국어] 최적 쓰기 정렬 (Namespace Preferred Write Alignment) */
		nvmf_nsdata.npdg = nvme_nsdata.npdg; /* [한국어] 최적 해제 단위 (Namespace Preferred Deallocate Granularity) */
		nvmf_nsdata.npda = nvme_nsdata.npda; /* [한국어] 최적 해제 정렬 (Namespace Preferred Deallocate Alignment) */
		nvmf_nsdata.nows = nvme_nsdata.nows; /* [한국어] 최적 쓰기 크기 (Namespace Optimal Write Size) */
	}

	if (nvme_nsdata.nsfeat.ns_atomic_write_unit) {
		/* [한국어] SSD가 NS 단위 원자 쓰기 크기를 지원하면 SSD 값 사용 */
		nvmf_nsdata.nsfeat.ns_atomic_write_unit = nvme_nsdata.nsfeat.ns_atomic_write_unit; /* [한국어] 플래그 */
		nvmf_nsdata.nawun = nvme_nsdata.nawun; /* [한국어] NS 원자 쓰기 단위 (Namespace Atomic Write Unit Normal) */
		nvmf_nsdata.nawupf = nvme_nsdata.nawupf; /* [한국어] NS 원자 쓰기 단위 (Power Fail) */
		nvmf_nsdata.nacwu = nvme_nsdata.nacwu; /* [한국어] NS 원자 Compare+Write 단위 */
	}

	nvmf_nsdata.nabsn = nvme_nsdata.nabsn; /* [한국어] 원자 경계 크기 Normal (Namespace Atomic Boundary Size Normal) */
	nvmf_nsdata.nabo = nvme_nsdata.nabo; /* [한국어] 원자 경계 오프셋 (Namespace Atomic Boundary Offset) */
	nvmf_nsdata.nabspf = nvme_nsdata.nabspf; /* [한국어] 원자 경계 크기 Power Fail */

	/* [한국어] 병합된 nsdata를 iov에 기록하여 호스트에게 전달 */
	spdk_nvmf_request_copy_from_buf(req, &nvmf_nsdata, datalen);
}

/*
 * [한국어]
 * spdk_nvmf_ctrlr_identify_ns_ext - Identify Namespace extended (NS passthrough 포함)
 *
 * @req: Identify Namespace 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE/ASYNCHRONOUS.
 *
 * 기본적으로 nvmf_ctrlr_identify_ns()로 fabric NS 데이터를 채운다.
 * 해당 NS가 NVMe bdev passthrough를 지원하면 SSD에서 Identify NS를 실행하고
 * identify_ns_passthru_cb에서 SSD 값을 병합하여 호스트에게 반환한다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify → [이 함수] → spdk_nvmf_bdev_ctrlr_nvme_passthru_admin → identify_ns_passthru_cb
 */
int
spdk_nvmf_ctrlr_identify_ns_ext(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req); /* [한국어] NVMe SQE */
	struct spdk_nvmf_ctrlr *ctrlr = spdk_nvmf_request_get_ctrlr(req); /* [한국어] 현재 컨트롤러 */
	struct spdk_nvmf_ns *ns = nvmf_ctrlr_get_ns(ctrlr, cmd->nsid); /* [한국어] NSID → NS 조회 */
	struct spdk_nvme_cpl *rsp = spdk_nvmf_request_get_response(req); /* [한국어] CQE 응답 */
	struct spdk_bdev *bdev; /* [한국어] NS의 bdev */
	struct spdk_bdev_desc *desc; /* [한국어] NS의 bdev descriptor */
	struct spdk_io_channel *ch; /* [한국어] bdev I/O channel */
	struct spdk_nvme_ns_data nsdata = {}; /* [한국어] fabric 계산 NS 데이터 */
	struct spdk_iov_xfer ix; /* [한국어] iov 전송 컨텍스트 */
	int rc; /* [한국어] bdev 조회 결과 */

	/* [한국어] fabric 레이어 NS 데이터 계산 */
	nvmf_ctrlr_identify_ns(ctrlr, cmd, rsp, &nsdata, cmd->nsid);

	/* [한국어] NS의 bdev/desc/channel 조회 */
	rc = spdk_nvmf_request_get_bdev(cmd->nsid, req, &bdev, &desc, &ch);
	if (rc) {
		/* [한국어] bdev 없음 → 이미 채운 fabric nsdata 반환 */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_ADMIN)) {
		/* [한국어] NVMe Admin passthrough 미지원 → fabric nsdata 직접 반환 */
		spdk_iov_xfer_init(&ix, req->iov, req->iovcnt); /* [한국어] iov 초기화 */
		spdk_iov_xfer_from_buf(&ix, &nsdata, sizeof(nsdata)); /* [한국어] nsdata → iov 복사 */

		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	assert(ns->passthru_nsid != 0); /* [한국어] passthrough NS는 passthru_nsid가 0이어선 안 됨 */
	req->orig_nsid = ns->nsid; /* [한국어] 원래 NSID를 저장 (콜백에서 fabric nsdata 재계산에 사용) */
	cmd->nsid = ns->passthru_nsid; /* [한국어] cmd의 NSID를 bdev NVMe 장치의 NSID로 교체 */

	/* [한국어] NVMe 드라이브에 Identify NS 요청 전달, 콜백에서 결과 병합 */
	return spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(bdev, desc, ch, req, identify_ns_passthru_cb);
}

/*
 * [한국어]
 * nvmf_ctrlr_populate_oacs - Identify Controller의 OACS(Optional Admin Command Support) 필드 채우기
 *
 * @ctrlr: 대상 컨트롤러.
 * @cdata: 결과 저장 Identify Controller 응답 구조체.
 *
 * g_nvmf_custom_admin_cmd_hdlrs 테이블에 등록된 핸들러 유무를 기반으로
 * OACS 비트필드의 각 선택적 Admin 명령 지원 여부를 설정한다.
 * 기본값은 ctrlr->cdata.oacs에서 가져오며, 사용자 정의 핸들러로 덮어쓴다.
 *
 * 실행 컨텍스트: ctrlr->thread (Identify Controller 처리 중).
 *
 * 호출 체인:
 *   spdk_nvmf_ctrlr_identify_ctrlr → [이 함수]
 */
static void
nvmf_ctrlr_populate_oacs(struct spdk_nvmf_ctrlr *ctrlr,
			 struct spdk_nvme_ctrlr_data *cdata)
{
	cdata->oacs = ctrlr->cdata.oacs; /* [한국어] 기본 OACS 값 복사 (nvmf_ctrlr_cdata_init에서 설정) */

	/* [한국어] Virtualization Management(VMs): 핸들러가 등록된 경우 지원 선언 */
	cdata->oacs.vms =
		g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_VIRTUALIZATION_MANAGEMENT].hdlr != NULL;
	/* [한국어] NVMe-MI Send/Receive(NSRs): 두 명령 모두 핸들러가 있어야 함 */
	cdata->oacs.nsrs = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_NVME_MI_SEND].hdlr != NULL
			   && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_NVME_MI_RECEIVE].hdlr != NULL;
	/* [한국어] Directive Send/Receive(DIRs): 두 명령 모두 핸들러가 있어야 함 */
	cdata->oacs.dirs = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_DIRECTIVE_SEND].hdlr != NULL
			   && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_DIRECTIVE_RECEIVE].hdlr != NULL;
	/* [한국어] Device Self-Test(DSTs): 핸들러 등록 여부 */
	cdata->oacs.dsts =
		g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_DEVICE_SELF_TEST].hdlr != NULL;
	/* [한국어] Namespace Management/Attachment(NMs): 두 명령 모두 핸들러가 있어야 함 */
	cdata->oacs.nms = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_NS_MANAGEMENT].hdlr != NULL
			  && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_NS_ATTACHMENT].hdlr != NULL;
	/* [한국어] Firmware Image Download/Commit(FWDs): 두 명령 모두 핸들러가 있어야 함 */
	cdata->oacs.fwds = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD].hdlr !=
			   NULL
			   && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_FIRMWARE_COMMIT].hdlr != NULL;
	/* [한국어] Format NVM(FNVMs): Format 명령 핸들러 등록 여부 */
	cdata->oacs.fnvms =
		g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_FORMAT_NVM].hdlr != NULL;
	/* [한국어] Security Send/Receive(SSRs): 두 명령 모두 핸들러가 있어야 함 */
	cdata->oacs.ssrs = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_SECURITY_SEND].hdlr != NULL
			   && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_SECURITY_RECEIVE].hdlr != NULL;
	/* [한국어] Get LBA Status(GLSSs): 핸들러 등록 여부 */
	cdata->oacs.glss = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_GET_LBA_STATUS].hdlr !=
			   NULL;
}

/*
 * [한국어]
 * spdk_nvmf_ctrlr_identify_ctrlr - Identify Controller(CNS=01h) 응답 생성
 *
 * @ctrlr: 대상 컨트롤러.
 * @cdata: 결과 저장 버퍼 (호출자가 zeroed 상태로 전달).
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * NVMe Identify Controller(CNS=01h) 명령에 대한 응답을 cdata에 채운다.
 * Discovery 서브시스템과 NVM 서브시스템에서 공통 필드와 각각 전용 필드를 채운다.
 * 공통 필드: FR(Firmware Rev), MDTS, CNTLID, VER, AERL, LPA, ELPE, MAXCMD, SGLS, FUSES 등.
 * NVM 전용 필드: MN, SN, KAS, SQES/CQES, NN, VWC, NVMf-specific, ONCS, ANA 관련 등.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify → [이 함수] → nvmf_ctrlr_populate_oacs
 */
int
spdk_nvmf_ctrlr_identify_ctrlr(struct spdk_nvmf_ctrlr *ctrlr, struct spdk_nvme_ctrlr_data *cdata)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys; /* [한국어] 컨트롤러의 subsystem */
	struct spdk_nvmf_transport *transport; /* [한국어] admin qpair의 transport (MDTS, MAXCMD 계산용) */

	/*
	 * Common fields for discovery and NVM subsystems
	 */
	assert(ctrlr->admin_qpair); /* [한국어] admin_qpair가 없으면 안 됨 */
	transport = ctrlr->admin_qpair->transport; /* [한국어] admin qpair가 사용하는 transport */
	spdk_strcpy_pad(cdata->fr, FW_VERSION, sizeof(cdata->fr), ' '); /* [한국어] FR: 펌웨어 버전 (공백 패딩) */
	assert((transport->opts.max_io_size % 4096) == 0); /* [한국어] max_io_size는 4KB 배수여야 함 */
	cdata->mdts = spdk_u32log2(transport->opts.max_io_size / 4096); /* [한국어] MDTS: 최대 데이터 전송 크기 (4KB 단위 log2) */
	cdata->cntlid = ctrlr->cntlid; /* [한국어] CNTLID: 컨트롤러 ID */
	cdata->ver = ctrlr->vcprop.vs; /* [한국어] VER: NVMe 버전 레지스터 값 */
	cdata->aerl = ctrlr->cdata.aerl; /* [한국어] AERL: 지원하는 AER(Async Event Request) 최대 수 */
	cdata->lpa.lpeds = 1; /* [한국어] LPA.LPEDS: 로그 페이지 디스크립터 지원 */
	cdata->elpe = 127; /* [한국어] ELPE: 오류 로그 페이지 엔트리 수 (최대값 128-1) */
	cdata->maxcmd = transport->opts.max_queue_depth; /* [한국어] MAXCMD: 최대 큐 깊이 */
	cdata->sgls = ctrlr->cdata.sgls; /* [한국어] SGLS: SGL(Scatter-Gather List) 지원 여부 */
	cdata->fuses = ctrlr->cdata.fuses; /* [한국어] FUSES: Fused 명령(Compare+Write) 지원 여부 */
	cdata->acwu = 0; /* ACWU is 0-based. */ /* [한국어] ACWU: 원자 Compare+Write 단위 (0=1 논리 블록) */
	cdata->wctemp = 0x0157; /* Recommended value from NVMe 1.3d spec. */ /* [한국어] WCTEMP: 경고 온도 (0x157 = 343K) */
	cdata->cctemp = 0x0175; /* [한국어] CCTEMP: 임계 온도 (0x175 = 373K = 100°C) */
	if (subsystem->flags.ana_reporting) {
		/* [한국어] ANA 멀티패스 지원 시 MNAN(Maximum NS Attachments per ANA group) = max_nsid */
		cdata->mnan = subsystem->max_nsid;
	}
	spdk_strcpy_pad(cdata->subnqn, subsystem->subnqn, sizeof(cdata->subnqn), '\0'); /* [한국어] SUBNQN: subsystem NQN (NULL 패딩) */

	SPDK_DEBUGLOG(nvmf, "ctrlr data: maxcmd 0x%x\n", cdata->maxcmd);
	SPDK_DEBUGLOG(nvmf, "sgls data: 0x%x\n", from_le32(&cdata->sgls));


	if (spdk_nvmf_subsystem_is_discovery(subsystem)) {
		/*
		 * NVM Discovery subsystem fields
		 */
		/* [한국어] Discovery 서브시스템: 디스커버리 로그 변경 알림 이벤트 지원 */
		cdata->oaes.discovery_log_change_notices = 1;
		cdata->cntrltype = SPDK_NVME_CTRLR_DISCOVERY; /* [한국어] 컨트롤러 타입: Discovery */
	} else {
		/* [한국어] NVM 서브시스템 전용 필드 */
		cdata->vid = ctrlr->cdata.vid; /* [한국어] VID: PCI 벤더 ID */
		cdata->ssvid = ctrlr->cdata.ssvid; /* [한국어] SSVID: PCI 서브시스템 벤더 ID */
		cdata->ieee[0] = ctrlr->cdata.ieee[0]; /* [한국어] IEEE OUI 바이트 0 */
		cdata->ieee[1] = ctrlr->cdata.ieee[1]; /* [한국어] IEEE OUI 바이트 1 */
		cdata->ieee[2] = ctrlr->cdata.ieee[2]; /* [한국어] IEEE OUI 바이트 2 */

		/*
		 * NVM subsystem fields (reserved for discovery subsystems)
		 */
		/* [한국어] MN: 모델명 (공백 패딩) */
		spdk_strcpy_pad(cdata->mn, spdk_nvmf_subsystem_get_mn(subsystem), sizeof(cdata->mn), ' ');
		/* [한국어] SN: 시리얼 번호 (공백 패딩) */
		spdk_strcpy_pad(cdata->sn, spdk_nvmf_subsystem_get_sn(subsystem), sizeof(cdata->sn), ' ');
		cdata->kas = ctrlr->cdata.kas; /* [한국어] KAS: Keep Alive Support (100ms 단위) */

		cdata->rab = 6; /* [한국어] RAB: Recommended Arbitration Burst = 2^6 = 64 */
		cdata->cmic.mports = 1; /* [한국어] CMIC.MPORTS: 다중 포트 지원 */
		cdata->cmic.mctrs = 1; /* [한국어] CMIC.MCTRS: 다중 컨트롤러 지원 */
		cdata->oaes.ns_attribute_notices = 1; /* [한국어] OAES: NS 속성 변경 알림 이벤트 지원 */
		cdata->ctratt.bits.host_id_exhid_supported = 1; /* [한국어] 128비트 호스트 ID(Host Identifier Extended) 지원 */
		cdata->ctratt.bits.fdps = ctrlr->subsys->fdp_supported; /* [한국어] FDP(Flexible Data Placement) 지원 여부 */
		cdata->cntrltype = SPDK_NVME_CTRLR_IO; /* [한국어] 컨트롤러 타입: I/O */
		/* We do not have any actual limitation to the number of abort commands.
		 * We follow the recommendation by the NVMe specification.
		 */
		cdata->acl = NVMF_ABORT_COMMAND_LIMIT; /* [한국어] ACL: Abort Command Limit (스펙 권장값) */
		cdata->frmw.slot1_ro = 1; /* [한국어] FRMW: 슬롯1은 읽기 전용 */
		cdata->frmw.num_slots = 1; /* [한국어] FRMW: 펌웨어 슬롯 수 = 1 */

		cdata->lpa.cses = 1; /* Command Effects log page supported */ /* [한국어] LPA.CSES: Command Effects Log 지원 */
		cdata->lpa.mlps = 1; /* [한국어] LPA.MLPS: Media Error Log Page 지원 */

		cdata->sqes.min = 6; /* [한국어] SQES: SQE(Submission Queue Entry) 최소 크기 = 2^6 = 64B */
		cdata->sqes.max = 6; /* [한국어] SQES: SQE 최대 크기 = 2^6 = 64B */
		cdata->cqes.min = 4; /* [한국어] CQES: CQE(Completion Queue Entry) 최소 크기 = 2^4 = 16B */
		cdata->cqes.max = 4; /* [한국어] CQES: CQE 최대 크기 = 2^4 = 16B */
		cdata->nn = subsystem->max_nsid; /* [한국어] NN: 지원하는 최대 Namespace 수 */
		cdata->vwc.present = 1; /* [한국어] VWC: Volatile Write Cache 존재 */
		cdata->vwc.flush_broadcast = SPDK_NVME_FLUSH_BROADCAST_NOT_SUPPORTED; /* [한국어] VWC: Flush Broadcast 미지원 */

		cdata->nvmf_specific = ctrlr->cdata.nvmf_specific; /* [한국어] NVMe-oF specific 필드 (ioccsz, iorcsz, icdoff 등) */

		cdata->oncs.nvmcmps = ctrlr->cdata.oncs.nvmcmps; /* [한국어] ONCS: Compare 명령 지원 여부 */
		/* [한국어] ONCS: Dataset Management(DSM) = 드라이버 지원 && 모든 NS가 DSM 지원할 때 */
		cdata->oncs.nvmdsmsv = ctrlr->cdata.oncs.nvmdsmsv && nvmf_ctrlr_dsm_supported(ctrlr);
		/* [한국어] ONCS: Write Zeroes = 드라이버 지원 && 모든 NS가 Write Zeroes 지원할 때 */
		cdata->oncs.nvmwzsv = ctrlr->cdata.oncs.nvmwzsv &&
				      nvmf_ctrlr_write_zeroes_supported(ctrlr);
		cdata->oncs.reservs = ctrlr->cdata.oncs.reservs; /* [한국어] ONCS: Reservation 지원 여부 */
		cdata->oncs.nvmcpys = ctrlr->cdata.oncs.nvmcpys; /* [한국어] ONCS: Copy(NVM Copy) 명령 지원 여부 */
		cdata->ocfs.copy_format0 = cdata->oncs.nvmcpys; /* [한국어] OCFS: Copy Format 0(SRC/DST 기술자) 지원 = NVM Copy와 동일 */
		if (subsystem->flags.ana_reporting) {
			/* Asymmetric Namespace Access Reporting is supported. */
			/* [한국어] ANA 멀티패스 지원 시 관련 CMIC/OAES/ANACAP 필드 설정 */
			cdata->cmic.anars = 1; /* [한국어] CMIC.ANARS: ANA Reporting 지원 */
			cdata->oaes.ana_change_notices = 1; /* [한국어] OAES: ANA 상태 변경 알림 이벤트 지원 */

			cdata->anatt = ANA_TRANSITION_TIME_IN_SEC; /* [한국어] ANATT: ANA 상태 전환 최대 시간 (초) */
			/* ANA Change state is not used, and ANA Persistent Loss state
			 * is not supported for now.
			 */
			cdata->anacap.ana_optimized_state = 1; /* [한국어] ANACAP: Optimized 상태 지원 */
			cdata->anacap.ana_non_optimized_state = 1; /* [한국어] ANACAP: Non-Optimized 상태 지원 */
			cdata->anacap.ana_inaccessible_state = 1; /* [한국어] ANACAP: Inaccessible 상태 지원 */
			/* ANAGRPID does not change while namespace is attached to controller */
			cdata->anacap.no_change_anagrpid = 1; /* [한국어] ANACAP: ANAGRPID는 컨트롤러 연결 중 변경되지 않음 */
			cdata->anagrpmax = subsystem->max_nsid; /* [한국어] ANAGRPMAX: 최대 ANA 그룹 수 = max_nsid */
			cdata->nanagrpid = subsystem->max_nsid; /* [한국어] NANAGRPID: 지원하는 ANA 그룹 ID 수 = max_nsid */
		}

		/* [한국어] OACS: 선택적 Admin 명령 지원 비트필드 채우기 */
		nvmf_ctrlr_populate_oacs(ctrlr, cdata);

		assert(subsystem->tgt != NULL); /* [한국어] tgt가 없으면 안 됨 */
		cdata->crdt[0] = subsystem->tgt->crdt[0]; /* [한국어] CRDT[0]: Command Retry Delay Time 1 */
		cdata->crdt[1] = subsystem->tgt->crdt[1]; /* [한국어] CRDT[1]: Command Retry Delay Time 2 */
		cdata->crdt[2] = subsystem->tgt->crdt[2]; /* [한국어] CRDT[2]: Command Retry Delay Time 3 */

		SPDK_DEBUGLOG(nvmf, "ext ctrlr data: ioccsz 0x%x\n",
			      cdata->nvmf_specific.ioccsz);
		SPDK_DEBUGLOG(nvmf, "ext ctrlr data: iorcsz 0x%x\n",
			      cdata->nvmf_specific.iorcsz);
		SPDK_DEBUGLOG(nvmf, "ext ctrlr data: icdoff 0x%x\n",
			      cdata->nvmf_specific.icdoff);
		SPDK_DEBUGLOG(nvmf, "ext ctrlr data: ctrattr 0x%x\n",
			      *(uint8_t *)&cdata->nvmf_specific.ctrattr);
		SPDK_DEBUGLOG(nvmf, "ext ctrlr data: msdbd 0x%x\n",
			      cdata->nvmf_specific.msdbd);
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ns_identify_iocs_zns - ZNS NS에 대한 I/O Command Set Specific Identify Namespace 응답 생성
 *
 * @ns: ZNS Namespace.
 * @cmd: NVMe SQE.
 * @rsp: CQE 응답.
 * @nsdata_zns: ZNS NS 데이터 응답 버퍼.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * ZNS(Zoned Namespace) NS에 대한 IOCS 특화 Identify NS 응답을 채운다.
 * mar/mor은 0-based이므로 bdev API 반환값에서 1을 뺀다(언더플로 시 FFFFFFFFh = 무제한).
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   spdk_nvmf_ns_identify_iocs_specific → [이 함수]
 */
static int
nvmf_ns_identify_iocs_zns(struct spdk_nvmf_ns *ns,
			  struct spdk_nvme_cmd *cmd,
			  struct spdk_nvme_cpl *rsp,
			  struct spdk_nvme_zns_ns_data *nsdata_zns)
{
	nsdata_zns->zoc.variable_zone_capacity = 0; /* [한국어] ZOC: 가변 존 용량 미지원 */
	nsdata_zns->zoc.zone_active_excursions = 0; /* [한국어] ZOC: 존 활성 초과 미지원 */
	nsdata_zns->ozcs.read_across_zone_boundaries = 1; /* [한국어] OZCS: 존 경계를 넘는 읽기 지원 */
	/* Underflowing the zero based mar and mor bdev helper results in the correct
	   value of FFFFFFFFh. */
	/* [한국어] MAR: Maximum Active Resources (0-based). bdev API 반환값=0이면 언더플로→FFFFFFFFh(무제한) */
	nsdata_zns->mar = spdk_bdev_get_max_active_zones(ns->bdev) - 1;
	/* [한국어] MOR: Maximum Open Resources (0-based). bdev API 반환값=0이면 언더플로→FFFFFFFFh(무제한) */
	nsdata_zns->mor = spdk_bdev_get_max_open_zones(ns->bdev) - 1;
	nsdata_zns->rrl = 0; /* [한국어] RRL: Reset Recommended Limit = 0 (제한 없음) */
	nsdata_zns->frl = 0; /* [한국어] FRL: Finish Recommended Limit = 0 (제한 없음) */
	nsdata_zns->lbafe[0].zsze = spdk_bdev_get_zone_size(ns->bdev); /* [한국어] LBAFE[0].ZSZE: 존 크기 (논리 블록 수) */

	rsp->status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] 성공 응답 */
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ns_identify_iocs_nvm - NVM NS에 대한 I/O Command Set Specific Identify Namespace 응답 생성
 *
 * @ns: NVM Namespace.
 * @rsp: CQE 응답.
 * @nsdata_nvm: NVM NS 데이터 응답 버퍼.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * NVM CSI NS에 대한 IOCS 특화 Identify NS 응답을 채운다.
 * bdev에서 복사 방향(copy format) 등의 필드를 채우는 nvmf_bdev_ctrlr_identify_iocs_nvm()를 호출.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   spdk_nvmf_ns_identify_iocs_specific → [이 함수] → nvmf_bdev_ctrlr_identify_iocs_nvm
 */
static int
nvmf_ns_identify_iocs_nvm(struct spdk_nvmf_ns *ns,
			  struct spdk_nvme_cpl *rsp,
			  struct spdk_nvme_nvm_ns_data *nsdata_nvm)
{
	nvmf_bdev_ctrlr_identify_iocs_nvm(ns, nsdata_nvm); /* [한국어] NVM IOCS NS 데이터 채우기 */

	rsp->status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] 성공 응답 */
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * spdk_nvmf_ns_identify_iocs_specific - IOCS specific Identify Namespace(CNS=05h) 처리
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: NVMe SQE (CSI 필드 포함).
 * @rsp: CQE 응답.
 * @nsdata: 결과 저장 버퍼.
 * @nsdata_size: 버퍼 크기.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * cmd->cdw11의 CSI 필드에 따라 ZNS/NVM IOCS 특화 NS 데이터를 반환한다.
 * DIF 삽입/제거 모드에서는 NVM IOCS NS 데이터를 지원하지 않는다(0으로 채워 반환).
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify → [이 함수] → nvmf_ns_identify_iocs_zns / nvmf_ns_identify_iocs_nvm
 */
int
spdk_nvmf_ns_identify_iocs_specific(struct spdk_nvmf_ctrlr *ctrlr,
				    struct spdk_nvme_cmd *cmd,
				    struct spdk_nvme_cpl *rsp,
				    void *nsdata,
				    size_t nsdata_size)
{
	uint8_t csi = cmd->cdw11_bits.identify.csi; /* [한국어] CSI: Command Set Identifier (ZNS=0x2, NVM=0x0) */
	struct spdk_nvmf_ns *ns = _nvmf_ctrlr_get_ns_safe(ctrlr, cmd->nsid, rsp); /* [한국어] NSID → NS 조회 */

	memset(nsdata, 0, nsdata_size); /* [한국어] 응답 버퍼 초기화 */

	if (ns == NULL) {
		/* [한국어] NS 없음 → INVALID_NAMESPACE_OR_FORMAT */
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	switch (csi) {
	case SPDK_NVME_CSI_ZNS:
		/* [한국어] ZNS CSI: ZNS NS Identify 데이터 반환 */
		return nvmf_ns_identify_iocs_zns(ns, cmd, rsp, nsdata);
	case SPDK_NVME_CSI_NVM:
		if (!ctrlr->dif_insert_or_strip) {
			/* [한국어] NVM CSI이고 DIF 모드가 아닐 때만 NVM NS Identify 데이터 반환 */
			return nvmf_ns_identify_iocs_nvm(ns, rsp, nsdata);
		}
		break; /* [한국어] DIF 모드이면 0-filled 버퍼 반환 */
	default:
		break; /* [한국어] 미지원 CSI: 0-filled 버퍼 반환 */
	}

	SPDK_DEBUGLOG(nvmf,
		      "Returning zero filled struct for the iocs specific ns "
		      "identify command and CSI 0x%02x\n",
		      csi);
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_identify_iocs_nvm - NVM Controller에 대한 IOCS specific Identify Controller 응답 생성
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: NVMe SQE.
 * @rsp: CQE 응답.
 * @cdata_nvm: NVM Controller 데이터 응답 버퍼.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * wzsl(Write Zeroes Size Limit)과 dmrsl(Dataset Management Range Size Limit)을 계산하여
 * NVM IOCS Identify Controller 데이터를 채운다.
 * 단위 변환: KiB → page size 단위 log2.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   spdk_nvmf_ctrlr_identify_iocs_specific → [이 함수]
 */
static int
nvmf_ctrlr_identify_iocs_nvm(struct spdk_nvmf_ctrlr *ctrlr,
			     struct spdk_nvme_cmd *cmd,
			     struct spdk_nvme_cpl *rsp,
			     struct spdk_nvme_nvm_ctrlr_data *cdata_nvm)
{
	/* The unit of max_write_zeroes_size_kib is KiB.
	 * The unit of wzsl is the minimum memory page size(2 ^ (12 + CAP.MPSMIN) bytes)
	 * and is reported as a power of two (2^n).
	 */
	/* [한국어] WZSL: Write Zeroes Size Limit. subsystem의 max_write_zeroes_size_kib(KiB)를
	 * page size 단위로 변환한 뒤 log2 취한 값 */
	cdata_nvm->wzsl = spdk_u64log2(ctrlr->subsys->max_write_zeroes_size_kib >>
				       (2 + ctrlr->vcprop.cap.bits.mpsmin));

	/* The unit of max_discard_size_kib is KiB.
	 * The dmrsl indicates the maximum number of logical blocks for
	 * dataset management command.
	 */
	/* [한국어] DMRSL: Dataset Management Range Size Limit. KiB 단위를 512B 단위 논리블록 수로 변환 (<<1 = *2) */
	cdata_nvm->dmrsl = ctrlr->subsys->max_discard_size_kib << 1;
	cdata_nvm->dmrl = 1; /* [한국어] DMRL: Dataset Management Range List 최대 항목 수 = 1 */

	rsp->status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] 성공 응답 */
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_identify_iocs_zns - ZNS Controller에 대한 IOCS specific Identify Controller 응답 생성
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: NVMe SQE.
 * @rsp: CQE 응답.
 * @cdata_zns: ZNS Controller 데이터 응답 버퍼.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * zasl(Zone Append Size Limit)을 계산하여 ZNS IOCS Identify Controller 데이터를 채운다.
 * 단위 변환: KiB → min memory page size 단위 log2.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   spdk_nvmf_ctrlr_identify_iocs_specific → [이 함수]
 */
static int
nvmf_ctrlr_identify_iocs_zns(struct spdk_nvmf_ctrlr *ctrlr,
			     struct spdk_nvme_cmd *cmd,
			     struct spdk_nvme_cpl *rsp,
			     struct spdk_nvme_zns_ctrlr_data *cdata_zns)
{
	/* The unit of max_zone_append_size_kib is KiB.
	The unit of zasl is the minimum memory page size
	(2 ^ (12 + CAP.MPSMIN) KiB)
	and is reported as a power of two (2^n). */
	/* [한국어] ZASL: Zone Append Size Limit. max_zone_append_size_kib(KiB)를
	 * min page size(2^(12+MPSMIN) bytes) 단위로 변환한 뒤 log2 취한 값 */
	cdata_zns->zasl = spdk_u64log2(ctrlr->subsys->max_zone_append_size_kib >>
				       (12 + ctrlr->vcprop.cap.bits.mpsmin));

	rsp->status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] 성공 응답 */
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * spdk_nvmf_ctrlr_identify_iocs_specific - IOCS specific Identify Controller(CNS=06h) 처리
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: NVMe SQE (CSI 필드 포함).
 * @rsp: CQE 응답.
 * @cdata: 결과 저장 버퍼.
 * @cdata_size: 버퍼 크기.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * cmd->cdw11의 CSI 필드에 따라 NVM/ZNS IOCS specific Controller Identify 데이터를 반환한다.
 * 미지원 CSI는 0으로 채워 SUCCESS를 반환한다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify → [이 함수] → nvmf_ctrlr_identify_iocs_nvm / nvmf_ctrlr_identify_iocs_zns
 */
int
spdk_nvmf_ctrlr_identify_iocs_specific(struct spdk_nvmf_ctrlr *ctrlr,
				       struct spdk_nvme_cmd *cmd,
				       struct spdk_nvme_cpl *rsp,
				       void *cdata,
				       size_t cdata_size)
{
	uint8_t csi = cmd->cdw11_bits.identify.csi; /* [한국어] CSI: Command Set Identifier */

	memset(cdata, 0, cdata_size); /* [한국어] 응답 버퍼 초기화 */

	switch (csi) {
	case SPDK_NVME_CSI_NVM:
		/* [한국어] NVM CSI: NVM Controller IOCS 데이터 반환 */
		return nvmf_ctrlr_identify_iocs_nvm(ctrlr, cmd, rsp, cdata);
	case SPDK_NVME_CSI_ZNS:
		/* [한국어] ZNS CSI: ZNS Controller IOCS 데이터 반환 */
		return nvmf_ctrlr_identify_iocs_zns(ctrlr, cmd, rsp, cdata);
	default:
		break; /* [한국어] 미지원 CSI: 0-filled 반환 */
	}

	SPDK_DEBUGLOG(nvmf,
		      "Returning zero filled struct for the iocs specific ctrlr "
		      "identify command and CSI 0x%02x\n",
		      csi);
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * spdk_nvmf_identify_ns_iocs_independent - I/O Command Set Independent Identify Namespace(CNS=08h) 처리
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: NVMe SQE (NSID 포함).
 * @rsp: CQE 응답.
 * @nsdata: 결과 저장 버퍼.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * NSID=FFFFFFFFh(GLOBAL_NS_TAG)이고 Namespace Management가 지원되면
 * 컨트롤러 공통 공유 NS 정보를 반환한다.
 * 그 외 NSID에 대해서는 nmic(공유 가능), rescap(예약 기능), anagrpid, nstat.nrdy를 채운다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify → [이 함수]
 */
int
spdk_nvmf_identify_ns_iocs_independent(struct spdk_nvmf_ctrlr *ctrlr,
				       struct spdk_nvme_cmd *cmd,
				       struct spdk_nvme_cpl *rsp,
				       struct spdk_nvme_ns_iocs_independent_data *nsdata)
{
	struct spdk_nvmf_ns *ns; /* [한국어] NSID에 해당하는 NS */

	memset(nsdata, 0, sizeof(*nsdata)); /* [한국어] 응답 버퍼 초기화 */

	/** From NVMe 2.0d
	 * If the controller supports the Namespace Management capability
	 * (refer to section 8.11) and the NSID field is set to FFFFFFFFh,
	 * then the controller returns an I/O Command Set Independent
	 * Identify Namespace data structure that specifies capabilities
	 * that are common for the controller.
	 */
	if (ctrlr->cdata.oacs.nms && cmd->nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		/* [한국어] NS Management 지원 + GLOBAL_NS_TAG: 컨트롤러 공통 공유 NS 특성 반환 */
		nsdata->nmic.shrns = 1; /* [한국어] NMIC.SHRNS: NS 공유 가능 */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = _nvmf_ctrlr_get_ns_safe(ctrlr, cmd->nsid, rsp); /* [한국어] NSID → NS 조회 */

	if (ns == NULL) {
		/* [한국어] NS 없음 → 0-filled 버퍼 반환 (rsp에 오류 설정됨) */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	nsdata->nmic.shrns = 1; /* [한국어] NMIC.SHRNS: 이 NS는 여러 컨트롤러와 공유 가능 */
	nsdata->rescap = nvmf_ns_get_rescap(ns); /* [한국어] RESCAP: NS의 예약(Reservation) 기능 비트맵 */

	nsdata->anagrpid = ns->anagrpid; /* [한국어] ANAGRPID: NS가 속한 ANA 그룹 ID */
	nsdata->nstat.nrdy = 1; /* [한국어] NSTAT.NRDY: NS 사용 준비 완료 */

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_is_csi_supported - 컨트롤러가 특정 CSI를 지원하는지 확인
 *
 * @ctrlr: 대상 컨트롤러.
 * @csi: Command Set Identifier (0=NVM, 2=ZNS 등).
 * @return: true(지원), false(미지원).
 *
 * NVM은 항상 지원하며, ZNS는 subsystem에 ZNS NS가 있을 때만 지원한다.
 * Active NS List (IOCS 필터) 등에서 CSI 유효성 검사에 사용.
 */
static bool
nvmf_ctrlr_is_csi_supported(struct spdk_nvmf_ctrlr *ctrlr, uint8_t csi)
{
	return (csi == SPDK_NVME_CSI_NVM) || /* [한국어] NVM CSI는 항상 지원 */
	       (csi == SPDK_NVME_CSI_ZNS && nvmf_subsystem_has_zns_iocs(ctrlr->subsys)); /* [한국어] ZNS는 ZNS NS가 있을 때만 지원 */
}

/*
 * [한국어]
 * nvmf_ctrlr_identify_active_ns_list - Active Namespace ID List(CNS=02h/07h) 응답 생성
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: NVMe SQE (NSID=시작 NSID 필터, CDW11.CSI=Command Set Identifier).
 * @rsp: CQE 응답.
 * @ns_list: 결과 저장 버퍼 (최대 1024개 NSID).
 * @iocs: true이면 CSI 필터 적용 (CNS=07h), false이면 CSI 무관 (CNS=02h).
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * cmd->nsid보다 큰 NSID 중 이 컨트롤러에 visible하고 (iocs=true이면) CSI가 일치하는
 * NS의 NSID를 오름차순으로 최대 1024개 열거한다.
 * cmd->nsid=0이면 전체 목록, 0xFFFFFFFEh 이상은 유효하지 않아 오류 반환.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify → [이 함수]
 */
static int
nvmf_ctrlr_identify_active_ns_list(struct spdk_nvmf_ctrlr *ctrlr,
				   struct spdk_nvme_cmd *cmd,
				   struct spdk_nvme_cpl *rsp,
				   struct spdk_nvme_ns_list *ns_list,
				   bool iocs)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys; /* [한국어] NS를 보유한 subsystem */
	struct spdk_nvmf_ns *ns; /* [한국어] 순회 중인 NS */
	uint32_t count = 0; /* [한국어] 목록에 추가된 NSID 수 */
	uint8_t csi = cmd->cdw11_bits.identify.csi; /* [한국어] CSI 필터 (iocs=true일 때만 사용) */

	if (cmd->nsid >= 0xfffffffeUL) {
		/* [한국어] NSID 필터가 0xFFFFFFFEh 이상이면 유효하지 않음 */
		SPDK_ERRLOG("Identify Active Namespace List with invalid NSID %u\n", cmd->nsid);
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (iocs && !nvmf_ctrlr_is_csi_supported(ctrlr, csi)) {
		/* [한국어] IOCS 필터 요청인데 CSI가 지원되지 않으면 오류 */
		SPDK_ERRLOG("Identify Active Namespace List with invalid CSI %u\n", csi);
		rsp->status.sc  = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	memset(ns_list, 0, sizeof(*ns_list)); /* [한국어] 목록 버퍼 초기화 */

	/* [한국어] subsystem의 모든 NS를 순회하여 조건에 맞는 NSID 수집 */
	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		if (ns->opts.nsid <= cmd->nsid || !nvmf_ctrlr_ns_is_visible(ctrlr, ns->opts.nsid)) {
			/* [한국어] NSID가 필터 이하이거나 이 컨트롤러에 visible하지 않으면 건너뜀 */
			continue;
		}

		if (iocs && csi != ns->csi) {
			/* [한국어] IOCS 필터링: CSI가 일치하지 않으면 건너뜀 */
			continue;
		}

		ns_list->ns_list[count++] = ns->opts.nsid; /* [한국어] NSID를 목록에 추가 */
		if (count == SPDK_COUNTOF(ns_list->ns_list)) {
			/* [한국어] 최대 1024개 도달 → 중단 (NVMe 스펙: 한 번에 최대 1024개 반환) */
			break;
		}
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * _add_ns_id_desc - NS ID Descriptor List 버퍼에 디스크립터 추가
 *
 * @buf_ptr: 현재 쓰기 위치 포인터 (작업 후 진행됨).
 * @buf_remain: 남은 버퍼 크기 (작업 후 감소).
 * @type: NS ID 디스크립터 타입 (EUI64/NGUID/UUID/CSI 등).
 * @data: 디스크립터 데이터 포인터.
 * @data_size: 데이터 크기 (1~255 범위).
 *
 * NS Identification Descriptor 항목 하나를 버퍼에 직렬화하여 추가한다.
 * 헤더(NIDT+NIDL) + 데이터 순서로 직렬화한다.
 * data_size가 0이거나 255 초과이거나 버퍼가 부족하면 아무것도 하지 않는다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify_ns_id_descriptor_list → [이 함수] (ADD_ID_DESC 매크로 통해)
 */
static void
_add_ns_id_desc(void **buf_ptr, size_t *buf_remain,
		enum spdk_nvme_nidt type,
		const void *data, size_t data_size)
{
	struct spdk_nvme_ns_id_desc *desc; /* [한국어] 버퍼에 쓸 디스크립터 포인터 */
	size_t desc_size = sizeof(*desc) + data_size; /* [한국어] 헤더(4B) + 데이터 크기 */

	/*
	 * These should never fail in practice, since all valid NS ID descriptors
	 * should be defined so that they fit in the available 4096-byte buffer.
	 */
	assert(data_size > 0); /* [한국어] 데이터가 0바이트이면 안 됨 */
	assert(data_size <= UINT8_MAX); /* [한국어] NIDL은 1바이트 필드 → 255 이하여야 함 */
	assert(desc_size < *buf_remain); /* [한국어] 남은 버퍼가 충분해야 함 */
	if (data_size == 0 || data_size > UINT8_MAX || desc_size > *buf_remain) {
		return; /* [한국어] 유효하지 않은 크기 → 아무것도 하지 않음 */
	}

	desc = *buf_ptr; /* [한국어] 현재 위치에 디스크립터 헤더 배치 */
	desc->nidt = type; /* [한국어] NIDT: NS ID 디스크립터 타입 (EUI64=1, NGUID=2, UUID=3, CSI=4) */
	desc->nidl = data_size; /* [한국어] NIDL: NS ID Length (데이터 크기) */
	memcpy(desc->nid, data, data_size); /* [한국어] 디스크립터 데이터 복사 */

	*buf_ptr += desc_size; /* [한국어] 버퍼 포인터 진행 */
	*buf_remain -= desc_size; /* [한국어] 남은 버퍼 크기 감소 */
}

/*
 * [한국어]
 * nvmf_ctrlr_identify_ns_id_descriptor_list - NS ID Descriptor List(CNS=03h) 응답 생성
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: NVMe SQE (NSID 포함).
 * @rsp: CQE 응답.
 * @id_desc_list: 결과 저장 버퍼 (4096B).
 * @id_desc_list_size: 버퍼 크기.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 지정된 NSID의 NS에 대한 NS ID Descriptor List를 반환한다.
 * EUI64, NGUID, UUID, CSI 디스크립터를 (값이 0이 아닌 경우에만) 순서대로 추가한다.
 * 목록은 자동으로 0-terminated된다(버퍼가 zeroed 상태로 시작).
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify → [이 함수] → _add_ns_id_desc
 */
static int
nvmf_ctrlr_identify_ns_id_descriptor_list(
	struct spdk_nvmf_ctrlr *ctrlr,
	struct spdk_nvme_cmd *cmd,
	struct spdk_nvme_cpl *rsp,
	void *id_desc_list, size_t id_desc_list_size)
{
	struct spdk_nvmf_ns *ns; /* [한국어] 대상 NS */
	size_t buf_remain = id_desc_list_size; /* [한국어] 남은 버퍼 크기 */
	void *buf_ptr = id_desc_list; /* [한국어] 현재 쓰기 위치 */
	uint32_t nsid = cmd->nsid; /* [한국어] 조회할 NSID */

	if (nsid == 0 || nsid > ctrlr->subsys->max_nsid) {
		/* [한국어] 유효 범위(1~max_nsid) 밖의 NSID → INVALID_NAMESPACE 오류 */
		SPDK_ERRLOG("Identify Namespace Identification Descriptor list with invalid NSID %u\n", nsid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = nvmf_ctrlr_get_ns(ctrlr, nsid); /* [한국어] NSID → NS 조회 */
	if (ns == NULL || ns->bdev == NULL) {
		/* [한국어] 비활성 NS → INVALID_FIELD 오류 */
		SPDK_ERRLOG("Identify Namespace Identification Descriptor list with inactive NSID %u\n", nsid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

#define ADD_ID_DESC(type, data, size) \
	do { \
		if (!spdk_mem_all_zero(data, size)) { \
			/* [한국어] 데이터가 0이 아닌 경우에만 디스크립터 추가 */ \
			_add_ns_id_desc(&buf_ptr, &buf_remain, type, data, size); \
		} \
	} while (0)

	ADD_ID_DESC(SPDK_NVME_NIDT_EUI64, ns->opts.eui64, sizeof(ns->opts.eui64)); /* [한국어] IEEE EUI-64 식별자 */
	ADD_ID_DESC(SPDK_NVME_NIDT_NGUID, ns->opts.nguid, sizeof(ns->opts.nguid)); /* [한국어] 128비트 NS GUID */
	ADD_ID_DESC(SPDK_NVME_NIDT_UUID, &ns->opts.uuid, sizeof(ns->opts.uuid)); /* [한국어] 128비트 UUID */
	ADD_ID_DESC(SPDK_NVME_NIDT_CSI, &ns->csi, sizeof(uint8_t)); /* [한국어] Command Set Identifier */

	/*
	 * The list is automatically 0-terminated, both in the temporary buffer
	 * used by nvmf_ctrlr_identify(), and the eventual iov destination -
	 * controller to host buffers in admin commands always get zeroed in
	 * nvmf_ctrlr_process_admin_cmd().
	 */

#undef ADD_ID_DESC

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_identify_iocs - I/O Command Set Vector(CNS=1Ch) Identify 응답 생성
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: NVMe SQE (CNTID 포함).
 * @rsp: CQE 응답.
 * @cdata: 결과 저장 버퍼.
 * @cdata_size: 버퍼 크기.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 컨트롤러가 지원하는 I/O Command Set의 비트벡터를 반환한다.
 * NVM CSI는 항상 지원하며, ZNS bdev NS가 존재하는 경우 ZNS도 지원.
 * CNTID=0xFFFF(모든 컨트롤러)이거나 현재 CNTLID와 일치하는 경우에만 처리.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify → [이 함수]
 */
static int
nvmf_ctrlr_identify_iocs(struct spdk_nvmf_ctrlr *ctrlr,
			 struct spdk_nvme_cmd *cmd,
			 struct spdk_nvme_cpl *rsp,
			 void *cdata, size_t cdata_size)
{
	struct spdk_nvme_iocs_vector *vector; /* [한국어] IOCS 지원 비트벡터 응답 포인터 */
	struct spdk_nvmf_ns *ns; /* [한국어] ZNS 지원 여부 확인을 위한 NS 순회용 */

	if (cdata_size < sizeof(struct spdk_nvme_iocs_vector)) {
		/* [한국어] 버퍼가 iocs_vector 구조체보다 작으면 유효하지 않음 */
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* For now we only support this command sent to the current
	 * controller.
	 */
	if (cmd->cdw10_bits.identify.cntid != 0xFFFF &&
	    cmd->cdw10_bits.identify.cntid != ctrlr->cntlid) {
		/* [한국어] CNTID가 0xFFFF(모든 컨트롤러)도 현재 CNTLID도 아니면 오류 */
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	memset(cdata, 0, cdata_size); /* [한국어] 응답 버퍼 초기화 */

	vector = cdata; /* [한국어] 응답 버퍼를 iocs_vector 구조체로 캐스트 */
	vector->nvm = 1; /* [한국어] NVM CSI: 항상 지원 */
	for (ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(ctrlr->subsys, ns)) {
		if (ns->bdev == NULL) {
			/* [한국어] 비활성 NS 건너뜀 */
			continue;
		}
		if (spdk_bdev_is_zoned(ns->bdev)) {
			/* [한국어] ZNS bdev NS가 하나라도 있으면 ZNS CSI 지원 선언 */
			vector->zns = 1;
		}
	}

	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_identify - Identify admin 명령(opcode 06h) 디스패처
 *
 * @req: 요청.
 * @return: COMPLETE 또는 (Identify NS의 passthru 경로일 때) ASYNCHRONOUS.
 *
 * Identify는 4096B 응답 데이터를 만들며 CNS(Identify Type) 분기:
 *  - CNS=00h NS: spdk_nvmf_ctrlr_identify_ns_ext (passthru 가능)
 *  - CNS=01h CTRLR: spdk_nvmf_ctrlr_identify_ctrlr (vendor/model/serial/oncs 등)
 *  - CNS=02h Active NS List: nvmf_ctrlr_identify_active_ns_list
 *  - CNS=03h NS ID Descriptor List: EUI64/NGUID/UUID/CSI 디스크립터
 *  - CNS=05h NS IOCS Specific (NVM/ZNS), CNS=06h Ctrlr IOCS Specific
 *  - CNS=08h NS IOCS Independent, CNS=07h Active NS List IOCS, CNS=1Ch IOCS Vector
 * Discovery subsystem이면 CTRLR(01h)만 허용한다.
 *
 * 응답 buffer 분할 가능성(req->iov 여러 개) 때문에 임시 4KB tmpbuf에 만들고
 * spdk_iov_xfer_from_buf로 흩어진 iov에 복사한다.
 */
static int
nvmf_ctrlr_identify(struct spdk_nvmf_request *req)
{
	uint8_t cns;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	int ret = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	char tmpbuf[SPDK_NVME_IDENTIFY_BUFLEN] = "";
	struct spdk_iov_xfer ix;

	if (req->iovcnt < 1 || req->length < SPDK_NVME_IDENTIFY_BUFLEN) {
		SPDK_DEBUGLOG(nvmf, "identify command with invalid buffer\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return ret;
	}

	cns = cmd->cdw10_bits.identify.cns;

	if (spdk_nvmf_subsystem_is_discovery(subsystem) &&
	    cns != SPDK_NVME_IDENTIFY_CTRLR) {
		/* Discovery controllers only support Identify Controller */
		goto invalid_cns;
	}

	/*
	 * We must use a temporary buffer: it's entirely possible the out buffer
	 * is split across more than one IOV.
	 */
	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt); /* [한국어] iov xfer 초기화 — tmpbuf→iov 복사에 사용 */

	SPDK_DEBUGLOG(nvmf, "Received identify command with CNS 0x%02x\n", cns);

	switch (cns) {
	case SPDK_NVME_IDENTIFY_NS:
		/* Function below can be asynchronous & we always need to have the data in request's buffer
		 * So just return here */
		/* [한국어] Identify Namespace: NSID 지정 네임스페이스 구조체 반환 — 비동기 가능하므로 직접 return */
		return spdk_nvmf_ctrlr_identify_ns_ext(req);
	case SPDK_NVME_IDENTIFY_CTRLR:
		/* [한국어] Identify Controller: 이 컨트롤러의 Identity 구조체(MDTS, NN, OACS 등) 반환 */
		ret = spdk_nvmf_ctrlr_identify_ctrlr(ctrlr, (void *)&tmpbuf);
		break;
	case SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST:
		/* [한국어] Identify Active Namespace ID List: 현재 활성 NSID 목록 반환 (non-IOCS 버전) */
		ret = nvmf_ctrlr_identify_active_ns_list(ctrlr, cmd, rsp, (void *)&tmpbuf, false);
		break;
	case SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST:
		/* [한국어] Namespace Identifier Descriptor List: NSID별 UUID/EUI64 등 식별자 반환 */
		ret = nvmf_ctrlr_identify_ns_id_descriptor_list(ctrlr, cmd, rsp,
				tmpbuf, sizeof(tmpbuf));
		break;
	case SPDK_NVME_IDENTIFY_NS_IOCS:
		/* [한국어] I/O Command Set Specific NS 구조체: ZNS 등 CSI별 확장 정보 반환 */
		ret = spdk_nvmf_ns_identify_iocs_specific(ctrlr, cmd, rsp, (void *)&tmpbuf, sizeof(tmpbuf));
		break;
	case SPDK_NVME_IDENTIFY_CTRLR_IOCS:
		/* [한국어] I/O Command Set Specific Controller 구조체: ZNS 컨트롤러 확장 정보 반환 */
		ret = spdk_nvmf_ctrlr_identify_iocs_specific(ctrlr, cmd, rsp, (void *)&tmpbuf, sizeof(tmpbuf));
		break;
	case SPDK_NVME_IDENTIFY_NS_IOCS_INDEPENDENT:
		/* [한국어] I/O Command Set Independent NS 구조체: CSI 무관 공통 NS 정보 반환 */
		ret = spdk_nvmf_identify_ns_iocs_independent(ctrlr, cmd, rsp, (void *)&tmpbuf);
		break;
	case SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST_IOCS:
		/* [한국어] Active NS List (IOCS 버전): I/O Command Set별 활성 NSID 목록 반환 */
		ret = nvmf_ctrlr_identify_active_ns_list(ctrlr, cmd, rsp, (void *)&tmpbuf, true);
		break;
	case SPDK_NVME_IDENTIFY_IOCS:
		/* [한국어] I/O Command Set: 지원하는 CSI 목록 반환 (NVM=0, ZNS=2 등) */
		ret = nvmf_ctrlr_identify_iocs(ctrlr, cmd, rsp, (void *)&tmpbuf, sizeof(tmpbuf));
		break;
	default:
		/* [한국어] 미지원 CNS → invalid_cns 레이블로 점프하여 오류 응답 */
		goto invalid_cns;
	}

	if (ret == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		spdk_iov_xfer_from_buf(&ix, tmpbuf, sizeof(tmpbuf));
	}

	return ret;

invalid_cns:
	SPDK_DEBUGLOG(nvmf, "Identify command with unsupported CNS 0x%02x\n", cns);
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	return ret;
}

/*
 * [한국어]
 * nvmf_qpair_abort_aer - admin qpair에 대기 중인 AER(Async Event Request)를 중단
 *
 * @qpair: 검색할 qpair (admin이 아니면 즉시 false 반환).
 * @cid: Abort할 Command ID.
 * @return: true이면 해당 CID의 AER을 찾아 ABORTED_BY_REQUEST로 완료시킴, false이면 미발견.
 *
 * Abort 명령(opcode 0Ch)이 수신되면 aer_req[] 배열에서 CID가 일치하는 요청을 찾아
 * ABORTED_BY_REQUEST 상태로 즉시 완료시킨다.
 * 배열의 연속성을 유지하기 위해 마지막 항목을 삭제된 위치로 이동한다.
 *
 * 실행 컨텍스트: ctrlr->thread (assert로 강제).
 *
 * 호출 체인:
 *   nvmf_ctrlr_abort_cmd → 본 함수 → _nvmf_request_complete
 */
static bool
nvmf_qpair_abort_aer(struct spdk_nvmf_qpair *qpair, uint16_t cid)
{
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr; /* [한국어] qpair의 컨트롤러 */
	struct spdk_nvmf_request *req; /* [한국어] Abort할 AER 요청 */
	int i; /* [한국어] aer_req[] 순회 인덱스 */

	if (!nvmf_qpair_is_admin_queue(qpair)) {
		/* [한국어] AER은 admin qpair에만 존재 */
		return false;
	}

	assert(spdk_get_thread() == ctrlr->thread); /* [한국어] ctrlr->thread에서만 aer_req[] 접근 가능 */

	for (i = 0; i < ctrlr->nr_aer_reqs; i++) {
		if (ctrlr->aer_req[i]->cmd->nvme_cmd.cid == cid) {
			/* [한국어] CID 일치 → 이 AER을 Abort */
			SPDK_DEBUGLOG(nvmf, "Aborting AER request\n");
			req = ctrlr->aer_req[i]; /* [한국어] Abort할 요청 저장 */
			ctrlr->aer_req[i] = NULL; /* [한국어] 슬롯 클리어 */
			ctrlr->nr_aer_reqs--; /* [한국어] AER 슬롯 수 감소 */

			/* Move the last req to the aborting position for making aer_reqs
			 * in continuous
			 */
			/* [한국어] 배열 연속성 유지: 마지막 요청을 삭제된 슬롯으로 이동 */
			if (i < ctrlr->nr_aer_reqs) {
				ctrlr->aer_req[i] = ctrlr->aer_req[ctrlr->nr_aer_reqs]; /* [한국어] 마지막 요청을 현재 위치로 이동 */
				ctrlr->aer_req[ctrlr->nr_aer_reqs] = NULL; /* [한국어] 기존 마지막 슬롯 클리어 */
			}

			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] Generic 오류 타입 */
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST; /* [한국어] Abort 명령에 의해 중단됨 */
			_nvmf_request_complete(req); /* [한국어] AER 응답 전송 + outstanding 큐에서 제거 */
			return true; /* [한국어] Abort 성공 */
		}
	}

	return false; /* [한국어] 해당 CID의 AER 미발견 */
}

/*
 * [한국어]
 * nvmf_qpair_abort_pending_zcopy_reqs - qpair의 pending zero-copy 요청들을 중단
 *
 * @qpair: 중단할 qpair.
 *
 * zero-copy(zcopy) 요청은 transport가 bdev로부터 직접 버퍼를 받아 처리하는 모드.
 * ZCOPY_PHASE_EXECUTE 상태인 요청은 zcopy_end 콜백을 받아야만 버퍼가 해제되므로
 * outstanding 큐에서 직접 제거할 수 없다. 대신 오류 응답을 설정하고
 * nvmf_transport_req_free()를 호출하여 transport가 zcopy_end로 정리하도록 한다.
 *
 * qpair disconnect 시 호출된다.
 *
 * 실행 컨텍스트: qpair->group->thread.
 *
 * 호출 체인:
 *   qpair disconnect 경로 → 본 함수 → nvmf_transport_req_free
 */
void
nvmf_qpair_abort_pending_zcopy_reqs(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_request *req, *tmp; /* [한국어] 순회 및 안전 삭제용 포인터 */

	TAILQ_FOREACH_SAFE(req, &qpair->outstanding, link, tmp) {
		/* [한국어] 모든 미완료 요청 순회 (safe 버전으로 삭제 중 안전) */
		if (req->zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE) {
			/* Zero-copy requests are kept on the outstanding queue from the moment
			 * zcopy_start is sent until a zcopy_end callback is received.  Therefore,
			 * we can't remove them from the outstanding queue here, but need to rely on
			 * the transport to do a zcopy_end to release their buffers and, in turn,
			 * remove them from the queue.
			 */
			/* [한국어] EXECUTE 단계: zcopy_end 콜백 전까지 outstanding 큐에 남아야 함
			 * 직접 제거하지 않고 오류 상태 설정 후 transport의 zcopy_end에 위임 */
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] 오류 타입 설정 */
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST; /* [한국어] Abort 상태 설정 */
			nvmf_transport_req_free(req); /* [한국어] transport 레이어에 zcopy 정리 요청 */
		}
	}
}

/*
 * [한국어]
 * nvmf_qpair_cid_is_reservation - 지정된 CID가 예약(Reservation) 명령인지 확인
 *
 * @qpair: 검색할 qpair.
 * @cid: 확인할 Command ID.
 * @return: true이면 해당 CID 명령이 Reservation 관련 명령, false이면 아님.
 *
 * outstanding 큐에서 CID가 일치하는 요청을 찾아 opcode가 Reservation 명령
 * (Register/Acquire/Release/Report)인지 확인한다.
 * SPDK NVMe-oF는 Reservation 명령의 Abort를 지원하지 않으므로 이를 판별하기 위해 사용.
 *
 * 실행 컨텍스트: qpair의 poll group thread.
 *
 * 호출 체인:
 *   nvmf_qpair_abort_request → [이 함수]
 */
static bool
nvmf_qpair_cid_is_reservation(const struct spdk_nvmf_qpair *qpair, uint16_t cid)
{
	struct spdk_nvmf_request *req; /* [한국어] outstanding 큐 순회용 */
	struct spdk_nvme_cmd *cmd; /* [한국어] 요청의 NVMe SQE */

	TAILQ_FOREACH(req, &qpair->outstanding, link) {
		cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe 명령 포인터 */

		if (cmd->cid != cid) {
			continue; /* [한국어] CID 불일치 → 다음 항목 */
		}
		switch (cmd->opc) {
		case SPDK_NVME_OPC_RESERVATION_REGISTER:
		case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		case SPDK_NVME_OPC_RESERVATION_RELEASE:
		case SPDK_NVME_OPC_RESERVATION_REPORT:
			return true; /* [한국어] Reservation 명령 → Abort 미지원 */
		default:
			return false; /* [한국어] 다른 명령 */
		}
	}
	return false; /* [한국어] 해당 CID 미발견 */
}

/*
 * [한국어]
 * nvmf_qpair_abort_request - qpair에서 CID에 해당하는 명령 Abort 시도
 *
 * @qpair: Abort 대상 qpair.
 * @req: Abort 명령 요청 (cdw10에 SQID/CID 포함).
 *
 * 1) AER 요청이면 nvmf_qpair_abort_aer로 즉시 중단.
 * 2) Reservation 명령이면 Abort 미지원으로 완료(not aborted 상태).
 * 3) 그 외: transport 레이어에 위임 (nvmf_transport_qpair_abort_request).
 * cdw0 비트0: 0=성공적으로 Abort, 1=Abort 안 됨.
 *
 * 실행 컨텍스트: qpair의 poll group thread (nvmf_ctrlr_abort_on_pg 콜백).
 *
 * 호출 체인:
 *   nvmf_ctrlr_abort_on_pg → [이 함수] → nvmf_qpair_abort_aer / nvmf_transport_qpair_abort_request
 */
static void
nvmf_qpair_abort_request(struct spdk_nvmf_qpair *qpair, struct spdk_nvmf_request *req)
{
	uint16_t cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid; /* [한국어] Abort 대상 명령의 CID */

	if (nvmf_qpair_abort_aer(qpair, cid)) {
		/* [한국어] AER 요청을 성공적으로 Abort */
		SPDK_DEBUGLOG(nvmf, "abort ctrlr=%p sqid=%u cid=%u successful\n",
			      qpair->ctrlr, qpair->qid, cid);
		req->rsp->nvme_cpl.cdw0 &= ~1U; /* Command successfully aborted */ /* [한국어] CDW0 비트0=0: Abort 성공 */

		spdk_nvmf_request_complete(req); /* [한국어] Abort 명령 완료 응답 전송 */
		return;
	}
	if (nvmf_qpair_cid_is_reservation(qpair, cid)) {
		/* We don't support aborting reservation requests, leave completion as not-aborted */
		/* [한국어] Reservation 명령 Abort 미지원 → CDW0 비트0=1(not aborted) 그대로 */
		SPDK_DEBUGLOG(nvmf, "abort ctrlr=%p sqid=%u cid=%u for reservation not supported\n",
			      qpair->ctrlr, qpair->qid, cid);
		spdk_nvmf_request_complete(req); /* [한국어] Abort 명령 완료 (not aborted 상태로) */
		return;
	}

	nvmf_transport_qpair_abort_request(qpair, req); /* [한국어] transport 레이어에 Abort 위임 (비동기) */
}

/*
 * [한국어]
 * nvmf_ctrlr_abort_done - for_each_channel 완료 콜백 (모든 poll group 순회 완료)
 *
 * @i: IO channel 이터레이터.
 * @status: 이터레이션 완료 상태 (0=미발견, -1=발견 후 중단).
 *
 * spdk_for_each_channel이 완료될 때 호출된다.
 * status=0이면 Abort 대상 qpair를 찾지 못한 것이므로 req를 직접 완료한다.
 * status=-1이면 nvmf_ctrlr_abort_on_pg에서 이미 처리했으므로 아무것도 안 한다.
 *
 * 실행 컨텍스트: ctrlr->thread (spdk_for_each_channel 완료 콜백).
 *
 * 호출 체인:
 *   nvmf_ctrlr_abort → spdk_for_each_channel → [이 함수] → _nvmf_request_complete
 */
static void
nvmf_ctrlr_abort_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_request *req = spdk_io_channel_iter_get_ctx(i); /* [한국어] Abort 명령 요청 */

	if (status == 0) {
		/* There was no qpair whose ID matches SQID of the abort command.
		 * Hence call _nvmf_request_complete() here.
		 */
		/* [한국어] 모든 poll group 순회 후 대상 qpair 미발견 → Abort 명령 완료 (not aborted 상태) */
		_nvmf_request_complete(req);
	}
	/* [한국어] status=-1이면 nvmf_ctrlr_abort_on_pg에서 이미 완료했으므로 아무것도 안 함 */
}

/*
 * [한국어]
 * nvmf_ctrlr_abort_on_pg - 각 poll group에서 Abort 대상 qpair 탐색
 *
 * @i: IO channel 이터레이터 (ctx=req, channel=poll group).
 *
 * poll group의 qpair 목록에서 ctrlr와 SQID가 일치하는 qpair를 찾아
 * nvmf_qpair_abort_request()를 호출한다. 발견 시 -1을 반환하여 이터레이션 중단.
 * 미발견 시 0을 반환하여 다음 poll group으로 계속.
 *
 * 실행 컨텍스트: 각 poll group thread (spdk_for_each_channel 콜백).
 *
 * 호출 체인:
 *   nvmf_ctrlr_abort → spdk_for_each_channel → [이 함수] → nvmf_qpair_abort_request
 */
static void
nvmf_ctrlr_abort_on_pg(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_request *req = spdk_io_channel_iter_get_ctx(i); /* [한국어] Abort 명령 요청 */
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i); /* [한국어] 현재 IO channel */
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch); /* [한국어] poll group 컨텍스트 */
	uint16_t sqid = req->cmd->nvme_cmd.cdw10_bits.abort.sqid; /* [한국어] Abort 대상 SQ ID */
	struct spdk_nvmf_qpair *qpair; /* [한국어] 순회 중인 qpair */

	TAILQ_FOREACH(qpair, &group->qpairs, link) {
		if (qpair->ctrlr == req->qpair->ctrlr && qpair->qid == sqid) {
			/* Found the qpair */
			/* [한국어] 동일 ctrlr이고 QID=SQID인 qpair 발견 */

			nvmf_qpair_abort_request(qpair, req); /* [한국어] 대상 qpair에서 Abort 실행 */

			/* Return -1 for the status so the iteration across threads stops. */
			/* [한국어] -1 반환 → for_each_channel 이터레이션 중단 */
			spdk_for_each_channel_continue(i, -1);
			return;
		}
	}

	spdk_for_each_channel_continue(i, 0); /* [한국어] 이 poll group에서 미발견 → 다음 poll group 계속 */
}

/*
 * [한국어]
 * nvmf_ctrlr_abort - Abort 명령(opcode 0Ch) 처리
 *
 * @req: Abort 명령 요청 (cdw10에 SQID/CID 포함).
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS.
 *
 * Abort 명령은 SQID/CID로 식별된 다른 명령을 중단하는 요청이다.
 * 응답 CDW0 비트0=1(not aborted)로 초기화하고 비동기 탐색을 시작한다.
 * spdk_for_each_channel로 모든 poll group을 순회하여 대상 qpair를 찾는다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_process_admin_cmd → [이 함수]
 *                                 → spdk_for_each_channel
 *                                   → nvmf_ctrlr_abort_on_pg (각 pg thread)
 *                                   → nvmf_ctrlr_abort_done (완료 콜백)
 */
static int
nvmf_ctrlr_abort(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */

	rsp->cdw0 = 1U; /* Command not aborted */ /* [한국어] CDW0 비트0=1: 기본값 "Abort 안 됨" */
	rsp->status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] Generic 상태 타입 */
	rsp->status.sc = SPDK_NVME_SC_SUCCESS; /* [한국어] SC=SUCCESS (Abort 명령 자체는 성공) */

	/* Send a message to each poll group, searching for this ctrlr, sqid, and command. */
	/* [한국어] 모든 poll group을 순회하며 ctrlr/SQID/CID에 해당하는 qpair 탐색 */
	spdk_for_each_channel(req->qpair->ctrlr->subsys->tgt, /* [한국어] tgt의 모든 poll group */
			      nvmf_ctrlr_abort_on_pg, /* [한국어] 각 poll group에서 탐색 */
			      req, /* [한국어] 컨텍스트로 req 전달 */
			      nvmf_ctrlr_abort_done /* [한국어] 완료 콜백 */
			     );

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS; /* [한국어] 비동기: 콜백에서 완료 */
}

/*
 * [한국어]
 * nvmf_ctrlr_abort_request - bdev 레이어에 Abort 요청 전달
 *
 * @req: Abort 명령 요청 (req->req_to_abort에 대상 요청이 설정됨).
 * @return: COMPLETE 또는 ASYNCHRONOUS.
 *
 * transport 레이어에서 Abort 대상 req를 찾은 후 이 함수를 호출한다.
 * 1) 사용자 정의 Abort 핸들러가 있으면 그것을 사용.
 * 2) 없으면 대상 요청의 NSID로 bdev를 찾아 spdk_nvmf_bdev_ctrlr_abort_cmd 호출.
 *
 * 실행 컨텍스트: qpair의 poll group thread.
 *
 * 호출 체인:
 *   transport abort 경로 → [이 함수] → spdk_nvmf_bdev_ctrlr_abort_cmd
 */
int
nvmf_ctrlr_abort_request(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_request *req_to_abort = req->req_to_abort; /* [한국어] Abort 대상 요청 */
	struct spdk_bdev *bdev; /* [한국어] 대상 bdev */
	struct spdk_bdev_desc *desc; /* [한국어] bdev descriptor */
	struct spdk_io_channel *ch; /* [한국어] bdev I/O channel */
	int rc; /* [한국어] bdev 조회 결과 */

	assert(req_to_abort != NULL); /* [한국어] 대상 요청이 반드시 설정되어 있어야 함 */

	if (g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_ABORT].hdlr &&
	    nvmf_qpair_is_admin_queue(req_to_abort->qpair)) {
		/* [한국어] 사용자 정의 Abort 핸들러 있고 대상이 Admin 명령이면 핸들러 사용 */
		return g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_ABORT].hdlr(req);
	}

	/* [한국어] 대상 요청의 NSID로 bdev/desc/channel 조회 */
	rc = spdk_nvmf_request_get_bdev(req_to_abort->cmd->nvme_cmd.nsid, req_to_abort,
					&bdev, &desc, &ch);
	if (rc != 0) {
		/* [한국어] bdev 없음 → 완료 (not aborted 상태) */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* [한국어] bdev 레이어에 Abort 전달 (비동기 가능) */
	return spdk_nvmf_bdev_ctrlr_abort_cmd(bdev, desc, ch, req, req_to_abort);
}

/*
 * [한국어]
 * get_features_generic - Get Features 응답에 raw Feature 값을 CDW0에 설정하는 공통 헬퍼
 *
 * @req: Admin 명령 요청.
 * @cdw0: 응답 CDW0에 설정할 Feature 값 (raw 32비트).
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * 대부분의 Get Features 응답은 Feature 값을 CDW0에 담아 반환한다.
 * 이 공통 처리를 위한 단순 래퍼이다.
 *
 * 호출 체인: nvmf_ctrlr_get_features → 본 함수
 */
static int
get_features_generic(struct spdk_nvmf_request *req, uint32_t cdw0)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */

	rsp->cdw0 = cdw0; /* [한국어] Feature 값을 CDW0에 설정 */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/* we have to use the typedef in the function declaration to appease astyle. */
/* [한국어] astyle 스타일 검사 도구를 위해 typedef 사용 */
typedef enum spdk_nvme_path_status_code spdk_nvme_path_status_code_t;

/*
 * [한국어]
 * _nvme_ana_state_to_path_status - ANA 상태를 NVMe Path Status Code로 변환
 *
 * @ana_state: 현재 ANA 상태 (INACCESSIBLE, PERSISTENT_LOSS, CHANGE 등).
 * @return: 해당하는 NVMe Path Status Code.
 *
 * Get/Set Features 등에서 ANA 상태가 접근 불가일 때 경로 오류 코드를 결정하기 위해 사용.
 * NVMe 스펙 §8.20.3에 따라 ANA 상태별 Path Status Code를 반환한다.
 *
 * 호출 체인: nvmf_ctrlr_get_features / nvmf_ctrlr_set_features → 본 함수
 */
static spdk_nvme_path_status_code_t
_nvme_ana_state_to_path_status(enum spdk_nvme_ana_state ana_state)
{
	switch (ana_state) {
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
		return SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE; /* [한국어] 이 경로를 통해 NS에 접근 불가 */
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
		return SPDK_NVME_SC_ASYMMETRIC_ACCESS_PERSISTENT_LOSS; /* [한국어] 영구적 손실 상태 */
	case SPDK_NVME_ANA_CHANGE_STATE:
		return SPDK_NVME_SC_ASYMMETRIC_ACCESS_TRANSITION; /* [한국어] ANA 상태 전환 중 */
	default:
		return SPDK_NVME_SC_INTERNAL_PATH_ERROR; /* [한국어] 기타 비정상 상태 */
	}
}

/*
 * [한국어]
 * nvmf_ctrlr_get_features - Get Features(opcode 0Ah) 디스패처
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * FID(Feature Identifier)에 따라 각 Feature 조회 함수로 분기한다.
 *
 * 분기 논리:
 *   1) NSID 유효성 검사 (0 < nsid <= max_nsid 또는 GLOBAL_NS_TAG).
 *   2) Discovery subsystem이면 KATO/AEC만 허용.
 *   3) ANA 상태(INACCESSIBLE/PERSISTENT_LOSS/CHANGE)이면 NS 관련 Feature 거절.
 *   4) FID별 해당 함수 호출.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_process_admin_cmd → [이 함수] → 각 nvmf_ctrlr_get_features_*
 */
static int
nvmf_ctrlr_get_features(struct spdk_nvmf_request *req)
{
	uint8_t feature; /* [한국어] Feature Identifier */
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */
	enum spdk_nvme_ana_state ana_state; /* [한국어] cmd->nsid의 ANA 상태 */

	feature = cmd->cdw10_bits.get_features.fid; /* [한국어] Feature ID 추출 */

	if ((cmd->nsid > ctrlr->subsys->max_nsid) && (cmd->nsid != SPDK_NVME_GLOBAL_NS_TAG)) {
		/* [한국어] NSID가 범위 초과이고 GLOBAL_NS_TAG도 아니면 오류 */
		SPDK_ERRLOG("Get Features command with invalid NSID %u, feature ID 0x%02x\n", cmd->nsid, feature);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_nvmf_subsystem_is_discovery(ctrlr->subsys)) {
		/*
		 * Features supported by Discovery controller
		 */
		/* [한국어] Discovery 컨트롤러: KATO와 AEC만 지원 */
		switch (feature) {
		case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
			return get_features_generic(req, ctrlr->feat.keep_alive_timer.raw); /* [한국어] KATO 값 반환 */
		case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
			return get_features_generic(req, ctrlr->feat.async_event_configuration.raw); /* [한국어] AEC 값 반환 */
		default:
			SPDK_INFOLOG(nvmf, "Get Features command with unsupported feature ID 0x%02x\n", feature);
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}
	/*
	 * Process Get Features command for non-discovery controller
	 */
	ana_state = nvmf_ctrlr_get_ana_state_from_nsid(ctrlr, cmd->nsid); /* [한국어] cmd->nsid의 ANA 상태 조회 */
	switch (ana_state) {
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
	case SPDK_NVME_ANA_CHANGE_STATE:
		/* [한국어] ANA 비정상 상태: NS 관련 Feature는 PATH 오류 반환 */
		switch (feature) {
		case SPDK_NVME_FEAT_ERROR_RECOVERY:
		case SPDK_NVME_FEAT_WRITE_ATOMICITY:
		case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
		case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
			/* [한국어] NS 범위 Feature → ANA 경로 오류 */
			response->status.sct = SPDK_NVME_SCT_PATH;
			response->status.sc = _nvme_ana_state_to_path_status(ana_state);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		default:
			break; /* [한국어] 컨트롤러 범위 Feature는 ANA 상태와 무관하게 진행 */
		}
		break;
	default:
		break; /* [한국어] ANA 정상 상태: 그대로 진행 */
	}

	/* [한국어] FID별 Get Features 처리 */
	switch (feature) {
	case SPDK_NVME_FEAT_ARBITRATION:
		return get_features_generic(req, ctrlr->feat.arbitration.raw); /* [한국어] Arbitration Feature */
	case SPDK_NVME_FEAT_POWER_MANAGEMENT:
		return get_features_generic(req, ctrlr->feat.power_management.raw); /* [한국어] Power Management Feature */
	case SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD:
		return nvmf_ctrlr_get_features_temperature_threshold(req); /* [한국어] 온도 임계값 Feature */
	case SPDK_NVME_FEAT_ERROR_RECOVERY:
		return get_features_generic(req, ctrlr->feat.error_recovery.raw); /* [한국어] Error Recovery Feature */
	case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE:
		return get_features_generic(req, ctrlr->feat.volatile_write_cache.raw); /* [한국어] Volatile Write Cache Feature */
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		return get_features_generic(req, ctrlr->feat.number_of_queues.raw); /* [한국어] Number of Queues Feature */
	case SPDK_NVME_FEAT_INTERRUPT_COALESCING:
		return get_features_generic(req, ctrlr->feat.interrupt_coalescing.raw); /* [한국어] Interrupt Coalescing Feature */
	case SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION:
		return nvmf_ctrlr_get_features_interrupt_vector_configuration(req); /* [한국어] 인터럽트 벡터 설정 Feature */
	case SPDK_NVME_FEAT_WRITE_ATOMICITY:
		return get_features_generic(req, ctrlr->feat.write_atomicity.raw); /* [한국어] Write Atomicity Feature */
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		return get_features_generic(req, ctrlr->feat.async_event_configuration.raw); /* [한국어] Async Event Configuration Feature */
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		return get_features_generic(req, ctrlr->feat.keep_alive_timer.raw); /* [한국어] Keep Alive Timer Feature */
	case SPDK_NVME_FEAT_HOST_IDENTIFIER:
		return nvmf_ctrlr_get_features_host_identifier(req); /* [한국어] Host Identifier Feature (128비트 포함) */
	case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
		return nvmf_ctrlr_get_features_reservation_notification_mask(req); /* [한국어] 예약 알림 마스크 Feature */
	case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
		return nvmf_ctrlr_get_features_reservation_persistence(req); /* [한국어] 예약 영속성 Feature */
	case SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT:
		return nvmf_ctrlr_get_features_host_behavior_support(req); /* [한국어] Host Behavior Support Feature */
	default:
		SPDK_INFOLOG(nvmf, "Get Features command with unsupported feature ID 0x%02x\n", feature);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

/*
 * [한국어]
 * is_feature_ctrlr_scope - Feature가 컨트롤러 범위인지 확인
 *
 * @feature: Feature Identifier.
 * @return: true이면 컨트롤러 범위 Feature (NSID 지정 불가).
 *
 * Set Features 명령 시 컨트롤러 범위 Feature에 특정 NSID가 지정되면 오류를 반환해야 한다.
 * NVMe 스펙 §5.27.1에 따라 컨트롤러 범위 Feature 목록을 정의한다.
 *
 * 호출 체인:
 *   nvmf_ctrlr_set_features → [이 함수]
 */
static bool
is_feature_ctrlr_scope(uint8_t feature)
{
	switch (feature) {
	case SPDK_NVME_FEAT_ARBITRATION: /* [한국어] Arbitration: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_POWER_MANAGEMENT: /* [한국어] Power Management: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD: /* [한국어] Temperature Threshold: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE: /* [한국어] Volatile Write Cache: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES: /* [한국어] Number of Queues: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_INTERRUPT_COALESCING: /* [한국어] Interrupt Coalescing: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION: /* [한국어] Interrupt Vector Config: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION: /* [한국어] AEC: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION: /* [한국어] APST: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_HOST_MEM_BUFFER: /* [한국어] Host Memory Buffer: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_TIMESTAMP: /* [한국어] Timestamp: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER: /* [한국어] KATO: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_HOST_CONTROLLED_THERMAL_MANAGEMENT: /* [한국어] 열 관리: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_NON_OPERATIONAL_POWER_STATE_CONFIG: /* [한국어] 비동작 전원 상태: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT: /* [한국어] Host Behavior: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_IO_COMMAND_SET_PROFILE: /* [한국어] IOCS Profile: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_ENHANCED_CONTROLLER_METADATA: /* [한국어] 향상된 컨트롤러 메타데이터: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_CONTROLLER_METADATA: /* [한국어] 컨트롤러 메타데이터: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_SOFTWARE_PROGRESS_MARKER: /* [한국어] 소프트웨어 진행 마커: 컨트롤러 범위 */
	case SPDK_NVME_FEAT_HOST_IDENTIFIER: /* [한국어] Host Identifier: 컨트롤러 범위 */
		return true;
	default:
		return false; /* [한국어] 위 목록 외의 Feature는 NS 범위 */
	}
}

/*
 * [한국어]
 * nvmf_ctrlr_set_features - Set Features(opcode 09h) 디스패처
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * FID에 따라 각 Feature 설정 함수로 분기한다.
 *
 * 분기 논리:
 *   1) SV(Save) 비트 확인: NVMe-oF는 Feature 저장 미지원 → FEATURE_ID_NOT_SAVEABLE.
 *   2) NSID 유효성 검사.
 *   3) Discovery subsystem이면 KATO/AEC만 허용.
 *   4) ANA 상태(INACCESSIBLE/CHANGE/PERSISTENT_LOSS)이면 관련 Feature 거절.
 *   5) 컨트롤러 범위 Feature에 특정 NSID 지정 시 오류.
 *   6) FID별 해당 함수 호출.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_process_admin_cmd → [이 함수] → 각 nvmf_ctrlr_set_features_*
 */
static int
nvmf_ctrlr_set_features(struct spdk_nvmf_request *req)
{
	uint8_t feature, save; /* [한국어] FID와 SV(Save) 비트 */
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */
	enum spdk_nvme_ana_state ana_state; /* [한국어] cmd->nsid의 ANA 상태 */
	/*
	 * Features are not saveable by the controller as indicated by
	 * ONCS field of the Identify Controller data.
	 * */
	save = cmd->cdw10_bits.set_features.sv; /* [한국어] SV: Feature 영속 저장 요청 비트 */
	if (save) {
		/* [한국어] NVMe-oF 컨트롤러는 Feature 저장 미지원 */
		response->status.sc = SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE;
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	feature = cmd->cdw10_bits.set_features.fid; /* [한국어] Feature ID 추출 */

	if ((cmd->nsid > ctrlr->subsys->max_nsid) && (cmd->nsid != SPDK_NVME_GLOBAL_NS_TAG)) {
		/* [한국어] NSID가 범위 초과이고 GLOBAL_NS_TAG도 아니면 오류 */
		SPDK_ERRLOG("Set Features command with invalid NSID %u, feature ID 0x%02x\n", cmd->nsid, feature);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_nvmf_subsystem_is_discovery(ctrlr->subsys)) {
		/*
		 * Features supported by Discovery controller
		 */
		/* [한국어] Discovery 컨트롤러: KATO와 AEC만 지원 */
		switch (feature) {
		case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
			return nvmf_ctrlr_set_features_keep_alive_timer(req); /* [한국어] KATO 설정 */
		case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
			return nvmf_ctrlr_set_features_async_event_configuration(req); /* [한국어] AEC 설정 */
		default:
			SPDK_INFOLOG(nvmf, "Set Features command with unsupported feature ID 0x%02x\n", feature);
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}
	/*
	 * Process Set Features command for non-discovery controller
	 */
	ana_state = nvmf_ctrlr_get_ana_state_from_nsid(ctrlr, cmd->nsid); /* [한국어] cmd->nsid의 ANA 상태 조회 */
	switch (ana_state) {
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
	case SPDK_NVME_ANA_CHANGE_STATE:
		if (cmd->nsid == SPDK_NVME_GLOBAL_NS_TAG) {
			/* [한국어] GLOBAL_NS_TAG이고 ANA 비정상 → PATH 오류 */
			response->status.sct = SPDK_NVME_SCT_PATH;
			response->status.sc = _nvme_ana_state_to_path_status(ana_state);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else {
			switch (feature) {
			case SPDK_NVME_FEAT_ERROR_RECOVERY:
			case SPDK_NVME_FEAT_WRITE_ATOMICITY:
			case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
			case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
				/* [한국어] 특정 NSID이고 NS 범위 Feature → PATH 오류 */
				response->status.sct = SPDK_NVME_SCT_PATH;
				response->status.sc = _nvme_ana_state_to_path_status(ana_state);
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			default:
				break; /* [한국어] 컨트롤러 범위 Feature는 진행 */
			}
		}
		break;
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
		/* [한국어] PERSISTENT_LOSS: 모든 Set Features 거절 */
		response->status.sct = SPDK_NVME_SCT_PATH;
		response->status.sc = SPDK_NVME_SC_ASYMMETRIC_ACCESS_PERSISTENT_LOSS;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	default:
		break; /* [한국어] ANA 정상 상태: 그대로 진행 */
	}

	if ((cmd->nsid < ctrlr->subsys->max_nsid) && (cmd->nsid != 0)) {
		if (is_feature_ctrlr_scope(feature)) {
			/* [한국어] 컨트롤러 범위 Feature에 특정 NSID 지정 → 오류 */
			SPDK_ERRLOG("Set Feature Controller scope with valid NSID. feature ID 0x%02x, NSID %u\n",
				    feature, cmd->nsid);
			response->status.sc = SPDK_NVME_SC_FEATURE_NOT_NAMESPACE_SPECIFIC;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}

	/* [한국어] FID별 Set Features 처리 */
	switch (feature) {
	case SPDK_NVME_FEAT_ARBITRATION:
		return nvmf_ctrlr_set_features_arbitration(req); /* [한국어] Arbitration 설정 */
	case SPDK_NVME_FEAT_POWER_MANAGEMENT:
		return nvmf_ctrlr_set_features_power_management(req); /* [한국어] Power Management 설정 */
	case SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD:
		return nvmf_ctrlr_set_features_temperature_threshold(req); /* [한국어] 온도 임계값 설정 */
	case SPDK_NVME_FEAT_ERROR_RECOVERY:
		return nvmf_ctrlr_set_features_error_recovery(req); /* [한국어] Error Recovery 설정 */
	case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE:
		return nvmf_ctrlr_set_features_volatile_write_cache(req); /* [한국어] Volatile Write Cache 설정 */
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		return nvmf_ctrlr_set_features_number_of_queues(req); /* [한국어] Number of Queues 설정 */
	case SPDK_NVME_FEAT_INTERRUPT_COALESCING:
		/* [한국어] Interrupt Coalescing은 NVMe-oF에서 변경 불가 */
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVME_SC_FEATURE_NOT_CHANGEABLE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	case SPDK_NVME_FEAT_WRITE_ATOMICITY:
		return nvmf_ctrlr_set_features_write_atomicity(req); /* [한국어] Write Atomicity 설정 */
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		return nvmf_ctrlr_set_features_async_event_configuration(req); /* [한국어] AEC 설정 */
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		return nvmf_ctrlr_set_features_keep_alive_timer(req); /* [한국어] KATO 설정 */
	case SPDK_NVME_FEAT_HOST_IDENTIFIER:
		return nvmf_ctrlr_set_features_host_identifier(req); /* [한국어] Host Identifier 설정 */
	case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
		return nvmf_ctrlr_set_features_reservation_notification_mask(req); /* [한국어] 예약 알림 마스크 설정 */
	case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
		return nvmf_ctrlr_set_features_reservation_persistence(req); /* [한국어] 예약 영속성 설정 */
	case SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT:
		return nvmf_ctrlr_set_features_host_behavior_support(req); /* [한국어] Host Behavior Support 설정 */
	default:
		SPDK_INFOLOG(nvmf, "Set Features command with unsupported feature ID 0x%02x\n", feature);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

/*
 * [한국어]
 * nvmf_ctrlr_keep_alive - Keep Alive(opcode 18h) 명령 처리
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE.
 *
 * KATO(Keep Alive Timeout) 타이머를 리셋한다.
 * last_keep_alive_tick에 현재 tick을 기록하여 폴러가 만료 여부를 판단할 기준을 갱신한다.
 * 이를 통해 컨트롤러가 연결 유지 상태임을 확인한다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_process_admin_cmd → [이 함수]
 */
static int
nvmf_ctrlr_keep_alive(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */

	SPDK_DEBUGLOG(nvmf, "Keep Alive\n");
	/*
	 * To handle keep alive just clear or reset the
	 * ctrlr based keep alive duration counter.
	 * When added, a separate timer based process
	 * will monitor if the time since last recorded
	 * keep alive has exceeded the max duration and
	 * take appropriate action.
	 */
	/* [한국어] KATO 타이머 리셋: 마지막 Keep Alive 수신 tick 갱신
	 * nvmf_ctrlr_keep_alive_poll()이 이 값과 현재 tick 차이로 타임아웃 여부 판단 */
	ctrlr->last_keep_alive_tick = spdk_get_ticks();

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * is_cmd_ctrlr_specific - Admin 명령이 컨트롤러 범위(NSID=0 필수)인지 확인
 *
 * @cmd: 검사할 NVMe 명령.
 * @return: true이면 컨트롤러 범위 명령 (NSID가 0이어야 함).
 *
 * NVMe 스펙에 따라 일부 Admin 명령(Abort, AER, Firmware, SQ/CQ 생성·삭제 등)은
 * 컨트롤러 범위이므로 NSID가 0(비워야)이어야 한다.
 * nvmf_ctrlr_process_admin_cmd에서 이 함수로 NSID 유효성을 검사한다.
 *
 * 호출 체인:
 *   nvmf_ctrlr_process_admin_cmd → [이 함수]
 */
static bool
is_cmd_ctrlr_specific(struct spdk_nvme_cmd *cmd)
{
	switch (cmd->opc) {
	case SPDK_NVME_OPC_DELETE_IO_SQ: /* [한국어] SQ 삭제: 컨트롤러 범위 */
	case SPDK_NVME_OPC_CREATE_IO_SQ: /* [한국어] SQ 생성: 컨트롤러 범위 */
	case SPDK_NVME_OPC_DELETE_IO_CQ: /* [한국어] CQ 삭제: 컨트롤러 범위 */
	case SPDK_NVME_OPC_CREATE_IO_CQ: /* [한국어] CQ 생성: 컨트롤러 범위 */
	case SPDK_NVME_OPC_ABORT: /* [한국어] Abort: 컨트롤러 범위 */
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST: /* [한국어] AER: 컨트롤러 범위 */
	case SPDK_NVME_OPC_FIRMWARE_COMMIT: /* [한국어] Firmware Commit: 컨트롤러 범위 */
	case SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD: /* [한국어] Firmware Download: 컨트롤러 범위 */
	case SPDK_NVME_OPC_KEEP_ALIVE: /* [한국어] Keep Alive: 컨트롤러 범위 */
	case SPDK_NVME_OPC_VIRTUALIZATION_MANAGEMENT: /* [한국어] Virtualization Management: 컨트롤러 범위 */
	case SPDK_NVME_OPC_NVME_MI_SEND: /* [한국어] NVMe-MI Send: 컨트롤러 범위 */
	case SPDK_NVME_OPC_NVME_MI_RECEIVE: /* [한국어] NVMe-MI Receive: 컨트롤러 범위 */
	case SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG: /* [한국어] Doorbell Buffer Config: 컨트롤러 범위 */
	case SPDK_NVME_OPC_SANITIZE: /* [한국어] Sanitize: 컨트롤러 범위 */
		return true;
	default:
		return false; /* [한국어] 위 목록 외의 명령은 NS 범위 (NSID 지정 가능) */
	}
}

/*
 * [한국어]
 * nvmf_ctrlr_process_admin_cmd - admin queue(qid=0)로 들어온 NVMe admin 명령의 디스패처
 *
 * @req: 요청.
 * @return: COMPLETE/ASYNCHRONOUS.
 *
 * spdk_nvmf_request_exec()가 admin qpair로 들어온 비-Fabric 명령을 본 함수에 위임.
 * 흐름:
 *  1) AER이면 mgmt_io_outstanding 카운터 보정 (AER은 outstanding으로 안 침).
 *  2) FUSE 또는 ctrlr-scope 명령에 NSID 잘못 set이면 INVALID_FIELD 응답.
 *  3) CC.EN=0이면 모든 admin 명령 거절 (COMMAND_SEQUENCE_ERROR).
 *  4) 데이터 전송 방향이 controller→host이면 응답 버퍼를 0으로 초기화 (보안/일관성).
 *  5) Discovery subsystem이면 IDENTIFY/GET_LOG_PAGE/KEEP_ALIVE/SET/GET_FEATURES/AER만 허용.
 *  6) g_nvmf_custom_admin_cmd_hdlrs[opc].hdlr 등록되어 있으면 우선 호출(ABORT 제외).
 *  7) subsystem->passthrough이고 NSID가 specific이면 nvmf_passthru_admin_cmd로.
 *  8) 그 외 표준 opcode 분기: GET_LOG_PAGE/IDENTIFY/ABORT/GET_FEATURES/SET_FEATURES/AER/KEEP_ALIVE.
 *  9) CREATE/DELETE IO SQ/CQ는 NVMe-oF에서 금지 (Connect로 대체).
 *
 * 호출 컨텍스트: ctrlr->thread (assert).
 */
int
nvmf_ctrlr_process_admin_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] admin qpair의 컨트롤러 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */
	struct spdk_nvmf_subsystem_poll_group *sgroup; /* [한국어] subsystem poll group (AER 카운터 보정용) */
	int rc; /* [한국어] 커스텀 핸들러 반환값 */

	assert(ctrlr != NULL); /* [한국어] ctrlr가 없으면 안 됨 */
	if (cmd->opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
		/* We do not want to treat AERs as outstanding commands,
		 * so decrement mgmt_io_outstanding here to offset
		 * the increment that happened prior to this call.
		 */
		/* [한국어] AER은 outstanding 명령이 아니므로 진입 전 증가된 mgmt_io_outstanding를 보정 */
		sgroup = &req->qpair->group->sgroups[ctrlr->subsys->id];
		assert(sgroup != NULL);
		sgroup->mgmt_io_outstanding--; /* [한국어] AER은 mgmt_io_outstanding로 추적하지 않음 */
	}

	assert(spdk_get_thread() == ctrlr->thread); /* [한국어] ctrlr->thread에서만 admin 명령 처리 가능 */

	if (cmd->fuse != 0 ||
	    (is_cmd_ctrlr_specific(cmd) && (cmd->nsid != 0))) {
		/* Fused admin commands are not supported.
		 * Commands with controller scope - should be rejected if NSID is set.
		 */
		/* [한국어] FUSE 플래그 있거나, 컨트롤러 범위 명령에 NSID가 설정됨 → 오류 */
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (ctrlr->vcprop.cc.bits.en != 1) {
		/* [한국어] CC.EN=0(컨트롤러 비활성화 상태): admin 명령 거절 */
		SPDK_ERRLOG("Admin command sent to disabled controller\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->iovcnt && spdk_nvme_opc_get_data_transfer(cmd->opc) == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		/* [한국어] controller→host 데이터 전송 명령이면 응답 버퍼를 0으로 초기화 (보안/일관성) */
		spdk_iov_memset(req->iov, req->iovcnt, 0);
	}

	if (spdk_nvmf_subsystem_is_discovery(ctrlr->subsys)) {
		/* Discovery controllers only support these admin OPS. */
		/* [한국어] Discovery 컨트롤러는 제한된 명령만 허용 */
		switch (cmd->opc) {
		case SPDK_NVME_OPC_IDENTIFY: /* [한국어] Identify: Discovery에서 허용 */
		case SPDK_NVME_OPC_GET_LOG_PAGE: /* [한국어] Get Log Page: Discovery에서 허용 */
		case SPDK_NVME_OPC_KEEP_ALIVE: /* [한국어] Keep Alive: Discovery에서 허용 */
		case SPDK_NVME_OPC_SET_FEATURES: /* [한국어] Set Features: Discovery에서 허용 */
		case SPDK_NVME_OPC_GET_FEATURES: /* [한국어] Get Features: Discovery에서 허용 */
		case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST: /* [한국어] AER: Discovery에서 허용 */
			break;
		default:
			goto invalid_opcode; /* [한국어] 그 외 opcode는 Discovery에서 금지 */
		}
	}

	/* Call a custom adm cmd handler if set. Aborts are handled in a different path (see nvmf_passthru_admin_cmd) */
	if (g_nvmf_custom_admin_cmd_hdlrs[cmd->opc].hdlr && cmd->opc != SPDK_NVME_OPC_ABORT) {
		/* [한국어] 커스텀 핸들러가 등록된 경우 우선 호출 (Abort는 별도 경로) */
		rc = g_nvmf_custom_admin_cmd_hdlrs[cmd->opc].hdlr(req);
		if (rc >= SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
			/* The handler took care of this command */
			/* [한국어] 핸들러가 처리 완료 → 표준 경로 건너뜀 */
			return rc;
		}
	}

	/* We only want to send passthrough admin commands to namespaces.
	 * However, we don't want to passthrough a command with intended for all namespaces.
	 */
	if (ctrlr->subsys->passthrough && cmd->nsid && cmd->nsid != SPDK_NVME_GLOBAL_NS_TAG) {
		/* [한국어] subsystem passthrough 모드이고 특정 NSID 명령이면 bdev에 passthrough */
		return nvmf_passthru_admin_cmd(req);
	}

	/* [한국어] 표준 opcode별 분기 */
	switch (cmd->opc) {
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		return nvmf_ctrlr_get_log_page(req); /* [한국어] Get Log Page 처리 */
	case SPDK_NVME_OPC_IDENTIFY:
		return nvmf_ctrlr_identify(req); /* [한국어] Identify 처리 */
	case SPDK_NVME_OPC_ABORT:
		return nvmf_ctrlr_abort(req); /* [한국어] Abort 처리 */
	case SPDK_NVME_OPC_GET_FEATURES:
		return nvmf_ctrlr_get_features(req); /* [한국어] Get Features 처리 */
	case SPDK_NVME_OPC_SET_FEATURES:
		return nvmf_ctrlr_set_features(req); /* [한국어] Set Features 처리 */
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		return nvmf_ctrlr_async_event_request(req); /* [한국어] Async Event Request 처리 */
	case SPDK_NVME_OPC_KEEP_ALIVE:
		return nvmf_ctrlr_keep_alive(req); /* [한국어] Keep Alive 처리 */

	case SPDK_NVME_OPC_CREATE_IO_SQ:
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		/* Create and Delete I/O CQ/SQ not allowed in NVMe-oF */
		/* [한국어] NVMe-oF에서 SQ/CQ 생성·삭제는 Fabric Connect으로 대체됨 → 오류 */
		goto invalid_opcode;

	default:
		goto invalid_opcode; /* [한국어] 미지원 opcode */
	}

invalid_opcode:
	SPDK_INFOLOG(nvmf, "Unsupported admin opcode 0x%x\n", cmd->opc);
	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_process_fabrics_cmd - Fabric capsule 명령(opcode 7Fh) 디스패처
 *
 * @req: 요청.
 * @return: COMPLETE/ASYNCHRONOUS.
 *
 * fctype 분기:
 *  - 컨트롤러가 아직 없으면 (qpair->ctrlr==NULL) Connect만 유효 → nvmf_ctrlr_cmd_connect.
 *  - admin queue에서: PROPERTY_SET/GET, AUTHENTICATION_SEND/RECV.
 *  - IO queue에서: AUTHENTICATION_SEND/RECV만 (Property는 admin에서만).
 * 호출 컨텍스트: spdk_nvmf_request_exec()에서 호출 → qpair group 스레드.
 */
static int
nvmf_ctrlr_process_fabrics_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] 이 명령이 수신된 qpair */
	struct spdk_nvmf_capsule_cmd *cap_hdr; /* [한국어] Fabric 명령 헤더 (fctype 필드 포함) */

	cap_hdr = &req->cmd->nvmf_cmd; /* [한국어] NVMe SQE를 Fabric 명령 뷰로 해석 */

	if (qpair->ctrlr == NULL) {
		/* No ctrlr established yet; the only valid command is Connect */
		/* [한국어] 아직 컨트롤러가 없는 qpair: 반드시 Connect 명령이어야 함.
		 * Connect 이외의 명령이 이 시점에 오면 프로토콜 위반 (assert로 감지) */
		assert(cap_hdr->fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT);
		return nvmf_ctrlr_cmd_connect(req); /* [한국어] admin(qid=0) 또는 IO(qid>0) Connect 처리 */
	} else if (nvmf_qpair_is_admin_queue(qpair)) {
		/*
		 * Controller session is established, and this is an admin queue.
		 * Disallow Connect and allow other fabrics commands.
		 */
		/* [한국어] 컨트롤러 세션이 맺어진 admin qpair(qid=0): Connect 이외 Fabric 명령 처리.
		 * Property Get/Set은 admin에서만 허용 (IO qpair에서는 거절) */
		switch (cap_hdr->fctype) {
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET:
			/* [한국어] fctype=00h: 가상 NVMe 레지스터(CC/AQA/ASQ/ACQ 등)에 값 쓰기 */
			return nvmf_property_set(req);
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET:
			/* [한국어] fctype=04h: 가상 NVMe 레지스터(CAP/VS/CC/CSTS 등) 읽기 */
			return nvmf_property_get(req);
		case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND:
		case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV:
			/* [한국어] fctype=05h/06h: DH-HMAC-CHAP 인증 메시지 교환 (auth.c 처리) */
			return nvmf_auth_request_exec(req);
		default:
			/* [한국어] 알 수 없는 fctype → INVALID_OPCODE 응답 */
			SPDK_DEBUGLOG(nvmf, "unknown fctype 0x%02x\n",
				      cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else {
		/*
		 * Controller session is established, and this is an I/O queue.
		 * Disallow everything besides authentication commands.
		 */
		/* [한국어] IO qpair(qid>0)에서 Fabric 명령: 인증 메시지만 허용.
		 * Property Get/Set, Connect는 IO qpair에서 불가 */
		switch (cap_hdr->fctype) {
		case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND:
		case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV:
			/* [한국어] IO qpair에서도 DH-HMAC-CHAP 인증은 허용 (per-qpair 인증) */
			return nvmf_auth_request_exec(req);
		default:
			/* [한국어] IO qpair에서 Property Get/Set 등 admin 전용 Fabric 명령 수신 → 거절 */
			SPDK_DEBUGLOG(nvmf, "Unexpected I/O fctype 0x%x\n", cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}
}

/*
 * [한국어]
 * nvmf_ctrlr_queue_pending_async_event - AER 요청이 없을 때 이벤트를 대기 큐에 저장
 *
 * @ctrlr: 대상 컨트롤러.
 * @event: 저장할 AER 완료 이벤트.
 *
 * 호스트가 AER 요청을 아직 보내지 않았을 때 발생한 이벤트를 ctrlr->async_events 큐에 보관한다.
 * 이후 호스트가 AER을 보내면 nvmf_ctrlr_async_event_request가 큐에서 꺼내 즉시 응답한다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 */
static inline void
nvmf_ctrlr_queue_pending_async_event(struct spdk_nvmf_ctrlr *ctrlr,
				     union spdk_nvme_async_event_completion *event)
{
	struct spdk_nvmf_async_event_completion *nvmf_event; /* [한국어] 대기 큐의 이벤트 항목 */

	nvmf_event = calloc(1, sizeof(*nvmf_event)); /* [한국어] 이벤트 항목 할당 */
	if (!nvmf_event) {
		/* [한국어] 메모리 부족 → 이벤트 드롭 (호스트가 나중에 로그 페이지로 확인 가능) */
		SPDK_ERRLOG("Alloc nvmf event failed, ignore the event\n");
		return;
	}
	nvmf_event->event.raw = event->raw; /* [한국어] 이벤트 데이터 복사 */
	STAILQ_INSERT_TAIL(&ctrlr->async_events, nvmf_event, link); /* [한국어] 대기 큐 끝에 삽입 */
}

/*
 * [한국어]
 * nvmf_ctrlr_async_event_notification - AER 이벤트 전송 (또는 대기 큐에 저장)
 *
 * @ctrlr: 대상 컨트롤러.
 * @event: 전송할 AER 완료 이벤트 (raw 32비트).
 * @return: 0(성공 또는 큐에 저장).
 *
 * 대기 중인 AER 요청이 있으면 즉시 완료 응답을 보내고,
 * 없으면 ctrlr->async_events 대기 큐에 저장한다.
 * ctrlr->thread에서만 aer_req[]에 접근해야 한다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_async_event_ns_notice 등 → [이 함수] → _nvmf_request_complete
 *                                                     또는 nvmf_ctrlr_queue_pending_async_event
 */
static inline int
nvmf_ctrlr_async_event_notification(struct spdk_nvmf_ctrlr *ctrlr,
				    union spdk_nvme_async_event_completion *event)
{
	struct spdk_nvmf_request *req; /* [한국어] 완료할 AER 요청 */
	struct spdk_nvme_cpl *rsp; /* [한국어] CQE 응답 */

	assert(spdk_get_thread() == ctrlr->thread); /* [한국어] ctrlr->thread에서만 aer_req[] 접근 가능 */

	/* If there is no outstanding AER request, queue the event.  Then
	 * if an AER is later submitted, this event can be sent as a
	 * response.
	 */
	if (ctrlr->nr_aer_reqs == 0) {
		/* [한국어] 대기 중인 AER 요청 없음 → 이벤트를 대기 큐에 저장 */
		nvmf_ctrlr_queue_pending_async_event(ctrlr, event);
		return 0;
	}

	req = ctrlr->aer_req[--ctrlr->nr_aer_reqs]; /* [한국어] 마지막 AER 요청 꺼내기 (nr_aer_reqs 감소) */
	rsp = &req->rsp->nvme_cpl; /* [한국어] AER 응답 CQE */

	rsp->cdw0 = event->raw; /* [한국어] CDW0에 이벤트 정보(type/info/log_page_id) 설정 */

	_nvmf_request_complete(req); /* [한국어] AER 요청 완료 응답 전송 */
	ctrlr->aer_req[ctrlr->nr_aer_reqs] = NULL; /* [한국어] 슬롯 클리어 */

	return 0;
}

/*
 * [한국어]
 * nvmf_ctrlr_async_event_ns_notice - NS 속성 변경 AER 이벤트 전송
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 0(성공 또는 마스크/비활성화로 건너뜀).
 *
 * 호스트의 AEC(Async Event Configuration)에 ns_attr_notice 비트가 설정된 경우에만 전송.
 * AEN 마스크 비트로 중복 발송을 방지한다.
 * 이벤트 타입: NOTICE, 이벤트 정보: NS_ATTR_CHANGED, 로그 페이지: Changed NS List.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_ns_changed → [이 함수] → nvmf_ctrlr_async_event_notification
 */
int
nvmf_ctrlr_async_event_ns_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	union spdk_nvme_async_event_completion event = {0}; /* [한국어] AER 완료 이벤트 초기화 */

	/* Users may disable the event notification */
	if (!ctrlr->feat.async_event_configuration.bits.ns_attr_notice) {
		/* [한국어] 호스트가 NS 속성 변경 알림을 비활성화한 경우 → 건너뜀 */
		return 0;
	}

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGE_MASK_BIT)) {
		/* [한국어] AEN 마스크 비트 이미 설정됨 (중복 이벤트 방지) → 건너뜀 */
		return 0;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE; /* [한국어] 이벤트 타입: Notice */
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED; /* [한국어] NS 속성 변경 이벤트 */
	event.bits.log_page_identifier = SPDK_NVME_LOG_CHANGED_NS_LIST; /* [한국어] Changed NS List 로그 페이지 */

	return nvmf_ctrlr_async_event_notification(ctrlr, &event); /* [한국어] AER 이벤트 전송 */
}

/*
 * [한국어]
 * nvmf_ctrlr_async_event_ana_change_notice - ANA 상태 변경 AER 이벤트 전송
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 0(성공 또는 마스크/비활성화로 건너뜀).
 *
 * 호스트의 AEC에 ana_change_notice 비트가 설정된 경우에만 전송.
 * ANA(Asymmetric Namespace Access) 상태 변경 시 호스트에게 알려 다른 경로로 전환하도록 한다.
 * 이벤트 타입: NOTICE, 이벤트 정보: ANA_CHANGE, 로그 페이지: ANA Log Page.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_subsystem_update_ctrlr_ana_state → [이 함수] → nvmf_ctrlr_async_event_notification
 */
int
nvmf_ctrlr_async_event_ana_change_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	union spdk_nvme_async_event_completion event = {0}; /* [한국어] AER 완료 이벤트 초기화 */

	/* Users may disable the event notification */
	if (!ctrlr->feat.async_event_configuration.bits.ana_change_notice) {
		/* [한국어] 호스트가 ANA 변경 알림을 비활성화한 경우 → 건너뜀 */
		return 0;
	}

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_ANA_CHANGE_MASK_BIT)) {
		/* [한국어] AEN 마스크 비트 이미 설정됨 → 건너뜀 */
		return 0;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE; /* [한국어] 이벤트 타입: Notice */
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_ANA_CHANGE; /* [한국어] ANA 변경 이벤트 */
	event.bits.log_page_identifier = SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS; /* [한국어] ANA Log Page */

	return nvmf_ctrlr_async_event_notification(ctrlr, &event); /* [한국어] AER 이벤트 전송 */
}

/*
 * [한국어]
 * nvmf_ctrlr_async_event_reservation_notification - 예약(Reservation) 로그 가용 AER 이벤트 전송
 *
 * @ctrlr: 대상 컨트롤러.
 *
 * ctrlr->num_avail_log_pages가 0보다 크고 AEN 마스크가 설정 가능한 경우에만 전송.
 * 예약 상태 변경 시 호스트에게 Reservation Notification 로그를 읽도록 알린다.
 * 이벤트 타입: I/O, 이벤트 정보: RESERVATION_LOG_AVAIL, 로그 페이지: Reservation Notification.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   예약 명령 처리 경로 → [이 함수] → nvmf_ctrlr_async_event_notification
 */
void
nvmf_ctrlr_async_event_reservation_notification(struct spdk_nvmf_ctrlr *ctrlr)
{
	union spdk_nvme_async_event_completion event = {0}; /* [한국어] AER 완료 이벤트 초기화 */

	if (!ctrlr->num_avail_log_pages) {
		/* [한국어] 가용 Reservation 로그 페이지 없음 → 이벤트 불필요 */
		return;
	}

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL_MASK_BIT)) {
		/* [한국어] AEN 마스크 비트 이미 설정됨 → 건너뜀 */
		return;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_IO; /* [한국어] 이벤트 타입: I/O */
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL; /* [한국어] 예약 로그 가용 이벤트 */
	event.bits.log_page_identifier = SPDK_NVME_LOG_RESERVATION_NOTIFICATION; /* [한국어] Reservation Notification Log */

	nvmf_ctrlr_async_event_notification(ctrlr, &event); /* [한국어] AER 이벤트 전송 */
}

/*
 * [한국어]
 * nvmf_ctrlr_async_event_discovery_log_change_notice - Discovery 로그 변경 AER 이벤트 전송
 *
 * @ctx: 대상 컨트롤러 포인터 (spdk_thread_send_msg 콜백 시그니처).
 *
 * Discovery 컨트롤러에 등록된 호스트에게 Discovery Log Page 변경을 알리는 AER 이벤트 전송.
 * 호스트의 AEC.discovery_log_change_notice 비트가 설정된 경우에만 전송.
 * KATO=0으로 연결된 경우 이 비트가 비활성화되어 있을 수 있다.
 *
 * 실행 컨텍스트: ctrlr->thread (send_msg 콜백).
 *
 * 호출 체인:
 *   Discovery subsystem 변경 → spdk_thread_send_msg → [이 함수] → nvmf_ctrlr_async_event_notification
 */
void
nvmf_ctrlr_async_event_discovery_log_change_notice(void *ctx)
{
	union spdk_nvme_async_event_completion event = {0}; /* [한국어] AER 완료 이벤트 초기화 */
	struct spdk_nvmf_ctrlr *ctrlr = ctx; /* [한국어] 대상 컨트롤러 */

	/* Users may disable the event notification manually or
	 * it may not be enabled due to keep alive timeout
	 * not being set in connect command to discovery controller.
	 */
	if (!ctrlr->feat.async_event_configuration.bits.discovery_log_change_notice) {
		/* [한국어] Discovery 로그 변경 알림이 비활성화된 경우 → 건너뜀 */
		return;
	}

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE_MASK_BIT)) {
		/* [한국어] AEN 마스크 비트 이미 설정됨 → 건너뜀 */
		return;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE; /* [한국어] 이벤트 타입: Notice */
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE; /* [한국어] Discovery 로그 변경 이벤트 */
	event.bits.log_page_identifier = SPDK_NVME_LOG_DISCOVERY; /* [한국어] Discovery Log Page ID */

	nvmf_ctrlr_async_event_notification(ctrlr, &event); /* [한국어] AER 이벤트 전송 */
}

/*
 * [한국어]
 * spdk_nvmf_ctrlr_async_event_error_event - Error 타입 AER 이벤트 전송 (공개 API)
 *
 * @ctrlr: 대상 컨트롤러.
 * @info: 오류 이벤트 정보 코드 (FW_IMAGE_LOAD 이하 값만 허용).
 * @return: 0(성공 또는 마스크로 건너뜀).
 *
 * Error Log Page에 오류가 기록될 때 호스트에게 알리기 위한 AER 이벤트 전송.
 * AEN 마스크 비트로 중복 발송을 방지하고, info가 유효 범위를 벗어나면 전송하지 않는다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   오류 발생 경로 → [이 함수] → nvmf_ctrlr_async_event_notification
 */
int
spdk_nvmf_ctrlr_async_event_error_event(struct spdk_nvmf_ctrlr *ctrlr,
					enum spdk_nvme_async_event_info_error info)
{
	union spdk_nvme_async_event_completion event; /* [한국어] AER 완료 이벤트 */

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_ERROR_MASK_BIT)) {
		/* [한국어] AEN 마스크 비트 이미 설정됨 → 건너뜀 */
		return 0;
	}

	if (info > SPDK_NVME_ASYNC_EVENT_FW_IMAGE_LOAD) {
		/* [한국어] 유효하지 않은 info 코드 → 전송하지 않음 */
		return 0;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_ERROR; /* [한국어] 이벤트 타입: Error */
	event.bits.log_page_identifier = SPDK_NVME_LOG_ERROR; /* [한국어] Error Log Page ID */
	event.bits.async_event_info = info; /* [한국어] 오류 이벤트 정보 코드 */

	return nvmf_ctrlr_async_event_notification(ctrlr, &event); /* [한국어] AER 이벤트 전송 */
}

/*
 * [한국어]
 * nvmf_qpair_free_aer - admin qpair의 대기 중인 AER 요청 메모리 해제
 *
 * @qpair: 정리할 qpair.
 *
 * qpair disconnect 시 admin qpair에 남아있는 AER 요청을 spdk_nvmf_request_free로 해제한다.
 * 완료 응답 없이 메모리만 해제하므로 transport 레이어에서 자원을 돌려받는 목적.
 * admin qpair가 아니거나 ctrlr가 없으면 즉시 반환.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   qpair disconnect 경로 → [이 함수] → spdk_nvmf_request_free
 */
void
nvmf_qpair_free_aer(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr; /* [한국어] qpair의 컨트롤러 */
	int i; /* [한국어] aer_req[] 순회 인덱스 */

	if (ctrlr == NULL || !nvmf_qpair_is_admin_queue(qpair)) {
		/* [한국어] ctrlr 없거나 admin qpair가 아니면 AER 없음 */
		return;
	}

	assert(spdk_get_thread() == ctrlr->thread); /* [한국어] ctrlr->thread에서만 aer_req[] 접근 가능 */

	for (i = 0; i < ctrlr->nr_aer_reqs; i++) {
		spdk_nvmf_request_free(ctrlr->aer_req[i]); /* [한국어] AER 요청 메모리 해제 (응답 없이) */
		ctrlr->aer_req[i] = NULL; /* [한국어] 슬롯 클리어 */
	}

	ctrlr->nr_aer_reqs = 0; /* [한국어] AER 슬롯 수 초기화 */
}

/*
 * [한국어]
 * spdk_nvmf_ctrlr_abort_aer - 컨트롤러의 대기 중인 AER 요청 모두 Abort
 *
 * @ctrlr: 대상 컨트롤러.
 *
 * 컨트롤러 shutdown/reset 시 대기 중인 모든 AER 요청을 ABORTED_BY_REQUEST 상태로 완료한다.
 * 완료 응답을 보낸 후 aer_req[] 슬롯을 클리어하고 nr_aer_reqs를 0으로 초기화.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   ctrlr shutdown/destroy 경로 → [이 함수] → _nvmf_request_complete
 */
void
spdk_nvmf_ctrlr_abort_aer(struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_request *req; /* [한국어] Abort할 AER 요청 */
	int i; /* [한국어] aer_req[] 순회 인덱스 */

	assert(spdk_get_thread() == ctrlr->thread); /* [한국어] ctrlr->thread에서만 aer_req[] 접근 가능 */

	if (!ctrlr->nr_aer_reqs) {
		/* [한국어] 대기 중인 AER 없음 → 즉시 반환 */
		return;
	}

	for (i = 0; i < ctrlr->nr_aer_reqs; i++) {
		req = ctrlr->aer_req[i]; /* [한국어] i번째 AER 요청 */

		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] Generic 상태 타입 */
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST; /* [한국어] Abort 상태 설정 */
		_nvmf_request_complete(req); /* [한국어] Abort 응답 전송 */

		ctrlr->aer_req[i] = NULL; /* [한국어] 슬롯 클리어 */
	}

	ctrlr->nr_aer_reqs = 0; /* [한국어] AER 슬롯 수 초기화 */
}

/*
 * [한국어]
 * _nvmf_ctrlr_add_reservation_log - Reservation Notification Log 페이지를 ctrlr 큐에 추가
 *
 * @ctx: spdk_nvmf_reservation_log 포인터 (spdk_thread_send_msg로 전달).
 *
 * ctrlr->thread에서 실행되는 send_msg 콜백.
 * 전역 log_page_count를 증가시키고 새 로그 엔트리를 ctrlr->log_head 큐에 추가한다.
 * 큐 최대 크기(0xFF)에 도달하면 마지막 엔트리의 log_page_count만 갱신하고 새 엔트리는 버린다.
 * 엔트리 추가 후 nvmf_ctrlr_async_event_reservation_notification으로 AER 이벤트를 발생시킨다.
 *
 * 실행 컨텍스트: ctrlr->thread (send_msg 콜백).
 *
 * 호출 체인:
 *   nvmf_ctrlr_reservation_notice_log → spdk_thread_send_msg → [이 함수]
 *                                                              → nvmf_ctrlr_async_event_reservation_notification
 */
static void
_nvmf_ctrlr_add_reservation_log(void *ctx)
{
	struct spdk_nvmf_reservation_log *log = (struct spdk_nvmf_reservation_log *)ctx; /* [한국어] 추가할 로그 엔트리 */
	struct spdk_nvmf_ctrlr *ctrlr = log->ctrlr; /* [한국어] 대상 컨트롤러 */

	if (spdk_unlikely(ctrlr->log_page_count == UINT64_MAX)) {
		/* [한국어] 카운터 오버플로 방지: UINT64_MAX에 도달하면 0으로 리셋 */
		ctrlr->log_page_count = 0;
	}

	ctrlr->log_page_count++; /* [한국어] 전역 로그 페이지 카운터 증가 */

	/* Maximum number of queued log pages is 255 */
	if (ctrlr->num_avail_log_pages == 0xff) {
		/* [한국어] 큐가 최대(255) 용량에 도달: 새 엔트리 대신 마지막 엔트리의 카운터 갱신 */
		struct spdk_nvmf_reservation_log *entry;
		entry = TAILQ_LAST(&ctrlr->log_head, log_page_head); /* [한국어] 큐의 마지막 엔트리 */
		entry->log.log_page_count = ctrlr->log_page_count; /* [한국어] 카운터 갱신 */
		free(log); /* [한국어] 새 엔트리는 버림 */
		return;
	}

	log->log.log_page_count = ctrlr->log_page_count; /* [한국어] 엔트리에 로그 카운터 기록 */
	log->log.num_avail_log_pages = ctrlr->num_avail_log_pages++; /* [한국어] 가용 로그 수 기록 후 증가 */
	TAILQ_INSERT_TAIL(&ctrlr->log_head, log, link); /* [한국어] 큐 끝에 로그 엔트리 삽입 */

	nvmf_ctrlr_async_event_reservation_notification(ctrlr); /* [한국어] AER 이벤트 발생 */
}

/*
 * [한국어]
 * nvmf_ctrlr_reservation_notice_log - 예약 알림 로그를 컨트롤러에 기록
 *
 * @ctrlr: 대상 컨트롤러.
 * @ns: 예약 이벤트가 발생한 NS.
 * @type: 로그 페이지 타입 (REGISTRATION_PREEMPTED/RESERVATION_RELEASED/PREEMPTED 등).
 *
 * NS의 예약 이벤트를 Reservation Notification Log Page에 기록한다.
 * NS의 마스크 비트로 해당 타입의 알림이 비활성화된 경우 기록하지 않는다.
 * _nvmf_ctrlr_add_reservation_log를 ctrlr->thread에 send_msg로 전달하여 thread-safe하게 처리한다.
 *
 * 실행 컨텍스트: 임의 thread (send_msg로 ctrlr->thread에 위임).
 *
 * 호출 체인:
 *   예약 명령 처리 경로 → [이 함수] → spdk_thread_send_msg → _nvmf_ctrlr_add_reservation_log
 */
void
nvmf_ctrlr_reservation_notice_log(struct spdk_nvmf_ctrlr *ctrlr,
				  struct spdk_nvmf_ns *ns,
				  enum spdk_nvme_reservation_notification_log_page_type type)
{
	struct spdk_nvmf_reservation_log *log; /* [한국어] 새 로그 엔트리 */

	switch (type) {
	case SPDK_NVME_RESERVATION_LOG_PAGE_EMPTY:
		return; /* [한국어] 빈 타입: 기록할 내용 없음 */
	case SPDK_NVME_REGISTRATION_PREEMPTED:
		if (ns->mask & SPDK_NVME_REGISTRATION_PREEMPTED_MASK) {
			/* [한국어] NS 마스크: Registration Preempted 알림 비활성화 */
			return;
		}
		break;
	case SPDK_NVME_RESERVATION_RELEASED:
		if (ns->mask & SPDK_NVME_RESERVATION_RELEASED_MASK) {
			/* [한국어] NS 마스크: Reservation Released 알림 비활성화 */
			return;
		}
		break;
	case SPDK_NVME_RESERVATION_PREEMPTED:
		if (ns->mask & SPDK_NVME_RESERVATION_PREEMPTED_MASK) {
			/* [한국어] NS 마스크: Reservation Preempted 알림 비활성화 */
			return;
		}
		break;
	default:
		return; /* [한국어] 미지원 타입 */
	}

	log = calloc(1, sizeof(*log)); /* [한국어] 로그 엔트리 할당 */
	if (!log) {
		/* [한국어] 메모리 부족 → 로그 드롭 */
		SPDK_ERRLOG("Alloc log page failed, ignore the log\n");
		return;
	}
	log->ctrlr = ctrlr; /* [한국어] 로그를 소유할 컨트롤러 */
	log->log.type = type; /* [한국어] 로그 타입 설정 */
	log->log.nsid = ns->nsid; /* [한국어] 이벤트 발생 NS의 NSID */

	/* [한국어] ctrlr->thread에 로그 추가 작업 위임 (thread-safe) */
	spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_add_reservation_log, log);
}

/* Check from subsystem poll group's namespace information data structure */
/*
 * [한국어]
 * nvmf_ns_info_ctrlr_is_registrant - 컨트롤러의 hostid가 NS의 예약 등록자 목록에 있는지 확인
 *
 * @ns_info: poll group의 NS 정보 (reg_hostid[] 배열 포함).
 * @ctrlr: 확인할 컨트롤러.
 * @return: true이면 등록자, false이면 비등록자.
 *
 * 예약 명령(Acquire/Release/Report/IO) 처리 시 현재 컨트롤러가 해당 NS의
 * 예약 등록자(Registrant)인지 확인하는 데 사용한다.
 *
 * 실행 컨텍스트: qpair의 poll group thread.
 *
 * 호출 체인:
 *   nvmf_ns_reservation_request_check → [이 함수]
 */
static bool
nvmf_ns_info_ctrlr_is_registrant(struct spdk_nvmf_subsystem_pg_ns_info *ns_info,
				 struct spdk_nvmf_ctrlr *ctrlr)
{
	uint32_t i; /* [한국어] 등록자 목록 순회 인덱스 */

	for (i = 0; i < SPDK_NVMF_MAX_NUM_REGISTRANTS; i++) {
		if (!spdk_uuid_compare(&ns_info->reg_hostid[i], &ctrlr->hostid)) {
			/* [한국어] 등록된 hostid와 현재 컨트롤러의 hostid가 일치 */
			return true;
		}
	}

	return false; /* [한국어] 등록자 목록에서 일치하는 hostid 없음 */
}

/*
 * Check the NVMe command is permitted or not for current controller(Host).
 */
/*
 * [한국어]
 * nvmf_ns_reservation_request_check - 현재 컨트롤러의 NS I/O 명령 예약 허용 여부 검사
 *
 * @ns_info: poll group의 NS 정보 (rtype, holder_id, reg_hostid 포함).
 * @ctrlr: 명령을 발행한 컨트롤러.
 * @req: 검사할 I/O 요청.
 * @return: 0(허용), -EPERM(RESERVATION_CONFLICT).
 *
 * NS에 예약이 설정된 경우 현재 컨트롤러가 해당 명령을 실행할 수 있는지
 * NVMe 스펙 §8.19의 예약 규칙에 따라 검사한다.
 *
 * 허용 조건:
 *   - 예약 없음(rtype=0): 항상 허용
 *   - ALL_REGS 타입이고 등록자인 경우: 허용
 *   - 현재 컨트롤러가 holder인 경우: 허용
 *   - 그 외: opcode별 예약 타입에 따라 RESERVATION_CONFLICT 반환
 *
 * opcode별 규칙:
 *   - Read/Compare: EXCLUSIVE_ACCESS이면 충돌
 *   - Write 계열: WRITE_EXCLUSIVE 또는 EXCLUSIVE_ACCESS이면 충돌
 *   - Reservation Acquire: Acquire 동작(RACQA=0)이면 충돌
 *   - Reservation Release: 비등록자이면 충돌
 *
 * 실행 컨텍스트: qpair의 poll group thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_process_io_cmd → [이 함수]
 */
static int
nvmf_ns_reservation_request_check(struct spdk_nvmf_subsystem_pg_ns_info *ns_info,
				  struct spdk_nvmf_ctrlr *ctrlr,
				  struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */
	enum spdk_nvme_reservation_type rtype = ns_info->rtype; /* [한국어] 현재 예약 타입 */
	uint8_t status = SPDK_NVME_SC_SUCCESS; /* [한국어] 기본 상태: SUCCESS */
	uint8_t racqa; /* [한국어] Reservation Acquire Command Action (Acquire 명령용) */
	bool is_registrant; /* [한국어] 현재 컨트롤러가 등록자인지 여부 */

	/* No valid reservation */
	if (!rtype) {
		/* [한국어] 예약 없음 → 모든 명령 허용 */
		return 0;
	}

	is_registrant = nvmf_ns_info_ctrlr_is_registrant(ns_info, ctrlr); /* [한국어] 등록자 여부 확인 */
	/* All registrants type and current ctrlr is a valid registrant */
	if ((rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS ||
	     rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS) && is_registrant) {
		/* [한국어] ALL_REGS 타입이고 등록자: 허용 */
		return 0;
	} else if (!spdk_uuid_compare(&ns_info->holder_id, &ctrlr->hostid)) {
		/* [한국어] 현재 컨트롤러가 예약 holder: 허용 */
		return 0;
	}

	/* Non-holder for current controller */
	/* [한국어] 현재 컨트롤러는 holder가 아님: opcode별 예약 규칙 적용 */
	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_COMPARE:
		if (rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS) {
			/* [한국어] EXCLUSIVE_ACCESS: 비holder Read/Compare → 충돌 */
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}
		if ((rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY ||
		     rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS) && !is_registrant) {
			/* [한국어] EXCLUSIVE_ACCESS_REG_ONLY/_ALL_REGS: 비등록자 Read/Compare → 충돌 */
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		}
		break;
	case SPDK_NVME_OPC_FLUSH:
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
	case SPDK_NVME_OPC_WRITE_ZEROES:
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		if (rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE ||
		    rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS) {
			/* [한국어] WRITE_EXCLUSIVE/_EXCLUSIVE_ACCESS: 비holder 쓰기 계열 → 충돌 */
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}
		if (!is_registrant) {
			/* [한국어] 그 외 타입에서 비등록자 쓰기 → 충돌 */
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		}
		break;
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		racqa = cmd->cdw10_bits.resv_acquire.racqa; /* [한국어] Acquire 동작 코드 */
		if (racqa == SPDK_NVME_RESERVE_ACQUIRE) {
			/* [한국어] Acquire 동작(RACQA=0): 비holder는 새로 Acquire 불가 */
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}
		if (!is_registrant) {
			/* [한국어] Preempt/Preempt-and-Abort(RACQA=1/2): 비등록자는 불가 */
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		}
		break;
	case SPDK_NVME_OPC_RESERVATION_RELEASE:
		if (!is_registrant) {
			/* [한국어] Release: 비등록자는 불가 */
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		}
		break;
	default:
		break; /* [한국어] 그 외 명령: 예약 규칙 미적용 */
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] Generic 상태 타입 */
	req->rsp->nvme_cpl.status.sc = status; /* [한국어] 상태 코드 설정 */
	if (status == SPDK_NVME_SC_RESERVATION_CONFLICT) {
		return -EPERM; /* [한국어] 충돌: 상위 함수가 요청 중단 처리 */
	}

	return 0; /* [한국어] 허용 */
}

/*
 * [한국어]
 * nvmf_ctrlr_process_io_fused_cmd - Fused 명령(Compare+Write) 처리
 *
 * @req: IO 명령 요청 (FUSE_FIRST 또는 FUSE_SECOND).
 * @bdev: 대상 bdev.
 * @desc: bdev descriptor.
 * @ch: bdev I/O channel.
 * @return: COMPLETE/ASYNCHRONOUS.
 *
 * NVMe Fused 명령은 Compare(FUSE_FIRST) + Write(FUSE_SECOND) 쌍으로 구성되어
 * 원자적으로 실행되어야 한다(Compare-and-Write).
 *
 * 처리 흐름:
 *   FUSE_FIRST(Compare):
 *     - first_fused_req가 이미 있으면 기존 요청을 ABORTED_MISSING_FUSED로 완료하고 현재로 교체.
 *     - opcode가 COMPARE가 아니면 INVALID_OPCODE.
 *     - qpair->first_fused_req에 저장하고 ASYNCHRONOUS 반환 (Write가 올 때까지 대기).
 *   FUSE_SECOND(Write):
 *     - first_fused_req가 없으면 ABORTED_MISSING_FUSED.
 *     - opcode가 WRITE가 아니면 INVALID_OPCODE + 첫 요청 Abort.
 *     - req->first_fused_req에 첫 요청 저장 후 nvmf_bdev_ctrlr_compare_and_write_cmd 호출.
 *     - Compare 실패 시 첫 요청에 오류 전파, Write는 ABORTED_FAILED_FUSED.
 *
 * 실행 컨텍스트: qpair->group->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_process_io_cmd → [이 함수] → nvmf_bdev_ctrlr_compare_and_write_cmd
 */
static int
nvmf_ctrlr_process_io_fused_cmd(struct spdk_nvmf_request *req, struct spdk_bdev *bdev,
				struct spdk_bdev_desc *desc, struct spdk_io_channel *ch)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */
	struct spdk_nvmf_request *first_fused_req = req->qpair->first_fused_req; /* [한국어] 이전에 저장된 FUSE_FIRST 요청 */
	int rc; /* [한국어] compare_and_write 반환값 */

	if (spdk_unlikely(!req->qpair->ctrlr->cdata.fuses.fcws)) {
		/* [한국어] 컨트롤러가 Fused 명령(FUSES.FCWS 비트)을 지원하지 않음 */
		SPDK_DEBUGLOG(nvmf, "Controller does not support fused operation.\n");
		goto invalid_field;
	}

	if (cmd->fuse == SPDK_NVME_CMD_FUSE_FIRST) {
		/* first fused operation (should be compare) */
		if (first_fused_req != NULL) {
			/* [한국어] 이미 FUSE_FIRST가 대기 중인데 새 FUSE_FIRST 도착 → 기존 요청 Abort */
			struct spdk_nvme_cpl *fused_response = &first_fused_req->rsp->nvme_cpl;

			SPDK_ERRLOG("Wrong sequence of fused operations\n");

			/* abort req->qpair->first_fused_request and continue with new fused command */
			/* [한국어] 기존 FUSE_FIRST를 ABORTED_MISSING_FUSED로 완료 */
			fused_response->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
			fused_response->status.sct = SPDK_NVME_SCT_GENERIC;
			_nvmf_request_complete(first_fused_req);
		} else if (cmd->opc != SPDK_NVME_OPC_COMPARE) {
			/* [한국어] FUSE_FIRST인데 Compare가 아닌 명령 → INVALID_OPCODE */
			SPDK_ERRLOG("Wrong op code of fused operations\n");
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		req->qpair->first_fused_req = req; /* [한국어] Compare 요청을 대기 중으로 저장 */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS; /* [한국어] Write가 올 때까지 대기 */
	} else if (cmd->fuse == SPDK_NVME_CMD_FUSE_SECOND) {
		/* second fused operation (should be write) */
		if (first_fused_req == NULL) {
			/* [한국어] FUSE_SECOND인데 이전 FUSE_FIRST 없음 → 시퀀스 오류 */
			SPDK_ERRLOG("Wrong sequence of fused operations\n");
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else if (cmd->opc != SPDK_NVME_OPC_WRITE) {
			/* [한국어] FUSE_SECOND인데 Write가 아닌 명령 → 첫 요청 Abort + 현재 요청 오류 */
			struct spdk_nvme_cpl *fused_response = &first_fused_req->rsp->nvme_cpl;

			SPDK_ERRLOG("Wrong op code of fused operations\n");

			/* abort req->qpair->first_fused_request and fail current command */
			/* [한국어] 첫 요청을 ABORTED_MISSING_FUSED로 Abort */
			fused_response->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
			fused_response->status.sct = SPDK_NVME_SCT_GENERIC;
			_nvmf_request_complete(first_fused_req);

			/* [한국어] 현재 요청도 INVALID_OPCODE로 완료 */
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			req->qpair->first_fused_req = NULL; /* [한국어] 대기 중인 FUSE_FIRST 포인터 클리어 */
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		/* save request of first command to generate response later */
		/* [한국어] 첫 요청(Compare)을 FUSE_SECOND 요청에 연결 (완료 시 응답 생성용) */
		req->first_fused_req = first_fused_req;
		req->first_fused = true; /* [한국어] 이 요청이 Fused 쌍의 두 번째임 표시 */
		req->qpair->first_fused_req = NULL; /* [한국어] qpair의 대기 중 포인터 클리어 */
	} else {
		/* [한국어] FUSE 필드가 FIRST도 SECOND도 아닌 잘못된 값 */
		SPDK_ERRLOG("Invalid fused command fuse field.\n");
		goto invalid_field;
	}

	/* [한국어] Compare+Write 원자 명령 실행 (bdev 레이어에 위임) */
	rc = nvmf_bdev_ctrlr_compare_and_write_cmd(bdev, desc, ch, req->first_fused_req, req);

	if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		if (spdk_nvme_cpl_is_error(rsp)) {
			/* [한국어] Compare 또는 Write 실패: 첫 요청에 오류 상태 전파 */
			struct spdk_nvme_cpl *fused_response = &first_fused_req->rsp->nvme_cpl;

			fused_response->status = rsp->status; /* [한국어] 첫 요청에 오류 상태 복사 */
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED; /* [한국어] Write는 ABORTED_FAILED_FUSED */
			/* Complete first of fused commands. Second will be completed by upper layer */
			/* [한국어] 첫 요청(Compare) 즉시 완료, 두 번째 요청(Write)는 상위에서 완료 */
			_nvmf_request_complete(first_fused_req);
			req->first_fused_req = NULL; /* [한국어] 연결 포인터 클리어 */
			req->first_fused = false; /* [한국어] Fused 표시 클리어 */
		}
	}

	return rc;

invalid_field:
	rsp->status.sct = SPDK_NVME_SCT_GENERIC; /* [한국어] Generic 상태 타입 */
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD; /* [한국어] INVALID_FIELD */
	rsp->status.dnr = 1; /* [한국어] DNR(Do Not Retry)=1: 재시도해도 소용없음 */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_ctrlr_use_zcopy - 이 요청에 zero-copy 경로를 사용할지 판단
 *
 * @req: IO 명령 요청.
 * @return: true이면 zcopy 사용 가능, false이면 일반 버퍼 복사 경로 사용.
 *
 * Zero-copy(zcopy)는 transport가 bdev의 메모리를 직접 사용하여 데이터를 전송하는
 * 최적화 경로이다. 별도의 버퍼 할당·복사 없이 bdev DMA 버퍼를 직접 노출한다.
 *
 * 조건: transport->opts.zcopy 활성화 + IO qpair + READ/WRITE 명령 + 비-Fused + NS의 zcopy 지원.
 * 통과하면 req->zcopy_phase를 INIT으로 설정하고 true를 반환한다.
 *
 * 실행 컨텍스트: qpair->group->thread (IO 명령 처리 중).
 *
 * 호출 체인:
 *   nvmf_ctrlr_process_io_cmd → 본 함수 → [zcopy 경로: nvmf_bdev_ctrlr_zcopy_start]
 */
bool
nvmf_ctrlr_use_zcopy(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_transport *transport = req->qpair->transport; /* [한국어] 현재 transport */
	struct spdk_nvmf_ns *ns; /* [한국어] 요청된 NS */

	assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_NONE); /* [한국어] zcopy 이전 상태여야 함 */

	if (!transport->opts.zcopy) {
		/* [한국어] transport 레벨에서 zcopy 비활성화 */
		return false;
	}

	if (nvmf_qpair_is_admin_queue(req->qpair)) {
		/* Admin queue */
		/* [한국어] Admin 명령에는 zcopy 사용 안 함 */
		return false;
	}

	if ((req->cmd->nvme_cmd.opc != SPDK_NVME_OPC_WRITE) &&
	    (req->cmd->nvme_cmd.opc != SPDK_NVME_OPC_READ)) {
		/* Not a READ or WRITE command */
		/* [한국어] READ/WRITE 이외 명령 (DSM, Flush 등)은 zcopy 미지원 */
		return false;
	}

	if (req->cmd->nvme_cmd.fuse != SPDK_NVME_CMD_FUSE_NONE) {
		/* Fused commands dont use zcopy buffers */
		/* [한국어] Fused 명령(Compare+Write)은 별도 버퍼 필요 → zcopy 미사용 */
		return false;
	}

	ns = nvmf_ctrlr_get_ns(req->qpair->ctrlr, req->cmd->nvme_cmd.nsid); /* [한국어] NSID → NS 룩업 */
	if (ns == NULL || ns->bdev == NULL || !ns->zcopy) {
		/* [한국어] NS가 없거나 bdev가 없거나 NS 단위 zcopy 설정이 비활성화 */
		return false;
	}

	req->zcopy_phase = NVMF_ZCOPY_PHASE_INIT; /* [한국어] zcopy 초기화 단계로 진입 */
	return true;
}

/*
 * [한국어]
 * spdk_nvmf_request_zcopy_start - zcopy 버퍼 획득 단계 시작
 *
 * @req: zcopy_phase=INIT 상태의 요청.
 *
 * transport가 bdev로부터 zero-copy 버퍼를 받기 위해 spdk_nvmf_request_exec()를
 * INIT 단계에서 재호출한다. iovcnt를 NVMF_REQ_MAX_BUFFERS로 설정하여 bdev가
 * 제공할 수 있는 최대 iov 수를 수용한다.
 *
 * 실행 컨텍스트: qpair->group->thread.
 *
 * 호출 체인:
 *   transport [zcopy_start 콜백] → 본 함수 → spdk_nvmf_request_exec
 */
void
spdk_nvmf_request_zcopy_start(struct spdk_nvmf_request *req)
{
	assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_INIT); /* [한국어] INIT 단계에서만 호출 가능 */

	/* Set iovcnt to be the maximum number of iovs that the ZCOPY can use */
	req->iovcnt = NVMF_REQ_MAX_BUFFERS; /* [한국어] bdev가 사용할 수 있는 최대 iov 수로 설정 */

	spdk_nvmf_request_exec(req); /* [한국어] INIT 단계 처리: bdev로부터 zcopy 버퍼 받기 */
}

/*
 * [한국어]
 * spdk_nvmf_request_zcopy_end - zcopy 버퍼 반환 단계 시작 (I/O 완료 후)
 *
 * @req: zcopy_phase=EXECUTE 상태의 요청.
 * @commit: true이면 쓰기 커밋(write 완료), false이면 폐기(read 완료 또는 abort).
 *
 * transport가 데이터 전송을 완료한 후 bdev에게 zcopy 버퍼를 돌려주기 위해 호출.
 * EXECUTE → END_PENDING 단계로 전환 후 nvmf_bdev_ctrlr_zcopy_end()에 위임.
 *
 * 실행 컨텍스트: qpair->group->thread.
 *
 * 호출 체인:
 *   transport [zcopy_end 콜백] → 본 함수 → nvmf_bdev_ctrlr_zcopy_end
 */
void
spdk_nvmf_request_zcopy_end(struct spdk_nvmf_request *req, bool commit)
{
	assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE); /* [한국어] EXECUTE 단계에서만 호출 가능 */
	req->zcopy_phase = NVMF_ZCOPY_PHASE_END_PENDING; /* [한국어] 버퍼 반환 대기 단계로 전환 */

	nvmf_bdev_ctrlr_zcopy_end(req, commit); /* [한국어] bdev에 zcopy 버퍼 반환 (commit 여부 전달) */
}

/*
 * [한국어]
 * nvmf_ctrlr_process_io_cmd - IO queue(qid>0)로 들어온 NVMe IO 명령 디스패처
 *
 * @req: 요청.
 * @return: COMPLETE/ASYNCHRONOUS.
 *
 * 흐름:
 *  1) CC.EN=0이면 COMMAND_SEQUENCE_ERROR.
 *  2) cmd->nsid → spdk_nvmf_ns 룩업. 없으면 INVALID_NAMESPACE_OR_FORMAT (DNR=1).
 *  3) ANA 상태가 OPTIMIZED/NON_OPTIMIZED가 아니면 PATH 에러로 응답.
 *  4) ns_info(per-PG namespace info) 가져와 reservation 충돌 체크.
 *  5) FUSE 분기: nvmf_ctrlr_process_io_fused_cmd로 위임 (compare→write 결합).
 *  6) subsystem->passthrough면 cmd->nsid를 ns->passthru_nsid로 치환 후 passthru 경로.
 *  7) zcopy enable이면 nvmf_bdev_ctrlr_zcopy_start.
 *  8) 그 외 opcode 분기: READ/WRITE/FLUSH/COMPARE/WRITE_ZEROES/DSM/RESERVATION_*/COPY 등.
 *
 * 호출 컨텍스트: qpair->group->thread (각 IO qpair는 하나의 PG에 고정).
 */
int
nvmf_ctrlr_process_io_cmd(struct spdk_nvmf_request *req)
{
	uint32_t nsid; /* [한국어] 명령 SQE의 nsid 필드 복사본 (룩업 및 검증에 사용) */
	struct spdk_nvmf_ns *ns; /* [한국어] nsid에 대응하는 NVM 네임스페이스 포인터 */
	struct spdk_bdev *bdev; /* [한국어] ns가 가리키는 bdev (블록 장치 추상화) */
	struct spdk_bdev_desc *desc; /* [한국어] ns->bdev에 대한 오픈된 desc (I/O 발행 핸들) */
	struct spdk_io_channel *ch; /* [한국어] ns_info에서 가져온 이 poll group의 bdev I/O 채널 */
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] 이 IO 명령이 도착한 IO qpair */
	struct spdk_nvmf_poll_group *group = qpair->group; /* [한국어] qpair가 소속된 poll group (per-reactor) */
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr; /* [한국어] qpair에 결합된 컨트롤러 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd; /* [한국어] NVMe SQE 구조체 (opc, nsid, fuse, cdw* 포함) */
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl; /* [한국어] NVMe CQE 응답 (status.sct/sc/dnr 설정 대상) */
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info; /* [한국어] poll group별 ns별 channel + io_outstanding + reservation 정보 */
	enum spdk_nvme_ana_state ana_state; /* [한국어] 이 ctrlr에서 ns->anagrpid의 ANA(Asymmetric Namespace Access) 상태 */

	/* pre-set response details for this command */
	/* [한국어] 기본 성공 응답으로 초기화. 오류 경로에서 sct/sc/dnr를 덮어씀 */
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	nsid = cmd->nsid; /* [한국어] SQE의 nsid 필드 읽기 (0=브로드캐스트 금지, 유효범위 1..max_nsid) */

	assert(ctrlr != NULL); /* [한국어] IO qpair에는 반드시 ctrlr가 결합되어 있어야 함 */
	if (spdk_unlikely(ctrlr->vcprop.cc.bits.en != 1)) {
		/* [한국어] CC.EN=0이면 컨트롤러가 아직 활성화되지 않은 상태 → IO 명령 거절 */
		SPDK_ERRLOG("I/O command sent to disabled controller\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR; /* [한국어] Command Sequence Error: 잘못된 순서의 명령 */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = nvmf_ctrlr_get_ns(ctrlr, nsid); /* [한국어] visible_ns 비트맵과 subsys->ns 배열로 nsid→ns 변환 */
	if (spdk_unlikely(ns == NULL || ns->bdev == NULL)) {
		/* [한국어] ns가 없거나 bdev가 없으면(연결 해제/제거 중) → INVALID_NAMESPACE DNR=1 반환 */
		SPDK_DEBUGLOG(nvmf, "Unsuccessful query for nsid %u\n", cmd->nsid);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT; /* [한국어] 존재하지 않는 NSID */
		response->status.dnr = 1; /* [한국어] DNR(Do Not Retry)=1: 재시도해도 같은 결과 */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ana_state = nvmf_ctrlr_get_ana_state(ctrlr, ns->anagrpid); /* [한국어] ctrlr->listener->ana_state[]에서 anagrpid 인덱스로 조회 */
	if (spdk_unlikely(ana_state != SPDK_NVME_ANA_OPTIMIZED_STATE &&
			  ana_state != SPDK_NVME_ANA_NON_OPTIMIZED_STATE)) {
		/* [한국어] ANA 상태가 INACCESSIBLE/CHANGE/PERSISTENT_LOSS이면 PATH 오류로 거절.
		 * 호스트 다중경로 드라이버(MPIO)가 다른 경로로 재시도하도록 PATH SCT를 사용함 */
		SPDK_DEBUGLOG(nvmf, "Fail I/O command due to ANA state %d\n",
			      ana_state);
		response->status.sct = SPDK_NVME_SCT_PATH; /* [한국어] NVMe 1.4 SCT=4h: Path Related Status */
		response->status.sc = _nvme_ana_state_to_path_status(ana_state); /* [한국어] ANA 상태→Path SC 변환 (INACCESSIBLE→01h 등) */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_likely(nvmf_subsystem_listener_is_active(ctrlr->listener))) {
		/* [한국어] SPDK DTrace 프로브: IO 명령이 어느 listener(IP:port)를 통해 들어왔는지 추적 */
		SPDK_DTRACE_PROBE3_TICKS(nvmf_request_io_exec_path, req,
					 ctrlr->listener->trid->traddr,
					 ctrlr->listener->trid->trsvcid);
	}

	/* scan-build falsely reporting dereference of null pointer */
	/* [한국어] poll group이 반드시 존재해야 함. scan-build의 false positive를 억제하기 위한 assert */
	assert(group != NULL && group->sgroups != NULL);
	ns_info = &group->sgroups[ctrlr->subsys->id].ns_info[nsid - 1]; /* [한국어] subsys->id로 sgroup 선택 후 nsid-1 인덱스로 ns_info 접근 (nsid는 1-based) */
	if (nvmf_ns_reservation_request_check(ns_info, ctrlr, req)) {
		/* [한국어] Reservation 충돌 → RESERVATION_CONFLICT SC로 응답하고 완료.
		 * reservation_waiting 카운터 증가 후 응답 큐 진입됨 (nvmf_ns_reservation_request_check 내부) */
		SPDK_DEBUGLOG(nvmf, "Reservation Conflict for nsid %u, opcode %u\n",
			      cmd->nsid, cmd->opc);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = ns->bdev; /* [한국어] 이 ns의 블록 장치 (Read/Write/Flush 등을 실제 처리) */
	desc = ns->desc; /* [한국어] bdev 오픈 핸들 (spdk_bdev_open_ext로 열린 desc, I/O 제출에 사용) */
	ch = ns_info->channel; /* [한국어] 이 poll group에서 ns->bdev에 대해 열린 I/O 채널 (qpair당 1개) */

	if (spdk_unlikely(cmd->fuse & SPDK_NVME_CMD_FUSE_MASK)) {
		/* [한국어] FUSE 비트(FIRST=01b, SECOND=10b) 설정됨 → Compare+Write 원자적 쌍 처리
		 * nvmf_ctrlr_process_io_fused_cmd이 FIRST를 저장하고 SECOND가 오면 함께 실행 */
		return nvmf_ctrlr_process_io_fused_cmd(req, bdev, desc, ch);
	} else if (spdk_unlikely(qpair->first_fused_req != NULL)) {
		/* [한국어] FUSE_FIRST가 저장된 상태에서 비-FUSE 명령이 도착 → 프로토콜 위반.
		 * FIRST 명령을 ABORTED_MISSING_FUSED로 강제 완료하고 현재 명령은 정상 처리를 계속 */
		struct spdk_nvme_cpl *fused_response = &qpair->first_fused_req->rsp->nvme_cpl;

		SPDK_ERRLOG("Second fused cmd expected - failing first one (cntlid:%u, qid:%u, opcode:0x%x)\n",
			    ctrlr->cntlid, qpair->qid,
			    req->qpair->first_fused_req->cmd->nvmf_cmd.opcode);

		/* abort qpair->first_fused_request and continue with new command */
		/* [한국어] 대기 중인 FUSE_FIRST 명령에 오류 응답을 채우고 강제 완료 처리 */
		fused_response->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED; /* [한국어] ABORTED_MISSING_FUSED: 짝 명령이 오지 않아 중단됨 */
		fused_response->status.sct = SPDK_NVME_SCT_GENERIC;
		_nvmf_request_complete(qpair->first_fused_req); /* [한국어] 오류 완료 처리: CQE 전송 + outstanding 큐에서 제거 */
		qpair->first_fused_req = NULL; /* [한국어] 저장된 FUSE_FIRST 포인터 초기화 */
	}

	if (ctrlr->subsys->passthrough) {
		/* [한국어] subsystem 전체 passthrough 모드: 모든 IO를 NVMe passthru로 드라이브에 직접 전달.
		 * ns->passthru_nsid를 실제 드라이브 NSID로 치환한 후 nvme_passthru_io 호출 */
		assert(ns->passthru_nsid > 0); /* [한국어] passthrough 모드에서는 반드시 passthru_nsid가 설정되어 있어야 함 */
		req->orig_nsid = req->cmd->nvme_cmd.nsid; /* [한국어] 원래 NSID 보존 (완료 시 복원을 위해) */
		req->cmd->nvme_cmd.nsid = ns->passthru_nsid; /* [한국어] SQE의 nsid를 드라이브 실제 NSID로 교체 */

		return nvmf_bdev_ctrlr_nvme_passthru_io(bdev, desc, ch, req); /* [한국어] NVMe Admin bypass: bdev_nvme 드라이버가 드라이브로 직접 전달 */
	}

	if (spdk_nvmf_request_using_zcopy(req)) {
		/* [한국어] Zero-copy 경로: transport가 bdev 버퍼를 직접 사용 가능한 경우
		 * INIT 단계에서 zcopy_start → bdev가 버퍼 제공 → EXECUTE 단계에서 transport가 DMA
		 * req->zcopy_phase는 이미 nvmf_ctrlr_process_io_cmd 진입 전에 INIT으로 설정됨 */
		assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_INIT); /* [한국어] zcopy 초기화 단계 확인 */
		return nvmf_bdev_ctrlr_zcopy_start(bdev, desc, ch, req); /* [한국어] bdev에게 zero-copy 버퍼 요청 → 콜백에서 transport에 버퍼 포인터 전달 */
	} else {
		switch (cmd->opc) {
		case SPDK_NVME_OPC_READ:
			/* [한국어] 표준 Read: bdev I/O 제출, 완료 콜백에서 데이터를 transport 버퍼로 복사 */
			return nvmf_bdev_ctrlr_read_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_WRITE:
			/* [한국어] 표준 Write: transport 버퍼의 데이터를 bdev에 기록 */
			return nvmf_bdev_ctrlr_write_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_FLUSH:
			/* [한국어] Flush: 드라이브의 휘발성 캐시를 비영구 미디어에 플러시 */
			return nvmf_bdev_ctrlr_flush_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_COMPARE:
			/* [한국어] Compare(opcode=05h): 드라이브 데이터와 호스트 버퍼 비교. ONCS.nvmcmps=1이어야 함 */
			if (spdk_unlikely(!ctrlr->cdata.oncs.nvmcmps)) {
				goto invalid_opcode; /* [한국어] Compare 미지원 컨트롤러 → INVALID_OPCODE */
			}
			return nvmf_bdev_ctrlr_compare_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_WRITE_ZEROES:
			/* [한국어] Write Zeroes(opcode=08h): 범위를 0으로 초기화. ONCS.nvmwzsv=1이어야 함 */
			if (spdk_unlikely(!ctrlr->cdata.oncs.nvmwzsv)) {
				goto invalid_opcode;
			}
			return nvmf_bdev_ctrlr_write_zeroes_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_DATASET_MANAGEMENT:
			/* [한국어] Dataset Management(opcode=09h): Deallocate(TRIM/UNMAP) 등. ONCS.nvmdsmsv=1이어야 함 */
			if (spdk_unlikely(!ctrlr->cdata.oncs.nvmdsmsv)) {
				goto invalid_opcode;
			}
			return nvmf_bdev_ctrlr_dsm_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_RESERVATION_REGISTER:
		case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		case SPDK_NVME_OPC_RESERVATION_RELEASE:
		case SPDK_NVME_OPC_RESERVATION_REPORT:
			/* [한국어] Reservation 명령(Register/Acquire/Release/Report): ONCS.reservs=1이어야 함.
			 * 처리는 subsystem->thread로 보내 registrant 목록에 안전하게 접근 (cross-thread) */
			if (spdk_unlikely(!ctrlr->cdata.oncs.reservs)) {
				goto invalid_opcode; /* [한국어] Reservation 미지원 → INVALID_OPCODE */
			}
			spdk_thread_send_msg(ctrlr->subsys->thread, nvmf_ns_reservation_request, req); /* [한국어] subsystem->thread로 전달하여 registrant 상태를 안전하게 수정 */
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS; /* [한국어] 비동기: 완료 시 _nvmf_request_complete 호출됨 */
		case SPDK_NVME_OPC_COPY:
			/* [한국어] Copy(opcode=19h, NVMe 2.0): Simple Copy. ONCS.nvmcpys=1이어야 함 */
			if (spdk_unlikely(!ctrlr->cdata.oncs.nvmcpys)) {
				goto invalid_opcode;
			}
			return nvmf_bdev_ctrlr_copy_cmd(bdev, desc, ch, req);
		default:
			/* [한국어] 위 opcode에 해당하지 않는 명령: passthru 허용이면 드라이브로 직접 전달.
			 * disable_command_passthru=true이면 INVALID_OPCODE */
			if (spdk_unlikely(qpair->transport->opts.disable_command_passthru)) {
				goto invalid_opcode; /* [한국어] passthru 비활성화 설정 → 미지원 opcode 거절 */
			}
			if (ns->passthru_nsid) {
				/* [한국어] ns에 passthru_nsid가 설정된 경우 NSID를 드라이브 실제 NSID로 교체 */
				req->orig_nsid = req->cmd->nvme_cmd.nsid;
				req->cmd->nvme_cmd.nsid = ns->passthru_nsid;
			}
			return nvmf_bdev_ctrlr_nvme_passthru_io(bdev, desc, ch, req); /* [한국어] 알 수 없는 opcode를 bdev_nvme에 그대로 전달 (드라이브 vendor-specific 등) */
		}
	}
invalid_opcode:
	/* [한국어] 지원하지 않는 opcode 또는 지원 비트가 없는 optional 명령 → INVALID_OPCODE DNR=1 */
	SPDK_INFOLOG(nvmf, "Unsupported IO opcode 0x%x\n", cmd->opc);
	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_INVALID_OPCODE; /* [한국어] 호스트가 재시도하지 않도록 DNR=1 함께 반환 */
	response->status.dnr = 1; /* [한국어] Do Not Retry: 같은 opcode는 항상 실패하므로 재시도 금지 */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_qpair_request_cleanup - qpair DEACTIVATING 상태에서 outstanding 큐 비었을 때 콜백 호출
 *
 * @qpair: 정리할 qpair.
 *
 * _nvmf_request_complete와 spdk_nvmf_request_free에서 요청이 완료/해제된 후 호출.
 * qpair가 DEACTIVATING 상태이고 outstanding 큐가 비면 state_cb를 호출하여
 * transport disconnect 완료를 알린다. outstanding이 남아있으면 대기.
 *
 * 실행 컨텍스트: qpair->group->thread.
 *
 * 호출 체인:
 *   _nvmf_request_complete / spdk_nvmf_request_free → [이 함수] → qpair->state_cb
 */
static void
nvmf_qpair_request_cleanup(struct spdk_nvmf_qpair *qpair)
{
	if (spdk_unlikely(qpair->state == SPDK_NVMF_QPAIR_DEACTIVATING)) {
		/* [한국어] qpair가 비활성화 진행 중: outstanding 큐가 비면 disconnect 완료 통보 */
		assert(qpair->state_cb != NULL); /* [한국어] state_cb 반드시 등록되어 있어야 함 */

		if (TAILQ_EMPTY(&qpair->outstanding)) {
			/* [한국어] 모든 요청이 완료됨 → disconnect 완료 콜백 호출 */
			qpair->state_cb(qpair->state_cb_arg, 0);
		}
	}
}

/*
 * [한국어]
 * spdk_nvmf_request_free - 요청을 응답 없이 해제 (AER 용)
 *
 * @req: 해제할 요청.
 * @return: 0.
 *
 * outstanding 큐에서 요청을 제거하고 transport에게 자원을 돌려준다.
 * 응답을 전송하지 않으므로 AER 해제나 오류 상황에서 사용한다.
 * 자원 해제 후 nvmf_qpair_request_cleanup으로 DEACTIVATING 상태 확인.
 *
 * 실행 컨텍스트: qpair->group->thread.
 *
 * 호출 체인:
 *   nvmf_qpair_free_aer / zcopy abort 경로 → [이 함수] → nvmf_transport_req_free
 */
int
spdk_nvmf_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] 요청의 qpair */

	TAILQ_REMOVE(&qpair->outstanding, req, link); /* [한국어] outstanding 큐에서 제거 */
	nvmf_transport_req_free(req); /* [한국어] transport에 자원 반환 (응답 없이) */

	nvmf_qpair_request_cleanup(qpair); /* [한국어] DEACTIVATING 상태 체크 */

	return 0;
}

/*
 * [한국어]
 * _nvmf_request_complete - 모든 NVMe-oF 요청의 실제 완료 처리 (내부 공통 경로)
 *
 * @ctx: spdk_nvmf_request 포인터 (spdk_thread_exec_msg 콜백 시그니처).
 *
 * spdk_nvmf_request_complete 또는 직접 호출로 진입한다.
 * 반드시 qpair->group->thread에서 실행해야 한다.
 *
 * 처리 순서:
 *   1) CQE 공통 필드(sqid=0, p=0, cid) 설정.
 *   2) ctrlr 있으면: sgroup 찾기, IO 통계 증가, passthrough nsid 복원, CRD 설정.
 *   3) Fabric Connect이면: sgroup을 connect 명령에서 찾기.
 *   4) reservation_waiting 해제: ns_info->preempt_abort.io_waiting 감소.
 *   5) zcopy 단계 처리: outstanding 큐 제거 조건 결정.
 *   6) nvmf_transport_req_complete: transport에 CQE 전송 위임.
 *   7) sgroup 카운터 감소 (mgmt_io_outstanding 또는 ns_info.io_outstanding).
 *   8) subsystem PAUSING → PAUSED 전환 조건 확인.
 *   9) nvmf_qpair_request_cleanup: DEACTIVATING qpair 체크.
 *
 * 실행 컨텍스트: qpair->group->thread.
 *
 * 호출 체인:
 *   spdk_nvmf_request_complete (exec_msg)
 *   또는 직접 호출 (_nvmf_request_complete(req)) → [이 함수]
 *                                                 → nvmf_transport_req_complete
 *                                                 → nvmf_qpair_request_cleanup
 */
static void
_nvmf_request_complete(void *ctx)
{
	struct spdk_nvmf_request *req = ctx; /* [한국어] 완료할 요청 */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl; /* [한국어] CQE 응답 */
	struct spdk_nvmf_qpair *qpair; /* [한국어] 요청의 qpair */
	struct spdk_nvmf_subsystem_poll_group *sgroup = NULL; /* [한국어] subsystem poll group */
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info; /* [한국어] NS별 통계 정보 */
	bool is_aer = false; /* [한국어] AER 요청 여부 */
	uint32_t nsid; /* [한국어] 요청의 NSID */
	bool paused; /* [한국어] subsystem이 PAUSED 상태로 전환 가능한지 */
	uint8_t opcode; /* [한국어] 요청의 opcode */

	rsp->sqid = 0; /* [한국어] SQID: NVMe-oF에서는 항상 0 */
	rsp->status.p = 0; /* [한국어] Phase Tag: 0으로 초기화 (transport가 반전) */
	rsp->cid = req->cmd->nvme_cmd.cid; /* [한국어] CID: 요청 명령의 CID를 CQE에 설정 */
	opcode = req->cmd->nvmf_cmd.opcode; /* [한국어] opcode 저장 (Fabric vs NVMe 구분용) */
	qpair = req->qpair; /* [한국어] 요청의 qpair */

	/* request should not be on a ns reservations list */
	assert(req->reservation_queued == false); /* [한국어] 완료 시점에 reservation 큐에 있으면 안 됨 */

	if (spdk_likely(qpair->ctrlr)) {
		/* [한국어] 컨트롤러 있음: subsystem poll group 찾기 */
		sgroup = &qpair->group->sgroups[qpair->ctrlr->subsys->id];
		assert(sgroup != NULL);
		if (spdk_likely(qpair->qid != 0)) {
			/* [한국어] IO qpair(qid>0): 완료된 IO 통계 증가 */
			qpair->group->stat.completed_nvme_io++;
		} else if (req->cmd->nvme_cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			/* [한국어] admin qpair의 AER 명령: 특수 처리 필요 */
			is_aer = true;
		}

		/* If we changed nvme_cmd.nsid to match the passthrough nsid, we need to
		 * restore it here for accounting purposes.
		 */
		if (qpair->ctrlr->subsys->passthrough && req->orig_nsid) {
			/* [한국어] passthrough NSID로 치환된 경우 원래 NSID 복원 (카운터 정확성) */
			req->cmd->nvme_cmd.nsid = req->orig_nsid;
		}

		/*
		 * Set the crd value.
		 * If the the IO has any error, and dnr (DoNotRetry) is not 1,
		 * and ACRE is enabled, we will set the crd to 1 to select the first CRDT.
		 */
		if (spdk_unlikely(spdk_nvme_cpl_is_error(rsp) &&
				  rsp->status.dnr == 0 &&
				  qpair->ctrlr->acre_enabled)) {
			/* [한국어] 오류 있고 DNR=0이고 ACRE 활성화됨: CRD=1로 첫 번째 재시도 지연 사용 */
			rsp->status.crd = 1;
		}
	} else if (spdk_unlikely(nvmf_request_is_fabric_connect(req))) {
		/* [한국어] ctrlr 없는 Fabric Connect 명령: connect 명령에서 sgroup 찾기 */
		sgroup = nvmf_subsystem_pg_from_connect_cmd(req);
	}

	nsid = req->cmd->nvme_cmd.nsid; /* [한국어] 요청의 NSID (복원 후) */

	/* Check if this IO is being waited on by a reservation commnd */
	if (spdk_unlikely(req->reservation_waiting)) {
		/* [한국어] reservation Preempt-and-Abort 대기 중인 요청 완료: io_waiting 카운터 감소 */
		if (sgroup && (nsid - 1 < sgroup->num_ns)) {
			ns_info = &sgroup->ns_info[nsid - 1]; /* [한국어] 해당 NS의 poll group 정보 */
			if (ns_info->preempt_abort.io_waiting > 0) {
				ns_info->preempt_abort.io_waiting--; /* [한국어] 대기 중인 IO 수 감소 */
			} else {
				SPDK_ERRLOG(
					"Request on reservation IO waiting but pg ns_info (%p) is not waiting\n",
					ns_info);
			}
		} else if (!sgroup) {
			SPDK_ERRLOG(
				"Request on reservation IO waiting but qpair (%p) detached from controller\n",
				qpair);
		} else {
			SPDK_ERRLOG("Request on reservation IO waiting but invalid nsid: %u\n", nsid);
		}
	}

	if (SPDK_DEBUGLOG_FLAG_ENABLED("nvmf")) {
		/* [한국어] 디버그 모드: CQE 내용 출력 */
		spdk_nvme_print_completion(qpair->qid, rsp);
	}

	/* [한국어] zcopy 단계에 따라 outstanding 큐 제거 여부 결정 */
	switch (req->zcopy_phase) {
	case NVMF_ZCOPY_PHASE_NONE:
		/* [한국어] 일반 요청: outstanding 큐에서 즉시 제거 */
		TAILQ_REMOVE(&qpair->outstanding, req, link);
		break;
	case NVMF_ZCOPY_PHASE_INIT:
		if (spdk_unlikely(spdk_nvme_cpl_is_error(rsp))) {
			/* [한국어] zcopy 초기화 실패: INIT_FAILED 단계로 전환 + outstanding 제거 */
			req->zcopy_phase = NVMF_ZCOPY_PHASE_INIT_FAILED;
			TAILQ_REMOVE(&qpair->outstanding, req, link);
		} else {
			/* [한국어] zcopy 초기화 성공: EXECUTE 단계로 전환 (outstanding 유지) */
			req->zcopy_phase = NVMF_ZCOPY_PHASE_EXECUTE;
		}
		break;
	case NVMF_ZCOPY_PHASE_EXECUTE:
		/* [한국어] zcopy 실행 중: outstanding 유지 (zcopy_end 대기) */
		break;
	case NVMF_ZCOPY_PHASE_END_PENDING:
		/* [한국어] zcopy 버퍼 반환 완료: outstanding 제거 + COMPLETE 단계로 전환 */
		TAILQ_REMOVE(&qpair->outstanding, req, link);
		req->zcopy_phase = NVMF_ZCOPY_PHASE_COMPLETE;
		break;
	default:
		SPDK_ERRLOG("Invalid ZCOPY phase %u\n", req->zcopy_phase);
		break;
	}

	nvmf_transport_req_complete(req); /* [한국어] transport에 CQE 전송 위임 */

	/* AER cmd is an exception */
	if (spdk_likely(sgroup && !is_aer)) {
		/* [한국어] sgroup이 있고 AER이 아닌 경우에만 outstanding 카운터 감소 */
		if (spdk_unlikely(opcode == SPDK_NVME_OPC_FABRIC ||
				  nvmf_qpair_is_admin_queue(qpair))) {
			/* [한국어] Fabric 명령 또는 Admin 명령: mgmt_io_outstanding 감소 */
			assert(sgroup->mgmt_io_outstanding > 0);
			sgroup->mgmt_io_outstanding--;
		} else {
			/* [한국어] IO 명령: zcopy가 완전히 끝났을 때만 ns_info.io_outstanding 감소 */
			if (req->zcopy_phase == NVMF_ZCOPY_PHASE_NONE ||
			    req->zcopy_phase == NVMF_ZCOPY_PHASE_COMPLETE ||
			    req->zcopy_phase == NVMF_ZCOPY_PHASE_INIT_FAILED) {
				/* End of request */

				/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
				if (spdk_likely(nsid - 1 < sgroup->num_ns)) {
					/* [한국어] NSID(1-based)를 0-based로 변환하여 ns_info 인덱스 계산 */
					assert(sgroup->ns_info[nsid - 1].io_outstanding != 0);
					sgroup->ns_info[nsid - 1].io_outstanding--; /* [한국어] NS별 IO outstanding 감소 */
				}
			}
		}

		if (spdk_unlikely(sgroup->state == SPDK_NVMF_SUBSYSTEM_PAUSING &&
				  sgroup->mgmt_io_outstanding == 0)) {
			/* [한국어] subsystem이 PAUSING 중이고 mgmt IO가 없음: PAUSED 전환 조건 확인 */
			paused = true;
			for (nsid = 0; nsid < sgroup->num_ns; nsid++) {
				ns_info = &sgroup->ns_info[nsid]; /* [한국어] 각 NS 정보 */

				if (ns_info->state == SPDK_NVMF_SUBSYSTEM_PAUSING &&
				    ns_info->io_outstanding > 0) {
					/* [한국어] 아직 PAUSING 중인 NS에 IO가 남아있음 → 아직 PAUSED 불가 */
					paused = false;
					break;
				}
			}

			if (paused) {
				/* [한국어] 모든 NS의 IO가 완료됨 → subsystem을 PAUSED 상태로 전환 */
				sgroup->state = SPDK_NVMF_SUBSYSTEM_PAUSED;
				sgroup->cb_fn(sgroup->cb_arg, 0); /* [한국어] PAUSED 완료 콜백 호출 */
				sgroup->cb_fn = NULL; /* [한국어] 콜백 클리어 */
				sgroup->cb_arg = NULL;
			}
		}

	}

	nvmf_qpair_request_cleanup(qpair); /* [한국어] DEACTIVATING qpair 체크 */
}

/*
 * [한국어]
 * spdk_nvmf_request_complete - 모든 NVMe-oF 요청의 공식 완료 진입점 (공개 API)
 *
 * @req: 완료할 요청.
 * @return: 항상 0.
 *
 * 호출자가 어느 스레드든 안전하게 호출할 수 있도록 spdk_thread_exec_msg를 사용해
 * qpair->group->thread에서 _nvmf_request_complete를 실행한다 (현재 스레드가 같으면
 * 즉시, 아니면 메시지 큐잉). 이로써 transport/bdev 모듈 어디서든 동일한 코드로
 * 응답을 보낼 수 있다.
 */
int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;

	spdk_thread_exec_msg(qpair->group->thread, _nvmf_request_complete, req); /* [한국어] thread-safe 위임 */

	return 0;
}

/*
 * [한국어]
 * nvmf_check_subsystem_active - subsystem/NS 활성 상태 확인 및 비활성 시 큐잉
 *
 * @req: 검사할 요청.
 * @return: true이면 처리 가능(활성), false이면 큐잉되거나 즉시 오류 응답됨.
 *
 * 모든 NVMe 명령을 실제 처리하기 전에 subsystem/NS 상태를 확인한다:
 *  - Fabric/Admin 명령: subsystem이 ACTIVE가 아니면 sgroup->queued에 저장.
 *  - IO 명령: nsid 범위 검증 → ns_info->channel NULL 체크(NS 초기화 중) → NS 상태 확인.
 *  - 활성이면 io_outstanding 또는 mgmt_io_outstanding를 증가시킨다.
 *
 * 실행 컨텍스트: qpair->group->thread.
 *
 * 호출 체인:
 *   spdk_nvmf_request_exec → 본 함수 → (비활성: TAILQ_INSERT, _nvmf_request_complete)
 */
static bool
nvmf_check_subsystem_active(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] 요청의 qpair */
	struct spdk_nvmf_subsystem_poll_group *sgroup = NULL; /* [한국어] 이 qpair의 subsystem poll group */
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info; /* [한국어] NS별 상태 정보 */
	uint32_t nsid; /* [한국어] IO 명령의 NSID */

	if (spdk_likely(qpair->ctrlr)) {
		/* [한국어] 이미 ctrlr가 연결된 qpair: ctrlr->subsys->id로 sgroup 조회 */
		sgroup = &qpair->group->sgroups[qpair->ctrlr->subsys->id];
		assert(sgroup != NULL);
	} else if (spdk_unlikely(nvmf_request_is_fabric_connect(req))) {
		/* [한국어] Connect 명령: ctrlr 미연결 상태에서 subnqn으로 sgroup 조회 */
		sgroup = nvmf_subsystem_pg_from_connect_cmd(req);
	}

	/* Check if the subsystem is paused (if there is a subsystem) */
	if (spdk_unlikely(sgroup == NULL)) {
		/* [한국어] sgroup 없음 (subnqn 불일치 등): 처리 계속 (이후에서 오류 처리) */
		return true;
	}

	if (spdk_unlikely(req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC ||
			  nvmf_qpair_is_admin_queue(qpair))) {
		/* [한국어] Fabric 캡슐 또는 Admin 명령: subsystem 단위로 상태 확인 */
		if (sgroup->state != SPDK_NVMF_SUBSYSTEM_ACTIVE) {
			/* The subsystem is not currently active. Queue this request. */
			/* [한국어] PAUSING/PAUSED 상태: 재개 시까지 sgroup->queued에 보관 */
			TAILQ_INSERT_TAIL(&sgroup->queued, req, link);
			return false;
		}
		sgroup->mgmt_io_outstanding++; /* [한국어] 완료 시 감소; 일시 정지 판단에 사용 */
	} else {
		/* [한국어] IO 명령 경로: NS 단위로 세밀한 상태 확인 */
		nsid = req->cmd->nvme_cmd.nsid; /* [한국어] IO SQE의 NSID (1-based) */

		/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
		/* [한국어] nsid-1 언더플로우로 nsid=0도 범위 초과 처리 */
		if (spdk_unlikely(nsid - 1 >= sgroup->num_ns)) {
			/* [한국어] NSID가 subsystem의 NS 수 이상 → INVALID_NAMESPACE (DNR=1) */
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
			req->rsp->nvme_cpl.status.dnr = 1; /* [한국어] Do Not Retry: 재시도 불필요 */
			TAILQ_INSERT_TAIL(&qpair->outstanding, req, link); /* [한국어] outstanding에 등록 후 즉시 완료 */
			_nvmf_request_complete(req);
			return false;
		}

		ns_info = &sgroup->ns_info[nsid - 1]; /* [한국어] NS별 bdev channel 및 상태 정보 */
		if (spdk_unlikely(ns_info->channel == NULL)) {
			/* This can can happen if host sends I/O to a namespace that is
			 * in the process of being added, but before the full addition
			 * process is complete.  Report invalid namespace in that case.
			 */
			/* [한국어] NS가 추가 중이나 아직 bdev channel이 설정 안 된 과도기 상태 */
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
			req->rsp->nvme_cpl.status.dnr = 1;
			TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);
			ns_info->io_outstanding++; /* [한국어] io_outstanding 증가 (완료 시 감소로 밸런스 유지) */
			_nvmf_request_complete(req);
			return false;
		}

		if (spdk_unlikely(ns_info->state != SPDK_NVMF_SUBSYSTEM_ACTIVE)) {
			/* The namespace is not currently active. Queue this request. */
			/* [한국어] NS가 PAUSING 상태이면 재개 시까지 큐잉 */
			TAILQ_INSERT_TAIL(&sgroup->queued, req, link);
			return false;
		}

		ns_info->io_outstanding++; /* [한국어] NS별 IO 카운터 증가 (완료 시 감소) */
	}

	return true; /* [한국어] 처리 가능 상태 */
}

/*
 * [한국어]
 * nvmf_check_qpair_active - qpair 상태 확인 및 비활성 시 오류 응답
 *
 * @req: 검사할 요청.
 * @return: true이면 처리 가능, false이면 오류 응답 후 return.
 *
 * qpair 상태에 따라 명령 허용 여부를 결정한다:
 *  - ENABLED: 모든 명령 허용.
 *  - CONNECTING: Fabric Connect(fctype=CONNECT)만 허용.
 *  - AUTHENTICATING: Auth Send/Recv Fabric 명령만 허용 (AUTH_REQUIRED 오류).
 *  - 그 외 상태(DEACTIVATING 등): COMMAND_SEQUENCE_ERROR.
 *
 * 실행 컨텍스트: qpair->group->thread.
 *
 * 호출 체인:
 *   spdk_nvmf_request_exec → 본 함수 → (비활성: TAILQ_INSERT, _nvmf_request_complete)
 */
static bool
nvmf_check_qpair_active(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] 요청의 qpair */
	int sc, sct; /* [한국어] 오류 응답 상태 코드 */

	if (spdk_likely(qpair->state == SPDK_NVMF_QPAIR_ENABLED)) {
		/* [한국어] 가장 일반적인 경로: ENABLED → 즉시 true */
		return true;
	}

	sct = SPDK_NVME_SCT_GENERIC; /* [한국어] 기본 오류 타입 */
	sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR; /* [한국어] 기본 오류 코드 */

	switch (qpair->state) {
	case SPDK_NVMF_QPAIR_CONNECTING:
		/* [한국어] 아직 Connect 명령 수신 전: Fabric Connect(opcode=FABRIC, fctype=CONNECT)만 허용 */
		if (req->cmd->nvmf_cmd.opcode != SPDK_NVME_OPC_FABRIC) {
			SPDK_ERRLOG("Received command 0x%x on qid %u before CONNECT\n",
				    req->cmd->nvmf_cmd.opcode, qpair->qid);
			break;
		}
		if (req->cmd->nvmf_cmd.fctype != SPDK_NVMF_FABRIC_COMMAND_CONNECT) {
			SPDK_ERRLOG("Received fctype 0x%x on qid %u before CONNECT\n",
				    req->cmd->nvmf_cmd.fctype, qpair->qid);
			break;
		}
		return true; /* [한국어] Connect 명령: 허용 */
	case SPDK_NVMF_QPAIR_AUTHENTICATING:
		/* [한국어] DH-HMAC-CHAP 인증 중: Auth Send/Recv만 허용 */
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC; /* [한국어] AUTH_REQUIRED는 Command Specific 오류 */
		sc = SPDK_NVMF_FABRIC_SC_AUTH_REQUIRED; /* [한국어] 인증 필요 오류 코드 */
		if (req->cmd->nvmf_cmd.opcode != SPDK_NVME_OPC_FABRIC) {
			SPDK_ERRLOG("Received command 0x%x on qid %u before authentication\n",
				    req->cmd->nvmf_cmd.opcode, qpair->qid);
			break;
		}
		if (req->cmd->nvmf_cmd.fctype != SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND &&
		    req->cmd->nvmf_cmd.fctype != SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV) {
			SPDK_ERRLOG("Received fctype 0x%x on qid %u before authentication\n",
				    req->cmd->nvmf_cmd.fctype, qpair->qid);
			break;
		}
		return true; /* [한국어] Auth 명령: 허용 */
	default:
		/* [한국어] DEACTIVATING 등 기타 비활성 상태: COMMAND_SEQUENCE_ERROR */
		SPDK_ERRLOG("Received command 0x%x on qid %u in state %d\n",
			    req->cmd->nvmf_cmd.opcode, qpair->qid, qpair->state);
		break;
	}

	req->rsp->nvme_cpl.status.sct = sct; /* [한국어] 오류 타입 설정 */
	req->rsp->nvme_cpl.status.sc = sc; /* [한국어] 오류 코드 설정 */
	TAILQ_INSERT_TAIL(&qpair->outstanding, req, link); /* [한국어] outstanding에 등록 */
	_nvmf_request_complete(req); /* [한국어] 오류 응답 즉시 전송 */

	return false;
}

/*
 * [한국어]
 * spdk_nvmf_request_exec - transport에서 들어온 모든 NVMe 명령의 최상위 진입점
 *
 * @req: 디스패치할 요청.
 *
 * Transport(rdma/tcp/fc/vfio_user)는 wire에서 명령 캡슐을 수신해 spdk_nvmf_request로
 * 변환한 뒤 본 함수를 호출한다. 흐름:
 *  1) nvmf_check_subsystem_active: subsystem이 ACTIVE가 아니면 큐잉 후 반환.
 *  2) nvmf_check_qpair_active: qpair가 CONNECTING/AUTHENTICATING이면 Connect/Auth만 허용.
 *  3) outstanding 큐에 등록 (응답 시 빠짐).
 *  4) opcode가 FABRIC이면 fabrics 디스패처, admin queue면 admin 디스패처, IO queue면 IO 디스패처.
 *  5) 동기적으로 COMPLETE를 받으면 _nvmf_request_complete를 즉시 호출.
 *
 * 호출 컨텍스트: qpair->group->thread. transport 폴러가 들어오는 캡슐을 처리할 때 호출.
 */
void
spdk_nvmf_request_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] 이 명령이 도착한 qpair */
	enum spdk_nvmf_request_exec_status status; /* [한국어] 디스패처 반환 상태 (COMPLETE=즉시완료, ASYNCHRONOUS=비동기) */

	if (spdk_unlikely(!nvmf_check_subsystem_active(req))) {
		/* [한국어] subsystem이 ACTIVE 아님(PAUSING/PAUSED/INACTIVE): 명령을 sgroup->queued에 보류.
		 * subsystem 재개 시 자동 재처리됨. outstanding 큐에 넣기 전이므로 그냥 return */
		return;
	}
	if (spdk_unlikely(!nvmf_check_qpair_active(req))) {
		/* [한국어] qpair가 CONNECTING/AUTHENTICATING 상태에서 Connect/Auth 이외 명령 수신.
		 * 비정상 순서로 응답 후 return (오류 처리는 nvmf_check_qpair_active 내부에서 수행) */
		return;
	}

	if (SPDK_DEBUGLOG_FLAG_ENABLED("nvmf")) {
		/* [한국어] debug 빌드에서 명령 SQE 내용을 로그에 출력 (성능 영향 없이 조건부 활성화) */
		spdk_nvme_print_command(qpair->qid, &req->cmd->nvme_cmd);
	}

	/* Place the request on the outstanding list so we can keep track of it */
	/* [한국어] outstanding 큐에 등록: 완료 처리(_nvmf_request_complete)에서 제거됨.
	 * 이 큐는 DEACTIVATING 상태에서 모든 미완료 요청 추적에도 사용됨 */
	TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);

	if (spdk_unlikely(req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC)) {
		/* [한국어] NVMe opcode 7Fh(Fabrics): Connect/Property Get/Set/Auth Send/Recv.
		 * qpair->ctrlr가 NULL인 경우에만 Connect가 허용되고 나머지는 ctrlr이 있어야 함 */
		status = nvmf_ctrlr_process_fabrics_cmd(req);
	} else if (spdk_unlikely(nvmf_qpair_is_admin_queue(qpair))) {
		/* [한국어] Admin qpair(qid==0): Identify/Get Log Page/Set Features/AER/ABORT 등 Admin 명령 */
		status = nvmf_ctrlr_process_admin_cmd(req);
	} else {
		/* [한국어] IO qpair(qid>0): READ/WRITE/FLUSH/COMPARE/WRITE_ZEROES/DSM/COPY/Reservation 등 IO 명령 */
		status = nvmf_ctrlr_process_io_cmd(req);
	}

	if (status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		/* [한국어] 동기 완료: outstanding 큐에서 빼고 CQE 전송 (transport가 wire로 전달)
		 * ASYNCHRONOUS이면 bdev/subsystem 콜백에서 spdk_nvmf_request_complete를 나중에 호출 */
		_nvmf_request_complete(req);
	}
}

/*
 * [한국어]
 * nvmf_ctrlr_get_dif_ctx - 명령의 DIF(Data Integrity Field) 컨텍스트 조회
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: NVMe SQE.
 * @dif_ctx: DIF 컨텍스트를 저장할 구조체.
 * @return: true이면 DIF 컨텍스트 획득 성공, false이면 미지원/해당 없음.
 *
 * DIF(Data Integrity Field)는 NVMe PI(Protection Information)의 T10 DIF 포맷으로
 * 섹터 끝 8바이트에 CRC16, Reference Tag, Application Tag를 삽입/스트리핑한다.
 * READ/WRITE/COMPARE 명령에 대해서만 DIF 컨텍스트를 반환한다.
 * NSID로 NS를 찾은 뒤 bdev descriptor를 통해 DIF 파라미터를 획득한다.
 *
 * 실행 컨텍스트: qpair->group->thread.
 *
 * 호출 체인:
 *   spdk_nvmf_request_get_dif_ctx → 본 함수 → nvmf_bdev_ctrlr_get_dif_ctx
 */
static bool
nvmf_ctrlr_get_dif_ctx(struct spdk_nvmf_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd,
		       struct spdk_dif_ctx *dif_ctx)
{
	struct spdk_nvmf_ns *ns; /* [한국어] cmd->nsid의 네임스페이스 */
	struct spdk_bdev_desc *desc; /* [한국어] bdev descriptor (DIF 파라미터 조회용) */

	if (ctrlr == NULL || cmd == NULL) {
		/* [한국어] 포인터 NULL이면 DIF 미적용 */
		return false;
	}

	ns = nvmf_ctrlr_get_ns(ctrlr, cmd->nsid); /* [한국어] NSID → NS 룩업 */
	if (ns == NULL || ns->bdev == NULL) {
		/* [한국어] NS가 없거나 비활성이면 DIF 없음 */
		return false;
	}

	desc = ns->desc; /* [한국어] NS의 bdev descriptor */

	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_COMPARE:
		/* [한국어] DIF 삽입/스트리핑이 필요한 데이터 명령들 */
		return nvmf_bdev_ctrlr_get_dif_ctx(desc, cmd, dif_ctx); /* [한국어] bdev에서 DIF 파라미터 획득 */
	default:
		break; /* [한국어] 기타 명령은 DIF 미적용 */
	}

	return false;
}

/*
 * [한국어]
 * spdk_nvmf_request_get_dif_ctx - 요청의 DIF 컨텍스트 공개 API
 *
 * @req: 대상 요청.
 * @dif_ctx: DIF 컨텍스트를 저장할 구조체.
 * @return: true이면 DIF 컨텍스트 획득 성공, false이면 DIF 미적용.
 *
 * transport가 데이터 전송 전에 DIF 처리 여부를 확인하기 위해 호출하는 공개 API.
 * ctrlr->dif_insert_or_strip이 false이면 즉시 false 반환(fast path).
 * Fabric/Admin 명령에는 DIF 미적용.
 *
 * 실행 컨텍스트: qpair->group->thread.
 */
bool
spdk_nvmf_request_get_dif_ctx(struct spdk_nvmf_request *req, struct spdk_dif_ctx *dif_ctx)
{
	struct spdk_nvmf_qpair *qpair = req->qpair; /* [한국어] 요청의 qpair */
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr; /* [한국어] qpair의 컨트롤러 */

	if (spdk_likely(ctrlr == NULL || !ctrlr->dif_insert_or_strip)) {
		/* [한국어] 가장 일반적인 경로: DIF 미사용 (fast path, likely 분기) */
		return false;
	}

	if (spdk_unlikely(!spdk_nvmf_qpair_is_active(qpair))) {
		/* [한국어] qpair가 비활성이면 DIF 처리 불가 */
		return false;
	}

	if (spdk_unlikely(req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC)) {
		/* [한국어] Fabric 캡슐은 NVMe 데이터 없음 → DIF 미적용 */
		return false;
	}

	if (spdk_unlikely(nvmf_qpair_is_admin_queue(qpair))) {
		/* [한국어] Admin 명령은 DIF 미적용 */
		return false;
	}

	return nvmf_ctrlr_get_dif_ctx(ctrlr, &req->cmd->nvme_cmd, dif_ctx); /* [한국어] NS별 DIF 컨텍스트 조회 */
}

/*
 * [한국어]
 * spdk_nvmf_set_custom_admin_cmd_hdlr - 특정 Admin 명령 opcode에 사용자 정의 핸들러 등록
 *
 * @opc: 핸들러를 등록할 NVMe Admin 명령 opcode.
 * @hdlr: 등록할 사용자 정의 핸들러 함수 포인터.
 *
 * g_nvmf_custom_admin_cmd_hdlrs[] 테이블에 핸들러를 등록하여 특정 opcode를
 * SPDK 기본 처리 대신 커스텀 함수로 처리할 수 있게 한다.
 * 예를 들어 Identify 명령 응답을 passthrough + 보정하는 방식으로 사용된다.
 */
void
spdk_nvmf_set_custom_admin_cmd_hdlr(uint8_t opc, spdk_nvmf_custom_cmd_hdlr hdlr)
{
	g_nvmf_custom_admin_cmd_hdlrs[opc].hdlr = hdlr; /* [한국어] opcode 인덱스에 핸들러 등록 */
}

/*
 * [한국어]
 * nvmf_passthru_admin_cmd_for_bdev_nsid - Admin 명령을 bdev NVMe passthrough로 전달
 *
 * @req: Admin 명령 요청.
 * @bdev_nsid: bdev를 찾기 위한 NSID.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE/ASYNCHRONOUS.
 *
 * NSID로 bdev를 찾아 NVMe passthrough admin 명령을 그대로 전달한다.
 * ns->passthru_nsid가 설정된 경우 cmd->nsid를 실제 NVMe 장치의 NSID로 교체한다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_passthru_admin_cmd → 본 함수 → spdk_nvmf_bdev_ctrlr_nvme_passthru_admin
 */
static int
nvmf_passthru_admin_cmd_for_bdev_nsid(struct spdk_nvmf_request *req, uint32_t bdev_nsid)
{
	struct spdk_bdev *bdev; /* [한국어] bdev 포인터 */
	struct spdk_bdev_desc *desc; /* [한국어] bdev descriptor */
	struct spdk_io_channel *ch; /* [한국어] bdev I/O 채널 */
	struct spdk_nvmf_ns *ns; /* [한국어] bdev_nsid에 해당하는 NS */
	struct spdk_nvmf_ctrlr *ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvme_cpl *response = spdk_nvmf_request_get_response(req); /* [한국어] CQE 응답 */
	int rc; /* [한국어] bdev 조회 반환값 */

	rc = spdk_nvmf_request_get_bdev(bdev_nsid, req, &bdev, &desc, &ch); /* [한국어] nsid → bdev/desc/channel 조회 */
	if (rc) {
		/* [한국어] bdev 조회 실패 → INVALID_NAMESPACE 응답 */
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	ns = nvmf_ctrlr_get_ns(ctrlr, bdev_nsid); /* [한국어] bdev_nsid → NS 룩업 */

	if (ns->passthru_nsid) {
		/* [한국어] 실제 NVMe 장치의 NSID로 교체 (가상 NSID → 물리 NSID 변환) */
		req->cmd->nvme_cmd.nsid = ns->passthru_nsid;
	}

	return spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(bdev, desc, ch, req, NULL); /* [한국어] NVMe Admin passthrough 발동 */
}

/*
 * [한국어]
 * nvmf_passthru_admin_cmd - Admin 명령 passthrough 라우터
 *
 * @req: Admin 명령 요청.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE/ASYNCHRONOUS.
 *
 * g_nvmf_custom_admin_cmd_hdlrs[opc].nsid가 0이 아니면 그 NSID를 사용하고,
 * 그렇지 않으면 cmd->nsid를 그대로 사용한다. 그런 다음
 * nvmf_passthru_admin_cmd_for_bdev_nsid()에 위임한다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_process_admin_cmd → 본 함수 → nvmf_passthru_admin_cmd_for_bdev_nsid
 */
static int
nvmf_passthru_admin_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req); /* [한국어] NVMe SQE */
	uint32_t bdev_nsid; /* [한국어] bdev 조회에 사용할 NSID */

	if (g_nvmf_custom_admin_cmd_hdlrs[cmd->opc].nsid != 0) {
		/* [한국어] 커스텀 핸들러에 특정 NSID가 지정된 경우 그것을 사용 */
		bdev_nsid = g_nvmf_custom_admin_cmd_hdlrs[cmd->opc].nsid;
	} else {
		/* [한국어] 지정 NSID 없으면 cmd->nsid 사용 */
		bdev_nsid = cmd->nsid;
	}

	return nvmf_passthru_admin_cmd_for_bdev_nsid(req, bdev_nsid); /* [한국어] bdev passthrough 처리 */
}

/*
 * [한국어]
 * nvmf_passthru_admin_cmd_for_ctrlr - 컨트롤러의 첫 번째 NS를 통해 Admin passthrough 실행
 *
 * @req: Admin 명령 요청.
 * @ctrlr: passthrough 대상 컨트롤러.
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE/ASYNCHRONOUS.
 *
 * ctrlr->subsys의 첫 번째 NS를 찾아 그 NSID로 passthrough 요청을 전달한다.
 * NS가 없으면 INVALID_NAMESPACE_OR_FORMAT 오류를 반환한다.
 * Identify Controller 등 NSID가 없어도 전달이 필요한 경우에 사용된다.
 *
 * 실행 컨텍스트: ctrlr->thread.
 */
int
nvmf_passthru_admin_cmd_for_ctrlr(struct spdk_nvmf_request *req, struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvme_cpl *response = spdk_nvmf_request_get_response(req); /* [한국어] CQE 응답 */
	struct spdk_nvmf_ns *ns; /* [한국어] 첫 번째 NS */

	ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); /* [한국어] subsystem의 첫 번째 NS 조회 */
	if (ns == NULL) {
		/* Is there a better sc to use here? */
		/* [한국어] NS가 없으면 passthrough 불가 → INVALID_NAMESPACE 응답 */
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return nvmf_passthru_admin_cmd_for_bdev_nsid(req, ns->nsid); /* [한국어] 첫 번째 NS의 NSID로 passthrough */
}

/*
 * [한국어]
 * spdk_nvmf_set_passthru_admin_cmd - Admin opcode에 passthrough 핸들러 등록
 *
 * @opc: NVMe Admin 명령 opcode.
 * @forward_nsid: passthrough 시 사용할 NSID (0이면 cmd->nsid 사용).
 *
 * g_nvmf_custom_admin_cmd_hdlrs[opc]에 nvmf_passthru_admin_cmd를 핸들러로,
 * forward_nsid를 NSID 오버라이드 값으로 등록한다.
 * 예: Identify Controller(opc=06h)를 NVMe 장치로 passthrough할 때 사용.
 */
void
spdk_nvmf_set_passthru_admin_cmd(uint8_t opc, uint32_t forward_nsid)
{
	g_nvmf_custom_admin_cmd_hdlrs[opc].hdlr = nvmf_passthru_admin_cmd; /* [한국어] passthrough 핸들러 등록 */
	g_nvmf_custom_admin_cmd_hdlrs[opc].nsid = forward_nsid; /* [한국어] NSID 오버라이드 값 등록 */
}

/*
 * [한국어]
 * spdk_nvmf_request_get_bdev - NSID로 bdev/desc/channel 조회
 *
 * @nsid: 조회할 Namespace ID (1-based).
 * @req: 요청 객체 (ctrlr, group 접근용).
 * @bdev: bdev 포인터를 저장할 출력 포인터.
 * @desc: bdev descriptor를 저장할 출력 포인터.
 * @ch: bdev I/O channel을 저장할 출력 포인터.
 * @return: 0(성공) 또는 -EINVAL (NS 없음/비활성).
 *
 * 여러 passthrough/abort 함수에서 NS의 bdev와 I/O 채널을 얻기 위해 공통적으로 사용.
 * poll group의 sgroup NS 정보에서 channel을 가져오므로 thread-safe하다.
 */
int
spdk_nvmf_request_get_bdev(uint32_t nsid, struct spdk_nvmf_request *req,
			   struct spdk_bdev **bdev, struct spdk_bdev_desc **desc, struct spdk_io_channel **ch)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr; /* [한국어] 현재 컨트롤러 */
	struct spdk_nvmf_ns *ns; /* [한국어] NSID에 해당하는 NS */
	struct spdk_nvmf_poll_group *group = req->qpair->group; /* [한국어] qpair의 poll group (channel 보유) */
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info; /* [한국어] NS별 bdev channel 정보 */

	/* [한국어] 출력 포인터 초기화 */
	*bdev = NULL;
	*desc = NULL;
	*ch = NULL;

	ns = nvmf_ctrlr_get_ns(ctrlr, nsid); /* [한국어] NSID → NS 룩업 */
	if (ns == NULL || ns->bdev == NULL) {
		/* [한국어] NS 없음 또는 비활성 */
		return -EINVAL;
	}

	assert(group != NULL && group->sgroups != NULL); /* [한국어] group은 항상 유효해야 함 */
	ns_info = &group->sgroups[ctrlr->subsys->id].ns_info[nsid - 1]; /* [한국어] nsid(1-based) → 0-based 인덱스 */
	*bdev = ns->bdev; /* [한국어] bdev 포인터 반환 */
	*desc = ns->desc; /* [한국어] bdev descriptor 반환 */
	*ch = ns_info->channel; /* [한국어] poll group 소유 bdev I/O channel 반환 */

	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_request_get_ctrlr - 요청의 컨트롤러 반환
 *
 * @req: 요청.
 * @return: req->qpair->ctrlr (NULL 가능).
 *
 * passthrough/abort 등 외부 모듈이 요청의 컨트롤러에 접근할 때 사용하는 공개 API.
 */
struct spdk_nvmf_ctrlr *spdk_nvmf_request_get_ctrlr(struct spdk_nvmf_request *req)
{
	return req->qpair->ctrlr; /* [한국어] qpair → ctrlr 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_request_get_cmd - 요청의 NVMe SQE 포인터 반환
 *
 * @req: 요청.
 * @return: &req->cmd->nvme_cmd.
 */
struct spdk_nvme_cmd *spdk_nvmf_request_get_cmd(struct spdk_nvmf_request *req)
{
	return &req->cmd->nvme_cmd; /* [한국어] 요청의 NVMe SQE 포인터 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_request_get_response - 요청의 NVMe CQE 포인터 반환
 *
 * @req: 요청.
 * @return: &req->rsp->nvme_cpl.
 */
struct spdk_nvme_cpl *spdk_nvmf_request_get_response(struct spdk_nvmf_request *req)
{
	return &req->rsp->nvme_cpl; /* [한국어] 요청의 NVMe CQE 응답 포인터 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_request_get_subsystem - 요청의 subsystem 반환
 *
 * @req: 요청.
 * @return: req->qpair->ctrlr->subsys.
 *
 * passthrough 등에서 subsystem에 접근할 때 사용하는 공개 API.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_request_get_subsystem(struct spdk_nvmf_request *req)
{
	return req->qpair->ctrlr->subsys; /* [한국어] ctrlr → subsys 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_request_copy_from_buf - 버퍼 내용을 요청의 iov에 복사
 *
 * @req: 요청 (iov, iovcnt 보유).
 * @buf: 복사할 원본 버퍼.
 * @buflen: 복사할 최대 바이트 수.
 * @return: 실제 복사된 바이트 수.
 *
 * 호스트에게 보낼 응답 데이터를 iov 배열에 복사하는 공통 헬퍼.
 */
size_t
spdk_nvmf_request_copy_from_buf(struct spdk_nvmf_request *req,
				void *buf, size_t buflen)
{
	struct spdk_iov_xfer ix; /* [한국어] iov 전송 컨텍스트 */

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt); /* [한국어] iov 배열 초기화 */
	return spdk_iov_xfer_from_buf(&ix, buf, buflen); /* [한국어] buf → iov 복사 */
}

/*
 * [한국어]
 * spdk_nvmf_request_copy_to_buf - 요청의 iov 내용을 버퍼로 복사
 *
 * @req: 요청 (iov, iovcnt 보유).
 * @buf: 복사할 대상 버퍼.
 * @buflen: 복사할 최대 바이트 수.
 * @return: 실제 복사된 바이트 수.
 *
 * 호스트가 보낸 데이터(in-capsule 또는 SGL로 전달된 데이터)를 로컬 버퍼로 복사하는 헬퍼.
 */
size_t
spdk_nvmf_request_copy_to_buf(struct spdk_nvmf_request *req,
			      void *buf, size_t buflen)
{
	struct spdk_iov_xfer ix; /* [한국어] iov 전송 컨텍스트 */

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt); /* [한국어] iov 배열 초기화 */
	return spdk_iov_xfer_to_buf(&ix, buf, buflen); /* [한국어] iov → buf 복사 */
}

/*
 * [한국어]
 * spdk_nvmf_ctrlr_get_subsystem - 컨트롤러의 subsystem 반환
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: ctrlr->subsys.
 *
 * 외부 모듈이 컨트롤러의 subsystem에 접근할 때 사용하는 공개 API.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_ctrlr_get_subsystem(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->subsys; /* [한국어] 컨트롤러의 subsystem 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_ctrlr_get_id - 컨트롤러 ID 반환
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: ctrlr->cntlid (Controller ID, 1-based 16비트).
 *
 * 외부 모듈이 컨트롤러 ID를 조회할 때 사용하는 공개 API.
 * cntlid는 subsystem 내에서 고유하며 host에게 Connect RSP로 알려진다.
 */
uint16_t
spdk_nvmf_ctrlr_get_id(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->cntlid; /* [한국어] 컨트롤러 ID (1~0xFFEF 범위, Dynamic 모드에서 target이 할당) */
}

/*
 * [한국어]
 * spdk_nvmf_request_get_req_to_abort - Abort 요청에서 Abort 대상 요청 반환
 *
 * @req: Abort 명령 요청.
 * @return: req->req_to_abort (Abort 대상 요청, NULL이면 아직 미탐색).
 *
 * Abort 명령 처리 시 CID로 찾은 대상 요청 포인터를 외부에 노출하는 공개 API.
 */
struct spdk_nvmf_request *spdk_nvmf_request_get_req_to_abort(struct spdk_nvmf_request *req)
{
	return req->req_to_abort; /* [한국어] Abort 대상 요청 포인터 반환 */
}
