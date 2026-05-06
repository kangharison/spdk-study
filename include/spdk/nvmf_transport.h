/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2025, Oracle and/or its affiliates.
 */

/** \file
 * NVMe-oF Target transport plugin API
 */

/*
 * [한국어 설명] NVMe-oF Target transport 플러그인 작성자용 API (nvmf_transport.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK NVMe-over-Fabrics target 의 "트랜스포트 플러그인 인터페이스" 를 정의한다.
 * RDMA / TCP / FC / vfio_user 등 실제 와이어 프로토콜 모듈은 본 헤더의 vtable
 * (spdk_nvmf_transport_ops) 를 구현하고, SPDK_NVMF_TRANSPORT_REGISTER 매크로로 자기 자신을
 * 코어에 등록한다. 또 본 헤더는 nvmf 코어와 transport 사이에서 공유되는 1급 구조체
 * (spdk_nvmf_request, spdk_nvmf_qpair, spdk_nvmf_transport_poll_group, spdk_nvmf_poll_group,
 * spdk_nvmf_listener, spdk_nvmf_registers, spdk_nvmf_ctrlr_data) 를 노출하여, transport
 * 모듈이 capsule(SQE) / response(CQE) / sgl / iobuf / poll group / qpair 상태를 직접 다룰
 * 수 있게 한다. 마지막으로 controller migration(VM 이주) 데이터 구조와 AER, zero-copy 단계
 * (zcopy phase) 도 함께 정의된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호스트 → wire(RDMA/TCP/FC) → transport plugin(lib/nvmf/rdma.c, tcp.c, fc.c, vfio_user.c) →
 * 본 헤더의 vtable hook(poll_group_poll, req_complete, qpair_*) → nvmf 코어
 * (lib/nvmf/{ctrlr,subsystem,nvmf}.c) → bdev → backing 디바이스. 본 헤더는 정확히 그
 * "transport↔core" 경계에 놓여 있다. 호출 컨텍스트는 transport poll_group 마다 1 개의
 * spdk_thread(reactor) 이며, 그 안에서 무한 polling 루프가 poll_group_poll 을 돌린다.
 * spdk_nvmf_request_exec / complete / free 는 nvmf 코어의 일반 API 이지만, 이들 역시 항상
 * qpair 가 바인딩된 동일 spdk_thread 에서만 호출되어야 한다(thread affinity).
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/nvmf.h(공개 target API), spdk/nvmf_cmd.h(custom admin), spdk/nvmf_spec.h
 *   (NVMe-oF wire spec, capsule/connect/property), spdk/nvme_spec.h(NVMe SQE/CQE/SGL),
 *   spdk/bdev.h(backing block), spdk/thread.h(spdk_poller/poll group thread), spdk/memory.h
 *   (VALUE_4KB), spdk/trace.h(tpoint id).
 * - 사용처: lib/nvmf/{rdma,tcp,fc,vfio_user}.c 가 spdk_nvmf_transport_ops 를 구현. nvmf
 *   코어(lib/nvmf/ctrlr.c, transport.c, nvmf.c) 는 등록된 vtable 을 통해 fan-out.
 * - 데이터 흐름: 호스트 capsule(NVMe-oF 64B SQE) → transport poll_group_poll → 자기 큐에서
 *   spdk_nvmf_request 객체 채우기 → spdk_nvmf_request_exec → nvmf 코어가 admin/IO 디스패치
 *   → bdev 처리 → 코어가 transport 의 req_complete 호출 → transport 가 wire 로 CQE 회신.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_nvmf_transport_ops: 트랜스포트 vtable. name/type/create/destroy/listen/poll_group_*
 *   /req_complete/req_free/qpair_* 등 30 여 개 콜백.
 * - SPDK_NVMF_TRANSPORT_REGISTER(name, ops): 컴파일러 constructor 로 vtable 자동 등록.
 * - spdk_nvmf_request: capsule + response + iov + dif + zcopy 정보를 포함한 1 급 요청 객체.
 * - spdk_nvmf_qpair: admin/IO qpair — state, sq_head, ctrlr, transport, poll group 백포인터.
 * - spdk_nvmf_poll_group: per-thread polling group. 여러 transport poll group + 여러 qpair
 *   를 fan-in.
 * - spdk_nvmf_transport_poll_group: 한 transport 가 한 poll_group 안에서 가지는 sub-group.
 * - spdk_nvmf_request_exec/complete/free: 코어가 제공하는 요청 lifecycle API.
 * - spdk_nvmf_request_zcopy_start/end: zero-copy 경로의 명시적 시작/종료.
 * - spdk_nvmf_ctrlr_save/restore_migr_data: NVMe-oF controller 상태 저장/복원(VM live migration).
 * - spdk_nvmf_req_get_xfer: SQE opcode + SGL 로 데이터 전송 방향(host→ctrlr/ctrlr→host/none)
 *   을 결정하는 inline.
 */

#ifndef SPDK_NVMF_TRANSPORT_H_
#define SPDK_NVMF_TRANSPORT_H_

#include "spdk/bdev.h"
/* [한국어] backing 블록 디바이스 추상화 — req 가 backing bdev 의 io channel 위에서
 * 실행되며, bdev_io_wait_entry 를 본 파일의 spdk_nvmf_request 에 임베드한다. */

#include "spdk/thread.h"
/* [한국어] spdk_poller / spdk_thread / iobuf channel — poll_group 이 spdk_thread 위에서
 * 동작하고, 트랜스포트마다 spdk_poller 를 1 개 가진다. */

#include "spdk/nvme_spec.h"
/* [한국어] NVMe 1.x/2.x wire 스펙(SQE/CQE/SGL/Identify/Feature ID 등). 본 헤더의 cmd/rsp
 * union 이 이들 자료구조를 그대로 임베드한다. */

#include "spdk/nvmf.h"
/* [한국어] target 1차 공개 API. spdk_nvmf_tgt / subsystem / ctrlr / ns 의 전방선언. */

#include "spdk/nvmf_cmd.h"
/* [한국어] custom admin 핸들러 헤더 — spdk_nvmf_nvme_passthru_cmd_cb 시그니처를 본 파일의
 * spdk_nvmf_request.cmd_cb_fn 에서 사용. */

#include "spdk/nvmf_spec.h"
/* [한국어] NVMe-oF wire 스펙 — fabric capsule, connect/property/auth 명령, discovery log
 * page entry 정의. spdk_nvmf_capsule_cmd / fabric_*_cmd 등을 union 에 사용. */

#include "spdk/memory.h"
/* [한국어] VALUE_4KB 매크로 — 데이터 버퍼 정렬값(4096B) 에 사용. */

#include "spdk/trace.h"
/* [한국어] SPDK 트레이스 프레임워크 — qpair 의 trace_id 필드에 사용. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 사용자가 본 헤더를 include 할 때 C linkage 강제. */
#endif

#define SPDK_NVMF_MAX_SGL_ENTRIES	16
/* [한국어] 한 NVMe-oF 요청이 가질 수 있는 SGL(Scatter-Gather List) 엔트리 수의 상한.
 * NVMe-oF 호스트가 capsule 의 SGL 을 통해 보낼 수 있는 최대 분산-모음 단위 수이며,
 * RDMA Memory Region 등록과 TCP iovec 길이 산정에 사용된다. */

/* The maximum number of buffers per request */
#define NVMF_REQ_MAX_BUFFERS	(SPDK_NVMF_MAX_SGL_ENTRIES * 2 + 1)
/* [한국어] 한 요청 객체에 임베드되는 iov 배열 길이. 기본 SGL 16 + DIF 분리 시 2 배수 +
 * 메타데이터 1 칸 의 여유를 주기 위해 *2+1. spdk_nvmf_request.iov 와
 * spdk_nvmf_stripped_data.iov 가 이 길이를 사용. */

/* Maximum pending AERs that can be migrated */
#define SPDK_NVMF_MIGR_MAX_PENDING_AERS 256
/* [한국어] live migration 시 옮길 수 있는 보류 AER(Asynchronous Event Request) 의 상한.
 * spdk_nvmf_ctrlr_migr_data.async_events 배열 크기에 사용. AER 은 controller 가 host 에게
 * 비동기적으로 이벤트를 알리는 NVMe 메커니즘. */

#define SPDK_NVMF_MAX_ASYNC_EVENTS 4
/* [한국어] 한 controller 가 동시 보유 가능한 host 측 AER 슬롯 수. NVMe Identify 의
 * AERL+1 과 일치하도록 4 로 고정. migration data 의 aer_cids 배열 크기. */

/* Some backends require 4K aligned buffers. The iobuf library gives us that
 * naturally, but there are buffers allocated other ways that need to use this.
 */
#define NVMF_DATA_BUFFER_ALIGNMENT	VALUE_4KB
/* [한국어] 데이터 버퍼 정렬값 = 4096B. NVMe DMA(PRP) 의 페이지 정렬, RDMA MR 등록의 정렬
 * 요구를 만족시키기 위함. iobuf 라이브러리는 자동으로 만족하지만, 다른 경로(스택 임시 등)
 * 에서 할당된 버퍼는 이 매크로로 정렬을 검사. */

#define NVMF_DATA_BUFFER_MASK		(NVMF_DATA_BUFFER_ALIGNMENT - 1LL)
/* [한국어] 정렬 검사용 마스크(0xFFF). 주소 & MASK 가 0 이 아니면 미정렬. 1LL 로 long-long
 * 캐스팅하여 64-bit 주소에서도 안전. */

#define SPDK_NVMF_DEFAULT_ACCEPT_POLL_RATE_US 10000
/* [한국어] 새 connection accept 폴러의 기본 주기(10000us = 10ms). 트랜스포트가 listen
 * 소켓에서 accept 를 시도하는 빈도. polled-mode 라도 connection 수립은 빈도가 낮으므로
 * 짧은 sleep 간격으로 CPU 부하를 줄임. */

/*
 * [한국어]
 * union nvmf_h2c_msg - host→controller 방향 capsule(SQE 64B) 의 통합 뷰.
 *
 * 와이어 상으로는 모두 64B 크기지만 의미가 명령 종류에 따라 다르다. 이 union 은 동일한
 * 64B 메모리 영역을 (a) NVMe-oF capsule cmd, (b) NVMe SQE, (c) fabric property set/get,
 * (d) fabric connect, (e) fabric AUTH send/recv 로 캐스팅 없이 접근하게 해준다. 즉
 * SQE 의 opcode/fctype 으로 어느 멤버를 읽을지를 결정.
 */
union nvmf_h2c_msg {
	struct spdk_nvmf_capsule_cmd			nvmf_cmd;
	/* [한국어] generic NVMe-oF capsule 헤더. fctype 등 fabric-specific 분기에 우선 사용.
	 * 설정자: 호스트가 wire 로 보내, transport 가 본 union 으로 디코드.
	 * 읽는 자: nvmf 코어가 opcode==FABRIC 인 경우 fctype 으로 분기 결정.
	 * 동기화: 요청 객체는 단일 spdk_thread 내에서만 다뤄지므로 락 불필요. */

	struct spdk_nvme_cmd				nvme_cmd;
	/* [한국어] 일반 NVMe SQE — admin/IO 명령(read/write/identify/get_log_page/...).
	 * 설정자: 호스트가 보낸 capsule 의 64B 데이터 그대로.
	 * 읽는 자: nvmf 코어가 opcode 별 디스패처에서 사용. cmd->cdw* 가 표준 NVMe 시멘틱.
	 * 값 범위: NVMe spec 의 모든 SQE field. */

	struct spdk_nvmf_fabric_prop_set_cmd		prop_set_cmd;
	/* [한국어] Fabric Property Set capsule(fctype=0x00). NVMe-oF 의 BAR 레지스터 등가
	 * (CC, CSTS, AQA 등) 를 호스트가 set 할 때 사용. nvmf_ctrlr_property_set() 가 처리.
	 * 동기화: 컨트롤러 상태 변경이므로 같은 admin qpair 에서 직렬 처리. */

