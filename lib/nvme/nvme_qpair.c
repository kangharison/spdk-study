/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] NVMe Queue Pair 제출/완료/상태머신 코어 (nvme_qpair.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK NVMe 드라이버의 **Queue Pair(SQ+CQ)** 추상 레이어 구현이다.
 * 상위(nvme_ns_cmd.c 등)가 빌드한 `struct nvme_request`를 받아 트랜스포트로
 * 디스패치하고, 반대로 트랜스포트가 CQE를 수확한 뒤 호출되는 완료 경로를
 * 관장한다. 즉 이 파일은 "트랜스포트 독립적 I/O 스케줄/상태머신"의 자리다.
 *
 * 네 가지 책임:
 *   1) **제출 경로 (hot path)**: `nvme_qpair_submit_request` ★ → `_nvme_qpair_submit_request`
 *      → split 요청이면 children 재귀 제출, 아니면 error-injection 체크 후
 *      `nvme_transport_qpair_submit_request(qpair, req)`로 트랜스포트에 위임.
 *   2) **완료 경로 (hot path)**: `spdk_nvme_qpair_process_completions` ★
 *      → 트랜스포트 `process_completions`를 호출 → 각 CQE마다 `nvme_complete_request`
 *      콜백 실행 → 완료한 개수만큼 queued_req 재제출 시도(`resubmit_requests`).
 *   3) **상태머신**: `nvme_qpair_check_enabled`가 DISCONNECTED → CONNECTING →
 *      CONNECTED → ENABLING → ENABLED 전이를 관장. reset 시 transport 레벨
 *      재연결 + queued_req flush/재제출을 수행.
 *   4) **수명주기·유틸·진단**: `nvme_qpair_init/deinit`, opcode·status 사전을
 *      이용한 디버그 프린터(`spdk_nvme_qpair_print_command/completion`),
 *      `error_injection`(테스트용 인공 에러 주입), getter 6종.
 *
 * `struct spdk_nvme_qpair`의 정의는 `lib/nvme/nvme_internal.h`에 있다
 *   (주요 필드: id, qprio, trtype, state, ctrlr, free_req, queued_req,
 *    aborting_queued_req, err_req_head, err_cmd_head, reserved_req,
 *    num_outstanding_reqs, in_completion_context, poll_group, ...).
 * 이 파일은 그 필드들을 읽고 갱신하면서 논리 흐름을 구현한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (제출 방향):
 *   [Application]
 *     → spdk_nvme_ns_cmd_read/write [lib/nvme/nvme_ns_cmd.c]
 *         → _nvme_ns_cmd_rw → nvme_request 빌드 + split 판정
 *         → nvme_qpair_submit_request(qpair, req) ★ [이 파일]
 *             → _nvme_qpair_submit_request(qpair, req)
 *                 → nvme_transport_qpair_submit_request() [lib/nvme/nvme_transport.c]
 *                     → nvme_pcie_qpair_submit_request() [lib/nvme/nvme_pcie_common.c]
 *                         → SQ[sq_tail] = SQE; doorbell MMIO write
 *                             → 장치가 SQE fetch → 수행 → CQE 기록 → phase 토글
 *
 * 호출 체인 (완료 방향):
 *   [Reactor poller]
 *     → spdk_nvme_qpair_process_completions(qpair, max) ★ [이 파일]
 *         → nvme_transport_qpair_process_completions() [lib/nvme/nvme_transport.c]
 *             → nvme_pcie_qpair_process_completions() [lib/nvme/nvme_pcie_common.c]
 *                 → CQ[cq_head].phase 검사 → CQE 수확
 *                 → tracker로 nvme_request 복원 → nvme_complete_request(cb_fn, ...)
 *                     → req->cb_fn(cb_arg, cpl)  ← 사용자 콜백
 *         → resubmit_requests() : 방금 비어난 slot만큼 queued_req에서 pop하여 재제출
 *
 * 실행 컨텍스트:
 *   - 이 파일의 모든 핫패스 함수는 **qpair를 소유한 단일 SPDK thread(코어)에서만 호출**.
 *     qpair.num_outstanding_reqs / free_req / queued_req 는 lock 없음(thread affinity).
 *   - Admin queue (qid=0)만 예외적으로 ctrlr_lock 아래 접근 — 다중 프로세스 공유.
 *   - `process_completions`는 reactor poller로서 무한 호출됨 (polled-mode).
 *     완료 콜백 내에서 새 I/O를 제출하는 것이 허용된다(가장 일반적 재귀 패턴).
 *
 * === 타 모듈과의 연결 ===
 *   - `nvme_ns_cmd.c` → submit 경로의 진입자. 이 파일의 `nvme_qpair_submit_request`만 호출.
 *   - `nvme_ctrlr.c`  → reset/connect/disconnect 시 이 파일의
 *                       `nvme_qpair_abort_all_queued_reqs` / `nvme_qpair_resubmit_requests` 호출.
 *   - `nvme_transport.c` → 트랜스포트 vtable 디스패치. 이 파일의 `nvme_transport_*` 호출이 모두 여기로 감.
 *   - `nvme_pcie_common.c` / `nvme_pcie.c` / `nvme_rdma.c` / `nvme_tcp.c` → 실제 트랜스포트 구현.
 *                    이 파일은 트랜스포트 구현을 모른 채 인터페이스로만 통신.
 *   - `nvme_poll_group.c` → I/O qpair들을 묶어 한 번에 폴링하는 상위 그룹.
 *   - `nvme_internal.h` → struct spdk_nvme_qpair, nvme_request, nvme_error_cmd,
 *                         nvme_qpair_get_state/set_state 등 모든 내부 API.
 *
 * 공유 자료구조 (nvme_internal.h 참고):
 *   - `qpair->free_req`            : 재사용 가능한 nvme_request LIFO 풀 (STAILQ)
 *   - `qpair->queued_req`          : NOMEM/EAGAIN으로 제출 대기 중인 request들 (FIFO)
 *   - `qpair->aborting_queued_req` : abort 대상으로 격리된 request들 (무한 재귀 방지용)
 *   - `qpair->err_req_head`        : error injection으로 "제출되지 않고 완료 큐에 갇힌" request들
 *   - `qpair->err_cmd_head`        : opcode→에러 주입 매핑 테이블
 *   - `qpair->reserved_req`        : 예비 1개 — fabrics CONNECT 등 크리티컬 경로에서 free_req가
 *                                    비어도 확보 가능하도록 따로 둠
 *   - `qpair->num_outstanding_reqs`: 장치에 제출되었으나 아직 미완료된 개수 (QD 추적)
 *
 * === 주요 함수/구조체 요약 ===
 *   ★ spdk_nvme_qpair_process_completions  — reactor poller. 트랜스포트 CQ 드레인 + 후속 재제출.
 *   ★ nvme_qpair_submit_request            — 외부 진입. queued_req가 있으면 뒤에 붙이고, 없으면 즉시 제출.
 *   ★ _nvme_qpair_submit_request           — 실제 제출 로직. split 처리 + err_cmd 주입 + transport dispatch.
 *   ★ nvme_qpair_check_enabled             — reset 경계의 상태머신 전이 + queued_req flush.
 *     nvme_qpair_resubmit_requests         — 완료 개수만큼 queued_req에서 pop 재제출.
 *     nvme_qpair_abort_queued_reqs         — queued_req를 SQ deletion abort로 일괄 완료.
 *     nvme_qpair_manual_complete_request   — "트랜스포트 경유 없이" SCT/SC를 만들어 호스트 콜백 실행.
 *     nvme_qpair_init / nvme_qpair_deinit  — req_buf 풀 할당/해제 + 연결 리스트 초기화/정리.
 *     nvme_completion_is_retry             — CQE status로 재시도 가능 여부 판단 (DNR 고려).
 *     spdk_nvme_qpair_print_command/completion — SQE/CQE 내용을 사람이 읽을 수 있는 문자열로.
 *     spdk_nvme_qpair_add/remove_cmd_error_injection — 테스트용 opcode별 인공 에러 주입.
 *
 * 관련 상수:
 *   NVME_CMD_DPTR_STR_SIZE(256), NVME_CMD_STR_SIZE(1024) — 디버그 문자열 버퍼 크기.
 *
 * 이 파일은 I/O hot-path의 심장이므로, 락 없이 순전히 thread affinity + STAILQ 조작만으로
 * 동작한다. 즉 "qpair에 접근하는 코드는 qpair 소유 스레드뿐"이라는 전제를 깨지 않는 한,
 * 데이터 경쟁은 구조적으로 발생하지 않는다.
 */

#include "nvme_internal.h"       /* [한국어] 내부 타입 — spdk_nvme_qpair/ctrlr, nvme_request, nvme_error_cmd,
                                  *         nvme_qpair_get_state/set_state, nvme_complete_request,
                                  *         nvme_transport_* 프로토타입, 모든 내부 enum 등 이 파일이 사용하는 거의 전부 */
#include "spdk/nvme_ocssd.h"     /* [한국어] Open-Channel SSD(OCSSD) opcode/status 상수 — 디버그 프린터
                                  *         (admin/io opcode 테이블과 media error 테이블)에서 사용 */
#include "spdk/string.h"         /* [한국어] spdk_strerror — process_completions의 transport error 로그에서 사용
                                  *         (-errno를 "No such device" 같은 문자열로 변환) */

#define NVME_CMD_DPTR_STR_SIZE 256
                                  /* [한국어] DPTR(Data Pointer) 프린트용 소(小) 버퍼 크기.
                                   *         "PRP1 0x... PRP2 0x..." 또는 "SGL DATA BLOCK ADDRESS 0x... len:0x... key:0x..." 정도로
                                   *         256B면 충분. snprintf에서 assert로 오버플로우 방지. */
#define NVME_CMD_STR_SIZE 1024
                                  /* [한국어] 전체 command/completion 문자열 버퍼 크기.
                                   *         opcode명 + cid + nsid + LBA + len + DPTR 포함 가능해야 하므로 1KB. */

static int nvme_qpair_resubmit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req);
                                  /* [한국어] 전방 선언. resubmit은 이 파일 내부에서만 쓰이는 static이지만
                                   *         check_enabled / resubmit_requests가 자기 정의 이전에 호출하므로
                                   *         선언부를 파일 최상단에 둔다. 완료 슬롯이 비어나면 queued_req의 선두를
                                   *         pop해 _nvme_qpair_submit_request로 다시 내려보낸다. */

struct nvme_string {
                                  /* [한국어] {enum 값, 사람이 읽는 문자열} 매핑 테이블 엔트리.
                                   *         opcode/status code/SGL type 등 모든 디버그 카테고리에서 재사용.
                                   *         각 테이블은 sentinel {0xFFFF, "…"}로 끝나며, nvme_get_string()이
                                   *         선형 검색 후 sentinel을 기본값으로 반환. */
	uint16_t	value;
                                  /* [한국어] NVMe 스펙상의 opcode/SCT/SC 등 수치값 (16비트면 모두 수용).
                                   *         sentinel은 0xFFFF. */
	const char	*str;
                                  /* [한국어] 해당 값의 사람이 읽는 이름. SPDK_NOTICELOG 등에 찍힘. */
};

/*
 * [한국어] ★ 디버그 프린트용 문자열 사전들 ★
 *
 * 다음 테이블들은 NVMe 스펙 수치값 → 사람이 읽는 문자열 변환을 위한 정적 사전이다.
 * SPDK_DEBUGLOG_FLAG_ENABLED("nvme")가 켜졌을 때 또는 CQE가 에러로 돌아왔을 때
 * `spdk_nvme_qpair_print_command/completion`이 이 사전들을 참조해 로그에 남긴다.
 *
 * 테이블 종류:
 *   admin_opcode          : Admin 큐 opcode (Delete/Create IO SQ/CQ, IDENTIFY, ABORT, SET/GET FEATURES ...)
 *   fabric_opcode         : NVMe-oF Fabrics capsule opcode (PROPERTY SET/GET, CONNECT, AUTH SEND/RECV)
 *   feat_opcode           : SET/GET FEATURES의 Feature Identifier (FID) — 파워, 큐 수, 인터럽트 ...
 *   io_opcode             : I/O 큐 opcode (READ, WRITE, FLUSH, DSM, COMPARE, WRITE ZEROES, RESERVATION ...)
 *   sgl_type / sgl_subtype: SGL descriptor의 type/subtype nibble — DPTR 프린트 시 사용
 *   status_type           : CQE status의 SCT(Status Code Type) — Generic/CmdSpecific/MediaError/Path/VendorSpec
 *   generic_status / command_specific_status / media_error_status / path_status
 *                         : SCT별 SC(Status Code) 테이블. spdk_nvme_cpl_get_status_string이 SCT로 분기 후 SC 검색.
 *
 * 공통 규약:
 *   - 각 배열은 sentinel `{ 0xFFFF, "<기본 이름>" }`로 종결.
 *   - `nvme_get_string(table, value)`가 선형 검색하고, 찾지 못하면 sentinel의 문자열 반환.
 *   - 배열이 `static const`이므로 컴파일러가 `.rodata`에 배치해 프로세스간 공유 가능.
 */

static const struct nvme_string admin_opcode[] = {
                                  /* [한국어] Admin Submission Queue(qid=0)용 opcode 이름 테이블.
                                   *         NVMe Base Spec의 Admin Command Set(주로 Fig.139 부근) 매핑. */
	{ SPDK_NVME_OPC_DELETE_IO_SQ, "DELETE IO SQ" },
	{ SPDK_NVME_OPC_CREATE_IO_SQ, "CREATE IO SQ" },
	{ SPDK_NVME_OPC_GET_LOG_PAGE, "GET LOG PAGE" },
	{ SPDK_NVME_OPC_DELETE_IO_CQ, "DELETE IO CQ" },
	{ SPDK_NVME_OPC_CREATE_IO_CQ, "CREATE IO CQ" },
	{ SPDK_NVME_OPC_IDENTIFY, "IDENTIFY" },
	{ SPDK_NVME_OPC_ABORT, "ABORT" },
	{ SPDK_NVME_OPC_SET_FEATURES, "SET FEATURES" },
	{ SPDK_NVME_OPC_GET_FEATURES, "GET FEATURES" },
	{ SPDK_NVME_OPC_ASYNC_EVENT_REQUEST, "ASYNC EVENT REQUEST" },
	{ SPDK_NVME_OPC_NS_MANAGEMENT, "NAMESPACE MANAGEMENT" },
	{ SPDK_NVME_OPC_FIRMWARE_COMMIT, "FIRMWARE COMMIT" },
	{ SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD, "FIRMWARE IMAGE DOWNLOAD" },
	{ SPDK_NVME_OPC_DEVICE_SELF_TEST, "DEVICE SELF-TEST" },
	{ SPDK_NVME_OPC_NS_ATTACHMENT, "NAMESPACE ATTACHMENT" },
	{ SPDK_NVME_OPC_KEEP_ALIVE, "KEEP ALIVE" },
	{ SPDK_NVME_OPC_DIRECTIVE_SEND, "DIRECTIVE SEND" },
	{ SPDK_NVME_OPC_DIRECTIVE_RECEIVE, "DIRECTIVE RECEIVE" },
	{ SPDK_NVME_OPC_VIRTUALIZATION_MANAGEMENT, "VIRTUALIZATION MANAGEMENT" },
	{ SPDK_NVME_OPC_NVME_MI_SEND, "NVME-MI SEND" },
	{ SPDK_NVME_OPC_NVME_MI_RECEIVE, "NVME-MI RECEIVE" },
	{ SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG, "DOORBELL BUFFER CONFIG" },
	{ SPDK_NVME_OPC_FABRIC, "FABRIC" },
	{ SPDK_NVME_OPC_FORMAT_NVM, "FORMAT NVM" },
	{ SPDK_NVME_OPC_SECURITY_SEND, "SECURITY SEND" },
	{ SPDK_NVME_OPC_SECURITY_RECEIVE, "SECURITY RECEIVE" },
	{ SPDK_NVME_OPC_SANITIZE, "SANITIZE" },
	{ SPDK_NVME_OPC_GET_LBA_STATUS, "GET LBA STATUS" },
	{ SPDK_OCSSD_OPC_GEOMETRY, "OCSSD / GEOMETRY" },
                                  /* [한국어] OCSSD 확장 opcode (Open-Channel SSD용 GEOMETRY). nvme_ocssd.h에서 정의. */
	{ 0xFFFF, "ADMIN COMMAND" }
                                  /* [한국어] sentinel — 표에 없는 opcode는 "ADMIN COMMAND"로 기본 처리 */
};

static const struct nvme_string fabric_opcode[] = {
                                  /* [한국어] Fabrics capsule 내부 fctype 값 테이블.
                                   *         NVMe-oF에서 Admin opcode가 SPDK_NVME_OPC_FABRIC(0x7F)인 경우
                                   *         실제 의미는 fctype에 있으므로 별도 사전 필요. */
	{ SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET, "PROPERTY SET" },
	{ SPDK_NVMF_FABRIC_COMMAND_CONNECT, "CONNECT" },
	{ SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET, "PROPERTY GET" },
	{ SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND, "AUTHENTICATION SEND" },
	{ SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV, "AUTHENTICATION RECV" },
	{ 0xFFFF, "RESERVED / VENDOR SPECIFIC" }
                                  /* [한국어] sentinel — 알 수 없는 fctype 기본값 */
};

