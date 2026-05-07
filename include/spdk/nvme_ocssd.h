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

/* [한국어] include guard 매크로 (#ifndef/#define/#endif 페어).
 * 역할: 동일 헤더가 한 번역 단위(translation unit)에서 두 번 이상 포함되더라도
 *       두 번째부터는 SPDK_NVME_OCSSD_H 가 이미 정의되어 있으므로 본문 전체가
 *       skip 되어, 함수/구조체 재선언으로 인한 컴파일 오류를 막는다.
 * 설정자: 첫 인클루드 시 #define 라인이 매크로를 정의.
 * 읽는 자: 두 번째 이후의 #ifndef 가 동일 매크로의 정의 여부를 본다.
 * 값 범위: 정의됨 / 미정의 의 두 상태 — 값 자체는 의미 없고 존재 여부만 의미.
 * 동기화: 전처리 단계의 토큰이라 런타임 동시성과 무관.
 * SPDK 컨벤션: 모든 공개 헤더가 SPDK_<PATH>_H 형태의 가드를 사용. */

#include "spdk/stdinc.h"
/* [한국어] SPDK가 의존하는 표준 C 라이브러리(stdint.h, stdbool.h, sys/types.h, errno.h 등)를
 * 한 곳에서 묶어주는 래퍼 헤더.
 * 왜 필요한가: 본 헤더의 함수 시그니처에 bool, uint32_t, uint64_t, size_t 가 등장하므로
 * 표준 정수/불리언 타입 정의가 선행되어야 한다. SPDK 는 플랫폼별(Linux/FreeBSD) 차이를
 * stdinc.h 한 곳에서 흡수해 #include 의 순서/조합 부담을 줄인다. */

#ifdef __cplusplus
extern "C" {
#endif
/* [한국어] C++ 컴파일러에서 본 헤더를 인클루드할 때 C 링키지(name mangling 비활성화)를 강제.
 * 동기: SPDK 자체는 C 로 작성되었지만 C++ 애플리케이션(예: RocksDB SPDK env,
 *       QEMU 빌드 일부, 외부 OCSSD FTL 사용자) 이 직접 호출할 수 있어야 한다.
 *       extern "C" 가 없으면 C++ 측에서 함수가 _Z*** 형태로 mangling 되어
 *       libspdk_nvme.a 의 심볼과 매칭되지 않아 링커 에러가 난다.
 * 닫는 } 는 파일 말미의 또 다른 #ifdef __cplusplus 블록에서 짝맞춤.
 * 컴파일 분기: __cplusplus 미정의(=C 컴파일러) 시 분기 자체가 사라져 영향 없음. */

#include "spdk/nvme.h"
/* [한국어] 베이스 NVMe 드라이버 공개 헤더. 본 헤더가 OCSSD 전용 명령으로 확장하는
 * 대상 API. 제공받는 핵심 심볼:
 *   - struct spdk_nvme_ctrlr  : 컨트롤러 핸들 (PCIe BAR/admin queue 추상화)
 *   - struct spdk_nvme_ns     : 네임스페이스 핸들 (LBA 공간/섹터 크기 메타)
 *   - struct spdk_nvme_qpair  : Submission/Completion Queue 한 쌍 (I/O qpair)
 *   - typedef spdk_nvme_cmd_cb: void (*)(void *cb_arg, const struct spdk_nvme_cpl *cpl)
 *                               완료 콜백 시그니처. 본 헤더 모든 함수가 이 타입을 받는다.
 * 인클루드 누락 시 본 헤더의 prototype 들이 미선언 식별자로 컴파일 실패. */
#include "spdk/nvme_ocssd_spec.h"
/* [한국어] OCSSD 1.2/2.0 와이어 포맷(스펙 레벨 구조체)을 정의하는 헤더.
 * 제공받는 핵심 심볼:
 *   - struct spdk_ocssd_geometry_data       : 4KB GEOMETRY payload 포맷.
 *   - struct spdk_ocssd_chunk_information_entry : 64B 청크 상태 엔트리 (스펙 §6.2.1).
 *     본 헤더의 vector_reset 시그니처에 직접 등장.
 *   - SPDK_OCSSD_IO_FLAGS_*  : vector I/O 명령의 control field(SQE dword12 상위)
 *     비트 플래그 (LIMITED_RETRY=0x80000000, SCRAMBLER_DISABLE=0x40000000 등 스펙 §4.5).
 *   - SPDK_OCSSD_OPC_*       : opcode 상수 (vector reset/write/read/copy = 0x90/0x91/0x92/0x95,
 *                              GEOMETRY = 0xE2 등).
 * 사용자가 본 헤더만 인클루드해도 vector I/O 호출에 필요한 비트 플래그를 함께
 * 사용할 수 있도록 미리 끌어들여 준다 (편의상 의도적인 transitive include). */

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
 * @ctrlr: spdk_nvme_probe()/spdk_nvme_connect() 로 얻어 attach 가 완료된 컨트롤러 핸들.
 *         NULL 금지(본 함수 내부에서 ctrlr->quirks 를 역참조하므로 NULL 시 SIGSEGV).
 *         호출자는 spdk_nvme_attach_cb 에서 받은 포인터, 혹은 spdk_nvme_first_ctrlr/
 *         next_ctrlr 등으로 획득한 살아있는 컨트롤러를 넘겨야 한다.
 * @return: true  = OCSSD 명령(0xE2 GEOMETRY admin / 0x90~0x95 vector I/O)을 발행해도 안전.
 *          false = 일반 NVMe(NVM 커맨드셋만 지원). 호출자는 false 시 본 헤더의 다른
 *                  함수를 호출해서는 안 되며, 무리하게 호출하면 디바이스가
 *                  status code = Invalid Opcode (0x01) 의 CQE 를 반환한다.
 *
 * 동기/배경:
 *   OCSSD 1.2 는 NVMe Identify Controller / Identify Namespace 의 별도 표준 비트로
 *   디바이스가 OCSSD 임을 광고하지 않는다 (스펙이 NVMe 위에 얹힌 비공식 확장이기 때문).
 *   따라서 SPDK 는 PCI Vendor/Device ID 기반 휴리스틱으로 컨트롤러를 식별한다 —
 *   lib/nvme/nvme_quirks.c 에서 NVME_QUIRK_OCSSD 가 부여되는 vid 의 대표는 CNEX Labs
 *   (vid 0x1d1d) 이다. 본 함수는 그 quirks 비트가 세팅돼 있는지를 체크하는
 *   thin wrapper 이며, 디바이스에 어떤 트랜잭션도 발행하지 않는 순수 in-memory 조회.
 *
 * 동작 단계:
 *   1) ctrlr->quirks (uint32_t 비트필드) 를 읽는다.
 *   2) NVME_QUIRK_OCSSD 비트가 1 이면 true, 아니면 false 반환.
 *
 * 실행 컨텍스트: 사용자/스터디 코드 어디서나 호출 가능. 컨트롤러 attach 이후 quirks 는
 *   read-only 로 굳어지므로 멀티 SPDK thread 환경에서도 락 없이 안전.
 * 에러 경로: 디바이스 트랜잭션이 없으므로 외부 에러 자체가 없다. 호출자가 NULL 또는
 *   detached 컨트롤러를 넘긴 경우 정의되지 않은 동작 (segfault 가능) — 호출자 책임.
 * 동시성: 락 불필요 (read-only). 동일 ctrlr 에 대해 여러 reactor 가 동시에 호출해도
 *   결과 일관성 보장.
 *
 * 호출 체인:
 *   사용자 앱 / lib/ftl 초기화 → spdk_nvme_ctrlr_is_ocssd_supported()
 *                              → (lib/nvme/nvme_ctrlr_ocssd_cmd.c) (ctrlr->quirks & NVME_QUIRK_OCSSD)
 */
