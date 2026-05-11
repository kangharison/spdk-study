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
	int rc = 0;
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_qpair *qpair, *temp_qpair;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group;

	ctrlr = spdk_io_channel_iter_get_ctx(i);
	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, temp_qpair) {
		if (qpair->ctrlr == ctrlr && (include_admin || !nvmf_qpair_is_admin_queue(qpair))) {
			rc = spdk_nvmf_qpair_disconnect(qpair);
			if (rc) {
				if (rc == -EINPROGRESS) {
					rc = 0;
				} else {
					SPDK_ERRLOG("Qpair disconnect failed\n");
					return rc;
				}
			}
		}
	}

	return rc;
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
	uint64_t keep_alive_timeout_tick;
	uint64_t now = spdk_get_ticks();
	struct spdk_nvmf_ctrlr *ctrlr = ctx;

	if (ctrlr->in_destruct) {
		nvmf_ctrlr_stop_keep_alive_timer(ctrlr);
		return SPDK_POLLER_IDLE;
	}

	SPDK_DEBUGLOG(nvmf, "Polling ctrlr keep alive timeout\n");

	/* If the Keep alive feature is in use and the timer expires */
	keep_alive_timeout_tick = ctrlr->last_keep_alive_tick +
				  ctrlr->feat.keep_alive_timer.bits.kato * spdk_get_ticks_hz() / UINT64_C(1000);
	if (now > keep_alive_timeout_tick) {
		SPDK_NOTICELOG("Disconnecting host %s from subsystem %s due to keep alive timeout.\n",
			       ctrlr->hostnqn, ctrlr->subsys->subnqn);
		/* set the Controller Fatal Status bit to '1' */
		if (ctrlr->vcprop.csts.bits.cfs == 0) {
			nvmf_ctrlr_set_fatal_status(ctrlr);

			/*
			 * disconnect qpairs, terminate Transport connection
			 * destroy ctrlr, break the host to controller association
			 * disconnect qpairs with qpair->ctrlr == ctrlr
			 */
			spdk_for_each_channel(ctrlr->subsys->tgt,
					      nvmf_ctrlr_disconnect_qpairs_on_pg,
					      ctrlr,
					      nvmf_ctrlr_disconnect_qpairs_done);
			return SPDK_POLLER_BUSY;
		}
	}

	return SPDK_POLLER_IDLE;
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

static int
_retry_qid_check(void *ctx)
{
	struct spdk_nvmf_qpair *qpair = ctx;
	struct spdk_nvmf_request *req = qpair->connect_req;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;

	spdk_poller_unregister(&req->poller);
	SPDK_WARNLOG("Retrying adding qpair, qid:%d\n", qpair->qid);
	nvmf_ctrlr_add_qpair(qpair, ctrlr, req);
	return SPDK_POLLER_BUSY;
}

static void
_nvmf_ctrlr_add_admin_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;

	ctrlr->admin_qpair = qpair;
	ctrlr->association_timeout = qpair->transport->opts.association_timeout;
	nvmf_ctrlr_start_keep_alive_timer(ctrlr);
	nvmf_ctrlr_add_qpair(qpair, ctrlr, req);
}

static void
_nvmf_subsystem_add_ctrlr(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;

	if (nvmf_subsystem_add_ctrlr(ctrlr->subsys, ctrlr)) {
		SPDK_ERRLOG("Unable to add controller to subsystem\n");
		spdk_bit_array_free(&ctrlr->qpair_mask);
		free(ctrlr);
		qpair->ctrlr = NULL;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		spdk_nvmf_request_complete(req);
		return;
	}

	spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_add_admin_qpair, req);
}

static void
nvmf_ctrlr_cdata_init(struct spdk_nvmf_transport *transport, struct spdk_nvmf_subsystem *subsystem,
		      struct spdk_nvmf_ctrlr_data *cdata)
{
	cdata->aerl = SPDK_NVMF_MAX_ASYNC_EVENTS - 1;
	cdata->kas = transport->opts.kas;
	cdata->vid = SPDK_PCI_VID_INTEL;
	cdata->ssvid = SPDK_PCI_VID_INTEL;
	/* INTEL OUI */
	cdata->ieee[0] = 0xe4;
	cdata->ieee[1] = 0xd2;
	cdata->ieee[2] = 0x5c;

	/* When adding support for a new ONCS feature, ensure it follows the pattern below. */
	cdata->oncs.nvmcmps = transport->opts.oncs.nvmcmps;
	cdata->oncs.nvmdsmsv = transport->opts.oncs.nvmdsmsv;
	cdata->oncs.nvmwzsv = transport->opts.oncs.nvmwzsv;
	cdata->oncs.reservs = transport->opts.oncs.reservs;
	cdata->oncs.nvmcpys = transport->opts.oncs.nvmcpys;

	/* When adding support for a new FUSES feature, ensure it follows the pattern below. */
	cdata->fuses.fcws = transport->opts.fuses.fcws;

	cdata->sgls.supported = 1;
	cdata->sgls.keyed_sgl = 1;
	cdata->sgls.sgl_offset = 1;
	cdata->cntrltype = spdk_nvmf_subsystem_is_discovery(subsystem) ?
			   SPDK_NVME_CTRLR_DISCOVERY : SPDK_NVME_CTRLR_IO;
	cdata->nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	cdata->nvmf_specific.ioccsz += transport->opts.in_capsule_data_size / 16;
	cdata->nvmf_specific.iorcsz = sizeof(struct spdk_nvme_cpl) / 16;
	cdata->nvmf_specific.icdoff = 0; /* offset starts directly after SQE */
	cdata->nvmf_specific.ctrattr.ctrlr_model = SPDK_NVMF_CTRLR_MODEL_DYNAMIC;
	cdata->nvmf_specific.msdbd = 1;

	if (transport->ops->cdata_init) {
		transport->ops->cdata_init(transport, subsystem, cdata);
	}
}

static bool
nvmf_subsystem_has_zns_iocs(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_ns *ns;
	uint32_t i;

	for (i = 0; i < subsystem->max_nsid; i++) {
		ns = subsystem->ns[i];
		if (ns && ns->bdev && spdk_bdev_is_zoned(ns->bdev)) {
			return true;
		}
	}
	return false;
}