static const struct nvme_string feat_opcode[] = {
                                  /* [한국어] SET/GET FEATURES의 FID(Feature Identifier) 테이블.
                                   *         nvme_get_admin_qpair_command_string이 SET/GET FEATURES일 때
                                   *         CDW10[FID]를 이름으로 변환해 로그에 표시. */
	{ SPDK_NVME_FEAT_ARBITRATION, "ARBITRATION" },
	{ SPDK_NVME_FEAT_POWER_MANAGEMENT, "POWER MANAGEMENT" },
	{ SPDK_NVME_FEAT_LBA_RANGE_TYPE, "LBA RANGE TYPE" },
	{ SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD, "TEMPERATURE THRESHOLD" },
	{ SPDK_NVME_FEAT_ERROR_RECOVERY, "ERROR_RECOVERY" },
	{ SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE, "VOLATILE WRITE CACHE" },
	{ SPDK_NVME_FEAT_NUMBER_OF_QUEUES, "NUMBER OF QUEUES" },
	{ SPDK_NVME_FEAT_INTERRUPT_COALESCING, "INTERRUPT COALESCING" },
	{ SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION, "INTERRUPT VECTOR CONFIGURATION" },
	{ SPDK_NVME_FEAT_WRITE_ATOMICITY, "WRITE ATOMICITY" },
	{ SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION, "ASYNC EVENT CONFIGURATION" },
	{ SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION, "AUTONOMOUS POWER STATE TRANSITION" },
	{ SPDK_NVME_FEAT_HOST_MEM_BUFFER, "HOST MEM BUFFER" },
	{ SPDK_NVME_FEAT_TIMESTAMP, "TIMESTAMP" },
	{ SPDK_NVME_FEAT_KEEP_ALIVE_TIMER, "KEEP ALIVE TIMER" },
	{ SPDK_NVME_FEAT_HOST_CONTROLLED_THERMAL_MANAGEMENT, "HOST CONTROLLED THERMAL MANAGEMENT" },
	{ SPDK_NVME_FEAT_NON_OPERATIONAL_POWER_STATE_CONFIG, "NON OPERATIONAL POWER STATE CONFIG" },
	{ SPDK_NVME_FEAT_SOFTWARE_PROGRESS_MARKER, "SOFTWARE PROGRESS MARKER" },
	{ SPDK_NVME_FEAT_HOST_IDENTIFIER, "HOST IDENTIFIER" },
	{ SPDK_NVME_FEAT_HOST_RESERVE_MASK, "HOST RESERVE MASK" },
	{ SPDK_NVME_FEAT_HOST_RESERVE_PERSIST, "HOST RESERVE PERSIST" },
	{ 0xFFFF, "RESERVED" }
                                  /* [한국어] sentinel */
};

static const struct nvme_string io_opcode[] = {
                                  /* [한국어] I/O 큐(qid≥1)용 opcode 테이블.
                                   *         NVM Command Set(Base Spec Fig.346)의 NVM opcode + OCSSD 벡터 확장. */
	{ SPDK_NVME_OPC_FLUSH, "FLUSH" },
	{ SPDK_NVME_OPC_WRITE, "WRITE" },
	{ SPDK_NVME_OPC_READ, "READ" },
	{ SPDK_NVME_OPC_WRITE_UNCORRECTABLE, "WRITE UNCORRECTABLE" },
	{ SPDK_NVME_OPC_COMPARE, "COMPARE" },
	{ SPDK_NVME_OPC_WRITE_ZEROES, "WRITE ZEROES" },
	{ SPDK_NVME_OPC_DATASET_MANAGEMENT, "DATASET MANAGEMENT" },
	{ SPDK_NVME_OPC_RESERVATION_REGISTER, "RESERVATION REGISTER" },
	{ SPDK_NVME_OPC_RESERVATION_REPORT, "RESERVATION REPORT" },
	{ SPDK_NVME_OPC_RESERVATION_ACQUIRE, "RESERVATION ACQUIRE" },
	{ SPDK_NVME_OPC_RESERVATION_RELEASE, "RESERVATION RELEASE" },
	{ SPDK_OCSSD_OPC_VECTOR_RESET, "OCSSD / VECTOR RESET" },
	{ SPDK_OCSSD_OPC_VECTOR_WRITE, "OCSSD / VECTOR WRITE" },
	{ SPDK_OCSSD_OPC_VECTOR_READ, "OCSSD / VECTOR READ" },
	{ SPDK_OCSSD_OPC_VECTOR_COPY, "OCSSD / VECTOR COPY" },
	{ 0xFFFF, "IO COMMAND" }
                                  /* [한국어] sentinel — 알 수 없는 I/O opcode 기본값 */
};

static const struct nvme_string sgl_type[] = {
                                  /* [한국어] SGL descriptor의 type 필드(상위 4비트) 이름. nvme_spec.h 정의. */
	{ SPDK_NVME_SGL_TYPE_DATA_BLOCK, "DATA BLOCK" },
	{ SPDK_NVME_SGL_TYPE_BIT_BUCKET, "BIT BUCKET" },
	{ SPDK_NVME_SGL_TYPE_SEGMENT, "SEGMENT" },
	{ SPDK_NVME_SGL_TYPE_LAST_SEGMENT, "LAST SEGMENT" },
	{ SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK, "KEYED DATA BLOCK" },
	{ SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK, "TRANSPORT DATA BLOCK" },
	{ SPDK_NVME_SGL_TYPE_VENDOR_SPECIFIC, "VENDOR SPECIFIC" },
	{ 0xFFFF, "RESERVED" }
                                  /* [한국어] sentinel */
};

static const struct nvme_string sgl_subtype[] = {
                                  /* [한국어] SGL descriptor subtype(하위 4비트) 이름.
                                   *         ADDRESS(직접 주소), OFFSET(세그먼트 내 상대), TRANSPORT(트랜스포트 특화),
                                   *         INVALIDATE_KEY(RDMA 키 무효화) 등. */
	{ SPDK_NVME_SGL_SUBTYPE_ADDRESS, "ADDRESS" },
	{ SPDK_NVME_SGL_SUBTYPE_OFFSET, "OFFSET" },
	{ SPDK_NVME_SGL_SUBTYPE_TRANSPORT, "TRANSPORT" },
	{ SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY, "INVALIDATE KEY" },
	{ 0xFFFF, "RESERVED" }
                                  /* [한국어] sentinel */
};

/*
 * [한국어] nvme_get_string - value→str 선형 검색
 *
 * @strings: nvme_string 배열. 반드시 {0xFFFF, "…"} sentinel로 종결되어야 함.
 * @value:   찾을 수치 (opcode, SC, SGL type 등)
 * @return:  매칭되는 엔트리의 str, 미매칭 시 sentinel의 str
 *
 * 배열 크기가 작아(대부분 ≤50) 선형 검색으로 충분. hot-path가 아니라
 * 디버그 프린트에서만 호출되므로 최적화 대상 아님.
 */
static const char *
nvme_get_string(const struct nvme_string *strings, uint16_t value)
{
	const struct nvme_string *entry;
                                  /* [한국어] 이동용 반복자. entry++로 테이블을 순회. */

	entry = strings;
                                  /* [한국어] 선두부터 시작 */

	while (entry->value != 0xFFFF) {
                                  /* [한국어] sentinel까지 순회. 정상 매칭이면 루프 중간에 return. */
		if (entry->value == value) {
                                  /* [한국어] 찾음 */
			return entry->str;
		}
		entry++;
                                  /* [한국어] 다음 엔트리로 */
	}
	return entry->str;
                                  /* [한국어] sentinel의 기본 문자열 ("RESERVED" / "IO COMMAND" 등) 반환 */
}

/*
 * [한국어] nvme_get_sgl_unkeyed_string - PCIe 등 non-keyed SGL descriptor의 길이만 포맷.
 *
 * unkeyed 타입(SPDK_NVME_SGL_TYPE_DATA_BLOCK)은 key 필드가 없으므로 length만 찍는다.
 * PCIe 로컬 DMA에서 주로 사용. 호출자는 nvme_get_sgl_string.
 */
static void
nvme_get_sgl_unkeyed_string(char *buf, size_t size, struct spdk_nvme_cmd *cmd)
{
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;
                                  /* [한국어] cmd의 DPTR union에서 sgl1(첫 번째 SGL descriptor) 위치
                                   *         — 64바이트 SQE 내부 오프셋 24부터 16바이트 */

	snprintf(buf, size, " len:0x%x", sgl->unkeyed.length);
                                  /* [한국어] " len:0x<길이>" 포맷으로 이어붙임. 앞선 문자열 뒤에 append 용도. */
}

/*
 * [한국어] nvme_get_sgl_keyed_string - RDMA keyed SGL의 length+key를 포맷.
 *
 * NVMe-oF RDMA(RoCE/iWARP)는 length와 함께 remote memory key(rkey)를 전달한다.
 * 타겟이 이 키로 호스트 메모리에 직접 RDMA read/write 하기 때문에, 디버그 시
 * key를 찍어 두면 RDMA transport 이슈 트러블슈팅에 유용하다.
 */
static void
nvme_get_sgl_keyed_string(char *buf, size_t size, struct spdk_nvme_cmd *cmd)
{
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;
                                  /* [한국어] DPTR 내 첫 SGL 위치 */

	snprintf(buf, size, " len:0x%x key:0x%x", sgl->keyed.length, sgl->keyed.key);
                                  /* [한국어] " len:0x<L> key:0x<K>" — 24비트 length + 32비트 key */
}

/*
 * [한국어] nvme_get_sgl_string - SGL descriptor 전체를 type/subtype/address/length(+key) 문자열로.
 *
 * 결과 예: "SGL DATA BLOCK ADDRESS 0x7fabc00000 len:0x1000"
 *         "SGL KEYED DATA BLOCK ADDRESS 0xfeedface len:0x1000 key:0xdeadbeef"
 *
 * 내부적으로 type에 따라 unkeyed/keyed 길이 포맷을 분기하여 이어붙인다.
 */
static void
nvme_get_sgl_string(char *buf, size_t size, struct spdk_nvme_cmd *cmd)
{
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;
                                  /* [한국어] DPTR 내 sgl1 */
	int c;
                                  /* [한국어] 지금까지 snprintf된 바이트 수 — 남은 버퍼에 이어붙이기 위해 보관 */

	c = snprintf(buf, size, "SGL %s %s 0x%" PRIx64, nvme_get_string(sgl_type, sgl->generic.type),
		     nvme_get_string(sgl_subtype, sgl->generic.subtype), sgl->address);
                                  /* [한국어] 기본 꼴 "SGL <type> <subtype> 0x<64비트 주소>" 출력.
                                   *         type/subtype은 사전 테이블로 디코딩. */
	assert(c >= 0 && (size_t)c < size);
                                  /* [한국어] snprintf 성공 + size 미초과 (항상 만족 — DPTR 버퍼 256B면 충분) */

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK) {
                                  /* [한국어] 일반 데이터 블록(PCIe 로컬 DMA) — length만 추가 */
		nvme_get_sgl_unkeyed_string(buf + c, size - c, cmd);
	}

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK) {
                                  /* [한국어] RDMA keyed 블록 — length + key 추가 */
		nvme_get_sgl_keyed_string(buf + c, size - c, cmd);
	}
}

/*
 * [한국어] nvme_get_prp_string - PRP(Physical Region Page) DPTR을 "PRP1 0x.. PRP2 0x.."로 포맷.
 *
 * PRP는 SGL의 구형 대안 — 각 엔트리는 64비트 물리 주소.
 *   - PRP1: 첫 번째 페이지. 4KB 경계 불필요 (offset 허용).
 *   - PRP2: transfer가 8KB 이하면 두 번째 페이지, 이상이면 PRP list의 물리 주소.
 */
static void
nvme_get_prp_string(char *buf, size_t size, struct spdk_nvme_cmd *cmd)
{
	snprintf(buf, size, "PRP1 0x%" PRIx64 " PRP2 0x%" PRIx64, cmd->dptr.prp.prp1, cmd->dptr.prp.prp2);
                                  /* [한국어] cmd->dptr.prp.{prp1,prp2} 64비트 주소 2개를 그대로 표시 */
}

/*
 * [한국어] nvme_get_dptr_string - PSDT(PRP vs SGL) 분기해서 DPTR 내용을 포맷.
 *
 * PSDT(Payload/SGL Data Type) 필드로 현재 명령이 PRP 모드인지 SGL 모드인지 판별.
 * 데이터 전송이 없는 명령(FLUSH 등)은 DPTR 의미 없으므로 포맷 생략.
 */
static void
nvme_get_dptr_string(char *buf, size_t size, struct spdk_nvme_cmd *cmd)
{
	if (spdk_nvme_opc_get_data_transfer(cmd->opc) != SPDK_NVME_DATA_NONE) {
                                  /* [한국어] opcode의 data transfer 방향(HtoC/CtoH/Bi/None) 조회 — None이면 DPTR 없음 */
		switch (cmd->psdt) {
                                  /* [한국어] Payload Submission Data type — PRP/SGL 모드 선택 */
		case SPDK_NVME_PSDT_PRP:
                                  /* [한국어] 0x0: PRP 모드 */
			nvme_get_prp_string(buf, size, cmd);
			break;
		case SPDK_NVME_PSDT_SGL_MPTR_CONTIG:
                                  /* [한국어] 0x1: SGL 데이터 + CONTIG metadata */
		case SPDK_NVME_PSDT_SGL_MPTR_SGL:
                                  /* [한국어] 0x2: SGL 데이터 + SGL metadata */
			nvme_get_sgl_string(buf, size, cmd);
			break;
		default:
                                  /* [한국어] Reserved 값 — 아무것도 안 함 (버퍼에 빈 문자열 유지) */
			;
		}
	}
}

/*
 * [한국어] nvme_get_admin_qpair_command_string - Admin 명령 SQE를 디버그 문자열로 포맷.
 *
 * Admin 큐(qid=0)에 제출되는 SQE의 주요 필드(opcode, cid, nsid, cdw10/11, DPTR)를
 * 사람이 읽을 수 있는 형태로 buf에 기록한다. FABRIC과 SET/GET FEATURES는 추가 컨텍스트
 * (fctype, FID)가 의미 있어 별도 포맷 분기를 둔다.
 */
static void
nvme_get_admin_qpair_command_string(char *buf, size_t size, uint16_t qid, struct spdk_nvme_cmd *cmd)
{
	struct spdk_nvmf_capsule_cmd *fcmd = (void *)cmd;
                                  /* [한국어] FABRIC 명령은 SQE를 nvmf capsule 레이아웃(fctype 포함)으로 재해석.
                                   *         같은 64바이트 영역을 union으로 해석하는 관행. */
	char dptr[NVME_CMD_DPTR_STR_SIZE] = {'\0'};
                                  /* [한국어] DPTR 전용 임시 버퍼 (256B) — 별도 포맷 후 최종 문자열에 이어붙임 */

	assert(cmd != NULL);
                                  /* [한국어] cmd NULL 방어 */

	nvme_get_dptr_string(dptr, sizeof(dptr), cmd);
                                  /* [한국어] PRP or SGL 문자열을 dptr 버퍼에 미리 채움 (data_transfer 없으면 빈 문자열) */

	switch ((int)cmd->opc) {
                                  /* [한국어] opcode별 분기 — FEATURES와 FABRIC만 특별 취급 */
	case SPDK_NVME_OPC_SET_FEATURES:
	case SPDK_NVME_OPC_GET_FEATURES:
                                  /* [한국어] SET/GET FEATURES: CDW10[FID]가 실제 의미라서 feat_opcode 사전으로 디코딩 */
		snprintf(buf, size,
			 "%s %s cid:%" PRIu16 " cdw10:%08" PRIx32 " %s",
			 nvme_get_string(admin_opcode, cmd->opc), nvme_get_string(feat_opcode,
					 cmd->cdw10_bits.set_features.fid), cmd->cid, cmd->cdw10, dptr);
                                  /* [한국어] 예: "SET FEATURES NUMBER OF QUEUES cid:42 cdw10:00000007 PRP1 0x... PRP2 0x0" */
		break;
	case SPDK_NVME_OPC_FABRIC:
                                  /* [한국어] FABRIC capsule: fctype이 진짜 의미 (CONNECT/PROPERTY SET 등) */
		snprintf(buf, size,
			 "%s %s qid:%" PRIu16 " cid:%" PRIu16 " %s",
			 nvme_get_string(admin_opcode, cmd->opc), nvme_get_string(fabric_opcode, fcmd->fctype), qid,
			 fcmd->cid, dptr);
                                  /* [한국어] 예: "FABRIC CONNECT qid:0 cid:10 SGL ..." */
		break;
	default:
                                  /* [한국어] 그 외 일반 Admin opcode — CDW10/11을 hex로 그대로 노출 */
		snprintf(buf, size,
			 "%s (%02" PRIu8 ") qid:%" PRIu16 " cid:%" PRIu16 " nsid:%" PRIu32 " cdw10:%08" PRIx32 " cdw11:%08"
			 PRIx32 " %s",
			 nvme_get_string(admin_opcode, cmd->opc), cmd->opc, qid, cmd->cid, cmd->nsid, cmd->cdw10, cmd->cdw11,
			 dptr);
                                  /* [한국어] 예: "IDENTIFY (06) qid:0 cid:12 nsid:1 cdw10:00000001 cdw11:00000000 PRP1 0x..." */
	}
}

/*
 * [한국어] nvme_admin_qpair_print_command - Admin 명령을 SPDK_NOTICELOG로 출력하는 래퍼.
 *
 * 트레이스/디버그 용도. 프로덕션에서는 SPDK_DEBUGLOG_FLAG_ENABLED("nvme")가 켜진 경우에만 경로가 활성화.
 */
static void
nvme_admin_qpair_print_command(uint16_t qid, struct spdk_nvme_cmd *cmd)
{
	char buf[NVME_CMD_STR_SIZE] = {'\0'};
                                  /* [한국어] 최종 문자열 버퍼 (1KB). 스택 할당이라 재진입·스레드 안전. */

	nvme_get_admin_qpair_command_string(buf, sizeof(buf), qid, cmd);
                                  /* [한국어] 문자열 포맷 */
	SPDK_NOTICELOG("%s\n", buf);
                                  /* [한국어] SPDK 로그 인프라로 출력. 실제로는 stderr or syslog. */
}

