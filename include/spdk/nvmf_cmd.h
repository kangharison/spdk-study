/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] NVMe-oF target 사용자 정의 명령(custom command) 핸들러 등록 API (nvmf_cmd.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK NVMe-over-Fabrics target 이 처리하는 admin/IO 명령 흐름에 외부 사용자
 * 모듈이 끼어들 수 있도록 하는 "확장 후크(extension hook)" API 를 정의한다. 구체적으로
 * (1) admin opcode 별로 사용자 콜백을 등록(spdk_nvmf_set_custom_admin_cmd_hdlr), (2) 특정
 * admin opcode 를 backing bdev(NVMe 드라이브)로 그대로 패스스루(spdk_nvmf_set_passthru_admin_cmd),
 * (3) custom 핸들러 안에서 호출하기 위한 보조 함수(요청에서 ctrlr/subsystem/cmd/cpl/iovec
 * 추출, bdev 접근, identify 페이지 채우기, abort) 를 노출한다. NVMe-oF 표준 외 vendor-specific
 * opcode, 또는 표준 opcode 의 재정의를 구현할 때 사용된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호스트(initiator)가 capsule 형태로 보낸 admin command 는 transport(RDMA/TCP/FC/vfio_user)
 * 가 받아 spdk_nvmf_request 로 만들고, lib/nvmf/ctrlr.c 의 nvmf_ctrlr_process_admin_cmd() 가
 * 디스패치한다. 이때 사용자가 등록한 custom 핸들러(g_nvmf_custom_admin_cmd_hdlrs[opc]) 가
 * 먼저 호출되며, -1 을 반환하면 SPDK 기본 처리로 폴백한다. SPDK_NVMF_REQUEST_EXEC_STATUS_*
 * 를 반환하면 곧바로 완료 또는 비동기 대기 상태로 진입한다. 즉 이 파일의 API 는 nvmf
 * 의 admin 디스패치 단계에서 "1순위 가로채기" 지점을 제공하며, 호출 컨텍스트는 해당
 * qpair 가 바인딩된 spdk_thread(reactor) 이다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/nvmf.h (spdk_nvmf_ctrlr/subsystem/request 전방선언), spdk/bdev.h
 *   (spdk_bdev/desc/io_channel), spdk/nvme_spec.h (cmd/cpl/log_page/identify 자료구조).
 * - 사용처: 외부 모듈(예: vendor 의 plug-in), 내부적으로는 lib/nvmf/ctrlr.c 의 admin
 *   디스패처가 g_nvmf_custom_admin_cmd_hdlrs 테이블을 lookup. spdk_nvmf_set_passthru_admin_cmd
 *   는 lib/nvmf/ctrlr_bdev.c 의 패스스루 경로(bdev_nvme passthru) 와 결합.
 * - 데이터 흐름: 호스트 SQE → transport capsule → spdk_nvmf_request(cmd, rsp, iov,
 *   length, ctrlr, qpair) → custom 핸들러 → (선택) spdk_nvmf_bdev_ctrlr_nvme_passthru_admin
 *   으로 bdev 에 위임 → bdev 완료 시 spdk_nvmf_nvme_passthru_cmd_cb → 최종 CQE 회신.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_nvmf_set_custom_admin_cmd_hdlr(opc, hdlr): admin opcode 별 사용자 핸들러 등록.
 * - spdk_nvmf_set_passthru_admin_cmd(opc, nsid): admin opcode 를 backing NVMe 드라이브로
 *   그대로 패스스루(투명 전달).
 * - spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(): custom 핸들러 안에서 명시적으로 bdev 에
 *   admin 명령을 보내는 헬퍼.
 * - spdk_nvmf_ctrlr_identify_ctrlr/_ns/_iocs_specific/_iocs_independent: identify 응답을
 *   채우는 헬퍼 — custom 핸들러에서 부분 재정의할 때 사용.
 * - spdk_nvmf_request_get_*: 요청 객체에서 ctrlr/subsystem/cmd/cpl/req_to_abort 를 추출.
 * - spdk_nvmf_request_copy_to/from_buf: capsule iovec ↔ 일반 버퍼 메모리 복사.
 * - spdk_nvmf_custom_cmd_hdlr typedef: 사용자 콜백 시그니처(int(req)). 반환값 의미는
 *   spdk_nvmf_request_exec_status, 또는 처리하지 않음을 의미하는 -1.
 * - spdk_nvmf_request_exec_status enum: COMPLETE(즉시 완료), ASYNCHRONOUS(나중에 완료
 *   콜백을 통해 회신).
 */

#ifndef SPDK_NVMF_CMD_H_
#define SPDK_NVMF_CMD_H_

#include "spdk/stdinc.h"
/* [한국어] 표준 C 헤더 묶음(stddef/stdint/stdbool/string 등) — uint8_t/size_t/NULL 등의
 * 기본 타입을 사용하기 위해 포함. */

#include "spdk/nvmf.h"
/* [한국어] NVMe-oF target 의 1차 공개 API. spdk_nvmf_ctrlr / spdk_nvmf_subsystem /
 * spdk_nvmf_request 의 전방선언이 들어 있어, 본 헤더에서 이 포인터들을 인자/반환으로
 * 다룰 수 있게 한다. 사용자 핸들러는 이 객체들을 통해 host/subsystem 식별을 한다. */

#include "spdk/bdev.h"
/* [한국어] block device 추상화 — spdk_bdev / spdk_bdev_desc / spdk_io_channel.
 * spdk_nvmf_bdev_ctrlr_nvme_passthru_admin / spdk_nvmf_request_get_bdev 등에서 backing
 * 디바이스를 다룬다. NVMe-oF target 의 namespace 는 결국 bdev 1개에 1:1 매핑된다. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 컴파일러가 본 헤더를 포함할 때 C linkage(이름 맹글링 회피) 를 강제. */
#endif

/*
 * [한국어]
 * spdk_nvmf_request_exec_status - NVMe-oF custom/내부 명령 핸들러의 반환값 타입.
 *
 * SPDK 의 admin 디스패처는 이 enum 값을 받아 후속 처리를 결정한다. 핸들러가 동기적으로
 * 결과를 채워 반환할 수도 있고(COMPLETE), bdev/transport 콜백을 기다려야 할 수도 있다
 * (ASYNCHRONOUS). 비동기 경로에서는 핸들러가 반환한 시점에 아직 spdk_nvmf_request_complete()
 * 가 호출되지 않은 상태이며, 이후 콜백 컨텍스트에서 완료 처리가 일어난다.
 */
enum spdk_nvmf_request_exec_status {
	SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE,
	/* [한국어] 핸들러가 동기적으로 모든 처리를 마쳤음을 의미.
	 * 설정자: custom/내부 admin 핸들러가 cpl(rsp) 필드까지 채운 뒤 이 값을 반환.
	 * 읽는 자: lib/nvmf/ctrlr.c 의 admin 디스패처 — 이 값을 받으면 즉시
	 *   spdk_nvmf_request_complete()(또는 transport 의 완료 회신)를 호출.
	 * 값 범위: 핸들러 함수의 정상 동기 경로에서 사용.
	 * 동기화: qpair 가 바인딩된 spdk_thread 단일 스레드에서만 다뤄지므로 별도 락 불필요. */

	SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS,
	/* [한국어] 핸들러가 비동기 경로(bdev I/O, AER pending 등)로 진입했음을 의미.
	 * 설정자: 핸들러가 spdk_bdev_*() 호출 등으로 작업을 위임한 직후 반환.
	 * 읽는 자: 디스패처 — 이 값을 받으면 즉시 완료시키지 않고 기다림. 나중에 콜백
	 *   컨텍스트에서 spdk_nvmf_request_complete() 가 명시적으로 호출되어야 한다.
	 * 값 범위: bdev passthru / log_page 비동기 채움 / AER 보류 등에서 사용.
	 * 동기화: 비동기 콜백이 동일 spdk_thread 에서 실행되도록 transport 가 보장. */
};

/**
 * Fills the identify controller attributes for the specified controller
 *
 * \param ctrlr The NVMe-oF controller
 * \param cdata The filled in identify controller attributes
 * \return \ref spdk_nvmf_request_exec_status
 */
/*
 * [한국어]
 * spdk_nvmf_ctrlr_identify_ctrlr - 특정 NVMe-oF 컨트롤러의 Identify Controller(CNS 01h) 응답
 *                                  자료구조(spdk_nvme_ctrlr_data, 4KB)를 채워 준다.
 *
 * @ctrlr: 대상 NVMe-oF 컨트롤러 인스턴스. 호스트 connect 시 lib/nvmf/ctrlr.c 가 생성하며,
 *         subsystem 의 NQN/시리얼/모델 등 attribute 를 보유한다.
 * @cdata: 호출자가 미리 zeroed/할당해 전달하는 NVMe Identify Controller 결과 버퍼. 본
 *         함수가 vendor/serial/firmware/oacs/cqes/sqes 등을 채워 반환.
 * @return: spdk_nvmf_request_exec_status. 항상 동기 완료(COMPLETE) 가 일반적.
 *
 * 호스트가 admin opcode Identify(0x06) + CNS=01h 로 컨트롤러 식별을 요청했을 때 응답을
 * 만드는 코어 로직. custom admin 핸들러에서 일부 필드만 재정의하고 싶을 때 호출하여
 * 기본값을 채우고 그 위에 자기 값을 덮어쓰는 패턴으로 쓰인다. 호출 컨텍스트는 admin
 * qpair 의 spdk_thread 이며, ctrlr 의 mutable 필드를 읽으므로 controller-state lock 가
 * 필요한 시점은 caller 가 보장한다.
 *
 * 호출 체인:
 *   nvmf_ctrlr_process_admin_cmd → nvmf_ctrlr_identify → [이 함수] → memcpy 결과를 capsule iov 에 복사
 */
int spdk_nvmf_ctrlr_identify_ctrlr(struct spdk_nvmf_ctrlr *ctrlr,
				   struct spdk_nvme_ctrlr_data *cdata);

/**
 * Fills log page struct with supported cmds and effects log page for specified controller
 *
 * \param ctrlr The NVMe-oF controller
 * \param log_page target structure to be filled with cmds and effects supported by the controller
 */
/*
 * [한국어]
 * spdk_nvmf_get_cmds_and_effects_log_page - Commands Supported and Effects log page(LID 0x05)
 *                                            응답 채우기.
 *
 * @ctrlr: 대상 컨트롤러. 어떤 admin/IO opcode 를 지원하는지가 ctrlr/subsystem 설정과
 *         transport 종류에 따라 달라진다.
 * @log_page: 호출자가 zeroed 로 전달하는 결과 버퍼(spdk_nvme_cmds_and_effect_log_page).
 *            opcode 별 acsupp/iocs/csupp/lbcc/ncc 비트필드가 채워진다.
 *
 * NVMe Get Log Page(LID=0x05) 는 호스트가 컨트롤러 능력 매트릭스를 동적으로 알아내는
 * 표준 수단. SPDK 는 이 응답을 통해 자기 admin 디스패처가 처리하도록 빌드된 opcode 만
 * "supported" 로 표시한다. 호출 컨텍스트는 admin qpair 의 spdk_thread 이며, 동기 함수다.
 *
 * 호출 체인:
 *   nvmf_ctrlr_get_log_page → [이 함수] → log_page 를 capsule data 로 회신
 */
void spdk_nvmf_get_cmds_and_effects_log_page(struct spdk_nvmf_ctrlr *ctrlr,
		struct spdk_nvme_cmds_and_effect_log_page *log_page);

/**
 * Fills log page struct with feature identifiers effects log page for specified controller
 *
 * \param ctrlr The NVMe-oF controller
 * \param log_page target struct to be filled with log page data
 */
/*
 * [한국어]
 * spdk_nvmf_get_feature_ids_effects_log_page - Feature Identifiers Effects log page
 *                                                 (LID 0x12) 응답 채우기.
 *
 * @ctrlr: 대상 컨트롤러.
 * @log_page: 결과 버퍼(spdk_nvme_feature_ids_effects_log_page). 각 FID 마다
 *            "set 시 controller-wide effect / namespace-scope / persistent across power"
 *            등의 메타데이터를 채운다.
 *
 * 호스트가 어떤 Set Features 호출이 어떤 부수효과를 일으키는지를 사전에 파악하도록 돕는
 * 진단성 로그. 동기 함수, admin qpair 컨텍스트.
 *
 * 호출 체인:
 *   nvmf_ctrlr_get_log_page (LID=0x12) → [이 함수] → 결과 회신
 */

void spdk_nvmf_get_feature_ids_effects_log_page(struct spdk_nvmf_ctrlr *ctrlr,
		struct spdk_nvme_feature_ids_effects_log_page *log_page);

/**
 * Fill the log page struct with supported log pages for specified controller
 *
 * \param ctrlr The NVMe-oF controller
 * \param log_page target struct to be filled with log pages supported by the controller
 */
/*
 * [한국어]
 * spdk_nvmf_get_supported_log_pages - Supported Log Pages log page(LID 0x00) 응답 채우기.
 *
 * @ctrlr: 대상 컨트롤러.
 * @log_page: 결과 버퍼(spdk_nvme_supported_log_pages). 각 LID(0x00..0xFF) 가 지원되는지를
 *            비트로 표시.
 *
 * 호스트가 GetLogPage 의 메타-검색을 위해 호출. 동기 함수.
 *
 * 호출 체인:
 *   nvmf_ctrlr_get_log_page (LID=0x00) → [이 함수] → 결과 회신
 */
void spdk_nvmf_get_supported_log_pages(struct spdk_nvmf_ctrlr *ctrlr,
				       struct spdk_nvme_supported_log_pages *log_page);

/**
 * Fills the I/O Command Set specific Identify Namespace data structure (CNS
 * 05h)
 *
 * \param ctrlr The NVMe-oF controller
 * \param cmd The NVMe command
 * \param rsp The NVMe command completion
 * \param nsdata The filled in I/O command set specific identify namespace
 * attributes
 * \param nsdata_size The size of nsdata
 * \return \ref spdk_nvmf_request_exec_status
 */
/*
 * [한국어]
 * spdk_nvmf_ns_identify_iocs_specific - Identify CNS=05h (I/O Command Set specific Identify
 *                                        Namespace) 응답 채우기.
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: 호스트가 보낸 NVMe Identify SQE — CDW10(CNS/CNTID), CDW11(CSI), nsid 를 참조.
 * @rsp: NVMe completion 구조체 — 본 함수가 SC/SCT(상태 코드) 를 채워 반환할 수 있다.
 * @nsdata: 결과 버퍼. CSI=ZNS 면 ZNS-specific 필드를, NVM 이면 NVM-specific 필드를 채운다.
 * @nsdata_size: 결과 버퍼의 크기(보통 4096B).
 * @return: spdk_nvmf_request_exec_status (보통 COMPLETE).
 *
 * NVMe 1.4 / 2.0 의 multiple I/O Command Set 지원의 일부. 호스트는 동일한 namespace 에
 * 대해 NVM 외에 ZNS, KV 등 다른 CSI 를 묻기 위해 본 식별을 호출한다.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify (CNS=0x05) → [이 함수] → nsdata 회신
 */
int spdk_nvmf_ns_identify_iocs_specific(struct spdk_nvmf_ctrlr *ctrlr,
					struct spdk_nvme_cmd *cmd,
					struct spdk_nvme_cpl *rsp,
					void *nsdata,
					size_t nsdata_size);

/**
 * Fills the I/O Command Set specific Identify Controller data structure (CNS
 * 06h)
 *
 * \param ctrlr The NVMe-oF controller
 * \param cmd The NVMe command
 * \param rsp The NVMe command completion
 * \param cdata The filled in I/O command set specific identify controller
 * attributes
 * \param cdata_size The size of cdata
 * \return \ref spdk_nvmf_request_exec_status
 */
/*
 * [한국어]
 * spdk_nvmf_ctrlr_identify_iocs_specific - Identify CNS=06h (I/O Command Set specific
 *                                            Identify Controller) 응답 채우기.
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: 입력 SQE (CSI 가 CDW11 에).
 * @rsp: 출력 CQE — 본 함수가 SC/SCT 를 변경할 수 있다.
 * @cdata: 결과 버퍼. CSI 별 컨트롤러 ability(예: ZNS 의 경우 ZASL 등)를 채운다.
 * @cdata_size: 결과 버퍼의 크기.
 * @return: spdk_nvmf_request_exec_status.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify (CNS=0x06) → [이 함수] → cdata 회신
 */
int spdk_nvmf_ctrlr_identify_iocs_specific(struct spdk_nvmf_ctrlr *ctrlr,
		struct spdk_nvme_cmd *cmd,
		struct spdk_nvme_cpl *rsp,
		void *cdata,
		size_t cdata_size);


/**
 * Fills the I/O Command Set Independent Identify Namespace Data Structure
 * (CNS 08h)
 *
 * \param ctrlr The NVMe-oF controller
 * \param cmd The NVMe command
 * \param rsp The NVMe command completion
 * \param nsdata The filled in I/O Command Set Independent Identify Namespace Data
 * \return \ref spdk_nvmf_request_exec_status
 */
/*
 * [한국어]
 * spdk_nvmf_identify_ns_iocs_independent - Identify CNS=08h (I/O Command Set Independent
 *                                            Identify Namespace) 응답 채우기.
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: 입력 SQE — nsid 가 핵심.
 * @rsp: 출력 CQE.
 * @nsdata: 결과 버퍼(spdk_nvme_ns_iocs_independent_data). CSI 와 무관하게 namespace 의
 *          ANA/NSFEAT/EUI64/NGUID 등 공통 attribute 를 채운다.
 * @return: spdk_nvmf_request_exec_status.
 *
 * NVMe 2.0 신규. 동일 namespace 에 여러 CSI 가 매핑될 때 공통 메타데이터만 분리해 회신.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify (CNS=0x08) → [이 함수] → nsdata 회신
 */
int spdk_nvmf_identify_ns_iocs_independent(struct spdk_nvmf_ctrlr *ctrlr,
		struct spdk_nvme_cmd *cmd,
		struct spdk_nvme_cpl *rsp,
		struct spdk_nvme_ns_iocs_independent_data *nsdata);

/**
 * Fills the identify namespace attributes for the specified controller
 *
 * \param ctrlr The NVMe-oF controller
 * \param cmd The NVMe command
 * \param rsp The NVMe command completion
 * \param nsdata The filled in identify namespace attributes
 * \return \ref spdk_nvmf_request_exec_status
 */
/*
 * [한국어]
 * spdk_nvmf_ctrlr_identify_ns - Identify CNS=00h (Identify Namespace, NVM CSI) 응답 채우기.
 *
 * @ctrlr: 대상 컨트롤러.
 * @cmd: 입력 SQE — nsid 가 1..n_namespaces 범위여야 한다. nsid=0/0xFFFFFFFF 등 특수값은
 *       에러 또는 별도 처리(BROADCAST).
 * @rsp: 출력 CQE — invalid nsid 면 본 함수가 에러 SC 를 채운다.
 * @nsdata: 결과 버퍼(spdk_nvme_ns_data, 4096B). NSZE/NCAP/NUSE/LBAF/FLBAS/PI/DPS 등을 채운다.
 * @return: spdk_nvmf_request_exec_status.
 *
 * Identify NS 는 host 가 LBA 크기, 보호 정보, end-to-end DIF, namespace 활성화 상태를 알아
 * 내는 핵심 admin command. 본 함수는 SPDK 가 보유한 spdk_nvmf_ns 메타데이터를 NVMe 표준
 * 포맷으로 변환한다.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify (CNS=0x00) → [이 함수] → nsdata 회신
 */
int spdk_nvmf_ctrlr_identify_ns(struct spdk_nvmf_ctrlr *ctrlr,
				struct spdk_nvme_cmd *cmd,
				struct spdk_nvme_cpl *rsp,
				struct spdk_nvme_ns_data *nsdata);

/**
 * Fills the identify namespace attributes for the specified controller.
 *
 * This funtion uses nvme passthru for the namespaces that are backed by bdevs
 * that support NVME_ADMIN IO type. It differs from spdk_nvmf_ctrlr_identify_ns
 * by requesting identify namespace data and populate performance and atomic
 * operations fields.
 *
 * \param req The NVMe-oF request
 * \return \ref spdk_nvmf_request_exec_status
 */
/*
 * [한국어]
 * spdk_nvmf_ctrlr_identify_ns_ext - 확장 Identify NS — backing bdev 가 NVMe 장치인 경우
 *                                    실제 NVMe 패스스루로 응답을 받아 "성능/원자성 필드"
 *                                    까지 채운다.
 *
 * @req: NVMe-oF 요청 객체. 안에 cmd/rsp/iov/length/ctrlr/qpair 가 다 들어 있다.
 * @return: spdk_nvmf_request_exec_status. backing bdev 패스스루로 진입하면 ASYNCHRONOUS,
 *          fallback(메타데이터만) 동기 채움이면 COMPLETE.
 *
 * 일반 spdk_nvmf_ctrlr_identify_ns 와의 차이는 NPWG/NPWA/NPDG/NPDA/NOWS 등 성능/atomic
 * 관련 필드까지 backing 디바이스로부터 직접 받아온다는 점. backing bdev 가 NVMe 가 아니면
 * 일반 경로로 폴백한다.
 *
 * 호출 체인:
 *   nvmf_ctrlr_identify (CNS=0x00, ext mode) → [이 함수] → bdev passthru → 콜백에서 회신
 */
int spdk_nvmf_ctrlr_identify_ns_ext(struct spdk_nvmf_request *req);

/**
 * Callback function definition for a custom admin command handler.
 *
 * A function of this type is passed to \ref spdk_nvmf_set_custom_admin_cmd_hdlr.
 * It is called for every admin command that is processed by the NVMe-oF subsystem.
 * If the function handled the admin command then it must return a value from
 * \ref spdk_nvmf_request_exec_status. If the function did not handle the
 * admin command then it should return -1. In this case the SPDK default admin
 * command processing is applied to the request.
 *
 * \param req The NVMe-oF request of the admin command that is currently
 *            processed
 * \return \ref spdk_nvmf_request_exec_status if the command has been handled
 *         by the handler or -1 if the command wasn't handled
 */
/*
 * [한국어]
 * spdk_nvmf_custom_cmd_hdlr - 사용자 정의 admin 핸들러의 함수 시그니처(typedef).
 *
 * 인자: req — 처리할 NVMe-oF 요청 객체.
 * 반환: 0/1 (spdk_nvmf_request_exec_status: COMPLETE/ASYNCHRONOUS) 또는 -1(처리하지 않음).
 *
 * 디스패처는 g_nvmf_custom_admin_cmd_hdlrs[opc] 가 NULL 이 아니면 본 시그니처로 호출한다.
 * 핸들러가 -1 을 반환하면 SPDK 의 default 처리(switch-case) 가 이어 실행된다.
 * 핸들러는 동일 spdk_thread 에서만 호출되므로 자기 컨텍스트 내 자료구조에 락 없이 접근
 * 가능하지만, 다른 코어의 자료구조에 접근하려면 spdk_thread_send_msg 가 필요하다.
 */
typedef int (*spdk_nvmf_custom_cmd_hdlr)(struct spdk_nvmf_request *req);

/**
 * Installs a custom admin command handler.
 *
 * \param opc NVMe admin command OPC for which the handler should be installed.
 * \param hdlr The handler function. See \ref spdk_nvmf_custom_cmd_hdlr.
 */
/*
 * [한국어]
 * spdk_nvmf_set_custom_admin_cmd_hdlr - opcode → 사용자 핸들러 매핑 등록.
 *
 * @opc: NVMe admin opcode (0x00..0xFF). vendor-specific(0xC0..0xFF) 또는 표준 opcode 모두
 *       가능하지만, 표준을 덮어쓰면 호스트 호환성에 영향. 이미 등록된 opc 면 덮어씌운다.
 * @hdlr: 호출 시점에 실행할 콜백. NULL 을 넣으면 디폴트 처리로 되돌리는 효과(구현에 따라).
 *
 * 본 함수는 일반적으로 SPDK 초기화 단계(spdk_subsystem_init 직후, target 시작 전) 에서
 * 한 번 호출된다. 내부적으로 g_nvmf_custom_admin_cmd_hdlrs[opc] = hdlr 형태로 전역 테이블
 * 에 저장. 모든 nvmf target 에 공통 적용된다(target 별 분리 X).
 *
 * 호출 체인(설정):
 *   사용자 모듈 init → [이 함수] → g_nvmf_custom_admin_cmd_hdlrs 갱신
 * 호출 체인(런타임):
 *   admin SQE 도착 → ctrlr.c admin 디스패처 → g_nvmf_custom_admin_cmd_hdlrs[opc] → 사용자 핸들러
 */
void spdk_nvmf_set_custom_admin_cmd_hdlr(uint8_t opc, spdk_nvmf_custom_cmd_hdlr hdlr);

/**
 * Forward an NVMe admin command to a namespace
 *
 * This function forwards all NVMe admin commands of value opc to the specified
 * namespace id.
 * If forward_nsid is 0, the command is sent to the namespace that was specified in the
 * original command.
 *
 * \param opc - NVMe admin command OPC
 * \param forward_nsid - nsid or 0
 */
/*
 * [한국어]
 * spdk_nvmf_set_passthru_admin_cmd - admin opcode 를 backing NVMe bdev 로 그대로 패스스루
 *                                      하도록 정책 등록.
 *
 * @opc: 패스스루 대상 admin opcode.
 * @forward_nsid: 0 이면 SQE 의 nsid 를 그대로 사용. 0 이 아니면 SQE 의 nsid 를 무시하고
 *                지정 nsid 의 backing bdev 에 보낸다(예: 모든 vendor admin 을 nsid=1 로 강제).
 *
 * 일반적으로 firmware 업데이트, vendor security/log page 등 backing 드라이브에서만 의미가
 * 있는 명령을 호스트가 그대로 쓰도록 만들 때 사용. 내부적으로 패스스루 규칙 테이블을 갱
 * 신하며, 디스패처는 해당 opcode 를 받으면 spdk_nvmf_bdev_ctrlr_nvme_passthru_admin 으로
 * 위임한다. backing bdev 가 NVMe 가 아니거나 admin passthru 미지원이면 호스트는
 * Invalid Opcode/Command 응답을 받게 된다.
 */
void spdk_nvmf_set_passthru_admin_cmd(uint8_t opc, uint32_t forward_nsid);

/**
 * Callback function that is called right before the admin command reply
 * is sent back to the initiator.
 *
 * \param req The NVMe-oF request
 */
/*
 * [한국어]
 * spdk_nvmf_nvme_passthru_cmd_cb - 패스스루 완료 직전 호출되는 콜백 시그니처(typedef).
 *
 * 인자: req — 거의 완성된 응답을 담은 NVMe-oF 요청. cpl(rsp) 가 backing 드라이브의 응답
 *       으로 채워진 상태.
 *
 * 사용처: spdk_nvmf_bdev_ctrlr_nvme_passthru_admin 의 마지막 인자. 이 콜백에서 응답의
 * 일부 필드를 후처리(예: 호스트가 보면 안 되는 vendor 필드 마스킹, identify 의 ctrlr id
 * 재작성) 한 뒤 자동으로 transport 가 호스트로 회신한다. 콜백은 admin qpair 의 spdk_thread
 * 에서 실행된다.
 */
typedef void (*spdk_nvmf_nvme_passthru_cmd_cb)(struct spdk_nvmf_request *req);

/**
 * Submits the NVMe-oF request to a bdev.
 *
 * This function can be used in a custom admin handler to send the command contained
 * in the req to a bdev. Once the bdev completes the command, the specified cb_fn
 * is called (which can be NULL if not needed).
 *
 * \param bdev The \ref spdk_bdev
 * \param desc The \ref spdk_bdev_desc
 * \param ch The \ref spdk_io_channel
 * \param req The \ref spdk_nvmf_request passed to the bdev for processing
 * \param cb_fn A callback function (or NULL) that is called before the request
 * is completed.
 *
 * \return A \ref spdk_nvmf_request_exec_status
 */
/*
 * [한국어]
 * spdk_nvmf_bdev_ctrlr_nvme_passthru_admin - custom 핸들러 안에서 명시적으로 admin
 *                                              패스스루를 실행하는 보조 함수.
 *
 * @bdev: 대상 bdev (보통 NVMe bdev). spdk_nvmf_request_get_bdev 으로 미리 얻는다.
 * @desc: 본 SPDK 컴포넌트가 가진 bdev open descriptor. close 시점까지 유효.
 * @ch:   현재 spdk_thread 에 할당된 IO channel(spdk_bdev_get_io_channel 결과).
 * @req:  진행 중인 NVMe-oF 요청 — 안의 cmd 가 그대로 backing 드라이브로 전달된다.
 * @cb_fn: 응답이 호스트로 회신되기 직전에 호출되는 사용자 후처리 콜백 (NULL 허용).
 * @return: spdk_nvmf_request_exec_status. 정상 진입 시 ASYNCHRONOUS.
 *
 * 내부적으로 spdk_bdev_nvme_admin_passthru() 를 호출. backing 드라이브가 admin passthru
 * 를 지원해야 하며, 미지원 시 INVALID OPCODE 로 즉시 완료된다. 핸들러가 cb_fn 으로
 * 응답을 가공하면, transport 는 그 가공된 cpl 을 호스트로 회신한다.
 *
 * 호출 체인:
 *   사용자 admin 핸들러 → [이 함수] → spdk_bdev_nvme_admin_passthru → bdev_nvme 모듈 →
 *     실제 NVMe 디바이스 admin SQE → 완료 콜백 → cb_fn → 호스트로 CQE 회신
 */
int spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch, struct spdk_nvmf_request *req, spdk_nvmf_nvme_passthru_cmd_cb cb_fn);

