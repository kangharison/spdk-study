/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] NVMe over Fabrics 와이어 포맷 정의 헤더 (nvmf_spec.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVMe over Fabrics (NVMe-oF) 1.x / 2.0 사양에서 정의하는 모든 와이어
 * 레벨 자료구조와 상수를 C 구조체/enum/매크로로 그대로 노출하는 "스펙 미러" 헤더이다.
 * Fabrics 명령(opcode 0x7F) 의 SQE/CQE 레이아웃, Property Get/Set 의 가상 레지스터
 * 접근 형식, Connect 캡슐의 hostid/hostnqn/subnqn 페이로드, Discovery Log Page
 * 엔트리, RDMA private data, TCP PDU(ICReq/ICResp/CapsuleCmd/H2CData/R2T 등),
 * DH-CHAP 인증 메시지 포맷을 모두 포함한다. 이 헤더는 코드 한 줄도 실행되지 않으며
 * 오직 메모리/네트워크 상의 바이트 배치(byte layout)만 정의한다.
 * `#pragma pack(push, 1)` 로 패킹하여 와이어 포맷과 1:1 매핑되도록 보장하고,
 * 모든 구조체는 `SPDK_STATIC_ASSERT(sizeof(...) == N)` 으로 컴파일 타임에
 * 사이즈가 스펙과 일치하는지 검증한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe-oF 스택은 호스트 측(lib/nvme/nvme_rdma.c, lib/nvme/nvme_tcp.c)과
 * 타깃 측(lib/nvmf/rdma.c, lib/nvmf/tcp.c) 양쪽에서 동일한 와이어 포맷을 송수신해야
 * 하며, 그 공통 어휘를 이 헤더가 제공한다. 호출 체인은 다음과 같다:
 *   [Host App]
 *     → spdk_nvme_connect() / spdk_nvme_ns_cmd_read() ...
 *     → lib/nvme/nvme_rdma.c|nvme_tcp.c (이 헤더의 구조체로 SQE/PDU 직렬화)
 *     → [Network: RDMA SEND/WRITE 또는 TCP socket write]
 *     → lib/nvmf/rdma.c|tcp.c (동일 헤더로 역직렬화)
 *     → lib/nvmf/ctrlr.c (Property Get/Set, Connect, Identify 처리)
 *     → bdev → NVMe SSD
 * 즉 이 헤더는 NVMe-oF 의 "프로토콜 어휘"이며, 상위 모든 모듈이 의존하는
 * 가장 낮은 레이어이다. NVMe Base 스펙(nvme_spec.h) 위에 Fabrics 확장(opcode 0x7F)
 * 만 추가하므로, nvme_spec.h 와 함께 읽어야 전체 그림이 완성된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존하는 헤더: <spdk/stdinc.h>, <spdk/assert.h>, <spdk/nvme_spec.h>
 *   (nvme_spec.h 에서 spdk_nvme_cmd, spdk_nvme_cpl, spdk_nvme_sgl_descriptor,
 *    SPDK_NVME_NQN_FIELD_SIZE 등을 가져온다)
 * - 이 헤더에 의존하는 모듈:
 *     • lib/nvme/nvme_fabric.c        — Property Get/Set, Connect, Discovery 송신
 *     • lib/nvme/nvme_rdma.c          — RDMA capsule (SQE + In-Capsule Data + SGL)
 *     • lib/nvme/nvme_tcp.c           — TCP PDU 송수신 (ICReq → CapsuleCmd → ...)
 *     • lib/nvmf/ctrlr.c, ctrlr_discovery.c — 타깃 측 Fabrics 명령 디스패치
 *     • lib/nvmf/rdma.c, tcp.c        — 트랜스포트 백엔드
 *     • lib/nvmf/auth.c               — DH-CHAP 인증 메시지 처리
 * - 데이터 흐름: 호스트 유저 코드가 NVMe 명령 발행 → 호스트 트랜스포트가 이 헤더의
 *   구조체에 채워서 전송 → 타깃 트랜스포트가 동일 헤더로 디코드 → ctrlr 코드가
 *   fctype 별로 분기하여 처리.
 * - 공유 핵심 자료구조:
 *     • spdk_nvmf_capsule_cmd        — 64B Fabrics SQE 의 일반형
 *     • spdk_nvmf_fabric_connect_*  — Connect 핸드셰이크
 *     • spdk_nvmf_discovery_log_page* — Discovery Controller 가 반환
 *     • spdk_nvme_tcp_*_hdr          — NVMe/TCP PDU 군
 *
 * === 주요 함수/구조체 요약 ===
 * 본 헤더에는 함수 정의가 없다 (순수 데이터 정의). 핵심 자료구조는 다음과 같다:
 *   • struct spdk_nvmf_capsule_cmd (64B)            — Fabrics 명령 일반형 (opcode=0x7F)
 *   • enum  spdk_nvmf_fabric_cmd_types              — fctype: Property Set/Get, Connect,
 *                                                     AUTH Send/Recv 등
 *   • struct spdk_nvmf_fabric_prop_{get,set}_cmd    — 가상 레지스터(R/W) — Fabrics 는
 *                                                     PCIe MMIO 가 없으므로 이 명령으로
 *                                                     CAP/VS/CC/CSTS 등을 접근
 *   • struct spdk_nvmf_fabric_connect_{cmd,data,rsp}— 컨트롤러 연결 (hostid/hostnqn/subnqn)
 *   • struct spdk_nvmf_discovery_log_page[_entry]   — LID 0x70 응답 (서브시스템 카탈로그)
 *   • struct spdk_nvmf_rdma_*_private_data          — RDMA CM private data (rdma_cm)
 *   • struct spdk_nvme_tcp_common_pdu_hdr 외 PDU 군 — NVMe/TCP 트랜스포트 PDU
 *   • struct spdk_nvmf_dhchap_{challenge,reply,...} — DH-CHAP 인증 메시지 (NVMe-oF 2.0)
 */

#ifndef SPDK_NVMF_SPEC_H
/* [한국어] 헤더 가드 시작 — 다중 #include 시 중복 정의 방지.
 * 이 헤더는 host(lib/nvme)와 target(lib/nvmf) 양쪽 모든 트랜스포트 .c 파일에
 * 포함되므로 가드 누락 시 빌드가 깨진다. */
#define SPDK_NVMF_SPEC_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 인클루드 묶음 — uint8_t/uint16_t/uint32_t/uint64_t 등
 * 정수 타입(stdint.h)과 size_t/offsetof 등(stddef.h) 사용에 필요.
 * 이 헤더의 모든 구조체 필드는 정확한 비트 폭이 보장되어야 와이어 포맷이
 * 일치하므로 fixed-width 타입이 필수. */

#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로 제공 — 컴파일 타임에 sizeof(struct)/
 * offsetof(필드) 가 NVMe-oF 스펙과 일치하는지 검증한다. 와이어 포맷이 단 1바이트라도
 * 틀어지면 호스트-타깃 간 호환성이 깨지므로 정적 검증은 필수. */

#include "spdk/nvme_spec.h"
/* [한국어] NVMe Base 스펙 정의 — 본 헤더는 Base 위에 Fabrics 확장만 추가하므로
 * spdk_nvme_cmd(64B SQE), spdk_nvme_cpl(16B CQE), spdk_nvme_sgl_descriptor(16B SGL),
 * spdk_nvme_status, SPDK_NVME_NQN_FIELD_SIZE(=223+1) 등을 재사용한다. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 에서 인클루드될 때 C 링키지 강제 — SPDK 일부 컴포넌트(테스트, 외부
 * 바인딩)가 C++ 로 컴파일될 수 있어 name mangling 방지. */
#endif

/**
 * \file
 * NVMe over Fabrics specification definitions
 */

#pragma pack(push, 1)
/* [한국어] 구조체 패킹을 1바이트 경계로 고정 — NVMe-oF 와이어 포맷은 자연 정렬
 * 패딩 없이 빽빽하게 직렬화되므로, 컴파일러 기본 정렬(보통 8B)을 사용하면 sizeof
 * 가 스펙과 어긋난다. push/pop 으로 본 헤더 영역에서만 적용하고 끝에서 복원.
 * 이 한 줄이 빠지면 SPDK_STATIC_ASSERT 들이 모두 실패한다. */

/* [한국어] === Fabrics 일반 캡슐 명령 (Generic Capsule SQE) ===
 * NVMe-oF 1.x §6.1: 모든 Fabrics 명령(opcode 0x7F)의 SQE 64바이트 일반 형식.
 * 트랜스포트(RDMA/TCP)는 이 64B 를 "command capsule" 의 헤더로 사용하며, 뒤에
 * In-Capsule Data 가 따라올 수 있다(RDMA: SGL inline / TCP: PDU body). */
struct spdk_nvmf_capsule_cmd {
	uint8_t		opcode;
	/* [한국어] opcode (와이어 offset 0, 1B) — 항상 0x7F (SPDK_NVME_OPC_FABRIC).
	 * Fabrics 명령임을 식별하는 1차 키. 값이 0x7F 가 아니면 일반 NVM/Admin 명령
	 * 으로 라우팅된다.
	 * 설정자: 호스트 트랜스포트(nvme_fabric.c). 읽는 자: 타깃 ctrlr.c 의 명령
	 * 디스패처. 동기화: SQE 는 단일 producer/consumer 큐 페어이므로 별도 락 불필요. */

	uint8_t		reserved1;
	/* [한국어] reserved (offset 1, 1B) — NVMe-oF 스펙에 의해 0으로 채워야 함.
	 * 타깃은 비-zero 일 때 무시(권장)하거나 invalid field 에러로 응답할 수 있음. */

	uint16_t	cid;
	/* [한국어] Command Identifier (offset 2, 2B) — 호스트가 큐 내에서 명령을
	 * 식별하기 위해 부여하는 16비트 태그. 응답 CQE 의 cid 필드로 그대로 회신되어
	 * 호스트가 in-flight 명령과 매칭한다.
	 * 설정자: 호스트 SQ 발행 시. 읽는 자: 타깃은 보존 후 CQE 에 복사.
	 * 값 범위: 0~SQ depth-1 (한 큐 내에서 unique). */

	uint8_t		fctype;
	/* [한국어] Fabric Command Type (offset 4, 1B) — Fabrics 명령 종류 식별자.
	 * \ref spdk_nvmf_fabric_cmd_types (0x00=Property Set, 0x01=Connect,
	 * 0x04=Property Get, 0x05=AUTH Send, 0x06=AUTH Recv, 0xC0~=벤더).
	 * 설정자: 호스트. 읽는 자: 타깃 ctrlr.c → 각 fctype 별 핸들러로 디스패치. */

	uint8_t		reserved2[35];
	/* [한국어] reserved (offset 5..39, 35B) — Fabrics 일반형에서는 미사용.
	 * 구체 명령(prop_get_cmd 등) 은 이 영역을 자기 필드로 재해석하여 사용한다
	 * (즉, capsule_cmd 는 union 의 "기본 뷰"). */

	uint8_t		fabric_specific[24];
	/* [한국어] fabric_specific (offset 40..63, 24B) — 각 fctype 마다 의미가 다른
	 * payload 영역. NVMe Base SQE 의 "DW10..DW15" 위치에 해당.
	 * 설정자/읽는 자: fctype 별 구체 구조체로 cast 하여 접근. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_capsule_cmd) == 64, "Incorrect size");
/* [한국어] NVMe-oF 1.x §6.1: capsule SQE 는 정확히 64B (NVMe Base SQE 와 동일 폭).
 * 어긋나면 캡슐 직렬화 코드가 모두 깨지므로 컴파일을 실패시킴. */

/* Fabric Command Set */
#define SPDK_NVME_OPC_FABRIC 0x7f
/* [한국어] Fabrics 명령군의 NVMe opcode — 0x7F 는 NVMe Base 스펙에서 Fabrics
 * 전용으로 예약된 값. Admin/IO opcode 모두에서 사용되며 fctype 으로 세부 구분. */

/* [한국어] === Fabrics 명령 타입 식별자 (fctype) ===
 * NVMe-oF 1.x §6 Table "Fabrics Command Types" 와 NVMe-oF 2.0 §6.5. */
enum spdk_nvmf_fabric_cmd_types {
	SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET			= 0x00,
	/* [한국어] Property Set — 가상 NVMe 컨트롤러 레지스터(CC/CSTS/AQA 등) 쓰기.
	 * Fabrics 는 PCIe MMIO BAR 가 없어 이 명령으로 레지스터를 갱신.
	 * 호스트 lib/nvme/nvme_fabric.c::nvme_fabric_prop_set_cmd() 가 발행. */

	SPDK_NVMF_FABRIC_COMMAND_CONNECT			= 0x01,
	/* [한국어] Connect — 컨트롤러 또는 큐 페어 연결을 수립.
	 * qid=0 → admin queue 연결 (컨트롤러 생성), qid>0 → I/O qpair 추가.
	 * 페이로드(spdk_nvmf_fabric_connect_data) 에 hostid/hostnqn/subnqn 포함. */

	SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET			= 0x04,
	/* [한국어] Property Get — 가상 레지스터 읽기 (CAP/VS/CSTS 등).
	 * 응답은 spdk_nvmf_fabric_prop_get_rsp 에 4B 또는 8B 값으로 반환. */

	SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND		= 0x05,
	/* [한국어] AUTH Send — DH-CHAP 등 인증 페이로드를 컨트롤러에 전송.
	 * SGL 로 가리키는 버퍼에 spdk_nvmf_auth_negotiate / dhchap_reply 등이 담김. */

	SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV		= 0x06,
	/* [한국어] AUTH Receive — 컨트롤러로부터 인증 페이로드 수신
	 * (dhchap_challenge, success1, failure 등). */

	SPDK_NVMF_FABRIC_COMMAND_START_VENDOR_SPECIFIC		= 0xC0,
	/* [한국어] 0xC0 이상은 벤더 정의 영역 — 표준 외 확장에 사용. SPDK 본체는
	 * 이 영역을 사용하지 않음. */
};

/* [한국어] === Fabrics 전용 상태 코드 (CQE.status.sct=0x07 Fabrics) ===
 * NVMe-oF 1.x §3.3.4 Fabrics Status Code. 일반 NVMe sc 와 충돌하지 않도록
 * 0x80~ 영역에 배치. 타깃 측 lib/nvmf/ctrlr.c 가 이 코드들로 응답. */
enum spdk_nvmf_fabric_cmd_status_code {
	SPDK_NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT		= 0x80,
	/* [한국어] 호스트 recfmt 가 타깃이 지원하지 않는 값 (현재 0만 정의됨).
	 * Connect 명령에서 가장 흔히 발생. */

	SPDK_NVMF_FABRIC_SC_CONTROLLER_BUSY		= 0x81,
	/* [한국어] 컨트롤러가 다른 연결을 처리 중이거나 자원 부족. 호스트는 잠시 후
	 * 재시도해야 함. */

	SPDK_NVMF_FABRIC_SC_INVALID_PARAM		= 0x82,
	/* [한국어] Connect/Property 명령의 필드 값이 유효 범위를 벗어남.
	 * 응답 connect_rsp.invalid.{ipo,iattr} 에 어느 필드인지 표시. */

	SPDK_NVMF_FABRIC_SC_RESTART_DISCOVERY		= 0x83,
	/* [한국어] 호스트가 Discovery 를 재시작해야 함 (서브시스템 토폴로지 변화). */

	SPDK_NVMF_FABRIC_SC_INVALID_HOST		= 0x84,
	/* [한국어] hostnqn/hostid 가 타깃 ACL(allow list)에 없음. */

	SPDK_NVMF_FABRIC_SC_LOG_RESTART_DISCOVERY	= 0x90,
	/* [한국어] 진행 중이던 log page 조회를 처음부터 다시 해야 함
	 * (genctr 변경으로 인한 cache invalidation). */

	SPDK_NVMF_FABRIC_SC_AUTH_REQUIRED		= 0x91,
	/* [한국어] 이 서브시스템은 인증을 요구하는데 호스트가 인증 없이 접근 시도. */
};

/**
 * RDMA Queue Pair service types
 */
/* [한국어] === RDMA QP 서비스 타입 (Discovery Log tsas.rdma.rdma_qptype) ===
 * IBTA 스펙의 QP transport service type 을 NVMe-oF 가 그대로 노출. */
enum spdk_nvmf_rdma_qptype {
	/** Reliable connected */
	SPDK_NVMF_RDMA_QPTYPE_RELIABLE_CONNECTED	= 0x1,
	/* [한국어] RC (Reliable Connected) — 1:1 연결, 신뢰성 보장, in-order 전달.
	 * NVMe-oF RDMA 의 사실상 표준이며 SPDK lib/nvmf/rdma.c 가 이것만 지원. */

	/** Reliable datagram */
	SPDK_NVMF_RDMA_QPTYPE_RELIABLE_DATAGRAM		= 0x2,
	/* [한국어] RD (Reliable Datagram) — 거의 사용되지 않음. SPDK 미지원. */
};

/**
 * RDMA provider types
 */
/* [한국어] === RDMA 하위 프로바이더 (tsas.rdma.rdma_prtype) === */
enum spdk_nvmf_rdma_prtype {
	/** No provider specified */
	SPDK_NVMF_RDMA_PRTYPE_NONE	= 0x1,
	/* [한국어] 미지정 — Discovery 응답에서 트랜스포트 종류를 숨길 때. */

	/** InfiniBand */
	SPDK_NVMF_RDMA_PRTYPE_IB	= 0x2,
	/* [한국어] InfiniBand — 전용 IB 패브릭. AF_IB 주소 + pkey 사용. */

	/** RoCE v1 */
	SPDK_NVMF_RDMA_PRTYPE_ROCE	= 0x3,
	/* [한국어] RoCEv1 — Ethernet L2 위에 IB transport. 라우팅 불가. (deprecated) */

	/** RoCE v2 */
	SPDK_NVMF_RDMA_PRTYPE_ROCE2	= 0x4,
	/* [한국어] RoCEv2 — UDP/IPv4|v6 위에 IB transport. 라우팅 가능, 데이터센터 표준. */

	/** iWARP */
	SPDK_NVMF_RDMA_PRTYPE_IWARP	= 0x5,
	/* [한국어] iWARP — TCP 위 RDMA (RFC 5040~). Chelsio NIC 등이 지원. */
};

/**
 * RDMA connection management service types
 */
/* [한국어] === RDMA 연결 관리 서비스 (tsas.rdma.rdma_cms) === */
enum spdk_nvmf_rdma_cms {
	/** Sockets based endpoint addressing */
	SPDK_NVMF_RDMA_CMS_RDMA_CM	= 0x1,
	/* [한국어] librdmacm (rdma_cm) 기반 — IP 주소+포트로 RDMA QP 를 수립.
	 * SPDK 는 이 방식만 사용. listen_id/event_channel 패턴. */
};

/**
 * NVMe over Fabrics transport types
 */
/* [한국어] === 트랜스포트 종류 (Discovery entry trtype, Connect 무관) === */
enum spdk_nvmf_trtype {
	/** RDMA */
	SPDK_NVMF_TRTYPE_RDMA		= 0x1,
	/* [한국어] RDMA 트랜스포트 — IB/RoCE/iWARP 통합. lib/nvmf/rdma.c. */

	/** Fibre Channel */
	SPDK_NVMF_TRTYPE_FC		= 0x2,
	/* [한국어] Fibre Channel — FC-NVMe (T11 FC-NVMe-2). SPDK 외부 모듈. */

	/** TCP */
	SPDK_NVMF_TRTYPE_TCP		= 0x3,
	/* [한국어] TCP/IP 트랜스포트 — NVMe/TCP (RFC 9999 등). lib/nvmf/tcp.c. */

	/** Intra-host transport (loopback) */
	SPDK_NVMF_TRTYPE_INTRA_HOST	= 0xfe,
	/* [한국어] 동일 호스트 내 loopback — 메모리 복사 기반 디버그/테스트용. */
};

/**
 * Address family types
 */
/* [한국어] === 주소 패밀리 (Discovery entry adrfam) ===
 * POSIX AF_* 와 매핑되지만 NVMe-oF 자체 인코딩(0~) 사용. */
enum spdk_nvmf_adrfam {
	/** IPv4 (AF_INET) */
	SPDK_NVMF_ADRFAM_IPV4		= 0x1,
	/* [한국어] IPv4 — traddr 는 점-십진수 문자열 (예 "192.168.1.10"). */

	/** IPv6 (AF_INET6) */
	SPDK_NVMF_ADRFAM_IPV6		= 0x2,
	/* [한국어] IPv6 — traddr 는 표준 IPv6 문자열. */

	/** InfiniBand (AF_IB) */
	SPDK_NVMF_ADRFAM_IB		= 0x3,
	/* [한국어] InfiniBand GID — 16바이트 GID 의 문자열 표현. */

	/** Fibre Channel address family */
	SPDK_NVMF_ADRFAM_FC		= 0x4,
	/* [한국어] FC WWNN/WWPN 표현. */

	/** Intra-host transport (loopback) */
	SPDK_NVMF_ADRFAM_INTRA_HOST	= 0xfe,
	/* [한국어] 인트라호스트 loopback. */
};

/**
 * NVM subsystem types
 */
/* [한국어] === 서브시스템 종류 (Discovery entry subtype, NVMe-oF 1.1+) ===
 * Discovery Controller 가 자기 자신/타 NVM 서브시스템을 분류해서 보고. */
enum spdk_nvmf_subtype {
	/** Referral to a discovery service */
	SPDK_NVMF_SUBTYPE_DISCOVERY		= 0x1,
	/* [한국어] (Legacy) 또 다른 Discovery 서비스로의 referral. NVMe-oF 1.0 호환. */

	/** NVM Subsystem */
	SPDK_NVMF_SUBTYPE_NVME			= 0x2,
	/* [한국어] 실제 I/O 가 가능한 NVM 서브시스템 — 호스트가 Connect 대상으로 삼음. */

	/** Current Discovery Subsystem */
	SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT	= 0x3
	/* [한국어] NVMe-oF 2.0 §3.1.2 — 현재 응답 중인 Discovery Controller 자체.
	 * 호스트가 persistent discovery connection 을 수립할 대상. */
};

/* Discovery Log Entry Flags - Duplicate Returned Information */
#define SPDK_NVMF_DISCOVERY_LOG_EFLAGS_DUPRETINFO (1u << 0u)
/* [한국어] eflags bit0: DUPRETINFO — 동일 엔트리 정보가 다른 entry 에서 중복될 수
 * 있음을 호스트에 알림 (멀티패스 환경에서 NVM 서브시스템이 여러 포트로 노출). */

/* Discovery Log Entry Flags - Explicit Persistent Connection Support for Discovery */
#define SPDK_NVMF_DISCOVERY_LOG_EFLAGS_EPCSD (1u << 1u)
/* [한국어] eflags bit1: EPCSD — 이 Discovery Controller 가 영속(persistent)
 * connection 을 지원함. 호스트는 KATO=0 으로 Connect 하여 long-lived 세션을 유지. */

/**
 * Connections shall be made over a fabric secure channel
 */
/* [한국어] === Discovery entry treq.secure_channel 값 ===
 * 호스트가 이 서브시스템에 접속할 때 보안 채널(IPSec/TLS) 요구 여부. */
enum spdk_nvmf_treq_secure_channel {
	/** Not specified */
	SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED	= 0x0,
	/* [한국어] 정책 미정 — 호스트가 자율 결정. */

	/** Required */
	SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED		= 0x1,
	/* [한국어] 필수 — 호스트는 반드시 TLS 등 보안 채널 위에서 Connect 해야 함. */

	/** Not required */
	SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED	= 0x2,
	/* [한국어] 불요 — 평문 트랜스포트 허용. */
};

/* [한국어] === AUTH Receive 명령 (fctype=0x06) SQE ===
 * NVMe-oF 2.0 §8.13. 호스트가 컨트롤러로부터 인증 메시지(challenge, success1,
 * failure)를 수신하기 위해 발행. SGL1 이 가리키는 버퍼에 컨트롤러가 응답을 채움.
 * SCSI Security Protocol In 명령과 유사한 시맨틱(secp/spsp/al). */
struct spdk_nvmf_fabric_auth_recv_cmd {
	uint8_t		opcode;
	/* [한국어] 항상 0x7F (SPDK_NVME_OPC_FABRIC). 설정자: 호스트. */

	uint8_t		reserved1;
	/* [한국어] reserved, 0. */

	uint16_t	cid;
	/* [한국어] 명령 ID — 응답 매칭용. */

	uint8_t		fctype; /* NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV (0x06) */
	/* [한국어] fctype = 0x06 (AUTH_RECV) 고정. */

	uint8_t		reserved2[19];
	/* [한국어] reserved (offset 5..23). */

	struct spdk_nvme_sgl_descriptor sgl1;
	/* [한국어] SGL Descriptor 1 (offset 24..39, 16B) — 호스트가 인증 응답을 받을
	 * 버퍼의 주소/길이. RDMA 면 keyed SGL, TCP 면 in-capsule offset 형태. */

	uint8_t		reserved3;
	/* [한국어] reserved (offset 40). */

	uint8_t		spsp0;
	/* [한국어] Security Protocol Specific 0 (offset 41) — SCSI 류 SPSP. */

	uint8_t		spsp1;
	/* [한국어] Security Protocol Specific 1 (offset 42). */

	uint8_t		secp;
	/* [한국어] Security Protocol (offset 43) — 0xE9 (SPDK_NVMF_AUTH_SECP_NVME)
	 * = NVMe 인증 프로토콜. */

	uint32_t	al;
	/* [한국어] Allocation Length (offset 44..47) — sgl1 버퍼의 크기, 4B. */

	uint8_t		reserved4[16];
	/* [한국어] reserved (offset 48..63). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_auth_recv_cmd) == 64, "Incorrect size");
/* [한국어] AUTH Recv SQE 는 정확히 64B (Fabrics 캡슐 SQE 일반 폭). */

/* [한국어] === AUTH Send 명령 (fctype=0x05) SQE ===
 * 호스트 → 컨트롤러로 인증 페이로드(negotiate/reply/success2)를 전송.
 * sgl1 이 가리키는 버퍼는 호스트가 채워서 보낸다. */
struct spdk_nvmf_fabric_auth_send_cmd {
	uint8_t		opcode;
	/* [한국어] 0x7F. */

	uint8_t		reserved1;
	uint16_t	cid;
	/* [한국어] 명령 ID. */

	uint8_t		fctype; /* NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND (0x05) */
	/* [한국어] fctype = 0x05. */

	uint8_t		reserved2[19];
	struct spdk_nvme_sgl_descriptor sgl1;
	/* [한국어] 송신할 인증 메시지가 들어있는 버퍼의 SGL. */

	uint8_t		reserved3;
	uint8_t		spsp0;
	/* [한국어] SPSP0 — Security Protocol Specific. */
	uint8_t		spsp1;
	/* [한국어] SPSP1. */
	uint8_t		secp;
	/* [한국어] Security Protocol = 0xE9 (NVMe). */

	uint32_t	tl;
	/* [한국어] Transfer Length (offset 44..47) — sgl1 송신 길이.
	 * (Recv 의 al 과 대응되는 송신 측 필드). */

	uint8_t		reserved4[16];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_auth_send_cmd) == 64, "Incorrect size");

/* [한국어] === Connect 명령의 데이터 페이로드 (1024B) ===
 * NVMe-oF 1.1 §3.3.1 Figure 20. Connect SQE 가 SGL 로 가리키는 1KB 버퍼의 내용.
 * hostid + cntlid + subnqn + hostnqn 을 포함하여 호스트 신원과 접속 대상 식별. */
struct spdk_nvmf_fabric_connect_data {
	uint8_t		hostid[16];
	/* [한국어] Host Identifier (offset 0..15, 16B) — UUID 형식의 호스트 고유 ID.
	 * RFC 4122 UUID. 호스트는 spdk_nvme_ctrlr_opts 또는 application config 에서
	 * 지정. 타깃은 같은 hostid+hostnqn 조합으로 reservation 등을 추적. */

	uint16_t	cntlid;
	/* [한국어] Controller ID (offset 16..17) — 호스트가 원하는 컨트롤러 ID.
	 * 0xFFFF = "any available" (타깃이 동적 할당). 0x0000~0xFFFE = 특정 컨트롤러
	 * (정적 매핑 시). 응답으로 실제 할당 cntlid 가 connect_rsp.success.cntlid 에 옴. */

	uint8_t		reserved5[238];
	/* [한국어] reserved (offset 18..255). */

	uint8_t		subnqn[SPDK_NVME_NQN_FIELD_SIZE];
	/* [한국어] Subsystem NQN (offset 256, 256B) — 접속할 NVM 서브시스템 NQN
	 * (예 "nqn.2016-06.io.spdk:cnode1"). NULL-terminated. SPDK_NVME_NQN_FIELD_SIZE
	 * = 256 (nvme_spec.h 정의, 223B max + null 패딩). */

	uint8_t		hostnqn[SPDK_NVME_NQN_FIELD_SIZE];
	/* [한국어] Host NQN (offset 512, 256B) — 호스트의 NQN. 타깃 ACL 에서
	 * allowed host 매칭 키. */

	uint8_t		reserved6[256];
	/* [한국어] reserved (offset 768..1023). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_connect_data) == 1024, "Incorrect size");
/* [한국어] 정확히 1024B (스펙 §3.3.1). */

/* [한국어] === Connect 명령 (fctype=0x01) SQE ===
 * NVMe-oF 1.1 §3.3. 컨트롤러 admin queue (qid=0) 또는 I/O qpair (qid>0) 를 생성.
 * sgl1 은 위 connect_data (1KB) 를 가리킨다. */
struct spdk_nvmf_fabric_connect_cmd {
	uint8_t		opcode;
	uint8_t		reserved1;
	uint16_t	cid;
	uint8_t		fctype;
	/* [한국어] fctype = 0x01 (CONNECT). */

	uint8_t		reserved2[19];

	struct spdk_nvme_sgl_descriptor sgl1;
	/* [한국어] 위 spdk_nvmf_fabric_connect_data (1024B) 를 가리키는 SGL. */

	uint16_t	recfmt; /* Connect Record Format */
	/* [한국어] Record Format (offset 40..41) — 이 Connect 명령 페이로드 포맷의
	 * 버전. 현재 정의된 값은 0만. 비매칭 시 SC_INCOMPATIBLE_FORMAT(0x80) 반환. */

	uint16_t	qid; /* Queue Identifier */
	/* [한국어] Queue ID (offset 42..43) — 0=admin queue, 1~=I/O qpair.
	 * qid=0 일 때 새 controller 가 생성되며, qid>0 은 기존 cntlid 에 qpair 추가. */

	uint16_t	sqsize; /* Submission Queue Size */
	/* [한국어] SQ size (zero-based, offset 44..45) — 이 qpair 의 SQ 깊이 - 1.
	 * 예: 0=1개 슬롯, 31=32 슬롯. admin qpair 는 ≥
	 * SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE-1 = 31 권장. */

	uint8_t		cattr; /* queue attributes */
	/* [한국어] Connect Attributes (offset 46) — bit0: priority class 등
	 * (현재 SPDK 는 0 사용). */

	uint8_t		reserved3;

	uint32_t	kato; /* keep alive timeout */
	/* [한국어] Keep Alive Timeout (offset 48..51, ms) — admin queue (qid=0)
	 * 에서만 의미. 0 = 미사용. 호스트는 이 시간 내에 Keep Alive 를 보내지 않으면
	 * 컨트롤러가 연결을 종료. Discovery Controller 에서는 0 도 허용. */

	uint8_t		reserved4[12];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_connect_cmd) == 64, "Incorrect size");

/* [한국어] === Connect 명령 응답 CQE ===
 * Connect 는 NVMe Base CQE 16B 를 그대로 쓰되, status_code_specific(DW0) 영역을
 * 성공/실패 케이스별로 union 으로 재해석한다. */
struct spdk_nvmf_fabric_connect_rsp {
	union {
		struct {
			uint16_t cntlid;
			/* [한국어] success.cntlid (offset 0..1) — 타깃이 호스트에 할당한
			 * 실제 controller ID. host 가 cntlid=0xFFFF 로 요청했을 때 결정됨. */
			struct {
				uint16_t reserved1	: 1;
				/* [한국어] bit0 reserved. */

				uint16_t atr		: 1;
				/* [한국어] ATR (Authentication Required, bit1) — 이 컨트롤러가
				 * 인증 요구. 호스트는 후속으로 AUTH Send/Recv 시퀀스를 수행. */

				uint16_t ascr		: 1;
				/* [한국어] ASCR (Authentication and Secure Channel Required, bit2)
				 * — 보안 채널 + 인증 모두 요구. */

				uint16_t reserved2	: 13;
				/* [한국어] bit3..15 reserved. */
			} authreq;
			/* [한국어] success.authreq (offset 2..3) — 인증 요구 플래그. */
		} success;
		/* [한국어] success 케이스 — status.sc==0 (Successful Completion) 일 때
		 * 이 뷰를 사용. */

		struct {
			uint16_t	ipo;
			/* [한국어] invalid.ipo (Invalid Parameter Offset, offset 0..1) —
			 * connect_data/connect_cmd 내에서 어느 오프셋이 잘못됐는지. */

			uint8_t		iattr;
			/* [한국어] invalid.iattr (offset 2) — bit0=1 면 ipo 가 connect_data
			 * 페이로드 오프셋, 0이면 SQE 필드 오프셋. */

			uint8_t		reserved;
		} invalid;
		/* [한국어] invalid 케이스 — sc=SPDK_NVMF_FABRIC_SC_INVALID_PARAM(0x82)
		 * 일 때 이 뷰. */

		uint32_t raw;
		/* [한국어] DW0 raw — union 의 size 를 4B 로 못박음. */
	} status_code_specific;

	uint32_t	reserved0;
	/* [한국어] DW1 reserved (CQE.dw1). */

	uint16_t	sqhd;
	/* [한국어] SQ Head Pointer (CQE.dw2 low) — 타깃이 호스트 SQ 의 head 까지
	 * 처리했음을 통보. flow control 에 사용. */

	uint16_t	reserved1;

	uint16_t	cid;
	/* [한국어] Command ID (CQE.dw3 low) — 매칭용. SQE.cid 와 동일. */

	struct spdk_nvme_status status;
	/* [한국어] NVMe status (CQE.dw3 high, 2B) — sct/sc/m/dnr 비트필드.
	 * Fabrics 에서는 sct=0x07 (Fabrics) + sc=spdk_nvmf_fabric_cmd_status_code. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_connect_rsp) == 16, "Incorrect size");

#define SPDK_NVMF_PROP_SIZE_4	0
/* [한국어] Property Get/Set 의 attrib.size = 0 → 32비트 레지스터 접근.
 * VS, CC, CSTS, NSSR 등 4B 레지스터에 사용. */

#define SPDK_NVMF_PROP_SIZE_8	1
/* [한국어] attrib.size = 1 → 64비트 레지스터 접근.
 * CAP (8B), ASQ (8B), ACQ (8B) 등에 사용. */

/* [한국어] === Property Get 명령 (fctype=0x04) SQE ===
 * NVMe-oF 1.1 §3.4. PCIe BAR 의 MMIO 가 없는 Fabrics 에서 컨트롤러 register
 * (CAP, VS, CC, CSTS 등 NVMe Base §3.1) 를 읽기 위한 명령.
 * 응답 prop_get_rsp.value.{u32,u64} 로 값이 반환된다. */
struct spdk_nvmf_fabric_prop_get_cmd {
	uint8_t		opcode;
	uint8_t		reserved1;
	uint16_t	cid;
	uint8_t		fctype;
	/* [한국어] fctype = 0x04 (PROPERTY_GET). */

	uint8_t		reserved2[35];

	struct {
		uint8_t size		: 3;
		/* [한국어] attrib.size (3비트) — 0=4B, 1=8B (SPDK_NVMF_PROP_SIZE_4/8).
		 * 다른 값은 invalid. */
		uint8_t reserved	: 5;
	} attrib;
	/* [한국어] attrib byte (offset 40) — 접근 폭 지정. */

	uint8_t		reserved3[3];

	uint32_t	ofst;
	/* [한국어] Offset (offset 44..47) — 컨트롤러 register space 내 오프셋
	 * (NVMe Base §3.1 Table). 예: 0x00=CAP, 0x08=VS, 0x14=CC, 0x1C=CSTS. */

	uint8_t		reserved4[16];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_prop_get_cmd) == 64, "Incorrect size");

/* [한국어] === Property Get 응답 CQE ===
 * 16B CQE. status_code_specific(DW0,DW1) 위치에 4B 또는 8B 값을 실어 보낸다. */
struct spdk_nvmf_fabric_prop_get_rsp {
	union {
		uint64_t u64;
		/* [한국어] 8B 값 view — attrib.size=1 일 때. CAP 등. */
		struct {
			uint32_t low;
			/* [한국어] 32비트 값 (DW0). attrib.size=0 일 때 low 만 유효. */
			uint32_t high;
			/* [한국어] DW1 — size=1 일 때 상위 32비트. */
		} u32;
	} value;

	uint16_t	sqhd;
	/* [한국어] SQ head — flow control. */
	uint16_t	reserved0;
	uint16_t	cid;
	/* [한국어] 명령 ID 매칭. */
	struct spdk_nvme_status status;
	/* [한국어] NVMe status. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_prop_get_rsp) == 16, "Incorrect size");

/* [한국어] === Property Set 명령 (fctype=0x00) SQE ===
 * Fabrics 가상 register 쓰기 — CC.EN=1 로 컨트롤러 enable, CC.SHN 으로 shutdown,
 * AQA 로 admin queue 깊이 설정 등 NVMe Base 의 BAR write 등가 동작. */
struct spdk_nvmf_fabric_prop_set_cmd {
	uint8_t		opcode;
	uint8_t		reserved0;
	uint16_t	cid;
	uint8_t		fctype;
	/* [한국어] fctype = 0x00 (PROPERTY_SET). */

	uint8_t		reserved1[35];

	struct {
		uint8_t size		: 3;
		/* [한국어] 접근 폭 (0=4B, 1=8B). */
		uint8_t reserved	: 5;
	} attrib;

	uint8_t		reserved2[3];

	uint32_t	ofst;
	/* [한국어] register 오프셋. */

	union {
		uint64_t u64;
		struct {
			uint32_t low;
			uint32_t high;
		} u32;
	} value;
	/* [한국어] 쓰기 값 (offset 48..55) — size 에 따라 4B 또는 8B. */

	uint8_t		reserved4[8];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fabric_prop_set_cmd) == 64, "Incorrect size");

/* [한국어] === NQN/주소 길이 상수 === */
#define SPDK_NVMF_NQN_MIN_LEN 11 /* The prefix in the spec is 11 characters */
/* [한국어] NQN 최소 길이 — NVMe-oF 스펙이 정의하는 NQN prefix "nqn.YYYY-MM."
 * (11자) 이상이어야 함. */

#define SPDK_NVMF_NQN_MAX_LEN 223
/* [한국어] NQN 최대 길이 — 스펙 §3.2: 최대 223자. NQN 필드는 256B 인데 null 포함
 * 하여 223자 이내. */

#define SPDK_NVMF_NQN_UUID_PRE_LEN 32
/* [한국어] UUID prefix "nqn.2014-08.org.nvmexpress:uuid:" 의 길이 (32자). */

#define SPDK_NVMF_UUID_STRING_LEN 36
/* [한국어] RFC 4122 UUID 문자열 길이 (8-4-4-4-12 = 36자, 하이픈 포함). */

#define SPDK_NVMF_NQN_UUID_PRE "nqn.2014-08.org.nvmexpress:uuid:"
/* [한국어] UUID 기반 NQN 표준 prefix — 사용자가 별도 NQN 을 안 정하면 이 prefix 에
 * 호스트/서브시스템 UUID 를 붙여서 NQN 을 자동 생성. */

#define SPDK_NVMF_DISCOVERY_NQN "nqn.2014-08.org.nvmexpress.discovery"
/* [한국어] 표준 Discovery Subsystem NQN — 호스트가 Discovery Controller 에 접속할
 * 때 subnqn 으로 사용. NVMe-oF 1.x §7.1. */

#define SPDK_DOMAIN_LABEL_MAX_LEN 63 /* RFC 1034 max domain label length */
/* [한국어] DNS 도메인 라벨 최대 길이 (RFC 1034 §3.1). NQN 검증에 사용. */

#define SPDK_NVMF_TRSTRING_MAX_LEN 32
/* [한국어] trstring (트랜스포트 종류 문자열, "RDMA"/"TCP"/"FC") 최대 길이. */

#define SPDK_NVMF_TRADDR_MAX_LEN 256
/* [한국어] traddr (전송 주소) 최대 길이 — IPv6 + zone-id 를 위해 256B. */

#define SPDK_NVMF_TRSVCID_MAX_LEN 32
/* [한국어] trsvcid (서비스 식별자, 보통 포트 번호 문자열) 최대 길이. */

/** RDMA transport-specific address subtype */
/* [한국어] === Discovery entry tsas (Transport Specific Address Subtype) - RDMA ===
 * Discovery Log entry 에서 RDMA 서비스 상세 정보를 noting. */
struct spdk_nvmf_rdma_transport_specific_address_subtype {
	/** RDMA QP service type (\ref spdk_nvmf_rdma_qptype) */
	uint8_t		rdma_qptype;
	/* [한국어] tsas[0] — RC(0x1)/RD(0x2). 호스트는 RC 인 경우만 통상 처리. */

	/** RDMA provider type (\ref spdk_nvmf_rdma_prtype) */
	uint8_t		rdma_prtype;
	/* [한국어] tsas[1] — IB/RoCE/RoCEv2/iWARP 구분. 호스트가 적절한 verbs context
	 * 를 선택하기 위한 hint. */

	/** RDMA connection management service (\ref spdk_nvmf_rdma_cms) */
	uint8_t		rdma_cms;
	/* [한국어] tsas[2] — 항상 1 (rdma_cm). */

	uint8_t		reserved0[5];
	/* [한국어] tsas[3..7] reserved. */

	/** RDMA partition key for AF_IB */
	uint16_t	rdma_pkey;
	/* [한국어] tsas[8..9] — InfiniBand partition key (P_Key). prtype=IB 일 때만
	 * 의미. RoCE/iWARP 는 0. */

	uint8_t		reserved2[246];
	/* [한국어] tsas[10..255] reserved — tsas 전체 256B 채우기 위해. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_transport_specific_address_subtype) == 256,
		   "Incorrect size");

/** TCP Secure Socket Type */
/* [한국어] === TCP tsas.sectype === */
enum spdk_nvme_tcp_secure_socket_type {
	/** No security */
	SPDK_NVME_TCP_SECURITY_NONE			= 0,
	/* [한국어] 평문 TCP — 디버그/사설 망. */

	/** TLS (Secure Sockets) version 1.2 */
	SPDK_NVME_TCP_SECURITY_TLS_1_2			= 1,
	/* [한국어] TLS 1.2 (deprecated by NVMe-oF 2.0, 일부 구현 호환용). */

	/** TLS (Secure Sockets) version 1.3 */
	SPDK_NVME_TCP_SECURITY_TLS_1_3			= 2,
	/* [한국어] TLS 1.3 + PSK — NVMe TCP 1.0a 표준. SPDK 의 권장 설정. */
};

/** TCP transport-specific address subtype */
/* [한국어] === Discovery entry tsas - TCP === */
struct spdk_nvme_tcp_transport_specific_address_subtype {
	/** Security type (\ref spdk_nvme_tcp_secure_socket_type) */
	uint8_t		sectype;
	/* [한국어] tsas[0] — 0=NONE / 1=TLSv1.2 / 2=TLSv1.3. */

	uint8_t		reserved0[255];
	/* [한국어] 나머지 255B reserved (tsas 256B 충족). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_tcp_transport_specific_address_subtype) == 256,
		   "Incorrect size");

/** Transport-specific address subtype */
/* [한국어] === Discovery entry tsas (256B) 의 union view ===
 * trtype 에 따라 rdma 또는 tcp 멤버 중 하나를 사용. raw 는 항상 256B 보장. */
union spdk_nvmf_transport_specific_address_subtype {
	uint8_t raw[256];
	/* [한국어] 알 수 없는/벤더 트랜스포트를 위한 raw 뷰. */

	/** RDMA */
	struct spdk_nvmf_rdma_transport_specific_address_subtype rdma;
	/* [한국어] trtype=1 (RDMA). */

	/** TCP */
	struct spdk_nvme_tcp_transport_specific_address_subtype tcp;
	/* [한국어] trtype=3 (TCP). */
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvmf_transport_specific_address_subtype) == 256,
		   "Incorrect size");

#define SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE 32
/* [한국어] admin SQ 최소 크기 (NVMe-oF 1.1 §3.3.1) — Connect 시 admin qpair 의
 * sqsize 는 최소 31(zero-based) → 실제 32 슬롯 이상이어야 함. */

/**
 * Discovery Log Page entry
 */
/* [한국어] === Discovery Log Page Entry (1024B) ===
 * NVMe-oF 1.1 §5.3 Figure 38. Discovery Controller 가 Get Log Page (LID=0x70)
 * 응답에 채우는 단일 NVM 서브시스템 정보. 호스트는 이 entry 의 trtype/traddr/
 * trsvcid/subnqn 을 보고 Connect 대상을 결정한다. */
struct spdk_nvmf_discovery_log_page_entry {
	/** Transport type (\ref spdk_nvmf_trtype) */
	uint8_t		trtype;
	/* [한국어] entry[0] — RDMA(1)/FC(2)/TCP(3). 호스트의 트랜스포트 모듈 선택 키. */

	/** Address family (\ref spdk_nvmf_adrfam) */
	uint8_t		adrfam;
	/* [한국어] entry[1] — IPv4/IPv6/IB/FC. traddr 파싱 방법 결정. */

	/** Subsystem type (\ref spdk_nvmf_subtype) */
	uint8_t		subtype;
	/* [한국어] entry[2] — DISCOVERY/NVME/DISCOVERY_CURRENT. 호스트는 NVME(2) 인
	 * entry 에만 I/O 용 Connect 를 시도. */

	/** Transport requirements */
	struct {
		/** Secure channel requirements (\ref spdk_nvmf_treq_secure_channel) */
		uint8_t secure_channel : 2;
		/* [한국어] treq.bit[0..1] — 보안 채널 요구 (NOT_SPEC/REQUIRED/NOT_REQUIRED). */

		uint8_t reserved : 6;
	} treq;
	/* [한국어] entry[3] — transport requirements byte. */

	/** NVM subsystem port ID */
	uint16_t	portid;
	/* [한국어] entry[4..5] — NVM 서브시스템 포트 ID (멀티포트 식별). */

	/** Controller ID */
	uint16_t	cntlid;
	/* [한국어] entry[6..7] — 추천 컨트롤러 ID. 0xFFFF = 동적, 0xFFFE = static
	 * any, 그 외 = 정확한 cntlid. */

	/** Admin max SQ size */
	uint16_t	asqsz;
	/* [한국어] entry[8..9] — admin queue 최대 SQ size (zero-based 아님).
	 * 호스트는 이 값을 넘는 sqsize 로 Connect 하지 않음. */

	/** Entry Flags */
	uint16_t	eflags;
	/* [한국어] entry[10..11] — DUPRETINFO/EPCSD 비트(NVMe-oF 2.0). 그 외 reserved. */

	uint8_t		reserved0[20];
	/* [한국어] entry[12..31] reserved. */

	/** Transport service identifier */
	uint8_t		trsvcid[SPDK_NVMF_TRSVCID_MAX_LEN];
	/* [한국어] entry[32..63] (32B) — TCP/IP 라면 포트 번호 문자열 ("4420").
	 * RDMA 라면 RDMA service id. NULL-padded. */

	uint8_t		reserved1[192];

	/** NVM subsystem qualified name */
	uint8_t		subnqn[256];
	/* [한국어] entry[256..511] — 이 서브시스템의 NQN. 호스트가 Connect 명령의
	 * subnqn 으로 그대로 사용. */

	/** Transport address */
	uint8_t		traddr[SPDK_NVMF_TRADDR_MAX_LEN];
	/* [한국어] entry[512..767] — 전송 주소 (IPv4/IPv6 문자열, IB GID 등).
	 * adrfam 에 따라 파싱. */

	/** Transport-specific address subtype */
	union spdk_nvmf_transport_specific_address_subtype tsas;
	/* [한국어] entry[768..1023] — RDMA 면 qptype/prtype/cms/pkey, TCP 면 sectype. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_discovery_log_page_entry) == 1024, "Incorrect size");

/* [한국어] === Discovery Log Page 헤더 + entry 배열 ===
 * Get Log Page LID=0x70 응답의 전체 레이아웃. 헤더 1024B + entries[numrec]. */
struct spdk_nvmf_discovery_log_page {
	uint64_t	genctr;
	/* [한국어] header[0..7] — Generation Counter. 카탈로그가 변할 때마다 타깃이
	 * 증가. 호스트는 변경 감지 시 캐시 invalidate 후 재조회. AEN(Asynchronous
	 * Event Notification) 으로 변경 통지 가능. */

	uint64_t	numrec;
	/* [한국어] header[8..15] — entries[] 배열 길이 (NVM 서브시스템 수). */

	uint16_t	recfmt;
	/* [한국어] header[16..17] — Discovery Log entry 포맷 버전 (현재 0). */

	uint8_t		reserved0[1006];
	/* [한국어] header[18..1023] reserved — 1024B 헤더 패딩. */

	struct spdk_nvmf_discovery_log_page_entry entries[0];
	/* [한국어] flexible array — header 뒤에 numrec 개 entry 가 이어짐.
	 * sizeof(header) 자체는 1024B (entries[0] 은 zero-length). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_discovery_log_page) == 1024, "Incorrect size");

/** Security protocol identifier assigned to NVMe */
#define SPDK_NVMF_AUTH_SECP_NVME 0xe9
/* [한국어] AUTH Send/Recv 의 secp 필드 값 — 0xE9 는 SCSI Security Protocol 표에서
 * NVMe 인증에 할당된 ID (T10 SPC). */

/** Authentication types */
/* [한국어] === 인증 메시지 auth_type ===
 * NVMe-oF 2.0 §8.13 Table "Authentication Message Header". */
enum spdk_nvmf_auth_type {
	SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE	= 0x0,
	/* [한국어] negotiate/failure 등 protocol-agnostic 메시지. */

	SPDK_NVMF_AUTH_TYPE_DHCHAP		= 0x1,
	/* [한국어] DH-CHAP (Diffie-Hellman Challenge Handshake). NVMe-oF 의 권장
	 * 인증 프로토콜. */
};

/** AUTH message identifiers */
/* [한국어] === 인증 메시지 auth_id (auth_type 결합 시 메시지 종류 결정) === */
enum spdk_nvmf_auth_id {
	SPDK_NVMF_AUTH_ID_NEGOTIATE		= 0x00,
	/* [한국어] 호스트→타깃: 지원 hash/dhgroup/SCC 협상 시작. */
	SPDK_NVMF_AUTH_ID_DHCHAP_CHALLENGE	= 0x01,
	/* [한국어] 타깃→호스트: 챌린지(C1) + DH 공개값. */
	SPDK_NVMF_AUTH_ID_DHCHAP_REPLY		= 0x02,
	/* [한국어] 호스트→타깃: 챌린지 응답 R1 (+선택적 호스트 챌린지 C2). */
	SPDK_NVMF_AUTH_ID_DHCHAP_SUCCESS1	= 0x03,
	/* [한국어] 타깃→호스트: 응답 검증 통과 + (양방향이면) R2. */
	SPDK_NVMF_AUTH_ID_DHCHAP_SUCCESS2	= 0x04,
	/* [한국어] 호스트→타깃: 양방향 인증 완료 ack. */
	SPDK_NVMF_AUTH_ID_FAILURE2		= 0xf0,
	/* [한국어] 호스트→타깃: 호스트 측에서 검증 실패 통지. */
	SPDK_NVMF_AUTH_ID_FAILURE1		= 0xf1,
	/* [한국어] 타깃→호스트: 타깃 측에서 검증 실패 통지. */
};

/** Hash function identifiers */
/* [한국어] === DH-CHAP 해시 함수 ID === */
enum spdk_nvmf_dhchap_hash {
	SPDK_NVMF_DHCHAP_HASH_NONE	= 0x0,
	/* [한국어] (placeholder, 협상 실패 표시용) */
	SPDK_NVMF_DHCHAP_HASH_SHA256	= 0x1,
	/* [한국어] HMAC-SHA-256 (32B 다이제스트). */
	SPDK_NVMF_DHCHAP_HASH_SHA384	= 0x2,
	/* [한국어] HMAC-SHA-384 (48B). */
	SPDK_NVMF_DHCHAP_HASH_SHA512	= 0x3,
	/* [한국어] HMAC-SHA-512 (64B). */
};

/** Diffie-Hellman group identifiers */
/* [한국어] === DH 그룹 (RFC 7919 표준 그룹) === */
enum spdk_nvmf_dhchap_dhgroup {
	SPDK_NVMF_DHCHAP_DHGROUP_NULL = 0x0,
	/* [한국어] DH 미사용 (PSK 만으로 인증). */
	SPDK_NVMF_DHCHAP_DHGROUP_2048 = 0x1,
	/* [한국어] ffdhe2048 (RFC 7919). */
	SPDK_NVMF_DHCHAP_DHGROUP_3072 = 0x2,
	/* [한국어] ffdhe3072. */
	SPDK_NVMF_DHCHAP_DHGROUP_4096 = 0x3,
	/* [한국어] ffdhe4096. */
	SPDK_NVMF_DHCHAP_DHGROUP_6144 = 0x4,
	/* [한국어] ffdhe6144. */
	SPDK_NVMF_DHCHAP_DHGROUP_8192 = 0x5,
	/* [한국어] ffdhe8192 (가장 강함, 가장 느림). */
};

/* [한국어] === AUTH_Negotiate 메시지의 단일 protocol descriptor ===
 * 호스트가 지원하는 hash/dhgroup 목록을 광고. */
struct spdk_nvmf_auth_descriptor {
	uint8_t		auth_id;
	/* [한국어] descriptor[0] — 보통 0x1 (DHCHAP). */
	uint8_t		reserved0;
	uint8_t		halen;
	/* [한국어] descriptor[2] — hash_id_list 의 유효 길이 (1~30). */
	uint8_t		dhlen;
	/* [한국어] descriptor[3] — dhg_id_list 유효 길이. */
	uint8_t		hash_id_list[30];
	/* [한국어] descriptor[4..33] — 지원하는 hash ID 목록 (선호 순). */
	uint8_t		dhg_id_list[30];
	/* [한국어] descriptor[34..63] — 지원하는 DH group 목록. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_auth_descriptor) == 64, "Incorrect size");

/** Secure channel concatenation */
/* [한국어] === SCC (Secure Channel Concatenation) ===
 * Negotiate 메시지의 sc_c 필드. TLS 위에서 인증을 묶을지 여부. */
enum spdk_nvmf_auth_scc {
	/** No secure channel concatenation */
	SPDK_NVMF_AUTH_SCC_DISABLED	= 0,
	/* [한국어] SCC 비활성 — 일반 PSK/DHCHAP 만. */
	/** Secure channel concatenation with TLS (TCP) */
	SPDK_NVMF_AUTH_SCC_TLS		= 1,
	/* [한국어] TLS (NVMe/TCP) 의 master secret 와 인증 secret 를 묶음. */
};

/* [한국어] === AUTH_Negotiate (auth_type=0x0, auth_id=0x00) ===
 * 호스트→타깃 첫 번째 인증 메시지. 협상 시작. */
struct spdk_nvmf_auth_negotiate {
	uint8_t					auth_type;
	/* [한국어] 0x0 (COMMON_MESSAGE). */
	uint8_t					auth_id;
	/* [한국어] 0x00 (NEGOTIATE). */
	uint8_t					reserved0[2];
	uint16_t				t_id;
	/* [한국어] Transaction ID — 호스트가 임의로 부여, 응답에서 동일 값 echo.
	 * 동시 다중 인증 세션 식별. */
	uint8_t					sc_c;
	/* [한국어] Secure Channel Concatenation 요청 (DISABLED/TLS). */
	uint8_t					napd;
	/* [한국어] Number of Auth Protocol Descriptors — descriptors[] 길이. */
	struct spdk_nvmf_auth_descriptor	descriptors[0];
	/* [한국어] flexible array — napd 개. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_auth_negotiate) == 8, "Incorrect size");
/* [한국어] 헤더 자체는 8B (descriptors 제외). */

/* [한국어] === DHCHAP_Challenge (auth_type=0x1, auth_id=0x01) ===
 * 타깃→호스트. 첫 챌린지 + DH 공개값 전달. */
struct spdk_nvmf_dhchap_challenge {
	uint8_t		auth_type;
	/* [한국어] 0x1 (DHCHAP). */
	uint8_t		auth_id;
	/* [한국어] 0x01 (CHALLENGE). */
	uint8_t		reserved0[2];
	uint16_t	t_id;
	/* [한국어] negotiate 의 t_id echo. */
	uint8_t		hl;
	/* [한국어] hash length — cval[] 의 길이 (예 SHA-256=32). */
	uint8_t		reserved1;
	uint8_t		hash_id;
	/* [한국어] 협상 결정된 hash (SHA256/384/512). */
	uint8_t		dhg_id;
	/* [한국어] 협상 결정된 DH group (NULL/ffdhe*). */
	uint16_t	dhvlen;
	/* [한국어] dhv (서버 DH public value) 길이. dhg_id=NULL 이면 0. */
	uint32_t	seqnum;
	/* [한국어] sequence number — replay 방지. */
	uint8_t		cval[0];
	/* [한국어] flexible: cval[hl] (challenge) 뒤에 dhv[dhvlen] 이 옴. */
	/* Followed by optional dhv if dhvlen > 0 */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_dhchap_challenge) == 16, "Incorrect size");

/* [한국어] === DHCHAP_Reply (호스트→타깃, auth_id=0x02) ===
 * 챌린지 응답 R1 + 선택적 호스트 챌린지 C2 (양방향 인증). */
struct spdk_nvmf_dhchap_reply {
	uint8_t		auth_type;
	uint8_t		auth_id;
	uint8_t		reserved0[2];
	uint16_t	t_id;
	uint8_t		hl;
	/* [한국어] hash length. */
	uint8_t		reserved1;
	uint8_t		cvalid;
	/* [한국어] bit0=1 면 호스트가 양방향 인증을 위한 cval 을 함께 보냄. */
	uint8_t		reserved2;
	uint16_t	dhvlen;
	/* [한국어] 호스트 DH public value 길이. */
	uint32_t	seqnum;
	uint8_t		rval[0];
	/* [한국어] flexible: rval[hl] (응답 해시) 뒤에 cvalid=1 이면 cval[hl],
	 * 그리고 dhv[dhvlen] 가 옴. */
	/* Followed by cval[hl] and dhv[dhvlen] */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_dhchap_reply) == 16, "Incorrect size");

/* [한국어] === DHCHAP_Success1 (타깃→호스트, auth_id=0x03) ===
 * 타깃이 reply 검증 성공 + (양방향) 호스트 챌린지에 대한 응답 R2 전달. */
struct spdk_nvmf_dhchap_success1 {
	uint8_t		auth_type;
	uint8_t		auth_id;
	uint8_t		reserved0[2];
	uint16_t	t_id;
	uint8_t		hl;
	uint8_t		reserved1;
	uint8_t		rvalid;
	/* [한국어] bit0=1 면 rval[] 이 유효 (양방향 인증). */
	uint8_t		reserved2[7];
	uint8_t		rval[0];
	/* [한국어] flexible: 호스트 챌린지에 대한 타깃의 응답 해시. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_dhchap_success1) == 16, "Incorrect size");

/* [한국어] === DHCHAP_Success2 (호스트→타깃, auth_id=0x04) ===
 * 호스트가 success1 의 R2 를 검증한 후 보내는 ack. 인증 종료. */
struct spdk_nvmf_dhchap_success2 {
	uint8_t		auth_type;
	uint8_t		auth_id;
	uint8_t		reserved0[2];
	uint16_t	t_id;
	uint8_t		reserved1[10];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_dhchap_success2) == 16, "Incorrect size");

/** AUTH_Failure reason codes */
#define SPDK_NVMF_AUTH_FAILURE 1
/* [한국어] auth_failure.rc 의 일반 실패 코드. */

/** AUTH_Failure reason code explanations */
/* [한국어] === auth_failure.rce (Reason Code Explanation) === */
enum spdk_nvmf_auth_failure_reason {
	SPDK_NVMF_AUTH_FAILED				= 0x1,
	/* [한국어] 챌린지 응답 검증 실패 (잘못된 PSK 등). */
	SPDK_NVMF_AUTH_PROTOCOL_UNUSABLE		= 0x2,
	/* [한국어] 협상된 인증 프로토콜 사용 불가. */
	SPDK_NVMF_AUTH_SCC_MISMATCH			= 0x3,
	/* [한국어] sc_c 정책 불일치 (요구/제공 미스매치). */
	SPDK_NVMF_AUTH_HASH_UNUSABLE			= 0x4,
	/* [한국어] 협상된 hash 알고리즘 사용 불가. */
	SPDK_NVMF_AUTH_DHGROUP_UNUSABLE			= 0x5,
	/* [한국어] DH group 사용 불가. */
	SPDK_NVMF_AUTH_INCORRECT_PAYLOAD		= 0x6,
	/* [한국어] 메시지 페이로드 길이/내용이 잘못됨. */
	SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE	= 0x7,
	/* [한국어] 시퀀스 위반 (예 challenge 전에 reply 도착). */
};

/* [한국어] === AUTH_Failure (auth_id=0xf0/0xf1) ===
 * 어느 쪽이든 인증 절차 중 오류 시 발신. */
struct spdk_nvmf_auth_failure {
	uint8_t		auth_type;
	uint8_t		auth_id;
	uint8_t		reserved0[2];
	uint16_t	t_id;
	uint8_t		rc;
	/* [한국어] reason code (보통 0x1 SPDK_NVMF_AUTH_FAILURE). */
	uint8_t		rce;
	/* [한국어] reason code explanation (위 enum). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_auth_failure) == 8, "Incorrect size");

/* RDMA Fabric specific definitions below */
/* [한국어] ============================================================
 * 이하 RDMA 트랜스포트 전용 정의. RDMA 는 zero-copy DMA(SEND/RECV/READ/WRITE) 와
 * RDMA-CM private data 를 사용해 호스트-타깃 간 큐페어 정보를 교환한다.
 * ============================================================ */

#define SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY	0xF
/* [한국어] NVMe SGL Descriptor 의 subtype=0xF — RDMA Fabrics 확장.
 * SEND-with-Invalidate 시 타깃이 호스트의 R_Key 를 무효화하도록 지시.
 * NVMe-oF RDMA 1.1 §4.1 — 보안/리소스 회수에 사용. */

/* [한국어] === RDMA-CM 연결 요청 시 호스트가 보내는 private data (32B) ===
 * librdmacm rdma_connect() 의 conn_param.private_data 로 실어 전송. 타깃은
 * RDMA_CM_EVENT_CONNECT_REQUEST 핸들러에서 이를 검사해 qpair 를 수락/거부. */
struct spdk_nvmf_rdma_request_private_data {
	uint16_t	recfmt; /* record format */
	/* [한국어] private data record format — 현재 0. 비매칭 시 reject 의 sts 로
	 * SPDK_NVMF_RDMA_ERROR_INVALID_RECFMT 반환. */
	uint16_t	qid;	/* queue id */
	/* [한국어] 호스트가 원하는 큐 ID — 0=admin, >0=I/O. */
	uint16_t	hrqsize;	/* host receive queue size */
	/* [한국어] Host Receive Queue size (RECV WR 개수). 타깃은 이 값에 맞춰
	 * SEND 발행 속도를 제어. */
	uint16_t	hsqsize;	/* host send queue size */
	/* [한국어] Host Send Queue size (SQ depth). zero-based. */
	uint16_t	cntlid;		/* controller id */
	/* [한국어] qid>0 일 때 admin qpair 의 cntlid (어느 컨트롤러에 붙일지). */
	uint8_t		reserved[22];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_request_private_data) == 32, "Incorrect size");

/* [한국어] === 타깃 → 호스트 accept 시 private data === */
struct spdk_nvmf_rdma_accept_private_data {
	uint16_t	recfmt; /* record format */
	/* [한국어] 0 고정. */
	uint16_t	crqsize;	/* controller receive queue size */
	/* [한국어] 타깃이 광고하는 컨트롤러 RECV 큐 크기 — 호스트의 SEND credit 결정. */
	uint8_t		reserved[28];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_accept_private_data) == 32, "Incorrect size");

/* [한국어] === 타깃 → 호스트 reject 시 private data (4B) === */
struct spdk_nvmf_rdma_reject_private_data {
	uint16_t	recfmt; /* record format */
	/* [한국어] 0. */
	uint16_t	sts; /* status */
	/* [한국어] reject 사유 — spdk_nvmf_rdma_transport_error 값. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_reject_private_data) == 4, "Incorrect size");

/* [한국어] === 위 3종 private data 의 union (32B 슬롯에 맞춤) ===
 * 코드에서 한 번에 다루기 위해 union. */
union spdk_nvmf_rdma_private_data {
	struct spdk_nvmf_rdma_request_private_data	pd_request;
	/* [한국어] CONNECT_REQUEST 이벤트일 때 사용. */
	struct spdk_nvmf_rdma_accept_private_data	pd_accept;
	/* [한국어] ESTABLISHED 이벤트 (accept 측). */
	struct spdk_nvmf_rdma_reject_private_data	pd_reject;
	/* [한국어] REJECTED 이벤트. */
};
SPDK_STATIC_ASSERT(sizeof(union spdk_nvmf_rdma_private_data) == 32, "Incorrect size");

/* [한국어] === RDMA reject 사유 코드 (reject_private_data.sts) === */
enum spdk_nvmf_rdma_transport_error {
	SPDK_NVMF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH	= 0x1,
	/* [한국어] private data 가 32B 가 아님. */
	SPDK_NVMF_RDMA_ERROR_INVALID_RECFMT			= 0x2,
	/* [한국어] recfmt != 0. */
	SPDK_NVMF_RDMA_ERROR_INVALID_QID			= 0x3,
	/* [한국어] qid 가 알려지지 않거나 admin 미연결 상태에서 I/O qid. */
	SPDK_NVMF_RDMA_ERROR_INVALID_HSQSIZE			= 0x4,
	/* [한국어] 호스트 SQ size 가 타깃 한도 초과. */
	SPDK_NVMF_RDMA_ERROR_INVALID_HRQSIZE			= 0x5,
	/* [한국어] 호스트 RQ size 부적절. */
	SPDK_NVMF_RDMA_ERROR_NO_RESOURCES			= 0x6,
	/* [한국어] 타깃 자원 부족 (PD/CQ/MR 등). */
	SPDK_NVMF_RDMA_ERROR_INVALID_IRD			= 0x7,
	/* [한국어] Initiator Read Depth 초과. */
	SPDK_NVMF_RDMA_ERROR_INVALID_ORD			= 0x8,
	/* [한국어] Outstanding Read Depth 초과. */
};

/* TCP transport specific definitions below */
/* [한국어] ============================================================
 * 이하 NVMe/TCP 트랜스포트 전용 정의. RFC "NVMe over TCP" / NVMe TCP 1.0a.
 * RDMA 와 달리 TCP 는 PDU(Protocol Data Unit) 단위로 socket 위에서 직렬화하며,
 * Header Digest(HDGST, CRC32C 4B) 와 Data Digest(DDGST, CRC32C 4B) 로 무결성
 * 보호. ICReq/ICResp 핸드셰이크로 digest/PDA(데이터 정렬) 협상.
 * ============================================================ */

/** NVMe/TCP PDU type */
/* [한국어] === NVMe/TCP PDU Type (common_pdu_hdr.pdu_type) ===
 * NVMe TCP 1.0a §3.4. */
enum spdk_nvme_tcp_pdu_type {
	/** Initialize Connection Request (ICReq) */
	SPDK_NVME_TCP_PDU_TYPE_IC_REQ			= 0x00,
	/* [한국어] 호스트→컨트롤러 첫 PDU — TCP 연결 직후 보내는 협상 요청.
	 * pfv/hpda/dgst/maxr2t 광고. */

	/** Initialize Connection Response (ICResp) */
	SPDK_NVME_TCP_PDU_TYPE_IC_RESP			= 0x01,
	/* [한국어] 컨트롤러→호스트 응답 — pfv/cpda/dgst/maxh2cdata 확정. */

	/** Terminate Connection Request (TermReq) */
	SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ		= 0x02,
	/* [한국어] 호스트→컨트롤러 비정상 종료 통지 (fes 에 사유). */

	/** Terminate Connection Response (TermResp) */
	SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ		= 0x03,
	/* [한국어] 컨트롤러→호스트 비정상 종료 통지. (이름은 "RESP" 류이지만 enum
	 * 명은 _C2H_TERM_REQ — 스펙 PDU type 0x03). */

	/** Command Capsule (CapsuleCmd) */
	SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD		= 0x04,
	/* [한국어] 호스트→컨트롤러 NVMe 명령 캡슐 (64B SQE + 선택 in-capsule data). */

	/** Response Capsule (CapsuleRsp) */
	SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP		= 0x05,
	/* [한국어] 컨트롤러→호스트 NVMe 완료 캡슐 (16B CQE). */

	/** Host To Controller Data (H2CData) */
	SPDK_NVME_TCP_PDU_TYPE_H2C_DATA			= 0x06,
	/* [한국어] 호스트→컨트롤러 쓰기 데이터 PDU (R2T 응답으로). */

	/** Controller To Host Data (C2HData) */
	SPDK_NVME_TCP_PDU_TYPE_C2H_DATA			= 0x07,
	/* [한국어] 컨트롤러→호스트 읽기 데이터 PDU. */

	/** Ready to Transfer (R2T) */
	SPDK_NVME_TCP_PDU_TYPE_R2T			= 0x09,
	/* [한국어] 컨트롤러→호스트 — "datao 부터 datal 바이트만큼 H2CData 로
	 * 보내달라" 요청. RDMA WRITE 의 등가물. */
};

/** Common NVMe/TCP PDU header */
/* [한국어] === 모든 NVMe/TCP PDU 의 공통 헤더 (8B) ===
 * NVMe TCP 1.0a §3.3. PDU 의 첫 8B 는 종류와 길이를 알려주어 receiver 가
 * pdu_type 별 specific header 길이를 결정할 수 있게 한다. */
struct spdk_nvme_tcp_common_pdu_hdr {
	/** PDU type (\ref spdk_nvme_tcp_pdu_type) */
	uint8_t				pdu_type;
	/* [한국어] [0] — PDU 종류 (위 enum). */

	/** pdu_type-specific flags */
	uint8_t				flags;
	/* [한국어] [1] — pdu_type 별 플래그. 공통: bit0=HDGSTF (header digest 존재),
	 * bit1=DDGSTF (data digest 존재). H2C/C2H Data 는 bit2=LAST_PDU,
	 * bit3=SUCCESS (성공 시 별도 CapsuleRsp 생략). */

	/** Length of PDU header (not including the Header Digest) */
	uint8_t				hlen;
	/* [한국어] [2] — header 길이 (HDGST 4B 제외). receiver 는 hlen 만큼 읽고
	 * HDGSTF 면 추가 4B 다이제스트를 읽어 CRC32C 검증. */

	/** PDU Data Offset from the start of the PDU */
	uint8_t				pdo;
	/* [한국어] [3] — PDU 시작부터 data 까지의 offset (data PDU 에서 의미).
	 * cpda 협상값(4 의 배수)에 정렬. data PDU 가 아닌 경우 0. */

	/** Total number of bytes in PDU, including pdu_hdr */
	uint32_t			plen;
	/* [한국어] [4..7] — 전체 PDU 길이 (header + (HDGST?) + data + (DDGST?)).
	 * receiver 가 다음 PDU 경계 결정에 사용. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_tcp_common_pdu_hdr) == 8, "Incorrect size");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdu_type) == 0,
		   "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_common_pdu_hdr, flags) == 1, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_common_pdu_hdr, hlen) == 2, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdo) == 3, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen) == 4, "Incorrect offset");
/* [한국어] 모든 PDU 의 첫 8B 가 정확한 위치에 있어야 — 트랜스포트 코드는 raw socket
 * read 후 이 offset 으로 직접 캐스트. */

#define SPDK_NVME_TCP_CH_FLAGS_HDGSTF		(1u << 0)
/* [한국어] common.flags bit0 — Header Digest 활성. ICReq/ICResp 에서 협상되며
 * 협상 후 모든 PDU 에 동일하게 적용. CRC32C 4B 가 hlen 직후. */
#define SPDK_NVME_TCP_CH_FLAGS_DDGSTF		(1u << 1)
/* [한국어] common.flags bit1 — Data Digest 활성. 데이터 영역 끝에 CRC32C 4B. */

/**
 * ICReq
 *
 * common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_REQ
 */
/* [한국어] === ICReq (Initialize Connection Request) PDU (128B) ===
 * TCP 핸드셰이크: 호스트가 TCP socket 연결 직후 첫 PDU 로 발행. 컨트롤러는 ICResp
 * 로 답해야 한다. 이 핸드셰이크 전에는 어떤 NVMe 명령도 전송하지 않는다. */
struct spdk_nvme_tcp_ic_req {
	struct spdk_nvme_tcp_common_pdu_hdr	common;
	/* [한국어] common.pdu_type=0x00, hlen=128, plen=128 (data 없음). */

	uint16_t				pfv;
	/* [한국어] PDU Format Version (offset 8..9) — 현재 0. 비매칭 시 컨트롤러
	 * TermReq 로 종료. */

	/** Specifies the data alignment for all PDUs transferred from the controller to the host that contain data */
	uint8_t					hpda;
	/* [한국어] Host PDA (offset 10) — 호스트가 받을 C2H Data PDU 의 pdo 정렬
	 * 단위 -1 (host data PDU alignment 0=4B, 1=8B, ..., 31=128B).
	 * SPDK_NVME_TCP_HPDA_MAX=31. */

	union {
		uint8_t				raw;
		struct {
			uint8_t			hdgst_enable : 1;
			/* [한국어] dgst.bits.hdgst_enable — 호스트가 헤더 digest 사용 의향. */
			uint8_t			ddgst_enable : 1;
			/* [한국어] dgst.bits.ddgst_enable — 데이터 digest 사용 의향. */
			uint8_t			reserved : 6;
		} bits;
	} dgst;
	/* [한국어] Digest Types (offset 11) — bits.* 로 광고. 컨트롤러가 ICResp 에서
	 * 자신도 지원해야 최종 활성. */

	uint32_t				maxr2t;
	/* [한국어] Maximum R2T (offset 12..15) — 한 명령당 동시에 발생 가능한 R2T
	 * 개수 -1. 0 = 1개. write multipath/parallel transfer 제어. */

	uint8_t					reserved16[112];
	/* [한국어] reserved (offset 16..127) — ICReq 를 128B 로 패딩. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_tcp_ic_req) == 128, "Incorrect size");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_ic_req, pfv) == 8, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_ic_req, hpda) == 10, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_ic_req, maxr2t) == 12, "Incorrect offset");

#define SPDK_NVME_TCP_HPDA_MAX 31
/* [한국어] hpda 최대값 — 32 단위 정렬 (실제 (hpda+1)*4B). */
#define SPDK_NVME_TCP_CPDA_MAX 31
/* [한국어] cpda 최대값 — ICResp 의 controller PDA 동일 한도. */
#define SPDK_NVME_TCP_PDU_PDO_MAX_OFFSET     ((SPDK_NVME_TCP_CPDA_MAX + 1) << 2)
/* [한국어] pdo (PDU data offset) 최대값 = (CPDA_MAX+1)*4 = 128B.
 * 즉 data 까지 최대 128B 헤더+패딩 가능. */

/**
 * ICResp
 *
 * common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP
 */
/* [한국어] === ICResp PDU (128B) ===
 * 컨트롤러→호스트 핸드셰이크 응답. 협상 결과 확정. */
struct spdk_nvme_tcp_ic_resp {
	struct spdk_nvme_tcp_common_pdu_hdr	common;
	/* [한국어] pdu_type=0x01. */

	uint16_t				pfv;
	/* [한국어] PDU Format Version (현재 0). */

	/** Specifies the data alignment for all PDUs transferred from the host to the controller that contain data */
	uint8_t					cpda;
	/* [한국어] Controller PDA (offset 10) — H2C Data PDU 의 pdo 정렬 단위.
	 * 호스트는 H2CData 보낼 때 pdo 를 (cpda+1)*4 의 배수로 맞춤. */

	union {
		uint8_t				raw;
		struct {
			uint8_t			hdgst_enable : 1;
			/* [한국어] 컨트롤러가 최종 동의한 header digest 활성 여부. */
			uint8_t			ddgst_enable : 1;
			/* [한국어] data digest 최종 활성. */
			uint8_t			reserved : 6;
		} bits;
	} dgst;

	/** Specifies the maximum number of PDU-Data bytes per H2C Data Transfer PDU */
	uint32_t				maxh2cdata;
	/* [한국어] MAXH2CDATA (offset 12..15, 4의 배수, ≥4096) — 컨트롤러가 한 번에
	 * 받을 수 있는 H2C Data PDU 의 최대 데이터 크기. R2T 의 r2tl 도 이 값을 넘지 않음. */

	uint8_t					reserved16[112];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_tcp_ic_resp) == 128, "Incorrect size");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_ic_resp, pfv) == 8, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_ic_resp, cpda) == 10, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_ic_resp, maxh2cdata) == 12, "Incorrect offset");

/**
 * TermReq
 *
 * common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_TERM_REQ
 */
/* [한국어] === TermReq/TermResp 공통 헤더 (24B) ===
 * 비정상 종료 통지. 발신자는 사유(fes/fei)를 채우고 추가 진단 데이터를 최대 128B
 * 까지 붙일 수 있음 (총 152B 한도). */
struct spdk_nvme_tcp_term_req_hdr {
	struct spdk_nvme_tcp_common_pdu_hdr	common;
	/* [한국어] pdu_type=0x02 (H2C) 또는 0x03 (C2H). */

	uint16_t				fes;
	/* [한국어] Fatal Error Status (offset 8..9) — spdk_nvme_tcp_term_req_fes
	 * 값. 어떤 종류의 오류인지. */

	uint8_t					fei[4];
	/* [한국어] Fatal Error Information (offset 10..13, 4B) — fes 에 대한 추가
	 * 정보. 예 invalid header field 의 offset. */

	uint8_t					reserved14[10];
	/* [한국어] reserved (offset 14..23). 헤더는 24B 고정. */
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_tcp_term_req_hdr) == 24, "Incorrect size");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_term_req_hdr, fes) == 8, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_term_req_hdr, fei) == 10, "Incorrect offset");

