/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] JSON-RPC 라이브러리 내부 공용 헤더 (jsonrpc_internal.h)
 *
 * === 파일의 역할 ===
 * lib/jsonrpc/ 디렉토리의 4개 .c 파일(jsonrpc_server.c, jsonrpc_server_tcp.c,
 * jsonrpc_client.c, jsonrpc_client_tcp.c)이 공유하는 내부 자료구조·매크로·
 * 함수 프로토타입을 정의한다. include/spdk/jsonrpc.h가 외부 사용자에게
 * "불투명 핸들"로만 노출한 spdk_jsonrpc_server / _server_conn / _request /
 * _client / _client_request / _client_response_internal 의 실제 필드 레이아웃을
 * 이 파일에서 펼쳐 놓는다. 또한 송수신 버퍼 크기, 최대 연결/JSON 토큰 개수
 * 등 라이브러리 전반의 컴파일타임 상한을 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 본 헤더는 lib/jsonrpc/ 내부에서만 #include 된다(외부에서 include 불가, 빌드
 * 시스템이 경로를 노출하지 않음). 호출 흐름:
 *   외부 사용자(spdk/rpc.h, app/spdk_tgt 등) → spdk/jsonrpc.h 공개 API →
 *   lib/jsonrpc/*.c (이 헤더의 구조체를 직접 다룸) → 소켓 시스템 호출.
 * 실행 컨텍스트는 단일 SPDK app thread (대개 main reactor의 default poller).
 * 본 헤더가 정의하는 모든 자료구조는 그 thread에서만 접근되며, 유일한
 * cross-thread 경로는 jsonrpc_server_send_response()(주석에 "Might be called
 * from any thread"로 명시)로, conn->queue_lock(spinlock)으로 보호된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/stdinc.h(POSIX 타입, STAILQ/TAILQ 매크로), spdk/jsonrpc.h
 * (공개 API의 typedef·매크로·콜백 시그니처), spdk/log.h(SPDK_ERRLOG/DEBUGLOG).
 * 데이터 흐름:
 *   server 측: socket recv → conn->recv_buf → spdk_json_parse →
 *              request->values → handle_request 콜백 → request->response
 *              (spdk_json_write_ctx) → request->send_buf → socket send.
 *   client 측: spdk_json_write_ctx → client_request->send_buf → socket send →
 *              socket recv → client->recv_buf → spdk_json_parse →
 *              client_response_internal->values → 사용자 파서.
 * 공유 자료구조:
 *   spdk_jsonrpc_server는 conns_array[MAX_CONNS]로 모든 연결 슬롯을 정적
 *   할당하고 free_conns/conns 두 TAILQ로 free·active를 분리한다.
 *   spdk_jsonrpc_server_conn은 outstanding_queue(처리 중)·send_queue(송신 대기)
 *   두 STAILQ를 가지며, queue_lock으로 보호된다 — 핸들러가 다른 thread에서
 *   완료 응답을 큐잉할 수 있기 때문.
 *
 * === 주요 함수/구조체 요약 ===
 *   - SPDK_JSONRPC_RECV_BUF_SIZE / SEND_BUF_SIZE_INIT/MAX: 32KB recv 고정,
 *     32KB→32MB까지 동적 증가 send 버퍼.
 *   - SPDK_JSONRPC_MAX_CONNS=64: 동시 연결 슬롯 정적 할당 개수 상한.
 *   - SPDK_JSONRPC_MAX_VALUES=1024(server) / 8192(client): 단일 RPC에서
 *     spdk_json_parse가 만들어낼 수 있는 토큰 최대 개수(요청보다 응답이 큼).
 *   - struct spdk_jsonrpc_request: 한 건의 RPC 요청 + 응답 buf/writer 컨텍스트.
 *   - struct spdk_jsonrpc_server_conn: 한 클라이언트와의 TCP/UDS 연결.
 *   - struct spdk_jsonrpc_server: listen 소켓 + conn 슬롯 풀.
 *   - struct spdk_jsonrpc_client / _request / _response_internal: 클라이언트.
 *   - jsonrpc_server_handle_request/_handle_error: server.c가 정의, server_tcp.c
 *     가 dispatch에 사용 (역방향: tcp가 server core로 진입).
 *   - jsonrpc_server_send_response: server.c가 호출(스레드 무관), tcp.c가 큐 처리.
 *   - jsonrpc_parse_request / _parse_response: 양쪽 코어 파서 함수.
 *   - jsonrpc_free_request / _complete_request: 요청 메모리 해제 (server thread).
 */

#ifndef SPDK_JSONRPC_INTERNAL_H_  /* [한국어] include 가드 — 중복 포함 방지 */
#define SPDK_JSONRPC_INTERNAL_H_

