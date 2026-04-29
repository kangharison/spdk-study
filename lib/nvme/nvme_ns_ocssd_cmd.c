/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] OCSSD(Open-Channel SSD) Namespace I/O 커맨드 빌더
 *               (nvme_ns_ocssd_cmd.c) — 205 라인
 *
 * === 파일의 역할 ===
 * OCSSD 2.0 스펙이 정의한 **벡터형(Vector) I/O 명령**을 생성·제출하는 빌더.
 * 표준 NVMe namespace 명령(read/write/dsm)이 단일 연속 LBA 범위(SLBA + NLB)를
 * 다루는 반면, OCSSD는 호스트가 NAND의 채널/LUN/Plane/Block/Page 위치를 직접
 * 관리하므로 LBA가 임의의 PPA(Physical Page Address) 리스트로 흩어진다.
 * 이 파일은 그 LBA 리스트(num_lbas <= SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES = 64)를
 * 받아 OCSSD 전용 opcode(VECTOR_RESET / VECTOR_WRITE / VECTOR_READ /
 * VECTOR_COPY)로 SQE를 채우고 qpair에 submit한다. 즉 nvme_ns_cmd.c의
 * "Open-Channel SSD 변형" 빌더에 해당.
 *
 * 다루는 OCSSD opcode (spdk/nvme_ocssd_spec.h):
 *   - SPDK_OCSSD_OPC_VECTOR_RESET = 0x90 — Chunk Reset (vector erase)
 *   - SPDK_OCSSD_OPC_VECTOR_WRITE = 0x91 — Vector Write
 *   - SPDK_OCSSD_OPC_VECTOR_READ  = 0x92 — Vector Read
 *   - SPDK_OCSSD_OPC_VECTOR_COPY  = 0x93 — Vector Copy (in-device read+write)
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe 드라이버 사용자 공개 API의 OCSSD 확장. 일반 namespace 빌더
 * (nvme_ns_cmd.c)와 동일한 위치에 자리하지만 호출자는 "OCSSD를 인식하는"
 * 애플리케이션(예: pblk-like FTL, 학술 OCSSD 연구 코드)에 한정된다.
 *
 * 호출 흐름 (vector write 예):
 *   [user] spdk_nvme_ocssd_ns_cmd_vector_write_with_md(ns, qpair, buf, md,
 *                                                      lba_list, n, cb, ctx, flags)
 *     → _nvme_ocssd_ns_cmd_vector_rw_with_md(opc=VECTOR_WRITE, ...) [이 파일]
 *       → 파라미터 검증 (buffer/lba_list 비-NULL, n in [1,64], io_flags 마스크)
 *       → nvme_allocate_request(qpair, payload, n*sector, n*md, cb, ctx)
 *           — qpair 로컬 nvme_request 풀에서 pop, payload는 CONTIG 모드
 *       → SQE 채움: opc=VECTOR_WRITE, nsid=ns->id, CDW10/11=LBA list addr,
 *                   CDW12 = (n-1) | io_flags
 *       → nvme_qpair_submit_request(qpair, req)
 *         → 트랜스포트 submit → SQ tail 기록 → doorbell ring → OCSSD device
 *
 * 실행 컨텍스트: 호출 스레드(= qpair를 생성·소유한 SPDK thread). qpair는
 * 단일 스레드에만 묶이므로 이 파일의 함수들은 자연스럽게 락 없이 동작한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(callee):
 *   - spdk/nvme_ocssd.h — 공개 OCSSD API 시그니처와 SPDK_OCSSD_OPC_* opcode,
 *     SPDK_OCSSD_IO_FLAGS_LIMITED_RETRY 등 io_flags 비트 정의
 *   - nvme_internal.h — nvme_request, spdk_nvme_qpair, nvme_payload,
 *     NVME_PAYLOAD_CONTIG 매크로
 *   - nvme.c — nvme_allocate_request_null / nvme_allocate_request
 *   - nvme_qpair.c — nvme_qpair_submit_request
 *   - env (DPDK 추상화) — spdk_vtophys: VA→PA 변환 (DMA 주소 획득)
 * 의존받음(caller): OCSSD 인지 사용자 애플리케이션이 직접 호출. SPDK 내부의
 *   bdev_nvme 모듈은 OCSSD 벡터 I/O를 직접 사용하지 않으므로, 이 파일은
 *   상위 layer 없이 사용자 → 드라이버 직결 형태.
 *
 * 데이터 흐름:
 *   사용자 데이터 버퍼(buffer/metadata) ─┐
 *   사용자 LBA 리스트(lba_list 배열) ──┐  │
 *                                      ▼  ▼
 *   nvme_request (req->cmd = SQE)  payload(CONTIG)
 *      cmd->opc       = VECTOR_*       ─┐
 *      cmd->nsid      = ns->id          │ submit 시 트랜스포트가
 *      cmd->cdw10/11  = LBA list 물리주소 │ payload를 PRP/SGL로 매핑
 *      cmd->cdw12     = (n-1) | flags  ─┘
 *      cmd->mptr      = chunk_info PA (reset 전용 — 완료 시 chunk 상태 기록)
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_nvme_ocssd_ns_cmd_vector_reset(): chunk 단위 erase. 완료 시
 *     chunk_info(있으면)에 erase 결과(상태/wear 카운터)가 채워진다.
 *   - _nvme_ocssd_ns_cmd_vector_rw_with_md(): static 공통 헬퍼. read/write +
 *     선택적 metadata. cdw10/11=LBA-list-PA, cdw12=(n-1)|flags 채움.
 *   - spdk_nvme_ocssd_ns_cmd_vector_write[_with_md](): 위 헬퍼의 write 래퍼.
 *   - spdk_nvme_ocssd_ns_cmd_vector_read[_with_md]():  위 헬퍼의 read 래퍼.
 *   - spdk_nvme_ocssd_ns_cmd_vector_copy(): 디바이스 내부에서 src LBA 리스트를
 *     읽어 dst LBA 리스트로 기록. cdw10/11=src, cdw14/15=dst.
 *
 * 공통 SQE 인코딩 규약 (OCSSD 2.0):
 *   - num_lbas == 1: cdw10/11에 LBA 자체를 직접 인코딩 (배열 DMA 생략 최적화)
 *   - num_lbas  > 1: cdw10/11에 LBA 배열의 물리 주소(PA), 개수는 cdw12에
 *                    "0-based"로 기록 (cdw12 = num_lbas - 1)
 *   - io_flags: 현재는 LIMITED_RETRY(0x8000_0000) 1비트만 허용
 */

