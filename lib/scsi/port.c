/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SCSI Port 객체 생성/소멸 및 TransportID 인코딩 (port.c)
 *
 * === 파일의 역할 ===
 * SCSI 표준에서 "port"는 SCSI 명령이 들어오는 종단점(initiator/target side endpoint)을 가리킨다.
 * SPDK SCSI에서는 이를 spdk_scsi_port 구조체로 추상화한다 — id/index/name 등 정체성 정보와
 * 옵션으로 SPC-3에서 정의한 TransportID(이니시에이터의 프로토콜별 식별자)를 보관한다.
 * 본 파일은 port 객체의 생성(spdk_scsi_port_create), 해제(spdk_scsi_port_free),
 * 내부 구축자(scsi_port_construct/destruct), getter(spdk_scsi_port_get_name),
 * iSCSI 이니시에이터 포트의 TransportID 인코딩(spdk_scsi_port_set_iscsi_transport_id)을 제공한다.
 * 본 파일은 SCSI 명령을 처리하지 않으며, 단순한 식별자 컨테이너 운영만 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SCSI 명령이 도착하면 task->initiator_port / task->target_port에 spdk_scsi_port * 가 부여되어
 * I_T nexus(이니시에이터-타겟 쌍) 식별의 기준이 된다(scsi_pr.c가 이를 사용해 PR 등록자를 매칭).
 * spdk_scsi_dev는 내장 port 배열(SPDK_SCSI_DEV_MAX_PORTS)을 가지며, dev.c의
 * spdk_scsi_dev_add_port()/find_port_by_id()가 본 파일의 scsi_port_construct/destruct를 호출한다.
 * 외부에서 동적으로 port를 만들려면 spdk_scsi_port_create()로 calloc된 객체를 사용하기도 한다.
 * 실행 컨텍스트: 보통 LUN을 소유한 SPDK 스레드(LUN 생성자가 결정) 또는 RPC 처리 스레드.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: scsi_internal.h (spdk_scsi_port 구조체), spdk/endian.h (big-endian 변환 to_be16),
 *         spdk_scsi_iscsi_transport_id (spdk/scsi_spec.h 정의), libc(calloc/free/snprintf/memset/strlen).
 * - 데이터 흐름: 상위(iSCSI/vhost-scsi target) → spdk_scsi_port_set_iscsi_transport_id로 TransportID 채움
 *   → SPC PR/Inquiry 응답 시 해당 바이트가 그대로 SCSI Wire data로 나감.
 * - 공유 자료구조: spdk_scsi_port 자체는 dev 내장 배열 또는 동적 객체이며, 한 LUN/한 dev 컨텍스트에서만 다뤄짐.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_scsi_port_create()       : calloc + scsi_port_construct, 외부 노출 동적 생성자.
 * - spdk_scsi_port_free()         : free + 포인터 NULL화 (이중 해제 방지 패턴).
 * - scsi_port_construct()         : 이미 메모리가 있는 port 구조체에 id/index/name 채움.
 * - scsi_port_destruct()          : 구조체 전체 zero-fill로 "사용 안함" 표시.
 * - spdk_scsi_port_get_name()     : 단순 getter.
 * - spdk_scsi_port_set_iscsi_transport_id(): SPC-3 iSCSI initiator TransportID(코드 0x01)를 채워넣음.
 */

#include "scsi_internal.h"
/* [한국어] spdk_scsi_port 구조체와 매크로(SPDK_SCSI_MAX_TRANSPORT_ID_LENGTH 등) 사용. */

#include "spdk/endian.h"
/* [한국어] to_be16() 등 빅엔디안 변환 함수 — SCSI 와이어 포맷이 빅엔디안이므로 필요. */

/*
 * [한국어]
 * spdk_scsi_port_create - 새 SCSI port 객체를 동적으로 할당하고 초기화
 *
 * @id:    port의 64비트 고유 식별자(보통 dev 내 인덱스나 시스템 전역 id).
 * @index: port 인덱스(상대적 target port id로 사용 — PR 등록자 식별 시 활용).
 * @name:  사람이 읽는 이름 문자열(SPDK_SCSI_PORT_MAX_NAME_LENGTH 미만이어야 함).
 * @return: 새 port 포인터(성공) 또는 NULL(메모리 부족 / 이름 길이 초과).
 *
 * dev 내장 배열을 쓰지 않고 별도로 SCSI port를 들고 다녀야 하는 상위 모듈을 위한 헬퍼.
 * calloc + 본 파일의 내부 scsi_port_construct를 묶은 편의 API다.
 * construct 실패 시 즉시 free하여 누수 방지. 실행 컨텍스트는 호출자 스레드.
 *
 * 호출 체인:
 *   상위(iSCSI/vhost) → [spdk_scsi_port_create] → calloc → scsi_port_construct
 */
