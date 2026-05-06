/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Configuration file parser
 */

/*
 * [한국어 설명] SPDK 레거시 INI-스타일 구성 파일 파서 공개 헤더 (conf.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK가 초창기부터 사용해 온 **INI-스타일 텍스트 구성 파일**을 메모리로
 * 읽어들이고, 그 안에서 [Section] 단위로 키=값 라인을 조회하는 공개 API 묶음을
 * 정의한다. iSCSI/vhost/NVMe-oF 타깃 같은 SPDK 앱이 부팅될 때 사용자가 작성한
 * conf 파일(예: `/etc/spdk/iscsi.conf`, `vhost.conf`)을 `spdk_conf_read()`로
 * 1회성 파싱하고, 각 서브시스템(target, transport, bdev 백엔드 등) 초기화 함수가
 * `spdk_conf_find_section()` + `spdk_conf_section_get_*val()` 콤보로 자기 영역의
 * 파라미터를 끄집어 쓰는 용도다. 본 파일은 opaque 핸들 4종(spdk_conf,
 * spdk_conf_section, spdk_conf_item, spdk_conf_value)과 그 핸들을 다루는 함수
 * 17개의 선언만 담고 있으며, 실제 구현은 lib/conf/conf.c에 있다.
 *
 * 중요 컨텍스트(deprecation): SPDK 18.04 이후 공식 구성 메커니즘은 **JSON-RPC
 * 기반**(`-c` 옵션으로 JSON 구성 파일을 받거나 런타임에 RPC 호출을 던지는 방식)
 * 으로 전환되었다. 즉 신규 코드는 `include/spdk/json.h`, `jsonrpc.h`,
 * `init.h`(spdk_subsystem_load_config)을 거쳐 JSON 트리를 사용한다. 이 INI 파서
 * 는 후방호환을 위해 일부 모듈에 남아 있는 *legacy* 진입점이며, 사실상 동결
 * 상태다(SPDK RELEASE NOTES 및 `doc/jsonrpc.md` 참조). 본 헤더의 함수들은
 * deprecated에 가깝게 취급되며, 새 코드/모듈은 JSON 경로를 사용해야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (legacy 경로):
 *   spdk_app_start()  ─[옵션 -c 가 INI 파일이면 분기]→ legacy conf 변환 레이어
 *      → spdk_conf_allocate()                      // 빈 컨테이너 할당
 *      → spdk_conf_read(cp, file)                  // 디스크의 텍스트를 파싱
 *      → spdk_conf_set_as_default(cp)              // 전역 기본 conf로 등록
 *      → 각 서브시스템 init() 가 spdk_conf_find_section() + get_*val() 로 조회
 *      → spdk_conf_free() (앱 종료 시 1회)
 *
 * 현대(JSON) 경로와의 관계:
 *   spdk_app_start()
 *      → spdk_subsystem_init()
 *      → spdk_subsystem_load_config(json_fd, ...)  // include/spdk/init.h
 *         → JSON 파서 + 각 서브시스템의 RPC 핸들러를 직접 호출
 *   이 흐름이 표준이며, conf.h 의 함수들은 호출되지 않는다.
 *
 * 실행 컨텍스트: 호스트 유저스페이스 단일 스레드(앱 init 단계). I/O 핫패스나
 * reactor poll 루프에서는 호출되지 않는다. 따라서 락 없는 단순 자료구조이고,
 * spdk_thread/poller 모델과는 무관하다. blocking 파일 I/O를 사용하므로 반드시
 * reactor 시작 이전에만 호출해야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더):
 *   - spdk/stdinc.h: 표준 C 헤더 일괄 포함 (FILE*, bool, size_t, int 등)
 * 의존(구현):
 *   - lib/conf/conf.c — 본 헤더 함수들의 실체 + opaque 구조체 정의 + 파서 본체
 * 의존하는 모듈 (legacy로 일부 잔존):
 *   - app/* — `-c` 옵션 처리 시 INI/JSON 자동 분기
 *   - module/event/, 일부 옛 sock/iscsi 코드 — 옛 conf 섹션 lookup
 *   - 신규 모듈은 spdk_subsystem_load_config(JSON) 만 사용
 * 데이터 흐름:
 *   디스크 텍스트 파일  →  spdk_conf_read()  →  메모리 내 (section ↔ item ↔
 *   value) 의 단방향 linked list 트리  →  서브시스템이 read-only 로 키 조회
 *   →  앱 종료 시 spdk_conf_free() 한 번에 통째로 해제.
 * 공유 자료구조:
 *   - 라이브러리 전역 default `struct spdk_conf *` (set_as_default 로 지정).
 *     일부 레거시 init 함수가 인자 없이 이 전역에서 conf 를 가져다 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_conf_allocate / spdk_conf_free: conf 핸들 수명 관리(생성/해제)
 *   - spdk_conf_read: 디스크의 INI 텍스트 → 메모리 트리로 파싱
 *   - spdk_conf_find_section / first_section / next_section: 섹션 탐색·이터레이터
 *   - spdk_conf_section_match_prefix / get_name / get_num: 섹션 메타데이터 조회
 *   - spdk_conf_section_get_{val, nval, nmval}: 키→값 조회 (다중 인덱스 지원)
 *   - spdk_conf_section_get_{intval, boolval}: 정수/불리언 변환 헬퍼
 *   - spdk_conf_set_as_default: 전역 기본 conf 지정 (서브시스템이 암묵적 참조)
 *   - spdk_conf_disable_sections_merge: 동일 이름 섹션 자동 병합 비활성화
 *   - 구조체 (모두 opaque, 정의는 lib/conf/conf.c):
 *       spdk_conf          - 한 conf 파일 전체 (섹션 list 컨테이너)
 *       spdk_conf_section  - [Section] 헤더 1개 + 그 아래 item list
 *       spdk_conf_item     - "key = v1 v2 v3" 한 줄 (key + value list)
 *       spdk_conf_value    - 공백 구분 다중 토큰 중 한 조각
 */