/* [한국어] === TermReq fes 값 (Fatal Error Status) ===
 * NVMe TCP 1.0a Table "Fatal Error Status". */
enum spdk_nvme_tcp_term_req_fes {
	SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD				= 0x01,
	/* [한국어] PDU 헤더 필드 값이 invalid. fei 에 위반 offset. */
	SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR				= 0x02,
	/* [한국어] PDU 시퀀스 위반 (예: ICReq 전에 CapsuleCmd). */
	SPDK_NVME_TCP_TERM_REQ_FES_HDGST_ERROR					= 0x03,
	/* [한국어] header digest CRC32C 미스매치. */
	SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE			= 0x04,
	/* [한국어] datao+datal 이 명령 transfer length 초과. */
	SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED			= 0x05,
	/* [한국어] 한 transfer 가 maxh2cdata 초과. */
	SPDK_NVME_TCP_TERM_REQ_FES_R2T_LIMIT_EXCEEDED				= 0x05,
	/* [한국어] 동시 R2T 가 maxr2t 초과. (값 0x05 가 의미상 둘로 매핑.) */
	SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER		= 0x06,
	/* [한국어] 지원하지 않는 PDU 파라미터. */
};

/* Total length of term req PDU (including PDU header and DATA) in bytes shall not exceed a limit of 152 bytes. */
#define SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE	128
/* [한국어] TermReq PDU 의 추가 진단 데이터 최대 길이. */
#define SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE		(SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE + sizeof(struct spdk_nvme_tcp_term_req_hdr))
/* [한국어] TermReq PDU 전체 최대 크기 (24B 헤더 + 128B 데이터 = 152B). */

