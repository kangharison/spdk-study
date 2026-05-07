/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2020, Western Digital Corporation. All rights reserved.
 */

/**
 * \file
 * NVMe driver public API extension for Zoned Namespace Command Set
 */

/*
 * [한국어 설명] NVMe ZNS(Zoned Namespace) Command Set 공개 API 헤더 (nvme_zns.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVMe TP 4053(이후 NVMe 2.0 ZNS Command Set Specification)을 사용자 코드가
 * 호출할 수 있도록 노출하는 SPDK 공개 API 헤더이다. ZNS는 SSD의 LBA 공간을 일정 크기의
 * "zone"(append-only 영역)으로 분할하고, 각 zone에 대해 EMPTY → IMPLICITLY_OPENED /
 * EXPLICITLY_OPENED → CLOSED → FULL → READ_ONLY → OFFLINE의 zone state machine을 강제하는
 * NVMe command set이다. 이 헤더는 (1) ZNS namespace/controller의 ZNS 전용 식별자(Identify) 데이터
 * 접근, (2) zone 단위 속성 조회(zone_size, MOR/MAR, ZASL 등), (3) Zone Append(0x7D),
 * (4) Zone Management Send(0x79: Open/Close/Finish/Reset/Offline/Set Descriptor Extension),
 * (5) Zone Management Receive(0x7A: Report/Extended Report Zones)의 5가지 카테고리를 모두
 * 선언한다. 사용자는 이 헤더만 include하면 ZNS SSD에 대한 모든 표준 ZNS 명령을 비동기적으로 발행할 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe driver의 사용자 진입점(public API) 계층에 해당한다. 호출 체인은
 * "사용자 코드(애플리케이션 / module/bdev/nvme의 ZNS bdev) →
 *  spdk_nvme_zns_*() (이 헤더) →
 *  lib/nvme/nvme_zns.c (SQE 빌드, 옵코드/CDW 채움) →
 *  lib/nvme/nvme_ns_cmd.c·nvme_qpair.c (request 객체화, qpair에 enqueue) →
 *  PCIe transport(lib/nvme/nvme_pcie*.c) 또는 Fabrics transport(lib/nvme/nvme_tcp.c, nvme_rdma.c) →
 *  NVMe SSD ZNS 펌웨어"이다. 실행 컨텍스트는 호스트 유저스페이스(SPDK reactor 스레드)이며,
 *  qpair 한 개당 단일 SPDK 스레드만 진입해야 한다는 SPDK 공통 규칙이 그대로 적용된다.
 *  완료 통지는 spdk_nvme_qpair_process_completions() 폴링이 CQE를 수확할 때 cb_fn 호출 형태로 이루어진다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h(표준 stdint/stdbool 등), spdk/nvme.h(spdk_nvme_ns/ctrlr/qpair/cmd_cb/sgl 콜백 타입,
 *  IO_FLAGS, spdk_nvme_zns_zra_report_opts enum 등). spdk/nvme_zns_spec 관련 타입은 spdk/nvme.h를 통해 간접
 *  포함되어 spdk_nvme_zns_ns_data / spdk_nvme_zns_ctrlr_data 구조체를 노출한다.
 * 의존되는 쪽: module/bdev/nvme의 ZNS bdev 백엔드(zone read/write/append/mgmt를 bdev I/O로 매핑),
 *  사용자 응용/예제(test/nvme/cuse, examples/nvme/identify 등)에서 직접 호출. 데이터 흐름은 호스트 메모리의
 *  payload 버퍼(또는 SGL 콜백 페어)가 PRP/SGL로 변환되어 SSD DMA 채널로 흘러가고, 결과(append된 LBA, zone descriptor
 *  배열 등)가 동일 버퍼 또는 CQE의 DW0/1을 통해 사용자에게 회신된다.
 * 공유 자료구조: spdk_nvme_ns(namespace 핸들 — ZNS 전용 식별자 캐시 포함), spdk_nvme_ctrlr(controller 핸들 —
 *  ZNS Identify Controller(CDW10 CNS=0x06) 캐시), spdk_nvme_qpair(I/O queue pair — SQ/CQ 한 쌍).
 *
 * === 주요 함수/구조체 요약 ===
 *  - spdk_nvme_zns_ns_get_data / ctrlr_get_data: ZNS Command Set Specific Identify 데이터 캐시 포인터 반환.
 *      ZNS가 아닌 namespace에서는 NULL을 돌려 ZNS 지원 여부 검사로도 쓰인다.
 *  - get_zone_size_sectors / get_zone_size / get_num_zones / get_max_open_zones / get_max_active_zones /
 *      get_max_zone_append_size: zone 기하/리소스 제약을 조회. zone_size는 LBAFE의 zsze, MOR/MAR/ZASL은 0-based
 *      인코딩(실제 한도 = 필드값 + 1)이라는 NVMe ZNS 규약을 캡슐화한다.
 *  - spdk_nvme_zns_zone_append / append_with_md / appendv / appendv_with_md: opcode 0x7D Zone Append.
 *      zslba에만 정렬된 LBA를 주고 컨트롤러가 실제 append 위치(ALBA)를 CQE DW0/1로 회신한다. write와 달리
 *      호스트가 write pointer를 추적할 필요가 없어 다중 producer가 같은 zone에 동시 발행 가능하다.
 *  - spdk_nvme_zns_open_zone / close_zone / finish_zone / reset_zone / offline_zone / set_zone_desc_ext:
 *      opcode 0x79 Zone Management Send. 각각 zone state machine의 EMPTY→EO, IO/EO→CSD, *→FULL, *→EMPTY,
 *      RO→OFFLINE 전이와 CSD zone에 descriptor extension 부착을 담당한다.
 *  - spdk_nvme_zns_report_zones / ext_report_zones: opcode 0x7A Zone Management Receive (ZRA=0x00 / 0x01).
 *      slba 이후의 zone descriptor 배열(또는 descriptor + extension)을 호스트 버퍼로 DMA. report_opts(ZRASF)로
 *      특정 상태(ZSE/ZSIO/ZSEO/ZSC/ZSF/ZSRO/ZSO) 필터, partial_report 플래그로 nr_zones 의미를 선택한다.
 */

#ifndef SPDK_NVME_ZNS_H
/* [한국어] SPDK_NVME_ZNS_H — 헤더 가드. 같은 컴파일 단위에서 nvme_zns.h가 두 번 이상 #include 되어도
 * 함수 선언이 중복되어 컴파일 에러를 일으키지 않도록 보호한다. (typedef/struct 선언 자체는 spdk/nvme.h 쪽에 있다.)
 * 두 번째 include 시 이 #ifndef 검사가 실패해 파일 끝의 #endif까지 전처리기 단계에서 통째로 스킵된다. */
#define SPDK_NVME_ZNS_H
/* [한국어] 헤더 가드 매크로 정의. 이 줄을 거치는 순간 SPDK_NVME_ZNS_H가 빈 토큰으로 정의되어, 같은 translation unit
 * 안에서 다음번 nvme_zns.h include는 위의 #ifndef에서 곧장 #endif로 점프한다(가드 동작의 핵심). */

#include "spdk/stdinc.h"
/* [한국어] spdk/stdinc.h — SPDK 전체에서 사용되는 표준 헤더 모음(stddef/stdint/stdbool/string 등)을 한 번에 가져온다.
 * uint32_t/uint64_t/uint16_t/bool/size_t 같은 이 헤더의 함수 시그니처에 등장하는 기본 타입이 여기서 공급된다. */

