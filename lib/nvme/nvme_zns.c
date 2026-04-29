/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2020, Western Digital Corporation. All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * [한국어 설명] NVMe Zoned Namespace (ZNS) public API 구현 (nvme_zns.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVMe TP(Technical Proposal) 4053 "Zoned Namespaces Command Set" 사양을
 * SPDK 사용자 응용프로그램에 노출하는 **public API 레이어**다. ZNS SSD는 LBA 공간을
 * 고정 크기 zone들로 나누고, 각 zone은 sequential write only(write pointer 위치에서만
 * 쓰기 가능), zone 단위 reset/finish/open/close/offline 같은 라이프사이클을 갖는
 * 스토리지 모델로, conventional NVMe(랜덤 R/W) 와는 다른 사용 패턴을 강제한다.
 * 본 파일이 제공하는 기능은 크게 4개 그룹:
 *   1) **ZNS namespace/controller 메타데이터 조회 게터** — zone size(섹터/바이트),
 *      총 zone 수, max open/active zones, max zone append size 등을 한 줄짜리 inline
 *      함수로 노출. 데이터 소스는 컨트롤러 init 시 캐싱된 `ctrlr->cdata_zns` 와
 *      `ns->nsdata_zns` (Identify ZNS Controller / NS Data Structure 결과).
 *   2) **Zone Append (NVMe opcode 0x7D)** — zone start LBA(zslba)에 데이터를 추가하면
 *      디바이스가 실제 기록된 LBA를 CQE에 회신. write pointer race 없이 동시 다중
 *      writer가 가능 — ZNS의 핵심 처리량 메커니즘. 4가지 변종 제공
 *      (with/without metadata × 평면/벡터 IO).
 *   3) **Zone Management Receive (opcode 0x7A)** — `Report Zones` /
 *      `Extended Report Zones` 로 zone descriptor 배열을 받아 zone 상태/wp 조회.
 *   4) **Zone Management Send (opcode 0x79)** — `Open/Close/Finish/Reset/Offline/
 *      Set Zone Descriptor Extension` 액션. zone 상태 머신 전이의 진입점.
 *
 * 모든 데이터 경로 함수는 비동기 콜백(`spdk_nvme_cmd_cb`) 모델을 따르며, 실제 NVMe
 * 명령 빌드/제출은 `nvme_qpair_submit_request`로 위임한다. 이 파일은 ZNS opcode와
 * cdw10/12/13 비트 레이아웃을 정확히 셋업하는 "포맷터" 역할에 집중한다.
 *
 * **본 파일은 코드 수정 없이 한국어 주석만 추가/보강된 학습 사본**이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe 드라이버 호스트 측 사용자 API 레이어. 호출 체인은:
 *   사용자 애플리케이션 (또는 module/bdev/nvme/bdev_zone.c)
 *     → spdk_nvme_zns_*  (이 파일, ZNS opcode/필드 셋업)
 *       → nvme_ns_cmd_zone_append_with_md  (lib/nvme/nvme_ns_cmd.c, append 한정)
 *       → nvme_qpair_submit_request  (lib/nvme/nvme_qpair.c)
 *         → 트랜스포트별 vtable.qpair_submit_request (PCIe doorbell write 등)
 *           → NVMe 컨트롤러가 zone management/append 처리 → CQE 회신
 *     → poll_group_process_completions → cb_fn(cb_arg, cpl) 호출
 *
 * 게터 함수(spdk_nvme_zns_ns_get_*, spdk_nvme_zns_ctrlr_get_*)는 비동기가 아니라
 * 동기 메모리 읽기다. 컨트롤러 init 시 한번 fetch된 Identify ZNS 결과를
 * `ctrlr->cdata_zns` / `ns->nsdata_zns` 에 보관한 것을 단순 dereference 한다.
 *
 * 실행 컨텍스트: zone append/mgmt 함수는 호출한 SPDK thread = qpair 소유 스레드에서만
 * 실행되어야 하며, completion 콜백도 동일 스레드에서 invoking. ZNS 컨트롤러 자체는
 * lockless 설계(qpair-per-thread)를 그대로 따른다.
 *
 * === 타 모듈과의 연결 ===
 *  - **include/spdk/nvme_zns.h** — 본 파일이 구현하는 public 함수 시그니처/엔서클러
 *    (spdk_nvme_zns_* prototypes), `enum spdk_nvme_zns_zra_report_opts` (REPORT 필터 비트).
 *  - **include/spdk/nvme_spec.h** — `SPDK_NVME_OPC_ZONE_MGMT_RECV` (0x7A),
 *    `SPDK_NVME_OPC_ZONE_MGMT_SEND` (0x79), `SPDK_NVME_OPC_ZONE_APPEND` (0x7D),
 *    Zone Send Action 상수 (CLOSE/OPEN/FINISH/RESET/OFFLINE/SET_ZDE),
 *    Zone Receive Action 상수 (REPORT/EXTENDED_REPORT), ZNS Identify 구조체
 *    (`spdk_nvme_zns_ns_data` lbafe[]/mor/mar, `spdk_nvme_zns_ctrlr_data`).
 *  - **include/spdk/bdev_zone.h** — 본 모듈의 상위 추상(zone 단위 bdev 인터페이스).
 *    bdev_zone.h가 정의한 zone IO 모델을 본 ZNS API가 NVMe 레벨에서 만족시킴.
 *  - **lib/nvme/nvme_internal.h** — `struct spdk_nvme_ns` (id/nsdata/nsdata_zns),
 *    `struct spdk_nvme_ctrlr` (cdata_zns/max_zone_append_size), `struct nvme_request`,
 *    `struct spdk_nvme_cmd` (NVMe SQE), `nvme_allocate_request_*` 헬퍼.
 *  - **lib/nvme/nvme_ns_cmd.c** — `nvme_ns_cmd_zone_append_with_md`,
 *    `nvme_ns_cmd_zone_appendv_with_md` 구현 (실제 SQE 빌드는 거기서, 본 파일은 thin wrapper).
 *  - **lib/nvme/nvme_qpair.c** — `nvme_qpair_submit_request` (모든 Zone Mgmt 명령의 공통 제출 경로).
 *  - **module/bdev/nvme/bdev_zone.c** — bdev_zone 추상화의 nvme 백엔드. 본 파일의 함수
 *    들을 호출하여 bdev 레이어의 zone 의미론을 제공.
 *
 * === 주요 함수/구조체 요약 ===
 *  - `spdk_nvme_zns_ns_get_data` / `_ctrlr_get_data` — Identify ZNS 결과 포인터 반환.
 *  - `spdk_nvme_zns_ns_get_zone_size_sectors/_zone_size/_num_zones/_max_open_zones/
 *     _max_active_zones` — zone 기하학 메타데이터 게터 (LBAFE/MOR/MAR 디코딩).
 *  - `spdk_nvme_zns_ctrlr_get_max_zone_append_size` — Append 한 번에 보낼 수 있는
 *    바이트 한계 (ZASL — Zone Append Size Limit, MDTS와 비슷).
 *  - `spdk_nvme_zns_zone_append[/_with_md/_appendv/_appendv_with_md]` — Zone Append
 *    공개 API 4 변종. metadata/벡터 IO 직교 조합.
 *  - `nvme_zns_zone_mgmt_recv` — Receive opcode SQE 빌드 정적 헬퍼 (ZONE_MGMT_RECV).
 *  - `spdk_nvme_zns_report_zones` / `_ext_report_zones` — Receive 의 REPORT/EXT_REPORT
 *    public API.
 *  - `nvme_zns_zone_mgmt_send` — Send opcode SQE 빌드 정적 헬퍼 (ZONE_MGMT_SEND).
 *  - `spdk_nvme_zns_close/finish/open/reset/offline_zone` — Send 의 5가지 액션 public API.
 *  - `spdk_nvme_zns_set_zone_desc_ext` — Set Zone Descriptor Extension (zone에 사용자
 *    정의 메타데이터 attach).
 *
 * === ZNS 도메인 기초 (TP 4053 요점) ===
 *  - **Zone**: ZNS namespace를 균일하게 나눈 LBA 구간. zsze(LBAFE의 Zone Size)는
 *    포맷별 동일.
 *  - **Zone State Machine**: EMPTY → IMP_OPEN/EXP_OPEN → CLOSED → FULL → RESET → EMPTY.
 *    OFFLINE/READ_ONLY 도 존재. Open/Close/Finish/Reset/Offline 명령으로 전이 강제.
 *  - **Write Pointer (wp)**: zone 내 다음 sequential write 위치. write/append만 가능.
 *  - **MOR (Max Open Resources)**: 동시에 OPEN 상태일 수 있는 zone 수 - 1.
 *    `mor + 1` 이 실제 한계 (NVMe 인코딩 관행).
 *  - **MAR (Max Active Resources)**: 동시에 ACTIVE(OPEN+CLOSED) 일 수 있는 zone 수 - 1.
 *  - **Zone Append (0x7D)**: zslba 만 주면 디바이스가 wp에 기록 후 실제 LBA를 CQE에
 *    회신. lock-free 다중 writer 가능 — 기존 write 명령(0x01)과의 결정적 차이.
 *  - **CDW10/11**: SLBA(64bit, little-endian).  CDW12: NUMD(Number of Dwords - 1) for
 *    Receive,  CDW13: action/feature flags.
 */