	struct spdk_nvmf_fabric_prop_get_cmd		prop_get_cmd;
	/* [한국어] Fabric Property Get capsule(fctype=0x04). 호스트가 컨트롤러 레지스터를 읽음.
	 * 응답은 nvmf_c2h_msg.prop_get_rsp 로 채움. */

	struct spdk_nvmf_fabric_connect_cmd		connect_cmd;
	/* [한국어] Fabric Connect capsule(fctype=0x01). 호스트가 admin/IO qpair 를 controller
	 * 에 연결하려는 첫 명령. host NQN, host_id, qid 가 들어 있다.
	 * 읽는 자: nvmf_ctrlr_cmd_connect() — host 인증/할당 후 spdk_nvmf_ctrlr 매핑. */

	struct spdk_nvmf_fabric_auth_send_cmd		auth_send_cmd;
	/* [한국어] Fabric Authentication Send(fctype=0x05). DH-HMAC-CHAP 등 인증 단계의
	 * "host→target" 메시지를 capsule 로 캡슐화. spdk_nvmf_qpair.state 가
	 * AUTHENTICATING 일 때 들어온다. */

	struct spdk_nvmf_fabric_auth_recv_cmd		auth_recv_cmd;
	/* [한국어] Fabric Authentication Receive(fctype=0x06). 인증 challenge 등을
	 * "target→host" 로 응답받기 위한 트리거 capsule. */
};
SPDK_STATIC_ASSERT(sizeof(union nvmf_h2c_msg) == 64, "Incorrect size");
/* [한국어] 컴파일 타임 검증 — union 이 정확히 NVMe SQE 표준 크기 64B 여야 한다(아니면
 * wire 호환성 깨짐). */

/*
 * [한국어]
 * union nvmf_c2h_msg - controller→host 방향 응답(CQE 16B) 의 통합 뷰.
 *
 * 일반 NVMe CQE 는 16B. fabric connect 응답은 16B 안에 controller id 등을 추가로 채운다.
 * fabric prop_get 응답도 16B 안에 4B/8B value 를 임베드.
 */
union nvmf_c2h_msg {
	struct spdk_nvme_cpl				nvme_cpl;
	/* [한국어] 일반 NVMe CQE — SC/SCT/cdw0/cid/sq_head 등.
	 * 설정자: nvmf 코어 / custom 핸들러 / bdev 완료 콜백.
	 * 읽는 자: 트랜스포트의 req_complete 가 wire 로 회신. */

	struct spdk_nvmf_fabric_prop_get_rsp		prop_get_rsp;
	/* [한국어] Property Get 응답 — 16B CQE 의 cdw0/1 영역에 register value(8B) 가 임베드. */

	struct spdk_nvmf_fabric_connect_rsp		connect_rsp;
	/* [한국어] Connect 응답 — controller id(cntlid), authreq, status 등 connect 결과. */
};
SPDK_STATIC_ASSERT(sizeof(union nvmf_c2h_msg) == 16, "Incorrect size");
/* [한국어] 컴파일 타임 검증 — NVMe CQE 표준 크기 16B 와 동일해야 한다. */

/*
 * [한국어]
 * struct spdk_nvmf_dif_info - NVMe Protection Information (DIF) 처리용 컨텍스트.
 *
 * NVMe end-to-end 데이터 보호(DIF/DIX)를 사용할 때, 호스트 와이어 페이로드는 LBA+meta 가
 * 인터리브된 "확장 LBA(eLBA)" 인 반면 backing bdev 가 받는 페이로드는 LBA 만이거나
 * stripped 형태일 수 있다. 본 구조체는 그 변환을 위한 ctx + 길이 메타를 보유.
 */
struct spdk_nvmf_dif_info {
	struct spdk_dif_ctx			dif_ctx;
	/* [한국어] DIF 변환 컨텍스트(LBA size, meta size, guard/apptag 정책 등).
	 * 설정자: nvmf 코어가 cmd 디코드 시 ns 의 DPS/PI 설정으로 초기화.
	 * 읽는 자: spdk_dif_generate / spdk_dif_verify 가 사용.
	 * 동기화: 요청 단위로 별개이므로 락 불필요. */

	uint32_t				elba_length;
	/* [한국어] 호스트 와이어 상의 확장 LBA 페이로드 길이(LBA 크기에 메타 인터리브 포함).
	 * 설정자: nvmf 코어가 (block_count * (block_size + md_size)) 로 산정.
	 * 읽는 자: 트랜스포트가 capsule iov 길이를 비교하거나 strip 변환 시 참조. */

	uint32_t				orig_length;
	/* [한국어] 메타 분리 전 원본 length(NVMe length field). req->length 와 비교/복원에 사용.
	 * 설정자: nvmf 코어 cmd 디코드.
	 * 읽는 자: strip/insert 변환 후 길이를 복원하는 코드 경로. */
};

/*
 * [한국어]
 * struct spdk_nvmf_stripped_data - DIF 변환 결과(메타 제거된) 페이로드를 보관하는 별도 iov 셋.
 *
 * 일부 트랜스포트(RDMA/TCP) 는 호스트와 송수신 시 "확장 LBA + 메타" 를 다루지만, backing
 * bdev 는 메타가 분리된 표현을 선호. 그래서 메타를 strip 한 별도 iov 를 본 구조체에 보관.
 */
struct spdk_nvmf_stripped_data {
	uint32_t			iovcnt;
	/* [한국어] iov 유효 엔트리 수. 0..NVMF_REQ_MAX_BUFFERS.
	 * 설정자: DIF strip 코드.
	 * 읽는 자: bdev I/O submit 시 iovcnt 를 그대로 사용. */

	struct iovec			iov[NVMF_REQ_MAX_BUFFERS];
	/* [한국어] strip 결과 iovec 배열 — base/length 표준 POSIX iovec.
	 * 설정자: DIF strip 코드가 메타 영역을 제외한 base/length 를 채움.
	 * 읽는 자: bdev_io 가 이 iov 로 backing 디바이스에 read/write. */
};

/*
 * [한국어]
 * enum spdk_nvmf_zcopy_phase - zero-copy 처리 단계 머신 상태.
 *
 * SPDK 의 ZCOPY 경로는 (1) 트랜스포트가 "버퍼 줘" → bdev 가 자기 메모리 영역을 노출 →
 * (2) 트랜스포트가 그 영역을 직접 RDMA/TCP 로 송수신 → (3) "끝났어" 로 진행. 단계가 어긋
 * 나면 사용 후 free 가 깨지므로 이 enum 으로 명시적 상태 머신을 둔다.
 */
enum spdk_nvmf_zcopy_phase {
	NVMF_ZCOPY_PHASE_NONE,        /* Request is not using ZCOPY */
	/* [한국어] zero-copy 미사용. 일반(copy) 경로.
	 * 설정자: 요청 생성 시 기본값. 동기화: 요청 단일 thread. */

	NVMF_ZCOPY_PHASE_INIT,        /* Requesting Buffers */
	/* [한국어] bdev 에 ZCOPY 버퍼 요청 보낸 상태. spdk_bdev_zcopy_start 진행 중.
	 * 설정자: spdk_nvmf_request_zcopy_start 호출 직후. */

	NVMF_ZCOPY_PHASE_EXECUTE,     /* Got buffers processing commands */
	/* [한국어] bdev 가 버퍼를 노출했고, 트랜스포트가 그 메모리로 wire 송수신 중.
	 * 설정자: bdev zcopy_start 완료 콜백.
	 * 읽는 자: 트랜스포트가 ZCOPY 데이터 이동 후 zcopy_end 를 호출하는 시점 결정. */

	NVMF_ZCOPY_PHASE_END_PENDING, /* Releasing buffers */
	/* [한국어] zcopy_end 호출하여 bdev 에 commit/abort 통보한 직후, bdev 응답 대기 중. */

	NVMF_ZCOPY_PHASE_COMPLETE,    /* Buffers Released */
	/* [한국어] bdev 가 버퍼 해제를 완료. 이후 요청을 free 해도 안전. */

	NVMF_ZCOPY_PHASE_INIT_FAILED  /* Failed to get the buffers */
	/* [한국어] zcopy_start 가 실패한 상태(예: bdev 미지원 or 자원 부족). 일반 copy 경로로
	 * 폴백되거나 즉시 에러 회신. */
};

/*
 * [한국어]
 * struct spdk_nvmf_request - NVMe-oF 요청 1급 객체 (capsule + response + iov + 메타).
 *
 * 호스트가 보낸 한 NVMe 명령에 대응하는 full lifecycle 컨텍스트. transport 가 capsule 을
 * 디코드해 만들어 nvmf 코어로 전달하고, 코어가 admin/IO 디스패치 후 transport 로 응답을
 * 회신할 때까지 이 객체가 살아 있다. 모든 처리는 qpair 가 바인딩된 단일 spdk_thread
 * 에서 일어나므로 락이 필요 없다(field-level synchronization 불필요).
 */
struct spdk_nvmf_request {
	struct spdk_nvmf_qpair		*qpair;
	/* [한국어] 이 요청이 속한 qpair(SQ/CQ 1쌍).
	 * 설정자: transport 가 새 capsule 을 받아 요청 객체를 만들 때.
	 * 읽는 자: nvmf 코어 전반 — qpair->ctrlr 로 host/subsystem 식별, qpair->group 으로
	 *   poll group 알아냄.
	 * 값 범위: 유효 qpair 포인터(NULL 불가). */

	uint32_t			length;
	/* [한국어] 요청 데이터 길이(byte). NVMe SGL 또는 NVMe-oF in-capsule data 길이의 합.
	 * 설정자: transport 가 capsule 헤더에서 추출.
	 * 읽는 자: bdev I/O submit 시 length 인자, iobuf 할당 크기 결정. */

	uint8_t				xfer; /* type enum spdk_nvme_data_transfer */
	/* [한국어] 데이터 전송 방향 (NONE / HOST_TO_CONTROLLER / CONTROLLER_TO_HOST /
	 * BIDIRECTIONAL). spdk_nvmf_req_get_xfer() 결과가 캐시됨.
	 * 설정자: 코어가 cmd 디코드 후 set.
	 * 읽는 자: iobuf 할당, sgl 처리, zcopy 가능 여부 판단. */

	union {
		uint8_t raw;
		/* [한국어] 비트필드 union 의 raw 8비트. 통째로 read/write 시 사용. */

		struct {
			uint8_t data_from_pool		: 1;
			/* [한국어] 1 이면 데이터 버퍼가 iobuf pool 에서 할당된 것 — req_free 시
			 * spdk_nvmf_request_free_buffers 로 반환 필요.
			 * 설정자: spdk_nvmf_request_get_buffers 성공 시.
			 * 읽는 자: req_free / completion 경로가 free 여부 결정. */

			uint8_t dif_enabled		: 1;
			/* [한국어] DIF/DIX 보호 정보가 활성화된 namespace 의 요청.
			 * 설정자: 코어가 ns 의 PI 설정 보고 set.
			 * 읽는 자: dif strip/insert 코드 경로. */

			uint8_t first_fused		: 1;
			/* [한국어] NVMe Fused Operation 의 첫 명령. 두 번째 fused 와 짝지어 처리.
			 * 설정자: 코어 admin/IO 디스패처가 cmd->fuse 비트 보고 set.
			 * 읽는 자: 다음 명령이 second-fused 인 경우 first_fused_req 와 결합. */

			uint8_t reservation_queued	: 1;
			/* [한국어] NVMe Reservation 충돌로 인해 reservation_queue 에 보류 중.
			 * 설정자: nvmf_ns_reservation 코드. 읽는 자: reservation 해제 시 재실행. */