#include "spdk/nvme_ocssd.h"
/* [한국어] OCSSD 공개 API: opcode 상수, IO flags, max LBAL 엔트리, chunk_info 등.
 *         이 파일이 구현하는 spdk_nvme_ocssd_ns_cmd_* 시그니처도 여기서 선언된다. */
#include "nvme_internal.h"
/* [한국어] SPDK NVMe 드라이버 내부 헤더: nvme_request, spdk_nvme_qpair,
 *         nvme_payload, NVME_PAYLOAD_CONTIG, nvme_allocate_request*,
 *         nvme_qpair_submit_request 프로토타입을 제공. */

/*
 * [한국어]
 * spdk_nvme_ocssd_ns_cmd_vector_reset - Vector chunk reset(erase) 명령 발행
 *
 * @ns:        OCSSD namespace 객체 (nsid 추출용; ns->id가 SQE의 NSID 필드로 들어감)
 * @qpair:     명령을 제출할 NVMe Queue Pair (SQ+CQ 한 쌍). 호출 스레드가 소유.
 * @lba_list:  reset 대상 chunk들의 LBA 배열. num_lbas==1이면 값 자체를,
 *             num_lbas>1이면 배열의 가상 주소(spdk_vtophys로 물리화)를 SQE에 인코딩.
 *             각 LBA는 chunk의 첫 번째 LBA(start LBA of chunk)이다.
 * @num_lbas:  배열 길이. [1, SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES(=64)] 범위.
 * @chunk_info: NULL 또는 erase 완료 후 컨트롤러가 결과(상태/wear)를 기록할 영역.
 *             NULL이 아니면 mptr(metadata pointer DW)에 PA를 넣어 DMA 수신 활성화.
 * @cb_fn/@cb_arg: 비동기 완료 콜백. SPDK 표준 cb_fn(ctx, &cpl) 시그니처.
 *                qpair 소유 스레드의 process_completions에서 호출됨.
 * @return: 0 성공, -EINVAL 파라미터 오류, -ENOMEM request 풀 고갈.
 *
 * OCSSD에서 chunk(=ZNS의 zone과 유사한 개념)는 새로 쓰기 전에 반드시 erase 되어야
 * 한다. NAND erase는 시간이 오래 걸리므로 호스트는 여러 chunk를 한 번의 vector
 * 명령으로 묶어 병렬 erase를 요청한다. 이 함수는 그 vector reset SQE를 빌드한다.
 *
 * 동작 단계:
 *   1) 파라미터 검증 (lba_list 비-NULL, num_lbas 범위)
 *   2) "데이터 페이로드 없음" 요청 할당 (nvme_allocate_request_null) — erase는 PRP/SGL 불필요
 *   3) opc/nsid 채움 + chunk_info DMA 주소 설정
 *   4) num_lbas에 따라 LBA를 cdw10/11에 직접 또는 PA로 인코딩
 *   5) cdw12에 (num_lbas - 1) — OCSSD spec은 0-based 카운트
 *   6) qpair에 submit (트랜스포트가 SQ tail 기록 + doorbell ring)
 *
 * 실행 컨텍스트: qpair 소유 스레드. 락 불필요(thread-local qpair).
 *
 * 호출 체인:
 *   user app → spdk_nvme_ocssd_ns_cmd_vector_reset → nvme_allocate_request_null
 *           → nvme_qpair_submit_request → 트랜스포트 submit → device
 */