/*
 * [한국어] nvme_get_io_qpair_command_string - I/O 명령 SQE를 디버그 문자열로 포맷.
 *
 * I/O 큐(qid≥1)에 가는 명령. READ/WRITE/COMPARE처럼 LBA/NLB가 있는 명령은
 * SLBA(CDW10+11)와 NLB(CDW12 하위 16비트 + 1)를 해석해서 사용자 친화적으로 표시.
 */
static void
nvme_get_io_qpair_command_string(char *buf, size_t size, uint16_t qid, struct spdk_nvme_cmd *cmd)
{
	char dptr[NVME_CMD_DPTR_STR_SIZE] = {'\0'};
                                  /* [한국어] DPTR 문자열 임시 버퍼 */

	assert(cmd != NULL);

	nvme_get_dptr_string(dptr, sizeof(dptr), cmd);
                                  /* [한국어] DPTR (PRP or SGL) 프리포맷 */

	switch ((int)cmd->opc) {
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
	case SPDK_NVME_OPC_COMPARE:
                                  /* [한국어] LBA+length가 있는 명령 그룹 — 상세 포맷 */
		snprintf(buf, size,
			 "%s sqid:%" PRIu16 " cid:%" PRIu16 " nsid:%" PRIu32 " lba:%" PRIu64 " len:%" PRIu32 " %s",
			 nvme_get_string(io_opcode, cmd->opc), qid, cmd->cid, cmd->nsid,
			 ((uint64_t)cmd->cdw11 << 32) + cmd->cdw10, (cmd->cdw12 & 0xFFFF) + 1, dptr);
                                  /* [한국어] lba = (cdw11 << 32) | cdw10 — 64비트 SLBA 재조립
                                   *         len = (cdw12 & 0xFFFF) + 1 — NLB 0-based를 1-based로 변환 */
		break;
	case SPDK_NVME_OPC_FLUSH:
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
                                  /* [한국어] LBA 없는 명령 — opcode와 기본 식별자만 */
		snprintf(buf, size,
			 "%s sqid:%" PRIu16 " cid:%" PRIu16 " nsid:%" PRIu32,
			 nvme_get_string(io_opcode, cmd->opc), qid, cmd->cid, cmd->nsid);
		break;
	default:
                                  /* [한국어] 그 외 I/O opcode — 최소 정보만 */
		snprintf(buf, size,
			 "%s (%02" PRIx8 ") sqid:%" PRIu16 " cid:%" PRIu16 " nsid:%" PRIu32,
			 nvme_get_string(io_opcode, cmd->opc), cmd->opc, qid, cmd->cid, cmd->nsid);
		break;
	}
}

/*
 * [한국어] nvme_io_qpair_print_command - I/O 명령 로그 출력 래퍼 (SPDK_NOTICELOG).
 */
