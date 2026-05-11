/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI 텍스트 협상 파라미터 처리 (param.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 iSCSI 로그인/텍스트 단계에서 교환되는 "KEY=VAL" 텍스트 파라미터를 (1) 파싱하고
 * (2) 단방향(연결 단위 또는 세션 단위) 리스트로 보관하며 (3) 타깃 측 정책과 협상하여
 * 적절한 응답 문자열을 생성하고 (4) 협상 완료된 값을 conn/sess 구조체의 캐시 필드에 복사
 * 하는 일을 담당한다. RFC 3720 §5(Text Mode Negotiation), §11(Negotiation Keys), RFC 7143
 * §6(Text Format)을 SPDK가 구현한 부분이다. iSCSI 로그인 phase의 "두뇌"에 해당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (입력 방향 — initiator → target):
 *   iscsi_op_login_rsp_handle_csg_bit() → iscsi_parse_params(data="K=V\0K=V\0...") →
 *     iscsi_parse_param() → iscsi_param_add() (linked list append).
 * 호출 체인 (협상 방향):
 *   iscsi_op_login() → iscsi_negotiate_params(conn, params, data, alloc_len, data_len) →
 *     iscsi_negotiate_param_init() → iscsi_negotiate_param_all() →
 *     iscsi_negotiate_param_list/numerical/boolean() →
 *     iscsi_construct_data_from_param() → iscsi_special_param_construction() →
 *     응답 PDU의 data 영역에 K=V\0 시퀀스 누적.
 * 호출 체인 (캐시):
 *   FullFeature 진입 직전 → iscsi_copy_param2var() → conn->{header_digest, MaxRecvDataSegmentLength,
 *     ...}, conn->sess->{MaxConnections, FirstBurstLength, ImmediateData, ...} 저장.
 * 실행 컨텍스트: 로그인 phase 동안 해당 conn을 소유한 SPDK thread (poll group). 락 불요
 * (conn별 격리).
 *
 * === 타 모듈과의 연결 ===
 * - iscsi.h: ISCSI_TEXT_MAX_*, SPDK_ISCSI_*_BURST_LENGTH 상수, ISPT_* 타입 enum.
 * - param.h: struct iscsi_param 자료구조 및 외부 API 선언.
 * - conn.h: struct spdk_iscsi_conn (params, sess->params, *_state_negotiated, 캐시 필드들).
 * - iscsi.c: 로그인 PDU 처리 흐름에서 본 파일의 함수를 호출.
 * 데이터 흐름: PDU data buf(uint8_t*) → struct iscsi_param 단방향 리스트 → 협상 결과
 *   data 응답 buf로 K=V\0 직렬화 → 종료 후 sess/conn 캐시 변수.
 *
 * === 주요 함수/구조체 요약 ===
 * - iscsi_param_add/del/find/set/get_val: 파라미터 리스트 CRUD 및 조회.
 * - iscsi_parse_param/iscsi_parse_params: PDU의 K=V 시퀀스를 리스트로 파싱.
 * - iscsi_conn_params_init/iscsi_sess_params_init: 기본 정책 테이블(conn_param_table /
 *   sess_param_table)로 초기 파라미터 리스트 구성.
 * - iscsi_negotiate_param_list/numerical/boolean: 키 타입별 협상 알고리즘.
 * - iscsi_negotiate_params: 전체 협상 루프 진입점 (응답 데이터 생성).
 * - iscsi_special_param_construction: 타깃 측이 declarative하게 추가로 송신할 키 처리
 *   (MaxRecvDataSegmentLength, FirstBurstLength).
 * - iscsi_copy_param2var: 협상 종료 후 핫패스 사용을 위한 정수/bool 캐시 변환.
 * - struct iscsi_param: { key, val, list, type, state_index, next } 단방향 리스트 노드.
 * - struct iscsi_param_table: 기본 정책 테이블 엔트리 { key, val, list, type }.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 라이브러리 일괄 포함. malloc/strcasecmp/snprintf/strtol 사용. */

#include "spdk/string.h"
/* [한국어] xstrdup, spdk_strsepq, spdk_sprintf_alloc 등 SPDK 문자열 헬퍼. */
#include "iscsi/iscsi.h"
/* [한국어] iSCSI 도메인 상수 — ISCSI_TEXT_MAX_KEY_LEN, ISCSI_TEXT_MAX_VAL_LEN,
 * ISCSI_TEXT_MAX_SIMPLE_VAL_LEN, SPDK_ISCSI_*_BURST_LENGTH, ISPT_* 등. */
#include "iscsi/param.h"
/* [한국어] 본 파일에서 구현하는 외부 API 선언과 struct iscsi_param 정의. */
#include "iscsi/conn.h"
/* [한국어] struct spdk_iscsi_conn — params/sess 포인터, state_negotiated 비트맵 사용. */

#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG/DEBUGLOG/WARNLOG 매크로. */

#define MAX_TMPBUF 1024
/* [한국어] 정수→문자열 등 임시 버퍼 크기. snprintf("%d", val) 같은 단순 변환에 충분. */

/* whose value may be bigger than 255 */
/*
 * [한국어]
 * non_simple_value_params - "단순값(255B 상한)"이 아닌 키 목록 (NULL 종료).
 *
 * RFC 3720 §5.1: simple-value의 길이는 기본 255바이트로 제한되지만 CHAP_C(challenge)나
 * CHAP_R(response)는 16진/base64 인코딩된 큰 값(최대 ISCSI_TEXT_MAX_VAL_LEN, 본 SPDK는
 * 2048B)이 들어올 수 있어 별도 상한을 적용한다.
 * 사용처: iscsi_parse_param()에서 키별 길이 상한 결정.
 */
static const char *non_simple_value_params[] = {
	"CHAP_C",
	/* [한국어] CHAP challenge — 1n-Login 단계에서 타깃이 보낸 임의 값. */
	"CHAP_R",
	/* [한국어] CHAP response — 이니시에이터가 challenge에 대해 계산한 응답 (MD5 등). */
	NULL,
	/* [한국어] sentinel — 배열 종료 표지. */
};

/*
 * [한국어]
 * iscsi_param_free - iscsi_param 단방향 리스트 전체를 free한다.
 *
 * @params: 리스트 헤드. NULL이면 no-op.
 *
 * 동작: head부터 next 포인터를 따라 가며 list/val/key/노드 자체를 차례로 free.
 * 호출자: 로그인 phase 종료/연결 종료 시 conn->params, sess->params 정리에 사용.
 *
 * 호출 체인:
 *   iscsi_conn_destruct/iscsi_session_destruct → [iscsi_param_free]
 */
void
iscsi_param_free(struct iscsi_param *params)
{
	struct iscsi_param *param, *next_param;
	/* [한국어] 현재 노드와 다음 노드 임시 보관 (순회 중 free 안전 패턴). */

	if (params == NULL) {
		/* [한국어] NULL 허용 — caller 편의. */
		return;
	}
	for (param = params; param != NULL; param = next_param) {
		/* [한국어] 헤드부터 끝까지 순회. */
		next_param = param->next;
		/* [한국어] free 전에 next 캐시 (free 후 param->next 접근 불가). */
		if (param->list) {
			/* [한국어] list 문자열은 NULL일 수 있음 (xstrdup(NULL)→NULL). */
			free(param->list);
		}
		free(param->val);
		/* [한국어] val 문자열 free. */
		free(param->key);
		/* [한국어] key 문자열 free. */
		free(param);
		/* [한국어] 노드 자체 free. */
	}
}

/*
 * [한국어]
 * iscsi_find_key_in_array - 키가 NULL 종료 문자열 배열에 있는지 case-insensitive 검사.
 *
 * @key: 검사할 키.
 * @array: NULL 종료 문자열 배열.
 * @return: 1 일치, 0 없음.
 *
 * non_simple_value_params, chap_type, discovery_ignored_param, multi_negot_conn_params,
 * target_declarative_params 같은 정적 분류 배열을 검사하는 데 사용된다.
 */
static int
iscsi_find_key_in_array(const char *key, const char *array[])
{
	int i;
	/* [한국어] 배열 인덱스. */

	for (i = 0; array[i] != NULL; i++) {
		/* [한국어] NULL 종료까지 순회. */
		if (strcasecmp(key, array[i]) == 0) {
			/* [한국어] 대소문자 무시 비교 — iSCSI 키는 case-insensitive (RFC 3720 §5.1). */
			return 1;
		}
	}
	return 0;
	/* [한국어] 없음. */
}

/*
 * [한국어]
 * iscsi_param_find - 단방향 리스트에서 key로 노드 검색.
 *
 * @params: 리스트 헤드.
 * @key: 검색 키 (case-insensitive).
 * @return: 일치 노드 또는 NULL.
 *
 * 첫 글자 비교로 가벼운 컷오프 후 strcasecmp로 정확 매치. iSCSI 협상 중 매우 자주 호출되는
 * 핫 함수이지만 리스트가 짧아(≤30) 선형 탐색으로 충분.
 */
struct iscsi_param *
iscsi_param_find(struct iscsi_param *params, const char *key)
{
	struct iscsi_param *param;
	/* [한국어] 순회용. */

	if (params == NULL || key == NULL) {
		/* [한국어] 빈 리스트나 NULL 키는 즉시 NULL. */
		return NULL;
	}
	for (param = params; param != NULL; param = param->next) {
		/* [한국어] 헤드부터 순회. */
		if (param->key != NULL && param->key[0] == key[0]
		    && strcasecmp(param->key, key) == 0) {
			/* [한국어] 첫 글자 빠른 컷오프 + case-insensitive 정확 비교. */
			return param;
		}
	}
	return NULL;
	/* [한국어] 미발견. */
}

/*
 * [한국어]
 * iscsi_param_del - 리스트에서 key로 노드 제거 (free 포함).
 *
 * @params: 리스트 헤드 포인터의 포인터 (헤드 변경 가능성).
 * @key: 제거 대상 키.
 * @return: 0 성공, -1 미발견.
 *
 * 단방향 리스트라 prev_param을 따로 추적. 헤드 제거 시 *params 갱신.
 *
 * 호출 체인:
 *   iscsi_param_add (중복 시 기존 노드 제거 → 신규 추가)
 */
int
iscsi_param_del(struct iscsi_param **params, const char *key)
{
	struct iscsi_param *param, *prev_param = NULL;
	/* [한국어] 현재 노드와 직전 노드 (단방향 리스트 분리용). */

	SPDK_DEBUGLOG(iscsi, "del %s\n", key);
	/* [한국어] 디버그 트레이스. */
	if (params == NULL || key == NULL) {
		/* [한국어] 인자 검증. */
		return 0;
	}
	for (param = *params; param != NULL; param = param->next) {
		/* [한국어] 헤드부터 순회. */
		if (param->key != NULL && param->key[0] == key[0]
		    && strcasecmp(param->key, key) == 0) {
			/* [한국어] 일치 발견 — prev/head를 갱신해 분리. */
			if (prev_param != NULL) {
				/* [한국어] 중간/꼬리 노드 — prev->next를 본 노드 다음으로. */
				prev_param->next = param->next;
			} else {
				/* [한국어] 헤드 — *params 자체를 갱신. */
				*params = param->next;
			}
			param->next = NULL;
			/* [한국어] free 전 next를 끊어 iscsi_param_free에서 1개만 free. */
			iscsi_param_free(param);
			/* [한국어] 노드 1개 정리 (next=NULL이라 다른 노드 영향 없음). */
			return 0;
		}
		prev_param = param;
		/* [한국어] 다음 iteration의 prev 갱신. */
	}
	return -1;
	/* [한국어] 미발견. */
}

/*
 * [한국어]
 * iscsi_param_add - 키-값 쌍을 새 노드로 만들어 리스트 끝에 추가 (중복 시 기존 제거 후 교체).
 *
 * @params: 리스트 헤드 포인터의 포인터.
 * @key/val/list/type: 노드 필드 — 모두 본 함수가 xstrdup으로 복사하므로 caller free 가능.
 * @return: 0 성공, -ENOMEM/-1 실패.
 *
 * iscsi_parse_param/iscsi_negotiate_params/iscsi_params_init_internal 모두에서 호출.
 * 리스트 끝 삽입(append)이므로 정의 순서가 유지된다.
 */
int
iscsi_param_add(struct iscsi_param **params, const char *key,
		const char *val, const char *list, int type)
{
	struct iscsi_param *param, *last_param;
	/* [한국어] 새 노드 / 리스트 마지막 노드. */

	SPDK_DEBUGLOG(iscsi, "add %s=%s, list=[%s], type=%d\n",
		      key, val, list, type);
	/* [한국어] 디버그 트레이스. */
	if (key == NULL) {
		/* [한국어] key 필수. */
		return -1;
	}

	param = iscsi_param_find(*params, key);
	/* [한국어] 동일 key 존재 검사 (idempotent 갱신 의미). */
	if (param != NULL) {
		/* [한국어] 존재하면 우선 제거 후 새로 만든다 (값 교체). */
		iscsi_param_del(params, key);
	}

	param = calloc(1, sizeof(*param));
	/* [한국어] 0-초기화 노드 할당. */
	if (!param) {
		SPDK_ERRLOG("calloc() failed for parameter\n");
		return -ENOMEM;
	}

	param->next = NULL;
	/* [한국어] 끝에 추가될 노드 — next 없음. */
	param->key = xstrdup(key);
	/* [한국어] key 복제. xstrdup은 NULL 입력 시 NULL 반환. */
	param->val = xstrdup(val);
	/* [한국어] val 복제. */
	param->list = xstrdup(list);
	/* [한국어] 허용값 후보 list 복제. */
	param->type = type;
	/* [한국어] ISPT_LIST/NUMERICAL_*/BOOLEAN_*/DECLARATIVE/INVALID 등. */

	last_param = *params;
	/* [한국어] 헤드부터 시작. */
	if (last_param != NULL) {
		/* [한국어] 비어있지 않으면 마지막 노드 찾기. */
		while (last_param->next != NULL) {
			last_param = last_param->next;
		}
		last_param->next = param;
		/* [한국어] tail에 append. */
	} else {
		*params = param;
		/* [한국어] 빈 리스트면 새 노드가 헤드. */
	}

	return 0;
	/* [한국어] 성공. */
}

/*
 * [한국어]
 * iscsi_param_set - 기존 키의 값을 문자열로 갱신.
 *
 * @params: 리스트 헤드.
 * @key: 갱신 대상 키.
 * @val: 새 값.
 * @return: 0 성공, -1 키 미존재.
 *
 * 협상 결과를 conn->params/sess->params에 반영할 때 사용.
 */
int
iscsi_param_set(struct iscsi_param *params, const char *key,
		const char *val)
{
	struct iscsi_param *param;
	/* [한국어] find 결과. */

	SPDK_DEBUGLOG(iscsi, "set %s=%s\n", key, val);
	/* [한국어] 디버그 트레이스. */
	param = iscsi_param_find(params, key);
	/* [한국어] 키 검색. */
	if (param == NULL) {
		/* [한국어] 미존재 — 협상 코드의 invariant 위반 시 발생. */
		SPDK_ERRLOG("no key %s\n", key);
		return -1;
	}

	free(param->val);
	/* [한국어] 기존 값 free 후 새 값으로 교체. */

	param->val = xstrdup(val);
	/* [한국어] 새 값 복제. */

	return 0;
}

/*
 * [한국어]
 * iscsi_param_set_int - 기존 키의 값을 정수 문자열로 갱신.
 *
 * @params/key: 위와 동일.
 * @val: uint32_t 정수.
 * @return: 0 성공, -1 키 미존재.
 *
 * MAX_TMPBUF 임시 버퍼에 snprintf로 변환 후 iscsi_param_set과 같은 흐름.
 */
int
iscsi_param_set_int(struct iscsi_param *params, const char *key, uint32_t val)
{
	char buf[MAX_TMPBUF];
	/* [한국어] "%d" 변환용 임시 버퍼. */
	struct iscsi_param *param;
	/* [한국어] find 결과. */

	SPDK_DEBUGLOG(iscsi, "set %s=%d\n", key, val);
	/* [한국어] 디버그 트레이스. */
	param = iscsi_param_find(params, key);
	/* [한국어] 검색. */
	if (param == NULL) {
		SPDK_ERRLOG("no key %s\n", key);
		return -1;
	}

	free(param->val);
	/* [한국어] 기존 값 free. */
	snprintf(buf, sizeof buf, "%d", val);
	/* [한국어] 정수→문자열. */

	param->val = strdup(buf);
	/* [한국어] 임시 버퍼를 heap 복제하여 보관. */

	return 0;
}

/**
 * Parse a single KEY=VAL pair
 *
 * data = "KEY=VAL<NUL>"
 */
/*
 * [한국어]
 * iscsi_parse_param - 한 개의 "KEY=VAL\0" 텍스트 항목을 파싱하여 리스트에 추가.
 *
 * @params: 리스트 헤드 포인터의 포인터.
 * @data: PDU data 영역 시작 주소.
 * @data_len: 잔여 가용 길이 (다음 K=V까지의 상한).
 * @return: 소비한 바이트 수 (key_len + 1('=') + val_len + 1('\0')) 또는 -1/-ENOMEM.
 *
 * 동작: data 안에서 '='를 찾아 key/val 분리 → 길이 검증 (RFC 7143 §6.1: key ≤63B,
 * RFC 3720 §5.1: simple value ≤255B; CHAP_C/R는 더 큼) → 중복 검사 → iscsi_param_add.
 *
 * 호출 체인:
 *   iscsi_parse_params → [iscsi_parse_param] → iscsi_param_add
 */
static int
iscsi_parse_param(struct iscsi_param **params, const uint8_t *data, uint32_t data_len)
{
	int rc;
	/* [한국어] add 결과. */
	uint8_t *key_copy, *val_copy;
	/* [한국어] key/val의 NUL-terminated 복사본. */
	const uint8_t *key_end;
	/* [한국어] '=' 위치. */
	int key_len, val_len;
	/* [한국어] 각각의 길이. */
	int max_len;
	/* [한국어] 키 종류에 따른 val 상한. */

	data_len = strnlen(data, data_len);
	/* [한국어] data 안의 NUL 또는 data_len 중 짧은 쪽 — 한 KV 항목 길이 제한. */
	/* No such thing as strnchr so use memchr instead. */
	key_end = memchr(data, '=', data_len);
	/* [한국어] '=' 위치 검색 — strnchr이 없어 memchr 사용. */
	if (!key_end) {
		/* [한국어] '=' 없음 — 잘못된 K=V. */
		SPDK_ERRLOG("'=' not found\n");
		return -1;
	}

	key_len = key_end - data;
	/* [한국어] '=' 앞까지가 key. */
	if (key_len == 0) {
		/* [한국어] "=VAL" 형태는 거부. */
		SPDK_ERRLOG("Empty key\n");
		return -1;
	}
	/*
	 * RFC 7143 6.1
	 */
	if (key_len > ISCSI_TEXT_MAX_KEY_LEN) {
		/* [한국어] RFC 7143 §6.1: key 최대 63 바이트. */
		SPDK_ERRLOG("Key name length is bigger than 63\n");
		return -1;
	}

	key_copy = malloc(key_len + 1);
	/* [한국어] NUL 자리 포함하여 +1. */
	if (!key_copy) {
		SPDK_ERRLOG("malloc() failed for key_copy\n");
		return -ENOMEM;
	}

	memcpy(key_copy, data, key_len);
	/* [한국어] key 복사. */
	key_copy[key_len] = '\0';
	/* [한국어] NUL 종료. */
	/* check whether this key is duplicated */
	if (NULL != iscsi_param_find(*params, key_copy)) {
		/* [한국어] 한 PDU 내 중복 KV는 RFC 위반 — 거부. */
		SPDK_ERRLOG("Duplicated Key %s\n", key_copy);
		free(key_copy);
		return -1;
	}

	val_len = strnlen(key_end + 1, data_len - key_len - 1);
	/* [한국어] '=' 이후부터 NUL까지의 길이. */
	/*
	 * RFC 3720 5.1
	 * If not otherwise specified, the maximum length of a simple-value
	 * (not its encoded representation) is 255 bytes, not including the delimiter
	 * (comma or zero byte).
	 */
	/*
	 * comma or zero is counted in, otherwise we need to iterate each parameter
	 * value
	 */
	max_len = iscsi_find_key_in_array(key_copy, non_simple_value_params) ?
		  ISCSI_TEXT_MAX_VAL_LEN : ISCSI_TEXT_MAX_SIMPLE_VAL_LEN;
	/* [한국어] CHAP_C/CHAP_R이면 큰 상한, 그 외는 simple-value 255B 상한. */
	if (val_len > max_len) {
		/* [한국어] 상한 초과는 거부. */
		SPDK_ERRLOG("Overflow Val %d\n", val_len);
		free(key_copy);
		return -1;
	}

	val_copy = calloc(1, val_len + 1);
	/* [한국어] NUL 종료 보장 위해 calloc. */
	if (val_copy == NULL) {
		SPDK_ERRLOG("Could not allocate value string\n");
		free(key_copy);
		return -1;
	}

	memcpy(val_copy, key_end + 1, val_len);
	/* [한국어] val 복사 (NUL은 calloc으로 이미 0). */

	rc = iscsi_param_add(params, key_copy, val_copy, NULL, 0);
	/* [한국어] 리스트에 추가. type=0(ISPT_INVALID) — 협상 시 cur_param 타입으로 갱신됨. */
	free(val_copy);
	free(key_copy);
	/* [한국어] add 내부에서 xstrdup 복사하므로 임시 버퍼 free 가능. */
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_add() failed\n");
		return -1;
	}

	/* return number of bytes consumed
	 * +1 for '=' and +1 for NUL
	 */
	return key_len + 1 + val_len + 1;
	/* [한국어] 호출자가 다음 KV 시작 위치를 결정할 수 있도록 소비 바이트 수 반환. */
}