/**
 * Attempts to abort a request in the specified bdev
 *
 * \param bdev Bdev that is processing req_to_abort
 * \param desc Bdev desc
 * \param ch Channel on which req_to_abort was originally submitted
 * \param req Abort cmd req
 * \param req_to_abort The request that should be aborted
 */
/*
 * [한국어]
 * spdk_nvmf_bdev_ctrlr_abort_cmd - bdev 레벨에서 진행 중인 nvmf 요청을 abort 시도.
 *
 * @bdev: req_to_abort 가 처리되고 있는 bdev.
 * @desc: 그 bdev 의 open descriptor.
 * @ch:   req_to_abort 가 submit 되었던 동일 channel(같은 spdk_thread 의 channel 이어야 한다).
 * @req:  현재 처리 중인 Abort 요청(NVMe admin Abort 명령). 본 함수의 결과로 채워진다.
 * @req_to_abort: 취소하려는 대상 요청 객체.
 * @return: 0 또는 음수 errno (호출자가 SC 로 변환).
 *
 * NVMe Abort 는 best-effort. backing bdev 가 abort 를 지원하면(spdk_bdev_abort) 실제로
 * 취소를 시도하고, 미지원이면 즉시 실패. abort 자체는 비동기일 수 있으며 그 경우 req 는
 * 콜백에서 완료된다. 핵심 제약: req_to_abort 의 channel 과 req 의 channel 이 같은
 * spdk_thread 여야 한다(SPDK abort 의 thread-affinity 규칙).
 *
 * 호출 체인:
 *   nvmf_ctrlr_abort → [이 함수] → spdk_bdev_abort → bdev 모듈 abort 콜백
 */
