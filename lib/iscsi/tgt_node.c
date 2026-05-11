/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI Target Node (LU 매핑 + ACL + redirect) 구현 (tgt_node.c)
 *
 * === 파일의 역할 ===
 * iSCSI 타깃 측에서 외부에 노출되는 "타깃 노드"(IQN으로 식별되는 SCSI 디바이스)와 그
 * 라이프사이클 전반을 담당한다. 구체적으로 (1) tgt_node 생성/소멸/등록/검색,
 * (2) (PG, IG) 매핑 (pg_map, ig_map) 관리 — 어떤 포털 경로로 접근하는 어떤 이니시에이터에게
 * 어떤 LU를 노출할지, (3) iSCSI Discovery(SendTargets) 응답 텍스트 직렬화, (4) iSCSI 이름
 * 형식(RFC 3720 §3) 검증, (5) CHAP 정책 변경, (6) 로그인 시 ACL 검사 (iqn + 넷마스크),
 * (7) 로그인 redirect (RFC 3720 §5.3.3 TargetAddress), (8) JSON-RPC 직렬화, (9) bdev 기반
 * SCSI device(LU) 부착·정리, (10) histogram. 본 파일이 만든 spdk_iscsi_tgt_node가 iSCSI
 * 세션의 최종 "디바이스 모델"이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (생성):
 *   RPC iscsi_create_target_node → iscsi_tgt_node_construct() → check_iscsi_name →
 *     spdk_scsi_dev_construct(bdev_name_list[]) → iscsi_target_node_add_pg_ig_maps →
 *     iscsi_tgt_node_register (g_iscsi.target_head 등록).
 * 호출 체인 (로그인 ACL):
 *   iscsi_op_login → iscsi_tgt_node_access(conn, target, iqn, addr) →
 *     iscsi_tgt_node_find_pg_map → iscsi_init_grp_allow_iscsi_name + iscsi_init_grp_allow_addr.
 * 호출 체인 (Discovery):
 *   SendTargets text 협상 → iscsi_send_tgts → iscsi_tgt_node_allow_iscsi_name (ACL 필터) →
 *     "TargetName=...\0TargetAddress=ip:port,tag\0" 직렬화 (continue 분할 지원).
 * 호출 체인 (소멸):
 *   RPC iscsi_delete_target_node → iscsi_shutdown_tgt_node_by_name →
 *     iscsi_tgt_node_destruct → iscsi_conns_request_logout → 활성 conn 0 도달까지 poller →
 *     spdk_scsi_dev_destruct → _iscsi_tgt_node_destruct (PG/IG map 해제, mutex_destroy, free).
 * 실행 컨텍스트: 변경 경로는 RPC thread + g_iscsi.mutex. 로그인 검사/access 경로는 conn의
 * SPDK thread (read-only 순회는 락 없이도 안전 — 변경이 RPC mutex로 직렬화되어 있고 순회
 * 시점에 타깃이 destruct되지 않도록 외부에서 ref가 보장).
 *
 * === 타 모듈과의 연결 ===
 * - iscsi.h: g_iscsi.target_head, MAX_TARGET_NAME, MAX_PORTAL_*, SPDK_ISCSI_*.
 * - portal_grp.h/init_grp.h: pg_map의 pg/ig 백포인터; ref count로 PG/IG 삭제 방지.
 * - conn.h: spdk_iscsi_conn(portal, sock, target_addr, send_tgt_completed_size).
 * - scsi.h (SPDK): spdk_scsi_dev_construct/destruct/add_lun/add_port — bdev → SCSI device.
 * - sock.h: spdk_sock_is_ipv4/6 — 와일드카드 포털 응답 시 conn의 target_addr 선택.
 * - histogram_data.h: spdk_histogram_data_alloc/free — I/O 지연 분포 측정 (옵션).
 * 데이터 흐름: RPC 입력 → tgt_node + scsi_dev 객체 → ACL 검사 통과 conn → 세션 attach →
 *   I/O 시 SCSI 명령이 spdk_scsi_lun으로 전달 → bdev 모듈로 위임.
 *
 * === 주요 함수/구조체 요약 ===
 * - iscsi_ipv4/6_netmask_allow_addr: 넷마스크 prefix 비트 비교로 IP 매칭.
 * - iscsi_init_grp_allow_iscsi_name: IQN 매칭 — "ANY"/정확/접두 "!"(deny).
 * - iscsi_tgt_node_access: 로그인 시 ACL 최종 판정.
 * - iscsi_send_tgts/iscsi_send_tgt_portals: SendTargets 응답 직렬화 (continue 분할 가능).
 * - iscsi_tgt_node_construct: 신규 타깃 생성 진입점.
 * - iscsi_tgt_node_add/delete_pg_map/ig_map: (PG, IG) 매핑 트리 조작.
 * - iscsi_tgt_node_destruct: 비동기 소멸 — 활성 conn 정리 후 SCSI device destruct.
 * - iscsi_tgt_node_redirect/_is_redirected: 로그인 임시 redirect 설정/조회.
 * - iscsi_check_chap_params: CHAP 4-튜플 합법 조합 판정.
 * - check_iscsi_name: iqn./eui./naa. 접두와 글자 집합 검증.
 * - iscsi_tgt_node_info_json/config_json: JSON-RPC 직렬화.
 * - iscsi_tgt_node_enable_histogram: 지연 분포 측정 토글.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 라이브러리 포함. inet_pton, isspace 등. */

#include "spdk/sock.h"
/* [한국어] spdk_sock_is_ipv4/6 — Discovery 응답 시 와일드카드 포털 결정. */
#include "spdk/scsi.h"
/* [한국어] spdk_scsi_dev_construct/destruct/add_lun/add_port/get_lun — SCSI 디바이스 모델. */

#include "spdk/log.h"
/* [한국어] SPDK_*LOG. */

#include "iscsi/iscsi.h"
/* [한국어] g_iscsi 전역, 도메인 상수. */
#include "iscsi/conn.h"
/* [한국어] struct spdk_iscsi_conn — ACL 검사에 portal/sock/target_addr 사용. */
#include "iscsi/tgt_node.h"
/* [한국어] 본 파일의 외부 API 선언과 자료구조. */
#include "iscsi/portal_grp.h"
/* [한국어] iscsi_portal_grp_find_by_tag, iscsi_parse_redirect_addr. */
#include "iscsi/init_grp.h"
/* [한국어] iscsi_init_grp_find_by_tag — ACL 매핑 등록 시 사용. */
#include "iscsi/task.h"
/* [한국어] iscsi_task_get, iscsi_op_abort_task_set — 셧다운 시 LUN cleanup. */
#include "spdk/histogram_data.h"
/* [한국어] spdk_histogram_data_* — I/O 지연 분포 측정 (옵션). */

#define MAX_TMPBUF 4096
/* [한국어] SendTargets 응답 임시 buffer 크기. */
#define MAX_MASKBUF 128
/* [한국어] 넷마스크 문자열 buffer (IPv6 표현 최대 길이 마진 포함). */


#define MAX_TMP_NAME_BUF (11 /* TargetName= */ + MAX_TARGET_NAME + 1 /* null */)
/* [한국어] "TargetName=<IQN>\0" 한 줄 buffer 크기. */
#define MAX_TMP_ADDR_BUF (14 /* TargetAddress= */ + MAX_PORTAL_ADDR + 1 /* : */ + \
			  MAX_PORTAL_PORT + 1 /* , */ + 10 /* max length of int in Decimal */ + 1 /* null */)
/* [한국어] "TargetAddress=<host>:<port>,<tag>\0" 한 줄 buffer 크기. */

/*
 * [한국어]
 * iscsi_ipv6_netmask_allow_addr - IPv6 넷마스크가 주어진 주소를 포함하는지 검사.
 *
 * @netmask: "[<addr>]/<bits>" 형식 (예: "[fe80::]/10").
 * @addr: 비교 대상 IPv6 문자열.
 * @return: true 매치, false 불일치/형식 오류.
 *
 * 동작: '['..']' 구간에서 mask 추출 → 이후 "/bits"가 있으면 prefix 길이, 없으면 128 →
 *   inet_pton으로 binary 변환 → bits/8 바이트 정확 비교 + 잔여 bits%8을 mask로 비교.
 */
static bool
iscsi_ipv6_netmask_allow_addr(const char *netmask, const char *addr)
{
	struct in6_addr in6_mask;
	struct in6_addr in6_addr;
	/* [한국어] 16바이트 binary IPv6 주소 buffer. */
	char mask[MAX_MASKBUF];
	/* [한국어] mask의 IP 부분 임시 buffer. */
	const char *p;
	/* [한국어] ']' 위치 추적 포인터. */
	size_t n;
	/* [한국어] mask 문자열 길이. */
	int bits, bmask;
	/* [한국어] prefix 비트 수, 잔여 비트 마스크. */
	int i;
	/* [한국어] 바이트 인덱스. */

	if (netmask[0] != '[') {
		/* [한국어] IPv6 표기는 반드시 '['로 시작 — 형식 검증. */
		return false;
	}
	p = strchr(netmask, ']');
	if (p == NULL) {
		/* [한국어] ']' 없음 — 잘못된 형식. */
		return false;
	}
	n = p - (netmask + 1);
	/* [한국어] '[' 이후, ']' 이전 문자수. */
	if (n + 1 > sizeof mask) {
		/* [한국어] buffer 초과 안전. */
		return false;
	}

	memcpy(mask, netmask + 1, n);
	/* [한국어] '[' 이후만 복사. */
	mask[n] = '\0';
	/* [한국어] NUL 종료. */
	p++;
	/* [한국어] ']' 다음 위치로 — 여기서부터 "/bits"이거나 끝. */

	if (p[0] == '/') {
		/* [한국어] prefix 길이 명시. */
		bits = (int) strtol(p + 1, NULL, 10);
		if (bits <= 0 || bits > 128) {
			/* [한국어] IPv6 prefix 유효 범위. */
			return false;
		}
	} else {
		/* [한국어] 명시 없으면 단일 호스트 매치 (128). */
		bits = 128;
	}

#if 0
	SPDK_DEBUGLOG(iscsi, "input %s\n", addr);
	SPDK_DEBUGLOG(iscsi, "mask  %s / %d\n", mask, bits);
#endif

	/* presentation to network order binary */
	if (inet_pton(AF_INET6, mask, &in6_mask) <= 0
	    || inet_pton(AF_INET6, addr, &in6_addr) <= 0) {
		/* [한국어] 둘 중 하나라도 binary 변환 실패면 false. */
		return false;
	}

	/* check 128bits */
	for (i = 0; i < (bits / 8); i++) {
		/* [한국어] 완전 일치해야 하는 바이트 단위 검사. */
		if (in6_mask.s6_addr[i] != in6_addr.s6_addr[i]) {
			return false;
		}
	}
	if (bits % 8) {
		/* [한국어] 잔여 비트 — 상위 비트만 일치하도록 마스크 비교. */
		bmask = (0xffU << (8 - (bits % 8))) & 0xffU;
		if ((in6_mask.s6_addr[i] & bmask) != (in6_addr.s6_addr[i] & bmask)) {
			return false;
		}
	}

	/* match */
	return true;
}

