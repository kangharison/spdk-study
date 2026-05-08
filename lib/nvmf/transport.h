/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * [한국어 설명] NVMe-oF Transport 내부 헬퍼 인터페이스 헤더 (transport.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 NVMe-over-Fabrics target 코어(nvmf.c, ctrlr.c, subsystem.c 등)와
 * 구체적 트랜스포트 구현(rdma.c, tcp.c, fc.c, vfio_user.c) 사이의 **내부 thunk
 * 함수**들을 선언한다. 공개 API(`spdk_nvmf_*`)는 `include/spdk/nvmf_transport.h`
 * 에 정의되어 있고, 본 헤더의 함수들(`nvmf_transport_*` 접두어)은 lib/nvmf/
 * 내부에서만 사용되는 비공개 호출 경로다.
 * 각 함수는 `struct spdk_nvmf_transport_ops` 가상 함수 테이블의 항목을 호출하는
 * 얇은 래퍼 역할을 하며, 트랜스포트별 구현(예: rdma_create_poll_group,
 * tcp_create_poll_group)은 트랜스포트 모듈에서 등록한다.
 * 즉, 이 헤더는 "트랜스포트 다형성"의 중앙 디스패치 진입점 모음이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe-oF target의 실행 흐름:
 *   spdk_nvmf_tgt_create -> spdk_nvmf_subsystem_create -> spdk_nvmf_subsystem_listen
 *     -> (트랜스포트 listen ops) -> spdk_nvmf_poll_group_create
 *     -> nvmf_transport_poll_group_create()  <-- 여기서 트랜스포트별 poll group 생성
 *     -> 호스트 connect -> qpair 생성 -> nvmf_transport_poll_group_add()
 *     -> reactor 주기마다 nvmf_transport_poll_group_poll() 호출
 * 호출 체인 관점:
 *   상위: lib/nvmf/nvmf.c, lib/nvmf/ctrlr.c, lib/nvmf/subsystem.c
 *   본 파일: 디스패치 thunk
 *   하위: lib/nvmf/rdma.c, lib/nvmf/tcp.c, lib/nvmf/fc.c, lib/nvmf/vfio_user.c
 *         (각각 spdk_nvmf_transport_ops를 채워 등록)
 * 실행 컨텍스트: 호스트 유저스페이스, SPDK reactor 스레드. 대부분의 함수는
 * 해당 qpair/poll group이 바인딩된 spdk_thread에서만 호출되어야 한다 (thread affinity).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 공개 API:
 *   - spdk/nvmf.h: spdk_nvmf_qpair, spdk_nvmf_poll_group, spdk_nvmf_request 등
 *   - spdk/nvmf_transport.h: spdk_nvmf_transport, spdk_nvmf_transport_ops,
 *     spdk_nvmf_discovery_log_page_entry, spdk_nvmf_transport_qpair_fini_cb
 *   - spdk/nvme.h: spdk_nvme_transport_id (트랜스포트 식별자: trtype/adrfam/traddr/trsvcid)
 * 데이터 흐름:
 *   - Discovery: nvmf_transport_listener_discover()가 트랜스포트 listener의 정보를
 *     log page entry로 채워, 호스트의 Get Log Page (Discovery) 명령에 응답한다.
 *   - I/O: nvmf_transport_poll_group_poll()가 트랜스포트 큐를 폴링해 들어온
 *     캡슐을 spdk_nvmf_request로 변환, ctrlr.c가 처리 후 nvmf_transport_req_complete()
 *     로 응답.
 * 공유 자료구조:
 *   - spdk_nvmf_transport_poll_group: 트랜스포트가 자체 확장한 poll group
 *   - spdk_nvmf_qpair: 호스트와의 NVMe Queue Pair (트랜스포트별 컨텍스트 포함)
 *   - spdk_nvmf_request: 호스트로부터 받은 명령 캡슐과 응답을 담는 객체
 *
 * === 주요 함수/구조체 요약 ===
 * - nvmf_transport_listener_discover: Discovery Log Page용 entry 채우기 위임
 * - nvmf_transport_poll_group_create: 트랜스포트별 poll group 생성 위임
 * - nvmf_transport_get_optimal_poll_group: qpair에 가장 적합한 poll group 선택 (예: RDMA SRQ 공유)
 * - nvmf_transport_poll_group_destroy/pause/resume: poll group 생명주기/상태 변경
 * - nvmf_transport_poll_group_add/remove: poll group에 qpair 등록/해제
 * - nvmf_transport_poll_group_poll: reactor 주기 콜백 - 큐 폴링 핵심 진입점
 * - nvmf_transport_req_free/complete: 요청 객체 반환/완료 응답 송신
 * - nvmf_transport_qpair_fini: qpair 해제 (비동기 콜백)
 * - nvmf_transport_qpair_get_peer/local/listen_trid: qpair의 트랜스포트 주소 조회
 * - nvmf_transport_qpair_abort_request: qpair에서 특정 요청을 취소
 * - nvmf_request_get/free_stripped_buffers: in-capsule data의 PI(메타데이터) strip 버퍼
 * - nvmf_request_get_buffers_abort: 버퍼 대기 중인 요청을 abort
 */

#ifndef SPDK_NVMF_TRANSPORT_H                       /* [한국어] 헤더 중복 포함 가드 - 한 번만 포함되도록 */
#define SPDK_NVMF_TRANSPORT_H                       /* [한국어] 가드 매크로 정의 */

#include "spdk/stdinc.h"                            /* [한국어] SPDK 표준 인클루드(<stdint.h>, <string.h> 등) - 기본 타입 사용 */

#include "spdk/nvme.h"                              /* [한국어] spdk_nvme_transport_id 등 NVMe 공개 타입 - 트랜스포트 주소 표현 */
#include "spdk/nvmf.h"                              /* [한국어] spdk_nvmf_qpair/poll_group/tgt 등 NVMe-oF 공개 객체 정의 */
#include "spdk/nvmf_transport.h"                    /* [한국어] spdk_nvmf_transport_ops, spdk_nvmf_transport_qpair_fini_cb 등 트랜스포트 가상 ops */

/*
 * [한국어]
 * nvmf_transport_listener_discover - Discovery Log Page entry를 트랜스포트별로 채운다
 *
 * @transport: 어떤 트랜스포트(예: RDMA/TCP/FC)에 대한 listener인지 지시
 * @trid: 채워야 할 listener의 트랜스포트 주소 (trtype/traddr/trsvcid)
 * @entry: 출력 - Discovery Log Page entry. trtype, adrfam, subtype, asqsz, trsvcid 등이 채워짐
 *
 * 호스트가 Discovery 컨트롤러에 Get Log Page (LID=0x70, Discovery) 명령을 보냈을 때,
 * 각 listener에 대해 호출되어 NVMe-oF Discovery Log Page Entry (NVMe-oF 1.x 5.3 절)를
 * 트랜스포트 특성에 맞게 채운다. trtype별 RDMA QP 속성, TCP TLS 정보 등이 포함될 수 있다.
 * 실행 컨텍스트: Discovery 요청을 처리 중인 reactor 스레드.
 *
 * 호출 체인:
 *   ctrlr_discovery.c (Get Log Page handler) -> [본 함수] -> transport->ops->listener_discover
 */
void nvmf_transport_listener_discover(struct spdk_nvmf_transport *transport,
				      struct spdk_nvme_transport_id *trid,
				      struct spdk_nvmf_discovery_log_page_entry *entry);

/*
 * [한국어]
 * nvmf_transport_poll_group_create - 트랜스포트별 poll group 객체 생성 디스패치
 *
 * @transport: 트랜스포트 인스턴스 (RDMA/TCP/FC 등 종류와 옵션 포함)
 * @group: 상위 nvmf poll group - 생성된 transport poll group은 이 group의 tgroups TAILQ에 연결
 * @return: 새로 만든 transport poll group 포인터, 실패 시 NULL
 *
 * spdk_nvmf_poll_group_create() 내부에서 모든 등록 트랜스포트에 대해 한 번씩 호출되어
 * 트랜스포트별 자원(예: RDMA CQ, TCP epoll instance, FC poll group)을 만든다.
 * 실행 컨텍스트: poll group이 소속될 spdk_thread (해당 reactor)에서 호출되어야 함.
 *
 * 호출 체인:
 *   spdk_nvmf_poll_group_create() -> [본 함수] -> transport->ops->poll_group_create()
 */
struct spdk_nvmf_transport_poll_group *nvmf_transport_poll_group_create(
	struct spdk_nvmf_transport *transport, struct spdk_nvmf_poll_group *group);

/*
 * [한국어]
 * nvmf_transport_get_optimal_poll_group - qpair에 최적의 poll group 선택 (트랜스포트 힌트)
 *
 * @transport: 해당 qpair의 트랜스포트
 * @qpair: 새로 들어온(또는 재배치할) qpair
 * @return: 트랜스포트가 추천하는 poll group, 또는 NULL이면 라운드로빈 폴백
 *
 * RDMA의 경우 SRQ(Shared Receive Queue) 공유나 NUMA-affinity, TCP의 경우 동일 NIC RX queue
 * 등을 고려해 트랜스포트가 자체 정책으로 추천한다.
 * spdk_nvmf_tgt::next_poll_group(라운드로빈)보다 우선됨.
 * 실행 컨텍스트: qpair connect/accept 처리 중인 spdk_thread.
 *
 * 호출 체인:
 *   nvmf_tgt_accept (or 유사 경로) -> [본 함수] -> transport->ops->get_optimal_poll_group()
 */
struct spdk_nvmf_transport_poll_group *nvmf_transport_get_optimal_poll_group(
	struct spdk_nvmf_transport *transport, struct spdk_nvmf_qpair *qpair);

/*
 * [한국어]
 * nvmf_transport_poll_group_destroy - 트랜스포트 poll group 자원 해제
 *
 * @group: 해제 대상 transport poll group
 *
 * 상위 nvmf poll group이 파기될 때 각 트랜스포트별 group마다 호출되어
 * RDMA CQ/QP, TCP socket epoll, FC HWQP 핸들 등을 정리한다.
 * 실행 컨텍스트: 해당 poll group의 소유 spdk_thread.
 *
 * 호출 체인:
 *   spdk_nvmf_poll_group_destroy -> [본 함수] -> transport->ops->poll_group_destroy()
 */
void nvmf_transport_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group);