/**
 * CapsuleCmd
 *
 * common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD
 */
/* [한국어] === Command Capsule PDU ===
 * NVMe 명령(64B SQE) 을 TCP 위에 실어 전달. In-Capsule Data 가 있다면 ccsqe 뒤,
 * pdo 로 정렬된 위치부터 채워짐. 명령 직후 컨트롤러가 R2T 또는 CapsuleRsp 응답. */
struct spdk_nvme_tcp_cmd {
	struct spdk_nvme_tcp_common_pdu_hdr	common;
	/* [한국어] pdu_type=0x04. */

	struct spdk_nvme_cmd			ccsqe;
	/* [한국어] Command Capsule SQE (offset 8..71, 64B) — NVMe Base 의 SQE
	 * 그대로. fabrics 명령(opcode 0x7F) 도 이 위치에 들어감. */

	/**< icdoff hdgst padding + in-capsule data + ddgst (if enabled) */
	/* [한국어] 뒤따르는 영역: HDGST(4B if HDGSTF) + ICDoFF padding (controller
	 * Identify 의 ICDoFF 필드로 광고) + In-Capsule Data + DDGST(4B if DDGSTF). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_tcp_cmd) == 72, "Incorrect size");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_cmd, ccsqe) == 8, "Incorrect offset");

/**
 * CapsuleResp
 *
 * common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP
 */
/* [한국어] === Response Capsule PDU ===
 * 명령 완료 CQE 를 PDU 로 회신. C2HData 의 SUCCESS flag 가 set 이면 별도
 * CapsuleRsp 없이 마지막 C2HData PDU 로 완료를 piggyback (지연 단축). */
struct spdk_nvme_tcp_rsp {
	struct spdk_nvme_tcp_common_pdu_hdr	common;
	/* [한국어] pdu_type=0x05. */

