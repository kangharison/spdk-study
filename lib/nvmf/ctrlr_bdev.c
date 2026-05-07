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

void
nvmf_bdev_ctrlr_identify_iocs_nvm(struct spdk_nvmf_ns *ns,
				  struct spdk_nvme_nvm_ns_data *nsdata_nvm)
{
	struct spdk_bdev_desc *desc = ns->desc;
	uint8_t _16bpists;
	uint32_t sts, pif;

	if (spdk_bdev_desc_get_dif_type(desc) == SPDK_DIF_DISABLE) {
		return;
	}

	pif = spdk_bdev_desc_get_dif_pi_format(desc);

	/*
	 * 16BPISTS shall be 1 for 32/64b Guard PI.
	 * STCRS shall be 1 if 16BPISTS is 1.
	 * 16 is the minimum value of STS for 32b Guard PI.
	 */
	switch (pif) {
	case SPDK_DIF_PI_FORMAT_16:
		_16bpists = 0;
		sts = 0;
		break;
	case SPDK_DIF_PI_FORMAT_32:
		_16bpists = 1;
		sts = 16;
		break;
	case SPDK_DIF_PI_FORMAT_64:
		_16bpists = 1;
		sts = 0;
		break;
	default:
		SPDK_WARNLOG("PI format %u is not supported\n", pif);
		return;
	}

	/* For 16b Guard PI, Storage Tag is not available because we set STS to 0.
	 * In this case, we do not have to set 16BPISTM to 1. For simplicity,
	 * set 16BPISTM to 0 and set LBSTM to all zeroes.
	 *
	 * We will revisit here when we find any OS uses Storage Tag.
	 */
	nsdata_nvm->lbstm = 0;
	nsdata_nvm->pic._16bpistm = 0;

	nsdata_nvm->pic._16bpists = _16bpists;
	nsdata_nvm->pic.stcrs = 0;
	nsdata_nvm->elbaf[0].sts = sts;
	nsdata_nvm->elbaf[0].pif = pif;
}

static void
nvmf_bdev_ctrlr_get_rw_params(const struct spdk_nvme_cmd *cmd, uint64_t *start_lba,
			      uint64_t *num_blocks)
{
	/* SLBA: CDW10 and CDW11 */
	*start_lba = from_le64(&cmd->cdw10);

	/* NLB: CDW12 bits 15:00, 0's based */
	*num_blocks = (from_le32(&cmd->cdw12) & 0xFFFFu) + 1;
}

static void
nvmf_bdev_ctrlr_get_rw_ext_params(const struct spdk_nvme_cmd *cmd,
				  struct spdk_bdev_ext_io_opts *opts,
				  bool set_dif_mask)
{
	/* Get CDW12 values */
	opts->nvme_cdw12.raw = from_le32(&cmd->cdw12);

	/* Get CDW13 values */
	opts->nvme_cdw13.raw = from_le32(&cmd->cdw13);

	/* Bdev layer checks PRACT in CDW12 because it is NVMe specific, but
	 * it does not check DIF check flags in CDW because DIF is not NVMe
	 * specific. Hence, copy DIF check flags from CDW12 to dif_check_flags_exclude_mask.
	 */
	if (set_dif_mask) {
		opts->dif_check_flags_exclude_mask = (~opts->nvme_cdw12.raw) & SPDK_NVME_IO_FLAGS_PRCHK_MASK;
	}
}

static bool
nvmf_bdev_ctrlr_lba_in_range(uint64_t bdev_num_blocks, uint64_t io_start_lba,
			     uint64_t io_num_blocks)
{
	if (io_start_lba + io_num_blocks > bdev_num_blocks ||
	    io_start_lba + io_num_blocks < io_start_lba) {
		return false;
	}

	return true;
}

static void
nvmf_ctrlr_process_io_cmd_resubmit(void *arg)
{
	struct spdk_nvmf_request *req = arg;
	int rc;

	rc = nvmf_ctrlr_process_io_cmd(req);
	if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		spdk_nvmf_request_complete(req);
	}
}