static void
nvme_io_qpair_print_command(uint16_t qid, struct spdk_nvme_cmd *cmd)
{
	char buf[NVME_CMD_STR_SIZE] = {'\0'};
                                  /* [한국어] 1KB 스택 버퍼 */

	nvme_get_io_qpair_command_string(buf, sizeof(buf), qid, cmd);
                                  /* [한국어] 문자열 생성 */
	SPDK_NOTICELOG("%s\n", buf);
                                  /* [한국어] 로그 출력 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_print_command — qid 기준으로 admin/io 분기해 프린트.
 *
 * qid==0 이거나 opcode가 FABRIC이면 Admin 프린터 사용 (FABRIC은 I/O 큐에도 갈 수 있음).
 * 사용자가 low-level SQE를 디버그할 때 직접 호출 가능.
 */
void
spdk_nvme_print_command(uint16_t qid, struct spdk_nvme_cmd *cmd)
{
	assert(cmd != NULL);
                                  /* [한국어] NULL 방어 */

	if (qid == 0 || cmd->opc == SPDK_NVME_OPC_FABRIC) {
                                  /* [한국어] Admin 큐(qid==0) 또는 FABRIC capsule → Admin 포맷 */
		nvme_admin_qpair_print_command(qid, cmd);
	} else {
                                  /* [한국어] 일반 I/O 큐 */
		nvme_io_qpair_print_command(qid, cmd);
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_qpair_print_command — qpair 컨텍스트를 포함한 프린트.
 *
 * spdk_nvme_print_command와 달리 NVME_QPAIR_NOTICELOG를 써서 로그에 [ctrlr trid, qid]
 * 접두어까지 붙인다. qpair 레벨 디버그에서 더 유용.
 */
void
spdk_nvme_qpair_print_command(struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd)
{
	char buf[NVME_CMD_STR_SIZE] = {'\0'};
                                  /* [한국어] 1KB 스택 버퍼 */

	assert(qpair != NULL);

	if (qpair->id == 0 || cmd->opc == SPDK_NVME_OPC_FABRIC) {
                                  /* [한국어] Admin qpair 또는 FABRIC */
		nvme_get_admin_qpair_command_string(buf, sizeof(buf), qpair->id, cmd);
	} else {
                                  /* [한국어] 일반 I/O qpair */
		nvme_get_io_qpair_command_string(buf, sizeof(buf), qpair->id, cmd);
	}

	NVME_QPAIR_NOTICELOG(qpair, "%s\n", buf);
                                  /* [한국어] qpair 컨텍스트 포함 로그 (trid/qid 접두어 자동 포함) */
}

/*
 * [한국어] ★ CQE Status Code 사전들 ★
 *
 * NVMe CQE의 status 필드는 SCT(Status Code Type, 3비트) + SC(Status Code, 8비트)로 구성.
 * SCT는 SC가 속한 카테고리이며, 아래 4개 테이블로 SC를 디코딩한다:
 *   - generic_status         : SCT=0x0 Generic (SUCCESS, INVALID OPCODE/FIELD, ABORTED 등 대부분의 에러)
 *   - command_specific_status: SCT=0x1 Command Specific (INVALID QUEUE, FIRMWARE 등)
 *   - media_error_status     : SCT=0x2 Media/Data-Integrity (WRITE FAULT, UNRECOVERED READ, GUARD/APP/REF TAG CHECK)
 *   - path_status            : SCT=0x3 Path (NVMe-oF 경로 에러)
 * SCT=0x7은 Vendor Specific → "VENDOR SPECIFIC" 상수 반환(테이블 불필요).
 */

static const struct nvme_string status_type[] = {
                                  /* [한국어] SCT 값 이름 — CQE 에러 분류에 쓰임 */
	{ SPDK_NVME_SCT_GENERIC, "GENERIC" },
	{ SPDK_NVME_SCT_COMMAND_SPECIFIC, "COMMAND SPECIFIC" },
	{ SPDK_NVME_SCT_MEDIA_ERROR, "MEDIA ERROR" },
	{ SPDK_NVME_SCT_PATH, "PATH" },
	{ SPDK_NVME_SCT_VENDOR_SPECIFIC, "VENDOR SPECIFIC" },
	{ 0xFFFF, "RESERVED" },
                                  /* [한국어] sentinel */
};

static const struct nvme_string generic_status[] = {
                                  /* [한국어] SCT=GENERIC일 때의 SC 이름 테이블.
                                   *         가장 자주 참조되는 테이블 (SUCCESS, ABORTED_*, NAMESPACE_NOT_READY 등). */
	{ SPDK_NVME_SC_SUCCESS, "SUCCESS" },
	{ SPDK_NVME_SC_INVALID_OPCODE, "INVALID OPCODE" },
	{ SPDK_NVME_SC_INVALID_FIELD, "INVALID FIELD" },
	{ SPDK_NVME_SC_COMMAND_ID_CONFLICT, "COMMAND ID CONFLICT" },
	{ SPDK_NVME_SC_DATA_TRANSFER_ERROR, "DATA TRANSFER ERROR" },
	{ SPDK_NVME_SC_ABORTED_POWER_LOSS, "ABORTED - POWER LOSS" },
	{ SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, "INTERNAL DEVICE ERROR" },
	{ SPDK_NVME_SC_ABORTED_BY_REQUEST, "ABORTED - BY REQUEST" },
	{ SPDK_NVME_SC_ABORTED_SQ_DELETION, "ABORTED - SQ DELETION" },
	{ SPDK_NVME_SC_ABORTED_FAILED_FUSED, "ABORTED - FAILED FUSED" },
	{ SPDK_NVME_SC_ABORTED_MISSING_FUSED, "ABORTED - MISSING FUSED" },
	{ SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT, "INVALID NAMESPACE OR FORMAT" },
	{ SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR, "COMMAND SEQUENCE ERROR" },
	{ SPDK_NVME_SC_INVALID_SGL_SEG_DESCRIPTOR, "INVALID SGL SEGMENT DESCRIPTOR" },
	{ SPDK_NVME_SC_INVALID_NUM_SGL_DESCIRPTORS, "INVALID NUMBER OF SGL DESCRIPTORS" },
	{ SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID, "DATA SGL LENGTH INVALID" },
	{ SPDK_NVME_SC_METADATA_SGL_LENGTH_INVALID, "METADATA SGL LENGTH INVALID" },
	{ SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID, "SGL DESCRIPTOR TYPE INVALID" },
	{ SPDK_NVME_SC_INVALID_CONTROLLER_MEM_BUF, "INVALID CONTROLLER MEMORY BUFFER" },
	{ SPDK_NVME_SC_INVALID_PRP_OFFSET, "INVALID PRP OFFSET" },
	{ SPDK_NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED, "ATOMIC WRITE UNIT EXCEEDED" },
	{ SPDK_NVME_SC_OPERATION_DENIED, "OPERATION DENIED" },
	{ SPDK_NVME_SC_INVALID_SGL_OFFSET, "INVALID SGL OFFSET" },
	{ SPDK_NVME_SC_HOSTID_INCONSISTENT_FORMAT, "HOSTID INCONSISTENT FORMAT" },
	{ SPDK_NVME_SC_KEEP_ALIVE_EXPIRED, "KEEP ALIVE EXPIRED" },
	{ SPDK_NVME_SC_KEEP_ALIVE_INVALID, "KEEP ALIVE INVALID" },
	{ SPDK_NVME_SC_ABORTED_PREEMPT, "ABORTED - PREEMPT AND ABORT" },
	{ SPDK_NVME_SC_SANITIZE_FAILED, "SANITIZE FAILED" },
	{ SPDK_NVME_SC_SANITIZE_IN_PROGRESS, "SANITIZE IN PROGRESS" },
	{ SPDK_NVME_SC_SGL_DATA_BLOCK_GRANULARITY_INVALID, "DATA BLOCK GRANULARITY INVALID" },
	{ SPDK_NVME_SC_COMMAND_INVALID_IN_CMB, "COMMAND NOT SUPPORTED FOR QUEUE IN CMB" },
	{ SPDK_NVME_SC_COMMAND_NAMESPACE_IS_PROTECTED, "COMMAND NAMESPACE IS PROTECTED" },
	{ SPDK_NVME_SC_COMMAND_INTERRUPTED, "COMMAND INTERRUPTED" },
	{ SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR, "COMMAND TRANSIENT TRANSPORT ERROR" },
	{ SPDK_NVME_SC_LBA_OUT_OF_RANGE, "LBA OUT OF RANGE" },
	{ SPDK_NVME_SC_CAPACITY_EXCEEDED, "CAPACITY EXCEEDED" },
	{ SPDK_NVME_SC_NAMESPACE_NOT_READY, "NAMESPACE NOT READY" },
	{ SPDK_NVME_SC_RESERVATION_CONFLICT, "RESERVATION CONFLICT" },
	{ SPDK_NVME_SC_FORMAT_IN_PROGRESS, "FORMAT IN PROGRESS" },
	{ SPDK_NVME_SC_INVALID_VALUE_SIZE, "INVALID VALUE SIZE" },
	{ SPDK_NVME_SC_INVALID_KEY_SIZE, "INVALID KEY SIZE" },
	{ SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST, "KV KEY DOES NOT EXIST" },
	{ SPDK_NVME_SC_UNRECOVERED_ERROR, "UNRECOVERED ERROR" },
	{ SPDK_NVME_SC_KEY_EXISTS, "KEY EXISTS" },
	{ 0xFFFF, "GENERIC" }
                                  /* [한국어] sentinel — 정의되지 않은 Generic SC는 그냥 "GENERIC" */
};

static const struct nvme_string command_specific_status[] = {
                                  /* [한국어] SCT=COMMAND_SPECIFIC일 때의 SC 테이블.
                                   *         queue 관리, firmware, namespace 관리, zone 관련 에러 등. */
	{ SPDK_NVME_SC_COMPLETION_QUEUE_INVALID, "INVALID COMPLETION QUEUE" },
	{ SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER, "INVALID QUEUE IDENTIFIER" },
	{ SPDK_NVME_SC_INVALID_QUEUE_SIZE, "INVALID QUEUE SIZE" },
	{ SPDK_NVME_SC_ABORT_COMMAND_LIMIT_EXCEEDED, "ABORT CMD LIMIT EXCEEDED" },
	{ SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED, "ASYNC LIMIT EXCEEDED" },
	{ SPDK_NVME_SC_INVALID_FIRMWARE_SLOT, "INVALID FIRMWARE SLOT" },
	{ SPDK_NVME_SC_INVALID_FIRMWARE_IMAGE, "INVALID FIRMWARE IMAGE" },
	{ SPDK_NVME_SC_INVALID_INTERRUPT_VECTOR, "INVALID INTERRUPT VECTOR" },
	{ SPDK_NVME_SC_INVALID_LOG_PAGE, "INVALID LOG PAGE" },
	{ SPDK_NVME_SC_INVALID_FORMAT, "INVALID FORMAT" },
	{ SPDK_NVME_SC_FIRMWARE_REQ_CONVENTIONAL_RESET, "FIRMWARE REQUIRES CONVENTIONAL RESET" },
	{ SPDK_NVME_SC_INVALID_QUEUE_DELETION, "INVALID QUEUE DELETION" },
	{ SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE, "FEATURE ID NOT SAVEABLE" },
	{ SPDK_NVME_SC_FEATURE_NOT_CHANGEABLE, "FEATURE NOT CHANGEABLE" },
	{ SPDK_NVME_SC_FEATURE_NOT_NAMESPACE_SPECIFIC, "FEATURE NOT NAMESPACE SPECIFIC" },
	{ SPDK_NVME_SC_FIRMWARE_REQ_NVM_RESET, "FIRMWARE REQUIRES NVM RESET" },
	{ SPDK_NVME_SC_FIRMWARE_REQ_RESET, "FIRMWARE REQUIRES RESET" },
	{ SPDK_NVME_SC_FIRMWARE_REQ_MAX_TIME_VIOLATION, "FIRMWARE REQUIRES MAX TIME VIOLATION" },
	{ SPDK_NVME_SC_FIRMWARE_ACTIVATION_PROHIBITED, "FIRMWARE ACTIVATION PROHIBITED" },
	{ SPDK_NVME_SC_OVERLAPPING_RANGE, "OVERLAPPING RANGE" },
	{ SPDK_NVME_SC_NAMESPACE_INSUFFICIENT_CAPACITY, "NAMESPACE INSUFFICIENT CAPACITY" },
	{ SPDK_NVME_SC_NAMESPACE_ID_UNAVAILABLE, "NAMESPACE ID UNAVAILABLE" },
	{ SPDK_NVME_SC_NAMESPACE_ALREADY_ATTACHED, "NAMESPACE ALREADY ATTACHED" },
	{ SPDK_NVME_SC_NAMESPACE_IS_PRIVATE, "NAMESPACE IS PRIVATE" },
	{ SPDK_NVME_SC_NAMESPACE_NOT_ATTACHED, "NAMESPACE NOT ATTACHED" },
	{ SPDK_NVME_SC_THINPROVISIONING_NOT_SUPPORTED, "THINPROVISIONING NOT SUPPORTED" },
	{ SPDK_NVME_SC_CONTROLLER_LIST_INVALID, "CONTROLLER LIST INVALID" },
	{ SPDK_NVME_SC_DEVICE_SELF_TEST_IN_PROGRESS, "DEVICE SELF-TEST IN PROGRESS" },
	{ SPDK_NVME_SC_BOOT_PARTITION_WRITE_PROHIBITED, "BOOT PARTITION WRITE PROHIBITED" },
	{ SPDK_NVME_SC_INVALID_CTRLR_ID, "INVALID CONTROLLER ID" },
	{ SPDK_NVME_SC_INVALID_SECONDARY_CTRLR_STATE, "INVALID SECONDARY CONTROLLER STATE" },
	{ SPDK_NVME_SC_INVALID_NUM_CTRLR_RESOURCES, "INVALID NUMBER OF CONTROLLER RESOURCES" },
	{ SPDK_NVME_SC_INVALID_RESOURCE_ID, "INVALID RESOURCE IDENTIFIER" },
	{ SPDK_NVME_SC_SANITIZE_PROHIBITED, "SANITIZE PROHIBITED" },
	{ SPDK_NVME_SC_ANA_GROUP_IDENTIFIER_INVALID, "ANA GROUP IDENTIFIER INVALID" },
	{ SPDK_NVME_SC_ANA_ATTACH_FAILED, "ANA ATTACH FAILED" },
	{ SPDK_NVME_SC_INSUFFICIENT_CAPACITY, "INSUFFICIENT CAPACITY" },
	{ SPDK_NVME_SC_NAMESPACE_ATTACH_LIMIT_EXCEEDED, "NAMESPACE ATTACH LIMIT EXCEEDED" },
	{ SPDK_NVME_SC_PROHIBIT_CMD_EXEC_NOT_SUPPORTED, "PROHIBIT COMMAND EXEC NOT SUPPORTED" },
	{ SPDK_NVME_SC_IOCS_NOT_SUPPORTED, "IOCS NOT SUPPORTED" },
	{ SPDK_NVME_SC_IOCS_NOT_ENABLED, "IOCS NOT ENABLED" },
	{ SPDK_NVME_SC_IOCS_COMBINATION_REJECTED, "IOCS COMBINATION REJECTED" },
	{ SPDK_NVME_SC_INVALID_IOCS, "INVALID IOCS" },
	{ SPDK_NVME_SC_IDENTIFIER_UNAVAILABLE, "IDENTIFIER UNAVAILABLE" },
	{ SPDK_NVME_SC_STREAM_RESOURCE_ALLOCATION_FAILED, "STREAM RESOURCE ALLOCATION FAILED"},
	{ SPDK_NVME_SC_CONFLICTING_ATTRIBUTES, "CONFLICTING ATTRIBUTES" },
	{ SPDK_NVME_SC_INVALID_PROTECTION_INFO, "INVALID PROTECTION INFO" },
	{ SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE, "WRITE TO RO RANGE" },
	{ SPDK_NVME_SC_CMD_SIZE_LIMIT_SIZE_EXCEEDED, "CMD SIZE LIMIT SIZE EXCEEDED" },
	{ SPDK_NVME_SC_ZONED_BOUNDARY_ERROR, "ZONED BOUNDARY ERROR" },
	{ SPDK_NVME_SC_ZONE_IS_FULL, "ZONE IS FULL" },
	{ SPDK_NVME_SC_ZONE_IS_READ_ONLY, "ZONE IS READ ONLY" },
	{ SPDK_NVME_SC_ZONE_IS_OFFLINE, "ZONE IS OFFLINE" },
	{ SPDK_NVME_SC_ZONE_INVALID_WRITE, "ZONE INVALID WRITE" },
	{ SPDK_NVME_SC_TOO_MANY_ACTIVE_ZONES, "TOO MANY ACTIVE ZONES" },
	{ SPDK_NVME_SC_TOO_MANY_OPEN_ZONES, "TOO MANY OPEN ZONES" },
	{ SPDK_NVME_SC_INVALID_ZONE_STATE_TRANSITION, "INVALID ZONE STATE TRANSITION" },
	{ 0xFFFF, "COMMAND SPECIFIC" }
                                  /* [한국어] sentinel */
};

static const struct nvme_string media_error_status[] = {
                                  /* [한국어] SCT=MEDIA_ERROR일 때의 SC 테이블.
                                   *         실제 매체 장애(WRITE FAULT, UNRECOVERED READ)와
                                   *         PI 체크 실패(GUARD/APP TAG/REF TAG)를 구분. COMPARE_FAILURE도 여기. */
	{ SPDK_NVME_SC_WRITE_FAULTS, "WRITE FAULTS" },
	{ SPDK_NVME_SC_UNRECOVERED_READ_ERROR, "UNRECOVERED READ ERROR" },
	{ SPDK_NVME_SC_GUARD_CHECK_ERROR, "GUARD CHECK ERROR" },
	{ SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR, "APPLICATION TAG CHECK ERROR" },
	{ SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR, "REFERENCE TAG CHECK ERROR" },
	{ SPDK_NVME_SC_COMPARE_FAILURE, "COMPARE FAILURE" },
	{ SPDK_NVME_SC_ACCESS_DENIED, "ACCESS DENIED" },
	{ SPDK_NVME_SC_DEALLOCATED_OR_UNWRITTEN_BLOCK, "DEALLOCATED OR UNWRITTEN BLOCK" },
	{ SPDK_NVME_SC_END_TO_END_STORAGE_TAG_CHECK_ERROR, "END TO END STORAGE TAG CHECK ERROR" },
	{ SPDK_OCSSD_SC_OFFLINE_CHUNK, "RESET OFFLINE CHUNK" },
	{ SPDK_OCSSD_SC_INVALID_RESET, "INVALID RESET" },
	{ SPDK_OCSSD_SC_WRITE_FAIL_WRITE_NEXT_UNIT, "WRITE FAIL WRITE NEXT UNIT" },
	{ SPDK_OCSSD_SC_WRITE_FAIL_CHUNK_EARLY_CLOSE, "WRITE FAIL CHUNK EARLY CLOSE" },
	{ SPDK_OCSSD_SC_OUT_OF_ORDER_WRITE, "OUT OF ORDER WRITE" },
	{ SPDK_OCSSD_SC_READ_HIGH_ECC, "READ HIGH ECC" },
	{ 0xFFFF, "MEDIA ERROR" }
                                  /* [한국어] sentinel */
};

static const struct nvme_string path_status[] = {
                                  /* [한국어] SCT=PATH일 때의 SC 테이블. NVMe-oF ANA(Asymmetric Namespace Access),
                                   *         controller/host path error 등 multi-path에서 중요. */
	{ SPDK_NVME_SC_INTERNAL_PATH_ERROR, "INTERNAL PATH ERROR" },
	{ SPDK_NVME_SC_ASYMMETRIC_ACCESS_PERSISTENT_LOSS, "ASYMMETRIC ACCESS PERSISTENT LOSS" },
	{ SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE, "ASYMMETRIC ACCESS INACCESSIBLE" },
	{ SPDK_NVME_SC_ASYMMETRIC_ACCESS_TRANSITION, "ASYMMETRIC ACCESS TRANSITION" },
	{ SPDK_NVME_SC_CONTROLLER_PATH_ERROR, "CONTROLLER PATH ERROR" },
	{ SPDK_NVME_SC_HOST_PATH_ERROR, "HOST PATH ERROR" },
	{ SPDK_NVME_SC_ABORTED_BY_HOST, "ABORTED BY HOST" },
	{ 0xFFFF, "PATH ERROR" }
                                  /* [한국어] sentinel */
};

/*
 * [한국어] ★ 공개 API: spdk_nvme_cpl_get_status_string ★
 *
 * CQE의 status를 사람이 읽는 문자열로 변환.
 * SCT로 적절한 테이블을 선택한 뒤 SC 검색. Vendor Specific/Reserved는 테이블 없이 직접 문자열.
 *
 * 예: status.sct=SPDK_NVME_SCT_MEDIA_ERROR, status.sc=SPDK_NVME_SC_COMPARE_FAILURE
 *     → "COMPARE FAILURE"
 *
 * 사용처: bdev_nvme의 에러 로그, tests, nvme_perf/identify 등에서 실패 원인 프린트.
 */
const char *
spdk_nvme_cpl_get_status_string(const struct spdk_nvme_status *status)
{
	const struct nvme_string *entry;
                                  /* [한국어] 선택된 SC 테이블 포인터 */

	switch (status->sct) {
                                  /* [한국어] SCT별 분기 — 5개 대표값 + default */
	case SPDK_NVME_SCT_GENERIC:
		entry = generic_status;
                                  /* [한국어] SCT=0 : 가장 흔한 에러들 */
		break;
	case SPDK_NVME_SCT_COMMAND_SPECIFIC:
		entry = command_specific_status;
		break;
	case SPDK_NVME_SCT_MEDIA_ERROR:
		entry = media_error_status;
		break;
	case SPDK_NVME_SCT_PATH:
		entry = path_status;
		break;
	case SPDK_NVME_SCT_VENDOR_SPECIFIC:
		return "VENDOR SPECIFIC";
                                  /* [한국어] 벤더 고유 — 의미는 각 벤더 문서 참조, 여기선 라벨만 */
	default:
		return "RESERVED";
                                  /* [한국어] 스펙상 미정의 값 */
	}

	return nvme_get_string(entry, status->sc);
                                  /* [한국어] 선택된 테이블에서 SC 검색 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_cpl_get_status_type_string — SCT의 이름만 반환 ★
 */
const char *
spdk_nvme_cpl_get_status_type_string(const struct spdk_nvme_status *status)
{
	return nvme_get_string(status_type, status->sct);
                                  /* [한국어] "GENERIC" / "COMMAND SPECIFIC" / ... / "RESERVED" */
}

/*
 * [한국어] nvme_get_completion_string - CQE 전 필드를 한 줄 문자열로 포맷.
 *
 * 출력 예: "SUCCESS (00/00) qid:1 cid:42 cdw0:00000000 sqhd:0042 p:1 m:0 dnr:0"
 *
 * 포함 필드:
 *   - status 문자열 + SCT/SC 숫자
 *   - qid(콜러 제공), cid(CQE에서)
 *   - cdw0 (명령별 완료 데이터 — 예: Zone Append의 최종 LBA)
 *   - sqhd(장치가 소비한 SQ head) — 디버그에서 SQ 드레인 추적에 유용
 *   - p(phase bit) / m(more) / dnr(do-not-retry) — 완료 메타
 */
static void
nvme_get_completion_string(char *buf, size_t size, uint16_t qid, struct spdk_nvme_cpl *cpl)
{
	assert(cpl != NULL);
                                  /* [한국어] NULL 방어 */

	snprintf(buf, size,
		 "%s (%02" PRIx8 "/%02" PRIx8 ") qid:%" PRIu16 " cid:%" PRIu16 " cdw0:%08" PRIx32 " sqhd:%04" PRIx16
		 " p:%" PRIx16 " m:%" PRIx16 " dnr:%" PRIx16,
		 spdk_nvme_cpl_get_status_string(&cpl->status), cpl->status.sct, cpl->status.sc, qid, cpl->cid,
		 cpl->cdw0, cpl->sqhd, cpl->status.p, cpl->status.m, cpl->status.dnr);
                                  /* [한국어] SCT/SC는 16진수 2자리(%02PRIx8), cdw0는 8자리,
                                   *         sqhd는 4자리, p/m/dnr은 비트이므로 1자리로 충분 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_print_completion ★
 *
 * 기본 CQE 프린트 (qpair 컨텍스트 없이). CQE.sqid가 기대 qid와 다르면 ERRLOG.
 * NVMe-oF에서는 sqid가 Reserved로 0일 수 있어 0은 예외로 경고 생략.
 */
void
spdk_nvme_print_completion(uint16_t qid, struct spdk_nvme_cpl *cpl)
{
	char buf[NVME_CMD_STR_SIZE] = {'\0'};
                                  /* [한국어] 1KB 스택 버퍼 */

	/* Check that sqid matches qid. Note that sqid is reserved
	 * for fabrics so don't print an error when sqid is 0. */
	if (cpl->sqid != qid && cpl->sqid != 0) {
                                  /* [한국어] sqid 불일치 경고 — 트랜스포트 라우팅 버그 조기 탐지 */
		SPDK_ERRLOG("sqid %u doesn't match qid\n", cpl->sqid);
	}

	nvme_get_completion_string(buf, sizeof(buf), qid, cpl);
                                  /* [한국어] 문자열 포맷 */
	SPDK_NOTICELOG("%s\n", buf);
                                  /* [한국어] 로그 출력 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_qpair_print_completion ★
 *
 * qpair 컨텍스트를 포함하는 CQE 프린트. NVME_QPAIR_ERRLOG/NOTICELOG는 trid/qid 접두어 자동 부착.
 */
void
spdk_nvme_qpair_print_completion(struct spdk_nvme_qpair *qpair, struct spdk_nvme_cpl *cpl)
{
	char buf[NVME_CMD_STR_SIZE] = {'\0'};
                                  /* [한국어] 1KB 스택 버퍼 */

	if (cpl->sqid != qpair->id && cpl->sqid != 0) {
                                  /* [한국어] sqid vs qpair->id 불일치 시 qpair-aware 에러 로그 */
		NVME_QPAIR_ERRLOG(qpair, "sqid %u doesn't match qid\n", cpl->sqid);
	}

	nvme_get_completion_string(buf, sizeof(buf), qpair->id, cpl);
                                  /* [한국어] 포맷 */
	NVME_QPAIR_NOTICELOG(qpair, "%s\n", buf);
                                  /* [한국어] qpair 문맥 로그 */
}

/*
 * [한국어] nvme_qpair_state_string - enum nvme_qpair_state를 사람이 읽는 문자열로.
 *
 * qpair 수명주기 상태머신 (nvme_internal.h 정의):
 *
 *   DISCONNECTED ─(connect 요청)─→ CONNECTING ─(트랜스포트 핸드셰이크 완료)─→ CONNECTED
 *                                                                               │
 *                                                                               ▼
 *                                                                          ENABLING
 *                                  (check_enabled가 전이)                       │
 *                                                                               ▼
 *                                                                           ENABLED   ← 제출·완료 hot path
 *                                                                               │
 *     ┌─────────────────────────────────────────────────────────────────────────┘
 *     ▼
 *  DISCONNECTING ─(transport disconnect 완료)─→ DISCONNECTED (재연결 가능)
 *     또는
 *  DESTROYING (해제 중) ─→ free
 *
 * 호출자: 디버그 로그, NVME_QPAIR_DEBUGLOG, 에러 메시지에서 상태 표현.
 */
const char *
nvme_qpair_state_string(enum nvme_qpair_state state)
{
	switch (state) {
	case NVME_QPAIR_DISCONNECTED:
                                  /* [한국어] 트랜스포트 연결 없음 — 초기 상태 또는 disconnect 후 */
		return "DISCONNECTED";
	case NVME_QPAIR_DISCONNECTING:
                                  /* [한국어] disconnect 진행 중 — async 완료 대기 */
		return "DISCONNECTING";
	case NVME_QPAIR_CONNECTING:
                                  /* [한국어] fabrics CONNECT 제출 후 완료 대기 (CONNECT SQE만 통과 허용) */
		return "CONNECTING";
	case NVME_QPAIR_CONNECTED:
                                  /* [한국어] 트랜스포트 연결 성공. 아직 I/O는 금지 — ENABLING 전이 대기 */
		return "CONNECTED";
	case NVME_QPAIR_ENABLING:
                                  /* [한국어] check_enabled가 CONNECTED→ENABLING로 전이시키고 queued_req flush 중 */
		return "ENABLING";
	case NVME_QPAIR_ENABLED:
                                  /* [한국어] ★ hot path — I/O 자유 제출 가능 */
		return "ENABLED";
	case NVME_QPAIR_DESTROYING:
                                  /* [한국어] free 경로 — outstanding I/O abort 후 해제 대기 */
		return "DESTROYING";
	default:
                                  /* [한국어] 스펙 외 값 방어 */
		return "UNKNOWN";
	}
}

/*
 * [한국어] nvme_completion_is_retry - CQE status를 보고 재시도 가능 여부 판단.
 *
 * @cpl: 완료 엔트리 (status.sct/sc/dnr 참조)
 * @return: true=재시도 권장, false=영구 실패
 *
 * 호출 맥락:
 *   - bdev_nvme 등 상위 레이어가 완료 콜백에서 "이 실패를 재시도할지"를 결정할 때 사용.
 *   - SPDK는 "오직 일시적으로 장치가 준비 안 된 경우"만 재시도 허용 (SC_NAMESPACE_NOT_READY,
 *     SC_FORMAT_IN_PROGRESS)이고, 이때도 DNR(Do Not Retry) 비트가 세팅되어 있으면 재시도 금지.
 *   - Path 에러는 TP 4028에 따라 INTERNAL_PATH_ERROR만 DNR 기반 재시도 가능.
 *
 * 보수적 접근: 대부분의 에러는 재시도 불가 반환. 호스트가 재시도하면 오히려 무한 루프 위험.
 */
bool
nvme_completion_is_retry(const struct spdk_nvme_cpl *cpl)
{
	/*
	 * TODO: spec is not clear how commands that are aborted due
	 *  to TLER will be marked.  So for now, it seems
	 *  NAMESPACE_NOT_READY is the only case where we should
	 *  look at the DNR bit.
	 */
	switch ((int)cpl->status.sct) {
                                  /* [한국어] SCT별로 분기 */
	case SPDK_NVME_SCT_GENERIC:
                                  /* [한국어] 대부분의 일반 에러 */
		switch ((int)cpl->status.sc) {
		case SPDK_NVME_SC_NAMESPACE_NOT_READY:
		case SPDK_NVME_SC_FORMAT_IN_PROGRESS:
                                  /* [한국어] 일시적 비가용 — 장치가 준비되면 다시 처리 가능 */
			if (cpl->status.dnr) {
                                  /* [한국어] 그래도 DNR가 세팅되어 있으면 장치가 "영구 실패" 선언 — 재시도 포기 */
				return false;
			} else {
				return true;
                                  /* [한국어] 재시도 권장 */
			}
		case SPDK_NVME_SC_INVALID_OPCODE:
		case SPDK_NVME_SC_INVALID_FIELD:
		case SPDK_NVME_SC_COMMAND_ID_CONFLICT:
		case SPDK_NVME_SC_DATA_TRANSFER_ERROR:
		case SPDK_NVME_SC_ABORTED_POWER_LOSS:
		case SPDK_NVME_SC_INTERNAL_DEVICE_ERROR:
		case SPDK_NVME_SC_ABORTED_BY_REQUEST:
		case SPDK_NVME_SC_ABORTED_SQ_DELETION:
		case SPDK_NVME_SC_ABORTED_FAILED_FUSED:
		case SPDK_NVME_SC_ABORTED_MISSING_FUSED:
		case SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT:
		case SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR:
		case SPDK_NVME_SC_LBA_OUT_OF_RANGE:
		case SPDK_NVME_SC_CAPACITY_EXCEEDED:
                                  /* [한국어] 명시적으로 "재시도 무의미"한 에러들 — 호출자가 이 코드를 읽고
                                   *         영구 실패 처리할 것을 명확히 함 (fall-through로 default에 합침) */
		default:
                                  /* [한국어] 그 외 Generic SC도 기본적으로 영구 실패 취급 */
			return false;
		}
	case SPDK_NVME_SCT_PATH:
		/*
		 * Per NVMe TP 4028 (Path and Transport Error Enhancements), retries should be
		 * based on the setting of the DNR bit for Internal Path Error
		 */
		switch ((int)cpl->status.sc) {
		case SPDK_NVME_SC_INTERNAL_PATH_ERROR:
                                  /* [한국어] TP 4028에 따라 DNR 기반 판단 — 경로가 복구되면 재시도 가능 */
			return !cpl->status.dnr;
		default:
                                  /* [한국어] 기타 path 에러 (ANA_PERSISTENT_LOSS, INACCESSIBLE 등)는 영구 */
			return false;
		}
	case SPDK_NVME_SCT_COMMAND_SPECIFIC:
	case SPDK_NVME_SCT_MEDIA_ERROR:
	case SPDK_NVME_SCT_VENDOR_SPECIFIC:
                                  /* [한국어] Command Specific / Media / Vendor 에러는 모두 영구 */
	default:
		return false;
	}
}

/*
 * [한국어] ★ nvme_qpair_manual_complete_request — 트랜스포트 경유 없이 req를 "가짜 CQE"로 완료.
 *
 * @qpair: 소유 qpair
 * @req:   완료 처리할 nvme_request (아직 장치로 안 나갔거나, 장치 응답 전 호스트가 포기한 요청)
 * @sct, @sc: 호스트가 지어내는 SCT/SC 쌍. 예: SPDK_NVME_SCT_GENERIC / SC_ABORTED_SQ_DELETION
 * @dnr:    Do-Not-Retry 비트 (0 or 1)
 * @print_on_error: true면 에러 상태일 때 로그 출력
 *
 * 사용 맥락:
 *   - queued_req를 abort할 때 (`abort_queued_reqs`): SC_ABORTED_SQ_DELETION
 *   - error injection으로 "do_not_submit" 설정된 opcode 완료 시
 *   - reset 중 제출 실패한 request 완료 시
 *
 * 핵심: 이 함수는 장치에 아무것도 보내지 않는다 — 그저 cpl를 메모리에서 합성한 뒤
 *       nvme_complete_request를 통해 호스트 콜백(cb_fn)을 즉시 실행할 뿐.
 *
 * 실행 컨텍스트: qpair 소유 스레드. 사용자 cb_fn이 동기 호출됨.
 */
static void
nvme_qpair_manual_complete_request(struct spdk_nvme_qpair *qpair,
				   struct nvme_request *req, uint32_t sct, uint32_t sc,
				   uint32_t dnr, bool print_on_error)
{
	struct spdk_nvme_cpl	cpl;
                                  /* [한국어] 합성할 가짜 CQE (스택 16바이트) */
	bool			error;
                                  /* [한국어] cpl이 에러 상태인지 — 로그 출력 판정용 */

	memset(&cpl, 0, sizeof(cpl));
                                  /* [한국어] CQE 전 필드 0으로 초기화 (cdw0/sqhd/sqid/cid/status 모두 0) */
	cpl.sqid = qpair->id;
                                  /* [한국어] 이 qpair의 SQ ID로 위조 — print_completion이 sqid 불일치 경고 내지 않도록 */
	cpl.status.sct = sct;
                                  /* [한국어] 호출자가 지어낸 SCT */
	cpl.status.sc = sc;
                                  /* [한국어] 호출자가 지어낸 SC */
	cpl.status.dnr = dnr;
                                  /* [한국어] Do-Not-Retry (영구 실패로 표시할지) */

	error = spdk_nvme_cpl_is_error(&cpl);
                                  /* [한국어] SC!=SUCCESS 면 에러로 판정 */

	if (error && print_on_error && !qpair->ctrlr->opts.disable_error_logging) {
                                  /* [한국어] 에러이면서 호출자가 로그 원하고, 컨트롤러 옵션이 에러 로그를 끄지 않은 경우 */
		NVME_QPAIR_NOTICELOG(qpair, "Command completed manually:\n");
                                  /* [한국어] 수동 완료임을 명시 (실제 장치 CQE와 구별) */
		spdk_nvme_qpair_print_command(qpair, &req->cmd);
                                  /* [한국어] 문제된 SQE 내용 */
		spdk_nvme_qpair_print_completion(qpair, &cpl);
                                  /* [한국어] 합성한 CQE 내용 */
	}

	nvme_complete_request(req->cb_fn, req->cb_arg, qpair, req, &cpl);
                                  /* [한국어] 공용 완료 경로 — child 병합, accel sequence 정리, cb_fn 호출,
                                   *         req를 free_req 풀로 반환 (nvme_internal.h에 선언, 구현은 nvme.c) */
}

/*
 * [한국어] nvme_qpair_abort_queued_reqs - qpair->queued_req 리스트 전체를 "SQ deletion" 에러로 완료.
 *
 * disconnect/destroy/reset 중 "아직 장치로 못 보낸 대기 요청"들을 호스트 측에서
 * 강제로 완료 처리하는 경로. cpl status = GENERIC/ABORTED_SQ_DELETION.
 *
 * 구현 트릭 — SWAP + local list:
 *   1) 로컬 tmp 리스트를 만들고 qpair->queued_req와 포인터 스왑 (한 순간에 원자적으로 비움)
 *   2) tmp 리스트에서 한 개씩 pop하며 manual_complete 수행
 *   이 방식을 쓰면 cb_fn이 실행 중 **새 request를 qpair->queued_req에 다시 enqueue 해도**
 *   현재 abort 루프에는 포함되지 않아 무한 루프 회피.
 */
void
nvme_qpair_abort_queued_reqs(struct spdk_nvme_qpair *qpair)
{
	struct nvme_request		*req;
                                  /* [한국어] 루프 반복자 */
	STAILQ_HEAD(, nvme_request)	tmp;
                                  /* [한국어] 로컬 단방향 리스트 — queued_req를 한 번에 옮겨 담을 용도 */

	STAILQ_INIT(&tmp);
                                  /* [한국어] 로컬 리스트 초기화 (head=NULL) */
	STAILQ_SWAP(&tmp, &qpair->queued_req, nvme_request);
                                  /* [한국어] ★ 한 줄로 qpair->queued_req를 tmp로 이관. qpair->queued_req는 빈 상태. */

	while (!STAILQ_EMPTY(&tmp)) {
                                  /* [한국어] tmp가 빌 때까지 — cb_fn이 새로 queued_req에 enqueue해도 현재 루프엔 영향 없음 */
		req = STAILQ_FIRST(&tmp);
		STAILQ_REMOVE_HEAD(&tmp, stailq);
		if (!qpair->ctrlr->opts.disable_error_logging) {
                                  /* [한국어] 컨트롤러 옵션이 에러 로그를 끄지 않으면 표시 */
			NVME_QPAIR_ERRLOG(qpair, "aborting queued i/o\n");
		}
		nvme_qpair_manual_complete_request(qpair, req, SPDK_NVME_SCT_GENERIC,
						   SPDK_NVME_SC_ABORTED_SQ_DELETION, qpair->abort_dnr, true);
                                  /* [한국어] "SQ 삭제로 인한 abort"로 완료 — dnr은 qpair 단위 설정
                                   *         (spdk_nvme_qpair_set_abort_dnr로 제어) */
	}
}

/* The callback to a request may submit the next request which is queued and
 * then the same callback may abort it immediately. This repetition may cause
 * infinite recursive calls. Hence move aborting requests to another list here
 * and abort them later at resubmission.
 */
/*
 * [한국어] _nvme_qpair_complete_abort_queued_reqs - aborting_queued_req 리스트를 완료 처리.
 *
 * `nvme_qpair_abort_queued_reqs_with_cbarg`가 cb_arg 기준으로 선별한 요청들은
 * 즉시 complete하지 않고 aborting_queued_req 리스트에 먼저 옮긴다. 이후
 * 안전한 타이밍(완료 후 또는 resubmit 후)에 이 함수를 호출해 실제 complete.
 *
 * 이렇게 분리하는 이유(영문 주석의 설명):
 *   cb_fn 안에서 새 request를 제출 → 그 request를 즉시 abort 요청 → 그 abort의 cb_fn에서
 *   또 제출/abort... 하는 재귀가 발생할 수 있다. `manual_complete` 내부는 cb_fn을 동기 호출하므로
 *   스택이 무한히 깊어짐. 따라서 abort 대상만 먼저 "격리"하고, 재귀 가능성 없는 위치에서 한 번에 complete.
 */
static void
_nvme_qpair_complete_abort_queued_reqs(struct spdk_nvme_qpair *qpair)
{
	struct nvme_request		*req;
                                  /* [한국어] 반복자 */
	STAILQ_HEAD(, nvme_request)	tmp;
                                  /* [한국어] 로컬 이동용 리스트 */

	if (spdk_likely(STAILQ_EMPTY(&qpair->aborting_queued_req))) {
                                  /* [한국어] 대부분 비어있음 — likely 힌트로 early return */
		return;
	}

	STAILQ_INIT(&tmp);
                                  /* [한국어] 로컬 리스트 초기화 */
	STAILQ_SWAP(&tmp, &qpair->aborting_queued_req, nvme_request);
                                  /* [한국어] aborting_queued_req를 로컬로 이관 — 이후 cb_fn이 다시 abort 요청해도 영향 없음 */

	while (!STAILQ_EMPTY(&tmp)) {
		req = STAILQ_FIRST(&tmp);
		STAILQ_REMOVE_HEAD(&tmp, stailq);
		nvme_qpair_manual_complete_request(qpair, req, SPDK_NVME_SCT_GENERIC,
						   SPDK_NVME_SC_ABORTED_BY_REQUEST, 1, true);
                                  /* [한국어] "명시적 abort 요청"으로 완료. dnr=1(영구) — 호출자가 abort를 요청했으므로 */
	}
}

/*
 * [한국어] nvme_qpair_abort_queued_reqs_with_cbarg - cb_arg 매칭되는 queued_req만 abort 대상으로 격리.
 *
 * @qpair:      대상 qpair
 * @cmd_cb_arg: 매칭 기준 — nvme_request.cb_arg와 비교 (포인터 일치)
 * @return:     격리된 request 개수
 *
 * 사용처: `spdk_nvme_ctrlr_cmd_abort`가 특정 I/O를 취소할 때.
 *         이미 장치에 나간 요청은 NVMe ABORT 명령을 보내지만, 아직 queued인 요청은
 *         이 함수로 즉시 로컬에서 abort.
 *
 * 즉시 complete가 아니라 aborting_queued_req로 **이동만** 수행 — 실제 complete는
 * `_nvme_qpair_complete_abort_queued_reqs`가 나중에 수행 (재귀 방지).
 */
uint32_t
nvme_qpair_abort_queued_reqs_with_cbarg(struct spdk_nvme_qpair *qpair, void *cmd_cb_arg)
{
	struct nvme_request	*req, *tmp;
                                  /* [한국어] FOREACH_SAFE는 tmp로 다음 노드 미리 보관 — 현재 노드 제거에도 안전 */
	uint32_t		aborting = 0;
                                  /* [한국어] 격리된 개수 카운터 */

	STAILQ_FOREACH_SAFE(req, &qpair->queued_req, stailq, tmp) {
                                  /* [한국어] queued_req를 순회 */
		if (!nvme_request_abort_match(req, cmd_cb_arg)) {
                                  /* [한국어] cb_arg가 일치하지 않으면 스킵 — split된 parent/child의 재귀 매칭 포함 */
			continue;
		}

		STAILQ_REMOVE(&qpair->queued_req, req, nvme_request, stailq);
                                  /* [한국어] queued_req에서 제거 */
		STAILQ_INSERT_TAIL(&qpair->aborting_queued_req, req, stailq);
                                  /* [한국어] aborting 리스트 꼬리에 추가 — 나중에 일괄 complete */
		if (!qpair->ctrlr->opts.disable_error_logging) {
			NVME_QPAIR_ERRLOG(qpair, "aborting queued i/o\n");
		}
		aborting++;
                                  /* [한국어] 카운트 증가 */
	}

	return aborting;
                                  /* [한국어] 호출자는 개수로 abort가 실제 발생했는지 판단 */
}

/*
 * [한국어] ★ nvme_qpair_check_enabled - 상태머신 전이 훅 + reset 감지 ★
 *
 * 이 함수는 "qpair가 I/O 제출 가능한 ENABLED 상태인가?"를 묻는 질문 외에,
 * 주요 상태 전이를 유발하는 **명령형(imperative) 헬퍼**이기도 하다.
 * submit/completion 진입부에서 먼저 호출되어 다음 작업을 수행:
 *
 * 1) **CONNECTED → ENABLING → ENABLED 전이**:
 *    - 트랜스포트 핸드셰이크가 끝나 qpair가 CONNECTED가 되면, 여기서 ENABLED로 승격.
 *    - PCIe reset의 특수 케이스: PCIe는 disconnect하지 않고 reset하므로
 *      old connection 이전에 제출된 outstanding request들을 **여기서 abort**
 *      (트랜스포트는 메모리 상태를 리셋했기 때문). 호스트 측 cb_fn은 에러 전달 받음.
 *    - ENABLING 진입 이후 queued_req에 대기하던 요청들을 resubmit으로 flush.
 *
 * 2) **reset 감지 → disconnect**:
 *    - transport_failure_reason이 세팅되어 있으면 reset이 발생한 것.
 *    - PCIe가 아닌 경우(NVMe-oF RDMA/TCP) 여기서 disconnect 호출.
 *    - FAILURE_RESET인 경우 multi-process 동기화를 위해 여기서 재초기화.
 *
 * @return: true=ENABLED(즉시 제출 가능), false=아직 enable 안 됨 or reset 중
 *
 * 호출자: `_nvme_qpair_submit_request`(제출 전), `spdk_nvme_qpair_process_completions`(완료 전).
 */
static inline bool
nvme_qpair_check_enabled(struct spdk_nvme_qpair *qpair)
{
	struct nvme_request *req;
                                  /* [한국어] queued_req flush 루프의 반복자 */

	/*
	 * Either during initial connect or reset, the qpair should follow the given state machine.
	 * QPAIR_DISABLED->QPAIR_CONNECTING->QPAIR_CONNECTED->QPAIR_ENABLING->QPAIR_ENABLED. In the
	 * reset case, once the qpair is properly connected, we need to abort any outstanding requests
	 * from the old transport connection and encourage the application to retry them. We also need
	 * to submit any queued requests that built up while we were in the connected or enabling state.
	 */
	if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTED &&
			  !qpair->ctrlr->is_resetting)) {
                                  /* [한국어] CONNECTED이고 ctrlr reset이 진행 중이 아닌 경우에만 enable 진행.
                                   *         reset 중이면 ctrlr 레벨에서 모든 qpair를 동기화하므로 여기선 미루기. */
		nvme_qpair_set_state(qpair, NVME_QPAIR_ENABLING);
                                  /* [한국어] 중간 상태 ENABLING — 다른 스레드가 제출 시도해도 여기서 drain 진행 중임을 표시 */
		/*
		 * PCIe is special, for fabrics transports, we can abort requests before disconnect during reset
		 * but we have historically not disconnected pcie qpairs during reset so we have to abort requests
		 * here.
		 */
		if (qpair->ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE &&
		    !qpair->is_new_qpair) {
                                  /* [한국어] PCIe + 재사용 qpair인 경우: 이전 연결의 outstanding request들이
                                   *         장치 reset으로 소실되었으므로 호스트 측에서 에러 완료해야 함 */
			nvme_qpair_abort_all_queued_reqs(qpair);
                                  /* [한국어] queued_req + err_req_head + aborting 전부 완료 */
			nvme_transport_qpair_abort_reqs(qpair);
                                  /* [한국어] 트랜스포트 레벨 outstanding(tracker 등)도 abort */
		}

		nvme_qpair_set_state(qpair, NVME_QPAIR_ENABLED);
                                  /* [한국어] ★ ENABLED 진입 — 이 시점부터 submit_request가 바로 transport로 내려감 */
		while (!STAILQ_EMPTY(&qpair->queued_req)) {
                                  /* [한국어] enable 전 쌓였던 queued_req를 flush. cb_fn이 다시 enqueue 해도 무한루프는 아님 */
			req = STAILQ_FIRST(&qpair->queued_req);
			STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
			if (nvme_qpair_resubmit_request(qpair, req)) {
                                  /* [한국어] resubmit이 -EAGAIN 등 실패를 돌려주면 이번 flush 중단
                                   *         (이미 resubmit이 queued_req 선두에 되돌려 놓음) */
				break;
			}
		}
	}

	/*
	 * When doing a reset, we must disconnect the qpair on the proper core.
	 * Note, reset is the only case where we set the failure reason without
	 * setting the qpair state since reset is done at the generic layer on the
	 * controller thread and we can't disconnect I/O qpairs from the controller
	 * thread.
	 */
	if (spdk_unlikely(qpair->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE &&
			  nvme_qpair_get_state(qpair) == NVME_QPAIR_ENABLED)) {
                                  /* [한국어] transport_failure가 세팅되었는데 아직 상태가 ENABLED이면
                                   *         ctrlr thread가 reset을 시작했고 이 qpair의 소유 thread에서
                                   *         disconnect를 처리해야 함을 의미 */
		/* Don't disconnect PCIe qpairs. They are a special case for reset. */
		if (qpair->ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
                                  /* [한국어] NVMe-oF 등: 여기서 disconnect. PCIe는 ctrlr reset에서 일괄 처리하므로 스킵 */
			nvme_ctrlr_disconnect_qpair(qpair);
		}
		if (qpair->transport_failure_reason == SPDK_NVME_QPAIR_FAILURE_RESET) {
			/*
			 * For multi-process, a synchronous reset may not reconnect
			 * foreign IO qpairs. So we will reconnect them here instead.
			 */
                                  /* [한국어] secondary 프로세스가 동기 reset을 통해 자기 프로세스 밖의 qpair를
                                   *         재연결하지 못하므로, 여기서 직접 재초기화 */
			nvme_ctrlr_reinitialize_io_qpair(qpair->ctrlr, qpair);
		}
		return false;
                                  /* [한국어] reset 경로 — ENABLED 아님을 반환 */
	}

	return nvme_qpair_get_state(qpair) == NVME_QPAIR_ENABLED;
                                  /* [한국어] 최종 상태가 ENABLED이면 true */
}

/*
 * [한국어] nvme_qpair_resubmit_requests - 완료된 개수만큼 queued_req에서 뽑아 재제출.
 *
 * @qpair:        소유 qpair
 * @num_requests: 이번 process_completions 사이클에서 완료된 개수
 *
 * 동작 원리:
 *   - SPDK의 backpressure 설계 — submit이 NOMEM이면 queued_req에 쌓고, 완료가 도착할 때만 재제출.
 *   - 최대 `num_requests`개를 resubmit하므로 outstanding 개수 불변 유지 (flow control).
 *   - ctrlr이 reset 중이면 중단 (reset이 resubmit 자체를 재초기화).
 *   - resubmit_rc != 0 (주로 -EAGAIN)이면 선두 요청이 다시 queued_req로 삽입되므로 이번 호출 종료.
 *
 * 마지막에 `_nvme_qpair_complete_abort_queued_reqs` 호출:
 *   - resubmit 과정에서 cb_fn이 실행되며 추가로 격리된 abort 대상이 있으면 여기서 완료.
 *
 * 호출자: `spdk_nvme_qpair_process_completions` 완료 직후.
 */
void
nvme_qpair_resubmit_requests(struct spdk_nvme_qpair *qpair, uint32_t num_requests)
{
	uint32_t i;
                                  /* [한국어] 루프 카운터 */
	int resubmit_rc;
                                  /* [한국어] resubmit 결과 코드 */
	struct nvme_request *req;
                                  /* [한국어] 선두 request 포인터 */

	assert(num_requests > 0);
                                  /* [한국어] 호출자가 완료 개수 0을 보내는 일은 없어야 함 (호출 전에 이미 필터) */

	for (i = 0; i < num_requests; i++) {
                                  /* [한국어] 최대 num_requests번 반복 — flow control */
		if (qpair->ctrlr->is_resetting) {
                                  /* [한국어] reset 중이면 재제출 중단 (reset이 queued_req를 다시 처리) */
			break;
		}
		if ((req = STAILQ_FIRST(&qpair->queued_req)) == NULL) {
                                  /* [한국어] queued_req가 비었으면 더 제출할 것 없음 */
			break;
		}
		STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
                                  /* [한국어] 선두 pop */
		resubmit_rc = nvme_qpair_resubmit_request(qpair, req);
                                  /* [한국어] _nvme_qpair_submit_request로 재내림. EAGAIN이면 다시 선두에 되돌림 */
		if (spdk_unlikely(resubmit_rc != 0)) {
                                  /* [한국어] EAGAIN/ENXIO 등 실패 — 이번 사이클 종료 */
			NVME_QPAIR_DEBUGLOG(qpair, "Unable to resubmit as many requests as we completed.\n");
			break;
		}
	}

	_nvme_qpair_complete_abort_queued_reqs(qpair);
                                  /* [한국어] resubmit 중 격리된 abort 대상들 일괄 complete */
}

/*
 * [한국어] nvme_complete_register_operations - multi-process NVMe register read/write 완료 큐 처리.
 *
 * SPDK는 primary/secondary 프로세스가 동일 컨트롤러를 공유할 수 있다 (DPDK 런타임).
 * 컨트롤러 레지스터(CC/CSTS 등) 읽기/쓰기는 admin queue를 통해 처리되며, 완료 콜백을
 * "요청한 프로세스"에서 반드시 실행해야 한다(콜백 함수 주소가 프로세스별로 다르기 때문).
 *
 * 동작:
 *   1) ctrlr_lock 획득 (multi-process이므로 spinlock)
 *   2) ctrlr->register_operations에서 내 PID의 완료 ctx만 로컬 리스트 operations로 이동
 *   3) unlock
 *   4) 로컬 리스트를 순회하며 cb_fn 호출 + ctx spdk_free (shared hugepage memory 해제)
 *
 * 이 함수는 admin qpair의 `process_completions`에서만 호출된다.
 *
 * 실행 컨텍스트: admin qpair를 소유한 스레드(보통 master lcore).
 */
static void
nvme_complete_register_operations(struct spdk_nvme_qpair *qpair)
{
	struct nvme_register_completion *ctx, *tmp;
                                  /* [한국어] FOREACH_SAFE용 반복자 쌍 */
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
                                  /* [한국어] 소속 컨트롤러 (register_operations 리스트 소유) */
	STAILQ_HEAD(, nvme_register_completion) operations;
                                  /* [한국어] 로컬 이동용 리스트 — lock 밖에서 cb_fn 호출하기 위함 */

	STAILQ_INIT(&operations);
                                  /* [한국어] 로컬 리스트 초기화 */
	nvme_ctrlr_lock(ctrlr);
                                  /* [한국어] multi-process 공유 리스트 접근 — spinlock */
	STAILQ_FOREACH_SAFE(ctx, &ctrlr->register_operations, stailq, tmp) {
                                  /* [한국어] 전체 순회 — 여러 프로세스의 ctx가 섞여 있을 수 있음 */
		/* We need to make sure we complete the register operation in
		 * the correct process.
		 */
		if (ctx->pid != getpid()) {
                                  /* [한국어] 내 프로세스가 요청한 ctx가 아니면 스킵 — 해당 프로세스가 나중에 처리 */
			continue;
		}
		STAILQ_REMOVE(&ctrlr->register_operations, ctx, nvme_register_completion, stailq);
                                  /* [한국어] 공유 리스트에서 제거 */
		STAILQ_INSERT_TAIL(&operations, ctx, stailq);
                                  /* [한국어] 로컬 리스트에 추가 */
	}
	nvme_ctrlr_unlock(ctrlr);
                                  /* [한국어] 공유 리스트 조작 종료 — cb_fn은 lock 밖에서 안전하게 호출 */

	while (!STAILQ_EMPTY(&operations)) {
                                  /* [한국어] 로컬 리스트 drain */
		ctx = STAILQ_FIRST(&operations);
		STAILQ_REMOVE_HEAD(&operations, stailq);
		if (ctx->cb_fn != NULL) {
                                  /* [한국어] 사용자 콜백 존재 시 호출 — value와 cpl(성공/실패 status) 전달 */
			ctx->cb_fn(ctx->cb_ctx, ctx->value, &ctx->cpl);
		}
		spdk_free(ctx);
                                  /* [한국어] ctx는 DPDK hugepage에 할당되었을 수 있어 spdk_free 사용 (shared 가능) */
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_qpair_get_fd — interrupt-mode용 fd 조회 ★
 *
 * @qpair: 대상 qpair
 * @opts:  이벤트 핸들러 옵션 (out — 트랜스포트가 epoll 설정을 채움)
 * @return: 트랜스포트가 노출하는 fd (예: PCIe의 eventfd/vfio-ioeventfd, RDMA/TCP의 socket fd)
 *
 * polled-mode가 아닌 interrupt-mode SPDK thread가 이 fd를 spdk_fd_group_add로 epoll에 등록.
 * CQ에 엔트리가 도착하면 fd가 readable이 되어 fgrp_wait가 깨어나고 process_completions를 호출.
 *
 * 트랜스포트 독립 추상 — 구현은 nvme_pcie.c의 pcie_qpair_get_fd, nvme_tcp.c의 tcp_qpair_get_fd 등.
 */
int
spdk_nvme_qpair_get_fd(struct spdk_nvme_qpair *qpair, struct spdk_event_handler_opts *opts)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
                                  /* [한국어] 컨트롤러 포인터 — transport vtable 디스패치에 필요 */

	return nvme_transport_qpair_get_fd(ctrlr, qpair, opts);
                                  /* [한국어] 트랜스포트별 구현으로 위임 */
}

/*
 * [한국어] ★★★ 공개 API: spdk_nvme_qpair_process_completions ★★★
 *
 * ★★ SPDK NVMe 드라이버의 "완료 폴링" 진입점. 이 함수가 없으면 SPDK NVMe는 전진하지 않는다. ★★
 *
 * @qpair:           대상 qpair (admin 또는 I/O)
 * @max_completions: 이번 호출에서 처리할 CQE 최대 개수. 0이면 무제한(드라이버 내부 기본값).
 * @return: >=0 처리한 완료 개수, <0 에러 (주로 -ENXIO = 컨트롤러 제거/failed)
 *
 * 호출자 (reactor poller):
 *   - SPDK 애플리케이션의 reactor 루프가 이 함수를 **무한 반복** 호출. CPU를 100% 소비하는
 *     polled-mode 본질. 인터럽트 모드는 epoll fd readable 시 호출.
 *   - poll_group이 있는 경우 `spdk_nvme_poll_group_process_completions`가 멤버 qpair마다 호출.
 *
 * 처리 단계:
 *
 *   [1] Admin 큐 전용 선행 작업
 *       - `nvme_complete_register_operations`: ctrlr CC/CSTS 등 레지스터 접근 완료 콜백.
 *       - `nvme_transport_ctrlr_process_transport_events`: AER, hotplug, keep-alive 등 트랜스포트 이벤트.
 *
 *   [2] 컨트롤러 failed 체크
 *       - ctrlr->is_failed + 제거된 경우 → DESTROYING 전이 + abort all + -ENXIO 반환.
 *       - is_failed이지만 제거는 안 됨 → 단순 -ENXIO 반환 (bdev_nvme가 재시도/failover 판단).
 *
 *   [3] check_enabled 호출
 *       - 상태머신 전이 + reset 감지. ENABLED가 아니면 -ENXIO로 호출자에게 재시도 유도
 *         (단, CONNECTING/DISCONNECTING은 정상 과정이므로 아래 실제 폴링으로 진행).
 *
 *   [4] Error injection 완료 처리
 *       - `err_req_head`에 "do_not_submit" 플래그로 잡혀있던 request들의 timeout이 지나면 manual complete.
 *
 *   [5] ★ 트랜스포트 CQE 드레인 (hot path)
 *       - `nvme_transport_qpair_process_completions(qpair, max)` 호출.
 *         - PCIe: CQ[cq_head].phase 체크, CQE 수확, tracker 찾아서 nvme_request 복원, cb_fn 호출.
 *         - NVMe-oF TCP/RDMA: 소켓/QP에서 capsule 수신 후 CQE 해석.
 *       - 음수 반환은 트랜스포트 에러. admin 큐 에러면 ctrlr를 fail 처리.
 *
 *   [6] delete_after_completion_context 처리
 *       - cb_fn 안에서 이 qpair를 free 요청한 경우. 재진입 방지를 위해 여기서 실제 free 수행.
 *
 *   [7] ★ Resubmit
 *       - 완료된 개수만큼 queued_req에서 pop해 재제출 (flow control).
 *       - 완료 개수 0이면 aborting_queued_req만 처리 (격리된 abort 대상 정리).
 *
 * 실행 컨텍스트:
 *   - qpair를 소유한 SPDK thread 전용. 다른 스레드에서 호출 금지.
 *   - `in_completion_context` 플래그로 "지금 내부에서 cb_fn이 실행 중"임을 표시 —
 *     cb_fn이 이 qpair의 delete를 요청하면 [6]에서 지연 처리.
 *
 * 왜 trasport_qpair_process_completions가 별도인가?
 *   - 트랜스포트(PCIe/RDMA/TCP)마다 CQ 드레인 방식이 완전히 다름:
 *     - PCIe: MMIO 기반 doorbell 갱신 + CQ entry phase bit polling
 *     - RDMA: ibv_poll_cq로 HW queue 드레인
 *     - TCP : recv(2)로 capsule 수신 후 PDU 재조립
 *     이 파일은 공통 로직만 담고 실제 폴링은 트랜스포트에 위임 (전략 패턴).
 */
int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	int32_t ret;
                                  /* [한국어] transport process_completions 반환값 — 수확한 CQE 개수 또는 에러 */
	struct nvme_request *req, *tmp;
                                  /* [한국어] err_req_head 순회용 */

	if (nvme_qpair_is_admin_queue(qpair)) {
                                  /* [한국어] admin 큐만의 선행 작업 */
		/* Complete any pending register operations */
		nvme_complete_register_operations(qpair);
                                  /* [한국어] CC/CSTS 등 레지스터 접근 완료 콜백 드레인 */
		/* Process transport-specific events */
		nvme_transport_ctrlr_process_transport_events(qpair->ctrlr);
                                  /* [한국어] AER, hotplug notification, keep-alive 등 트랜스포트 이벤트 */
	}

	if (spdk_unlikely(qpair->ctrlr->is_failed &&
			  nvme_qpair_get_state(qpair) != NVME_QPAIR_DISCONNECTING)) {
                                  /* [한국어] 컨트롤러가 실패 상태이고 이 qpair도 disconnect 중이 아니면 */
		if (qpair->ctrlr->is_removed) {
                                  /* [한국어] 물리적으로 제거된 경우 (PCIe hotplug 등) */
			nvme_qpair_set_state(qpair, NVME_QPAIR_DESTROYING);
                                  /* [한국어] qpair 파괴 상태로 전이 */
			nvme_qpair_abort_all_queued_reqs(qpair);
                                  /* [한국어] 대기 중인 요청 전부 abort (queued_req + err_req_head + aborting) */
			nvme_transport_qpair_abort_reqs(qpair);
                                  /* [한국어] 트랜스포트 측 outstanding도 abort */
		}
		return -ENXIO;
                                  /* [한국어] "컨트롤러 없음" 에러 — 호출자는 폴링 중단/failover 판단 */
	}

	if (spdk_unlikely(!nvme_qpair_check_enabled(qpair) &&
			  !(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING ||
			    nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING))) {
                                  /* [한국어] ENABLED가 아닌데 CONNECTING/DISCONNECTING도 아니면 (예: reset 중) */
		/*
		 * qpair is not enabled, likely because a controller reset is
		 *  in progress.
		 */
		return -ENXIO;
                                  /* [한국어] 폴링 금지 — 호출자는 reset 완료 후 재시도 */
	}

	/* error injection for those queued error requests */
	if (spdk_unlikely(!STAILQ_EMPTY(&qpair->err_req_head))) {
                                  /* [한국어] error injection 목록이 있는 경우만 */
		STAILQ_FOREACH_SAFE(req, &qpair->err_req_head, stailq, tmp) {
			if (req->pid == getpid() &&
			    spdk_get_ticks() - req->submit_tick > req->timeout_tsc) {
                                  /* [한국어] 내 프로세스가 주입한 것 + 가짜 timeout이 경과한 것 */
				STAILQ_REMOVE(&qpair->err_req_head, req, nvme_request, stailq);
				nvme_qpair_manual_complete_request(qpair, req,
								   req->cpl.status.sct,
								   req->cpl.status.sc, qpair->abort_dnr, true);
                                  /* [한국어] 주입된 SCT/SC 그대로 완료 콜백 호출 — 테스트용 */
			}
		}
	}

	qpair->in_completion_context = 1;
                                  /* [한국어] ★ "지금 완료 경로 실행 중" 플래그 — cb_fn이 qpair free 요청 시 즉시 실행 안 함 */
	ret = nvme_transport_qpair_process_completions(qpair, max_completions);
                                  /* [한국어] ★ 실제 CQ 드레인 — 트랜스포트별 구현 (PCIe phase bit polling 등).
                                   *         반환: 수확한 CQE 개수. cb_fn이 그 안에서 호출됨. */
	if (ret < 0) {
                                  /* [한국어] 트랜스포트 에러 처리 */
		if (ret == -ENXIO && nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
                                  /* [한국어] disconnect 중에 발생한 -ENXIO는 기대되는 에러 — 0으로 보정 */
			ret = 0;
		} else {
                                  /* [한국어] 기타 에러는 로깅 */
			NVME_QPAIR_ERRLOG(qpair, "CQ transport error %d (%s)\n", ret, spdk_strerror(-ret));
                                  /* [한국어] -errno를 사람이 읽는 문자열로 (spdk_strerror는 thread-local) */
			if (nvme_qpair_is_admin_queue(qpair)) {
                                  /* [한국어] admin 큐가 실패하면 컨트롤러 전체가 위험 — fail 처리 유도 */
				nvme_ctrlr_fail(qpair->ctrlr, false);
			}
		}
	}
	qpair->in_completion_context = 0;
                                  /* [한국어] 완료 경로 종료 */
	if (qpair->delete_after_completion_context) {
		/*
		 * A request to delete this qpair was made in the context of this completion
		 *  routine - so it is safe to delete it now.
		 */
                                  /* [한국어] cb_fn 안에서 이 qpair를 free 요청했던 경우 — 이제 안전하게 free */
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		return ret;
                                  /* [한국어] qpair가 파괴됐으므로 더 이상 아무 것도 하지 않고 반환 */
	}

	/*
	 * At this point, ret must represent the number of completions we reaped.
	 * submit as many queued requests as we completed.
	 */
	if (ret > 0) {
                                  /* [한국어] ★ 완료가 있었으면 그 개수만큼 queued_req에서 재제출 — flow control */
		nvme_qpair_resubmit_requests(qpair, ret);
	} else {
                                  /* [한국어] 완료 없음 — 그래도 격리된 abort 대상은 정리 */
		_nvme_qpair_complete_abort_queued_reqs(qpair);
	}

	return ret;
                                  /* [한국어] 수확한 개수를 호출자(reactor/poll_group)에 보고 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_qpair_get_failure_reason ★
 *
 * transport_failure_reason 필드 반환 — reset이나 disconnect 사유 조회.
 *
 * 값 예시(nvme_spec.h 정의):
 *   - SPDK_NVME_QPAIR_FAILURE_NONE       : 정상
 *   - SPDK_NVME_QPAIR_FAILURE_LOCAL      : 로컬 에러 (PCIe link loss, etc)
 *   - SPDK_NVME_QPAIR_FAILURE_REMOTE     : 원격 에러 (NVMe-oF target crash)
 *   - SPDK_NVME_QPAIR_FAILURE_RESET      : reset 요청됨
 *   - SPDK_NVME_QPAIR_FAILURE_UNKNOWN    : 원인 불명
 *
 * bdev_nvme가 failover 판단할 때 사용.
 */
spdk_nvme_qp_failure_reason
spdk_nvme_qpair_get_failure_reason(struct spdk_nvme_qpair *qpair)
{
	return qpair->transport_failure_reason;
                                  /* [한국어] 단순 필드 reader — 이 값은 트랜스포트/상위가 세팅 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_qpair_set_abort_dnr ★
 *
 * abort 시 CQE에 세팅할 DNR 비트를 결정.
 * true=DNR=1 (호출자 재시도 포기), false=DNR=0 (재시도 가능으로 표시).
 *
 * 호출자가 "이후 abort되는 요청을 영구 실패로 만들지"를 qpair 단위로 제어 가능.
 */
void
spdk_nvme_qpair_set_abort_dnr(struct spdk_nvme_qpair *qpair, bool dnr)
{
	qpair->abort_dnr = dnr ? 1 : 0;
                                  /* [한국어] bool → 1비트 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_qpair_is_connected ★
 *
 * qpair가 "제출 가능한 범위의 상태"인지 질의.
 * CONNECTED ≤ state ≤ ENABLED (즉 CONNECTED, ENABLING, ENABLED 모두 포함).
 *
 * 호출자: 상위 레이어가 fast-path 진입 전 qpair 가용성 체크.
 */
bool
spdk_nvme_qpair_is_connected(struct spdk_nvme_qpair *qpair)
{
	return nvme_qpair_get_state(qpair) >= NVME_QPAIR_CONNECTED &&
	       nvme_qpair_get_state(qpair) <= NVME_QPAIR_ENABLED;
                                  /* [한국어] enum 값이 CONNECTED(3) ~ ENABLED(5) 범위 안인지 — 수치 비교 */
}

/*
 * [한국어] ★ nvme_qpair_init - qpair 공통 초기화 (트랜스포트 무관) ★
 *
 * @qpair:        트랜스포트가 할당해 제로-초기화한 qpair 구조체
 * @id:           SQ/CQ 식별자 (0=admin, 1~N=I/O)
 * @ctrlr:        소속 컨트롤러
 * @qprio:        스펙상 큐 우선순위 (URGENT/HIGH/MEDIUM/LOW) — WRR arbitration 시 사용
 * @num_requests: 이 qpair의 request 풀 크기 (일반적으로 ctrlr->opts.io_queue_requests)
 * @async:        비동기 모드 여부 — true면 connect도 비동기로 진행
 * @return:       0=성공, -ENOMEM=req_buf 할당 실패
 *
 * 역할: free_req 풀과 각종 리스트 초기화. 트랜스포트별 init(예: nvme_pcie_qpair_init)이
 * 이 함수를 호출한 후 추가로 SQ/CQ 페이지 할당, doorbell mmap, tracker 배열 등을 설정.
 *
 * 메모리 레이아웃:
 *   - 모든 nvme_request를 64바이트 정렬로 배치 (캐시라인 정렬 + DMA 친화)
 *   - req_size_padded = sizeof(nvme_request) 올림 → 64바이트 배수
 *   - 한 번의 spdk_zmalloc으로 전체 풀 할당 (num_requests+1 개 — 1은 reserved_req)
 *   - 0번 인덱스 → qpair->reserved_req (Fabrics CONNECT 등 크리티컬 경로 대비)
 *   - 1..N 인덱스 → free_req LIFO 풀에 삽입
 *
 * SPDK_MALLOC_SHARE: multi-process에서 secondary가 fork 없이 map 가능하도록.
 */
int
nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
		struct spdk_nvme_ctrlr *ctrlr,
		enum spdk_nvme_qprio qprio,
		uint32_t num_requests, bool async)
{
	struct nvme_request *req;
                                  /* [한국어] 풀 배치 루프에서 각 request 포인터 보관 */
	size_t req_size_padded;
                                  /* [한국어] 64바이트 정렬 크기 */
	uint32_t i;
                                  /* [한국어] 루프 카운터 */

	qpair->id = id;
                                  /* [한국어] SQ/CQ ID */
	qpair->qprio = qprio;
                                  /* [한국어] 큐 우선순위 (Admin은 URGENT 고정, I/O는 호출자 지정) */

	qpair->in_completion_context = 0;
                                  /* [한국어] process_completions 재진입 표시 플래그 */
	qpair->delete_after_completion_context = 0;
                                  /* [한국어] cb_fn 안에서 free 요청 시 지연 삭제용 */
	qpair->no_deletion_notification_needed = 0;
                                  /* [한국어] delete 알림 불필요 플래그 (특수 케이스) */

	qpair->ctrlr = ctrlr;
                                  /* [한국어] 소속 컨트롤러 back-pointer */
	qpair->trtype = ctrlr->trid.trtype;
                                  /* [한국어] 트랜스포트 타입 (PCIe/RDMA/TCP) 캐시 — hot path에서 분기 조건으로 사용 */
	qpair->is_new_qpair = true;
                                  /* [한국어] 첫 connect 여부 — reset 시 이전 outstanding 처리 구분 */
	qpair->async = async;
                                  /* [한국어] 비동기 connect 모드 */
	qpair->fabric_poll_status = NULL;
                                  /* [한국어] fabrics connect 중 진행 상태 (비동기 워크플로우) */
	qpair->num_outstanding_reqs = 0;
                                  /* [한국어] outstanding 카운터 — submit/complete에서 ±1 */

	qpair->poll_group = NULL;
                                  /* [한국어] 아직 poll_group에 가입 안 됨 — 상위가 나중에 add */

	STAILQ_INIT(&qpair->free_req);
                                  /* [한국어] 재사용 request LIFO 풀 초기화 */
	STAILQ_INIT(&qpair->queued_req);
                                  /* [한국어] 제출 대기 FIFO (NOMEM/EAGAIN) */
	STAILQ_INIT(&qpair->aborting_queued_req);
                                  /* [한국어] abort 격리 리스트 */
	TAILQ_INIT(&qpair->err_cmd_head);
                                  /* [한국어] error injection opcode 매핑 테이블 */
	STAILQ_INIT(&qpair->err_req_head);
                                  /* [한국어] do_not_submit 주입된 request 대기열 */

	req_size_padded = (sizeof(struct nvme_request) + 63) & ~(size_t)63;
                                  /* [한국어] 64바이트 경계 올림 — bit-trick: (n+63) & ~63 */

	/* Add one for the reserved_req */
	num_requests++;
                                  /* [한국어] 0번을 reserved_req로 쓰기 위한 +1 */

	qpair->req_buf = spdk_zmalloc(req_size_padded * num_requests, 64, NULL,
				      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
                                  /* [한국어] 전체 풀 한 번에 할당 — 64B align, NUMA 제약 없음, 프로세스 공유 가능 */
	if (qpair->req_buf == NULL) {
                                  /* [한국어] DPDK mempool/hugepage 고갈 */
		NVME_QPAIR_ERRLOG(qpair, "no memory to allocate qpair req_buf with %d request\n", num_requests);
		return -ENOMEM;
	}

	for (i = 0; i < num_requests; i++) {
                                  /* [한국어] 각 request 슬롯 초기 배치 */
		req = (void *)((uintptr_t)qpair->req_buf + i * req_size_padded);
                                  /* [한국어] 포인터 산술 — req_buf의 i번째 슬롯 */

		req->qpair = qpair;
                                  /* [한국어] back-pointer 세팅 */
		if (i == 0) {
                                  /* [한국어] 첫 슬롯은 reserved — 크리티컬 경로(Fabrics CONNECT 등)용 */
			qpair->reserved_req = req;
		} else {
                                  /* [한국어] 나머지는 free 풀에 LIFO로 삽입 — 캐시 hot path 선호 */
			STAILQ_INSERT_HEAD(&qpair->free_req, req, stailq);
		}
	}

	return 0;
                                  /* [한국어] 성공 */
}

/*
 * [한국어] nvme_qpair_complete_error_reqs - err_req_head의 모든 request를 (주입된 에러로) 완료.
 *
 * error injection으로 do_not_submit 설정된 요청들을 한 번에 complete.
 * deinit/reset 시 대기 중인 인공 에러 request를 정리하는 용도.
 */
void
nvme_qpair_complete_error_reqs(struct spdk_nvme_qpair *qpair)
{
	struct nvme_request		*req;
                                  /* [한국어] 반복자 */

	while (!STAILQ_EMPTY(&qpair->err_req_head)) {
                                  /* [한국어] 리스트가 빌 때까지 */
		req = STAILQ_FIRST(&qpair->err_req_head);
		assert(req->pid == getpid());
                                  /* [한국어] 내 프로세스가 주입한 것만 있어야 함 (multi-process 위반 체크) */
		STAILQ_REMOVE_HEAD(&qpair->err_req_head, stailq);
		nvme_qpair_manual_complete_request(qpair, req,
						   req->cpl.status.sct,
						   req->cpl.status.sc, qpair->abort_dnr, true);
                                  /* [한국어] 주입된 SCT/SC 그대로 완료 */
	}
}

/*
 * [한국어] nvme_qpair_deinit - qpair 공통 해제 (트랜스포트 무관).
 *
 * 해제 순서:
 *   1) queued_req abort (장치로 못 나간 대기 요청)
 *   2) aborting_queued_req 정리 (abort 격리 리스트)
 *   3) err_req_head 정리 (error injection 대기)
 *   4) err_cmd_head free (opcode→에러 매핑 테이블)
 *   5) req_buf free (전체 풀)
 *
 * 이 함수를 호출하기 전에 트랜스포트가 outstanding(tracker 등)을 먼저 abort해야 함.
 * 호출자: `nvme_pcie_qpair_destroy` 등 트랜스포트별 destroy 함수.
 */
void
nvme_qpair_deinit(struct spdk_nvme_qpair *qpair)
{
	struct nvme_error_cmd *cmd, *entry;
                                  /* [한국어] err_cmd 해제 루프용 */

	assert(!qpair->fabric_poll_status);
                                  /* [한국어] fabrics 비동기 connect 진행 중 상태에서 deinit 금지 */

	nvme_qpair_abort_queued_reqs(qpair);
                                  /* [한국어] queued_req 일괄 abort */
	_nvme_qpair_complete_abort_queued_reqs(qpair);
                                  /* [한국어] abort 격리 리스트도 정리 */
	nvme_qpair_complete_error_reqs(qpair);
                                  /* [한국어] error injection 대기도 정리 */

	TAILQ_FOREACH_SAFE(cmd, &qpair->err_cmd_head, link, entry) {
                                  /* [한국어] err_cmd 매핑 테이블 전부 free */
		TAILQ_REMOVE(&qpair->err_cmd_head, cmd, link);
		spdk_free(cmd);
                                  /* [한국어] SPDK_MALLOC_DMA로 할당된 구조체 해제 */
	}

	spdk_free(qpair->req_buf);
                                  /* [한국어] 전체 request 풀 한 번에 해제 */
}

/*
 * [한국어] ★★★ _nvme_qpair_submit_request — SPDK NVMe 제출 경로의 실제 구현 ★★★
 *
 * 이 함수는 한 개의 nvme_request를 실제 트랜스포트로 내려보내는 일을 한다.
 * `nvme_qpair_submit_request`(외부 진입)는 간단한 큐잉 로직만 있고, 제출의 본질은 모두 여기.
 *
 * @qpair: 대상 qpair (ENABLED 상태여야 본체 제출 가능)
 * @req:   제출할 nvme_request — _nvme_ns_cmd_rw 등이 이미 SQE 채웠음
 * @return: 0=성공, -EAGAIN=reset 중(재큐 필요), -ENXIO=disconnect됨, -ENOMEM=tracker 고갈
 *
 * 처리 순서 (hot path 그대로):
 *
 *   [1] check_enabled 호출 — 상태 전이 + reset 감지.
 *       DISCONNECTED/DISCONNECTING/DESTROYING이면 children 전체 free하고 -ENXIO.
 *
 *   [2] Split 요청 처리 (req->num_children > 0)
 *       - 이 req는 "parent" — 직접 제출하지 않고 children을 하나씩 재귀 submit.
 *       - 한 child 실패 시 나머지는 free (첫 실패 이후는 제출 안 함).
 *       - 이미 일부 child가 장치로 갔으면 나중에 parent가 완료됐을 때
 *         INTERNAL_DEVICE_ERROR 상태로 표시해 호출자에게 실패 알림.
 *
 *   [3] Error injection 매칭
 *       - err_cmd_head 순회, do_not_submit 플래그 set + opcode 일치 + err_count>0 인 항목이면:
 *         장치에 보내지 않고 err_req_head에 추가 → timeout 경과 후 process_completions가 manual complete.
 *
 *   [4] ctrlr failed 체크 — 실패했으면 즉시 -ENXIO.
 *
 *   [5] submit_tick 기록 (timeout 추적용)
 *
 *   [6] ★ 상태 확인 + 트랜스포트 제출:
 *       - ENABLED → `nvme_transport_qpair_submit_request` 호출 (실제 SQ 기록 + doorbell)
 *       - 예외: Fabrics CONNECT은 CONNECTING 상태에서도 허용 (fabric connect로 ENABLED 도달해야 하므로)
 *       - 그 외 (ENABLING/CONNECTED) → -EAGAIN 반환 → 호출자가 queued_req에 큐잉.
 *
 *   [7] 성공 시 debug log + req->queued=false 설정.
 *   [8] -EAGAIN 그대로 전파.
 *   [9] 기타 실패 (error label):
 *       - parent child 관계 정리
 *       - 이미 queued_req에서 나온 request라면 수동 complete로 콜백 호출
 *       - 아니면 cleanup_user_req + free_request로 풀에 반환
 */
static inline int
_nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	int			rc = 0;
                                  /* [한국어] 최종 반환값 — 단계마다 갱신 */
	struct nvme_request	*child_req, *tmp;
                                  /* [한국어] split된 parent의 children 순회 */
	struct nvme_error_cmd	*cmd;
                                  /* [한국어] error injection 테이블 엔트리 */
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
                                  /* [한국어] ctrlr 포인터 캐시 — failed/timeout_enabled 체크에 사용 */
	bool			child_req_failed = false;
                                  /* [한국어] split 경로에서 한 child가 실패했는지 */

	nvme_qpair_check_enabled(qpair);
                                  /* [한국어] ★ 상태머신 훅 호출 — 필요 시 CONNECTED→ENABLED 전이 + flush 수행 */

	if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTED ||
			  nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING ||
			  nvme_qpair_get_state(qpair) == NVME_QPAIR_DESTROYING)) {
                                  /* [한국어] qpair가 "종결" 상태면 이 요청은 포기 */
		TAILQ_FOREACH_SAFE(child_req, &req->children, child_tailq, tmp) {
                                  /* [한국어] split 요청이면 children도 전부 해제 */
			nvme_request_remove_child(req, child_req);
			nvme_request_free_children(child_req);
			nvme_free_request(child_req);
		}

		rc = -ENXIO;
                                  /* [한국어] "장치/큐 없음" 에러 */
		goto error;
	}

	if (req->num_children) {
		/*
		 * This is a split (parent) request. Submit all of the children but not the parent
		 * request itself, since the parent is the original unsplit request.
		 */
                                  /* [한국어] ★ split parent 처리 — parent는 직접 SQ에 안 들어가고 children만 제출
                                   *         완료는 children 전체가 complete되어야 parent가 complete됨 */
		TAILQ_FOREACH_SAFE(child_req, &req->children, child_tailq, tmp) {
			if (spdk_likely(!child_req_failed)) {
                                  /* [한국어] 첫 실패 전까지는 children을 하나씩 제출 */
				rc = nvme_qpair_submit_request(qpair, child_req);
                                  /* [한국어] 외부 진입 함수 호출 — 재귀적으로 여기로 돌아오지만
                                   *         child는 num_children==0이므로 이 분기 재진입 없음 */
				if (spdk_unlikely(rc != 0)) {
                                  /* [한국어] child 하나라도 실패 → 이후 children은 제출 중단 */
					child_req_failed = true;
				}
			} else { /* free remaining child_reqs since one child_req fails */
                                  /* [한국어] 나머지 children 해제 (제출 안 함) */
				nvme_request_remove_child(req, child_req);
				nvme_request_free_children(child_req);
				nvme_free_request(child_req);
			}
		}

		if (spdk_unlikely(child_req_failed)) {
			/* part of children requests have been submitted,
			 * return success since we must wait for those children to complete,
			 * but set the parent request to failure.
			 */
                                  /* [한국어] 일부 children이 이미 장치로 나갔다면 그들의 완료를 기다려야 하므로
                                   *         호출자에게는 0을 반환하되, parent CPL에 INTERNAL_DEVICE_ERROR 세팅
                                   *         → 마지막 child complete 시 parent의 에러 상태가 호스트 cb_fn에 전달 */
			if (req->num_children) {
				req->cpl.status.sct = SPDK_NVME_SCT_GENERIC;
				req->cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				return 0;
			}
                                  /* [한국어] children 개수가 0이면 (아무것도 제출 못 했음) error 경로로 */
			goto error;
		}

		return rc;
                                  /* [한국어] children 전부 성공 — 마지막 rc 반환 (보통 0) */
	}

	/* queue those requests which matches with opcode in err_cmd list */
	if (spdk_unlikely(!TAILQ_EMPTY(&qpair->err_cmd_head))) {
                                  /* [한국어] error injection 목록 있는 경우만 */
		TAILQ_FOREACH(cmd, &qpair->err_cmd_head, link) {
			if (!cmd->do_not_submit) {
                                  /* [한국어] do_not_submit=false인 항목(단순 통계용)은 제출 경로에서 스킵 */
				continue;
			}

			if ((cmd->opc == req->cmd.opc) && cmd->err_count) {
                                  /* [한국어] opcode 일치하고 아직 주입 남아 있으면 */
				/* add to error request list and set cpl */
				req->timeout_tsc = cmd->timeout_tsc;
                                  /* [한국어] 언제 완료시킬지 타임아웃 */
				req->submit_tick = spdk_get_ticks();
                                  /* [한국어] 제출 시각 저장 */
				req->cpl.status.sct = cmd->status.sct;
				req->cpl.status.sc = cmd->status.sc;
                                  /* [한국어] 주입할 에러 상태 복사 */
				STAILQ_INSERT_TAIL(&qpair->err_req_head, req, stailq);
                                  /* [한국어] err_req_head에 추가 — process_completions가 timeout 경과 후 complete */
				cmd->err_count--;
                                  /* [한국어] 주입 개수 감소 (0이 되면 다음 요청은 정상 처리) */
				return 0;
                                  /* [한국어] 제출 성공처럼 반환 (실제로는 가짜 완료 대기 중) */
			}
		}
	}

	if (spdk_unlikely(ctrlr->is_failed)) {
                                  /* [한국어] 컨트롤러 실패 — 제출 거부 */
		rc = -ENXIO;
		goto error;
	}

	/* assign submit_tick before submitting req to specific transport */
	if (ctrlr->timeout_enabled) {
                                  /* [한국어] timeout 감시 기능이 켜져 있으면 tick 기록 */
		if (req->submit_tick == 0) { /* req submitted for the first time */
                                  /* [한국어] 재제출이 아닌 첫 제출이면 */
			req->submit_tick = spdk_get_ticks();
			req->timed_out = false;
                                  /* [한국어] 재진입 초기화 */
		}
	} else {
		req->submit_tick = 0;
                                  /* [한국어] timeout 기능 꺼짐 — tick 불필요 */
	}

	/* Allow two cases:
	 * 1. NVMe qpair is enabled.
	 * 2. Always allow fabrics commands through - these get
	 * the controller out of reset state.
	 */
                                  /* [한국어] ★ 제출 가능 조건:
                                   *         (a) qpair가 ENABLED 상태 (정상 hot path), 또는
                                   *         (b) Fabrics capsule이고 현재 CONNECTING 중 (CONNECT 커맨드가 통해야 ENABLED 도달) */
	if (spdk_likely(nvme_qpair_get_state(qpair) == NVME_QPAIR_ENABLED) ||
	    (req->cmd.opc == SPDK_NVME_OPC_FABRIC &&
	     nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING)) {
		rc = nvme_transport_qpair_submit_request(qpair, req);
                                  /* [한국어] ★★ 실제 트랜스포트 제출 — PCIe면 SQ 기록 + doorbell MMIO write.
                                   *         이 한 줄이 CPU→장치 첫 신호 지점. */
	} else {
		/* The controller is being reset - queue this request and
		 *  submit it later when the reset is completed.
		 */
                                  /* [한국어] reset 중 — -EAGAIN 반환하면 외부 진입이 queued_req에 enqueue */
		return -EAGAIN;
	}

	if (spdk_likely(rc == 0)) {
                                  /* [한국어] 제출 성공 */
		if (SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
                                  /* [한국어] 디버그 플래그가 켜져 있으면 SQE 문자열 로그 */
			spdk_nvme_qpair_print_command(qpair, &req->cmd);
		}
		req->queued = false;
                                  /* [한국어] queued_req에서 나온 게 아님을 확실히 — 이후 오류 경로 구분에 사용 */
		return 0;
	}

	if (rc == -EAGAIN) {
                                  /* [한국어] 트랜스포트가 자원 부족(NOMEM 등)으로 -EAGAIN 반환 — 그대로 전파 */
		return -EAGAIN;
	}

error:
                                  /* [한국어] 에러 공통 처리 레이블 */
	if (req->parent != NULL) {
                                  /* [한국어] split의 child인 경우 parent에서 떼어냄 (parent 정합성 유지) */
		nvme_request_remove_child(req->parent, req);
	}

	/* The request is from queued_req list we should trigger the callback from caller */
	if (spdk_unlikely(req->queued)) {
                                  /* [한국어] queued_req에서 꺼내왔던 request — 호출자가 이미 기다리고 있으므로
                                   *         수동 complete로 콜백 호출 필요 */
		if (rc == -ENXIO) {
			nvme_qpair_manual_complete_request(qpair, req, SPDK_NVME_SCT_GENERIC,
							   SPDK_NVME_SC_ABORTED_SQ_DELETION,
							   qpair->abort_dnr, true);
                                  /* [한국어] qpair 제거로 인한 abort */
		} else {
			nvme_qpair_manual_complete_request(qpair, req, SPDK_NVME_SCT_GENERIC,
							   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR,
							   true, true);
                                  /* [한국어] 일반 내부 에러 (dnr=true) */
		}
		return rc;
	}

	nvme_cleanup_user_req(req);
                                  /* [한국어] user_copy 버퍼 등 정리 (accel_sequence 해제 포함) */
	nvme_free_request(req);
                                  /* [한국어] request를 free_req 풀로 반환 */

	return rc;
                                  /* [한국어] 에러 코드 원본 반환 */
}

/*
 * [한국어] ★★ 외부 진입점: nvme_qpair_submit_request ★★
 *
 * nvme_ns_cmd.c의 `nvme_qpair_submit_request(qpair, req)` 호출이 여기로 온다.
 *
 * 이 함수의 역할은 얇다:
 *   (A) 이미 queued_req에 대기 중인 요청이 있고, 이번 req가 split 없는 leaf이면
 *       → FIFO 순서 유지를 위해 곧바로 queued_req 뒤에 enqueue만 하고 리턴.
 *       (이전 요청들이 뒤늦게 처리되기 전에 나중 요청이 앞질러 나가지 않도록)
 *       예외: FABRIC + CONNECTING 조합은 우선 처리 (CONNECT가 먼저 가야 큐가 ENABLED 됨).
 *   (B) 그 외 경우는 `_nvme_qpair_submit_request`로 실제 제출.
 *       EAGAIN 반환이면 이번 요청을 queued_req에 enqueue하고 성공(0) 리턴.
 *
 * split된 parent 요청은 (A) 분기에 들어가지 않아도 _submit 내부에서 children을 재귀적으로
 * 여기에 다시 보내므로, 각 child에 대해서는 정상적인 큐잉 로직이 적용된다.
 */
int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	int rc;
                                  /* [한국어] 반환값 */

	if (spdk_unlikely(!STAILQ_EMPTY(&qpair->queued_req) && req->num_children == 0)) {
                                  /* [한국어] 대기 중인 요청이 있고 이번 req는 split 없는 leaf */
		/*
		 * Requests that have no children should be sent to the transport after all
		 * currently queued requests. Requests with children will be split and go back
		 * through this path.  We need to make an exception for the fabrics commands
		 * while the qpair is connecting to be able to send the connect command
		 * asynchronously.
		 */
		if (req->cmd.opc != SPDK_NVME_OPC_FABRIC ||
		    nvme_qpair_get_state(qpair) != NVME_QPAIR_CONNECTING) {
                                  /* [한국어] FABRIC+CONNECTING이 아닌 일반 요청은 FIFO 순서 유지 */
			STAILQ_INSERT_TAIL(&qpair->queued_req, req, stailq);
                                  /* [한국어] queued_req 꼬리에 enqueue */
			req->queued = true;
                                  /* [한국어] "큐에서 나온 것"임을 표시 — 에러 경로가 manual complete 여부 판단에 사용 */
			return 0;
                                  /* [한국어] 바로 리턴 — 실제 제출은 process_completions가 완료 처리 후 resubmit */
		}
                                  /* [한국어] FABRIC+CONNECTING이면 fallthrough해서 즉시 제출 시도 */
	}

	rc = _nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] ★ 실제 제출 호출 */
	if (rc == -EAGAIN) {
                                  /* [한국어] 트랜스포트가 자원 부족으로 재시도 요청 */
		STAILQ_INSERT_TAIL(&qpair->queued_req, req, stailq);
                                  /* [한국어] queued_req 꼬리에 enqueue — 다음 완료 때 resubmit */
		req->queued = true;
		rc = 0;
                                  /* [한국어] 호출자에게는 "성공(큐잉됨)"으로 보고 */
	}

	return rc;
                                  /* [한국어] 0=성공 or 성공처럼 큐잉됨, 음수=실제 실패 */
}

/*
 * [한국어] nvme_qpair_resubmit_request - queued_req에서 pop한 요청을 다시 제출.
 *
 * - resubmit 대상은 절대 split parent가 아니어야 함 (children은 각자 독립 관리).
 *   이는 "완료 1개 ↔ 재제출 1개" 원칙을 보장하기 위함.
 * - _submit이 -EAGAIN을 돌려주면 큐의 **머리**에 다시 넣어 순서 보존 (insert_tail 아님).
 *
 * 호출자: `nvme_qpair_resubmit_requests` (완료 후), `nvme_qpair_check_enabled` (enable drain).
 */
static int
nvme_qpair_resubmit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	int rc;
                                  /* [한국어] 반환값 */

	/*
	 * We should never have a request with children on the queue.
	 * This is necessary to preserve the 1:1 relationship between
	 * completions and resubmissions.
	 */
	assert(req->num_children == 0);
                                  /* [한국어] queued_req에는 leaf request만 들어옴 — 이 불변식이 깨지면 버그 */
	assert(req->queued);
                                  /* [한국어] queued 플래그가 세팅되어 있어야 정상 */
	rc = _nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 실제 제출 시도 */
	if (spdk_unlikely(rc == -EAGAIN)) {
                                  /* [한국어] 자원 부족 시 queued_req 머리로 되돌려 순서 보존 */
		STAILQ_INSERT_HEAD(&qpair->queued_req, req, stailq);
	}

	return rc;
                                  /* [한국어] 호출자(resubmit_requests)가 rc!=0이면 그 루프 중단 */
}

/*
 * [한국어] nvme_qpair_abort_all_queued_reqs - 모든 호스트 측 대기열을 일괄 abort.
 *
 * 호출되는 맥락: reset / disconnect / destroy 등 qpair를 깨끗하게 비워야 할 때.
 *
 * 호출 순서:
 *   1) err_req_head 완료 (error injection 대기)
 *   2) queued_req abort (대기 중)
 *   3) aborting_queued_req 완료 (격리된 abort)
 *   4) admin queue이면 ctrlr 레벨 abort queue도 처리 (keep-alive/async abort 등)
 */
void
nvme_qpair_abort_all_queued_reqs(struct spdk_nvme_qpair *qpair)
{
	nvme_qpair_complete_error_reqs(qpair);
                                  /* [한국어] 주입된 인공 에러 요청 정리 */
	nvme_qpair_abort_queued_reqs(qpair);
                                  /* [한국어] 대기 중 요청 일괄 SQ_DELETION abort */
	_nvme_qpair_complete_abort_queued_reqs(qpair);
                                  /* [한국어] 격리된 abort 대상도 완료 */
	if (nvme_qpair_is_admin_queue(qpair)) {
                                  /* [한국어] admin 전용: ctrlr에 쌓인 추가 abort 요청도 정리 */
		nvme_ctrlr_abort_queued_aborts(qpair->ctrlr);
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_qpair_add_cmd_error_injection ★
 *
 * 특정 opcode에 인공 에러를 주입하는 테스트/디버그 유틸리티.
 *
 * @ctrlr:         대상 컨트롤러
 * @qpair:         대상 qpair (NULL이면 admin 큐에 주입)
 * @opc:           대상 opcode (예: SPDK_NVME_OPC_READ)
 * @do_not_submit: true면 장치로 보내지 않고 timeout 경과 후 가짜 완료,
 *                 false면 장치로 보내되 tracer로 통계만
 * @timeout_in_us: 가짜 완료 지연 (마이크로초)
 * @err_count:     주입할 횟수 (0이 될 때까지 매칭되는 요청마다 에러 생성)
 * @sct, @sc:      주입할 SCT/SC (e.g. MEDIA_ERROR/UNRECOVERED_READ_ERROR)
 * @return:        0=성공, -ENOMEM=entry 할당 실패
 *
 * 동작: err_cmd_head 리스트에서 동일 opcode 엔트리를 찾아 갱신하거나, 없으면 새로 할당해 추가.
 *       이후 `_nvme_qpair_submit_request`가 제출 전 이 리스트를 조회해 매칭되는 opcode면
 *       do_not_submit=true 시 err_req_head로 격리.
 *
 * Admin 큐 조작 시 `ctrlr_lock`이 필요한 이유: admin queue는 multi-process 공유 가능.
 */
int
spdk_nvme_qpair_add_cmd_error_injection(struct spdk_nvme_ctrlr *ctrlr,
					struct spdk_nvme_qpair *qpair,
					uint8_t opc, bool do_not_submit,
					uint64_t timeout_in_us,
					uint32_t err_count,
					uint8_t sct, uint8_t sc)
{
	struct nvme_error_cmd *entry, *cmd = NULL;
                                  /* [한국어] entry=순회자, cmd=찾거나 새로 만든 대상 */
	int rc = 0;

	if (qpair == NULL) {
                                  /* [한국어] qpair 미지정 → admin 큐를 기본값으로 */
		qpair = ctrlr->adminq;
		nvme_ctrlr_lock(ctrlr);
                                  /* [한국어] admin 큐 변경이므로 ctrlr lock 획득 (multi-process 공유) */
	}

	TAILQ_FOREACH(entry, &qpair->err_cmd_head, link) {
                                  /* [한국어] 기존 엔트리 중 같은 opcode 찾기 */
		if (entry->opc == opc) {
			cmd = entry;
			break;
		}
	}

	if (cmd == NULL) {
                                  /* [한국어] 새 엔트리 할당 필요 */
		cmd = spdk_zmalloc(sizeof(*cmd), 64, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
                                  /* [한국어] 64B 정렬 DMA-safe 할당 (DPDK 풀에서) */
		if (!cmd) {
			rc = -ENOMEM;
			goto out;
		}
		TAILQ_INSERT_TAIL(&qpair->err_cmd_head, cmd, link);
                                  /* [한국어] 리스트 꼬리에 추가 */
	}

	cmd->do_not_submit = do_not_submit;
	cmd->err_count = err_count;
	cmd->timeout_tsc = timeout_in_us * spdk_get_ticks_hz() / 1000000ULL;
                                  /* [한국어] us → ticks 변환 (ticks_hz = 초당 tick) */
	cmd->opc = opc;
	cmd->status.sct = sct;
	cmd->status.sc = sc;
out:
	if (nvme_qpair_is_admin_queue(qpair)) {
                                  /* [한국어] admin 큐였으면 lock 해제 */
		nvme_ctrlr_unlock(ctrlr);
	}

	return rc;
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_qpair_remove_cmd_error_injection ★
 *
 * 주입된 에러 엔트리를 opcode 단위로 제거.
 * qpair=NULL이면 admin queue에서 제거.
 */
void
spdk_nvme_qpair_remove_cmd_error_injection(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair,
		uint8_t opc)
{
	struct nvme_error_cmd *cmd, *entry;
                                  /* [한국어] FOREACH_SAFE용 */

	if (qpair == NULL) {
                                  /* [한국어] qpair 미지정 → admin */
		qpair = ctrlr->adminq;
		nvme_ctrlr_lock(ctrlr);
                                  /* [한국어] multi-process 공유 락 */
	}

	TAILQ_FOREACH_SAFE(cmd, &qpair->err_cmd_head, link, entry) {
                                  /* [한국어] 같은 opcode 찾아서 제거 */
		if (cmd->opc == opc) {
			TAILQ_REMOVE(&qpair->err_cmd_head, cmd, link);
			spdk_free(cmd);
                                  /* [한국어] DPDK 풀로 반환 */
			break;
                                  /* [한국어] opcode당 엔트리 1개 가정 — 첫 매칭만 제거 */
		}
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_ctrlr_unlock(ctrlr);
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_qpair_get_id ★
 *
 * qpair의 NVMe SQ/CQ ID(1~65535) 반환. 0은 Admin 큐.
 * 디버그 로그/트레이스에서 I/O가 어느 큐를 타는지 표시.
 */
uint16_t
spdk_nvme_qpair_get_id(struct spdk_nvme_qpair *qpair)
{
	return qpair->id;
                                  /* [한국어] 단순 reader */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_qpair_get_num_outstanding_reqs ★
 *
 * 장치에 제출되어 아직 완료되지 않은 request 개수 (QD; Queue Depth).
 * bdev_nvme가 latency/throughput 측정 시 활용.
 *
 * 주의: num_outstanding_reqs는 `nvme_qpair_submit_request` 성공 시 +1, 완료 시 -1로 증감.
 *       split된 parent request는 children 집계에 따라 +N되는 것이 아니라 "실제 device SQE" 단위로 증가.
 */
uint32_t
spdk_nvme_qpair_get_num_outstanding_reqs(struct spdk_nvme_qpair *qpair)
{
	return qpair->num_outstanding_reqs;
                                  /* [한국어] 단순 reader (atomic 아님 — qpair 소유 스레드만 변경) */
}
