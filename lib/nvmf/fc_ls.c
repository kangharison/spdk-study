/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2018-2019 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] NVMe-FC Link Service(LS) frame 처리 (fc_ls.c)
 *
 * === 파일의 역할 ===
 * NVMe-FC 트랜스포트의 컨트롤 평면을 담당한다. 호스트는 NVMe-FC association/connection
 * 수립과 해제를 위해 FC ELS와 유사한 형식의 NVMe-FC Link Service(LS) frame을 보낸다.
 * 본 파일은 다음 NVMe-FC LS frame들을 디스패치하고 응답을 생성한다:
 *   - Create Association (CR_ASSOC, LS Cmd Code 0x01) - 새 association 생성, admin queue 1개 포함
 *   - Create Connection (CR_CONN, 0x02) - I/O queue 추가
 *   - Disconnect Association (LS Cmd Code 0x05) - association 해제
 *   - Disconnect Connection - 개별 connection 해제
 * 각 LS는 헤더/페이로드 길이, 명령 코드, ERSP ratio 등을 엄밀히 검증해야 하며, 위반 시
 * NVMe-FC 스펙(FC-NVMe-2 8.x)의 LS Reject(LS_RJT) IU로 응답한다.
 * 검증 실패 케이스는 enum(VERR_*)와 validation_errors[] 문자열 테이블로 식별/로깅한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe-FC 컨트롤 평면 흐름:
 *   호스트가 FC ELS PLOGI/PRLI 완료 후 첫 NVMe-FC LS frame 전송
 *   -> LLD가 frame을 수신해 nvmf_fc_handle_ls_rqst() 호출 (entry point)
 *   -> LS Cmd Code별 핸들러로 분기:
 *        nvmf_fc_ls_process_assoc_rqst (Create Association)
 *        nvmf_fc_ls_process_conn_rqst (Create Connection)
 *        nvmf_fc_ls_process_disc_rqst (Disconnect)
 *   -> 검증 통과 시: spdk_nvmf_fc_association/conn 객체 할당/연결
 *   -> 응답 LS IU 작성 후 nvmf_fc_xmt_ls_rsp()로 송신
 *   -> 응답 송신 완료 콜백에서 hwqp/poll group 등록 (qpair는 Connect 캡슐 전까지 미완성)
 * 호출 체인:
 *   상위: fc_lld의 RQ 폴링이 LS frame을 발견하면 본 파일의 dispatcher 호출
 *   본 파일: LS 프로토콜 처리
 *   하위: fc.c의 association/connection lifecycle helper (nvmf_fc_assoc_set_state 등)
 * 실행 컨텍스트: FC 메인 스레드(main_thread) 또는 LS 전용 HWQP가 바인딩된 poll group.
 * 모든 LS 처리는 동일 스레드로 직렬화되어 lock-free 운용.
 *
 * === 타 모듈과의 연결 ===
 * - nvmf_internal.h: subsystem/host NQN 검증을 위한 spdk_nvmf_subsystem 등 코어 타입
 * - transport.h: nvmf_transport_* 디스패치 (특히 listener 매칭)
 * - nvmf_fc.h: FC association/connection/nport/hwqp 객체 정의
 * - fc_lld.h: LS IU 송신, FC frame helper 등 LLD 콜백
 * - spdk/nvmf_spec.h: NVMe-oF Connect 명령 형식 (LS 인증 시 사용)
 * 데이터 흐름:
 *   FC LS Request IU -> dispatcher -> 검증 -> 객체 생성/탐색 ->
 *   LS Accept(LS_ACC) IU 또는 LS_RJT IU 작성 -> LLD send
 *
 * === 주요 함수/구조체 요약 ===
 * - nvmf_fc_handle_ls_rqst: LS frame dispatcher (LS Cmd Code 기반)
 * - nvmf_fc_ls_process_assoc_rqst: Create Association 처리 + admin queue 생성
 * - nvmf_fc_ls_process_conn_rqst: Create Connection 처리 + I/O queue 추가
 * - nvmf_fc_ls_process_disc_rqst: Disconnect 처리 + 자원 회수
 * - nvmf_fc_xmt_ls_rsp / nvmf_fc_xmt_ls_rsp_cb: LS 응답 송신 및 후처리
 * - 검증 enum(VERR_*) + validation_errors[]: LS 검증 에러 코드/메시지 테이블
 */

#include "spdk/env.h"                               /* [한국어] DPDK 환경 추상화 - mempool/spinlock 등 */
#include "spdk/assert.h"                            /* [한국어] SPDK_STATIC_ASSERT - 컴파일 타임 구조체 크기 검증 */
#include "spdk/nvmf.h"                              /* [한국어] 공개 NVMe-oF 타입 (spdk_nvmf_subsystem 등) */
#include "spdk/nvmf_spec.h"                         /* [한국어] NVMe-oF Connect 페이로드 정의 - LS Create Connection이 동일 페이로드 사용 */
#include "spdk/string.h"                            /* [한국어] spdk_strerror 등 문자열 헬퍼 */
#include "spdk/trace.h"                             /* [한국어] LS 단계 latency tracing */
#include "spdk/util.h"                              /* [한국어] SPDK_COUNTOF 등 */
#include "spdk/endian.h"                            /* [한국어] FC는 빅엔디안 - from_be32/to_be32 변환 매크로 */
#include "spdk/log.h"                               /* [한국어] SPDK_ERRLOG/DEBUGLOG - LS 검증 실패 로깅 */
#include "nvmf_internal.h"                          /* [한국어] subsystem state/host ACL 검증을 위한 내부 정의 */
#include "transport.h"                              /* [한국어] nvmf_transport_* 디스패치 헬퍼 */
#include "spdk/nvmf_transport.h"                    /* [한국어] 공개 트랜스포트 인터페이스 */

#include "nvmf_fc.h"                                /* [한국어] FC 자체 객체 정의 - association/conn/nport */
#include "fc_lld.h"                                 /* [한국어] LLD LS 송신 콜백 등 */

/* set to 1 to send ls disconnect in response to ls disconnect from host (per standard) */
#define NVMF_FC_LS_SEND_LS_DISCONNECT 0
/* [한국어] 호스트가 보낸 LS Disconnect에 대한 응답으로 target도 LS Disconnect를 보낼지 여부.
 * FC-NVMe 스펙은 양방향 disconnect를 권장하나, 대부분 구현은 단방향만으로 충분하므로 기본 0.
 * 1로 설정 시 응답에 LS Disconnect Request IU를 동봉. */

/* Validation Error indexes into the string table below */
/* [한국어]
 * enum VERR_* - LS frame 검증 실패 코드.
 * 각 값은 LS 검증 시 발견된 오류 유형을 식별하며, validation_errors[] 배열의 인덱스로 사용된다.
 * 로깅 시 코드 -> 사람이 읽는 문자열로 변환. NVMe-FC 스펙의 LS_RJT reason code와는 별개의
 * 내부 디버깅 코드(target 내부 분류). LS_RJT는 별도 매핑.
 */
enum {
	VERR_NO_ERROR = 0,                          /* [한국어] 검증 통과 */
	VERR_CR_ASSOC_LEN = 1,                      /* [한국어] Create Association IU 전체 길이 검증 실패 */
	VERR_CR_ASSOC_RQST_LEN = 2,                 /* [한국어] CR_ASSOC 요청 헤더 desc_list_len 필드 검증 실패 */
	VERR_CR_ASSOC_CMD = 3,                      /* [한국어] CR_ASSOC 페이로드 내 Command descriptor 식별자 불일치 */
	VERR_CR_ASSOC_CMD_LEN = 4,                  /* [한국어] CR_ASSOC Command descriptor 길이 필드 검증 실패 */
	VERR_ERSP_RATIO = 5,                        /* [한국어] ERSP(Explicit Response) ratio 값이 sqsize 대비 비유효 */
	VERR_ASSOC_ALLOC_FAIL = 6,                  /* [한국어] association 객체 할당 실패 (resource exhausted) */
	VERR_CONN_ALLOC_FAIL = 7,                   /* [한국어] connection 객체 할당 실패 */
	VERR_CR_CONN_LEN = 8,                       /* [한국어] Create Connection IU 전체 길이 검증 실패 */
	VERR_CR_CONN_RQST_LEN = 9,                  /* [한국어] CR_CONN 요청 헤더 길이 검증 실패 */
	VERR_ASSOC_ID = 10,                         /* [한국어] CR_CONN의 Association ID descriptor 식별자 불일치 */
	VERR_ASSOC_ID_LEN = 11,                     /* [한국어] Association ID descriptor 길이 검증 실패 */
	VERR_NO_ASSOC = 12,                         /* [한국어] Association ID로 association을 못 찾음 (이미 disconnect되었거나 잘못된 ID) */
	VERR_CONN_ID = 13,                          /* [한국어] Disconnect의 Connection ID descriptor 식별자 불일치 */
	VERR_CONN_ID_LEN = 14,                      /* [한국어] Connection ID descriptor 길이 검증 실패 */
	VERR_NO_CONN = 15,                          /* [한국어] Connection ID로 connection을 못 찾음 */
	VERR_CR_CONN_CMD = 16,                      /* [한국어] CR_CONN Command descriptor 식별자 불일치 */
	VERR_CR_CONN_CMD_LEN = 17,                  /* [한국어] CR_CONN Command descriptor 길이 검증 실패 */
	VERR_DISCONN_LEN = 18,                      /* [한국어] Disconnect IU 전체 길이 검증 실패 */
	VERR_DISCONN_RQST_LEN = 19,                 /* [한국어] Disconnect 요청 헤더 길이 검증 실패 */
	VERR_DISCONN_CMD = 20,                      /* [한국어] Disconnect Command descriptor 식별자 불일치 */
	VERR_DISCONN_CMD_LEN = 21,                  /* [한국어] Disconnect Command descriptor 길이 검증 실패 */
	VERR_DISCONN_SCOPE = 22,                    /* [한국어] Disconnect scope 필드가 spec에 정의되지 않은 값 */
	VERR_RS_LEN = 23,                           /* [한국어] (예약) Read State IU 길이 검증 실패 */
	VERR_RS_RQST_LEN = 24,                      /* [한국어] (예약) Read State 요청 길이 검증 실패 */
	VERR_RS_CMD = 25,                           /* [한국어] (예약) Read State Command descriptor 검증 실패 */
	VERR_RS_CMD_LEN = 26,                       /* [한국어] (예약) Read State Command 길이 검증 실패 */
	VERR_RS_RCTL = 27,                          /* [한국어] (예약) Read State R_CTL 필드 검증 실패 */
	VERR_RS_RO = 28,                            /* [한국어] (예약) Read State Resv 필드 검증 실패 */
	VERR_CONN_TOO_MANY = 29,                    /* [한국어] association당 connection 수 한도 초과 (qsize/maxq 위반) */
	VERR_SUBNQN = 30,                           /* [한국어] CR_ASSOC의 SubNQN이 등록된 subsystem과 매칭 안됨 */
	VERR_HOSTNQN = 31,                          /* [한국어] CR_ASSOC의 HostNQN이 subsystem ACL에서 거부됨 */
	VERR_SQSIZE = 32,                           /* [한국어] CR_CONN/ASSOC의 SQ size가 트랜스포트 한도 초과 */
	VERR_NO_RPORT = 33,                         /* [한국어] LS 출처 N_Port_ID에 매칭되는 rport 객체 없음 */
	VERR_SUBLISTENER = 34,                      /* [한국어] subsystem이 해당 nport listener에 노출되어 있지 않음 (ACL) */
};

static char *validation_errors[] = {
	"OK",
	"Bad CR_ASSOC Length",
	"Bad CR_ASSOC Rqst Length",
	"Not CR_ASSOC Cmd",
	"Bad CR_ASSOC Cmd Length",
	"Bad Ersp Ratio",
	"Association Allocation Failed",
	"Queue Allocation Failed",
	"Bad CR_CONN Length",
	"Bad CR_CONN Rqst Length",
	"Not Association ID",
	"Bad Association ID Length",
	"No Association",
	"Not Connection ID",
	"Bad Connection ID Length",
	"No Connection",
	"Not CR_CONN Cmd",
	"Bad CR_CONN Cmd Length",
	"Bad DISCONN Length",
	"Bad DISCONN Rqst Length",
	"Not DISCONN Cmd",
	"Bad DISCONN Cmd Length",
	"Bad Disconnect Scope",
	"Bad RS Length",
	"Bad RS Rqst Length",
	"Not RS Cmd",
	"Bad RS Cmd Length",
	"Bad RS R_CTL",
	"Bad RS Relative Offset",
	"Too many connections for association",
	"Invalid subnqn or subsystem not found",
	"Invalid hostnqn or subsystem doesn't allow host",
	"SQ size = 0 or too big",
	"No Remote Port",
	"Bad Subsystem Port",
};

/* [한국어] forward declaration - association을 nport에 연결하는 헬퍼 (Create Association 성공 시 호출) */
static inline void nvmf_fc_add_assoc_to_tgt_port(struct spdk_nvmf_fc_nport *tgtport,
		struct spdk_nvmf_fc_association *assoc,
		struct spdk_nvmf_fc_remote_port_info *rport);

/* [한국어] forward declaration - association에서 특정 connection 해제 (Disconnect 처리 시 호출) */
static void nvmf_fc_del_connection(struct spdk_nvmf_fc_association *assoc,
				   struct spdk_nvmf_fc_conn *fc_conn);

/*
 * [한국어]
 * cpu_to_be32 - 호스트 엔디안 uint32를 FC 와이어 형식(big-endian)으로 변환
 *
 * @in: 변환할 호스트 엔디안 32-bit 정수
 * @return: FC big-endian 표현 (FCNVME_BE32 타입 alias)
 *
 * FC 프로토콜은 빅엔디안이지만 호스트 CPU는 대부분 리틀엔디안이므로,
 * LS IU 필드를 작성할 때 명시 변환이 필요하다. SPDK의 to_be32 헬퍼를 래핑.
 */
static inline FCNVME_BE32
cpu_to_be32(uint32_t in)
{
	uint32_t t;                                 /* [한국어] 변환 결과 임시 저장 - to_be32가 in-place로 채움 */

	to_be32(&t, in);                            /* [한국어] 호스트 endian -> big-endian 변환 (spdk/endian.h) */
	return (FCNVME_BE32)t;                      /* [한국어] FC LS descriptor 필드 타입으로 캐스팅 */
}

/*
 * [한국어]
 * nvmf_fc_lsdesc_len - LS descriptor의 desc_len 필드 값 계산 (big-endian)
 *
 * @sz: descriptor의 전체 크기 (헤더 + 페이로드)
 * @return: desc_len 필드에 들어갈 big-endian 값
 *
 * NVMe-FC LS descriptor의 desc_len은 desc_tag와 desc_len 자체(각 4B)를 제외한 길이.
 * 따라서 sz - (2 * 4B)를 빅엔디안으로 변환해 반환한다. (FC-NVMe-2 8.3.6)
 */
static inline FCNVME_BE32
nvmf_fc_lsdesc_len(size_t sz)
{
	uint32_t t;                                 /* [한국어] 빅엔디안 변환 결과 임시 저장 */

	to_be32(&t, sz - (2 * sizeof(uint32_t)));   /* [한국어] desc_tag(4B) + desc_len(4B) 제외 후 변환 */
	return (FCNVME_BE32)t;                      /* [한국어] LS descriptor 형식으로 캐스팅 */
}

/*
 * [한국어]
 * nvmf_fc_ls_format_rsp_hdr - LS Accept(LS_ACC) 응답 IU의 공통 헤더 채우기
 *
 * @buf: 출력 버퍼 (LS Response IU 시작 주소)
 * @ls_cmd: 응답 LS Cmd Code (예: 0x01=CR_ASSOC 응답, 0x05=DISCONN 응답)
 * @desc_len: 전체 descriptor list 길이
 * @rqst_ls_cmd: 원래 요청의 LS Cmd Code (RQST descriptor에 echo)
 *
 * 모든 LS_ACC IU는 동일한 헤더 구조(LS Cmd Code + desc_list_len + RQST descriptor)로 시작하므로
 * 이 헬퍼가 공통 부분을 채운다. 호출자는 이후 명령별 descriptor를 이어 작성.
 *
 * 호출 체인:
 *   Create Association/Connection/Disconnect 핸들러 -> [본 함수] -> 명령별 descriptor 작성
 */
static void
nvmf_fc_ls_format_rsp_hdr(void *buf, uint8_t ls_cmd, uint32_t desc_len,
			  uint8_t rqst_ls_cmd)
{
	struct spdk_nvmf_fc_ls_acc_hdr *acc_hdr = buf; /* [한국어] LS Accept IU 헤더 구조체로 캐스팅 */

	acc_hdr->w0.ls_cmd = ls_cmd;                /* [한국어] w0[7:0] = 응답 LS Cmd Code (요청과 동일하거나 LS_ACC=0x02) */
	acc_hdr->desc_list_len = desc_len;          /* [한국어] descriptor list 전체 길이 (이미 big-endian이어야 함) */
	to_be32(&acc_hdr->rqst.desc_tag, FCNVME_LSDESC_RQST); /* [한국어] RQST descriptor 태그 - 어떤 요청에 대한 응답인지 echo */
	acc_hdr->rqst.desc_len =                    /* [한국어] RQST descriptor 길이 */
		nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_rqst));
	acc_hdr->rqst.w0.ls_cmd = rqst_ls_cmd;      /* [한국어] 원 요청의 LS Cmd Code를 echo (호스트가 응답 매칭 시 사용) */
}

/*
 * [한국어]
 * nvmf_fc_ls_format_rjt - LS Reject(LS_RJT) 응답 IU 작성
 *
 * @buf: 출력 버퍼
 * @buflen: 버퍼 크기 (검증용, 본 함수는 사용 안 함 - 호출자가 책임)
 * @ls_cmd: 거부 대상 LS Cmd Code
 * @reason: LS_RJT reason code (FC-NVMe-2 Table 31)
 * @explanation: reason의 추가 설명 코드
 * @vendor: vendor-specific reason code (0이면 미사용)
 * @return: 작성한 IU 크기(바이트) - 호출자가 LS 송신 시 size로 사용
 *
 * 검증 실패 시 호스트에 거부 응답을 보낸다. reason/explanation 코드는 호스트가
 * 어떤 필드가 잘못됐는지 진단하는 데 사용된다.
 */
static int
nvmf_fc_ls_format_rjt(void *buf, uint16_t buflen, uint8_t ls_cmd,
		      uint8_t reason, uint8_t explanation, uint8_t vendor)
{
	struct spdk_nvmf_fc_ls_rjt *rjt = buf;      /* [한국어] LS_RJT IU 구조체로 캐스팅 - 헤더 + RJT descriptor */

	bzero(buf, sizeof(struct spdk_nvmf_fc_ls_rjt));     /* [한국어] 안정적 응답을 위해 전체 IU를 0으로 초기화 (Reserved 필드 안전성) */
	nvmf_fc_ls_format_rsp_hdr(buf, FCNVME_LSDESC_RQST,  /* [한국어] 공통 응답 헤더 작성 - RQST descriptor에 ls_cmd echo */
				  nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_ls_rjt)),
				  ls_cmd);
	to_be32(&rjt->rjt.desc_tag, FCNVME_LSDESC_RJT);     /* [한국어] RJT descriptor 태그 - 응답 종류가 거부임을 알림 */
	rjt->rjt.desc_len = nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_rjt)); /* [한국어] RJT descriptor 길이 */
	rjt->rjt.reason_code = reason;              /* [한국어] reason code - 호스트의 진단/재시도 정책 결정 */
	rjt->rjt.reason_explanation = explanation;  /* [한국어] reason의 보조 설명 (예: Invalid LS Length Field) */
	rjt->rjt.vendor = vendor;                   /* [한국어] vendor-specific 확장 (0이면 미사용) */

	return sizeof(struct spdk_nvmf_fc_ls_rjt);  /* [한국어] 작성한 IU 크기 - LLD 송신 호출 시 length 인자 */
}

/* *************************************************** */
/* Allocators/Deallocators (associations, connections, */
/* poller API data)                                    */

/*
 * [한국어]
 * nvmf_fc_ls_free_association - association 객체와 그 부속 자원 일괄 해제
 *
 * @assoc: 해제할 association 객체 (마지막 connection이 사라졌거나 생성 도중 실패한 경우)
 *
 * association은 (1) LS Disconnect 송신용 DMA 버퍼, (2) connection 풀 배열,
 * (3) association 구조체 본체 세 부분으로 구성되므로 역순으로 모두 해제한다.
 * 생성 도중 부분 실패 시 롤백 경로로도 호출되므로 NULL-안전하게 동작해야 한다.
 *
 * 실행 컨텍스트: LS 처리 스레드(LS HWQP 바인딩 poll group). 단일 스레드 직렬화로 lock 불필요.
 *
 * 호출 체인:
 *   nvmf_fc_ls_new_association(롤백) / nvmf_fc_del_connection(마지막 conn) -> [본 함수] -> free
 */
