/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2023 Samsung Electronics Co., Ltd. All rights reserved.
 */

/*
 * [한국어 설명] NVMe Namespace I/O 커맨드 빌더 (nvme_ns_cmd.c) — 1516 라인
 *
 * === 파일의 역할 ===
 * SPDK 공개 API `spdk_nvme_ns_cmd_read/write/...`의 **실제 구현**.
 * 사용자 I/O 요청을 받아 `struct nvme_request`로 변환하고, 필요 시 split하여
 * qpair submit 경로로 전달한다. 즉 "애플리케이션 의도를 NVMe SQE로 번역"하는
 * 핵심 번역 레이어.
 *
 * 주요 책임:
 *   1) 파라미터 검증 (정렬, 범위, io_flags, accel_sequence 적용 가능성)
 *   2) nvme_request 할당 (qpair 로컬 풀에서 pop)
 *   3) SQE 채우기 — opcode/nsid/SLBA(CDW10-11)/NLB(CDW12)/APPTAG/REFTAG
 *   4) split 판정 — MDTS 초과, stripe 경계, PRP boundary 위반 시 parent/child 분할
 *   5) qpair submit 경로로 제출 (nvme_qpair_submit_request)
 *
 * 호출 흐름 (read 예):
 *   [user] spdk_nvme_ns_cmd_read(ns, qpair, buf, lba, count, cb, ctx, flags)
 *     → _nvme_ns_cmd_rw(..., opc=READ, ...) [이 파일]
 *       → nvme_allocate_request_contig(qpair, buf, size, cb, ctx)
 *         → nvme_request 확보 + payload(CONTIG 모드) 초기화
 *       → 크기·경계 초과면 _nvme_ns_cmd_split_request_* 로 분할
 *       → _nvme_ns_cmd_setup_request() — SQE 필드 채움 (opc, nsid, LBA, NLB, PI)
 *       → nvme_qpair_submit_request(qpair, req) [nvme_internal.h 프로토타입]
 *         → 트랜스포트 submit → SQ 기록 → doorbell ring → 장치
 *
 * 지원 명령 (전체):
 *   - READ / READV / READV_ext
 *   - WRITE / WRITEV / WRITEV_ext
 *   - WRITE_ZEROES / WRITE_UNCORRECTABLE
 *   - DATASET MANAGEMENT (UNMAP, TRIM)
 *   - FLUSH
 *   - COMPARE / COMPAREV
 *   - COMPARE + WRITE (fused)
 *   - COPY (Simple Copy)
 *   - RESERVATION Register/Report/Release/Acquire
 *   - Verify
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe 드라이버에서 "사용자 API 진입점"의 실제 구현. 공개 헤더
 * include/spdk/nvme.h가 선언한 함수들을 이 파일이 구현.
 *
 * 상위 (호출 측):
 *   - 애플리케이션 직접 호출
 *   - module/bdev/nvme/bdev_nvme.c — bdev_io를 spdk_nvme_ns_cmd_*로 변환
 *   - NVMe-oF target의 local bdev passthru 경로
 *
 * 하위 (호출 당하는 측):
 *   - lib/nvme/nvme.c — nvme_allocate_request_*, nvme_free_request
 *   - lib/nvme/nvme_qpair.c — nvme_qpair_submit_request
 *   - (간접) lib/nvme/nvme_pcie*.c — 트랜스포트 submit 구현
 *
 * 실행 컨텍스트: 호출 스레드(= qpair 소유 스레드). 모든 함수가 thread-local.
 * qpair는 생성한 스레드에만 묶이므로 이 파일의 함수들은 자연스럽게 락 없이 동작.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - nvme_internal.h — 모든 내부 구조체 (nvme_request, qpair, ns 등)
 * 공유 자료구조:
 *   - struct spdk_nvme_ns, struct spdk_nvme_qpair (읽기 전용)
 *   - struct nvme_request (qpair 풀에서 소유권 획득 후 submit까지 유지)
 * 에러 매핑:
 *   - 풀 고갈: -ENOMEM (사용자가 process_completions 후 재시도)
 *   - 요청이 너무 큼(split도 불가): -EINVAL (nvme_ns_map_failure_rc가 변환)
 *   - 잘못된 io_flags: -EINVAL
 *   - accel_sequence 미지원: -EINVAL
 *
 * === 주요 함수/구조체 요약 ===
 *   - _nvme_ns_cmd_rw(): ★ 모든 R/W 명령의 공통 엔트리 ★
 *     파라미터 검증 + request 할당 + split 판정 + SQE 빌드 + submit
 *   - _nvme_ns_cmd_setup_request(): SQE 필드(opc/nsid/SLBA/NLB/PI) 채움
 *   - _nvme_ns_cmd_split_request():      stripe/MDTS 경계 분할 (LBA 기준)
 *   - _nvme_ns_cmd_split_request_prp():  PRP 경계 위반 시 분할 (페이지 경계)
 *   - _nvme_ns_cmd_split_request_sgl():  SGL 모드 split
 *   - _nvme_add_child_request(): split 시 child 생성·parent 링크
 *   - nvme_ns_check_request_length(): split 결과가 qdepth 초과인지 체크
 *   - nvme_ns_map_failure_rc(): -ENOMEM vs -EINVAL 분류
 *   - spdk_nvme_ns_cmd_read/write/...: 공개 API 진입점 (파일 뒷부분)
 */

#include "nvme_internal.h"       /* [한국어] 모든 내부 타입 — nvme_request, spdk_nvme_ns/qpair/ctrlr, allocate_request_* 등 */

/*
 * [한국어] _nvme_ns_cmd_rw - 모든 R/W 명령의 공통 엔트리 (전방 선언)
 *
 * 아래 wrapper 함수들(spdk_nvme_ns_cmd_read 등)이 호출하는 내부 구현.
 * opc 인자로 READ/WRITE/COMPARE 등을 구분.
 * check_sgl=true 시 PRP 경계 위반 검사 수행 (CONTIG payload에서 중요).
 * accel_sequence 전달 시 해당 qpair가 accel 지원인지 검증 후 req에 연결.
 */
static inline struct nvme_request *_nvme_ns_cmd_rw(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		const struct nvme_payload *payload, uint32_t payload_offset, uint32_t md_offset,
		uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
		void *cb_arg, uint32_t opc, uint32_t io_flags,
		uint16_t apptag_mask, uint16_t apptag, uint32_t cdw13, bool check_sgl,
		void *accel_sequence, int *rc);

static bool
nvme_ns_check_request_length(uint32_t lba_count, uint32_t sectors_per_max_io,
			     uint32_t sectors_per_stripe, uint32_t qdepth)
/*
 * [한국어]
 * nvme_ns_check_request_length - 요청이 split되면 child 수가 qpair 큐 깊이를
 *                                초과하는지 판정
 *
 * @lba_count: 요청 LBA 수
 * @sectors_per_max_io: MDTS 기반 단일 I/O 최대 섹터
 * @sectors_per_stripe: stripe 크기 (0=stripe 분할 없음)
 * @qdepth: qpair 큐 깊이 (SQ 엔트리 수)
 * @return: true면 qdepth 초과 — -EINVAL로 변환돼야 함
 *
 * 사용: -ENOMEM 발생 시 "풀 고갈"인지 "애초에 요청이 불가능한 크기"인지
 *       구분 — 후자는 retry해도 무의미하므로 -EINVAL로 실패 고정.
 *
 * 특이: namespace 해제(hot-plug) 시 sectors_per_* 필드가 0으로 초기화됨 →
 *       이 경우 child_per_io = UINT32_MAX로 남아 true 반환(체크 통과 불가).
 */
{
	uint32_t child_per_io = UINT32_MAX;
                                  /* [한국어] 초기값 — 계산 불가(hot-plug 후)일 때 "무한히 많은 자식" 가정 */

	/* After a namespace is destroyed(e.g. hotplug), all the fields associated with the
	 * namespace will be cleared to zero, the function will return TRUE for this case,
	 * and -EINVAL will be returned to caller.
	 */
	if (sectors_per_stripe > 0) {
                                  /* [한국어] stripe 분할 모드 — 하드웨어가 특정 경계에서 성능 이점 제공 시 */
		child_per_io = (lba_count + sectors_per_stripe - 1) / sectors_per_stripe;
                                  /* [한국어] 올림 나눗셈 — lba_count를 stripe 크기로 나눈 child 수 */
	} else if (sectors_per_max_io > 0) {
                                  /* [한국어] MDTS 기반 분할 — 단일 I/O 최대 섹터 초과 시 */
		child_per_io = (lba_count + sectors_per_max_io - 1) / sectors_per_max_io;
                                  /* [한국어] MDTS 기준 child 수 계산 */
	}

	SPDK_DEBUGLOG(nvme, "checking maximum i/o length %d\n", child_per_io);
                                  /* [한국어] 디버그 로그 — split 분석 용도 */

	return child_per_io >= qdepth;
                                  /* [한국어] child가 qpair 깊이를 넘으면 parent가 모든 child를 동시 발행 불가 → 실패 */
}

static inline int
nvme_ns_map_failure_rc(uint32_t lba_count, uint32_t sectors_per_max_io,
		       uint32_t sectors_per_stripe, uint32_t qdepth, int rc)
/*
 * [한국어]
 * nvme_ns_map_failure_rc - 실패 코드 정제
 *
 * -ENOMEM으로 실패했을 때, 사실상 크기 자체가 불가능한 경우(qdepth 초과)
 * 라면 -EINVAL로 재매핑. 이렇게 해야 호출자가 retry vs fail을 올바르게
 * 구분 가능.
 */
{
	assert(rc);                   /* [한국어] 이 함수는 rc != 0일 때만 호출되어야 함 */
	if (rc == -ENOMEM &&
	    nvme_ns_check_request_length(lba_count, sectors_per_max_io, sectors_per_stripe, qdepth)) {
                                  /* [한국어] -ENOMEM이면서 요청이 근본적으로 불가능한 크기 → 영구 실패로 분류 */
		return -EINVAL;
	}
	return rc;                    /* [한국어] 정상 -ENOMEM (일시적 풀 고갈) 또는 기타 에러는 그대로 반환 */
}

static inline bool
_nvme_md_excluded_from_xfer(struct spdk_nvme_ns *ns, uint32_t io_flags)
/*
 * [한국어]
 * _nvme_md_excluded_from_xfer - 이 요청에서 메타데이터가 호스트 버퍼에서 "빠지는가"
 *
 * 조건 4개 동시 만족 시 true:
 *   1) PRACT 비트 (PI Action enabled — 장치가 PI를 자동 삽입/검증)
 *   2) Extended LBA 지원 NS
 *   3) PI 지원 NS
 *   4) md_size == 8 (16-bit PI + AppTag + RefTag = 8B)
 *
 * 이 조건이면 호스트는 메타 필드를 포함하지 않은 데이터만 전송(장치가 PI를 on-the-fly 처리).
 * false이면 호스트가 (데이터 + 메타) 모두 전송해야 함.
 */
{
	return (io_flags & SPDK_NVME_IO_FLAGS_PRACT) &&
	       (ns->flags & SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED) &&
	       (ns->flags & SPDK_NVME_NS_DPS_PI_SUPPORTED) &&
	       (ns->md_size == 8);
}

static inline uint32_t
_nvme_get_host_buffer_sector_size(struct spdk_nvme_ns *ns, uint32_t io_flags)
/*
 * [한국어]
 * _nvme_get_host_buffer_sector_size - 호스트 버퍼 내 "1 섹터"의 실제 바이트 크기
 *
 * - 메타가 xfer에서 제외되면(위 함수) sector_size만(예: 512)
 * - 포함되면 extended_lba_size(예: 512+8 = 520)
 *
 * split 계산에서 payload_offset 증가량을 정확히 잡기 위해 필요.
 */
{
	return _nvme_md_excluded_from_xfer(ns, io_flags) ?
	       ns->sector_size : ns->extended_lba_size;
}

static inline uint32_t
_nvme_get_sectors_per_max_io(struct spdk_nvme_ns *ns, uint32_t io_flags)
/*
 * [한국어]
 * _nvme_get_sectors_per_max_io - 현재 io_flags 조건에서의 MDTS 기반 최대 섹터
 *
 * 메타 제외 시 섹터가 작으므로 같은 MDTS 바이트에 더 많은 섹터 들어감 →
 * sectors_per_max_io_no_md (더 큰 값) 사용.
 * 포함 시 섹터가 크므로 sectors_per_max_io (기본값) 사용.
 */
{
	return _nvme_md_excluded_from_xfer(ns, io_flags) ?
	       ns->sectors_per_max_io_no_md : ns->sectors_per_max_io;
}

static struct nvme_request *
_nvme_add_child_request(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			const struct nvme_payload *payload,
			uint32_t payload_offset, uint32_t md_offset,
			uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc,
			uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag, uint32_t cdw13,
			struct nvme_request *parent, bool check_sgl, int *rc)
/*
 * [한국어]
 * _nvme_add_child_request - parent 아래 하나의 child I/O 생성·등록
 *
 * split 경로의 핵심 헬퍼. _nvme_ns_cmd_rw로 child request를 만들고 parent에
 * 연결한다. child 생성이 실패하면 이미 만들어진 parent와 child들을 일괄
 * 해제(roll-back)하여 자원 누수·부분 제출 방지.
 */
{
	struct nvme_request	*child;

	child = _nvme_ns_cmd_rw(ns, qpair, payload, payload_offset, md_offset, lba, lba_count, cb_fn,
				cb_arg, opc, io_flags, apptag_mask, apptag, cdw13, check_sgl, NULL, rc);
                                  /* [한국어] child request 생성 — 재귀가 아니라 단일 호출 (parent가 아닌 child 경로로 실행)
                                   *  accel_sequence는 NULL(split 중에는 미지원, _nvme_ns_cmd_split_request 진입 시 이미 검사) */
	if (child == NULL) {
                                  /* [한국어] child 생성 실패 → 롤백 */
		nvme_request_free_children(parent);
                                  /* [한국어] 이미 만들어진 형제 child들 모두 해제 */
		nvme_free_request(parent);
                                  /* [한국어] parent도 해제 — 호출자에게는 NULL 반환으로 실패 전파 */
		return NULL;
	}

	nvme_request_add_child(parent, child);
                                  /* [한국어] parent의 children 리스트에 등록 — 완료 집계에 사용 */
	return child;
}