/* [한국어] spdk/nvme_zns.h: ZNS public API 헤더 — 함수 prototype 일치 보장. */
#include "spdk/nvme_zns.h"
/* [한국어] nvme_internal.h: 컨트롤러/네임스페이스 내부 구조체와 nvme_request 빌더. */
#include "nvme_internal.h"

/*
 * [한국어]
 * spdk_nvme_zns_ns_get_data - ZNS namespace identify 데이터 포인터 반환.
 *
 * @ns: 대상 namespace 핸들 (spdk_nvme_ctrlr_get_ns로 획득).
 * @return: Identify NS Data Structure(ZNS, CNS=0x05) 결과 포인터. ZNS가 아닌 ns는 NULL.
 *
 * 컨트롤러 init 시 nvme_ns_construct 가 ZNS 네임스페이스에 대해 Identify CNS 0x05를
 * 발행하여 결과를 ns->nsdata_zns에 캐싱. 본 함수는 그 캐시를 그대로 반환하는 thin
 * accessor — 매번 admin 명령을 보내지 않으므로 hot-path에서도 안전.
 *
 * 호출 체인:
 *   사용자/bdev_zone → [spdk_nvme_zns_ns_get_data] → ns->nsdata_zns
 */
const struct spdk_nvme_zns_ns_data *
spdk_nvme_zns_ns_get_data(struct spdk_nvme_ns *ns)
{
	return ns->nsdata_zns; /* [한국어] init 시 캐싱된 Identify ZNS NS 데이터 포인터를 그대로 반환. */
}

