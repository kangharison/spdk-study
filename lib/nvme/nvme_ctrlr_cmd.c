/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation. All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * [한국어 설명] NVMe Admin/Controller 커맨드 빌더 (nvme_ctrlr_cmd.c) — 1048 라인 원본
 *
 * === 파일의 역할 ===
 * SPDK NVMe 드라이버에서 컨트롤러(=Admin) 레벨 커맨드의 SQE를 빌드하고
 * Admin Queue Pair에 제출하는 "단일 책임 빌더 모듈". 이 파일은 NVMe Admin
 * Command Set (NVMe Base Spec 5.x장)의 거의 모든 명령에 대해 1:1로
 * `nvme_ctrlr_cmd_*()` / `spdk_nvme_ctrlr_cmd_*()` 진입점을 노출한다.
 * 처리 패턴은 단순하고 일관적이다 — (1) 컨트롤러 락 획득, (2) admin qpair
 * 풀에서 nvme_request 확보, (3) SQE의 opcode/CDW10~15 필드 채움,
 * (4) `nvme_ctrlr_submit_admin_request()`로 큐에 푸시, (5) 락 해제 후 rc 반환.
 * SQE 채우는 부분이 곧 NVMe 스펙 dword 매핑이며, 이 파일을 읽으면 SPDK가
 * 어떤 admin opcode를 어떻게 호출하는지 한눈에 파악할 수 있다.
 *
 * 지원 명령(파일 정의 순서):
 *   - PCIe raw I/O passthru (no-payload / contig / contig+md / iov+md)
 *   - admin raw passthru (spdk_nvme_ctrlr_cmd_admin_raw)
 *   - IDENTIFY (CNS=0/1/0x10/0x11/0x13… ; CNTID, CSI 포함)
 *   - Namespace Attachment (Attach/Detach Controller)
 *   - Namespace Management (Create/Delete)
 *   - Doorbell Buffer Config (Shadow Doorbell, NVMe 1.3+)
 *   - Format NVM
 *   - Set/Get Features (FID 단위; controller-wide 또는 namespace 단위)
 *     · Number of Queues (FID 0x07)
 *     · Async Event Configuration (FID 0x0B)
 *     · Host Identifier (FID 0x81; 64/128-bit)
 *   - Get Log Page (LID, NUMD, LPOL/LPOU offset)
 *   - Abort (단일 cid + 큐된 abort 재발행 로직 + Abort Limit ACL 처리)
 *   - Firmware Image Download / Commit
 *   - Security Send/Receive (TCG 등 SPSP/NSSF/SECP)
 *   - Sanitize
 *   - Directive Send/Receive
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 SPDK NVMe 유저스페이스 드라이버의 "Admin command translation
 * layer"이다. 상위에서 호출하는 주체는 (a) 드라이버 초기화 코드인
 * lib/nvme/nvme_ctrlr.c (process_init 단계에서 IDENTIFY/Set Features 등
 * 다수 발행), (b) 외부 공개 API를 통해 사용자 애플리케이션이 직접 호출,
 * (c) NVMe-oF target에서 management 명령을 위임하는 경로 등이다.
 * 하위에서는 lib/nvme/nvme.c의 `nvme_allocate_request_*` 풀 헬퍼와,
 * `nvme_ctrlr_submit_admin_request()`(nvme_internal.h)를 호출한다. 후자는
 * 트랜스포트 계층(`nvme_transport_qpair_submit_request`)으로 넘어가며,
 * PCIe면 SQ memory에 64B SQE를 기록하고 doorbell BAR에 tail을 쓴다.
 *
 * 실행 컨텍스트: 호출 스레드(보통 컨트롤러를 attach한 스레드 = mainline
 * 스레드). nvme_ctrlr_lock()으로 ctrlr-wide 직렬화를 보장 — admin qpair는
 * 본질적으로 단일 큐이므로 멀티 스레드가 동시에 admin을 발행하면 락으로
 * 직렬화. 완료 콜백은 향후 `spdk_nvme_ctrlr_process_admin_completions()`가
 * polled-mode로 CQ를 폴링하며 호출.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - nvme_internal.h: struct spdk_nvme_ctrlr/qpair, nvme_request,
 *     nvme_allocate_request_*(), nvme_ctrlr_submit_admin_request(),
 *     nvme_ctrlr_lock/unlock(), spdk_nvme_bytes_to_numd().
 *   - spdk/nvme.h: 공개 API (spdk_nvme_ctrlr_cmd_* prototype, opcode/feat enum).
 * 데이터 흐름:
 *   호출자 → 이 파일에서 SQE 빌드 → admin qpair tail에 enqueue → 트랜스포트
 *   submit → 디바이스 → 디바이스가 CQ에 CQE 작성 → polled 컨슈머가 cb_fn 호출.
 * 공유 자료구조:
 *   - struct spdk_nvme_ctrlr (ctrlr->adminq, ctrlr->cdata, ctrlr->outstanding_aborts
 *     등). 이 파일은 cdata.acl(Abort Command Limit)·cdata.lpa.lpeds(Log Page
 *     Extended Data Support)·queued_aborts(STAILQ)에 직접 접근.
 *   - nvme_request: 풀 객체. cmd(64B SQE), cb_fn/cb_arg, parent/children(abort_ext
 *     의 fan-out), user_cb_fn/arg(원본 사용자 콜백 보존) 등을 담음.
 * 에러 매핑:
 *   - 풀 고갈: -ENOMEM (호출자가 admin completions polling 후 재시도)
 *   - 잘못된 인자(예: log offset 정렬, payload_size=0, host_id_size 부적합):
 *     -EINVAL을 즉시 반환.
 *   - Abort 큐 초과(ACL+1): 즉시 큐잉 후 0 반환, 이전 abort 완료 시 재발행.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_nvme_ctrlr_cmd_admin_raw():  사용자 정의 admin 명령 그대로 발행
 *   - spdk_nvme_ctrlr_cmd_io_raw / _with_md / iov_with_md:  IO qpair raw 발행
 *   - nvme_ctrlr_cmd_identify():        IDENTIFY (CNS+CNTID+CSI 조합)
 *   - nvme_ctrlr_cmd_attach_ns / detach_ns / create_ns / delete_ns
 *   - nvme_ctrlr_cmd_doorbell_buffer_config(): Shadow Doorbell 설정
 *   - nvme_ctrlr_cmd_format():          Format NVM
 *   - spdk_nvme_ctrlr_cmd_set_feature / get_feature [_ns]: FID 기반
 *   - nvme_ctrlr_cmd_set_num_queues / get_num_queues: NSQR/NCQR 협상
 *   - nvme_ctrlr_cmd_set_async_event_config / set_host_id
 *   - spdk_nvme_ctrlr_cmd_get_log_page[_ext]: LID/NUMD/LPO 오프셋 처리
 *   - spdk_nvme_ctrlr_cmd_abort / abort_ext: 단일 cid abort 및 cb_arg 매칭 fan-out
 *   - nvme_ctrlr_cmd_fw_image_download / fw_commit
 *   - spdk_nvme_ctrlr_cmd_security_send / receive
 *   - nvme_ctrlr_cmd_sanitize
 *   - nvme_ctrlr_cmd_directive (+ send/receive wrapper)
 */

#include "nvme_internal.h"   /* [한국어] SPDK NVMe 드라이버 내부 타입·헬퍼 — nvme_request, spdk_nvme_ctrlr/qpair, nvme_allocate_request_*, nvme_ctrlr_lock/unlock, nvme_ctrlr_submit_admin_request 선언 포함 */
#include "spdk/nvme.h"       /* [한국어] SPDK NVMe 공개 API — spdk_nvme_ctrlr_cmd_* prototype, opcode/CDW 비트필드 enum, struct spdk_nvme_cmd 정의 */

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_io_cmd_raw_no_payload_build - PCIe 전용 raw I/O 빌더 (페이로드 없음)
 *
 * @ctrlr:  대상 NVMe 컨트롤러 (이 함수는 PCIe 트랜스포트만 허용)
 * @qpair:  명령을 발행할 I/O qpair (admin이 아닌 사용자 IO qpair)
 * @cmd:    호출자가 미리 채운 64B SQE — opcode와 모든 CDW를 사용자가 직접 설정
 * @cb_fn:  완료 시 호출될 콜백 (cqe와 cb_arg가 전달됨)
 * @cb_arg: 콜백 인자
 * @return: 0 성공, -EINVAL(PCIe 아님), -ENOMEM(요청 풀 고갈)
 *
 * 동기/배경: NVMe Vendor Specific 명령이나 SPDK가 직접 노출하지 않는 특수
 * IO opcode를 호출자가 자유롭게 발행하기 위함. 페이로드(PRP/SGL)가 없는
 * 명령(Flush 등)을 가정. PCIe 전용 — Fabrics(RDMA/TCP)는 transport-level
 * 페이로드 변환 규칙이 다르기 때문에 거부.
 *
 * 호출 체인:
 *   [user/test] → spdk_nvme_ctrlr_cmd_io_cmd_raw_no_payload_build
 *     → nvme_allocate_request → nvme_qpair_submit_request
 *       → nvme_transport_qpair_submit_request (PCIe) → SQ doorbell ring
 */