/**
 * Parse a sequence of KEY=VAL pairs.
 *
 * \param data "KEY=VAL<NUL>KEY=VAL<NUL>..."
 * \param len length of data in bytes
 *
 * Data must point to a valid pointer if len > 0.
 */
/*
 * [한국어]
 * iscsi_parse_params - 다수의 KV 쌍이 NUL 구분으로 직렬화된 PDU data를 파싱.
 *
 * @params: 출력 리스트 헤드 포인터의 포인터 (caller가 NULL로 초기화).
 * @data: PDU data 시작.
 * @len: data 길이.
 * @cbit_enabled: 텍스트 PDU의 C(continue) 비트 — 이번 PDU가 끝나도 다음 PDU에 이어짐을 의미.
 * @partial_parameter: 이전 PDU에서 잘린 부분 KV 보관 buffer (caller heap, 본 함수가 갱신).
 * @return: 0 성공, -1 실패.
 *
 * 흐름:
 *   1) 이전 PDU 잔여(*partial_parameter)가 있으면 현재 데이터 앞부분과 연결하여 1개 KV 파싱.
 *   2) cbit가 set이면 현재 PDU 끝쪽의 NUL 없는 잔여 부분을 *partial_parameter로 잘라 보관.
 *   3) 남은 영역에서 NUL 단위로 iscsi_parse_param 반복 호출.
 *
 * 호출 체인:
 *   로그인/텍스트 PDU 처리 → [iscsi_parse_params] → iscsi_parse_param → iscsi_param_add
 */
