/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI Initiator Group(이니시에이터 그룹) 관리 헤더 (init_grp.h)
 *
 * === 파일의 역할 ===
 * iSCSI 타깃이 어떤 호스트(이니시에이터)에게 LUN 접근을 허용할지를 그룹 단위로 관리하는
 * "Initiator Group(IG)" 자료구조와 그 조작 API를 선언한다. 각 IG는 (1) 허용 IQN 이름의
 * 리스트와 (2) 허용 IP/넷마스크 리스트를 갖는다. RPC `iscsi_create_initiator_group`이
 * 들어오면 본 헤더의 함수들이 호출되어 전역 IG 테이블(g_iscsi.ig_head)에 등록된다.
 * 로그인 단계에서 tgt_node가 (PG, IG) 매핑을 통해 접속한 conn의 이니시에이터 IQN/주소가
 * 어떤 IG에 속해 있는지 검사하여 접근 허용/거부를 판단한다. 즉 RFC 3720 §12의 ACL을
 * 구현하는 핵심 자료구조이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK iSCSI target 컨트롤 플레인의 일부로, 데이터 플레인(I/O 처리)이 아닌
 * "구성(configuration)" 레이어에 속한다. 호출 체인:
 *   RPC 핸들러 (lib/iscsi/iscsi_rpc.c) →
 *     iscsi_init_grp_create_from_initiator_list() →
 *       iscsi_init_grp_register() (전역 g_iscsi.ig_head 등록).
 *   로그인/Discovery 시: tgt_node.c::iscsi_tgt_node_access() →
 *     IG의 initiator_head/netmask_head를 순회하며 conn의 IQN/주소 매칭.
 * 실행 컨텍스트는 호스트 유저스페이스(SPDK 메인 thread)이며, IG 테이블 변경은
 * g_iscsi.mutex(pthread_mutex)로 직렬화된다 (RPC와 로그인 경로 모두 동일 락 사용).
 *
 * === 타 모듈과의 연결 ===
 * - iscsi.h: 전역 g_iscsi(`spdk_iscsi_globals`) 정의를 통해 ig_head/mutex 공유.
 * - tgt_node.h: tgt_node가 (PG, IG) 페어로 매핑되며 IG가 LUN 접근 ACL을 결정.
 * - conn.h: 들어온 TCP 연결이 어떤 IG에 속하는지 판단하기 위해 conn의
 *           initiator_name과 initiator_addr를 IG의 initiator_head/netmask_head와 비교.
 * - portal_grp.h: PG와 IG가 짝을 이뤄 tgt_node의 pg_map에 등록됨.
 * - JSON RPC: iscsi_init_grps_info_json/config_json이 RPC 응답으로 IG 상태를 출력.
 * 데이터 흐름: RPC 입력 (이름·넷마스크 문자열 배열) → IG 생성/등록 → 로그인 시 매칭 검사
 * → 인증/거부 판단.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_iscsi_initiator_name: 단일 IQN(또는 "ANY") 노드. TAILQ로 IG에 매달림.
 * - struct spdk_iscsi_initiator_netmask: 단일 IP/넷마스크 노드. 주소 매칭에 사용.
 * - struct spdk_iscsi_init_grp: tag(IG ID)로 식별되는 그룹. 이름 리스트 + 넷마스크 리스트.
 * - iscsi_init_grp_create_from_initiator_list(): RPC 진입점, IG 신규 생성+등록까지 한 번에.
 * - iscsi_init_grp_add/delete_initiators_from_initiator_list(): 기존 IG에 항목 추가/제거.
 * - iscsi_init_grp_register/unregister(): 전역 g_iscsi.ig_head에 등록/해제 (락 보호).
 * - iscsi_init_grp_find_by_tag(): tag로 IG 검색 (락은 호출자 책임).
 * - iscsi_init_grp_destroy(): IG와 그 자식 노드들 전부 free.
 * - iscsi_init_grps_destroy(): 모든 IG 일괄 정리(셧다운 시).
 */

#ifndef SPDK_INIT_GRP_H
#define SPDK_INIT_GRP_H
/* [한국어] 이중 include 방지 가드. SPDK 헤더 컨벤션. */

#include "iscsi/iscsi.h"
/* [한국어] iscsi.h에서 MAX_INITIATOR_NAME, MAX_INITIATOR_ADDR, g_iscsi 등을 가져온다. */
#include "iscsi/conn.h"
/* [한국어] conn.h는 직접 사용되지 않지만 IG 매칭을 호출하는 conn 구조와의 결합을 명시. */