int
spdk_nvme_ctrlr_io_cmd_raw_no_payload_build(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cmd *cmd,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;        /* [한국어] qpair 요청 풀에서 받아올 nvme_request 포인터 — SQE/콜백/payload meta 보유 */
	struct nvme_payload payload;     /* [한국어] payload 디스크립터(스택 임시) — 빈 payload를 만들기 위함 (PRP1/PRP2가 0으로 채워질 명령용) */

	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
                                  /* [한국어] PCIe 전용 가드 — Fabrics는 SGL/Capsule 변환이 별도이므로 raw 패스가 다른 경로 사용 */
		return -EINVAL;          /* [한국어] PCIe가 아니면 즉시 거부 */
	}

	memset(&payload, 0, sizeof(payload));
                                  /* [한국어] payload를 0으로 초기화 — 타입(NVME_PAYLOAD_TYPE_INVALID=0), 콜백/포인터 모두 NULL → "no data transfer" */
	req = nvme_allocate_request(qpair, &payload, 0, 0, cb_fn, cb_arg);
                                  /* [한국어] qpair 풀에서 nvme_request 한 개 확보 — payload_size=0, md_size=0 (전송 데이터 없음) */

	if (req == NULL) {
                                  /* [한국어] 풀 고갈 시 NULL — 호출자는 process_completions 후 재시도해야 함 */
		return -ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));
                                  /* [한국어] 호출자가 작성한 64B SQE를 그대로 복사 — opc/cdw0~15 사용자 책임 */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] IO qpair에 enqueue → 트랜스포트가 SQ에 기록 후 doorbell 갱신 */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_io_raw - 사용자 IO qpair에 임의 명령(연속 버퍼) 발행
 *
 * @ctrlr/qpair/cmd: 위와 동일
 * @buf:   데이터 버퍼 (가상주소; SPDK가 PRP/SGL로 변환)
 * @len:   버퍼 길이(바이트)
 * @return: 0 성공, -ENOMEM (풀 고갈)
 *
 * NVMe vendor-specific opcode 등을 가상주소 + 길이만으로 발행하는 편의 API.
 * payload는 CONTIG 타입 — 단일 가상주소 영역으로, 트랜스포트가 페이지 경계
 * 마다 PRP entry를 만들어 PRP1/PRP2를 채운다.
 *
 * 호출 체인:
 *   [user/bdev_nvme] → spdk_nvme_ctrlr_cmd_io_raw
 *     → nvme_allocate_request_contig → nvme_qpair_submit_request
 */
int
spdk_nvme_ctrlr_cmd_io_raw(struct spdk_nvme_ctrlr *ctrlr,
			   struct spdk_nvme_qpair *qpair,
			   struct spdk_nvme_cmd *cmd,
			   void *buf, uint32_t len,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;    /* [한국어] qpair 풀에서 가져올 요청 객체 */

	req = nvme_allocate_request_contig(qpair, buf, len, cb_fn, cb_arg);
                                  /* [한국어] CONTIG payload 모드로 요청 할당 — 트랜스포트가 buf의 가상→물리 변환 후 PRP 빌드 */

	if (req == NULL) {
                                  /* [한국어] 풀 고갈 — 일시적, 재시도 권장 */
		return -ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));
                                  /* [한국어] 사용자 SQE 그대로 복사 — DPTR(PRP)는 트랜스포트가 채움 (호출자가 채워도 무시되지 않음 주의) */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] IO qpair로 enqueue */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_io_raw_with_md - raw IO 명령 + 메타데이터 분리 버퍼
 *
 * @md_buf: 메타데이터 버퍼(별도 분리). NULL이면 메타 없음.
 * 그 외 인자/반환은 io_raw와 동일.
 *
 * 메타 분리 모드(=non-extended LBA, MPTR 사용) 명령 발행에 사용. md_len은
 * NS의 sector_size와 md_size를 곱해 자동 계산 — 호출자가 별도로 넘길 필요 없음.
 */
int
spdk_nvme_ctrlr_cmd_io_raw_with_md(struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_nvme_qpair *qpair,
				   struct spdk_nvme_cmd *cmd,
				   void *buf, uint32_t len, void *md_buf,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct nvme_payload payload;     /* [한국어] CONTIG + md 분리 payload 디스크립터 */
	uint32_t md_len = 0;             /* [한국어] 메타 길이(바이트) — md_buf가 없으면 0 유지 */

	payload = NVME_PAYLOAD_CONTIG(buf, md_buf);
                                  /* [한국어] payload 매크로 — type=CONTIG, contig.buf=buf, md=md_buf 설정 */

	/* Calculate metadata length */
	if (md_buf) {
                                  /* [한국어] 메타 버퍼가 주어졌으면 길이 계산 */
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, cmd->nsid);
                                  /* [한국어] cmd->nsid로 NS 객체 조회 — sector_size와 md_size를 알기 위함 */

		assert(ns != NULL);          /* [한국어] 유효 NS여야 함 (호출자가 cmd->nsid를 올바로 설정한 가정) */
		assert(ns->sector_size != 0);/* [한국어] active NS는 sector_size > 0 — 0이면 NS uninit */
		md_len =  len / ns->sector_size * ns->md_size;
                                  /* [한국어] 섹터 수 × 섹터당 메타 크기 = 총 메타 바이트 (예: 4KB/512B × 8B = 64B) */
	}

	req = nvme_allocate_request(qpair, &payload, len, md_len, cb_fn, cb_arg);
                                  /* [한국어] 일반 할당자 — payload+meta 정보 포함 */
	if (req == NULL) {
                                  /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));
                                  /* [한국어] 사용자 SQE 복사 (MPTR은 트랜스포트가 채움) */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] IO qpair에 enqueue */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_iov_raw_with_md - raw IO + 메타 + SGL(scatter list) 콜백 모드
 *
 * @reset_sgl_fn: payload 오프셋을 0으로 리셋하는 콜백 (트랜스포트가 분할마다 호출)
 * @next_sge_fn:  다음 SGE(주소+길이)를 콜백 ctx로 반환
 *
 * 호스트가 분산된 여러 영역으로 데이터를 가지고 있을 때 SGL/PRP list를
 * 트랜스포트가 콜백으로 walk하면서 빌드하는 방식. bdev_nvme의 zero-copy
 * 경로 등에서 사용.
 */
int
spdk_nvme_ctrlr_cmd_iov_raw_with_md(struct spdk_nvme_ctrlr *ctrlr,
				    struct spdk_nvme_qpair *qpair,
				    struct spdk_nvme_cmd *cmd,
				    uint32_t len, void *md_buf,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				    spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				    spdk_nvme_req_next_sge_cb next_sge_fn)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct nvme_payload payload;     /* [한국어] SGL 모드 payload 디스크립터 */
	uint32_t md_len = 0;             /* [한국어] 메타 길이 */

	if (reset_sgl_fn == NULL || next_sge_fn == NULL) {
                                  /* [한국어] SGL 모드는 두 콜백이 모두 필수 — 없으면 walk 불가 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_SGL(reset_sgl_fn, next_sge_fn, cb_arg, md_buf);
                                  /* [한국어] payload 매크로 — type=SGL, 콜백/ctx/메타 버퍼 설정 (cb_arg를 SGL 콜백에 전달) */

	/* Calculate metadata length */
	if (md_buf) {
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, cmd->nsid);
                                  /* [한국어] NS lookup으로 sector/md 크기 획득 */

		assert(ns != NULL);          /* [한국어] NS 유효성 단언 */
		assert(ns->sector_size != 0);/* [한국어] sector_size > 0 보장 */
		md_len = len / ns->sector_size * ns->md_size;
                                  /* [한국어] 섹터수 × 메타바이트 = 메타 총 길이 */
	}

	req = nvme_allocate_request(qpair, &payload, len, md_len, cb_fn, cb_arg);
                                  /* [한국어] 요청 할당 (SGL payload 정보 포함) */
	if (req == NULL) {
                                  /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));
                                  /* [한국어] 사용자 SQE 복사 */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] IO qpair enqueue */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_admin_raw - admin qpair에 임의 admin 명령 raw 발행
 *
 * @cmd:  사용자가 작성한 admin SQE (opc/cdw 모두 채워야 함)
 * @buf, len: 데이터 페이로드(in/out 방향은 opc에 따름)
 *
 * SPDK가 직접 wrapper를 제공하지 않는 admin opcode(예: vendor-specific)나
 * 디버깅/시험 도구에서 사용. 본 파일의 다른 admin 함수들과 동일하게
 * ctrlr lock으로 직렬화.
 */
int
spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
			      struct spdk_nvme_cmd *cmd,
			      void *buf, uint32_t len,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;    /* [한국어] 요청 객체 */
	int			rc;          /* [한국어] submit 결과 코드 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 자원 직렬화 — ctrlr->adminq 동시 접근 방지 (멀티 스레드 호출 가능) */
	req = nvme_allocate_request_contig(ctrlr->adminq, buf, len, cb_fn, cb_arg);
                                  /* [한국어] admin qpair 풀에서 CONTIG payload 요청 할당 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 락 누수 방지 */
		return -ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));
                                  /* [한국어] 사용자 SQE 그대로 복사 */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin qpair 전용 submit 헬퍼 (내부에서 nvme_qpair_submit_request 호출) */

	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 — submit 후 즉시 해제 (완료는 polling에서 처리) */
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_identify - IDENTIFY admin 명령 발행 (opc=0x06, NVMe Spec §5.17)
 *
 * @cns:    Controller or Namespace Structure (CDW10[7:0])
 *          0x00=NS data, 0x01=Controller data, 0x02=Active NS list,
 *          0x10=Allocated NS ID list, 0x11=Allocated NS data,
 *          0x12=Attached Controller list, 0x13=Controller list,
 *          0x14=Primary Controller Caps, 0x15=Secondary Controller list … (스펙 표 참조)
 * @cntid:  Controller Identifier (CDW10[31:16]) — CNS=0x12/0x13 등에서 의미
 * @nsid:   Namespace Identifier (CDW1) — CNS=0x00/0x11 등에서 의미
 * @csi:    Command Set Identifier (CDW11[31:24]) — 0=NVM, 1=KV, 2=ZNS (NVMe 2.0)
 * @payload, payload_size: IDENTIFY 응답 버퍼 (4096B 권장)
 *
 * SPDK 드라이버 초기화에서 nvme_ctrlr_identify(), nvme_ns_construct() 등이
 * 이 함수를 통해 컨트롤러/NS/feature 메타데이터를 수집한다. payload는
 * `nvme_allocate_request_user_copy` (false=device→host) 패턴으로 처리되어
 * 완료 후 사용자 buf로 복사된다.
 *
 * 호출 체인:
 *   nvme_ctrlr_identify()/nvme_ns_construct() → nvme_ctrlr_cmd_identify
 *     → nvme_ctrlr_submit_admin_request → device → CQE → user copy → cb_fn
 */