static struct nvme_request *
_nvme_ns_cmd_split_request(struct spdk_nvme_ns *ns,
			   struct spdk_nvme_qpair *qpair,
			   const struct nvme_payload *payload,
			   uint32_t payload_offset, uint32_t md_offset,
			   uint64_t lba, uint32_t lba_count,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc,
			   uint32_t io_flags, struct nvme_request *req,
			   uint32_t sectors_per_max_io, uint32_t sector_mask,
			   uint16_t apptag_mask, uint16_t apptag, uint32_t cdw13,
			   void *accel_sequence, int *rc)
/*
 * [한국어]
 * _nvme_ns_cmd_split_request - ★ stripe/MDTS 경계 기반 LBA split ★
 *
 * lba_count가 sectors_per_max_io를 초과하거나 stripe 경계를 가로지르면
 * 여러 child로 분할해 각각 제출. parent(req)는 완료 집계만 담당.
 *
 * 분할 로직:
 *   - 남은 범위에서 "다음 stripe 경계까지의 거리" 만큼만 child로 사용
 *   - 그 뒤부터는 sectors_per_max_io 만큼씩 child 생성
 *   - payload_offset/md_offset를 각 child에 맞게 증가시켜 전달
 *
 * 제약: accel_sequence가 있으면 split 금지 — sequence는 atomic 단위라 분할 불가.
 */
{
	uint32_t		sector_size = _nvme_get_host_buffer_sector_size(ns, io_flags);
                                  /* [한국어] 호스트 버퍼에서 "1 섹터"가 차지하는 바이트 (메타 포함/제외에 따라 다름) */
	uint32_t		remaining_lba_count = lba_count;
                                  /* [한국어] 아직 할당하지 않은 LBA 수 */
	struct nvme_request	*child;

	if (spdk_unlikely(accel_sequence != NULL)) {
                                  /* [한국어] accel 시퀀스는 분할할 수 없음 — 전체 단일 제출이어야만 가속기 체인이 원자 적용 */
		NVME_QPAIR_ERRLOG(qpair, "Splitting requests with accel sequence is unsupported\n");
		*rc = -EINVAL;
		return NULL;
	}

	while (remaining_lba_count > 0) {
                                  /* [한국어] 모든 LBA가 child에 배정될 때까지 반복 */
		lba_count = sectors_per_max_io - (lba & sector_mask);
                                  /* [한국어] 다음 stripe 경계까지의 거리 계산
                                   *  - lba & sector_mask : 현재 LBA의 stripe 내 오프셋
                                   *  - sectors_per_max_io - 오프셋 : 경계까지 남은 섹터
                                   *  이렇게 해야 child가 stripe 경계를 가로지르지 않음 */
		lba_count = spdk_min(remaining_lba_count, lba_count);
                                  /* [한국어] 남은 양이 더 적으면 그 크기로 한정 (마지막 child) */

		child = _nvme_add_child_request(ns, qpair, payload, payload_offset, md_offset,
						lba, lba_count, cb_fn, cb_arg, opc,
						io_flags, apptag_mask, apptag, cdw13, req, true, rc);
                                  /* [한국어] 이 청크를 처리할 child 생성 + parent에 연결 */
		if (child == NULL) {
			return NULL;  /* [한국어] 실패 시 _nvme_add_child_request가 parent+형제 이미 해제함 */
		}

		remaining_lba_count -= lba_count;
                                  /* [한국어] 처리된 만큼 감소 */
		lba += lba_count;     /* [한국어] LBA 커서 전진 */
		payload_offset += lba_count * sector_size;
                                  /* [한국어] 호스트 버퍼 오프셋 전진 — 메타 포함/제외 고려한 sector_size 사용 */
		md_offset += lba_count * ns->md_size;
                                  /* [한국어] 분리형 메타 버퍼 오프셋도 별도 전진 */
	}

	return req;                   /* [한국어] parent 반환 — 모든 child가 parent에 링크됨. 호출자는 parent를 submit */
}

static inline bool
_is_io_flags_valid(uint32_t io_flags)
/*
 * [한국어]
 * _is_io_flags_valid - io_flags가 정의된 비트만 사용하는지 검증
 *
 * 정의되지 않은 비트가 설정되어 있으면 장치가 예측 불가 동작 가능 →
 * 제출 전 차단. VALID_MASK는 nvme_spec.h에서 NVMe 스펙이 정의한 모든 플래그 OR.
 */
{
	if (spdk_unlikely(io_flags & ~SPDK_NVME_IO_FLAGS_VALID_MASK)) {
                                  /* [한국어] unlikely — 대부분 사용자가 유효 플래그만 전달. 런타임 최적화 힌트 */
		/* Invalid io_flags */
		SPDK_ERRLOG("Invalid io_flags 0x%x\n", io_flags);
		return false;
	}

	return true;
}

static inline bool
_is_accel_sequence_valid(struct spdk_nvme_qpair *qpair, void *seq)
/*
 * [한국어]
 * _is_accel_sequence_valid - accel 시퀀스 사용 가능 여부 검증
 *
 * 허용 조건:
 *   - seq == NULL (시퀀스 없음 — 항상 OK)
 *   - 또는 (컨트롤러 accel 지원 + qpair가 poll_group에 속함)
 * poll_group이 있어야 하는 이유: accel 시퀀스 실행은 group 수준 accel_fn_table을
 * 통해 디스패치됨.
 */
{
	/* An accel sequence can only be executed if the controller supports accel and a qpair is
	 * part of a of a poll group */
	if (spdk_likely(seq == NULL || ((qpair->ctrlr->flags & SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED) &&
					qpair->poll_group != NULL))) {
                                  /* [한국어] likely — 대부분 NULL 또는 정상 설정 */
		return true;
	}

	return false;
}

static void
_nvme_ns_cmd_setup_request(struct spdk_nvme_ns *ns, struct nvme_request *req,
			   uint32_t opc, uint64_t lba, uint32_t lba_count,
			   uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag,
			   uint32_t cdw13)
/*
 * [한국어]
 * _nvme_ns_cmd_setup_request - ★ SQE 필드 채움 (I/O 경로의 핵심 번역 단계) ★
 *
 * 사용자 인자(opc, lba, lba_count, io_flags, apptag/mask)를 NVMe 스펙의
 * SQE 레이아웃(CDW10-15 등)에 정확히 매핑한다. 이 함수가 호출된 이후
 * req->cmd는 장치가 읽을 수 있는 완전한 SQE가 된다(dptr은 나중에 트랜스포트가 채움).
 *
 * 매핑 규칙 (NVMe NVM Command Set):
 *   OPC  (CDW0 하위) = opc
 *   NSID (CDW1)     = ns->id
 *   CDW10-11        = SLBA (64-bit Starting LBA)
 *   CDW12           = NLB (0-based, 하위 16bit) + 제어 플래그(PRCHK/FUA/LR 등)
 *   CDW13           = DSM hint, directive 등 (호출자가 지정)
 *   CDW14           = EILBRT (PI Type1/2 only — LBA 하위 32비트를 RefTag로 기대)
 *   CDW15           = ELBAT (상위 16 = AppTag mask, 하위 16 = AppTag)
 *
 * 호출 체인: _nvme_ns_cmd_rw → 이 함수 → nvme_qpair_submit_request
 */
{
	struct spdk_nvme_cmd	*cmd;

	assert(_is_io_flags_valid(io_flags));
                                  /* [한국어] 호출 전에 검증됐어야 함 — 디버그 빌드에서 방어 */

	cmd = &req->cmd;              /* [한국어] SQE 본체 참조 */
	cmd->opc = opc;               /* [한국어] opcode (READ=0x02, WRITE=0x01, COMPARE=0x05, WRITE_ZEROES=0x08 등) */
	cmd->nsid = ns->id;           /* [한국어] NSID — 대상 네임스페이스. 특수값 0xFFFFFFFF는 여기선 사용 안 함(I/O는 개별 NS) */

	*(uint64_t *)&cmd->cdw10 = lba;
                                  /* [한국어] CDW10-11에 64비트 SLBA 통째로 기록.
                                   *  - cmd 구조체의 cdw10은 uint32_t이지만 cdw10-cdw11이 메모리상 연속 배치 → 64비트 alias로 접근
                                   *  - NVMe 스펙: SLBA[31:0] in CDW10, SLBA[63:32] in CDW11 (little-endian)
                                   *  - struct alignment/packed 가정에 의존 → SPDK_STATIC_ASSERT로 보장 */

	if (ns->flags & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
                                  /* [한국어] NS가 PI 지원이면 RefTag 필드 세팅 (CDW14) */
		switch (ns->pi_type) {
		case SPDK_NVME_FMT_NVM_PROTECTION_TYPE1:
		case SPDK_NVME_FMT_NVM_PROTECTION_TYPE2:
                                  /* [한국어] Type1/2 PI: EILBRT = SLBA 하위 32비트
                                   *  - 장치가 read/write 시 이 RefTag가 각 블록의 RefTag와 일치하는지 검증
                                   *  - Type3는 RefTag 검사 안 함 → 설정 불필요 */
			cmd->cdw14 = (uint32_t)lba;
                                  /* [한국어] SLBA의 하위 32비트만 사용 (NVMe PI 기본 32-bit RefTag) */
			break;
		}
	}

	cmd->fuse = (io_flags & SPDK_NVME_IO_FLAGS_FUSE_MASK);
                                  /* [한국어] fused 비트 추출 — CDW0의 FUSE 필드 (0=normal, 1=first, 2=second)
                                   *  COMPARE+WRITE atomic 구현에서 사용 */

	cmd->cdw12 = lba_count - 1;   /* [한국어] NLB (Number of Logical Blocks) — **0-based** 표기
                                   *  - 1블록 I/O는 NLB=0, 2블록은 NLB=1, ..., 64K 블록은 NLB=0xFFFF
                                   *  - 스펙 §6 "Number of Logical Blocks is a 0's based value" */
	cmd->cdw12 |= (io_flags & SPDK_NVME_IO_FLAGS_CDW12_MASK);
                                  /* [한국어] CDW12의 상위 비트에 플래그 OR — PRCHK(PI check bits), PRACT, FUA, LR 등 */

	cmd->cdw13 = cdw13;           /* [한국어] 호출자가 지정한 CDW13 (DSM hint, directive specific) — 일반 R/W는 0 */

	cmd->cdw15 = apptag_mask;     /* [한국어] ELBAT 필드의 상위 16비트에 AppTag mask 배치 준비 */
	cmd->cdw15 = (cmd->cdw15 << 16 | apptag);
                                  /* [한국어] 최종 CDW15 = (apptag_mask << 16) | apptag
                                   *  - NVMe 스펙: CDW15 [31:16] = ELBATM, [15:0] = ELBAT
                                   *  - ELBAT는 PI AppTag 기대값, ELBATM은 비교 마스크 (bit=1인 비트만 비교) */
}

