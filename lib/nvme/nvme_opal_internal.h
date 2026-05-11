/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] TCG Opal SED (Self-Encrypting Drive) 보안 내부 정의 헤더 (nvme_opal_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK가 NVMe SSD의 TCG Opal/Pyrite SSC 보안 기능(SED, Self-Encrypting Drive)을
 * 다루기 위해 필요한 내부 상수, enum, 자료구조, 그리고 표준 UID/Method 바이트열 테이블을 모은 헤더이다.
 * Opal은 호스트가 디스크 컨트롤러 안의 보안 메타데이터(KEK, locking range 등)에
 * "세션"을 열어 명령 패킷(ComPacket → Packet → SubPacket → tokenized payload)을
 * 주고받아 SED를 잠그고 푸는 표준이다 (TCG Storage Architecture Core Spec, TCG SSC).
 * 본 헤더의 정의는 lib/nvme/nvme_opal.c가 NVMe Security Send/Receive 명령으로 페이로드를
 * 빌드/파싱할 때 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   사용자 RPC (bdev_nvme_opal_*) / 공개 API (spdk/opal.h)
 *     → lib/nvme/nvme_opal.c
 *       → 본 헤더의 UID/Method 바이트열로 ComPacket 빌드
 *         → spdk_nvme_ctrlr_security_send/receive (NVMe Admin Cmd 0x81/0x82)
 *           → SSD 내 SED 펌웨어
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/opal_spec.h(스펙 구조체 com_packet 등), spdk/opal.h(공개 타입), scsi_spec.h.
 * - 의존받음: lib/nvme/nvme_opal.c (모든 enum/struct/매크로 사용).
 * - 데이터 흐름: 사용자 키/PIN → opal_session.cmd 버퍼에 토큰 인코딩 → NVMe 보안 명령으로
 *   SSD에 전송 → 응답을 opal_session.resp로 받아 spdk_opal_resp_parsed로 파싱.
 *
 * === 주요 함수/구조체 요약 ===
 * - 매크로 IO_BUFFER_LENGTH 등  : Opal 명령/응답 버퍼 크기, 키 길이, UID 길이 상수.
 * - enum opal_token_type        : Opal 응답 토큰 타입 (TCG spec §3.2.2).
 * - enum opal_atom_width        : Opal atom 폭(tiny/short/medium/long).
 * - enum opal_uid_enum          : 자주 쓰이는 표준 UID 테이블 인덱스.
 * - enum opal_method_enum       : 자주 쓰이는 표준 Method 인덱스.
 * - struct spdk_opal_key        : 비밀번호/PIN 키 구조.
 * - struct spdk_opal_resp_token / resp_parsed: 응답 토큰 파싱 결과.
 * - struct spdk_opal_header     : 응답 ComPacket 헤더(고정 부분).
 * - struct opal_session         : 한 세션의 명령/응답 버퍼와 콜백 ctx.
 * - struct spdk_opal_dev        : ctrlr·comid·feature·locking range를 묶은 Opal 디바이스 핸들.
 */

#ifndef SPDK_OPAL_INTERNAL_H
#define SPDK_OPAL_INTERNAL_H
/* [한국어] 다중 포함 가드. */

#include "spdk/opal_spec.h"  /* [한국어] TCG Opal 스펙에서 정의된 packet 구조체. */
#include "spdk/opal.h"        /* [한국어] 공개 Opal API/타입 (locking_range_info 등). */
#include "spdk/scsi_spec.h"   /* [한국어] SCSI 보안 프로토콜 매크로 일부 차용. */

#define IO_BUFFER_LENGTH		2048
/* [한국어] 단일 Opal 명령/응답 페이로드 버퍼 크기(바이트).
 * TCG spec에서 ComPacket 최대 크기 한계와 일반적인 Opal SSC 명령 크기에 맞춘 값. */

#define MAX_TOKS			64
/* [한국어] 응답 1개를 파싱했을 때 추출되는 토큰 슬롯 최대 개수.
 * 일반 GET/AUTHENTICATE 응답이 64개 이하 토큰으로 충분하다는 휴리스틱. */

#define OPAL_KEY_MAX			256
/* [한국어] PIN/key 바이트열의 최대 길이. TCG 스펙 상 admin password 최대치. */

#define OPAL_UID_LENGTH			8
/* [한국어] 표준 Opal UID는 항상 8바이트 고정 (TCG spec). */

#define GENERIC_HOST_SESSION_NUM	0x69
/* [한국어] 호스트가 부여하는 generic HSN(Host Session Number). 임의 비-0 값.
 * TSN(Target Session Number)은 SSD가 할당해 응답으로 돌려준다. */

#define OPAL_INVAL_PARAM		12
/* [한국어] TCG SP에서 파라미터 무효를 의미하는 상태 코드. */

#define SPDK_DTAERROR_NO_METHOD_STATUS	0x89
/* [한국어] Method 상태 토큰이 응답에 없을 때 SPDK가 반환할 내부 에러 코드. */

enum opal_token_type {
	/* [한국어] Opal 응답에 등장하는 4종 토큰 타입(헤더 1바이트 식별자).
	 * TCG Storage Architecture Core Spec §3.2.2.3 atom token 참조. */

	OPAL_DTA_TOKENID_BYTESTRING	= 0xE0,
	/* [한국어] 가변 길이 바이트열 atom — UID, hash, salt 등에 사용. */

	OPAL_DTA_TOKENID_SINT		= 0xE1,
	/* [한국어] 부호 있는 정수 atom. */

	OPAL_DTA_TOKENID_UINT		= 0xE2,
	/* [한국어] 부호 없는 정수 atom — 상태 코드, 카운터 등에 사용. */

	OPAL_DTA_TOKENID_TOKEN		= 0xE3, /* actual token is returned */
	/* [한국어] tiny token — 1바이트 헤더 자체가 곧 데이터(예: 시작/끝 리스트 마커). */

	OPAL_DTA_TOKENID_INVALID	= 0X0,
	/* [한국어] 파서가 아직 분류하지 못한/유효하지 않은 토큰 표시용 sentinel. */
};

enum opal_atom_width {
	/* [한국어] 가변 atom의 길이 클래스. 헤더 비트 패턴으로 폭이 결정되며
	 * tiny→short→medium→long 순으로 표현 가능 데이터 크기가 커진다.
	 * (TCG spec §3.2.2.1, §3.2.2.2) */

	OPAL_WIDTH_TINY,    /* 1 byte in length */
	/* [한국어] tiny atom: 헤더만으로 6비트 데이터 표현 (signed/unsigned 작은 값). */

	OPAL_WIDTH_SHORT,   /* a 1-byte header and contain up to 15 bytes of data */
	/* [한국어] short atom: 1바이트 헤더 + 0~15바이트 데이터. */

	OPAL_WIDTH_MEDIUM,  /* a 2-byte header and contain up to 2047 bytes of data */
	/* [한국어] medium atom: 2바이트 헤더 + 최대 2047바이트 데이터. */

	OPAL_WIDTH_LONG,    /* a 4-byte header and which contain up to 16,777,215 bytes of data */
	/* [한국어] long atom: 4바이트 헤더 + 최대 16MB 데이터(거의 안 쓰임). */

	OPAL_WIDTH_TOKEN
	/* [한국어] atom이 아닌 control token (start/end list, call, end-of-data 등). */
};

enum opal_uid_enum {
	/* [한국어] 자주 쓰이는 표준 Opal UID에 대한 인덱스(아래 spdk_opal_uid 테이블의 row).
	 * 모든 Opal 명령은 "어느 객체(UID)에" "어느 method(UID)를 호출하라"는 형태이며,
	 * 본 enum으로 코드에서 가독성 있게 UID를 참조한다. (TCG SSC spec) */

	/* users */
	UID_SMUID,        /* [한국어] Session Manager UID — 세션 시작/종료 대상. */
	UID_THISSP,       /* [한국어] 현재 SP(Security Provider) 자체를 가리킬 때. */
	UID_ADMINSP,      /* [한국어] Admin SP — 보안 모드 전환·소유권 변경에 사용. */
	UID_LOCKINGSP,    /* [한국어] Locking SP — 일상 잠금/해제, locking range 관리. */
	UID_ANYBODY,      /* [한국어] 익명 권한자 — 인증 없이 가능한 동작 식별용. */
	UID_SID,          /* [한국어] Security ID(공장 출하 admin 자격). */
	UID_ADMIN1,       /* [한국어] LockingSP의 첫 번째 Admin 권한자. */
	UID_USER1,        /* [한국어] LockingSP의 첫 번째 User 권한자. */
	UID_USER2,        /* [한국어] LockingSP의 두 번째 User 권한자. */

	/* tables */
	UID_LOCKINGRANGE_GLOBAL,        /* [한국어] 전체 LBA 범위에 해당하는 Global Locking Range. */
	UID_LOCKINGRANGE_ACE_RDLOCKED,  /* [한국어] 읽기 잠금 ACE(Access Control Element) 객체. */
	UID_LOCKINGRANGE_ACE_WRLOCKED,  /* [한국어] 쓰기 잠금 ACE 객체. */
	UID_MBRCONTROL,                  /* [한국어] PBA/MBR 모드 제어 테이블. */
	UID_MBR,                         /* [한국어] Pre-Boot Authentication MBR shadow image. */
	UID_AUTHORITY_TABLE,             /* [한국어] Authority(권한자) 정의 테이블. */
	UID_C_PIN_TABLE,                 /* [한국어] Credential PIN 테이블. */
	UID_LOCKING_INFO_TABLE,          /* [한국어] Locking SP 메타정보 테이블. */
	UID_PSID,                        /* [한국어] Physical Secure ID — 디바이스 라벨 PSID 공장 초기화용. */

	/* C_PIN_TABLE object ID's */
	UID_C_PIN_MSID,    /* [한국어] MSID(Manufacturing SID) PIN — 공장 초기 SID 값. */
	UID_C_PIN_SID,     /* [한국어] SID PIN 객체. */
	UID_C_PIN_ADMIN1,  /* [한국어] Admin1 PIN 객체. */
	UID_C_PIN_USER1,   /* [한국어] User1 PIN 객체. */

	/* half UID's (only first 4 bytes used) */
	UID_HALF_AUTHORITY_OBJ_REF, /* [한국어] ACE 정의에서 권한자 참조용 4바이트 prefix. */
	UID_HALF_BOOLEAN_ACE,       /* [한국어] Boolean ACE 빌드 시 사용하는 4바이트 prefix. */
};

/* enum for indexing the spdk_opal_method array */
enum opal_method_enum {
	/* [한국어] Opal SSC method 표준 UID의 인덱스 (아래 spdk_opal_method 테이블의 row). */

	PROPERTIES_METHOD,     /* [한국어] Properties — TPer 속성 조회. */
	STARTSESSION_METHOD,   /* [한국어] StartSession — SP에 세션 개시. */
	REVERT_METHOD,         /* [한국어] Revert — SP를 공장 초기 상태로 되돌림. */
	ACTIVATE_METHOD,       /* [한국어] Activate — Locking SP 활성화. */
	NEXT_METHOD,           /* [한국어] Next — 테이블 행 순회. */
	GETACL_METHOD,         /* [한국어] GetACL — ACL 조회. */
	GENKEY_METHOD,         /* [한국어] GenKey — 암호화 키 재생성(즉시 모든 데이터 무효화). */
	REVERTSP_METHOD,       /* [한국어] RevertSP — 특정 SP만 되돌림. */
	GET_METHOD,            /* [한국어] Get — 객체 속성 읽기. */
	SET_METHOD,            /* [한국어] Set — 객체 속성 쓰기 (PIN 변경 등). */
	AUTHENTICATE_METHOD,   /* [한국어] Authenticate — 권한자 인증. */
	RANDOM_METHOD,         /* [한국어] Random — TRNG에서 난수 획득. */
	ERASE_METHOD,          /* [한국어] Erase — locking range crypto erase. */
};

struct spdk_opal_key {
	/* [한국어] 사용자 PIN/패스워드 컨테이너.
	 * 키 자체는 평문이며, 호출자는 사용 직후 secure-zero로 지워야 한다. */

	uint8_t key_len;
	/* [한국어] 실제 키 길이(바이트). 0 ~ OPAL_KEY_MAX.
	 * 설정자: 사용자 입력 처리 코드(공개 API). 읽는 자: 토큰 인코더. */

	uint8_t key[OPAL_KEY_MAX];
	/* [한국어] 키 본체 바이트열. 길이는 key_len이 결정.
	 * 보안 주의: 메모리 덤프/스왑 노출 가능성 — 사용자는 mlock 권장.
	 * 읽는 자: TCG 토큰 인코딩 시 BYTESTRING atom으로 패킹. */
};

/* [한국어] 표준 Opal UID 8바이트 바이트열 테이블.
 * enum opal_uid_enum의 인덱스로 접근하며, 이 정의는 헤더에 const로 들어있어 본 헤더를
 * include한 모든 .o가 자기 사본을 갖는다(헤더 단위 const 배열). 각 UID는 TCG Core
 * Spec / SSC에서 객체별로 정해진 표준 식별자이며, 호스트는 ComPacket 안의 method-call에
 * 이 8바이트를 그대로 박아 SSD에 전달한다. */
const uint8_t spdk_opal_uid[][OPAL_UID_LENGTH] = {
	/* users */
	[UID_SMUID] = /* Session Manager UID */
	/* [한국어] StartSession/EndSession을 발행할 때 "이 객체에 호출"이라는 의미로 이 UID 사용. */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff },
	[UID_THISSP] =
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 },
	[UID_ADMINSP] =
	{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 0x01 },
	[UID_LOCKINGSP] =
	{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 0x02 },
	[UID_ANYBODY] =
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01 },
	[UID_SID] = /* Security Identifier UID */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x06 },
	[UID_ADMIN1] =
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x01, 0x00, 0x01 },
	[UID_USER1] =
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x03, 0x00, 0x01 },
	[UID_USER2] =
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x03, 0x00, 0x02 },

	/* tables */
	[UID_LOCKINGRANGE_GLOBAL] =
	{ 0x00, 0x00, 0x08, 0x02, 0x00, 0x00, 0x00, 0x01 },
	[UID_LOCKINGRANGE_ACE_RDLOCKED] =
	{ 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xE0, 0x01 },
	[UID_LOCKINGRANGE_ACE_WRLOCKED] =
	{ 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xE8, 0x01 },
	[UID_MBRCONTROL] =
	{ 0x00, 0x00, 0x08, 0x03, 0x00, 0x00, 0x00, 0x01 },
	[UID_MBR] =
	{ 0x00, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00 },
	[UID_AUTHORITY_TABLE] =
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00},
	[UID_C_PIN_TABLE] =
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x00},
	[UID_LOCKING_INFO_TABLE] =
	{ 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x01 },
	[UID_PSID] =
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x01, 0xff, 0x01 },

	/* C_PIN_TABLE object ID's */
	[UID_C_PIN_MSID] =
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x84, 0x02},
	[UID_C_PIN_SID] =
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x01},
	[UID_C_PIN_ADMIN1] =
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x01, 0x00, 0x01},
	[UID_C_PIN_USER1] =
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x03, 0x00, 0x01},

	/* half UID's (only first 4 bytes used) */
	[UID_HALF_AUTHORITY_OBJ_REF] =
	{ 0x00, 0x00, 0x0C, 0x05, 0xff, 0xff, 0xff, 0xff },
	[UID_HALF_BOOLEAN_ACE] =
	{ 0x00, 0x00, 0x04, 0x0E, 0xff, 0xff, 0xff, 0xff },
};