#include "spdk/stdinc.h"          /* [한국어] POSIX 타입·sys/queue.h(STAILQ/TAILQ)·socket 타입 import */

#include "spdk/jsonrpc.h"         /* [한국어] 공개 API의 typedef(handle_request_fn 등) — 내부 구조체와 결합 */

#include "spdk/log.h"             /* [한국어] SPDK_ERRLOG/DEBUGLOG/WARNLOG/SPDK_LOG_REGISTER_COMPONENT 매크로 */

/* [한국어] 한 connection의 수신 버퍼 크기(32KB, 고정).
 * recv()로 한 번에 읽어들여 staging하는 ring 형태 버퍼. JSON-RPC 한 건은
 * 보통 수백 바이트~수 KB이지만, 큰 명령(예: NVMe controller list)은 더 클 수
 * 있어 32KB로 잡혀 있다. 한 요청이 이보다 크면 partial 처리 후 memmove로 압축. */
#define SPDK_JSONRPC_RECV_BUF_SIZE	(32 * 1024)

/* [한국어] 송신 버퍼 초기 크기(32KB). spdk_json_write_ctx가 응답을 직렬화하면서
 * 이 버퍼에 누적된다. 부족하면 jsonrpc_server_write_cb()가 2배씩 realloc. */
#define SPDK_JSONRPC_SEND_BUF_SIZE_INIT	(32 * 1024)

/* [한국어] 송신 버퍼 최대 크기(32MB). 이를 초과하는 응답은 거부 — RPC 메시지가
 * 비정상적으로 크면 보통 서버 핸들러 버그(무한 루프 출력)이거나 악의적 입력.
 * spdk_get_iostat 같은 광범위 응답도 32MB 안에 들어와야 한다. */
#define SPDK_JSONRPC_SEND_BUF_SIZE_MAX	(32 * 1024 * 1024)

/* [한국어] 요청 id 문자열 최대 길이(미사용 — 코드상 직접 검사 없음, 향후 확장용). */
#define SPDK_JSONRPC_ID_MAX_LEN		128

/* [한국어] 한 server가 동시에 수용할 수 있는 client 연결 최대 개수(64).
 * spdk_jsonrpc_server::conns_array[]가 이 크기로 정적 할당된다. 64를 넘는
 * 클라이언트는 accept() 단계에서 거부 — RPC는 control plane이라 1~수 개의
 * 동시 연결만 예상되므로 충분. */
#define SPDK_JSONRPC_MAX_CONNS		64

/* [한국어] 단일 server 측 요청에서 spdk_json_parse가 만들 수 있는 최대 토큰 수.
 * "토큰"은 JSON value(object_begin, string, number, …)의 나열. 1024는 보통의
 * RPC 요청에 충분하나, 수신 측은 작게 잡고 송신(client 응답)은 크게 잡는다. */
#define SPDK_JSONRPC_MAX_VALUES		1024

/* [한국어] 클라이언트 측 응답 파싱 시 토큰 최대 개수(8192). 응답에는 디바이스
 * 리스트, 통계 dump 등 큰 결과가 포함되므로 server 요청 한도(1024)보다 8배 크다. */
#define SPDK_JSONRPC_CLIENT_MAX_VALUES		8192

/*
 * [한국어]
 * struct spdk_jsonrpc_request - 진행 중인 server측 RPC 요청 1건의 모든 상태
 *
 * 한 클라이언트가 보낸 단일 JSON-RPC 요청에 대해 (1) 수신한 raw byte 사본,
 * (2) 파싱된 JSON value 토큰 배열, (3) 핸들러가 작성 중인 응답 buf와 writer
 * 컨텍스트, (4) 어느 conn에 속한 요청인지를 한데 묶어 관리한다.
 *
 * 생애주기:
 *   jsonrpc_parse_request()가 calloc — outstanding_queue에 등록 (recv 시점) →
 *   handle_request 콜백 dispatch → 핸들러가 begin_result/end_result로 응답 작성 →
 *   spdk_jsonrpc_end_result() → jsonrpc_server_send_response()가 send_queue로 이동 →
 *   jsonrpc_server_conn_send()가 socket으로 보낸 후 jsonrpc_complete_request() →
 *   jsonrpc_free_request()로 모든 buf free.
 *
 * 동시성: link로 두 큐(outstanding/send)에 걸리는 동안에는 conn->queue_lock으로
 * 보호. 핸들러 본체가 응답을 작성하는 동안에는 그 핸들러 thread만 접근(보통
 * server thread이지만 비동기 핸들러는 다른 thread에서 완료 후 send_response 호출
 * 가능). 실제 메모리 read/write는 락 없이 한 thread만 다룬다는 가정.
 */