int
spdk_nvme_ocssd_ns_cmd_vector_reset(struct spdk_nvme_ns *ns,
				    struct spdk_nvme_qpair *qpair,
				    uint64_t *lba_list, uint32_t num_lbas,
				    struct spdk_ocssd_chunk_information_entry *chunk_info,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	/* [한국어] qpair 풀에서 할당받을 nvme_request 포인터.
	 *         req->cmd가 실제 SQE이고 req->cb_fn/cb_arg가 완료 시 호출된다. */
	struct spdk_nvme_cmd	*cmd;
	/* [한국어] req->cmd로의 단축 포인터. SQE 64B 구조 (NVMe 1.x §4.2). */

	if (!lba_list || (num_lbas == 0) ||
	    (num_lbas > SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES)) {
		/* [한국어] 가드: lba_list가 NULL이거나 개수가 0 또는 64 초과면 즉시 거부.
		 *         OCSSD 스펙상 LBAL(LBA List)은 한 번에 최대 64개 PPA만 담을 수 있다. */
		return -EINVAL;
	}

	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
	/* [한국어] payload가 없는(data DPTR 미사용) 요청 할당. erase는 호스트→장치
	 *         데이터 전송이 없으므로 NULL payload가 적합. 풀 고갈 시 NULL. */
	if (req == NULL) {
		/* [한국어] 풀 고갈 — 호출자는 process_completions로 in-flight를 비운 뒤
		 *         재시도해야 한다. SPDK는 동적 할당을 피하기 위해 풀 모델을 쓴다. */
		return -ENOMEM;
	}

	cmd = &req->cmd;
	/* [한국어] SQE 빌드 시작: req->cmd를 직접 채운다. */
	cmd->opc = SPDK_OCSSD_OPC_VECTOR_RESET;
	/* [한국어] OCSSD opcode 0x90 (Vector Chunk Reset). NAND erase 트리거. */
	cmd->nsid = ns->id;
	/* [한국어] 대상 namespace ID. OCSSD에서도 namespace 단위로 chunk 공간이 분리. */

	if (chunk_info != NULL) {
		/* [한국어] 호출자가 erase 결과(chunk 상태/wear 카운터)를 받기를 원하면
		 *         metadata pointer를 통해 컨트롤러가 DMA로 채울 위치를 알려준다. */
		cmd->mptr = spdk_vtophys(chunk_info, NULL);
		/* [한국어] DPDK 헬퍼로 가상→물리 주소 변환. hugepage에 정주(pinned)된
		 *         메모리여야 안전하다 (장치 DMA 대상이므로). */
	}

	/*
	 * Dword 10 and 11 store a pointer to the list of logical block addresses.
	 * If there is a single entry in the LBA list, the logical block
	 * address should be stored instead.
	 */
	if (num_lbas == 1) {
		/* [한국어] 단일 LBA 최적화: 별도 DMA 없이 LBA 값 자체를 cdw10/11에 인라인. */
		*(uint64_t *)&cmd->cdw10 = *lba_list;
		/* [한국어] 64-bit LBA를 두 32-bit DW에 little-endian 그대로 복사. */
	} else {
		/* [한국어] 복수 LBA: 배열의 물리 주소를 cdw10/11에 기록. 컨트롤러가
		 *         이 주소에서 num_lbas개의 LBA를 DMA 읽기로 가져간다. */
		*(uint64_t *)&cmd->cdw10 = spdk_vtophys(lba_list, NULL);
		/* [한국어] lba_list 또한 hugepage(DMA-able) 상에 있어야 한다. */
	}

	cmd->cdw12 = num_lbas - 1;
	/* [한국어] NLB(Number of Logical Blocks)는 0-based — 즉 N개를 보내려면 N-1.
	 *         OCSSD spec과 NVMe 표준이 공통으로 사용하는 인코딩. */

	return nvme_qpair_submit_request(qpair, req);
	/* [한국어] qpair에 제출. 내부에서 트랜스포트 vtable의 qpair_submit_request를
	 *         호출 → SQ tail 갱신 → doorbell MMIO write → 장치가 명령 fetch.
	 *         실패 시 음수 errno; 성공 시 0(완료는 추후 콜백). */
}