	struct spdk_nvme_cpl			rccqe;
	/* [한국어] Response Capsule CQE (offset 8..23, 16B). cid 로 명령 매칭. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_tcp_rsp) == 24, "incorrect size");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_rsp, rccqe) == 8, "Incorrect offset");


/**
 * H2CData
 *
 * hdr.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_DATA
 */
/* [한국어] === H2C Data PDU 헤더 (24B) ===
 * 호스트→컨트롤러 쓰기 데이터. R2T 응답으로 발생. 한 R2T 에 대해 여러 H2CData
 * PDU 로 분할 가능. 마지막 PDU 는 LAST_PDU 플래그 set. */
struct spdk_nvme_tcp_h2c_data_hdr {
	struct spdk_nvme_tcp_common_pdu_hdr	common;
	/* [한국어] pdu_type=0x06. */

	uint16_t				cccid;
	/* [한국어] Command Capsule CID (offset 8..9) — 어떤 명령에 대한 데이터인지. */

	uint16_t				ttag;
	/* [한국어] Transfer Tag (offset 10..11) — R2T 가 발급한 transfer 식별자.
	 * 호스트는 R2T 의 ttag 를 그대로 echo. 한 명령 내 여러 R2T 시 구분. */

	uint32_t				datao;
	/* [한국어] Data Offset (offset 12..15) — 명령의 transfer 시작점부터 이 PDU
	 * 데이터 시작까지의 byte offset. */

