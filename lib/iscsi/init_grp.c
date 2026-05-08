/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI Initiator Group 관리 구현 (init_grp.c)
 *
 * === 파일의 역할 ===
 * iSCSI 타깃의 ACL을 구성하는 Initiator Group(IG)에 대한 모든 mutating 연산을 구현한다.
 * 즉 (1) IG 생성/소멸, (2) 그룹 내 이름·넷마스크 노드 추가/제거, (3) 전역 IG 테이블
 * 등록/해제 검색, (4) JSON-RPC 직렬화를 담당한다. 모든 변경 연산은 g_iscsi.mutex로
 * 직렬화되어 RPC 스레드와 로그인 검사 경로 사이의 race를 방지한다. RFC 3720 §12 ACL
 * 모델의 SPDK 구현체이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   RPC 핸들러 (iscsi_rpc.c) → iscsi_init_grp_create_from_initiator_list()
 *     → iscsi_init_grp_create() → iscsi_init_grp_add_initiators()
 *     → iscsi_init_grp_add_netmasks() → iscsi_init_grp_register()
 *     → 전역 g_iscsi.ig_head TAILQ에 등록.
 * 셧다운 경로: spdk_iscsi_fini() → iscsi_init_grps_destroy() →
 *   각 IG에 iscsi_init_grp_destroy() (이름/넷마스크 free + IG free).
 * 실행 컨텍스트: SPDK init/fini thread 또는 RPC 처리 thread (모두 메인 reactor).
 * 데이터 플레인(I/O)와 격리되어 있으며 락 보호 하에서만 변경.
 *
 * === 타 모듈과의 연결 ===
 * - iscsi.h: g_iscsi 전역 인스턴스, MAX_INITIATOR/MAX_NETMASK/MAX_INITIATOR_NAME 상수.
 * - init_grp.h: 자료구조와 외부 API 선언.
 * - tgt_node.c: 본 파일에서 등록한 IG를 (PG, IG) 매핑으로 참조하여 ACL 검사.
 * - JSON RPC: spdk_json_write_ctx로 IG 정보를 직렬화.
 * 데이터 흐름: RPC 입력 (이름·넷마스크 char*[]) → calloc된 spdk_iscsi_init_grp 노드 →
 *   g_iscsi.ig_head 등록 → 로그인 시 conn 매칭에 사용 → fini 시 free.
 *
 * === 주요 함수/구조체 요약 ===
 * - iscsi_init_grp_create(): tag로 빈 IG calloc 후 TAILQ 초기화.
 * - iscsi_init_grp_add/delete_initiator(): 단일 IQN 노드 추가/삭제 (검사 + 메모리 관리).
 * - iscsi_init_grp_add/delete_netmask(): 단일 넷마스크 노드 추가/삭제.
 * - iscsi_init_grp_add_initiators/netmasks(): 배열 단위 추가, 실패 시 부분 롤백.
 * - iscsi_init_grp_register/unregister(): 전역 ig_head 등록/제거 (g_iscsi.mutex 보호).
 * - iscsi_init_grp_create_from_initiator_list(): RPC 진입점, create + add + register.
 * - iscsi_init_grp_destroy/iscsi_init_grps_destroy(): 단일/전체 IG 정리.
 * - iscsi_init_grp_info_json/config_json(): JSON-RPC 직렬화.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 라이브러리(POSIX) 일괄 include. SPDK 코드 컨벤션 (calloc, strlen, errno 등). */

#include "spdk/string.h"
/* [한국어] spdk_strerror() 등 SPDK 문자열 유틸. (본 파일에서는 직접 사용 안 하나 일반적). */

#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG/SPDK_WARNLOG/SPDK_DEBUGLOG 매크로. 진단 메시지 출력에 사용. */

#include "iscsi/iscsi.h"
/* [한국어] 전역 g_iscsi 및 iSCSI 도메인 상수 정의. */
#include "iscsi/init_grp.h"
/* [한국어] 본 파일에서 구현하는 외부 API의 선언. */

/*
 * [한국어]
 * iscsi_init_grp_create - 빈 IG 객체 1개를 calloc하여 초기화한다.
 *
 * @tag: 호출자가 지정한 IG 식별자. 양의 정수가 일반적.
 * @return: 새로 만들어진 spdk_iscsi_init_grp 포인터, calloc 실패 시 NULL.
 *
 * 본 함수는 IG 자료구조를 0으로 채운 뒤 TAILQ 헤드만 초기화하여 반환한다.
 * 등록(g_iscsi.ig_head 삽입)은 호출자가 iscsi_init_grp_register()로 별도 수행해야 한다.
 * 실행 컨텍스트: RPC 처리 thread (g_iscsi.mutex 미보유 상태에서 호출되어도 무방 — 아직
 * 전역에 노출되지 않은 객체이므로 race 없음).
 *
 * 호출 체인:
 *   iscsi_init_grp_create_from_initiator_list() → [iscsi_init_grp_create] → calloc
 */
static struct spdk_iscsi_init_grp *
iscsi_init_grp_create(int tag)
{
	struct spdk_iscsi_init_grp *ig;
	/* [한국어] 새로 생성할 IG 객체 포인터. 성공 시 호출자에게 반환. */

	ig = calloc(1, sizeof(*ig));
	/* [한국어] 0으로 초기화된 IG 메모리 할당. ref/ninitiators/nnetmasks가 0으로 시작. */
	if (ig == NULL) {
		/* [한국어] 메모리 부족: SPDK_ERRLOG로 진단 후 NULL 반환. 호출자는 -ENOMEM 처리. */
		SPDK_ERRLOG("calloc() failed for initiator group\n");
		return NULL;
	}

	ig->tag = tag;
	/* [한국어] 식별자 설정. 이후 변경되지 않음 (find_by_tag 키). */
	TAILQ_INIT(&ig->initiator_head);
	/* [한국어] 이름 노드 리스트 헤드 초기화 (빈 리스트). */
	TAILQ_INIT(&ig->netmask_head);
	/* [한국어] 넷마스크 노드 리스트 헤드 초기화 (빈 리스트). */
	return ig;
	/* [한국어] 등록 전 단계의 IG 객체 반환. 호출자가 register 또는 destroy 책임. */
}