static inline void
nvmf_fc_ls_free_association(struct spdk_nvmf_fc_association *assoc)
{
	/* free association's send disconnect buffer */
	if (assoc->snd_disconn_bufs) {                      /* [한국어] LS Disconnect 송신 버퍼가 아직 남아 있으면 (송신 미완 또는 미사용) */
		nvmf_fc_free_srsr_bufs(assoc->snd_disconn_bufs); /* [한국어] DMA-가능한 SRSR(send-request/recv-response) 버퍼쌍 반환 (LLD 할당) */
	}

	/* free association's connections */
	free(assoc->conns_buf);                             /* [한국어] alloc_connections에서 calloc한 connection 풀 배열 일괄 해제 */

	/* free the association */
	free(assoc);                                        /* [한국어] association 구조체 본체 해제 - 이후 assoc 포인터는 dangling */
}

/*
 * [한국어]
 * nvmf_fc_ls_alloc_connections - association의 connection 풀을 한 번에 선할당
 *
 * @assoc: connection을 부착할 association
 * @nvmf_transport: 트랜스포트 옵션(max_qpairs_per_ctrlr)을 제공하는 FC 트랜스포트
 * @return: 0 성공, -ENOMEM 할당 실패
 *
 * Create Connection LS가 올 때마다 개별 malloc하면 hot path에서 할당 지연/실패 위험이
 * 있으므로, association 생성 시점에 max_qpairs_per_ctrlr+1개(admin queue 1 + I/O queue 다수)
 * connection을 calloc로 한꺼번에 확보하고 avail_fc_conns free list에 모두 등록한다.
 * 이후 new_connection은 이 free list에서 O(1)로 꺼내 쓴다.
 *
 * 실행 컨텍스트: LS 처리 스레드. new_association 내부에서만 호출되어 직렬화됨.
 *
 * 호출 체인:
 *   nvmf_fc_ls_new_association -> [본 함수] -> calloc + TAILQ_INSERT_TAIL
 */
static int
nvmf_fc_ls_alloc_connections(struct spdk_nvmf_fc_association *assoc,
			     struct spdk_nvmf_transport *nvmf_transport)
{
	uint32_t i;                                         /* [한국어] connection 풀 순회 인덱스 */
	struct spdk_nvmf_fc_conn *fc_conn;                  /* [한국어] free list에 넣을 개별 connection 포인터 */

	SPDK_DEBUGLOG(nvmf_fc_ls, "Pre-alloc %d qpairs for host NQN %s\n",
		      nvmf_transport->opts.max_qpairs_per_ctrlr, assoc->host_nqn); /* [한국어] 선할당 개수/host NQN 디버그 로그 */

	/* allocate memory for all connections at once */
	assoc->conns_buf = calloc(nvmf_transport->opts.max_qpairs_per_ctrlr + 1, /* [한국어] +1은 admin queue(qid 0) 몫 - I/O queue 한도와 별도 */
				  sizeof(struct spdk_nvmf_fc_conn));
	if (assoc->conns_buf == NULL) {                     /* [한국어] calloc 실패 - 자원 고갈 */
		SPDK_ERRLOG("Out of memory for connections for new association\n");
		return -ENOMEM;                             /* [한국어] 호출자(new_association)가 association 롤백 트리거 */
	}

	for (i = 0; i < nvmf_transport->opts.max_qpairs_per_ctrlr; i++) { /* [한국어] 각 슬롯을 free list에 등록 (admin 1개는 여유분으로 남김) */
		fc_conn = assoc->conns_buf + (i * sizeof(struct spdk_nvmf_fc_conn)); /* [한국어] i번째 connection 슬롯 주소 계산 (포인터 산술 - 주의: 바이트 단위 오프셋) */
		TAILQ_INSERT_TAIL(&assoc->avail_fc_conns, fc_conn, assoc_avail_link); /* [한국어] 가용(free) connection 리스트에 추가 - new_connection이 여기서 pop */
	}

	return 0;                                           /* [한국어] 선할당 성공 */
}

/*
 * [한국어]
 * nvmf_fc_ls_new_association - Create Association LS 처리 시 association 객체 생성/초기화
 *
 * @s_id: LS frame의 source N_Port_ID (호스트 포트 식별자)
 * @tgtport: LS를 수신한 target nport
 * @rport: s_id에 매칭되는 remote port 객체 (호스트)
 * @a_cmd: Create Association command descriptor (hostid/hostnqn/subnqn 포함)
 * @subsys: subnqn으로 찾은 대상 subsystem
 * @rpi: Remote Port Index (LLD가 호스트 포트에 부여한 핸들)
 * @nvmf_transport: FC 트랜스포트 (connection 풀 크기 결정)
 * @return: 생성된 association 포인터, 실패 시 NULL
 *
 * Create Association LS 검증을 통과한 뒤 호출되어, association 식별 정보(host_id/host_nqn/
 * sub_nqn)와 connection 리스트, 상태를 초기화하고 connection 풀을 선할당한 뒤 nport의
 * association 리스트에 등록한다. 어떤 단계든 실패하면 부분 자원을 롤백하고 NULL 반환.
 *
 * 실행 컨텍스트: LS 처리 스레드. 단일 스레드 직렬화로 association/nport 리스트 lock-free.
 *
 * 호출 체인:
 *   nvmf_fc_ls_process_cass -> [본 함수] -> alloc_connections + add_assoc_to_tgt_port
 */
static struct spdk_nvmf_fc_association *
nvmf_fc_ls_new_association(uint32_t s_id,
			   struct spdk_nvmf_fc_nport *tgtport,
			   struct spdk_nvmf_fc_remote_port_info *rport,
			   struct spdk_nvmf_fc_lsdesc_cr_assoc_cmd *a_cmd,
			   struct spdk_nvmf_subsystem *subsys,
			   uint16_t rpi,
			   struct spdk_nvmf_transport *nvmf_transport)
{
	struct spdk_nvmf_fc_association *assoc;              /* [한국어] 새로 만들 association 객체 */
	int rc;                                             /* [한국어] connection 선할당 결과 코드 */

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "New Association request for port %d nport %d rpi 0x%x\n",
		      tgtport->fc_port->port_hdl, tgtport->nport_hdl, rpi); /* [한국어] 어느 물리 port/nport/rpi에 대한 요청인지 트레이스 */

	assert(rport);                                      /* [한국어] rport는 호출 전에 검증되었어야 함 (디버그 빌드 강제) */
	if (!rport) {                                       /* [한국어] 릴리스 빌드 방어 - rport 없으면 association 생성 불가 */
		SPDK_ERRLOG("rport is null.\n");
		return NULL;
	}

	assoc = calloc(1, sizeof(struct spdk_nvmf_fc_association)); /* [한국어] association 본체 0-초기화 할당 */
	if (!assoc) {                                       /* [한국어] 할당 실패 - 자원 고갈 */
		SPDK_ERRLOG("unable to allocate memory for new association\n");
		return NULL;
	}

	/* initialize association */
#if (NVMF_FC_LS_SEND_LS_DISCONNECT == 1)               /* [한국어] target -> host LS Disconnect 송신 기능 활성화 시에만 버퍼 선할당 */
	/* allocate buffers to send LS disconnect command to host */
	assoc->snd_disconn_bufs =                           /* [한국어] LS Disconnect 요청/응답 DMA 버퍼쌍 선할당 (송신은 마지막 conn 삭제 시) */
		nvmf_fc_alloc_srsr_bufs(sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst),
					sizeof(struct spdk_nvmf_fc_ls_rjt));
	if (!assoc->snd_disconn_bufs) {                     /* [한국어] DMA 버퍼 부족 - 롤백 */
		SPDK_ERRLOG("no dma memory for association's ls disconnect bufs\n");
		free(assoc);
		return NULL;
	}

	assoc->snd_disconn_bufs->rpi = rpi;                 /* [한국어] 송신 시 사용할 remote port index 저장 */
#endif
	assoc->s_id = s_id;                                 /* [한국어] 호스트 source N_Port_ID 기록 (conn으로 전파) */
	assoc->tgtport = tgtport;                           /* [한국어] 소속 target nport 역참조 저장 */
	assoc->rport = rport;                               /* [한국어] 호스트 remote port 객체 - assoc_count 추적용 */
	assoc->subsystem = subsys;                          /* [한국어] 이 association이 접속하는 NVMe subsystem */
	assoc->nvmf_transport = nvmf_transport;             /* [한국어] FC 트랜스포트 역참조 (conn->qpair.transport로 전파) */
	assoc->assoc_state = SPDK_NVMF_FC_OBJECT_CREATED;   /* [한국어] 객체 상태머신 초기값 = CREATED (이후 TO_BE_DELETED/ZOMBIE 전이) */
	memcpy(assoc->host_id, a_cmd->hostid, FCNVME_ASSOC_HOSTID_LEN); /* [한국어] 호스트 ID 복사 (16B) - Connect 캡슐 검증에 사용 */
	memcpy(assoc->host_nqn, a_cmd->hostnqn, SPDK_NVME_NQN_FIELD_SIZE); /* [한국어] 호스트 NQN 복사 (256B) - ACL 검증에 이미 사용된 값 보관 */
	memcpy(assoc->sub_nqn, a_cmd->subnqn, SPDK_NVME_NQN_FIELD_SIZE); /* [한국어] subsystem NQN 복사 - 진단/로깅용 */
	TAILQ_INIT(&assoc->fc_conns);                       /* [한국어] in-use connection 리스트 초기화 */
	TAILQ_INIT(&assoc->avail_fc_conns);                 /* [한국어] free(가용) connection 리스트 초기화 - alloc_connections가 채움 */
	assoc->ls_del_op_ctx = NULL;                        /* [한국어] 삭제 완료 콜백 컨텍스트 체인 head 초기화 */

	/* allocate and assign connections for association */
	rc =  nvmf_fc_ls_alloc_connections(assoc, nvmf_transport); /* [한국어] connection 풀 선할당 */
	if (rc != 0) {                                      /* [한국어] 풀 할당 실패 - 지금까지 할당한 자원 롤백 */
		nvmf_fc_ls_free_association(assoc);
		return NULL;
	}

	/* add association to target port's association list */
	nvmf_fc_add_assoc_to_tgt_port(tgtport, assoc, rport); /* [한국어] nport의 association 리스트 등록 + assoc_count 증가 */
	return assoc;                                       /* [한국어] 호출자(process_cass)가 admin connection 추가 진행 */
}

/*
 * [한국어]
 * nvmf_fc_ls_append_del_cb_ctx - 삭제 완료 콜백 컨텍스트를 단일 연결 리스트 끝에 추가
 *
 * @opd_list: 콜백 컨텍스트 체인의 head 포인터 주소 (assoc/conn의 ls_del_op_ctx)
 * @opd: 추가할 새 op context
 *
 * 같은 association/connection 삭제를 여러 caller가 동시에 요청할 수 있으므로, 삭제 완료
 * 시 모두에게 통지하기 위해 콜백 컨텍스트들을 next_op_ctx 단일 연결 리스트로 누적한다.
 * 실제 삭제 완료 시 do_del_*_cbs가 이 리스트를 순회하며 각 콜백을 호출하고 free한다.
 *
 * 실행 컨텍스트: LS 처리 스레드. 단일 스레드 직렬화로 리스트 lock-free.
 *
 * 호출 체인:
 *   _nvmf_fc_delete_association / nvmf_fc_ls_poller_delete_conn -> [본 함수]
 */
static inline void
nvmf_fc_ls_append_del_cb_ctx(struct nvmf_fc_ls_op_ctx **opd_list,
			     struct nvmf_fc_ls_op_ctx *opd)
{
	struct nvmf_fc_ls_op_ctx *nxt;                      /* [한국어] 리스트 말단을 찾기 위한 순회 포인터 */

	if (*opd_list) {                                    /* [한국어] 이미 대기 중인 콜백이 있으면 끝에 append */
		nxt = *opd_list;                            /* [한국어] head부터 시작 */
		while (nxt->next_op_ctx) {                  /* [한국어] 마지막 노드까지 전진 */
			nxt = nxt->next_op_ctx;
		}
		nxt->next_op_ctx = opd;                     /* [한국어] 말단에 새 컨텍스트 연결 */
	} else {                                            /* [한국어] 첫 콜백이면 head로 설정 */
		*opd_list = opd;
	}
}

/*
 * [한국어]
 * nvmf_fc_ls_new_connection - free 풀에서 connection 하나를 꺼내 초기화
 *
 * @assoc: connection이 속할 association
 * @qid: NVMe queue ID (0 = admin queue, 1+ = I/O queue)
 * @esrp_ratio: ERSP(Explicit Response) ratio - N개 명령마다 1개 ERSP
 * @rpi: Remote Port Index
 * @sq_size: Submission Queue 크기 (0-based가 아닌 entry 수)
 * @tgtport: target nport (trid 생성용 WWN 제공)
 * @return: 초기화된 connection 포인터, 풀 고갈 시 NULL
 *
 * Create Association(admin queue, qid=0) 또는 Create Connection(I/O queue) 처리 시
 * 호출되어, 선할당된 avail_fc_conns 풀에서 connection 하나를 O(1)로 꺼내 qpair/식별
 * 정보를 초기화하고 in-use 리스트(fc_conns)로 옮긴다. qid==0이면 admin queue로 기록.
 *
 * 실행 컨텍스트: LS 처리 스레드. 풀 조작은 단일 스레드 직렬화로 lock-free.
 *
 * 호출 체인:
 *   nvmf_fc_ls_process_cass / nvmf_fc_ls_process_cioc -> [본 함수]
 */
static struct spdk_nvmf_fc_conn *
nvmf_fc_ls_new_connection(struct spdk_nvmf_fc_association *assoc, uint16_t qid,
			  uint16_t esrp_ratio, uint16_t rpi, uint16_t sq_size,
			  struct spdk_nvmf_fc_nport *tgtport)
{
	struct spdk_nvmf_fc_conn *fc_conn;                  /* [한국어] free 풀에서 꺼낼 connection */

	fc_conn = TAILQ_FIRST(&assoc->avail_fc_conns);      /* [한국어] 가용 리스트 head에서 꺼내기 (선할당된 풀) */
	if (!fc_conn) {                                     /* [한국어] 풀 소진 - max_qpairs_per_ctrlr 초과 요청 */
		SPDK_ERRLOG("out of connections for association %p\n", assoc);
		return NULL;
	}

	/* Remove from avail list and add to in use. */
	TAILQ_REMOVE(&assoc->avail_fc_conns, fc_conn, assoc_avail_link); /* [한국어] free 리스트에서 제거 */
	memset(fc_conn, 0, sizeof(struct spdk_nvmf_fc_conn)); /* [한국어] 재사용 슬롯을 깨끗이 초기화 (이전 conn 잔여 제거) */

	/* Add conn to association's connection list */
	TAILQ_INSERT_TAIL(&assoc->fc_conns, fc_conn, assoc_link); /* [한국어] in-use connection 리스트에 등록 */
	assoc->conn_count++;                                /* [한국어] association 활성 connection 수 증가 (마지막 0이면 assoc 삭제 트리거) */

	if (qid == 0) {                                     /* [한국어] qid 0은 admin queue - association당 1개 */
		/* AdminQ connection. */
		assoc->aq_conn = fc_conn;                   /* [한국어] admin connection 빠른 참조 캐시 */
	}

	fc_conn->qpair.qid = qid;                           /* [한국어] 공통 qpair 추상화의 queue ID 설정 */
	fc_conn->qpair.sq_head_max = sq_size;               /* [한국어] SQ head 최대값 - flow control 경계 */
	fc_conn->qpair.state = SPDK_NVMF_QPAIR_UNINITIALIZED; /* [한국어] qpair 상태 초기값 - Connect 캡슐 수신 전까지 미완성 */
	fc_conn->qpair.transport = assoc->nvmf_transport;   /* [한국어] qpair가 속한 트랜스포트 역참조 (nvmf 코어가 사용) */
	TAILQ_INIT(&fc_conn->qpair.outstanding);            /* [한국어] 진행 중 nvmf_request 리스트 초기화 */

	fc_conn->conn_id = NVMF_FC_INVALID_CONN_ID;         /* [한국어] connection ID는 poller가 add_connection 시 부여 - 우선 무효값 */
	fc_conn->esrp_ratio = esrp_ratio;                   /* [한국어] ERSP ratio 저장 - 응답 빈도 결정 */
	fc_conn->fc_assoc = assoc;                          /* [한국어] 소속 association 역참조 */
	fc_conn->s_id = assoc->s_id;                        /* [한국어] 호스트 source N_Port_ID 전파 */
	fc_conn->d_id = assoc->tgtport->d_id;               /* [한국어] target destination N_Port_ID 전파 (FC frame 헤더용) */
	fc_conn->rpi = rpi;                                 /* [한국어] remote port index - hwqp rport 해시 키 */
	fc_conn->max_queue_depth = sq_size + 1;             /* [한국어] 큐 깊이 = SQ size + 1 (NVMe는 1개 슬롯을 full 판정에 예약) */
	fc_conn->conn_state = SPDK_NVMF_FC_OBJECT_CREATED;  /* [한국어] connection 객체 상태머신 초기값 = CREATED */
	TAILQ_INIT(&fc_conn->in_use_reqs);                  /* [한국어] 이 conn의 진행 중 fc_request 리스트 초기화 */
	TAILQ_INIT(&fc_conn->fused_waiting_queue);          /* [한국어] fused 명령(compare-and-write) 대기 큐 초기화 */

	/* save target port trid in connection (for subsystem
	 * listener validation in fabric connect command)
	 */
	nvmf_fc_create_trid(&fc_conn->trid, tgtport->fc_nodename.u.wwn, /* [한국어] WWN(node/port name)으로 transport ID 생성 - Connect 시 listener ACL 검증에 사용 */
			    tgtport->fc_portname.u.wwn);

	return fc_conn;                                     /* [한국어] 호출자가 응답 작성 + poller 등록 진행 */
}

/* End - Allocators/Deallocators (associations, connections, */
/*       poller API data)                                    */
/* ********************************************************* */

/*
 * [한국어]
 * nvmf_fc_ls_find_assoc - nport의 association 리스트에서 assoc_id로 association 검색
 *
 * @tgtport: 검색 대상 target nport
 * @assoc_id: 찾을 association ID (Create Connection/Disconnect LS의 Association ID descriptor)
 * @return: 매칭되는 활성 association, 없거나 ZOMBIE 상태면 NULL
 *
 * Create Connection/Disconnect LS는 기존 association을 참조하므로 ID로 탐색해야 한다.
 * 이미 삭제 중(ZOMBIE)인 association은 못 찾은 것으로 취급해 재사용을 막는다.
 *
 * 실행 컨텍스트: LS 처리 스레드. 리스트는 단일 스레드 직렬화로 lock-free.
 *
 * 호출 체인:
 *   nvmf_fc_ls_process_cioc / nvmf_fc_ls_process_disc / _nvmf_fc_delete_association -> [본 함수]
 */
static inline struct spdk_nvmf_fc_association *
nvmf_fc_ls_find_assoc(struct spdk_nvmf_fc_nport *tgtport, uint64_t assoc_id)
{
	struct spdk_nvmf_fc_association *assoc = NULL;       /* [한국어] 탐색 결과 (못 찾으면 NULL) */

	TAILQ_FOREACH(assoc, &tgtport->fc_associations, link) { /* [한국어] nport의 모든 association 선형 순회 */
		if (assoc->assoc_id == assoc_id) {          /* [한국어] ID 일치 */
			if (assoc->assoc_state == SPDK_NVMF_FC_OBJECT_ZOMBIE) { /* [한국어] 삭제 완료 대기(ZOMBIE) 상태면 무효 처리 */
				assoc = NULL;
			}
			break;                              /* [한국어] 일치하는 ID는 유일하므로 즉시 종료 */
		}
	}
	return assoc;
}

/*
 * [한국어]
 * nvmf_fc_add_assoc_to_tgt_port - association을 nport/rport 리스트에 등록하고 카운트 증가
 *
 * @tgtport: 등록 대상 target nport
 * @assoc: 등록할 association
 * @rport: 호스트 remote port (assoc_count 추적)
 *
 * association 생성 성공 시 nport의 fc_associations 리스트에 삽입하고, nport와 rport의
 * association 카운트를 증가시킨다. 카운트는 port offline/cleanup 시 0 도달 판정에 사용.
 *
 * 실행 컨텍스트: LS 처리 스레드(new_association 내부). 단일 스레드 직렬화로 lock-free.
 *
 * 호출 체인:
 *   nvmf_fc_ls_new_association -> [본 함수]
 */