bool spdk_nvme_ctrlr_is_ocssd_supported(struct spdk_nvme_ctrlr *ctrlr);
/* [한국어] 함수 시그니처 라인.
 * - 반환 타입 bool : <stdbool.h> 의 _Bool 별칭. spdk/stdinc.h 에서 가져온 정의.
 * - 인자 struct spdk_nvme_ctrlr * : 불투명 포인터 (구조체 본체는 lib/nvme/nvme_internal.h).
 *   사용자는 멤버에 직접 접근할 수 없고, SPDK 가 제공하는 getter API 로만 다뤄야 한다. */

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
 *                일반 NVMe 컨트롤러에 호출하면 디바이스가 Invalid Opcode CQE 반환.
 * @nsid:         네임스페이스 ID (1-base, 0 은 무효). OCSSD 는 보통 단일 NS(=1) 만 노출.
 *                값은 SQE 의 NSID 필드(dword 1) 에 그대로 들어간다.
 * @payload:      DMA 가능 버퍼(spdk_dma_malloc / spdk_zmalloc 또는 hugepage 매핑) 주소.
 *                디바이스가 spdk_ocssd_geometry_data (4KB) 포맷으로 다음을 기록:
 *                  - num_grp/num_pu/num_chk/clba (그룹/PU/청크/LBA 수)
 *                  - ws_min/ws_opt (minimum/optimal write size, sector 단위)
 *                  - mw_cunits   (cache units before read-after-write 가능)
 *                  - maxoc/maxocpu (max open chunks, total / per PU)
 *                  - 등 raw NAND 기하. 자세히는 OCSSD 2.0 스펙 §3.2.2 참고.
 * @payload_size: 버퍼 크기 (바이트). 4096 의 배수여야 한다 (디바이스 DMA 전송 단위 제약).
 *                위반 시 -EINVAL 반환 (디바이스 발행 전 사전 검증).
 * @cb_fn:        완료 콜백 시그니처 void (*)(void *cb_arg, const struct spdk_nvme_cpl *cpl).
 *                cpl 의 status.sc/sct 로 성공/실패 판정. admin 큐이므로
 *                spdk_nvme_ctrlr_process_admin_completions() 폴링 컨텍스트에서 호출.
 *                cb_fn 미설정(NULL) 은 일반적으로 허용되지 않음 — 결과 회수 불가.
 * @cb_arg:       cb_fn 의 첫 인자로 전달되는 사용자 컨텍스트 (보통 자기 자신의 상태 객체).
 * @return:        0     = 요청을 admin SQ 에 큐잉 성공 (실제 완료는 비동기, cb_fn 으로 회신).
 *                -ENOMEM= nvme_request 풀 고갈 (in-flight 요청이 큐 깊이 초과).
 *                -EINVAL= payload_size 가 4KB 정렬 아님 / payload NULL 등.
 *
 * 동기/배경: 호스트 사이드 FTL(lib/ftl, app/ftl_pmem 등) 이 OCSSD 디바이스의 raw NAND
 *   기하 — 채널/LUN(parallel unit)/플레인/청크/섹터 수, sequential write 단위(ws_min),
 *   open chunk 한계(maxoc) 등 — 를 알아야 매핑 테이블 크기 산정, sequential 라이트
 *   파이프라인 설계, GC 정책을 수립할 수 있다. 본 함수는 그 메타 회수 단계의 진입점.
 *
 * 동작 단계:
 *   1) admin 큐 단일-스레드 가정(또는 호출자 측 락) 하에 nvme_allocate_request_user_copy()
 *      로 nvme_request 풀에서 한 슬롯을 잡는다.
 *   2) opcode 0xE2 (OCSSD GEOMETRY), nsid 를 채운 spdk_nvme_cmd 를 생성하고
 *      PRP1/PRP2 또는 SGL 로 payload 를 가리키게 한다 (디바이스 DMA 쓰기 대상).
 *   3) admin SQ tail doorbell MMIO write 로 디바이스에 통지 (1 cycle, no MSI-X).
 *   4) 디바이스가 NAND 기하 데이터를 payload 로 DMA 한 후 CQE 를 반환.
 *   5) spdk_nvme_ctrlr_process_admin_completions() 폴링이 CQE 를 회수하면서 cb_fn 호출.
 *
 * 실행 컨텍스트: 통상 컨트롤러를 소유한 단일 SPDK thread (initialization phase 또는
 *   전용 admin polling thread). 멀티스레드에서 동시 호출 시 admin 큐 보호가 호출자 책임.
 * 에러 경로:
 *   - 동기적 실패: 인자 검증 실패(-EINVAL), 풀 고갈(-ENOMEM) 시 cb_fn 호출 없이 반환.
 *   - 비동기적 실패: 디바이스가 cpl.status.sc != 0 으로 보고하면 cb_fn 에 그대로 전달,
 *     호스트 측에서 spdk_nvme_cpl_is_error() 로 분기.
 * 동시성: admin qpair 단일 소유 가정. 고도 동시성 환경에서는 호출자가 mutex 등을 사용.
 *
 * 호출 체인:
 *   사용자 / lib/ftl probe 단계 → spdk_nvme_ocssd_ctrlr_cmd_geometry()
 *     → (lib/nvme/nvme_ctrlr_ocssd_cmd.c) nvme_ctrlr_cmd_geometry()
 *       → nvme_ctrlr_submit_admin_request()
 *         → nvme_qpair_submit_request() → MMIO admin SQ doorbell write
 */