int spdk_nvmf_bdev_ctrlr_abort_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				   struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
				   struct spdk_nvmf_request *req_to_abort);

/**
 * Provide access to the underlying bdev that is associated with a namespace.
 *
 * This function can be used to communicate with the bdev. For example,
 * a \ref spdk_nvmf_custom_admin_cmd_hdlr can use \ref spdk_nvmf_bdev_nvme_passthru_admin
 * to pass on a \ref spdk_nvmf_request to a NVMe bdev.
 *
 * \param nsid The namespace id of a namespace that is valid for the
 * underlying subsystem
 * \param req The NVMe-oF request that is being processed
 * \param bdev Returns the \ref spdk_bdev corresponding to the namespace id
 * \param desc Returns the \ref spdk_bdev_desc corresponding to the namespace id
 * \param ch Returns the \ref spdk_io_channel corresponding to the namespace id
 *
 * \return 0 upon success
 * \return -EINVAL if the namespace id can't be found
 */
/*
 * [한국어]
 * spdk_nvmf_request_get_bdev - 요청의 nsid 로부터 backing bdev/desc/channel 을 한꺼번에 추출.
 *
 * @nsid: 1..n_namespaces. 0 또는 invalid nsid 는 -EINVAL.
 * @req:  현재 처리 중인 NVMe-oF 요청. 안의 ctrlr/qpair 로부터 subsystem 과 thread 의 channel 을 찾는다.
 * @bdev: [out] 해당 nsid 의 backing bdev 포인터.
 * @desc: [out] 그 bdev 의 open descriptor.
 * @ch:   [out] 현 spdk_thread 에 할당된 IO channel.
 * @return: 0 성공, -EINVAL 실패.
 *
 * custom 핸들러가 spdk_nvmf_bdev_ctrlr_nvme_passthru_admin / abort 등 bdev 레벨 헬퍼를
 * 호출하기 위한 사전 준비 함수. 호출자는 *desc/*ch 를 close/put 해서는 안 된다 — 이
 * 자원들은 nvmf 코어가 관리한다.
 */