/*
 * [한국어]
 * nvmf_transport_poll_group_pause - poll group 일시 정지 (큐 폴링 중단)
 *
 * @group: 일시 정지할 transport poll group
 *
 * subsystem pause 같은 상위 동작 시 호출되어 트랜스포트가 새 명령 수신을 막는다.
 * 일반적으로 트랜스포트 콜백이 NULL이면 no-op이며, 파일/디스크 RDMA 등에서만 동작.
 * 실행 컨텍스트: 해당 poll group spdk_thread.
 */
void nvmf_transport_poll_group_pause(struct spdk_nvmf_transport_poll_group *group);

/*
 * [한국어]
 * nvmf_transport_poll_group_resume - poll group 일시 정지 해제
 *
 * @group: 재개할 transport poll group
 *
 * pause의 반대. subsystem resume 시 호출.
 * 실행 컨텍스트: 해당 poll group spdk_thread.
 */
void nvmf_transport_poll_group_resume(struct spdk_nvmf_transport_poll_group *group);

/*
 * [한국어]
 * nvmf_transport_poll_group_add - poll group에 qpair 등록
 *
 * @group: 대상 transport poll group
 * @qpair: 추가할 qpair (방금 connect 받은 새 qpair)
 * @return: 0 성공, 음수 errno 실패
 *
 * 호스트가 Connect 명령으로 새 qpair를 만들면 적절한 poll group에 add한다.
 * RDMA: QP를 group의 CQ에 바인딩, TCP: socket을 group의 epoll에 등록.
 * 등록 후부터 poll_group_poll()에서 이 qpair의 명령을 수신/처리하기 시작.
 * 실행 컨텍스트: poll group의 spdk_thread (cross-thread는 send_msg 후 호출).
 *
 * 호출 체인:
 *   spdk_nvmf_poll_group_add -> [본 함수] -> transport->ops->poll_group_add()
 */
