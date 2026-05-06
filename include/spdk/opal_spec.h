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
/* [한국어] tiny atom 첫 바이트 상한값. 첫 비트(0x80)가 0이면 tiny atom 으로 판정.
 *          페이로드 = 첫 바이트 자체의 하위 6비트(부호 비트 포함). 가장 흔한 enum/플래그/숫자 인코딩. */
#define SPDK_SHORT_ATOM_TYPE_MAX		0xBF
/* [한국어] short atom 첫 바이트 상한값. 첫 두 비트(10b)가 패턴 매치되면 short.
 *          이후 0~15바이트 페이로드 (UID 8B, 패스워드 등 대부분의 데이터가 여기). */
#define SPDK_MEDIUM_ATOM_TYPE_MAX		0xDF
/* [한국어] medium atom 첫 바이트 상한값. 첫 세 비트(110b)가 매치. 헤더 2B + 페이로드 0~2047B. */
#define SPDK_LONG_ATOM_TYPE_MAX			0xE3
/* [한국어] long atom 첫 바이트 상한값. 5비트(11100b)가 매치. 헤더 4B + 최대 ~16MB.
 *          0xE4 이상은 모두 control token 영역 (enum spdk_opal_token 참조). */

#define SPDK_TINY_ATOM_SIGN_FLAG		0x40
/* [한국어] tiny atom 첫 바이트의 signed/unsigned 표시 비트. 1이면 signed integer,
 *          0이면 unsigned. 6비트 페이로드를 부호확장할지 결정 (TCG 5.1.3.1.1). */

#define SPDK_TINY_ATOM_DATA_MASK		0x3F
/* [한국어] tiny atom 첫 바이트에서 페이로드 6비트만 추출하기 위한 마스크.
 *          (first_byte & DATA_MASK) ⇒ 실제 정수 값(0..63 또는 부호확장 -32..31). */

#define SPDK_SHORT_ATOM_ID			0x80
/* [한국어] short atom 시그니처 비트. (first & 0xC0) == 0x80 이면 short atom.
 *          전체 종류 식별 시 이 매크로와 OR 또는 비교 연산을 함께 사용. */
#define SPDK_SHORT_ATOM_BYTESTRING_FLAG		0x20
/* [한국어] short atom의 첫 바이트 비트5 — 1이면 byte-string(이진 데이터, 패스워드/UID),
 *          0이면 integer(부호 또는 무부호 정수). UID 8바이트는 byte-string으로 전송. */
#define SPDK_SHORT_ATOM_SIGN_FLAG		0x10
/* [한국어] short atom integer일 때만 의미 — 1이면 signed, 0이면 unsigned.
 *          BYTESTRING_FLAG=1이면 이 비트는 무시(reserved). */
#define SPDK_SHORT_ATOM_LEN_MASK		0x0F
/* [한국어] short atom 페이로드 길이(0~15바이트)를 첫 바이트 하위 4비트에서 추출.
 *          UID byte-string은 length=8, MSID 패스워드는 length=가변(<=15). */

#define SPDK_MEDIUM_ATOM_ID			0xC0
/* [한국어] medium atom 시그니처. (first & 0xE0) == 0xC0이면 medium atom.
 *          medium은 헤더가 2바이트 — 첫 바이트에 플래그+길이상위3비트, 둘째 바이트에 길이하위8비트. */
#define SPDK_MEDIUM_ATOM_BYTESTRING_FLAG	0x10
/* [한국어] medium atom 첫 바이트 비트4 — byte-string vs integer 구분.
 *          큰 패스워드/Datastore 데이터를 wire에 실을 때 사용. */

#define SPDK_MEDIUM_ATOM_SIGN_FLAG		0x08
/* [한국어] medium atom integer 부호 표시 비트. */
#define SPDK_MEDIUM_ATOM_LEN_MASK		0x07
/* [한국어] medium atom 첫 바이트의 길이 상위 3비트 마스크.
 *          전체 길이 = ((first & 0x07) << 8) | second_byte → 0~2047. */

#define SPDK_LONG_ATOM_ID			0xE0
/* [한국어] long atom 시그니처. (first & 0xFC) == 0xE0이면 long atom.
 *          헤더 4바이트 — 첫 바이트에 플래그, 다음 3바이트에 길이(big-endian 24비트). */
#define SPDK_LONG_ATOM_BYTESTRING_FLAG		0x02
/* [한국어] long atom 첫 바이트 비트1 — byte-string 여부.
 *          MBR 이미지 같은 큰 페이로드(>2KB)를 한 atom에 담을 때 사용. */