			uint8_t reservation_waiting	: 1; /* a reservation is waiting on this request */
			/* [한국어] 다른 요청이 본 요청에 reservation lock 걸기 위해 대기 중.
			 * 설정자: reservation 락 획득 코드. 읽는 자: 본 요청 완료 시 wakeup. */

			uint8_t rsvd			: 3;
			/* [한국어] 향후 확장용 예약 비트. 항상 0. */
		};
	};

	uint8_t				zcopy_phase; /* type enum spdk_nvmf_zcopy_phase */
	/* [한국어] zero-copy 단계 상태. enum spdk_nvmf_zcopy_phase 참조.
	 * 설정자: spdk_nvmf_request_zcopy_start/end, bdev zcopy 콜백.
	 * 읽는 자: spdk_nvmf_request_using_zcopy() inline. */

	uint8_t				iovcnt;
	/* [한국어] iov[] 의 유효 엔트리 수. 1..NVMF_REQ_MAX_BUFFERS.
	 * 설정자: 코어가 SGL 디코드 / iobuf 할당 후 set.
	 * 읽는 자: bdev_io submit 시 인자. */

	union nvmf_h2c_msg		*cmd;
	/* [한국어] 호스트가 보낸 SQE 캐슐 64B 의 포인터. transport 가 자기 capsule 메모리
	 * 영역을 가리키게 set.
	 * 설정자: transport — RDMA recv 버퍼 또는 TCP receive 버퍼의 일부. 동기화: thread 내. */

	union nvmf_c2h_msg		*rsp;
	/* [한국어] 응답으로 보낼 CQE 16B 의 포인터. 코어와 핸들러가 채우면 transport 가 wire 로 전송.
	 * 설정자: transport 가 자기 송신 버퍼의 일부를 가리키게 set. */

	STAILQ_ENTRY(spdk_nvmf_request)	buf_link;
	/* [한국어] iobuf 가 부족할 때 pending_buf_queue(transport_poll_group) 에 들어가는 STAILQ
	 * 엔트리. 버퍼 가용 시 큐에서 꺼내 재시도. */

	TAILQ_ENTRY(spdk_nvmf_request)	link;
	/* [한국어] qpair->outstanding 리스트에 연결되는 TAILQ 엔트리. 진행 중인 요청 추적/abort
	 * 시 리스트 탐색용. */

	/* Memory domain which describes payload in this request. If the bdev doesn't support memory
	 * domains, bdev layer will do the necessary push or pull operation. */
	struct spdk_memory_domain	*memory_domain;
	/* [한국어] payload 가 위치한 메모리 도메인(예: GPU memory, RDMA registered memory).
	 * 설정자: transport(주로 RDMA) 가 자기 MR 도메인을 가리키게 set.
	 * 읽는 자: bdev — memory_domain 을 지원하면 zero-copy, 아니면 push/pull 을 자동 수행.
	 * 동기화: 도메인 객체 자체는 SPDK memory subsystem 이 ref-count 로 관리. */

	/* Context to be passed to memory domain operations. */
	void				*memory_domain_ctx;
	/* [한국어] memory_domain 호출 시 콜백에 전달할 opaque context. transport 가 MR 핸들 등
	 * 자기 정보를 넣어 두고, push/pull 시 자기 핸들러가 다시 받음. */

	struct spdk_accel_sequence	*accel_sequence;
	/* [한국어] DSA(Intel Data Streaming Accelerator) 같은 가속기 명령 시퀀스. compress/copy
	 * 등을 bdev 가 가속기에 위임할 때 사용. NULL 이면 가속기 사용 없음. */

	struct iovec			iov[NVMF_REQ_MAX_BUFFERS];
	/* [한국어] 이 요청의 데이터 iov 배열. transport SGL 을 표준 POSIX iovec 로 디코드한 결과.
	 * 설정자: transport 또는 spdk_nvmf_request_get_buffers.
	 * 읽는 자: bdev_io submit, dif strip/insert. */

	struct spdk_nvmf_stripped_data	*stripped_data;
	/* [한국어] DIF strip 결과 별도 iov 셋(있으면). 메타 분리가 필요할 때만 할당.
	 * 설정자: dif strip 코드. 읽는 자: bdev I/O 가 메타 분리된 iov 로 처리. NULL 이면 비활성. */

	struct spdk_nvmf_dif_info	dif;
	/* [한국어] DIF 컨텍스트(in-place). 위 spdk_nvmf_dif_info 참조. */

	struct spdk_bdev_io_wait_entry	bdev_io_wait;
	/* [한국어] bdev 에 io_channel 의 in-flight 슬롯이 부족하여 대기할 때 사용하는 엔트리.
	 * spdk_bdev_queue_io_wait 가 콜백을 등록 → 슬롯 가용 시 wakeup. */

	spdk_nvmf_nvme_passthru_cmd_cb	cmd_cb_fn;
	/* [한국어] passthru admin 응답 직전 호출되는 사용자 후처리 콜백 (nvmf_cmd.h 참조).
	 * 설정자: spdk_nvmf_bdev_ctrlr_nvme_passthru_admin 호출 시. NULL 가능. */

	struct spdk_nvmf_request	*first_fused_req;
	/* [한국어] Fused Op 의 첫 명령(저장본). 두 번째 명령 처리 시 짝을 찾을 때 사용.
	 * 설정자: 코어가 first_fused 비트 set 시 가리킴. */

	struct spdk_nvmf_request	*req_to_abort;
	/* [한국어] 본 요청이 NVMe Abort 인 경우, 취소 대상 요청 포인터.
	 * 설정자: 코어가 Abort 명령 처리 시. 읽는 자: spdk_nvmf_request_get_req_to_abort. */

	struct spdk_poller		*poller;
	/* [한국어] 요청별 보조 poller(timeout 감시 등). 사용 시 set, 종료 시 unregister. */

	struct spdk_bdev_io		*zcopy_bdev_io; /* Contains the bdev_io when using ZCOPY */
	/* [한국어] ZCOPY 진행 중인 bdev_io 핸들. zcopy_end 시 commit/abort 결정에 사용. */

	/* Internal state that keeps track of the iobuf allocation progress */
	struct {
		struct spdk_iobuf_entry	entry;
		/* [한국어] iobuf 큐에 보류 중일 때 사용하는 wait 엔트리. 콜백/우선순위 정보 보유. */

		uint32_t		remaining_length;
		/* [한국어] 요청 데이터 중 아직 iobuf 가 할당되지 않은 잔여 길이. 부분 할당된
		 * 상태에서 추가 할당 시도 시 사용. */
	} iobuf;

	/* Timeout tracked for connect and abort flows. */
	uint64_t timeout_tsc;
	/* [한국어] connect/abort 흐름의 deadline (rdtsc 단위). polling 시 현재 tsc 와 비교해
	 * 만료 검사. */

	uint32_t			orig_nsid;
	/* [한국어] passthru 시 SQE 의 원본 nsid 백업. set_passthru_admin_cmd 가 forward_nsid
	 * 를 강제로 바꾸기 전 원본을 보존하여, 응답 시 호스트에 본래 값으로 복원. */

	STAILQ_ENTRY(spdk_nvmf_request)	reservation_link;
	/* [한국어] reservation 충돌 큐 엔트리. ns 의 reservation_pending_queue 에 연결. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_request) == 832, "Incorrect size");
/* [한국어] 컴파일 타임 검증 — 객체 크기가 832B 인지 확인(ABI 안정성). 변경 시 mempool 크기와
 * cache line alignment 가 영향받으므로 의도적 변경이 아니면 fail. */

/*
 * [한국어]
 * enum spdk_nvmf_qpair_state - qpair 의 lifecycle 상태 머신.
 */
enum spdk_nvmf_qpair_state {
	SPDK_NVMF_QPAIR_UNINITIALIZED = 0,
	/* [한국어] 초기값. transport 가 qpair 객체를 만들었지만 아직 어떤 단계에도 진입하지 않음.
	 * 설정자: 트랜스포트 qpair 할당 직후. 읽는 자: 디버그/검증. */

	SPDK_NVMF_QPAIR_CONNECTING,
	/* [한국어] Fabric Connect capsule 처리 중. host 측 Connect 가 도착했지만 인증/할당이
	 * 미완료. 설정자: nvmf_ctrlr_cmd_connect 진입 시. 읽는 자: dispatcher 가 일반 cmd 를
	 * 거부하고 Connect 만 받도록. */

	SPDK_NVMF_QPAIR_AUTHENTICATING,
	/* [한국어] DH-HMAC-CHAP 등 Fabric Auth Send/Recv 진행 중.
	 * 설정자: Connect 후 인증 필요 시. 읽는 자: dispatcher 가 auth 외 명령 차단. */

	SPDK_NVMF_QPAIR_ENABLED,
	/* [한국어] 정상 동작 — admin/IO 명령 모두 수락. spdk_nvmf_qpair_is_active() == true 의 핵심 상태. */

	SPDK_NVMF_QPAIR_DEACTIVATING,
	/* [한국어] disconnect 진행 중. outstanding I/O 정리 중이며 새 요청 거부.
	 * 설정자: spdk_nvmf_qpair_disconnect 호출 시. */

	SPDK_NVMF_QPAIR_ERROR,
	/* [한국어] 회복 불가능한 transport-level 에러 발생. 강제 종료 대상. */
};

/*
 * [한국어]
 * spdk_nvmf_state_change_done - qpair 상태 전환 완료 콜백 시그니처.
 *
 * 인자: cb_arg — 호출자가 등록 시 넘긴 opaque, status — 0 성공/음수 errno.
 * 비동기 disconnect/auth 완료를 nvmf 코어가 호출자에게 통지할 때 쓰임.
 */
typedef void (*spdk_nvmf_state_change_done)(void *cb_arg, int status);

struct spdk_nvmf_qpair_auth;
/* [한국어] 전방선언 — 인증 상태 머신 컨텍스트. 정의는 lib/nvmf/auth.c 의 internal 헤더에.
 * 본 헤더에서는 포인터로만 사용. */

/*
 * [한국어]
 * struct spdk_nvmf_qpair - admin 또는 IO Queue Pair (SQ+CQ 1 쌍) 객체.
 *
 * 호스트와 controller 사이의 단일 논리 채널. qid==0 이면 admin, 그 외 IO. 모든 필드는
 * 본 qpair 가 바인딩된 spdk_thread 한 곳에서만 다뤄지므로 락 없이 접근.
 */
struct spdk_nvmf_qpair {
	uint8_t					state; /* ref spdk_nvmf_qpair_state */
	/* [한국어] 현재 상태. spdk_nvmf_qpair_state enum 값. 설정자: 코어/transport 의 상태 머신.
	 * 읽는 자: 디스패처가 명령 수락 가부 결정. */

	uint8_t					rsvd;
	/* [한국어] 정렬용 reserved 패딩. 항상 0. */

	uint16_t				qid;
	/* [한국어] Queue Pair ID. 0 = admin, 1.. = IO. NVMe Connect 시 호스트가 지정. */

	uint16_t				sq_head;
	/* [한국어] 현재 SQ head 포인터(논리 위치). 호스트가 다음에 채울 슬롯의 인덱스.
	 * NVMe-oF 는 capsule 마다 sq_head 를 응답에 실어 호스트에 알린다. */

	uint16_t				sq_head_max;
	/* [한국어] SQ 의 최대 깊이. capsule connect 시 host 가 요청한 sqsize+1.
	 * sq_head 는 modulo (sq_head_max+1) 로 순환. */

	struct spdk_nvmf_transport		*transport;
	/* [한국어] 이 qpair 가 속한 transport(RDMA/TCP/FC/...) 인스턴스 백포인터.
	 * 설정자: qpair 생성 시. 읽는 자: req 처리 시 vtable 사용. */

