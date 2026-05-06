/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 레거시 INI-스타일 구성(configuration) 파일 파서 (conf.c)
 *
 * === 파일의 역할 ===
 * SPDK 18.04 이전 버전에서 사용하던 INI 형식 구성 파일을 메모리 트리(spdk_conf
 * → section → item → value)로 파싱하고 조회·해제하는 라이브러리이다. 현재 SPDK
 * 의 표준 구성 메커니즘은 JSON-RPC(spdk_subsystem_init_from_json_config 등)로
 * 대체되었으므로 본 모듈은 실질적으로 deprecated/freeze 상태이며, 일부 레거시
 * 모듈(예: 옛 iSCSI 타깃 일부 옵션)이나 외부에서 backward-compat 용도로만 호출
 * 한다. 새 코드는 사용하지 말 것.
 *
 * 지원 포맷 요약:
 *   - 섹션 헤더: `[Section]` 또는 `[Section3]` (섹션 이름 끝의 숫자는 인스턴스
 *     번호 num 으로 분리되어 저장).
 *   - 키-값:    `Key Value1 Value2 ...` (공백/탭 구분, 다중 값 지원).
 *   - 줄 끝의 `\` 로 다음 줄과 이어붙이기(line continuation).
 *   - `#` 으로 시작하는 줄은 주석.
 *   - 같은 섹션이 여러 번 나오면 기본적으로 병합(merge_sections=true).
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 데몬 부팅 단계 → (구) -c <conf 파일> 옵션 처리 → spdk_conf_allocate
 *   → spdk_conf_read(file) [본 파일의 핵심 진입점] → 트리 구축
 *   → spdk_conf_set_as_default 로 기본 설정 등록
 *   → 각 서브시스템(예: 옛 iSCSI 코어)이 spdk_conf_find_section / get_*val
 *     로 자기 섹션 조회
 *
 * 현재 권장 경로: app/spdk_tgt 등이 JSON-RPC 기반 구성을 사용하며 본 코드는
 * 호출되지 않는다.
 *
 * 실행 컨텍스트는 호스트 유저스페이스의 SPDK 데몬 메인 thread (부팅 단계). 데이터
 * 평면(reactor/poller)에는 진입하지 않으므로 동시성·재진입을 거의 고려하지 않는다
 * (사실상 단일 스레드 사용 가정).
 *
 * === 타 모듈과의 연결 ===
 *  - include/spdk/conf.h    : 외부 공개 API (struct spdk_conf, *_section_*).
 *  - lib/util/string.c      : spdk_str_trim, spdk_strsepq, spdk_strtol — 문자열
 *                             파싱 보조 유틸.
 *  - lib/log/log.c          : SPDK_ERRLOG 매크로.
 *  - 외부 사용자(레거시 모듈): spdk_conf_read 후 spdk_conf_set_as_default 로
 *                              글로벌 default_config 를 설정하면, 같은 라이브러리
 *                              사용자는 cp 인자 NULL 로도 조회가 가능(편의).
 *
 * 데이터 흐름:
 *   파일(file) → fgets_line(가변 길이 라인 읽기, '\' 연결 처리)
 *               → parse_line(섹션/키-값 분리, 트리 노드 생성/추가)
 *               → struct spdk_conf 트리(섹션 → 항목 → 값 단방향 링크드 리스트)
 *   조회       : spdk_conf_find_section → find_cf_nitem → 각 *_get_*val
 *
 * 공유 자료구조:
 *   - default_config (file-static): 기본 설정 핸들. CHECK_CP_OR_USE_DEFAULT 로
 *     cp == NULL 호출을 자동 우회.
 *
 * === 주요 함수/구조체 요약 ===
 *  - struct spdk_conf            : 최상위 핸들 (file 이름 + 섹션 리스트 헤드).
 *  - struct spdk_conf_section    : 섹션 한 건 (이름, num, 항목 리스트).
 *  - struct spdk_conf_item       : 키 한 건 (이름 + 값 리스트).
 *  - struct spdk_conf_value      : 값 한 건 (다중 값을 위한 단방향 링크드 리스트).
 *  - spdk_conf_allocate          : 빈 핸들 할당, merge_sections=true 기본값 설정.
 *  - spdk_conf_free              : 트리 전체 재귀 free.
 *  - spdk_conf_read              : 파일 열어 fgets_line + parse_line 반복.
 *  - parse_line                  : 한 라인을 섹션 헤더 또는 key-value 로 분류·등록.
 *  - fgets_line                  : 가변 길이 라인 + '\\' continuation 지원 reader.
 *  - spdk_conf_find_section      : 이름(case-insensitive)으로 섹션 검색.
 *  - spdk_conf_section_get_*val  : 섹션 내 키 인덱스/값 인덱스 기반 조회.
 *  - spdk_conf_set_as_default    : default_config 를 설정 (NULL 핸들 fallback).
 *  - spdk_conf_disable_sections_merge: 같은 이름 섹션을 병합하지 않고 분리 보관.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 라이브러리 묶음 (stdio/stdlib/string/ctype 등). 본 파일은
 * fopen/fgets/strncasecmp/isspace/isdigit 등 기본 libc만 사용. */

#include "spdk/conf.h"
/* [한국어] 본 파일이 구현하는 외부 API 선언. 호출 시그니처와 불투명 자료형의
 * forward declaration 포함. */
#include "spdk/string.h"
/* [한국어] spdk_str_trim, spdk_strsepq, spdk_strtol 등 SPDK 전용 문자열
 * 헬퍼. quote 인식 separator(strsepq) 등이 INI 파싱에 유용. */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG 매크로. 본 파일은 비동기 콜백이 없는 단순 파서이므로
 * 에러는 즉시 로그 출력 + 음수 반환으로만 처리. */

/*
 * [한국어]
 * struct spdk_conf_value — 키 1개에 매달려 있는 다중 값(multi-value) 노드.
 *
 * INI 한 줄에 "Key v1 v2 v3" 처럼 여러 값이 적힐 수 있고, 이를 v1→v2→v3 단방향
 * 링크드 리스트로 보관한다. v1=0번, v2=1번 처럼 인덱스로 구분 조회된다.
 */
struct spdk_conf_value {
	struct spdk_conf_value *next;
	/* [한국어] 같은 키의 다음 값 (NULL 이면 마지막).
	 * 설정자: append_cf_value 가 꼬리 삽입 시 갱신.
	 * 읽는 자: spdk_conf_section_get_nmval 의 인덱스 순회.
	 * 동기화: 트리 구축 후 read-only 사용 가정 → 락 없음. */
	char *value;
	/* [한국어] 값 문자열의 strdup 사본 (소유권 보유 — free 책임).
	 * 설정자: parse_line 에서 strdup 으로 복사.
	 * 읽는 자: get_*val 함수들이 그대로 const 포인터 반환.
	 * 값 범위: NULL 이면 free 호출 안전. NUL 종료 보장.
	 * 동기화: read-only 사용 가정. */
};

/*
 * [한국어]
 * struct spdk_conf_item — 섹션 내 키-값(들) 묶음 1건.
 */
struct spdk_conf_item {
	struct spdk_conf_item *next;
	/* [한국어] 같은 섹션 안의 다음 키 (NULL 이면 마지막).
	 * 설정자: append_cf_item.
	 * 읽는 자: find_cf_nitem 의 인덱스 순회. */
	char *key;
	/* [한국어] 키 이름 strdup 사본. 비교는 case-insensitive(strcasecmp).
	 * 설정자: parse_line 의 strdup. 읽는 자: find_cf_nitem. */
	struct spdk_conf_value *val;
	/* [한국어] 이 키의 값 리스트 헤드 (NULL = 값 없음).
	 * 다중 값일 경우 v1→v2→... 형태. */
};

/*
 * [한국어]
 * struct spdk_conf_section — 섹션 1건 ([Name] 또는 [Name<숫자>]).
 */
struct spdk_conf_section {
	struct spdk_conf_section *next;
	/* [한국어] 다음 섹션 (NULL 이면 마지막). */
	char *name;
	/* [한국어] 섹션 이름 strdup. case-insensitive 비교 대상.
	 * 끝의 숫자는 num 필드로 분리된 후에도 이름에 그대로 남는 점 주의
	 * (예: "[Foo3]" → name="Foo3", num=3). */
	int num;
	/* [한국어] 섹션 이름 끝의 첫 숫자(인스턴스 번호). 숫자가 없으면 0.
	 * 예: "[BdevMalloc1]" → num=1. 같은 이름의 다중 인스턴스 구분에 쓰임. */
	struct spdk_conf_item *item;
	/* [한국어] 이 섹션의 키 리스트 헤드. */
};

/*
 * [한국어]
 * struct spdk_conf — 파싱된 구성 전체에 대한 핸들.
 */
struct spdk_conf {
	char *file;
	/* [한국어] 읽어들인 파일 경로의 strdup. 에러 메시지에 사용. */
	struct spdk_conf_section *current_section;
	/* [한국어] parse_line 이 진행 중인 "현재" 섹션 — 키-값 라인을 만나면 이
	 * 섹션의 item 리스트에 추가한다.
	 * 설정자: parse_line 에서 [Section] 만날 때 갱신.
	 * 읽는 자: parse_line 에서 일반 키-값 라인 처리 시.
	 * 동기화: 단일 thread 파싱 가정. */
	struct spdk_conf_section *section;
	/* [한국어] 섹션 리스트 헤드(첫 노드). */
	bool merge_sections;
	/* [한국어] 같은 이름의 섹션이 여러 번 등장할 때 병합 여부.
	 * true(기본): 두 번째 [Foo] 의 항목이 첫 번째 [Foo] 에 합쳐짐.
	 * false: 새 섹션 노드로 분리(이름이 같지만 별개 섹션).
	 * 설정자: spdk_conf_allocate(true), spdk_conf_disable_sections_merge(false).
	 * 읽는 자: parse_line 의 분기.
	 * 동기화: 파싱 전에 한 번 결정 → read-only. */
};

/* [한국어] 키-값 라인의 값 부분에서 공백/탭을 구분자로 분리. */
#define CF_DELIM " \t"
/* [한국어] 키-값 라인의 첫 토큰(키)을 분리할 때 = 도 허용 (Key=Value 호환). */
#define CF_DELIM_KEY " \t="

/* [한국어] fgets_line 이 한 번에 읽는 청크 크기. 더 긴 라인은 동적 확장. */
#define LIB_MAX_TMPBUF 1024

/* [한국어] 글로벌 기본 설정 — spdk_conf_set_as_default 로 지정.
 * 호출자가 cp == NULL 로 조회할 때 이 값이 자동으로 사용된다. NULL 인 상태에서
 * cp == NULL 호출은 모두 NULL 반환으로 처리. */
static struct spdk_conf *default_config = NULL;

/*
 * [한국어]
 * spdk_conf_allocate - 빈 spdk_conf 핸들을 할당하고 기본값을 설정한다.
 *
 * @return: 성공 시 새로 할당된 핸들, OOM 시 NULL.
 *
 * merge_sections 기본값 true (호환성: 같은 이름 섹션이 자동 병합됨).
 * 호출자는 spdk_conf_read 로 파일을 채워 넣고, 사용 후 spdk_conf_free 로
 * 해제해야 한다.
 */
struct spdk_conf *
spdk_conf_allocate(void)
{
	struct spdk_conf *ret = calloc(1, sizeof(struct spdk_conf));
	/* [한국어] calloc 으로 0-init — section, current_section, file 모두 NULL 시작. */

	if (ret) {
		ret->merge_sections = true;
		/* [한국어] 기본값을 명시적으로 true 설정(0-init 후이므로 명시 필요).
		 * 호환성을 위해 같은 이름 섹션의 항목을 합치는 동작이 기본. */
	}

	return ret;
}

/*
 * [한국어]
 * free_conf_value - 단일 value 노드와 그 문자열 버퍼를 해제.
 *
 * @vp: 해제할 노드 (NULL 안전).
 *
 * 단일 노드만 처리하며 next 체인 추적은 하지 않는다(상위 free_all_conf_value
 * 가 담당).
 */
static void
free_conf_value(struct spdk_conf_value *vp)
{
	if (vp == NULL) {
		return;
		/* [한국어] NULL 안전 — 호출자의 NULL 검증 부담 경감. */
	}

	if (vp->value) {
		free(vp->value);
		/* [한국어] strdup 으로 할당된 값 문자열 해제. */
	}

	free(vp);
	/* [한국어] 노드 자체 해제. */
}

/*
 * [한국어]
 * free_all_conf_value - 단방향 value 리스트 전체를 순회 해제.
 *
 * @vp: 리스트 헤드 (NULL 안전).
 */
static void
free_all_conf_value(struct spdk_conf_value *vp)
{
	struct spdk_conf_value *next;
	/* [한국어] 현재 노드를 free 한 뒤에도 다음 노드로 이동할 수 있도록 임시 보관. */

	if (vp == NULL) {
		return;
	}

	while (vp != NULL) {
		next = vp->next;
		/* [한국어] free 전에 next 보존 — 사후 접근 금지. */
		free_conf_value(vp);
		vp = next;
	}
}

/*
 * [한국어]
 * free_conf_item - 단일 item 노드와 그 키 문자열, 값 리스트를 해제.
 *
 * @ip: 해제할 노드 (NULL 안전).
 */
static void
free_conf_item(struct spdk_conf_item *ip)
{
	if (ip == NULL) {
		return;
	}

	if (ip->val != NULL) {
		free_all_conf_value(ip->val);
		/* [한국어] 자식인 값 리스트를 먼저 모두 해제. */
	}

	if (ip->key != NULL) {
		free(ip->key);
		/* [한국어] strdup 키 문자열 해제. */
	}

	free(ip);
	/* [한국어] 노드 자체 해제. */
}

/*
 * [한국어]
 * free_all_conf_item - 단방향 item 리스트 전체 해제.
 */
static void
free_all_conf_item(struct spdk_conf_item *ip)
{
	struct spdk_conf_item *next;

	if (ip == NULL) {
		return;
	}

	while (ip != NULL) {
		next = ip->next;
		free_conf_item(ip);
		ip = next;
	}
}

/*
 * [한국어]
 * free_conf_section - 단일 섹션 노드와 그 이름, 자식 item 리스트 해제.
 */
static void
free_conf_section(struct spdk_conf_section *sp)
{
	if (sp == NULL) {
		return;
	}

	if (sp->item) {
		free_all_conf_item(sp->item);
		/* [한국어] 자식 item 리스트를 먼저 정리. */
	}

	if (sp->name) {
		free(sp->name);
		/* [한국어] 섹션 이름 strdup 해제. */
	}

	free(sp);
}

/*
 * [한국어]
 * free_all_conf_section - 섹션 리스트 전체 해제.
 */
static void
free_all_conf_section(struct spdk_conf_section *sp)
{
	struct spdk_conf_section *next;

	if (sp == NULL) {
		return;
	}

	while (sp != NULL) {
		next = sp->next;
		free_conf_section(sp);
		sp = next;
	}
}

/*
 * [한국어]
 * spdk_conf_free - 핸들과 그 모든 자식 트리(섹션/항목/값)를 재귀적으로 해제.
 *
 * @cp: 해제할 핸들. NULL 안전.
 *
 * 사용 후 호출자는 cp 포인터를 더 이상 사용하면 안 된다(use-after-free 위험).
 * default_config 로 지정된 핸들을 free 한 경우 호출자가 set_as_default(NULL)
 * 로 글로벌 변수를 갱신해 주어야 한다(본 함수는 자동으로 글로벌 NULL 갱신을
 * 하지 않음).
 */
void
spdk_conf_free(struct spdk_conf *cp)
{
	if (cp == NULL) {
		return;
		/* [한국어] NULL 안전 — 이중 free 보호. */
	}

	if (cp->section != NULL) {
		free_all_conf_section(cp->section);
		/* [한국어] 모든 섹션 + 자식 노드 재귀 해제. */
	}

	if (cp->file != NULL) {
		free(cp->file);
		/* [한국어] 파일 경로 strdup 해제. */
	}

	free(cp);
	/* [한국어] 핸들 자체 해제. */
}

/*
 * [한국어]
 * allocate_cf_section - 빈 섹션 노드 1건 calloc.
 */
static struct spdk_conf_section *
allocate_cf_section(void)
{
	return calloc(1, sizeof(struct spdk_conf_section));
	/* [한국어] 0-init 으로 next/item/name=NULL, num=0. 호출자가 name/num 채움. */
}

/*
 * [한국어]
 * allocate_cf_item - 빈 item 노드 1건 calloc.
 */
static struct spdk_conf_item *
allocate_cf_item(void)
{
	return calloc(1, sizeof(struct spdk_conf_item));
	/* [한국어] 0-init 으로 key/val/next=NULL. */
}

/*
 * [한국어]
 * allocate_cf_value - 빈 value 노드 1건 calloc.
 */
static struct spdk_conf_value *
allocate_cf_value(void)
{
	return calloc(1, sizeof(struct spdk_conf_value));
	/* [한국어] 0-init 으로 value/next=NULL. */
}


/* [한국어] cp 가 NULL 일 때 default_config 를 자동 사용하는 매크로 — 사용자가
 * 매번 글로벌 핸들을 명시적으로 넘기지 않도록 하는 편의 장치.
 * 둘 다 NULL 이면 결과도 NULL 이며, 호출자는 추가 NULL 검사가 필요. */
#define CHECK_CP_OR_USE_DEFAULT(cp) (((cp) == NULL) && (default_config != NULL)) ? default_config : (cp)

/*
 * [한국어]
 * spdk_conf_find_section - 이름으로 섹션을 case-insensitive 검색.
 *
 * @cp:   설정 핸들 (NULL 이면 default_config 사용).
 * @name: 찾을 섹션 이름 (NULL/빈문자열 거부).
 * @return: 찾은 섹션 포인터, 없거나 인자 invalid 면 NULL.
 *
 * 외부 코드(레거시 모듈)가 자기 섹션 데이터를 가져올 때 사용. parse_line 도
 * merge_sections=true 분기에서 호출하여 동일 이름 섹션 병합 처리.
 */
struct spdk_conf_section *
spdk_conf_find_section(struct spdk_conf *cp, const char *name)
{
	struct spdk_conf_section *sp;

	if (name == NULL || name[0] == '\0') {
		return NULL;
		/* [한국어] 빈 이름 검색 거부. */
	}

	cp = CHECK_CP_OR_USE_DEFAULT(cp);
	/* [한국어] cp NULL 이면 default_config 로 fallback. */
	if (cp == NULL) {
		return NULL;
		/* [한국어] fallback 도 없으면 검색 불가. */
	}

	for (sp = cp->section; sp != NULL; sp = sp->next) {
		/* [한국어] 섹션 리스트 선형 탐색 — 섹션 수가 적어 O(N) 충분. */
		if (sp->name != NULL && sp->name[0] == name[0]
		    && strcasecmp(sp->name, name) == 0) {
			/* [한국어] 빠른 1차 비교(첫 글자 match) 후 strcasecmp 로 정밀 비교
			 * — 불일치 케이스의 strcasecmp 호출 비용을 줄이는 마이크로
			 * 최적화. case-insensitive 비교를 통해 [BDEV] / [bdev] 가 같은
			 * 섹션으로 매칭. */
			return sp;
		}
	}

	return NULL;
	/* [한국어] 못 찾음 — 호출자가 NULL 처리. */
}

/*
 * [한국어]
 * spdk_conf_first_section - 첫 섹션 포인터 반환 (iteration 시작점).
 *
 * @cp: 설정 핸들 (NULL 이면 default_config).
 * @return: 첫 섹션 또는 NULL.
 *
 * spdk_conf_next_section 과 함께 모든 섹션 순회에 사용.
 */
struct spdk_conf_section *
spdk_conf_first_section(struct spdk_conf *cp)
{
	cp = CHECK_CP_OR_USE_DEFAULT(cp);
	if (cp == NULL) {
		return NULL;
	}

	return cp->section;
}

/*
 * [한국어]
 * spdk_conf_next_section - 다음 섹션 포인터 반환.
 *
 * @sp: 현재 섹션. NULL 안전.
 * @return: 다음 섹션 또는 NULL.
 */
struct spdk_conf_section *
spdk_conf_next_section(struct spdk_conf_section *sp)
{
	if (sp == NULL) {
		return NULL;
	}

	return sp->next;
}

/*
 * [한국어]
 * append_cf_section - 섹션 리스트 꼬리에 새 섹션 추가.
 *
 * @cp: 핸들 (NULL → default_config). NULL 이면 에러 로그 후 무시.
 * @sp: 추가할 섹션 노드.
 *
 * 단방향 리스트 꼬리 삽입 — 입력 파일의 등장 순서 유지가 목적.
 * O(N) 비용이지만 N(섹션 수) 작아서 실용상 문제 없음.
 */
static void
append_cf_section(struct spdk_conf *cp, struct spdk_conf_section *sp)
{
	struct spdk_conf_section *last;
	/* [한국어] 끝 노드를 가리킬 변수. */

	cp = CHECK_CP_OR_USE_DEFAULT(cp);
	if (cp == NULL) {
		SPDK_ERRLOG("cp == NULL\n");
		return;
		/* [한국어] 핸들 없이 추가 불가 — 사용자가 cp 를 만들지 않은 상태에서
		 * append 가 호출된 비정상 흐름. */
	}

	if (cp->section == NULL) {
		cp->section = sp;
		return;
		/* [한국어] 첫 섹션 — 헤드에 직접 연결. */
	}

	for (last = cp->section; last->next != NULL; last = last->next)
		;
	/* [한국어] 끝 노드까지 이동(꼬리 검색). 빈 루프 본문은 의도. */
	last->next = sp;
	/* [한국어] 꼬리에 새 섹션 연결. */
}

/*
 * [한국어]
 * find_cf_nitem - 섹션 내에서 같은 키 이름의 idx 번째 item 검색.
 *
 * @sp:  대상 섹션.
 * @key: 키 이름 (NULL/빈문자열 거부).
 * @idx: 같은 이름이 여러 번 나오는 경우의 0-base 인덱스.
 * @return: 찾은 노드 또는 NULL.
 *
 * INI 형식상 같은 키가 같은 섹션에 여러 번 나올 수 있으므로(예: 다중 LUN,
 * 다중 portal), 인덱스 기반 조회를 지원한다.
 */
static struct spdk_conf_item *
find_cf_nitem(struct spdk_conf_section *sp, const char *key, int idx)
{
	struct spdk_conf_item *ip;
	int i;
	/* [한국어] 동일 키 등장 횟수 카운터. */

	if (key == NULL || key[0] == '\0') {
		return NULL;
	}

	i = 0;
	for (ip = sp->item; ip != NULL; ip = ip->next) {
		if (ip->key != NULL && ip->key[0] == key[0]
		    && strcasecmp(ip->key, key) == 0) {
			/* [한국어] case-insensitive 매칭 (find_section 과 동일 패턴). */
			if (i == idx) {
				return ip;
				/* [한국어] idx 번째 매칭 발견 — 즉시 반환. */
			}
			i++;
			/* [한국어] 다음 매칭을 찾기 위해 카운터 증가. */
		}
	}

	return NULL;
}

/*
 * [한국어]
 * append_cf_item - 섹션의 item 리스트 꼬리에 새 항목 추가.
 *
 * @sp: 섹션 (NULL 안전).
 * @ip: 추가할 item 노드.
 */
static void
append_cf_item(struct spdk_conf_section *sp, struct spdk_conf_item *ip)
{
	struct spdk_conf_item *last;

	if (sp == NULL) {
		return;
	}

	if (sp->item == NULL) {
		sp->item = ip;
		return;
		/* [한국어] 첫 항목 — 헤드에 직접 연결. */
	}

	for (last = sp->item; last->next != NULL; last = last->next)
		;
	last->next = ip;
}

/*
 * [한국어]
 * append_cf_value - item 의 값 리스트 꼬리에 새 값 추가.
 *
 * @ip: item (NULL 안전).
 * @vp: 추가할 값 노드.
 */
static void
append_cf_value(struct spdk_conf_item *ip, struct spdk_conf_value *vp)
{
	struct spdk_conf_value *last;

	if (ip == NULL) {
		return;
	}

	if (ip->val == NULL) {
		ip->val = vp;
		return;
	}

	for (last = ip->val; last->next != NULL; last = last->next)
		;
	last->next = vp;
}

/*
 * [한국어]
 * spdk_conf_section_match_prefix - 섹션 이름이 prefix 로 시작하는지 검사.
 *
 * @sp:           섹션.
 * @name_prefix:  접두사 문자열.
 * @return:       매칭이면 true.
 *
 * 예: "BdevMalloc1" 이 prefix "Bdev" 와 매칭 — 같은 카테고리의 인스턴스 필터링에 사용.
 */
bool
spdk_conf_section_match_prefix(const struct spdk_conf_section *sp, const char *name_prefix)
{
	return strncasecmp(sp->name, name_prefix, strlen(name_prefix)) == 0;
	/* [한국어] case-insensitive prefix 비교. strlen(prefix) 만큼만 비교. */
}

/*
 * [한국어]
 * spdk_conf_section_get_name - 섹션 이름 문자열 포인터 반환.
 */
const char *
spdk_conf_section_get_name(const struct spdk_conf_section *sp)
{
	return sp->name;
	/* [한국어] read-only 노출. 호출자는 수정/해제 금지. */
}

/*
 * [한국어]
 * spdk_conf_section_get_num - 섹션 이름 끝의 인스턴스 번호 반환.
 *
 * 예: "[Foo3]" → 3, "[Foo]" → 0.
 */
int
spdk_conf_section_get_num(const struct spdk_conf_section *sp)
{
	return sp->num;
}

/*
 * [한국어]
 * spdk_conf_section_get_nmval - (key idx1번째 등장)의 idx2번째 값을 반환.
 *
 * @sp:    섹션.
 * @key:   키 이름.
 * @idx1:  같은 키 중 몇 번째 등장 (0-base).
 * @idx2:  해당 항목 안에서 몇 번째 값 (0-base, 다중 값 지원).
 * @return: 값 문자열 포인터 또는 NULL.
 *
 * 예: 입력 "Foo a b c\nFoo x y z\n" 에서
 *   get_nmval("Foo", 0, 1) = "b"
 *   get_nmval("Foo", 1, 2) = "z"
 */
char *
spdk_conf_section_get_nmval(struct spdk_conf_section *sp, const char *key, int idx1, int idx2)
{
	struct spdk_conf_item *ip;
	struct spdk_conf_value *vp;
	int i;

	ip = find_cf_nitem(sp, key, idx1);
	/* [한국어] idx1 번째 같은 키 항목을 찾는다. */
	if (ip == NULL) {
		return NULL;
		/* [한국어] 해당 인덱스의 항목이 없음. */
	}

	vp = ip->val;
	if (vp == NULL) {
		return NULL;
		/* [한국어] 키는 있는데 값이 없는 경우. */
	}

	for (i = 0; vp != NULL; vp = vp->next, i++) {
		if (i == idx2) {
			return vp->value;
			/* [한국어] idx2 번째 값 도달. */
		}
	}

	return NULL;
	/* [한국어] idx2 가 값 개수보다 큼 — 범위 밖. */
}

/*
 * [한국어]
 * spdk_conf_section_get_nval - idx 번째 등장 키의 첫 번째 값 반환 (편의 함수).
 *
 * @sp:  섹션.
 * @key: 키 이름.
 * @idx: 같은 키 중 몇 번째 등장.
 */
char *
spdk_conf_section_get_nval(struct spdk_conf_section *sp, const char *key, int idx)
{
	struct spdk_conf_item *ip;
	struct spdk_conf_value *vp;

	ip = find_cf_nitem(sp, key, idx);
	if (ip == NULL) {
		return NULL;
	}

	vp = ip->val;
	if (vp == NULL) {
		return NULL;
	}

	return vp->value;
	/* [한국어] 첫 값 직반환 (다중 값일 경우 v[0]). */
}

/*
 * [한국어]
 * spdk_conf_section_get_val - 같은 키의 0번째 등장의 0번째 값 반환 (가장 단순).
 */
char *
spdk_conf_section_get_val(struct spdk_conf_section *sp, const char *key)
{
	return spdk_conf_section_get_nval(sp, key, 0);
	/* [한국어] 가장 빈번한 사용 패턴 — 한 키에 한 값. */
}

/*
 * [한국어]
 * spdk_conf_section_get_intval - 키 값을 정수로 변환하여 반환.
 *
 * @return: 값 문자열을 base 10 으로 strtol 한 결과. 키 없으면 -1.
 *
 * 주의: -1 은 "키 없음"과 "실제 -1 값" 을 구분하지 못한다(레거시 API 한계).
 */
int
spdk_conf_section_get_intval(struct spdk_conf_section *sp, const char *key)
{
	const char *v;
	int value;

	v = spdk_conf_section_get_nval(sp, key, 0);
	if (v == NULL) {
		return -1;
		/* [한국어] 센티넬 — 호출자는 키 존재 여부를 별도로 확인할 필요 있음. */
	}

	value = (int)spdk_strtol(v, 10);
	/* [한국어] base 10 정수 변환. 변환 실패 시 spdk_strtol 이 음수 반환(에러
	 * 코드). 본 함수는 그 음수를 그대로 호출자에게 전달. */
	return value;
}

/*
 * [한국어]
 * spdk_conf_section_get_boolval - 키 값을 bool 로 해석하여 반환.
 *
 * 인식하는 값(case-insensitive):
 *   true 계열  : "Yes", "Y", "True"
 *   false 계열 : "No",  "N", "False"
 * 그 외 또는 키 없음 → default_val 반환.
 *
 * 키 누락과 잘못된 값 모두 default 로 떨어지므로, 호출자가 "있는데 잘못 적힘"
 * 을 감지하려면 별도 검증 필요.
 */
bool
spdk_conf_section_get_boolval(struct spdk_conf_section *sp, const char *key, bool default_val)
{
	const char *v;

	v = spdk_conf_section_get_nval(sp, key, 0);
	if (v == NULL) {
		return default_val;
		/* [한국어] 키 자체가 없음. */
	}

	if (!strcasecmp(v, "Yes") || !strcasecmp(v, "Y") || !strcasecmp(v, "True")) {
		return true;
		/* [한국어] 다양한 진리값 표기 허용. */
	}

	if (!strcasecmp(v, "No") || !strcasecmp(v, "N") || !strcasecmp(v, "False")) {
		return false;
	}

	return default_val;
	/* [한국어] 인식 못 한 문자열은 기본값으로 fallback. */
}

/*
 * [한국어]
 * parse_line - 입력 한 줄(lp)을 분석하여 섹션 헤더 또는 키-값으로 분류·등록.
 *
 * @cp: 진행 중인 spdk_conf 핸들.
 * @lp: NUL 종료된 한 라인 문자열 (호출자가 trim 전 상태로 전달).
 * @return: 0 성공, -1 파싱 에러.
 *
 * 섹션 헤더 처리:
 *   "[Name]" 파싱 → 이름 끝의 첫 숫자를 num 으로 분리.
 *   merge_sections 면 기존 섹션 검색, 없으면 새 노드 생성.
 *   cp->current_section 갱신.
 *
 * 키-값 처리:
 *   "Key val1 val2 ..." 형태로 분리.
 *   현재 섹션의 item 리스트에 추가, 값들을 value 리스트에 차례로 추가.
 *   섹션 미정 상태에서 키-값이 오면 에러.
 *
 * 메모리 에러는 -1 반환 후에도 부분적으로 등록된 노드들이 남아 있을 수 있다
 * (호출자는 spdk_conf_free 로 일괄 정리 가능).
 *
 * 호출 체인:
 *   spdk_conf_read → fgets_line → [parse_line] → allocate_cf_*, append_cf_*
 */
static int
parse_line(struct spdk_conf *cp, char *lp)
{
	struct spdk_conf_section *sp;
	struct spdk_conf_item *ip;
	struct spdk_conf_value *vp;
	char *arg;
	char *key;
	char *val;
	char *p;
	int num;

	arg = spdk_str_trim(lp);
	/* [한국어] 양쪽 공백/탭/개행 제거. lp 의 내용을 in-place 수정. */
	if (arg == NULL) {
		SPDK_ERRLOG("no section\n");
		/* [한국어] 잘못된 라인(또는 NULL). 본래 메시지는 다소 부정확. */
		return -1;
	}

	if (arg[0] == '[') {
		/* section */
		/* [한국어] 섹션 헤더 처리: "[Name]" 또는 "[Name3]" 형태. */
		arg++;
		/* [한국어] 여는 대괄호 건너뜀 → arg 는 이름 + 닫는 대괄호. */
		key = spdk_strsepq(&arg, "]");
		/* [한국어] 닫는 ']' 까지를 key 로, 그 뒤를 arg 에 남긴다.
		 * spdk_strsepq 는 quote-aware separator. */
		if (key == NULL || arg != NULL) {
			/* [한국어] ']' 가 없거나, ']' 뒤에 추가 토큰이 있으면(예: "[A] B")
			 * 형식 오류. */
			SPDK_ERRLOG("broken section\n");
			return -1;
		}
		/* determine section number */
		/* [한국어] 섹션 이름 끝의 숫자를 num 으로 분리.
		 * 예: "Foo3" → p 가 '3' 위치 → num=3. 숫자 없으면 num=0. */
		for (p = key; *p != '\0' && !isdigit((int) *p); p++)
			;
		/* [한국어] 첫 숫자까지 포인터 전진. */
		if (*p != '\0') {
			num = (int)spdk_strtol(p, 10);
			/* [한국어] 첫 숫자에서 시작해 정수 파싱. 음수 반환은 변환 실패. */
		} else {
			num = 0;
			/* [한국어] 숫자 없음 → 기본 인스턴스 0. */
		}

		if (cp->merge_sections) {
			sp = spdk_conf_find_section(cp, key);
			/* [한국어] 같은 이름 섹션이 이미 있으면 그것을 재사용 → 항목들이
			 * 합쳐진다. */
		} else {
			sp = NULL;
			/* [한국어] 병합 비활성 — 항상 새 섹션 노드 생성. */
		}

		if (sp == NULL) {
			sp = allocate_cf_section();
			/* [한국어] 새 섹션 노드. */
			if (sp == NULL) {
				SPDK_ERRLOG("cannot allocate cf section\n");
				return -1;
			}
			append_cf_section(cp, sp);
			/* [한국어] 섹션 리스트 꼬리에 등록 — 등장 순서 보존. */

			sp->name = strdup(key);
			/* [한국어] 이름 복사 소유. parse 후에도 영구 보관. */
			if (sp->name == NULL) {
				SPDK_ERRLOG("cannot duplicate %s to sp->name\n", key);
				return -1;
			}
		}
		cp->current_section = sp;
		/* [한국어] 이후 키-값 라인은 이 섹션의 자식으로 추가됨. */


		sp->num = num;
		/* [한국어] 인스턴스 번호 갱신 (병합된 섹션이라도 마지막 헤더의 num 을
		 * 사용 — 사실상 변하지 않는 경우가 일반적). */
	} else {
		/* parameters */
		/* [한국어] 키-값 라인 처리. 현재 섹션이 정해져 있어야 함. */
		sp = cp->current_section;
		if (sp == NULL) {
			SPDK_ERRLOG("unknown section\n");
			/* [한국어] 첫 섹션 헤더 이전에 키-값이 등장 — 형식 오류. */
			return -1;
		}
		key = spdk_strsepq(&arg, CF_DELIM_KEY);
		/* [한국어] 첫 토큰을 키로 분리. 구분자는 공백/탭/`=`. */
		if (key == NULL) {
			SPDK_ERRLOG("broken key\n");
			return -1;
		}

		ip = allocate_cf_item();
		/* [한국어] 새 키 항목 노드 할당. */
		if (ip == NULL) {
			SPDK_ERRLOG("cannot allocate cf item\n");
			return -1;
		}
		append_cf_item(sp, ip);
		/* [한국어] 섹션 item 리스트 꼬리에 추가. */
		ip->key = strdup(key);
		if (ip->key == NULL) {
			SPDK_ERRLOG("cannot make duplicate of %s\n", key);
			return -1;
		}
		ip->val = NULL;
		/* [한국어] 값 리스트 초기화 (calloc 으로 이미 NULL 이지만 가독성용). */
		if (arg != NULL) {
			/* key has value(s) */
			/* [한국어] 키 뒤에 값이 1개 이상 있는 경우. */
			while (arg != NULL) {
				val = spdk_strsepq(&arg, CF_DELIM);
				/* [한국어] 다음 값 토큰 분리. arg 가 NULL 이 되면 종료. */
				vp = allocate_cf_value();
				if (vp == NULL) {
					SPDK_ERRLOG("cannot allocate cf value\n");
					return -1;
				}
				append_cf_value(ip, vp);
				/* [한국어] item 의 값 리스트 꼬리에 등록. */
				vp->value = strdup(val);
				if (vp->value == NULL) {
					SPDK_ERRLOG("cannot duplicate %s to vp->value\n", val);
					return -1;
				}
			}
		}
	}

	return 0;
	/* [한국어] 정상 처리 완료. */
}

/*
 * [한국어]
 * fgets_line - 가변 길이 한 라인을 동적 버퍼로 읽어들이는 헬퍼.
 *
 * @fp:    열린 FILE*.
 * @return: NUL 종료된 라인 버퍼(끝에 '\n' 포함, 호출자가 free), EOF 또는 OOM
 *          시 NULL.
 *
 * 동작:
 *   1) LIB_MAX_TMPBUF(1024) 청크로 fgets 하면서 누적.
 *   2) 한 청크 안에서 끝났거나 '\n' 으로 끝나면 정확한 크기로 realloc 하여 반환.
 *   3) 끝나지 않았으면 버퍼를 LIB_MAX_TMPBUF 만큼 확장 후 다음 청크 읽기.
 *   4) EOF 이면서 마지막에 '\n' 이 없으면 추가 1바이트 확보 후 '\n' 보충
 *      (parse_line 의 trim 단순화 목적).
 *
 * 메모리 에러 시 부분 할당된 버퍼는 즉시 free. 호출자에게는 NULL 만 보임.
 */
static char *
fgets_line(FILE *fp)
{
	char *dst, *dst2, *p;
	size_t total, len;

	dst = p = malloc(LIB_MAX_TMPBUF);
	/* [한국어] 초기 버퍼 + 쓰기 포인터 동시 초기화. p 는 다음 청크가 들어갈 위치. */
	if (!dst) {
		return NULL;
	}

	dst[0] = '\0';
	/* [한국어] 빈 문자열로 초기화. 빈 라인 처리 안전화. */
	total = 0;

	while (fgets(p, LIB_MAX_TMPBUF, fp) != NULL) {
		/* [한국어] 한 청크 읽기. fgets 는 LIB_MAX_TMPBUF-1 바이트까지 읽고 NUL
		 * 종료. EOF 또는 에러 시 NULL 반환. */
		len = strlen(p);
		total += len;
		if (len + 1 < LIB_MAX_TMPBUF || dst[total - 1] == '\n') {
			/* [한국어] (1) 한 청크를 다 못 채웠거나(=짧은 라인 끝) (2) '\n' 으로
			 * 끝났으면 라인 완성. realloc 으로 정확한 크기로 줄여 반환. */
			dst2 = realloc(dst, total + 1);
			if (!dst2) {
				free(dst);
				return NULL;
			} else {
				return dst2;
			}
		}

		/* [한국어] 라인이 청크보다 길다 — 버퍼 확장. */
		dst2 = realloc(dst, total + LIB_MAX_TMPBUF);
		if (!dst2) {
			free(dst);
			return NULL;
		} else {
			dst = dst2;
		}

		p = dst + total;
		/* [한국어] 다음 청크가 쓰일 위치 갱신 (확장된 영역의 시작점). */
	}

	if (feof(fp) && total != 0) {
		/* [한국어] EOF 인데 마지막 라인이 '\n' 없이 끝난 경우 — '\n' 보충하여
		 * parse_line 단순화. */
		dst2 = realloc(dst, total + 2);
		if (!dst2) {
			free(dst);
			return NULL;
		} else {
			dst = dst2;
		}

		dst[total] = '\n';
		dst[total + 1] = '\0';
		return dst;
	}

	free(dst);
	/* [한국어] 빈 EOF 또는 에러 — 호출자는 NULL 로 종료 인식. */

	return NULL;
}

/*
 * [한국어]
 * spdk_conf_read - 파일을 열어 트리에 채워 넣는다(파싱 진입점).
 *
 * @cp:   미리 spdk_conf_allocate 로 만든 핸들.
 * @file: 파일 경로.
 * @return: 0 성공, -1 실패(파일 못 열거나 OOM 등; 파싱 에러는 라인별 로그만
 *          남기고 계속 진행하여 0 반환).
 *
 * 동작:
 *   1) fopen.
 *   2) 한 라인씩 fgets_line 으로 읽고:
 *      - 선두 공백 스킵.
 *      - '#' 시작 또는 빈 줄은 스킵.
 *      - 줄 끝 "\\\n" continuation 처리(다음 라인을 이어붙임).
 *   3) parse_line 으로 위임. 파싱 에러 시 로그 후 다음 라인 진행
 *      (실패해도 read 자체는 성공으로 간주).
 *
 * 라인 번호는 에러 메시지에만 사용되며, continuation 시에도 정확히 카운트.
 *
 * 호출 체인:
 *   (구) main → spdk_conf_allocate → [spdk_conf_read]
 *     → fgets_line → parse_line → 트리 구축
 */
int
spdk_conf_read(struct spdk_conf *cp, const char *file)
{
	FILE *fp;
	char *lp, *p;
	char *lp2, *q;
	int line;
	int n, n2;

	if (file == NULL || file[0] == '\0') {
		return -1;
		/* [한국어] 파일 경로 인자 invalid. */
	}

	fp = fopen(file, "r");
	if (fp == NULL) {
		SPDK_ERRLOG("open error: %s\n", file);
		/* [한국어] 권한, 경로 오류 등. 표준 fopen 실패. */
		return -1;
	}

	cp->file = strdup(file);
	/* [한국어] 핸들에 파일 경로 보관 (디버깅 메시지에 사용). */
	if (cp->file == NULL) {
		SPDK_ERRLOG("cannot duplicate %s to cp->file\n", file);
		fclose(fp);
		return -1;
	}

	line = 1;
	/* [한국어] 사람이 읽는 1-base 라인 번호. */
	while ((lp = fgets_line(fp)) != NULL) {
		/* skip spaces */
		/* [한국어] 라인 선두 공백 스킵 — 인덴트 무시. */
		for (p = lp; *p != '\0' && isspace((int) *p); p++)
			;
		/* skip comment, empty line */
		if (p[0] == '#' || p[0] == '\0') {
			goto next_line;
			/* [한국어] 주석 또는 빈 줄. */
		}

		/* concatenate line end with '\' */
		/* [한국어] continuation 처리: 줄이 "\\\n" 으로 끝나면 다음 줄을 이어붙인다. */
		n = strlen(p);
		while (n > 2 && p[n - 1] == '\n' && p[n - 2] == '\\') {
			n -= 2;
			/* [한국어] '\\' 와 '\n' 두 글자 잘라낼 길이 조정. */
			lp2 = fgets_line(fp);
			if (lp2 == NULL) {
				break;
				/* [한국어] EOF — continuation 끝. */
			}

			line++;
			/* [한국어] 라인 번호 갱신 — continuation 도 별개 라인으로 카운트. */
			n2 = strlen(lp2);

			q = malloc(n + n2 + 1);
			/* [한국어] 새 합성 버퍼 — '\\' 제거된 앞부분 + 다음 라인 + NUL. */
			if (!q) {
				free(lp2);
				free(lp);
				SPDK_ERRLOG("malloc failed at line %d of %s\n", line, cp->file);
				fclose(fp);
				return -1;
			}

			memcpy(q, p, n);
			/* [한국어] '\\' 제거된 앞부분 복사. */
			memcpy(q + n, lp2, n2);
			/* [한국어] 다음 라인 본문 이어 붙이기. */
			q[n + n2] = '\0';
			free(lp2);
			free(lp);
			p = lp = q;
			/* [한국어] 작업 포인터들을 합성된 새 버퍼로 갱신.
			 * lp 는 free 대상, p 는 파싱용. 둘 다 같은 버퍼. */
			n += n2;
			/* [한국어] 합성 후 길이 갱신 — 이후 다시 continuation 검사 가능. */
		}

		/* parse one line */
		if (parse_line(cp, p) < 0) {
			SPDK_ERRLOG("parse error at line %d of %s\n", line, cp->file);
			/* [한국어] 파싱 에러는 로그만 남기고 read 는 계속 진행 — 부분
			 * 파싱 결과는 트리에 보존됨. 호출자가 무결성 검사를 별도로
			 * 수행해야 함. */
		}
next_line:
		line++;
		free(lp);
		/* [한국어] fgets_line 이 할당한 버퍼 해제. */
	}

	fclose(fp);
	return 0;
	/* [한국어] 파일 읽기 종료 — 라인 단위 에러는 무시되므로 거의 항상 0 반환. */
}

/*
 * [한국어]
 * spdk_conf_set_as_default - 글로벌 기본 핸들을 등록.
 *
 * @cp: 새 기본 핸들 (NULL 도 허용 — 기본 해제 의미).
 *
 * 등록 후에는 cp == NULL 인 모든 조회 함수가 이 핸들을 자동 사용한다.
 * 본 함수는 기존 default_config 를 free 하지 않으며, 이전 핸들의 수명은 호출자
 * 책임.
 */
void
spdk_conf_set_as_default(struct spdk_conf *cp)
{
	default_config = cp;
	/* [한국어] 단순 글로벌 갱신. 동시성 보호 없음 — 부팅 시 1회 호출 가정. */
}

/*
 * [한국어]
 * spdk_conf_disable_sections_merge - 같은 이름 섹션 병합을 비활성화한다.
 *
 * @cp: 핸들.
 *
 * spdk_conf_read 호출 전에 설정해야 의미가 있다(이미 파싱된 트리에는 영향 없음).
 * 비활성 시 [Foo] 가 두 번 나오면 별개 섹션 노드 두 개가 만들어진다.
 */
void
spdk_conf_disable_sections_merge(struct spdk_conf *cp)
{
	cp->merge_sections = false;
	/* [한국어] true → false 로 전환. */
}
