/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI Target Node 관리 헤더 (tgt_node.h)
 *
 * === 파일의 역할 ===
 * iSCSI 타깃의 가장 큰 논리 단위인 "Target Node"를 정의한다. 한 Target Node는 (1) IQN
 * 이름과 별칭, (2) 다수의 LUN(=spdk_scsi_dev에 묶인 SCSI Logical Units), (3) Portal Group×
 * Initiator Group의 매핑(`pg_map_head`), (4) CHAP/Digest/QueueDepth 정책을 가진다.
 * 클라이언트가 로그인할 때 (TargetName, 도착한 PG, 자신의 IQN/IP)를 본 헤더의 매핑으로
 * 검사하여 어떤 target에 attach할지 결정한다 (RFC 3720 §3.4.2 Target Name).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (구성):
 *   RPC iscsi_create_target_node → iscsi_tgt_node_construct() →
 *     iscsi_target_node_add_pg_ig_maps() (PG×IG 매핑 등록) →
 *     iscsi_tgt_node_add_lun() (LUN 추가, bdev 연결) → 전역 g_iscsi.target_head 등록.
 * 호출 체인 (런타임):
 *   conn 로그인 단계 → iscsi_find_tgt_node(name) → iscsi_tgt_node_access() (PG/IG/IP/IQN 검증)
 *     → conn->target = target → SCSI dev attach.
 *   redirect: iscsi_tgt_node_redirect()로 특정 PG의 매핑을 redirect_host/port로 응답.
 *   삭제: iscsi_shutdown_tgt_node_by_name() → iscsi_conns_request_logout → destruct_poller.
 * 실행 컨텍스트: target.mutex로 매핑·LUN 변경을 직렬화. 삭제는 비동기 (active conn 종료 대기).
 *
 * === 타 모듈과의 연결 ===
 * - iscsi.h: g_iscsi.target_head, MAX_TARGET_NAME 등 상수.
 * - init_grp.h, portal_grp.h: pg_map의 (PG, IG) 매핑 구성요소.
 * - conn.h: 로그인 단계에서 본 헤더 함수 호출, conn->target 설정.
 * - spdk/scsi.h: spdk_scsi_dev로 LUN 풀 관리.
 * - histogram_data.h: target별 latency histogram 옵션.
 * 데이터 흐름: RPC 입력 → target_node 생성 → bdev → spdk_scsi_dev/LUN 추가 → 클라이언트
 * 로그인 시 매핑 검사 → SCSI 명령은 spdk_scsi_dev_queue_task로 dev에 디스패치.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_iscsi_ig_map: pg_map 안에 매달리는 IG 한 개.
 * - struct spdk_iscsi_pg_map: 한 target_node가 PG와 매핑되는 단위 (1 PG = N IG 허용).
 * - struct spdk_iscsi_tgt_node: target node 본체. dev/PG×IG 매핑/CHAP 정책/destruct 상태.
 * - iscsi_tgt_node_construct(): RPC 진입, pg/ig map + LUN 일괄 생성.
 * - iscsi_tgt_node_access(): conn의 portal/initiator가 매핑 통과하는지 검사.
 * - iscsi_tgt_node_redirect(): 특정 PG에서 들어오는 로그인을 다른 portal로 redirect.
 * - iscsi_send_tgts(): SendTargets 응답 작성 (Discovery 세션).
 * - iscsi_shutdown_tgt_nodes(): 모든 target 비동기 종료.
 */

#ifndef SPDK_ISCSI_TGT_NODE_H_
#define SPDK_ISCSI_TGT_NODE_H_
/* [한국어] include guard. */

#include "spdk/stdinc.h"
/* [한국어] 표준 라이브러리. */

#include "iscsi/iscsi.h"
/* [한국어] 도메인 상수, g_iscsi 전역. */
#include "spdk/histogram_data.h"
/* [한국어] target별 latency histogram 데이터 구조. */

struct spdk_iscsi_conn;
/* [한국어] forward 선언 — 순환 의존 회피. */
struct spdk_iscsi_init_grp;
/* [한국어] forward — pg_map의 IG 매핑. */
struct spdk_iscsi_portal_grp;
/* [한국어] forward — pg_map의 PG. */
struct spdk_iscsi_portal;
/* [한국어] forward — redirect 처리 시 사용. */
struct spdk_json_write_ctx;
/* [한국어] forward — JSON 직렬화. */

#define MAX_TARGET_MAP			256
/* [한국어] 한 target_node의 (PG, IG) 매핑 최대 수. */
#define SPDK_TN_TAG_MAX			0x0000ffff
/* [한국어] target_node 식별자(num) 최대값 (16-bit). */

typedef void (*iscsi_tgt_node_destruct_cb)(void *cb_arg, int rc);
/* [한국어] target_node 비동기 destruct 완료 콜백. rc=0 성공/음수=실패. */

struct spdk_iscsi_ig_map {
	/* [한국어] pg_map 안에 매달리는 IG 한 개에 대한 매핑 노드. */