/*
 * [한국어]
 * _nvme_ocssd_ns_cmd_vector_rw_with_md - vector read/write 공통 헬퍼 (static)
 *
 * @ns:        OCSSD namespace (sector_size/md_size로 페이로드 크기 산출)
 * @qpair:     submit 대상 qpair (스레드-로컬)
 * @buffer:    데이터 버퍼 — read는 컨트롤러가 채움, write는 호스트가 보내는 데이터
 * @metadata:  메타데이터 버퍼(NULL 가능). NULL이면 _with_md 없는 래퍼와 동일 동작.
 * @lba_list:  PPA 배열 (각 항목은 물리 페이지의 시작 LBA)
 * @num_lbas:  배열 길이. 총 전송 크기 = num_lbas * ns->sector_size [+ md_size]
 * @cb_fn/@cb_arg: 비동기 완료 콜백
 * @opc:       SPDK_OCSSD_OPC_VECTOR_READ(0x92) 또는 _VECTOR_WRITE(0x91)
 * @io_flags:  현재는 SPDK_OCSSD_IO_FLAGS_LIMITED_RETRY 비트만 허용
 * @return: 0 성공, -EINVAL 잘못된 flag/파라미터, -ENOMEM 풀 고갈
 *
 * 4개의 공개 래퍼(write/write_with_md/read/read_with_md)가 호출하는 공통 백엔드.
 * 동일한 SQE 빌드 절차를 한 곳에 모아두고 opc만 바꿔 재사용한다.
 *
 * payload는 CONTIG 모드 — buffer/metadata가 가상 메모리 상 연속이라고 가정.
 * 트랜스포트가 제출 시점에 buffer를 PRP/SGL로 매핑한다 (PCIe 트랜스포트의 경우).
 *
 * 실행 컨텍스트: qpair 소유 스레드. 락 불필요.
 *
 * 호출 체인:
 *   spdk_nvme_ocssd_ns_cmd_vector_{read,write}[_with_md]
 *     → _nvme_ocssd_ns_cmd_vector_rw_with_md (이 함수)
 *       → nvme_allocate_request → nvme_qpair_submit_request
 */
