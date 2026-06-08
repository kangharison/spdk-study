/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] NVMe-oF target 컨트롤러 ↔ backing bdev 어댑터 (ctrlr_bdev.c)
 *
 * === 파일의 역할 ===
 * NVMe-over-Fabrics target 의 컨트롤러(spdk_nvmf_ctrlr) 가 호스트(initiator) 로부터
 * 받은 NVMe IO/Admin 명령을 SPDK 내부의 backing block device(spdk_bdev) 호출로
 * 변환(adapter)하는 파일이다. 즉, 와이어로 들어온 NVMe SQE 의 opcode 를 분석하여
 * 해당하는 spdk_bdev_*() API (readv_blocks_ext, writev_blocks_ext, comparev_blocks,
 * comparev_and_writev_blocks, write_zeroes_blocks, flush_blocks, unmap_blocks,
 * copy_blocks, nvme_iov_passthru_md, nvme_admin_passthru, abort, zcopy_start/end 등)
 * 로 디스패치하고, 완료 시 콜백에서 spdk_bdev_io 의 상태를 NVMe CQE(SCT/SC/CDW0)로
 * 변환하여 spdk_nvmf_request_complete() 로 호스트에 회신한다.
 * Identify Namespace / Identify NVM Command Set Specific 응답 페이로드도 backing
 * bdev 의 geometry / DIF/PI 형식 / write granularity 등 메타데이터를 NVMe 스펙
 * 필드(NSZE/NCAP/LBAF/MC/DPS/NPWG/NPWA/...) 로 채워주는 빌더가 여기에 있다.
 * 추가로 PRACT/PRCHK 비트 처리, accel_sequence / memory_domain 전파, ENOMEM 시
 * spdk_bdev_queue_io_wait() 기반 재시도, fused (compare-and-write) 처리, zero-copy
 * (zcopy) 시작·종료까지 한 파일에 모여 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호스트 → (RDMA/TCP/FC transport) → lib/nvmf/transport.c → lib/nvmf/ctrlr.c
 *   → nvmf_ctrlr_process_io_cmd() / nvmf_ctrlr_process_admin_cmd() (디스패처)
 *     → 본 파일의 nvmf_bdev_ctrlr_*_cmd() (opcode 별 어댑터)
 *       → spdk_bdev_*() 공개 API (include/spdk/bdev.h)
 *         → bdev_module 의 fn_table->submit_request() (NVMe 드라이버 / lvol / raid 등)
 *           → 실제 디바이스 IO
 * 완료 경로는 역방향으로 bdev_io 콜백 nvmf_bdev_ctrlr_complete_cmd() →
 * NVMe CPL 작성 → spdk_nvmf_request_complete() → transport response.
 * 실행 컨텍스트는 모두 호스트 유저스페이스의 단일 spdk_thread (transport poll group
 * thread) 이다. 잡/스레드 affinity 가 강제되어 있어 본 파일 내부 자료구조에 별도 락이
 * 필요 없다 — 단, spdk_bdev_*() 호출 자체는 thread-local io_channel(ch)로 라우팅된다.
 *
 * === 타 모듈과의 연결 ===
 * - 입력: spdk_nvmf_request (lib/nvmf/ctrlr.c 가 채워서 전달) — req->cmd(NVMe SQE),
 *   req->iov/iovcnt(SGL 풀어진 데이터 버퍼), req->memory_domain/_ctx, req->accel_sequence,
 *   req->dif_enabled(host-side strip 여부), req->qpair->ctrlr->subsys (max_discard/zeroes
 *   사이즈, reservation_capability 설정 출처).
 * - 출력: spdk_bdev_*() 호출(include/spdk/bdev.h 의 readv_blocks_ext, writev_blocks_ext,
 *   comparev_blocks, comparev_and_writev_blocks, write_zeroes_blocks, unmap_blocks,
 *   flush_blocks, copy_blocks, zcopy_start/end, abort, nvme_iov_passthru_md,
 *   nvme_admin_passthru, queue_io_wait), 완료시 req->rsp->nvme_cpl 작성.
 * - 데이터 흐름: 호스트 SQE(64B Capsule) → req->cmd → 본 파일이 cdw10/11/12/13 파싱 →
 *   start_lba/num_blocks/check_flags 추출 → spdk_bdev_ext_io_opts.{accel_sequence,
 *   memory_domain, nvme_cdw12/13, dif_check_flags_exclude_mask} 채움 → bdev I/O 제출 →
 *   bdev_io 완료 시 spdk_bdev_io_get_nvme_status(cdw0, sct, sc) 로 CPL 변환.
 * - DIF/PI 통합: req->dif_enabled 가 true 이면 트랜스포트가 호스트→target 사이에서 PI 를
 *   strip/insert 처리한 상태이므로 PRCHK 마스크를 bdev 에 넘기지 않고, false 면 PRACT/PRCHK
 *   를 bdev_ext_io_opts.nvme_cdw12 / dif_check_flags_exclude_mask 로 그대로 전달하여
 *   bdev/accel 레이어가 검사한다.
 * - accel_sequence 통합: req->accel_sequence(상위에서 빌드된 zero-copy chain — encrypt/
 *   decrypt + CRC verify + DIX strip 등)를 bdev_ext_io_opts 에 그대로 전달, bdev/accel 이
 *   single-pass 로 합쳐서 실행 (mlx5 RDMA 등에서 와이어 ↔ DMA 무복사).
 *
 * === 주요 함수/구조체 요약 ===
 * - 헬퍼:
 *   nvmf_subsystem_bdev_io_type_supported / nvmf_ctrlr_dsm_supported /
 *   nvmf_ctrlr_write_zeroes_supported / nvmf_ctrlr_copy_supported — 서브시스템 내 모든
 *     namespace 의 backing bdev 가 특정 io_type 을 지원하는지 일괄 검사 (Identify Controller
 *     ONCS/OACS 비트 결정에 사용).
 *   nvmf_bdev_ctrlr_complete_cmd / nvmf_bdev_ctrlr_complete_admin_cmd — bdev_io 완료 콜백,
 *     spdk_bdev_io_get_nvme_status() 로 SCT/SC/CDW0 추출 → NVMe CPL 채움 → request_complete.
 *     fused 명령(compare-and-write) 의 경우 두 req(first_fused + second) 를 함께 완료 보고.
 *   nvmf_bdev_ctrlr_get_rw_params / nvmf_bdev_ctrlr_get_rw_ext_params — SQE CDW10/11(SLBA) +
 *     CDW12 NLB(0-based) 와 PRACT/PRCHK/CDW13 ELBAT 등 PI 필드 파싱.
 *   nvmf_bdev_ctrlr_lba_in_range — LBA 오버플로/끝 초과 검사.
 *   nvmf_bdev_ctrl_queue_io / nvmf_ctrlr_process_io_cmd_resubmit — bdev_io 풀 고갈(ENOMEM)
 *     시 spdk_bdev_queue_io_wait() 로 등록, 풀이 비면 콜백에서 재시도.
 * - Identify 빌더:
 *   nvmf_bdev_ctrlr_identify_ns — Identify Namespace(CNS=00h) 데이터 빌드. NSZE/NCAP/NUSE,
 *     LBAF[0]/FLBAS, MC/DPS, NACWU, NPWG/NPWA/NPDG/NPDA/NOWS, NMIC, NSRESCAP, NGUID/EUI64,
 *     MSRC/MCL/MSSRL(copy 한도) 등 채움.
 *   nvmf_bdev_ctrlr_identify_iocs_nvm — Identify NVM Command Set Specific(CNS=05h, CSI=NVM)
 *     의 16BPISTS / STCRS / ELBAF[0].STS·PIF 채움 (32/64b Guard PI 신규 포맷).
 * - I/O opcode 어댑터 (호스트 NVMe opcode → bdev_io_type 매핑):
 *   nvmf_bdev_ctrlr_read_cmd       (NVMe OPC 02h Read   → SPDK_BDEV_IO_TYPE_READ)
 *   nvmf_bdev_ctrlr_write_cmd      (NVMe OPC 01h Write  → SPDK_BDEV_IO_TYPE_WRITE)
 *   nvmf_bdev_ctrlr_compare_cmd    (NVMe OPC 05h Compare → SPDK_BDEV_IO_TYPE_COMPARE)
 *   nvmf_bdev_ctrlr_compare_and_write_cmd (FUSE bit 01b+02b → SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE)
 *   nvmf_bdev_ctrlr_write_zeroes_cmd (NVMe OPC 08h → SPDK_BDEV_IO_TYPE_WRITE_ZEROES)
 *   nvmf_bdev_ctrlr_flush_cmd      (NVMe OPC 00h Flush  → SPDK_BDEV_IO_TYPE_FLUSH)
 *   nvmf_bdev_ctrlr_dsm_cmd        (NVMe OPC 09h DSM AD → SPDK_BDEV_IO_TYPE_UNMAP, multi-range
 *                                    chunking)
 *   nvmf_bdev_ctrlr_copy_cmd       (NVMe OPC 19h Copy   → SPDK_BDEV_IO_TYPE_COPY)
 *   nvmf_bdev_ctrlr_nvme_passthru_io / spdk_nvmf_bdev_ctrlr_nvme_passthru_admin
 *                                    (모듈 고유 vendor opcode 우회 — bdev_nvme 가 직접 SQE
 *                                    포워딩)
 *   spdk_nvmf_bdev_ctrlr_abort_cmd (NVMe Admin OPC 08h Abort → spdk_bdev_abort)
 * - DIF/Zcopy:
 *   nvmf_bdev_ctrlr_get_dif_ctx — DIF context(block_size, md_size, interleaved, type, ref_tag)
 *     초기화. 트랜스포트가 자체 DIF 처리할 때 사용.
 *   nvmf_bdev_ctrlr_zcopy_start / zcopy_end — read/write 의 zero-copy populate/commit 사이클
 *     (호스트 메모리 직접 DMA — RDMA target 에서 와이어 buffer 와 bdev buffer 동일).
 *
 * === NVMe opcode → bdev_io_type 매핑 표 ===
 * NVMe Base Spec rev 2.0 §7 (NVM Command Set):
 *   OPC 0x00 Flush               → SPDK_BDEV_IO_TYPE_FLUSH         (flush_cmd)
 *   OPC 0x01 Write               → SPDK_BDEV_IO_TYPE_WRITE         (write_cmd)
 *   OPC 0x02 Read                → SPDK_BDEV_IO_TYPE_READ          (read_cmd)
 *   OPC 0x05 Compare             → SPDK_BDEV_IO_TYPE_COMPARE       (compare_cmd)
 *   FUSE=01b Compare + FUSE=10b Write (compare-and-write atomic)
 *                                → SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE
 *   OPC 0x08 Write Zeroes        → SPDK_BDEV_IO_TYPE_WRITE_ZEROES  (write_zeroes_cmd)
 *   OPC 0x09 Dataset Mgmt (AD=1) → SPDK_BDEV_IO_TYPE_UNMAP         (dsm_cmd → unmap)
 *   OPC 0x19 Copy                → SPDK_BDEV_IO_TYPE_COPY          (copy_cmd)
 * Admin Command Set (NVMe Base Spec §5):
 *   OPC 0x08 Abort               → spdk_bdev_abort()               (abort_cmd)
 *   OPC 0x06 Identify            → identify_ns / identify_iocs_nvm 페이로드 빌드
 * Vendor-specific 또는 모듈 고유 opcode → spdk_bdev_nvme_*_passthru* 로 SQE 그대로 전달.
 *
 * === DIF/PI(Protection Information) 처리 위치 ===
 * NVMe PI 비트는 SQE CDW12 의 PRACT(bit 13) + PRCHK(bit 14:12 — RefTag/ApplTag/Guard) 로
 * 표현된다. 본 파일은 두 가지 모드를 처리한다:
 *   1) req->dif_enabled == true (트랜스포트 측 strip): RDMA/TCP 가 와이어에서 이미 PI 를
 *      검사·제거했으므로 backing bdev 에는 PI 없는 데이터만 전달. set_dif_mask 인자가 false
 *      가 되어 dif_check_flags_exclude_mask 를 채우지 않는다.
 *   2) req->dif_enabled == false (target/bdev 측 처리): PRACT/PRCHK 비트를 bdev_ext_io_opts.
 *      nvme_cdw12.raw 와 dif_check_flags_exclude_mask 에 그대로 실어 보내 bdev/accel 레이어
 *      에서 strip/insert/verify. PRACT=1 이면 호스트는 PI 없는 데이터, target/장치가 생성;
 *      PRCHK 의 각 비트가 0 이면 해당 검사 "exclude" (mask 비트 SET).
 * Identify NS 에서는 spdk_bdev_get_dif_type() 결과를 DPS.PIT (TYPE1/2/3) 와 DPC.PIT1/2/3
 * 로 매핑하고, 32/64b Guard 신규 PI 포맷이면 16BPISTS/STCRS/ELBAF.STS·PIF 를 채운다.
 *
 * === ZNS Append 와 일반 Write 차이 ===
 * (※ 본 파일에는 ZNS opcode 디스패치 자체는 없음 — ZNS 는 nvmf_ctrlr_process_io_cmd 의 별도
 *  분기에서 lib/nvmf/zns.c 또는 모듈 passthru 로 처리. 본 파일은 PR/ZNS 빌더 결과의 backing
 *  bdev 호출만 수행. ZNS Zone Append(OPC 0x7D) 는 SLBA 가 zone start 만 지정하고 LBA 는 장치
 *  가 결정하여 CDW0 에 응답하므로, complete 콜백에서 cdw0 를 그대로 CPL.cdw0 에 복사하는
 *  로직이 필수 — 본 파일의 spdk_bdev_io_get_nvme_status(cdw0, ...) 는 일반 IO 에서는 0 이지만
 *  ZNS Append 에서는 할당된 LBA 를 담아 반환됨.)
 *
 * === accel_sequence 통합 ===
 * spdk_nvmf_request 가 accel_sequence(append 된 op chain — 예: decrypt + CRC verify) 를
 * 가지고 있으면 read/write_cmd 가 spdk_bdev_ext_io_opts.accel_sequence 에 그대로 전달.
 * bdev 코어가 모듈로 디스패치할 때 accel driver(예: mlx5)가 단일 sequence 로 합쳐 실행하면
 * RDMA WR 한 번에 데이터+CRC+크립토 처리 가능 (zero-copy + zero-extra-pass).
 */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 C 헤더 래퍼 — 플랫폼별 stdint.h, stddef.h, string.h 등 통일 포함. */

#include "nvmf_internal.h"
/* [한국어] lib/nvmf 내부 정의(spdk_nvmf_request, spdk_nvmf_ctrlr, spdk_nvmf_subsystem,
 *  spdk_nvmf_ns, nvmf_ctrlr_process_io_cmd 등 본 파일에서 호출/조작하는 핵심 구조체와
 *  내부 API 노출). */

#include "spdk/bdev.h"
/* [한국어] backing bdev 의 공개 API (spdk_bdev_readv_blocks_ext, spdk_bdev_get_num_blocks,
 *  spdk_bdev_io_type_supported 등). 본 파일 전체가 이 API 의 사용자(adapter)이다. */
#include "spdk/endian.h"
/* [한국어] from_le32/from_le64 — NVMe SQE 의 little-endian CDW10/11/12/13 → 호스트 endian
 *  변환에 사용. NVMe 와이어 형식이 LE 이므로 CPU 가 BE 라도 정확히 디코딩. */
#include "spdk/thread.h"
/* [한국어] spdk_io_channel — bdev I/O 제출 시 thread-local 채널 식별. 본 파일은 채널을 직접
 *  생성하지 않고 ch 파라미터로 전달받아 사용. */
#include "spdk/likely.h"
/* [한국어] spdk_likely / spdk_unlikely — 분기 예측 힌트. 일반 IO 핫패스에서 ENOMEM/오류
 *  분기를 unlikely 로 표시해 i-cache 효율 향상. */
#include "spdk/nvme.h"
/* [한국어] NVMe 스펙 상수 (SPDK_NVME_SCT_*, SPDK_NVME_SC_*, SPDK_NVME_OPC_*, PI 포맷 enum). */
#include "spdk/nvmf_cmd.h"
/* [한국어] NVMe-oF 명령 헬퍼 (spdk_nvmf_bdev_ctrlr_nvme_passthru_admin 등 외부 노출 API
 *  프로토타입 — 본 파일에서 정의). */
#include "spdk/nvmf_spec.h"
/* [한국어] NVMe-oF 와이어 스펙(Capsule, Property, Discovery Log) — Identify 빌더가 일부
 *  Fabric 구조체를 참조할 수 있어 포함. */
#include "spdk/trace.h"
/* [한국어] SPDK trace point 시스템 — 디버깅용 (현재 파일에서 직접 호출은 적지만 헤더 의존성
 *  유지). */
#include "spdk/scsi_spec.h"
/* [한국어] SCSI 상수(특히 PI/DIF 관련 비교 — NVMe DIF 설계가 SCSI T10 PI 호환). */
#include "spdk/string.h"
/* [한국어] SPDK 문자열 헬퍼(spdk_strerror 등). */
#include "spdk/util.h"
/* [한국어] SPDK_SIZEOF, spdk_min, SPDK_STATIC_ASSERT, spdk_u32log2 등 매크로. SPDK_SIZEOF 는
 *  ABI 호환을 위한 partial-struct 사이즈 계산. */