int spdk_nvmf_request_get_bdev(uint32_t nsid,
			       struct spdk_nvmf_request *req,
			       struct spdk_bdev **bdev,
			       struct spdk_bdev_desc **desc,
			       struct spdk_io_channel **ch);

/**
 * Get the NVMe-oF controller associated with this request.
 *
 * \param req The NVMe-oF request
 *
 * \return The NVMe-oF controller
 */
/*
 * [한국어]
 * spdk_nvmf_request_get_ctrlr - 요청 객체에서 소유 컨트롤러 인스턴스 반환.
 *
 * @req: NVMe-oF 요청.
 * @return: req->qpair->ctrlr 와 같은 의미. fabrics connect 이전(인증/접속 단계)에는 NULL
 *          일 수 있으므로 호출자는 NULL 체크가 필요할 수 있다.
 *
 * 사용처: custom 핸들러에서 호스트 NQN, ANA state, host_id 등을 ctrlr->* 로부터 읽을 때.
 */
struct spdk_nvmf_ctrlr *spdk_nvmf_request_get_ctrlr(struct spdk_nvmf_request *req);

/**
 * Get the NVMe-oF subsystem associated with this request.
 *
 * \param req The NVMe-oF request
 *
 * \return The NVMe-oF subsystem
 */
/*
 * [한국어]
 * spdk_nvmf_request_get_subsystem - 요청 객체에서 소유 subsystem 반환.
 *
 * @req: NVMe-oF 요청.
 * @return: req->qpair->ctrlr->subsys. 마찬가지로 connect 이전에는 NULL 가능.
 *
 * 사용처: custom 핸들러에서 NQN, allowed host 목록, listener 정보 등을 알아낼 때.
 */