struct spdk_scsi_port *
spdk_scsi_port_create(uint64_t id, uint16_t index, const char *name)
{
	struct spdk_scsi_port *port;
	/* [한국어] 새로 할당될 port의 포인터. */

	port = calloc(1, sizeof(struct spdk_scsi_port));
	/* [한국어] zero-init 할당 — is_used=0, transport_id=0 등 모든 필드가 0으로 시작. */

	if (!port) {
		/* [한국어] 메모리 부족 — 호출자에게 NULL 반환. */
		return NULL;
	}

	if (scsi_port_construct(port, id, index, name) != 0) {
		/* [한국어] 이름 길이 초과 등으로 구축 실패 → 누수 방지하려고 즉시 해제. */
		spdk_scsi_port_free(&port);
		return NULL;
	}

	return port;
	/* [한국어] 성공: 호출자가 spdk_scsi_port_free로 해제할 책임을 가진다. */
}

/*
 * [한국어]
 * spdk_scsi_port_free - 동적 할당된 SCSI port를 해제하고 포인터를 NULL로 만든다
 *
 * @pport: 해제할 port 포인터의 주소. NULL이면 무시. *pport == NULL이어도 안전(free(NULL)).
 *
 * 이중 해제 사고를 막기 위해 더블 포인터를 받아 *pport=NULL을 함께 수행한다(전형적 SPDK 패턴).
 * 실행 컨텍스트: 호출자 스레드. 동시성: 호출자가 보장.
 *
 * 호출 체인:
 *   상위(iSCSI/vhost), spdk_scsi_port_create의 에러 경로 → [spdk_scsi_port_free] → free
 */
void
spdk_scsi_port_free(struct spdk_scsi_port **pport)
{
	struct spdk_scsi_port *port;
	/* [한국어] 실제 해제할 객체 포인터를 잠시 보관. */

	if (!pport) {
		/* [한국어] 인자 자체가 NULL이면 호출자 버그 — 안전하게 종료. */
		return;
	}

	port = *pport;
	/* [한국어] 더블 포인터에서 객체 포인터를 추출. */
	*pport = NULL;
	/* [한국어] 호출자 측 포인터를 먼저 NULL로 만들어 dangling 사고 방지. */
	free(port);
	/* [한국어] free(NULL)은 no-op이므로 *pport가 원래 NULL이어도 안전. */
}

/*
 * [한국어]
 * scsi_port_construct - 이미 메모리가 있는 port 구조체에 id/index/name을 세팅
 *
 * @port:  대상 port (dev 내장 배열의 슬롯이거나 calloc된 객체).
 * @id:    64비트 식별자.
 * @index: 인덱스(relative target port id).
 * @name:  이름 문자열 — port->name 버퍼(SPDK_SCSI_PORT_MAX_NAME_LENGTH)에 들어갈 길이여야 함.
 * @return: 0 성공, -1 이름 길이 초과.
 *
 * dev.c의 spdk_scsi_dev_add_port가 dev 내장 port 슬롯에 대해 직접 호출하기도 하고,
 * spdk_scsi_port_create가 calloc된 객체에 대해 호출하기도 한다.
 * is_used=1 마킹으로 "사용 중" 슬롯임을 표시한다.
 *
 * 호출 체인:
 *   spdk_scsi_dev_add_port / spdk_scsi_port_create → [scsi_port_construct] → snprintf
 */
int
scsi_port_construct(struct spdk_scsi_port *port, uint64_t id, uint16_t index,
		    const char *name)
{
	if (strlen(name) >= sizeof(port->name)) {
		/* [한국어] '\0' 포함 자리 확보 위해 < name 버퍼 크기여야 함 — 경계값에서 잘림 방지. */
		SPDK_ERRLOG("port name too long\n");
		return -1;
	}

	port->is_used = 1;
	/* [한국어] dev 내장 배열에서 빈/사용 슬롯 구분하는 플래그 — scsi_dev_find_free_port가 0 슬롯을 찾는다. */
	port->id = id;
	/* [한국어] 호출자가 부여한 64비트 ID 저장 — find_port_by_id 매칭 키. */
	port->index = index;
	/* [한국어] PR registrant가 relative_target_port_id로 활용. */
	snprintf(port->name, sizeof(port->name), "%s",
		 name);
	/* [한국어] 안전 복사(자동 truncate + null terminate). 길이 검사는 위에서 이미 통과. */
	return 0;
}

/*
 * [한국어]
 * scsi_port_destruct - port를 "미사용" 상태로 되돌림
 *
 * @port: 대상 port. memset으로 모든 필드를 0으로 만들어 is_used=0 상태가 된다.
 *
 * 동적 객체일 경우 free는 별도(spdk_scsi_port_free)로 호출해야 한다.
 * dev 내장 슬롯의 경우 이 함수만으로 해당 슬롯을 다시 빈 슬롯으로 만들 수 있다.
 *
 * 호출 체인:
 *   spdk_scsi_dev_delete_port → [scsi_port_destruct]
 */