/*
 * TCG Storage SSC Methods.
 */
/* [한국어] 표준 Opal Method UID 8바이트 바이트열 테이블.
 * 호스트가 method-call 토큰을 만들 때 (객체 UID, method UID, args...) 형태로
 * 직렬화하므로, method UID 또한 8바이트 표준값이 필요하다. enum opal_method_enum로 인덱싱. */
const uint8_t spdk_opal_method[][OPAL_UID_LENGTH] = {
	[PROPERTIES_METHOD] =
	/* [한국어] Properties method — TPer 일반 속성을 조회. 보통 첫 번째로 호출. */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x01 },
	[STARTSESSION_METHOD] =
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x02 },
	[REVERT_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x02, 0x02 },
	[ACTIVATE_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x02, 0x03 },
	[NEXT_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x08 },
	[GETACL_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0d },
	[GENKEY_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x10 },
	[REVERTSP_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x11 },
	[GET_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x16 },
	[SET_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x17 },
	[AUTHENTICATE_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1c },
	[RANDOM_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x06, 0x01 },
	[ERASE_METHOD] =
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x08, 0x03 },
};

/*
 * Response token
 */
struct spdk_opal_resp_token {
	/* [한국어] 응답 페이로드를 토큰 단위로 파싱한 결과.
	 * 파서는 응답 버퍼를 한 번 훑어 토큰 경계와 타입/폭/값을 식별해 이 구조체에 채운다. */