int nvmf_transport_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
				  struct spdk_nvmf_qpair *qpair);

/*
 * [한국어]
 * nvmf_transport_poll_group_remove - poll group에서 qpair 제거
 *
 * @group: poll group
 * @qpair: 제거할 qpair (disconnect 또는 transport 오류 시)
 * @return: 0 성공, 음수 errno
 *
 * qpair_fini 직전에 호출되어 epoll/CQ에서 분리.
 * 실행 컨텍스트: poll group의 spdk_thread.
 */
int nvmf_transport_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				     struct spdk_nvmf_qpair *qpair);

/*
 * [한국어]
 * nvmf_transport_poll_group_poll - poll group의 모든 qpair에 대해 한 번 폴링
 *
 * @group: 폴링 대상 transport poll group
 * @return: 처리한 이벤트(완료) 개수 (0이면 idle, 양수면 busy)
 *
 * SPDK reactor의 무한 루프에서 spdk_poller_register로 등록된 콜백이 주기적으로 호출.
 * RDMA: ibv_poll_cq, TCP: epoll_wait를 통해 들어온 캡슐/완료를 수확하고 ctrlr 처리로 진입.
 * polled-mode이므로 인터럽트 없이 CPU 100%로 돈다 — 그 트레이드오프로 ns단위 지연 달성.
 * 실행 컨텍스트: 해당 reactor spdk_thread (절대 cross-thread 금지).
 *
 * 호출 체인:
 *   reactor 무한 루프 -> spdk_poller cb -> [본 함수] -> transport->ops->poll_group_poll()
 *     -> 각 qpair의 캡슐 수신 -> spdk_nvmf_request_exec()
 */