#ifdef __cplusplus
/* [한국어] __cplusplus — C++ 컴파일러가 정의하는 매크로. SPDK는 C로 작성되었지만 C++ 사용자(예: SPDK를 사용하는
 * C++ 애플리케이션)도 본 헤더를 include 할 수 있도록 extern "C"로 감싸 함수 이름이 C 링키지(맨글링 없음)로
 * 묶이도록 한다. */
extern "C" {
#endif

#include "spdk/nvme.h"
/* [한국어] spdk/nvme.h — SPDK NVMe driver 공개 API의 본체. 본 ZNS 확장 헤더에서 사용하는 핵심 식별자
 * (struct spdk_nvme_ns, struct spdk_nvme_ctrlr, struct spdk_nvme_qpair, spdk_nvme_cmd_cb 함수 포인터,
 *  SPDK_NVME_IO_FLAGS_*, spdk_nvme_req_reset_sgl_cb / spdk_nvme_req_next_sge_cb,
 *  enum spdk_nvme_zns_zra_report_opts, struct spdk_nvme_zns_ns_data / ctrlr_data 등)이 모두 여기서 정의되거나
 * 추가 nvme_zns_spec 헤더를 통해 노출된다. 이 include가 없으면 본 헤더의 모든 시그니처가 미해결 타입으로 떨어진다. */

/**
 * Get the Zoned Namespace Command Set Specific Identify Namespace data
 * as defined by the NVMe Zoned Namespace Command Set Specification.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace.
 *
 * \return a pointer to the namespace data, or NULL if the namespace is not
 * a Zoned Namespace.
 */
/*
 * [한국어]
 * spdk_nvme_zns_ns_get_data - ZNS Command Set Specific Identify Namespace 데이터(IDNS, CSI=0x02) 캐시 반환.
 *
 * @ns: 대상 namespace 핸들. spdk_nvme_ctrlr_get_ns()로 얻은 포인터.
 * @return: const struct spdk_nvme_zns_ns_data * — 컨트롤러 attach 시점에 1회 캐시된 ZNS 식별자 포인터.
 *          이 namespace가 ZNS Command Set이 아니면 NULL. 호출자는 이 NULL 검사를 "ZNS 여부 판별"로 활용한다.
 *
 * 동기/배경: NVMe Identify는 admin command(opcode 0x06)로 비싸기 때문에, SPDK는 attach 시
 *   "Identify Namespace (CNS=0x05, CSI=0x02)"를 한 번 발행해 namespace당 캐시한다. 본 함수는
 *   그 캐시 포인터를 그대로 돌려준다.
 * 동작: namespace 객체 내부에 저장된 nsdata_zns 멤버 주소를 반환(불변 데이터). admin SQ에 새 명령을
 *   submit하지 않으므로 thread-safe하며 어떤 SPDK 스레드에서 호출해도 안전하다.
 * 실행 컨텍스트: 호스트 유저스페이스. read-only 캐시 접근만 하므로 락 불필요(lockless).
 * 호출자: 사용자가 zone_size_sectors/lbafe/MOR/MAR을 직접 들여다보고 싶을 때, 또는 module/bdev/nvme의
 *   ZNS bdev가 zone geometry를 부트업 시 캐시할 때.
 * 호출 대상: 단순 멤버 dereference — admin/IO command를 발행하지 않는다.
 * 에러 경로: 구조적으로 호출 자체는 실패하지 않고, 비-ZNS namespace이면 NULL 반환.
 *
 * 호출 체인:
 *   사용자/ZNS bdev 초기화 → [spdk_nvme_zns_ns_get_data] → ns->nsdata_zns 캐시 dereference (lib/nvme/nvme_zns.c)
 */
const struct spdk_nvme_zns_ns_data *spdk_nvme_zns_ns_get_data(struct spdk_nvme_ns *ns);

/**
 * Get the zone size, in number of sectors, of the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the zone size of the given namespace in number of sectors.
 */
/*
 * [한국어]
 * spdk_nvme_zns_ns_get_zone_size_sectors - 한 zone이 몇 개의 LBA(섹터)로 구성되는지 반환.
 *
 * @ns: 대상 namespace 핸들. ZNS namespace여야 의미 있는 값.
 * @return: zone당 LBA 개수(uint64_t). 즉 zsze 필드 값으로, "임의 zone의 ZSLBA + zone_size_sectors"가
 *          다음 zone의 ZSLBA가 된다. 비-ZNS namespace일 경우 정의되지 않은 값을 반환할 수 있으므로
 *          ns_get_data()로 ZNS임을 먼저 확인할 것.
 *
 * 동기/배경: ZNS 호스트는 모든 I/O를 zone 경계 안에 맞춰야 한다(append/write가 zone을 가로지르면 ZSF). 사용자
 *   코드가 zone 경계를 계산하려면 zone_size를 LBA 단위로 알아야 한다.
 * 동작: 캐시된 nsdata_zns의 lbafe[fmt].zsze (LBA Format Extension의 zone size, 단위=LBA)를 그대로 반환.
 *   SQE 발행 없음. read-only 캐시 접근.
 * 실행 컨텍스트: 호스트 유저스페이스, lockless. 모든 SPDK 스레드에서 호출 가능.
 * 호출자: 사용자 코드가 zone 인덱스 ↔ ZSLBA 변환을 할 때, ZNS bdev가 bdev->zone_size를 채울 때.
 * 호출 대상: 캐시 멤버 접근만 수행.
 * 에러 경로: 별도 에러 표시 없음. 비-ZNS namespace이면 0이거나 쓰레기 값일 수 있음.
 *
 * 호출 체인:
 *   사용자 → [spdk_nvme_zns_ns_get_zone_size_sectors] → ns->nsdata_zns->lbafe[fmt_idx].zsze (lib/nvme/nvme_zns.c)
 */
uint64_t spdk_nvme_zns_ns_get_zone_size_sectors(struct spdk_nvme_ns *ns);

/**
 * Get the zone size, in bytes, of the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the zone size of the given namespace in bytes.
 */
/*
 * [한국어]
 * spdk_nvme_zns_ns_get_zone_size - 한 zone의 크기를 바이트 단위로 반환.
 *
 * @ns: 대상 namespace. ZNS여야 의미 있음.
 * @return: zone 크기(byte). 내부적으로 zone_size_sectors * sector_size로 계산된다. 64비트라
 *          수십 GB 규모의 zone도 표현 가능.
 *
 * 동기/배경: 사용자 코드가 zone 단위 메모리 버퍼를 할당하거나 capacity를 계산할 때 LBA가 아닌 byte 단위가
 *   더 직관적인 경우를 위한 헬퍼. NVMe ZNS 스펙의 zsze는 LBA 단위로만 노출되어 있어 호스트 측 변환이 필요하다.
 * 동작: zone_size_sectors getter가 돌려준 값에 spdk_nvme_ns_get_sector_size()를 곱해 반환. 모두 캐시 접근.
 * 실행 컨텍스트: 호스트 유저스페이스, lockless. 어디서든 호출 가능.
 * 호출자: 사용자 응용/도구(예: nvme zns identify CLI). ZNS bdev 초기화 시 logical block 크기를 곱해 capacity 계산.
 * 호출 대상: 내부 인라인 산술. SQE 발행 없음.
 * 에러 경로: 비-ZNS namespace이면 sector_size * (정의되지 않은 zsze)가 되어 신뢰 불가 — 호출 전 ns_get_data() 확인.
 *
 * 호출 체인:
 *   사용자 → [spdk_nvme_zns_ns_get_zone_size] → spdk_nvme_zns_ns_get_zone_size_sectors() ×
 *           spdk_nvme_ns_get_sector_size()
 */
uint64_t spdk_nvme_zns_ns_get_zone_size(struct spdk_nvme_ns *ns);

/**
 * Get the number of zones for the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the number of zones.
 */
/*
 * [한국어]
 * spdk_nvme_zns_ns_get_num_zones - namespace에 존재하는 총 zone 개수 반환.
 *
 * @ns: 대상 namespace.
 * @return: zone 개수(uint64_t). 일반적으로 (namespace 총 LBA 수) / zone_size_sectors. ZNS namespace의 NSZE에는
 *          여분 capacity가 zone 정렬되어 있다고 가정한다.
 *
 * 동기/배경: Report Zones를 페이지 단위로 순회하려면 호스트가 끝 조건을 알기 위해 총 zone 수를 알아야 한다. 또한
 *   ZNS bdev는 bdev->num_blocks와 함께 이 값으로 zone array 인덱싱 한도를 결정한다.
 * 동작: ns_data nsze / zsze를 나눠 반환(또는 NVMe Identify ZNS의 별도 필드 사용). 캐시 접근만 수행.
 * 실행 컨텍스트: 호스트 유저스페이스, lockless.
 * 호출자: 사용자, ZNS bdev. Zone Append 인덱스 검사, Report Zones 마지막 zone 도달 검사 등에 사용.
 * 호출 대상: 산술 연산만, SQE 발행 없음.
 * 에러 경로: 비-ZNS namespace이면 정의되지 않음.
 *
 * 호출 체인:
 *   사용자/ZNS bdev → [spdk_nvme_zns_ns_get_num_zones] → 캐시된 nsdata 산술
 */
uint64_t spdk_nvme_zns_ns_get_num_zones(struct spdk_nvme_ns *ns);

/**
 * Get the maximum number of open zones for the given namespace.
 *
 * An open zone is a zone in any of the zone states:
 * EXPLICIT OPEN or IMPLICIT OPEN.
 *
 * If this value is 0, there is no limit.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the maximum number of open zones.
 */
/*
 * [한국어]
 * spdk_nvme_zns_ns_get_max_open_zones - namespace의 동시 OPEN 가능한 zone 수 한도(MOR) 반환.
 *
 * @ns: 대상 namespace.
 * @return: 동시 OPEN 가능한 zone 개수(uint32_t). 0이면 한도 없음(unlimited). NVMe ZNS의 MOR
 *          (Maximum Open Resources) 필드는 0-based로 인코딩되어 있으나, 본 getter는 호스트가 직관적으로
 *          쓸 수 있도록 "실제 한도(=필드값+1)"로 변환된 값을 돌려준다(0 == unlimited 특수 케이스 유지).
 *
 * 동기/배경: ZNS는 동시 OPEN 상태 zone 수를 제한해 디바이스 내부 buffer/cache 자원을 보호한다. 호스트가 새
 *   zone을 OPEN하려 할 때 이 한도를 초과하면 implicit open이 trigger한 가장 오래된 zone이 자동 close된다
 *   (transition: ZSIO → ZSC). 호스트는 이 값을 알아야 워크로드 scheduling을 안전하게 한다.
 * 동작: nsdata_zns의 MOR 필드를 읽어 0-based → 1-based 변환 후 반환. 0이면 그대로 0 반환.
 * 실행 컨텍스트: 호스트 유저스페이스, lockless.
 * 호출자: ZNS-aware 파일시스템/응용(F2FS, ZenFS 등), 사용자 도구, ZNS bdev.
 * 호출 대상: 캐시 멤버 산술. SQE 발행 없음.
 * 에러 경로: 비-ZNS namespace이면 의미 없는 값.
 *
 * 호출 체인:
 *   사용자/ZNS bdev → [spdk_nvme_zns_ns_get_max_open_zones] → ns->nsdata_zns->mor 인코딩 변환
 */
uint32_t spdk_nvme_zns_ns_get_max_open_zones(struct spdk_nvme_ns *ns);

/**
 * Get the maximum number of active zones for the given namespace.
 *
 * An active zone is a zone in any of the zone states:
 * EXPLICIT OPEN, IMPLICIT OPEN or CLOSED.
 *
 * If this value is 0, there is no limit.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the maximum number of active zones.
 */
/*
 * [한국어]
 * spdk_nvme_zns_ns_get_max_active_zones - namespace의 동시 ACTIVE 가능한 zone 수 한도(MAR) 반환.
 *
 * @ns: 대상 namespace.
 * @return: ACTIVE 상태(EXPLICIT_OPEN ∪ IMPLICIT_OPEN ∪ CLOSED) 동시 보유 가능한 zone 개수(uint32_t).
 *          0이면 한도 없음. MAR(Maximum Active Resources) 필드는 NVMe ZNS 스펙상 0-based(0xFFFFFFFF == unlimited)
 *          이며, 본 getter는 호스트 친화적인 형태로 변환해 돌려준다.
 *
 * 동기/배경: MAR은 MOR보다 큰 카테고리이다 — CLOSED zone도 메타데이터를 디바이스 내부 자원에서 차지하기 때문에,
 *   FULL/EMPTY로 보내지 않는 한 MAR을 잠식한다. 호스트가 너무 많은 zone을 partial-write 상태로 두면 새 zone open이
 *   거절되거나 디바이스 정책에 따라 implicit close가 발생한다.
 * 동작: nsdata_zns의 MAR 필드 0-based → 실제 한도 변환 반환. 0이면 그대로 0(=unlimited).
 * 실행 컨텍스트: 호스트 유저스페이스, lockless.
 * 호출자: ZNS-aware 응용, ZNS bdev. write scheduling/zone reset 정책 결정에 사용.
 * 호출 대상: 캐시 멤버 산술.
 * 에러 경로: 비-ZNS namespace이면 의미 없음.
 *
 * 호출 체인:
 *   사용자/ZNS bdev → [spdk_nvme_zns_ns_get_max_active_zones] → ns->nsdata_zns->mar 인코딩 변환
 */
uint32_t spdk_nvme_zns_ns_get_max_active_zones(struct spdk_nvme_ns *ns);

/**
 * Get the Zoned Namespace Command Set Specific Identify Controller data
 * as defined by the NVMe Zoned Namespace Command Set Specification.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return pointer to the controller data, or NULL if the controller does not
 * support the Zoned Command Set.
 */
/*
 * [한국어]
 * spdk_nvme_zns_ctrlr_get_data - ZNS Command Set Specific Identify Controller 데이터(IDCTL, CSI=0x02) 캐시 반환.
 *
 * @ctrlr: NVMe controller 핸들. probe/attach 단계에서 얻은 불투명 포인터.
 * @return: const struct spdk_nvme_zns_ctrlr_data * — controller 단위의 ZNS 식별자 캐시 포인터.
 *          이 controller가 ZNS Command Set을 지원하지 않으면 NULL. ZNS Identify Controller는 namespace와
 *          별도로 controller 전체에 적용되는 한도(예: ZASL)를 노출한다.
 *
 * 동기/배경: ZNS는 namespace 단위 한도(MOR/MAR)와 controller 단위 한도(ZASL)를 모두 정의한다. 본 함수는 후자를 캐시한다.
 *   사용자는 NULL/non-NULL 검사로 controller의 ZNS 지원 여부를 빠르게 확인할 수 있다.
 * 동작: ctrlr 객체의 ctrlr_data_zns 캐시 멤버 포인터 반환. SPDK는 attach 시 Identify Controller(CNS=0x06, CSI=0x02)를
 *   admin SQ로 한 번 발행해 캐시한다.
 * 실행 컨텍스트: 호스트 유저스페이스, 캐시 read만, lockless. 어느 SPDK 스레드에서 호출해도 안전.
 * 호출자: 사용자, ZNS bdev 초기화. ZASL을 알아야 zone append 한도를 계산.
 * 호출 대상: 멤버 dereference. SQE 발행 없음.
 * 에러 경로: ZNS 미지원 controller이면 NULL. 호출자는 NULL 검사로 분기.
 *
 * 호출 체인:
 *   사용자/ZNS bdev → [spdk_nvme_zns_ctrlr_get_data] → ctrlr->cdata_zns 캐시 dereference
 */
const struct spdk_nvme_zns_ctrlr_data *spdk_nvme_zns_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the maximum zone append data transfer size of a given NVMe controller.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return Maximum zone append data transfer size of the NVMe controller in bytes.
 */
/*
 * [한국어]
 * spdk_nvme_zns_ctrlr_get_max_zone_append_size - controller의 ZASL(Zone Append Size Limit)을 byte 단위로 반환.
 *
 * @ctrlr: NVMe controller (const). 캐시만 보므로 const도 허용.
 * @return: 단일 zone append 명령(0x7D)에 실어 보낼 수 있는 최대 데이터 전송 크기(bytes, uint32_t).
 *          NVMe ZNS 스펙의 ZASL은 0-based 2^N exponent 형태(데이터 = 2^(MPSMIN + ZASL) bytes)로 인코딩된다.
 *          본 getter는 호스트가 즉시 비교 가능한 byte 수로 변환해 돌려준다.
 *
 * 동기/배경: ZASL은 일반적인 namespace MDTS(Max Data Transfer Size)와 별개로 zone append 명령에만 적용되는
 *   상한이다. SSD가 단일 append를 단일 LBA-segment에 매핑하기 위해 보통 MDTS보다 작거나 같은 값을 광고한다.
 *   호스트는 이 값을 넘으면 -EINVAL이므로 큰 IO를 분할해야 한다.
 * 동작: cdata_zns의 ZASL exponent와 controller의 MPSMIN을 결합해 2^(MPSMIN+12+ZASL) bytes를 계산.
 *   캐시 접근만 수행하므로 SQE 발행 없음.
 * 실행 컨텍스트: 호스트 유저스페이스, lockless.
 * 호출자: 사용자, ZNS bdev. spdk_bdev_register 시 max_zone_append_size 필드를 채울 때 사용.
 * 호출 대상: 산술 연산만.
 * 에러 경로: ZNS 미지원이면 0 또는 미정의. ctrlr_get_data() NULL 검사를 선행할 것.
 *
 * 호출 체인:
 *   사용자/ZNS bdev → [spdk_nvme_zns_ctrlr_get_max_zone_append_size] → MPSMIN, cdata_zns->zasl 산술
 */
uint32_t spdk_nvme_zns_ctrlr_get_max_zone_append_size(const struct spdk_nvme_ctrlr *ctrlr);

/**
 * Submit a zone append I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the zone append I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param buffer Virtual address pointer to the data payload buffer.
 * \param zslba Zone Start LBA of the zone that we are appending to.
 * \param lba_count Length (in sectors) for the zone append operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_zns_zone_append - NVMe Zone Append (opcode 0x7D, contiguous PRP/단일 buffer 형태) 발행.
 *
 * @ns: 대상 ZNS namespace.
 * @qpair: I/O qpair (spdk_nvme_ctrlr_alloc_io_qpair()로 할당). qpair당 단일 SPDK thread만 진입해야 한다(SPDK lockless 규칙).
 * @buffer: 호스트 가상주소의 데이터 payload (sector_size * lba_count bytes). spdk_dma_*로 할당된 DMA-able 메모리여야 함
 *          (PCIe transport는 IOVA, fabrics transport는 등록된 RDMA MR 기반).
 * @zslba: 대상 zone의 시작 LBA(Zone Start LBA). zone append는 임의의 LBA가 아닌 zone 시작에만 정렬되어야 한다는
 *         스펙 제약이 있고, 컨트롤러가 실제 write 위치(ALBA)를 결정한다.
 * @lba_count: append할 LBA 개수. 0이 아닌 값이며, lba_count * sector_size <= ZASL이어야 함(초과 시 -EINVAL).
 * @cb_fn: 완료 콜백. spdk_nvme_qpair_process_completions()가 CQE를 폴링할 때 cb_fn(cb_arg, &cpl) 형태로 호출.
 *         cpl->cdw0(하위32) + cpl->cdw1(상위32)에 컨트롤러가 결정한 ALBA(append된 시작 LBA)가 실린다.
 * @cb_arg: cb_fn에 그대로 전달되는 사용자 컨텍스트.
 * @io_flags: SPDK_NVME_IO_FLAGS_FUSE_*, FORCE_UNIT_ACCESS, LIMITED_RETRY, PRCHK_* 등 NVMe CDW12 상위 비트로 매핑되는 플래그.
 * @return: 0 성공 submit, -EINVAL(요청 형식 오류 — ZASL 초과/zslba misalign 등), -ENOMEM(req 풀 고갈),
 *          -ENXIO(qpair transport-level 실패).
 *
 * 동기/배경: 일반 Write(0x01)는 호스트가 정확한 LBA를 지정해야 하므로 다중 producer가 같은 zone에 접근할 때 atomic
 *   write pointer 관리가 필요하다. Zone Append는 이를 디바이스로 위임 — 호스트는 zslba만 주면 되고, SSD가 atomically
 *   현재 write pointer 위치에 데이터를 붙이고 ALBA를 반환한다. 따라서 lockless multi-producer 패턴이 자연스러워진다.
 * 동작: 내부적으로 (1) request 객체 할당(req mempool), (2) cmd.opc=0x7D, nsid 설정, (3) cdw10/11 = zslba,
 *   cdw12 = (lba_count-1) | io_flags 등 ZNS spec의 CDW 매핑 채움, (4) PRP 또는 SGL로 buffer 변환, (5) qpair SQ tail에
 *   enqueue → MMIO doorbell write(PCIe) 또는 fabric capsule 송신.
 * 실행 컨텍스트: SPDK reactor thread. qpair 소유 스레드에서만 호출. 인터럽트 컨텍스트 호출 금지.
 * 호출자: ZNS bdev 모듈, ZNS 응용/도구.
 * 호출 대상: lib/nvme/nvme_zns.c의 internal append builder → nvme_qpair_submit_request → transport submit.
 * 에러 경로: 즉시 반환되는 -EINVAL/-ENOMEM/-ENXIO는 호출자가 처리. submit 후의 컨트롤러 측 에러(zone full, write boundary
 *   violation 등)는 cb_fn의 cpl.status로 전달된다.
 *
 * 호출 체인:
 *   사용자/ZNS bdev → [spdk_nvme_zns_zone_append] → nvme_zns_zone_append (lib/nvme/nvme_zns.c)
 *     → nvme_qpair_submit_request → transport (PCIe doorbell / TCP/RDMA capsule) → SSD ZNS 펌웨어
 *     → CQE → spdk_nvme_qpair_process_completions → cb_fn (호스트 reactor)
 */
int spdk_nvme_zns_zone_append(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      void *buffer, uint64_t zslba,
			      uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			      uint32_t io_flags);

/**
 * Submit a zone append I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the zone append I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param buffer Virtual address pointer to the data payload buffer.
 * \param metadata Virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size().
 * \param zslba Zone Start LBA of the zone that we are appending to.
 * \param lba_count Length (in sectors) for the zone append operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 * \param apptag_mask Application tag mask.
 * \param apptag Application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_zns_zone_append_with_md - Zone Append + 메타데이터(separate metadata buffer) 변형.
 *
 * @ns: 대상 ZNS namespace. PI(Protection Information)/메타데이터 영역이 sector마다 정의되어 있어야 함
 *      (spdk_nvme_ns_get_md_size() > 0).
 * @qpair: I/O qpair. qpair당 단일 thread 규칙 동일.
 * @buffer: 데이터 payload 가상주소(sector_size * lba_count). PRP/SGL 변환 대상.
 * @metadata: 메타데이터 payload 가상주소(md_size * lba_count). transport에 따라 metadata pointer(MPTR) 또는
 *            extended LBA SGL로 매핑됨. NULL이면 -EINVAL이 될 수 있음.
 * @zslba: 대상 zone의 시작 LBA.
 * @lba_count: append할 LBA 수.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 * @io_flags: SPDK_NVME_IO_FLAGS_PRCHK_* 비트는 PI 검사를 활성화해 메타데이터 내 reftag/guard/apptag를 컨트롤러가 검증하게 함.
 * @apptag_mask: PI Application Tag 마스크 — 컨트롤러는 (apptag_mask & 메타데이터의 apptag)를 PRACT/PRCHK 규칙에 따라 검사.
 * @apptag: 사용자가 부여하는 Application Tag(보통 호스트가 데이터 무결성 채널로 사용).
 * @return: 0 성공 submit / -EINVAL(요청 형식·ZASL 초과·md 불일치) / -ENOMEM(req 부족) / -ENXIO(transport 실패).
 *
 * 동기/배경: end-to-end data protection이 활성화된 ZNS namespace에서 zone append와 동시에 PI 메타데이터를 공급해야 할 때
 *   사용. 일반 zone_append는 metadata 인자를 받지 않으므로 PI ns에서는 본 _with_md 변형이 필요.
 * 동작: zone_append 경로와 동일한 SQE 빌더를 호출하되 cmd.mptr (또는 metadata SGL) 필드를 채우고 PI 관련 cdw14/15
 *   (Initial Logical Block Reference Tag = ILBRT, Logical Block Application Tag/Mask = LBAT/LBATM)를 셋팅한다.
 * 실행 컨텍스트: reactor thread, qpair 단독 진입.
 * 호출자: ZNS bdev with PI 활성화, end-to-end protection을 사용하는 응용.
 * 호출 대상: lib/nvme/nvme_zns.c의 nvme_ns_cmd_zone_append_with_md → submit_request.
 * 에러 경로: 즉시 errno 반환 또는 cpl.status로 PI guard fail/ref tag mismatch 보고.
 *
 * 호출 체인:
 *   사용자/PI-enabled ZNS bdev → [spdk_nvme_zns_zone_append_with_md] → nvme_ns_cmd_zone_append_with_md
 *     → nvme_qpair_submit_request → transport → SSD
 */
int spdk_nvme_zns_zone_append_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				      void *buffer, void *metadata, uint64_t zslba,
				      uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				      uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag);