/*
 * [한국어]
 * iscsi_ipv4_netmask_allow_addr - IPv4 넷마스크가 주어진 주소를 포함하는지 검사.
 *
 * @netmask: "<addr>[/<bits>]" 형식 (예: "10.0.0.0/8").
 * @addr: 비교 대상 IPv4 문자열.
 * @return: true 매치, false 불일치/형식 오류.
 *
 * 32비트 정수로 변환 후 prefix 마스크 적용 비교 (network byte order → host order로 변환).
 */
static bool
iscsi_ipv4_netmask_allow_addr(const char *netmask, const char *addr)
{
	struct in_addr in4_mask;
	struct in_addr in4_addr;
	/* [한국어] 4바이트 binary IPv4 주소. */
	char mask[MAX_MASKBUF];
	/* [한국어] mask 문자열 buffer. */
	const char *p;
	/* [한국어] '/' 위치. */
	uint32_t bmask;
	/* [한국어] 비트 마스크. */
	size_t n;
	/* [한국어] mask 문자열 길이. */
	int bits;
	/* [한국어] prefix 길이. */

	p = strchr(netmask, '/');
	if (p == NULL) {
		/* [한국어] '/' 없음 — 단일 호스트. */
		p = netmask + strlen(netmask);
	}
	n = p - netmask;
	if (n + 1 > sizeof mask) {
		return false;
	}

	memcpy(mask, netmask, n);
	mask[n] = '\0';

	if (p[0] == '/') {
		bits = (int) strtol(p + 1, NULL, 10);
		if (bits <= 0 || bits > 32) {
			/* [한국어] IPv4 prefix 유효 범위. */
			return false;
		}
	} else {
		bits = 32;
		/* [한국어] 단일 호스트. */
	}

	/* presentation to network order binary */
	if (inet_pton(AF_INET, mask, &in4_mask) <= 0
	    || inet_pton(AF_INET, addr, &in4_addr) <= 0) {
		return false;
	}

	/* check 32bits */
	bmask = (0xffffffffU << (32 - bits)) & 0xffffffffU;
	/* [한국어] 상위 bits비트만 1인 마스크 (host byte order). */
	if ((ntohl(in4_mask.s_addr) & bmask) != (ntohl(in4_addr.s_addr) & bmask)) {
		/* [한국어] network→host 변환 후 prefix 비교. */
		return false;
	}

	/* match */
	return true;
}

/*
 * [한국어]
 * iscsi_netmask_allow_addr - IPv4/v6 자동 분기 + "ANY" 와일드카드.
 *
 * "ANY"는 모든 주소 허용. '['로 시작하면 IPv6, 아니면 IPv4.
 */
static bool
iscsi_netmask_allow_addr(const char *netmask, const char *addr)
{
	if (netmask == NULL || addr == NULL) {
		return false;
	}
	if (strcasecmp(netmask, "ANY") == 0) {
		/* [한국어] 와일드카드 — 항상 허용. */
		return true;
	}
	if (netmask[0] == '[') {
		/* IPv6 */
		if (iscsi_ipv6_netmask_allow_addr(netmask, addr)) {
			return true;
		}
	} else {
		/* IPv4 */
		if (iscsi_ipv4_netmask_allow_addr(netmask, addr)) {
			return true;
		}
	}
	return false;
}

/*
 * [한국어]
 * iscsi_init_grp_allow_addr - IG의 모든 넷마스크 중 하나라도 addr를 허용하면 true.
 *
 * IG 자체가 OR 조합을 형성. 호출자가 mutex 보유 가정.
 */
static bool
iscsi_init_grp_allow_addr(struct spdk_iscsi_init_grp *igp,
			  const char *addr)
{
	struct spdk_iscsi_initiator_netmask *imask;
	/* [한국어] 순회용. */

	TAILQ_FOREACH(imask, &igp->netmask_head, tailq) {
		SPDK_DEBUGLOG(iscsi, "netmask=%s, addr=%s\n",
			      imask->mask, addr);
		if (iscsi_netmask_allow_addr(imask->mask, addr)) {
			return true;
		}
	}
	return false;
}

/*
 * [한국어]
 * iscsi_init_grp_allow_iscsi_name - IG의 IQN 리스트로 iqn 매칭 (deny 우선).
 *
 * @igp: 검색 대상 IG.
 * @iqn: 검사 IQN.
 * @result: 출력 — true(허용)/false(거부).
 * @return: 0 결정됨, -1 결정되지 않음 (등록 항목 없음).
 *
 * 매칭 규칙: 리스트 앞에서부터 평가. "!ANY"/"!iqn"이 매칭되면 즉시 거부, 그 외 "ANY"/iqn
 * 정확 매치는 즉시 허용. 어느 것도 매치되지 않으면 -1 (호출자 default 결정).
 */
static int
iscsi_init_grp_allow_iscsi_name(struct spdk_iscsi_init_grp *igp,
				const char *iqn, bool *result)
{
	struct spdk_iscsi_initiator_name *iname;
	/* [한국어] 순회용. */

	TAILQ_FOREACH(iname, &igp->initiator_head, tailq) {
		/* denied if iqn is matched */
		if ((iname->name[0] == '!')
		    && (strcasecmp(&iname->name[1], "ANY") == 0
			|| strcasecmp(&iname->name[1], iqn) == 0)) {
			/* [한국어] 접두 '!' = 명시적 거부. ANY 또는 정확 매치이면 deny. */
			*result = false;
			return 0;
		}
		/* allowed if iqn is matched */
		if (strcasecmp(iname->name, "ANY") == 0
		    || strcasecmp(iname->name, iqn) == 0) {
			/* [한국어] ANY 또는 정확 매치 → 허용. */
			*result = true;
			return 0;
		}
	}
	return -1;
	/* [한국어] 매치 없음 — 결정 미완. 호출자가 추가 IG로 시도. */
}

static struct spdk_iscsi_pg_map *iscsi_tgt_node_find_pg_map(struct spdk_iscsi_tgt_node *target,
		struct spdk_iscsi_portal_grp *pg);
/* [한국어] 전방 선언 — iscsi_tgt_node_access에서 사용. */

/*
 * [한국어]
 * iscsi_tgt_node_access - 로그인 phase에서 (target, iqn, addr)에 대한 ACL 최종 판정.
 *
 * @conn: 현재 연결 (portal로 PG 식별).
 * @target: 대상 타깃 노드.
 * @iqn/addr: initiator IQN과 IP 주소.
 * @return: true 허용, false 거부.
 *
 * 동작: conn의 portal→PG로 target의 pg_map 검색 → 그 pg_map의 모든 ig_map 순회 →
 *   IG에서 iqn 허용 여부(deny 우선) + 허용이면 넷마스크 검사. 모든 ig_map이 거부면 false.
 *
 * 호출 체인:
 *   iscsi_op_login → [iscsi_tgt_node_access]
 */
bool
iscsi_tgt_node_access(struct spdk_iscsi_conn *conn,
		      struct spdk_iscsi_tgt_node *target, const char *iqn, const char *addr)
{
	struct spdk_iscsi_portal_grp *pg;
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;
	int rc;
	bool allowed = false;
	/* [한국어] iqn 검사 결과 캐시. */

	if (conn == NULL || target == NULL || iqn == NULL || addr == NULL) {
		/* [한국어] 인자 검증. */
		return false;
	}
	pg = conn->portal->group;
	/* [한국어] 이 conn이 들어온 포털의 PG. */

	SPDK_DEBUGLOG(iscsi, "pg=%d, iqn=%s, addr=%s\n",
		      pg->tag, iqn, addr);
	pg_map = iscsi_tgt_node_find_pg_map(target, pg);
	/* [한국어] 이 타깃이 그 PG로 노출되었는지. */
	if (pg_map == NULL) {
		/* [한국어] 노출되지 않음 — 거부. */
		return false;
	}
	TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
		/* [한국어] 이 (PG)에 매핑된 모든 IG 시도. */
		rc = iscsi_init_grp_allow_iscsi_name(ig_map->ig, iqn, &allowed);
		if (rc == 0) {
			if (allowed == false) {
				/* [한국어] 명시적 deny — 즉시 거부 (다른 IG 검사하지 않음). */
				goto denied;
			} else {
				/* [한국어] 이름 허용 — 넷마스크도 검사. */
				if (iscsi_init_grp_allow_addr(ig_map->ig, addr)) {
					return true;
				}
			}
		} else {
			/* netmask is denied in this initiator group */
			/* [한국어] 이 IG에서는 결정 불가 — 다음 IG로. */
		}
	}