/*
 * [한국어]
 * iscsi_init_grp_find_initiator - IG의 이름 리스트에서 특정 IQN 노드를 찾는다.
 *
 * @ig: 검색 대상 IG. NULL 불가.
 * @name: 찾을 IQN 문자열 (NUL-terminated).
 * @return: 일치 노드 포인터 또는 NULL.
 *
 * O(N) 선형 탐색. 일반적으로 한 IG의 이름 수는 적기 때문에 충분히 빠르다.
 * 호출자가 g_iscsi.mutex를 잡은 상태에서 호출해야 일관된 결과를 보장.
 *
 * 호출 체인:
 *   iscsi_init_grp_add_initiator()/delete_initiator() → [find_initiator] → strcmp
 */
static struct spdk_iscsi_initiator_name *
iscsi_init_grp_find_initiator(struct spdk_iscsi_init_grp *ig, char *name)
{
	struct spdk_iscsi_initiator_name *iname;
	/* [한국어] 순회 루프 변수. 일치 시 그대로 반환. */

	TAILQ_FOREACH(iname, &ig->initiator_head, tailq) {
		/* [한국어] IG의 이름 리스트를 헤드부터 끝까지 순회. */
		if (!strcmp(iname->name, name)) {
			/* [한국어] 정확히 일치하면 즉시 반환 (중복 추가 검출 등에 사용). */
			return iname;
		}
	}
	return NULL;
	/* [한국어] 끝까지 못 찾으면 NULL — 신규 노드 추가 가능을 의미. */
}

/*
 * [한국어]
 * iscsi_init_grp_add_initiator - IG에 IQN 이름 노드 1개를 추가한다.
 *
 * @ig: 추가 대상 IG.
 * @name: 추가할 IQN 문자열.
 * @return: 0 성공, -EPERM(MAX 초과)/-EINVAL(이름 길이 초과)/-EEXIST(중복)/-ENOMEM.
 *
 * 동작: (1) 개수/길이 검증 → (2) 중복 검사 → (3) calloc → (4) 문자열 복사 →
 * (5) "ALL"→"ANY" 자동 변환 (deprecated 키워드 호환) → (6) TAILQ 끝 삽입 + 카운터 증가.
 * 실행 컨텍스트: RPC thread, g_iscsi.mutex 보유 상태로 호출되어야 함 (상위에서 보장).
 *
 * 호출 체인:
 *   iscsi_init_grp_add_initiators() → [add_initiator] → calloc/strstr/TAILQ_INSERT_TAIL
 */
static int
iscsi_init_grp_add_initiator(struct spdk_iscsi_init_grp *ig, char *name)
{
	struct spdk_iscsi_initiator_name *iname;
	/* [한국어] 새로 만들 (또는 기존) 이름 노드 포인터. */
	char *p;
	/* [한국어] strstr 결과 — "ALL" 부분 문자열 위치 (있으면 in-place 치환). */
	size_t len;
	/* [한국어] 입력 name의 길이 캐시. memcpy 길이로도 사용. */

	if (ig->ninitiators >= MAX_INITIATOR) {
		/* [한국어] IG당 이름 상한(=256) 도달: 이건 관리적 한도이므로 EPERM. */
		SPDK_ERRLOG("> MAX_INITIATOR(=%d) is not allowed\n", MAX_INITIATOR);
		return -EPERM;
	}

	len = strlen(name);
	/* [한국어] 입력 길이 측정. 이후 검증/복사에 동일하게 사용. */
	if (len > MAX_INITIATOR_NAME) {
		/* [한국어] RFC 3720 IQN 최대 223자 초과: 사용자 오류 → -EINVAL. */
		SPDK_ERRLOG("Initiator Name is larger than 223 bytes\n");
		return -EINVAL;
	}

	iname = iscsi_init_grp_find_initiator(ig, name);
	/* [한국어] 이미 같은 이름이 있는지 확인 (idempotent 보장). */
	if (iname != NULL) {
		/* [한국어] 중복: 호출자에게 -EEXIST 반환하여 RPC가 적절히 응답. */
		return -EEXIST;
	}

	iname = calloc(1, sizeof(*iname));
	/* [한국어] 새 이름 노드 0-초기화 할당. */
	if (iname == NULL) {
		/* [한국어] 메모리 부족: SPDK_ERRLOG 후 -ENOMEM. */
		SPDK_ERRLOG("malloc() failed for initiator name str\n");
		return -ENOMEM;
	}

	memcpy(iname->name, name, len);
	/* [한국어] 이름을 노드 버퍼로 복사. NUL은 calloc으로 이미 0이므로 별도 처리 불필요. */

	/* Replace "ALL" by "ANY" if set */
	p = strstr(iname->name, "ALL");
	/* [한국어] 레거시 와일드카드 "ALL" 위치 검색 — 발견 시 in-place로 "ANY"로 교체. */
	if (p != NULL) {
		SPDK_WARNLOG("Please use \"%s\" instead of \"%s\"\n", "ANY", "ALL");
		/* [한국어] 사용자에게 권장 키워드 안내. */
		SPDK_WARNLOG("Converting \"%s\" to \"%s\" automatically\n", "ALL", "ANY");
		/* [한국어] 자동 변환 사실 통지 (디버깅 시 추적 용이). */
		memcpy(p, "ANY", 3);
		/* [한국어] 길이 같은 3바이트 in-place 교체. NUL 변경 없음. */
	}

	TAILQ_INSERT_TAIL(&ig->initiator_head, iname, tailq);
	/* [한국어] 노드를 IG 이름 리스트의 끝에 삽입. 순서가 의미를 가지지 않음. */
	ig->ninitiators++;
	/* [한국어] 캐시 카운터 증가. MAX_INITIATOR 검증과 동기 유지. */

	SPDK_DEBUGLOG(iscsi, "InitiatorName %s\n", name);
	/* [한국어] 디버그 트레이스: 어느 이름이 추가됐는지 기록. */
	return 0;
	/* [한국어] 성공. */
}