static void
nvmf_ctrlr_process_admin_cmd_resubmit(void *arg)
{
	struct spdk_nvmf_request *req = arg;
	int rc;

	rc = nvmf_ctrlr_process_admin_cmd(req);
	if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		spdk_nvmf_request_complete(req);
	}
}

static void
nvmf_bdev_ctrl_queue_io(struct spdk_nvmf_request *req, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn, void *cb_arg)
{
	int rc;

	req->bdev_io_wait.bdev = bdev;
	req->bdev_io_wait.cb_fn = cb_fn;
	req->bdev_io_wait.cb_arg = cb_arg;

	rc = spdk_bdev_queue_io_wait(bdev, ch, &req->bdev_io_wait);
	if (rc != 0) {
		assert(false);
	}
	req->qpair->group->stat.pending_bdev_io++;
}

bool
nvmf_bdev_zcopy_enabled(struct spdk_bdev *bdev)
{
	return spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ZCOPY);
}

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
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);
	nvmf_bdev_ctrlr_get_rw_ext_params(cmd, &opts, !req->dif_enabled);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Read NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	assert(!spdk_nvmf_request_using_zcopy(req));

	rc = spdk_bdev_readv_blocks_ext(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
					nvmf_bdev_ctrlr_complete_cmd, req, &opts);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

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
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);
	nvmf_bdev_ctrlr_get_rw_ext_params(cmd, &opts, !req->dif_enabled);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Write NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	assert(!spdk_nvmf_request_using_zcopy(req));

	rc = spdk_bdev_writev_blocks_ext(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
					 nvmf_bdev_ctrlr_complete_cmd, req, &opts);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_compare_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Compare NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_comparev_blocks(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
				       nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_compare_and_write_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				      struct spdk_io_channel *ch, struct spdk_nvmf_request *cmp_req, struct spdk_nvmf_request *write_req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
	struct spdk_nvme_cmd *cmp_cmd = &cmp_req->cmd->nvme_cmd;
	struct spdk_nvme_cmd *write_cmd = &write_req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &write_req->rsp->nvme_cpl;
	uint64_t write_start_lba, cmp_start_lba;
	uint64_t write_num_blocks, cmp_num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmp_cmd, &cmp_start_lba, &cmp_num_blocks);
	nvmf_bdev_ctrlr_get_rw_params(write_cmd, &write_start_lba, &write_num_blocks);

	if (spdk_unlikely(write_start_lba != cmp_start_lba || write_num_blocks != cmp_num_blocks)) {
		SPDK_ERRLOG("Fused command start lba / num blocks mismatch\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, write_start_lba,
			  write_num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(write_num_blocks * block_size > write_req->length)) {
		SPDK_ERRLOG("Write NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    write_num_blocks, block_size, write_req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_comparev_and_writev_blocks(desc, ch, cmp_req->iov, cmp_req->iovcnt, write_req->iov,
			write_req->iovcnt, write_start_lba, write_num_blocks, nvmf_bdev_ctrlr_complete_cmd, write_req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(cmp_req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, cmp_req);
			nvmf_bdev_ctrl_queue_io(write_req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, write_req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_write_zeroes_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t max_write_zeroes_size = req->qpair->ctrlr->subsys->max_write_zeroes_size_kib;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);
	if (spdk_unlikely(max_write_zeroes_size > 0 &&
			  num_blocks > (max_write_zeroes_size << 10) / spdk_bdev_desc_get_block_size(desc))) {
		SPDK_ERRLOG("invalid write zeroes size, should not exceed %" PRIu64 "Kib\n", max_write_zeroes_size);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(cmd->cdw12_bits.write_zeroes.deac)) {
		SPDK_ERRLOG("Write Zeroes Deallocate is not supported\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_write_zeroes_blocks(desc, ch, start_lba, num_blocks,
					   nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_flush_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	int rc;

	/* As for NVMeoF controller, SPDK always set volatile write
	 * cache bit to 1, return success for those block devices
	 * which can't support FLUSH command.
	 */
	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH)) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_SUCCESS;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_flush_blocks(desc, ch, 0, spdk_bdev_get_num_blocks(bdev),
				    nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

struct nvmf_bdev_ctrlr_unmap {
	struct spdk_nvmf_request	*req;
	uint32_t			count;
	struct spdk_bdev_desc		*desc;
	struct spdk_bdev		*bdev;
	struct spdk_io_channel		*ch;
	uint32_t			range_index;
};

static void
nvmf_bdev_ctrlr_unmap_cpl(struct spdk_bdev_io *bdev_io, bool success,
			  void *cb_arg)
{
	struct nvmf_bdev_ctrlr_unmap *unmap_ctx = cb_arg;
	struct spdk_nvmf_request	*req = unmap_ctx->req;
	struct spdk_nvme_cpl		*response = &req->rsp->nvme_cpl;
	int				sc, sct;
	uint32_t			cdw0;

	unmap_ctx->count--;

	if (response->status.sct == SPDK_NVME_SCT_GENERIC &&
	    response->status.sc == SPDK_NVME_SC_SUCCESS) {
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);
		response->cdw0 = cdw0;
		response->status.sc = sc;
		response->status.sct = sct;
	}

	if (unmap_ctx->count == 0) {
		spdk_nvmf_request_complete(req);
		free(unmap_ctx);
	}
	spdk_bdev_free_io(bdev_io);
}

static int nvmf_bdev_ctrlr_unmap(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
				 struct nvmf_bdev_ctrlr_unmap *unmap_ctx);
static void
nvmf_bdev_ctrlr_unmap_resubmit(void *arg)
{
	struct nvmf_bdev_ctrlr_unmap *unmap_ctx = arg;
	struct spdk_nvmf_request *req = unmap_ctx->req;
	struct spdk_bdev_desc *desc = unmap_ctx->desc;
	struct spdk_bdev *bdev = unmap_ctx->bdev;
	struct spdk_io_channel *ch = unmap_ctx->ch;

	nvmf_bdev_ctrlr_unmap(bdev, desc, ch, req, unmap_ctx);
}

static int
nvmf_bdev_ctrlr_unmap(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		      struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
		      struct nvmf_bdev_ctrlr_unmap *unmap_ctx)
{
	uint16_t nr, i;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	uint64_t max_discard_size = req->qpair->ctrlr->subsys->max_discard_size_kib;
	uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
	struct spdk_iov_xfer ix;
	uint64_t lba;
	uint32_t lba_count;
	int rc;

	nr = cmd->cdw10_bits.dsm.nr + 1;
	if (nr * sizeof(struct spdk_nvme_dsm_range) > req->length) {
		SPDK_ERRLOG("Dataset Management number of ranges > SGL length\n");
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (unmap_ctx == NULL) {
		unmap_ctx = calloc(1, sizeof(*unmap_ctx));
		if (!unmap_ctx) {
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		unmap_ctx->req = req;
		unmap_ctx->desc = desc;
		unmap_ctx->ch = ch;
		unmap_ctx->bdev = bdev;

		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_SUCCESS;
	} else {
		unmap_ctx->count--;	/* dequeued */
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);

	for (i = unmap_ctx->range_index; i < nr; i++) {
		struct spdk_nvme_dsm_range dsm_range = { 0 };

		spdk_iov_xfer_to_buf(&ix, &dsm_range, sizeof(dsm_range));

		lba = dsm_range.starting_lba;
		lba_count = dsm_range.length;
		if (max_discard_size > 0 && lba_count > (max_discard_size << 10) / block_size) {
			SPDK_ERRLOG("invalid unmap size %" PRIu32 " blocks, should not exceed %" PRIu64 " blocks\n",
				    lba_count, max_discard_size << 1);
			response->status.sct = SPDK_NVME_SCT_GENERIC;
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			break;
		}

		unmap_ctx->count++;

		rc = spdk_bdev_unmap_blocks(desc, ch, lba, lba_count,
					    nvmf_bdev_ctrlr_unmap_cpl, unmap_ctx);
		if (rc) {
			if (rc == -ENOMEM) {
				nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_bdev_ctrlr_unmap_resubmit, unmap_ctx);
				/* Unmap was not yet submitted to bdev */
				/* unmap_ctx->count will be decremented when the request is dequeued */
				return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
			}
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			unmap_ctx->count--;
			/* We can't return here - we may have to wait for any other
				* unmaps already sent to complete */
			break;
		}
		unmap_ctx->range_index++;
	}

	if (unmap_ctx->count == 0) {
		free(unmap_ctx);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_dsm_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	if (cmd->cdw11_bits.dsm.ad) {
		return nvmf_bdev_ctrlr_unmap(bdev, desc, ch, req, NULL);
	}

	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
nvmf_bdev_ctrlr_copy_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	uint64_t sdlba = ((uint64_t)cmd->cdw11 << 32) + cmd->cdw10;
	struct spdk_nvme_scc_source_range range = { 0 };
	struct spdk_iov_xfer ix;
	int rc;

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

	if (spdk_unlikely(req->length != (cmd->cdw12_bits.copy.nr + 1) *
			  sizeof(struct spdk_nvme_scc_source_range))) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/*
	 * We support only one source range, and rely on this with the xfer
	 * below.
	 */
	if (cmd->cdw12_bits.copy.nr > 0) {
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVME_SC_CMD_SIZE_LIMIT_SIZE_EXCEEDED;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->cdw12_bits.copy.df != 0) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
	spdk_iov_xfer_to_buf(&ix, &range, sizeof(range));

	rc = spdk_bdev_copy_blocks(desc, ch, sdlba, range.slba, range.nlb + 1,
				   nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}

		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_nvme_passthru_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	int rc;

	rc = spdk_bdev_nvme_iov_passthru_md(desc, ch, &req->cmd->nvme_cmd, req->iov, req->iovcnt,
					    req->length, NULL, 0, nvmf_bdev_ctrlr_complete_cmd, req);

	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		req->rsp->nvme_cpl.status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
		spdk_nvmf_nvme_passthru_cmd_cb cb_fn)
{
	int rc;

	if (spdk_unlikely(req->iovcnt > 1)) {
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		req->rsp->nvme_cpl.status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	req->cmd_cb_fn = cb_fn;

	rc = spdk_bdev_nvme_admin_passthru(desc, ch, &req->cmd->nvme_cmd, req->iov[0].iov_base, req->length,
					   nvmf_bdev_ctrlr_complete_admin_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_admin_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		if (rc == -ENOTSUP) {
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		} else {
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}

		req->rsp->nvme_cpl.status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static void
nvmf_bdev_ctrlr_complete_abort_cmd(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_nvmf_request *req = cb_arg;

	if (success) {
		req->rsp->nvme_cpl.cdw0 &= ~1U;
	}

	spdk_nvmf_request_complete(req);
	spdk_bdev_free_io(bdev_io);
}

int
spdk_nvmf_bdev_ctrlr_abort_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			       struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
			       struct spdk_nvmf_request *req_to_abort)
{
	int rc;

	assert((req->rsp->nvme_cpl.cdw0 & 1U) != 0);

	rc = spdk_bdev_abort(desc, ch, req_to_abort, nvmf_bdev_ctrlr_complete_abort_cmd, req);
	if (spdk_likely(rc == 0)) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	} else if (rc == -ENOMEM) {
		nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_admin_cmd_resubmit, req);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	} else {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

bool
nvmf_bdev_ctrlr_get_dif_ctx(struct spdk_bdev_desc *desc, struct spdk_nvme_cmd *cmd,
			    struct spdk_dif_ctx *dif_ctx)
{
	uint32_t init_ref_tag, dif_check_flags = 0;
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;

	if (spdk_bdev_desc_get_md_size(desc) == 0) {
		return false;
	}

	/* Initial Reference Tag is the lower 32 bits of the start LBA. */
	init_ref_tag = (uint32_t)from_le64(&cmd->cdw10);

	if (spdk_bdev_desc_is_dif_check_enabled(desc, SPDK_DIF_CHECK_TYPE_REFTAG)) {
		dif_check_flags |= SPDK_DIF_FLAGS_REFTAG_CHECK;
	}

	if (spdk_bdev_desc_is_dif_check_enabled(desc, SPDK_DIF_CHECK_TYPE_GUARD)) {
		dif_check_flags |= SPDK_DIF_FLAGS_GUARD_CHECK;
	}

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;
	rc = spdk_dif_ctx_init(dif_ctx,
			       spdk_bdev_desc_get_block_size(desc),
			       spdk_bdev_desc_get_md_size(desc),
			       spdk_bdev_desc_is_md_interleaved(desc),
			       spdk_bdev_desc_is_dif_head_of_md(desc),
			       spdk_bdev_desc_get_dif_type(desc),
			       dif_check_flags,
			       init_ref_tag, 0, 0, 0, 0, &dif_opts);

	return (rc == 0) ? true : false;
}

static void
nvmf_bdev_ctrlr_zcopy_start_complete(struct spdk_bdev_io *bdev_io, bool success,
				     void *cb_arg)
{
	struct spdk_nvmf_request	*req = cb_arg;
	struct iovec *iov;
	int iovcnt = 0;

	if (spdk_unlikely(!success)) {
		int                     sc = 0, sct = 0;
		uint32_t                cdw0 = 0;
		struct spdk_nvme_cpl    *response = &req->rsp->nvme_cpl;
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);

		response->cdw0 = cdw0;
		response->status.sc = sc;
		response->status.sct = sct;

		spdk_bdev_free_io(bdev_io);
		spdk_nvmf_request_complete(req);
		return;
	}

	spdk_bdev_io_get_iovec(bdev_io, &iov, &iovcnt);

	assert(iovcnt <= NVMF_REQ_MAX_BUFFERS);
	assert(iovcnt > 0);

	req->iovcnt = iovcnt;

	assert(req->iov == iov);

	req->zcopy_bdev_io = bdev_io; /* Preserve the bdev_io for the end zcopy */

	spdk_nvmf_request_complete(req);
	/* Don't free the bdev_io here as it is needed for the END ZCOPY */
}

int
nvmf_bdev_ctrlr_zcopy_start(struct spdk_bdev *bdev,
			    struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch,
			    struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_desc_get_block_size(desc);
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(&req->cmd->nvme_cmd, &start_lba, &num_blocks);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Read NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bool populate = (req->cmd->nvme_cmd.opc == SPDK_NVME_OPC_READ) ? true : false;

	rc = spdk_bdev_zcopy_start(desc, ch, req->iov, req->iovcnt, start_lba,
				   num_blocks, populate, nvmf_bdev_ctrlr_zcopy_start_complete, req);
	if (spdk_unlikely(rc != 0)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static void
nvmf_bdev_ctrlr_zcopy_end_complete(struct spdk_bdev_io *bdev_io, bool success,
				   void *cb_arg)
{
	struct spdk_nvmf_request	*req = cb_arg;

	if (spdk_unlikely(!success)) {
		int                     sc = 0, sct = 0;
		uint32_t                cdw0 = 0;
		struct spdk_nvme_cpl    *response = &req->rsp->nvme_cpl;
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);

		response->cdw0 = cdw0;
		response->status.sc = sc;
		response->status.sct = sct;
	}

	spdk_bdev_free_io(bdev_io);
	req->zcopy_bdev_io = NULL;
	spdk_nvmf_request_complete(req);
}

void
nvmf_bdev_ctrlr_zcopy_end(struct spdk_nvmf_request *req, bool commit)
{
	int rc __attribute__((unused));

	rc = spdk_bdev_zcopy_end(req->zcopy_bdev_io, commit, nvmf_bdev_ctrlr_zcopy_end_complete, req);

	/* The only way spdk_bdev_zcopy_end() can fail is if we pass a bdev_io type that isn't ZCOPY */
	assert(rc == 0);
}
