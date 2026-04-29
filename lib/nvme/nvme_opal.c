/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] TCG Opal SSC 2.0 Self-Encrypting Drive 클라이언트 구현 (nvme_opal.c)
 *
 * === 파일의 역할 ===
 * NVMe SSD에 내장된 **TCG(Trusted Computing Group) Opal SSC 2.0 보안 서브시스템**
 * (Self-Encrypting Drive; SED)을 구동하기 위한 호스트 측 클라이언트의 완전 구현.
 * NVMe Admin 명령 Security Send (opcode 0x81) / Security Receive (opcode 0x82)를
 * 캡슐로 사용하여, 그 페이로드에 TCG Storage Working Group(SWG) "Opal SSC" 프로토콜
 * 메시지를 실어 보낸다. 이를 통해 SSD 내부 AES-256 암호화 엔진이 보호하는 LBA 범위
 * (Locking Range)를 활성화/잠금/해제하고, 권한 객체(Authority)와 해당 비밀번호
 * (C_PIN credential)를 관리한다. 한 마디로 "SSD 자체가 데이터를 자동 암호화하고,
 * 호스트는 키(비밀번호)를 알아야만 데이터를 읽을 수 있게 하는" 보안 명령 계층이다.
 *
 * 핵심 가치 5가지:
 *   1) **무결성 강제**: 디스크 분실 시 키를 모르면 데이터 복호화 불가 (AES-256 하드웨어 가속).
 *   2) **세션 기반**: Admin SP / Locking SP라는 두 보안 도메인(SP=Security Provider)에
 *      세션을 열고 닫는 모델 — 모든 명령은 (HSN, TSN) 세션 식별자로 보호.
 *   3) **TCG SWG Token Stream**: SmallAtom/MediumAtom/LongAtom + StartList/EndList/
 *      StartName/EndName/Call로 구성된 자체 TLV(Tag-Length-Value) 인코딩.
 *      마치 "단순 ASN.1" 같은 바이너리 RPC 포맷.
 *   4) **하드웨어 키 격리**: PIN 자체는 SSD 펌웨어 내부에만 저장되고 외부로 절대 노출되지 않음.
 *      호스트는 매 명령마다 PIN을 다시 보내 인증.
 *   5) **MSID 출하 키 회수**: 처음 디바이스를 인수할 때 MSID(공장 디폴트 PIN)를 GET으로 읽어
 *      그것으로 SID(Security Identifier; 디바이스 소유자) 세션을 시작해 새 PIN으로 변경 (TakeOwnership).
 *
 * **본 파일은 코드 수정 없이 한국어 주석만 추가/보강된 학습 사본**이다. 원본은
 * SPDK upstream의 lib/nvme/nvme_opal.c와 동일하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 진입 — 두 가지 경로:
 *   [경로 1: SPDK Opal 관리 도구]
 *     사용자 → spdk_opal_dev_construct() → opal_discovery0() → SSD에 SECP_INFO 요청 →
 *       Opal v2.00 Feature 발견 → 디바이스가 SED 지원하면 spdk_opal_dev 객체 반환.
 *     이후 사용자가 spdk_opal_cmd_take_ownership / activate_locking_sp / lock_unlock /
 *       setup_locking_range / revert_tper / set_new_passwd 등 공개 API 호출.
 *
 *   [경로 2: bdev_nvme 모듈/RPC]
 *     RPC 핸들러 → bdev_nvme의 opal 통합 코드 → 이 파일의 spdk_opal_cmd_* 호출.
 *
 * 각 spdk_opal_cmd_*는 표준 패턴을 따른다:
 *   1. opal_init_key()로 패스워드를 spdk_opal_key 구조체로 변환
 *   2. opal_alloc_session()으로 세션 컨텍스트 할당
 *   3. opal_start_generic_session() 또는 opal_start_auth_session()으로 SSD에 세션 개시
 *      → 응답에서 HSN(Host Session Number), TSN(TPer Session Number) 수령
 *   4. opal_build_*_cmd()로 TCG 토큰 스트림 구성 → opal_cmd_finalize()로 헤더 채우고
 *   5. opal_send_recv()로 ADMIN 큐에 SECURITY_SEND 발행 → 완료 폴링 → SECURITY_RECEIVE로 응답 수령
 *   6. opal_parse_and_check_status()로 상태 토큰 검증
 *   7. opal_end_session()으로 EOS(EndOfSession) 토큰 송신, free(sess)
 *
 * 내부 동기 동작 모델:
 *   opal_send_recv()는 spdk_nvme_ctrlr_process_admin_completions를 spin polling 하며
 *   sess->done = true가 될 때까지 동기 대기. 즉 이 파일의 모든 공개 API는 **동기 블로킹**.
 *   호출자는 admin queue를 주관하는 spdk_thread 컨텍스트에서 호출해야 한다.
 *
 * === 타 모듈과의 연결 ===
 *  - **nvme_ctrlr_cmd.c**: spdk_nvme_ctrlr_cmd_security_send / security_receive 사용.
 *    이들은 NVMe Admin SQE에 NVMe opcode 0x81/0x82를 채우고 CDW10에 (SECP, SPSP=COMID)를
 *    넣어 큐잉한다. 콜백으로 opal_nvme_security_send_done / recv_done이 트리거.
 *  - **nvme_ctrlr.c**: spdk_nvme_ctrlr_process_admin_completions로 polling. 단일 reactor에서
 *    돌므로 race 없이 sess->done 플래그로 동기화 가능.
 *  - **spdk_nvme_ctrlr_security_receive (nvme_ctrlr.c의 wrapper)**: opal_discovery0의
 *    Level 0 Discovery에서만 사용 (process_admin_completions를 내부적으로 polling하는 동기 헬퍼).
 *  - **spdk/opal_spec.h**: TCG Opal 와이어 포맷 정의 — spdk_opal_compacket, spdk_opal_packet,
 *    spdk_opal_data_subpacket, spdk_opal_d0_*_feat (Level 0 Discovery feature blocks).
 *  - **spdk/opal.h**: 공개 API 시그니처와 enum (spdk_opal_user, spdk_opal_lock_state,
 *    spdk_opal_locking_range, spdk_opal_locking_range_info, spdk_opal_d0_features_info).
 *  - **nvme_opal_internal.h**: 내부 상수(IO_BUFFER_LENGTH=2048, OPAL_KEY_MAX=256,
 *    GENERIC_HOST_SESSION_NUM=0x69, OPAL_UID_LENGTH=8), spdk_opal_uid 테이블, spdk_opal_method
 *    테이블, opal_session 구조체.
 *  - **spdk/scsi_spec.h**: SPDK_SCSI_SECP_TCG (Security Protocol 0x01), SPDK_SCSI_SECP_INFO (0x00).
 *
 * 데이터 흐름:
 *   호스트 메모리 sess->cmd[2048] (TCG token stream)
 *     → spdk_nvme_ctrlr_cmd_security_send (PRP 매핑)
 *     → NVMe SQE (opcode 0x81, CDW10=SECP/SPSP, CDW11=length)
 *     → MMIO doorbell → SSD 펌웨어가 TCG 메시지 파싱
 *     → 처리 결과를 SSD 내부 응답 버퍼에 저장 + CQE 발행
 *     → opal_nvme_security_send_done 콜백 → 즉시 SECURITY_RECEIVE 발행
 *     → SSD가 응답 토큰 스트림을 sess->resp[2048]로 DMA 전송
 *     → opal_nvme_security_recv_done 콜백 → outstanding 데이터 있으면 다시 receive,
 *       없으면 sess->sess_cb 호출
 *
 * === 주요 함수/구조체 요약 ===
 *  - **opal_nvme_security_send / recv**: 저수준 NVMe security 명령 발행 + 콜백 체인.
 *  - **opal_send_recv**: 동기 spin-polling 래퍼 — 완료까지 admin completions 폴링.
 *  - **opal_add_token_u8 / u64 / bytestring / opal_add_tokens**: TCG SWG Token TLV 인코더.
 *    Tiny(1B), Short(1B header + ≤15B), Medium(2B header + ≤2047B), Long(4B header) 4종 atom.
 *  - **opal_cmd_finalize**: Sub-Packet/Packet/ComPacket 3중 헤더의 length 필드를 BE32로 패딩 정렬과 함께 채움.
 *  - **opal_response_parse / opal_response_status**: 응답 토큰 스트림을 parse_tiny/short/medium/long으로
 *    분해하고, EOS/StartList...EndList 사이의 1번째 정수가 메서드 상태 코드 (0=성공).
 *  - **opal_discovery0 / discovery0_end**: SECP_INFO로 지원 SECP 목록 확인 → SECP_TCG로
 *    Level 0 Discovery 페이로드 수령 → Feature Code 파싱(TPer/Locking/Geometry/SUM/Datastore/v100/v200) →
 *    BaseComID 추출 (이후 모든 명령의 COMID 헤더 필드에 사용).
 *  - **opal_start_generic_session**: AdminSP/LockingSP에 SID 또는 ANYBODY 권한으로 세션 개시.
 *  - **opal_start_auth_session**: LockingSP에 USER1..N 또는 Admin1 권한으로 R/W 세션 개시.
 *  - **opal_end_session**: EOS 토큰만 보내 HSN/TSN 무효화.
 *  - **opal_get_msid_cpin_pin**: 공장 디폴트 PIN(MSID, C_PIN_MSID 객체)을 GET 메서드로 회수.
 *  - **opal_set_sid_cpin_pin / new_user_passwd / build_generic_pw_cmd**: C_PIN의 PIN 컬럼을 SET 메서드로 갱신.
 *  - **opal_lock_unlock_range / generic_locking_range_enable_disable / setup_locking_range**:
 *    LockingRange 객체의 ReadLocked/WriteLocked/ReadLockEnabled/WriteLockEnabled/RangeStart/RangeLength SET.
 *  - **opal_get_locking_range_info / get_max_ranges / get_active_key**: 잠금 범위 메타 + 활성 암호화 키 GET.
 *  - **opal_gen_new_active_key / erase_locking_range**: GENKEY/ERASE 메서드로 키 회전 또는 영역 제로화.
 *  - **opal_activate / build_revert_tper_cmd**: LockingSP 활성화 / 공장 초기화(REVERT).
 *  - **spdk_opal_cmd_***: 위 내부 헬퍼들을 1회성 워크플로(세션 시작→작업→세션 종료)로 묶은 공개 API.
 *
 * 핵심 자료구조:
 *  - **struct opal_session**: HSN/TSN, cmd[2048]/resp[2048] 버퍼, 콜백, 파싱된 응답 토큰 캐시.
 *    한 명령 단위의 ephemeral 컨텍스트로, 매 spdk_opal_cmd_* 호출 시 alloc/free.
 *  - **struct spdk_opal_dev**: ctrlr 핸들 + COMID + Level 0 Discovery 결과(feat_info) +
 *    locking_ranges[] 캐시. 디바이스 수명 동안 유지.
 */

#include "spdk/opal.h"     /* [한국어] Opal 공개 API: spdk_opal_dev_construct, spdk_opal_cmd_*,
                            * enum spdk_opal_user/lock_state/locking_range, spdk_opal_locking_range_info. */
#include "spdk/log.h"      /* [한국어] SPDK 로깅 매크로 (SPDK_ERRLOG, SPDK_INFOLOG, SPDK_DEBUGLOG, SPDK_NOTICELOG). */
#include "spdk/util.h"     /* [한국어] spdk_min, to_be32/from_be32, to_be16/from_be16 등의 BE 변환 유틸리티.
                            * TCG Opal 와이어 포맷이 모두 빅엔디안이므로 헤더 필드 인코딩에 필수. */

#include "nvme_opal_internal.h"
                           /* [한국어] Opal 모듈 내부 헤더 — IO_BUFFER_LENGTH(2048),
                            * OPAL_UID_LENGTH(8), GENERIC_HOST_SESSION_NUM(0x69),
                            * spdk_opal_uid[](24개 표준 UID), spdk_opal_method[](13개 메서드 UID),
                            * struct opal_session, struct spdk_opal_dev, opal_token_type/atom_width enum 정의. */

/*
 * [한국어]
 * opal_nvme_security_recv_done - NVMe SECURITY_RECEIVE 명령 완료 콜백 (TCG 응답 수신 종료 처리)
 *
 * @arg: opal_session 포인터 (security_receive 발행 시 cb_arg로 전달된 값을 복원)
 * @cpl: NVMe Completion Queue Entry (SC/SCT 상태와 result 필드를 담은 16바이트 구조체)
 *
 * SSD가 SECURITY_RECEIVE 응답을 sess->resp 버퍼로 DMA 전송 완료한 직후 호출되는 콜백.
 * 처리 흐름은 3단계:
 *   1) NVMe-수준 에러 검사: 컨트롤러가 SC≠0(예: Invalid Field, Internal Error)으로 완료했는지 확인.
 *   2) TCG 분할 응답 처리: ComPacket 헤더의 outstanding_data가 0이고 min_transfer가 0이면
 *      "더 가져올 데이터 없음" → 사용자 콜백 sess->sess_cb(0)로 성공 종료.
 *      그렇지 않으면 SSD가 추가 응답 청크를 가지고 있다는 신호 (TCG Opal 스펙 3.2.4 ComPacket).
 *   3) 추가 RECEIVE 발행: 버퍼를 0으로 클리어 후 같은 콜백으로 또 한 번 SECURITY_RECEIVE 발행.
 *      이 자가 재귀 패턴으로 모든 sub-packet을 끌어옴.
 *
 * 실행 컨텍스트: spdk_nvme_ctrlr_process_admin_completions 내부에서 호출 — admin queue를
 *                소유한 spdk_thread (보통 메인 reactor). 동기화 불필요.
 *
 * 호출 체인:
 *   spdk_nvme_ctrlr_process_admin_completions → admin CQE 콜백 디스패치 →
 *     [opal_nvme_security_recv_done] → sess->sess_cb (보통 opal_send_recv_done) →
 *     opal_send_recv 폴링 루프가 sess->done 감지 → 호출자 반환
 */
static void
opal_nvme_security_recv_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct opal_session *sess = arg;
	/* [한국어] 콜백 컨텍스트 복원 — 발행 시 cb_arg로 sess를 전달했음. */
	struct spdk_opal_dev *dev = sess->dev;
	/* [한국어] dev에서 ctrlr와 COMID가 필요함 (재발행 시 사용). */
	void *response = sess->resp;
	/* [한국어] 응답 버퍼 베이스 — SSD가 DMA로 채워준 위치 (2048바이트). */
	struct spdk_opal_compacket *header = response;
	/* [한국어] ComPacket 헤더로 캐스팅 — TCG Opal 스펙 3.2.4: outstanding_data(BE32),
	 * min_transfer(BE32) 필드가 분할 응답 제어용. */
	int ret;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* [한국어] CPL의 SC(Status Code) 또는 SCT(Status Code Type)가 0이 아니면 에러.
		 * 예: 0x01(Invalid Field), 0x06(Internal Error), 0x82(Invalid Security Protocol). */
		sess->sess_cb(sess, -EIO, sess->cb_arg);
		/* [한국어] 사용자 콜백에 -EIO 전달 — 상위 opal_send_recv 폴러가 이 값을 sess->status에 저장. */
		return;
	}

	if (!header->outstanding_data && !header->min_transfer) {
		/* [한국어] outstanding_data=0 & min_transfer=0 → SSD가 보낼 데이터를 모두 보냈다는 의미.
		 * outstanding_data!=0이면 그만큼 바이트가 SSD에 남아있고, min_transfer는 다음 호출에서
		 * 최소한 받아야 할 바이트 수 힌트. */
		sess->sess_cb(sess, 0, sess->cb_arg);
		/* [한국어] 성공 종료 — 응답 토큰 스트림이 sess->resp에 모두 들어 있음. 호출자가 파싱 가능. */
		return;
	}

	memset(response, 0, IO_BUFFER_LENGTH);
	/* [한국어] 다음 청크를 받기 전 응답 버퍼 전체를 0으로 초기화 — 잔존 데이터로 인한 파싱 오류 방지. */
	ret = spdk_nvme_ctrlr_cmd_security_receive(dev->ctrlr, SPDK_SCSI_SECP_TCG,
			dev->comid, 0, sess->resp, IO_BUFFER_LENGTH,
			opal_nvme_security_recv_done, sess);
	/* [한국어] 같은 COMID(SPSP)에 추가 SECURITY_RECEIVE 발행 — SECP_TCG=0x01.
	 * NVMe 1.x 5.25.2: SPSP는 Security Protocol-Specific 16비트 식별자, Opal에서는 COMID로 사용.
	 * 콜백 자기참조로 outstanding_data가 0이 될 때까지 자동으로 청크를 흡수. */
	if (ret) {
		/* [한국어] 명령 큐잉 실패 (예: ENOMEM, ENXIO) — 사용자에게 즉시 보고. */
		sess->sess_cb(sess, ret, sess->cb_arg);
	}
}

/*
 * [한국어]
 * opal_nvme_security_send_done - SECURITY_SEND 완료 후 자동으로 SECURITY_RECEIVE 발행
 *
 * @arg: opal_session 포인터
 * @cpl: NVMe CQE
 *
 * TCG Opal은 항상 "Send 후 Receive" 페어로 동작한다 (RPC 패턴).
 * SSD는 Send로 받은 명령을 처리한 후, 응답을 내부 버퍼에 보관하고 호스트가
 * RECEIVE로 가져갈 때까지 기다린다. 이 콜백은 그 자동 트리거를 담당한다.
 *
 * 호출 체인:
 *   opal_nvme_security_send → admin SQE 큐잉 → CQE 도착 → process_admin_completions →
 *     [opal_nvme_security_send_done] → spdk_nvme_ctrlr_cmd_security_receive →
 *     opal_nvme_security_recv_done → (재귀로 분할 응답 모두 흡수) → sess->sess_cb
 */
static void
opal_nvme_security_send_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct opal_session *sess = arg;
	/* [한국어] 발행 시 cb_arg로 전달한 세션 컨텍스트 복원. */
	struct spdk_opal_dev *dev = sess->dev;
	/* [한국어] RECEIVE 발행에 ctrlr와 COMID 필요. */
	int ret;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* [한국어] Send 자체가 실패 — TCG 처리 시도조차 못한 상황. */
		sess->sess_cb(sess, -EIO, sess->cb_arg);
		return;
	}

	ret = spdk_nvme_ctrlr_cmd_security_receive(dev->ctrlr, SPDK_SCSI_SECP_TCG,
			dev->comid, 0, sess->resp, IO_BUFFER_LENGTH,
			opal_nvme_security_recv_done, sess);
	/* [한국어] Send 성공 → 즉시 RECEIVE 발행. SECP_TCG=0x01, COMID는 Discovery에서 얻은 값.
	 * 응답이 도착하면 opal_nvme_security_recv_done 콜백이 분할 처리까지 마무리. */
	if (ret) {
		/* [한국어] Receive 큐잉 실패 — 재시도 없이 사용자에게 즉시 에러 통보. */
		sess->sess_cb(sess, ret, sess->cb_arg);
	}
}

/*
 * [한국어]
 * opal_nvme_security_send - 비동기 SECURITY_SEND 발행 (콜백 등록 + admin 큐잉)
 *
 * @dev: 대상 Opal 디바이스 (ctrlr와 COMID 포함)
 * @sess: 명령 컨텍스트 — sess->cmd[]에 이미 TCG 토큰 스트림이 직렬화되어 있어야 함
 * @sess_cb: 최종 완료 콜백 (Send→Recv 체인 끝나면 호출)
 * @cb_arg: 콜백에 전달할 사용자 컨텍스트
 * @return: 0=큐잉 성공, 음수=실패 (-ENXIO/-ENOMEM 등)
 *
 * SECURITY_SEND를 NVMe Admin SQ에 enqueue하는 저수준 헬퍼. 실제 완료/응답 수령은
 * opal_nvme_security_send_done → opal_nvme_security_recv_done 체인에서 처리되며,
 * 사용자 콜백 sess_cb는 그 체인이 끝나야 호출된다.
 *
 * 호출 체인:
 *   opal_send_recv (또는 다른 워크플로) → [opal_nvme_security_send] →
 *     spdk_nvme_ctrlr_cmd_security_send → 큐잉 후 즉시 반환 (비동기)
 */
static int
opal_nvme_security_send(struct spdk_opal_dev *dev, struct opal_session *sess,
			opal_sess_cb sess_cb, void *cb_arg)
{
	sess->sess_cb = sess_cb;
	/* [한국어] 최종 완료 콜백 저장 — recv_done 콜백 체인 마지막에서 호출됨. */
	sess->cb_arg = cb_arg;
	/* [한국어] 사용자 컨텍스트 보관. */

	return spdk_nvme_ctrlr_cmd_security_send(dev->ctrlr, SPDK_SCSI_SECP_TCG, dev->comid,
			0, sess->cmd, IO_BUFFER_LENGTH,
			opal_nvme_security_send_done, sess);
	/* [한국어] NVMe opcode 0x81 (Security Send) admin 명령 발행.
	 *   SECP_TCG=0x01: Security Protocol = TCG (NVMe 1.x 5.25.2)
	 *   dev->comid: SPSP 필드 — Opal에서는 Communication ID
	 *   0: NSSF (NVMe Security Specific Field, Opal에서는 미사용)
	 *   sess->cmd: 송신 페이로드 (TCG token stream + 헤더)
	 *   IO_BUFFER_LENGTH(2048): 전체 버퍼 길이
	 * 큐잉만 하고 즉시 반환 — 완료는 콜백으로 통보. */
}

/*
 * [한국어]
 * opal_send_recv_done - opal_send_recv의 동기 polling 완료 콜백 (state flag 설정용 trivial 함수)
 *
 * @sess: 세션
 * @status: 최종 결과 (0=성공, 음수=에러)
 * @ctx: 미사용
 *
 * opal_send_recv가 spin polling으로 sess->done을 감시하므로, 이 콜백이 단순히
 * status를 sess->status에 저장하고 done=true로 표시. 별도 wakeup 메커니즘 불필요
 * (단일 스레드 동기 모델이라서 process_admin_completions가 직접 콜백을 호출).
 */
static void
opal_send_recv_done(struct opal_session *sess, int status, void *ctx)
{
	sess->status = status;
	/* [한국어] 최종 상태 저장 — 호출자가 sess->status로 결과 확인. */
	sess->done = true;
	/* [한국어] polling 종료 신호 — 같은 스레드라 memory barrier 불필요. */
}