int
nvme_ctrlr_cmd_identify(struct spdk_nvme_ctrlr *ctrlr, uint8_t cns, uint16_t cntid, uint32_t nsid,
			uint8_t csi, void *payload, size_t payload_size,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;       /* [한국어] req->cmd로의 별칭 — SQE 필드 가독성 향상 */
	int		     rc;         /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 락 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, payload_size,
					      cb_fn, cb_arg, false);
                                  /* [한국어] user_copy 모드 — host_to_ctrlr=false (응답을 받음). 내부에서 DMA 가능 버퍼를 복제 후 완료 시 user payload로 복사 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 시 락 해제 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                 /* [한국어] SQE 편집을 위한 별칭 */
	cmd->opc = SPDK_NVME_OPC_IDENTIFY; /* [한국어] opcode=0x06 (Admin Command Set, IDENTIFY) */
	cmd->cdw10_bits.identify.cns = cns;     /* [한국어] CDW10[7:0] = CNS — 어떤 구조를 받을지 결정 */
	cmd->cdw10_bits.identify.cntid = cntid; /* [한국어] CDW10[31:16] = CNTID — Controller ID 필터 (해당 CNS에서만 의미) */
	cmd->cdw11_bits.identify.csi = csi;     /* [한국어] CDW11[31:24] = CSI — Command Set Identifier (NVMe 2.0 ZNS/KV 지원) */
	cmd->nsid = nsid;                /* [한국어] CDW1 = NSID — CNS=0x00 등에서 어떤 NS를 가리킬지 */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin qpair에 enqueue */

	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_attach_ns - Namespace Attachment(opc=0x15) — Controller Attach
 *
 * @nsid:    attach 대상 NS ID
 * @payload: 컨트롤러 ID 리스트 (struct spdk_nvme_ctrlr_list, 4KB) — 어떤 ctrlr ID들에 attach할지
 *
 * SEL=Controller Attach (CDW10[3:0]=0x0). NVMe Spec §5.20 NS Attachment.
 * payload는 host→device 방향(true).
 */
int
nvme_ctrlr_cmd_attach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request			*req;    /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd			*cmd;    /* [한국어] SQE 별칭 */
	int					rc;      /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);                  /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, sizeof(struct spdk_nvme_ctrlr_list),
					      cb_fn, cb_arg, true);
                                  /* [한국어] host→device(true) — 컨트롤러 리스트를 장치로 보냄 (4KB 페이지) */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);            /* [한국어] 풀 고갈 시 락 해제 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                         /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_NS_ATTACHMENT;  /* [한국어] opcode=0x15 (Namespace Attachment) */
	cmd->nsid = nsid;                        /* [한국어] CDW1=NSID — 대상 NS */
	cmd->cdw10_bits.ns_attach.sel = SPDK_NVME_NS_CTRLR_ATTACH;
                                  /* [한국어] CDW10[3:0]=SEL=0x0 (Controller Attach) — 0x1이면 Detach */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */

	nvme_ctrlr_unlock(ctrlr);                /* [한국어] 락 해제 */
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_detach_ns - Namespace Attachment(opc=0x15) — Controller Detach
 *
 * attach와 동일하나 SEL=0x1 (Controller Detach).
 */
int
nvme_ctrlr_cmd_detach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request			*req;    /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd			*cmd;    /* [한국어] SQE 별칭 */
	int					rc;      /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);                  /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, sizeof(struct spdk_nvme_ctrlr_list),
					      cb_fn, cb_arg, true);
                                  /* [한국어] host→device — 컨트롤러 리스트를 장치로 송신 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);            /* [한국어] 풀 고갈 시 해제 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                         /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_NS_ATTACHMENT;  /* [한국어] opcode=0x15 */
	cmd->nsid = nsid;                        /* [한국어] CDW1=NSID */
	cmd->cdw10_bits.ns_attach.sel = SPDK_NVME_NS_CTRLR_DETACH;
                                  /* [한국어] CDW10[3:0]=SEL=0x1 (Controller Detach) */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */

	nvme_ctrlr_unlock(ctrlr);                /* [한국어] 락 해제 */
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_create_ns - Namespace Management(opc=0x0D) — Create
 *
 * @payload: NS data structure (4KB) — NSZE/NCAP/FLBAS/DPS 등 생성할 NS의 형상
 *
 * SEL=Create. 성공 시 cqe.cdw0에 새 NSID가 반환됨. 후속으로 attach_ns 호출이
 * 일반적인 시퀀스. NVMe Spec §5.21 NS Management.
 */
int
nvme_ctrlr_cmd_create_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns_data *payload,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request			*req;    /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd			*cmd;    /* [한국어] SQE 별칭 */
	int					rc;      /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);                  /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, sizeof(struct spdk_nvme_ns_data),
					      cb_fn, cb_arg, true);
                                  /* [한국어] host→device — NS data 구조 전송 (4KB) */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);            /* [한국어] 풀 고갈 시 해제 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                         /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_NS_MANAGEMENT;  /* [한국어] opcode=0x0D (NS Management) */
	cmd->cdw10_bits.ns_manage.sel = SPDK_NVME_NS_MANAGEMENT_CREATE;
                                  /* [한국어] CDW10[3:0]=SEL=0x0 (Create) — 응답 cdw0에 새 NSID */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */

	nvme_ctrlr_unlock(ctrlr);                /* [한국어] 락 해제 */
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_delete_ns - Namespace Management(opc=0x0D) — Delete
 *
 * @nsid: 삭제할 NS ID (0xFFFFFFFF면 모든 NS)
 *
 * SEL=Delete (0x1). 페이로드 없음(_null 할당자). NVMe Spec §5.21.
 */
int
nvme_ctrlr_cmd_delete_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, spdk_nvme_cmd_cb cb_fn,
			 void *cb_arg)
{
	struct nvme_request			*req;    /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd			*cmd;    /* [한국어] SQE 별칭 */
	int					rc;      /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);                  /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
                                  /* [한국어] payload 없는 요청 할당 (Delete는 in/out 데이터 없음) */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);            /* [한국어] 풀 고갈 시 해제 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                         /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_NS_MANAGEMENT;  /* [한국어] opcode=0x0D */
	cmd->cdw10_bits.ns_manage.sel = SPDK_NVME_NS_MANAGEMENT_DELETE;
                                  /* [한국어] CDW10[3:0]=SEL=0x1 (Delete) */
	cmd->nsid = nsid;                        /* [한국어] CDW1=NSID — 0xFFFFFFFF면 모든 NS 삭제 */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */

	nvme_ctrlr_unlock(ctrlr);                /* [한국어] 락 해제 */
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_doorbell_buffer_config - Shadow Doorbell 설정 (opc=0x7C, NVMe 1.3+)
 *
 * @prp1: Shadow Doorbell Buffer 물리주소 (PRP1) — 호스트가 갱신, 디바이스가 polling
 * @prp2: EventIdx Buffer 물리주소 (PRP2) — 디바이스가 어디까지 봤는지 알림
 *
 * 가상화/하이퍼바이저 환경에서 MMIO doorbell write의 비싼 VM-exit를 줄이기
 * 위한 NVMe 기능. SPDK 드라이버는 vfio-user/QEMU 환경에서 자동 협상.
 * 페이로드 없음 — 주소만 전달.
 */
int
nvme_ctrlr_cmd_doorbell_buffer_config(struct spdk_nvme_ctrlr *ctrlr, uint64_t prp1, uint64_t prp2,
				      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request			*req;    /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd			*cmd;    /* [한국어] SQE 별칭 */
	int					rc;      /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);                  /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
                                  /* [한국어] in/out 데이터 없음 → null 할당자 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);            /* [한국어] 풀 고갈 시 해제 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                         /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG;
                                  /* [한국어] opcode=0x7C (Doorbell Buffer Config) */
	cmd->dptr.prp.prp1 = prp1;               /* [한국어] DPTR.PRP1 = Shadow Doorbell 버퍼 PA — 4KB 정렬 */
	cmd->dptr.prp.prp2 = prp2;               /* [한국어] DPTR.PRP2 = EventIdx 버퍼 PA */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */

	nvme_ctrlr_unlock(ctrlr);                /* [한국어] 락 해제 */
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_format - Format NVM (opc=0x80, NVMe Spec §5.14)
 *
 * @nsid:   포맷할 NS (0xFFFFFFFF=전체)
 * @format: struct spdk_nvme_format — LBAF/MS/PI/PIL/SES 등 32-bit 비트필드
 *          이를 CDW10에 그대로 복사한다.
 *
 * 페이로드 없음. 디바이스가 NS를 미디어 단위로 재포맷(보안 삭제 옵션 SES 포함).
 */