struct spdk_jsonrpc_request {
	struct spdk_jsonrpc_server_conn *conn;
	/* [한국어] 이 요청이 도착한 server-side 연결의 역참조 포인터.
	 * 설정자: jsonrpc_parse_request()가 conn 인자로 받은 값을 저장.
	 * 읽는 자: 응답 송신 시 어느 socket으로 send 할지, 어느 send_queue에
	 *         enqueue 할지 결정하기 위해 jsonrpc_server_send_response() 등이 사용.
	 * 값 범위: 유효한 conn 포인터. 단, 연결이 먼저 닫히면(jsonrpc_server_free_conn_request)
	 *         NULL로 강제 변경되어 응답 송신 불가 상태를 표시.
	 * 동기화: conn->queue_lock 하에서 NULL로 바뀔 수 있음 — 핸들러는 send_response
	 *         호출 시 다시 NULL 검사를 수행. */

	/* Copy of request id value */
	const struct spdk_json_val *id;
	/* [한국어] JSON-RPC 요청의 "id" 필드를 가리키는 토큰 포인터(혹은 NULL=notification).
	 * 설정자: parse_single_request()가 jsonrpc_request_decoders 디코딩 후 저장.
	 * 읽는 자: begin_response()가 응답 객체에 같은 id를 echo하기 위해, 그리고
	 *         spdk_jsonrpc_end_result()가 id의 유무/타입에 따라 응답을 송신할지
	 *         skip(notification)할지 결정하기 위해 읽는다.
	 * 값 범위: SPDK_JSON_VAL_STRING / NUMBER / NULL 중 하나, 또는 NULL 포인터.
	 *         NULL 포인터 = 요청 객체에 id 필드가 없음(notification per JSON-RPC 2.0).
	 * 동기화: request 메모리(values 배열)에 대한 in-place 디코딩 결과 — 단일 thread. */

	/* Total space allocated for send_buf */
	size_t send_buf_size;
	/* [한국어] send_buf의 현재 할당 크기(bytes). 초기 SEND_BUF_SIZE_INIT(32KB).
	 * 설정자: jsonrpc_parse_request()가 초기화, jsonrpc_server_write_cb()가 부족 시
	 *         2배씩 키우며 realloc.
	 * 읽는 자: write_cb가 매 write 시 잔여 공간(send_buf_size - send_len)을 확인.
	 * 값 범위: SEND_BUF_SIZE_INIT(32KB) ~ SEND_BUF_SIZE_MAX(32MB).
	 * 동기화: 단일 thread에서만 다뤄짐. */

	/* Number of bytes used in send_buf (<= send_buf_size) */
	size_t send_len;
	/* [한국어] 응답 직렬화로 채워진 유효 byte 수.
	 * 설정자: jsonrpc_server_write_cb()가 누적, end_response()가 \n까지 추가.
	 *         jsonrpc_server_conn_send()는 send() 한 만큼 감소시킴.
	 * 읽는 자: tcp send 루프가 송신할 잔여 바이트 수로 사용, jsonrpc_reset_response()는
	 *         에러 응답으로 갈 때 누적된 일부 출력을 폐기하기 위해 0으로 리셋.
	 * 값 범위: 0 ~ send_buf_size.
	 * 동기화: 큐 이동 시 queue_lock(spinlock) 보호 — 실제 byte 조작은 단일 thread. */

	size_t send_offset;
	/* [한국어] tcp send 루프에서 다음 send() 호출이 사용할 buf 내 시작 오프셋.
	 * 설정자: jsonrpc_parse_request()가 0으로 초기화, jsonrpc_server_conn_send()가
	 *         부분 송신 시 send() 결과만큼 증가.
	 * 읽는 자: send(sockfd, send_buf + send_offset, send_len, 0).
	 * 값 범위: 0 ~ send_buf_size.
	 * 동기화: send_request로 잡힌 후에는 server poll thread만 다룸. */

	uint8_t *recv_buffer;
	/* [한국어] 수신 raw JSON 텍스트의 사본(파싱 시 in-place로 NUL 채워짐).
	 * 설정자: jsonrpc_parse_request()가 conn->recv_buf에서 len+1 바이트 복사.
	 * 읽는 자: spdk_json_parse가 IN_PLACE 모드로 디코딩 — values 배열의 string 토큰들이
	 *         이 buf 내부를 가리킴. jsonrpc_log()가 trace 출력에도 사용.
	 * 값 범위: 유효 malloc 포인터(NULL이면 free_request 단계에서 실패 분기).
	 * 동기화: 단일 thread. */