/*
 * [한국어]
 * opal_send_recv - 동기 송수신 — TCG 명령을 보내고 응답이 올 때까지 polling 블록
 *
 * @dev: Opal 디바이스
 * @sess: cmd 버퍼에 직렬화된 명령이 들어 있는 세션
 * @return: 0=성공, 음수=에러 (NVMe 에러 또는 큐잉 실패)
 *
 * 이 함수가 모든 spdk_opal_cmd_* 공개 API의 동기 모델 핵심.
 * 비동기 콜백 모델 위에 spin-polling을 얹어 호출자에게 동기 인터페이스를 제공.
 *
 * 동작:
 *   1. sess->done = false로 리셋
 *   2. SECURITY_SEND 발행 (opal_send_recv_done이 최종 콜백으로 등록됨)
 *   3. sess->done이 true가 될 때까지 spdk_nvme_ctrlr_process_admin_completions polling
 *   4. sess->status 반환
 *
 * **주의**: 호출 스레드가 admin queue 폴링 권한을 가져야 함 (보통 단일 owner thread 모델).
 * 폴링 중에는 다른 admin 명령이 진행되지 않으므로 깊은 stack에서 호출하면 reactor 정체 위험.
 */
static int
opal_send_recv(struct spdk_opal_dev *dev, struct opal_session *sess)
{
	int ret;

	sess->done = false;
	/* [한국어] polling 시작 전 done 플래그 리셋. */
	ret = opal_nvme_security_send(dev, sess, opal_send_recv_done, NULL);
	/* [한국어] Send 발행 + 콜백으로 opal_send_recv_done 등록 — done=true 세팅용 trivial cb. */
	if (ret) {
		/* [한국어] 큐잉 자체 실패 시 polling 들어가지 않고 즉시 에러 반환. */
		return ret;
	}

	while (!sess->done) {
		/* [한국어] busy-wait spin — 다른 작업 없이 admin completions만 폴링.
		 * polled-mode의 정수: interrupt 없이 CPU 사이클로 응답 대기.
		 * Send→Recv 체인이 모두 끝나면 opal_send_recv_done이 done=true 세팅. */
		spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
		/* [한국어] admin CQ를 한 번 sweep — 도착한 CQE에 대해 콜백 디스패치. */
	}

	return sess->status;
	/* [한국어] 콜백이 저장한 최종 상태 반환 (NVMe 에러는 -EIO, 그 외는 큐잉 errno). */
}

/*
 * [한국어]
 * opal_alloc_session - opal_session 컨텍스트 동적 할당 (calloc + dev 바인딩)
 *
 * @dev: 부모 Opal 디바이스 — 세션이 어느 컨트롤러에 바인딩되는지
 * @return: 새 세션 포인터 또는 NULL (calloc 실패)
 *
 * 모든 spdk_opal_cmd_* 진입점이 이 함수로 세션을 만들고 종료 시 free.
 * 한 명령 워크플로(start_session → 작업 → end_session)당 한 세션.
 *
 * **메모리 비용**: sizeof(opal_session) ≈ 4KB+ (cmd[2048] + resp[2048] + parsed_resp).
 * calloc으로 모든 필드를 0으로 초기화 (특히 cmd_pos/hsn/tsn=0이 중요).
 */
static struct opal_session *
opal_alloc_session(struct spdk_opal_dev *dev)
{
	struct opal_session *sess;

	sess = calloc(1, sizeof(*sess));
	/* [한국어] heap allocation + zero — 모든 필드(hsn=0, tsn=0, cmd_pos=0, done=false)가 안전한 초기값. */
	if (!sess) {
		/* [한국어] OOM 시 NULL 반환 — 호출자가 -ENOMEM으로 변환. */
		return NULL;
	}
	sess->dev = dev;
	/* [한국어] 부모 디바이스 역참조 보관 — 콜백에서 dev->ctrlr/comid 접근에 사용. */

	return sess;
}

/*
 * [한국어]
 * opal_add_token_u8 - 1바이트 토큰을 cmd 버퍼에 추가 (TCG 단일 control token용)
 *
 * @err: in/out 에러 누적기 — 한 번이라도 0이 아니면 이후 호출은 모두 no-op (비제어 단축 평가)
 * @sess: 명령 세션 — sess->cmd[]에 직렬화하고 sess->cmd_pos를 전진시킴
 * @token: 1바이트 토큰 값 (예: SPDK_OPAL_STARTLIST=0xF0, SPDK_OPAL_ENDLIST=0xF1,
 *         SPDK_OPAL_STARTNAME=0xF2, SPDK_OPAL_ENDNAME=0xF3, SPDK_OPAL_CALL=0xF8,
 *         SPDK_OPAL_ENDOFDATA=0xF9, SPDK_OPAL_ENDOFSESSION=0xFA, SPDK_OPAL_TRUE=0x01)
 *
 * TCG SWG 토큰 스트림은 4종 atom (Tiny/Short/Medium/Long)과 6종 control token으로 구성.
 * 본 함수는 Tiny atom의 데이터 1바이트나 control token 1바이트를 그대로 buffer에 push.
 * err 누적 패턴으로 다수의 add_token_* 호출을 묶어도 첫 에러만 잡으면 됨 (cleaner code).
 */
static void
opal_add_token_u8(int *err, struct opal_session *sess, uint8_t token)
{
	if (*err) {
		/* [한국어] 이전 호출에서 이미 에러 — 이후 모든 add는 no-op으로 단축. */
		return;
	}
	if (sess->cmd_pos >= IO_BUFFER_LENGTH - 1) {
		/* [한국어] 버퍼 끝 도달 — 1바이트도 더 못 씀.
		 * IO_BUFFER_LENGTH=2048 / sizeof(opal_session.cmd) 한계 검사. */
		SPDK_ERRLOG("Error adding u8: end of buffer.\n");
		*err = -ERANGE;
		return;
	}
	sess->cmd[sess->cmd_pos++] = token;
	/* [한국어] 1바이트 push 후 cmd_pos++ — 다음 add는 이 바이트 다음에 직렬화. */
}

/*
 * [한국어]
 * opal_add_short_atom_header - Short atom 헤더(1바이트) 작성
 *
 * @sess: 명령 세션
 * @bytestring: true면 데이터가 byte string (예: PIN, UID), false면 정수
 * @has_sign: true면 signed integer
 * @len: payload 길이 (≤15바이트, SPDK_SHORT_ATOM_LEN_MASK=0x0F)
 *
 * Short Atom 헤더 비트 구조 (TCG SWG TLV):
 *   [7:6] = 10 (Short Atom 식별자, SPDK_SHORT_ATOM_ID=0x80)
 *   [5]   = bytestring 플래그 (SPDK_SHORT_ATOM_BYTESTRING_FLAG=0x20)
 *   [4]   = sign 플래그 (SPDK_SHORT_ATOM_SIGN_FLAG=0x10)
 *   [3:0] = payload 길이 (0~15)
 *
 * 헤더만 쓰고, 페이로드 자체는 호출자가 별도로 추가 (opal_add_token_bytestring 참고).
 */
static void
opal_add_short_atom_header(struct opal_session *sess, bool bytestring,
			   bool has_sign, size_t len)
{
	uint8_t atom;
	int err = 0;

	atom = SPDK_SHORT_ATOM_ID;
	/* [한국어] Short Atom base 비트 (0b10xxxxxx). */
	atom |= bytestring ? SPDK_SHORT_ATOM_BYTESTRING_FLAG : 0;
	/* [한국어] bytestring 데이터일 때 비트 5 set. */
	atom |= has_sign ? SPDK_SHORT_ATOM_SIGN_FLAG : 0;
	/* [한국어] signed integer일 때 비트 4 set — Opal에서는 거의 unsigned이므로 보통 0. */
	atom |= len & SPDK_SHORT_ATOM_LEN_MASK;
	/* [한국어] 하위 4비트에 길이 인코딩 (최대 15). */

	opal_add_token_u8(&err, sess, atom);
	/* [한국어] 1바이트 헤더 push — err은 내부 폐기 (호출자가 곧 다음 add에서 다시 검사). */
}

/*
 * [한국어]
 * opal_add_medium_atom_header - Medium atom 헤더(2바이트) 작성
 *
 * @sess: 명령 세션
 * @bytestring: bytestring 플래그
 * @has_sign: signed 플래그
 * @len: payload 길이 (16~2047바이트)
 *
 * Medium Atom 헤더 비트 구조:
 *   바이트 0: [7:5]=110 (SPDK_MEDIUM_ATOM_ID=0xC0), [4]=bytestring, [3]=sign,
 *             [2:0]=length high bits (총 11비트 length 중 상위 3비트)
 *   바이트 1: length의 하위 8비트
 *
 * Short Atom으로 표현 못 하는 16바이트 이상 데이터(예: 8바이트 UID, 32바이트 PIN)에 사용.
 * 본 함수는 buffer overflow를 검사하지 않음 — 호출자(opal_add_token_bytestring)가 사전 검증.
 */
static void
opal_add_medium_atom_header(struct opal_session *sess, bool bytestring,
			    bool has_sign, size_t len)
{
	uint8_t header;

	header = SPDK_MEDIUM_ATOM_ID;
	/* [한국어] Medium Atom base 비트 (0b110xxxxx). */
	header |= bytestring ? SPDK_MEDIUM_ATOM_BYTESTRING_FLAG : 0;
	/* [한국어] bytestring일 때 비트 4 set. */
	header |= has_sign ? SPDK_MEDIUM_ATOM_SIGN_FLAG : 0;
	/* [한국어] signed일 때 비트 3 set. */
	header |= (len >> 8) & SPDK_MEDIUM_ATOM_LEN_MASK;
	/* [한국어] length 상위 3비트를 헤더 바이트0의 [2:0]에 인코딩. */
	sess->cmd[sess->cmd_pos++] = header;
	/* [한국어] 첫 바이트 push. */
	sess->cmd[sess->cmd_pos++] = len;
	/* [한국어] 두 번째 바이트에 length 하위 8비트. */
}

/*
 * [한국어]
 * opal_add_token_bytestring - bytestring(byte 배열) 토큰을 cmd 버퍼에 추가
 *
 * @err: 누적 에러
 * @sess: 명령 세션
 * @bytestring: 추가할 바이트 데이터 (UID 8바이트, PIN, 등)
 * @len: 데이터 길이 (≤2047 권장)
 *
 * 길이에 따라 자동으로 Short(1B 헤더) vs Medium(2B 헤더) atom 헤더 선택 후 데이터 복사.
 * Opal에서 가장 자주 사용 — UID(8B)/PIN(가변)/method 식별자(8B) 등.
 *
 * 흐름:
 *   1. 에러 누적 검사
 *   2. len이 4비트 초과면 Medium atom으로 결정
 *   3. buffer overflow 사전 검사 (헤더 + 데이터)
 *   4. 헤더 작성 → 데이터 memcpy → cmd_pos 전진
 */
static void
opal_add_token_bytestring(int *err, struct opal_session *sess,
			  const uint8_t *bytestring, size_t len)
{
	size_t header_len = 1;
	/* [한국어] 디폴트는 Short Atom 헤더 1바이트. */
	bool is_short_atom = true;

	if (*err) {
		/* [한국어] 누적 에러 — short-circuit. */
		return;
	}

	if (len & ~SPDK_SHORT_ATOM_LEN_MASK) {
		/* [한국어] len이 SPDK_SHORT_ATOM_LEN_MASK(0x0F=15) 초과 → Short Atom으로 표현 불가.
		 * Medium Atom (2바이트 헤더, 최대 2047바이트)로 fallback. */
		header_len = 2;
		is_short_atom = false;
	}

	if (len >= IO_BUFFER_LENGTH - sess->cmd_pos - header_len) {
		/* [한국어] 헤더 + 데이터를 합한 크기가 남은 버퍼를 초과 → 거부.
		 * IO_BUFFER_LENGTH=2048, sess->cmd_pos는 현재 위치. */
		SPDK_ERRLOG("Error adding bytestring: end of buffer.\n");
		*err = -ERANGE;
		return;
	}

	if (is_short_atom) {
		opal_add_short_atom_header(sess, true, false, len);
		/* [한국어] bytestring=true, has_sign=false, len ≤15. */
	} else {
		opal_add_medium_atom_header(sess, true, false, len);
		/* [한국어] Medium 헤더 (2바이트). */
	}

	memcpy(&sess->cmd[sess->cmd_pos], bytestring, len);
	/* [한국어] 헤더 뒤에 데이터 raw copy. */
	sess->cmd_pos += len;
	/* [한국어] cmd_pos 전진. */
}

/*
 * [한국어]
 * opal_add_token_u64 - unsigned integer 토큰을 자동 폭 결정으로 추가
 *
 * @err: 누적 에러
 * @sess: 명령 세션
 * @number: 인코딩할 숫자 (0~UINT64_MAX)
 *
 * TCG SWG는 정수 인코딩 효율을 위해 4가지 폭을 지원:
 *   - Tiny atom (헤더만 1바이트, 0~63 범위, 헤더 [7]=0이면 unsigned tiny)
 *   - Short atom + 1바이트 길이 (256 미만): 헤더 0x81 (10000001 = bytestring=0, sign=0, len=1)
 *   - Short atom + 2바이트 길이 (65536 미만): 헤더 0x82
 *   - Short atom + 4바이트 길이 (2^32 미만): 헤더 0x84
 *   - Short atom + 8바이트 길이 (그 외): 헤더 0x88
 *
 * 정수 본체는 빅엔디안으로 인코딩 (high byte first).
 *
 * 사용 예: HSN/TSN 같은 32비트 세션 ID, locking range start/length 같은 64비트 LBA 값.
 */
static void
opal_add_token_u64(int *err, struct opal_session *sess, uint64_t number)
{
	int startat = 0;
	/* [한국어] 다바이트 인코딩 시 가장 상위 바이트 시프트량 (예: 4바이트면 startat=3). */

	if (*err) {
		return;
	}

	/* add header first */
	if (number <= SPDK_TINY_ATOM_DATA_MASK) {
		/* [한국어] 0~63 (Tiny atom 데이터 마스크 0x3F) — 헤더 1바이트만으로 표현.
		 * Tiny atom: [7]=0, [6]=sign(0=unsigned), [5:0]=값. */
		sess->cmd[sess->cmd_pos++] = (uint8_t) number & SPDK_TINY_ATOM_DATA_MASK;
	} else {
		if (number < 0x100) {
			/* [한국어] 1바이트로 표현 가능 (64~255) — Short Atom 헤더 0x81. */
			sess->cmd[sess->cmd_pos++] = 0x81; /* short atom, 1 byte length */
			startat = 0;
		} else if (number < 0x10000) {
			/* [한국어] 2바이트 (256~65535) — 헤더 0x82. */
			sess->cmd[sess->cmd_pos++] = 0x82; /* short atom, 2 byte length */
			startat = 1;
		} else if (number < 0x100000000) {
			/* [한국어] 4바이트 — 헤더 0x84. 일반적인 32비트 ID 표현. */
			sess->cmd[sess->cmd_pos++] = 0x84; /* short atom, 4 byte length */
			startat = 3;
		} else {
			/* [한국어] 8바이트 — 헤더 0x88. 64비트 LBA 등. */
			sess->cmd[sess->cmd_pos++] = 0x88; /* short atom, 8 byte length */
			startat = 7;
		}

		/* add number value */
		for (int i = startat; i > -1; i--) {
			/* [한국어] 빅엔디안 직렬화 — i가 startat → 0으로 감소하며 high byte → low byte 순. */
			sess->cmd[sess->cmd_pos++] = (uint8_t)((number >> (i * 8)) & 0xff);
		}
	}
}

/*
 * [한국어]
 * opal_add_tokens - 가변인자로 여러 1바이트 토큰을 한 번에 push
 *
 * @err: 누적 에러
 * @sess: 명령 세션
 * @num: 가변인자로 전달된 토큰 개수
 * @...: spdk_opal_token enum 값들 (예: SPDK_OPAL_STARTLIST, SPDK_OPAL_STARTNAME, …)
 *
 * 같은 패턴의 여러 control token을 한 줄에 나열할 때 가독성 향상.
 * 예: opal_add_tokens(&err, sess, 3, SPDK_OPAL_ENDNAME, SPDK_OPAL_STARTNAME, 3);
 *
 * 첫 에러 발생 시 즉시 break — 그 이후 토큰은 무시.
 */
static void
opal_add_tokens(int *err, struct opal_session *sess, int num, ...)
{
	int i;
	va_list args_ptr;
	/* [한국어] stdarg.h 가변인자 포인터. */
	enum spdk_opal_token tmp;

	va_start(args_ptr, num);
	/* [한국어] 마지막 명시 인자 num부터 가변 부분 시작. */

	for (i = 0; i < num; i++) {
		tmp = va_arg(args_ptr, enum spdk_opal_token);
		/* [한국어] enum은 int promote — va_arg 타입은 int 호환. */
		opal_add_token_u8(err, sess, tmp);
		/* [한국어] 1바이트씩 push (TCG control token은 모두 1B). */
		if (*err != 0) { break; }
		/* [한국어] 첫 에러에서 중단 — 이후 push 시도 안 함. */
	}

	va_end(args_ptr);
	/* [한국어] 가변인자 cleanup. */
}

/*
 * [한국어]
 * opal_cmd_finalize - 명령 직렬화 완료 — 헤더 길이/세션ID/패딩 모두 확정
 *
 * @sess: 페이로드가 모두 추가된 세션 (cmd[]에 직렬화 끝났고 cmd_pos가 마지막 위치)
 * @hsn: Host Session Number (start_session 응답에서 받은 값, 또는 0=세션 외)
 * @tsn: TPer Session Number (start_session 응답에서 받은 값, 또는 0)
 * @eod: true면 EndOfData + StartList[0,0,0]EndList 메서드 status placeholder 자동 추가
 * @return: 0=성공, -EFAULT=직렬화 실패, -ERANGE=버퍼 부족
 *
 * TCG Opal 패킷 3중 헤더(ComPacket / Packet / SubPacket)의 length 필드들을
 * BE32로 채우고, SubPacket을 4바이트 정렬되도록 0 패딩.
 *
 * 패킷 구조:
 *   ComPacket (20 byte): COMID, length(SubPacket+Packet 합)
 *   Packet (24 byte): session_tsn, session_hsn, length(SubPacket 길이)
 *   SubPacket (12 byte): kind=0(Data), length(payload만), payload(token stream)
 *
 * eod=true면 메서드 호출의 끝을 알리는 표준 토큰 시퀀스 추가:
 *   F9 (EndOfData) F0 (StartList) 00 00 00 F1 (EndList)
 *   여기서 0,0,0은 method status (호출자가 응답 측에서 받을 placeholder).
 *
 * **빅엔디안 변환**: TCG/NVMe 와이어는 모두 네트워크 바이트 오더이므로 to_be32 필수.
 */
static int
opal_cmd_finalize(struct opal_session *sess, uint32_t hsn, uint32_t tsn, bool eod)
{
	struct spdk_opal_header *hdr;
	int err = 0;

	if (eod) {
		/* [한국어] EndOfData 마커 + 메서드 status 4토큰 placeholder.
		 * Method 호출(call)의 마지막에 항상 따라오는 표준 시퀀스. */
		opal_add_tokens(&err, sess, 6, SPDK_OPAL_ENDOFDATA,
				SPDK_OPAL_STARTLIST,
				0, 0, 0,
				SPDK_OPAL_ENDLIST);
	}

	if (err) {
		/* [한국어] EOD 추가 중 버퍼 오버플로우 발생. */
		SPDK_ERRLOG("Error finalizing command.\n");
		return -EFAULT;
	}

	hdr = (struct spdk_opal_header *)sess->cmd;
	/* [한국어] 헤더는 cmd 버퍼 시작에 배치되어 있음. opal_clear_cmd가 cmd_pos를 sizeof(header)로
	 * 미리 옮겨놓아서 페이로드는 헤더 뒤에서부터 직렬화됨. */

	to_be32(&hdr->packet.session_tsn, tsn);
	/* [한국어] Packet 헤더의 TSN(TPer Session Number) BE32 인코딩 — 세션 시작 후엔 SSD가 발급한 값. */
	to_be32(&hdr->packet.session_hsn, hsn);
	/* [한국어] HSN(Host Session Number) BE32 — 호스트가 부여한 세션 ID. */

	to_be32(&hdr->sub_packet.length, sess->cmd_pos - sizeof(*hdr));
	/* [한국어] SubPacket payload 길이 = cmd_pos - 전체 헤더 크기 (token stream 자체 길이). */
	while (sess->cmd_pos % 4) {
		/* [한국어] SubPacket을 4바이트 경계 정렬 — TCG SWG 3.2.4 SubPacket padding 규칙.
		 * 길이 필드는 패딩 제외 실제 데이터 길이지만, 패킷 전체는 정렬되어야 함. */
		if (sess->cmd_pos >= IO_BUFFER_LENGTH) {
			SPDK_ERRLOG("Error: Buffer overrun\n");
			return -ERANGE;
		}
		sess->cmd[sess->cmd_pos++] = 0;
		/* [한국어] 0 패딩. */
	}
	to_be32(&hdr->packet.length, sess->cmd_pos - sizeof(hdr->com_packet) -
		sizeof(hdr->packet));
	/* [한국어] Packet 길이 = SubPacket 헤더+payload+padding 합. */
	to_be32(&hdr->com_packet.length, sess->cmd_pos - sizeof(hdr->com_packet));
	/* [한국어] ComPacket 길이 = Packet+SubPacket 전체 합 (ComPacket 자기 크기 제외). */

	return 0;
}

/*
 * [한국어]
 * opal_response_parse_tiny - Tiny atom(1바이트) 디코드
 *
 * @token: 출력 — 파싱된 토큰 메타 (pos, len=1, width, type, stored.unsigned_num)
 * @pos: 응답 버퍼 내 현재 위치
 * @return: 소비한 바이트 수 (항상 1)
 *
 * Tiny Atom: 1바이트 표현, [7]=0이면 tiny atom, [6]=signed, [5:0]=데이터(0~63).
 * Opal 응답에서 메서드 status 코드(0=Success, 1+=에러), boolean 등에 사용.
 */