static void
nvmf_ctrlr_init_visible_ns(struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_ns *ns;

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		if (ns->always_visible || nvmf_ns_find_host(ns, ctrlr->hostnqn) != NULL) {
			nvmf_ctrlr_ns_set_visible(ctrlr, ns->nsid, true);
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
		if (connect_cmd->kato == 0) {
			ctrlr->feat.keep_alive_timer.bits.kato = 0;
		} else if (connect_cmd->kato <= transport->opts.min_kato) {
			ctrlr->feat.keep_alive_timer.bits.kato = transport->opts.min_kato;
		} else {
			ctrlr->feat.keep_alive_timer.bits.kato = spdk_round_up(connect_cmd->kato,
					ctrlr->cdata.kas * NVMF_KAS_TIME_UNIT_IN_MS);
		}
	}

	ctrlr->feat.async_event_configuration.bits.ns_attr_notice = 1;
	if (ctrlr->subsys->flags.ana_reporting) {
		ctrlr->feat.async_event_configuration.bits.ana_change_notice = 1;
	}
	ctrlr->feat.volatile_write_cache.bits.wce = 1;
	/* Coalescing Disable */
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
		if (ctrlr->feat.keep_alive_timer.bits.kato == 0) {
			ctrlr->feat.keep_alive_timer.bits.kato = NVMF_DISC_KATO_IN_MS;
			ctrlr->feat.async_event_configuration.bits.discovery_log_change_notice = 0;
		} else {
			ctrlr->feat.async_event_configuration.bits.discovery_log_change_notice = 1;
		}
	}

	/* Subtract 1 for admin queue, 1 for 0's based */
	ctrlr->feat.number_of_queues.bits.ncqr = transport->opts.max_qpairs_per_ctrlr - 1 -
			1;
	ctrlr->feat.number_of_queues.bits.nsqr = transport->opts.max_qpairs_per_ctrlr - 1 -
			1;

	spdk_uuid_copy(&ctrlr->hostid, (struct spdk_uuid *)connect_data->hostid);
	memcpy(ctrlr->hostnqn, connect_data->hostnqn, SPDK_NVMF_NQN_MAX_LEN);

	ctrlr->visible_ns = spdk_bit_array_create(subsystem->max_nsid);
	if (!ctrlr->visible_ns) {
		SPDK_ERRLOG("Failed to allocate visible namespace array\n");
		goto err_visible_ns;
	}
	nvmf_ctrlr_init_visible_ns(ctrlr);

	ctrlr->vcprop.cap.raw = 0;
	ctrlr->vcprop.cap.bits.cqr = 1; /* NVMe-oF specification required */
	ctrlr->vcprop.cap.bits.mqes = transport->opts.max_queue_depth -
				      1; /* max queue depth */
	ctrlr->vcprop.cap.bits.ams = 0; /* optional arb mechanisms */
	/* ready timeout - 500 msec units */
	ctrlr->vcprop.cap.bits.to = NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS / 500;
	ctrlr->vcprop.cap.bits.dstrd = 0; /* fixed to 0 for NVMe-oF */
	subsys_has_multi_iocs = nvmf_subsystem_has_zns_iocs(subsystem);
	if (subsys_has_multi_iocs) {
		ctrlr->vcprop.cap.bits.css =
			SPDK_NVME_CAP_CSS_IOCS; /* One or more I/O command sets supported */
	} else {
		ctrlr->vcprop.cap.bits.css = SPDK_NVME_CAP_CSS_NVM; /* NVM command set */
	}

	ctrlr->vcprop.cap.bits.mpsmin = 0; /* 2 ^ (12 + mpsmin) == 4k */
	ctrlr->vcprop.cap.bits.mpsmax = 0; /* 2 ^ (12 + mpsmax) == 4k */

	if (subsystem->nssr_enabled == 1) {
		ctrlr->vcprop.cap.bits.nssrs = 1;
	}

	/* NVMe 2.0 specification required */
	ctrlr->vcprop.cap.bits.crwms = 1;

	/* Version Supported: 2.0 */
	ctrlr->vcprop.vs.bits.mjr = 2;
	ctrlr->vcprop.vs.bits.mnr = 0;
	ctrlr->vcprop.vs.bits.ter = 0;

	ctrlr->vcprop.cc.raw = 0;
	ctrlr->vcprop.cc.bits.en = 0; /* Init controller disabled */
	if (subsys_has_multi_iocs) {
		ctrlr->vcprop.cc.bits.css =
			SPDK_NVME_CC_CSS_IOCS; /* All supported I/O Command Sets */
	}

	ctrlr->vcprop.csts.raw = 0;
	ctrlr->vcprop.csts.bits.rdy = 0; /* Init controller as not ready */

	/* set controller ready with media timeout to CAP.TO value */
	ctrlr->vcprop.crto.bits.crwmt = ctrlr->vcprop.cap.bits.to;

	SPDK_DEBUGLOG(nvmf, "cap 0x%" PRIx64 "\n", ctrlr->vcprop.cap.raw);
	SPDK_DEBUGLOG(nvmf, "vs 0x%x\n", ctrlr->vcprop.vs.raw);
	SPDK_DEBUGLOG(nvmf, "cc 0x%x\n", ctrlr->vcprop.cc.raw);
	SPDK_DEBUGLOG(nvmf, "csts 0x%x\n", ctrlr->vcprop.csts.raw);

	ctrlr->dif_insert_or_strip = transport->opts.dif_insert_or_strip;

	if (ctrlr->subsys->subtype == SPDK_NVMF_SUBTYPE_NVME) {
		if (spdk_nvmf_qpair_get_listen_trid(req->qpair, &listen_trid) != 0) {
			SPDK_ERRLOG("Could not get listener transport ID\n");
			goto err_listener;
		}

		ctrlr->listener = nvmf_subsystem_find_listener(ctrlr->subsys, &listen_trid);
		if (!ctrlr->listener) {
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

static void
nvmf_ctrlr_add_io_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvmf_qpair *admin_qpair = ctrlr->admin_qpair;
	struct spdk_nvmf_poll_group *admin_qpair_group = NULL;
	enum spdk_nvmf_qpair_state admin_qpair_state = SPDK_NVMF_QPAIR_UNINITIALIZED;
	bool admin_qpair_active = false;

	SPDK_DTRACE_PROBE4_TICKS(nvmf_ctrlr_add_io_qpair, ctrlr, req->qpair, req->qpair->qid,
				 spdk_thread_get_id(ctrlr->thread));

	/* For error case, the value should be NULL. So set it to NULL at first. */
	qpair->ctrlr = NULL;

	/* Make sure the controller is not being destroyed. */
	if (ctrlr->in_destruct) {
		SPDK_ERRLOG("Got I/O connect while ctrlr was being destroyed.\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	if (spdk_nvmf_subsystem_is_discovery(ctrlr->subsys)) {
		SPDK_ERRLOG("I/O connect not allowed on discovery controller\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	if (!ctrlr->vcprop.cc.bits.en) {
		SPDK_ERRLOG("Got I/O connect before ctrlr was enabled\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	if (1u << ctrlr->vcprop.cc.bits.iosqes != sizeof(struct spdk_nvme_cmd)) {
		SPDK_ERRLOG("Got I/O connect with invalid IOSQES %u\n",
			    ctrlr->vcprop.cc.bits.iosqes);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	if (1u << ctrlr->vcprop.cc.bits.iocqes != sizeof(struct spdk_nvme_cpl)) {
		SPDK_ERRLOG("Got I/O connect with invalid IOCQES %u\n",
			    ctrlr->vcprop.cc.bits.iocqes);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	/* There is a chance that admin qpair was destroyed. This is an issue that was observed only with ESX initiators */
	if (admin_qpair) {
		admin_qpair_active = spdk_nvmf_qpair_is_active(admin_qpair);
		admin_qpair_group = admin_qpair->group;
		admin_qpair_state = admin_qpair->state;
	}

	if (!admin_qpair_active || admin_qpair_group == NULL) {
		/* There is a chance that admin qpair was destroyed or is being destroyed at this moment due to e.g.
		 * expired keep alive timer. Part of the qpair destruction process is change of qpair's
		 * state to DEACTIVATING and removing it from poll group */
		SPDK_ERRLOG("Inactive admin qpair (state %d, group %p)\n", admin_qpair_state, admin_qpair_group);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	/* check if we would exceed ctrlr connection limit */
	if (qpair->qid >= spdk_bit_array_capacity(ctrlr->qpair_mask)) {
		SPDK_ERRLOG("Requested QID %u but Max QID is %u\n",
			    qpair->qid, spdk_bit_array_capacity(ctrlr->qpair_mask) - 1);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto end;
	}

	nvmf_ctrlr_add_qpair(qpair, ctrlr, req);
	return;
end:
	spdk_nvmf_request_complete(req);
}

static void
_nvmf_ctrlr_add_io_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_fabric_connect_data *data;
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_qpair *admin_qpair;
	struct spdk_nvmf_tgt *tgt = qpair->transport->tgt;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvme_transport_id listen_trid = {};
	const struct spdk_nvmf_subsystem_listener *listener;
	struct spdk_nvmf_poll_group *admin_qpair_group = NULL;
	enum spdk_nvmf_qpair_state admin_qpair_state = SPDK_NVMF_QPAIR_UNINITIALIZED;
	bool admin_qpair_active = false;

	assert(req->iovcnt == 1);

	data = req->iov[0].iov_base;

	SPDK_DEBUGLOG(nvmf, "Connect I/O Queue for controller id 0x%x\n", data->cntlid);

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, data->subnqn);
	/* We already checked this in _nvmf_ctrlr_connect */
	assert(subsystem != NULL);

	ctrlr = nvmf_subsystem_get_ctrlr(subsystem, data->cntlid);
	if (ctrlr == NULL) {
		SPDK_ERRLOG("Unknown controller ID 0x%x\n", data->cntlid);
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, cntlid);
		spdk_nvmf_request_complete(req);
		return;
	}

	/* fail before passing a message to the controller thread. */
	if (ctrlr->in_destruct) {
		SPDK_ERRLOG("Got I/O connect while ctrlr was being destroyed.\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		spdk_nvmf_request_complete(req);
		return;
	}

	/* If ANA reporting is enabled, check if I/O connect is on the same listener. */
	if (subsystem->flags.ana_reporting) {
		if (spdk_nvmf_qpair_get_listen_trid(req->qpair, &listen_trid) != 0) {
			SPDK_ERRLOG("Could not get listener transport ID\n");
			SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
			spdk_nvmf_request_complete(req);
			return;
		}

		listener = nvmf_subsystem_find_listener(subsystem, &listen_trid);
		if (listener != ctrlr->listener) {
			SPDK_ERRLOG("I/O connect is on a listener different from admin connect\n");
			SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
			spdk_nvmf_request_complete(req);
			return;
		}
	}

	admin_qpair = ctrlr->admin_qpair;

	/* There is a chance that admin qpair was destroyed. This is an issue that was observed only with ESX initiators */
	if (admin_qpair) {
		admin_qpair_active = spdk_nvmf_qpair_is_active(admin_qpair);
		admin_qpair_group = admin_qpair->group;
		admin_qpair_state = admin_qpair->state;
	}

	if (!admin_qpair_active || admin_qpair_group == NULL) {
		/* There is a chance that admin qpair was destroyed or is being destroyed at this moment due to e.g.
		 * expired keep alive timer. Part of the qpair destruction process is change of qpair's
		 * state to DEACTIVATING and removing it from poll group */
		SPDK_ERRLOG("Inactive admin qpair (state %d, group %p)\n", admin_qpair_state, admin_qpair_group);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		spdk_nvmf_request_complete(req);
		return;
	}
	qpair->ctrlr = ctrlr;
	spdk_thread_send_msg(admin_qpair_group->thread, nvmf_ctrlr_add_io_qpair, req);
}

static bool
nvmf_qpair_access_allowed(struct spdk_nvmf_qpair *qpair, struct spdk_nvmf_subsystem *subsystem,
			  const char *hostnqn)
{
	struct spdk_nvme_transport_id listen_trid = {};

	if (!spdk_nvmf_subsystem_host_allowed(subsystem, hostnqn)) {
		SPDK_ERRLOG("Subsystem '%s' does not allow host '%s'\n", subsystem->subnqn, hostnqn);
		return false;
	}

	if (spdk_nvmf_qpair_get_listen_trid(qpair, &listen_trid)) {
		SPDK_ERRLOG("Subsystem '%s' is unable to enforce access control due to an internal error.\n",
			    subsystem->subnqn);
		return false;
	}

	if (!spdk_nvmf_subsystem_listener_allowed(subsystem, &listen_trid)) {
		SPDK_ERRLOG("Subsystem '%s' does not allow host '%s' to connect at this address.\n",
			    subsystem->subnqn, hostnqn);
		return false;
	}

	return true;
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
	struct spdk_nvmf_fabric_connect_data *data = req->iov[0].iov_base;
	struct spdk_nvmf_fabric_connect_cmd *cmd = &req->cmd->connect_cmd;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_transport *transport = qpair->transport;
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_subsystem *subsystem;

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

	subsystem = spdk_nvmf_tgt_find_subsystem(transport->tgt, data->subnqn);
	if (!subsystem) {
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, subnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->recfmt != 0) {
		SPDK_ERRLOG("Connect command unsupported RECFMT %u\n", cmd->recfmt);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/*
	 * SQSIZE is a 0-based value, so it must be at least 1 (minimum queue depth is 2) and
	 * strictly less than max_aq_depth (admin queues) or max_queue_depth (io queues).
	 */
	if (cmd->sqsize == 0) {
		SPDK_ERRLOG("Invalid SQSIZE = 0\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, sqsize);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->qid == 0) {
		if (cmd->sqsize >= transport->opts.max_aq_depth) {
			SPDK_ERRLOG("Invalid SQSIZE for admin queue %u (min 1, max %u)\n",
				    cmd->sqsize, transport->opts.max_aq_depth - 1);
			SPDK_NVMF_INVALID_CONNECT_CMD(rsp, sqsize);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else if (cmd->sqsize >= transport->opts.max_queue_depth) {
		SPDK_ERRLOG("Invalid SQSIZE %u (min 1, max %u)\n",
			    cmd->sqsize, transport->opts.max_queue_depth - 1);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, sqsize);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	qpair->sq_head_max = cmd->sqsize;
	qpair->qid = cmd->qid;
	qpair->connect_received = true;

	pthread_mutex_lock(&qpair->group->mutex);
	assert(qpair->group->current_unassociated_qpairs > 0);
	qpair->group->current_unassociated_qpairs--;
	pthread_mutex_unlock(&qpair->group->mutex);

	if (0 == qpair->qid) {
		qpair->group->stat.admin_qpairs++;
		qpair->group->stat.current_admin_qpairs++;
	} else {
		qpair->group->stat.io_qpairs++;
		qpair->group->stat.current_io_qpairs++;
	}

	if (cmd->qid == 0) {
		SPDK_DEBUGLOG(nvmf, "Connect Admin Queue for controller ID 0x%x\n", data->cntlid);

		if (spdk_nvme_trtype_is_fabrics(transport->ops->type) && data->cntlid != 0xFFFF) {
			/* This NVMf target only supports dynamic mode. */
			SPDK_ERRLOG("The NVMf target only supports dynamic mode (CNTLID = 0x%x).\n", data->cntlid);
			SPDK_NVMF_INVALID_CONNECT_DATA(rsp, cntlid);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		/* Establish a new ctrlr */
		ctrlr = nvmf_ctrlr_create(subsystem, req, cmd, data);
		if (!ctrlr) {
			SPDK_ERRLOG("nvmf_ctrlr_create() failed\n");
			rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else {
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
	} else {
		spdk_thread_send_msg(subsystem->thread, _nvmf_ctrlr_add_io_qpair, req);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	}
}

static struct spdk_nvmf_subsystem_poll_group *
nvmf_subsystem_pg_from_connect_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_data *data;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;

	assert(nvmf_request_is_fabric_connect(req));
	assert(req->qpair->ctrlr == NULL);
	assert(req->iovcnt == 1);

	data = req->iov[0].iov_base;
	tgt = req->qpair->transport->tgt;

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, data->subnqn);
	if (subsystem == NULL) {
		return NULL;
	}

	return &req->qpair->group->sgroups[subsystem->id];
}

SPDK_LOG_DEPRECATION_REGISTER(spdk_nvmf_ctrlr_connect, "", "v26.05",
			      SPDK_LOG_DEPRECATION_EVERY_24H);

int
spdk_nvmf_ctrlr_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	enum spdk_nvmf_request_exec_status status;

	SPDK_LOG_DEPRECATED(spdk_nvmf_ctrlr_connect);

	if (req->iovcnt > 1) {
		SPDK_ERRLOG("Connect command invalid iovcnt: %d\n", req->iovcnt);
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		goto out;
	}

	sgroup = nvmf_subsystem_pg_from_connect_cmd(req);
	if (!sgroup) {
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, subnqn);
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		goto out;
	}

	sgroup->mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);

	status = _nvmf_ctrlr_connect(req);

out:
	if (status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		_nvmf_request_complete(req);
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
	struct spdk_nvmf_fabric_connect_data *data = req->iov[0].iov_base;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_transport *transport = req->qpair->transport;
	struct spdk_nvmf_subsystem *subsystem;

	if (req->length < sizeof(struct spdk_nvmf_fabric_connect_data)) {
		SPDK_ERRLOG("Connect command data length 0x%x too small\n", req->length);
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->iovcnt > 1) {
		SPDK_ERRLOG("Connect command invalid iovcnt: %d\n", req->iovcnt);
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(transport->tgt, data->subnqn);
	if (!subsystem) {
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
		sgroup = nvmf_subsystem_pg_from_connect_cmd(req);
		assert(sgroup != NULL);
		sgroup->mgmt_io_outstanding--;
		TAILQ_REMOVE(&req->qpair->outstanding, req, link);
		TAILQ_INSERT_TAIL(&sgroup->queued, req, link);
		SPDK_DEBUGLOG(nvmf, "Subsystem '%s' is not ready for connect, retrying...\n", subsystem->subnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	}

	/* Ensure that hostnqn is null terminated */
	if (!memchr(data->hostnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1)) {
		SPDK_ERRLOG("Connect HOSTNQN is not null terminated\n");
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, hostnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (!nvmf_qpair_access_allowed(req->qpair, subsystem, data->hostnqn)) {
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_HOST;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return _nvmf_ctrlr_connect(req);
}

static int
nvmf_ctrlr_association_remove(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx;
	int rc;

	nvmf_ctrlr_stop_association_timer(ctrlr);

	if (ctrlr->in_destruct) {
		return SPDK_POLLER_IDLE;
	}
	SPDK_DEBUGLOG(nvmf, "Disconnecting host from subsystem %s due to association timeout.\n",
		      ctrlr->subsys->subnqn);

	if (ctrlr->admin_qpair) {
		rc = spdk_nvmf_qpair_disconnect(ctrlr->admin_qpair);
		if (rc < 0 && rc != -EINPROGRESS) {
			SPDK_ERRLOG("Fail to disconnect admin ctrlr qpair\n");
			assert(false);
		}
	}

	return SPDK_POLLER_BUSY;
}

static int
_nvmf_ctrlr_cc_reset_shn_done(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx;
	uint64_t now = spdk_get_ticks();
	uint32_t count;

	if (ctrlr->cc_timer) {
		spdk_poller_unregister(&ctrlr->cc_timer);
	}

	count = spdk_bit_array_count_set(ctrlr->qpair_mask);
	SPDK_DEBUGLOG(nvmf, "ctrlr %p active queue count %u\n", ctrlr, count);

	if (count > 1) {
		if (now < ctrlr->cc_timeout_tsc) {
			/* restart cc timer */
			ctrlr->cc_timer = SPDK_POLLER_REGISTER(_nvmf_ctrlr_cc_reset_shn_done, ctrlr, 100 * 1000);
			return SPDK_POLLER_IDLE;
		} else {
			/* controller fatal status */
			SPDK_WARNLOG("IO timeout, ctrlr %p is in fatal status\n", ctrlr);
			nvmf_ctrlr_set_fatal_status(ctrlr);
		}
	}

	spdk_poller_unregister(&ctrlr->cc_timeout_timer);

	if (ctrlr->disconnect_is_shn) {
		ctrlr->vcprop.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
		ctrlr->disconnect_is_shn = false;
	} else {
		/* Only a subset of the registers are cleared out on a reset */
		ctrlr->vcprop.cc.raw = 0;
		ctrlr->vcprop.csts.raw = 0;
	}

	if (ctrlr->executing_nssr) {
		ctrlr->executing_nssr = false;
		ctrlr->vcprop.csts.bits.nssro = 1;
	}

	/* After CC.EN transitions to 0 (due to shutdown or reset), the association
	 * between the host and controller shall be preserved for at least 2 minutes */
	if (ctrlr->association_timer) {
		SPDK_DEBUGLOG(nvmf, "Association timer already set\n");
		nvmf_ctrlr_stop_association_timer(ctrlr);
	}
	if (ctrlr->association_timeout) {
		ctrlr->association_timer = SPDK_POLLER_REGISTER(nvmf_ctrlr_association_remove, ctrlr,
					   ctrlr->association_timeout * 1000);
	}
	ctrlr->disconnect_in_progress = false;
	return SPDK_POLLER_BUSY;
}

static void
nvmf_ctrlr_cc_reset_shn_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_ctrlr *ctrlr = spdk_io_channel_iter_get_ctx(i);

	if (status < 0) {
		SPDK_ERRLOG("Fail to disconnect io ctrlr qpairs\n");
		assert(false);
	}

	_nvmf_ctrlr_cc_reset_shn_done((void *)ctrlr);
}

static void
nvmf_bdev_complete_reset(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	SPDK_NOTICELOG("Resetting bdev done with %s\n", success ? "success" : "failure");

	spdk_bdev_free_io(bdev_io);
}


static int
nvmf_ctrlr_cc_timeout(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx;
	struct spdk_nvmf_poll_group *group;
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;

	spdk_poller_unregister(&ctrlr->cc_timeout_timer);
	SPDK_DEBUGLOG(nvmf, "Ctrlr %p reset or shutdown timeout\n", ctrlr);

	if (!ctrlr->admin_qpair) {
		SPDK_NOTICELOG("Ctrlr %p admin qpair disconnected\n", ctrlr);
		return SPDK_POLLER_IDLE;
	}

	group = ctrlr->admin_qpair->group;
	assert(group != NULL && group->sgroups != NULL);

	for (ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(ctrlr->subsys, ns)) {
		if (ns->bdev == NULL) {
			continue;
		}
		ns_info = &group->sgroups[ctrlr->subsys->id].ns_info[ns->opts.nsid - 1];
		SPDK_NOTICELOG("Ctrlr %p resetting NSID %u\n", ctrlr, ns->opts.nsid);
		spdk_bdev_reset(ns->desc, ns_info->channel, nvmf_bdev_complete_reset, NULL);
	}

	return SPDK_POLLER_BUSY;
}

const struct spdk_nvmf_registers *
spdk_nvmf_ctrlr_get_regs(struct spdk_nvmf_ctrlr *ctrlr)
{
	return &ctrlr->vcprop;
}

void
nvmf_ctrlr_set_fatal_status(struct spdk_nvmf_ctrlr *ctrlr)
{
	ctrlr->vcprop.csts.bits.cfs = 1;
}

static uint64_t
nvmf_prop_get_cap(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.cap.raw;
}

static uint64_t
nvmf_prop_get_vs(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.vs.raw;
}

static uint64_t
nvmf_prop_get_cc(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.cc.raw;
}

static uint64_t
nvmf_prop_get_nssr(struct spdk_nvmf_ctrlr *ctrlr)
{
	return 0;
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
	union spdk_nvme_cc_register cc, diff;
	uint32_t cc_timeout_ms;

	cc.raw = value; /* [한국어] 호스트가 쓴 새 값 */

	SPDK_DEBUGLOG(nvmf, "cur CC: 0x%08x\n", ctrlr->vcprop.cc.raw);
	SPDK_DEBUGLOG(nvmf, "new CC: 0x%08x\n", cc.raw);

	/*
	 * Calculate which bits changed between the current and new CC.
	 * Mark each bit as 0 once it is handled to determine if any unhandled bits were changed.
	 */
	diff.raw = cc.raw ^ ctrlr->vcprop.cc.raw;

	if (diff.bits.en) {
		if (cc.bits.en) {
			SPDK_DEBUGLOG(nvmf, "Property Set CC Enable!\n");
			nvmf_ctrlr_stop_association_timer(ctrlr);

			ctrlr->vcprop.cc.bits.en = 1;
			ctrlr->vcprop.csts.bits.rdy = 1;
		} else {
			SPDK_DEBUGLOG(nvmf, "Property Set CC Disable!\n");
			if (ctrlr->disconnect_in_progress) {
				SPDK_DEBUGLOG(nvmf, "Disconnect in progress\n");
				return true;
			}

			ctrlr->cc_timeout_timer = SPDK_POLLER_REGISTER(nvmf_ctrlr_cc_timeout, ctrlr,
						  NVMF_CC_RESET_SHN_TIMEOUT_IN_MS * 1000);
			/* Make sure cc_timeout_ms is between cc_timeout_timer and Host reset/shutdown timeout */
			cc_timeout_ms = (NVMF_CC_RESET_SHN_TIMEOUT_IN_MS + NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS) / 2;
			ctrlr->cc_timeout_tsc = spdk_get_ticks() + cc_timeout_ms * spdk_get_ticks_hz() / (uint64_t)1000;

			ctrlr->vcprop.cc.bits.en = 0;
			ctrlr->disconnect_in_progress = true;
			ctrlr->disconnect_is_shn = false;
			spdk_for_each_channel(ctrlr->subsys->tgt,
					      nvmf_ctrlr_disconnect_io_qpairs_on_pg,
					      ctrlr,
					      nvmf_ctrlr_cc_reset_shn_done);
		}
		diff.bits.en = 0;
	}

	if (diff.bits.shn) {
		if (cc.bits.shn == SPDK_NVME_SHN_NORMAL ||
		    cc.bits.shn == SPDK_NVME_SHN_ABRUPT) {
			SPDK_DEBUGLOG(nvmf, "Property Set CC Shutdown %u%ub!\n",
				      cc.bits.shn >> 1, cc.bits.shn & 1);
			if (ctrlr->disconnect_in_progress) {
				SPDK_DEBUGLOG(nvmf, "Disconnect in progress\n");
				return true;
			}

			ctrlr->cc_timeout_timer = SPDK_POLLER_REGISTER(nvmf_ctrlr_cc_timeout, ctrlr,
						  NVMF_CC_RESET_SHN_TIMEOUT_IN_MS * 1000);
			/* Make sure cc_timeout_ms is between cc_timeout_timer and Host reset/shutdown timeout */
			cc_timeout_ms = (NVMF_CC_RESET_SHN_TIMEOUT_IN_MS + NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS) / 2;
			ctrlr->cc_timeout_tsc = spdk_get_ticks() + cc_timeout_ms * spdk_get_ticks_hz() / (uint64_t)1000;

			ctrlr->vcprop.cc.bits.shn = cc.bits.shn;
			ctrlr->disconnect_in_progress = true;
			ctrlr->disconnect_is_shn = true;
			spdk_for_each_channel(ctrlr->subsys->tgt,
					      nvmf_ctrlr_disconnect_io_qpairs_on_pg,
					      ctrlr,
					      nvmf_ctrlr_cc_reset_shn_done);

			/* From the time a shutdown is initiated the controller shall disable
			 * Keep Alive timer */
			nvmf_ctrlr_stop_keep_alive_timer(ctrlr);
		} else if (cc.bits.shn == 0) {
			ctrlr->vcprop.cc.bits.shn = 0;
		} else {
			SPDK_ERRLOG("Prop Set CC: Invalid SHN value %u%ub\n",
				    cc.bits.shn >> 1, cc.bits.shn & 1);
			return false;
		}
		diff.bits.shn = 0;
	}

	if (diff.bits.iosqes) {
		SPDK_DEBUGLOG(nvmf, "Prop Set IOSQES = %u (%u bytes)\n",
			      cc.bits.iosqes, 1u << cc.bits.iosqes);
		ctrlr->vcprop.cc.bits.iosqes = cc.bits.iosqes;
		diff.bits.iosqes = 0;
	}

	if (diff.bits.iocqes) {
		SPDK_DEBUGLOG(nvmf, "Prop Set IOCQES = %u (%u bytes)\n",
			      cc.bits.iocqes, 1u << cc.bits.iocqes);
		ctrlr->vcprop.cc.bits.iocqes = cc.bits.iocqes;
		diff.bits.iocqes = 0;
	}

	if (diff.bits.ams) {
		SPDK_ERRLOG("Arbitration Mechanism Selected (AMS) 0x%x not supported!\n", cc.bits.ams);
		return false;
	}

	if (diff.bits.mps) {
		SPDK_ERRLOG("Memory Page Size (MPS) %u KiB not supported!\n", (1 << (2 + cc.bits.mps)));
		return false;
	}

	if (diff.bits.css) {
		if (cc.bits.css > SPDK_NVME_CC_CSS_IOCS) {
			SPDK_ERRLOG("I/O Command Set Selected (CSS) 0x%x not supported!\n", cc.bits.css);
			return false;
		}
		diff.bits.css = 0;
	}

	if (diff.bits.crime) {
		SPDK_WARNLOG("Property Set for read only property CC.CRIME\n");
		diff.bits.crime = 0;
	}

	if (diff.raw != 0) {
		/* Print an error message, but don't fail the command in this case.
		 * If we did want to fail in this case, we'd need to ensure we acted
		 * on no other bits or the initiator gets confused. */
		SPDK_ERRLOG("Prop Set CC toggled reserved bits 0x%x!\n", diff.raw);
	}

	return true;
}

static bool
nvmf_prop_set_nssr(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	struct spdk_nvmf_poll_group *group;
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;
	struct spdk_nvmf_ctrlr *ctrlr_iter;
	union spdk_nvme_cc_register cc_new;
	int rc = 0;

	if (ctrlr->vcprop.cap.bits.nssrs != 1) {
		return false;
	}

	/* A write of the value 4E564D65h ("NVMe")
	 * to this field initiates an NVM Subsystem Reset.
	 * A write of any other value has no
	 * functional effect on the operation of the NVM subsystem. */
	if (value != SPDK_NVME_NSSR_VALUE) {
		return true;
	}

	group = ctrlr->admin_qpair->group;
	assert(group != NULL && group->sgroups != NULL);

	for (ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(ctrlr->subsys, ns)) {
		if (ns->bdev == NULL) {
			continue;
		}

		ns_info = &group->sgroups[ctrlr->subsys->id].ns_info[ns->opts.nsid - 1];

		SPDK_DEBUGLOG(nvmf, "Ctrlr %p setting NSSR to NSID %u\n", ctrlr, ns->opts.nsid);
		rc = spdk_bdev_nvme_nssr(ns->desc, ns_info->channel, nvmf_bdev_complete_reset, NULL);
		if (rc == -ENOTSUP) {
			SPDK_DEBUGLOG(nvmf, "Ctrlr %p NSSR not supported, performing reset\n", ctrlr);
			spdk_bdev_reset(ns->desc, ns_info->channel, nvmf_bdev_complete_reset, NULL);
		}
	}

	TAILQ_FOREACH(ctrlr_iter, &ctrlr->subsys->ctrlrs, link) {
		cc_new = ctrlr_iter->vcprop.cc;
		cc_new.bits.en = 0;
		ctrlr_iter->executing_nssr = true;
		nvmf_prop_set_cc(ctrlr_iter, cc_new.raw);
	}

	return true;
}

static bool
nvmf_prop_set_csts(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	union spdk_nvme_csts_register csts;
	csts.raw = value;

	SPDK_DEBUGLOG(nvmf, "cur CSTS: 0x%08x\n", ctrlr->vcprop.csts.raw);
	SPDK_DEBUGLOG(nvmf, "new CSTS: 0x%08x\n", csts.raw);

	/* only NSSRO bit is RWC (Read/Write ‘1’ to clear),
	 * rest of CSTS is RO, so ignore it */
	if (csts.bits.nssro == 1) {
		ctrlr->vcprop.csts.bits.nssro = 0;
	}

	return true;
}

static uint64_t
nvmf_prop_get_csts(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.csts.raw;
}

static uint64_t
nvmf_prop_get_aqa(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.aqa.raw;
}

static bool
nvmf_prop_set_aqa(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	union spdk_nvme_aqa_register aqa;

	aqa.raw = value;

	/*
	 * We don't need to explicitly check for maximum size, as the fields are
	 * limited to 12 bits (4096).
	 */
	if (aqa.bits.asqs < SPDK_NVME_ADMIN_QUEUE_MIN_ENTRIES - 1 ||
	    aqa.bits.acqs < SPDK_NVME_ADMIN_QUEUE_MIN_ENTRIES - 1 ||
	    aqa.bits.reserved1 != 0 || aqa.bits.reserved2 != 0) {
		return false;
	}

	ctrlr->vcprop.aqa.raw = value;

	return true;
}

static uint64_t
nvmf_prop_get_asq(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.asq;
}

static bool
nvmf_prop_set_asq_lower(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	ctrlr->vcprop.asq = (ctrlr->vcprop.asq & (0xFFFFFFFFULL << 32ULL)) | value;

	return true;
}

static bool
nvmf_prop_set_asq_upper(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	ctrlr->vcprop.asq = (ctrlr->vcprop.asq & 0xFFFFFFFFULL) | ((uint64_t)value << 32ULL);

	return true;
}

static uint64_t
nvmf_prop_get_acq(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.acq;
}

static bool
nvmf_prop_set_acq_lower(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	ctrlr->vcprop.acq = (ctrlr->vcprop.acq & (0xFFFFFFFFULL << 32ULL)) | value;

	return true;
}

static bool
nvmf_prop_set_acq_upper(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	ctrlr->vcprop.acq = (ctrlr->vcprop.acq & 0xFFFFFFFFULL) | ((uint64_t)value << 32ULL);

	return true;
}

static uint64_t
nvmf_prop_get_crto(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.crto.raw;
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
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(nvmf_props); i++) {
		const struct nvmf_prop *prop = &nvmf_props[i];

		if ((ofst >= prop->ofst) && (ofst + size <= prop->ofst + prop->size)) {
			return prop;
		}
	}

	return NULL;
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
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_fabric_prop_get_cmd *cmd = &req->cmd->prop_get_cmd;
	struct spdk_nvmf_fabric_prop_get_rsp *response = &req->rsp->prop_get_rsp;
	const struct nvmf_prop *prop;
	uint8_t size;

	response->status.sc = 0;
	response->value.u64 = 0;

	SPDK_DEBUGLOG(nvmf, "size %d, offset 0x%x\n",
		      cmd->attrib.size, cmd->ofst);

	switch (cmd->attrib.size) {
	case SPDK_NVMF_PROP_SIZE_4:
		size = 4;
		break;
	case SPDK_NVMF_PROP_SIZE_8:
		size = 8;
		break;
	default:
		SPDK_DEBUGLOG(nvmf, "Invalid size value %d\n", cmd->attrib.size);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	prop = find_prop(cmd->ofst, size);
	if (prop == NULL || prop->get_cb == NULL) {
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	SPDK_DEBUGLOG(nvmf, "name: %s\n", prop->name);

	response->value.u64 = prop->get_cb(ctrlr);

	if (size != prop->size) {
		/* The size must be 4 and the prop->size is 8. Figure out which part of the property to read. */
		assert(size == 4);
		assert(prop->size == 8);

		if (cmd->ofst == prop->ofst) {
			/* Keep bottom 4 bytes only */
			response->value.u64 &= 0xFFFFFFFF;
		} else {
			/* Keep top 4 bytes only */
			response->value.u64 >>= 32;
		}
	}

	SPDK_DEBUGLOG(nvmf, "response value: 0x%" PRIx64 "\n", response->value.u64);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
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
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_fabric_prop_set_cmd *cmd = &req->cmd->prop_set_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	const struct nvmf_prop *prop;
	uint64_t value;
	uint8_t size;
	bool ret;

	SPDK_DEBUGLOG(nvmf, "size %d, offset 0x%x, value 0x%" PRIx64 "\n",
		      cmd->attrib.size, cmd->ofst, cmd->value.u64);

	switch (cmd->attrib.size) {
	case SPDK_NVMF_PROP_SIZE_4:
		size = 4;
		break;
	case SPDK_NVMF_PROP_SIZE_8:
		size = 8;
		break;
	default:
		SPDK_DEBUGLOG(nvmf, "Invalid size value %d\n", cmd->attrib.size);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	prop = find_prop(cmd->ofst, size);
	if (prop == NULL || prop->set_cb == NULL) {
		SPDK_INFOLOG(nvmf, "Invalid offset 0x%x\n", cmd->ofst);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	SPDK_DEBUGLOG(nvmf, "name: %s\n", prop->name);

	value = cmd->value.u64;

	if (prop->size == 4) {
		ret = prop->set_cb(ctrlr, (uint32_t)value);
	} else if (size != prop->size) {
		/* The size must be 4 and the prop->size is 8. Figure out which part of the property to write. */
		assert(size == 4);
		assert(prop->size == 8);

		if (cmd->ofst == prop->ofst) {
			ret = prop->set_cb(ctrlr, (uint32_t)value);
		} else {
			ret = prop->set_upper_cb(ctrlr, (uint32_t)value);
		}
	} else {
		ret = prop->set_cb(ctrlr, (uint32_t)value);
		if (ret) {
			ret = prop->set_upper_cb(ctrlr, (uint32_t)(value >> 32));
		}
	}

	if (!ret) {
		SPDK_ERRLOG("prop set_cb failed\n");
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_arbitration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_DEBUGLOG(nvmf, "Set Features - Arbitration (cdw11 = 0x%0x)\n", cmd->cdw11);

	ctrlr->feat.arbitration.raw = cmd->cdw11;
	ctrlr->feat.arbitration.bits.reserved = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_power_management(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(nvmf, "Set Features - Power Management (cdw11 = 0x%0x)\n", cmd->cdw11);

	/* Only PS = 0 is allowed, since we report NPSS = 0 */
	if (cmd->cdw11_bits.feat_power_management.bits.ps != 0) {
		SPDK_ERRLOG("Invalid power state %u\n", cmd->cdw11_bits.feat_power_management.bits.ps);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr->feat.power_management.raw = cmd->cdw11;
	ctrlr->feat.power_management.bits.reserved = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

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
		SPDK_ERRLOG("Invalid THSEL %u\n", opts->bits.thsel);
		return false;
	}

	return true;
}

static int
nvmf_ctrlr_set_features_temperature_threshold(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(nvmf, "Set Features - Temperature Threshold (cdw11 = 0x%0x)\n", cmd->cdw11);

	if (!temp_threshold_opts_valid(&cmd->cdw11_bits.feat_temp_threshold)) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* TODO: no sensors implemented - ignore new values */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_get_features_temperature_threshold(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(nvmf, "Get Features - Temperature Threshold (cdw11 = 0x%0x)\n", cmd->cdw11);

	if (!temp_threshold_opts_valid(&cmd->cdw11_bits.feat_temp_threshold)) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* TODO: no sensors implemented - return 0 for all thresholds */
	rsp->cdw0 = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_get_features_interrupt_vector_configuration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	union spdk_nvme_feat_interrupt_vector_configuration iv_conf = {};

	SPDK_DEBUGLOG(nvmf, "Get Features - Interrupt Vector Configuration (cdw11 = 0x%0x)\n", cmd->cdw11);

	iv_conf.bits.iv = cmd->cdw11_bits.feat_interrupt_vector_configuration.bits.iv;
	iv_conf.bits.cd = ctrlr->feat.interrupt_vector_configuration.bits.cd;
	rsp->cdw0 = iv_conf.raw;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_error_recovery(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(nvmf, "Set Features - Error Recovery (cdw11 = 0x%0x)\n", cmd->cdw11);

	if (cmd->cdw11_bits.feat_error_recovery.bits.dulbe) {
		/*
		 * Host is not allowed to set this bit, since we don't advertise it in
		 * Identify Namespace.
		 */
		SPDK_ERRLOG("Host set unsupported DULBE bit\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr->feat.error_recovery.raw = cmd->cdw11;
	ctrlr->feat.error_recovery.bits.reserved = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_volatile_write_cache(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_DEBUGLOG(nvmf, "Set Features - Volatile Write Cache (cdw11 = 0x%0x)\n", cmd->cdw11);

	ctrlr->feat.volatile_write_cache.raw = cmd->cdw11;
	ctrlr->feat.volatile_write_cache.bits.reserved = 0;

	SPDK_DEBUGLOG(nvmf, "Set Features - Volatile Write Cache %s\n",
		      ctrlr->feat.volatile_write_cache.bits.wce ? "Enabled" : "Disabled");
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_write_atomicity(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_DEBUGLOG(nvmf, "Set Features - Write Atomicity (cdw11 = 0x%0x)\n", cmd->cdw11);

	ctrlr->feat.write_atomicity.raw = cmd->cdw11;
	ctrlr->feat.write_atomicity.bits.reserved = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_host_identifier(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	SPDK_ERRLOG("Set Features - Host Identifier not allowed\n");
	response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_get_features_host_identifier(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_iov_xfer ix;

	SPDK_DEBUGLOG(nvmf, "Get Features - Host Identifier\n");

	if (!cmd->cdw11_bits.feat_host_identifier.bits.exhid) {
		/* NVMe over Fabrics requires EXHID=1 (128-bit/16-byte host ID) */
		SPDK_ERRLOG("Get Features - Host Identifier with EXHID=0 not allowed\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->iovcnt < 1 || req->length < sizeof(ctrlr->hostid)) {
		SPDK_ERRLOG("Invalid data buffer for Get Features - Host Identifier\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
	spdk_iov_xfer_from_buf(&ix, &ctrlr->hostid, sizeof(ctrlr->hostid));

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

static int
nvmf_ctrlr_get_features_host_behavior_support(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvme_host_behavior host_behavior = {};
	struct spdk_iov_xfer ix;

	SPDK_DEBUGLOG(nvmf, "Get Features - Host Behavior Support\n");

	if (req->iovcnt < 1 || req->length < sizeof(struct spdk_nvme_host_behavior)) {
		SPDK_ERRLOG("invalid data buffer for Host Behavior Support\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	host_behavior.acre = ctrlr->acre_enabled;
	host_behavior.lbafee = ctrlr->lbafee_enabled;

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
	spdk_iov_xfer_from_buf(&ix, &host_behavior, sizeof(host_behavior));

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_host_behavior_support(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvme_host_behavior *host_behavior;

	SPDK_DEBUGLOG(nvmf, "Set Features - Host Behavior Support\n");
	if (req->iovcnt != 1) {
		SPDK_ERRLOG("Host Behavior Support invalid iovcnt: %d\n", req->iovcnt);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	if (req->iov[0].iov_len != sizeof(struct spdk_nvme_host_behavior)) {
		SPDK_ERRLOG("Host Behavior Support invalid iov_len: %zd\n", req->iov[0].iov_len);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	host_behavior = (struct spdk_nvme_host_behavior *)req->iov[0].iov_base;
	if (host_behavior->acre == 0) {
		ctrlr->acre_enabled = false;
	} else if (host_behavior->acre == 1) {
		ctrlr->acre_enabled = true;
	} else {
		SPDK_ERRLOG("Host Behavior Support invalid acre: 0x%02x\n", host_behavior->acre);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	if (host_behavior->lbafee == 0) {
		ctrlr->lbafee_enabled = false;
	} else if (host_behavior->lbafee == 1) {
		ctrlr->lbafee_enabled = true;
	} else {
		SPDK_ERRLOG("Host Behavior Support invalid lbafee: 0x%02x\n", host_behavior->lbafee);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_keep_alive_timer(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_transport *transport = req->qpair->transport;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(nvmf, "Set Features - Keep Alive Timer (%u ms)\n", cmd->cdw11);

	/*
	 * if attempts to disable keep alive by setting kato to 0h
	 * a status value of keep alive invalid shall be returned
	 */
	if (cmd->cdw11_bits.feat_keep_alive_timer.bits.kato == 0) {
		rsp->status.sc = SPDK_NVME_SC_KEEP_ALIVE_INVALID;
	} else if (cmd->cdw11_bits.feat_keep_alive_timer.bits.kato <= transport->opts.min_kato) {
		ctrlr->feat.keep_alive_timer.bits.kato = transport->opts.min_kato;
	} else {
		/* round up to milliseconds */
		ctrlr->feat.keep_alive_timer.bits.kato = spdk_round_up(
					cmd->cdw11_bits.feat_keep_alive_timer.bits.kato,
					transport->opts.kas * NVMF_KAS_TIME_UNIT_IN_MS);
	}

	/*
	 * if change the keep alive timeout value successfully
	 * update the keep alive poller.
	 */
	if (cmd->cdw11_bits.feat_keep_alive_timer.bits.kato != 0) {
		if (ctrlr->keep_alive_poller != NULL) {
			spdk_poller_unregister(&ctrlr->keep_alive_poller);
		}
		ctrlr->keep_alive_poller = SPDK_POLLER_REGISTER(nvmf_ctrlr_keep_alive_poll, ctrlr,
					   ctrlr->feat.keep_alive_timer.bits.kato * 1000);
	}

	SPDK_DEBUGLOG(nvmf, "Set Features - Keep Alive Timer set to %u ms\n",
		      ctrlr->feat.keep_alive_timer.bits.kato);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_number_of_queues(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint32_t count;

	SPDK_DEBUGLOG(nvmf, "Set Features - Number of Queues, cdw11 0x%x\n",
		      req->cmd->nvme_cmd.cdw11);

	if (cmd->cdw11_bits.feat_num_of_queues.bits.ncqr == UINT16_MAX ||
	    cmd->cdw11_bits.feat_num_of_queues.bits.nsqr == UINT16_MAX) {
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	count = spdk_bit_array_count_set(ctrlr->qpair_mask);
	/* verify that the controller is ready to process commands */
	if (count > 1) {
		SPDK_DEBUGLOG(nvmf, "Queue pairs already active!\n");
		rsp->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
	} else {
		/*
		 * Ignore the value requested by the host -
		 * always return the pre-configured value based on max_qpairs_allowed.
		 */
		rsp->cdw0 = ctrlr->feat.number_of_queues.raw;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ctrlr) == 4936,
		   "Please check migration fields that need to be added or not");

static void
nvmf_ctrlr_migr_data_copy(struct spdk_nvmf_ctrlr_migr_data *data,
			  const struct spdk_nvmf_ctrlr_migr_data *data_src, size_t data_size)
{
	assert(data);
	assert(data_src);
	assert(data_size);

	memcpy(&data->regs, &data_src->regs, spdk_min(data->regs_size, data_src->regs_size));
	memcpy(&data->feat, &data_src->feat, spdk_min(data->feat_size, data_src->feat_size));

#define SET_FIELD(field) \
    if (offsetof(struct spdk_nvmf_ctrlr_migr_data, field) + sizeof(data->field) <= data_size) { \
        data->field = data_src->field; \
    } \

	SET_FIELD(cntlid);
	SET_FIELD(acre);
	SET_FIELD(num_aer_cids);
	SET_FIELD(num_async_events);
	SET_FIELD(notice_aen_mask);
#undef SET_FIELD

#define SET_ARRAY(arr) \
    if (offsetof(struct spdk_nvmf_ctrlr_migr_data, arr) + sizeof(data->arr) <= data_size) { \
        memcpy(&data->arr, &data_src->arr, sizeof(data->arr)); \
    } \

	SET_ARRAY(async_events);
	SET_ARRAY(aer_cids);
#undef SET_ARRAY
}

int
spdk_nvmf_ctrlr_save_migr_data(struct spdk_nvmf_ctrlr *ctrlr,
			       struct spdk_nvmf_ctrlr_migr_data *data)
{
	struct spdk_nvmf_async_event_completion *event, *event_tmp;
	uint32_t i;
	struct spdk_nvmf_ctrlr_migr_data data_local = {
		.data_size = offsetof(struct spdk_nvmf_ctrlr_migr_data, unused),
		.regs_size = sizeof(struct spdk_nvmf_registers),
		.feat_size = sizeof(struct spdk_nvmf_ctrlr_feat)
	};

	assert(data->data_size <= sizeof(data_local));
	assert(spdk_get_thread() == ctrlr->thread);

	memcpy(&data_local.regs, &ctrlr->vcprop, sizeof(struct spdk_nvmf_registers));
	memcpy(&data_local.feat, &ctrlr->feat, sizeof(struct spdk_nvmf_ctrlr_feat));

	data_local.cntlid = ctrlr->cntlid;
	data_local.acre = ctrlr->acre_enabled;
	data_local.num_aer_cids = ctrlr->nr_aer_reqs;

	STAILQ_FOREACH_SAFE(event, &ctrlr->async_events, link, event_tmp) {
		if (data_local.num_async_events + 1 > SPDK_NVMF_MIGR_MAX_PENDING_AERS) {
			SPDK_ERRLOG("ctrlr %p has too many pending AERs\n", ctrlr);
			break;
		}

		data_local.async_events[data_local.num_async_events++].raw = event->event.raw;
	}

	for (i = 0; i < ctrlr->nr_aer_reqs; i++) {
		struct spdk_nvmf_request *req = ctrlr->aer_req[i];
		data_local.aer_cids[i] = req->cmd->nvme_cmd.cid;
	}
	data_local.notice_aen_mask = ctrlr->notice_aen_mask;

	nvmf_ctrlr_migr_data_copy(data, &data_local, spdk_min(data->data_size, data_local.data_size));
	return 0;
}

int
spdk_nvmf_ctrlr_restore_migr_data(struct spdk_nvmf_ctrlr *ctrlr,
				  const struct spdk_nvmf_ctrlr_migr_data *data)
{
	uint32_t i;
	struct spdk_nvmf_ctrlr_migr_data data_local = {
		.data_size = offsetof(struct spdk_nvmf_ctrlr_migr_data, unused),
		.regs_size = sizeof(struct spdk_nvmf_registers),
		.feat_size = sizeof(struct spdk_nvmf_ctrlr_feat)
	};

	assert(data->data_size <= sizeof(data_local));
	assert(spdk_get_thread() == ctrlr->thread);

	/* local version of data should have defaults set before copy */
	nvmf_ctrlr_migr_data_copy(&data_local, data, spdk_min(data->data_size, data_local.data_size));
	memcpy(&ctrlr->vcprop, &data_local.regs, sizeof(struct spdk_nvmf_registers));
	memcpy(&ctrlr->feat, &data_local.feat, sizeof(struct spdk_nvmf_ctrlr_feat));

	ctrlr->cntlid = data_local.cntlid;
	ctrlr->acre_enabled = data_local.acre;

	for (i = 0; i < data_local.num_async_events; i++) {
		struct spdk_nvmf_async_event_completion *event;

		event = calloc(1, sizeof(*event));
		if (!event) {
			return -ENOMEM;
		}

		event->event.raw = data_local.async_events[i].raw;
		STAILQ_INSERT_TAIL(&ctrlr->async_events, event, link);
	}
	ctrlr->notice_aen_mask = data_local.notice_aen_mask;

	return 0;
}

static int
nvmf_ctrlr_set_features_async_event_configuration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_DEBUGLOG(nvmf, "Set Features - Async Event Configuration, cdw11 0x%08x\n",
		      cmd->cdw11);
	ctrlr->feat.async_event_configuration.raw = cmd->cdw11;
	ctrlr->feat.async_event_configuration.bits.reserved1 = 0;
	ctrlr->feat.async_event_configuration.bits.reserved2 = 0;
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

void
spdk_nvmf_get_cmds_and_effects_log_page(struct spdk_nvmf_ctrlr *ctrlr,
					struct spdk_nvme_cmds_and_effect_log_page *log_page)
{
	struct spdk_nvme_cmds_and_effect_entry *entry;

	*log_page = g_cmds_and_effect_log_page;
	if (!ctrlr->cdata.oncs.nvmwzsv || !nvmf_ctrlr_write_zeroes_supported(ctrlr)) {
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_WRITE_ZEROES];
		memset(entry, 0, sizeof(*entry));
	}
	if (!ctrlr->cdata.oncs.nvmdsmsv || !nvmf_ctrlr_dsm_supported(ctrlr)) {
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_DATASET_MANAGEMENT];
		memset(entry, 0, sizeof(*entry));
	}
	if (!ctrlr->cdata.oncs.nvmcmps) {
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_COMPARE];
		memset(entry, 0, sizeof(*entry));
	}
	if (!nvmf_subsystem_has_zns_iocs(ctrlr->subsys)) {
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_ZONE_MGMT_SEND];
		memset(entry, 0, sizeof(*entry));
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_ZONE_MGMT_RECV];
		memset(entry, 0, sizeof(*entry));
	}
	if (!nvmf_subsystem_zone_append_supported(ctrlr->subsys)) {
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_ZONE_APPEND];
		memset(entry, 0, sizeof(*entry));
	}
	if (!ctrlr->cdata.oncs.nvmcpys) {
		entry = &log_page->io_cmds_supported[SPDK_NVME_OPC_COPY];
		memset(entry, 0, sizeof(*entry));
	}
	if (!ctrlr->cdata.oncs.reservs) {
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

static int
nvmf_get_cmds_and_effects_log_page(struct spdk_nvmf_ctrlr *ctrlr, struct iovec *iovs, int iovcnt,
				   uint64_t offset, uint32_t length)
{
	uint32_t page_size = sizeof(struct spdk_nvme_cmds_and_effect_log_page);
	size_t copy_len = 0;
	struct spdk_iov_xfer ix;
	struct spdk_nvme_cmds_and_effect_log_page cmds_and_effects_log_page = {};

	spdk_nvmf_get_cmds_and_effects_log_page(ctrlr, &cmds_and_effects_log_page);

	spdk_iov_xfer_init(&ix, iovs, iovcnt);
	if (offset < page_size) {
		copy_len = spdk_min(page_size - offset, length);
		spdk_iov_xfer_from_buf(&ix, (char *)(&cmds_and_effects_log_page) + offset, copy_len);
	} else {
		SPDK_ERRLOG("Invalid Get log page cmds effects offset: (%" PRIu64 "), log page size (%" PRIu32")\n",
			    offset, page_size);
		return -EINVAL;
	}

	return 0;
}

static int
nvmf_get_reservation_notification_log_page(struct spdk_nvmf_ctrlr *ctrlr,
		struct iovec *iovs, int iovcnt, uint64_t offset, uint32_t length, uint32_t rae)
{
	uint32_t unit_log_len, avail_log_len, next_pos, copy_len;
	struct spdk_nvmf_reservation_log *log, *log_tmp;
	struct spdk_iov_xfer ix;

	spdk_iov_xfer_init(&ix, iovs, iovcnt);

	unit_log_len = sizeof(struct spdk_nvme_reservation_notification_log);
	/* No available log, return zeroed log pages */
	if (!ctrlr->num_avail_log_pages) {
		return 0;
	}

	avail_log_len = ctrlr->num_avail_log_pages * unit_log_len;
	if (offset >= avail_log_len) {
		SPDK_ERRLOG("Invalid Get log page reservation notification offset: (%" PRIu64"), log page size (%"
			    PRIu32")\n", offset, avail_log_len);
		return -EINVAL;
	}

	next_pos = 0;
	TAILQ_FOREACH_SAFE(log, &ctrlr->log_head, link, log_tmp) {
		TAILQ_REMOVE(&ctrlr->log_head, log, link);
		ctrlr->num_avail_log_pages--;

		next_pos += unit_log_len;
		if (next_pos > offset) {
			copy_len = spdk_min(next_pos - offset, length);
			spdk_iov_xfer_from_buf(&ix, &log->log, copy_len);
			length -= copy_len;
			offset += copy_len;
		}
		free(log);

		if (length == 0) {
			break;
		}
	}

	if (!rae) {
		nvmf_ctrlr_unmask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL_MASK_BIT);
	}
	return 0;
}

static bool
is_log_page_ctrlr_nvm_scope(uint8_t lid)
{
	switch (lid) {
	case SPDK_NVME_LOG_SUPPORTED_LOG_PAGES:
	case SPDK_NVME_LOG_ERROR:
	case SPDK_NVME_LOG_FIRMWARE_SLOT:
	case SPDK_NVME_LOG_CHANGED_NS_LIST:
	case SPDK_NVME_LOG_COMMAND_EFFECTS_LOG:
	case SPDK_NVME_LOG_DEVICE_SELF_TEST:
	case SPDK_NVME_LOG_TELEMETRY_HOST_INITIATED:
	case SPDK_NVME_LOG_TELEMETRY_CTRLR_INITIATED:
	case SPDK_NVME_LOG_ENDURANCE_GROUP_INFORMATION:
	case SPDK_NVME_LOG_PREDICATBLE_LATENCY:
	case SPDK_NVME_LOG_PREDICTABLE_LATENCY_EVENT:
	case SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS:
	case SPDK_NVME_LOG_PERSISTENT_EVENT_LOG:
	case SPDK_NVME_LOG_ENDURANCE_GROUP_EVENT:
		return true;
	default:
		return false;
	}
}

static const struct spdk_nvme_feature_ids_effects_log_page
	g_supported_feat_id_effects_log_pages_discovery = {
	.fis = {
		[SPDK_NVME_FEAT_KEEP_ALIVE_TIMER] =	     { .fsupp = 1, .cscpe = 1},
		[SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION] = { .fsupp = 1, .cscpe = 1},
	}
};

static const struct spdk_nvme_feature_ids_effects_log_page g_supported_feat_id_effects_log_pages = {
	.fis = {
		[SPDK_NVME_FEAT_KEEP_ALIVE_TIMER] =		  { .fsupp = 1, .cscpe = 1},
		[SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION] =	  { .fsupp = 1, .cscpe = 1},
		[SPDK_NVME_FEAT_ARBITRATION] =			  { .fsupp = 1, .cscpe = 1},
		[SPDK_NVME_FEAT_POWER_MANAGEMENT] =		  { .fsupp = 1, .cscpe = 1},
		[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD] =	  { .fsupp = 1, .cscpe = 1},
		[SPDK_NVME_FEAT_ERROR_RECOVERY] =		  { .fsupp = 1, .nscpe = 1},
		[SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE] =		  { .fsupp = 1, .cscpe = 1},
		[SPDK_NVME_FEAT_NUMBER_OF_QUEUES] =		  { .fsupp = 1, .cscpe = 1},
		[SPDK_NVME_FEAT_INTERRUPT_COALESCING] =		  { .fsupp = 1, .cscpe = 1},
		[SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION] = { .fsupp = 1, .cscpe = 1},
		[SPDK_NVME_FEAT_WRITE_ATOMICITY] =		  { .fsupp = 1, .cscpe = 1},
		[SPDK_NVME_FEAT_HOST_IDENTIFIER] =		  { .fsupp = 1, .cscpe = 1},
		[SPDK_NVME_FEAT_HOST_RESERVE_MASK] =		  { .fsupp = 1, .nscpe = 1},
		[SPDK_NVME_FEAT_HOST_RESERVE_PERSIST] =		  { .fsupp = 1, .nscpe = 1},
		[SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT] =	  { .fsupp = 1, .cscpe = 1},
	}
};

void
spdk_nvmf_get_feature_ids_effects_log_page(struct spdk_nvmf_ctrlr *ctrlr,
		struct spdk_nvme_feature_ids_effects_log_page *log_page)
{
	if (spdk_nvmf_subsystem_is_discovery(ctrlr->subsys)) {
		*log_page = g_supported_feat_id_effects_log_pages_discovery;
	} else {
		*log_page = g_supported_feat_id_effects_log_pages;
	}
}

static int
nvmf_get_feature_ids_effects_log_page(struct spdk_nvmf_ctrlr *ctrlr, struct iovec *iovs, int iovcnt,
				      uint64_t offset, uint32_t length)
{
	uint32_t page_size = sizeof(struct spdk_nvme_feature_ids_effects_log_page);
	size_t copy_len = 0;
	struct spdk_iov_xfer ix;
	struct spdk_nvme_feature_ids_effects_log_page supported_feature_ids_effects_log_page = {};

	spdk_nvmf_get_feature_ids_effects_log_page(ctrlr, &supported_feature_ids_effects_log_page);

	spdk_iov_xfer_init(&ix, iovs, iovcnt);
	if (offset < page_size) {
		copy_len = spdk_min(page_size - offset, length);
		spdk_iov_xfer_from_buf(&ix, (char *)(&supported_feature_ids_effects_log_page) + offset, copy_len);
	} else {
		SPDK_ERRLOG("Invalid Get feat id effects log page offset: (%" PRIu64 "),"
			    " log page size (%" PRIu32")\n", offset, page_size);
		return -EINVAL;
	}

	return 0;
}

static const struct spdk_nvme_supported_log_pages g_supported_log_pages_discover = {
	.lids = {
		[SPDK_NVME_LOG_SUPPORTED_LOG_PAGES] = { .lsupp = 1 },
		[SPDK_NVME_LOG_DISCOVERY] =	      { .lsupp = 1 },
		[SPDK_NVME_LOG_FEATURE_IDS_EFFECTS] = { .lsupp = 1 },
	}
};

static const struct spdk_nvme_supported_log_pages g_supported_log_pages = {
	.lids = {
		[SPDK_NVME_LOG_SUPPORTED_LOG_PAGES] =	      { .lsupp = 1 },
		[SPDK_NVME_LOG_ERROR] =			      { .lsupp = 1 },
		[SPDK_NVME_LOG_HEALTH_INFORMATION] =	      { .lsupp = 1 },
		[SPDK_NVME_LOG_FIRMWARE_SLOT] =		      { .lsupp = 1 },
		[SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS] = { .lsupp = 1 },
		[SPDK_NVME_LOG_COMMAND_EFFECTS_LOG] =	      { .lsupp = 1 },
		[SPDK_NVME_LOG_CHANGED_NS_LIST] =	      { .lsupp = 1 },
		[SPDK_NVME_LOG_RESERVATION_NOTIFICATION] =    { .lsupp = 1 },
		[SPDK_NVME_LOG_FEATURE_IDS_EFFECTS] =	      { .lsupp = 1 },
		[SPDK_NVME_LOG_NVME_MI_COMMANDS_EFFECTS] =    { .lsupp = 1 },
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
	case SPDK_NVME_LOG_SUPPORTED_LOG_PAGES:
		rc = nvmf_get_supported_log_pages(ctrlr, req->iov, req->iovcnt, offset, len);
		break;
	case SPDK_NVME_LOG_ERROR:
		nvmf_get_error_log_page(ctrlr, req->iov, req->iovcnt, offset, len, rae);
		break;
	case SPDK_NVME_LOG_HEALTH_INFORMATION:
		/* TODO: actually fill out log page data */
		break;
	case SPDK_NVME_LOG_FIRMWARE_SLOT:
		rc = nvmf_get_firmware_slot_log_page(req->iov, req->iovcnt, offset, len);
		break;
	case SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS:
		if (subsystem->flags.ana_reporting) {
			uint32_t rgo = cmd->cdw10_bits.get_log_page.lsp & 1;
			rc = nvmf_get_ana_log_page(ctrlr, req->iov, req->iovcnt, offset, len, rae, rgo);
			break;
		} else {
			SPDK_INFOLOG(nvmf, "Get Log Page for Asymmetric Namespace Access is not supported\n");
			goto invalid_field_log_page;
		}
	case SPDK_NVME_LOG_COMMAND_EFFECTS_LOG:
		rc = nvmf_get_cmds_and_effects_log_page(ctrlr, req->iov, req->iovcnt, offset, len);
		break;
	case SPDK_NVME_LOG_CHANGED_NS_LIST:
		rc = nvmf_get_changed_ns_list_log_page(ctrlr, req->iov, req->iovcnt, offset, len, rae);
		break;
	case SPDK_NVME_LOG_RESERVATION_NOTIFICATION:
		rc = nvmf_get_reservation_notification_log_page(ctrlr, req->iov, req->iovcnt, offset, len, rae);
		break;
	case SPDK_NVME_LOG_FEATURE_IDS_EFFECTS:
		rc = nvmf_get_feature_ids_effects_log_page(ctrlr, req->iov, req->iovcnt, offset, len);
		break;
	case SPDK_NVME_LOG_NVME_MI_COMMANDS_EFFECTS:
		/* To comply with id-ctrl LPA bit 5, don't fail this log but return zeroed buffer instead. */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	default:
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

static struct spdk_nvmf_ns *
_nvmf_ctrlr_get_ns_safe(struct spdk_nvmf_ctrlr *ctrlr,
			uint32_t nsid,
			struct spdk_nvme_cpl *rsp)
{
	struct spdk_nvmf_ns *ns;
	if (nsid == 0 || nsid > ctrlr->subsys->max_nsid) {
		SPDK_ERRLOG("Identify Namespace for invalid NSID %u\n", nsid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return NULL;
	}

	ns = nvmf_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL || ns->bdev == NULL) {
		/*
		 * Inactive namespaces should return a zero filled data structure.
		 * The data buffer is already zeroed by nvmf_ctrlr_process_admin_cmd(),
		 * so we can just return early here.
		 */
		SPDK_DEBUGLOG(nvmf, "Identify Namespace for inactive NSID %u\n", nsid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_SUCCESS;
		return NULL;
	}
	return ns;
}

static void
nvmf_ctrlr_identify_ns(struct spdk_nvmf_ctrlr *ctrlr,
		       struct spdk_nvme_cmd *cmd,
		       struct spdk_nvme_cpl *rsp,
		       struct spdk_nvme_ns_data *nsdata,
		       uint32_t nsid)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_ns *ns;
	enum spdk_nvme_ana_state ana_state;

	ns = _nvmf_ctrlr_get_ns_safe(ctrlr, nsid, rsp);
	if (ns == NULL) {
		return;
	}

	nvmf_bdev_ctrlr_identify_ns(ns, nsdata, ctrlr->dif_insert_or_strip,
				    ctrlr->admin_qpair->transport->opts.max_io_size);

	assert(ctrlr->admin_qpair);

	if (subsystem->flags.ana_reporting) {
		assert(ns->anagrpid - 1 < subsystem->max_nsid);
		nsdata->anagrpid = ns->anagrpid;

		ana_state = nvmf_ctrlr_get_ana_state(ctrlr, ns->anagrpid);
		if (ana_state == SPDK_NVME_ANA_INACCESSIBLE_STATE ||
		    ana_state == SPDK_NVME_ANA_PERSISTENT_LOSS_STATE) {
			nsdata->nuse = 0;
		}
	}
}

int
spdk_nvmf_ctrlr_identify_ns(struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvme_cmd *cmd,
			    struct spdk_nvme_cpl *rsp,
			    struct spdk_nvme_ns_data *nsdata)
{
	nvmf_ctrlr_identify_ns(ctrlr, cmd, rsp, nsdata, cmd->nsid);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static void
identify_ns_passthru_cb(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
	struct spdk_nvmf_ctrlr *ctrlr = spdk_nvmf_request_get_ctrlr(req);
	struct spdk_nvme_cpl *rsp = spdk_nvmf_request_get_response(req);
	struct spdk_nvme_ns_data nvmf_nsdata = {};
	struct spdk_nvme_ns_data nvme_nsdata = {};
	size_t datalen;

	/* This is the identify data from the NVMe drive */
	datalen = spdk_nvmf_request_copy_to_buf(req, &nvme_nsdata,
						sizeof(nvme_nsdata));
	nvmf_ctrlr_identify_ns(ctrlr, cmd, rsp, &nvmf_nsdata, req->orig_nsid);

	/* Update fabric's namespace according to SSD's namespace */
	if (nvme_nsdata.nsfeat.optperf) {
		nvmf_nsdata.nsfeat.optperf = nvme_nsdata.nsfeat.optperf;
		nvmf_nsdata.npwg = nvme_nsdata.npwg;
		nvmf_nsdata.npwa = nvme_nsdata.npwa;
		nvmf_nsdata.npdg = nvme_nsdata.npdg;
		nvmf_nsdata.npda = nvme_nsdata.npda;
		nvmf_nsdata.nows = nvme_nsdata.nows;
	}

	if (nvme_nsdata.nsfeat.ns_atomic_write_unit) {
		nvmf_nsdata.nsfeat.ns_atomic_write_unit = nvme_nsdata.nsfeat.ns_atomic_write_unit;
		nvmf_nsdata.nawun = nvme_nsdata.nawun;
		nvmf_nsdata.nawupf = nvme_nsdata.nawupf;
		nvmf_nsdata.nacwu = nvme_nsdata.nacwu;
	}

	nvmf_nsdata.nabsn = nvme_nsdata.nabsn;
	nvmf_nsdata.nabo = nvme_nsdata.nabo;
	nvmf_nsdata.nabspf = nvme_nsdata.nabspf;

	spdk_nvmf_request_copy_from_buf(req, &nvmf_nsdata, datalen);
}

int
spdk_nvmf_ctrlr_identify_ns_ext(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
	struct spdk_nvmf_ctrlr *ctrlr = spdk_nvmf_request_get_ctrlr(req);
	struct spdk_nvmf_ns *ns = nvmf_ctrlr_get_ns(ctrlr, cmd->nsid);
	struct spdk_nvme_cpl *rsp = spdk_nvmf_request_get_response(req);
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_nvme_ns_data nsdata = {};
	struct spdk_iov_xfer ix;
	int rc;

	nvmf_ctrlr_identify_ns(ctrlr, cmd, rsp, &nsdata, cmd->nsid);

	rc = spdk_nvmf_request_get_bdev(cmd->nsid, req, &bdev, &desc, &ch);
	if (rc) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_ADMIN)) {
		spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
		spdk_iov_xfer_from_buf(&ix, &nsdata, sizeof(nsdata));

		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	assert(ns->passthru_nsid != 0);
	req->orig_nsid = ns->nsid;
	cmd->nsid = ns->passthru_nsid;

	return spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(bdev, desc, ch, req, identify_ns_passthru_cb);
}

static void
nvmf_ctrlr_populate_oacs(struct spdk_nvmf_ctrlr *ctrlr,
			 struct spdk_nvme_ctrlr_data *cdata)
{
	cdata->oacs = ctrlr->cdata.oacs;

	cdata->oacs.vms =
		g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_VIRTUALIZATION_MANAGEMENT].hdlr != NULL;
	cdata->oacs.nsrs = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_NVME_MI_SEND].hdlr != NULL
			   && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_NVME_MI_RECEIVE].hdlr != NULL;
	cdata->oacs.dirs = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_DIRECTIVE_SEND].hdlr != NULL
			   && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_DIRECTIVE_RECEIVE].hdlr != NULL;
	cdata->oacs.dsts =
		g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_DEVICE_SELF_TEST].hdlr != NULL;
	cdata->oacs.nms = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_NS_MANAGEMENT].hdlr != NULL
			  && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_NS_ATTACHMENT].hdlr != NULL;
	cdata->oacs.fwds = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD].hdlr !=
			   NULL
			   && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_FIRMWARE_COMMIT].hdlr != NULL;
	cdata->oacs.fnvms =
		g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_FORMAT_NVM].hdlr != NULL;
	cdata->oacs.ssrs = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_SECURITY_SEND].hdlr != NULL
			   && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_SECURITY_RECEIVE].hdlr != NULL;
	cdata->oacs.glss = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_GET_LBA_STATUS].hdlr !=
			   NULL;
}

int
spdk_nvmf_ctrlr_identify_ctrlr(struct spdk_nvmf_ctrlr *ctrlr, struct spdk_nvme_ctrlr_data *cdata)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_transport *transport;

	/*
	 * Common fields for discovery and NVM subsystems
	 */
	assert(ctrlr->admin_qpair);
	transport = ctrlr->admin_qpair->transport;
	spdk_strcpy_pad(cdata->fr, FW_VERSION, sizeof(cdata->fr), ' ');
	assert((transport->opts.max_io_size % 4096) == 0);
	cdata->mdts = spdk_u32log2(transport->opts.max_io_size / 4096);
	cdata->cntlid = ctrlr->cntlid;
	cdata->ver = ctrlr->vcprop.vs;
	cdata->aerl = ctrlr->cdata.aerl;
	cdata->lpa.lpeds = 1;
	cdata->elpe = 127;
	cdata->maxcmd = transport->opts.max_queue_depth;
	cdata->sgls = ctrlr->cdata.sgls;
	cdata->fuses = ctrlr->cdata.fuses;
	cdata->acwu = 0; /* ACWU is 0-based. */
	cdata->wctemp = 0x0157; /* Recommended value from NVMe 1.3d spec. */
	cdata->cctemp = 0x0175;
	if (subsystem->flags.ana_reporting) {
		cdata->mnan = subsystem->max_nsid;
	}
	spdk_strcpy_pad(cdata->subnqn, subsystem->subnqn, sizeof(cdata->subnqn), '\0');

	SPDK_DEBUGLOG(nvmf, "ctrlr data: maxcmd 0x%x\n", cdata->maxcmd);
	SPDK_DEBUGLOG(nvmf, "sgls data: 0x%x\n", from_le32(&cdata->sgls));


	if (spdk_nvmf_subsystem_is_discovery(subsystem)) {
		/*
		 * NVM Discovery subsystem fields
		 */
		cdata->oaes.discovery_log_change_notices = 1;
		cdata->cntrltype = SPDK_NVME_CTRLR_DISCOVERY;
	} else {
		cdata->vid = ctrlr->cdata.vid;
		cdata->ssvid = ctrlr->cdata.ssvid;
		cdata->ieee[0] = ctrlr->cdata.ieee[0];
		cdata->ieee[1] = ctrlr->cdata.ieee[1];
		cdata->ieee[2] = ctrlr->cdata.ieee[2];

		/*
		 * NVM subsystem fields (reserved for discovery subsystems)
		 */
		spdk_strcpy_pad(cdata->mn, spdk_nvmf_subsystem_get_mn(subsystem), sizeof(cdata->mn), ' ');
		spdk_strcpy_pad(cdata->sn, spdk_nvmf_subsystem_get_sn(subsystem), sizeof(cdata->sn), ' ');
		cdata->kas = ctrlr->cdata.kas;

		cdata->rab = 6;
		cdata->cmic.mports = 1;
		cdata->cmic.mctrs = 1;
		cdata->oaes.ns_attribute_notices = 1;
		cdata->ctratt.bits.host_id_exhid_supported = 1;
		cdata->ctratt.bits.fdps = ctrlr->subsys->fdp_supported;
		cdata->cntrltype = SPDK_NVME_CTRLR_IO;
		/* We do not have any actual limitation to the number of abort commands.
		 * We follow the recommendation by the NVMe specification.
		 */
		cdata->acl = NVMF_ABORT_COMMAND_LIMIT;
		cdata->frmw.slot1_ro = 1;
		cdata->frmw.num_slots = 1;

		cdata->lpa.cses = 1; /* Command Effects log page supported */
		cdata->lpa.mlps = 1;

		cdata->sqes.min = 6;
		cdata->sqes.max = 6;
		cdata->cqes.min = 4;
		cdata->cqes.max = 4;
		cdata->nn = subsystem->max_nsid;
		cdata->vwc.present = 1;
		cdata->vwc.flush_broadcast = SPDK_NVME_FLUSH_BROADCAST_NOT_SUPPORTED;

		cdata->nvmf_specific = ctrlr->cdata.nvmf_specific;

		cdata->oncs.nvmcmps = ctrlr->cdata.oncs.nvmcmps;
		cdata->oncs.nvmdsmsv = ctrlr->cdata.oncs.nvmdsmsv && nvmf_ctrlr_dsm_supported(ctrlr);
		cdata->oncs.nvmwzsv = ctrlr->cdata.oncs.nvmwzsv &&
				      nvmf_ctrlr_write_zeroes_supported(ctrlr);
		cdata->oncs.reservs = ctrlr->cdata.oncs.reservs;
		cdata->oncs.nvmcpys = ctrlr->cdata.oncs.nvmcpys;
		cdata->ocfs.copy_format0 = cdata->oncs.nvmcpys;
		if (subsystem->flags.ana_reporting) {
			/* Asymmetric Namespace Access Reporting is supported. */
			cdata->cmic.anars = 1;
			cdata->oaes.ana_change_notices = 1;

			cdata->anatt = ANA_TRANSITION_TIME_IN_SEC;
			/* ANA Change state is not used, and ANA Persistent Loss state
			 * is not supported for now.
			 */
			cdata->anacap.ana_optimized_state = 1;
			cdata->anacap.ana_non_optimized_state = 1;
			cdata->anacap.ana_inaccessible_state = 1;
			/* ANAGRPID does not change while namespace is attached to controller */
			cdata->anacap.no_change_anagrpid = 1;
			cdata->anagrpmax = subsystem->max_nsid;
			cdata->nanagrpid = subsystem->max_nsid;
		}

		nvmf_ctrlr_populate_oacs(ctrlr, cdata);

		assert(subsystem->tgt != NULL);
		cdata->crdt[0] = subsystem->tgt->crdt[0];
		cdata->crdt[1] = subsystem->tgt->crdt[1];
		cdata->crdt[2] = subsystem->tgt->crdt[2];

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

static int
nvmf_ns_identify_iocs_zns(struct spdk_nvmf_ns *ns,
			  struct spdk_nvme_cmd *cmd,
			  struct spdk_nvme_cpl *rsp,
			  struct spdk_nvme_zns_ns_data *nsdata_zns)
{
	nsdata_zns->zoc.variable_zone_capacity = 0;
	nsdata_zns->zoc.zone_active_excursions = 0;
	nsdata_zns->ozcs.read_across_zone_boundaries = 1;
	/* Underflowing the zero based mar and mor bdev helper results in the correct
	   value of FFFFFFFFh. */
	nsdata_zns->mar = spdk_bdev_get_max_active_zones(ns->bdev) - 1;
	nsdata_zns->mor = spdk_bdev_get_max_open_zones(ns->bdev) - 1;
	nsdata_zns->rrl = 0;
	nsdata_zns->frl = 0;
	nsdata_zns->lbafe[0].zsze = spdk_bdev_get_zone_size(ns->bdev);

	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ns_identify_iocs_nvm(struct spdk_nvmf_ns *ns,
			  struct spdk_nvme_cpl *rsp,
			  struct spdk_nvme_nvm_ns_data *nsdata_nvm)
{
	nvmf_bdev_ctrlr_identify_iocs_nvm(ns, nsdata_nvm);

	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ns_identify_iocs_specific(struct spdk_nvmf_ctrlr *ctrlr,
				    struct spdk_nvme_cmd *cmd,
				    struct spdk_nvme_cpl *rsp,
				    void *nsdata,
				    size_t nsdata_size)
{
	uint8_t csi = cmd->cdw11_bits.identify.csi;
	struct spdk_nvmf_ns *ns = _nvmf_ctrlr_get_ns_safe(ctrlr, cmd->nsid, rsp);

	memset(nsdata, 0, nsdata_size);

	if (ns == NULL) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	switch (csi) {
	case SPDK_NVME_CSI_ZNS:
		return nvmf_ns_identify_iocs_zns(ns, cmd, rsp, nsdata);
	case SPDK_NVME_CSI_NVM:
		if (!ctrlr->dif_insert_or_strip) {
			return nvmf_ns_identify_iocs_nvm(ns, rsp, nsdata);
		}
		break;
	default:
		break;
	}

	SPDK_DEBUGLOG(nvmf,
		      "Returning zero filled struct for the iocs specific ns "
		      "identify command and CSI 0x%02x\n",
		      csi);
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

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
	cdata_nvm->wzsl = spdk_u64log2(ctrlr->subsys->max_write_zeroes_size_kib >>
				       (2 + ctrlr->vcprop.cap.bits.mpsmin));

	/* The unit of max_discard_size_kib is KiB.
	 * The dmrsl indicates the maximum number of logical blocks for
	 * dataset management command.
	 */
	cdata_nvm->dmrsl = ctrlr->subsys->max_discard_size_kib << 1;
	cdata_nvm->dmrl = 1;

	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

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
	cdata_zns->zasl = spdk_u64log2(ctrlr->subsys->max_zone_append_size_kib >>
				       (12 + ctrlr->vcprop.cap.bits.mpsmin));

	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ctrlr_identify_iocs_specific(struct spdk_nvmf_ctrlr *ctrlr,
				       struct spdk_nvme_cmd *cmd,
				       struct spdk_nvme_cpl *rsp,
				       void *cdata,
				       size_t cdata_size)
{
	uint8_t csi = cmd->cdw11_bits.identify.csi;

	memset(cdata, 0, cdata_size);

	switch (csi) {
	case SPDK_NVME_CSI_NVM:
		return nvmf_ctrlr_identify_iocs_nvm(ctrlr, cmd, rsp, cdata);
	case SPDK_NVME_CSI_ZNS:
		return nvmf_ctrlr_identify_iocs_zns(ctrlr, cmd, rsp, cdata);
	default:
		break;
	}

	SPDK_DEBUGLOG(nvmf,
		      "Returning zero filled struct for the iocs specific ctrlr "
		      "identify command and CSI 0x%02x\n",
		      csi);
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_identify_ns_iocs_independent(struct spdk_nvmf_ctrlr *ctrlr,
				       struct spdk_nvme_cmd *cmd,
				       struct spdk_nvme_cpl *rsp,
				       struct spdk_nvme_ns_iocs_independent_data *nsdata)
{
	struct spdk_nvmf_ns *ns;

	memset(nsdata, 0, sizeof(*nsdata));

	/** From NVMe 2.0d
	 * If the controller supports the Namespace Management capability
	 * (refer to section 8.11) and the NSID field is set to FFFFFFFFh,
	 * then the controller returns an I/O Command Set Independent
	 * Identify Namespace data structure that specifies capabilities
	 * that are common for the controller.
	 */
	if (ctrlr->cdata.oacs.nms && cmd->nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		nsdata->nmic.shrns = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = _nvmf_ctrlr_get_ns_safe(ctrlr, cmd->nsid, rsp);

	if (ns == NULL) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	nsdata->nmic.shrns = 1;
	nsdata->rescap = nvmf_ns_get_rescap(ns);

	nsdata->anagrpid = ns->anagrpid;
	nsdata->nstat.nrdy = 1;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static bool
nvmf_ctrlr_is_csi_supported(struct spdk_nvmf_ctrlr *ctrlr, uint8_t csi)
{
	return (csi == SPDK_NVME_CSI_NVM) ||
	       (csi == SPDK_NVME_CSI_ZNS && nvmf_subsystem_has_zns_iocs(ctrlr->subsys));
}

static int
nvmf_ctrlr_identify_active_ns_list(struct spdk_nvmf_ctrlr *ctrlr,
				   struct spdk_nvme_cmd *cmd,
				   struct spdk_nvme_cpl *rsp,
				   struct spdk_nvme_ns_list *ns_list,
				   bool iocs)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_ns *ns;
	uint32_t count = 0;
	uint8_t csi = cmd->cdw11_bits.identify.csi;

	if (cmd->nsid >= 0xfffffffeUL) {
		SPDK_ERRLOG("Identify Active Namespace List with invalid NSID %u\n", cmd->nsid);
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (iocs && !nvmf_ctrlr_is_csi_supported(ctrlr, csi)) {
		SPDK_ERRLOG("Identify Active Namespace List with invalid CSI %u\n", csi);
		rsp->status.sc  = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	memset(ns_list, 0, sizeof(*ns_list));

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		if (ns->opts.nsid <= cmd->nsid || !nvmf_ctrlr_ns_is_visible(ctrlr, ns->opts.nsid)) {
			continue;
		}

		if (iocs && csi != ns->csi) {
			continue;
		}

		ns_list->ns_list[count++] = ns->opts.nsid;
		if (count == SPDK_COUNTOF(ns_list->ns_list)) {
			break;
		}
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static void
_add_ns_id_desc(void **buf_ptr, size_t *buf_remain,
		enum spdk_nvme_nidt type,
		const void *data, size_t data_size)
{
	struct spdk_nvme_ns_id_desc *desc;
	size_t desc_size = sizeof(*desc) + data_size;

	/*
	 * These should never fail in practice, since all valid NS ID descriptors
	 * should be defined so that they fit in the available 4096-byte buffer.
	 */
	assert(data_size > 0);
	assert(data_size <= UINT8_MAX);
	assert(desc_size < *buf_remain);
	if (data_size == 0 || data_size > UINT8_MAX || desc_size > *buf_remain) {
		return;
	}

	desc = *buf_ptr;
	desc->nidt = type;
	desc->nidl = data_size;
	memcpy(desc->nid, data, data_size);

	*buf_ptr += desc_size;
	*buf_remain -= desc_size;
}

static int
nvmf_ctrlr_identify_ns_id_descriptor_list(
	struct spdk_nvmf_ctrlr *ctrlr,
	struct spdk_nvme_cmd *cmd,
	struct spdk_nvme_cpl *rsp,
	void *id_desc_list, size_t id_desc_list_size)
{
	struct spdk_nvmf_ns *ns;
	size_t buf_remain = id_desc_list_size;
	void *buf_ptr = id_desc_list;
	uint32_t nsid = cmd->nsid;

	if (nsid == 0 || nsid > ctrlr->subsys->max_nsid) {
		SPDK_ERRLOG("Identify Namespace Identification Descriptor list with invalid NSID %u\n", nsid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = nvmf_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL || ns->bdev == NULL) {
		SPDK_ERRLOG("Identify Namespace Identification Descriptor list with inactive NSID %u\n", nsid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

#define ADD_ID_DESC(type, data, size) \
	do { \
		if (!spdk_mem_all_zero(data, size)) { \
			_add_ns_id_desc(&buf_ptr, &buf_remain, type, data, size); \
		} \
	} while (0)

	ADD_ID_DESC(SPDK_NVME_NIDT_EUI64, ns->opts.eui64, sizeof(ns->opts.eui64));
	ADD_ID_DESC(SPDK_NVME_NIDT_NGUID, ns->opts.nguid, sizeof(ns->opts.nguid));
	ADD_ID_DESC(SPDK_NVME_NIDT_UUID, &ns->opts.uuid, sizeof(ns->opts.uuid));
	ADD_ID_DESC(SPDK_NVME_NIDT_CSI, &ns->csi, sizeof(uint8_t));

	/*
	 * The list is automatically 0-terminated, both in the temporary buffer
	 * used by nvmf_ctrlr_identify(), and the eventual iov destination -
	 * controller to host buffers in admin commands always get zeroed in
	 * nvmf_ctrlr_process_admin_cmd().
	 */

#undef ADD_ID_DESC

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_identify_iocs(struct spdk_nvmf_ctrlr *ctrlr,
			 struct spdk_nvme_cmd *cmd,
			 struct spdk_nvme_cpl *rsp,
			 void *cdata, size_t cdata_size)
{
	struct spdk_nvme_iocs_vector *vector;
	struct spdk_nvmf_ns *ns;

	if (cdata_size < sizeof(struct spdk_nvme_iocs_vector)) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* For now we only support this command sent to the current
	 * controller.
	 */
	if (cmd->cdw10_bits.identify.cntid != 0xFFFF &&
	    cmd->cdw10_bits.identify.cntid != ctrlr->cntlid) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	memset(cdata, 0, cdata_size);

	vector = cdata;
	vector->nvm = 1;
	for (ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(ctrlr->subsys, ns)) {
		if (ns->bdev == NULL) {
			continue;
		}
		if (spdk_bdev_is_zoned(ns->bdev)) {
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
	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);

	SPDK_DEBUGLOG(nvmf, "Received identify command with CNS 0x%02x\n", cns);

	switch (cns) {
	case SPDK_NVME_IDENTIFY_NS:
		/* Function below can be asynchronous & we always need to have the data in request's buffer
		 * So just return here */
		return spdk_nvmf_ctrlr_identify_ns_ext(req);
	case SPDK_NVME_IDENTIFY_CTRLR:
		ret = spdk_nvmf_ctrlr_identify_ctrlr(ctrlr, (void *)&tmpbuf);
		break;
	case SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST:
		ret = nvmf_ctrlr_identify_active_ns_list(ctrlr, cmd, rsp, (void *)&tmpbuf, false);
		break;
	case SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST:
		ret = nvmf_ctrlr_identify_ns_id_descriptor_list(ctrlr, cmd, rsp,
				tmpbuf, sizeof(tmpbuf));
		break;
	case SPDK_NVME_IDENTIFY_NS_IOCS:
		ret = spdk_nvmf_ns_identify_iocs_specific(ctrlr, cmd, rsp, (void *)&tmpbuf, sizeof(tmpbuf));
		break;
	case SPDK_NVME_IDENTIFY_CTRLR_IOCS:
		ret = spdk_nvmf_ctrlr_identify_iocs_specific(ctrlr, cmd, rsp, (void *)&tmpbuf, sizeof(tmpbuf));
		break;
	case SPDK_NVME_IDENTIFY_NS_IOCS_INDEPENDENT:
		ret = spdk_nvmf_identify_ns_iocs_independent(ctrlr, cmd, rsp, (void *)&tmpbuf);
		break;
	case SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST_IOCS:
		ret = nvmf_ctrlr_identify_active_ns_list(ctrlr, cmd, rsp, (void *)&tmpbuf, true);
		break;
	case SPDK_NVME_IDENTIFY_IOCS:
		ret = nvmf_ctrlr_identify_iocs(ctrlr, cmd, rsp, (void *)&tmpbuf, sizeof(tmpbuf));
		break;
	default:
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

static bool
nvmf_qpair_abort_aer(struct spdk_nvmf_qpair *qpair, uint16_t cid)
{
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvmf_request *req;
	int i;

	if (!nvmf_qpair_is_admin_queue(qpair)) {
		return false;
	}

	assert(spdk_get_thread() == ctrlr->thread);

	for (i = 0; i < ctrlr->nr_aer_reqs; i++) {
		if (ctrlr->aer_req[i]->cmd->nvme_cmd.cid == cid) {
			SPDK_DEBUGLOG(nvmf, "Aborting AER request\n");
			req = ctrlr->aer_req[i];
			ctrlr->aer_req[i] = NULL;
			ctrlr->nr_aer_reqs--;

			/* Move the last req to the aborting position for making aer_reqs
			 * in continuous
			 */
			if (i < ctrlr->nr_aer_reqs) {
				ctrlr->aer_req[i] = ctrlr->aer_req[ctrlr->nr_aer_reqs];
				ctrlr->aer_req[ctrlr->nr_aer_reqs] = NULL;
			}

			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
			_nvmf_request_complete(req);
			return true;
		}
	}

	return false;
}

void
nvmf_qpair_abort_pending_zcopy_reqs(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_request *req, *tmp;

	TAILQ_FOREACH_SAFE(req, &qpair->outstanding, link, tmp) {
		if (req->zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE) {
			/* Zero-copy requests are kept on the outstanding queue from the moment
			 * zcopy_start is sent until a zcopy_end callback is received.  Therefore,
			 * we can't remove them from the outstanding queue here, but need to rely on
			 * the transport to do a zcopy_end to release their buffers and, in turn,
			 * remove them from the queue.
			 */
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
			nvmf_transport_req_free(req);
		}
	}
}

static bool
nvmf_qpair_cid_is_reservation(const struct spdk_nvmf_qpair *qpair, uint16_t cid)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cmd *cmd;

	TAILQ_FOREACH(req, &qpair->outstanding, link) {
		cmd = &req->cmd->nvme_cmd;

		if (cmd->cid != cid) {
			continue;
		}
		switch (cmd->opc) {
		case SPDK_NVME_OPC_RESERVATION_REGISTER:
		case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		case SPDK_NVME_OPC_RESERVATION_RELEASE:
		case SPDK_NVME_OPC_RESERVATION_REPORT:
			return true;
		default:
			return false;
		}
	}
	return false;
}

static void
nvmf_qpair_abort_request(struct spdk_nvmf_qpair *qpair, struct spdk_nvmf_request *req)
{
	uint16_t cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;

	if (nvmf_qpair_abort_aer(qpair, cid)) {
		SPDK_DEBUGLOG(nvmf, "abort ctrlr=%p sqid=%u cid=%u successful\n",
			      qpair->ctrlr, qpair->qid, cid);
		req->rsp->nvme_cpl.cdw0 &= ~1U; /* Command successfully aborted */

		spdk_nvmf_request_complete(req);
		return;
	}
	if (nvmf_qpair_cid_is_reservation(qpair, cid)) {
		/* We don't support aborting reservation requests, leave completion as not-aborted */
		SPDK_DEBUGLOG(nvmf, "abort ctrlr=%p sqid=%u cid=%u for reservation not supported\n",
			      qpair->ctrlr, qpair->qid, cid);
		spdk_nvmf_request_complete(req);
		return;
	}

	nvmf_transport_qpair_abort_request(qpair, req);
}

static void
nvmf_ctrlr_abort_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_request *req = spdk_io_channel_iter_get_ctx(i);

	if (status == 0) {
		/* There was no qpair whose ID matches SQID of the abort command.
		 * Hence call _nvmf_request_complete() here.
		 */
		_nvmf_request_complete(req);
	}
}

static void
nvmf_ctrlr_abort_on_pg(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_request *req = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch);
	uint16_t sqid = req->cmd->nvme_cmd.cdw10_bits.abort.sqid;
	struct spdk_nvmf_qpair *qpair;

	TAILQ_FOREACH(qpair, &group->qpairs, link) {
		if (qpair->ctrlr == req->qpair->ctrlr && qpair->qid == sqid) {
			/* Found the qpair */

			nvmf_qpair_abort_request(qpair, req);

			/* Return -1 for the status so the iteration across threads stops. */
			spdk_for_each_channel_continue(i, -1);
			return;
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static int
nvmf_ctrlr_abort(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	rsp->cdw0 = 1U; /* Command not aborted */
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;

	/* Send a message to each poll group, searching for this ctrlr, sqid, and command. */
	spdk_for_each_channel(req->qpair->ctrlr->subsys->tgt,
			      nvmf_ctrlr_abort_on_pg,
			      req,
			      nvmf_ctrlr_abort_done
			     );

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_ctrlr_abort_request(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_request *req_to_abort = req->req_to_abort;
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	int rc;

	assert(req_to_abort != NULL);

	if (g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_ABORT].hdlr &&
	    nvmf_qpair_is_admin_queue(req_to_abort->qpair)) {
		return g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_ABORT].hdlr(req);
	}

	rc = spdk_nvmf_request_get_bdev(req_to_abort->cmd->nvme_cmd.nsid, req_to_abort,
					&bdev, &desc, &ch);
	if (rc != 0) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return spdk_nvmf_bdev_ctrlr_abort_cmd(bdev, desc, ch, req, req_to_abort);
}

static int
get_features_generic(struct spdk_nvmf_request *req, uint32_t cdw0)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	rsp->cdw0 = cdw0;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/* we have to use the typedef in the function declaration to appease astyle. */
typedef enum spdk_nvme_path_status_code spdk_nvme_path_status_code_t;

static spdk_nvme_path_status_code_t
_nvme_ana_state_to_path_status(enum spdk_nvme_ana_state ana_state)
{
	switch (ana_state) {
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
		return SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE;
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
		return SPDK_NVME_SC_ASYMMETRIC_ACCESS_PERSISTENT_LOSS;
	case SPDK_NVME_ANA_CHANGE_STATE:
		return SPDK_NVME_SC_ASYMMETRIC_ACCESS_TRANSITION;
	default:
		return SPDK_NVME_SC_INTERNAL_PATH_ERROR;
	}
}

static int
nvmf_ctrlr_get_features(struct spdk_nvmf_request *req)
{
	uint8_t feature;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	enum spdk_nvme_ana_state ana_state;

	feature = cmd->cdw10_bits.get_features.fid;

	if ((cmd->nsid > ctrlr->subsys->max_nsid) && (cmd->nsid != SPDK_NVME_GLOBAL_NS_TAG)) {
		SPDK_ERRLOG("Get Features command with invalid NSID %u, feature ID 0x%02x\n", cmd->nsid, feature);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_nvmf_subsystem_is_discovery(ctrlr->subsys)) {
		/*
		 * Features supported by Discovery controller
		 */
		switch (feature) {
		case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
			return get_features_generic(req, ctrlr->feat.keep_alive_timer.raw);
		case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
			return get_features_generic(req, ctrlr->feat.async_event_configuration.raw);
		default:
			SPDK_INFOLOG(nvmf, "Get Features command with unsupported feature ID 0x%02x\n", feature);
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}
	/*
	 * Process Get Features command for non-discovery controller
	 */
	ana_state = nvmf_ctrlr_get_ana_state_from_nsid(ctrlr, cmd->nsid);
	switch (ana_state) {
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
	case SPDK_NVME_ANA_CHANGE_STATE:
		switch (feature) {
		case SPDK_NVME_FEAT_ERROR_RECOVERY:
		case SPDK_NVME_FEAT_WRITE_ATOMICITY:
		case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
		case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
			response->status.sct = SPDK_NVME_SCT_PATH;
			response->status.sc = _nvme_ana_state_to_path_status(ana_state);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		default:
			break;
		}
		break;
	default:
		break;
	}

	switch (feature) {
	case SPDK_NVME_FEAT_ARBITRATION:
		return get_features_generic(req, ctrlr->feat.arbitration.raw);
	case SPDK_NVME_FEAT_POWER_MANAGEMENT:
		return get_features_generic(req, ctrlr->feat.power_management.raw);
	case SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD:
		return nvmf_ctrlr_get_features_temperature_threshold(req);
	case SPDK_NVME_FEAT_ERROR_RECOVERY:
		return get_features_generic(req, ctrlr->feat.error_recovery.raw);
	case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE:
		return get_features_generic(req, ctrlr->feat.volatile_write_cache.raw);
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		return get_features_generic(req, ctrlr->feat.number_of_queues.raw);
	case SPDK_NVME_FEAT_INTERRUPT_COALESCING:
		return get_features_generic(req, ctrlr->feat.interrupt_coalescing.raw);
	case SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION:
		return nvmf_ctrlr_get_features_interrupt_vector_configuration(req);
	case SPDK_NVME_FEAT_WRITE_ATOMICITY:
		return get_features_generic(req, ctrlr->feat.write_atomicity.raw);
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		return get_features_generic(req, ctrlr->feat.async_event_configuration.raw);
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		return get_features_generic(req, ctrlr->feat.keep_alive_timer.raw);
	case SPDK_NVME_FEAT_HOST_IDENTIFIER:
		return nvmf_ctrlr_get_features_host_identifier(req);
	case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
		return nvmf_ctrlr_get_features_reservation_notification_mask(req);
	case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
		return nvmf_ctrlr_get_features_reservation_persistence(req);
	case SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT:
		return nvmf_ctrlr_get_features_host_behavior_support(req);
	default:
		SPDK_INFOLOG(nvmf, "Get Features command with unsupported feature ID 0x%02x\n", feature);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static bool
is_feature_ctrlr_scope(uint8_t feature)
{
	switch (feature) {
	case SPDK_NVME_FEAT_ARBITRATION:
	case SPDK_NVME_FEAT_POWER_MANAGEMENT:
	case SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD:
	case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE:
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
	case SPDK_NVME_FEAT_INTERRUPT_COALESCING:
	case SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION:
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
	case SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION:
	case SPDK_NVME_FEAT_HOST_MEM_BUFFER:
	case SPDK_NVME_FEAT_TIMESTAMP:
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
	case SPDK_NVME_FEAT_HOST_CONTROLLED_THERMAL_MANAGEMENT:
	case SPDK_NVME_FEAT_NON_OPERATIONAL_POWER_STATE_CONFIG:
	case SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT:
	case SPDK_NVME_FEAT_IO_COMMAND_SET_PROFILE:
	case SPDK_NVME_FEAT_ENHANCED_CONTROLLER_METADATA:
	case SPDK_NVME_FEAT_CONTROLLER_METADATA:
	case SPDK_NVME_FEAT_SOFTWARE_PROGRESS_MARKER:
	case SPDK_NVME_FEAT_HOST_IDENTIFIER:
		return true;
	default:
		return false;
	}
}

static int
nvmf_ctrlr_set_features(struct spdk_nvmf_request *req)
{
	uint8_t feature, save;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	enum spdk_nvme_ana_state ana_state;
	/*
	 * Features are not saveable by the controller as indicated by
	 * ONCS field of the Identify Controller data.
	 * */
	save = cmd->cdw10_bits.set_features.sv;
	if (save) {
		response->status.sc = SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE;
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	feature = cmd->cdw10_bits.set_features.fid;

	if ((cmd->nsid > ctrlr->subsys->max_nsid) && (cmd->nsid != SPDK_NVME_GLOBAL_NS_TAG)) {
		SPDK_ERRLOG("Set Features command with invalid NSID %u, feature ID 0x%02x\n", cmd->nsid, feature);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_nvmf_subsystem_is_discovery(ctrlr->subsys)) {
		/*
		 * Features supported by Discovery controller
		 */
		switch (feature) {
		case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
			return nvmf_ctrlr_set_features_keep_alive_timer(req);
		case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
			return nvmf_ctrlr_set_features_async_event_configuration(req);
		default:
			SPDK_INFOLOG(nvmf, "Set Features command with unsupported feature ID 0x%02x\n", feature);
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}
	/*
	 * Process Set Features command for non-discovery controller
	 */
	ana_state = nvmf_ctrlr_get_ana_state_from_nsid(ctrlr, cmd->nsid);
	switch (ana_state) {
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
	case SPDK_NVME_ANA_CHANGE_STATE:
		if (cmd->nsid == SPDK_NVME_GLOBAL_NS_TAG) {
			response->status.sct = SPDK_NVME_SCT_PATH;
			response->status.sc = _nvme_ana_state_to_path_status(ana_state);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else {
			switch (feature) {
			case SPDK_NVME_FEAT_ERROR_RECOVERY:
			case SPDK_NVME_FEAT_WRITE_ATOMICITY:
			case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
			case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
				response->status.sct = SPDK_NVME_SCT_PATH;
				response->status.sc = _nvme_ana_state_to_path_status(ana_state);
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			default:
				break;
			}
		}
		break;
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
		response->status.sct = SPDK_NVME_SCT_PATH;
		response->status.sc = SPDK_NVME_SC_ASYMMETRIC_ACCESS_PERSISTENT_LOSS;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	default:
		break;
	}

	if ((cmd->nsid < ctrlr->subsys->max_nsid) && (cmd->nsid != 0)) {
		if (is_feature_ctrlr_scope(feature)) {
			SPDK_ERRLOG("Set Feature Controller scope with valid NSID. feature ID 0x%02x, NSID %u\n",
				    feature, cmd->nsid);
			response->status.sc = SPDK_NVME_SC_FEATURE_NOT_NAMESPACE_SPECIFIC;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}

	switch (feature) {
	case SPDK_NVME_FEAT_ARBITRATION:
		return nvmf_ctrlr_set_features_arbitration(req);
	case SPDK_NVME_FEAT_POWER_MANAGEMENT:
		return nvmf_ctrlr_set_features_power_management(req);
	case SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD:
		return nvmf_ctrlr_set_features_temperature_threshold(req);
	case SPDK_NVME_FEAT_ERROR_RECOVERY:
		return nvmf_ctrlr_set_features_error_recovery(req);
	case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE:
		return nvmf_ctrlr_set_features_volatile_write_cache(req);
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		return nvmf_ctrlr_set_features_number_of_queues(req);
	case SPDK_NVME_FEAT_INTERRUPT_COALESCING:
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVME_SC_FEATURE_NOT_CHANGEABLE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	case SPDK_NVME_FEAT_WRITE_ATOMICITY:
		return nvmf_ctrlr_set_features_write_atomicity(req);
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		return nvmf_ctrlr_set_features_async_event_configuration(req);
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		return nvmf_ctrlr_set_features_keep_alive_timer(req);
	case SPDK_NVME_FEAT_HOST_IDENTIFIER:
		return nvmf_ctrlr_set_features_host_identifier(req);
	case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
		return nvmf_ctrlr_set_features_reservation_notification_mask(req);
	case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
		return nvmf_ctrlr_set_features_reservation_persistence(req);
	case SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT:
		return nvmf_ctrlr_set_features_host_behavior_support(req);
	default:
		SPDK_INFOLOG(nvmf, "Set Features command with unsupported feature ID 0x%02x\n", feature);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
nvmf_ctrlr_keep_alive(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;

	SPDK_DEBUGLOG(nvmf, "Keep Alive\n");
	/*
	 * To handle keep alive just clear or reset the
	 * ctrlr based keep alive duration counter.
	 * When added, a separate timer based process
	 * will monitor if the time since last recorded
	 * keep alive has exceeded the max duration and
	 * take appropriate action.
	 */
	ctrlr->last_keep_alive_tick = spdk_get_ticks();

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static bool
is_cmd_ctrlr_specific(struct spdk_nvme_cmd *cmd)
{
	switch (cmd->opc) {
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_CREATE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_ABORT:
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
	case SPDK_NVME_OPC_FIRMWARE_COMMIT:
	case SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD:
	case SPDK_NVME_OPC_KEEP_ALIVE:
	case SPDK_NVME_OPC_VIRTUALIZATION_MANAGEMENT:
	case SPDK_NVME_OPC_NVME_MI_SEND:
	case SPDK_NVME_OPC_NVME_MI_RECEIVE:
	case SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG:
	case SPDK_NVME_OPC_SANITIZE:
		return true;
	default:
		return false;
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
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	int rc;

	assert(ctrlr != NULL);
	if (cmd->opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
		/* We do not want to treat AERs as outstanding commands,
		 * so decrement mgmt_io_outstanding here to offset
		 * the increment that happened prior to this call.
		 */
		sgroup = &req->qpair->group->sgroups[ctrlr->subsys->id];
		assert(sgroup != NULL);
		sgroup->mgmt_io_outstanding--;
	}

	assert(spdk_get_thread() == ctrlr->thread);

	if (cmd->fuse != 0 ||
	    (is_cmd_ctrlr_specific(cmd) && (cmd->nsid != 0))) {
		/* Fused admin commands are not supported.
		 * Commands with controller scope - should be rejected if NSID is set.
		 */
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (ctrlr->vcprop.cc.bits.en != 1) {
		SPDK_ERRLOG("Admin command sent to disabled controller\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->iovcnt && spdk_nvme_opc_get_data_transfer(cmd->opc) == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		spdk_iov_memset(req->iov, req->iovcnt, 0);
	}

	if (spdk_nvmf_subsystem_is_discovery(ctrlr->subsys)) {
		/* Discovery controllers only support these admin OPS. */
		switch (cmd->opc) {
		case SPDK_NVME_OPC_IDENTIFY:
		case SPDK_NVME_OPC_GET_LOG_PAGE:
		case SPDK_NVME_OPC_KEEP_ALIVE:
		case SPDK_NVME_OPC_SET_FEATURES:
		case SPDK_NVME_OPC_GET_FEATURES:
		case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
			break;
		default:
			goto invalid_opcode;
		}
	}

	/* Call a custom adm cmd handler if set. Aborts are handled in a different path (see nvmf_passthru_admin_cmd) */
	if (g_nvmf_custom_admin_cmd_hdlrs[cmd->opc].hdlr && cmd->opc != SPDK_NVME_OPC_ABORT) {
		rc = g_nvmf_custom_admin_cmd_hdlrs[cmd->opc].hdlr(req);
		if (rc >= SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
			/* The handler took care of this command */
			return rc;
		}
	}

	/* We only want to send passthrough admin commands to namespaces.
	 * However, we don't want to passthrough a command with intended for all namespaces.
	 */
	if (ctrlr->subsys->passthrough && cmd->nsid && cmd->nsid != SPDK_NVME_GLOBAL_NS_TAG) {
		return nvmf_passthru_admin_cmd(req);
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		return nvmf_ctrlr_get_log_page(req);
	case SPDK_NVME_OPC_IDENTIFY:
		return nvmf_ctrlr_identify(req);
	case SPDK_NVME_OPC_ABORT:
		return nvmf_ctrlr_abort(req);
	case SPDK_NVME_OPC_GET_FEATURES:
		return nvmf_ctrlr_get_features(req);
	case SPDK_NVME_OPC_SET_FEATURES:
		return nvmf_ctrlr_set_features(req);
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		return nvmf_ctrlr_async_event_request(req);
	case SPDK_NVME_OPC_KEEP_ALIVE:
		return nvmf_ctrlr_keep_alive(req);

	case SPDK_NVME_OPC_CREATE_IO_SQ:
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		/* Create and Delete I/O CQ/SQ not allowed in NVMe-oF */
		goto invalid_opcode;

	default:
		goto invalid_opcode;
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
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_capsule_cmd *cap_hdr;

	cap_hdr = &req->cmd->nvmf_cmd;

	if (qpair->ctrlr == NULL) {
		/* No ctrlr established yet; the only valid command is Connect */
		assert(cap_hdr->fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT);
		return nvmf_ctrlr_cmd_connect(req);
	} else if (nvmf_qpair_is_admin_queue(qpair)) {
		/*
		 * Controller session is established, and this is an admin queue.
		 * Disallow Connect and allow other fabrics commands.
		 */
		switch (cap_hdr->fctype) {
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET:
			return nvmf_property_set(req);
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET:
			return nvmf_property_get(req);
		case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND:
		case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV:
			return nvmf_auth_request_exec(req);
		default:
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
		switch (cap_hdr->fctype) {
		case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND:
		case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV:
			return nvmf_auth_request_exec(req);
		default:
			SPDK_DEBUGLOG(nvmf, "Unexpected I/O fctype 0x%x\n", cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}
}

static inline void
nvmf_ctrlr_queue_pending_async_event(struct spdk_nvmf_ctrlr *ctrlr,
				     union spdk_nvme_async_event_completion *event)
{
	struct spdk_nvmf_async_event_completion *nvmf_event;

	nvmf_event = calloc(1, sizeof(*nvmf_event));
	if (!nvmf_event) {
		SPDK_ERRLOG("Alloc nvmf event failed, ignore the event\n");
		return;
	}
	nvmf_event->event.raw = event->raw;
	STAILQ_INSERT_TAIL(&ctrlr->async_events, nvmf_event, link);
}

static inline int
nvmf_ctrlr_async_event_notification(struct spdk_nvmf_ctrlr *ctrlr,
				    union spdk_nvme_async_event_completion *event)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;

	assert(spdk_get_thread() == ctrlr->thread);

	/* If there is no outstanding AER request, queue the event.  Then
	 * if an AER is later submitted, this event can be sent as a
	 * response.
	 */
	if (ctrlr->nr_aer_reqs == 0) {
		nvmf_ctrlr_queue_pending_async_event(ctrlr, event);
		return 0;
	}

	req = ctrlr->aer_req[--ctrlr->nr_aer_reqs];
	rsp = &req->rsp->nvme_cpl;

	rsp->cdw0 = event->raw;

	_nvmf_request_complete(req);
	ctrlr->aer_req[ctrlr->nr_aer_reqs] = NULL;

	return 0;
}

int
nvmf_ctrlr_async_event_ns_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	union spdk_nvme_async_event_completion event = {0};

	/* Users may disable the event notification */
	if (!ctrlr->feat.async_event_configuration.bits.ns_attr_notice) {
		return 0;
	}

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGE_MASK_BIT)) {
		return 0;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE;
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED;
	event.bits.log_page_identifier = SPDK_NVME_LOG_CHANGED_NS_LIST;

	return nvmf_ctrlr_async_event_notification(ctrlr, &event);
}

int
nvmf_ctrlr_async_event_ana_change_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	union spdk_nvme_async_event_completion event = {0};

	/* Users may disable the event notification */
	if (!ctrlr->feat.async_event_configuration.bits.ana_change_notice) {
		return 0;
	}

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_ANA_CHANGE_MASK_BIT)) {
		return 0;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE;
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_ANA_CHANGE;
	event.bits.log_page_identifier = SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS;

	return nvmf_ctrlr_async_event_notification(ctrlr, &event);
}

void
nvmf_ctrlr_async_event_reservation_notification(struct spdk_nvmf_ctrlr *ctrlr)
{
	union spdk_nvme_async_event_completion event = {0};

	if (!ctrlr->num_avail_log_pages) {
		return;
	}

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL_MASK_BIT)) {
		return;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_IO;
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL;
	event.bits.log_page_identifier = SPDK_NVME_LOG_RESERVATION_NOTIFICATION;

	nvmf_ctrlr_async_event_notification(ctrlr, &event);
}

void
nvmf_ctrlr_async_event_discovery_log_change_notice(void *ctx)
{
	union spdk_nvme_async_event_completion event = {0};
	struct spdk_nvmf_ctrlr *ctrlr = ctx;

	/* Users may disable the event notification manually or
	 * it may not be enabled due to keep alive timeout
	 * not being set in connect command to discovery controller.
	 */
	if (!ctrlr->feat.async_event_configuration.bits.discovery_log_change_notice) {
		return;
	}

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE_MASK_BIT)) {
		return;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE;
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE;
	event.bits.log_page_identifier = SPDK_NVME_LOG_DISCOVERY;

	nvmf_ctrlr_async_event_notification(ctrlr, &event);
}

int
spdk_nvmf_ctrlr_async_event_error_event(struct spdk_nvmf_ctrlr *ctrlr,
					enum spdk_nvme_async_event_info_error info)
{
	union spdk_nvme_async_event_completion event;

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_ERROR_MASK_BIT)) {
		return 0;
	}

	if (info > SPDK_NVME_ASYNC_EVENT_FW_IMAGE_LOAD) {
		return 0;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_ERROR;
	event.bits.log_page_identifier = SPDK_NVME_LOG_ERROR;
	event.bits.async_event_info = info;

	return nvmf_ctrlr_async_event_notification(ctrlr, &event);
}

void
nvmf_qpair_free_aer(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	int i;

	if (ctrlr == NULL || !nvmf_qpair_is_admin_queue(qpair)) {
		return;
	}

	assert(spdk_get_thread() == ctrlr->thread);

	for (i = 0; i < ctrlr->nr_aer_reqs; i++) {
		spdk_nvmf_request_free(ctrlr->aer_req[i]);
		ctrlr->aer_req[i] = NULL;
	}

	ctrlr->nr_aer_reqs = 0;
}

void
spdk_nvmf_ctrlr_abort_aer(struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_request *req;
	int i;

	assert(spdk_get_thread() == ctrlr->thread);

	if (!ctrlr->nr_aer_reqs) {
		return;
	}

	for (i = 0; i < ctrlr->nr_aer_reqs; i++) {
		req = ctrlr->aer_req[i];

		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
		_nvmf_request_complete(req);

		ctrlr->aer_req[i] = NULL;
	}

	ctrlr->nr_aer_reqs = 0;
}

static void
_nvmf_ctrlr_add_reservation_log(void *ctx)
{
	struct spdk_nvmf_reservation_log *log = (struct spdk_nvmf_reservation_log *)ctx;
	struct spdk_nvmf_ctrlr *ctrlr = log->ctrlr;

	if (spdk_unlikely(ctrlr->log_page_count == UINT64_MAX)) {
		ctrlr->log_page_count = 0;
	}

	ctrlr->log_page_count++;

	/* Maximum number of queued log pages is 255 */
	if (ctrlr->num_avail_log_pages == 0xff) {
		struct spdk_nvmf_reservation_log *entry;
		entry = TAILQ_LAST(&ctrlr->log_head, log_page_head);
		entry->log.log_page_count = ctrlr->log_page_count;
		free(log);
		return;
	}

	log->log.log_page_count = ctrlr->log_page_count;
	log->log.num_avail_log_pages = ctrlr->num_avail_log_pages++;
	TAILQ_INSERT_TAIL(&ctrlr->log_head, log, link);

	nvmf_ctrlr_async_event_reservation_notification(ctrlr);
}

void
nvmf_ctrlr_reservation_notice_log(struct spdk_nvmf_ctrlr *ctrlr,
				  struct spdk_nvmf_ns *ns,
				  enum spdk_nvme_reservation_notification_log_page_type type)
{
	struct spdk_nvmf_reservation_log *log;

	switch (type) {
	case SPDK_NVME_RESERVATION_LOG_PAGE_EMPTY:
		return;
	case SPDK_NVME_REGISTRATION_PREEMPTED:
		if (ns->mask & SPDK_NVME_REGISTRATION_PREEMPTED_MASK) {
			return;
		}
		break;
	case SPDK_NVME_RESERVATION_RELEASED:
		if (ns->mask & SPDK_NVME_RESERVATION_RELEASED_MASK) {
			return;
		}
		break;
	case SPDK_NVME_RESERVATION_PREEMPTED:
		if (ns->mask & SPDK_NVME_RESERVATION_PREEMPTED_MASK) {
			return;
		}
		break;
	default:
		return;
	}

	log = calloc(1, sizeof(*log));
	if (!log) {
		SPDK_ERRLOG("Alloc log page failed, ignore the log\n");
		return;
	}
	log->ctrlr = ctrlr;
	log->log.type = type;
	log->log.nsid = ns->nsid;

	spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_add_reservation_log, log);
}

/* Check from subsystem poll group's namespace information data structure */
static bool
nvmf_ns_info_ctrlr_is_registrant(struct spdk_nvmf_subsystem_pg_ns_info *ns_info,
				 struct spdk_nvmf_ctrlr *ctrlr)
{
	uint32_t i;

	for (i = 0; i < SPDK_NVMF_MAX_NUM_REGISTRANTS; i++) {
		if (!spdk_uuid_compare(&ns_info->reg_hostid[i], &ctrlr->hostid)) {
			return true;
		}
	}

	return false;
}

/*
 * Check the NVMe command is permitted or not for current controller(Host).
 */
static int
nvmf_ns_reservation_request_check(struct spdk_nvmf_subsystem_pg_ns_info *ns_info,
				  struct spdk_nvmf_ctrlr *ctrlr,
				  struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	enum spdk_nvme_reservation_type rtype = ns_info->rtype;
	uint8_t status = SPDK_NVME_SC_SUCCESS;
	uint8_t racqa;
	bool is_registrant;

	/* No valid reservation */
	if (!rtype) {
		return 0;
	}

	is_registrant = nvmf_ns_info_ctrlr_is_registrant(ns_info, ctrlr);
	/* All registrants type and current ctrlr is a valid registrant */
	if ((rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS ||
	     rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS) && is_registrant) {
		return 0;
	} else if (!spdk_uuid_compare(&ns_info->holder_id, &ctrlr->hostid)) {
		return 0;
	}

	/* Non-holder for current controller */
	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_COMPARE:
		if (rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS) {
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}
		if ((rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY ||
		     rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS) && !is_registrant) {
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
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}
		if (!is_registrant) {
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		}
		break;
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		racqa = cmd->cdw10_bits.resv_acquire.racqa;
		if (racqa == SPDK_NVME_RESERVE_ACQUIRE) {
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}
		if (!is_registrant) {
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		}
		break;
	case SPDK_NVME_OPC_RESERVATION_RELEASE:
		if (!is_registrant) {
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		}
		break;
	default:
		break;
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	if (status == SPDK_NVME_SC_RESERVATION_CONFLICT) {
		return -EPERM;
	}

	return 0;
}

static int
nvmf_ctrlr_process_io_fused_cmd(struct spdk_nvmf_request *req, struct spdk_bdev *bdev,
				struct spdk_bdev_desc *desc, struct spdk_io_channel *ch)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_request *first_fused_req = req->qpair->first_fused_req;
	int rc;

	if (spdk_unlikely(!req->qpair->ctrlr->cdata.fuses.fcws)) {
		SPDK_DEBUGLOG(nvmf, "Controller does not support fused operation.\n");
		goto invalid_field;
	}

	if (cmd->fuse == SPDK_NVME_CMD_FUSE_FIRST) {
		/* first fused operation (should be compare) */
		if (first_fused_req != NULL) {
			struct spdk_nvme_cpl *fused_response = &first_fused_req->rsp->nvme_cpl;

			SPDK_ERRLOG("Wrong sequence of fused operations\n");

			/* abort req->qpair->first_fused_request and continue with new fused command */
			fused_response->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
			fused_response->status.sct = SPDK_NVME_SCT_GENERIC;
			_nvmf_request_complete(first_fused_req);
		} else if (cmd->opc != SPDK_NVME_OPC_COMPARE) {
			SPDK_ERRLOG("Wrong op code of fused operations\n");
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		req->qpair->first_fused_req = req;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	} else if (cmd->fuse == SPDK_NVME_CMD_FUSE_SECOND) {
		/* second fused operation (should be write) */
		if (first_fused_req == NULL) {
			SPDK_ERRLOG("Wrong sequence of fused operations\n");
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else if (cmd->opc != SPDK_NVME_OPC_WRITE) {
			struct spdk_nvme_cpl *fused_response = &first_fused_req->rsp->nvme_cpl;

			SPDK_ERRLOG("Wrong op code of fused operations\n");

			/* abort req->qpair->first_fused_request and fail current command */
			fused_response->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
			fused_response->status.sct = SPDK_NVME_SCT_GENERIC;
			_nvmf_request_complete(first_fused_req);

			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			req->qpair->first_fused_req = NULL;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		/* save request of first command to generate response later */
		req->first_fused_req = first_fused_req;
		req->first_fused = true;
		req->qpair->first_fused_req = NULL;
	} else {
		SPDK_ERRLOG("Invalid fused command fuse field.\n");
		goto invalid_field;
	}

	rc = nvmf_bdev_ctrlr_compare_and_write_cmd(bdev, desc, ch, req->first_fused_req, req);

	if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		if (spdk_nvme_cpl_is_error(rsp)) {
			struct spdk_nvme_cpl *fused_response = &first_fused_req->rsp->nvme_cpl;

			fused_response->status = rsp->status;
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
			/* Complete first of fused commands. Second will be completed by upper layer */
			_nvmf_request_complete(first_fused_req);
			req->first_fused_req = NULL;
			req->first_fused = false;
		}
	}

	return rc;

invalid_field:
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	rsp->status.dnr = 1;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

bool
nvmf_ctrlr_use_zcopy(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_transport *transport = req->qpair->transport;
	struct spdk_nvmf_ns *ns;

	assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_NONE);

	if (!transport->opts.zcopy) {
		return false;
	}

	if (nvmf_qpair_is_admin_queue(req->qpair)) {
		/* Admin queue */
		return false;
	}

	if ((req->cmd->nvme_cmd.opc != SPDK_NVME_OPC_WRITE) &&
	    (req->cmd->nvme_cmd.opc != SPDK_NVME_OPC_READ)) {
		/* Not a READ or WRITE command */
		return false;
	}

	if (req->cmd->nvme_cmd.fuse != SPDK_NVME_CMD_FUSE_NONE) {
		/* Fused commands dont use zcopy buffers */
		return false;
	}

	ns = nvmf_ctrlr_get_ns(req->qpair->ctrlr, req->cmd->nvme_cmd.nsid);
	if (ns == NULL || ns->bdev == NULL || !ns->zcopy) {
		return false;
	}

	req->zcopy_phase = NVMF_ZCOPY_PHASE_INIT;
	return true;
}

void
spdk_nvmf_request_zcopy_start(struct spdk_nvmf_request *req)
{
	assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_INIT);

	/* Set iovcnt to be the maximum number of iovs that the ZCOPY can use */
	req->iovcnt = NVMF_REQ_MAX_BUFFERS;

	spdk_nvmf_request_exec(req);
}

void
spdk_nvmf_request_zcopy_end(struct spdk_nvmf_request *req, bool commit)
{
	assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE);
	req->zcopy_phase = NVMF_ZCOPY_PHASE_END_PENDING;

	nvmf_bdev_ctrlr_zcopy_end(req, commit);
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
	uint32_t nsid;
	struct spdk_nvmf_ns *ns;
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_poll_group *group = qpair->group;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;
	enum spdk_nvme_ana_state ana_state;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	nsid = cmd->nsid;

	assert(ctrlr != NULL);
	if (spdk_unlikely(ctrlr->vcprop.cc.bits.en != 1)) {
		SPDK_ERRLOG("I/O command sent to disabled controller\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = nvmf_ctrlr_get_ns(ctrlr, nsid);
	if (spdk_unlikely(ns == NULL || ns->bdev == NULL)) {
		SPDK_DEBUGLOG(nvmf, "Unsuccessful query for nsid %u\n", cmd->nsid);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		response->status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ana_state = nvmf_ctrlr_get_ana_state(ctrlr, ns->anagrpid);
	if (spdk_unlikely(ana_state != SPDK_NVME_ANA_OPTIMIZED_STATE &&
			  ana_state != SPDK_NVME_ANA_NON_OPTIMIZED_STATE)) {
		SPDK_DEBUGLOG(nvmf, "Fail I/O command due to ANA state %d\n",
			      ana_state);
		response->status.sct = SPDK_NVME_SCT_PATH;
		response->status.sc = _nvme_ana_state_to_path_status(ana_state);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_likely(nvmf_subsystem_listener_is_active(ctrlr->listener))) {
		SPDK_DTRACE_PROBE3_TICKS(nvmf_request_io_exec_path, req,
					 ctrlr->listener->trid->traddr,
					 ctrlr->listener->trid->trsvcid);
	}

	/* scan-build falsely reporting dereference of null pointer */
	assert(group != NULL && group->sgroups != NULL);
	ns_info = &group->sgroups[ctrlr->subsys->id].ns_info[nsid - 1];
	if (nvmf_ns_reservation_request_check(ns_info, ctrlr, req)) {
		SPDK_DEBUGLOG(nvmf, "Reservation Conflict for nsid %u, opcode %u\n",
			      cmd->nsid, cmd->opc);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = ns->bdev;
	desc = ns->desc;
	ch = ns_info->channel;

	if (spdk_unlikely(cmd->fuse & SPDK_NVME_CMD_FUSE_MASK)) {
		return nvmf_ctrlr_process_io_fused_cmd(req, bdev, desc, ch);
	} else if (spdk_unlikely(qpair->first_fused_req != NULL)) {
		struct spdk_nvme_cpl *fused_response = &qpair->first_fused_req->rsp->nvme_cpl;

		SPDK_ERRLOG("Second fused cmd expected - failing first one (cntlid:%u, qid:%u, opcode:0x%x)\n",
			    ctrlr->cntlid, qpair->qid,
			    req->qpair->first_fused_req->cmd->nvmf_cmd.opcode);

		/* abort qpair->first_fused_request and continue with new command */
		fused_response->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
		fused_response->status.sct = SPDK_NVME_SCT_GENERIC;
		_nvmf_request_complete(qpair->first_fused_req);
		qpair->first_fused_req = NULL;
	}

	if (ctrlr->subsys->passthrough) {
		assert(ns->passthru_nsid > 0);
		req->orig_nsid = req->cmd->nvme_cmd.nsid;
		req->cmd->nvme_cmd.nsid = ns->passthru_nsid;

		return nvmf_bdev_ctrlr_nvme_passthru_io(bdev, desc, ch, req);
	}

	if (spdk_nvmf_request_using_zcopy(req)) {
		assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
		return nvmf_bdev_ctrlr_zcopy_start(bdev, desc, ch, req);
	} else {
		switch (cmd->opc) {
		case SPDK_NVME_OPC_READ:
			return nvmf_bdev_ctrlr_read_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_WRITE:
			return nvmf_bdev_ctrlr_write_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_FLUSH:
			return nvmf_bdev_ctrlr_flush_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_COMPARE:
			if (spdk_unlikely(!ctrlr->cdata.oncs.nvmcmps)) {
				goto invalid_opcode;
			}
			return nvmf_bdev_ctrlr_compare_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_WRITE_ZEROES:
			if (spdk_unlikely(!ctrlr->cdata.oncs.nvmwzsv)) {
				goto invalid_opcode;
			}
			return nvmf_bdev_ctrlr_write_zeroes_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_DATASET_MANAGEMENT:
			if (spdk_unlikely(!ctrlr->cdata.oncs.nvmdsmsv)) {
				goto invalid_opcode;
			}
			return nvmf_bdev_ctrlr_dsm_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_RESERVATION_REGISTER:
		case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		case SPDK_NVME_OPC_RESERVATION_RELEASE:
		case SPDK_NVME_OPC_RESERVATION_REPORT:
			if (spdk_unlikely(!ctrlr->cdata.oncs.reservs)) {
				goto invalid_opcode;
			}
			spdk_thread_send_msg(ctrlr->subsys->thread, nvmf_ns_reservation_request, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		case SPDK_NVME_OPC_COPY:
			if (spdk_unlikely(!ctrlr->cdata.oncs.nvmcpys)) {
				goto invalid_opcode;
			}
			return nvmf_bdev_ctrlr_copy_cmd(bdev, desc, ch, req);
		default:
			if (spdk_unlikely(qpair->transport->opts.disable_command_passthru)) {
				goto invalid_opcode;
			}
			if (ns->passthru_nsid) {
				req->orig_nsid = req->cmd->nvme_cmd.nsid;
				req->cmd->nvme_cmd.nsid = ns->passthru_nsid;
			}
			return nvmf_bdev_ctrlr_nvme_passthru_io(bdev, desc, ch, req);
		}
	}
invalid_opcode:
	SPDK_INFOLOG(nvmf, "Unsupported IO opcode 0x%x\n", cmd->opc);
	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
	response->status.dnr = 1;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static void
nvmf_qpair_request_cleanup(struct spdk_nvmf_qpair *qpair)
{
	if (spdk_unlikely(qpair->state == SPDK_NVMF_QPAIR_DEACTIVATING)) {
		assert(qpair->state_cb != NULL);

		if (TAILQ_EMPTY(&qpair->outstanding)) {
			qpair->state_cb(qpair->state_cb_arg, 0);
		}
	}
}

int
spdk_nvmf_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;

	TAILQ_REMOVE(&qpair->outstanding, req, link);
	nvmf_transport_req_free(req);

	nvmf_qpair_request_cleanup(qpair);

	return 0;
}

static void
_nvmf_request_complete(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_qpair *qpair;
	struct spdk_nvmf_subsystem_poll_group *sgroup = NULL;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;
	bool is_aer = false;
	uint32_t nsid;
	bool paused;
	uint8_t opcode;

	rsp->sqid = 0;
	rsp->status.p = 0;
	rsp->cid = req->cmd->nvme_cmd.cid;
	opcode = req->cmd->nvmf_cmd.opcode;
	qpair = req->qpair;

	/* request should not be on a ns reservations list */
	assert(req->reservation_queued == false);

	if (spdk_likely(qpair->ctrlr)) {
		sgroup = &qpair->group->sgroups[qpair->ctrlr->subsys->id];
		assert(sgroup != NULL);
		if (spdk_likely(qpair->qid != 0)) {
			qpair->group->stat.completed_nvme_io++;
		} else if (req->cmd->nvme_cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			is_aer = true;
		}

		/* If we changed nvme_cmd.nsid to match the passthrough nsid, we need to
		 * restore it here for accounting purposes.
		 */
		if (qpair->ctrlr->subsys->passthrough && req->orig_nsid) {
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
			rsp->status.crd = 1;
		}
	} else if (spdk_unlikely(nvmf_request_is_fabric_connect(req))) {
		sgroup = nvmf_subsystem_pg_from_connect_cmd(req);
	}

	nsid = req->cmd->nvme_cmd.nsid;

	/* Check if this IO is being waited on by a reservation commnd */
	if (spdk_unlikely(req->reservation_waiting)) {
		if (sgroup && (nsid - 1 < sgroup->num_ns)) {
			ns_info = &sgroup->ns_info[nsid - 1];
			if (ns_info->preempt_abort.io_waiting > 0) {
				ns_info->preempt_abort.io_waiting--;
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
		spdk_nvme_print_completion(qpair->qid, rsp);
	}

	switch (req->zcopy_phase) {
	case NVMF_ZCOPY_PHASE_NONE:
		TAILQ_REMOVE(&qpair->outstanding, req, link);
		break;
	case NVMF_ZCOPY_PHASE_INIT:
		if (spdk_unlikely(spdk_nvme_cpl_is_error(rsp))) {
			req->zcopy_phase = NVMF_ZCOPY_PHASE_INIT_FAILED;
			TAILQ_REMOVE(&qpair->outstanding, req, link);
		} else {
			req->zcopy_phase = NVMF_ZCOPY_PHASE_EXECUTE;
		}
		break;
	case NVMF_ZCOPY_PHASE_EXECUTE:
		break;
	case NVMF_ZCOPY_PHASE_END_PENDING:
		TAILQ_REMOVE(&qpair->outstanding, req, link);
		req->zcopy_phase = NVMF_ZCOPY_PHASE_COMPLETE;
		break;
	default:
		SPDK_ERRLOG("Invalid ZCOPY phase %u\n", req->zcopy_phase);
		break;
	}

	nvmf_transport_req_complete(req);

	/* AER cmd is an exception */
	if (spdk_likely(sgroup && !is_aer)) {
		if (spdk_unlikely(opcode == SPDK_NVME_OPC_FABRIC ||
				  nvmf_qpair_is_admin_queue(qpair))) {
			assert(sgroup->mgmt_io_outstanding > 0);
			sgroup->mgmt_io_outstanding--;
		} else {
			if (req->zcopy_phase == NVMF_ZCOPY_PHASE_NONE ||
			    req->zcopy_phase == NVMF_ZCOPY_PHASE_COMPLETE ||
			    req->zcopy_phase == NVMF_ZCOPY_PHASE_INIT_FAILED) {
				/* End of request */

				/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
				if (spdk_likely(nsid - 1 < sgroup->num_ns)) {
					assert(sgroup->ns_info[nsid - 1].io_outstanding != 0);
					sgroup->ns_info[nsid - 1].io_outstanding--;
				}
			}
		}

		if (spdk_unlikely(sgroup->state == SPDK_NVMF_SUBSYSTEM_PAUSING &&
				  sgroup->mgmt_io_outstanding == 0)) {
			paused = true;
			for (nsid = 0; nsid < sgroup->num_ns; nsid++) {
				ns_info = &sgroup->ns_info[nsid];

				if (ns_info->state == SPDK_NVMF_SUBSYSTEM_PAUSING &&
				    ns_info->io_outstanding > 0) {
					paused = false;
					break;
				}
			}

			if (paused) {
				sgroup->state = SPDK_NVMF_SUBSYSTEM_PAUSED;
				sgroup->cb_fn(sgroup->cb_arg, 0);
				sgroup->cb_fn = NULL;
				sgroup->cb_arg = NULL;
			}
		}

	}

	nvmf_qpair_request_cleanup(qpair);
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

static bool
nvmf_check_subsystem_active(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_subsystem_poll_group *sgroup = NULL;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;
	uint32_t nsid;

	if (spdk_likely(qpair->ctrlr)) {
		sgroup = &qpair->group->sgroups[qpair->ctrlr->subsys->id];
		assert(sgroup != NULL);
	} else if (spdk_unlikely(nvmf_request_is_fabric_connect(req))) {
		sgroup = nvmf_subsystem_pg_from_connect_cmd(req);
	}

	/* Check if the subsystem is paused (if there is a subsystem) */
	if (spdk_unlikely(sgroup == NULL)) {
		return true;
	}

	if (spdk_unlikely(req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC ||
			  nvmf_qpair_is_admin_queue(qpair))) {
		if (sgroup->state != SPDK_NVMF_SUBSYSTEM_ACTIVE) {
			/* The subsystem is not currently active. Queue this request. */
			TAILQ_INSERT_TAIL(&sgroup->queued, req, link);
			return false;
		}
		sgroup->mgmt_io_outstanding++;
	} else {
		nsid = req->cmd->nvme_cmd.nsid;

		/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
		if (spdk_unlikely(nsid - 1 >= sgroup->num_ns)) {
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
			req->rsp->nvme_cpl.status.dnr = 1;
			TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);
			_nvmf_request_complete(req);
			return false;
		}

		ns_info = &sgroup->ns_info[nsid - 1];
		if (spdk_unlikely(ns_info->channel == NULL)) {
			/* This can can happen if host sends I/O to a namespace that is
			 * in the process of being added, but before the full addition
			 * process is complete.  Report invalid namespace in that case.
			 */
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
			req->rsp->nvme_cpl.status.dnr = 1;
			TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);
			ns_info->io_outstanding++;
			_nvmf_request_complete(req);
			return false;
		}

		if (spdk_unlikely(ns_info->state != SPDK_NVMF_SUBSYSTEM_ACTIVE)) {
			/* The namespace is not currently active. Queue this request. */
			TAILQ_INSERT_TAIL(&sgroup->queued, req, link);
			return false;
		}

		ns_info->io_outstanding++;
	}

	return true;
}

static bool
nvmf_check_qpair_active(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	int sc, sct;

	if (spdk_likely(qpair->state == SPDK_NVMF_QPAIR_ENABLED)) {
		return true;
	}

	sct = SPDK_NVME_SCT_GENERIC;
	sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;

	switch (qpair->state) {
	case SPDK_NVMF_QPAIR_CONNECTING:
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
		return true;
	case SPDK_NVMF_QPAIR_AUTHENTICATING:
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVMF_FABRIC_SC_AUTH_REQUIRED;
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
		return true;
	default:
		SPDK_ERRLOG("Received command 0x%x on qid %u in state %d\n",
			    req->cmd->nvmf_cmd.opcode, qpair->qid, qpair->state);
		break;
	}

	req->rsp->nvme_cpl.status.sct = sct;
	req->rsp->nvme_cpl.status.sc = sc;
	TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);
	_nvmf_request_complete(req);

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
	struct spdk_nvmf_qpair *qpair = req->qpair;
	enum spdk_nvmf_request_exec_status status;

	if (spdk_unlikely(!nvmf_check_subsystem_active(req))) {
		return;
	}
	if (spdk_unlikely(!nvmf_check_qpair_active(req))) {
		return;
	}

	if (SPDK_DEBUGLOG_FLAG_ENABLED("nvmf")) {
		spdk_nvme_print_command(qpair->qid, &req->cmd->nvme_cmd);
	}

	/* Place the request on the outstanding list so we can keep track of it */
	TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);

	if (spdk_unlikely(req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC)) {
		status = nvmf_ctrlr_process_fabrics_cmd(req);
	} else if (spdk_unlikely(nvmf_qpair_is_admin_queue(qpair))) {
		status = nvmf_ctrlr_process_admin_cmd(req);
	} else {
		status = nvmf_ctrlr_process_io_cmd(req);
	}

	if (status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		_nvmf_request_complete(req);
	}
}

static bool
nvmf_ctrlr_get_dif_ctx(struct spdk_nvmf_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd,
		       struct spdk_dif_ctx *dif_ctx)
{
	struct spdk_nvmf_ns *ns;
	struct spdk_bdev_desc *desc;

	if (ctrlr == NULL || cmd == NULL) {
		return false;
	}

	ns = nvmf_ctrlr_get_ns(ctrlr, cmd->nsid);
	if (ns == NULL || ns->bdev == NULL) {
		return false;
	}

	desc = ns->desc;

	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_COMPARE:
		return nvmf_bdev_ctrlr_get_dif_ctx(desc, cmd, dif_ctx);
	default:
		break;
	}

	return false;
}

bool
spdk_nvmf_request_get_dif_ctx(struct spdk_nvmf_request *req, struct spdk_dif_ctx *dif_ctx)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;

	if (spdk_likely(ctrlr == NULL || !ctrlr->dif_insert_or_strip)) {
		return false;
	}

	if (spdk_unlikely(!spdk_nvmf_qpair_is_active(qpair))) {
		return false;
	}

	if (spdk_unlikely(req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC)) {
		return false;
	}

	if (spdk_unlikely(nvmf_qpair_is_admin_queue(qpair))) {
		return false;
	}

	return nvmf_ctrlr_get_dif_ctx(ctrlr, &req->cmd->nvme_cmd, dif_ctx);
}

void
spdk_nvmf_set_custom_admin_cmd_hdlr(uint8_t opc, spdk_nvmf_custom_cmd_hdlr hdlr)
{
	g_nvmf_custom_admin_cmd_hdlrs[opc].hdlr = hdlr;
}

static int
nvmf_passthru_admin_cmd_for_bdev_nsid(struct spdk_nvmf_request *req, uint32_t bdev_nsid)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvme_cpl *response = spdk_nvmf_request_get_response(req);
	int rc;

	rc = spdk_nvmf_request_get_bdev(bdev_nsid, req, &bdev, &desc, &ch);
	if (rc) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr = req->qpair->ctrlr;
	ns = nvmf_ctrlr_get_ns(ctrlr, bdev_nsid);

	if (ns->passthru_nsid) {
		req->cmd->nvme_cmd.nsid = ns->passthru_nsid;
	}

	return spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(bdev, desc, ch, req, NULL);
}

static int
nvmf_passthru_admin_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
	uint32_t bdev_nsid;

	if (g_nvmf_custom_admin_cmd_hdlrs[cmd->opc].nsid != 0) {
		bdev_nsid = g_nvmf_custom_admin_cmd_hdlrs[cmd->opc].nsid;
	} else {
		bdev_nsid = cmd->nsid;
	}

	return nvmf_passthru_admin_cmd_for_bdev_nsid(req, bdev_nsid);
}

int
nvmf_passthru_admin_cmd_for_ctrlr(struct spdk_nvmf_request *req, struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvme_cpl *response = spdk_nvmf_request_get_response(req);
	struct spdk_nvmf_ns *ns;

	ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys);
	if (ns == NULL) {
		/* Is there a better sc to use here? */
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return nvmf_passthru_admin_cmd_for_bdev_nsid(req, ns->nsid);
}

void
spdk_nvmf_set_passthru_admin_cmd(uint8_t opc, uint32_t forward_nsid)
{
	g_nvmf_custom_admin_cmd_hdlrs[opc].hdlr = nvmf_passthru_admin_cmd;
	g_nvmf_custom_admin_cmd_hdlrs[opc].nsid = forward_nsid;
}

int
spdk_nvmf_request_get_bdev(uint32_t nsid, struct spdk_nvmf_request *req,
			   struct spdk_bdev **bdev, struct spdk_bdev_desc **desc, struct spdk_io_channel **ch)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_poll_group *group = req->qpair->group;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;

	*bdev = NULL;
	*desc = NULL;
	*ch = NULL;

	ns = nvmf_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL || ns->bdev == NULL) {
		return -EINVAL;
	}

	assert(group != NULL && group->sgroups != NULL);
	ns_info = &group->sgroups[ctrlr->subsys->id].ns_info[nsid - 1];
	*bdev = ns->bdev;
	*desc = ns->desc;
	*ch = ns_info->channel;

	return 0;
}

struct spdk_nvmf_ctrlr *spdk_nvmf_request_get_ctrlr(struct spdk_nvmf_request *req)
{
	return req->qpair->ctrlr;
}

struct spdk_nvme_cmd *spdk_nvmf_request_get_cmd(struct spdk_nvmf_request *req)
{
	return &req->cmd->nvme_cmd;
}

struct spdk_nvme_cpl *spdk_nvmf_request_get_response(struct spdk_nvmf_request *req)
{
	return &req->rsp->nvme_cpl;
}

struct spdk_nvmf_subsystem *spdk_nvmf_request_get_subsystem(struct spdk_nvmf_request *req)
{
	return req->qpair->ctrlr->subsys;
}

size_t
spdk_nvmf_request_copy_from_buf(struct spdk_nvmf_request *req,
				void *buf, size_t buflen)
{
	struct spdk_iov_xfer ix;

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
	return spdk_iov_xfer_from_buf(&ix, buf, buflen);
}

size_t
spdk_nvmf_request_copy_to_buf(struct spdk_nvmf_request *req,
			      void *buf, size_t buflen)
{
	struct spdk_iov_xfer ix;

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
	return spdk_iov_xfer_to_buf(&ix, buf, buflen);
}

struct spdk_nvmf_subsystem *spdk_nvmf_ctrlr_get_subsystem(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->subsys;
}

uint16_t
spdk_nvmf_ctrlr_get_id(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->cntlid;
}

struct spdk_nvmf_request *spdk_nvmf_request_get_req_to_abort(struct spdk_nvmf_request *req)
{
	return req->req_to_abort;
}