int
nvme_ctrlr_cmd_format(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, struct spdk_nvme_format *format,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;       /* [한국어] SQE 별칭 */
	int rc;                          /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
                                  /* [한국어] payload 없음 → null 할당자 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                 /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_FORMAT_NVM; /* [한국어] opcode=0x80 (Format NVM) */
	cmd->nsid = nsid;                /* [한국어] CDW1=NSID */
	memcpy(&cmd->cdw10, format, sizeof(uint32_t));
                                  /* [한국어] CDW10 = format 비트필드(LBAF[3:0], MS[4], PI[7:5], PIL[8], SES[11:9]) — struct가 32-bit 그대로 매핑 */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */
	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_set_feature - Set Features (opc=0x09, NVMe Spec §5.27)
 *
 * @feature: FID (Feature Identifier) — CDW10[7:0]
 * @cdw11/cdw12: feature별 비트필드 (host가 호출 전 union 비트필드로 인코딩)
 * @payload, payload_size: 일부 feature에 따른 부가 데이터 (예: Host Identifier, LBA Range Type)
 *
 * Feature ID는 NVMe 스펙 Figure 314 (Feature Identifiers) 참조 (예:
 * 0x01=Arbitration, 0x02=Power Management, 0x04=Temperature Threshold,
 * 0x07=Number of Queues, 0x0B=Async Event Configuration, 0x81=Host Identifier).
 * payload host→device(true).
 */
int
spdk_nvme_ctrlr_cmd_set_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				uint32_t cdw11, uint32_t cdw12, void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;       /* [한국어] SQE 별칭 */
	int rc;                          /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size, cb_fn, cb_arg,
					      true);
                                  /* [한국어] host→device — feature payload 송신 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                 /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_SET_FEATURES;   /* [한국어] opcode=0x09 (Set Features) */
	cmd->cdw10_bits.set_features.fid = feature;
                                  /* [한국어] CDW10[7:0] = FID. CDW10[31]=SV(Save) 옵션은 호출자가 set 하지 않음(휘발성 default) */
	cmd->cdw11 = cdw11;              /* [한국어] CDW11 = feature별 인코딩 (예: Number of Queues면 NSQR/NCQR) */
	cmd->cdw12 = cdw12;              /* [한국어] CDW12 = feature별 추가 인코딩 (대부분 0; Timestamp/HMB 일부에서 사용) */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */
	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_get_feature - Get Features (opc=0x0A, NVMe Spec §5.15)
 *
 * @feature: FID
 * @cdw11:   일부 feature(예: SEL 비트, Save 비트)에서 사용
 * @payload: 응답 데이터 (FID에 따라 0~4KB; Get Features는 보통 cdw0에 값을 반환)
 *
 * 응답은 cqe.cdw0에 들어오는 경우가 대부분이며, payload는 일부 FID에서만 의미.
 * device→host(false).
 */
int
spdk_nvme_ctrlr_cmd_get_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				uint32_t cdw11, void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;       /* [한국어] SQE 별칭 */
	int rc;                          /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size, cb_fn, cb_arg,
					      false);
                                  /* [한국어] device→host — payload 응답 수신 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                 /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_GET_FEATURES;   /* [한국어] opcode=0x0A (Get Features) */
	cmd->cdw10_bits.get_features.fid = feature;
                                  /* [한국어] CDW10[7:0] = FID. CDW10[10:8]=SEL(Current/Default/Saved/Caps) */
	cmd->cdw11 = cdw11;              /* [한국어] CDW11 = FID별 입력 (예: HMB의 EHM, NS-specific feature의 NS context) */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */
	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_get_feature_ns - Get Features의 NS 지정 버전
 *
 * NS-specific feature(예: 0x05=Error Recovery)에 대해 nsid를 명시.
 * 그 외 동작은 get_feature와 동일.
 */
int
spdk_nvme_ctrlr_cmd_get_feature_ns(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				   uint32_t cdw11, void *payload,
				   uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
				   void *cb_arg, uint32_t ns_id)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;       /* [한국어] SQE 별칭 */
	int rc;                          /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size, cb_fn, cb_arg,
					      false);
                                  /* [한국어] device→host */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                 /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_GET_FEATURES;
                                  /* [한국어] opcode=0x0A */
	cmd->cdw10_bits.get_features.fid = feature; /* [한국어] CDW10[7:0]=FID */
	cmd->cdw11 = cdw11;              /* [한국어] CDW11 입력 */
	cmd->nsid = ns_id;               /* [한국어] CDW1=NSID — NS-specific feature 컨텍스트 */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */
	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_set_feature_ns - Set Features의 NS 지정 버전
 *
 * Set + nsid 조합. host→device(true). NS-specific feature 변경에 사용.
 */
int
spdk_nvme_ctrlr_cmd_set_feature_ns(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				   uint32_t cdw11, uint32_t cdw12, void *payload,
				   uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
				   void *cb_arg, uint32_t ns_id)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;       /* [한국어] SQE 별칭 */
	int rc;                          /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size, cb_fn, cb_arg,
					      true);
                                  /* [한국어] host→device — payload 송신 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                 /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_SET_FEATURES;
                                  /* [한국어] opcode=0x09 */
	cmd->cdw10_bits.set_features.fid = feature; /* [한국어] CDW10[7:0]=FID */
	cmd->cdw11 = cdw11;              /* [한국어] CDW11 인코딩 */
	cmd->cdw12 = cdw12;              /* [한국어] CDW12 인코딩 */
	cmd->nsid = ns_id;               /* [한국어] CDW1=NSID — NS-specific feature 컨텍스트 */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */
	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */

	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_set_num_queues - Set Features FID=0x07 (Number of Queues) 협상
 *
 * @num_queues: 호스트가 요청하는 IO qpair 수 (1-based 입력)
 *
 * 디바이스는 cqe.cdw0[15:0]/[31:16]에 실제 허용 NSQ/NCQ를 0-based로 반환.
 * SPDK는 ctrlr init에서 이 값을 받아 IO qpair 풀 크기를 결정.
 *
 * NVMe 스펙: NSQR (Number of I/O Submission Queues Requested, CDW11[15:0]),
 * NCQR (Completion, CDW11[31:16]), 둘 다 0-based.
 */
int
nvme_ctrlr_cmd_set_num_queues(struct spdk_nvme_ctrlr *ctrlr,
			      uint32_t num_queues, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	union spdk_nvme_feat_number_of_queues feat_num_queues;
                                  /* [한국어] CDW11 인코딩 union — bits.nsqr / bits.ncqr / raw 32-bit */

	feat_num_queues.raw = 0;         /* [한국어] 전체 비트 클리어 — reserved 보존 */
	feat_num_queues.bits.nsqr = num_queues - 1;
                                  /* [한국어] NSQR (0-based): 호스트가 num_queues개 SQ를 요청 — 1을 빼서 0-based 인코딩 */
	feat_num_queues.bits.ncqr = num_queues - 1;
                                  /* [한국어] NCQR (0-based): 동일 수의 CQ — SPDK는 SQ:CQ를 1:1 매핑 */

	return spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_FEAT_NUMBER_OF_QUEUES, feat_num_queues.raw,
					       0,
					       NULL, 0, cb_fn, cb_arg);
                                  /* [한국어] FID=0x07로 set_feature 호출. cdw12=0, payload 없음 */
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_get_num_queues - Set과 짝을 이루는 Get (현재 협상값 조회)
 *
 * 응답은 cqe.cdw0[15:0]/[31:16]에 실제 NSQ/NCQ (0-based).
 */