static int
_nvme_ocssd_ns_cmd_vector_rw_with_md(struct spdk_nvme_ns *ns,
				     struct spdk_nvme_qpair *qpair,
				     void *buffer, void *metadata,
				     uint64_t *lba_list, uint32_t num_lbas,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				     enum spdk_ocssd_io_opcode opc,
				     uint32_t io_flags)
{
	struct nvme_request	*req;
	/* [한국어] qpair 풀에서 할당받을 요청 객체. */
	struct spdk_nvme_cmd	*cmd;
	/* [한국어] req->cmd 단축 포인터 (NVMe 64B SQE). */
	struct nvme_payload	payload;
	/* [한국어] payload 디스크립터 (CONTIG/SGL 등 모드 + base 주소).
	 *         트랜스포트가 이를 보고 PRP/SGL을 만든다. */
	uint32_t valid_flags = SPDK_OCSSD_IO_FLAGS_LIMITED_RETRY;
	/* [한국어] 허용 io_flags 마스크. 현재는 LIMITED_RETRY 한 비트만 정의됨. */

	if (io_flags & ~valid_flags) {
		/* [한국어] 정의되지 않은 비트가 켜져 있으면 즉시 거부 (forward-compat 보호). */
		return -EINVAL;
	}

	if (!buffer || !lba_list || (num_lbas == 0) ||
	    (num_lbas > SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES)) {
		/* [한국어] 데이터/LBA 배열 NULL 금지, num_lbas는 [1, 64] 범위.
		 *         OCSSD LBAL 한도(64)는 spec이 정한 SQE에 인코딩 가능한 최대치. */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_CONTIG(buffer, metadata);
	/* [한국어] CONTIG payload 매크로: payload.type=CONTIG, .u.contig.base=buffer,
	 *         .md=metadata. 트랜스포트는 이를 바탕으로 PRP1/PRP2 또는 PRP list를 채운다. */

	req = nvme_allocate_request(qpair, &payload, num_lbas * ns->sector_size, num_lbas * ns->md_size,
				    cb_fn, cb_arg);
	/* [한국어] 일반 요청 할당: 데이터 길이 = N * sector_size, 메타 길이 = N * md_size.
	 *         num_lbas는 곧 "PPA 개수 = 페이지 개수"이므로 총 바이트가 정확히 산출된다. */
	if (req == NULL) {
		/* [한국어] 풀 고갈 — 호출자가 process_completions 후 재시도해야 함. */
		return -ENOMEM;
	}

	cmd = &req->cmd;
	/* [한국어] SQE 빌드 시작. */
	cmd->opc = opc;
	/* [한국어] 호출자가 지정한 OCSSD opcode (VECTOR_READ 또는 VECTOR_WRITE). */
	cmd->nsid = ns->id;
	/* [한국어] 대상 namespace. */

	/*
	 * Dword 10 and 11 store a pointer to the list of logical block addresses.
	 * If there is a single entry in the LBA list, the logical block
	 * address should be stored instead.
	 */
	if (num_lbas == 1) {
		/* [한국어] 단일 LBA 최적화 경로: DMA 없이 LBA 값 자체를 인라인. */
		*(uint64_t *)&cmd->cdw10 = *lba_list;
	} else {
		/* [한국어] 다중 LBA: 배열 물리 주소를 인코딩 → 컨트롤러가 DMA로 읽음. */
		*(uint64_t *)&cmd->cdw10 = spdk_vtophys(lba_list, NULL);
	}

	cmd->cdw12 = num_lbas - 1;
	/* [한국어] 0-based 카운트 (NLB). */
	cmd->cdw12 |= io_flags;
	/* [한국어] io_flags는 cdw12 상위 비트(예: LIMITED_RETRY=bit 31)에 OR 결합.
	 *         NLB 필드(하위 16비트)와 충돌하지 않는 상위 비트를 사용. */

	return nvme_qpair_submit_request(qpair, req);
	/* [한국어] qpair로 submit. 트랜스포트가 SQ tail 기록 + doorbell ring 수행. */
}

/*
 * [한국어]
 * spdk_nvme_ocssd_ns_cmd_vector_write_with_md - vector write + 메타데이터 공개 API
 *
 * 사용자가 OCSSD 디바이스의 지정된 PPA들에 데이터+메타데이터를 동시에 기록할 때 사용.
 * NAND의 OOB(out-of-band) 영역에 저장될 메타(예: PI, FTL 매핑 태그)를 함께 보낸다.
 * 인자 그대로 _nvme_ocssd_ns_cmd_vector_rw_with_md(opc=VECTOR_WRITE)를 호출하는 박형 래퍼.
 *
 * 호출 체인: user → 이 함수 → _nvme_ocssd_ns_cmd_vector_rw_with_md → submit
 */
int
spdk_nvme_ocssd_ns_cmd_vector_write_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lba_list, uint32_t num_lbas,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags)
{
	/* [한국어] 메타 포함 vector write — 공통 헬퍼에 metadata 그대로 전달. */
	return _nvme_ocssd_ns_cmd_vector_rw_with_md(ns, qpair, buffer, metadata, lba_list,
			num_lbas, cb_fn, cb_arg, SPDK_OCSSD_OPC_VECTOR_WRITE, io_flags);
}