static size_t
opal_response_parse_tiny(struct spdk_opal_resp_token *token,
			 const uint8_t *pos)
{
	token->pos = pos;
	/* [한국어] 원본 버퍼 위치 보관 — 후속 데이터 추출 시 참조. */
	token->len = 1;
	/* [한국어] Tiny atom은 항상 1바이트. */
	token->width = OPAL_WIDTH_TINY;

	if (pos[0] & SPDK_TINY_ATOM_SIGN_FLAG) {
		/* [한국어] 비트 6 set → signed integer. */
		token->type = OPAL_DTA_TOKENID_SINT;
	} else {
		/* [한국어] 비트 6 unset → unsigned integer. */
		token->type = OPAL_DTA_TOKENID_UINT;
		token->stored.unsigned_num = pos[0] & SPDK_TINY_ATOM_DATA_MASK;
		/* [한국어] 하위 6비트가 실제 값 (0~63) — 즉시 디코드 후 stored에 저장. */
	}

	return token->len;
}

/*
 * [한국어]
 * opal_response_parse_short - Short atom(1B 헤더 + 최대 15B 데이터) 디코드
 *
 * @token: 출력 토큰 메타
 * @pos: 응답 버퍼 위치
 * @return: 소비한 총 바이트 수, 또는 -EINVAL
 *
 * Short Atom: [7:6]=10 (식별자), [5]=bytestring, [4]=sign, [3:0]=length(0~15).
 * unsigned int인 경우 즉시 디코드해 stored.unsigned_num에 BE→host 변환.
 * bytestring(예: PIN, key)이면 token->pos+1부터 length 바이트가 raw 데이터.
 */
static int
opal_response_parse_short(struct spdk_opal_resp_token *token,
			  const uint8_t *pos)
{
	token->pos = pos;
	token->len = (pos[0] & SPDK_SHORT_ATOM_LEN_MASK) + 1; /* plus 1-byte header */
	/* [한국어] 헤더 [3:0]에서 데이터 길이 추출 + 헤더 1바이트. */
	token->width = OPAL_WIDTH_SHORT;

	if (pos[0] & SPDK_SHORT_ATOM_BYTESTRING_FLAG) {
		/* [한국어] bytestring (key/PIN/UID) — 즉시 디코드 안 하고 type만 표시. */
		token->type = OPAL_DTA_TOKENID_BYTESTRING;
	} else if (pos[0] & SPDK_SHORT_ATOM_SIGN_FLAG) {
		/* [한국어] signed integer. */
		token->type = OPAL_DTA_TOKENID_SINT;
	} else {
		/* [한국어] unsigned integer — 본체 즉시 디코드 (1~8바이트 BE → uint64). */
		uint64_t u_integer = 0;
		size_t i, b = 0;

		token->type = OPAL_DTA_TOKENID_UINT;
		if (token->len > 9) {
			/* [한국어] 데이터 8바이트 + 헤더 1바이트 = 9 초과면 uint64 범위 초과. */
			SPDK_ERRLOG("uint64 with more than 8 bytes\n");
			return -EINVAL;
		}
		for (i = token->len - 1; i > 0; i--) {
			/* [한국어] 마지막 바이트(LSB)부터 첫 데이터 바이트(MSB)까지 역순 순회.
			 * b는 LSB부터의 시프트량 — BE 디스크 → host order 변환. */
			u_integer |= ((uint64_t)pos[i] << (8 * b));
			b++;
		}
		token->stored.unsigned_num = u_integer;
		/* [한국어] 디코드된 값 stored에 저장. */
	}

	return token->len;
}

/*
 * [한국어]
 * opal_response_parse_medium - Medium atom(2B 헤더 + 최대 2047B 데이터) 디코드
 *
 * @token: 출력 토큰 메타
 * @pos: 응답 버퍼 위치
 * @return: 소비한 총 바이트 수
 *
 * Medium Atom: [7:5]=110, [4]=bytestring, [3]=sign, [2:0]=length high, [byte1]=length low.
 * 데이터 본체 디코드는 하지 않음 (호출자가 token->pos+2부터 raw 접근).
 */
static size_t
opal_response_parse_medium(struct spdk_opal_resp_token *token,
			   const uint8_t *pos)
{
	token->pos = pos;
	token->len = (((pos[0] & SPDK_MEDIUM_ATOM_LEN_MASK) << 8) | pos[1]) + 2; /* plus 2-byte header */
	/* [한국어] 11비트 length 디코드: 헤더 [2:0] << 8 | 두번째 바이트, + 헤더 2바이트. */
	token->width = OPAL_WIDTH_MEDIUM;

	if (pos[0] & SPDK_MEDIUM_ATOM_BYTESTRING_FLAG) {
		/* [한국어] bytestring (큰 데이터, e.g. 활성 키). */
		token->type = OPAL_DTA_TOKENID_BYTESTRING;
	} else if (pos[0] & SPDK_MEDIUM_ATOM_SIGN_FLAG) {
		token->type = OPAL_DTA_TOKENID_SINT;
	} else {
		token->type = OPAL_DTA_TOKENID_UINT;
	}

	return token->len;
}

/*
 * [한국어]
 * opal_response_parse_long - Long atom(4B 헤더 + 최대 16M 데이터) 디코드
 *
 * @token: 출력
 * @pos: 응답 위치
 * @return: 소비 바이트
 *
 * Long Atom: [7:4]=1110, [3]=bytestring, [2]=sign, [1:0]=reserved, [byte1:3]=24비트 length.
 * 매우 긴 페이로드 (사용 거의 없음 — IO_BUFFER_LENGTH=2048이라 실효 없음).
 */
static size_t
opal_response_parse_long(struct spdk_opal_resp_token *token,
			 const uint8_t *pos)
{
	token->pos = pos;
	token->len = ((pos[1] << 16) | (pos[2] << 8) | pos[3]) + 4; /* plus 4-byte header */
	/* [한국어] 24비트 length: byte1<<16 | byte2<<8 | byte3, + 헤더 4바이트. */
	token->width = OPAL_WIDTH_LONG;

	if (pos[0] & SPDK_LONG_ATOM_BYTESTRING_FLAG) {
		token->type = OPAL_DTA_TOKENID_BYTESTRING;
	} else if (pos[0] & SPDK_LONG_ATOM_SIGN_FLAG) {
		token->type = OPAL_DTA_TOKENID_SINT;
	} else {
		token->type = OPAL_DTA_TOKENID_UINT;
	}

	return token->len;
}

/*
 * [한국어]
 * opal_response_parse_token - Control token(STARTLIST/ENDLIST 등) 1바이트 디코드
 *
 * @token: 출력
 * @pos: 응답 위치
 * @return: 항상 1
 *
 * 0xF0~0xFF 범위의 control token은 의미만 있고 데이터 페이로드 없음.
 * STARTLIST(F0), ENDLIST(F1), STARTNAME(F2), ENDNAME(F3), CALL(F8), ENDOFDATA(F9),
 * ENDOFSESSION(FA) 등.
 */
static size_t
opal_response_parse_token(struct spdk_opal_resp_token *token,
			  const uint8_t *pos)
{
	token->pos = pos;
	token->len = 1;
	token->type = OPAL_DTA_TOKENID_TOKEN;
	/* [한국어] 토큰 타입 표시 — 호출자가 pos[0]으로 어떤 control token인지 분류. */
	token->width = OPAL_WIDTH_TOKEN;

	return token->len;
}

/*
 * [한국어]
 * opal_response_parse - 전체 SECURITY_RECEIVE 응답 버퍼를 토큰 배열로 분해
 *
 * @buf: SSD가 전송한 응답 버퍼 (sess->resp)
 * @length: 버퍼 가용 길이 (보통 IO_BUFFER_LENGTH=2048)
 * @resp: 출력 — resp_tokens[MAX_TOKS=64] 배열에 분해된 토큰들 저장
 * @return: 0=성공, -EINVAL/-EFAULT=헤더 손상 또는 길이 불일치
 *
 * 응답 페이로드 구조:
 *   [ComPacket 20B][Packet 24B][SubPacket 12B][token stream...]
 *
 * 동작:
 *   1. 헤더 length 필드 검증 (cp/pkt/subpkt 모두 비0, subpkt이 buf 한도 내)
 *   2. SubPacket payload(token stream)를 한 토큰씩 dispatch:
 *      - pos[0] <= 0x3F : tiny atom (range: 0x00-0x7F이지만 0x40-0x7F는 sign tiny)
 *      - pos[0] <= 0xBF : short atom (0x80-0xBF)
 *      - pos[0] <= 0xDF : medium atom (0xC0-0xDF)
 *      - pos[0] <= 0xE3 : long atom (0xE0-0xE3)
 *      - else : control token (0xE4-0xFF, 그러나 실제는 0xF0~0xFA)
 *   3. 각 토큰을 resp_tokens[]에 누적, num_entries 카운트
 *
 * 호출자: opal_parse_and_check_status가 단일 진입점.
 */
static int
opal_response_parse(const uint8_t *buf, size_t length,
		    struct spdk_opal_resp_parsed *resp)
{
	const struct spdk_opal_header *hdr;
	struct spdk_opal_resp_token *token_iter;
	/* [한국어] resp_tokens 배열 내 현재 슬롯 포인터. */
	int num_entries = 0;
	int total;
	/* [한국어] 남은 디코드 바이트 수 (signed로 음수 underflow 검출). */
	size_t token_length;
	const uint8_t *pos;
	uint32_t clen, plen, slen;

	if (!buf || !resp) {
		/* [한국어] NULL 체크 — 호출자 버그 방어. */
		return -EINVAL;
	}

	hdr = (struct spdk_opal_header *)buf;
	pos = buf + sizeof(*hdr);
	/* [한국어] 헤더 다음부터가 SubPacket payload (token stream). */

	clen = from_be32(&hdr->com_packet.length);
	/* [한국어] ComPacket length BE32 → host. */
	plen = from_be32(&hdr->packet.length);
	/* [한국어] Packet length BE32 → host. */
	slen = from_be32(&hdr->sub_packet.length);
	/* [한국어] SubPacket payload 길이 = 디코드해야 할 token stream 바이트 수. */
	SPDK_DEBUGLOG(opal, "Response size: cp: %u, pkt: %u, subpkt: %u\n",
		      clen, plen, slen);

	if (clen == 0 || plen == 0 || slen == 0 ||
	    slen > IO_BUFFER_LENGTH - sizeof(*hdr)) {
		/* [한국어] 모든 길이 필드는 비0이어야 하며, SubPacket이 버퍼를 넘으면 안 됨. */
		SPDK_ERRLOG("Bad header length. cp: %u, pkt: %u, subpkt: %u\n",
			    clen, plen, slen);
		return -EINVAL;
	}

	if (pos > buf + length) {
		/* [한국어] sizeof(header) > length — 응답이 헤더보다도 짧은 비정상 상황. */
		SPDK_ERRLOG("Pointer out of range\n");
		return -EFAULT;
	}

	token_iter = resp->resp_tokens;
	/* [한국어] 출력 배열 첫 슬롯부터 채우기 시작. */
	total = slen;
	/* [한국어] 디코드할 총 바이트 — 매 토큰마다 토큰 길이만큼 감소. */

	while (total > 0) {
		/* [한국어] 토큰 stream 끝까지 반복. */
		if (pos[0] <= SPDK_TINY_ATOM_TYPE_MAX) { /* tiny atom */
			/* [한국어] 0x00-0x7F (MSB=0) → Tiny Atom. */
			token_length = opal_response_parse_tiny(token_iter, pos);
		} else if (pos[0] <= SPDK_SHORT_ATOM_TYPE_MAX) { /* short atom */
			/* [한국어] 0x80-0xBF (10xxxxxx) → Short Atom. */
			token_length = opal_response_parse_short(token_iter, pos);
		} else if (pos[0] <= SPDK_MEDIUM_ATOM_TYPE_MAX) { /* medium atom */
			/* [한국어] 0xC0-0xDF (110xxxxx) → Medium Atom. */
			token_length = opal_response_parse_medium(token_iter, pos);
		} else if (pos[0] <= SPDK_LONG_ATOM_TYPE_MAX) { /* long atom */
			/* [한국어] 0xE0-0xE3 (1110000x) → Long Atom. */
			token_length = opal_response_parse_long(token_iter, pos);
		} else { /* TOKEN */
			/* [한국어] 0xF0-0xFA → Control Token (STARTLIST/ENDLIST/STARTNAME/...). */
			token_length = opal_response_parse_token(token_iter, pos);
		}

		if (token_length <= 0) {
			/* [한국어] 음수면 sub-parser가 에러 반환 (예: uint64 9바이트 초과). */
			SPDK_ERRLOG("Parse response failure.\n");
			return -EINVAL;
		}

		pos += token_length;
		/* [한국어] 다음 토큰으로 이동. */
		total -= token_length;
		/* [한국어] 남은 바이트 차감. */
		token_iter++;
		/* [한국어] 다음 출력 슬롯. */
		num_entries++;

		if (total < 0) {
			/* [한국어] 마지막 토큰이 SubPacket 경계를 넘어 길이 mismatch — 손상 응답. */
			SPDK_ERRLOG("Length not matching.\n");
			return -EINVAL;
		}
	}

	if (num_entries == 0) {
		/* [한국어] 한 토큰도 못 파싱 — 비어 있는 응답. */
		SPDK_ERRLOG("Couldn't parse response.\n");
		return -EINVAL;
	}
	resp->num = num_entries;
	/* [한국어] 파싱된 토큰 개수 기록 — opal_response_get_token에서 사용. */

	return 0;
}

/*
 * [한국어]
 * opal_response_token_matches - 인덱스 토큰이 특정 control token인지 일치 검사
 *
 * @token: 검사할 토큰
 * @match: 비교할 1바이트 (예: SPDK_OPAL_STARTLIST=0xF0)
 * @return: token이 control 종류이고 첫 바이트가 match와 같으면 true
 *
 * StartList/EndList/EndOfSession 등을 찾을 때 사용. opal_response_status에서 EOS를
 * 검출하거나 status list 시작점을 찾는 데 활용.
 */
static inline bool
opal_response_token_matches(const struct spdk_opal_resp_token *token,
			    uint8_t match)
{
	if (!token ||
	    token->type != OPAL_DTA_TOKENID_TOKEN ||
	    token->pos[0] != match) {
		/* [한국어] NULL이거나 control token이 아니거나 값 불일치 → no match. */
		return false;
	}
	return true;
}

/*
 * [한국어]
 * opal_response_get_token - 파싱된 응답 토큰 배열에서 인덱스 접근
 *
 * @resp: 파싱된 응답 구조체
 * @index: 0부터 resp->num-1까지의 인덱스
 * @return: 토큰 포인터 또는 NULL (out-of-range / zero-length)
 *
 * 안전한 배열 접근 wrapper — 범위 검사와 0길이 토큰 거부.
 */
static const struct spdk_opal_resp_token *
opal_response_get_token(const struct spdk_opal_resp_parsed *resp, int index)
{
	const struct spdk_opal_resp_token *token;

	if (index >= resp->num) {
		/* [한국어] 파싱된 토큰 수 초과 — caller에 NULL로 신호. */
		SPDK_ERRLOG("Token number doesn't exist: %d, resp: %d\n",
			    index, resp->num);
		return NULL;
	}

	token = &resp->resp_tokens[index];
	if (token->len == 0) {
		/* [한국어] len=0이면 비정상 (파서가 항상 ≥1로 채움). */
		SPDK_ERRLOG("Token length must be non-zero\n");
		return NULL;
	}

	return token;
}

/*
 * [한국어]
 * opal_response_get_u64 - 응답 토큰을 unsigned 64bit 정수로 추출
 *
 * @resp: 파싱된 응답
 * @index: 토큰 인덱스
 * @return: 정수값 (실패 시 0)
 *
 * Tiny 또는 Short width의 UINT 토큰만 허용 (Medium/Long은 데이터 본체가
 * 즉시 디코드되지 않으므로 이 함수로는 못 가져옴).
 *
 * Caller 예: opal_start_session_done에서 HSN/TSN 추출, lifecycle 상태 추출 등.
 */
static uint64_t
opal_response_get_u64(const struct spdk_opal_resp_parsed *resp, int index)
{
	if (!resp) {
		SPDK_ERRLOG("Response is NULL\n");
		return 0;
	}

	if (resp->resp_tokens[index].type != OPAL_DTA_TOKENID_UINT) {
		/* [한국어] 정수 타입이 아닌 경우 거부 (bytestring/sint 등). */
		SPDK_ERRLOG("Token is not unsigned int: %d\n",
			    resp->resp_tokens[index].type);
		return 0;
	}

	if (!(resp->resp_tokens[index].width == OPAL_WIDTH_TINY ||
	      resp->resp_tokens[index].width == OPAL_WIDTH_SHORT)) {
		/* [한국어] Tiny/Short만 stored.unsigned_num이 즉시 디코드되어 있음.
		 * Medium/Long uint는 거의 발생 안 함 — 발생 시 에러. */
		SPDK_ERRLOG("Atom is not short or tiny: %d\n",
			    resp->resp_tokens[index].width);
		return 0;
	}

	return resp->resp_tokens[index].stored.unsigned_num;
	/* [한국어] 파싱 시 미리 계산된 값 즉시 반환. */
}

/*
 * [한국어]
 * opal_response_get_u16 - u64 추출 후 16비트 범위 검사 wrapper
 *
 * @resp: 응답
 * @index: 토큰 인덱스
 * @return: 16비트 값 또는 0 (overflow 시)
 *
 * MaxRanges 같은 작은 정수 컬럼 추출용.
 */
static uint16_t
opal_response_get_u16(const struct spdk_opal_resp_parsed *resp, int index)
{
	uint64_t i = opal_response_get_u64(resp, index);
	if (i > 0xffffull) {
		/* [한국어] 16비트 초과 — 타입 mismatch 가능성. */
		SPDK_ERRLOG("parse response u16 failed. Overflow\n");
		return 0;
	}
	return (uint16_t) i;
}

/*
 * [한국어]
 * opal_response_get_u8 - u64 추출 후 8비트 범위 검사 wrapper
 *
 * @resp: 응답
 * @index: 토큰 인덱스
 * @return: 8비트 값
 *
 * read_locked/write_locked, lifecycle 상태 같은 1바이트 enum 값 추출용.
 */
static uint8_t
opal_response_get_u8(const struct spdk_opal_resp_parsed *resp, int index)
{
	uint64_t i = opal_response_get_u64(resp, index);
	if (i > 0xffull) {
		SPDK_ERRLOG("parse response u8 failed. Overflow\n");
		return 0;
	}
	return (uint8_t) i;
}

/*
 * [한국어]
 * opal_response_get_string - bytestring 토큰의 raw 데이터 포인터 + 길이 추출
 *
 * @resp: 응답
 * @n: 토큰 인덱스
 * @store: 출력 — 데이터 시작 포인터 (응답 버퍼 내부 위치, deep copy 아님)
 * @return: 데이터 길이 (바이트), 실패 시 0
 *
 * Short/Medium/Long bytestring 모두 지원. atom width에 따라 헤더 길이를 빼서
 * 실제 데이터만 가리키도록 포인터 조정.
 *
 * **주의**: 반환된 포인터는 sess->resp 버퍼 내부를 가리키므로, sess가 살아있는 동안만 유효.
 * 호출자 예: MSID PIN 추출(opal_get_msid_cpin_pin_done), 활성 키 추출(opal_get_active_key_done).
 */
static size_t
opal_response_get_string(const struct spdk_opal_resp_parsed *resp, int n,
			 const char **store)
{
	uint8_t header_len;
	struct spdk_opal_resp_token token;
	*store = NULL;
	if (!resp) {
		SPDK_ERRLOG("Response is NULL\n");
		return 0;
	}

	if (n > resp->num) {
		SPDK_ERRLOG("Response has %d tokens. Can't access %d\n",
			    resp->num, n);
		return 0;
	}

	token = resp->resp_tokens[n];
	if (token.type != OPAL_DTA_TOKENID_BYTESTRING) {
		/* [한국어] bytestring 아닌 경우 거부 — 정수에는 데이터 본체 없음. */
		SPDK_ERRLOG("Token is not a byte string!\n");
		return 0;
	}

	switch (token.width) {
	case OPAL_WIDTH_SHORT:
		/* [한국어] Short atom: 1B 헤더. */
		header_len = 1;
		break;
	case OPAL_WIDTH_MEDIUM:
		/* [한국어] Medium atom: 2B 헤더. */
		header_len = 2;
		break;
	case OPAL_WIDTH_LONG:
		/* [한국어] Long atom: 4B 헤더. */
		header_len = 4;
		break;
	default:
		/* [한국어] Tiny/control은 bytestring 가질 수 없음. */
		SPDK_ERRLOG("Can't get string from this Token\n");
		return 0;
	}

	*store = token.pos + header_len;
	/* [한국어] 데이터 시작 = 토큰 위치 + 헤더 크기. */
	return token.len - header_len;
	/* [한국어] 데이터 길이 = 전체 - 헤더. */
}

/*
 * [한국어]
 * opal_response_status - 메서드 호출 응답에서 status 코드 추출
 *
 * @resp: 파싱된 응답
 * @return: 0=성공(SPDK_OPAL_SUCCESS), 그 외 TCG 에러 코드 (예: 0x12=NOT_AUTHORIZED)
 *
 * TCG Opal 메서드 응답 마지막에는 "EndOfData StartList <status> 0 0 EndList" 시퀀스가
 * 따라온다. 첫 번째 status 값(보통 정수)이 메서드 호출 결과.
 *
 * 동작:
 *   1. 첫 토큰이 EndOfSession(0xFA)이면 (end_session 응답) 즉시 0 반환.
 *   2. 응답 끝에서부터 역방향으로 StartList/EndList 페어 탐색 — 응답 데이터 안에
 *      포함된 임의의 StartList/EndList와 혼동하지 않도록 마지막 페어를 사용.
 *   3. StartList 다음 토큰이 status 코드 (UINT).
 *
 * **TCG 상태 코드 예시**:
 *   0x00 SUCCESS
 *   0x01 NOT_AUTHORIZED
 *   0x05 SP_BUSY
 *   0x0C INVALID_PARAMETER
 *   0x12 AUTHORITY_LOCKED_OUT
 *   0x3F FAIL
 */