/**
 * Submit a zone append I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the zone append I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param zslba Zone Start LBA of the zone that we are appending to.
 * \param lba_count Length (in sectors) for the zone append operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param reset_sgl_fn Callback function to reset scattered payload.
 * \param next_sge_fn Callback function to iterate each scattered payload memory
 * segment.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_zns_zone_appendv - Zone Append, Scatter-Gather List (SGL) 콜백 기반 변형.
 *
 * @ns: 대상 ZNS namespace.
 * @qpair: I/O qpair (단일 thread 진입).
 * @zslba: 대상 zone 시작 LBA.
 * @lba_count: append할 LBA 수.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트 — 동시에 SGL iterator state도 보유한다(reset/next 콜백이 이 ctx를 받음).
 * @io_flags: SPDK_NVME_IO_FLAGS_*.
 * @reset_sgl_fn: SPDK가 SGL 빌드를 시작/재시도할 때 호출 — 사용자 측 iterator를 0번째 segment로 되돌리는 콜백.
 *                재시도(예: PRP 재할당 실패) 시 다시 호출될 수 있음을 가정해야 한다.
 * @next_sge_fn: SPDK가 다음 SGE를 요구할 때 호출 — (address, length) 한 쌍을 반환. 모든 segment를 합한 길이가
 *                lba_count*sector_size여야 한다.
 * @return: 0 성공 submit / -EINVAL / -ENOMEM / -ENXIO.
 *
 * 동기/배경: 사용자 데이터가 비연속 메모리(예: vector I/O, blob의 cluster 분산)에 흩어져 있는 경우 단일 buffer 인자로는
 *   표현 불가. SPDK는 콜백 방식 SGL을 채택해 호출자가 자료구조와 무관하게 segment를 노출할 수 있게 한다.
 *   transport가 SGL을 native 지원(NVMe-oF RDMA/TCP, PCIe SGL ENABLE)하면 그대로 SGL로, 아니면 PRP로 변환된다.
 * 동작: zone_append와 같은 SQE 빌더이지만 PRP/SGL 빌드 단계에서 reset_sgl_fn → next_sge_fn 반복 호출로 segment를 수집.
 *   submit 후 완료 시 cb_fn 호출.
 * 실행 컨텍스트: reactor thread. reset/next 콜백도 같은 스레드에서 동기적으로 호출된다.
 * 호출자: blob/lvol/raid 등 비연속 버퍼를 다루는 상위 layer, 사용자 SGL 응용.
 * 호출 대상: nvme_ns_cmd_zone_appendv → SGL 빌더 → submit_request.
 * 에러 경로: 즉시 errno 반환 또는 cpl.status.
 *
 * 호출 체인:
 *   상위 layer(blob/raid/사용자) → [spdk_nvme_zns_zone_appendv]
 *     → reset_sgl_fn / next_sge_fn (사용자 콜백, 호스트) → submit → SSD
 */