static struct nvme_request *
_nvme_ns_cmd_split_request_prp(struct spdk_nvme_ns *ns,
			       struct spdk_nvme_qpair *qpair,
			       const struct nvme_payload *payload,
			       uint32_t payload_offset, uint32_t md_offset,
			       uint64_t lba, uint32_t lba_count,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc,
			       uint32_t io_flags, struct nvme_request *req,
			       uint16_t apptag_mask, uint16_t apptag, uint32_t cdw13,
			       void *accel_sequence, int *rc)
{
	spdk_nvme_req_reset_sgl_cb reset_sgl_fn = req->payload.reset_sgl_fn;
	spdk_nvme_req_next_sge_cb next_sge_fn = req->payload.next_sge_fn;
	void *sgl_cb_arg = req->payload.contig_or_cb_arg;
	bool start_valid, end_valid, last_sge, child_equals_parent;
	uint64_t child_lba = lba;
	uint32_t req_current_length = 0;
	uint32_t child_length = 0;
	uint32_t sge_length;
	uint32_t page_size = qpair->ctrlr->page_size;
	uintptr_t address;

	reset_sgl_fn(sgl_cb_arg, payload_offset);
	next_sge_fn(sgl_cb_arg, (void **)&address, &sge_length);
	while (req_current_length < req->payload_size) {

		if (sge_length == 0) {
			continue;
		} else if (req_current_length + sge_length > req->payload_size) {
			sge_length = req->payload_size - req_current_length;
		}

		/*
		 * The start of the SGE is invalid if the start address is not page aligned,
		 *  unless it is the first SGE in the child request.
		 */
		start_valid = child_length == 0 || _is_page_aligned(address, page_size);

		/* Boolean for whether this is the last SGE in the parent request. */
		last_sge = (req_current_length + sge_length == req->payload_size);

		/*
		 * The end of the SGE is invalid if the end address is not page aligned,
		 *  unless it is the last SGE in the parent request.
		 */
		end_valid = last_sge || _is_page_aligned(address + sge_length, page_size);

		/*
		 * This child request equals the parent request, meaning that no splitting
		 *  was required for the parent request (the one passed into this function).
		 *  In this case, we do not create a child request at all - we just send
		 *  the original request as a single request at the end of this function.
		 */
		child_equals_parent = (child_length + sge_length == req->payload_size);

		if (start_valid) {
			/*
			 * The start of the SGE is valid, so advance the length parameters,
			 *  to include this SGE with previous SGEs for this child request
			 *  (if any).  If it is not valid, we do not advance the length
			 *  parameters nor get the next SGE, because we must send what has
			 *  been collected before this SGE as a child request.
			 */
			child_length += sge_length;
			req_current_length += sge_length;
			if (req_current_length < req->payload_size) {
				next_sge_fn(sgl_cb_arg, (void **)&address, &sge_length);
				/*
				 * If the next SGE is not page aligned, we will need to create a
				 *  child request for what we have so far, and then start a new
				 *  child request for the next SGE.
				 */
				start_valid = _is_page_aligned(address, page_size);
			}
		}

		if (start_valid && end_valid && !last_sge) {
			continue;
		}

		/*
		 * We need to create a split here.  Send what we have accumulated so far as a child
		 *  request.  Checking if child_equals_parent allows us to *not* create a child request
		 *  when no splitting is required - in that case we will fall-through and just create
		 *  a single request with no children for the entire I/O.
		 */
		if (!child_equals_parent) {
			struct nvme_request *child;
			uint32_t child_lba_count;

			if ((child_length % ns->extended_lba_size) != 0) {
				NVME_QPAIR_ERRLOG(qpair, "child_length %u not even multiple of lba_size %u\n",
						  child_length, ns->extended_lba_size);
				*rc = -EINVAL;
				return NULL;
			}
			if (spdk_unlikely(accel_sequence != NULL)) {
				NVME_QPAIR_ERRLOG(qpair, "Splitting requests with accel sequence is unsupported\n");
				*rc = -EINVAL;
				return NULL;
			}

			child_lba_count = child_length / ns->extended_lba_size;
			/*
			 * Note the last parameter is set to "false" - this tells the recursive
			 *  call to _nvme_ns_cmd_rw() to not bother with checking for SGL splitting
			 *  since we have already verified it here.
			 */
			child = _nvme_add_child_request(ns, qpair, payload, payload_offset, md_offset,
							child_lba, child_lba_count,
							cb_fn, cb_arg, opc, io_flags,
							apptag_mask, apptag, cdw13, req, false, rc);
			if (child == NULL) {
				return NULL;
			}
			payload_offset += child_length;
			md_offset += child_lba_count * ns->md_size;
			child_lba += child_lba_count;
			child_length = 0;
		}
	}

	if (child_length == req->payload_size) {
		/* No splitting was required, so setup the whole payload as one request. */
		_nvme_ns_cmd_setup_request(ns, req, opc, lba, lba_count, io_flags, apptag_mask, apptag, cdw13);
	}

	return req;
}

static struct nvme_request *
_nvme_ns_cmd_split_request_sgl(struct spdk_nvme_ns *ns,
			       struct spdk_nvme_qpair *qpair,
			       const struct nvme_payload *payload,
			       uint32_t payload_offset, uint32_t md_offset,
			       uint64_t lba, uint32_t lba_count,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc,
			       uint32_t io_flags, struct nvme_request *req,
			       uint16_t apptag_mask, uint16_t apptag, uint32_t cdw13,
			       void *accel_sequence, int *rc)
{
	spdk_nvme_req_reset_sgl_cb reset_sgl_fn = req->payload.reset_sgl_fn;
	spdk_nvme_req_next_sge_cb next_sge_fn = req->payload.next_sge_fn;
	void *sgl_cb_arg = req->payload.contig_or_cb_arg;
	uint64_t child_lba = lba;
	uint32_t req_current_length = 0;
	uint32_t accumulated_length = 0;
	uint32_t sge_length;
	uint16_t max_sges, num_sges;
	uintptr_t address;

	max_sges = ns->ctrlr->max_sges;

	reset_sgl_fn(sgl_cb_arg, payload_offset);
	num_sges = 0;

	while (req_current_length < req->payload_size) {
		next_sge_fn(sgl_cb_arg, (void **)&address, &sge_length);

		if (req_current_length + sge_length > req->payload_size) {
			sge_length = req->payload_size - req_current_length;
		}

		accumulated_length += sge_length;
		req_current_length += sge_length;
		num_sges++;

		if (num_sges < max_sges && req_current_length < req->payload_size) {
			continue;
		}

		/*
		 * We need to create a split here.  Send what we have accumulated so far as a child
		 *  request.  Checking if the child equals the full payload allows us to *not*
		 *  create a child request when no splitting is required - in that case we will
		 *  fall-through and just create a single request with no children for the entire I/O.
		 */
		if (accumulated_length != req->payload_size) {
			struct nvme_request *child;
			uint32_t child_lba_count;
			uint32_t child_length;
			uint32_t extra_length;

			child_length = accumulated_length;
			/* Child length may not be a multiple of the block size! */
			child_lba_count = child_length / ns->extended_lba_size;
			extra_length = child_length - (child_lba_count * ns->extended_lba_size);
			if (extra_length != 0) {
				/* The last SGE does not end on a block boundary. We need to cut it off. */
				if (extra_length >= child_length) {
					NVME_QPAIR_ERRLOG(qpair, "Unable to send I/O. Would require more than the supported number of "
							  "SGL Elements.");
					*rc = -EINVAL;
					return NULL;
				}
				child_length -= extra_length;
			}

			if (spdk_unlikely(accel_sequence != NULL)) {
				NVME_QPAIR_ERRLOG(qpair, "Splitting requests with accel sequence is unsupported\n");
				*rc = -EINVAL;
				return NULL;
			}

			/*
			 * Note the last parameter is set to "false" - this tells the recursive
			 *  call to _nvme_ns_cmd_rw() to not bother with checking for SGL splitting
			 *  since we have already verified it here.
			 */
			child = _nvme_add_child_request(ns, qpair, payload, payload_offset, md_offset,
							child_lba, child_lba_count,
							cb_fn, cb_arg, opc, io_flags,
							apptag_mask, apptag, cdw13, req, false, rc);
			if (child == NULL) {
				return NULL;
			}
			payload_offset += child_length;
			md_offset += child_lba_count * ns->md_size;
			child_lba += child_lba_count;
			accumulated_length -= child_length;
			num_sges = accumulated_length > 0;
		}
	}

	if (accumulated_length == req->payload_size) {
		/* No splitting was required, so setup the whole payload as one request. */
		_nvme_ns_cmd_setup_request(ns, req, opc, lba, lba_count, io_flags, apptag_mask, apptag, cdw13);
	}

	return req;
}

/*
 * [한국어] ★★★ _nvme_ns_cmd_rw - 모든 R/W 경로의 실제 구현 진입점 ★★★
 *
 * 공개 API `spdk_nvme_ns_cmd_read/write/compare/...`가 공통으로 호출하는
 * 내부 함수. 역할:
 *   1) nvme_request 할당 (qpair 풀에서, 실패 시 -ENOMEM)
 *   2) payload_offset/md_offset/accel_sequence 등 request 메타 설정
 *   3) split 판정 (stripe / MDTS / SGL·PRP 경계)
 *   4) split 불필요하면 _nvme_ns_cmd_setup_request로 SQE 빌드
 *   5) split이 필요하면 적절한 split 함수로 위임
 *
 * 반환값:
 *   - 성공: parent request 포인터 (호출자가 submit)
 *   - 실패: NULL + *rc에 음수 errno
 *
 * 호출자 패턴 (spdk_nvme_ns_cmd_read):
 *   req = _nvme_ns_cmd_rw(...);
 *   if (req) return nvme_qpair_submit_request(qpair, req);
 *   else return nvme_ns_map_failure_rc(lba_count, ..., rc);
 */
static inline struct nvme_request *
_nvme_ns_cmd_rw(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		const struct nvme_payload *payload, uint32_t payload_offset, uint32_t md_offset,
		uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc,
		uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag, uint32_t cdw13, bool check_sgl,
		void *accel_sequence, int *rc)
{
	struct nvme_request	*req;
	uint32_t		sector_size = _nvme_get_host_buffer_sector_size(ns, io_flags);
                                  /* [한국어] 호스트 버퍼 내 "1 섹터" 크기 (메타 포함/제외 반영) */
	uint32_t		sectors_per_max_io = _nvme_get_sectors_per_max_io(ns, io_flags);
                                  /* [한국어] 단일 I/O 최대 섹터 수 (io_flags 보정값) */
	uint32_t		sectors_per_stripe = ns->sectors_per_stripe;
                                  /* [한국어] stripe 크기 (Intel DC P3x00 같은 일부 NVMe에서 성능 최적화용) */

	assert(rc != NULL);           /* [한국어] 호출자 약속: rc 포인터 필수 */
	assert(*rc == 0);             /* [한국어] 호출 전 *rc=0 초기화 필수 */

	req = nvme_allocate_request(qpair, payload, lba_count * sector_size, lba_count * ns->md_size,
				    cb_fn, cb_arg);
                                  /* [한국어] qpair 풀에서 request 획득
                                   *  - payload_size = lba_count * sector_size (데이터 바이트)
                                   *  - md_size      = lba_count * ns->md_size (분리 메타 바이트, PI 등)
                                   *  - 내부에서 req->payload, cb_fn, cb_arg, qpair 필드도 초기화 */
	if (req == NULL) {
		*rc = -ENOMEM;        /* [한국어] 풀 고갈 — 호출자는 spdk_bdev_queue_io_wait 또는 폴링 후 재시도 */
		return NULL;
	}

	req->payload_offset = payload_offset;
                                  /* [한국어] split parent→child 경로에서 전달된 payload 내 시작 오프셋 */
	req->md_offset = md_offset;
                                  /* [한국어] 분리 메타 버퍼 내 시작 오프셋 */
	req->accel_sequence = accel_sequence;
                                  /* [한국어] accel 시퀀스 연결 (NULL 가능) */

	/* Zone append commands cannot be split. */
	if (opc == SPDK_NVME_OPC_ZONE_APPEND) {
                                  /* [한국어] ZNS Zone Append는 장치가 wp를 원자 할당하므로 split 불가
                                   *  - 분할하면 각 child가 독립 wp를 받아 논리적 단일 append 불성립
                                   *  - 호출자 측에서 max_zone_append_size 초과를 사전 검증해야 함 */
		assert(ns->csi == SPDK_NVME_CSI_ZNS);
                                  /* [한국어] ZONE_APPEND opcode는 ZNS NS에서만 유효 */
		/*
		 * As long as we disable driver-assisted striping for Zone append commands,
		 * _nvme_ns_cmd_rw() should never cause a proper request to be split.
		 * If a request is split, after all, error handling is done in caller functions.
		 */
		sectors_per_stripe = 0;
                                  /* [한국어] stripe split 경로 비활성 — 아래 검사에서 stripe split이 발동되지 않도록 */
	}

	/*
	 * Intel DC P3*00 NVMe controllers benefit from driver-assisted striping.
	 * If this controller defines a stripe boundary and this I/O spans a stripe
	 *  boundary, split the request into multiple requests and submit each
	 *  separately to hardware.
	 */
	if (sectors_per_stripe > 0 &&
	    (((lba & (sectors_per_stripe - 1)) + lba_count) > sectors_per_stripe)) {
                                  /* [한국어] stripe split 조건:
                                   *  - stripe 정의됨 AND
                                   *  - (stripe 내 시작 오프셋 + 요청 섹터 수) > stripe 크기
                                   *    = 이 I/O가 stripe 경계를 가로지름
                                   *  stripe_mask = stripe - 1 (stripe가 2의 거듭제곱일 때 유효)
                                   *  경계 가로지르는 I/O는 장치가 두 stripe에 분산 처리해야 해서 지연 증가 → 드라이버가 미리 분할 */
		return _nvme_ns_cmd_split_request(ns, qpair, payload, payload_offset, md_offset, lba, lba_count,
						  cb_fn,
						  cb_arg, opc,
						  io_flags, req, sectors_per_stripe, sectors_per_stripe - 1,
						  apptag_mask, apptag, cdw13,  accel_sequence, rc);
                                  /* [한국어] stripe 기반 분할 — sector_mask=stripe-1로 경계 계산 */
	} else if (lba_count > sectors_per_max_io) {
                                  /* [한국어] MDTS 기반 분할 — 단일 I/O 최대치 초과 시 */
		return _nvme_ns_cmd_split_request(ns, qpair, payload, payload_offset, md_offset, lba, lba_count,
						  cb_fn,
						  cb_arg, opc,
						  io_flags, req, sectors_per_max_io, 0, apptag_mask,
						  apptag, cdw13, accel_sequence, rc);
                                  /* [한국어] sector_mask=0 → stripe 경계 없이 순수하게 sectors_per_max_io 단위로만 분할 */
	} else if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL && check_sgl) {
                                  /* [한국어] SGL 페이로드이며 SGL 경계 검사가 요구된 경우
                                   *  - SGL 세그먼트가 페이지/장치 제약을 위반하면 split 필요
                                   *  - check_sgl은 split 재귀 방지용(이미 split된 child 재검사 안 함) */
		if (ns->ctrlr->flags & SPDK_NVME_CTRLR_SGL_SUPPORTED) {
                                  /* [한국어] 장치가 SGL 지원 → SGL 모드로 split */
			return _nvme_ns_cmd_split_request_sgl(ns, qpair, payload, payload_offset, md_offset,
							      lba, lba_count, cb_fn, cb_arg, opc, io_flags,
							      req, apptag_mask, apptag, cdw13,
							      accel_sequence, rc);
		} else {
                                  /* [한국어] 장치가 SGL 미지원 → PRP로 변환하며 4KB 경계 기준 split */
			return _nvme_ns_cmd_split_request_prp(ns, qpair, payload, payload_offset, md_offset,
							      lba, lba_count, cb_fn, cb_arg, opc, io_flags,
							      req, apptag_mask, apptag, cdw13,
							      accel_sequence, rc);
		}
	}

	_nvme_ns_cmd_setup_request(ns, req, opc, lba, lba_count, io_flags, apptag_mask, apptag, cdw13);
                                  /* [한국어] split 불필요 — 단일 SQE로 완성. 호출자가 nvme_qpair_submit_request로 제출 */
	return req;
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_compare ★
 *
 * NVMe Compare 커맨드 (opcode 0x05)의 기본 진입점.
 * 장치 내부에 저장된 데이터와 호스트가 제공한 buffer를 "비교"만 수행 (장치 상태 불변).
 * 불일치 시 CQE status = SPDK_NVME_SC_COMPARE_FAILURE 반환.
 *
 * 사용처:
 *   - fused operation(COMPARE+WRITE)의 전반부 — atomic CAS 구현
 *     (컨트롤러가 CSN(Compare-Success-required-Next) 플래그로 연속 처리)
 *   - 데이터 무결성 검증(읽어와서 host가 memcmp 하는 대신 장치 오프로드)
 *
 * 호출 체인:
 *   spdk_nvme_ns_cmd_compare
 *     → _nvme_ns_cmd_rw (opc=COMPARE)
 *       → _nvme_ns_cmd_setup_request (SQE 완성: SLBA/NLB/PI 필드)
 *     → nvme_qpair_submit_request → 트랜스포트 → 장치
 *
 * read/write와 완전히 동일한 파이프라인 — opcode만 다르다.
 */