void
scsi_port_destruct(struct spdk_scsi_port *port)
{
	memset(port, 0, sizeof(struct spdk_scsi_port));
	/* [한국어] 이름/transport_id 등 잠재적으로 민감한 데이터까지 0으로 지운다. */
}

/*
 * [한국어]
 * spdk_scsi_port_get_name - port 이름 문자열에 대한 읽기 전용 포인터 반환
 *
 * @port: 대상 port.
 * @return: port->name (널 종료 문자열). port가 살아있는 동안만 유효.
 *
 * 디버깅/로그/RPC 응답에서 포트 이름을 노출할 때 사용된다.
 *
 * 호출 체인:
 *   상위(iSCSI/vhost-scsi 로그) → [spdk_scsi_port_get_name]
 */
const char *
spdk_scsi_port_get_name(const struct spdk_scsi_port *port)
{
	return port->name;
	/* [한국어] 내부 버퍼 그대로 반환 — 호출자는 free 하지 않는다. */
}

/*
 * spc3r23 7.5.4.6 iSCSI initiator port TransportID,
 * using code format 0x01.
 */
/*
 * [한국어]
 * spdk_scsi_port_set_iscsi_transport_id - iSCSI 이니시에이터 포트의 TransportID를 채워넣음
 *
 * @port:       대상 port (보통 task->initiator_port).
 * @iscsi_name: iSCSI Qualified Name (IQN), 예: "iqn.2016-06.io.spdk:host1".
 * @isid:       iSCSI Session ID (RFC 3720) 48비트 값.
 *
 * SPC-3 spec 7.5.4.6에 정의된 "iSCSI initiator TransportID, format code 0x01" 포맷의
 * 바이트 시퀀스를 port->transport_id에 인코딩한다. 이 데이터는 SCSI PR(Persistent Reservation)
 * "READ FULL STATUS" 같은 응답이나 PR 등록자 비교 시에 사용된다.
 * 포맷: protocol_id(상위 헤더) + 가변 길이 ASCII 이름 "iqn,i,0xISID" + 4-byte 정렬 패딩 + additional_len(BE16).
 *
 * 호출 체인:
 *   iSCSI target 로그인 처리 → [spdk_scsi_port_set_iscsi_transport_id] → snprintf, to_be16
 */
void
spdk_scsi_port_set_iscsi_transport_id(struct spdk_scsi_port *port, char *iscsi_name,
				      uint64_t isid)
{
	struct spdk_scsi_iscsi_transport_id *data;
	/* [한국어] port->transport_id 버퍼를 SPC-3 헤더 구조체로 캐스트해서 다룰 포인터. */
	uint32_t len;
	/* [한국어] 동적으로 채워진 가변 길이 이름 부분의 길이 누계. */
	char *name;
	/* [한국어] data 헤더 뒤의 가변 이름 영역 시작점. */

	memset(port->transport_id, 0, sizeof(port->transport_id));
	/* [한국어] 이전 transport_id 잔여물 제거 — 길이 미달 시에도 보장. */
	port->transport_id_len = 0;
	/* [한국어] 길이도 0으로 리셋 — 실패 분기에서 미초기화 상태 노출 방지. */

	data = (struct spdk_scsi_iscsi_transport_id *)port->transport_id;
	/* [한국어] in-place로 헤더 필드 채우기 위해 캐스트(별도 할당 불필요). */

	data->protocol_id = (uint8_t)SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI;
	/* [한국어] SPC-3 Table 244 — 0x05가 iSCSI 프로토콜 식별자. */
	data->format = 0x1;
	/* [한국어] format code 0x01 = iSCSI initiator port + ISID 형식. */

	name = data->name;
	/* [한국어] 헤더 직후의 ASCII 영역으로 진입. */
	len = snprintf(name, SPDK_SCSI_MAX_TRANSPORT_ID_LENGTH - sizeof(*data),
		       "%s,i,0x%12.12" PRIx64, iscsi_name, isid);
	/* [한국어] SPC-3 7.5.4.6: "<IQN>,i,0x<12자리 hex ISID>" 포맷. 12.12는 폭/정밀도 둘 다 12. */
	do {
		name[len++] = '\0';
		/* [한국어] 패딩 바이트(0)를 4바이트 경계에 맞을 때까지 추가 — 스펙 요구사항. */
	} while (len & 3);

	if (len < 20) {
		/* [한국어] SPC-3은 TransportID 가변부 최소 20바이트를 요구 — 미달 시 거부. */
		SPDK_ERRLOG("The length of Transport ID should >= 20 bytes\n");
		return;
	}

	to_be16(&data->additional_len, len);
	/* [한국어] 가변 부분 길이를 빅엔디안 16비트로 헤더의 additional_len에 기록 (SCSI 와이어 포맷). */
	port->transport_id_len = len + sizeof(*data);
	/* [한국어] 헤더+가변부 합산 — PR 응답에서 그대로 직렬화될 전체 길이. */
}