static int
opal_response_status(const struct spdk_opal_resp_parsed *resp)
{
	const struct spdk_opal_resp_token *tok;
	int startlist_idx = -1, endlist_idx = -1;

	/* if we get an EOS token, just return 0 */
	tok = opal_response_get_token(resp, 0);
	if (opal_response_token_matches(tok, SPDK_OPAL_ENDOFSESSION)) {
		/* [한국어] EndOfSession 응답 — end_session 명령에 대한 ack. status list 없음. */
		return 0;
	}

	/* Search for a StartList token in the response. Start from the end to ensure that we find
	 * the StartList token after the EndOfData token and not any StartList token that is part
	 * of the * actual response data. */
	for (int i = resp->num - 1; i >= 0; i--) {
		/* [한국어] 끝에서부터 탐색 — 메서드 인자 안의 nested list와 구분하기 위함.
		 * EndOfData 직후 status list가 항상 마지막에 위치. */
		tok = opal_response_get_token(resp, i);
		if (opal_response_token_matches(tok, SPDK_OPAL_STARTLIST) && startlist_idx == -1) {
			startlist_idx = i;
		}
		if (opal_response_token_matches(tok, SPDK_OPAL_ENDLIST) && endlist_idx == -1) {
			endlist_idx = i;
		}
	}

	if (startlist_idx == -1 || endlist_idx == -1 || startlist_idx >= endlist_idx) {
		/* [한국어] status list를 찾을 수 없음 — 비정상 응답. */
		return SPDK_DTAERROR_NO_METHOD_STATUS;
	}

	/* The second and third values in the status list are reserved, and are
	defined in core spec to be 0x00 and 0x00 and SHOULD be ignored by the host. */
	return (int)opal_response_get_u64(resp,
					  startlist_idx + 1); /* We only need the first value in the status list. */
	/* [한국어] StartList 바로 다음 토큰이 메서드 status. 두 번째, 세 번째는 reserved. */
}

/*
 * [한국어]
 * opal_parse_and_check_status - 응답 버퍼 파싱 + status 추출의 단일 진입점
 *
 * @sess: 응답이 도착한 세션
 * @return: 0=성공, NVMe 에러(음수) 또는 TCG 에러 코드(양수)
 *
 * 모든 워크플로의 마지막 단계에서 호출되는 표준 패턴 — 사실상 "응답 검증" 단축 함수.
 * 결과를 sess->parsed_resp에 저장해 후속 옵션 데이터 추출(예: HSN/TSN, key, MSID)에 활용 가능.
 */
static int
opal_parse_and_check_status(struct opal_session *sess)
{
	int error;

	error = opal_response_parse(sess->resp, IO_BUFFER_LENGTH, &sess->parsed_resp);
	/* [한국어] 응답 토큰 stream 파싱 → resp_tokens[] 배열에 누적. */
	if (error) {
		SPDK_ERRLOG("Couldn't parse response.\n");
		return error;
	}
	return opal_response_status(&sess->parsed_resp);
	/* [한국어] status 코드 추출 후 반환. */
}

/*
 * [한국어]
 * opal_clear_cmd - 새 명령 직렬화 시작 전 cmd 버퍼 리셋
 *
 * @sess: 명령 세션
 *
 * 모든 명령 빌더의 첫 단계 — 페이로드 영역 0클리어, cmd_pos를 헤더 다음 위치로 설정.
 * 헤더는 opal_cmd_finalize에서 마지막에 채워지므로 미리 비워둠.
 */
static inline void
opal_clear_cmd(struct opal_session *sess)
{
	sess->cmd_pos = sizeof(struct spdk_opal_header);
	/* [한국어] 직렬화 시작점을 헤더 다음으로 — 이전 명령 잔재 무시. */
	memset(sess->cmd, 0, IO_BUFFER_LENGTH);
	/* [한국어] 전체 버퍼 0클리어 — 잔존 데이터로 인한 SSD 펌웨어 혼란 방지. */
}

/*
 * [한국어]
 * opal_set_comid - ComPacket 헤더의 COMID 필드 인코딩
 *
 * @sess: 명령 세션
 * @comid: 사용할 Communication ID (Discovery에서 얻은 dev->comid)
 *
 * COMID는 호스트와 SSD 사이에 어떤 채널로 통신할지 식별 (TCG SWG 3.2.4).
 * Opal v2.x는 BaseComID 1개만 사용. extended_comid는 미사용 (0).
 */
static inline void
opal_set_comid(struct opal_session *sess, uint16_t comid)
{
	struct spdk_opal_header *hdr = (struct spdk_opal_header *)sess->cmd;

	hdr->com_packet.comid[0] = comid >> 8;
	/* [한국어] 빅엔디안 high byte. */
	hdr->com_packet.comid[1] = comid;
	/* [한국어] 빅엔디안 low byte. */
	hdr->com_packet.extended_comid[0] = 0;
	/* [한국어] Extended COMID 미사용 (Opal SSC). */
	hdr->com_packet.extended_comid[1] = 0;
}

/*
 * [한국어]
 * opal_init_key - C 문자열 패스워드를 spdk_opal_key 구조체에 복사
 *
 * @opal_key: 출력 — key[OPAL_KEY_MAX=256] 버퍼와 key_len에 채움
 * @passwd: NULL-terminated 패스워드 문자열
 * @return: 0=성공, -EINVAL=빈 문자열 또는 너무 김
 *
 * NULL/빈 문자열은 공장 디폴트 NULL key와 다른 의미라서 거부.
 * OPAL_KEY_MAX=256은 TCG SSC 2.0 spec의 PIN 컬럼 최대 크기.
 */
static inline int
opal_init_key(struct spdk_opal_key *opal_key, const char *passwd)
{
	int len;

	if (passwd == NULL || passwd[0] == '\0') {
		/* [한국어] 빈 패스워드 거부 — 공장 디폴트 키와 혼동 방지. */
		SPDK_ERRLOG("Password is empty. Create key failed\n");
		return -EINVAL;
	}

	len = strlen(passwd);

	if (len >= OPAL_KEY_MAX) {
		/* [한국어] 256 byte 제한 (NUL 포함) — TCG C_PIN 객체 PIN 컬럼 한도. */
		SPDK_ERRLOG("Password too long. Create key failed\n");
		return -EINVAL;
	}

	opal_key->key_len = len;
	memcpy(opal_key->key, passwd, opal_key->key_len);
	/* [한국어] raw bytes 복사 (NUL 미포함). 이후 SET_METHOD에서 bytestring으로 직렬화. */

	return 0;
}

/*
 * [한국어]
 * opal_build_locking_range - LockingRange UID 8바이트를 buffer에 작성
 *
 * @buffer: 출력 — OPAL_UID_LENGTH(8)바이트 UID 작성
 * @locking_range: 0=Global, 1~N=Range1..RangeN
 *
 * UID 인코딩:
 *   Global    : { 0x00, 0x00, 0x08, 0x02, 0x00, 0x00, 0x00, 0x01 }
 *   Range N>0 : { 0x00, 0x00, 0x08, 0x02, 0x00, 0x03, 0x00, N }  (byte5=0x03, byte7=N)
 *
 * Global Range는 디바이스 전체를 포괄하며, non-global Range는 특정 LBA 영역만 보호.
 */
static void
opal_build_locking_range(uint8_t *buffer, uint8_t locking_range)
{
	memcpy(buffer, spdk_opal_uid[UID_LOCKINGRANGE_GLOBAL], OPAL_UID_LENGTH);
	/* [한국어] Global LockingRange UID로 시작. */

	/* global */
	if (locking_range == 0) {
		/* [한국어] Range 0 = Global → 추가 변경 없이 그대로 반환. */
		return;
	}

	/* non-global */
	buffer[5] = LOCKING_RANGE_NON_GLOBAL;
	/* [한국어] byte5를 0x03으로 — non-global 식별자. */
	buffer[7] = locking_range;
	/* [한국어] byte7에 range 번호 — 1, 2, 3, … 식으로 인덱싱. */
}

/*
 * [한국어]
 * opal_check_tper - Level 0 Discovery의 TPer (Trusted Peripheral) Feature 캐시
 *
 * @dev: Opal 디바이스 — feat_info.tper에 복사
 * @data: TPer Feature 블록 raw 포인터 (Discovery payload 내부)
 *
 * TPer Feature: SyncSupported, AsyncSupported, AckNakSupported, BufferMgmtSupported,
 * StreamingSupported, ComIDMgmtSupported 등 TCG 프로토콜 capability bitfield.
 */
static void
opal_check_tper(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_opal_d0_tper_feat *tper = data;

	dev->feat_info.tper = *tper;
	/* [한국어] 구조체 통째 복사 — 이후 spdk_opal_get_d0_features_info로 외부 노출. */
}

/*
 * check single user mode
 */
/*
 * [한국어]
 * opal_check_sum - SUM(Single User Mode) Feature 검증 + 캐시
 *
 * @dev: 디바이스
 * @data: SUM Feature 블록
 * @return: true=SUM 사용 가능 (locking_objects ≥ 1), false=불가
 *
 * Single User Mode는 디바이스의 모든 Locking Range를 한 사용자(Admin1)만 관리하는 모드.
 * num_locking_objects가 0이면 SUM 자체가 의미 없음 (range 없는 디바이스).
 */
static bool
opal_check_sum(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_opal_d0_single_user_mode_feat *sum = data;
	uint32_t num_locking_objects = from_be32(&sum->num_locking_objects);
	/* [한국어] SUM이 관리할 수 있는 locking object 개수 (BE32). */

	if (num_locking_objects == 0) {
		/* [한국어] SUM 사용 불가 — 호출자에게 false로 알림. */
		SPDK_NOTICELOG("Need at least one locking object.\n");
		return false;
	}

	dev->feat_info.single_user = *sum;

	return true;
}

/*
 * [한국어]
 * opal_check_lock - Locking Feature 캐시
 *
 * @dev: 디바이스
 * @data: Locking Feature 블록
 *
 * LockingSupported, LockingEnabled, Locked, MediaEncryption, MBREnabled, MBRDone
 * 같은 잠금 상태 비트필드. 디바이스가 SED로 활성화되어 있는지 확인용.
 */
static void
opal_check_lock(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_opal_d0_locking_feat *lock = data;

	dev->feat_info.locking = *lock;
}

/*
 * [한국어]
 * opal_check_geometry - Geometry Feature 캐시 (LBA 정렬, MBR 영역 크기 등)
 *
 * @dev: 디바이스
 * @data: Geometry Feature 블록
 */
static void
opal_check_geometry(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_opal_d0_geo_feat *geo = data;

	dev->feat_info.geo = *geo;
}

/*
 * [한국어]
 * opal_check_datastore - DataStore Feature 캐시 (Datastore 테이블 크기/개수)
 *
 * @dev: 디바이스
 * @data: DataStore Feature 블록
 *
 * Datastore는 호스트가 임의 데이터를 SSD 보안 영역에 저장할 수 있는 테이블.
 * 인증 정책이나 키 wrapping 메타데이터 보관에 활용 (Opal 2.0 옵션).
 */
static void
opal_check_datastore(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_opal_d0_datastore_feat *datastore = data;

	dev->feat_info.datastore = *datastore;
}

/*
 * [한국어]
 * opal_get_comid_v100 - Opal v1.00 Feature에서 BaseComID 추출
 *
 * @dev: 디바이스 — feat_info.v100에 복사
 * @data: Opal v1.00 Feature 블록
 * @return: BaseComID (이후 모든 명령의 COMID 헤더에 사용)
 *
 * Opal v1.00은 구버전. v2.00이 우선이지만 백워드 호환성을 위해 둘 다 지원.
 */
static uint16_t
opal_get_comid_v100(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_opal_d0_v100_feat *v100 = data;
	uint16_t base_comid = from_be16(&v100->base_comid);
	/* [한국어] BaseComID BE16 → host. SSD가 할당한 통신 채널 ID. */

	dev->feat_info.v100 = *v100;

	return base_comid;
}

/*
 * [한국어]
 * opal_get_comid_v200 - Opal v2.00 Feature에서 BaseComID 추출
 *
 * @dev: 디바이스
 * @data: Opal v2.00 Feature 블록
 * @return: BaseComID
 *
 * Opal v2.00 = SED 표준 현행 버전 (TCG Storage Opal SSC v2.00).
 * v100과 v200이 모두 광고되면 v200이 우선 사용 (opal_discovery0_end의 switch case 순서).
 */
static uint16_t
opal_get_comid_v200(struct spdk_opal_dev *dev, const void *data)
{
	const struct spdk_opal_d0_v200_feat *v200 = data;
	uint16_t base_comid = from_be16(&v200->base_comid);

	dev->feat_info.v200 = *v200;

	return base_comid;
}

/*
 * [한국어]
 * opal_discovery0_end - Level 0 Discovery payload를 feature 단위로 파싱
 *
 * @dev: Opal 디바이스 — feat_info와 comid를 채움
 * @payload: SECURITY_RECEIVE로 받은 Level 0 Discovery 응답
 * @payload_size: 버퍼 가용 크기 (보통 IO_BUFFER_LENGTH=2048)
 * @return: 0=Opal 지원 확인 + COMID 획득, -ENOTSUP=Opal 미지원, -EFAULT=헤더 손상
 *
 * Level 0 Discovery 응답 구조:
 *   [Discovery Header (48B): length, version, ...] [Feature 0 hdr+data] [Feature 1 hdr+data] ...
 *   각 Feature: [code(2B) + version/rsvd(1B) + length(1B)] + payload
 *
 * 동작:
 *   1. 헤더 길이 검증
 *   2. 각 Feature를 walk하면서 code별로 dispatch (TPer/Locking/Geometry/SUM/Datastore/v100/v200)
 *   3. Opal v100 또는 v200을 발견하면 BaseComID 획득 + supported=true
 *   4. dev->comid에 저장 (이후 모든 명령이 이 COMID 사용)
 *
 * Feature Code 상수 (TCG Opal SSC v2.0 Table 2):
 *   0x0001 TPer, 0x0002 Locking, 0x0003 Geometry, 0x0200 Opal SSC v1.00, 0x0203 Opal SSC v2.00,
 *   0x0202 SingleUserMode, 0x0202+ DataStore.
 */
static int
opal_discovery0_end(struct spdk_opal_dev *dev, void *payload, uint32_t payload_size)
{
	bool supported = false, single_user = false;
	const struct spdk_opal_d0_hdr *hdr = (struct spdk_opal_d0_hdr *)payload;
	/* [한국어] payload 시작 = Discovery 헤더. */
	struct spdk_opal_d0_feat_hdr *feat_hdr;
	const uint8_t *epos = payload, *cpos = payload;
	/* [한국어] epos = end position, cpos = current position. */
	uint16_t comid = 0;
	uint32_t hlen = from_be32(&(hdr->length));
	/* [한국어] Discovery payload 총 길이 (BE32). */

	if (hlen > payload_size - sizeof(*hdr)) {
		/* [한국어] 헤더가 광고한 길이가 실제 버퍼를 초과 — 손상된 응답. */
		SPDK_ERRLOG("Discovery length overflows buffer (%zu+%u)/%u\n",
			    sizeof(*hdr), hlen, payload_size);
		return -EFAULT;
	}

	epos += hlen; /* end of buffer */
	/* [한국어] 끝 포인터 = 시작 + 광고된 길이. */
	cpos += sizeof(*hdr); /* current position on buffer */
	/* [한국어] 첫 Feature 시작 = 헤더 다음. */

	while (cpos < epos) {
		/* [한국어] Feature 단위 walk. */
		feat_hdr = (struct spdk_opal_d0_feat_hdr *)cpos;
		uint16_t feat_code = from_be16(&feat_hdr->code);
		/* [한국어] Feature code BE16 → host. */

		switch (feat_code) {
		case FEATURECODE_TPER:
			/* [한국어] TPer Feature 0x0001. */
			opal_check_tper(dev, cpos);
			break;
		case FEATURECODE_SINGLEUSER:
			/* [한국어] SUM Feature. */
			single_user = opal_check_sum(dev, cpos);
			break;
		case FEATURECODE_GEOMETRY:
			/* [한국어] Geometry Feature. */
			opal_check_geometry(dev, cpos);
			break;
		case FEATURECODE_LOCKING:
			/* [한국어] Locking Feature. */
			opal_check_lock(dev, cpos);
			break;
		case FEATURECODE_DATASTORE:
			/* [한국어] DataStore Feature. */
			opal_check_datastore(dev, cpos);
			break;
		case FEATURECODE_OPALV100:
			/* [한국어] Opal SSC v1.00 — 구버전. supported=true 마킹. */
			comid = opal_get_comid_v100(dev, cpos);
			supported = true;
			break;
		case FEATURECODE_OPALV200:
			/* [한국어] Opal SSC v2.00 — 표준. v100을 덮어씀 (v200이 후순위라 우선). */
			comid = opal_get_comid_v200(dev, cpos);
			supported = true;
			break;
		default:
			/* [한국어] 알 수 없는 Feature — 무시하고 다음으로. */
			SPDK_INFOLOG(opal, "Unknown feature code: %d\n", feat_code);
		}
		cpos += feat_hdr->length + sizeof(*feat_hdr);
		/* [한국어] 다음 Feature로 이동 — feat_hdr.length는 Feature 데이터 길이 (헤더 제외). */
	}

	if (supported == false) {
		/* [한국어] Opal v100/v200 둘 다 발견 못함 → SED 미지원. */
		SPDK_INFOLOG(opal, "Opal Not Supported.\n");
		return -ENOTSUP;
	}

	if (single_user == false) {
		/* [한국어] SUM 미지원 정보성 로그 (필수 아님). */
		SPDK_INFOLOG(opal, "Single User Mode Not Supported\n");
	}

	dev->comid = comid;
	/* [한국어] 이후 모든 명령에서 사용할 BaseComID 저장. */
	return 0;
}

/*
 * [한국어]
 * opal_discovery0 - Opal Level 0 Discovery 절차 전체 (SECP_INFO + SECP_TCG L0)
 *
 * @dev: 디바이스
 * @payload: 임시 버퍼 (호출자가 제공)
 * @payload_size: 버퍼 크기
 * @return: 0=Opal 디바이스 확인, -ENOTSUP=비지원, NVMe 에러
 *
 * 2단계 절차:
 *   Stage 1: SECURITY_RECEIVE(SECP_INFO=0x00, SPSP=0)로 지원 Security Protocol 목록 조회.
 *            응답 형식 (SPC-4 7.7.1.3):
 *              offset 6-7: list length BE16
 *              offset 8+ : 지원 protocol ID 1바이트씩 (예: 0x01=TCG, 0xEA=ATA Security)
 *            여기서 SECP_TCG(0x01)가 있는지 확인 — 없으면 Opal 자체 불가.
 *
 *   Stage 2: SECURITY_RECEIVE(SECP_TCG=0x01, SPSP=LV0_DISCOVERY_COMID=0x0001)로
 *            Level 0 Discovery payload 수령 → opal_discovery0_end로 파싱.
 *
 * Caller: spdk_opal_dev_construct에서 한 번만 호출.
 */
static int
opal_discovery0(struct spdk_opal_dev *dev, void *payload, uint32_t payload_size)
{
	int ret;
	uint16_t i, sp_list_len;
	uint8_t *sp_list;
	bool sp_tcg_supported = false;

	/* NVMe 1.4 chapter 5.25.2 Security Protocol 00h */
	ret = spdk_nvme_ctrlr_security_receive(dev->ctrlr, SPDK_SCSI_SECP_INFO, 0,
					       0, payload, payload_size);
	/* [한국어] Stage 1: SECP_INFO 요청 — 동기 헬퍼 (내부에서 process_admin_completions 폴링). */
	if (ret) {
		return ret;
	}

	/* spc4r31 chapter 7.7.1.3 Supported security protocols list description */
	sp_list_len = from_be16((uint8_t *)payload + 6);
	/* [한국어] offset 6-7: 지원 protocol 개수 (BE16). */
	sp_list = (uint8_t *)payload + 8;
	/* [한국어] offset 8 이후: protocol ID 배열. */

	if (sp_list_len + 8 > (int)payload_size) {
		/* [한국어] 광고된 list가 버퍼를 넘으면 손상 응답. */
		return -EINVAL;
	}

	for (i = 0; i < sp_list_len; i++) {
		if (sp_list[i] == SPDK_SCSI_SECP_TCG) {
			/* [한국어] SECP_TCG=0x01 발견 — Opal 가능. */
			sp_tcg_supported = true;
			break;
		}
	}

	if (!sp_tcg_supported) {
		/* [한국어] TCG 보안 protocol 미지원 → Opal 불가. */
		return -ENOTSUP;
	}

	memset(payload, 0, payload_size);
	/* [한국어] Stage 2 전에 버퍼 클리어. */
	ret = spdk_nvme_ctrlr_security_receive(dev->ctrlr, SPDK_SCSI_SECP_TCG, LV0_DISCOVERY_COMID,
					       0, payload, payload_size);
	/* [한국어] Stage 2: TCG 프로토콜의 SPSP=0x0001(Level 0 Discovery)로 정보 요청. */
	if (ret) {
		return ret;
	}

	return opal_discovery0_end(dev, payload, payload_size);
	/* [한국어] payload 파싱 — Feature 발견 + dev->comid 설정. */
}

/*
 * [한국어]
 * opal_end_session - 활성 세션 종료 (EndOfSession 토큰만 보내 HSN/TSN 무효화)
 *
 * @dev: 디바이스
 * @sess: 종료할 세션 (hsn/tsn이 0이 아닌 활성 세션)
 * @comid: 사용 중인 COMID
 * @return: 0=성공, NVMe/TCG 에러
 *
 * TCG SWG 세션 종료 절차: SubPacket 페이로드를 단일 EndOfSession(0xFA) 토큰만 담아
 * 보내면 SSD가 세션을 destroy하고 ack로 EOS 응답.
 *
 * 호출 후 sess->hsn/tsn=0으로 리셋 — 같은 sess 구조체로 새 세션을 다시 열 수 있게 함.
 *
 * 호출 체인: 모든 spdk_opal_cmd_*가 작업 완료 후 cleanup으로 호출.
 */
static int
opal_end_session(struct spdk_opal_dev *dev, struct opal_session *sess, uint16_t comid)
{
	int err = 0;
	int ret;

	opal_clear_cmd(sess);
	/* [한국어] cmd 버퍼 리셋 — 이전 명령 잔재 제거. */
	opal_set_comid(sess, comid);
	/* [한국어] ComPacket 헤더의 COMID 필드 채움. */
	opal_add_token_u8(&err, sess, SPDK_OPAL_ENDOFSESSION);
	/* [한국어] 단일 EOS(0xFA) 토큰 — 메서드 호출 형식이 아닌 직접 종료 신호. */

	if (err < 0) {
		return err;
	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, false);
	/* [한국어] eod=false — EOS는 자체 종료 토큰이라 EndOfData 시퀀스 불필요. */
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	/* [한국어] 동기 송수신 — SSD가 세션 destroy ack 반환. */
	if (ret) {
		return ret;
	}

	sess->hsn = 0;
	/* [한국어] HSN 무효화 — 같은 sess로 새 세션 시작 가능. */
	sess->tsn = 0;

	return opal_parse_and_check_status(sess);
	/* [한국어] EOS 응답 검증 — opal_response_status가 EOS 토큰을 보고 0 반환. */
}