	struct spdk_iscsi_init_grp *ig;
	/* [한국어] 매핑된 IG 포인터 (ref++된 상태).
	 * 설정자: iscsi_target_node_add_pg_ig_maps.
	 * 읽는 자: iscsi_tgt_node_access의 ACL 검사. */

	TAILQ_ENTRY(spdk_iscsi_ig_map) tailq;
	/* [한국어] pg_map.ig_map_head 링크. */
};

struct spdk_iscsi_pg_map {
	/* [한국어] 한 target_node와 한 PG 사이의 매핑. 1 PG = N IG 매핑 가능. */

	struct spdk_iscsi_portal_grp *pg;
	/* [한국어] 매핑된 PG 포인터 (ref++된 상태).
	 * 설정자: add_pg_ig_maps.
	 * 읽는 자: 로그인 시 conn->portal->group과 비교. */

	int num_ig_maps;
	/* [한국어] ig_map_head에 매달린 IG 매핑 수. */
	TAILQ_HEAD(, spdk_iscsi_ig_map) ig_map_head;
	/* [한국어] 이 PG에 허용된 IG 매핑들의 헤드. */

	char redirect_host[MAX_PORTAL_ADDR + 1];
	/* [한국어] non-discovery 로그인 redirect 대상 host (비어있으면 redirect 비활성).
	 * RFC 3720 §5.3.3 — public PG의 로그인을 private PG로 우회. */
	char redirect_port[MAX_PORTAL_PORT + 1];
	/* [한국어] redirect 대상 port 문자열. */

	TAILQ_ENTRY(spdk_iscsi_pg_map) tailq ;
	/* [한국어] target_node.pg_map_head 링크. */
};

struct spdk_iscsi_tgt_node {
	/* [한국어] target_node 본체. */

	int num;
	/* [한국어] target_node 식별 번호. iqn 자동 생성과 표시용. */
	char name[MAX_TARGET_NAME + 1];
	/* [한국어] TargetName(IQN). 로그인 매칭 키. */
	char alias[MAX_TARGET_NAME + 1];
	/* [한국어] TargetAlias (사람 읽는 라벨, RFC 3720 선택사항). */

	pthread_mutex_t mutex;
	/* [한국어] target 상태 변경(매핑·LUN·destruct) 직렬화 락.
	 * 설정자/읽는 자: 모든 mutating 함수가 잡음.
	 * 동기화: pthread_mutex. */

	bool disable_chap;
	bool require_chap;
	bool mutual_chap;
	int chap_group;
	/* [한국어] target 단위 CHAP 정책. PG의 정책과 결합되어 결정. */

	bool header_digest;
	/* [한국어] target에서 강제할 HeaderDigest 정책 (true면 CRC32C 강제). */
	bool data_digest;
	/* [한국어] DataDigest 정책. */
	int queue_depth;
	/* [한국어] target 단위 큐 깊이. SCSI 레이어 제한값. */

	struct spdk_scsi_dev *dev;
	/* [한국어] target_node의 SCSI dev (LUN 컨테이너). */
	/**
	 * Counts number of active iSCSI connections associated with this
	 *  target node.
	 */
	uint32_t num_active_conns;
	/* [한국어] 현재 attach된 conn 수. destruct가 0 도달 대기.
	 * 설정자: conn 로그인 후 ++, destruct 시 --.
	 * 동기화: target->mutex. */
	struct spdk_iscsi_poll_group *pg;
	/* [한국어] target 전용 poll_group (옵션 — 일부 구성에서만 사용). */

	int num_pg_maps;
	/* [한국어] pg_map_head 길이 캐시. */
	TAILQ_HEAD(, spdk_iscsi_pg_map) pg_map_head;
	/* [한국어] PG×IG 매핑 리스트. */
	TAILQ_ENTRY(spdk_iscsi_tgt_node) tailq;
	/* [한국어] g_iscsi.target_head 링크. */

	bool destructed;
	/* [한국어] destruct 시작 플래그 (재진입 방지). */
	struct spdk_poller *destruct_poller;
	/* [한국어] destruct 완료를 기다리는 poller. num_active_conns==0이면 free 후 stop. */
	iscsi_tgt_node_destruct_cb destruct_cb_fn;
	/* [한국어] destruct 완료 콜백 (RPC 응답에 사용). */
	void *destruct_cb_arg;
	/* [한국어] 콜백 ctx. */

	struct spdk_histogram_data *histogram;
	/* [한국어] (옵션) target latency histogram. enable RPC로 켜짐. */
};

void iscsi_shutdown_tgt_nodes(void);
/* [한국어] 모든 target_node에 logout 요청 + destruct 시작. SPDK fini 경로. */
void iscsi_shutdown_tgt_node_by_name(const char *target_name,
				     iscsi_tgt_node_destruct_cb cb_fn, void *cb_arg);
/* [한국어] 특정 이름의 target_node 비동기 종료. 완료 시 cb_fn(cb_arg, rc). */
bool iscsi_tgt_node_is_destructed(struct spdk_iscsi_tgt_node *target);
/* [한국어] destruct 진행 중인지 검사 (연결 거부 결정). */
int iscsi_send_tgts(struct spdk_iscsi_conn *conn, const char *iiqn,
		    const char *tiqn, uint8_t *data, int alloc_len, int data_len);