/*
 * [한국어]
 * iscsi_init_grp_delete_initiator - IG에서 특정 IQN 노드 1개를 제거한다.
 *
 * @ig: 대상 IG.
 * @name: 제거할 IQN 문자열.
 * @return: 0 성공, -ENOENT(존재하지 않음).
 *
 * 동작: find로 노드 위치 확인 → TAILQ_REMOVE → 카운터 감소 → free.
 * 실행 컨텍스트: g_iscsi.mutex 보유 상태에서 호출.
 *
 * 호출 체인:
 *   iscsi_init_grp_delete_initiators() → [delete_initiator] → free
 */
static int
iscsi_init_grp_delete_initiator(struct spdk_iscsi_init_grp *ig, char *name)
{
	struct spdk_iscsi_initiator_name *iname;
	/* [한국어] 제거할 노드 포인터 (find 결과). */

	iname = iscsi_init_grp_find_initiator(ig, name);
	/* [한국어] 이름으로 노드 검색. */
	if (iname == NULL) {
		/* [한국어] 존재하지 않는 이름은 -ENOENT. */
		return -ENOENT;
	}

	TAILQ_REMOVE(&ig->initiator_head, iname, tailq);
	/* [한국어] 리스트에서 분리. */
	ig->ninitiators--;
	/* [한국어] 카운터 감소. */
	free(iname);
	/* [한국어] 노드 메모리 반환. */
	return 0;
	/* [한국어] 성공. */
}

/*
 * [한국어]
 * iscsi_init_grp_add_initiators - IG에 이름 배열을 일괄 추가, 실패 시 롤백.
 *
 * @ig: 대상 IG.
 * @num_inames: inames 배열 길이.
 * @inames: NULL 종료 IQN 문자열 배열.
 * @return: 0 성공, 첫 실패의 errno 음수 (롤백 후 반환).
 *
 * 트랜잭셔널 의미: 부분 추가 성공 후 실패하면 이미 추가한 항목을 역순으로 제거하여
 * "all-or-nothing"을 보장한다. RPC 사용자에게 일관된 상태를 보여주기 위함.
 *
 * 호출 체인:
 *   iscsi_init_grp_create_from_initiator_list()/add_initiators_from_initiator_list()
 *     → [add_initiators] → add_initiator (반복) → cleanup: delete_initiator (역순)
 */
