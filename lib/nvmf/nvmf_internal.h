/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2025, Oracle and/or its affiliates.
 */

/*
 * [한국어 설명] NVMe-oF target 핵심 내부 헤더 (nvmf_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK NVMe-over-Fabrics target 구현(lib/nvmf/*.c)이 공유하는 모든 핵심 내부 자료구조와
 * 함수 선언을 모은 파일이다. 공개 헤더(include/spdk/nvmf.h)는 외부 애플리케이션용 API만
 * 포함하므로, 컨트롤러/서브시스템/qpair/네임스페이스의 실제 내부 표현(상태 머신 enum,
 * 구조체 필드, 락, 콜백 등)은 모두 여기에 정의된다.
 * 즉 본 파일은 "NVMe-oF target의 내부 데이터 모델"의 표준 정의이며, ctrlr.c, subsystem.c,
 * nvmf.c, transport.c, ctrlr_bdev.c 등 모든 코어 모듈이 이 헤더를 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe-oF target 객체 계층:
 *   spdk_nvmf_tgt (1개 또는 여러 개)
 *     ├── subsystem_tree (RB-tree of spdk_nvmf_subsystem)
 *     │     └── spdk_nvmf_subsystem
 *     │           ├── listeners (TAILQ of spdk_nvmf_subsystem_listener)
 *     │           ├── ctrlrs (TAILQ of spdk_nvmf_ctrlr) -- 호스트별 세션
 *     │           │     └── admin_qpair / qpair_mask (I/O qpairs)
 *     │           │           └── spdk_nvmf_request -- 명령 캡슐
 *     │           ├── ns[] (array of spdk_nvmf_ns) -- 네임스페이스 = bdev 매핑
 *     │           │     └── registrants/holder (Reservation 상태)
 *     │           └── hosts (NQN 화이트리스트)
 *     ├── transports (TAILQ of spdk_nvmf_transport) -- RDMA/TCP/FC
 *     ├── poll_groups (TAILQ of spdk_nvmf_poll_group) -- reactor당 1개
 *     │     └── transport poll groups + subsystem_pg state
 *     └── referrals (CDC 연동용)
 * 호출 체인:
 *   상위 사용자: app (nvmf_tgt), bdev_subsystem, RPC handlers (nvmf_rpc.c)
 *   본 헤더: 공통 모델 정의
 *   하위 사용자: ctrlr.c (admin/io 명령), subsystem.c (생명주기), nvmf.c (target 관리),
 *              ctrlr_bdev.c (bdev 위임), auth.c (DH-HMAC-CHAP), 각 transport 모듈
 * 실행 컨텍스트: 호스트 유저스페이스, SPDK reactor 스레드. 각 객체가 어느 thread에 묶이는지는
 * 필드 단위 주석에서 명시 (예: subsystem->thread, ctrlr->thread, qpair는 poll group thread).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 공개 헤더:
 *   - spdk/nvmf.h: 공개 API에서 노출되는 forward decl (spdk_nvmf_qpair, spdk_nvmf_request 등)
 *   - spdk/nvmf_transport.h: 트랜스포트 ops 인터페이스
 *   - spdk/nvmf_spec.h, spdk/nvmf_cmd.h: NVMe-oF 명령/응답 캡슐 스펙 정의
 *   - spdk/nvme.h, spdk/bdev.h: NVMe 명령어/완료, bdev 백엔드 추상화
 *   - spdk/thread.h, spdk/keyring.h: SPDK thread, 키 관리
 *   - spdk/queue.h, spdk/tree.h, spdk/bit_array.h: TAILQ/RB/bit array 자료구조
 * 데이터 흐름:
 *   호스트 -> 트랜스포트 -> spdk_nvmf_request 객체 -> ctrlr.c -> spdk_nvmf_ns -> bdev
 *   응답: bdev cb -> nvmf_transport_req_complete -> 트랜스포트 -> 호스트
 * 공유 상태:
 *   - spdk_nvmf_tgt::mutex: target/subsystem 등록 보호
 *   - spdk_nvmf_subsystem::mutex: hosts/listeners(non-IO 경로) 보호
 *   - subsystem state (PAUSED 시 IO 정지) 기반의 quiesce 모델
 *
 * === 주요 함수/구조체 요약 ===
 * 구조체:
 *   - spdk_nvmf_tgt: target (전역 컨테이너) - subsystem들과 transport들 보유
 *   - spdk_nvmf_subsystem: NVMe Subsystem (NQN 단위) - ctrlr/ns/host/listener
 *   - spdk_nvmf_ctrlr: NVMe-oF controller (호스트 세션, cntlid 식별)
 *   - spdk_nvmf_ns: 네임스페이스 (bdev에 1:1 매핑)
 *   - spdk_nvmf_subsystem_listener: 한 subsystem이 노출되는 트랜스포트 listener
 *   - spdk_nvmf_subsystem_pg_ns_info: poll group별 ns 상태 (Reservation 캐시)
 *   - spdk_nvmf_subsystem_poll_group: poll group의 subsystem별 상태
 *   - spdk_nvmf_async_event_completion / spdk_nvmf_reservation_log: AER/Reservation 큐 항목
 * 함수 (선언만, 구현은 각 .c):
 *   - nvmf_poll_group_*: poll group ↔ subsystem 변경 (add/remove/pause/resume/update)
 *   - nvmf_ctrlr_process_admin_cmd / process_io_cmd: 명령 디스패치 진입점
 *   - nvmf_bdev_ctrlr_*_cmd: NVMe 명령 -> bdev 호출로 변환 (read/write/dsm/copy/zcopy 등)
 *   - nvmf_subsystem_*: subsystem 상태/host/listener 관리
 *   - nvmf_ctrlr_async_event_*: AER (비동기 이벤트 통지) 트리거
 *   - nvmf_ns_reservation_*: Persistent Reservation 처리
 *   - 인라인 헬퍼: nvmf_ctrlr_get_ns / nvmf_ctrlr_ns_is_visible / nvmf_request_is_fabric_connect 등
 */

#ifndef __NVMF_INTERNAL_H__                         /* [한국어] 헤더 중복 포함 가드 */
#define __NVMF_INTERNAL_H__                         /* [한국어] 가드 매크로 정의 */

#include "spdk/stdinc.h"                            /* [한국어] 표준 라이브러리 (stdint, string 등) */

#include "spdk/keyring.h"                           /* [한국어] spdk_key 타입 - DH-HMAC-CHAP PSK 관리 */
#include "spdk/likely.h"                            /* [한국어] spdk_likely/unlikely 분기 힌트 - hot path 최적화 */
#include "spdk/nvmf.h"                              /* [한국어] 공개 API 타입 (spdk_nvmf_qpair/request/poll_group 등) forward decl */
#include "spdk/nvmf_cmd.h"                          /* [한국어] NVMe-oF 명령 캡슐 layout (Connect/Property Get/Set 등) */
#include "spdk/nvmf_transport.h"                    /* [한국어] 트랜스포트 ops 인터페이스 - 본 헤더는 spdk_nvmf_transport_poll_group 타입 사용 */
#include "spdk/nvmf_spec.h"                         /* [한국어] NVMe-oF 스펙 상수 (NQN 길이, capsule size 등) */
#include "spdk/assert.h"                            /* [한국어] SPDK_STATIC_ASSERT - 컴파일 타임 검증 (구조체 크기/정렬) */
#include "spdk/bdev.h"                              /* [한국어] spdk_bdev/bdev_desc - 네임스페이스가 매핑되는 백엔드 */
#include "spdk/queue.h"                             /* [한국어] TAILQ/STAILQ/LIST 매크로 - 모든 리스트 자료구조 */
#include "spdk/util.h"                              /* [한국어] SPDK_COUNTOF 등 잡유틸 */
#include "spdk/thread.h"                            /* [한국어] spdk_thread, spdk_poller, spdk_io_channel - 스레드 모델 */
#include "spdk/tree.h"                              /* [한국어] RB_HEAD/RB_ENTRY 매크로 - subsystem RB tree */
#include "spdk/bit_array.h"                         /* [한국어] spdk_bit_array - subsystem_ids/qpair_mask/visible_ns 등 비트 인덱스 집합 */

/* The spec reserves cntlid values in the range FFF0h to FFFFh. */
#define NVMF_MIN_CNTLID 1                           /* [한국어] 컨트롤러 ID 최소값 - 0은 invalid, NVMe-oF 1.x: 동적 할당 시작점 */
#define NVMF_MAX_CNTLID 0xFFEF                      /* [한국어] 컨트롤러 ID 최대값 - NVMe-oF 스펙이 0xFFF0~0xFFFF를 reserved로 두기 때문 */

#define NVMF_DISC_KATO_IN_MS 120000                 /* [한국어] Discovery 컨트롤러의 Keep-Alive Timeout (KATO) 고정값 = 120초 (NVMe-oF 1.1) */
#define NVMF_KAS_TIME_UNIT_IN_MS 100                /* [한국어] Keep-Alive Support(KAS) 시간 단위 = 100ms - Identify Controller cdata.kas 단위 */
#define NVMF_DEFAULT_KAS 100                        /* [한국어] 기본 KAS = 100 (단위 100ms = 10초 권장 KA 송신 간격) */
#define NVMF_DEFAULT_MIN_KATO 10000                 /* [한국어] 권장 KATO 하한 = 10초 - 너무 짧으면 false-positive disconnect 위험 */

#if __has_attribute(nonstring)                      /* [한국어] GCC/Clang의 nonstring 속성 지원 여부 - 문자배열에 NUL이 없을 때 경고 억제 */
#define __spdk_nonstring __attribute__((nonstring)) /* [한국어] 지원 시 nonstring 속성 적용 - char[] 필드(SN/MN 등) 경고 회피 */
#else
#define __spdk_nonstring                            /* [한국어] 미지원 컴파일러에서는 빈 매크로로 폴백 */
#endif

/*
 * [한국어] target 전체의 상태 머신 - 일시 정지/재개를 통한 일관된 dump/migration 지원.
 * 일반 IO 시 RUNNING이며, RPC nvmf_pause_target 등으로 PAUSING/PAUSED 전이.
 */
enum spdk_nvmf_tgt_state {
	NVMF_TGT_IDLE = 0,
	/* [한국어] 초기 상태 - 아직 시작되지 않음. spdk_nvmf_tgt_create 직후.
	 * 설정자: spdk_nvmf_tgt_create (init).
	 * 읽는 자: tgt 시작 경로의 가드.
	 * 값 범위: 0 (열거 시작값). */

	NVMF_TGT_RUNNING,
	/* [한국어] 정상 운영 상태 - subsystem listen/connect/IO 모두 가능.
	 * 설정자: spdk_nvmf_tgt_listen 등 startup 완료 시.
	 * 읽는 자: 모든 IO 경로의 전제. */

	NVMF_TGT_PAUSING,
	/* [한국어] PAUSE 요청 후 모든 subsystem이 PAUSED로 전이되기를 기다리는 중간 상태.
	 * 설정자: spdk_nvmf_tgt_pause 진입.
	 * 읽는 자: subsystem state change 콜백 합산 후 PAUSED로 진행. */

	NVMF_TGT_PAUSED,
	/* [한국어] 모든 subsystem이 일시 정지된 상태 - 새 IO submit 차단, 진행 중 IO 완료 대기.
	 * 설정자: 모든 subsystem이 PAUSED 도달 시.
	 * 읽는 자: dump/migration 도구가 일관된 스냅샷 가능한 시점. */

	NVMF_TGT_RESUMING,
	/* [한국어] RESUME 요청 후 subsystem들이 ACTIVE로 복귀 중인 중간 상태.
	 * 설정자: spdk_nvmf_tgt_resume 진입.
	 * 읽는 자: 완료 시 RUNNING으로 복귀. */
};

/*
 * [한국어] 각 subsystem의 8단계 상태 머신.
 * Activate/Deactivate(파이프라인 셋업/해체)와 Pause/Resume(IO quiesce)을 분리.
 * 상태 전이는 nvmf_subsystem_state_change_ctx로 직렬화된다.
 */
enum spdk_nvmf_subsystem_state {
	SPDK_NVMF_SUBSYSTEM_INACTIVE = 0,
	/* [한국어] 생성 직후 또는 deactivate 완료 후 - listener에서 광고되지 않고 호스트도 connect 불가.
	 * 설정자: subsystem create / deactivate 완료.
	 * 읽는 자: ACTIVATING으로의 시작점 검증. */

	SPDK_NVMF_SUBSYSTEM_ACTIVATING,
	/* [한국어] start() 진입 후 모든 poll group에 subsystem을 add 하는 중간 상태.
	 * 설정자: spdk_nvmf_subsystem_start.
	 * 읽는 자: 모든 poll group이 add 완료하면 ACTIVE로 전이. */

	SPDK_NVMF_SUBSYSTEM_ACTIVE,
	/* [한국어] 정상 IO 처리 가능 상태.
	 * 설정자: 모든 PG가 add 완료된 후.
	 * 읽는 자: IO 명령 디스패치 진입 조건. */

	SPDK_NVMF_SUBSYSTEM_PAUSING,
	/* [한국어] pause 요청 후 outstanding IO 완료 + 새 IO 차단 진행 중.
	 * 설정자: spdk_nvmf_subsystem_pause.
	 * 읽는 자: 모든 PG가 quiesce되면 PAUSED. */

	SPDK_NVMF_SUBSYSTEM_PAUSED,
	/* [한국어] IO 완전 정지 - ns add/remove, host 변경 등 안전한 수정 가능.
	 * 설정자: PAUSING 완료.
	 * 읽는 자: namespace 추가/삭제 RPC 등이 이 상태를 요구. */

	SPDK_NVMF_SUBSYSTEM_RESUMING,
	/* [한국어] PAUSED에서 ACTIVE로 복귀 중 - 모든 PG가 다시 IO 수락 준비.
	 * 설정자: spdk_nvmf_subsystem_resume.
	 * 읽는 자: 완료 시 ACTIVE. */

	SPDK_NVMF_SUBSYSTEM_DEACTIVATING,
	/* [한국어] stop() 진입 - 모든 ctrlr disconnect + PG에서 subsystem 제거 진행 중.
	 * 설정자: spdk_nvmf_subsystem_stop.
	 * 읽는 자: 완료 시 INACTIVE. */

	SPDK_NVMF_SUBSYSTEM_NUM_STATES,
	/* [한국어] enum 카운트용 sentinel - 상태 수 = 7. 배열 크기에 사용. */
};