int spdk_nvme_zns_zone_appendv(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       uint64_t zslba, uint32_t lba_count,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			       spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			       spdk_nvme_req_next_sge_cb next_sge_fn);

/**
 * Submit a zone append I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the zone append I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param zslba Zone Start LBA of the zone that we are appending to.
 * \param lba_count Length (in sectors) for the zone append operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param reset_sgl_fn Callback function to reset scattered payload.
 * \param next_sge_fn Callback function to iterate each scattered payload memory
 * segment.
 * \param metadata Virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size().
 * \param apptag_mask Application tag mask.
 * \param apptag Application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
/*
 * [한국어]
 * spdk_nvme_zns_zone_appendv_with_md - SGL 콜백 + 메타데이터 결합형 Zone Append. 4가지 append 변형 중 가장 일반화된 형태.
 *
 * @ns: 대상 ZNS namespace (PI/메타데이터 가능 ns).
 * @qpair: I/O qpair.
 * @zslba: 대상 zone 시작 LBA.
 * @lba_count: append할 LBA 수.
 * @cb_fn / @cb_arg: 완료 콜백과 컨텍스트.
 * @io_flags: SPDK_NVME_IO_FLAGS_PRCHK_*, FUA 등.
 * @reset_sgl_fn / @next_sge_fn: 데이터 영역 SGL iterator 콜백 (zone_appendv와 동일).
 * @metadata: 메타데이터 buffer 가상주소 (md_size * lba_count). 컨트롤러는 데이터와 별도 DMA 채널로 접근.
 *            transport에 따라 mptr 또는 metadata SGL로 전달됨.
 * @apptag_mask / @apptag: PI Application Tag 검사용. PRCHK_APPTAG가 io_flags에 있으면 컨트롤러가
 *            (apptag & apptag_mask) 비교를 수행.
 * @return: 0 성공 submit / -EINVAL / -ENOMEM / -ENXIO.
 *
 * 동기/배경: PI ns에서 SGL 데이터를 sumbit하려면 데이터·메타데이터·PI 비트를 모두 다뤄야 한다. ZNS bdev나 RAID layer가
 *   blob 같은 비연속 버퍼 + PI 메타데이터 결합 워크로드를 지원할 때 사용.
 * 동작: zone_appendv 빌더에 metadata pointer/SGL 채움 + ILBRT/LBAT/LBATM CDW 채움. 그 외는 zone_appendv와 동일.
 * 실행 컨텍스트: reactor thread, qpair 단독 진입. 콜백 동기 호출.
 * 호출자: PI 활성 ZNS bdev, 사용자 응용.
 * 호출 대상: nvme_ns_cmd_zone_appendv_with_md → submit_request.
 * 에러 경로: 즉시 errno 또는 cpl.status (PI mismatch 포함).
 *
 * 호출 체인:
 *   PI-enabled 상위 layer → [spdk_nvme_zns_zone_appendv_with_md] → SGL/MD 빌더 → submit → SSD
 */