int
spdk_nvme_ns_cmd_compare(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer,
			 uint64_t lba,
			 uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			 uint32_t io_flags)
{
	struct nvme_request *req;
                                  /* [한국어] 빌드된 nvme_request 포인터 수령 변수 */
	struct nvme_payload payload;
                                  /* [한국어] payload 기술자 (CONTIG/SGL + md 포인터) */
	int rc = 0;
                                  /* [한국어] _nvme_ns_cmd_rw 실패 시 에러 코드 OUT 파라미터 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] 예약 비트·미지원 플래그 검사 — 스펙 외 동작 방지 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_CONTIG(buffer, NULL);
                                  /* [한국어] 연속 버퍼 기술자 구성 (metadata=NULL — interleaved 또는 metadata 미사용 NS) */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg,
			      SPDK_NVME_OPC_COMPARE,
			      io_flags, 0,
			      0, 0, false, NULL, &rc);
                                  /* [한국어] 공통 R/W 빌더 호출 — opc=COMPARE(0x05)
                                   *  - apptag/mask=0, cdw13=0 (DSM hint 없음)
                                   *  - check_sgl=false (CONTIG 경로)
                                   *  - accel_sequence=NULL */
	if (req != NULL) {
                                  /* [한국어] 성공 경로 — SQ에 제출 */
		return nvme_qpair_submit_request(qpair, req);
	} else {
                                  /* [한국어] 실패 경로 — lba_count가 qdepth 초과 시 -ENOMEM→-EINVAL로 재분류 */
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_compare_with_md ★
 *
 * Compare 커맨드의 separate-metadata 변형.
 * metadata 버퍼가 별도로 제공되는 경우(namespace가 extended LBA가 아닌 PI/MD 모드)에 사용.
 *
 * apptag_mask/apptag는 NVMe PI(Protection Information) 검사 파라미터:
 *   - mask의 비트가 1인 위치만 apptag와 비교
 *   - Type1/Type2 PI에서 PRACT=1이고 Read처럼 장치가 PI를 제거하지 않는
 *     Compare의 경우 호스트 제공 tag로 검사 수행
 *
 * 호출 체인: compare와 동일, 다만 payload에 metadata 포인터가 채워진다.
 */
int
spdk_nvme_ns_cmd_compare_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				 void *buffer,
				 void *metadata,
				 uint64_t lba,
				 uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				 uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request *req;
                                  /* [한국어] 빌드된 nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] CONTIG + 분리 metadata 포인터 포함 payload */
	int rc = 0;
                                  /* [한국어] 실패 OUT 에러 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] io_flags 유효성 검사 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_CONTIG(buffer, metadata);
                                  /* [한국어] CONTIG + separate MD 버퍼 (장치 DMA가 PRP 2세트 사용) */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg,
			      SPDK_NVME_OPC_COMPARE,
			      io_flags,
			      apptag_mask, apptag, 0, false, NULL, &rc);
                                  /* [한국어] apptag_mask/apptag 전달 — _nvme_ns_cmd_setup_request가 CDW15에 배치 */
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 성공 경로 */
	} else {
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 실패 시 에러 재분류 */
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_comparev ★
 *
 * Compare 커맨드의 scatter-gather 변형 — payload가 여러 비연속 버퍼에 흩어져 있을 때.
 *
 * SGL 모드의 동작:
 *   - reset_sgl_fn: 분할 offset으로 SGL 반복자를 재설정 (split 발생 시)
 *   - next_sge_fn : 다음 SGE(주소, 길이) 반환
 *   - _nvme_ns_cmd_rw는 SGE를 순회하여 총 바이트를 산출하고, SGL/PRP 제약 위반 시 child로 분할
 *
 * check_sgl=true로 전달 → SGL 경계/max_sge 초과 검사 수행.
 */
int
spdk_nvme_ns_cmd_comparev(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			  uint64_t lba, uint32_t lba_count,
			  spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			  spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			  spdk_nvme_req_next_sge_cb next_sge_fn)
{
	struct nvme_request *req;
                                  /* [한국어] 빌드된 nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] SGL 모드 payload (reset/next 콜백 쌍 저장) */
	int rc = 0;
                                  /* [한국어] 실패 OUT */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] flags 유효성 검사 */
		return -EINVAL;
	}

	if (reset_sgl_fn == NULL || next_sge_fn == NULL) {
                                  /* [한국어] SGL 모드 필수 콜백 누락 거부 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_SGL(reset_sgl_fn, next_sge_fn, cb_arg, NULL);
                                  /* [한국어] SGL 모드 payload 구성 — cb_arg는 호출자 SGL iter 상태 보관
                                   *         (호스트 측 콜백 문맥으로 재사용) */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg,
			      SPDK_NVME_OPC_COMPARE,
			      io_flags, 0, 0, 0, true, NULL, &rc);
                                  /* [한국어] check_sgl=true — SGL split 판정 필요 */
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 — split된 child들도 함께 제출됨 */
	} else {
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 에러 재분류 */
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_comparev_with_md ★
 *
 * Compare의 SGL + separate metadata 풀 버전 — 데이터는 SGL, metadata는 단일 contiguous 버퍼.
 * PI+extended LBA NS에서 host가 metadata를 별도로 검사하거나 제공할 때.
 *
 * 의미: comparev와 compare_with_md의 합집합.
 *   데이터 payload    = SGL (reset_sgl_fn/next_sge_fn)
 *   metadata payload  = contiguous (metadata 포인터)
 *   PI apptag/mask    = PI 검증 파라미터
 */
int
spdk_nvme_ns_cmd_comparev_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				  uint64_t lba, uint32_t lba_count,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				  spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				  spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				  uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request *req;
                                  /* [한국어] nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] SGL + separate MD payload */
	int rc = 0;
                                  /* [한국어] OUT 에러 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] io_flags 검증 */
		return -EINVAL;
	}

	if (reset_sgl_fn == NULL || next_sge_fn == NULL) {
                                  /* [한국어] SGL 콜백 필수 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_SGL(reset_sgl_fn, next_sge_fn, cb_arg, metadata);
                                  /* [한국어] SGL 데이터 + contiguous metadata */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg,
			      SPDK_NVME_OPC_COMPARE, io_flags, apptag_mask, apptag, 0, true,
			      NULL, &rc);
                                  /* [한국어] apptag_mask/apptag는 PI 검사 파라미터로 CDW15에 기록됨 */
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 성공 */
	} else {
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 에러 재분류 */
	}
}

/*
 * [한국어] ★ 공개 API 진입점: spdk_nvme_ns_cmd_read ★
 *
 * 애플리케이션이 호출하는 가장 일반적인 read 진입점.
 * 전형 I/O 경로의 "호스트 측 첫 단계":
 *   1) io_flags 검증
 *   2) CONTIG 페이로드 기술자 생성 (buffer, metadata=NULL)
 *   3) _nvme_ns_cmd_rw로 nvme_request 준비 (+ split 판정)
 *   4) 성공 시 nvme_qpair_submit_request로 제출 — 반환값이 최종 결과
 *   5) 실패 시 nvme_ns_map_failure_rc로 -ENOMEM을 -EINVAL로 재분류 가능성 확인
 *
 * 다른 read 변형(read_with_md, readv, readv_with_md, read_ext, ...)도
 * 이 함수와 거의 동일한 구조 — payload만 다르게 구성하여 동일한
 * _nvme_ns_cmd_rw로 위임.
 */
int
spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer,
		      uint64_t lba,
		      uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		      uint32_t io_flags)
{
	struct nvme_request *req;
	struct nvme_payload payload;
	int rc = 0;

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] 정의되지 않은 io_flags 비트 거부 (스펙 외 동작 방지) */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_CONTIG(buffer, NULL);
                                  /* [한국어] CONTIG 모드 페이로드 구성 — 단일 연속 버퍼, 메타 없음
                                   *  - NVME_PAYLOAD_CONTIG 매크로가 reset_sgl_fn=NULL로 설정 (CONTIG 식별) */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_READ,
			      io_flags, 0,
			      0, 0, false, NULL, &rc);
                                  /* [한국어] 공통 R/W 빌더 호출
                                   *  - opc = SPDK_NVME_OPC_READ (0x02)
                                   *  - apptag/mask = 0 (PI 검사 파라미터 미지정)
                                   *  - cdw13 = 0 (DSM hint 없음)
                                   *  - check_sgl = false (CONTIG 모드에서는 SGL 경계 검사 불필요)
                                   *  - accel_sequence = NULL
                                   *  - rc = OUT 에러 코드 수령 */
	if (req != NULL) {
                                  /* [한국어] 성공 경로 — request 준비 완료, qpair에 제출 */
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] submit 결과가 최종 반환값:
                                   *  0 = 제출 성공 (cb가 나중에 호출됨)
                                   *  -ENOMEM = tracker 풀 고갈 (이미 queued_req에 적재됐으나 드물게 실패)
                                   *  기타 음수 = qpair 상태 오류 등 */
	} else {
                                  /* [한국어] 실패 경로 — rc에 -ENOMEM 등. map_failure_rc로 영구 실패 여부 재분류 */
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 요청 크기가 qdepth 초과로 불가능하면 -ENOMEM → -EINVAL로 변환
                                   *  (호출자가 queue_io_wait 재시도 헛수고 방지) */
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_read_with_md ★
 *
 * read의 separate-metadata 변형.
 * 장치가 LBA와 별도로 metadata(일반적 8/16B)를 DMA하도록 요청.
 *
 * NVMe에서 metadata 배치는 두 가지:
 *   1) Extended LBA: 데이터 + metadata가 연속 저장 (이 함수 대신 기본 read 사용)
 *   2) Separate metadata: metadata가 별도 buffer (이 함수가 지원) — 장치는 MPTR 필드로 메타 PRP/SGL 전달
 *
 * apptag_mask/apptag: PI Type1/Type2 NS에서 AppTag 검증 파라미터.
 */
int
spdk_nvme_ns_cmd_read_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer,
			      void *metadata,
			      uint64_t lba,
			      uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			      uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request *req;
                                  /* [한국어] nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] CONTIG + 분리 metadata */
	int rc = 0;
                                  /* [한국어] OUT 에러 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] io_flags 검증 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_CONTIG(buffer, metadata);
                                  /* [한국어] CONTIG + separate metadata */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_READ,
			      io_flags,
			      apptag_mask, apptag, 0, false, NULL, &rc);
                                  /* [한국어] opc=READ, apptag/mask 전달 */
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 성공 */
	} else {
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 에러 재분류 */
	}
}

/*
 * [한국어] nvme_ns_cmd_rw_ext - _ext 변형(read_ext/write_ext)의 공통 내부 빌더 (CONTIG 전용)
 *
 * @ns / @qpair: 대상 네임스페이스와 제출 큐
 * @buffer: 데이터 CONTIG 버퍼 (가상 주소)
 * @lba / @lba_count: 시작 LBA와 블록 수
 * @cb_fn / @cb_arg: 완료 콜백 + 컨텍스트
 * @opts: struct spdk_nvme_ns_cmd_ext_io_opts (★) — 확장 옵션 번들. ABI 호환성 패턴(size 필드 선행)으로
 *        필드 추가 시에도 전방 호환 보장. 주요 필드:
 *          - metadata: separate metadata 포인터
 *          - io_flags: LR/FUA/PRCHK/PRACT 등
 *          - apptag/apptag_mask: PI 검사
 *          - accel_sequence: accel(DPU/GPU) 오프로드 시퀀스 포인터
 *          - cdw13: DSM hint 등 제조사별 DWORD13 확장
 * @opc: SPDK_NVME_OPC_READ 또는 WRITE (assert로 강제)
 * @return: 0=성공, -EINVAL=잘못된 플래그/accel, -ENOMEM=request 고갈
 *
 * 기본 read/write와 차이:
 *   - ext 옵션을 payload.opts에 포인터로 연결 — 하위 레이어(pcie_common, accel 엔진 등)가 참조
 *   - accel_sequence 지원 — 장치 DMA 전/후 호스트 측 컴퓨트 파이프라인 연결 가능
 *     (예: CRC 계산, 압축 해제 등을 accel 엔진에 오프로드)
 *
 * 호출 체인:
 *   spdk_nvme_ns_cmd_read_ext/write_ext → nvme_ns_cmd_rw_ext → _nvme_ns_cmd_rw → submit
 */