/*
 * [한국어] DH-HMAC-CHAP 인증 키 타입.
 * 호스트 키와 컨트롤러 키를 구분 저장 (양방향 인증 시 필요).
 */
enum nvmf_auth_key_type {
	NVMF_AUTH_KEY_HOST,
	/* [한국어] 호스트가 자신을 인증할 때 사용하는 PSK.
	 * 설정자: subsystem_add_host_dhchap_key RPC.
	 * 읽는 자: nvmf_subsystem_get_dhchap_key()에서 type별 dispatch. */

	NVMF_AUTH_KEY_CTRLR,
	/* [한국어] 컨트롤러가 호스트에 자신을 증명할 때 사용하는 PSK (양방향 인증).
	 * 설정자: subsystem_add_host_dhchap_ctrlr_key RPC.
	 * 읽는 자: 컨트롤러 응답 챌린지 생성 시. */
};

/*
 * Asynchronous Event Mask Bit
 *
 * [한국어] 호스트가 Set Features (FID=0x0B Async Event Configuration)으로 설정하는 마스크 비트 위치.
 * NVMe Base Spec - 비트가 1이면 해당 이벤트는 마스킹(통지 안 함), 0이면 통지.
 * 컨트롤러 측 ctrlr->notice_aen_mask가 이 비트들을 추적한다.
 */
enum spdk_nvme_async_event_mask_bit {
	/* Mask Namespace Change Notification */
	SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGE_MASK_BIT		= 0,
	/* [한국어] Namespace Attribute Change AEN 마스크 비트.
	 * 설정자: 호스트 Set Features.
	 * 읽는 자: nvmf_ctrlr_async_event_ns_notice가 발화 전 검사.
	 * 값 범위: 비트 위치 0. */

	/* Mask Asymmetric Namespace Access Change Notification */
	SPDK_NVME_ASYNC_EVENT_ANA_CHANGE_MASK_BIT		= 1,
	/* [한국어] ANA(Asymmetric Namespace Access) 상태 변경 AEN 마스크.
	 * 설정자: 호스트 Set Features.
	 * 읽는 자: nvmf_ctrlr_async_event_ana_change_notice.
	 * 값 범위: 비트 위치 1. */

	/* Mask Discovery Log Change Notification */
	SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE_MASK_BIT	= 2,
	/* [한국어] Discovery Log Page 변경 AEN 마스크 (Discovery 컨트롤러용).
	 * 설정자: 호스트 Set Features.
	 * 읽는 자: nvmf_ctrlr_async_event_discovery_log_change_notice.
	 * 값 범위: 비트 위치 2. */

	/* Mask Reservation Log Page Available Notification */
	SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL_MASK_BIT	= 3,
	/* [한국어] Persistent Reservation 로그 사용 가능 AEN 마스크.
	 * 설정자: 호스트 Set Features.
	 * 읽는 자: nvmf_ctrlr_async_event_reservation_notification.
	 * 값 범위: 비트 위치 3. */

	/* Mask Error Event */
	SPDK_NVME_ASYNC_EVENT_ERROR_MASK_BIT			= 4,
	/* [한국어] Error Information AEN 마스크 (controller fatal status 등).
	 * 설정자: 호스트 Set Features.
	 * 읽는 자: nvmf_ctrlr_set_fatal_status가 AER 발화 전 검사.
	 * 값 범위: 비트 위치 4. */
	/* 4 - 63 Reserved */
};

/*
 * [한국어] subsystem들을 NQN으로 정렬한 RB-tree의 헤드 타입 정의.
 * spdk_nvmf_tgt::subsystems 필드 타입으로 사용되며, subsystem_cmp(NQN strncmp)로 비교한다.
 */
RB_HEAD(subsystem_tree, spdk_nvmf_subsystem);

/*
 * [한국어] NVMe-oF target - 가장 외곽의 컨테이너 객체.
 * 한 프로세스에 여러 개를 만들 수 있으며(전역 g_nvmf_tgts TAILQ에 등록), 각 target은 독립적인
 * subsystem 집합/transport 집합/poll group 집합을 가진다. 일반적으로 nvmf_tgt 앱은 1개를 사용.
 */
struct spdk_nvmf_tgt {
	char					name[NVMF_TGT_NAME_MAX_LENGTH];
	/* [한국어] target 식별 이름 (RPC에서 참조).
	 * 설정자: spdk_nvmf_tgt_create(opts->name)에서 복사.
	 * 읽는 자: RPC nvmf_get_targets, 디버그 로그.
	 * 값 범위: NUL-terminated 문자열, 최대 NVMF_TGT_NAME_MAX_LENGTH 길이.
	 * 동기화: 생성 후 불변 - 락 불필요. */

	pthread_mutex_t				mutex;
	/* [한국어] target 내부 상태 보호 mutex.
	 * 설정자: tgt_create에서 init, destroy에서 destroy.
	 * 읽는 자: subsystem 추가/제거, listener 변경 등 non-IO 경로.
	 * 동기화: 다중 스레드(여러 reactor)가 subsystem 등록할 수 있어 보호 필요.
	 *         IO hot path에서는 사용하지 않음 - subsystem state로 quiesce. */

	uint64_t				discovery_genctr;
	/* [한국어] Discovery Generation Counter - listener/subsystem 변경 시 1 증가.
	 * 호스트가 본 변화를 감지해 Discovery Log를 다시 가져오게 하는 단조 증가 카운터.
	 * 설정자: nvmf_update_discovery_log 등 토폴로지 변경 시.
	 * 읽는 자: Discovery Log Page Header의 genctr 필드.
	 * 값 범위: 단조 증가 (wrap 시 host 호환성 트레이드오프).
	 * 동기화: mutex 또는 단일 thread 가정. */

	uint32_t				max_subsystems;
	/* [한국어] target에 등록 가능한 subsystem 최대 개수.
	 * 설정자: tgt_create opts->max_subsystems.
	 * 읽는 자: subsystem 추가 경로의 가드, subsystem_ids bit_array 크기.
	 * 값 범위: 1 ~ 0xFFEF (cntlid 한계 영향).
	 * 동기화: 생성 후 불변. */

	uint32_t				discovery_filter;
	/* [한국어] Discovery 응답 필터 마스크 (SPDK_NVMF_TGT_DISCOVERY_*_FILTER 비트들).
	 * 설정자: spdk_nvmf_tgt_set_discovery_filter RPC.
	 * 읽는 자: Discovery Log 빌드 시 어떤 listener를 host에게 노출할지 결정.
	 * 값 범위: 비트 OR 조합 (transport_type, address family, by NQN 등).
	 * 동기화: mutex 보호. */

	enum spdk_nvmf_tgt_state                state;
	/* [한국어] target 전체 상태 (IDLE/RUNNING/PAUSING/PAUSED/RESUMING).
	 * 설정자: tgt_pause/resume.
	 * 읽는 자: subsystem startup/shutdown 게이트.
	 * 동기화: mutex 보호. */

	struct spdk_bit_array			*subsystem_ids;
	/* [한국어] subsystem 슬롯 사용 여부 비트맵 - 새 subsystem id 할당 시 free 비트 검색.
	 * 설정자: tgt_create 시 max_subsystems 크기로 alloc, subsystem add/remove 시 set/clear.
	 * 읽는 자: spdk_nvmf_subsystem_create의 id 할당 로직.
	 * 값 범위: 비트 N=1이면 subsystem id N 사용 중.
	 * 동기화: mutex 보호. */

	struct subsystem_tree			subsystems;
	/* [한국어] NQN으로 정렬된 RB-tree - O(log N) NQN 검색용.
	 * 설정자: subsystem add/remove.
	 * 읽는 자: spdk_nvmf_tgt_find_subsystem(NQN 기반 빠른 lookup).
	 * 동기화: mutex 보호. */

	TAILQ_HEAD(, spdk_nvmf_transport)	transports;
	/* [한국어] target에 등록된 트랜스포트(RDMA/TCP/FC/vfio_user 등) 리스트.
	 * 설정자: spdk_nvmf_tgt_add_transport.
	 * 읽는 자: poll group 생성 시 모든 transport에 대해 group_create, listen 시 trtype lookup.
	 * 동기화: 생성 후 IO 경로에서는 read-only, 변경은 mutex 보호. */

	TAILQ_HEAD(, spdk_nvmf_poll_group)	poll_groups;
	/* [한국어] reactor마다 1개씩 만들어지는 poll group들의 리스트.
	 * 설정자: spdk_nvmf_poll_group_create 시 추가.
	 * 읽는 자: 라운드로빈 connect 분배(next_poll_group), state change 전파.
	 * 동기화: mutex 보호. */

	TAILQ_HEAD(, spdk_nvmf_referral)	referrals;
	/* [한국어] CDC 연동을 위한 Referral 엔트리 리스트 (Discovery Log에 노출).
	 * 설정자: nvmf_referral_create RPC.
	 * 읽는 자: Discovery Log 빌드 시.
	 * 동기화: mutex 보호. */

	/* Used for round-robin assignment of connections to poll groups */
	struct spdk_nvmf_poll_group		*next_poll_group;
	/* [한국어] 라운드로빈 다음 차례 poll group 포인터 - connect 분배 부하 균형.
	 * 설정자: connect 처리 시 advance.
	 * 읽는 자: 다음 connect.
	 * 값 범위: poll_groups TAILQ 안의 노드 (또는 첫 노드).
	 * 동기화: mutex 또는 atomic - 다중 connect 동시 처리 시 race 가능. */

	spdk_nvmf_tgt_destroy_done_fn		*destroy_cb_fn;
	/* [한국어] tgt_destroy 비동기 완료 콜백.
	 * 설정자: spdk_nvmf_tgt_destroy 호출자가 등록.
	 * 읽는 자: 모든 subsystem deactivate + transport destroy 후 호출.
	 * 값 범위: NULL 가능. */

	void					*destroy_cb_arg;
	/* [한국어] destroy_cb_fn에 전달될 사용자 컨텍스트.
	 * 설정자/읽는 자: destroy_cb_fn과 동일. */

	uint16_t				crdt[3];
	/* [한국어] Command Retry Delay Times (NVMe Identify Controller cdata.crdt 1/2/3).
	 * 호스트에 명령 재시도 지연을 알려 throttling 정책 표현.
	 * 설정자: spdk_nvmf_tgt_set_crdt RPC.
	 * 읽는 자: Identify Controller 응답 빌드.
	 * 값 범위: 100ms 단위 (NVMe Base Spec). */

	uint16_t				num_poll_groups;
	/* [한국어] 현재 등록된 poll_groups의 수 - O(1) 조회용 캐시.
	 * 설정자: poll_group create/destroy.
	 * 읽는 자: 분배 통계, RPC.
	 * 동기화: mutex 보호. */

	/* Allowed DH-HMAC-CHAP digests/dhgroups */
	uint32_t				dhchap_digests;
	/* [한국어] 허용 DH-HMAC-CHAP 다이제스트 비트마스크 (SHA-256/384/512 비트).
	 * 설정자: tgt_create opts.
	 * 읽는 자: AUTH 협상 시 호스트 제안 검증.
	 * 값 범위: SPDK_NVMF_DHCHAP_HASH_* 비트 OR. */

	uint32_t				dhchap_dhgroups;
	/* [한국어] 허용 DH 그룹 비트마스크 (NULL/2048/3072/4096/6144/8192 등).
	 * 설정자: tgt_create opts.
	 * 읽는 자: AUTH 협상.
	 * 값 범위: SPDK_NVMF_DHCHAP_DHGROUP_* 비트 OR. */

	TAILQ_ENTRY(spdk_nvmf_tgt)		link;
	/* [한국어] 전역 g_nvmf_tgts 리스트의 노드.
	 * 설정자: tgt_create에서 INSERT_TAIL.
	 * 읽는 자: spdk_nvmf_get_first_target/next 등 iteration.
	 * 동기화: 전역 nvmf 라이브러리 락 (g_tgts_mutex). */
};

/*
 * [한국어] subsystem에 등록된 호스트 식별자 (NQN 기반 ACL 항목).
 * subsystem->hosts TAILQ에 매달리며, 호스트 NQN별로 별개의 인증 키를 보유 가능.
 */
struct spdk_nvmf_host {
	char				nqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	/* [한국어] 허용 호스트의 NQN 문자열 (NUL terminated).
	 * 설정자: subsystem_add_host RPC.
	 * 읽는 자: Connect 시 host NQN 매칭, Discovery 응답 필터링.
	 * 값 범위: NQN 문법(nqn.YYYY-MM.<reverse-dns>:<id>) - 최대 SPDK_NVMF_NQN_MAX_LEN.
	 * 동기화: subsystem->mutex 보호. */

	struct spdk_key			*dhchap_key;
	/* [한국어] 호스트 인증용 PSK (DH-HMAC-CHAP 호스트 비밀).
	 * 설정자: subsystem_add_host RPC --dhchap-key 옵션.
	 * 읽는 자: nvmf_subsystem_get_dhchap_key(HOST type)에서 챌린지 검증.
	 * 값 범위: NULL이면 인증 미요구, 비-NULL이면 keyring에서 빌려온 reference.
	 * 동기화: subsystem->mutex로 set/get 보호. */

	struct spdk_key			*dhchap_ctrlr_key;
	/* [한국어] 양방향 인증 시 컨트롤러가 호스트에 자신을 증명하는 PSK.
	 * 설정자: subsystem_add_host --dhchap-ctrlr-key.
	 * 읽는 자: AUTH 응답 챌린지 생성.
	 * 값 범위: NULL=단방향만, 비-NULL=양방향 인증.
	 * 동기화: subsystem->mutex. */

	TAILQ_ENTRY(spdk_nvmf_host)	link;
	/* [한국어] subsystem->hosts 리스트의 노드.
	 * 설정자: add 시 INSERT_TAIL.
	 * 읽는 자: TAILQ_FOREACH로 ACL 검색.
	 * 동기화: subsystem->mutex. */
};

/*
 * [한국어] subsystem이 트랜스포트 listener를 통해 노출되는 인스턴스.
 * (subsystem, transport, trid) 튜플당 1개 - 같은 subsystem이 여러 IP/포트로 노출 가능.
 * Discovery Log에서 호스트가 보는 endpoint 정보의 원천.
 */
