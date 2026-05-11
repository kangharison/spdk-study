/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SCSI 서브시스템 JSON-RPC 핸들러 (scsi_rpc.c)
 *
 * === 파일의 역할 ===
 * SPDK SCSI 서브시스템에 노출되는 JSON-RPC 메서드를 등록하고 처리한다.
 * 현재 구현된 RPC는 "scsi_get_devices" 한 가지로, dev.c가 관리하는
 * 전역 g_devs[] 배열을 순회하며 할당된 SCSI device의 메타데이터(id, name)를
 * JSON 배열로 직렬화하여 클라이언트(spdk_rpc.py 등)에게 반환한다.
 * RPC 핸들러는 SPDK_RPC_REGISTER 매크로로 등록되며, SPDK 메인 reactor 스레드의
 * RPC poller가 호출한다 — 따라서 별도 동기화 없이 g_devs[]에 접근할 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * spdk_rpc_listen() (lib/rpc) → spdk_jsonrpc_server_poll() → 디스패처가
 * 메서드명 "scsi_get_devices"를 매칭하여 rpc_scsi_get_devices()를 호출한다.
 * 본 파일은 SCSI 서브시스템의 외부 인터페이스 일부로, dev.c의 scsi_dev_get_list()를
 * 통해 SCSI device 테이블을 읽기만 한다(쓰기 없음).
 * 실행 컨텍스트: SPDK RPC가 동작하는 단일 스레드(보통 main reactor) 유저스페이스.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: scsi_internal.h(scsi_dev_get_list 선언), lib/rpc(JSON-RPC 프레임워크),
 *   lib/jsonrpc(요청 디코드/응답 인코드), lib/json(write_ctx).
 * - 데이터 흐름: g_devs[] (dev.c) → 핸들러가 읽음 → JSON 응답 →
 *   클라이언트(예: scripts/rpc.py scsi_get_devices).
 * - 공유 상태: 전역 g_devs[]는 SCSI device 풀이며, 본 핸들러는 read-only로 접근.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_scsi_get_devices(): "scsi_get_devices" RPC 메서드 핸들러. 파라미터 없음.
 *   결과는 [{id:int, device_name:string}, ...] 형태의 JSON 배열.
 * - SPDK_RPC_REGISTER: 위 핸들러를 SPDK_RPC_RUNTIME 상태(런타임에만 호출 허용)로
 *   등록하는 정적 초기화 매크로.
 */

#include "scsi_internal.h"
/* [한국어] SCSI 내부 헤더 — scsi_dev_get_list(), spdk_scsi_dev 타입 등. */

#include "spdk/rpc.h"
/* [한국어] SPDK_RPC_REGISTER 매크로와 SPDK_RPC_RUNTIME 상태 상수 제공. */
#include "spdk/util.h"
/* [한국어] SPDK 공통 유틸리티(SPDK_COUNTOF 등) — 본 파일에서 직접 쓰진 않으나
 *          상위 헤더 종속성 위해 포함. */

/*
 * [한국어]
 * rpc_scsi_get_devices - "scsi_get_devices" JSON-RPC 메서드 핸들러
 *
 * @request: JSON-RPC 요청 핸들. 응답 작성 후 spdk_jsonrpc_end_result로 전송됨.
 * @params:  요청 파라미터(JSON 값). 본 메서드는 파라미터를 받지 않으므로 NULL이어야 한다.
 *           NULL이 아니면 "Invalid params" 에러로 응답한다.
 *
 * SPDK CLI/관리 도구가 현재 시스템에 등록된 SCSI device 목록을 조회할 때 호출된다.
 * 동작:
 *   1) params != NULL이면 잘못된 호출이므로 에러 응답 후 종료.
 *   2) scsi_dev_get_list()로 g_devs[] 배열의 시작 포인터를 얻음.
 *   3) JSON 응답 컨텍스트(w)를 시작하고 배열 begin.
 *   4) [0..SPDK_SCSI_MAX_DEVS) 순회하며 is_allocated=1인 슬롯만 객체로 출력.
 *      - id: dev->id (int32), device_name: dev->name (문자열).
 *   5) 배열 end → end_result로 응답 송신.
 *
 * 실행 컨텍스트: SPDK RPC poller가 동작하는 단일 스레드(main reactor) — 동시성 없음.
 * 호출자: spdk_jsonrpc_server_poll() 내부 디스패처(SPDK_RPC_REGISTER 등록 정보 기반).
 * 호출 대상: scsi_dev_get_list (dev.c), spdk_jsonrpc_*, spdk_json_write_*.
 * 에러 경로: params 검증 실패 시 invalid params 응답. 그 외 경로에서 에러는 없음.
 *
 * 호출 체인:
 *   spdk_jsonrpc_server_poll → 디스패처 → [rpc_scsi_get_devices] → scsi_dev_get_list, spdk_json_write_*
 */
