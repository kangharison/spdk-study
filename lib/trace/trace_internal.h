/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK trace 라이브러리 내부 전용 헤더 (trace_internal.h)
 *
 * === 파일의 역할 ===
 * lib/trace/ 디렉토리 내부의 .c 파일들끼리만 공유하는 private API를 선언한다.
 * 공개 헤더 spdk/trace.h가 외부 라이브러리(lib/nvme, lib/bdev 등)에 노출하는
 * "사용자용" API라면, 이 헤더는 trace.c ↔ trace_flags.c ↔ trace_rpc.c 사이의
 * 협력에 필요한 함수들만 모아둔다. 외부 모듈은 절대 include 하지 않는다.
 * 선언된 심볼은 (1) shm 파일명 조회 (2) flag 서브시스템 lifecycle 두 종류이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * trace 라이브러리 내부 토폴로지:
 *   trace.c        — shm/mmap 관리 + _spdk_trace_record() (hot path)
 *   trace_flags.c  — owner/object/tpoint 메타데이터 등록 + tpoint mask 토글
 *   trace_rpc.c    — JSON-RPC 핸들러 (trace_set_tpoint_mask 등)
 * 의존 방향:
 *   trace.c → trace_flags_init/fini() (lifecycle 위임)
 *   trace_rpc.c → trace_get_shm_name() (RPC 응답에 shm 경로 포함)
 *   trace_flags.c는 본 헤더의 함수를 정의(소비자 아님)
 * 실행 컨텍스트: 모두 SPDK app 초기화/종료 경로 또는 RPC 핸들러 컨텍스트
 *               (shm_open/munmap/pthread_spin 등 비-DPDK 일반 POSIX).
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/trace.h — 본 내부 헤더가 공개 API의 상위 집합으로 동작.
 *                            spdk/trace.h를 include하면 g_trace_file 등 핵심
 *                            전역과 구조체 정의가 함께 따라옴.
 * 의존(구현): trace.c (trace_get_shm_name 정의), trace_flags.c (flags_init/fini 정의).
 * 사용처: trace_rpc.c가 rpc_trace_get_info 응답을 만들 때 shm 경로를 가져오기
 *         위해 trace_get_shm_name 호출. trace.c의 spdk_trace_init/cleanup가
 *         trace_flags_init/fini를 호출하여 owner ID 풀과 reg_fn 일괄 호출.
 * 데이터 흐름: spdk_trace_init() → trace_flags_init() → 모든 reg_fn 호출
 *              → owner ring 초기화 → spdk_trace_cleanup() → trace_flags_fini().
 *
 * === 주요 함수/구조체 요약 ===
 *   - trace_get_shm_name(): /dev/shm에 매핑된 trace 파일명 반환 (RPC 응답용).
 *   - trace_flags_init(): reg_fn 일괄 호출 + owner_id ring buffer 할당/초기화.
 *   - trace_flags_fini(): owner_id ring buffer 해제 + spinlock destroy.
 */

#ifndef __TRACE_INTERNAL_H__   /* [한국어] include 가드 — 다중 포함 시 재정의 방지. */
#define __TRACE_INTERNAL_H__

#include "spdk/trace.h"        /* [한국어] 공개 trace API 정의 (g_trace_file, struct spdk_trace_*, 매크로 등).
                                * 본 헤더는 공개 헤더의 superset이므로, trace_internal.h만 include하면
                                * 공개 API도 자연스럽게 따라온다. */

/* Get shared memory file name. */
/*
 * [한국어]
 * trace_get_shm_name - 현재 trace 시스템이 매핑하고 있는 shm 파일 이름 반환.
 *
 * @return: g_shm_name 전역의 const char* (소유권 호출자에게 이전되지 않음 — 절대 free 금지).
 *
 * 동기: trace_rpc.c::rpc_trace_get_info가 응답 JSON에 "tpoint_shm_path"
 *       (예: "/dev/shm/_trace.spdk_tgt.12345")를 채우려면 init 시 사용된 이름이 필요.
 *       g_shm_name은 spdk_trace_init이 strncpy로 한 번 채우고 그 뒤로 변하지 않음.
 * 컨텍스트: RPC 핸들러 스레드(보통 main reactor)에서 호출 — read-only 접근이라 lock 불필요.
 * 호출 체인: RPC client → spdk_jsonrpc_server_handle_request → rpc_trace_get_info →
 *             trace_get_shm_name → snprintf "/dev/shm%s".
 */
const char *trace_get_shm_name(void);

/*
 * [한국어]
 * trace_flags_init - flag 서브시스템 부트스트랩 (owner ID ring + reg_fn 일괄 호출).
 *
 * @return: 0 성공 / 음수 errno (-ENOMEM = ring calloc 실패, pthread_spin_init 실패 시 그 값).
 *
 * 동작:
 *   1) g_reg_fn_head 링크드 리스트를 순회하며 각 모듈의 reg_fn() 호출
 *      → spdk_trace_register_owner_type/object/description_ext가 g_trace_file에
 *        메타데이터를 채움.
 *   2) owner_id 0은 "no owner" 예약, 1~255는 레거시 poller_id 충돌 회피로 skip,
 *      owner_id 256부터 g_trace_file->num_owners(=16K)까지 가용 ID로 ring에 push.
 *   3) pthread_spin_init으로 g_owner_ids.lock 생성.
 * 컨텍스트: spdk_trace_init() 후반부에서 1회 호출, 메인 스레드.
 * 호출 체인: spdk_app_start → spdk_trace_init → trace_flags_init.
 */
int trace_flags_init(void);

/*
 * [한국어]
 * trace_flags_fini - flag 서브시스템 종료 (ring/lock 해제).
 *
 * 동작: g_owner_ids.ring이 NULL이면 즉시 return (init 실패 후 안전 호출 가능),
 *       아니면 pthread_spin_destroy + free(ring) + ring=NULL.
 * 컨텍스트: spdk_trace_cleanup()의 첫 단계에서 호출 (mmap 해제 전).
 * 호출 체인: spdk_app_stop → spdk_trace_cleanup → trace_flags_fini.
 */
void trace_flags_fini(void);

#endif  /* [한국어] __TRACE_INTERNAL_H__ — include 가드 닫기. */