struct spdk_nvmf_subsystem_listener {
	struct spdk_nvmf_subsystem			*subsystem;
	/* [한국어] 이 listener가 속한 subsystem (back-pointer).
	 * 설정자: spdk_nvmf_subsystem_listen 시 설정.
	 * 읽는 자: listener 콜백에서 subsystem 컨텍스트 복원.
	 * 동기화: 생성 후 불변. */

	spdk_nvmf_tgt_subsystem_listen_done_fn		cb_fn;
	/* [한국어] listen 완료 비동기 콜백 (트랜스포트 레벨 listen 후 호출).
	 * 설정자: subsystem_listen 호출자.
	 * 읽는 자: nvmf_subsystem_listener_is_active 검사 및 완료 트리거. */

	void						*cb_arg;
	/* [한국어] cb_fn에 전달될 사용자 컨텍스트.
	 * 설정자/읽는 자: cb_fn과 동일. */

	struct spdk_nvme_transport_id			*trid;
	/* [한국어] 이 listener의 트랜스포트 주소 (trtype/adrfam/traddr/trsvcid).
	 * 설정자: subsystem_listen 시 alloc + copy.
	 * 읽는 자: Discovery Log entry 빌드, 트랜스포트 lookup.
	 * 값 범위: heap 할당된 사본 - listener 해제 시 free.
	 * 동기화: subsystem->mutex. */

	struct spdk_nvmf_transport			*transport;
	/* [한국어] trid->trtype에 매칭되는 트랜스포트 인스턴스.
	 * 설정자: subsystem_listen 시 trtype으로 lookup.
	 * 읽는 자: Discovery 응답 필터, qpair lookup. */

	enum spdk_nvme_ana_state			*ana_state;
	/* [한국어] ANA 그룹별 상태 배열 (Optimized/Non-Optimized/Inaccessible 등).
	 * 설정자: subsystem_listener_set_ana_state RPC.
	 * 읽는 자: ANA Log Page 빌드, IO 라우팅 결정.
	 * 값 범위: anagrpid - 1로 인덱싱, 크기 = max_nsid.
	 * 동기화: subsystem->mutex. */

	uint64_t					ana_state_change_count;
	/* [한국어] ANA 상태 변경 누적 카운터 - AER 트리거 조건 추적.
	 * 설정자: ana_state 변경 시 증가.
	 * 읽는 자: 이전 값과 비교해 AEN 발화 여부 결정. */

	uint16_t					id;
	/* [한국어] subsystem 내 listener 고유 ID (used_listener_ids bit_array에서 할당).
	 * 설정자: listener 추가 시.
	 * 읽는 자: 로깅, RPC, ana_log entry 빌드.
	 * 값 범위: 0 ~ NVMF_MAX_LISTENERS_PER_SUBSYSTEM - 1. */

	struct spdk_nvmf_listener_opts			opts;
	/* [한국어] listener별 옵션 (secure_channel, sock_impl 힌트 등).
	 * 설정자: subsystem_listen RPC.
	 * 읽는 자: 트랜스포트 listen 동작 (예: TCP TLS 활성화). */

	TAILQ_ENTRY(spdk_nvmf_subsystem_listener)	link;
	/* [한국어] subsystem->listeners 리스트의 노드.
	 * 동기화: subsystem->mutex. */
};

/*
 * [한국어] CDC(Centralized Discovery Controller) 연동을 위한 Referral.
 * 호스트가 이 target에 Discovery 요청을 보내면, referral 정보도 함께 응답해 다른 NVMe-oF
 * 인스턴스를 알린다 (TP4126 / NVMe-oF 1.1 Section 5.3).
 */
struct spdk_nvmf_referral {
	/* Target to which the referral belongs */
	struct spdk_nvmf_tgt *tgt;
	/* [한국어] 이 referral이 속한 target (back-pointer).
	 * 설정자: nvmf_referral_create.
	 * 읽는 자: 정리 시 tgt->referrals에서 제거. */

	/* Discovery Log Page Entry for this referral */
	struct spdk_nvmf_discovery_log_page_entry entry;
	/* [한국어] 이미 빌드된 Discovery Log Page Entry (호스트에 그대로 복사).
	 * 설정자: referral_create 시 trid에서 변환해 채움.
	 * 읽는 자: Discovery Log 응답 빌드.
	 * 값 범위: NVMe-oF 스펙 5.3.4 entry layout. */

	/* Transport ID */
	struct spdk_nvme_transport_id trid;
	/* [한국어] 가리키는 원격 NVMe-oF target의 트랜스포트 주소.
	 * 설정자: referral_create.
	 * 읽는 자: entry 빌드, RPC 응답. */

	/* Visible to these hosts */
	TAILQ_HEAD(, spdk_nvmf_host) hosts;
	/* [한국어] allow_any_host=false일 때만 사용 - referral이 보일 호스트 NQN 화이트리스트.
	 * 설정자: referral_add_host RPC.
	 * 읽는 자: Discovery 응답 빌드 시 host 매칭. */

	/* Visible to all hosts or not */
	bool allow_any_host;
	/* [한국어] true면 모든 호스트에 referral 노출, false면 hosts 리스트만.
	 * 설정자: referral_create 또는 후속 set RPC.
	 * 읽는 자: 응답 필터. */

	TAILQ_ENTRY(spdk_nvmf_referral) link;
	/* [한국어] tgt->referrals 리스트의 노드.
	 * 동기화: tgt->mutex. */
};

/*
 * [한국어] poll group 별 namespace 캐시 정보.
 * Reservation 상태(crkey/rtype/holder_id)는 모든 PG가 일관되게 보아야 하므로 ns 본체 변경 후
 * 각 PG의 이 사본도 갱신된다 (nvmf_subsystem_poll_group_update_ns_reservation).
 * 또한 PG-소유 io_channel(bdev 채널)을 캐시해 IO 라우팅 시 빠르게 접근.
 */
struct spdk_nvmf_subsystem_pg_ns_info {
	struct spdk_io_channel		*channel;
	/* [한국어] 이 ns의 bdev에 대한 PG-소유 io_channel.
	 * 설정자: poll_group에 subsystem add 시 spdk_bdev_get_io_channel.
	 * 읽는 자: read/write 등 IO 명령 디스패치 시 bdev 호출 인자로 사용.
	 * 값 범위: 유효한 spdk_io_channel; ns가 PG에 추가되지 않은 동안 NULL.
	 * 동기화: poll group thread 단독 사용. */

	struct spdk_uuid		uuid;
	/* [한국어] ns의 UUID 캐시 - bdev 변경 감지에 사용.
	 * 설정자: ns add/replace 시.
	 * 읽는 자: 일관성 검증.
	 * 동기화: PG thread. */

	/* current reservation key, no reservation if the value is 0 */
	uint64_t			crkey;
	/* [한국어] 현재 등록된 Reservation Key. 0이면 reservation 없음.
	 * 설정자: Reservation Acquire/Register/Release 처리 후 PG로 전파.
	 * 읽는 자: Reservation 검증 fast path.
	 * 동기화: subsystem PAUSED 후 일괄 전파, IO path는 PG thread 단독. */

	/* reservation type */
	enum spdk_nvme_reservation_type	rtype;
	/* [한국어] 현재 reservation 타입 (Write Exclusive, Exclusive Access 등).
	 * 설정자: Reservation 명령 처리.
	 * 읽는 자: IO 명령 시 reservation 검사. */

	/* Host ID which holds the reservation */
	struct spdk_uuid		holder_id;
	/* [한국어] reservation을 잡고 있는 호스트의 Host Identifier UUID.
	 * 설정자: Reservation Acquire.
	 * 읽는 자: IO 명령에서 호스트가 holder인지 확인. */

	/* Host ID for the registrants with the namespace */
	struct spdk_uuid		reg_hostid[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	/* [한국어] 등록된 호스트 ID 배열 - Reservation Register/Unregister 추적.
	 * 설정자: Register/Unregister 명령.
	 * 읽는 자: Reservation Report 응답 빌드.
	 * 값 범위: 최대 SPDK_NVMF_MAX_NUM_REGISTRANTS개. */

	uint64_t			num_blocks;
	/* [한국어] ns의 LBA 개수 캐시 (bdev->blockcnt).
	 * 설정자: ns add 시.
	 * 읽는 자: 명령의 LBA 범위 검증 fast path. */

	uint32_t			anagrpid;
	/* [한국어] 이 ns가 속한 ANA 그룹 ID.
	 * 설정자: ns_set_ana_group 등.
	 * 읽는 자: ANA log/응답 빌드 시. */

	struct {
		/* Generational counter for preempted hostids list */
		uint32_t			hostids_gen;
		/* [한국어] preempt-and-abort에서 preempt 대상 hostids 리스트의 generational counter.
		 * 설정자: 새 preempt 시 ns 본체에서 복사.
		 * 읽는 자: PG에서 본 것과 본체 비교 - mismatch 시 새 preempt 진행 중 인지.
		 * 동기화: PG thread, 본체는 ns 락. */

		/* Count of IOs preempt-and-abort is waiting on */
		uint64_t			io_waiting;
		/* [한국어] preempt-and-abort가 완료를 기다리는 IO 개수.
		 * 설정자: preempt 처리 시작 시 outstanding IO 수로 초기화.
		 * 읽는 자: 모든 IO complete 후 0이 되면 preempt 완료 처리.
		 * 동기화: PG thread 단독. */
	} preempt_abort;
	/* [한국어] Reservation Preempt-and-Abort 처리 상태 (NVMe Reservation Preempt 0x06).
	 * 호스트가 다른 호스트의 reservation을 preempt 시 진행 중 IO를 abort하기 위한 카운터들. */

	/* I/O outstanding to this namespace */
	uint64_t			io_outstanding;
	/* [한국어] 이 ns에 대해 진행 중인 IO 수 - subsystem PAUSE 시 0이 되기를 대기.
	 * 설정자: IO 시작 시 +1, 완료 시 -1 (atomic이 아닌 PG thread 단일 사용).
	 * 읽는 자: PG state change 시 quiesce 판정. */

	enum spdk_nvmf_subsystem_state	state;
	/* [한국어] PG 시각의 ns별 상태 (subsystem 전체 상태와 동기화 추적).
	 * 설정자: PG가 subsystem 상태 변경에 따라 갱신.
	 * 읽는 자: ns add/remove 등 partial 상태 변경 시 검사. */
};

/*
 * [한국어] poll group 변경 작업(add/remove/pause/resume)의 비동기 완료 콜백 타입.
 * cb_arg: 호출자가 등록한 컨텍스트.
 * status: 0 성공, 음수 errno.
 */
typedef void(*spdk_nvmf_poll_group_mod_done)(void *cb_arg, int status);

/*
 * [한국어] poll group의 subsystem별 상태.
 * (poll_group, subsystem) 쌍마다 1개 - subsystem 추가 시 PG들이 각각 보유.
 * pause/resume 콜백 직렬화에 핵심.
 */
struct spdk_nvmf_subsystem_poll_group {
	/* Array of namespace information for each namespace indexed by nsid - 1 */
	struct spdk_nvmf_subsystem_pg_ns_info	*ns_info;
	/* [한국어] PG-시각의 ns 정보 배열 (nsid-1 인덱스).
	 * 설정자: subsystem add to PG 시 alloc.
	 * 읽는 자: IO dispatch에서 ns별 정보 fast path 접근.
	 * 값 범위: 크기 = num_ns. */

	uint32_t				num_ns;
	/* [한국어] ns_info 배열 크기.
	 * 설정자: alloc 시.
	 * 읽는 자: 인덱스 범위 검증. */

	enum spdk_nvmf_subsystem_state		state;
	/* [한국어] PG가 본 subsystem의 현재 상태 (subsystem 본체와 동기화 진행 추적).
	 * 설정자: state change 콜백 처리 시.
	 * 읽는 자: IO submit 가드. */

	/* Number of ADMIN and FABRICS requests outstanding */
	uint64_t				mgmt_io_outstanding;
	/* [한국어] Admin/Fabrics 명령 outstanding 카운터 - subsystem PAUSE 시 quiesce 대기 대상.
	 * 설정자: admin/fabrics 명령 시작 시 +1, 완료 시 -1.
	 * 읽는 자: pause 진행 시 0 도달 검사. */

	spdk_nvmf_poll_group_mod_done		cb_fn;
	/* [한국어] 진행 중인 PG-subsystem state change의 완료 콜백.
	 * 설정자: nvmf_poll_group_*_subsystem 호출 시.
	 * 읽는 자: state change 완료 시 호출. */

	void					*cb_arg;
	/* [한국어] cb_fn에 전달될 컨텍스트. */

	TAILQ_HEAD(, spdk_nvmf_request)		queued;
	/* [한국어] subsystem이 PAUSED 상태일 때 큐잉되는 요청들 (resume 시 재실행).
	 * 설정자: PAUSE 중 도착한 요청 enqueue.
	 * 읽는 자: RESUME 시 dequeue 후 재처리.
	 * 동기화: PG thread 단독. */
};

/*
 * [한국어] Persistent Reservation 등록자(Registrant).
 * 한 ns에 등록한 호스트마다 1개 생성, ns->registrants 리스트에 매달림.
 */
struct spdk_nvmf_registrant {
	TAILQ_ENTRY(spdk_nvmf_registrant) link;
	/* [한국어] ns->registrants 리스트의 노드.
	 * 동기화: ns 락 (subsystem PAUSED 또는 nvmf_ns_reservation_request 직렬화). */

	struct spdk_uuid hostid;
	/* [한국어] 등록한 호스트의 Host Identifier UUID.
	 * 설정자: Reservation Register 처리.
	 * 읽는 자: Reservation Report, IO 검증.
	 * 값 범위: 호스트가 Set Features Host Identifier로 보낸 UUID. */

	/* Registration key */
	uint64_t rkey;
	/* [한국어] 등록 키 (CRKEY/NRKEY).
	 * 설정자: Register / Replace 처리.
	 * 읽는 자: Acquire/Release/Preempt에서 키 매칭. */