	const uint8_t *pos;
	/* [한국어] 응답 버퍼(opal_session.resp) 안에서 이 토큰의 시작 주소.
	 * BYTESTRING의 경우 이 포인터로부터 len 바이트가 데이터 본체.
	 * 읽는 자: 키/UID 추출 헬퍼. 라이프타임: 응답 버퍼와 동일. */

	uint8_t _padding[7];
	/* [한국어] 다음 union 필드를 8바이트 정렬에 맞추기 위한 패딩 (포인터 4/8바이트 환경 모두 안전). */

	union {
		uint64_t unsigned_num;
		/* [한국어] 토큰이 UINT일 때 디코딩된 64비트 부호 없는 값. */

		int64_t signed_num;
		/* [한국어] 토큰이 SINT일 때 디코딩된 부호 있는 값. */
	} stored;
	/* [한국어] 정수 atom의 즉시 사용 가능한 디코딩 결과를 저장.
	 * 비-정수 토큰(BYTESTRING/TOKEN)에서는 미사용. */

	size_t len; /* header + data */
	/* [한국어] 토큰의 전체 길이(헤더+데이터 바이트). pos+len이 다음 토큰 시작. */

	enum opal_token_type type;
	/* [한국어] 토큰 타입 (BYTESTRING/UINT/SINT/TOKEN/INVALID). */