#include "spdk/log.h"
/* [한국어] SPDK_DEBUGLOG / SPDK_ERRLOG / SPDK_WARNLOG — 본 파일의 진단 로깅. */

/*
 * [한국어]
 * nvmf_subsystem_bdev_io_type_supported - 서브시스템 내 모든 namespace 의 backing bdev 가
 *                                          특정 spdk_bdev_io_type 을 지원하는지 일괄 검사.
 *
 * @subsystem: 검사 대상 NVMe-oF 서브시스템(NQN 단위 그룹).
 * @io_type:   확인할 I/O 종류 (예: SPDK_BDEV_IO_TYPE_UNMAP / WRITE_ZEROES / COPY).
 * @return:    모든 namespace 의 bdev 가 io_type 을 지원하면 true, 하나라도 미지원이면 false.
 *
 * NVMe-oF 컨트롤러는 Identify Controller 의 ONCS(Optional NVM Command Support)/OACS 비트로
 * 호스트에 지원 명령을 광고한다. SPDK 는 서브시스템에 여러 namespace 가 들어있을 수 있고
 * 각 namespace 의 backing bdev 가 다를 수 있으므로(예: nvme + lvol 혼합), 서브시스템 차원의
 * 광고는 "모든 namespace 가 동시에 지원하는" 경우에만 가능 — 이 함수는 그 AND 결과를 계산.
 *
 * 실행 컨텍스트: nvmf_ctrlr_dsm_supported / write_zeroes_supported / copy_supported 의 헬퍼로
 * Identify 빌드 또는 IO 디스패치 시 호출. 모든 호출은 admin 명령 처리 스레드(컨트롤러 소유
 * spdk_thread)에서 일어남 — 락 없음.
 *
 * 호출 체인:
 *   nvmf_ctrlr_dsm/write_zeroes/copy_supported() → [본 함수]
 *     → spdk_nvmf_subsystem_get_first_ns/_get_next_ns → spdk_bdev_io_type_supported
 */
static bool
nvmf_subsystem_bdev_io_type_supported(struct spdk_nvmf_subsystem *subsystem,
				      enum spdk_bdev_io_type io_type)
{
	struct spdk_nvmf_ns *ns;
	/* [한국어] 순회 커서 — 서브시스템에 등록된 namespace 리스트의 한 항목을 가리킴. */

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		/* [한국어] 서브시스템의 namespace 를 처음→끝까지 순회. SPDK 는 NSID 기반 정렬 리스트로
		 *  관리되며, 빈 슬롯은 NULL 로 반환하므로 명시적 NSID 인덱스가 아니라 iterator API 사용. */
		if (ns->bdev == NULL) {
			/* [한국어] namespace 가 hot-remove 되어 backing bdev 가 분리된 상태 — 검사 대상에서 제외.
			 *  (이 경우 namespace 는 활성 상태가 아님 → AnA Inaccessible 등으로 처리됨.) */
			continue;
		}

		if (!spdk_bdev_io_type_supported(ns->bdev, io_type)) {
			/* [한국어] bdev 가 해당 io_type 을 fn_table->io_type_supported 에서 false 로 보고하면
			 *  서브시스템 전체로도 광고할 수 없음 — 즉시 false 리턴. 예: lvol 은 unmap 지원하지만
			 *  ramdisk 는 미지원 → 둘이 같은 subsystem 에 있으면 unmap 미광고. */
			SPDK_DEBUGLOG(nvmf,
				      "Subsystem %s namespace %u (%s) does not support io_type %d\n",
				      spdk_nvmf_subsystem_get_nqn(subsystem),
				      ns->opts.nsid, spdk_bdev_get_name(ns->bdev), (int)io_type);
			/* [한국어] 진단 로그 — 어떤 NQN 의 어떤 NSID(어떤 bdev)가 미지원인지 출력. NVMe 호스트
			 *  드라이버가 광고와 다른 동작을 보고할 때 디버깅 단서. */
			return false;
			/* [한국어] 첫 미지원 발견 즉시 리턴 — short-circuit AND. */
		}
	}

	SPDK_DEBUGLOG(nvmf, "All devices in Subsystem %s support io_type %d\n",
		      spdk_nvmf_subsystem_get_nqn(subsystem), (int)io_type);
	/* [한국어] 모든 ns 통과 — 서브시스템이 io_type 을 지원함을 로깅. */
	return true;
	/* [한국어] 빈 서브시스템(ns 없음)도 true — 호스트에 명령 광고는 가능하지만 실제로는 호스트가
	 *  대상 NSID 를 못 찾음. 광고/실행 분리. */
}

/*
 * [한국어]
 * nvmf_ctrlr_dsm_supported - 컨트롤러가 Dataset Management(DSM, OPC 0x09) 의 Deallocate(AD)
 *                             를 지원하는지 — 즉 unmap 가능한지 — 서브시스템 단위로 결정.
 *
 * @ctrlr:  검사 대상 spdk_nvmf_ctrlr (호스트 connect 로 만들어진 컨트롤러).
 * @return: 모든 ns 의 bdev 가 SPDK_BDEV_IO_TYPE_UNMAP 지원이면 true.
 *
 * Identify Controller(CNS=01h) 의 ONCS bit 2(Dataset Management) 결정에 사용. 호스트는 이 비트
 * 를 보고 DSM AD(thin-provisioning trim/discard) 명령 사용 여부를 정한다.
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_identify_controller() → [본 함수] → nvmf_subsystem_bdev_io_type_supported
 */
bool
nvmf_ctrlr_dsm_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return nvmf_subsystem_bdev_io_type_supported(ctrlr->subsys, SPDK_BDEV_IO_TYPE_UNMAP);
	/* [한국어] DSM AD → bdev unmap 으로 매핑되므로 io_type 도 UNMAP 으로 검사. */
}

/*
 * [한국어]
 * nvmf_ctrlr_write_zeroes_supported - Write Zeroes(OPC 0x08) 지원 여부 결정.
 *
 * @ctrlr:  검사 대상 컨트롤러.
 * @return: 모든 ns 의 bdev 가 WRITE_ZEROES 지원이면 true.
 *
 * Identify Controller ONCS bit 3 결정에 사용. 호스트는 이 비트로 큰 영역 zero-fill 시 데이터
 * 전송 없는 경량 명령 사용 가능 여부를 판단.
 */
bool
nvmf_ctrlr_write_zeroes_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return nvmf_subsystem_bdev_io_type_supported(ctrlr->subsys, SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
	/* [한국어] NVMe Write Zeroes ↔ bdev WRITE_ZEROES 직접 매핑. */
}

/*
 * [한국어]
 * nvmf_ctrlr_copy_supported - Copy(OPC 0x19) 지원 여부 결정.
 *
 * @ctrlr:  검사 대상 컨트롤러.
 * @return: 모든 ns 의 bdev 가 COPY 지원이면 true.
 *
 * Identify Controller ONCS bit 8(Copy) 결정에 사용. SCC(Simple Copy Command) — 호스트 데이터
 * 전송 없이 장치 내부에서 src LBA → dst LBA 복사. NVMe Base Spec rev 2.0 §7.6.
 */
bool
nvmf_ctrlr_copy_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return nvmf_subsystem_bdev_io_type_supported(ctrlr->subsys, SPDK_BDEV_IO_TYPE_COPY);
	/* [한국어] NVMe Copy ↔ bdev COPY (offload internal copy) 직접 매핑. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_complete_cmd - bdev_io 완료 → NVMe CPL(Completion Queue Entry) 변환·전송.
 *
 * @bdev_io: 완료된 spdk_bdev_io (bdev 코어가 콜백 호출 전 필드 채움).
 * @success: 완료 성공 여부 (bdev_io->internal.status == SUCCESS 와 동치).
 * @cb_arg:  spdk_bdev_*() 호출 시 등록한 spdk_nvmf_request 포인터.
 *
 * 모든 일반 IO 명령(read/write/compare/write_zeroes/flush/copy/dsm/passthru) 의 bdev_io 완료
 * 콜백이다. spdk_bdev_io_get_nvme_status() 로 SC/SCT/CDW0 를 추출해 req->rsp->nvme_cpl 에
 * 채우고, 트랜스포트(RDMA/TCP)를 통해 호스트에 CQE 전송하도록 spdk_nvmf_request_complete()
 * 호출. 마지막으로 bdev_io 를 풀에 반납(spdk_bdev_free_io). NVMe Base Spec §4.6 의 CQE
 * 포맷(SCT bits 28:25, SC bits 24:17, CDW0 = 명령별 결과 — 예: ZNS Append 의 할당된 LBA).
 *
 * Fused 명령(compare-and-write atomic) 처리:
 *   호스트가 두 개의 SQE 를 FUSE bits(CDW0의 fuse field) 01b/10b 조합으로 보내면 target 은 두
 *   요청을 묶어 atomic compare-and-write 로 처리. 이 경우 bdev 는 두 번째 명령(write) 의
 *   완료에서 두 결과를 함께 보고 — spdk_bdev_io_get_nvme_fused_status() 가 두 SCT/SC 쌍을
 *   동시 반환. 본 함수는 first_fused_req(compare 부분) 의 CPL 도 채워 별도 호출로 완료 보고.
 *
 * 실행 컨텍스트: bdev 모듈의 IO 완료 시점 — 모듈은 동일 spdk_thread 에서 콜백 실행. transport
 * thread 와 동일 스레드라면 즉시 wire 전송, 다른 스레드라면 spdk_thread_send_msg 로 전달
 * (transport 가 처리). 본 함수는 thread-affinity 보장된 호출자만 가정.
 *
 * 호출 체인:
 *   bdev_module IO 완료 → spdk_bdev_io_complete → 본 함수
 *     → spdk_nvmf_request_complete → transport->ops.req_complete → wire CQE
 */
static void
nvmf_bdev_ctrlr_complete_cmd(struct spdk_bdev_io *bdev_io, bool success,
			     void *cb_arg)
{
	struct spdk_nvmf_request	*req = cb_arg;
	/* [한국어] cb_arg 로 등록된 NVMe-oF 요청 포인터 — req->cmd(SQE), req->rsp(CPL 빈 슬롯),
	 *  req->qpair 등 호스트 ↔ target 에 필요한 모든 컨텍스트가 여기에. */
	struct spdk_nvme_cpl		*response = &req->rsp->nvme_cpl;
	/* [한국어] CPL 작성 대상 — req->rsp 는 트랜스포트가 사전 할당해 둔 CQE 슬롯. */
	int				sc = 0, sct = 0;
	/* [한국어] Status Code(8b) / Status Code Type(3b) — NVMe 완료 상태. NVMe Base Spec §4.6.1.
	 *  default 0/0 = SCT_GENERIC + SC_SUCCESS. */
	uint32_t			cdw0 = 0;
	/* [한국어] CPL DW0 (CDW0) — 명령별 결과. read/write 는 0, ZNS Append 는 할당된 LBA, Get
	 *  Features 는 feature value 등. bdev_io 가 모듈에서 받은 그대로 전달. */

	if (spdk_unlikely(req->first_fused)) {
		/* [한국어] 이 req 가 fused pair 의 두 번째 명령(write 부분) 인 경우 — 동시에 첫 번째
		 *  명령(compare) 의 완료도 보고해야 함. unlikely: fused 사용 비율은 매우 낮음. */
		struct spdk_nvmf_request	*first_req = req->first_fused_req;
		/* [한국어] 짝이 되는 첫 번째 요청(compare 부분). ctrlr.c 가 두 번째 SQE 도착 시 페어링. */
		struct spdk_nvme_cpl		*first_response = &first_req->rsp->nvme_cpl;
		/* [한국어] 첫 번째 요청의 CPL 슬롯 — 별도 트랜스포트 응답 대상. */
		int				first_sc = 0, first_sct = 0;
		/* [한국어] 첫 번째 명령(compare) 의 SC/SCT — 두 번째와 분리되어야 하는데 bdev 는 한 번의
		 *  bdev_io 완료에서 두 쌍을 함께 보고. */

		/* get status for both operations */
		spdk_bdev_io_get_nvme_fused_status(bdev_io, &cdw0, &first_sct, &first_sc, &sct, &sc);
		/* [한국어] bdev 가 fused 결과를 분해 — first(compare) 결과와 second(write) 결과를 별도
		 *  SCT/SC 로 반환. 예: compare 미스매치 시 first_sc=COMPARE_FAILURE, sc=ABORTED_BY_REQ. */
		first_response->cdw0 = cdw0;
		/* [한국어] CDW0 는 두 명령 공유(fused 는 보통 0). */
		first_response->status.sc = first_sc;
		first_response->status.sct = first_sct;
		/* [한국어] 첫 번째 CPL 의 SCT/SC 채움. */

		/* first request should be completed */
		spdk_nvmf_request_complete(first_req);
		/* [한국어] 첫 번째 요청을 트랜스포트에 보고 → 호스트는 compare 명령에 대한 CQE 수신.
		 *  (두 번째 요청의 CQE 는 본 함수 끝에서 별도로 보고.) */
		req->first_fused_req = NULL;
		req->first_fused = false;
		/* [한국어] 페어링 해제 — 다음 req 재사용 시 fused 잔여 상태가 남지 않도록. */
	} else {
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);
		/* [한국어] 일반 IO — SC/SCT/CDW0 단일 추출. bdev 모듈이 NVMe 드라이버일 때는 장치 CQE
		 *  의 raw 값, lvol/raid 등은 bdev 코어가 generic 매핑한 값. */
	}

	response->cdw0 = cdw0;
	response->status.sc = sc;
	response->status.sct = sct;
	/* [한국어] 본 요청의 CPL 슬롯에 결과 기록. dnr/m/p/crd 비트는 default 0 — 필요 시 호출자가
	 *  사전에 채워 둠. */

	spdk_nvmf_request_complete(req);
	/* [한국어] 트랜스포트로 CQE 전송 위임 (RDMA SEND / TCP CqeAndOptionalData PDU 등). */
	spdk_bdev_free_io(bdev_io);
	/* [한국어] bdev_io 객체를 채널 mempool 로 반납. 누락 시 풀 고갈 → ENOMEM 연쇄. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_complete_admin_cmd - admin passthru 명령의 bdev_io 완료 콜백.
 *
 * @bdev_io: 완료된 spdk_bdev_io.
 * @success: 성공 여부.
 * @cb_arg:  spdk_nvmf_request 포인터.
 *
 * spdk_nvmf_bdev_ctrlr_nvme_passthru_admin() 이 등록한 콜백. admin 명령은 응답 페이로드를
 * 호스트에 회신하기 전에 추가 후처리(예: identify 응답에 컨트롤러 측 필드 보강)가 필요할 수
 * 있어 req->cmd_cb_fn 후처리 훅을 먼저 실행한 뒤 일반 complete 경로로 위임.
 *
 * 호출 체인:
 *   bdev_io 완료 → 본 함수 → req->cmd_cb_fn(req) → nvmf_bdev_ctrlr_complete_cmd
 */