	uint16_t cntlid;
	/* [한국어] 등록 시점의 컨트롤러 ID - 호스트의 어느 ctrlr에서 등록했는지 추적.
	 * 설정자: Register.
	 * 읽는 자: Reservation Report. */
};

/*
 * [한국어] Reservation Preempt-and-Abort 진행 컨텍스트.
 * Preempt 시 영향받는 호스트들의 IO를 abort 대기하는 동안 hostids 리스트와 generational
 * counter를 사용해 인-플라이트 추적.
 */
struct spdk_nvmf_reservation_preempt_abort_info {
	/* preempted controllers */
	struct spdk_uuid hostids[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	/* [한국어] preempt 대상 호스트 UUID 배열 - 이 호스트들의 IO를 abort.
	 * 설정자: Preempt 명령 처리 시작 시.
	 * 읽는 자: IO 진입 시 preempt 대상이면 abort 처리.
	 * 값 범위: hostids_cnt까지 유효. */

	struct {
		uint8_t io_waiting_done:	1; /* IO waiting is complete */
		/* [한국어] IO 대기가 완료되었는지 (모든 outstanding이 abort 또는 complete).
		 * 설정자: io_waiting이 0 도달 시 또는 timer 만료.
		 * 읽는 자: preempt 완료 처리. */

		uint8_t rsvd_1:			7;
		/* [한국어] 예약 비트 - 비트필드 패딩. */
	};
	uint8_t rsvd_2[2];
	/* [한국어] 정렬용 패딩. */

	uint8_t hostids_cnt;
	/* [한국어] hostids 배열의 유효 개수.
	 * 설정자: Preempt 처리 시.
	 * 값 범위: 0 ~ SPDK_NVMF_MAX_NUM_REGISTRANTS (UINT8_MAX 이내 보증). */

	uint32_t hostids_gen; /* Generational counter every time the list changes */
	/* [한국어] hostids 리스트가 바뀔 때마다 증가하는 단조 카운터.
	 * 설정자: Preempt 처리 시 ++.
	 * 읽는 자: PG의 캐시(pg_ns_info.preempt_abort.hostids_gen)와 비교해 변경 감지. */

	struct spdk_poller *io_waiting_timer;
	/* [한국어] IO abort 대기 타임아웃 타이머.
	 * 설정자: Preempt 처리 시 등록.
	 * 읽는 자: 만료 시 강제 완료. */

	uint64_t io_waiting_timeout_ticks;
	/* [한국어] timer의 타임아웃 시점 (TSC ticks).
	 * 설정자: 타이머 등록 시 spdk_get_ticks() + 타임아웃.
	 * 읽는 자: timer 콜백 비교. */
};
SPDK_STATIC_ASSERT(SPDK_NVMF_MAX_NUM_REGISTRANTS <= UINT8_MAX, "hostids_cnt storage type");
/* [한국어] hostids_cnt가 uint8_t이므로 MAX_NUM_REGISTRANTS가 255를 초과하지 않음을 컴파일 타임 검증. */

/*
 * [한국어] NVMe-oF 네임스페이스 - subsystem 안에서 nsid로 구분되는 논리 디바이스.
 * 본체에서는 bdev 1개와 1:1 매핑되며, Reservation/PI/ZCOPY 같은 NVMe-스펙 속성을 보유.
 * 호스트별 visibility는 hosts/always_visible로 통제.
 */
struct spdk_nvmf_ns {
	uint32_t nsid;
	/* [한국어] Namespace ID (NVMe Spec - 1부터 시작, 0xFFFFFFFF는 broadcast).
	 * 설정자: spdk_nvmf_subsystem_add_ns_ext 시 할당.
	 * 읽는 자: 모든 IO 명령의 cmd.nsid 매칭, ns[] 인덱싱(nsid-1).
	 * 값 범위: 1 ~ subsystem->max_nsid. */

	uint32_t anagrpid;
	/* [한국어] 이 ns가 속한 ANA 그룹 ID.
	 * 설정자: ns 추가 시 또는 ns_set_ana_group RPC.
	 * 읽는 자: ANA Log Page 응답, listener의 ana_state 인덱싱. */

	struct spdk_nvmf_subsystem *subsystem;
	/* [한국어] 이 ns가 속한 subsystem (back-pointer).
	 * 설정자: ns 생성 시.
	 * 읽는 자: ns 콜백에서 컨텍스트 복원. */

	struct spdk_bdev *bdev;
	/* [한국어] 백엔드 bdev - 실제 IO를 처리하는 SPDK 블록 디바이스.
	 * 설정자: ns add 시 spdk_bdev_open_ext 결과로 설정.
	 * 읽는 자: nvmf_bdev_ctrlr_*_cmd가 spdk_bdev_*를 호출.
	 * 값 범위: 유효한 spdk_bdev (NULL이면 ns 미연결).
	 * 동기화: bdev open이 ns 생명주기 동안 보장. */

	struct spdk_bdev_desc *desc;
	/* [한국어] bdev 디스크립터 (open handle) - bdev 콜백 등록과 IO에 사용.
	 * 설정자: spdk_bdev_open_ext.
	 * 읽는 자: bdev IO 호출, spdk_bdev_get_io_channel.
	 * 동기화: ns 종료 시 close. */

	struct spdk_nvmf_ns_opts opts;
	/* [한국어] ns 옵션 (nguid, eui64, uuid, anagrpid 등 명시 메타).
	 * 설정자: ns add 시 호출자가 전달.
	 * 읽는 자: Identify Namespace 응답 빌드. */

	/* reservation notification mask */
	uint32_t mask;
	/* [한국어] Reservation 통지 마스크 - 어떤 reservation 이벤트를 호스트에 알릴지.
	 * 설정자: Set Features Reservation Notification.
	 * 읽는 자: Reservation 이벤트 발생 시 통지 발화 여부 결정.
	 * 값 범위: NVMe spec Fig. - SPDK_NVME_RESERVATION_*_MASK 비트 OR. */

	/* generation code */
	uint32_t gen;
	/* [한국어] Reservation Generation - reservation이 변경될 때마다 증가.
	 * 설정자: Acquire/Release/Preempt 처리 시 ++.
	 * 읽는 자: Reservation Report 응답에 포함, 호스트가 변화 감지. */

	/* registrants head */
	TAILQ_HEAD(, spdk_nvmf_registrant) registrants;
	/* [한국어] 등록된 호스트들의 리스트.
	 * 설정자: Reservation Register/Unregister.
	 * 읽는 자: Reservation Report, IO 검증. */

	/* Queued reservation requests: head is in-progress, rest are pending */
	STAILQ_HEAD(, spdk_nvmf_request) reservations;
	/* [한국어] reservation 명령들을 직렬 처리하기 위한 큐 - head는 진행 중, 나머지는 pending.
	 * 설정자: nvmf_ns_reservation_request 진입 시 enqueue, 완료 시 dequeue.
	 * 읽는 자: head 명령만 진행하므로 reservation 상태 머신이 race 없이 적용됨.
	 * 동기화: subsystem thread 단일 직렬화. */

	/* current reservation key */
	uint64_t crkey;
	/* [한국어] 현재 활성 reservation key (holder가 잡고 있는 키).
	 * 설정자: Acquire 성공 시.
	 * 읽는 자: IO/Acquire/Release/Preempt 시 키 매칭.
	 * 값 범위: 0이면 reservation 없음. */

	/* reservation type */
	enum spdk_nvme_reservation_type rtype;
	/* [한국어] 활성 reservation의 타입 (Write Exclusive 등).
	 * 설정자: Acquire 성공.
	 * 읽는 자: 명령 검증. */

	/* current reservation holder, only valid if reservation type can only have one holder */
	struct spdk_nvmf_registrant *holder;
	/* [한국어] 현재 reservation을 잡고 있는 registrant 포인터 (rtype이 single-holder인 경우만 유효).
	 * 설정자: Acquire 성공.
	 * 읽는 자: IO 검증.
	 * 값 범위: registrants 리스트 안의 노드 또는 NULL. */

	struct spdk_nvmf_reservation_preempt_abort_info *preempt_abort;
	/* [한국어] Preempt-and-Abort 진행 시 컨텍스트 (없으면 NULL).
	 * 설정자: Preempt 시 alloc, 완료 시 free.
	 * 읽는 자: 진행 중 IO들이 본인이 abort 대상인지 검사. */

	/* Persist Through Power Loss file which contains the persistent reservation */
	char *ptpl_file;
	/* [한국어] PTPL(Persist Through Power Loss) 활성 시 reservation 상태를 저장할 파일 경로.
	 * 설정자: ns add opts->ptpl_file.
	 * 읽는 자: Reservation 변경 후 파일에 persist. */

	/* Persist Through Power Loss feature is enabled */
	bool ptpl_activated;
	/* [한국어] PTPL 기능 활성 여부.
	 * 설정자: Set Features Reservation Persistence.
	 * 읽는 자: persist 여부 결정. */

	/* ZCOPY supported on bdev device */
	bool zcopy;
	/* [한국어] 이 ns의 bdev가 zero-copy 지원 여부.
	 * 설정자: ns add 시 nvmf_bdev_zcopy_enabled로 결정.
	 * 읽는 자: nvmf_ctrlr_use_zcopy 분기. */

	/* Command Set Identifier */
	enum spdk_nvme_csi csi;
	/* [한국어] Command Set Identifier - NVM(0)/KV(1)/ZNS(2) 등 ns의 명령 집합.
	 * 설정자: ns add opts->csi.
	 * 읽는 자: Identify Namespace iocs/cs 응답 빌드.
	 * 값 범위: NVMe Spec - SPDK_NVME_CSI_*. */

	/* Make namespace visible to controllers of these hosts */
	TAILQ_HEAD(, spdk_nvmf_host) hosts;
	/* [한국어] 이 ns가 보일 호스트 NQN 화이트리스트 (always_visible=false 시).
	 * 설정자: namespace_add_host RPC.
	 * 읽는 자: 컨트롤러 connect/Identify NS List 빌드 시 visible 결정. */

	/* Namespace is always visible to all controllers */
	bool always_visible;
	/* [한국어] true면 모든 host에 항상 visible (hosts 리스트 무시).
	 * 설정자: ns add opts.
	 * 읽는 자: visibility 결정. */

	/* Namespace id of the underlying device, used for passthrough commands */
	uint32_t passthru_nsid;
	/* [한국어] passthrough 모드에서 백엔드 NVMe 디바이스의 실제 nsid.
	 * subsystem.passthrough=true에서만 의미 있음 - host nsid != device nsid 가능.
	 * 설정자: ns add 시.
	 * 읽는 자: nvmf_bdev_ctrlr_nvme_passthru_io에서 cmd.nsid 치환. */
};

/*
 * NVMf reservation notification log page.
 *
 * [한국어] Reservation 변경 통지를 위해 컨트롤러가 보관하는 로그 페이지 항목.
 * 호스트가 Reservation Notification AER를 받고 Get Log Page (Reservation Notification)으로 수확.
 */
struct spdk_nvmf_reservation_log {
	struct spdk_nvme_reservation_notification_log	log;
	/* [한국어] NVMe Spec Reservation Notification Log entry 본체.
	 * 설정자: nvmf_ctrlr_reservation_notice_log에서 채움.
	 * 읽는 자: Get Log Page (LID=0x80) 응답 빌드.
	 * 값 범위: 스펙 4.1.1 - log_page_count, type, nsid 등. */

	TAILQ_ENTRY(spdk_nvmf_reservation_log)		link;
	/* [한국어] ctrlr->log_head 리스트의 노드 - 컨트롤러별 로그 큐. */

	struct spdk_nvmf_ctrlr				*ctrlr;
	/* [한국어] 이 로그가 속한 컨트롤러 (back-pointer). */
};

/*
 * NVMf async event completion.
 *
 * [한국어] AER(Asynchronous Event Request) 발화를 위한 큐 항목.
 * 호스트가 보낸 AER 명령이 부족(또는 마스킹)일 때 이 큐에 쌓아두었다가, 다음 AER 도착 시 즉시 응답.
 */
struct spdk_nvmf_async_event_completion {
	union spdk_nvme_async_event_completion		event;
	/* [한국어] AER 응답 정보 (event_type/event_info/log_page_id).
	 * 설정자: nvmf_ctrlr_async_event_*_notice가 enqueue 시 채움.
	 * 읽는 자: 다음 AER 도착 시 dequeue 후 응답에 복사. */

	STAILQ_ENTRY(spdk_nvmf_async_event_completion)	link;
	/* [한국어] ctrlr->async_events 큐의 노드. */
};

/*
 * This structure represents an NVMe-oF controller,
 * which is like a "session" in networking terms.
 *
 * [한국어] NVMe-oF 컨트롤러 - 호스트와 subsystem 간의 한 "세션".
 * 호스트가 Connect 명령으로 admin qpair를 만들면 ctrlr이 생성되고, 이후 IO qpair가 연결됨.
 * cntlid로 식별되며, subsystem->ctrlrs TAILQ에 매달림.
 * NVMe Property/Feature/AER 상태와 keep-alive/association 타이머를 보유.
 * 모든 ctrlr 작업은 ctrlr->thread (보통 admin qpair의 PG thread)에서 직렬 실행.
 */
struct spdk_nvmf_ctrlr {
	uint16_t			cntlid;
	/* [한국어] Controller ID - subsystem 안에서 unique.
	 * 설정자: nvmf_subsystem_gen_cntlid 또는 호스트 요청 cntlid.
	 * 읽는 자: 모든 컨트롤러 lookup, 응답 cdata.cntlid.
	 * 값 범위: NVMF_MIN_CNTLID ~ NVMF_MAX_CNTLID. */

	char				hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	/* [한국어] 호스트 NQN 사본 (Connect 시 받음).
	 * 설정자: ctrlr_create.
	 * 읽는 자: Discovery 필터, ACL, AER 통지 대상.
	 * 값 범위: NUL terminated NQN. */

	struct spdk_nvmf_subsystem	*subsys;
	/* [한국어] 이 ctrlr이 속한 subsystem (back-pointer).
	 * 설정자: ctrlr_create.
	 * 읽는 자: ns lookup, 명령 디스패치. */

	struct spdk_bit_array		*visible_ns;
	/* [한국어] 이 ctrlr에 visible한 ns 비트맵 (nsid-1 인덱싱).
	 * 설정자: ctrlr_create 시 host visibility 평가.
	 * 읽는 자: nvmf_ctrlr_ns_is_visible (IO 명령마다 빠른 가드).
	 * 값 범위: 비트 N=1이면 nsid N+1 visible.
	 * 동기화: ctrlr->thread 단일 사용. */

