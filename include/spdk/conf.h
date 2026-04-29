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
 * 이 헤더는 SPDK가 초창기부터 사용해 온 **INI-스타일 텍스트 구성 파일**을
 * 읽고 섹션/키/값을 조회하는 API를 정의한다. 한 SPDK 앱(app/iscsi_tgt 등)이
 * 시작될 때 사용자가 만든 conf 파일(예: iscsi.conf, vhost.conf)을
 * `spdk_conf_read()`로 파싱하고, 각 서브시스템(target, bdev, transport 등)이
 * `spdk_conf_find_section()` + `spdk_conf_section_get_*val()` 콤보로 자기
 * 영역의 파라미터를 끄집어 쓰는 용도다.
 *
 * 중요 컨텍스트: SPDK 18.04 이후 공식 구성 메커니즘은 **JSON-RPC 기반**
 * (`-c` 옵션으로 JSON config 파일을 받거나 런타임에 RPC를 던지는 방식)으로
 * 전환되었다. 이 INI 파서는 후방호환을 위해 일부 모듈에 남아 있는 *legacy*
 * 경로이며, 신규 코드는 사용해서는 안 된다(상위 디렉토리의 RELEASE NOTES 및
 * include/spdk/jsonrpc.h 참조). 따라서 이 파일의 함수들은 사실상 **deprecated
 * 동결 API**로 취급된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (legacy 경로):
 *   spdk_app_start()  ─[옵션 -c 가 INI 파일이면]→ legacy conf 변환 레이어
 *      → spdk_conf_allocate()
 *      → spdk_conf_read(cp, file)        // 디스크의 텍스트를 파싱
 *      → spdk_conf_set_as_default(cp)    // 전역 기본 conf로 등록
 *      → 각 서브시스템 init 함수가 spdk_conf_find_section() / get_*val()
 *      → spdk_conf_free() (앱 종료 시)
 *
 * 실행 컨텍스트: 호스트 유저스페이스 단일 스레드(앱 init 단계).
 * I/O 핫패스에서는 호출되지 않는다. 따라서 락 없는 단순 자료구조이고
 * spdk_thread/reactor 모델과는 무관하다.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/stdinc.h: 표준 C 헤더 일괄 포함 (FILE*, bool, size_t 등)
 * 의존하는 모듈 (legacy):
 *   - app 측 인자 파서가 -c 옵션 처리 시 INI/JSON 자동 분기
 *   - 일부 module/event/, module/sock 등에서 옛 conf 섹션 lookup
 * 데이터 흐름:
 *   디스크 텍스트 파일 → spdk_conf_read() → 내부 linked list of (section,
 *   item, value) → 서브시스템이 read-only로 조회 → 앱 종료 시 free.
 * 공유 자료구조: 전역 default `struct spdk_conf *` (set_as_default로 지정).
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_conf_allocate / spdk_conf_free: conf 핸들 수명 관리
 *   - spdk_conf_read: INI 파일 디스크 → 메모리 파싱
 *   - spdk_conf_find_section / first / next: 섹션 탐색 (예: [Nvme], [Bdev])
 *   - spdk_conf_section_get_{val, nval, nmval}: 키→값 조회 (다중 인덱스 지원)
 *   - spdk_conf_section_get_{intval, boolval}: 정수/불리언 변환 헬퍼
 *   - spdk_conf_set_as_default: 전역 기본 conf 지정 (서브시스템이 암묵적 참조)
 *   - spdk_conf_disable_sections_merge: 동일 이름 섹션 자동 병합 비활성화
 *   - 구조체 spdk_conf / spdk_conf_section / spdk_conf_item / spdk_conf_value:
 *       opaque 핸들 — 내부 lib/conf/conf.c 에 정의
 */

#ifndef SPDK_CONF_H
#define SPDK_CONF_H
/* [한국어] include 가드 — 같은 컴파일 단위에서 중복 포함 시 재정의 충돌 방지 */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 C 헤더 묶음 — 이 헤더에서 등장하는 bool, char*, int 등의
 * 기본 타입과 (구현부에서 사용할) FILE*, size_t 등을 한꺼번에 가져온다.
 * SPDK는 빌드 환경 호환성을 위해 stdinc.h로 표준 헤더를 일원화한다. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 컴파일러가 이 헤더를 include할 때 C 링크 사양으로 심볼을 노출.
 * SPDK는 C로 작성됐지만 일부 사용자(QEMU 등)는 C++에서 호출 → name mangling 차단 필요. */
#endif