static inline void
nvmf_fc_add_assoc_to_tgt_port(struct spdk_nvmf_fc_nport *tgtport,
			      struct spdk_nvmf_fc_association *assoc,
			      struct spdk_nvmf_fc_remote_port_info *rport)
{
	TAILQ_INSERT_TAIL(&tgtport->fc_associations, assoc, link); /* [한국어] nport association 리스트 말단에 추가 */
	tgtport->assoc_count++;                             /* [한국어] nport의 association 수 증가 */
	rport->assoc_count++;                               /* [한국어] 호스트 rport의 association 수 증가 (rport cleanup 판정용) */
}

/*
 * [한국어]
 * nvmf_fc_del_assoc_from_tgt_port - association을 nport/rport 리스트에서 제거하고 카운트 감소
 *
 * @assoc: 제거할 association (마지막 connection이 사라진 시점)
 *
 * add_assoc_to_tgt_port의 역연산. 마지막 connection 삭제 콜백에서 호출되어 nport의
 * association 리스트에서 제거하고 nport/rport 카운트를 감소시킨다.
 *
 * 실행 컨텍스트: LS 처리 스레드(del_connection 내부). 단일 스레드 직렬화로 lock-free.
 *
 * 호출 체인:
 *   nvmf_fc_del_connection(마지막 conn) -> [본 함수]
 */
static inline void
nvmf_fc_del_assoc_from_tgt_port(struct spdk_nvmf_fc_association *assoc)
{
	struct spdk_nvmf_fc_nport *tgtport = assoc->tgtport; /* [한국어] association이 속한 nport 역참조 */

	TAILQ_REMOVE(&tgtport->fc_associations, assoc, link); /* [한국어] nport association 리스트에서 제거 */
	tgtport->assoc_count--;                             /* [한국어] nport association 수 감소 */
	assoc->rport->assoc_count--;                        /* [한국어] 호스트 rport association 수 감소 */
}

/*
 * [한국어]
 * nvmf_fc_do_del_conn_cbs - connection 삭제 완료 시 대기 중인 모든 콜백 실행
 *
 * @opd: 콜백 컨텍스트 단일 연결 리스트 head (append_del_cb_ctx로 누적된)
 * @ret: 삭제 결과 코드 (현재 사용처에서는 0)
 *
 * connection 삭제가 완료되면 그 동안 누적된 모든 op context를 순회하며 (1) 미응답 LS가
 * 있으면 LS 응답을 송신하고, (2) 등록된 완료 콜백을 호출한 뒤, (3) 컨텍스트를 free한다.
 * 같은 connection 삭제를 여러 caller가 요청했을 수 있으므로 체인을 모두 비운다.
 *
 * 실행 컨텍스트: LS 처리 스레드(poller 삭제 콜백 컨텍스트). 단일 스레드 직렬화.
 *
 * 호출 체인:
 *   nvmf_fc_ls_poller_delete_conn_cb -> [본 함수] -> nvmf_fc_xmt_ls_rsp + del_conn_cb
 */
static void
nvmf_fc_do_del_conn_cbs(struct nvmf_fc_ls_op_ctx *opd,
			int ret)
{
	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "performing delete conn. callbacks\n");           /* [한국어] 콜백 처리 시작 트레이스 */
	while (opd) {                                       /* [한국어] 콜백 컨텍스트 체인을 끝까지 순회 */
		struct nvmf_fc_ls_op_ctx *nxt = opd->next_op_ctx; /* [한국어] 현재 노드 free 전에 다음 노드 미리 저장 (use-after-free 방지) */
		struct spdk_nvmf_fc_ls_del_conn_api_data *dp = &opd->u.del_conn; /* [한국어] union에서 del_conn 변형 추출 */

		if (dp->ls_rqst) {                          /* [한국어] LS 요청이 동반된 삭제면 호스트에 응답 송신 필요 */
			if (nvmf_fc_xmt_ls_rsp(dp->ls_rqst->nport, dp->ls_rqst) != 0) { /* [한국어] LS 응답 송신 - LLD로 위임 */
				SPDK_ERRLOG("Send LS response for delete connection failed\n");
			}
		}
		if (dp->del_conn_cb) {                      /* [한국어] 외부 삭제 완료 콜백이 등록돼 있으면 호출 */
			dp->del_conn_cb(dp->del_conn_cb_data);
		}
		free(opd);                                  /* [한국어] 처리 완료한 컨텍스트 해제 */
		opd = nxt;                                  /* [한국어] 다음 노드로 진행 */
	}
}

/*
 * [한국어]
 * nvmf_fc_ls_poller_delete_conn_cb - poller가 connection 삭제(I/O abort 포함)를 끝낸 뒤의 완료 콜백
 *
 * @cb_data: nvmf_fc_ls_op_ctx 포인터 (del_conn 변형)
 * @ret: poller API 결과 코드
 *
 * HWQP poller가 DEL_CONNECTION 처리(진행 중 I/O abort, lookup 해시 제거)를 완료하면
 * spdk_thread_send_msg를 통해 LS 스레드에서 본 콜백이 실행된다. 여기서 association
 * 자료구조 차원의 connection 해제(del_connection)를 수행하고 누적된 콜백들을 실행한다.
 *
 * 실행 컨텍스트: cb_thread(= LS 처리 스레드)로 cross-thread 디스패치되어 실행.
 *   poller는 HWQP 스레드에서 동작하지만 자료구조 변경은 LS 스레드로 직렬화해 lock-free 유지.
 *
 * 호출 체인:
 *   poller(HWQP) DEL_CONNECTION 완료 -> perform_cb -> spdk_thread_send_msg -> [본 함수]
 *     -> nvmf_fc_del_connection + nvmf_fc_do_del_conn_cbs
 */
static void
nvmf_fc_ls_poller_delete_conn_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct nvmf_fc_ls_op_ctx *opd =                     /* [한국어] poller가 전달한 op context 복원 */
		(struct nvmf_fc_ls_op_ctx *)cb_data;
	struct spdk_nvmf_fc_ls_del_conn_api_data *dp = &opd->u.del_conn; /* [한국어] del_conn 변형 추출 */
	struct spdk_nvmf_fc_conn *fc_conn = dp->args.fc_conn; /* [한국어] 삭제 대상 connection */
	struct spdk_nvmf_fc_association *assoc = fc_conn->fc_assoc; /* [한국어] 소속 association */
	struct nvmf_fc_ls_op_ctx *opd_list = (struct nvmf_fc_ls_op_ctx *)fc_conn->ls_del_op_ctx; /* [한국어] 이 conn에 누적된 모든 삭제 콜백 체인 */

	SPDK_DEBUGLOG(nvmf_fc_ls, "Poller Delete connection callback "
		      "for assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		      fc_conn->conn_id);                    /* [한국어] 어느 assoc/conn 삭제 콜백인지 트레이스 */

	nvmf_fc_del_connection(assoc, fc_conn);             /* [한국어] association 리스트에서 conn 제거 + 풀 반환 (마지막이면 assoc도 삭제) */
	nvmf_fc_do_del_conn_cbs(opd_list, 0);               /* [한국어] 누적된 LS 응답/콜백 일괄 실행 */
}

/*
 * [한국어]
 * nvmf_fc_ls_poller_delete_conn - connection 삭제를 HWQP poller에 요청
 *
 * @fc_conn: 삭제할 connection
 * @send_abts: 진행 중 I/O에 ABTS(Abort) frame 송신 여부
 * @ls_rqst: 동반 LS 요청 (Disconnect-connection LS 경로면 응답 송신용, 아니면 NULL)
 * @backend_initiated: target 측 자발적 삭제 여부 (호스트 LS와 구분)
 * @cb_fn: 삭제 완료 외부 콜백 (없으면 NULL)
 * @cb_data: 콜백 인자
 * @return: 0 성공, -ENOMEM op context 할당 실패
 *
 * connection의 실제 종료(진행 중 I/O abort, 하드웨어 자원 해제)는 HWQP poller 스레드에서
 * 수행해야 하므로, 삭제 컨텍스트를 만들어 poller API DEL_CONNECTION 메시지로 위임한다.
 * 상태가 CREATED일 때만 TO_BE_DELETED로 전이시켜 중복 삭제를 막는다(idempotent).
 * 이미 삭제 진행 중이면 콜백 컨텍스트만 체인에 추가하고 별도 동작 없이 반환한다.
 *
 * 실행 컨텍스트: LS 처리 스레드. poller로의 전달은 spdk_thread_send_msg 기반 cross-thread.
 *
 * 호출 체인:
 *   _nvmf_fc_delete_association / nvmf_fc_ls_add_conn_cb / nvmf_fc_delete_connection
 *     -> [본 함수] -> nvmf_fc_poller_api_func(DEL_CONNECTION)
 */
static int
nvmf_fc_ls_poller_delete_conn(struct spdk_nvmf_fc_conn *fc_conn, bool send_abts,
			      struct spdk_nvmf_fc_ls_rqst *ls_rqst, bool backend_initiated,
			      spdk_nvmf_fc_del_conn_cb cb_fn, void *cb_data)
{
	struct spdk_nvmf_fc_association *assoc = fc_conn->fc_assoc; /* [한국어] 삭제 대상이 속한 association */
	struct spdk_nvmf_fc_ls_del_conn_api_data *api_data; /* [한국어] poller에 넘길 삭제 인자 묶음 */
	struct nvmf_fc_ls_op_ctx *opd = NULL;               /* [한국어] 삭제 콜백 컨텍스트 */

	SPDK_DEBUGLOG(nvmf_fc_ls, "Poller Delete connection "
		      "for assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		      fc_conn->conn_id);                    /* [한국어] 삭제 요청 트레이스 */

	/* create context for delete connection API */
	opd = calloc(1, sizeof(struct nvmf_fc_ls_op_ctx)); /* [한국어] op context 할당 (poller 완료 후 콜백 정보 보관) */
	if (!opd) {                                         /* [한국어] 할당 실패 */
		SPDK_ERRLOG("Mem alloc failed for del conn op data");
		return -ENOMEM;
	}

	api_data = &opd->u.del_conn;                        /* [한국어] union의 del_conn 변형 선택 */
	api_data->assoc = assoc;                            /* [한국어] 소속 association 기록 */
	api_data->ls_rqst = ls_rqst;                        /* [한국어] 완료 시 응답할 LS (없으면 NULL) */
	api_data->del_conn_cb = cb_fn;                      /* [한국어] 외부 완료 콜백 */
	api_data->del_conn_cb_data = cb_data;               /* [한국어] 외부 콜백 인자 */
	api_data->aq_conn = (assoc->aq_conn == fc_conn ? true : false); /* [한국어] admin queue connection 여부 표시 */
	api_data->args.fc_conn = fc_conn;                   /* [한국어] poller에 전달할 대상 conn */
	api_data->args.send_abts = send_abts;               /* [한국어] ABTS 송신 여부 전달 */
	api_data->args.backend_initiated = backend_initiated; /* [한국어] 자발적 삭제 여부 전달 */
	api_data->args.hwqp = fc_conn->hwqp;                /* [한국어] 이 conn이 바인딩된 HWQP (poller 라우팅 대상) */
	api_data->args.cb_info.cb_thread = spdk_get_thread(); /* [한국어] 완료 콜백을 실행할 스레드 = 현재 LS 스레드 (cross-thread 복귀 지점) */
	api_data->args.cb_info.cb_func = nvmf_fc_ls_poller_delete_conn_cb; /* [한국어] poller 완료 시 호출될 함수 */
	api_data->args.cb_info.cb_data = opd;               /* [한국어] 콜백에 op context 자기참조 전달 */

	nvmf_fc_ls_append_del_cb_ctx((struct nvmf_fc_ls_op_ctx **) &fc_conn->ls_del_op_ctx, opd); /* [한국어] conn 삭제 콜백 체인에 누적 (중복 요청 대비) */

	assert(fc_conn->conn_state != SPDK_NVMF_FC_OBJECT_ZOMBIE); /* [한국어] 이미 완전 삭제된 conn에 대한 재삭제는 버그 */
	if (fc_conn->conn_state == SPDK_NVMF_FC_OBJECT_CREATED) { /* [한국어] CREATED 상태에서만 실제 삭제 트리거 (idempotent 보장) */
		fc_conn->conn_state = SPDK_NVMF_FC_OBJECT_TO_BE_DELETED; /* [한국어] 삭제 진행 중 상태로 전이 - 이후 중복 요청은 콜백만 누적 */
		nvmf_fc_poller_api_func(api_data->args.hwqp, /* [한국어] HWQP poller에게 DEL_CONNECTION 비동기 위임 */
					SPDK_NVMF_FC_POLLER_API_DEL_CONNECTION,
					&api_data->args);
	}

	return 0;                                           /* [한국어] 위임 성공 - 실제 완료는 콜백에서 */
}

/* callback from poller's ADD_Connection event */
/*
 * [한국어]
 * nvmf_fc_ls_add_conn_cb - poller가 connection을 등록 완료한 뒤의 콜백 (LS 응답 송신)
 *
 * @cb_data: nvmf_fc_ls_op_ctx 포인터 (add_conn 변형)
 * @ret: poller API 결과 코드
 *
 * Create Association/Connection 처리에서 connection을 만든 뒤 poller에 ADD_CONNECTION을
 * 위임하면, poller가 conn_id를 부여하고 hwqp 해시에 등록한 다음 본 콜백을 호출한다.
 * 여기서 부여된 conn_id(및 admin queue면 association_id)를 LS Accept 응답에 채워 호스트로
 * 송신한다. 응답 송신 실패 시 방금 만든 connection을 즉시 정리(rollback)한다.
 *
 * 실행 컨텍스트: cb_thread(= LS 처리 스레드)로 cross-thread 디스패치되어 실행.
 *
 * 호출 체인:
 *   poller(HWQP) ADD_CONNECTION 완료 -> spdk_thread_send_msg -> [본 함수]
 *     -> nvmf_fc_xmt_ls_rsp (성공) / nvmf_fc_ls_poller_delete_conn (실패 롤백)
 */
static void
nvmf_fc_ls_add_conn_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct nvmf_fc_ls_op_ctx *opd =                     /* [한국어] poller가 전달한 op context 복원 */
		(struct nvmf_fc_ls_op_ctx *)cb_data;
	struct spdk_nvmf_fc_ls_add_conn_api_data *dp = &opd->u.add_conn; /* [한국어] add_conn 변형 추출 */
	struct spdk_nvmf_fc_association *assoc = dp->assoc; /* [한국어] 소속 association */
	struct spdk_nvmf_fc_nport *tgtport = assoc->tgtport; /* [한국어] 응답 송신할 target nport */
	struct spdk_nvmf_fc_conn *fc_conn = dp->args.fc_conn; /* [한국어] 방금 등록된 connection (conn_id 부여됨) */
	struct spdk_nvmf_fc_ls_rqst *ls_rqst = dp->ls_rqst; /* [한국어] 원본 LS 요청 (응답 버퍼 포함) */

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "add_conn_cb: assoc_id = 0x%lx, conn_id = 0x%lx\n",
		      assoc->assoc_id, fc_conn->conn_id);   /* [한국어] 등록 완료 트레이스 */

	fc_conn->create_opd = NULL;                         /* [한국어] 생성 진행 컨텍스트 참조 해제 (이 opd를 곧 free하므로) */

	if (assoc->assoc_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) { /* [한국어] 등록 대기 중 association이 삭제로 진입한 경우 */
		/* association is already being deleted - don't continue */
		free(opd);                                  /* [한국어] 응답 송신 없이 컨텍스트만 정리하고 종료 */
		return;
	}

	if (dp->aq_conn) {                                  /* [한국어] admin queue connection - Create Association 응답 형식 */
		struct spdk_nvmf_fc_ls_cr_assoc_acc *assoc_acc = /* [한국어] CR_ASSOC Accept IU로 응답 버퍼 해석 */
			(struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		/* put connection and association ID in response */
		to_be64(&assoc_acc->conn_id.connection_id, fc_conn->conn_id); /* [한국어] 부여된 connection ID를 big-endian으로 응답에 기록 */
		assoc_acc->assoc_id.association_id = assoc_acc->conn_id.connection_id; /* [한국어] FC-NVMe에서 association_id = admin connection_id (동일값) */
	} else {                                            /* [한국어] I/O connection - Create Connection 응답 형식 */
		struct spdk_nvmf_fc_ls_cr_conn_acc *conn_acc = /* [한국어] CR_CONN Accept IU로 응답 버퍼 해석 */
			(struct spdk_nvmf_fc_ls_cr_conn_acc *)ls_rqst->rspbuf.virt;
		/* put connection ID in response */
		to_be64(&conn_acc->conn_id.connection_id, fc_conn->conn_id); /* [한국어] connection ID만 응답에 기록 */
	}

	/* send LS response */
	if (nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst) != 0) {    /* [한국어] LS Accept 응답 송신 (LLD 위임) */
		SPDK_ERRLOG("Send LS response for %s failed - cleaning up\n",
			    dp->aq_conn ? "association" : "connection");
		nvmf_fc_ls_poller_delete_conn(fc_conn, false, NULL, false, NULL, NULL); /* [한국어] 송신 실패 - 호스트가 conn_id를 모르므로 방금 만든 conn 롤백 */
	} else {                                            /* [한국어] 송신 성공 */
		SPDK_DEBUGLOG(nvmf_fc_ls,
			      "LS response (conn_id 0x%lx) sent\n", fc_conn->conn_id);
	}

	free(opd);                                          /* [한국어] add_conn op context 해제 */
}

/*
 * [한국어]
 * nvmf_fc_ls_add_conn_failure - connection을 poller에 넘기기 전 단계 실패 시 정리/거부 응답
 *
 * @assoc: connection이 속한 association
 * @ls_rqst: 원본 LS 요청
 * @fc_conn: 정리할 connection
 * @aq_conn: admin queue connection 여부
 *
 * add_conn_to_poller가 fc_req 풀 생성이나 op context 할당에 실패하면, 호스트에 자원 부족
 * (INSUFF_RES) LS Reject를 보내고 방금 만든 connection을 association에서 즉시 제거한다.
 * 이 시점은 아직 poller에 등록 전이므로 poller 경유 없이 직접 del_connection을 호출한다.
 *
 * 실행 컨텍스트: LS 처리 스레드. (외부 링크 가능하도록 non-static이나 본 파일 내부에서만 사용)
 *
 * 호출 체인:
 *   nvmf_fc_ls_add_conn_to_poller(error) -> [본 함수] -> format_rjt + del_connection
 */
void
nvmf_fc_ls_add_conn_failure(
	struct spdk_nvmf_fc_association *assoc,
	struct spdk_nvmf_fc_ls_rqst *ls_rqst,
	struct spdk_nvmf_fc_conn *fc_conn,
	bool aq_conn)
{
	struct spdk_nvmf_fc_ls_cr_assoc_rqst *rqst;         /* [한국어] 원본 요청 IU (ls_cmd echo용) */
	struct spdk_nvmf_fc_ls_cr_assoc_acc *acc;           /* [한국어] 응답 버퍼 (RJT로 덮어씀) */
	struct spdk_nvmf_fc_nport *tgtport = assoc->tgtport; /* [한국어] 응답 송신할 nport */

	if (fc_conn->create_opd) {                          /* [한국어] 생성 op context가 이미 할당돼 있으면 정리 */
		free(fc_conn->create_opd);
		fc_conn->create_opd = NULL;
	}

	rqst	 = (struct spdk_nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt; /* [한국어] 요청 버퍼 해석 (CR_ASSOC/CR_CONN 공통 w0 위치) */
	acc	 = (struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt; /* [한국어] 응답 버퍼 해석 */

	/* send failure response */
	ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,       /* [한국어] 자원 부족 사유로 LS Reject IU 작성, 길이 반환 */
			   FCNVME_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
			   FCNVME_RJT_RC_INSUFF_RES,        /* [한국어] reason = Insufficient Resources */
			   FCNVME_RJT_EXP_NONE, 0);

	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);               /* [한국어] LS Reject 송신 */
	nvmf_fc_del_connection(assoc, fc_conn);             /* [한국어] poller 미등록 conn이므로 직접 association에서 제거 */
}


/*
 * [한국어]
 * nvmf_fc_ls_add_conn_to_poller - 새 connection을 nvmf 타깃에 등록해 poll group/HWQP에 배치
 *
 * @assoc: connection이 속한 association
 * @ls_rqst: 원본 LS 요청 (등록 완료 콜백에서 응답 송신)
 * @fc_conn: 등록할 connection
 * @aq_conn: admin queue connection 여부 (응답 형식 결정)
 *
 * Create Association/Connection 검증과 객체 생성을 마친 뒤 호출된다. connection별 fc_req
 * 풀을 만들고 add_conn op context를 준비한 다음, spdk_nvmf_tgt_new_qpair로 qpair를 타깃에
 * 넘긴다. 그러면 nvmf 코어가 poll group을 선택해 qpair를 배치하고, 최종적으로 poller가
 * ADD_CONNECTION을 수행한 뒤 add_conn_cb에서 LS 응답이 송신된다(응답이 곧 등록 결과).
 *
 * 실행 컨텍스트: LS 처리 스레드. new_qpair 이후 흐름은 cross-thread(poll group)로 이어짐.
 *
 * 호출 체인:
 *   nvmf_fc_ls_process_cass / nvmf_fc_ls_process_cioc -> [본 함수]
 *     -> spdk_nvmf_tgt_new_qpair -> (poller) -> nvmf_fc_ls_add_conn_cb
 */