struct spdk_nvmf_subsystem *spdk_nvmf_request_get_subsystem(struct spdk_nvmf_request *req);

/**
 * Copy the data from the given @buf into the request iovec.
 *
 * \param req The NVMe-oF request
 * \param buf The data buffer
 * \param buflen The length of the data buffer
 *
 * \return the number of bytes copied
 */
/*
 * [한국어]
 * spdk_nvmf_request_copy_from_buf - 일반 메모리 buf 에서 req 의 iovec(전송 버퍼) 로 복사.
 *
 * @req: NVMe-oF 요청. transport 에 따라 1 개 또는 다수 iov 로 분산되어 있다.
 * @buf: 소스 메모리(연속 영역).
 * @buflen: 복사할 크기.
 * @return: 실제 복사된 바이트 수(req 의 iov 총합과 buflen 의 작은 쪽).
 *
 * 패스스루 응답을 호스트에게 보내기 전, 임시 버퍼에 만든 결과를 transport iovec 로
 * 옮길 때 사용. transport-specific 인 PRP/SGL 변환은 이미 iov 가 추상화하고 있다.
 */
size_t spdk_nvmf_request_copy_from_buf(struct spdk_nvmf_request *req,
				       void *buf, size_t buflen);

/**
 * Copy the data from the request iovec into the given @buf.
 *
 * \param req The NVMe-oF request
 * \param buf The data buffer
 * \param buflen The length of the data buffer
 *
 * \return the number of bytes copied
 */