int spdk_nvme_zns_zone_appendv_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				       uint64_t zslba, uint32_t lba_count,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				       spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				       spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				       uint16_t apptag_mask, uint16_t apptag);

/**
 * Submit a Close Zone operation to the specified NVMe namespace.
 *
 * \param ns Namespace.
 * \param qpair I/O queue pair to submit the request.
 * \param slba starting LBA of the zone to operate on.
 * \param select_all If this is set, slba will be ignored, and operation will
 * be performed on all zones that are in ZSIO or ZSEO state.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 on success. Negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_zns_close_zone - Zone Management Send (opcode 0x79) — Close Zone(action=0x01) 발행.
 *
 * @ns: 대상 ZNS namespace.
 * @qpair: I/O qpair (단일 thread).
 * @slba: 대상 zone의 시작 LBA. zone 경계에 정렬되어 있어야 하며 그렇지 않으면 컨트롤러가 Invalid Field로 거부.
 *        select_all=true면 무시됨.
 * @select_all: true 설정 시 cdw13의 Select All 비트가 켜져 slba 무시되고, 현재 ZSIO/ZSEO(Implicitly/Explicitly Open)
 *              상태인 모든 zone에 대해 일괄 close 수행. false면 slba가 가리키는 단일 zone만 대상.
 * @cb_fn / @cb_arg: 완료 콜백/컨텍스트.
 * @return: 0 submit 성공 / 음수 errno (-EINVAL/-ENOMEM/-ENXIO).
 *
 * 동기/배경: Close는 zone state를 ZSIO/ZSEO → ZSC(Closed)로 전이시켜 컨트롤러 내부 OPEN 자원을 회수한다.
 *   ZSC 상태에서도 zone은 ACTIVE로 카운트되어 MAR을 잠식하지만 MOR은 풀린다. write pointer는 보존되며,
 *   이후 Open Zone 또는 Reset Zone으로만 다음 전이가 가능.
 * 동작: SQE에 opc=0x79, nsid 설정, cdw10/11 = slba, cdw13 = (select_all<<8) | 0x01(action=Close) 채워 submit.
 * 실행 컨텍스트: reactor thread, qpair 단독.
 * 호출자: ZNS bdev (zone reset 정책), 사용자 도구.
 * 호출 대상: lib/nvme/nvme_zns.c의 nvme_zns_zone_mgmt_send → submit_request → transport.
 * 에러 경로: 잘못된 state(예: ZSE) → cpl.status에 Invalid Zone State Transition.
 *
 * 호출 체인:
 *   사용자/ZNS bdev → [spdk_nvme_zns_close_zone] → nvme_zns_zone_mgmt_send → submit → SSD
 */