static void
nvmf_fc_ls_add_conn_to_poller(
	struct spdk_nvmf_fc_association *assoc,
	struct spdk_nvmf_fc_ls_rqst *ls_rqst,
	struct spdk_nvmf_fc_conn *fc_conn,
	bool aq_conn)
{
	struct nvmf_fc_ls_op_ctx *opd;                      /* [한국어] add_conn 콜백 컨텍스트 */
	struct spdk_nvmf_fc_ls_add_conn_api_data *api_data; /* [한국어] poller 콜백 시 사용할 인자 묶음 */

	SPDK_DEBUGLOG(nvmf_fc_ls, "Add Connection to poller for "
		      "assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		      fc_conn->conn_id);                    /* [한국어] poller 등록 시작 트레이스 */

	/* Create fc_req pool for this connection */
	if (nvmf_fc_create_conn_reqpool(fc_conn)) {         /* [한국어] 이 connection 전용 fc_request 풀 생성 (I/O 처리에 필요) */
		SPDK_ERRLOG("allocate fc_req pool failed\n");
		goto error;                                 /* [한국어] 풀 생성 실패 - 거부 응답 후 정리 */
	}

	opd = calloc(1, sizeof(struct nvmf_fc_ls_op_ctx)); /* [한국어] add_conn op context 할당 */
	if (!opd) {                                         /* [한국어] 할당 실패 */
		SPDK_ERRLOG("allocate api data for add conn op failed\n");
		goto error;
	}

	api_data = &opd->u.add_conn;                        /* [한국어] union의 add_conn 변형 선택 */

	api_data->args.fc_conn = fc_conn;                   /* [한국어] 등록 대상 conn */
	api_data->args.cb_info.cb_thread = spdk_get_thread(); /* [한국어] 등록 완료 콜백을 실행할 스레드 = 현재 LS 스레드 */
	api_data->args.cb_info.cb_func = nvmf_fc_ls_add_conn_cb; /* [한국어] poller ADD 완료 시 호출될 함수 */
	api_data->args.cb_info.cb_data = (void *)opd;       /* [한국어] 콜백에 op context 자기참조 전달 */
	api_data->assoc = assoc;                            /* [한국어] 소속 association */
	api_data->ls_rqst = ls_rqst;                        /* [한국어] 응답 송신할 LS 요청 */
	api_data->aq_conn = aq_conn;                        /* [한국어] admin/IO 구분 (응답 형식) */

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "New QP callback called.\n");         /* [한국어] qpair 등록 진입 트레이스 */

	/* Let the nvmf_tgt decide which pollgroup to use. */
	fc_conn->create_opd = opd;                          /* [한국어] 진행 중 op context를 conn에 보관 (실패 정리 경로에서 회수) */
	spdk_nvmf_tgt_new_qpair(ls_rqst->nvmf_tgt, &fc_conn->qpair); /* [한국어] nvmf 코어에 qpair 인계 - poll group 선택 + poller ADD_CONNECTION 유발 */
	return;
error:                                                  /* [한국어] 등록 전 단계 실패 공통 처리 */
	nvmf_fc_ls_add_conn_failure(assoc, ls_rqst, fc_conn, aq_conn); /* [한국어] 거부 응답 + conn 정리 */
}

/* Delete association functions */

/*
 * [한국어]
 * nvmf_fc_do_del_assoc_cbs - association 삭제 완료 시 대기 중인 모든 콜백 실행
 *
 * @opd: 콜백 컨텍스트 단일 연결 리스트 head
 * @ret: 삭제 결과 코드 (콜백에 전달)
 *
 * association이 완전히 삭제되면(마지막 connection까지 정리됨) 그 동안 누적된 모든 삭제
 * 요청자(예: 호스트 LS Disconnect, RPC, port offline)의 콜백을 순회 호출하고 free한다.
 *
 * 실행 컨텍스트: LS 처리 스레드(마지막 connection 삭제 콜백 컨텍스트).
 *
 * 호출 체인:
 *   nvmf_fc_del_connection(마지막 conn) -> [본 함수] -> del_assoc_cb (각 요청자)
 */
static void
nvmf_fc_do_del_assoc_cbs(struct nvmf_fc_ls_op_ctx *opd, int ret)
{
	struct nvmf_fc_ls_op_ctx *nxt;                      /* [한국어] free 전 다음 노드 보관 */
	struct spdk_nvmf_fc_delete_assoc_api_data *dp;      /* [한국어] del_assoc 변형 */

	while (opd) {                                       /* [한국어] 콜백 체인 끝까지 순회 */
		dp = &opd->u.del_assoc;                     /* [한국어] union에서 del_assoc 추출 */

		SPDK_DEBUGLOG(nvmf_fc_ls, "performing delete assoc. callback\n");
		dp->del_assoc_cb(dp->del_assoc_cb_data, ret); /* [한국어] 요청자 완료 콜백 호출 (RPC 응답/추가 정리 등) */

		nxt = opd->next_op_ctx;                     /* [한국어] 다음 노드 저장 */
		free(opd);                                  /* [한국어] 처리한 컨텍스트 해제 */
		opd = nxt;                                  /* [한국어] 진행 */
	}
}

/*
 * [한국어]
 * nvmf_fs_send_ls_disconnect_cb - target->host LS Disconnect 송신 완료 콜백 (버퍼 해제)
 *
 * @hwqp: 송신을 수행한 HWQP (미사용)
 * @status: 송신 결과 상태 (미사용 - 성공/실패 무관하게 버퍼 회수)
 * @args: 해제할 SRSR 버퍼쌍 포인터
 *
 * NVMF_FC_LS_SEND_LS_DISCONNECT가 활성일 때, association 삭제 마무리로 호스트에 LS
 * Disconnect를 보낸 뒤 그 DMA 버퍼를 회수하는 콜백. LLD가 송신 완료 시 호출한다.
 *
 * 실행 컨텍스트: LLD 송신 완료 콜백 컨텍스트.
 *
 * 호출 체인:
 *   nvmf_fc_del_connection -> nvmf_fc_xmt_srsr_req(... cb) -> (LLD) -> [본 함수]
 */
static void
nvmf_fs_send_ls_disconnect_cb(void *hwqp, int32_t status, void *args)
{
	if (args) {                                         /* [한국어] 회수할 버퍼가 전달됐으면 */
		SPDK_DEBUGLOG(nvmf_fc_ls, "free disconnect buffers\n");
		nvmf_fc_free_srsr_bufs((struct spdk_nvmf_fc_srsr_bufs *)args); /* [한국어] LS Disconnect 송신용 DMA 버퍼쌍 해제 */
	}
}

/*
 * [한국어]
 * nvmf_fc_del_connection - connection을 association 자료구조에서 제거 (마지막이면 association도 삭제)
 *
 * @assoc: connection이 속한 association
 * @fc_conn: 제거할 connection
 *
 * poller가 connection 종료(I/O abort, 하드웨어 해제)를 마친 뒤 호출되어, connection을
 * in-use 리스트에서 빼 free 풀로 되돌리고 ZOMBIE로 표시한다. 이것이 association의 마지막
 * connection이면 association을 nport 리스트에서 제거하고, (옵션) 호스트에 LS Disconnect를
 * 송신한 뒤 association을 free하며 누적된 삭제 콜백들을 실행한다.
 *
 * 실행 컨텍스트: LS 처리 스레드(poller 삭제 완료 콜백 컨텍스트). 자료구조 변경 lock-free.
 *
 * 호출 체인:
 *   nvmf_fc_ls_poller_delete_conn_cb / nvmf_fc_ls_add_conn_failure -> [본 함수]
 *     -> (마지막) nvmf_fc_ls_free_association + nvmf_fc_do_del_assoc_cbs
 */
static void
nvmf_fc_del_connection(struct spdk_nvmf_fc_association *assoc,
		       struct spdk_nvmf_fc_conn *fc_conn)
{
	/* Free connection specific fc_req pool */
	nvmf_fc_free_conn_reqpool(fc_conn);                 /* [한국어] 이 connection 전용 fc_request 풀 해제 */

	/* remove connection from association's connection list */
	TAILQ_REMOVE(&assoc->fc_conns, fc_conn, assoc_link); /* [한국어] in-use connection 리스트에서 제거 */

	/* Give back connection to association's free pool */
	TAILQ_INSERT_TAIL(&assoc->avail_fc_conns, fc_conn, assoc_avail_link); /* [한국어] free 풀로 반환 (선할당 슬롯 재사용) */

	fc_conn->conn_state = SPDK_NVMF_FC_OBJECT_ZOMBIE;   /* [한국어] 완전 삭제됨 표시 - find_assoc/재삭제 차단 */
	fc_conn->ls_del_op_ctx = NULL;                      /* [한국어] 콜백 체인 참조 해제 (do_del_conn_cbs가 별도 보관) */

	if (--assoc->conn_count == 0) {                     /* [한국어] 활성 connection 수 감소 - 0이면 association도 정리 */
		/* last connection - remove association from target port's association list */
		struct nvmf_fc_ls_op_ctx *cb_opd = (struct nvmf_fc_ls_op_ctx *)assoc->ls_del_op_ctx; /* [한국어] association 삭제 콜백 체인 보관 (free 전) */

		SPDK_DEBUGLOG(nvmf_fc_ls,
			      "remove assoc. %lx\n", assoc->assoc_id); /* [한국어] association 제거 트레이스 */
		nvmf_fc_del_assoc_from_tgt_port(assoc);     /* [한국어] nport/rport 리스트에서 제거 + 카운트 감소 */

		if (assoc->snd_disconn_bufs &&              /* [한국어] LS Disconnect 송신 버퍼가 있고 (기능 활성) */
		    assoc->tgtport->fc_port->hw_port_status == SPDK_FC_PORT_ONLINE) { /* [한국어] 포트가 online일 때만 송신 가능 */

			struct spdk_nvmf_fc_ls_disconnect_rqst *dc_rqst; /* [한국어] LS Disconnect 요청 IU */
			struct spdk_nvmf_fc_srsr_bufs *srsr_bufs; /* [한국어] 송신용 DMA 버퍼쌍 */

			dc_rqst = (struct spdk_nvmf_fc_ls_disconnect_rqst *)
				  assoc->snd_disconn_bufs->rqst; /* [한국어] 선할당된 요청 버퍼로 해석 */

			bzero(dc_rqst, sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst)); /* [한국어] 버퍼 0-초기화 (Reserved 안전성) */

			/* fill in request descriptor */
			dc_rqst->w0.ls_cmd = FCNVME_LS_DISCONNECT; /* [한국어] LS Cmd Code = Disconnect (0x05) */
			to_be32(&dc_rqst->desc_list_len,    /* [한국어] descriptor list 길이 (tag+len 8B 제외) big-endian */
				sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst) -
				(2 * sizeof(uint32_t)));

			/* fill in disconnect command descriptor */
			to_be32(&dc_rqst->disconn_cmd.desc_tag, FCNVME_LSDESC_DISCONN_CMD); /* [한국어] Disconnect command descriptor 태그 */
			to_be32(&dc_rqst->disconn_cmd.desc_len, /* [한국어] command descriptor 길이 */
				sizeof(struct spdk_nvmf_fc_lsdesc_disconn_cmd) -
				(2 * sizeof(uint32_t)));

			/* fill in association id descriptor */
			to_be32(&dc_rqst->assoc_id.desc_tag, FCNVME_LSDESC_ASSOC_ID), /* [한국어] Association ID descriptor 태그 (쉼표 연산자 - 원본 코드 유지) */
				to_be32(&dc_rqst->assoc_id.desc_len, /* [한국어] Association ID descriptor 길이 */
					sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id) -
					(2 * sizeof(uint32_t)));
			to_be64(&dc_rqst->assoc_id.association_id, assoc->assoc_id); /* [한국어] 삭제할 association ID를 big-endian으로 기록 */

			srsr_bufs = assoc->snd_disconn_bufs; /* [한국어] 송신할 버퍼쌍 확보 */
			assoc->snd_disconn_bufs = NULL;      /* [한국어] free_association이 이중 해제하지 않도록 소유권 이전 */

			SPDK_DEBUGLOG(nvmf_fc_ls, "Send LS disconnect\n");
			if (nvmf_fc_xmt_srsr_req(&assoc->tgtport->fc_port->ls_queue, /* [한국어] LS 큐로 SRSR(send/recv) 요청 송신 - 완료 시 버퍼 해제 콜백 */
						 srsr_bufs, nvmf_fs_send_ls_disconnect_cb,
						 (void *)srsr_bufs)) {
				SPDK_ERRLOG("Error sending LS disconnect\n");
				assoc->snd_disconn_bufs = srsr_bufs; /* [한국어] 송신 실패 - 소유권 되돌려 free_association이 회수하게 함 */
			}
		}

		nvmf_fc_ls_free_association(assoc);         /* [한국어] association 본체와 부속 자원 해제 */

		/* perform callbacks to all callers to delete association */
		nvmf_fc_do_del_assoc_cbs(cb_opd, 0);        /* [한국어] 누적된 모든 삭제 요청자 콜백 실행 */
	}
}

/* Disconnect/delete (association) request functions */

/*
 * [한국어]
 * _nvmf_fc_delete_association - association 삭제 공통 구현 (LS/RPC/port 경로 통합)
 *
 * @tgtport: association이 속한 target nport
 * @assoc_id: 삭제할 association ID
 * @send_abts: 진행 중 I/O에 ABTS 송신 여부
 * @backend_initiated: target 자발적 삭제 여부
 * @del_assoc_cb: 삭제 완료 콜백
 * @cb_data: 콜백 인자
 * @from_ls_rqst: 호스트 LS Disconnect 경로에서 호출됐는지 (응답 송신 분기용)
 * @return: 0 성공, VERR_NO_ASSOC(association 없음), -ENOMEM
 *
 * association을 TO_BE_DELETED로 표시하고 모든 connection 삭제를 poller에 위임한다. 실제
 * association free는 마지막 connection 삭제 콜백(del_connection)에서 일어나므로 본 함수는
 * 삭제를 "시작"만 한다. 이미 삭제 진행 중이면 콜백만 누적하고 즉시 0 반환(idempotent).
 *
 * 실행 컨텍스트: LS 처리 스레드. connection 삭제는 spdk_thread_send_msg로 poller에 전달.
 *
 * 호출 체인:
 *   nvmf_fc_ls_disconnect_assoc(LS) / nvmf_fc_delete_association(RPC) -> [본 함수]
 *     -> nvmf_fc_ls_poller_delete_conn (각 connection)
 */
static int
_nvmf_fc_delete_association(struct spdk_nvmf_fc_nport *tgtport,
			    uint64_t assoc_id, bool send_abts, bool backend_initiated,
			    spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
			    void *cb_data, bool from_ls_rqst)
{
	int rc;                                             /* [한국어] connection 삭제 결과 코드 */
	struct nvmf_fc_ls_op_ctx *opd;                      /* [한국어] association 삭제 콜백 컨텍스트 */
	struct spdk_nvmf_fc_delete_assoc_api_data *api_data; /* [한국어] del_assoc 변형 */
	struct spdk_nvmf_fc_conn *fc_conn;                  /* [한국어] 순회용 connection 포인터 */
	struct spdk_nvmf_fc_association *assoc =            /* [한국어] assoc_id로 association 탐색 */
		nvmf_fc_ls_find_assoc(tgtport, assoc_id);
	enum spdk_nvmf_fc_object_state assoc_state;         /* [한국어] 진입 시점의 association 상태 스냅샷 */

	SPDK_DEBUGLOG(nvmf_fc_ls, "Delete association, "
		      "assoc_id 0x%lx\n", assoc_id);        /* [한국어] 삭제 시작 트레이스 */

	if (!assoc) {                                       /* [한국어] association을 못 찾음 (이미 삭제됐거나 잘못된 ID) */
		SPDK_ERRLOG("Delete association failed: %s\n",
			    validation_errors[VERR_NO_ASSOC]);
		return VERR_NO_ASSOC;                       /* [한국어] 호출자가 LS Reject reason으로 변환 */
	}

	/* create cb context to put in association's list of
	 * callbacks to call when delete association is done */
	opd = calloc(1, sizeof(struct nvmf_fc_ls_op_ctx)); /* [한국어] 삭제 완료 콜백 컨텍스트 할당 */
	if (!opd) {                                         /* [한국어] 할당 실패 */
		SPDK_ERRLOG("Mem alloc failed for del assoc cb data");
		return -ENOMEM;
	}

	api_data = &opd->u.del_assoc;                       /* [한국어] union의 del_assoc 변형 선택 */
	api_data->assoc = assoc;                            /* [한국어] 삭제 대상 association */
	api_data->from_ls_rqst = from_ls_rqst;              /* [한국어] LS 경로 여부 기록 */
	api_data->del_assoc_cb = del_assoc_cb;              /* [한국어] 완료 콜백 */
	api_data->del_assoc_cb_data = cb_data;              /* [한국어] 콜백 인자 */
	api_data->args.cb_info.cb_data = opd;               /* [한국어] 콜백 자기참조 */
	nvmf_fc_ls_append_del_cb_ctx((struct nvmf_fc_ls_op_ctx **) &assoc->ls_del_op_ctx, opd); /* [한국어] association 삭제 콜백 체인에 누적 */

	assoc_state = assoc->assoc_state;                   /* [한국어] 현재 상태 캡처 */
	if (assoc_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) { /* [한국어] 이미 삭제 진행 중이면 */
		/* association already being deleted */
		return 0;                                   /* [한국어] 콜백만 누적하고 종료 (idempotent) - 완료 시 함께 통지됨 */
	}

	/* mark assoc. to be deleted */
	assoc->assoc_state = SPDK_NVMF_FC_OBJECT_TO_BE_DELETED; /* [한국어] 삭제 진행 중으로 전이 - 이후 새 connection/중복 삭제 차단 */

	/* delete all of the association's connections */
	TAILQ_FOREACH(fc_conn, &assoc->fc_conns, assoc_link) { /* [한국어] 모든 활성 connection 순회하며 삭제 위임 */
		rc = nvmf_fc_ls_poller_delete_conn(fc_conn, send_abts, NULL, backend_initiated, NULL, NULL); /* [한국어] connection 삭제 (LS 미동반=NULL, 콜백 미동반=NULL) */
		if (rc) {                                   /* [한국어] 삭제 위임 실패 (op context 할당 실패 등) */
			SPDK_ERRLOG("Delete connection failed for assoc_id 0x%lx conn_id 0x%lx\n",
				    assoc->assoc_id, fc_conn->conn_id);
			return rc;                          /* [한국어] 오류 전파 */
		}
	}

	return 0;                                           /* [한국어] 삭제 시작 성공 - 실제 완료는 콜백 체인에서 */
}

/*
 * [한국어]
 * nvmf_fc_ls_disconnect_assoc_cb - 호스트 LS Disconnect로 시작된 association 삭제 완료 콜백
 *
 * @cb_data: nvmf_fc_ls_op_ctx 포인터 (disconn_assoc 변형)
 * @err: 삭제 결과 (0 성공, 비0이면 실패)
 *
 * Disconnect LS 처리로 association 삭제가 완료되면 호출되어 호스트에 응답을 보낸다.
 * 성공이면 (이미 작성된) LS Accept를, 실패면 LS Reject(Unable to perform)를 송신한다.
 *
 * 실행 컨텍스트: LS 처리 스레드(association 삭제 완료 콜백 체인).
 *
 * 호출 체인:
 *   nvmf_fc_do_del_assoc_cbs -> [본 함수] -> nvmf_fc_xmt_ls_rsp
 */
static void
nvmf_fc_ls_disconnect_assoc_cb(void *cb_data, uint32_t err)
{
	struct nvmf_fc_ls_op_ctx *opd = (struct nvmf_fc_ls_op_ctx *)cb_data; /* [한국어] op context 복원 */
	struct spdk_nvmf_fc_ls_disconn_assoc_api_data *dp = &opd->u.disconn_assoc; /* [한국어] disconn_assoc 변형 */
	struct spdk_nvmf_fc_nport *tgtport = dp->tgtport;   /* [한국어] 응답 송신할 nport */
	struct spdk_nvmf_fc_ls_rqst *ls_rqst = dp->ls_rqst; /* [한국어] 원본 Disconnect LS 요청 */

	SPDK_DEBUGLOG(nvmf_fc_ls, "Disconnect association callback begin "
		      "nport %d\n", tgtport->nport_hdl);    /* [한국어] 콜백 시작 트레이스 */
	if (err != 0) {                                     /* [한국어] 삭제 실패 시 응답을 Reject로 교체 */
		/* send failure response */
		struct spdk_nvmf_fc_ls_cr_assoc_rqst *rqst = /* [한국어] 요청 IU (ls_cmd echo) */
			(struct spdk_nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
		struct spdk_nvmf_fc_ls_cr_assoc_acc *acc =  /* [한국어] 응답 버퍼 (RJT로 덮어씀) */
			(struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc, /* [한국어] LS Reject IU 작성 */
				   FCNVME_MAX_LS_BUFFER_SIZE,
				   rqst->w0.ls_cmd,
				   FCNVME_RJT_RC_UNAB,       /* [한국어] reason = Unable to perform */
				   FCNVME_RJT_EXP_NONE,
				   0);
	}

	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);               /* [한국어] LS Accept 또는 Reject 송신 */

	free(opd);                                          /* [한국어] disconn_assoc op context 해제 */
	SPDK_DEBUGLOG(nvmf_fc_ls, "Disconnect association callback complete "
		      "nport %d err %d\n", tgtport->nport_hdl, err); /* [한국어] 콜백 완료 트레이스 */
}