/*
 * [한국어]
 * spdk_nvmf_request_copy_to_buf - req 의 iovec(수신 데이터) 에서 일반 메모리 buf 로 복사.
 *
 * @req: NVMe-oF 요청.
 * @buf: 목적지 메모리.
 * @buflen: 복사할 크기.
 * @return: 실제 복사된 바이트 수.
 *
 * 호스트가 보낸 write 데이터를 custom 핸들러에서 단일 버퍼로 모아 처리할 때 사용.
 * (예: vendor-specific write 명령의 페이로드를 한 번에 검사.)
 */
size_t spdk_nvmf_request_copy_to_buf(struct spdk_nvmf_request *req,
				     void *buf, size_t buflen);

/**
 * Get the NVMe-oF command associated with this request.
 *
 * \param req The NVMe-oF request
 *
 * \return The NVMe command
 */
/*
 * [한국어]
 * spdk_nvmf_request_get_cmd - 요청에서 NVMe SQE(submission queue entry, 64B) 포인터 반환.
 *
 * @req: NVMe-oF 요청.
 * @return: spdk_nvme_cmd 포인터(opcode/cdw10..15/nsid/cid 등). 핸들러가 이 포인터로 명령을
 *          파싱한다. 본 포인터는 req 수명 동안 유효.
 */