/* [한국어] 4개의 opaque struct 선언 — 모두 lib/conf/conf.c 내부에 실제 정의.
 * 사용자는 포인터로만 다루며, 필드 직접 접근은 불가능(캡슐화). */
struct spdk_conf_value;     /* [한국어] 키 하나에 대응하는 값 토큰 (공백 구분 다중 값의 한 조각) */
struct spdk_conf_item;      /* [한국어] "key = v1 v2 v3" 형태의 키 + 값 리스트 한 줄 */
struct spdk_conf_section;   /* [한국어] [Section] 헤더 하나 + 그 아래 item 리스트 */
struct spdk_conf;           /* [한국어] 한 conf 파일 전체 — section 리스트 컨테이너 */

/**
 * Allocate a configuration struct used for the initialization of SPDK app.
 *
 * \return a pointer to the allocated configuration struct.
 */
/*
 * [한국어]
 * spdk_conf_allocate - 빈 conf 핸들을 동적 할당
 *
 * @return: malloc된 spdk_conf 포인터 (실패 시 NULL).
 *          호출자가 spdk_conf_free로 해제 책임을 진다.
 *
 * 왜 필요한가: spdk_conf_read를 호출하기 전에 빈 컨테이너를 먼저 만들어야 한다.
 * 분리 이유는 read를 여러 번 호출해 누적 파싱하거나, read 없이 메모리 내에서
 * 인공 conf를 구성하는 시나리오를 허용하기 위함.
 *
 * 실행 컨텍스트: 앱 init(단일 스레드). 멀티스레드 진입 안전성 보장 안 함.
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
 * @cp: spdk_conf_allocate가 반환한 포인터. NULL이면 no-op (관례적 안전성).
 *
 * 왜 필요한가: conf 트리는 strdup/malloc 노드들의 연결 리스트라 수동 해제가 필수.
 * 앱 종료 시 누락하면 valgrind 등에서 leak으로 잡힌다.
 *
 * 실행 컨텍스트: 앱 fini 단계.
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
 * spdk_conf_read - 디스크의 INI 텍스트 파일을 파싱해 cp에 채워 넣음
 *
 * @cp:   spdk_conf_allocate로 미리 만든 빈/누적 conf 핸들
 * @file: 사용자 작성 conf 파일 경로 (예: "/etc/spdk/iscsi.conf")
 * @return: 0 성공, -1 실패 (파일 없음/파싱 에러). errno로 추가 진단 가능.
 *
 * 동작:
 *   1) fopen(file, "r")로 텍스트 모드 오픈
 *   2) 한 줄씩 읽어 [Section] / key = val / # 주석 토큰화
 *   3) 동일 이름 섹션은 disable_sections_merge가 꺼져 있으면 병합
 *   4) cp의 section list 끝에 추가
 *
 * 실행 컨텍스트: 앱 init. blocking I/O — reactor 시작 이전에만 호출할 것.
 *
 * 호출 체인:
 *   spdk_app_start (-c 옵션 분기) → spdk_conf_read → fgets / parse
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
 * spdk_conf_find_section - 이름이 정확히 일치하는 섹션을 cp에서 검색
 *
 * @cp:   대상 conf 핸들
 * @name: 섹션 이름 ([Bracketless] — 예: "Nvme", "Subsystem1")
 * @return: 일치 섹션 포인터, 없으면 NULL.
 *
 * 동작: cp의 section linked list를 strcmp로 선형 탐색. 섹션 수가 작아 O(N)로 충분.
 *
 * 실행 컨텍스트: 앱 init. 반환 포인터의 수명은 cp가 free될 때까지.
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
 * spdk_conf_first_section - 섹션 리스트 순회 시작점
 *
 * @cp:   대상 conf 핸들
 * @return: cp가 가진 첫 섹션 포인터. 빈 conf면 NULL.
 *
 * 왜 필요한가: 이름을 모르는 채 모든 섹션을 훑어야 하는 경우(예: prefix 매칭으로
 * "Subsystem*" 섹션 전체 처리)를 위한 이터레이터 시작점.
 *
 * 호출 체인:
 *   for (sp = spdk_conf_first_section(cp); sp; sp = spdk_conf_next_section(sp)) { ... }
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
 * spdk_conf_next_section - 현재 섹션의 다음 섹션 반환 (이터레이터 진행)
 *
 * @sp:   현재 섹션 (NULL 금지)
 * @return: 다음 섹션 포인터, 마지막이면 NULL.
 *
 * 호출 체인: spdk_conf_first_section과 짝을 이뤄 사용.
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
 * @name_prefix: 검사할 접두사 (예: "Subsystem")
 * @return: 일치 시 true, 아니면 false.
 *
 * 왜 필요한가: SPDK INI 관례상 인덱스가 붙는 섹션(예: Subsystem1, Subsystem2)을
 * 한꺼번에 처리할 때 사용. 각 섹션의 인덱스는 spdk_conf_section_get_num으로 얻는다.
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
 * @return: 내부 strdup된 이름 (read-only). 수명은 sp와 동일.
 *
 * 사용처: 로그/에러 메시지 출력, prefix 검사 후 인덱스 추출 등.
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
 * @return: 끝에 붙은 정수 (예: "Subsystem3" → 3). 숫자가 없으면 0.
 *
 * 왜 필요한가: SPDK INI 관습이 [Subsystem1], [Subsystem2]처럼 동종 섹션을
 * 인덱스로 구분하기 때문에, prefix 매칭 후 이 인덱스로 디바이스/타깃을 식별.
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
 * spdk_conf_section_get_nmval - 섹션 안에서 (key의 N번째 등장, 그 안의 M번째 토큰) 값 조회
 *
 * @sp:   섹션 핸들
 * @key:  찾을 키 이름 (예: "Listen")
 * @idx1: 같은 key가 여러 번 등장할 때 몇 번째인지 (0-based)
 * @idx2: 그 line의 값 토큰들 중 몇 번째인지 (공백 구분, 0-based)
 * @return: 해당 토큰 문자열 (read-only) 또는 NULL.
 *
 * 사용 예: 같은 섹션에 "Listen RDMA 192.168.0.1 4420"이 여러 줄 있을 때
 *   idx1=0,idx2=2 → "192.168.0.1"
 *   idx1=0,idx2=3 → "4420"
 *
 * 왜 두 인덱스인가: INI에서 같은 키 반복 + 공백 토큰 다중값을 모두 표현하기 위함.
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
 * spdk_conf_section_get_nval - get_nmval(sp, key, idx, 0)에 해당하는 단축 호출
 *
 * @sp:  섹션 핸들
 * @key: 키 이름
 * @idx: 같은 key의 등장 인덱스 (0-based)
 * @return: 해당 줄의 첫 값 토큰 또는 NULL.
 *
 * 키 한 줄에 단일 값만 있는 경우 가장 흔히 사용.
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
 * spdk_conf_section_get_val - get_nval(sp, key, 0)에 해당하는 가장 단순한 조회
 *
 * @sp:  섹션 핸들
 * @key: 키 이름
 * @return: 키의 첫 등장의 첫 토큰. 없으면 NULL.
 *
 * 단일 등장·단일 값 키(예: Name = "MyName")에 사용.
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
 * 주의: atoi 기반이라 0과 "0이 아닌 비-숫자(NULL)"를 구별 못 함.
 * 정확한 진단이 필요하면 get_val + strtol 직접 사용 권장.
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
 * @default_val:  키가 없거나 인식 불가능한 값일 때의 기본
 * @return: "Yes"/"Y"/"True" 계열이면 true, "No"/"N"/"False" 계열이면 false,
 *          그 외(키 없음 등)는 default_val.
 *
 * 사용처: 서비스 enable/disable 토글, 보안 옵션 활성화 여부 등.
 */