/*
 * [한국어]
 * nvmf_fc_ls_disconnect_assoc - Disconnect LS 처리에서 association 삭제를 시작
 *
 * @tgtport: target nport
 * @ls_rqst: 원본 Disconnect LS 요청
 * @assoc_id: 삭제할 association ID
 *
 * Disconnect LS 검증을 통과한 뒤 호출되어 disconn_assoc 컨텍스트를 만들고
 * _nvmf_fc_delete_association을 from_ls_rqst=true로 호출한다. 삭제가 시작되면 완료 시
 * disconnect_assoc_cb에서 응답이 송신되므로 여기서는 즉시 반환한다. 삭제 시작 자체가
 * 실패하면(자원 부족/association 없음/논리 오류) 그 자리에서 LS Reject를 송신한다.
 *
 * 실행 컨텍스트: LS 처리 스레드.
 *
 * 호출 체인:
 *   nvmf_fc_ls_process_disc -> [본 함수] -> _nvmf_fc_delete_association
 */
static void
nvmf_fc_ls_disconnect_assoc(struct spdk_nvmf_fc_nport *tgtport,
			    struct spdk_nvmf_fc_ls_rqst *ls_rqst, uint64_t assoc_id)
{
	struct nvmf_fc_ls_op_ctx *opd;                      /* [한국어] disconn_assoc 콜백 컨텍스트 */
	struct spdk_nvmf_fc_ls_cr_assoc_rqst *rqst =        /* [한국어] 요청 IU (ls_cmd echo) */
		(struct spdk_nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
	struct spdk_nvmf_fc_ls_cr_assoc_acc *acc =          /* [한국어] 응답 버퍼 (실패 시 RJT) */
		(struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_fc_ls_disconn_assoc_api_data *api_data; /* [한국어] disconn_assoc 변형 */
	int ret;                                            /* [한국어] 삭제 시작 결과 */
	uint8_t reason = 0;                                 /* [한국어] LS Reject reason code */

	opd = calloc(1, sizeof(struct nvmf_fc_ls_op_ctx)); /* [한국어] 콜백 컨텍스트 할당 */
	if (!opd) {                                         /* [한국어] 할당 실패 */
		/* send failure response */
		SPDK_ERRLOG("Allocate disconn assoc op data failed\n");
		reason = FCNVME_RJT_RC_INSUFF_RES;          /* [한국어] 자원 부족으로 거부 */
		goto send_rjt;
	}

	api_data = &opd->u.disconn_assoc;                   /* [한국어] union의 disconn_assoc 선택 */
	api_data->tgtport = tgtport;                        /* [한국어] 완료 콜백에서 응답 송신할 nport */
	api_data->ls_rqst = ls_rqst;                        /* [한국어] 완료 콜백에서 응답할 LS */
	ret = _nvmf_fc_delete_association(tgtport, assoc_id, /* [한국어] association 삭제 시작 (LS 경로=true, ABTS/backend=false) */
					  false, false,
					  nvmf_fc_ls_disconnect_assoc_cb,
					  api_data, true);
	if (!ret) {                                         /* [한국어] 삭제 시작 성공 - 완료 콜백이 응답 담당 */
		return;
	}

	/* delete association failed */
	switch (ret) {                                      /* [한국어] 삭제 시작 실패 코드 -> LS Reject reason 매핑 */
	case VERR_NO_ASSOC:                                 /* [한국어] association 없음 */
		reason = FCNVME_RJT_RC_INV_ASSOC;           /* [한국어] Invalid Association ID */
		break;
	case -ENOMEM:                                       /* [한국어] 메모리 부족 */
		reason = FCNVME_RJT_RC_INSUFF_RES;          /* [한국어] Insufficient Resources */
		break;
	default:                                            /* [한국어] 기타 - 논리 오류 */
		reason = FCNVME_RJT_RC_LOGIC;
	}

	free(opd);                                          /* [한국어] 삭제 시작 실패 - 콜백 컨텍스트는 쓰이지 않으므로 해제 */

send_rjt:                                               /* [한국어] LS Reject 송신 공통 처리 */
	ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,       /* [한국어] reason 코드로 LS Reject IU 작성 */
			   FCNVME_MAX_LS_BUFFER_SIZE,
			   rqst->w0.ls_cmd, reason,
			   FCNVME_RJT_EXP_NONE, 0);
	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);               /* [한국어] LS Reject 송신 */
}

/*
 * [한국어]
 * nvmf_fc_ls_validate_host - 호스트 NQN이 subsystem ACL에서 허용되는지 검증
 *
 * @subsystem: 대상 NVMe subsystem
 * @hostnqn: Create Association LS가 제시한 호스트 NQN
 * @return: 0 허용, -EPERM 거부
 *
 * subsystem은 허용 호스트 목록(allow any host 또는 명시 ACL)을 가지므로, association
 * 수립 전에 호스트가 접근 권한이 있는지 확인한다. 거부 시 LS Reject(Invalid Host).
 *
 * 실행 컨텍스트: LS 처리 스레드.
 *
 * 호출 체인:
 *   nvmf_fc_ls_process_cass -> [본 함수] -> spdk_nvmf_subsystem_host_allowed
 */
static int
nvmf_fc_ls_validate_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{

	if (!spdk_nvmf_subsystem_host_allowed(subsystem, hostnqn)) { /* [한국어] subsystem ACL 조회 - 미허용 호스트면 false */
		return -EPERM;                              /* [한국어] 권한 없음 - 호출자가 INV_HOST reject로 변환 */
	}

	return 0;                                           /* [한국어] 허용 */
}

/* **************************** */
/* LS Request Handler Functions */

/*
 * [한국어]
 * nvmf_fc_ls_process_cass - Create Association LS 처리 (association + admin queue 생성)
 *
 * @s_id: LS frame source N_Port_ID (호스트 포트)
 * @tgtport: LS를 수신한 target nport
 * @ls_rqst: LS 요청 컨텍스트 (요청/응답 버퍼 포함)
 *
 * 호스트가 보내는 첫 NVMe-FC LS. (1) IU/descriptor 길이·태그·ERSP ratio·SQ size를 엄격히
 * 검증하고, (2) subnqn으로 subsystem을 찾아 hostnqn ACL을 확인한 뒤, (3) association과
 * admin queue connection(qid 0)을 생성하고, (4) LS Accept 응답 헤더를 작성한 다음
 * add_conn_to_poller로 connection을 poll group에 배치한다(응답은 등록 콜백에서 송신).
 * 검증/할당 실패 시 rjt_cass 라벨로 점프해 적절한 LS Reject를 송신한다.
 *
 * 실행 컨텍스트: LS 처리 스레드(LS HWQP 바인딩 poll group). 단일 스레드 직렬화.
 *
 * 호출 체인:
 *   nvmf_fc_handle_ls_rqst -> [본 함수]
 *     -> nvmf_fc_ls_new_association + nvmf_fc_ls_new_connection + nvmf_fc_ls_add_conn_to_poller
 */
static void
nvmf_fc_ls_process_cass(uint32_t s_id,
			struct spdk_nvmf_fc_nport *tgtport,
			struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_cr_assoc_rqst *rqst =        /* [한국어] CR_ASSOC 요청 IU로 요청 버퍼 해석 */
		(struct spdk_nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
	struct spdk_nvmf_fc_ls_cr_assoc_acc *acc =          /* [한국어] CR_ASSOC Accept IU로 응답 버퍼 해석 */
		(struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_fc_association *assoc;              /* [한국어] 생성할 association */
	struct spdk_nvmf_fc_conn *fc_conn;                  /* [한국어] 생성할 admin queue connection */
	struct spdk_nvmf_subsystem *subsystem = NULL;       /* [한국어] subnqn으로 찾을 대상 subsystem */
	const char *hostnqn = (const char *)rqst->assoc_cmd.hostnqn; /* [한국어] 요청의 호스트 NQN 포인터 (ACL 검증용) */
	int errmsg_ind = 0;                                 /* [한국어] validation_errors[] 인덱스 (로깅용) */
	uint8_t rc = FCNVME_RJT_RC_NONE;                    /* [한국어] LS Reject reason code (NONE=거부 안 함) */
	uint8_t ec = FCNVME_RJT_EXP_NONE;                   /* [한국어] LS Reject explanation code */
	struct spdk_nvmf_transport *transport = spdk_nvmf_tgt_get_transport(ls_rqst->nvmf_tgt, /* [한국어] FC 트랜스포트 핸들 - 한도(max_aq_depth) 검증용 */
						SPDK_NVME_TRANSPORT_NAME_FC);

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "LS_CASS: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d, sq_size=%d, "
		      "Subnqn: %s, Hostnqn: %s, Tgtport nn:%lx, pn:%lx\n",
		      ls_rqst->rqst_len, from_be32(&rqst->desc_list_len),
		      from_be32(&rqst->assoc_cmd.desc_len),
		      from_be32(&rqst->assoc_cmd.sqsize),
		      rqst->assoc_cmd.subnqn, hostnqn,
		      tgtport->fc_nodename.u.wwn, tgtport->fc_portname.u.wwn); /* [한국어] 요청 파라미터 일괄 트레이스 (FC 빅엔디안 -> 호스트 endian 변환) */

	if (ls_rqst->rqst_len < FCNVME_LS_CA_CMD_MIN_LEN) { /* [한국어] [검증1] 전체 IU 길이가 최소 길이 미만 */
		SPDK_ERRLOG("assoc_cmd req len = %d, should be at least %d\n",
			    ls_rqst->rqst_len, FCNVME_LS_CA_CMD_MIN_LEN);
		errmsg_ind = VERR_CR_ASSOC_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;               /* [한국어] Invalid Parameter */
		ec = FCNVME_RJT_EXP_INV_LEN;                /* [한국어] Invalid Length Field */
	} else if (from_be32(&rqst->desc_list_len) <        /* [한국어] [검증2] descriptor list 길이 부족 */
		   FCNVME_LS_CA_DESC_LIST_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd desc list len = %d, should be at least %d\n",
			    from_be32(&rqst->desc_list_len),
			    FCNVME_LS_CA_DESC_LIST_MIN_LEN);
		errmsg_ind = VERR_CR_ASSOC_RQST_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->assoc_cmd.desc_tag !=              /* [한국어] [검증3] Command descriptor 태그가 CREATE_ASSOC가 아님 */
		   cpu_to_be32(FCNVME_LSDESC_CREATE_ASSOC_CMD)) {
		errmsg_ind = VERR_CR_ASSOC_CMD;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (from_be32(&rqst->assoc_cmd.desc_len) <   /* [한국어] [검증4] Command descriptor 길이 부족 */
		   FCNVME_LS_CA_DESC_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd desc len = %d, should be at least %d\n",
			    from_be32(&rqst->assoc_cmd.desc_len),
			    FCNVME_LS_CA_DESC_MIN_LEN);
		errmsg_ind = VERR_CR_ASSOC_CMD_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (!rqst->assoc_cmd.ersp_ratio ||           /* [한국어] [검증5] ERSP ratio가 0이거나 SQ size 이상 (비유효) */
		   (from_be16(&rqst->assoc_cmd.ersp_ratio) >=
		    from_be16(&rqst->assoc_cmd.sqsize))) {
		errmsg_ind = VERR_ERSP_RATIO;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_ESRP;               /* [한국어] Invalid ERSP Ratio */
	} else if (from_be16(&rqst->assoc_cmd.sqsize) == 0 || /* [한국어] [검증6] SQ size가 0이거나 admin queue 한도 초과 */
		   from_be16(&rqst->assoc_cmd.sqsize) > transport->opts.max_aq_depth) {
		errmsg_ind = VERR_SQSIZE;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_SQ_SIZE;                /* [한국어] Invalid SQ Size */
	}

	if (rc != FCNVME_RJT_RC_NONE) {                     /* [한국어] 형식 검증 중 하나라도 실패하면 거부로 점프 */
		goto rjt_cass;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(ls_rqst->nvmf_tgt, rqst->assoc_cmd.subnqn); /* [한국어] subnqn으로 등록된 subsystem 조회 */
	if (subsystem == NULL) {                            /* [한국어] 해당 subsystem 없음 */
		errmsg_ind = VERR_SUBNQN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_SUBNQN;             /* [한국어] Invalid SubNQN */
		goto rjt_cass;
	}

	if (nvmf_fc_ls_validate_host(subsystem, hostnqn)) { /* [한국어] 호스트 NQN ACL 검증 - 미허용이면 비0 */
		errmsg_ind = VERR_HOSTNQN;
		rc = FCNVME_RJT_RC_INV_HOST;                /* [한국어] Invalid Host */
		ec = FCNVME_RJT_EXP_INV_HOSTNQN;            /* [한국어] Invalid HostNQN */
		goto rjt_cass;
	}

	/* get new association */
	assoc = nvmf_fc_ls_new_association(s_id, tgtport, ls_rqst->rport, /* [한국어] 검증 통과 - association 객체 생성 + connection 풀 선할당 */
					   &rqst->assoc_cmd, subsystem,
					   ls_rqst->rpi, transport);
	if (!assoc) {                                       /* [한국어] association 할당 실패 */
		errmsg_ind = VERR_ASSOC_ALLOC_FAIL;
		rc = FCNVME_RJT_RC_INSUFF_RES;              /* [한국어] Insufficient Resources */
		ec = FCNVME_RJT_EXP_NONE;
		goto rjt_cass;
	}

	/* alloc admin q (i.e. connection) */
	fc_conn = nvmf_fc_ls_new_connection(assoc, 0,       /* [한국어] qid=0 admin queue connection 생성 */
					    from_be16(&rqst->assoc_cmd.ersp_ratio),
					    ls_rqst->rpi,
					    from_be16(&rqst->assoc_cmd.sqsize),
					    tgtport);
	if (!fc_conn) {                                     /* [한국어] connection 할당 실패 - association 롤백 */
		nvmf_fc_ls_free_association(assoc);
		errmsg_ind = VERR_CONN_ALLOC_FAIL;
		rc = FCNVME_RJT_RC_INSUFF_RES;
		ec = FCNVME_RJT_EXP_NONE;
		goto rjt_cass;
	}

	/* format accept response */
	bzero(acc, sizeof(*acc));                           /* [한국어] 응답 버퍼 0-초기화 */
	ls_rqst->rsp_len = sizeof(*acc);                    /* [한국어] 응답 길이 = CR_ASSOC Accept IU 크기 */

	nvmf_fc_ls_format_rsp_hdr(acc, FCNVME_LS_ACC,       /* [한국어] LS Accept 공통 헤더 작성 (RQST descriptor에 CR_ASSOC echo) */
				  nvmf_fc_lsdesc_len(
					  sizeof(struct spdk_nvmf_fc_ls_cr_assoc_acc)),
				  FCNVME_LS_CREATE_ASSOCIATION);
	to_be32(&acc->assoc_id.desc_tag, FCNVME_LSDESC_ASSOC_ID); /* [한국어] Association ID descriptor 태그 (값은 등록 콜백에서 채움) */
	acc->assoc_id.desc_len =                            /* [한국어] Association ID descriptor 길이 */
		nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id));
	to_be32(&acc->conn_id.desc_tag, FCNVME_LSDESC_CONN_ID); /* [한국어] Connection ID descriptor 태그 */
	acc->conn_id.desc_len =                             /* [한국어] Connection ID descriptor 길이 */
		nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_conn_id));

	/* assign connection to HWQP poller - also sends response */
	nvmf_fc_ls_add_conn_to_poller(assoc, ls_rqst, fc_conn, true); /* [한국어] admin connection을 poll group에 배치 - conn_id 부여 후 콜백에서 응답 송신 */

	return;                                             /* [한국어] 정상 경로 종료 (응답은 비동기 콜백이 담당) */

rjt_cass:                                               /* [한국어] 검증/할당 실패 공통 거부 처리 */
	SPDK_ERRLOG("Create Association LS failed: %s\n", validation_errors[errmsg_ind]); /* [한국어] 실패 사유 문자열 로깅 */
	ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc, FCNVME_MAX_LS_BUFFER_SIZE, /* [한국어] reason/explanation으로 LS Reject IU 작성 */
			   rqst->w0.ls_cmd, rc, ec, 0);
	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);               /* [한국어] LS Reject 송신 */
}

/*
 * [한국어]
 * nvmf_fc_ls_process_cioc - Create Connection LS 처리 (기존 association에 I/O queue 추가)
 *
 * @tgtport: LS를 수신한 target nport
 * @ls_rqst: LS 요청 컨텍스트
 *
 * association 수립 후 호스트가 추가 I/O queue를 만들 때 보내는 LS. IU/descriptor 길이·태그·
 * ERSP ratio·SQ size를 검증하고, Association ID로 기존 association을 찾는다. association이
 * 없거나 삭제 중이거나 connection 한도 초과면 거부한다. 통과 시 I/O connection을 생성하고
 * LS Accept 헤더를 작성한 뒤 add_conn_to_poller로 배치한다(응답은 등록 콜백에서 송신).
 *
 * 실행 컨텍스트: LS 처리 스레드.
 *
 * 호출 체인:
 *   nvmf_fc_handle_ls_rqst -> [본 함수]
 *     -> nvmf_fc_ls_find_assoc + nvmf_fc_ls_new_connection + nvmf_fc_ls_add_conn_to_poller
 */