	struct spdk_json_val *values;
	/* [한국어] 파싱된 JSON 토큰 배열(spdk_json_parse 결과).
	 * 설정자: jsonrpc_parse_request()가 SPDK_JSONRPC_MAX_VALUES 한도 내로 malloc.
	 * 읽는 자: parse_single_request, spdk_json_decode_object 등이 토큰 트리를 따라가며 디코딩.
	 * 값 범위: NULL(파싱 실패 분기) 또는 values_cnt개의 유효 토큰.
	 * 동기화: 단일 thread. */

	size_t values_cnt;
	/* [한국어] values 배열에 들어 있는 토큰 개수.
	 * 설정자: jsonrpc_parse_request()가 spdk_json_parse 1차 호출 결과 저장.
	 * 읽는 자: spdk_json_decode_object 등에 SPDK_COUNTOF 대신 전달.
	 * 값 범위: 1 ~ SPDK_JSONRPC_MAX_VALUES(1024).
	 * 동기화: 단일 thread. */

	uint8_t *send_buf;
	/* [한국어] 송신 응답 버퍼(응답 JSON 직렬화 결과 + 끝의 \n + null term).
	 * 설정자: jsonrpc_parse_request()가 SEND_BUF_SIZE_INIT+1 바이트 malloc,
	 *         write_cb가 필요 시 realloc.
	 * 읽는 자: jsonrpc_server_conn_send()의 send() 시스템 호출 source.
	 * 값 범위: 유효 malloc 포인터.
	 * 동기화: 단일 thread (write 시점 = 핸들러 thread, send 시점 = poll thread). */

	struct spdk_json_write_ctx *response;
	/* [한국어] 응답 JSON을 시리얼라이즈하는 writer 컨텍스트(spdk/json.h 정의).
	 * 설정자: jsonrpc_parse_request()가 spdk_json_write_begin(write_cb=jsonrpc_server_write_cb)
	 *         으로 생성. 사용 종료 시 end_response/skip_response가 NULL로 만든다.
	 *         (NULL 포인터는 "응답 작성 종료" 사후 조건 — assert로 검증).
	 * 읽는 자: spdk_jsonrpc_begin_result()가 핸들러에 노출하는 writer.
	 * 값 범위: 유효 ctx 포인터 또는 NULL(작성 종료 후).
	 * 동기화: 단일 thread (응답을 작성하는 핸들러 thread). */

	STAILQ_ENTRY(spdk_jsonrpc_request) link;
	/* [한국어] conn->outstanding_queue 또는 conn->send_queue 한쪽에 끼우기 위한
	 * sys/queue.h STAILQ entry.
	 * 설정자/읽는 자: STAILQ_INSERT_TAIL/REMOVE 매크로가 조작.
	 * 값 범위: 어느 시점이든 두 큐 중 정확히 한 곳에 속함.
	 * 동기화: queue_lock(pthread_spinlock) 하에서만 조작 — 핸들러가 다른 thread에서
	 *        send_response를 호출해도 안전하게 send_queue로 이동 가능. */
};

/*
 * [한국어]
 * struct spdk_jsonrpc_server_conn - 한 클라이언트와의 단일 TCP/UDS 연결 상태
 *
 * accept() 결과로 만들어진 socket fd 1개에 대응. recv 측 ring 버퍼,
 * 처리 중/송신 대기 요청의 두 큐, 종료 콜백, 상위 server 역참조를 담는다.
 *
 * 생애주기:
 *   server.conns_array[]에서 미사용 슬롯을 free_conns에서 가져옴 →
 *   jsonrpc_server_accept()가 sockfd/큐/spinlock 초기화, conns 리스트로 이동 →
 *   poll 루프에서 recv/send/parse 반복 → closed=true 후 outstanding_requests==0이면
 *   jsonrpc_server_conn_close() (close_cb 호출, sockfd close) →
 *   jsonrpc_server_conn_remove() (큐 정리, free_conns로 반환).
 *
 * 동시성: 모든 socket I/O와 큐 traverse는 server poll thread에서 수행.
 * 단, send_response는 핸들러가 다른 thread에서 호출 가능 — queue_lock으로
 * outstanding_queue↔send_queue 이동을 보호한다. recv_buf와 send_buf는 한 thread만 다룸.
 */
struct spdk_jsonrpc_server_conn {
	struct spdk_jsonrpc_server *server;
	/* [한국어] 이 연결을 소유한 server의 역참조.
	 * 설정자: jsonrpc_server_accept()가 server 인자를 저장.
	 * 읽는 자: jsonrpc_server_conn_remove()가 free/active 리스트 이동에 server를 본다,
	 *         jsonrpc_server_handle_request()가 server->handle_request 콜백을 디스패치.
	 * 값 범위: 유효한 server 포인터 (NULL 불가).
	 * 동기화: 한 번 설정 후 변경 없음 — 락 불필요. */