static int
nvme_ns_cmd_rw_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer,
		   uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		   struct spdk_nvme_ns_cmd_ext_io_opts *opts, enum spdk_nvme_nvm_opcode opc)
{
	struct nvme_request *req;
                                  /* [한국어] 빌드된 nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] payload (CONTIG + opts 포인터 연결) */
	void *seq;
                                  /* [한국어] accel sequence 포인터 (accel 오프로드 활성 시 non-NULL) */
	int rc = 0;
                                  /* [한국어] OUT 에러 */

	assert(opc == SPDK_NVME_OPC_READ || opc == SPDK_NVME_OPC_WRITE);
                                  /* [한국어] 이 함수는 READ/WRITE 전용 — 다른 opc는 호출자 버그 */
	assert(opts);
                                  /* [한국어] opts는 NULL 불가 — 기본 read/write를 써야 할 경우 다른 API 사용 */

	payload = NVME_PAYLOAD_CONTIG(buffer, opts->metadata);
                                  /* [한국어] CONTIG 페이로드 + opts 내 metadata 포인터 */

	if (spdk_unlikely(!_is_io_flags_valid(opts->io_flags))) {
                                  /* [한국어] unlikely — 정상 경로에서 flags가 거의 항상 유효하다는 힌트 */
		return -EINVAL;
	}

	seq = nvme_ns_cmd_get_ext_io_opt(opts, accel_sequence, NULL);
                                  /* [한국어] opts에서 accel_sequence 필드를 안전하게 추출
                                   *         (size 필드 기반 — 구버전 opts에는 필드 없음이 정상) */
	if (spdk_unlikely(!_is_accel_sequence_valid(qpair, seq))) {
                                  /* [한국어] accel_sequence 사용 시 qpair가 poll_group에 연결되어 있어야 함 */
		return -EINVAL;
	}

	payload.opts = opts;
                                  /* [한국어] 하위 레이어가 ext 옵션 전체를 참조할 수 있게 payload에 포인터 연결
                                   *         (NVMe cdw12/cdw13 확장, memory_domain, DIF 마스크 등 사용) */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, opc, opts->io_flags,
			      opts->apptag_mask, opts->apptag, 0, false, seq, &rc);
                                  /* [한국어] 공통 R/W 빌더 호출 — opc=호출자 지정, accel_sequence=seq */
	if (spdk_unlikely(req == NULL)) {
                                  /* [한국어] 요청 할당/빌드 실패 */
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
	}

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 결과 반환 (0/음수 errno) */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_read_ext ★
 *
 * read의 extended options 풀버전. bdev_nvme 등 상위 모듈이 memory_domain/accel_sequence/
 * metadata/PI/vendor cdw13 등을 세밀하게 제어할 때 사용한다.
 *
 * 내부적으로 nvme_ns_cmd_rw_ext(opc=READ)로 위임.
 */
int
spdk_nvme_ns_cmd_read_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer,
			  uint64_t lba,
			  uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			  struct spdk_nvme_ns_cmd_ext_io_opts *opts)
{
	return nvme_ns_cmd_rw_ext(ns, qpair, buffer, lba, lba_count, cb_fn, cb_arg, opts,
				  SPDK_NVME_OPC_READ);
                                  /* [한국어] 공통 _ext 빌더에 opc=READ로 위임 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_readv ★
 *
 * read의 scatter-gather 변형. 호스트 버퍼가 비연속(예: bdev_io iovec 여러 개)일 때 사용.
 *
 * SGL 콜백 쌍(reset_sgl_fn/next_sge_fn)은 호출자가 구현:
 *   - reset_sgl_fn(cb_arg, offset): 상태를 offset 위치로 재설정 (split 시 sub-request마다 호출)
 *   - next_sge_fn(cb_arg, &addr, &len): 다음 SGE 반환, 마지막이면 non-zero
 *
 * 장치가 SGL 미지원이면 내부에서 PRP로 변환(4KB 경계 정렬 검사) — 이때 정렬 위반이면 split.
 */
int
spdk_nvme_ns_cmd_readv(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       uint64_t lba, uint32_t lba_count,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
		       spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
		       spdk_nvme_req_next_sge_cb next_sge_fn)
{
	struct nvme_request *req;
                                  /* [한국어] 빌드된 nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] SGL 모드 payload */
	int rc = 0;
                                  /* [한국어] OUT 에러 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] io_flags 검증 */
		return -EINVAL;
	}

	if (reset_sgl_fn == NULL || next_sge_fn == NULL) {
                                  /* [한국어] SGL 콜백 필수 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_SGL(reset_sgl_fn, next_sge_fn, cb_arg, NULL);
                                  /* [한국어] SGL 모드, metadata 없음. cb_arg는 호출자 iter 상태 보관용 */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_READ,
			      io_flags, 0, 0, 0, true, NULL, &rc);
                                  /* [한국어] opc=READ, check_sgl=true — SGL/PRP 경계 검증 및 필요 시 split */
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 — split된 child들도 함께 */
	} else {
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 에러 재분류 */
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_readv_with_md ★
 *
 * readv + separate metadata. SGL 데이터 + CONTIG metadata + PI apptag 검사 통합.
 * bdev_nvme가 DIF 검증을 수행할 때의 표준 경로.
 */
int
spdk_nvme_ns_cmd_readv_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       uint64_t lba, uint32_t lba_count,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			       spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			       spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
			       uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request *req;
                                  /* [한국어] 빌드된 nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] SGL + separate MD */
	int rc = 0;
                                  /* [한국어] OUT 에러 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] io_flags 검증 */
		return -EINVAL;
	}

	if (reset_sgl_fn == NULL || next_sge_fn == NULL) {
                                  /* [한국어] SGL 콜백 필수 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_SGL(reset_sgl_fn, next_sge_fn, cb_arg, metadata);
                                  /* [한국어] SGL 데이터 + contiguous metadata */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_READ,
			      io_flags, apptag_mask, apptag, 0, true, NULL, &rc);
                                  /* [한국어] apptag/mask 전달 — PI 검사 파라미터 */
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
	} else {
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 에러 재분류 */
	}
}

/*
 * [한국어] nvme_ns_cmd_rwv_ext - _ext 변형(readv_ext/writev_ext)의 공통 내부 빌더 (SGL 전용)
 *
 * SGL 버전의 _ext 래퍼. opts가 NULL일 수도 있는 점이 rw_ext와의 차이.
 *   - opts == NULL → 기본 readv/writev와 동일(io_flags=0, apptag=0, accel 미사용)
 *   - opts != NULL → ext 옵션 전체 적용 + accel 시퀀스 지원
 *
 * @opc: READ 또는 WRITE (assert 강제)
 *
 * 호출 체인:
 *   spdk_nvme_ns_cmd_readv_ext/writev_ext → nvme_ns_cmd_rwv_ext → _nvme_ns_cmd_rw → submit
 */
static int
nvme_ns_cmd_rwv_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t lba,
		    uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg, spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
		    spdk_nvme_req_next_sge_cb next_sge_fn, struct spdk_nvme_ns_cmd_ext_io_opts *opts,
		    enum spdk_nvme_nvm_opcode opc)
{
	struct nvme_request *req;
                                  /* [한국어] 빌드된 nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] SGL payload (+ opts 연결 가능) */
	void *seq;
                                  /* [한국어] accel sequence (opts 경로에서만 설정) */
	int rc = 0;
                                  /* [한국어] OUT 에러 */

	assert(opc == SPDK_NVME_OPC_READ || opc == SPDK_NVME_OPC_WRITE);
                                  /* [한국어] READ/WRITE 전용 */

	if (reset_sgl_fn == NULL || next_sge_fn == NULL) {
                                  /* [한국어] SGL 콜백 필수 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_SGL(reset_sgl_fn, next_sge_fn, cb_arg, NULL);
                                  /* [한국어] SGL 기본 구성 (metadata는 opts에서 가져오거나 NULL 유지) */

	if (opts) {
                                  /* [한국어] 확장 옵션 경로 */
		if (spdk_unlikely(!_is_io_flags_valid(opts->io_flags))) {
                                  /* [한국어] io_flags 검증 */
			return -EINVAL;
		}

		seq = nvme_ns_cmd_get_ext_io_opt(opts, accel_sequence, NULL);
                                  /* [한국어] opts 구조체 크기 기반 accel_sequence 안전 추출 */
		if (spdk_unlikely(!_is_accel_sequence_valid(qpair, seq))) {
                                  /* [한국어] accel 사용 시 poll_group 연결 필수 */
			return -EINVAL;
		}

		payload.opts = opts;
                                  /* [한국어] 하위 레이어가 ext 옵션 전체 참조 */
		payload.md = opts->metadata;
                                  /* [한국어] separate metadata도 opts에서 가져와 payload에 연결 */
		req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, opc, opts->io_flags,
				      opts->apptag_mask, opts->apptag, opts->cdw13, true, seq, &rc);
                                  /* [한국어] opts의 io_flags, apptag, cdw13, accel_seq 전달 */

	} else {
                                  /* [한국어] 일반 경로 — opts 없음. io_flags=0, apptag=0, cdw13=0 */
		req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, opc, 0, 0, 0, 0,
				      true, NULL, &rc);
	}

	if (req == NULL) {
                                  /* [한국어] 빌드 실패 — 에러 재분류 */
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
	}

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_readv_ext ★
 *
 * readv의 extended options 풀버전. SGL + ext opts(metadata/accel_seq/cdw13 등).
 * opts는 NULL 허용 — NULL이면 일반 readv와 동일(단, check_sgl=true 경로).
 */
int
spdk_nvme_ns_cmd_readv_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			   void *cb_arg, spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			   spdk_nvme_req_next_sge_cb next_sge_fn,
			   struct spdk_nvme_ns_cmd_ext_io_opts *opts)
{
	return nvme_ns_cmd_rwv_ext(ns, qpair, lba, lba_count, cb_fn, cb_arg, reset_sgl_fn, next_sge_fn,
				   opts, SPDK_NVME_OPC_READ);
                                  /* [한국어] 공통 _ext SGL 빌더에 opc=READ로 위임 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_write ★
 *
 * read와 완전히 동일한 구조 — 차이점은 opcode = SPDK_NVME_OPC_WRITE (0x01)
 * 뿐. 바이트 단위 전송, 완료 콜백, 에러 재분류 흐름 모두 공유.
 *
 * 요청이 장치로 나가는 최종 순서 (단일 request 기준):
 *   1) 이 함수 → _nvme_ns_cmd_rw → _nvme_ns_cmd_setup_request에서 SQE 완성
 *      (opc=WRITE, nsid, SLBA=lba, NLB=lba_count-1, io_flags)
 *   2) nvme_qpair_submit_request → 트랜스포트 submit_request
 *   3) nvme_pcie_qpair_submit_request (pcie_internal.h)
 *      → tracker 할당 + PRP 빌드 (CONTIG payload에서 4KB 경계 고려)
 *      → SQ[sq_tail] 위치에 memcpy(SQE)
 *      → nvme_pcie_qpair_ring_sq_doorbell → MMIO write
 *   4) 장치가 SQE fetch → 쓰기 처리 → CQE 기록 → phase 토글
 *   5) 다음 process_completions 폴링에서 CQE 해석 → req->cb_fn 호출
 */
int
spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       void *buffer, uint64_t lba,
		       uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		       uint32_t io_flags)
{
	struct nvme_request *req;
	struct nvme_payload payload;
	int rc = 0;

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] 정의되지 않은 io_flags 비트 있으면 거부 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_CONTIG(buffer, NULL);
                                  /* [한국어] CONTIG 페이로드, 분리 메타 없음 */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_WRITE,
			      io_flags, 0, 0, 0, false, NULL, &rc);
                                  /* [한국어] 공통 R/W 빌더 — opc=WRITE(0x01) */
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] qpair에 제출 → 트랜스포트 → 장치 SQE fetch */
	} else {
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 실패 → 에러 코드 정제 후 반환 */
	}
}

/*
 * [한국어] nvme_ns_cmd_check_zone_append - Zone Append 사전 검사 헬퍼
 *
 * @ns: 대상 네임스페이스 (ZNS일 수도 아닐 수도 있음)
 * @lba_count: 요청 블록 수
 * @io_flags: CDW12 확장 등 (PRACT 포함)
 * @return: 0=OK, -EINVAL=지원 안 함 또는 크기 초과
 *
 * Zone Append는 일반 write와 달리 장치가 wp를 자동 할당해 돌려주는 명령 (ZNS 필수 기능).
 * 이 함수는 두 가지를 검사:
 *   1) 컨트롤러가 Zone Append 지원 플래그를 set 했는지 (identify 결과로 설정됨)
 *   2) 요청 바이트 수(lba_count * sector_size)가 max_zone_append_size(ZASL) 이하인지
 *      — ZASL ≤ MDTS 이므로, check_request_length와 별개 검사 필요
 *
 * sector_size는 extended LBA 여부에 따라 sector_size 또는 extended_lba_size.
 */