#define SPDK_LONG_ATOM_SIGN_FLAG		0x01
/* [한국어] long atom integer 부호 비트(거의 사용되지 않음 — 큰 정수는 드뭄). */

/*
 * TCG Storage Architecture Core Spec v2.01 r1.00
 * Table-26 ComID management
 */
#define LV0_DISCOVERY_COMID			0x01
/* [한국어] Level 0 Discovery 전용 예약 COMID. NVMe Security Receive 명령의
 *          SECP=0x01(TCG), SP_SPECIFIC=0x0001 로 호출하면 디바이스가 1KB 디스커버리
 *          응답(struct spdk_opal_d0_hdr 시작)을 반환한다. 정상 세션은 디바이스가
 *          discovery 응답 안에서 동적 base_comid를 알려주며 호스트는 그 COMID로 전환. */

/*
 * TCG Storage Opal v2.01 r1.00
 * 5.2.3 Type Table Modification
 */
#define OPAL_MANUFACTURED_INACTIVE		0x08
/* [한국어] Locking SP의 Life Cycle State 값 중 하나 — "Manufactured-Inactive".
 *          출하 직후 Locking SP가 비활성 상태임을 의미하며, Activate 메서드를 호출해야
 *          Range/Authority가 사용 가능해진다. SP::LifeCycle 컬럼 비교용 상수. */

#define LOCKING_RANGE_NON_GLOBAL		0x03
/* [한국어] LockingTable에서 GlobalRange가 아닌 일반 Range를 식별하기 위한 표식 값.
 *          (전역 Range는 row 0번, 사용자 정의 Range는 row 1..MAX) — 코드에서 row 종류
 *          판별/검증 시 사용. */

#define SPDK_OPAL_MAX_PASSWORD_SIZE		32 /* in bytes */
/* [한국어] SPDK Opal 호스트 측 패스워드 버퍼 상한(32바이트).
 *          C_PIN_SID/Admin1/User1 등 모든 PIN 입력에 공통 적용 — 스펙은 더 큰 PIN을
 *          허용하지만, SPDK는 운영상 32B로 제한 (UI/RPC 입력 길이 검증에 사용). */