int
iscsi_parse_params(struct iscsi_param **params, const uint8_t *data,
		   int len, bool cbit_enabled, char **partial_parameter)
{
	int rc, offset = 0;
	/* [한국어] rc=각 호출 결과, offset=현재 처리 위치. */
	char *p;
	/* [한국어] 잔여+신규 결합 임시 버퍼. */
	int i;
	/* [한국어] 부분 파라미터 길이 측정용 인덱스. */

	/* Spec does not disallow TEXT PDUs with zero length, just return
	 * immediately in that case, since there is no param data to parse
	 * and any existing partial parameter would remain as-is.
	 */
	if (len == 0) {
		/* [한국어] 빈 PDU는 정상 — 이전 partial은 보존. */
		return 0;
	}

	assert(data != NULL);
	/* [한국어] len>0이면 data는 반드시 유효 포인터. */

	/* strip the partial text parameters if previous PDU have C enabled */
	if (partial_parameter && *partial_parameter) {
		/* [한국어] 이전 PDU에서 잘린 KV 잔여가 있는 경우, 현재 PDU 앞과 합쳐 1개 KV 완성. */
		for (i = 0; i < len && data[i] != '\0'; i++) {
			;
			/* [한국어] 첫 NUL 위치 i 탐색 — i 직전까지가 잘린 KV의 뒷부분. */
		}
		p = spdk_sprintf_alloc("%s%s", *partial_parameter, (const char *)data);
		/* [한국어] 잔여 + 현재 앞부분 연결한 새 문자열 (heap). */
		if (!p) {
			return -1;
		}
		rc = iscsi_parse_param(params, p, i + strlen(*partial_parameter));
		/* [한국어] 합쳐진 KV를 단일 항목으로 파싱. */
		free(p);
		if (rc < 0) {
			return -1;
		}
		free(*partial_parameter);
		/* [한국어] 잔여 소진 — free + NULL 표시. */
		*partial_parameter = NULL;

		data = data + i + 1;
		/* [한국어] 합쳐 처리한 부분 건너뜀 (NUL 1바이트 포함). */
		len = len - (i + 1);
	}

	/* strip the partial text parameters if C bit is enabled */
	if (cbit_enabled) {
		/* [한국어] 이번 PDU가 C(Continue) — 마지막 KV가 잘려있을 수 있음. */
		if (partial_parameter == NULL) {
			SPDK_ERRLOG("C bit set but no partial parameters provided\n");
			return -1;
		}

		/*
		 * reverse iterate the string from the tail not including '\0'
		 */
		for (i = len - 1; data[i] != '\0' && i > 0; i--) {
			;
			/* [한국어] 끝부터 거꾸로 가다가 첫 NUL을 발견하면 정지 — i는 NUL 위치(또는 0). */
		}
		if (i != 0) {
			/* We found a NULL character - don't copy it into the
			 * partial parameter.
			 */
			i++;
			/* [한국어] NUL 다음 위치부터가 잘린 KV의 시작. */
		}

		*partial_parameter = calloc(1, len - i + 1);
		/* [한국어] 잘린 부분 보관 buffer (NUL 종료). */
		if (*partial_parameter == NULL) {
			SPDK_ERRLOG("could not allocate partial parameter\n");
			return -1;
		}
		memcpy(*partial_parameter, &data[i], len - i);
		/* [한국어] data[i..] 복사. */
		if (i == 0) {
			/* No full parameters to parse - so return now. */
			/* [한국어] PDU 전체가 잘린 1개 KV였음 — 파싱할 완전한 KV 없음. */
			return 0;
		} else {
			len = i - 1;
			/* [한국어] 마지막 NUL 직전까지만 정상 KV 영역으로 처리. */
		}
	}

	while (offset < len && data[offset] != '\0') {
		/* [한국어] 남은 영역에서 NUL 단위로 KV 반복 파싱. */
		rc = iscsi_parse_param(params, data + offset, len - offset);
		if (rc < 0) {
			/* [한국어] 어느 KV든 파싱 실패는 즉시 -1. */
			return -1;
		}
		offset += rc;
		/* [한국어] 소비 바이트만큼 전진. */
	}
	return 0;
	/* [한국어] 모든 KV 정상 파싱. */
}

/*
 * [한국어]
 * iscsi_param_get_val - 키의 값 문자열을 즉시 가져오는 편의 함수.
 *
 * @params/key: 검색 대상.
 * @return: val 포인터 (리스트 내부 — caller free 금지) 또는 NULL.
 */
char *
iscsi_param_get_val(struct iscsi_param *params, const char *key)
{
	struct iscsi_param *param;
	/* [한국어] find 결과. */

	param = iscsi_param_find(params, key);
	/* [한국어] 검색. */
	if (param == NULL) {
		return NULL;
	}
	return param->val;
	/* [한국어] 내부 buffer 그대로 반환. */
}

/*
 * [한국어]
 * iscsi_param_eq_val - 키의 값이 주어진 문자열과 case-insensitive 일치하는지 검사.
 *
 * @return: 1 일치, 0 불일치 또는 미존재.
 */
int
iscsi_param_eq_val(struct iscsi_param *params, const char *key,
		   const char *val)
{
	struct iscsi_param *param;
	/* [한국어] find 결과. */

	param = iscsi_param_find(params, key);
	/* [한국어] 검색. */
	if (param == NULL) {
		return 0;
	}
	if (strcasecmp(param->val, val) == 0) {
		/* [한국어] 값 비교 (case-insensitive). */
		return 1;
	}
	return 0;
}

/*
 * [한국어]
 * struct iscsi_param_table - 기본 정책 테이블 엔트리 (정적 초기화용).
 *
 * conn_param_table / sess_param_table에서 키별 기본값/허용값/타입을 정의한다.
 * iscsi_params_init_internal이 이 테이블을 순회해 iscsi_param_add를 호출한다.
 */
struct iscsi_param_table {
	const char *key;
	/* [한국어] 키 이름 (case-insensitive). RFC 3720 §11에 정의된 키 또는 SPDK 확장. */
	const char *val;
	/* [한국어] 기본값 (initiator가 보내지 않으면 사용). */
	const char *list;
	/* [한국어] 허용값 후보 — ISPT_LIST이면 콤마 구분 후보, ISPT_NUMERICAL_*이면 "min,max". */
	int type;
	/* [한국어] 협상 알고리즘 식별자 — ISPT_LIST/NUMERICAL_MIN/MAX/DECLARATIVE/BOOLEAN_AND/OR. */
};