	uint32_t				datal;
	/* [한국어] Data Length (offset 16..19) — 이 PDU 의 데이터 바이트 수. */

	uint8_t					reserved20[4];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_tcp_h2c_data_hdr) == 24, "Incorrect size");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_h2c_data_hdr, cccid) == 8, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_h2c_data_hdr, ttag) == 10, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_h2c_data_hdr, datao) == 12, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_h2c_data_hdr, datal) == 16, "Incorrect offset");

#define SPDK_NVME_TCP_H2C_DATA_FLAGS_LAST_PDU	(1u << 2)
/* [한국어] H2CData common.flags bit2 — 이번 R2T 에 대한 마지막 H2CData PDU.
 * 컨트롤러는 이를 받아야 transfer 완료로 간주. */
#define SPDK_NVME_TCP_H2C_DATA_FLAGS_SUCCESS	(1u << 3)
/* [한국어] H2CData common.flags bit3 — (현재 호스트 측에서는 보통 사용하지 않음;
 * C2H 와 대칭성을 위해 정의). */
#define SPDK_NVME_TCP_H2C_DATA_PDO_MULT		8u
/* [한국어] H2CData 의 pdo 는 8의 배수여야 함 (cpda 와 별도로 8B align 보장). */

/**
 * C2HData
 *
 * hdr.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_DATA
 */