	struct spdk_nvmf_ctrlr_data	cdata;
	/* [한국어] Identify Controller 응답 데이터 캐시 (cdata.kas, oncs, mdts 등).
	 * 설정자: ctrlr_create 시 subsystem/transport에서 산출.
	 * 읽는 자: Identify 명령 응답 빌드.
	 * 값 범위: NVMe spec Identify Controller 4096B layout. */

	struct spdk_nvmf_registers	vcprop;
	/* [한국어] 가상 컨트롤러 레지스터 (CAP/VS/CC/CSTS/AQA/ASQ/ACQ).
	 * 설정자: Property Set fabric 명령.
	 * 읽는 자: Property Get fabric 명령, CC.EN 변화에 따른 enable/shutdown 처리.
	 * 값 범위: NVMe Base Spec - 32/64-bit 레지스터 본떠 정의. */

	struct spdk_nvmf_ctrlr_feat feat;
	/* [한국어] Feature 캐시 (arbitration, power management, temperature 등).
	 * 설정자: Set Features 처리.
	 * 읽는 자: Get Features 응답 빌드. */

	struct spdk_nvmf_qpair	*admin_qpair;
	/* [한국어] 이 ctrlr의 admin qpair (qid=0). 항상 1개.
	 * 설정자: 첫 Connect (admin) 시.
	 * 읽는 자: AER 응답 송신 경로(admin qpair로만 보냄), keep-alive 처리.
	 * 값 범위: 유효한 qpair 또는 NULL(아직 connect 전 - 거의 없음). */

	struct spdk_thread	*thread;
	/* [한국어] 이 ctrlr이 바인딩된 spdk_thread - admin qpair의 PG thread와 동일.
	 * 설정자: ctrlr_create 시.
	 * 읽는 자: cross-thread 호출 시 spdk_thread_send_msg 대상.
	 * 동기화: 모든 ctrlr 변경은 이 thread에서. */

	struct spdk_bit_array	*qpair_mask;
	/* [한국어] qid 사용 비트맵 - 새 IO qpair Connect 시 사용 가능한 qid 검색.
	 * 설정자: Connect 시 qid 할당, disconnect 시 해제.
	 * 읽는 자: Connect 처리, qid 검증.
	 * 값 범위: 크기 = cdata.maxcmd 등 max qpair 수. */

	const struct spdk_nvmf_subsystem_listener	*listener;
	/* [한국어] 이 ctrlr이 들어온 listener (어떤 trid를 통해 connect 했는지).
	 * 설정자: 첫 Connect 시 qpair의 listener를 복사.
	 * 읽는 자: ANA 상태 결정, Discovery 응답 일관성. */

	struct spdk_nvmf_request *aer_req[SPDK_NVMF_MAX_ASYNC_EVENTS];
	/* [한국어] 호스트가 미리 보낸 AER(Asynchronous Event Request) 슬롯들.
	 * 설정자: AER 명령 도착 시 비어있는 슬롯에 보관.
	 * 읽는 자: AER 발화 시 이 슬롯에서 꺼내 응답 (FIFO 아님 - 빠른 슬롯 사용).
	 * 값 범위: 최대 SPDK_NVMF_MAX_ASYNC_EVENTS개 (보통 4).
	 * 동기화: ctrlr->thread. */

	STAILQ_HEAD(, spdk_nvmf_async_event_completion) async_events;
	/* [한국어] AER 발화는 됐지만 호스트가 AER 슬롯을 안 보내 대기 중인 이벤트 큐.
	 * 설정자: notice 함수가 enqueue.
	 * 읽는 자: AER 명령 도착 시 즉시 dequeue 후 응답. */

	uint64_t notice_aen_mask;
	/* [한국어] 호스트가 Set Features (AsyncEventConfig)로 설정한 통지 마스크.
	 * 설정자: Set Features.
	 * 읽는 자: 각 AER 발화 함수가 비트 검사 후 발화 여부 결정. */

	uint8_t nr_aer_reqs;
	/* [한국어] 현재 보관 중인 aer_req 슬롯 수.
	 * 설정자: AER enqueue/dequeue 시 ±1.
	 * 읽는 자: 빈 슬롯 검색. */

	struct spdk_uuid  hostid;
	/* [한국어] 호스트가 Set Features Host Identifier로 보낸 UUID.
	 * 설정자: Set Features (FID=0x81).
	 * 읽는 자: Reservation registrant 등록, 호스트 식별. */

	uint32_t association_timeout; /* in milliseconds */
	/* [한국어] association 타임아웃 (ms) - admin qpair connect 후 IO qpair 미연결 시 끊는 시간.
	 * 설정자: subsystem 옵션 또는 기본값.
	 * 읽는 자: association_timer 등록 시 사용. */

	uint16_t changed_ns_list_count;
	/* [한국어] changed_ns_list 안의 유효 nsid 개수.
	 * 설정자: nvmf_ctrlr_ns_changed에서 ++ (overflow 시 0xFFFFFFFF 마커).
	 * 읽는 자: Get Log Page (Changed NS List) 응답. */

	struct spdk_nvme_ns_list changed_ns_list;
	/* [한국어] AER로 통지된 변경된 ns 목록 (Get Log Page (LID=0x04)에서 응답).
	 * 설정자: ns_changed 시 추가.
	 * 읽는 자: Log Page 응답 빌드. */

	uint64_t log_page_count;
	/* [한국어] reservation log 단조 카운터 - 각 reservation_log entry에 부여.
	 * 설정자: reservation_notice_log 시 ++.
	 * 읽는 자: log entry의 log_page_count 필드. */

	uint8_t num_avail_log_pages;
	/* [한국어] log_head에 쌓인 reservation log 엔트리 수.
	 * 설정자: log enqueue/dequeue.
	 * 읽는 자: AER 발화 여부 / 호스트 응답 시 사용 가능 페이지 수. */

	TAILQ_HEAD(log_page_head, spdk_nvmf_reservation_log) log_head;
	/* [한국어] 미수확 reservation log 페이지들의 큐 (호스트가 Get Log Page로 가져감). */

	/* Time to trigger keep-alive--poller_time = now_tick + period */
	uint64_t			last_keep_alive_tick;
	/* [한국어] 마지막 keep-alive 갱신 시점 (TSC ticks).
	 * 설정자: 호스트가 Keep Alive 명령 보낼 때마다.
	 * 읽는 자: keep_alive_poller가 만료 검사 (KATO 초과 시 disconnect). */

	struct spdk_poller		*keep_alive_poller;
	/* [한국어] keep-alive 만료 검사 주기 poller.
	 * 설정자: ctrlr_create 시 등록.
	 * 읽는 자: 만료 시 ctrlr disconnect. */

	struct spdk_poller		*association_timer;
	/* [한국어] association 타임아웃 타이머 (admin connect 후 IO qpair 미연결 감시).
	 * 설정자: ctrlr_create 시 등록.
	 * 읽는 자: 만료 시 ctrlr disconnect. */

	struct spdk_poller		*cc_timer;
	/* [한국어] CC.SHN(shutdown notification) 처리 진행 검사 타이머.
	 * 설정자: shutdown 시작 시.
	 * 읽는 자: 진행 상태 확인. */

	uint64_t			cc_timeout_tsc;
	/* [한국어] CC 변경(특히 shutdown)의 타임아웃 시점 TSC.
	 * 설정자: shutdown 시작 시 spdk_get_ticks() + timeout.
	 * 읽는 자: cc_timeout_timer가 만료 비교. */

	struct spdk_poller		*cc_timeout_timer;
	/* [한국어] CC 타임아웃 타이머 - shutdown이 너무 오래 걸리면 강제 종료.
	 * 설정자: shutdown 시작 시.
	 * 읽는 자: 만료 시 강제 disconnect. */

	bool				dif_insert_or_strip;
	/* [한국어] PI(Protection Information) 삽입/제거 활성 여부.
	 * 설정자: ctrlr_create 시 transport opts.
	 * 읽는 자: bdev 호출 시 dif 옵션 결정. */

	bool				in_destruct;
	/* [한국어] 컨트롤러가 destruct 진행 중인지 - 새 명령/AER 거부.
	 * 설정자: nvmf_ctrlr_destruct 진입 시.
	 * 읽는 자: 명령 디스패치 가드, AER enqueue. */

	bool				disconnect_in_progress;
	/* [한국어] 모든 qpair disconnect 진행 중.
	 * 설정자: disconnect 시작 시.
	 * 읽는 자: 추가 disconnect 요청 무시. */

	/* valid only when disconnect_in_progress is true */
	bool				disconnect_is_shn;
	/* [한국어] disconnect가 CC.SHN(정상 shutdown)에 의한 것인지 여부.
	 * 설정자: disconnect 진입 시.
	 * 읽는 자: 후처리 분기 (정상 종료 vs 강제 종료). */

	bool				executing_nssr;
	/* [한국어] NSSR(NVM Subsystem Reset) 진행 중.
	 * 설정자: NSSR 명령 처리 시.
	 * 읽는 자: 중복 reset 방지. */

	bool				acre_enabled;
	/* [한국어] Asynchronous Command Retry Enable - 호스트가 명령 재시도 활성화했는지.
	 * 설정자: Set Features Host Behavior.
	 * 읽는 자: 명령 실패 응답 시 retry 가능 표시. */

	bool				dynamic_ctrlr;
	/* [한국어] cntlid가 동적 할당됐는지 (정적 할당이면 false).
	 * 설정자: ctrlr_create 시.
	 * 읽는 자: destruct 시 cntlid 반환 결정. */

	/* LBA Format Extension Enabled (LBAFEE) */
	bool				lbafee_enabled;
	/* [한국어] LBAFEE - 64-bit LBA Format 등 확장 LBA 포맷 활성.
	 * 설정자: Set Features Host Behavior.
	 * 읽는 자: Identify NS, IO 명령 LBA 해석. */

	TAILQ_ENTRY(spdk_nvmf_ctrlr)	link;
	/* [한국어] subsystem->ctrlrs 리스트 노드.
	 * 동기화: subsystem state 또는 mutex 보호. */
};

#define NVMF_MAX_LISTENERS_PER_SUBSYSTEM	16
/* [한국어] 한 subsystem이 가질 수 있는 최대 listener 수 (used_listener_ids bit_array 크기). */

/*
 * [한국어] subsystem 상태 변경 작업 컨텍스트.
 * pause/resume/start/stop/ns add 등 상태 전이 요청을 큐에 직렬화 - 동시에 여러 변경이 들어와도
 * 순차 처리되도록 한다.
 */
struct nvmf_subsystem_state_change_ctx {
	struct spdk_nvmf_subsystem			*subsystem;
	/* [한국어] 변경 대상 subsystem. */

	uint16_t					nsid;
	/* [한국어] 부분 PAUSE 등에서 영향받는 nsid (전체 변경 시 0).
	 * 설정자: state change 요청자.
	 * 읽는 자: PG 단위 처리에서 ns 단위 분기. */

	enum spdk_nvmf_subsystem_state			original_state;
	/* [한국어] 변경 전 상태 - 실패 시 롤백 대상.
	 * 설정자: 진입 시 현재 state 저장.
	 * 읽는 자: 실패 처리. */

	enum spdk_nvmf_subsystem_state			requested_state;
	/* [한국어] 요청된 목표 상태 (ACTIVE/PAUSED/INACTIVE 등).
	 * 설정자: 요청 진입 시.
	 * 읽는 자: 진행 시 분기. */

	int						status;
	/* [한국어] 진행 중 누적 에러 코드 (0이면 성공).
	 * 설정자: PG 콜백에서 음수 errno 반영.
	 * 읽는 자: 최종 cb_fn에 전달. */

	struct spdk_thread				*thread;
	/* [한국어] 요청을 시작한 thread - 완료 콜백을 어디서 호출할지 결정.
	 * 설정자: 진입 시 spdk_get_thread().
	 * 읽는 자: cb_fn 호출 시 send_msg 대상. */

	spdk_nvmf_subsystem_state_change_done		cb_fn;
	/* [한국어] 상태 변경 완료 콜백.
	 * 설정자: 호출자 등록.
	 * 읽는 자: 모든 PG 작업 완료 후. */

	void						*cb_arg;
	/* [한국어] cb_fn에 전달될 컨텍스트. */

	TAILQ_ENTRY(nvmf_subsystem_state_change_ctx)	link;
	/* [한국어] subsystem->state_changes 큐의 노드 - 직렬 처리. */
};

/*
 * [한국어] NVMe-oF Subsystem - NQN으로 식별되는 namespace 컬렉션.
 * 호스트는 Connect 시 subnqn을 지정해 이 subsystem에 접속하고, ctrlr이 만들어진다.
 * subsystem은 단일 spdk_thread(보통 main 또는 첫 PG thread)에 바인딩되어 상태 변경/destroy를 직렬 처리.
 */
struct spdk_nvmf_subsystem {
	struct spdk_thread				*thread;
	/* [한국어] subsystem 본체가 바인딩된 spdk_thread - state 변경, ns add/remove 등 모두 여기서.
	 * 설정자: subsystem_create 시.
	 * 읽는 자: cross-thread 호출 시 send_msg 대상.
	 * 동기화: 모든 mutating 작업은 이 thread에서. */

	uint32_t					id;
	/* [한국어] target 안에서의 subsystem 고유 id (subsystem_ids bit_array 비트 인덱스).
	 * 설정자: subsystem_create 시 free 비트 할당.
	 * 읽는 자: 디버그/RPC, 해제 시 비트 clear. */

	enum spdk_nvmf_subsystem_state			state;
	/* [한국어] 8단계 subsystem 상태 - INACTIVE/ACTIVATING/ACTIVE/PAUSING/PAUSED/RESUMING/DEACTIVATING.
	 * 설정자: state_change_ctx 처리 흐름.
	 * 읽는 자: IO 디스패치, RPC.
	 * 동기화: thread 단일 사용 + atomic 사용 가능 시 보조. */

	enum spdk_nvmf_subtype				subtype;
	/* [한국어] subsystem 종류 (NVME 또는 DISCOVERY).
	 * 설정자: subsystem_create.
	 * 읽는 자: Discovery 처리 분기. */

	uint16_t					next_cntlid;
	/* [한국어] 다음 동적 cntlid 할당 시작점 (round-robin 스타일).
	 * 설정자: gen_cntlid 호출 시 advance.
	 * 읽는 자: gen_cntlid. */

	struct {
		uint8_t					allow_any_listener : 1;
		/* [한국어] true면 어떤 listener를 통해서도 connect 허용 (target opts allow_any_listener).
		 * 설정자: subsystem_create.
		 * 읽는 자: connect 검증. */