/*
 * [한국어]
 * conn_param_table - 연결 단위 협상 키 테이블 (RFC 3720 §12).
 *
 * 연결마다 독립적인 협상이 가능한 키들. 일부(예: HeaderDigest, DataDigest)는 다이제스트
 * 알고리즘 선택, MaxRecvDataSegmentLength는 PDU의 한 burst 안에서 수신 가능한 최대 길이.
 * Marker(OF/IF Marker)는 RFC 7143에서 deprecated이지만 호환을 위해 No로 협상.
 */
static const struct iscsi_param_table conn_param_table[] = {
	{ "HeaderDigest", "None", "CRC32C,None", ISPT_LIST },
	/* [한국어] BHS+AHS의 CRC32C 무결성 — initiator/target 중 하나라도 None이면 None. */
	{ "DataDigest", "None", "CRC32C,None", ISPT_LIST },
	/* [한국어] data 영역 CRC32C — 위와 동일한 협상 패턴. */
	{ "MaxRecvDataSegmentLength", "8192", "512,16777215", ISPT_NUMERICAL_DECLARATIVE },
	/* [한국어] declarative — 양 방향 독립값. RFC 3720 §12.12. */
	{ "OFMarker", "No", "Yes,No", ISPT_BOOLEAN_AND },
	/* [한국어] AND boolean — 둘 다 Yes일 때만 Yes. RFC 7143에서 폐지 권고. */
	{ "IFMarker", "No", "Yes,No", ISPT_BOOLEAN_AND },
	/* [한국어] 동일. */
	{ "OFMarkInt", "1", "1,65535", ISPT_NUMERICAL_MIN },
	/* [한국어] MIN — 양측 제시값 중 더 작은 값. */
	{ "IFMarkInt", "1", "1,65535", ISPT_NUMERICAL_MIN },
	/* [한국어] 동일. */
	{ "AuthMethod", "None", "CHAP,None", ISPT_LIST },
	/* [한국어] 보안 협상 — CHAP 또는 None. */
	{ "CHAP_A", "5", "5", ISPT_LIST },
	/* [한국어] CHAP 알고리즘 — 5(MD5)만 허용. */
	{ "CHAP_N", "", "", ISPT_DECLARATIVE },
	/* [한국어] CHAP user name. declarative — 양측 독립. */
	{ "CHAP_R", "", "", ISPT_DECLARATIVE },
	/* [한국어] CHAP response. */
	{ "CHAP_I", "", "", ISPT_DECLARATIVE },
	/* [한국어] CHAP identifier. */
	{ "CHAP_C", "", "", ISPT_DECLARATIVE },
	/* [한국어] CHAP challenge. */
	{ NULL, NULL, NULL, ISPT_INVALID },
	/* [한국어] sentinel — 테이블 종료. */
};

/*
 * [한국어]
 * sess_param_table - 세션 단위 협상 키 테이블 (RFC 3720 §12).
 *
 * 한 세션 내 모든 연결에 공통적용되는 키들. R2T/Burst length 등 세션 전체의 흐름 제어에
 * 영향을 주는 항목이 많다.
 */
static const struct iscsi_param_table sess_param_table[] = {
	{ "MaxConnections", "1", "1,65535", ISPT_NUMERICAL_MIN },
	/* [한국어] 세션 내 최대 connection 수 — MIN 협상. */
#if 0
	/* need special handling */
	{ "SendTargets", "", "", ISPT_DECLARATIVE },
#endif
	/* [한국어] SendTargets는 Discovery 전용 명령으로 본 테이블 일반 협상에서 제외. */
	{ "TargetName", "", "", ISPT_DECLARATIVE },
	/* [한국어] IQN. */
	{ "InitiatorName", "", "", ISPT_DECLARATIVE },
	/* [한국어] 이니시에이터 IQN. */
	{ "TargetAlias", "", "", ISPT_DECLARATIVE },
	/* [한국어] 사람이 읽기 위한 별칭. */
	{ "InitiatorAlias", "", "", ISPT_DECLARATIVE },
	/* [한국어] 동일. */
	{ "TargetAddress", "", "", ISPT_DECLARATIVE },
	/* [한국어] redirect 용 주소. */
	{ "TargetPortalGroupTag", "1", "1,65535", ISPT_NUMERICAL_DECLARATIVE },
	/* [한국어] 연결된 PG의 tag. */
	{ "InitialR2T", "Yes", "Yes,No", ISPT_BOOLEAN_OR },
	/* [한국어] OR boolean — 한쪽이라도 Yes면 Yes (R2T 사용 강제 가능). */
	{ "ImmediateData", "Yes", "Yes,No", ISPT_BOOLEAN_AND },
	/* [한국어] AND boolean — 둘 다 Yes일 때만 Immediate Data 허용. */
	{ "MaxBurstLength", "262144", "512,16777215", ISPT_NUMERICAL_MIN },
	/* [한국어] 한 burst의 최대 R/W data 길이. */
	{ "FirstBurstLength", "65536", "512,16777215", ISPT_NUMERICAL_MIN },
	/* [한국어] R2T 없이 보낼 수 있는 첫 burst 길이. */
	{ "DefaultTime2Wait", "2", "0,3600", ISPT_NUMERICAL_MAX },
	/* [한국어] 연결 종료 후 대기 시간 (sec). */
	{ "DefaultTime2Retain", "20", "0,3600", ISPT_NUMERICAL_MIN },
	/* [한국어] 세션 정보 보존 시간. */
	{ "MaxOutstandingR2T", "1", "1,65536", ISPT_NUMERICAL_MIN },
	/* [한국어] 동시에 outstanding 가능한 R2T 수. */
	{ "DataPDUInOrder", "Yes", "Yes,No", ISPT_BOOLEAN_OR },
	/* [한국어] data PDU의 순서 보장. */
	{ "DataSequenceInOrder", "Yes", "Yes,No", ISPT_BOOLEAN_OR },
	/* [한국어] data 시퀀스의 순서 보장. */
	{ "ErrorRecoveryLevel", "0", "0,2", ISPT_NUMERICAL_MIN },
	/* [한국어] 오류 복구 레벨 (0/1/2). */
	{ "SessionType", "Normal", "Normal,Discovery", ISPT_DECLARATIVE },
	/* [한국어] 세션 종류 — Normal(I/O) 또는 Discovery(SendTargets). */
	{ NULL, NULL, NULL, ISPT_INVALID },
	/* [한국어] sentinel. */
};

/*
 * [한국어]
 * iscsi_params_init_internal - 정적 테이블을 순회하여 기본 파라미터 리스트를 만든다.
 *
 * @params: 출력 리스트 헤드 포인터의 포인터.
 * @table: NULL 종료 정책 테이블.
 * @return: 0 성공, -1 실패.
 *
 * 각 엔트리에 대해 iscsi_param_add 후 state_index를 테이블 인덱스로 설정. state_index는
 * conn->{conn,sess}_param_state_negotiated 비트맵을 인덱싱하여 "이 키가 이미 협상됨" 표시.
 *
 * 호출 체인:
 *   iscsi_conn_params_init/iscsi_sess_params_init → [iscsi_params_init_internal]
 */
static int
iscsi_params_init_internal(struct iscsi_param **params,
			   const struct iscsi_param_table *table)
{
	int rc;
	/* [한국어] add 결과. */
	int i;
	/* [한국어] 테이블 인덱스 (state_index와 동일). */
	struct iscsi_param *param;
	/* [한국어] 방금 추가된 노드 검색 결과. */

	for (i = 0; table[i].key != NULL; i++) {
		/* [한국어] sentinel(NULL) 이전까지 순회. */
		rc = iscsi_param_add(params, table[i].key, table[i].val,
				     table[i].list, table[i].type);
		/* [한국어] 기본값으로 새 노드 추가. */
		if (rc < 0) {
			SPDK_ERRLOG("iscsi_param_add() failed\n");
			return -1;
		}
		param = iscsi_param_find(*params, table[i].key);
		/* [한국어] 방금 추가된 노드를 다시 찾아 state_index 설정. */
		if (param != NULL) {
			param->state_index = i;
			/* [한국어] state_negotiated 비트맵 인덱스 — 협상 1회 검사용. */
		} else {
			SPDK_ERRLOG("iscsi_param_find() failed\n");
			return -1;
		}
	}

	return 0;
}

/*
 * [한국어]
 * iscsi_conn_params_init - conn 단위 기본 파라미터 리스트 생성.
 */
int
iscsi_conn_params_init(struct iscsi_param **params)
{
	return iscsi_params_init_internal(params, &conn_param_table[0]);
	/* [한국어] conn_param_table을 사용. */
}

/*
 * [한국어]
 * iscsi_sess_params_init - 세션 단위 기본 파라미터 리스트 생성.
 */
int
iscsi_sess_params_init(struct iscsi_param **params)
{
	return iscsi_params_init_internal(params, &sess_param_table[0]);
	/* [한국어] sess_param_table을 사용. */
}

/*
 * [한국어]
 * chap_type - CHAP 관련 키 목록 (협상 루프에서 일반 협상 대상에서 제외).
 *
 * CHAP 키들은 별도의 인증 상태 머신(iscsi_auth.c)에서 처리하므로 본 협상기에서는 skip.
 */
static const char *chap_type[] = {
	"CHAP_A",
	"CHAP_N",
	"CHAP_R",
	"CHAP_I",
	"CHAP_C",
	NULL,
	/* [한국어] sentinel. */
};

/*
 * [한국어]
 * discovery_ignored_param - Discovery 세션에서 무시되는 파라미터 (RFC 3720 §11).
 *
 * Discovery 세션은 SendTargets 응답만 다루므로 R2T/Burst 등 I/O 흐름 제어 키는
 * "Irrelevant"로 응답해야 한다(§12.2 등).
 */