	int sockfd;
	/* [한국어] 이 연결의 소켓 디스크립터(연결됨 = >=0, 닫힘 = -1).
	 * 설정자: jsonrpc_server_accept()가 accept() 반환값으로 설정,
	 *         jsonrpc_server_conn_close()가 close() 후 -1로 설정.
	 * 읽는 자: recv/send 시스템 호출 인자, poll 루프가 -1일 경우 skip.
	 * 값 범위: -1 (닫힘) 또는 유효 fd.
	 * 동기화: server poll thread만 다룸 — 응답 송신 시점에 한 thread에서만 close 가능. */

	bool closed;
	/* [한국어] 클라이언트가 정상 종료(recv 0)했거나 에러로 닫혀야 함을 표시.
	 * 설정자: jsonrpc_server_conn_recv()가 recv()==0일 때 true, conn_close가 true.
	 * 읽는 자: poll 루프가 closed && outstanding_requests==0이면 conn_close 진행.
	 * 값 범위: false / true.
	 * 동기화: 단일 thread(server poll). */

	size_t recv_len;
	/* [한국어] recv_buf 내 미파싱 잔여 byte 길이.
	 * 설정자: recv()가 받은 만큼 더하고, 파싱 후 memmove로 압축하면서 감소.
	 * 읽는 자: 다음 recv() 시 잔여 공간 계산 (RECV_BUF_SIZE - recv_len).
	 * 값 범위: 0 ~ SPDK_JSONRPC_RECV_BUF_SIZE.
	 * 동기화: server poll thread 단독 접근. */

	uint8_t recv_buf[SPDK_JSONRPC_RECV_BUF_SIZE];
	/* [한국어] 부분 수신용 staging 버퍼(32KB, conn 구조체 내부에 inline).
	 * 설정자: recv()가 추가로 채움.
	 * 읽는 자: jsonrpc_parse_request()가 여기서 raw JSON 텍스트를 읽어 파싱,
	 *         완전 파싱된 만큼 memmove로 buf 시작으로 압축.
	 * 값 범위: recv_len 만큼이 유효 byte.
	 * 동기화: 단일 thread. */

	uint32_t outstanding_requests;
	/* [한국어] 이 연결에서 아직 응답을 보내지 못한 처리 중 요청의 개수.
	 * 설정자: jsonrpc_parse_request()가 ++, jsonrpc_free_request()가 --.
	 * 읽는 자: poll 루프가 closed&&outstanding==0이면 conn 종료 단계 진입,
	 *         jsonrpc_server_conn_send()가 0이면 send loop short-circuit 종료.
	 * 값 범위: 0 ~ 무제한(클라이언트가 batch로 보낼 수 있음).
	 * 동기화: queue_lock(spinlock) 하에서 ++/-- — 비동기 핸들러의 free_request도 안전. */

	pthread_spinlock_t queue_lock;
	/* [한국어] outstanding_queue/send_queue/outstanding_requests 보호용 spinlock.
	 * Spinlock은 RPC 큐 조작이 매우 짧고 cross-thread 호출이 드물어 mutex보다 적합.
	 * 설정자: jsonrpc_server_accept()가 PTHREAD_PROCESS_PRIVATE로 init,
	 *         jsonrpc_server_conn_remove()가 destroy.
	 * 읽는 자: 모든 큐 INSERT/REMOVE/FOREACH가 lock/unlock으로 감쌈.
	 * 동기화: 보호 대상은 위 두 큐와 outstanding_requests, close_cb 한정. */

	STAILQ_HEAD(, spdk_jsonrpc_request) send_queue;
	/* [한국어] 응답이 완성되어 socket 송신을 기다리는 요청의 FIFO 큐.
	 * 설정자: jsonrpc_server_send_response()가 outstanding→send로 이동.
	 * 읽는 자: jsonrpc_server_dequeue_request()가 head를 꺼내 socket으로 송신.
	 * 동기화: queue_lock 필수. */

	/* List of incomplete requests that are not yet ready to be sent.
	 * This is a safety net for cases, where server shutdown is called
	 * before all requests are placed into send_queue. */
	STAILQ_HEAD(, spdk_jsonrpc_request) outstanding_queue;
	/* [한국어] 핸들러에 디스패치되었지만 응답 작성이 끝나지 않은 요청들.
	 * 설정자: jsonrpc_parse_request()가 INSERT_TAIL,
	 *         jsonrpc_server_send_response()가 REMOVE 후 send_queue로 이동.
	 * 읽는 자: jsonrpc_server_free_conn_request()가 conn 강제 종료 시
	 *         FOREACH로 conn=NULL 마킹(요청은 핸들러가 free하도록 둠).
	 * 동기화: queue_lock 필수. */