/*
 * [한국어]
 * spdk_nvme_zns_ns_get_zone_size_sectors - 한 zone의 크기를 LBA(섹터) 단위로 반환.
 *
 * @ns: 대상 ZNS namespace.
 * @return: zone size (단위: 섹터/LBA). 일반적으로 256MB~2GB 범위.
 *
 * ZNS spec: zone size는 namespace 내 모든 zone에서 동일하며, 현재 LBA Format에 대응하는
 * `lbafe[format_index].zsze` (Zone Size for LBAF Extension) 필드에서 읽는다. 기존 NVMe의
 * lbaf[] 와 1:1 대응되는 ZNS 전용 확장 배열이 lbafe[].
 *
 * 호출 체인:
 *   사용자 / spdk_nvme_zns_ns_get_zone_size / _get_num_zones
 *     → [이 함수] → ns->nsdata_zns->lbafe[format_index].zsze
 */
uint64_t
spdk_nvme_zns_ns_get_zone_size_sectors(struct spdk_nvme_ns *ns)
{
	/* [한국어] ZNS 전용 identify 데이터 (lbafe[]/mor/mar/zoc/...). NULL이면 ZNS 아님. */
	const struct spdk_nvme_zns_ns_data *nsdata_zns = spdk_nvme_zns_ns_get_data(ns);
	/* [한국어] 일반 NVMe identify NS 데이터 (LBA format 정보, sector size 등). */
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);
	uint32_t format_index; /* [한국어] 현재 사용 중인 LBAF index (0~63). */

	/* [한국어] flbas + lbafx 비트를 조합하여 현재 활성 LBA format index 산출.
	 * ZNS lbafe[] 와 일반 lbaf[] 모두 같은 index로 lookup. */
	format_index = spdk_nvme_ns_get_format_index(nsdata);

	/* [한국어] 해당 format의 zone size(섹터 수) 반환. ZNS spec에서 zsze는 64bit. */
	return nsdata_zns->lbafe[format_index].zsze;
}

/*
 * [한국어]
 * spdk_nvme_zns_ns_get_zone_size - zone 크기를 바이트 단위로 반환.
 *
 * @ns: 대상 ZNS namespace.
 * @return: zone size (단위: byte). zone_size_sectors × sector_size.
 *
 * 단순 곱셈 helper. application이 byte offset 기반으로 zone 경계를 계산할 때 편의.
 */
uint64_t
spdk_nvme_zns_ns_get_zone_size(struct spdk_nvme_ns *ns)
{
	/* [한국어] (zone size in sectors) × (sector size) = bytes per zone.
	 * 두 게터 모두 캐싱된 identify 데이터를 읽으므로 이중 호출 비용 무시 가능. */
	return spdk_nvme_zns_ns_get_zone_size_sectors(ns) * spdk_nvme_ns_get_sector_size(ns);
}

/*
 * [한국어]
 * spdk_nvme_zns_ns_get_num_zones - namespace 내 총 zone 개수.
 *
 * @ns: 대상 ZNS namespace.
 * @return: 총 zone 수 (NSZE / zsze 정수 나눗셈).
 *
 * NVMe spec은 ZNS namespace의 LBA 공간이 zone size 배수가 아닐 수도 있다고 허용하지만,
 * 잔여 LBA는 OFFLINE 상태로 표시되고 이 게터의 결과에는 포함되지 않는다.
 */
uint64_t
spdk_nvme_zns_ns_get_num_zones(struct spdk_nvme_ns *ns)
{
	/* [한국어] (총 LBA 수) / (zone당 LBA 수) = zone 개수. 정수 나눗셈으로 잘림. */
	return spdk_nvme_ns_get_num_sectors(ns) / spdk_nvme_zns_ns_get_zone_size_sectors(ns);
}

/*
 * [한국어]
 * spdk_nvme_zns_ns_get_max_open_zones - 동시에 OPEN 상태일 수 있는 zone 수.
 *
 * @ns: 대상 ZNS namespace.
 * @return: max open resources (디바이스가 허용하는 최대 동시 OPEN zone 수).
 *
 * NVMe ZNS 인코딩 관례: identify의 mor 필드는 (실제 한계 - 1)을 보고하므로 1을 더해
 * 사용자에게는 자연수로 반환.
 */