static const char *discovery_ignored_param[] = {
	"MaxConnections",
	"InitialR2T",
	"ImmediateData",
	"MaxBurstLength",
	"FirstBurstLength",
	"MaxOutstandingR2T",
	"DataPDUInOrder",
	"DataSequenceInOrder",
	NULL,
	/* [한국어] sentinel. */
};

/*
 * [한국어]
 * multi_negot_conn_params - 한 번 이상 협상 가능한 conn 키 (LeadingConn 후 추가 conn).
 *
 * 일반 키는 한 세션에서 1회만 협상되어야 하지만 MaxRecvDataSegmentLength는 declarative 라
 * conn별 다를 수 있어 여러 번 등장 허용.
 */
static const char *multi_negot_conn_params[] = {
	"MaxRecvDataSegmentLength",
	NULL,
	/* [한국어] sentinel. */
};

/* The following params should be declared by target */
/*
 * [한국어]
 * target_declarative_params - 타깃이 일방 선언하는 키 (initiator는 변경 불가).
 *
 * initiator가 이 키를 보내도 response에는 타깃 측 값을 그대로 declarative로 응답해
 * 변경을 막는다.
 */
static const char *target_declarative_params[] = {
	"TargetAlias",
	"TargetAddress",
	"TargetPortalGroupTag",
	NULL,
	/* [한국어] sentinel. */
};

/* This function is used to construct the data from the special param (e.g.,
 * MaxRecvDataSegmentLength)
 * return:
 * normal: the total len of the data
 * error: -1
 */
/*
 * [한국어]
 * iscsi_special_param_construction - 특수 키(MaxRecvDataSegmentLength, MaxBurstLength)에
 *   대해 타깃 측 declarative 응답을 응답 buffer에 추가.
 *
 * @conn: 현재 연결.
 * @param: 처리 중인 파라미터 (initiator 측).
 * @FirstBurstLength_flag: initiator가 FirstBurstLength도 보낸 경우 true (이미 처리됨).
 * @data/alloc_len/total: 출력 buffer/총 capacity/현재 누적 길이.
 * @return: 갱신된 total 또는 -1/-ENOMEM.
 *
 * 처리 키:
 *   1) MaxRecvDataSegmentLength: declarative이므로 타깃 자기 값을 추가 송신.
 *   2) MaxBurstLength (그리고 initiator가 FirstBurstLength를 보내지 않은 경우):
 *      FirstBurstLength ≤ MaxBurstLength 강제하여 갱신값 송신.
 *
 * 호출 체인:
 *   iscsi_negotiate_params → [iscsi_special_param_construction]
 */
static int
iscsi_special_param_construction(struct spdk_iscsi_conn *conn,
				 struct iscsi_param *param,
				 bool FirstBurstLength_flag, char *data,
				 int alloc_len, int total)
{
	int len;
	/* [한국어] snprintf 결과 (NUL 제외). */
	struct iscsi_param *param_first;
	/* [한국어] FirstBurstLength 노드. */
	struct iscsi_param *param_max;
	/* [한국어] MaxBurstLength 노드. */
	uint32_t FirstBurstLength;
	uint32_t MaxBurstLength;
	/* [한국어] 정수 변환 캐시. */
	char *val;
	/* [한국어] 임시 정수→문자열 buffer. */

	val = malloc(ISCSI_TEXT_MAX_VAL_LEN + 1);
	/* [한국어] 임시 buffer 할당. */
	if (!val) {
		SPDK_ERRLOG("malloc() failed for temporary buffer\n");
		return -ENOMEM;
	}

	if (strcasecmp(param->key, "MaxRecvDataSegmentLength") == 0) {
		/*
		 * MaxRecvDataSegmentLength is sent by both
		 *      initiator and target, but is declarative - meaning
		 *      each direction can have different values.
		 * So when MaxRecvDataSegmentLength is found in the
		 *      the parameter set sent from the initiator, add SPDK
		 *      iscsi target's MaxRecvDataSegmentLength value to
		 *      the returned parameter list.
		 */
		if (alloc_len - total < 1) {
			/* [한국어] 응답 buffer 부족. */
			SPDK_ERRLOG("data space small %d\n", alloc_len);
			free(val);
			return -1;
		}

		SPDK_DEBUGLOG(iscsi,
			      "returning MaxRecvDataSegmentLength=%d\n",
			      SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH);
		/* [한국어] 디버그 — 타깃 측 declarative 송신. */
		len = snprintf((char *)data + total, alloc_len - total,
			       "MaxRecvDataSegmentLength=%d",
			       SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH);
		/* [한국어] data buffer에 K=V 직렬화 (NUL은 별도 1바이트 포함). */
		total += len + 1;
		/* [한국어] +1: K=V 끝의 NUL 포함. */
	}

	if (strcasecmp(param->key, "MaxBurstLength") == 0 &&
	    !FirstBurstLength_flag) {
		/* [한국어] initiator가 MaxBurstLength만 보내고 FirstBurstLength는 안 보낸 경우.
		 * FirstBurstLength ≤ MaxBurstLength 일관성을 강제하기 위해 타깃이 FBL을 declarative 송신. */
		if (alloc_len - total < 1) {
			SPDK_ERRLOG("data space small %d\n", alloc_len);
			free(val);
			return -1;
		}

		param_first = iscsi_param_find(conn->sess->params,
					       "FirstBurstLength");
		/* [한국어] 세션의 현재 FBL 노드 (기본값). */
		if (param_first != NULL) {
			FirstBurstLength = (uint32_t)strtol(param_first->val, NULL, 10);
		} else {
			FirstBurstLength = SPDK_ISCSI_FIRST_BURST_LENGTH;
		}
		param_max = iscsi_param_find(conn->sess->params,
					     "MaxBurstLength");
		/* [한국어] 세션의 MBL 노드 (initiator 협상 결과 반영됨). */
		if (param_max != NULL) {
			MaxBurstLength = (uint32_t)strtol(param_max->val, NULL, 10);
		} else {
			MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;
		}

		if (FirstBurstLength > MaxBurstLength) {
			/* [한국어] FBL > MBL 불일치 — FBL을 MBL로 캡. */
			FirstBurstLength = MaxBurstLength;
			if (param_first != NULL) {
				/* [한국어] 세션 노드 업데이트 — 이후 일관 조회. */
				free(param_first->val);
				snprintf(val, ISCSI_TEXT_MAX_VAL_LEN, "%d",
					 FirstBurstLength);
				param_first->val = xstrdup(val);
			}
		}
		len = snprintf((char *)data + total, alloc_len - total,
			       "FirstBurstLength=%d", FirstBurstLength);
		/* [한국어] declarative로 응답 buffer에 추가. */
		total += len + 1;
		/* [한국어] NUL 포함 길이 누적. */
	}

	free(val);
	/* [한국어] 임시 buffer free. */
	return total;

}

/**
 * iscsi_construct_data_from_param:
 * To construct the data which will be returned to the initiator
 * return: length of the negotiated data, -1 indicates error;
 */
/*
 * [한국어]
 * iscsi_construct_data_from_param - 협상 결과 KV를 응답 buffer에 직렬화 (declarative 키는 제외).
 *
 * @param: 처리 중인 파라미터.
 * @new_val: 협상 결과 값.
 * @data/alloc_len/total: 응답 buffer/총 capacity/현재 누적.
 * @return: 갱신된 total 또는 -1.
 *
 * declarative 키는 동일 응답을 다시 보낼 필요가 없으므로 skip. 이외 키는 "K=V\0"으로 추가.
 */
static int
iscsi_construct_data_from_param(struct iscsi_param *param, char *new_val,
				char *data, int alloc_len, int total)
{
	int len;
	/* [한국어] snprintf 결과. */

	if (param->type != ISPT_DECLARATIVE &&
	    param->type != ISPT_NUMERICAL_DECLARATIVE) {
		/* [한국어] declarative가 아니면 응답 buffer에 누적. */
		if (alloc_len - total < 1) {
			SPDK_ERRLOG("data space small %d\n", alloc_len);
			return -1;
		}

		SPDK_DEBUGLOG(iscsi, "negotiated %s=%s\n",
			      param->key, new_val);
		/* [한국어] 디버그 — 협상 결과 트레이스. */
		len = snprintf((char *)data + total, alloc_len - total, "%s=%s",
			       param->key, new_val);
		/* [한국어] "K=V" 직렬화. */
		total += len + 1;
		/* [한국어] NUL 1바이트 포함. */
	}
	return total;
}

/**
 * To negotiate param with
 * type = ISPT_LIST
 * return: the negotiated value of the key
 */
/*
 * [한국어]
 * iscsi_negotiate_param_list - ISPT_LIST 키 협상.
 *
 * @add_param_value: 출력 — caller에게 "별도 추가가 아닌 set 갱신" 표시 (NULL이면 reject).
 * @param: 처리 중인 파라미터.
 * @valid_list: 타깃 허용 후보 (콤마 구분).
 * @in_val: initiator가 보낸 후보 (콤마 구분, in-place 분리됨).
 * @cur_val: 현재 타깃 측 값 (사용하지 않음 — 시그니처 통일용).
 * @return: initiator·타깃 양측 후보의 첫 교집합 문자열 또는 NULL.
 *
 * RFC 3720 §5.1 list-value: initiator의 후보를 좌→우 순회하며 타깃 허용 후보 중 첫 매치
 * 사용. in_val/valid_list는 콤마를 NUL로 in-place 치환했다가 미매치면 콤마 복구하는 전형적
 * strsep 패턴.
 */