/*
 * [한국어]
 * spdk_opal_dev_destruct - Opal 디바이스 컨텍스트 해제 (공개 API)
 *
 * @dev: spdk_opal_dev_construct로 생성된 디바이스
 *
 * 단순 free — ctrlr 자체는 NVMe 드라이버 소유라 여기서 해제 안 함.
 * 활성 세션이 있다면 호출자가 spdk_opal_cmd_*로 미리 종료해야 함.
 */
void
spdk_opal_dev_destruct(struct spdk_opal_dev *dev)
{
	free(dev);
	/* [한국어] dev 구조체와 그 안의 feat_info/locking_ranges 캐시 해제.
	 * NVMe ctrlr 핸들은 외부 소유이므로 건드리지 않음. */
}

/*
 * [한국어]
 * opal_start_session_done - StartSession 응답에서 HSN/TSN 추출 후 sess에 저장
 *
 * @sess: 응답이 도착한 세션
 * @return: 0=성공, -EPERM=인증 실패, 그 외 NVMe/TCG 에러
 *
 * StartSession 메서드 응답 형식 (TCG SSC v2.0):
 *   index 0: STARTLIST(F0) (응답 시작)
 *   index 1: STARTLIST 또는 메서드 결과 마커
 *   index 2,3: ...
 *   index 4: HSN (UINT) ← 호스트가 시작 시 보낸 값과 같음
 *   index 5: TSN (UINT) ← SSD가 발급한 새 세션 ID
 *
 * HSN+TSN 둘 다 0이면 인증 실패 (잘못된 PIN 또는 Authority).
 */
static int
opal_start_session_done(struct opal_session *sess)
{
	uint32_t hsn, tsn;
	int error = 0;

	error = opal_parse_and_check_status(sess);
	/* [한국어] 응답 검증 — TCG status 코드가 0이어야 함. */
	if (error) {
		return error;
	}

	hsn = opal_response_get_u64(&sess->parsed_resp, 4);
	/* [한국어] 인덱스 4 = HSN (호스트가 보낸 값을 SSD가 echo). */
	tsn = opal_response_get_u64(&sess->parsed_resp, 5);
	/* [한국어] 인덱스 5 = TSN (SSD가 새로 발급한 TPer Session Number). */

	if (hsn == 0 && tsn == 0) {
		/* [한국어] 둘 다 0 = 인증 실패 (잘못된 PIN). */
		SPDK_ERRLOG("Couldn't authenticate session\n");
		return -EPERM;
	}

	sess->hsn = hsn;
	/* [한국어] 이후 모든 명령의 Packet 헤더에 사용. */
	sess->tsn = tsn;

	return 0;
}

/*
 * [한국어]
 * opal_start_generic_session - SMUID에 StartSession 메서드 호출 (Admin SP 세션 개시)
 *
 * @dev: 디바이스
 * @sess: 새 세션 — 시작 후 sess->hsn/tsn에 발급된 ID 저장됨
 * @auth: 권한 객체 UID (UID_ANYBODY=인증 없음, UID_SID=공장 SID, UID_ADMIN1=Locking SP Admin1)
 * @sp_type: 대상 SP UID (UID_ADMINSP / UID_LOCKINGSP)
 * @key: PIN 바이트 (UID_ANYBODY일 땐 NULL 허용)
 * @key_len: PIN 길이
 * @return: 0=세션 개시 + HSN/TSN 획득, OPAL_INVAL_PARAM=인자 오류, NVMe/TCG 에러
 *
 * TCG SSC v2.0 StartSession 메서드 호출 형식 (의사 코드):
 *   CALL (0xF8)
 *   <SMUID 8B>                       ← invokingId = Session Manager UID
 *   <STARTSESSION_METHOD UID 8B>     ← methodId
 *   STARTLIST (0xF0)
 *     <HSN uint>                     ← 호스트가 부여하는 세션 ID (GENERIC_HOST_SESSION_NUM=0x69)
 *     <SP UID 8B>                    ← 어느 Security Provider에 세션을 열지
 *     TRUE (0x01)                    ← Write 세션 (Read-only false=0x00)
 *     [STARTNAME 0 <HostChallenge bytestring> ENDNAME]   ← optional PIN
 *     [STARTNAME 3 <HostSignAuth UID 8B> ENDNAME]        ← optional 서명자 권한 UID
 *   ENDLIST (0xF1)
 *   EndOfData + status placeholder (opal_cmd_finalize가 자동 추가)
 *
 * UID_ANYBODY: PIN/Authority 없이 비로그인 세션 (예: TakeOwnership 첫 단계, MSID 읽기용).
 * UID_SID: 공장 디폴트 SID 권한으로 AdminSP 로그인 (TakeOwnership에서 SID 변경 시).
 * UID_ADMIN1: LockingSP의 Admin1 — locking range 활성화/관리.
 */
static int
opal_start_generic_session(struct spdk_opal_dev *dev,
			   struct opal_session *sess,
			   enum opal_uid_enum auth,
			   enum opal_uid_enum sp_type,
			   const char *key,
			   uint8_t key_len)
{
	uint32_t hsn;
	int err = 0;
	int ret;

	if (key == NULL && auth != UID_ANYBODY) {
		/* [한국어] ANYBODY가 아닌 권한은 PIN 필수. */
		return OPAL_INVAL_PARAM;
	}

	opal_clear_cmd(sess);
	/* [한국어] cmd 버퍼 리셋. */

	opal_set_comid(sess, dev->comid);
	/* [한국어] 헤더 COMID 채움. */
	hsn = GENERIC_HOST_SESSION_NUM;
	/* [한국어] 호스트 세션 번호 = 0x69 (관습적 상수, 모든 generic 세션에 같은 값 사용). */

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	/* [한국어] CALL 토큰 (0xF8) — 메서드 호출 시작. */
	opal_add_token_bytestring(&err, sess, spdk_opal_uid[UID_SMUID],
				  OPAL_UID_LENGTH);
	/* [한국어] invokingId = SMUID (Session Manager UID, {0,…,0xff}) — 세션 시작 메서드의 진입점. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[STARTSESSION_METHOD],
				  OPAL_UID_LENGTH);
	/* [한국어] methodId = StartSession ({0,0,0,0,0,0,0xff,0x02}). */
	opal_add_token_u8(&err, sess, SPDK_OPAL_STARTLIST);
	/* [한국어] 인자 리스트 시작. */
	opal_add_token_u64(&err, sess, hsn);
	/* [한국어] 첫 인자: HSN. */
	opal_add_token_bytestring(&err, sess, spdk_opal_uid[sp_type], OPAL_UID_LENGTH);
	/* [한국어] 두번째 인자: SP UID (Admin SP 또는 Locking SP). */
	opal_add_token_u8(&err, sess, SPDK_OPAL_TRUE); /* Write */
	/* [한국어] 세번째 인자: Write=true (R/W 세션). */

	switch (auth) {
	case UID_ANYBODY:
		/* [한국어] 익명 세션 — PIN/Authority 인자 생략, 바로 EndList. */
		opal_add_token_u8(&err, sess, SPDK_OPAL_ENDLIST);
		break;
	case UID_ADMIN1:
	case UID_SID:
		/* [한국어] 인증 세션 — HostChallenge(PIN) + HostSignAuth(권한 UID) 추가. */
		opal_add_token_u8(&err, sess, SPDK_OPAL_STARTNAME);
		/* [한국어] Named parameter 시작. */
		opal_add_token_u8(&err, sess, 0); /* HostChallenge */
		/* [한국어] 파라미터 ID 0 = HostChallenge (PIN 페이로드). */
		opal_add_token_bytestring(&err, sess, key, key_len);
		/* [한국어] PIN 자체. SSD가 내부에서 C_PIN과 비교. */
		opal_add_tokens(&err, sess, 3,    /* number of token */
				SPDK_OPAL_ENDNAME,
				SPDK_OPAL_STARTNAME,
				3);/* HostSignAuth */
		/* [한국어] 첫 named param 종료 → 새 named param 시작 → 파라미터 ID 3 = HostSignAuth. */
		opal_add_token_bytestring(&err, sess, spdk_opal_uid[auth],
					  OPAL_UID_LENGTH);
		/* [한국어] HostSignAuth 값 = Authority UID (SID 또는 Admin1). */
		opal_add_token_u8(&err, sess, SPDK_OPAL_ENDNAME);
		/* [한국어] HostSignAuth 종료. */
		opal_add_token_u8(&err, sess, SPDK_OPAL_ENDLIST);
		/* [한국어] 인자 리스트 종료. */
		break;
	default:
		/* [한국어] 지원하지 않는 권한 타입. */
		SPDK_ERRLOG("Cannot start Admin SP session with auth %d\n", auth);
		return -EINVAL;
	}

	if (err) {
		/* [한국어] 토큰 직렬화 중 버퍼 오버플로우 발생. */
		SPDK_ERRLOG("Error building start adminsp session command.\n");
		return err;
	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	/* [한국어] eod=true — EndOfData + status placeholder 추가, 헤더 길이 채우기.
	 * 이때 sess->hsn/tsn=0 (아직 세션 미개시) → 헤더 HSN/TSN 0으로 송신. */
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	/* [한국어] 동기 송수신. */
	if (ret) {
		return ret;
	}

	return opal_start_session_done(sess);
	/* [한국어] 응답에서 HSN/TSN 추출 + sess에 저장. */
}

/*
 * [한국어]
 * opal_get_msid_cpin_pin_done - C_PIN_MSID GET 응답에서 공장 디폴트 PIN 추출
 *
 * @sess: 응답이 도착한 세션
 * @opal_key: 출력 — MSID PIN을 key/key_len에 저장
 * @return: 0=성공, NVMe/TCG 에러
 *
 * MSID(Manufacturer SID)는 SSD 출하 시 펌웨어에 박혀있는 공장 디폴트 SID PIN.
 * 보통 사용자 매뉴얼에 적혀 있거나 IDENTIFY로 조회 가능. 첫 TakeOwnership에서 이를
 * 사용해 SID 세션 시작 → 새 PIN으로 변경하는 것이 표준 절차.
 *
 * 응답 형식:
 *   index 4: MSID PIN (bytestring) — Get 메서드의 결과 컬럼
 */
static int
opal_get_msid_cpin_pin_done(struct opal_session *sess,
			    struct spdk_opal_key *opal_key)
{
	const char *msid_pin;
	size_t strlen;
	int error = 0;

	error = opal_parse_and_check_status(sess);
	/* [한국어] 응답 파싱 + status=0 확인. */
	if (error) {
		return error;
	}

	strlen = opal_response_get_string(&sess->parsed_resp, 4, &msid_pin);
	/* [한국어] index 4의 bytestring → MSID PIN raw 포인터 추출 (sess->resp 내부 위치). */
	if (!msid_pin) {
		SPDK_ERRLOG("Couldn't extract PIN from response\n");
		return -EINVAL;
	}

	opal_key->key_len = strlen;
	memcpy(opal_key->key, msid_pin, opal_key->key_len);
	/* [한국어] PIN을 호출자 소유 버퍼로 복사 — sess가 free되어도 안전. */

	SPDK_DEBUGLOG(opal, "MSID = %p\n", opal_key->key);
	return 0;
}

/*
 * [한국어]
 * opal_get_msid_cpin_pin - C_PIN_MSID 객체의 PIN 컬럼 GET 메서드 호출
 *
 * @dev: 디바이스
 * @sess: ANYBODY 세션이 열려있어야 함 (PIN 없이도 MSID는 읽기 가능)
 * @opal_key: 출력 PIN
 * @return: 0=성공
 *
 * Get 메서드 호출 형식:
 *   CALL <C_PIN_MSID UID> <GET_METHOD UID>
 *   STARTLIST
 *     STARTLIST   ← cellblock list
 *       STARTNAME StartColumn=PIN ENDNAME
 *       STARTNAME EndColumn=PIN ENDNAME
 *     ENDLIST
 *   ENDLIST
 *
 * → "C_PIN_MSID 객체의 PIN 컬럼만 단일 셀로 읽어달라"는 요청.
 */
static int
opal_get_msid_cpin_pin(struct spdk_opal_dev *dev, struct opal_session *sess,
		       struct spdk_opal_key *opal_key)
{
	int err = 0;
	int ret;

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	/* [한국어] CALL — 메서드 호출 시작. */
	opal_add_token_bytestring(&err, sess, spdk_opal_uid[UID_C_PIN_MSID],
				  OPAL_UID_LENGTH);
	/* [한국어] invokingId = C_PIN_MSID 객체 UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[GET_METHOD], OPAL_UID_LENGTH);
	/* [한국어] methodId = Get. */

	opal_add_tokens(&err, sess, 12, SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_STARTCOLUMN,
			SPDK_OPAL_PIN,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_ENDCOLUMN,
			SPDK_OPAL_PIN,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDLIST);
	/* [한국어] 인자: cellblock {StartColumn=PIN, EndColumn=PIN} — 단일 셀 PIN 컬럼 요청.
	 * SPDK_OPAL_PIN은 컬럼 ID enum 값 (TCG SSC 5.7.1 Table 181: PIN=0x03). */

	if (err) {
		SPDK_ERRLOG("Error building Get MSID CPIN PIN command.\n");
		return err;
	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	/* [한국어] HSN/TSN을 사용한 헤더 채우기 — ANYBODY 세션이라도 hsn/tsn은 발급되어 있음. */
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_get_msid_cpin_pin_done(sess, opal_key);
	/* [한국어] 응답에서 MSID PIN 추출. */
}

/*
 * [한국어]
 * opal_build_generic_pw_cmd - C_PIN 객체의 PIN 컬럼을 Set 메서드로 갱신하는 명령 빌드
 *
 * @sess: 명령 세션
 * @key: 새 PIN 바이트
 * @key_len: PIN 길이
 * @cpin_uid: 대상 C_PIN 객체 UID (8B) — UID_C_PIN_SID/ADMIN1/USER1 등
 * @dev: 디바이스 (COMID 추출용)
 * @return: 0=빌드 성공
 *
 * Set 메서드 형식:
 *   CALL <C_PIN_xxx UID> <SET_METHOD UID>
 *   STARTLIST
 *     STARTNAME Values=
 *       STARTLIST
 *         STARTNAME PIN=<key bytestring> ENDNAME
 *       ENDLIST
 *     ENDNAME
 *   ENDLIST
 *
 * 송수신과 status 검증은 호출자(opal_set_sid_cpin_pin / opal_new_user_passwd)에서.
 */
static int
opal_build_generic_pw_cmd(struct opal_session *sess, uint8_t *key, size_t key_len,
			  uint8_t *cpin_uid, struct spdk_opal_dev *dev)
{
	int err = 0;

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, cpin_uid, OPAL_UID_LENGTH);
	/* [한국어] invokingId = 갱신할 C_PIN 객체 UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[SET_METHOD],
				  OPAL_UID_LENGTH);
	/* [한국어] methodId = Set. */

	opal_add_tokens(&err, sess, 6,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_VALUES,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_PIN);
	/* [한국어] Values 파라미터 → 값 리스트 → PIN 컬럼 named param 시작. */
	opal_add_token_bytestring(&err, sess, key, key_len);
	/* [한국어] 새 PIN bytestring. */
	opal_add_tokens(&err, sess, 4,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST);
	/* [한국어] PIN named param 종료 → 값 리스트 종료 → Values 종료 → 인자 리스트 종료. */
	if (err) {
		return err;
	}

	return opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	/* [한국어] EOD 추가 + 헤더 길이 확정. */
}

/*
 * [한국어]
 * opal_get_locking_sp_lifecycle_done - LockingSP의 Lifecycle 컬럼 응답 파싱
 *
 * @sess: 응답이 도착한 세션
 * @return: 0=Manufactured-Inactive 상태 확인, -EINVAL=다른 상태
 *
 * Lifecycle 상태 (TCG SSC 5.7.4):
 *   0x08 Manufactured-Inactive (LockingSP 활성화 전 — Activate 가능)
 *   0x09 Manufactured (활성화됨, 정상 동작)
 *   0x10 Issued / Disabled / Frozen 등
 *
 * Activate 호출 전 검사 — 이미 활성화되어 있으면 재활성화 불필요.
 */
static int
opal_get_locking_sp_lifecycle_done(struct opal_session *sess)
{
	uint8_t lifecycle;
	int error = 0;

	error = opal_parse_and_check_status(sess);
	if (error) {
		return error;
	}

	lifecycle = opal_response_get_u64(&sess->parsed_resp, 4);
	/* [한국어] index 4 = Lifecycle 컬럼 값 (uint). */
	if (lifecycle != OPAL_MANUFACTURED_INACTIVE) { /* status before activate */
		/* [한국어] 0x08(Manufactured-Inactive)이 아니면 활성화 진행 불가. */
		SPDK_ERRLOG("Couldn't determine the status of the Lifecycle state\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * opal_get_locking_sp_lifecycle - LockingSP 객체의 Lifecycle 컬럼 GET
 *
 * @dev: 디바이스
 * @sess: SID 인증된 AdminSP 세션
 * @return: 0=Manufactured-Inactive 확인됨
 *
 * 호출자: opal_activate 직전에 호출 — LockingSP가 아직 활성화되지 않았는지 확인.
 */
static int
opal_get_locking_sp_lifecycle(struct spdk_opal_dev *dev, struct opal_session *sess)
{
	int err = 0;
	int ret;

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, spdk_opal_uid[UID_LOCKINGSP],
				  OPAL_UID_LENGTH);
	/* [한국어] invokingId = LockingSP 객체 UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[GET_METHOD], OPAL_UID_LENGTH);
	/* [한국어] methodId = Get. */

	opal_add_tokens(&err, sess, 12, SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_STARTCOLUMN,
			SPDK_OPAL_LIFECYCLE,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_ENDCOLUMN,
			SPDK_OPAL_LIFECYCLE,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDLIST);
	/* [한국어] cellblock {StartColumn=Lifecycle, EndColumn=Lifecycle} — 단일 셀. */

	if (err) {
		SPDK_ERRLOG("Error Building GET Lifecycle Status command\n");
		return err;
	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_get_locking_sp_lifecycle_done(sess);
}

/*
 * [한국어]
 * opal_activate - LockingSP에 Activate 메서드 호출 (LockingSP 활성화)
 *
 * @dev: 디바이스
 * @sess: SID 인증된 AdminSP 세션
 * @return: 0=Activate 성공, NVMe/TCG 에러
 *
 * Activate 메서드는 LockingSP의 Lifecycle을 Manufactured-Inactive → Manufactured로 전환,
 * Locking Range/User Authority 등의 객체를 사용 가능 상태로 만든다. SED 활성화의 핵심.
 *
 * 인자 리스트는 빈 STARTLIST/ENDLIST (Single User Mode 옵션 등은 미사용 — TODO).
 */
static int
opal_activate(struct spdk_opal_dev *dev, struct opal_session *sess)
{
	int err = 0;
	int ret;

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, spdk_opal_uid[UID_LOCKINGSP],
				  OPAL_UID_LENGTH);
	/* [한국어] invokingId = LockingSP UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[ACTIVATE_METHOD],
				  OPAL_UID_LENGTH);
	/* [한국어] methodId = Activate. */

	opal_add_tokens(&err, sess, 2, SPDK_OPAL_STARTLIST, SPDK_OPAL_ENDLIST);
	/* [한국어] 빈 인자 리스트 — 옵션 없음. */

	if (err) {
		SPDK_ERRLOG("Error building Activate LockingSP command.\n");
		return err;
	}

	/* TODO: Single User Mode for activation */
	/* [한국어] SUM 옵션 추가는 미구현 — 기본 multi-user 모드로 활성화. */

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_parse_and_check_status(sess);
	/* [한국어] Activate 성공 status 검증. */
}

/*
 * [한국어]
 * opal_start_auth_session - LockingSP에 User1..N 또는 Admin1 권한으로 인증 R/W 세션 개시
 *
 * @dev: 디바이스
 * @sess: 새 세션
 * @user: 인증 권한 (OPAL_ADMIN1=0 or OPAL_USER1..8)
 * @opal_key: PIN
 * @return: 0=세션 개시 + HSN/TSN 획득
 *
 * opal_start_generic_session과 거의 동일하지만 차이점:
 *   - SP는 항상 LockingSP (잠금 범위 작업용)
 *   - User UID 동적 생성: USER1 UID에서 byte7만 user 번호로 교체 (USER2 UID는 …,0x02)
 *   - Admin1은 미리 정의된 UID_ADMIN1 사용
 *
 * 호출자: spdk_opal_cmd_lock_unlock, spdk_opal_cmd_get_locking_range_info,
 *         spdk_opal_cmd_get_max_ranges 등 LockingSP 작업 진입점들.
 */
static int
opal_start_auth_session(struct spdk_opal_dev *dev,
			struct opal_session *sess,
			enum spdk_opal_user user,
			struct spdk_opal_key *opal_key)
{
	uint8_t uid_user[OPAL_UID_LENGTH];
	int err = 0;
	int ret;
	uint32_t hsn = GENERIC_HOST_SESSION_NUM;
	/* [한국어] HSN = 0x69 (관습값). */

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	if (user != OPAL_ADMIN1) {
		/* [한국어] User1..N: USER1 UID 베이스에서 마지막 바이트만 user 번호로 교체. */
		memcpy(uid_user, spdk_opal_uid[UID_USER1], OPAL_UID_LENGTH);
		uid_user[7] = user;
	} else {
		/* [한국어] Admin1: 미리 정의된 UID 그대로. */
		memcpy(uid_user, spdk_opal_uid[UID_ADMIN1], OPAL_UID_LENGTH);
	}

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, spdk_opal_uid[UID_SMUID],
				  OPAL_UID_LENGTH);
	/* [한국어] invokingId = SMUID (모든 세션 시작은 SMUID). */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[STARTSESSION_METHOD],
				  OPAL_UID_LENGTH);
	/* [한국어] methodId = StartSession. */

	opal_add_token_u8(&err, sess, SPDK_OPAL_STARTLIST);
	opal_add_token_u64(&err, sess, hsn);
	/* [한국어] 첫 인자: HSN. */
	opal_add_token_bytestring(&err, sess, spdk_opal_uid[UID_LOCKINGSP],
				  OPAL_UID_LENGTH);
	/* [한국어] 두번째 인자: SP UID = LockingSP. */
	opal_add_tokens(&err, sess, 3, SPDK_OPAL_TRUE, SPDK_OPAL_STARTNAME,
			0); /* True for a Read-Write session  */
	/* [한국어] R/W 세션 + named param 0 (HostChallenge) 시작. */
	opal_add_token_bytestring(&err, sess, opal_key->key, opal_key->key_len);
	/* [한국어] PIN bytestring. */
	opal_add_tokens(&err, sess, 3, SPDK_OPAL_ENDNAME, SPDK_OPAL_STARTNAME, 3); /* HostSignAuth */
	/* [한국어] HostChallenge 종료 + HostSignAuth (param 3) 시작. */
	opal_add_token_bytestring(&err, sess, uid_user, OPAL_UID_LENGTH);
	/* [한국어] 권한 객체 UID — 위에서 user 번호로 패치된 값. */
	opal_add_tokens(&err, sess, 2, SPDK_OPAL_ENDNAME, SPDK_OPAL_ENDLIST);
	/* [한국어] HostSignAuth 종료 + 인자 리스트 종료. */

	if (err) {
		SPDK_ERRLOG("Error building STARTSESSION command.\n");
		return err;
	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_start_session_done(sess);
}

/*
 * [한국어]
 * opal_lock_unlock_range - LockingRange 객체의 ReadLocked/WriteLocked 컬럼 갱신
 *
 * @dev: 디바이스
 * @sess: User1..N 또는 Admin1 인증된 LockingSP 세션
 * @locking_range: 0=Global, 1~N=Range1..N
 * @l_state: OPAL_READONLY/READWRITE/RWLOCK
 * @return: 0=잠금 변경 성공
 *
 * Lock state 매핑:
 *   READONLY  : ReadLocked=0, WriteLocked=1 (읽기만 가능)
 *   READWRITE : ReadLocked=0, WriteLocked=0 (완전 잠금 해제)
 *   RWLOCK    : ReadLocked=1, WriteLocked=1 (읽기/쓰기 모두 차단)
 *
 * Set 메서드로 Values { ReadLocked=x, WriteLocked=y } 동시 갱신.
 */
static int
opal_lock_unlock_range(struct spdk_opal_dev *dev, struct opal_session *sess,
		       enum spdk_opal_locking_range locking_range,
		       enum spdk_opal_lock_state l_state)
{
	uint8_t uid_locking_range[OPAL_UID_LENGTH];
	uint8_t read_locked, write_locked;
	int err = 0;
	int ret;

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_build_locking_range(uid_locking_range, locking_range);
	/* [한국어] Range 번호 → 8B UID 생성. */

	switch (l_state) {
	case OPAL_READONLY:
		/* [한국어] 읽기는 허용, 쓰기 차단. */
		read_locked = 0;
		write_locked = 1;
		break;
	case OPAL_READWRITE:
		/* [한국어] 읽기/쓰기 모두 허용 (잠금 해제). */
		read_locked = 0;
		write_locked = 0;
		break;
	case OPAL_RWLOCK:
		/* [한국어] 읽기/쓰기 모두 차단. */
		read_locked = 1;
		write_locked = 1;
		break;
	default:
		SPDK_ERRLOG("Tried to set an invalid locking state.\n");
		return -EINVAL;
	}

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, uid_locking_range, OPAL_UID_LENGTH);
	/* [한국어] invokingId = LockingRange UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[SET_METHOD], OPAL_UID_LENGTH);

	opal_add_tokens(&err, sess, 15, SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_VALUES,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_READLOCKED,
			read_locked,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_WRITELOCKED,
			write_locked,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST);
	/* [한국어] Set 인자: Values { ReadLocked=read_locked, WriteLocked=write_locked }
	 * 두 컬럼을 한 번에 업데이트 — atomic 보장 (TCG SSC 5.7.4 Set semantics). */

	if (err) {
		SPDK_ERRLOG("Error building SET command.\n");
		return err;
	}
	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_parse_and_check_status(sess);
}

/*
 * [한국어]
 * opal_generic_locking_range_enable_disable - Global Range의 Lock/Unlock enable bits + 잠금 해제
 *
 * @dev: 디바이스
 * @sess: 인증 세션 (cmd 버퍼는 이미 clear되어야 함 — 호출자 책임)
 * @uid: LockingRange UID (보통 Global)
 * @read_lock_enabled: ReadLockEnabled 컬럼 값 (true=Lock 기능 활성)
 * @write_lock_enabled: WriteLockEnabled 컬럼 값
 * @return: 0=명령 빌드 성공
 *
 * Global Range는 RangeStart/RangeLength 변경 불가 (전체 디바이스 = 전체 LBA).
 * 그래서 ReadLockEnabled/WriteLockEnabled 만 갱신 + ReadLocked/WriteLocked=0으로 강제 해제.
 *
 * 주의: 이 함수는 cmd 빌드만 하고 송수신 안 함. opal_setup_locking_range에서 호출 후 finalize.
 */
static int
opal_generic_locking_range_enable_disable(struct spdk_opal_dev *dev,
		struct opal_session *sess,
		uint8_t *uid, bool read_lock_enabled, bool write_lock_enabled)
{
	int err = 0;

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, uid, OPAL_UID_LENGTH);
	/* [한국어] invokingId = Global LockingRange UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[SET_METHOD], OPAL_UID_LENGTH);

	opal_add_tokens(&err, sess, 23, SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_VALUES,
			SPDK_OPAL_STARTLIST,

			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_READLOCKENABLED,
			read_lock_enabled,
			SPDK_OPAL_ENDNAME,

			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_WRITELOCKENABLED,
			write_lock_enabled,
			SPDK_OPAL_ENDNAME,

			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_READLOCKED,
			0,
			SPDK_OPAL_ENDNAME,

			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_WRITELOCKED,
			0,
			SPDK_OPAL_ENDNAME,

			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST);
	/* [한국어] Set 인자: Values {
	 *   ReadLockEnabled = read_lock_enabled,
	 *   WriteLockEnabled = write_lock_enabled,
	 *   ReadLocked = 0,    ← 강제 해제
	 *   WriteLocked = 0,   ← 강제 해제
	 * }
	 * 4 컬럼을 atomic하게 갱신. */
	if (err) {
		SPDK_ERRLOG("Error building locking range enable/disable command.\n");
	}
	return err;
}