int spdk_nvme_zns_close_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			     uint64_t slba, bool select_all,
			     spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a Finish Zone operation to the specified NVMe namespace.
 *
 * \param ns Namespace.
 * \param qpair I/O queue pair to submit the request.
 * \param slba starting LBA of the zone to operate on.
 * \param select_all If this is set, slba will be ignored, and operation will
 * be performed on all zones that are in ZSIO, ZSEO, or ZSC state.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 on success. Negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_zns_finish_zone - Zone Management Send (opcode 0x79) — Finish Zone(action=0x02) 발행.
 *
 * @ns: 대상 ZNS namespace.
 * @qpair: I/O qpair.
 * @slba: 대상 zone 시작 LBA(select_all=true면 무시).
 * @select_all: true → ZSIO/ZSEO/ZSC(IO/EO/Closed) 모든 zone 일괄 finish. false → slba가 지정한 단일 zone.
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0 submit 성공 / 음수 errno.
 *
 * 동기/배경: Finish는 zone을 강제로 ZSF(Full)로 전이시킨다. write pointer가 zone 끝에 도달하지 않았어도
 *   호스트가 "이 zone은 이제 더 쓰지 않겠다"고 선언할 때 사용. 이후 추가 write/append는 거부되며 Reset해야 다시 EMPTY로 복귀.
 *   ACTIVE 카운트(MAR)에서도 빠져 자원이 즉시 회수된다.
 * 동작: cdw13 action=0x02(Finish), select_all 비트 설정. 그 외는 close_zone과 동일.
 * 실행 컨텍스트: reactor thread, qpair 단독.
 * 호출자: ZNS bdev, GC 정책, 사용자 도구.
 * 호출 대상: nvme_zns_zone_mgmt_send.
 * 에러 경로: 잘못된 상태(ZSE/ZSF/ZSRO/ZSO) → cpl.status에 Invalid Zone State Transition.
 *
 * 호출 체인:
 *   사용자/ZNS bdev → [spdk_nvme_zns_finish_zone] → nvme_zns_zone_mgmt_send → submit → SSD
 */
int spdk_nvme_zns_finish_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      uint64_t slba, bool select_all,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a Open Zone operation to the specified NVMe namespace.
 *
 * \param ns Namespace.
 * \param qpair I/O queue pair to submit the request.
 * \param slba starting LBA of the zone to operate on.
 * \param select_all If this is set, slba will be ignored, and operation will
 * be performed on all zones that are in ZSC state.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 on success. Negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_zns_open_zone - Zone Management Send (opcode 0x79) — Open Zone(action=0x03) 발행.
 *
 * @ns: 대상 ZNS namespace.
 * @qpair: I/O qpair.
 * @slba: 대상 zone 시작 LBA(select_all=true면 무시).
 * @select_all: true → ZSC(Closed) 모든 zone 일괄 open(이전에 close된 zone들을 다시 EXPLICIT_OPEN으로 끌어올림).
 *              false → slba가 지정한 단일 zone.
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0 submit 성공 / 음수 errno.
 *
 * 동기/배경: Open Zone은 zone을 ZSEO(Explicitly Opened)로 전이시킨다. 일반 write/append가 zone에 처음 닿으면
 *   자동으로 ZSIO(Implicitly Opened)가 되지만, 호스트가 "이 zone에 대량 write를 곧 할 것이다"라고 명시적으로
 *   알리려면 명시적 Open이 유리하다(MOR 자원을 미리 확보). 또한 Close→Open 재활성화에도 사용.
 *   상태 전이: ZSE/ZSC → ZSEO. 이미 IO/EO이면 무효.
 * 동작: cdw13 action=0x03(Open), select_all 비트 설정. 나머지 SQE 빌드는 close/finish와 동일.
 * 실행 컨텍스트: reactor thread, qpair 단독.
 * 호출자: ZNS bdev, ZNS-aware FS(예: ZenFS), 사용자 도구.
 * 호출 대상: nvme_zns_zone_mgmt_send.
 * 에러 경로: MOR 초과 시 Too Many Open Zones, 잘못된 상태 시 Invalid Zone State Transition (cpl.status).
 *
 * 호출 체인:
 *   사용자/ZNS bdev → [spdk_nvme_zns_open_zone] → nvme_zns_zone_mgmt_send → submit → SSD
 */