int spdk_nvme_ocssd_ctrlr_cmd_geometry(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				       void *payload, uint32_t payload_size,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg);
/* [한국어] 함수 prototype 라인.
 * - 반환 int : POSIX errno 양식의 음수(-ENOMEM/-EINVAL) 또는 0(성공) 반환.
 * - struct spdk_nvme_ctrlr * : 컨트롤러 핸들 (불투명).
 * - uint32_t nsid : NVMe NSID 필드는 32-bit dword 1 에 위치 (스펙 Figure 12).
 * - void *payload : DMA 가능 가상주소; 디바이스 DMA 마스터링을 위해 IOVA 변환은
 *                   spdk_vtophys() 로 lib/nvme 내부에서 수행.
 * - spdk_nvme_cmd_cb cb_fn : <spdk/nvme.h> 의 typedef 콜백 포인터.
 * - void *cb_arg : 사용자 컨텍스트 — SPDK 는 내부에서 해석하지 않고 그대로 전달. */

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
 * spdk_nvme_ocssd_ns_cmd_vector_reset - vector reset(=erase) I/O 비동기 발행 (OCSSD opcode 0x90)
 *
 * @ns:         대상 네임스페이스 핸들 (spdk_nvme_ctrlr_get_ns() 로 획득).
 *              불투명 포인터 (구조체 본체는 lib/nvme/nvme_internal.h 의 struct spdk_nvme_ns).
 * @qpair:      이 호출 스레드가 소유한 I/O qpair (NVMe Submission+Completion Queue 한 쌍).
 *              SPDK lockless 규약: qpair 는 단일 스레드(=단일 reactor/코어)에서만 다뤄야 한다.
 *              cross-thread 호출 시 spdk_thread_send_msg 등으로 owner thread 로 위임 필수.
 * @lba_list:   각 청크의 시작 LBA(=erase 단위 시작점) 배열.
 *              요구사항: spdk_dma_malloc 등으로 할당된 DMA 가능(physically pinned) 메모리.
 *                       디바이스가 PRP/SGL 로 직접 읽어 가져간다 (SQE 의 dword10/11 vector
 *                       LBA pointer 또는 dword14/15 의 logical block address — 스펙 §4.2).
 *              인코딩: OCSSD 는 PPA(Physical Page Address) 비트 인코딩으로 LBA 안에
 *                       채널/LUN(=PU)/플레인/블록(=청크)/페이지/섹터 위치를 담는다.
 *                       각 LBA 는 청크 시작 정렬이어야 함 (vector reset 의 의미상 제약).
 * @num_lbas:   lba_list 길이 (= 한 번에 erase 할 청크 수). NVMe SQE 1 개에 인코딩 가능한
 *              상한은 디바이스 GEOMETRY 의 max-LBAs-per-command (mccap/maxoc 관련) 에 의함.
 * @chunk_info: 디바이스가 erase 결과로 각 청크 상태(SPDK_OCSSD_CHUNK_STATE_FREE/CLOSED/
 *              OPEN/OFFLINE 등)와 잔여 wear-cycles, 라이트 포인터를 기록할 출력 버퍼 배열.
 *              엔트리 크기 64B (스펙 §6.2.1). NULL 허용 여부는 디바이스 의존
 *              (일부 펌웨어는 NULL 허용, 일부는 거부).
 * @cb_fn:      완료 콜백 — qpair owner 스레드의 spdk_nvme_qpair_process_completions()
 *              폴링 컨텍스트에서 동기 호출됨 (즉, cb_fn 안에서 다시 같은 qpair 에 발행 가능).
 * @cb_arg:     사용자 컨텍스트 (예: 자체 I/O 트래커, 콜백 chain root).
 * @return:     0     = I/O qpair SQ 에 SQE 큐잉 성공 (실제 NAND 소거 완료는 비동기).
 *              -ENOMEM= nvme_request 풀 고갈 (qpair 깊이 초과한 in-flight).
 *              그 외 음수 errno 가능 (인자 검증 실패).
 *
 * 의미: NAND 블록(=청크) 소거. NVMe NVM 커맨드셋의 Deallocate/Write Zeroes 와는 다른
 *   raw erase 로, 청크가 free 상태가 되어 다시 sequential 하게 쓸 수 있게 된다 (호스트
 *   사이드 FTL 의 garbage collection / wear leveling 의 핵심 동작).
 *   여러 청크를 한 SQE 에 모으는 vector 형식이라, 채널/LUN parallelism 을 활용해
 *   동시 erase 가 가능 (디바이스 내부 NAND command queueing).
 *
 * 실행 컨텍스트: I/O qpair 소유 SPDK 스레드. 동일 qpair 에 다른 스레드가 동시에 큐잉하면
 *   정의되지 않은 동작 (race on SQ tail). 완료는 polling 으로만 수행되며 인터럽트 미사용.
 * 에러 경로:
 *   - 동기 실패 시 cb_fn 미호출, 음수 errno 반환 — 호출자가 자체 fallback 수행.
 *   - NAND 소거 실패는 cpl.status.sc 로 보고 (예: SPDK_OCSSD_SC_OFFLINE_CHUNK 0xC0).
 *     cb_fn 안에서 spdk_nvme_cpl_is_error(cpl) 검사 필요.
 * 동시성: qpair 단일 스레드 소유 가정 — 락 없음(lockless).
 *
 * 호출 체인:
 *   사용자 FTL GC 워커 → spdk_nvme_ocssd_ns_cmd_vector_reset()
 *     → (lib/nvme/nvme_ns_ocssd_cmd.c) nvme_ns_ocssd_cmd_vector_reset_*()
 *       → _nvme_ns_cmd_rw 계열 → nvme_qpair_submit_request() → MMIO SQ doorbell write
 *         → 디바이스 NAND erase → CQE → spdk_nvme_qpair_process_completions() → cb_fn
 */