/* [한국어] === C2H Data PDU 헤더 (24B) ===
 * 컨트롤러→호스트 읽기 데이터. ttag 가 없는 이유: 읽기는 명령 자체가 transfer
 * 출처라 별도 R2T 가 없음. SUCCESS 플래그로 마지막 PDU 가 완료까지 알릴 수 있음. */
struct spdk_nvme_tcp_c2h_data_hdr {
	struct spdk_nvme_tcp_common_pdu_hdr	common;
	/* [한국어] pdu_type=0x07. */

	uint16_t				cccid;
	/* [한국어] Command Capsule CID (offset 8..9) — 어느 read 명령인지. */

	uint8_t					reserved10[2];
	/* [한국어] (H2CData 의 ttag 자리에 해당 — C2HData 는 R2T 가 없음). */

	uint32_t				datao;
	/* [한국어] Data Offset — read transfer 내 byte offset. */

	uint32_t				datal;
	/* [한국어] Data Length — 이 PDU 의 데이터 길이. */

	uint8_t					reserved20[4];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_tcp_c2h_data_hdr) == 24, "Incorrect size");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_c2h_data_hdr, cccid) == 8, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_c2h_data_hdr, datao) == 12, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_c2h_data_hdr, datal) == 16, "Incorrect offset");

#define SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS	(1u << 3)
/* [한국어] C2HData common.flags bit3 — 이 PDU 가 마지막이며 명령이 성공 종료됨을
 * piggyback. 호스트는 이 플래그를 보면 별도 CapsuleRsp 를 기다리지 않음 (RTT 절약).
 * 컨트롤러는 LAST_PDU 와 함께 SUCCESS 를 set 가능 (controller-side optimization). */