		uint8_t					ana_reporting : 1;
		/* [한국어] ANA(Asymmetric Namespace Access) 보고 활성 여부.
		 * 설정자: subsystem_create opts.
		 * 읽는 자: ANA Log Page 응답, Identify Controller cdata.cmic 등. */

		uint8_t					reserved : 6;
		/* [한국어] 예약 비트 - 패딩. */
	} flags;

	/* Protected against concurrent access by ->mutex */
	bool						allow_any_host;
	/* [한국어] true면 hosts 리스트 무시하고 모든 host 허용.
	 * 설정자: subsystem_set_allow_any_host RPC.
	 * 읽는 자: connect 시 ACL 검사.
	 * 동기화: subsystem->mutex. */

	bool						destroying;
	/* [한국어] subsystem destroy 진행 중 - 새 작업 거부.
	 * 설정자: spdk_nvmf_subsystem_destroy 진입.
	 * 읽는 자: 모든 mutating 호출 가드. */

	bool						async_destroy;
	/* [한국어] async_destroy_cb 사용한 비동기 destroy 모드.
	 * 설정자: subsystem_destroy 진입.
	 * 읽는 자: destroy 완료 시 콜백 호출 분기. */

	/* FDP related fields */
	bool						fdp_supported;
	/* [한국어] FDP(Flexible Data Placement, NVMe TP4146) 지원 여부.
	 * 설정자: subsystem_create 시 transport/bdev capability 체크.
	 * 읽는 자: Identify Controller, FDP 관련 명령 처리. */

	/* Zoned storage related fields */
	uint64_t					max_zone_append_size_kib;
	/* [한국어] Zoned NS의 최대 Zone Append 크기 (KiB).
	 * 설정자: ZNS bdev capability에서 산출.
	 * 읽는 자: Identify NS (ZNS), IO 검증. */

	struct spdk_nvmf_tgt				*tgt;
	/* [한국어] 소속 target back-pointer. */

	RB_ENTRY(spdk_nvmf_subsystem)			link;
	/* [한국어] tgt->subsystems RB-tree의 노드 (NQN 정렬).
	 * 동기화: tgt->mutex. */

	/* Array of pointers to namespaces of size max_nsid indexed by nsid - 1 */
	struct spdk_nvmf_ns				**ns;
	/* [한국어] ns 포인터 배열 - nsid-1로 인덱싱.
	 * 설정자: ns add/remove 시 슬롯에 set/clear.
	 * 읽는 자: _nvmf_subsystem_get_ns() 등 핫패스에서 인덱싱.
	 * 값 범위: 크기 = max_nsid; NULL 슬롯은 미할당. */

	uint32_t					max_nsid;
	/* [한국어] 최대 nsid (ns 배열 크기).
	 * 설정자: subsystem_create opts->max_nsid 또는 동적 확장.
	 * 읽는 자: 인덱스 검증. */

	uint16_t					min_cntlid;
	/* [한국어] 동적 cntlid 할당 하한 (subsystem 단위).
	 * 설정자: subsystem_set_cntlid_range.
	 * 읽는 자: gen_cntlid. */

	uint16_t					max_cntlid;
	/* [한국어] 동적 cntlid 할당 상한.
	 * 설정자/읽는 자: 위와 대칭. */

	uint64_t					max_discard_size_kib;
	/* [한국어] 단일 DSM Deallocate 명령 최대 크기 (KiB).
	 * 설정자: subsystem opts.
	 * 읽는 자: Identify, 명령 분할. */

	uint64_t					max_write_zeroes_size_kib;
	/* [한국어] 단일 Write Zeroes 명령 최대 크기 (KiB).
	 * 설정자/읽는 자: 위와 대칭. */

	TAILQ_HEAD(, spdk_nvmf_ctrlr)			ctrlrs;
	/* [한국어] 이 subsystem에 연결된 컨트롤러 리스트.
	 * 설정자: ctrlr_create/destruct.
	 * 읽는 자: AER 통지 루프, 통계.
	 * 동기화: subsystem thread 단독 또는 mutex. */

	/* This mutex is used to protect fields that aren't touched on the I/O path (e.g. it's
	 * needed for handling things like the CONNECT command) instead of requiring the subsystem
	 * to be paused.  It makes it possible to modify those fields (e.g. add/remove hosts)
	 * without affecting outstanding I/O requests.
	 */
	pthread_mutex_t					mutex;
	/* [한국어] non-IO 필드 보호 mutex (hosts/listeners/allow_any_host 등).
	 * IO 경로는 PG-locality + state 머신으로 quiesce하므로 mutex 없이 동작.
	 * 설정자/읽는 자: subsystem 변경 RPC, Connect 처리.
	 * 동기화: 모든 non-IO 필드 보호. */

	/* Protected against concurrent access by ->mutex */
	TAILQ_HEAD(, spdk_nvmf_host)			hosts;
	/* [한국어] 허용 host NQN 리스트 (allow_any_host=false 시 ACL).
	 * 동기화: ->mutex. */

	TAILQ_HEAD(, spdk_nvmf_subsystem_listener)	listeners;
	/* [한국어] subsystem이 광고되는 listener 리스트.
	 * 동기화: ->mutex. */

	struct spdk_bit_array				*used_listener_ids;
	/* [한국어] listener id 사용 비트맵 (NVMF_MAX_LISTENERS_PER_SUBSYSTEM 크기).
	 * 설정자: listener add/remove.
	 * 읽는 자: 새 id 할당. */

	TAILQ_ENTRY(spdk_nvmf_subsystem)		entries;
	/* [한국어] (보조 리스트용 - 일부 코드 경로의 entries 리스트). */

	nvmf_subsystem_destroy_cb			async_destroy_cb;
	/* [한국어] async destroy 완료 콜백.
	 * 설정자: subsystem_destroy 호출자.
	 * 읽는 자: destroy 완료 시 호출. */

	void						*async_destroy_cb_arg;
	/* [한국어] async_destroy_cb에 전달될 컨텍스트. */

	char						sn[SPDK_NVME_CTRLR_SN_LEN + 1];
	/* [한국어] Identify Controller Serial Number 문자열.
	 * 설정자: subsystem_create opts->serial_number.
	 * 읽는 자: Identify 응답.
	 * 값 범위: 20자 + NUL. */

	char						mn[SPDK_NVME_CTRLR_MN_LEN + 1];
	/* [한국어] Identify Controller Model Number 문자열.
	 * 설정자/읽는 자: 위와 대칭. 40자 + NUL. */

	char						subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	/* [한국어] subsystem NQN - 호스트 Connect 매칭 키.
	 * 설정자: subsystem_create.
	 * 읽는 자: 모든 NQN 비교, RB-tree 정렬 키.
	 * 값 범위: NUL terminated NQN. */

	/* Array of namespace count per ANA group of size max_nsid indexed anagrpid - 1
	 * It will be enough for ANA group to use the same size as namespaces.
	 */
	uint32_t					*ana_group;
	/* [한국어] ANA 그룹 ID별 ns 개수 카운터 - ANA Log Page 빌드 시 사용.
	 * 설정자: ns add/remove 시 anagrpid 카운터 증감.
	 * 읽는 자: ANA Log Page 응답. */

	/* Queue of a state change requests */
	TAILQ_HEAD(, nvmf_subsystem_state_change_ctx)	state_changes;
	/* [한국어] 진행 중 + 대기 state change 컨텍스트 큐 - 직렬 처리.
	 * 설정자: state change 진입 시 enqueue.
	 * 읽는 자: 현재 진행 중 작업 완료 시 다음 dequeue. */

	/* In-band authentication sequence number, protected by ->mutex */
	uint32_t					auth_seqnum;
	/* [한국어] DH-HMAC-CHAP in-band auth 시퀀스 번호 (replay 방지용).
	 * 설정자: auth.c.
	 * 읽는 자: AUTH 챌린지 검증.
	 * 동기화: ->mutex. */

	bool						passthrough;
	/* [한국어] passthrough 모드 - host 명령을 백엔드 NVMe 디바이스에 직접 전달.
	 * 설정자: subsystem_create opts.
	 * 읽는 자: 명령 디스패치 분기 (custom NVMe admin command pass). */