int spdk_nvme_ocssd_ns_cmd_vector_reset(struct spdk_nvme_ns *ns,
					struct spdk_nvme_qpair *qpair,
					uint64_t *lba_list, uint32_t num_lbas,
					struct spdk_ocssd_chunk_information_entry *chunk_info,
					spdk_nvme_cmd_cb cb_fn, void *cb_arg);
/* [한국어] 함수 prototype 라인.
 * - struct spdk_nvme_ns * / spdk_nvme_qpair * : 불투명 핸들.
 * - uint64_t *lba_list : LBA 가 64-bit 인 이유는 OCSSD 의 PPA 인코딩(채널/LUN/플레인/청크/
 *                       페이지/섹터를 비트필드로 인코딩) 이 32-bit 를 초과할 수 있기 때문.
 * - uint32_t num_lbas : NVMe NLB 필드(dword 12 의 16-bit) 와 별개로 SPDK 가 32-bit 로 받지만,
 *                       실제 디바이스 한계는 GEOMETRY 의 max LBAs per command 가 정함.
 * - struct spdk_ocssd_chunk_information_entry * : <spdk/nvme_ocssd_spec.h> 정의 — 64B 엔트리. */

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
 * spdk_nvme_ocssd_ns_cmd_vector_write - vector write 비동기 발행 (메타데이터 없음, opcode 0x91)
 *
 * @ns:         대상 네임스페이스 핸들 (불투명).
 * @qpair:      호출 스레드 소유 I/O qpair. 단일 스레드 lockless 규약.
 * @buffer:     데이터 페이로드 가상주소. spdk_dma_malloc()/spdk_zmalloc() 등으로 할당된
 *              hugepage 매핑 버퍼여야 한다 (DMA 가능 = 물리 주소 안정 + 페이지 핀).
 *              디바이스가 PRP1/PRP2 또는 SGL 로 DMA 읽기를 수행. 총 유효 크기는
 *              num_lbas * spdk_nvme_ns_get_sector_size(ns) 이상이어야 한다 (부족하면
 *              디바이스가 호스트 메모리 밖을 읽는 보안/안정성 사고 — SPDK 가 IOMMU 로
 *              차단하지만 호출자 책임).
 * @lba_list:   각 섹터(=write unit) 의 목적지 LBA. 청크/플레인/LUN/채널 비트 인코딩이
 *              OCSSD 1.2 §4.4 의 PPA addressing 에 따라 들어간다. 한 vector 명령 내
 *              LBA 들은 동일 청크의 sequential write pointer 를 단조 증가로 따라가야 함
 *              (OCSSD 의 sequential write 제약 — 청크 내 random write 불가).
 *              위반 시 디바이스가 SPDK_OCSSD_SC_OUT_OF_ORDER_WRITE (0xF2) 등을 반환.
 * @num_lbas:   배열 길이. GEOMETRY 의 ws_min(minimum write size, 보통 4 또는 8 섹터)
 *              단위로 정렬되어야 디바이스가 수락 — 미정렬은 transient write fail 유발.
 * @cb_fn:      완료 콜백. qpair_process_completions 폴링 컨텍스트에서 호출.
 * @cb_arg:     사용자 컨텍스트.
 * @io_flags:   SPDK_OCSSD_IO_FLAGS_* 비트마스크 (<spdk/nvme_ocssd_spec.h>):
 *                - LIMITED_RETRY      : 재시도 횟수 제한
 *                - SUSPEND_ON_INFO    : 디바이스가 정보 이벤트로 일시정지 가능
 *                - SCRAMBLER_DISABLE  : NAND 데이터 스크램블러 비활성화 (테스트용)
 *                - DIRECT_ACCESS      : 호스트 매핑 우회 (특정 펌웨어 진단용)
 *              SQE control field(dword 12 의 상위 비트) 에 OR 되어 디바이스에 전달.
 * @return:     0      = 성공 큐잉.
 *              -ENOMEM= nvme_request 풀 고갈 (qpair 깊이 초과 in-flight).
 *
 * 의미: 단일 SQE 로 다중 LBA 에 분산 기록 (NAND plane parallelism 활용). 호스트 사이드
 *   FTL 이 한 번의 호출로 여러 plane/LUN 에 stripe 형식으로 데이터를 쓰며 처리량을 극대화.
 *   메타데이터(보호 정보, 사용자 메타) 동반 시는 _with_md 변형을 사용.
 *
 * 실행 컨텍스트: qpair 소유 SPDK 스레드 단독 (lockless). 완료는 polling 모드.
 * 에러 경로:
 *   - 동기 실패: -ENOMEM 등 음수 errno, cb_fn 미호출.
 *   - 비동기 실패: cpl.status 의 sct/sc 코드(예: out-of-order, write-fail) 를 cb_fn 으로 통보.
 *     호스트 FTL 은 실패 LBA 를 무효화하고 다른 청크로 재시도.
 * 동시성: 동일 qpair 에 멀티 스레드 동시 호출 금지.
 *
 * 호출 체인:
 *   사용자 FTL writer → spdk_nvme_ocssd_ns_cmd_vector_write()
 *     → (lib/nvme/nvme_ns_ocssd_cmd.c) nvme_ns_ocssd_cmd_vector_write()
 *       → nvme_qpair_submit_request() → MMIO I/O SQ doorbell write
 *         → 디바이스 NAND program → CQE → cb_fn
 */
