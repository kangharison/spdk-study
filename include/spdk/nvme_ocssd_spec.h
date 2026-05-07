/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Open-Channel specification definitions
 */

/*
 * [한국어 설명] Open-Channel SSD (OCSSD) 1.2/2.0 와이어 프로토콜 정의 (nvme_ocssd_spec.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 OCSSD(Open-Channel SSD) 1.2/2.0 스펙(LightNVM / CNEX Labs / Lightning
 * Memory Solutions가 표준화)에서 정의하는 모든 와이어 레벨 상수, 구조체, enum,
 * 매크로를 C 언어 표현으로 묶어둔다. OCSSD는 "호스트 측 FTL(Flash Translation
 * Layer)" 모델을 채택한 SSD로서, 디바이스 내부의 마모 평준화/가비지 컬렉션을
 * 호스트로 노출하여 chunk(=erase block) 단위 쓰기/리셋, 병렬 PU(Parallel Unit,
 * NAND LUN과 유사한 독립 병렬 단위) 활용, 마모도 정보 조회, chunk 상태 비동기
 * 통지 등을 지원한다. 본 헤더는 NVMe 기본 spec(nvme_spec.h) 위에 OCSSD
 * vendor-specific opcode(Admin 0xE2, I/O 0x90~0x93)와 log page(0xCA/0xD0),
 * feature(0xCA), media error SC(0xC0/0xC1/0xD0/0xF0~0xF2) 정의를 얹어서,
 * lib/nvme/nvme_ctrlr_ocssd_cmd.c, lib/nvme/nvme_ns_ocssd_cmd.c가 SQE
 * (Submission Queue Entry)를 빌드할 때 정확한 바이트 레이아웃을 사용할 수
 * 있도록 한다. 즉 이 파일이 정의하는 비트필드 레이아웃은 그대로 PCIe 트랜잭션의
 * SQE 페이로드, Identify Geometry 응답 4096바이트, Chunk Information Log Page
 * 엔트리(32B), Chunk Notification Log 엔트리(64B), vector I/O CQE 16바이트에
 * 매핑된다. 본 헤더는 데이터 레이아웃 정의만 포함하므로 빌드 산출물에는
 * 함수 심볼이 발생하지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe I/O 스택을 따르면 다음 순서로 호출된다.
 *   [응용/모듈]
 *     → spdk_nvme_ocssd_ctrlr_cmd_geometry()  (lib/nvme/nvme_ctrlr_ocssd_cmd.c)
 *     → spdk_nvme_ocssd_ns_cmd_vector_*()     (lib/nvme/nvme_ns_ocssd_cmd.c)
 *         → 본 헤더의 SPDK_OCSSD_OPC_* / 구조체 비트필드를 사용해 SQE 작성
 *     → nvme_qpair_submit_request()           (lib/nvme/nvme_qpair.c)
 *     → MMIO doorbell write → PCIe → NVMe SSD 펌웨어
 *     ← Completion Queue Entry
 *     ← Identify Geometry / Chunk Info Log → 본 헤더의 구조체로 캐스팅 후 해석
 * 이 파일은 호스트 유저스페이스(SPDK reactor 스레드)에서 컴파일되며, 어떠한
 * 실행 코드도 포함하지 않고 데이터 레이아웃만 제공한다. 따라서 reactor·thread·
 * poller 어떤 컨텍스트에서도 그대로 include 가능하며, ABI 호환성은 NVMe 스펙
 * 1.x 위에 정의된 OCSSD 1.2/2.0 와이어 포맷에 의해 보장된다. 컴파일 타임
 * SPDK_STATIC_ASSERT로 모든 핵심 구조체의 sizeof를 spec과 동일한 8/4096/32/
 * 64/16바이트로 강제하여 패딩 또는 정렬 오차가 발생하면 빌드 단계에서 즉시
 * 실패하도록 설계되어 있다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h(uint*_t 등), spdk/assert.h(SPDK_STATIC_ASSERT로 와이어
 *   포맷 크기 검증), spdk/nvme_spec.h(spdk_nvme_status, NVMe 공통 SQE/CQE/Log
 *   Page 골격). OCSSD는 NVMe 1.x 위에 vendor-specific opcode 0xE2(Geometry),
 *   0x90~0x93(vector I/O), 0xCA/0xD0(log page), 0xCA(feature)를 얹는 형태이므로
 *   기본 NVMe 정의가 반드시 선행되어야 한다.
 * - 데이터 흐름: 호스트가 빌드한 SQE → SQ → 디바이스 → 처리 결과(geometry 4KB
 *   페이지 / chunk info 32B 엔트리 배열 / vector CQE의 lba_status 비트맵) →
 *   호스트가 본 헤더 구조체로 파싱. 반대로 호스트의 명령(vector reset/write/
 *   read/copy)은 cdw10/11(LBA 또는 LBA list dptr), cdw12(NLB-1) 인코딩을 거쳐
 *   디바이스에 전달된다. AER(Asynchronous Event Request) 경로로 디바이스가
 *   chunk notification(에러율 변화, refresh, WIT 초과 등)을 알리면 호스트는
 *   Get Log Page 0xD0를 발급해 본 헤더의 chunk_notification_entry 배열로 파싱.
 * - 공유 자료구조: spdk_ocssd_geometry_data는 디바이스 부팅 시 1회 캐시되어
 *   호스트 FTL 전반에 readonly로 공유. spdk_ocssd_chunk_information_entry
 *   배열은 호스트 FTL의 chunk state cache에 사상되어 lib/ftl/* 모듈이
 *   write pointer/마모도 추적에 사용. spdk_ocssd_vector_cpl은 NVMe CQE ring
 *   슬롯 위에 직접 캐스팅되므로 메모리 복사 없이 in-place 파싱.
 * - 사용자: lib/nvme/nvme_ns_ocssd_cmd.c가 spdk_nvme_ocssd_ns_cmd_vector_*에서
 *   SPDK_OCSSD_OPC_VECTOR_* opcode를 SQE.opc에 채우고, lib/nvme/
 *   nvme_ctrlr_ocssd_cmd.c가 SPDK_OCSSD_OPC_GEOMETRY 명령을 발급한다.
 *   상위 응용(예: ftl 모듈)은 spdk_ocssd_geometry_data·
 *   spdk_ocssd_chunk_information_entry를 읽어 호스트 측 FTL 매핑 테이블을
 *   유지한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_ocssd_dev_lba_fmt(8B): 디바이스의 LBA 비트 분할(grp/pu/chk/lbk
 *   비트 길이) 기술. PPA(Physical Page Address)를 평면 LBA로 인코딩할 때의
 *   좌표 폭 정의. geometry_data.lbaf로 임베드.
 * - struct spdk_ocssd_geometry_data (4096B): Identify Geometry(opcode 0xE2)
 *   응답 페이로드. 버전(mjr/mnr), mccap(vector copy/multi-reset 능력),
 *   num_grp/num_pu/num_chk/clba(기하), ws_min/ws_opt/mw_cunits/maxoc/maxocpu
 *   (쓰기 제약), tRD/tWR/tCRS 타이밍, vendor specific 영역으로 구성.
 * - struct spdk_ocssd_chunk_information_entry (32B): Chunk Info Log Page
 *   (LID 0xCA) 엔트리. cs(free/closed/open/offline), ct(seq/rnd/size_deviate),
 *   wli, slba, cnlb, wp 필드로 chunk state machine과 write pointer 노출.
 * - struct spdk_ocssd_chunk_notification_entry (64B): Chunk Notification
 *   Log(LID 0xD0). 디바이스가 호스트에 chunk 상태 변경(error rate 변화,
 *   refresh, WIT 초과)을 비동기 통지.
 * - struct spdk_ocssd_vector_cpl (16B): vector I/O 명령의 CQE 확장으로,
 *   dword 0/1에 64비트 lba_status 비트맵(각 LBA의 성공/실패)이 들어간다.
 * - enum spdk_ocssd_admin_opcode / io_opcode / log_page / feat /
 *   media_error_status_code: OCSSD가 NVMe 상에 추가 정의한 opcode/LID/FID/
 *   media error SC들. 와이어 상의 정확한 16진 값 그대로 매크로화.
 * - SPDK_OCSSD_IO_FLAGS_LIMITED_RETRY: vector I/O 명령의 cdw12 bit31에
 *   해당하는 limited retry 플래그.
 * - SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES(=64): 한 vector I/O 명령에 묶일 수 있는
 *   LBA list 최대 길이. lba_status 비트맵의 64비트 폭과 정확히 일치.
 */

/* [한국어] include guard — 본 헤더가 동일 컴파일 단위에서 두 번 이상 전개되어
 * struct/enum 재정의 충돌이 발생하는 것을 방지한다.
 * 설정자: 컴파일러 전처리기. 읽는 자: 컴파일러 전처리기.
 * 값 범위: 매크로 정의/미정의 두 상태. 동기화: 단일 translation unit 내부
 * 전처리 시점에만 영향. */
#ifndef SPDK_NVME_OCSSD_SPEC_H
#define SPDK_NVME_OCSSD_SPEC_H

/* [한국어] uint8_t/uint16_t/uint32_t/uint64_t 등 고정폭 정수 타입을 비롯해
 * SPDK 전반에서 쓰는 표준 헤더(stddef/stdint/stdbool/string 등)를 한 번에
 * 들여온다. OCSSD 와이어 구조체는 비트필드와 정확한 바이트 폭이 중요하므로
 * 이 표준 타입 선언이 선행되어야 한다.
 * 설정자: SPDK 빌드 시스템(common stdinc 묶음). 읽는 자: 본 헤더 전체의
 * 구조체 정의가 이 타입에 의존. 동기화: 컴파일 타임. */
#include "spdk/stdinc.h"

#ifdef __cplusplus
/* [한국어] C++에서 이 헤더를 include해도 OCSSD 구조체와 매크로의 심볼이
 * C 링크 규약(name mangling 없음)으로 노출되도록 extern "C" 블록 시작.
 * SPDK는 C로 작성되었지만 사용자 측 C++ 코드와의 ABI 호환을 보장한다.
 * 설정자: 사용자 컴파일러 모드. 읽는 자: 링커가 심볼 해소 시.
 * 값 범위: __cplusplus 정의 시에만 활성. 동기화: 컴파일 타임. */