int spdk_nvme_zns_open_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			    uint64_t slba, bool select_all,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a Reset Zone operation to the specified NVMe namespace.
 *
 * \param ns Namespace.
 * \param qpair I/O queue pair to submit the request.
 * \param slba starting LBA of the zone to operate on.
 * \param select_all If this is set, slba will be ignored, and operation will
 * be performed on all zones that are in ZSIO, ZSEO, ZSC, or ZSF state.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 on success. Negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_zns_reset_zone - Zone Management Send (opcode 0x79) — Reset Zone(action=0x04) 발행.
 *
 * @ns: 대상 ZNS namespace.
 * @qpair: I/O qpair.
 * @slba: 대상 zone 시작 LBA(select_all=true면 무시).
 * @select_all: true → ZSIO/ZSEO/ZSC/ZSF(Implicit/Explicit Open/Closed/Full) 모든 zone 일괄 reset.
 *              false → 단일 zone.
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0 submit 성공 / 음수 errno.
 *
 * 동기/배경: Reset은 zone을 ZSE(Empty)로 되돌리는 유일한 경로이다. SSD가 zone 내 NAND 블록을 erase하고 write pointer를
 *   ZSLBA로 리셋한다. ZNS의 "지운다"는 의미와 정확히 일치하며, 호스트의 GC/wear-leveling 정책 핵심 동작이다.
 *   ZSRO(Read-Only) zone은 reset 불가, ZSO(Offline)는 reset 후에도 OFFLINE 유지.
 * 동작: cdw13 action=0x04(Reset), select_all 비트. 그 외 SQE 빌드는 동일.
 * 실행 컨텍스트: reactor thread, qpair 단독.
 * 호출자: ZNS bdev, FS의 GC, 사용자 도구.
 * 호출 대상: nvme_zns_zone_mgmt_send.
 * 에러 경로: ZSRO/ZSO에 reset 시도 시 Invalid Zone State Transition (cpl.status).
 *
 * 호출 체인:
 *   사용자/ZNS bdev → [spdk_nvme_zns_reset_zone] → nvme_zns_zone_mgmt_send → submit → SSD
 */
int spdk_nvme_zns_reset_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			     uint64_t slba, bool select_all,
			     spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a Offline Zone operation to the specified NVMe namespace.
 *
 * \param ns Namespace.
 * \param qpair I/O queue pair to submit the request.
 * \param slba starting LBA of the zone to operate on.
 * \param select_all If this is set, slba will be ignored, and operation will
 * be performed on all zones that are in ZSRO state.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 on success. Negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_zns_offline_zone - Zone Management Send (opcode 0x79) — Offline Zone(action=0x05) 발행.
 *
 * @ns: 대상 ZNS namespace.
 * @qpair: I/O qpair.
 * @slba: 대상 zone 시작 LBA(select_all=true면 무시).
 * @select_all: true → ZSRO(Read-Only) zone 모두 일괄 offline. false → slba 단일 zone.
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0 submit 성공 / 음수 errno.
 *
 * 동기/배경: Offline은 zone을 ZSO(Offline)로 영구적으로 사용 불가능 상태로 만든다. ZSRO 상태(미디어 결함 등으로 더 이상
 *   write 불가, read만 가능)인 zone을 호스트가 의식적으로 폐기할 때 사용. 한번 ZSO가 되면 이후 어떤 reset/open으로도
 *   복구되지 않으며 read도 거부된다. 사실상 namespace에서 해당 zone capacity를 영구 회수.
 * 동작: cdw13 action=0x05(Offline), select_all 비트. 그 외 SQE 빌드 동일.
 * 실행 컨텍스트: reactor thread, qpair 단독.
 * 호출자: ZNS-aware 관리 도구, 결함 처리 응용.
 * 호출 대상: nvme_zns_zone_mgmt_send.
 * 에러 경로: ZSRO가 아닌 zone에 시도 시 Invalid Zone State Transition (cpl.status).
 *
 * 호출 체인:
 *   사용자/관리 도구 → [spdk_nvme_zns_offline_zone] → nvme_zns_zone_mgmt_send → submit → SSD
 */