/*
 * [한국어]
 * opal_setup_locking_range - LockingRange 객체의 Range/Enable 컬럼을 한 번에 설정
 *
 * @dev: 디바이스
 * @sess: 인증 세션
 * @locking_range: 0=Global, 1~N=Non-global
 * @range_start: LBA 시작 (non-global만, Global은 무시)
 * @range_length: LBA 길이 (non-global만)
 * @read_lock_enabled: ReadLockEnabled
 * @write_lock_enabled: WriteLockEnabled
 * @return: 0=설정 성공
 *
 * Global vs Non-global 분기:
 *   - Global: opal_generic_locking_range_enable_disable로 Enable bits만 갱신
 *   - Non-global: RangeStart/RangeLength/ReadLockEnabled/WriteLockEnabled 4 컬럼 모두 갱신
 *
 * Set 인자 형식 (non-global):
 *   STARTLIST
 *     STARTNAME Values=
 *       STARTLIST
 *         STARTNAME RangeStart=<LBA u64> ENDNAME
 *         STARTNAME RangeLength=<LBA u64> ENDNAME
 *         STARTNAME ReadLockEnabled=<bool> ENDNAME
 *         STARTNAME WriteLockEnabled=<bool> ENDNAME
 *       ENDLIST
 *     ENDNAME
 *   ENDLIST
 */
static int
opal_setup_locking_range(struct spdk_opal_dev *dev, struct opal_session *sess,
			 enum spdk_opal_locking_range locking_range,
			 uint64_t range_start, uint64_t range_length,
			 bool read_lock_enabled, bool write_lock_enabled)
{
	uint8_t uid_locking_range[OPAL_UID_LENGTH];
	int err = 0;
	int ret;

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_build_locking_range(uid_locking_range, locking_range);
	/* [한국어] Range 번호 → 8B UID 생성. */

	if (locking_range == 0) {
		/* [한국어] Global Range: RangeStart/Length는 SSD가 고정 관리하므로 Enable bits만 갱신. */
		err = opal_generic_locking_range_enable_disable(dev, sess, uid_locking_range,
				read_lock_enabled, write_lock_enabled);
	} else {
		/* [한국어] Non-global Range: 4개 컬럼 atomic Set. */
		opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
		opal_add_token_bytestring(&err, sess, uid_locking_range, OPAL_UID_LENGTH);
		opal_add_token_bytestring(&err, sess, spdk_opal_method[SET_METHOD],
					  OPAL_UID_LENGTH);

		opal_add_tokens(&err, sess, 6,
				SPDK_OPAL_STARTLIST,
				SPDK_OPAL_STARTNAME,
				SPDK_OPAL_VALUES,
				SPDK_OPAL_STARTLIST,
				SPDK_OPAL_STARTNAME,
				SPDK_OPAL_RANGESTART);
		/* [한국어] Values 시작 → 컬럼 list 시작 → RangeStart named param 시작. */
		opal_add_token_u64(&err, sess, range_start);
		/* [한국어] LBA 시작 위치. */
		opal_add_tokens(&err, sess, 3,
				SPDK_OPAL_ENDNAME,
				SPDK_OPAL_STARTNAME,
				SPDK_OPAL_RANGELENGTH);
		/* [한국어] RangeStart 종료 → RangeLength 시작. */
		opal_add_token_u64(&err, sess, range_length);
		/* [한국어] LBA 길이. */
		opal_add_tokens(&err, sess, 3,
				SPDK_OPAL_ENDNAME,
				SPDK_OPAL_STARTNAME,
				SPDK_OPAL_READLOCKENABLED);
		/* [한국어] RangeLength 종료 → ReadLockEnabled 시작. */
		opal_add_token_u64(&err, sess, read_lock_enabled);
		/* [한국어] bool (0/1). */
		opal_add_tokens(&err, sess, 3,
				SPDK_OPAL_ENDNAME,
				SPDK_OPAL_STARTNAME,
				SPDK_OPAL_WRITELOCKENABLED);
		opal_add_token_u64(&err, sess, write_lock_enabled);
		opal_add_tokens(&err, sess, 4,
				SPDK_OPAL_ENDNAME,
				SPDK_OPAL_ENDLIST,
				SPDK_OPAL_ENDNAME,
				SPDK_OPAL_ENDLIST);
		/* [한국어] WriteLockEnabled 종료 → 컬럼 list 종료 → Values 종료 → 인자 list 종료. */
	}
	if (err) {
		SPDK_ERRLOG("Error building Setup Locking range command.\n");
		return err;

	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_parse_and_check_status(sess);
}

/*
 * [한국어]
 * opal_get_max_ranges_done - LockingInfoTable.MaxRanges GET 응답에서 값 추출
 *
 * @sess: 응답 세션
 * @return: MaxRanges (지원 가능한 non-global Range 최대 개수, 음수=에러)
 *
 * SED는 보통 8~16개의 non-global Range를 지원. 디바이스마다 상이.
 */
static int
opal_get_max_ranges_done(struct opal_session *sess)
{
	int error = 0;

	error = opal_parse_and_check_status(sess);
	if (error) {
		return error;
	}

	/* "MaxRanges" is token 4 of response */
	return opal_response_get_u16(&sess->parsed_resp, 4);
	/* [한국어] index 4 = MaxRanges 컬럼 값. */
}

/*
 * [한국어]
 * opal_get_max_ranges - LockingInfoTable 객체의 MaxRanges 컬럼 GET
 *
 * @dev: 디바이스
 * @sess: Admin1 인증된 LockingSP 세션
 * @return: MaxRanges 또는 음수 에러
 *
 * 호출자가 사전에 사용 가능한 Range 수 알고 싶을 때 호출. dev->max_ranges에 캐시.
 */
static int
opal_get_max_ranges(struct spdk_opal_dev *dev, struct opal_session *sess)
{
	int err = 0;
	int ret;

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, spdk_opal_uid[UID_LOCKING_INFO_TABLE],
				  OPAL_UID_LENGTH);
	/* [한국어] invokingId = LockingInfoTable UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[GET_METHOD], OPAL_UID_LENGTH);

	opal_add_tokens(&err, sess, 12, SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_STARTCOLUMN,
			SPDK_OPAL_MAXRANGES,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_ENDCOLUMN,
			SPDK_OPAL_MAXRANGES,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDLIST);
	/* [한국어] cellblock {StartColumn=MaxRanges, EndColumn=MaxRanges} — 단일 셀 GET. */

	if (err) {
		SPDK_ERRLOG("Error Building GET Lifecycle Status command\n");
		return err;
	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_get_max_ranges_done(sess);
}

/*
 * [한국어]
 * opal_get_locking_range_info_done - LockingRange GET 응답에서 6컬럼 추출
 *
 * @sess: 응답 세션
 * @info: 출력 — locking range 메타 (RangeStart/Length/ReadLockEnabled/WriteLockEnabled/Locked)
 * @return: 0=성공
 *
 * 응답 인덱스 매핑:
 *   index 4  : RangeStart (u64 LBA)
 *   index 8  : RangeLength (u64 LBA)
 *   index 12 : ReadLockEnabled (u8 bool)
 *   index 16 : WriteLockEnabled (u8 bool)
 *   index 20 : ReadLocked (u8 bool, 현재 잠금 상태)
 *   index 24 : WriteLocked (u8 bool)
 *
 * 인덱스 간격이 4인 이유: 각 컬럼이 STARTNAME <colId> <value> ENDNAME 4토큰으로 인코딩되기 때문.
 */
static int
opal_get_locking_range_info_done(struct opal_session *sess,
				 struct spdk_opal_locking_range_info *info)
{
	int error = 0;

	error = opal_parse_and_check_status(sess);
	if (error) {
		return error;
	}

	info->range_start = opal_response_get_u64(&sess->parsed_resp, 4);
	/* [한국어] LBA 시작. */
	info->range_length = opal_response_get_u64(&sess->parsed_resp, 8);
	/* [한국어] LBA 길이. */
	info->read_lock_enabled = opal_response_get_u8(&sess->parsed_resp, 12);
	/* [한국어] Read Lock 기능 활성 여부 (1=활성). */
	info->write_lock_enabled = opal_response_get_u8(&sess->parsed_resp, 16);
	/* [한국어] Write Lock 기능 활성 여부. */
	info->read_locked = opal_response_get_u8(&sess->parsed_resp, 20);
	/* [한국어] 현재 read 잠겨있는지. */
	info->write_locked = opal_response_get_u8(&sess->parsed_resp, 24);
	/* [한국어] 현재 write 잠겨있는지. */

	return 0;
}

/*
 * [한국어]
 * opal_get_locking_range_info - LockingRange 객체의 RangeStart..WriteLocked 6컬럼 모두 GET
 *
 * @dev: 디바이스
 * @sess: 인증 세션
 * @locking_range_id: Range 번호 (0=Global)
 * @return: 0=정보를 dev->locking_ranges[id]에 캐시
 *
 * Cellblock {StartColumn=RangeStart, EndColumn=WriteLocked} — 6 컬럼 범위 GET 요청.
 * SSD 응답에는 6 컬럼이 모두 포함되어 한 번의 RPC로 전체 메타 회수.
 */
static int
opal_get_locking_range_info(struct spdk_opal_dev *dev,
			    struct opal_session *sess,
			    enum spdk_opal_locking_range locking_range_id)
{
	int err = 0;
	int ret;
	uint8_t uid_locking_range[OPAL_UID_LENGTH];
	struct spdk_opal_locking_range_info *info;

	opal_build_locking_range(uid_locking_range, locking_range_id);

	assert(locking_range_id < SPDK_OPAL_MAX_LOCKING_RANGE);
	/* [한국어] dev->locking_ranges[] 배열 한도 검사. */
	info = &dev->locking_ranges[locking_range_id];
	/* [한국어] 결과를 디바이스 캐시에 저장. */
	memset(info, 0, sizeof(*info));
	/* [한국어] 잔재 클리어. */
	info->locking_range_id = locking_range_id;
	/* [한국어] ID 자체 보관. */

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, uid_locking_range, OPAL_UID_LENGTH);
	opal_add_token_bytestring(&err, sess, spdk_opal_method[GET_METHOD], OPAL_UID_LENGTH);


	opal_add_tokens(&err, sess, 12, SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_STARTCOLUMN,
			SPDK_OPAL_RANGESTART,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_ENDCOLUMN,
			SPDK_OPAL_WRITELOCKED,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDLIST);
	/* [한국어] cellblock {StartColumn=RangeStart, EndColumn=WriteLocked}.
	 * RangeStart..WriteLocked 사이의 모든 컬럼이 응답에 포함됨. */

	if (err) {
		SPDK_ERRLOG("Error Building get locking range info command\n");
		return err;
	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_get_locking_range_info_done(sess, info);
}

/*
 * [한국어]
 * opal_enable_user - User1..N Authority의 Enabled 컬럼을 true로 갱신 (사용자 활성화)
 *
 * @dev: 디바이스
 * @sess: Admin1 인증된 LockingSP 세션
 * @user: 활성화할 사용자 (OPAL_USER1..8)
 * @return: 0=활성화 성공
 *
 * Authority의 Enabled 컬럼이 false면 해당 권한으로 세션 시작 불가.
 * TakeOwnership 후 새 사용자를 만들기 전에 반드시 호출.
 *
 * Set { Enabled=TRUE } 단일 컬럼 갱신.
 */
static int
opal_enable_user(struct spdk_opal_dev *dev, struct opal_session *sess,
		 enum spdk_opal_user user)
{
	int err = 0;
	int ret;
	uint8_t uid_user[OPAL_UID_LENGTH];

	memcpy(uid_user, spdk_opal_uid[UID_USER1], OPAL_UID_LENGTH);
	uid_user[7] = user;
	/* [한국어] USER1 UID 베이스 + byte7에 user 번호 패치. */

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, uid_user, OPAL_UID_LENGTH);
	/* [한국어] invokingId = User Authority UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[SET_METHOD], OPAL_UID_LENGTH);

	opal_add_tokens(&err, sess, 11,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_VALUES,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_AUTH_ENABLE,
			SPDK_OPAL_TRUE,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST);
	/* [한국어] Set { Values { Enabled=TRUE } } — 사용자 활성화 1바이트 boolean. */

	if (err) {
		SPDK_ERRLOG("Error Building enable user command\n");
		return err;
	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_parse_and_check_status(sess);
}

/*
 * [한국어]
 * opal_add_user_to_locking_range - User에게 LockingRange의 Read/Write Lock 권한 부여
 *
 * @dev: 디바이스
 * @sess: Admin1 인증 세션
 * @user: 권한 부여 대상 사용자
 * @locking_range: 대상 Range
 * @l_state: OPAL_READONLY (Read ACE 갱신) 또는 OPAL_READWRITE (Write ACE 갱신)
 * @return: 0=권한 부여 성공
 *
 * ACE(Access Control Element) 객체의 BooleanExpr 컬럼을 갱신하여 어떤 Authority가
 * 어떤 잠금 컬럼에 SET을 호출할 수 있는지를 정의.
 *
 * BooleanExpr 형식: { (User OR User) AND TRUE } 같은 OPA(Object Property Access) 표현.
 * 여기서는 단순히 [HalfUid_AuthorityObjRef + UserUID, HalfUid_AuthorityObjRef + UserUID,
 * HalfUid_BooleanACE + TRUE] 3 토큰으로 user에 대한 OR 조건 + boolean true.
 *
 * UID_LOCKINGRANGE_ACE_RDLOCKED: ReadLocked 컬럼에 SET을 허용하는 ACE
 * UID_LOCKINGRANGE_ACE_WRLOCKED: WriteLocked 컬럼에 SET을 허용하는 ACE
 * uid_locking_range[7]에 range 번호 패치.
 *
 * UID_HALF_AUTHORITY_OBJ_REF: half UID — 4바이트만 사용해 UID 참조 표현 시작.
 * UID_HALF_BOOLEAN_ACE: half UID — Boolean 표현 종료 마커.
 */
static int
opal_add_user_to_locking_range(struct spdk_opal_dev *dev,
			       struct opal_session *sess,
			       enum spdk_opal_user user,
			       enum spdk_opal_locking_range locking_range,
			       enum spdk_opal_lock_state l_state)
{
	int err = 0;
	int ret;
	uint8_t uid_user[OPAL_UID_LENGTH];
	uint8_t uid_locking_range[OPAL_UID_LENGTH];

	memcpy(uid_user, spdk_opal_uid[UID_USER1], OPAL_UID_LENGTH);
	uid_user[7] = user;
	/* [한국어] User UID 패치. */

	switch (l_state) {
	case OPAL_READONLY:
		/* [한국어] Read Lock ACE 선택 — 이 사용자가 ReadLocked 컬럼을 SET 가능. */
		memcpy(uid_locking_range, spdk_opal_uid[UID_LOCKINGRANGE_ACE_RDLOCKED], OPAL_UID_LENGTH);
		break;
	case OPAL_READWRITE:
		/* [한국어] Write Lock ACE 선택. */
		memcpy(uid_locking_range, spdk_opal_uid[UID_LOCKINGRANGE_ACE_WRLOCKED], OPAL_UID_LENGTH);
		break;
	default:
		SPDK_ERRLOG("locking state should only be OPAL_READONLY or OPAL_READWRITE\n");
		return -EINVAL;
	}

	uid_locking_range[7] = locking_range;
	/* [한국어] Range 번호 패치 (byte7). */

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, uid_locking_range, OPAL_UID_LENGTH);
	/* [한국어] invokingId = ACE UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[SET_METHOD], OPAL_UID_LENGTH);

	opal_add_tokens(&err, sess, 8,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_VALUES,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_BOOLEAN_EXPR,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME);
	/* [한국어] Set { Values { BooleanExpr=[ ... ] } } 시작. */
	opal_add_token_bytestring(&err, sess, spdk_opal_uid[UID_HALF_AUTHORITY_OBJ_REF],
				  OPAL_UID_LENGTH / 2);
	/* [한국어] half UID = AuthorityObjRef (4B) — "다음에 오는 UID는 권한 객체 참조"라는 명세. */
	opal_add_token_bytestring(&err, sess, uid_user, OPAL_UID_LENGTH);
	/* [한국어] User Authority UID. */

	opal_add_tokens(&err, sess, 2, SPDK_OPAL_ENDNAME, SPDK_OPAL_STARTNAME);
	/* [한국어] 첫 항목 종료 → 두 번째 항목 시작 (OR 조건). */
	opal_add_token_bytestring(&err, sess, spdk_opal_uid[UID_HALF_AUTHORITY_OBJ_REF],
				  OPAL_UID_LENGTH / 2);
	opal_add_token_bytestring(&err, sess, uid_user, OPAL_UID_LENGTH);
	/* [한국어] 같은 UID 한 번 더 — TCG 스펙상 ACE BooleanExpr는 두 번 참조하는 관습. */

	opal_add_tokens(&err, sess, 2, SPDK_OPAL_ENDNAME, SPDK_OPAL_STARTNAME);
	opal_add_token_bytestring(&err, sess, spdk_opal_uid[UID_HALF_BOOLEAN_ACE], OPAL_UID_LENGTH / 2);
	/* [한국어] half UID = Boolean ACE — 다음 boolean 값으로 표현 종료. */
	opal_add_tokens(&err, sess, 7,
			SPDK_OPAL_TRUE,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST);
	/* [한국어] Boolean=TRUE → 항목 종료 → list 종료 → BooleanExpr 종료 → Values list 종료 →
	 * Values 종료 → 인자 list 종료. */
	if (err) {
		SPDK_ERRLOG("Error building add user to locking range command\n");
		return err;
	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_parse_and_check_status(sess);
}

/*
 * [한국어]
 * opal_new_user_passwd - User1..N 또는 Admin1의 C_PIN을 새 PIN으로 갱신
 *
 * @dev: 디바이스
 * @sess: Admin1 또는 해당 User로 인증된 세션
 * @user: 대상 사용자
 * @opal_key: 새 PIN
 * @return: 0=성공
 *
 * 대상 C_PIN UID 결정:
 *   Admin1 : C_PIN_ADMIN1 그대로
 *   User N : C_PIN_USER1 베이스 + byte7=N
 */
static int
opal_new_user_passwd(struct spdk_opal_dev *dev, struct opal_session *sess,
		     enum spdk_opal_user user,
		     struct spdk_opal_key *opal_key)
{
	uint8_t uid_cpin[OPAL_UID_LENGTH];
	int ret;

	if (user == OPAL_ADMIN1) {
		/* [한국어] Admin1 C_PIN. */
		memcpy(uid_cpin, spdk_opal_uid[UID_C_PIN_ADMIN1], OPAL_UID_LENGTH);
	} else {
		/* [한국어] User N C_PIN — UID byte7에 user 번호 패치. */
		memcpy(uid_cpin, spdk_opal_uid[UID_C_PIN_USER1], OPAL_UID_LENGTH);
		uid_cpin[7] = user;
	}

	ret = opal_build_generic_pw_cmd(sess, opal_key->key, opal_key->key_len, uid_cpin, dev);
	/* [한국어] PIN 갱신 명령 빌드 (Set { Values { PIN=key } }). */
	if (ret != 0) {
		SPDK_ERRLOG("Error building set password command\n");
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_parse_and_check_status(sess);
}

/*
 * [한국어]
 * opal_set_sid_cpin_pin - C_PIN_SID의 PIN 컬럼을 새 PIN으로 갱신 (TakeOwnership 핵심)
 *
 * @dev: 디바이스
 * @sess: SID 인증된 AdminSP 세션
 * @new_passwd: 새 SID PIN (사용자 소유권 비밀번호)
 * @return: 0=성공
 *
 * TakeOwnership 워크플로의 핵심 단계 — 공장 디폴트 MSID로 SID 세션을 열고,
 * 그 안에서 SID PIN 자체를 사용자가 정한 값으로 교체. 이후엔 새 PIN을 알아야만
 * AdminSP에 SID 권한으로 로그인 가능.
 */
static int
opal_set_sid_cpin_pin(struct spdk_opal_dev *dev, struct opal_session *sess, char *new_passwd)
{
	uint8_t cpin_uid[OPAL_UID_LENGTH];
	struct spdk_opal_key opal_key = {};
	int ret;

	ret = opal_init_key(&opal_key, new_passwd);
	/* [한국어] 새 PIN을 spdk_opal_key 구조체로 변환. */
	if (ret != 0) {
		return ret;
	}

	memcpy(cpin_uid, spdk_opal_uid[UID_C_PIN_SID], OPAL_UID_LENGTH);
	/* [한국어] 대상 = C_PIN_SID 객체. */

	if (opal_build_generic_pw_cmd(sess, opal_key.key, opal_key.key_len, cpin_uid, dev)) {
		SPDK_ERRLOG("Error building Set SID cpin\n");
		return -ERANGE;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_parse_and_check_status(sess);
}

/*
 * [한국어]
 * spdk_opal_cmd_take_ownership - 공장 출하 SED를 사용자 소유로 전환 (3단계 핸드셰이크)
 *
 * @dev: Opal 디바이스
 * @new_passwd: 새 SID PIN (소유자 비밀번호)
 * @return: 0=소유권 획득 성공
 *
 * **표준 TakeOwnership 시퀀스**:
 *
 *   Phase 1: ANYBODY 세션으로 MSID(공장 디폴트 PIN) 회수
 *     1) opal_start_generic_session(ANYBODY, AdminSP)
 *        → 인증 없는 익명 세션 (HSN/TSN 발급되지만 권한 ANYBODY)
 *     2) opal_get_msid_cpin_pin → C_PIN_MSID 객체의 PIN 컬럼 GET
 *     3) opal_end_session
 *
 *   Phase 2: SID 세션으로 SID C_PIN을 새 PIN으로 변경
 *     4) opal_start_generic_session(SID, AdminSP, MSID-PIN)
 *        → SID 권한 인증 세션 (공장 디폴트 PIN으로 로그인)
 *     5) opal_set_sid_cpin_pin(new_passwd) → C_PIN_SID 객체의 PIN 컬럼 SET
 *     6) opal_end_session
 *
 *   이후 디바이스는 new_passwd를 알아야만 SID 권한으로 로그인 가능.
 *   공장 디폴트 MSID는 그대로 남지만 SID PIN이 바뀐 상태이므로 무력화됨.
 *
 * 메모리 보안: opal_key를 phase 1 끝나고 phase 2 시작 후 즉시 0으로 클리어.
 */
int
spdk_opal_cmd_take_ownership(struct spdk_opal_dev *dev, char *new_passwd)
{
	int ret;
	struct spdk_opal_key opal_key = {};
	struct opal_session *sess;

	assert(dev != NULL);

	sess = opal_alloc_session(dev);
	if (!sess) {
		return -ENOMEM;
	}

	ret = opal_start_generic_session(dev, sess, UID_ANYBODY, UID_ADMINSP, NULL, 0);
	/* [한국어] Phase 1 단계 1: ANYBODY 세션 시작 (PIN 없음). */
	if (ret) {
		SPDK_ERRLOG("start admin SP session error %d\n", ret);
		goto end;
	}

	ret = opal_get_msid_cpin_pin(dev, sess, &opal_key);
	/* [한국어] Phase 1 단계 2: MSID PIN 회수 → opal_key에 저장. */
	if (ret) {
		SPDK_ERRLOG("get msid error %d\n", ret);
		opal_end_session(dev, sess, dev->comid);
		goto end;
	}

	ret = opal_end_session(dev, sess, dev->comid);
	/* [한국어] Phase 1 단계 3: ANYBODY 세션 종료. */
	if (ret) {
		SPDK_ERRLOG("end session error %d\n", ret);
		goto end;
	}

	/* reuse the session structure */
	memset(sess, 0, sizeof(*sess));
	/* [한국어] sess 구조체 재활용 — calloc 비용 절약. hsn/tsn=0으로 리셋. */
	sess->dev = dev;
	ret = opal_start_generic_session(dev, sess, UID_SID, UID_ADMINSP,
					 opal_key.key, opal_key.key_len);
	/* [한국어] Phase 2 단계 4: SID 세션 시작 (MSID PIN으로 로그인). */
	if (ret) {
		SPDK_ERRLOG("start admin SP session error %d\n", ret);
		goto end;
	}
	memset(&opal_key, 0, sizeof(struct spdk_opal_key));
	/* [한국어] MSID PIN 메모리에서 즉시 제거 — 보안 모범 사례. */

	ret = opal_set_sid_cpin_pin(dev, sess, new_passwd);
	/* [한국어] Phase 2 단계 5: SID PIN을 새 값으로 갱신. */
	if (ret) {
		SPDK_ERRLOG("set cpin error %d\n", ret);
		opal_end_session(dev, sess, dev->comid);
		goto end;
	}

	ret = opal_end_session(dev, sess, dev->comid);
	/* [한국어] Phase 2 단계 6: SID 세션 종료. */
	if (ret) {
		SPDK_ERRLOG("end session error %d\n", ret);
	}

end:
	free(sess);
	return ret;
}

/*
 * [한국어]
 * spdk_opal_dev_construct - Opal 디바이스 컨텍스트 생성 + Level 0 Discovery 수행 (공개 API)
 *
 * @ctrlr: 이미 attach된 NVMe 컨트롤러
 * @return: spdk_opal_dev 포인터 또는 NULL (Opal 미지원, OOM, NVMe 에러)
 *
 * SED 사용을 위한 첫 진입점 — 디바이스가 Opal 지원하는지 확인하고 BaseComID 획득.
 * 호출 흐름:
 *   1. spdk_opal_dev calloc + ctrlr 바인딩
 *   2. payload 임시 버퍼 alloc (IO_BUFFER_LENGTH=2048)
 *   3. opal_discovery0 → SECP_INFO + Level 0 Discovery → dev->comid 설정
 *   4. payload free, dev 반환
 *
 * 호출자: bdev_nvme의 opal 통합 코드, RPC 핸들러, 사용자 코드.
 */
struct spdk_opal_dev *
	spdk_opal_dev_construct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_opal_dev *dev;
	void *payload;

	dev = calloc(1, sizeof(*dev));
	/* [한국어] 디바이스 컨텍스트 zero alloc — feat_info/locking_ranges 캐시 모두 0으로 시작. */
	if (!dev) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}

	dev->ctrlr = ctrlr;
	/* [한국어] NVMe ctrlr 핸들 보관 — 모든 security_send/recv에 사용. */

	payload = calloc(1, IO_BUFFER_LENGTH);
	/* [한국어] Discovery 응답 임시 버퍼 — 함수 종료 후 free. */
	if (!payload) {
		free(dev);
		return NULL;
	}

	if (opal_discovery0(dev, payload, IO_BUFFER_LENGTH)) {
		/* [한국어] Discovery 실패 = SED 미지원 또는 NVMe 에러. */
		SPDK_INFOLOG(opal, "Opal is not supported on this device\n");
		free(dev);
		free(payload);
		return NULL;
	}

	free(payload);
	return dev;
}

/*
 * [한국어]
 * opal_build_revert_tper_cmd - Revert 메서드 호출 명령 빌드 (공장 초기화)
 *
 * @dev: 디바이스
 * @sess: SID 인증 AdminSP 세션
 * @return: 0=빌드 성공
 *
 * Revert 메서드 (TCG SSC v2.0 5.7.6): Trusted Peripheral 전체를 공장 디폴트 상태로 되돌림.
 *   - 모든 Locking Range 비활성화
 *   - 모든 사용자 PIN 초기화 (Admin1/User1.. 모두 삭제)
 *   - SID PIN을 MSID로 복원
 *   - 데이터는 **암호학적으로 erase** (AES 키 폐기 → 즉시 복호화 불가)
 *
 * Crypto-erase: 디바이스 내부 DEK(Data Encryption Key)를 GENKEY로 새로 만들면
 * 기존 암호문은 의미 없는 random bytes로 변환됨 — secure erase의 즉각적 형태.
 */
static int
opal_build_revert_tper_cmd(struct spdk_opal_dev *dev, struct opal_session *sess)
{
	int err = 0;

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, spdk_opal_uid[UID_ADMINSP],
				  OPAL_UID_LENGTH);
	/* [한국어] invokingId = AdminSP UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[REVERT_METHOD],
				  OPAL_UID_LENGTH);
	/* [한국어] methodId = Revert. */
	opal_add_token_u8(&err, sess, SPDK_OPAL_STARTLIST);
	opal_add_token_u8(&err, sess, SPDK_OPAL_ENDLIST);
	/* [한국어] 빈 인자 리스트 — Revert는 옵션 없음. */
	if (err) {
		SPDK_ERRLOG("Error building REVERT TPER command.\n");
		return -ERANGE;
	}

	return opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
}