	enum opal_atom_width width;
	/* [한국어] atom 폭 클래스 (tiny/short/medium/long/token). */
};

struct spdk_opal_resp_parsed {
	/* [한국어] 한 번의 응답에서 파싱된 모든 토큰을 담는 결과 컨테이너. */

	int num;
	/* [한국어] 실제로 파싱된 토큰 수 (0 ~ MAX_TOKS).
	 * 읽는 자: 응답 처리 코드가 resp_tokens[i]를 i<num 범위에서 순회. */

	struct spdk_opal_resp_token resp_tokens[MAX_TOKS];
	/* [한국어] 파싱된 토큰 배열. 정적 크기로 잡혀 있어 응답 한 개당 메모리 할당 불필요. */
};

/* header of a response */
struct spdk_opal_header {
	/* [한국어] Opal 응답의 고정 헤더 부분.
	 * TCG ComPacket → Packet → SubPacket의 3단 헤더가 차례로 등장한다 (TCG Core Spec §3.2.3). */

	struct spdk_opal_compacket com_packet;
	/* [한국어] ComPacket 헤더 — Extended ComID, length, sequence 등 포함. */

	struct spdk_opal_packet packet;
	/* [한국어] Packet 헤더 — TSN/HSN, ack, kind 등. */

	struct spdk_opal_data_subpacket sub_packet;
	/* [한국어] Data SubPacket 헤더 — payload length 포함. SubPacket 뒤에 토큰 페이로드가 따른다. */
};