static int
iscsi_init_grp_add_initiators(struct spdk_iscsi_init_grp *ig, int num_inames,
			      char **inames)
{
	int i;
	/* [한국어] 루프 인덱스 (cleanup에서도 사용되므로 함수 스코프에 둠). */
	int rc;
	/* [한국어] 각 add_initiator 호출의 반환 코드 캐시. */

	for (i = 0; i < num_inames; i++) {
		/* [한국어] 입력 배열을 순서대로 IG에 추가 시도. */
		rc = iscsi_init_grp_add_initiator(ig, inames[i]);
		if (rc < 0) {
			/* [한국어] 어느 하나라도 실패 → 트랜잭션 취소를 위해 cleanup 점프. */
			goto cleanup;
		}
	}
	return 0;
	/* [한국어] 모두 성공. */

cleanup:
	for (; i > 0; --i) {
		/* [한국어] 실패 직전까지 추가된 i-1개 항목을 역순으로 제거 (rollback). */
		iscsi_init_grp_delete_initiator(ig, inames[i - 1]);
	}
	return rc;
	/* [한국어] 첫 실패의 errno를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * iscsi_init_grp_delete_all_initiators - IG의 이름 노드를 전부 free한다.
 *
 * @ig: 대상 IG (NULL 불가).
 *
 * destroy 경로의 헬퍼. TAILQ_FOREACH_SAFE로 안전하게 순회하며 모든 노드를 free.
 * 실패하지 않음 (메모리 free만 수행).
 *
 * 호출 체인:
 *   iscsi_init_grp_destroy()/iscsi_init_grp_delete_initiators(cleanup)
 *     → [delete_all_initiators]
 */
static void
iscsi_init_grp_delete_all_initiators(struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_initiator_name *iname, *tmp;
	/* [한국어] 현재 노드와 다음 노드 임시 보관용 (FOREACH_SAFE 패턴). */

	TAILQ_FOREACH_SAFE(iname, &ig->initiator_head, tailq, tmp) {
		/* [한국어] 순회 중 노드를 free해도 안전하도록 SAFE variant 사용. */
		TAILQ_REMOVE(&ig->initiator_head, iname, tailq);
		/* [한국어] 리스트에서 분리. */
		ig->ninitiators--;
		/* [한국어] 카운터 감소 (모두 끝나면 0). */
		free(iname);
		/* [한국어] 노드 메모리 해제. */
	}
}

/*
 * [한국어]
 * iscsi_init_grp_delete_initiators - IG에서 이름 배열을 일괄 제거, 실패 시 롤백 시도.
 *
 * @ig: 대상 IG.
 * @num_inames: inames 길이.
 * @inames: 제거할 IQN 배열.
 * @return: 0 성공, -1 실패 (롤백도 실패하면 IG 이름이 전부 비어 있을 수 있음).
 *
 * 동작: 순서대로 delete 시도 → 실패 시 이미 삭제한 항목들을 다시 add (rollback).
 * 만약 rollback 도중 다시 실패하면 일관성을 포기하고 모든 이름을 정리(delete_all)한다 —
 * 부분 일관성보다 깨끗한 상태가 안전하다는 판단.
 *
 * 호출 체인:
 *   iscsi_init_grp_delete_initiators_from_initiator_list()
 *     → [delete_initiators] → delete_initiator (반복) → cleanup: add_initiator
 */
static int
iscsi_init_grp_delete_initiators(struct spdk_iscsi_init_grp *ig, int num_inames, char **inames)
{
	int i;
	/* [한국어] 루프 인덱스. */
	int rc;
	/* [한국어] 각 호출의 반환 코드. */

	for (i = 0; i < num_inames; i++) {
		/* [한국어] 입력 순서대로 IG에서 이름 제거 시도. */
		rc = iscsi_init_grp_delete_initiator(ig, inames[i]);
		if (rc < 0) {
			/* [한국어] 어느 하나라도 실패하면 롤백. */
			goto cleanup;
		}
	}
	return 0;
	/* [한국어] 모두 성공. */

cleanup:
	for (; i > 0; --i) {
		/* [한국어] 직전까지 삭제된 항목을 역순으로 다시 추가하여 원상 복구 시도. */
		rc = iscsi_init_grp_add_initiator(ig, inames[i - 1]);
		if (rc != 0) {
			/* [한국어] 롤백조차 실패: 일관성을 포기하고 전체 이름을 비운다 (안전한 상태). */
			iscsi_init_grp_delete_all_initiators(ig);
			break;
		}
	}
	return -1;
	/* [한국어] 일관된 실패 코드 (-1). 호출자는 IG 상태를 다시 검사할 책임. */
}

/*
 * [한국어]
 * iscsi_init_grp_find_netmask - IG의 넷마스크 리스트에서 일치 노드 검색.
 *
 * @ig: 대상 IG.
 * @mask: 찾을 넷마스크 문자열.
 * @return: 일치 노드 또는 NULL.
 *
 * 이름 검색과 동일한 패턴(O(N) 선형). 호출자 락 보유 가정.
 *
 * 호출 체인:
 *   add_netmask()/delete_netmask() → [find_netmask] → strcmp
 */
static struct spdk_iscsi_initiator_netmask *
iscsi_init_grp_find_netmask(struct spdk_iscsi_init_grp *ig, const char *mask)
{
	struct spdk_iscsi_initiator_netmask *netmask;
	/* [한국어] 순회 루프 변수. */

	TAILQ_FOREACH(netmask, &ig->netmask_head, tailq) {
		/* [한국어] IG의 넷마스크 리스트 순회. */
		if (!strcmp(netmask->mask, mask)) {
			/* [한국어] 문자열 정확 매치 — 정규화는 하지 않음(예: 슬래시 위치 등은 호출자 책임). */
			return netmask;
		}
	}
	return NULL;
	/* [한국어] 미발견. */
}

/*
 * [한국어]
 * iscsi_init_grp_add_netmask - IG에 넷마스크 노드 1개를 추가한다.
 *
 * @ig: 대상 IG.
 * @mask: 추가할 넷마스크 문자열 (예: "10.0.0.0/24").
 * @return: 0 성공, -EPERM(MAX 초과)/-EINVAL(길이 초과)/-EEXIST/-ENOMEM.
 *
 * add_initiator와 같은 형태의 검증·할당·"ALL"→"ANY" 변환·삽입을 수행한다.
 * 실행 컨텍스트: RPC thread, g_iscsi.mutex 보유.
 *
 * 호출 체인:
 *   iscsi_init_grp_add_netmasks() → [add_netmask]
 */
static int
iscsi_init_grp_add_netmask(struct spdk_iscsi_init_grp *ig, char *mask)
{
	struct spdk_iscsi_initiator_netmask *imask;
	/* [한국어] 새/기존 넷마스크 노드 포인터. */
	char *p;
	/* [한국어] strstr 결과 — "ALL" 위치 (있으면 in-place 변환). */
	size_t len;
	/* [한국어] 입력 mask 길이. */

	if (ig->nnetmasks >= MAX_NETMASK) {
		/* [한국어] IG당 넷마스크 상한(=256) 도달. */
		SPDK_ERRLOG("> MAX_NETMASK(=%d) is not allowed\n", MAX_NETMASK);
		return -EPERM;
	}

	len = strlen(mask);
	/* [한국어] 길이 측정. */
	if (len > MAX_INITIATOR_ADDR) {
		/* [한국어] 주소 문자열 최대 64자 초과: 사용자 오류 → -EINVAL.
		 * 메시지가 "Initiator Name is..."로 적힌 것은 원본 코드 문구 그대로 (오타로 보이지만 수정하지 않음). */
		SPDK_ERRLOG("Initiator Name is larger than %d bytes\n", MAX_INITIATOR_ADDR);
		return -EINVAL;
	}

	imask = iscsi_init_grp_find_netmask(ig, mask);
	/* [한국어] 중복 검사. */
	if (imask != NULL) {
		/* [한국어] 이미 존재하는 마스크: idempotent를 위해 -EEXIST. */
		return -EEXIST;
	}

	imask = calloc(1, sizeof(*imask));
	/* [한국어] 새 노드 0-초기화 할당. */
	if (imask == NULL) {
		/* [한국어] 메모리 부족. */
		SPDK_ERRLOG("malloc() failed for initiator mask str\n");
		return -ENOMEM;
	}

	memcpy(imask->mask, mask, len);
	/* [한국어] 노드 버퍼로 마스크 문자열 복사. */

	/* Replace "ALL" by "ANY" if set */
	p = strstr(imask->mask, "ALL");
	/* [한국어] 레거시 키워드 검색. */
	if (p != NULL) {
		SPDK_WARNLOG("Please use \"%s\" instead of \"%s\"\n", "ANY", "ALL");
		/* [한국어] 권장 키워드 안내. */
		SPDK_WARNLOG("Converting \"%s\" to \"%s\" automatically\n", "ALL", "ANY");
		/* [한국어] 자동 변환 통지. */
		memcpy(p, "ANY", 3);
		/* [한국어] 3바이트 in-place 치환. */
	}

	TAILQ_INSERT_TAIL(&ig->netmask_head, imask, tailq);
	/* [한국어] 넷마스크 리스트 끝에 삽입. */
	ig->nnetmasks++;
	/* [한국어] 카운터 증가. */

	SPDK_DEBUGLOG(iscsi, "Netmask %s\n", mask);
	/* [한국어] 디버그 트레이스. */
	return 0;
	/* [한국어] 성공. */
}

/*
 * [한국어]
 * iscsi_init_grp_delete_netmask - IG에서 특정 넷마스크 노드 1개 제거.
 *
 * @ig: 대상 IG.
 * @mask: 제거할 넷마스크 문자열.
 * @return: 0 성공, -ENOENT.
 *
 * delete_initiator의 넷마스크 버전. 동일 패턴.
 */
static int
iscsi_init_grp_delete_netmask(struct spdk_iscsi_init_grp *ig, char *mask)
{
	struct spdk_iscsi_initiator_netmask *imask;
	/* [한국어] 제거할 노드. */

	imask = iscsi_init_grp_find_netmask(ig, mask);
	/* [한국어] 검색. */
	if (imask == NULL) {
		/* [한국어] 미존재. */
		return -ENOENT;
	}

	TAILQ_REMOVE(&ig->netmask_head, imask, tailq);
	/* [한국어] 분리. */
	ig->nnetmasks--;
	/* [한국어] 카운터 감소. */
	free(imask);
	/* [한국어] 메모리 반환. */
	return 0;
	/* [한국어] 성공. */
}

/*
 * [한국어]
 * iscsi_init_grp_add_netmasks - 넷마스크 배열 일괄 추가, 실패 시 롤백.
 *
 * add_initiators의 넷마스크 버전. 트랜잭셔널 의미 동일.
 */
static int
iscsi_init_grp_add_netmasks(struct spdk_iscsi_init_grp *ig, int num_imasks, char **imasks)
{
	int i;
	/* [한국어] 루프 인덱스. */
	int rc;
	/* [한국어] 반환 코드. */

	for (i = 0; i < num_imasks; i++) {
		/* [한국어] 입력 순서대로 추가 시도. */
		rc = iscsi_init_grp_add_netmask(ig, imasks[i]);
		if (rc != 0) {
			/* [한국어] 실패 시 롤백. */
			goto cleanup;
		}
	}
	return 0;
	/* [한국어] 모두 성공. */

cleanup:
	for (; i > 0; --i) {
		/* [한국어] 역순 제거. */
		iscsi_init_grp_delete_netmask(ig, imasks[i - 1]);
	}
	return rc;
	/* [한국어] 첫 실패 코드 그대로. */
}

/*
 * [한국어]
 * iscsi_init_grp_delete_all_netmasks - IG의 넷마스크 노드를 전부 free.
 *
 * destroy 경로 헬퍼. delete_all_initiators의 넷마스크 버전.
 */
static void
iscsi_init_grp_delete_all_netmasks(struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_initiator_netmask *imask, *tmp;
	/* [한국어] FOREACH_SAFE용 임시 변수. */

	TAILQ_FOREACH_SAFE(imask, &ig->netmask_head, tailq, tmp) {
		/* [한국어] 순회 중 free 안전한 SAFE variant. */
		TAILQ_REMOVE(&ig->netmask_head, imask, tailq);
		/* [한국어] 리스트 분리. */
		ig->nnetmasks--;
		/* [한국어] 카운터 감소. */
		free(imask);
		/* [한국어] 메모리 반환. */
	}
}

/*
 * [한국어]
 * iscsi_init_grp_delete_netmasks - 넷마스크 배열 일괄 제거, 실패 시 롤백 + 안전 폴백.
 *
 * delete_initiators의 넷마스크 버전. 롤백 실패 시 모든 넷마스크를 비워 일관 상태로 만듦.
 */
static int
iscsi_init_grp_delete_netmasks(struct spdk_iscsi_init_grp *ig, int num_imasks, char **imasks)
{
	int i;
	/* [한국어] 루프 인덱스. */
	int rc;
	/* [한국어] 반환 코드. */

	for (i = 0; i < num_imasks; i++) {
		/* [한국어] 입력 순서대로 삭제 시도. */
		rc = iscsi_init_grp_delete_netmask(ig, imasks[i]);
		if (rc != 0) {
			/* [한국어] 실패 시 롤백 점프. */
			goto cleanup;
		}
	}
	return 0;
	/* [한국어] 모두 성공. */

cleanup:
	for (; i > 0; --i) {
		/* [한국어] 직전까지 삭제된 항목을 역순 add로 복구 시도. */
		rc = iscsi_init_grp_add_netmask(ig, imasks[i - 1]);
		if (rc != 0) {
			/* [한국어] 복구 실패: 안전을 위해 모든 넷마스크 폐기. */
			iscsi_init_grp_delete_all_netmasks(ig);
			break;
		}
	}
	return -1;
	/* [한국어] 일관된 실패 코드. */
}

/*
 * [한국어]
 * iscsi_init_grp_register - IG를 전역 g_iscsi.ig_head에 등록한다.
 *
 * @ig: 등록할 IG 포인터 (NULL 불가).
 * @return: 0 성공, -1 (이미 같은 tag 존재).
 *
 * 동작: g_iscsi.mutex 획득 → tag 중복 검사 → 없으면 ig_head 끝에 삽입 → 락 해제.
 * 실행 컨텍스트: RPC thread. 이 시점부터 IG는 다른 경로(로그인 검사)에서도 보일 수 있다.
 *
 * 호출 체인:
 *   iscsi_init_grp_create_from_initiator_list() → [register] → TAILQ_INSERT_TAIL
 */
int
iscsi_init_grp_register(struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_init_grp *tmp;
	/* [한국어] 중복 검사용 임시 포인터. */
	int rc = -1;
	/* [한국어] 기본값 -1 (실패) — 중복 시 그대로 반환. */

	assert(ig != NULL);
	/* [한국어] 인자 사전 조건 검증 (디버그 빌드). */

	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] 전역 ig_head 보호 락 획득. */
	tmp = iscsi_init_grp_find_by_tag(ig->tag);
	/* [한국어] 같은 tag의 IG가 이미 등록되어 있는지 확인. */
	if (tmp == NULL) {
		/* [한국어] 미존재 → 안전하게 삽입. */
		TAILQ_INSERT_TAIL(&g_iscsi.ig_head, ig, tailq);
		rc = 0;
		/* [한국어] 성공 표시. */
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	/* [한국어] 락 해제. */

	return rc;
	/* [한국어] 0 또는 -1 (호출자가 destroy 또는 진행). */
}

/*
 * Create initiator group from list of initiator ip/hostnames and netmasks
 * The initiator hostname/netmask lists are allocated by the caller on the
 * heap.  Freed later by common initiator_group_destroy() code
 */
/*
 * [한국어]
 * iscsi_init_grp_create_from_initiator_list - RPC 진입점, IG 신규 생성+채움+등록 일괄 수행.
 *
 * @tag: 신규 IG 식별자.
 * @num_initiator_names: initiator_names 길이.
 * @initiator_names: IQN 문자열 배열 (caller heap, 본 함수에서 복사).
 * @num_initiator_masks: initiator_masks 길이.
 * @initiator_masks: 넷마스크 문자열 배열 (caller heap, 본 함수에서 복사).
 * @return: 0 성공, -1 어떤 단계든 실패.
 *
 * 단계: create → add_initiators → add_netmasks → register. 어느 단계에서 실패해도
 * cleanup으로 점프하여 IG와 자식 노드를 모두 destroy 한다 (atomicity 보장).
 * 입력 문자열은 caller가 heap 할당했지만 본 함수가 그 포인터를 보관하지 않고 내용을
 * 복사하므로 caller가 자유롭게 free 가능.
 *
 * 호출 체인:
 *   RPC iscsi_create_initiator_group → [create_from_initiator_list]
 *     → create → add_initiators → add_netmasks → register
 */
int
iscsi_init_grp_create_from_initiator_list(int tag,
		int num_initiator_names,
		char **initiator_names,
		int num_initiator_masks,
		char **initiator_masks)
{
	int rc = -1;
	/* [한국어] 기본 실패 반환값. */
	struct spdk_iscsi_init_grp *ig = NULL;
	/* [한국어] 새로 만들 IG 포인터. */

	SPDK_DEBUGLOG(iscsi,
		      "add initiator group (from initiator list) tag=%d, #initiators=%d, #masks=%d\n",
		      tag, num_initiator_names, num_initiator_masks);
	/* [한국어] RPC 입력 요약 디버그 로그. */

	ig = iscsi_init_grp_create(tag);
	/* [한국어] 빈 IG 생성. */
	if (!ig) {
		/* [한국어] 메모리 부족. */
		SPDK_ERRLOG("initiator group create error (%d)\n", tag);
		return rc;
	}

	rc = iscsi_init_grp_add_initiators(ig, num_initiator_names,
					   initiator_names);
	/* [한국어] 이름 일괄 추가 (트랜잭셔널). */
	if (rc < 0) {
		SPDK_ERRLOG("add initiator name error\n");
		goto cleanup;
	}

	rc = iscsi_init_grp_add_netmasks(ig, num_initiator_masks,
					 initiator_masks);
	/* [한국어] 넷마스크 일괄 추가. */
	if (rc < 0) {
		SPDK_ERRLOG("add initiator netmask error\n");
		goto cleanup;
	}

	rc = iscsi_init_grp_register(ig);
	/* [한국어] 전역 등록 (tag 중복 검사 포함). */
	if (rc < 0) {
		SPDK_ERRLOG("initiator group register error (%d)\n", tag);
		goto cleanup;
	}
	return 0;
	/* [한국어] 모든 단계 성공. */

cleanup:
	iscsi_init_grp_destroy(ig);
	/* [한국어] 어느 단계든 실패 시 IG와 자식 노드 전부 free. */
	return rc;
	/* [한국어] 첫 실패의 errno 또는 -1. */
}

/*
 * [한국어]
 * iscsi_init_grp_add_initiators_from_initiator_list - 기존 IG에 이름·넷마스크 추가.
 *
 * @tag: 기존 IG 식별자.
 * @num_initiator_names/initiator_names: 추가할 이름 배열.
 * @num_initiator_masks/initiator_masks: 추가할 넷마스크 배열.
 * @return: 0 성공, -1 실패.
 *
 * 동작: g_iscsi.mutex 보유 상태에서 find_by_tag → add_initiators → add_netmasks.
 * 넷마스크 추가가 실패하면 이름 추가도 롤백하여 일관성 보장.
 *
 * 호출 체인:
 *   RPC iscsi_initiator_group_add_initiators → [add_initiators_from_initiator_list]
 */
int
iscsi_init_grp_add_initiators_from_initiator_list(int tag,
		int num_initiator_names,
		char **initiator_names,
		int num_initiator_masks,
		char **initiator_masks)
{
	int rc = -1;
	/* [한국어] 반환 코드. */
	struct spdk_iscsi_init_grp *ig;
	/* [한국어] find_by_tag 결과 IG. */

	SPDK_DEBUGLOG(iscsi,
		      "add initiator to initiator group: tag=%d, #initiators=%d, #masks=%d\n",
		      tag, num_initiator_names, num_initiator_masks);
	/* [한국어] 입력 요약 디버그. */

	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] 락 획득 (find/add 모두 보호). */
	ig = iscsi_init_grp_find_by_tag(tag);
	/* [한국어] 대상 IG 검색. */
	if (!ig) {
		/* [한국어] 미존재: 락 풀고 즉시 -1 반환. */
		pthread_mutex_unlock(&g_iscsi.mutex);
		SPDK_ERRLOG("initiator group (%d) is not found\n", tag);
		return rc;
	}

	rc = iscsi_init_grp_add_initiators(ig, num_initiator_names,
					   initiator_names);
	/* [한국어] 이름 일괄 추가. */
	if (rc < 0) {
		SPDK_ERRLOG("add initiator name error\n");
		goto error;
	}

	rc = iscsi_init_grp_add_netmasks(ig, num_initiator_masks,
					 initiator_masks);
	/* [한국어] 넷마스크 일괄 추가. */
	if (rc < 0) {
		/* [한국어] 넷마스크 추가 실패: 이름은 이미 추가되었으므로 롤백한다. */
		SPDK_ERRLOG("add initiator netmask error\n");
		iscsi_init_grp_delete_initiators(ig, num_initiator_names,
						 initiator_names);
	}

error:
	pthread_mutex_unlock(&g_iscsi.mutex);
	/* [한국어] 모든 경로에서 락 해제 보장. */
	return rc;
	/* [한국어] 0 또는 음수 errno. */
}