denied:
	SPDK_DEBUGLOG(iscsi, "access denied from %s (%s) to %s (%s:%s,%d)\n",
		      iqn, addr, target->name, conn->portal_host,
		      conn->portal_port, conn->pg_tag);
	return false;
}

/*
 * [한국어]
 * iscsi_tgt_node_allow_iscsi_name - 어느 PG/IG든 iqn을 허용하는지 (Discovery 필터용).
 *
 * Discovery는 특정 portal 정보 없이 "이 initiator에게 어떤 target을 노출할까?"를 결정하므로
 * netmask는 보지 않고 이름만으로 빠르게 필터.
 */
static bool
iscsi_tgt_node_allow_iscsi_name(struct spdk_iscsi_tgt_node *target, const char *iqn)
{
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;
	int rc;
	bool result = false;

	if (target == NULL || iqn == NULL) {
		return false;
	}

	TAILQ_FOREACH(pg_map, &target->pg_map_head, tailq) {
		/* [한국어] 모든 PG-매핑 순회. */
		TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
			/* [한국어] 그 안의 모든 IG-매핑 순회. */
			rc = iscsi_init_grp_allow_iscsi_name(ig_map->ig, iqn, &result);
			if (rc == 0) {
				/* [한국어] 결정됐으면 그대로 반환 (deny든 allow든). */
				return result;
			}
		}
	}

	return false;
	/* [한국어] 어떤 IG도 결정 못 함 → default 거부. */
}

/*
 * [한국어]
 * iscsi_copy_str - SendTargets 응답 직렬화 헬퍼 — buffer 끝까지 분할 복사.
 *
 * @data: 출력 buffer.
 * @total: 출력 buffer 누적 길이 (in/out).
 * @alloc_len: 출력 buffer 총 capacity.
 * @previous_completed_len: 이전 호출에서 이미 송신된 길이 (in/out, 분할 기준).
 * @expected_size: src의 총 길이 (NUL 포함).
 * @src: 복사 원본.
 * @return: true buffer 부족 (다음 PDU에서 이어서), false 정상 완료.
 *
 * SendTargets는 한 PDU에 다 못 들어가면 다음 PDU에 이어 보내야 하는데, 이때 송신 위치를
 * conn->send_tgt_completed_size로 보존하기 위한 보조 함수.
 */
static bool
iscsi_copy_str(char *data, int *total, int alloc_len,
	       int *previous_completed_len, int expected_size, char *src)
{
	int len = 0;
	/* [한국어] 이번 iteration에 복사할 길이. */

	assert(*previous_completed_len >= 0);

	if (alloc_len - *total < 1) {
		/* [한국어] buffer 가득 — 더 못 씀. */
		return true;
	}

	if (*previous_completed_len < expected_size) {
		/* [한국어] 이 src의 일부 또는 전부가 아직 송신 안 됨. */
		len = spdk_min(alloc_len - *total, expected_size - *previous_completed_len);
		/* [한국어] 남은 buffer와 남은 src 중 작은 쪽. */
		memcpy((char *)data + *total, src + *previous_completed_len, len);
		*total += len;
		*previous_completed_len = 0;
		/* [한국어] 이번 src를 처리했으면 카운터 리셋 — 다음 src부터 다시 0 기준. */
	} else {
		/* [한국어] 이미 다 송신된 src — 건너뜀. previous_completed_len에서 차감. */
		*previous_completed_len -= expected_size;
	}

	return false;
}

/*
 * [한국어]
 * iscsi_send_tgt_portals - 한 타깃의 모든 (PG, 포털)을 "TargetAddress=..." 라인으로 직렬화.
 *
 * @conn: 응답 대상 conn.
 * @target: 대상 타깃 노드.
 * @data/alloc_len/total: 응답 buffer.
 * @previous_completed_len: 분할 송신 상태.
 * @no_buf_space: 출력 — buffer 부족 시 true.
 * @return: 갱신된 total.
 *
 * is_private PG는 redirect 전용이므로 Discovery에 노출하지 않음. 와일드카드 host("[::]"
 * 또는 "0.0.0.0")는 conn->target_addr(자신의 IP)로 치환.
 */
static int
iscsi_send_tgt_portals(struct spdk_iscsi_conn *conn,
		       struct spdk_iscsi_tgt_node *target,
		       uint8_t *data, int alloc_len, int total,
		       int *previous_completed_len, bool *no_buf_space)
{
	char buf[MAX_TARGET_ADDR + 2];
	/* [한국어] 와일드카드 치환 시 사용 임시 buffer. */
	struct spdk_iscsi_portal_grp *pg;
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_portal *p;
	char *host;
	/* [한국어] 출력에 쓸 host (원본 또는 치환). */
	char tmp_buf[MAX_TMP_ADDR_BUF];
	/* [한국어] "TargetAddress=..." 한 줄 buffer. */
	int len;
	/* [한국어] 한 줄 길이. */

	TAILQ_FOREACH(pg_map, &target->pg_map_head, tailq) {
		pg = pg_map->pg;
		/* [한국어] 이 PG의 모든 portal에 대해. */

		if (pg->is_private) {
			/* Skip the private portal group. Portals in the private portal group
			 * will be returned only by temporary login redirection responses.
			 */
			/* [한국어] private PG는 redirect 전용 — Discovery에서는 비공개. */
			continue;
		}

		TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
			host = p->host;
			/* wildcard? */
			if (strcasecmp(host, "[::]") == 0 || strcasecmp(host, "0.0.0.0") == 0) {
				/* [한국어] 와일드카드 — conn 자신의 target_addr로 치환. */
				if (spdk_sock_is_ipv6(conn->sock)) {
					snprintf(buf, sizeof buf, "[%s]", conn->target_addr);
					host = buf;
				} else if (spdk_sock_is_ipv4(conn->sock)) {
					snprintf(buf, sizeof buf, "%s", conn->target_addr);
					host = buf;
				} else {
					/* skip portal for the family */
					/* [한국어] 알 수 없는 family — 이 portal 생략. */
					continue;
				}
			}
			SPDK_DEBUGLOG(iscsi, "TargetAddress=%s:%s,%d\n",
				      host, p->port, pg->tag);

			memset(tmp_buf, 0, sizeof(tmp_buf));
			/* Calculate the whole string size */
			len = snprintf(NULL, 0, "TargetAddress=%s:%s,%d", host, p->port, pg->tag);
			/* [한국어] 길이만 측정 (NUL 제외). */
			assert(len < MAX_TMPBUF);

			/* string contents are not fully copied */
			if (*previous_completed_len < len) {
				/* Copy the string into the temporary buffer */
				/* [한국어] 송신 미완 — 임시 buffer 채움. */
				snprintf(tmp_buf, len + 1, "TargetAddress=%s:%s,%d", host, p->port, pg->tag);
			}

			*no_buf_space = iscsi_copy_str(data, &total, alloc_len, previous_completed_len,
						       len + 1, tmp_buf);
			/* [한국어] +1 = NUL 포함. */
			if (*no_buf_space) {
				break;
			}
		}
	}

	return total;
}

/*
 * [한국어]
 * iscsi_send_tgts - SendTargets 텍스트 명령에 대한 응답 데이터 생성.
 *
 * @conn: 응답 대상 conn.
 * @iiqn: initiator IQN (ACL 필터에 사용).
 * @tiqn: 요청 target IQN ("ALL"이면 전체 노출).
 * @data/alloc_len/data_len: 응답 buffer/총 크기/이미 채워진 길이.
 * @return: 갱신된 응답 길이 (또는 잘림 시 alloc_len).
 *
 * 흐름:
 *   1) g_iscsi.target_head를 락 보호 하에 순회.
 *   2) tiqn 매칭 + iiqn ACL 통과 타깃만 선택.
 *   3) "TargetName=...\0" + 그 타깃의 portal들 직렬화.
 *   4) buffer 부족하면 conn->send_tgt_completed_size에 진행 길이 보존하여 다음 PDU 이어서.
 *
 * 호출 체인:
 *   iscsi_op_text/iscsi_op_login(Discovery) → [iscsi_send_tgts]
 */
int
iscsi_send_tgts(struct spdk_iscsi_conn *conn, const char *iiqn,
		const char *tiqn, uint8_t *data, int alloc_len, int data_len)
{
	struct spdk_iscsi_tgt_node *target;
	int total;
	int len;
	int rc;
	int previous_completed_size = 0;
	/* [한국어] 분할 송신 누적값. */
	bool no_buf_space = false;
	/* [한국어] buffer 부족 신호. */
	char tmp_buf[MAX_TMP_NAME_BUF];
	/* [한국어] "TargetName=..." 한 줄 buffer. */

	if (conn == NULL) {
		return 0;
	}
	previous_completed_size = conn->send_tgt_completed_size;
	/* [한국어] 이전 PDU에서 보낸 길이 복원. */

	total = data_len;
	if (alloc_len < 1) {
		return 0;
	}
	if (total >= alloc_len) {
		/* [한국어] 이미 buffer 가득. */
		total = alloc_len;
		data[total - 1] = '\0';
		return total;
	}

	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] target_head 보호. */
	TAILQ_FOREACH(target, &g_iscsi.target_head, tailq) {
		if (strcasecmp(tiqn, "ALL") != 0
		    && strcasecmp(tiqn, target->name) != 0) {
			/* [한국어] tiqn 필터 — 정확 매치만 (또는 ALL). */
			continue;
		}
		rc = iscsi_tgt_node_allow_iscsi_name(target, iiqn);
		if (rc == 0) {
			/* [한국어] iiqn ACL 거부 — 노출하지 않음. */
			continue;
		}

		memset(tmp_buf, 0, sizeof(tmp_buf));
		/* Calculate the whole string size */
		len = snprintf(NULL, 0, "TargetName=%s", target->name);
		assert(len < MAX_TMPBUF);

		/* String contents are not copied */
		if (previous_completed_size < len) {
			/* Copy the string into the temporary buffer */
			snprintf(tmp_buf, len + 1, "TargetName=%s", target->name);
		}

		no_buf_space = iscsi_copy_str(data, &total, alloc_len, &previous_completed_size,
					      len + 1, tmp_buf);
		if (no_buf_space) {
			break;
		}

		total = iscsi_send_tgt_portals(conn, target, data, alloc_len, total,
					       &previous_completed_size, &no_buf_space);
		/* [한국어] 그 타깃의 모든 portal 추가. */
		if (no_buf_space) {
			break;
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);

	/* Only set it when it is not successfully completed */
	if (no_buf_space) {
		/* [한국어] 분할 진행 — 다음 PDU에 이어서. */
		conn->send_tgt_completed_size += total;
	} else {
		/* [한국어] 완료. */
		conn->send_tgt_completed_size = 0;
	}

	return total;
}