/* [한국어] SendTargets PDU 응답 작성. iiqn(요청 시작자 IQN), tiqn(필터)으로 매칭되는
 * target들의 TargetAddress=...,PG_TAG 텍스트를 data에 직렬화. 반환=총 출력 바이트. */

/*
 * bdev_name_list and lun_id_list are equal sized arrays of size num_luns.
 * bdev_name_list refers to the names of the bdevs that will be used for the LUNs on the
 *  new target node.
 * lun_id_list refers to the LUN IDs that will be used for the LUNs on the target node.
 */
struct spdk_iscsi_tgt_node *iscsi_tgt_node_construct(int target_index,
		const char *name, const char *alias,
		int *pg_tag_list, int *ig_tag_list, uint16_t num_maps,
		const char *bdev_name_list[], int *lun_id_list, int num_luns,
		int queue_depth,
		bool disable_chap, bool require_chap, bool mutual_chap, int chap_group,
		bool header_digest, bool data_digest);
/* [한국어] RPC iscsi_create_target_node 진입점. (PG, IG) 매핑·LUN·정책을 한 번에 채워 등록. */

bool iscsi_check_chap_params(bool disable, bool require, bool mutual, int group);
/* [한국어] (disable, require, mutual, group) 조합의 정합성 검증. */

int iscsi_target_node_add_pg_ig_maps(struct spdk_iscsi_tgt_node *target,
				     int *pg_tag_list, int *ig_tag_list,
				     uint16_t num_maps);
/* [한국어] (PG tag, IG tag) 페어 배열을 target에 일괄 추가. ref++. 실패 시 롤백. */
int iscsi_target_node_remove_pg_ig_maps(struct spdk_iscsi_tgt_node *target,
					int *pg_tag_list, int *ig_tag_list,
					uint16_t num_maps);
/* [한국어] PG×IG 매핑 일괄 제거. ref--. */
int iscsi_tgt_node_redirect(struct spdk_iscsi_tgt_node *target, int pg_tag,
			    const char *host, const char *port);
/* [한국어] 특정 PG에서 들어오는 로그인을 (host, port)로 redirect 설정. */
bool iscsi_tgt_node_is_redirected(struct spdk_iscsi_conn *conn,
				  struct spdk_iscsi_tgt_node *target,
				  char *buf, int buf_len);
/* [한국어] conn의 portal이 redirect 대상이면 true 반환 + buf에 redirect 정보 작성. */

bool iscsi_tgt_node_access(struct spdk_iscsi_conn *conn,
			   struct spdk_iscsi_tgt_node *target, const char *iqn,
			   const char *addr);
/* [한국어] conn의 portal이 target의 어느 PG에 매핑되어 있고, iqn/addr이 해당 IG의 ACL을
 * 통과하는지 검사. true=허용, false=거부. 로그인 핵심 ACL. */
struct spdk_iscsi_tgt_node *iscsi_find_tgt_node(const char *target_name);
/* [한국어] g_iscsi.target_head에서 IQN으로 검색. 호출자 mutex 책임. */
int iscsi_tgt_node_cleanup_luns(struct spdk_iscsi_conn *conn,
				struct spdk_iscsi_tgt_node *target);
/* [한국어] conn이 attach 해제될 때 LUN reservation 등을 정리. */
void iscsi_tgt_node_delete_map(struct spdk_iscsi_portal_grp *portal_group,
			       struct spdk_iscsi_init_grp *initiator_group);
/* [한국어] 모든 target에서 (PG, IG) 매핑을 일괄 제거. PG/IG 삭제 RPC가 호출. */
int iscsi_tgt_node_add_lun(struct spdk_iscsi_tgt_node *target,
			   const char *bdev_name, int lun_id);
/* [한국어] target에 새 LUN 추가 (bdev_name으로 spdk_bdev 찾아 spdk_scsi_dev에 부착). */
int iscsi_tgt_node_set_chap_params(struct spdk_iscsi_tgt_node *target,
				   bool disable_chap, bool require_chap,
				   bool mutual_chap, int32_t chap_group);
/* [한국어] target의 CHAP 정책 변경. 정합성 검증 후 일괄 대입. */
void iscsi_tgt_nodes_info_json(struct spdk_json_write_ctx *w);
/* [한국어] 모든 target의 현재 상태를 JSON으로 출력. */
void iscsi_tgt_nodes_config_json(struct spdk_json_write_ctx *w);
/* [한국어] 모든 target을 재생성 RPC method/params 형태로 출력 (save_config). */

int iscsi_tgt_node_enable_histogram(struct spdk_iscsi_tgt_node *target, bool enable);
/* [한국어] target의 latency histogram 측정 활성/비활성. 리소스 할당 후 측정 시작. */
#endif /* SPDK_ISCSI_TGT_NODE_H_ */
/* [한국어] include guard 종료. */