/*
 * [한국어]
 * iscsi_init_grp_delete_initiators_from_initiator_list - 기존 IG에서 이름·넷마스크 제거.
 *
 * 동작: find_by_tag → delete_initiators → delete_netmasks. 넷마스크 삭제가 실패하면
 * 이미 삭제한 이름들을 add로 복구하여 원상태로 되돌린다.
 *
 * 호출 체인:
 *   RPC iscsi_initiator_group_remove_initiators → [delete_initiators_from_initiator_list]
 */
int
iscsi_init_grp_delete_initiators_from_initiator_list(int tag,
		int num_initiator_names,
		char **initiator_names,
		int num_initiator_masks,
		char **initiator_masks)
{
	int rc = -1;
	/* [한국어] 반환 코드. */
	struct spdk_iscsi_init_grp *ig;
	/* [한국어] find_by_tag 결과 IG. */

	SPDK_DEBUGLOG(iscsi,
		      "delete initiator from initiator group: tag=%d, #initiators=%d, #masks=%d\n",
		      tag, num_initiator_names, num_initiator_masks);
	/* [한국어] 디버그 로그. */

	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] 락 획득. */
	ig = iscsi_init_grp_find_by_tag(tag);
	/* [한국어] IG 검색. */
	if (!ig) {
		/* [한국어] 미존재 → 즉시 종료. */
		pthread_mutex_unlock(&g_iscsi.mutex);
		SPDK_ERRLOG("initiator group (%d) is not found\n", tag);
		return rc;
	}

	rc = iscsi_init_grp_delete_initiators(ig, num_initiator_names,
					      initiator_names);
	/* [한국어] 이름 일괄 삭제 (실패 시 내부에서 롤백). */
	if (rc < 0) {
		SPDK_ERRLOG("delete initiator name error\n");
		goto error;
	}

	rc = iscsi_init_grp_delete_netmasks(ig, num_initiator_masks,
					    initiator_masks);
	/* [한국어] 넷마스크 일괄 삭제. */
	if (rc < 0) {
		/* [한국어] 넷마스크 삭제 실패: 이름은 이미 지워졌으므로 다시 추가하여 복구. */
		SPDK_ERRLOG("delete initiator netmask error\n");
		iscsi_init_grp_add_initiators(ig, num_initiator_names,
					      initiator_names);
		goto error;
	}