int spdk_nvme_ocssd_ns_cmd_vector_write(struct spdk_nvme_ns *ns,
					struct spdk_nvme_qpair *qpair,
					void *buffer,
					uint64_t *lba_list, uint32_t num_lbas,
					spdk_nvme_cmd_cb cb_fn, void *cb_arg,
					uint32_t io_flags);
/* [한국어] 함수 prototype.
 * - void *buffer : 가상주소. 내부에서 spdk_vtophys() 로 IOVA 변환 후 PRP/SGL 작성.
 * - uint64_t *lba_list : LBA 배열. 첫 LBA 는 SQE dword14/15 (SLBA) 에, 나머지는
 *                       lba_list 자체가 PRP/SGL 로 디바이스에 전달되는 vector 인코딩.
 * - uint32_t io_flags : OCSSD 전용 control 비트 (스펙 §4.5). dword 12 상위에 OR. */

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
 * spdk_nvme_ocssd_ns_cmd_vector_write_with_md - 메타데이터를 동반한 vector write (opcode 0x91 + MPTR)
 *
 * @ns:         대상 네임스페이스 (불투명).
 * @qpair:      I/O qpair, 스레드 단독 소유.
 * @buffer:     데이터 페이로드 (DMA 가능 메모리). PRP1/PRP2 또는 SGL 로 디바이스 DMA.
 *              크기 ≥ num_lbas * spdk_nvme_ns_get_sector_size(ns).
 * @metadata:   섹터별 메타데이터 페이로드. 길이 = num_lbas * spdk_nvme_ns_get_md_size(ns).
 *              조건: NVMe Identify Namespace 의 MS(metadata size) 필드가 0 이 아니어야
 *                    의미가 있다. MS=0 디바이스에 메타 동반 명령은 거부됨.
 *              포맷: Extended LBA(=메타가 데이터 LBA 뒤에 인터리빙) 가 아닌 separate
 *                    buffer 형식. SPDK 는 SQE 의 MPTR (Metadata Pointer, dword 6/7) 로
 *                    metadata 의 IOVA 를 적어 넣어 디바이스에 전달.
 *              용도: T10 DIF (8B GuardTag+RefTag+ApplicationTag) PI 검증 또는 사용자
 *                    정의 메타(논리 LBA, 시퀀스 번호 등) 저장.
 * @lba_list:   목적지 LBA 배열. vector_write 와 동일 sequential 제약.
 * @num_lbas:   LBA 개수.
 * @cb_fn/@cb_arg/@io_flags: vector_write 와 동일 의미.
 * @return:     0       = 큐잉 성공.
 *              -ENOMEM = nvme_request 풀 고갈.
 *
 * 의미: 보호 정보(PI: Protection Information, T10 DIF) 또는 사용자 정의 메타데이터를
 *   동반한 분산 기록. 호스트 FTL 이 LBA-to-PPA 매핑 외에 NAND 의 spare area 에 별도
 *   메타(원래 논리 LBA, write epoch, GC generation 등) 를 함께 저장하고 싶을 때 사용.
 *   복구 시 spare area 의 메타가 매핑 테이블 재구성의 정답지가 된다.
 *
 * 실행 컨텍스트: qpair 소유 단일 SPDK 스레드.
 * 에러 경로: 동기 -ENOMEM, 비동기 cpl.status 통보 (vector_write 와 동일).
 * 동시성: 동일 qpair 멀티 스레드 동시 호출 금지.
 *
 * 호출 체인:
 *   사용자 / lib/ftl writer → spdk_nvme_ocssd_ns_cmd_vector_write_with_md()
 *     → (lib/nvme/nvme_ns_ocssd_cmd.c) nvme_ns_ocssd_cmd_vector_write_with_md()
 *       → submit_request → MMIO doorbell write
 */