struct spdk_iscsi_initiator_name {
	/* [한국어] IG에 속하는 허용 IQN(또는 "ANY") 한 개를 표현하는 TAILQ 노드.
	 * 한 IG는 이 노드를 1..MAX_INITIATOR(=256)개 가질 수 있다 (iscsi.h MAX_INITIATOR). */

	char name[MAX_INITIATOR_NAME + 1];
	/* [한국어] iSCSI Qualified Name 문자열 버퍼.
	 * 설정자: iscsi_init_grp_add_initiator()에서 RPC 입력 문자열을 memcpy로 복사.
	 * 읽는 자: iscsi_tgt_node_access()/SendTargets 처리에서 conn->initiator_name과 strcmp.
	 * 값 범위: 길이 ≤ MAX_INITIATOR_NAME(=223, RFC 3720 iqn 최대길이) + NUL. "ALL"은
	 *          입력 시 자동으로 "ANY"로 변환된다(레거시 호환).
	 * 동기화: g_iscsi.mutex 보호 하에 IG 단위로 추가/삭제, 매칭은 같은 락 보유 시 안전. */

	TAILQ_ENTRY(spdk_iscsi_initiator_name) tailq;
	/* [한국어] 부모 IG의 initiator_head TAILQ에 연결되는 링크 필드.
	 * 설정자: TAILQ_INSERT_TAIL(...) 호출 시 자동.
	 * 읽는 자: TAILQ_FOREACH로 그룹 내 이름 순회.
	 * 동기화: g_iscsi.mutex 하에서만 수정. */
};

struct spdk_iscsi_initiator_netmask {
	/* [한국어] IG에 속하는 허용 IP/넷마스크(또는 "ANY") 한 개를 표현하는 TAILQ 노드.
	 * IQN 매칭에 더해 소스 IP 기반 ACL을 위해 사용된다. */

	char mask[MAX_INITIATOR_ADDR + 1];
	/* [한국어] 넷마스크 문자열(예: "10.0.0.0/24" 또는 "192.168.1.5" 또는 "ANY").
	 * 설정자: iscsi_init_grp_add_netmask()에서 RPC 입력 문자열을 memcpy.
	 * 읽는 자: tgt_node 매칭 시 conn의 initiator_addr와 비교 (마스크 파싱 포함).
	 * 값 범위: 길이 ≤ MAX_INITIATOR_ADDR(=64) + NUL. "ALL"→"ANY" 자동 변환.
	 * 동기화: g_iscsi.mutex로 보호. */

	TAILQ_ENTRY(spdk_iscsi_initiator_netmask) tailq;
	/* [한국어] 부모 IG의 netmask_head TAILQ에 연결되는 링크.
	 * 설정자/읽는 자/동기화: 위 spdk_iscsi_initiator_name::tailq와 동일. */
};

struct spdk_iscsi_init_grp {
	/* [한국어] 하나의 Initiator Group 전체를 표현하는 구조체.
	 * tag로 식별되며 (PG, IG) 매핑의 IG 슬롯에 사용됨. */

	int ninitiators;
	/* [한국어] initiator_head에 연결된 이름 노드 수 (캐시).
	 * 설정자: add_initiator()에서 ++, delete_initiator()에서 --, delete_all에서 0까지 감소.
	 * 읽는 자: MAX_INITIATOR 경계 검사 시.
	 * 값 범위: 0 ≤ ninitiators ≤ MAX_INITIATOR(=256).
	 * 동기화: g_iscsi.mutex. */

	TAILQ_HEAD(, spdk_iscsi_initiator_name) initiator_head;
	/* [한국어] 허용 IQN 노드들의 TAILQ 헤드.
	 * 설정자: iscsi_init_grp_create()에서 TAILQ_INIT.
	 * 읽는 자: tgt_node가 conn 매칭 시 순회.
	 * 동기화: g_iscsi.mutex. */

	int nnetmasks;
	/* [한국어] netmask_head에 연결된 넷마스크 노드 수 (캐시).
	 * 설정자: add/delete_netmask()에서 ±, delete_all에서 0.
	 * 읽는 자: MAX_NETMASK 경계 검사.
	 * 값 범위: 0 ≤ nnetmasks ≤ MAX_NETMASK(=256).
	 * 동기화: g_iscsi.mutex. */