error:
	pthread_mutex_unlock(&g_iscsi.mutex);
	/* [한국어] 모든 경로 락 해제. */
	return rc;
	/* [한국어] 0 또는 음수. */
}

/*
 * [한국어]
 * iscsi_init_grp_destroy - IG 1개와 그 자식 노드 전체를 free한다.
 *
 * @ig: 대상 IG (NULL 허용 — early return).
 *
 * 동작: 이름·넷마스크 노드 전부 free → IG 자체 free.
 * 본 함수는 IG가 g_iscsi.ig_head에서 이미 unregister된 상태이거나, 등록 전 단계에서
 * 호출된다고 가정한다 (등록 중인 IG에 대해 호출하면 dangling pointer 위험).
 */
void
iscsi_init_grp_destroy(struct spdk_iscsi_init_grp *ig)
{
	if (!ig) {
		/* [한국어] NULL 허용 — caller 편의상 early return. */
		return;
	}

	iscsi_init_grp_delete_all_initiators(ig);
	/* [한국어] 모든 이름 노드 free. */
	iscsi_init_grp_delete_all_netmasks(ig);
	/* [한국어] 모든 넷마스크 노드 free. */
	free(ig);
	/* [한국어] IG 자체 free. */
};

/*
 * [한국어]
 * iscsi_init_grp_find_by_tag - 전역 ig_head에서 tag로 IG 검색.
 *
 * @tag: 식별자.
 * @return: IG 또는 NULL.
 *
 * 호출자가 g_iscsi.mutex를 보유한 상태에서 호출해야 race 없이 결과 사용 가능.
 * (register/unregister 함수 안에서 락을 잡고 호출하는 것이 일반적.)
 */