#define SPDK_OPAL_MAX_LOCKING_RANGE		8 /* maximum 8 ranges defined  by spec */
/* [한국어] Locking Range 최대 개수. Opal SSC 2.0이 의무화하는 최소 Range 수는 8개이며
 *          (Range 0=Global + Range 1..7), SPDK는 그 상한까지만 관리한다.
 *          런타임 Range 배열 인덱스 검증에 사용. */

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
	/* [한국어] TPer Feature (Trusted Peripheral 자체의 기본 능력).
	 *          페이로드: struct spdk_opal_d0_tper_feat (sync/async/comid_management 등).
	 *          어느 SED든 반드시 보고하는 의무 feature — 없으면 TCG 미지원. */

	FEATURECODE_LOCKING	= 0x0002,
	/* [한국어] Locking Feature — Locking SP의 활성/비활성/암호화 상태 비트맵 보고.
	 *          페이로드: struct spdk_opal_d0_locking_feat. host가 SED가 이미 락 걸려 있는지,
	 *          MBR shadowing이 활성인지를 확인할 때 이 feature를 파싱. */

	/*
	 * Opal SSC 1.00 r3.00 Final
	 * 3.1.1.4 Opal SSC Feature
	 */
	FEATURECODE_OPALV100	= 0x0200,
	/* [한국어] Opal SSC v1.0 디바이스가 보고하는 SSC feature.
	 *          페이로드: struct spdk_opal_d0_v100_feat — base_comid/number_comids 등.
	 *          SPDK는 v2를 우선 사용하지만 후방호환을 위해 v1 디스크립터도 인식. */

	/*
	 * TCG Storage Opal v2.01 r1.00
	 * 3.1.1.4 Geometry Reporting Feature
	 * 3.1.1.5 Opal SSC V2.00 Feature
	 */
	FEATURECODE_OPALV200	= 0x0203,
	/* [한국어] Opal SSC v2.0 SSC feature 코드. 페이로드: struct spdk_opal_d0_v200_feat.
	 *          base_comid·num_comids·num_locking_admin_auth·num_locking_user_auth·initial_pin·
	 *          reverted_pin 등 운영에 필수 정보 다수 포함. SPDK는 이 값으로 어떤 COMID를 통해
	 *          세션을 열지 결정한다. */
	FEATURECODE_GEOMETRY	= 0x0003,
	/* [한국어] Geometry Reporting Feature — LBA 정렬/논리블록크기/lowest_aligned_lba 보고.
	 *          페이로드: struct spdk_opal_d0_geo_feat. Range 생성 시 Range start/length가
	 *          이 정렬 경계에 맞아야 디바이스가 받아준다 → setup_locking_range에서 확인용. */

	/*
	 * TCG Storage Opal Feature Set Single User Mode v1.00 r2.00
	 * 4.2.1 Single User Mode Feature Descriptor
	 */
	FEATURECODE_SINGLEUSER	= 0x0201,
	/* [한국어] Single User Mode 지원 보고. 페이로드: struct spdk_opal_d0_single_user_mode_feat.
	 *          여러 사용자 권한을 사용하지 않고 한 명이 모든 Range를 단독 관리하는 모드 —
	 *          운영 단순화용. SPDK는 이 feature 보고 시 num_locking_objects를 참고. */

	/*
	 * TCG Storage Opal Feature Set Additional DataStore Tables v1.00 r1.00
	 * 4.1.1 DataStore Table Feature Descriptor
	 */
	FEATURECODE_DATASTORE	= 0x0202,
	/* [한국어] Additional DataStore Tables feature — 호스트가 디바이스 내부에 저장 가능한
	 *          작은 KV 영역(boot 정보 등)의 max_tables / max_table_size / alignment 보고.
	 *          페이로드: struct spdk_opal_d0_datastore_feat. */
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
	/* [한국어] Boolean true. cell_block 값/컬럼 값으로 등장.
	 *          예: ReadLocked = TRUE 로 Set 메서드 호출 시 인자로 전달.
	 *          관련: SPDK_OPAL_FALSE 와 한 쌍. */
	SPDK_OPAL_FALSE			= 0x00,
	/* [한국어] Boolean false. ReadLockEnabled=FALSE 등 락 비활성화 인자로 사용.
	 *          tiny atom 0x00은 동시에 unsigned 0 정수이기도 하므로 컨텍스트 종속. */

	/* cell_block
	 * 5.1.4.2.3 */
	SPDK_OPAL_TABLE			= 0x00,
	/* [한국어] cell_block 키 "Table" — Get/Set 메서드 인자 named-value의 이름 식별자.
	 *          (StartName 0xF2 뒤에 tiny atom 0x00 으로 등장). */
	SPDK_OPAL_STARTROW		= 0x01,
	/* [한국어] cell_block 키 "StartRow" — 부분 row 범위 지정 시 시작 row 번호 표식. */
	SPDK_OPAL_ENDROW		= 0x02,
	/* [한국어] cell_block 키 "EndRow" — 부분 row 범위의 끝 row 번호 표식. */
	SPDK_OPAL_STARTCOLUMN		= 0x03,
	/* [한국어] cell_block 키 "StartColumn" — 시작 컬럼 번호. */
	SPDK_OPAL_ENDCOLUMN		= 0x04,
	/* [한국어] cell_block 키 "EndColumn" — 끝 컬럼 번호. */
	SPDK_OPAL_VALUES		= 0x01,
	/* [한국어] Set 메서드의 named-value 키 "Values" — 새로 쓸 값 리스트의 이름 표식.
	 *          STARTROW(0x01)와 같은 값이지만 메서드/파라미터 컨텍스트로 구분. */

	/* C_PIN table
	 * 5.3.2.12 */
	SPDK_OPAL_PIN			= 0x03,
	/* [한국어] C_PIN 객체의 "PIN" 컬럼 번호. C_PIN_SID/C_PIN_Admin1 등 PIN row를
	 *          Get/Set할 때 이 컬럼 인덱스를 named-value 키로 사용. */

	/* locking table
	 * 5.7.2.2 */
	SPDK_OPAL_RANGESTART		= 0x03,
	/* [한국어] LockingTable row의 컬럼 3 — RangeStart (LBA in blocks). */
	SPDK_OPAL_RANGELENGTH		= 0x04,
	/* [한국어] LockingTable 컬럼 4 — RangeLength (블록 수). */
	SPDK_OPAL_READLOCKENABLED	= 0x05,
	/* [한국어] LockingTable 컬럼 5 — ReadLockEnabled (read 락 메커니즘 자체 활성화). */
	SPDK_OPAL_WRITELOCKENABLED	= 0x06,
	/* [한국어] LockingTable 컬럼 6 — WriteLockEnabled (write 락 메커니즘 활성화). */
	SPDK_OPAL_READLOCKED		= 0x07,
	/* [한국어] LockingTable 컬럼 7 — ReadLocked (현재 read 차단 여부). 활성화 비트가
	 *          켜져 있어야 이 값이 효과를 가짐. */
	SPDK_OPAL_WRITELOCKED		= 0x08,
	/* [한국어] LockingTable 컬럼 8 — WriteLocked (현재 write 차단 여부). */
	SPDK_OPAL_ACTIVEKEY		= 0x0A,
	/* [한국어] LockingTable 컬럼 10 — ActiveKey (이 Range의 미디어 암호화 키 객체 UID
	 *          참조). GenKey 메서드로 새 키 생성 시 이 컬럼이 새 K_AES UID로 갱신. */

	/* locking info table */
	SPDK_OPAL_MAXRANGES		= 0x04,
	/* [한국어] LockingInfo 객체의 "MaxRanges" 컬럼 — 디바이스가 지원하는 최대 Range 수.
	 *          Get으로 읽어 SPDK_OPAL_MAX_LOCKING_RANGE와 비교. */

	/* mbr control */
	SPDK_OPAL_MBRENABLE		= 0x01,
	/* [한국어] MBRControl 객체 컬럼 1 — Enable (MBR shadowing 활성). */
	SPDK_OPAL_MBRDONE		= 0x02,
	/* [한국어] MBRControl 컬럼 2 — Done (Pre-boot 종료 표식 — true로 set하면 실제
	 *          유저 LBA가 보임, false면 shadow MBR이 보임). */

	/* properties */
	SPDK_OPAL_HOSTPROPERTIES	= 0x00,
	/* [한국어] Properties 메서드 호출 시 named-arg "HostProperties"의 이름 식별자.
	 *          호스트가 자신의 buffer 한계 등을 디바이스에 통보하는 인자에 사용. */

	/* control tokens */
	SPDK_OPAL_STARTLIST		= 0xF0,
	/* [한국어] control token: list 시작. 모든 메서드 인자/응답 list는 [STARTLIST ... ENDLIST]로 감싼다. */
	SPDK_OPAL_ENDLIST		= 0xF1,
	/* [한국어] control token: list 종료. STARTLIST와 짝. 중첩 list도 가능. */
	SPDK_OPAL_STARTNAME		= 0xF2,
	/* [한국어] control token: named-value 시작. 뒤에 [name_atom][value_atom] 두 토큰이 따라옴
	 *          (Get의 cell_block, Set의 column→value 매핑 등). */
	SPDK_OPAL_ENDNAME		= 0xF3,
	/* [한국어] control token: named-value 종료. STARTNAME ... ENDNAME 한 쌍. */
	SPDK_OPAL_CALL			= 0xF8,
	/* [한국어] control token: 메서드 호출 시작. 패턴: [CALL][object_uid 8B short atom][method_uid 8B][arg list][EOD][status list]
	 *          토큰 스트림에서 호스트가 디바이스에 메서드를 dispatch하는 시그널. */
	SPDK_OPAL_ENDOFDATA		= 0xF9,
	/* [한국어] control token: 메서드 인자 종료(End-of-Data). CALL 패킷의 인자 list 직후 등장하며,
	 *          그 뒤에 status list가 따라온다 (성공/실패 코드 3개 정수 + ENDLIST). */
	SPDK_OPAL_ENDOFSESSION		= 0xFA,
	/* [한국어] control token: 세션 종료. 단독으로 또는 [STARTLIST EOS ENDLIST] 형태로 보내
	 *          현재 세션(HSN/TSN)을 닫음. 디바이스도 동일 토큰으로 응답. */
	SPDK_OPAL_STARTTRANSACTON	= 0xFB,
	/* [한국어] control token: 트랜잭션 시작 (멀티 메서드 atomic 묶음 시작 표식).
	 *          현재 SPDK 호스트 코드는 사용하지 않지만 스펙 정의를 위해 노출. */
	SPDK_OPAL_ENDTRANSACTON		= 0xFC,
	/* [한국어] control token: 트랜잭션 종료. STARTTRANSACTION과 짝. */
	SPDK_OPAL_EMPTYATOM		= 0xFF,
	/* [한국어] control token: 빈 atom (padding/placeholder). 디바이스가 응답에 빈 셀을
	 *          표시할 때 사용 (예: 컬럼 값이 없음). */
	SPDK_OPAL_WHERE			= 0x00,
	/* [한국어] Next 메서드의 named-arg "Where" 키 — 어디부터 row를 열거할지 지정.
	 *          tiny atom 0x00 (= TABLE/FALSE와 동일 값)이지만 메서드 컨텍스트로 구분. */

	/* life cycle */
	SPDK_OPAL_LIFECYCLE		= 0x06,
	/* [한국어] SP 객체의 LifeCycle 컬럼 번호 — Get/Set 시 컬럼 식별자.
	 *          값으로는 OPAL_MANUFACTURED_INACTIVE(0x08) 등이 와이어에 실림. */

	/* Authority table */
	SPDK_OPAL_AUTH_ENABLE		= 0x05,
	/* [한국어] Authority 객체 컬럼 5 — Enabled (해당 Authority의 사용 가능 여부).
	 *          User1~User8 활성/비활성 토글 시 Set 인자에 사용. */

	/* ACE table */
	SPDK_OPAL_BOOLEAN_EXPR		= 0x03,
	/* [한국어] ACE(Access Control Element) row의 컬럼 3 — BooleanExpression
	 *          (어느 Authority가 이 작업을 수행할 수 있는지를 표현하는 boolean 식).
	 *          예: ACE_Locking_Range1_Set_RdLocked의 BooleanExpr를 갱신하여 User1이 락 풀 수 있게 허용. */
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
	/* [한국어] 헤더 자신을 제외한 feature descriptor 영역의 총 바이트 길이 (big-endian).
	 *          설정자: 디바이스 펌웨어. 읽는 자: 호스트 nvme_opal.c::opal_check_support()가
	 *          이 값을 보고 feature 순회 루프 종료 조건으로 사용.
	 *          값 범위: 0..1024-48 정도(보통 100~200B). 동기화: 응답 버퍼는 단일 호스트 스레드만 접근. */
	uint32_t revision;
	/* [한국어] 디스커버리 헤더 포맷 리비전 (현재는 1로 고정).
	 *          호스트가 미래 버전 지원 시 분기 처리 가능. 현재 SPDK는 무시. */
	uint32_t reserved_0;
	/* [한국어] 예약(0) 영역. 디바이스가 0으로 채워야 함 — 호스트는 검증 없이 무시. */
	uint32_t reserved_1;
	/* [한국어] 예약(0) 영역. 향후 스펙 확장용 슬롯. */
	uint8_t vendor_specfic[32];
	/* [한국어] 디스크 벤더가 자유롭게 정의 가능한 32바이트 — 모델별 메타데이터.
	 *          SPDK는 사용하지 않으나 디버깅 시 hexdump 출력에 활용 가능.
	 *          동기화: read-only 응답 버퍼 안이라 별도 락 불필요. */
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
	/* [한국어] feature 종류 식별자 (big-endian) — enum spdk_lv0_discovery_feature_code 값.
	 *          호스트가 switch(code) 분기로 어느 feat 구조체로 캐스팅할지 결정. */
	uint8_t		reserved : 4;
	/* [한국어] 예약 4비트 — 디바이스가 0으로 채움. 호스트는 무시. */
	uint8_t		version : 4;
	/* [한국어] 이 feature descriptor 자신의 버전(0~15). 동일 code의 포맷이 향후 확장될 때
	 *          version으로 구분. 호스트는 알려진 version만 파싱하고 모르는 version은 skip. */
	uint8_t		length;
	/* [한국어] 이 헤더를 제외한 feature 페이로드 바이트 수. 즉 한 feature 디스크립터 전체
	 *          크기 = sizeof(hdr) + length. 호스트가 다음 feature까지 점프할 때 사용. */
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
	/* [한국어] feature 공통 헤더 (code = FEATURECODE_TPER = 0x0001).
	 *          호스트는 hdr.code로 이 디스크립터 인지 확인 후 캐스팅. */
	uint8_t sync : 1;
	/* [한국어] 동기 통신 지원 여부 — 1이면 보내고 같은 IF-RECV로 응답 회수 가능.
	 *          SPDK는 폴링 모델이라 sync 모드 사용 (Security Send → 짧은 대기 → Receive). */
	uint8_t async : 1;
	/* [한국어] 비동기 통신 지원 여부. 별도 인터럽트/이벤트로 응답 통지.
	 *          현재 SPDK는 sync만 사용. */
	uint8_t acknack : 1;
	/* [한국어] ACK/NACK 시퀀스 지원 — 패킷 단편 전달 시 신뢰성 보장.
	 *          단일 패킷 통신이 일반적이라 SPDK는 사용하지 않음. */
	uint8_t buffer_management : 1;
	/* [한국어] 디바이스 측 버퍼 협상 기능 — 큰 데이터(MBR 이미지 등)를 분할 전송하는데 필요.
	 *          현재 사용 안 함(필요 시 host_properties 메서드로 협상). */
	uint8_t streaming : 1;
	/* [한국어] streaming 통신(연속 페이로드) 지원. SPDK는 단발 메서드 호출만 사용. */
	uint8_t reserved_1 : 1;
	/* [한국어] 예약 비트 — 0. */
	uint8_t comid_management : 1;
	/* [한국어] ComID 동적 관리(요청/해제) 지원 여부. 1이면 별도 메서드로 임시 ComID 할당 가능.
	 *          SPDK는 v200 feat의 base_comid를 정적으로 사용하므로 활용하지 않음. */
	uint8_t reserved_2 : 1;
	/* [한국어] 예약 비트. */

	uint8_t reserved_3[3];
	/* [한국어] 예약 3바이트(0). 비트맵 정렬을 위해 추가됨. */
	uint32_t reserved_4;
	/* [한국어] 예약 4바이트(0). 향후 capability 확장 슬롯. */
	uint32_t reserved_5;
	/* [한국어] 예약 4바이트(0). 동일. */
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
	/* [한국어] code = FEATURECODE_LOCKING = 0x0002. */
	uint8_t locking_supported : 1;
	/* [한국어] 디바이스가 Locking SP 자체를 지원하는지(Opal 의무 — 보통 1). */
	uint8_t locking_enabled : 1;
	/* [한국어] Locking SP가 활성화되었는지(Activate 메서드 수행됨).
	 *          0이면 출하 직후 — Admin1 PIN 변경 후 Activate 필요. */
	uint8_t locked : 1;
	/* [한국어] 어떤 Range든 현재 Read 또는 Write 락이 걸려 있는지를 요약 표시.
	 *          호스트는 이 비트로 "락 해제 시도가 필요한가"를 판단. */
	uint8_t media_encryption : 1;
	/* [한국어] 미디어 자체 암호화(SED) 지원 — Opal SED는 항상 1.
	 *          0이면 단순 락만 있고 데이터는 평문 → SPDK는 보통 1만 다룸. */
	uint8_t mbr_enabled : 1;
	/* [한국어] MBR shadowing이 활성화되어 있는지. */
	uint8_t mbr_done : 1;
	/* [한국어] MBR Done 비트 — true면 사용자 LBA 보임, false면 shadow MBR 보임. */
	uint8_t reserved_1 : 1;
	/* [한국어] 예약 비트(0). */
	uint8_t reserved_2 : 1;
	/* [한국어] 예약 비트(0). */

	uint8_t reserved_3[3];
	/* [한국어] 예약 패딩 3B. */
	uint32_t reserved_4;
	/* [한국어] 예약 4B. */
	uint32_t reserved_5;
	/* [한국어] 예약 4B. */
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
	/* [한국어] code = FEATURECODE_SINGLEUSER = 0x0201. */
	uint32_t num_locking_objects;
	/* [한국어] Single User Mode에서 사용 가능한 Locking 객체 개수 (big-endian).
	 *          Opal v2 디바이스가 보고하는 Range 수와 별개로 Single User Mode 한정 카운트. */
	uint8_t any : 1;
	/* [한국어] "Any" 정책 — 임의의 한 사용자가 단독 소유 가능. */
	uint8_t all : 1;
	/* [한국어] "All" 정책 — 모든 Range가 동시에 Single User Mode로 운영. */
	uint8_t policy : 1;
	/* [한국어] policy 비트 — 0이면 "Owner sets policy", 1이면 "Authority sets policy". */
	uint8_t reserved_1 : 5;
	/* [한국어] 예약 5비트(0). */

	uint8_t reserved_2;
	/* [한국어] 예약 1B(0). */
	uint16_t reserved_3;
	/* [한국어] 예약 2B(0). */
	uint32_t reserved_4;
	/* [한국어] 예약 4B(0). */
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
	/* [한국어] code = FEATURECODE_GEOMETRY = 0x0003. */
	uint8_t align : 1;
	/* [한국어] 정렬 강제 여부 — 1이면 Range start/length가 alignment_granularity 배수여야 함.
	 *          0이면 임의 LBA 가능 (드물다). */
	uint8_t reserved_1 : 7;
	/* [한국어] 예약 7비트. */
	uint8_t reserved_2[7];
	/* [한국어] 예약 7B(0). */
	uint32_t logical_block_size;
	/* [한국어] 논리 블록 크기 (보통 512 또는 4096 바이트, big-endian).
	 *          Range 길이 계산 시 단위 변환 기준. */
	uint64_t alignment_granularity;
	/* [한국어] 정렬 단위(블록 수) — Range start와 length가 이 값의 배수여야 함. */
	uint64_t lowest_aligned_lba;
	/* [한국어] 정렬이 시작되는 최소 LBA. 일부 디바이스가 앞쪽 일부 LBA를 정렬에서 제외. */
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
	/* [한국어] code = FEATURECODE_DATASTORE = 0x0202. */
	uint16_t reserved_1;
	/* [한국어] 예약 2B(0). */
	uint16_t max_tables;
	/* [한국어] 디바이스가 지원하는 추가 DataStore 테이블 최대 개수 (big-endian).
	 *          호스트는 0..max_tables-1 인덱스로 테이블 사용. */
	uint32_t max_table_size;
	/* [한국어] 한 테이블의 최대 바이트 수 (big-endian). 큰 페이로드 분할 시 기준. */
	uint32_t alignment;
	/* [한국어] DataStore 접근 시 LBA/오프셋 정렬 단위 (big-endian). */
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
	/* [한국어] code = FEATURECODE_OPALV100 = 0x0200. */
	uint16_t base_comid;
	/* [한국어] 호스트가 일반 세션에 사용해야 하는 시작 ComID (big-endian).
	 *          Discovery COMID(0x01) 와는 별개. nvme_opal_internal의 session.comid가 이 값에서 출발. */
	uint16_t number_comids;
	/* [한국어] base_comid부터 사용 가능한 연속 ComID 개수. 멀티 세션 시 다음 ComID 결정. */
	uint8_t range_crossing : 1;
	/* [한국어] Range를 가로지르는 read/write가 허용되는지(1) 아니면 디바이스가 차단하는지(0).
	 *          호스트가 큰 IO를 분할할지 판단하는 데 사용. */

	uint8_t reserved_1 : 7;
	/* [한국어] 예약 7비트. */
	uint8_t reserved_2;
	/* [한국어] 예약 1B. */
	uint16_t reserved_3;
	/* [한국어] 예약 2B. */
	uint32_t reserved_4;
	/* [한국어] 예약 4B. */
	uint32_t reserved_5;
	/* [한국어] 예약 4B. */
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
	/* [한국어] code = FEATURECODE_OPALV200 = 0x0203. */
	uint16_t base_comid;
	/* [한국어] 일반 세션 시작 ComID (big-endian). 호스트는 이 ComID로 ComPacket 헤더의 comid
	 *          필드를 채워 StartSession 메서드 패킷을 발사. */
	uint16_t num_comids;
	/* [한국어] base_comid부터 사용 가능한 ComID 수. */
	uint8_t range_crossing : 1;
	/* [한국어] Range를 가로지르는 IO 허용 여부 (v100과 동일 의미). */
	uint8_t reserved_1 : 7;
	/* [한국어] 예약 7비트. */
	uint16_t num_locking_admin_auth; /* Number of Locking SP Admin Authorities Supported */
	/* [한국어] Locking SP에서 지원하는 Admin Authority 수 (big-endian) — 보통 4 (Admin1~Admin4).
	 *          Activate 후 Admin 권한자 수의 상한으로 작용. */
	uint16_t num_locking_user_auth;
	/* [한국어] User Authority 수 (User1~User8 등). 사용자 단위로 Range 권한 분리 시 상한. */
	uint8_t initial_pin;
	/* [한국어] 초기 SID PIN 정책 코드 — 0x00이면 MSID와 동일, 0xFF면 vendor 정의.
	 *          호스트는 출하 직후 SID 인증 시 어떤 PIN을 시도할지 이 값으로 결정. */
	uint8_t reverted_pin;
	/* [한국어] Revert 후 SID PIN 정책 — Revert(공장 초기화) 메서드 수행 후 SID PIN이
	 *          MSID로 돌아가는지(0x00) vendor 값(0xFF)인지 표시. */

	uint8_t reserved_2;
	/* [한국어] 예약 1B. */
	uint32_t reserved_3;
	/* [한국어] 예약 4B. */
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
	/* [한국어] 예약 4B(0). 모든 ComPacket 시작에 위치. */
	uint8_t comid[2];
	/* [한국어] 이 ComPacket의 대상 ComID (2B big-endian, byte 배열로 선언된 이유는 정렬 회피).
	 *          호스트→디바이스 send 시 v200_feat.base_comid 또는 LV0_DISCOVERY_COMID(0x01)을 기록.
	 *          디바이스 응답에는 동일 ComID가 그대로 들어옴 → 호스트가 매칭에 사용. */
	uint8_t extended_comid[2];
	/* [한국어] 확장 ComID — 일반적으로 0. 디바이스가 ComID 동적 관리 지원 시 0이 아닌 값 사용. */
	uint32_t outstanding_data;
	/* [한국어] 디바이스가 호스트에 보낼 추가 응답 데이터 바이트 수 (Receive 응답에서 의미).
	 *          호스트는 이 값을 보고 Security Receive를 한 번 더 호출할지 결정. */
	uint32_t min_transfer;
	/* [한국어] 디바이스가 권장하는 최소 전송 단위 — 호스트가 다음 Receive 시 이 만큼 버퍼 준비. */
	uint32_t length;
	/* [한국어] 이 ComPacket 헤더 뒤에 따라오는 Packet들의 총 바이트 수 (big-endian).
	 *          호스트는 이 길이만큼 페이로드 영역을 파싱. 0이면 추가 페이로드 없음. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_compacket) == 20, "Incorrect size");
/* [한국어] ComPacket 헤더 = 20B 검증 (TCG 3.2.3.1). */