static void
nvmf_fc_ls_process_cioc(struct spdk_nvmf_fc_nport *tgtport,
			struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_cr_conn_rqst *rqst =         /* [한국어] CR_CONN 요청 IU로 요청 버퍼 해석 */
		(struct spdk_nvmf_fc_ls_cr_conn_rqst *)ls_rqst->rqstbuf.virt;
	struct spdk_nvmf_fc_ls_cr_conn_acc *acc =           /* [한국어] CR_CONN Accept IU로 응답 버퍼 해석 */
		(struct spdk_nvmf_fc_ls_cr_conn_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_fc_association *assoc;              /* [한국어] Association ID로 찾을 기존 association */
	struct spdk_nvmf_fc_conn *fc_conn = NULL;           /* [한국어] 생성할 I/O connection */
	int errmsg_ind = 0;                                 /* [한국어] 에러 메시지 인덱스 */
	uint8_t rc = FCNVME_RJT_RC_NONE;                    /* [한국어] LS Reject reason */
	uint8_t ec = FCNVME_RJT_EXP_NONE;                   /* [한국어] LS Reject explanation */
	struct spdk_nvmf_transport *transport = spdk_nvmf_tgt_get_transport(ls_rqst->nvmf_tgt, /* [한국어] FC 트랜스포트 핸들 - max_queue_depth/max_qpairs 검증용 */
						SPDK_NVME_TRANSPORT_NAME_FC);

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "LS_CIOC: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d, "
		      "assoc_id=0x%lx, sq_size=%d, esrp=%d, Tgtport nn:%lx, pn:%lx\n",
		      ls_rqst->rqst_len, from_be32(&rqst->desc_list_len),
		      from_be32(&rqst->connect_cmd.desc_len),
		      from_be64(&rqst->assoc_id.association_id),
		      from_be32(&rqst->connect_cmd.sqsize),
		      from_be32(&rqst->connect_cmd.ersp_ratio),
		      tgtport->fc_nodename.u.wwn, tgtport->fc_portname.u.wwn); /* [한국어] 요청 파라미터 일괄 트레이스 */

	if (ls_rqst->rqst_len < sizeof(struct spdk_nvmf_fc_ls_cr_conn_rqst)) { /* [한국어] [검증1] IU 전체 길이 부족 */
		errmsg_ind = VERR_CR_CONN_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->desc_list_len !=                   /* [한국어] [검증2] descriptor list 길이 불일치 */
		   nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_ls_cr_conn_rqst))) {
		errmsg_ind = VERR_CR_CONN_RQST_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->assoc_id.desc_tag !=               /* [한국어] [검증3] Association ID descriptor 태그 불일치 */
		   cpu_to_be32(FCNVME_LSDESC_ASSOC_ID)) {
		errmsg_ind = VERR_ASSOC_ID;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (rqst->assoc_id.desc_len !=               /* [한국어] [검증4] Association ID descriptor 길이 불일치 */
		   nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id))) {
		errmsg_ind = VERR_ASSOC_ID_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->connect_cmd.desc_tag !=            /* [한국어] [검증5] Connect command descriptor 태그 불일치 */
		   cpu_to_be32(FCNVME_LSDESC_CREATE_CONN_CMD)) {
		errmsg_ind = VERR_CR_CONN_CMD;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (rqst->connect_cmd.desc_len !=            /* [한국어] [검증6] Connect command descriptor 길이 불일치 */
		   nvmf_fc_lsdesc_len(
			   sizeof(struct spdk_nvmf_fc_lsdesc_cr_conn_cmd))) {
		errmsg_ind = VERR_CR_CONN_CMD_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (!rqst->connect_cmd.ersp_ratio ||         /* [한국어] [검증7] ERSP ratio 비유효 (0 또는 SQ size 이상) */
		   (from_be16(&rqst->connect_cmd.ersp_ratio) >=
		    from_be16(&rqst->connect_cmd.sqsize))) {
		errmsg_ind = VERR_ERSP_RATIO;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_ESRP;
	} else if (from_be16(&rqst->connect_cmd.sqsize) == 0 || /* [한국어] [검증8] SQ size 0이거나 I/O queue 깊이 한도 초과 */
		   from_be16(&rqst->connect_cmd.sqsize) > transport->opts.max_queue_depth) {
		errmsg_ind = VERR_SQSIZE;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_SQ_SIZE;
	}

	if (rc != FCNVME_RJT_RC_NONE) {                     /* [한국어] 형식 검증 실패 시 거부 */
		goto rjt_cioc;
	}

	/* find association */
	assoc = nvmf_fc_ls_find_assoc(tgtport,              /* [한국어] Association ID로 기존 association 탐색 */
				      from_be64(&rqst->assoc_id.association_id));
	if (!assoc) {                                       /* [한국어] association 없음 (잘못된 ID 또는 이미 삭제) */
		errmsg_ind = VERR_NO_ASSOC;
		rc = FCNVME_RJT_RC_INV_ASSOC;
	} else if (assoc->assoc_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) { /* [한국어] 삭제 진행 중인 association에는 connection 추가 금지 */
		/* association is being deleted - don't allow more connections */
		errmsg_ind = VERR_NO_ASSOC;
		rc = FCNVME_RJT_RC_INV_ASSOC;
	} else  if (assoc->conn_count >= transport->opts.max_qpairs_per_ctrlr) { /* [한국어] connection 한도 초과 */
		errmsg_ind = VERR_CONN_TOO_MANY;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec =  FCNVME_RJT_EXP_INV_Q_ID;              /* [한국어] Invalid Queue ID */
	}

	if (rc != FCNVME_RJT_RC_NONE) {                     /* [한국어] association 검증 실패 시 거부 */
		goto rjt_cioc;
	}

	fc_conn = nvmf_fc_ls_new_connection(assoc, from_be16(&rqst->connect_cmd.qid), /* [한국어] 요청한 qid로 I/O connection 생성 */
					    from_be16(&rqst->connect_cmd.ersp_ratio),
					    ls_rqst->rpi,
					    from_be16(&rqst->connect_cmd.sqsize),
					    tgtport);
	if (!fc_conn) {                                     /* [한국어] connection 풀 소진 등으로 실패 */
		errmsg_ind = VERR_CONN_ALLOC_FAIL;
		rc = FCNVME_RJT_RC_INSUFF_RES;
		ec = FCNVME_RJT_EXP_NONE;
		goto rjt_cioc;
	}

	/* format accept response */
	SPDK_DEBUGLOG(nvmf_fc_ls, "Formatting LS accept response for "
		      "assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		      fc_conn->conn_id);                    /* [한국어] Accept 작성 트레이스 */
	bzero(acc, sizeof(*acc));                           /* [한국어] 응답 버퍼 0-초기화 */
	ls_rqst->rsp_len = sizeof(*acc);                    /* [한국어] 응답 길이 설정 */
	nvmf_fc_ls_format_rsp_hdr(acc, FCNVME_LS_ACC,       /* [한국어] LS Accept 공통 헤더 (CR_CONN echo) */
				  nvmf_fc_lsdesc_len(
					  sizeof(struct spdk_nvmf_fc_ls_cr_conn_acc)),
				  FCNVME_LS_CREATE_CONNECTION);
	to_be32(&acc->conn_id.desc_tag, FCNVME_LSDESC_CONN_ID); /* [한국어] Connection ID descriptor 태그 (값은 등록 콜백에서) */
	acc->conn_id.desc_len =                             /* [한국어] Connection ID descriptor 길이 */
		nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_conn_id));

	/* assign connection to HWQP poller - also sends response */
	nvmf_fc_ls_add_conn_to_poller(assoc, ls_rqst, fc_conn, false); /* [한국어] I/O connection을 poll group에 배치 (aq_conn=false) - 콜백에서 응답 */

	return;                                             /* [한국어] 정상 종료 (응답 비동기) */

rjt_cioc:                                               /* [한국어] 실패 공통 거부 처리 */
	SPDK_ERRLOG("Create Connection LS failed: %s\n", validation_errors[errmsg_ind]);

	ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc, FCNVME_MAX_LS_BUFFER_SIZE, /* [한국어] LS Reject IU 작성 */
			   rqst->w0.ls_cmd, rc, ec, 0);
	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);               /* [한국어] LS Reject 송신 */
}

/*
 * [한국어]
 * nvmf_fc_ls_process_disc - Disconnect LS 처리 (association 해제)
 *
 * @tgtport: LS를 수신한 target nport
 * @ls_rqst: LS 요청 컨텍스트
 *
 * 호스트가 association을 정상 종료할 때 보내는 LS. IU/descriptor 길이·태그를 검증하고
 * Association ID로 활성 association을 찾는다. 통과 시 LS Accept 헤더를 미리 작성한 뒤
 * disconnect_assoc로 association 삭제를 시작한다(실제 응답 송신은 삭제 완료 콜백에서).
 * 검증 실패나 association 없음이면 LS Reject를 송신한다.
 *
 * 실행 컨텍스트: LS 처리 스레드.
 *
 * 호출 체인:
 *   nvmf_fc_handle_ls_rqst -> [본 함수] -> nvmf_fc_ls_disconnect_assoc
 */
static void
nvmf_fc_ls_process_disc(struct spdk_nvmf_fc_nport *tgtport,
			struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_disconnect_rqst *rqst =      /* [한국어] Disconnect 요청 IU로 요청 버퍼 해석 */
		(struct spdk_nvmf_fc_ls_disconnect_rqst *)ls_rqst->rqstbuf.virt;
	struct spdk_nvmf_fc_ls_disconnect_acc *acc =        /* [한국어] Disconnect Accept IU로 응답 버퍼 해석 */
		(struct spdk_nvmf_fc_ls_disconnect_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_fc_association *assoc;              /* [한국어] 해제할 association */
	int errmsg_ind = 0;                                 /* [한국어] 에러 메시지 인덱스 */
	uint8_t rc = FCNVME_RJT_RC_NONE;                    /* [한국어] LS Reject reason */
	uint8_t ec = FCNVME_RJT_EXP_NONE;                   /* [한국어] LS Reject explanation */

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "LS_DISC: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d,"
		      "assoc_id=0x%lx\n",
		      ls_rqst->rqst_len, from_be32(&rqst->desc_list_len),
		      from_be32(&rqst->disconn_cmd.desc_len),
		      from_be64(&rqst->assoc_id.association_id)); /* [한국어] 요청 파라미터 트레이스 */

	if (ls_rqst->rqst_len < sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst)) { /* [한국어] [검증1] IU 전체 길이 부족 */
		errmsg_ind = VERR_DISCONN_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->desc_list_len !=                   /* [한국어] [검증2] descriptor list 길이 불일치 */
		   nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst))) {
		errmsg_ind = VERR_DISCONN_RQST_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->assoc_id.desc_tag !=               /* [한국어] [검증3] Association ID descriptor 태그 불일치 */
		   cpu_to_be32(FCNVME_LSDESC_ASSOC_ID)) {
		errmsg_ind = VERR_ASSOC_ID;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (rqst->assoc_id.desc_len !=               /* [한국어] [검증4] Association ID descriptor 길이 불일치 */
		   nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id))) {
		errmsg_ind = VERR_ASSOC_ID_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->disconn_cmd.desc_tag !=            /* [한국어] [검증5] Disconnect command descriptor 태그 불일치 */
		   cpu_to_be32(FCNVME_LSDESC_DISCONN_CMD)) {
		rc = FCNVME_RJT_RC_INV_PARAM;
		errmsg_ind = VERR_DISCONN_CMD;
	} else if (rqst->disconn_cmd.desc_len !=            /* [한국어] [검증6] Disconnect command descriptor 길이 불일치 */
		   nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_disconn_cmd))) {
		errmsg_ind = VERR_DISCONN_CMD_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	}

	if (rc != FCNVME_RJT_RC_NONE) {                     /* [한국어] 형식 검증 실패 시 거부 */
		goto rjt_disc;
	}

	/* match an active association */
	assoc = nvmf_fc_ls_find_assoc(tgtport,              /* [한국어] Association ID로 활성 association 탐색 */
				      from_be64(&rqst->assoc_id.association_id));
	if (!assoc) {                                       /* [한국어] association 없음 */
		errmsg_ind = VERR_NO_ASSOC;
		rc = FCNVME_RJT_RC_INV_ASSOC;
		goto rjt_disc;
	}

	/* format response */
	bzero(acc, sizeof(*acc));                           /* [한국어] 응답 버퍼 0-초기화 */
	ls_rqst->rsp_len = sizeof(*acc);                    /* [한국어] 응답 길이 설정 */

	nvmf_fc_ls_format_rsp_hdr(acc, FCNVME_LS_ACC,       /* [한국어] LS Accept 헤더 미리 작성 (삭제 성공 시 그대로 송신됨) */
				  nvmf_fc_lsdesc_len(
					  sizeof(struct spdk_nvmf_fc_ls_disconnect_acc)),
				  FCNVME_LS_DISCONNECT);

	nvmf_fc_ls_disconnect_assoc(tgtport, ls_rqst, assoc->assoc_id); /* [한국어] association 삭제 시작 - 완료 콜백에서 Accept/Reject 송신 */
	return;                                             /* [한국어] 정상 종료 (응답 비동기) */

rjt_disc:                                               /* [한국어] 실패 공통 거부 처리 */
	SPDK_ERRLOG("Disconnect LS failed: %s\n", validation_errors[errmsg_ind]);
	ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc, FCNVME_MAX_LS_BUFFER_SIZE, /* [한국어] LS Reject IU 작성 */
			   rqst->w0.ls_cmd, rc, ec, 0);
	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);               /* [한국어] LS Reject 송신 */
}

/* ************************ */
/* external functions       */
/* [한국어] 아래는 본 파일의 외부 API - fc.c 또는 LLD가 호출. */

/*
 * [한국어]
 * nvmf_fc_ls_init - FC port의 LS 처리 컨텍스트 초기화 (현재 no-op)
 *
 * @fc_port: 대상 FC port
 *
 * 향후 LS 처리에 필요한 자원(예: LS 캐시) 초기화 자리. 현 구현은 빈 구현이지만
 * fc.c가 hw_port_init 이벤트 시점에 호출하므로 유지됨.
 */
void
nvmf_fc_ls_init(struct spdk_nvmf_fc_port *fc_port)
{
}

/*
 * [한국어]
 * nvmf_fc_ls_fini - FC port의 LS 처리 자원 해제 (현재 no-op)
 *
 * @fc_port: 대상 FC port
 *
 * hw_port_free 이벤트 시 호출. ls_init에 대응하는 cleanup 자리.
 */
void
nvmf_fc_ls_fini(struct spdk_nvmf_fc_port *fc_port)
{
}

/*
 * [한국어]
 * nvmf_fc_handle_ls_rqst - NVMe-FC Link Service frame dispatcher (외부 진입점)
 *
 * @ls_rqst: LLD가 수신한 LS request 컨텍스트 (rqstbuf/rspbuf, source N_Port_ID, nport 포함)
 *
 * LLD의 LS 큐 폴링에서 LS frame이 발견되면 본 함수를 호출한다.
 * LS Cmd Code(rqstbuf 첫 바이트)를 보고 적절한 핸들러로 분기:
 *   FCNVME_LS_CREATE_ASSOCIATION  -> nvmf_fc_ls_process_cass
 *   FCNVME_LS_CREATE_CONNECTION   -> nvmf_fc_ls_process_cioc
 *   FCNVME_LS_DISCONNECT          -> nvmf_fc_ls_process_disc
 *   알 수 없는 LS               -> LS_RJT with reason=INVAL
 * 모든 핸들러는 응답 IU를 ls_rqst->rspbuf에 작성하고 nvmf_fc_xmt_ls_rsp()로 송신.
 *
 * 실행 컨텍스트: LS 처리용 HWQP가 바인딩된 poll group 스레드.
 *
 * 호출 체인:
 *   LLD RQ poll -> nvmf_fc_process_queue -> [본 함수] -> 명령별 핸들러 -> 응답 LS 송신
 */
void
nvmf_fc_handle_ls_rqst(struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_rqst_w0 *w0 =                    /* [한국어] LS request의 첫 워드(ls_cmd 포함) 포인터 */
		(struct spdk_nvmf_fc_ls_rqst_w0 *)ls_rqst->rqstbuf.virt;
	uint32_t s_id = ls_rqst->s_id;                          /* [한국어] LS frame의 source N_Port_ID - 어떤 호스트가 보냈는지 */
	struct spdk_nvmf_fc_nport *tgtport = ls_rqst->nport;    /* [한국어] LS를 수신한 target nport - 응답 송신 시 사용 */

	SPDK_DEBUGLOG(nvmf_fc_ls, "LS cmd=%d\n", w0->ls_cmd);   /* [한국어] LS Cmd Code 디버그 로그 - 진입 트레이스 */

	switch (w0->ls_cmd) {                                   /* [한국어] LS Cmd Code별 분기 (FC-NVMe-2 8.3) */
	case FCNVME_LS_CREATE_ASSOCIATION:                      /* [한국어] LS Cmd = 0x01 - Create Association (호스트의 첫 LS) */
		nvmf_fc_ls_process_cass(s_id, tgtport, ls_rqst); /* [한국어] association + admin queue 생성 처리 */
		break;
	case FCNVME_LS_CREATE_CONNECTION:                       /* [한국어] LS Cmd = 0x02 - Create Connection (I/O queue 추가) */
		nvmf_fc_ls_process_cioc(tgtport, ls_rqst);      /* [한국어] 기존 association에 I/O connection 추가 */
		break;
	case FCNVME_LS_DISCONNECT:                              /* [한국어] LS Cmd = 0x05 - Disconnect (association 또는 connection 해제) */
		nvmf_fc_ls_process_disc(tgtport, ls_rqst);      /* [한국어] scope에 따라 association/connection 정리 */
		break;
	default:                                                /* [한국어] 미지원 LS Cmd Code - LS_RJT로 응답 */
		SPDK_ERRLOG("Invalid LS cmd=%d\n", w0->ls_cmd);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(ls_rqst->rspbuf.virt, /* [한국어] reason=INVAL로 거부 응답 작성 */
				   FCNVME_MAX_LS_BUFFER_SIZE, w0->ls_cmd,
				   FCNVME_RJT_RC_INVAL, FCNVME_RJT_EXP_NONE, 0);
		nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);           /* [한국어] LS_RJT 송신 */
	}
}

/*
 * [한국어]
 * nvmf_fc_delete_association - association 외부 삭제 진입점
 *
 * @tgtport: association이 속한 target nport
 * @assoc_id: 삭제할 association의 ID
 * @send_abts: 진행 중 I/O에 ABTS 송신 여부
 * @backend_initiated: target 측이 시작한 삭제인지 (호스트 LS Disconnect와 구분)
 * @del_assoc_cb: 비동기 완료 콜백
 * @cb_data: 콜백 인자
 * @return: 0 성공, 음수 errno
 *
 * RPC nvmf_subsystem_remove_listener나 hw_port_offline 등에서 호스트 LS 없이도
 * target이 자체적으로 association을 정리해야 할 때 사용.
 */
int
nvmf_fc_delete_association(struct spdk_nvmf_fc_nport *tgtport,
			   uint64_t assoc_id, bool send_abts, bool backend_initiated,
			   spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
			   void *cb_data)
{
	return _nvmf_fc_delete_association(tgtport, assoc_id, send_abts, backend_initiated,
					   del_assoc_cb, cb_data, false); /* [한국어] 내부 implementer에 위임 - from_ls=false (LS 처리 경로 아님) */
}

/*
 * [한국어]
 * nvmf_fc_delete_connection - 단일 connection 외부 삭제 진입점
 *
 * @fc_conn: 삭제할 connection
 * @send_abts: ABTS 송신 여부
 * @backend_initiated: target 자체 시작 여부
 * @cb_fn: 비동기 완료 콜백
 * @cb_data: 콜백 인자
 * @return: 0 성공, 음수 errno
 *
 * association은 그대로 두고 특정 I/O connection만 정리할 때 사용.
 * HWQP에 poller API로 SPDK_NVMF_FC_POLLER_API_DEL_CONNECTION 메시지 송신.
 */
int
nvmf_fc_delete_connection(struct spdk_nvmf_fc_conn *fc_conn, bool send_abts,
			  bool backend_initiated, spdk_nvmf_fc_del_conn_cb cb_fn,
			  void *cb_data)
{
	return nvmf_fc_ls_poller_delete_conn(fc_conn, send_abts, NULL,
					     backend_initiated, cb_fn, cb_data); /* [한국어] LS 컨텍스트 없는 호출 - opd=NULL */
}


/*
 * [한국어]
 * nvmf_fc_poller_api_cb_event - poller API 완료 콜백을 요청 스레드에서 실제 호출하는 래퍼
 *
 * @arg: spdk_nvmf_fc_poller_api_cb_info 포인터
 *
 * poller(HWQP 스레드)가 처리를 마치면 완료 콜백은 요청자 스레드(cb_thread)에서 실행돼야
 * 한다. perform_cb가 spdk_thread_send_msg로 본 함수를 cb_thread에 디스패치하고, 본 함수가
 * 저장된 cb_func(cb_data, ret)을 호출한다.
 *
 * 실행 컨텍스트: cb_thread(요청자 스레드, 보통 FC main/LS 스레드)에서 실행.
 *
 * 호출 체인:
 *   nvmf_fc_poller_api_perform_cb -> spdk_thread_send_msg -> [본 함수] -> cb_func
 */
static void
nvmf_fc_poller_api_cb_event(void *arg)
{
	struct spdk_nvmf_fc_poller_api_cb_info *cb_info =   /* [한국어] 콜백 정보 복원 */
		(struct spdk_nvmf_fc_poller_api_cb_info *) arg;

	assert(cb_info != NULL);                            /* [한국어] cb_info는 항상 유효해야 함 */
	cb_info->cb_func(cb_info->cb_data, cb_info->ret);   /* [한국어] 실제 완료 콜백 호출 (cb_data, 결과 코드 전달) */
}

/*
 * [한국어]
 * nvmf_fc_poller_api_perform_cb - poller 처리 결과를 요청 스레드로 비동기 디스패치
 *
 * @cb_info: 콜백 정보 (cb_func/cb_thread/cb_data/ret)
 * @ret: poller 처리 결과 코드
 *
 * poller가 HWQP 스레드에서 작업을 끝낸 뒤, 결과를 cb_info에 기록하고 요청자 스레드로
 * 콜백 실행을 넘긴다. cross-thread 디스패치(spdk_thread_send_msg)를 쓰는 이유는, 완료
 * 후속 처리(자료구조 변경, LS 응답)가 단일 스레드 직렬화 영역에서 lock-free로 이뤄지도록
 * 보장하기 위함이다. cb_func/cb_thread가 없으면 (콜백 불필요) 아무 동작도 하지 않는다.
 *
 * 실행 컨텍스트: HWQP poller 스레드에서 호출.
 *
 * 호출 체인:
 *   각 nvmf_fc_poller_api_* 핸들러 -> [본 함수] -> spdk_thread_send_msg(cb_thread)
 */
static void
nvmf_fc_poller_api_perform_cb(struct spdk_nvmf_fc_poller_api_cb_info *cb_info,
			      enum spdk_nvmf_fc_poller_api_ret ret)
{
	if (cb_info->cb_func && cb_info->cb_thread) {       /* [한국어] 콜백 함수와 대상 스레드가 모두 지정된 경우에만 */
		cb_info->ret = ret;                         /* [한국어] 결과 코드를 콜백 정보에 기록 (event 래퍼가 읽음) */
		/* callback to main thread */
		spdk_thread_send_msg(cb_info->cb_thread, nvmf_fc_poller_api_cb_event, /* [한국어] cb_thread로 콜백 실행 메시지 전달 (lockless 메시지 큐) */
				     (void *) cb_info);
	}
}

