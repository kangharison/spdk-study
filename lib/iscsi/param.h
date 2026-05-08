/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI Login/Negotiation Parameter 헤더 (param.h)
 *
 * === 파일의 역할 ===
 * iSCSI 로그인 단계와 텍스트 네고시에이션에서 주고받는 key=value 파라미터를 표현·관리하는
 * 자료구조 `iscsi_param`과 그 조작 API를 선언한다. RFC 3720 §12 ("Login and Full Feature
 * Phase Negotiation"), §5.3에서 정의된 Boolean/Numerical/List 키를 본 헤더의 enum
 * `iscsi_param_type`으로 분류하고, 각 키의 협상 규칙(min/max/AND/OR 등)을 적용한다.
 * conn별/세션별 파라미터 테이블을 만드는 init 함수 (iscsi_conn_params_init,
 * iscsi_sess_params_init)와 PDU의 텍스트 데이터에서 파라미터를 파싱하는
 * iscsi_parse_params(), 협상을 수행하는 iscsi_negotiate_params()가 핵심.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (로그인 단계):
 *   iscsi_op_login (lib/iscsi/iscsi.c) →
 *     iscsi_parse_params(conn->params, pdu_data, ...) → 키 추출 →
 *     iscsi_negotiate_params(conn, &response_params, ...) → key별 응답 결정 →
 *     iscsi_copy_param2var(conn) → 협상 결과를 conn/sess의 빠른 접근 변수로 복사.
 * 텍스트 네고시에이션 단계도 동일한 함수들을 재사용.
 * 실행 컨텍스트: conn에 바인딩된 SPDK thread (poll group).
 *
 * === 타 모듈과의 연결 ===
 * - conn.h: spdk_iscsi_conn::params, sess_param_state_negotiated[] 등이 본 자료구조 사용.
 * - iscsi.h: spdk_iscsi_sess::params로 세션 단위 파라미터도 동일 구조체 사용.
 * - iscsi_spec.h: RFC 3720 정의된 키 이름 문자열 사용.
 * - param.c: 본 헤더 선언의 구현, 테이블 (conn_param_table, sess_param_table) 정의.
 * 데이터 흐름: 외부 PDU의 텍스트 데이터 → iscsi_parse_params → 단일 연결 리스트 (params)
 *   → negotiate → 응답 PDU에 직렬화 → conn/sess 변수에 캐시.
 *
 * === 주요 함수/구조체 요약 ===
 * - enum iscsi_param_type: 파라미터 협상 규칙 분류 (LIST/MIN/MAX/AND/OR 등).
 * - struct iscsi_param: 단일 key=value 노드 (문자열로 보관, 단일 연결 리스트).
 * - iscsi_param_add/del/find/set/eq_val/get_val(): 리스트 조작 헬퍼.
 * - iscsi_parse_params(): 텍스트 데이터에서 key=value 추출, params 리스트로 누적.
 * - iscsi_negotiate_params(): conn 파라미터에 따라 응답 파라미터를 생성.
 * - iscsi_copy_param2var(): 협상 결과를 conn/sess의 native 타입 필드로 복사.
 * - iscsi_conn_params_init/iscsi_sess_params_init(): 기본값 테이블로 초기화.
 */

#ifndef SPDK_ISCSI_PARAM_H
#define SPDK_ISCSI_PARAM_H
/* [한국어] 이중 include 방지. */

#include "spdk/stdinc.h"
/* [한국어] 표준 라이브러리 (uint32_t, bool 등). */

struct spdk_iscsi_conn;
/* [한국어] forward 선언 — conn.h를 include하지 않고 포인터만 사용. */

enum iscsi_param_type {
	/* [한국어] iSCSI 키별 협상 규칙 분류. RFC 3720 §12 별로 정해진 협상 방식이
	 * 다르므로 키마다 이 type을 지정해 적절한 규칙을 적용한다. */

	ISPT_INVALID = -1,
	/* [한국어] 잘못된 type — 초기화 안 된 상태 표식. */

	ISPT_NOTSPECIFIED = 0,
	/* [한국어] 협상 규칙 미지정 — 단순 declarative 출력만 하는 경우. */

	ISPT_LIST,
	/* [한국어] 후보 리스트 중 첫 일치를 선택 (예: AuthMethod=CHAP,None → 첫 일치 None). */

	ISPT_NUMERICAL_MIN,
	/* [한국어] 두 측 값의 minimum을 선택 (예: MaxRecvDataSegmentLength). */

	ISPT_NUMERICAL_MAX,
	/* [한국어] 두 측 값의 maximum을 선택. */

	ISPT_NUMERICAL_DECLARATIVE,
	/* [한국어] 한 측의 선언만 — 협상 없이 받아쓰는 숫자 값. */

	ISPT_DECLARATIVE,
	/* [한국어] declarative 키 (협상 없음, 송신자가 일방적으로 알림). */

	ISPT_BOOLEAN_OR,
	/* [한국어] 두 측 값의 OR — 둘 중 하나라도 Yes면 Yes (예: HeaderDigest 설정 등 일부). */

	ISPT_BOOLEAN_AND,
	/* [한국어] 두 측 값의 AND — 둘 다 Yes일 때만 Yes (예: ImmediateData). */
};

struct iscsi_param {
	/* [한국어] 단일 key=value 파라미터 노드. 단일 연결 리스트로 conn/sess의 params 체인 구성. */

	struct iscsi_param *next;
	/* [한국어] 다음 파라미터 노드 포인터 (NULL 종료).
	 * 설정자: iscsi_param_add()에서 헤드에 prepend.
	 * 읽는 자: iscsi_param_find/iterator.
	 * 동기화: conn 단위로 단일 thread에서 접근 — 락 불필요. */

	char *key;
	/* [한국어] iSCSI 표준 키 문자열 (예: "MaxConnections", "HeaderDigest").
	 * 설정자: iscsi_param_add()에서 strdup.
	 * 읽는 자: iscsi_param_find()의 strcmp.
	 * 값 범위: 키별 사양 (보통 ≤ 63 byte).
	 * 동기화: conn 단위 단일 thread. */

	char *val;
	/* [한국어] 값 문자열 (현재 협상된 값 또는 초기 기본값).
	 * 설정자: iscsi_param_set/add에서 strdup, 협상 도중 갱신.
	 * 읽는 자: iscsi_param_get_val/eq_val, copy_param2var.
	 * 값 범위: 키별 사양. Boolean은 "Yes"/"No", Numerical은 10진 문자열.
	 * 동기화: conn 단위 단일 thread. */

	char *list;
	/* [한국어] LIST 타입 키의 후보 값들 (콤마 분리 문자열, 예: "CHAP,None").
	 * 설정자: 초기화 시 사양 기반 strdup, NUMERICAL_MIN/MAX는 "min,max" 형태.
	 * 읽는 자: negotiate 시 후보군 결정에 사용.
	 * 값 범위: 키별 사양. */

	int type;
	/* [한국어] 협상 규칙(enum iscsi_param_type 값).
	 * 설정자: 초기화 시 사양 기반 설정, 변경 안 됨.
	 * 읽는 자: negotiate가 type 분기 처리. */

	int state_index;
	/* [한국어] conn->conn_param_state_negotiated[] / sess_param_state_negotiated[]의 인덱스.
	 * 설정자: 초기화 시 키별 고유 인덱스 부여.
	 * 읽는 자: 같은 키의 중복 협상 검출 시 (RFC 위반 차단).
	 * 값 범위: 0 ≤ idx < MAX_CONNECTION_PARAMS(=14) 또는 MAX_SESSION_PARAMS(=19). */
};

void iscsi_param_free(struct iscsi_param *params);
/* [한국어] params 리스트의 모든 노드와 자식 문자열을 free. */
struct iscsi_param *
iscsi_param_find(struct iscsi_param *params, const char *key);
/* [한국어] 리스트에서 key로 노드 검색. NULL이면 미존재. */
int iscsi_param_del(struct iscsi_param **params, const char *key);
/* [한국어] 리스트에서 key 노드 제거 + free. 0 성공, -ENOENT. */
int iscsi_param_add(struct iscsi_param **params, const char *key,
		    const char *val, const char *list, int type);
/* [한국어] 새 노드 생성하여 헤드에 prepend. 중복 시 기존 항목 갱신. */
int iscsi_param_set(struct iscsi_param *params, const char *key,
		    const char *val);
/* [한국어] 기존 키의 val 갱신 (strdup으로 새로 할당). 미존재 시 -ENOENT. */
int iscsi_param_set_int(struct iscsi_param *params, const char *key, uint32_t val);
/* [한국어] 정수 값을 10진 문자열로 변환 후 set. */
int iscsi_parse_params(struct iscsi_param **params, const uint8_t *data,
		       int len, bool cbit_enabled, char **partial_parameter);
/* [한국어] PDU 텍스트 데이터(key=value\0...)를 파싱해 params 리스트에 누적.
 * cbit_enabled가 true면 Continue bit 적용 — 마지막 부분 파라미터를 *partial_parameter에
 * 보관해 다음 PDU에서 이어 처리. */
char *iscsi_param_get_val(struct iscsi_param *params, const char *key);
/* [한국어] key의 val 문자열 반환 (없으면 NULL). 호출자가 변경 금지. */
int iscsi_param_eq_val(struct iscsi_param *params, const char *key,
		       const char *val);
/* [한국어] key의 val이 주어진 val과 같은지 비교 (1=일치, 0=불일치/미존재). */

int iscsi_negotiate_params(struct spdk_iscsi_conn *conn,
			   struct iscsi_param **params_p, uint8_t *data,
			   int alloc_len, int data_len);
/* [한국어] 입력 params를 conn->params와 협상하여 응답을 data 버퍼에 직렬화.
 * 반환: 응답 크기(data_len에서 시작 후 실제 사용한 바이트). 실패 시 음수. */
int iscsi_copy_param2var(struct spdk_iscsi_conn *conn);
/* [한국어] 협상 완료 후 conn->params와 sess->params의 문자열 값을 conn/sess의 native
 * 정수/bool 필드로 복사 (런타임 빠른 접근용). */

int iscsi_conn_params_init(struct iscsi_param **params);
/* [한국어] 연결 단위 파라미터 (HeaderDigest, DataDigest 등 14개) 기본값 초기화. */
int iscsi_sess_params_init(struct iscsi_param **params);
/* [한국어] 세션 단위 파라미터 (MaxConnections, MaxBurstLength 등 19개) 기본값 초기화. */

#endif /* SPDK_ISCSI_PARAM_H */
/* [한국어] include guard 종료. */