/* packet header format */
/* [한국어] === Packet 헤더 — Session ID(HSN/TSN)와 Sequence Number를 운반 ===
 *          한 세션 안의 모든 Packet은 동일 HSN/TSN 짝을 갖는다. StartSession 응답에서 디바이스가
 *          TSN을 부여하기 전까지 호스트는 HSN만 임의 random으로 채워 보낸다. */
struct __attribute__((packed)) spdk_opal_packet {
	uint32_t session_tsn;
	/* [한국어] TPer Session Number (TSN, big-endian) — 디바이스가 StartSession 메서드 응답에서
	 *          호스트에 부여하는 세션 ID. 첫 StartSession Send에서는 0, 이후 모든 Packet에 디바이스가
	 *          준 값을 그대로 기록. EndSession 후 무효.
	 *          설정자: 디바이스(응답)/호스트(이후 send 시 복사). 읽는 자: 디바이스 펌웨어가 세션 식별. */
	uint32_t session_hsn;
	/* [한국어] Host Session Number (HSN, big-endian) — 호스트가 StartSession 시 임의로 생성한 ID.
	 *          이후 모든 Packet에 동일 값 기록. 디바이스가 응답 패킷에 그대로 미러링.
	 *          호스트는 이 값으로 동시 다중 세션 응답을 구분(SPDK는 단일 세션만 사용해도 필수). */
	uint32_t seq_number;
	/* [한국어] 패킷 시퀀스 번호 — ACK/NACK 사용 시 패킷 순서 추적. SPDK는 0 고정. */
	uint16_t reserved;
	/* [한국어] 예약 2B(0). */
	uint16_t ack_type;
	/* [한국어] ACK 종류 — 0이면 ACK 미사용. SPDK는 sync 단발 모드라 항상 0. */
	uint32_t acknowledgment;
	/* [한국어] 마지막으로 ACK한 시퀀스 번호 — ack_type!=0일 때만 의미. SPDK 0. */
	uint32_t length;
	/* [한국어] 이 Packet 헤더 뒤 Subpacket들의 총 바이트 수 (big-endian, 4B 정렬 패딩 미포함).
	 *          호스트가 페이로드 끝을 식별하는 기준. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_packet) == 24, "Incorrect size");
/* [한국어] Packet 헤더 = 24B 검증 (TCG 3.2.3.2). */

/* data subpacket header */
/* [한국어] === Subpacket 헤더 — 실제 토큰 스트림(또는 raw bytes)의 컨테이너 ===
 *          kind 필드로 data 종류 구분. 끝에 4B 정렬 패딩이 따라붙지만 length는 패딩 미포함. */
struct __attribute__((packed)) spdk_opal_data_subpacket {
	uint8_t reserved[6];
	/* [한국어] 예약 6B(0). Subpacket 헤더의 앞 6바이트 — TCG가 정렬을 위해 비워둠. */
	uint16_t kind;
	/* [한국어] Subpacket 종류 (big-endian).
	 *           0x0000 = data subpacket (SWG 토큰 스트림 — SPDK 기본),
	 *           0x8000 이상 = credit control 등 특수 subpacket (SPDK 미사용).
	 *          호스트 파서는 이 값을 보고 토큰 디코더로 라우팅. */
	uint32_t length;
	/* [한국어] 이 Subpacket 헤더 뒤 페이로드 바이트 수 (big-endian, 4B align 패딩 제외).
	 *          호스트는 length만큼 토큰 스트림을 디코드하고, 그 뒤 (4 - length%4)%4 바이트는
	 *          패딩으로 skip. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_opal_data_subpacket) == 12, "Incorrect size");
/* [한국어] Subpacket 헤더 = 12B 검증 (TCG 3.2.3.3). */

#ifdef __cplusplus
/* [한국어] C++ extern "C" 블록 종료 — 이 헤더의 C 심볼들을 C++ 호출자가 mangling 없이 사용. */
}
#endif

#endif
/* [한국어] SPDK_OPAL_SPEC_H 가드 종료. */