	struct spdk_nvmf_ctrlr			*ctrlr;
	/* [한국어] 이 qpair 가 연결된 NVMe-oF controller. CONNECT 완료 후 set.
	 * 설정자: nvmf_ctrlr_cmd_connect 성공 시. 읽는 자: 모든 admin/IO dispatch. NULL == 미연결. */

	struct spdk_nvmf_poll_group		*group;
	/* [한국어] 이 qpair 를 polling 하는 poll_group 백포인터.
	 * 설정자: spdk_nvmf_poll_group_add. 읽는 자: complete 콜백 실행 thread 결정. */

	union {
		struct spdk_nvmf_request	*first_fused_req;
		/* [한국어] Fused Op 진행 중일 때 첫 명령 저장. ENABLED 상태에서 사용. */

		struct spdk_nvmf_request	*connect_req;
		/* [한국어] CONNECTING 상태에서 진행 중인 Connect 요청. 동시에 사용되지 않으므로 union. */
	};

	TAILQ_HEAD(, spdk_nvmf_request)		outstanding;
	/* [한국어] 현재 진행 중인 모든 요청 리스트. abort 시 탐색, disconnect 시 일괄 정리.
	 * 설정자: 요청 시작 시 TAILQ_INSERT, 완료 시 TAILQ_REMOVE.
	 * 읽는 자: NVMe Abort 핸들러, qpair fini. */

	TAILQ_ENTRY(spdk_nvmf_qpair)		link;
	/* [한국어] poll_group->qpairs 리스트의 연결 엔트리. */

	spdk_nvmf_state_change_done		state_cb;
	/* [한국어] 상태 전환(주로 disconnect/auth) 완료 시 호출할 콜백. NULL 가능.
	 * 설정자: spdk_nvmf_qpair_disconnect 호출자. */

	void					*state_cb_arg;
	/* [한국어] state_cb 에 전달할 opaque. */

	bool					connect_received;
	/* [한국어] CONNECT capsule 을 1 회 수신했는지 (재전송 방지/검증용).
	 * 설정자: connect 디스패처 진입 시 true. */

	bool					disconnect_started;
	/* [한국어] disconnect 가 시작되어 더 이상 새 명령 수락 안 함을 표시.
	 * 설정자: spdk_nvmf_qpair_disconnect. */

	uint16_t				trace_id;
	/* [한국어] SPDK 트레이스 프레임워크용 객체 식별자. spdk_trace_register_object 로 할당. */

	/* Number of IO outstanding at transport level */
	uint16_t				queue_depth;
	/* [한국어] 트랜스포트 레벨에서 진행 중인 I/O 개수. 백압 / 통계용.
	 * 설정자: transport req 시작/완료 시 ++/--. */

	struct spdk_nvmf_qpair_auth		*auth;
	/* [한국어] DH-HMAC-CHAP 인증 컨텍스트. AUTHENTICATING 상태에서 사용, 이후 NULL. */

	struct {
		/* Indicates whether numa.id is valid, needed for numa.id == 0 case */
		uint32_t			id_valid : 1;
		/* [한국어] numa.id 필드가 유효한지 여부. id==0 인 경우와 "미설정" 을 구분하기 위해 필요. */

		int32_t				id : 31;
		/* [한국어] 본 qpair 의 NUMA 노드 ID. 트랜스포트가 wire 인터페이스의 NUMA 위치를 보고
		 * set. 코어는 이를 보고 같은 NUMA 의 poll_group 으로 배정. */
	} numa;
};

/*
 * [한국어]
 * spdk_nvmf_qpair_get_numa_id - qpair 의 NUMA id 안전 조회.
 *
 * @qpair: 대상 qpair.
 * @return: numa.id_valid 면 numa.id, 아니면 SPDK_ENV_NUMA_ID_ANY.
 *
 * 트랜스포트가 NUMA 정보를 제공한 경우 그 값을 반환하고, 미설정이면 "any" 를 의미하는
 * 마커 값을 반환. poll_group 배정 시 호출되어 NUMA-locality 최적화에 사용.
 */
static inline int32_t
spdk_nvmf_qpair_get_numa_id(struct spdk_nvmf_qpair *qpair)
{
	/* [한국어] id_valid 비트가 1 이면 실제 NUMA id 반환, 아니면 ANY 매직값. */
	return qpair->numa.id_valid ? qpair->numa.id : SPDK_ENV_NUMA_ID_ANY;
}

/*
 * [한국어]
 * struct spdk_nvmf_transport_poll_group - 한 transport 가 한 poll_group 안에서 가지는 sub-group.
 *
 * spdk_nvmf_poll_group 은 N 개의 transport 별 sub-group(rdma_pg, tcp_pg, ...) 을 fan-in.
 * 각 sub-group 이 자기 transport 의 qpair 들을 polling 하고, 자기 iobuf 캐시를 갖는다.
 */
struct spdk_nvmf_transport_poll_group {
	struct spdk_nvmf_transport					*transport;
	/* [한국어] 이 sub-group 이 속한 transport 백포인터. 설정자: poll_group_create 시. */

	/* Requests that are waiting to obtain a data buffer */
	STAILQ_HEAD(, spdk_nvmf_request)				pending_buf_queue;
	/* [한국어] iobuf 가 모자라 보류된 요청 리스트. 가용 시 dequeue 하여 재시도.
	 * 설정자/읽는 자: spdk_nvmf_request_get_buffers / 콜백. */

	struct spdk_iobuf_channel					*buf_cache;
	/* [한국어] 이 thread 의 iobuf 채널 — 데이터 버퍼 풀 핸들. polled-mode 의 lockless 버퍼 풀.
	 * 설정자: poll_group 생성 시 spdk_iobuf_channel_init. */

	struct spdk_nvmf_poll_group					*group;
	/* [한국어] 부모 poll_group 백포인터. */

	struct spdk_poller						*poller;
	/* [한국어] 이 sub-group 의 polling 콜백(spdk_poller). poll_group_poll 을 주기적 실행.
	 * 설정자: poll_group_create. */

	TAILQ_ENTRY(spdk_nvmf_transport_poll_group)			link;
	/* [한국어] 부모 group->tgroups 리스트의 엔트리. */
};

/*
 * [한국어]
 * struct spdk_nvmf_poll_group - per-thread polling group. nvmf 의 fan-in 핵심 단위.
 *
 * 한 spdk_thread(=한 reactor) 위에 1 개 생성. 그 thread 가 처리하는 모든 qpair / 모든
 * transport / 모든 subsystem 의 통계가 본 객체에 모인다. 코어는 spdk_nvmf_tgt 위의
 * poll_group 들을 round-robin 으로 새 qpair 에 배정.
 */
struct spdk_nvmf_poll_group {
	struct spdk_thread				*thread;
	/* [한국어] 이 poll_group 을 소유한 spdk_thread. 모든 콜백은 이 thread 에서 실행됨이 보장.
	 * 설정자: spdk_nvmf_poll_group_create. 읽는 자: spdk_thread_send_msg 등 cross-thread 호출. */

	TAILQ_HEAD(, spdk_nvmf_transport_poll_group)	tgroups;
	/* [한국어] 등록된 transport 별 sub-group 리스트. 등록된 모든 transport 마다 1 개씩 존재. */

	/* Array of poll groups indexed by subsystem id (sid) */
	struct spdk_nvmf_subsystem_poll_group		*sgroups;
	/* [한국어] subsystem 별 sub-group 배열 (sid → sgroup). subsystem 단위 통계/상태 보관.
	 * 설정자: subsystem 추가 시 grow. 읽는 자: 모든 subsystem-scoped 처리. */

	uint32_t					num_sgroups;
	/* [한국어] sgroups 배열 길이(sid 의 max + 1). */

	/* Protected by mutex. Counts qpairs that have connected at a
	 * transport level, but are not associated with a subsystem
	 * or controller yet (because the CONNECT capsule hasn't
	 * been received). */
	uint32_t					current_unassociated_qpairs;
	/* [한국어] 트랜스포트 연결은 됐지만 아직 NVMe Connect 안 받은 qpair 수. mutex 보호.
	 * 설정자/읽는 자: new_qpair / connect 처리 / disconnect — 모두 mutex 잠금. */

	/* All of the queue pairs that belong to this poll group */
	TAILQ_HEAD(, spdk_nvmf_qpair)			qpairs;
	/* [한국어] 이 poll_group 이 polling 하는 모든 qpair 목록. 설정자: poll_group_add/remove. */

	/* Statistics */
	struct spdk_nvmf_poll_group_stat		stat;
	/* [한국어] admin/IO 명령 누적 카운터, 완료 누적, in-flight 등. RPC 로 노출. */

	spdk_nvmf_poll_group_destroy_done_fn		destroy_cb_fn;
	/* [한국어] poll_group 비동기 파괴 완료 콜백. NULL 가능. */

	void						*destroy_cb_arg;
	/* [한국어] destroy_cb_fn 에 전달할 opaque. */

	struct spdk_nvmf_tgt				*tgt;
	/* [한국어] 부모 nvmf target 백포인터. */

	TAILQ_ENTRY(spdk_nvmf_poll_group)		link;
	/* [한국어] tgt->poll_groups 리스트 엔트리. */

	pthread_mutex_t					mutex;
	/* [한국어] current_unassociated_qpairs 등 cross-thread 변경이 가능한 필드 보호용 락.
	 * 일반 polled-mode 경로에서는 락 없이 동작하지만, transport accept thread 등 외부에서
	 * 카운터 변경 시 본 mutex 사용. */
};

/*
 * [한국어]
 * struct spdk_nvmf_listener - 트랜스포트가 listen 중인 주소 1개의 표현.
 *
 * 한 transport 가 여러 trid(IPv4/v6 + port 조합) 에서 listen 가능. 동일 trid 에 여러
 * subsystem 이 associate 될 수 있어 ref-count 로 관리.
 */
struct spdk_nvmf_listener {
	struct spdk_nvme_transport_id	trid;
	/* [한국어] 트랜스포트 ID — trtype/adrfam/traddr/trsvcid/subnqn. listen 의 식별자. */

	uint32_t			ref;
	/* [한국어] 이 listener 를 사용 중인 subsystem 수. 0 이 되면 stop_listen + 해제. */

	char				*sock_impl;
	/* [한국어] (TCP 한정) 어떤 sock 백엔드(posix/uring/...) 를 쓸지 지정. NULL 이면 기본. */

	TAILQ_ENTRY(spdk_nvmf_listener)	link;
	/* [한국어] transport->listeners 리스트 엔트리. */
};

/**
 * A subset of struct spdk_nvme_ctrlr_data that are emulated by a fabrics device.
 */
/*
 * [한국어]
 * struct spdk_nvmf_ctrlr_data - target 이 emulate 하는 NVMe Identify Controller 의 부분집합.
 *
 * 트랜스포트마다 wire 특성에 맞춰 일부 cdata 필드를 다르게 채워야 하므로, 코어가 아니라
 * 트랜스포트 ops->cdata_init 콜백이 채우도록 분리한 미니 구조체. 이후 코어가 이 값을
 * 전체 spdk_nvme_ctrlr_data 에 머지.
 */
struct spdk_nvmf_ctrlr_data {
	uint8_t aerl;
	/* [한국어] Async Event Request Limit (실제 = aerl+1 개). 호스트가 동시에 걸 수 있는 AER
	 * 슬롯 수 제한. 설정자: 트랜스포트의 cdata_init. 읽는 자: identify ctrlr 응답. */

	uint16_t kas;
	/* [한국어] Keep Alive Support 단위(100ms). 0 이면 미지원, 그 외 keep-alive 최소 timeout. */

	/** pci vendor id */
	uint16_t vid;
	/* [한국어] 가상 NVMe 컨트롤러의 vendor id. 호스트가 vendor 식별에 사용. */

	/** pci subsystem vendor id */
	uint16_t ssvid;
	/* [한국어] 서브시스템 vendor id. */