struct spdk_iscsi_init_grp *
iscsi_init_grp_find_by_tag(int tag)
{
	struct spdk_iscsi_init_grp *ig;
	/* [한국어] 순회용 변수. */

	TAILQ_FOREACH(ig, &g_iscsi.ig_head, tailq) {
		/* [한국어] 등록된 IG들을 순회. */
		if (ig->tag == tag) {
			/* [한국어] tag 일치 — 그대로 반환. */
			return ig;
		}
	}

	return NULL;
	/* [한국어] 미발견. */
}

/*
 * [한국어]
 * iscsi_init_grps_destroy - 전체 IG 일괄 destroy (셧다운 경로).
 *
 * SPDK 종료 시점에 g_iscsi.ig_head를 비우고 모든 IG를 free한다.
 * 락을 잡은 채 destroy를 수행 — destroy 자체는 자식 노드 free만 하므로 추가 락 불필요.
 *
 * 호출 체인:
 *   spdk_iscsi_fini → [iscsi_init_grps_destroy]
 */
void
iscsi_init_grps_destroy(void)
{
	struct spdk_iscsi_init_grp *ig, *tmp;
	/* [한국어] FOREACH_SAFE용 (순회 중 free). */

	SPDK_DEBUGLOG(iscsi, "iscsi_init_grp_array_destroy\n");
	/* [한국어] 진단 로그. */
	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] 전역 락 획득 — fini 시점에도 다른 RPC가 들어올 가능성 차단. */
	TAILQ_FOREACH_SAFE(ig, &g_iscsi.ig_head, tailq, tmp) {
		/* [한국어] 헤드부터 순회하며 안전하게 제거. */
		TAILQ_REMOVE(&g_iscsi.ig_head, ig, tailq);
		/* [한국어] 전역 리스트에서 분리. */
		iscsi_init_grp_destroy(ig);
		/* [한국어] 자식과 본체 free. */
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	/* [한국어] 락 해제. */
}