uint32_t
spdk_nvme_zns_ns_get_max_open_zones(struct spdk_nvme_ns *ns)
{
	const struct spdk_nvme_zns_ns_data *nsdata_zns = spdk_nvme_zns_ns_get_data(ns);

	return nsdata_zns->mor + 1; /* [한국어] +1: spec의 0-based 인코딩 보정. */
}

/*
 * [한국어]
 * spdk_nvme_zns_ns_get_max_active_zones - 동시에 ACTIVE(OPEN+CLOSED) zone 수.
 *
 * @ns: 대상 ZNS namespace.
 * @return: max active resources.
 *
 * ACTIVE = IMP_OPEN + EXP_OPEN + CLOSED. mar 필드도 0-based이므로 +1 보정.
 */
uint32_t
spdk_nvme_zns_ns_get_max_active_zones(struct spdk_nvme_ns *ns)
{
	const struct spdk_nvme_zns_ns_data *nsdata_zns = spdk_nvme_zns_ns_get_data(ns);

	return nsdata_zns->mar + 1; /* [한국어] +1: spec의 0-based 인코딩 보정. */
}

/*
 * [한국어]
 * spdk_nvme_zns_ctrlr_get_data - ZNS controller identify 데이터 포인터 반환.
 *
 * @ctrlr: 대상 컨트롤러 핸들.
 * @return: Identify Controller Data Structure(ZNS, CSI=0x02) 결과 포인터. ZNS 미지원 시 NULL.
 *
 * 컨트롤러 init 시 1회 fetch한 데이터를 ctrlr->cdata_zns에 캐싱. 직접 반환만 함.
 */
const struct spdk_nvme_zns_ctrlr_data *
spdk_nvme_zns_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cdata_zns; /* [한국어] init 시 캐싱된 Identify ZNS Controller 데이터. */
}

/*
 * [한국어]
 * spdk_nvme_zns_ctrlr_get_max_zone_append_size - 단일 Zone Append 명령의 바이트 한계(ZASL).
 *
 * @ctrlr: 대상 컨트롤러 (const — 이 함수는 read-only).
 * @return: 최대 zone append 크기 (단위: byte).
 *
 * NVMe spec의 ZASL(Zone Append Size Limit, exponent로 인코딩됨)을 init 단계에서
 * 바이트 단위로 풀어 max_zone_append_size에 저장한 값을 반환. write/MDTS와 별도.
 * 사용자가 zone_append 단일 호출의 buffer 크기를 결정할 때 사용.
 */
uint32_t
spdk_nvme_zns_ctrlr_get_max_zone_append_size(const struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->max_zone_append_size; /* [한국어] init 시 ZASL exponent를 바이트로 변환한 캐시. */
}

/*
 * [한국어]
 * spdk_nvme_zns_zone_append - Zone Append (NVMe opcode 0x7D, no metadata, contiguous buffer).
 *
 * @ns: 대상 ZNS namespace.
 * @qpair: 명령을 제출할 qpair (사용자 스레드 affinity).
 * @buffer: 쓰기 데이터의 가상 주소 (DPDK hugepage / NVMe DMA 버퍼).
 * @zslba: Zone Start LBA — 어느 zone의 wp에 append 할지 식별.
 * @lba_count: 쓰기 LBA 개수 (NLB+1, NVMe SQE의 NLB 인코딩으로 변환됨).
 * @cb_fn: 완료 콜백 (CQE 도착 시 process_completions에서 호출).
 * @cb_arg: 완료 콜백의 컨텍스트 인자.
 * @io_flags: 부가 IO 플래그 (FUA, LR 등 SPDK_NVME_IO_FLAGS_*).
 * @return: 0 성공(요청 큐잉됨, 완료는 cb_fn으로), -ENOMEM 등 동기 에러.
 *
 * Zone Append는 일반 write(0x01)와 달리 wp를 명시하지 않으며, 디바이스가 wp에 기록 후
 * 실제 LBA를 CQE의 dword 0/1에 회신. 동시 다중 writer가 wp race 없이 가능 → ZNS의
 * 처리량 핵심 기능. 본 thin wrapper는 metadata=NULL, apptag=0 으로 with_md 변종에 위임.
 *
 * 호출 체인:
 *   사용자/bdev_zone → [이 함수] → nvme_ns_cmd_zone_append_with_md
 *     → nvme_qpair_submit_request → 트랜스포트 vtable → CQE → cb_fn
 */
int
spdk_nvme_zns_zone_append(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			  void *buffer, uint64_t zslba,
			  uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			  uint32_t io_flags)
{
	/* [한국어] metadata=NULL, apptag_mask=0, apptag=0 인 with_md 변종으로 위임.
	 * with_md 함수가 metadata==NULL이면 PI/메타 처리 생략. */
	return nvme_ns_cmd_zone_append_with_md(ns, qpair, buffer, NULL, zslba, lba_count,
					       cb_fn, cb_arg, io_flags, 0, 0);
}