/*
 * [한국어]
 * iscsi_find_tgt_node - target_name으로 타깃 노드 검색 (case-insensitive).
 *
 * 호출자 락 보유 가정 (변경과의 race 방지).
 */
struct spdk_iscsi_tgt_node *
iscsi_find_tgt_node(const char *target_name)
{
	struct spdk_iscsi_tgt_node *target;

	if (target_name == NULL) {
		return NULL;
	}
	TAILQ_FOREACH(target, &g_iscsi.target_head, tailq) {
		if (strcasecmp(target_name, target->name) == 0) {
			return target;
		}
	}
	return NULL;
}

/*
 * [한국어]
 * iscsi_tgt_node_register - 타깃을 g_iscsi.target_head에 등록 (이름 중복 검사 포함).
 *
 * @return: 0 OK, -EEXIST.
 *
 * mutex 획득 후 find→insert. 이름 중복 시 등록하지 않음.
 */
static int
iscsi_tgt_node_register(struct spdk_iscsi_tgt_node *target)
{
	pthread_mutex_lock(&g_iscsi.mutex);

	if (iscsi_find_tgt_node(target->name) != NULL) {
		pthread_mutex_unlock(&g_iscsi.mutex);
		return -EEXIST;
	}

	TAILQ_INSERT_TAIL(&g_iscsi.target_head, target, tailq);

	pthread_mutex_unlock(&g_iscsi.mutex);
	return 0;
}

/*
 * [한국어]
 * iscsi_tgt_node_unregister - 등록 해제. 호출자 락 보유 가정 (delete 흐름 내부).
 *
 * @return: 0 OK, -1 미발견.
 */
static int
iscsi_tgt_node_unregister(struct spdk_iscsi_tgt_node *target)
{
	struct spdk_iscsi_tgt_node *t;

	TAILQ_FOREACH(t, &g_iscsi.target_head, tailq) {
		if (t == target) {
			/* [한국어] 포인터 동일성 검사 (이름이 아닌 객체 매치). */
			TAILQ_REMOVE(&g_iscsi.target_head, t, tailq);
			return 0;
		}
	}

	return -1;
}

/*
 * [한국어]
 * iscsi_pg_map_find_ig_map - pg_map의 ig_map 리스트에서 ig 일치 노드 검색.
 */