	TAILQ_HEAD(, spdk_iscsi_initiator_netmask) netmask_head;
	/* [한국어] 허용 IP/넷마스크 노드들의 TAILQ 헤드.
	 * 설정자/읽는 자/동기화: initiator_head와 동일. */

	int ref;
	/* [한국어] 이 IG를 참조하는 tgt_node pg_map 수 (참조 카운트).
	 * 설정자: tgt_node가 IG를 매핑할 때 ++, 해제 시 --.
	 * 읽는 자: IG 삭제 RPC가 ref>0이면 거부할 때.
	 * 동기화: g_iscsi.mutex. */

	int tag;
	/* [한국어] 이 IG의 식별자 (사용자 지정 정수, 양의 값).
	 * 설정자: iscsi_init_grp_create(tag) 시 1회 설정 후 불변.
	 * 읽는 자: iscsi_init_grp_find_by_tag(), tgt_node pg_ig 매핑.
	 * 값 범위: 양의 정수 (RPC에서 검증).
	 * 동기화: 생성 후 read-only이므로 락 불필요. */

	TAILQ_ENTRY(spdk_iscsi_init_grp)	tailq;
	/* [한국어] 전역 g_iscsi.ig_head TAILQ에 연결되는 링크.
	 * 설정자: iscsi_init_grp_register()에서 TAILQ_INSERT_TAIL.
	 * 읽는 자: iscsi_init_grp_find_by_tag()의 TAILQ_FOREACH.
	 * 동기화: g_iscsi.mutex. */
};

/* SPDK iSCSI Initiator Group management API */
/* [한국어] === 아래는 IG 관리 외부 API. RPC 핸들러와 셧다운 경로에서 호출. === */

int iscsi_init_grp_create_from_initiator_list(int tag,
		int num_initiator_names, char **initiator_names,
		int num_initiator_masks, char **initiator_masks);
/* [한국어] 신규 IG를 생성하고 이름·넷마스크 리스트를 한 번에 채워 등록한다.
 * RPC `iscsi_create_initiator_group` 처리에서 호출. 실패 시 부분 생성 롤백 후 -EINVAL. */

int iscsi_init_grp_add_initiators_from_initiator_list(int tag,
		int num_initiator_names, char **initiator_names,
		int num_initiator_masks, char **initiator_masks);
/* [한국어] 이미 등록된 IG에 이니시에이터·넷마스크를 추가. RPC `iscsi_initiator_group_add_initiators`. */

int iscsi_init_grp_delete_initiators_from_initiator_list(int tag,
		int num_initiator_names, char **initiator_names,
		int num_initiator_masks, char **initiator_masks);
/* [한국어] 이미 등록된 IG에서 이름·넷마스크 일부를 제거. 실패 시 add 롤백 시도. */

int iscsi_init_grp_register(struct spdk_iscsi_init_grp *ig);
/* [한국어] IG를 g_iscsi.ig_head에 등록 (tag 중복 검사 포함). 락 내부에서 획득. */

struct spdk_iscsi_init_grp *iscsi_init_grp_unregister(int tag);
/* [한국어] tag로 IG를 g_iscsi.ig_head에서 떼어낸다. 반환 IG는 호출자가 destroy 책임. */

struct spdk_iscsi_init_grp *iscsi_init_grp_find_by_tag(int tag);
/* [한국어] g_iscsi.ig_head에서 tag로 IG 검색. 호출자가 g_iscsi.mutex를 잡고 호출해야 함. */

void iscsi_init_grp_destroy(struct spdk_iscsi_init_grp *ig);
/* [한국어] IG와 그 자식 이름·넷마스크 노드 전체를 free. ig_head에서 제거된 후에만 호출. */

int iscsi_parse_init_grps(void);
/* [한국어] (legacy) 설정 파일 기반 IG 일괄 파싱. 현재 RPC 기반 사용으로 호출 빈도 낮음. */

void iscsi_init_grps_destroy(void);
/* [한국어] 전체 IG를 일괄 destroy. SPDK 셧다운(spdk_iscsi_fini) 경로에서 호출. */

void iscsi_init_grps_info_json(struct spdk_json_write_ctx *w);
/* [한국어] JSON-RPC 응답으로 모든 IG의 현재 상태를 출력 (status query). */

void iscsi_init_grps_config_json(struct spdk_json_write_ctx *w);
/* [한국어] save_config RPC 호출 시 IG 재생성 RPC method/params를 출력 (replay 가능 형태). */

#endif /* SPDK_INIT_GRP_H */
/* [한국어] include guard 종료. */