/*
 * [한국어]
 * iscsi_init_grp_unregister - tag로 IG를 ig_head에서 분리만 하고 반환한다.
 *
 * @tag: 식별자.
 * @return: 분리된 IG (호출자가 destroy 책임) 또는 NULL.
 *
 * destroy와 분리되어 있는 이유는 RPC delete 흐름에서 ref 검사 등의 추가 단계 후
 * 별도로 destroy하기 위함이다.
 *
 * 호출 체인:
 *   RPC iscsi_delete_initiator_group → [unregister] → (caller) iscsi_init_grp_destroy
 */
struct spdk_iscsi_init_grp *
iscsi_init_grp_unregister(int tag)
{
	struct spdk_iscsi_init_grp *ig;
	/* [한국어] 순회/반환용. */

	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] 전역 락. */
	TAILQ_FOREACH(ig, &g_iscsi.ig_head, tailq) {
		/* [한국어] 등록된 IG들 순회. */
		if (ig->tag == tag) {
			/* [한국어] tag 일치 — 리스트에서 분리하고 반환. */
			TAILQ_REMOVE(&g_iscsi.ig_head, ig, tailq);
			pthread_mutex_unlock(&g_iscsi.mutex);
			/* [한국어] 락 해제 후 반환 (성공 경로). */
			return ig;
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	/* [한국어] 못 찾음: 락 해제 후 NULL. */
	return NULL;
}

/*
 * [한국어]
 * iscsi_init_grp_info_json - 단일 IG의 상태를 JSON 객체로 직렬화.
 *
 * @ig: 직렬화할 IG.
 * @w: JSON writer 컨텍스트 (RPC 응답 stream).
 *
 * 출력 형식: { "tag": <int>, "initiators": [<iqn>...], "netmasks": [<mask>...] }
 * RPC 응답이나 save_config의 params 부분에 모두 사용된다.
 */
static void
iscsi_init_grp_info_json(struct spdk_iscsi_init_grp *ig,
			 struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_initiator_name *iname;
	/* [한국어] 이름 노드 순회용. */
	struct spdk_iscsi_initiator_netmask *imask;
	/* [한국어] 넷마스크 노드 순회용. */

	spdk_json_write_object_begin(w);
	/* [한국어] JSON object "{" 시작. */

	spdk_json_write_named_int32(w, "tag", ig->tag);
	/* [한국어] "tag": <int> 출력. */

	spdk_json_write_named_array_begin(w, "initiators");
	/* [한국어] "initiators": [ 시작. */
	TAILQ_FOREACH(iname, &ig->initiator_head, tailq) {
		/* [한국어] 이름 리스트 순회. */
		spdk_json_write_string(w, iname->name);
		/* [한국어] 각 이름을 JSON string으로 출력. */
	}
	spdk_json_write_array_end(w);
	/* [한국어] ] 닫기. */

	spdk_json_write_named_array_begin(w, "netmasks");
	/* [한국어] "netmasks": [ 시작. */
	TAILQ_FOREACH(imask, &ig->netmask_head, tailq) {
		/* [한국어] 넷마스크 리스트 순회. */
		spdk_json_write_string(w, imask->mask);
		/* [한국어] 각 넷마스크 출력. */
	}
	spdk_json_write_array_end(w);
	/* [한국어] ] 닫기. */

	spdk_json_write_object_end(w);
	/* [한국어] } 닫기. */
}

/*
 * [한국어]
 * iscsi_init_grp_config_json - 단일 IG의 재생성 RPC 호출 형태로 직렬화.
 *
 * @ig: 직렬화할 IG.
 * @w: JSON writer 컨텍스트.
 *
 * 출력: { "method": "iscsi_create_initiator_group", "params": { ...info_json... } }
 * save_config로 출력된 JSON을 그대로 다시 RPC로 replay하면 동일 IG가 재생성된다.
 */
static void
iscsi_init_grp_config_json(struct spdk_iscsi_init_grp *ig,
			   struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	/* [한국어] 외부 wrapper object 시작. */

	spdk_json_write_named_string(w, "method", "iscsi_create_initiator_group");
	/* [한국어] RPC method 명. */

	spdk_json_write_name(w, "params");
	/* [한국어] "params": (값은 다음 호출에서). */
	iscsi_init_grp_info_json(ig, w);
	/* [한국어] info_json 형태를 그대로 params 값으로 사용 (재생성 가능 형태). */

	spdk_json_write_object_end(w);
	/* [한국어] wrapper object 종료. */
}

/*
 * [한국어]
 * iscsi_init_grps_info_json - 모든 IG의 상태를 JSON으로 출력.
 *
 * RPC `iscsi_get_initiator_groups` 등의 진입점에서 호출된다. 락은 호출자 책임.
 */
void
iscsi_init_grps_info_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_init_grp *ig;
	/* [한국어] 순회용. */

	TAILQ_FOREACH(ig, &g_iscsi.ig_head, tailq) {
		/* [한국어] 등록된 모든 IG 출력. */
		iscsi_init_grp_info_json(ig, w);
	}
}

/*
 * [한국어]
 * iscsi_init_grps_config_json - 모든 IG를 재생성 RPC 호출 형태로 출력 (save_config).
 *
 * SPDK save_config 시 IG 부분을 직렬화. 출력 결과를 nbd로 저장 후 다시 load 하면
 * 동일 IG들이 재생성된다.
 */
void
iscsi_init_grps_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_init_grp *ig;
	/* [한국어] 순회용. */

	TAILQ_FOREACH(ig, &g_iscsi.ig_head, tailq) {
		/* [한국어] 모든 IG에 대해 method+params 형태 출력. */
		iscsi_init_grp_config_json(ig, w);
	}
}