	/** ieee oui identifier */
	uint8_t ieee[3];
	/* [한국어] IEEE OUI 3바이트 — namespace identifier(EUI/NGUID) 생성 시 prefix 로 사용. */

	uint8_t cntrltype;
	/* [한국어] Controller Type (1 = I/O, 2 = Discovery, 3 = Admin). subsystem 종류에 따라 다르게 set. */

	struct spdk_nvme_cdata_oacs oacs;
	/* [한국어] Optional Admin Command Support 비트필드(security send/recv, namespace mgmt 등).
	 * 트랜스포트가 자기 능력을 비트로 표시. */

	struct spdk_nvme_cdata_oncs oncs;
	/* [한국어] Optional NVM Command Support (write zeroes, dataset mgmt, reservations, ...). */

	struct spdk_nvme_cdata_fuses fuses;
	/* [한국어] Fused Operation 지원 비트. */

	struct spdk_nvme_cdata_sgls sgls;
	/* [한국어] SGL 지원 비트필드 — 키ed/언키ed/in-capsule/bit-bucket 지원 여부. 트랜스포트별 차이. */

	struct spdk_nvme_cdata_nvmf_specific nvmf_specific;
	/* [한국어] NVMe-oF specific cdata — IO 큐 facility, controller attribute. */
};

#define MAX_MEMPOOL_NAME_LENGTH 40
/* [한국어] DPDK rte_mempool 이름 길이 상한(NUL 포함). spdk_nvmf_transport.iobuf_name 에 사용. */

/* abidiff has a problem with changes in spdk_nvmf_transport_opts, so spdk_nvmf_transport had to be
 * added to the suppression list, so if spdk_nvmf_transport is changed, we need to remove the
 * suppression and bump up the major version.
 */
/*
 * [한국어]
 * struct spdk_nvmf_transport - 트랜스포트 인스턴스(타입별 1 개) 의 베이스 클래스.
 *
 * 각 트랜스포트(rdma/tcp/fc/...) 모듈은 본 구조체를 임베드한 자기 확장 구조체를 만들어
 * 사용 (예: spdk_nvmf_rdma_transport — 첫 멤버가 본 구조체). 따라서 코어는 base 포인터로
 * 다루고 모듈은 container_of 로 자기 확장에 접근.
 */
struct spdk_nvmf_transport {
	struct spdk_nvmf_tgt			*tgt;
	/* [한국어] 부모 nvmf target. 설정자: spdk_nvmf_tgt_add_transport 시. */

	const struct spdk_nvmf_transport_ops	*ops;
	/* [한국어] vtable. 모든 트랜스포트 콜백의 진입점. SPDK_NVMF_TRANSPORT_REGISTER 가 register. */

	struct spdk_nvmf_transport_opts		opts;
	/* [한국어] 트랜스포트 옵션 — io_unit_size, max_qpairs, in_capsule_data_size 등. RPC 또는
	 * config 에서 set. */

	char					iobuf_name[MAX_MEMPOOL_NAME_LENGTH];
	/* [한국어] 이 transport 가 쓰는 iobuf pool 이름. 디버깅/통계용. */

	TAILQ_HEAD(, spdk_nvmf_listener)	listeners;
	/* [한국어] 이 transport 가 listen 중인 주소 목록. 설정자: ops->listen / stop_listen. */

	TAILQ_ENTRY(spdk_nvmf_transport)	link;
	/* [한국어] tgt->transports 리스트 엔트리. */

	pthread_mutex_t				mutex;
	/* [한국어] listeners / opts 변경 시 사용하는 락. polled-mode 경로(I/O hot path) 에서는
	 * 사용되지 않고, RPC/config 경로의 cross-thread 동기화 용도. */
};

/*
 * [한국어]
 * spdk_nvmf_transport_qpair_fini_cb - qpair_fini 비동기 완료 콜백 시그니처.
 *
 * 트랜스포트가 자기 자원(connection, MR 등) 정리를 완료한 뒤 코어에 통지할 때 호출.
 */
typedef void (*spdk_nvmf_transport_qpair_fini_cb)(void *cb_arg);

/*
 * [한국어]
 * struct spdk_nvmf_transport_ops - 트랜스포트 vtable.
 *
 * 모든 트랜스포트 모듈이 구현해야 하는 콜백 집합. 일부는 optional(NULL 허용). 코어는
 * 이 vtable 을 통해서만 트랜스포트와 상호작용하므로, RDMA/TCP/FC/vfio_user 같이 매우
 * 다른 wire 프로토콜이 동일한 인터페이스로 다룰 수 있다.
 */
struct spdk_nvmf_transport_ops {
	/**
	 * Transport name
	 */
	char name[SPDK_NVMF_TRSTRING_MAX_LEN];
	/* [한국어] 트랜스포트 이름 문자열("RDMA", "TCP", "FC", "PCIE-vfio"). lookup 키.
	 * 설정자: 정적 초기화. 읽는 자: spdk_nvmf_get_transport / RPC. */

	/**
	 * Transport type
	 */
	enum spdk_nvme_transport_type type;
	/* [한국어] NVMe spec 정의 트랜스포트 타입(SPDK_NVME_TRANSPORT_RDMA/TCP/FC/CUSTOM 등).
	 * 호스트의 trid 매칭에 사용. */

	/**
	 * Initialize transport options to default value
	 */
	void (*opts_init)(struct spdk_nvmf_transport_opts *opts);
	/* [한국어] 트랜스포트별 기본 opts 채우기. RPC 가 사용자 입력 머지 전에 호출.
	 * 호출 컨텍스트: 코어 init thread. callee: 자기 모듈 정적 default 값. */

	/**
	 * Create a transport for the given transport opts. Either synchronous
	 * or asynchronous version shall be implemented.
	 */
	struct spdk_nvmf_transport *(*create)(struct spdk_nvmf_transport_opts *opts);
	/* [한국어] 동기 create — opts 기반으로 transport 인스턴스 생성. async 와 둘 중 하나만
	 * 구현해도 됨. 실패 시 NULL. */

	int (*create_async)(struct spdk_nvmf_transport_opts *opts, spdk_nvmf_transport_create_done_cb cb_fn,
			    void *cb_arg);
	/* [한국어] 비동기 create — TCP/RDMA accept thread 같이 별도 setup 이 필요한 트랜스포트용.
	 * 0 = 진행 중(콜백으로 결과 통지), 음수 = 즉시 실패. */

	/**
	 * Dump transport-specific opts into JSON
	 */
	void (*dump_opts)(struct spdk_nvmf_transport *transport,
			  struct spdk_json_write_ctx *w);
	/* [한국어] write_config_json 시 트랜스포트별 추가 옵션을 JSON 으로 출력.
	 * 호출 컨텍스트: RPC thread. */

	/**
	 * Destroy the transport
	 */
	void (*destroy)(struct spdk_nvmf_transport *transport,
			spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg);
	/* [한국어] 트랜스포트 인스턴스 비동기 파괴. 콜백으로 완료 통지. */

	/**
	  * Instruct the transport to accept new connections at the address
	  * provided. This may be called multiple times.
	  */
	int (*listen)(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
		      struct spdk_nvmf_listen_opts *opts);
	/* [한국어] trid 에서 listen 시작. 동일 trid 에 다시 호출되면 ref++. 0 = 성공, 음수 errno. */

	/**
	 * Dump transport-specific listen opts into JSON
	 */
	void (*listen_dump_opts)(struct spdk_nvmf_transport *transport,
				 const struct spdk_nvme_transport_id *trid, struct spdk_json_write_ctx *w);
	/* [한국어] listen 별 추가 JSON 옵션 dump. */

	/**
	  * Stop accepting new connections at the given address.
	  */
	void (*stop_listen)(struct spdk_nvmf_transport *transport,
			    const struct spdk_nvme_transport_id *trid);
	/* [한국어] 해당 trid 의 listen 종료. ref-- 가 0 이면 실제 socket close. */

	/**
	 * It is a notification that a listener is being associated with the subsystem.
	 * Most transports will not need to take any action here, as the enforcement
	 * of the association is done in the generic code.
	 *
	 * Returns a negated errno code to block the association. 0 to allow.
	 */
	int (*listen_associate)(struct spdk_nvmf_transport *transport,
				const struct spdk_nvmf_subsystem *subsystem,
				const struct spdk_nvme_transport_id *trid);
	/* [한국어] subsystem 이 listener 와 associate 되려 함을 알림. 트랜스포트가 거부 가능
	 * (예: 보안 정책). 대부분 구현은 no-op 으로 0 반환. */

	/**
	 * It is a notification that a namespace is being added to the subsystem.
	 * Most transports will not need to take any action here.
	 *
	 * Returns a negated errno code to block the attachment. 0 to allow.
	 */
	int (*subsystem_add_ns)(struct spdk_nvmf_transport *transport,
				const struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *ns);
	/* [한국어] ns 추가 통지 (예: FC 가 namespace 정책 검사). 0 허용 / 음수 차단. */

	/**
	 * It is a notification that a namespace has been removed from the subsystem.
	 * Most transports will not need to take any action here.
	 */
	void (*subsystem_remove_ns)(struct spdk_nvmf_transport *transport,
				    const struct spdk_nvmf_subsystem *subsystem, uint32_t nsid);
	/* [한국어] ns 제거 통지. */

	/**
	 * Initialize subset of identify controller data.
	 */
	void (*cdata_init)(struct spdk_nvmf_transport *transport, struct spdk_nvmf_subsystem *subsystem,
			   struct spdk_nvmf_ctrlr_data *cdata);
	/* [한국어] spdk_nvmf_ctrlr_data 의 트랜스포트별 부분 채우기. 코어가 호출. */

	/**
	 * Fill out a discovery log entry for a specific listen address.
	 */
	void (*listener_discover)(struct spdk_nvmf_transport *transport,
				  struct spdk_nvme_transport_id *trid,
				  struct spdk_nvmf_discovery_log_page_entry *entry);
	/* [한국어] Discovery Log Page 의 한 entry(1 listener) 채우기. trid + transport-specific
	 * fabric attribute(예: RDMA QP service type, TCP TLS PSK). */

	/**
	 * Create a new poll group
	 */
	struct spdk_nvmf_transport_poll_group *(*poll_group_create)(struct spdk_nvmf_transport *transport,
			struct spdk_nvmf_poll_group *group);
	/* [한국어] 새 thread 의 poll_group 안에 자기 sub-group 을 생성(spdk_thread per-thread).
	 * 실패 시 NULL. */

	/**
	 * Get the polling group of the queue pair optimal for the specific transport
	 */
	struct spdk_nvmf_transport_poll_group *(*get_optimal_poll_group)(struct spdk_nvmf_qpair *qpair);
	/* [한국어] 새 qpair 가 들어왔을 때 어느 poll_group 에 배정해야 가장 효율적인지 추천.
	 * 예: RDMA 는 동일 CQ 를 공유하는 group, TCP 는 NUMA-locality 우선. NULL 이면 기본
	 * round-robin. */

	/**
	 * Destroy a poll group
	 */
	void (*poll_group_destroy)(struct spdk_nvmf_transport_poll_group *group);
	/* [한국어] sub-group 자원 해제. */

	/**
	 * Add a qpair to a poll group
	 */
	int (*poll_group_add)(struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_qpair *qpair);
	/* [한국어] qpair 를 sub-group 의 polling 셋에 등록. 0 성공, 음수 실패. */

	/**
	 * Remove a qpair from a poll group
	 */
	int (*poll_group_remove)(struct spdk_nvmf_transport_poll_group *group,
				 struct spdk_nvmf_qpair *qpair);
	/* [한국어] qpair 를 polling 셋에서 제거. */