/*
 * [한국어]
 * nvmf_fc_poller_add_conn_lookup_data - HWQP의 connection/rport 조회 해시에 connection 등록
 *
 * @hwqp: 대상 HWQP (DPDK rte_hash 기반 조회 테이블 보유)
 * @fc_conn: 등록할 connection
 * @return: 0 성공, 음수 errno (실패 시 부분 등록 롤백)
 *
 * I/O frame 처리 hot path에서 conn_id로 connection을, rpi로 ABTS 대상 후보를 빠르게 찾기
 * 위해 두 개의 해시(conn_id->conn, rpi->rport)를 유지한다. 같은 rpi의 connection들은 rport
 * 노드 아래 conn_list로 묶인다. conn 해시 등록 후 rport 노드를 (없으면 생성해) 부착한다.
 * DPDK rte_hash는 lockless lookup이 가능하나 본 경로는 HWQP poller 스레드에서만 변경한다.
 *
 * 실행 컨텍스트: HWQP poller 스레드(add_connection 핸들러).
 *
 * 호출 체인:
 *   nvmf_fc_poller_api_add_connection -> [본 함수] -> rte_hash_add_key_data
 */
static int
nvmf_fc_poller_add_conn_lookup_data(struct spdk_nvmf_fc_hwqp *hwqp,
				    struct spdk_nvmf_fc_conn *fc_conn)
{
	int rc = -1;                                        /* [한국어] 등록 결과 코드 */
	struct spdk_nvmf_fc_hwqp_rport *rport = NULL;       /* [한국어] rpi -> rport 노드 (conn 그룹) */

	/* Add connection based lookup entry. */
	rc = rte_hash_add_key_data(hwqp->connection_list_hash, /* [한국어] conn_id -> conn 해시 등록 (DPDK rte_hash) */
				   (void *)&fc_conn->conn_id, (void *)fc_conn);

	if (rc < 0) {                                       /* [한국어] conn 해시 등록 실패 */
		SPDK_ERRLOG("Failed to add connection hash entry\n");
		return rc;
	}

	/* RPI based lookup */
	if (rte_hash_lookup_data(hwqp->rport_list_hash, (void *)&fc_conn->rpi, (void **)&rport) < 0) { /* [한국어] 이 rpi의 rport 노드가 아직 없으면 */
		rport = calloc(1, sizeof(struct spdk_nvmf_fc_hwqp_rport)); /* [한국어] rport 노드 새로 할당 */
		if (!rport) {                               /* [한국어] 할당 실패 - conn 해시 롤백 */
			SPDK_ERRLOG("Failed to allocate rport entry\n");
			rc = -ENOMEM;
			goto del_conn_hash;
		}

		/* Add rport table entry */
		rc = rte_hash_add_key_data(hwqp->rport_list_hash, /* [한국어] rpi -> rport 해시 등록 */
					   (void *)&fc_conn->rpi, (void *)rport);
		if (rc < 0) {                               /* [한국어] rport 해시 등록 실패 - rport free + conn 해시 롤백 */
			SPDK_ERRLOG("Failed to add rport hash entry\n");
			goto del_rport;
		}
		TAILQ_INIT(&rport->conn_list);              /* [한국어] 이 rpi에 속한 connection 리스트 초기화 */
	}

	/* Add to rport conn list */
	TAILQ_INSERT_TAIL(&rport->conn_list, fc_conn, rport_link); /* [한국어] connection을 rport conn 그룹에 추가 (ABTS 매칭용) */
	return 0;                                           /* [한국어] 등록 성공 */

del_rport:                                              /* [한국어] rport 해시 등록 실패 롤백 */
	free(rport);
del_conn_hash:                                          /* [한국어] conn 해시 롤백 */
	rte_hash_del_key(hwqp->connection_list_hash, (void *)&fc_conn->conn_id); /* [한국어] 앞서 등록한 conn 해시 엔트리 제거 */
	return rc;                                          /* [한국어] 실패 코드 반환 */
}

/*
 * [한국어]
 * nvmf_fc_poller_del_conn_lookup_data - HWQP 조회 해시에서 connection 등록 제거
 *
 * @hwqp: 대상 HWQP
 * @fc_conn: 제거할 connection
 *
 * add_conn_lookup_data의 역연산. conn_id 해시 엔트리를 제거하고 rport conn 리스트에서도
 * 떼어낸다. 그 rpi의 마지막 connection이면 rport 해시 엔트리와 rport 노드까지 정리한다.
 *
 * 실행 컨텍스트: HWQP poller 스레드(del_connection 완료 경로).
 *
 * 호출 체인:
 *   nvmf_fc_poller_conn_abort_done / nvmf_fc_poller_api_del_connection -> [본 함수]
 */
static void
nvmf_fc_poller_del_conn_lookup_data(struct spdk_nvmf_fc_hwqp *hwqp,
				    struct spdk_nvmf_fc_conn *fc_conn)
{
	struct spdk_nvmf_fc_hwqp_rport *rport = NULL;       /* [한국어] rpi -> rport 노드 */

	if (rte_hash_del_key(hwqp->connection_list_hash, (void *)&fc_conn->conn_id) < 0) { /* [한국어] conn_id 해시 엔트리 제거 */
		SPDK_ERRLOG("Failed to del connection(%lx) hash entry\n",
			    fc_conn->conn_id);
	}

	if (rte_hash_lookup_data(hwqp->rport_list_hash, (void *)&fc_conn->rpi, (void **)&rport) >= 0) { /* [한국어] 이 conn의 rport 노드 조회 */
		TAILQ_REMOVE(&rport->conn_list, fc_conn, rport_link); /* [한국어] rport conn 그룹에서 제거 */

		/* If last conn del rpi hash */
		if (TAILQ_EMPTY(&rport->conn_list)) {       /* [한국어] 이 rpi의 마지막 connection이면 rport 노드도 정리 */
			if (rte_hash_del_key(hwqp->rport_list_hash, (void *)&fc_conn->rpi) < 0) { /* [한국어] rpi 해시 엔트리 제거 */
				SPDK_ERRLOG("Failed to del rpi(%lx) hash entry\n",
					    fc_conn->conn_id);
			}
			free(rport);                        /* [한국어] rport 노드 해제 */
		}
	} else {                                            /* [한국어] rport 노드가 없으면 (등록/제거 불일치 - 비정상) */
		SPDK_ERRLOG("RPI(%d) hash entry not found\n", fc_conn->rpi);
	}
}

/*
 * [한국어]
 * nvmf_fc_poller_rpi_find_req - rpi + oxid로 진행 중 fc_request 찾기 (ABTS 처리용)
 *
 * @hwqp: 대상 HWQP
 * @rpi: ABTS frame의 remote port index
 * @oxid: ABTS가 가리키는 originator exchange ID
 * @return: 매칭되는 fc_request, 없으면 NULL
 *
 * 호스트가 ABTS(Abort Sequence)를 보내면, 어떤 진행 중 I/O를 중단하라는 것인지 rpi와 oxid로
 * 식별해야 한다. rpi -> rport -> conn_list -> 각 conn의 in_use_reqs를 순회하며 oxid가 같은
 * fc_request를 찾는다. (이중 순회는 association당 connection이 적어 비용이 작음)
 *
 * 실행 컨텍스트: HWQP poller 스레드(ABTS 수신 핸들러).
 *
 * 호출 체인:
 *   nvmf_fc_poller_api_abts_received -> [본 함수]
 */
static struct spdk_nvmf_fc_request *
nvmf_fc_poller_rpi_find_req(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t rpi, uint16_t oxid)
{
	struct spdk_nvmf_fc_request *fc_req = NULL;         /* [한국어] 탐색 결과 fc_request */
	struct spdk_nvmf_fc_conn *fc_conn;                  /* [한국어] rport 그룹 내 connection 순회 */
	struct spdk_nvmf_fc_hwqp_rport *rport = NULL;       /* [한국어] rpi -> rport 노드 */

	if (rte_hash_lookup_data(hwqp->rport_list_hash, (void *)&rpi, (void **)&rport) >= 0) { /* [한국어] rpi로 rport 노드 조회 */
		TAILQ_FOREACH(fc_conn, &rport->conn_list, rport_link) { /* [한국어] 이 rpi의 모든 connection 순회 */
			TAILQ_FOREACH(fc_req, &fc_conn->in_use_reqs, conn_link) { /* [한국어] 각 conn의 진행 중 요청 순회 */
				if (fc_req->oxid == oxid) {  /* [한국어] oxid 일치 - 중단 대상 발견 */
					return fc_req;
				}
			}
		}
	}
	return NULL;                                        /* [한국어] 매칭 없음 - 이미 완료됐거나 알 수 없는 exchange */
}

/*
 * [한국어]
 * nvmf_fc_poller_api_add_connection - poller가 connection을 자신의 조회 테이블에 등록
 *
 * @arg: spdk_nvmf_fc_poller_api_add_connection_args 포인터
 *
 * ADD_CONNECTION 메시지를 받은 HWQP poller 스레드에서 실행된다. 중복 conn_id를 방어한 뒤
 * conn/rport 해시에 등록하고 HWQP의 connection 수를 증가시킨다. 결과는 perform_cb로 요청
 * 스레드(add_conn_cb)에 비동기 통지되어 거기서 LS 응답이 송신된다.
 *
 * 실행 컨텍스트: HWQP poller 스레드 (spdk_thread_send_msg로 진입).
 *
 * 호출 체인:
 *   nvmf_fc_poller_api_func(ADD_CONNECTION) -> spdk_thread_send_msg -> [본 함수]
 *     -> nvmf_fc_poller_add_conn_lookup_data + perform_cb
 */
static void
nvmf_fc_poller_api_add_connection(void *arg)
{
	enum spdk_nvmf_fc_poller_api_ret ret = SPDK_NVMF_FC_POLLER_API_SUCCESS; /* [한국어] 등록 결과 (기본 성공) */
	struct spdk_nvmf_fc_poller_api_add_connection_args *conn_args = /* [한국어] add_connection 인자 복원 */
		(struct spdk_nvmf_fc_poller_api_add_connection_args *)arg;
	struct spdk_nvmf_fc_conn *fc_conn = conn_args->fc_conn, *tmp; /* [한국어] 등록할 conn + 중복 검사용 임시 포인터 */

	SPDK_DEBUGLOG(nvmf_fc_poller_api, "Poller add connection, conn_id 0x%lx\n",
		      fc_conn->conn_id);                    /* [한국어] 등록 시작 트레이스 */

	/* make sure connection is not already in poller's list */
	if (rte_hash_lookup_data(fc_conn->hwqp->connection_list_hash, /* [한국어] 동일 conn_id가 이미 등록돼 있는지 확인 */
				 (void *)&fc_conn->conn_id, (void **)&tmp) >= 0) {
		SPDK_ERRLOG("duplicate connection found");
		ret = SPDK_NVMF_FC_POLLER_API_DUP_CONN_ID;  /* [한국어] 중복 conn_id 오류 */
	} else {
		if (nvmf_fc_poller_add_conn_lookup_data(fc_conn->hwqp, fc_conn)) { /* [한국어] conn/rport 조회 해시에 등록 */
			SPDK_ERRLOG("Failed to add connection 0x%lx\n", fc_conn->conn_id);
			ret = SPDK_NVMF_FC_POLLER_API_ERROR; /* [한국어] 해시 등록 실패 */
		} else {
			SPDK_DEBUGLOG(nvmf_fc_poller_api, "conn_id=%lx", fc_conn->conn_id);
			fc_conn->hwqp->num_conns++;         /* [한국어] HWQP 활성 connection 수 증가 */
		}
	}

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, ret); /* [한국어] 등록 결과를 요청 스레드로 비동기 통지 */
}

/*
 * [한국어]
 * nvmf_fc_poller_api_quiesce_queue - HWQP 폴링을 일시 중단(quiesce)하고 잔여 abort 정리
 *
 * @arg: spdk_nvmf_fc_poller_api_quiesce_queue_args 포인터
 *
 * 어댑터 리셋이나 I_T Nexus 삭제 중 HWQP를 안전 상태로 만들기 위해 호출된다. 큐 상태를
 * OFFLINE로 표시하고, transfer 상태이면서 abort 진행 중인 잔여 명령들의 abort 완료를
 * 강제로 트리거해 멈춰 있는 exchange를 정리한다.
 *
 * 실행 컨텍스트: HWQP poller 스레드.
 *
 * 호출 체인:
 *   nvmf_fc_poller_api_func(QUIESCE_QUEUE) -> spdk_thread_send_msg -> [본 함수]
 */
static void
nvmf_fc_poller_api_quiesce_queue(void *arg)
{
	struct spdk_nvmf_fc_poller_api_quiesce_queue_args *q_args = /* [한국어] quiesce 인자 복원 */
		(struct spdk_nvmf_fc_poller_api_quiesce_queue_args *) arg;
	struct spdk_nvmf_fc_request *fc_req = NULL, *tmp;   /* [한국어] 순회용 요청 + 안전 삭제용 임시 포인터 */

	/* should be already, but make sure queue is quiesced */
	q_args->hwqp->state = SPDK_FC_HWQP_OFFLINE;         /* [한국어] 폴링 중단 상태로 확정 (새 frame 처리 안 함) */

	/*
	 * Kill all the outstanding commands that are in the transfer state and
	 * in the process of being aborted.
	 * We can run into this situation if an adapter reset happens when an I_T Nexus delete
	 * is in progress.
	 */
	TAILQ_FOREACH_SAFE(fc_req, &q_args->hwqp->in_use_reqs, link, tmp) { /* [한국어] HWQP의 모든 진행 중 요청을 안전 순회 (콜백에서 제거 가능) */
		if (nvmf_fc_req_in_xfer(fc_req) && fc_req->is_aborted == true) { /* [한국어] 데이터 전송 중 + 이미 abort 표시된 요청 */
			nvmf_fc_poller_api_func(q_args->hwqp, SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE, /* [한국어] 멈춘 abort를 강제 완료 처리 */
						(void *)fc_req);
		}
	}

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&q_args->cb_info, SPDK_NVMF_FC_POLLER_API_SUCCESS); /* [한국어] quiesce 완료 통지 */
}

/*
 * [한국어]
 * nvmf_fc_poller_api_activate_queue - quiesce된 HWQP 폴링을 다시 ONLINE으로 재개
 *
 * @arg: spdk_nvmf_fc_poller_api_quiesce_queue_args 포인터 (quiesce와 동일 타입 재사용)
 *
 * 리셋/복구 후 HWQP를 다시 활성화한다. 상태를 ONLINE으로 바꾸면 poller가 frame 처리를
 * 재개한다. quiesce의 역연산.
 *
 * 실행 컨텍스트: HWQP poller 스레드.
 *
 * 호출 체인:
 *   nvmf_fc_poller_api_func(ACTIVATE_QUEUE) -> spdk_thread_send_msg -> [본 함수]
 */
static void
nvmf_fc_poller_api_activate_queue(void *arg)
{
	struct spdk_nvmf_fc_poller_api_quiesce_queue_args *q_args = /* [한국어] activate 인자 복원 */
		(struct spdk_nvmf_fc_poller_api_quiesce_queue_args *) arg;

	q_args->hwqp->state = SPDK_FC_HWQP_ONLINE;          /* [한국어] 폴링 재개 상태로 전환 */

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&q_args->cb_info, 0); /* [한국어] 재개 완료 통지 */
}

/*
 * [한국어]
 * nvmf_fc_poller_conn_abort_done - connection 삭제 시 각 I/O abort 완료마다 호출되는 콜백
 *
 * @hwqp: abort를 수행한 HWQP (미사용)
 * @status: abort 결과 상태 (미사용)
 * @cb_args: spdk_nvmf_fc_poller_api_del_connection_args 포인터
 *
 * del_connection이 connection의 모든 진행 중 I/O에 abort를 건 뒤, abort가 하나씩 완료될
 * 때마다 본 콜백이 호출되어 fc_request_cnt를 감소시킨다. 0이 되면(모든 I/O 정리됨) qpair를
 * 정상 종료(qpair_disconnect)하고, qpair_fini까지 끝나면 조회 해시에서 connection을 제거한
 * 뒤 최종 완료를 LS 스레드로 통지한다. qpair_fini가 아직이면 자신을 fini 콜백으로 등록하고
 * 상위 계층의 qpair_fini 호출을 기다린다(재진입).
 *
 * 실행 컨텍스트: HWQP poller 스레드(abort 완료/ qpair fini 콜백).
 *
 * 호출 체인:
 *   nvmf_fc_request_abort 완료 / qpair_fini -> [본 함수]
 *     -> (마지막) nvmf_fc_poller_del_conn_lookup_data + perform_cb
 */
static void
nvmf_fc_poller_conn_abort_done(void *hwqp, int32_t status, void *cb_args)
{
	struct spdk_nvmf_fc_poller_api_del_connection_args *conn_args = cb_args; /* [한국어] del_connection 인자 복원 */

	if (conn_args->fc_request_cnt) {                    /* [한국어] 남은 abort 대기 수가 있으면 */
		conn_args->fc_request_cnt -= 1;             /* [한국어] 완료된 abort 1건 차감 */
	}

	if (!conn_args->fc_request_cnt) {                   /* [한국어] 모든 I/O abort 완료 - connection 종료 단계 진입 */
		struct spdk_nvmf_fc_conn *fc_conn = conn_args->fc_conn, *tmp; /* [한국어] 대상 conn + 해시 검증용 임시 포인터 */

		SPDK_DEBUGLOG(nvmf_fc_poller_api, "Poller connection abort done, fc_conn %p conn_id 0x%lx "
			      "s_id 0x%x rpi 0x%x qpair_fini_done %d\n", fc_conn, fc_conn->conn_id,
			      fc_conn->s_id, fc_conn->rpi, fc_conn->qpair_fini_done); /* [한국어] abort 완료 트레이스 */

		if (!fc_conn->qpair_fini_done) { /* nvmf_fc_close_qpair not called yet */ /* [한국어] qpair 정상 종료가 아직 안 끝났으면 */
			fc_conn->qpair_fini_done_cb = nvmf_fc_poller_conn_abort_done; /* [한국어] qpair fini 완료 시 본 함수를 다시 호출하도록 등록 */
			fc_conn->qpair_fini_done_cb_args = cb_args; /* [한국어] 재진입 시 같은 인자 사용 */

			spdk_nvmf_qpair_disconnect(&fc_conn->qpair); /* [한국어] nvmf 코어에 qpair 정상 종료 요청 (controller cleanup 등) */
			/* Wait for upper layer to call qpair_fini */
			return;                             /* [한국어] fini 완료 콜백에서 본 함수 재진입까지 대기 */
		}

		if (rte_hash_lookup_data(conn_args->hwqp->connection_list_hash, /* [한국어] conn이 아직 해시에 있는지 확인 (이중 삭제 방어) */
					 (void *)&fc_conn->conn_id, (void *)&tmp) >= 0) {
			/* All the requests for this connection are aborted. */
			nvmf_fc_poller_del_conn_lookup_data(conn_args->hwqp, fc_conn); /* [한국어] 조회 해시에서 conn 제거 */
			fc_conn->hwqp->num_conns--;         /* [한국어] HWQP connection 수 감소 */

			SPDK_DEBUGLOG(nvmf_fc_poller_api, "Connection lookup data deleted, fc_conn %p conn_id 0x%lx "
				      "rport %p rpi 0x%x assoc %p\n", fc_conn, fc_conn->conn_id,
				      fc_conn->fc_assoc->rport, fc_conn->rpi, fc_conn->fc_assoc); /* [한국어] 정리 완료 트레이스 */

			/* perform callback */
			nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, SPDK_NVMF_FC_POLLER_API_SUCCESS); /* [한국어] LS 스레드로 최종 삭제 완료 통지 (del_conn_cb) */
		} else {                                    /* [한국어] 해시에 없음 - 이미 제거된 비정상 상황 */
			SPDK_ERRLOG("fc_conn %p conn_id 0x%lx hash on delete failed.\n", fc_conn, fc_conn->conn_id);
		}
	}
}

/*
 * [한국어]
 * nvmf_fc_poller_api_del_connection - poller가 connection을 종료 (진행 중 I/O abort 시작)
 *
 * @arg: spdk_nvmf_fc_poller_api_del_connection_args 포인터
 *
 * DEL_CONNECTION 메시지를 받은 HWQP poller 스레드에서 실행된다. conn이 유효한지 해시로
 * 확인하고, 진행 중인 모든 I/O에 abort를 건다(admin queue의 AER은 qpair_disconnect가 처리
 * 하므로 건너뜀). abort가 하나라도 진행되면 각 완료를 conn_abort_done이 카운트한다. abort할
 * I/O가 하나도 없으면 즉시 qpair 종료 후 조회 해시에서 제거하고 완료를 통지한다.
 *
 * 실행 컨텍스트: HWQP poller 스레드.
 *
 * 호출 체인:
 *   nvmf_fc_poller_api_func(DEL_CONNECTION) -> spdk_thread_send_msg -> [본 함수]
 *     -> nvmf_fc_request_abort (각 I/O) -> nvmf_fc_poller_conn_abort_done
 */