static int
nvme_ns_cmd_check_zone_append(struct spdk_nvme_ns *ns, uint32_t lba_count, uint32_t io_flags)
{
	uint32_t sector_size;
                                  /* [한국어] 호스트 버퍼 섹터 크기 (PRACT/extended 여부에 따라) */

	/* Not all NVMe Zoned Namespaces support the zone append command. */
	if (!(ns->ctrlr->flags & SPDK_NVME_CTRLR_ZONE_APPEND_SUPPORTED)) {
                                  /* [한국어] 컨트롤러 기능 비트 미세팅 시 거부 (ZNS라도 append 미지원일 수 있음) */
		return -EINVAL;
	}

	sector_size =  _nvme_get_host_buffer_sector_size(ns, io_flags);
                                  /* [한국어] host buffer sector size — extended LBA면 data+md, 아니면 data만 */

	/* Fail a too large zone append command early. */
	if (lba_count * sector_size > ns->ctrlr->max_zone_append_size) {
                                  /* [한국어] ZASL(Zone Append Size Limit) 초과 — split 불가하므로 여기서 조기 실패
                                   *         (ZASL ≤ MDTS 보장됨) */
		return -EINVAL;
	}

	return 0;
                                  /* [한국어] 검사 통과 — 호출자가 _nvme_ns_cmd_rw 진행 가능 */
}

/*
 * [한국어] ★ 공개 API (내부 가시): nvme_ns_cmd_zone_append_with_md ★
 *
 * ZNS Zone Append 커맨드 (opcode 0x7D) CONTIG 버전.
 *
 * Zone Append의 핵심 개념:
 *   - 호스트는 zslba(zone start LBA)만 지정 — 실제 쓰기 위치는 장치가 write pointer에 할당
 *   - 완료 CQE의 cdw0 하위 64비트에 "최종 기록된 LBA" 반환
 *     (spdk_bdev_io_get_append_location가 이를 bdev 레이어까지 전달)
 *   - 이유: 여러 호스트 스레드가 race 없이 같은 zone에 append 가능 (SPDK의 log-structured bdev에서 유용)
 *
 * Split 금지:
 *   - Zone Append는 분할 제출 시 위치 순서 보장 불가 → 분할 불가
 *   - 사전 check_zone_append로 ZASL 초과 차단, CONTIG에서는 이론상 split 발생 안 함
 *   - 그래도 방어적으로 num_children 확인 (assert + runtime guard)
 */
int
nvme_ns_cmd_zone_append_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				void *buffer, void *metadata, uint64_t zslba,
				uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request *req;
                                  /* [한국어] nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] CONTIG + separate MD */
	int rc = 0;
                                  /* [한국어] OUT 에러 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] io_flags 검증 */
		return -EINVAL;
	}

	rc = nvme_ns_cmd_check_zone_append(ns, lba_count, io_flags);
                                  /* [한국어] ZNS 지원 여부 + ZASL 초과 조기 검사 */
	if (rc) {
		return rc;
	}

	payload = NVME_PAYLOAD_CONTIG(buffer, metadata);
                                  /* [한국어] CONTIG 데이터 + 분리 메타 */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, zslba, lba_count, cb_fn, cb_arg,
			      SPDK_NVME_OPC_ZONE_APPEND,
			      io_flags, apptag_mask, apptag, 0, false, NULL, &rc);
                                  /* [한국어] opc=ZONE_APPEND(0x7D), lba=zslba (zone 시작 LBA)
                                   *         실제 기록 위치는 장치가 완료 시 CQE에 반환 */
	if (req != NULL) {
		/*
		 * Zone append commands cannot be split (num_children has to be 0).
		 * For NVME_PAYLOAD_TYPE_CONTIG, _nvme_ns_cmd_rw() should never cause a split
		 * to happen, since a too large request would have already been failed by
		 * nvme_ns_cmd_check_zone_append(), since zasl <= mdts.
		 */
		assert(req->num_children == 0);
                                  /* [한국어] CONTIG + check_zone_append 통과 → split 발생 불가 (ZASL ≤ MDTS) */
		if (req->num_children) {
                                  /* [한국어] 방어적 런타임 체크 — 스펙 위반 상황에서 스플릿이 생긴 경우 */
			nvme_request_free_children(req);
                                  /* [한국어] child request들을 parent에서 떼어 free 풀에 반납 */
			nvme_free_request(req);
                                  /* [한국어] parent request도 free */
			return -EINVAL;
		}
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
	} else {
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 에러 재분류 */
	}
}

/*
 * [한국어] ★ 공개 API (내부 가시): nvme_ns_cmd_zone_appendv_with_md ★
 *
 * Zone Append의 SGL + separate MD 버전.
 *
 * CONTIG 버전과의 차이:
 *   - SGL 경로는 max_sge 초과 / 정렬 위반 시 split을 시도할 수 있음
 *   - check_zone_append로 크기는 안전하게 걸러졌지만, SGE 배치에 따라 split 발생 가능성 존재
 *   - Zone Append는 split 불가이므로, 이 경우 즉시 -EINVAL 반환하고 request 해제
 *
 * 호출자가 SGE 정렬/개수를 장치 max_sge 이하로 유지해야 안전.
 */
int
nvme_ns_cmd_zone_appendv_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				 uint64_t zslba, uint32_t lba_count,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				 spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				 spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				 uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request *req;
                                  /* [한국어] nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] SGL + separate MD */
	int rc = 0;
                                  /* [한국어] OUT 에러 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] io_flags 검증 */
		return -EINVAL;
	}

	if (reset_sgl_fn == NULL || next_sge_fn == NULL) {
                                  /* [한국어] SGL 콜백 필수 */
		return -EINVAL;
	}

	rc = nvme_ns_cmd_check_zone_append(ns, lba_count, io_flags);
                                  /* [한국어] ZNS 지원/ZASL 검사 */
	if (rc) {
		return rc;
	}

	payload = NVME_PAYLOAD_SGL(reset_sgl_fn, next_sge_fn, cb_arg, metadata);
                                  /* [한국어] SGL 데이터 + CONTIG metadata */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, zslba, lba_count, cb_fn, cb_arg,
			      SPDK_NVME_OPC_ZONE_APPEND,
			      io_flags, apptag_mask, apptag, 0, true, NULL, &rc);
                                  /* [한국어] opc=ZONE_APPEND, check_sgl=true (SGL 검증만 필요 — split 금지이지만) */
	if (req != NULL) {
		/*
		 * Zone append commands cannot be split (num_children has to be 0).
		 * For NVME_PAYLOAD_TYPE_SGL, _nvme_ns_cmd_rw() can cause a split.
		 * However, _nvme_ns_cmd_split_request_sgl() and _nvme_ns_cmd_split_request_prp()
		 * do not always cause a request to be split. These functions verify payload size,
		 * verify num sge < max_sge, and verify SGE alignment rules (in case of PRPs).
		 * If any of the verifications fail, they will split the request.
		 * In our case, a split is very unlikely, since we already verified the size using
		 * nvme_ns_cmd_check_zone_append(), however, we still need to call these functions
		 * in order to perform the verification part. If they do cause a split, we return
		 * an error here. For proper requests, these functions will never cause a split.
		 */
		if (req->num_children) {
                                  /* [한국어] SGE 정렬/개수 위반으로 split이 실제 발생 — Zone Append는 split 금지이므로 실패 반환 */
			nvme_request_free_children(req);
                                  /* [한국어] child 해제 */
			nvme_free_request(req);
                                  /* [한국어] parent 해제 */
			return -EINVAL;
		}
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] split 없음 → 단일 request로 제출 */
	} else {
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 에러 재분류 */
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_write_with_md ★
 *
 * write의 separate metadata 변형. PI/MD가 데이터와 별도 버퍼에 제공되는 경우.
 * 장치는 MPTR 필드로 메타 PRP를 받는다.
 */
int
spdk_nvme_ns_cmd_write_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       void *buffer, void *metadata, uint64_t lba,
			       uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			       uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request *req;
                                  /* [한국어] nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] CONTIG + 분리 metadata */
	int rc = 0;
                                  /* [한국어] OUT 에러 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] io_flags 검증 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_CONTIG(buffer, metadata);
                                  /* [한국어] CONTIG + separate MD */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_WRITE,
			      io_flags, apptag_mask, apptag, 0, false, NULL, &rc);
                                  /* [한국어] opc=WRITE(0x01), apptag/mask 전달 */
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 성공 */
	} else {
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 에러 재분류 */
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_write_ext ★
 *
 * write의 extended options 풀버전. CONTIG + ext opts(metadata/accel_seq/cdw13 등).
 * bdev_nvme의 기본 쓰기 경로(with memory_domain, accel)가 이 함수를 호출.
 */
int
spdk_nvme_ns_cmd_write_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   void *buffer, uint64_t lba,
			   uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			   struct spdk_nvme_ns_cmd_ext_io_opts *opts)
{
	return nvme_ns_cmd_rw_ext(ns, qpair, buffer, lba, lba_count, cb_fn, cb_arg, opts,
				  SPDK_NVME_OPC_WRITE);
                                  /* [한국어] 공통 _ext 빌더에 opc=WRITE로 위임 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_writev ★
 *
 * write의 scatter-gather 변형. 호스트 버퍼가 비연속(iovec)일 때 사용.
 * SGL 지원 장치면 장치 DMA가 직접 처리, 미지원이면 PRP로 변환(정렬 위반 시 split).
 */
int
spdk_nvme_ns_cmd_writev(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			uint64_t lba, uint32_t lba_count,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			spdk_nvme_req_next_sge_cb next_sge_fn)
{
	struct nvme_request *req;
                                  /* [한국어] nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] SGL 모드 payload */
	int rc = 0;
                                  /* [한국어] OUT 에러 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] io_flags 검증 */
		return -EINVAL;
	}

	if (reset_sgl_fn == NULL || next_sge_fn == NULL) {
                                  /* [한국어] SGL 콜백 필수 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_SGL(reset_sgl_fn, next_sge_fn, cb_arg, NULL);
                                  /* [한국어] SGL 데이터, metadata 없음 */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_WRITE,
			      io_flags, 0, 0, 0, true, NULL, &rc);
                                  /* [한국어] opc=WRITE, check_sgl=true */
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
	} else {
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 에러 재분류 */
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_writev_with_md ★
 *
 * writev + separate metadata. SGL 데이터 + CONTIG metadata + PI apptag 검사 통합.
 */
int
spdk_nvme_ns_cmd_writev_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				uint64_t lba, uint32_t lba_count,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request *req;
                                  /* [한국어] nvme_request */
	struct nvme_payload payload;
                                  /* [한국어] SGL + separate MD */
	int rc = 0;
                                  /* [한국어] OUT 에러 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] io_flags 검증 */
		return -EINVAL;
	}

	if (reset_sgl_fn == NULL || next_sge_fn == NULL) {
                                  /* [한국어] SGL 콜백 필수 */
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_SGL(reset_sgl_fn, next_sge_fn, cb_arg, metadata);
                                  /* [한국어] SGL 데이터 + CONTIG metadata */

	req = _nvme_ns_cmd_rw(ns, qpair, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_WRITE,
			      io_flags, apptag_mask, apptag, 0, true, NULL, &rc);
                                  /* [한국어] opc=WRITE, apptag/mask=PI 파라미터, check_sgl=true */
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
	} else {
		return nvme_ns_map_failure_rc(lba_count,
					      ns->sectors_per_max_io,
					      ns->sectors_per_stripe,
					      qpair->ctrlr->opts.io_queue_requests,
					      rc);
                                  /* [한국어] 에러 재분류 */
	}
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_writev_ext ★
 *
 * writev의 extended options 풀버전. SGL + ext opts.
 * opts == NULL 허용 — NULL이면 일반 writev와 유사(단, check_sgl=true 경로).
 */
int
spdk_nvme_ns_cmd_writev_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t lba,
			    uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			    spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			    spdk_nvme_req_next_sge_cb next_sge_fn,
			    struct spdk_nvme_ns_cmd_ext_io_opts *opts)
{
	return nvme_ns_cmd_rwv_ext(ns, qpair, lba, lba_count, cb_fn, cb_arg, reset_sgl_fn, next_sge_fn,
				   opts, SPDK_NVME_OPC_WRITE);
                                  /* [한국어] 공통 _ext SGL 빌더에 opc=WRITE로 위임 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_write_zeroes ★
 *
 * NVMe Write Zeroes 커맨드 (opcode 0x08).
 * 지정 LBA 범위를 0으로 채운다. 호스트 데이터 전송 없음 — 장치가 내부적으로 처리.
 * 일반 write보다 훨씬 빠름 (데이터 PCIe 전송 불필요).
 *
 * 사용처:
 *   - 씬 프로비전(thin-provisioning) 초기화
 *   - 보안 삭제(Security Erase)의 저비용 대안
 *   - 파일시스템 discard 경로에서 "zero trim" 동작
 *
 * 특징:
 *   - payload 없음 → nvme_allocate_request_null 사용 (PRP 없는 request)
 *   - _nvme_ns_cmd_rw를 거치지 않고 SQE를 직접 조립
 *   - lba_count는 16비트(NLB 필드 크기) 제약 → UINT16_MAX+1 초과 불가
 */
int
spdk_nvme_ns_cmd_write_zeroes(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      uint64_t lba, uint32_t lba_count,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			      uint32_t io_flags)
{
	struct nvme_request	*req;
                                  /* [한국어] 빌드할 nvme_request */
	struct spdk_nvme_cmd	*cmd;
                                  /* [한국어] req 내부 SQE 포인터 */
	uint64_t		*tmp_lba;
                                  /* [한국어] CDW10/11에 64비트 SLBA 쓰기 위한 alias 포인터 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] io_flags 검증 */
		return -EINVAL;
	}