	bool						nssr_enabled;
	/* [한국어] NSSR(NVM Subsystem Reset) 처리 활성 여부.
	 * 설정자: subsystem opts.
	 * 읽는 자: NSSR 명령 처리 진입 검증. */
};

extern spdk_nvmf_custom_discovery_filter g_custom_discovery_filter;
/* [한국어] 사용자 정의 Discovery 필터 콜백 - Discovery 응답에서 listener를 추가로 필터링.
 * 설정자: spdk_nvmf_set_custom_discovery_filter.
 * 읽는 자: Discovery Log 빌드 시. */

/*
 * [한국어]
 * subsystem_cmp - subsystem RB-tree의 비교 함수 (NQN 사전순)
 *
 * @subsystem1: 비교 대상 1
 * @subsystem2: 비교 대상 2
 * @return: <0 / 0 / >0 (strncmp 결과)
 *
 * RB-tree에서 subsystem을 NQN으로 정렬하기 위한 비교 함수.
 * subnqn은 NUL terminated이지만 strncmp(sizeof(subnqn))로 안전하게 비교한다.
 * 실행 컨텍스트: tree insert/lookup 시 호출 - 임의 thread (mutex로 보호된 영역).
 */
static int
subsystem_cmp(struct spdk_nvmf_subsystem *subsystem1, struct spdk_nvmf_subsystem *subsystem2)
{
	return strncmp(subsystem1->subnqn, subsystem2->subnqn, sizeof(subsystem1->subnqn)); /* [한국어] subnqn 사전 비교 - 정확한 매칭과 정렬 동시 지원 */
}

RB_GENERATE_STATIC(subsystem_tree, spdk_nvmf_subsystem, link, subsystem_cmp);
/* [한국어] subsystem RB-tree의 핵심 함수들(subsystem_tree_INSERT/REMOVE/FIND 등)을 static으로 생성.
 * 본 헤더가 포함되는 모든 .c가 자기 TU에 RB-tree 함수 사본을 만들지만, static이므로 충돌 없음.
 * link 필드(RB_ENTRY)와 subsystem_cmp를 사용. */

/*
 * [한국어]
 * nvmf_poll_group_update_subsystem - poll group의 subsystem state를 본체와 동기화 (동기 호출)
 *
 * @group: 갱신할 poll group
 * @subsystem: 본체 subsystem
 * @return: 0 성공, 음수 errno
 *
 * 주로 ns add/remove 후 PG의 ns_info 배열을 본체와 sync 시킬 때 사용. 비동기 콜백 없이 즉시 반환.
 * 호출 체인: subsystem 변경 RPC -> [본 함수] -> ns_info 갱신
 */
int nvmf_poll_group_update_subsystem(struct spdk_nvmf_poll_group *group,
				     struct spdk_nvmf_subsystem *subsystem);

/*
 * [한국어]
 * nvmf_poll_group_add_subsystem - PG에 subsystem을 추가 (비동기)
 *
 * @group: 대상 PG
 * @subsystem: 추가할 subsystem
 * @cb_fn: 완료 콜백 (status=0 성공)
 * @cb_arg: 콜백 인자
 *
 * subsystem 시작 흐름의 일부 - 모든 PG에 대해 호출되어 PG-side 자원(ns_info, io_channel)을 준비.
 * 실행 컨텍스트: subsystem->thread에서 시작, PG thread로 send_msg 후 처리.
 */
int nvmf_poll_group_add_subsystem(struct spdk_nvmf_poll_group *group,
				  struct spdk_nvmf_subsystem *subsystem,
				  spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg);

/*
 * [한국어]
 * nvmf_poll_group_remove_subsystem - PG에서 subsystem 제거 (비동기)
 *
 * @group: 대상 PG
 * @subsystem: 제거할 subsystem
 * @cb_fn/@cb_arg: 완료 콜백
 *
 * subsystem deactivate 흐름의 일부 - PG-side 자원 해제.
 */
void nvmf_poll_group_remove_subsystem(struct spdk_nvmf_poll_group *group,
				      struct spdk_nvmf_subsystem *subsystem, spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg);

/*
 * [한국어]
 * nvmf_poll_group_pause_subsystem - PG에서 subsystem(또는 특정 ns) IO 일시 정지
 *
 * @group: 대상 PG
 * @subsystem: 일시 정지할 subsystem
 * @nsid: 0이면 전체, 그 외면 해당 ns만 부분 PAUSE
 * @cb_fn/@cb_arg: 완료 콜백
 *
 * outstanding IO가 모두 끝날 때까지 새 IO submit 차단. PAUSED 상태에서 ns 수정 등이 가능.
 */
void nvmf_poll_group_pause_subsystem(struct spdk_nvmf_poll_group *group,
				     struct spdk_nvmf_subsystem *subsystem,
				     uint32_t nsid,
				     spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg);

/*
 * [한국어]
 * nvmf_poll_group_resume_subsystem - PG에서 subsystem 일시 정지 해제
 *
 * 위와 대응. queued 큐의 요청을 dequeue 후 재실행.
 */
void nvmf_poll_group_resume_subsystem(struct spdk_nvmf_poll_group *group,
				      struct spdk_nvmf_subsystem *subsystem, spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg);

/*
 * [한국어]
 * nvmf_get_discovery_log_page_async - Discovery Log Page 비동기 빌드/응답
 *
 * @req: 호스트의 Get Log Page (Discovery) 요청
 * @offset: 호스트가 요청한 페이지 내 offset
 * @length: 요청 길이
 * @cmd_source_trid: 요청이 들어온 listener의 trid (ana_state 컨텍스트)
 * @rae: Retain Asynchronous Event - true면 AER 마스크 유지
 *
 * 등록된 모든 subsystem listener를 순회해 Discovery Log Page Header + Entry들을 합성.
 * 큰 페이지의 경우 비동기 처리 후 nvmf_request_complete 호출.
 */
void nvmf_get_discovery_log_page_async(struct spdk_nvmf_request *req,
				       uint64_t offset, uint32_t length,
				       struct spdk_nvme_transport_id *cmd_source_trid,
				       bool rae);

/*
 * [한국어]
 * nvmf_ctrlr_unmask_aen - 호스트가 Get Log Page로 이벤트를 수확한 후 마스크 해제
 *
 * @ctrlr: 대상 ctrlr
 * @mask: 해제할 비트 (ENUM 위치)
 *
 * AER 발화 시 자동으로 mask가 set되며, 호스트가 해당 log page를 가져가면 unmask해 다음 통지 가능.
 */
void nvmf_ctrlr_unmask_aen(struct spdk_nvmf_ctrlr *ctrlr,
			   enum spdk_nvme_async_event_mask_bit mask);

/*
 * [한국어]
 * nvmf_ctrlr_destruct - 컨트롤러 비동기 해제 (모든 qpair disconnect + 자원 free)
 *
 * @ctrlr: 해제 대상
 *
 * keep-alive timeout / association timeout / 호스트 explicit disconnect 등에서 호출.
 */
void nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * nvmf_ctrlr_process_admin_cmd - 호스트의 Admin 명령 디스패치 진입점
 *
 * @req: 명령 요청 (cmd opcode = NVMe Admin)
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE/ASYNCHRONOUS
 *
 * Identify, Get/Set Features, AER, Get Log Page, Format, Abort, Keep Alive 등을 분기 처리.
 */
int nvmf_ctrlr_process_admin_cmd(struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_ctrlr_process_io_cmd - 호스트의 IO 명령 디스패치 진입점
 *
 * @req: IO 명령 (Read/Write/Flush/DSM/Compare/Copy/ZNS 등)
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_*
 *
 * ns visibility 검사 -> nvmf_bdev_ctrlr_*_cmd 위임.
 */
int nvmf_ctrlr_process_io_cmd(struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_ctrlr_dsm_supported - 이 ctrlr이 DSM(Dataset Management) 지원하는지
 *
 * Identify Controller cdata.oncs 결정에 사용. 모든 ns의 bdev가 unmap 지원해야 true.
 */
bool nvmf_ctrlr_dsm_supported(struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * nvmf_ctrlr_write_zeroes_supported - Write Zeroes 지원 여부
 */
bool nvmf_ctrlr_write_zeroes_supported(struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * nvmf_ctrlr_copy_supported - Copy 명령 지원 여부 (NVMe TP4065)
 */
bool nvmf_ctrlr_copy_supported(struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * nvmf_ctrlr_ns_changed - 특정 ns 변경 사실을 ctrlr에 기록 (AER 트리거 후보)
 *
 * @ctrlr: 통지할 ctrlr
 * @nsid: 변경된 nsid
 *
 * changed_ns_list에 추가하고 ns_attr_change AER을 발화 (마스킹되지 않은 경우).
 */
void nvmf_ctrlr_ns_changed(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid);

/*
 * [한국어]
 * nvmf_ctrlr_use_zcopy - 이 요청이 zero-copy 경로를 사용할 수 있는지 검사
 */
bool nvmf_ctrlr_use_zcopy(struct spdk_nvmf_request *req);

/*
 * [한국어] 아래 nvmf_bdev_ctrlr_* 함수들은 NVMe 명령을 bdev API로 변환하는 어댑터들이다.
 * 모두 lib/nvmf/ctrlr_bdev.c에 구현되어 있으며 PG thread에서 실행.
 */

/*
 * [한국어]
 * nvmf_bdev_ctrlr_identify_ns - Identify Namespace 응답 데이터 빌드 (bdev에서 추출)
 *
 * @ns: 대상 ns
 * @nsdata: 출력 버퍼 (NVMe Spec Identify NS 4096B layout)
 * @dif_insert_or_strip: PI 설정에 따라 lbaf 조정
 * @transport_max_io_size: 트랜스포트 최대 IO 크기 (mdts 산정)
 */
void nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata,
				 bool dif_insert_or_strip, uint32_t transport_max_io_size);

/*
 * [한국어]
 * nvmf_bdev_ctrlr_identify_iocs_nvm - Identify NS의 IOCS-specific(NVM) 응답 빌드
 */
void nvmf_bdev_ctrlr_identify_iocs_nvm(struct spdk_nvmf_ns *ns,
				       struct spdk_nvme_nvm_ns_data *nsdata_nvm);

/*
 * [한국어]
 * nvmf_bdev_ctrlr_read_cmd - NVMe Read 명령을 spdk_bdev_readv로 변환
 *
 * @bdev/@desc/@ch: bdev API 인자
 * @req: NVMe Read 요청 (cmd.cdw10/11=SLBA, cdw12=NLB)
 * @return: COMPLETE(즉시 처리) 또는 ASYNCHRONOUS(완료 콜백 등록 완료)
 */
int nvmf_bdev_ctrlr_read_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			     struct spdk_io_channel *ch, struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_bdev_ctrlr_write_cmd - NVMe Write 명령을 spdk_bdev_writev로 변환
 */
int nvmf_bdev_ctrlr_write_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			      struct spdk_io_channel *ch, struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_bdev_ctrlr_compare_cmd - NVMe Compare 명령
 */
int nvmf_bdev_ctrlr_compare_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				struct spdk_io_channel *ch, struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_bdev_ctrlr_compare_and_write_cmd - NVMe Compare + Write fused 명령
 *
 * @cmp_req: Compare 요청
 * @write_req: Write 요청 (atomic)
 *
 * Fused 명령은 두 개의 요청을 atomic하게 처리해야 하므로 별도 진입점.
 */
int nvmf_bdev_ctrlr_compare_and_write_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch, struct spdk_nvmf_request *cmp_req, struct spdk_nvmf_request *write_req);

/*
 * [한국어]
 * nvmf_bdev_ctrlr_write_zeroes_cmd - NVMe Write Zeroes 명령
 */
int nvmf_bdev_ctrlr_write_zeroes_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				     struct spdk_io_channel *ch, struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_bdev_ctrlr_flush_cmd - NVMe Flush 명령
 */
int nvmf_bdev_ctrlr_flush_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			      struct spdk_io_channel *ch, struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_bdev_ctrlr_dsm_cmd - NVMe Dataset Management (Deallocate=trim) 명령
 */
int nvmf_bdev_ctrlr_dsm_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch, struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_bdev_ctrlr_copy_cmd - NVMe Copy 명령 (TP4065)
 */
int nvmf_bdev_ctrlr_copy_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			     struct spdk_io_channel *ch, struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_bdev_ctrlr_nvme_passthru_io - 백엔드 NVMe 디바이스에 IO 명령 그대로 전달
 *
 * subsystem.passthrough=true에서만 사용. host nsid -> passthru_nsid 치환 후 spdk_bdev_nvme_io_passthru.
 */
int nvmf_bdev_ctrlr_nvme_passthru_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				     struct spdk_io_channel *ch, struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_bdev_ctrlr_get_dif_ctx - 명령에 대한 DIF 컨텍스트 빌드
 *
 * @desc: bdev desc
 * @cmd: NVMe 명령
 * @dif_ctx: 출력 - PI 위치/타입/CRC seed 등
 * @return: true 빌드 성공, false PI 비활성/에러
 */
bool nvmf_bdev_ctrlr_get_dif_ctx(struct spdk_bdev_desc *desc, struct spdk_nvme_cmd *cmd,
				 struct spdk_dif_ctx *dif_ctx);

/*
 * [한국어]
 * nvmf_bdev_zcopy_enabled - bdev이 zero-copy 지원하는지
 */
bool nvmf_bdev_zcopy_enabled(struct spdk_bdev *bdev);

/*
 * [한국어]
 * nvmf_subsystem_add_ctrlr - subsystem->ctrlrs 리스트에 ctrlr 등록
 */
int nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			     struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * nvmf_subsystem_remove_ctrlr - subsystem->ctrlrs에서 ctrlr 제거 + cntlid 반환
 */
void nvmf_subsystem_remove_ctrlr(struct spdk_nvmf_subsystem *subsystem,
				 struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * nvmf_subsystem_remove_all_listeners - subsystem의 모든 listener 제거
 *
 * @stop: true면 트랜스포트 레벨 listen도 stop
 */
void nvmf_subsystem_remove_all_listeners(struct spdk_nvmf_subsystem *subsystem,
		bool stop);

/*
 * [한국어]
 * nvmf_subsystem_get_ctrlr - subsystem 안에서 cntlid로 컨트롤러 검색
 */
struct spdk_nvmf_ctrlr *nvmf_subsystem_get_ctrlr(struct spdk_nvmf_subsystem *subsystem,
		uint16_t cntlid);

/*
 * [한국어]
 * nvmf_subsystem_host_auth_required - 특정 호스트가 인증 요구되는지
 *
 * 호스트 dhchap_key가 등록되어 있으면 true.
 */
bool nvmf_subsystem_host_auth_required(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn);

/*
 * [한국어]
 * nvmf_subsystem_get_dhchap_key - 호스트 NQN에 대한 DH-HMAC-CHAP 키 조회
 *
 * @type: HOST 또는 CTRLR 키 종류
 * @return: 새 reference (호출자가 spdk_keyring_put 책임)
 */
struct spdk_key *nvmf_subsystem_get_dhchap_key(struct spdk_nvmf_subsystem *subsys, const char *nqn,
		enum nvmf_auth_key_type type);

/*
 * [한국어]
 * nvmf_subsystem_find_listener - subsystem에서 trid 매칭 listener 검색
 */
struct spdk_nvmf_subsystem_listener *nvmf_subsystem_find_listener(
	struct spdk_nvmf_subsystem *subsystem,
	const struct spdk_nvme_transport_id *trid);

/*
 * [한국어]
 * nvmf_subsystem_listener_is_active - listener가 활성(listen 완료)인지
 *
 * trtype-level listen이 cb_fn 호출까지 끝났는지로 판단.
 */
bool nvmf_subsystem_listener_is_active(const struct spdk_nvmf_subsystem_listener *listener);

/*
 * [한국어]
 * nvmf_subsystem_zone_append_supported - subsystem이 ZNS Zone Append 지원하는지
 */
bool nvmf_subsystem_zone_append_supported(struct spdk_nvmf_subsystem *subsystem);

/*
 * [한국어]
 * nvmf_subsystem_poll_group_update_ns_reservation - ns 본체 reservation을 PG ns_info로 전파
 *
 * Reservation 명령 처리 후 모든 PG의 ns_info 캐시를 갱신해 fast-path 일관성 확보.
 */
void nvmf_subsystem_poll_group_update_ns_reservation(const struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_subsystem_pg_ns_info *pg_ns);

/*
 * [한국어]
 * nvmf_transport_find_listener - transport 안에서 trid 매칭 listener 검색
 */
struct spdk_nvmf_listener *nvmf_transport_find_listener(
	struct spdk_nvmf_transport *transport,
	const struct spdk_nvme_transport_id *trid);

/*
 * [한국어]
 * nvmf_transport_dump_opts - 트랜스포트 옵션을 JSON으로 덤프 (RPC 응답)
 *
 * @named: true면 옵션 이름과 함께, false면 값만.
 */
void nvmf_transport_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w,
			      bool named);

/*
 * [한국어]
 * nvmf_transport_listen_dump_trid - listener trid를 JSON으로 덤프
 */
void nvmf_transport_listen_dump_trid(const struct spdk_nvme_transport_id *trid,
				     struct spdk_json_write_ctx *w);

/*
 * [한국어]
 * nvmf_ctrlr_async_event_ns_notice - Namespace Attribute Change AER 발화
 */
int nvmf_ctrlr_async_event_ns_notice(struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * nvmf_ctrlr_async_event_ana_change_notice - ANA Change AER 발화
 */
int nvmf_ctrlr_async_event_ana_change_notice(struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * nvmf_ctrlr_async_event_discovery_log_change_notice - Discovery Log Change AER 발화 (ctx 받는 형태)
 *
 * @ctx: 일반적으로 ctrlr 포인터 (콜백 인터페이스)
 */
void nvmf_ctrlr_async_event_discovery_log_change_notice(void *ctx);

/*
 * [한국어]
 * nvmf_ctrlr_async_event_reservation_notification - Reservation Log Available AER 발화
 */
void nvmf_ctrlr_async_event_reservation_notification(struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * nvmf_ns_reservation_request - ns의 reservation 명령 큐 head 처리 진입점
 *
 * @ctx: 명령 컨텍스트 (보통 spdk_nvmf_request)
 *
 * STAILQ로 직렬화된 reservation 명령 처리 - ns->reservations 큐 가공.
 */
void nvmf_ns_reservation_request(void *ctx);

/*
 * [한국어]
 * nvmf_ctrlr_reservation_notice_log - Reservation 변경을 로그 큐에 enqueue + AER 검토
 *
 * @ctrlr/@ns: 알릴 컨트롤러/ns
 * @type: NVMe Reservation Notification Log entry type (Registration Preempted 등)
 */
void nvmf_ctrlr_reservation_notice_log(struct spdk_nvmf_ctrlr *ctrlr,
				       struct spdk_nvmf_ns *ns,
				       enum spdk_nvme_reservation_notification_log_page_type type);

/*
 * [한국어]
 * nvmf_ns_is_ptpl_capable - ns가 PTPL(Persist Through Power Loss) 가능한지 (ptpl_file 설정 여부)
 */
bool nvmf_ns_is_ptpl_capable(const struct spdk_nvmf_ns *ns);

/*
 * [한국어]
 * nvmf_ns_get_rescap - ns의 Reservation Capabilities 비트필드 (Identify NS rescap)
 */
struct spdk_nvme_rescap nvmf_ns_get_rescap(struct spdk_nvmf_ns *ns);

/*
 * [한국어]
 * nvmf_ns_registrants_get_count - ns->registrants 리스트 길이
 */
size_t nvmf_ns_registrants_get_count(const struct spdk_nvmf_ns *ns);

/*
 * [한국어]
 * nvmf_ns_find_host - ns->hosts 화이트리스트에서 hostnqn 검색 (인라인)
 *
 * @ns: 대상 ns
 * @hostnqn: 찾을 호스트 NQN
 * @return: 매칭된 host 포인터 또는 NULL
 *
 * ns visibility 평가에서 ctrlr_create 시 사용.
 * 실행 컨텍스트: subsystem thread 또는 PG thread (ns->hosts 보호 컨텍스트 안에서).
 */
static inline struct spdk_nvmf_host *
nvmf_ns_find_host(struct spdk_nvmf_ns *ns, const char *hostnqn)
{
	struct spdk_nvmf_host *host = NULL;         /* [한국어] 검색 결과 - 못 찾으면 NULL 유지 */

	TAILQ_FOREACH(host, &ns->hosts, link) {     /* [한국어] ns의 host 화이트리스트 순회 */
		if (strcmp(hostnqn, host->nqn) == 0) { /* [한국어] NQN 정확 일치 비교 */
			return host;                /* [한국어] 첫 매칭 반환 - NQN unique 보장 */
		}
	}

	return NULL;                                /* [한국어] 화이트리스트에 없음 - visibility 거부 신호 */
}

/*
 * Abort zero-copy requests that already got the buffer (received zcopy_start cb), but haven't
 * started zcopy_end.  These requests are kept on the outstanding queue, but are not waiting for a
 * completion from the bdev layer, so, when a qpair is being disconnected, we need to kick them to
 * force their completion.
 *
 * [한국어]
 * nvmf_qpair_abort_pending_zcopy_reqs - 버퍼만 받고 zcopy_end 미진행인 zcopy 요청들 강제 abort
 *
 * @qpair: 대상 qpair
 *
 * Zero-copy 흐름: zcopy_start로 bdev에서 버퍼를 빌림 -> 트랜스포트가 호스트에 데이터 송수신
 * -> zcopy_end로 commit/free. 중간 단계에서 qpair가 끊어지면 bdev은 outstanding으로 인식하지 않으나
 * 우리가 가진 요청들은 outstanding 큐에 박혀 있으므로 명시적으로 abort해 정리.
 */
void nvmf_qpair_abort_pending_zcopy_reqs(struct spdk_nvmf_qpair *qpair);

/*
 * Free aer simply frees the rdma resources for the aer without informing the host.
 * This function should be called when deleting a qpair when one wants to make sure
 * the qpair is completely empty before freeing the request. The reason we free the
 * AER without sending a completion is to prevent the host from sending another AER.
 *
 * [한국어]
 * nvmf_qpair_free_aer - qpair에 보관된 AER 슬롯들을 호스트 통지 없이 해제
 *
 * @qpair: 대상 qpair
 *
 * qpair 삭제 시 단순 free - 호스트에 응답 송신하면 호스트가 다음 AER을 또 보내므로 의도적으로 응답 안 함.
 * Zero-copy 같은 outstanding 처리를 깨끗이 비우기 위한 cleanup 단계.
 */
void nvmf_qpair_free_aer(struct spdk_nvmf_qpair *qpair);

/*
 * [한국어]
 * nvmf_ctrlr_abort_request - 호스트의 NVMe Abort 명령 처리
 *
 * @req: Abort admin 명령 요청 (cdw10에 cid, sqid 지정)
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_*
 */
int nvmf_ctrlr_abort_request(struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_ctrlr_set_fatal_status - ctrlr을 fatal 상태로 표시 (CSTS.CFS=1)
 *
 * 복구 불가 에러 발생 시 호출 - 호스트는 CSTS 폴링으로 인지 후 reset 시도.
 */
void nvmf_ctrlr_set_fatal_status(struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * nvmf_ctrlr_ns_is_visible - 인라인 visibility 검사 (hot path)
 *
 * @ctrlr: 검사할 ctrlr
 * @nsid: 검사할 nsid (1-based)
 * @return: true면 visible
 *
 * 모든 IO 명령마다 호출되므로 비트 배열 1회 조회로 끝나도록 인라인.
 */
static inline bool
nvmf_ctrlr_ns_is_visible(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
	assert(nsid > 0 && nsid <= ctrlr->subsys->max_nsid); /* [한국어] nsid 유효 범위 검사 - 0과 max_nsid 초과는 호출자 버그 */
	return spdk_bit_array_get(ctrlr->visible_ns, nsid - 1); /* [한국어] 비트 배열에서 (nsid-1) 비트 조회 - 1-based -> 0-based 변환 */
}

/*
 * [한국어]
 * nvmf_ctrlr_ns_set_visible - 인라인 visibility 설정/해제
 *
 * @visible: true면 set, false면 clear
 *
 * ns add/remove + host visibility 변경 시 사용.
 */
static inline void
nvmf_ctrlr_ns_set_visible(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid, bool visible)
{
	assert(nsid > 0 && nsid <= ctrlr->subsys->max_nsid); /* [한국어] nsid 범위 검사 */
	if (visible) {                              /* [한국어] visible=true → 비트 set */
		spdk_bit_array_set(ctrlr->visible_ns, nsid - 1);
	} else {                                    /* [한국어] visible=false → 비트 clear */
		spdk_bit_array_clear(ctrlr->visible_ns, nsid - 1);
	}
}

/*
 * [한국어]
 * _nvmf_subsystem_get_ns - subsystem의 ns 배열에서 nsid 슬롯 반환 (visibility 미검사)
 *
 * @subsystem: 대상 subsystem
 * @nsid: 1-based nsid
 * @return: ns 포인터 또는 NULL (범위 밖 / 슬롯 비어있음)
 *
 * 핵심 hot path: spdk_unlikely 힌트로 분기 예측 최적화.
 * NOTE: 0-1 == UINT32_MAX 트릭으로 nsid==0도 동일 분기에서 거부.
 */
static inline struct spdk_nvmf_ns *
_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
	if (spdk_unlikely(nsid - 1 >= subsystem->max_nsid)) { /* [한국어] 범위 밖 또는 nsid=0 (underflow trick) - 한 번의 비교로 둘 다 체크 */
		return NULL;
	}

	return subsystem->ns[nsid - 1];             /* [한국어] 0-based 인덱스로 슬롯 반환 (NULL 가능 - 호출자가 처리) */
}

/*
 * [한국어]
 * nvmf_ctrlr_get_ns - ctrlr 시각의 ns 조회 (visibility 검사 포함)
 *
 * @ctrlr: 조회할 ctrlr
 * @nsid: 1-based nsid
 * @return: visible한 ns 또는 NULL
 *
 * IO 명령 디스패치의 첫 단계. visible_ns 비트맵으로 ACL 적용.
 */
static inline struct spdk_nvmf_ns *
nvmf_ctrlr_get_ns(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys; /* [한국어] back-pointer로 subsystem 획득 */
	struct spdk_nvmf_ns *ns = _nvmf_subsystem_get_ns(subsystem, nsid); /* [한국어] 슬롯에서 ns 조회 (NULL 가능) */

	return ns && nvmf_ctrlr_ns_is_visible(ctrlr, nsid) ? ns : NULL; /* [한국어] 존재 + visible일 때만 반환 - ACL gate */
}

/*
 * [한국어]
 * nvmf_qpair_is_admin_queue - qpair가 admin queue인지 (qid==0)
 *
 * NVMe Spec: qid 0이 admin, 1+가 IO queue.
 */
static inline bool
nvmf_qpair_is_admin_queue(struct spdk_nvmf_qpair *qpair)
{
	return qpair->qid == 0;                     /* [한국어] qid 0은 admin queue로 정의됨 (NVMe Base Spec) */
}

/*
 * [한국어]
 * nvmf_qpair_set_state - qpair 상태 머신 전이 (CONNECTING/AUTHENTICATING/ENABLED/DEACTIVATING/ERROR 등)
 */
void nvmf_qpair_set_state(struct spdk_nvmf_qpair *qpair, enum spdk_nvmf_qpair_state state);

/*
 * [한국어]
 * nvmf_qpair_auth_init - qpair용 DH-HMAC-CHAP 인증 컨텍스트 초기화
 *
 * Connect 직후 호출되어 챌린지/응답 상태 준비. 미빌드 시 stubs.c가 -ENOTSUP 반환.
 */
int nvmf_qpair_auth_init(struct spdk_nvmf_qpair *qpair);

/*
 * [한국어]
 * nvmf_qpair_auth_destroy - 인증 컨텍스트 해제
 */
void nvmf_qpair_auth_destroy(struct spdk_nvmf_qpair *qpair);

/*
 * [한국어]
 * nvmf_qpair_auth_dump - qpair 인증 상태를 JSON으로 덤프 (RPC nvmf_get_qpair용)
 */
void nvmf_qpair_auth_dump(struct spdk_nvmf_qpair *qpair, struct spdk_json_write_ctx *w);

/*
 * [한국어]
 * nvmf_auth_request_exec - AUTH Send/Receive 캡슐 처리 진입점
 */
int nvmf_auth_request_exec(struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_auth_is_supported - 빌드 타임 인증 가용성 (EVP MAC 빌드 여부)
 */
bool nvmf_auth_is_supported(void);

/*
 * [한국어]
 * nvmf_request_is_fabric_connect - 요청이 NVMe-oF Connect fabric 명령인지 (인라인)
 *
 * 명령 디스패치에서 Connect를 다른 명령보다 먼저 처리해야 하는 곳에 사용.
 */
static inline bool
nvmf_request_is_fabric_connect(struct spdk_nvmf_request *req)
{
	return req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC && /* [한국어] opcode가 Fabric (0x7F)인지 - NVMe-oF 캡슐 표시 */
	       req->cmd->nvmf_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT; /* [한국어] fctype이 Connect(0x01)인지 - NVMe-oF 1.x 5.x */
}

/*
 * Tests whether a given string represents a valid NQN.
 *
 * [한국어]
 * nvmf_nqn_is_valid - NQN 문법 검증
 *
 * NVMe-oF 1.x 7.9 - "nqn." prefix + reverse-DNS + 길이 제한 등.
 */
bool nvmf_nqn_is_valid(const char *nqn);

/*
 * Tests whether a given NQN describes a discovery subsystem.
 *
 * [한국어]
 * nvmf_nqn_is_discovery - NQN이 표준 Discovery NQN인지 (nqn.2014-08.org.nvmexpress.discovery)
 */
bool nvmf_nqn_is_discovery(const char *nqn);

/**
 * Initiates a zcopy start operation
 *
 * \param bdev The \ref spdk_bdev
 * \param desc The \ref spdk_bdev_desc
 * \param ch The \ref spdk_io_channel
 * \param req The \ref spdk_nvmf_request passed to the bdev for processing
 *
 * \return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE if the command was completed immediately or
 *         SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS if the command was submitted and will be
 *         completed asynchronously.  Asynchronous completions are notified through
 *         spdk_nvmf_request_complete().
 *
 * [한국어]
 * nvmf_bdev_ctrlr_zcopy_start - zero-copy IO 시작 (bdev에서 버퍼 빌리기)
 *
 * Read/Write 명령에서 트랜스포트가 zcopy를 지원하면 호출되어 bdev이 자체 버퍼를 노출,
 * 트랜스포트가 그 버퍼를 직접 호스트로 송수신 - 메모리 복사 1회 절감.
 * 이후 nvmf_bdev_ctrlr_zcopy_end로 commit/abort.
 */
int nvmf_bdev_ctrlr_zcopy_start(struct spdk_bdev *bdev,
				struct spdk_bdev_desc *desc,
				struct spdk_io_channel *ch,
				struct spdk_nvmf_request *req);

/**
 * Ends a zcopy operation
 *
 * \param req The NVMe-oF request
 * \param commit Flag indicating whether the buffers should be committed
 *
 * [한국어]
 * nvmf_bdev_ctrlr_zcopy_end - zero-copy IO 종료
 *
 * @commit: true면 Write 데이터 commit, false면 abort.
 * Read 완료 시에도 호출되어 빌린 버퍼 해제.
 */
void nvmf_bdev_ctrlr_zcopy_end(struct spdk_nvmf_request *req, bool commit);

/**
 * Publishes the mDNS PRR (Pull Registration Request) for the NVMe-oF target.
 *
 * \param tgt The NVMe-oF target
 *
 * \return 0 on success, negative errno on failure
 *
 * [한국어]
 * mdns_server.c (Avahi 빌드) 또는 stubs.c (미빌드)에 구현. 본 헤더는 선언만.
 */
int nvmf_publish_mdns_prr(struct spdk_nvmf_tgt *tgt);

/**
 * Stops the mDNS PRR (Pull Registration Request) for the NVMe-oF target.
 *
 * \param tgt The NVMe-oF target
 *
 * [한국어] 위 함수와 짝.
 */
void nvmf_tgt_stop_mdns_prr(struct spdk_nvmf_tgt *tgt);

/**
 * Updates the listener list in the mDNS PRR (Pull Registration Request) for the NVMe-oF target.
 *
 * \param tgt The NVMe-oF target
 *
 * \return 0 on success, negative errno on failure
 *
 * [한국어] listener add/remove 시 mDNS 엔트리 reset 후 재등록.
 */
int nvmf_tgt_update_mdns_prr(struct spdk_nvmf_tgt *tgt);

/*
 * [한국어]
 * nvmf_get_transport_poll_group - PG 안에서 특정 트랜스포트의 PG-tgroup 검색 (인라인)
 *
 * @group: 상위 nvmf poll group
 * @transport: 찾을 트랜스포트
 * @return: 매칭되는 transport poll group 또는 NULL
 *
 * IO 처리 시 어느 트랜스포트의 PG-측 자원에 접근해야 하는지 빠르게 매핑.
 * 트랜스포트 수는 보통 1~2개로 작아 선형 검색으로 충분.
 */
static inline struct spdk_nvmf_transport_poll_group *
nvmf_get_transport_poll_group(struct spdk_nvmf_poll_group *group,
			      struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport_poll_group *tgroup; /* [한국어] 순회용 임시 */

	TAILQ_FOREACH(tgroup, &group->tgroups, link) { /* [한국어] PG에 등록된 트랜스포트별 group들 순회 */
		if (tgroup->transport == transport) { /* [한국어] 트랜스포트 인스턴스 일치 검사 */
			return tgroup;              /* [한국어] 매칭 반환 */
		}
	}

	return NULL;                                /* [한국어] 미매칭 - 트랜스포트가 PG에 없음 */
}

/**
 * Generates a new NVMF controller id
 *
 * \param subsystem The subsystem
 *
 * \return unique controller id or 0xFFFF when all controller ids are in use
 *
 * [한국어]
 * nvmf_subsystem_gen_cntlid - subsystem 안에서 사용 가능한 다음 cntlid 할당
 *
 * min_cntlid ~ max_cntlid 범위에서 next_cntlid부터 round-robin 검색.
 * 모두 사용 중이면 0xFFFF (invalid) 반환.
 */
uint16_t nvmf_subsystem_gen_cntlid(struct spdk_nvmf_subsystem *subsystem);

#endif /* __NVMF_INTERNAL_H__ */                    /* [한국어] 헤더 가드 종료 */