	/**
	 * Poll the group to process I/O
	 */
	int (*poll_group_poll)(struct spdk_nvmf_transport_poll_group *group);
	/* [한국어] 핫 루프의 핵심 — 본 sub-group 의 모든 qpair 에서 도착한 capsule/완료를 한 번
	 * 폴링하고, spdk_nvmf_request_exec / req_complete 로 처리. 양수 = 처리한 이벤트 수,
	 * 0 = 없음. SPDK_POLLER_REGISTER 로 spdk_poller 에 묶임. */

	/*
	 * Free the request without sending a response
	 * to the originator. Release memory tied to this request.
	 */
	void (*req_free)(struct spdk_nvmf_request *req);
	/* [한국어] 응답 전송 없이 요청 자원만 해제. 보통 abort 후 cleanup 경로. */

	/*
	 * Signal request completion, which sends a response
	 * to the originator.
	 */
	void (*req_complete)(struct spdk_nvmf_request *req);
	/* [한국어] 응답(CQE) 을 wire 로 전송 + 요청 객체 정리. spdk_nvmf_request_complete 가 호출. */

	/**
	 * Callback for the iobuf based queuing of requests awaiting free buffers.
	 * Called when all requested buffers are allocated for the given request.
	 * Used only if initial spdk_iobuf_get() call didn't allocate all buffers at once
	 * and request was queued internally in the iobuf until free buffers become available.
	 * This callback is optional and not all transports need to implement it.
	 * If not set then transport implementation must queue such requests internally.
	 */
	void (*req_get_buffers_done)(struct spdk_nvmf_request *req);
	/* [한국어] iobuf 기반 보류된 요청에 모든 버퍼가 할당됐을 때 통지. optional. */

	/*
	 * Deinitialize a connection.
	 */
	void (*qpair_fini)(struct spdk_nvmf_qpair *qpair,
			   spdk_nvmf_transport_qpair_fini_cb cb_fn,
			   void *cb_args);
	/* [한국어] qpair 비동기 종료(connection close, MR 해제). 완료 시 cb_fn 호출. */

	/*
	 * Get the peer transport ID for the queue pair.
	 */
	int (*qpair_get_peer_trid)(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid);
	/* [한국어] 호스트(peer) 의 trid (IP/port/NQN) 조회. 0 성공. */

	/*
	 * Get the local transport ID for the queue pair.
	 */
	int (*qpair_get_local_trid)(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid);
	/* [한국어] 로컬(자기 측) trid 조회. */

	/*
	 * Get the listener transport ID that accepted this qpair originally.
	 */
	int (*qpair_get_listen_trid)(struct spdk_nvmf_qpair *qpair,
				     struct spdk_nvme_transport_id *trid);
	/* [한국어] 이 qpair 를 accept 한 listener 의 trid 조회. multipath/ANA 결정에 사용. */

	/*
	 * Abort the request which the abort request specifies.
	 * This function can complete synchronously or asynchronously, but
	 * is expected to call spdk_nvmf_request_complete() in the end
	 * for both cases.
	 */
	void (*qpair_abort_request)(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvmf_request *req);
	/* [한국어] NVMe Abort 명령(req) 처리 — 트랜스포트 내부 큐의 in-flight 요청을 정리.
	 * sync/async 모두 가능하나 끝에 반드시 spdk_nvmf_request_complete(req) 호출. */

	/*
	 * Dump transport poll group statistics into JSON.
	 */
	void (*poll_group_dump_stat)(struct spdk_nvmf_transport_poll_group *group,
				     struct spdk_json_write_ctx *w);
	/* [한국어] 통계 JSON dump (RPC poll_group_get_stat 응답에 머지). */

	/*
	 * A notification that a subsystem has been configured to allow access
	 * from the given host.
	 * This callback is optional and not all transports need to implement it.
	 */
	int (*subsystem_add_host)(struct spdk_nvmf_transport *transport,
				  const struct spdk_nvmf_subsystem *subsystem,
				  const char *hostnqn,
				  const struct spdk_json_val *transport_specific);
	/* [한국어] subsystem 이 host 1 명을 허용하도록 구성됐음을 알림. 트랜스포트가 거부 가능
	 * (음수 errno). transport_specific 은 RPC JSON 의 추가 파라미터(예: TCP PSK 키링 식별자). */

	/*
	 * A notification that a subsystem is no longer configured to allow access
	 * from the given host.
	 * This callback is optional and not all transports need to implement it.
	 */
	void (*subsystem_remove_host)(struct spdk_nvmf_transport *transport,
				      const struct spdk_nvmf_subsystem *subsystem,
				      const char *hostnqn);
	/* [한국어] host 허용 해제 통지. */

	/*
	 * A callback used to dump subsystem's host data for a specific transport.
	 * This callback is optional and not all transports need to implement it.
	 */
	void (*subsystem_dump_host)(struct spdk_nvmf_transport *transport,
				    const struct spdk_nvmf_subsystem *subsystem,
				    const char *hostnqn, struct spdk_json_write_ctx *w);
	/* [한국어] write_config_json 에서 subsystem-host 의 트랜스포트별 추가 정보를 JSON 으로
	 * 출력. */
};

/**
 * Register the operations for a given transport type.
 *
 * This function should be invoked by referencing the macro
 * SPDK_NVMF_TRANSPORT_REGISTER macro in the transport's .c file.
 *
 * \param ops The operations associated with an NVMe-oF transport.
 */
/*
 * [한국어]
 * spdk_nvmf_transport_register - 트랜스포트 vtable 을 nvmf 코어에 등록.
 *
 * @ops: 정적 const 으로 만든 vtable 포인터.
 *
 * 보통 직접 호출하지 않고 SPDK_NVMF_TRANSPORT_REGISTER 매크로를 통해 컴파일 타임 GCC
 * constructor attribute 로 자동 호출. 등록되면 spdk_nvmf_get_transport(name) 으로 lookup
 * 가능. 호출 컨텍스트: 프로세스 시작 시 main() 진입 전(constructor) 또는 모듈 init.
 */
void spdk_nvmf_transport_register(const struct spdk_nvmf_transport_ops *ops);

/*
 * [한국어]
 * spdk_nvmf_ctrlr_connect - Fabric Connect capsule 처리 진입점.
 *
 * @req: connect_cmd 가 들어 있는 요청.
 * @return: 0 성공, 음수 errno.
 *
 * 트랜스포트가 NVMe-oF Connect capsule 을 받았을 때 코어에 위임하는 핵심 함수. 호스트 NQN
 * 검증, controller 인스턴스 할당, qpair-controller 매핑, 인증 필요 시 AUTHENTICATING 진입.
 * admin qpair(qid=0) 면 새 ctrlr 생성, IO qpair 면 기존 ctrlr 에 attach.
 *
 * 호출 체인:
 *   transport 가 connect capsule 디코드 → [이 함수] → nvmf_ctrlr_cmd_connect → spdk_nvmf_request_complete
 */
int spdk_nvmf_ctrlr_connect(struct spdk_nvmf_request *req);

/**
 * Function to be called for each newly discovered qpair.
 *
 * \param tgt The nvmf target
 * \param qpair The newly discovered qpair.
 */
/*
 * [한국어]
 * spdk_nvmf_tgt_new_qpair - 트랜스포트가 새 qpair 를 만들어 코어에 인계할 때 호출.
 *
 * @tgt: 부모 nvmf target.
 * @qpair: 새로 만들어진 qpair (state = UNINITIALIZED 또는 CONNECTING 진입 직전).
 *
 * 코어가 이 qpair 를 어느 poll_group 에 배정할지 결정하고(spdk_thread_send_msg 로 해당
 * thread 에 등록 메시지), poll_group_add ops 를 호출. 트랜스포트는 이 함수를 자기
 * accept thread / poll_group_poll 에서 호출.
 */
void spdk_nvmf_tgt_new_qpair(struct spdk_nvmf_tgt *tgt, struct spdk_nvmf_qpair *qpair);

/*
 * [한국어]
 * spdk_nvmf_qpair_is_active - qpair 가 capsule 을 받아 처리할 수 있는 상태인지 검사.
 *
 * @qpair: 대상.
 * @return: CONNECTING/AUTHENTICATING/ENABLED 면 true. 그 외(UNINIT/DEACTIVATING/ERROR) false.
 *
 * 디스패처/통계가 "현재 valid 인 qpair 인가" 빠르게 판단할 때 사용. inline 으로 핫패스 비용 최소화.
 */
static inline bool
spdk_nvmf_qpair_is_active(struct spdk_nvmf_qpair *qpair)
{
	/* [한국어] 활성 상태 3 가지 중 하나면 true. fabric connect 진행 / 인증 중 / 일반 운영 */
	return qpair->state == SPDK_NVMF_QPAIR_CONNECTING ||
	       qpair->state == SPDK_NVMF_QPAIR_AUTHENTICATING ||
	       qpair->state == SPDK_NVMF_QPAIR_ENABLED;
}

/**
 * A subset of struct spdk_nvme_registers that are emulated by a fabrics device.
 */
/*
 * [한국어]
 * struct spdk_nvmf_registers - target 이 emulate 하는 NVMe controller register 의 부분집합.
 *
 * NVMe controller 가 PCIe 에서 노출하는 BAR 레지스터(CC/CSTS/AQA/ASQ/ACQ/...) 를
 * Fabrics 에서는 "Property Get/Set" capsule 로 대체. 본 구조체가 그 emulated state.
 */
struct spdk_nvmf_registers {
	union spdk_nvme_cap_register	cap;
	/* [한국어] Controller Capabilities (CAP) — MQES, CQR, AMS, TO 등.
	 * 설정자: 코어 init 시 한 번. 읽는 자: 호스트 prop_get(CAP). 보통 read-only. */

	union spdk_nvme_vs_register	vs;
	/* [한국어] NVMe Version (VS). spec 호환 버전. */

	union spdk_nvme_cc_register	cc;
	/* [한국어] Controller Configuration. enable/iosq es/cqes/shutdown 등.
	 * 설정자: 호스트 prop_set(CC). EN 비트가 0→1 이면 controller 활성화 트리거. */

	union spdk_nvme_csts_register	csts;
	/* [한국어] Controller Status. RDY/CFS/SHST 등. EN 따라 RDY 가 변함.
	 * 설정자: 코어가 cc 변경 후 갱신. */

	union spdk_nvme_aqa_register	aqa;
	/* [한국어] Admin Queue Attributes — admin SQ/CQ 크기. */

	uint64_t			asq;
	/* [한국어] Admin Submission Queue base address (Fabrics 에서는 사용되지 않거나 placeholder). */

	uint64_t			acq;
	/* [한국어] Admin Completion Queue base address (마찬가지). */

	uint32_t			nssr;
	/* [한국어] NVM Subsystem Reset. 0x4E564D65 ("NVMe") 쓰면 subsystem 리셋 트리거. */

