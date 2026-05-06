/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   Copyright (c) 2018-2019 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * [한국어 설명] NVMe over Fibre Channel(FC-NVMe) 와이어 프로토콜 스펙 정의 (nvmf_fc_spec.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 INCITS T11 FC-NVMe 표준에서 정의한 모든 와이어 포맷(프레임 헤더, Information Unit,
 * Link Service 명령, Link Service 디스크립터, 거절 사유 코드 등)을 C 구조체와 매크로로 옮긴 파일이다.
 * SPDK 의 NVMe-oF FC 트랜스포트(`lib/nvmf/fc.c`, `lib/nvmf/fc_ls.c`)가 이 정의를 그대로 사용해
 * 호스트 ↔ 컨트롤러 간 FC 프레임을 작성·해석하므로, 이 파일은 사실상 SPDK 의 FC 트랜스포트가
 * "어떤 비트가 어떤 의미인지" 를 합의하는 ABI 역할을 한다.
 * 코드를 포함하지 않고 자료형/상수만 모아 두었기 때문에 실행 함수는 없으며, FC HBA 드라이버(예: Broadcom
 * lpfc 의 user-space 변형, vendor SDK)와 SPDK NVMe-oF 코어가 동일한 메모리 레이아웃을 공유하기 위한
 * "스펙 미러" 역할만 한다. 빅엔디언(BE) 정수 타입을 별칭(typedef)으로 명시해 FC 와이어가 빅엔디언임을
 * 명확히 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인은 아래와 같다:
 *   FC HBA(하드웨어) → vendor HBA driver → SPDK FC poller(`lib/nvmf/fc.c`)
 *     → 본 헤더의 구조체로 캡슐 해석 → `lib/nvmf` 코어(NVMe 명령 디스패치)
 *     → bdev 레이어(`lib/bdev`) → bdev 모듈(`bdev_nvme`/`bdev_aio`/...) → 백엔드 저장장치
 * SPDK 는 PHY 레벨을 직접 다루지 않으며 HBA 가 LS(Link Service) / IU(Information Unit) 를 분리해
 * 위로 올려준 뒤, 이 파일의 구조체에 캐스팅해 처리한다. 따라서 이 헤더는 호스트 유저스페이스에서
 * SPDK reactor 스레드 컨텍스트(폴링 모드)에서만 사용된다.
 * 같은 NVMe-oF 위 layer 인 RDMA(`nvmf_rdma_spec`)·TCP(`nvmf_tcp_spec`) 와 달리, FC 는 64-bit
 * Connection ID/Association ID, OX_ID/RX_ID 같은 FC 고유 식별자를 사용하므로 별도의 와이어 포맷이
 * 필요하다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: `spdk/env.h`(SPDK_STATIC_ASSERT 등 환경 매크로), `spdk/nvme.h`(`struct spdk_nvme_cmd`,
 *       `struct spdk_nvme_cpl`, `SPDK_NVME_NQN_FIELD_SIZE`).
 * 의존당함: `lib/nvmf/fc*.c`(FC 트랜스포트 구현), `module/nvmf/fc/*`(있다면 FC 어댑터),
 *           Broadcom/vendor HBA 사용자 공간 드라이버 통합 코드.
 * 데이터 흐름: 호스트가 보낸 FC 프레임 페이로드는 HBA 드라이버에서 buffer 로 노출되며, SPDK 는 그 buffer
 *   을 `struct spdk_nvmf_fc_cmnd_iu *` 등으로 캐스팅해 NVMe 명령을 추출(read path), 또는 응답을 작성해
 *   HBA 에 송신(write path) 한다. 따라서 이 헤더의 구조체는 모두 `__attribute__((packed))` 와 동등한
 *   FC 와이어 레이아웃이어야 하며, 모든 정의에 `SPDK_STATIC_ASSERT` 로 크기를 강제 검증한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - `struct spdk_nvmf_fc_frame_hdr`     : 24 byte FC 프레임 헤더 (R_CTL/D_ID/S_ID/F_CTL/SEQ/OX_ID/RX_ID)
 * - `struct spdk_nvmf_fc_cmnd_iu`       : NVMe Command IU (96B). NVMe SQE 를 FC 캡슐 안에 담는다.
 * - `struct spdk_nvmf_fc_ersp_iu`       : NVMe Extended Response IU (32B). NVMe CQE 를 운반.
 * - `struct spdk_nvmf_fc_xfer_rdy_iu`   : Transfer Ready (XFER_RDY) IU. write 데이터 푸시를 협상.
 * - `struct spdk_nvmf_fc_ls_cr_assoc_*` : Create Association LS 명령/응답. NVMe 컨트롤러 1개 = FC association 1개.
 * - `struct spdk_nvmf_fc_ls_cr_conn_*`  : Create I/O Connection LS. admin queue + 각 IO queue 별 connection.
 * - `struct spdk_nvmf_fc_ls_disconnect_*`: Disconnect LS. 단일 connection 또는 association 해제.
 * - `struct spdk_nvmf_fc_ls_rjt`        : LS 거절(Reject) 응답.
 * - 핵심 매크로: FCNVME_R_CTL_*  (FC 프레임 R_CTL 필드 값들 - LS req/rsp, ABTS, status 등)
 *               FCNVME_F_CTL_*  (F_CTL 필드 - END_SEQ/SEQ_INIT/우선순위 비트)
 *               FCNVME_TYPE_*   (FC TYPE 필드 - BLS, FC exchange, NVMF data)
 *               FCNVME_LS_*     (LS 명령 코드 enum)
 *               FCNVME_LSDESC_* (LS descriptor tag enum)
 *               FCNVME_RJT_RC_*/FCNVME_RJT_EXP_* (LS 거절 사유/설명 enum)
 */

#ifndef __NVMF_FC_SPEC_H__
#define __NVMF_FC_SPEC_H__
/* [한국어] 헤더 가드 - 다중 #include 시 중복 정의를 방지한다. FC-NVMe 와이어 포맷 정의는
 *  여러 트랜스포트 파일에서 import 될 수 있으므로 가드가 반드시 필요하다. */

#include "spdk/env.h"
/* [한국어] SPDK 환경 추상화 헤더 - SPDK_STATIC_ASSERT(컴파일타임 size 검증), 빅엔디언 타입 등
 *  와이어 포맷 강제에 필요한 매크로를 가져온다. 이 매크로는 FC 와이어 구조체의 크기가 스펙대로
 *  유지되는지 컴파일 타임에 막아주므로 빌드 안전성에 필수적이다. */
#include "spdk/nvme.h"
/* [한국어] NVMe 베이스 스펙 헤더 - `struct spdk_nvme_cmd`(64B SQE), `struct spdk_nvme_cpl`(16B CQE),
 *  `SPDK_NVME_NQN_FIELD_SIZE`(NVMe Qualified Name 최대 길이) 등을 가져온다. FC IU 는 NVMe SQE/CQE 를
 *  그대로 페이로드 안에 임베드하므로 NVMe 베이스 헤더가 반드시 필요하다. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 컴파일러가 본 헤더를 include 할 때 C 링크 규약(name mangling 없음)을 강제한다.
 *  SPDK 는 C 라이브러리이므로 C++ 애플리케이션에서 사용 시 이 가드가 없으면 심볼 mangling 으로
 *  링크 실패가 발생한다. */
#endif

/*
 * FC-NVMe Spec. Definitions
 */

#define FCNVME_R_CTL_CMD_REQ                   0x06
/* [한국어] R_CTL(Routing Control) 필드 값: 일반 Command 요청 - FC 프레임이 NVMe Command IU 를 운반함을
 *  표시. R_CTL 은 FC 헤더 첫 바이트로, 라우터/HBA 가 프레임 분류에 사용한다 (FC-FS-3 §9.2). */
#define FCNVME_R_CTL_DATA_OUT                  0x01
/* [한국어] R_CTL: Solicited Data (initiator → target 으로 전송되는 write 데이터). 호스트가 XFER_RDY 를
 *  받은 뒤 실제 write 데이터를 채워 보낼 때 사용한다. */
#define FCNVME_R_CTL_CONFIRM                   0x03
/* [한국어] R_CTL: Confirm 프레임 - 시퀀스 수신 확인. FC class 2/3 환경에서 명시적 ACK 가 필요한 경우
 *  사용되며 NVMe-oF FC 에서는 자주 쓰이지 않는다. */
#define FCNVME_R_CTL_STATUS                    0x07
/* [한국어] R_CTL: Status 프레임 - NVMe 명령의 단순 완료 상태를 운반(FCNVME_GOOD_RSP_LEN=12B 의 짧은 응답). */
#define FCNVME_R_CTL_ERSP_STATUS               0x08
/* [한국어] R_CTL: Extended Response (ERSP) - 단순 status 가 아닌, NVMe CQE 전체를 담은 32B ERSP IU 를
 *  운반. 호스트가 CQE 의 SC/SCT 등 상세 결과를 확인해야 할 때 사용된다(ersp_ratio 협상 결과). */
#define FCNVME_R_CTL_LS_REQUEST                0x32
/* [한국어] R_CTL: Link Service Request - association/connection 생성·해제 같은 제어 명령(LS) 요청
 *  프레임. NVMe 데이터 경로와 분리된 컨트롤 채널이다. */
#define FCNVME_R_CTL_LS_RESPONSE               0x33
/* [한국어] R_CTL: Link Service Response - LS 요청에 대한 ACC(accept) 또는 RJT(reject) 응답. */
#define FCNVME_R_CTL_BA_ABTS                   0x81
/* [한국어] R_CTL: Basic Accept / Abort Sequence (ABTS) - 진행 중인 exchange 를 강제 중단할 때 송신.
 *  타임아웃·취소 처리에 사용된다. */

#define FCNVME_F_CTL_END_SEQ                   0x080000
/* [한국어] F_CTL(Frame Control) 비트: 이 프레임이 시퀀스의 마지막 프레임임을 표시(End of Sequence).
 *  세트되면 수신측은 시퀀스 누락 검사를 종료한다. */
#define FCNVME_F_CTL_SEQ_INIT                  0x010000
/* [한국어] F_CTL 비트: Sequence Initiative 토큰 이전 - 이 비트가 세트되면 송신측이 보유하던 SI 토큰이
 *  수신측으로 넘어가, 다음 시퀀스를 수신측이 시작할 권한을 갖게 된다(양방향 전송 토큰). */

/* END_SEQ | LAST_SEQ | Exchange Responder | SEQ init */
#define FCNVME_F_CTL_RSP                       0x990000
/* [한국어] F_CTL 비트 마스크 조합값으로 응답 프레임 작성 시 사용:
 *  - END_SEQ(0x080000): 마지막 프레임
 *  - LAST_SEQ(0x100000): exchange 의 마지막 시퀀스
 *  - Exchange Responder(0x800000): 송신측이 exchange responder(즉, target 측)
 *  - SEQ_INIT(0x010000): SI 를 상대에게 넘겨 다음 명령을 받을 수 있게 한다.
 *  값 0x990000 = 0x800000 | 0x100000 | 0x080000 | 0x010000. */
#define FCNVME_F_CTL_PRIORITY_ENABLE           0x020000
/* [한국어] F_CTL 비트: CS_CTL/Priority 필드를 우선순위로 해석할지 여부. SAN 에서 QoS 트래픽 분리를
 *  위해 사용되지만 NVMe-oF FC 기본 경로에서는 거의 사용되지 않는다. */

#define FCNVME_D_FCTL_DEVICE_HDR_16_MASK       0x1
/* [한국어] DF_CTL(Data Field Control) 마스크: 16-byte device header 가 데이터 필드 앞에 존재함을 표시.
 *  FC-FS-3 §9.4 에 따라 일부 디바이스가 페이로드 앞에 추가 header 를 넣는 경우 사용. */
#define FCNVME_D_FCTL_NETWORK_HDR_MASK         0x20
/* [한국어] DF_CTL 마스크: Network header 가 페이로드 앞에 존재함을 표시. 라우팅이 필요한 토폴로지에서
 *  FC 프레임 위에 네트워크 layer 헤더를 얹을 때 세트된다. */
#define FCNVME_D_FCTL_NETWORK_HDR_SIZE         16
/* [한국어] Network header 의 고정 크기 (16 byte). 마스크가 세트되었을 때 페이로드 시작점을 16B 만큼
 *  shift 해서 NVMe IU 를 찾는 데 사용. */
#define FCNVME_D_FCTL_ESP_HDR_MASK             0x40
/* [한국어] DF_CTL 마스크: FC-SP(ESP) 보안 header 가 존재함을 표시. IPsec ESP 와 유사한 8B 보안 헤더가
 *  페이로드 앞에 prepended 된다. */
#define FCNVME_D_FCTL_ESP_HDR_SIZE             8
/* [한국어] FC-SP ESP header 의 고정 크기 (8 byte). 보안 활성화 시 페이로드 오프셋 보정에 사용. */

#define FCNVME_TYPE_BLS                        0x0
/* [한국어] FC Type 필드 값: Basic Link Service - ABTS, BA_ACC, BA_RJT 같은 저수준 link service.
 *  Type 필드는 FC 헤더 9번째 바이트로 상위 layer 프로토콜을 지정한다. */
#define FCNVME_TYPE_FC_EXCHANGE                0x08
/* [한국어] FC Type: 일반 FC exchange (LS 컨트롤 메시지 운반에 사용). */
#define FCNVME_TYPE_NVMF_DATA                  0x28
/* [한국어] FC Type: NVMe-oF data 채널 (Command IU/Data/ERSP IU 운반). T11 FC-NVMe 가 0x28 을 NVMe-oF
 *  전용으로 할당받았다. */

#define FCNVME_CMND_IU_FC_ID                   0x28
/* [한국어] Command IU 의 fc_id 필드 기대값 - FC-NVMe 프로토콜 식별자(NVMe-oF data type 과 동일). 수신측이
 *  IU 가 올바른 FC-NVMe 패킷인지 sanity check 할 때 사용. */
#define FCNVME_CMND_IU_SCSI_ID                 0xFD
/* [한국어] Command IU 의 scsi_id 필드 기대값 - FC-NVMe 식별 매직 넘버(0xFD). FCP-SCSI 와 구별하기 위한
 *  버전·식별 바이트로, 수신측은 이 값으로 NVMe IU 인지 확인한다. */

#define FCNVME_CMND_IU_NODATA                  0x00
/* [한국어] Command IU flags: 데이터 전송 없음 (admin 명령 등). 호스트와 컨트롤러 사이 데이터 페이로드가
 *  발생하지 않는 명령. */
#define FCNVME_CMND_IU_READ                    0x10
/* [한국어] Command IU flags: read 명령 (target → host 데이터 전송). target 이 곧이어 R_CTL=DATA_OUT
 *  프레임으로 데이터를 푸시한다. */
#define FCNVME_CMND_IU_WRITE                   0x01
/* [한국어] Command IU flags: write 명령 (host → target 데이터 전송). target 이 XFER_RDY 를 보낸 뒤
 *  host 가 데이터를 푸시. */

/* BLS reject error codes */
#define FCNVME_BLS_REJECT_UNABLE_TO_PERFORM    0x09
/* [한국어] Basic Link Service Reject(BLS RJT) reason code: 요청을 수행할 수 없음. ABTS 등 BLS 명령을
 *  거절할 때 reason 으로 사용. */
#define FCNVME_BLS_REJECT_EXP_NOINFO           0x00
/* [한국어] BLS RJT explanation: 추가 정보 없음. 기본 거절 사유. */
#define FCNVME_BLS_REJECT_EXP_INVALID_OXID     0x03
/* [한국어] BLS RJT explanation: OX_ID(Originator Exchange ID) 가 유효하지 않음. 이미 종료된 exchange 에
 *  대해 ABTS 가 들어온 경우 등에 반환. */

/*
 * FC NVMe Link Services (LS) constants
 */
#define FCNVME_MAX_LS_REQ_SIZE                  1536
/* [한국어] LS 요청 페이로드 최대 크기(byte). Create Association 명령의 host NQN+subsys NQN 등 큰 필드를
 *  수용할 수 있어야 하므로 1.5KB 로 잡혀 있다. */
#define FCNVME_MAX_LS_RSP_SIZE                  64
/* [한국어] LS 응답 페이로드 최대 크기. ACC 응답은 짧은 connection_id/association_id 만 담으므로 64B 면
 *  충분. */

#define FCNVME_LS_CA_CMD_MIN_LEN                592
/* [한국어] Create Association 명령 페이로드의 최소 길이(LSDESC + descriptor list). 미만이면 형식 오류로
 *  거절. */
#define FCNVME_LS_CA_DESC_LIST_MIN_LEN          584
/* [한국어] Create Association 의 descriptor list 최소 길이 (위 값에서 8B header 제외). */
#define FCNVME_LS_CA_DESC_MIN_LEN               576
/* [한국어] Create Association 의 단일 descriptor 최소 길이 (필수 필드 + reserved 영역). */

/* this value needs to be in sync with low level driver buffer size */
#define FCNVME_MAX_LS_BUFFER_SIZE               2048
/* [한국어] LS 메시지 송수신 버퍼 크기. HBA 드라이버 측 버퍼 크기와 반드시 동일해야 한다 - 두 layer 가
 *  같은 메모리 청크를 공유하기 때문에 mismatch 가 있으면 buffer overflow 가 발생한다. */

#define FCNVME_GOOD_RSP_LEN                     12
/* [한국어] 단순 성공 응답(R_CTL_STATUS) 의 페이로드 길이. ERSP 가 아닌 12B 의 짧은 응답. */
#define FCNVME_ASSOC_HOSTID_LEN                 16
/* [한국어] Association 생성 시 host ID 필드 길이 (16 byte UUID 형태). NVMe 1.4 host identifier 와 동일
 *  형식. */


typedef uint64_t FCNVME_BE64;
/* [한국어] FC 와이어상 64-bit 빅엔디언 정수 타입 별칭. C 표준에는 endian-aware 타입이 없으므로 단순
 *  uint64_t 별칭으로 두되, 이름으로 "이 값은 빅엔디언" 임을 명시. 실제 변환은 spdk_be64toh 등으로 수행. */
typedef uint32_t FCNVME_BE32;
/* [한국어] 32-bit 빅엔디언 정수 별칭 (FC descriptor tag/length, OX_ID 등에 사용). */
typedef uint16_t FCNVME_BE16;
/* [한국어] 16-bit 빅엔디언 정수 별칭 (cntlid, sqsize, qid 등). */

/*
 * FC-NVME LS Commands
 */
enum {
	/* [한국어] FC-NVMe Link Service 명령 코드 enum.
	 *  LS 페이로드 word0 의 ls_cmd 필드에 들어가며, 수신측이 어떤 컨트롤 명령인지 dispatch 하는 키. */
	FCNVME_LS_RSVD                = 0,
	/* [한국어] 0 - 예약 (사용 금지). 0 이 들어오면 형식 오류. */
	FCNVME_LS_RJT                 = 1,
	/* [한국어] 1 - LS Reject 응답. 요청측이 보낸 LS 를 거절할 때 응답 ls_cmd 가 이 값. */
	FCNVME_LS_ACC                 = 2,
	/* [한국어] 2 - LS Accept 응답. 요청을 수락할 때 ls_cmd 가 이 값으로 세트된다. */
	FCNVME_LS_CREATE_ASSOCIATION  = 3,
	/* [한국어] 3 - Create Association 요청. NVMe controller 1개에 대응하는 FC association 을 생성한다.
	 *  이 명령이 성공해야 후속 admin queue connection 까지 한번에 만들어진다. */
	FCNVME_LS_CREATE_CONNECTION	  = 4,
	/* [한국어] 4 - Create I/O Connection 요청. admin queue 외 추가 IO queue 별로 connection 을 만들 때
	 *  사용. NVMe queue pair 1개 = FC connection 1개로 매핑된다. */
	FCNVME_LS_DISCONNECT          = 5,
	/* [한국어] 5 - Disconnect 요청. 단일 connection 을 닫거나 association 전체를 종료할 수 있다. */
};

/*
 * FC-NVME Link Service Descriptors
 */
enum {
	/* [한국어] LS payload 안에 들어가는 descriptor 의 tag 값 enum.
	 *  각 descriptor 는 (desc_tag, desc_len, body) 형태의 TLV 구조이며 tag 로 종류를 식별한다. */
	FCNVME_LSDESC_RSVD             = 0x0,
	/* [한국어] 예약 - 사용 금지. */
	FCNVME_LSDESC_RQST             = 0x1,
	/* [한국어] LS 요청 정보 descriptor (어떤 LS 명령에 대한 응답인지를 ACC/RJT 안에 echo). */
	FCNVME_LSDESC_RJT              = 0x2,
	/* [한국어] LS Reject descriptor - reason_code/reason_explanation/vendor 코드를 운반. */
	FCNVME_LSDESC_CREATE_ASSOC_CMD = 0x3,
	/* [한국어] Create Association 명령 본체 descriptor (host NQN, subsys NQN, hostid, sqsize, cntlid 등). */
	FCNVME_LSDESC_CREATE_CONN_CMD  = 0x4,
	/* [한국어] Create IO Connection 명령 본체 descriptor (qid, sqsize 등). */
	FCNVME_LSDESC_DISCONN_CMD      = 0x5,
	/* [한국어] Disconnect 명령 본체 descriptor (reserved 4 word, scope 식별). */
	FCNVME_LSDESC_CONN_ID          = 0x6,
	/* [한국어] Connection ID descriptor - ACC 응답에 포함되어 호스트가 후속 IU 에서 사용할 64-bit
	 *  connection_id 를 알려준다. */
	FCNVME_LSDESC_ASSOC_ID         = 0x7,
	/* [한국어] Association ID descriptor - 64-bit association_id 운반. Disconnect/Create Connection 시
	 *  대상 association 식별에 사용. */
};

/*
 * LS Reject reason_codes
 */
enum fcnvme_ls_rjt_reason {
	/* [한국어] LS RJT 의 reason_code 필드 값. RJT 응답 시 이 코드로 거절 이유를 분류한다.
	 *  호스트는 reason+explanation 조합으로 재시도/abort 결정을 내린다. */
	FCNVME_RJT_RC_NONE         = 0,     /* no reason - not to be sent */
	/* [한국어] 0 - 사유 없음. 정상적으로는 송신되어서는 안 되는 sentinel 값. */
	FCNVME_RJT_RC_INVAL        = 0x01,  /* invalid NVMe_LS command code */
	/* [한국어] LS command code 가 정의되지 않은 값 (FCNVME_LS_RSVD 등 미지정 코드 수신). */
	FCNVME_RJT_RC_LOGIC        = 0x03,  /* logical error */
	/* [한국어] 논리 오류 - 현재 association 상태에서 허용되지 않는 명령(예: 미생성 상태에서 disconnect). */
	FCNVME_RJT_RC_UNAB         = 0x09,  /* unable to perform request */
	/* [한국어] 일반적 수행 불가 - 자원 부족이나 일시적 사유. 호스트는 잠시 후 재시도 가능. */
	FCNVME_RJT_RC_UNSUP        = 0x0b,  /* command not supported */
	/* [한국어] 명령 미지원 - target 이 해당 LS 를 구현하지 않음 (예: vendor extension). */
	FCNVME_RJT_RC_INPROG       = 0x0e,  /* command already in progress */
	/* [한국어] 동일 명령이 이미 진행 중 - 중복 요청. */
	FCNVME_RJT_RC_INV_ASSOC    = 0x40,  /* invalid Association ID */
	/* [한국어] 알 수 없는 association_id - 이미 종료되었거나 위조된 ID. */
	FCNVME_RJT_RC_INV_CONN     = 0x41,  /* invalid Connection ID */
	/* [한국어] 알 수 없는 connection_id. */
	FCNVME_RJT_RC_INV_PARAM    = 0x42,  /* invalid parameters */
	/* [한국어] 파라미터 값 범위 위반(예: sqsize 가 한계 초과). */
	FCNVME_RJT_RC_INSUFF_RES   = 0x43,  /* insufficient resources */
	/* [한국어] 자원 부족 - 큐/메모리 한계 도달. */
	FCNVME_RJT_RC_INV_HOST     = 0x44,  /* invalid or rejected host */
	/* [한국어] 호스트 식별자가 거부됨(접근 제어/ACL 위반). */
	FCNVME_RJT_RC_VENDOR       = 0xff,  /* vendor specific error */
	/* [한국어] vendor 고유 오류 - 디스크립터의 vendor 필드와 함께 해석. */
};

/*
 * LS Reject reason_explanation codes
 */
enum fcnvme_ls_rjt_explan {
	/* [한국어] LS RJT 의 reason_explanation 필드 값. reason_code 의 상세 사유를 추가 비트로 분리해서
	 *  운반한다 (예: INV_PARAM 인 경우 어떤 파라미터가 문제인지). */
	FCNVME_RJT_EXP_NONE	       = 0x00,  /* No additional explanation */
	/* [한국어] 추가 설명 없음 (디폴트). */
	FCNVME_RJT_EXP_OXID_RXID   = 0x17,  /* invalid OX_ID-RX_ID combo */
	/* [한국어] OX_ID/RX_ID 조합이 알려진 exchange 와 일치하지 않음 - 이미 종료된 exchange 등. */
	FCNVME_RJT_EXP_UNAB_DATA   = 0x2a,  /* unable to supply data */
	/* [한국어] target 이 요구된 데이터를 공급할 수 없음 (자원 부족 등). */
	FCNVME_RJT_EXP_INV_LEN     = 0x2d,  /* invalid payload length */
	/* [한국어] 페이로드 길이 비정상 - 최소 크기 미달 또는 최대 크기 초과. */
	FCNVME_RJT_EXP_INV_ESRP    = 0x40,  /* invalid ESRP ratio */
	/* [한국어] ersp_ratio(Extended Response 비율) 값이 범위를 벗어남. ersp_ratio 는 N개의 명령마다 1번씩
	 *  ERSP 를 강제하는 throttle 파라미터. */
	FCNVME_RJT_EXP_INV_CTL_ID  = 0x41,  /* invalid controller ID */
	/* [한국어] cntlid (NVMe controller ID) 가 알려진 컨트롤러와 매칭되지 않음. */
	FCNVME_RJT_EXP_INV_Q_ID    = 0x42,  /* invalid queue ID */
	/* [한국어] qid 값이 범위 밖(0=admin, 1..n=IO 큐)이거나 미할당. */
	FCNVME_RJT_EXP_SQ_SIZE     = 0x43,  /* invalid submission queue size */
	/* [한국어] sqsize 가 NVMe controller 의 MQES(Maximum Queue Entries Supported) 초과. */
	FCNVME_RJT_EXP_INV_HOST_ID = 0x44,  /* invalid or rejected host ID */
	/* [한국어] 16-byte hostid 가 ACL 에서 거부됨. */
	FCNVME_RJT_EXP_INV_HOSTNQN = 0x45,  /* invalid or rejected host NQN */
	/* [한국어] hostnqn 이 NVMe NQN 형식 위반이거나 ACL 에서 거부. */
	FCNVME_RJT_EXP_INV_SUBNQN  = 0x46,  /* invalid or rejected subsys nqn */
	/* [한국어] subnqn 이 알려진 NVMe subsystem NQN 과 매칭되지 않음. */
};

/*
 * NVMe over FC CMD IU
 */
struct spdk_nvmf_fc_cmnd_iu {
	/* [한국어] NVMe over FC Command Information Unit (96 byte 고정).
	 *  호스트가 컨트롤러에 NVMe 명령을 보낼 때 FC 프레임 페이로드로 들어가며, FC 헤더 24B 다음에 위치한다.
	 *  설정자: 호스트 NVMe-oF 드라이버 (Linux nvme-fc 또는 SPDK 호스트). 읽는 자: SPDK FC target 의 LS/IO
	 *  핸들러. 동기화: 한 IU 는 단일 OX_ID exchange 에 속하므로 별도 락 없이 처리. */
	uint32_t scsi_id: 8,
		 fc_id: 8,
		 cmnd_iu_len: 16;
	/* [한국어] Word0 (32-bit) - FC 식별자 + IU 길이.
	 *  scsi_id (8 bit): FCNVME_CMND_IU_SCSI_ID(0xFD) 고정. FCP-SCSI 와 구분하는 식별자.
	 *  fc_id (8 bit): FCNVME_CMND_IU_FC_ID(0x28) 고정. NVMe-oF data type ID.
	 *  cmnd_iu_len (16 bit): IU 전체 길이를 4byte 단위로 표현 (96/4 = 24).
	 *  설정자: 호스트가 IU 작성 시 채움. 읽는 자: target 이 sanity check 에 사용. */

	uint32_t rsvd0: 24,
		 flags: 8;
	/* [한국어] Word1 (32-bit).
	 *  rsvd0 (24 bit): 예약 - 0 으로 송신.
	 *  flags (8 bit): FCNVME_CMND_IU_NODATA/READ/WRITE 중 하나. target 이 데이터 전송 방향(read=target→host,
	 *    write=host→target)을 결정하는 데 사용. read 면 target 이 곧이어 DATA_OUT 프레임을 보내고, write 면
	 *    XFER_RDY 를 보낸다. */

	uint64_t conn_id;
	/* [한국어] 64-bit Connection ID - LS Create Connection (또는 Create Association 의 admin queue) 에서
	 *  target 이 발급한 값. 호스트는 이후 모든 IU 의 conn_id 에 이 값을 채워, target 이 어느 NVMe queue
	 *  pair 에 속한 명령인지 식별하게 한다.
	 *  설정자: target 이 ACC 응답에서 발급. 읽는 자: target 의 IU dispatch 핸들러가 conn_id → qpair lookup. */

	uint32_t cmnd_seq_num;
	/* [한국어] Command sequence number - 동일 connection 내 명령 순번. NVMe 의 CID(Command Identifier)와
	 *  유사하게 호스트가 부여하며, ERSP 응답에서 어느 명령에 대한 것인지 매칭하는 데 사용. */

	uint32_t data_len;
	/* [한국어] 명령에 동반되는 데이터 전송의 총 길이(byte). read 면 target 이 보낼 데이터 크기, write 면
	 *  호스트가 보낼 데이터 크기. XFER_RDY 의 burst_len 과도 연동된다. */

	struct spdk_nvme_cmd cmd;
	/* [한국어] NVMe SQE(Submission Queue Entry, 64 byte) 본체. opcode/CDW0~15 이 그대로 들어간다.
	 *  target 은 이 필드를 fc 트랜스포트에서 추출해 `lib/nvmf` 의 NVMe 명령 디스패처로 전달.
	 *  설정자: 호스트 NVMe 코어 (NVMe 1.x §4.2). 읽는 자: SPDK FC target 의 IO 처리 콜백. */

	uint32_t rsvd1[2];
	/* [한국어] 8 byte 예약 - 96 byte 정렬을 맞추기 위한 padding. 향후 spec 확장 시 사용될 수 있다. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_cmnd_iu) == 96, "size_mismatch");
/* [한국어] 컴파일타임에 IU 크기 96 byte 를 강제 검증. 컴파일러 패딩이 끼면 와이어 호환이 깨지므로 실패시
 *  빌드 자체를 막는다. */

/*
 * NVMe over Extended Response IU
 */
struct spdk_nvmf_fc_ersp_iu {
	/* [한국어] Extended Response Information Unit (32 byte). NVMe CQE 를 운반하는 응답 IU 로,
	 *  R_CTL=FCNVME_R_CTL_ERSP_STATUS(0x08) 프레임의 페이로드.
	 *  설정자: SPDK FC target 의 IO 완료 콜백. 읽는 자: 호스트 NVMe-oF FC 드라이버 (CQE 처리). */
	uint32_t status_code: 8,
		 rsvd0: 8,
		 ersp_len: 16;
	/* [한국어] Word0 - status_code(8b) FCNVME 응답 상태, rsvd0(8b) 예약, ersp_len(16b) ERSP 페이로드 길이
	 *  (4 byte 단위, 32B/4=8). status_code 는 transport 수준 오류를 나타내고 NVMe-level 결과는 rsp 필드의
	 *  CQE 에 들어간다. */

	uint32_t response_seq_no;
	/* [한국어] 응답 시퀀스 번호 - 호스트가 보낸 cmnd_seq_num 과 매칭되어 어떤 명령에 대한 응답인지 식별. */

	uint32_t transferred_data_len;
	/* [한국어] 실제로 전송된 데이터 길이(byte). 호스트가 요청한 data_len 과 다를 수 있으며, 호스트는
	 *  이 값으로 short-transfer 를 감지한다. */

	uint32_t rsvd1;
	/* [한국어] 4 byte 예약 - 32 byte 정렬 padding. */

	struct spdk_nvme_cpl rsp;
	/* [한국어] NVMe CQE(Completion Queue Entry, 16 byte) 본체. SC/SCT/CID 등이 들어가며, 호스트는 이것을
	 *  NVMe 코어에 그대로 전달. NVMe 1.x §4.6 의 CQE 레이아웃과 동일. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ersp_iu) == 32, "size_mismatch");
/* [한국어] ERSP IU 가 정확히 32 byte 임을 보증 - 와이어 호환성 강제. */

/*
 * Transfer ready IU
 */
struct spdk_nvmf_fc_xfer_rdy_iu {
	/* [한국어] Transfer Ready (XFER_RDY) IU - write 명령에서 target 이 호스트에게 "이만큼 데이터를 보낼
	 *  준비가 됐다" 고 알리는 12 byte IU.
	 *  설정자: SPDK FC target (write 명령 처리 시작 시). 읽는 자: 호스트가 이를 받고 R_CTL=DATA_OUT 으로
	 *  실제 데이터 전송 시작. */
	uint32_t relative_offset;
	/* [한국어] 명령 데이터 버퍼 내 시작 오프셋 (byte). 0 부터 시작이 일반적이지만 target 이 분할 burst 를
	 *  요청할 수도 있다. */

	uint32_t burst_len;
	/* [한국어] 이번 burst 에서 호스트가 보낼 수 있는 최대 데이터 길이(byte). target 의 buffer credit 만큼
	 *  제한해 흐름 제어 역할을 한다. */

	uint32_t rsvd;
	/* [한국어] 4 byte 예약 - 12 byte 정렬 padding. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_xfer_rdy_iu) == 12, "size_mismatch");
/* [한국어] XFER_RDY IU 크기 12B 강제. */

/*
 * FC VM header
 */
struct spdk_nvmf_fc_vm_header {
	/* [한국어] FC Virtual Machine 헤더 (16 byte) - VM ID 기반 가상 머신 트래픽 분리를 지원하는 SAN 환경에서
	 *  페이로드 앞에 prepended 될 수 있는 옵션 헤더. NVMe-oF FC 기본 경로에서는 거의 사용되지 않으나
	 *  스펙상 정의되어 있어 포함. */
	FCNVME_BE32 dst_vmid;
	/* [한국어] 목적지 VM ID (32 bit, 빅엔디언). 트래픽이 도착할 가상 머신 식별자. */

	FCNVME_BE32 src_vmid;
	/* [한국어] 송신측 VM ID. */

	FCNVME_BE32 rsvd0;
	/* [한국어] 예약 word0 - 0 으로 송신. */

	FCNVME_BE32 rsvd1;
	/* [한국어] 예약 word1. */
};

/*
 * FC NVME Frame Header
 */
struct spdk_nvmf_fc_frame_hdr {
	/* [한국어] FC 프레임 헤더 (24 byte 고정). 모든 FC 프레임의 가장 앞에 위치하는 메타데이터 블록으로,
	 *  routing/sequence/exchange 식별을 담당한다(FC-FS-3 §9).
	 *  설정자: HBA 또는 SPDK FC target. 읽는 자: 송수신 양 끝단의 HBA·소프트웨어 스택. */
	FCNVME_BE32 r_ctl: 8,
		    d_id: 24;
	/* [한국어] Word0 - r_ctl(8b) 프레임 타입(FCNVME_R_CTL_*), d_id(24b) Destination ID(목적지 N_Port ID).
	 *  HBA 가 d_id 로 라우팅하고, r_ctl 로 SW 가 frame 클래스를 분류한다. */

	FCNVME_BE32 cs_ctl: 8,
		    s_id: 24;
	/* [한국어] Word1 - cs_ctl(8b) Class-Specific Control(우선순위/preferred path), s_id(24b) Source ID. */

	FCNVME_BE32 type: 8,
		    f_ctl: 24;
	/* [한국어] Word2 - type(8b) 상위 layer 프로토콜(FCNVME_TYPE_*), f_ctl(24b) Frame Control 비트필드
	 *  (END_SEQ/SEQ_INIT/exchange 방향 등). */

	FCNVME_BE32 seq_id: 8,
		    df_ctl: 8,
		    seq_cnt: 16;
	/* [한국어] Word3 - seq_id(8b) Sequence ID(같은 exchange 내 시퀀스 번호), df_ctl(8b) Data Field Control
	 *  (선택적 device/network/ESP header 존재 여부), seq_cnt(16b) Sequence Count(시퀀스 내 프레임 순번,
	 *  순서 검증·중복 감지용). */

	FCNVME_BE32 ox_id: 16,
		    rx_id: 16;
	/* [한국어] Word4 - ox_id(16b) Originator Exchange ID, rx_id(16b) Responder Exchange ID. exchange 는
	 *  관련 시퀀스의 묶음이며, originator/responder 가 각자 부여한 ID 로 양방향 식별한다. NVMe 명령 1개 =
	 *  exchange 1개로 보통 매핑된다. */

	FCNVME_BE32 parameter;
	/* [한국어] Word5 - parameter 필드. R_CTL/F_CTL 에 따라 의미가 달라진다(예: relative offset for class 3
	 *  data). DATA_OUT 프레임에서 데이터 페이로드의 시작 offset 을 운반. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_frame_hdr) == 24, "size_mismatch");
/* [한국어] FC frame header 가 정확히 24 byte 임을 강제(스펙 고정). */

/*
 * Request payload word 0
 */
struct spdk_nvmf_fc_ls_rqst_w0 {
	/* [한국어] LS 요청 페이로드 word0 (4 byte). 모든 LS 명령 페이로드의 맨 앞 4 byte 는 이 형식.
	 *  설정자: 호스트(요청) 또는 target(ACC/RJT 응답). 읽는 자: 수신측의 LS dispatch 핸들러. */
	uint8_t	ls_cmd;			/* FCNVME_LS_xxx */
	/* [한국어] LS 명령 코드(FCNVME_LS_RJT/ACC/CREATE_ASSOCIATION/...). 수신측이 dispatch 키로 사용. */

	uint8_t zeros[3];
	/* [한국어] 3 byte 예약 - 항상 0 으로 송신. word0 을 32-bit 정렬에 맞추기 위한 padding. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_rqst_w0) == 4, "size_mismatch");

/*
 * LS request information descriptor
 */
struct spdk_nvmf_fc_lsdesc_rqst {
	/* [한국어] LS 요청 정보 descriptor (16 byte) - ACC/RJT 응답에 echo 되어 어떤 LS 요청에 대한 응답인지
	 *  식별하게 한다. */
	FCNVME_BE32 desc_tag;		/* FCNVME_LSDESC_xxx */
	/* [한국어] descriptor tag - FCNVME_LSDESC_RQST(0x1). TLV 구조의 type 부. */

	FCNVME_BE32 desc_len;
	/* [한국어] descriptor body 길이(byte) - 8 (이 구조체 16B 에서 tag/len 8B 제외). */

	struct spdk_nvmf_fc_ls_rqst_w0 w0;
	/* [한국어] 원래 LS 요청의 word0 echo - 호스트가 보낸 ls_cmd 를 그대로 반사. */

	FCNVME_BE32 rsvd12;
	/* [한국어] 예약 - 16B 정렬 padding. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_lsdesc_rqst) == 16, "size_mismatch");

/*
 * LS accept header
 */
struct spdk_nvmf_fc_ls_acc_hdr {
	/* [한국어] LS Accept(ACC) 응답의 공통 헤더 (24 byte). 모든 ACC 응답이 이 헤더로 시작하고, 뒤에
	 *  명령 종류별 추가 descriptor 가 이어진다. */
	struct spdk_nvmf_fc_ls_rqst_w0 w0;
	/* [한국어] word0 - ls_cmd 가 FCNVME_LS_ACC(2). */

	FCNVME_BE32 desc_list_len;
	/* [한국어] 본 헤더 뒤에 이어지는 descriptor 리스트의 총 길이(byte). 수신측은 이 값으로 페이로드 끝을
	 *  판단. */

	struct spdk_nvmf_fc_lsdesc_rqst rqst;
	/* [한국어] 원래 요청의 echo descriptor - 호스트가 어떤 LS 에 대한 응답인지 매칭하는 데 사용. */
	/* Followed by cmd-specific ACC descriptors, see next definitions */
	/* [한국어] 이 헤더 뒤에 ASSOC_ID/CONN_ID 같은 명령 종류별 추가 descriptor 가 따른다(C 의 FAM 패턴). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_acc_hdr) == 24, "size_mismatch");

/*
 * LS descriptor connection id
 */
struct spdk_nvmf_fc_lsdesc_conn_id {
	/* [한국어] Connection ID descriptor (16 byte) - target 이 발급한 64-bit connection_id 를 호스트에
	 *  알려준다. 호스트는 이 ID 를 후속 IU 의 conn_id 필드에 채워 어느 큐의 명령인지 식별하게 한다. */
	FCNVME_BE32 desc_tag;
	/* [한국어] FCNVME_LSDESC_CONN_ID(0x6). */

	FCNVME_BE32 desc_len;
	/* [한국어] body 길이 8 byte. */

	FCNVME_BE64 connection_id;
	/* [한국어] target 이 발급하는 64-bit connection ID. NVMe queue pair 1개에 1:1 대응. 보통 상위
	 *  비트는 association 식별, 하위 비트는 qid 로 분할해 인코딩한다(구현 자유). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_lsdesc_conn_id) == 16, "size_mismatch");

/*
 * LS descriptor association id
 */
struct spdk_nvmf_fc_lsdesc_assoc_id {
	/* [한국어] Association ID descriptor (16 byte) - 1 NVMe controller = 1 association 단위로 발급되는
	 *  64-bit ID. Disconnect/Create Connection 시 대상 association 식별에 사용. */
	FCNVME_BE32 desc_tag;
	/* [한국어] FCNVME_LSDESC_ASSOC_ID(0x7). */

	FCNVME_BE32 desc_len;
	/* [한국어] body 길이 8 byte. */

	FCNVME_BE64 association_id;
	/* [한국어] 64-bit association ID - target 이 Create Association ACC 에서 발급. host 는 이후 모든
	 *  Create Connection / Disconnect 에서 이 값을 사용해 어느 association 에 속한 작업인지 식별. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id) == 16, "size_mismatch");

/*
 * LS Create Association descriptor
 */
struct spdk_nvmf_fc_lsdesc_cr_assoc_cmd {
	/* [한국어] Create Association 명령 본체 descriptor (1016 byte).
	 *  호스트가 NVMe 컨트롤러를 새로 attach 할 때 host NQN/subsys NQN/host UUID 와 admin queue 파라미터를
	 *  싣는다. 1016B 중 대부분은 NQN 문자열(SPDK_NVME_NQN_FIELD_SIZE) 영역. */
	FCNVME_BE32  desc_tag;
	/* [한국어] FCNVME_LSDESC_CREATE_ASSOC_CMD(0x3). */

	FCNVME_BE32  desc_len;
	/* [한국어] body 길이(byte). */

	FCNVME_BE16  ersp_ratio;
	/* [한국어] Extended Response 비율 - N개 명령마다 1번 ERSP 를 강제(throttle). 0 이면 매 명령마다 ERSP. */

	FCNVME_BE16  rsvd10;
	/* [한국어] 예약. */

	FCNVME_BE32  rsvd12[9];
	/* [한국어] 9 word(36 byte) 예약 - 향후 확장 영역. */

	FCNVME_BE16  cntlid;
	/* [한국어] 호스트가 요청하는 NVMe controller ID. 0xFFFF 이면 동적 할당 요청, 그 외는 정적 매핑. */

	FCNVME_BE16  sqsize;
	/* [한국어] admin Submission Queue 크기(엔트리 수). MQES 이하여야 한다. */

	FCNVME_BE32  rsvd52;
	/* [한국어] 예약. */

	uint8_t hostid[FCNVME_ASSOC_HOSTID_LEN];
	/* [한국어] 16-byte host identifier (UUID 형태). NVMe Host Identifier feature 와 동일 의미로,
	 *  reservation/persistent reservation 등에 사용된다. */

	uint8_t hostnqn[SPDK_NVME_NQN_FIELD_SIZE];
	/* [한국어] Host NVMe Qualified Name 문자열. SPDK_NVME_NQN_FIELD_SIZE 바이트 고정 폭이며 ASCII null
	 *  종료. ACL 매칭에 사용된다. */

	uint8_t subnqn[SPDK_NVME_NQN_FIELD_SIZE];
	/* [한국어] Subsystem NQN 문자열 - 호스트가 attach 하려는 NVMe subsystem 의 식별자. target 의
	 *  subsystem 등록 테이블과 매칭. */

	uint8_t rsvd584[432];
	/* [한국어] 432 byte 예약 - descriptor 전체 1016 byte 정렬을 맞추기 위한 trailing reserved 영역. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_lsdesc_cr_assoc_cmd) == 1016, "size_mismatch");

/*
 * LS Create Association request payload
 */
struct spdk_nvmf_fc_ls_cr_assoc_rqst {
	/* [한국어] Create Association LS 요청 전체 페이로드 (1024 byte). w0(4) + desc_list_len(4) +
	 *  assoc_cmd(1016) = 1024. R_CTL=LS_REQUEST 프레임의 페이로드로 송신된다. */
	struct spdk_nvmf_fc_ls_rqst_w0 w0;
	/* [한국어] word0 - ls_cmd = FCNVME_LS_CREATE_ASSOCIATION(3). */

	FCNVME_BE32 desc_list_len;
	/* [한국어] 뒤따르는 descriptor 의 총 길이(byte). 호스트가 송신 시 명시적으로 채운다. */

	struct spdk_nvmf_fc_lsdesc_cr_assoc_cmd assoc_cmd;
	/* [한국어] Create Association 명령 본체 descriptor (host NQN, subsys NQN, hostid, sqsize, cntlid 등). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_cr_assoc_rqst) == 1024, "size_mismatch");

/*
 * LS Create Association accept payload
 */
struct spdk_nvmf_fc_ls_cr_assoc_acc {
	/* [한국어] Create Association ACC 응답 페이로드 (56 byte). 성공적으로 association 이 만들어졌을 때
	 *  target 이 호스트에 association_id 와 admin queue 의 connection_id 를 함께 발급해 돌려준다. */
	struct spdk_nvmf_fc_ls_acc_hdr hdr;
	/* [한국어] 24 byte 공통 ACC 헤더 (w0=ACC + desc_list_len + 원래 RQST descriptor echo). */

	struct spdk_nvmf_fc_lsdesc_assoc_id assoc_id;
	/* [한국어] 16 byte association_id descriptor. */

	struct spdk_nvmf_fc_lsdesc_conn_id conn_id;
	/* [한국어] 16 byte admin queue 의 connection_id descriptor. NVMe admin queue 는 association 생성과
	 *  동시에 만들어지므로 별도 Create Connection LS 가 불필요. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_cr_assoc_acc) == 56, "size_mismatch");

/*
 * LS Create IO Connection descriptor
 */
struct spdk_nvmf_fc_lsdesc_cr_conn_cmd {
	/* [한국어] Create IO Connection 명령 본체 descriptor (56 byte) - admin queue 외 추가 IO queue 1개를
	 *  생성한다. host 는 association_id 와 함께 이 descriptor 를 보낸다. */
	FCNVME_BE32 desc_tag;
	/* [한국어] FCNVME_LSDESC_CREATE_CONN_CMD(0x4). */

	FCNVME_BE32 desc_len;
	/* [한국어] body 길이. */

	FCNVME_BE16 ersp_ratio;
	/* [한국어] 이 IO 큐에 대한 ERSP 비율. */

	FCNVME_BE16 rsvd10;
	/* [한국어] 예약. */

	FCNVME_BE32 rsvd12[9];
	/* [한국어] 9 word 예약 - association descriptor 와 동일 레이아웃 정렬. */

	FCNVME_BE16 qid;
	/* [한국어] NVMe queue ID (1..n). 0 은 admin 이라 여기 등장하지 않는다. */

	FCNVME_BE16 sqsize;
	/* [한국어] 이 IO 큐의 SQ 크기(엔트리 수). 컨트롤러 MQES 이하. */

	FCNVME_BE32 rsvd52;
	/* [한국어] 예약. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_cr_assoc_acc) == 56, "size_mismatch");
/* [한국어] 위 STATIC_ASSERT 는 원본 그대로 유지 - cr_assoc_acc(56B) 검증을 다시 한 번 수행
 *  (스펙 사이즈 sanity check). 코드 수정 금지 원칙에 따라 그대로 둔다. */

/*
 * LS Create IO Connection payload
 */
struct spdk_nvmf_fc_ls_cr_conn_rqst {
	/* [한국어] Create IO Connection LS 요청 전체 페이로드 (80 byte).
	 *  w0(4) + desc_list_len(4) + assoc_id(16) + connect_cmd(56) = 80. */
	struct spdk_nvmf_fc_ls_rqst_w0 w0;
	/* [한국어] word0 - ls_cmd = FCNVME_LS_CREATE_CONNECTION(4). */

	FCNVME_BE32 desc_list_len;
	/* [한국어] 뒤따르는 descriptor 길이 합. */

	struct spdk_nvmf_fc_lsdesc_assoc_id assoc_id;
	/* [한국어] 어느 association 에 큐를 추가할 것인지를 식별하는 association_id descriptor. */

	struct spdk_nvmf_fc_lsdesc_cr_conn_cmd connect_cmd;
	/* [한국어] qid/sqsize 등 IO 큐 파라미터 descriptor. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_cr_conn_rqst) == 80, "size_mismatch");

/*
 * LS Create IO Connection accept payload
 */
struct spdk_nvmf_fc_ls_cr_conn_acc {
	/* [한국어] Create IO Connection ACC 응답 페이로드 (40 byte). 새 IO 큐의 connection_id 만 추가로 반환. */
	struct spdk_nvmf_fc_ls_acc_hdr hdr;
	/* [한국어] 24 byte 공통 ACC 헤더. */

	struct spdk_nvmf_fc_lsdesc_conn_id conn_id;
	/* [한국어] 새로 만든 IO 큐의 16 byte connection_id descriptor. 호스트는 이 값을 후속 IU 의 conn_id 에
	 *  채워 사용한다. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_cr_conn_acc) == 40, "size_mismatch");

/*
 * LS Disconnect descriptor
 */
struct spdk_nvmf_fc_lsdesc_disconn_cmd {
	/* [한국어] Disconnect 명령 본체 descriptor (24 byte). reserved 워드들로 구성되어 있고, 실제 대상
	 *  식별은 동반된 assoc_id descriptor 가 담당한다(rev 1.x 단순화). */
	FCNVME_BE32 desc_tag;
	/* [한국어] FCNVME_LSDESC_DISCONN_CMD(0x5). */

	FCNVME_BE32 desc_len;
	/* [한국어] body 길이. */

	FCNVME_BE32 rsvd8;
	/* [한국어] 예약 word - scope 비트 등 향후 확장용. */

	FCNVME_BE32 rsvd12;
	/* [한국어] 예약 word. */

	FCNVME_BE32 rsvd16;
	/* [한국어] 예약 word. */

	FCNVME_BE32 rsvd20;
	/* [한국어] 예약 word - 24 byte 정렬을 맞춤. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_lsdesc_disconn_cmd) == 24, "size_mismatch");

/*
 * LS Disconnect payload
 */
struct spdk_nvmf_fc_ls_disconnect_rqst {
	/* [한국어] Disconnect LS 요청 전체 페이로드 (48 byte). w0(4) + desc_list_len(4) + assoc_id(16) +
	 *  disconn_cmd(24) = 48.
	 *  단일 connection 만 끊을지, association 전체를 끊을지는 SPDK 구현에서는 conn_id 동반 여부와
	 *  컨텍스트로 판단(rev 1.0 한정 단순화). */
	struct spdk_nvmf_fc_ls_rqst_w0 w0;
	/* [한국어] word0 - ls_cmd = FCNVME_LS_DISCONNECT(5). */

	FCNVME_BE32 desc_list_len;
	/* [한국어] 뒤따르는 descriptor 총 길이. */

	struct spdk_nvmf_fc_lsdesc_assoc_id assoc_id;
	/* [한국어] 끊을 association 식별 descriptor. */

	struct spdk_nvmf_fc_lsdesc_disconn_cmd disconn_cmd;
	/* [한국어] disconnect 명령 본체 (대부분 reserved). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst) == 48, "size_mismatch");

/*
 * LS Disconnect accept payload
 */
struct spdk_nvmf_fc_ls_disconnect_acc {
	/* [한국어] Disconnect ACC 응답 페이로드 (24 byte). 단순히 공통 ACC 헤더만 반환하면 충분하다 - 추가
	 *  descriptor 가 없다. */
	struct spdk_nvmf_fc_ls_acc_hdr hdr;
	/* [한국어] 24 byte 공통 ACC 헤더 - 원래 disconnect 요청을 echo 하면서 성공을 알린다. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_disconnect_acc) == 24, "size_mismatch");

/*
 * LS Reject descriptor
 */
struct spdk_nvmf_fc_lsdesc_rjt {
	/* [한국어] LS Reject descriptor (16 byte). 거절 사유 분류(reason_code) + 상세 설명(reason_explanation)
	 *  + vendor 코드를 운반.
	 *  설정자: target. 읽는 자: 호스트가 retry/abort 정책을 결정하는 데 사용. */
	FCNVME_BE32 desc_tag;
	/* [한국어] FCNVME_LSDESC_RJT(0x2). */

	FCNVME_BE32 desc_len;
	/* [한국어] body 길이 8 byte. */

	uint8_t rsvd8;
	/* [한국어] 예약 - 0 으로 송신. */

	uint8_t reason_code;
	/* [한국어] enum fcnvme_ls_rjt_reason 값. 거절 분류(예: INV_ASSOC, UNSUP, INV_PARAM). */

	uint8_t reason_explanation;
	/* [한국어] enum fcnvme_ls_rjt_explan 값. 거절 상세 사유(예: INV_HOSTNQN, INV_LEN). */

	uint8_t vendor;
	/* [한국어] vendor 고유 추가 코드. reason_code=VENDOR(0xff) 일 때만 의미 있고 그 외에는 0. */

	FCNVME_BE32 rsvd12;
	/* [한국어] 예약 - 16 byte 정렬 padding. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_lsdesc_rjt) == 16, "size_mismatch");

/*
 * LS Reject payload
 */
struct spdk_nvmf_fc_ls_rjt {
	/* [한국어] LS Reject 응답 전체 페이로드 (40 byte). w0(4) + desc_list_len(4) + rqst echo(16) + rjt(16)
	 *  = 40. R_CTL=LS_RESPONSE 프레임으로 송신. */
	struct spdk_nvmf_fc_ls_rqst_w0 w0;
	/* [한국어] word0 - ls_cmd = FCNVME_LS_RJT(1). */

	FCNVME_BE32 desc_list_len;
	/* [한국어] 뒤따르는 descriptor 총 길이. */

	struct spdk_nvmf_fc_lsdesc_rqst rqst;
	/* [한국어] 어떤 LS 요청을 거절했는지 식별하는 echo descriptor. 호스트는 이걸 보고 어느 요청이 실패
	 *  했는지 매칭. */

	struct spdk_nvmf_fc_lsdesc_rjt rjt;
	/* [한국어] 실제 거절 사유 descriptor (reason_code/reason_explanation/vendor). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_rjt) == 40, "size_mismatch");

/*
 * FC World Wide Name
 */
struct spdk_nvmf_fc_wwn {
	/* [한국어] FC World Wide Name (8 byte). N_Port WWPN 또는 Node WWNN 같은 글로벌 고유 식별자.
	 *  union 으로 64-bit 정수 또는 8-byte 배열 두 형태로 접근 가능 - SAN 관리도구는 보통 콜론 구분 hex 로
	 *  표시(예: 21:00:00:e0:8b:11:22:33). */
	union {
		uint64_t wwn; /* World Wide Names consist of eight bytes */
		/* [한국어] 64-bit 정수로 보는 형태. 비교/해시 연산에 편리. */

		uint8_t octets[sizeof(uint64_t)];
		/* [한국어] 8 byte 배열로 보는 형태. 텍스트 출력(콜론 구분) 또는 바이트 단위 비교에 사용. */
	} u;
	/* [한국어] anonymous 이 아닌 명명된 union - 코드에서 wwn.u.wwn / wwn.u.octets 로 접근. */
};

#ifdef __cplusplus
}
/* [한국어] extern "C" 닫기 - C++ 호환을 위한 링크 규약 가드 종료. */
#endif

#endif
/* [한국어] 헤더 가드 종료 (__NVMF_FC_SPEC_H__). */