struct spdk_nvme_cmd *spdk_nvmf_request_get_cmd(struct spdk_nvmf_request *req);

/**
 * Get the NVMe-oF completion associated with this request.
 *
 * \param req The NVMe-oF request
 *
 * \return The NVMe completion
 */
/*
 * [한국어]
 * spdk_nvmf_request_get_response - 요청에서 NVMe CQE(completion queue entry, 16B) 포인터 반환.
 *
 * @req: NVMe-oF 요청.
 * @return: spdk_nvme_cpl 포인터(SC/SCT/cdw0/cid 등). 핸들러는 이 영역에 결과를 기록하고
 *          spdk_nvmf_request_complete 시 transport 가 호스트에게 회신한다.
 */
struct spdk_nvme_cpl *spdk_nvmf_request_get_response(struct spdk_nvmf_request *req);

/**
 * Get the request to abort that is associated with this request.
 * The req to abort is only set if the request processing a SPDK_NVME_OPC_ABORT cmd
 *
 * \param req The NVMe-oF abort request
 *
 * \return req_to_abort The NVMe-oF request that is in process of being aborted
 */
/*
 * [한국어]
 * spdk_nvmf_request_get_req_to_abort - Abort 요청이 가리키는 "취소 대상" 원래 요청 반환.
 *
 * @req: NVMe admin Abort(opcode 0x08) 처리 중인 요청.
 * @return: 취소 대상 spdk_nvmf_request — 못 찾으면 NULL. abort 가 아닌 일반 요청에 호출
 *          하면 의미 없음(보통 NULL).
 *
 * Abort SQE 의 CDW10(SQID/CID) 으로 lib/nvmf/ctrlr.c 가 매칭되는 in-flight 요청을 찾아
 * req_to_abort 필드에 저장. custom abort 핸들러는 이 함수로 그 대상을 얻어 추가 정리
 * 작업(예: 사용자 정의 자원 해제) 을 수행한다.
 */
struct spdk_nvmf_request *spdk_nvmf_request_get_req_to_abort(struct spdk_nvmf_request *req);

#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 종료. */
#endif

#endif /* SPDK_NVMF_CMD_H_ */
/* [한국어] include guard 종료. 본 헤더는 nvmf.h 와 함께 외부 모듈이 NVMe-oF target
 * 디스패처에 끼어드는 핵심 진입점이다. */