	union spdk_nvme_crto_register	crto;
	/* [한국어] Controller Ready Timeouts. RTD3, CRWMT 등 NVMe 1.4+ 신규. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_registers) == 48, "Incorrect size");
/* [한국어] 컴파일 타임 검증 — migration data 와 호환되도록 정확히 48B 여야 한다. */

/*
 * [한국어]
 * spdk_nvmf_ctrlr_get_regs - controller 의 emulated register 묶음 포인터 반환.
 *
 * @ctrlr: 대상 controller.
 * @return: 읽기 전용(const) 레지스터 구조체 포인터.
 *
 * RPC/디버그가 controller 상태를 조회할 때 사용. 쓰기 시도는 prop_set capsule 로만 가능.
 */
const struct spdk_nvmf_registers *spdk_nvmf_ctrlr_get_regs(struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * spdk_nvmf_request_free_buffers - 요청에 할당됐던 iobuf 데이터 버퍼들을 풀로 반환.
 *
 * @req: 대상 요청 (data_from_pool 비트가 1 이어야 의미 있음).
 * @group: req 를 처리한 transport poll group (iobuf channel 보유).
 * @transport: 그 transport.
 *
 * req_free 또는 abort cleanup 에서 호출. 호출 후 req->iov 는 더 이상 유효하지 않다.
 * pending_buf_queue 에 보류된 다른 요청이 있으면 wakeup.
 */
void spdk_nvmf_request_free_buffers(struct spdk_nvmf_request *req,
				    struct spdk_nvmf_transport_poll_group *group,
				    struct spdk_nvmf_transport *transport);

/*
 * [한국어]
 * spdk_nvmf_request_get_buffers - 요청에 필요한 데이터 버퍼들을 iobuf 풀에서 할당.
 *
 * @req: 대상 요청.
 * @group: transport poll group(per-thread iobuf 채널 소유).
 * @transport: 그 transport.
 * @length: 요구 데이터 길이.
 * @return: 0 성공(req->iov 채워짐), 음수 = 즉시 실패, 양수 = 부분 할당됨(pending_buf_queue
 *          에 본 요청이 enqueue 되어 후속 가용 시 콜백).
 *
 * 호스트가 send/receive 할 페이로드 영역을 polled-mode 의 lockless 풀에서 확보. 풀이 비면
 * 보류 큐에 들어가고, 다른 요청이 free_buffers 를 호출할 때 wakeup.
 */
int spdk_nvmf_request_get_buffers(struct spdk_nvmf_request *req,
				  struct spdk_nvmf_transport_poll_group *group,
				  struct spdk_nvmf_transport *transport,
				  uint32_t length);

/*
 * [한국어]
 * spdk_nvmf_request_get_dif_ctx - 요청의 DIF 컨텍스트 채우기.
 *
 * @req: 대상 요청.
 * @dif_ctx: [out] DIF 변환 ctx.
 * @return: dif 적용 가능하면 true 후 ctx 채움, 아니면 false.
 *
 * 트랜스포트가 dif strip/insert 를 자체 처리할 때 사용. ns 의 PI 설정과 cmd 의 LBA 정보를
 * 읽어 LBA size, meta size, guard tag 정책을 알려줌.
 */
bool spdk_nvmf_request_get_dif_ctx(struct spdk_nvmf_request *req, struct spdk_dif_ctx *dif_ctx);

/*
 * [한국어]
 * spdk_nvmf_request_exec - 요청을 nvmf 코어 디스패처에 제출.
 *
 * @req: capsule 디코드가 끝난 요청.
 *
 * 트랜스포트가 capsule 을 디코드하고 cmd/iov 를 채운 뒤 호출. 코어가 admin/IO/auth/connect
 * 분기를 결정하고, custom 핸들러 검사, bdev 위임, 결과를 spdk_nvmf_request_complete 로 회신.
 *
 * 호출 체인:
 *   transport poll_group_poll → [이 함수] → nvmf_ctrlr_process_admin/io_cmd → bdev → req_complete
 */
void spdk_nvmf_request_exec(struct spdk_nvmf_request *req);

/*
 * [한국어]
 * spdk_nvmf_request_free - 요청을 응답 없이 해제.
 *
 * @req: 대상 요청.
 * @return: 0 성공, 음수 errno.
 *
 * abort 시점/에러 정리 등 호스트로의 응답을 보내지 않고 자원만 해제할 때 사용.
 * 트랜스포트의 ops->req_free 가 호출되어 wire 자원도 정리.
 */
int spdk_nvmf_request_free(struct spdk_nvmf_request *req);

/*
 * [한국어]
 * spdk_nvmf_request_complete - 요청 응답 회신 + 자원 해제.
 *
 * @req: 응답이 채워진 요청 (rsp 가 정상/에러 SC 포함).
 * @return: 0 성공, 음수 errno.
 *
 * 코어와 사용자 핸들러가 호출하는 표준 종료 함수. 내부적으로 ops->req_complete 호출 →
 * wire 송신 → req_free 또는 풀 반환. 비동기 경로의 마지막 단계.
 */
int spdk_nvmf_request_complete(struct spdk_nvmf_request *req);

/*
 * [한국어]
 * spdk_nvmf_request_zcopy_start - zero-copy 시작 (bdev 의 메모리 영역 노출 요청).
 *
 * @req: zcopy 가능 요청 (read/write 단순 명령). 시작 후 zcopy_phase = INIT.
 *
 * bdev 가 자기 메모리 영역을 직접 노출하면 트랜스포트는 그 영역 위에서 wire 송수신을
 * 수행 — 메모리 복사 회피. bdev 가 zcopy 를 지원해야 하며, 미지원이면 INIT_FAILED → 일반
 * copy 경로 폴백 또는 즉시 에러.
 */
void spdk_nvmf_request_zcopy_start(struct spdk_nvmf_request *req);

/*
 * [한국어]
 * spdk_nvmf_request_zcopy_end - zero-copy 종료 (commit 또는 abort).
 *
 * @req: zcopy 진행 중 요청.
 * @commit: true = 데이터 유효(write 의 경우 backing 에 반영), false = 데이터 무효(abort).
 *
 * EXECUTE 단계 종료 후 호출. END_PENDING → bdev 콜백 → COMPLETE 로 진행.
 */
void spdk_nvmf_request_zcopy_end(struct spdk_nvmf_request *req, bool commit);

/*
 * [한국어]
 * spdk_nvmf_request_using_zcopy - 요청이 zero-copy 경로인지 빠른 검사.
 *
 * @req: 대상.
 * @return: zcopy_phase != NONE 이면 true.
 *
 * 트랜스포트가 핫패스에서 일반/zcopy 경로 분기를 결정할 때 사용. inline.
 */
static inline bool
spdk_nvmf_request_using_zcopy(const struct spdk_nvmf_request *req)
{
	/* [한국어] zcopy_phase 가 NONE(0) 이 아니면 zcopy 사용 중. */
	return req->zcopy_phase != NVMF_ZCOPY_PHASE_NONE;
}

/**
 * Remove the given qpair from the poll group.
 *
 * \param qpair The qpair to remove.
 */
/*
 * [한국어]
 * spdk_nvmf_poll_group_remove - qpair 를 자기 poll_group 에서 제거.
 *
 * @qpair: 대상 qpair.
 *
 * disconnect 경로에서 호출. 내부적으로 트랜스포트의 poll_group_remove 를 거쳐 polling 셋
 * 에서 빠지고, qpair->group = NULL 로 설정. 반드시 qpair 가 바인딩된 spdk_thread 에서 호출.
 */
void spdk_nvmf_poll_group_remove(struct spdk_nvmf_qpair *qpair);

/**
 * Get the NVMe-oF subsystem associated with this controller.
 *
 * \param ctrlr The NVMe-oF controller
 *
 * \return The NVMe-oF subsystem
 */
/*
 * [한국어]
 * spdk_nvmf_ctrlr_get_subsystem - controller 의 부모 subsystem 반환.
 *
 * @ctrlr: 대상 컨트롤러.
 * @return: 그 ctrlr 이 attach 된 subsystem.
 */
struct spdk_nvmf_subsystem *
spdk_nvmf_ctrlr_get_subsystem(struct spdk_nvmf_ctrlr *ctrlr);

/**
 * Get the NVMe-oF controller ID.
 *
 * \param ctrlr The NVMe-oF controller
 *
 * \return The NVMe-oF controller ID
 */
/*
 * [한국어]
 * spdk_nvmf_ctrlr_get_id - controller 의 cntlid 반환.
 *
 * @ctrlr: 대상.
 * @return: cntlid (host 별로 unique 한 16비트 ID, NVMe-oF 가 connect 응답에 회신하는 값).
 */
uint16_t spdk_nvmf_ctrlr_get_id(struct spdk_nvmf_ctrlr *ctrlr);

/*
 * [한국어]
 * struct spdk_nvmf_ctrlr_feat - controller 가 emulate 하는 NVMe Feature 값들.
 *
 * NVMe Get/Set Features 가 변경하는 controller-wide 상태. live migration 시 함께 옮긴다.
 */
struct spdk_nvmf_ctrlr_feat {
	union spdk_nvme_feat_arbitration arbitration;
	/* [한국어] FID 0x01 — Arbitration. AB/LPW/MPW/HPW 가중치. 호스트가 set 하면 코어가 저장. */

	union spdk_nvme_feat_power_management power_management;
	/* [한국어] FID 0x02 — Power Management state. WH/PS. */

	union spdk_nvme_feat_error_recovery error_recovery;
	/* [한국어] FID 0x05 — TLER, DULBE 정책. */

	union spdk_nvme_feat_volatile_write_cache volatile_write_cache;
	/* [한국어] FID 0x06 — VWC. write cache enable 여부. */

	union spdk_nvme_feat_number_of_queues number_of_queues;
	/* [한국어] FID 0x07 — NSQR/NCQR. host 가 요청한 IO 큐 수. */

	union spdk_nvme_feat_interrupt_coalescing interrupt_coalescing;
	/* [한국어] FID 0x08 — interrupt coalescing 임계. NVMe-oF 에서는 의미 약함이지만 emulate. */

	union spdk_nvme_feat_interrupt_vector_configuration interrupt_vector_configuration;
	/* [한국어] FID 0x09 — vector 별 설정. */

	union spdk_nvme_feat_write_atomicity write_atomicity;
	/* [한국어] FID 0x0A — write atomicity. */

	union spdk_nvme_feat_async_event_configuration async_event_configuration;
	/* [한국어] FID 0x0B — AER 활성 마스크. 어떤 이벤트(namespace change, ANA change 등)를
	 * 호스트가 받을지. 본 마스크는 ctrlr 의 AER pending 결정에 사용. */

	union spdk_nvme_feat_keep_alive_timer keep_alive_timer;
	/* [한국어] FID 0x0F — keep-alive timeout. 0 이면 미사용. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ctrlr_feat) == 40, "Incorrect size");
/* [한국어] 컴파일 타임 검증 — migration data 와 호환되도록 정확히 40B. */

/*
 * Migration data structure used to save & restore a NVMe-oF controller.
 *
 * The data structure is experimental.
 *
 */
/*
 * [한국어]
 * struct spdk_nvmf_ctrlr_migr_data - VM live migration 시 controller 상태를 저장/복원하는
 *                                     wire-stable 구조체. 4096B 고정.
 *
 * 4 KB 페이지에 register/feature/AER/cntlid 등을 직렬화. data_size/regs_size/feat_size 는
 * 미래 호환을 위한 헤더(앞쪽 16B). source/destination 노드 간 SPDK 버전이 달라도 size
 * 검사로 부분 복원 가능. 마지막 unused 영역은 padding.
 */
struct spdk_nvmf_ctrlr_migr_data {
	/* `data_size` is valid size of `spdk_nvmf_ctrlr_migr_data` without counting `unused`.
	 * We use this field to migrate `spdk_nvmf_ctrlr_migr_data` from source VM and restore
	 * it in destination VM.
	 */
	uint32_t data_size;
	/* [한국어] unused 제외 본 구조체 유효 크기. version 호환 검사용.
	 * 설정자: save_migr_data 호출 시. 읽는 자: restore_migr_data 가 본 값까지만 복사. */

	/* `regs_size` is valid size of `spdk_nvmf_registers`. */
	uint32_t regs_size;
	/* [한국어] regs 의 유효 크기 (sizeof(spdk_nvmf_registers)). 향후 register 추가 시 호환용. */

	/* `feat_size` is valid size of `spdk_nvmf_ctrlr_feat`. */
	uint32_t feat_size;
	/* [한국어] feat 의 유효 크기. */

	uint32_t reserved;
	/* [한국어] 정렬용 reserved. 항상 0. */

	struct spdk_nvmf_registers regs;
	/* [한국어] emulated controller register snapshot. */

	uint8_t regs_reserved[208];
	/* [한국어] regs 영역의 향후 확장용 reserved 영역. 현재 0 채움. */

	struct spdk_nvmf_ctrlr_feat feat;
	/* [한국어] emulated feature 값 snapshot. */

	uint8_t feat_reserved[216];
	/* [한국어] feat 영역의 향후 확장용 reserved. */

	uint16_t cntlid;
	/* [한국어] controller ID. 복원 시에도 동일 cntlid 가 유지되어야 host 가 같은 controller 로 인식. */

	uint8_t acre;
	/* [한국어] Asynchronous Command Retry Enable. NVMe spec 의 controller 옵션. */

	uint8_t num_aer_cids;
	/* [한국어] aer_cids 배열의 유효 길이 (0..SPDK_NVMF_MAX_ASYNC_EVENTS). */

	uint32_t num_async_events;
	/* [한국어] async_events 배열의 유효 길이 (0..MIGR_MAX_PENDING_AERS). */

	union spdk_nvme_async_event_completion async_events[SPDK_NVMF_MIGR_MAX_PENDING_AERS];
	/* [한국어] migration 시점에 보류 중인 AER completion 들. 복원 후 destination 에서
	 * pending 큐에 다시 넣어 host 가 AER 응답으로 받게 한다. */

	uint16_t aer_cids[SPDK_NVMF_MAX_ASYNC_EVENTS];
	/* [한국어] in-flight AER 의 NVMe cid 들. 복원 후 동일 cid 로 응답을 회신해야 host 가 매칭. */

	uint64_t notice_aen_mask;
	/* [한국어] 어떤 notice AEN 들이 host 에 전달되었는지의 비트마스크. 중복 전달 방지용. */

	uint8_t unused[2516];
	/* [한국어] 4KB 패딩. 향후 확장 영역. 항상 0 으로 초기화. */
};
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvmf_ctrlr_migr_data,
			    regs) - offsetof(struct spdk_nvmf_ctrlr_migr_data, data_size) == 16, "Incorrect header size");
/* [한국어] 헤더(앞 4 개 uint32) 가 16B 고정 — wire 호환을 위한 ABI 검증. */
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvmf_ctrlr_migr_data,
			    feat) - offsetof(struct spdk_nvmf_ctrlr_migr_data, regs) == 256, "Incorrect regs size");