/*
 * [한국어]
 * spdk_nvme_zns_zone_append_with_md - Zone Append with separate metadata buffer.
 *
 * @ns/qpair/buffer/zslba/lba_count/cb_fn/cb_arg/io_flags: 위와 동일.
 * @metadata: separate metadata buffer (E2E PI/사용자 메타). DPS=0 인 경우 사용자 직접 채움.
 * @apptag_mask: PI Application Tag mask (E2E 보호 비교 마스크).
 * @apptag: PI Application Tag 기대값.
 * @return: 0 성공(요청 큐잉됨), 음수 에러.
 *
 * NVMe 메타데이터는 (a) interleaved (sector + metadata 인접 저장) 또는 (b) separated
 * (별도 metadata buffer) 두 모드. 본 함수는 (b)를 위한 진입점. PI 활성화된 namespace
 * 라면 apptag/apptag_mask는 컨트롤러가 검사.
 */
int
spdk_nvme_zns_zone_append_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				  void *buffer, void *metadata, uint64_t zslba,
				  uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				  uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag)
{
	/* [한국어] 인자 그대로 nvme_ns_cmd_zone_append_with_md 에 전달.
	 * 본 파일은 public API 안정성 wrapper, 실제 SQE 빌드는 nvme_ns_cmd.c. */
	return nvme_ns_cmd_zone_append_with_md(ns, qpair, buffer, metadata, zslba, lba_count,
					       cb_fn, cb_arg, io_flags, apptag_mask, apptag);
}

/*
 * [한국어]
 * spdk_nvme_zns_zone_appendv - Vector(SGL) Zone Append, no metadata.
 *
 * @ns/qpair/zslba/lba_count/cb_fn/cb_arg/io_flags: 위와 동일.
 * @reset_sgl_fn: SGL 반복자 리셋 콜백 (드라이버가 SGL 처음부터 다시 읽을 때).
 * @next_sge_fn: SGL 다음 entry 읽기 콜백 (사용자 정의 SGL 생성기).
 * @return: 0 성공, 음수 에러.
 *
 * 단일 buffer 대신 사용자가 SGL iterator 콜백으로 scatter buffer를 공급. SPDK는
 * 트랜스포트가 SGL을 지원하면 그대로 전달, 아니면 PRP로 변환. 큰 IO를 zero-copy로
 * 보낼 때 유용.
 */
int
spdk_nvme_zns_zone_appendv(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   uint64_t zslba, uint32_t lba_count,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			   spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			   spdk_nvme_req_next_sge_cb next_sge_fn)
{
	/* [한국어] metadata=NULL, apptag_mask=0, apptag=0 인 with_md 변종으로 위임. */
	return nvme_ns_cmd_zone_appendv_with_md(ns, qpair, zslba, lba_count, cb_fn, cb_arg,
						io_flags, reset_sgl_fn, next_sge_fn,
						NULL, 0, 0);
}

/*
 * [한국어]
 * spdk_nvme_zns_zone_appendv_with_md - Vector Zone Append with metadata.
 *
 * @ns/qpair/zslba/lba_count/cb_fn/cb_arg/io_flags/reset_sgl_fn/next_sge_fn/metadata/
 * apptag_mask/apptag: 위 두 함수의 인자 합집합.
 * @return: 0 성공, 음수 에러.
 *
 * Vector(SGL) + metadata 가 모두 있는 가장 일반적 형태. PI/E2E 활성 ZNS 시 사용.
 */
int
spdk_nvme_zns_zone_appendv_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				   uint64_t zslba, uint32_t lba_count,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				   spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				   spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				   uint16_t apptag_mask, uint16_t apptag)
{
	/* [한국어] 모든 인자 그대로 nvme_ns_cmd_zone_appendv_with_md 에 전달. */
	return nvme_ns_cmd_zone_appendv_with_md(ns, qpair, zslba, lba_count, cb_fn, cb_arg,
						io_flags, reset_sgl_fn, next_sge_fn,
						metadata, apptag_mask, apptag);
}