int
nvme_ctrlr_cmd_get_num_queues(struct spdk_nvme_ctrlr *ctrlr,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return spdk_nvme_ctrlr_cmd_get_feature(ctrlr, SPDK_NVME_FEAT_NUMBER_OF_QUEUES, 0, NULL, 0,
					       cb_fn, cb_arg);
                                  /* [한국어] FID=0x07 Get — cdw11=0, payload 없음. 결과는 cqe.cdw0 */
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_set_async_event_config - Set Features FID=0x0B (AEN config)
 *
 * @config: 어떤 이벤트(예: Smart/Health, Namespace Attribute Notice, Firmware
 *          Activation Notice)를 AEN(Async Event Notification)으로 받을지 비트마스크
 *
 * SPDK ctrlr init이 AER(Async Event Request) 발행 전 이 함수로 활성 이벤트
 * 비트를 설정. 이후 디바이스가 이벤트 발생 시 outstanding AER로 통지.
 */
int
nvme_ctrlr_cmd_set_async_event_config(struct spdk_nvme_ctrlr *ctrlr,
				      union spdk_nvme_feat_async_event_configuration config, spdk_nvme_cmd_cb cb_fn,
				      void *cb_arg)
{
	uint32_t cdw11;                  /* [한국어] CDW11에 들어갈 32-bit AEN 마스크 */

	cdw11 = config.raw;              /* [한국어] union의 raw 필드 — 비트필드 인코딩된 32-bit 값 */
	return spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION, cdw11, 0,
					       NULL, 0,
					       cb_fn, cb_arg);
                                  /* [한국어] FID=0x0B Set — cdw12=0, payload 없음 */
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_set_host_id - Set Features FID=0x81 (Host Identifier)
 *
 * @host_id_size: 8(64-bit) 또는 16(128-bit Extended)
 * @host_id:      호스트 식별자 바이트열 (DMA 가능 영역으로 user_copy됨)
 *
 * NVMe Spec §5.27.1.16. 동일 호스트가 여러 controller에 attach되었음을 표시
 * (멀티-경로 reservation 등에 사용). EXHID 비트(CDW11[0])로 길이 인코딩.
 */
int
nvme_ctrlr_cmd_set_host_id(struct spdk_nvme_ctrlr *ctrlr, void *host_id, uint32_t host_id_size,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	union spdk_nvme_feat_host_identifier feat_host_identifier;
                                  /* [한국어] CDW11 인코딩 union — bits.exhid + raw */

	feat_host_identifier.raw = 0;    /* [한국어] reserved 클리어 */
	if (host_id_size == 16) {
		/* 128-bit extended host identifier */
		feat_host_identifier.bits.exhid = 1;
                                  /* [한국어] EXHID=1 → 128-bit Extended Host Identifier 사용 */
	} else if (host_id_size == 8) {
		/* 64-bit host identifier */
		feat_host_identifier.bits.exhid = 0;
                                  /* [한국어] EXHID=0 → 64-bit Host Identifier (기본) */
	} else {
		NVME_CTRLR_ERRLOG(ctrlr, "Invalid host ID size %u\n", host_id_size);
                                  /* [한국어] 8/16 외 값은 NVMe 스펙에서 허용되지 않음 */
		return -EINVAL;
	}

	return spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_FEAT_HOST_IDENTIFIER,
					       feat_host_identifier.raw, 0,
					       host_id, host_id_size, cb_fn, cb_arg);
                                  /* [한국어] FID=0x81 Set — payload로 host_id 8/16바이트 송신 */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_get_log_page_ext - Get Log Page (opc=0x02, Extended)
 *
 * @log_page:    LID (CDW10[7:0]) — 0x01=Error, 0x02=SMART/Health, 0x03=Firmware Slot,
 *               0x05=Cmds Supported & Effects, 0x70=Discovery(Fabrics), 0x80=Reservation 등
 * @nsid:        CDW1 — NS별 로그(예: SMART per-NS) 필요 시
 * @payload, payload_size: 로그 데이터 수신 버퍼 (4-byte 정렬, payload_size>0 필수)
 * @offset:      LPO (Log Page Offset, 8-byte 단위 입력 받지만 dword 4-byte 단위 정렬 검증).
 *               LPOL=offset[31:0], LPOU=offset[63:32]. 디바이스가 LPEDS(Log Page Extended
 *               Data Support)를 지원해야 비-제로 offset 사용 가능.
 * @cdw10/11/14: 호출자가 일부 비트(예: RAE=Retain Asynchronous Event, LSI/LSP/UUID Index 등)를
 *               미리 인코딩하여 전달
 *
 * NVMe Spec §5.16 Get Log Page. Numeric Dwords (NUMD)는 0-based, 16-bit씩 split:
 * NUMDL=CDW10[31:16], NUMDU=CDW11[15:0]. 페이지가 4KB를 넘으면 16-bit×2로 표현.
 */
int
spdk_nvme_ctrlr_cmd_get_log_page_ext(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page,
				     uint32_t nsid, void *payload, uint32_t payload_size,
				     uint64_t offset, uint32_t cdw10,
				     uint32_t cdw11, uint32_t cdw14,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;       /* [한국어] SQE 별칭 */
	uint32_t numd, numdl, numdu;     /* [한국어] NUMD(0-based dword), 하위/상위 16-bit 분할 */
	uint32_t lpol, lpou;             /* [한국어] LPO 하위/상위 32-bit (CDW12/CDW13) */
	int rc;                          /* [한국어] submit 결과 */

	if (payload_size == 0) {
                                  /* [한국어] 로그 데이터 0바이트 요청은 무의미 — NVMe 스펙상 NUMD 0 가능하지만 SPDK는 미허용 */
		return -EINVAL;
	}

	if (offset & 3) {
                                  /* [한국어] 오프셋은 4-byte(dword) 정렬 필수 — 비정렬은 LPO 인코딩 불가 */
		return -EINVAL;
	}

	numd = spdk_nvme_bytes_to_numd(payload_size);
                                  /* [한국어] 바이트→0-based dword 카운트로 변환: (payload_size/4)-1 */
	numdl = numd & 0xFFFFu;          /* [한국어] NUMDL = NUMD 하위 16-bit (CDW10[31:16]) */
	numdu = (numd >> 16) & 0xFFFFu;  /* [한국어] NUMDU = NUMD 상위 16-bit (CDW11[15:0]) — 4KB 초과 페이지에서 사용 */

	lpol = (uint32_t)offset;         /* [한국어] LPOL = offset 하위 32-bit (CDW12) */
	lpou = (uint32_t)(offset >> 32); /* [한국어] LPOU = offset 상위 32-bit (CDW13) */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 */

	if (offset && !ctrlr->cdata.lpa.lpeds) {
                                  /* [한국어] 비-제로 offset인데 디바이스가 LPEDS(Log Page Extended Data) 미지원 → 거부 */
		nvme_ctrlr_unlock(ctrlr);
		return -EINVAL;
	}

	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, payload_size, cb_fn, cb_arg, false);
                                  /* [한국어] device→host — 로그 데이터 수신 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                 /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_GET_LOG_PAGE; /* [한국어] opcode=0x02 (Get Log Page) */
	cmd->nsid = nsid;                /* [한국어] CDW1=NSID (전 NS 평균이면 0xFFFFFFFF, 컨트롤러 글로벌이면 0) */
	cmd->cdw10 = cdw10;              /* [한국어] 호출자가 미리 인코딩한 CDW10 베이스 (RAE/LSP 등) */
	cmd->cdw10_bits.get_log_page.numdl = numdl;
                                  /* [한국어] CDW10[31:16] = NUMDL — 위 cdw10 베이스 위에 NUMDL/LID를 덮어씀 */
	cmd->cdw10_bits.get_log_page.lid = log_page;
                                  /* [한국어] CDW10[7:0] = LID */

	cmd->cdw11 = cdw11;              /* [한국어] 호출자가 미리 인코딩한 CDW11 (LSI 등) */
	cmd->cdw11_bits.get_log_page.numdu = numdu;
                                  /* [한국어] CDW11[15:0] = NUMDU */
	cmd->cdw12 = lpol;               /* [한국어] CDW12 = LPOL */
	cmd->cdw13 = lpou;               /* [한국어] CDW13 = LPOU */
	cmd->cdw14 = cdw14;              /* [한국어] CDW14 = UUID Index 등 호출자가 직접 인코딩 */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */
	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_get_log_page - Get Log Page의 단순 wrapper
 *
 * cdw10/11/14를 모두 0으로 두고 _ext 호출. 일반적인 로그(SMART, Error, Firmware
 * Slot 등)에 사용.
 */
int
spdk_nvme_ctrlr_cmd_get_log_page(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page,
				 uint32_t nsid, void *payload, uint32_t payload_size,
				 uint64_t offset, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return spdk_nvme_ctrlr_cmd_get_log_page_ext(ctrlr, log_page, nsid, payload,
			payload_size, offset, 0, 0, 0, cb_fn, cb_arg);
                                  /* [한국어] cdw10=cdw11=cdw14=0 — 추가 옵션 없음 */
}

/*
 * [한국어]
 * nvme_ctrlr_retry_queued_abort - 큐에 대기 중인 abort 요청을 재발행
 *
 * NVMe ACL(Abort Command Limit)을 초과해 잠시 큐에 보관했던 abort 명령을,
 * 기존 abort 하나가 완료된 시점에서 하나씩 다시 시도. 첫 성공 발행 후 break
 * — 한 번에 하나만 in-flight로 보내 ACL 슬롯을 초과하지 않게.
 *
 * 호출자: nvme_ctrlr_cmd_abort_cpl, nvme_complete_abort_request (abort 완료 콜백).
 */
static void
nvme_ctrlr_retry_queued_abort(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_request	*next, *tmp; /* [한국어] STAILQ 순회를 위한 커서/임시 (FOREACH_SAFE는 next를 미리 캐시) */
	int rc;                              /* [한국어] submit 결과 */

	if (ctrlr->is_resetting || ctrlr->is_destructed || ctrlr->is_failed) {
		/* Don't resubmit aborts if ctrlr is failing */
                                  /* [한국어] reset/destroy/fail 상태에서는 재발행 금지 — 컨트롤러가 더 이상 정상 admin 처리 불가 */
		return;
	}

	if (spdk_nvme_ctrlr_get_admin_qp_failure_reason(ctrlr) != SPDK_NVME_QPAIR_FAILURE_NONE) {
		/* Don't resubmit aborts if admin qpair is failed */
                                  /* [한국어] admin qpair가 fail이면 재발행 무의미 — 어차피 즉시 실패 */
		return;
	}

	STAILQ_FOREACH_SAFE(next, &ctrlr->queued_aborts, stailq, tmp) {
                                  /* [한국어] queued_aborts 큐를 안전 순회 (next를 큐에서 빼는 동안에도 안전) */
		STAILQ_REMOVE_HEAD(&ctrlr->queued_aborts, stailq);
                                  /* [한국어] 헤드를 큐에서 제거 (FOREACH_SAFE 내에서 next == head 보장) */
		ctrlr->outstanding_aborts++;
                                  /* [한국어] 발행 직전 카운트 증가 — ACL 추적 */
		rc = nvme_ctrlr_submit_admin_request(ctrlr, next);
                                  /* [한국어] admin enqueue */
		if (rc < 0) {
			NVME_CTRLR_ERRLOG(ctrlr, "Failed to submit queued abort.\n");
                                  /* [한국어] 발행 실패 — 즉시 합성 CQE로 사용자 콜백 호출 */
			memset(&next->cpl, 0, sizeof(next->cpl)); /* [한국어] CQE 클리어 후 합성 */
			next->cpl.status.sct = SPDK_NVME_SCT_GENERIC;
                                  /* [한국어] SCT=Generic Command Status (0x0) */
			next->cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
                                  /* [한국어] SC=Internal Device Error (0x06) — 호스트측 합성이지만 내부 오류로 보고 */
			next->cpl.status.dnr = 1;
                                  /* [한국어] DNR=1 — Do Not Retry (호출자가 재시도 안 하도록) */
			nvme_complete_request(next->cb_fn, next->cb_arg, next->qpair, next, &next->cpl);
                                  /* [한국어] 콜백 호출 + req 풀 반환 */
		} else {
			/* If the first abort succeeds, stop iterating. */
                                  /* [한국어] 한 번에 하나만 — ACL 슬롯 초과 방지 */
			break;
		}
	}
}

/*
 * [한국어]
 * _nvme_ctrlr_submit_abort_request - abort 요청을 ACL 한도 내에서 submit하거나 큐잉
 *
 * cdata.acl은 0-based (ACL+1이 실제 한도). outstanding_aborts가 이미 한도면
 * queued_aborts STAILQ에 보관 후 0 반환(실패가 아닌 "큐잉됨"). 한도 미만이면
 * 즉시 발행.
 */
static int
_nvme_ctrlr_submit_abort_request(struct spdk_nvme_ctrlr *ctrlr,
				 struct nvme_request *req)
{
	/* ACL is a 0's based value. */
                                  /* [한국어] ACL은 0-based — 실제 한도는 acl+1 (NVMe Spec) */
	if (ctrlr->outstanding_aborts >= ctrlr->cdata.acl + 1U) {
                                  /* [한국어] 이미 한도 도달 → 큐잉 (나중에 retry_queued_abort가 재발행) */
		STAILQ_INSERT_TAIL(&ctrlr->queued_aborts, req, stailq);
                                  /* [한국어] queued_aborts 꼬리에 추가 (FIFO) */
		return 0;            /* [한국어] 호출자 입장에서 "수락됨" — 비동기 발행 보장 */
	} else {
		ctrlr->outstanding_aborts++;
                                  /* [한국어] 한도 미만 → 즉시 발행 가능. 카운트 증가 후 submit */
		return nvme_ctrlr_submit_admin_request(ctrlr, req);
	}
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_abort_cpl - Abort 완료 콜백 (단일 cid abort용 spdk_nvme_ctrlr_cmd_abort 경로)
 *
 * 컨트롤러가 abort 완료를 통지하면 outstanding_aborts를 감소시키고,
 * queued_aborts에 대기 중인 abort가 있으면 하나 재발행. 그 후 user_cb_fn
 * 호출(원본 사용자 콜백 보존).
 */
static void
nvme_ctrlr_cmd_abort_cpl(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_request	*req = ctx;       /* [한국어] cb_arg로 전달된 자기 자신 (req->cb_arg = req) */
	struct spdk_nvme_ctrlr	*ctrlr;       /* [한국어] req로부터 ctrlr 역추적 */

	ctrlr = req->qpair->ctrlr;            /* [한국어] qpair → ctrlr 역참조 */

	assert(ctrlr->outstanding_aborts > 0);
                                  /* [한국어] 콜백 도달 = 발행되었던 명령이므로 카운트 > 0 보장 */
	ctrlr->outstanding_aborts--;          /* [한국어] 슬롯 반환 */
	nvme_ctrlr_retry_queued_abort(ctrlr); /* [한국어] 빈 슬롯에 큐된 abort 하나 재발행 */

	req->user_cb_fn(req->user_cb_arg, cpl);
                                  /* [한국어] 사용자가 spdk_nvme_ctrlr_cmd_abort 호출 시 넘긴 원본 콜백 호출 */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_abort - 단일 (sqid, cid) 명령에 대한 Abort (opc=0x08)
 *
 * @qpair: abort 대상이 발행된 qpair (NULL이면 admin qpair). 이 qpair의 id가 SQID로 들어감.
 * @cid:   abort할 명령의 Command Identifier
 *
 * NVMe Spec §5.1 Abort. Abort 자체는 admin qpair에 발행된다. 디바이스의
 * ACL 한도를 넘으면 내부 큐에 보관 후 슬롯이 비면 재발행.
 */
int
spdk_nvme_ctrlr_cmd_abort(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
			  uint16_t cid, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	int rc;                              /* [한국어] submit 결과 */
	struct nvme_request *req;            /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;           /* [한국어] SQE 별칭 */

	if (qpair == NULL) {
                                  /* [한국어] qpair 미지정 시 admin qpair를 대상으로 가정 */
		qpair = ctrlr->adminq;
	}

	nvme_ctrlr_lock(ctrlr);              /* [한국어] outstanding_aborts/queued_aborts 보호 */
	req = nvme_allocate_request_null(ctrlr->adminq, nvme_ctrlr_cmd_abort_cpl, NULL);
                                  /* [한국어] payload 없음. 콜백을 abort_cpl로 두어 ACL 슬롯 관리. cb_arg는 아래에서 자기 참조로 설정 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);        /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}
	req->cb_arg = req;                   /* [한국어] cb_arg를 req 자신으로 — abort_cpl에서 ctrlr 추출 */
	req->user_cb_fn = cb_fn;             /* [한국어] 원본 사용자 콜백 보관 */
	req->user_cb_arg = cb_arg;           /* [한국어] 원본 사용자 콜백 인자 보관 */

	cmd = &req->cmd;                     /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_ABORT;      /* [한국어] opcode=0x08 (Abort) */
	cmd->cdw10_bits.abort.sqid = qpair->id;
                                  /* [한국어] CDW10[31:16]=SQID — abort 대상 명령이 들어있는 SQ id */
	cmd->cdw10_bits.abort.cid = cid;     /* [한국어] CDW10[15:0]=CID — abort 대상 명령의 cid */

	rc = _nvme_ctrlr_submit_abort_request(ctrlr, req);
                                  /* [한국어] ACL 검사 후 즉시 submit 또는 queued_aborts에 enqueue */

	nvme_ctrlr_unlock(ctrlr);            /* [한국어] 락 해제 */
	return rc;
}

/*
 * [한국어]
 * nvme_complete_abort_request - abort_ext가 만든 child abort 완료 콜백
 *
 * spdk_nvme_ctrlr_cmd_abort_ext는 cmd_cb_arg 매칭 outstanding 명령마다 child
 * abort 하나씩 fan-out한다. 각 child가 완료되면 이 콜백이 호출되어:
 *   1) outstanding_aborts 감소 + queued retry
 *   2) parent에서 child 제거
 *   3) 한 child라도 abort 실패면 parent_status.cdw0 |= 1 (실패 표시)
 *   4) 마지막 child 완료(num_children==0) 시 parent 콜백 호출
 */
static void
nvme_complete_abort_request(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_request *req = ctx;          /* [한국어] 자기 자신 (cb_arg = req) */
	struct nvme_request *parent = req->parent; /* [한국어] fan-out parent — abort_ext가 만든 통합 요청 */
	struct spdk_nvme_ctrlr *ctrlr;           /* [한국어] ctrlr 역참조 */

	ctrlr = req->qpair->ctrlr;               /* [한국어] qpair → ctrlr */

	assert(ctrlr->outstanding_aborts > 0);   /* [한국어] 발행되었던 abort이므로 카운트 > 0 */
	ctrlr->outstanding_aborts--;             /* [한국어] 슬롯 반환 */
	nvme_ctrlr_retry_queued_abort(ctrlr);    /* [한국어] 빈 슬롯에 큐된 abort 재발행 */

	nvme_request_remove_child(parent, req);  /* [한국어] parent의 children 리스트에서 이 req 제거 + num_children-- */

	if (!spdk_nvme_cpl_is_abort_success(cpl)) {
                                  /* [한국어] 디바이스가 보고한 abort 실패(예: 대상 명령이 이미 완료된 경우 SC=Aborted Command Limit Exceeded 등) */
		parent->parent_status.cdw0 |= 1U;
                                  /* [한국어] parent의 합성 status에 "한 child 실패" 표시 (cdw0 LSB) — 사용자 콜백에서 인식 */
	}

	if (parent->num_children == 0) {
                                  /* [한국어] 모든 child가 끝났으면 parent 완료 → 사용자 콜백 호출 */
		nvme_complete_request(parent->cb_fn, parent->cb_arg, parent->qpair,
				      parent, &parent->parent_status);
	}
}

/*
 * [한국어]
 * nvme_request_add_abort - qpair iterate 콜백: cb_arg가 일치하는 outstanding 요청에 child abort 추가
 *
 * @req: iterate 중인 outstanding 요청 (qpair에 in-flight인 명령)
 * @arg: parent abort_ext request
 *
 * nvme_transport_qpair_iterate_requests가 qpair의 모든 outstanding req를 순회
 * 하며 호출. cb_arg 매칭(=같은 사용자 콜백 컨텍스트)이면 child abort req를
 * 만들어 parent 아래에 매단다.
 */
static int
nvme_request_add_abort(struct nvme_request *req, void *arg)
{
	struct nvme_request *parent = arg;       /* [한국어] abort_ext가 만든 fan-out parent */
	struct nvme_request *child;              /* [한국어] 새로 만들 child abort req */
	void *cmd_cb_arg;                        /* [한국어] 매칭에 사용할 사용자 콜백 컨텍스트 */

	cmd_cb_arg = parent->user_cb_arg;        /* [한국어] parent에 저장해둔 매칭 키 */

	if (!nvme_request_abort_match(req, cmd_cb_arg)) {
                                  /* [한국어] req의 사용자 콜백 컨텍스트가 cmd_cb_arg와 다르면 abort 대상 아님 → skip */
		return 0;
	}

	child = nvme_allocate_request_null(parent->qpair->ctrlr->adminq,
					   nvme_complete_abort_request, NULL);
                                  /* [한국어] admin qpair 풀에서 child abort req 할당. cb는 위 fan-in 처리기 */
	if (child == NULL) {
                                  /* [한국어] 풀 고갈 — iterate 중단 */
		return -ENOMEM;
	}

	child->cb_arg = child;                   /* [한국어] 콜백에서 자기 자신 참조 → req->parent로 parent 접근 */

	child->cmd.opc = SPDK_NVME_OPC_ABORT;    /* [한국어] opcode=0x08 (Abort) */
	/* Copy SQID from the parent. */
                                  /* [한국어] SQID는 parent에 미리 저장된 값(대상 qpair id) */
	child->cmd.cdw10_bits.abort.sqid = parent->cmd.cdw10_bits.abort.sqid;
                                  /* [한국어] CDW10[31:16]=SQID 복사 */
	child->cmd.cdw10_bits.abort.cid = req->cmd.cid;
                                  /* [한국어] CDW10[15:0]=CID — 매칭된 outstanding req의 cid */

	child->parent = parent;                  /* [한국어] parent 역포인터 — 완료 시 fan-in */

	TAILQ_INSERT_TAIL(&parent->children, child, child_tailq);
                                  /* [한국어] parent의 children 리스트에 추가 (TAILQ) */
	parent->num_children++;                  /* [한국어] 자식 수 증가 — 0이 되면 parent 완료 */

	return 0;                                /* [한국어] 정상 등록 — iterate 계속 */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_abort_ext - 콜백 컨텍스트 매칭 기반 multi-abort
 *
 * @qpair:        대상 qpair (NULL이면 admin)
 * @cmd_cb_arg:   매칭 키 — 같은 cb_arg로 발행된 모든 outstanding 명령에 abort 발행
 * @cb_fn/cb_arg: 모든 abort 완료 후 호출될 통합 콜백
 *
 * 패턴: parent request 1개 생성 → qpair iterate로 매칭 outstanding 각각에
 * child abort 생성/제출 → 큐에 보관된 매칭 요청은 즉시 합성 abort
 * (nvme_qpair_abort_queued_reqs_with_cbarg) → 모든 child 완료 시 parent
 * 콜백 호출. 대상이 없으면 -ENOENT.
 */
int
spdk_nvme_ctrlr_cmd_abort_ext(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
			      void *cmd_cb_arg,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	int rc = 0;                              /* [한국어] 반환 코드 */
	struct nvme_request *parent, *child, *tmp; /* [한국어] parent와 child 순회 */
	bool child_failed = false;               /* [한국어] child submit 실패가 발생했는지 추적 */
	int aborted = 0;                         /* [한국어] queued abort로 즉시 정리된 요청 수 */

	if (cmd_cb_arg == NULL) {
                                  /* [한국어] 매칭 키 NULL은 무의미 (모든 명령을 abort하는 의도가 아님) */
		return -EINVAL;
	}

	nvme_ctrlr_lock(ctrlr);                  /* [한국어] outstanding_aborts/queued_aborts 보호 */

	if (qpair == NULL) {
                                  /* [한국어] qpair 미지정 시 admin qpair 사용 */
		qpair = ctrlr->adminq;
	}

	parent = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
                                  /* [한국어] parent req 할당 — 직접 발행되지 않고 fan-out 통합 콜백 컨테이너 역할 */
	if (parent == NULL) {
		nvme_ctrlr_unlock(ctrlr);

		return -ENOMEM;
	}

	TAILQ_INIT(&parent->children);           /* [한국어] children 리스트 초기화 */
	parent->num_children = 0;                /* [한국어] 자식 수 0 (add_abort에서 증가) */

	parent->cmd.opc = SPDK_NVME_OPC_ABORT;   /* [한국어] opcode 표시 (parent는 직접 발행 안 됨) */
	memset(&parent->parent_status, 0, sizeof(struct spdk_nvme_cpl));
                                  /* [한국어] 합성 CQE를 0으로 초기화 — child 실패 시 cdw0 LSB로 표시 */

	/* Hold SQID that the requests to abort are associated with.
	 * This will be copied to the children.
	 *
	 * CID is not set here because the parent is not submitted directly
	 * and CID is not determined until request to abort is found.
	 */
                                  /* [한국어] SQID는 parent에 보관 (대상 qpair id) — child 생성 시 복사. CID는 매칭된 req에서 추출 */
	parent->cmd.cdw10_bits.abort.sqid = qpair->id;

	/* This is used to find request to abort. */
                                  /* [한국어] user_cb_arg를 매칭 키로 재활용 — add_abort에서 nvme_request_abort_match가 비교 */
	parent->user_cb_arg = cmd_cb_arg;

	/* Add an abort request for each outstanding request which has cmd_cb_arg
	 * as its callback context.
	 */
                                  /* [한국어] qpair의 모든 in-flight req를 순회하며 매칭되는 것마다 child abort 생성 */
	rc = nvme_transport_qpair_iterate_requests(qpair, nvme_request_add_abort, parent);
	if (rc != 0) {
		/* Free abort requests already added. */
                                  /* [한국어] iterate 중 ENOMEM 등 실패 — 이미 추가된 child들을 롤백 */
		child_failed = true;
	}

	TAILQ_FOREACH_SAFE(child, &parent->children, child_tailq, tmp) {
                                  /* [한국어] children 리스트 안전 순회 (제거 가능) */
		if (spdk_likely(!child_failed)) {
			rc = _nvme_ctrlr_submit_abort_request(ctrlr, child);
                                  /* [한국어] 정상 경로: 각 child를 ACL 검사 후 submit/큐잉 */
			if (spdk_unlikely(rc != 0)) {
				child_failed = true;
                                  /* [한국어] 한 child라도 실패하면 이후 child들은 폐기 모드로 */
			}
		} else {
			/* Free remaining abort requests. */
                                  /* [한국어] 폐기 모드: 남은 child를 parent에서 제거 후 풀에 반환 */
			nvme_request_remove_child(parent, child);
			nvme_free_request(child);
		}
	}

	if (spdk_likely(!child_failed)) {
		/* There is no error so far. Abort requests were submitted successfully
		 * or there was no outstanding request to abort.
		 *
		 * Hence abort queued requests which has cmd_cb_arg as its callback
		 * context next.
		 */
                                  /* [한국어] in-flight 처리 후, qpair에 큐잉된(아직 submit 안된) 요청 중 매칭되는 것을 즉시 합성 abort */
		aborted = nvme_qpair_abort_queued_reqs_with_cbarg(qpair, cmd_cb_arg);
		if (parent->num_children == 0) {
			/* There was no outstanding request to abort. */
                                  /* [한국어] in-flight 매칭이 0건이었음 */
			if (aborted > 0) {
				/* The queued requests were successfully aborted. Hence
				 * complete the parent request with success synchronously.
				 */
                                  /* [한국어] 큐된 것만 abort됨 → parent를 즉시 성공 완료(동기) */
				nvme_complete_request(parent->cb_fn, parent->cb_arg, parent->qpair,
						      parent, &parent->parent_status);
			} else {
				/* There was no queued request to abort. */
                                  /* [한국어] 매칭 대상이 전혀 없음 → -ENOENT */
				rc = -ENOENT;
			}
		}
	} else {
		/* Failed to add or submit abort request. */
                                  /* [한국어] 일부 child가 실패한 경우 */
		if (parent->num_children != 0) {
			/* Return success since we must wait for those children
			 * to complete but set the parent request to failure.
			 */
                                  /* [한국어] 살아남은 child의 완료를 기다려야 하므로 동기 실패 반환 불가 → status에 실패 표시 후 비동기 완료 위임 */
			parent->parent_status.cdw0 |= 1U;
			rc = 0;
		}
	}

	if (rc != 0) {
                                  /* [한국어] 동기 실패 경로(parent 콜백 호출 안 됨) → parent 풀 반환 */
		nvme_free_request(parent);
	}

	nvme_ctrlr_unlock(ctrlr);                /* [한국어] 락 해제 */
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_fw_commit - Firmware Commit (opc=0x10, NVMe Spec §5.13)
 *
 * @fw_commit: struct spdk_nvme_fw_commit — FS(Firmware Slot)/CA(Commit Action)/BPID 등
 *             32-bit를 CDW10에 그대로 매핑.
 *
 * 호스트가 download해둔 펌웨어 이미지를 슬롯에 활성화/저장. CA에 따라
 * Replace/Activate Now/Replace+Activate Reset/Activate-on-reset 등 수행.
 */
int
nvme_ctrlr_cmd_fw_commit(struct spdk_nvme_ctrlr *ctrlr,
			 const struct spdk_nvme_fw_commit *fw_commit,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;       /* [한국어] SQE 별칭 */
	int rc;                          /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
                                  /* [한국어] payload 없음 → null 할당 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                 /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_FIRMWARE_COMMIT; /* [한국어] opcode=0x10 (Firmware Commit) */
	memcpy(&cmd->cdw10, fw_commit, sizeof(uint32_t));
                                  /* [한국어] CDW10에 fw_commit 비트필드(FS[2:0], CA[5:3], BPID[31]) 그대로 복사 */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */
	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */

	return rc;

}

/*
 * [한국어]
 * nvme_ctrlr_cmd_fw_image_download - Firmware Image Download (opc=0x11)
 *
 * @size:    이번 청크 길이(바이트, 4-byte 정렬)
 * @offset:  펌웨어 전체에서 이번 청크의 바이트 오프셋(4-byte 정렬)
 * @payload: 청크 데이터
 *
 * 큰 펌웨어를 여러 청크로 분할 다운로드 후 Firmware Commit으로 활성화.
 * NUMD/OFFSET은 0-based dword 단위 인코딩.
 */
int
nvme_ctrlr_cmd_fw_image_download(struct spdk_nvme_ctrlr *ctrlr,
				 uint32_t size, uint32_t offset, void *payload,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;       /* [한국어] SQE 별칭 */
	int rc;                          /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, size, cb_fn, cb_arg, true);
                                  /* [한국어] host→device — 펌웨어 청크 송신 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                 /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD; /* [한국어] opcode=0x11 (Firmware Image Download) */
	cmd->cdw10 = spdk_nvme_bytes_to_numd(size);
                                  /* [한국어] CDW10 = NUMD (0-based dword 카운트, (size/4)-1) */
	cmd->cdw11 = offset >> 2;        /* [한국어] CDW11 = OFFSET in dword units — 바이트 오프셋의 dword(>>2) */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */
	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_security_receive - Security Receive (opc=0x82, NVMe Spec §5.25)
 *
 * @secp: Security Protocol (예: TCG=0x01, IEEE 1667=0xEE)
 * @spsp: SP Specific (16-bit; ATA TRUSTED 등에서 의미 정의)
 * @nssf: NVMe Security Specific Field (CDW10[7:0])
 *
 * TCG Opal 같은 보안 프로토콜의 receive 단계. 디바이스→호스트(false).
 * spsp는 8-bit 두 개로 나눠 인코딩.
 */
int
spdk_nvme_ctrlr_cmd_security_receive(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
				     uint16_t spsp, uint8_t nssf, void *payload,
				     uint32_t payload_size, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;       /* [한국어] SQE 별칭 */
	int rc;                          /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size,
					      cb_fn, cb_arg, false);
                                  /* [한국어] device→host — 보안 응답 수신 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                 /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_SECURITY_RECEIVE; /* [한국어] opcode=0x82 (Security Receive) */
	cmd->cdw10_bits.sec_send_recv.nssf = nssf; /* [한국어] CDW10[7:0] = NSSF (NVMe Specific) */
	cmd->cdw10_bits.sec_send_recv.spsp0 = (uint8_t)spsp;       /* [한국어] CDW10[15:8] = SPSP0 (SP Specific 하위) */
	cmd->cdw10_bits.sec_send_recv.spsp1 = (uint8_t)(spsp >> 8); /* [한국어] CDW10[23:16] = SPSP1 (SP Specific 상위) */
	cmd->cdw10_bits.sec_send_recv.secp = secp;                 /* [한국어] CDW10[31:24] = SECP (Security Protocol) */
	cmd->cdw11 = payload_size;       /* [한국어] CDW11 = Allocation Length (수신 가능 최대 바이트, 디바이스가 채울 양 상한) */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */
	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_security_send - Security Send (opc=0x81, NVMe Spec §5.26)
 *
 * Security Receive와 짝. host→device(true). payload_size는 Transfer Length.
 */
int
spdk_nvme_ctrlr_cmd_security_send(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
				  uint16_t spsp, uint8_t nssf, void *payload,
				  uint32_t payload_size, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;       /* [한국어] SQE 별칭 */
	int rc;                          /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size,
					      cb_fn, cb_arg, true);
                                  /* [한국어] host→device — 보안 페이로드 송신 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                 /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_SECURITY_SEND; /* [한국어] opcode=0x81 (Security Send) */
	cmd->cdw10_bits.sec_send_recv.nssf = nssf; /* [한국어] CDW10[7:0] = NSSF */
	cmd->cdw10_bits.sec_send_recv.spsp0 = (uint8_t)spsp;       /* [한국어] CDW10[15:8] = SPSP0 */
	cmd->cdw10_bits.sec_send_recv.spsp1 = (uint8_t)(spsp >> 8); /* [한국어] CDW10[23:16] = SPSP1 */
	cmd->cdw10_bits.sec_send_recv.secp = secp;                 /* [한국어] CDW10[31:24] = SECP */
	cmd->cdw11 = payload_size;       /* [한국어] CDW11 = Transfer Length (송신 바이트) */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */
	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */

	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_sanitize - Sanitize (opc=0x84, NVMe Spec §5.24)
 *
 * @nsid:     보통 0xFFFFFFFF (Sanitize는 컨트롤러 전체 미디어 대상)
 * @sanitize: SANACT(Sanitize Action: Block Erase/Overwrite/Crypto Erase 등)/AUSE/OWPASS 등
 *            32-bit 비트필드를 CDW10에 매핑
 * @cdw11:    Overwrite Pattern 등 호출자가 직접 인코딩
 *
 * Sanitize는 디바이스의 모든 사용자 데이터를 영구 삭제. 진행상황은 Sanitize
 * Status Log(LID=0x81)로 폴링.
 */
int
nvme_ctrlr_cmd_sanitize(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			struct spdk_nvme_sanitize *sanitize, uint32_t cdw11,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;        /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd;       /* [한국어] SQE 별칭 */
	int rc;                          /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
                                  /* [한국어] payload 없음 → null 할당 */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}

	cmd = &req->cmd;                 /* [한국어] SQE 별칭 */
	cmd->opc = SPDK_NVME_OPC_SANITIZE; /* [한국어] opcode=0x84 (Sanitize) */
	cmd->nsid = nsid;                /* [한국어] CDW1=NSID (보통 broadcast) */
	cmd->cdw11 = cdw11;              /* [한국어] CDW11 = Overwrite Pattern 등 옵션 */
	memcpy(&cmd->cdw10, sanitize, sizeof(cmd->cdw10));
                                  /* [한국어] CDW10 = sanitize 비트필드(SANACT[2:0], AUSE[3], OWPASS[7:4], OIPBP[8], NDAS[9]) */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */
	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */

	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_cmd_directive - Directive Send/Receive 공통 빌더 (opc=0x19/0x1A)
 *
 * @doper: Directive Operation (CDW11[7:0]) — 예: Identify, Return Parameters, Enable
 * @dtype: Directive Type      (CDW11[15:8]) — 0x00=Identify, 0x01=Streams 등
 * @dspec: Directive Specific  (CDW11[31:16]) — Stream ID 등
 * @cdw12/cdw13: directive-specific 추가 필드
 * @opc_type: SPDK_NVME_OPC_DIRECTIVE_SEND(0x19) 또는 _RECEIVE(0x1A)
 * @host_to_ctrlr: payload 방향 (Send=true, Receive=false)
 *
 * NVMe Spec §5.10/5.11 Directive. CDW10 = NUMD-1(payload dword 수 0-based; 0이면 0).
 * Streams Directive 등에서 사용.
 */
static int
nvme_ctrlr_cmd_directive(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			 uint32_t doper, uint32_t dtype, uint32_t dspec,
			 void *payload, uint32_t payload_size, uint32_t cdw12,
			 uint32_t cdw13, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			 uint16_t opc_type, bool host_to_ctrlr)
{
	struct nvme_request *req = NULL; /* [한국어] 요청 객체 */
	struct spdk_nvme_cmd *cmd = NULL;/* [한국어] SQE 별칭 */
	int rc;                          /* [한국어] submit 결과 */

	nvme_ctrlr_lock(ctrlr);          /* [한국어] admin 직렬화 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size,
					      cb_fn, cb_arg, host_to_ctrlr);
                                  /* [한국어] 방향에 따라 host→device(Send) 또는 device→host(Receive) */
	if (req == NULL) {
		nvme_ctrlr_unlock(ctrlr);    /* [한국어] 풀 고갈 */
		return -ENOMEM;
	}
	cmd = &req->cmd;                 /* [한국어] SQE 별칭 */
	cmd->opc = opc_type;             /* [한국어] opcode = Directive Send(0x19) 또는 Receive(0x1A) */
	cmd->nsid = nsid;                /* [한국어] CDW1=NSID — 일부 directive는 NS context 필요 */

	if ((payload_size >> 2) > 0) {
                                  /* [한국어] 페이로드가 있으면 NUMD-1을 CDW10에 인코딩 (0-based dword 카운트) */
		cmd->cdw10 = (payload_size >> 2) - 1;
                                  /* [한국어] CDW10 = (payload_size/4) - 1 — payload가 4바이트 미만이면 0(스킵) */
	}
	cmd->cdw11_bits.directive.doper = doper; /* [한국어] CDW11[7:0] = Directive Operation */
	cmd->cdw11_bits.directive.dtype = dtype; /* [한국어] CDW11[15:8] = Directive Type */
	cmd->cdw11_bits.directive.dspec = dspec; /* [한국어] CDW11[31:16] = Directive Specific (Stream ID 등) */
	cmd->cdw12 = cdw12;              /* [한국어] CDW12 = directive별 추가 인자 */
	cmd->cdw13 = cdw13;              /* [한국어] CDW13 = directive별 추가 인자 */
	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
                                  /* [한국어] admin enqueue */
	nvme_ctrlr_unlock(ctrlr);        /* [한국어] 락 해제 */

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_directive_send - Directive Send (opc=0x19) wrapper
 *
 * 방향: host→device (true). 예: Streams Enable.
 */
int
spdk_nvme_ctrlr_cmd_directive_send(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				   uint32_t doper, uint32_t dtype, uint32_t dspec,
				   void *payload, uint32_t payload_size, uint32_t cdw12,
				   uint32_t cdw13, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_ctrlr_cmd_directive(ctrlr, nsid, doper, dtype, dspec,
					payload, payload_size, cdw12, cdw13, cb_fn, cb_arg,
					SPDK_NVME_OPC_DIRECTIVE_SEND, true);
                                  /* [한국어] opc=0x19, host→device(true) */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_cmd_directive_receive - Directive Receive (opc=0x1A) wrapper
 *
 * 방향: device→host (false). 예: Streams Return Parameters/Identify.
 */
int
spdk_nvme_ctrlr_cmd_directive_receive(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				      uint32_t doper, uint32_t dtype, uint32_t dspec,
				      void *payload, uint32_t payload_size, uint32_t cdw12,
				      uint32_t cdw13, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_ctrlr_cmd_directive(ctrlr, nsid, doper, dtype, dspec,
					payload, payload_size, cdw12, cdw13, cb_fn, cb_arg,
					SPDK_NVME_OPC_DIRECTIVE_RECEIVE, false);
                                  /* [한국어] opc=0x1A, device→host(false) */
}