static void
nvmf_fc_poller_api_del_connection(void *arg)
{
	struct spdk_nvmf_fc_poller_api_del_connection_args *conn_args = /* [한국어] del_connection 인자 복원 */
		(struct spdk_nvmf_fc_poller_api_del_connection_args *)arg;
	struct spdk_nvmf_fc_conn *fc_conn = NULL;          /* [한국어] 해시에서 조회한 conn */
	struct spdk_nvmf_fc_request *fc_req = NULL, *tmp;  /* [한국어] 순회용 요청 + 안전 삭제용 임시 포인터 */
	struct spdk_nvmf_fc_hwqp *hwqp = conn_args->hwqp;  /* [한국어] 대상 HWQP */

	SPDK_DEBUGLOG(nvmf_fc_poller_api,
		      "Poller delete connection, fc_conn %p conn_id 0x%lx s_id 0x%x rpi 0x%x\n",
		      conn_args->fc_conn, conn_args->fc_conn->conn_id,
		      conn_args->fc_conn->s_id, conn_args->fc_conn->rpi); /* [한국어] 삭제 시작 트레이스 */

	/* Make sure connection is valid */
	if (rte_hash_lookup_data(hwqp->connection_list_hash, /* [한국어] conn_id가 이 HWQP에 등록돼 있는지 확인 */
				 (void *)&conn_args->fc_conn->conn_id, (void **)&fc_conn) < 0) {
		/* perform callback */
		nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, SPDK_NVMF_FC_POLLER_API_NO_CONN_ID); /* [한국어] 미등록 conn - NO_CONN_ID 통지 */
		return;
	}

	assert(conn_args->fc_conn == fc_conn);              /* [한국어] 해시 조회 결과가 요청 conn과 동일해야 함 */

	conn_args->fc_request_cnt = 0;                      /* [한국어] abort 대기 카운트 초기화 */

	TAILQ_FOREACH_SAFE(fc_req, &fc_conn->in_use_reqs, conn_link, tmp) { /* [한국어] conn의 진행 중 요청 안전 순회 */
		if (nvmf_qpair_is_admin_queue(&fc_conn->qpair) && /* [한국어] admin queue이면서 */
		    (fc_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST)) { /* [한국어] AER(비동기 이벤트 요청)인 경우 */
			/* AER will be cleaned by spdk_nvmf_qpair_disconnect. */
			continue;                           /* [한국어] AER은 qpair_disconnect가 정리하므로 건너뜀 */
		}

		conn_args->fc_request_cnt += 1;             /* [한국어] abort 걸 요청 수 증가 (완료 시 conn_abort_done이 차감) */
		nvmf_fc_request_abort(fc_req, conn_args->send_abts, /* [한국어] 요청 abort 시작 (필요 시 ABTS frame 송신) */
				      nvmf_fc_poller_conn_abort_done,
				      conn_args);
	}

	SPDK_DEBUGLOG(nvmf_fc_poller_api, "Poller Disconnect API, fc_conn %p conn_id 0x%lx "
		      "s_id %x rpi %x requested abort count %d qpair_fini_done %d\n", fc_conn,
		      fc_conn->conn_id, fc_conn->s_id, fc_conn->rpi,
		      conn_args->fc_request_cnt, fc_conn->qpair_fini_done); /* [한국어] abort 요청 수 트레이스 */

	if (!conn_args->fc_request_cnt) {                   /* [한국어] abort할 I/O가 하나도 없으면 즉시 종료 단계 */
		if (!fc_conn->qpair_fini_done) { /* nvmf_fc_close_qpair not called yet */ /* [한국어] qpair 정상 종료 미완료 */
			conn_args->fc_conn->qpair_fini_done_cb = nvmf_fc_poller_conn_abort_done; /* [한국어] fini 완료 시 conn_abort_done 재진입 등록 */
			conn_args->fc_conn->qpair_fini_done_cb_args = conn_args;
			spdk_nvmf_qpair_disconnect(&fc_conn->qpair); /* [한국어] qpair 정상 종료 요청 */
			return;                             /* [한국어] fini 콜백 대기 */
		}

		SPDK_DEBUGLOG(nvmf_fc_poller_api, "Connection lookup data deleted, fc_conn %p conn_id 0x%lx "
			      "rport %p rpi 0x%x assoc %p\n", fc_conn, fc_conn->conn_id,
			      fc_conn->fc_assoc->rport, fc_conn->rpi, fc_conn->fc_assoc); /* [한국어] 정리 트레이스 */

		nvmf_fc_poller_del_conn_lookup_data(conn_args->hwqp, conn_args->fc_conn); /* [한국어] 조회 해시에서 conn 제거 */
		hwqp->num_conns--;                          /* [한국어] HWQP connection 수 감소 */

		nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, SPDK_NVMF_FC_POLLER_API_SUCCESS); /* [한국어] LS 스레드로 삭제 완료 통지 */
	}
}

/*
 * [한국어]
 * nvmf_fc_poller_abts_done - ABTS로 시작된 요청 abort가 완료되면 호출되는 콜백
 *
 * @hwqp: abort를 수행한 HWQP (미사용)
 * @status: abort 결과 (미사용)
 * @cb_args: spdk_nvmf_fc_poller_api_abts_recvd_args 포인터
 *
 * 호스트 ABTS에 의해 중단된 요청의 정리가 끝나면 호출되어 ABTS 처리 완료를 요청자에게
 * 통지한다. rpi/oxid/rxid는 어떤 exchange가 중단됐는지 식별한다.
 *
 * 실행 컨텍스트: HWQP poller 스레드(abort 완료 콜백).
 *
 * 호출 체인:
 *   nvmf_fc_request_abort 완료 -> [본 함수] -> perform_cb
 */
static void
nvmf_fc_poller_abts_done(void *hwqp, int32_t status, void *cb_args)
{
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *args = cb_args; /* [한국어] ABTS 인자 복원 */

	SPDK_DEBUGLOG(nvmf_fc_poller_api,
		      "ABTS poller done, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		      args->ctx->rpi, args->ctx->oxid, args->ctx->rxid); /* [한국어] 중단된 exchange 식별 트레이스 */

	nvmf_fc_poller_api_perform_cb(&args->cb_info,       /* [한국어] ABTS 처리 완료 통지 */
				      SPDK_NVMF_FC_POLLER_API_SUCCESS);
}

/*
 * [한국어]
 * nvmf_fc_poller_api_abts_received - 호스트 ABTS 수신 시 해당 exchange의 요청 abort 시작
 *
 * @arg: spdk_nvmf_fc_poller_api_abts_recvd_args 포인터
 *
 * ABTS_RECEIVED 메시지를 받은 HWQP poller 스레드에서 실행된다. rpi+oxid로 진행 중 요청을
 * 찾아 abort를 시작한다(ABTS 응답을 위해 ABTS 재송신은 하지 않으므로 send_abts=false).
 * 매칭 요청이 없으면(이미 완료) OXID_NOT_FOUND를 통지한다.
 *
 * 실행 컨텍스트: HWQP poller 스레드.
 *
 * 호출 체인:
 *   nvmf_fc_poller_api_func(ABTS_RECEIVED) -> spdk_thread_send_msg -> [본 함수]
 *     -> nvmf_fc_poller_rpi_find_req + nvmf_fc_request_abort
 */
static void
nvmf_fc_poller_api_abts_received(void *arg)
{
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *args = arg; /* [한국어] ABTS 인자 복원 */
	struct spdk_nvmf_fc_request *fc_req;               /* [한국어] 중단 대상 요청 */

	fc_req = nvmf_fc_poller_rpi_find_req(args->hwqp, args->ctx->rpi, args->ctx->oxid); /* [한국어] rpi+oxid로 진행 중 요청 탐색 */
	if (fc_req) {                                       /* [한국어] 매칭 요청 발견 */
		nvmf_fc_request_abort(fc_req, false, nvmf_fc_poller_abts_done, args); /* [한국어] abort 시작 (ABTS 재송신 없이) */
		return;
	}

	nvmf_fc_poller_api_perform_cb(&args->cb_info,       /* [한국어] 매칭 없음 - oxid 미발견 통지 */
				      SPDK_NVMF_FC_POLLER_API_OXID_NOT_FOUND);
}

/*
 * [한국어]
 * nvmf_fc_poller_api_queue_sync - HWQP에 sync 마커를 등록 (큐 배리어 시작)
 *
 * @arg: spdk_nvmf_fc_poller_api_queue_sync_args 포인터
 *
 * HWQP에 제출된 명령들이 특정 시점까지 모두 처리됐음을 보장하기 위한 배리어 메커니즘.
 * 고유 u_id를 가진 sync 요청을 HWQP의 sync_cbs 리스트에 등록하고, 별도로 큐에 흘려보낸
 * sync marker가 poller에 도달해 queue_sync_done이 같은 u_id로 호출되면 완료로 처리된다.
 *
 * 실행 컨텍스트: HWQP poller 스레드.
 *
 * 호출 체인:
 *   nvmf_fc_poller_api_func(QUEUE_SYNC) -> spdk_thread_send_msg -> [본 함수]
 */
static void
nvmf_fc_poller_api_queue_sync(void *arg)
{
	struct spdk_nvmf_fc_poller_api_queue_sync_args *args = arg; /* [한국어] sync 요청 인자 복원 */

	SPDK_DEBUGLOG(nvmf_fc_poller_api,
		      "HWQP sync requested for u_id = 0x%lx\n", args->u_id); /* [한국어] sync 시작 트레이스 (고유 식별자) */

	/* Add this args to hwqp sync_cb list */
	TAILQ_INSERT_TAIL(&args->hwqp->sync_cbs, args, link); /* [한국어] 대기 sync 리스트에 등록 - done이 u_id 매칭으로 회수 */
}

/*
 * [한국어]
 * nvmf_fc_poller_api_queue_sync_done - 큐를 통과한 sync marker 도달 시 대응 sync 요청 완료
 *
 * @arg: spdk_nvmf_fc_poller_api_queue_sync_done_args 포인터 (tag = 매칭할 u_id)
 *
 * queue_sync로 등록한 sync 요청에 대응하는 마커가 큐를 다 통과해 poller에 도달하면 호출
 * 된다. tag와 같은 u_id의 대기 sync를 찾아 리스트에서 제거하고 완료 콜백을 호출한다.
 * 본 함수 자체의 인자(arg)는 매칭 성공 시 호출자 책임이 아니므로 매칭 실패 경로에서만 free.
 *
 * 실행 컨텍스트: HWQP poller 스레드.
 *
 * 호출 체인:
 *   nvmf_fc_poller_api_func(QUEUE_SYNC_DONE) -> spdk_thread_send_msg -> [본 함수]
 */
static void
nvmf_fc_poller_api_queue_sync_done(void *arg)
{
	struct spdk_nvmf_fc_poller_api_queue_sync_done_args *args = arg; /* [한국어] sync_done 인자 복원 */
	struct spdk_nvmf_fc_hwqp *hwqp = args->hwqp;       /* [한국어] 대상 HWQP */
	uint64_t tag = args->tag;                          /* [한국어] 매칭할 sync u_id */
	struct spdk_nvmf_fc_poller_api_queue_sync_args *sync_args = NULL, *tmp = NULL; /* [한국어] 대기 sync 순회용 */

	assert(args != NULL);                              /* [한국어] 인자는 항상 유효 */

	TAILQ_FOREACH_SAFE(sync_args, &hwqp->sync_cbs, link, tmp) { /* [한국어] 대기 sync 리스트 안전 순회 */
		if (sync_args->u_id == tag) {              /* [한국어] u_id 일치 - 이 sync가 완료됨 */
			/* Queue successfully synced. Remove from cb list */
			TAILQ_REMOVE(&hwqp->sync_cbs, sync_args, link); /* [한국어] 대기 리스트에서 제거 */

			SPDK_DEBUGLOG(nvmf_fc_poller_api,
				      "HWQP sync done for u_id = 0x%lx\n", sync_args->u_id); /* [한국어] sync 완료 트레이스 */

			/* Return the status to poller */
			nvmf_fc_poller_api_perform_cb(&sync_args->cb_info, /* [한국어] sync 완료를 요청자에게 통지 */
						      SPDK_NVMF_FC_POLLER_API_SUCCESS);
			return;                            /* [한국어] arg는 호출자/큐 흐름이 관리하므로 free하지 않고 종료 */
		}
	}

	free(arg);                                         /* [한국어] 매칭 sync 없음 (이미 처리됨) - done 인자만 정리 */
	/* note: no callback from this api */
}

/*
 * [한국어]
 * nvmf_fc_poller_api_add_hwqp - HWQP를 poll group에 등록하고 현재 코어에 고정
 *
 * @arg: spdk_nvmf_fc_hwqp 포인터
 *
 * HWQP를 특정 FC poll group의 hwqp_list에 추가하고, 본 함수가 실행되는 코어를 HWQP의
 * lcore_id로 기록해 thread affinity를 확정한다. 이후 이 HWQP의 폴링/콜백은 모두 이 코어
 * (= reactor/spdk_thread)에서 직렬화되어 lock-free로 동작한다.
 *
 * 실행 컨텍스트: 대상 HWQP가 배치될 spdk_thread(= poll group 스레드).
 *
 * 호출 체인:
 *   nvmf_fc_poller_api_func(ADD_HWQP) -> spdk_thread_send_msg(hwqp->thread) -> [본 함수]
 */
static void
nvmf_fc_poller_api_add_hwqp(void *arg)
{
	struct spdk_nvmf_fc_hwqp *hwqp = (struct spdk_nvmf_fc_hwqp *)arg; /* [한국어] 등록할 HWQP */
	struct spdk_nvmf_fc_poll_group *fgroup = hwqp->fgroup; /* [한국어] 배치 대상 FC poll group */

	assert(fgroup);                                    /* [한국어] poll group은 호출 전에 지정돼 있어야 함 */

	if (nvmf_fc_poll_group_valid(fgroup)) {            /* [한국어] poll group이 여전히 유효하면 (해제 경합 방어) */
		TAILQ_INSERT_TAIL(&fgroup->hwqp_list, hwqp, link); /* [한국어] poll group의 HWQP 리스트에 추가 */
		hwqp->lcore_id	= spdk_env_get_current_core(); /* [한국어] 현재 실행 코어를 HWQP에 고정 (affinity 확정) */
	}
	/* note: no callback from this api */
}

/*
 * [한국어]
 * nvmf_fc_poller_api_remove_hwqp - HWQP를 poll group에서 분리
 *
 * @arg: spdk_nvmf_fc_poller_api_remove_hwqp_args 포인터
 *
 * add_hwqp의 역연산. HWQP를 poll group의 hwqp_list에서 제거하고 fgroup/thread 바인딩을
 * 끊는다. port offline이나 재배치 시 호출되며 완료를 요청자에게 통지한다.
 *
 * 실행 컨텍스트: 해당 HWQP가 바인딩됐던 spdk_thread.
 *
 * 호출 체인:
 *   nvmf_fc_poller_api_func(REMOVE_HWQP) -> spdk_thread_send_msg(hwqp->thread) -> [본 함수]
 */
static void
nvmf_fc_poller_api_remove_hwqp(void *arg)
{
	struct spdk_nvmf_fc_poller_api_remove_hwqp_args *args = arg; /* [한국어] remove 인자 복원 */
	struct spdk_nvmf_fc_hwqp *hwqp = args->hwqp;       /* [한국어] 분리할 HWQP */
	struct spdk_nvmf_fc_poll_group *fgroup = hwqp->fgroup; /* [한국어] 현재 소속 poll group */

	if (nvmf_fc_poll_group_valid(fgroup)) {            /* [한국어] poll group이 유효하면 리스트에서 제거 */
		TAILQ_REMOVE(&fgroup->hwqp_list, hwqp, link);
	}
	hwqp->fgroup = NULL;                               /* [한국어] poll group 바인딩 해제 */
	hwqp->thread = NULL;                               /* [한국어] 스레드 바인딩 해제 (affinity 해제) */

	nvmf_fc_poller_api_perform_cb(&args->cb_info, SPDK_NVMF_FC_POLLER_API_SUCCESS); /* [한국어] 분리 완료 통지 */
}

/*
 * [한국어]
 * nvmf_fc_poller_api_func - poller API 진입점: 요청을 해당 HWQP의 poller 스레드로 디스패치
 *
 * @hwqp: 대상 HWQP (그 스레드로 메시지가 전달됨)
 * @api: 수행할 poller API 종류 (enum)
 * @api_args: API별 인자 구조체 포인터
 * @return: SPDK_NVMF_FC_POLLER_API_SUCCESS, 잘못된 api면 INVALID_ARG
 *
 * connection/queue 관련 작업은 반드시 HWQP가 바인딩된 poller 스레드에서 수행돼야 lock-free
 * 불변식이 유지되므로, 본 함수는 거의 모든 작업을 spdk_thread_send_msg로 cross-thread
 * 디스패치한다(호출자는 임의 스레드 가능). QUIESCE만은 새 frame 처리를 즉시 막기 위해
 * 디스패치 전에 호출 스레드에서 큐 상태를 OFFLINE로 미리 설정한다.
 *
 * 실행 컨텍스트: 임의 스레드에서 호출 가능(메시지 전달이 affinity를 보장).
 *
 * 호출 체인:
 *   fc.c/fc_ls.c 여러 경로 -> [본 함수] -> spdk_thread_send_msg(hwqp->thread, 각 핸들러)
 */
enum spdk_nvmf_fc_poller_api_ret
nvmf_fc_poller_api_func(struct spdk_nvmf_fc_hwqp *hwqp, enum spdk_nvmf_fc_poller_api api,
			void *api_args) {
	switch (api)                                       /* [한국어] API 종류별 분기 */
	{
	case SPDK_NVMF_FC_POLLER_API_ADD_CONNECTION:       /* [한국어] connection 등록 */
				spdk_thread_send_msg(hwqp->thread, /* [한국어] HWQP 스레드로 add_connection 디스패치 */
						     nvmf_fc_poller_api_add_connection, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_DEL_CONNECTION:       /* [한국어] connection 삭제 */
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_del_connection, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_QUIESCE_QUEUE:        /* [한국어] 큐 일시 중단 */
		/* quiesce q polling now, don't wait for poller to do it */
		hwqp->state = SPDK_FC_HWQP_OFFLINE;        /* [한국어] 디스패치 전에 즉시 OFFLINE로 표시 - 새 frame 처리 차단 */
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_quiesce_queue, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_ACTIVATE_QUEUE:       /* [한국어] 큐 재개 */
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_activate_queue, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_ABTS_RECEIVED:        /* [한국어] ABTS 수신 처리 */
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_abts_received, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE:   /* [한국어] 요청 abort 완료 처리 (fc.c 정의 핸들러) */
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_request_abort_complete, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC:           /* [한국어] 큐 sync 배리어 시작 */
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_queue_sync, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC_DONE:      /* [한국어] 큐 sync marker 도달 처리 */
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_queue_sync_done, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_ADD_HWQP:             /* [한국어] HWQP를 poll group에 등록 (인자 = hwqp 자신) */
		spdk_thread_send_msg(hwqp->thread, nvmf_fc_poller_api_add_hwqp, (void *) hwqp);
		break;

	case SPDK_NVMF_FC_POLLER_API_REMOVE_HWQP:          /* [한국어] HWQP를 poll group에서 분리 */
		spdk_thread_send_msg(hwqp->thread, nvmf_fc_poller_api_remove_hwqp, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_ADAPTER_EVENT:        /* [한국어] (미지원) 어댑터 이벤트 */
	case SPDK_NVMF_FC_POLLER_API_AEN:                  /* [한국어] (미지원) 비동기 이벤트 알림 */
	default:                                           /* [한국어] 알 수 없는 API */
		SPDK_ERRLOG("BAD ARG!");
		return SPDK_NVMF_FC_POLLER_API_INVALID_ARG; /* [한국어] 잘못된 인자 반환 */
	}

	return SPDK_NVMF_FC_POLLER_API_SUCCESS;            /* [한국어] 디스패치 성공 (실제 처리는 poller 스레드에서 비동기) */
}

SPDK_LOG_REGISTER_COMPONENT(nvmf_fc_poller_api) /* [한국어] nvmf_fc_poller_api 디버그 로그 컴포넌트 등록 - SPDK_DEBUGLOG 게이팅 */
SPDK_LOG_REGISTER_COMPONENT(nvmf_fc_ls)         /* [한국어] nvmf_fc_ls 디버그 로그 컴포넌트 등록 - LS 처리 트레이스 게이팅 */