/*
 * [한국어]
 * spdk_nvme_ocssd_ns_cmd_vector_write - vector write 공개 API (메타데이터 없음)
 *
 * 메타 버퍼만 NULL로 넘기는 박형 래퍼. 디바이스가 메타 영역을 자동으로 처리하거나
 * 사용자가 PI/태그를 신경쓰지 않는 경우 사용.
 *
 * 호출 체인: user → 이 함수 → _nvme_ocssd_ns_cmd_vector_rw_with_md(metadata=NULL)
 */
int
spdk_nvme_ocssd_ns_cmd_vector_write(struct spdk_nvme_ns *ns,
				    struct spdk_nvme_qpair *qpair,
				    void *buffer,
				    uint64_t *lba_list, uint32_t num_lbas,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				    uint32_t io_flags)
{
	/* [한국어] 메타 미사용 vector write. metadata 자리에 NULL 전달. */
	return _nvme_ocssd_ns_cmd_vector_rw_with_md(ns, qpair, buffer, NULL, lba_list,
			num_lbas, cb_fn, cb_arg, SPDK_OCSSD_OPC_VECTOR_WRITE, io_flags);
}

/*
 * [한국어]
 * spdk_nvme_ocssd_ns_cmd_vector_read_with_md - vector read + 메타데이터 공개 API
 *
 * write_with_md의 read 버전. opcode만 VECTOR_READ로 바뀐다.
 * 컨트롤러가 buffer/metadata 두 영역에 DMA 쓰기로 결과를 채운다.
 */
int
spdk_nvme_ocssd_ns_cmd_vector_read_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lba_list, uint32_t num_lbas,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags)
{
	/* [한국어] 메타 포함 vector read — 공통 헬퍼에 opc=VECTOR_READ로 위임. */
	return _nvme_ocssd_ns_cmd_vector_rw_with_md(ns, qpair, buffer, metadata, lba_list,
			num_lbas, cb_fn, cb_arg, SPDK_OCSSD_OPC_VECTOR_READ, io_flags);
}

/*
 * [한국어]
 * spdk_nvme_ocssd_ns_cmd_vector_read - vector read 공개 API (메타데이터 없음)
 *
 * 메타 버퍼만 NULL로 넘기는 박형 래퍼. 일반적인 데이터 read 경로.
 */
int
spdk_nvme_ocssd_ns_cmd_vector_read(struct spdk_nvme_ns *ns,
				   struct spdk_nvme_qpair *qpair,
				   void *buffer,
				   uint64_t *lba_list, uint32_t num_lbas,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				   uint32_t io_flags)
{
	/* [한국어] 메타 미사용 vector read. metadata=NULL. */
	return _nvme_ocssd_ns_cmd_vector_rw_with_md(ns, qpair, buffer, NULL, lba_list,
			num_lbas, cb_fn, cb_arg, SPDK_OCSSD_OPC_VECTOR_READ, io_flags);
}

/*
 * [한국어]
 * spdk_nvme_ocssd_ns_cmd_vector_copy - 디바이스 내부 벡터 복사 (in-device copy)
 *
 * @ns:           OCSSD namespace
 * @qpair:        명령 제출 qpair
 * @dst_lba_list: 목적지 PPA 배열 (기록 대상)
 * @src_lba_list: 출발지 PPA 배열 (읽기 대상)
 * @num_lbas:     배열 길이 (양쪽 동일). [1, 64].
 * @cb_fn/@cb_arg: 비동기 완료 콜백
 * @io_flags:     LIMITED_RETRY만 허용
 * @return: 0 성공, -EINVAL/-ENOMEM 실패
 *
 * 호스트 메모리를 거치지 않고 컨트롤러가 src LBA들을 읽어 dst LBA들로 직접
 * 복사한다(가비지 컬렉션, FTL 내부 마이그레이션 같은 워크로드에 유용).
 * 데이터 페이로드가 호스트 측에 없으므로 nvme_allocate_request_null을 사용한다.
 *
 * SQE 인코딩:
 *   - cdw10/11: src LBA 리스트 (단일이면 값, 다중이면 PA)
 *   - cdw14/15: dst LBA 리스트 (단일이면 값, 다중이면 PA)
 *   - cdw12   : (num_lbas - 1) | io_flags
 *
 * 실행 컨텍스트: qpair 소유 스레드. 락 불필요.
 *
 * 호출 체인:
 *   user → spdk_nvme_ocssd_ns_cmd_vector_copy → nvme_allocate_request_null
 *        → nvme_qpair_submit_request → 트랜스포트 submit → device 내부 카피
 */
