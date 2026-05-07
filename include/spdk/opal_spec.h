/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] TCG Opal SSC 2.0 와이어 프로토콜 상수/enum/구조체 정의 (opal_spec.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 TCG(Trusted Computing Group) Storage Architecture Core Specification(SSC) 2.0
 * 및 Opal SSC 2.0의 와이어 레벨 프로토콜에서 호스트가 디바이스(SED, Self-Encrypting Drive)와
 * 주고받는 모든 바이트 단위 상수, 토큰 식별자, 디스커버리 헤더, ComPacket/Packet/Subpacket
 * 구조체를 한 파일에 모아놓은 "스펙 헤더"이다. SPDK는 이 정의들을 사용하여 NVMe Security Send
 * (opcode 0x81) / Security Receive(0x82) 명령의 SECP(Security Protocol) 0x01("TCG") 페이로드를
 * 빌드·파싱한다. 즉, 이 헤더는 코드 로직을 갖지 않고 스펙 그대로의 매직 넘버/비트 레이아웃을
 * C 매크로와 packed struct로 노출하는 "데이터 시트" 역할만 수행한다. 실제 토큰 빌드/세션 관리
 * 로직은 lib/nvme/nvme_opal.c가 담당하며, 그 코드가 이 매크로들을 참조하여 SWG 토큰 스트림을
 * 직렬화한다. 결과적으로 이 헤더가 TCG SWG(Storage Working Group) 스펙과 SPDK Opal 구현 사이의
 * 단일 진실 소스(single source of truth)가 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK SED/Opal 스택은 다음 계층으로 구성된다:
 *   [상위] app/spdk_opal_user (또는 RPC 호출자)
 *     → lib/nvme/nvme_opal_internal.h (호스트 측 세션 상태/명령 컨텍스트)
 *     → lib/nvme/nvme_opal.c (CALL/세션 빌더, ComPacket 조립, 응답 파싱)
 *     → include/spdk/opal_spec.h ← (이 파일) — 모든 와이어 상수의 정의처
 *     → lib/nvme/nvme.c::spdk_nvme_ctrlr_security_send/receive (NVMe admin queue submit)
 *     → NVMe SSD 펌웨어 내부의 TPer (Trusted Peripheral) → SP (Security Provider) 핸들러
 * 이 헤더 자체는 코드를 실행하지 않으며, 호스트 유저스페이스 컨파일 단계에서만 등장한다.
 * 단, packed struct들은 NVMe 데이터 버퍼(host→device DMA)에 그대로 직렬화되어 PCIe TLP를 통해
 * 디바이스로 전송된다. 따라서 모든 multi-byte 정수 필드는 TCG가 정의한 "big-endian on the wire"
 * 규약을 따라야 하며, 호스트가 little-endian 아키텍처라면 직렬화 단계에서 to_be16/to_be32
 * 변환을 거친 뒤 이 구조체에 기록해야 한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존성 (#include): "spdk/stdinc.h" (uint8_t/uint16_t/uint32_t/uint64_t 등 표준 정수 타입),
 *   "spdk/assert.h" (SPDK_STATIC_ASSERT — 컴파일 타임에 packed struct 크기를 스펙과 비교).
 * - 이 파일을 포함하는 모듈:
 *     · lib/nvme/nvme_opal.c — TCG SWG 토큰 스트림 빌더/파서. 모든 매크로(SPDK_TINY_ATOM_*,
 *       SPDK_OPAL_STARTLIST 등)와 enum 값(spdk_opal_token)을 직접 wire에 기록.
 *     · lib/nvme/nvme_opal_internal.h — 세션 상태(HSN/TSN, COMID)와 함께 일부 enum 사용.
 *     · app/spdk_opal/(있다면) opal_user.c — 사용자 명령 입력 → opal_spec.h 토큰으로 매핑.
 * - 데이터 흐름: app → opal_user 명령 → nvme_opal.c가 STARTLIST/CALL/STARTNAME/EOS 등 토큰을
 *   순서대로 쓴다 → struct spdk_opal_compacket (CommPacket) 헤더 + struct spdk_opal_packet
 *   (Packet 헤더, HSN/TSN 포함) + struct spdk_opal_data_subpacket (Subpacket 헤더) + 토큰 페이로드를
 *   하나의 DMA 버퍼에 packed 레이아웃으로 직렬화 → NVMe Security Send로 디바이스 전송 → 디바이스
 *   응답을 Security Receive로 회수 → 동일 packet 구조체를 통해 역파싱 → 호스트가 결과 토큰 디코드.
 * - 공유 자료구조: 호스트의 세션 핸드셰이크 상태(HSN/TSN)는 nvme_opal_internal.h가 보관하고,
 *   이 파일의 struct spdk_opal_packet::session_hsn/tsn 필드가 그 값을 wire에 기록한다.
 *
 * === 주요 함수/구조체 요약 ===
 * 이 헤더는 함수 정의가 없고, 다음 핵심 컴포넌트로 구성된다:
 *   1) Atom 인코딩 매크로 (SPDK_TINY/SHORT/MEDIUM/LONG_ATOM_*) — TCG 5.1.3.1 토큰 인코딩
 *      규칙. 첫 바이트의 비트 패턴으로 atom 종류를 판별하고 페이로드 길이를 결정.
 *   2) enum spdk_lv0_discovery_feature_code — Level 0 Discovery 응답에서 디바이스가 보고하는
 *      Feature Code(TPer/Locking/Geometry/Opal v1/Opal v2/SingleUser/DataStore).
 *   3) enum spdk_opal_token — TCG SWG의 모든 control token(STARTLIST/CALL/EOS/...)과 자주 쓰는
 *      Locking SP 컬럼/메서드 인덱스. 토큰 스트림 작성 시 직접 1바이트로 wire에 기록.
 *   4) struct spdk_opal_d0_hdr / spdk_opal_d0_feat_hdr / spdk_opal_d0_*_feat — Level 0 Discovery
 *      응답(보통 1KB)의 헤더와 feature descriptor들. 디바이스 capability 디코딩에 사용.
 *   5) struct spdk_opal_compacket / spdk_opal_packet / spdk_opal_data_subpacket — 호스트와
 *      디바이스가 한 번 주고받는 한 transmission unit의 3-layer 헤더. 모두 big-endian wire format.
 *
 * === TCG Opal 와이어 핸드셰이크 한 눈에 보기 ===
 *   [Discovery]       Security Recv(COMID=0x01) → struct spdk_opal_d0_hdr + N×feat_hdr →
 *                     base_comid·v200·locking 등으로 디바이스 capability 파악
 *   [StartSession]    Security Send: ComPacket+Packet(HSN=host random, TSN=0)+Subpacket+
 *                     [STARTLIST 0xF0][CALL 0xF8][SMUid][StartSession_method_uid][...args][EOD][EOS]
 *                     → 디바이스가 TSN 부여하여 응답
 *   [CALL on object]  Security Send: 같은 구조에 HSN/TSN=세션 키, CALL <obj_uid> <method_uid> ...
 *   [EndSession]      [STARTLIST][ENDOFSESSION 0xFA][ENDLIST] 단독 토큰
 *   각 단계의 응답에는 Status code(SUCCESS=0, NOT_AUTHORIZED, AUTHORITY_LOCKED_OUT, ABORTED ...)가
 *   end_data 직전 atom으로 실려 온다.
 */

#ifndef SPDK_OPAL_SPEC_H
/* [한국어] 헤더 다중 포함 가드 — 동일 컴파일 단위에서 두 번 처리되어
 *          packed struct 재정의 에러가 발생하지 않도록 보호. */
#define SPDK_OPAL_SPEC_H

#include "spdk/stdinc.h"
/* [한국어] uint8_t/uint16_t/uint32_t/uint64_t 같은 고정폭 정수 타입과 size_t,
 *          stdbool.h 등 SPDK가 표준 라이브러리를 한 번에 import하는 진입 헤더.
 *          TCG 스펙은 모든 wire 필드 크기를 정확히 지정하므로 고정폭 타입이 필수. */
#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로 제공 — 컴파일 타임에 packed struct의
 *          sizeof()가 TCG 스펙이 명시한 바이트 수와 일치하는지 검증한다.
 *          이 검증이 실패하면 이 헤더는 wire format을 잘못 정의한 것으로 간주되어
 *          빌드 자체가 실패한다 (런타임에 잘못된 패킷이 전송되는 사고를 차단). */

#ifdef __cplusplus
/* [한국어] C++에서 이 헤더를 포함하는 경우, 내부 선언을 C linkage로 묶어
 *          name mangling을 막는다 (lib/nvme의 C 함수가 C++ 호출자와 호환되어야 하기 때문). */