bool spdk_conf_section_get_boolval(struct spdk_conf_section *sp, const char *key, bool default_val);

/**
 * Set the configuration as the default.
 *
 * \param cp Configuration to set.
 */
/*
 * [한국어]
 * spdk_conf_set_as_default - 주어진 conf를 라이브러리 전역 기본 conf로 등록
 *
 * @cp: 기본으로 지정할 conf 포인터. NULL이면 기본 해제.
 *
 * 왜 필요한가: 일부 레거시 서브시스템 init이 conf 핸들을 인자로 받지 않고
 * 내부 전역에서 가져다 쓰는 패턴이라, 앱 시작 시 한 번 default를 박아둬야 함.
 *
 * 동시성: 앱 init 단일 스레드에서만 호출하는 것이 전제 — 락 없음.
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
 * 기본 동작: spdk_conf_read는 같은 이름의 [Section]이 두 번 등장하면 item을
 * 합쳐 하나의 섹션으로 만든다(편의 기능).
 * 이 함수를 호출해 두면 두 섹션을 분리해 유지 — 호출자가 first/next로 모두 순회 가능.
 *
 * 사용처: 동일 헤더의 인스턴스마다 별도 컨텍스트로 처리해야 하는 케이스.
 */
void spdk_conf_disable_sections_merge(struct spdk_conf *cp);

#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 닫기 */
#endif

#endif
/* [한국어] include 가드 종료 */