int spdk_nvme_ocssd_ns_cmd_vector_write_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lba_list, uint32_t num_lbas,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags);
/* [한국어] 함수 prototype.
 * - void *metadata : 별도 metadata buffer (separate-buffer 모드 전용).
 *                    Extended-LBA 모드(데이터 끝에 메타 인터리브) 는 본 API 가 아닌
 *                    sector_size 자체에 메타 크기가 포함되어 있는 일반 vector_write 사용.
 * - 그 외 인자는 vector_write 와 동일. */

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
 * spdk_nvme_ocssd_ns_cmd_vector_read - vector read 비동기 발행 (메타데이터 없음, opcode 0x92)
 *
 * @ns:         대상 네임스페이스 (불투명).
 * @qpair:      스레드 소유 I/O qpair (lockless 단일 스레드).
 * @buffer:     데이터를 받을 DMA 가능 버퍼 (디바이스가 PRP/SGL 로 DMA 쓰기 수행).
 *              크기 ≥ num_lbas * spdk_nvme_ns_get_sector_size(ns). hugepage 매핑 권장.
 * @lba_list:   판독 대상 LBA 배열 (DMA 가능 메모리).
 *              write 와 달리 read 는 임의 순서/임의 청크 위치 가능 (sequential 제약 없음).
 *              단, ECC unrecovered LBA 면 디바이스가 CQE 의 status field 에
 *              SPDK_OCSSD_SC_READ_HIGH_ECC (0xD0) 또는 read-fail 을 보고.
 *              호스트 FTL 은 그 LBA 의 데이터를 잃은 것으로 간주하고 사본/이중화 복구.
 * @num_lbas:   배열 길이. 한 SQE 의 최대 LBAs 는 GEOMETRY mccap/maxoc 등에 따라 결정.
 * @cb_fn/@cb_arg/@io_flags: vector_write 와 표준 의미. io_flags 는 SPDK_OCSSD_IO_FLAGS_*
 *                          비트마스크.
 * @return:     0       = 성공 큐잉.
 *              -ENOMEM = nvme_request 풀 고갈.
 *
 * 의미: 단일 SQE 로 NAND 의 분산된 LBA 들을 한 번에 조회. 호스트 FTL 의 garbage
 *   collection 시 valid pages 를 모아 읽어 새 청크에 다시 쓰는 시퀀스의 read leg 에 사용.
 *   write 와 달리 임의 순서가 가능하므로, 매핑 테이블이 가리키는 산재된 valid LBA 들을
 *   한 번에 모아 읽어 PCIe 왕복 횟수를 줄인다.
 *
 * 실행 컨텍스트: qpair 소유 단일 SPDK 스레드.
 * 에러 경로: 동기 -ENOMEM / 비동기 cpl.status 의 ECC 등 read-failure 코드.
 *   호출자(FTL) 는 cb 안에서 에러 LBA 만 추출해 redundancy 경로로 폴백.
 * 동시성: 동일 qpair 멀티 스레드 동시 호출 금지.
 *
 * 호출 체인:
 *   사용자 FTL reader / GC → spdk_nvme_ocssd_ns_cmd_vector_read()
 *     → (lib/nvme/nvme_ns_ocssd_cmd.c) nvme_ns_ocssd_cmd_vector_read()
 *       → submit_request → MMIO doorbell
 */