static char *
iscsi_negotiate_param_list(int *add_param_value,
			   struct iscsi_param *param,
			   char *valid_list, char *in_val,
			   char *cur_val)
{
	char *val_start, *val_end;
	/* [한국어] valid_list 내 한 후보의 시작/끝. */
	char *in_start, *in_end;
	/* [한국어] in_val 내 한 후보의 시작/끝. */
	int flag = 0;
	/* [한국어] 매치 발견 표시. */

	if (add_param_value == NULL) {
		/* [한국어] reject 신호 (caller invariant). */
		return NULL;
	}

	in_start = in_val;
	/* [한국어] initiator 후보 좌측부터. */
	do {
		if ((in_end = strchr(in_start, (int)',')) != NULL) {
			*in_end = '\0';
			/* [한국어] 콤마를 NUL로 — 한 후보 추출. */
		}
		val_start = valid_list;
		/* [한국어] 타깃 허용 후보 좌측부터. */
		do {
			if ((val_end = strchr(val_start, (int)',')) != NULL) {
				*val_end = '\0';
				/* [한국어] 마찬가지로 콤마 NUL. */
			}
			if (strcasecmp(in_start, val_start) == 0) {
				/* [한국어] 매치. */
				SPDK_DEBUGLOG(iscsi, "match %s\n",
					      val_start);
				flag = 1;
				break;
			}
			if (val_end) {
				*val_end = ',';
				/* [한국어] 미매치 시 콤마 복구. */
				val_start = val_end + 1;
				/* [한국어] 다음 후보로. */
			}
		} while (val_end);
		if (flag) {
			break;
			/* [한국어] 매치 발견 — outer loop 탈출. */
		}
		if (in_end) {
			*in_end = ',';
			/* [한국어] 미매치 시 콤마 복구. */
			in_start = in_end + 1;
		}
	} while (in_end);

	return flag ? val_start : NULL;
	/* [한국어] 매치된 val_start (NUL 잘려있어 단일 후보) 또는 NULL. */
}

/**
 * To negotiate param with
 * type = ISPT_NUMERICAL_MIN/MAX, ISPT_NUMERICAL_DECLARATIVE
 * return: the negotiated value of the key
 */
/*
 * [한국어]
 * iscsi_negotiate_param_numerical - 정수 키 협상 (MIN/MAX/DECLARATIVE).
 *
 * @valid_list: "min,max" 형태.
 * @in_val: initiator 값 (in-place 갱신됨).
 * @cur_val: 타깃 현재 값.
 * @return: 협상 결과 정수 문자열 (in_val) 또는 NULL(범위 외).
 *
 * MIN: min(initiator, target). MAX: max(initiator, target). DECLARATIVE: initiator 값 그대로.
 * FirstBurstLength는 별도 처리: in_val을 사용하여 산정.
 */
static char *
iscsi_negotiate_param_numerical(int *add_param_value,
				struct iscsi_param *param,
				char *valid_list, char *in_val,
				char *cur_val)
{
	char *valid_next;
	/* [한국어] strsepq 진행 포인터. */
	char *new_val = NULL;
	/* [한국어] 결과. */
	char *min_val, *max_val;
	/* [한국어] valid_list 분해 결과 (min/max 문자열). */
	int val_i, cur_val_i;
	/* [한국어] initiator 값과 현재값(정수). */
	int min_i, max_i;
	/* [한국어] 허용 범위. */

	if (add_param_value == NULL) {
		return NULL;
	}

	val_i = (int)strtol(param->val, NULL, 10);
	/* [한국어] initiator 값 정수화 (param->val은 파싱 결과 보관). */
	/* check whether the key is FirstBurstLength, if that we use in_val */
	if (strcasecmp(param->key, "FirstBurstLength") == 0) {
		val_i = (int)strtol(in_val, NULL, 10);
		/* [한국어] FBL은 위쪽에서 MaxBurstLength와 캡 처리되어 in_val에 미리 갱신됨. */
	}

	cur_val_i = (int)strtol(cur_val, NULL, 10);
	/* [한국어] 타깃 현재값 정수화. */
	valid_next = valid_list;
	min_val = spdk_strsepq(&valid_next, ",");
	/* [한국어] valid_list "min,max"의 min 추출 (in-place 분리). */
	max_val = spdk_strsepq(&valid_next, ",");
	/* [한국어] max 추출. */
	min_i = (min_val != NULL) ? (int)strtol(min_val, NULL, 10) : 0;
	max_i = (max_val != NULL) ? (int)strtol(max_val, NULL, 10) : 0;
	if (val_i < min_i || val_i > max_i) {
		/* [한국어] 범위 외 — reject. */
		SPDK_DEBUGLOG(iscsi, "key %.64s reject\n", param->key);
		new_val = NULL;
	} else {
		switch (param->type) {
		case ISPT_NUMERICAL_MIN:
			if (val_i > cur_val_i) {
				val_i = cur_val_i;
				/* [한국어] MIN 협상 — 더 작은 값 채택. */
			}
			break;
		case ISPT_NUMERICAL_MAX:
			if (val_i < cur_val_i) {
				val_i = cur_val_i;
				/* [한국어] MAX 협상 — 더 큰 값 채택. */
			}
			break;
		default:
			break;
			/* [한국어] DECLARATIVE — initiator 값 그대로. */
		}
		snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN, "%d", val_i);
		/* [한국어] 결과를 in_val에 in-place 기록 (caller가 응답에 사용). */
		new_val = in_val;
	}

	return new_val;
}

/**
 * To negotiate param with
 * type = ISPT_BOOLEAN_OR, ISPT_BOOLEAN_AND
 * return: the negotiated value of the key
 */
/*
 * [한국어]
 * iscsi_negotiate_param_boolean - boolean 키 협상 (OR/AND).
 *
 * @value: "Yes"(OR) 또는 "No"(AND) — 강제 결정값.
 *   OR: cur_val=="Yes"이면 결과 "Yes" (어느 한 쪽이 Yes면 Yes).
 *   AND: cur_val=="No"이면 결과 "No" (어느 한 쪽이 No면 No).
 * 그 외에는 initiator 값을 그대로 채택(param->val).
 *
 * Yes/No가 아닌 입력은 "Reject"로 응답.
 */
static char *
iscsi_negotiate_param_boolean(int *add_param_value,
			      struct iscsi_param *param,
			      char *in_val, char *cur_val,
			      const char *value)
{
	char *new_val = NULL;
	/* [한국어] 결과. */

	if (add_param_value == NULL) {
		return NULL;
	}

	/* Make sure the val is Yes or No */
	if (!((strcasecmp(in_val, "Yes") == 0) ||
	      (strcasecmp(in_val, "No") == 0))) {
		/* unknown value */
		/* [한국어] Yes/No 외 입력 — Reject 응답. */
		snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", "Reject");
		new_val = in_val;
		*add_param_value = 1;
		/* [한국어] 1: caller가 set 갱신 없이 응답 buffer에만 추가. */
		return new_val;
	}

	if (strcasecmp(cur_val, value) == 0) {
		/* [한국어] 타깃 측 값이 강제 결정값(value)과 같음 — 결과를 강제. */
		snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", value);
		new_val = in_val;
	} else {
		new_val = param->val;
		/* [한국어] 그 외에는 initiator 값 채택. */
	}

	return new_val;
}

/**
 * The entry function to handle each type of the param
 * return value: the new negotiated value
 */
/*
 * [한국어]
 * iscsi_negotiate_param_all - 키 타입에 따라 적절한 협상 함수 디스패치.
 *
 * @return: 협상 결과 문자열 (in_val 또는 param->val 중 하나).
 */
static char *
iscsi_negotiate_param_all(int *add_param_value, struct iscsi_param *param,
			  char *valid_list, char *in_val, char *cur_val)
{
	char *new_val;
	/* [한국어] 결과. */
	switch (param->type) {
	case ISPT_LIST:
		new_val = iscsi_negotiate_param_list(add_param_value,
						     param,
						     valid_list,
						     in_val,
						     cur_val);
		/* [한국어] 콤마 후보 중 교집합 첫 항목. */
		break;

	case ISPT_NUMERICAL_MIN:
	case ISPT_NUMERICAL_MAX:
	case ISPT_NUMERICAL_DECLARATIVE:
		new_val = iscsi_negotiate_param_numerical(add_param_value,
				param,
				valid_list,
				in_val,
				cur_val);
		/* [한국어] 정수 협상. */
		break;

	case ISPT_BOOLEAN_OR:
		new_val = iscsi_negotiate_param_boolean(add_param_value,
							param,
							in_val,
							cur_val,
							"Yes");
		/* [한국어] OR — Yes가 결정값. */
		break;
	case ISPT_BOOLEAN_AND:
		new_val = iscsi_negotiate_param_boolean(add_param_value,
							param,
							in_val,
							cur_val,
							"No");
		/* [한국어] AND — No가 결정값. */
		break;

	default:
		/* [한국어] DECLARATIVE 또는 알 수 없는 타입 — param->val을 그대로 in_val로 복사. */
		snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", param->val);
		new_val = in_val;
		break;
	}

	return new_val;
}

/**
 * This function is used to judge whether the param is in session's params or
 * connection's params
 */
/*
 * [한국어]
 * iscsi_negotiate_param_init - 키가 conn 측인지 sess 측인지 판단하고 1회 협상 invariant 검증.
 *
 * @conn: 현재 연결.
 * @cur_param_p: 출력 — 매칭된 현재 노드.
 * @params_dst_p: in/out — 검색 시작 리스트(처음 conn->params, 미발견 시 sess->params로 자동 전환).
 * @param: initiator가 보낸 파라미터.
 * @return: 0 정상, 1 미지원 키(unknown), SPDK_ISCSI_PARAMETER_EXCHANGE_NOT_ONCE 중복 협상 위반.
 *
 * 동작: conn->params에서 먼저 검색 → 없으면 sess->params 검색. 미발견이면 X-/X# 확장 키
 * 여부 확인. 발견 시 state_negotiated 비트맵으로 1회 협상 검사 (multi_negot/target_declarative
 * 예외 키는 통과).
 */
static int
iscsi_negotiate_param_init(struct spdk_iscsi_conn *conn,
			   struct iscsi_param **cur_param_p,
			   struct iscsi_param **params_dst_p,
			   struct iscsi_param *param)
{
	int index;
	/* [한국어] state_negotiated 비트맵 인덱스. */