#ifndef SPDK_CONF_H
/* [한국어] include 가드 매크로 — 동일 컴파일 단위에서 conf.h 가 두 번 이상
 * 포함될 때 typedef/forward 선언/함수 선언이 중복되어 컴파일 오류가 나는 것을
 * 방지한다. SPDK 공개 헤더 컨벤션은 `SPDK_<NAME>_H` 형태이며, 본 파일도 그를
 * 따른다. */
#define SPDK_CONF_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 C 헤더 묶음 — 본 헤더 인터페이스에서 등장하는 `bool`,
 * `char *`, `int` 등 기본 타입과, 구현부(spdk_conf_read의 fopen/fgets 등)가
 * 사용할 `FILE *`, `size_t` 등을 한꺼번에 가져온다. SPDK 는 빌드 환경
 * 호환성을 위해 stdinc.h 로 표준 헤더 인클루드를 일원화한다(직접 <stdio.h>
 * 등을 #include 하지 않는 컨벤션). */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 컴파일러가 이 헤더를 include 할 때 C 링크 사양으로 심볼을
 * 노출하도록 강제. SPDK 는 C 로 작성됐지만 일부 사용자(QEMU vhost 백엔드,
 * 외부 어플리케이션 등)는 C++ 코드에서 이 헤더를 포함할 수 있다. C++ 의
 * name mangling 이 적용되면 spdk_conf_read 같은 함수 심볼이 변형되어
 * 라이브러리에서 찾을 수 없게 되므로, extern "C" 블록으로 묶어 차단한다. */
#endif

/* [한국어] 4개의 opaque struct 전방 선언 — 모두 lib/conf/conf.c 내부에 실제
 * 정의되어 있다. 사용자는 포인터로만 핸들을 다루며, 필드에 직접 접근하는
 * 것은 캡슐화 위반이므로 금지된다. 향후 내부 표현이 바뀌어도 ABI 가 깨지지
 * 않도록 하기 위한 디자인이다. */
struct spdk_conf_value;
/* [한국어] 한 키의 값 토큰 하나에 대응하는 노드.
 * 설정자: spdk_conf_read 가 한 줄을 파싱하면서 공백 구분으로 토큰화할 때 생성.
 * 읽는 자: spdk_conf_section_get_nmval 이 idx2 로 인덱싱하여 char* 로 변환·반환.
 * 값 범위: 내부 strdup 된 NUL-종료 문자열 + 다음 토큰 포인터. opaque 이므로
 *          외부에서 직접 dereference 불가. */

struct spdk_conf_item;
/* [한국어] "key = v1 v2 v3" 형태의 한 줄(item)을 나타내는 노드. key 문자열과
 * spdk_conf_value linked list head 를 보유하며, 같은 섹션 안에서 형제
 * item 으로 연결된다.
 * 설정자: 파서가 'key = ...' 라인을 만나면 새 item 을 섹션에 push.
 * 읽는 자: get_nval/get_nmval 이 idx1 로 같은 key 의 N 번째 등장을 찾을 때 순회. */

struct spdk_conf_section;
/* [한국어] [Section] 헤더 하나에 대응하는 노드. 이름 문자열, 끝에 붙은 숫자
 * (Subsystem3 의 3 등), item linked list head 를 가진다. spdk_conf 의 자식.
 * 설정자: 파서가 '[Name]' 라인을 만나면 새 섹션을 conf 에 push (또는 merge).
 * 읽는 자: spdk_conf_find_section / first_section / next_section. */

struct spdk_conf;
/* [한국어] 한 conf 파일 전체에 대응하는 최상위 컨테이너. 섹션 linked list head
 * 와 sections-merge 플래그(disable_sections_merge 로 토글) 등을 보유.
 * 설정자: spdk_conf_allocate 가 zero-init 후 반환.
 * 읽는 자: spdk_conf_read(쓰기), spdk_conf_find_section(읽기) 등 모든 API.
 * 동기화: 앱 init 단일 스레드 사용 전제 — lock 없음. */

/**
 * Allocate a configuration struct used for the initialization of SPDK app.
 *
 * \return a pointer to the allocated configuration struct.
 */
/*
 * [한국어]
 * spdk_conf_allocate - 빈 conf 컨테이너 핸들을 동적 할당
 *
 * @return: malloc 된 spdk_conf 포인터 (실패 시 NULL).
 *          호출자가 사용 후 spdk_conf_free 로 해제할 책임을 진다.
 *
 * 왜 필요한가: spdk_conf_read 를 호출하기 전에 빈 컨테이너를 먼저 만들어야
 * 한다. allocate 와 read 를 분리한 이유는 (1) 같은 컨테이너에 read 를 여러
 * 번 호출해 누적 파싱하거나 (2) read 없이 메모리 내에서 인공 conf 트리를
 * 구성해 테스트하는 시나리오를 허용하기 위함이다.
 *
 * 동작:
 *   1) calloc(sizeof(struct spdk_conf), 1) 로 zero-init.
 *   2) 섹션 list head 와 disable_sections_merge 플래그를 0 으로 초기화.
 *   3) 핸들 포인터 반환.
 *
 * 실행 컨텍스트: 앱 init 단일 스레드. 멀티스레드 진입 안전성은 보장하지 않음.
 * 에러 경로: malloc 실패 시 NULL 반환. 호출자는 NULL 검사 후 fatal 처리.
 *
 * 호출 체인:
 *   app init → spdk_conf_allocate → (이후 spdk_conf_read 또는 직접 조립)
 */
struct spdk_conf *spdk_conf_allocate(void);

/**
 * Free the configuration struct.
 *
 * \param cp Configuration struct to free.
 */
/*
 * [한국어]
 * spdk_conf_free - conf 핸들과 그 하위 section/item/value 트리 전부 해제
 *
 * @cp: spdk_conf_allocate 가 반환한 포인터. NULL 이면 no-op (관례적 안전성).
 *
 * 왜 필요한가: conf 트리는 strdup/malloc 으로 만든 노드들의 다중 linked
 * list 라 수동 해제가 필수다. 앱 종료 시 누락하면 valgrind/asan 등에서
 * leak 으로 잡힌다. 또한 set_as_default 로 등록된 default 와 동일한 핸들
 * 이라면 별도로 default 를 NULL 로 풀어야 한다(이 함수는 그것을 자동으로
 * 처리하지 않으므로 호출 순서에 주의).
 *
 * 동작:
 *   1) cp->section list 를 순회하며 각 섹션의 item list 를 순회.
 *   2) item->value list 를 순회하며 각 value 의 strdup 문자열과 노드 free.
 *   3) item 자체 free.
 *   4) section 의 이름 문자열과 노드 free.
 *   5) 마지막으로 cp 자체 free.
 *
 * 실행 컨텍스트: 앱 fini 단계 단일 스레드.
 *
 * 호출 체인:
 *   spdk_app_fini → spdk_conf_free → 내부 free of sections/items/values
 */
void spdk_conf_free(struct spdk_conf *cp);

/**
 * Read configuration file for spdk_conf struct.
 *
 * \param cp Configuration struct used for the initialization of SPDK app.
 * \param file File to read that is created by user to configure SPDK app.
 *
 * \return 0 on success, -1 on failure.
 */
/*
 * [한국어]
 * spdk_conf_read - 디스크의 INI 텍스트 파일을 파싱해 cp 에 채워 넣음
 *
 * @cp:   spdk_conf_allocate 로 미리 만든 빈/누적 conf 핸들 (NULL 금지)
 * @file: 사용자 작성 conf 파일 경로 (예: "/etc/spdk/iscsi.conf")
 * @return: 0 성공, -1 실패 (파일 없음 / 권한 거부 / 파싱 에러).
 *          errno 로 추가 진단 가능 (fopen errno 가 그대로 전파됨).
 *
 * 동작 요약:
 *   1) fopen(file, "r") 로 텍스트 모드 오픈.
 *   2) fgets() 로 한 줄씩 읽으며 다음 토큰 인식:
 *        - '#' 로 시작하면 주석 (line 끝까지 무시)
 *        - '[Name]' → 새 섹션 시작 (또는 merge 플래그 켜져 있으면 기존 섹션
 *           에 이어 붙임)
 *        - 'key = v1 v2 v3' → item 추가, 공백 구분으로 value 토큰화
 *        - 빈 줄은 skip
 *   3) section / item / value 노드를 strdup + linked list push.
 *   4) fclose 후 0 반환. 중간 에러 시 부분 파싱된 트리는 그대로 남고 -1 반환
 *      (호출자가 spdk_conf_free 로 정리해야 함).
 *
 * 동일 이름 섹션 처리: 기본은 자동 병합(같은 이름이면 item 만 이어 붙임).
 * spdk_conf_disable_sections_merge 를 read 호출 전에 부르면 두 섹션을 분리
 * 유지하여 first/next 로 별개 인스턴스로 노출한다.
 *
 * 실행 컨텍스트: 앱 init. blocking I/O — reactor 시작 이전에만 호출할 것.
 * 에러 경로: 파일 미존재/권한 부족 시 -1 + errno. 호출자는 SPDK_ERRLOG 후
 * 종료하거나 폴백 처리.
 *
 * 호출 체인:
 *   spdk_app_start (-c 옵션 분기) → spdk_conf_read → fgets / parse loop
 */
int spdk_conf_read(struct spdk_conf *cp, const char *file);

/**
 * Find the specified section of the configuration.
 *
 * \param cp Configuration struct used for the initialization of SPDK app.
 * \param name Name of section to find.
 *
 * \return a pointer to the requested section on success or NULL otherwise.
 */
/*
 * [한국어]
 * spdk_conf_find_section - 이름이 정확히 일치하는 섹션을 cp 에서 검색
 *
 * @cp:   대상 conf 핸들 (NULL 이면 NULL 반환)
 * @name: 섹션 이름 (대괄호 제외 — 예: "Nvme", "Subsystem1")
 * @return: 일치 섹션 포인터, 없으면 NULL.
 *
 * 동작: cp 의 섹션 linked list 를 strcmp 로 선형 탐색. SPDK 의 conf 파일은
 * 섹션 수가 작아(보통 수십 개 이내) O(N) 선형 탐색으로 충분하다. 첫 번째
 * 매칭에서 멈춘다 — 이름이 같은 섹션이 둘 이상이면(merge 비활성화 케이스)
 * 첫 번째만 반환되며, 나머지는 first/next 로 순회해야 한다.
 *
 * 실행 컨텍스트: 앱 init 단일 스레드. 반환 포인터의 수명은 cp 가 free 될
 * 때까지 유효하며, read-only 로만 사용해야 한다.
 *
 * 호출 체인:
 *   각 서브시스템 init() → spdk_conf_find_section("MySection") → get_*val()
 */
struct spdk_conf_section *spdk_conf_find_section(struct spdk_conf *cp, const char *name);

/**
 * Get the first section of the configuration.
 *
 * \param cp Configuration struct used for the initialization of SPDK app.
 *
 * \return a pointer to the requested section on success or NULL otherwise.
 */
/*
 * [한국어]
 * spdk_conf_first_section - 섹션 리스트 순회 시작점 반환 (이터레이터 begin)
 *
 * @cp:   대상 conf 핸들
 * @return: cp 가 가진 첫 섹션 포인터. 빈 conf 면 NULL.
 *
 * 왜 필요한가: 이름을 미리 알지 못한 채 모든 섹션을 훑어야 하는 경우(예:
 * prefix 매칭으로 "Subsystem*" 섹션 전체를 디바이스마다 처리)를 위한
 * 이터레이터의 시작점. find_section 은 정확한 이름 매칭이라 이런 케이스에는
 * 부적합하다.
 *
 * 실행 컨텍스트: 앱 init 단일 스레드.
 *
 * 호출 체인:
 *   for (sp = spdk_conf_first_section(cp); sp; sp = spdk_conf_next_section(sp))
 *       { ...spdk_conf_section_match_prefix(sp, "Subsystem")... }
 */
struct spdk_conf_section *spdk_conf_first_section(struct spdk_conf *cp);

/**
 * Get the next section of the configuration.
 *
 * \param sp The current section of the configuration.
 *
 * \return a pointer to the requested section on success or NULL otherwise.
 */
/*
 * [한국어]
 * spdk_conf_next_section - 현재 섹션의 다음 섹션 반환 (이터레이터 advance)
 *
 * @sp:   현재 섹션 (NULL 금지 — first_section 이 NULL 을 반환했다면 루프
 *        진입 자체가 안 되므로 next 를 부를 일이 없다)
 * @return: 다음 섹션 포인터, 마지막이면 NULL.
 *
 * 동작: sp 노드의 next 포인터를 그대로 반환. 단순 linked list traversal.
 *
 * 호출 체인: spdk_conf_first_section 과 짝을 이뤄 사용 (위 예시 참조).
 */
struct spdk_conf_section *spdk_conf_next_section(struct spdk_conf_section *sp);

/**
 * Match prefix of the name of section.
 *
 * \param sp The section of the configuration.
 * \param name_prefix Prefix name to match.
 *
 * \return true on success, false on failure.
 */
/*
 * [한국어]
 * spdk_conf_section_match_prefix - 섹션 이름이 주어진 접두사로 시작하는지 검사
 *
 * @sp:          섹션 핸들
 * @name_prefix: 검사할 접두사 문자열 (예: "Subsystem")
 * @return: 일치 시 true, 아니면 false.
 *
 * 왜 필요한가: SPDK INI 관례상 인덱스가 붙는 섹션(예: Subsystem1, Subsystem2,
 * PortalGroup3)을 한꺼번에 처리할 때 사용한다. 보통 first/next 로 순회하면서
 * 이 함수로 prefix 일치 여부 확인 → 일치하면 spdk_conf_section_get_num 으로
 * 인덱스 추출 → 그 인덱스로 디바이스/타깃을 식별하는 패턴이다.
 *
 * 동작: strncmp(sp->name, name_prefix, strlen(name_prefix)) == 0 인지 검사.
 */
bool spdk_conf_section_match_prefix(const struct spdk_conf_section *sp, const char *name_prefix);

/**
 * Get the name of the section.
 *
 * \param sp The section of the configuration.
 *
 * \return the name of the section.
 */
/*
 * [한국어]
 * spdk_conf_section_get_name - 섹션의 전체 이름 문자열 반환
 *
 * @sp: 섹션 핸들 (NULL 금지)
 * @return: 내부 strdup 된 이름 문자열의 const 포인터. 수명은 sp 와 동일하며
 *          호출자가 free 해서는 안 된다.
 *
 * 사용처: 로그/에러 메시지 출력, prefix 검사 후 인덱스 추출, 사용자 표시 등.
 * 인덱스만 필요하면 spdk_conf_section_get_num 을 직접 사용하는 편이 간편하다.
 */
const char *spdk_conf_section_get_name(const struct spdk_conf_section *sp);

/**
 * Get the number of the section.
 *
 * \param sp The section of the configuration.
 *
 * \return the number of the section.
 */
/*
 * [한국어]
 * spdk_conf_section_get_num - 섹션 이름 끝의 숫자 부분 (인덱스) 반환
 *
 * @sp: 섹션 핸들
 * @return: 이름 끝에 붙은 정수 (예: "Subsystem3" → 3). 숫자가 없으면 0.
 *
 * 왜 필요한가: SPDK INI 관습이 [Subsystem1], [Subsystem2] 처럼 동종 섹션을
 * 인덱스로 구분하기 때문에, prefix 매칭(spdk_conf_section_match_prefix) 후
 * 이 인덱스로 실제 디바이스/타깃을 식별한다. 예: PortalGroup1 의 1 번 포털
 * 그룹을 가져올 때 사용.
 *
 * 동작: 파서가 섹션 생성 시 이름 뒤쪽의 숫자를 atoi 로 추출해 캐싱한 값을
 * 그대로 반환 (매 호출마다 재파싱하지 않음).
 */
int spdk_conf_section_get_num(const struct spdk_conf_section *sp);

/**
 * Get the value of the item with name 'key' in the section.
 *
 * If key appears multiple times, idx1 will control which version to retrieve.
 * Indices will start from the top of the configuration file at 0 and increment
 * by one for each new appearance. If the configuration key contains multiple
 * whitespace delimited values, idx2 controls which value is returned. The index
 * begins at 0.
 *
 *
 * \param sp The section of the configuration.
 * \param key Name of item.
 * \param idx1 The index into the item list for the key.
 * \param idx2 The index into the value list for the item.
 *
 * \return the requested value on success or NULL otherwise.
 */
/*
 * [한국어]
 * spdk_conf_section_get_nmval - 2 차원 인덱스(item idx, value idx)로 값 조회
 *
 * @sp:   섹션 핸들
 * @key:  찾을 키 이름 (예: "Listen")
 * @idx1: 같은 key 가 여러 번 등장할 때 몇 번째 줄인지 (0-based)
 * @idx2: 그 줄의 값 토큰들 중 몇 번째인지 (공백 구분, 0-based)
 * @return: 해당 토큰의 char* (read-only, 수명은 sp 와 동일) 또는 NULL.
 *
 * 사용 예: 같은 섹션에 다음과 같이 "Listen" 키가 여러 줄 있을 때
 *   Listen RDMA 192.168.0.1 4420
 *   Listen RDMA 192.168.0.2 4420
 * idx1=0, idx2=2 → "192.168.0.1"
 * idx1=0, idx2=3 → "4420"
 * idx1=1, idx2=2 → "192.168.0.2"
 *
 * 왜 두 인덱스인가: INI 포맷에서 (1) 같은 키가 여러 줄 반복되는 경우와
 * (2) 한 줄 안에서 공백 구분으로 다중 값을 갖는 경우를 모두 표현해야 하기
 * 때문이다. JSON 으로 옮겨오면 array of object 한 번이면 끝나므로, 이 API
 * 는 본질적으로 INI 의 한계 때문에 존재한다.
 *
 * 에러 경로: idx1 또는 idx2 가 범위를 벗어나거나 key 가 없으면 NULL 반환.
 * 호출자는 NULL 체크로 종료 조건을 판단할 수 있다.
 */
char *spdk_conf_section_get_nmval(struct spdk_conf_section *sp, const char *key,
				  int idx1, int idx2);

/**
 * Get the first value of the item with name 'key' in the section.
 *
 * \param sp The section of the configuration.
 * \param key Name of item.
 * \param idx The index into the value list for the item.
 *
 * \return the requested value on success or NULL otherwise.
 */
/*
 * [한국어]
 * spdk_conf_section_get_nval - get_nmval(sp, key, idx, 0) 단축 호출
 *
 * @sp:  섹션 핸들
 * @key: 키 이름
 * @idx: 같은 key 의 등장 인덱스 (0-based)
 * @return: 해당 줄의 첫 값 토큰 또는 NULL.
 *
 * 키 한 줄에 단일 값만 있는 경우(idx2 가 항상 0) 가장 흔히 사용한다. 예:
 *   Bdev myssd0
 *   Bdev myssd1
 * 에서 idx=0 → "myssd0", idx=1 → "myssd1".
 */
char *spdk_conf_section_get_nval(struct spdk_conf_section *sp, const char *key, int idx);

/**
 * Get the first value of the first item with name 'key' in the section.
 *
 * \param sp The section of the configuration.
 * \param key Name of item.
 *
 * \return the requested value on success or NULL otherwise.
 */
/*
 * [한국어]
 * spdk_conf_section_get_val - get_nval(sp, key, 0) 단축 호출 (가장 단순)
 *
 * @sp:  섹션 핸들
 * @key: 키 이름
 * @return: 키의 첫 등장의 첫 토큰 char* 또는 NULL.
 *
 * 단일 등장 + 단일 값 키(예: `Name = "MyTarget"`) 를 읽을 때 사용한다. SPDK
 * 레거시 INI 의 대부분의 키가 이 형태라 가장 빈번하게 호출된다.
 */
char *spdk_conf_section_get_val(struct spdk_conf_section *sp, const char *key);

/**
 * Get the first value of the first item with name 'key' in the section.
 *
 * \param sp The section of the configuration.
 * \param key Name of item.
 */
/*
 * [한국어]
 * spdk_conf_section_get_intval - 키의 첫 값을 정수(atoi)로 변환해 반환
 *
 * @sp:  섹션 핸들
 * @key: 키 이름
 * @return: 변환된 정수. 키가 없거나 변환 실패 시 -1.
 *
 * 동작: 내부적으로 spdk_conf_section_get_val(sp, key) 후 atoi 적용.
 *
 * 주의(중요): atoi 기반이라 다음 케이스를 구별할 수 없다 —
 *   (a) 키가 없음 → -1
 *   (b) 값이 숫자가 아님 → atoi 가 0 반환 (그러나 이 함수는 -1 처럼 보일 수
 *       있도록 별도 처리하지 않음)
 *   (c) 정상 값이 0 인 경우 → 0
 * 정확한 진단(존재성 vs 0)이 필요하면 get_val 로 NULL 검사를 먼저 하고
 * strtol(endptr) 로 직접 파싱하는 편이 안전하다.
 */
int spdk_conf_section_get_intval(struct spdk_conf_section *sp, const char *key);

/**
 * Get the bool value of the item with name 'key' in the section.
 *
 * This is used to check whether the service is enabled.
 *
 * \param sp The section of the configuration.
 * \param key Name of item.
 * \param default_val Default value.
 *
 * \return true if matching 'Yes/Y/True', false if matching 'No/N/False', default value otherwise.
 */
/*
 * [한국어]
 * spdk_conf_section_get_boolval - 키의 첫 값을 불리언으로 해석
 *
 * @sp:           섹션 핸들
 * @key:          키 이름
 * @default_val:  키가 없거나 인식 불가능한 값일 때 반환할 기본값
 * @return: "Yes"/"Y"/"True" 계열(대소문자 무시)이면 true,
 *          "No"/"N"/"False" 계열이면 false,
 *          그 외(키 없음, 인식 실패 등)는 default_val.
 *
 * 사용처: 서비스 enable/disable 토글, 보안 옵션 활성화 여부 등 on/off 설정.
 * default_val 인자 덕분에 호출자가 "기본은 켜짐 / 기본은 꺼짐" 정책을 명시할
 * 수 있다.
 */
bool spdk_conf_section_get_boolval(struct spdk_conf_section *sp, const char *key, bool default_val);

/**
 * Set the configuration as the default.
 *
 * \param cp Configuration to set.
 */
/*
 * [한국어]
 * spdk_conf_set_as_default - 주어진 conf 를 라이브러리 전역 기본 conf 로 등록
 *
 * @cp: 기본으로 지정할 conf 포인터. NULL 이면 기본 해제(default 슬롯 비움).
 *
 * 왜 필요한가: 일부 레거시 서브시스템 init() 함수가 conf 핸들을 인자로
 * 받지 않고 라이브러리 내부의 전역 default 를 암묵적으로 가져다 쓰는
 * 패턴이라, 앱 시작 시 사용자가 한 번 default 를 박아둬야 한다. 이는 SPDK
 * 18.04 이전의 설계 잔재이며, JSON-RPC 경로로 옮겨간 신규 코드는 이 전역
 * 을 사용하지 않는다.
 *
 * 동기화: 앱 init 단일 스레드에서만 호출하는 것이 전제 — 락 없음. reactor
 * 가 시작된 뒤에 이 값을 바꾸면 정의되지 않은 동작.
 *
 * 호출 체인:
 *   app init → spdk_conf_read(cp, file) → spdk_conf_set_as_default(cp)
 *      → 각 subsystem_init() 가 내부에서 default 참조
 */
void spdk_conf_set_as_default(struct spdk_conf *cp);

/**
 * Disable sections merging during 'spdk_conf_read()'
 *
 * \param cp Configuration to be read
 */
/*
 * [한국어]
 * spdk_conf_disable_sections_merge - 같은 이름 섹션 자동 병합 비활성화
 *
 * @cp: 대상 conf 핸들 (read 호출 전에 설정해야 효과 있음)
 *
 * 기본 동작: spdk_conf_read 는 같은 이름의 [Section] 헤더가 두 번 이상
 * 등장하면 두 섹션의 item 들을 한 섹션으로 합쳐 보관한다. 이는 같은 키를
 * 누적해서 늘릴 때 편리하다.
 *
 * 이 함수를 호출해 두면 두 섹션을 분리해 유지 — 호출자가 first_section /
 * next_section 으로 순회하면서 각 인스턴스를 별개로 처리할 수 있다.
 * 즉 "동일 헤더의 인스턴스마다 별도 컨텍스트(별도 디바이스, 별도 포털 등)
 * 로 처리" 해야 하는 케이스에 사용한다.
 *
 * 호출 순서: spdk_conf_allocate → spdk_conf_disable_sections_merge →
 *            spdk_conf_read 순서를 지켜야 한다. read 가 끝난 뒤 호출하면
 *            이미 병합된 트리에는 영향이 없다.
 */
void spdk_conf_disable_sections_merge(struct spdk_conf *cp);

#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 닫기 — C++ 컴파일 시 위에서 연 C 링키지 영역의
 * 종료 지점. 이 위치 이후의 헤더/매크로는 다시 C++ 기본 링키지로 돌아간다. */
#endif

#endif
/* [한국어] include 가드 (#ifndef SPDK_CONF_H) 종료. 이 endif 는 파일 최상단
 * 의 #ifndef 와 짝을 이루며, 동일 컴파일 단위에서 conf.h 가 두 번 이상
 * 인클루드되어도 전체 파일 내용이 한 번만 컴파일되도록 한다. */
