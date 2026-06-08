/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   Copyright (c) 2018-2019 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe_FC transport functions.
 */

/*
 * [한국어 설명] NVMe over Fibre Channel(FC) 트랜스포트 구현 (fc.c)
 *
 * === 파일의 역할 ===
 * NVMe-oF의 Fibre Channel(FC) 트랜스포트를 구현한다. FC는 RDMA/TCP와 달리 SCSI 시대부터
 * 데이터센터 SAN의 표준 패브릭으로, NVMe-FC는 FC4 타입 0x28(FC_NVME) 위에 NVMe 캡슐을
 * 실어 나른다. 본 파일은 FC HBA(host bus adapter) 위에서 동작하는 SPDK NVMe-oF target의
 * FC 트랜스포트 ops 테이블을 정의하고, FC LS(Link Service) 처리 dispatcher의 진입점,
 * Association/Connection/HWQP(Hardware Queue Pair) 단위의 객체 생명주기와 I/O 처리
 * 상태 머신(SPDK_NVMF_FC_REQ_*)을 관리한다. 실제 HBA hardware abstraction은 fc_lld.h가
 * 제공하는 LLD(Low Level Driver) 콜백을 통해 위임된다.
 * fc_ls.c가 LS Frame 단위의 Create Association/Connection/Disconnect 등 컨트롤 처리를,
 * 본 파일이 일반 NVMe I/O 캡슐(FC-NVMe CMND IU/DATA IU/RSP IU) 처리와 트랜스포트 ops를
 * 담당하는 분업 관계다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe-FC 객체 계층:
 *   spdk_nvmf_fc_port (1개 FC port = 1개 N_Port)
 *     └── spdk_nvmf_fc_hwqp[] (port가 제공하는 H/W Queue Pair 배열)
 *           └── spdk_nvmf_fc_nport (Virtual N_Port - PRLI 단위)
 *                 └── spdk_nvmf_fc_remote_port_info (호스트 측 FC port)
 *                       └── spdk_nvmf_fc_association (Create Association LS로 생성)
 *                             └── spdk_nvmf_fc_conn[] (Create Connection LS로 추가)
 *                                   └── spdk_nvmf_fc_request (NVMe 명령 in-flight)
 * 호출 체인:
 *   상위: lib/nvmf/transport.c (트랜스포트 ops 디스패치) / nvmf_fc_lld(드라이버 imp)
 *   본 파일: NVMe-FC 트랜스포트 ops + 상태 머신
 *   하위: fc_lld.h ops (실제 HBA 명령 전달) / fc_ls.c (LS frame 처리)
 * 실행 컨텍스트: SPDK 메인 스레드(main_thread, ASSERT_SPDK_FC_MAIN_THREAD로 강제) 위에서
 * 트랜스포트 관리(create/destroy/listen)가 수행되며, I/O 처리는 각 HWQP가 바인딩된
 * poll group 스레드에서 polled-mode로 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * - nvmf_internal.h: spdk_nvmf_request/transport 등 코어 정의
 * - nvmf_fc.h: SPDK FC 트랜스포트 자체 객체(association/conn/hwqp/request 등) 정의
 * - fc_lld.h: HBA 벤더별 LLD 콜백 인터페이스 (Broadcom/Marvell 등이 채워줌)
 * - spdk/nvmf_transport.h: 트랜스포트 ops 등록
 * - spdk_internal/trace_defs.h: FC 단계별 trace point ID 정의
 * 데이터 흐름:
 *   호스트 LS frame -> LLD -> nvmf_fc_handle_ls_rqst (fc_ls.c) -> 응답 LS
 *   호스트 CMND IU -> LLD RQ poll -> nvmf_fc_hwqp_handle_request -> spdk_nvmf_request_exec
 *     -> bdev I/O -> 콜백 -> 상태 머신 진행 -> XFER_RDY/DATA IU 송신 -> RSP IU 송신
 *
 * === 주요 함수/구조체 요약 ===
 * - nvmf_fc_create / nvmf_fc_destroy: 트랜스포트 인스턴스 생성/파기
 * - nvmf_fc_get_main_thread: FC 메인 스레드 핸들 (모든 관리 호출은 여기서)
 * - nvmf_fc_listen / nvmf_fc_stop_listen: FC nport listen (Add/Remove Discovery)
 * - nvmf_fc_request_exec / nvmf_fc_request_complete: I/O 캡슐 상태 머신 처리
 * - nvmf_fc_hwqp_poll: HWQP polling (LLD가 채운 RQ/CQ 소비)
 * - nvmf_fc_create_association / nvmf_fc_delete_association: Association 객체 lifecycle
 * - 상태 enum spdk_nvmf_fc_request_state: 16개 상태 (INIT/READ_*/WRITE_*/RSP/ABORTED 등)
 */

#include "spdk/env.h"                               /* [한국어] DPDK 추상화 - rte_mempool/spdk_mempool 등 자원 풀 사용 */
#include "spdk/assert.h"                            /* [한국어] SPDK_STATIC_ASSERT - 컴파일 타임 구조체 크기/정렬 검사 */
#include "spdk/nvmf_transport.h"                    /* [한국어] spdk_nvmf_transport_ops 인터페이스 - 본 파일이 이 테이블을 채워 등록 */
#include "spdk/string.h"                            /* [한국어] spdk_sprintf_alloc 등 문자열 헬퍼 */
#include "spdk/trace.h"                             /* [한국어] spdk_trace_record - I/O 경로의 latency tracing */
#include "spdk/util.h"                              /* [한국어] SPDK_COUNTOF 등 공통 유틸 */
#include "spdk/likely.h"                            /* [한국어] spdk_likely/unlikely - hot path 분기 힌트 */
#include "spdk/endian.h"                            /* [한국어] FC 프로토콜 빅엔디안 변환 (from_be32 등) */
#include "spdk/log.h"                               /* [한국어] SPDK_ERRLOG/DEBUGLOG - 로깅 */
#include "spdk/thread.h"                            /* [한국어] spdk_thread - 메인/poll group 스레드 식별 */

#include "nvmf_fc.h"                                /* [한국어] SPDK 자체 FC 트랜스포트 객체 정의 */
#include "fc_lld.h"                                 /* [한국어] LLD(Low Level Driver) 콜백 인터페이스 - HBA 벤더별 구현 */

#include "spdk_internal/trace_defs.h"               /* [한국어] FC 전용 trace point ID 매크로 정의 */

#ifndef DEV_VERIFY                                  /* [한국어] LLD에 따라 DEV_VERIFY가 정의되어 있을 수 있음 - 미정의 시 assert로 대체 */
#define DEV_VERIFY assert                           /* [한국어] 디버그 단언문 - 빌드 모드와 무관히 assert 사용 */
#endif

#ifndef ASSERT_SPDK_FC_MAIN_THREAD                  /* [한국어] LLD에서 더 정교한 매크로를 제공할 수 있어 미정의 시만 정의 */
#define ASSERT_SPDK_FC_MAIN_THREAD() \
        DEV_VERIFY(spdk_get_thread() == nvmf_fc_get_main_thread());
/* [한국어] 본 매크로를 호출하는 함수는 FC 트랜스포트의 메인 스레드 위에서만 실행되어야 함을 강제.
 * FC의 nport/association/HWQP 관리 자료구조는 lock-free 모델로 운용되므로,
 * 모든 변경을 단일 스레드(main_thread)에 직렬화해야 안전하다. */
#endif

/*
 * PRLI service parameters
 */
/* [한국어]
 * enum spdk_nvmf_fc_service_parameters - PRLI(Process Login) 시 교환되는 FC4 service param 비트마스크
 *
 * FC 호스트는 NVMe-FC 세션 수립 전 PRLI ELS frame을 보내 양측이 지원하는 기능을 협상한다.
 * NVMe-FC 스펙(FC-NVMe-2 7.5)의 Service Parameter 필드의 각 비트가 아래 값에 대응한다.
 */
enum spdk_nvmf_fc_service_parameters {
	SPDK_NVMF_FC_FIRST_BURST_SUPPORTED = 0x0001,
	/* [한국어] First Burst (FBS) 지원 표시 비트.
	 * 설정자: PRLI 응답에서 본 target이 First Burst를 받을 수 있을 때 set.
	 * 읽는 자: 호스트의 PRLI 처리 코드 - 활성화 시 호스트는 XFER_RDY 없이 첫 데이터를 푸시 가능.
	 * 값 범위: 단일 비트 (0=미지원/1=지원).
	 * 동기화: PRLI 응답 생성 시 단발성으로 결정되며 변경 없음. */

	SPDK_NVMF_FC_DISCOVERY_SERVICE = 0x0008,
	/* [한국어] Discovery Service 역할 비트 - 본 노드가 NVMe-FC discovery controller를 제공함.
	 * 설정자: NVMe-FC subsystem 종류에 따라 nvmf_fc_handle_prli에서 결정.
	 * 읽는 자: 호스트가 이 비트로 discovery vs IO controller 분기.
	 * 값 범위: 단일 비트. 동기화: PRLI 처리 시 단발성. */

	SPDK_NVMF_FC_TARGET_FUNCTION = 0x0010,
	/* [한국어] Target Function 비트 - 본 노드가 NVMe-FC target 역할을 수행함을 알림.
	 * 설정자: 항상 set(SPDK는 target 구현이므로). 값 범위: 단일 비트. */

	SPDK_NVMF_FC_INITIATOR_FUNCTION = 0x0020,
	/* [한국어] Initiator Function 비트 - host 역할 수행 여부.
	 * 설정자: SPDK target 구현에서는 항상 clear. 값 범위: 단일 비트. */

	SPDK_NVMF_FC_CONFIRMED_COMPLETION_SUPPORTED = 0x0080,
	/* [한국어] Confirmed Completion(완료 확인) 지원 비트.
	 * 설정자: SLER(Sequence Level Error Recovery) 사용 시 set.
	 * 읽는 자: 호스트의 retry/recovery 정책 결정.
	 * 값 범위: 단일 비트. 동기화: PRLI 단발성. */
};

/* [한국어]
 * fc_req_state_strs - FC request 상태 enum을 사람이 읽을 수 있는 문자열로 매핑.
 * 인덱스는 enum spdk_nvmf_fc_request_state(nvmf_fc.h)의 정수 값에 1:1 대응.
 * 사용처: SPDK_DEBUGLOG/SPDK_ERRLOG 메시지에서 현재 상태 출력 시.
 * 동기화: 읽기 전용 const(엄밀히는 char* 배열) - 변경 없음. */
static char *fc_req_state_strs[] = {
	"SPDK_NVMF_FC_REQ_INIT",                    /* [한국어] 0: 새 요청 초기 상태 - LLD가 RQ에서 캡슐 받자마자 부여 */
	"SPDK_NVMF_FC_REQ_READ_BDEV",               /* [한국어] 1: Read 명령을 bdev에 submit한 상태 - 데이터 가져오는 중 */
	"SPDK_NVMF_FC_REQ_READ_XFER",               /* [한국어] 2: bdev에서 데이터 읽음, FC DATA IU 전송 중 */
	"SPDK_NVMF_FC_REQ_READ_RSP",                /* [한국어] 3: 데이터 전송 완료, RSP IU 송신 대기/진행 중 */
	"SPDK_NVMF_FC_REQ_WRITE_BUFFS",             /* [한국어] 4: Write 명령 - 호스트 데이터를 받을 버퍼 확보 대기 */
	"SPDK_NVMF_FC_REQ_WRITE_XFER",              /* [한국어] 5: XFER_RDY 송신 후 호스트로부터 DATA IU 수신 중 */
	"SPDK_NVMF_FC_REQ_WRITE_BDEV",              /* [한국어] 6: 모든 DATA IU 수신 완료, bdev write submit 상태 */
	"SPDK_NVMF_FC_REQ_WRITE_RSP",               /* [한국어] 7: bdev write 완료, RSP IU 송신 대기/진행 중 */
	"SPDK_NVMF_FC_REQ_NONE_BDEV",               /* [한국어] 8: 데이터 없는 명령(Flush 등) bdev 처리 중 */
	"SPDK_NVMF_FC_REQ_NONE_RSP",                /* [한국어] 9: 데이터 없는 명령 완료, RSP IU 송신 대기 */
	"SPDK_NVMF_FC_REQ_SUCCESS",                 /* [한국어] 10: 정상 완료 - 객체 회수 직전 */
	"SPDK_NVMF_FC_REQ_FAILED",                  /* [한국어] 11: 실패 완료 - 에러 RSP 송신 후 회수 */
	"SPDK_NVMF_FC_REQ_ABORTED",                 /* [한국어] 12: ABTS(Abort Sequence) 수신/내부 abort로 취소됨 */
	"SPDK_NVMF_FC_REQ_BDEV_ABORTED",            /* [한국어] 13: bdev에 abort 전파 완료, 응답 대기 */
	"SPDK_NVMF_FC_REQ_PENDING",                 /* [한국어] 14: 자원 부족(WAITLIST) 상태 - bdev/buffer 가용 시까지 대기 */
	"SPDK_NVMF_FC_REQ_FUSED_WAITING"            /* [한국어] 15: NVMe Fused command의 짝(첫/두번째) 대기 상태 */
};

#define HWQP_CONN_TABLE_SIZE			8192        /* [한국어] HWQP당 동시 연결(connection) 최대 개수 - hash table 슬롯 수 */
#define HWQP_RPI_TABLE_SIZE			4096        /* [한국어] HWQP당 RPI(Remote Port Index) 최대 개수 - hash table 슬롯 수 */

/*
 * [한국어]
 * nvmf_fc_trace - FC I/O 경로의 trace point(tpoint) 메타데이터를 SPDK trace 프레임워크에 등록.
 *
 * @return: 없음.
 *
 * SPDK trace는 /dev/shm의 공유메모리 환형 버퍼에 lockless로 이벤트를 기록하는데,
 * 기록 전에 각 tpoint ID가 어떤 객체/소유자/문자열 인자를 갖는지 메타데이터를 등록해야
 * trace_parser가 후처리할 수 있다. 본 함수는 FC request의 16개 상태 전이 각각에 대응하는
 * tpoint(FC_NEW, FC_READ_*, FC_WRITE_*, FC_ABRT 등)를 한 번 등록한다.
 * 실행 컨텍스트: SPDK_TRACE_REGISTER_FN 매크로가 등록한 constructor 시점(프로그램 초기화)에
 * trace 프레임워크가 1회 호출. 멀티스레드 호출 없음.
 *
 * 호출 체인:
 *   SPDK trace init → [nvmf_fc_trace] → spdk_trace_register_object/_description
 */
static void
nvmf_fc_trace(void)
{
	spdk_trace_register_object(OBJECT_NVMF_FC_IO, 'r');                  /* [한국어] FC I/O 객체 종류를 'r' 약자로 trace에 등록 - parser가 객체별 그룹핑 */
	spdk_trace_register_description("FC_NEW",
					TRACE_FC_REQ_INIT,
					OWNER_TYPE_NONE, OBJECT_NVMF_FC_IO, 1,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_READ_SBMT_TO_BDEV",
					TRACE_FC_REQ_READ_BDEV,
					OWNER_TYPE_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_READ_XFER_DATA",
					TRACE_FC_REQ_READ_XFER,
					OWNER_TYPE_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_READ_RSP",
					TRACE_FC_REQ_READ_RSP,
					OWNER_TYPE_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_WRITE_NEED_BUFFER",
					TRACE_FC_REQ_WRITE_BUFFS,
					OWNER_TYPE_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_WRITE_XFER_DATA",
					TRACE_FC_REQ_WRITE_XFER,
					OWNER_TYPE_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_WRITE_SBMT_TO_BDEV",
					TRACE_FC_REQ_WRITE_BDEV,
					OWNER_TYPE_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_WRITE_RSP",
					TRACE_FC_REQ_WRITE_RSP,
					OWNER_TYPE_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_NONE_SBMT_TO_BDEV",
					TRACE_FC_REQ_NONE_BDEV,
					OWNER_TYPE_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_NONE_RSP",
					TRACE_FC_REQ_NONE_RSP,
					OWNER_TYPE_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_SUCCESS",
					TRACE_FC_REQ_SUCCESS,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_FAILED",
					TRACE_FC_REQ_FAILED,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_ABRT",
					TRACE_FC_REQ_ABORTED,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_ABRT_SBMT_TO_BDEV",
					TRACE_FC_REQ_BDEV_ABORTED,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_PENDING",
					TRACE_FC_REQ_PENDING,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_FUSED_WAITING",
					TRACE_FC_REQ_FUSED_WAITING,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
}
SPDK_TRACE_REGISTER_FN(nvmf_fc_trace, "nvmf_fc", TRACE_GROUP_NVMF_FC)

/**
 * The structure used by all fc adm functions
 */
/* [한국어]
 * struct spdk_nvmf_fc_adm_api_data - FC adm(administrative) 비동기 호출의 공통 인자 컨테이너.
 * FC 트랜스포트의 관리 명령(port add/delete, nport create 등)은 모두 main_thread에서
 * 비동기로 수행되며, 호출자 인자와 완료 콜백을 한 묶음으로 전달하기 위한 래퍼다. */
struct spdk_nvmf_fc_adm_api_data {
	void *api_args;
	/* [한국어] 실제 adm 명령별 인자 구조체의 불투명 포인터.
	 * 설정자: adm 명령 진입점에서 명령별 args 구조체 주소를 저장.
	 * 읽는 자: 명령 핸들러가 본래 타입으로 캐스팅하여 사용.
	 * 값 범위: 유효한 args 포인터. 동기화: main_thread 단일 소유. */

	spdk_nvmf_fc_callback cb_func;
	/* [한국어] adm 명령 완료 시 호출되는 콜백 함수 포인터.
	 * 설정자: adm 명령 발행자가 등록. 읽는 자: 명령 완료 경로.
	 * 값 범위: 유효한 함수 포인터 또는 NULL. 동기화: main_thread에서 호출. */
};

/**
 * The callback structure for nport-delete
 */
/* [한국어]
 * struct spdk_nvmf_fc_adm_nport_del_cb_data - nport 삭제 완료 콜백에 넘길 컨텍스트.
 * Virtual N_Port(nport)를 제거하는 비동기 절차가 끝났을 때 어느 nport였는지와
 * 원래 호출자 콜백을 복원하기 위해 유지한다. */
struct spdk_nvmf_fc_adm_nport_del_cb_data {
	struct spdk_nvmf_fc_nport *nport;
	/* [한국어] 삭제 대상 Virtual N_Port 객체.
	 * 설정자: nport-delete 진입점. 읽는 자: 완료 콜백.
	 * 값 범위: 유효 포인터. 동기화: main_thread 직렬화. */

	uint8_t port_handle;
	/* [한국어] nport가 속한 물리 FC port의 핸들(LLD가 부여한 port 번호).
	 * 설정자: 진입점. 읽는 자: 완료 콜백이 호출자에게 결과 보고 시.
	 * 값 범위: 0~255. 동기화: main_thread. */

	spdk_nvmf_fc_callback fc_cb_func;
	/* [한국어] 원래 호출자가 등록한 완료 콜백.
	 * 설정자: 진입점. 읽는 자: 삭제 완료 시 호출. 동기화: main_thread. */

	void *fc_cb_ctx;
	/* [한국어] fc_cb_func에 전달할 사용자 컨텍스트.
	 * 설정자: 진입점. 읽는 자: 콜백 호출 시. 동기화: main_thread. */
};

/**
 * The callback structure for it-delete
 */
/* [한국어]
 * struct spdk_nvmf_fc_adm_i_t_del_cb_data - I_T(Initiator-Target) nexus 삭제 콜백 컨텍스트.
 * 특정 호스트(rport)와 nport 사이의 모든 association을 제거하는 비동기 절차의 완료를
 * 전달하기 위한 컨테이너다. */
struct spdk_nvmf_fc_adm_i_t_del_cb_data {
	struct spdk_nvmf_fc_nport *nport;
	/* [한국어] I_T nexus가 속한 nport(target 측).
	 * 설정자: it-delete 진입점. 읽는 자: 완료 콜백. 동기화: main_thread. */

	struct spdk_nvmf_fc_remote_port_info *rport;
	/* [한국어] 상대 호스트 FC port 정보(initiator 측).
	 * 설정자: 진입점. 읽는 자: 완료 콜백이 어느 호스트인지 식별. 동기화: main_thread. */

	uint8_t port_handle;
	/* [한국어] 물리 FC port 핸들. 설정자: 진입점. 읽는 자: 완료 보고. 동기화: main_thread. */

	spdk_nvmf_fc_callback fc_cb_func;
	/* [한국어] 호출자 완료 콜백. 설정자: 진입점. 읽는 자: 완료 시. 동기화: main_thread. */

	void *fc_cb_ctx;
	/* [한국어] fc_cb_func용 사용자 컨텍스트. 설정자: 진입점. 동기화: main_thread. */
};


/* [한국어] I_T nexus 삭제에 딸린 association 삭제 단계의 완료 콜백 타입.
 * arg: 콜백 컨텍스트 / err: 0=성공, 비0=오류 코드. main_thread에서 호출됨. */
typedef void (*spdk_nvmf_fc_adm_i_t_delete_assoc_cb_fn)(void *arg, uint32_t err);

/**
 * The callback structure for the it-delete-assoc callback
 */
/* [한국어]
 * struct spdk_nvmf_fc_adm_i_t_del_assoc_cb_data - I_T 삭제의 association 정리 콜백 컨텍스트.
 * I_T nexus 삭제는 그 아래 매달린 모든 association을 먼저 끊어야 하며, 그 중간 단계 완료를
 * 전달하기 위한 구조체다(상위 i_t_del_cb_data와 구분되는 내부 단계). */
struct spdk_nvmf_fc_adm_i_t_del_assoc_cb_data {
	struct spdk_nvmf_fc_nport *nport;
	/* [한국어] 대상 nport. 설정자: I_T 삭제 진입점. 읽는 자: assoc 정리 완료 콜백. 동기화: main_thread. */

	struct spdk_nvmf_fc_remote_port_info *rport;
	/* [한국어] 상대 호스트 rport. 설정자: 진입점. 읽는 자: 완료 콜백. 동기화: main_thread. */

	uint8_t port_handle;
	/* [한국어] FC port 핸들. 설정자: 진입점. 읽는 자: 결과 보고. 동기화: main_thread. */

	spdk_nvmf_fc_adm_i_t_delete_assoc_cb_fn cb_func;
	/* [한국어] association 정리 완료 콜백. 설정자: 진입점. 읽는 자: 완료 시. 동기화: main_thread. */

	void *cb_ctx;
	/* [한국어] cb_func용 컨텍스트. 설정자: 진입점. 동기화: main_thread. */
};

/*
 * Call back function pointer for HW port quiesce.
 */
/* [한국어] HW port quiesce(I/O 정지) 완료 콜백 타입. ctx: 컨텍스트 / err: 결과 코드.
 * port reset 등에서 HBA의 모든 HWQP에 신규 I/O 수신을 멈추라고 요청한 뒤 호출된다. */
typedef void (*spdk_nvmf_fc_adm_hw_port_quiesce_cb_fn)(void *ctx, int err);

/**
 * Context structure for quiescing a hardware port
 */
/* [한국어]
 * struct spdk_nvmf_fc_adm_hw_port_quiesce_ctx - HW port의 여러 HWQP를 동시에 quiesce할 때의 진행 추적.
 * port에는 여러 HWQP가 있으므로 각 HWQP의 quiesce 완료를 카운트다운하여 모두 끝났을 때만
 * 상위 콜백을 호출한다(fan-in 동기화). */
struct spdk_nvmf_fc_adm_hw_port_quiesce_ctx {
	int quiesce_count;
	/* [한국어] 아직 quiesce 완료를 기다리는 HWQP 개수(카운트다운).
	 * 설정자: quiesce 시작 시 HWQP 수로 초기화. 읽는 자/감소자: 각 HWQP quiesce 완료 콜백.
	 * 값 범위: 0 이상. 0 도달 시 상위 cb_func 호출. 동기화: main_thread 단일 처리. */

	void *ctx;
	/* [한국어] 상위 호출자(port reset 등)의 컨텍스트.
	 * 설정자: quiesce 시작 시. 읽는 자: 완료 콜백. 동기화: main_thread. */

	spdk_nvmf_fc_adm_hw_port_quiesce_cb_fn cb_func;
	/* [한국어] 모든 HWQP quiesce 완료 시 호출할 콜백. 설정자: 시작 시. 동기화: main_thread. */
};

/**
 * Context structure used to reset a hardware port
 */
/* [한국어]
 * struct spdk_nvmf_fc_adm_hw_port_reset_ctx - HW port reset 절차의 컨텍스트.
 * quiesce → dump → re-init의 다단계 port reset에서 원래 reset 인자와 완료 콜백을 보관한다. */
struct spdk_nvmf_fc_adm_hw_port_reset_ctx {
	void *reset_args;
	/* [한국어] port reset 명령의 원본 인자 구조체.
	 * 설정자: reset 진입점. 읽는 자: reset 완료 핸들러. 동기화: main_thread. */

	spdk_nvmf_fc_callback reset_cb_func;
	/* [한국어] reset 완료 콜백. 설정자: 진입점. 읽는 자: 완료 시. 동기화: main_thread. */
};

/* [한국어]
 * struct spdk_nvmf_fc_transport - SPDK NVMe-oF 코어 transport를 FC로 확장한 래퍼.
 * spdk_nvmf_transport(코어)를 첫 멤버로 임베드하여 코어 ↔ FC 간 container_of 캐스팅이
 * 가능하게 하고, FC 고유의 accept poller와 전역 자료구조 보호 락을 추가로 보유한다. */
struct spdk_nvmf_fc_transport {
	struct spdk_nvmf_transport transport;
	/* [한국어] NVMe-oF 코어 transport 객체(반드시 첫 멤버 - 캐스팅 호환).
	 * 설정자: nvmf_fc_create. 읽는 자: 코어 nvmf가 ops를 디스패치할 때.
	 * 값 범위: 임베드된 값. 동기화: 코어 nvmf 규약을 따름. */

	struct spdk_poller *accept_poller;
	/* [한국어] FC LS(Link Service) 큐를 주기적으로 폴링하는 poller 핸들.
	 * 설정자: nvmf_fc_create가 spdk_poller_register로 등록.
	 * 읽는 자: nvmf_fc_destroy가 unregister. 동기화: main_thread. */

	pthread_mutex_t lock;
	/* [한국어] 전역 poll group 리스트(g_nvmf_fgroups) 등 cross-thread 접근 자료구조 보호 락.
	 * 설정자/사용자: poll group 생성/소멸, hwqp 배정 코드가 lock/unlock.
	 * 값 범위: pthread_mutex. 동기화: poll group 배정은 여러 스레드 컨텍스트가 접근하므로
	 *   FC의 lock-free 원칙이 적용 안 되는 예외 지점이라 명시적 mutex 필요. */
};

/* [한국어] FC 트랜스포트의 유일한 인스턴스 전역 포인터.
 * 설정자: nvmf_fc_create. 읽는 자: 전역 락 접근/poll group 배정 등. 동기화: 생성 후 read-mostly. */
static struct spdk_nvmf_fc_transport *g_nvmf_ftransport;

/* [한국어] 트랜스포트 destroy의 비동기 완료를 알릴 콜백(파기 중 보존).
 * 설정자: nvmf_fc_destroy. 읽는 자: 파기 완료 경로. 값 범위: 함수 포인터/NULL. 동기화: main_thread. */
static spdk_nvmf_transport_destroy_done_cb g_transport_destroy_done_cb = NULL;

/* [한국어] 시스템에 등록된 모든 FC port 객체의 전역 리스트(TAILQ).
 * 설정자: port add/remove adm 경로. 읽는 자: port 조회 함수들. 동기화: main_thread 직렬화. */
static TAILQ_HEAD(, spdk_nvmf_fc_port) g_spdk_nvmf_fc_port_list =
	TAILQ_HEAD_INITIALIZER(g_spdk_nvmf_fc_port_list);

/* [한국어] FC 트랜스포트의 main_thread 핸들. 모든 관리(adm) 호출의 직렬화 기준점.
 * 설정자: nvmf_fc_create. 읽는 자: ASSERT_SPDK_FC_MAIN_THREAD/cross-thread send_msg 대상. 동기화: 생성 후 불변. */
static struct spdk_thread *g_nvmf_fc_main_thread = NULL;

/* [한국어] 생성된 FC poll group 개수. HWQP 배정 시 유효성 검사에 사용.
 * 설정자: poll group create/destroy. 읽는 자: add_hwqp assert. 동기화: g_nvmf_ftransport->lock. */
static uint32_t g_nvmf_fgroup_count = 0;
/* [한국어] 모든 FC poll group의 전역 리스트. HWQP를 가장 한가한 poll group에 배정할 때 순회.
 * 설정자: poll group create/destroy. 읽는 자: assign_idlest_poll_group. 동기화: g_nvmf_ftransport->lock. */
static TAILQ_HEAD(, spdk_nvmf_fc_poll_group) g_nvmf_fgroups =
	TAILQ_HEAD_INITIALIZER(g_nvmf_fgroups);

/*
 * [한국어]
 * nvmf_fc_get_main_thread - FC 트랜스포트의 main_thread 핸들을 반환.
 *
 * @return: g_nvmf_fc_main_thread (생성 후 불변, 초기화 전엔 NULL).
 *
 * FC의 모든 관리(adm) 자료구조는 lock-free로 main_thread에 직렬화되므로,
 * cross-thread에서 관리 작업을 하려면 이 스레드로 spdk_thread_send_msg를 보내야 한다.
 * ASSERT_SPDK_FC_MAIN_THREAD 매크로도 이 함수로 현재 스레드를 검증한다.
 * 실행 컨텍스트: 어느 스레드에서든 호출 가능(단순 전역 read).
 *
 * 호출 체인:
 *   ASSERT_SPDK_FC_MAIN_THREAD / send_msg 호출자 → [nvmf_fc_get_main_thread]
 */
struct spdk_thread *
nvmf_fc_get_main_thread(void)
{
	return g_nvmf_fc_main_thread;                                       /* [한국어] 생성 시 설정된 main_thread 전역을 그대로 반환 */
}

/*
 * [한국어]
 * nvmf_fc_record_req_trace_point - FC request의 상태 전이 1건을 trace 버퍼에 기록.
 *
 * @fc_req: 상태가 바뀐 FC request 객체.
 * @state: 새로 진입한 상태(enum spdk_nvmf_fc_request_state).
 * @return: 없음.
 *
 * I/O latency 분석을 위해, 상태 머신이 각 단계로 진입할 때마다 해당 단계의 tpoint를
 * 시각(TSC)과 함께 lockless 공유메모리 버퍼에 남긴다. 본 함수는 enum 상태를 tpoint ID로
 * 매핑한 뒤 spdk_trace_record를 호출한다. nvmf_fc_trace에서 미리 등록한 메타데이터와 짝을 이룬다.
 * 실행 컨텍스트: HWQP가 바인딩된 poll group 스레드(I/O hot path). inline으로 오버헤드 최소화.
 *
 * 호출 체인:
 *   nvmf_fc_request_set_state → [nvmf_fc_record_req_trace_point] → spdk_trace_record
 */
static inline void
nvmf_fc_record_req_trace_point(struct spdk_nvmf_fc_request *fc_req,
			       enum spdk_nvmf_fc_request_state state)
{
	uint16_t tpoint_id = SPDK_TRACE_MAX_TPOINT_ID;                      /* [한국어] 기본값을 sentinel(무효 ID)로 두어, 매핑 실패 시 기록을 건너뛰게 함 */

	switch (state) {                                                    /* [한국어] 새 상태값에 따라 대응하는 tpoint ID 선택 */
	case SPDK_NVMF_FC_REQ_INIT:
		/* Start IO tracing */
		tpoint_id = TRACE_FC_REQ_INIT;
		break;
	case SPDK_NVMF_FC_REQ_READ_BDEV:
		tpoint_id = TRACE_FC_REQ_READ_BDEV;
		break;
	case SPDK_NVMF_FC_REQ_READ_XFER:
		tpoint_id = TRACE_FC_REQ_READ_XFER;
		break;
	case SPDK_NVMF_FC_REQ_READ_RSP:
		tpoint_id = TRACE_FC_REQ_READ_RSP;
		break;
	case SPDK_NVMF_FC_REQ_WRITE_BUFFS:
		tpoint_id = TRACE_FC_REQ_WRITE_BUFFS;
		break;
	case SPDK_NVMF_FC_REQ_WRITE_XFER:
		tpoint_id = TRACE_FC_REQ_WRITE_XFER;
		break;
	case SPDK_NVMF_FC_REQ_WRITE_BDEV:
		tpoint_id = TRACE_FC_REQ_WRITE_BDEV;
		break;
	case SPDK_NVMF_FC_REQ_WRITE_RSP:
		tpoint_id = TRACE_FC_REQ_WRITE_RSP;
		break;
	case SPDK_NVMF_FC_REQ_NONE_BDEV:
		tpoint_id = TRACE_FC_REQ_NONE_BDEV;
		break;
	case SPDK_NVMF_FC_REQ_NONE_RSP:
		tpoint_id = TRACE_FC_REQ_NONE_RSP;
		break;
	case SPDK_NVMF_FC_REQ_SUCCESS:
		tpoint_id = TRACE_FC_REQ_SUCCESS;
		break;
	case SPDK_NVMF_FC_REQ_FAILED:
		tpoint_id = TRACE_FC_REQ_FAILED;
		break;
	case SPDK_NVMF_FC_REQ_ABORTED:
		tpoint_id = TRACE_FC_REQ_ABORTED;
		break;
	case SPDK_NVMF_FC_REQ_BDEV_ABORTED:
		tpoint_id = TRACE_FC_REQ_ABORTED;
		break;
	case SPDK_NVMF_FC_REQ_PENDING:
		tpoint_id = TRACE_FC_REQ_PENDING;
		break;
	case SPDK_NVMF_FC_REQ_FUSED_WAITING:
		tpoint_id = TRACE_FC_REQ_FUSED_WAITING;
		break;
	default:
		assert(0);                                                 /* [한국어] 알 수 없는 상태값은 프로그래밍 오류 - 디버그 빌드에서 즉시 중단 */
		break;
	}
	if (tpoint_id != SPDK_TRACE_MAX_TPOINT_ID) {                        /* [한국어] 유효 tpoint로 매핑된 경우에만 기록(default 분기는 sentinel 유지로 스킵) */
		spdk_trace_record(tpoint_id, fc_req->poller_lcore, 0,      /* [한국어] (tpoint, lcore, size, arg) - lcore별 lockless 버퍼에 TSC와 함께 기록 */
				  (uint64_t)(&fc_req->req));                /* [한국어] arg로 req 주소를 넘겨 parser가 동일 I/O의 상태 전이들을 연결 */
	}
}

/*
 * [한국어]
 * nvmf_fc_create_hash_table - DPDK rte_hash 기반 해시 테이블을 생성하는 헬퍼.
 *
 * @name: 해시 테이블 이름(DPDK 내부 식별용, 전역 유일해야 함).
 * @num_entries: 최대 엔트리 수(슬롯 개수).
 * @key_len: 키 바이트 길이.
 * @return: 생성된 rte_hash 포인터, 실패 시 NULL.
 *
 * HWQP는 connection ID(64비트)와 RPI(16비트)로 빠른 조회가 필요한데, DPDK rte_hash는
 * lockless 동시 lookup을 지원하는 cuckoo 해시여서 polled-mode I/O 경로에 적합하다.
 * 본 함수는 공통 파라미터 설정을 캡슐화한다.
 * 실행 컨텍스트: nvmf_fc_init_hwqp(HWQP 초기화 시점, main_thread).
 *
 * 호출 체인:
 *   nvmf_fc_init_hwqp → [nvmf_fc_create_hash_table] → rte_hash_create
 */
static struct rte_hash *
nvmf_fc_create_hash_table(const char *name, size_t num_entries, size_t key_len)
{
	struct rte_hash_parameters hash_params = { 0 };                    /* [한국어] DPDK 해시 생성 파라미터 - 0으로 초기화 후 필요 필드만 설정 */

	hash_params.entries = num_entries;                                 /* [한국어] 테이블 용량(슬롯 수) 설정 */
	hash_params.key_len = key_len;                                     /* [한국어] 키 길이 설정(conn은 8B, rpi는 2B) */
	hash_params.name = name;                                           /* [한국어] 테이블 식별 이름 설정 */

	return rte_hash_create(&hash_params);                              /* [한국어] DPDK에 해시 테이블 생성 요청 - hugepage 메모리에 할당 */
}

/*
 * [한국어]
 * nvmf_fc_free_conn_reqpool - connection의 FC request 객체 풀 메모리를 해제.
 *
 * @fc_conn: 대상 connection 객체.
 * @return: 없음.
 *
 * connection 생성 시 max_queue_depth*2 개의 fc_request를 한 덩어리로 calloc해 두는데,
 * connection 소멸/생성 실패 시 그 메모리를 통째로 free한다.
 * 실행 컨텍스트: main_thread(connection lifecycle).
 *
 * 호출 체인:
 *   connection destroy / create_conn_reqpool 실패 → [nvmf_fc_free_conn_reqpool] → free
 */
void
nvmf_fc_free_conn_reqpool(struct spdk_nvmf_fc_conn *fc_conn)
{
	free(fc_conn->pool_memory);                                        /* [한국어] 풀로 잡아둔 연속 메모리 블록 해제 */
	fc_conn->pool_memory = NULL;                                       /* [한국어] dangling 방지를 위해 NULL로 표시 */
}

/*
 * [한국어]
 * nvmf_fc_create_conn_reqpool - connection별 FC request 객체 풀을 미리 할당하고 free 리스트로 연결.
 *
 * @fc_conn: 풀을 생성할 connection 객체.
 * @return: 0=성공, -1=메모리 부족 실패.
 *
 * I/O hot path에서 매번 malloc하지 않도록, connection 수립 시 fc_request를 한꺼번에 미리
 * 잡아 STAILQ free 리스트(pool_queue)로 엮어둔다. 큐 깊이의 2배를 잡는 이유는, target이
 * RSP를 보낸 직후 그 CQE를 처리하기 전에 호스트가 새 명령을 보내 RQE가 먼저 reap되는
 * race를 흡수하기 위함이다(원문 영어 주석 참조).
 * 실행 컨텍스트: main_thread(connection 생성).
 *
 * 호출 체인:
 *   connection create → [nvmf_fc_create_conn_reqpool] → calloc / STAILQ_INSERT_TAIL
 */
int
nvmf_fc_create_conn_reqpool(struct spdk_nvmf_fc_conn *fc_conn)
{
	uint32_t i, qd;                                                    /* [한국어] i: 루프 인덱스, qd: 풀 크기(=큐 깊이의 2배) */
	struct spdk_nvmf_fc_pooled_request *req;                           /* [한국어] 풀 링크용 경량 헤더 뷰 - fc_request 메모리를 free 리스트로 엮는 캐스팅 타입 */

	/*
	 * Create number of fc-requests to be more than the actual SQ size.
	 * This is to handle race conditions where the target driver may send
	 * back a RSP and before the target driver gets to process the CQE
	 * for the RSP, the initiator may have sent a new command.
	 * Depending on the load on the HWQP, there is a slim possibility
	 * that the target reaps the RQE corresponding to the new
	 * command before processing the CQE corresponding to the RSP.
	 */
	qd = fc_conn->max_queue_depth * 2;                                 /* [한국어] RSP CQE-신규 RQE reap race 흡수를 위해 SQ 크기의 2배로 over-provision */

	STAILQ_INIT(&fc_conn->pool_queue);                                 /* [한국어] free 리스트(STAILQ) 헤드 초기화 */
	fc_conn->pool_memory = calloc((fc_conn->max_queue_depth * 2),      /* [한국어] qd개의 fc_request를 연속 메모리로 한 번에 할당(0 초기화) */
				      sizeof(struct spdk_nvmf_fc_request));
	if (!fc_conn->pool_memory) {                                       /* [한국어] 할당 실패 시 에러 경로로 */
		SPDK_ERRLOG("create fc req ring objects failed\n");
		goto error;
	}
	fc_conn->pool_size = qd;                                           /* [한국어] 풀 총 크기 기록 */
	fc_conn->pool_free_elems = qd;                                     /* [한국어] 초기에는 모든 원소가 free 상태 */

	/* Initialise value in ring objects and link the objects */
	for (i = 0; i < qd; i++) {                                         /* [한국어] qd개의 원소를 순회하며 free 리스트에 연결 */
		req = (struct spdk_nvmf_fc_pooled_request *)((char *)fc_conn->pool_memory +  /* [한국어] i번째 fc_request 시작 주소를 pooled_request 뷰로 캐스팅 */
				i * sizeof(struct spdk_nvmf_fc_request));         /* [한국어] sizeof(fc_request) 단위로 오프셋 - 풀 원소 간 stride */

		STAILQ_INSERT_TAIL(&fc_conn->pool_queue, req, pool_link);  /* [한국어] free 리스트 꼬리에 삽입 */
	}
	return 0;                                                          /* [한국어] 풀 구성 성공 */
error:
	nvmf_fc_free_conn_reqpool(fc_conn);                                /* [한국어] 부분 할당 정리(여기선 pool_memory만 free) */
	return -1;                                                         /* [한국어] 실패 보고 - 호출자는 connection 생성을 중단 */
}

/*
 * [한국어]
 * nvmf_fc_conn_alloc_fc_request - connection의 free 풀에서 fc_request 하나를 꺼내 초기화.
 *
 * @fc_conn: 요청을 할당받을 connection.
 * @return: 초기화된 fc_request 포인터, 풀 고갈 시 NULL.
 *
 * 새 NVMe-FC 명령 캡슐(CMND IU)이 도착하면 호출되어 free 리스트에서 객체를 떼어내고,
 * 상태를 INIT으로 세팅한 뒤 HWQP/connection의 in-use 리스트에 등록한다. malloc 없이
 * 미리 잡은 풀에서 O(1)로 꺼내므로 hot path에 적합하다.
 * 실행 컨텍스트: HWQP poll group 스레드(I/O hot path). inline.
 *
 * 호출 체인:
 *   nvmf_fc_hwqp_handle_request → [nvmf_fc_conn_alloc_fc_request] → STAILQ_REMOVE_HEAD/set_state
 */
static inline struct spdk_nvmf_fc_request *
nvmf_fc_conn_alloc_fc_request(struct spdk_nvmf_fc_conn *fc_conn)
{
	struct spdk_nvmf_fc_request *fc_req;                               /* [한국어] 반환할 실제 fc_request 포인터 */
	struct spdk_nvmf_fc_pooled_request *pooled_req;                   /* [한국어] free 리스트에서 떼어낼 풀 헤더 뷰 */
	struct spdk_nvmf_fc_hwqp *hwqp = fc_conn->hwqp;                   /* [한국어] connection이 바인딩된 HWQP - in_use 리스트 등록 대상 */

	pooled_req = STAILQ_FIRST(&fc_conn->pool_queue);                  /* [한국어] free 리스트 head 참조(아직 제거 전) */
	if (!pooled_req) {                                                /* [한국어] 풀 고갈 - in-flight가 큐 깊이 2배에 도달한 비정상 상황 */
		SPDK_ERRLOG("Alloc request buffer failed\n");
		return NULL;
	}
	STAILQ_REMOVE_HEAD(&fc_conn->pool_queue, pool_link);              /* [한국어] free 리스트에서 head를 제거(할당 확정) */
	fc_conn->pool_free_elems -= 1;                                    /* [한국어] free 카운트 감소 */

	fc_req = (struct spdk_nvmf_fc_request *)pooled_req;               /* [한국어] 풀 헤더 주소를 실제 fc_request 타입으로 환원(메모리 동일) */
	memset(fc_req, 0, sizeof(struct spdk_nvmf_fc_request));           /* [한국어] 이전 사용 흔적 제거 - 깨끗한 상태로 재사용 */
	nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_INIT);         /* [한국어] 상태 머신 시작점 INIT으로 설정(+trace 기록) */

	TAILQ_INSERT_TAIL(&hwqp->in_use_reqs, fc_req, link);             /* [한국어] HWQP 단위 in-use 리스트에 등록(abort 시 순회 대상) */
	TAILQ_INSERT_TAIL(&fc_conn->in_use_reqs, fc_req, conn_link);     /* [한국어] connection 단위 in-use 리스트에 등록(conn drain 시 순회) */
	TAILQ_INIT(&fc_req->abort_cbs);                                  /* [한국어] 이 요청에 대한 abort 콜백 리스트 초기화 */
	return fc_req;                                                    /* [한국어] 초기화된 요청 반환 - 호출자가 명령 파싱 진행 */
}

/*
 * [한국어]
 * nvmf_fc_conn_free_fc_request - 처리 완료된 fc_request를 in-use 리스트에서 빼고 free 풀로 반환.
 *
 * @fc_conn: 요청이 속한 connection.
 * @fc_req: 반환할 fc_request.
 * @return: 없음.
 *
 * I/O가 RSP 송신까지 끝나거나 abort로 종료되면 호출되어 객체를 재사용 풀에 되돌린다.
 * magic을 0xDEADBEEF로 덮어 use-after-free를 조기 감지하도록 한다.
 * 실행 컨텍스트: HWQP poll group 스레드(I/O 완료 경로). inline.
 *
 * 호출 체인:
 *   nvmf_fc_request_free → [nvmf_fc_conn_free_fc_request] → TAILQ_REMOVE/STAILQ_INSERT_HEAD
 */
static inline void
nvmf_fc_conn_free_fc_request(struct spdk_nvmf_fc_conn *fc_conn, struct spdk_nvmf_fc_request *fc_req)
{
	if (fc_req->state != SPDK_NVMF_FC_REQ_SUCCESS) {                  /* [한국어] 정상 완료(SUCCESS)가 아닌 채 해제되면 디버깅을 위해 FAILED로 표시 */
		/* Log an error for debug purpose. */
		nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_FAILED);
	}

	/* set the magic to mark req as no longer valid. */
	fc_req->magic = 0xDEADBEEF;                                      /* [한국어] use-after-free 감지용 sentinel - 이후 이 객체를 유효 req로 쓰면 magic 검사 실패 */

	TAILQ_REMOVE(&fc_conn->hwqp->in_use_reqs, fc_req, link);         /* [한국어] HWQP in-use 리스트에서 제거 */
	TAILQ_REMOVE(&fc_conn->in_use_reqs, fc_req, conn_link);          /* [한국어] connection in-use 리스트에서 제거 */

	STAILQ_INSERT_HEAD(&fc_conn->pool_queue, (struct spdk_nvmf_fc_pooled_request *)fc_req, pool_link);  /* [한국어] free 리스트 head에 반환(LIFO - 캐시 지역성) */
	fc_conn->pool_free_elems += 1;                                   /* [한국어] free 카운트 복원 */
}

/*
 * [한국어]
 * nvmf_fc_request_remove_from_pending - 자원 대기 큐(pending_buf_queue)에서 요청을 제거.
 *
 * @fc_req: 제거할 요청.
 * @return: 없음.
 *
 * write 데이터 버퍼 등 자원이 부족해 PENDING 상태로 코어 nvmf의 pending_buf_queue에
 * 매달려 있던 요청이 자원을 얻거나 abort될 때 그 큐에서 떼어낸다.
 * 실행 컨텍스트: HWQP poll group 스레드. inline.
 *
 * 호출 체인:
 *   요청 재개/abort → [nvmf_fc_request_remove_from_pending] → STAILQ_REMOVE
 */
static inline void
nvmf_fc_request_remove_from_pending(struct spdk_nvmf_fc_request *fc_req)
{
	STAILQ_REMOVE(&fc_req->hwqp->fgroup->group.pending_buf_queue, &fc_req->req,  /* [한국어] poll group이 소유한 버퍼 대기 큐에서 코어 req 노드를 제거 */
		      spdk_nvmf_request, buf_link);                      /* [한국어] STAILQ_REMOVE의 타입/링크 멤버 인자(코어 req의 buf_link) */
}

/*
 * [한국어]
 * nvmf_fc_init_hwqp - HWQP(Hardware Queue Pair) 객체를 초기화.
 *
 * @fc_port: HWQP가 속한 물리 FC port.
 * @hwqp: 초기화할 HWQP 객체.
 * @return: 0=성공, -ENOMEM=해시 테이블 생성 실패.
 *
 * port 등록 시 그 port가 제공하는 각 HWQP에 대해 호출되어, in-use/sync/ls_pending 리스트와
 * connection·RPI 조회용 DPDK 해시 테이블을 만들고 LLD의 하드웨어 큐를 초기화한다.
 * HWQP는 I/O가 실제로 흐르는 최하위 큐 단위로, 이후 poll group에 배정된다.
 * 실행 컨텍스트: main_thread(port lifecycle).
 *
 * 호출 체인:
 *   port add → [nvmf_fc_init_hwqp] → nvmf_fc_create_hash_table / nvmf_fc_init_q(LLD)
 */
int
nvmf_fc_init_hwqp(struct spdk_nvmf_fc_port *fc_port, struct spdk_nvmf_fc_hwqp *hwqp)
{
	char name[64];                                                    /* [한국어] 해시 테이블 식별 이름 버퍼(port-hwqp 조합으로 유일화) */

	hwqp->fc_port = fc_port;                                          /* [한국어] 소속 port 역참조 설정 */

	/* clear counters */
	memset(&hwqp->counters, 0, sizeof(struct spdk_nvmf_fc_errors));   /* [한국어] 에러/통계 카운터 0으로 초기화 */

	TAILQ_INIT(&hwqp->in_use_reqs);                                  /* [한국어] 처리 중 요청 리스트 초기화 */
	TAILQ_INIT(&hwqp->sync_cbs);                                     /* [한국어] HWQP 동기화 콜백 리스트 초기화(큐 동기화 barrier용) */
	TAILQ_INIT(&hwqp->ls_pending_queue);                            /* [한국어] LS frame 대기 큐 초기화 */

	snprintf(name, sizeof(name), "nvmf_fc_conn_hash:%d-%d", fc_port->port_hdl, hwqp->hwqp_id);  /* [한국어] connection 해시 테이블 이름을 port/hwqp로 유일화 */
	hwqp->connection_list_hash = nvmf_fc_create_hash_table(name, HWQP_CONN_TABLE_SIZE,  /* [한국어] conn ID(64비트)→conn 조회용 해시 생성 */
				     sizeof(uint64_t));
	if (!hwqp->connection_list_hash) {                              /* [한국어] 해시 생성 실패 시 즉시 오류 반환 */
		SPDK_ERRLOG("Failed to create connection hash table.\n");
		return -ENOMEM;
	}

	snprintf(name, sizeof(name), "nvmf_fc_rpi_hash:%d-%d", fc_port->port_hdl, hwqp->hwqp_id);  /* [한국어] RPI 해시 테이블 이름 유일화 */
	hwqp->rport_list_hash = nvmf_fc_create_hash_table(name, HWQP_RPI_TABLE_SIZE, sizeof(uint16_t));  /* [한국어] RPI(16비트)→rport 조회용 해시 생성 */
	if (!hwqp->rport_list_hash) {                                  /* [한국어] 두 번째 해시 실패 시 첫 해시를 정리하고 반환(누수 방지) */
		SPDK_ERRLOG("Failed to create rpi hash table.\n");
		rte_hash_free(hwqp->connection_list_hash);
		return -ENOMEM;
	}

	/* Init low level driver queues */
	nvmf_fc_init_q(hwqp);                                          /* [한국어] LLD에 위임 - HBA 하드웨어 SQ/CQ/RQ 큐를 실제로 초기화 */
	return 0;                                                      /* [한국어] HWQP 초기화 성공 */
}

/*
 * [한국어]
 * nvmf_fc_assign_idlest_poll_group - HWQP를 가장 한가한 poll group에 배정.
 *
 * @hwqp: 배정할 HWQP.
 * @return: 선택된 poll group, 없으면 NULL.
 *
 * HWQP를 어느 reactor(spdk_thread)에서 폴링할지 부하 분산하기 위해, 현재 가장 적은
 * HWQP를 가진 poll group을 선택해 그 스레드에 바인딩한다. 전역 poll group 리스트는
 * 여러 스레드가 접근할 수 있어 g_nvmf_ftransport->lock으로 보호한다(FC lock-free 예외).
 * 실행 컨텍스트: main_thread(또는 add_hwqp 경로). pthread_mutex로 보호.
 *
 * 호출 체인:
 *   nvmf_fc_poll_group_add_hwqp → [nvmf_fc_assign_idlest_poll_group]
 */
static struct spdk_nvmf_fc_poll_group *
nvmf_fc_assign_idlest_poll_group(struct spdk_nvmf_fc_hwqp *hwqp)
{
	uint32_t max_count = UINT32_MAX;                                 /* [한국어] 지금까지 본 최소 hwqp_count(처음엔 최대값으로 시작) */
	struct spdk_nvmf_fc_poll_group *fgroup;                          /* [한국어] 순회 커서 */
	struct spdk_nvmf_fc_poll_group *ret_fgroup = NULL;              /* [한국어] 선택될(가장 한가한) poll group */

	pthread_mutex_lock(&g_nvmf_ftransport->lock);                   /* [한국어] 전역 poll group 리스트 보호 - cross-thread 동시 변경 차단 */
	/* find poll group with least number of hwqp's assigned to it */
	TAILQ_FOREACH(fgroup, &g_nvmf_fgroups, link) {                  /* [한국어] 모든 poll group을 순회하며 최소 부하 탐색 */
		if (fgroup->hwqp_count < max_count) {                   /* [한국어] 더 적은 HWQP를 가진 그룹을 발견하면 후보 갱신 */
			ret_fgroup = fgroup;
			max_count = fgroup->hwqp_count;
		}
	}

	if (ret_fgroup) {                                              /* [한국어] 후보가 있으면 배정 확정 */
		ret_fgroup->hwqp_count++;                              /* [한국어] 선택된 그룹의 HWQP 카운트 증가(부하 반영) */
		hwqp->thread = ret_fgroup->group.group->thread;        /* [한국어] HWQP가 폴링될 reactor 스레드를 그룹 스레드로 고정(affinity) */
		hwqp->fgroup = ret_fgroup;                             /* [한국어] HWQP→poll group 역참조 설정 */
	}

	pthread_mutex_unlock(&g_nvmf_ftransport->lock);               /* [한국어] 전역 리스트 락 해제 */

	return ret_fgroup;                                            /* [한국어] 배정된 그룹(또는 NULL) 반환 */
}

/*
 * [한국어]
 * nvmf_fc_poll_group_valid - 주어진 poll group이 아직 전역 리스트에 살아있는지 검사.
 *
 * @fgroup: 유효성 확인할 poll group 포인터.
 * @return: true=현재 등록된 유효 그룹, false=이미 소멸됨.
 *
 * 비동기 콜백이 도착했을 때 그 사이 poll group이 파기되었을 수 있으므로,
 * 포인터를 역참조하기 전에 전역 리스트에서 존재 여부를 확인하는 안전장치다.
 * 실행 컨텍스트: 다양한 스레드. pthread_mutex로 전역 리스트 보호.
 *
 * 호출 체인:
 *   비동기 콜백 → [nvmf_fc_poll_group_valid] → TAILQ_FOREACH
 */
bool
nvmf_fc_poll_group_valid(struct spdk_nvmf_fc_poll_group *fgroup)
{
	struct spdk_nvmf_fc_poll_group *tmp;                            /* [한국어] 순회 커서 */
	bool rc = false;                                               /* [한국어] 발견 여부(기본 false) */

	pthread_mutex_lock(&g_nvmf_ftransport->lock);                  /* [한국어] 전역 리스트 보호 */
	TAILQ_FOREACH(tmp, &g_nvmf_fgroups, link) {                    /* [한국어] 전역 리스트 순회 */
		if (tmp == fgroup) {                                   /* [한국어] 포인터 동일성으로 존재 확인 */
			rc = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_nvmf_ftransport->lock);               /* [한국어] 락 해제 */
	return rc;                                                    /* [한국어] 유효 여부 반환 */
}

/*
 * [한국어]
 * nvmf_fc_poll_group_add_hwqp - HWQP를 poll group에 배정하고 폴링을 시작하도록 등록.
 *
 * @hwqp: 추가할 HWQP.
 * @return: 없음.
 *
 * port가 온라인되면 그 HWQP들을 reactor 스레드 풀에 분산 배정해야 polled-mode로
 * I/O를 처리할 수 있다. 가장 한가한 그룹을 고른 뒤, poller API(ADD_HWQP)를 통해
 * 해당 스레드에 비동기로 등록을 요청한다.
 * 실행 컨텍스트: main_thread.
 *
 * 호출 체인:
 *   port online 경로 → [nvmf_fc_poll_group_add_hwqp]
 *     → nvmf_fc_assign_idlest_poll_group / nvmf_fc_poller_api_func(ADD_HWQP)
 */
void
nvmf_fc_poll_group_add_hwqp(struct spdk_nvmf_fc_hwqp *hwqp)
{
	assert(hwqp);                                                 /* [한국어] 디버그 빌드에서 NULL 입력 조기 검출 */
	if (hwqp == NULL) {                                          /* [한국어] 릴리스 빌드 안전망 - NULL이면 조용히 반환 */
		SPDK_ERRLOG("Error: hwqp is NULL\n");
		return;
	}

	assert(g_nvmf_fgroup_count);                                 /* [한국어] poll group이 최소 1개는 있어야 배정 가능 */

	if (!nvmf_fc_assign_idlest_poll_group(hwqp)) {              /* [한국어] 가장 한가한 그룹 배정 시도 - 실패 시 등록 중단 */
		SPDK_ERRLOG("Could not assign poll group for hwqp (%d)\n", hwqp->hwqp_id);
		return;
	}

	nvmf_fc_poller_api_func(hwqp, SPDK_NVMF_FC_POLLER_API_ADD_HWQP, NULL);  /* [한국어] 배정된 그룹 스레드에 ADD_HWQP 메시지 전달 - 그 스레드가 폴링 목록에 추가 */
}

/*
 * [한국어]
 * nvmf_fc_poll_group_remove_hwqp_cb - HWQP 제거 poller API의 완료 콜백.
 *
 * @cb_data: remove_hwqp_args 컨텍스트.
 * @ret: poller API 결과 코드.
 * @return: 없음.
 *
 * poll group 스레드에서 HWQP가 폴링 목록에서 제거 완료되면 호출되어, 결과를 로깅하고
 * 상위 호출자의 콜백을 호출한 뒤 args를 해제한다.
 * 실행 컨텍스트: 요청을 보낸 스레드(args->cb_info.cb_thread)에서 실행되도록 디스패치됨.
 *
 * 호출 체인:
 *   poller API 완료 → [nvmf_fc_poll_group_remove_hwqp_cb] → args->cb_fn / free
 */
static void
nvmf_fc_poll_group_remove_hwqp_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct spdk_nvmf_fc_poller_api_remove_hwqp_args *args = cb_data;  /* [한국어] 불투명 cb_data를 remove_hwqp_args로 복원 */

	if (ret == SPDK_NVMF_FC_POLLER_API_SUCCESS) {                  /* [한국어] 제거 성공/실패를 디버그 로그로 구분 기록 */
		SPDK_DEBUGLOG(nvmf_fc_adm_api,
			      "Remove hwqp%d from fgroup success\n", args->hwqp->hwqp_id);
	} else {
		SPDK_ERRLOG("Remove hwqp%d from fgroup failed.\n", args->hwqp->hwqp_id);
	}

	if (args->cb_fn) {                                            /* [한국어] 상위 호출자가 등록한 완료 콜백이 있으면 호출 */
		args->cb_fn(args->cb_ctx, 0);                        /* [한국어] rc=0 - 콜백 단계에서는 제거 자체는 끝난 것으로 통지 */
	}

	free(args);                                                  /* [한국어] 비동기 인자 컨텍스트 해제 */
}

/*
 * [한국어]
 * nvmf_fc_poll_group_remove_hwqp - HWQP를 polling 중인 poll group에서 제거.
 *
 * @hwqp: 제거할 HWQP.
 * @cb_fn: 제거 완료 콜백(NULL 가능).
 * @cb_ctx: 콜백 컨텍스트.
 * @return: 없음(결과는 cb_fn으로 비동기 통지).
 *
 * port가 오프라인되거나 제거될 때 HWQP를 reactor 폴링 목록에서 빼야 한다. 전역 리스트에서
 * 부하 카운트를 줄인 뒤, poller API(REMOVE_HWQP)로 해당 스레드에 제거를 요청한다. 그 사이
 * 이미 그룹이 사라졌으면(tmp != fgroup) 작업을 건너뛴다.
 * 실행 컨텍스트: main_thread(또는 port 제거 경로).
 *
 * 호출 체인:
 *   port offline → [nvmf_fc_poll_group_remove_hwqp]
 *     → nvmf_fc_poller_api_func(REMOVE_HWQP) → nvmf_fc_poll_group_remove_hwqp_cb
 */
void
nvmf_fc_poll_group_remove_hwqp(struct spdk_nvmf_fc_hwqp *hwqp,
			       spdk_nvmf_fc_remove_hwqp_cb cb_fn, void *cb_ctx)
{
	struct spdk_nvmf_fc_poller_api_remove_hwqp_args *args;        /* [한국어] poller API에 넘길 비동기 인자 */
	struct spdk_nvmf_fc_poll_group *tmp;                         /* [한국어] 전역 리스트 순회 커서 겸 존재 확인용 */
	int rc = 0;                                                  /* [한국어] 에러 코드 누적(콜백으로 전달) */

	assert(hwqp);                                               /* [한국어] NULL 입력 조기 검출 */

	SPDK_DEBUGLOG(nvmf_fc,
		      "Remove hwqp from poller: for port: %d, hwqp: %d\n",
		      hwqp->fc_port->port_hdl, hwqp->hwqp_id);

	if (!hwqp->fgroup) {                                        /* [한국어] 애초에 그룹에 배정된 적 없으면 할 일 없음(에러 로깅 후 done) */
		SPDK_ERRLOG("HWQP (%d) not assigned to poll group\n", hwqp->hwqp_id);
	} else {
		pthread_mutex_lock(&g_nvmf_ftransport->lock);       /* [한국어] 전역 리스트/카운트 보호 */
		TAILQ_FOREACH(tmp, &g_nvmf_fgroups, link) {         /* [한국어] HWQP가 매달린 그룹이 아직 살아있는지 확인 */
			if (tmp == hwqp->fgroup) {
				hwqp->fgroup->hwqp_count--;         /* [한국어] 그룹 부하 카운트 감소 */
				break;
			}
		}
		pthread_mutex_unlock(&g_nvmf_ftransport->lock);     /* [한국어] 락 해제 */

		if (tmp != hwqp->fgroup) {                          /* [한국어] 순회 중 일치 못 찾음 = 그룹이 이미 제거됨 */
			/* Pollgroup was already removed. Dont bother. */
			goto done;
		}

		args = calloc(1, sizeof(struct spdk_nvmf_fc_poller_api_remove_hwqp_args));  /* [한국어] 비동기 인자 할당 */
		if (args == NULL) {                                 /* [한국어] 할당 실패 시 -ENOMEM으로 done */
			rc = -ENOMEM;
			SPDK_ERRLOG("Failed to allocate memory for poller remove hwqp:%d\n", hwqp->hwqp_id);
			goto done;
		}

		args->hwqp   = hwqp;                                /* [한국어] 제거 대상 HWQP */
		args->cb_fn  = cb_fn;                               /* [한국어] 상위 완료 콜백 보존 */
		args->cb_ctx = cb_ctx;                              /* [한국어] 상위 콜백 컨텍스트 보존 */
		args->cb_info.cb_func = nvmf_fc_poll_group_remove_hwqp_cb;  /* [한국어] poller API 내부 완료 콜백 지정 */
		args->cb_info.cb_data = args;                       /* [한국어] 콜백에 args 자신을 전달 */
		args->cb_info.cb_thread = spdk_get_thread();        /* [한국어] 콜백을 이 스레드로 되돌려 실행하도록 기록 */

		rc = nvmf_fc_poller_api_func(hwqp, SPDK_NVMF_FC_POLLER_API_REMOVE_HWQP, args);  /* [한국어] poll group 스레드에 REMOVE_HWQP 비동기 요청 */
		if (rc) {                                           /* [한국어] 요청 자체 실패 시 args 정리 후 done */
			rc = -EINVAL;
			SPDK_ERRLOG("Remove hwqp%d from fgroup failed.\n", hwqp->hwqp_id);
			free(args);
			goto done;
		}
		return;                                             /* [한국어] 정상 요청됨 - 완료는 콜백에서 통지(여기서 cb_fn 직접 호출 안 함) */
	}
done:
	if (cb_fn) {                                                /* [한국어] 비동기 요청 전에 종료된 경로 - 즉시 콜백 호출로 결과 통지 */
		cb_fn(cb_ctx, rc);
	}
}

/*
 * Note: This needs to be used only on main poller.
 */
/*
 * [한국어]
 * nvmf_fc_get_abts_unique_id - ABTS(Abort Sequence) 처리에 쓸 단조 증가 고유 ID 생성.
 *
 * @return: 1부터 증가하는 64비트 고유 ID.
 *
 * 호스트가 보낸 ABTS는 여러 HWQP에 흩어진 같은 exchange의 in-flight를 모두 취소해야 하는데,
 * 그 fan-out/fan-in을 식별하기 위해 작업마다 유일 ID를 부여한다. static 카운터를 증가시키므로
 * 반드시 단일 스레드(main poller)에서만 호출해야 race가 없다(원문 주석 명시).
 * 실행 컨텍스트: main_thread 전용(lock-free 가정).
 *
 * 호출 체인:
 *   ABTS 수신 처리 → [nvmf_fc_get_abts_unique_id]
 */
static uint64_t
nvmf_fc_get_abts_unique_id(void)
{
	static uint32_t u_id = 0;                                   /* [한국어] 함수 정적 카운터 - 호출 간 값 유지(main_thread 단일 접근이라 락 불필요) */

	return (uint64_t)(++u_id);                                  /* [한국어] 선증가 후 반환 - 1부터 시작하는 유일 ID */
}

/*
 * [한국어]
 * nvmf_fc_queue_synced_cb - 모든 HWQP의 queue sync 완료 시 ABTS를 재발행하는 콜백.
 *
 * @cb_data: spdk_nvmf_fc_abts_ctx 컨텍스트.
 * @ret: queue sync poller API 결과.
 * @return: 없음.
 *
 * ABTS 대상 exchange를 처음 못 찾았을 때(notfound), 각 HWQP에 marker를 넣어 큐를
 * 동기화시킨다. 모든 HWQP가 sync 완료를 보고하면(fan-in), 이제 marker 이후 도착한
 * 명령까지 큐에 반영됐으므로 ABTS를 한 번 더 모든 HWQP에 보낸다(재시도).
 * 실행 컨텍스트: 콜백을 보낸 스레드(보통 main_thread). hwqps_responded 카운트다운으로 fan-in.
 *
 * 호출 체인:
 *   QUEUE_SYNC poller 완료 → [nvmf_fc_queue_synced_cb] → nvmf_fc_poller_api_func(ABTS_RECEIVED)
 */
static void
nvmf_fc_queue_synced_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct spdk_nvmf_fc_abts_ctx *ctx = cb_data;                       /* [한국어] ABTS 처리 전체 컨텍스트 복원 */
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *args, *poller_arg; /* [한국어] HWQP별 ABTS 재발행 인자 배열/커서 */

	ctx->hwqps_responded++;                                            /* [한국어] 이 HWQP의 sync 완료를 카운트(fan-in 누적) */

	if (ctx->hwqps_responded < ctx->num_hwqps) {                       /* [한국어] 아직 모든 HWQP가 응답 안 했으면 대기 */
		/* Wait for all pollers to complete. */
		return;
	}

	/* Free the queue sync poller args. */
	free(ctx->sync_poller_args);                                       /* [한국어] sync 단계 인자 배열 해제(역할 끝남) */

	/* Mark as queue synced */
	ctx->queue_synced = true;                                          /* [한국어] 이미 한 번 sync했음을 표시 - 무한 재시도 방지 */

	/* Reset the ctx values */
	ctx->hwqps_responded = 0;                                          /* [한국어] 다음 fan-in(ABTS 재발행)을 위해 카운터 리셋 */
	ctx->handled = false;                                              /* [한국어] 재시도 결과를 새로 집계하기 위해 초기화 */

	SPDK_DEBUGLOG(nvmf_fc,
		      "QueueSync(0x%lx) completed for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		      ctx->u_id, ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);

	/* Resend ABTS to pollers */
	args = ctx->abts_poller_args;                                      /* [한국어] HWQP별 ABTS 인자 배열 시작 */
	for (int i = 0; i < ctx->num_hwqps; i++) {                         /* [한국어] 관련된 모든 HWQP에 ABTS를 다시 발행 */
		poller_arg = args + i;                                     /* [한국어] i번째 HWQP 인자 */
		nvmf_fc_poller_api_func(poller_arg->hwqp,                  /* [한국어] 해당 HWQP 스레드에 ABTS_RECEIVED 재요청 */
					SPDK_NVMF_FC_POLLER_API_ABTS_RECEIVED,
					poller_arg);
	}
}

/*
 * [한국어]
 * nvmf_fc_handle_abts_notfound - ABTS 대상 exchange를 못 찾았을 때 queue sync로 재시도 준비.
 *
 * @ctx: ABTS 처리 컨텍스트.
 * @return: 0=sync 발행 성공, -EPERM=드라이버 미지원, -EINVAL/-ENOMEM=오류.
 *
 * ABTS가 가리키는 OXID가 어느 HWQP에서도 안 보이면, target이 아직 그 명령의 RQE를
 * 처리하지 못한 race일 수 있다. 각 HWQP에 sync marker를 넣어 큐를 한 시점까지 밀어낸 뒤
 * (nvmf_fc_issue_q_sync), 동기화 완료 콜백에서 ABTS를 재발행하게 한다.
 * 실행 컨텍스트: main_thread.
 *
 * 호출 체인:
 *   nvmf_fc_abts_handled_cb(notfound) → [nvmf_fc_handle_abts_notfound]
 *     → nvmf_fc_poller_api_func(QUEUE_SYNC) / nvmf_fc_issue_q_sync
 */
static int
nvmf_fc_handle_abts_notfound(struct spdk_nvmf_fc_abts_ctx *ctx)
{
	struct spdk_nvmf_fc_poller_api_queue_sync_args *args, *poller_arg;        /* [한국어] HWQP별 sync 인자 배열/커서 */
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *abts_args, *abts_poller_arg;  /* [한국어] 기존 ABTS 인자(HWQP 목록 재사용용) */

	/* check if FC driver supports queue sync */
	if (!nvmf_fc_q_sync_available()) {                                 /* [한국어] LLD가 queue sync를 지원하지 않으면 이 복구 불가 */
		return -EPERM;
	}

	assert(ctx);                                                       /* [한국어] NULL 컨텍스트는 프로그래밍 오류 */
	if (!ctx) {                                                        /* [한국어] 릴리스 안전망 */
		SPDK_ERRLOG("NULL ctx pointer");
		return -EINVAL;
	}

	/* Reset the ctx values */
	ctx->hwqps_responded = 0;                                          /* [한국어] sync 단계 fan-in 카운터 초기화 */

	args = calloc(ctx->num_hwqps,                                      /* [한국어] HWQP 수만큼 sync 인자 배열 할당 */
		      sizeof(struct spdk_nvmf_fc_poller_api_queue_sync_args));
	if (!args) {                                                       /* [한국어] 할당 실패 시 ENOMEM */
		SPDK_ERRLOG("QueueSync(0x%lx) failed for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
			    ctx->u_id, ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);
		return -ENOMEM;
	}
	ctx->sync_poller_args = args;                                      /* [한국어] 컨텍스트에 sync 인자 배열 보관(콜백에서 free) */

	abts_args = ctx->abts_poller_args;                                /* [한국어] 기존 ABTS 인자에서 HWQP 목록 재사용 */
	for (int i = 0; i < ctx->num_hwqps; i++) {                        /* [한국어] 관련 HWQP마다 sync 메시지 구성/발행 */
		abts_poller_arg = abts_args + i;                          /* [한국어] i번째 ABTS 인자(HWQP 출처) */
		poller_arg = args + i;                                    /* [한국어] i번째 sync 인자 */
		poller_arg->u_id = ctx->u_id;                            /* [한국어] 이 ABTS 작업의 고유 ID 전파 - marker 매칭용 */
		poller_arg->hwqp = abts_poller_arg->hwqp;                /* [한국어] 대상 HWQP 복사 */
		poller_arg->cb_info.cb_func = nvmf_fc_queue_synced_cb;   /* [한국어] sync 완료 콜백 지정 */
		poller_arg->cb_info.cb_data = ctx;                       /* [한국어] 콜백에 동일 ctx 전달 */
		poller_arg->cb_info.cb_thread = spdk_get_thread();       /* [한국어] 콜백을 이 스레드로 되돌림 */

		/* Send a Queue sync message to interested pollers */
		nvmf_fc_poller_api_func(poller_arg->hwqp,                /* [한국어] HWQP 스레드에 QUEUE_SYNC 발행 */
					SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC,
					poller_arg);
	}

	SPDK_DEBUGLOG(nvmf_fc,
		      "QueueSync(0x%lx) Sent for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		      ctx->u_id, ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);

	/* Post Marker to queue to track aborted request */
	nvmf_fc_issue_q_sync(ctx->ls_hwqp, ctx->u_id, ctx->fcp_rq_id);   /* [한국어] LS HWQP에 marker frame을 게시 - 큐가 이 지점까지 처리되면 sync 완료 통지 */

	return 0;                                                        /* [한국어] sync 발행 성공 - 완료는 콜백에서 진행 */
}

/*
 * [한국어]
 * nvmf_fc_abts_handled_cb - 모든 HWQP의 ABTS 처리 결과를 모아 BLS 응답(ACC/REJ)을 송신.
 *
 * @cb_data: spdk_nvmf_fc_abts_ctx 컨텍스트.
 * @ret: HWQP의 ABTS 처리 결과(OXID_NOT_FOUND 등).
 * @return: 없음.
 *
 * ABTS는 여러 HWQP에 fan-out되어 각자 해당 exchange를 abort 시도한다. 한 곳이라도
 * 처리했으면(handled=true) BLS Accept를, 아무도 못 찾았으면 queue sync 재시도 후
 * 그래도 없으면 BLS Reject를 보낸다. 그 사이 nport가 삭제됐을 수 있어 재확인한다.
 * 실행 컨텍스트: 콜백 스레드(main_thread). hwqps_responded fan-in.
 *
 * 호출 체인:
 *   ABTS_RECEIVED poller 완료 → [nvmf_fc_abts_handled_cb]
 *     → nvmf_fc_handle_abts_notfound(재시도) / nvmf_fc_xmt_bls_rsp(ACC/REJ)
 */
static void
nvmf_fc_abts_handled_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct spdk_nvmf_fc_abts_ctx *ctx = cb_data;                      /* [한국어] ABTS 처리 컨텍스트 복원 */
	struct spdk_nvmf_fc_nport *nport  = NULL;                         /* [한국어] 현재 시점의 nport 재조회 결과 */

	if (ret != SPDK_NVMF_FC_POLLER_API_OXID_NOT_FOUND) {              /* [한국어] not-found가 아니면 = 이 HWQP가 exchange를 찾아 처리함 */
		ctx->handled = true;                                     /* [한국어] 최소 한 HWQP가 처리했음을 마킹(→ Accept) */
	}

	ctx->hwqps_responded++;                                          /* [한국어] 이 HWQP 응답 카운트(fan-in 누적) */

	if (ctx->hwqps_responded < ctx->num_hwqps) {                     /* [한국어] 모든 HWQP 응답 전까지 대기 */
		/* Wait for all pollers to complete. */
		return;
	}

	nport = nvmf_fc_nport_find(ctx->port_hdl, ctx->nport_hdl);       /* [한국어] 처리 도중 nport가 삭제됐는지 핸들로 재조회 */

	if (ctx->nport != nport) {                                       /* [한국어] 저장해둔 nport와 현재 조회 결과가 다르면 그 사이 삭제/교체됨 */
		/* Nport can be deleted while this abort is being
		 * processed by the pollers.
		 */
		SPDK_NOTICELOG("nport_%d deleted while processing ABTS frame, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
			       ctx->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);  /* [한국어] 응답을 보낼 대상이 사라졌으므로 로그만 남기고 무응답 */
	} else {
		if (!ctx->handled) {                                    /* [한국어] 아무 HWQP도 exchange를 못 찾은 경우 */
			/* Try syncing the queues and try one more time */
			if (!ctx->queue_synced && (nvmf_fc_handle_abts_notfound(ctx) == 0)) {  /* [한국어] 아직 sync 안 했고 sync 발행 성공 시 - 재시도 진행, 응답 보류 */
				SPDK_DEBUGLOG(nvmf_fc,
					      "QueueSync(0x%lx) for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
					      ctx->u_id, ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);
				return;                                 /* [한국어] sync 완료 콜백에서 다시 이 함수로 돌아옴 */
			} else {
				/* Send Reject */
				nvmf_fc_xmt_bls_rsp(&ctx->nport->fc_port->ls_queue,  /* [한국어] 재시도해도 못 찾음 → BLS Reject 송신 */
						    ctx->oxid, ctx->rxid, ctx->rpi, true,  /* [한국어] rjt=true, OXID/RXID/RPI로 거절 대상 식별 */
						    FCNVME_BLS_REJECT_EXP_INVALID_OXID, NULL, NULL);  /* [한국어] 거절 사유: Invalid OXID */
			}
		} else {
			/* Send Accept */
			nvmf_fc_xmt_bls_rsp(&ctx->nport->fc_port->ls_queue,  /* [한국어] 처리 성공 → BLS Accept 송신 */
					    ctx->oxid, ctx->rxid, ctx->rpi, false,  /* [한국어] rjt=false(수락), reason=0 */
					    0, NULL, NULL);
		}
	}
	SPDK_NOTICELOG("BLS_%s sent for ABTS frame nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		       (ctx->handled) ? "ACC" : "REJ", ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);

	free(ctx->abts_poller_args);                                    /* [한국어] HWQP별 ABTS 인자 배열 해제 */
	free(ctx);                                                      /* [한국어] ABTS 컨텍스트 해제 - 처리 종료 */
}

/*
 * [한국어]
 * nvmf_fc_handle_abts_frame - 호스트로부터 받은 ABTS(Abort Sequence) frame을 처리하는 진입점.
 *
 * @nport: ABTS가 향한 Virtual N_Port.
 * @rpi: Remote Port Index(상대 호스트 식별).
 * @oxid: Originator Exchange ID(호스트가 부여한 exchange 번호).
 * @rxid: Responder Exchange ID(target이 부여한 exchange 번호).
 * @return: 없음(BLS 응답은 내부에서 송신).
 *
 * ABTS는 진행 중인 FC exchange(=NVMe 명령)를 호스트가 강제 취소하는 요청이다. 해당 RPI로
 * 활성 connection을 가진 HWQP들을 모두 모아(중복 제거) 각 HWQP에 ABTS_RECEIVED를 fan-out하고,
 * 모든 응답이 모이면 nvmf_fc_abts_handled_cb에서 BLS ACC/REJ로 회신한다. 관련 HWQP가
 * 없으면 즉시 BLS Reject를 보낸다.
 * 실행 컨텍스트: main_thread(LS 큐 폴링에서 ABTS 발견 시).
 *
 * 호출 체인:
 *   LS 큐 폴링(ABTS 감지) → [nvmf_fc_handle_abts_frame]
 *     → nvmf_fc_poller_api_func(ABTS_RECEIVED) → nvmf_fc_abts_handled_cb
 */
void
nvmf_fc_handle_abts_frame(struct spdk_nvmf_fc_nport *nport, uint16_t rpi,
			  uint16_t oxid, uint16_t rxid)
{
	struct spdk_nvmf_fc_abts_ctx *ctx = NULL;                          /* [한국어] ABTS 처리 전체 컨텍스트(fan-in 추적) */
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *args = NULL, *poller_arg;  /* [한국어] HWQP별 ABTS 인자 배열/커서 */
	struct spdk_nvmf_fc_association *assoc = NULL;                     /* [한국어] association 순회 커서 */
	struct spdk_nvmf_fc_conn *conn = NULL;                             /* [한국어] connection 순회 커서 */
	uint32_t hwqp_cnt = 0;                                             /* [한국어] 중복 제거된 관련 HWQP 개수 */
	bool skip_hwqp_cnt;                                                /* [한국어] 현재 HWQP가 이미 목록에 있는지 표시 */
	struct spdk_nvmf_fc_hwqp **hwqps = NULL;                           /* [한국어] 관련 HWQP 포인터 임시 배열 */
	uint32_t i;                                                        /* [한국어] 루프 인덱스 */

	SPDK_NOTICELOG("Handle ABTS frame for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		       nport->nport_hdl, rpi, oxid, rxid);

	/* Allocate memory to track hwqp's with at least 1 active connection. */
	hwqps = calloc(nport->fc_port->num_io_queues, sizeof(struct spdk_nvmf_fc_hwqp *));  /* [한국어] 최대 io_queue 수만큼 HWQP 포인터 배열 확보 */
	if (hwqps == NULL) {                                              /* [한국어] 할당 실패 시 Reject 경로로 */
		SPDK_ERRLOG("Unable to allocate temp. hwqp array for abts processing!\n");
		goto bls_rej;
	}

	TAILQ_FOREACH(assoc, &nport->fc_associations, link) {            /* [한국어] nport의 모든 association 순회 */
		TAILQ_FOREACH(conn, &assoc->fc_conns, assoc_link) {     /* [한국어] association의 모든 connection 순회 */
			if ((conn->rpi != rpi) || !conn->hwqp) {        /* [한국어] ABTS 대상 RPI가 아니거나 HWQP 미바인딩이면 건너뜀 */
				continue;
			}

			skip_hwqp_cnt = false;                          /* [한국어] 중복 검사 플래그 초기화 */
			for (i = 0; i < hwqp_cnt; i++) {                /* [한국어] 이미 수집한 HWQP들과 비교 */
				if (hwqps[i] == conn->hwqp) {           /* [한국어] 동일 HWQP면 중복 - 추가 안 함 */
					/* Skip. This is already present */
					skip_hwqp_cnt = true;
					break;
				}
			}
			if (!skip_hwqp_cnt) {                           /* [한국어] 새 HWQP면 목록에 추가 */
				assert(hwqp_cnt < nport->fc_port->num_io_queues);  /* [한국어] 배열 범위 초과 방지(논리상 보장) */
				hwqps[hwqp_cnt] = conn->hwqp;          /* [한국어] HWQP 포인터 저장 */
				hwqp_cnt++;                            /* [한국어] 수집 개수 증가 */
			}
		}
	}

	if (!hwqp_cnt) {                                                 /* [한국어] 관련 HWQP가 하나도 없으면 abort 대상 없음 → Reject */
		goto bls_rej;
	}

	args = calloc(hwqp_cnt,                                          /* [한국어] 수집된 HWQP 수만큼 ABTS 인자 배열 할당 */
		      sizeof(struct spdk_nvmf_fc_poller_api_abts_recvd_args));
	if (!args) {                                                    /* [한국어] 할당 실패 시 Reject */
		goto bls_rej;
	}

	ctx = calloc(1, sizeof(struct spdk_nvmf_fc_abts_ctx));          /* [한국어] ABTS 처리 컨텍스트 할당 */
	if (!ctx) {                                                     /* [한국어] 실패 시 Reject(args는 bls_rej에서 free) */
		goto bls_rej;
	}
	ctx->rpi = rpi;                                                 /* [한국어] 응답 시 사용할 RPI 보관 */
	ctx->oxid = oxid;                                              /* [한국어] OXID 보관(BLS 응답 식별) */
	ctx->rxid = rxid;                                              /* [한국어] RXID 보관 */
	ctx->nport = nport;                                            /* [한국어] 대상 nport 포인터(나중에 삭제 여부 재확인) */
	ctx->nport_hdl = nport->nport_hdl;                            /* [한국어] nport 핸들(재조회 키) */
	ctx->port_hdl = nport->fc_port->port_hdl;                    /* [한국어] 물리 port 핸들(재조회 키) */
	ctx->num_hwqps = hwqp_cnt;                                    /* [한국어] fan-in 대상 HWQP 수 */
	ctx->ls_hwqp = &nport->fc_port->ls_queue;                    /* [한국어] queue sync marker를 게시할 LS 큐 */
	ctx->fcp_rq_id = nport->fc_port->fcp_rq_id;                  /* [한국어] FCP RQ id(marker 게시에 필요) */
	ctx->abts_poller_args = args;                                /* [한국어] HWQP별 인자 배열 보관(콜백에서 free) */

	/* Get a unique context for this ABTS */
	ctx->u_id = nvmf_fc_get_abts_unique_id();                    /* [한국어] 이 ABTS 작업의 고유 ID 발급(queue sync marker 매칭) */

	for (i = 0; i < hwqp_cnt; i++) {                             /* [한국어] 수집된 각 HWQP에 ABTS_RECEIVED를 fan-out */
		poller_arg = args + i;                              /* [한국어] i번째 인자 */
		poller_arg->hwqp = hwqps[i];                        /* [한국어] 대상 HWQP */
		poller_arg->cb_info.cb_func = nvmf_fc_abts_handled_cb;  /* [한국어] 완료 콜백 - 모든 HWQP 응답 후 BLS 결정 */
		poller_arg->cb_info.cb_data = ctx;                  /* [한국어] 공유 컨텍스트 */
		poller_arg->cb_info.cb_thread = spdk_get_thread();  /* [한국어] 콜백을 이 스레드로 되돌림 */
		poller_arg->ctx = ctx;                              /* [한국어] poller가 직접 참조할 컨텍스트 */

		nvmf_fc_poller_api_func(poller_arg->hwqp,           /* [한국어] HWQP 스레드에 ABTS_RECEIVED 발행 */
					SPDK_NVMF_FC_POLLER_API_ABTS_RECEIVED,
					poller_arg);
	}

	free(hwqps);                                                /* [한국어] 임시 HWQP 배열 해제(args/ctx로 필요한 건 복사됨) */

	return;                                                     /* [한국어] 정상 fan-out 완료 - 응답은 콜백에서 */
bls_rej:
	free(args);                                                /* [한국어] 부분 할당 정리(NULL이면 무해) */
	free(hwqps);

	/* Send Reject */
	nvmf_fc_xmt_bls_rsp(&nport->fc_port->ls_queue, oxid, rxid, rpi,  /* [한국어] 처리 불가 → BLS Reject 송신 */
			    true, FCNVME_BLS_REJECT_EXP_NOINFO, NULL, NULL);  /* [한국어] rjt=true, 사유=NoInfo */
	SPDK_NOTICELOG("BLS_RJT for ABTS frame for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		       nport->nport_hdl, rpi, oxid, rxid);
	return;
}

/*** Accessor functions for the FC structures - BEGIN */
/*
 * Returns true if the port is in offline state.
 */
/*
 * [한국어]
 * nvmf_fc_port_is_offline - FC port가 OFFLINE 상태인지 확인.
 * @fc_port: 대상 port(NULL 허용). @return: OFFLINE이면 true.
 * port 상태는 link down/admin 명령으로 바뀌며, I/O 발행 전 상태 게이팅에 쓰인다.
 * 실행 컨텍스트: main_thread. 호출 체인: 관리/I/O 게이팅 코드 → [nvmf_fc_port_is_offline]
 */
bool
nvmf_fc_port_is_offline(struct spdk_nvmf_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status == SPDK_FC_PORT_OFFLINE)) {  /* [한국어] port 존재 && 상태가 OFFLINE이면 참 */
		return true;
	}

	return false;                                                   /* [한국어] NULL이거나 OFFLINE이 아니면 거짓 */
}

/*
 * Returns true if the port is in online state.
 */
/*
 * [한국어]
 * nvmf_fc_port_is_online - FC port가 ONLINE 상태인지 확인.
 * @fc_port: 대상 port(NULL 허용). @return: ONLINE이면 true.
 * 실행 컨텍스트: main_thread. 호출 체인: 관리/I/O 게이팅 → [nvmf_fc_port_is_online]
 */
bool
nvmf_fc_port_is_online(struct spdk_nvmf_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status == SPDK_FC_PORT_ONLINE)) {  /* [한국어] port 존재 && ONLINE이면 참 */
		return true;
	}

	return false;                                                   /* [한국어] 그 외 거짓 */
}

/*
 * [한국어]
 * nvmf_fc_port_set_online - FC port 상태를 ONLINE으로 전이.
 * @fc_port: 대상 port. @return: 0=전이 성공, -EPERM=이미 ONLINE이거나 NULL.
 * 이미 ONLINE이면 멱등성 위반으로 -EPERM을 돌려 중복 online을 막는다.
 * 실행 컨텍스트: main_thread(port online adm). 호출 체인: port online → [nvmf_fc_port_set_online]
 */
int
nvmf_fc_port_set_online(struct spdk_nvmf_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status != SPDK_FC_PORT_ONLINE)) {  /* [한국어] 존재 && 아직 ONLINE 아님일 때만 전이 */
		fc_port->hw_port_status = SPDK_FC_PORT_ONLINE;          /* [한국어] 상태를 ONLINE으로 설정 */
		return 0;
	}

	return -EPERM;                                                  /* [한국어] NULL이거나 이미 ONLINE - 작업 거부 */
}

/*
 * [한국어]
 * nvmf_fc_port_set_offline - FC port 상태를 OFFLINE으로 전이.
 * @fc_port: 대상 port. @return: 0=전이 성공, -EPERM=이미 OFFLINE이거나 NULL.
 * 실행 컨텍스트: main_thread(port offline adm). 호출 체인: port offline → [nvmf_fc_port_set_offline]
 */
int
nvmf_fc_port_set_offline(struct spdk_nvmf_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status != SPDK_FC_PORT_OFFLINE)) {  /* [한국어] 존재 && 아직 OFFLINE 아님일 때만 전이 */
		fc_port->hw_port_status = SPDK_FC_PORT_OFFLINE;        /* [한국어] 상태를 OFFLINE으로 설정 */
		return 0;
	}

	return -EPERM;                                                  /* [한국어] NULL이거나 이미 OFFLINE - 거부 */
}

/*
 * [한국어]
 * nvmf_fc_hwqp_set_online - HWQP를 ONLINE으로 전이하고 LLD에 통지.
 * @hwqp: 대상 HWQP. @return: LLD 결과(0=성공), -EPERM=이미 ONLINE/NULL.
 * HWQP ONLINE 시 connection 카운터를 리셋하고 LLD에 큐를 활성화하라고 알린다.
 * 실행 컨텍스트: main_thread. 호출 체인: port online → [nvmf_fc_hwqp_set_online] → nvmf_fc_set_q_online_state
 */
int
nvmf_fc_hwqp_set_online(struct spdk_nvmf_fc_hwqp *hwqp)
{
	if (hwqp && (hwqp->state != SPDK_FC_HWQP_ONLINE)) {            /* [한국어] 존재 && 아직 ONLINE 아님일 때만 */
		hwqp->state = SPDK_FC_HWQP_ONLINE;                    /* [한국어] HWQP 상태를 ONLINE으로 */
		/* reset some queue counters */
		hwqp->num_conns = 0;                                 /* [한국어] online 시작 시점의 연결 수 카운터 초기화 */
		return nvmf_fc_set_q_online_state(hwqp, true);       /* [한국어] LLD에 큐 online 통지 - 하드웨어가 RQ 수신 시작 */
	}

	return -EPERM;                                                /* [한국어] 이미 ONLINE이거나 NULL */
}

/*
 * [한국어]
 * nvmf_fc_hwqp_set_offline - HWQP를 OFFLINE으로 전이하고 LLD에 통지.
 * @hwqp: 대상 HWQP. @return: LLD 결과(0=성공), -EPERM=이미 OFFLINE/NULL.
 * 실행 컨텍스트: main_thread. 호출 체인: port offline → [nvmf_fc_hwqp_set_offline] → nvmf_fc_set_q_online_state
 */
int
nvmf_fc_hwqp_set_offline(struct spdk_nvmf_fc_hwqp *hwqp)
{
	if (hwqp && (hwqp->state != SPDK_FC_HWQP_OFFLINE)) {          /* [한국어] 존재 && 아직 OFFLINE 아님일 때만 */
		hwqp->state = SPDK_FC_HWQP_OFFLINE;                  /* [한국어] HWQP 상태를 OFFLINE으로 */
		return nvmf_fc_set_q_online_state(hwqp, false);      /* [한국어] LLD에 큐 offline 통지 - 신규 RQ 수신 중단 */
	}

	return -EPERM;                                               /* [한국어] 이미 OFFLINE이거나 NULL */
}

/*
 * [한국어]
 * nvmf_fc_port_add - 새 FC port를 전역 리스트와 LLD 목록에 등록.
 * @fc_port: 등록할 port. @return: 없음.
 * port enumerate 시 호출되어 SPDK 전역 리스트(g_spdk_nvmf_fc_port_list)에 넣고
 * LLD에도 추가하여 양측이 동일 port를 추적하게 한다.
 * 실행 컨텍스트: main_thread. 호출 체인: port create adm → [nvmf_fc_port_add] → nvmf_fc_lld_port_add
 */
void
nvmf_fc_port_add(struct spdk_nvmf_fc_port *fc_port)
{
	TAILQ_INSERT_TAIL(&g_spdk_nvmf_fc_port_list, fc_port, link);   /* [한국어] SPDK 전역 port 리스트 꼬리에 추가 */

	/*
	 * Let LLD add the port to its list.
	 */
	nvmf_fc_lld_port_add(fc_port);                                /* [한국어] LLD에도 port 등록 - 드라이버 측 추적 목록 동기화 */
}

/*
 * [한국어]
 * nvmf_fc_port_remove - FC port를 전역 리스트와 LLD 목록에서 제거.
 * @fc_port: 제거할 port. @return: 없음.
 * 실행 컨텍스트: main_thread. 호출 체인: port delete adm → [nvmf_fc_port_remove] → nvmf_fc_lld_port_remove
 */
static void
nvmf_fc_port_remove(struct spdk_nvmf_fc_port *fc_port)
{
	TAILQ_REMOVE(&g_spdk_nvmf_fc_port_list, fc_port, link);        /* [한국어] SPDK 전역 리스트에서 제거 */

	/*
	 * Let LLD remove the port from its list.
	 */
	nvmf_fc_lld_port_remove(fc_port);                            /* [한국어] LLD 목록에서도 제거 - 양측 동기화 */
}

/*
 * [한국어]
 * nvmf_fc_port_lookup - port 핸들로 FC port 객체를 조회.
 * @port_hdl: LLD가 부여한 물리 port 번호. @return: 일치하는 port 또는 NULL.
 * 실행 컨텍스트: main_thread. 호출 체인: adm/조회 코드 → [nvmf_fc_port_lookup]
 */
struct spdk_nvmf_fc_port *
nvmf_fc_port_lookup(uint8_t port_hdl)
{
	struct spdk_nvmf_fc_port *fc_port = NULL;                     /* [한국어] 순회 커서 */

	TAILQ_FOREACH(fc_port, &g_spdk_nvmf_fc_port_list, link) {     /* [한국어] 전역 port 리스트 선형 탐색 */
		if (fc_port->port_hdl == port_hdl) {                 /* [한국어] 핸들 일치 시 즉시 반환 */
			return fc_port;
		}
	}
	return NULL;                                                  /* [한국어] 없으면 NULL */
}

/*
 * [한국어]
 * nvmf_fc_get_prli_service_params - 본 target이 PRLI에서 광고할 service parameter 비트를 반환.
 * @return: DISCOVERY_SERVICE | TARGET_FUNCTION 비트 OR.
 * SPDK는 NVMe-FC target이므로 Target Function 비트를 항상 켜고, discovery controller도
 * 제공하므로 Discovery Service 비트를 함께 켜서 호스트 PRLI 응답에 넣는다.
 * 실행 컨텍스트: PRLI 처리(main_thread). 호출 체인: PRLI 핸들러 → [nvmf_fc_get_prli_service_params]
 */
uint32_t
nvmf_fc_get_prli_service_params(void)
{
	return (SPDK_NVMF_FC_DISCOVERY_SERVICE | SPDK_NVMF_FC_TARGET_FUNCTION);  /* [한국어] discovery+target 역할 비트 결합 */
}

/*
 * [한국어]
 * nvmf_fc_port_add_nport - Virtual N_Port(nport)를 물리 port의 nport 리스트에 등록.
 * @fc_port: 부모 port. @nport: 추가할 nport. @return: 0=성공, -EINVAL=port NULL.
 * Create Association 전에 nport(가상 N_Port)가 port에 매달려야 한다.
 * 실행 컨텍스트: main_thread. 호출 체인: nport create adm → [nvmf_fc_port_add_nport]
 */
int
nvmf_fc_port_add_nport(struct spdk_nvmf_fc_port *fc_port,
		       struct spdk_nvmf_fc_nport *nport)
{
	if (fc_port) {                                                /* [한국어] 부모 port가 유효할 때만 등록 */
		TAILQ_INSERT_TAIL(&fc_port->nport_list, nport, link); /* [한국어] port의 nport 리스트 꼬리에 추가 */
		fc_port->num_nports++;                               /* [한국어] nport 카운트 증가 */
		return 0;
	}

	return -EINVAL;                                              /* [한국어] port NULL - 잘못된 인자 */
}

/*
 * [한국어]
 * nvmf_fc_port_remove_nport - Virtual N_Port를 port의 nport 리스트에서 제거.
 * @fc_port: 부모 port. @nport: 제거할 nport. @return: 0=성공, -EINVAL=인자 NULL.
 * 실행 컨텍스트: main_thread. 호출 체인: nport delete adm → [nvmf_fc_port_remove_nport]
 */
int
nvmf_fc_port_remove_nport(struct spdk_nvmf_fc_port *fc_port,
			  struct spdk_nvmf_fc_nport *nport)
{
	if (fc_port && nport) {                                      /* [한국어] port/nport 모두 유효할 때만 */
		TAILQ_REMOVE(&fc_port->nport_list, nport, link);    /* [한국어] nport 리스트에서 제거 */
		fc_port->num_nports--;                              /* [한국어] nport 카운트 감소 */
		return 0;
	}

	return -EINVAL;                                             /* [한국어] 인자 NULL - 거부 */
}

/*
 * [한국어]
 * nvmf_fc_nport_hdl_lookup - 특정 port 내에서 nport 핸들로 nport를 조회(내부 헬퍼).
 * @fc_port: 검색 대상 port. @nport_hdl: nport 핸들. @return: 일치 nport 또는 NULL.
 * 실행 컨텍스트: main_thread. 호출 체인: nvmf_fc_nport_find → [nvmf_fc_nport_hdl_lookup]
 */
static struct spdk_nvmf_fc_nport *
nvmf_fc_nport_hdl_lookup(struct spdk_nvmf_fc_port *fc_port, uint16_t nport_hdl)
{
	struct spdk_nvmf_fc_nport *fc_nport = NULL;                  /* [한국어] 순회 커서 */

	TAILQ_FOREACH(fc_nport, &fc_port->nport_list, link) {       /* [한국어] port의 nport 리스트 선형 탐색 */
		if (fc_nport->nport_hdl == nport_hdl) {            /* [한국어] 핸들 일치 시 반환 */
			return fc_nport;
		}
	}

	return NULL;                                               /* [한국어] 없으면 NULL */
}

/*
 * [한국어]
 * nvmf_fc_nport_find - (port_hdl, nport_hdl) 쌍으로 nport를 전역에서 조회.
 * @port_hdl: 물리 port 핸들. @nport_hdl: nport 핸들. @return: 일치 nport 또는 NULL.
 * port를 먼저 찾고 그 안에서 nport를 찾는 2단계 조회로, 비동기 콜백이 nport 생존을
 * 재확인할 때(ABTS 등) 쓰인다.
 * 실행 컨텍스트: main_thread. 호출 체인: nvmf_fc_abts_handled_cb 등 → [nvmf_fc_nport_find]
 */
struct spdk_nvmf_fc_nport *
nvmf_fc_nport_find(uint8_t port_hdl, uint16_t nport_hdl)
{
	struct spdk_nvmf_fc_port *fc_port = NULL;                   /* [한국어] 1단계로 찾은 부모 port */

	fc_port = nvmf_fc_port_lookup(port_hdl);                   /* [한국어] port 핸들로 port 조회 */
	if (fc_port) {                                             /* [한국어] port가 있으면 그 안에서 nport 조회 */
		return nvmf_fc_nport_hdl_lookup(fc_port, nport_hdl);
	}

	return NULL;                                              /* [한국어] port 자체가 없으면 NULL */
}

/*
 * [한국어]
 * nvmf_fc_hwqp_find_nport_and_rport - 수신 frame의 D_ID/S_ID로 nport와 rport를 동시 조회.
 *
 * @hwqp: frame을 받은 HWQP.
 * @d_id: Destination ID(target 측 nport의 FC 주소).
 * @nport: [out] 찾은 nport 포인터를 저장할 위치.
 * @s_id: Source ID(호스트 rport의 FC 주소).
 * @rport: [out] 찾은 rport 포인터를 저장할 위치.
 * @return: 0=둘 다 찾음, -EINVAL=인자 NULL, -ENOENT=미발견.
 *
 * FC frame은 24비트 D_ID/S_ID로 출발지/목적지를 표기한다. I/O 캡슐 처리 전, frame이
 * 어느 (nport, rport) 쌍에 속하는지 알아야 connection을 찾을 수 있다. nport를 D_ID로
 * 먼저 찾고 그 안에서 rport를 S_ID로 찾는 중첩 탐색이다.
 * 실행 컨텍스트: HWQP poll group 스레드(I/O hot path). inline.
 *
 * 호출 체인:
 *   nvmf_fc_hwqp_handle_request → [nvmf_fc_hwqp_find_nport_and_rport]
 */
static inline int
nvmf_fc_hwqp_find_nport_and_rport(struct spdk_nvmf_fc_hwqp *hwqp,
				  uint32_t d_id, struct spdk_nvmf_fc_nport **nport,
				  uint32_t s_id, struct spdk_nvmf_fc_remote_port_info **rport)
{
	struct spdk_nvmf_fc_nport *n_port;                               /* [한국어] nport 순회 커서 */
	struct spdk_nvmf_fc_remote_port_info *r_port;                    /* [한국어] rport 순회 커서 */

	assert(hwqp);                                                   /* [한국어] NULL 입력 조기 검출 */
	if (hwqp == NULL) {                                            /* [한국어] 릴리스 안전망 */
		SPDK_ERRLOG("Error: hwqp is NULL\n");
		return -EINVAL;
	}
	assert(nport);                                                /* [한국어] out 포인터 NULL 검출 */
	if (nport == NULL) {
		SPDK_ERRLOG("Error: nport is NULL\n");
		return -EINVAL;
	}
	assert(rport);                                                /* [한국어] out 포인터 NULL 검출 */
	if (rport == NULL) {
		SPDK_ERRLOG("Error: rport is NULL\n");
		return -EINVAL;
	}

	TAILQ_FOREACH(n_port, &hwqp->fc_port->nport_list, link) {     /* [한국어] port의 모든 nport 순회 */
		if (n_port->d_id == d_id) {                          /* [한국어] D_ID 일치 nport 발견 */
			TAILQ_FOREACH(r_port, &n_port->rem_port_list, link) {  /* [한국어] 그 nport의 rport들 순회 */
				if (r_port->s_id == s_id) {          /* [한국어] S_ID 일치 rport 발견 - 둘 다 출력 */
					*nport = n_port;
					*rport = r_port;
					return 0;
				}
			}
			break;                                       /* [한국어] D_ID는 유일하므로 nport 찾은 뒤엔 더 볼 필요 없음 */
		}
	}

	return -ENOENT;                                              /* [한국어] 매칭 (nport,rport) 쌍 없음 */
}

/* Returns true if the Nport is empty of all rem_ports */
/*
 * [한국어]
 * nvmf_fc_nport_has_no_rport - nport에 매달린 rport가 하나도 없는지 확인.
 * @nport: 대상 nport. @return: 비어 있으면 true.
 * nport 삭제 가능 여부 판단에 쓰인다(rport가 남아 있으면 삭제 불가).
 * 실행 컨텍스트: main_thread. 호출 체인: nport delete 경로 → [nvmf_fc_nport_has_no_rport]
 */
bool
nvmf_fc_nport_has_no_rport(struct spdk_nvmf_fc_nport *nport)
{
	if (nport && TAILQ_EMPTY(&nport->rem_port_list)) {          /* [한국어] nport 존재 && rport 리스트 비었으면 */
		assert(nport->rport_count == 0);                   /* [한국어] 리스트와 카운트 일관성 검증 */
		return true;
	} else {
		return false;                                      /* [한국어] rport가 남아 있거나 nport NULL */
	}
}

/*
 * [한국어]
 * nvmf_fc_nport_set_state - nport의 객체 상태(생성/삭제 중 등)를 설정.
 * @nport: 대상 nport. @state: 새 상태. @return: 0=성공, -EINVAL=NULL.
 * 실행 컨텍스트: main_thread. 호출 체인: nport lifecycle → [nvmf_fc_nport_set_state]
 */
int
nvmf_fc_nport_set_state(struct spdk_nvmf_fc_nport *nport,
			enum spdk_nvmf_fc_object_state state)
{
	if (nport) {                                              /* [한국어] 유효 nport일 때만 상태 갱신 */
		nport->nport_state = state;                      /* [한국어] 상태 필드 설정 */
		return 0;
	} else {
		return -EINVAL;                                  /* [한국어] NULL - 거부 */
	}
}

/*
 * [한국어]
 * nvmf_fc_nport_add_rem_port - nport에 원격 호스트 port(rport)를 등록.
 * @nport: 부모 nport. @rem_port: 추가할 rport. @return: 0=성공, -EINVAL=NULL.
 * PLOGI/PRLI로 호스트가 로그인하면 그 호스트를 rport로 nport에 매단다.
 * 실행 컨텍스트: main_thread. 호출 체인: I_T add → [nvmf_fc_nport_add_rem_port]
 */
bool
nvmf_fc_nport_add_rem_port(struct spdk_nvmf_fc_nport *nport,
			   struct spdk_nvmf_fc_remote_port_info *rem_port)
{
	if (nport && rem_port) {                                 /* [한국어] 둘 다 유효할 때만 */
		TAILQ_INSERT_TAIL(&nport->rem_port_list, rem_port, link);  /* [한국어] rport 리스트 꼬리에 추가 */
		nport->rport_count++;                           /* [한국어] rport 카운트 증가 */
		return 0;
	} else {
		return -EINVAL;
	}
}

/*
 * [한국어]
 * nvmf_fc_nport_remove_rem_port - nport에서 원격 호스트 port(rport)를 제거.
 * @nport: 부모 nport. @rem_port: 제거할 rport. @return: 0=성공, -EINVAL=NULL.
 * 실행 컨텍스트: main_thread. 호출 체인: I_T delete → [nvmf_fc_nport_remove_rem_port]
 */
bool
nvmf_fc_nport_remove_rem_port(struct spdk_nvmf_fc_nport *nport,
			      struct spdk_nvmf_fc_remote_port_info *rem_port)
{
	if (nport && rem_port) {                                 /* [한국어] 둘 다 유효할 때만 */
		TAILQ_REMOVE(&nport->rem_port_list, rem_port, link);  /* [한국어] rport 리스트에서 제거 */
		nport->rport_count--;                           /* [한국어] rport 카운트 감소 */
		return 0;
	} else {
		return -EINVAL;
	}
}

/*
 * [한국어]
 * nvmf_fc_rport_set_state - rport 객체 상태를 설정.
 * @rport: 대상 rport. @state: 새 상태. @return: 0=성공, -EINVAL=NULL.
 * 실행 컨텍스트: main_thread. 호출 체인: rport lifecycle → [nvmf_fc_rport_set_state]
 */
int
nvmf_fc_rport_set_state(struct spdk_nvmf_fc_remote_port_info *rport,
			enum spdk_nvmf_fc_object_state state)
{
	if (rport) {                                            /* [한국어] 유효 rport일 때만 */
		rport->rport_state = state;                    /* [한국어] 상태 설정 */
		return 0;
	} else {
		return -EINVAL;
	}
}
/*
 * [한국어]
 * nvmf_fc_assoc_set_state - association 객체 상태를 설정.
 * @assoc: 대상 association. @state: 새 상태. @return: 0=성공, -EINVAL=NULL.
 * 실행 컨텍스트: main_thread. 호출 체인: association lifecycle → [nvmf_fc_assoc_set_state]
 */
int
nvmf_fc_assoc_set_state(struct spdk_nvmf_fc_association *assoc,
			enum spdk_nvmf_fc_object_state state)
{
	if (assoc) {                                           /* [한국어] 유효 association일 때만 */
		assoc->assoc_state = state;                    /* [한국어] 상태 설정 */
		return 0;
	} else {
		return -EINVAL;
	}
}

/*
 * [한국어]
 * nvmf_ctrlr_get_fc_assoc - NVMe-oF 코어 controller에서 FC association을 역추적.
 * @ctrlr: 코어 nvmf controller. @return: 연결된 FC association 또는 NULL.
 *
 * 코어 nvmf는 FC를 모르므로, controller의 admin qpair를 FC conn으로 container_of 환원한 뒤
 * 그 conn이 속한 association을 얻는다. controller가 어느 nport에 있는지 확인할 때 쓰인다.
 * 실행 컨텍스트: main_thread. 호출 체인: nvmf_ctrlr_is_on_nport → [nvmf_ctrlr_get_fc_assoc]
 */
static struct spdk_nvmf_fc_association *
nvmf_ctrlr_get_fc_assoc(struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_qpair *qpair = ctrlr->admin_qpair;            /* [한국어] controller의 admin qpair(연결의 대표) */
	struct spdk_nvmf_fc_conn *fc_conn;                            /* [한국어] qpair를 환원한 FC conn */

	if (!qpair) {                                                 /* [한국어] admin qpair 없으면 association도 없음 */
		SPDK_ERRLOG("Controller %d has no associations\n", ctrlr->cntlid);
		return NULL;
	}

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);  /* [한국어] 임베드된 qpair 주소에서 바깥 fc_conn 복원 */

	return fc_conn->fc_assoc;                                    /* [한국어] conn이 속한 association 반환 */
}

/*
 * [한국어]
 * nvmf_ctrlr_is_on_nport - 주어진 controller가 특정 (port, nport)에 속하는지 판정.
 * @port_hdl: 물리 port 핸들. @nport_hdl: nport 핸들. @ctrlr: 검사할 controller.
 * @return: 해당 nport에 있으면 true.
 * nport 삭제 시 그 nport에 속한 controller만 정리하기 위해 사용한다.
 * 실행 컨텍스트: main_thread. 호출 체인: nport delete → [nvmf_ctrlr_is_on_nport] → nvmf_ctrlr_get_fc_assoc
 */
bool
nvmf_ctrlr_is_on_nport(uint8_t port_hdl, uint16_t nport_hdl,
		       struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_fc_nport *fc_nport = NULL;                   /* [한국어] (port,nport)로 찾은 nport */
	struct spdk_nvmf_fc_association *assoc = NULL;               /* [한국어] controller의 association */

	if (!ctrlr) {                                               /* [한국어] controller NULL이면 무조건 아님 */
		return false;
	}

	fc_nport = nvmf_fc_nport_find(port_hdl, nport_hdl);        /* [한국어] 대상 nport 조회 */
	if (!fc_nport) {                                          /* [한국어] nport 없으면 아님 */
		return false;
	}

	assoc = nvmf_ctrlr_get_fc_assoc(ctrlr);                   /* [한국어] controller의 association 역추적 */
	if (assoc && assoc->tgtport == fc_nport) {               /* [한국어] association의 target nport가 대상 nport와 같으면 일치 */
		SPDK_DEBUGLOG(nvmf_fc,
			      "Controller: %d corresponding to association: %p(%lu:%d) is on port: %d nport: %d\n",
			      ctrlr->cntlid, assoc, assoc->assoc_id, assoc->assoc_state, port_hdl,
			      nport_hdl);
		return true;
	}
	return false;
}

/*
 * [한국어]
 * nvmf_fc_release_ls_rqst - 대기 중인 LS 요청을 큐에서 빼고 수신 버퍼를 칩에 반환.
 * @hwqp: 요청이 매달린 HWQP. @ls_rqst: 해제할 LS 요청. @return: 없음.
 * LS frame을 받으면 칩의 RQ 버퍼를 점유하는데, 처리/취소 후 그 버퍼를 칩에 돌려줘야
 * 다음 frame을 받을 수 있다. ls_pending_queue에서 제거하고 RQ 버퍼를 release한다.
 * 실행 컨텍스트: main_thread/HWQP. 호출 체인: nvmf_fc_delete_ls_pending → [nvmf_fc_release_ls_rqst] → nvmf_fc_rqpair_buffer_release
 */
static void
nvmf_fc_release_ls_rqst(struct spdk_nvmf_fc_hwqp *hwqp,
			struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	assert(ls_rqst);                                               /* [한국어] NULL 요청 방어 */

	TAILQ_REMOVE(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);  /* [한국어] LS 대기 큐에서 제거 */

	/* Return buffer to chip */
	nvmf_fc_rqpair_buffer_release(hwqp, ls_rqst->rqstbuf.buf_index);  /* [한국어] 점유했던 RQ 수신 버퍼를 칩에 반환 - 재사용 가능 */
}

/*
 * [한국어]
 * nvmf_fc_delete_ls_pending - 특정 (nport, rport)에 속한 대기 LS 요청을 모두 삭제.
 * @hwqp: 대상 HWQP. @nport: target nport. @rport: 호스트 rport.
 * @return: 삭제한 LS 요청 개수.
 * I_T nexus나 nport 삭제 시, 아직 처리 못 한 그 호스트의 LS 요청들을 정리한다.
 * 실행 컨텍스트: main_thread. 호출 체인: I_T/nport delete → [nvmf_fc_delete_ls_pending] → nvmf_fc_release_ls_rqst
 */
static int
nvmf_fc_delete_ls_pending(struct spdk_nvmf_fc_hwqp *hwqp,
			  struct spdk_nvmf_fc_nport *nport,
			  struct spdk_nvmf_fc_remote_port_info *rport)
{
	struct spdk_nvmf_fc_ls_rqst *ls_rqst = NULL, *tmp;            /* [한국어] 순회 커서와 안전 순회용 임시 포인터 */
	int num_deleted = 0;                                         /* [한국어] 삭제 개수 누적 */

	assert(hwqp);                                               /* [한국어] 인자 방어 */
	assert(nport);
	assert(rport);

	TAILQ_FOREACH_SAFE(ls_rqst, &hwqp->ls_pending_queue, ls_pending_link, tmp) {  /* [한국어] 순회 중 삭제 가능한 SAFE 변형으로 LS 대기 큐 순회 */
		if ((ls_rqst->d_id == nport->d_id) && (ls_rqst->s_id == rport->s_id)) {  /* [한국어] D_ID/S_ID가 대상 nport/rport와 모두 일치하면 삭제 대상 */
			num_deleted++;                              /* [한국어] 삭제 개수 증가 */
			nvmf_fc_release_ls_rqst(hwqp, ls_rqst);    /* [한국어] 해당 LS 요청 해제 및 버퍼 반환 */
		}
	}
	return num_deleted;                                        /* [한국어] 삭제 총수 반환 */
}

/*
 * [한국어]
 * nvmf_fc_req_bdev_abort - bdev에 제출된 요청의 abort 처리(주로 AER 특수 케이스).
 * @arg1: abort할 fc_request(spdk_thread_send_msg로 전달).
 * @return: 없음.
 *
 * bdev 단계의 일반 Admin/Fabric 명령은 짧게 끝나므로 명시적 abort가 무의미하지만,
 * AER(Asynchronous Event Request)는 무한 대기형이라 별도로 free해줘야 한다. 본 함수는
 * controller의 AER 슬롯과 이 요청을 비교해 일치하면 AER을 해제한다.
 * 실행 컨텍스트: 요청이 바인딩된 HWQP 스레드(send_msg로 cross-thread 안전 진입).
 *
 * 호출 체인:
 *   nvmf_fc_request_abort(BDEV 상태) → spdk_thread_send_msg → [nvmf_fc_req_bdev_abort]
 */
static void
nvmf_fc_req_bdev_abort(void *arg1)
{
	struct spdk_nvmf_fc_request *fc_req = arg1;                   /* [한국어] send_msg로 전달된 abort 대상 요청 */
	struct spdk_nvmf_ctrlr *ctrlr = fc_req->req.qpair->ctrlr;    /* [한국어] 요청이 속한 controller(AER 슬롯 보유) */
	int i;                                                      /* [한국어] AER 슬롯 순회 인덱스 */

	/* Initial release - we don't have to abort Admin Queue or
	 * Fabric commands. The AQ commands supported at this time are
	 * Get-Log-Page,
	 * Identify
	 * Set Features
	 * Get Features
	 * AER -> Special case and handled differently.
	 * Every one of the above Admin commands (except AER) run
	 * to completion and so an Abort of such commands doesn't
	 * make sense.
	 */
	/* The Fabric commands supported are
	 * Property Set
	 * Property Get
	 * Connect -> Special case (async. handling). Not sure how to
	 * handle at this point. Let it run to completion.
	 */
	if (ctrlr) {                                                /* [한국어] controller가 있으면 AER 슬롯 검사 */
		for (i = 0; i < SPDK_NVMF_MAX_ASYNC_EVENTS; i++) {  /* [한국어] 모든 AER 슬롯 순회 */
			if (ctrlr->aer_req[i] == &fc_req->req) {   /* [한국어] 이 요청이 등록된 AER이면 */
				SPDK_NOTICELOG("Abort AER request\n");
				nvmf_qpair_free_aer(fc_req->req.qpair);  /* [한국어] 무한 대기형 AER을 해제 - 코어 nvmf에 통지 */
			}
		}
	}
}

/*
 * [한국어]
 * nvmf_fc_request_abort_complete - 요청 abort 완료 후 객체를 해제하고 등록된 콜백들에 통지.
 * @arg1: abort 완료된 fc_request.
 * @return: 없음.
 *
 * abort 절차(HBA abort/bdev notify 등)가 끝나면 호출되어, fc_request를 풀에 반환하고
 * 그동안 누적된 모든 abort 콜백(여러 호출자가 같은 요청 abort를 요청했을 수 있음)을
 * 순서대로 호출한다. fc_req가 free되기 전에 콜백 리스트를 SWAP으로 떼어내는 게 핵심이다.
 * 실행 컨텍스트: HWQP 스레드(poller API 완료 디스패치).
 *
 * 호출 체인:
 *   REQ_ABORT_COMPLETE poller 완료 → [nvmf_fc_request_abort_complete] → _nvmf_fc_request_free / ctx->cb
 */
void
nvmf_fc_request_abort_complete(void *arg1)
{
	struct spdk_nvmf_fc_request *fc_req =
		(struct spdk_nvmf_fc_request *)arg1;                 /* [한국어] abort 완료된 요청 */
	struct spdk_nvmf_fc_hwqp *hwqp = fc_req->hwqp;              /* [한국어] 콜백에 넘길 HWQP(free 후에도 필요하므로 미리 저장) */
	struct spdk_nvmf_fc_caller_ctx *ctx = NULL, *tmp = NULL;   /* [한국어] 콜백 순회 커서 */
	TAILQ_HEAD(, spdk_nvmf_fc_caller_ctx) abort_cbs;           /* [한국어] 콜백 리스트의 로컬 사본 헤드 */

	/* Make a copy of the cb list from fc_req */
	TAILQ_INIT(&abort_cbs);                                    /* [한국어] 로컬 콜백 리스트 초기화 */
	TAILQ_SWAP(&abort_cbs, &fc_req->abort_cbs, spdk_nvmf_fc_caller_ctx, link);  /* [한국어] fc_req의 콜백 리스트를 로컬로 통째 이동 - free 후에도 안전하게 순회하기 위함 */

	SPDK_NOTICELOG("FC Request(%p) in state :%s aborted\n", fc_req,
		       fc_req_state_strs[fc_req->state]);

	_nvmf_fc_request_free(fc_req);                            /* [한국어] 요청 객체를 풀에 반환(이 시점 이후 fc_req 접근 금지) */

	/* Request abort completed. Notify all the callbacks */
	TAILQ_FOREACH_SAFE(ctx, &abort_cbs, link, tmp) {          /* [한국어] 누적된 모든 abort 콜백을 순회(삭제 안전) */
		/* Notify */
		ctx->cb(hwqp, 0, ctx->cb_args);                  /* [한국어] 콜백 호출 - rc=0(abort 완료), 미리 저장한 hwqp 사용 */
		/* Remove */
		TAILQ_REMOVE(&abort_cbs, ctx, link);             /* [한국어] 처리한 콜백을 리스트에서 제거 */
		/* free */
		free(ctx);                                       /* [한국어] 콜백 컨텍스트 해제 */
	}
}

/*
 * [한국어]
 * nvmf_fc_request_abort - in-flight FC 요청을 현재 상태에 따라 적절히 abort.
 *
 * @fc_req: abort할 요청.
 * @send_abts: HBA가 상대에게 ABTS를 보내야 하는지 여부.
 * @cb: abort 완료 콜백(NULL 가능).
 * @cb_args: 콜백 인자.
 * @return: 없음(완료는 콜백/abort_complete로 비동기 통지).
 *
 * 같은 요청에 대해 여러 출처(호스트 ABTS, 내부 timeout, port down)가 abort를 요청할 수 있어
 * 콜백을 리스트로 누적하고, is_aborted로 중복 처리를 막는다. 요청의 현재 상태 머신 위치에 따라
 * 처리 방식이 다르다: BDEV 단계면 bdev에 abort 통지, XFER/RSP 단계면 HBA에 exchange abort 발행,
 * PENDING/FUSED_WAITING이면 대기 큐에서 제거 후 즉시 완료. port가 죽었고 데이터 전송 중이면
 * WQE 발행을 생략한다.
 * 실행 컨텍스트: HWQP poll group 스레드. bdev 통지는 send_msg로 cross-thread 안전 진입.
 *
 * 호출 체인:
 *   ABTS 처리 / timeout / port down → [nvmf_fc_request_abort]
 *     → spdk_thread_send_msg(bdev) / nvmf_fc_issue_abort(HBA) / nvmf_fc_poller_api_func(ABORT_COMPLETE)
 */
void
nvmf_fc_request_abort(struct spdk_nvmf_fc_request *fc_req, bool send_abts,
		      spdk_nvmf_fc_caller_cb cb, void *cb_args)
{
	struct spdk_nvmf_fc_caller_ctx *ctx = NULL;                  /* [한국어] 이번 호출의 abort 콜백 컨텍스트 */
	bool kill_req = false;                                      /* [한국어] port dead로 강제 종료해야 하는지 */

	/* Add the cb to list */
	if (cb) {                                                  /* [한국어] 콜백이 주어졌으면 누적 리스트에 추가 */
		ctx = calloc(1, sizeof(struct spdk_nvmf_fc_caller_ctx));  /* [한국어] 콜백 컨텍스트 할당 */
		if (!ctx) {                                       /* [한국어] 할당 실패 시 abort 통지 불가 - 조용히 반환 */
			SPDK_ERRLOG("ctx alloc failed.\n");
			return;
		}
		ctx->cb = cb;                                     /* [한국어] 완료 콜백 저장 */
		ctx->cb_args = cb_args;                           /* [한국어] 콜백 인자 저장 */

		TAILQ_INSERT_TAIL(&fc_req->abort_cbs, ctx, link); /* [한국어] 요청의 abort 콜백 리스트에 추가(완료 시 일괄 호출) */
	}

	if (!fc_req->is_aborted) {                                /* [한국어] 첫 abort 요청일 때만 카운터 증가(중복 집계 방지) */
		/* Increment aborted command counter */
		fc_req->hwqp->counters.num_aborted++;
	}

	/* If port is dead, skip abort wqe */
	kill_req = nvmf_fc_is_port_dead(fc_req->hwqp);            /* [한국어] HBA port가 죽었는지 확인 */
	if (kill_req && nvmf_fc_req_in_xfer(fc_req)) {            /* [한국어] port dead + 데이터 전송 중이면 WQE 발행 불가 → 즉시 완료 경로 */
		fc_req->is_aborted = true;
		goto complete;
	}

	/* Check if the request is already marked for deletion */
	if (fc_req->is_aborted) {                                /* [한국어] 이미 abort 진행 중이면 콜백만 등록하고 반환(중복 처리 금지) */
		return;
	}

	/* Mark request as aborted */
	fc_req->is_aborted = true;                               /* [한국어] abort 진행 표시 - 이후 중복 호출 차단 */

	/* If xchg is allocated, then save if we need to send abts or not. */
	if (fc_req->xchg) {                                      /* [한국어] HBA exchange가 할당돼 있으면 ABTS 송신 여부를 exchange에 기록 */
		fc_req->xchg->send_abts = send_abts;            /* [한국어] 상대에 ABTS를 보낼지 여부 */
		fc_req->xchg->aborted	= true;                 /* [한국어] exchange를 aborted로 표시 */
	}

	switch (fc_req->state) {                                 /* [한국어] 현재 상태 머신 위치에 따라 abort 방식 분기 */
	case SPDK_NVMF_FC_REQ_BDEV_ABORTED:
		/* Aborted by backend */
		_nvmf_fc_request_free(fc_req);                  /* [한국어] 이미 bdev가 abort 완료한 상태 → 바로 해제 */
		break;

	case SPDK_NVMF_FC_REQ_READ_BDEV:
	case SPDK_NVMF_FC_REQ_WRITE_BDEV:
	case SPDK_NVMF_FC_REQ_NONE_BDEV:
		/* Notify bdev */
		spdk_thread_send_msg(fc_req->hwqp->thread,      /* [한국어] bdev I/O 진행 중 → 요청 소유 스레드로 bdev abort 메시지 전달 */
				     nvmf_fc_req_bdev_abort, (void *)fc_req);
		break;

	case SPDK_NVMF_FC_REQ_READ_XFER:
	case SPDK_NVMF_FC_REQ_READ_RSP:
	case SPDK_NVMF_FC_REQ_WRITE_XFER:
	case SPDK_NVMF_FC_REQ_WRITE_RSP:
	case SPDK_NVMF_FC_REQ_NONE_RSP:
		/* Notify HBA to abort this exchange  */
		nvmf_fc_issue_abort(fc_req->hwqp, fc_req->xchg, NULL, NULL);  /* [한국어] FC 데이터/응답 전송 중 → HBA에 exchange abort WQE 발행 */
		break;

	case SPDK_NVMF_FC_REQ_PENDING:
		/* Remove from pending */
		nvmf_fc_request_remove_from_pending(fc_req);    /* [한국어] 자원 대기 큐에만 있던 요청 → 큐에서 빼고 즉시 완료 */
		goto complete;
	case SPDK_NVMF_FC_REQ_FUSED_WAITING:
		TAILQ_REMOVE(&fc_req->fc_conn->fused_waiting_queue, fc_req, fused_link);  /* [한국어] fused 명령 짝 대기 중 → 대기 큐에서 제거 후 완료 */
		goto complete;
	default:
		SPDK_ERRLOG("Request in invalid state.\n");     /* [한국어] abort 불가한 비정상 상태 - 로깅 후 완료 처리 */
		goto complete;
	}

	return;                                                /* [한국어] 비동기 abort 발행됨 - 완료는 콜백에서 */
complete:
	nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_ABORTED);  /* [한국어] 즉시 완료 경로: 상태를 ABORTED로 전이 */
	nvmf_fc_poller_api_func(fc_req->hwqp, SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE,  /* [한국어] HWQP 스레드에 abort 완료 처리(콜백/free)를 위임 */
				(void *)fc_req);
}

/*
 * [한국어]
 * nvmf_fc_request_alloc_buffers - FC 요청의 데이터 버퍼를 트랜스포트 풀에서 확보.
 * @fc_req: 버퍼가 필요한 요청. @return: 0=성공, -ENOMEM=풀 고갈.
 * write 수신/read 송신용 데이터 버퍼를 코어 nvmf 트랜스포트 iobuf 풀에서 빌려온다.
 * 실행 컨텍스트: HWQP poll group 스레드. 호출 체인: nvmf_fc_request_execute → [nvmf_fc_request_alloc_buffers] → spdk_nvmf_request_get_buffers
 */
static int
nvmf_fc_request_alloc_buffers(struct spdk_nvmf_fc_request *fc_req)
{
	uint32_t length = fc_req->req.length;                         /* [한국어] 필요한 데이터 길이(바이트) */
	struct spdk_nvmf_fc_poll_group *fgroup = fc_req->hwqp->fgroup;  /* [한국어] HWQP가 속한 FC poll group */
	struct spdk_nvmf_transport_poll_group *group = &fgroup->group;  /* [한국어] 임베드된 코어 poll group */
	struct spdk_nvmf_transport *transport = group->transport;     /* [한국어] iobuf 풀을 소유한 코어 트랜스포트 */

	if (spdk_nvmf_request_get_buffers(&fc_req->req, group, transport, length)) {  /* [한국어] 코어 풀에서 length만큼 버퍼 확보 시도 */
		return -ENOMEM;                                      /* [한국어] 풀 고갈 - 호출자가 PENDING으로 큐잉 */
	}

	return 0;                                                    /* [한국어] 버퍼 확보 성공 */
}

/*
 * [한국어]
 * nvmf_fc_request_execute - 파싱 완료된 FC 요청을 실제 실행(XCHG/버퍼 확보 후 데이터 방향별 처리).
 *
 * @fc_req: 실행할 요청.
 * @return: 0=실행 시작됨, -EAGAIN=XCHG/버퍼 부족(호출자가 PENDING 큐잉).
 *
 * 명령의 데이터 전송 방향에 따라 상태 머신을 분기시키는 핵심 함수다. send frame을 쓰지 않으면
 * 먼저 FC exchange(XRI)를 확보하고, 데이터가 있으면 버퍼를 확보한다. WRITE면 호스트로부터
 * 데이터 수신(XFER_RDY→DATA IU)을 시작하고, READ/NONE이면 bdev에 명령을 바로 실행한다.
 * 자원이 부족하면 -EAGAIN을 돌려 PENDING으로 재시도하게 한다.
 * 실행 컨텍스트: HWQP poll group 스레드(I/O hot path).
 *
 * 호출 체인:
 *   nvmf_fc_hwqp_handle_request / process_pending_reqs → [nvmf_fc_request_execute]
 *     → nvmf_fc_recv_data(WRITE) / spdk_nvmf_request_exec(READ/NONE)
 */
static int
nvmf_fc_request_execute(struct spdk_nvmf_fc_request *fc_req)
{
	/* Allocate an XCHG if we dont use send frame for this command. */
	if (!nvmf_fc_use_send_frame(fc_req)) {                       /* [한국어] send frame 최적화 경로가 아니면 HBA exchange 필요 */
		fc_req->xchg = nvmf_fc_get_xri(fc_req->hwqp);       /* [한국어] HBA에서 XRI(exchange resource indicator) 확보 */
		if (!fc_req->xchg) {                               /* [한국어] XCHG 고갈 시 -EAGAIN으로 재시도 유도 */
			fc_req->hwqp->counters.no_xchg++;
			return -EAGAIN;
		}
	}

	if (fc_req->req.length) {                                  /* [한국어] 데이터가 있는 명령이면 버퍼 확보 */
		if (nvmf_fc_request_alloc_buffers(fc_req) < 0) {  /* [한국어] 버퍼 풀 고갈 시 정리 후 재시도 */
			fc_req->hwqp->counters.buf_alloc_err++;
			if (fc_req->xchg) {                       /* [한국어] 앞서 확보한 XCHG를 반환(누수 방지) */
				nvmf_fc_put_xchg(fc_req->hwqp, fc_req->xchg);
				fc_req->xchg = NULL;
			}
			return -EAGAIN;                           /* [한국어] PENDING 재시도 신호 */
		}
	}

	if (fc_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {  /* [한국어] WRITE: 호스트→컨트롤러 데이터 전송 */
		SPDK_DEBUGLOG(nvmf_fc, "WRITE CMD.\n");

		nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_WRITE_XFER);  /* [한국어] 상태를 WRITE_XFER로 - XFER_RDY 송신 후 DATA IU 수신 단계 */

		if (nvmf_fc_recv_data(fc_req)) {                  /* [한국어] HBA에 데이터 수신(XFER_RDY) 발행 - 실패 시 drop */
			/* Dropped return success to caller */
			fc_req->hwqp->counters.unexpected_err++;
			_nvmf_fc_request_free(fc_req);            /* [한국어] 수신 발행 실패 시 요청 즉시 회수 */
		}
	} else {                                                  /* [한국어] READ 또는 데이터 없는 명령 */
		SPDK_DEBUGLOG(nvmf_fc, "READ/NONE CMD\n");

		if (fc_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {  /* [한국어] READ: 컨트롤러→호스트 */
			nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_READ_BDEV);  /* [한국어] bdev에서 데이터 읽는 단계로 */
		} else {                                          /* [한국어] Flush 등 데이터 없는 명령 */
			nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_NONE_BDEV);  /* [한국어] bdev 처리 단계로 */
		}
		spdk_nvmf_request_exec(&fc_req->req);             /* [한국어] 코어 nvmf에 명령 실행 위임 - bdev I/O 발행 */
	}

	return 0;                                                 /* [한국어] 실행 시작 성공 */
}

/*
 * [한국어]
 * nvmf_fc_set_vmid_priority - FC frame 헤더에서 VMID와 우선순위(CS_CTL)를 추출해 요청에 저장.
 *
 * @fc_req: 값을 채울 요청.
 * @fchdr: 수신한 FC frame 헤더.
 * @return: 없음.
 *
 * FC-NVMe는 가상머신 식별(VMID)과 frame 우선순위를 선택적으로 부가 헤더에 싣는다. DF_CTL/F_CTL
 * 비트로 부가 헤더(ESP/Network) 존재를 판정해 오프셋을 계산한 뒤 VM 헤더에서 src_vmid를 읽고,
 * F_CTL의 우선순위 활성 비트가 켜져 있으면 CS_CTL 값을 보존한다. QoS/멀티테넌시 라우팅용 메타데이터다.
 * 실행 컨텍스트: HWQP poll group 스레드(요청 파싱 중).
 *
 * 호출 체인:
 *   nvmf_fc_hwqp_handle_request → [nvmf_fc_set_vmid_priority]
 */
static void
nvmf_fc_set_vmid_priority(struct spdk_nvmf_fc_request *fc_req,
			  struct spdk_nvmf_fc_frame_hdr *fchdr)
{
	uint8_t df_ctl = fchdr->df_ctl;                            /* [한국어] DF_CTL - 부가 헤더(device/ESP/network) 존재 비트 */
	uint32_t f_ctl = fchdr->f_ctl;                            /* [한국어] F_CTL - frame 제어(우선순위 활성 등) 비트 */

	/* VMID */
	if (df_ctl & FCNVME_D_FCTL_DEVICE_HDR_16_MASK) {          /* [한국어] 16바이트 device 헤더(VM 헤더)가 존재하는 경우 */
		struct spdk_nvmf_fc_vm_header *vhdr;              /* [한국어] VM 헤더 포인터 */
		uint32_t vmhdr_offset = 0;                       /* [한국어] frame 헤더 끝부터 VM 헤더까지의 가변 오프셋 */

		if (df_ctl & FCNVME_D_FCTL_ESP_HDR_MASK) {       /* [한국어] ESP(보안) 헤더가 앞에 있으면 그만큼 오프셋 가산 */
			vmhdr_offset += FCNVME_D_FCTL_ESP_HDR_SIZE;
		}

		if (df_ctl & FCNVME_D_FCTL_NETWORK_HDR_MASK) {   /* [한국어] Network 헤더가 앞에 있으면 그만큼 오프셋 가산 */
			vmhdr_offset += FCNVME_D_FCTL_NETWORK_HDR_SIZE;
		}

		vhdr = (struct spdk_nvmf_fc_vm_header *)((char *)fchdr +  /* [한국어] frame 헤더 + 선행 부가 헤더 오프셋 = VM 헤더 위치 */
				sizeof(struct spdk_nvmf_fc_frame_hdr) + vmhdr_offset);
		fc_req->app_id = from_be32(&vhdr->src_vmid);     /* [한국어] 빅엔디안 src_vmid를 호스트 바이트오더로 변환해 app_id에 저장 */
	}

	/* Priority */
	if ((from_be32(&f_ctl) >> 8) & FCNVME_F_CTL_PRIORITY_ENABLE) {  /* [한국어] F_CTL 상위 비트에 우선순위 활성 플래그가 켜져 있으면 */
		fc_req->csctl = fchdr->cs_ctl;                   /* [한국어] CS_CTL(우선순위 코드)을 요청에 보존 */
	}
}

/*
 * [한국어]
 * nvmf_fc_hwqp_handle_request - 수신한 NVMe-FC CMND IU(명령 캡슐)를 검증하고 요청을 생성·실행.
 *
 * @hwqp: 명령을 받은 HWQP.
 * @frame: FC frame 헤더(S_ID/D_ID/OX_ID 등).
 * @buffer: 명령 IU가 담긴 수신 버퍼.
 * @plen: payload 길이.
 * @return: 0=정상 처리, 음수=검증 실패(드롭).
 *
 * I/O 처리의 시작점이다. CMND IU 포맷/길이, 데이터 방향, conn_id로 connection 조회,
 * S_ID/D_ID 일치, association/connection/qpair 활성 상태, MDTS(max_io_size) 초과 여부를
 * 모두 검증한 뒤, 요청 객체를 풀에서 꺼내 NVMe 명령을 복사하고 메타데이터를 채워 실행한다.
 * pending 큐가 비어있지 않거나 즉시 실행이 -EAGAIN이면 PENDING으로 큐잉한다(순서 보존).
 * 실행 컨텍스트: HWQP poll group 스레드(I/O hot path).
 *
 * 호출 체인:
 *   nvmf_fc_hwqp_process_frame(CMD_REQ) → [nvmf_fc_hwqp_handle_request]
 *     → nvmf_fc_conn_alloc_fc_request → nvmf_fc_request_execute
 */
static int
nvmf_fc_hwqp_handle_request(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_frame_hdr *frame,
			    struct spdk_nvmf_fc_buffer_desc *buffer, uint32_t plen)
{
	uint16_t cmnd_len;                                          /* [한국어] CMND IU 길이(4바이트 워드 단위) */
	uint64_t rqst_conn_id;                                     /* [한국어] 명령이 지정한 connection ID */
	struct spdk_nvmf_fc_request *fc_req = NULL;               /* [한국어] 생성할 FC 요청 */
	struct spdk_nvmf_fc_cmnd_iu *cmd_iu = NULL;              /* [한국어] 수신 버퍼를 CMND IU로 본 뷰 */
	struct spdk_nvmf_fc_conn *fc_conn = NULL;               /* [한국어] conn_id로 조회한 connection */
	enum spdk_nvme_data_transfer xfer;                       /* [한국어] 명령의 데이터 전송 방향 */
	uint32_t s_id, d_id;                                     /* [한국어] frame의 24비트 S_ID/D_ID */

	s_id = (uint32_t)frame->s_id;                            /* [한국어] frame에서 S_ID 바이트 추출 */
	d_id = (uint32_t)frame->d_id;                            /* [한국어] frame에서 D_ID 바이트 추출 */
	s_id = from_be32(&s_id) >> 8;                           /* [한국어] 빅엔디안 변환 후 상위 8비트 버려 24비트 S_ID 획득 */
	d_id = from_be32(&d_id) >> 8;                           /* [한국어] 동일하게 24비트 D_ID 획득 */

	cmd_iu = buffer->virt;                                   /* [한국어] 수신 버퍼 가상주소를 CMND IU 구조로 해석 */
	cmnd_len = cmd_iu->cmnd_iu_len;                          /* [한국어] IU 길이 필드 읽기(빅엔디안) */
	cmnd_len = from_be16(&cmnd_len);                        /* [한국어] 호스트 바이트오더로 변환 */

	/* check for a valid cmnd_iu format */
	if ((cmd_iu->fc_id != FCNVME_CMND_IU_FC_ID) ||          /* [한국어] FC-NVMe 식별자 검증 */
	    (cmd_iu->scsi_id != FCNVME_CMND_IU_SCSI_ID) ||      /* [한국어] SCSI ID 필드 검증 */
	    (cmnd_len != sizeof(struct spdk_nvmf_fc_cmnd_iu) / 4)) {  /* [한국어] IU 길이가 정의된 크기(워드 단위)와 일치하는지 */
		SPDK_ERRLOG("IU CMD error\n");
		hwqp->counters.nvme_cmd_iu_err++;               /* [한국어] 포맷 오류 카운트 */
		return -ENXIO;                                  /* [한국어] 잘못된 IU - 드롭 */
	}

	xfer = spdk_nvme_opc_get_data_transfer(cmd_iu->flags);  /* [한국어] 명령 플래그에서 데이터 전송 방향 디코드 */
	if (xfer == SPDK_NVME_DATA_BIDIRECTIONAL) {            /* [한국어] 양방향 전송은 NVMe-FC에서 미지원 */
		SPDK_ERRLOG("IU CMD xfer error\n");
		hwqp->counters.nvme_cmd_xfer_err++;
		return -EPERM;
	}

	rqst_conn_id = from_be64(&cmd_iu->conn_id);            /* [한국어] 64비트 connection ID를 호스트 오더로 변환 */

	if (rte_hash_lookup_data(hwqp->connection_list_hash,  /* [한국어] DPDK 해시에서 conn_id로 connection 조회(lockless) */
				 (void *)&rqst_conn_id, (void **)&fc_conn) < 0) {
		SPDK_ERRLOG("IU CMD conn(%ld) invalid\n", rqst_conn_id);
		hwqp->counters.invalid_conn_err++;              /* [한국어] 미등록 connection - 드롭 */
		return -ENODEV;
	}

	/* Validate s_id and d_id */
	if (s_id != fc_conn->s_id) {                          /* [한국어] frame S_ID가 connection의 호스트 주소와 일치하는지(스푸핑 방지) */
		hwqp->counters.rport_invalid++;
		SPDK_ERRLOG("Frame s_id invalid for connection %ld\n", rqst_conn_id);
		return -ENODEV;
	}

	if (d_id != fc_conn->d_id) {                          /* [한국어] frame D_ID가 connection의 target 주소와 일치하는지 */
		hwqp->counters.nport_invalid++;
		SPDK_ERRLOG("Frame d_id invalid for connection %ld\n", rqst_conn_id);
		return -ENODEV;
	}

	/* If association/connection is being deleted - return */
	if (fc_conn->fc_assoc->assoc_state != SPDK_NVMF_FC_OBJECT_CREATED) {  /* [한국어] association이 생성 완료 상태가 아니면(삭제 중 등) 거부 */
		SPDK_ERRLOG("Association %ld state = %d not valid\n",
			    fc_conn->fc_assoc->assoc_id, fc_conn->fc_assoc->assoc_state);
		return -EACCES;
	}

	if (fc_conn->conn_state != SPDK_NVMF_FC_OBJECT_CREATED) {  /* [한국어] connection이 생성 완료 상태가 아니면 거부 */
		SPDK_ERRLOG("Connection %ld state = %d not valid\n",
			    rqst_conn_id, fc_conn->conn_state);
		return -EACCES;
	}

	if (!spdk_nvmf_qpair_is_active(&fc_conn->qpair)) {    /* [한국어] 코어 qpair가 active가 아니면(연결 중/종료 중) 거부 */
		SPDK_ERRLOG("Connection %ld qpair state = %d not valid\n",
			    rqst_conn_id, fc_conn->qpair.state);
		return -EACCES;
	}

	/* Make sure xfer len is according to mdts */
	if (from_be32(&cmd_iu->data_len) >                    /* [한국어] 요청 데이터 길이가 트랜스포트 MDTS(max_io_size)를 넘으면 거부 */
	    hwqp->fgroup->group.transport->opts.max_io_size) {
		SPDK_ERRLOG("IO length requested is greater than MDTS\n");
		return -EINVAL;
	}

	/* allocate a request buffer */
	fc_req = nvmf_fc_conn_alloc_fc_request(fc_conn);      /* [한국어] connection 풀에서 요청 객체 확보 */
	if (fc_req == NULL) {                                 /* [한국어] 풀 고갈 시 드롭 */
		return -ENOMEM;
	}

	fc_req->req.length = from_be32(&cmd_iu->data_len);   /* [한국어] 코어 req에 데이터 길이 설정 */
	fc_req->req.qpair = &fc_conn->qpair;                 /* [한국어] 코어 req를 connection의 qpair에 연결 */
	memcpy(&fc_req->cmd, &cmd_iu->cmd, sizeof(union nvmf_h2c_msg));  /* [한국어] CMND IU의 NVMe 명령(SQE)을 요청에 복사 */
	fc_req->req.cmd = (union nvmf_h2c_msg *)&fc_req->cmd;  /* [한국어] 코어 req의 cmd 포인터를 복사본으로 설정 */
	fc_req->req.rsp = (union nvmf_c2h_msg *)&fc_req->ersp.rsp;  /* [한국어] 응답(CQE)이 채워질 위치를 ERSP IU 내부로 지정 */
	fc_req->oxid = frame->ox_id;                         /* [한국어] OX_ID 보관(응답/abort 식별) */
	fc_req->oxid = from_be16(&fc_req->oxid);             /* [한국어] 빅엔디안 OX_ID를 호스트 오더로 변환 */
	fc_req->rpi = fc_conn->rpi;                          /* [한국어] connection의 RPI 복사 */
	fc_req->poller_lcore = hwqp->lcore_id;               /* [한국어] 이 요청을 처리할 lcore(trace 기록용) */
	fc_req->poller_thread = hwqp->thread;                /* [한국어] 처리 스레드(affinity) */
	fc_req->hwqp = hwqp;                                 /* [한국어] 소속 HWQP 역참조 */
	fc_req->fc_conn = fc_conn;                           /* [한국어] 소속 connection 역참조 */
	fc_req->req.xfer = xfer;                             /* [한국어] 데이터 전송 방향 저장 */
	fc_req->s_id = s_id;                                 /* [한국어] 호스트 주소 보관 */
	fc_req->d_id = d_id;                                 /* [한국어] target 주소 보관 */
	fc_req->csn  = from_be32(&cmd_iu->cmnd_seq_num);    /* [한국어] command sequence number(순서/SLER 추적) */
	nvmf_fc_set_vmid_priority(fc_req, frame);            /* [한국어] frame에서 VMID/우선순위 메타데이터 추출 */

	nvmf_fc_record_req_trace_point(fc_req, SPDK_NVMF_FC_REQ_INIT);  /* [한국어] I/O 시작 trace point 기록 */

	if (!STAILQ_EMPTY(&hwqp->fgroup->group.pending_buf_queue) || nvmf_fc_request_execute(fc_req)) {  /* [한국어] 이미 대기 요청이 있거나 즉시 실행이 -EAGAIN이면 순서 보존 위해 PENDING 큐잉 */
		STAILQ_INSERT_TAIL(&hwqp->fgroup->group.pending_buf_queue, &fc_req->req, buf_link);  /* [한국어] 코어 pending 큐 꼬리에 추가 */
		nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_PENDING);  /* [한국어] 상태를 PENDING으로 - 자원 가용 시 재시도 */
	}

	return 0;                                            /* [한국어] 명령 접수 성공(실행 시작 또는 큐잉됨) */
}

/*
 * These functions are called from the FC LLD
 */
/* [한국어] 아래는 LLD(벤더별 Low Level Driver)가 호출하는 외부 API들 - SPDK FC 모듈의 인터페이스. */

/*
 * [한국어]
 * _nvmf_fc_request_free - FC request 객체의 자원 회수
 *
 * @fc_req: 회수할 FC request (NULL이면 no-op)
 *
 * FC request 완료/abort 후 자원 회수 단계:
 *  1. xchg(FC exchange context) LLD 풀에 반환
 *  2. iobuf 데이터 버퍼 풀 반환 (data_from_pool인 경우만)
 *  3. iovcnt/raw/cmd_cb_fn 초기화
 *  4. fc_conn의 reqpool로 fc_req 자체 반환
 *
 * 호출 체인:
 *   nvmf_fc_handle_rsp 완료 콜백 -> [본 함수]
 *   nvmf_fc_request_abort_complete -> [본 함수]
 */
void
_nvmf_fc_request_free(struct spdk_nvmf_fc_request *fc_req)
{
	struct spdk_nvmf_fc_hwqp *hwqp;                               /* [한국어] 요청이 속한 HWQP */
	struct spdk_nvmf_transport_poll_group *group;               /* [한국어] 버퍼 반환 대상 코어 poll group */

	if (!fc_req) {                                              /* [한국어] NULL이면 no-op */
		return;
	}
	hwqp = fc_req->hwqp;                                       /* [한국어] HWQP 캐시 */

	if (fc_req->xchg) {                                        /* [한국어] HBA exchange가 남아있으면 LLD 풀에 반환 */
		nvmf_fc_put_xchg(hwqp, fc_req->xchg);             /* [한국어] XRI 반환 - 다음 명령이 재사용 */
		fc_req->xchg = NULL;                              /* [한국어] dangling 방지 */
	}

	/* Release IO buffers */
	if (fc_req->req.data_from_pool) {                         /* [한국어] 풀에서 빌린 데이터 버퍼가 있을 때만 반환 */
		group = &hwqp->fgroup->group;                    /* [한국어] 코어 poll group 참조 */
		spdk_nvmf_request_free_buffers(&fc_req->req, group,  /* [한국어] iobuf 데이터 버퍼를 트랜스포트 풀에 반환 */
					       group->transport);
	}
	fc_req->req.iovcnt = 0;                                   /* [한국어] iov 카운트 초기화 */
	fc_req->req.raw = 0; /* clear all flags */               /* [한국어] req 플래그 비트들을 한 번에 클리어 */
	fc_req->req.cmd_cb_fn = NULL;                            /* [한국어] custom 명령 콜백 초기화 */

	/* Free Fc request */
	nvmf_fc_conn_free_fc_request(fc_req->fc_conn, fc_req);   /* [한국어] 요청 객체를 connection 풀에 반환 */
}

/*
 * [한국어]
 * nvmf_fc_request_set_state - FC 요청의 상태를 전이하고 trace/디버그 로그를 남김.
 * @fc_req: 대상 요청. @state: 새 상태. @return: 없음.
 * 상태 머신의 모든 전이가 이 함수를 거치므로 trace point 기록의 단일 진입점이 된다.
 * magic이 DEADBEEF면 use-after-free이므로 assert로 잡는다.
 * 실행 컨텍스트: HWQP poll group 스레드. 호출 체인: 모든 상태 전이 코드 → [nvmf_fc_request_set_state] → nvmf_fc_record_req_trace_point
 */
void
nvmf_fc_request_set_state(struct spdk_nvmf_fc_request *fc_req,
			  enum spdk_nvmf_fc_request_state state)
{
	assert(fc_req->magic != 0xDEADBEEF);                     /* [한국어] 해제된 요청을 다시 쓰면 즉시 중단(use-after-free 방어) */

	SPDK_DEBUGLOG(nvmf_fc,
		      "FC Request(%p):\n\tState Old:%s New:%s\n", fc_req,
		      nvmf_fc_request_get_state_str(fc_req->state),  /* [한국어] 이전 상태 문자열 */
		      nvmf_fc_request_get_state_str(state));         /* [한국어] 새 상태 문자열 */
	nvmf_fc_record_req_trace_point(fc_req, state);          /* [한국어] 상태 전이를 trace 버퍼에 기록(latency 분석) */
	fc_req->state = state;                                  /* [한국어] 실제 상태 갱신 */
}

/*
 * [한국어]
 * nvmf_fc_request_get_state_str - 상태 enum 값을 사람이 읽을 문자열로 변환.
 * @state: 상태 정수. @return: 대응 문자열 또는 "unknown".
 * 범위를 벗어난 값은 "unknown"으로 안전 처리한다.
 * 실행 컨텍스트: 어디서나. 호출 체인: 로그/디버그 → [nvmf_fc_request_get_state_str]
 */
char *
nvmf_fc_request_get_state_str(int state)
{
	static char *unk_str = "unknown";                       /* [한국어] 범위 밖 값에 대한 기본 문자열 */

	return (state >= 0 && state < (int)(sizeof(fc_req_state_strs) / sizeof(char *)) ?  /* [한국어] 인덱스 범위 검사 */
		fc_req_state_strs[state] : unk_str);            /* [한국어] 유효하면 매핑 테이블, 아니면 unknown */
}

/*
 * [한국어]
 * nvmf_fc_hwqp_process_frame - HWQP가 RQ에서 받은 FC frame을 종류별로 분기 처리.
 *
 * @hwqp: frame을 받은 HWQP.
 * @buff_idx: RQ 수신 버퍼 인덱스(반환 시 필요).
 * @frame: FC frame 헤더.
 * @buffer: frame payload 버퍼.
 * @plen: payload 길이.
 * @return: 0=정상, 음수=드롭.
 *
 * LLD가 RQ에서 frame을 꺼내 호출하는 디스패처다. R_CTL/TYPE 조합으로 (1) LS 요청이면
 * nport/rport를 검증하고 RQ 버퍼를 LS 요청 구조로 재활용해 fc_ls.c 모듈에 넘기며,
 * (2) NVMe 명령 캡슐이면 nvmf_fc_hwqp_handle_request로 처리하고 성공 시 버퍼를 칩에 반환,
 * (3) 알 수 없는 frame은 드롭한다. LS는 XCHG가 없으면 ls_pending_queue로 미룬다.
 * 실행 컨텍스트: HWQP poll group 스레드(polled-mode RQ 소비).
 *
 * 호출 체인:
 *   nvmf_fc_hwqp_poll(LLD RQ poll) → [nvmf_fc_hwqp_process_frame]
 *     → nvmf_fc_handle_ls_rqst(LS) / nvmf_fc_hwqp_handle_request(CMD)
 */
int
nvmf_fc_hwqp_process_frame(struct spdk_nvmf_fc_hwqp *hwqp,
			   uint32_t buff_idx,
			   struct spdk_nvmf_fc_frame_hdr *frame,
			   struct spdk_nvmf_fc_buffer_desc *buffer,
			   uint32_t plen)
{
	int rc = 0;                                              /* [한국어] 처리 결과 코드 */
	uint32_t s_id, d_id;                                    /* [한국어] frame의 24비트 출발/목적 주소 */
	struct spdk_nvmf_fc_nport *nport = NULL;               /* [한국어] LS 처리 시 찾은 nport */
	struct spdk_nvmf_fc_remote_port_info *rport = NULL;    /* [한국어] LS 처리 시 찾은 rport */

	s_id = (uint32_t)frame->s_id;                          /* [한국어] S_ID 바이트 추출 */
	d_id = (uint32_t)frame->d_id;                          /* [한국어] D_ID 바이트 추출 */
	s_id = from_be32(&s_id) >> 8;                         /* [한국어] 빅엔디안 변환 후 24비트 S_ID */
	d_id = from_be32(&d_id) >> 8;                         /* [한국어] 24비트 D_ID */

	SPDK_DEBUGLOG(nvmf_fc,
		      "Process NVME frame s_id:0x%x d_id:0x%x oxid:0x%x rxid:0x%x.\n",
		      s_id, d_id,
		      ((frame->ox_id << 8) & 0xff00) | ((frame->ox_id >> 8) & 0xff),  /* [한국어] OX_ID 바이트스왑(로그 표시용) */
		      ((frame->rx_id << 8) & 0xff00) | ((frame->rx_id >> 8) & 0xff)); /* [한국어] RX_ID 바이트스왑 */

	if ((frame->r_ctl == FCNVME_R_CTL_LS_REQUEST) &&        /* [한국어] R_CTL이 LS 요청이고 */
	    (frame->type == FCNVME_TYPE_NVMF_DATA)) {           /* [한국어] TYPE이 NVMe-FC LS 데이터이면 LS 처리 경로 */
		struct spdk_nvmf_fc_rq_buf_ls_request *req_buf = buffer->virt;  /* [한국어] RQ 버퍼를 LS 요청 레이아웃으로 해석 */
		struct spdk_nvmf_fc_ls_rqst *ls_rqst;           /* [한국어] 채울 LS 요청 구조 */

		SPDK_DEBUGLOG(nvmf_fc, "Process LS NVME frame\n");

		rc = nvmf_fc_hwqp_find_nport_and_rport(hwqp, d_id, &nport, s_id, &rport);  /* [한국어] D_ID/S_ID로 nport/rport 조회 */
		if (rc) {                                       /* [한국어] 조회 실패 시 원인별 카운트 후 드롭 */
			if (nport == NULL) {
				SPDK_ERRLOG("Nport not found. Dropping\n");
				/* increment invalid nport counter */
				hwqp->counters.nport_invalid++;
			} else if (rport == NULL) {
				SPDK_ERRLOG("Rport not found. Dropping\n");
				/* increment invalid rport counter */
				hwqp->counters.rport_invalid++;
			}
			return rc;
		}

		if (nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED ||  /* [한국어] nport/rport가 생성 완료 상태가 아니면(삭제 중 등) 드롭 */
		    rport->rport_state != SPDK_NVMF_FC_OBJECT_CREATED) {
			SPDK_ERRLOG("%s state not created. Dropping\n",
				    nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED ?
				    "Nport" : "Rport");
			return -EACCES;
		}

		/* Use the RQ buffer for holding LS request. */
		ls_rqst = (struct spdk_nvmf_fc_ls_rqst *)&req_buf->ls_rqst;  /* [한국어] RQ 버퍼 내부의 ls_rqst 영역을 그대로 사용(zero-copy) */

		/* Fill in the LS request structure */
		ls_rqst->rqstbuf.virt = (void *)&req_buf->rqst;  /* [한국어] 요청 payload 가상주소 */
		ls_rqst->rqstbuf.phys = buffer->phys +           /* [한국어] 요청 payload 물리주소(LLD가 DMA용으로 필요) */
					offsetof(struct spdk_nvmf_fc_rq_buf_ls_request, rqst);  /* [한국어] 버퍼 시작 물리주소 + rqst 필드 오프셋 */
		ls_rqst->rqstbuf.buf_index = buff_idx;          /* [한국어] RQ 버퍼 인덱스(나중에 칩에 반환할 때 사용) */
		ls_rqst->rqst_len = plen;                       /* [한국어] 요청 길이 */

		ls_rqst->rspbuf.virt = (void *)&req_buf->resp;  /* [한국어] 응답 payload 가상주소(같은 RQ 버퍼 내) */
		ls_rqst->rspbuf.phys = buffer->phys +           /* [한국어] 응답 payload 물리주소 */
				       offsetof(struct spdk_nvmf_fc_rq_buf_ls_request, resp);  /* [한국어] 버퍼 물리주소 + resp 오프셋 */
		ls_rqst->rsp_len = FCNVME_MAX_LS_RSP_SIZE;      /* [한국어] 응답 버퍼 최대 크기 */

		ls_rqst->private_data = (void *)hwqp;           /* [한국어] LS 모듈이 응답 송신 시 HWQP 복원용 */
		ls_rqst->rpi = rport->rpi;                      /* [한국어] 응답을 보낼 RPI */
		ls_rqst->oxid = (uint16_t)frame->ox_id;         /* [한국어] OX_ID 보관 */
		ls_rqst->oxid = from_be16(&ls_rqst->oxid);     /* [한국어] 빅엔디안 변환 */
		ls_rqst->s_id = s_id;                           /* [한국어] 호스트 주소 */
		ls_rqst->d_id = d_id;                           /* [한국어] target 주소 */
		ls_rqst->nport = nport;                         /* [한국어] 대상 nport */
		ls_rqst->rport = rport;                         /* [한국어] 상대 rport */
		ls_rqst->nvmf_tgt = g_nvmf_ftransport->transport.tgt;  /* [한국어] 코어 nvmf target 핸들(Create Association 등에 필요) */

		if (TAILQ_EMPTY(&hwqp->ls_pending_queue)) {     /* [한국어] 대기 중인 LS가 없으면 즉시 XCHG 확보 시도(순서 보존) */
			ls_rqst->xchg = nvmf_fc_get_xri(hwqp);
		} else {
			ls_rqst->xchg = NULL;                   /* [한국어] 이미 대기 LS가 있으면 FIFO 유지를 위해 새 frame도 대기 */
		}

		if (ls_rqst->xchg) {                            /* [한국어] XCHG 확보되면 LS 모듈에 즉시 처리 위임 */
			/* Handover the request to LS module */
			nvmf_fc_handle_ls_rqst(ls_rqst);        /* [한국어] fc_ls.c로 넘김 - Create Assoc/Conn/Disconnect 처리 */
		} else {
			/* No XCHG available. Add to pending list. */
			hwqp->counters.no_xchg++;               /* [한국어] XCHG 고갈 카운트 */
			TAILQ_INSERT_TAIL(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);  /* [한국어] LS 대기 큐에 추가 - 나중에 재시도 */
		}
	} else if ((frame->r_ctl == FCNVME_R_CTL_CMD_REQ) &&    /* [한국어] R_CTL이 명령 요청이고 */
		   (frame->type == FCNVME_TYPE_FC_EXCHANGE)) {  /* [한국어] TYPE이 FC exchange이면 I/O 명령 경로 */

		SPDK_DEBUGLOG(nvmf_fc, "Process IO NVME frame\n");
		rc = nvmf_fc_hwqp_handle_request(hwqp, frame, buffer, plen);  /* [한국어] NVMe 명령 캡슐 처리 */
		if (!rc) {                                      /* [한국어] 처리 성공 시 RQ 버퍼를 칩에 즉시 반환(요청은 자체 버퍼 사용) */
			nvmf_fc_rqpair_buffer_release(hwqp, buff_idx);
		}
	} else {                                                /* [한국어] 알 수 없는 frame 종류 */

		SPDK_ERRLOG("Unknown frame received. Dropping\n");
		hwqp->counters.unknown_frame++;                 /* [한국어] 미지 frame 카운트 */
		rc = -EINVAL;
	}

	return rc;                                              /* [한국어] 처리 결과 반환 */
}

/*
 * [한국어]
 * nvmf_fc_hwqp_process_pending_reqs - 자원 부족으로 PENDING된 I/O 요청을 budget 내에서 재시도.
 * @hwqp: 대상 HWQP. @return: 없음.
 *
 * XCHG/버퍼 고갈로 pending_buf_queue에 쌓인 요청들을 polling마다 다시 실행 시도한다.
 * 한 번에 너무 오래 점유하지 않도록 budget(64)으로 제한한다. LS 큐 전용 HWQP(fgroup 없음)는
 * 건너뛴다.
 * 실행 컨텍스트: HWQP poll group 스레드(polling 루프). 호출 체인: nvmf_fc_hwqp_poll → [nvmf_fc_hwqp_process_pending_reqs] → nvmf_fc_request_execute
 */
void
nvmf_fc_hwqp_process_pending_reqs(struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct spdk_nvmf_request *req = NULL, *tmp;             /* [한국어] 코어 req 순회 커서(삭제 안전) */
	struct spdk_nvmf_fc_request *fc_req;                   /* [한국어] 환원한 FC 요청 */
	int budget = 64;                                       /* [한국어] 한 번의 polling에서 재시도할 최대 요청 수(CPU 독점 방지) */

	if (!hwqp->fgroup) {                                   /* [한국어] poll group 미배정(LS 전용) HWQP는 pending_buf_queue를 안 씀 */
		/* LS queue is tied to acceptor_poll group and LS pending requests
		 * are stagged and processed using hwqp->ls_pending_queue.
		 */
		return;
	}

	STAILQ_FOREACH_SAFE(req, &hwqp->fgroup->group.pending_buf_queue, buf_link, tmp) {  /* [한국어] pending 큐를 FIFO 순서로 순회 */
		fc_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_fc_request, req);  /* [한국어] 코어 req에서 FC 요청 복원 */
		if (!nvmf_fc_request_execute(fc_req)) {       /* [한국어] 재실행 성공(0 반환)이면 pending에서 제거 */
			/* Successfully posted, Delete from pending. */
			nvmf_fc_request_remove_from_pending(fc_req);
		}

		if (budget) {                                 /* [한국어] budget 남아있으면 감소 */
			budget--;
		} else {                                      /* [한국어] budget 소진 시 나머지는 다음 polling으로 미룸 */
			return;
		}
	}
}

/*
 * [한국어]
 * nvmf_fc_hwqp_process_pending_ls_rqsts - XCHG 부족으로 보류된 LS 요청을 재시도한다.
 *
 * @hwqp: LS 요청을 수신·처리하는 대상 HWQP. ls_pending_queue를 소유.
 * @return: 없음.
 *
 * Link Service(LS) 요청(Create Association/Connection, Disconnect 등)은 처리에
 * XCHG(eXCHanGe, FC HBA의 교환 컨텍스트 자원)가 필요하다. nvmf_fc_handle_ls_rqst가
 * XCHG 고갈로 즉시 처리하지 못하면 요청을 ls_pending_queue에 적재해 두는데, 본 함수가
 * polling 루프마다 이 큐를 다시 훑어 자원이 회복되었으면 처리를 재개한다.
 * 일반 I/O의 pending_buf_queue(nvmf_fc_hwqp_process_pending_reqs)와 달리, LS 큐는
 * acceptor poll group에 묶여 있고 별도의 ls_pending_queue로 staging된다.
 *
 * 동작 과정:
 *  1) nport/rport를 d_id/s_id로 재조회한다. polling 사이에 nport/rport가 삭제되었을
 *     수 있으므로 보류된 요청마다 유효성을 다시 검증한다(stale 참조 방지).
 *  2) nport/rport가 없거나 CREATED 상태가 아니면 요청을 drop하고 카운터를 올린다.
 *  3) XCHG를 다시 할당 시도. 성공하면 큐에서 제거 후 LS 모듈로 handover.
 *  4) XCHG가 여전히 없으면 더 시도해도 소용없으므로 즉시 return(FIFO 순서 보존).
 *
 * 실행 컨텍스트: HWQP를 폴링하는 spdk_thread(LS는 acceptor poll group 스레드).
 * 단일 스레드 소유이므로 ls_pending_queue 접근에 락 불필요.
 *
 * 호출 체인:
 *   nvmf_fc_process_queue/poller cleanup -> [본 함수]
 *     -> nvmf_fc_hwqp_find_nport_and_rport / nvmf_fc_get_xri / nvmf_fc_handle_ls_rqst
 */
void
nvmf_fc_hwqp_process_pending_ls_rqsts(struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct spdk_nvmf_fc_ls_rqst *ls_rqst = NULL, *tmp;     /* [한국어] 보류 LS 요청 순회 커서(삭제 안전 매크로용 tmp 포함) */
	struct spdk_nvmf_fc_nport *nport = NULL;               /* [한국어] 재조회로 채워질 target nport(D_ID 매칭) */
	struct spdk_nvmf_fc_remote_port_info *rport = NULL;    /* [한국어] 재조회로 채워질 host rport(S_ID 매칭) */

	TAILQ_FOREACH_SAFE(ls_rqst, &hwqp->ls_pending_queue, ls_pending_link, tmp) {  /* [한국어] 보류 LS 큐를 FIFO로 순회(처리 중 제거 가능하므로 SAFE) */
		/* lookup nport and rport again - make sure they are still valid */
		int rc = nvmf_fc_hwqp_find_nport_and_rport(hwqp, ls_rqst->d_id, &nport, ls_rqst->s_id, &rport);  /* [한국어] D_ID->nport, S_ID->rport 재조회 - polling 사이 삭제 여부 검증 */
		if (rc) {                                     /* [한국어] 조회 실패(nport 또는 rport 없음) */
			if (nport == NULL) {                  /* [한국어] target nport 자체가 사라짐 - 수신 대상 부재 */
				SPDK_ERRLOG("Nport not found. Dropping\n");
				/* increment invalid nport counter */
				hwqp->counters.nport_invalid++;  /* [한국어] 통계: 무효 nport 수신 카운터 증가(진단용) */
			} else if (rport == NULL) {           /* [한국어] nport는 있으나 보낸 host rport가 사라짐 */
				SPDK_ERRLOG("Rport not found. Dropping\n");
				/* increment invalid rport counter */
				hwqp->counters.rport_invalid++;  /* [한국어] 통계: 무효 rport 수신 카운터 증가 */
			}
			nvmf_fc_release_ls_rqst(hwqp, ls_rqst);  /* [한국어] 처리 불가 LS 요청을 큐에서 제거하고 자원 반환(drop) */
			continue;                             /* [한국어] 다음 보류 요청으로 진행 */
		}
		if (nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED ||  /* [한국어] nport가 CREATED가 아니면(삭제 진행 중 등) */
		    rport->rport_state != SPDK_NVMF_FC_OBJECT_CREATED) {  /* [한국어] 또는 rport가 CREATED가 아니면 - 처리 불가 */
			SPDK_ERRLOG("%s state not created. Dropping\n",
				    nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED ?
				    "Nport" : "Rport");          /* [한국어] 어느 객체가 비정상 상태인지 로그에 명시 */
			nvmf_fc_release_ls_rqst(hwqp, ls_rqst);  /* [한국어] 상태 불일치 요청을 drop */
			continue;                             /* [한국어] 다음 요청으로 */
		}

		ls_rqst->xchg = nvmf_fc_get_xri(hwqp);        /* [한국어] HBA의 XCHG/XRI 자원 재할당 시도 - 처리에 필수 */
		if (ls_rqst->xchg) {                          /* [한국어] XCHG 확보 성공 */
			/* Got an XCHG */
			TAILQ_REMOVE(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);  /* [한국어] 보류 큐에서 제거 - 이제 실제 처리로 넘어감 */
			/* Handover the request to LS module */
			nvmf_fc_handle_ls_rqst(ls_rqst);      /* [한국어] fc_ls.c LS dispatcher로 전달 - Create Assoc/Conn 등 상태 머신 진행 */
		} else {                                      /* [한국어] XCHG 여전히 고갈 */
			/* No more XCHGs. Stop processing. */
			hwqp->counters.no_xchg++;             /* [한국어] 통계: XCHG 부족 카운터 증가 */
			return;                               /* [한국어] 뒤 요청도 동일하게 실패하므로 즉시 중단(FIFO 공정성 보존) */
		}
	}
}

/*
 * [한국어]
 * nvmf_fc_handle_rsp - NVMe 명령 완료 응답(RSP IU 또는 ERSP IU)을 host로 송신한다.
 *
 * @fc_req: 완료된 FC 요청. req->rsp에 NVMe CQE(completion)가 채워져 있음.
 * @return: 0 송신 성공, 음수면 LLD 송신 실패.
 *
 * FC-NVMe는 명령 완료를 두 형태로 보낸다:
 *  - RSP IU (Response Information Unit): SQHD만 전달하는 경량 응답. 성공·정상 시 사용.
 *  - ERSP IU (Extended RSP IU): 전체 NVMe CQE를 담는 확장 응답. 에러/특정 조건에서 사용.
 * 어느 쪽을 쓸지는 nvmf_fc_send_ersp_required()가 판정한다(ersp_ratio, 에러, 전송길이
 * 불일치 등). ERSP는 RSN(Response Sequence Number)으로 host가 순서를 추적하게 한다.
 *
 * 동작 과정:
 *  1) NVMe CQE의 SQHD(Submission Queue Head)를 현재 conn의 sqhead로 채워 host의
 *     SQ 흐름 제어(flow control)를 갱신한다(NVMe 스펙 SQHD 의미).
 *  2) ERSP 필요 시: ersp_len(32bit word 단위), RSN, 전송된 데이터 길이를 빅엔디언으로
 *     채우고 ERSP IU 전체를 송신.
 *  3) 그렇지 않으면 RSP만 송신(payload NULL).
 *
 * 실행 컨텍스트: HWQP poll group 스레드(요청 완료 처리 경로).
 *
 * 호출 체인:
 *   nvmf_fc_request_complete -> [본 함수] -> nvmf_fc_xmt_rsp(LLD send)
 */
int
nvmf_fc_handle_rsp(struct spdk_nvmf_fc_request *fc_req)
{
	int rc = 0;                                           /* [한국어] LLD 송신 반환코드 - 호출자가 실패 시 req free */
	struct spdk_nvmf_request *req = &fc_req->req;         /* [한국어] 임베드된 공통 NVMe-oF request */
	struct spdk_nvmf_qpair *qpair = req->qpair;           /* [한국어] 이 요청이 속한 qpair(=FC connection) */
	struct spdk_nvmf_fc_conn *fc_conn = nvmf_fc_get_conn(qpair);  /* [한국어] qpair에서 FC connection 컨테이너 복원 - rsn/rsp_count 접근 */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;      /* [한국어] NVMe CQE(completion queue entry) - SQHD를 채울 대상 */
	uint16_t ersp_len = 0;                                /* [한국어] ERSP IU 길이(32bit word 단위), 빅엔디언 변환용 임시 */

	/* set sq head value in resp */
	rsp->sqhd = nvmf_fc_advance_conn_sqhead(qpair);       /* [한국어] conn의 SQ head 포인터를 1 전진시키고 그 값을 CQE에 기록 - host의 SQ 슬롯 반환(flow control) */

	/* Increment connection responses */
	fc_conn->rsp_count++;                                 /* [한국어] 이 connection이 송신한 응답 수 - ersp_ratio 판정 입력 */

	if (nvmf_fc_send_ersp_required(fc_req, fc_conn->rsp_count,
				       fc_req->transferred_len)) {  /* [한국어] ERSP가 필요한지 판정(주기/에러/전송길이 불일치 등) */
		/* Fill ERSP Len */
		to_be16(&ersp_len, (sizeof(struct spdk_nvmf_fc_ersp_iu) /
				    sizeof(uint32_t)));       /* [한국어] ERSP IU 크기를 32bit word 개수로 환산해 빅엔디언으로 - FC는 네트워크 바이트 오더 */
		fc_req->ersp.ersp_len = ersp_len;             /* [한국어] ERSP IU 헤더의 길이 필드 채움 */

		/* Fill RSN */
		to_be32(&fc_req->ersp.response_seq_no, fc_conn->rsn);  /* [한국어] RSN(응답 시퀀스 번호)을 빅엔디언으로 채움 - host가 응답 순서 검증 */
		fc_conn->rsn++;                               /* [한국어] 다음 ERSP를 위해 RSN 증가(connection별 단조 증가) */

		/* Fill transfer length */
		to_be32(&fc_req->ersp.transferred_data_len, fc_req->transferred_len);  /* [한국어] 실제 전송한 데이터 바이트 수 - host가 부분전송 감지 */

		SPDK_DEBUGLOG(nvmf_fc, "Posting ERSP.\n");
		rc = nvmf_fc_xmt_rsp(fc_req, (uint8_t *)&fc_req->ersp,
				     sizeof(struct spdk_nvmf_fc_ersp_iu));  /* [한국어] ERSP IU 전체를 payload로 LLD 송신 큐에 제출 */
	} else {                                              /* [한국어] 경량 RSP만 보내도 충분한 일반 성공 케이스 */
		SPDK_DEBUGLOG(nvmf_fc, "Posting RSP.\n");
		rc = nvmf_fc_xmt_rsp(fc_req, NULL, 0);        /* [한국어] payload 없이 RSP IU만 송신(SQHD는 LLD가 채움) */
	}

	return rc;                                            /* [한국어] 송신 결과 반환 - 실패 시 호출자가 정리 */
}

/*
 * [한국어]
 * nvmf_fc_send_ersp_required - 이번 응답을 ERSP IU로 보내야 하는지 판정한다.
 *
 * @fc_req: 완료된 FC 요청(NVMe CQE/CMD 접근용).
 * @rsp_cnt: 이 connection이 지금까지 보낸 응답 수(ersp_ratio 주기 판정 입력).
 * @xfer_len: 실제 전송된 데이터 길이.
 * @return: true면 ERSP(확장 응답) 필요, false면 경량 RSP로 충분.
 *
 * FC-NVMe는 모든 완료에 전체 CQE를 실어 보내지 않고(대역폭 절약), 다음 조건 중
 * 하나라도 만족할 때만 ERSP를 보낸다(FC-NVMe-2 ERSP 규칙):
 *  1) ersp_ratio마다 1번(주기적 SQHD/RSN 동기화로 host 흐름 제어 보정)
 *  2) Fabric 명령(Connect/Property 등 - 항상 전체 응답 필요)
 *  3) 완료 상태가 에러이거나 CQE의 dw0/dw1(명령별 결과)이 유효
 *  4) 전송 길이가 요청 길이와 다름(부분 전송 - host가 알아야 함)
 * 본 함수는 부수효과 없이 판정만 한다.
 *
 * 실행 컨텍스트: nvmf_fc_handle_rsp와 동일한 HWQP poll group 스레드.
 *
 * 호출 체인:
 *   nvmf_fc_handle_rsp -> [본 함수]
 */
bool
nvmf_fc_send_ersp_required(struct spdk_nvmf_fc_request *fc_req,
			   uint32_t rsp_cnt, uint32_t xfer_len)
{
	struct spdk_nvmf_request *req = &fc_req->req;         /* [한국어] 공통 request - cmd/rsp/length 접근 */
	struct spdk_nvmf_qpair *qpair = req->qpair;           /* [한국어] 소속 qpair */
	struct spdk_nvmf_fc_conn *fc_conn = nvmf_fc_get_conn(qpair);  /* [한국어] connection 복원 - esrp_ratio 접근 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;      /* [한국어] 원본 NVMe 명령 SQE - opcode 검사용 */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;      /* [한국어] NVMe CQE - status/dw0/dw1 검사용 */
	uint16_t status = *((uint16_t *)&rsp->status);        /* [한국어] CQE status 필드를 16bit로 raw 추출(phase bit 포함 비트 검사) */

	/*
	 * Check if we need to send ERSP
	 * 1) For every N responses where N == ersp_ratio
	 * 2) Fabric commands.
	 * 3) Completion status failed or Completion dw0 or dw1 valid.
	 * 4) SQ == 90% full.
	 * 5) Transfer length not equal to CMD IU length
	 */

	if (!(rsp_cnt % fc_conn->esrp_ratio) ||               /* [한국어] 조건1: ersp_ratio 주기마다(나머지 0) ERSP 강제 */
	    (cmd->opc == SPDK_NVME_OPC_FABRIC) ||             /* [한국어] 조건2: Fabric 명령(opc=0x7F)은 전체 응답 필요 */
	    (status & 0xFFFE) || rsp->cdw0 || rsp->cdw1 ||    /* [한국어] 조건3: status 비트(phase bit 제외 0xFFFE) 또는 dw0/dw1 유효 = 에러/결과 있음 */
	    (req->length != xfer_len)) {                      /* [한국어] 조건4: 요청 길이≠전송 길이 = 부분 전송, host에 통지 필요 */
		return true;                                  /* [한국어] 하나라도 해당하면 ERSP 송신 */
	}
	return false;                                         /* [한국어] 모두 미해당 - 경량 RSP로 충분 */
}

/*
 * [한국어]
 * nvmf_fc_request_complete - bdev/subsystem이 명령 처리를 마쳤을 때의 FC측 완료 핸들러 (ops.req_complete).
 *
 * @req: 완료된 공통 NVMe-oF request. req->rsp에 NVMe CQE가 채워진 상태.
 * @return: 없음.
 *
 * NVMe-oF 코어가 spdk_nvmf_request_complete()를 호출하면 트랜스포트 vtable의
 * req_complete 콜백으로 이 함수가 불린다. FC 요청의 상태 머신을 데이터 전송 방향에
 * 따라 다음 단계로 진행시킨다:
 *  - abort 진행 중: 같은 컨텍스트에서 io cleanup을 하면 재진입 위험이 있으므로,
 *    poller API 메시지로 REQ_ABORT_COMPLETE를 deferred 실행(다른 폴 사이클로 미룸).
 *  - Read 성공(controller->host) + 성공 상태: 먼저 데이터를 전송(READ_XFER 상태)해야
 *    하므로 nvmf_fc_send_data()로 FCP_DATA IU를 송신.
 *  - 그 외(Write 완료/Read 에러/No-data): 곧바로 응답 단계로 전이 후 RSP/ERSP 송신.
 *
 * 실행 컨텍스트: bdev 완료 콜백이 도는 HWQP poll group 스레드(요청을 발행한 스레드와 동일).
 *
 * 호출 체인:
 *   bdev completion -> spdk_nvmf_request_complete -> ops.req_complete -> [본 함수]
 *     -> nvmf_fc_send_data / nvmf_fc_handle_rsp / nvmf_fc_poller_api_func
 */
static void
nvmf_fc_request_complete(struct spdk_nvmf_request *req)
{
	int rc = 0;                                           /* [한국어] 데이터/응답 송신 결과 - 실패 시 강제 free */
	struct spdk_nvmf_fc_request *fc_req = nvmf_fc_get_fc_req(req);  /* [한국어] 공통 req에서 FC 요청 컨테이너 복원 */
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;      /* [한국어] NVMe 완료 상태 - 성공 여부 판정 */

	if (fc_req->is_aborted) {                             /* [한국어] 이 요청이 abort 대상이면 정상 완료 경로를 타지 않음 */
		/* Defer this to make sure we dont call io cleanup in same context. */
		nvmf_fc_poller_api_func(fc_req->hwqp, SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE,
					(void *)fc_req);     /* [한국어] abort 완료 처리를 poller 메시지로 미룸 - 같은 스택에서 cleanup 재진입 방지 */
	} else if (spdk_nvme_cpl_is_success(rsp) &&           /* [한국어] 완료 상태가 성공이고 */
		   req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {  /* [한국어] Read(controller->host) 방향이면 데이터부터 전송 */

		nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_READ_XFER);  /* [한국어] 상태머신: READ_XFER(데이터 전송 중)로 전이 */

		rc = nvmf_fc_send_data(fc_req);               /* [한국어] FCP_DATA IU로 read 데이터를 host에 송신(이후 RSP는 별도 콜백) */
	} else {                                              /* [한국어] Write 완료 / Read 에러 / no-data 명령 - 응답만 보내면 됨 */
		if (req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {  /* [한국어] Write(host->controller): 데이터는 이미 받았고 응답만 남음 */
			nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_WRITE_RSP);  /* [한국어] 상태머신: WRITE_RSP로 전이 */
		} else if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {  /* [한국어] Read인데 에러(성공 분기 미진입) - 데이터 없이 응답 */
			nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_READ_RSP);  /* [한국어] 상태머신: READ_RSP로 전이 */
		} else {                                      /* [한국어] 데이터 전송 없는 명령(Flush 등) */
			nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_NONE_RSP);  /* [한국어] 상태머신: NONE_RSP로 전이 */
		}

		rc = nvmf_fc_handle_rsp(fc_req);              /* [한국어] RSP/ERSP IU를 host로 송신 */
	}

	if (rc) {                                             /* [한국어] 데이터/응답 송신 실패 */
		SPDK_ERRLOG("Error in request complete.\n");
		_nvmf_fc_request_free(fc_req);                /* [한국어] 더 진행 불가 - 요청 자원 강제 해제(XCHG/buffer 반환) */
	}
}

/*
 * [한국어]
 * nvmf_fc_get_tgt - 현재 FC 트랜스포트가 바인딩된 NVMe-oF target 포인터를 반환한다.
 *
 * @return: spdk_nvmf_tgt 포인터(transport 생성 시 코어가 채움), FC transport 미생성 시 NULL.
 *
 * FC 관리 평면(LS handler, HW port admin)에서 target 객체(subsystem 조회 등)가 필요할 때
 * 전역 singleton g_nvmf_ftransport를 통해 안전하게 접근하는 헬퍼. transport가 아직
 * 생성되지 않았으면 NULL을 반환하여 호출자가 방어할 수 있게 한다.
 *
 * 실행 컨텍스트: 어느 spdk_thread에서나 호출 가능(읽기 전용, singleton 포인터 비교).
 *
 * 호출 체인:
 *   fc_ls.c / fc admin 경로 -> [본 함수]
 */
struct spdk_nvmf_tgt *
nvmf_fc_get_tgt(void)
{
	if (g_nvmf_ftransport) {                              /* [한국어] FC transport singleton이 생성된 상태인지 확인 */
		return g_nvmf_ftransport->transport.tgt;      /* [한국어] transport 생성 시 코어가 설정한 target 포인터 반환 */
	}
	return NULL;                                          /* [한국어] FC transport 미생성 - 호출자는 NULL 방어 필요 */
}

/*
 * FC Transport Public API begins here
 */

#define SPDK_NVMF_FC_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_FC_DEFAULT_AQ_DEPTH 32
#define SPDK_NVMF_FC_DEFAULT_MAX_QPAIRS_PER_CTRLR 5
#define SPDK_NVMF_FC_DEFAULT_IN_CAPSULE_DATA_SIZE 0
#define SPDK_NVMF_FC_DEFAULT_MAX_IO_SIZE 65536
#define SPDK_NVMF_FC_DEFAULT_IO_UNIT_SIZE 4096
#define SPDK_NVMF_FC_DEFAULT_NUM_SHARED_BUFFERS 8192
#define SPDK_NVMF_FC_DEFAULT_MAX_SGE (SPDK_NVMF_FC_DEFAULT_MAX_IO_SIZE /	\
				      SPDK_NVMF_FC_DEFAULT_IO_UNIT_SIZE)

/*
 * [한국어]
 * nvmf_fc_opts_init - FC 트랜스포트의 기본 옵션값을 채운다 (ops.opts_init 콜백)
 *
 * @opts: 호출자가 zero-init한 옵션 구조체 - 본 함수가 FC 기본값으로 덮어씀
 *
 * RPC nvmf_create_transport 또는 코어 spdk_nvmf_transport_create() 시점에 호출되어
 * 사용자가 명시하지 않은 필드를 FC 권장값으로 채운다. 이후 호출자가 사용자 입력으로
 * 덮어쓸 수 있다.
 *
 * 호출 체인:
 *   spdk_nvmf_transport_create -> transport->ops->opts_init -> [본 함수]
 */
static void
nvmf_fc_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =      SPDK_NVMF_FC_DEFAULT_MAX_QUEUE_DEPTH;   /* [한국어] I/O 큐 깊이 기본값 - FC HBA QD 권장값 */
	opts->max_qpairs_per_ctrlr = SPDK_NVMF_FC_DEFAULT_MAX_QPAIRS_PER_CTRLR; /* [한국어] 컨트롤러당 최대 qpair 수 (admin + IO) */
	opts->in_capsule_data_size = SPDK_NVMF_FC_DEFAULT_IN_CAPSULE_DATA_SIZE; /* [한국어] in-capsule data 한도 - FC는 보통 SGL 사용으로 0 */
	opts->max_io_size =          SPDK_NVMF_FC_DEFAULT_MAX_IO_SIZE;        /* [한국어] 단일 I/O 최대 바이트 - HBA driver 한도와 정렬 */
	opts->io_unit_size =         SPDK_NVMF_FC_DEFAULT_IO_UNIT_SIZE;       /* [한국어] iobuf 분할 단위 - 보통 64KB */
	opts->max_aq_depth =         SPDK_NVMF_FC_DEFAULT_AQ_DEPTH;           /* [한국어] Admin Queue 깊이 기본값 */
	opts->num_shared_buffers =   SPDK_NVMF_FC_DEFAULT_NUM_SHARED_BUFFERS; /* [한국어] iobuf 공유 풀 크기 - poll group 간 공유 */
}

static int nvmf_fc_accept(void *ctx);                                    /* [한국어] forward decl - LS 큐 폴링 poller 콜백, 아래에서 정의 */

/*
 * [한국어]
 * nvmf_fc_create - FC 트랜스포트 인스턴스 생성 (ops.create)
 *
 * @opts: 사용자 옵션 + 기본값 병합본
 * @return: 생성된 transport 포인터, 실패 시 NULL
 *
 * SPDK 프로세스당 단일 FC transport만 허용(g_nvmf_ftransport singleton). 본 함수는:
 *   1. opts 검증 (sge_count, core 수)
 *   2. g_nvmf_fc_main_thread = 현재 spdk_thread 저장 (이후 모든 관리 호출 검증용)
 *   3. accept_poller 등록 (acceptor_poll_rate 주기, LS 큐 폴링)
 *   4. nvmf_fc_lld_init() - 벤더 LLD 초기화
 *
 * 호출 체인:
 *   RPC nvmf_create_transport -> spdk_nvmf_transport_create -> ops->create -> [본 함수]
 */
static struct spdk_nvmf_transport *
nvmf_fc_create(struct spdk_nvmf_transport_opts *opts)
{
	uint32_t sge_count;

	SPDK_INFOLOG(nvmf_fc, "*** FC Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_io_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  max_aq_depth=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr - 1,
		     opts->io_unit_size,
		     opts->max_aq_depth);

	if (g_nvmf_ftransport) {
		SPDK_ERRLOG("Duplicate NVMF-FC transport create request!\n");
		return NULL;
	}

	if (spdk_env_get_last_core() < 1) {
		SPDK_ERRLOG("Not enough cores/threads (%d) to run NVMF-FC transport!\n",
			    spdk_env_get_last_core() + 1);
		return NULL;
	}

	sge_count = opts->max_io_size / opts->io_unit_size;
	if (sge_count > SPDK_NVMF_FC_DEFAULT_MAX_SGE) {
		SPDK_ERRLOG("Unsupported IO Unit size specified, %d bytes\n", opts->io_unit_size);
		return NULL;
	}

	g_nvmf_fc_main_thread = spdk_get_thread();
	g_nvmf_fgroup_count = 0;
	g_nvmf_ftransport = calloc(1, sizeof(*g_nvmf_ftransport));

	if (!g_nvmf_ftransport) {
		SPDK_ERRLOG("Failed to allocate NVMF-FC transport\n");
		return NULL;
	}

	if (pthread_mutex_init(&g_nvmf_ftransport->lock, NULL)) {
		SPDK_ERRLOG("pthread_mutex_init() failed\n");
		free(g_nvmf_ftransport);
		g_nvmf_ftransport = NULL;
		return NULL;
	}

	g_nvmf_ftransport->accept_poller = SPDK_POLLER_REGISTER(nvmf_fc_accept,
					   &g_nvmf_ftransport->transport, opts->acceptor_poll_rate);
	if (!g_nvmf_ftransport->accept_poller) {
		free(g_nvmf_ftransport);
		g_nvmf_ftransport = NULL;
		return NULL;
	}

	/* initialize the low level FC driver */
	nvmf_fc_lld_init();

	return &g_nvmf_ftransport->transport;
}

/*
 * [한국어]
 * nvmf_fc_destroy_done_cb - LLD fini 완료 후 transport free 콜백
 *
 * @cb_arg: 코어가 destroy 호출 시 전달한 인자 (사용자 콜백에 전달됨)
 *
 * nvmf_fc_lld_fini는 비동기이므로, LLD가 자원 해제를 마치면 본 콜백이 호출되어
 * g_nvmf_ftransport singleton을 free하고 사용자 콜백을 호출한다.
 */
static void
nvmf_fc_destroy_done_cb(void *cb_arg)
{
	free(g_nvmf_ftransport);                              /* [한국어] singleton transport 객체 해제 */
	if (g_transport_destroy_done_cb) {                    /* [한국어] 사용자가 destroy 호출 시 콜백을 등록했는지 확인 */
		g_transport_destroy_done_cb(cb_arg);          /* [한국어] 사용자 콜백 호출 - tgt destroy 다음 단계 진행 */
		g_transport_destroy_done_cb = NULL;           /* [한국어] 재호출 방지 - 한 번만 트리거 */
	}
}

/*
 * [한국어]
 * nvmf_fc_destroy - FC 트랜스포트 파기 (ops.destroy)
 *
 * @transport: 파기할 transport 포인터 (g_nvmf_ftransport->transport)
 * @cb_fn: 모든 자원 해제 완료 후 호출될 사용자 콜백
 * @cb_arg: 사용자 콜백 인자
 *
 * 1. 남아있는 poll group들을 모두 free.
 * 2. accept_poller 해제.
 * 3. nvmf_fc_lld_fini(나중에 nvmf_fc_destroy_done_cb 호출) - 비동기 해제 시작.
 * 본 함수는 즉시 반환하나 실제 free는 LLD fini 콜백에서 마무리.
 */
static void
nvmf_fc_destroy(struct spdk_nvmf_transport *transport,
		spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	if (transport) {
		struct spdk_nvmf_fc_poll_group *fgroup, *pg_tmp;

		/* clean up any FC poll groups still around */
		TAILQ_FOREACH_SAFE(fgroup, &g_nvmf_fgroups, link, pg_tmp) {
			TAILQ_REMOVE(&g_nvmf_fgroups, fgroup, link);
			free(fgroup);
		}

		spdk_poller_unregister(&g_nvmf_ftransport->accept_poller);
		g_nvmf_fgroup_count = 0;
		g_transport_destroy_done_cb = cb_fn;

		/* low level FC driver clean up */
		nvmf_fc_lld_fini(nvmf_fc_destroy_done_cb, cb_arg);
	}
}

/*
 * [한국어]
 * nvmf_fc_listen - FC nport listen 등록 (ops.listen)
 *
 * @transport: FC transport
 * @trid: listener의 trid (trtype=FC, traddr=WWN, trsvcid=N_Port_ID 또는 무시)
 * @listen_opts: listener 옵션
 * @return: 항상 0 (FC는 별도 listen 동작 불필요)
 *
 * RDMA/TCP는 socket bind/listen이 필요하나, FC는 HBA가 이미 fabric에 join되어 있고
 * 모든 frame이 LLD를 통해 들어오므로 본 함수는 단순 success 반환.
 * 실제 ACL은 add_listener 시 listener trid를 비교하여 수행.
 */
static int
nvmf_fc_listen(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
	       struct spdk_nvmf_listen_opts *listen_opts)
{
	return 0;                                             /* [한국어] FC는 listen 동작 자체가 없음 - 항상 성공 */
}

/*
 * [한국어]
 * nvmf_fc_stop_listen - FC nport listen 해제 (ops.stop_listen)
 *
 * @transport: FC transport
 * @_trid: 해제할 trid (FC는 사용 안 함)
 *
 * listen이 no-op이므로 stop도 no-op.
 */
static void
nvmf_fc_stop_listen(struct spdk_nvmf_transport *transport,
		    const struct spdk_nvme_transport_id *_trid)
{
}

/*
 * [한국어]
 * nvmf_fc_accept - FC LS 큐 폴링 poller 콜백 (acceptor 역할)
 *
 * @ctx: spdk_nvmf_transport 포인터 (poller 등록 시 전달)
 * @return: SPDK_POLLER_BUSY(이벤트 처리함) 또는 SPDK_POLLER_IDLE
 *
 * FC port 목록을 순회하며 ONLINE 상태인 port의 LS 큐를 폴링한다.
 * LS frame이 발견되면 fc_ls.c의 dispatcher로 전달.
 * 처음 호출 시 nvmf_fc_lld_start()로 LLD를 부팅한다.
 *
 * 호출 체인:
 *   accept_poller (acceptor_poll_rate us 주기) -> [본 함수] -> nvmf_fc_process_queue -> fc_ls.c
 */
static int
nvmf_fc_accept(void *ctx)
{
	struct spdk_nvmf_fc_port *fc_port = NULL;
	uint32_t count = 0;
	static bool start_lld = false;

	if (spdk_unlikely(!start_lld)) {
		start_lld  = true;
		nvmf_fc_lld_start();
	}

	/* poll the LS queue on each port */
	TAILQ_FOREACH(fc_port, &g_spdk_nvmf_fc_port_list, link) {
		if (fc_port->hw_port_status == SPDK_FC_PORT_ONLINE) {
			count += nvmf_fc_process_queue(&fc_port->ls_queue);
		}
	}

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

/*
 * [한국어]
 * nvmf_fc_discover - Discovery Log Page entry의 FC-specific 필드 채우기
 *
 * @transport: FC transport
 * @trid: listener trid (입력)
 * @entry: 출력 - Discovery Log Page entry (NVMe-oF 1.x 5.3)
 *
 * Discovery 컨트롤러에 대한 Get Log Page 응답을 빌드할 때 호출.
 * FC 특화 필드: trtype=FC, adrfam(주소 패밀리: FC), secure_channel(보안 미지정),
 * trsvcid/traddr는 N_Port_ID/WWN 문자열로 패딩 채움.
 */
static void
nvmf_fc_discover(struct spdk_nvmf_transport *transport,
		 struct spdk_nvme_transport_id *trid,
		 struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = (enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC; /* [한국어] NVMe-oF trtype = FC (0x4) */
	entry->adrfam = trid->adrfam;                           /* [한국어] 주소 패밀리 - FC traddr 형식 */
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED; /* [한국어] FC는 보안 채널 미지정 - 자체 보안은 별도 */

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' '); /* [한국어] trsvcid를 ' '로 패딩하여 fixed-size 필드에 복사 */
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');   /* [한국어] traddr(WWN/WWPN 문자열) 패딩 복사 */
}

/*
 * [한국어]
 * nvmf_fc_poll_group_create - poll group별 FC 자원 할당 (ops.poll_group_create)
 *
 * @transport: FC transport
 * @group: 상위 nvmf poll group (poll group의 spdk_thread에서 호출됨)
 * @return: 새로 만든 transport poll group, 실패 시 NULL
 *
 * poll group 생성 시 모든 등록 트랜스포트에 대해 한 번씩 호출. FC는:
 *  1. interrupt mode 미지원 검사
 *  2. spdk_nvmf_fc_poll_group 할당 + TAILQ 초기화
 *  3. g_nvmf_fgroups에 등록 (다음 HWQP가 어느 group으로 갈지 idle 선택용)
 * 실제 HWQP 바인딩은 hw_port_init 이벤트 시점에 nvmf_fc_assign_idlest_poll_group()이 처리.
 */
static struct spdk_nvmf_transport_poll_group *
nvmf_fc_poll_group_create(struct spdk_nvmf_transport *transport,
			  struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_fc_poll_group *fgroup;
	struct spdk_nvmf_fc_transport *ftransport =
		SPDK_CONTAINEROF(transport, struct spdk_nvmf_fc_transport, transport);

	if (spdk_interrupt_mode_is_enabled()) {
		SPDK_ERRLOG("FC transport does not support interrupt mode\n");
		return NULL;
	}

	fgroup = calloc(1, sizeof(struct spdk_nvmf_fc_poll_group));
	if (!fgroup) {
		SPDK_ERRLOG("Unable to alloc FC poll group\n");
		return NULL;
	}

	TAILQ_INIT(&fgroup->hwqp_list);

	pthread_mutex_lock(&ftransport->lock);
	TAILQ_INSERT_TAIL(&g_nvmf_fgroups, fgroup, link);
	g_nvmf_fgroup_count++;
	pthread_mutex_unlock(&ftransport->lock);

	return &fgroup->group;
}

/*
 * [한국어]
 * nvmf_fc_poll_group_destroy - poll group의 FC 자원 해제 (ops.poll_group_destroy)
 *
 * @group: 해제할 transport poll group
 *
 * 1. g_nvmf_fgroups에서 제거 (mutex로 보호 - g_nvmf_fgroup_count도 동시 갱신)
 * 2. fgroup 자체 free
 * HWQP들은 이미 다른 곳에서 다 옮겨졌거나 quiesce되었다고 가정.
 */
static void
nvmf_fc_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_fc_poll_group *fgroup;                 /* [한국어] FC 자체 poll group 컨테이너로 다운캐스트할 변수 */
	struct spdk_nvmf_fc_transport *ftransport =
		SPDK_CONTAINEROF(group->transport, struct spdk_nvmf_fc_transport, transport);
	/* [한국어] group->transport에서 SPDK FC transport 컨테이너로 다운캐스트 - mutex 사용 위함 */

	fgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_fc_poll_group, group); /* [한국어] poll group 다운캐스트 */
	pthread_mutex_lock(&ftransport->lock);                  /* [한국어] g_nvmf_fgroups TAILQ 보호 - 다른 reactor가 동시 add/remove 가능 */
	TAILQ_REMOVE(&g_nvmf_fgroups, fgroup, link);            /* [한국어] 전역 fgroup 목록에서 제거 */
	g_nvmf_fgroup_count--;                                  /* [한국어] 카운터 감소 - 새 HWQP 분배 시 round-robin 모듈로 사용 */
	pthread_mutex_unlock(&ftransport->lock);

	free(fgroup);                                           /* [한국어] poll group 컨테이너 해제 */
}

/*
 * [한국어]
 * nvmf_fc_poll_group_add - qpair(=FC connection)를 poll group에 등록 (ops.poll_group_add)
 *
 * @group: 대상 transport poll group
 * @qpair: 추가할 qpair (Create Association/Connection LS로 만들어진 fc_conn)
 * @return: 0 성공, -1 실패
 *
 * 1. fc_conn 컨테이너 다운캐스트
 * 2. group의 hwqp_list에서 conn이 속한 FC port의 HWQP 찾기
 * 3. nvmf_fc_assign_conn_to_hwqp()로 connection ID 할당
 * 4. admin queue(qid=0)인 경우 assoc_id를 conn_id로 사용 (FC-NVMe-2 8.2.1)
 * 5. POLLER_API_ADD_CONNECTION 메시지로 HWQP에 연결 통보 (cross-thread)
 */
static int
nvmf_fc_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
		       struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_fc_poll_group *fgroup;
	struct spdk_nvmf_fc_conn *fc_conn;
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;
	struct spdk_nvmf_fc_ls_add_conn_api_data *api_data = NULL;
	bool hwqp_found = false;

	fgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_fc_poll_group, group);
	fc_conn  = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);

	TAILQ_FOREACH(hwqp, &fgroup->hwqp_list, link) {
		if (fc_conn->fc_assoc->tgtport->fc_port == hwqp->fc_port) {
			hwqp_found = true;
			break;
		}
	}

	if (!hwqp_found) {
		SPDK_ERRLOG("No valid hwqp found for new QP.\n");
		goto err;
	}

	if (!nvmf_fc_assign_conn_to_hwqp(hwqp,
					 &fc_conn->conn_id,
					 fc_conn->max_queue_depth)) {
		SPDK_ERRLOG("Failed to get a connection id for new QP.\n");
		goto err;
	}

	fc_conn->hwqp = hwqp;

	/* If this is for ADMIN connection, then update assoc ID. */
	if (fc_conn->qpair.qid == 0) {
		fc_conn->fc_assoc->assoc_id = fc_conn->conn_id;
	}

	api_data = &fc_conn->create_opd->u.add_conn;
	nvmf_fc_poller_api_func(hwqp, SPDK_NVMF_FC_POLLER_API_ADD_CONNECTION, &api_data->args);
	return 0;
err:
	return -1;
}

/*
 * [한국어]
 * nvmf_fc_poll_group_poll - poll group의 모든 HWQP 폴링 (ops.poll_group_poll)
 *
 * @group: 폴링 대상 transport poll group
 * @return: 처리한 이벤트 개수
 *
 * SPDK reactor가 무한 루프에서 호출. 본 group에 속한 ONLINE HWQP들을 순회하며
 * nvmf_fc_process_queue()로 LLD가 채워둔 CQ/RQ를 소비한다. 들어온 NVMe-FC capsule은
 * nvmf_fc_hwqp_handle_request()로 dispatched되어 상태 머신 진행.
 *
 * 호출 체인:
 *   reactor poller cb -> nvmf_transport_poll_group_poll -> ops.poll_group_poll -> [본 함수]
 *     -> nvmf_fc_process_queue -> spdk_nvmf_request_exec
 */
static int
nvmf_fc_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	uint32_t count = 0;                                     /* [한국어] 처리한 이벤트 누적 카운터 - return 시 busy/idle 판단 */
	struct spdk_nvmf_fc_poll_group *fgroup;                 /* [한국어] FC poll group 컨테이너 캐스트 */
	struct spdk_nvmf_fc_hwqp *hwqp;                         /* [한국어] 순회용 HWQP 포인터 */

	fgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_fc_poll_group, group); /* [한국어] 다운캐스트 */

	TAILQ_FOREACH(hwqp, &fgroup->hwqp_list, link) {         /* [한국어] 본 poll group이 소유한 모든 HWQP 순회 */
		if (hwqp->state == SPDK_FC_HWQP_ONLINE) {       /* [한국어] OFFLINE/QUIESCED HWQP는 skip - 데이터 없음 */
			count += nvmf_fc_process_queue(hwqp);   /* [한국어] LLD CQ/RQ 소비 - 처리한 이벤트 수 누적 */
		}
	}

	return (int) count;                                     /* [한국어] 0이면 idle, 양수면 busy - reactor가 다음 polling 주기 결정 */
}

/*
 * [한국어]
 * nvmf_fc_request_free - 완료된 req 객체 풀 반환 (ops.req_free)
 *
 * @req: 반환할 NVMe-oF request
 *
 * 본 함수는 정상 완료 경로가 아닌 "예외 정리" 경로에서 호출된다.
 * 일반 정상 완료는 nvmf_fc_request_complete + 상태 머신 진행을 거쳐 별도 free.
 * 본 경로는 abort/cleanup에 사용:
 *  - 아직 abort 진행 중이지 않으면 BDEV_ABORTED 상태로 전환 후 abort 시작
 *  - 이미 abort 중이면 abort_complete로 마무리
 */
static void
nvmf_fc_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fc_request *fc_req = nvmf_fc_get_fc_req(req); /* [한국어] 공통 req에서 FC 컨테이너 추출 (engine_data 또는 container_of) */

	if (!fc_req->is_aborted) {                              /* [한국어] 아직 abort가 진행 중이지 않으면 abort 경로로 진입 */
		nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_BDEV_ABORTED); /* [한국어] 상태를 BDEV_ABORTED로 전환 - trace 기록 */
		nvmf_fc_request_abort(fc_req, true, NULL, NULL); /* [한국어] ABTS 송신 + bdev abort 전파 시작 */
	} else {
		nvmf_fc_request_abort_complete(fc_req);         /* [한국어] 이미 abort 처리 중 - 마무리 단계로 직접 진입 */
	}
}

/*
 * [한국어]
 * nvmf_fc_connection_delete_done_cb - FC connection 삭제가 끝난 뒤 원래 qpair 스레드로 완료를 통지한다.
 *
 * @arg: spdk_nvmf_fc_qpair_remove_ctx 포인터(close_qpair 시 할당된 컨텍스트).
 * @return: 없음.
 *
 * nvmf_fc_delete_connection은 main thread에서 비동기로 진행되는데, 완료 콜백을
 * 호출해야 할 곳은 코어가 qpair 종료를 요청한 원래 spdk_thread다. 따라서 cb_fn을
 * 직접 부르지 않고 spdk_thread_send_msg로 qpair_thread에 전달하여 thread affinity를
 * 지킨다(cross-thread 콜백 직접 호출 금지 원칙).
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(connection 삭제가 완료된 스레드).
 *
 * 호출 체인:
 *   _nvmf_fc_close_qpair / nvmf_fc_delete_connection 완료 -> [본 함수]
 *     -> spdk_thread_send_msg(qpair_thread, cb_fn)
 */
static void
nvmf_fc_connection_delete_done_cb(void *arg)
{
	struct spdk_nvmf_fc_qpair_remove_ctx *fc_ctx = arg;  /* [한국어] close_qpair가 넘긴 컨텍스트(원 스레드/콜백 보관) */

	if (fc_ctx->cb_fn) {                                  /* [한국어] 코어가 종료 완료 콜백을 등록했으면 */
		spdk_thread_send_msg(fc_ctx->qpair_thread, fc_ctx->cb_fn, fc_ctx->cb_ctx);  /* [한국어] 원래 qpair 스레드로 콜백 디스패치 - affinity 보존(직접 호출 금지) */
	}
	free(fc_ctx);                                         /* [한국어] 컨텍스트 객체 해제 - 더 이상 필요 없음 */
}

/*
 * [한국어]
 * _nvmf_fc_close_qpair - main thread 컨텍스트에서 실제 FC connection 종료 작업을 수행한다.
 *
 * @arg: spdk_nvmf_fc_qpair_remove_ctx 포인터.
 * @return: 없음.
 *
 * nvmf_fc_close_qpair가 spdk_thread_send_msg로 main thread에 위임한 본체. connection의
 * 진행 상태에 따라 분기한다:
 *  - conn_id == INVALID: 아직 HWQP에 connection이 등록되지 않은 상태(Add Connection LS가
 *    실패 중). create_opd의 add_conn 정보로 nvmf_fc_ls_add_conn_failure를 호출해
 *    Create Connection을 거부(LS_RJT)하고 자원 정리.
 *  - conn_state == CREATED: 정상 등록된 connection이므로 nvmf_fc_delete_connection으로
 *    HWQP에서 분리·Disconnect 진행. 성공하면 비동기 완료를 기다리며 return.
 * 어느 경로든 끝나면 nvmf_fc_connection_delete_done_cb로 원 스레드에 완료를 통지.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(send_msg로 진입). connection 전역 상태를 만지므로
 * main thread 단일화로 직렬화.
 *
 * 호출 체인:
 *   nvmf_fc_close_qpair -> spdk_thread_send_msg(main) -> [본 함수]
 *     -> nvmf_fc_delete_connection / nvmf_fc_ls_add_conn_failure / nvmf_fc_connection_delete_done_cb
 */
static void
_nvmf_fc_close_qpair(void *arg)
{
	struct spdk_nvmf_fc_qpair_remove_ctx *fc_ctx = arg;  /* [한국어] 종료 컨텍스트(qpair/콜백/원 스레드) */
	struct spdk_nvmf_qpair *qpair = fc_ctx->qpair;       /* [한국어] 종료 대상 공통 qpair */
	struct spdk_nvmf_fc_conn *fc_conn;                   /* [한국어] qpair에서 복원할 FC connection */
	int rc;                                              /* [한국어] delete_connection 반환코드 */

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);  /* [한국어] qpair 임베드로부터 FC connection 컨테이너 복원 */

	SPDK_NOTICELOG("Close qpair %p, fc_conn %p conn_state %d conn_id 0x%lx\n",
		       qpair, fc_conn, fc_conn->conn_state, fc_conn->conn_id);  /* [한국어] 종료 진입 상태 로깅(디버깅) */
	if (fc_conn->conn_id == NVMF_FC_INVALID_CONN_ID) {   /* [한국어] HWQP에 conn_id가 아직 미할당 = Add Connection 진행 중 실패 */
		struct spdk_nvmf_fc_ls_add_conn_api_data *api_data = NULL;  /* [한국어] Add Connection LS 컨텍스트 */

		if (fc_conn->create_opd) {                   /* [한국어] Create Connection 진행 데이터(operation descriptor)가 남아있으면 */
			api_data = &fc_conn->create_opd->u.add_conn;  /* [한국어] add_conn union 멤버 추출 */

			nvmf_fc_ls_add_conn_failure(api_data->assoc, api_data->ls_rqst,
						    api_data->args.fc_conn, api_data->aq_conn);  /* [한국어] Create Connection을 LS_RJT로 거부하고 부분 할당 자원 롤백 */
		}
	} else if (fc_conn->conn_state == SPDK_NVMF_FC_OBJECT_CREATED) {  /* [한국어] 정상 등록된 connection - Disconnect 절차 필요 */
		rc = nvmf_fc_delete_connection(fc_conn, false, true,
					       nvmf_fc_connection_delete_done_cb, fc_ctx);  /* [한국어] HWQP에서 connection 제거 + 완료 시 done_cb 호출(send_disconn=false, backend cleanup=true) */
		if (!rc) {                                   /* [한국어] 삭제 시작 성공(비동기 진행) */
			/* Wait for transport to complete its work. */
			return;                              /* [한국어] done_cb가 나중에 완료 통지하므로 여기서 종료 */
		}

		SPDK_ERRLOG("Delete fc_conn %p failed.\n", fc_conn);  /* [한국어] 삭제 시작 실패 - 아래 fallthrough로 즉시 완료 처리 */
	}

	nvmf_fc_connection_delete_done_cb(fc_ctx);           /* [한국어] INVALID conn 또는 삭제 실패/즉시 완료 케이스 - 원 스레드에 완료 통지 */
}

/*
 * [한국어]
 * nvmf_fc_close_qpair - qpair(=FC connection) 종료 진입점 (ops.qpair_fini).
 *
 * @qpair: 종료할 공통 qpair.
 * @cb_fn: 종료 완료 후 코어가 호출받을 콜백.
 * @cb_arg: cb_fn 인자.
 * @return: 없음.
 *
 * NVMe-oF 코어가 connection을 닫을 때(Disconnect LS, link drop, subsystem 제거 등)
 * 트랜스포트 vtable의 qpair_fini로 호출된다. connection 상태에 따라:
 *  - 이미 TO_BE_DELETED(삭제 진행 중): 실제 종료는 이미 다른 경로가 처리 중이므로
 *    qpair_fini_done_cb(HWQP에 등록)를 즉시 호출하고 코어 콜백도 즉시 호출 후 반환.
 *  - 그 외: remove ctx를 할당해 현재 스레드/콜백을 보관하고, 실제 종료 작업
 *    _nvmf_fc_close_qpair를 main thread로 위임(connection 전역 상태 직렬화).
 *
 * 실행 컨텍스트: qpair가 속한 poll group 스레드(코어가 호출). 실제 작업은 main thread로 위임.
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_disconnect -> ops.qpair_fini -> [본 함수]
 *     -> spdk_thread_send_msg(main, _nvmf_fc_close_qpair)
 */
static void
nvmf_fc_close_qpair(struct spdk_nvmf_qpair *qpair,
		    spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_fc_qpair_remove_ctx *fc_ctx;        /* [한국어] main thread로 넘길 종료 컨텍스트 */
	struct spdk_nvmf_fc_conn *fc_conn;                   /* [한국어] qpair에서 복원할 FC connection */

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);  /* [한국어] qpair 임베드로부터 connection 컨테이너 복원 */
	fc_conn->qpair_fini_done = true;                     /* [한국어] 코어가 qpair_fini를 호출했음을 표시 - 삭제 경로가 이 플래그로 동기 */

	if (fc_conn->conn_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {  /* [한국어] 이미 삭제 진행 중인 connection이면 추가 작업 불필요 */
		if (fc_conn->qpair_fini_done_cb) {           /* [한국어] 삭제 경로가 fini 완료를 기다리며 등록한 콜백이 있으면 */
			SPDK_NOTICELOG("Invoke qpair_fini_done_cb, fc_conn %p conn_id 0x%lx qpair %p conn_state %d\n",
				       fc_conn, fc_conn->conn_id, qpair, fc_conn->conn_state);  /* [한국어] 진단 로그 */

			fc_conn->qpair_fini_done_cb(fc_conn->hwqp, 0, fc_conn->qpair_fini_done_cb_args);  /* [한국어] 삭제 경로에 "코어가 fini 완료"를 통지 - 삭제 마무리 깨우기 */
		}

		if (cb_fn) {                                 /* [한국어] 코어가 넘긴 완료 콜백이 있으면 */
			cb_fn(cb_arg);                       /* [한국어] 즉시 호출 - 이미 종료 진행 중이라 비동기 위임 불필요 */
		}

		return;                                      /* [한국어] 종료 - 별도 위임 없음 */
	}

	fc_ctx = calloc(1, sizeof(struct spdk_nvmf_fc_qpair_remove_ctx));  /* [한국어] 종료 컨텍스트 할당(0 초기화) */
	if (!fc_ctx) {                                       /* [한국어] 메모리 부족 */
		SPDK_ERRLOG("Unable to allocate close_qpair ctx.");
		if (cb_fn) {                                 /* [한국어] 위임 불가하지만 코어 콜백은 반드시 호출해야 함 */
			cb_fn(cb_arg);                       /* [한국어] 즉시 콜백 호출(종료는 실패했지만 코어를 멈추지 않도록) */
		}

		return;                                      /* [한국어] 할당 실패로 종료 */
	}

	fc_ctx->qpair = qpair;                               /* [한국어] 종료 대상 qpair 보관 */
	fc_ctx->cb_fn = cb_fn;                               /* [한국어] 완료 시 호출할 코어 콜백 보관 */
	fc_ctx->cb_ctx = cb_arg;                             /* [한국어] 콜백 인자 보관 */
	fc_ctx->qpair_thread = spdk_get_thread();            /* [한국어] 현재 스레드 기록 - 완료 통지를 이 스레드로 되돌리기 위함(affinity) */

	spdk_thread_send_msg(nvmf_fc_get_main_thread(), _nvmf_fc_close_qpair, fc_ctx);  /* [한국어] 실제 종료를 main thread로 위임 - connection 전역 상태 직렬화 */
}

/*
 * [한국어]
 * nvmf_fc_qpair_get_peer_trid - host(peer)측 FC 주소(trid)를 조회한다 (ops.qpair_get_peer_trid).
 *
 * @qpair: 대상 qpair.
 * @trid: 출력 - peer FC 주소(WWN/N_Port_ID)가 복사될 버퍼.
 * @return: 항상 0(성공).
 *
 * NVMe-oF 코어/RPC가 연결의 상대측 식별자를 알고자 할 때 호출. FC connection은
 * 생성 시 자신의 trid를 보관해 두므로 그 값을 그대로 복사한다. FC에서는 peer/local/
 * listen trid가 모두 connection의 trid로 단순화되어 있다(connection 컨텍스트가 양측 정보 보유).
 *
 * 실행 컨텍스트: qpair 소유 스레드(읽기 전용 memcpy).
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_get_peer_trid -> ops.qpair_get_peer_trid -> [본 함수]
 */
static int
nvmf_fc_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			    struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_fc_conn *fc_conn;                   /* [한국어] qpair에서 복원할 connection */

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);  /* [한국어] connection 컨테이너 복원 */
	memcpy(trid, &fc_conn->trid, sizeof(struct spdk_nvme_transport_id));  /* [한국어] connection이 보관한 FC trid를 출력 버퍼로 복사 */
	return 0;                                            /* [한국어] FC는 항상 성공 */
}

/*
 * [한국어]
 * nvmf_fc_qpair_get_local_trid - target(local)측 FC 주소(trid)를 조회한다 (ops.qpair_get_local_trid).
 *
 * @qpair: 대상 qpair.
 * @trid: 출력 - local FC 주소가 복사될 버퍼.
 * @return: 항상 0(성공).
 *
 * peer 버전과 동일하게 connection의 trid를 복사한다. FC에서는 connection 컨텍스트가
 * local nport와 remote rport 정보를 모두 표현하므로 동일 소스를 사용한다.
 *
 * 실행 컨텍스트: qpair 소유 스레드(읽기 전용).
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_get_local_trid -> ops.qpair_get_local_trid -> [본 함수]
 */
static int
nvmf_fc_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			     struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_fc_conn *fc_conn;                   /* [한국어] qpair에서 복원할 connection */

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);  /* [한국어] connection 컨테이너 복원 */
	memcpy(trid, &fc_conn->trid, sizeof(struct spdk_nvme_transport_id));  /* [한국어] connection trid를 출력 버퍼로 복사 */
	return 0;                                            /* [한국어] 성공 */
}

/*
 * [한국어]
 * nvmf_fc_qpair_get_listen_trid - 이 qpair를 수용한 listener의 FC 주소(trid)를 조회한다 (ops.qpair_get_listen_trid).
 *
 * @qpair: 대상 qpair.
 * @trid: 출력 - listener FC 주소가 복사될 버퍼.
 * @return: 항상 0(성공).
 *
 * 코어가 connection이 어느 listener(nport)에 들어왔는지 알고자 할 때 호출. FC는
 * listener trid도 connection trid와 동일하게 취급하므로 같은 소스를 복사한다.
 *
 * 실행 컨텍스트: qpair 소유 스레드(읽기 전용).
 *
 * 호출 체인:
 *   spdk_nvmf_qpair_get_listen_trid -> ops.qpair_get_listen_trid -> [본 함수]
 */
static int
nvmf_fc_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_fc_conn *fc_conn;                   /* [한국어] qpair에서 복원할 connection */

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);  /* [한국어] connection 컨테이너 복원 */
	memcpy(trid, &fc_conn->trid, sizeof(struct spdk_nvme_transport_id));  /* [한국어] connection trid를 출력 버퍼로 복사 */
	return 0;                                            /* [한국어] 성공 */
}

/*
 * [한국어]
 * nvmf_fc_qpair_abort_request - 특정 요청에 대한 abort 요구를 처리한다 (ops.qpair_abort_request).
 *
 * @qpair: 대상 요청이 속한 qpair.
 * @req: abort 명령 그 자체(Abort admin command를 표현하는 request).
 *
 * 코어가 Abort 명령을 처리할 때 트랜스포트에 위임하는 콜백. FC 구현은 실제 in-flight
 * 명령 취소(ABTS)를 별도의 poller API 경로(nvmf_fc_request_abort)로 처리하고, 여기서는
 * Abort 명령 자체를 즉시 완료 처리한다(코어가 Abort 결과를 host에 보내도록).
 *
 * 실행 컨텍스트: qpair 소유 스레드.
 *
 * 호출 체인:
 *   nvmf_ctrlr_abort_request -> ops.qpair_abort_request -> [본 함수]
 *     -> spdk_nvmf_request_complete
 */
static void
nvmf_fc_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
			    struct spdk_nvmf_request *req)
{
	spdk_nvmf_request_complete(req);                     /* [한국어] Abort 명령 자체를 즉시 완료 - 실제 in-flight 취소는 별도 ABTS 경로 담당 */
}

/* [한국어]
 * spdk_nvmf_transport_fc - SPDK NVMe-oF FC 트랜스포트의 vtable.
 * spdk_nvmf_transport_register("FC") 시점에 등록되어, 코어가 spdk_nvmf_transport_ops_lookup
 * 으로 검색해 사용한다. 각 ops 멤버는 transport.h의 thunk 함수에 의해 호출된다.
 * 멤버 매핑은 lib/nvmf/transport.c의 dispatch 헬퍼와 1:1 대응. */
const struct spdk_nvmf_transport_ops spdk_nvmf_transport_fc = {
	.name = "FC",                               /* [한국어] 사용자에게 노출되는 트랜스포트 이름 - RPC/JSON에서 "FC"로 식별 */
	.type = (enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC, /* [한국어] NVMe-oF trtype 코드 = 0x4 (NVMe-oF Spec Fig.) - discovery log entry의 trtype 필드값 */
	.opts_init = nvmf_fc_opts_init,             /* [한국어] 트랜스포트 기본 옵션(io_unit_size 등) 초기화 콜백 */
	.create = nvmf_fc_create,                   /* [한국어] 트랜스포트 인스턴스 생성 - spdk_nvmf_tgt_add_transport 시 호출 */
	.destroy = nvmf_fc_destroy,                 /* [한국어] 트랜스포트 인스턴스 파기 - tgt destroy 경로에서 호출 */

	.listen = nvmf_fc_listen,                   /* [한국어] FC nport 단위 listen 등록 (실제 FC에서는 nport binding 의미) */
	.stop_listen = nvmf_fc_stop_listen,         /* [한국어] FC nport listen 해제 */

	.listener_discover = nvmf_fc_discover,      /* [한국어] Discovery Log Page entry 채우기 (trtype/adrfam/traddr 등) */

	.poll_group_create = nvmf_fc_poll_group_create,   /* [한국어] poll group별 FC 자원(HWQP 매핑) 할당 */
	.poll_group_destroy = nvmf_fc_poll_group_destroy, /* [한국어] poll group 종료 - HWQP를 다른 group으로 이관 또는 quiesce */
	.poll_group_add = nvmf_fc_poll_group_add,         /* [한국어] qpair(=FC connection)를 poll group에 등록 */
	.poll_group_poll = nvmf_fc_poll_group_poll,       /* [한국어] reactor 주기 호출 - LLD의 RQ/CQ 폴링 진입점 */

	.req_complete = nvmf_fc_request_complete,   /* [한국어] NVMe 명령 응답(RSP IU) 송신 */
	.req_free = nvmf_fc_request_free,           /* [한국어] 완료된 req 객체 풀 반환 */
	.qpair_fini = nvmf_fc_close_qpair,          /* [한국어] FC connection 종료 (Disconnect LS 또는 link drop 시) */
	.qpair_get_peer_trid = nvmf_fc_qpair_get_peer_trid,   /* [한국어] 호스트 FC port의 WWN/N_Port_ID 조회 */
	.qpair_get_local_trid = nvmf_fc_qpair_get_local_trid, /* [한국어] target nport의 WWN/N_Port_ID 조회 */
	.qpair_get_listen_trid = nvmf_fc_qpair_get_listen_trid, /* [한국어] qpair를 수용한 listener의 trid 조회 */
	.qpair_abort_request = nvmf_fc_qpair_abort_request,   /* [한국어] 특정 req abort - ABTS 송신 또는 내부 cleanup */
};

/* Initializes the data for the creation of a FC-Port object in the SPDK
 * library. The spdk_nvmf_fc_port is a well defined structure that is part of
 * the API to the library. The contents added to this well defined structure
 * is private to each vendors implementation.
 */
/*
 * [한국어]
 * nvmf_fc_adm_hw_port_data_init - HBA의 물리 FC 포트를 표현하는 spdk_nvmf_fc_port 객체를 초기화한다.
 *
 * @fc_port: 초기화할 FC 포트 객체(상위에서 할당됨).
 * @args: HW port init RPC/이벤트가 전달한 인자(port_handle, LLD 큐 핸들, IO 큐 개수 등).
 * @return: 0 성공, 음수면 HWQP 초기화 실패(부분 자원 롤백 후 반환).
 *
 * "HW Port Init"은 FC HBA 드라이버(LLD)가 새 물리 포트를 SPDK에 등록할 때의 첫 단계다.
 * 이 함수는 한 물리 포트에 대해 1개의 LS(Link Service) HWQP와 N개의 I/O HWQP를 만든다:
 *  - LS 큐: Create Association/Connection 같은 제어 프레임 처리 전용. 항상 main thread에
 *    바인딩되며 hwqp_id를 IO 큐와 겹치지 않게 높은 값으로 부여(트레이스 가독성).
 *  - I/O 큐: 실제 FCP 명령(read/write)을 처리. 나중에 poll group에 분배됨.
 * 각 HWQP는 nvmf_fc_init_hwqp로 connection/rport 해시 테이블(rte_hash) 등을 만든다.
 * 중간에 실패하면 지금까지 만든 해시 테이블을 역순으로 모두 해제(자원 누수 방지).
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(HW port admin 경로). 포트 전역 자료구조 초기화.
 *
 * 호출 체인:
 *   nvmf_fc_adm_evnt_hw_port_init -> [본 함수] -> nvmf_fc_init_hwqp / nvmf_fc_ls_init
 */
static int
nvmf_fc_adm_hw_port_data_init(struct spdk_nvmf_fc_port *fc_port,
			      struct spdk_nvmf_fc_hw_port_init_args *args)
{
	int rc = 0;                                           /* [한국어] HWQP 초기화 반환코드 - 실패 시 롤백 분기 */
	/* Used a high number for the LS HWQP so that it does not clash with the
	 * IO HWQP's and immediately shows a LS queue during tracing.
	 */
	uint32_t i;                                           /* [한국어] I/O 큐 초기화 루프 인덱스(롤백에도 재사용) */

	fc_port->port_hdl       = args->port_handle;         /* [한국어] LLD가 부여한 포트 핸들 보관 - 이후 이벤트 매칭 키 */
	fc_port->lld_fc_port	= args->lld_fc_port;         /* [한국어] LLD 비공개 포트 컨텍스트 포인터(벤더별) */
	fc_port->hw_port_status = SPDK_FC_PORT_OFFLINE;      /* [한국어] 초기 상태는 OFFLINE - online 이벤트 전까지 프레임 처리 안 함 */
	fc_port->fcp_rq_id      = args->fcp_rq_id;           /* [한국어] FCP receive queue ID(LLD가 데이터를 채우는 RQ 식별자) */
	fc_port->num_io_queues  = args->io_queue_cnt;        /* [한국어] 이 포트의 I/O HWQP 개수 보관 */

	/*
	 * Set port context from init args. Used for FCP port stats.
	 */
	fc_port->port_ctx = args->port_ctx;                  /* [한국어] FCP 포트 통계용 LLD 컨텍스트 보관 */

	/*
	 * Initialize the LS queue wherever needed.
	 */
	fc_port->ls_queue.queues = args->ls_queue;           /* [한국어] LS HWQP에 LLD 큐 핸들 연결 */
	fc_port->ls_queue.thread = nvmf_fc_get_main_thread();  /* [한국어] LS 큐는 main thread에 고정 - 제어 프레임 직렬 처리 */
	fc_port->ls_queue.hwqp_id = SPDK_MAX_NUM_OF_FC_PORTS * fc_port->num_io_queues;  /* [한국어] IO 큐 ID와 겹치지 않는 큰 값 부여 - 트레이스에서 LS 큐 즉시 식별 */
	fc_port->ls_queue.is_ls_queue = true;                /* [한국어] 이 HWQP가 LS 전용임을 표시 - pending_buf_queue 미사용 분기 등에 사용 */

	/*
	 * Initialize the LS queue.
	 */
	rc = nvmf_fc_init_hwqp(fc_port, &fc_port->ls_queue); /* [한국어] LS HWQP의 connection/rport 해시·통계 등 초기화 */
	if (rc) {                                            /* [한국어] LS 큐 초기화 실패면 IO 큐 만들기 전에 즉시 반환 */
		return rc;
	}

	/*
	 * Initialize the IO queues.
	 */
	for (i = 0; i < args->io_queue_cnt; i++) {           /* [한국어] 요청된 개수만큼 I/O HWQP 초기화 */
		struct spdk_nvmf_fc_hwqp *hwqp = &fc_port->io_queues[i];  /* [한국어] i번째 I/O HWQP 슬롯 */
		hwqp->hwqp_id = i;                           /* [한국어] I/O 큐 ID = 0..N-1(LS 큐와 구분) */
		hwqp->queues = args->io_queues[i];           /* [한국어] LLD가 제공한 i번째 큐 핸들 연결 */
		hwqp->is_ls_queue = false;                   /* [한국어] I/O 큐 표시 - pending_buf_queue 사용 대상 */
		rc = nvmf_fc_init_hwqp(fc_port, hwqp);       /* [한국어] I/O HWQP 해시/통계 초기화 */
		if (rc) {                                    /* [한국어] 중간 실패 - 지금까지 만든 자원 롤백 필요 */
			for (; i > 0; --i) {                 /* [한국어] 직전까지 성공한 I/O 큐들의 해시 테이블 역순 해제 */
				rte_hash_free(fc_port->io_queues[i - 1].connection_list_hash);  /* [한국어] DPDK rte_hash: connection lookup 테이블 해제 */
				rte_hash_free(fc_port->io_queues[i - 1].rport_list_hash);       /* [한국어] DPDK rte_hash: rport lookup 테이블 해제 */
			}
			rte_hash_free(fc_port->ls_queue.connection_list_hash);  /* [한국어] LS 큐 해시도 해제(이미 성공해 있었음) */
			rte_hash_free(fc_port->ls_queue.rport_list_hash);
			return rc;                           /* [한국어] 롤백 후 에러 반환 */
		}
	}

	/*
	 * Initialize the LS processing for port
	 */
	nvmf_fc_ls_init(fc_port);                            /* [한국어] fc_ls.c의 포트별 LS 처리 상태 초기화(association/connection 카운터 등) */

	/*
	 * Initialize the list of nport on this HW port.
	 */
	TAILQ_INIT(&fc_port->nport_list);                    /* [한국어] 이 물리 포트에 매핑될 nport(가상 target port) 리스트 초기화 */
	fc_port->num_nports = 0;                             /* [한국어] 아직 nport 없음 */

	return 0;                                            /* [한국어] 포트 초기화 성공 */
}

/*
 * FC port must have all its nports deleted before transitioning to offline state.
 */
/*
 * [한국어]
 * nvmf_fc_adm_hw_port_offline_nport_delete - 포트를 offline 전환하기 전 잔존 nport를 zombie 처리한다.
 *
 * @fc_port: offline으로 전환 중인 FC 포트.
 * @return: 없음.
 *
 * FC 포트는 모든 nport(가상 target port)가 정상 삭제된 뒤에야 offline이 되어야 한다.
 * 정상 경로라면 nport_list가 비어 있어야 하므로 DEV_VERIFY로 불변식을 단언한다.
 * 그래도 만약 nport가 남아 있다면(비정상) 각각을 ZOMBIE 상태로 표시하여, 이후
 * 처리에서 유효 객체로 오인되지 않게 한다(방어적 정리).
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(HW port offline 경로).
 *
 * 호출 체인:
 *   nvmf_fc_adm_evnt_hw_port_offline -> [본 함수] -> nvmf_fc_nport_set_state
 */
static void
nvmf_fc_adm_hw_port_offline_nport_delete(struct spdk_nvmf_fc_port *fc_port)
{
	struct spdk_nvmf_fc_nport *nport = NULL;             /* [한국어] 잔존 nport 순회 커서 */
	/* All nports must have been deleted at this point for this fc port */
	DEV_VERIFY(fc_port && TAILQ_EMPTY(&fc_port->nport_list));  /* [한국어] 불변식 단언: offline 시점엔 nport가 없어야 함(디버그 빌드 abort) */
	DEV_VERIFY(fc_port->num_nports == 0);                /* [한국어] 불변식 단언: nport 카운터도 0이어야 함 */
	/* Mark the nport states to be zombie, if they exist */
	if (fc_port && !TAILQ_EMPTY(&fc_port->nport_list)) { /* [한국어] (방어) 그럼에도 nport가 남아 있으면 */
		TAILQ_FOREACH(nport, &fc_port->nport_list, link) {  /* [한국어] 잔존 nport 전부 순회 */
			(void)nvmf_fc_nport_set_state(nport, SPDK_NVMF_FC_OBJECT_ZOMBIE);  /* [한국어] ZOMBIE로 표시 - 이후 유효 객체로 처리되지 않도록 */
		}
	}
}

/*
 * [한국어]
 * nvmf_fc_adm_i_t_delete_cb - I-T(Initiator-Target) nexus 삭제 완료 후 사용자에게 통지하는 콜백.
 *
 * @args: spdk_nvmf_fc_adm_i_t_del_cb_data 포인터(nport/rport/사용자 콜백 보관).
 * @err: 삭제 결과(0 성공, 그 외 실패).
 * @return: 없음.
 *
 * I-T delete는 한 host(rport)와 한 target(nport) 사이의 모든 association을 제거하는
 * 작업이다(예: host가 로그아웃). 그 작업이 끝나면 본 콜백이 호출되어 RPC를 발행한
 * 사용자/LLD에게 SPDK_FC_IT_DELETE 이벤트로 완료를 알린다. 실패 시 DEV_VERIFY로
 * 디버그 빌드에서 abort(설계상 발생하면 안 되는 경로). 마지막에 진단 로그를 남긴다.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(ASSERT_SPDK_FC_MAIN_THREAD로 강제 검증).
 *
 * 호출 체인:
 *   nvmf_fc_adm_i_t_delete_assoc 완료 -> [본 함수] -> 사용자 fc_cb_func
 */
static void
nvmf_fc_adm_i_t_delete_cb(void *args, uint32_t err)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] 본 콜백이 반드시 main thread에서 도는지 단언 - 전역 자료구조 직렬화 보장 */
	struct spdk_nvmf_fc_adm_i_t_del_cb_data *cb_data = args;  /* [한국어] 콜백 컨텍스트 복원 */
	struct spdk_nvmf_fc_nport *nport = cb_data->nport;  /* [한국어] 삭제 대상 target nport */
	struct spdk_nvmf_fc_remote_port_info *rport = cb_data->rport;  /* [한국어] 삭제 대상 host rport */
	spdk_nvmf_fc_callback cb_func = cb_data->fc_cb_func; /* [한국어] 사용자가 등록한 최종 완료 콜백 */
	int spdk_err = 0;                                    /* [한국어] 사용자 콜백에 전달할 SPDK 에러코드(여기선 항상 0) */
	uint8_t port_handle = cb_data->port_handle;          /* [한국어] 물리 포트 핸들(로그/콜백 인자) */
	uint32_t s_id = rport->s_id;                         /* [한국어] host의 FC Source ID - 로그용으로 미리 복사(아래 free 후에도 사용) */
	uint32_t rpi = rport->rpi;                           /* [한국어] Remote Port Index - 로그용 사본 */
	uint32_t assoc_count = rport->assoc_count;           /* [한국어] rport의 남은 association 수 - 로그용 사본 */
	uint32_t nport_hdl = nport->nport_hdl;               /* [한국어] nport 핸들 - 로그용 사본 */
	uint32_t d_id = nport->d_id;                         /* [한국어] target의 FC Destination ID - 로그용 사본 */
	char log_str[256];                                   /* [한국어] 진단 로그 조립 버퍼 */

	/*
	 * Assert on any delete failure.
	 */
	if (0 != err) {                                      /* [한국어] 삭제 실패면(설계상 발생 불가) */
		DEV_VERIFY(!"Error in IT Delete callback.");  /* [한국어] 디버그 빌드 abort - 버그 조기 발견 */
		goto out;                                    /* [한국어] 콜백 생략하고 정리만 */
	}

	if (cb_func != NULL) {                               /* [한국어] 사용자 완료 콜백이 등록되어 있으면 */
		(void)cb_func(port_handle, SPDK_FC_IT_DELETE, cb_data->fc_cb_ctx, spdk_err);  /* [한국어] IT delete 완료를 사용자에게 통지(이벤트 타입 + ctx + 결과) */
	}

out:
	free(cb_data);                                       /* [한국어] 콜백 컨텍스트 해제 - 그래서 위에서 필드를 미리 사본으로 떠둠 */

	snprintf(log_str, sizeof(log_str),
		 "IT delete assoc_cb on nport %d done, port_handle:%d s_id:%d d_id:%d rpi:%d rport_assoc_count:%d rc = %d.\n",
		 nport_hdl, port_handle, s_id, d_id, rpi, assoc_count, err);  /* [한국어] 진단 문자열 조립(미리 떠둔 사본 사용) */

	if (err != 0) {                                      /* [한국어] 실패면 에러 레벨 로그 */
		SPDK_ERRLOG("%s", log_str);
	} else {                                             /* [한국어] 성공이면 디버그 레벨 로그 */
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "%s", log_str);
	}
}

/*
 * [한국어]
 * nvmf_fc_adm_i_t_delete_assoc_cb - I-T 삭제 중 개별 association 삭제 완료 콜백(참조 카운팅).
 *
 * @args: spdk_nvmf_fc_adm_i_t_del_assoc_cb_data 포인터.
 * @err: 이 association 삭제 결과(0 성공).
 * @return: 없음.
 *
 * 한 rport(I-T nexus)에는 여러 association이 있을 수 있다. nvmf_fc_adm_i_t_delete_assoc가
 * 각 association마다 비동기 삭제를 걸고, 삭제될 때마다 본 콜백이 호출되어 rport의
 * assoc_count를 사실상 감소시킨다(실제 감소는 delete_association 경로에서; 여기선 0 도달
 * 시점을 감지). 마지막 association이 사라지면(assoc_count==0) rport를 remote port 리스트에서
 * 제거하고 상위 콜백을 호출한 뒤 rport와 콜백 컨텍스트를 free한다.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(ASSERT로 검증). rport/nport 카운터는 main thread
 * 단일화로 직렬 갱신되어 락 불필요.
 *
 * 호출 체인:
 *   nvmf_fc_delete_association 완료 -> [본 함수] -> 상위 cb_func(예: nvmf_fc_adm_i_t_delete_cb)
 */
static void
nvmf_fc_adm_i_t_delete_assoc_cb(void *args, uint32_t err)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 - rport 카운터 직렬 접근 보장 */
	struct spdk_nvmf_fc_adm_i_t_del_assoc_cb_data *cb_data = args;  /* [한국어] 콜백 컨텍스트 복원 */
	struct spdk_nvmf_fc_nport *nport = cb_data->nport;  /* [한국어] 대상 target nport */
	struct spdk_nvmf_fc_remote_port_info *rport = cb_data->rport;  /* [한국어] 대상 host rport */
	spdk_nvmf_fc_adm_i_t_delete_assoc_cb_fn cb_func = cb_data->cb_func;  /* [한국어] 모든 association 삭제 완료 시 호출할 상위 콜백 */
	uint32_t s_id = rport->s_id;                         /* [한국어] 로그용 사본(아래 free 전 보관) */
	uint32_t rpi = rport->rpi;                           /* [한국어] 로그용 사본 */
	uint32_t assoc_count = rport->assoc_count;           /* [한국어] 로그용 사본(진입 시점 값) */
	uint32_t nport_hdl = nport->nport_hdl;               /* [한국어] 로그용 사본 */
	uint32_t d_id = nport->d_id;                         /* [한국어] 로그용 사본 */
	char log_str[256];                                   /* [한국어] 진단 로그 버퍼 */

	/*
	 * Assert on any association delete failure. We continue to delete other
	 * associations in promoted builds.
	 */
	if (0 != err) {                                      /* [한국어] association 삭제 실패(설계상 발생 불가) */
		DEV_VERIFY(!"Nport's association delete callback returned error");  /* [한국어] 디버그 빌드 abort */
		if (nport->assoc_count > 0) {                /* [한국어] 릴리즈 빌드에선 계속 진행 - nport 카운터 보정 감소 */
			nport->assoc_count--;
		}
		if (rport->assoc_count > 0) {                /* [한국어] rport 카운터도 보정 감소(0 도달 감지 위함) */
			rport->assoc_count--;
		}
	}

	/*
	 * If this is the last association being deleted for the ITN,
	 * execute the callback(s).
	 */
	if (0 == rport->assoc_count) {                       /* [한국어] 이 rport의 마지막 association이 사라짐 - I-T nexus 종료 */
		/* Remove the rport from the remote port list. */
		if (nvmf_fc_nport_remove_rem_port(nport, rport) != 0) {  /* [한국어] nport의 rport 리스트에서 제거 시도 */
			SPDK_ERRLOG("Error while removing rport from list.\n");
			DEV_VERIFY(!"Error while removing rport from list.");  /* [한국어] 제거 실패는 자료구조 손상 - 디버그 abort */
		}

		if (cb_func != NULL) {                       /* [한국어] 상위 완료 콜백이 있으면 */
			/*
			 * Callback function is provided by the caller
			 * of nvmf_fc_adm_i_t_delete_assoc().
			 */
			(void)cb_func(cb_data->cb_ctx, 0);   /* [한국어] I-T delete 전체 완료를 상위에 통지 */
		}
		free(rport);                                 /* [한국어] rport 객체 해제(더 이상 참조 없음) */
		free(args);                                  /* [한국어] 콜백 컨텍스트 해제 */
	}

	snprintf(log_str, sizeof(log_str),
		 "IT delete assoc_cb on nport %d done, s_id:%d d_id:%d rpi:%d rport_assoc_count:%d err = %d.\n",
		 nport_hdl, s_id, d_id, rpi, assoc_count, err);  /* [한국어] 진단 문자열 조립(사본 사용) */

	if (err != 0) {                                      /* [한국어] 실패 로그 */
		SPDK_ERRLOG("%s", log_str);
	} else {                                             /* [한국어] 성공 디버그 로그 */
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "%s", log_str);
	}
}

/**
 * Process a IT delete.
 */
/*
 * [한국어]
 * nvmf_fc_adm_i_t_delete_assoc - 한 I-T nexus(rport)에 속한 모든 association을 삭제 스케줄한다.
 *
 * @nport: 대상 target nport.
 * @rport: 삭제할 host rport(I-T nexus의 initiator측).
 * @cb_func: 모든 association 삭제 완료 시 호출할 콜백.
 * @cb_ctx: cb_func 인자.
 * @return: 없음(결과는 콜백으로 비동기 통지).
 *
 * I-T delete의 핵심 로직. nport에 등록된 association들 중 이 rport(s_id 일치)에 속한
 * 것을 모두 찾아 nvmf_fc_delete_association로 비동기 삭제를 건다. 삭제마다
 * nvmf_fc_adm_i_t_delete_assoc_cb가 호출되어 rport->assoc_count를 줄이고, 0이 되면
 * 상위 cb_func가 호출된다.
 * 만약 삭제 스케줄이 하나도 잡히지 않으면(매칭 association 없음) 콜백이 영원히 안 불릴
 * 것이므로, 여기서 직접 nvmf_fc_adm_i_t_delete_assoc_cb를 호출해 완료를 보장한다.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread. association 리스트 직렬 순회.
 *
 * 호출 체인:
 *   nvmf_fc_adm_evnt_i_t_delete -> [본 함수] -> nvmf_fc_delete_association
 *     -> (각 완료) nvmf_fc_adm_i_t_delete_assoc_cb
 */
static void
nvmf_fc_adm_i_t_delete_assoc(struct spdk_nvmf_fc_nport *nport,
			     struct spdk_nvmf_fc_remote_port_info *rport,
			     spdk_nvmf_fc_adm_i_t_delete_assoc_cb_fn cb_func,
			     void *cb_ctx)
{
	int err = 0;                                         /* [한국어] 전체 작업 에러 누적 - 로그/조기종료용 */
	struct spdk_nvmf_fc_association *assoc = NULL;       /* [한국어] association 순회 커서 */
	int assoc_err = 0;                                   /* [한국어] 개별 delete_association 반환코드 */
	uint32_t num_assoc = 0;                              /* [한국어] 순회한 association 총수(로그용) */
	uint32_t num_assoc_del_scheduled = 0;                /* [한국어] 실제 삭제 스케줄된 수 - 0이면 콜백 직접 호출 필요 */
	struct spdk_nvmf_fc_adm_i_t_del_assoc_cb_data *cb_data = NULL;  /* [한국어] 모든 삭제가 공유하는 콜백 컨텍스트(refcount 추적) */
	uint8_t port_hdl = nport->port_hdl;                  /* [한국어] 물리 포트 핸들 사본 */
	uint32_t s_id = rport->s_id;                         /* [한국어] 삭제 대상 host의 S_ID - association 매칭 키 */
	uint32_t rpi = rport->rpi;                           /* [한국어] 로그용 사본 */
	uint32_t assoc_count = rport->assoc_count;           /* [한국어] 진입 시점 association 수(로그용) */
	char log_str[256];                                   /* [한국어] 진단 로그 버퍼 */

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "IT delete associations on nport:%d begin.\n",
		      nport->nport_hdl);

	/*
	 * Allocate memory for callback data.
	 * This memory will be freed by the callback function.
	 */
	cb_data = calloc(1, sizeof(struct spdk_nvmf_fc_adm_i_t_del_assoc_cb_data));  /* [한국어] 콜백 컨텍스트 할당(콜백이 free) */
	if (NULL == cb_data) {                               /* [한국어] 메모리 부족 - 작업 불가 */
		SPDK_ERRLOG("Failed to allocate memory for cb_data on nport:%d.\n", nport->nport_hdl);
		err = -ENOMEM;
		goto out;
	}
	cb_data->nport       = nport;                        /* [한국어] 콜백이 참조할 nport */
	cb_data->rport       = rport;                        /* [한국어] 콜백이 참조할 rport(완료 시 리스트에서 제거·free) */
	cb_data->port_handle = port_hdl;                     /* [한국어] 물리 포트 핸들 */
	cb_data->cb_func     = cb_func;                      /* [한국어] 상위 완료 콜백 */
	cb_data->cb_ctx      = cb_ctx;                       /* [한국어] 상위 콜백 인자 */

	/*
	 * Delete all associations, if any, related with this ITN/remote_port.
	 */
	TAILQ_FOREACH(assoc, &nport->fc_associations, link) {  /* [한국어] nport의 모든 association 순회 */
		num_assoc++;                                /* [한국어] 순회 카운트 증가 */
		if (assoc->s_id == s_id) {                  /* [한국어] 이 rport(s_id)에 속한 association만 대상 */
			assoc_err = nvmf_fc_delete_association(nport,
							       assoc->assoc_id,
							       false /* send abts */, false,
							       nvmf_fc_adm_i_t_delete_assoc_cb, cb_data);  /* [한국어] association 비동기 삭제(ABTS 미송신, backend cleanup 없음, 완료콜백 공유) */
			if (0 != assoc_err) {               /* [한국어] 삭제 스케줄 실패 */
				/*
				 * Mark this association as zombie.
				 */
				err = -EINVAL;
				DEV_VERIFY(!"Error while deleting association");  /* [한국어] 디버그 abort */
				(void)nvmf_fc_assoc_set_state(assoc, SPDK_NVMF_FC_OBJECT_ZOMBIE);  /* [한국어] 삭제 못한 association을 ZOMBIE로 격리 */
			} else {                            /* [한국어] 삭제 스케줄 성공 */
				num_assoc_del_scheduled++;  /* [한국어] 콜백이 한 번 더 불릴 것이므로 카운트 */
			}
		}
	}

out:
	if ((cb_data) && (num_assoc_del_scheduled == 0)) {  /* [한국어] 콜백 컨텍스트는 있는데 스케줄된 삭제가 0개 */
		/*
		 * Since there are no association_delete calls
		 * successfully scheduled, the association_delete
		 * callback function will never be called.
		 * In this case, call the callback function now.
		 */
		nvmf_fc_adm_i_t_delete_assoc_cb(cb_data, 0);  /* [한국어] 콜백이 자동 호출될 일이 없으므로 직접 호출해 완료 보장 */
	}

	snprintf(log_str, sizeof(log_str),
		 "IT delete associations on nport:%d end. "
		 "s_id:%d rpi:%d assoc_count:%d assoc:%d assoc_del_scheduled:%d rc:%d.\n",
		 nport->nport_hdl, s_id, rpi, assoc_count, num_assoc, num_assoc_del_scheduled, err);  /* [한국어] 작업 요약 로그 조립 */

	if (err == 0) {                                     /* [한국어] 성공 디버그 로그 */
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "%s", log_str);
	} else {                                            /* [한국어] 실패 에러 로그 */
		SPDK_ERRLOG("%s", log_str);
	}
}

/*
 * [한국어]
 * nvmf_fc_adm_queue_quiesce_cb - 개별 HWQP quiesce 완료 콜백(전체 포트 quiesce 집계).
 *
 * @cb_data: spdk_nvmf_fc_poller_api_quiesce_queue_args 포인터(어느 HWQP가 멈췄는지).
 * @ret: poller API 결과(여기선 사용 안 함).
 * @return: 없음.
 *
 * HW port quiesce는 한 포트의 LS 큐 + 모든 I/O 큐를 polling으로 "더 이상 새 작업을
 * 시작하지 않는" 정지 상태로 만든다. 각 HWQP가 멈출 때마다 본 콜백이 호출되어
 * port_quiesce_ctx->quiesce_count를 감소시킨다. 모든 큐(LS+IO)가 멈춰 count가 0이 되면
 * 포트 상태를 QUIESCED로 바꾸고 사용자 콜백을 호출한 뒤 컨텍스트를 free한다.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(ASSERT로 검증). quiesce_count 직렬 감소.
 *
 * 호출 체인:
 *   poller QUIESCE_QUEUE 완료 -> [본 함수] -> 사용자 cb_func(마지막 큐일 때만)
 */
static void
nvmf_fc_adm_queue_quiesce_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 - quiesce_count 직렬 갱신 보장 */
	struct spdk_nvmf_fc_poller_api_quiesce_queue_args *quiesce_api_data = NULL;  /* [한국어] 멈춘 HWQP 정보 */
	struct spdk_nvmf_fc_adm_hw_port_quiesce_ctx *port_quiesce_ctx = NULL;  /* [한국어] 포트 전체 quiesce 집계 컨텍스트 */
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;               /* [한국어] 멈춘 HWQP */
	struct spdk_nvmf_fc_port *fc_port = NULL;            /* [한국어] 해당 포트 */
	int err = 0;                                         /* [한국어] 사용자 콜백에 전달할 결과 */

	quiesce_api_data = (struct spdk_nvmf_fc_poller_api_quiesce_queue_args *)cb_data;  /* [한국어] poller가 넘긴 인자 캐스트 */
	hwqp = quiesce_api_data->hwqp;                       /* [한국어] 멈춘 HWQP 추출 */
	fc_port = hwqp->fc_port;                             /* [한국어] HWQP가 속한 포트 */
	port_quiesce_ctx = (struct spdk_nvmf_fc_adm_hw_port_quiesce_ctx *)quiesce_api_data->ctx;  /* [한국어] 포트 집계 컨텍스트 추출 */
	spdk_nvmf_fc_adm_hw_port_quiesce_cb_fn cb_func = port_quiesce_ctx->cb_func;  /* [한국어] 모든 큐 멈춤 시 호출할 사용자 콜백 */

	/*
	 * Decrement the callback/quiesced queue count.
	 */
	port_quiesce_ctx->quiesce_count--;                   /* [한국어] 멈춘 큐 1개 반영 - 0이 되면 포트 전체 완료 */
	SPDK_DEBUGLOG(nvmf_fc_adm_api, "Queue%d Quiesced\n", quiesce_api_data->hwqp->hwqp_id);

	free(quiesce_api_data);                              /* [한국어] 개별 큐 quiesce 인자 해제 */
	/*
	 * Wait for call backs i.e. max_ioq_queues + LS QUEUE.
	 */
	if (port_quiesce_ctx->quiesce_count > 0) {           /* [한국어] 아직 멈출 큐가 남았으면 대기(다른 콜백이 마저 처리) */
		return;
	}

	if (fc_port->hw_port_status == SPDK_FC_PORT_QUIESCED) {  /* [한국어] (드문 경쟁) 이미 QUIESCED면 중복 - 경고만 */
		SPDK_ERRLOG("Port %d already in quiesced state.\n", fc_port->port_hdl);
	} else {                                             /* [한국어] 정상: 마지막 큐가 멈췄으니 포트를 QUIESCED로 전환 */
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d quiesced.\n", fc_port->port_hdl);
		fc_port->hw_port_status = SPDK_FC_PORT_QUIESCED;  /* [한국어] 포트 상태 갱신 - 이후 폴링/이벤트 처리 중단 */
	}

	if (cb_func) {                                       /* [한국어] quiesce 요청자가 등록한 완료 콜백 */
		/*
		 * Callback function for the called of quiesce.
		 */
		cb_func(port_quiesce_ctx->ctx, err);         /* [한국어] 포트 전체 quiesce 완료 통지 */
	}

	/*
	 * Free the context structure.
	 */
	free(port_quiesce_ctx);                              /* [한국어] 포트 집계 컨텍스트 해제 */

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d quiesce done, rc = %d.\n", fc_port->port_hdl,
		      err);
}

/*
 * [한국어]
 * nvmf_fc_adm_hw_queue_quiesce - 단일 HWQP에 quiesce(정지) poller 메시지를 보낸다.
 *
 * @fc_hwqp: 정지시킬 HWQP.
 * @ctx: 완료 콜백에 전달될 컨텍스트(포트 집계 컨텍스트).
 * @cb_func: 정지 완료 시 호출될 콜백(nvmf_fc_adm_queue_quiesce_cb).
 * @return: 0 메시지 전송 성공, 음수면 실패.
 *
 * HWQP는 자기 poll group 스레드에서만 안전하게 멈출 수 있으므로, main thread가 직접
 * 멈추지 않고 poller API(SPDK_NVMF_FC_POLLER_API_QUIESCE_QUEUE) 메시지를 보내 해당
 * HWQP 스레드가 스스로 정지하게 한다(thread affinity 준수). 완료되면 cb_func가 원래
 * 스레드(cb_thread)에서 호출된다.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(quiesce 시작). 실제 정지는 HWQP 스레드에서 수행.
 *
 * 호출 체인:
 *   nvmf_fc_adm_hw_port_quiesce -> [본 함수] -> nvmf_fc_poller_api_func(QUIESCE_QUEUE)
 */
static int
nvmf_fc_adm_hw_queue_quiesce(struct spdk_nvmf_fc_hwqp *fc_hwqp, void *ctx,
			     spdk_nvmf_fc_poller_api_cb cb_func)
{
	struct spdk_nvmf_fc_poller_api_quiesce_queue_args *args;  /* [한국어] poller에 전달할 quiesce 인자(콜백 정보 포함) */
	enum spdk_nvmf_fc_poller_api_ret rc = SPDK_NVMF_FC_POLLER_API_SUCCESS;  /* [한국어] poller API 반환코드 */
	int err = 0;                                         /* [한국어] 함수 반환 에러코드 */

	args = calloc(1, sizeof(struct spdk_nvmf_fc_poller_api_quiesce_queue_args));  /* [한국어] 인자 할당(콜백이 free) */

	if (args == NULL) {                                 /* [한국어] 메모리 부족 */
		err = -ENOMEM;
		SPDK_ERRLOG("Failed to allocate memory for poller quiesce args, hwqp:%d\n", fc_hwqp->hwqp_id);
		goto done;
	}
	args->hwqp = fc_hwqp;                               /* [한국어] 멈출 HWQP 지정 */
	args->ctx = ctx;                                    /* [한국어] 포트 집계 컨텍스트 전달 */
	args->cb_info.cb_func = cb_func;                    /* [한국어] 완료 콜백 함수 */
	args->cb_info.cb_data = args;                       /* [한국어] 콜백에 자기 자신(args)을 넘김 - 어느 큐인지 식별 */
	args->cb_info.cb_thread = spdk_get_thread();        /* [한국어] 완료 콜백을 돌릴 스레드 = main thread - affinity 보존 */

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "Quiesce queue %d\n", fc_hwqp->hwqp_id);
	rc = nvmf_fc_poller_api_func(fc_hwqp, SPDK_NVMF_FC_POLLER_API_QUIESCE_QUEUE, args);  /* [한국어] HWQP 스레드로 QUIESCE 메시지 전송 */
	if (rc) {                                           /* [한국어] 메시지 전송 실패 */
		free(args);                                 /* [한국어] 인자 해제(콜백 안 불릴 것이므로 직접 정리) */
		err = -EINVAL;
	}

done:
	return err;                                         /* [한국어] 호출자가 quiesce_count 증가 여부 판단 */
}

/*
 * Hw port Quiesce
 */
/*
 * [한국어]
 * nvmf_fc_adm_hw_port_quiesce - 한 FC 포트의 LS+모든 IO 큐를 정지(quiesce)시킨다.
 *
 * @fc_port: 정지시킬 포트.
 * @ctx: 사용자 완료 콜백에 전달될 컨텍스트.
 * @cb_func: 모든 큐 정지 완료 시 호출될 사용자 콜백.
 * @return: 0 정지 시작 성공, 음수면 실패.
 *
 * HW port reset/free 전에 in-flight 처리를 안전히 멈추기 위한 단계. 포트가 이미
 * OFFLINE이면 곧장 QUIESCED로 바꾸고 콜백을 즉시 호출(멈출 큐 없음). 그렇지 않으면
 * 포트 집계 컨텍스트(port_quiesce_ctx)를 만들고 LS 큐와 모든 IO 큐 각각에
 * nvmf_fc_adm_hw_queue_quiesce를 호출해 quiesce_count를 누적시킨다. 각 큐가 멈출 때마다
 * nvmf_fc_adm_queue_quiesce_cb가 count를 줄여 0이 되면 최종 콜백을 호출한다.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   nvmf_fc_adm_evnt_hw_port_free/reset/offline -> [본 함수] -> nvmf_fc_adm_hw_queue_quiesce
 */
static int
nvmf_fc_adm_hw_port_quiesce(struct spdk_nvmf_fc_port *fc_port, void *ctx,
			    spdk_nvmf_fc_adm_hw_port_quiesce_cb_fn cb_func)
{
	struct spdk_nvmf_fc_adm_hw_port_quiesce_ctx *port_quiesce_ctx = NULL;  /* [한국어] 포트 전체 quiesce 집계 컨텍스트 */
	uint32_t i = 0;                                     /* [한국어] IO 큐 순회 인덱스 */
	int err = 0;                                        /* [한국어] 반환 에러코드 */

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port:%d is being quiesced.\n", fc_port->port_hdl);

	/*
	 * If the port is in an OFFLINE state, set the state to QUIESCED
	 * and execute the callback.
	 */
	if (fc_port->hw_port_status == SPDK_FC_PORT_OFFLINE) {  /* [한국어] OFFLINE 포트는 이미 처리 중인 작업 없음 */
		fc_port->hw_port_status = SPDK_FC_PORT_QUIESCED;  /* [한국어] 곧장 QUIESCED로 전환 */
	}

	if (fc_port->hw_port_status == SPDK_FC_PORT_QUIESCED) {  /* [한국어] 이미(또는 방금) QUIESCED면 멈출 큐 없음 */
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "Port %d already in quiesced state.\n",
			      fc_port->port_hdl);
		/*
		 * Execute the callback function directly.
		 */
		cb_func(ctx, err);                          /* [한국어] 비동기 큐 정지 없이 즉시 완료 콜백 호출 */
		goto out;
	}

	port_quiesce_ctx = calloc(1, sizeof(struct spdk_nvmf_fc_adm_hw_port_quiesce_ctx));  /* [한국어] 집계 컨텍스트 할당 */

	if (port_quiesce_ctx == NULL) {                     /* [한국어] 메모리 부족 */
		err = -ENOMEM;
		SPDK_ERRLOG("Failed to allocate memory for LS queue quiesce ctx, port:%d\n",
			    fc_port->port_hdl);
		goto out;
	}

	port_quiesce_ctx->quiesce_count = 0;                /* [한국어] 정지 대기 큐 수 초기화 */
	port_quiesce_ctx->ctx = ctx;                        /* [한국어] 사용자 콜백 컨텍스트 보관 */
	port_quiesce_ctx->cb_func = cb_func;                /* [한국어] 최종 완료 콜백 보관 */

	/*
	 * Quiesce the LS queue.
	 */
	err = nvmf_fc_adm_hw_queue_quiesce(&fc_port->ls_queue, port_quiesce_ctx,
					   nvmf_fc_adm_queue_quiesce_cb);  /* [한국어] LS 큐 정지 요청 */
	if (err != 0) {                                     /* [한국어] LS 큐 정지 시작 실패면 IO 큐 진행 안 함 */
		SPDK_ERRLOG("Failed to quiesce the LS queue.\n");
		goto out;
	}
	port_quiesce_ctx->quiesce_count++;                  /* [한국어] LS 큐 1개 대기 반영 */

	/*
	 * Quiesce the IO queues.
	 */
	for (i = 0; i < fc_port->num_io_queues; i++) {      /* [한국어] 모든 IO 큐에 대해 정지 요청 */
		err = nvmf_fc_adm_hw_queue_quiesce(&fc_port->io_queues[i],
						   port_quiesce_ctx,
						   nvmf_fc_adm_queue_quiesce_cb);  /* [한국어] i번째 IO 큐 정지 요청 */
		if (err != 0) {                             /* [한국어] (드문) 개별 IO 큐 실패 - 단언만 하고 계속 진행 */
			DEV_VERIFY(0);
			SPDK_ERRLOG("Failed to quiesce the IO queue:%d.\n", fc_port->io_queues[i].hwqp_id);
		}
		port_quiesce_ctx->quiesce_count++;          /* [한국어] IO 큐 대기 카운트 증가(실패해도 콜백 일관성 위해 증가) */
	}

out:
	if (port_quiesce_ctx && err != 0) {                 /* [한국어] 컨텍스트는 만들었으나 에러로 진행 못함 */
		free(port_quiesce_ctx);                     /* [한국어] 집계 컨텍스트 해제(콜백이 안 불릴 것이므로) */
	}
	return err;                                         /* [한국어] 정지 시작 결과 반환 */
}

/*
 * Initialize and add a HW port entry to the global
 * HW port list.
 */
/*
 * [한국어]
 * nvmf_fc_adm_evnt_hw_port_init - "HW Port Init" 관리 이벤트 핸들러: 새 FC 포트를 생성·등록한다.
 *
 * @arg: spdk_nvmf_fc_adm_api_data 포인터(api_args에 hw_port_init_args, cb_func 포함).
 * @return: 없음(결과는 cb_func로 통지).
 *
 * LLD가 새 물리 FC 포트를 SPDK에 알릴 때 main thread로 디스패치되는 이벤트. 단계:
 *  1) IO 큐 개수가 코어 수를 넘지 않는지 검증(큐는 코어/poll group에 배치되므로).
 *  2) port_handle 중복 등록 검사(nvmf_fc_port_lookup).
 *  3) fc_port + IO 큐 배열을 한 덩어리로 calloc(가변 길이 - io_queues가 구조체 뒤에 인접).
 *  4) nvmf_fc_adm_hw_port_data_init로 LS/IO HWQP 초기화.
 *  5) nvmf_fc_port_add로 전역 포트 리스트에 등록.
 * 실패 시 부분 할당을 free하고, 항상 사용자 콜백(SPDK_FC_HW_PORT_INIT)으로 결과 통지.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(ASSERT로 검증).
 *
 * 호출 체인:
 *   spdk_nvmf_fc_master_enqueue_event(HW_PORT_INIT) -> spdk_thread_send_msg(main) -> [본 함수]
 *     -> nvmf_fc_adm_hw_port_data_init / nvmf_fc_port_add
 */
static void
nvmf_fc_adm_evnt_hw_port_init(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 - 전역 포트 리스트 직렬 갱신 */
	struct spdk_nvmf_fc_port *fc_port = NULL;            /* [한국어] 새로 만들 포트 객체 */
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;  /* [한국어] 이벤트 래퍼(콜백+인자) */
	struct spdk_nvmf_fc_hw_port_init_args *args = (struct spdk_nvmf_fc_hw_port_init_args *)
			api_data->api_args;                 /* [한국어] init 인자(포트 핸들, 큐 개수 등) */
	int err = 0;                                         /* [한국어] 결과 코드 - 콜백에 전달 */

	if (args->io_queue_cnt > spdk_env_get_core_count()) {  /* [한국어] IO 큐 수가 코어 수 초과 - 배치 불가 */
		SPDK_ERRLOG("IO queues count greater than cores for %d.\n", args->port_handle);
		err = EINVAL;
		goto abort_port_init;
	}

	/*
	 * 1. Check for duplicate initialization.
	 */
	fc_port = nvmf_fc_port_lookup(args->port_handle);   /* [한국어] 같은 핸들의 포트가 이미 있는지 조회 */
	if (fc_port != NULL) {                              /* [한국어] 중복 등록 - 거부 */
		SPDK_ERRLOG("Duplicate port found %d.\n", args->port_handle);
		goto abort_port_init;
	}

	/*
	 * 2. Get the memory to instantiate a fc port.
	 */
	fc_port = calloc(1, sizeof(struct spdk_nvmf_fc_port) +
			 (args->io_queue_cnt * sizeof(struct spdk_nvmf_fc_hwqp)));  /* [한국어] 포트 헤더 + IO HWQP 배열을 한 번에 할당(연속 배치) */
	if (fc_port == NULL) {                             /* [한국어] 메모리 부족 */
		SPDK_ERRLOG("Failed to allocate memory for fc_port %d.\n", args->port_handle);
		err = -ENOMEM;
		goto abort_port_init;
	}

	/* assign the io_queues array */
	fc_port->io_queues = (struct spdk_nvmf_fc_hwqp *)((uint8_t *)fc_port + sizeof(
				     struct spdk_nvmf_fc_port));  /* [한국어] io_queues 포인터를 구조체 바로 뒤 영역으로 설정(가변 길이 트릭) */

	/*
	 * 3. Initialize the contents for the FC-port
	 */
	err = nvmf_fc_adm_hw_port_data_init(fc_port, args); /* [한국어] LS/IO HWQP 및 nport 리스트 초기화 */

	if (err != 0) {                                    /* [한국어] HWQP 초기화 실패 */
		SPDK_ERRLOG("Data initialization failed for fc_port %d.\n", args->port_handle);
		DEV_VERIFY(!"Data initialization failed for fc_port");  /* [한국어] 디버그 abort */
		goto abort_port_init;
	}

	/*
	 * 4. Add this port to the global fc port list in the library.
	 */
	nvmf_fc_port_add(fc_port);                          /* [한국어] 전역 g_spdk_nvmf_fc_port_list에 등록 - 이후 accept poller가 폴링 */

abort_port_init:
	if (err && fc_port) {                              /* [한국어] 에러 발생 + 포트는 할당된 상태 */
		free(fc_port);                              /* [한국어] 부분 할당 포트 해제(누수 방지) */
	}
	if (api_data->cb_func != NULL) {                   /* [한국어] 사용자 콜백 등록 시 */
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_INIT, args->cb_ctx, err);  /* [한국어] HW_PORT_INIT 완료를 LLD/사용자에게 통지 */
	}

	free(arg);                                          /* [한국어] 이벤트 래퍼 해제 */

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d initialize done, rc = %d.\n",
		      args->port_handle, err);
}

/*
 * [한국어]
 * nvmf_fc_adm_hwqp_clean_sync_cb - HWQP에 매달린 미완료 queue-sync 콜백을 정리한다.
 *
 * @hwqp: 정리 대상 HWQP.
 * @return: 없음.
 *
 * ABTS(Abort Sequence) 처리 등에서 여러 HWQP가 동기점(queue sync)에 도달했는지 세는
 * 메커니즘이 있다. 포트 free 시 아직 응답하지 않은 sync 콜백들을 강제로 정리해야
 * 자원 누수를 막는다. 각 sync 인자에 매달린 abts ctx의 hwqps_responded를 증가시켜
 * num_hwqps에 도달하면(마지막 HWQP) ctx와 관련 인자들을 free한다.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(포트 free 경로).
 *
 * 호출 체인:
 *   nvmf_fc_adm_evnt_hw_port_free -> [본 함수]
 */
static void
nvmf_fc_adm_hwqp_clean_sync_cb(struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct spdk_nvmf_fc_abts_ctx *ctx;                  /* [한국어] sync 콜백에 연결된 ABTS 컨텍스트 */
	struct spdk_nvmf_fc_poller_api_queue_sync_args *args = NULL, *tmp = NULL;  /* [한국어] sync 콜백 인자 순회 커서(삭제 안전) */

	TAILQ_FOREACH_SAFE(args, &hwqp->sync_cbs, link, tmp) {  /* [한국어] 이 HWQP의 미완료 sync 콜백 모두 순회 */
		TAILQ_REMOVE(&hwqp->sync_cbs, args, link);   /* [한국어] 리스트에서 제거 */
		ctx = args->cb_info.cb_data;                 /* [한국어] sync에 연결된 ABTS 집계 컨텍스트 추출 */
		if (ctx) {                                   /* [한국어] 컨텍스트가 있으면 응답 카운트 */
			if (++ctx->hwqps_responded == ctx->num_hwqps) {  /* [한국어] 마지막 HWQP가 응답하면 - 전체 sync 완료 */
				free(ctx->sync_poller_args);  /* [한국어] sync poller 인자 배열 해제 */
				free(ctx->abts_poller_args);  /* [한국어] abts poller 인자 배열 해제 */
				free(ctx);                    /* [한국어] ABTS 집계 컨텍스트 해제 */
			}
		}
	}
}

/*
 * [한국어]
 * nvmf_fc_adm_evnt_hw_port_free - "HW Port Free" 이벤트 핸들러: FC 포트와 그 HWQP 자원을 해제한다.
 *
 * @arg: spdk_nvmf_fc_adm_api_data 포인터(hw_port_free_args 포함).
 * @return: 없음(결과는 cb_func로 통지).
 *
 * 포트가 offline 처리되고 모든 nport가 제거된 뒤 LLD가 포트 메모리 반납을 요청하는 이벤트.
 *  - 포트 존재 확인, nport_list가 비었는지 검증(아직 nport 남으면 거부).
 *  - LS 큐와 모든 IO 큐에 대해 미완료 sync 콜백 정리 + rte_hash(connection/rport) 해제.
 *  - 전역 포트 리스트에서 제거 후 fc_port 자체 free.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(ASSERT로 검증).
 *
 * 호출 체인:
 *   spdk_nvmf_fc_master_enqueue_event(HW_PORT_FREE) -> spdk_thread_send_msg(main) -> [본 함수]
 *     -> nvmf_fc_adm_hwqp_clean_sync_cb / nvmf_fc_port_remove
 */
static void
nvmf_fc_adm_evnt_hw_port_free(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 - 포트 리스트/해시 직렬 해제 */
	int err = 0, i;                                      /* [한국어] err: 결과코드, i: IO 큐 순회 인덱스 */
	struct spdk_nvmf_fc_port *fc_port = NULL;            /* [한국어] 해제할 포트 */
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;               /* [한국어] HWQP 순회 포인터 */
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;  /* [한국어] 이벤트 래퍼 */
	struct spdk_nvmf_fc_hw_port_free_args *args = (struct spdk_nvmf_fc_hw_port_free_args *)
			api_data->api_args;                 /* [한국어] free 인자(port_handle) */

	fc_port = nvmf_fc_port_lookup(args->port_handle);   /* [한국어] 해제 대상 포트 조회 */
	if (!fc_port) {                                     /* [한국어] 없는 포트 - 잘못된 요청 */
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = -EINVAL;
		goto out;
	}

	if (!TAILQ_EMPTY(&fc_port->nport_list)) {           /* [한국어] 아직 nport가 남아 있으면 free 불가(먼저 nport 삭제 필요) */
		SPDK_ERRLOG("Hw port %d: nports not cleared up yet.\n", args->port_handle);
		err = -EIO;
		goto out;
	}

	/* Clean up and free fc_port */
	hwqp = &fc_port->ls_queue;                          /* [한국어] 먼저 LS 큐부터 정리 */
	nvmf_fc_adm_hwqp_clean_sync_cb(hwqp);               /* [한국어] LS 큐의 미완료 sync 콜백 정리 */
	rte_hash_free(hwqp->connection_list_hash);          /* [한국어] LS 큐 connection 해시 해제 */
	rte_hash_free(hwqp->rport_list_hash);               /* [한국어] LS 큐 rport 해시 해제 */

	for (i = 0; i < (int)fc_port->num_io_queues; i++) { /* [한국어] 모든 IO 큐 정리 */
		hwqp = &fc_port->io_queues[i];              /* [한국어] i번째 IO 큐 */

		nvmf_fc_adm_hwqp_clean_sync_cb(&fc_port->io_queues[i]);  /* [한국어] IO 큐 sync 콜백 정리 */
		rte_hash_free(hwqp->connection_list_hash);  /* [한국어] IO 큐 connection 해시 해제 */
		rte_hash_free(hwqp->rport_list_hash);       /* [한국어] IO 큐 rport 해시 해제 */
	}

	nvmf_fc_port_remove(fc_port);                       /* [한국어] 전역 포트 리스트에서 제거 */
	free(fc_port);                                      /* [한국어] 포트 객체(+IO 큐 배열) 통째 해제 */
out:
	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d free done, rc = %d.\n",
		      args->port_handle, err);
	if (api_data->cb_func != NULL) {                   /* [한국어] 사용자 콜백 통지 */
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_FREE, args->cb_ctx, err);
	}

	free(arg);                                          /* [한국어] 이벤트 래퍼 해제 */
}

/*
 * Online a HW port.
 */
/*
 * [한국어]
 * nvmf_fc_adm_evnt_hw_port_online - "HW Port Online" 이벤트 핸들러: 포트를 온라인 전환하고 폴링을 시작한다.
 *
 * @arg: spdk_nvmf_fc_adm_api_data 포인터(hw_port_online_args 포함).
 * @return: 없음(결과는 cb_func로 통지).
 *
 * FC link가 올라오면 LLD가 이 이벤트를 보낸다. 포트를 ONLINE으로 바꾸고:
 *  - LS 큐를 online 표시(accept poller가 폴링 시작).
 *  - 모든 IO 큐를 online 표시 후 nvmf_fc_poll_group_add_hwqp로 idle poll group에 배치
 *    (그러면 해당 poll group의 reactor가 IO 큐를 폴링하게 됨 - thread affinity 확정).
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   spdk_nvmf_fc_master_enqueue_event(HW_PORT_ONLINE) -> spdk_thread_send_msg(main) -> [본 함수]
 *     -> nvmf_fc_hwqp_set_online / nvmf_fc_poll_group_add_hwqp
 */
static void
nvmf_fc_adm_evnt_hw_port_online(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 */
	struct spdk_nvmf_fc_port *fc_port = NULL;            /* [한국어] 온라인 전환할 포트 */
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;               /* [한국어] HWQP 순회 포인터 */
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;  /* [한국어] 이벤트 래퍼 */
	struct spdk_nvmf_fc_hw_port_online_args *args = (struct spdk_nvmf_fc_hw_port_online_args *)
			api_data->api_args;                 /* [한국어] online 인자(port_handle) */
	int i = 0;                                          /* [한국어] IO 큐 순회 인덱스 */
	int err = 0;                                        /* [한국어] 결과코드 */

	fc_port = nvmf_fc_port_lookup(args->port_handle);   /* [한국어] 포트 조회 */
	if (fc_port) {                                      /* [한국어] 포트가 존재 */
		/* Set the port state to online */
		err = nvmf_fc_port_set_online(fc_port);     /* [한국어] 포트 상태를 ONLINE으로 전환 */
		if (err != 0) {                             /* [한국어] 전환 실패(이미 online 등) */
			SPDK_ERRLOG("Hw port %d online failed. err = %d\n", fc_port->port_hdl, err);
			DEV_VERIFY(!"Hw port online failed");  /* [한국어] 디버그 abort */
			goto out;
		}

		hwqp = &fc_port->ls_queue;                  /* [한국어] LS 큐부터 online */
		hwqp->context = NULL;                       /* [한국어] HWQP 컨텍스트 초기화 */
		(void)nvmf_fc_hwqp_set_online(hwqp);        /* [한국어] LS 큐 online 표시 - accept poller가 폴링 시작 */

		/* Cycle through all the io queues and setup a hwqp poller for each. */
		for (i = 0; i < (int)fc_port->num_io_queues; i++) {  /* [한국어] 모든 IO 큐 online + poll group 배치 */
			hwqp = &fc_port->io_queues[i];      /* [한국어] i번째 IO 큐 */
			hwqp->context = NULL;               /* [한국어] 컨텍스트 초기화 */
			(void)nvmf_fc_hwqp_set_online(hwqp);  /* [한국어] IO 큐 online 표시 */
			nvmf_fc_poll_group_add_hwqp(hwqp);  /* [한국어] idle poll group에 배치 - reactor가 폴링하도록(affinity 확정) */
		}
	} else {                                            /* [한국어] 없는 포트 */
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = -EINVAL;
	}

out:
	if (api_data->cb_func != NULL) {                   /* [한국어] 사용자 콜백 통지 */
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_ONLINE, args->cb_ctx, err);
	}

	free(arg);                                          /* [한국어] 이벤트 래퍼 해제 */

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d online done, rc = %d.\n", args->port_handle,
		      err);
}

/*
 * [한국어]
 * nvmf_fc_adm_hw_port_offline_cb - 모든 IO HWQP가 poll group에서 제거된 뒤 offline을 마무리하는 콜백.
 *
 * @ctx: spdk_nvmf_fc_remove_hwqp_cb_args 포인터(pending 카운터 + 원 콜백).
 * @status: 개별 hwqp 제거 결과(여기선 미사용).
 * @return: 없음.
 *
 * hw_port_offline은 각 IO 큐를 poll group에서 비동기로 떼어낸다. 큐가 하나 제거될 때마다
 * 본 콜백이 호출되어 pending_remove_hwqp를 감소시킨다. 마지막 큐가 제거되어 0이 되면
 * 잔존 nport를 zombie 처리하고(보통은 이미 비어 있음) 사용자 콜백을 호출한다.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(poll group remove 완료가 main으로 돌아옴).
 *
 * 호출 체인:
 *   nvmf_fc_poll_group_remove_hwqp 완료 -> [본 함수] -> 사용자 cb_fn(마지막 큐일 때)
 */
static void
nvmf_fc_adm_hw_port_offline_cb(void *ctx, int status)
{
	int err = 0;                                        /* [한국어] 결과코드 */
	struct spdk_nvmf_fc_port *fc_port = NULL;           /* [한국어] offline 중인 포트 */
	struct spdk_nvmf_fc_remove_hwqp_cb_args *remove_hwqp_args = ctx;  /* [한국어] 제거 집계 컨텍스트 */
	struct spdk_nvmf_fc_hw_port_offline_args *args = remove_hwqp_args->cb_args;  /* [한국어] 원 offline 인자(port_handle) */

	if (--remove_hwqp_args->pending_remove_hwqp) {      /* [한국어] 남은 제거 대기 큐가 있으면 아직 미완료 - 대기 */
		return;
	}

	fc_port = nvmf_fc_port_lookup(args->port_handle);   /* [한국어] 마지막 큐 제거됨 - 포트 재조회 */
	if (!fc_port) {                                     /* [한국어] 포트가 사라짐(드문 경쟁) */
		err = -EINVAL;
		SPDK_ERRLOG("fc_port not found.\n");
		goto out;
	}

	/*
	 * Delete all the nports. Ideally, the nports should have been purged
	 * before the offline event, in which case, only a validation is required.
	 */
	nvmf_fc_adm_hw_port_offline_nport_delete(fc_port);  /* [한국어] 잔존 nport zombie 처리(정상이면 검증만) */
out:
	if (remove_hwqp_args->cb_fn) {                      /* [한국어] offline 완료 콜백 */
		remove_hwqp_args->cb_fn(args->port_handle, SPDK_FC_HW_PORT_OFFLINE, args->cb_ctx, err);
	}

	free(remove_hwqp_args);                             /* [한국어] 제거 집계 컨텍스트 해제 */
}

/*
 * Offline a HW port.
 */
/*
 * [한국어]
 * nvmf_fc_adm_evnt_hw_port_offline - "HW Port Offline" 이벤트 핸들러: 포트를 오프라인 전환한다.
 *
 * @arg: spdk_nvmf_fc_adm_api_data 포인터(hw_port_offline_args 포함).
 * @return: 없음(결과는 cb_func로 비동기 통지).
 *
 * FC link drop 시 LLD가 보내는 이벤트. 포트를 OFFLINE으로 표시하고 모든 HWQP의 폴링을
 * 멈춘다:
 *  - 이미 offline이면 성공 처리 후 종료.
 *  - remove_hwqp_args(집계 컨텍스트)를 만들고 pending 카운터를 IO 큐 수로 설정.
 *  - LS 큐는 곧장 offline 표시(accept poller가 ONLINE 검사로 스킵).
 *  - 각 IO 큐를 offline 표시 후 nvmf_fc_poll_group_remove_hwqp로 poll group에서 비동기 제거.
 *  - 모든 제거가 끝나면 nvmf_fc_adm_hw_port_offline_cb가 사용자 콜백을 호출(여기선 return).
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   spdk_nvmf_fc_master_enqueue_event(HW_PORT_OFFLINE) -> spdk_thread_send_msg(main) -> [본 함수]
 *     -> nvmf_fc_poll_group_remove_hwqp -> (완료) nvmf_fc_adm_hw_port_offline_cb
 */
static void
nvmf_fc_adm_evnt_hw_port_offline(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 */
	struct spdk_nvmf_fc_port *fc_port = NULL;            /* [한국어] offline 전환할 포트 */
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;               /* [한국어] HWQP 순회 포인터 */
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;  /* [한국어] 이벤트 래퍼 */
	struct spdk_nvmf_fc_hw_port_offline_args *args = (struct spdk_nvmf_fc_hw_port_offline_args *)
			api_data->api_args;                 /* [한국어] offline 인자(port_handle) */
	struct spdk_nvmf_fc_remove_hwqp_cb_args *remove_hwqp_args;  /* [한국어] IO 큐 제거 집계 컨텍스트 */
	int i = 0;                                          /* [한국어] IO 큐 순회 인덱스 */
	int err = 0;                                        /* [한국어] 결과코드 */

	fc_port = nvmf_fc_port_lookup(args->port_handle);   /* [한국어] 포트 조회 */
	if (fc_port) {                                      /* [한국어] 포트 존재 */
		/* Set the port state to offline, if it is not already. */
		err = nvmf_fc_port_set_offline(fc_port);    /* [한국어] OFFLINE으로 전환 시도 */
		if (err != 0) {                             /* [한국어] 이미 offline이면 set 실패 - 정상으로 간주 */
			SPDK_ERRLOG("Hw port %d already offline. err = %d\n", fc_port->port_hdl, err);
			err = 0;                            /* [한국어] 에러 무효화(중복 offline은 성공 취급) */
			goto out;
		}

		remove_hwqp_args = calloc(1, sizeof(struct spdk_nvmf_fc_remove_hwqp_cb_args));  /* [한국어] 제거 집계 컨텍스트 할당 */
		if (!remove_hwqp_args) {                    /* [한국어] 메모리 부족 */
			SPDK_ERRLOG("Failed to alloc memory for remove_hwqp_args\n");
			err = -ENOMEM;
			goto out;
		}
		remove_hwqp_args->cb_fn = api_data->cb_func;  /* [한국어] 최종 완료 콜백 보관 */
		remove_hwqp_args->cb_args = api_data->api_args;  /* [한국어] 콜백에 넘길 원 인자 보관 */
		remove_hwqp_args->pending_remove_hwqp = fc_port->num_io_queues;  /* [한국어] 제거 대기 큐 수 = IO 큐 수(LS 큐는 동기 처리) */

		hwqp = &fc_port->ls_queue;                  /* [한국어] LS 큐 */
		(void)nvmf_fc_hwqp_set_offline(hwqp);       /* [한국어] LS 큐 offline 표시(poll group에 없으므로 동기) */

		/* Remove poller for all the io queues. */
		for (i = 0; i < (int)fc_port->num_io_queues; i++) {  /* [한국어] 모든 IO 큐 offline + poll group 제거 */
			hwqp = &fc_port->io_queues[i];      /* [한국어] i번째 IO 큐 */
			(void)nvmf_fc_hwqp_set_offline(hwqp);  /* [한국어] offline 표시 */
			nvmf_fc_poll_group_remove_hwqp(hwqp, nvmf_fc_adm_hw_port_offline_cb,
						       remove_hwqp_args);  /* [한국어] poll group에서 비동기 제거, 완료 시 집계 콜백 */
		}

		free(arg);                                  /* [한국어] 이벤트 래퍼는 지금 해제(완료 콜백은 remove_hwqp_args 사용) */

		/* Wait until all the hwqps are removed from poll groups. */
		return;                                     /* [한국어] 비동기 제거 완료를 기다림 - 콜백이 마무리 */
	} else {                                            /* [한국어] 없는 포트 */
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = -EINVAL;
	}
out:
	if (api_data->cb_func != NULL) {                   /* [한국어] (즉시 종료 경로) 사용자 콜백 통지 */
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_OFFLINE, args->cb_ctx, err);
	}

	free(arg);                                          /* [한국어] 이벤트 래퍼 해제 */

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d offline done, rc = %d.\n", args->port_handle,
		      err);
}

/*
 * [한국어]
 * struct nvmf_fc_add_rem_listener_ctx - nport listener를 subsystem에 추가/제거할 때의 비동기 컨텍스트.
 *
 * subsystem은 listener 변경 전 일시정지(pause)되어야 하므로, pause->수정->resume의
 * 여러 콜백 단계를 거치는 동안 이 컨텍스트가 상태를 보관한다.
 */
struct nvmf_fc_add_rem_listener_ctx {
	struct spdk_nvmf_subsystem *subsystem;
	/* [한국어] listener를 추가/제거할 대상 subsystem.
	 * 설정자: nvmf_fc_adm_add_rem_nport_listener가 subsystem 순회 중 설정.
	 * 읽는 자: paused_cb/listen_done/resume_cb가 add_listener/resume 호출에 사용.
	 * 값 범위: 유효한 subsystem 포인터. 동기화: main thread 단일 처리. */

	bool add_listener;
	/* [한국어] true면 listener 추가, false면 제거.
	 * 설정자: add_rem_nport_listener에서 add 인자로 설정.
	 * 읽는 자: nvmf_fc_adm_subsystem_paused_cb가 분기 판단에 사용.
	 * 값 범위: true/false. 동기화: 불변(설정 후 변경 없음). */

	struct spdk_nvme_transport_id trid;
	/* [한국어] 추가/제거할 listener의 FC trid(nport의 WWN으로 생성).
	 * 설정자: nvmf_fc_create_trid로 nport의 nodename/portname WWN을 채움.
	 * 읽는 자: add_listener/remove_listener/tgt_listen_ext가 사용.
	 * 값 범위: trtype=FC인 trid. 동기화: 불변. */
};

/*
 * [한국어]
 * nvmf_fc_adm_subsystem_resume_cb - listener 변경 후 subsystem resume이 완료되면 컨텍스트를 해제한다.
 *
 * @subsystem: resume된 subsystem.
 * @cb_arg: nvmf_fc_add_rem_listener_ctx 포인터.
 * @status: resume 결과(미사용).
 * @return: 없음.
 *
 * pause->listener 수정->resume 시퀀스의 마지막 단계. resume이 끝났으므로 더 이상 필요
 * 없는 컨텍스트를 free한다.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_resume 완료 -> [본 함수]
 */
static void
nvmf_fc_adm_subsystem_resume_cb(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 */
	struct nvmf_fc_add_rem_listener_ctx *ctx = (struct nvmf_fc_add_rem_listener_ctx *)cb_arg;  /* [한국어] 컨텍스트 복원 */
	free(ctx);                                          /* [한국어] 시퀀스 종료 - 컨텍스트 해제 */
}

/*
 * [한국어]
 * nvmf_fc_adm_listen_done - listener 추가/제거가 끝난 뒤 subsystem을 다시 resume시킨다.
 *
 * @cb_arg: nvmf_fc_add_rem_listener_ctx 포인터.
 * @status: listener 변경 결과(미사용).
 * @return: 없음.
 *
 * listener 수정이 완료되면 paused 상태의 subsystem을 resume해야 정상 운영으로 복귀한다.
 * resume 시작이 실패하면 컨텍스트를 즉시 해제(자원 누수 방지).
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   add_listener 완료 / remove_listener 직후 -> [본 함수] -> spdk_nvmf_subsystem_resume
 */
static void
nvmf_fc_adm_listen_done(void *cb_arg, int status)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 */
	struct nvmf_fc_add_rem_listener_ctx *ctx = cb_arg; /* [한국어] 컨텍스트 복원 */

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_fc_adm_subsystem_resume_cb, ctx)) {  /* [한국어] subsystem resume 시작 - 완료 시 resume_cb */
		SPDK_ERRLOG("Failed to resume subsystem: %s\n", ctx->subsystem->subnqn);
		free(ctx);                                  /* [한국어] resume 시작 실패 - 컨텍스트 해제 */
	}
}

/*
 * [한국어]
 * nvmf_fc_adm_subsystem_paused_cb - subsystem이 일시정지되면 listener를 추가 또는 제거한다.
 *
 * @subsystem: paused된 subsystem.
 * @cb_arg: nvmf_fc_add_rem_listener_ctx 포인터.
 * @status: pause 결과(미사용).
 * @return: 없음.
 *
 * subsystem은 활성 상태에서 listener 토폴로지를 바꾸면 안 되므로 pause 후에만 수정한다.
 * add_listener 플래그에 따라 add 또는 remove를 수행하고, 이어서 resume(listen_done)으로 진행.
 * add는 비동기(완료 시 listen_done), remove는 동기이므로 직접 listen_done 호출.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_pause 완료 -> [본 함수]
 *     -> spdk_nvmf_subsystem_add_listener / remove_listener -> nvmf_fc_adm_listen_done
 */
static void
nvmf_fc_adm_subsystem_paused_cb(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 */
	struct nvmf_fc_add_rem_listener_ctx *ctx = (struct nvmf_fc_add_rem_listener_ctx *)cb_arg;  /* [한국어] 컨텍스트 복원 */

	if (ctx->add_listener) {                            /* [한국어] listener 추가 요청 */
		spdk_nvmf_subsystem_add_listener(subsystem, &ctx->trid, nvmf_fc_adm_listen_done, ctx);  /* [한국어] FC trid를 subsystem listener로 추가(비동기 - 완료 시 listen_done) */
	} else {                                            /* [한국어] listener 제거 요청 */
		spdk_nvmf_subsystem_remove_listener(subsystem, &ctx->trid);  /* [한국어] FC trid listener 제거(동기) */
		nvmf_fc_adm_listen_done(ctx, 0);            /* [한국어] 동기 제거이므로 직접 resume 단계로 진행 */
	}
}

/*
 * [한국어]
 * nvmf_fc_adm_add_rem_nport_listener - nport(가상 target port)를 모든 subsystem의 listener로 추가/제거한다.
 *
 * @nport: listener 주소로 사용할 nport(WWN 보유).
 * @add: true면 추가, false면 제거.
 * @return: 0 성공(또는 부분 실패 후 진행), -EINVAL이면 target 미정의.
 *
 * FC nport가 생기거나 사라질 때, 그 nport의 FC 주소(WWN→trid)를 모든 적격 subsystem에
 * listener로 등록/해제해야 host가 해당 nport를 통해 subsystem에 접속할 수 있다. 각
 * subsystem마다:
 *  - listener 허용 여부 확인(any_listener_allowed).
 *  - 컨텍스트 할당 + nport WWN으로 FC trid 생성.
 *  - tgt_listen_ext로 transport listener 등록(추가 시).
 *  - subsystem을 pause시키고(paused_cb에서 실제 add/remove), 그 콜백 체인으로 진행.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   nvmf_fc_adm_evnt_nport_create/delete -> [본 함수]
 *     -> spdk_nvmf_subsystem_pause -> nvmf_fc_adm_subsystem_paused_cb
 */
static int
nvmf_fc_adm_add_rem_nport_listener(struct spdk_nvmf_fc_nport *nport, bool add)
{
	struct spdk_nvmf_tgt *tgt = nvmf_fc_get_tgt();      /* [한국어] FC transport가 바인딩된 target 조회 */
	struct spdk_nvmf_subsystem *subsystem;              /* [한국어] subsystem 순회 커서 */
	struct spdk_nvmf_listen_opts opts;                  /* [한국어] listener 옵션(기본값으로 초기화) */

	if (!tgt) {                                         /* [한국어] target이 없으면 listener 등록 불가 */
		SPDK_ERRLOG("No nvmf target defined\n");
		return -EINVAL;
	}

	spdk_nvmf_listen_opts_init(&opts, sizeof(opts));    /* [한국어] listen 옵션 ABI 호환 초기화(opts_size 전달) */

	subsystem = spdk_nvmf_subsystem_get_first(tgt);     /* [한국어] target의 첫 subsystem부터 순회 */
	while (subsystem) {                                 /* [한국어] 모든 subsystem 처리 */
		struct nvmf_fc_add_rem_listener_ctx *ctx;   /* [한국어] subsystem별 비동기 컨텍스트 */

		if (spdk_nvmf_subsystem_any_listener_allowed(subsystem) == true) {  /* [한국어] 이 subsystem이 listener 추가를 허용하는지 */
			ctx = calloc(1, sizeof(struct nvmf_fc_add_rem_listener_ctx));  /* [한국어] 컨텍스트 할당 */
			if (ctx) {                          /* [한국어] 할당 성공 시에만 진행 */
				ctx->add_listener = add;    /* [한국어] add/remove 의도 저장 */
				ctx->subsystem = subsystem; /* [한국어] 대상 subsystem 저장 */
				nvmf_fc_create_trid(&ctx->trid,
						    nport->fc_nodename.u.wwn,
						    nport->fc_portname.u.wwn);  /* [한국어] nport의 node/port WWN으로 FC trid 조립 */

				if (spdk_nvmf_tgt_listen_ext(subsystem->tgt, &ctx->trid, &opts)) {  /* [한국어] transport listener 등록(FC는 no-op이지만 코어 등록 필요) */
					SPDK_ERRLOG("Failed to add transport address %s to tgt listeners\n",
						    ctx->trid.traddr);
					free(ctx);          /* [한국어] 실패 시 컨텍스트 해제 */
				} else if (spdk_nvmf_subsystem_pause(subsystem,
								     0,
								     nvmf_fc_adm_subsystem_paused_cb,
								     ctx)) {  /* [한국어] subsystem 일시정지 요청(nsid=0=전체), 완료 시 paused_cb */
					SPDK_ERRLOG("Failed to pause subsystem: %s\n",
						    subsystem->subnqn);
					free(ctx);          /* [한국어] pause 시작 실패 시 컨텍스트 해제 */
				}
			}
		}

		subsystem = spdk_nvmf_subsystem_get_next(subsystem);  /* [한국어] 다음 subsystem으로 */
	}

	return 0;                                          /* [한국어] 전체 순회 완료(개별 실패는 로그로만) */
}

/*
 * Create a Nport.
 */
/*
 * [한국어]
 * nvmf_fc_adm_evnt_nport_create - "NPort Create" 이벤트 핸들러: 물리 포트 위에 가상 target port(nport)를 만든다.
 *
 * @arg: spdk_nvmf_fc_adm_api_data 포인터(nport_create_args 포함).
 * @return: 없음(결과는 cb_func로 통지).
 *
 * 하나의 물리 FC 포트는 여러 nport(NPIV 가상 포트)를 호스팅할 수 있다. 이 이벤트는
 * 새 nport를 등록한다:
 *  - 물리 포트 존재 확인 + nport 중복 검사.
 *  - nport 객체 할당 후 핸들/WWN(nodename/portname)/D_ID/상태(CREATED)를 채움.
 *  - rem_port_list/fc_associations 리스트 초기화.
 *  - nvmf_fc_adm_add_rem_nport_listener(true)로 이 nport를 모든 subsystem의 listener로 등록.
 *  - nvmf_fc_port_add_nport로 물리 포트의 nport_list에 추가.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   spdk_nvmf_fc_master_enqueue_event(NPORT_CREATE) -> spdk_thread_send_msg(main) -> [본 함수]
 *     -> nvmf_fc_adm_add_rem_nport_listener / nvmf_fc_port_add_nport
 */
static void
nvmf_fc_adm_evnt_nport_create(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 - nport 리스트 직렬 갱신 */
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;  /* [한국어] 이벤트 래퍼 */
	struct spdk_nvmf_fc_nport_create_args *args = (struct spdk_nvmf_fc_nport_create_args *)
			api_data->api_args;                 /* [한국어] create 인자(핸들/WWN/D_ID) */
	struct spdk_nvmf_fc_nport *nport = NULL;            /* [한국어] 새로 만들 nport */
	struct spdk_nvmf_fc_port *fc_port = NULL;           /* [한국어] 부모 물리 포트 */
	int err = 0;                                        /* [한국어] 결과코드 */

	/*
	 * Get the physical port.
	 */
	fc_port = nvmf_fc_port_lookup(args->port_handle);   /* [한국어] 부모 물리 포트 조회 */
	if (fc_port == NULL) {                              /* [한국어] 없는 물리 포트 */
		err = -EINVAL;
		goto out;
	}

	/*
	 * Check for duplicate initialization.
	 */
	nport = nvmf_fc_nport_find(args->port_handle, args->nport_handle);  /* [한국어] 같은 nport 핸들 중복 검사 */
	if (nport != NULL) {                               /* [한국어] 이미 존재 - 거부 */
		SPDK_ERRLOG("Duplicate SPDK FC nport %d exists for FC port:%d.\n", args->nport_handle,
			    args->port_handle);
		err = -EINVAL;
		goto out;
	}

	/*
	 * Get the memory to instantiate a fc nport.
	 */
	nport = calloc(1, sizeof(struct spdk_nvmf_fc_nport));  /* [한국어] nport 객체 할당 */
	if (nport == NULL) {                               /* [한국어] 메모리 부족 */
		SPDK_ERRLOG("Failed to allocate memory for nport %d.\n",
			    args->nport_handle);
		err = -ENOMEM;
		goto out;
	}

	/*
	 * Initialize the contents for the nport
	 */
	nport->nport_hdl    = args->nport_handle;          /* [한국어] nport 핸들(이 물리 포트 내 식별자) */
	nport->port_hdl     = args->port_handle;           /* [한국어] 부모 물리 포트 핸들 */
	nport->nport_state  = SPDK_NVMF_FC_OBJECT_CREATED; /* [한국어] 초기 상태 CREATED */
	nport->fc_nodename  = args->fc_nodename;           /* [한국어] FC Node Name WWN(WWNN) */
	nport->fc_portname  = args->fc_portname;           /* [한국어] FC Port Name WWN(WWPN) - listener 주소 */
	nport->d_id         = args->d_id;                  /* [한국어] FC Destination ID(N_Port_ID) */
	nport->fc_port      = nvmf_fc_port_lookup(args->port_handle);  /* [한국어] 부모 물리 포트 포인터 캐시 */

	(void)nvmf_fc_nport_set_state(nport, SPDK_NVMF_FC_OBJECT_CREATED);  /* [한국어] 상태 설정(trace 기록 포함) */
	TAILQ_INIT(&nport->rem_port_list);                 /* [한국어] 이 nport에 로그인한 host rport 리스트 초기화 */
	nport->rport_count = 0;                            /* [한국어] rport 카운터 0 */
	TAILQ_INIT(&nport->fc_associations);               /* [한국어] 이 nport의 association 리스트 초기화 */
	nport->assoc_count = 0;                            /* [한국어] association 카운터 0 */

	/*
	 * Populate the nport address (as listening address) to the nvmf subsystems.
	 */
	err = nvmf_fc_adm_add_rem_nport_listener(nport, true);  /* [한국어] 이 nport WWN을 모든 subsystem의 listener로 등록 */

	(void)nvmf_fc_port_add_nport(fc_port, nport);      /* [한국어] 물리 포트의 nport_list에 추가(num_nports++) */
out:
	if (err && nport) {                                /* [한국어] 에러 + nport 할당된 상태 */
		free(nport);                                /* [한국어] 부분 할당 nport 해제 */
	}

	if (api_data->cb_func != NULL) {                   /* [한국어] 사용자 콜백 통지 */
		(void)api_data->cb_func(args->port_handle, SPDK_FC_NPORT_CREATE, args->cb_ctx, err);
	}

	free(arg);                                          /* [한국어] 이벤트 래퍼 해제 */
}

/*
 * [한국어]
 * nvmf_fc_adm_delete_nport_cb - nport에 속한 마지막 I-T 삭제가 끝나면 nport를 free하는 콜백.
 *
 * @port_handle: 물리 포트 핸들.
 * @event_type: 이벤트 타입(SPDK_FC_IT_DELETE 등 - 로그용).
 * @cb_args: spdk_nvmf_fc_adm_nport_del_cb_data 포인터.
 * @spdk_err: 직전 I-T delete 결과.
 * @return: 없음.
 *
 * nport 삭제는 그 nport에 로그인한 모든 host rport(I-T nexus)를 먼저 제거해야 완료된다.
 * 각 I-T delete가 끝날 때마다 본 콜백이 호출되고, nvmf_fc_nport_has_no_rport로 rport가
 * 모두 사라졌는지 확인한다. 마지막 rport였다면 nport를 물리 포트에서 떼어내고 free한 뒤
 * 사용자 콜백(SPDK_FC_NPORT_DELETE)을 호출한다.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread(ASSERT로 검증).
 *
 * 호출 체인:
 *   nvmf_fc_adm_evnt_i_t_delete 완료 -> [본 함수] -> 사용자 fc_cb_func(마지막 rport일 때)
 */
static void
nvmf_fc_adm_delete_nport_cb(uint8_t port_handle, enum spdk_fc_event event_type,
			    void *cb_args, int spdk_err)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 */
	struct spdk_nvmf_fc_adm_nport_del_cb_data *cb_data = cb_args;  /* [한국어] nport 삭제 콜백 컨텍스트 */
	struct spdk_nvmf_fc_nport *nport = cb_data->nport; /* [한국어] 삭제 중인 nport */
	spdk_nvmf_fc_callback cb_func = cb_data->fc_cb_func; /* [한국어] 최종 완료 사용자 콜백 */
	int err = 0;                                        /* [한국어] 결과코드 */
	uint16_t nport_hdl = 0;                             /* [한국어] 로그용 nport 핸들 */
	char log_str[256];                                 /* [한국어] 진단 로그 버퍼 */

	/*
	 * Assert on any delete failure.
	 */
	if (nport == NULL) {                               /* [한국어] nport NULL(설계상 불가) */
		SPDK_ERRLOG("Nport delete callback returned null nport");
		DEV_VERIFY(!"nport is null.");              /* [한국어] 디버그 abort */
		goto out;
	}

	nport_hdl = nport->nport_hdl;                      /* [한국어] 로그용 핸들 캐시 */
	if (0 != spdk_err) {                               /* [한국어] 직전 I-T delete 실패 */
		SPDK_ERRLOG("Nport delete callback returned error. FC Port: "
			    "%d, Nport: %d\n",
			    nport->port_hdl, nport->nport_hdl);
		DEV_VERIFY(!"nport delete callback error.");  /* [한국어] 디버그 abort */
	}

	/*
	 * Free the nport if this is the last rport being deleted and
	 * execute the callback(s).
	 */
	if (nvmf_fc_nport_has_no_rport(nport)) {           /* [한국어] 이 nport에 더 이상 rport가 없으면 - nport 완전 삭제 가능 */
		if (0 != nport->assoc_count) {             /* [한국어] (불변식) rport가 없으면 association도 0이어야 함 */
			SPDK_ERRLOG("association count != 0\n");
			DEV_VERIFY(!"association count != 0");  /* [한국어] 디버그 abort */
		}

		err = nvmf_fc_port_remove_nport(nport->fc_port, nport);  /* [한국어] 물리 포트의 nport_list에서 제거 */
		if (0 != err) {                            /* [한국어] 제거 실패(자료구조 이상) */
			SPDK_ERRLOG("Nport delete callback: Failed to remove "
				    "nport from nport list. FC Port:%d Nport:%d\n",
				    nport->port_hdl, nport->nport_hdl);
		}
		/* Free the nport */
		free(nport);                               /* [한국어] nport 객체 해제 */

		if (cb_func != NULL) {                     /* [한국어] 사용자 완료 콜백 */
			(void)cb_func(cb_data->port_handle, SPDK_FC_NPORT_DELETE, cb_data->fc_cb_ctx, spdk_err);  /* [한국어] NPORT_DELETE 완료 통지 */
		}
		free(cb_data);                             /* [한국어] 콜백 컨텍스트 해제 */
	}
out:
	snprintf(log_str, sizeof(log_str),
		 "port:%d nport:%d delete cb exit, evt_type:%d rc:%d.\n",
		 port_handle, nport_hdl, event_type, spdk_err);  /* [한국어] 진단 로그 조립 */

	if (err != 0) {                                    /* [한국어] 실패 로그 */
		SPDK_ERRLOG("%s", log_str);
	} else {                                           /* [한국어] 성공 디버그 로그 */
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "%s", log_str);
	}
}

/*
 * Delete Nport.
 */
/*
 * [한국어]
 * nvmf_fc_adm_evnt_nport_delete - "NPort Delete" 이벤트 핸들러: nport와 그에 속한 모든 I-T를 제거한다.
 *
 * @arg: spdk_nvmf_fc_adm_api_data 포인터(nport_delete_args 포함).
 * @return: 없음(결과는 cb_func로 비동기 통지).
 *
 * nport 삭제 절차:
 *  - nport 존재 확인 + 콜백 컨텍스트 할당.
 *  - 상태가 CREATED면 TO_BE_DELETED로 전환(삭제 진행 표시). 이미 삭제 중이면 -ENODEV.
 *    ZOMBIE면 부분 생성/삭제 상태이므로 -ENODEV.
 *  - 모든 subsystem listener에서 이 nport 제거(add_rem_nport_listener false).
 *  - rport가 없으면 즉시 delete_nport_cb로 완료.
 *  - rport가 있으면 각 rport마다 SPDK_FC_IT_DELETE 이벤트를 큐잉하여 비동기 제거.
 *    마지막 rport 제거 시 delete_nport_cb가 nport를 free.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   spdk_nvmf_fc_master_enqueue_event(NPORT_DELETE) -> spdk_thread_send_msg(main) -> [본 함수]
 *     -> nvmf_fc_main_enqueue_event(IT_DELETE) -> nvmf_fc_adm_delete_nport_cb
 */
static void
nvmf_fc_adm_evnt_nport_delete(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 */
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;  /* [한국어] 이벤트 래퍼 */
	struct spdk_nvmf_fc_nport_delete_args *args = (struct spdk_nvmf_fc_nport_delete_args *)
			api_data->api_args;                 /* [한국어] delete 인자(포트/nport 핸들) */
	struct spdk_nvmf_fc_nport *nport = NULL;            /* [한국어] 삭제 대상 nport */
	struct spdk_nvmf_fc_adm_nport_del_cb_data *cb_data = NULL;  /* [한국어] rport 삭제 완료 추적 컨텍스트 */
	struct spdk_nvmf_fc_remote_port_info *rport_iter = NULL;  /* [한국어] rport 순회 커서 */
	int err = 0;                                        /* [한국어] 결과코드 */
	uint32_t rport_cnt = 0;                             /* [한국어] 삭제 스케줄된 rport 수(로그용) */
	int rc = 0;                                         /* [한국어] listener 제거 반환코드 */

	/*
	 * Make sure that the nport exists.
	 */
	nport = nvmf_fc_nport_find(args->port_handle, args->nport_handle);  /* [한국어] 삭제 대상 nport 조회 */
	if (nport == NULL) {                               /* [한국어] 없는 nport */
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d for FC Port: %d.\n", args->nport_handle,
			    args->port_handle);
		err = -EINVAL;
		goto out;
	}

	/*
	 * Allocate memory for callback data.
	 */
	cb_data = calloc(1, sizeof(struct spdk_nvmf_fc_adm_nport_del_cb_data));  /* [한국어] 삭제 완료 콜백 컨텍스트 할당 */
	if (NULL == cb_data) {                             /* [한국어] 메모리 부족 */
		SPDK_ERRLOG("Failed to allocate memory for cb_data %d.\n", args->nport_handle);
		err = -ENOMEM;
		goto out;
	}

	cb_data->nport = nport;                            /* [한국어] 삭제 대상 nport */
	cb_data->port_handle = args->port_handle;          /* [한국어] 물리 포트 핸들 */
	cb_data->fc_cb_func = api_data->cb_func;            /* [한국어] 최종 사용자 콜백 */
	cb_data->fc_cb_ctx = args->cb_ctx;                 /* [한국어] 사용자 콜백 인자 */

	/*
	 * Begin nport tear down
	 */
	if (nport->nport_state == SPDK_NVMF_FC_OBJECT_CREATED) {  /* [한국어] 정상 상태 - 삭제 시작 가능 */
		(void)nvmf_fc_nport_set_state(nport, SPDK_NVMF_FC_OBJECT_TO_BE_DELETED);  /* [한국어] TO_BE_DELETED로 전환(이후 신규 작업 거부) */
	} else if (nport->nport_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {  /* [한국어] 이미 삭제 진행 중 */
		/*
		 * Deletion of this nport already in progress. Register callback
		 * and return.
		 */
		/* TODO: Register callback in callback vector. For now, set the error and return. */
		err = -ENODEV;                              /* [한국어] 중복 삭제 - 에러로 반환(콜백 등록은 미구현) */
		goto out;
	} else {                                           /* [한국어] ZOMBIE 등 비정상 상태 */
		/* nport partially created/deleted */
		DEV_VERIFY(nport->nport_state == SPDK_NVMF_FC_OBJECT_ZOMBIE);  /* [한국어] zombie 상태 단언 */
		DEV_VERIFY(0 != "Nport in zombie state");   /* [한국어] 디버그 abort */
		err = -ENODEV;
		goto out;
	}

	/*
	 * Remove this nport from listening addresses across subsystems
	 */
	rc = nvmf_fc_adm_add_rem_nport_listener(nport, false);  /* [한국어] 모든 subsystem listener에서 이 nport 제거 */

	if (0 != rc) {                                     /* [한국어] listener 제거 실패 */
		err = nvmf_fc_nport_set_state(nport, SPDK_NVMF_FC_OBJECT_ZOMBIE);  /* [한국어] nport를 zombie로 격리(부분 삭제 상태) */
		SPDK_ERRLOG("Unable to remove the listen addr in the subsystems for nport %d.\n",
			    nport->nport_hdl);
		goto out;
	}

	/*
	 * Delete all the remote ports (if any) for the nport
	 */
	/* TODO - Need to do this with a "first" and a "next" accessor function
	 * for completeness. Look at app-subsystem as examples.
	 */
	if (nvmf_fc_nport_has_no_rport(nport)) {           /* [한국어] 삭제할 rport가 없으면 */
		/* No rports to delete. Complete the nport deletion. */
		nvmf_fc_adm_delete_nport_cb(nport->port_hdl, SPDK_FC_NPORT_DELETE, cb_data, 0);  /* [한국어] 곧장 완료 콜백으로 nport free */
		goto out;
	}

	TAILQ_FOREACH(rport_iter, &nport->rem_port_list, link) {  /* [한국어] 이 nport의 모든 rport 순회하며 I-T delete 스케줄 */
		struct spdk_nvmf_fc_hw_i_t_delete_args *it_del_args = calloc(
					1, sizeof(struct spdk_nvmf_fc_hw_i_t_delete_args));  /* [한국어] I-T delete 이벤트 인자 할당 */

		if (it_del_args == NULL) {                 /* [한국어] 메모리 부족 */
			err = -ENOMEM;
			SPDK_ERRLOG("SPDK_FC_IT_DELETE no mem to delete rport with rpi:%d s_id:%d.\n",
				    rport_iter->rpi, rport_iter->s_id);
			DEV_VERIFY(!"SPDK_FC_IT_DELETE failed, cannot allocate memory");  /* [한국어] 디버그 abort */
			goto out;
		}

		rport_cnt++;                               /* [한국어] 스케줄된 rport 수 증가 */
		it_del_args->port_handle = nport->port_hdl;  /* [한국어] 물리 포트 핸들 */
		it_del_args->nport_handle = nport->nport_hdl;  /* [한국어] nport 핸들 */
		it_del_args->cb_ctx = (void *)cb_data;     /* [한국어] 공유 nport 삭제 컨텍스트(마지막 rport가 nport free) */
		it_del_args->rpi = rport_iter->rpi;        /* [한국어] 삭제할 rport의 RPI */
		it_del_args->s_id = rport_iter->s_id;      /* [한국어] 삭제할 rport의 S_ID */

		err = nvmf_fc_main_enqueue_event(SPDK_FC_IT_DELETE, (void *)it_del_args,
						 nvmf_fc_adm_delete_nport_cb);  /* [한국어] I-T delete 이벤트 큐잉(완료 시 delete_nport_cb) */
		if (err) {                                 /* [한국어] 큐잉 실패 */
			free(it_del_args);                  /* [한국어] 인자 해제 */
		}
	}

out:
	/* On failure, execute the callback function now */
	if ((err != 0) || (rc != 0)) {                    /* [한국어] 어딘가 실패했으면 즉시 콜백으로 에러 통지 */
		SPDK_ERRLOG("NPort %d delete failed, error:%d, fc port:%d, "
			    "rport_cnt:%d rc:%d.\n",
			    args->nport_handle, err, args->port_handle,
			    rport_cnt, rc);
		if (cb_data) {                             /* [한국어] 콜백 컨텍스트가 있으면 */
			free(cb_data);                      /* [한국어] 해제(콜백이 자동 호출 안 되므로) */
		}
		if (api_data->cb_func != NULL) {           /* [한국어] 사용자 콜백 직접 호출 */
			(void)api_data->cb_func(args->port_handle, SPDK_FC_NPORT_DELETE, args->cb_ctx, err);
		}

	} else {                                           /* [한국어] 성공(또는 비동기 진행 중) */
		SPDK_DEBUGLOG(nvmf_fc_adm_api,
			      "NPort %d delete done successfully, fc port:%d. "
			      "rport_cnt:%d\n",
			      args->nport_handle, args->port_handle, rport_cnt);
	}

	free(arg);                                          /* [한국어] 이벤트 래퍼 해제 */
}

/*
 * Process an PRLI/IT add.
 */
/*
 * [한국어]
 * nvmf_fc_adm_evnt_i_t_add - "I-T Add" 이벤트 핸들러: 새 host(rport)가 nport에 로그인(PRLI)했음을 등록한다.
 *
 * @arg: spdk_nvmf_fc_adm_api_data 포인터(hw_i_t_add_args 포함).
 * @return: 없음(결과는 cb_func로 통지).
 *
 * FC에서 host가 target nport에 PRLI(Process Login)를 보내면 LLD가 I-T Add 이벤트를 올린다.
 * 이 핸들러는 그 host를 표현하는 remote port(rport) 객체를 만들어 nport에 등록한다:
 *  - nport 존재 확인 + (s_id, rpi) 중복 rport 검사.
 *  - rport 할당 후 상태(CREATED)/S_ID/RPI/WWN을 채워 nport의 rem_port_list에 추가.
 *  - target의 PRLI service parameter를 args에 채워 LLD가 PRLI 응답(PRLI_ACC)에 쓰게 함.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   spdk_nvmf_fc_master_enqueue_event(IT_ADD) -> spdk_thread_send_msg(main) -> [본 함수]
 *     -> nvmf_fc_nport_add_rem_port / nvmf_fc_get_prli_service_params
 */
static void
nvmf_fc_adm_evnt_i_t_add(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 */
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;  /* [한국어] 이벤트 래퍼 */
	struct spdk_nvmf_fc_hw_i_t_add_args *args = (struct spdk_nvmf_fc_hw_i_t_add_args *)
			api_data->api_args;                 /* [한국어] I-T add 인자(s_id/rpi/WWN) */
	struct spdk_nvmf_fc_nport *nport = NULL;            /* [한국어] 부모 nport */
	struct spdk_nvmf_fc_remote_port_info *rport_iter = NULL;  /* [한국어] 중복 검사용 순회 커서 */
	struct spdk_nvmf_fc_remote_port_info *rport = NULL;  /* [한국어] 새로 만들 rport */
	int err = 0;                                        /* [한국어] 결과코드 */

	/*
	 * Make sure the nport port exists.
	 */
	nport = nvmf_fc_nport_find(args->port_handle, args->nport_handle);  /* [한국어] 부모 nport 조회 */
	if (nport == NULL) {                               /* [한국어] 없는 nport */
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d\n", args->nport_handle);
		err = -EINVAL;
		goto out;
	}

	/*
	 * Check for duplicate i_t_add.
	 */
	TAILQ_FOREACH(rport_iter, &nport->rem_port_list, link) {  /* [한국어] 기존 rport와 (s_id, rpi) 중복 검사 */
		if ((rport_iter->s_id == args->s_id) && (rport_iter->rpi == args->rpi)) {  /* [한국어] 같은 host가 이미 로그인됨 */
			SPDK_ERRLOG("Duplicate rport found for FC nport %d: sid:%d rpi:%d\n",
				    args->nport_handle, rport_iter->s_id, rport_iter->rpi);
			err = -EEXIST;
			goto out;
		}
	}

	/*
	 * Get the memory to instantiate the remote port
	 */
	rport = calloc(1, sizeof(struct spdk_nvmf_fc_remote_port_info));  /* [한국어] rport 객체 할당 */
	if (rport == NULL) {                               /* [한국어] 메모리 부족 */
		SPDK_ERRLOG("Memory allocation for rem port failed.\n");
		err = -ENOMEM;
		goto out;
	}

	/*
	 * Initialize the contents for the rport
	 */
	(void)nvmf_fc_rport_set_state(rport, SPDK_NVMF_FC_OBJECT_CREATED);  /* [한국어] rport 상태 CREATED */
	rport->s_id = args->s_id;                          /* [한국어] host의 FC Source ID */
	rport->rpi = args->rpi;                            /* [한국어] Remote Port Index(LLD가 부여한 host 핸들) */
	rport->fc_nodename = args->fc_nodename;            /* [한국어] host Node Name WWN */
	rport->fc_portname = args->fc_portname;            /* [한국어] host Port Name WWN */

	/*
	 * Add remote port to nport
	 */
	if (nvmf_fc_nport_add_rem_port(nport, rport) != 0) {  /* [한국어] nport의 rem_port_list에 추가(rport_count++) */
		DEV_VERIFY(!"Error while adding rport to list");  /* [한국어] 추가 실패는 자료구조 이상 - 디버그 abort */
	};

	/*
	 * TODO: Do we validate the initiators service parameters?
	 */

	/*
	 * Get the targets service parameters from the library
	 * to return back to the driver.
	 */
	args->target_prli_info = nvmf_fc_get_prli_service_params();  /* [한국어] target의 PRLI service param(첫 burst 등)을 args에 채워 LLD가 PRLI_ACC에 사용 */

out:
	if (api_data->cb_func != NULL) {                   /* [한국어] 사용자 콜백 통지 */
		/*
		 * Passing pointer to the args struct as the first argument.
		 * The cb_func should handle this appropriately.
		 */
		(void)api_data->cb_func(args->port_handle, SPDK_FC_IT_ADD, args->cb_ctx, err);  /* [한국어] IT_ADD 완료 통지(args 포함 - PRLI param 전달) */
	}

	free(arg);                                          /* [한국어] 이벤트 래퍼 해제 */

	SPDK_DEBUGLOG(nvmf_fc_adm_api,
		      "IT add on nport %d done, rc = %d.\n",
		      args->nport_handle, err);
}

/**
 * Process a IT delete.
 */
/*
 * [한국어]
 * nvmf_fc_adm_evnt_i_t_delete - "I-T Delete" 이벤트 핸들러: host(rport)의 로그아웃을 처리한다.
 *
 * @arg: spdk_nvmf_fc_adm_api_data 포인터(hw_i_t_delete_args 포함).
 * @return: 없음(결과는 cb_func로 비동기 통지).
 *
 * host가 로그아웃(LOGO)하거나 link가 끊기면 LLD가 I-T Delete 이벤트를 올린다.
 *  - nport와 대상 rport((s_id, rpi)+CREATED 상태)를 찾는다. 못 찾으면 이미 제거됨(-ENODEV).
 *  - 이 rport에 묶인 pending LS 요청을 먼저 정리(신규 LS가 더 들어오지 않게).
 *  - 콜백 컨텍스트 할당, rport 상태를 TO_BE_DELETED로 전환(중복 삭제 방지).
 *  - nvmf_fc_adm_i_t_delete_assoc로 이 rport의 모든 association을 비동기 삭제 + 완료 시
 *    nvmf_fc_adm_i_t_delete_cb -> cb_data free.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   spdk_nvmf_fc_master_enqueue_event(IT_DELETE) 또는 nport_delete -> [본 함수]
 *     -> nvmf_fc_adm_i_t_delete_assoc -> nvmf_fc_adm_i_t_delete_cb
 */
static void
nvmf_fc_adm_evnt_i_t_delete(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 */
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;  /* [한국어] 이벤트 래퍼 */
	struct spdk_nvmf_fc_hw_i_t_delete_args *args = (struct spdk_nvmf_fc_hw_i_t_delete_args *)
			api_data->api_args;                 /* [한국어] I-T delete 인자(s_id/rpi) */
	int rc = 0;                                        /* [한국어] 결과코드 */
	struct spdk_nvmf_fc_nport *nport = NULL;           /* [한국어] 부모 nport */
	struct spdk_nvmf_fc_adm_i_t_del_cb_data *cb_data = NULL;  /* [한국어] I-T delete 완료 콜백 컨텍스트 */
	struct spdk_nvmf_fc_remote_port_info *rport_iter = NULL;  /* [한국어] rport 검색 커서 */
	struct spdk_nvmf_fc_remote_port_info *rport = NULL;  /* [한국어] 삭제 대상 rport */
	uint32_t num_rport = 0;                            /* [한국어] 순회한 rport 수(로그용) */
	char log_str[256];                                 /* [한국어] 진단 로그 버퍼 */

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "IT delete on nport:%d begin.\n", args->nport_handle);

	/*
	 * Make sure the nport port exists. If it does not, error out.
	 */
	nport = nvmf_fc_nport_find(args->port_handle, args->nport_handle);  /* [한국어] nport 조회 */
	if (nport == NULL) {                               /* [한국어] 없는 nport */
		SPDK_ERRLOG("Unable to find the SPDK FC nport:%d\n", args->nport_handle);
		rc = -EINVAL;
		goto out;
	}

	/*
	 * Find this ITN / rport (remote port).
	 */
	TAILQ_FOREACH(rport_iter, &nport->rem_port_list, link) {  /* [한국어] (s_id, rpi)로 삭제 대상 rport 검색 */
		num_rport++;                               /* [한국어] 순회 카운트 */
		if ((rport_iter->s_id == args->s_id) &&
		    (rport_iter->rpi == args->rpi) &&
		    (rport_iter->rport_state == SPDK_NVMF_FC_OBJECT_CREATED)) {  /* [한국어] s_id+rpi 일치 + CREATED 상태 */
			rport = rport_iter;                /* [한국어] 대상 발견 */
			break;
		}
	}

	/*
	 * We should find either zero or exactly one rport.
	 *
	 * If we find zero rports, that means that a previous request has
	 * removed the rport by the time we reached here. In this case,
	 * simply return out.
	 */
	if (rport == NULL) {                               /* [한국어] 이미 제거됨 - 작업 없음 */
		rc = -ENODEV;
		goto out;
	}

	/*
	 * We have the rport slated for deletion. At this point clean up
	 * any LS requests that are sitting in the pending list. Do this
	 * first, then, set the states of the rport so that new LS requests
	 * are not accepted. Then start the cleanup.
	 */
	nvmf_fc_delete_ls_pending(&(nport->fc_port->ls_queue), nport, rport);  /* [한국어] 이 rport 관련 pending LS 요청 정리(신규 차단 전 처리) */

	/*
	 * We have found exactly one rport. Allocate memory for callback data.
	 */
	cb_data = calloc(1, sizeof(struct spdk_nvmf_fc_adm_i_t_del_cb_data));  /* [한국어] 완료 콜백 컨텍스트 할당 */
	if (NULL == cb_data) {                             /* [한국어] 메모리 부족 */
		SPDK_ERRLOG("Failed to allocate memory for cb_data for nport:%d.\n", args->nport_handle);
		rc = -ENOMEM;
		goto out;
	}

	cb_data->nport = nport;                            /* [한국어] 부모 nport */
	cb_data->rport = rport;                            /* [한국어] 삭제 대상 rport */
	cb_data->port_handle = args->port_handle;          /* [한국어] 물리 포트 핸들 */
	cb_data->fc_cb_func = api_data->cb_func;            /* [한국어] 사용자 완료 콜백 */
	cb_data->fc_cb_ctx = args->cb_ctx;                 /* [한국어] 사용자 콜백 인자 */

	/*
	 * Validate rport object state.
	 */
	if (rport->rport_state == SPDK_NVMF_FC_OBJECT_CREATED) {  /* [한국어] 정상 상태 - 삭제 시작 가능 */
		(void)nvmf_fc_rport_set_state(rport, SPDK_NVMF_FC_OBJECT_TO_BE_DELETED);  /* [한국어] TO_BE_DELETED로 전환(중복 삭제 차단) */
	} else if (rport->rport_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {  /* [한국어] 이미 삭제 진행 중 */
		/*
		 * Deletion of this rport already in progress. Register callback
		 * and return.
		 */
		/* TODO: Register callback in callback vector. For now, set the error and return. */
		rc = -ENODEV;
		goto out;
	} else {                                           /* [한국어] ZOMBIE 등 비정상 */
		/* rport partially created/deleted */
		DEV_VERIFY(rport->rport_state == SPDK_NVMF_FC_OBJECT_ZOMBIE);  /* [한국어] zombie 단언 */
		DEV_VERIFY(!"Invalid rport_state");        /* [한국어] 디버그 abort */
		rc = -ENODEV;
		goto out;
	}

	/*
	 * We have successfully found a rport to delete. Call
	 * nvmf_fc_i_t_delete_assoc(), which will perform further
	 * IT-delete processing as well as free the cb_data.
	 */
	nvmf_fc_adm_i_t_delete_assoc(nport, rport, nvmf_fc_adm_i_t_delete_cb,
				     (void *)cb_data);          /* [한국어] 이 rport의 모든 association 비동기 삭제 시작(완료 시 i_t_delete_cb) */

out:
	if (rc != 0) {                                     /* [한국어] 에러/대상 없음 - 콜백이 자동 호출 안 되므로 직접 처리 */
		/*
		 * We have entered here because either we encountered an
		 * error, or we did not find a rport to delete.
		 * As a result, we will not call the function
		 * nvmf_fc_i_t_delete_assoc() for further IT-delete
		 * processing. Therefore, execute the callback function now.
		 */
		if (cb_data) {                             /* [한국어] 컨텍스트 할당됐으면 해제 */
			free(cb_data);
		}
		if (api_data->cb_func != NULL) {           /* [한국어] 사용자 콜백 직접 호출(에러 통지) */
			(void)api_data->cb_func(args->port_handle, SPDK_FC_IT_DELETE, args->cb_ctx, rc);
		}
	}

	snprintf(log_str, sizeof(log_str),
		 "IT delete on nport:%d end. num_rport:%d rc = %d.\n",
		 args->nport_handle, num_rport, rc);          /* [한국어] 작업 요약 로그 */

	if (rc != 0) {                                     /* [한국어] 실패 로그 */
		SPDK_ERRLOG("%s", log_str);
	} else {                                           /* [한국어] 성공 디버그 로그 */
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "%s", log_str);
	}

	free(arg);                                          /* [한국어] 이벤트 래퍼 해제 */
}

/*
 * Process ABTS received
 */
/*
 * [한국어]
 * nvmf_fc_adm_evnt_abts_recv - "ABTS Received" 이벤트 핸들러: host가 보낸 ABTS 프레임을 처리한다.
 *
 * @arg: spdk_nvmf_fc_adm_api_data 포인터(abts_args 포함).
 * @return: 없음(결과는 cb_func로 통지).
 *
 * ABTS(Abort Sequence)는 host가 진행 중인 FC exchange(I/O)를 취소하라고 보내는 제어 프레임이다.
 * args에는 RPI(host)와 OX_ID/RX_ID(취소 대상 exchange 식별자)가 들어 있다.
 *  - nport 존재 확인. nport가 삭제 중이면 ABTS를 그냥 drop(곧 정리될 것).
 *  - nvmf_fc_handle_abts_frame으로 라이브러리에 위임 - 해당 exchange를 찾아 in-flight req
 *    abort + BA_ACC/BA_RJT 응답 결정.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   spdk_nvmf_fc_master_enqueue_event(ABTS_RECV) -> spdk_thread_send_msg(main) -> [본 함수]
 *     -> nvmf_fc_handle_abts_frame
 */
static void
nvmf_fc_adm_evnt_abts_recv(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 */
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;  /* [한국어] 이벤트 래퍼 */
	struct spdk_nvmf_fc_abts_args *args = (struct spdk_nvmf_fc_abts_args *)api_data->api_args;  /* [한국어] ABTS 인자(rpi/oxid/rxid) */
	struct spdk_nvmf_fc_nport *nport = NULL;            /* [한국어] 대상 nport */
	int err = 0;                                        /* [한국어] 결과코드 */

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "FC ABTS received. RPI:%d, oxid:%d, rxid:%d\n", args->rpi,
		      args->oxid, args->rxid);

	/*
	 * 1. Make sure the nport port exists.
	 */
	nport = nvmf_fc_nport_find(args->port_handle, args->nport_handle);  /* [한국어] nport 조회 */
	if (nport == NULL) {                               /* [한국어] 없는 nport */
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d\n", args->nport_handle);
		err = -EINVAL;
		goto out;
	}

	/*
	 * 2. If the nport is in the process of being deleted, drop the ABTS.
	 */
	if (nport->nport_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {  /* [한국어] nport 삭제 중이면 ABTS 무시(곧 모든 I/O 정리됨) */
		SPDK_DEBUGLOG(nvmf_fc_adm_api,
			      "FC ABTS dropped because the nport is being deleted; RPI:%d, oxid:%d, rxid:%d\n",
			      args->rpi, args->oxid, args->rxid);
		err = 0;                                   /* [한국어] drop은 에러 아님 */
		goto out;

	}

	/*
	 * 3. Pass the received ABTS-LS to the library for handling.
	 */
	nvmf_fc_handle_abts_frame(nport, args->rpi, args->oxid, args->rxid);  /* [한국어] 라이브러리에 위임 - exchange 찾아 abort + BA_ACC/BA_RJT 응답 */

out:
	if (api_data->cb_func != NULL) {                   /* [한국어] 콜백 등록 시 */
		/*
		 * Passing pointer to the args struct as the first argument.
		 * The cb_func should handle this appropriately.
		 */
		(void)api_data->cb_func(args->port_handle, SPDK_FC_ABTS_RECV, args, err);  /* [한국어] ABTS_RECV 통지(args를 넘겨 LLD가 BA 응답 송신) */
	} else {                                           /* [한국어] 콜백 없으면 args 직접 해제 */
		/* No callback set, free the args */
		free(args);
	}

	free(arg);                                          /* [한국어] 이벤트 래퍼 해제 */
}

/*
 * Callback function for hw port quiesce.
 */
/*
 * [한국어]
 * nvmf_fc_adm_hw_port_quiesce_reset_cb - 포트 quiesce 완료 후 reset 처리(선택적 큐 덤프)를 마무리한다.
 *
 * @ctx: spdk_nvmf_fc_adm_hw_port_reset_ctx 포인터(reset 인자 + 콜백).
 * @err: quiesce 결과.
 * @return: 없음.
 *
 * HW port reset은 먼저 포트를 quiesce시킨 뒤 이 콜백에서 마무리된다. dump_queues가
 * 설정되어 있으면 진단용으로 LS/IO 큐의 상태를 덤프 버퍼에 기록한다(디버깅 목적).
 * 덤프 버퍼는 LLD(FCT)가 나중에 free한다. 마지막에 사용자 콜백(HW_PORT_RESET)을 호출한다.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   nvmf_fc_adm_hw_port_quiesce 완료 -> [본 함수] -> nvmf_fc_dump_all_queues / 사용자 cb_func
 */
static void
nvmf_fc_adm_hw_port_quiesce_reset_cb(void *ctx, int err)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 */
	struct spdk_nvmf_fc_adm_hw_port_reset_ctx *reset_ctx =
		(struct spdk_nvmf_fc_adm_hw_port_reset_ctx *)ctx;  /* [한국어] reset 컨텍스트 복원 */
	struct spdk_nvmf_fc_hw_port_reset_args *args = reset_ctx->reset_args;  /* [한국어] reset 인자(dump_queues/reason/dump_buf) */
	spdk_nvmf_fc_callback cb_func = reset_ctx->reset_cb_func;  /* [한국어] 사용자 완료 콜백 */
	struct spdk_nvmf_fc_queue_dump_info dump_info;      /* [한국어] 큐 덤프 작성 커서(buffer/offset) */
	struct spdk_nvmf_fc_port *fc_port = NULL;           /* [한국어] reset 대상 포트 */
	char *dump_buf = NULL;                              /* [한국어] 덤프 버퍼 */
	uint32_t dump_buf_size = SPDK_FC_HW_DUMP_BUF_SIZE;  /* [한국어] 덤프 버퍼 크기 */

	/*
	 * Free the callback context struct.
	 */
	free(ctx);                                          /* [한국어] reset 컨텍스트는 더 이상 불필요 - 해제(필드는 위에서 복사) */

	if (err != 0) {                                     /* [한국어] quiesce 실패면 덤프/reset 생략 */
		SPDK_ERRLOG("Port %d  quiesce operation failed.\n", args->port_handle);
		goto out;
	}

	if (args->dump_queues == false) {                   /* [한국어] 덤프 요청 없으면 바로 완료 */
		/*
		 * Queues need not be dumped.
		 */
		goto out;
	}

	SPDK_ERRLOG("Dumping queues for HW port %d\n", args->port_handle);

	/*
	 * Get the fc port.
	 */
	fc_port = nvmf_fc_port_lookup(args->port_handle);   /* [한국어] 포트 재조회 */
	if (fc_port == NULL) {                              /* [한국어] 포트 사라짐 */
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = -EINVAL;
		goto out;
	}

	/*
	 * Allocate memory for the dump buffer.
	 * This memory will be freed by FCT.
	 */
	dump_buf = (char *)calloc(1, dump_buf_size);        /* [한국어] 덤프 버퍼 할당(LLD/FCT가 나중에 free) */
	if (dump_buf == NULL) {                             /* [한국어] 메모리 부족 */
		err = -ENOMEM;
		SPDK_ERRLOG("Memory allocation for dump buffer failed, SPDK FC port %d\n", args->port_handle);
		goto out;
	}
	*args->dump_buf  = (uint32_t *)dump_buf;            /* [한국어] 호출자에게 덤프 버퍼 포인터 반환 */
	dump_info.buffer = dump_buf;                        /* [한국어] 덤프 작성 커서 버퍼 설정 */
	dump_info.offset = 0;                               /* [한국어] 쓰기 오프셋 0부터 */

	/*
	 * Add the dump reason to the top of the buffer.
	 */
	nvmf_fc_dump_buf_print(&dump_info, "%s\n", args->reason);  /* [한국어] 덤프 사유를 버퍼 맨 위에 기록 */

	/*
	 * Dump the hwqp.
	 */
	nvmf_fc_dump_all_queues(&fc_port->ls_queue, fc_port->io_queues,
				fc_port->num_io_queues, &dump_info);  /* [한국어] LS 큐 + 모든 IO 큐의 상태를 버퍼에 덤프 */

out:
	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d reset done, queues_dumped = %d, rc = %d.\n",
		      args->port_handle, args->dump_queues, err);

	if (cb_func != NULL) {                              /* [한국어] 사용자 완료 콜백 */
		(void)cb_func(args->port_handle, SPDK_FC_HW_PORT_RESET, args->cb_ctx, err);  /* [한국어] HW_PORT_RESET 완료 통지 */
	}
}

/*
 * HW port reset

 */
/*
 * [한국어]
 * nvmf_fc_adm_evnt_hw_port_reset - "HW Port Reset" 이벤트 핸들러: 포트를 quiesce시키고 reset을 시작한다.
 *
 * @arg: spdk_nvmf_fc_adm_api_data 포인터(hw_port_reset_args 포함).
 * @return: 없음(결과는 cb_func로 비동기 통지).
 *
 * 포트 reset(예: HBA 에러 복구)은 in-flight 처리를 멈추는 quiesce가 선행되어야 한다.
 *  - 포트 존재 확인 + reset 컨텍스트 할당(args/콜백 보관).
 *  - nvmf_fc_adm_hw_port_quiesce로 모든 큐를 비동기 정지. 완료 시
 *    nvmf_fc_adm_hw_port_quiesce_reset_cb에서 reset(+선택적 덤프) 마무리.
 *  - quiesce 시작이 실패하면 ctx를 해제하고 즉시 콜백으로 에러 통지.
 *
 * 실행 컨텍스트: g_nvmf_fc_main_thread.
 *
 * 호출 체인:
 *   spdk_nvmf_fc_master_enqueue_event(HW_PORT_RESET) -> spdk_thread_send_msg(main) -> [본 함수]
 *     -> nvmf_fc_adm_hw_port_quiesce -> nvmf_fc_adm_hw_port_quiesce_reset_cb
 */
static void
nvmf_fc_adm_evnt_hw_port_reset(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();                        /* [한국어] main thread 단언 */
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;  /* [한국어] 이벤트 래퍼 */
	struct spdk_nvmf_fc_hw_port_reset_args *args = (struct spdk_nvmf_fc_hw_port_reset_args *)
			api_data->api_args;                 /* [한국어] reset 인자 */
	struct spdk_nvmf_fc_port *fc_port = NULL;           /* [한국어] reset 대상 포트 */
	struct spdk_nvmf_fc_adm_hw_port_reset_ctx *ctx = NULL;  /* [한국어] quiesce 완료까지 args/콜백 보관 컨텍스트 */
	int err = 0;                                        /* [한국어] 결과코드 */

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d dump\n", args->port_handle);

	/*
	 * Make sure the physical port exists.
	 */
	fc_port = nvmf_fc_port_lookup(args->port_handle);   /* [한국어] 포트 조회 */
	if (fc_port == NULL) {                              /* [한국어] 없는 포트 */
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = -EINVAL;
		goto out;
	}

	/*
	 * Save the reset event args and the callback in a context struct.
	 */
	ctx = calloc(1, sizeof(struct spdk_nvmf_fc_adm_hw_port_reset_ctx));  /* [한국어] reset 컨텍스트 할당 */

	if (ctx == NULL) {                                 /* [한국어] 메모리 부족 */
		err = -ENOMEM;
		SPDK_ERRLOG("Memory allocation for reset ctx failed, SPDK FC port %d\n", args->port_handle);
		goto fail;
	}

	ctx->reset_args = args;                            /* [한국어] reset 인자 보관(quiesce 콜백에서 사용) */
	ctx->reset_cb_func = api_data->cb_func;            /* [한국어] 사용자 완료 콜백 보관 */

	/*
	 * Quiesce the hw port.
	 */
	err = nvmf_fc_adm_hw_port_quiesce(fc_port, ctx, nvmf_fc_adm_hw_port_quiesce_reset_cb);  /* [한국어] 포트 정지 시작, 완료 시 reset_cb */
	if (err != 0) {                                    /* [한국어] quiesce 시작 실패 */
		goto fail;
	}

	/*
	 * Once the ports are successfully quiesced the reset processing
	 * will continue in the callback function: spdk_fc_port_quiesce_reset_cb
	 */
	return;                                            /* [한국어] 비동기 quiesce 완료를 기다림(콜백이 마무리) */
fail:
	free(ctx);                                          /* [한국어] 실패 경로 - 컨텍스트 해제 */

out:
	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d dump done, rc = %d.\n", args->port_handle,
		      err);

	if (api_data->cb_func != NULL) {                   /* [한국어] (실패 즉시 종료 경로) 사용자 콜백 통지 */
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_RESET, args->cb_ctx, err);
	}

	free(arg);                                          /* [한국어] 이벤트 래퍼 해제 */
}

/*
 * [한국어]
 * nvmf_fc_adm_run_on_main_thread - 주어진 함수를 FC main thread에서 실행하도록 메시지를 보낸다.
 *
 * @fn: main thread에서 실행할 함수(이벤트 핸들러).
 * @args: fn에 전달할 인자.
 * @return: 없음.
 *
 * 모든 FC 관리 이벤트는 전역 자료구조(포트/nport/rport 리스트)를 직렬로 다루기 위해
 * g_nvmf_fc_main_thread에서만 실행되어야 한다. 이 헬퍼는 spdk_thread_send_msg로 lockless
 * 메시지 전달을 통해 fn을 main thread 컨텍스트로 옮긴다(다른 스레드에서 호출돼도 안전).
 * main thread가 아직 없으면(transport 미생성) 아무것도 하지 않는다.
 *
 * 실행 컨텍스트: 어느 스레드에서나 호출 가능(LLD 인터럽트/poll 스레드 포함).
 *
 * 호출 체인:
 *   nvmf_fc_main_enqueue_event -> [본 함수] -> spdk_thread_send_msg(main, fn)
 */
static inline void
nvmf_fc_adm_run_on_main_thread(spdk_msg_fn fn, void *args)
{
	if (nvmf_fc_get_main_thread()) {                   /* [한국어] FC main thread가 존재(transport 생성됨)할 때만 */
		spdk_thread_send_msg(nvmf_fc_get_main_thread(), fn, args);  /* [한국어] lockless 메시지로 fn을 main thread에 디스패치 - affinity 보장 */
	}
}

/*
 * Queue up an event in the SPDK main threads event queue.
 * Used by the FC driver to notify the SPDK main thread of FC related events.
 */
/*
 * [한국어]
 * nvmf_fc_main_enqueue_event - LLD가 발생시킨 FC 관리 이벤트를 main thread로 디스패치하는 진입점.
 *
 * @event_type: 이벤트 종류(SPDK_FC_HW_PORT_INIT/FREE/ONLINE/OFFLINE, NPORT_CREATE/DELETE,
 *              IT_ADD/DELETE, ABTS_RECV, HW_PORT_RESET).
 * @args: 이벤트별 인자 구조체 포인터(이벤트 핸들러가 캐스트해 사용).
 * @cb_func: 이벤트 처리 완료 시 호출될 콜백(LLD가 결과를 받음).
 * @return: 0 큐잉 성공, 음수면 실패(잘못된 이벤트/NULL args/메모리 부족).
 *
 * 이 함수가 FC HBA 드라이버(LLD)↔SPDK FC target 사이의 주된 관리 인터페이스다. LLD는
 * 자신의 컨텍스트(인터럽트/별도 스레드)에서 이 함수를 호출하고, 본 함수는:
 *  1) 이벤트 타입/인자 유효성 검사.
 *  2) api_data 래퍼 할당(args + cb_func 묶음).
 *  3) event_type을 해당 이벤트 핸들러(nvmf_fc_adm_evnt_*)로 switch 매핑.
 *  4) nvmf_fc_adm_run_on_main_thread로 핸들러를 main thread에서 실행(thread affinity).
 * 따라서 실제 이벤트 처리는 항상 g_nvmf_fc_main_thread에서 직렬로 일어나 락이 필요 없다.
 *
 * 실행 컨텍스트: 임의의 스레드(주로 LLD 컨텍스트). 처리는 main thread로 위임.
 *
 * 호출 체인:
 *   LLD (HBA driver) -> [본 함수] -> nvmf_fc_adm_run_on_main_thread -> nvmf_fc_adm_evnt_*
 */
int
nvmf_fc_main_enqueue_event(enum spdk_fc_event event_type, void *args,
			   spdk_nvmf_fc_callback cb_func)
{
	int err = 0;                                        /* [한국어] 결과코드 */
	struct spdk_nvmf_fc_adm_api_data *api_data = NULL; /* [한국어] args+cb_func를 묶는 이벤트 래퍼 */
	spdk_msg_fn event_fn = NULL;                       /* [한국어] event_type에 매핑되는 이벤트 핸들러 */

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "Enqueue event %d.\n", event_type);

	if (event_type >= SPDK_FC_EVENT_MAX) {             /* [한국어] 정의된 이벤트 범위 초과 - 잘못된 요청 */
		SPDK_ERRLOG("Invalid spdk_fc_event_t %d.\n", event_type);
		err = -EINVAL;
		goto done;
	}

	if (args == NULL) {                                /* [한국어] 인자 없음 - 모든 이벤트는 args 필요 */
		SPDK_ERRLOG("Null args for event %d.\n", event_type);
		err = -EINVAL;
		goto done;
	}

	api_data = calloc(1, sizeof(*api_data));           /* [한국어] 이벤트 래퍼 할당(핸들러가 free) */

	if (api_data == NULL) {                            /* [한국어] 메모리 부족 */
		SPDK_ERRLOG("Failed to alloc api data for event %d.\n", event_type);
		err = -ENOMEM;
		goto done;
	}

	api_data->api_args = args;                         /* [한국어] 이벤트별 인자 보관 */
	api_data->cb_func = cb_func;                       /* [한국어] 완료 콜백 보관 */

	switch (event_type) {                              /* [한국어] 이벤트 타입 -> 핸들러 매핑 */
	case SPDK_FC_HW_PORT_INIT:
		event_fn = nvmf_fc_adm_evnt_hw_port_init;  /* [한국어] 물리 포트 생성 */
		break;

	case SPDK_FC_HW_PORT_FREE:
		event_fn = nvmf_fc_adm_evnt_hw_port_free;  /* [한국어] 물리 포트 해제 */
		break;

	case SPDK_FC_HW_PORT_ONLINE:
		event_fn = nvmf_fc_adm_evnt_hw_port_online;  /* [한국어] 포트 온라인(폴링 시작) */
		break;

	case SPDK_FC_HW_PORT_OFFLINE:
		event_fn = nvmf_fc_adm_evnt_hw_port_offline;  /* [한국어] 포트 오프라인(폴링 중단) */
		break;

	case SPDK_FC_NPORT_CREATE:
		event_fn = nvmf_fc_adm_evnt_nport_create;  /* [한국어] 가상 target port(nport) 생성 */
		break;

	case SPDK_FC_NPORT_DELETE:
		event_fn = nvmf_fc_adm_evnt_nport_delete;  /* [한국어] nport 삭제 */
		break;

	case SPDK_FC_IT_ADD:
		event_fn = nvmf_fc_adm_evnt_i_t_add;       /* [한국어] host 로그인(PRLI) - rport 추가 */
		break;

	case SPDK_FC_IT_DELETE:
		event_fn = nvmf_fc_adm_evnt_i_t_delete;    /* [한국어] host 로그아웃 - rport+association 삭제 */
		break;

	case SPDK_FC_ABTS_RECV:
		event_fn = nvmf_fc_adm_evnt_abts_recv;     /* [한국어] ABTS(exchange abort) 수신 처리 */
		break;

	case SPDK_FC_HW_PORT_RESET:
		event_fn = nvmf_fc_adm_evnt_hw_port_reset; /* [한국어] 포트 reset(quiesce + 덤프) */
		break;

	case SPDK_FC_UNRECOVERABLE_ERR:                    /* [한국어] 복구 불가 에러는 핸들러 없음 */
	default:
		SPDK_ERRLOG("Invalid spdk_fc_event_t: %d\n", event_type);
		err = -EINVAL;
		break;
	}

done:

	if (err == 0) {                                    /* [한국어] 유효 이벤트 - main thread로 위임 */
		assert(event_fn != NULL);                  /* [한국어] err==0이면 핸들러가 반드시 매핑됐어야 함 */
		nvmf_fc_adm_run_on_main_thread(event_fn, (void *)api_data);  /* [한국어] 핸들러를 main thread에서 실행 예약 */
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "Enqueue event %d done successfully\n", event_type);
	} else {                                           /* [한국어] 실패 - 래퍼 정리 */
		SPDK_ERRLOG("Enqueue event %d failed, err = %d\n", event_type, err);
		if (api_data) {                            /* [한국어] 래퍼가 할당됐으면 해제 */
			free(api_data);
		}
	}

	return err;                                        /* [한국어] LLD가 큐잉 성공 여부 판단 */
}

/* [한국어] FC 트랜스포트 vtable을 코어에 등록하는 constructor 매크로.
 * 프로그램 로드 시(SPDK_NVMF_TRANSPORT_REGISTER가 __attribute__((constructor)) 사용)
 * spdk_nvmf_transport_fc를 전역 트랜스포트 목록에 추가하여, RPC nvmf_create_transport "FC"가
 * 이 vtable을 lookup할 수 있게 한다. */
SPDK_NVMF_TRANSPORT_REGISTER(fc, &spdk_nvmf_transport_fc);
/* [한국어] FC admin/관리 평면 로그 컴포넌트 등록 - "nvmf_fc_adm_api" 플래그로 디버그 로그 토글. */
SPDK_LOG_REGISTER_COMPONENT(nvmf_fc_adm_api)
/* [한국어] FC 데이터 경로 로그 컴포넌트 등록 - "nvmf_fc" 플래그로 디버그 로그 토글. */
SPDK_LOG_REGISTER_COMPONENT(nvmf_fc)