	struct spdk_jsonrpc_request *send_request;
	/* [한국어] 현재 partial-send 중인 요청 포인터(부분 send에 대한 cursor).
	 * 설정자: jsonrpc_server_conn_send()가 dequeue 결과를 저장,
	 *         완전 송신 후 NULL로 리셋.
	 * 읽는 자: 다음 conn_send 호출이 NULL인지 확인 후 새로 dequeue.
	 * 값 범위: NULL(idle) 또는 유효 request 포인터.
	 * 동기화: server poll thread만 다룸 — 큐에서 빠져나온 후의 cursor이므로 락 불필요. */

	spdk_jsonrpc_conn_closed_fn close_cb;
	/* [한국어] 연결 종료 시 1회 호출되는 사용자 콜백(예: rpc 등록표 정리).
	 * 설정자: spdk_jsonrpc_conn_add_close_cb()가 등록(이미 있으면 EEXIST/ENOSPC).
	 * 읽는 자: jsonrpc_server_conn_close()가 sockfd close 직후 호출.
	 * 값 범위: NULL(미등록) 또는 사용자 함수 포인터.
	 * 동기화: queue_lock 하에서 set/unset. */

	void *close_cb_ctx;
	/* [한국어] close_cb에 전달할 user context.
	 * 설정자/읽는 자/동기화: close_cb와 동일. */

	TAILQ_ENTRY(spdk_jsonrpc_server_conn) link;
	/* [한국어] server->free_conns 또는 server->conns 한쪽에 걸리기 위한 TAILQ entry.
	 * accept 시 free→conns 이동, conn_remove 시 conns→free 이동.
	 * 동기화: server poll thread만 조작(외부 thread에서 conn 리스트를 만지지 않음). */
};

/*
 * [한국어]
 * struct spdk_jsonrpc_server - JSON-RPC server 인스턴스 1개의 모든 상태
 *
 * listen 소켓 + 사용자 핸들러 콜백 + 정적 conn 풀(64개)을 보유.
 * spdk_jsonrpc_server_listen()이 calloc, spdk_jsonrpc_server_shutdown()이 free.
 * 인스턴스는 SPDK app thread 1개에서만 사용된다(공개 헤더 NOTE 참조).
 */
struct spdk_jsonrpc_server {
	int sockfd;
	/* [한국어] listen 소켓 fd. 도메인은 AF_UNIX 또는 AF_INET/INET6,
	 * SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC.
	 * 설정자: spdk_jsonrpc_server_listen()이 socket()/bind()/listen() 후 저장.
	 * 읽는 자: jsonrpc_server_accept()가 accept() 인자, server_shutdown이 close().
	 * 동기화: 단일 thread. */

	spdk_jsonrpc_handle_request_fn handle_request;
	/* [한국어] 메서드 디스패치 콜백 — RPC 요청 처리의 진입점(주로 spdk/rpc.h 등록표 lookup).
	 * 설정자: server_listen() 인자.
	 * 읽는 자: jsonrpc_server_handle_request()가 conn->server->handle_request로 호출.
	 * 동기화: 한 번 설정 후 read-only — 락 불필요. */

	TAILQ_HEAD(, spdk_jsonrpc_server_conn) free_conns;
	/* [한국어] 비어 있는 conn 슬롯의 자유 리스트(LIFO 처럼 head 사용).
	 * conns_array[i] 들이 server_listen 시 모두 여기로 push되고, accept 시 빠져나감.
	 * 동기화: server poll thread 단독. */

	TAILQ_HEAD(, spdk_jsonrpc_server_conn) conns;
	/* [한국어] 활성 연결 리스트. accept 후 추가, conn_remove 시 제거.
	 * 동기화: server poll thread 단독. */

	struct spdk_jsonrpc_server_conn conns_array[SPDK_JSONRPC_MAX_CONNS];
	/* [한국어] 정적으로 할당된 conn 슬롯 풀(64개). 동적 alloc/free 비용 회피와
	 * 메모리 단편화 방지를 위해 인덱스 풀 형태. free_conns/conns가 이 배열의 원소들을 참조. */
};

/*
 * [한국어]
 * struct spdk_jsonrpc_client_request - 클라이언트가 보낼 단일 요청의 직렬화 버퍼
 *
 * spdk_jsonrpc_client_create_request()가 calloc + send_buf malloc,
 * spdk_jsonrpc_begin_request()/end_request()로 spdk_json_write_ctx를 통해 채워지고,
 * spdk_jsonrpc_client_send_request()가 client->request 슬롯에 등록 → poll 루프에서
 * send() 후 free.
 */