/*
 * [한국어]
 * nvme_zns_zone_mgmt_recv - Zone Management Receive (opcode 0x7A) SQE 빌드/제출 헬퍼.
 *
 * @ns: 대상 namespace.
 * @qpair: 제출 qpair.
 * @payload: 디바이스가 채워서 회신할 호스트 버퍼 (zone descriptor 배열 등).
 * @payload_size: payload 바이트 크기 (4 byte aligned).
 * @slba: 시작 LBA. 보통 0(전체) 또는 특정 zone 시작 LBA.
 * @zone_recv_action: SPDK_NVME_ZONE_REPORT(0x00) | SPDK_NVME_ZONE_EXTENDED_REPORT(0x01).
 * @zra_spec_field: ZNS Receive Action Specific (REPORT 시 zone state filter).
 * @zra_spec_feats: ZRA Specific Features (REPORT 시 partial-report 비트).
 * @cb_fn / cb_arg: 완료 콜백.
 * @return: 0 성공(큐잉됨), -ENOMEM 등 동기 에러.
 *
 * NVMe SQE 레이아웃 (Zone Mgmt Receive):
 *   CDW10/11: SLBA (64bit little-endian)
 *   CDW12   : NUMD (Number of Dwords - 1, 즉 (payload_size/4) - 1)
 *   CDW13   : [7:0] ZRA action, [15:8] ZRA spec field, [16] ZRA spec feats(partial bit)
 *
 * 데이터 방향이 device → host (Receive) 이므로 buffer는 사용자 메모리이고, SPDK가
 * `nvme_allocate_request_user_copy(..., is_write=false)` 로 DMA 가능 메모리로 복사 후
 * 완료 시 사용자 buffer 로 복사.
 *
 * static — 본 파일의 spdk_nvme_zns_report_zones / _ext_report_zones 만이 호출.
 *
 * 호출 체인:
 *   spdk_nvme_zns_report_zones / _ext_report_zones
 *     → [이 함수] → nvme_qpair_submit_request → 트랜스포트 → CQE → cb_fn
 */
static int
nvme_zns_zone_mgmt_recv(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			void *payload, uint32_t payload_size, uint64_t slba,
			uint8_t zone_recv_action, uint8_t zra_spec_field, bool zra_spec_feats,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req; /* [한국어] SPDK 내부 IO 요청 객체 (qpair에 큐잉되는 단위). */
	struct spdk_nvme_cmd *cmd; /* [한국어] NVMe SQE (Submission Queue Entry) — 64바이트. */

	/* [한국어] 사용자 buffer는 DMA 가능성이 보장되지 않으므로 SPDK가 내부 hugepage 풀에서
	 * 임시 buffer를 잡고 완료 시 user buffer로 copy back. 마지막 인자 false = 읽기 IO
	 * (device→host). 풀 고갈 시 NULL 반환 → -ENOMEM 즉시 반환. */
	req = nvme_allocate_request_user_copy(qpair, payload, payload_size, cb_fn, cb_arg, false);
	if (req == NULL) {
		return -ENOMEM; /* [한국어] 동기 에러: 사용자가 backoff 후 재시도해야 함. */
	}

	cmd = &req->cmd; /* [한국어] req에 임베드된 SQE 영역 — 이 함수가 직접 채움. */
	cmd->opc = SPDK_NVME_OPC_ZONE_MGMT_RECV; /* [한국어] NVMe opcode 0x7A — Zone Mgmt Receive. */
	cmd->nsid = ns->id; /* [한국어] 대상 namespace ID — admin이 아닌 NVM 명령은 NSID 필수. */

	/* [한국어] CDW10/11 = SLBA (64bit). cdw10 주소를 64bit 포인터로 캐스팅 후 한 번에 기록.
	 * NVMe SQE는 little-endian이므로 host가 LE인 한 그대로 OK. */
	*(uint64_t *)&cmd->cdw10 = slba;
	/* [한국어] CDW12 = NUMD = (payload_size/4) - 1. spdk_nvme_bytes_to_numd가 변환.
	 * NVMe spec: log/zone receive 모두 payload 크기를 dword 개수 - 1 로 인코딩. */
	cmd->cdw12 = spdk_nvme_bytes_to_numd(payload_size);
	/* [한국어] CDW13 비트필드: [7:0]=action, [15:8]=ZRA spec field(report state filter),
	 * [16]=ZRA spec feats(partial bit). bool→int 변환은 0/1 보장되므로 안전. */
	cmd->cdw13 = zone_recv_action | zra_spec_field << 8 | zra_spec_feats << 16;

	/* [한국어] qpair sq에 넣고 doorbell write까지 완료시킴. 완료는 비동기. */
	return nvme_qpair_submit_request(qpair, req);
}

/*
 * [한국어]
 * spdk_nvme_zns_report_zones - Report Zones (Zone Mgmt Receive, action=REPORT) public API.
 *
 * @ns/qpair/payload/payload_size/slba/cb_fn/cb_arg: 위 helper와 동일.
 * @report_opts: zone state 필터 (예: ALL, EMPTY, IMP_OPEN, EXP_OPEN, CLOSED, FULL,
 *               READ_ONLY, OFFLINE) — enum spdk_nvme_zns_zra_report_opts.
 * @partial_report: true면 buffer 부족 시 가능한 만큼만 보고(NULL terminator 없음).
 * @return: 0 성공(큐잉됨), 음수 에러.
 *
 * payload 레이아웃: [Report Header (16 byte: 64bit nr_zones + reserved)] +
 *                   [zone descriptor[] (각 64 byte)]
 */