int spdk_nvme_zns_offline_zone(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       uint64_t slba, bool select_all,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a Set Zone Descriptor Extension operation to the specified NVMe namespace.
 *
 * \param ns Namespace.
 * \param qpair I/O queue pair to submit the request.
 * \param slba starting LBA of the zone to operate on.
 * \param buffer Virtual address pointer to the data payload buffer.
 * \param payload_size Payload buffer size.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 on success. Negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_zns_set_zone_desc_ext - Zone Management Send (opcode 0x79) — Set Zone Descriptor Extension(action=0x10) 발행.
 *
 * @ns: 대상 ZNS namespace. Identify에서 ZNS Zone Descriptor Extension을 지원해야 하며 (ZDES > 0), 그 크기에 맞는
 *      buffer가 필요하다.
 * @qpair: I/O qpair.
 * @slba: 대상 zone의 시작 LBA. zone state는 반드시 ZSE(Empty)이어야 하며, 이 명령이 성공하면 zone이 ZSC(Closed)로 자동 전이된다.
 * @buffer: descriptor extension data 가상주소(payload_size bytes). ZDES * 64 bytes에 정렬된 페이로드여야 한다.
 * @payload_size: buffer 크기(bytes). PRP/SGL 변환 대상.
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0 submit 성공 / 음수 errno.
 *
 * 동기/배경: Zone Descriptor Extension은 호스트가 zone 단위로 임의의 메타데이터(예: write generation, ZenFS의
 *   zone-level 정보)를 SSD에 보관하고 Report Zones (ZRA=0x01 ext report)로 회수할 수 있게 한다. 호스트 측 별도
 *   메타데이터 저장소 없이 SSD가 영속화해 주는 일종의 "zone-local note"이다. 빈 zone에만 부착 가능하다는 제약은
 *   기록 파편화/원자성 보장을 위해서이다.
 * 동작: cdw10/11=slba, cdw13 action=0x10, buffer를 PRP/SGL로 변환해 SQE에 붙임. 성공 시 컨트롤러는 zone의
 *   descriptor extension 영역에 buffer를 영속화하고 zone을 ZSE → ZSC로 전이.
 * 실행 컨텍스트: reactor thread, qpair 단독.
 * 호출자: ZNS-aware FS(ZenFS 등), 사용자 응용.
 * 호출 대상: lib/nvme/nvme_zns.c의 nvme_zns_zone_mgmt_send_with_data → submit_request.
 * 에러 경로: zone이 ZSE가 아니면 Invalid Zone State Transition, payload_size mismatch면 Invalid Field (cpl.status).
 *
 * 호출 체인:
 *   ZNS-aware FS → [spdk_nvme_zns_set_zone_desc_ext] → nvme_zns_zone_mgmt_send_with_data → submit → SSD
 */
int spdk_nvme_zns_set_zone_desc_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				    uint64_t slba, void *buffer, uint32_t payload_size,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Get a zone report from the specified NVMe namespace.
 *
 * \param ns Namespace.
 * \param qpair I/O queue pair to submit the request.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param slba starting LBA of the zone to operate on.
 * \param report_opts Filter on which zone states to include in the zone report.
 * \param partial_report If true, nr_zones field in the zone report indicates the number of zone
 * descriptors that were successfully written to the zone report. If false, nr_zones field in the
 * zone report indicates the number of zone descriptors that match the report_opts criteria.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 on success. Negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_zns_report_zones - Zone Management Receive (opcode 0x7A) — Report Zones (ZRA=0x00) 발행.
 *
 * @ns: 대상 ZNS namespace.
 * @qpair: I/O qpair.
 * @payload: 호스트 buffer 가상주소. 컨트롤러가 zone descriptor 배열을 DMA write할 대상.
 *           구조: 64-byte zone report header(nr_zones 등) + (zone_descriptor[64B] × N).
 * @payload_size: buffer 크기(bytes). 컨트롤러는 이 크기를 넘지 않도록 잘라서 채움.
 * @slba: 보고를 시작할 zone의 LBA. 이 LBA가 속한 zone부터 LBA 오름차순으로 보고됨. 다음 페이지 호출 시 마지막
 *        보고된 zone의 다음 ZSLBA를 넘기는 식으로 페이지네이션.
 * @report_opts: enum spdk_nvme_zns_zra_report_opts — ZRASF(Zone Receive Action Specific Field) 필터.
 *               0 = ALL (모든 zone), 그 외 ZSE/ZSIO/ZSEO/ZSC/ZSF/ZSRO/ZSO 각 상태별 필터에 매핑.
 * @partial_report: false → header.nr_zones는 필터 조건과 일치하는 namespace 전체 zone 수(buffer에 다 못 담겨도 큰 값).
 *                  true  → header.nr_zones는 buffer에 실제로 채워진 descriptor 수(작은 값). 페이지네이션 사용 시 true가 편리.
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0 submit 성공 / 음수 errno (-EINVAL/-ENOMEM/-ENXIO).
 *
 * 동기/배경: 호스트가 namespace의 모든 zone 상태(write pointer, capacity, state, type)를 일괄/조건 조회하기 위한 명령.
 *   ZNS-aware FS는 부팅 시 한 번 전 namespace를 report해 zone 메타데이터를 메모리에 빌드하는 것이 표준 패턴.
 * 동작: opc=0x7A, nsid 설정, cdw10/11=slba, cdw12 = (payload_size/4 - 1), cdw13 = ZRA(0x00) | (ZRASF<<8) |
 *   (partial<<16). PRP/SGL로 payload buffer 매핑. 컨트롤러가 zone descriptor 배열을 DMA write로 호스트 buffer에 채움.
 * 실행 컨텍스트: reactor thread, qpair 단독.
 * 호출자: ZNS-aware FS(부팅 시 전 zone 스캔), ZNS bdev(주기적 zone 상태 갱신), 사용자 도구.
 * 호출 대상: lib/nvme/nvme_zns.c의 nvme_zns_zone_mgmt_recv → submit_request.
 * 에러 경로: payload_size가 너무 작아 header조차 못 담으면 Invalid Field, 그 외 cpl.status.
 *
 * 호출 체인:
 *   ZNS-aware FS/사용자 → [spdk_nvme_zns_report_zones] → nvme_zns_zone_mgmt_recv → submit → SSD
 *     → CQE/DMA write → cb_fn (호스트 buffer에 zone descriptor 배열 도착)
 */
int spdk_nvme_zns_report_zones(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       void *payload, uint32_t payload_size, uint64_t slba,
			       enum spdk_nvme_zns_zra_report_opts report_opts, bool partial_report,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Get a extended zone report from the specified NVMe namespace.
 *
 * \param ns Namespace.
 * \param qpair I/O queue pair to submit the request.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param slba starting LBA of the zone to operate on.
 * \param report_opts Filter on which zone states to include in the extended zone report.
 * \param partial_report If true, nr_zones field in the extended zone report indicates the number of zone
 * descriptors that were successfully written to the extended zone report. If false, nr_zones field in the
 * extended zone report indicates the number of zone descriptors that match the report_opts criteria.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 on success. Negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvme_zns_ext_report_zones - Zone Management Receive (opcode 0x7A) — Extended Report Zones (ZRA=0x01) 발행.
 *
 * @ns: 대상 ZNS namespace. Identify ZNS의 ZDES(Zone Descriptor Extension Size) > 0이어야 의미 있음.
 * @qpair: I/O qpair.
 * @payload: 호스트 buffer 가상주소. 구조: 64-byte header + ((zone_descriptor[64B] + extension[ZDES*64B]) × N).
 *           즉 일반 report에 비해 zone당 ZDES*64 바이트의 사용자 메타데이터(set_zone_desc_ext로 부착한 데이터)가 함께 회수된다.
 * @payload_size: buffer 크기(bytes).
 * @slba: 보고 시작 zone의 LBA.
 * @report_opts: ZRASF 필터(report_zones와 동일).
 * @partial_report: nr_zones 의미 결정(report_zones와 동일).
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0 submit 성공 / 음수 errno.
 *
 * 동기/배경: ZNS-aware FS가 zone descriptor와 함께 자신이 set_zone_desc_ext로 저장해 둔 메타데이터를 한 번에 회수할 때 사용.
 *   별도 read를 발행할 필요 없이 zone state + 사용자 정의 extension을 결합해 받을 수 있어 부팅 시간이 단축된다.
 * 동작: opc=0x7A, ZRA=0x01(Extended Report). 그 외 CDW 매핑은 report_zones와 동일.
 * 실행 컨텍스트: reactor thread, qpair 단독.
 * 호출자: ZenFS 같은 ZNS-aware FS, 사용자 도구(zone extension 관찰).
 * 호출 대상: nvme_zns_zone_mgmt_recv (ZRA=0x01 인자) → submit_request.
 * 에러 경로: ZDES=0인 ns에 호출 시 Invalid Field, payload_size 부족 시 Invalid Field (cpl.status).
 *
 * 호출 체인:
 *   ZNS-aware FS → [spdk_nvme_zns_ext_report_zones] → nvme_zns_zone_mgmt_recv (ZRA=0x01) → submit → SSD
 *     → CQE/DMA write → cb_fn (호스트 buffer에 descriptor + extension 배열 도착)
 */
int spdk_nvme_zns_ext_report_zones(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				   void *payload, uint32_t payload_size, uint64_t slba,
				   enum spdk_nvme_zns_zra_report_opts report_opts, bool partial_report,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
/* [한국어] C++ 컴파일러 환경에서 위에서 연 extern "C" 블록을 닫는다. C 컴파일에서는 매크로 미정의로 인해 이 블록이 통째로 무시된다.
 * 이 #ifdef는 라인 72의 #ifdef __cplusplus 블록과 짝을 이루며, 그 사이에 선언된 모든 spdk_nvme_zns_*() 함수가
 * C 링키지로 노출되도록 보장한다. */
}
/* [한국어] extern "C" { 블록의 닫는 중괄호. C 컴파일에서는 위 #ifdef가 거짓이므로 이 라인까지 모두 전처리기 단계에서 사라진다. */
#endif

#endif /* SPDK_NVME_ZNS_H */
/* [한국어] SPDK_NVME_ZNS_H 헤더 가드의 종결. 라인 63의 #ifndef와 짝을 이뤄, 같은 translation unit에서 nvme_zns.h가
 * 두 번째로 include될 때 본 파일 전체가 비어 있는 것처럼 처리되도록 한다(중복 선언 방지). */