struct opal_session;
struct spdk_opal_dev;
/* [한국어] 상호 참조를 위한 전방 선언 (opal_sess_cb, opal_session, spdk_opal_dev가
 * 서로를 참조하기 때문). */

typedef void (*opal_sess_cb)(struct opal_session *sess, int status, void *ctx);
/* [한국어] Opal 세션 명령 완료 콜백 시그니처.
 * @sess  : 완료된 세션 ctx.
 * @status: 0 성공 또는 음수/Opal 상태 코드.
 * @ctx   : 호출자가 등록한 user ctx.
 * 호출 컨텍스트: NVMe driver thread (security_send/receive 완료 시점). */

struct opal_session {
	/* [한국어] Opal 명령 한 사이클(send + receive)을 추적하는 세션 컨테이너.
	 * 한 번에 하나의 outstanding 명령만 가질 수 있도록 단일 cmd/resp 버퍼를 갖는다. */

	uint32_t hsn;
	/* [한국어] Host Session Number — 호스트가 부여(GENERIC_HOST_SESSION_NUM).
	 * StartSession에서 설정되며 세션 종료 전까지 유지. */

	uint32_t tsn;
	/* [한국어] TPer(드라이브)이 할당한 Target Session Number. StartSession 응답에서 추출. */

	size_t cmd_pos;
	/* [한국어] cmd 버퍼에서 다음 토큰을 쓸 오프셋. encode 헬퍼가 증가시킴. */