int
spdk_nvme_ocssd_ns_cmd_vector_copy(struct spdk_nvme_ns *ns,
				   struct spdk_nvme_qpair *qpair,
				   uint64_t *dst_lba_list,
				   uint64_t *src_lba_list,
				   uint32_t num_lbas,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				   uint32_t io_flags)
{
	struct nvme_request	*req;
	/* [한국어] qpair 풀에서 받을 요청 객체. */
	struct spdk_nvme_cmd	*cmd;
	/* [한국어] req->cmd 단축 포인터 (SQE). */

	uint32_t valid_flags = SPDK_OCSSD_IO_FLAGS_LIMITED_RETRY;
	/* [한국어] 허용 io_flags 비트 마스크. */

	if (io_flags & ~valid_flags) {
		/* [한국어] 정의되지 않은 비트가 켜져 있으면 거부. */
		return -EINVAL;
	}

	if (!dst_lba_list || !src_lba_list || (num_lbas == 0) ||
	    (num_lbas > SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES)) {
		/* [한국어] src/dst 둘 다 비-NULL이어야 하고 num_lbas는 [1, 64]. */
		return -EINVAL;
	}

	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
	/* [한국어] 호스트→장치 데이터 전송이 없으므로 NULL payload 요청. */
	if (req == NULL) {
		/* [한국어] 풀 고갈 — 호출자가 재시도 책임. */
		return -ENOMEM;
	}

	cmd = &req->cmd;
	/* [한국어] SQE 빌드 시작. */
	cmd->opc = SPDK_OCSSD_OPC_VECTOR_COPY;
	/* [한국어] OCSSD opcode 0x93 (Vector Copy) — 디바이스 내부 read+write. */
	cmd->nsid = ns->id;
	/* [한국어] 대상 namespace. */

	/*
	 * Dword 10 and 11 store a pointer to the list of source logical
	 * block addresses.
	 * Dword 14 and 15 store a pointer to the list of destination logical
	 * block addresses.
	 * If there is a single entry in the LBA list, the logical block
	 * address should be stored instead.
	 */
	if (num_lbas == 1) {
		/* [한국어] 단일 LBA 최적화: src/dst 모두 값 자체를 인라인. */
		*(uint64_t *)&cmd->cdw10 = *src_lba_list;
		/* [한국어] cdw10/11에 src LBA. */
		*(uint64_t *)&cmd->cdw14 = *dst_lba_list;
		/* [한국어] cdw14/15에 dst LBA. */
	} else {
		/* [한국어] 다중 LBA: 각 배열의 물리 주소를 인코딩. */
		*(uint64_t *)&cmd->cdw10 = spdk_vtophys(src_lba_list, NULL);
		/* [한국어] cdw10/11에 src 배열 PA — 컨트롤러가 DMA로 읽음. */
		*(uint64_t *)&cmd->cdw14 = spdk_vtophys(dst_lba_list, NULL);
		/* [한국어] cdw14/15에 dst 배열 PA — 동일하게 DMA로 읽음. */
	}

	cmd->cdw12 = num_lbas - 1;
	/* [한국어] 0-based NLB. */
	cmd->cdw12 |= io_flags;
	/* [한국어] LIMITED_RETRY 등 io_flags 비트 OR. */

	return nvme_qpair_submit_request(qpair, req);
	/* [한국어] qpair에 submit. 디바이스가 내부적으로 src 페이지를 읽어 dst에 기록. */
}
