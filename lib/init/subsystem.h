/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.  All rights reserved.
 */

/*
 * [한국어 설명] SPDK 초기화 서브시스템 내부 API 헤더 (subsystem.h)
 *
 * === 파일의 역할 ===
 * SPDK는 부팅 시 다양한 "서브시스템(subsystem)"을 정해진 의존성 순서대로 초기화한다.
 * 예: bdev_subsystem 는 thread_subsystem 이후에 떠야 하고, nvmf_subsystem 는
 * bdev_subsystem 이후에 뜬다. 본 헤더는 이러한 서브시스템 등록 테이블과 의존성 그래프를
 * 순회하기 위한 "내부" 헬퍼 함수들을 선언한다 (lib/init 모듈 안에서만 사용).
 * 외부 공개 API는 include/spdk/init.h에 있고, 이 파일은 init 자체 구현에서
 * 이름으로 검색하거나, JSON config dump 시 모든 서브시스템을 walk할 때 사용된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 초기화 흐름:
 *   spdk_app_start() → spdk_subsystem_init()
 *     → 의존성 그래프(topological sort) 계산
 *     → 각 서브시스템의 .init 콜백을 순서대로 호출
 *     → 모두 완료되면 사용자 메인 콜백(start_fn) 실행
 * 본 헤더의 함수들은 그래프 탐색/순회의 빌딩 블록이다. subsystem_find()는
 * 이름→포인터 해석에, get_first/get_next는 전체 walk(예: rpc save_config) 시,
 * subsystem_config_json()은 각 서브시스템이 RPC를 통해 자신의 현재 설정을
 * JSON으로 직렬화할 때 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk_internal/init.h (struct spdk_subsystem 정의), spdk/json.h
 * - 의존받음: lib/init/subsystem.c, lib/init/subsystem_rpc.c,
 *   lib/init/json_config.c (저장/로드 시 walk).
 * - 데이터 흐름: 모듈이 SPDK_SUBSYSTEM_REGISTER 매크로로 자신을 전역 리스트에
 *   등록 → init 코드가 본 헤더의 walker로 순회 → 각 서브시스템의 .init/.fini/.write_config
 *   콜백 호출.
 *
 * === 주요 함수/구조체 요약 ===
 * - subsystem_find          : 이름으로 서브시스템 검색.
 * - subsystem_get_first/next: 등록된 서브시스템 리스트 순회용 (TAILQ_FIRST/NEXT 추상화).
 * - subsystem_get_first_depend/next_depend: 의존성 엣지 리스트 순회.
 * - subsystem_config_json   : 한 서브시스템의 설정을 JSON으로 출력.
 * 자료구조 자체(struct spdk_subsystem, spdk_subsystem_depend)는 본 파일에 노출되지
 * 않고 spdk_internal/init.h에 정의되어 있다.
 */

#ifndef SPDK_SUBSYSTEM_H
#define SPDK_SUBSYSTEM_H
/* [한국어] 헤더 다중 포함 가드. */

/*
 * [한국어]
 * subsystem_find - 이름으로 등록된 서브시스템 객체를 검색.
 *
 * @name: 서브시스템 이름 문자열 (예: "bdev", "nvmf"). SPDK_SUBSYSTEM_REGISTER 시 부여됨.
 * @return: 존재하면 struct spdk_subsystem 포인터, 없으면 NULL.
 *
 * 의존성 등록(SPDK_SUBSYSTEM_DEPEND) 처리, RPC handler에서 특정 서브시스템 정보를
 * 끄집어낼 때 사용. 실행 컨텍스트: 초기화 또는 RPC 스레드(마스터). 등록 리스트는
 * 모듈 로드 시(.init_array)에 채워지므로 런타임 중 변경되지 않아 lock 불필요.
 *
 * 호출 체인:
 *   subsystem_init() / RPC → [subsystem_find] → 전역 g_subsystems TAILQ 선형 탐색
 */
struct spdk_subsystem *subsystem_find(const char *name);

/*
 * [한국어]
 * subsystem_get_first - 등록된 서브시스템 리스트의 첫 원소를 반환.
 *
 * @return: 첫 spdk_subsystem 포인터, 등록된 서브시스템이 없으면 NULL.
 *
 * subsystem_get_next와 짝을 이뤄 모든 서브시스템을 순회할 때 사용된다.
 *
 * 호출 체인:
 *   walk 코드 → [subsystem_get_first] → TAILQ_FIRST(&g_subsystems)
 */
struct spdk_subsystem *subsystem_get_first(void);

/*
 * [한국어]
 * subsystem_get_next - 주어진 서브시스템의 다음 원소를 반환.
 *
 * @cur_subsystem: 현재 위치를 가리키는 포인터(NULL 불가).
 * @return: 다음 노드 포인터 또는 끝일 경우 NULL.
 *
 * 호출 체인:
 *   walker → [subsystem_get_next] → TAILQ_NEXT(cur, tailq)
 */
struct spdk_subsystem *subsystem_get_next(struct spdk_subsystem *cur_subsystem);

/*
 * [한국어]
 * subsystem_get_first_depend - 의존성 엣지 리스트의 첫 원소를 반환.
 *
 * @return: 첫 spdk_subsystem_depend 포인터, 없으면 NULL.
 *
 * 의존성 엣지는 SPDK_SUBSYSTEM_DEPEND(sub, dep) 매크로로 등록된다 — "sub은 dep
 * 이후에 init/이전에 fini" 라는 단방향 그래프를 표현. topological sort 계산에 사용.
 */
struct spdk_subsystem_depend *subsystem_get_first_depend(void);

/*
 * [한국어]
 * subsystem_get_next_depend - 주어진 의존성 엣지의 다음 원소.
 *
 * @cur_depend: 현재 엣지 포인터.
 * @return: 다음 엣지 또는 NULL.
 */
struct spdk_subsystem_depend *subsystem_get_next_depend(struct spdk_subsystem_depend
		*cur_depend);

/**
 * Save pointed \c subsystem configuration to the JSON write context \c w. In case of
 * error \c null is written to the JSON context.
 *
 * \param w JSON write context
 * \param subsystem the subsystem to query
 */
/*
 * [한국어]
 * subsystem_config_json - 한 서브시스템의 현재 설정을 JSON으로 직렬화.
 *
 * @w         : 결과를 기록할 JSON write context (이미 시작된 array/object 안에서 호출됨).
 * @subsystem : 직렬화 대상 서브시스템.
 *
 * 각 서브시스템은 자체 write_config_json 콜백을 등록할 수 있고, 본 함수는 그 콜백을
 * 호출해 RPC "save_config" 응답을 만든다. 콜백이 없거나 실패한 경우 JSON에 null을 적는다.
 * 실행 컨텍스트: RPC handler 스레드.
 *
 * 호출 체인:
 *   spdk_subsystem_config_json (외부 API) → walker → [subsystem_config_json] → 모듈별 write_config_json
 */
void subsystem_config_json(struct spdk_json_write_ctx *w, struct spdk_subsystem *subsystem);

#endif
/* [한국어] 다중 포함 가드 종결. */