/*
 * [한국어]
 * opal_gen_new_active_key - Locking Range의 ActiveKey UID에 대해 GenKey 메서드 호출 (DEK 회전)
 *
 * @dev: 디바이스
 * @sess: 인증 세션
 * @active_key: 이전에 GET으로 얻은 ActiveKey UID (보통 K_AES_256 객체 UID)
 * @return: 0=새 키 생성 성공
 *
 * GenKey 메서드 (TCG SSC v2.0 5.7.10): 대상 K_AES_xxx 객체 안의 DEK(Data Encryption Key)를
 * 디바이스 내부 RNG로 새로 생성. 기존 데이터는 즉시 복호화 불가 (cryptographic erase).
 *
 * Locking Range의 K_AES UID는 "ActiveKey" 컬럼에서 GET으로 얻을 수 있으며,
 * uid_data[OPAL_UID_LENGTH]에 active_key->key의 첫 8바이트 복사 후 invokingId로 사용.
 */
static int
opal_gen_new_active_key(struct spdk_opal_dev *dev, struct opal_session *sess,
			struct spdk_opal_key *active_key)
{
	uint8_t uid_data[OPAL_UID_LENGTH] = {0};
	int err = 0;
	int length;
	int ret;

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	if (active_key->key_len == 0) {
		/* [한국어] 이전 GET 결과가 비어있음 — 호출자 순서 오류. */
		SPDK_ERRLOG("Error finding previous data to generate new active key\n");
		return -EINVAL;
	}

	length = spdk_min(active_key->key_len, OPAL_UID_LENGTH);
	memcpy(uid_data, active_key->key, length);
	/* [한국어] ActiveKey 응답에서 받은 UID(8B)를 uid_data로 복사. */

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, uid_data, OPAL_UID_LENGTH);
	/* [한국어] invokingId = K_AES UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[GENKEY_METHOD],
				  OPAL_UID_LENGTH);
	/* [한국어] methodId = GenKey. */

	opal_add_tokens(&err, sess, 2, SPDK_OPAL_STARTLIST, SPDK_OPAL_ENDLIST);
	/* [한국어] 빈 인자 — GenKey는 옵션 없이 즉시 새 DEK 생성. */

	if (err) {
		SPDK_ERRLOG("Error building new key generation command.\n");
		return err;
	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_parse_and_check_status(sess);
}

/*
 * [한국어]
 * opal_get_active_key_done - LockingRange.ActiveKey 응답에서 K_AES UID 추출
 *
 * @sess: 응답 세션
 * @active_key: 출력 — UID 8B를 key/key_len에 저장
 * @return: 0=성공
 *
 * ActiveKey 컬럼 값은 K_AES_128 또는 K_AES_256 객체의 8B UID (bytestring 인코딩).
 */
static int
opal_get_active_key_done(struct opal_session *sess, struct spdk_opal_key *active_key)
{
	const char *key;
	size_t str_len;
	int error = 0;

	error = opal_parse_and_check_status(sess);
	if (error) {
		return error;
	}

	str_len = opal_response_get_string(&sess->parsed_resp, 4, &key);
	/* [한국어] index 4 = ActiveKey 컬럼 (bytestring) → K_AES UID. */
	if (!key) {
		SPDK_ERRLOG("Couldn't extract active key from response\n");
		return -EINVAL;
	}

	active_key->key_len = str_len;
	memcpy(active_key->key, key, active_key->key_len);
	/* [한국어] UID를 호출자 버퍼로 복사. */

	SPDK_DEBUGLOG(opal, "active key = %p\n", active_key->key);
	return 0;
}

/*
 * [한국어]
 * opal_get_active_key - LockingRange의 ActiveKey 컬럼 GET (K_AES UID 회수)
 *
 * @dev: 디바이스
 * @sess: 인증 세션
 * @locking_range: Range 번호
 * @active_key: 출력 — K_AES UID
 * @return: 0=성공
 *
 * Secure erase 워크플로의 1단계 — DEK 객체 식별자 획득. 다음 단계로 GenKey 호출.
 */
static int
opal_get_active_key(struct spdk_opal_dev *dev, struct opal_session *sess,
		    enum spdk_opal_locking_range locking_range,
		    struct spdk_opal_key *active_key)
{
	uint8_t uid_locking_range[OPAL_UID_LENGTH];
	int err = 0;
	int ret;

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_build_locking_range(uid_locking_range, locking_range);

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, uid_locking_range, OPAL_UID_LENGTH);
	/* [한국어] invokingId = LockingRange UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[GET_METHOD],
				  OPAL_UID_LENGTH);
	opal_add_tokens(&err, sess, 12,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTLIST,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_STARTCOLUMN,
			SPDK_OPAL_ACTIVEKEY,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_STARTNAME,
			SPDK_OPAL_ENDCOLUMN,
			SPDK_OPAL_ACTIVEKEY,
			SPDK_OPAL_ENDNAME,
			SPDK_OPAL_ENDLIST,
			SPDK_OPAL_ENDLIST);
	/* [한국어] cellblock {StartColumn=ActiveKey, EndColumn=ActiveKey} — 단일 컬럼 GET. */

	if (err) {
		SPDK_ERRLOG("Error building get active key command.\n");
		return err;
	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_get_active_key_done(sess, active_key);
}

/*
 * [한국어]
 * opal_erase_locking_range - LockingRange 객체에 Erase 메서드 호출 (영역 제로화)
 *
 * @dev: 디바이스
 * @sess: 인증 세션
 * @locking_range: 대상 Range
 * @return: 0=Erase 성공
 *
 * Erase 메서드 (TCG SSC v2.0 5.7.7): Locking Range의 RangeStart/RangeLength 컬럼을
 * 디폴트 값으로 되돌리고, ReadLocked/WriteLocked 모두 0으로 해제. 데이터 자체는 직접 지우진 않음
 * (즉시 erase는 GenKey 사용 — opal_secure_erase_locking_range 참조).
 */
static int
opal_erase_locking_range(struct spdk_opal_dev *dev, struct opal_session *sess,
			 enum spdk_opal_locking_range locking_range)
{
	uint8_t uid_locking_range[OPAL_UID_LENGTH];
	int err = 0;
	int ret;

	opal_clear_cmd(sess);
	opal_set_comid(sess, dev->comid);

	opal_build_locking_range(uid_locking_range, locking_range);

	opal_add_token_u8(&err, sess, SPDK_OPAL_CALL);
	opal_add_token_bytestring(&err, sess, uid_locking_range, OPAL_UID_LENGTH);
	/* [한국어] invokingId = LockingRange UID. */
	opal_add_token_bytestring(&err, sess, spdk_opal_method[ERASE_METHOD],
				  OPAL_UID_LENGTH);
	/* [한국어] methodId = Erase. */
	opal_add_tokens(&err, sess, 2, SPDK_OPAL_STARTLIST, SPDK_OPAL_ENDLIST);
	/* [한국어] 빈 인자. */

	if (err) {
		SPDK_ERRLOG("Error building erase locking range.\n");
		return err;
	}

	ret = opal_cmd_finalize(sess, sess->hsn, sess->tsn, true);
	if (ret) {
		return ret;
	}

	ret = opal_send_recv(dev, sess);
	if (ret) {
		return ret;
	}

	return opal_parse_and_check_status(sess);
}

/*
 * [한국어]
 * spdk_opal_cmd_revert_tper - 디바이스 공장 초기화 (모든 키/PIN/Range 폐기) (공개 API)
 *
 * @dev: 디바이스
 * @passwd: 현재 SID PIN
 * @return: 0=Revert 성공
 *
 * 워크플로:
 *   1. SID 세션 시작 (현재 SID PIN 사용)
 *   2. Revert 메서드 호출 → SSD가 모든 보안 상태 초기화 + DEK 폐기
 *   3. Revert 성공 시 SSD가 자체적으로 세션을 무효화하므로 별도 EndSession 불필요
 *      (실패 시에만 명시적 end_session)
 *
 * **데이터 손실 경고**: 이 호출 후 디바이스의 모든 데이터는 cryptographic erase되어
 * 복호화 불가. SED의 secure-disposal 시나리오에서 사용.
 */
int
spdk_opal_cmd_revert_tper(struct spdk_opal_dev *dev, const char *passwd)
{
	int ret;
	struct opal_session *sess;
	struct spdk_opal_key opal_key = {};

	assert(dev != NULL);

	ret = opal_init_key(&opal_key, passwd);
	/* [한국어] PIN을 spdk_opal_key로 변환. */
	if (ret) {
		SPDK_ERRLOG("Init key failed\n");
		return ret;
	}

	sess = opal_alloc_session(dev);
	if (!sess) {
		return -ENOMEM;
	}

	ret = opal_start_generic_session(dev, sess, UID_SID, UID_ADMINSP,
					 opal_key.key, opal_key.key_len);
	/* [한국어] SID 권한으로 AdminSP 세션 시작. */
	if (ret) {
		SPDK_ERRLOG("Error on starting admin SP session with error %d\n", ret);
		free(sess);
		return ret;
	}

	ret = opal_build_revert_tper_cmd(dev, sess);
	/* [한국어] Revert 명령 빌드. */
	if (ret) {
		opal_end_session(dev, sess, dev->comid);
		SPDK_ERRLOG("Build revert tper command with error %d\n", ret);
		goto end;
	}

	ret = opal_send_recv(dev, sess);
	/* [한국어] Revert 실행. */
	if (ret) {
		opal_end_session(dev, sess, dev->comid);
		SPDK_ERRLOG("Error on reverting TPer with error %d\n", ret);
		goto end;
	}

	ret = opal_parse_and_check_status(sess);
	if (ret) {
		opal_end_session(dev, sess, dev->comid);
		SPDK_ERRLOG("Error on reverting TPer with error %d\n", ret);
	}
	/* No opal_end_session() required here for successful case */
	/* [한국어] 성공 시 SSD가 자동으로 세션 destroy — 호스트에서 EndSession 보낼 필요 없음.
	 * (Revert가 모든 보안 상태를 초기화하므로 세션 컨텍스트도 같이 무효화됨) */

end:
	free(sess);
	return ret;
}