int nvmf_transport_poll_group_poll(struct spdk_nvmf_transport_poll_group *group);

/*
 * [한국어]
 * nvmf_transport_req_free - 사용 끝난 NVMe-oF 요청 객체 해제 (트랜스포트별)
 *
 * @req: 해제할 spdk_nvmf_request (트랜스포트 자원과 묶인 컨텍스트 포함)
 *
 * 요청이 완료되거나 abort된 후 트랜스포트별 자원(RDMA WR, TCP PDU 버퍼 등)을 풀로 반납.
 * spdk_nvmf_request_complete가 송신을 마치면 그 다음 단계에서 호출된다.
 * 실행 컨텍스트: 요청을 처리한 spdk_thread.
 */
void nvmf_transport_req_free(struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_transport_req_complete - 요청 응답(CQE)을 호스트로 전송
 *
 * @req: 응답할 요청 (req->rsp에 NVMe Completion이 채워져 있어야 함)
 *
 * ctrlr/bdev에서 명령 처리가 끝나 status가 결정되면 호출된다.
 * RDMA: SEND WR로 CQE 송신, TCP: PDU 작성 후 send, FC: ERSP IU 작성.
 * 실행 컨텍스트: 요청이 바인딩된 qpair의 poll group spdk_thread.
 *
 * 호출 체인:
 *   spdk_nvmf_request_complete -> [본 함수] -> transport->ops->req_complete()
 */
void nvmf_transport_req_complete(struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_transport_qpair_fini - qpair 비동기 해제 (트랜스포트별 정리)
 *
 * @qpair: 해제할 qpair
 * @cb_fn: 정리 완료 후 호출되는 콜백 (NULL 가능)
 * @cb_arg: 콜백 인자
 *
 * RDMA QP destroy, TCP socket close 등 트랜스포트 자원을 비동기로 정리한다.
 * 일부 자원(예: RDMA QP drain)은 즉시 끝나지 않으므로 콜백을 통해 완료 시점을 알린다.
 * 실행 컨텍스트: qpair 소유 spdk_thread.
 */
void nvmf_transport_qpair_fini(struct spdk_nvmf_qpair *qpair,
			       spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg);

/*
 * [한국어]
 * nvmf_transport_qpair_get_peer_trid - qpair의 원격(피어) 트랜스포트 주소 조회
 *
 * @qpair: 조회 대상 qpair
 * @trid: 출력 - 호스트 측 traddr/trsvcid가 채워짐
 * @return: 0 성공, 음수 errno
 *
 * 호스트(initiator)의 IP/포트나 FC WWN을 알아내야 할 때(로깅, ACL 검사) 사용.
 */
int nvmf_transport_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				       struct spdk_nvme_transport_id *trid);

/*
 * [한국어]
 * nvmf_transport_qpair_get_local_trid - qpair의 로컬(타겟) 트랜스포트 주소 조회
 *
 * @qpair: 조회 대상 qpair
 * @trid: 출력 - 타겟 측에서 사용 중인 traddr/trsvcid (실제 binding된 IP/포트)
 * @return: 0 성공, 음수 errno
 *
 * NIC 다중 IP 환경에서 어떤 인터페이스로 들어왔는지 식별할 때 유용.
 */
int nvmf_transport_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
					struct spdk_nvme_transport_id *trid);