int spdk_nvme_ocssd_ns_cmd_vector_read(struct spdk_nvme_ns *ns,
				       struct spdk_nvme_qpair *qpair,
				       void *buffer,
				       uint64_t *lba_list, uint32_t num_lbas,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				       uint32_t io_flags);
/* [한국어] 함수 prototype.
 * - void *buffer : 디바이스가 DMA 쓰기를 수행하는 수신 버퍼 (write 와 데이터 방향 반대).
 * - 다른 인자 의미는 vector_write 와 동일. */

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
 * spdk_nvme_ocssd_ns_cmd_vector_read_with_md - 메타데이터를 회수하는 vector read (opcode 0x92 + MPTR)
 *
 * @ns:         대상 네임스페이스 (불투명).
 * @qpair:      스레드 소유 I/O qpair.
 * @buffer:     데이터 수신 DMA 버퍼 (디바이스가 PRP/SGL 로 DMA 쓰기).
 *              크기 ≥ num_lbas * spdk_nvme_ns_get_sector_size(ns).
 * @metadata:   섹터별 메타데이터 수신 버퍼. 길이 = num_lbas * spdk_nvme_ns_get_md_size(ns).
 *              디바이스가 NAND spare area 에 저장된 메타를 SQE 의 MPTR(dword 6/7) 로 DMA 쓰기.
 *              호스트는 회수된 메타와 vs.동기적으로 데이터 정합성을 교차 검증 가능.
 * @lba_list/@num_lbas/@cb_fn/@cb_arg/@io_flags: vector_read 와 표준 의미.
 * @return:     0       = 큐잉.
 *              -ENOMEM = 풀 고갈.
 *
 * 의미: write_with_md 로 NAND spare area 에 저장한 메타와 짝이 되는 read 경로.
 *   - T10 DIF 보호 정보 검증을 호스트가 수행 (CRC/RefTag 일치 확인).
 *   - 호스트 FTL 의 사용자 정의 메타(원래 논리 LBA, write epoch) 를 함께 회수해
 *     매핑 테이블 reverse-lookup / 복구에 사용.
 *
 * 실행 컨텍스트: qpair 소유 단일 SPDK 스레드.
 * 에러 경로: 동기 -ENOMEM / 비동기 cpl.status 의 ECC/PI fail.
 *   PI 검증 실패 시 디바이스가 SPDK_NVME_SC_GUARD_CHECK_ERROR (0x82) 등 보고.
 * 동시성: 동일 qpair 멀티 스레드 금지.
 *
 * 호출 체인:
 *   사용자 FTL reader → spdk_nvme_ocssd_ns_cmd_vector_read_with_md()
 *     → (lib/nvme/nvme_ns_ocssd_cmd.c) nvme_ns_ocssd_cmd_vector_read_with_md()
 *       → submit_request → MMIO doorbell
 */