extern "C" {
#endif

/*
 * TCG Storage Architecture Core Spec v2.01 r1.00
 * 3.2.2.3 Tokens
 */
/* [한국어] === Atom(원자) 인코딩 식별 매크로 ===
 *          TCG SWG 토큰 스트림은 5종의 atom으로 구성된다:
 *            tiny atom   : 1바이트  — 0x00~0x7F  (signed/unsigned 6bit 정수)
 *            short atom  : 1+N바이트 — 0x80~0xBF  (최대 15바이트 페이로드, byte/integer)
 *            medium atom : 2+N바이트 — 0xC0~0xDF  (최대 2047바이트)
 *            long atom   : 4+N바이트 — 0xE0~0xE3  (최대 16MB-1)
 *            control     : 1바이트  — 0xF0~0xFF  (STARTLIST/CALL/EOS 등 — enum spdk_opal_token)
 *          첫 바이트의 상위 비트만으로 종류 판별이 가능하도록 설계됨.
 *          아래 _MAX 매크로는 "어디까지 이 종류인가"의 상한 경계를 나타낸다 (즉 first_byte <= MAX). */

#define SPDK_TINY_ATOM_TYPE_MAX			0x7F
/* [한국어] Tiny atom 첫 바이트 상한.
 *  - 역할: first_byte <= 0x7F 이면 tiny atom 판정 (즉 최상위 비트가 0).
 *    페이로드 = 첫 바이트의 하위 6비트(부호확장 여부는 SIGN_FLAG로 결정).
 *  - 사용처: 호스트·디바이스의 SWG 토큰 디코더가 atom 종류 판별 분기에 사용.
 *  - 값 범위: 0x7F 고정. 동기화: 컴파일타임 상수. */
#define SPDK_SHORT_ATOM_TYPE_MAX		0xBF
/* [한국어] Short atom 첫 바이트 상한.
 *  - 역할: 0x80..0xBF 이면 short atom. 헤더 1B + 페이로드 0..15B.
 *  - 사용처: UID 8바이트, 짧은 PIN, 정수 등 가장 흔한 데이터 인코딩.
 *  - 값 범위: 0xBF 고정. 동기화: 상수. */
#define SPDK_MEDIUM_ATOM_TYPE_MAX		0xDF
/* [한국어] Medium atom 첫 바이트 상한.
 *  - 역할: 0xC0..0xDF 이면 medium atom. 헤더 2B + 페이로드 0..2047B.
 *  - 사용처: short atom으로 못 담는 중간 크기 데이터(긴 PIN 등).
 *  - 값 범위: 0xDF 고정. 동기화: 상수. */
#define SPDK_LONG_ATOM_TYPE_MAX			0xE3
/* [한국어] Long atom 첫 바이트 상한.
 *  - 역할: 0xE0..0xE3 이면 long atom. 헤더 4B + 페이로드 ~16MB-1.
 *  - 사용처: MBR shadow 이미지 등 대용량 페이로드 한 atom 에 담을 때.
 *  - 값 범위: 0xE3 고정. 0xE4 이상은 모두 control token(enum spdk_opal_token).
 *  - 동기화: 상수. */

#define SPDK_TINY_ATOM_SIGN_FLAG		0x40
/* [한국어] Tiny atom signed/unsigned 표시 비트.
 *  - 역할: 1=signed integer(6비트 부호확장), 0=unsigned(0..63).
 *  - 설정자: 호스트 토큰 빌더가 정수 인코딩 시 결정. 읽는 자: 토큰 디코더.
 *  - 값 범위: 0x40. 동기화: 상수. (TCG SAC v2.01 5.1.3.1.1). */
#define SPDK_TINY_ATOM_DATA_MASK		0x3F
/* [한국어] Tiny atom 페이로드 추출 마스크.
 *  - 역할: (first_byte & 0x3F) ⇒ 6비트 정수 값(0..63 또는 부호확장 시 -32..31).
 *  - 사용처: 토큰 디코더가 첫 바이트에서 값 분리.
 *  - 값 범위: 0x3F. 동기화: 상수. */

#define SPDK_SHORT_ATOM_ID			0x80
/* [한국어] Short atom 시그니처 비트.
 *  - 역할: (first & 0xC0) == 0x80 이면 short atom. 토큰 빌더가 헤더 첫 바이트 OR 시 사용.
 *  - 값 범위: 0x80. 동기화: 상수. */
#define SPDK_SHORT_ATOM_BYTESTRING_FLAG		0x20
/* [한국어] Short atom byte-string vs integer 구분 비트(첫 바이트 비트5).
 *  - 역할: 1=byte-string(UID 8B/PIN 등 이진 데이터), 0=정수.
 *  - 설정자: 호스트 토큰 빌더가 wire 인코딩 시 결정. 읽는 자: 디코더.
 *  - 값 범위: 0x20. 동기화: 상수. */
#define SPDK_SHORT_ATOM_SIGN_FLAG		0x10
/* [한국어] Short atom integer 부호 비트.
 *  - 역할: BYTESTRING_FLAG=0일 때만 의미. 1=signed, 0=unsigned.
 *    BYTESTRING_FLAG=1 이면 이 비트는 reserved.
 *  - 값 범위: 0x10. 동기화: 상수. */
#define SPDK_SHORT_ATOM_LEN_MASK		0x0F
/* [한국어] Short atom 페이로드 길이 마스크(첫 바이트 하위 4비트).
 *  - 역할: 길이 = first_byte & 0x0F (0..15 바이트). UID 길이는 8 고정.
 *  - 값 범위: 0x0F. 동기화: 상수. */

#define SPDK_MEDIUM_ATOM_ID			0xC0
/* [한국어] Medium atom 시그니처 비트.
 *  - 역할: (first & 0xE0) == 0xC0 이면 medium atom. 헤더 2B(첫 바이트 플래그+길이상위3비트, 둘째 바이트 길이하위8비트).
 *  - 값 범위: 0xC0. 동기화: 상수. */
#define SPDK_MEDIUM_ATOM_BYTESTRING_FLAG	0x10
/* [한국어] Medium atom byte-string 비트(첫 바이트 비트4).
 *  - 역할: 1=byte-string, 0=integer.
 *  - 사용처: 큰 PIN/DataStore 페이로드 wire 인코딩.
 *  - 값 범위: 0x10. 동기화: 상수. */

#define SPDK_MEDIUM_ATOM_SIGN_FLAG		0x08
/* [한국어] Medium atom 정수 부호 비트.
 *  - 역할: 1=signed, 0=unsigned. 값 범위: 0x08. 동기화: 상수. */
#define SPDK_MEDIUM_ATOM_LEN_MASK		0x07
/* [한국어] Medium atom 길이 상위 3비트 마스크(첫 바이트).
 *  - 역할: 전체 길이 = ((first & 0x07) << 8) | second_byte → 0..2047 바이트.
 *  - 값 범위: 0x07. 동기화: 상수. */

#define SPDK_LONG_ATOM_ID			0xE0
/* [한국어] Long atom 시그니처 비트.
 *  - 역할: (first & 0xFC) == 0xE0 이면 long atom. 헤더 4B(플래그 + 24비트 BE 길이).
 *  - 값 범위: 0xE0. 동기화: 상수. */
#define SPDK_LONG_ATOM_BYTESTRING_FLAG		0x02
/* [한국어] Long atom byte-string 비트(첫 바이트 비트1).
 *  - 역할: 1=byte-string, 0=integer. MBR 이미지 등 대용량 byte-string에 사용.
 *  - 값 범위: 0x02. 동기화: 상수. */
#define SPDK_LONG_ATOM_SIGN_FLAG		0x01
/* [한국어] Long atom 정수 부호 비트.
 *  - 역할: 거의 사용되지 않음(>16비트 정수는 드뭄). 1=signed, 0=unsigned.
 *  - 값 범위: 0x01. 동기화: 상수. */

/*
 * TCG Storage Architecture Core Spec v2.01 r1.00
 * Table-26 ComID management
 */
#define LV0_DISCOVERY_COMID			0x01
/* [한국어] Level 0 Discovery 전용 예약 ComID.
 *  - 역할: NVMe Security Receive(opcode 0x82)의 SECP=0x01(TCG), SP_SPECIFIC=0x0001 호출 시
 *    디바이스가 1KB 디스커버리 응답(struct spdk_opal_d0_hdr + N×feature descriptor)을 반환.
 *  - 설정자: TCG SAC Table-26이 정의한 고정 예약 값(컴파일타임 상수).
 *  - 읽는 자: 호스트 lib/nvme/nvme_opal.c::opal_init_discovery 가 ComPacket::comid에 채움.
 *  - 값 범위: 0x01 고정. 동기화: 상수.
 *  - 비고: 정식 세션은 v200_feat.base_comid로 전환되며 LV0_DISCOVERY_COMID는 capability
 *    조회 전용으로만 사용. */

/*
 * TCG Storage Opal v2.01 r1.00
 * 5.2.3 Type Table Modification
 */
#define OPAL_MANUFACTURED_INACTIVE		0x08
/* [한국어] Locking SP의 LifeCycle 컬럼 값 "Manufactured-Inactive".
 *  - 역할: 출하 직후 Locking SP가 비활성 상태임을 의미. Activate 메서드 호출 후에야
 *    Range/Authority 사용 가능.
 *  - 설정자: 디바이스 펌웨어가 출하 시 SP row::LifeCycle 컬럼에 기록.
 *  - 읽는 자: 호스트가 Get(SP, LifeCycle) 응답을 이 값과 비교 → Activate 필요성 판단.
 *  - 값 범위: 0x08 (TCG Opal SSC v2.01 5.2.3 LifeCycle enum).
 *  - 동기화: 상수. */

#define LOCKING_RANGE_NON_GLOBAL		0x03
/* [한국어] 일반(non-Global) Locking Range 식별 표식.
 *  - 역할: LockingTable에서 row 0(GlobalRange)이 아닌 row 1..MAX 사용자 Range를 가리키는 매직 값.
 *    호스트 코드가 "이 row가 일반 Range인가 GlobalRange인가" 분기에 사용.
 *  - 설정자/읽는 자: 호스트 lib/nvme/nvme_opal.c.
 *  - 값 범위: 0x03 (Opal SSC 5.7.2.2). 동기화: 상수. */

#define SPDK_OPAL_MAX_PASSWORD_SIZE		32 /* in bytes */
/* [한국어] SPDK Opal 패스워드 버퍼 상한(32바이트).
 *  - 역할: C_PIN_SID/Admin1/User1 등 모든 PIN 입력의 SPDK 운영 한도.
 *    TCG 스펙은 더 큰 PIN을 허용하지만 SPDK는 RPC/UI 검증을 위해 32B로 제한.
 *  - 설정자: 컴파일타임 상수. 읽는 자: 호스트 RPC 파서, PIN 비교 함수.
 *  - 값 범위: 32 고정. 동기화: 상수. */

#define SPDK_OPAL_MAX_LOCKING_RANGE		8 /* maximum 8 ranges defined  by spec */
/* [한국어] SPDK가 관리하는 Locking Range 최대 개수.
 *  - 역할: Opal SSC 2.0 의무 최소 Range 수 = Range 0(Global) + Range 1..7 = 8.
 *    SPDK는 이 상한까지만 관리하며 런타임 인덱스 검증에 사용.
 *  - 설정자: 컴파일타임 상수. 읽는 자: 호스트 Range 배열 접근 코드.
 *  - 값 범위: 8 고정. 동기화: 상수.
 *  - 비고: 디바이스가 LockingInfo::MaxRanges로 더 큰 값을 보고해도 SPDK는 8까지만 활용. */

/*
 * Feature Code
 */
/* [한국어] === Level 0 Discovery feature code ===
 *          디바이스가 디스커버리 응답에서 보고하는 feature descriptor 종류 ID.
 *          struct spdk_opal_d0_feat_hdr::code 필드에 big-endian uint16으로 실린다.
 *          호스트 코드는 첫 바이트부터 순회하면서 code 별로 spdk_opal_d0_*_feat 구조체로
 *          캐스팅하여 디바이스 capability를 디코딩한다. */
enum spdk_lv0_discovery_feature_code {
	/*
	 * TCG Storage Architecture Core Spec v2.01 r1.00
	 * 3.3.6 Level 0 Discovery
	 */
	FEATURECODE_TPER	= 0x0001,
	/* [한국어] TPer(Trusted Peripheral) Feature 디스크립터 식별 코드.
	 *  - 역할: 디바이스 내장 보안 코프로세서 자체가 어떤 통신 모드(sync/async/streaming/
	 *    ACK-NACK/buffer_mgmt/ComID 동적 관리)를 지원하는지 비트맵으로 보고.
	 *  - 설정자: 디바이스 펌웨어가 Level 0 Discovery 응답에 packed로 직렬화.
	 *  - 읽는 자: 호스트의 lib/nvme/nvme_opal.c::opal_check_support()가
	 *    spdk_opal_d0_feat_hdr::code (big-endian)와 비교 후 struct spdk_opal_d0_tper_feat
	 *    포인터로 캐스팅하여 디코드.
	 *  - 값 범위: 16비트 코드값 0x0001 (TCG SAC v2.01 Table-42에서 확정).
	 *  - 동기화: 디스커버리 응답 버퍼는 단일 호스트 스레드에서만 파싱되므로 락 불필요.
	 *  - 비고: TCG 인증 SED라면 반드시 보고해야 하는 의무 feature — 누락 시 TCG 미지원으로 간주. */

	FEATURECODE_LOCKING	= 0x0002,
	/* [한국어] Locking Feature 디스크립터 식별 코드.
	 *  - 역할: Locking SP(Security Provider)의 현재 라이프사이클 상태(activated/locked/
	 *    media_encryption/mbr_enabled/mbr_done)를 호스트에 보고.
	 *  - 설정자: 디바이스 펌웨어가 현재 상태를 매 디스커버리 응답마다 새로 채움.
	 *  - 읽는 자: 호스트가 "디스크가 이미 락 걸려 있는가? MBR shadowing이 활성인가?"
	 *    를 판단할 때 struct spdk_opal_d0_locking_feat의 비트필드를 검사.
	 *  - 값 범위: 0x0002 고정 (TCG SAC v2.01 Table-43).
	 *  - 동기화: 디바이스 측 상태 변경(락 해제 등) 후에는 새 Discovery를 발사해야 최신화.
	 *  - 비고: 부팅 시 MBR shadow 모드 진입 여부 결정에 핵심적으로 사용됨. */

	/*
	 * Opal SSC 1.00 r3.00 Final
	 * 3.1.1.4 Opal SSC Feature
	 */
	FEATURECODE_OPALV100	= 0x0200,
	/* [한국어] Opal SSC v1.0 SSC feature 디스크립터 식별 코드.
	 *  - 역할: 디바이스가 Opal SSC 버전 1.0을 지원함을 알리고 base_comid/number_comids/
	 *    range_crossing 정보를 함께 운반.
	 *  - 설정자: 펌웨어가 출하 시 정해진 정적 값으로 채움.
	 *  - 읽는 자: 호스트는 v200을 우선하지만 후방 호환을 위해 v100 디스크립터 발견 시
	 *    struct spdk_opal_d0_v100_feat로 캐스팅하여 base_comid를 추출.
	 *  - 값 범위: 0x0200 고정.
	 *  - 동기화: 정적 정보이므로 동기화 불요.
	 *  - 비고: 동일 디바이스가 v100과 v200을 동시에 보고하는 경우, SPDK는 v200 정보를 우선 사용. */

	/*
	 * TCG Storage Opal v2.01 r1.00
	 * 3.1.1.4 Geometry Reporting Feature
	 * 3.1.1.5 Opal SSC V2.00 Feature
	 */
	FEATURECODE_OPALV200	= 0x0203,
	/* [한국어] Opal SSC v2.0 SSC feature 디스크립터 식별 코드.
	 *  - 역할: v100 정보 + Admin/User Authority 수, initial_pin/reverted_pin 정책을 추가 보고.
	 *  - 설정자: 펌웨어 출하 시 정적 채움.
	 *  - 읽는 자: 호스트가 어떤 ComID로 StartSession을 발사할지(base_comid),
	 *    Authority가 몇 명까지 활성화 가능한지를 결정할 때 struct spdk_opal_d0_v200_feat 사용.
	 *  - 값 범위: 0x0203 고정 (TCG Opal SSC v2.01 3.1.1.5).
	 *  - 동기화: 정적 정보 — 동기화 불요.
	 *  - 비고: SPDK가 가장 자주 참조하는 feat. 본 코드와 v100을 모두 지원하는 경우, 본 코드의
	 *    base_comid를 정식 세션 ComID로 채택. */
	FEATURECODE_GEOMETRY	= 0x0003,
	/* [한국어] Geometry Reporting Feature 디스크립터 식별 코드.
	 *  - 역할: 디바이스의 LBA 정렬 강제 여부, 논리 블록 크기, 정렬 단위, 최저 정렬 LBA 보고.
	 *  - 설정자: 펌웨어 출하 시 정적 채움.
	 *  - 읽는 자: 호스트가 setup_locking_range 호출 직전 Range start/length를
	 *    alignment_granularity 배수로 보정할 때 struct spdk_opal_d0_geo_feat 참조.
	 *  - 값 범위: 0x0003 고정 (TCG Opal v2.01 3.1.1.4).
	 *  - 동기화: 정적 — 동기화 불요.
	 *  - 비고: align 비트가 1인데 정렬되지 않은 Range를 보내면 디바이스가 InvalidParameter 반환. */

	/*
	 * TCG Storage Opal Feature Set Single User Mode v1.00 r2.00
	 * 4.2.1 Single User Mode Feature Descriptor
	 */
	FEATURECODE_SINGLEUSER	= 0x0201,
	/* [한국어] Single User Mode Feature 디스크립터 식별 코드.
	 *  - 역할: 단일 사용자가 모든 Range를 독점 운영하는 단순 모드 지원 여부와 정책 보고.
	 *  - 설정자: 펌웨어 정적.
	 *  - 읽는 자: 호스트가 SUM 활성화 정책 결정 시 struct spdk_opal_d0_single_user_mode_feat의
	 *    any/all/policy 비트와 num_locking_objects 카운트 검사.
	 *  - 값 범위: 0x0201 고정 (Opal Feature Set: SUM v1.0 4.2.1).
	 *  - 동기화: 정적 — 동기화 불요.
	 *  - 비고: SPDK 현재 구현은 SUM을 옵션으로만 처리, 기본은 다중 사용자 모드. */

	/*
	 * TCG Storage Opal Feature Set Additional DataStore Tables v1.00 r1.00
	 * 4.1.1 DataStore Table Feature Descriptor
	 */
	FEATURECODE_DATASTORE	= 0x0202,
	/* [한국어] Additional DataStore Tables Feature 디스크립터 식별 코드.
	 *  - 역할: 디바이스가 호스트 정의 KV 데이터(부트 인증 정보 등) 저장용으로 노출하는
	 *    추가 DataStore 테이블의 max_tables/max_table_size/alignment 보고.
	 *  - 설정자: 펌웨어 정적.
	 *  - 읽는 자: 호스트가 DataStore에 데이터를 쓸 때 struct spdk_opal_d0_datastore_feat의
	 *    max_table_size를 페이로드 분할 기준으로 참조.
	 *  - 값 범위: 0x0202 고정 (Opal Feature Set: AddtlDataStore v1.0 4.1.1).
	 *  - 동기화: 정적 — 동기화 불요.
	 *  - 비고: 현재 SPDK 코드 경로에서는 디버깅/검증 외에 DataStore를 적극 사용하지 않음. */
};

/*
 * TCG Storage Architecture Core Spec v2.01 r1.00
 * 5.1.4 Abstract Type
 */
/* [한국어] === enum spdk_opal_token ===
 *          TCG SWG의 control token(0xF0~0xFF)과 자주 사용되는 컬럼 인덱스/메서드 인덱스를 한곳에
 *          모아놓은 enum. 같은 정수 값이 여러 enumerator에 등장하는 것은 의도된 동작이며,
 *          토큰 스트림의 "어느 위치에 등장하는가"에 따라 의미가 달라진다 (예: 0x03이 cell_block의
 *          STARTCOLUMN인지, locking row의 RANGESTART 컬럼 번호인지, ACE의 BOOLEAN_EXPR 컬럼인지는
 *          현재 파싱 중인 객체 종류로 결정).
 *          호스트는 이 값들을 1바이트 그대로 wire에 기록(또는 tiny atom으로 인코딩)한다. */
enum spdk_opal_token {
	/* boolean */
	SPDK_OPAL_TRUE			= 0x01,
	/* [한국어] Boolean TRUE 상수 (tiny atom unsigned 정수 0x01).
	 *  - 역할: cell_block 값 또는 메서드 인자에서 boolean true를 표현.
	 *    예: Set(MBRControl, Done=TRUE) — Pre-boot 종료 통지 시 인자로 전달.
	 *  - 설정자: 호스트 토큰 빌더(nvme_opal.c::opal_setup_locking_range 등)가 wire에 1바이트로 기록.
	 *  - 읽는 자: 응답 파서가 atom 디코드 후 비교. 디바이스도 boolean 컬럼 응답 시 0x01로 회신.
	 *  - 값 범위: 0x01 (tiny atom unsigned 1).
	 *  - 동기화: 컴파일타임 상수 — 동기화 불요. */
	SPDK_OPAL_FALSE			= 0x00,
	/* [한국어] Boolean FALSE 상수 (tiny atom unsigned 정수 0x00).
	 *  - 역할: ReadLockEnabled=FALSE 등 boolean 컬럼 비활성화 표현.
	 *  - 설정자: 호스트 토큰 빌더가 락 해제 시 인자로 wire에 기록.
	 *  - 읽는 자: 응답 파서/디바이스 펌웨어. tiny atom 0x00은 동시에 unsigned 정수 0이기도 하므로
	 *    호출 컨텍스트(컬럼 타입)로 의미 결정.
	 *  - 값 범위: 0x00.
	 *  - 동기화: 상수 — 동기화 불요. */

	/* cell_block
	 * 5.1.4.2.3 */
	SPDK_OPAL_TABLE			= 0x00,
	/* [한국어] cell_block의 named-value 키 "Table".
	 *  - 역할: Get/Set 메서드 인자에서 어떤 테이블을 대상으로 하는지 지정하는 이름 식별자.
	 *    토큰 스트림 패턴: [STARTNAME 0xF2][TABLE 0x00][table_uid byte-string][ENDNAME 0xF3].
	 *  - 설정자: 호스트(nvme_opal.c)가 STARTNAME 직후에 tiny atom 0x00으로 기록.
	 *  - 읽는 자: 디바이스 펌웨어의 메서드 dispatcher가 named arg lookup에 사용.
	 *  - 값 범위: 0x00 (TCG SAC v2.01 5.1.4.2.3 cell_block 키 인덱스).
	 *  - 동기화: 상수. */
	SPDK_OPAL_STARTROW		= 0x01,
	/* [한국어] cell_block의 named-value 키 "StartRow".
	 *  - 역할: Get 메서드의 row 범위 지정에서 시작 row 번호를 가리키는 이름 식별자.
	 *  - 설정자: 호스트가 부분 row 읽기 시 STARTNAME 직후 tiny atom 0x01로 기록.
	 *  - 읽는 자: 디바이스 펌웨어.
	 *  - 값 범위: 0x01.
	 *  - 동기화: 상수. */
	SPDK_OPAL_ENDROW		= 0x02,
	/* [한국어] cell_block의 named-value 키 "EndRow".
	 *  - 역할: Get 메서드 row 범위의 끝 row 번호 이름 식별자.
	 *  - 설정자: 호스트가 STARTNAME 직후 tiny atom 0x02로 기록.
	 *  - 읽는 자: 디바이스 펌웨어.
	 *  - 값 범위: 0x02.
	 *  - 동기화: 상수. */
	SPDK_OPAL_STARTCOLUMN		= 0x03,
	/* [한국어] cell_block의 named-value 키 "StartColumn".
	 *  - 역할: Get/Set이 다룰 컬럼 범위의 시작 인덱스 이름 식별자.
	 *  - 설정자: 호스트가 STARTNAME 후 tiny atom 0x03으로 기록.
	 *  - 읽는 자: 디바이스 펌웨어.
	 *  - 값 범위: 0x03.
	 *  - 동기화: 상수. */
	SPDK_OPAL_ENDCOLUMN		= 0x04,
	/* [한국어] cell_block의 named-value 키 "EndColumn".
	 *  - 역할: Get/Set이 다룰 컬럼 범위의 끝 인덱스 이름 식별자.
	 *  - 설정자: 호스트가 STARTNAME 후 tiny atom 0x04로 기록.
	 *  - 읽는 자: 디바이스 펌웨어.
	 *  - 값 범위: 0x04.
	 *  - 동기화: 상수. */
	SPDK_OPAL_VALUES		= 0x01,
	/* [한국어] Set 메서드의 named-arg 키 "Values".
	 *  - 역할: Set이 새 값으로 쓸 컬럼/값 리스트의 이름 식별자.
	 *  - 설정자: 호스트가 Set CALL 인자 빌드 시 STARTNAME 후 tiny atom 0x01로 기록.
	 *  - 읽는 자: 디바이스 펌웨어 Set 핸들러.
	 *  - 값 범위: 0x01 (STARTROW와 동일 정수지만 메서드 컨텍스트로 구분 — Get vs Set).
	 *  - 동기화: 상수. */

	/* C_PIN table
	 * 5.3.2.12 */
	SPDK_OPAL_PIN			= 0x03,
	/* [한국어] C_PIN 객체의 "PIN" 컬럼 인덱스.
	 *  - 역할: C_PIN_SID/C_PIN_Admin1/C_PIN_User1 등 패스워드 row의 PIN 컬럼을 named-value 키로
	 *    참조 (Get으로 PIN 비교용 hash 읽기, Set으로 새 PIN 저장).
	 *  - 설정자: 호스트가 set_new_pin 등의 토큰 빌드 시 컬럼 인덱스 0x03 기록.
	 *  - 읽는 자: 디바이스 펌웨어 C_PIN 핸들러가 row의 어느 컬럼인지 lookup.
	 *  - 값 범위: 0x03 (TCG Opal v2.01 5.3.2.12).
	 *  - 동기화: 상수. */

	/* locking table
	 * 5.7.2.2 */
	SPDK_OPAL_RANGESTART		= 0x03,
	/* [한국어] LockingTable row의 컬럼 3 — RangeStart (LBA in blocks).
	 *  - 역할: 잠금 가능 영역의 시작 LBA를 저장하는 컬럼 인덱스.
	 *  - 설정자: 호스트가 setup_locking_range Set 호출에 named-value로 기록.
	 *  - 읽는 자: 디바이스 펌웨어 Range 매니저. align 비트가 1일 때 alignment_granularity 배수 검사.
	 *  - 값 범위: 0x03 (Opal v2.01 5.7.2.2 Locking 테이블 컬럼 정의).
	 *  - 동기화: 상수. */
	SPDK_OPAL_RANGELENGTH		= 0x04,
	/* [한국어] LockingTable 컬럼 4 — RangeLength (블록 수).
	 *  - 역할: Range가 RangeStart부터 몇 블록 길이인지 저장.
	 *  - 설정자: 호스트 setup_locking_range. 읽는 자: 디바이스 Range 매니저.
	 *  - 값 범위: 0x04. 동기화: 상수. */
	SPDK_OPAL_READLOCKENABLED	= 0x05,
	/* [한국어] LockingTable 컬럼 5 — ReadLockEnabled.
	 *  - 역할: 이 Range에 대해 Read 락 메커니즘 자체를 켤지 끌지 결정 (true=락 가능).
	 *  - 설정자: 호스트가 Range 활성화 시 boolean true 기록.
	 *  - 읽는 자: 디바이스가 ReadLocked 비트 평가 시 이 비트가 켜져 있어야 의미 있음.
	 *  - 값 범위: 0x05. 동기화: 상수. */
	SPDK_OPAL_WRITELOCKENABLED	= 0x06,
	/* [한국어] LockingTable 컬럼 6 — WriteLockEnabled.
	 *  - 역할: Write 락 메커니즘 활성화 표식.
	 *  - 설정자: 호스트. 읽는 자: 디바이스 Range 매니저.
	 *  - 값 범위: 0x06. 동기화: 상수. */
	SPDK_OPAL_READLOCKED		= 0x07,
	/* [한국어] LockingTable 컬럼 7 — ReadLocked (현재 read 차단 여부).
	 *  - 역할: true이면 호스트의 Read 명령이 SED에서 거부됨.
	 *  - 설정자: 호스트가 Set으로 토글 (락/해제). 읽는 자: 디바이스 IO 게이트.
	 *  - 값 범위: 0x07.
	 *  - 동기화: 상수. ReadLockEnabled가 켜진 경우만 효과. */
	SPDK_OPAL_WRITELOCKED		= 0x08,
	/* [한국어] LockingTable 컬럼 8 — WriteLocked (현재 write 차단 여부).
	 *  - 역할/설정자/읽는 자: ReadLocked와 대칭이지만 write 방향.
	 *  - 값 범위: 0x08. 동기화: 상수. */
	SPDK_OPAL_ACTIVEKEY		= 0x0A,
	/* [한국어] LockingTable 컬럼 10 — ActiveKey (Range의 미디어 암호화 키 객체 UID 참조).
	 *  - 역할: 이 Range의 데이터를 암호화할 K_AES_256/K_AES_128 row UID를 가리킴.
	 *  - 설정자: 호스트가 GenKey 메서드 응답에서 받은 새 K_AES UID를 Set으로 기록.
	 *  - 읽는 자: 디바이스 IO 경로의 암호화 엔진이 이 키로 read/write 변환.
	 *  - 값 범위: 0x0A.
	 *  - 동기화: 상수. 단, 키 갱신 후에는 그 시점부터 새 키로 암호화되므로 호스트는 키 변경
	 *    전·후 데이터를 혼용해서는 안 됨 (사실상 영구 erase 효과). */

	/* locking info table */
	SPDK_OPAL_MAXRANGES		= 0x04,
	/* [한국어] LockingInfo 객체의 "MaxRanges" 컬럼 인덱스.
	 *  - 역할: 디바이스가 지원하는 최대 Range 개수를 보고하는 컬럼.
	 *  - 설정자: 디바이스 펌웨어 정적. 읽는 자: 호스트 Get → SPDK_OPAL_MAX_LOCKING_RANGE와 비교.
	 *  - 값 범위: 0x04 (RANGELENGTH와 정수값 동일하나 객체 컨텍스트(LockingInfo)로 구분).
	 *  - 동기화: 상수. */

	/* mbr control */
	SPDK_OPAL_MBRENABLE		= 0x01,
	/* [한국어] MBRControl 객체 컬럼 1 — Enable.
	 *  - 역할: MBR shadowing 기능 자체 활성화 비트(true이면 shadow MBR 메커니즘 동작).
	 *  - 설정자: 호스트가 enable_user 후 boolean true로 Set.
	 *  - 읽는 자: 디바이스 부팅 ROM/펌웨어가 LBA 0 응답 분기.
	 *  - 값 범위: 0x01. 동기화: 상수. */
	SPDK_OPAL_MBRDONE		= 0x02,
	/* [한국어] MBRControl 컬럼 2 — Done.
	 *  - 역할: Pre-boot 인증 완료 통지(true이면 실제 사용자 LBA 노출, false면 shadow MBR 노출).
	 *  - 설정자: 호스트가 사용자 인증 직후 true로 Set.
	 *  - 읽는 자: 디바이스 LBA 매핑 게이트.
	 *  - 값 범위: 0x02. 동기화: 상수. */

	/* properties */
	SPDK_OPAL_HOSTPROPERTIES	= 0x00,
	/* [한국어] Properties 메서드의 named-arg "HostProperties" 키.
	 *  - 역할: 호스트가 자신의 능력(MaxComPacketSize, MaxResponseComPacketSize 등)을
	 *    디바이스에 통보할 때 사용하는 인자 이름 식별자.
	 *  - 설정자: 호스트 nvme_opal.c::opal_session_open이 STARTNAME 직후 tiny atom 0x00 기록.
	 *  - 읽는 자: 디바이스 펌웨어 Properties 핸들러.
	 *  - 값 범위: 0x00 (TABLE/WHERE와 정수 동일, 메서드 컨텍스트로 구분).
	 *  - 동기화: 상수. */

	/* control tokens */
	SPDK_OPAL_STARTLIST		= 0xF0,
	/* [한국어] Control token: List 시작.
	 *  - 역할: 모든 메서드 인자/응답 list의 시작 마커. 1바이트로 wire에 기록.
	 *  - 설정자: 호스트 토큰 빌더가 CALL 인자 list 직전에 기록.
	 *  - 읽는 자: 디바이스/호스트 토큰 파서가 list 진입을 인식.
	 *  - 값 범위: 0xF0 (TCG SAC 5.1.4 control token 영역).
	 *  - 동기화: 상수. ENDLIST와 짝, 중첩 가능. */
	SPDK_OPAL_ENDLIST		= 0xF1,
	/* [한국어] Control token: List 종료.
	 *  - 역할: STARTLIST에서 시작된 list의 끝 마커.
	 *  - 설정자/읽는 자: 호스트·디바이스 양쪽 토큰 빌더/파서.
	 *  - 값 범위: 0xF1.
	 *  - 동기화: 상수. */
	SPDK_OPAL_STARTNAME		= 0xF2,
	/* [한국어] Control token: Named-value 시작.
	 *  - 역할: 직후 [name_atom][value_atom] 두 토큰을 named-value pair로 묶는 마커.
	 *    cell_block 키-값, Set의 column→value, properties named-arg 등 광범위 사용.
	 *  - 설정자: 호스트 토큰 빌더. 읽는 자: 디바이스 named arg lookup, 호스트 응답 파서.
	 *  - 값 범위: 0xF2.
	 *  - 동기화: 상수. ENDNAME과 짝. */
	SPDK_OPAL_ENDNAME		= 0xF3,
	/* [한국어] Control token: Named-value 종료.
	 *  - 역할/설정자/읽는 자: STARTNAME 짝.
	 *  - 값 범위: 0xF3. 동기화: 상수. */
	SPDK_OPAL_CALL			= 0xF8,
	/* [한국어] Control token: 메서드 호출 시작.
	 *  - 역할: 디바이스 객체에 메서드를 dispatch하는 시그널.
	 *    패턴: [CALL 0xF8][object_uid 8B short atom byte-string][method_uid 8B][arg STARTLIST...ENDLIST]
	 *           [ENDOFDATA 0xF9][status STARTLIST(int int int) ENDLIST].
	 *  - 설정자: 호스트(nvme_opal.c)가 모든 메서드 호출 패킷에 기록.
	 *  - 읽는 자: 디바이스 펌웨어 method dispatcher 진입점.
	 *  - 값 범위: 0xF8.
	 *  - 동기화: 상수. */
	SPDK_OPAL_ENDOFDATA		= 0xF9,
	/* [한국어] Control token: 메서드 인자 종료(End-of-Data).
	 *  - 역할: CALL 패킷의 인자 list 종료 후 status list로 진입함을 표시.
	 *    이후 [STARTLIST status_code reserved reserved ENDLIST]가 따라옴.
	 *  - 설정자: 호스트가 인자 직렬화 완료 후 기록(요청), 디바이스가 응답 인자 종료 시 기록.
	 *  - 읽는 자: 양쪽 파서 — status code 디코드 시작점.
	 *  - 값 범위: 0xF9. 동기화: 상수. */
	SPDK_OPAL_ENDOFSESSION		= 0xFA,
	/* [한국어] Control token: 세션 종료.
	 *  - 역할: 현재 HSN/TSN 세션을 닫음.
	 *    호스트는 [STARTLIST EOS ENDLIST] 또는 단독 EOS로 송신, 디바이스도 동일 토큰으로 응답.
	 *  - 설정자: 호스트 nvme_opal.c::opal_end_session.
	 *  - 읽는 자: 디바이스 세션 매니저가 세션 자원 해제.
	 *  - 값 범위: 0xFA.
	 *  - 동기화: 상수. EOS 후에는 같은 HSN/TSN으로 메서드 호출 불가. */
	SPDK_OPAL_STARTTRANSACTON	= 0xFB,
	/* [한국어] Control token: Transaction 시작.
	 *  - 역할: 여러 메서드를 atomic 묶음으로 처리하는 트랜잭션의 시작 마커.
	 *  - 설정자: (현재 SPDK 미사용) 트랜잭션 사용 시 호스트가 기록.
	 *  - 읽는 자: 디바이스 트랜잭션 매니저.
	 *  - 값 범위: 0xFB.
	 *  - 동기화: 상수. SPDK 호스트 코드는 단일 메서드 호출만 사용. */
	SPDK_OPAL_ENDTRANSACTON		= 0xFC,
	/* [한국어] Control token: Transaction 종료.
	 *  - 역할/설정자/읽는 자: STARTTRANSACTION 짝.
	 *  - 값 범위: 0xFC. 동기화: 상수. */
	SPDK_OPAL_EMPTYATOM		= 0xFF,
	/* [한국어] Control token: Empty atom (placeholder).
	 *  - 역할: 디바이스 응답에서 "값 없음"을 표시하거나 패딩 자리에 사용.
	 *  - 설정자: 디바이스 펌웨어. 읽는 자: 호스트 파서가 값 부재로 처리.
	 *  - 값 범위: 0xFF.
	 *  - 동기화: 상수. */
	SPDK_OPAL_WHERE			= 0x00,
	/* [한국어] Next 메서드의 named-arg 키 "Where".
	 *  - 역할: 어디부터 row 열거를 시작할지 가리키는 row UID 인자 이름.
	 *  - 설정자: 호스트가 Next CALL 인자 빌드 시 STARTNAME 직후 tiny atom 0x00 기록.
	 *  - 읽는 자: 디바이스 Next 핸들러.
	 *  - 값 범위: 0x00 (TABLE/HOSTPROPERTIES와 정수 동일, 메서드 컨텍스트로 구분).
	 *  - 동기화: 상수. */

	/* life cycle */
	SPDK_OPAL_LIFECYCLE		= 0x06,
	/* [한국어] SP 객체의 LifeCycle 컬럼 인덱스.
	 *  - 역할: Locking SP 등 SP row의 라이프사이클 상태(Manufactured/Issued/Disabled/Activated 등)
	 *    저장 컬럼. 값으로는 OPAL_MANUFACTURED_INACTIVE(0x08) 등이 와이어에 실림.
	 *  - 설정자: 호스트(Activate/Revert 시 Set), 디바이스 펌웨어(자동 전이).
	 *  - 읽는 자: 호스트 Get으로 현재 라이프사이클 확인.
	 *  - 값 범위: 0x06.
	 *  - 동기화: 상수. */

	/* Authority table */
	SPDK_OPAL_AUTH_ENABLE		= 0x05,
	/* [한국어] Authority 객체 컬럼 5 — Enabled.
	 *  - 역할: 해당 Authority(Admin1/User1 등)의 사용 가능 여부 boolean.
	 *  - 설정자: 호스트가 enable_user / activate_user 시 boolean true로 Set.
	 *  - 읽는 자: 디바이스 Authority 검증 로직.
	 *  - 값 범위: 0x05.
	 *  - 동기화: 상수. */

	/* ACE table */
	SPDK_OPAL_BOOLEAN_EXPR		= 0x03,
	/* [한국어] ACE(Access Control Element) row 컬럼 3 — BooleanExpression.
	 *  - 역할: 어느 Authority가 어떤 조건으로 이 객체에 대한 작업을 수행할 수 있는지 표현하는
	 *    boolean 식(반-half/quarter authority half UID 등이 RPN 형태로 나열).
	 *  - 설정자: 호스트 add_user_to_locking_range가 BooleanExpr를 갱신해 User1이 락 풀 수 있도록 허용.
	 *  - 읽는 자: 디바이스 ACL 평가 엔진이 메서드 호출 권한 여부 판정.
	 *  - 값 범위: 0x03 (RANGESTART/PIN/STARTCOLUMN과 정수 동일, ACE row 컨텍스트로 구분).
	 *  - 동기화: 상수. */
};

/*
 * TCG Storage Architecture Core Spec v2.01 r1.00
 * Table-39 Level0 Discovery Header Format
 */
/* [한국어] === Level 0 Discovery 응답 헤더 ===
 *          Security Receive(SECP=0x01, COMID=0x01) 시 디바이스가 반환하는 1KB 버퍼의 첫 48B.
 *          이 헤더 뒤에 N개의 feature descriptor가 연속해서 packed 배치된다. 모든 multi-byte
 *          필드는 big-endian (TCG wire 규약). 호스트는 length 필드까지 읽고 그 만큼 이후를 순회. */
struct spdk_opal_d0_hdr {
	uint32_t length;
	/* [한국어] Discovery 응답 본문의 전체 길이(big-endian, 헤더 자신 제외).
	 *  - 역할: 헤더 뒤에 packed로 이어지는 N개 feature descriptor의 총 바이트 수.
	 *  - 설정자: 디바이스 펌웨어가 Security Receive(SECP=0x01, COMID=LV0_DISCOVERY_COMID)
	 *    응답을 직렬화할 때 채움.
	 *  - 읽는 자: 호스트 lib/nvme/nvme_opal.c::opal_check_support()가 이 값으로
	 *    feature 순회 루프 종료 조건을 결정.
	 *  - 값 범위: 0..(1024-48) — 보통 100~200B 수준. 1024는 Discovery 응답 표준 버퍼 크기.
	 *  - 동기화: 응답 버퍼는 단일 호스트 스레드(보안 명령을 발사한 admin queue thread)만 접근. */
	uint32_t revision;
	/* [한국어] Discovery 헤더 포맷 리비전 번호(big-endian).
	 *  - 역할: 향후 헤더 레이아웃이 확장될 때 호스트가 분기 처리 가능하도록.
	 *  - 설정자: 디바이스 펌웨어. 현재는 1 고정.
	 *  - 읽는 자: 호스트(현재 SPDK는 검증 없이 진행).
	 *  - 값 범위: 1 (TCG SAC v2.01 Table-39 기준).
	 *  - 동기화: 디스커버리 응답 안의 read-only 값 — 동기화 불요. */
	uint32_t reserved_0;
	/* [한국어] 예약 4B (반드시 0).
	 *  - 역할: 향후 스펙 확장용 슬롯. 현재 의미 없음.
	 *  - 설정자: 디바이스 펌웨어가 0으로 채움. 읽는 자: 호스트는 무시.
	 *  - 값 범위: 0 고정. 동기화: 불요. */
	uint32_t reserved_1;
	/* [한국어] 예약 4B (반드시 0).
	 *  - 역할: 향후 스펙 확장용 두 번째 예약 슬롯.
	 *  - 설정자/읽는 자/값 범위/동기화: reserved_0과 동일. */
	uint8_t vendor_specfic[32];
	/* [한국어] 벤더 정의 32B 영역.
	 *  - 역할: 디스크 제조사가 자유롭게 사용하는 모델별 메타데이터(시리얼/펌웨어 패턴 등).
	 *  - 설정자: 디바이스 펌웨어가 출하 시 또는 응답 생성 시 채움.
	 *  - 읽는 자: SPDK는 무시. 디버깅 시 hexdump로만 확인.
	 *  - 값 범위: 임의의 32바이트.
	 *  - 동기화: read-only 응답 버퍼 — 락 불필요. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_d0_hdr) == 48, "Incorrect size");
/* [한국어] 컴파일 타임 검증 — TCG Table-39가 명시한 48바이트와 정확히 일치해야 함.
 *          padding이 들어가면 wire 디코드가 어긋나므로 빌드 실패로 차단. */

/*
 * Level 0 Discovery Feature Header
 */
/* [한국어] 각 feature descriptor의 공통 4B 헤더. spdk_opal_d0_*_feat 구조체들이 첫 멤버로 포함. */
struct spdk_opal_d0_feat_hdr {
	uint16_t	code;
	/* [한국어] Feature 종류 식별자(big-endian).
	 *  - 역할: 이 디스크립터가 어떤 feature 인지(TPER/LOCKING/GEOMETRY/OPALV200 등)를 호스트에 알림.
	 *  - 설정자: 디바이스 펌웨어가 enum spdk_lv0_discovery_feature_code 값을 채움.
	 *  - 읽는 자: 호스트 opal_check_support()가 switch(code) 분기로 적합한 feat 구조체로 캐스팅.
	 *  - 값 범위: 0x0001~0x0203 (현재까지 정의된 feature codes).
	 *  - 동기화: read-only 응답 버퍼 — 동기화 불요. */
	uint8_t		reserved : 4;
	/* [한국어] 예약 4비트(0).
	 *  - 역할: 향후 확장용. 설정자: 디바이스 0 기록. 읽는 자: 호스트 무시.
	 *  - 값 범위: 0 고정. 동기화: 불요. */
	uint8_t		version : 4;
	/* [한국어] 이 feature descriptor 포맷의 버전(0~15).
	 *  - 역할: 동일 code 내 포맷 확장 시 호스트가 미지원 버전 skip 가능하도록.
	 *  - 설정자: 디바이스 펌웨어. 읽는 자: 호스트 — 알려진 version만 파싱.
	 *  - 값 범위: 0..15 (4비트). 보통 1.
	 *  - 동기화: 불요. */
	uint8_t		length;
	/* [한국어] 헤더를 제외한 페이로드 바이트 수.
	 *  - 역할: 한 디스크립터 총 크기 = sizeof(hdr) + length. 호스트가 다음 디스크립터로 점프할 때 사용.
	 *  - 설정자: 디바이스 펌웨어. 읽는 자: 호스트 순회 루프.
	 *  - 값 범위: 0..255 (1바이트). 흔히 12 또는 28.
	 *  - 동기화: 불요. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_d0_feat_hdr) == 4, "Incorrect size");
/* [한국어] feature 헤더는 정확히 4B. 비트필드 선언 순서·크기가 wire 그대로여야 함을 검증. */


/*
 * TCG Storage Architecture Core Spec v2.01 r1.00
 * Table-42 TPer Feature Descriptor
 */
/* [한국어] TPer(Trusted Peripheral) 자체 능력 비트맵. 디바이스가 어떤 동기/비동기 통신
 *          모드를 지원하고 ComID 동적 관리가 가능한지를 호스트에 알린다. */
struct __attribute__((packed)) spdk_opal_d0_tper_feat {
	struct spdk_opal_d0_feat_hdr hdr;
	/* [한국어] feature 공통 헤더.
	 *  - 역할: code = FEATURECODE_TPER (0x0001), version = 1, length = 12 식별 정보.
	 *  - 설정자: 디바이스 펌웨어. 읽는 자: 호스트 opal_check_support().
	 *  - 값 범위: code 0x0001 고정. 동기화: 불요. */
	uint8_t sync : 1;
	/* [한국어] Sync 통신 모드 지원 비트.
	 *  - 역할: 1이면 호스트가 IF-SEND 후 곧바로 IF-RECV로 응답을 회수하는 동기식 통신 가능.
	 *  - 설정자: 디바이스 펌웨어. 읽는 자: 호스트 — SPDK는 폴링 모델이라 항상 sync 모드 활용.
	 *  - 값 범위: 0 또는 1.
	 *  - 동기화: 불요(읽기 전용 capability). */
	uint8_t async : 1;
	/* [한국어] Async 통신 모드 지원 비트.
	 *  - 역할: 1이면 디바이스가 별도 이벤트/인터럽트로 응답 통지하는 비동기 모드 지원.
	 *  - 설정자: 디바이스 펌웨어. 읽는 자: 호스트(현재 SPDK는 활용 안 함).
	 *  - 값 범위: 0 또는 1. 동기화: 불요. */
	uint8_t acknack : 1;
	/* [한국어] ACK/NACK 시퀀스 지원 비트.
	 *  - 역할: 1이면 패킷 단편 전달 시 디바이스가 ACK/NACK으로 신뢰성 확인.
	 *  - 설정자: 디바이스 펌웨어. 읽는 자: 호스트(SPDK 미사용).
	 *  - 값 범위: 0/1. 동기화: 불요. */
	uint8_t buffer_management : 1;
	/* [한국어] Buffer Management 지원 비트.
	 *  - 역할: 1이면 큰 페이로드(MBR 이미지 등)를 분할 전송할 때 디바이스 측 버퍼 협상 가능.
	 *  - 설정자: 펌웨어. 읽는 자: 호스트가 큰 데이터 전송 시 host_properties로 협상할지 결정.
	 *  - 값 범위: 0/1. 동기화: 불요. */
	uint8_t streaming : 1;
	/* [한국어] Streaming 통신 지원 비트.
	 *  - 역할: 1이면 연속된 페이로드 스트림 전송 가능.
	 *  - 설정자: 펌웨어. 읽는 자: 호스트(SPDK는 단발 메서드 호출만 사용).
	 *  - 값 범위: 0/1. 동기화: 불요. */
	uint8_t reserved_1 : 1;
	/* [한국어] 예약 1비트(0). 향후 capability 비트 추가용. 설정자 펌웨어 0. 호스트 무시. */
	uint8_t comid_management : 1;
	/* [한국어] ComID 동적 관리 지원 비트.
	 *  - 역할: 1이면 임시 ComID 할당/해제 메서드를 호출 가능. 다중 호스트 동시 세션 시 유용.
	 *  - 설정자: 펌웨어. 읽는 자: 호스트(SPDK는 v200 base_comid를 정적으로 사용하므로 활용 안 함).
	 *  - 값 범위: 0/1. 동기화: 불요. */
	uint8_t reserved_2 : 1;
	/* [한국어] 예약 1비트(0). 비트맵 8비트 정렬을 채우기 위한 슬롯. */

	uint8_t reserved_3[3];
	/* [한국어] 예약 3B(0).
	 *  - 역할: capability 비트맵 다음의 패딩/예약 영역. 향후 비트맵 확장 시 활용.
	 *  - 설정자: 펌웨어 0. 읽는 자: 호스트 무시. 값 범위: 0. 동기화: 불요. */
	uint32_t reserved_4;
	/* [한국어] 예약 4B(0). 향후 capability 확장 슬롯. */
	uint32_t reserved_5;
	/* [한국어] 예약 4B(0). 향후 capability 확장 슬롯. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_d0_tper_feat) == 16, "Incorrect size");
/* [한국어] TPer feat = 4B(hdr) + 12B(payload) = 16B 검증. */

/*
 * TCG Storage Architecture Core Spec v2.01 r1.00
 * Table-43 Locking Feature Descriptor
 */
/* [한국어] Locking SP 현재 상태 비트맵 — 디스크가 출하 직후인지, 이미 락 적용 중인지를 보고. */
struct __attribute__((packed)) spdk_opal_d0_locking_feat {
	struct spdk_opal_d0_feat_hdr hdr;
	/* [한국어] feature 공통 헤더.
	 *  - 역할: code = FEATURECODE_LOCKING (0x0002), version=1, length=12 식별.
	 *  - 설정자: 펌웨어. 읽는 자: 호스트 opal_check_support().
	 *  - 값 범위: code 0x0002 고정. 동기화: 불요. */
	uint8_t locking_supported : 1;
	/* [한국어] Locking SP 자체 지원 비트.
	 *  - 역할: 1이면 디바이스가 Locking SP를 보유(Opal SED는 의무적으로 1).
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트가 Opal 호환성 확인 시 검사.
	 *  - 값 범위: 0/1. 동기화: 불요(정적). */
	uint8_t locking_enabled : 1;
	/* [한국어] Locking SP 활성화 상태 비트.
	 *  - 역할: 1이면 Activate 메서드가 이미 수행되어 Range/Authority 사용 가능.
	 *    0이면 Manufactured-Inactive — Admin1 PIN 변경 후 Activate 필요.
	 *  - 설정자: 디바이스가 Activate/Revert 시 갱신. 읽는 자: 호스트 — 초기 setup 분기.
	 *  - 값 범위: 0/1.
	 *  - 동기화: 디스커버리 응답마다 최신 값 반환 — 락 적용 후 호스트는 새 Discovery로 재확인. */
	uint8_t locked : 1;
	/* [한국어] 현재 어떤 Range든 락 적용 중 비트.
	 *  - 역할: 임의 Range가 Read/Write 락 상태이면 1로 보고 — 호스트의 빠른 판정.
	 *  - 설정자: 디바이스가 Range 락 상태 OR 결과로 갱신. 읽는 자: 호스트가 락 해제 필요성 판단.
	 *  - 값 범위: 0/1. 동기화: 디스커버리 시점 스냅샷. */
	uint8_t media_encryption : 1;
	/* [한국어] 미디어 암호화(SED) 지원 비트.
	 *  - 역할: 1이면 디바이스가 미디어 자체를 AES로 암호화(Opal SED 표준).
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트.
	 *  - 값 범위: 0/1. 0인 디바이스는 단순 락만 — SPDK는 보통 1만 다룸.
	 *  - 동기화: 정적. */
	uint8_t mbr_enabled : 1;
	/* [한국어] MBR shadowing 활성화 비트.
	 *  - 역할: 1이면 LBA 0..MBR_LEN 영역이 shadow MBR로 가려져 있음.
	 *  - 설정자: 디바이스가 MBRControl::Enable Set 결과로 갱신. 읽는 자: 호스트가 부팅 전략 결정.
	 *  - 값 범위: 0/1. 동기화: 디스커버리 스냅샷. */
	uint8_t mbr_done : 1;
	/* [한국어] MBR Done 비트.
	 *  - 역할: 1이면 사용자 LBA 노출, 0이면 shadow MBR 노출.
	 *  - 설정자: 디바이스가 MBRControl::Done Set으로 갱신. 읽는 자: 호스트.
	 *  - 값 범위: 0/1. 동기화: 스냅샷. */
	uint8_t reserved_1 : 1;
	/* [한국어] 예약 1비트(0). 향후 상태 비트 추가용. */
	uint8_t reserved_2 : 1;
	/* [한국어] 예약 1비트(0). 비트맵 8비트 정렬용 슬롯. */

	uint8_t reserved_3[3];
	/* [한국어] 예약 3B(0). 펌웨어 0 기록, 호스트 무시. */
	uint32_t reserved_4;
	/* [한국어] 예약 4B(0). 향후 확장. */
	uint32_t reserved_5;
	/* [한국어] 예약 4B(0). 향후 확장. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_d0_locking_feat) == 16, "Incorrect size");
/* [한국어] Locking feat = 16B. */

/*
 * TCG Storage Opal Feature Set Single User Mode v1.00 r2.00
 * 4.2.1 Single User Mode Feature Descriptor
 */
/* [한국어] Single User Mode 능력 보고 — Range 단위로 사용자가 단일 권한자만 갖는 단순 모드. */
struct __attribute__((packed)) spdk_opal_d0_single_user_mode_feat {
	struct spdk_opal_d0_feat_hdr hdr;
	/* [한국어] feature 공통 헤더.
	 *  - 역할: code = FEATURECODE_SINGLEUSER (0x0201) 식별.
	 *  - 설정자: 펌웨어. 읽는 자: 호스트.
	 *  - 값 범위: 0x0201 고정. 동기화: 불요. */
	uint32_t num_locking_objects;
	/* [한국어] Single User Mode에서 사용 가능한 Locking 객체 개수(big-endian).
	 *  - 역할: SUM이 적용 가능한 최대 Range 카운트. Opal v2 num_locking_admin_auth와 별개 차원.
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트가 SUM 활성화 시 사용자 슬롯 한도 계산.
	 *  - 값 범위: 0..N (보통 8과 일치).
	 *  - 동기화: 정적. */
	uint8_t any : 1;
	/* [한국어] SUM "Any" 정책 비트.
	 *  - 역할: 1이면 임의의 한 사용자가 Range를 단독 소유 가능.
	 *  - 설정자: 펌웨어. 읽는 자: 호스트가 SUM 정책 협상 시 검사.
	 *  - 값 범위: 0/1. 동기화: 정적. */
	uint8_t all : 1;
	/* [한국어] SUM "All" 정책 비트.
	 *  - 역할: 1이면 모든 Range가 동시에 SUM 모드로 운영 가능.
	 *  - 설정자/읽는 자/값 범위/동기화: any와 대칭. */
	uint8_t policy : 1;
	/* [한국어] SUM policy 결정자 비트.
	 *  - 역할: 0이면 "Owner sets policy"(Owner Authority가 SUM 설정),
	 *    1이면 "Authority sets policy"(개별 Authority가 자기 Range 정책 설정 가능).
	 *  - 설정자: 펌웨어. 읽는 자: 호스트.
	 *  - 값 범위: 0/1. 동기화: 정적. */
	uint8_t reserved_1 : 5;
	/* [한국어] 예약 5비트(0). 향후 정책 비트 추가용. */

	uint8_t reserved_2;
	/* [한국어] 예약 1B(0). 향후 확장. */
	uint16_t reserved_3;
	/* [한국어] 예약 2B(0). 향후 확장. */
	uint32_t reserved_4;
	/* [한국어] 예약 4B(0). 향후 확장. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_d0_single_user_mode_feat) == 16, "Incorrect size");
/* [한국어] SingleUserMode feat = 16B. */

/*
 * TCG Storage Opal v2.01 r1.00
 * 3.1.1.4 Geometry Reporting Feature
 */
/* [한국어] 디바이스의 LBA 정렬/논리블록 크기 보고 — Range 생성 시 정렬 검증에 사용. */
struct __attribute__((packed)) spdk_opal_d0_geo_feat {
	struct spdk_opal_d0_feat_hdr hdr;
	/* [한국어] feature 공통 헤더.
	 *  - 역할: code = FEATURECODE_GEOMETRY (0x0003) 식별.
	 *  - 설정자: 펌웨어. 읽는 자: 호스트.
	 *  - 값 범위: 0x0003 고정. 동기화: 정적. */
	uint8_t align : 1;
	/* [한국어] LBA 정렬 강제 비트.
	 *  - 역할: 1이면 Range start/length가 alignment_granularity 배수여야 디바이스가 수락.
	 *    0이면 임의 LBA 허용(드물다).
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트 setup_locking_range가 사전 검증.
	 *  - 값 범위: 0/1. 동기화: 정적. */
	uint8_t reserved_1 : 7;
	/* [한국어] 예약 7비트(0). 비트맵 1바이트 채움. */
	uint8_t reserved_2[7];
	/* [한국어] 예약 7B(0). 64비트 정렬 padding. */
	uint32_t logical_block_size;
	/* [한국어] 논리 블록 크기(big-endian, 바이트).
	 *  - 역할: 한 LBA가 몇 바이트인지(보통 512 또는 4096). Range 길이 LBA → bytes 변환 기준.
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트가 Range 인자 단위 변환에 사용.
	 *  - 값 범위: 512, 4096 등 2의 거듭제곱.
	 *  - 동기화: 정적. */
	uint64_t alignment_granularity;
	/* [한국어] 정렬 단위(big-endian, 블록 수).
	 *  - 역할: Range start/length는 이 값의 배수여야 함. align=1일 때 검증 기준.
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트.
	 *  - 값 범위: 보통 1, 8, 16 등. 동기화: 정적. */
	uint64_t lowest_aligned_lba;
	/* [한국어] 정렬 시작 LBA(big-endian).
	 *  - 역할: 정렬 검증이 적용되는 최소 LBA. 그 이전 LBA는 정렬에서 제외(부트 영역 보호 등).
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트.
	 *  - 값 범위: 0..디스크 크기. 보통 0. 동기화: 정적. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_d0_geo_feat) == 32, "Incorrect size");
/* [한국어] Geometry feat = 4B + 28B = 32B 검증. */

/*
 * TCG Storage Opal Feature Set Additional DataStore Tables v1.00 r1.00
 * 4.1.1 DataStore Table Feature Descriptor
 */
/* [한국어] DataStore 테이블 능력 — 호스트가 디바이스 안에 저장 가능한 KV 영역의 한계 보고. */
struct __attribute__((packed)) spdk_opal_d0_datastore_feat {
	struct spdk_opal_d0_feat_hdr hdr;
	/* [한국어] feature 공통 헤더.
	 *  - 역할: code = FEATURECODE_DATASTORE (0x0202) 식별.
	 *  - 설정자: 펌웨어. 읽는 자: 호스트.
	 *  - 값 범위: 0x0202. 동기화: 정적. */
	uint16_t reserved_1;
	/* [한국어] 예약 2B(0). 정렬용. */
	uint16_t max_tables;
	/* [한국어] 디바이스가 지원하는 추가 DataStore 테이블 최대 개수(big-endian).
	 *  - 역할: 호스트가 0..max_tables-1 인덱스로 KV 영역을 할당 가능한 한도.
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트가 DataStore 사용 전 검증.
	 *  - 값 범위: 1..N. 동기화: 정적. */
	uint32_t max_table_size;
	/* [한국어] 한 테이블의 최대 바이트 수(big-endian).
	 *  - 역할: 큰 KV 페이로드를 쓸 때 호스트가 분할 단위로 사용.
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트 DataStore writer.
	 *  - 값 범위: 1..2^32-1. 동기화: 정적. */
	uint32_t alignment;
	/* [한국어] DataStore 접근 정렬 단위(big-endian).
	 *  - 역할: DataStore 오프셋이 이 값의 배수여야 함.
	 *  - 설정자: 펌웨어. 읽는 자: 호스트.
	 *  - 값 범위: 1, 4, 8 등. 동기화: 정적. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_d0_datastore_feat) == 16, "Incorrect size");
/* [한국어] DataStore feat = 16B. */

/*
 * Opal SSC 1.00 r3.00 Final
 * 3.1.1.4 Opal SSC Feature
 */
/* [한국어] Opal SSC v1.0 디스크립터 — base_comid, range_crossing 보고. */
struct __attribute__((packed)) spdk_opal_d0_v100_feat {
	struct spdk_opal_d0_feat_hdr hdr;
	/* [한국어] feature 공통 헤더.
	 *  - 역할: code = FEATURECODE_OPALV100 (0x0200) 식별.
	 *  - 설정자: 펌웨어. 읽는 자: 호스트(v200 미지원 디바이스 폴백).
	 *  - 값 범위: 0x0200 고정. 동기화: 정적. */
	uint16_t base_comid;
	/* [한국어] 일반 세션의 시작 ComID(big-endian).
	 *  - 역할: 호스트가 StartSession 패킷의 ComPacket::comid 필드에 채워 디바이스에 발사.
	 *    Discovery 전용 LV0_DISCOVERY_COMID(0x01)과는 별개의 동적 할당 영역.
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트가 세션 빌더에 전달.
	 *  - 값 범위: 0x0800~0xFFFE 영역에 할당되는 것이 일반적.
	 *  - 동기화: 정적. */
	uint16_t number_comids;
	/* [한국어] base_comid부터 사용 가능한 연속 ComID 개수(big-endian).
	 *  - 역할: 멀티 세션 시 호스트가 base_comid + i 로 추가 ComID 사용.
	 *  - 설정자: 펌웨어. 읽는 자: 호스트.
	 *  - 값 범위: 1..N. 동기화: 정적. */
	uint8_t range_crossing : 1;
	/* [한국어] Range 횡단 IO 허용 비트.
	 *  - 역할: 0이면 한 IO가 두 Range를 동시에 건드릴 수 없음(디바이스가 분할 거부),
	 *    1이면 허용 — 호스트는 IO 분할 전략을 이 값으로 결정.
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트 IO 빌더.
	 *  - 값 범위: 0/1. 동기화: 정적. */

	uint8_t reserved_1 : 7;
	/* [한국어] 예약 7비트(0). 비트맵 정렬용. */
	uint8_t reserved_2;
	/* [한국어] 예약 1B(0). 정렬용. */
	uint16_t reserved_3;
	/* [한국어] 예약 2B(0). 향후 확장. */
	uint32_t reserved_4;
	/* [한국어] 예약 4B(0). 향후 확장. */
	uint32_t reserved_5;
	/* [한국어] 예약 4B(0). 향후 확장. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_d0_v100_feat) == 20, "Incorrect size");
/* [한국어] Opal v100 feat = 4 + 16 = 20B. */

/*
 * TCG Storage Opal v2.01 r1.00
 * 3.1.1.4 Geometry Reporting Feature
 * 3.1.1.5 Opal SSC V2.00 Feature
 */
/* [한국어] Opal SSC v2.0 디스크립터 — v1.0 정보 + Authority 수, 초기/리셋 PIN 정책 추가.
 *          SPDK가 가장 자주 참조하는 feat. */
struct __attribute__((packed)) spdk_opal_d0_v200_feat {
	struct spdk_opal_d0_feat_hdr hdr;
	/* [한국어] feature 공통 헤더.
	 *  - 역할: code = FEATURECODE_OPALV200 (0x0203) 식별. SPDK 주력 feat.
	 *  - 설정자: 펌웨어. 읽는 자: 호스트.
	 *  - 값 범위: 0x0203 고정. 동기화: 정적. */
	uint16_t base_comid;
	/* [한국어] 일반 세션 시작 ComID(big-endian).
	 *  - 역할: 호스트가 ComPacket::comid에 기록하여 StartSession 패킷 발사.
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트 nvme_opal.c 세션 매니저.
	 *  - 값 범위: 0x0800~0xFFFE 등 (디바이스 별 다름).
	 *  - 동기화: 정적. */
	uint16_t num_comids;
	/* [한국어] base_comid부터 사용 가능한 ComID 수(big-endian).
	 *  - 역할: 멀티 세션 시 ComID 인덱싱 한도.
	 *  - 설정자/읽는 자/동기화: v100의 number_comids와 동일.
	 *  - 값 범위: 1..N. */
	uint8_t range_crossing : 1;
	/* [한국어] Range 횡단 IO 허용 비트.
	 *  - 역할/설정자/읽는 자: v100의 range_crossing과 동일 의미.
	 *  - 값 범위: 0/1. 동기화: 정적. */
	uint8_t reserved_1 : 7;
	/* [한국어] 예약 7비트(0). 비트맵 정렬. */
	uint16_t num_locking_admin_auth; /* Number of Locking SP Admin Authorities Supported */
	/* [한국어] Locking SP가 지원하는 Admin Authority 수(big-endian).
	 *  - 역할: Activate 후 Admin1~AdminN 사용 한도. 보통 4 (Admin1~Admin4).
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트가 권한 인덱스 검증에 사용.
	 *  - 값 범위: 1..N. 동기화: 정적. */
	uint16_t num_locking_user_auth;
	/* [한국어] Locking SP가 지원하는 User Authority 수(big-endian).
	 *  - 역할: User1~UserN 슬롯의 상한. 보통 8.
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트.
	 *  - 값 범위: 1..N. 동기화: 정적. */
	uint8_t initial_pin;
	/* [한국어] 초기 SID PIN 정책 코드.
	 *  - 역할: 출하 직후 SID PIN이 무엇인지 호스트에 통지.
	 *    0x00 = MSID(드라이브 별 디스커버리로 읽을 수 있는 기본 PIN)와 동일,
	 *    0xFF = 벤더 정의(별도 매뉴얼/스티커 PIN), 그 외 = TCG 미지정 코드.
	 *  - 설정자: 펌웨어 출하 시 정적. 읽는 자: 호스트가 첫 SID 인증 시 어떤 PIN 시도할지 결정.
	 *  - 값 범위: 0x00..0xFF. 동기화: 정적. */
	uint8_t reverted_pin;
	/* [한국어] Revert 후 SID PIN 정책 코드.
	 *  - 역할: Revert(공장 초기화) 메서드 수행 후 SID PIN이 어떤 상태로 돌아가는지 표시.
	 *    값 의미는 initial_pin과 동일.
	 *  - 설정자: 펌웨어 정적. 읽는 자: 호스트가 Revert 후 재인증 시 PIN 결정.
	 *  - 값 범위: 0x00..0xFF. 동기화: 정적. */

	uint8_t reserved_2;
	/* [한국어] 예약 1B(0). 정렬. */
	uint32_t reserved_3;
	/* [한국어] 예약 4B(0). 향후 확장. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_d0_v200_feat) == 20, "Incorrect size");
/* [한국어] Opal v200 feat = 20B. */

/*
 * TCG Storage Architecture Core Spec v2.01 r1.00
 * 3.2.3 ComPackets, Packets & Subpackets
 */
/* [한국어] === 와이어 전송 단위의 3-layer 헤더 ===
 *          호스트가 디바이스로 보내는 한 번의 NVMe Security Send 페이로드(또는 Receive 응답)는
 *          다음 중첩 구조를 갖는다:
 *
 *            +-- ComPacket header (20B, struct spdk_opal_compacket) -------------+
 *            |  +-- Packet header (24B, struct spdk_opal_packet, HSN/TSN) ----+  |
 *            |  |  +-- Subpacket header (12B, struct spdk_opal_data_subpacket)  |
 *            |  |  |   << SWG token stream payload (CALL/EOS/STARTLIST/...) >>  |
 *            |  |  +-- (4B align padding) -+-+
 *            |  +-- (4B align padding) ----+
 *            +-- (4B align padding) -------+
 *
 *          Subpacket 안의 페이로드 길이는 4B 정렬 패딩 전 길이. 모든 multi-byte 정수는 big-endian.
 *          한 ComPacket 안에 여러 Packet, 한 Packet 안에 여러 Subpacket이 들어갈 수 있으나
 *          SPDK는 단순화를 위해 1:1:1로 사용. */

/* CommPacket header format
 * (big-endian)
 */
struct __attribute__((packed)) spdk_opal_compacket {
	uint32_t reserved;
	/* [한국어] 예약 4B(0).
	 *  - 역할: TCG ComPacket 헤더 시작에 위치하는 패딩.
	 *  - 설정자: 호스트(send) 또는 디바이스(receive)가 0으로 채움. 읽는 자: 양쪽 무시.
	 *  - 값 범위: 0 고정. 동기화: 불요. */
	uint8_t comid[2];
	/* [한국어] 이 ComPacket의 대상 ComID(big-endian, 2B 바이트 배열로 선언 — 정렬 회피).
	 *  - 역할: 호스트→디바이스 send 시 v200_feat.base_comid 또는 LV0_DISCOVERY_COMID(0x01)을 기록.
	 *    디바이스→호스트 응답에는 같은 ComID 미러링 → 호스트가 응답 매칭에 사용.
	 *    NVMe Security Send/Recv의 SECP=0x01(TCG) + SP_SPECIFIC=ComID로 wire에 실린다.
	 *  - 설정자: 호스트 nvme_opal.c가 send 시 채움. 디바이스 펌웨어가 receive 시 미러링.
	 *  - 읽는 자: 디바이스 ComPacket 라우터 / 호스트 응답 디스패처.
	 *  - 값 범위: 0x0001 (Discovery) 또는 base_comid 영역(0x0800~).
	 *  - 동기화: 한 세션 동안 단일 ComID 고정 — 별도 락 불요. */
	uint8_t extended_comid[2];
	/* [한국어] 확장 ComID(big-endian, 2B).
	 *  - 역할: 디바이스가 ComID 동적 관리(comid_management)를 지원할 때 임시 ComID를 식별.
	 *  - 설정자: 호스트(통상 0). 읽는 자: 디바이스.
	 *  - 값 범위: 0 (정적 ComID 모드) 또는 1..N (동적 모드).
	 *  - 동기화: 불요. */
	uint32_t outstanding_data;
	/* [한국어] 응답 잔여 데이터 바이트 수(big-endian, Receive 응답에서 의미).
	 *  - 역할: 디바이스가 한 번의 Receive에 다 못 실은 경우 남은 데이터 양을 보고.
	 *    호스트는 0이 아니면 한 번 더 Security Receive 호출.
	 *  - 설정자: 디바이스 펌웨어. 읽는 자: 호스트 응답 처리 루프.
	 *  - 값 범위: 0..N 바이트.
	 *  - 동기화: 응답 버퍼 안 — 동기화 불요. */
	uint32_t min_transfer;
	/* [한국어] 디바이스 권장 최소 전송 단위(big-endian).
	 *  - 역할: 다음 Security Receive 호출 시 호스트가 준비할 최소 버퍼 크기.
	 *  - 설정자: 디바이스. 읽는 자: 호스트가 다음 Receive 버퍼 할당 크기 결정.
	 *  - 값 범위: 0..N. 동기화: 불요. */
	uint32_t length;
	/* [한국어] ComPacket 페이로드(Packet들) 총 바이트 수(big-endian).
	 *  - 역할: 헤더 뒤에 따라오는 Packet 영역의 길이. 호스트 파서가 이 만큼 디코드.
	 *  - 설정자: 호스트가 send 시 자체 계산해 채움. 디바이스가 receive 시 응답 길이로 채움.
	 *  - 값 범위: 0(헤더만) ~ DMA 버퍼 크기.
	 *  - 동기화: 단일 호스트 admin queue thread만 접근 — 동기화 불요. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_compacket) == 20, "Incorrect size");
/* [한국어] ComPacket 헤더 = 20B 검증 (TCG 3.2.3.1). */

/* packet header format */
/* [한국어] === Packet 헤더 — Session ID(HSN/TSN)와 Sequence Number를 운반 ===
 *          한 세션 안의 모든 Packet은 동일 HSN/TSN 짝을 갖는다. StartSession 응답에서 디바이스가
 *          TSN을 부여하기 전까지 호스트는 HSN만 임의 random으로 채워 보낸다. */
struct __attribute__((packed)) spdk_opal_packet {
	uint32_t session_tsn;
	/* [한국어] TPer Session Number(TSN, big-endian).
	 *  - 역할: 디바이스가 부여하는 세션 식별자. 디바이스 측 세션 자원 lookup 키.
	 *  - 설정자: 첫 StartSession Send에서는 호스트가 0으로 채움 → 디바이스 응답에서 새 TSN 부여 →
	 *    호스트가 nvme_opal_internal의 session.tsn에 저장 → 이후 모든 Packet에 그대로 복사.
	 *  - 읽는 자: 디바이스 펌웨어 세션 매니저가 어느 세션의 메서드인지 식별.
	 *  - 값 범위: 0 (첫 StartSession Send) 또는 디바이스 부여 32비트 양의 정수.
	 *    EndSession 후 무효.
	 *  - 동기화: 한 세션 = 단일 호스트 thread → 별도 락 불요. */
	uint32_t session_hsn;
	/* [한국어] Host Session Number(HSN, big-endian).
	 *  - 역할: 호스트가 임의로 생성한 세션 식별자. 디바이스는 응답에 그대로 미러링.
	 *    호스트가 동시 다중 세션 응답을 구분할 때 키로 사용 (SPDK 단일 세션이어도 필수 채움).
	 *  - 설정자: 호스트 nvme_opal.c::opal_start_session이 random 32비트로 생성, 이후 모든 Packet에 동일 값.
	 *  - 읽는 자: 디바이스(미러링) / 호스트(응답 디스패치).
	 *  - 값 범위: 임의의 0이 아닌 32비트 정수.
	 *  - 동기화: 단일 thread 소유 — 락 불요. */
	uint32_t seq_number;
	/* [한국어] 패킷 시퀀스 번호(big-endian).
	 *  - 역할: ACK/NACK 모드에서 패킷 순서 추적용.
	 *  - 설정자: ACK 사용 시 호스트 / 미사용 시 0. SPDK는 항상 0.
	 *  - 읽는 자: 디바이스 ACK 매니저(SPDK 미사용).
	 *  - 값 범위: 0..2^32-1. 동기화: 불요. */
	uint16_t reserved;
	/* [한국어] 예약 2B(0). 정렬용. */
	uint16_t ack_type;
	/* [한국어] ACK 종류(big-endian).
	 *  - 역할: 0이면 ACK 미사용, 1=ACK, 2=NAK 등. SPDK는 sync 단발 모드라 항상 0.
	 *  - 설정자: 호스트. 읽는 자: 디바이스.
	 *  - 값 범위: 0..N. 동기화: 불요. */
	uint32_t acknowledgment;
	/* [한국어] 마지막으로 ACK한 패킷 시퀀스 번호(big-endian).
	 *  - 역할: ack_type!=0일 때만 의미. ACK된 마지막 seq를 가리킴.
	 *  - 설정자/읽는 자: 호스트·디바이스 ACK 매니저(SPDK 미사용).
	 *  - 값 범위: 0..2^32-1. 동기화: 불요. SPDK 0 고정. */
	uint32_t length;
	/* [한국어] Packet 페이로드(Subpacket들) 총 바이트 수(big-endian, 4B 정렬 패딩 제외).
	 *  - 역할: Packet 헤더 뒤 Subpacket 영역 길이. 호스트 파서가 이 만큼 Subpacket 디코드.
	 *  - 설정자: 호스트(send) / 디바이스(receive 응답).
	 *  - 값 범위: 12(빈 Subpacket) ~ ComPacket length 한도.
	 *  - 동기화: 단일 thread — 동기화 불요. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_packet) == 24, "Incorrect size");
/* [한국어] Packet 헤더 = 24B 검증 (TCG 3.2.3.2). */

/* data subpacket header */
/* [한국어] === Subpacket 헤더 — 실제 토큰 스트림(또는 raw bytes)의 컨테이너 ===
 *          kind 필드로 data 종류 구분. 끝에 4B 정렬 패딩이 따라붙지만 length는 패딩 미포함. */
struct __attribute__((packed)) spdk_opal_data_subpacket {
	uint8_t reserved[6];
	/* [한국어] 예약 6B(0).
	 *  - 역할: Subpacket 헤더 앞쪽 정렬 패딩. TCG가 향후 확장 슬롯으로 비워둠.
	 *  - 설정자: 호스트/디바이스 모두 0으로 채움. 읽는 자: 양쪽 무시.
	 *  - 값 범위: 모두 0. 동기화: 불요. */
	uint16_t kind;
	/* [한국어] Subpacket 종류(big-endian).
	 *  - 역할: 페이로드 해석 방식을 결정.
	 *    0x0000 = data subpacket (SWG 토큰 스트림 — SPDK 기본),
	 *    0x8000 이상 = credit control 등 특수 subpacket (SPDK 미사용).
	 *  - 설정자: 호스트(send 시 0) / 디바이스(receive 응답).
	 *  - 읽는 자: 호스트 파서가 토큰 디코더로 라우팅 / 디바이스가 페이로드 핸들러 선택.
	 *  - 값 범위: 0x0000, 0x8000~ (TCG SAC v2.01 3.2.3.3).
	 *  - 동기화: 단일 thread — 불요. */
	uint32_t length;
	/* [한국어] Subpacket 페이로드 바이트 수(big-endian, 4B align 패딩 제외).
	 *  - 역할: 헤더 직후 토큰 스트림(또는 raw 데이터)의 정확한 길이.
	 *    호스트는 length만큼 디코드 후 (4 - length%4) % 4 바이트를 align padding으로 skip.
	 *  - 설정자: 호스트(send 시 자체 계산) / 디바이스(응답).
	 *  - 읽는 자: 호스트 토큰 파서 / 디바이스 토큰 디코더.
	 *  - 값 범위: 0..(Packet length - 12). 동기화: 단일 thread — 불요. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_data_subpacket) == 12, "Incorrect size");
/* [한국어] Subpacket 헤더 = 12B 검증 (TCG 3.2.3.3). */

#ifdef __cplusplus
/* [한국어] C++ extern "C" 블록 종료 — 이 헤더의 C 심볼들을 C++ 호출자가 mangling 없이 사용. */
}
#endif

#endif
/* [한국어] SPDK_OPAL_SPEC_H 가드 종료. */