/*
 * [한국어]
 * spdk_opal_cmd_activate_locking_sp - LockingSP 활성화 (TakeOwnership 후 다음 단계) (공개 API)
 *
 * @dev: 디바이스
 * @passwd: SID PIN (TakeOwnership 후 변경된 새 PIN)
 * @return: 0=활성화 성공
 *
 * 워크플로:
 *   1. SID 세션으로 AdminSP 로그인
 *   2. LockingSP의 Lifecycle 컬럼 GET → Manufactured-Inactive 확인
 *   3. Activate 메서드 호출 → Lifecycle을 Manufactured로 전환 (Locking Range/User 객체 사용 가능)
 *   4. EndSession
 *
 * 활성화 후엔 Admin1 권한 (LockingSP)으로 로그인하여 Range 설정 + User 추가 가능.
 */
int
spdk_opal_cmd_activate_locking_sp(struct spdk_opal_dev *dev, const char *passwd)
{
	struct opal_session *sess;
	struct spdk_opal_key opal_key = {};
	int ret;

	ret = opal_init_key(&opal_key, passwd);
	if (ret != 0) {
		return ret;
	}

	sess = opal_alloc_session(dev);
	if (!sess) {
		return -ENOMEM;
	}

	ret = opal_start_generic_session(dev, sess, UID_SID, UID_ADMINSP,
					 opal_key.key, opal_key.key_len);
	/* [한국어] SID 권한으로 AdminSP 로그인. */
	if (ret) {
		SPDK_ERRLOG("Error on starting admin SP session with error %d\n", ret);
		free(sess);
		return ret;
	}

	ret = opal_get_locking_sp_lifecycle(dev, sess);
	/* [한국어] Lifecycle = Manufactured-Inactive 확인. */
	if (ret) {
		SPDK_ERRLOG("Error on getting SP lifecycle with error %d\n", ret);
		goto end;
	}

	ret = opal_activate(dev, sess);
	/* [한국어] Activate 실행. */
	if (ret) {
		SPDK_ERRLOG("Error on activation with error %d\n", ret);
	}

end:
	ret += opal_end_session(dev, sess, dev->comid);
	/* [한국어] cleanup: 작업 결과와 EndSession 결과 합산 — 어느 쪽이든 0이 아니면 에러. */
	if (ret) {
		SPDK_ERRLOG("Error on ending session with error %d\n", ret);
	}

	free(sess);
	return ret;
}

/*
 * [한국어]
 * spdk_opal_cmd_lock_unlock - LockingRange 잠금/해제 (공개 API)
 *
 * @dev: 디바이스
 * @user: 인증 권한 (Admin1 또는 권한 부여된 User)
 * @flag: OPAL_READONLY/READWRITE/RWLOCK
 * @locking_range: 대상 Range
 * @passwd: User/Admin1 PIN
 * @return: 0=잠금 변경 성공
 *
 * 워크플로:
 *   1. User/Admin1로 LockingSP 인증 세션
 *   2. opal_lock_unlock_range → Set { ReadLocked=x, WriteLocked=y }
 *   3. EndSession
 *
 * 가장 자주 호출되는 API — 부팅 시 unlock, suspend 전 lock 등 일상 운영 사이클.
 */
int
spdk_opal_cmd_lock_unlock(struct spdk_opal_dev *dev, enum spdk_opal_user user,
			  enum spdk_opal_lock_state flag, enum spdk_opal_locking_range locking_range,
			  const char *passwd)
{
	struct opal_session *sess;
	struct spdk_opal_key opal_key = {};
	int ret;

	assert(dev != NULL);

	ret = opal_init_key(&opal_key, passwd);
	if (ret != 0) {
		return ret;
	}

	sess = opal_alloc_session(dev);
	if (!sess) {
		return -ENOMEM;
	}

	ret = opal_start_auth_session(dev, sess, user, &opal_key);
	/* [한국어] user(Admin1 또는 USER1..N)로 LockingSP 인증 R/W 세션 시작. */
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d\n", ret);
		free(sess);
		return ret;
	}

	ret = opal_lock_unlock_range(dev, sess, locking_range, flag);
	/* [한국어] ReadLocked/WriteLocked 컬럼 갱신. */
	if (ret) {
		SPDK_ERRLOG("lock unlock range error %d\n", ret);
	}

	ret += opal_end_session(dev, sess, dev->comid);
	if (ret) {
		SPDK_ERRLOG("end session error %d\n", ret);
	}

	free(sess);
	return ret;
}

/*
 * [한국어]
 * spdk_opal_cmd_setup_locking_range - LockingRange 객체의 RangeStart/Length/Enable 설정 (공개 API)
 *
 * @dev: 디바이스
 * @user: Admin1 또는 권한 부여된 User
 * @locking_range_id: 0=Global, 1~N=Range1..N
 * @range_start: LBA 시작 (Global은 무시)
 * @range_length: LBA 길이
 * @passwd: PIN
 * @return: 0=성공
 *
 * Range를 처음 정의할 때 호출 — RangeStart/Length를 정하고 Read/WriteLockEnabled를 모두 활성화.
 * 이후엔 spdk_opal_cmd_lock_unlock으로 잠금/해제만.
 */
int
spdk_opal_cmd_setup_locking_range(struct spdk_opal_dev *dev, enum spdk_opal_user user,
				  enum spdk_opal_locking_range locking_range_id, uint64_t range_start,
				  uint64_t range_length, const char *passwd)
{
	struct opal_session *sess;
	struct spdk_opal_key opal_key = {};
	int ret;

	assert(dev != NULL);

	ret = opal_init_key(&opal_key, passwd);
	if (ret != 0) {
		return ret;
	}

	sess = opal_alloc_session(dev);
	if (!sess) {
		return -ENOMEM;
	}

	ret = opal_start_auth_session(dev, sess, user, &opal_key);
	/* [한국어] User 권한으로 LockingSP 인증 세션. */
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d\n", ret);
		free(sess);
		return ret;
	}

	ret = opal_setup_locking_range(dev, sess, locking_range_id, range_start, range_length, true,
				       true);
	/* [한국어] ReadLockEnabled=true, WriteLockEnabled=true로 활성화 — 잠금 기능 사용 가능 상태. */
	if (ret) {
		SPDK_ERRLOG("setup locking range error %d\n", ret);
	}

	ret += opal_end_session(dev, sess, dev->comid);
	if (ret) {
		SPDK_ERRLOG("end session error %d\n", ret);
	}

	free(sess);
	return ret;
}

/*
 * [한국어]
 * spdk_opal_cmd_get_max_ranges - 디바이스가 지원하는 non-global LockingRange 최대 수 조회 (공개 API)
 *
 * @dev: 디바이스
 * @passwd: Admin1 PIN
 * @return: MaxRanges 또는 음수 에러
 *
 * dev->max_ranges 캐시 — 처음 조회 후엔 SSD에 다시 묻지 않음.
 */
int
spdk_opal_cmd_get_max_ranges(struct spdk_opal_dev *dev, const char *passwd)
{
	struct opal_session *sess;
	struct spdk_opal_key opal_key = {};
	int ret;

	assert(dev != NULL);

	if (dev->max_ranges) {
		/* [한국어] 캐시 히트 — 비싼 세션 작업 회피. */
		return dev->max_ranges;
	}

	ret = opal_init_key(&opal_key, passwd);
	if (ret != 0) {
		return ret;
	}

	sess = opal_alloc_session(dev);
	if (!sess) {
		return -ENOMEM;
	}

	ret = opal_start_auth_session(dev, sess, OPAL_ADMIN1, &opal_key);
	/* [한국어] Admin1 권한 — LockingInfoTable은 Admin1 권한 필요. */
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d\n", ret);
		free(sess);
		return ret;
	}

	ret = opal_get_max_ranges(dev, sess);
	if (ret > 0) {
		/* [한국어] 양수면 MaxRanges 값 — 캐시에 저장. */
		dev->max_ranges = ret;
	}

	ret = opal_end_session(dev, sess, dev->comid);
	if (ret) {
		SPDK_ERRLOG("end session error %d\n", ret);
	}

	free(sess);

	return (ret == 0 ? dev->max_ranges : ret);
	/* [한국어] EndSession도 성공이어야 캐시 값 반환. */
}

/*
 * [한국어]
 * spdk_opal_cmd_get_locking_range_info - 특정 Range의 메타 정보 회수 후 dev에 캐시 (공개 API)
 *
 * @dev: 디바이스
 * @passwd: User/Admin1 PIN
 * @user_id: 인증 권한
 * @locking_range_id: Range 번호
 * @return: 0=성공 (결과는 spdk_opal_get_locking_range_info로 조회)
 */
int
spdk_opal_cmd_get_locking_range_info(struct spdk_opal_dev *dev, const char *passwd,
				     enum spdk_opal_user user_id,
				     enum spdk_opal_locking_range locking_range_id)
{
	struct opal_session *sess;
	struct spdk_opal_key opal_key = {};
	int ret;

	assert(dev != NULL);

	ret = opal_init_key(&opal_key, passwd);
	if (ret != 0) {
		return ret;
	}

	sess = opal_alloc_session(dev);
	if (!sess) {
		return -ENOMEM;
	}

	ret = opal_start_auth_session(dev, sess, user_id, &opal_key);
	/* [한국어] User로 LockingSP 인증 세션. */
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d\n", ret);
		free(sess);
		return ret;
	}

	ret = opal_get_locking_range_info(dev, sess, locking_range_id);
	/* [한국어] 6 컬럼 GET — 결과는 dev->locking_ranges[id] 캐시에 저장. */
	if (ret) {
		SPDK_ERRLOG("get locking range info error %d\n", ret);
	}

	ret += opal_end_session(dev, sess, dev->comid);
	if (ret) {
		SPDK_ERRLOG("end session error %d\n", ret);
	}

	free(sess);
	return ret;
}

/*
 * [한국어]
 * spdk_opal_cmd_enable_user - User1..N Authority 활성화 (공개 API)
 *
 * @dev: 디바이스
 * @user_id: 활성화할 사용자 (OPAL_USER1..8)
 * @passwd: Admin1 PIN (LockingSP)
 * @return: 0=활성화 성공
 *
 * Admin1 권한으로 LockingSP에 들어가 User Authority의 Enabled=TRUE 설정.
 * 이 단계 후엔 spdk_opal_cmd_set_new_passwd로 해당 사용자에게 PIN 부여 필요.
 */
int
spdk_opal_cmd_enable_user(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
			  const char *passwd)
{
	struct opal_session *sess;
	struct spdk_opal_key opal_key = {};
	int ret;

	assert(dev != NULL);

	ret = opal_init_key(&opal_key, passwd);
	if (ret != 0) {
		return ret;
	}

	sess = opal_alloc_session(dev);
	if (!sess) {
		return -ENOMEM;
	}

	ret =  opal_start_generic_session(dev, sess, UID_ADMIN1, UID_LOCKINGSP,
					  opal_key.key, opal_key.key_len);
	/* [한국어] Admin1 권한으로 LockingSP 세션. */
	if (ret) {
		SPDK_ERRLOG("start locking SP session error %d\n", ret);
		free(sess);
		return ret;
	}

	ret = opal_enable_user(dev, sess, user_id);
	if (ret) {
		SPDK_ERRLOG("enable user error %d\n", ret);
	}

	ret += opal_end_session(dev, sess, dev->comid);
	if (ret) {
		SPDK_ERRLOG("end session error %d\n", ret);
	}

	free(sess);
	return ret;
}

/*
 * [한국어]
 * spdk_opal_cmd_add_user_to_locking_range - User에게 LockingRange 잠금 권한 부여 (공개 API)
 *
 * @dev: 디바이스
 * @user_id: 권한 받을 사용자
 * @locking_range_id: 대상 Range
 * @lock_flag: OPAL_READONLY=Read ACE, OPAL_READWRITE=Write ACE
 * @passwd: Admin1 PIN
 * @return: 0=성공
 *
 * Admin1로 LockingSP 들어가 ACE_RDLOCKED/ACE_WRLOCKED의 BooleanExpr 갱신 — 해당 user UID를 OR 조건에 추가.
 */
int
spdk_opal_cmd_add_user_to_locking_range(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
					enum spdk_opal_locking_range locking_range_id,
					enum spdk_opal_lock_state lock_flag, const char *passwd)
{
	struct opal_session *sess;
	struct spdk_opal_key opal_key = {};
	int ret;

	assert(dev != NULL);

	ret = opal_init_key(&opal_key, passwd);
	if (ret != 0) {
		return ret;
	}

	sess = opal_alloc_session(dev);
	if (!sess) {
		return -ENOMEM;
	}

	ret =  opal_start_generic_session(dev, sess, UID_ADMIN1, UID_LOCKINGSP,
					  opal_key.key, opal_key.key_len);
	/* [한국어] Admin1 권한 LockingSP 세션. */
	if (ret) {
		SPDK_ERRLOG("start locking SP session error %d\n", ret);
		free(sess);
		return ret;
	}

	ret = opal_add_user_to_locking_range(dev, sess, user_id, locking_range_id, lock_flag);
	/* [한국어] ACE BooleanExpr에 user 추가. */
	if (ret) {
		SPDK_ERRLOG("add user to locking range error %d\n", ret);
	}

	ret += opal_end_session(dev, sess, dev->comid);
	if (ret) {
		SPDK_ERRLOG("end session error %d\n", ret);
	}

	free(sess);
	return ret;
}

/*
 * [한국어]
 * spdk_opal_cmd_set_new_passwd - User/Admin1의 PIN 변경 (공개 API)
 *
 * @dev: 디바이스
 * @user_id: 대상 사용자
 * @new_passwd: 새 PIN
 * @old_passwd: 인증 PIN — new_user=true면 Admin1 PIN, 아니면 user_id 자신의 현재 PIN
 * @new_user: true=Admin1 권한으로 신규 사용자 PIN 부여, false=사용자 자신이 자기 PIN 변경
 * @return: 0=PIN 변경 성공
 *
 * 두 모드:
 *   - 새 사용자 등록: Admin1 권한으로 로그인 → 신규 user의 C_PIN 객체에 PIN 부여
 *   - 자기 PIN 변경: user 본인이 로그인 → 자기 C_PIN 갱신
 */
int
spdk_opal_cmd_set_new_passwd(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
			     const char *new_passwd, const char *old_passwd, bool new_user)
{
	struct opal_session *sess;
	struct spdk_opal_key old_key = {};
	struct spdk_opal_key new_key = {};
	int ret;

	assert(dev != NULL);

	ret = opal_init_key(&old_key, old_passwd);
	/* [한국어] 인증용 PIN 변환. */
	if (ret != 0) {
		return ret;
	}

	ret = opal_init_key(&new_key, new_passwd);
	/* [한국어] 새 PIN 변환. */
	if (ret != 0) {
		return ret;
	}

	sess = opal_alloc_session(dev);
	if (!sess) {
		return -ENOMEM;
	}

	ret = opal_start_auth_session(dev, sess, new_user ? OPAL_ADMIN1 : user_id,
				      &old_key);
	/* [한국어] 신규 등록 모드면 Admin1로 로그인, 자기 PIN 변경 모드면 user_id 본인으로 로그인. */
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d\n", ret);
		free(sess);
		return ret;
	}

	ret = opal_new_user_passwd(dev, sess, user_id, &new_key);
	/* [한국어] user_id의 C_PIN 객체에 new_key 설정. */
	if (ret) {
		SPDK_ERRLOG("set new passwd error %d\n", ret);
	}

	ret += opal_end_session(dev, sess, dev->comid);
	if (ret) {
		SPDK_ERRLOG("end session error %d\n", ret);
	}

	free(sess);
	return ret;
}

/*
 * [한국어]
 * spdk_opal_cmd_erase_locking_range - LockingRange Erase 호출 (공개 API, RangeStart/Length 리셋)
 *
 * @dev: 디바이스
 * @user_id: User 또는 Admin1
 * @locking_range_id: 대상 Range
 * @password: PIN
 * @return: 0=성공
 *
 * Erase 메서드는 Range의 RangeStart/Length를 디폴트로 되돌리고 잠금 해제. 데이터 자체는 유지
 * (cryptographic erase는 spdk_opal_cmd_secure_erase_locking_range 사용).
 */
int
spdk_opal_cmd_erase_locking_range(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
				  enum spdk_opal_locking_range locking_range_id, const char *password)
{
	struct opal_session *sess;
	struct spdk_opal_key opal_key = {};
	int ret;

	assert(dev != NULL);

	ret = opal_init_key(&opal_key, password);
	if (ret != 0) {
		return ret;
	}

	sess = opal_alloc_session(dev);
	if (!sess) {
		return -ENOMEM;
	}

	ret = opal_start_auth_session(dev, sess, user_id, &opal_key);
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d\n", ret);
		free(sess);
		return ret;
	}

	ret = opal_erase_locking_range(dev, sess, locking_range_id);
	/* [한국어] Erase 메서드 호출. */
	if (ret) {
		SPDK_ERRLOG("get active key error %d\n", ret);
	}

	ret += opal_end_session(dev, sess, dev->comid);
	if (ret) {
		SPDK_ERRLOG("end session error %d\n", ret);
	}

	free(sess);
	return ret;
}

/*
 * [한국어]
 * spdk_opal_cmd_secure_erase_locking_range - Cryptographic erase (DEK 회전) (공개 API)
 *
 * @dev: 디바이스
 * @user_id: User 또는 Admin1
 * @locking_range_id: 대상 Range
 * @password: PIN
 * @return: 0=성공
 *
 * 워크플로:
 *   1. opal_get_active_key → Range의 K_AES UID 조회
 *   2. opal_gen_new_active_key → GenKey 메서드로 새 DEK 생성 (기존 데이터는 즉시 복호화 불가)
 *   3. EndSession
 *
 * 이는 진짜 secure erase — 디바이스 내부 키만 폐기하므로 시간 ≈ ms 단위 (블록 덮어쓰기보다 훨씬 빠름).
 */
int
spdk_opal_cmd_secure_erase_locking_range(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
		enum spdk_opal_locking_range locking_range_id, const char *password)
{
	struct opal_session *sess;
	struct spdk_opal_key opal_key = {};
	struct spdk_opal_key *active_key;
	int ret;

	assert(dev != NULL);

	ret = opal_init_key(&opal_key, password);
	if (ret != 0) {
		return ret;
	}

	active_key = calloc(1, sizeof(*active_key));
	/* [한국어] K_AES UID 임시 저장 버퍼 (286B). */
	if (!active_key) {
		return -ENOMEM;
	}

	sess = opal_alloc_session(dev);
	if (!sess) {
		free(active_key);
		return -ENOMEM;
	}

	ret = opal_start_auth_session(dev, sess, user_id, &opal_key);
	if (ret) {
		SPDK_ERRLOG("start authenticate session error %d\n", ret);
		free(active_key);
		free(sess);
		return ret;
	}

	ret = opal_get_active_key(dev, sess, locking_range_id, active_key);
	/* [한국어] 단계 1: K_AES UID 조회. */
	if (ret) {
		SPDK_ERRLOG("get active key error %d\n", ret);
		goto end;
	}

	ret = opal_gen_new_active_key(dev, sess, active_key);
	/* [한국어] 단계 2: GenKey 호출 → DEK 회전 (기존 데이터는 cryptographic erase). */
	if (ret) {
		SPDK_ERRLOG("generate new active key error %d\n", ret);
		goto end;
	}
	memset(active_key, 0, sizeof(struct spdk_opal_key));
	/* [한국어] UID 메모리 클리어 — 비밀은 아니지만 보안 모범 사례. */

end:
	ret += opal_end_session(dev, sess, dev->comid);
	if (ret) {
		SPDK_ERRLOG("end session error %d\n", ret);
	}
	free(active_key);
	free(sess);
	return ret;
}

/*
 * [한국어]
 * spdk_opal_get_d0_features_info - Level 0 Discovery에서 캐시한 Feature 구조체 반환 (공개 API)
 *
 * @dev: 디바이스
 * @return: feat_info 포인터 (TPer/Locking/Geometry/SUM/Datastore/v100/v200 모두 포함)
 *
 * 호출자는 이 포인터로 디바이스 capability를 조회할 뿐, 수정 금지. 디바이스 lifetime 동안 유효.
 */
struct spdk_opal_d0_features_info *
spdk_opal_get_d0_features_info(struct spdk_opal_dev *dev)
{
	return &dev->feat_info;
	/* [한국어] dev 내부 캐시 직접 노출 — 디바이스 lifetime 동안 유효. */
}

/*
 * [한국어]
 * spdk_opal_get_locking_range_info - dev->locking_ranges[]에 캐시된 Range 메타 반환 (공개 API)
 *
 * @dev: 디바이스
 * @id: Range 번호
 * @return: locking_range_info 포인터
 *
 * 미리 spdk_opal_cmd_get_locking_range_info로 SSD에서 fetch해야 의미 있는 값.
 */
struct spdk_opal_locking_range_info *
spdk_opal_get_locking_range_info(struct spdk_opal_dev *dev, enum spdk_opal_locking_range id)
{
	assert(id < SPDK_OPAL_MAX_LOCKING_RANGE);
	/* [한국어] 배열 한도 검사. */
	return &dev->locking_ranges[id];
}

/*
 * [한국어]
 * spdk_opal_free_locking_range_info - 캐시된 Range 메타 클리어 (공개 API)
 *
 * @dev: 디바이스
 * @id: Range 번호
 *
 * 메모리 해제는 아니고 in-place memset(0) — dev 자체가 살아있는 동안 다시 채워질 수 있음.
 * dev free와 무관하게 보안상 민감한 캐시(예: ActiveKey UID)를 미리 지우고 싶을 때 사용.
 */
void
spdk_opal_free_locking_range_info(struct spdk_opal_dev *dev, enum spdk_opal_locking_range id)
{
	struct spdk_opal_locking_range_info *info;

	assert(id < SPDK_OPAL_MAX_LOCKING_RANGE);
	info = &dev->locking_ranges[id];
	memset(info, 0, sizeof(*info));
	/* [한국어] in-place 0 클리어 — 다음 GET이 다시 채울 수 있음. */
}

/* Log component for opal submodule */
/* [한국어] SPDK 로깅 시스템에 "opal" 컴포넌트 등록 — SPDK_DEBUGLOG(opal, ...) 매크로 활성화.
 * 사용자가 spdk_log_set_flag("opal")로 디버그 로그 켜기/끄기 가능. */
SPDK_LOG_REGISTER_COMPONENT(opal)