int spdk_nvme_ocssd_ns_cmd_vector_read_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lba_list, uint32_t num_lbas,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags);
/* [한국어] 함수 prototype.
 * - void *metadata : 메타 수신 버퍼. 크기는 NS 의 MS(metadata size) 와 num_lbas 곱.
 *                    SQE 의 MPTR(IOVA) 로 디바이스에 전달.
 * - 그 외 인자 의미는 vector_read 와 동일. */

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
 * spdk_nvme_ocssd_ns_cmd_vector_copy - 디바이스 내부 NAND-to-NAND 복사 발행 (opcode 0x95)
 *
 * @ns:           대상 네임스페이스 (불투명).
 * @qpair:        I/O qpair, 단일 스레드 소유.
 * @dst_lba_list: 목적지 LBA 배열 (DMA 가능 메모리 — 디바이스가 SQE 에서 PRP 로 fetch).
 *                vector_write 와 동일하게 destination 청크의 sequential write pointer
 *                를 따라야 한다 (단조 증가, ws_min 정렬).
 * @src_lba_list: 출발지 LBA 배열. 임의 분산 가능 (read 와 동일 자유도).
 *                길이 = dst_lba_list 와 동일해야 함 — 1:1 매핑.
 * @num_lbas:     양측 배열 공통 길이. 한 SQE 의 최대 LBAs 는 GEOMETRY 가 광고.
 * @cb_fn/@cb_arg/@io_flags: 표준 의미. io_flags 는 SPDK_OCSSD_IO_FLAGS_* 비트마스크.
 * @return:       0       = 큐잉 성공.
 *                -ENOMEM = 풀 고갈.
 *
 * 의미: 호스트 메모리를 경유하지 않고(=PCIe 왕복 비용 없이) 디바이스가 직접 NAND 내부에서
 *   src 의 데이터를 dst 로 복제. 호스트 사이드 FTL 의 garbage collection 에서 valid page
 *   이주(migration) 를 가속하는 핵심 명령. SQE 1 개로 다중 LBA 동시 복사가 가능하므로,
 *   기존의 read→host buffer→write 두 단계(왕복 PCIe + 호스트 RAM 점유) 를 한 단계로
 *   대체해 GC 처리량을 크게 끌어올린다.
 *   디바이스가 지원하지 않을 수 있으므로 (GEOMETRY mccap = Multiple Command Capability
 *   필드의 vector copy 비트로 광고) 사용 전 capability 확인이 권장된다.
 *
 * 실행 컨텍스트: qpair 소유 단일 SPDK 스레드.
 * 에러 경로:
 *   - 동기 실패: -ENOMEM, cb_fn 미호출.
 *   - 비동기 실패: 디바이스 미지원 시 Invalid Opcode (cpl.status.sc=0x01),
 *                  src ECC fail 시 read-fail 코드, dst sequential 위반 시 out-of-order 코드.
 *     호스트 FTL 은 cb 안에서 spdk_nvme_cpl_is_error 로 분기.
 * 동시성: 동일 qpair 멀티 스레드 동시 호출 금지.
 *
 * 호출 체인:
 *   사용자 FTL GC → spdk_nvme_ocssd_ns_cmd_vector_copy()
 *     → (lib/nvme/nvme_ns_ocssd_cmd.c) nvme_ns_ocssd_cmd_vector_copy()
 *       → submit_request → MMIO doorbell
 */
int spdk_nvme_ocssd_ns_cmd_vector_copy(struct spdk_nvme_ns *ns,
				       struct spdk_nvme_qpair *qpair,
				       uint64_t *dst_lba_list, uint64_t *src_lba_list,
				       uint32_t num_lbas,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				       uint32_t io_flags);
/* [한국어] 함수 prototype.
 * - dst_lba_list / src_lba_list : 두 LBA 배열을 모두 DMA 가능 메모리에 둬야 한다 —
 *                                 디바이스가 SQE 의 PRP 두 슬롯으로 양측을 함께 fetch.
 * - 데이터 자체는 호스트 메모리를 통과하지 않고 NAND 내부에서 이동 (peer-DMA-like).
 * - num_lbas 는 두 배열의 공통 원소 수. */

#ifdef __cplusplus
}
#endif
/* [한국어] 위쪽 extern "C" { 와 짝이 되는 닫는 중괄호.
 * 분기: __cplusplus 정의된 C++ 컴파일에서만 } 가 토큰화되어 extern "C" 블록을 닫는다.
 * 누락 시 본 헤더 끝에서 블록이 열린 채로 다음 코드가 이어져 syntax error.
 * 위치 의의: include guard #endif 보다 안쪽에 두어야 하므로 SPDK 컨벤션상 여기 배치.
 * 동기화: 전처리 토큰이라 런타임 동시성과 무관. */

#endif
/* [한국어] include guard 종료 (#ifndef SPDK_NVME_OCSSD_H 와 짝).
 * 역할: 본 헤더가 정의한 매크로/선언이 한 번역 단위에 정확히 한 번만 들어가도록 보장.
 * 두 번째 #include 부터는 SPDK_NVME_OCSSD_H 가 이미 정의되어 있어 전체 본문이 skip 되며,
 * 이 #endif 로 그 conditionally-compiled 영역의 끝을 닫는다.
 * 위치 의의: 파일 가장 마지막 라인 — 모든 declaration 을 가드 안에 포함시키기 위함.
 * 동기화: 전처리 단계의 매크로라 런타임 동시성과 무관. */