/*
 * [한국어]
 * nvmf_transport_qpair_get_listen_trid - qpair가 들어온 listener의 trid 조회
 *
 * @qpair: 조회 대상 qpair
 * @trid: 출력 - 이 qpair를 accept한 listener에 등록된 trid
 * @return: 0 성공, 음수 errno
 *
 * local_trid와 다를 수 있다 (예: listener가 0.0.0.0:4420으로 등록됐으나 실제 local IP는 다름).
 * Discovery Log에 어떤 listener entry로 노출되는지 결정할 때 사용.
 */
int nvmf_transport_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid);

/*
 * [한국어]
 * nvmf_transport_qpair_abort_request - qpair 안의 특정 요청을 취소
 *
 * @qpair: 대상 qpair
 * @req: ABORT 명령에 의해 지정된, 취소되어야 할 요청 (다른 요청)
 *
 * 호스트가 NVMe Abort admin 명령을 보내면 ctrlr.c에서 처리되며, 트랜스포트 레벨에서
 * 이미 진행 중인 I/O를 중단해야 할 때 이 함수가 위임된다.
 * 실행 컨텍스트: qpair 소유 spdk_thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_abort_request -> [본 함수] -> transport->ops->qpair_abort_request()
 */
void nvmf_transport_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
					struct spdk_nvmf_request *req);

/*
 * [한국어]
 * nvmf_request_free_stripped_buffers - PI(메타데이터) strip 용 보조 버퍼 해제
 *
 * @req: 대상 요청
 * @group: 버퍼를 빌렸던 transport poll group
 * @transport: 트랜스포트 (mempool 소유자)
 *
 * dif_insert_or_strip가 활성화되면 데이터 + 메타데이터를 분리/병합하기 위한
 * 보조 버퍼를 트랜스포트 mempool에서 빌려 사용한다. 요청 완료 후 반납.
 */
void nvmf_request_free_stripped_buffers(struct spdk_nvmf_request *req,
					struct spdk_nvmf_transport_poll_group *group,
					struct spdk_nvmf_transport *transport);

/*
 * [한국어]
 * nvmf_request_get_stripped_buffers - PI strip 용 보조 버퍼 할당
 *
 * @req: 대상 요청
 * @group: 버퍼를 빌릴 transport poll group
 * @transport: mempool 소유자
 * @length: 필요한 페이로드 길이 (메타데이터 제외 데이터)
 * @return: 0 성공, 음수 실패 (mempool 고갈 등)
 *
 * 호출 즉시 buffer pool에서 빌려와 req->stripped_buffers에 채운다.
 * 실패 시 호출자가 request를 enqueue하고 나중에 재시도(get_buffers_abort 가능).
 */
int nvmf_request_get_stripped_buffers(struct spdk_nvmf_request *req,
				      struct spdk_nvmf_transport_poll_group *group,
				      struct spdk_nvmf_transport *transport,
				      uint32_t length);

/*
 * [한국어]
 * nvmf_request_get_buffers_abort - 버퍼 대기 큐에서 요청을 제거(abort)
 *
 * @req: 대상 요청
 * @return: true면 큐에서 찾아서 제거함, false면 이미 처리 중이거나 큐에 없음
 *
 * 트랜스포트는 buffer pool 고갈 시 요청을 buf_wait_link에 enqueue 후 mempool에 등록한
 * register_buffer_cb가 깨워준다. 그러나 qpair 종료/abort 시 이 대기열에 박혀있는
 * 요청을 제거할 필요가 있어 이 함수를 호출한다.
 * 실행 컨텍스트: poll group spdk_thread.
 */
bool nvmf_request_get_buffers_abort(struct spdk_nvmf_request *req);

#endif /* SPDK_NVMF_TRANSPORT_H */                  /* [한국어] 헤더 가드 종료 */