static struct spdk_iscsi_ig_map *
iscsi_pg_map_find_ig_map(struct spdk_iscsi_pg_map *pg_map,
			 struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_ig_map *ig_map;

	TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
		if (ig_map->ig == ig) {
			/* [한국어] IG 포인터 동일성. */
			return ig_map;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * iscsi_pg_map_add_ig_map - pg_map에 새 ig_map 추가 + IG ref 증가.
 *
 * @return: 새 ig_map 또는 NULL (중복/ENOMEM).
 *
 * IG의 ref count를 증가시켜 IG 삭제 RPC가 매핑 중인 IG는 거부하도록 한다.
 */
static struct spdk_iscsi_ig_map *
iscsi_pg_map_add_ig_map(struct spdk_iscsi_pg_map *pg_map,
			struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_ig_map *ig_map;

	if (iscsi_pg_map_find_ig_map(pg_map, ig) != NULL) {
		/* [한국어] 중복. */
		return NULL;
	}

	ig_map = malloc(sizeof(*ig_map));
	if (ig_map == NULL) {
		return NULL;
	}

	ig_map->ig = ig;
	ig->ref++;
	/* [한국어] IG의 참조 카운터 증가 (삭제 보호). */
	pg_map->num_ig_maps++;
	TAILQ_INSERT_TAIL(&pg_map->ig_map_head, ig_map, tailq);

	return ig_map;
}

/*
 * [한국어]
 * _iscsi_pg_map_delete_ig_map - 단일 ig_map 분리·free + IG ref 감소.
 */
static void
_iscsi_pg_map_delete_ig_map(struct spdk_iscsi_pg_map *pg_map,
			    struct spdk_iscsi_ig_map *ig_map)
{
	TAILQ_REMOVE(&pg_map->ig_map_head, ig_map, tailq);
	pg_map->num_ig_maps--;
	ig_map->ig->ref--;
	/* [한국어] IG ref 감소 — 0이 되면 외부 삭제 가능. */
	free(ig_map);
}

/*
 * [한국어]
 * iscsi_pg_map_delete_ig_map - ig 일치 ig_map 1개 제거 (find+_delete 헬퍼).
 *
 * @return: 0 OK, -ENOENT.
 */
static int
iscsi_pg_map_delete_ig_map(struct spdk_iscsi_pg_map *pg_map,
			   struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_ig_map *ig_map;

	ig_map = iscsi_pg_map_find_ig_map(pg_map, ig);
	if (ig_map == NULL) {
		return -ENOENT;
	}

	_iscsi_pg_map_delete_ig_map(pg_map, ig_map);
	return 0;
}

/*
 * [한국어]
 * iscsi_pg_map_delete_all_ig_maps - pg_map의 모든 ig_map 제거.
 *
 * FOREACH_SAFE로 순회 중 free 안전.
 */
static void
iscsi_pg_map_delete_all_ig_maps(struct spdk_iscsi_pg_map *pg_map)
{
	struct spdk_iscsi_ig_map *ig_map, *tmp;

	TAILQ_FOREACH_SAFE(ig_map, &pg_map->ig_map_head, tailq, tmp) {
		_iscsi_pg_map_delete_ig_map(pg_map, ig_map);
	}
}

/*
 * [한국어]
 * iscsi_tgt_node_find_pg_map - target에서 pg 일치 pg_map 검색.
 */
static struct spdk_iscsi_pg_map *
iscsi_tgt_node_find_pg_map(struct spdk_iscsi_tgt_node *target,
			   struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_pg_map *pg_map;

	TAILQ_FOREACH(pg_map, &target->pg_map_head, tailq) {
		if (pg_map->pg == pg) {
			return pg_map;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * iscsi_tgt_node_add_pg_map - 새 pg_map 추가 + SCSI port 추가 + PG ref 증가.
 *
 * SCSI port 이름은 "<target>,t,0x%04X<pg_tag>" 형식 (SAM-3 §4.7).
 *
 * @return: 새 pg_map 또는 NULL (중복/한도/ENOMEM/SCSI 실패).
 */
static struct spdk_iscsi_pg_map *
iscsi_tgt_node_add_pg_map(struct spdk_iscsi_tgt_node *target,
			  struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_pg_map *pg_map;
	char port_name[MAX_TMPBUF];
	int rc;

	if (iscsi_tgt_node_find_pg_map(target, pg) != NULL) {
		return NULL;
	}

	if (target->num_pg_maps >= SPDK_SCSI_DEV_MAX_PORTS) {
		/* [한국어] SCSI device 당 port 한도. */
		SPDK_ERRLOG("Number of PG maps is more than allowed (max=%d)\n",
			    SPDK_SCSI_DEV_MAX_PORTS);
		return NULL;
	}

	pg_map = calloc(1, sizeof(*pg_map));
	if (pg_map == NULL) {
		return NULL;
	}

	snprintf(port_name, sizeof(port_name), "%s,t,0x%4.4x",
		 spdk_scsi_dev_get_name(target->dev), pg->tag);
	/* [한국어] SAM-3 SCSI port name format: "<dev>,t,0x<TPGT 4hex>". */
	rc = spdk_scsi_dev_add_port(target->dev, pg->tag, port_name);
	/* [한국어] SCSI device에 port 등록 — port_index = pg->tag. */
	if (rc != 0) {
		free(pg_map);
		return NULL;
	}

	TAILQ_INIT(&pg_map->ig_map_head);
	pg_map->num_ig_maps = 0;
	pg->ref++;
	/* [한국어] PG ref 증가 — 매핑 중인 PG 삭제 보호. */
	pg_map->pg = pg;
	target->num_pg_maps++;
	TAILQ_INSERT_TAIL(&target->pg_map_head, pg_map, tailq);

	return pg_map;
}

/*
 * [한국어]
 * _iscsi_tgt_node_delete_pg_map - pg_map 분리·free + SCSI port 제거 + PG ref 감소.
 */
static void
_iscsi_tgt_node_delete_pg_map(struct spdk_iscsi_tgt_node *target,
			      struct spdk_iscsi_pg_map *pg_map)
{
	TAILQ_REMOVE(&target->pg_map_head, pg_map, tailq);
	target->num_pg_maps--;
	pg_map->pg->ref--;
	/* [한국어] PG ref 감소. */

	spdk_scsi_dev_delete_port(target->dev, pg_map->pg->tag);
	/* [한국어] SCSI port 제거 (대응되는 SAM-3 port). */

	free(pg_map);
}

/*
 * [한국어]
 * iscsi_tgt_node_delete_pg_map - pg 일치 pg_map 강제 제거 (자식 ig_map 모두 free).
 *
 * @return: 0 OK, -ENOENT.
 */
static int
iscsi_tgt_node_delete_pg_map(struct spdk_iscsi_tgt_node *target,
			     struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_pg_map *pg_map;

	pg_map = iscsi_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		return -ENOENT;
	}

	if (pg_map->num_ig_maps > 0) {
		/* [한국어] 부착된 ig_map이 있어도 강제 제거 (셧다운 등). */
		SPDK_DEBUGLOG(iscsi, "delete %d ig_maps forcefully\n",
			      pg_map->num_ig_maps);
	}

	iscsi_pg_map_delete_all_ig_maps(pg_map);
	_iscsi_tgt_node_delete_pg_map(target, pg_map);
	return 0;
}

/*
 * [한국어]
 * iscsi_tgt_node_delete_ig_maps - ig 일치 모든 ig_map을 모든 pg_map에서 제거.
 *
 * IG 삭제 시 사용. 결과적으로 ig_map 수가 0인 pg_map도 함께 정리.
 */
static void
iscsi_tgt_node_delete_ig_maps(struct spdk_iscsi_tgt_node *target,
			      struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_pg_map *pg_map, *tmp;

	TAILQ_FOREACH_SAFE(pg_map, &target->pg_map_head, tailq, tmp) {
		iscsi_pg_map_delete_ig_map(pg_map, ig);
		if (pg_map->num_ig_maps == 0) {
			/* [한국어] 비어있는 pg_map은 정리. */
			_iscsi_tgt_node_delete_pg_map(target, pg_map);
		}
	}
}

/*
 * [한국어]
 * iscsi_tgt_node_delete_all_pg_maps - 모든 pg_map과 자식 ig_map 일괄 free.
 *
 * 타깃 destruct 경로에서 호출.
 */
static void
iscsi_tgt_node_delete_all_pg_maps(struct spdk_iscsi_tgt_node *target)
{
	struct spdk_iscsi_pg_map *pg_map, *tmp;

	TAILQ_FOREACH_SAFE(pg_map, &target->pg_map_head, tailq, tmp) {
		iscsi_pg_map_delete_all_ig_maps(pg_map);
		_iscsi_tgt_node_delete_pg_map(target, pg_map);
	}
}

/*
 * [한국어]
 * _iscsi_tgt_node_destruct - SCSI device destruct 완료 후 호출되는 콜백.
 *
 * @cb_arg: spdk_iscsi_tgt_node*.
 * @rc: scsi_dev_destruct 결과.
 *
 * rc!=0이면 cb_fn(rc) 발사 후 종료. 정상이면 mutex 보호 하에 모든 pg_map 정리 → mutex_destroy
 * → histogram free → 타깃 free → cb_fn(0).
 */
static void
_iscsi_tgt_node_destruct(void *cb_arg, int rc)
{
	struct spdk_iscsi_tgt_node *target = cb_arg;
	iscsi_tgt_node_destruct_cb destruct_cb_fn = target->destruct_cb_fn;
	void *destruct_cb_arg = target->destruct_cb_arg;

	if (rc != 0) {
		/* [한국어] SCSI device destruct 실패 — 콜백에 에러 전달 후 종료 (free 안 함). */
		if (destruct_cb_fn) {
			destruct_cb_fn(destruct_cb_arg, rc);
		}
		return;
	}

	pthread_mutex_lock(&g_iscsi.mutex);
	iscsi_tgt_node_delete_all_pg_maps(target);
	/* [한국어] 모든 pg_map 정리 (PG/IG ref 감소). */
	pthread_mutex_unlock(&g_iscsi.mutex);

	pthread_mutex_destroy(&target->mutex);

	spdk_histogram_data_free(target->histogram);
	/* [한국어] histogram NULL이면 no-op. */

	free(target);

	if (destruct_cb_fn) {
		destruct_cb_fn(destruct_cb_arg, 0);
	}
}

/*
 * [한국어]
 * iscsi_tgt_node_check_active_conns - 활성 conn이 0이 될 때까지 polling.
 *
 * 10us 주기 poller. 0이 되면 unregister 후 spdk_scsi_dev_destruct 시작.
 */
static int
iscsi_tgt_node_check_active_conns(void *arg)
{
	struct spdk_iscsi_tgt_node *target = arg;

	if (iscsi_get_active_conns(target) != 0) {
		/* [한국어] 아직 활성 conn 존재 — 다음 tick에 다시 검사. */
		return SPDK_POLLER_BUSY;
	}

	spdk_poller_unregister(&target->destruct_poller);
	/* [한국어] 더 이상 폴링 안 함 — &포인터 → NULL. */

	spdk_scsi_dev_destruct(target->dev, _iscsi_tgt_node_destruct, target);
	/* [한국어] SCSI device destruct (비동기, 완료 시 _iscsi_tgt_node_destruct 호출). */

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * iscsi_tgt_node_destruct - 타깃 노드 비동기 소멸 진입점.
 *
 * @cb_fn/cb_arg: 완료 콜백.
 *
 * 흐름: destructed 플래그 체크 → 모든 conn에 logout 요청 → 활성 conn 있으면 poller로
 * 대기, 없으면 즉시 SCSI device destruct.
 */
static void
iscsi_tgt_node_destruct(struct spdk_iscsi_tgt_node *target,
			iscsi_tgt_node_destruct_cb cb_fn, void *cb_arg)
{
	if (target == NULL) {
		/* [한국어] NULL 허용 — caller 편의. */
		if (cb_fn) {
			cb_fn(cb_arg, -ENOENT);
		}
		return;
	}

	if (target->destructed) {
		/* [한국어] 이미 destruct 중복 진입. */
		SPDK_ERRLOG("Destructing %s is already started\n", target->name);
		if (cb_fn) {
			cb_fn(cb_arg, -EBUSY);
		}
		return;
	}

	target->destructed = true;
	target->destruct_cb_fn = cb_fn;
	target->destruct_cb_arg = cb_arg;

	iscsi_conns_request_logout(target, -1);
	/* [한국어] -1 = 모든 PG의 모든 conn에 logout async 트리거. */

	if (iscsi_get_active_conns(target) != 0) {
		/* [한국어] logout 진행 중 — poller로 완료 대기. */
		target->destruct_poller = SPDK_POLLER_REGISTER(iscsi_tgt_node_check_active_conns,
					  target, 10);
	} else {
		/* [한국어] 이미 0 — 즉시 SCSI device destruct. */
		spdk_scsi_dev_destruct(target->dev, _iscsi_tgt_node_destruct, target);
	}

}

/*
 * [한국어]
 * iscsi_tgt_node_delete_pg_ig_map - 단일 (pg_tag, ig_tag) 매핑 1개 제거.
 *
 * @return: 0 OK, -ENOENT (PG/IG/매핑 어느 하나 미존재).
 *
 * pg_map의 마지막 ig_map이 제거되면 pg_map도 함께 정리.
 */
static int
iscsi_tgt_node_delete_pg_ig_map(struct spdk_iscsi_tgt_node *target,
				int pg_tag, int ig_tag)
{
	struct spdk_iscsi_portal_grp	*pg;
	struct spdk_iscsi_init_grp	*ig;
	struct spdk_iscsi_pg_map	*pg_map;
	struct spdk_iscsi_ig_map	*ig_map;

	pg = iscsi_portal_grp_find_by_tag(pg_tag);
	if (pg == NULL) {
		SPDK_ERRLOG("%s: PortalGroup%d not found\n", target->name, pg_tag);
		return -ENOENT;
	}
	ig = iscsi_init_grp_find_by_tag(ig_tag);
	if (ig == NULL) {
		SPDK_ERRLOG("%s: InitiatorGroup%d not found\n", target->name, ig_tag);
		return -ENOENT;
	}

	pg_map = iscsi_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		SPDK_ERRLOG("%s: PortalGroup%d is not mapped\n", target->name, pg_tag);
		return -ENOENT;
	}
	ig_map = iscsi_pg_map_find_ig_map(pg_map, ig);
	if (ig_map == NULL) {
		SPDK_ERRLOG("%s: InitiatorGroup%d is not mapped\n", target->name, pg_tag);
		return -ENOENT;
	}

	_iscsi_pg_map_delete_ig_map(pg_map, ig_map);
	if (pg_map->num_ig_maps == 0) {
		/* [한국어] 마지막 ig_map 제거 — pg_map도 정리. */
		_iscsi_tgt_node_delete_pg_map(target, pg_map);
	}

	return 0;
}

/*
 * [한국어]
 * iscsi_tgt_node_add_pg_ig_map - 단일 (pg_tag, ig_tag) 매핑 1개 추가.
 *
 * pg_map이 없으면 새로 생성. ig_map 추가 실패 시 새로 만든 pg_map은 롤백 (atomicity).
 */
static int
iscsi_tgt_node_add_pg_ig_map(struct spdk_iscsi_tgt_node *target,
			     int pg_tag, int ig_tag)
{
	struct spdk_iscsi_portal_grp	*pg;
	struct spdk_iscsi_pg_map	*pg_map;
	struct spdk_iscsi_init_grp	*ig;
	struct spdk_iscsi_ig_map	*ig_map;
	bool				new_pg_map = false;
	/* [한국어] 이번 호출에서 새 pg_map을 만들었는지 (롤백 결정용). */

	pg = iscsi_portal_grp_find_by_tag(pg_tag);
	if (pg == NULL) {
		SPDK_ERRLOG("%s: PortalGroup%d not found\n", target->name, pg_tag);
		return -ENOENT;
	}
	ig = iscsi_init_grp_find_by_tag(ig_tag);
	if (ig == NULL) {
		SPDK_ERRLOG("%s: InitiatorGroup%d not found\n", target->name, ig_tag);
		return -ENOENT;
	}

	/* get existing pg_map or create new pg_map and add it to target */
	pg_map = iscsi_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		/* [한국어] 첫 매핑 — 새 pg_map 생성. */
		pg_map = iscsi_tgt_node_add_pg_map(target, pg);
		if (pg_map == NULL) {
			goto failed;
		}
		new_pg_map = true;
	}

	/* create new ig_map and add it to pg_map */
	ig_map = iscsi_pg_map_add_ig_map(pg_map, ig);
	if (ig_map == NULL) {
		goto failed;
	}

	return 0;

failed:
	if (new_pg_map) {
		/* [한국어] 새로 만든 pg_map만 롤백 (기존 pg_map은 보존). */
		_iscsi_tgt_node_delete_pg_map(target, pg_map);
	}

	return -1;
}

/*
 * [한국어]
 * iscsi_target_node_add_pg_ig_maps - 다수의 (pg_tag, ig_tag) 쌍 일괄 추가, 실패 시 롤백.
 *
 * RPC 진입점. mutex 보호.
 */
int
iscsi_target_node_add_pg_ig_maps(struct spdk_iscsi_tgt_node *target,
				 int *pg_tag_list, int *ig_tag_list, uint16_t num_maps)
{
	uint16_t i;
	int rc;

	pthread_mutex_lock(&g_iscsi.mutex);
	for (i = 0; i < num_maps; i++) {
		rc = iscsi_tgt_node_add_pg_ig_map(target, pg_tag_list[i],
						  ig_tag_list[i]);
		if (rc != 0) {
			SPDK_ERRLOG("could not add map to target\n");
			goto invalid;
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	return 0;

invalid:
	for (; i > 0; --i) {
		/* [한국어] 직전까지 추가된 매핑 역순 제거 (트랜잭션 롤백). */
		iscsi_tgt_node_delete_pg_ig_map(target, pg_tag_list[i - 1],
						ig_tag_list[i - 1]);
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	return -1;
}

/*
 * [한국어]
 * iscsi_target_node_remove_pg_ig_maps - 일괄 제거, 실패 시 add 롤백 + 안전 폴백.
 *
 * 롤백 add도 실패하면 모든 매핑 제거 (안전한 일관 상태).
 */
int
iscsi_target_node_remove_pg_ig_maps(struct spdk_iscsi_tgt_node *target,
				    int *pg_tag_list, int *ig_tag_list, uint16_t num_maps)
{
	uint16_t i;
	int rc;

	pthread_mutex_lock(&g_iscsi.mutex);
	for (i = 0; i < num_maps; i++) {
		rc = iscsi_tgt_node_delete_pg_ig_map(target, pg_tag_list[i],
						     ig_tag_list[i]);
		if (rc != 0) {
			SPDK_ERRLOG("could not delete map from target\n");
			goto invalid;
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	return 0;

invalid:
	for (; i > 0; --i) {
		rc = iscsi_tgt_node_add_pg_ig_map(target, pg_tag_list[i - 1],
						  ig_tag_list[i - 1]);
		if (rc != 0) {
			/* [한국어] 롤백조차 실패 — 모든 매핑 폐기. */
			iscsi_tgt_node_delete_all_pg_maps(target);
			break;
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	return -1;
}

/*
 * [한국어]
 * iscsi_tgt_node_redirect - 특정 PG에 대해 임시 redirect 주소 설정/해제.
 *
 * @host/port: NULL이면 redirect 해제, 아니면 (host, port) 설정.
 * @return: 0 OK, -EINVAL.
 *
 * RFC 3720 §5.3.3 Login Redirect — 로그인 응답에 TargetAddress=redirect:port,tag 키 첨부.
 * private PG에는 설정 불가 (redirect 결과가 또 다른 redirect 대상이 되어선 안 됨).
 * (redirect_host/port가 다른 private PG 안의 포털과 일치해야 — 정책: redirect 대상은
 * private PG 내에서만 선택).
 */
int
iscsi_tgt_node_redirect(struct spdk_iscsi_tgt_node *target, int pg_tag,
			const char *host, const char *port)
{
	struct spdk_iscsi_portal_grp *pg;
	struct spdk_iscsi_pg_map *pg_map;
	struct sockaddr_storage sa;
	/* [한국어] 주소 파싱 검증용. */

	if (target == NULL) {
		return -EINVAL;
	}

	pg = iscsi_portal_grp_find_by_tag(pg_tag);
	if (pg == NULL) {
		SPDK_ERRLOG("Portal group %d is not found.\n", pg_tag);
		return -EINVAL;
	}

	if (pg->is_private) {
		/* [한국어] private PG는 redirect 출발지가 될 수 없음 (이미 비공개). */
		SPDK_ERRLOG("Portal group %d is not public portal group.\n", pg_tag);
		return -EINVAL;
	}

	pg_map = iscsi_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		SPDK_ERRLOG("Portal group %d is not mapped.\n", pg_tag);
		return -EINVAL;
	}

	if (host == NULL && port == NULL) {
		/* Clear redirect setting. */
		/* [한국어] 양쪽 NULL = 해제. */
		memset(pg_map->redirect_host, 0, MAX_PORTAL_ADDR + 1);
		memset(pg_map->redirect_port, 0, MAX_PORTAL_PORT + 1);
	} else {
		if (iscsi_parse_redirect_addr(&sa, host, port) != 0) {
			/* [한국어] AI_NUMERIC* 검증 실패. */
			SPDK_ERRLOG("IP address-port pair is not valid.\n");
			return -EINVAL;
		}

		if (iscsi_portal_grp_find_portal_by_addr(pg, port, host) != NULL) {
			/* [한국어] 같은 PG 안에 이미 동일 portal 존재 — redirect 의미 없음. */
			SPDK_ERRLOG("IP address-port pair must be chosen from a "
				    "different private portal group\n");
			return -EINVAL;
		}

		snprintf(pg_map->redirect_host, MAX_PORTAL_ADDR + 1, "%s", host);
		snprintf(pg_map->redirect_port, MAX_PORTAL_PORT + 1, "%s", port);
	}

	return 0;
}

/*
 * [한국어]
 * iscsi_tgt_node_is_redirected - 이 conn에 대해 redirect 설정이 있는지 + 결과 buffer에 작성.
 *
 * @buf: 출력 — "host:port" 형식.
 * @buf_len: buf 크기.
 * @return: true redirect 있음, false 없음.
 */
bool
iscsi_tgt_node_is_redirected(struct spdk_iscsi_conn *conn,
			     struct spdk_iscsi_tgt_node *target,
			     char *buf, int buf_len)
{
	struct spdk_iscsi_pg_map *pg_map;

	if (conn == NULL || target == NULL || buf == NULL || buf_len == 0) {
		return false;
	}

	pg_map = iscsi_tgt_node_find_pg_map(target, conn->portal->group);
	if (pg_map == NULL) {
		return false;
	}

	if (pg_map->redirect_host[0] == '\0' || pg_map->redirect_port[0] == '\0') {
		/* [한국어] redirect 미설정. */
		return false;
	}

	snprintf(buf, buf_len, "%s:%s", pg_map->redirect_host, pg_map->redirect_port);

	return true;
}

/*
 * [한국어]
 * check_iscsi_name - iSCSI 이름 형식 검증 (RFC 3720 §3.2.6.3).
 *
 * 길이 ≤223, 제어/공백/특수문자 일부 거부, "iqn./eui./naa." 접두 형식 검사.
 * iqn 형식: "iqn.YYYY-MM.reversed.domain.name". eui/naa는 hex 영역만 검증되어야 하나
 * 본 구현에서는 접두만 확인 (XXX 표시).
 */
static int
check_iscsi_name(const char *name)
{
	const unsigned char *up = (const unsigned char *) name;
	/* [한국어] unsigned 문자 비교 (서명 비트 회피). */
	size_t n;

	/* valid iSCSI name no larger than 223 bytes */
	if (strlen(name) > MAX_TARGET_NAME) {
		return -1;
	}

	/* valid iSCSI name? */
	for (n = 0; up[n] != 0; n++) {
		/* [한국어] 글자 집합 검사 — RFC 3720 §3.2.6.3에 정의된 LCASE-ALPHA + DIGIT + 일부 특수만 허용. */
		if (up[n] > 0x00U && up[n] <= 0x2cU) {
			/* [한국어] 0x01~0x2C: 제어 문자와 공백, 부호 일부. 거부. */
			return -1;
		}
		if (up[n] == 0x2fU) {
			/* [한국어] '/' 거부. */
			return -1;
		}
		if (up[n] >= 0x3bU && up[n] <= 0x40U) {
			/* [한국어] ';'..'@' 거부. */
			return -1;
		}
		if (up[n] >= 0x5bU && up[n] <= 0x60U) {
			/* [한국어] '['..'`' 거부. */
			return -1;
		}
		if (up[n] >= 0x7bU && up[n] <= 0x7fU) {
			/* [한국어] '{'..DEL 거부. */
			return -1;
		}
		if (isspace(up[n])) {
			/* [한국어] 추가 공백 거부 (안전망). */
			return -1;
		}
	}
	/* valid format? */
	if (strncasecmp(name, "iqn.", 4) == 0) {
		/* iqn.YYYY-MM.reversed.domain.name */
		if (!isdigit(up[4]) || !isdigit(up[5]) || !isdigit(up[6])
		    || !isdigit(up[7]) || up[8] != '-' || !isdigit(up[9])
		    || !isdigit(up[10]) || up[11] != '.') {
			/* [한국어] iqn 접두 후 4자리 연도 + '-' + 2자리 월 + '.' 검사. */
			SPDK_ERRLOG("invalid iqn format. "
				    "expect \"iqn.YYYY-MM.reversed.domain.name\"\n");
			return -1;
		}
	} else if (strncasecmp(name, "eui.", 4) == 0) {
		/* EUI-64 -> 16bytes */
		/* XXX */
		/* [한국어] EUI-64 hex 16자 검증은 미구현. */
	} else if (strncasecmp(name, "naa.", 4) == 0) {
		/* 64bit -> 16bytes, 128bit -> 32bytes */
		/* XXX */
		/* [한국어] NAA hex 검증은 미구현. */
	}
	/* OK */
	return 0;
}

/*
 * [한국어]
 * iscsi_check_chap_params - CHAP 4-튜플 합법 조합 판정 (외부 노출).
 *
 * 합법 조합 (DRM 줄임표):
 *   Auto:   d=F, r=F, m=F
 *   None:   d=T, r=F, m=F
 *   CHAP:   d=F, r=T, m=F
 *   Mutual: d=F, r=T, m=T
 * 그 외는 모순.
 *
 * @return: true 합법, false 불법.
 */
bool
iscsi_check_chap_params(bool disable, bool require, bool mutual, int group)
{
	if (group < 0) {
		SPDK_ERRLOG("Invalid auth group ID (%d)\n", group);
		return false;
	}
	if ((!disable && !require && !mutual) ||	/* Auto */
	    (disable && !require && !mutual) ||	/* None */
	    (!disable && require && !mutual) ||	/* CHAP */
	    (!disable && require && mutual)) {	/* CHAP Mutual */
		return true;
	}
	SPDK_ERRLOG("Invalid combination of CHAP params (d=%d,r=%d,m=%d)\n",
		    disable, require, mutual);
	return false;
}

/*
 * [한국어]
 * iscsi_tgt_node_construct - 신규 타깃 노드 생성 진입점 (외부 RPC).
 *
 * @target_index: 식별자 (RPC가 부여한 번호).
 * @name: TargetName (iqn./eui./naa. 접두 또는 그냥 짧은 이름 — 그 경우 nodebase 자동 prefix).
 * @alias: TargetAlias (옵션).
 * @pg_tag_list/ig_tag_list/num_maps: 초기 (PG, IG) 매핑.
 * @bdev_name_list/lun_id_list/num_luns: 초기 LUN 부착.
 * @queue_depth: 0 또는 g_iscsi.MaxQueueDepth 초과 시 max로 캡.
 * @disable/require/mutual_chap, chap_group: 인증 정책.
 * @header/data_digest: 다이제스트 강제 여부.
 *
 * 부분 실패 시 iscsi_tgt_node_destruct로 일관 정리.
 */
struct spdk_iscsi_tgt_node *iscsi_tgt_node_construct(int target_index,
		const char *name, const char *alias,
		int *pg_tag_list, int *ig_tag_list, uint16_t num_maps,
		const char *bdev_name_list[], int *lun_id_list, int num_luns,
		int queue_depth,
		bool disable_chap, bool require_chap, bool mutual_chap, int chap_group,
		bool header_digest, bool data_digest)
{
	char				fullname[MAX_TMPBUF];
	/* [한국어] iqn 접두 자동 prepend 후 최종 이름. */
	struct spdk_iscsi_tgt_node	*target;
	int				rc;

	if (!iscsi_check_chap_params(disable_chap, require_chap,
				     mutual_chap, chap_group)) {
		/* [한국어] CHAP 4-튜플 모순 거부. */
		return NULL;
	}

	if (num_maps == 0) {
		/* [한국어] 최소 1개 매핑 필요 — 없으면 conn 진입 불가. */
		SPDK_ERRLOG("num_maps = 0\n");
		return NULL;
	}

	if (name == NULL) {
		SPDK_ERRLOG("TargetName not found\n");
		return NULL;
	}

	if (strncasecmp(name, "iqn.", 4) != 0
	    && strncasecmp(name, "eui.", 4) != 0
	    && strncasecmp(name, "naa.", 4) != 0) {
		/* [한국어] 접두 없으면 nodebase prepend — "iqn.2016-06.io.spdk:<name>" 식. */
		snprintf(fullname, sizeof(fullname), "%s:%s", g_iscsi.nodebase, name);
	} else {
		snprintf(fullname, sizeof(fullname), "%s", name);
	}

	if (check_iscsi_name(fullname) != 0) {
		SPDK_ERRLOG("TargetName %s contains an invalid character or format.\n",
			    name);
		return NULL;
	}

	target = calloc(1, sizeof(*target));
	if (!target) {
		SPDK_ERRLOG("could not allocate target\n");
		return NULL;
	}

	rc = pthread_mutex_init(&target->mutex, NULL);
	/* [한국어] CHAP 정책 갱신 등 변경에 사용되는 per-target 락. */
	if (rc != 0) {
		SPDK_ERRLOG("tgt_node%d: mutex_init() failed\n", target->num);
		iscsi_tgt_node_destruct(target, NULL, NULL);
		return NULL;
	}

	target->num = target_index;

	memcpy(target->name, fullname, strlen(fullname));
	/* [한국어] calloc으로 NUL은 이미 0. */

	if (alias != NULL) {
		if (strlen(alias) > MAX_TARGET_NAME) {
			iscsi_tgt_node_destruct(target, NULL, NULL);
			return NULL;
		}
		memcpy(target->alias, alias, strlen(alias));
	}

	target->dev = spdk_scsi_dev_construct(fullname, bdev_name_list, lun_id_list, num_luns,
					      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);
	/* [한국어] bdev_name_list[i]를 LUN i로 갖는 SCSI device 생성. SPC 프로토콜 식별자=ISCSI. */
	if (!target->dev) {
		SPDK_ERRLOG("Could not construct SCSI device\n");
		iscsi_tgt_node_destruct(target, NULL, NULL);
		return NULL;
	}

	TAILQ_INIT(&target->pg_map_head);
	rc = iscsi_target_node_add_pg_ig_maps(target, pg_tag_list,
					      ig_tag_list, num_maps);
	/* [한국어] 매핑 일괄 추가 (실패 시 자동 롤백 — 위 함수 참고). */
	if (rc != 0) {
		SPDK_ERRLOG("could not add map to target\n");
		iscsi_tgt_node_destruct(target, NULL, NULL);
		return NULL;
	}

	target->disable_chap = disable_chap;
	target->require_chap = require_chap;
	target->mutual_chap = mutual_chap;
	target->chap_group = chap_group;
	target->header_digest = header_digest;
	target->data_digest = data_digest;
	/* [한국어] 정책 일괄 저장. */

	if (queue_depth > 0 && ((uint32_t)queue_depth <= g_iscsi.MaxQueueDepth)) {
		target->queue_depth = queue_depth;
	} else {
		/* [한국어] 0 또는 MaxQueueDepth 초과 — 글로벌 max로 강제. */
		SPDK_DEBUGLOG(iscsi, "QueueDepth %d is invalid and %d is used instead.\n",
			      queue_depth, g_iscsi.MaxQueueDepth);
		target->queue_depth = g_iscsi.MaxQueueDepth;
	}

	rc = iscsi_tgt_node_register(target);
	if (rc != 0) {
		/* [한국어] 이름 중복 — 롤백. */
		SPDK_ERRLOG("register target is failed\n");
		iscsi_tgt_node_destruct(target, NULL, NULL);
		return NULL;
	}

	return target;
}

/*
 * [한국어]
 * iscsi_shutdown_tgt_nodes - 모든 타깃 노드 일괄 destruct (셧다운 경로).
 *
 * 패턴: 락 잡고 head에서 분리 → 락 풀고 destruct → 락 다시 잡기 (destruct 내부에서 mutex
 * 잡으므로 재진입 회피).
 */
void
iscsi_shutdown_tgt_nodes(void)
{
	struct spdk_iscsi_tgt_node *target;

	pthread_mutex_lock(&g_iscsi.mutex);
	while (!TAILQ_EMPTY(&g_iscsi.target_head)) {
		target = TAILQ_FIRST(&g_iscsi.target_head);
		TAILQ_REMOVE(&g_iscsi.target_head, target, tailq);

		pthread_mutex_unlock(&g_iscsi.mutex);

		iscsi_tgt_node_destruct(target, NULL, NULL);

		pthread_mutex_lock(&g_iscsi.mutex);
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
}

/*
 * [한국어]
 * iscsi_shutdown_tgt_node_by_name - 이름으로 단일 타깃 destruct (RPC iscsi_delete_target_node).
 *
 * @cb_fn: 완료 콜백 (destruct는 비동기).
 *
 * 미존재 시 콜백에 -ENOENT.
 */
void
iscsi_shutdown_tgt_node_by_name(const char *target_name,
				iscsi_tgt_node_destruct_cb cb_fn, void *cb_arg)
{
	struct spdk_iscsi_tgt_node *target;

	pthread_mutex_lock(&g_iscsi.mutex);
	target = iscsi_find_tgt_node(target_name);
	if (target != NULL) {
		iscsi_tgt_node_unregister(target);
		/* [한국어] 즉시 unregister하여 새 conn이 ACL 검사에서 못 보게. */
		pthread_mutex_unlock(&g_iscsi.mutex);

		iscsi_tgt_node_destruct(target, cb_fn, cb_arg);

		return;
	}
	pthread_mutex_unlock(&g_iscsi.mutex);

	if (cb_fn) {
		cb_fn(cb_arg, -ENOENT);
	}
}

/*
 * [한국어]
 * iscsi_tgt_node_is_destructed - destruct가 시작되었는지 조회.
 */
bool
iscsi_tgt_node_is_destructed(struct spdk_iscsi_tgt_node *target)
{
	return target->destructed;
}

/*
 * [한국어]
 * iscsi_tgt_node_cleanup_luns - 로그아웃 시 conn별 outstanding task 정리 (SCSI Target Reset).
 *
 * 각 LUN에 대해 가짜 management task를 만들어 abort_task_set(TARGET_RESET) 발사.
 *
 * 호출 체인:
 *   conn 종료 시 → [iscsi_tgt_node_cleanup_luns]
 */
int
iscsi_tgt_node_cleanup_luns(struct spdk_iscsi_conn *conn,
			    struct spdk_iscsi_tgt_node *target)
{
	struct spdk_scsi_lun *lun;
	struct spdk_iscsi_task *task;

	for (lun = spdk_scsi_dev_get_first_lun(target->dev); lun != NULL;
	     lun = spdk_scsi_dev_get_next_lun(lun)) {
		/* we create a fake management task per LUN to cleanup */
		task = iscsi_task_get(conn, NULL, iscsi_task_mgmt_cpl);
		/* [한국어] 정리용 fake task — 응답이 conn으로 돌아오면 mgmt completion 호출. */
		if (!task) {
			SPDK_ERRLOG("Unable to acquire task\n");
			return -1;
		}

		task->scsi.target_port = conn->target_port;
		task->scsi.initiator_port = conn->initiator_port;
		task->scsi.lun = lun;

		iscsi_op_abort_task_set(task, SPDK_SCSI_TASK_FUNC_TARGET_RESET);
		/* [한국어] 이 LUN의 모든 outstanding task abort 요청 (TMF Target Reset). */
	}

	return 0;
}

/*
 * [한국어]
 * iscsi_tgt_node_delete_map - PG 또는 IG 삭제 시 모든 타깃에서 관련 매핑 제거 (cascade).
 *
 * portal_group != NULL이면 PG 매핑 제거, initiator_group != NULL이면 IG 매핑 제거.
 */
void
iscsi_tgt_node_delete_map(struct spdk_iscsi_portal_grp *portal_group,
			  struct spdk_iscsi_init_grp *initiator_group)
{
	struct spdk_iscsi_tgt_node *target;

	pthread_mutex_lock(&g_iscsi.mutex);
	TAILQ_FOREACH(target, &g_iscsi.target_head, tailq) {
		if (portal_group) {
			iscsi_tgt_node_delete_pg_map(target, portal_group);
		}
		if (initiator_group) {
			iscsi_tgt_node_delete_ig_maps(target, initiator_group);
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
}

/*
 * [한국어]
 * iscsi_tgt_node_add_lun - 운영 중 LUN 추가 (단, 활성 conn이 없는 동안만).
 *
 * @return: 0 OK, -1 실패.
 *
 * lun_id == -1이면 SCSI device가 자동 할당. 활성 conn이 있으면 거부 (LU 변경 race 회피).
 */
int
iscsi_tgt_node_add_lun(struct spdk_iscsi_tgt_node *target,
		       const char *bdev_name, int lun_id)
{
	struct spdk_scsi_dev *dev;
	int rc;

	if (target->num_active_conns > 0) {
		/* [한국어] 활성 conn 존재 — 변경 거부. */
		SPDK_ERRLOG("Target has active connections (count=%d)\n",
			    target->num_active_conns);
		return -1;
	}

	if (lun_id < -1) {
		/* [한국어] -1만 자동 할당, 그 외 음수는 거부. */
		SPDK_ERRLOG("Specified LUN ID (%d) is negative\n", lun_id);
		return -1;
	}

	dev = target->dev;
	if (dev == NULL) {
		SPDK_ERRLOG("SCSI device is not found\n");
		return -1;
	}

	rc = spdk_scsi_dev_add_lun(dev, bdev_name, lun_id, NULL, NULL);
	/* [한국어] bdev를 새 LUN으로 부착. cb_fn은 NULL — 동기 추가. */
	if (rc != 0) {
		SPDK_ERRLOG("spdk_scsi_dev_add_lun failed\n");
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * iscsi_tgt_node_set_chap_params - 타깃 CHAP 정책 변경 (per-target mutex).
 *
 * 4-튜플 합법성 검사 후 일괄 갱신. 이후 새 로그인부터 적용.
 */
int
iscsi_tgt_node_set_chap_params(struct spdk_iscsi_tgt_node *target,
			       bool disable_chap, bool require_chap,
			       bool mutual_chap, int32_t chap_group)
{
	if (!iscsi_check_chap_params(disable_chap, require_chap,
				     mutual_chap, chap_group)) {
		return -EINVAL;
	}

	pthread_mutex_lock(&target->mutex);
	target->disable_chap = disable_chap;
	target->require_chap = require_chap;
	target->mutual_chap = mutual_chap;
	target->chap_group = chap_group;
	pthread_mutex_unlock(&target->mutex);

	return 0;
}

/*
 * [한국어]
 * iscsi_tgt_node_info_json - 단일 타깃을 JSON 객체로 직렬화.
 *
 * 형식: { name, alias_name?, pg_ig_maps:[{pg_tag,ig_tag}], luns:[{bdev_name,lun_id}],
 *   queue_depth, disable_chap, require_chap, mutual_chap, chap_group,
 *   header_digest, data_digest }
 */
static void
iscsi_tgt_node_info_json(struct spdk_iscsi_tgt_node *target,
			 struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;
	struct spdk_scsi_lun *lun;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", target->name);

	if (target->alias[0] != '\0') {
		spdk_json_write_named_string(w, "alias_name", target->alias);
	}

	spdk_json_write_named_array_begin(w, "pg_ig_maps");
	TAILQ_FOREACH(pg_map, &target->pg_map_head, tailq) {
		TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_int32(w, "pg_tag", pg_map->pg->tag);
			spdk_json_write_named_int32(w, "ig_tag", ig_map->ig->tag);
			spdk_json_write_object_end(w);
		}
	}
	spdk_json_write_array_end(w);

	spdk_json_write_named_array_begin(w, "luns");
	for (lun = spdk_scsi_dev_get_first_lun(target->dev); lun != NULL;
	     lun = spdk_scsi_dev_get_next_lun(lun)) {
		/* [한국어] SCSI device의 LUN 순회. */
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "bdev_name", spdk_scsi_lun_get_bdev_name(lun));
		spdk_json_write_named_int32(w, "lun_id", spdk_scsi_lun_get_id(lun));
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_named_int32(w, "queue_depth", target->queue_depth);

	spdk_json_write_named_bool(w, "disable_chap", target->disable_chap);
	spdk_json_write_named_bool(w, "require_chap", target->require_chap);
	spdk_json_write_named_bool(w, "mutual_chap", target->mutual_chap);
	spdk_json_write_named_int32(w, "chap_group", target->chap_group);

	spdk_json_write_named_bool(w, "header_digest", target->header_digest);
	spdk_json_write_named_bool(w, "data_digest", target->data_digest);

	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * iscsi_tgt_node_histogram_config_json - histogram 활성화 RPC를 method/params 형태로 직렬화.
 *
 * histogram이 활성된 타깃만 출력 (replay 시 재활성화).
 */
static void
iscsi_tgt_node_histogram_config_json(struct spdk_iscsi_tgt_node *target,
				     struct spdk_json_write_ctx *w)
{
	if (!target->histogram) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "iscsi_enable_histogram");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", target->name);

	spdk_json_write_named_bool(w, "enable", true);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * iscsi_tgt_node_config_json - 타깃을 iscsi_create_target_node method/params 형태로 직렬화.
 *
 * histogram이 활성되어 있으면 추가로 iscsi_enable_histogram 호출도 출력.
 */
static void
iscsi_tgt_node_config_json(struct spdk_iscsi_tgt_node *target,
			   struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "iscsi_create_target_node");

	spdk_json_write_name(w, "params");
	iscsi_tgt_node_info_json(target, w);

	spdk_json_write_object_end(w);

	iscsi_tgt_node_histogram_config_json(target, w);
}

/*
 * [한국어]
 * iscsi_tgt_nodes_info_json - 모든 타깃 정보 JSON 출력.
 */
void
iscsi_tgt_nodes_info_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_tgt_node *target;

	TAILQ_FOREACH(target, &g_iscsi.target_head, tailq) {
		iscsi_tgt_node_info_json(target, w);
	}
}

/*
 * [한국어]
 * iscsi_tgt_nodes_config_json - 모든 타깃 재생성 RPC 형태로 출력 (save_config).
 */
void
iscsi_tgt_nodes_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_tgt_node *target;

	TAILQ_FOREACH(target, &g_iscsi.target_head, tailq) {
		iscsi_tgt_node_config_json(target, w);
	}
}

/*
 * [한국어]
 * iscsi_tgt_node_enable_histogram - 지연 분포 측정 토글.
 *
 * @enable: true면 histogram 할당, false면 free.
 *
 * histogram은 SPDK util — 로그 단위 buckets에 task 완료 시 latency 누적.
 */
int
iscsi_tgt_node_enable_histogram(struct spdk_iscsi_tgt_node *target, bool enable)
{
	if (enable) {
		if (!target->histogram) {
			target->histogram = spdk_histogram_data_alloc();
			/* [한국어] 새로 할당. */
			if (target->histogram == NULL) {
				SPDK_ERRLOG("could not allocate histogram\n");
				return -ENOMEM;
			}
		}
	} else {
		if (target->histogram) {
			/* [한국어] 비활성화 — free + NULL. */
			spdk_histogram_data_free(target->histogram);
			target->histogram = NULL;
		}
	}

	return 0;
}