/* [한국어] regs(48B) + regs_reserved(208B) = 256B. */
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvmf_ctrlr_migr_data,
			    cntlid) - offsetof(struct spdk_nvmf_ctrlr_migr_data, feat) == 256, "Incorrect feat size");
/* [한국어] feat(40B) + feat_reserved(216B) = 256B. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ctrlr_migr_data) == 4096, "Incorrect size");
/* [한국어] 전체 4 KB — page 정렬, NVMe data buffer 정렬과 동일. */

/**
 * Save the NVMe-oF controller state and configuration.
 *
 * The API is experimental.
 *
 * It is allowed to save the data only when the nvmf subsystem is in paused
 * state i.e. there are no outstanding cmds in nvmf layer (other than aer),
 * pending async event completions are getting blocked.
 *
 * To preserve thread safety this function must be executed on the same thread
 * the NVMe-OF controller was created.
 *
 * \param ctrlr The NVMe-oF controller
 * \param data The NVMe-oF controller state and configuration to be saved
 *
 * \return 0 on success or a negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_ctrlr_save_migr_data - controller 상태를 4KB 직렬화 버퍼에 저장 (live migration).
 *
 * @ctrlr: 대상.
 * @data: [out] 4KB 버퍼.
 * @return: 0 성공, 음수 errno.
 *
 * 사전 조건: subsystem 이 PAUSED 상태여야 함 (in-flight 명령 없음, AER 만 보류). 현재
 * register / feature / pending AER / cntlid 등을 전부 캡처한다. 반드시 controller 가
 * 만들어진 spdk_thread 에서 호출 (cross-thread 사용 시 race).
 */
int spdk_nvmf_ctrlr_save_migr_data(struct spdk_nvmf_ctrlr *ctrlr,
				   struct spdk_nvmf_ctrlr_migr_data *data);

/**
 * Restore the NVMe-oF controller state and configuration.
 *
 * The API is experimental.
 *
 * It is allowed to restore the data only when the nvmf subsystem is in paused
 * state.
 *
 * To preserve thread safety this function must be executed on the same thread
 * the NVMe-OF controller was created.
 *
 * AERs shall be restored using spdk_nvmf_request_exec after this function is executed.
 *
 * \param ctrlr The NVMe-oF controller
 * \param data The NVMe-oF controller state and configuration to be restored
 *
 * \return 0 on success or a negated errno on failure.
 */
/*
 * [한국어]
 * spdk_nvmf_ctrlr_restore_migr_data - 직렬화 버퍼로부터 controller 상태 복원.
 *
 * @ctrlr: 대상 (이미 connect 가 끝난 controller).
 * @data: 복원할 4KB 버퍼.
 * @return: 0 성공, 음수 errno.
 *
 * AER 은 본 함수 호출 후 spdk_nvmf_request_exec 로 다시 제출해야 호스트가 응답을 받는다.
 * 사전 조건: PAUSED + 동일 spdk_thread.
 */
int spdk_nvmf_ctrlr_restore_migr_data(struct spdk_nvmf_ctrlr *ctrlr,
				      const struct spdk_nvmf_ctrlr_migr_data *data);

/*
 * [한국어]
 * spdk_nvmf_req_get_xfer - 요청의 데이터 전송 방향 결정 (inline 핫패스 헬퍼).
 *
 * @req: capsule 디코드 직후의 요청.
 * @return: spdk_nvme_data_transfer (NONE/HOST_TO_CONTROLLER/CONTROLLER_TO_HOST/BIDIRECTIONAL).
 *
 * (1) opcode==FABRIC 이면 fctype 으로 방향 판단 (capsule cmd), 그 외 NVMe opcode 의
 * 표준 비트 0..1 로 판단. (2) 그 결과가 NONE 이면 SGL 검사 없이 즉시 반환. (3) NONE 이
 * 아닌데 SGL length 가 0 이면 데이터 없음 → NONE 으로 강등. KEYED 와 일반 SGL 의 length
 * 필드가 다른 위치에 있으므로 type 별 분기.
 *
 * 호출 컨텍스트: poll_group_poll 핫 루프에서 매 요청마다 호출 → inline 으로 함수 호출 비용
 * 제거가 중요.
 */
static inline enum spdk_nvme_data_transfer
spdk_nvmf_req_get_xfer(struct spdk_nvmf_request *req) {
	enum spdk_nvme_data_transfer xfer;
	/* [한국어] 결과를 담을 로컬 enum. */

	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	/* [한국어] union 의 NVMe SQE 뷰 — opcode/dptr 접근. */

	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;
	/* [한국어] SQE 의 dptr 영역을 SGL 1 디스크립터로 해석. NVMe-oF 는 항상 SGL1 사용
	 * (호스트가 PRP 가 아니라 SGL 만 보냄). */

	/* Figure out data transfer direction */
	if (cmd->opc == SPDK_NVME_OPC_FABRIC)
	{
		/* [한국어] Fabric capsule 인 경우 fctype(connect/property/auth) 별 방향. */
		xfer = spdk_nvme_opc_get_data_transfer(req->cmd->nvmf_cmd.fctype);
	} else
	{
		/* [한국어] 일반 NVMe 명령 — opcode 의 비트 0..1 이 방향(spec NVMe 4.1 Figure). */
		xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);
	}

	if (xfer == SPDK_NVME_DATA_NONE)
	{
		/* [한국어] 명령 자체가 데이터 없는 종류면 SGL 검사 불필요. */
		return xfer;
	}

	/* Even for commands that may transfer data, they could have specified 0 length.
	 * We want those to show up with xfer SPDK_NVME_DATA_NONE.
	 */
	switch (sgl->generic.type)
	{
	case SPDK_NVME_SGL_TYPE_DATA_BLOCK:
	case SPDK_NVME_SGL_TYPE_BIT_BUCKET:
	case SPDK_NVME_SGL_TYPE_SEGMENT:
	case SPDK_NVME_SGL_TYPE_LAST_SEGMENT:
	case SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK:
		/* [한국어] 일반 unkeyed SGL — length 가 unkeyed.length 위치에 있음. */
		if (sgl->unkeyed.length == 0) {
			/* [한국어] 길이 0 → 실제로는 데이터 없음. NONE 으로 강등. */
			xfer = SPDK_NVME_DATA_NONE;
		}
		break;
	case SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK:
		/* [한국어] keyed SGL (RDMA rkey 사용) — length 는 keyed.length 에 있음. */
		if (sgl->keyed.length == 0) {
			/* [한국어] 길이 0 → NONE 으로 강등. */
			xfer = SPDK_NVME_DATA_NONE;
		}
		break;
	}

	return xfer;
	/* [한국어] 최종 결정된 데이터 방향. */
}

/**
 * Complete Asynchronous Event as Error.
 *
 * \param ctrlr Controller whose AER is going to be completed.
 * \param info Asynchronous Event Error Information to be reported.
 *
 * \return int. 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_nvmf_ctrlr_async_event_error_event - 보류 AER 1 개를 "Error" 카테고리로 완료(host 통지).
 *
 * @ctrlr: 대상.
 * @info: 에러 상세(spdk_nvme_async_event_info_error). 예: invalid doorbell write.
 * @return: 0 성공, 음수 errno (보류 AER 없으면 실패).
 *
 * 코어가 controller-wide 에러를 호스트에 알리고 싶을 때 사용. 호스트는 미리 AER 명령을
 * 큐에 걸어 두고 본 함수가 그 중 1 개를 완료시킨다. 호스트는 응답 수신 후 즉시 새 AER 을
 * 다시 보낸다(모니터 패턴).
 */
int spdk_nvmf_ctrlr_async_event_error_event(struct spdk_nvmf_ctrlr *ctrlr,
		enum spdk_nvme_async_event_info_error info);

/**
 * Abort outstanding Asynchronous Event Requests (AERs).
 *
 * Completes AERs with ABORTED_BY_REQUEST status code.
 *
 * \param ctrlr Controller whose AERs are going to be aborted.
 */
/*
 * [한국어]
 * spdk_nvmf_ctrlr_abort_aer - 보류 AER 들을 ABORTED_BY_REQUEST 로 일괄 완료.
 *
 * @ctrlr: 대상 controller.
 *
 * controller shutdown / reset 시 모든 in-flight AER 을 깔끔하게 정리하기 위해 사용.
 */
void spdk_nvmf_ctrlr_abort_aer(struct spdk_nvmf_ctrlr *ctrlr);

/*
 * Macro used to register new transports.
 */
/*
 * [한국어]
 * SPDK_NVMF_TRANSPORT_REGISTER - 트랜스포트 vtable 자동 등록 매크로.
 *
 * @name: 식별자 토큰(중복 방지). 함수명에 stringized.
 * @transport_ops: 정적 const struct spdk_nvmf_transport_ops 의 포인터.
 *
 * GCC __attribute__((constructor)) 를 이용해 main() 진입 전에 spdk_nvmf_transport_register
 * 를 자동 호출. 모듈 .c 파일에서 사용:
 *   SPDK_NVMF_TRANSPORT_REGISTER(rdma, &spdk_nvmf_transport_rdma);
 * 이렇게 하면 사용자가 별도 init 함수 호출 없이도 lib/nvmf/rdma.c 가 링크되는 순간
 * 자동으로 코어에 트랜스포트 등록된다.
 */
#define SPDK_NVMF_TRANSPORT_REGISTER(name, transport_ops) \
static void __attribute__((constructor)) _spdk_nvmf_transport_register_##name(void) \
{ \
	spdk_nvmf_transport_register(transport_ops); \
}

#ifdef __cplusplus
}
/* [한국어] extern "C" 종료. */
#endif

#endif
/* [한국어] include guard 종료. 본 헤더는 트랜스포트 작성자가 가장 먼저 include 해야 하는
 * 핵심 인터페이스이며, 코어와 wire 모듈 사이의 ABI 경계를 정의한다. */