extern "C" {
#endif

/* [한국어] SPDK_STATIC_ASSERT 매크로 — 컴파일 타임에 sizeof()를 검증하기 위해
 * 필요. 본 헤더 하단에서 와이어 포맷 구조체 크기(8/4096/32/64/16바이트)를
 * 정확히 강제하는 데 사용된다. 잘못 패딩되면 컴파일 실패하여 런타임 와이어
 * 불일치를 사전 차단.
 * 설정자: SPDK 공통 헤더. 읽는 자: 본 헤더 하단의 5개 SPDK_STATIC_ASSERT.
 * 값 범위: _Static_assert / static_assert 호환 매크로. 동기화: 컴파일 타임. */
#include "spdk/assert.h"

/* [한국어] NVMe 1.x 기본 spec(spdk_nvme_status, SQE/CQE 공통 골격, 기본
 * opcode 정의)을 들여온다. OCSSD는 NVMe 위에 vendor extension으로 얹히므로
 * 본 헤더의 spdk_ocssd_vector_cpl이 사용하는 spdk_nvme_status가 반드시
 * 정의되어 있어야 한다.
 * 설정자: SPDK NVMe 표준 헤더. 읽는 자: spdk_ocssd_vector_cpl.status.
 * 값 범위: NVMe Base Specification 1.x/2.x 정의 전체. 동기화: 컴파일 타임. */
#include "spdk/nvme_spec.h"

/** A maximum number of LBAs that can be issued by vector I/O commands */
/* [한국어] vector I/O(opcode 0x90~0x93)에서 한 SQE가 다룰 수 있는 LBA의
 * 최대 개수를 64로 정의. SQE의 cdw12 NLB(Number of Logical Blocks) 필드는
 * 0's based 6비트 부분과 매핑되며, OCSSD spec에서 한 vector 명령의 PPA list
 * 길이를 64로 제한한다(lba_status 64비트 비트맵과 정확히 일치).
 * 와이어 위치: 별도 와이어 필드 아님 — 호스트 측 상한 검사 상수.
 * 설정자: 본 헤더가 컴파일 타임에 정의. 읽는 자: lib/nvme/nvme_ns_ocssd_cmd.c
 * 가 lba_list 배열 길이가 이 값을 초과하면 -EINVAL 반환.
 * 값 범위: 64 고정(OCSSD 1.2/2.0 spec 상 변경 불가).
 * 동기화: 컴파일 타임 상수 — 멀티스레드 접근에서도 불변. */
#define SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES	64

/*
 * [한국어]
 * struct spdk_ocssd_dev_lba_fmt — 디바이스가 LBA를 grp/pu/chk/lbk 4축으로
 * 나누어 인코딩할 때 각 축에 할당된 비트 폭을 알려주는 8바이트 기술자.
 *
 * 어디서 등장: spdk_ocssd_geometry_data.lbaf 필드(Identify Geometry 응답
 * 바이트 8~15). 호스트 FTL은 이 값을 받아 PPA(grp,pu,chk,lbk)→ flat LBA
 * 변환을 위한 비트 시프트/마스크를 구성한다. 예를 들어 grp_len=2, pu_len=4,
 * chk_len=11, lbk_len=21이면 LBA = (grp<<(pu_len+chk_len+lbk_len)) |
 * (pu<<(chk_len+lbk_len)) | (chk<<lbk_len) | lbk 형태로 인코딩.
 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 측 FTL(SPDK ftl 모듈 또는
 * 응용). 값 범위: 4개 _len 합 ≤ 64(LBA 64비트 한도).
 * 동기화: 디바이스 부팅 시 고정 — 런타임 변경 없음. 호스트는 처음 1회 읽고
 * 정적 캐시에 보관, 락 없이 readonly 공유.
 */
struct spdk_ocssd_dev_lba_fmt {
	/**  Contiguous number of bits assigned to Group addressing */
	uint8_t grp_len;
	/* [한국어] LBA 인코딩에서 Group(channel과 유사한 최상위 병렬 단위)
	 * 비트 수. 와이어 위치: Identify Geometry 응답 byte 8.
	 * 의미: PPA 최상위 grp 인덱스가 차지하는 연속 비트 길이.
	 * 값 범위: 0~255이지만 실제로는 num_grp ≤ 2^grp_len을 만족하는 작은 값
	 * (보통 1~3비트). 설정자: 디바이스 펌웨어(부팅 시 고정).
	 * 읽는 자: 호스트 FTL(LBA→PPA 디코딩 시 시프트량 계산).
	 * 동기화: 부팅 후 readonly — 별도 락 불필요. */

	/** Contiguous number of bits assigned to PU addressing */
	uint8_t pu_len;
	/* [한국어] PU(Parallel Unit, 그룹 내 병렬 LUN)에 할당된 비트 수.
	 * 와이어 위치: Identify Geometry 응답 byte 9.
	 * 의미: grp 다음으로 위치하는 pu 인덱스의 비트 폭.
	 * 값 범위: num_pu ≤ 2^pu_len(보통 3~5비트, 8~32 PU/group 표현).
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL.
	 * 동기화: 부팅 후 readonly. */

	/** Contiguous number of bits assigned to Chunk addressing */
	uint8_t chk_len;
	/* [한국어] Chunk(=erase block 1개)에 할당된 비트 수.
	 * 와이어 위치: Identify Geometry 응답 byte 10.
	 * 의미: pu 다음에 오는 chunk 인덱스 비트 폭.
	 * 값 범위: num_chk ≤ 2^chk_len(보통 9~12비트, 512~4096 chunk/PU).
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 chunk 단위 reset/
	 * write 명령에서 PPA를 디코드할 때.
	 * 동기화: 부팅 후 readonly. */

	/** Contiguous number of bits assigned to logical blocks within Chunk */
	uint8_t lbk_len;
	/* [한국어] 한 chunk 내부의 logical block 인덱스 비트 수.
	 * 와이어 위치: Identify Geometry 응답 byte 11.
	 * 의미: 가장 하위에서 chunk 내부 페이지/섹터 위치를 표현.
	 * 값 범위: clba ≤ 2^lbk_len(보통 12~16비트).
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 write pointer 비교,
	 * sequential 검증 시 사용.
	 * 동기화: 부팅 후 readonly. */

	uint8_t reserved[4];
	/* [한국어] OCSSD spec 예약 영역(byte 12~15, 4바이트).
	 * 와이어 위치: dev_lba_fmt 끝부분, 8바이트 정렬을 맞추는 padding 겸용.
	 * 의미: 향후 spec 확장(예: 추가 좌표 축 또는 plane 인덱스)을 위한
	 * placeholder.
	 * 값 범위: spec 상 디바이스는 0으로 채워야 함.
	 * 설정자: 디바이스(0). 읽는 자: 호스트는 의미 부여 금지(무시).
	 * 동기화: 의미 없음 — readonly 무시 영역. */
};
/* [한국어] 와이어 포맷 안전장치: dev_lba_fmt 구조체는 OCSSD spec 상 정확히
 * 8바이트여야 한다. 컴파일러가 패딩을 추가하면 Identify Geometry 응답
 * 4096바이트 레이아웃이 어긋나 PPA 디코딩이 깨지므로 즉시 컴파일 실패시킴.
 * 설정자: 본 헤더의 컴파일 타임 검증. 읽는 자: 빌드 시스템(에러 메시지).
 * 값 범위: sizeof 결과가 8이 아니면 _Static_assert 실패. 동기화: 컴파일 타임. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_ocssd_dev_lba_fmt) == 8, "Incorrect size");

/*
 * [한국어]
 * struct spdk_ocssd_geometry_data — Identify Geometry(admin opcode 0xE2)의
 * 응답으로 디바이스가 돌려주는 4096바이트 페이지 전체 레이아웃.
 *
 * 어디서 등장: spdk_nvme_ocssd_ctrlr_cmd_geometry()가 4KB DMA 버퍼를 PRP에
 * 등록하여 0xE2 명령을 발급 → 디바이스 펌웨어가 이 구조 그대로 채워서 반환.
 * 호스트 FTL(또는 ftl 모듈/사용자 도구)이 부팅 시 한 번 읽고 캐시하여 모든
 * I/O의 PPA 인코딩, 쓰기 단위 결정, 타이밍 추정에 활용한다.
 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 유저스페이스(SPDK reactor 스레드).
 * 동기화: 부팅 시 1회 읽고 정적으로 보관 — 런타임 변경 없으므로 락 불필요.
 */
struct spdk_ocssd_geometry_data {
	/** Major Version Number */
	uint8_t		mjr;
	/* [한국어] OCSSD spec 메이저 버전(예: 2 → 2.0).
	 * 와이어 위치: Identify Geometry 응답 byte 0.
	 * 값 범위: 1(=1.2 호환), 2(=2.0). 그 외 값은 미지원.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트가 1.2/2.0 분기를 결정
	 * (필드 의미·opcode가 spec별로 다름).
	 * 동기화: 디바이스 부팅 시 고정 — readonly. */

	/** Minor Version Number */
	uint8_t		mnr;
	/* [한국어] OCSSD spec 마이너 버전.
	 * 와이어 위치: byte 1. 값 범위: 0~255. 1.2의 경우 mjr=1, mnr=2.
	 * 설정자: 디바이스. 읽는 자: 호스트가 동일 메이저 내 점진 호환을 판단.
	 * 동기화: readonly. */

	uint8_t		reserved1[6];
	/* [한국어] 예약(byte 2~7), 6바이트 padding.
	 * 와이어 위치: 버전 필드 직후, lbaf(8B 정렬) 시작 직전.
	 * 의미: spec 확장 여지 — 향후 minor 단위 호환성 플래그가 들어갈 가능성.
	 * 값 범위: 디바이스는 0 채움.
	 * 설정자: 디바이스(0). 읽는 자: 호스트는 무시.
	 * 동기화: 의미 없음. */

	/** LBA format */
	struct spdk_ocssd_dev_lba_fmt	lbaf;
	/* [한국어] LBA 비트 분할(grp/pu/chk/lbk) 기술자, 8바이트.
	 * 와이어 위치: byte 8~15. 의미: PPA의 좌표 폭 정의(위 dev_lba_fmt 참조).
	 * 값 범위: 4개 _len 합 ≤ 64.
	 * 설정자: 디바이스. 읽는 자: 호스트 FTL의 LBA 인코더/디코더.
	 * 동기화: readonly. */

	/** Media and Controller Capabilities */
	struct {
		/* Supports the Vector Chunk Copy I/O Command */
		uint32_t	vec_chk_cpy	: 1;
		/* [한국어] mccap.bit0 — Vector Chunk Copy(opcode 0x93) 지원 여부.
		 * 와이어 위치: byte 16의 LSB(LE 32비트 워드의 bit0).
		 * 값 범위: 1=지원(SPDK_OCSSD_OPC_VECTOR_COPY 발급 가능), 0=미지원.
		 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 device-side copy로
		 * GC를 가속할지 host-side read-then-write로 갈지 결정.
		 * 동기화: readonly. */

		/* Supports multiple resets when a chunk is in its free state */
		uint32_t	multi_reset	: 1;
		/* [한국어] mccap.bit1 — chunk가 free 상태에서도 추가 reset 명령을
		 * 허용하는지 여부.
		 * 와이어 위치: byte 16의 bit1.
		 * 값 범위: 1=free chunk에 reset 허용(에러 없이 NOP 또는 재초기화),
		 * 0=free chunk에 reset 시 SPDK_OCSSD_SC_INVALID_RESET(0xC1) 반환.
		 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL의 chunk 재사용 정책.
		 * 동기화: readonly. */

		uint32_t	reserved	: 30;
		/* [한국어] mccap의 나머지 30비트 예약(byte 16의 bit2 ~ byte 19의 MSB).
		 * 의미: 향후 capability 비트(예: vector_resize, scrub 등) 추가 여지.
		 * 값 범위: 디바이스 0, 호스트 무시.
		 * 설정자: 디바이스(0). 읽는 자: 호스트는 마스킹/무시.
		 * 동기화: readonly. */
	} mccap;

	uint8_t		reserved2[12];
	/* [한국어] 예약(byte 20~31, 12바이트).
	 * 의미: mccap과 wit 사이의 spec 확장 placeholder. 32바이트 정렬 유지.
	 * 값 범위: 디바이스 0 채움.
	 * 설정자: 디바이스(0). 읽는 자: 호스트는 무시.
	 * 동기화: 의미 없음. */

	/** Wear-level Index Delta Threshold */
	uint8_t		wit;
	/* [한국어] Wear-Level Index Delta Threshold — chunk 평균 마모도 대비
	 * 개별 chunk가 얼마나 벗어나면 알림(WIT exceeded)을 발생시킬지 임계값.
	 * 와이어 위치: byte 32.
	 * 값 범위: 0~255 (vendor 정의 단위, 일반적으로 wli 차이의 절대값 임계).
	 * 설정자: 디바이스 펌웨어 또는 vendor 설정 — Set Features로 가변 가능.
	 * 읽는 자: 호스트 FTL이 chunk_notification의 wit_exceeded 플래그를 해석할 때
	 * 디바이스의 임계값을 함께 표시하기 위함.
	 * 동기화: readonly snapshot — Set Features 후 재조회 필요. */

	uint8_t		reserved3[31];
	/* [한국어] 예약(byte 33~63, 31바이트).
	 * 의미: wit과 num_grp(byte 64) 사이의 64바이트 정렬을 위한 padding 및
	 * spec 확장 영역.
	 * 값 범위: 디바이스 0 채움.
	 * 설정자: 디바이스(0). 읽는 자: 호스트는 무시.
	 * 동기화: 의미 없음. */

	/** Number of Groups */
	uint16_t	num_grp;
	/* [한국어] Group(상위 병렬 단위, 1.2의 channel) 총 개수.
	 * 와이어 위치: byte 64~65 (LE).
	 * 값 범위: 1~2^lbaf.grp_len. 디바이스 모델별 1~16 정도 일반적.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 grp 좌표 합법 범위
	 * 검증, GC 대상 PU 산출 시.
	 * 동기화: readonly. */

	/** Number of parallel units per group */
	uint16_t	num_pu;
	/* [한국어] 그룹당 PU(Parallel Unit, NAND LUN과 유사) 개수.
	 * 와이어 위치: byte 66~67. 값 범위: 1~2^lbaf.pu_len.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 동시 다발 쓰기를 위한
	 * PU stripe 폭 결정 — num_grp × num_pu = 총 병렬 채널 폭.
	 * 동기화: readonly. */

	/** Number of chunks per parallel unit */
	uint32_t	num_chk;
	/* [한국어] PU 하나당 chunk(=erase block) 개수.
	 * 와이어 위치: byte 68~71. 값 범위: 1~2^lbaf.chk_len.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 chunk allocation pool
	 * 크기, GC victim 선택 시 후보 집합 크기 산출에 사용.
	 * 동기화: readonly. */

	/** Chunk Size */
	uint32_t	clba;
	/* [한국어] chunk당 logical block 개수(섹터 단위).
	 * 와이어 위치: byte 72~75. 값 범위: 1~2^lbaf.lbk_len.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 chunk_information.wp가
	 * clba에 도달했는지(=chunk full) 검사할 때, 그리고 chunk 단위 sequential
	 * write 검증에 사용. 단 chunk_information_entry.ct.size_deviate=1인
	 * chunk는 entry.cnlb를 우선 신뢰.
	 * 동기화: readonly. */

	uint8_t		reserved4[52];
	/* [한국어] 예약(byte 76~127, 52바이트).
	 * 의미: clba와 ws_min 사이의 spec 확장 영역. 128바이트 정렬 padding.
	 * 값 범위: 디바이스 0 채움.
	 * 설정자: 디바이스(0). 읽는 자: 호스트 무시.
	 * 동기화: 의미 없음. */

	/** Minimum Write Size */
	uint32_t	ws_min;
	/* [한국어] Minimum Write Size — 한 번의 write 명령이 다뤄야 하는 최소
	 * sector 수. NAND의 page 정렬 요구를 반영.
	 * 와이어 위치: byte 128~131. 값 범위: 1 이상의 sector 수.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 write 분할 시 ws_min
	 * 배수로 정렬 — 위반 시 SPDK_OCSSD_SC_OUT_OF_ORDER_WRITE(0xF2) 가능.
	 * 동기화: readonly. */

	/** Optimal Write Size */
	uint32_t	ws_opt;
	/* [한국어] Optimal Write Size — 처리량 최적의 write 단위 sector 수.
	 * 와이어 위치: byte 132~135.
	 * 값 범위: ws_min의 배수 권장(예: ws_min=4 sec, ws_opt=32 sec).
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 streaming write의
	 * 청크 크기 선택 — ws_opt 미만은 throughput 손실 가능.
	 * 동기화: readonly. */

	/** Cache Minimum Write Size Units */
	uint32_t	mw_cunits;
	/* [한국어] Cache Minimum Write Size Units — 디바이스가 write를 latch에
	 * 잡고 있는 최소 cache 깊이(ws_min 단위). 읽기 직후 같은 영역을 읽으면
	 * latch에 있어 데이터가 반환될 수도 있으나, 그 이전에 mw_cunits 만큼
	 * 더 써야 안전하다는 디바이스 측 약속.
	 * 와이어 위치: byte 136~139.
	 * 값 범위: ws_min 단위의 정수.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL의 write-then-read
	 * 정합성 보장 정책 — 막 쓴 영역을 안전하게 read하려면 mw_cunits만큼의
	 * 후속 write가 선행되어야 함.
	 * 동기화: readonly. */

	/** Maximum Open Chunks */
	uint32_t	maxoc;
	/* [한국어] 디바이스 전체에서 동시에 open 상태로 둘 수 있는 chunk 최대 수.
	 * 와이어 위치: byte 140~143.
	 * 값 범위: 1~num_grp×num_pu×num_chk(상한은 사실상 이론값).
	 * 의미: maxoc를 초과해 open 시도 → 명령 실패.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 open chunk pool을
	 * maxoc로 캡.
	 * 동기화: readonly. */

	/** Maximum Open Chunks per PU */
	uint32_t	maxocpu;
	/* [한국어] 단일 PU 내에서 동시에 open 상태로 둘 수 있는 chunk 최대 수.
	 * 와이어 위치: byte 144~147.
	 * 값 범위: 1~num_chk.
	 * 의미: PU 단위 자원 제약(예: PU 내부 page latch 개수).
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 PU별 open 카운터를 추적.
	 * 동기화: readonly. */

	uint8_t		reserved5[44];
	/* [한국어] 예약(byte 148~191, 44바이트).
	 * 의미: 쓰기 제약 필드와 timing 필드 사이 spec 확장 placeholder.
	 * 값 범위: 디바이스 0 채움.
	 * 설정자: 디바이스(0). 읽는 자: 호스트 무시.
	 * 동기화: 의미 없음. */

	/** tRD Typical */
	uint32_t	trdt;
	/* [한국어] read 명령의 평균 소요 시간(ns 단위가 일반적, vendor 정의).
	 * 와이어 위치: byte 192~195.
	 * 값 범위: vendor가 정의한 시간 단위(보통 ns). 0이면 미보고.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL의 latency QoS 추정/스케줄링.
	 * 동기화: readonly. */

	/** tRD Max */
	uint32_t	trdm;
	/* [한국어] read 명령의 최악 소요 시간.
	 * 와이어 위치: byte 196~199.
	 * 값 범위: trdt 이상의 값(ns 단위).
	 * 의미: trdm 이내에 완료되지 않으면 timeout/abort 후보.
	 * 설정자: 디바이스. 읽는 자: 호스트 FTL의 timeout watchdog 산정.
	 * 동기화: readonly. */

	/** tWR Typical */
	uint32_t	twrt;
	/* [한국어] write 명령의 평균 소요 시간.
	 * 와이어 위치: byte 200~203.
	 * 값 범위: vendor 정의 시간 단위. NAND program 시간(수십~수백 us).
	 * 설정자: 디바이스. 읽는 자: 호스트 FTL의 write 큐잉 백프레셔 산정.
	 * 동기화: readonly. */

	/** tWR Max */
	uint32_t	twrm;
	/* [한국어] write 명령의 최악 소요 시간.
	 * 와이어 위치: byte 204~207.
	 * 값 범위: twrt 이상.
	 * 의미: twrm 초과 시 timeout 산정에 사용.
	 * 설정자: 디바이스. 읽는 자: 호스트 FTL.
	 * 동기화: readonly. */

	/** tCRS Typical */
	uint32_t	tcrst;
	/* [한국어] chunk reset(=erase) 명령의 평균 소요 시간.
	 * 와이어 위치: byte 208~211. NAND erase는 보통 수 ms 단위.
	 * 값 범위: vendor 정의 시간 단위.
	 * 설정자: 디바이스. 읽는 자: 호스트 FTL의 GC 스케줄링 비용 추정에 사용.
	 * 동기화: readonly. */

	/** tCRS Max */
	uint32_t	tcrsm;
	/* [한국어] chunk reset 최악 시간.
	 * 와이어 위치: byte 212~215.
	 * 값 범위: tcrst 이상.
	 * 의미: 디바이스 측 reset timeout 산정 — 초과 시 abort/재발급.
	 * 설정자: 디바이스. 읽는 자: 호스트 FTL.
	 * 동기화: readonly. */

	/** bytes 216-255: reserved for performance related metrics */
	uint8_t		reserved6[40];
	/* [한국어] 예약(byte 216~255, 40바이트).
	 * 의미: 향후 성능 관련 지표(예: temperature derating coefficient,
	 * background activity time 등)를 위한 placeholder.
	 * 값 범위: 디바이스 0 채움.
	 * 설정자: 디바이스(0). 읽는 자: 호스트 무시.
	 * 동기화: 의미 없음. */

	uint8_t		reserved7[3071 - 255];
	/* [한국어] byte 256~3071 영역(2816바이트). 광범위 예약.
	 * 와이어 위치: timing 메트릭 종료 후부터 vendor specific 영역 직전까지.
	 * 의미: spec 1.2/2.0에서 미사용 영역, 향후 메이저 확장을 위한 큰
	 * placeholder. 디바이스는 0으로 채운다.
	 * 값 범위: 디바이스 0. 호스트 파서는 이 영역을 건드리지 않는다.
	 * 설정자: 디바이스(0). 읽는 자: 호스트 무시.
	 * 동기화: 의미 없음.
	 * (배열 크기 산식: 3071-255 = 2816바이트.) */

	/** bytes 3072-4095: Vendor Specific */
	uint8_t		vs[4095 - 3071];
	/* [한국어] byte 3072~4095, 총 1024바이트의 Vendor Specific 영역.
	 * 와이어 위치: geometry 페이지의 마지막 1KB.
	 * 의미: 디바이스 제조사가 자체 진단/설정 정보를 노출하는 자유 공간.
	 * 값 범위: vendor가 자유 정의 — 호스트 FTL은 일반적으로 무시하며,
	 * vendor SDK가 별도 해석.
	 * 설정자: 디바이스 펌웨어. 읽는 자: vendor 전용 도구.
	 * 동기화: readonly snapshot.
	 * (배열 크기 산식: 4095-3071 = 1024바이트, 0~1023 인덱스로 1024개.) */
};
/* [한국어] geometry_data 구조체는 NVMe Identify 페이지 1개(4096바이트)를
 * 정확히 채워야 한다. 컴파일러 패딩으로 사이즈가 어긋나면 와이어 파싱이
 * 깨지므로 즉시 컴파일 실패.
 * 설정자: 본 헤더의 컴파일 타임 검증. 읽는 자: 빌드 시스템.
 * 값 범위: sizeof != 4096이면 _Static_assert 실패. 동기화: 컴파일 타임. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_ocssd_geometry_data) == 4096, "Incorrect size");

/*
 * [한국어]
 * struct spdk_ocssd_chunk_information_entry — Chunk Information Log Page
 * (LID = 0xCA, SPDK_OCSSD_LOG_CHUNK_INFO)의 단일 chunk 엔트리. 32바이트.
 *
 * 어디서 등장: 호스트가 NVMe Get Log Page(opcode 0x02)를 LID 0xCA로 발급하면
 * 디바이스가 num_grp × num_pu × num_chk 개의 본 엔트리를 평면 배열로 반환.
 * 호스트 FTL은 부팅 시 전체를 읽어 chunk state machine과 write pointer를
 * 복원하고, 이후 chunk_notification으로 증분 갱신 받는다.
 *
 * Chunk State Machine (cs 비트 + 명령 전이):
 *   FREE/EMPTY (cs.free=1) ── vector_write 첫 LBA → OPEN (cs.open=1, wp 증가)
 *   OPEN ── 추가 vector_write → wp 증가, wp == cnlb 시 CLOSED (cs.closed=1)
 *   OPEN/CLOSED ── vector_reset → FREE (wp=0)
 *   FREE/OPEN/CLOSED ── 결함 발견 → OFFLINE (cs.offline=1, 회복 불가)
 * cs는 mutually exclusive 4비트 중 정확히 한 개가 1.
 *
 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL.
 */
struct spdk_ocssd_chunk_information_entry {
	/** Chunk State */
	struct {
		/** if set to 1 chunk is free */
		uint8_t free		: 1;
		/* [한국어] cs.bit0(free) — chunk가 erase 직후 비어있는 상태.
		 * 와이어 위치: chunk info entry byte 0의 LSB.
		 * 의미: vector_write를 시작할 수 있는 후보. wp는 slba와 같음.
		 * 값 범위: 1=free, 0=다른 상태. closed/open/offline과 상호 배타.
		 * 설정자: 디바이스 펌웨어(reset 완료 시 또는 초기 상태).
		 * 읽는 자: 호스트 FTL이 free chunk pool에 추가.
		 * 동기화: 호스트 FTL은 chunk_notification으로 증분 갱신, 락 없이
		 * 자기 chunk 캐시를 갱신. */

		/** if set to 1 chunk is closed */
		uint8_t closed		: 1;
		/* [한국어] cs.bit1(closed) — chunk가 끝까지 쓰여진 상태.
		 * 와이어 위치: byte 0의 bit1.
		 * 의미: wp == slba+cnlb. 이후 read만 가능, write는
		 * SC_OUT_OF_ORDER_WRITE 또는 SC_WRITE_FAIL_CHUNK_EARLY_CLOSE 반환.
		 * 값 범위: 1=closed, 0=아님(상호 배타).
		 * 설정자: 디바이스 펌웨어(쓰기 완료 시 자동 전이, 또는 조기 종료).
		 * 읽는 자: 호스트 FTL이 read-only로 분류, GC victim 후보로 등록.
		 * 동기화: chunk 단위 — 호스트 FTL이 chunk별 락 또는 단일 thread
		 * 소유로 보호. */

		/** if set to 1 chunk is open */
		uint8_t open		: 1;
		/* [한국어] cs.bit2(open) — chunk에 일부 쓰기가 진행 중인 상태.
		 * 와이어 위치: byte 0의 bit2.
		 * 의미: slba ≤ wp < slba+cnlb. 추가 sequential write 가능.
		 * 값 범위: 1=open, 0=아님.
		 * 설정자: 디바이스 펌웨어(첫 vector_write로 free→open 전이).
		 * 읽는 자: 호스트 FTL이 active write streams 트래킹.
		 * 동기화: maxoc/maxocpu 한도 내에서 호스트 FTL이 카운터 유지. */

		/** if set to 1 chunk is offline */
		uint8_t offline		: 1;
		/* [한국어] cs.bit3(offline) — chunk가 결함으로 사용 불가.
		 * 와이어 위치: byte 0의 bit3.
		 * 의미: read/write 모두 거절. SC_OFFLINE_CHUNK(0xC0) 반환.
		 * 값 범위: 1=offline(영구), 0=아님. offline 진입 후에는 복구 불가.
		 * 설정자: 디바이스 펌웨어(grown bad block 검출 시).
		 * 읽는 자: 호스트 FTL이 bad block table에 등록, 사용 풀에서 제외.
		 * 동기화: 일방향 전이 — readonly 처리. */

		uint8_t reserved	: 4;
		/* [한국어] cs의 상위 4비트 예약(byte 0의 bit4~7).
		 * 의미: 추가 chunk 상태(예: erasing, draining) 확장을 위한 placeholder.
		 * 값 범위: 디바이스 0 채움.
		 * 설정자: 디바이스(0). 읽는 자: 호스트는 마스킹/무시.
		 * 동기화: readonly. */
	} cs;

	/** Chunk Type */
	struct {
		/** If set to 1 chunk must be written sequentially */
		uint8_t seq_write		: 1;
		/* [한국어] ct.bit0(seq_write) — 이 chunk는 sequential write만 허용.
		 * 와이어 위치: chunk info entry byte 1의 LSB.
		 * 의미: write LBA는 반드시 wp부터 1씩 증가. 위반 시
		 * SPDK_OCSSD_SC_OUT_OF_ORDER_WRITE(0xF2).
		 * 값 범위: 1=sequential 강제, 0=비활성.
		 * 설정자: 디바이스 펌웨어(NAND 특성상 거의 항상 1).
		 * 읽는 자: 호스트 FTL이 write 순서를 강제.
		 * 동기화: chunk 부팅 시 결정 — readonly. */

		/** If set to 1 chunk allows random writes */
		uint8_t rnd_write		: 1;
		/* [한국어] ct.bit1(rnd_write) — chunk 내부 random write 허용.
		 * 와이어 위치: byte 1의 bit1.
		 * 의미: 일부 SLC 캐시 영역이나 SCM 같은 미디어에서만 1.
		 * 값 범위: 1=random 허용, 0=불가. seq_write와 mutually exclusive.
		 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL의 write 정렬 정책 분기.
		 * 동기화: readonly. */

		uint8_t reserved1		: 2;
		/* [한국어] ct의 bit2~3 예약.
		 * 의미: 추가 chunk type(예: SLC/MLC/TLC 구분, write-once) 확장 여지.
		 * 값 범위: 디바이스 0. 호스트 무시.
		 * 설정자: 디바이스(0). 읽는 자: 호스트.
		 * 동기화: readonly. */

		/**
		 * If set to 1 chunk deviates from the chunk size reported
		 * in identify geometry command.
		 */
		uint8_t size_deviate		: 1;
		/* [한국어] ct.bit4(size_deviate) — 이 chunk의 cnlb가 geometry의
		 * clba와 다름을 표시.
		 * 와이어 위치: byte 1의 bit4.
		 * 의미: 1이면 entry.cnlb 필드를 우선 신뢰하고 geometry.clba를
		 * 사용하지 말 것(드물지만 vendor가 일부 chunk를 축소).
		 * 값 범위: 1=cnlb!=clba, 0=동일.
		 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL의 chunk 용량 계산기.
		 * 동기화: readonly. */

		uint8_t reserved2		: 3;
		/* [한국어] ct의 bit5~7 예약.
		 * 의미: 추가 type 비트 확장 placeholder.
		 * 값 범위: 디바이스 0. 호스트 무시.
		 * 설정자: 디바이스(0). 읽는 자: 호스트.
		 * 동기화: readonly. */
	} ct;

	/** Wear-level Index */
	uint8_t wli;
	/* [한국어] Wear-Level Index — 이 chunk의 상대 마모도 지수.
	 * 와이어 위치: chunk info entry byte 2.
	 * 값 범위: 0~255 (vendor 정의 스케일, 보통 erase 횟수 기반 정규화 값).
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 wear leveling 결정에 사용.
	 * geometry.wit과의 차이가 임계 초과면 chunk_notification.wit_exceeded
	 * 발생.
	 * 동기화: chunk_notification 또는 Get Log Page 폴링으로 갱신 — readonly. */

	uint8_t reserved[5];
	/* [한국어] byte 3~7 예약(5바이트).
	 * 의미: cs/ct/wli와 8바이트 정렬된 slba 사이의 padding.
	 * 값 범위: 디바이스 0 채움.
	 * 설정자: 디바이스(0). 읽는 자: 호스트 무시.
	 * 동기화: 의미 없음. */

	/** Starting LBA */
	uint64_t slba;
	/* [한국어] 이 chunk의 시작 LBA(PPA를 lbaf로 평면화한 값).
	 * 와이어 위치: byte 8~15 (LE).
	 * 의미: chunk가 다루는 LBA 구간 = [slba, slba+cnlb).
	 * 값 범위: lbaf로 인코딩된 64비트 LBA. grp/pu/chk 부분만 비영, lbk=0.
	 * 설정자: 디바이스 펌웨어(geometry로부터 결정적 산출).
	 * 읽는 자: 호스트 FTL이 vector_write/read의 LBA list를 빌드할 때 base
	 * 주소로 사용.
	 * 동기화: 부팅 후 readonly — chunk 자체가 사라지지 않는 한 변하지 않음. */

	/** Number of blocks in chunk */
	uint64_t cnlb;
	/* [한국어] chunk 내 logical block 총 개수(=용량).
	 * 와이어 위치: byte 16~23.
	 * 값 범위: 보통 geometry.clba와 동일, 단 ct.size_deviate=1이면 다를 수 있음.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 chunk full 판정
	 * (wp == slba+cnlb) 및 sequential write 상한 검증에 사용.
	 * 동기화: readonly. */

	/** Write Pointer */
	uint64_t wp;
	/* [한국어] 다음에 써야 할 LBA(slba 기준의 절대 LBA).
	 * 와이어 위치: byte 24~31.
	 * 의미: free chunk → wp = slba(즉 0 offset). open chunk → slba ≤
	 * wp < slba+cnlb. closed chunk → wp = slba+cnlb. offline → 의미 없음.
	 * 값 범위: [slba, slba+cnlb].
	 * 설정자: 디바이스 펌웨어(vector_write 성공 시 자동 증가, vector_reset
	 * 시 slba로 복귀).
	 * 읽는 자: 호스트 FTL이 sequential write 위치 추적, 재부팅 후 in-flight
	 * write 복구 시 안전한 시작점 결정.
	 * 동기화: 디바이스가 단일 truth source — 호스트는 Get Log Page 0xCA로
	 * 폴링하거나 chunk_notification으로 증분 동기화. */
};
/* [한국어] chunk_information_entry는 OCSSD spec 상 정확히 32바이트.
 * Get Log Page 0xCA 응답이 이 크기 단위 배열로 들어오므로 어긋나면
 * 인덱싱이 깨진다.
 * 설정자: 본 헤더의 컴파일 타임 검증. 읽는 자: 빌드 시스템.
 * 값 범위: sizeof != 32이면 _Static_assert 실패. 동기화: 컴파일 타임. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_ocssd_chunk_information_entry) == 32, "Incorrect size");

/*
 * [한국어]
 * struct spdk_ocssd_chunk_notification_entry — Chunk Notification Log Page
 * (LID = 0xD0, SPDK_OCSSD_LOG_CHUNK_NOTIFICATION)의 단일 통지 엔트리.
 * 64바이트.
 *
 * 어디서 등장: 디바이스가 chunk 상태 변화(에러율 변동, refresh, 마모도
 * 임계 초과, BAD/RECOVERY 전이)를 호스트에 통지할 때 NVMe AER(Async Event
 * Request) → Get Log Page 0xD0 시퀀스로 전달. 호스트 FTL은 nc(notification
 * counter)를 추적해 누락 없이 순차 처리한다.
 *
 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 chunk_information 캐시를
 * 갱신, bad block table을 수정.
 */
struct spdk_ocssd_chunk_notification_entry {

	/**
	 * This is a 64-bit incrementing notification count, indicating a
	 * unique identifier for this notification. The counter begins at 1h
	 * and is incremented for each unique event
	 */
	uint64_t		nc;
	/* [한국어] notification counter — 통지 고유 식별자.
	 * 와이어 위치: notification entry byte 0~7 (LE).
	 * 의미: 1부터 시작해 각 이벤트마다 +1. 호스트는 마지막으로 본 nc를
	 * 저장해두고 nc 갭이 생기면 통지 누락을 감지(드라이버 레벨 재폴링 트리거).
	 * 값 범위: 1~UINT64_MAX(0은 빈 슬롯/미사용을 의미).
	 * 설정자: 디바이스 펌웨어(이벤트 생성 시점에 단조 증가).
	 * 읽는 자: 호스트 FTL이 nc 워터마크 비교로 새 이벤트 식별.
	 * 동기화: 디바이스가 모든 발행을 직렬화 — 호스트는 Get Log Page 폴링
	 * 시 nc 단조성을 보고 read-after-read 정합성을 확보. */

	/** This field points to the chunk that has its state updated */
	uint64_t		lba;
	/* [한국어] 영향을 받은 chunk(또는 PU/단일 LBA)의 시작 LBA.
	 * 와이어 위치: byte 8~15.
	 * 의미: mask 필드와 결합해 단일 LBA(lblk=1) / chunk 전체(chunk=1) /
	 * PU 전체(pu=1)를 식별. 호스트 FTL은 lbaf로 디코드해 좌표를 얻는다.
	 * 값 범위: lbaf 인코딩 범위 내의 64비트 LBA.
	 * 설정자: 디바이스 펌웨어(이벤트 발생 위치 기준).
	 * 읽는 자: 호스트 FTL의 통지 라우터(grp/pu/chk 디코드 후 캐시 업데이트).
	 * 동기화: 이벤트 단위 readonly. */

	/**
	 * This field indicates the namespace id that the event is associated
	 * with
	 */
	uint32_t		nsid;
	/* [한국어] 이벤트가 속한 NVMe namespace ID.
	 * 와이어 위치: byte 16~19.
	 * 의미: 멀티 namespace 디바이스에서 어떤 namespace의 chunk 인지 구분.
	 * 값 범위: 1~num_ns(0과 0xFFFFFFFF은 통상 잘못된 통지로 호스트가 폐기).
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 namespace별 캐시로
	 * 라우팅 — namespace마다 별도 chunk_information 캐시를 둔다.
	 * 동기화: 이벤트 단위 readonly. */

	/** Field that indicate the state of the block */
	struct {

		/**
		 * If set to 1, then the error rate of the chunk has been
		 * changed to low
		 */
		uint8_t error_rate_low : 1;
		/* [한국어] state.bit0(error_rate_low) — chunk의 에러율이 LOW 등급으로
		 * 변경됨. 와이어 위치: notification entry byte 20의 LSB.
		 * 의미: 이전보다 양호해졌거나 유지. 호스트 FTL은 GC 우선도 하향.
		 * 값 범위: 1=LOW로 전이, 0=해당 이벤트 아님.
		 * 설정자: 디바이스 ECC 모니터. 읽는 자: 호스트 FTL.
		 * 동기화: 이벤트 단위 — 한 통지에서 여러 비트가 동시에 1일 수 있음. */

		/**
		 * If set to 1, then the error rate of the chunk has been
		 * changed to medium
		 */
		uint8_t error_rate_medium : 1;
		/* [한국어] state.bit1(error_rate_medium) — 에러율 MEDIUM.
		 * 와이어 위치: byte 20의 bit1.
		 * 의미: 주의 단계, 호스트 FTL이 데이터 마이그레이션 후보로 표시.
		 * 값 범위: 1=MEDIUM 전이, 0=아님.
		 * 설정자: 디바이스 ECC 모니터. 읽는 자: 호스트 FTL의 wear/health 모니터.
		 * 동기화: 이벤트 단위. */

		/**
		 * If set to 1, then the error rate of the chunk has been
		 * changed to high
		 */
		uint8_t error_rate_high : 1;
		/* [한국어] state.bit2(error_rate_high) — 에러율 HIGH.
		 * 와이어 위치: byte 20의 bit2.
		 * 의미: 즉시 데이터 이전 권장. 호스트 FTL이 read-only로 마킹 후 GC.
		 * 값 범위: 1=HIGH 전이, 0=아님.
		 * 설정자: 디바이스 ECC 모니터. 읽는 자: 호스트 FTL의 GC 트리거.
		 * 동기화: 이벤트 단위. */

		/**
		 * If set to 1, then the error rate of the chunk has been
		 * changed to unrecoverable
		 */
		uint8_t unrecoverable : 1;
		/* [한국어] state.bit3(unrecoverable) — 에러율 복구 불가 단계.
		 * 와이어 위치: byte 20의 bit3.
		 * 의미: chunk_information.cs.offline=1로 곧 전이. 호스트 FTL은
		 * 해당 chunk를 bad list에 등록, 매핑 무효화.
		 * 값 범위: 1=복구 불가, 0=아님.
		 * 설정자: 디바이스 ECC 모니터. 읽는 자: 호스트 FTL의 데이터 손실
		 * 회계.
		 * 동기화: 이벤트 단위 — 발생 후 chunk 상태는 사실상 영구. */

		/**
		 * If set to 1, then the chunk has been refreshed by the
		 * device
		 */
		uint8_t refreshed : 1;
		/* [한국어] state.bit4(refreshed) — 디바이스가 자체 read scrub로
		 * chunk 데이터를 다른 위치로 복사·재기록.
		 * 와이어 위치: byte 20의 bit4.
		 * 의미: 디바이스 측 retention 보강 동작 발생을 호스트에 통지.
		 * 값 범위: 1=refresh 수행됨, 0=아님.
		 * 설정자: 디바이스 background scrub. 읽는 자: 호스트 FTL의 통계.
		 * 동기화: 이벤트 단위. */

		uint8_t rsvd : 3;
		/* [한국어] state byte 20의 bit5~7 예약.
		 * 의미: 추가 미디어 이벤트(예: thermal throttle, weak read 등) 확장 여지.
		 * 값 범위: 디바이스 0. 호스트 무시.
		 * 설정자: 디바이스(0). 읽는 자: 호스트.
		 * 동기화: 의미 없음. */

		/**
		 * If set to 1 then the chunk's wear-level index is outside
		 * the average wear-level index threshold defined by the
		 * controller
		 */
		uint8_t wit_exceeded : 1;
		/* [한국어] state(byte 21).bit0(wit_exceeded) — 이 chunk의 WLI가
		 * 평균 대비 geometry.wit 임계를 초과.
		 * 와이어 위치: byte 21의 LSB.
		 * 의미: 호스트 FTL의 wear-leveling 정책이 cold→hot swap을 검토해야
		 * 함을 시사.
		 * 값 범위: 1=초과, 0=정상 범위.
		 * 설정자: 디바이스 wear monitor. 읽는 자: 호스트 FTL의 wear leveler.
		 * 동기화: 이벤트 단위. */

		uint8_t rsvd2 : 7;
		/* [한국어] state byte 21의 bit1~7 예약.
		 * 의미: 추가 wear/health 이벤트 확장 placeholder.
		 * 값 범위: 디바이스 0. 호스트 무시.
		 * 설정자: 디바이스(0). 읽는 자: 호스트.
		 * 동기화: 의미 없음. */
	} state;

	/**
	 * The address provided is covering either logical block, chunk, or
	 * parallel unit
	 */
	struct {

		/** If set to 1, the LBA covers the logical block */
		uint8_t lblk : 1;
		/* [한국어] mask.bit0(lblk) — lba 필드가 단일 LBA를 가리킴.
		 * 와이어 위치: byte 22의 LSB.
		 * 의미: 호스트 FTL은 정확히 한 LBA의 상태만 갱신. nlb 필드 유효.
		 * 값 범위: 1=단일 LBA 범위, 0=아님. chunk/pu와 mutually exclusive.
		 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL의 통지 디스패처.
		 * 동기화: 이벤트 단위. */

		/** If set to 1, the LBA covers the respecting chunk */
		uint8_t chunk : 1;
		/* [한국어] mask.bit1(chunk) — lba가 chunk 시작 LBA로 chunk 전체를 가리킴.
		 * 와이어 위치: byte 22의 bit1.
		 * 의미: chunk 전체 상태 갱신. nlb 무시.
		 * 값 범위: 1=chunk 단위, 0=아님.
		 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 chunk_information
		 * 항목 전체 갱신.
		 * 동기화: 이벤트 단위. */

		/**
		 * If set to 1, the LBA covers the respecting parallel unit
		 * including all chunks
		 */
		uint8_t pu : 1;
		/* [한국어] mask.bit2(pu) — lba가 PU 전체(=모든 chunk)를 가리킴.
		 * 와이어 위치: byte 22의 bit2.
		 * 의미: PU 단위 결함 — 호스트 FTL이 PU 내 모든 chunk를 일괄 처리.
		 * 값 범위: 1=PU 전체, 0=아님.
		 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL의 PU-wide 정책 핸들러.
		 * 동기화: 이벤트 단위. */

		uint8_t rsvd : 5;
		/* [한국어] mask byte 22의 bit3~7 예약.
		 * 의미: 추가 범위(예: group 단위, 디바이스 전체) 확장 placeholder.
		 * 값 범위: 디바이스 0. 호스트 무시.
		 * 설정자: 디바이스(0). 읽는 자: 호스트.
		 * 동기화: 의미 없음. */
	} mask;

	uint8_t			rsvd[9];
	/* [한국어] byte 23~31 예약(9바이트).
	 * 의미: mask와 nlb 사이의 padding 및 spec 확장 placeholder.
	 * nlb를 32바이트(0x20) 오프셋에 정렬시키기 위한 정렬 padding 역할.
	 * 값 범위: 디바이스 0 채움.
	 * 설정자: 디바이스(0). 읽는 자: 호스트 무시.
	 * 동기화: 의미 없음. */

	/**
	 * This field indicates the number of logical chunks to be written.
	 * This is a 0's based value. This field is only valid if mask bit 0 is
	 * set. The number of blocks addressed shall not be outside the boundary
	 * of the specified chunk.
	 */
	uint16_t		nlb;
	/* [한국어] 영향을 받는 logical block 수 (0's based, 즉 0=1블록).
	 * 와이어 위치: byte 32~33 (LE).
	 * 유효 조건: mask.lblk=1일 때만 유효. mask.chunk/pu일 때는 무시.
	 * 제약: lba + nlb는 해당 chunk 경계를 넘지 못함.
	 * 값 범위: 0~UINT16_MAX (실제 1~chunk_size).
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL의 LBA 범위 갱신기.
	 * 동기화: 이벤트 단위. */

	uint8_t			rsvd2[30];
	/* [한국어] byte 34~63 예약(30바이트).
	 * 의미: 64바이트 정렬을 맞추기 위한 패딩 + spec 확장 placeholder.
	 * 값 범위: 디바이스 0 채움.
	 * 설정자: 디바이스(0). 읽는 자: 호스트 무시.
	 * 동기화: 의미 없음. */
};
/* [한국어] chunk_notification_entry는 OCSSD spec 상 정확히 64바이트.
 * Get Log Page 0xD0의 응답 배열 인덱싱을 위해 컴파일 타임에 강제.
 * 설정자: 본 헤더의 컴파일 타임 검증. 읽는 자: 빌드 시스템.
 * 값 범위: sizeof != 64이면 _Static_assert 실패. 동기화: 컴파일 타임. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_ocssd_chunk_notification_entry) == 64, "Incorrect size");

/**
 * Vector completion queue entry
 */
/*
 * [한국어]
 * struct spdk_ocssd_vector_cpl — vector I/O 명령(opcode 0x90~0x93)의 CQE
 * 확장 포맷. NVMe 표준 CQE는 16바이트이고, OCSSD는 그 안에서 dword 0/1을
 * lba_status 비트맵으로 재정의해 한 번에 최대 64개의 LBA 각각의 성공/실패를
 * 표현한다.
 *
 * 와이어 매핑(NVMe CQE 16B):
 *   dword 0 (4B): lba_status 하위 32비트
 *   dword 1 (4B): lba_status 상위 32비트
 *   dword 2 (4B): SQ Head Pointer(sqhd, 16b) | SQ Identifier(sqid, 16b)
 *   dword 3 (4B): Command Identifier(cid, 16b) | Phase Tag + Status(status, 16b)
 *
 * 설정자: 디바이스 펌웨어. 읽는 자: SPDK NVMe 드라이버
 * (lib/nvme/nvme_qpair.c)가 CQE를 폴링해 본 구조로 캐스팅, 호스트 FTL의
 * 완료 콜백에 lba_status를 전달.
 */
struct spdk_ocssd_vector_cpl {
	/* dword 0,1 */
	uint64_t		lba_status;	/* completion status bit array */
	/* [한국어] vector 명령에 포함된 N개의 LBA(N ≤ 64) 각각에 대한 결과
	 * 비트맵. 와이어 위치: CQE dword 0+1 (8바이트 LE).
	 * 의미: bit i = (i번째 LBA가 실패면 1, 성공이면 0). 일부만 실패 시
	 * status.SC는 전체적으로 에러로 표시되지만 lba_status로 어떤 항목이
	 * 실패했는지 식별 가능.
	 * 값 범위: bit 0~63 (LBA 인덱스 동일). N < 64일 때 사용하지 않는 비트는 0.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL이 부분 실패 시 재시도
	 * 대상 LBA 추출.
	 * 동기화: in-place 파싱 — CQE ring slot 위에 직접 캐스팅. */

	/* dword 2 */
	uint16_t		sqhd;	/* submission queue head pointer */
	/* [한국어] 디바이스가 알려주는 SQ의 head 포인터 — 호스트의 SQ tail과
	 * 비교해 현재 SQ 점유율을 계산.
	 * 와이어 위치: CQE dword 2의 [15:0].
	 * 의미: 디바이스가 처리해 free화한 슬롯 인덱스. 표준 NVMe와 동일.
	 * 값 범위: 0~SQ size-1.
	 * 설정자: 디바이스 펌웨어. 읽는 자: SPDK NVMe 드라이버의 SQ 백프레셔
	 * (lib/nvme/nvme_qpair.c).
	 * 동기화: per-qpair, polled-mode에서 단일 reactor thread가 소유. */

	uint16_t		sqid;	/* submission queue identifier */
	/* [한국어] 이 완료가 속한 Submission Queue ID.
	 * 와이어 위치: CQE dword 2의 [31:16].
	 * 의미: I/O qpair가 여러 개일 때 완료를 올바른 SQ에 연결.
	 * 값 범위: 0(admin) ~ NVMe controller의 max QID.
	 * 설정자: 디바이스 펌웨어. 읽는 자: SPDK NVMe 드라이버의 qpair 라우터.
	 * 동기화: 디바이스가 정확히 발급된 SQ의 ID를 echo. */

	/* dword 3 */
	uint16_t		cid;	/* command identifier */
	/* [한국어] 명령 식별자 — 호스트가 SQE에 넣은 cdw0의 cid를 그대로 반환.
	 * 와이어 위치: CQE dword 3의 [15:0].
	 * 의미: in-flight 트래커에서 nvme_request*를 cid로 키 조회해 콜백 호출.
	 * 값 범위: 0~UINT16_MAX (호스트가 할당, 디바이스는 변경 없이 echo).
	 * 설정자: 디바이스(SQE.cid 그대로 echo). 읽는 자: SPDK 드라이버.
	 * 동기화: per-qpair tracker는 단일 thread 소유 — 락 없이 조회. */

	struct spdk_nvme_status	status;
	/* [한국어] 표준 NVMe status 필드(16비트, phase + SCT + SC 등).
	 * 와이어 위치: CQE dword 3의 [31:16].
	 * 의미: 명령 전체의 성공/실패 코드. lba_status가 0이면 모든 항목 성공,
	 * 0 아니면 SCT=Media error(2h) + SC=SPDK_OCSSD_SC_*(0xC0/0xC1/0xD0/0xF0~0xF2)
	 * 같은 OCSSD 확장 미디어 에러 코드 가능.
	 * 값 범위: spdk_nvme_status 비트 레이아웃 — phase 1bit, SC 8bit,
	 * SCT 3bit, CRD 2bit, M 1bit, DNR 1bit.
	 * 설정자: 디바이스 펌웨어. 읽는 자: SPDK 드라이버 + 호스트 FTL.
	 * 동기화: phase tag 토글로 호스트 polling이 새 CQE 도착을 감지하는
	 * 메커니즘 — 마지막에 기록되어야 하므로 디바이스 측에서 store ordering
	 * 보장 필수. */
};
/* [한국어] vector CQE는 NVMe 표준 CQE와 동일한 16바이트. CQ ring slot 크기와
 * 일치해야 polling 인덱싱이 정상 동작.
 * 설정자: 본 헤더의 컴파일 타임 검증. 읽는 자: 빌드 시스템.
 * 값 범위: sizeof != 16이면 _Static_assert 실패. 동기화: 컴파일 타임. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_ocssd_vector_cpl) == 16, "Incorrect size");

/**
 * OCSSD admin command set opcodes
 */
/*
 * [한국어]
 * enum spdk_ocssd_admin_opcode — Admin Submission Queue(SQ 0)에서만 발급
 * 가능한 OCSSD 전용 admin opcode. NVMe 1.x admin opcode 공간(0x00~0xBF는
 * standard, 0xC0~0xFF는 vendor-specific)에서 0xE2를 차지.
 * 와이어 위치: Admin SQE.cdw0의 OPC(8비트).
 * 설정자: 호스트(spdk_nvme_ocssd_ctrlr_cmd_geometry). 읽는 자: 디바이스 펌웨어.
 */
enum spdk_ocssd_admin_opcode {
	SPDK_OCSSD_OPC_GEOMETRY	= 0xE2
	/* [한국어] Geometry Identify — 디바이스의 OCSSD 기하 정보를 4096바이트
	 * 페이지로 가져오는 admin 명령.
	 * 와이어 위치: Admin SQE.opc=0xE2, NSID=대상 namespace, PRP1/PRP2=4KB
	 * 응답 버퍼. 결과는 spdk_ocssd_geometry_data로 캐스팅하여 해석.
	 * 값 범위: 0xE2 고정(NVMe vendor admin 영역).
	 * 설정자: 호스트가 SQE 빌드 시 .opc=0xE2 기록.
	 * 발급 주체: lib/nvme/nvme_ctrlr_ocssd_cmd.c의
	 * spdk_nvme_ocssd_ctrlr_cmd_geometry().
	 * 읽는 자: 디바이스 펌웨어가 admin SQ에서 dispatch.
	 * 동기화: admin SQ는 controller당 1개로 단일 thread가 소유. */
};

/**
 * OCSSD I/O command set opcodes
 */
/*
 * [한국어]
 * enum spdk_ocssd_io_opcode — I/O Submission Queue에서 발급하는 vector
 * 명령의 opcode. NVMe I/O opcode 공간 중 OCSSD vendor 영역(0x80~0xFF) 사용.
 *
 * vector I/O cdw 인코딩(공통):
 *   cdw0    : OPC(아래 enum), CID, FUSE, PSDT
 *   cdw10/11: 64비트 LBA list 포인터(또는 단일 LBA — 단일이면 PRP 없이 cdw에 직접)
 *   cdw12   : NLB(Number of Logical Blocks, 0's based, [5:0]) | RSVD |
 *             SPDK_OCSSD_IO_FLAGS_LIMITED_RETRY(bit31) 등의 플래그
 *   cdw13   : DSM 힌트(write/read 시 미디어 패턴 힌트)
 *   cdw14/15: PPA list metadata pointer(write 시 OOB 메타데이터 영역)
 *
 * 설정자: 호스트(lib/nvme/nvme_ns_ocssd_cmd.c). 읽는 자: 디바이스.
 */
enum spdk_ocssd_io_opcode {
	SPDK_OCSSD_OPC_VECTOR_RESET	= 0x90,
	/* [한국어] Vector Chunk Reset — 다수 chunk를 한 번에 erase(=free 전이).
	 * 와이어: I/O SQE.opc=0x90, cdw10/11=PPA list, cdw12.NLB=리셋 chunk 수-1.
	 * 효과: 대상 chunk의 cs를 free=1, wp=slba로 초기화. 동시에 여러 PU에
	 * 분산된 chunk를 묶어 발급함으로써 erase 병렬화.
	 * 값 범위: 0x90 고정(NVMe vendor I/O 영역).
	 * 에러: free 상태에서 mccap.multi_reset=0인데 발급 시 SC_INVALID_RESET(0xC1).
	 * 설정자: 호스트가 SQE.opc=0x90 기록.
	 * 발급 주체: lib/nvme/nvme_ns_ocssd_cmd.c의
	 * spdk_nvme_ocssd_ns_cmd_vector_reset(). 읽는 자: 디바이스 펌웨어.
	 * 동기화: 발급 qpair에 고정된 단일 thread가 소유. */

	SPDK_OCSSD_OPC_VECTOR_WRITE	= 0x91,
	/* [한국어] Vector Chunk Write — 다수 LBA에 동시에 데이터 기록.
	 * 와이어: I/O SQE.opc=0x91, cdw10/11=PPA list, PRP1/PRP2=write data,
	 * cdw14/15=metadata, cdw12.NLB=쓸 LBA 수-1.
	 * 값 범위: 0x91 고정.
	 * 제약: 각 chunk 내 sequential(wp부터). 위반 시 SC_OUT_OF_ORDER_WRITE(0xF2).
	 * NLB는 ws_min 배수여야 안전(권장 ws_opt 배수).
	 * 부분 실패: lba_status 비트맵에서 식별 — 호스트 FTL이 SC_WRITE_FAIL_*
	 * 처리.
	 * 설정자: 호스트(SQE.opc=0x91). 읽는 자: 디바이스 펌웨어.
	 * 발급 주체: spdk_nvme_ocssd_ns_cmd_vector_write().
	 * 동기화: 발급 qpair에 고정된 단일 thread. */

	SPDK_OCSSD_OPC_VECTOR_READ	= 0x92,
	/* [한국어] Vector Chunk Read — 다수 LBA에서 동시에 데이터 읽기.
	 * 와이어: I/O SQE.opc=0x92, cdw10/11=PPA list, PRP1/PRP2=read 버퍼.
	 * 값 범위: 0x92 고정.
	 * 안정 조건: closed chunk는 자유 read, open chunk는 wp 미만 영역만,
	 * 단 mw_cunits 이내 갓 쓴 영역은 read 결과가 indeterminate.
	 * 경고: SC_READ_HIGH_ECC(0xD0)는 데이터 retention 한계 근접 힌트.
	 * 설정자: 호스트(SQE.opc=0x92). 읽는 자: 디바이스 펌웨어.
	 * 발급 주체: spdk_nvme_ocssd_ns_cmd_vector_read().
	 * 동기화: 발급 qpair에 고정된 단일 thread. */

	SPDK_OCSSD_OPC_VECTOR_COPY	= 0x93
	/* [한국어] Vector Chunk Copy — 디바이스 내부에서 source LBA들의 데이터를
	 * destination LBA들로 직접 복사(host DRAM 경유 X).
	 * 와이어: I/O SQE.opc=0x93, cdw10/11=dest PPA list, cdw14/15=src PPA list,
	 * cdw12.NLB=LBA 수-1.
	 * 값 범위: 0x93 고정.
	 * 가용 조건: geometry.mccap.vec_chk_cpy=1일 때만. GC를 호스트 메모리
	 * 트래픽 없이 가속(PCIe 대역폭 절약).
	 * 설정자: 호스트(SQE.opc=0x93). 읽는 자: 디바이스 펌웨어.
	 * 발급 주체: spdk_nvme_ocssd_ns_cmd_vector_copy().
	 * 동기화: 발급 qpair에 고정된 단일 thread. */
};

/**
 * Log page identifiers for SPDK_NVME_OPC_GET_LOG_PAGE
 */
/*
 * [한국어]
 * enum spdk_ocssd_log_page — NVMe Get Log Page(opcode 0x02) admin 명령에
 * 넘기는 LID(Log Identifier). OCSSD vendor 영역(0xC0~0xFF) 사용.
 * 호스트는 cdw10[15:0]=LID, cdw10[31:16]=NUMDL(=size_in_dwords-1),
 * cdw11=NUMDU 형태로 길이를 지정해 응답을 받는다.
 */
enum spdk_ocssd_log_page {
	/** Chunk Information */
	SPDK_OCSSD_LOG_CHUNK_INFO		= 0xCA,
	/* [한국어] Chunk Information Log Page (LID=0xCA).
	 * 와이어: Admin Get Log Page SQE.cdw10의 [15:0] LID에 0xCA, 응답은
	 * spdk_ocssd_chunk_information_entry × (num_grp×num_pu×num_chk) 배열.
	 * 값 범위: 0xCA 고정(NVMe vendor LID 영역).
	 * 용도: 호스트 FTL이 부팅 시 모든 chunk의 cs/wp/wli/cnlb 일괄 조회.
	 * 응답이 매우 클 수 있어 다회 분할 발급(NUMDL/NUMDU + LPOL 페이징).
	 * 설정자: 호스트(SQE.cdw10). 읽는 자: 디바이스 펌웨어.
	 * 발급 주체: spdk_nvme_ctrlr_cmd_get_log_page() + 본 LID.
	 * 동기화: admin SQ — 단일 thread 소유. */

	/** Chunk Notification Log */
	SPDK_OCSSD_LOG_CHUNK_NOTIFICATION	= 0xD0,
	/* [한국어] Chunk Notification Log Page (LID=0xD0).
	 * 와이어: Admin Get Log Page SQE.cdw10의 LID에 0xD0, 응답은
	 * spdk_ocssd_chunk_notification_entry × N 배열.
	 * 값 범위: 0xD0 고정.
	 * 트리거: 디바이스가 AER(Asynchronous Event Request)로 통지 → 호스트가
	 * 0xD0 폴링하여 nc 갭이 없게 누적 수신.
	 * 용도: chunk 상태 변동(에러율, refresh, BAD/RECOVERY, WIT 초과) 증분 갱신.
	 * 설정자: 호스트(SQE.cdw10). 읽는 자: 디바이스 펌웨어.
	 * 동기화: admin SQ — 단일 thread 소유. */
};

/**
 * OCSSD feature identifiers
 * Defines OCSSD specific features that may be configured with Set Features and
 * retrieved with Get Features.
 */
/*
 * [한국어]
 * enum spdk_ocssd_feat — NVMe Set Features(0x09) / Get Features(0x0A)에서
 * 사용하는 FID(Feature Identifier). OCSSD vendor 영역(0xC0~0xFF) 사용.
 */
enum spdk_ocssd_feat {
	/**  Media Feedback feature identifier */
	SPDK_OCSSD_FEAT_MEDIA_FEEDBACK	= 0xCA
	/* [한국어] Media Feedback feature (FID=0xCA).
	 * 와이어: Admin Set Features(0x09)/Get Features(0x0A) SQE.cdw10의 [7:0]
	 * FID에 0xCA. cdw11은 토글할 이벤트 카테고리 비트맵.
	 * 값 범위: 0xCA 고정.
	 * 의미: chunk_notification에서 어떤 종류의 미디어 이벤트(에러율 변동,
	 * WIT 초과 등)를 받을지 호스트가 활성/비활성 토글.
	 * 설정자: 호스트 FTL의 초기화 단계 — Set Features로 1회 활성화.
	 * 읽는 자: 디바이스 펌웨어가 어떤 통지를 발행할지 결정.
	 * 동기화: admin SQ — 단일 thread, controller-wide 영구 설정. */
};

/**
 * OCSSD media error status codes extension.
 * Additional error codes for status code type “2h” (media errors)
 */
/*
 * [한국어]
 * enum spdk_ocssd_media_error_status_code — NVMe CQE.status.SC 필드에 들어가는
 * OCSSD 확장 media error 코드. Status Code Type(SCT) = 2h(Media and Data
 * Integrity Errors)와 결합되어 사용. CQE 파싱 시 SCT==2 && SC==0xC0..0xF2
 * 범위면 OCSSD-specific 에러로 분기 처리한다.
 *
 * 와이어 위치: spdk_nvme_status.SC([7:0]) — CQE dword 3 [25:17].
 * 설정자: 디바이스. 읽는 자: SPDK NVMe 드라이버 + 호스트 FTL.
 */
enum spdk_ocssd_media_error_status_code {
	/**
	 * The chunk was either marked offline by the reset or the state
	 * of the chunk is already offline.
	 */
	SPDK_OCSSD_SC_OFFLINE_CHUNK			= 0xC0,
	/* [한국어] SC=0xC0, OFFLINE_CHUNK — vector_reset 시도가 chunk를
	 * offline으로 마킹하거나, 이미 offline인 chunk에 대한 명령을 발급한 경우.
	 * 와이어 위치: CQE.status.SC, SCT=2(Media error)와 결합.
	 * 값 범위: 0xC0 고정.
	 * 처리: 호스트 FTL이 해당 chunk를 bad list에 등록, 매핑에서 제외.
	 * 트리거 명령: vector_reset/write/read 모두 가능.
	 * 설정자: 디바이스 펌웨어. 읽는 자: SPDK 드라이버 + 호스트 FTL. */

	/**
	 * Invalid reset if chunk state is either “Free” or “Open”
	 */
	SPDK_OCSSD_SC_INVALID_RESET			= 0xC1,
	/* [한국어] SC=0xC1, INVALID_RESET — free 또는 open 상태 chunk에 대한
	 * vector_reset이 mccap.multi_reset=0 디바이스에서 거절됨.
	 * 와이어 위치: CQE.status.SC.
	 * 값 범위: 0xC1 고정.
	 * 처리: 호스트 FTL이 chunk가 closed가 될 때까지 reset 보류 또는
	 * multi_reset 지원 여부에 따라 다른 GC 정책 채택.
	 * 트리거 명령: SPDK_OCSSD_OPC_VECTOR_RESET.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL. */

	/**
	 * Write failed, chunk remains open.
	 * Host should proceed to write to next write unit.
	 */
	SPDK_OCSSD_SC_WRITE_FAIL_WRITE_NEXT_UNIT	= 0xF0,
	/* [한국어] SC=0xF0, WRITE_FAIL_WRITE_NEXT_UNIT — 특정 LBA의 write가
	 * 실패했지만 chunk는 여전히 open. 다음 write unit(=ws_min 정렬) 위치로
	 * skip하여 진행 가능.
	 * 와이어 위치: CQE.status.SC.
	 * 값 범위: 0xF0 고정.
	 * 처리: 호스트 FTL이 실패한 LBA를 stripe에서 제거하고 wp가 자동 전진했음을
	 * 가정한 채 다음 ws_min 위치로 write 재시도.
	 * 트리거 명령: SPDK_OCSSD_OPC_VECTOR_WRITE.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL의 write retry 로직. */

	/**
	 * The writes ended prematurely. The chunk state is set to closed.
	 * The host can read up to the value of the write pointer.
	 */
	SPDK_OCSSD_SC_WRITE_FAIL_CHUNK_EARLY_CLOSE	= 0xF1,
	/* [한국어] SC=0xF1, WRITE_FAIL_CHUNK_EARLY_CLOSE — write 실패로 chunk가
	 * 강제 closed 처리됨. wp까지의 데이터는 read 가능, 그 이후는 정의되지 않음.
	 * 와이어 위치: CQE.status.SC.
	 * 값 범위: 0xF1 고정.
	 * 처리: 호스트 FTL이 chunk_information을 다시 조회해 새로운 wp/cnlb를
	 * 캐시에 반영, 미사용 영역은 손실로 회계 처리.
	 * 트리거 명령: SPDK_OCSSD_OPC_VECTOR_WRITE.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL의 chunk lifecycle 관리. */

	/**
	 * The write corresponds to a write out of order within an open
	 * chunk or the write is to a closed or offline chunk.
	 */
	SPDK_OCSSD_SC_OUT_OF_ORDER_WRITE		= 0xF2,
	/* [한국어] SC=0xF2, OUT_OF_ORDER_WRITE — open chunk에 wp가 아닌 위치를
	 * 쓰거나 closed/offline chunk에 write를 시도.
	 * 와이어 위치: CQE.status.SC.
	 * 값 범위: 0xF2 고정.
	 * 처리: 호스트 FTL의 write 순서 버그 진단. wp를 재조회 후 정상 위치로
	 * 재시작. seq_write=1 chunk에서 가장 흔한 사용자 측 위반.
	 * 트리거 명령: SPDK_OCSSD_OPC_VECTOR_WRITE.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL — assertion 또는 panic 후보. */

	/**
	 * The data retrieved is nearing its limit for reading.
	 * The limit is vendor specific, and only provides a hint
	 * to the host that should refresh its data in the future.
	 */
	SPDK_OCSSD_SC_READ_HIGH_ECC			= 0xD0,
	/* [한국어] SC=0xD0, READ_HIGH_ECC — 읽기 자체는 성공했지만 ECC가 한계
	 * 근접. 데이터는 정상이나 retention이 위태롭다는 힌트.
	 * 와이어 위치: CQE.status.SC.
	 * 값 범위: 0xD0 고정.
	 * 처리: 호스트 FTL이 백그라운드에서 해당 chunk를 새 chunk로 마이그레이션
	 * (host-side refresh). 실패가 아니므로 사용자 read는 성공 반환.
	 * 트리거 명령: SPDK_OCSSD_OPC_VECTOR_READ.
	 * 설정자: 디바이스 펌웨어. 읽는 자: 호스트 FTL의 health 모니터. */
};

/* [한국어] SPDK_OCSSD_IO_FLAGS_LIMITED_RETRY — vector I/O 명령(opcode 0x91/
 * 0x92/0x93)의 cdw12 bit31에 해당하는 limited retry 플래그.
 * 와이어 위치: I/O SQE.cdw12의 최상위 비트(1U << 31).
 * 값 범위: 0 또는 (1U<<31). cdw12의 다른 비트들은 NLB 등으로 사용되므로
 * OR 연산으로 결합한다.
 * 의미: 1=디바이스가 내부 재시도를 최소한으로만 수행하고 빠르게 실패 반환,
 * 0=디바이스 기본 retry 정책 사용.
 * 용도: latency-critical 경로(예: real-time read)에서 호스트가 직접 재시도
 * 정책을 통제하려 할 때 1로 설정. NVMe 표준 LR 비트와 의미적으로 동일.
 * 설정자: lib/nvme/nvme_ns_ocssd_cmd.c가 io_flags 인자를 받아 cdw12에 OR
 * 형태로 합성.
 * 읽는 자: 디바이스 펌웨어 — retry 횟수 결정.
 * 동기화: 컴파일 타임 상수. */
#define SPDK_OCSSD_IO_FLAGS_LIMITED_RETRY (1U << 31)

#ifdef __cplusplus
/* [한국어] C++ 호환을 위한 extern "C" 블록 종료.
 * 설정자/읽는 자: 컴파일러 전처리기. 동기화: 컴파일 타임. */
}
#endif

/* [한국어] include guard 종료(SPDK_NVME_OCSSD_SPEC_H).
 * 두 번째 include 시 본문 전체가 스킵되도록 하는 마무리 지점.
 * 설정자/읽는 자: 컴파일러 전처리기. 동기화: 컴파일 타임. */
#endif