	*cur_param_p = iscsi_param_find(*params_dst_p, param->key);
	/* [한국어] 우선 conn->params에서 검색. */
	if (*cur_param_p == NULL) {
		/* [한국어] 없으면 sess->params로 검색 대상 전환. */
		*params_dst_p = conn->sess->params;
		*cur_param_p = iscsi_param_find(*params_dst_p, param->key);
		if (*cur_param_p == NULL) {
			/* [한국어] sess에도 없으면 미지원 키. */
			if ((strncasecmp(param->key, "X-", 2) == 0) ||
			    (strncasecmp(param->key, "X#", 2) == 0)) {
				/* Extension Key */
				/* [한국어] X-, X# 접두는 RFC 7143 §6.4 확장 키 — 무시 허용. */
				SPDK_DEBUGLOG(iscsi,
					      "extension key %.64s\n",
					      param->key);
			} else {
				SPDK_ERRLOG("unknown key %.64s\n", param->key);
			}
			return 1;
			/* [한국어] caller가 "NotUnderstood" 응답하도록 신호. */
		} else {
			/* [한국어] sess에서 찾음 — 1회 협상 검사. */
			index = (*cur_param_p)->state_index;
			if (conn->sess_param_state_negotiated[index] &&
			    !iscsi_find_key_in_array(param->key,
						     target_declarative_params)) {
				/* [한국어] 이미 협상됨 + declarative 예외 아님 = 위반. */
				return SPDK_ISCSI_PARAMETER_EXCHANGE_NOT_ONCE;
			}
			conn->sess_param_state_negotiated[index] = true;
			/* [한국어] 협상 완료 표시. */
		}
	} else {
		/* [한국어] conn에서 찾음 — 1회 협상 검사. */
		index = (*cur_param_p)->state_index;
		if (conn->conn_param_state_negotiated[index] &&
		    !iscsi_find_key_in_array(param->key,
					     multi_negot_conn_params)) {
			/* [한국어] 이미 협상됨 + multi_negot 예외 아님 = 위반. */
			return SPDK_ISCSI_PARAMETER_EXCHANGE_NOT_ONCE;
		}
		conn->conn_param_state_negotiated[index] = true;
		/* [한국어] 표시. */
	}

	return 0;
}

/*
 * [한국어]
 * iscsi_negotiate_params - 로그인 phase의 핵심: 입력 K=V 리스트를 협상하고 응답 K=V 시퀀스 생성.
 *
 * @conn: 현재 연결.
 * @params: initiator 측 입력 K=V 리스트 (parse_params 결과).
 * @data: 응답 PDU의 data 영역 buffer.
 * @alloc_len: 응답 buffer 총 크기.
 * @data_len: 응답 buffer의 현재 누적 길이 (이미 일부 채워져 있을 수 있음).
 * @return: 갱신된 누적 길이, -EINVAL/-ENOMEM/-1 또는 SPDK_ISCSI_LOGIN_ERROR_PARAMETER.
 *
 * 흐름:
 *   1) discovery 세션 여부 확인 (SessionType=Discovery).
 *   2) FirstBurstLength를 입력 리스트의 끝으로 옮겨, MaxBurstLength 협상이 먼저 끝나도록.
 *   3) 입력 K=V를 head→tail 순회하며:
 *      - SendTargets/CHAP_*은 skip.
 *      - Discovery에서 무시 키는 "Irrelevant".
 *      - iscsi_negotiate_param_init으로 conn/sess 분류 + state 검사.
 *      - iscsi_negotiate_param_all로 타입별 협상.
 *      - iscsi_construct_data_from_param/iscsi_special_param_construction으로 응답 직렬화.
 *   4) 모든 키 처리 후 갱신된 total 반환.
 *
 * 호출 체인:
 *   iscsi_op_login_response/text 처리 → [iscsi_negotiate_params]
 */