struct spdk_jsonrpc_client_request {
	/* Total space allocated for send_buf */
	size_t send_buf_size;
	/* [한국어] send_buf의 현재 할당 크기.
	 * 설정자: create_request가 SEND_BUF_SIZE_INIT으로, jsonrpc_client_write_cb가 부족 시 2배.
	 * 읽는 자: write_cb의 capacity 검사. */

	/* Number of bytes used in send_buf (<= send_buf_size) */
	size_t send_len;
	/* [한국어] 직렬화로 채워진 유효 byte 수. send() 한 만큼 감소.
	 * 설정자: write_cb가 누적, jsonrpc_client_send_request()가 send 결과만큼 감소. */

	size_t send_offset;
	/* [한국어] 다음 send() 호출이 시작할 buf 내 오프셋(부분 송신 cursor).
	 * 0에서 시작, 부분 send 시 가산. */

	uint8_t *send_buf;
	/* [한국어] 직렬화 버퍼. send()의 source. SEND_BUF_SIZE_INIT으로 시작, 2배씩 확장. */
};

/*
 * [한국어]
 * struct spdk_jsonrpc_client_response_internal - 파싱 완료된 응답의 내부 표현
 *
 * 외부에 노출되는 spdk_jsonrpc_client_response를 첫 필드로 임베드하여,
 * SPDK_CONTAINEROF로 외부→내부 캐스팅이 가능하다(spdk_jsonrpc_client_free_response 패턴).
 * values는 가변 길이 배열 — calloc 시 sizeof(*r) + sizeof(spdk_json_val) * (cnt+1)로 잡음.
 */
struct spdk_jsonrpc_client_response_internal {
	struct spdk_jsonrpc_client_response jsonrpc;
	/* [한국어] 사용자에게 노출되는 응답 객체(version/id/result/error 토큰 포인터들).
	 * 첫 필드여야 SPDK_CONTAINEROF가 작동. */

	bool ready;
	/* [한국어] 응답 파싱이 끝나 사용자에게 반환할 준비가 됐는지 여부.
	 * 설정자: jsonrpc_parse_response()가 성공 시 true.
	 * 읽는 자: spdk_jsonrpc_client_get_response()가 NULL 반환 여부 결정에 사용. */

	uint8_t *buf;
	/* [한국어] 파싱 in-place 대상이 되는 raw JSON 텍스트(client->recv_buf의 소유권 이전).
	 * spdk_json_val의 string 포인터가 이 buf 내부를 가리키므로 응답 free 전까지 유지. */

	size_t values_cnt;
	/* [한국어] values 배열의 토큰 개수 — calloc 시 정해짐. */

	struct spdk_json_val values[];
	/* [한국어] 가변 길이 토큰 배열(C99 flexible array member). 응답 JSON 트리. */
};

/*
 * [한국어]
 * struct spdk_jsonrpc_client - JSON-RPC client 인스턴스
 *
 * 한 서버와의 단일 TCP/UDS 연결을 다룸. send 슬롯과 recv 슬롯이 각각 1개씩이어서
 * 동시에 1개의 in-flight 요청만 가능 — 사용자가 응답을 받기 전에 다음 요청을
 * send_request로 등록하려 하면 -ENOSPC 반환.
 */
struct spdk_jsonrpc_client {
	int sockfd;
	/* [한국어] 서버에 연결된 socket fd. -1이면 close됨.
	 * 설정자: jsonrpc_client_connect()가 socket()/connect() 후 저장. */

	bool connected;
	/* [한국어] connect()가 완료되어 송수신 가능한 상태인지.
	 * non-blocking connect의 EINPROGRESS 단계에서는 false, poll로 확인 후 true. */

	size_t recv_buf_size;
	/* [한국어] recv_buf의 현재 할당 크기. 부족 시 recv_buf_expand가 2배 realloc. */

	size_t recv_offset;
	/* [한국어] recv_buf 내 누적된 byte 수(파싱 진행 위치 추적용).
	 * recv()마다 가산, 응답 파싱이 완료되면 buf 소유권을 resp로 넘기고 0으로 리셋. */

	char *recv_buf;
	/* [한국어] 응답 raw JSON 텍스트 누적 버퍼. SEND_BUF_SIZE_INIT(32KB)부터 시작, 2배씩 확장.
	 * 파싱 완료 후 resp->buf로 이전되고 client->recv_buf는 NULL로 — 다음 응답에서 재할당. */

	/* Parsed response */
	struct spdk_jsonrpc_client_response_internal *resp;
	/* [한국어] 파싱 완료된 응답(미수령 상태). 사용자가 get_response()로 가져가기 전까지 보관.
	 * 설정자: jsonrpc_parse_response()가 calloc 후 저장, get_response가 client->resp=NULL로 비움.
	 * 1개 슬롯뿐이므로 동시 in-flight = 1. */