	if (lba_count == 0 || lba_count > UINT16_MAX + 1) {
                                  /* [한국어] NLB 필드가 16비트 0-based → 최대 (UINT16_MAX+1)=65536 블록 */
		return -EINVAL;
	}

	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
                                  /* [한국어] payload 없는 request 할당 — PRP1/2 =0 초기화 */
	if (req == NULL) {
                                  /* [한국어] request 풀 고갈 — 호출자가 queue_io_wait로 재시도 가능 */
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE 포인터 */
	cmd->opc = SPDK_NVME_OPC_WRITE_ZEROES;
                                  /* [한국어] opcode 0x08 */
	cmd->nsid = ns->id;
                                  /* [한국어] 타겟 namespace ID */

	tmp_lba = (uint64_t *)&cmd->cdw10;
                                  /* [한국어] CDW10(32b) + CDW11(32b)을 64b SLBA로 alias
                                   *         NVMe 스펙: SLBA[31:0]=CDW10, SLBA[63:32]=CDW11 (little-endian 호스트 가정) */
	*tmp_lba = lba;
                                  /* [한국어] 시작 LBA (64비트) */
	cmd->cdw12 = lba_count - 1;
                                  /* [한국어] NLB = lba_count-1 (0-based) */
	cmd->fuse = (io_flags & SPDK_NVME_IO_FLAGS_FUSE_MASK);
                                  /* [한국어] fused 명령 여부 (보통 0 — write_zeroes는 fused 거의 안 씀) */
	cmd->cdw12 |= (io_flags & SPDK_NVME_IO_FLAGS_CDW12_MASK);
                                  /* [한국어] CDW12 상위에 DEAC/PRINFO/LR/FUA 등 io_flags 비트를 OR */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 — SQE는 이미 완성됨. 트랜스포트가 SQ에 기록 후 doorbell ring */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_verify ★
 *
 * NVMe Verify 커맨드 (opcode 0x0C).
 * 지정 LBA 범위의 저장 데이터를 장치 내부적으로 읽어 무결성(ECC/PI) 검증만 수행.
 * 호스트로 데이터 반환 없음 — payload 없는 명령.
 *
 * 사용처: scrubbing(미디어 오류 조기 발견), backup 전 무결성 확인.
 * 실패 시 CQE status로 보고 (미디어 오류 / PI 불일치 등).
 */
int
spdk_nvme_ns_cmd_verify(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			uint64_t lba, uint32_t lba_count,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			uint32_t io_flags)
{
	struct nvme_request	*req;
                                  /* [한국어] nvme_request */
	struct spdk_nvme_cmd	*cmd;
                                  /* [한국어] SQE 포인터 */

	if (!_is_io_flags_valid(io_flags)) {
                                  /* [한국어] io_flags 검증 */
		return -EINVAL;
	}

	if (lba_count == 0 || lba_count > UINT16_MAX + 1) {
                                  /* [한국어] NLB 16비트 제약 */
		return -EINVAL;
	}

	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
                                  /* [한국어] payload 없는 request */
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE 포인터 */
	cmd->opc = SPDK_NVME_OPC_VERIFY;
                                  /* [한국어] opcode 0x0C */
	cmd->nsid = ns->id;
                                  /* [한국어] namespace ID */

	*(uint64_t *)&cmd->cdw10 = lba;
                                  /* [한국어] CDW10+11에 64비트 SLBA 기록 */
	cmd->cdw12 = lba_count - 1;
                                  /* [한국어] NLB 0-based */
	cmd->fuse = (io_flags & SPDK_NVME_IO_FLAGS_FUSE_MASK);
                                  /* [한국어] fused 비트 */
	cmd->cdw12 |= (io_flags & SPDK_NVME_IO_FLAGS_CDW12_MASK);
                                  /* [한국어] PRINFO 등 CDW12 io_flags */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_write_uncorrectable ★
 *
 * NVMe Write Uncorrectable 커맨드 (opcode 0x04).
 * 지정 LBA 범위를 "복구 불가 에러 상태"로 마킹 — 이후 읽기 시 Media Error 반환.
 *
 * 사용처:
 *   - 테스트/진단: 특정 블록 이후 읽기에서 에러 경로 검증
 *   - 보안 삭제 보조: 해당 LBA 읽으면 실패하도록
 *
 * payload 없음 — 데이터 전송 없이 장치 내부 매핑만 갱신.
 * io_flags 지원 안 함 — 단순히 불량 블록 마커 명령.
 */
int
spdk_nvme_ns_cmd_write_uncorrectable(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				     uint64_t lba, uint32_t lba_count,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
                                  /* [한국어] nvme_request */
	struct spdk_nvme_cmd	*cmd;
                                  /* [한국어] SQE 포인터 */
	uint64_t		*tmp_lba;
                                  /* [한국어] CDW10/11 64비트 alias */

	if (lba_count == 0 || lba_count > UINT16_MAX + 1) {
                                  /* [한국어] NLB 16비트 제약 */
		return -EINVAL;
	}

	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
                                  /* [한국어] payload 없는 request */
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE */
	cmd->opc = SPDK_NVME_OPC_WRITE_UNCORRECTABLE;
                                  /* [한국어] opcode 0x04 */
	cmd->nsid = ns->id;
                                  /* [한국어] namespace ID */

	tmp_lba = (uint64_t *)&cmd->cdw10;
                                  /* [한국어] CDW10+11 64비트 SLBA alias */
	*tmp_lba = lba;
                                  /* [한국어] 시작 LBA */
	cmd->cdw12 = lba_count - 1;
                                  /* [한국어] NLB 0-based */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_dataset_management (DSM) ★
 *
 * NVMe Dataset Management 커맨드 (opcode 0x09).
 * 호스트가 "이 LBA 범위에 대해 이런 힌트를 줄게"를 장치에 전달.
 *
 * @type: DSM attributes 비트마스크
 *   - SPDK_NVME_DSM_ATTR_DEALLOCATE: Discard/TRIM (해당 범위 삭제 가능 표시 → GC 효율화)
 *   - SPDK_NVME_DSM_ATTR_INTEGRAL_READ/WRITE: 읽기/쓰기 원자성 힌트
 *   - SPDK_NVME_DSM_ATTR_SEQUENTIAL_READ/WRITE: 순차 액세스 힌트 (prefetch 최적화)
 *   - SPDK_NVME_DSM_ATTR_LATENCY: 지연 민감도 힌트
 *
 * @ranges: LBA 범위 배열 (최대 SPDK_NVME_DATASET_MANAGEMENT_MAX_RANGES = 256)
 *          각 range는 {attributes, length, starting_lba}
 *
 * 장치로 PRP로 ranges 버퍼 DMA 전송 → 장치가 해석 후 내부 메타 갱신.
 * nvme_allocate_request_user_copy: 사용자 버퍼를 DMA-safe 복사본으로 옮기고 request에 연결.
 * 호스트 전달 데이터이므로 user_copy의 host_to_controller=true.
 *
 * 사용처: bdev의 UNMAP/TRIM 구현, FTL GC 힌트 제공.
 */
int
spdk_nvme_ns_cmd_dataset_management(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				    uint32_t type,
				    const struct spdk_nvme_dsm_range *ranges, uint16_t num_ranges,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
                                  /* [한국어] nvme_request */
	struct spdk_nvme_cmd	*cmd;
                                  /* [한국어] SQE */

	if (num_ranges == 0 || num_ranges > SPDK_NVME_DATASET_MANAGEMENT_MAX_RANGES) {
                                  /* [한국어] NR(Number of Ranges) 필드 제약 — NVMe 스펙 최대 256 */
		return -EINVAL;
	}

	if (ranges == NULL) {
                                  /* [한국어] ranges 포인터 필수 */
		return -EINVAL;
	}

	req = nvme_allocate_request_user_copy(qpair, (void *)ranges,
					      num_ranges * sizeof(struct spdk_nvme_dsm_range),
					      cb_fn, cb_arg, true);
                                  /* [한국어] 사용자 버퍼 → DMA 가능 복사본으로 이동 (host_to_controller=true)
                                   *         - 호스트 버퍼가 hugepage가 아닐 수 있어 복사 필요
                                   *         - 완료 후 복사본은 자동 해제 */
	if (req == NULL) {
                                  /* [한국어] 할당 실패 */
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE */
	cmd->opc = SPDK_NVME_OPC_DATASET_MANAGEMENT;
                                  /* [한국어] opcode 0x09 */
	cmd->nsid = ns->id;
                                  /* [한국어] namespace ID */

	cmd->cdw10_bits.dsm.nr = num_ranges - 1;
                                  /* [한국어] CDW10 NR 필드 = num_ranges-1 (0-based) */
	cmd->cdw11 = type;
                                  /* [한국어] CDW11 = AD/IDW/IDR/SR/SW/LR attribute 비트마스크 */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_copy (Simple Copy Command; SCC) ★
 *
 * NVMe Copy 커맨드 (opcode 0x19). NVMe 2.0에서 추가.
 * 장치 내부에서 여러 소스 LBA 범위를 단일 dest_lba로 "장치 offload 복사" 수행.
 * 호스트 PCIe 대역폭 소비 없이 완료 — GC/defrag/migration에 유용.
 *
 * @ranges: 소스 범위 배열 (spdk_nvme_scc_source_range), 각 항목: {starting LBA, nlb, ...}
 * @num_ranges: 소스 범위 수 (최대 128, 벤더별 제약 있음)
 * @dest_lba: 목적 시작 LBA (연속 공간)
 *
 * 제약:
 *   - 컨트롤러가 identify의 ONCS.Copy 비트 set해야 지원
 *   - ranges 배열을 user_copy로 DMA 전달 (DSM과 동일 패턴)
 */
int
spdk_nvme_ns_cmd_copy(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		      const struct spdk_nvme_scc_source_range *ranges,
		      uint16_t num_ranges, uint64_t dest_lba,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
                                  /* [한국어] nvme_request */
	struct spdk_nvme_cmd	*cmd;
                                  /* [한국어] SQE */

	if (num_ranges == 0) {
                                  /* [한국어] 최소 1개 범위 필요 */
		return -EINVAL;
	}

	if (ranges == NULL) {
                                  /* [한국어] ranges 필수 */
		return -EINVAL;
	}

	req = nvme_allocate_request_user_copy(qpair, (void *)ranges,
					      num_ranges * sizeof(struct spdk_nvme_scc_source_range),
					      cb_fn, cb_arg, true);
                                  /* [한국어] 소스 범위 배열을 DMA-safe 복사본으로 이동 (host_to_controller=true) */
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE */
	cmd->opc = SPDK_NVME_OPC_COPY;
                                  /* [한국어] opcode 0x19 */
	cmd->nsid = ns->id;
                                  /* [한국어] namespace ID */

	*(uint64_t *)&cmd->cdw10 = dest_lba;
                                  /* [한국어] CDW10+11에 SDLBA(Destination LBA) 64비트 기록 */
	cmd->cdw12 = num_ranges - 1;
                                  /* [한국어] CDW12 NR(num ranges) 0-based */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_flush ★
 *
 * NVMe Flush 커맨드 (opcode 0x00).
 * 장치에 "쓰기 캐시 → 비휘발성 매체" 플러시를 요청.
 *
 * 의미:
 *   - FUA(Force Unit Access)와 다르게, 특정 쓰기가 아닌 해당 namespace 전체 캐시 플러시
 *   - VWC(Volatile Write Cache) 지원 장치에서만 유효
 *   - 플러시 완료 = 이전 모든 write가 내구성 있는 매체에 도달 보장
 *
 * 사용처:
 *   - 파일시스템 sync/fsync 경로
 *   - 트랜잭션 커밋 포인트 (journal flush)
 *   - 전원 차단 전 안전 보장
 *
 * 가장 간단한 SQE — opcode와 nsid만 설정.
 */
int
spdk_nvme_ns_cmd_flush(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
                                  /* [한국어] nvme_request */
	struct spdk_nvme_cmd	*cmd;
                                  /* [한국어] SQE 포인터 */

	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
                                  /* [한국어] payload 없는 request */
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE */
	cmd->opc = SPDK_NVME_OPC_FLUSH;
                                  /* [한국어] opcode 0x00 */
	cmd->nsid = ns->id;
                                  /* [한국어] 대상 namespace — nsid=0xFFFFFFFF면 모든 NS 플러시 (지원 시) */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
}

/*
 * [한국어] ★★ NVMe Reservation 그룹 배경 설명 ★★
 *
 * NVMe Reservation은 multi-host / multi-path 환경(NVMe-oF, shared SSD)에서
 * 한 호스트가 namespace에 대해 "배타적 또는 공유 쓰기 권한"을 예약하는 메커니즘.
 * SCSI Persistent Reservation (PR)과 유사 — clustered filesystem / failover에 필수.
 *
 * 4개 연관 커맨드:
 *   - Register (0x0D): Host Identifier를 namespace에 등록/갱신/해제 (reservation 참여 등록)
 *   - Release (0x15): 이미 획득한 reservation 해제
 *   - Acquire (0x11): reservation 획득 (Exclusive/Shared 등 타입 지정)
 *   - Report (0x0E): 현재 namespace의 reservation 상태를 호스트로 읽어옴
 *
 * 네 개 모두 nvme_allocate_request_user_copy 사용 — 호스트가 제공한 구조체를
 * DMA 가능 복사본으로 이동해 장치에 전달.
 */

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_reservation_register ★
 *
 * Reservation Register (opcode 0x0D).
 * 호스트의 reservation key를 namespace에 등록/갱신/해제.
 *
 * @payload: register data 구조체 — 현재 키(CRKEY)와 신규 키(NRKEY)
 * @ignore_key: true면 CRKEY 검증 생략 (Register Once 의미 — 충돌 시 초기 등록용)
 * @action: RREGA
 *   - SPDK_NVME_RESERVE_REGISTER_KEY (0): NRKEY로 신규 등록
 *   - SPDK_NVME_RESERVE_UNREGISTER_KEY (1): 등록 해제
 *   - SPDK_NVME_RESERVE_REPLACE_KEY (2): CRKEY → NRKEY 교체
 * @cptpl: Change Persist Through Power Loss
 *   - SPDK_NVME_RESERVE_PTPL_NO_CHANGES: 유지
 *   - SPDK_NVME_RESERVE_PTPL_CLEAR_POWER_ON: 전원 재부팅 시 해제
 *   - SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS: 전원 유지
 */
int
spdk_nvme_ns_cmd_reservation_register(struct spdk_nvme_ns *ns,
				      struct spdk_nvme_qpair *qpair,
				      struct spdk_nvme_reservation_register_data *payload,
				      bool ignore_key,
				      enum spdk_nvme_reservation_register_action action,
				      enum spdk_nvme_reservation_register_cptpl cptpl,
				      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
                                  /* [한국어] nvme_request */
	struct spdk_nvme_cmd	*cmd;
                                  /* [한국어] SQE */

	req = nvme_allocate_request_user_copy(qpair,
					      payload, sizeof(struct spdk_nvme_reservation_register_data),
					      cb_fn, cb_arg, true);
                                  /* [한국어] register data → DMA-safe 복사 후 PRP 연결 (host→controller) */
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE */
	cmd->opc = SPDK_NVME_OPC_RESERVATION_REGISTER;
                                  /* [한국어] opcode 0x0D */
	cmd->nsid = ns->id;
                                  /* [한국어] namespace ID */

	cmd->cdw10_bits.resv_register.rrega = action;
                                  /* [한국어] RREGA: Register action (register/unregister/replace) */
	cmd->cdw10_bits.resv_register.iekey = ignore_key;
                                  /* [한국어] IEKEY: 현재 키 검증 무시 (초기 등록 시 true) */
	cmd->cdw10_bits.resv_register.cptpl = cptpl;
                                  /* [한국어] CPTPL: Change Persist Through Power Loss 설정 */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_reservation_release ★
 *
 * Reservation Release (opcode 0x15).
 * 이미 획득한 reservation을 해제하거나 clear 수행.
 *
 * @action: RRELA
 *   - SPDK_NVME_RESERVE_RELEASE (0): 자신의 reservation만 해제
 *   - SPDK_NVME_RESERVE_CLEAR (1): 모든 reservation과 모든 등록 키 삭제 (관리자 용도)
 * @type: RTYPE — 해제할 reservation의 타입 (Exclusive/Shared Write/Read 등) — release는 현재 타입과 일치해야
 *
 * @payload: 현재 reservation key (CRKEY) — 권한 확인용
 */
int
spdk_nvme_ns_cmd_reservation_release(struct spdk_nvme_ns *ns,
				     struct spdk_nvme_qpair *qpair,
				     struct spdk_nvme_reservation_key_data *payload,
				     bool ignore_key,
				     enum spdk_nvme_reservation_release_action action,
				     enum spdk_nvme_reservation_type type,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
                                  /* [한국어] nvme_request */
	struct spdk_nvme_cmd	*cmd;
                                  /* [한국어] SQE */

	req = nvme_allocate_request_user_copy(qpair,
					      payload, sizeof(struct spdk_nvme_reservation_key_data), cb_fn,
					      cb_arg, true);
                                  /* [한국어] key data → DMA 복사 후 PRP 연결 */
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE */
	cmd->opc = SPDK_NVME_OPC_RESERVATION_RELEASE;
                                  /* [한국어] opcode 0x15 */
	cmd->nsid = ns->id;
                                  /* [한국어] namespace ID */

	cmd->cdw10_bits.resv_release.rrela = action;
                                  /* [한국어] RRELA: Release/Clear */
	cmd->cdw10_bits.resv_release.iekey = ignore_key;
                                  /* [한국어] IEKEY: 키 검증 무시 (관리자 강제 해제 시) */
	cmd->cdw10_bits.resv_release.rtype = type;
                                  /* [한국어] RTYPE: 해제할 reservation 타입 (현재 타입과 일치 필요) */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_reservation_acquire ★
 *
 * Reservation Acquire (opcode 0x11).
 * 호스트가 namespace에 대한 특정 타입의 reservation을 획득 시도.
 *
 * @action: RACQA
 *   - SPDK_NVME_RESERVE_ACQUIRE (0): 표준 획득 (기존 reservation 없어야 성공)
 *   - SPDK_NVME_RESERVE_PREEMPT (1): 기존 reservation을 prempt (기존 권리자 축출)
 *   - SPDK_NVME_RESERVE_PREEMPT_ABORT (2): preempt + 기존 권리자의 진행 중 I/O abort
 * @type: RTYPE
 *   - WRITE_EXCLUSIVE / EXCLUSIVE_ACCESS
 *   - WRITE_EXCLUSIVE_REG_ONLY / EXCLUSIVE_ACCESS_REG_ONLY
 *   - WRITE_EXCLUSIVE_ALL_REGS / EXCLUSIVE_ACCESS_ALL_REGS
 *
 * @payload: 현재 키(CRKEY) + preempt 대상 키(PRKEY)
 */
int
spdk_nvme_ns_cmd_reservation_acquire(struct spdk_nvme_ns *ns,
				     struct spdk_nvme_qpair *qpair,
				     struct spdk_nvme_reservation_acquire_data *payload,
				     bool ignore_key,
				     enum spdk_nvme_reservation_acquire_action action,
				     enum spdk_nvme_reservation_type type,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
                                  /* [한국어] nvme_request */
	struct spdk_nvme_cmd	*cmd;
                                  /* [한국어] SQE */

	req = nvme_allocate_request_user_copy(qpair,
					      payload, sizeof(struct spdk_nvme_reservation_acquire_data),
					      cb_fn, cb_arg, true);
                                  /* [한국어] acquire data (CRKEY/PRKEY) → DMA 복사본 */
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE */
	cmd->opc = SPDK_NVME_OPC_RESERVATION_ACQUIRE;
                                  /* [한국어] opcode 0x11 */
	cmd->nsid = ns->id;
                                  /* [한국어] namespace ID */

	cmd->cdw10_bits.resv_acquire.racqa = action;
                                  /* [한국어] RACQA: Acquire/Preempt/Preempt+Abort */
	cmd->cdw10_bits.resv_acquire.iekey = ignore_key;
                                  /* [한국어] IEKEY: 키 검증 무시 */
	cmd->cdw10_bits.resv_acquire.rtype = type;
                                  /* [한국어] RTYPE: 획득할 reservation 타입 */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_reservation_report ★
 *
 * Reservation Report (opcode 0x0E).
 * 현재 namespace의 reservation 상태(등록된 호스트 키 목록, 현재 holder 등)를 호스트로 읽어옴.
 *
 * 이 함수는 장치→호스트 방향 데이터이므로 user_copy의 host_to_controller=false.
 * 완료 시 복사본 → 사용자 버퍼로 자동 복사.
 *
 * @len: 바이트 단위 — 4바이트 배수여야 (DWORD 필드 크기 제약). 최대 payload 크기는
 *       등록 호스트 수에 비례 (작은 NS는 512B로 충분, 큰 클러스터는 수 KB).
 *
 * Host Identifier 크기에 따라 두 가지 반환 구조체:
 *   - 64-bit Host ID: spdk_nvme_reservation_status_data (EDS=0, 기본)
 *   - 128-bit Host ID: spdk_nvme_reservation_status_extended_data (EDS=1)
 *
 * ctratt.bits.host_id_exhid_supported 체크로 자동 EDS=1 설정.
 */
int
spdk_nvme_ns_cmd_reservation_report(struct spdk_nvme_ns *ns,
				    struct spdk_nvme_qpair *qpair,
				    void *payload, uint32_t len,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	uint32_t		num_dwords;
                                  /* [한국어] DWORD 단위 길이 (NUMD 필드로 전달) */
	struct nvme_request	*req;
                                  /* [한국어] nvme_request */
	struct spdk_nvme_cmd	*cmd;
                                  /* [한국어] SQE */

	if (len & 0x3) {
                                  /* [한국어] 4바이트 정렬 검사 (NUMD = DWORD 단위) */
		return -EINVAL;
	}

	req = nvme_allocate_request_user_copy(qpair, payload, len, cb_fn, cb_arg, false);
                                  /* [한국어] controller→host 방향 (false) — 장치가 payload에 상태 데이터 기록
                                   *         완료 시 DMA-safe 복사본에서 원래 payload로 복원 */
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE */
	cmd->opc = SPDK_NVME_OPC_RESERVATION_REPORT;
                                  /* [한국어] opcode 0x0E */
	cmd->nsid = ns->id;
                                  /* [한국어] namespace ID */

	num_dwords = (len >> 2);
                                  /* [한국어] 바이트 → DWORD 변환 */
	cmd->cdw10 = num_dwords - 1; /* 0-based */
                                  /* [한국어] NUMD(Number of DWORDs to transfer) — 0-based */

	/* Reservation Status Extended Data Structure is expected if a 128-bit Host Identifier was selected. */
	if (qpair->ctrlr->cdata.ctratt.bits.host_id_exhid_supported) {
                                  /* [한국어] 컨트롤러가 128비트 Host ID 지원이면 확장 구조체 사용 */
		cmd->cdw11_bits.resv_report.eds = 1;
                                  /* [한국어] EDS(Extended Data Structure) = 1 */
	}

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
}

/*
 * [한국어] ★★ NVMe I/O Management Receive/Send 그룹 배경 설명 ★★
 *
 * NVMe 2.0에서 추가된 범용 I/O 관리 커맨드 쌍 (TP4100/4083 등).
 * 다양한 관리 오퍼레이션을 opcode 단위로 추가하지 않고 "Management Operation" 서브타입으로 통합:
 *
 *   - I/O Management Receive (0x12): 장치→호스트 — 관리 정보 조회
 *     (FDP 활성화 상태, endurance group 정보 등)
 *   - I/O Management Send (0x1D): 호스트→장치 — 관리 동작 수행
 *     (FDP reclaim, 경로 제어 등)
 *
 * @mo: Management Operation (서브타입). NVMe 스펙의 MO 값 테이블 참조.
 * @mos: Management Operation Specific 필드 (MO별 추가 파라미터)
 *
 * 핵심 사용처:
 *   - FDP (Flexible Data Placement, TP4146): 호스트가 GC 제어하는 write
 *   - Namespace path info, reservation notification 등
 */

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_io_mgmt_recv ★
 *
 * I/O Management Receive (opcode 0x12) — 관리 정보 조회 (controller→host).
 */
int
spdk_nvme_ns_cmd_io_mgmt_recv(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      void *payload, uint32_t len, uint8_t mo, uint16_t mos,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	uint32_t		num_dwords;
                                  /* [한국어] DWORD 단위 길이 */
	struct nvme_request	*req;
                                  /* [한국어] nvme_request */
	struct spdk_nvme_cmd	*cmd;
                                  /* [한국어] SQE */

	if (len & 0x3) {
                                  /* [한국어] 4바이트 정렬 필수 */
		return -EINVAL;
	}

	req = nvme_allocate_request_user_copy(qpair, payload, len, cb_fn, cb_arg, false);
                                  /* [한국어] controller→host (false) — 장치가 관리 정보를 payload에 기록 */
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE */
	cmd->opc = SPDK_NVME_OPC_IO_MANAGEMENT_RECEIVE;
                                  /* [한국어] opcode 0x12 */
	cmd->nsid = ns->id;
                                  /* [한국어] namespace ID */

	cmd->cdw10_bits.mgmt_send_recv.mo = mo;
                                  /* [한국어] CDW10 MO: Management Operation 서브타입 */
	cmd->cdw10_bits.mgmt_send_recv.mos = mos;
                                  /* [한국어] CDW10 MOS: MO별 specific 파라미터 */

	num_dwords = (len >> 2);
                                  /* [한국어] 바이트 → DWORD */
	cmd->cdw11 = num_dwords - 1; /* 0-based */
                                  /* [한국어] CDW11 NUMD: 반환 버퍼 크기 (0-based DWORD) */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ns_cmd_io_mgmt_send ★
 *
 * I/O Management Send (opcode 0x1D) — 관리 동작 수행 (host→controller).
 *
 * Receive와 대비되는 쌍. payload가 명령 파라미터로 장치에 전달.
 * 길이 정렬 검사가 없는 점이 차이(MO에 따라 payload 크기 고정/가변 다양).
 *
 * 전형 사용처:
 *   - FDP Configuration: 호스트가 Reclaim Unit Handle 설정
 *   - Feature control: namespace 수준 관리 설정 전송
 */
int
spdk_nvme_ns_cmd_io_mgmt_send(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      void *payload, uint32_t len, uint8_t mo, uint16_t mos,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
                                  /* [한국어] nvme_request */
	struct spdk_nvme_cmd	*cmd;
                                  /* [한국어] SQE */

	req = nvme_allocate_request_user_copy(qpair, payload, len, cb_fn, cb_arg, false);
                                  /* [한국어] host→controller지만 여기서는 완료 후 복사 방향이 양방향이 될 수 있어
                                   *         false로 지정 — 일부 MO는 응답 데이터 반환
                                   *         (SPDK 구현 정책) */
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
                                  /* [한국어] SQE */
	cmd->opc = SPDK_NVME_OPC_IO_MANAGEMENT_SEND;
                                  /* [한국어] opcode 0x1D */
	cmd->nsid = ns->id;
                                  /* [한국어] namespace ID */

	cmd->cdw10_bits.mgmt_send_recv.mo = mo;
                                  /* [한국어] CDW10 MO: Management Operation */
	cmd->cdw10_bits.mgmt_send_recv.mos = mos;
                                  /* [한국어] CDW10 MOS: specific 파라미터 */

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] 제출 */
}