static void
nvmf_bdev_ctrlr_complete_admin_cmd(struct spdk_bdev_io *bdev_io, bool success,
				   void *cb_arg)
{
	struct spdk_nvmf_request *req = cb_arg;
	/* [한국어] 콜백 컨텍스트 — admin passthru 호출 시 등록한 nvmf_request. */

	if (req->cmd_cb_fn) {
		/* [한국어] 후처리 훅 등록되어 있으면 먼저 실행. 예: spdk_nvmf_bdev_ctrlr_nvme_passthru_admin
		 *  이 호출 시 spdk_nvmf_nvme_passthru_cmd_cb 를 cmd_cb_fn 에 저장 — 응답 데이터에 SPDK 측
		 *  필드(컨트롤러 NN, MN/SN 등)를 덮어쓰기 가능. */
		req->cmd_cb_fn(req);
	}

	nvmf_bdev_ctrlr_complete_cmd(bdev_io, success, req);
	/* [한국어] 일반 IO 와 동일한 CPL 변환 + request_complete + bdev_free_io 경로 재사용. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_identify_ns - Identify Namespace(CNS=00h) 응답 페이로드(spdk_nvme_ns_data,
 *                                4096B) 를 backing bdev 의 메타데이터로 채움.
 *
 * @ns:                    NVMe-oF namespace (백엔드 bdev 와 NSID/NGUID/EUI64 등 매핑 정보).
 * @nsdata:                호스트에 보낼 4KB Identify NS 데이터 버퍼 — 호출자가 0 초기화 후 전달.
 * @dif_insert_or_strip:   true 면 트랜스포트가 DIF/PI 를 insert/strip 처리(호스트는 PI 비노출),
 *                         false 면 호스트가 PI 를 직접 다룸 — Identify 의 LBAF[0].ms / FLBAS /
 *                         MC / DPS 를 어떻게 채울지 분기.
 * @transport_max_io_size: 트랜스포트(RDMA/TCP)별 단일 IO 최대 크기(byte). NOWS/NOIOB 의 상한.
 * @return:                없음 — nsdata 출력 버퍼에 직접 기록.
 *
 * NVMe Base Spec rev 2.0 §5.17.2.1.1 (Identify Namespace data structure) 의 모든 핵심 필드를
 * backing bdev 의 geometry/DIF/write granularity getter 로부터 추출해 채운다.
 *   - NSZE/NCAP/NUSE: spdk_bdev_get_num_blocks (LBA 단위 크기, thin-provisioning 무시 단순화)
 *   - NLBAF/FLBAS/LBAF[0]: 단일 LBA 포맷만 지원 (MS=메타사이즈, LBADS=log2(block_size))
 *   - DPS/DPC/MC: DIF Type 1/2/3 + extended metadata
 *   - NACWU: bdev 의 atomic compare-and-write unit (0-based)
 *   - NPWG/NPWA/NPDG/NPDA/NOWS: 권장 write/unmap granularity·alignment, optimal write size
 *     (모두 0-based — NVMe 스펙)
 *   - NOIOB: optimal IO boundary (lvol stripe, RAID strip 등)
 *   - NMIC.SHRNS=1: namespace 가 multi-controller shared 가능 (NVMe-oF 는 multi-host 지원)
 *   - NSRESCAP: reservation capability (Persistent Reservation)
 *   - NGUID/EUI64: namespace 고유 식별자 (host-side dm-multipath 가 사용)
 *   - MSRC/MCL/MSSRL: Copy 명령 한도 (1 source range, max copy length, max single source range)
 *
 * 실행 컨텍스트: ctrlr.c 의 admin command 처리 (Identify) 시점 — 컨트롤러 소유 spdk_thread.
 * bdev 는 즉시 동기 getter 로만 접근 (IO 제출 없음).
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_identify_ns_iocs_specific or generic identify
 *     → [본 함수] → spdk_bdev_get_*() / spdk_bdev_desc_get_*() 동기 getter 다수
 */
void
nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata,
			    bool dif_insert_or_strip, uint32_t transport_max_io_size)
{
	struct spdk_bdev *bdev = ns->bdev;
	/* [한국어] backing bdev 인스턴스 — geometry/feature getter 의 인자. */
	struct spdk_bdev_desc *desc = ns->desc;
	/* [한국어] bdev descriptor (open 결과) — desc-level getter (DIF mask 적용된 view) 사용. */
	uint64_t num_blocks;
	/* [한국어] bdev 의 총 LBA 수 — NSZE/NCAP/NUSE 공통값. */
	uint32_t phys_blocklen;
	/* [한국어] 물리 블록 크기(byte) — NPWG 미지정 시 fallback 으로 사용. */
	uint32_t max_num_blocks;
	/* [한국어] transport_max_io_size 를 LBA 단위로 변환한 값 — NOWS/NOIOB 상한. */
	uint32_t max_copy;
	/* [한국어] bdev 의 max_copy (한 번의 Copy 명령으로 복사 가능한 최대 LBA) — MCL/MSSRL 결정. */
	uint32_t npwa, npwg, npda, npdg;
	/* [한국어] preferred write alignment/granularity, preferred unmap alignment/granularity —
	 *  bdev 가 advertise. 0 이면 fallback. */

	num_blocks = spdk_bdev_get_num_blocks(bdev);
	/* [한국어] bdev 의 LBA 수 조회 (geometry). */

	nsdata->nsze = num_blocks;
	/* [한국어] Namespace Size — 광고하는 namespace 크기(LBA). */
	nsdata->ncap = num_blocks;
	/* [한국어] Namespace Capacity — 할당 가능한 최대 LBA 수 (thin-provision 미고려, NSZE=NCAP). */
	nsdata->nuse = num_blocks;
	/* [한국어] Namespace Utilization — 현재 사용 LBA 수. SPDK 단순화로 NSZE 와 동일 (장치별
	 *  thin-provisioning 정확 보고는 미지원). */
	nsdata->nlbaf = 0;
	/* [한국어] Number of LBA Formats - 1 — 0 = 단일 포맷만 광고 (LBAF[0] 만 유효). */
	nsdata->flbas.format = 0;
	/* [한국어] Formatted LBA Size — 사용 중인 LBAF index (LBAF[0] 사용). */
	nsdata->flbas.msb_format = 0;
	/* [한국어] FLBAS upper bits — 단일 포맷이므로 0. */
	nsdata->nacwu = spdk_bdev_get_acwu(bdev) - 1; /* nacwu is 0-based */
	/* [한국어] Namespace Atomic Compare & Write Unit — bdev 의 acwu(1-based) 를 0-based 로 변환.
	 *  호스트가 fused compare-and-write 시 단일 atomic 단위. */
	if (!dif_insert_or_strip) {
		/* [한국어] 트랜스포트가 PI 를 변환하지 않음 → 호스트가 PI 를 와이어 그대로 봄. LBAF[0] 의
		 *  MS(메타사이즈)/LBADS 와 DPS/DPC 를 모두 채워야 함. */
		nsdata->lbaf[0].ms = spdk_bdev_desc_get_md_size(desc);
		/* [한국어] Metadata Size (byte) — 0 이면 PI 없음, 8/16 등이면 PI 포함. */
		nsdata->lbaf[0].lbads = spdk_u32log2(spdk_bdev_desc_get_block_size(desc));
		/* [한국어] LBA Data Size = log2(block_size). 예: 512B → 9, 4KB → 12. NVMe 스펙은 log2 형식. */
		if (nsdata->lbaf[0].ms != 0) {
			/* [한국어] PI 가 있으면 — extended LBA 형태(데이터+메타 인터리브) 또는 separate 메타 */
			nsdata->flbas.extended = 1;
			/* [한국어] Extended LBA — 데이터 다음에 메타 인접 배치 (interleaved). SPDK bdev 모델
			 *  이 separate metadata 도 지원하지만 NVMe-oF 광고는 extended 우선. */
			nsdata->mc.extended = 1;
			nsdata->mc.pointer = 0;
			/* [한국어] Metadata Capabilities — extended 형식 지원, separate pointer 미지원 광고. */
			nsdata->dps.md_start = spdk_bdev_desc_is_dif_head_of_md(desc);
			/* [한국어] DPS.MD_START — PI 가 메타 영역의 시작에 위치하는지(head) 끝에 위치하는지(tail).
			 *  bdev 가 head 면 1. NVMe Base Spec §5.17.2.1.1 DPS field. */

			switch (spdk_bdev_get_dif_type(bdev)) {
			case SPDK_DIF_TYPE1:
				nsdata->dpc.pit1 = 1;
				nsdata->dps.pit = SPDK_NVME_FMT_NVM_PROTECTION_TYPE1;
				/* [한국어] DIF Type 1 — Guard + ApplTag + RefTag(논리 LBA 와 일치). NVMe 스펙 §8.2. */
				break;
			case SPDK_DIF_TYPE2:
				nsdata->dpc.pit2 = 1;
				nsdata->dps.pit = SPDK_NVME_FMT_NVM_PROTECTION_TYPE2;
				/* [한국어] DIF Type 2 — Guard + ApplTag + RefTag(호스트 임의 시작값). */
				break;
			case SPDK_DIF_TYPE3:
				nsdata->dpc.pit3 = 1;
				nsdata->dps.pit = SPDK_NVME_FMT_NVM_PROTECTION_TYPE3;
				/* [한국어] DIF Type 3 — Guard 만 검사, ApplTag/RefTag 무시. */
				break;
			default:
				SPDK_DEBUGLOG(nvmf, "Protection Disabled\n");
				nsdata->dps.pit = SPDK_NVME_FMT_NVM_PROTECTION_DISABLE;
				/* [한국어] PI 없음 — bdev 가 PI type 미지정 (예: ramdisk). */
				break;
			}
		}
	} else {
		/* [한국어] 트랜스포트가 PI 를 strip/insert 하므로 호스트는 PI 없는 데이터만 봄. LBAF.MS=0
		 *  + LBADS = log2(data_block_size — 메타 제외 순수 데이터 크기) 로 광고. */
		nsdata->lbaf[0].ms = 0;
		nsdata->lbaf[0].lbads = spdk_u32log2(spdk_bdev_get_data_block_size(bdev));
		/* [한국어] data_block_size = block_size - md_size (메타 분리). 호스트 입장에서의 LBA 크기. */
	}

	max_num_blocks = transport_max_io_size / (1U << nsdata->lbaf[0].lbads);
	/* [한국어] 트랜스포트 최대 IO 크기를 블록 수로 변환 — NOWS/NOIOB 상한 계산용. */

	phys_blocklen = spdk_bdev_get_physical_block_size(bdev);
	/* [한국어] 물리 블록 크기 (논리 블록의 정수배) — 4KB AF(Advanced Format) 디스크 등. */
	assert(phys_blocklen > 0);
	/* [한국어] bdev contract — 0 인 phys block 은 잘못된 모듈 등록. */
	/* Linux driver uses min(nawupf, npwg) to set physical_block_size */
	nsdata->nsfeat.optperf = 1;
	/* [한국어] NSFEAT.OPTPERF — 최적 성능 필드(NPWG/NPWA/NPDG/NPDA/NOWS) 가 유효함을 광고. */
	nsdata->nsfeat.ns_atomic_write_unit = 1;
	/* [한국어] NSFEAT.NS_AWU — namespace 고유 atomic write unit(NAWUN/NAWUPF) 가 컨트롤러 광고
	 *  값(AWUN)과 다를 수 있음을 표시. */
	/* Note that all of these preferred and optimal sizes are 0-based. */
	npwg = spdk_bdev_get_preferred_write_granularity(bdev);
	/* [한국어] NPWG (Namespace Preferred Write Granularity) — write 시 최소 권장 단위. */
	if (npwg == 0) {
		/* [한국어] bdev 가 명시 안 함 → 물리 블록 / 논리 블록 비율로 fallback. */
		nsdata->npwg = (phys_blocklen >> nsdata->lbaf[0].lbads) - 1;
	} else {
		nsdata->npwg = npwg - 1;
		/* [한국어] 0-based 변환. */
	}
	nsdata->nawupf = nsdata->npwg;
	/* [한국어] NAWUPF (Namespace Atomic Write Unit Power Fail) — 정전 시에도 atomic 보장 단위.
	 *  단순화로 NPWG 와 동일하게 광고. */
	npwa = spdk_bdev_get_preferred_write_alignment(bdev);
	/* [한국어] NPWA — write 시 LBA 정렬 권장. */
	if (npwa == 0) {
		nsdata->npwa = nsdata->npwg;
		/* [한국어] 미지정 시 NPWG 와 동일. */
	} else {
		nsdata->npwa = npwa - 1;
	}
	npdg = spdk_bdev_get_preferred_unmap_granularity(bdev);
	/* [한국어] NPDG — unmap(deallocate) 권장 단위. SSD 의 zone/erase block 크기에 맞추면 유리. */
	if (npdg == 0) {
		nsdata->npdg = nsdata->npwg;
	} else {
		nsdata->npdg = npdg - 1;
	}
	npda = spdk_bdev_get_preferred_unmap_alignment(bdev);
	/* [한국어] NPDA — unmap LBA 정렬 권장. */
	if (npda == 0) {
		nsdata->npda = nsdata->npwg;
	} else {
		nsdata->npda = npda - 1;
	}
	nsdata->nows = spdk_bdev_get_optimal_write_size(bdev);
	/* [한국어] NOWS (Namespace Optimal Write Size) — RAID stripe / lvol cluster 크기 등. */
	if (nsdata->nows > 0) {
		nsdata->nows -= 1;
		/* [한국어] 0-based 변환. */
	} else {
		/* Set NOWS equal to controller MDTS if the namespace did
		 * not set one explicitly.
		 */
		nsdata->nows = max_num_blocks - 1;
		/* [한국어] 미지정 시 컨트롤러 MDTS(Maximum Data Transfer Size) 만큼 광고. */
	}

	if (spdk_bdev_get_write_unit_size(bdev) == 1) {
		/* [한국어] write_unit_size=1 (모든 LBA 가 독립적으로 쓰기 가능, ZNS 같은 sequential 제약
		 *  없음) 인 경우에만 NOIOB 광고. ZNS 같이 zone 단위 제약이 있으면 NOIOB 가 의미 없음. */
		/* Due to bug in the Linux kernel NVMe driver, we have to set noiob no larger
		 * than mdts.
		 */
		nsdata->noiob = spdk_min(spdk_bdev_get_optimal_io_boundary(bdev), max_num_blocks);
		/* [한국어] NOIOB (Namespace Optimal IO Boundary) — IO 가 이 경계를 넘으면 성능 저하 가능성.
		 *  Linux 커널 드라이버 버그로 MDTS 초과 값을 거부하므로 spdk_min 으로 클램프. */
	}
	nsdata->nmic.shrns = 1;
	/* [한국어] NMIC.SHRNS — namespace 가 multi-controller shared 가능 (NVMe-oF target 은 여러
	 *  호스트가 동일 NQN+NSID 에 connect 가능). */
	nsdata->nsrescap.rescap = nvmf_ns_get_rescap(ns);
	/* [한국어] NSRESCAP — Persistent Reservation 지원 capability 비트맵 (write_exclusive,
	 *  exclusive_access, write_excl_reg_only 등 NVMe Base Spec §8.19). nvmf_ns_get_rescap 은
	 *  ns->opts 에서 광고 비트 산출. */

	SPDK_STATIC_ASSERT(sizeof(nsdata->nguid) == sizeof(ns->opts.nguid), "size mismatch");
	/* [한국어] 컴파일타임 sanity — NVMe NGUID(16B) 와 SPDK 내부 표현 크기 일치 확인. */
	memcpy(nsdata->nguid, ns->opts.nguid, sizeof(nsdata->nguid));
	/* [한국어] NGUID (Namespace Globally Unique Identifier, 16B) — 호스트 multipath 가 동일 ns
	 *  를 다른 컨트롤러로도 식별. ns->opts 에서 RPC/구성으로 입력. */

	SPDK_STATIC_ASSERT(sizeof(nsdata->eui64) == sizeof(ns->opts.eui64), "size mismatch");
	memcpy(&nsdata->eui64, ns->opts.eui64, sizeof(nsdata->eui64));
	/* [한국어] EUI-64 (8B) — 레거시 IEEE 식별자. NGUID 가 0 이면 EUI64 사용. */

	/* For now we support just one source range for copy command */
	nsdata->msrc = 0;
	/* [한국어] MSRC (Maximum Source Range Count) — Copy 명령에서 허용하는 source range 수 - 1.
	 *  0 = 1 개 range 만 지원 (단순 src→dst 1:1 복사). */

	max_copy = spdk_bdev_get_max_copy(bdev);
	/* [한국어] bdev 의 최대 copy 길이 (LBA). 0 = 무제한. */
	if (max_copy == 0 || max_copy > UINT16_MAX) {
		/* Zero means copy size is unlimited */
		nsdata->mcl = UINT16_MAX;
		nsdata->mssrl = UINT16_MAX;
		/* [한국어] MCL (Max Copy Length) / MSSRL (Max Single Source Range Length) — 16-bit 광고
		 *  필드 한계로 65535 LBA 캡. */
	} else {
		nsdata->mcl = max_copy;
		nsdata->mssrl = max_copy;
		/* [한국어] bdev 한도 그대로 광고. */
	}
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_identify_iocs_nvm - Identify NVM Command Set Specific(CNS=05h, CSI=NVM)
 *                                      응답(spdk_nvme_nvm_ns_data) 의 PI 포맷 관련 필드를
 *                                      backing bdev 의 DIF/PI 형식 정보로 채움.
 *
 * @ns:         NVMe-oF namespace (backing bdev descriptor 보유).
 * @nsdata_nvm: 호출자가 0 초기화해 전달하는 NVM I/O Command Set Identify NS 페이로드.
 * @return:     없음 — nsdata_nvm 출력 버퍼에 직접 기록.
 *
 * NVMe TP-4068 / Base Spec rev 2.0 §5.17.2.1.2 가 정의한 32b/64b Guard PI 신규 포맷을
 * 호스트에 광고. 16b Guard 면 본 함수가 채울 내용이 거의 없고, 32b/64b 면 16BPISTS=1,
 * ELBAF[0].STS·PIF 를 설정해야 호스트가 새로운 PI 포맷을 인식.
 *   - 16BPISTS (16-byte PI Storage Tag Support) — 1 = 16B PI 사용(32/64b Guard) 가능 광고.
 *   - STCRS (Storage Tag Check Required Sources) — 0 = host 에서만 검사, 1 = 모든 소스에서 강제.
 *   - ELBAF[0].STS — Storage Tag Size(bit 단위). 32b Guard 는 최소 16, 64b Guard 는 0.
 *   - ELBAF[0].PIF — Protection Information Format (16b/32b/64b Guard).
 *   - LBSTM (LBA Storage Tag Mask) — Storage Tag 의 검사 마스크. SPDK 단순화로 0.
 *   - 16BPISTM — 16B PI Storage Tag Mask Support. SPDK 단순화로 0.
 *
 * 실행 컨텍스트: ctrlr.c 의 Identify(CNS=05h CSI=NVM) admin 처리 — 컨트롤러 spdk_thread.
 * bdev 동기 getter 만 호출, IO 제출 없음.
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_identify_ns_iocs_specific(CSI=NVM)
 *     → [본 함수] → spdk_bdev_desc_get_dif_type / spdk_bdev_desc_get_dif_pi_format
 */
void
nvmf_bdev_ctrlr_identify_iocs_nvm(struct spdk_nvmf_ns *ns,
				  struct spdk_nvme_nvm_ns_data *nsdata_nvm)
{
	struct spdk_bdev_desc *desc = ns->desc;
	/* [한국어] backing bdev descriptor — DIF type/format getter 의 인자. */
	uint8_t _16bpists;
	/* [한국어] 16BPISTS 비트 결정값 — 32/64b Guard 면 1, 16b Guard 면 0. underscore prefix 는
	 *  C 식별자가 숫자로 시작 못함 회피. */
	uint32_t sts, pif;
	/* [한국어] sts: Storage Tag Size (bit), pif: PI Format enum (SPDK_DIF_PI_FORMAT_*).
	 *  ELBAF[0] 의 STS/PIF 필드로 그대로 들어감. */

	if (spdk_bdev_desc_get_dif_type(desc) == SPDK_DIF_DISABLE) {
		/* [한국어] PI 비활성화 ns 면 NVM IOCS 의 PI 관련 필드를 채울 게 없음. 호스트는 LBAF.MS=0
		 *  으로 PI 없음을 이미 인식. 즉시 반환. */
		return;
	}

	pif = spdk_bdev_desc_get_dif_pi_format(desc);
	/* [한국어] bdev 가 광고하는 PI 포맷 enum 조회 (16b/32b/64b Guard 중 하나). */

	/*
	 * 16BPISTS shall be 1 for 32/64b Guard PI.
	 * STCRS shall be 1 if 16BPISTS is 1.
	 * 16 is the minimum value of STS for 32b Guard PI.
	 */
	/* [한국어] NVMe TP-4068: 32/64b Guard 신규 PI 포맷은 PI 슬롯이 16B 로 확장됨. 16BPISTS=1 광고
	 *  필수. STS 는 32b 면 최소 16bit, 64b 면 0(Guard 가 64b 다 사용). */
	switch (pif) {
	case SPDK_DIF_PI_FORMAT_16:
		_16bpists = 0;
		sts = 0;
		/* [한국어] 16b Guard (legacy) — 8B PI 슬롯, 신규 필드 비활성. */
		break;
	case SPDK_DIF_PI_FORMAT_32:
		_16bpists = 1;
		sts = 16;
		/* [한국어] 32b Guard — 16B PI, Storage Tag 16bit 사용 (RefTag 32b + Guard 32b + ApplTag 16b
		 *  + StorageTag 16b 합산). */
		break;
	case SPDK_DIF_PI_FORMAT_64:
		_16bpists = 1;
		sts = 0;
		/* [한국어] 64b Guard — 16B PI, Guard 가 64bit 차지하므로 Storage Tag 영역 없음(STS=0). */
		break;
	default:
		SPDK_WARNLOG("PI format %u is not supported\n", pif);
		/* [한국어] SPDK 가 모르는 신규 포맷 — 안전하게 NOP 으로 반환. */
		return;
	}

	/* For 16b Guard PI, Storage Tag is not available because we set STS to 0.
	 * In this case, we do not have to set 16BPISTM to 1. For simplicity,
	 * set 16BPISTM to 0 and set LBSTM to all zeroes.
	 *
	 * We will revisit here when we find any OS uses Storage Tag.
	 */
	nsdata_nvm->lbstm = 0;
	/* [한국어] LBA Storage Tag Mask — Storage Tag 검사 시 호스트가 마스킹할 비트맵. SPDK 는 Storage
	 *  Tag 활용 OS 가 없는 현재까지 0 (전체 검사) 으로 단순화. */
	nsdata_nvm->pic._16bpistm = 0;
	/* [한국어] 16B PI Storage Tag Mask Support — Storage Tag 마스크 변경 가능 여부. 현재 미지원
	 *  광고로 0. */

	nsdata_nvm->pic._16bpists = _16bpists;
	/* [한국어] 16B PI Storage Tag Support — 32/64b Guard 면 1. 호스트 드라이버가 16B PI 슬롯 파싱
	 *  활성화 신호. */
	nsdata_nvm->pic.stcrs = 0;
	/* [한국어] Storage Tag Check Required Sources — 0 = host-only 검사. SPDK target 은 별도 강제
	 *  소스 없음. */
	nsdata_nvm->elbaf[0].sts = sts;
	/* [한국어] Extended LBA Format[0] 의 Storage Tag Size — 위 switch 에서 결정. */
	nsdata_nvm->elbaf[0].pif = pif;
	/* [한국어] Extended LBA Format[0] 의 PI Format — 16b/32b/64b Guard 중 하나. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_get_rw_params - NVMe Read/Write SQE 의 CDW10/11 (SLBA) + CDW12 (NLB) 를
 *                                  파싱해 호출자에게 64-bit start_lba 와 num_blocks 로 반환.
 *
 * @cmd:        호스트가 보낸 NVMe SQE (네트워크 capsule 에서 추출된 것, little-endian).
 * @start_lba:  [out] CDW10|CDW11 합본의 64-bit 시작 LBA.
 * @num_blocks: [out] (CDW12[15:0] + 1) — NVMe 의 NLB 는 0-based 이므로 +1 보정.
 * @return:     없음.
 *
 * NVMe Base Spec rev 2.0 §7 NVM Command Set 의 Read(02h)/Write(01h) 명령 SQE 포맷:
 *   CDW10 = SLBA[31:0], CDW11 = SLBA[63:32], CDW12.NLB[15:0] = (전송 블록 수 - 1).
 * 본 헬퍼는 read/write/compare/zcopy_start 등에서 공통 사용.
 *
 * 실행 컨텍스트: NVMe-oF target IO 핫패스의 모든 dispatch 함수에서 호출 — 단일 transport
 * thread 컨텍스트.
 *
 * 호출 체인:
 *   nvmf_bdev_ctrlr_{read,write,compare,...}_cmd → [본 함수] → from_le32/64
 */
static void
nvmf_bdev_ctrlr_get_rw_params(const struct spdk_nvme_cmd *cmd, uint64_t *start_lba,
			      uint64_t *num_blocks)
{
	/* SLBA: CDW10 and CDW11 */
	*start_lba = from_le64(&cmd->cdw10);
	/* [한국어] from_le64 — &cmd->cdw10 부터 8B 를 little-endian 으로 읽어 64-bit 호스트 정수로
	 *  변환. NVMe 는 와이어 LE 라서 BE 호스트에서도 정확히 디코딩. CDW10|CDW11 가 인접 8B 임을
	 *  활용. */

	/* NLB: CDW12 bits 15:00, 0's based */
	*num_blocks = (from_le32(&cmd->cdw12) & 0xFFFFu) + 1;
	/* [한국어] CDW12 의 하위 16bit 가 NLB(Number of Logical Blocks) 이며 0-based. & 0xFFFFu 로
	 *  PRACT/PRCHK/FUA 등 상위 비트 차단 후 +1 로 실제 블록 수 산출. NVMe Base Spec §6.13. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_get_rw_ext_params - SQE CDW12/CDW13(PRACT/PRCHK/STC/FUA + ELBAT/EILBRT 등)
 *                                      를 spdk_bdev_ext_io_opts 에 채워 bdev/accel 레이어에
 *                                      전달.
 *
 * @cmd:          NVMe SQE.
 * @opts:         [in/out] 호출자가 size/memory_domain/accel_sequence 까지는 채워둠. 본 함수는
 *                nvme_cdw12/cdw13 + dif_check_flags_exclude_mask 를 추가 채움.
 * @set_dif_mask: true 면 dif_check_flags_exclude_mask 를 PRCHK 비트로 계산. false 면 트랜스
 *                포트가 PI 처리한 상태(req->dif_enabled) 이므로 mask 미설정.
 * @return:       없음.
 *
 * PRACT/PRCHK 처리 의도:
 *   - bdev 레이어는 NVMe 스펙 (PRACT bit 13) 을 직접 알고 있어 nvme_cdw12 를 그대로 받아 처리.
 *   - 그러나 PRCHK (Reference Tag/Application Tag/Guard 검사 사용 여부) 는 일반 DIF 추상화로
 *     매핑되어야 함 — bdev 의 dif_check_flags 는 "검사 활성" 비트, NVMe PRCHK 는 "검사 활성"
 *     비트의 negation (CDW12 의 PRCHK 비트가 1 이면 검사 활성, bdev 의 exclude_mask 비트가 1
 *     이면 검사 제외). ~PRCHK & MASK 로 exclude_mask 산출.
 *
 * 호출 체인:
 *   nvmf_bdev_ctrlr_{read,write}_cmd → [본 함수] → from_le32
 */
static void
nvmf_bdev_ctrlr_get_rw_ext_params(const struct spdk_nvme_cmd *cmd,
				  struct spdk_bdev_ext_io_opts *opts,
				  bool set_dif_mask)
{
	/* Get CDW12 values */
	opts->nvme_cdw12.raw = from_le32(&cmd->cdw12);
	/* [한국어] CDW12 raw 32-bit — bdev 가 NLB/PRINFO/FUA/LR 비트를 직접 해석. */

	/* Get CDW13 values */
	opts->nvme_cdw13.raw = from_le32(&cmd->cdw13);
	/* [한국어] CDW13 raw — DSM hints (Compression/Sequential/etc) 와 ZNS append 의 일부 필드. */

	/* Bdev layer checks PRACT in CDW12 because it is NVMe specific, but
	 * it does not check DIF check flags in CDW because DIF is not NVMe
	 * specific. Hence, copy DIF check flags from CDW12 to dif_check_flags_exclude_mask.
	 */
	if (set_dif_mask) {
		/* [한국어] target 측 PI 처리 모드 — PRCHK 비트를 generic DIF exclude mask 로 변환. */
		opts->dif_check_flags_exclude_mask = (~opts->nvme_cdw12.raw) & SPDK_NVME_IO_FLAGS_PRCHK_MASK;
		/* [한국어] PRCHK 비트(CDW12 [29:26] 영역의 GUARD/APPTAG/REFTAG 검사 enable) 를 NOT 한 후
		 *  PRCHK 마스크로 AND — "비활성 검사" 만 남김. bdev/accel 이 이 mask 를 보고 해당 검사
		 *  스킵. NVMe Base Spec §5.17.2.1.1 PRINFO/PRCHK 정의. */
	}
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_lba_in_range - I/O LBA 범위가 backing bdev 크기 안에 들어가는지 + 64-bit
 *                                 산술 오버플로 없는지 검사.
 *
 * @bdev_num_blocks: bdev 의 총 LBA 수 (geometry).
 * @io_start_lba:    호스트가 요청한 시작 LBA.
 * @io_num_blocks:   호스트가 요청한 블록 수 (이미 0-based 보정 후).
 * @return:          true = 범위 안전 (제출 가능), false = 범위 초과 또는 오버플로 (호스트에
 *                   LBA_OUT_OF_RANGE 회신해야 함).
 *
 * NVMe Base Spec §6.13: SLBA + NLB 가 namespace 크기 초과시 LBA_OUT_OF_RANGE (SC 0x80) 반환.
 * 추가로 io_start_lba + io_num_blocks 가 64-bit 오버플로하면(2^64 wrap-around) 결과가 < start
 * 가 되므로 그것도 거부. 악의적 호스트로부터 NSZE 우회 방어.
 */
static bool
nvmf_bdev_ctrlr_lba_in_range(uint64_t bdev_num_blocks, uint64_t io_start_lba,
			     uint64_t io_num_blocks)
{
	if (io_start_lba + io_num_blocks > bdev_num_blocks ||
	    io_start_lba + io_num_blocks < io_start_lba) {
		/* [한국어] (a) 끝 LBA 가 NSZE 초과, 또는 (b) 64-bit wrap-around 발생. 둘 다 거부. */
		return false;
	}

	return true;
	/* [한국어] 범위 안전 — 호출자가 spdk_bdev_*() 제출 진행 가능. */
}

/*
 * [한국어]
 * nvmf_ctrlr_process_io_cmd_resubmit - bdev_io 풀 고갈(ENOMEM)로 큐잉된 IO 요청을 풀이 비면
 *                                      재제출하는 콜백.
 *
 * @arg: spdk_nvmf_request 포인터 (queue_io_wait 등록 시 cb_arg 로 저장).
 *
 * spdk_bdev_*() 가 -ENOMEM 반환하면 본 모듈은 spdk_bdev_queue_io_wait 로 등록 (관련:
 * nvmf_bdev_ctrl_queue_io). bdev 코어가 풀 슬롯 확보 시 본 콜백을 invoke — 처음부터 다시
 * nvmf_ctrlr_process_io_cmd 로 dispatch. 만약 이번에도 ENOMEM 이면 다시 큐잉 (재귀 안전:
 * 본 함수가 호출되는 시점은 bdev_io 1 개 확보된 상태).
 *
 * 실행 컨텍스트: bdev_io_pool free 시 bdev 코어가 호출 — 컨트롤러 IO 스레드.
 *
 * 호출 체인:
 *   spdk_bdev_io_complete (다른 IO 의 완료) → bdev 코어가 wait queue drain
 *     → [본 함수] → nvmf_ctrlr_process_io_cmd → 다시 spdk_bdev_*()
 */
static void
nvmf_ctrlr_process_io_cmd_resubmit(void *arg)
{
	struct spdk_nvmf_request *req = arg;
	/* [한국어] 큐잉 시 전달된 요청 — 호스트 SQE/CPL 컨텍스트 보존. */
	int rc;
	/* [한국어] dispatch 재시도 반환값 — COMPLETE(동기 완료) 또는 ASYNCHRONOUS(비동기 진행 중). */

	rc = nvmf_ctrlr_process_io_cmd(req);
	/* [한국어] IO opcode dispatch 재시도 — opcode 분기 후 본 파일의 *_cmd 함수 중 하나가 다시
	 *  호출됨. */
	if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		/* [한국어] 동기 완료 (예: LBA out-of-range, INVALID_FIELD 등) 면 transport 에 즉시 회신.
		 *  ASYNCHRONOUS 면 bdev 가 콜백으로 회신 — 본 함수는 더 할 일 없음. */
		spdk_nvmf_request_complete(req);
	}
}

/*
 * [한국어]
 * nvmf_ctrlr_process_admin_cmd_resubmit - admin 명령 버전의 ENOMEM 재제출 콜백.
 *
 * @arg: spdk_nvmf_request 포인터.
 *
 * spdk_nvmf_bdev_ctrlr_nvme_passthru_admin / abort 등이 -ENOMEM 시 등록. 동작은 IO 버전과
 * 동일하되 dispatcher 가 nvmf_ctrlr_process_admin_cmd 로 다름.
 *
 * 호출 체인:
 *   bdev_io_pool free → bdev 코어 wait queue drain
 *     → [본 함수] → nvmf_ctrlr_process_admin_cmd
 */
static void
nvmf_ctrlr_process_admin_cmd_resubmit(void *arg)
{
	struct spdk_nvmf_request *req = arg;
	/* [한국어] 큐잉 시 전달된 admin 요청 — SQE/CPL 컨텍스트 보존. */
	int rc;
	/* [한국어] admin dispatch 재시도 반환값 — COMPLETE(동기 완료) 또는 ASYNCHRONOUS(비동기 진행 중). */

	rc = nvmf_ctrlr_process_admin_cmd(req);
	/* [한국어] admin opcode dispatch 재시도 — Identify/SetFeatures/Abort/passthru_admin 등. */
	if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		spdk_nvmf_request_complete(req);
		/* [한국어] 동기 완료 → 트랜스포트로 즉시 회신. */
	}
}

/*
 * [한국어]
 * nvmf_bdev_ctrl_queue_io - bdev_io 풀 고갈(ENOMEM)로 IO 제출 실패한 요청을 풀 free 까지
 *                            대기 큐에 등록 + 통계 카운터 증가.
 *
 * @req:    제출 실패한 요청.
 * @bdev:   IO 대상 bdev.
 * @ch:     thread-local io_channel.
 * @cb_fn:  풀 free 시 호출될 콜백 (보통 *_resubmit 함수).
 * @cb_arg: cb_fn 의 인자.
 *
 * spdk_bdev_queue_io_wait 는 bdev 의 wait queue 에 항목 추가. bdev 코어가 다른 IO 완료로
 * 풀 슬롯 free 할 때 FIFO 로 cb_fn(cb_arg) 호출. 본 파일의 모든 IO dispatch 함수에서 ENOMEM
 * 시 호출. queue_io_wait 자체가 실패하면 (할당 실패) assert — 정상 운용에서는 일어나지 않음.
 *
 * Backpressure 메커니즘: bdev_io_pool size 이상의 IO 가 동시에 쌓이지 않도록 자연스레 제한.
 *
 * 호출 체인:
 *   nvmf_bdev_ctrlr_*_cmd 가 ENOMEM 받음
 *     → [본 함수] → spdk_bdev_queue_io_wait → 풀 free 시 cb_fn(*_resubmit)
 */
static void
nvmf_bdev_ctrl_queue_io(struct spdk_nvmf_request *req, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn, void *cb_arg)
{
	int rc;

	req->bdev_io_wait.bdev = bdev;
	req->bdev_io_wait.cb_fn = cb_fn;
	req->bdev_io_wait.cb_arg = cb_arg;
	/* [한국어] req 내장 wait 엔트리 채우기 — bdev_io_wait_entry 는 spdk_nvmf_request 가
	 *  보유하므로 별도 alloc 불필요. */

	rc = spdk_bdev_queue_io_wait(bdev, ch, &req->bdev_io_wait);
	/* [한국어] bdev 의 채널별 wait queue 에 등록. 채널 단위라 thread affinity 자동 보장. */
	if (rc != 0) {
		/* [한국어] 정상 운용 중 발생 불가 — bdev_queue_io_wait 는 단순 list insert 만 수행.
		 *  실패 시 메모리 손상이나 잘못된 사용 패턴으로 보고 assert 로 종료. */
		assert(false);
	}
	req->qpair->group->stat.pending_bdev_io++;
	/* [한국어] poll group 통계 — 현재 풀 부족으로 대기 중인 IO 수 증가. RPC 로 노출되어 호스트
	 *  처리량 vs bdev_io_pool size 튜닝 단서. */
}

/*
 * [한국어]
 * nvmf_bdev_zcopy_enabled - backing bdev 가 ZCOPY (zero-copy populate/commit) 지원하는지
 *                            조회 헬퍼.
 *
 * @bdev:   대상 bdev.
 * @return: ZCOPY 지원이면 true.
 *
 * ZCOPY 는 호스트 SGL 버퍼를 그대로 bdev 백엔드에 노출 (예: lvol 의 backing buffer 를
 * RDMA wire 와 공유). 지원 시 read/write 가 본 파일의 zcopy_start/zcopy_end 경로로 우회되어
 * 데이터 복사 0번 가능.
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_process_io_cmd → [본 함수] → spdk_bdev_io_type_supported(ZCOPY)
 */
bool
nvmf_bdev_zcopy_enabled(struct spdk_bdev *bdev)
{
	return spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ZCOPY);
	/* [한국어] bdev 의 io_type_supported fn 으로 ZCOPY 비트 조회. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_read_cmd - NVMe Read(OPC 02h) → spdk_bdev_readv_blocks_ext 디스패치.
 *
 * @bdev: 대상 bdev.
 * @desc: open descriptor (DIF mask/block_size 메타).
 * @ch:   thread-local io_channel.
 * @req:  NVMe-oF 요청 (SQE/iov/iovcnt/length/dif_enabled 등).
 * @return: ASYNCHRONOUS = bdev 가 콜백으로 완료 보고, COMPLETE = 호출자가 즉시 완료 처리.
 *
 * 호스트 → wire → req 까지 도달한 SQE 의 SLBA/NLB 를 추출 → LBA 범위/SGL 길이 검증 →
 * spdk_bdev_readv_blocks_ext 에 PRACT/PRCHK + memory_domain + accel_sequence 옵션 첨부하여
 * 제출. ENOMEM 은 큐잉 후 재시도, LBA 초과/SGL 부족은 호스트에 SC 회신.
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_process_io_cmd (opcode=READ) → [본 함수]
 *     → spdk_bdev_readv_blocks_ext → bdev module submit
 *       → 완료 콜백 nvmf_bdev_ctrlr_complete_cmd
 */
int
nvmf_bdev_ctrlr_read_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_bdev_ext_io_opts opts = {
		.size = SPDK_SIZEOF(&opts, nvme_cdw13),
		.memory_domain = req->memory_domain,
		.memory_domain_ctx = req->memory_domain_ctx,
		.accel_sequence = req->accel_sequence,
	};
	/* [한국어] ext_io_opts — bdev API 의 옵션 구조체. SPDK_SIZEOF(&opts, nvme_cdw13) 는 ABI
	 *  호환을 위해 nvme_cdw13 까지의 partial size — 새 필드가 추가되면 이 값을 늘려 모듈이
	 *  자기보다 큰 opts 를 안전하게 거부. memory_domain/memory_domain_ctx 는 GPU/RDMA buffer
	 *  처럼 호스트 가상주소가 아닌 메모리, accel_sequence 는 zero-copy decrypt/CRC chain. */
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	/* [한국어] bdev 총 LBA — LBA 범위 검사용. */
	uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
	/* [한국어] desc 기준 block_size — DIF strip 모드면 data block 만, 아니면 metadata 포함. */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	/* [한국어] 호스트 SQE — CDW10/11/12/13 파싱 대상. */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	/* [한국어] CPL 빈 슬롯 — 동기 실패 시 SC/SCT 채움. */
	uint64_t start_lba;
	uint64_t num_blocks;
	/* [한국어] 추출된 SLBA + NLB. */
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);
	/* [한국어] CDW10/11/12 → start_lba, num_blocks 파싱 (0-based 보정 포함). */
	nvmf_bdev_ctrlr_get_rw_ext_params(cmd, &opts, !req->dif_enabled);
	/* [한국어] PRACT/PRCHK/CDW13 → opts. dif_enabled=true 면 트랜스포트가 PI 처리 → mask 미설정.
	 *  set_dif_mask = !req->dif_enabled. */

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		/* [한국어] LBA 범위 위반 검출 — unlikely (정상 호스트는 NSZE 광고 따름). */
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		/* [한국어] SC 0x80 LBA Out Of Range — NVMe Base Spec §4.6.1. */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		/* [한국어] 동기 완료 — 호출자가 spdk_nvmf_request_complete 호출. */
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		/* [한국어] 호스트가 약속한 데이터량(SGL length) 보다 NLB*block_size 가 큼 — capsule
		 *  파싱과 명령이 일치하지 않음. */
		SPDK_ERRLOG("Read NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		/* [한국어] SC 0x21 Data SGL Length Invalid. */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	assert(!spdk_nvmf_request_using_zcopy(req));
	/* [한국어] 본 함수는 일반 read 경로 — zcopy 활성 요청은 zcopy_start 로 분기되어야 함.
	 *  잘못 들어오면 이중 buffer 사용 위험. */

	rc = spdk_bdev_readv_blocks_ext(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
					nvmf_bdev_ctrlr_complete_cmd, req, &opts);
	/* [한국어] bdev 코어에 비동기 read 제출 — _ext suffix 는 ext_io_opts 받는 변종. iov 는 호스트
	 *  SGL 풀어진 버퍼들. 완료 시 nvmf_bdev_ctrlr_complete_cmd(bdev_io, success, req) 호출. */
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			/* [한국어] bdev_io 풀 고갈 — 큐잉 후 풀 free 시 재시도. */
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
			/* [한국어] 호스트는 ASYNCHRONOUS 로 보이고, 재시도 콜백이 결국 완료 보고. */
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		/* [한국어] -EINVAL 등 기타 동기 오류 — Internal Device Error (SC 0x06). */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	/* [한국어] 정상 제출 — bdev 콜백이 추후 spdk_nvmf_request_complete 호출. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_write_cmd - NVMe Write(OPC 01h) → spdk_bdev_writev_blocks_ext 디스패치.
 *
 * @bdev/desc/ch/req: read_cmd 와 동일한 의미.
 * @return: ASYNCHRONOUS = bdev 가 콜백으로 완료 보고.
 *
 * read_cmd 와 거의 동일한 구조 — 차이는 readv → writev. PRACT/PRCHK 처리도 동일하지만,
 * 호스트가 PI 를 직접 만들어 보내는 모드(req->dif_enabled=false)에서는 bdev/accel 이 PI
 * 검증 후 디스크 기록.
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_process_io_cmd (opcode=WRITE) → [본 함수]
 *     → spdk_bdev_writev_blocks_ext → 모듈 → 디스크
 *       → 완료 콜백 nvmf_bdev_ctrlr_complete_cmd
 */
int
nvmf_bdev_ctrlr_write_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_bdev_ext_io_opts opts = {
		.size = SPDK_SIZEOF(&opts, nvme_cdw13),
		.memory_domain = req->memory_domain,
		.memory_domain_ctx = req->memory_domain_ctx,
		.accel_sequence = req->accel_sequence,
	};
	/* [한국어] read_cmd 와 동일한 의도의 ext_io_opts 초기화. */
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	/* [한국어] LBA 범위 상한. */
	uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
	/* [한국어] desc 기준 block_size — SGL 길이 검증용. */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);
	/* [한국어] SLBA/NLB 추출. */
	nvmf_bdev_ctrlr_get_rw_ext_params(cmd, &opts, !req->dif_enabled);
	/* [한국어] PRACT/PRCHK/CDW13 → opts. */

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		/* [한국어] write 가 NSZE 초과 영역에 닿으면 거부 — 데이터 무결성 보호. */
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		/* [한국어] 호스트가 SGL 로 약속한 양보다 NLB 가 큼 — capsule 불일치. */
		SPDK_ERRLOG("Write NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	assert(!spdk_nvmf_request_using_zcopy(req));
	/* [한국어] 일반 write 경로 — zcopy write 는 zcopy_start(populate=false) 로 분기. */

	rc = spdk_bdev_writev_blocks_ext(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
					 nvmf_bdev_ctrlr_complete_cmd, req, &opts);
	/* [한국어] 비동기 write 제출. */
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			/* [한국어] bdev_io 풀 고갈 — 큐잉. */
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	/* [한국어] 정상 비동기 경로. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_compare_cmd - NVMe Compare(OPC 05h) → spdk_bdev_comparev_blocks 디스패치.
 *
 * @bdev/desc/ch/req: 표준.
 * @return: 동기 또는 비동기 완료 상태.
 *
 * Compare 명령은 호스트가 보낸 데이터 패턴이 디스크 LBA 범위와 byte-for-byte 일치하는지
 * 검사. 불일치 시 SC=COMPARE_FAILURE (0x85). 호스트의 storage assertion (예: 동시 접근
 * 검출) 에 사용. PI 검사는 별도로 PRCHK 따라.
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_process_io_cmd (opcode=COMPARE, fused 아님)
 *     → [본 함수] → spdk_bdev_comparev_blocks
 */
int
nvmf_bdev_ctrlr_compare_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	/* [한국어] LBA 범위 상한. */
	uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
	/* [한국어] SGL 길이 검증용. */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);
	/* [한국어] SLBA/NLB 파싱. compare 는 ext_params 사용 안 함 — bdev_compare API 가
	 *  ext_io_opts 비변종. */

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		/* [한국어] 범위 초과 — 일반 read/write 와 동일 처리. */
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		/* [한국어] SGL 부족 — 비교할 데이터가 모자람. */
		SPDK_ERRLOG("Compare NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_comparev_blocks(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
				       nvmf_bdev_ctrlr_complete_cmd, req);
	/* [한국어] bdev compare 제출 — bdev 가 디스크 read 후 host iov 와 memcmp. 불일치는 SC 로 보고. */
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			/* [한국어] 풀 고갈 → 큐잉. */
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	/* [한국어] 비동기 — 결과는 콜백에서 SCT_MEDIA + SC_COMPARE_FAILURE 또는 SUCCESS. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_compare_and_write_cmd - NVMe FUSED Compare(0x05) + Write(0x01) atomic 쌍을
 *                                          spdk_bdev_comparev_and_writev_blocks 로 합쳐서 제출.
 *
 * @bdev/desc/ch: 표준.
 * @cmp_req:    fused 페어의 첫 번째 요청 (Compare 부분, FUSE=01b).
 * @write_req:  fused 페어의 두 번째 요청 (Write 부분, FUSE=10b).
 * @return:     동기 완료(검증 실패) 또는 비동기.
 *
 * NVMe Base Spec §6.2 Fused Operations: 두 SQE 가 인접 SQ slot 에 들어와야 하며 각자 fuse
 * field 가 first(01b)/second(10b) 로 마킹된 경우 컨트롤러는 두 명령을 atomic 으로 처리.
 * 호스트는 이를 lock-free 자료구조 갱신용으로 사용 (fetch-and-update 패턴).
 *
 * 두 SQE 의 SLBA/NLB 가 동일해야 하며 (검증 후 spec 위반 시 INVALID_FIELD), bdev API
 * comparev_and_writev_blocks 가 atomicity 를 모듈에 위임. 완료 보고는 write_req 의 콜백에서
 * cmp_req CPL 까지 함께 채움 (nvmf_bdev_ctrlr_complete_cmd 의 first_fused 분기).
 *
 * 호출 체인:
 *   ctrlr.c 가 인접 SQE 두 개의 fuse 비트 검출 후 페어링
 *     → [본 함수] → spdk_bdev_comparev_and_writev_blocks
 *       → 완료 콜백 nvmf_bdev_ctrlr_complete_cmd(write_req, first_fused=true)
 */
int
nvmf_bdev_ctrlr_compare_and_write_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				      struct spdk_io_channel *ch, struct spdk_nvmf_request *cmp_req, struct spdk_nvmf_request *write_req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	/* [한국어] LBA 범위 상한. */
	uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
	/* [한국어] SGL 길이 검증용. */
	struct spdk_nvme_cmd *cmp_cmd = &cmp_req->cmd->nvme_cmd;
	struct spdk_nvme_cmd *write_cmd = &write_req->cmd->nvme_cmd;
	/* [한국어] 두 SQE 각각 — SLBA/NLB 일치 검증 위해 별도 파싱. */
	struct spdk_nvme_cpl *rsp = &write_req->rsp->nvme_cpl;
	/* [한국어] write 의 CPL 슬롯 — 동기 실패 시 SC 채움 (cmp 의 CPL 은 호출자가 후속 처리). */
	uint64_t write_start_lba, cmp_start_lba;
	uint64_t write_num_blocks, cmp_num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmp_cmd, &cmp_start_lba, &cmp_num_blocks);
	nvmf_bdev_ctrlr_get_rw_params(write_cmd, &write_start_lba, &write_num_blocks);
	/* [한국어] 두 SQE 의 SLBA/NLB 추출. */

	if (spdk_unlikely(write_start_lba != cmp_start_lba || write_num_blocks != cmp_num_blocks)) {
		/* [한국어] fused 명령의 두 부분은 동일 LBA 영역을 다뤄야 함 — spec 위반. */
		SPDK_ERRLOG("Fused command start lba / num blocks mismatch\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		/* [한국어] SC 0x02 Invalid Field — fused 페어 LBA 불일치. */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, write_start_lba,
			  write_num_blocks))) {
		/* [한국어] 범위 초과 검사 — write 기준으로 (cmp 와 동일하므로 한 번만). */
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(write_num_blocks * block_size > write_req->length)) {
		/* [한국어] write 의 SGL 길이 검증. cmp 도 같이 검증해야 하지만 보통 트랜스포트가 동일
		 *  iov 풀에서 할당하므로 write 만 체크. */
		SPDK_ERRLOG("Write NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    write_num_blocks, block_size, write_req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_comparev_and_writev_blocks(desc, ch, cmp_req->iov, cmp_req->iovcnt, write_req->iov,
			write_req->iovcnt, write_start_lba, write_num_blocks, nvmf_bdev_ctrlr_complete_cmd, write_req);
	/* [한국어] bdev 코어가 atomic compare-and-write 수행 — 모듈이 SCC(Simple Compare And Write)
	 *  지원하면 device offload, 아니면 channel-level lock 후 read+memcmp+write. 콜백은 write_req
	 *  로 등록하고, complete_cmd 가 fused 분기에서 cmp_req->first_fused_req 를 통해 양쪽 보고. */
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			/* [한국어] 풀 고갈 — 두 요청 모두 큐잉. */
			nvmf_bdev_ctrl_queue_io(cmp_req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, cmp_req);
			nvmf_bdev_ctrl_queue_io(write_req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, write_req);
			/* [한국어] cmp_req 와 write_req 둘 다 재시도 큐 — bdev 가 두 요청을 다시 받으면 fused
			 *  페어링은 ctrlr.c 레벨에서 보존되어 본 함수로 다시 들어옴. */
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	/* [한국어] 비동기 — 결과는 콜백에서 fused status 분리 후 양쪽 CPL 보고. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_write_zeroes_cmd - NVMe Write Zeroes(OPC 08h) → spdk_bdev_write_zeroes_blocks.
 *
 * @bdev/desc/ch/req: 표준.
 * @return: 동기 또는 비동기 완료.
 *
 * Write Zeroes 는 호스트가 데이터 페이로드 없이 LBA 범위를 0 으로 채우라는 명령. SSD 가
 * 내부적으로 erase block 또는 zero-page 매핑으로 처리하면 wire bandwidth 0. 본 함수는
 * 추가로 두 가지 검사:
 *   1) 서브시스템의 max_write_zeroes_size_kib (RPC 설정) 초과 거부 — 운영자가 큰 zero 명령으로
 *      장치 점유 시간 폭증 방지.
 *   2) DEAC (Deallocate) 비트 미지원 — DEAC=1 이면 0 채움 + 동시에 unmap 도 — bdev 로 별도
 *      hint 전달 미지원으로 거부.
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_process_io_cmd (opcode=WRITE_ZEROES) → [본 함수]
 *     → spdk_bdev_write_zeroes_blocks
 */
int
nvmf_bdev_ctrlr_write_zeroes_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	/* [한국어] LBA 범위 상한. */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t max_write_zeroes_size = req->qpair->ctrlr->subsys->max_write_zeroes_size_kib;
	/* [한국어] 서브시스템 단위 정책 — 단일 Write Zeroes 의 최대 크기(KiB). 0 = 무제한. RPC
	 *  nvmf_create_subsystem 등에서 설정. */
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);
	/* [한국어] SLBA/NLB 추출. */
	if (spdk_unlikely(max_write_zeroes_size > 0 &&
			  num_blocks > (max_write_zeroes_size << 10) / spdk_bdev_desc_get_block_size(desc))) {
		/* [한국어] (KiB << 10) = bytes. 그것을 block_size 로 나누면 최대 LBA 수. NLB 가 그보다
		 *  크면 거부. unlikely: 호스트가 광고된 한도를 따르면 안 걸림. */
		SPDK_ERRLOG("invalid write zeroes size, should not exceed %" PRIu64 "Kib\n", max_write_zeroes_size);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		/* [한국어] LBA 범위 초과. */
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(cmd->cdw12_bits.write_zeroes.deac)) {
		/* [한국어] CDW12 의 DEAC(Deallocate, bit 25) 가 1 이면 호스트는 zero-fill + unmap 동작을
		 *  요구. SPDK bdev API 는 둘을 분리하므로 현재 미지원 — 거부. */
		SPDK_ERRLOG("Write Zeroes Deallocate is not supported\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_write_zeroes_blocks(desc, ch, start_lba, num_blocks,
					   nvmf_bdev_ctrlr_complete_cmd, req);
	/* [한국어] bdev 의 zero-fill 제출 — 모듈이 supports_write_zeroes 면 device-offload, 아니면
	 *  bdev 코어가 에뮬레이션(0 buffer 로 write). */
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			/* [한국어] 풀 고갈 → 큐잉. */
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_flush_cmd - NVMe Flush(OPC 00h) → spdk_bdev_flush_blocks.
 *
 * @bdev/desc/ch/req: 표준.
 * @return: 동기 (모듈이 flush 미지원이면 즉시 SUCCESS) 또는 비동기.
 *
 * Flush 는 volatile write cache (장치 측 DRAM/PMEM) 의 모든 변경분을 영구 매체로 강제 동기화.
 * SPDK NVMe-oF target 은 호스트에 항상 VWC=1 광고하므로 Flush 명령을 받지만, 백엔드 bdev 가
 * 실제 cache 가 없거나 flush 미지원이면 (예: ramdisk) NOP 처리하고 SUCCESS 회신해야 호스트
 * 드라이버가 만족.
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_process_io_cmd (opcode=FLUSH) → [본 함수]
 *     → spdk_bdev_flush_blocks (전체 LBA 범위)
 */
int
nvmf_bdev_ctrlr_flush_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	/* [한국어] CPL 슬롯 — bdev flush 미지원 시 즉시 SUCCESS 기록, 지원 시 콜백에서 기록. */
	int rc;
	/* [한국어] spdk_bdev_flush_blocks 반환값 — 0(성공·비동기) / -ENOMEM(풀 고갈) / 기타 에러. */

	/* As for NVMeoF controller, SPDK always set volatile write
	 * cache bit to 1, return success for those block devices
	 * which can't support FLUSH command.
	 */
	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH)) {
		/* [한국어] bdev 가 flush 미지원 — VWC=1 로 광고했으므로 NOP 으로 응답. 호스트는 fsync()/
		 *  sync_file_range() 등이 정상 동작한다고 인식. */
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_SUCCESS;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_flush_blocks(desc, ch, 0, spdk_bdev_get_num_blocks(bdev),
				    nvmf_bdev_ctrlr_complete_cmd, req);
	/* [한국어] flush 제출 — NVMe Flush 는 namespace 전체이므로 (0 ~ NSZE) 범위로 호출. */
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			/* [한국어] 풀 고갈 → 큐잉. */
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		/* [한국어] 기타 동기 오류 — Internal Device Error. SCT 는 default 0 (GENERIC). */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

/*
 * [한국어]
 * struct nvmf_bdev_ctrlr_unmap - DSM(Dataset Management) Deallocate 명령의 멀티 range 처리
 *                                 컨텍스트.
 *
 * NVMe DSM AD 명령은 한 SQE 가 최대 256 개 range 를 가질 수 있고, 각 range 마다 별도 unmap
 * 호출이 필요. 본 구조체는 진행 중 상태(현재 range 인덱스, 미완료 카운터, 재시도용 인자)를
 * 보관하여 모든 range 가 완료될 때까지 호스트 응답 보류.
 */
struct nvmf_bdev_ctrlr_unmap {
	struct spdk_nvmf_request	*req;
	/* [한국어] 호스트 NVMe-oF DSM 요청 — 모든 range 완료 후 단일 CPL 회신 대상.
	 *  설정자: 본 컨텍스트 calloc 시 nvmf_bdev_ctrlr_unmap()이 채움.
	 *  읽는 자: nvmf_bdev_ctrlr_unmap_cpl() 이 마지막 range 완료시 spdk_nvmf_request_complete.
	 *  값 범위: 유효한 spdk_nvmf_request 포인터, NULL 불가.
	 *  동기화: 단일 IO thread 내에서만 사용 — 락 없음. */

	uint32_t			count;
	/* [한국어] 현재 진행 중(unmap 제출되어 콜백 대기) range 수 — 0 이면 모두 끝.
	 *  설정자: 새 range 제출 시 ++, 콜백 nvmf_bdev_ctrlr_unmap_cpl() 가 -- 로 감소.
	 *  읽는 자: cpl 콜백이 0 도달 시 호스트 응답 + ctx free.
	 *  값 범위: 0 ~ nr (DSM range 총 수). */

	struct spdk_bdev_desc		*desc;
	/* [한국어] backing bdev 의 open descriptor — ENOMEM 재시도(unmap_resubmit) 시
	 *  spdk_bdev_unmap_blocks() 의 첫 번째 인자로 재전달.
	 *  설정자: 컨텍스트 생성 시 nvmf_bdev_ctrlr_unmap() 이 채움 (호출자가 전달한 desc).
	 *  읽는 자: nvmf_bdev_ctrlr_unmap_resubmit() 이 nvmf_bdev_ctrlr_unmap() 재호출할 때.
	 *  값 범위: 유효한 spdk_bdev_desc 포인터 — spdk_bdev_open_ext() 결과, NULL 불가.
	 *  동기화: 단일 IO thread 에서만 사용 — 락 없음. */

	struct spdk_bdev		*bdev;
	/* [한국어] backing bdev 인스턴스 — ENOMEM 재시도(nvmf_bdev_ctrlr_unmap_resubmit) 시
	 *  spdk_bdev_queue_io_wait() 의 첫 번째 인자로 사용.
	 *  설정자: 컨텍스트 생성 시 nvmf_bdev_ctrlr_unmap() 이 채움 (ns->bdev 에서 복사).
	 *  읽는 자: nvmf_bdev_ctrlr_unmap_resubmit() 이 nvmf_bdev_ctrlr_unmap() 재호출할 때.
	 *  값 범위: 유효한 spdk_bdev 포인터 (NULL 불가 — dsm_cmd 에서 검증 후 진입).
	 *  동기화: 단일 IO thread 내에서만 참조되므로 락 없음. */

	struct spdk_io_channel		*ch;
	/* [한국어] backing bdev 의 thread-local io_channel — ENOMEM 재시도 시 스레드 재진입 없이
	 *  동일 채널을 재사용하기 위해 컨텍스트에 저장.
	 *  설정자: nvmf_bdev_ctrlr_unmap() 이 컨텍스트 생성 시 채움 (호출자가 전달한 ch).
	 *  읽는 자: nvmf_bdev_ctrlr_unmap_resubmit() 이 nvmf_bdev_ctrlr_unmap() 재호출 시 전달.
	 *  값 범위: 유효한 spdk_io_channel 포인터 — spdk_get_io_channel() 이 반환한 채널.
	 *  동기화: io_channel 은 thread-local 이므로 이 컨텍스트를 소유한 IO thread 에서만 유효. */

	uint32_t			range_index;
	/* [한국어] 현재까지 처리(제출 시도)한 range 인덱스 — for 루프 진행 위치. ENOMEM 으로 중단 후
	 *  재진입 시 이 인덱스부터 계속.
	 *  설정자: unmap() for-loop 가 매 range 제출 후 ++.
	 *  읽는 자: 재시도 진입 시 i = range_index. */
};

/*
 * [한국어]
 * nvmf_bdev_ctrlr_unmap_cpl - 단일 unmap range 의 bdev_io 완료 콜백 (DSM 멀티 range 집계).
 *
 * @bdev_io: 완료된 단일 range 의 bdev_io.
 * @success: 성공 여부.
 * @cb_arg:  공유 nvmf_bdev_ctrlr_unmap 컨텍스트.
 *
 * count 를 감소시키고, 한 번이라도 실패한 status 가 응답에 기록되어 있지 않으면 현재 결과로
 * 채움 (먼저 발생한 오류 우선). count 가 0 도달하면 모든 range 완료 → 호스트에 단일 CPL 회신.
 *
 * 호출 체인:
 *   bdev module IO 완료 → 본 함수 → spdk_nvmf_request_complete (마지막 range 에서)
 */
static void
nvmf_bdev_ctrlr_unmap_cpl(struct spdk_bdev_io *bdev_io, bool success,
			  void *cb_arg)
{
	struct nvmf_bdev_ctrlr_unmap *unmap_ctx = cb_arg;
	/* [한국어] 공유 progress 컨텍스트 — 여러 range 의 콜백이 동일 포인터를 cb_arg 로 공유. */
	struct spdk_nvmf_request	*req = unmap_ctx->req;
	/* [한국어] 호스트 DSM 요청 — 최종 응답 대상. */
	struct spdk_nvme_cpl		*response = &req->rsp->nvme_cpl;
	/* [한국어] CPL 슬롯 — first-failure 결과를 기록. */
	int				sc, sct;
	/* [한국어] 현재 range 의 Status Code / Status Code Type. */
	uint32_t			cdw0;
	/* [한국어] 현재 range 의 CDW0 — unmap 은 보통 0. */

	unmap_ctx->count--;
	/* [한국어] 콜백 1번 = range 1개 완료 — 미완료 카운터 감소. */

	if (response->status.sct == SPDK_NVME_SCT_GENERIC &&
	    response->status.sc == SPDK_NVME_SC_SUCCESS) {
		/* [한국어] 아직 다른 range 에서 실패 기록 안됨 — 현재 결과로 채움. 한 번 실패가 기록되면
		 *  이후 range 결과는 무시 (NVMe spec 의 first-failure-wins). */
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);
		response->cdw0 = cdw0;
		response->status.sc = sc;
		response->status.sct = sct;
	}

	if (unmap_ctx->count == 0) {
		/* [한국어] 모든 range 완료 — 호스트 응답 + 컨텍스트 free. */
		spdk_nvmf_request_complete(req);
		free(unmap_ctx);
	}
	spdk_bdev_free_io(bdev_io);
	/* [한국어] 단일 range 의 bdev_io 반납 — 다른 range 의 bdev_io 는 별도 콜백에서 반납. */
}