	struct spdk_jsonrpc_client_request *request;
	/* [한국어] 현재 송신 중인 요청(부분 send cursor).
	 * 설정자: spdk_jsonrpc_client_send_request()가 1개 슬롯에 한해 등록 — 이미 있으면 -ENOSPC.
	 * 읽는 자: jsonrpc_client_send_request()가 socket으로 byte 송신, 완료 시 free 후 NULL. */
};

/* jsonrpc_server_tcp */
/* [한국어] 아래 두 함수는 jsonrpc_server.c에서 jsonrpc_parse_request()가 호출하여
 * 서버 코어의 메서드 디스패치/에러 응답 작성을 트리거한다.
 * 정의는 jsonrpc_server_tcp.c가 아니라 같은 파일(jsonrpc_server.c) 내부에 있는 게 아니라
 * jsonrpc_server_tcp.c에 있다 — server.c는 transport-agnostic 파싱·응답 빌드만 담당하고,
 * tcp.c는 transport별 디스패치 stub과 큐 관리를 책임진다. */
void jsonrpc_server_handle_request(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *method,
				   const struct spdk_json_val *params);
/* [한국어] 파싱 성공한 요청을 server->handle_request 사용자 콜백으로 전달한다.
 *   request: parse_single_request가 갓 채운 객체.
 *   method: SPDK_JSON_VAL_STRING 토큰 — RPC 메서드 이름(예 "bdev_get_bdevs").
 *   params: SPDK_JSON_VAL_OBJECT_BEGIN/ARRAY_BEGIN 또는 NULL(파라미터 없음). */

void jsonrpc_server_handle_error(struct spdk_jsonrpc_request *request, int error);
/* [한국어] 파싱 단계 또는 invalid request 단계에서 에러 응답을 송신.
 * error 코드(SPDK_JSONRPC_ERROR_*)에 따른 표준 메시지 문자열을 골라
 * spdk_jsonrpc_send_error_response()로 응답 객체를 작성한다. */

/* Might be called from any thread */
void jsonrpc_server_send_response(struct spdk_jsonrpc_request *request);
/* [한국어] 응답 작성 완료 후 outstanding_queue → send_queue로 옮기는 함수.
 * 비동기 핸들러가 다른 thread에서 완료를 알릴 수 있어 어느 thread에서나 호출 가능 —
 * 내부에서 conn->queue_lock(spinlock)으로 큐 이동을 보호한다.
 * conn==NULL(연결이 이미 닫힘)이면 free_request만 수행 후 종료. */

/* jsonrpc_server */
int jsonrpc_parse_request(struct spdk_jsonrpc_server_conn *conn, const void *json,
			  size_t size);
/* [한국어] recv_buf의 한 chunk에서 완전한 JSON-RPC 요청 1건을 파싱.
 * 반환값: 소비한 byte 수(>0), 0=incomplete(더 받아야 함), -1=치명적 파싱 에러(연결 종료).
 * 한 번에 1건만 처리하므로 server_tcp.c의 do/while 루프가 반복 호출. */

/* Must be called only from server poll thread */
void jsonrpc_free_request(struct spdk_jsonrpc_request *request);
/* [한국어] request의 모든 buf와 자체를 해제하고 outstanding_queue에서 제거.
 * conn->queue_lock 하에서 outstanding_requests--와 큐 제거를 수행.
 * 호출 thread 제약 이유: conn 라이프사이클이 server poll thread에 묶여 있어
 * cross-thread free는 use-after-free 위험. */

/* Must be called only from server poll thread */
void jsonrpc_complete_request(struct spdk_jsonrpc_request *request);
/* [한국어] 응답 송신 완료 후의 마무리 — trace 로깅 후 free_request.
 * server_tcp.c의 conn_send 루프가 호출. */

/*
 * Parse JSON data as RPC command response.
 *
 * \param client structure pointer of jsonrpc client
 *
 * \return 0 On success. Negative error code in error
 * -EAGAIN - If the provided data is not a complete JSON value (SPDK_JSON_PARSE_INCOMPLETE)
 * -EINVAL - If the provided data has invalid JSON syntax and can't be parsed (SPDK_JSON_PARSE_INVALID).
 * -ENOSPC - No space left to store parsed response.
 */
int jsonrpc_parse_response(struct spdk_jsonrpc_client *client);
/* [한국어] 클라이언트 측 응답 파싱. recv_buf의 누적 데이터를 spdk_json_parse(IN_PLACE)로
 * 변환 후 client->resp에 보관. INCOMPLETE면 0 반환(더 recv 필요), 치명적이면 음수,
 * 성공 시 1 반환(상위 jsonrpc_client_resp_ready_count에서 ready 체크에 사용). */

#endif