static void
rpc_scsi_get_devices(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	/* [한국어] JSON 응답을 직렬화할 writer 컨텍스트. begin_result로 획득, end_result로 반환. */
	struct spdk_scsi_dev *devs = scsi_dev_get_list();
	/* [한국어] g_devs[] 배열의 시작 포인터를 얻는다 — 정적 배열이므로 평생 유효, lock 불필요. */
	int i;
	/* [한국어] 배열 순회 인덱스. */

	if (params != NULL) {
		/* [한국어] scsi_get_devices는 파라미터를 받지 않는 메서드 — 파라미터가 있으면 invalid. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "scsi_get_devices requires no parameters");
		/* [한국어] JSON-RPC 표준 에러 코드 -32602(Invalid params) 응답 송신. */
		return;
		/* [한국어] 에러 응답 후 핸들러 종료. */
	}

	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] 응답 작성 시작 — 내부적으로 "result": ... 키 직전까지 헤더를 기록한다. */
	spdk_json_write_array_begin(w);
	/* [한국어] 결과는 device 객체의 배열이므로 '[' 출력. */

	for (i = 0; i < SPDK_SCSI_MAX_DEVS; i++) {
		/* [한국어] g_devs[]의 모든 슬롯을 순회 — 빈 슬롯(is_allocated=0)은 건너뛴다. */
		struct spdk_scsi_dev *dev = &devs[i];
		/* [한국어] 현재 인덱스의 device 포인터. */

		if (!dev->is_allocated) {
			/* [한국어] 미할당 슬롯은 노출하지 않음 — allocate_dev()가 1로 세팅. */
			continue;
		}

		spdk_json_write_object_begin(w);
		/* [한국어] device 한 개를 JSON 객체로 출력 시작 ('{'). */

		spdk_json_write_named_int32(w, "id", dev->id);
		/* [한국어] "id": <dev->id> 출력 — g_devs[] 인덱스와 같다(allocate_dev에서 dev->id=i). */

		spdk_json_write_named_string(w, "device_name", dev->name);
		/* [한국어] "device_name": "<dev->name>" 출력 — spdk_scsi_dev_construct에서 복사된 이름. */

		spdk_json_write_object_end(w);
		/* [한국어] device 객체 종료 ('}'). */
	}
	spdk_json_write_array_end(w);
	/* [한국어] 결과 배열 종료 (']'). */

	spdk_jsonrpc_end_result(request, w);
	/* [한국어] 응답 완성 — 클라이언트로 송신되고 writer 자원 해제. */
}
SPDK_RPC_REGISTER("scsi_get_devices", rpc_scsi_get_devices, SPDK_RPC_RUNTIME)
/* [한국어] 정적 등록 매크로 — 컴파일 단위 로드 시점에 RPC 메서드 테이블에
 *          {name="scsi_get_devices", handler=rpc_scsi_get_devices, state_mask=SPDK_RPC_RUNTIME}을
 *          삽입한다. SPDK_RPC_RUNTIME은 "init 완료 후 런타임에만 호출 허용"을 의미. */