/* [한국어] 전방 선언 — unmap_resubmit 가 unmap 을 호출하기 위해 필요. */
static int nvmf_bdev_ctrlr_unmap(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
				 struct nvmf_bdev_ctrlr_unmap *unmap_ctx);
/*
 * [한국어]
 * nvmf_bdev_ctrlr_unmap_resubmit - DSM 처리 중 ENOMEM 으로 큐잉된 range 의 재시도 콜백.
 *
 * @arg: nvmf_bdev_ctrlr_unmap 컨텍스트.
 *
 * 컨텍스트에 보관된 range_index 부터 unmap() 을 다시 호출 — 진행 위치 유지.
 *
 * 호출 체인:
 *   bdev_io_pool free → bdev queue_io_wait → [본 함수] → nvmf_bdev_ctrlr_unmap()
 */
static void
nvmf_bdev_ctrlr_unmap_resubmit(void *arg)
{
	struct nvmf_bdev_ctrlr_unmap *unmap_ctx = arg;
	/* [한국어] 큐잉 시 보존된 컨텍스트. */
	struct spdk_nvmf_request *req = unmap_ctx->req;
	struct spdk_bdev_desc *desc = unmap_ctx->desc;
	struct spdk_bdev *bdev = unmap_ctx->bdev;
	struct spdk_io_channel *ch = unmap_ctx->ch;
	/* [한국어] 재시도 인자 4개를 컨텍스트에서 추출. */

	nvmf_bdev_ctrlr_unmap(bdev, desc, ch, req, unmap_ctx);
	/* [한국어] unmap_ctx 를 다시 전달 — 새 컨텍스트 alloc 우회, range_index 부터 재개. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_unmap - DSM Deallocate 의 메인 루프 — 데이터 페이로드(range 배열) 파싱 후
 *                         각 range 를 spdk_bdev_unmap_blocks 로 제출.
 *
 * @bdev/desc/ch/req: 표준.
 * @unmap_ctx:        NULL 이면 새로 alloc, non-NULL 이면 재시도 진입(이미 진행 중인 컨텍스트).
 * @return:           모든 range 동기 처리 완료(또는 0 개) 면 COMPLETE, 아니면 ASYNCHRONOUS.
 *
 * NVMe Base Spec §7.4 DSM 명령:
 *   - CDW10.NR = number of ranges - 1 (16-bit, max 256).
 *   - Data payload = NR+1 개의 dsm_range (16B each: starting_lba 8B + length 4B + attr 4B).
 *   - max_discard_size_kib (서브시스템 정책) 으로 단일 range 길이 제한.
 *
 * 멀티 range 처리: for-loop 으로 각 range 를 unmap 제출, 콜백이 count 감소. ENOMEM 발생시
 * 그 시점의 range_index 와 진행중 count 를 보존하여 큐잉 → 풀 free 시 재진입.
 *
 * 호출 체인:
 *   nvmf_bdev_ctrlr_dsm_cmd (AD bit 검출)
 *     → [본 함수] → spdk_bdev_unmap_blocks (range 별)
 *       → 콜백 nvmf_bdev_ctrlr_unmap_cpl (집계)
 */
static int
nvmf_bdev_ctrlr_unmap(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		      struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
		      struct nvmf_bdev_ctrlr_unmap *unmap_ctx)
{
	uint16_t nr, i;
	/* [한국어] nr: range 총수, i: 루프 인덱스. */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	uint64_t max_discard_size = req->qpair->ctrlr->subsys->max_discard_size_kib;
	/* [한국어] 단일 range 최대 크기(KiB) — 운영자 정책. 0 = 무제한. */
	uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
	/* [한국어] block_size — KiB → block 변환에 사용. */
	struct spdk_iov_xfer ix;
	/* [한국어] iov 순차 읽기 헬퍼 — req->iov 에서 sizeof(dsm_range) 씩 카피. */
	uint64_t lba;
	uint32_t lba_count;
	int rc;

	nr = cmd->cdw10_bits.dsm.nr + 1;
	/* [한국어] CDW10.NR(0-based) +1 = 실제 range 수. NVMe Base Spec §7.4. */
	if (nr * sizeof(struct spdk_nvme_dsm_range) > req->length) {
		/* [한국어] 호스트가 약속한 SGL 길이가 NR 만큼의 range 를 담기에 부족 — capsule 불일치. */
		SPDK_ERRLOG("Dataset Management number of ranges > SGL length\n");
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (unmap_ctx == NULL) {
		/* [한국어] 첫 진입 — 컨텍스트 alloc. */
		unmap_ctx = calloc(1, sizeof(*unmap_ctx));
		if (!unmap_ctx) {
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			/* [한국어] heap 부족 — Internal Device Error. */
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		unmap_ctx->req = req;
		unmap_ctx->desc = desc;
		unmap_ctx->ch = ch;
		unmap_ctx->bdev = bdev;
		/* [한국어] 재시도 시 필요한 4개 인자 보존. */

		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_SUCCESS;
		/* [한국어] 응답 슬롯 초기화 — first-failure-wins 로직이 SUCCESS 상태를 기준으로 갱신. */
	} else {
		unmap_ctx->count--;	/* dequeued */
		/* [한국어] 재진입 — 큐잉 시 ++ 했던 카운터를 dequeue 시 -- 로 보정. unmap 제출 직전에 ++
		 *  하므로 같은 카운트 두 번 안 됨. */
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
	/* [한국어] iov 순차 reader 초기화 — 매 range 마다 spdk_iov_xfer_to_buf 로 16B 추출. */

	for (i = unmap_ctx->range_index; i < nr; i++) {
		/* [한국어] range_index 부터 시작 — 재시도 시 처음부터가 아니라 중단 위치 재개. */
		struct spdk_nvme_dsm_range dsm_range = { 0 };
		/* [한국어] 현재 range 의 16B descriptor — payload 에서 한 entry 카피. */

		spdk_iov_xfer_to_buf(&ix, &dsm_range, sizeof(dsm_range));
		/* [한국어] iov 에서 16B 읽기 — internal cursor 자동 이동. */

		lba = dsm_range.starting_lba;
		lba_count = dsm_range.length;
		/* [한국어] DSM range descriptor field 추출. */
		if (max_discard_size > 0 && lba_count > (max_discard_size << 10) / block_size) {
			/* [한국어] 단일 range 가 정책 한도 초과 — 거부 후 break. */
			SPDK_ERRLOG("invalid unmap size %" PRIu32 " blocks, should not exceed %" PRIu64 " blocks\n",
				    lba_count, max_discard_size << 1);
			response->status.sct = SPDK_NVME_SCT_GENERIC;
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			break;
		}

		unmap_ctx->count++;
		/* [한국어] 제출 직전 ++ — 콜백이 -- 함. */

		rc = spdk_bdev_unmap_blocks(desc, ch, lba, lba_count,
					    nvmf_bdev_ctrlr_unmap_cpl, unmap_ctx);
		/* [한국어] 비동기 unmap 제출 — bdev 가 모듈로 위임. */
		if (rc) {
			if (rc == -ENOMEM) {
				/* [한국어] 풀 고갈 — 큐잉 후 풀 free 시 unmap_resubmit 으로 재진입. range_index
				 *  는 ++ 안 했으므로 같은 range 재제출. */
				nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_bdev_ctrlr_unmap_resubmit, unmap_ctx);
				/* Unmap was not yet submitted to bdev */
				/* unmap_ctx->count will be decremented when the request is dequeued */
				return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
			}
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			unmap_ctx->count--;
			/* [한국어] 일반 오류 — 카운터 원복(이 range 는 콜백 안 옴). 단, 이미 제출된 다른 range
			 *  콜백을 기다려야 하므로 break 후 ASYNCHRONOUS 분기로. */
			/* We can't return here - we may have to wait for any other
				* unmaps already sent to complete */
			break;
		}
		unmap_ctx->range_index++;
		/* [한국어] 정상 제출 — 다음 range 위치로 진행. 재시도 시 이 인덱스부터. */
	}

	if (unmap_ctx->count == 0) {
		/* [한국어] 진행 중 range 가 없음 — 모두 동기 처리됐거나 0 개 range 였음. ctx free 후
		 *  COMPLETE 반환 → 호출자가 spdk_nvmf_request_complete. */
		free(unmap_ctx);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	/* [한국어] 진행 중 range 존재 — 마지막 콜백이 request_complete 호출. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_dsm_cmd - NVMe Dataset Management(OPC 09h) opcode 디스패처.
 *
 * @bdev/desc/ch/req: 표준.
 * @return: COMPLETE 또는 ASYNCHRONOUS.
 *
 * DSM 명령은 hint 또는 deallocate 를 호스트가 보내는 명령. CDW11.AD(Attribute - Deallocate)
 * 비트가 1 이면 unmap, 아니면 hint 만 (현재 SPDK 는 hint 무시하고 SUCCESS).
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_process_io_cmd (opcode=DSM) → [본 함수]
 *     → AD=1 면 nvmf_bdev_ctrlr_unmap (멀티 range 처리)
 *     → AD=0 면 즉시 SUCCESS
 */
int
nvmf_bdev_ctrlr_dsm_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	if (cmd->cdw11_bits.dsm.ad) {
		/* [한국어] AD=1 → 실제 unmap 처리. */
		return nvmf_bdev_ctrlr_unmap(bdev, desc, ch, req, NULL);
	}

	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	/* [한국어] hint-only DSM (IDR/IDW/SR/SW 등) — SPDK 는 무시하고 즉시 SUCCESS. */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_copy_cmd - NVMe Copy(OPC 19h, SCC) → spdk_bdev_copy_blocks 디스패치.
 *
 * @bdev/desc/ch/req: 표준.
 * @return: 동기 또는 비동기 완료.
 *
 * Copy 명령은 호스트 데이터 전송 없이 장치 내부에서 src LBA → dst LBA 복사. NVMe Base Spec
 * §7.6 Simple Copy Command. SQE 의 데이터 페이로드는 source range descriptor 배열.
 *   - CDW10/11 = SDLBA (Destination LBA, 64-bit)
 *   - CDW12.NR = number of source ranges - 1
 *   - CDW12.DF = Descriptor Format (0 = 32B Source Range Descriptor)
 *   - CDW12.PRINFOR/PRINFOW = read/write 측 PI 처리
 *   - CDW12.FUA/LR/STCW = Force Unit Access / Limited Retry / Single Source Tag Check Word
 *
 * SPDK 단순화: source range 1개만 지원 (Identify NS 의 MSRC=0 광고). NR>0 이면 거부.
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_process_io_cmd (opcode=COPY) → [본 함수]
 *     → spdk_bdev_copy_blocks
 */
int
nvmf_bdev_ctrlr_copy_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	/* [한국어] 호스트 SQE — CDW10/11(SDLBA), CDW12(NR/DF/PI bits) 파싱 대상. */
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	/* [한국어] CPL 슬롯 — 동기 실패 시 SC/SCT 채움. */
	uint64_t sdlba = ((uint64_t)cmd->cdw11 << 32) + cmd->cdw10;
	/* [한국어] SDLBA(Destination LBA) — CDW10|CDW11 합성. NVMe-oF 와이어가 LE 라 cdw* 가 이미
	 *  호스트 endian 이지만 read/write 의 from_le64 와 달리 직접 << 32 + add 로 합성. */
	struct spdk_nvme_scc_source_range range = { 0 };
	/* [한국어] 추출할 단일 source range descriptor (32B: SLBA 8B + NLB 4B + reserved + ELBAT/EILBRT). */
	struct spdk_iov_xfer ix;
	/* [한국어] payload iov reader — req->iov 에서 32B range 를 순차 추출. */
	int rc;
	/* [한국어] spdk_bdev_copy_blocks() 반환값 — 0(성공·비동기) / -ENOMEM / 기타. */

	SPDK_DEBUGLOG(nvmf, "Copy command: SDLBA %lu, NR %u, desc format %u, PRINFOR %u, "
		      "DTYPE %u, STCW %u, PRINFOW %u, FUA %u, LR %u\n",
		      sdlba,
		      cmd->cdw12_bits.copy.nr,
		      cmd->cdw12_bits.copy.df,
		      cmd->cdw12_bits.copy.prinfor,
		      cmd->cdw12_bits.copy.dtype,
		      cmd->cdw12_bits.copy.stcw,
		      cmd->cdw12_bits.copy.prinfow,
		      cmd->cdw12_bits.copy.fua,
		      cmd->cdw12_bits.copy.lr);
	/* [한국어] 진단 로그 — Copy 명령의 모든 CDW12 필드 덤프. trace 디버깅 단서. */

	if (spdk_unlikely(req->length != (cmd->cdw12_bits.copy.nr + 1) *
			  sizeof(struct spdk_nvme_scc_source_range))) {
		/* [한국어] 호스트가 제공한 SGL 길이가 (NR+1) * 32B 와 일치하지 않으면 capsule 불일치 — 거부. */
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/*
	 * We support only one source range, and rely on this with the xfer
	 * below.
	 */
	if (cmd->cdw12_bits.copy.nr > 0) {
		/* [한국어] SPDK 한도 — Identify NS 에서 MSRC=0 (1개 range) 으로 광고했는데 호스트가 더
		 *  많은 range 요청 — Command Specific 에러로 거부. */
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVME_SC_CMD_SIZE_LIMIT_SIZE_EXCEEDED;
		/* [한국어] SC 0x07 Command Size Limit Exceeded — NVMe spec 에서 MSRC 초과 시 사용. */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->cdw12_bits.copy.df != 0) {
		/* [한국어] Descriptor Format — 0 = 32B SCC range. SPDK 는 0 만 지원. 1/2/3 은 PI 가 추가된
		 *  variant 이며 미지원. */
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
	spdk_iov_xfer_to_buf(&ix, &range, sizeof(range));
	/* [한국어] payload 에서 32B range descriptor 1개를 추출. */

	rc = spdk_bdev_copy_blocks(desc, ch, sdlba, range.slba, range.nlb + 1,
				   nvmf_bdev_ctrlr_complete_cmd, req);
	/* [한국어] bdev 의 copy 제출 — 모듈이 SCC offload 면 device, 아니면 read+write 에뮬레이션.
	 *  range.nlb 는 0-based 라 +1. */
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			/* [한국어] 풀 고갈 → 큐잉. */
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}

		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	/* [한국어] 비동기 — 콜백이 SC/SCT 보고. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_nvme_passthru_io - bdev_nvme 모듈 IO 명령 raw 우회 — SPDK 가 인식 못하는
 *                                     vendor opcode 또는 module-specific opcode 를 NVMe SQE
 *                                     그대로 후방 NVMe 드라이버에 전달.
 *
 * @bdev/desc/ch/req: 표준.
 * @return: 동기 또는 비동기 완료.
 *
 * spdk_bdev_nvme_iov_passthru_md — bdev_nvme 모듈에서만 의미 있는 API. bdev 가 nvme 모듈이면
 * 호스트 SQE 를 후방 SSD 의 NVMe SQ 에 그대로 인큐하고, 다른 모듈이면 -ENOTSUP. INVALID_OPCODE
 * 로 호스트에 회신하면 호스트 드라이버가 명령을 큐에서 제거.
 *
 * 사용 예: NVMe Vendor Specific 명령 (firmware update, 디바이스 진단 명령 등) 또는 ZNS 처럼
 * SPDK 코어 추상화에 미반영된 신규 명령.
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_process_io_cmd (opcode 매칭 실패 → passthru)
 *     → [본 함수] → spdk_bdev_nvme_iov_passthru_md → bdev_nvme → SSD NVMe SQ
 */
int
nvmf_bdev_ctrlr_nvme_passthru_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	int rc;
	/* [한국어] spdk_bdev_nvme_iov_passthru_md() 반환값 — 0(성공·비동기) / -ENOMEM / -ENOTSUP(non-nvme bdev). */

	rc = spdk_bdev_nvme_iov_passthru_md(desc, ch, &req->cmd->nvme_cmd, req->iov, req->iovcnt,
					    req->length, NULL, 0, nvmf_bdev_ctrlr_complete_cmd, req);
	/* [한국어] bdev_nvme 의 passthru — SQE 64B 와 host iov 를 그대로 전달, MD(metadata) 버퍼는
	 *  NULL 로 미사용. 후방 SSD CQE 가 그대로 nvmf_bdev_ctrlr_complete_cmd 로 전달되어 호스트
	 *  CPL 로 변환. */

	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			/* [한국어] 풀 고갈 → 큐잉. */
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		/* [한국어] non-nvme bdev 에서 호출되어 -ENOTSUP — 호스트에 미지원 opcode 로 회신. */
		req->rsp->nvme_cpl.status.dnr = 1;
		/* [한국어] DNR (Do Not Retry) — 호스트 드라이버가 재시도 안 함을 광고. */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

/*
 * [한국어]
 * spdk_nvmf_bdev_ctrlr_nvme_passthru_admin - bdev_nvme 모듈 admin 명령 raw 우회 + 후처리 훅
 *                                             등록.
 *
 * @bdev/desc/ch/req: 표준.
 * @cb_fn:  bdev 완료 후 호출될 후처리 함수 — 호스트 응답 데이터 수정용 (예: Identify Controller
 *          응답에 SPDK 측 필드 덮어쓰기).
 * @return: 동기 또는 비동기 완료.
 *
 * IO passthru 와 차이점:
 *   1) admin 은 응답 데이터 후처리 (cmd_cb_fn) 가 필요할 수 있어 별도 콜백 nvmf_bdev_ctrlr_
 *      complete_admin_cmd 사용.
 *   2) iov 1개만 지원 (admin payload 는 보통 4KB 미만, scattered 불필요).
 *   3) -ENOTSUP 시 INVALID_OPCODE 회신 (IO 와 동일).
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_process_admin_cmd (Identify 등 fall-through)
 *     → [본 함수] → spdk_bdev_nvme_admin_passthru → bdev_nvme → SSD admin SQ
 */
int
spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
		spdk_nvmf_nvme_passthru_cmd_cb cb_fn)
{
	int rc;
	/* [한국어] spdk_bdev_nvme_admin_passthru() 반환값 — 0(성공·비동기) / -ENOMEM / -ENOTSUP. */

	if (spdk_unlikely(req->iovcnt > 1)) {
		/* [한국어] admin passthru 는 단일 buffer 만 — host iov 1 개 초과 capsule 은 구현 단순화로
		 *  거부. */
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		req->rsp->nvme_cpl.status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	req->cmd_cb_fn = cb_fn;
	/* [한국어] 후처리 훅 등록 — 완료 콜백(complete_admin_cmd) 이 먼저 cb_fn 호출 후 일반 변환. */

	rc = spdk_bdev_nvme_admin_passthru(desc, ch, &req->cmd->nvme_cmd, req->iov[0].iov_base, req->length,
					   nvmf_bdev_ctrlr_complete_admin_cmd, req);
	/* [한국어] bdev_nvme admin passthru — 단일 buffer 형식. iov[0].iov_base 가 응답 페이로드 슬롯. */
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			/* [한국어] 풀 고갈 → admin 재제출 콜백 등록. */
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_admin_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		if (rc == -ENOTSUP) {
			/* [한국어] non-nvme bdev → 미지원 opcode. */
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		} else {
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}

		req->rsp->nvme_cpl.status.dnr = 1;
		/* [한국어] 호스트가 재시도 안 함. */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_complete_abort_cmd - Abort admin 명령의 bdev_io 완료 콜백.
 *
 * @bdev_io: 완료된 abort bdev_io.
 * @success: bdev_abort 가 IO 를 실제로 취소했는지.
 * @cb_arg:  Abort 를 발행한 admin request.
 *
 * NVMe Abort 명령 (Admin OPC 08h) 의 응답 CDW0:
 *   bit 0 = 0 → 취소 성공 (target 이 IO 를 취소함)
 *   bit 0 = 1 → 취소 실패 (이미 완료되었거나 취소 불가)
 * 호출자(spdk_nvmf_bdev_ctrlr_abort_cmd) 가 미리 cdw0 의 bit 0 을 1 로 설정해 둠. 본 함수는
 * success=true 면 비트 클리어해 "성공" 광고. 실패면 그대로 1 유지.
 *
 * 호출 체인:
 *   bdev_abort 완료 → 본 함수 → spdk_nvmf_request_complete (Abort 발행자 응답)
 */
static void
nvmf_bdev_ctrlr_complete_abort_cmd(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_nvmf_request *req = cb_arg;
	/* [한국어] Abort 명령 자체의 요청 — abort 대상이 아닌 발행자. */

	if (success) {
		req->rsp->nvme_cpl.cdw0 &= ~1U;
		/* [한국어] CDW0.bit0 클리어 — "취소 성공". 비트마스크 ~1U = 0xFFFFFFFE. */
	}
	/* [한국어] success=false 면 cdw0.bit0 은 호출자가 설정한 1 그대로 — "취소 실패". */

	spdk_nvmf_request_complete(req);
	/* [한국어] 호스트에 Abort 명령의 CPL 전송. */
	spdk_bdev_free_io(bdev_io);
	/* [한국어] abort 용 bdev_io 풀 반납. */
}

/*
 * [한국어]
 * spdk_nvmf_bdev_ctrlr_abort_cmd - NVMe Abort(Admin OPC 08h) → spdk_bdev_abort 디스패치.
 *
 * @bdev/desc/ch:  표준.
 * @req:           Abort 명령 자체의 요청 (응답 대상).
 * @req_to_abort:  취소 대상 request (ctrlr.c 가 CID 검색으로 찾아 전달).
 * @return:        ASYNCHRONOUS (정상 또는 ENOMEM) 또는 COMPLETE (즉시 실패).
 *
 * NVMe Abort 는 best-effort: 이미 모듈로 제출되어 디스크 통신 중인 IO 는 취소 못 할 수 있고,
 * spdk_bdev_abort 는 큐 또는 채널 wait queue 에 있는 IO 만 안전하게 취소. 취소 결과는
 * complete_abort_cmd 에서 CDW0.bit0 으로 보고.
 *
 * 호출 체인:
 *   ctrlr.c::admin handler — Abort opcode 검출 → CID 검색 → req_to_abort 결정
 *     → [본 함수] → spdk_bdev_abort
 *       → 완료 콜백 nvmf_bdev_ctrlr_complete_abort_cmd
 */
int
spdk_nvmf_bdev_ctrlr_abort_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			       struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
			       struct spdk_nvmf_request *req_to_abort)
{
	int rc;
	/* [한국어] spdk_bdev_abort() 반환값 — 0(성공·비동기) / -ENOMEM / 기타(-EINVAL 등). */

	assert((req->rsp->nvme_cpl.cdw0 & 1U) != 0);
	/* [한국어] 호출자가 cdw0.bit0=1 로 사전 설정해야 함 — 콜백에서 success 면 클리어 가능하게.
	 *  호출 시 광고된 contract 위반시 assert. */

	rc = spdk_bdev_abort(desc, ch, req_to_abort, nvmf_bdev_ctrlr_complete_abort_cmd, req);
	/* [한국어] bdev 코어가 채널 큐에서 req_to_abort 검색 — 발견하면 취소 후 success=true,
	 *  발견 못하거나 이미 진행중이면 success=false 로 콜백. */
	if (spdk_likely(rc == 0)) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		/* [한국어] 정상 비동기 — 콜백이 응답. */
	} else if (rc == -ENOMEM) {
		/* [한국어] 풀 고갈 → admin 재제출 큐잉. */
		nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_admin_cmd_resubmit, req);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	} else {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		/* [한국어] 기타 동기 오류 — 호출자가 cdw0 (실패 비트 set 상태) 그대로 회신. */
	}
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_get_dif_ctx - 트랜스포트 측에서 PI 를 strip/insert 처리하기 위한 DIF
 *                                컨텍스트(spdk_dif_ctx) 초기화.
 *
 * @desc:    backing bdev descriptor — block_size/md_size/interleaved/head_of_md 등 PI 형상.
 * @cmd:     NVMe SQE — start_lba 추출 위해 사용.
 * @dif_ctx: [out] 초기화될 DIF 컨텍스트 — 트랜스포트(RDMA/TCP)가 와이어에서 PI 검사·생성 시 사용.
 * @return:  true = 초기화 성공, false = 미지원(메타 없음) 또는 init 실패.
 *
 * NVMe-oF target 의 RDMA/TCP transport 가 호스트와 backing bdev 사이에서 PI 를 직접 다룰 때
 * 필요한 정보를 담은 컨텍스트. 호스트가 PI 포함 LBA 를 보내고 target 이 strip 후 bdev 에 PI
 * 없이 전달, 또는 그 역방향. 본 함수는 LBA 의 Reference Tag 초기값까지 계산해 spdk_dif_ctx
 * 를 채워 transport 가 with-PI ↔ without-PI 변환을 할 수 있게 함.
 *
 * - Reference Tag (Type 1) — 32-bit, 시작 LBA 의 하위 32bit (LBA 마다 +1 ascending).
 * - Application Tag — 호스트 application 데이터 16-bit 식별자 (Type 2/3 만 사용).
 * - Guard — 16/32/64bit CRC.
 * - PRCHK 비트 — 어떤 검사를 활성화할지 결정 (REFTAG/APPTAG/GUARD).
 *
 * 호출 체인:
 *   transport (예: rdma transport) 가 capsule 처리 시
 *     → [본 함수] → spdk_dif_ctx_init → transport 가 dif_ctx 보관 후 read/write 시 strip/insert
 */
bool
nvmf_bdev_ctrlr_get_dif_ctx(struct spdk_bdev_desc *desc, struct spdk_nvme_cmd *cmd,
			    struct spdk_dif_ctx *dif_ctx)
{
	uint32_t init_ref_tag, dif_check_flags = 0;
	/* [한국어] init_ref_tag: Type 1 Ref Tag 초기값 (SLBA[31:0]).
	 *  dif_check_flags: GUARD/APPTAG/REFTAG 활성 비트 조합 (SPDK_DIF_FLAGS_* 상수). */
	int rc;
	/* [한국어] spdk_dif_ctx_init() 반환값 — 0(성공) / 음수(잘못된 인자 조합). */
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	/* [한국어] dif_ctx_init 의 확장 옵션 — size(ABI 호환 partial-size) + dif_pi_format(16b Guard 고정).
	 *  트랜스포트는 16b Guard 만 지원 (NVMe-oF 와이어 표준); 32/64b 는 bdev 레이어 전용. */

	if (spdk_bdev_desc_get_md_size(desc) == 0) {
		/* [한국어] desc 가 PI 없는 view — 트랜스포트 측 strip 불필요. false 반환으로 fast-path. */
		return false;
	}

	/* Initial Reference Tag is the lower 32 bits of the start LBA. */
	init_ref_tag = (uint32_t)from_le64(&cmd->cdw10);
	/* [한국어] NVMe Type 1 PI 정의: Initial Ref Tag = SLBA[31:0]. 이후 LBA 마다 +1 ascending. */

	if (spdk_bdev_desc_is_dif_check_enabled(desc, SPDK_DIF_CHECK_TYPE_REFTAG)) {
		dif_check_flags |= SPDK_DIF_FLAGS_REFTAG_CHECK;
		/* [한국어] desc 가 RefTag 검사 활성 → 비트 set. */
	}

	if (spdk_bdev_desc_is_dif_check_enabled(desc, SPDK_DIF_CHECK_TYPE_GUARD)) {
		dif_check_flags |= SPDK_DIF_FLAGS_GUARD_CHECK;
		/* [한국어] desc 가 Guard 검사 활성 → 비트 set. ApplTag 는 트랜스포트 단계에서 검사 안 함. */
	}

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	/* [한국어] ABI 호환 partial-size — dif_pi_format 까지 채움. */
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;
	/* [한국어] 트랜스포트는 16b Guard PI 만 지원 (TCP/RDMA capsule 의 표준). 32/64b 는 bdev 측에서만. */
	rc = spdk_dif_ctx_init(dif_ctx,
			       spdk_bdev_desc_get_block_size(desc),
			       spdk_bdev_desc_get_md_size(desc),
			       spdk_bdev_desc_is_md_interleaved(desc),
			       spdk_bdev_desc_is_dif_head_of_md(desc),
			       spdk_bdev_desc_get_dif_type(desc),
			       dif_check_flags,
			       init_ref_tag, 0, 0, 0, 0, &dif_opts);
	/* [한국어] DIF 라이브러리가 컨텍스트 초기화 — block_size, md_size, interleaved, head_of_md,
	 *  dif_type(1/2/3), check_flags, init_ref_tag 받음. ApplTag/StorageTag 인자는 0 (트랜스포트
	 *  측은 검사 안 함). */

	return (rc == 0) ? true : false;
	/* [한국어] dif_ctx_init 성공 = true, 실패(잘못된 인자 조합) = false → 트랜스포트가 fallback. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_zcopy_start_complete - zcopy_start 의 bdev_io 완료 콜백 — bdev 가 노출한
 *                                         buffer iov 를 req 에 연결.
 *
 * @bdev_io: 완료된 zcopy_start bdev_io.
 * @success: 성공 여부.
 * @cb_arg:  spdk_nvmf_request.
 *
 * Zero-copy 동작 흐름:
 *   1) 호스트가 read/write 발행 → ctrlr.c 가 zcopy 활성 ns 면 zcopy_start 분기.
 *   2) bdev 모듈이 backing storage 의 실제 buffer 를 iov 로 노출 (예: lvol cluster 의 blob
 *      buffer 직접). 이 buffer 가 본 콜백에 도달.
 *   3) 본 함수가 req->iov 를 bdev_io 의 iov 로 교체 — 이후 transport 가 해당 buffer 로 직접
 *      RDMA WRITE/READ 수행 (호스트 메모리 ↔ bdev buffer 직접). 데이터 복사 0번.
 *   4) zcopy_end 호출 시 bdev_io 가 commit (write) 또는 release (read) 처리.
 *
 * Read 의 경우 bdev 가 데이터를 buffer 에 미리 채워두고 (populate=true), Write 의 경우 빈
 * buffer 만 노출 (populate=false) — 호스트가 데이터를 그 buffer 에 쓰면 zcopy_end 가 commit.
 *
 * 호출 체인:
 *   bdev module zcopy_start 완료 → 본 함수 → spdk_nvmf_request_complete (transport 가 RDMA
 *   READ/WRITE)
 */
static void
nvmf_bdev_ctrlr_zcopy_start_complete(struct spdk_bdev_io *bdev_io, bool success,
				     void *cb_arg)
{
	struct spdk_nvmf_request	*req = cb_arg;
	/* [한국어] zcopy_start 가 비동기로 완료되면 이 요청에 buffer 를 연결하고 transport 에 알림. */
	struct iovec *iov;
	/* [한국어] bdev 가 노출한 backing storage buffer iov 포인터 — 호스트와 zero-copy 로 공유될
	 *  실제 메모리 영역. spdk_bdev_io_get_iovec() 가 설정. */
	int iovcnt = 0;
	/* [한국어] bdev 가 노출한 iov 배열의 항목 수 — 0 초기화 후 get_iovec() 가 채움. */

	if (spdk_unlikely(!success)) {
		/* [한국어] zcopy_start 실패 — bdev 모듈이 backing buffer 노출 도중 I/O 오류(예: lvol
		 *  cluster read 실패). 일반 read/write 와 동일한 에러 변환 후 bdev_io 즉시 해제. */
		int                     sc = 0, sct = 0;
		/* [한국어] NVMe 오류 상태 코드 — bdev_io 에서 추출. */
		uint32_t                cdw0 = 0;
		/* [한국어] CDW0 — zcopy_start 실패는 보통 0. */
		struct spdk_nvme_cpl    *response = &req->rsp->nvme_cpl;
		/* [한국어] CPL 슬롯 — 오류 상태 기록 대상. */
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);
		/* [한국어] bdev 결과 → NVMe SC/SCT 추출. */

		response->cdw0 = cdw0;
		response->status.sc = sc;
		response->status.sct = sct;
		/* [한국어] CPL 슬롯 채움. */

		spdk_bdev_free_io(bdev_io);
		/* [한국어] 실패 시 bdev_io 즉시 반납 — zcopy_end 는 호출 안 됨. */
		spdk_nvmf_request_complete(req);
		return;
	}

	spdk_bdev_io_get_iovec(bdev_io, &iov, &iovcnt);
	/* [한국어] bdev_io 가 보유한 backing buffer iov 를 추출 — 이게 zero-copy 의 핵심. */

	assert(iovcnt <= NVMF_REQ_MAX_BUFFERS);
	assert(iovcnt > 0);
	/* [한국어] 안전 검증 — req 의 iov 배열 슬롯 수와 호환되어야 함. */

	req->iovcnt = iovcnt;
	/* [한국어] iovcnt 갱신 — transport 가 이 만큼만 사용. */

	assert(req->iov == iov);
	/* [한국어] zcopy 모드에서 req->iov 는 bdev 가 채울 위치를 가리키도록 사전 설정되어야 함 —
	 *  포인터 일치 확인 (zero-copy 가 실제로 buffer 를 공유 중인지 sanity). */

	req->zcopy_bdev_io = bdev_io; /* Preserve the bdev_io for the end zcopy */
	/* [한국어] bdev_io 보존 — zcopy_end 시 이 핸들로 commit/release. 일반 IO 처럼 free 하지 않음. */

	spdk_nvmf_request_complete(req);
	/* [한국어] 호스트에 응답 — 단, transport 는 이 시점에 buffer 로 RDMA WRITE/READ 진행. */
	/* Don't free the bdev_io here as it is needed for the END ZCOPY */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_zcopy_start - NVMe Read/Write 의 zero-copy populate 시작.
 *
 * @bdev/desc/ch/req: 표준.
 * @return: 동기 또는 비동기 완료.
 *
 * read_cmd / write_cmd 와 거의 동일하게 LBA 검증 후 spdk_bdev_zcopy_start 호출. 차이점은:
 *   - opcode=READ → populate=true (bdev 가 buffer 에 데이터 미리 채움)
 *   - opcode=WRITE → populate=false (빈 buffer 만 노출 — 호스트가 채울 영역)
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_ctrlr_process_io_cmd (zcopy 활성)
 *     → [본 함수] → spdk_bdev_zcopy_start
 *       → 콜백 nvmf_bdev_ctrlr_zcopy_start_complete
 */
int
nvmf_bdev_ctrlr_zcopy_start(struct spdk_bdev *bdev,
			    struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch,
			    struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	/* [한국어] CPL 슬롯 — 동기 실패(LBA 범위 초과, SGL 부족) 시 SC/SCT 채움. */
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	/* [한국어] bdev 총 LBA — LBA 범위 검사용 상한. */
	uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
	/* [한국어] desc 기준 block_size — SGL 길이(bytes) vs num_blocks*block_size 비교용. */
	uint64_t start_lba;
	/* [한국어] 추출된 SLBA — CDW10|CDW11 합성. */
	uint64_t num_blocks;
	/* [한국어] 추출된 NLB+1 — 실제 전송 블록 수. */
	int rc;
	/* [한국어] spdk_bdev_zcopy_start() 반환값 — 0(성공) / -ENOMEM / 기타. */

	nvmf_bdev_ctrlr_get_rw_params(&req->cmd->nvme_cmd, &start_lba, &num_blocks);
	/* [한국어] SLBA/NLB 추출 — read/write 와 동일. zcopy 는 ext_io_opts 미사용. */

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		/* [한국어] LBA 범위 검사. */
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		/* [한국어] SGL 길이 검사. */
		SPDK_ERRLOG("Read NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bool populate = (req->cmd->nvme_cmd.opc == SPDK_NVME_OPC_READ) ? true : false;
	/* [한국어] populate 결정 — Read 는 buffer 미리 채움(true), Write 는 빈 buffer(false).
	 *  bdev 모듈이 이 비트로 backing storage read 여부 결정. */

	rc = spdk_bdev_zcopy_start(desc, ch, req->iov, req->iovcnt, start_lba,
				   num_blocks, populate, nvmf_bdev_ctrlr_zcopy_start_complete, req);
	/* [한국어] bdev zcopy_start — 모듈이 buffer 노출 후 콜백. */
	if (spdk_unlikely(rc != 0)) {
		if (rc == -ENOMEM) {
			/* [한국어] 풀 고갈 → 큐잉. */
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	/* [한국어] 비동기 — start_complete 콜백이 응답. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_zcopy_end_complete - zcopy_end 의 bdev_io 완료 콜백.
 *
 * @bdev_io: 완료된 zcopy_end bdev_io.
 * @success: commit 성공 여부 (read 의 release 는 항상 success).
 * @cb_arg:  spdk_nvmf_request.
 *
 * Write 시 호스트가 buffer 채운 후 zcopy_end(commit=true) 호출 — bdev 모듈이 backing storage
 * 에 실제 기록. 실패 시 SC 회신.
 *
 * 호출 체인:
 *   bdev module zcopy_end 완료 → 본 함수 → spdk_nvmf_request_complete
 */
static void
nvmf_bdev_ctrlr_zcopy_end_complete(struct spdk_bdev_io *bdev_io, bool success,
				   void *cb_arg)
{
	struct spdk_nvmf_request	*req = cb_arg;
	/* [한국어] zcopy_start 시 등록한 요청 — commit/release 완료 후 응답 대상. */

	if (spdk_unlikely(!success)) {
		/* [한국어] commit 실패 — write zcopy end 에서 backing storage 기록 중 오류. read release 는
		 *  항상 success 이므로 여기에 도달하지 않음. SC 변환 후 CPL 채움. */
		int                     sc = 0, sct = 0;
		/* [한국어] 실패 시 추출할 NVMe SC/SCT. */
		uint32_t                cdw0 = 0;
		/* [한국어] CDW0 — zcopy_end 실패는 일반적으로 0. */
		struct spdk_nvme_cpl    *response = &req->rsp->nvme_cpl;
		/* [한국어] CPL 슬롯 — 실패 결과 기록. */
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);

		response->cdw0 = cdw0;
		response->status.sc = sc;
		response->status.sct = sct;
	}
	/* [한국어] success 면 cpl 은 default 0 (SUCCESS) 그대로. */

	spdk_bdev_free_io(bdev_io);
	/* [한국어] zcopy 의 bdev_io 최종 반납 — start 시 보존했던 핸들. */
	req->zcopy_bdev_io = NULL;
	/* [한국어] req 의 zcopy 핸들 클리어 — 재사용 시 잔여 상태 방지. */
	spdk_nvmf_request_complete(req);
	/* [한국어] 호스트에 commit/release 결과 응답. write 면 호스트가 fsync 의미로 인식. */
}

/*
 * [한국어]
 * nvmf_bdev_ctrlr_zcopy_end - zcopy 사이클 종료 — commit (write) 또는 release (read).
 *
 * @req:    zcopy_start 가 처리한 요청 (zcopy_bdev_io 보유).
 * @commit: write 면 true (실제 디스크 기록), read 면 false (단순 buffer 반환).
 *
 * 트랜스포트가 호스트와의 RDMA 전송을 완료하면 호출. write 의 경우 호스트 데이터가 buffer 에
 * 도착했으므로 commit=true, read 의 경우 호스트가 데이터 수신했으므로 buffer 만 release.
 *
 * 호출 체인:
 *   transport → 호스트 RDMA WRITE/READ 완료 검출
 *     → ctrlr.c → [본 함수] → spdk_bdev_zcopy_end
 *       → 콜백 nvmf_bdev_ctrlr_zcopy_end_complete
 */
void
nvmf_bdev_ctrlr_zcopy_end(struct spdk_nvmf_request *req, bool commit)
{
	int rc __attribute__((unused));
	/* [한국어] spdk_bdev_zcopy_end() 의 반환값 — assert 용. release 빌드에서 assert 가 제거되면
	 *  unused 변수 경고를 막기 위해 __attribute__((unused)) 붙임. */
	/* [한국어] release 빌드에서 assert 가 빠지면 미사용 — unused 어노테이션으로 경고 차단. */

	rc = spdk_bdev_zcopy_end(req->zcopy_bdev_io, commit, nvmf_bdev_ctrlr_zcopy_end_complete, req);
	/* [한국어] zcopy_start 시 보존한 bdev_io 로 commit/release 호출 — bdev 모듈이 storage flush
	 *  또는 buffer 해제 처리. */

	/* The only way spdk_bdev_zcopy_end() can fail is if we pass a bdev_io type that isn't ZCOPY */
	assert(rc == 0);
	/* [한국어] zcopy_end 의 유일한 실패 경로는 bdev_io 가 ZCOPY 타입 아닐 때 — start 가 성공했으면
	 *  타입은 보장되므로 assert 로만 검증. */
}
