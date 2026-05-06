/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * NVMe driver public API extension for Open-Channel
 */

/*
 * [한국어 설명] Open-Channel SSD (OCSSD) 1.2/2.0 호스트 측 공개 API 헤더 (nvme_ocssd.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK가 Open-Channel SSD (OCSSD) 디바이스에 직접 명령을 발행할 수 있도록
 * 제공하는 공개 C API의 선언을 모은다. OCSSD는 일반 NVMe SSD와 달리 SSD 내부의 FTL
 * (Flash Translation Layer)을 호스트가 직접 운영하도록 raw NAND 기하 정보(채널/LUN/플레인/
 * 청크)를 노출하는 디바이스 클래스로, 호스트 애플리케이션(예: 호스트 사이드 FTL,
 * pblk-like 사용자 정의 매핑 계층)이 가비지 컬렉션·웨어 레벨링·매핑 테이블을 직접
 * 관리한다. 본 헤더는 (1) 컨트롤러가 OCSSD를 지원하는지 질의(spdk_nvme_ctrlr_is_ocssd_supported),
 * (2) GEOMETRY admin 커맨드(opcode 0xE2)로 디바이스 기하 식별, (3) Vector I/O
 * 커맨드(opcode 0x90 reset / 0x91 write / 0x92 read / 0x95 copy 등)로 다중 LBA를 한
 * SQE에 인코딩해 발행하는 함수들을 노출한다.
 * OCSSD 사양은 NVMe 표준에 ZNS(Zoned Namespace)가 도입되면서 사실상 폐기 추세이지만,
 * SPDK는 잔존 OCSSD 호환 디바이스(특히 CNEX Labs 1d1d:* 계열)와 기존 사용자 사이드 FTL
 * (lib/ftl)이 의존하는 vector I/O 경로를 위해 본 API를 유지한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 본 헤더는 SPDK 공개 헤더 트리(include/spdk/) 중 NVMe 드라이버(nvme.h)를 OCSSD 전용
 * 명령으로 확장하는 레이어이다. 호출 체인은 다음과 같다:
 *   [사용자 앱 / lib/ftl FTL 매니저]
 *     → [본 헤더의 spdk_nvme_ocssd_* 진입점]
 *       → [lib/nvme/nvme_ctrlr_ocssd_cmd.c (admin: GEOMETRY)]
 *       → [lib/nvme/nvme_ns_ocssd_cmd.c (I/O: vector reset/write/read/copy)]
 *         → nvme_allocate_request_*() (요청 객체 풀에서 nvme_request 할당)
 *         → submit_request() (PCIe SQ에 SQE 작성, MMIO doorbell write)
 *         → [PCIe → NVMe SSD Hardware]
 * 완료 경로는 폴링 모드로 spdk_nvme_qpair_process_completions() → CQE 파싱 → 사용자
 * cb_fn(ctx, cpl) 콜백 호출이며, 콜백은 호출 측이 polling 중인 동일 SPDK 스레드(=동일
 * reactor/코어)에서 실행된다 (lockless 가정). 모든 함수는 유저스페이스 NVMe 드라이버
 * 컨텍스트에서 동작하며, 커널 NVMe 드라이버를 우회한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(이 헤더가 사용하는 것):
 *   - <spdk/stdinc.h>: bool/uint*_t 등 표준 정수 타입.
 *   - <spdk/nvme.h>: spdk_nvme_ctrlr / spdk_nvme_ns / spdk_nvme_qpair / spdk_nvme_cmd_cb
 *     등 베이스 NVMe 드라이버 핵심 객체와 비동기 콜백 시그니처를 가져온다.
 *   - <spdk/nvme_ocssd_spec.h>: OCSSD 1.2/2.0 스펙 정의(GEOMETRY payload 포맷
 *     spdk_ocssd_geometry_data, vector 명령 opcode·io_flags 비트 정의
 *     SPDK_OCSSD_IO_FLAGS_*, chunk_information_entry 포맷 등). 본 헤더에 등장하는
 *     spdk_ocssd_chunk_information_entry는 이 파일에서 정의된다.
 * 역의존(이 헤더를 사용하는 것):
 *   - lib/nvme/nvme_ctrlr_ocssd_cmd.c, lib/nvme/nvme_ns_ocssd_cmd.c (구현)
 *   - lib/ftl/* (사용자 사이드 FTL이 vector I/O로 NAND를 직접 다룬다)
 *   - app/ftl_pmem (예제), test/unit/lib/ftl/* (단위 테스트)
 *   - module/bdev/ocssd 가 한때 존재했고(현재 트리에서는 lib/ftl로 통합) bdev_io를
 *     vector cmd로 변환하는 데 본 API를 호출했다.
 * 데이터 흐름: 사용자 → DMA 가능 메모리(spdk_dma_malloc) 위 lba_list/buffer/metadata →
 *   본 함수가 nvme_request에 PRP/SGL과 vector 명령을 채움 → SQE가 PCIe MMIO를 통해
 *   디바이스로 → 디바이스가 raw NAND read/write/erase 수행 → CQE 반환 → cb_fn 호출.
 *   chunk_info 버퍼는 vector reset 완료 시 디바이스가 청크별 잔여 라이트 포인터/상태를
 *   기록해 호스트 FTL이 wear leveling을 갱신하는 데 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_nvme_ctrlr_is_ocssd_supported(ctrlr)
 *       컨트롤러가 OCSSD인지 검사. 내부적으로 NVME_QUIRK_OCSSD 플래그(=lib/nvme/nvme_quirks.c
 *       에서 CNEX Labs vid 0x1d1d 등에 부여)를 확인한다.
 *   - spdk_nvme_ocssd_ctrlr_cmd_geometry(ctrlr, nsid, payload, size, cb, cb_arg)
 *       GEOMETRY admin 커맨드(opcode 0xE2). 채널/LUN/청크/섹터 수 같은 NAND 기하를
 *       4KB 정렬 페이로드로 받아온다.
 *   - spdk_nvme_ocssd_ns_cmd_vector_reset(ns, qpair, lba_list, num_lbas, chunk_info, cb, cb_arg)
 *       vector reset(=erase) 발행. lba_list의 각 LBA는 청크의 시작 LBA를 가리킨다.
 *   - spdk_nvme_ocssd_ns_cmd_vector_write / vector_write_with_md
 *       다중 LBA로 분산 기록. _with_md 변형은 보호/사용자 메타데이터 버퍼를 동반.
 *   - spdk_nvme_ocssd_ns_cmd_vector_read / vector_read_with_md
 *       다중 LBA에서 분산 판독. 메타데이터 변형은 보호 정보(PI) 또는 사용자 메타 회수.
 *   - spdk_nvme_ocssd_ns_cmd_vector_copy(ns, qpair, dst, src, n, cb, cb_arg, flags)
 *       디바이스 내 NAND-to-NAND 복사. 호스트 FTL의 가비지 컬렉션에서 데이터를 PCIe로
 *       왕복시키지 않고 옮기기 위해 사용.
 *   - spdk_ocssd_chunk_information_entry (외부 정의)
 *       청크 단위 상태/잔여 쓰기 가능 LBA를 담는 64바이트 엔트리 (스펙 문서 §6.2.1).
 */