#define SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU	(1u << 2)
/* [한국어] 이 PDU 가 read transfer 의 마지막. */
#define SPDK_NVME_TCP_C2H_DATA_PDO_MULT		8u
/* [한국어] C2HData pdo 는 8B 배수. */

/**
 * R2T
 *
 * common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_R2T
 */
/* [한국어] === R2T (Ready to Transfer) PDU (24B) ===
 * 컨트롤러→호스트 — write 명령에 대해 "지금 r2to..r2to+r2tl 영역의 데이터를
 * H2CData 로 보내라" 요청. ttag 로 transfer 식별 (한 명령에 multiple R2T 가능).
 * RDMA 의 RDMA_WRITE 등가물 (단 TCP 에서는 항상 컨트롤러가 pull 로 요청). */
struct spdk_nvme_tcp_r2t_hdr {
	struct spdk_nvme_tcp_common_pdu_hdr	common;
	/* [한국어] pdu_type=0x09. */

	uint16_t				cccid;
	/* [한국어] 어느 write 명령에 대한 R2T 인지. */

	uint16_t				ttag;
	/* [한국어] Transfer Tag — 컨트롤러가 새로 부여. 호스트는 H2CData PDU 들에서
	 * 이 ttag 를 echo. 동일 명령에 multiple R2T 시 매칭 키. */

	uint32_t				r2to;
	/* [한국어] R2T Offset — transfer 시작 offset (byte). */

	uint32_t				r2tl;
	/* [한국어] R2T Length — 요청 바이트 수 (≤ maxh2cdata 권장). */

	uint8_t					reserved20[4];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_tcp_r2t_hdr) == 24, "Incorrect size");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_r2t_hdr, cccid) == 8, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_r2t_hdr, ttag) == 10, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_r2t_hdr, r2to) == 12, "Incorrect offset");
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvme_tcp_r2t_hdr, r2tl) == 16, "Incorrect offset");

#pragma pack(pop)
/* [한국어] pack(push,1) 해제 — 본 헤더 밖에서는 컴파일러 기본 정렬 복원.
 * 누락 시 이후 인클루드된 헤더의 구조체 정렬이 1바이트로 강제되어 ABI 가 깨짐. */

#ifdef __cplusplus
}
/* [한국어] extern "C" 닫기. */
#endif

#endif /* __NVMF_SPEC_H__ */
/* [한국어] 헤더 가드 종료. (주석의 "__NVMF_SPEC_H__" 는 실제 매크로명
 * SPDK_NVMF_SPEC_H 와 다른 historical 표기 — 가드 동작 자체는 영향 없음.) */