	uint8_t cmd[IO_BUFFER_LENGTH];
	/* [한국어] 송신 명령 버퍼. ComPacket 헤더 + 토큰 페이로드를 직렬화해 담는다.
	 * NVMe Security Send 시 그대로 SSD에 전달. */

	uint8_t resp[IO_BUFFER_LENGTH];
	/* [한국어] Security Receive로 받은 응답 페이로드. parsed_resp가 이를 파싱한 뷰. */

	struct spdk_opal_resp_parsed parsed_resp;
	/* [한국어] resp[]를 토큰 단위로 파싱한 결과(in-place 포인터 사용). */

	opal_sess_cb sess_cb;
	/* [한국어] 명령 완료 시 호출할 사용자 콜백. */

	void *cb_arg;
	/* [한국어] sess_cb에 전달할 ctx. */

	bool done;
	/* [한국어] 명령 사이클 종료 표식 (sync 대기에서 사용). 비동기 콜백 모델에서는 보조용. */

	int status;
	/* [한국어] 가장 최근 명령의 결과 코드 (0 또는 음수 errno / Opal 상태). */

	struct spdk_opal_dev *dev;
	/* [한국어] 이 세션이 속한 Opal 디바이스 핸들 — back-pointer. */
};

struct spdk_opal_dev {
	/* [한국어] 한 NVMe 컨트롤러에 대한 Opal 보안 핸들.
	 * spdk_nvme_opal_init() 시 생성되어 컨트롤러 ctx에 저장된다. */

	struct spdk_nvme_ctrlr *ctrlr;
	/* [한국어] 백킹 NVMe 컨트롤러 핸들 — Security Send/Receive 발행 대상. */

	uint16_t comid;
	/* [한국어] Extended ComID (TCG Discovery 0 응답으로 협상된 채널 ID).
	 * ComPacket 헤더에 매번 채워진다. */

	struct spdk_opal_d0_features_info feat_info;
	/* [한국어] Discovery 0에서 파싱한 SED 기능 비트(잠금 지원, encrypt 알고리즘 등). */

	uint8_t max_ranges; /* max locking range number */
	/* [한국어] 디바이스가 지원하는 locking range 최대 개수 (Discovery 0에서 추출).
	 * locking_ranges 배열에서 [0..max_ranges) 범위만 의미 있다. */

	struct spdk_opal_locking_range_info locking_ranges[SPDK_OPAL_MAX_LOCKING_RANGE];
	/* [한국어] locking range별 메타정보 캐시 (LBA range start/length, RLE/WLE 잠금 비트 등).
	 * 갱신: spdk_opal_get_locking_range_info() 호출 시. */
};

#endif
/* [한국어] 다중 포함 가드 종결. */