int
spdk_nvme_zns_report_zones(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   void *payload, uint32_t payload_size, uint64_t slba,
			   enum spdk_nvme_zns_zra_report_opts report_opts, bool partial_report,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	/* [한국어] action=SPDK_NVME_ZONE_REPORT(0x00). report_opts는 spec field로 인코딩,
	 * partial_report는 spec feats 비트. helper가 CDW13에 조합. */
	return nvme_zns_zone_mgmt_recv(ns, qpair, payload, payload_size, slba,
				       SPDK_NVME_ZONE_REPORT, report_opts, partial_report,
				       cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_nvme_zns_ext_report_zones - Extended Report Zones (action=EXTENDED_REPORT).
 *
 * 인자/return은 spdk_nvme_zns_report_zones 와 동일.
 *
 * Extended Report는 각 zone descriptor 뒤에 디바이스가 정의한 zone descriptor extension
 * (예: 압축률, 사용자 정의 메타) 을 함께 회신. 단순 REPORT 의 superset.
 */
int
spdk_nvme_zns_ext_report_zones(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       void *payload, uint32_t payload_size, uint64_t slba,
			       enum spdk_nvme_zns_zra_report_opts report_opts, bool partial_report,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	/* [한국어] action=SPDK_NVME_ZONE_EXTENDED_REPORT(0x01). 그 외 동일. */
	return nvme_zns_zone_mgmt_recv(ns, qpair, payload, payload_size, slba,
				       SPDK_NVME_ZONE_EXTENDED_REPORT, report_opts, partial_report,
				       cb_fn, cb_arg);
}

/*
 * [한국어]
 * nvme_zns_zone_mgmt_send - Zone Management Send (opcode 0x79) SQE 빌드/제출 헬퍼.
 *
 * @ns: 대상 namespace.
 * @qpair: 제출 qpair.
 * @slba: 대상 zone의 시작 LBA. select_all=true 일 때는 무시(SQE에 기록 안 함).
 * @select_all: true면 모든 zone에 액션 적용 (CDW13[8] = Select All bit).
 * @zone_send_action: 액션 코드 (CLOSE/OPEN/FINISH/RESET/OFFLINE/SET_ZDE).
 * @cb_fn / cb_arg: 완료 콜백.
 * @return: 0 성공, -ENOMEM 등 동기 에러.
 *
 * NVMe SQE 레이아웃 (Zone Mgmt Send):
 *   CDW10/11: SLBA (select_all=true 면 의미 없음, 0 또는 미설정)
 *   CDW13   : [7:0] Zone Send Action, [8] Select All, 나머지 reserved
 *
 * 데이터 전송 없는(`null`) 명령. SET_ZDE 만 예외이며 그건 별도 함수
 * (spdk_nvme_zns_set_zone_desc_ext)에서 처리. 따라서 본 helper는
 * `nvme_allocate_request_null`로 0 byte payload req를 만든다.
 *
 * static — 본 파일의 close/finish/open/reset/offline_zone 5함수만 호출.
 */
static int
nvme_zns_zone_mgmt_send(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			uint64_t slba, bool select_all, uint8_t zone_send_action,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req; /* [한국어] qpair에 큐잉할 SPDK 요청 객체. */
	struct spdk_nvme_cmd *cmd; /* [한국어] NVMe SQE 64바이트. */

	/* [한국어] 데이터 페이로드 없음(SET_ZDE 외 모든 zone send action). */
	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM; /* [한국어] req 풀 고갈. 사용자가 backoff 후 재시도. */
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_ZONE_MGMT_SEND; /* [한국어] NVMe opcode 0x79. */
	cmd->nsid = ns->id; /* [한국어] 대상 NS — NVM 명령 필수. */

	if (!select_all) {
		/* [한국어] 단일 zone 대상: SLBA 필수. select_all=true면 디바이스가 SLBA 무시,
		 * 그래도 0 초기화돼 있으면 무관 (calloc된 req). */
		*(uint64_t *)&cmd->cdw10 = slba;
	}

	/* [한국어] CDW13: [7:0] action, [8] Select All bit. select_all 은 bool→int.
	 * spec 상 select_all=true 시 "namespace 모든 zone에 일괄 적용" — RESET ALL이 자주 사용. */
	cmd->cdw13 = zone_send_action | select_all << 8;

	return nvme_qpair_submit_request(qpair, req); /* [한국어] sq enqueue + doorbell. 비동기 완료. */
}

/*
 * [한국어]
 * spdk_nvme_zns_close_zone - Zone Close (action=0x01).
 *
 * @ns/qpair/slba/select_all/cb_fn/cb_arg: 표준 zone send 인자.
 * @return: 0 성공, 음수 에러.
 *
 * Zone State 전이: IMP_OPEN/EXP_OPEN → CLOSED. write resource 해제 (open count↓),
 * 그러나 zone에 기록된 데이터/wp 위치는 보존. 다시 write/append하면 IMP_OPEN으로 복귀.
 */
int
spdk_nvme_zns_close_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t slba,
			 bool select_all, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_zns_zone_mgmt_send(ns, qpair, slba, select_all, SPDK_NVME_ZONE_CLOSE,
				       cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_nvme_zns_finish_zone - Zone Finish (action=0x02).
 *
 * @return: 0 성공, 음수 에러.
 *
 * Zone State 전이: 임의 상태 → FULL. wp를 zone 끝(zslba+zsze)으로 강제 이동시켜
 * 더 이상 write 받지 않게 만듬. 사전 종료가 필요한 호스트 정책에 사용.
 */
int
spdk_nvme_zns_finish_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t slba,
			  bool select_all, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_zns_zone_mgmt_send(ns, qpair, slba, select_all, SPDK_NVME_ZONE_FINISH,
				       cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_nvme_zns_open_zone - Zone Open (action=0x03), Explicit Open.
 *
 * @return: 0 성공, 음수 에러.
 *
 * Zone State 전이: EMPTY/CLOSED → EXP_OPEN. 명시적으로 write resource(open count) 1
 * 차지. 사용자가 곧 write할 zone을 미리 알리는 hint — 펌웨어가 buffer 미리 할당.
 */
int
spdk_nvme_zns_open_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t slba,
			bool select_all, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_zns_zone_mgmt_send(ns, qpair, slba, select_all, SPDK_NVME_ZONE_OPEN,
				       cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_nvme_zns_reset_zone - Zone Reset (action=0x04).
 *
 * @return: 0 성공, 음수 에러.
 *
 * Zone State 전이: 임의 상태 → EMPTY. wp를 zslba로 reset, 데이터는 erase된 것으로 간주
 * (이후 read 시 invalid LBA 또는 zero, namespace 정책에 따름). select_all=true 가 자주 쓰임.
 */
int
spdk_nvme_zns_reset_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t slba,
			 bool select_all, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_zns_zone_mgmt_send(ns, qpair, slba, select_all, SPDK_NVME_ZONE_RESET,
				       cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_nvme_zns_offline_zone - Zone Offline (action=0x05).
 *
 * @return: 0 성공, 음수 에러.
 *
 * Zone State 전이: READ_ONLY → OFFLINE. 디바이스가 미디어 결함을 감지해 READ_ONLY로
 * 강등된 zone을 사용자가 영구 폐기 처리할 때 사용. OFFLINE zone은 더 이상 어떤 명령도
 * 수락하지 않음.
 */
int
spdk_nvme_zns_offline_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t slba,
			   bool select_all, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_zns_zone_mgmt_send(ns, qpair, slba, select_all, SPDK_NVME_ZONE_OFFLINE,
				       cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_nvme_zns_set_zone_desc_ext - Set Zone Descriptor Extension (action=0x10) public API.
 *
 * @ns: 대상 namespace.
 * @qpair: 제출 qpair.
 * @slba: 대상 zone 의 zslba (해당 zone 식별).
 * @buffer: 디바이스에 attach 할 zone descriptor extension 데이터.
 * @payload_size: buffer 바이트 크기 (>0, ZDES * dword 와 일치해야 함).
 * @cb_fn / cb_arg: 완료 콜백.
 * @return: 0 성공, -EINVAL(buffer/size 잘못), -ENOMEM(req 풀 고갈).
 *
 * 다른 zone send action과 달리 host→device 데이터 전송이 있으므로 별도 구현.
 * Identify ZNS NS의 ZDES 필드에 정의된 길이 만큼 사용자 메타데이터를 zone에 attach.
 * 이후 EXTENDED REPORT로 회수 가능.
 *
 * Zone State 요구: zone이 EMPTY 상태여야 SET_ZDE 가능 (spec). 이 함수에서는 검증
 * 안 하고 컨트롤러에 위임 → 실패는 CQE의 status code로 보고.
 */
int
spdk_nvme_zns_set_zone_desc_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				uint64_t slba, void *buffer, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req; /* [한국어] qpair에 큐잉할 SPDK 요청 객체. */
	struct spdk_nvme_cmd *cmd; /* [한국어] NVMe SQE. */

	/* [한국어] 0 byte payload는 의미 없음 → 인자 검증 실패. */
	if (payload_size == 0) {
		return -EINVAL;
	}

	/* [한국어] payload_size > 0 인데 buffer 가 NULL 이면 잘못된 호출. */
	if (buffer == NULL) {
		return -EINVAL;
	}

	/* [한국어] 마지막 인자 true = 쓰기 IO (host→device). SPDK가 hugepage에 buffer 복사 후 DMA.
	 * 풀 고갈 시 -ENOMEM. */
	req = nvme_allocate_request_user_copy(qpair, buffer, payload_size, cb_fn, cb_arg, true);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_ZONE_MGMT_SEND; /* [한국어] opcode 0x79 — 모든 zone send. */
	cmd->nsid = ns->id; /* [한국어] 대상 NS. */

	/* [한국어] CDW10/11 = SLBA. SET_ZDE는 단일 zone 대상이므로 select_all 분기 없음. */
	*(uint64_t *)&cmd->cdw10 = slba;

	/* [한국어] CDW13 = action 코드만 (SPDK_NVME_ZONE_SET_ZDE = 0x10). select_all bit 없음. */
	cmd->cdw13 = SPDK_NVME_ZONE_SET_ZDE;

	return nvme_qpair_submit_request(qpair, req); /* [한국어] sq enqueue + doorbell. 비동기 완료. */
}