#ifndef SPDK_NVME_OCSSD_H
#define SPDK_NVME_OCSSD_H

/* [한국어] include guard 매크로. 동일 헤더가 한 번역 단위에서 중복 포함되어 재정의·
 * 재선언 오류가 나는 것을 막는다. SPDK 공개 헤더 전반의 컨벤션. */

#include "spdk/stdinc.h"
/* [한국어] SPDK가 의존하는 표준 C 라이브러리(stdint.h, stdbool.h, sys/types.h 등)를
 * 한 곳에서 묶어주는 래퍼. 본 헤더에서 bool / uint32_t / uint64_t / size_t 가
 * 함수 시그니처에 등장하므로 필요하다. */

#ifdef __cplusplus
extern "C" {
#endif
/* [한국어] C++ 컴파일러에서 본 헤더를 인클루드할 때 C 링키지(name mangling 비활성화)를
 * 강제. SPDK는 C로 작성되었지만 C++ 사용자 애플리케이션(예: RocksDB env, QEMU 빌드 일부)
 * 에서도 직접 호출되기 위해 필요. 닫는 } 는 파일 말미의 동일 #ifdef 블록에 있다. */

#include "spdk/nvme.h"
/* [한국어] 베이스 NVMe 드라이버 공개 헤더. spdk_nvme_ctrlr, spdk_nvme_ns,
 * spdk_nvme_qpair (NVMe Submission/Completion Queue Pair), spdk_nvme_cmd_cb
 * (typedef void (*)(void *cb_arg, const struct spdk_nvme_cpl *cpl)) 등
 * 본 헤더의 시그니처가 의존하는 핵심 타입을 가져온다. */
#include "spdk/nvme_ocssd_spec.h"
/* [한국어] OCSSD 1.2/2.0 와이어 포맷(스펙 레벨 구조체)을 정의. 본 헤더 시그니처에
 * 직접 등장하는 spdk_ocssd_chunk_information_entry 와, 함수 doc-comment 가 참조하는
 * SPDK_OCSSD_IO_FLAGS_* 비트 플래그가 여기서 정의된다. 본 헤더 사용자가 별도로
 * 그 헤더를 인클루드하지 않아도 되도록 미리 끌어들여 준다. */

/**
 * \brief Determine if OpenChannel is supported by the given NVMe controller.
 * \param ctrlr NVMe controller to check.
 *
 * \return true if support OpenChannel
 */
/*
 * [한국어]
 * spdk_nvme_ctrlr_is_ocssd_supported - 주어진 NVMe 컨트롤러가 OCSSD 인지 판별
 *
 * @ctrlr: spdk_nvme_probe()/spdk_nvme_connect() 로 얻은 attached 컨트롤러 핸들. NULL 금지.
 * @return: true=OCSSD 명령(0xE2 GEOMETRY, 0x90~0x95 vector I/O)을 발행해도 안전.
 *          false=일반 NVMe(NVM 커맨드셋만 지원). 호출자는 false 시 본 헤더의 다른
 *          함수를 호출해서는 안 되며, 호출하면 디바이스가 Invalid Opcode CQE 를 반환한다.
 *
 * OCSSD 는 표준 NVMe Identify 의 별도 비트로 노출되지 않으므로, SPDK 는 PCI VID/DID 기반
 * 휴리스틱(lib/nvme/nvme_quirks.c 의 NVME_QUIRK_OCSSD: CNEX Labs vid 0x1d1d 등) 으로
 * 컨트롤러를 식별한다. 본 함수는 그 quirks 비트가 세팅돼 있는지 체크하는 thin wrapper 이며,
 * 디바이스에 어떤 트랜잭션도 발행하지 않는 순수 in-memory 조회이므로 어느 SPDK 스레드에서
 * 호출해도 안전하다 (동시성 이슈 없음 — 컨트롤러 attach 이후 quirks 는 read-only).
 *
 * 호출 체인:
 *   사용자 / lib/ftl 초기화 → spdk_nvme_ctrlr_is_ocssd_supported()
 *                            → (lib/nvme/nvme_ctrlr_ocssd_cmd.c) ctrlr->quirks & NVME_QUIRK_OCSSD
 */
bool spdk_nvme_ctrlr_is_ocssd_supported(struct spdk_nvme_ctrlr *ctrlr);

/**
 * \brief Identify geometry of the given namespace.
 * \param ctrlr NVMe controller to query.
 * \param nsid Id of the given namespace.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer. Shall be multiple of 4K.
 * \param cb_fn Callback function to invoke when the feature has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be
 * allocated for this request, EINVAL if wrong payload size.
 *
 */
/*
 * [한국어]
 * spdk_nvme_ocssd_ctrlr_cmd_geometry - GEOMETRY admin 커맨드 비동기 발행
 *
 * @ctrlr:        OCSSD 컨트롤러 핸들. spdk_nvme_ctrlr_is_ocssd_supported() 로 사전 검증 권장.
 * @nsid:         네임스페이스 ID (1-base). OCSSD 는 보통 단일 NS(=1) 만 노출.
 * @payload:      DMA 가능 버퍼(spdk_dma_malloc 또는 hugepage 매핑) 주소. 디바이스가
 *                spdk_ocssd_geometry_data (4KB) 포맷으로 채널/LUN/플레인/청크 수,
 *                섹터 크기, write/read/erase 단위, 청크 정보 테이블 위치 등을 기록.
 * @payload_size: 버퍼 크기 (바이트). 4096 의 배수여야 한다 (디바이스 전송 단위 제약).
 *                위반 시 EINVAL 반환.
 * @cb_fn:        완료 콜백 (cpl 의 status field 로 성공/실패 판정). qpair 가 admin 큐이므로
 *                spdk_nvme_ctrlr_process_admin_completions() 폴링 컨텍스트에서 호출됨.
 * @cb_arg:       cb_fn 의 첫 인자로 전달되는 사용자 컨텍스트.
 * @return:       0=요청을 admin SQ 에 큐잉 성공 (실제 완료는 비동기).
 *                -ENOMEM=nvme_request 풀 고갈, -EINVAL=payload_size 가 4KB 정렬 아님.
 *
 * 동작:
 *   1) admin 큐 락(혹은 single-thread 가정) 하에 nvme_allocate_request_user_copy()
 *      로 nvme_request 를 잡는다.
 *   2) opcode 0xE2 (Geometry), nsid 를 채운 spdk_nvme_cmd 를 생성하고 PRP/SGL 로
 *      payload 를 가리키게 한다.
 *   3) admin SQ tail doorbell MMIO write 로 디바이스에 통지.
 *   4) 완료 시 cb_fn 호출.
 * 컨텍스트: 통상 컨트롤러를 소유한 단일 SPDK thread (initialization phase 또는 전용
 *   admin polling thread). 멀티스레드에서 동시 호출 시 admin 큐 보호가 호출자 책임.
 *
 * 호출 체인:
 *   사용자 / lib/ftl probe 단계 → spdk_nvme_ocssd_ctrlr_cmd_geometry()
 *     → (lib/nvme/nvme_ctrlr_ocssd_cmd.c) nvme_ctrlr_cmd_geometry()
 *       → nvme_ctrlr_submit_admin_request()
 *         → nvme_qpair_submit_request() → MMIO doorbell write
 */
int spdk_nvme_ocssd_ctrlr_cmd_geometry(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				       void *payload, uint32_t payload_size,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Submits a vector reset command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param lba_list an array of LBAs for processing.
 * LBAs must correspond to the start of chunks to reset.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param num_lbas number of LBAs stored in lba_list
 * \param chunk_info an array of chunk info on DMA-able memory
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
/*
 * [한국어]
 * spdk_nvme_ocssd_ns_cmd_vector_reset - vector reset(=erase) I/O 비동기 발행
 *
 * @ns:         대상 네임스페이스 핸들 (spdk_nvme_ctrlr_get_ns()).
 * @qpair:      이 호출 스레드가 소유한 I/O qpair (NVMe SQ+CQ 한 쌍).
 *              SPDK lockless 규약: qpair 는 단일 스레드(=단일 reactor)에서만 다뤄야 한다.
 * @lba_list:   각 청크의 시작 LBA(=erase 단위 시작점) 배열. DMA 가능 메모리여야 하며
 *              디바이스가 PRP/SGL 로 직접 읽는다 (SQE 의 dword10/11 vector LBA).
 * @num_lbas:   lba_list 길이. 한 번에 erase 할 청크 수. NVMe SQE 1 개에 인코딩 가능한
 *              최대치는 디바이스 GEOMETRY 의 max LBAs per command 에 따른다.
 * @chunk_info: 디바이스가 erase 결과로 각 청크 상태(SPDK_OCSSD_CHUNK_STATE_FREE 등)와
 *              잔여 wear-cycles 를 기록할 출력 버퍼 배열. NULL 허용 여부는 디바이스 의존.
 * @cb_fn/@cb_arg: 비동기 완료 콜백과 사용자 컨텍스트.
 * @return:     0=I/O qpair SQ 에 큐잉 성공. -ENOMEM=nvme_request 풀 고갈.
 *
 * 의미: NAND 블록(=청크) 소거. NVMe NVM 커맨드셋의 Deallocate/Write Zeroes 와는 다른
 * raw erase 로, 청크가 free 상태가 되어 다시 sequential 하게 쓸 수 있게 된다 (호스트
 * 사이드 FTL 의 GC/wear leveling 핵심 동작). OCSSD 1.2: opcode 0x90.
 * 컨텍스트: I/O qpair 소유 SPDK 스레드. 동일 qpair 에 다른 스레드가 동시에 큐잉하면
 * 정의되지 않은 동작이 되며, 완료는 spdk_nvme_qpair_process_completions() 가 폴링.
 *
 * 호출 체인:
 *   사용자 FTL GC 워커 → spdk_nvme_ocssd_ns_cmd_vector_reset()
 *     → (lib/nvme/nvme_ns_ocssd_cmd.c) nvme_ns_ocssd_cmd_vector_reset_*()
 *       → _nvme_ns_cmd_rw 계열 → MMIO SQ doorbell write
 */
int spdk_nvme_ocssd_ns_cmd_vector_reset(struct spdk_nvme_ns *ns,
					struct spdk_nvme_qpair *qpair,
					uint64_t *lba_list, uint32_t num_lbas,
					struct spdk_ocssd_chunk_information_entry *chunk_info,
					spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Submits a vector write command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param buffer virtual address pointer to the data payload
 * \param lba_list an array of LBAs for processing.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param num_lbas number of LBAs stored in lba_list
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_OCSSD_IO_FLAGS_* entries
 * in spdk/nvme_ocssd_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
/*
 * [한국어]
 * spdk_nvme_ocssd_ns_cmd_vector_write - vector write 비동기 발행 (메타데이터 없음)
 *
 * @ns/@qpair:  대상 네임스페이스와 호출 스레드 소유 I/O qpair.
 * @buffer:     데이터 페이로드 가상주소. spdk_dma_malloc() 류로 할당된 hugepage 매핑
 *              버퍼여야 하며, 디바이스가 PRP/SGL 로 DMA 읽기를 수행한다. 총 크기는
 *              num_lbas * spdk_nvme_ns_get_sector_size(ns) 이상.
 * @lba_list:   각 섹터(=write unit) 의 목적지 LBA. 청크/플레인/LUN/채널 비트 인코딩이
 *              스펙(§4.4 PPA addressing) 에 따라 들어간다. 한 vector 명령 내 LBA 들은
 *              동일 chunk 의 시퀀셜 라이트 포인터를 따라야 한다 (OCSSD 의 sequential
 *              write 제약).
 * @num_lbas:   배열 길이. GEOMETRY 의 ws_min/ws_opt 단위로 정렬되어야 디바이스가 수락.
 * @cb_fn/@cb_arg: 비동기 완료 콜백.
 * @io_flags:   SPDK_OCSSD_IO_FLAGS_* (예: LIMITED_RETRY, SCRAMBLER_DISABLE 등).
 *              SQE 의 control field(dword12 의 상위 비트) 에 OR 되어 디바이스에 전달.
 * @return:     0=성공 큐잉, -ENOMEM=request 풀 고갈.
 *
 * 의미: 단일 SQE 로 다중 LBA 에 분산 기록 (NAND plane parallelism 활용). OCSSD 1.2 opcode 0x91.
 * 메타데이터(보호 정보 등) 가 필요하면 _with_md 변형을 사용.
 * 컨텍스트: qpair 소유 SPDK 스레드 단독. 완료는 polling 모드.
 *
 * 호출 체인:
 *   사용자 FTL writer → spdk_nvme_ocssd_ns_cmd_vector_write()
 *     → (lib/nvme/nvme_ns_ocssd_cmd.c) nvme_ns_ocssd_cmd_vector_write()
 *       → MMIO SQ doorbell write
 */
int spdk_nvme_ocssd_ns_cmd_vector_write(struct spdk_nvme_ns *ns,
					struct spdk_nvme_qpair *qpair,
					void *buffer,
					uint64_t *lba_list, uint32_t num_lbas,
					spdk_nvme_cmd_cb cb_fn, void *cb_arg,
					uint32_t io_flags);

/**
 * \brief Submits a vector write command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param buffer virtual address pointer to the data payload
 * \param metadata virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size()
 * \param lba_list an array of LBAs for processing.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param num_lbas number of LBAs stored in lba_list
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_OCSSD_IO_FLAGS_* entries
 * in spdk/nvme_ocssd_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
/*
 * [한국어]
 * spdk_nvme_ocssd_ns_cmd_vector_write_with_md - 메타데이터를 동반한 vector write
 *
 * @ns/@qpair:  대상 네임스페이스, qpair (스레드 단독 소유).
 * @buffer:     데이터 페이로드 (DMA 메모리). PRP1/PRP2 로 디바이스 DMA.
 * @metadata:   섹터별 메타데이터 페이로드. 길이는 num_lbas * spdk_nvme_ns_get_md_size(ns).
 *              NVMe Identify Namespace 의 MS(metadata size) 비트가 0 이면 호출 무의미.
 *              Extended LBA(=메타가 데이터 LBA 끝에 인터리빙) 가 아닌 separate buffer 형식.
 *              SPDK 는 SQE 의 MPTR (metadata pointer, dword 6/7) 로 전달.
 * @lba_list/@num_lbas/@cb_fn/@cb_arg/@io_flags: 위 vector_write 와 동일 의미.
 * @return:     0=큐잉 성공, -ENOMEM=풀 고갈.
 *
 * 의미: 보호 정보(PI: Protection Information, T10 DIF) 또는 사용자 정의 메타데이터를
 * 동반한 분산 기록. 호스트 FTL 이 LBA-PPA 매핑 외에 별도 메타(예: 논리 LBA, 시퀀스 번호)
 * 를 NAND 의 spare area 에 함께 저장하고 싶을 때 사용. OCSSD 1.2 opcode 0x91 + MPTR set.
 *
 * 호출 체인:
 *   사용자 / lib/ftl writer → spdk_nvme_ocssd_ns_cmd_vector_write_with_md()
 *     → nvme_ns_ocssd_cmd_vector_write_with_md() → submit_request → MMIO doorbell
 */
int spdk_nvme_ocssd_ns_cmd_vector_write_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lba_list, uint32_t num_lbas,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags);

/**
 * \brief Submits a vector read command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param buffer virtual address pointer to the data payload
 * \param lba_list an array of LBAs for processing.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param num_lbas number of LBAs stored in lba_list
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_OCSSD_IO_FLAGS_* entries
 * in spdk/nvme_ocssd_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
/*
 * [한국어]
 * spdk_nvme_ocssd_ns_cmd_vector_read - vector read 비동기 발행 (메타데이터 없음)
 *
 * @ns/@qpair:  대상 네임스페이스, 스레드 소유 qpair.
 * @buffer:     데이터를 받을 DMA 가능 버퍼 (디바이스가 PRP 로 DMA 쓰기 수행).
 * @lba_list:   판독 대상 LBA 배열. write 와 달리 read 는 임의 순서/임의 청크 위치
 *              가능 (sequential 제약 없음). 단, ECC unrecovered LBA 면 디바이스가
 *              CQE 의 status field 에 SPDK_OCSSD_SC_READ_HIGH_ECC 등을 보고.
 * @num_lbas:   배열 길이. 한 SQE 최대 LBAs 는 GEOMETRY mccap/maxoc 비트로 결정.
 * @cb_fn/@cb_arg/@io_flags: 표준 의미.
 * @return:     0=큐잉, -ENOMEM=풀 고갈.
 *
 * 의미: 단일 SQE 로 NAND 의 분산된 LBA 들을 한 번에 조회. 호스트 FTL 의 garbage
 * collection 시 valid pages 를 모아 읽어 새 청크에 다시 쓰는 시퀀스의 read leg 에
 * 사용. OCSSD 1.2 opcode 0x92.
 *
 * 호출 체인:
 *   사용자 FTL reader / GC → spdk_nvme_ocssd_ns_cmd_vector_read()
 *     → nvme_ns_ocssd_cmd_vector_read() → submit_request → MMIO doorbell
 */
int spdk_nvme_ocssd_ns_cmd_vector_read(struct spdk_nvme_ns *ns,
				       struct spdk_nvme_qpair *qpair,
				       void *buffer,
				       uint64_t *lba_list, uint32_t num_lbas,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				       uint32_t io_flags);

/**
 * \brief Submits a vector read command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param buffer virtual address pointer to the data payload
 * \param metadata virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size()
 * \param lba_list an array of LBAs for processing.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param num_lbas number of LBAs stored in lba_list
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_OCSSD_IO_FLAGS_* entries
 * in spdk/nvme_ocssd_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
/*
 * [한국어]
 * spdk_nvme_ocssd_ns_cmd_vector_read_with_md - 메타데이터를 회수하는 vector read
 *
 * @ns/@qpair:  대상 네임스페이스, qpair.
 * @buffer:     데이터 수신 DMA 버퍼.
 * @metadata:   섹터별 메타데이터 수신 버퍼. 길이=num_lbas * spdk_nvme_ns_get_md_size(ns).
 *              디바이스가 NAND spare area 에 저장된 메타를 SQE 의 MPTR 로 DMA 쓰기.
 * @lba_list/@num_lbas/@cb_fn/@cb_arg/@io_flags: 표준 의미.
 * @return:     0=큐잉, -ENOMEM=풀 고갈.
 *
 * 의미: write_with_md 로 저장한 메타와 짝이 되는 read 경로. T10 DIF 보호 정보 검증을
 * 호스트가 수행하거나, 호스트 FTL 의 사용자 정의 메타(논리 LBA, ts) 를 함께 회수.
 * OCSSD 1.2 opcode 0x92 + MPTR set.
 *
 * 호출 체인:
 *   사용자 FTL reader → spdk_nvme_ocssd_ns_cmd_vector_read_with_md()
 *     → nvme_ns_ocssd_cmd_vector_read_with_md() → submit_request
 */
int spdk_nvme_ocssd_ns_cmd_vector_read_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lba_list, uint32_t num_lbas,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags);

/**
 * \brief Submits a vector copy command to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the command
 * \param qpair I/O queue pair to submit the request
 * \param dst_lba_list an array of destination LBAs for processing.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param src_lba_list an array of source LBAs for processing.
 * Must be allocated through spdk_dma_malloc() or its variants
 * \param num_lbas number of LBAs stored in src_lba_list and dst_lba_list
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_OCSSD_IO_FLAGS_* entries
 * in spdk/nvme_ocssd_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 */
/*
 * [한국어]
 * spdk_nvme_ocssd_ns_cmd_vector_copy - 디바이스 내부 NAND-to-NAND 복사 발행
 *
 * @ns/@qpair:    대상 네임스페이스, qpair.
 * @dst_lba_list: 목적지 LBA 배열 (DMA 가능 메모리 — 디바이스가 SQE 에서 PRP 로 fetch).
 *                vector_write 와 동일하게 destination 청크의 sequential write pointer
 *                를 따라야 한다.
 * @src_lba_list: 출발지 LBA 배열. 임의 분산 가능. dst_lba_list 와 길이 동일.
 * @num_lbas:     양측 배열 공통 길이.
 * @cb_fn/@cb_arg/@io_flags: 표준 의미.
 * @return:       0=큐잉, -ENOMEM=풀 고갈.
 *
 * 의미: 호스트 메모리를 경유하지 않고(=PCIe 왕복 비용 없이) 디바이스가 직접 NAND 내부에서
 * src 의 데이터를 dst 로 복제. 호스트 사이드 FTL 의 garbage collection 에서 valid page
 * 이주(migration) 를 가속하는 핵심 명령. SQE 1 개로 다중 LBA 동시 복사. OCSSD 1.2 opcode 0x95.
 * 디바이스가 지원하지 않을 수 있으므로 (GEOMETRY mccap 비트로 광고) 사용 전 capability
 * 확인이 권장된다.
 *
 * 호출 체인:
 *   사용자 FTL GC → spdk_nvme_ocssd_ns_cmd_vector_copy()
 *     → nvme_ns_ocssd_cmd_vector_copy() → submit_request → MMIO doorbell
 */
int spdk_nvme_ocssd_ns_cmd_vector_copy(struct spdk_nvme_ns *ns,
				       struct spdk_nvme_qpair *qpair,
				       uint64_t *dst_lba_list, uint64_t *src_lba_list,
				       uint32_t num_lbas,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				       uint32_t io_flags);

#ifdef __cplusplus
}
#endif
/* [한국어] 위쪽 extern "C" { 와 짝이 되는 닫는 중괄호. C++ 컴파일러에서만 활성화. */

#endif
/* [한국어] include guard 종료. 본 헤더가 정의한 매크로/선언이 컴파일 단위에 한 번만
 * 들어가도록 보장. */