int
iscsi_negotiate_params(struct spdk_iscsi_conn *conn,
		       struct iscsi_param **params, uint8_t *data, int alloc_len,
		       int data_len)
{
	struct iscsi_param *param;
	/* [한국어] 입력 리스트 순회용. */
	struct iscsi_param *cur_param;
	/* [한국어] conn/sess 측 매칭 노드. */
	char *valid_list, *in_val;
	/* [한국어] 협상 임시 buffer (각 ISCSI_TEXT_MAX_VAL_LEN+1 바이트). */
	char *cur_val;
	/* [한국어] 현재값 임시 buffer. */
	char *new_val;
	/* [한국어] 협상 결과 (in_val 가리킴). */
	int discovery;
	/* [한국어] discovery 세션 플래그. */
	int total;
	/* [한국어] 응답 buffer 누적 길이. */
	int rc;
	/* [한국어] init 함수 결과. */
	uint32_t FirstBurstLength;
	uint32_t MaxBurstLength;
	/* [한국어] FBL/MBL 정합성 검사용. */
	bool FirstBurstLength_flag = false;
	/* [한국어] initiator가 FBL을 보냈는지 여부. */
	int type;
	/* [한국어] FBL 노드 type 캐시. */

	total = data_len;
	/* [한국어] 호출자가 이미 직렬화한 길이부터 시작. */
	if (data_len < 0) {
		/* [한국어] caller invariant 위반. */
		assert(false);
		return -EINVAL;
	}
	if (alloc_len < 1) {
		/* [한국어] buffer 없음 — 그냥 반환. */
		return 0;
	}
	if (total > alloc_len) {
		/* [한국어] 이미 초과 — 잘라서 NUL 종료 후 반환. */
		total = alloc_len;
		data[total - 1] = '\0';
		return total;
	}

	if (*params == NULL) {
		/* no input */
		/* [한국어] 입력 KV 없음 — 추가 작업 없이 반환. */
		return total;
	}

	/* discovery? */
	discovery = 0;
	/* [한국어] 기본 Normal. */
	cur_param = iscsi_param_find(*params, "SessionType");
	/* [한국어] 입력에 SessionType 있는지. */
	if (cur_param == NULL) {
		/* [한국어] 없으면 세션 측 값 확인. */
		cur_param = iscsi_param_find(conn->sess->params, "SessionType");
		if (cur_param == NULL) {
			/* no session type */
			/* [한국어] 둘 다 없음 — 기본 Normal. */
		} else {
			if (strcasecmp(cur_param->val, "Discovery") == 0) {
				discovery = 1;
			}
		}
	} else {
		if (strcasecmp(cur_param->val, "Discovery") == 0) {
			discovery = 1;
		}
	}

	/* for temporary store */
	valid_list = malloc(ISCSI_TEXT_MAX_VAL_LEN + 1);
	/* [한국어] 협상에 쓰일 list 임시 buffer. */
	if (!valid_list) {
		SPDK_ERRLOG("malloc() failed for valid_list\n");
		return -ENOMEM;
	}

	in_val = malloc(ISCSI_TEXT_MAX_VAL_LEN + 1);
	/* [한국어] in_val 임시 buffer. */
	if (!in_val) {
		SPDK_ERRLOG("malloc() failed for in_val\n");
		free(valid_list);
		return -ENOMEM;
	}

	cur_val = malloc(ISCSI_TEXT_MAX_VAL_LEN + 1);
	/* [한국어] cur_val 임시 buffer. */
	if (!cur_val) {
		SPDK_ERRLOG("malloc() failed for cur_val\n");
		free(valid_list);
		free(in_val);
		return -ENOMEM;
	}

	/* To adjust the location of FirstBurstLength location and put it to
	 *  the end, then we can always firstly determine the MaxBurstLength
	 */
	param = iscsi_param_find(*params, "MaxBurstLength");
	/* [한국어] 입력에 MaxBurstLength가 있으면 FBL 위치 조정 시도. */
	if (param != NULL) {
		param = iscsi_param_find(*params, "FirstBurstLength");

		/* check the existence of FirstBurstLength */
		if (param != NULL) {
			FirstBurstLength_flag = true;
			/* [한국어] FBL도 입력에 있음 표시. */
			if (param->next != NULL) {
				/* [한국어] FBL이 마지막이 아니면 끝으로 옮김 (값 보존을 위해 add). */
				snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", param->val);
				type = param->type;
				iscsi_param_add(params, "FirstBurstLength",
						in_val, NULL, type);
				/* [한국어] add는 기존 노드 제거 후 끝에 재삽입 — 결과적으로 FBL이 tail로 이동. */
			}
		}
	}

	for (param = *params; param != NULL; param = param->next) {
		/* [한국어] 입력 KV 순회. */
		struct iscsi_param *params_dst = conn->params;
		/* [한국어] 검색 시작 리스트 (init 함수가 sess로 전환할 수 있음). */
		int add_param_value = 0;
		/* [한국어] 0=set 갱신, 1=set 없이 응답에만 추가. */
		new_val = NULL;
		param->type = ISPT_INVALID;
		/* [한국어] 입력 노드의 type은 unknown으로 시작 (init/negotiate 과정에서 결정). */

		/* sendtargets is special */
		if (strcasecmp(param->key, "SendTargets") == 0) {
			/* [한국어] SendTargets는 본 협상기 대상이 아님 (별도 Discovery 응답). */
			continue;
		}
		/* CHAP keys */
		if (iscsi_find_key_in_array(param->key, chap_type)) {
			/* [한국어] CHAP_* 키는 인증 상태 머신에서 처리 — skip. */
			continue;
		}

		/* 12.2, 12.10, 12.11, 12.13, 12.14, 12.17, 12.18, 12.19 */
		if (discovery &&
		    iscsi_find_key_in_array(param->key, discovery_ignored_param)) {
			/* [한국어] Discovery 세션은 I/O 흐름 키를 "Irrelevant"로 응답. */
			snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", "Irrelevant");
			new_val = in_val;
			add_param_value = 1;
			/* [한국어] set 없이 응답에만 추가. */
		} else {
			rc = iscsi_negotiate_param_init(conn,
							&cur_param,
							&params_dst,
							param);
			/* [한국어] conn/sess 분류 + 1회 협상 검사. */
			if (rc < 0) {
				/* [한국어] NOT_ONCE 등 위반 — 응답 buffer 정리하고 즉시 반환. */
				free(valid_list);
				free(in_val);
				free(cur_val);
				return rc;
			} else if (rc > 0) {
				/* [한국어] 미지원 키 — "NotUnderstood" 응답. */
				snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", "NotUnderstood");
				new_val = in_val;
				add_param_value = 1;
			} else {
				/* [한국어] 정상 — 타깃 측 정보를 임시 buffer에 복사 후 협상. */
				snprintf(valid_list, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", cur_param->list);
				snprintf(cur_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", cur_param->val);
				param->type = cur_param->type;
				/* [한국어] 입력 노드에 타입 정보 복사 — construct_data에서 declarative 제외용. */
			}
		}

		if (param->type > 0) {
			/* [한국어] 정상 분류된 키만 협상 진행. */
			snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", param->val);

			/* "NotUnderstood" value shouldn't be assigned to "Understood" key */
			if (strcasecmp(in_val, "NotUnderstood") == 0) {
				/* [한국어] initiator가 정상 키에 "NotUnderstood"를 값으로 보낸 것은 프로토콜 위반. */
				free(in_val);
				free(valid_list);
				free(cur_val);
				return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
			}

			if (strcasecmp(param->key, "FirstBurstLength") == 0) {
				/* [한국어] FBL ≤ MBL 일관성 강제. */
				FirstBurstLength = (uint32_t)strtol(param->val, NULL,
								    10);
				new_val = iscsi_param_get_val(conn->sess->params,
							      "MaxBurstLength");
				if (new_val != NULL) {
					MaxBurstLength = (uint32_t) strtol(new_val, NULL,
									   10);
				} else {
					MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;
				}
				if (FirstBurstLength < SPDK_ISCSI_MAX_FIRST_BURST_LENGTH &&
				    FirstBurstLength > MaxBurstLength) {
					/* [한국어] FBL이 MBL 초과 + SPDK 상한 미만 → MBL로 캡. */
					FirstBurstLength = MaxBurstLength;
					snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN, "%d",
						 FirstBurstLength);
				}
			}

			/* prevent target's declarative params from being changed by initiator */
			if (iscsi_find_key_in_array(param->key, target_declarative_params)) {
				/* [한국어] 타깃이 일방 선언하는 키 — set 변경 금지. */
				add_param_value = 1;
			}

			new_val = iscsi_negotiate_param_all(&add_param_value,
							    param,
							    valid_list,
							    in_val,
							    cur_val);
			/* [한국어] 타입별 협상. */
		}

		/* check the negotiated value of the key */
		if (new_val != NULL) {
			/* add_param_value = 0 means updating the value of
			 *      existed key in the connection's parameters
			 */
			if (add_param_value == 0) {
				/* [한국어] 일반 협상 — 타깃 측 노드 값 갱신. */
				iscsi_param_set(params_dst, param->key, new_val);
			}
			total = iscsi_construct_data_from_param(param,
								new_val,
								data,
								alloc_len,
								total);
			/* [한국어] 비declarative 키만 응답 buffer에 직렬화. */
			if (total < 0) {
				goto final_return;
			}

			total = iscsi_special_param_construction(conn,
					param,
					FirstBurstLength_flag,
					data,
					alloc_len,
					total);
			/* [한국어] declarative 특수 키(MaxRecvDataSegmentLength 등) 추가. */
			if (total < 0) {
				goto final_return;
			}
		} else {
			/* [한국어] new_val=NULL은 reject — 협상 실패. */
			total = -1;
			break;
		}
	}

final_return:
	free(valid_list);
	free(in_val);
	free(cur_val);
	/* [한국어] 임시 buffer 정리. */

	return total;
	/* [한국어] 응답 buffer의 최종 누적 길이 또는 -1. */
}

/*
 * [한국어]
 * iscsi_copy_param2var - 협상 완료된 파라미터를 conn/sess의 정수/bool 캐시 변수로 복사.
 *
 * @conn: 대상 연결.
 * @return: 0 성공, -1 필수 키 누락.
 *
 * 핫패스(I/O)에서 매번 string→int 변환을 피하기 위해 FullFeature 진입 직전에 일괄 변환.
 * 변환 키: MaxRecvDataSegmentLength, HeaderDigest, DataDigest (conn 측),
 *   MaxConnections, MaxOutstandingR2T, FirstBurstLength, MaxBurstLength,
 *   InitialR2T, ImmediateData (sess 측).
 *
 * 호출 체인:
 *   iscsi_op_login → 로그인 단계 종료 직전 → [iscsi_copy_param2var]
 */
int
iscsi_copy_param2var(struct spdk_iscsi_conn *conn)
{
	const char *val;
	/* [한국어] iscsi_param_get_val 반환 (내부 buffer — caller free 금지). */

	val = iscsi_param_get_val(conn->params, "MaxRecvDataSegmentLength");
	/* [한국어] conn 측 키. */
	if (val == NULL) {
		SPDK_ERRLOG("Getval MaxRecvDataSegmentLength failed\n");
		return -1;
	}
	SPDK_DEBUGLOG(iscsi,
		      "copy MaxRecvDataSegmentLength=%s\n", val);
	conn->MaxRecvDataSegmentLength = (int)strtol(val, NULL, 10);
	/* [한국어] 정수 변환 — PDU 송수신 길이 검사에 사용. */
	if (conn->MaxRecvDataSegmentLength > SPDK_BDEV_LARGE_BUF_MAX_SIZE) {
		conn->MaxRecvDataSegmentLength = SPDK_BDEV_LARGE_BUF_MAX_SIZE;
		/* [한국어] SPDK bdev large buf 한도로 캡 — 메모리 보호. */
	}

	val = iscsi_param_get_val(conn->params, "HeaderDigest");
	if (val == NULL) {
		SPDK_ERRLOG("Getval HeaderDigest failed\n");
		return -1;
	}
	if (strcasecmp(val, "CRC32C") == 0) {
		SPDK_DEBUGLOG(iscsi, "set HeaderDigest=1\n");
		conn->header_digest = 1;
		/* [한국어] CRC32C 활성. */
	} else {
		SPDK_DEBUGLOG(iscsi, "set HeaderDigest=0\n");
		conn->header_digest = 0;
		/* [한국어] None. */
	}
	val = iscsi_param_get_val(conn->params, "DataDigest");
	if (val == NULL) {
		SPDK_ERRLOG("Getval DataDigest failed\n");
		return -1;
	}
	if (strcasecmp(val, "CRC32C") == 0) {
		SPDK_DEBUGLOG(iscsi, "set DataDigest=1\n");
		conn->data_digest = 1;
	} else {
		SPDK_DEBUGLOG(iscsi, "set DataDigest=0\n");
		conn->data_digest = 0;
	}

	val = iscsi_param_get_val(conn->sess->params, "MaxConnections");
	/* [한국어] sess 측 키들. */
	if (val == NULL) {
		SPDK_ERRLOG("Getval MaxConnections failed\n");
		return -1;
	}
	SPDK_DEBUGLOG(iscsi, "copy MaxConnections=%s\n", val);
	conn->sess->MaxConnections = (uint32_t) strtol(val, NULL, 10);
	/* [한국어] 세션 내 최대 connection 수 — 추가 connection login 검사. */
	val = iscsi_param_get_val(conn->sess->params, "MaxOutstandingR2T");
	if (val == NULL) {
		SPDK_ERRLOG("Getval MaxOutstandingR2T failed\n");
		return -1;
	}
	SPDK_DEBUGLOG(iscsi, "copy MaxOutstandingR2T=%s\n", val);
	conn->sess->MaxOutstandingR2T = (uint32_t) strtol(val, NULL, 10);
	/* [한국어] 동시에 outstanding 가능한 R2T 수. */
	val = iscsi_param_get_val(conn->sess->params, "FirstBurstLength");
	if (val == NULL) {
		SPDK_ERRLOG("Getval FirstBurstLength failed\n");
		return -1;
	}
	SPDK_DEBUGLOG(iscsi, "copy FirstBurstLength=%s\n", val);
	conn->sess->FirstBurstLength = (uint32_t) strtol(val, NULL, 10);
	/* [한국어] 첫 burst 길이. */
	val = iscsi_param_get_val(conn->sess->params, "MaxBurstLength");
	if (val == NULL) {
		SPDK_ERRLOG("Getval MaxBurstLength failed\n");
		return -1;
	}
	SPDK_DEBUGLOG(iscsi, "copy MaxBurstLength=%s\n", val);
	conn->sess->MaxBurstLength = (uint32_t) strtol(val, NULL, 10);
	/* [한국어] 한 burst의 최대 길이. */
	val = iscsi_param_get_val(conn->sess->params, "InitialR2T");
	if (val == NULL) {
		SPDK_ERRLOG("Getval InitialR2T failed\n");
		return -1;
	}
	if (strcasecmp(val, "Yes") == 0) {
		SPDK_DEBUGLOG(iscsi, "set InitialR2T=1\n");
		conn->sess->InitialR2T = true;
		/* [한국어] R2T 사용 강제. */
	} else {
		SPDK_DEBUGLOG(iscsi, "set InitialR2T=0\n");
		conn->sess->InitialR2T = false;
	}
	val = iscsi_param_get_val(conn->sess->params, "ImmediateData");
	if (val == NULL) {
		SPDK_ERRLOG("Getval ImmediateData failed\n");
		return -1;
	}
	if (strcasecmp(val, "Yes") == 0) {
		SPDK_DEBUGLOG(iscsi, "set ImmediateData=1\n");
		conn->sess->ImmediateData = true;
		/* [한국어] Immediate Data 허용. */
	} else {
		SPDK_DEBUGLOG(iscsi, "set ImmediateData=0\n");
		conn->sess->ImmediateData = false;
	}
	return 0;
	/* [한국어] 모든 키 복사 완료. */
}
