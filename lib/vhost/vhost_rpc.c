/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK vhost JSON-RPC 핸들러 모음 (vhost_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK vhost target을 외부에서 제어하기 위한 JSON-RPC 핸들러를 모은 곳이다.
 * scripts/rpc.py 또는 사용자가 직접 보내는 JSON-RPC 메시지가 SPDK rpc 서버로 들어오면,
 * 이 파일에서 등록한 핸들러들이 해당 요청을 디코딩하여 vhost.c / vhost_blk.c / vhost_scsi.c
 * 핵심 함수를 호출한다. vhost-blk/vhost-scsi 컨트롤러의 생성·삭제·조회·target 추가/제거,
 * 인터럽트 coalescing 설정, virtio-blk transport 관리 RPC가 모두 이 파일에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   사용자 (rpc.py) → JSON 메시지 → SPDK RPC 서버 (lib/rpc) → 이 파일의 핸들러
 *     → spdk_vhost_*_dev_construct / vhost_*_controller_start / virtio_blk_transport_create 등
 *     → 응답을 spdk_jsonrpc_send_*로 클라이언트에 회신.
 * 호출 컨텍스트: SPDK RPC poller가 실행되는 thread (보통 main reactor의 spdk_thread).
 * 디바이스 thread와 RPC thread가 다를 수 있어 핸들러 내부에서 spdk_vhost_lock()/unlock()로
 * vdev 전역 디렉토리(RB tree)를 보호한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/rpc.h(JSON-RPC), spdk/jsonrpc.h, spdk/vhost.h(공개 API), vhost_internal.h.
 * - 호출 대상: spdk_vhost_scsi_dev_construct, spdk_vhost_blk_construct,
 *   spdk_vhost_dev_remove, spdk_vhost_get_coalescing, virtio_blk_transport_create 등.
 * - 데이터 흐름: 클라이언트가 JSON 객체를 보내면 spdk_json_decode_object로 C 구조체로 변환,
 *   처리 후 결과(boolean/int/object)를 spdk_jsonrpc_send_*로 회신.
 * - 공유 자료구조: spdk_vhost_dev RB tree(spdk_vhost_lock 보호) — 컨트롤러 lookup 대상.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_vhost_create_scsi_controller: virtio-scsi 컨트롤러 생성 RPC.
 * - rpc_vhost_start_scsi_controller: delay 모드로 만들어 둔 scsi 컨트롤러 활성화 RPC.
 * - rpc_vhost_scsi_controller_add_target / _remove_target: scsi LUN 추가/제거 RPC.
 * - rpc_vhost_create_blk_controller: virtio-blk 컨트롤러 생성 RPC.
 * - rpc_vhost_delete_controller: 컨트롤러 삭제 (busy 시 메시지로 재시도).
 * - rpc_vhost_get_controllers: 모든/특정 컨트롤러 정보를 JSON 배열로 반환.
 * - rpc_vhost_controller_set_coalescing: IRQ coalescing 파라미터 설정.
 * - rpc_virtio_blk_get_transports / _create_transport: virtio-blk transport 관리.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 C 헤더 통합 인클루드(stdint, stdio, string 등). */

#include "spdk/log.h"
/* [한국어] SPDK_DEBUGLOG/SPDK_ERRLOG 매크로. */
#include "spdk/rpc.h"
/* [한국어] SPDK_RPC_REGISTER 매크로 — RPC 핸들러를 SPDK RPC 디스패처에 자동 등록. */
#include "spdk/util.h"
/* [한국어] SPDK_COUNTOF 등 유틸 매크로 (decoder 배열 길이 계산). */
#include "spdk/string.h"
/* [한국어] spdk_strerror 등 문자열 헬퍼. */
#include "spdk/env.h"
/* [한국어] SPDK 환경 헬퍼 (spdk_get_thread 등). */
#include "spdk/scsi.h"
/* [한국어] SPDK SCSI 레이어 (vhost-scsi가 의존). */
#include "spdk/vhost.h"
/* [한국어] vhost 공개 API (spdk_vhost_dev_find, spdk_vhost_lock 등). */
#include "vhost_internal.h"
/* [한국어] vhost 내부 헬퍼 (vhost_session_info_json, vhost_dump_info_json, transport API). */
#include "spdk/bdev.h"
/* [한국어] bdev API — virtio-blk/scsi의 백엔드 디바이스 식별 시 필요할 수 있음. */

struct rpc_vhost_scsi_ctrlr {
	char *ctrlr;
	/* [한국어] 컨트롤러 이름(필수) — 새로 생성할 vhost-scsi 컨트롤러의 고유 이름.
	 * 설정자: spdk_json_decode_string. 해제: free_rpc_vhost_scsi_ctrlr. */
	char *cpumask;
	/* [한국어] 디바이스 thread를 배치할 CPU mask 문자열(선택).
	 * NULL이면 SPDK 앱의 기본 cpumask를 사용. 형식: "0xF" 같은 hex. */
	bool delay;
	/* [한국어] true면 컨트롤러를 만들고 listen socket은 활성화하지 않은 채로 둔다.
	 * 이후 vhost_start_scsi_controller RPC로 명시적 활성화 — target add 후 시작 시나리오에 사용. */
};

/*
 * [한국어]
 * free_rpc_vhost_scsi_ctrlr - vhost-scsi 생성 RPC 인자 구조체의 동적 할당 자원 해제.
 *
 * @req: 디코딩된 인자 구조체.
 * @return: 없음.
 *
 * spdk_json_decode_string은 strdup으로 문자열을 할당하므로 핸들러는 사용 후 반드시 free 해야 한다.
 * 호출 컨텍스트: RPC 핸들러 thread (main reactor).
 *
 * 호출 체인:
 *   rpc_vhost_create_scsi_controller → free_rpc_vhost_scsi_ctrlr → free()
 */
static void
free_rpc_vhost_scsi_ctrlr(struct rpc_vhost_scsi_ctrlr *req)
{
	free(req->ctrlr);
	/* [한국어] strdup으로 받은 컨트롤러 이름 해제. NULL이어도 free()는 안전. */
	free(req->cpumask);
	/* [한국어] cpumask 문자열 해제. */
}

static const struct spdk_json_object_decoder rpc_vhost_create_scsi_controller_decoders[] = {
	{"ctrlr", offsetof(struct rpc_vhost_scsi_ctrlr, ctrlr), spdk_json_decode_string },
	/* [한국어] "ctrlr" 필드를 string으로 디코딩 → req->ctrlr. 필수(default false). */
	{"cpumask", offsetof(struct rpc_vhost_scsi_ctrlr, cpumask), spdk_json_decode_string, true},
	/* [한국어] "cpumask" string, optional=true (없어도 됨). */
	{"delay", offsetof(struct rpc_vhost_scsi_ctrlr, delay), spdk_json_decode_bool, true},
	/* [한국어] "delay" bool, optional. */
};
/* [한국어] JSON 객체의 키 → C 구조체 필드 매핑 표. spdk_json_decode_object가 이 표를 따라 변환. */

/*
 * [한국어]
 * rpc_vhost_create_scsi_controller - "vhost_create_scsi_controller" RPC 핸들러.
 *
 * @request: JSON-RPC 요청 컨텍스트(응답을 보낼 때 사용).
 * @params: JSON 인자(객체).
 * @return: 없음. 결과는 request로 회신.
 *
 * 사용자가 새로운 virtio-scsi 컨트롤러를 만들 때 호출되는 RPC.
 * delay=true면 socket listen을 미루고(spdk_vhost_scsi_dev_construct_no_start),
 * delay=false면 바로 listen 시작 후 게스트 connect 대기.
 *
 * 처리 단계:
 *   1) JSON 디코드 → req 구조체 채우기.
 *   2) delay 분기 처리 → 실제 컨트롤러 생성.
 *   3) 성공이면 boolean true 회신, 실패면 invalid_params 에러 회신.
 *
 * 호출 컨텍스트: SPDK RPC thread.
 *
 * 호출 체인:
 *   클라이언트 RPC → SPDK rpc 디스패처 → rpc_vhost_create_scsi_controller
 *     → spdk_vhost_scsi_dev_construct(_no_start)
 */
static void
rpc_vhost_create_scsi_controller(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_vhost_scsi_ctrlr req = {0};
	/* [한국어] 0초기화 — 모든 포인터 NULL/플래그 false 시작. 디코딩 실패시 free 안전. */
	int rc;
	/* [한국어] 결과 코드(음수 errno 또는 0). */

	if (spdk_json_decode_object(params, rpc_vhost_create_scsi_controller_decoders,
				    SPDK_COUNTOF(rpc_vhost_create_scsi_controller_decoders),
				    &req)) {
		/* [한국어] JSON 객체를 req 구조체로 디코딩. 실패 시 0이 아닌 값 반환 → 잘못된 인자. */
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		/* [한국어] 디버그 로그 — 운영에서는 보이지 않음(component 활성 시만). */
		rc = -EINVAL;
		/* [한국어] 잘못된 인자임을 알리는 errno. */
		goto invalid;
		/* [한국어] 공통 에러 회신 경로로 점프. */
	}

	if (req.delay) {
		/* [한국어] delay=true: listen 미루고 컨트롤러만 등록 (target 미리 추가 후 start 패턴). */
		rc = spdk_vhost_scsi_dev_construct_no_start(req.ctrlr, req.cpumask);
		/* [한국어] vhost-scsi 컨트롤러 생성 (소켓 등록은 안 함). */
	} else {
		rc = spdk_vhost_scsi_dev_construct(req.ctrlr, req.cpumask);
		/* [한국어] 일반 경로: 컨트롤러 생성 + 소켓 listen 시작. */
	}
	if (rc < 0) {
		/* [한국어] 실패 코드면 에러 회신. */
		goto invalid;
	}

	free_rpc_vhost_scsi_ctrlr(&req);
	/* [한국어] 성공 시 인자 자원 해제. */

	spdk_jsonrpc_send_bool_response(request, true);
	/* [한국어] {"jsonrpc":"2.0", "result": true, ...} 형식 회신. */
	return;

invalid:
	free_rpc_vhost_scsi_ctrlr(&req);
	/* [한국어] 에러 경로에서도 자원 해제 누수 방지. */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	/* [한국어] 표준 errno(-EINVAL 등)을 문자열로 변환해 에러 응답. */
}
SPDK_RPC_REGISTER("vhost_create_scsi_controller", rpc_vhost_create_scsi_controller,
		  SPDK_RPC_RUNTIME)
/* [한국어] RPC 이름 "vhost_create_scsi_controller"를 SPDK_RPC_RUNTIME 단계에서 등록.
 * RUNTIME = 앱이 동작 중인 상태에서 호출 가능 (subsystem init 이후). */

struct rpc_start_vhost_scsi_ctrlr {
	char *ctrlr;
	/* [한국어] 시작할 컨트롤러 이름. */
};

/*
 * [한국어]
 * free_rpc_start_vhost_scsi_ctrlr - start 인자 자원 해제.
 *
 * @req: 디코딩된 인자.
 * @return: 없음.
 */
static void
free_rpc_start_vhost_scsi_ctrlr(struct rpc_start_vhost_scsi_ctrlr *req)
{
	free(req->ctrlr);
	/* [한국어] 컨트롤러 이름 해제. */
}

static const struct spdk_json_object_decoder rpc_vhost_start_scsi_controller_decoders[] = {
	{"ctrlr", offsetof(struct rpc_start_vhost_scsi_ctrlr, ctrlr), spdk_json_decode_string },
	/* [한국어] 필수 string "ctrlr". */
};

/*
 * [한국어]
 * rpc_vhost_start_scsi_controller - delay 모드로 생성된 vhost-scsi 컨트롤러 활성화 RPC.
 *
 * @request: RPC 요청.
 * @params: JSON 인자.
 *
 * vhost_create_scsi_controller(delay=true)로 만든 컨트롤러는 socket이 listen 상태가 아니다.
 * target들을 모두 추가한 후 이 RPC로 vhost_scsi_controller_start()를 호출하여 활성화.
 *
 * 호출 체인: 클라이언트 → 이 핸들러 → vhost_scsi_controller_start → vhost_user_dev_start.
 */
static void
rpc_vhost_start_scsi_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_start_vhost_scsi_ctrlr req = {0};
	/* [한국어] 0초기화. */
	int rc;
	/* [한국어] 처리 결과. */

	if (spdk_json_decode_object(params, rpc_vhost_start_scsi_controller_decoders,
				    SPDK_COUNTOF(rpc_vhost_start_scsi_controller_decoders),
				    &req)) {
		/* [한국어] JSON 디코드. */
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = vhost_scsi_controller_start(req.ctrlr);
	/* [한국어] 핵심: 이름으로 컨트롤러 lookup 후 listen 활성화. */
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_start_vhost_scsi_ctrlr(&req);
	/* [한국어] 성공 시 자원 해제. */

	spdk_jsonrpc_send_bool_response(request, true);
	/* [한국어] true 회신. */
	return;

invalid:
	free_rpc_start_vhost_scsi_ctrlr(&req);
	/* [한국어] 에러 시도 자원 해제. */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	/* [한국어] 에러 응답. */
}
SPDK_RPC_REGISTER("vhost_start_scsi_controller", rpc_vhost_start_scsi_controller,
		  SPDK_RPC_RUNTIME)
/* [한국어] "vhost_start_scsi_controller" RPC 등록. */

struct rpc_vhost_scsi_ctrlr_add_target {
	char *ctrlr;
	/* [한국어] 대상 컨트롤러 이름. */
	int32_t scsi_target_num;
	/* [한국어] target 번호(LUN 그룹) — [-1, SPDK_VHOST_SCSI_CTRLR_MAX_DEVS).
	 * -1이면 빈 슬롯에 자동 배치, 그 외엔 명시적 배치. */
	char *bdev_name;
	/* [한국어] 이 target에 연결할 bdev 이름 — 게스트가 보게 될 LUN의 백엔드. */
};

/*
 * [한국어]
 * free_rpc_vhost_scsi_ctrlr_add_target - target 추가 인자 자원 해제.
 */
static void
free_rpc_vhost_scsi_ctrlr_add_target(struct rpc_vhost_scsi_ctrlr_add_target *req)
{
	free(req->ctrlr);
	/* [한국어] 컨트롤러 이름 해제. */
	free(req->bdev_name);
	/* [한국어] bdev 이름 해제. */
}

static const struct spdk_json_object_decoder rpc_vhost_scsi_controller_add_target_decoders[] = {
	{"ctrlr", offsetof(struct rpc_vhost_scsi_ctrlr_add_target, ctrlr), spdk_json_decode_string },
	/* [한국어] 필수 string. */
	{"scsi_target_num", offsetof(struct rpc_vhost_scsi_ctrlr_add_target, scsi_target_num), spdk_json_decode_int32},
	/* [한국어] 필수 int32 (음수 허용 — -1은 자동 배치). */
	{"bdev_name", offsetof(struct rpc_vhost_scsi_ctrlr_add_target, bdev_name), spdk_json_decode_string },
	/* [한국어] 필수 string. */
};

/*
 * [한국어]
 * rpc_vhost_scsi_controller_add_target - vhost-scsi 컨트롤러에 target(LUN 그룹) 추가 RPC.
 *
 * @request: RPC 요청.
 * @params: JSON 인자.
 *
 * 처리 단계:
 *   1) 인자 디코드.
 *   2) spdk_vhost_lock으로 vdev tree 보호 → spdk_vhost_dev_find로 컨트롤러 검색.
 *   3) spdk_vhost_scsi_dev_add_tgt로 target 추가.
 *   4) 결과(target_num, 음수 errno)를 정수로 회신.
 *
 * 호출 체인: 클라이언트 → 이 핸들러 → spdk_vhost_scsi_dev_add_tgt.
 */
static void
rpc_vhost_scsi_controller_add_target(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_vhost_scsi_ctrlr_add_target req = {0};
	/* [한국어] 0초기화. */
	struct spdk_json_write_ctx *w;
	/* [한국어] 응답 JSON writer. */
	struct spdk_vhost_dev *vdev;
	/* [한국어] lookup된 컨트롤러 포인터. */
	int rc;
	/* [한국어] 처리 결과(성공 시 target_num, 실패 시 음수 errno). */

	if (spdk_json_decode_object(params, rpc_vhost_scsi_controller_add_target_decoders,
				    SPDK_COUNTOF(rpc_vhost_scsi_controller_add_target_decoders),
				    &req)) {
		/* [한국어] 인자 디코딩. */
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_lock();
	/* [한국어] vhost 전역 디렉토리(RB tree) 락 획득 — vdev 검색 + target 추가가 원자적. */
	vdev = spdk_vhost_dev_find(req.ctrlr);
	/* [한국어] 이름으로 컨트롤러 lookup. */
	if (vdev == NULL) {
		spdk_vhost_unlock();
		/* [한국어] 락을 즉시 풀고 ENODEV 반환. */
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_scsi_dev_add_tgt(vdev, req.scsi_target_num, req.bdev_name);
	/* [한국어] target 추가 실행. 성공 시 실제 부여된 target_num 반환. */
	spdk_vhost_unlock();
	/* [한국어] 락 해제 — 이후 단계는 락 없이 안전. */
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_vhost_scsi_ctrlr_add_target(&req);
	/* [한국어] 인자 자원 해제. */

	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] 응답 JSON 작성 시작. */
	spdk_json_write_int32(w, rc);
	/* [한국어] 결과로 부여된 target_num을 정수로 출력. */
	spdk_jsonrpc_end_result(request, w);
	/* [한국어] 응답 종료 + 전송. */
	return;

invalid:
	free_rpc_vhost_scsi_ctrlr_add_target(&req);
	/* [한국어] 에러 시도 자원 해제. */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	/* [한국어] 에러 응답. */
}
SPDK_RPC_REGISTER("vhost_scsi_controller_add_target", rpc_vhost_scsi_controller_add_target,
		  SPDK_RPC_RUNTIME)
/* [한국어] RPC 등록. */

struct rpc_remove_vhost_scsi_ctrlr_target {
	char *ctrlr;
	/* [한국어] 대상 컨트롤러. */
	uint32_t scsi_target_num;
	/* [한국어] 제거할 target 번호. */
};

/*
 * [한국어]
 * free_rpc_remove_vhost_scsi_ctrlr_target - target 제거 인자 자원 해제.
 */
static void
free_rpc_remove_vhost_scsi_ctrlr_target(struct rpc_remove_vhost_scsi_ctrlr_target *req)
{
	free(req->ctrlr);
	/* [한국어] 컨트롤러 이름 해제. */
}

static const struct spdk_json_object_decoder rpc_vhost_scsi_controller_remove_target_decoders[] = {
	{"ctrlr", offsetof(struct rpc_remove_vhost_scsi_ctrlr_target, ctrlr), spdk_json_decode_string },
	/* [한국어] 필수 string. */
	{"scsi_target_num", offsetof(struct rpc_remove_vhost_scsi_ctrlr_target, scsi_target_num), spdk_json_decode_uint32},
	/* [한국어] 필수 uint32 (양수만). */
};

/*
 * [한국어]
 * rpc_vhost_scsi_controller_remove_target_finish_cb - target 제거 비동기 완료 콜백.
 *
 * @vdev: 대상 컨트롤러.
 * @arg: spdk_jsonrpc_request* 캡슐화 — 응답 회신용.
 * @return: 0 (성공).
 *
 * spdk_vhost_scsi_dev_remove_tgt는 LUN drain이 끝나길 기다리므로 비동기.
 * 콜백 시점에 클라이언트에 true 회신.
 *
 * 호출 컨텍스트: 디바이스 thread (drain 완료 시점).
 */
static int
rpc_vhost_scsi_controller_remove_target_finish_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct spdk_jsonrpc_request *request = arg;
	/* [한국어] arg를 RPC request로 복원. */

	spdk_jsonrpc_send_bool_response(request, true);
	/* [한국어] 클라이언트에 성공 회신. */
	return 0;
	/* [한국어] foreach_session 콜백 컨벤션 — 0/양수는 계속 진행. */
}

/*
 * [한국어]
 * rpc_vhost_scsi_controller_remove_target - target 제거 RPC.
 *
 * @request: RPC 요청.
 * @params: JSON 인자.
 *
 * spdk_vhost_scsi_dev_remove_tgt가 비동기이므로 본 핸들러는 응답을 즉시 보내지 않고
 * 완료 콜백(rpc_vhost_scsi_controller_remove_target_finish_cb)에서 응답.
 */
static void
rpc_vhost_scsi_controller_remove_target(struct spdk_jsonrpc_request *request,
					const struct spdk_json_val *params)
{
	struct rpc_remove_vhost_scsi_ctrlr_target req = {0};
	/* [한국어] 0초기화. */
	struct spdk_vhost_dev *vdev;
	/* [한국어] 대상 컨트롤러. */
	int rc;
	/* [한국어] 처리 결과. */

	if (spdk_json_decode_object(params, rpc_vhost_scsi_controller_remove_target_decoders,
				    SPDK_COUNTOF(rpc_vhost_scsi_controller_remove_target_decoders),
				    &req)) {
		/* [한국어] 디코딩 실패. */
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_lock();
	/* [한국어] 디렉토리 락. */
	vdev = spdk_vhost_dev_find(req.ctrlr);
	/* [한국어] 컨트롤러 lookup. */
	if (vdev == NULL) {
		spdk_vhost_unlock();
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_scsi_dev_remove_tgt(vdev, req.scsi_target_num,
					    rpc_vhost_scsi_controller_remove_target_finish_cb,
					    request);
	/* [한국어] target 제거 시작 — 완료 콜백에서 응답. request를 컨텍스트로 전달. */
	spdk_vhost_unlock();
	/* [한국어] 락 해제. */
	if (rc < 0) {
		/* [한국어] 시작 자체가 실패하면 즉시 에러 응답. */
		goto invalid;
	}

	free_rpc_remove_vhost_scsi_ctrlr_target(&req);
	/* [한국어] 인자 자원 해제 (응답은 콜백이 할 것). */
	return;

invalid:
	free_rpc_remove_vhost_scsi_ctrlr_target(&req);
	/* [한국어] 에러 시도 해제. */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	/* [한국어] 에러 응답. */
}

SPDK_RPC_REGISTER("vhost_scsi_controller_remove_target",
		  rpc_vhost_scsi_controller_remove_target, SPDK_RPC_RUNTIME)
/* [한국어] RPC 등록. */

struct rpc_vhost_blk_ctrlr {
	char *ctrlr;
	/* [한국어] 새 vhost-blk 컨트롤러 이름. */
	char *dev_name;
	/* [한국어] 백엔드 bdev 이름 — 게스트가 단일 디스크로 보게 되는 디바이스. */
	char *cpumask;
	/* [한국어] 디바이스 thread 배치 cpumask (선택). */
	char *transport;
	/* [한국어] 사용할 virtio-blk transport 이름 ("vhost_user", "vfio_user" 등) — 선택. */
};

static const struct spdk_json_object_decoder rpc_vhost_create_blk_controller_decoders[] = {
	{"ctrlr", offsetof(struct rpc_vhost_blk_ctrlr, ctrlr), spdk_json_decode_string },
	/* [한국어] 필수 string. */
	{"dev_name", offsetof(struct rpc_vhost_blk_ctrlr, dev_name), spdk_json_decode_string },
	/* [한국어] 필수 string. */
	{"cpumask", offsetof(struct rpc_vhost_blk_ctrlr, cpumask), spdk_json_decode_string, true},
	/* [한국어] optional. */
	{"transport", offsetof(struct rpc_vhost_blk_ctrlr, transport), spdk_json_decode_string, true},
	/* [한국어] optional — 미지정 시 기본 transport(vhost_user). */
};

/*
 * [한국어]
 * free_rpc_vhost_blk_ctrlr - blk 컨트롤러 인자 자원 해제.
 */
static void
free_rpc_vhost_blk_ctrlr(struct rpc_vhost_blk_ctrlr *req)
{
	free(req->ctrlr);
	/* [한국어] 컨트롤러 이름 해제. */
	free(req->dev_name);
	/* [한국어] bdev 이름 해제. */
	free(req->cpumask);
	/* [한국어] cpumask 해제. */
	free(req->transport);
	/* [한국어] transport 이름 해제. */
}

/*
 * [한국어]
 * rpc_vhost_create_blk_controller - virtio-blk 컨트롤러 생성 RPC.
 *
 * @request: RPC 요청.
 * @params: JSON 인자.
 *
 * relaxed 디코더(spdk_json_decode_object_relaxed)를 사용해 모르는 키도 허용 —
 * transport별 추가 옵션(예: vfio_user의 socket 경로 옵션)을 같은 객체에 담아 전달.
 *
 * 호출 체인: 클라이언트 → 이 핸들러 → spdk_vhost_blk_construct → transport->create_ctrlr.
 */
static void
rpc_vhost_create_blk_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_vhost_blk_ctrlr req = {0};
	/* [한국어] 0초기화. */
	int rc;
	/* [한국어] 결과 코드. */

	if (spdk_json_decode_object_relaxed(params, rpc_vhost_create_blk_controller_decoders,
					    SPDK_COUNTOF(rpc_vhost_create_blk_controller_decoders),
					    &req)) {
		/* [한국어] relaxed: 알 수 없는 키 무시 — transport별 추가 옵션 허용. */
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vhost_blk_construct(req.ctrlr, req.cpumask, req.dev_name, req.transport, params);
	/* [한국어] 컨트롤러 생성. params 자체도 전달 — transport별 추가 옵션 디코딩 위해. */
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_vhost_blk_ctrlr(&req);
	/* [한국어] 성공 시 자원 해제. */

	spdk_jsonrpc_send_bool_response(request, true);
	/* [한국어] true 회신. */
	return;

invalid:
	free_rpc_vhost_blk_ctrlr(&req);
	/* [한국어] 에러 시도 자원 해제. */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	/* [한국어] 에러 응답. */

}
SPDK_RPC_REGISTER("vhost_create_blk_controller", rpc_vhost_create_blk_controller,
		  SPDK_RPC_RUNTIME)
/* [한국어] RPC 등록. */

struct rpc_delete_vhost_ctrlr {
	char *ctrlr;
	/* [한국어] 삭제할 컨트롤러 이름. */
};

static const struct spdk_json_object_decoder rpc_vhost_delete_controller_decoders[] = {
	{"ctrlr", offsetof(struct rpc_delete_vhost_ctrlr, ctrlr), spdk_json_decode_string },
	/* [한국어] 필수 string. */
};

/*
 * [한국어]
 * free_rpc_delete_vhost_ctrlr - delete 인자 자원 해제.
 */
static void
free_rpc_delete_vhost_ctrlr(struct rpc_delete_vhost_ctrlr *req)
{
	free(req->ctrlr);
	/* [한국어] 컨트롤러 이름 해제. */
}

struct vhost_delete_ctrlr_context {
	struct spdk_jsonrpc_request *request;
	/* [한국어] 진행 중 RPC 요청 핸들 — 재시도 후 응답에 사용. */
	const struct spdk_json_val *params;
	/* [한국어] 원본 JSON 인자 — 재진입 시 다시 디코드. */
};

static void _rpc_vhost_delete_controller(void *arg);
/* [한국어] forward declaration — busy 시 spdk_thread_send_msg로 자기 자신을 재 enqueue. */

/*
 * [한국어]
 * rpc_vhost_delete_controller - vhost 컨트롤러 삭제 RPC.
 *
 * @request: RPC 요청.
 * @params: JSON 인자.
 *
 * spdk_vhost_dev_remove가 -EBUSY를 반환하면 (진행 중 비동기 작업이 있어 즉시 못 지움),
 * spdk_thread_send_msg로 같은 thread에 _rpc_vhost_delete_controller를 enqueue하여
 * 다음 reactor 폴링 사이클에 다시 시도한다 — 짧은 대기 후 재진입 패턴.
 *
 * 호출 체인: 클라이언트 → 이 핸들러 → spdk_vhost_dev_remove → backend->remove_device.
 */
static void
rpc_vhost_delete_controller(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_delete_vhost_ctrlr req = {0};
	/* [한국어] 0초기화. */
	struct spdk_vhost_dev *vdev;
	/* [한국어] 대상 컨트롤러. */
	int rc;
	/* [한국어] 결과 코드. */

	if (spdk_json_decode_object(params, rpc_vhost_delete_controller_decoders,
				    SPDK_COUNTOF(rpc_vhost_delete_controller_decoders), &req)) {
		/* [한국어] 인자 디코딩. */
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_lock();
	/* [한국어] 디렉토리 락. */
	vdev = spdk_vhost_dev_find(req.ctrlr);
	/* [한국어] 컨트롤러 lookup. */
	if (vdev == NULL) {
		spdk_vhost_unlock();
		rc = -ENODEV;
		goto invalid;
	}
	spdk_vhost_unlock();
	/* [한국어] vdev 포인터를 캡처했으므로 락을 풀어도 안전(remove가 vdev 자체를 잠금). */

	rc = spdk_vhost_dev_remove(vdev);
	/* [한국어] 삭제 시도. */
	if (rc < 0) {
		if (rc == -EBUSY) {
			/* [한국어] busy: 진행 중 작업이 있어 지금은 못 지움 → 재시도 메시지 enqueue. */
			struct vhost_delete_ctrlr_context *ctx;

			ctx = calloc(1, sizeof(*ctx));
			/* [한국어] 컨텍스트 할당 — 재진입 시 request/params 보존. */
			if (ctx == NULL) {
				SPDK_ERRLOG("Failed to allocate memory for vhost_delete_ctrlr context\n");
				/* [한국어] 메모리 부족 — 더 이상 재시도 불가. */
				rc = -ENOMEM;
				goto invalid;
			}
			ctx->request = request;
			/* [한국어] 응답할 RPC 핸들 보존. */
			ctx->params = params;
			/* [한국어] 인자 보존. */
			spdk_thread_send_msg(spdk_get_thread(), _rpc_vhost_delete_controller, ctx);
			/* [한국어] 같은 thread의 메시지 큐에 재시도 함수 enqueue.
			 * 다음 reactor poll에서 _rpc_vhost_delete_controller가 호출됨 → 사실상 짧은 yield. */
			free_rpc_delete_vhost_ctrlr(&req);
			/* [한국어] 인자 자원 해제 (params 자체는 SPDK rpc 인프라가 관리). */
			return;
			/* [한국어] 재시도 enqueue 후 즉시 리턴 — 응답은 다음 시도가 처리. */
		}
		goto invalid;
		/* [한국어] busy 외 에러는 즉시 에러 응답. */
	}

	free_rpc_delete_vhost_ctrlr(&req);
	/* [한국어] 성공 시 자원 해제. */

	spdk_jsonrpc_send_bool_response(request, true);
	/* [한국어] true 응답. */
	return;

invalid:
	free_rpc_delete_vhost_ctrlr(&req);
	/* [한국어] 에러 시도 자원 해제. */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	/* [한국어] 에러 응답. */

}
SPDK_RPC_REGISTER("vhost_delete_controller", rpc_vhost_delete_controller, SPDK_RPC_RUNTIME)
/* [한국어] RPC 등록. */

/*
 * [한국어]
 * _rpc_vhost_delete_controller - delete RPC의 재시도 진입점 (메시지 큐로 enqueue됨).
 *
 * @arg: vhost_delete_ctrlr_context*.
 *
 * spdk_thread_send_msg가 호출하는 콜백. ctx에서 request/params를 꺼내 본 핸들러를 재실행.
 *
 * 호출 컨텍스트: 디바이스 thread (다음 polling 사이클).
 */
static void _rpc_vhost_delete_controller(void *arg)
{
	struct vhost_delete_ctrlr_context *ctx = arg;
	/* [한국어] 컨텍스트 캐스팅. */

	rpc_vhost_delete_controller(ctx->request, ctx->params);
	/* [한국어] 본 핸들러 재실행 — 또 EBUSY면 다시 enqueue, 성공/실패면 응답 발사. */
	free(ctx);
	/* [한국어] 컨텍스트 해제. */
}

struct rpc_get_vhost_ctrlrs {
	char *name;
	/* [한국어] 조회할 특정 컨트롤러 이름 (선택). NULL이면 전체. */
};

/*
 * [한국어]
 * _rpc_get_vhost_controller - 단일 컨트롤러 정보를 JSON 객체로 출력.
 *
 * @w: JSON writer.
 * @vdev: 대상 컨트롤러.
 *
 * 출력 스키마:
 *   {
 *     "ctrlr": "<name>",
 *     "cpumask": "0x<hex>",
 *     "delay_base_us": <coalescing>,
 *     "iops_threshold": <coalescing>,
 *     "socket": "<UNIX path>",
 *     "sessions": [...],            // 활성 세션들
 *     "backend_specific": { ... }   // blk/scsi별 정보
 *   }
 */
static void
_rpc_get_vhost_controller(struct spdk_json_write_ctx *w, struct spdk_vhost_dev *vdev)
{
	uint32_t delay_base_us, iops_threshold;
	/* [한국어] coalescing 값 임시 저장. */

	spdk_vhost_get_coalescing(vdev, &delay_base_us, &iops_threshold);
	/* [한국어] 현재 coalescing 파라미터 조회. */

	spdk_json_write_object_begin(w);
	/* [한국어] '{' 출력. */

	spdk_json_write_named_string(w, "ctrlr", spdk_vhost_dev_get_name(vdev));
	/* [한국어] "ctrlr": "<name>" 출력. */
	spdk_json_write_named_string_fmt(w, "cpumask", "0x%s",
					 spdk_cpuset_fmt(spdk_thread_get_cpumask(vdev->thread)));
	/* [한국어] "cpumask": "0x<hex>" 출력 — 디바이스 thread의 cpumask를 hex 문자열로 변환. */
	spdk_json_write_named_uint32(w, "delay_base_us", delay_base_us);
	/* [한국어] coalescing delay. */
	spdk_json_write_named_uint32(w, "iops_threshold", iops_threshold);
	/* [한국어] coalescing IOPS 임계. */
	spdk_json_write_named_string(w, "socket", vdev->path);
	/* [한국어] UNIX 소켓 경로. */
	spdk_json_write_named_array_begin(w, "sessions");
	/* [한국어] "sessions": [ — 활성 세션 배열 시작. */
	vhost_session_info_json(vdev, w);
	/* [한국어] 디바이스의 모든 세션을 JSON 객체로 출력. */
	spdk_json_write_array_end(w);
	/* [한국어] ']' 닫기. */

	spdk_json_write_named_object_begin(w, "backend_specific");
	/* [한국어] "backend_specific": { — blk/scsi별 정보. */
	vhost_dump_info_json(vdev, w);
	/* [한국어] backend->dump_info_json 디스패치 (blk: bdev 이름/transport, scsi: LUN 리스트 등). */
	spdk_json_write_object_end(w);
	/* [한국어] '}' 닫기. */

	spdk_json_write_object_end(w);
	/* [한국어] 컨트롤러 객체 '}' 닫기. */
}

static const struct spdk_json_object_decoder rpc_vhost_get_controllers_decoders[] = {
	{"name", offsetof(struct rpc_get_vhost_ctrlrs, name), spdk_json_decode_string, true},
	/* [한국어] 선택 string. */
};

/*
 * [한국어]
 * free_rpc_get_vhost_ctrlrs - get 인자 자원 해제.
 */
static void
free_rpc_get_vhost_ctrlrs(struct rpc_get_vhost_ctrlrs *req)
{
	free(req->name);
	/* [한국어] 이름 해제(NULL이면 무해). */
}

/*
 * [한국어]
 * rpc_vhost_get_controllers - 컨트롤러 정보 조회 RPC.
 *
 * @request: RPC 요청.
 * @params: JSON 인자(NULL 또는 {"name": "..."}).
 *
 * params NULL이면 전체 컨트롤러 배열, name 명시 시 해당 컨트롤러만 반환.
 * 응답은 항상 배열 형태 (단일 결과도 길이 1 배열).
 *
 * 호출 체인: 클라이언트 → 이 핸들러 → spdk_vhost_dev_next 순회 → _rpc_get_vhost_controller.
 */
static void
rpc_vhost_get_controllers(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_get_vhost_ctrlrs req = {0};
	/* [한국어] 0초기화. */
	struct spdk_json_write_ctx *w;
	/* [한국어] JSON writer. */
	struct spdk_vhost_dev *vdev;
	/* [한국어] 순회/lookup 대상. */
	int rc;
	/* [한국어] 결과 코드. */

	if (params && spdk_json_decode_object(params, rpc_vhost_get_controllers_decoders,
					      SPDK_COUNTOF(rpc_vhost_get_controllers_decoders), &req)) {
		/* [한국어] params가 있으면 디코딩 시도. params NULL이면 디코딩 스킵(전체 조회). */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_lock();
	/* [한국어] 디렉토리 락 — 순회 중 RB tree 변경 방지. */
	if (req.name != NULL) {
		/* [한국어] 특정 이름 조회 경로. */
		vdev = spdk_vhost_dev_find(req.name);
		/* [한국어] 이름으로 lookup. */
		if (vdev == NULL) {
			spdk_vhost_unlock();
			rc = -ENODEV;
			goto invalid;
		}

		free_rpc_get_vhost_ctrlrs(&req);
		/* [한국어] 인자 자원 해제 (vdev 포인터는 락 보호하에 유효). */

		w = spdk_jsonrpc_begin_result(request);
		/* [한국어] 응답 JSON 시작. */
		spdk_json_write_array_begin(w);
		/* [한국어] '[' — 단일 원소 배열 형태 유지. */

		_rpc_get_vhost_controller(w, vdev);
		/* [한국어] 컨트롤러 정보 출력. */
		spdk_vhost_unlock();
		/* [한국어] 출력 완료 후 락 해제. */

		spdk_json_write_array_end(w);
		/* [한국어] ']' 닫기. */
		spdk_jsonrpc_end_result(request, w);
		/* [한국어] 응답 전송. */
		return;
	}

	free_rpc_get_vhost_ctrlrs(&req);
	/* [한국어] 전체 조회 경로 — 인자 해제 (req.name은 NULL). */

	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] 응답 시작. */
	spdk_json_write_array_begin(w);
	/* [한국어] '[' — 전체 컨트롤러 배열 시작. */

	vdev = spdk_vhost_dev_next(NULL);
	/* [한국어] RB tree 순회 시작 — NULL은 첫 번째. */
	while (vdev != NULL) {
		_rpc_get_vhost_controller(w, vdev);
		/* [한국어] 각 컨트롤러 정보 출력. */
		vdev = spdk_vhost_dev_next(vdev);
		/* [한국어] 다음 컨트롤러로. */
	}
	spdk_vhost_unlock();
	/* [한국어] 순회 종료 후 락 해제. */

	spdk_json_write_array_end(w);
	/* [한국어] ']' 닫기. */
	spdk_jsonrpc_end_result(request, w);
	/* [한국어] 응답 전송. */
	return;

invalid:
	free_rpc_get_vhost_ctrlrs(&req);
	/* [한국어] 에러 시도 자원 해제. */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					 spdk_strerror(-rc));
	/* [한국어] 에러 응답 (INTERNAL_ERROR 코드). */
}
SPDK_RPC_REGISTER("vhost_get_controllers", rpc_vhost_get_controllers, SPDK_RPC_RUNTIME)
/* [한국어] RPC 등록. */


struct rpc_vhost_ctrlr_coalescing {
	char *ctrlr;
	/* [한국어] 대상 컨트롤러 이름. */
	uint32_t delay_base_us;
	/* [한국어] coalescing delay 베이스(마이크로초). 0이면 비활성화. */
	uint32_t iops_threshold;
	/* [한국어] coalescing 활성 IOPS 임계치. */
};

static const struct spdk_json_object_decoder rpc_vhost_controller_set_coalescing_decoders[] = {
	{"ctrlr", offsetof(struct rpc_vhost_ctrlr_coalescing, ctrlr), spdk_json_decode_string },
	/* [한국어] 필수 string. */
	{"delay_base_us", offsetof(struct rpc_vhost_ctrlr_coalescing, delay_base_us), spdk_json_decode_uint32},
	/* [한국어] 필수 uint32. */
	{"iops_threshold", offsetof(struct rpc_vhost_ctrlr_coalescing, iops_threshold), spdk_json_decode_uint32},
	/* [한국어] 필수 uint32. */
};

/*
 * [한국어]
 * free_rpc_set_vhost_controllers_event_coalescing - coalescing 인자 자원 해제.
 */
static void
free_rpc_set_vhost_controllers_event_coalescing(struct rpc_vhost_ctrlr_coalescing *req)
{
	free(req->ctrlr);
	/* [한국어] 컨트롤러 이름 해제. */
}

/*
 * [한국어]
 * rpc_vhost_controller_set_coalescing - "vhost_controller_set_coalescing" RPC 핸들러.
 *
 * @request: RPC 요청.
 * @params: JSON 인자.
 *
 * 컨트롤러의 IRQ coalescing 파라미터를 동적으로 변경 — 게스트 IRQ 부하/지연 트레이드오프 조절.
 * spdk_vhost_set_coalescing은 백엔드 콜백을 호출 → 모든 활성 세션에 전파.
 */
static void
rpc_vhost_controller_set_coalescing(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct rpc_vhost_ctrlr_coalescing req = {0};
	/* [한국어] 0초기화. */
	struct spdk_vhost_dev *vdev;
	/* [한국어] 대상 컨트롤러. */
	int rc;
	/* [한국어] 결과. */

	if (spdk_json_decode_object(params, rpc_vhost_controller_set_coalescing_decoders,
				    SPDK_COUNTOF(rpc_vhost_controller_set_coalescing_decoders), &req)) {
		/* [한국어] 디코딩. */
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_lock();
	/* [한국어] 디렉토리 락. */
	vdev = spdk_vhost_dev_find(req.ctrlr);
	/* [한국어] lookup. */
	if (vdev == NULL) {
		spdk_vhost_unlock();
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_set_coalescing(vdev, req.delay_base_us, req.iops_threshold);
	/* [한국어] 백엔드 set_coalescing 호출 (vhost-user 외 transport 미지원 시 ENOTSUP 반환 가능). */
	spdk_vhost_unlock();
	/* [한국어] 락 해제. */
	if (rc) {
		goto invalid;
	}

	free_rpc_set_vhost_controllers_event_coalescing(&req);
	/* [한국어] 인자 자원 해제. */

	spdk_jsonrpc_send_bool_response(request, true);
	/* [한국어] true 응답. */
	return;

invalid:
	free_rpc_set_vhost_controllers_event_coalescing(&req);
	/* [한국어] 에러 시도 해제. */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	/* [한국어] 에러 응답. */
}
SPDK_RPC_REGISTER("vhost_controller_set_coalescing", rpc_vhost_controller_set_coalescing,
		  SPDK_RPC_RUNTIME)
/* [한국어] RPC 등록. */

struct rpc_get_transport {
	char *name;
	/* [한국어] 조회할 transport 이름 (선택, NULL이면 전체). */
};

static const struct spdk_json_object_decoder rpc_virtio_blk_get_transports_decoders[] = {
	{"name", offsetof(struct rpc_get_transport, name), spdk_json_decode_string, true},
	/* [한국어] optional. */
};

/*
 * [한국어]
 * rpc_virtio_blk_get_transports - virtio-blk transport 정보 조회 RPC.
 *
 * @request: RPC 요청.
 * @params: JSON 인자(NULL 또는 {"name": "..."}).
 *
 * 출력: transport 옵션 객체들의 배열. name 지정 시 길이 1, 아니면 전체.
 */
static void
rpc_virtio_blk_get_transports(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_get_transport req = { 0 };
	/* [한국어] 0초기화. */
	struct spdk_json_write_ctx *w;
	/* [한국어] JSON writer. */
	struct spdk_virtio_blk_transport *transport = NULL;
	/* [한국어] 단일 lookup 결과 또는 순회 변수. */

	if (params) {
		/* [한국어] params가 있을 때만 디코딩. */
		if (spdk_json_decode_object(params, rpc_virtio_blk_get_transports_decoders,
					    SPDK_COUNTOF(rpc_virtio_blk_get_transports_decoders),
					    &req)) {
			/* [한국어] 디코딩 실패 — 즉시 에러 응답 후 리턴. */
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	if (req.name) {
		/* [한국어] 특정 이름 lookup. */
		transport = virtio_blk_tgt_get_transport(req.name);
		if (transport == NULL) {
			/* [한국어] 없으면 ENODEV 응답. */
			SPDK_ERRLOG("transport '%s' does not exist\n", req.name);
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
			free(req.name);
			/* [한국어] name 해제. */
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] 응답 시작. */
	spdk_json_write_array_begin(w);
	/* [한국어] '['. */

	if (transport) {
		/* [한국어] 단일 transport 출력. */
		virtio_blk_transport_dump_opts(transport, w);
	} else {
		/* [한국어] 전체 transport 순회 출력. */
		for (transport = virtio_blk_transport_get_first(); transport != NULL;
		     transport = virtio_blk_transport_get_next(transport)) {
			/* [한국어] TAILQ 순회. */
			virtio_blk_transport_dump_opts(transport, w);
		}
	}

	spdk_json_write_array_end(w);
	/* [한국어] ']'. */
	spdk_jsonrpc_end_result(request, w);
	/* [한국어] 응답 전송. */
	free(req.name);
	/* [한국어] name 해제 (NULL이면 무해). */
}
SPDK_RPC_REGISTER("virtio_blk_get_transports", rpc_virtio_blk_get_transports, SPDK_RPC_RUNTIME)
/* [한국어] RPC 등록. */

struct rpc_virtio_blk_create_transport {
	char *name;
	/* [한국어] 생성할 transport 이름. */
};

static const struct spdk_json_object_decoder rpc_virtio_blk_create_transport_decoders[] = {
	{"name", offsetof(struct rpc_virtio_blk_create_transport, name), spdk_json_decode_string},
	/* [한국어] 필수 string. */
};

/*
 * [한국어]
 * free_rpc_virtio_blk_create_transport - create_transport 인자 자원 해제.
 */
static void
free_rpc_virtio_blk_create_transport(struct rpc_virtio_blk_create_transport *req)
{
	free(req->name);
	/* [한국어] 이름 해제. */
}

/*
 * [한국어]
 * rpc_virtio_blk_create_transport - virtio-blk transport 인스턴스 생성 RPC.
 *
 * @request: RPC 요청.
 * @params: JSON 인자.
 *
 * relaxed 디코더 사용 — transport별 추가 옵션을 같은 객체에 담아 전달 가능.
 * virtio_blk_transport_create는 등록된 ops에서 이름으로 transport_ops를 찾아 ops->create() 호출.
 *
 * 호출 체인: 클라이언트 → 이 핸들러 → virtio_blk_transport_create → ops->create.
 */
static void
rpc_virtio_blk_create_transport(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_virtio_blk_create_transport req = {0};
	/* [한국어] 0초기화. */
	int rc;
	/* [한국어] 결과 코드. */

	if (spdk_json_decode_object_relaxed(params, rpc_virtio_blk_create_transport_decoders,
					    SPDK_COUNTOF(rpc_virtio_blk_create_transport_decoders), &req)) {
		/* [한국어] relaxed 디코딩 — 알 수 없는 키 허용. */
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_lock();
	/* [한국어] 디렉토리 락 — transport 리스트도 같은 락으로 보호. */
	rc = virtio_blk_transport_create(req.name, params);
	/* [한국어] transport 인스턴스 생성. params 자체도 전달 — ops->create가 추가 옵션 파싱. */
	spdk_vhost_unlock();
	/* [한국어] 락 해제. */
	if (rc != 0) {
		goto invalid;
	}

	free_rpc_virtio_blk_create_transport(&req);
	/* [한국어] 인자 자원 해제. */
	spdk_jsonrpc_send_bool_response(request, true);
	/* [한국어] true 응답. */
	return;

invalid:
	free_rpc_virtio_blk_create_transport(&req);
	/* [한국어] 에러 시도 해제. */
	spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	/* [한국어] 에러 응답. rc 자체를 코드로 전달 (음수). */
}
SPDK_RPC_REGISTER("virtio_blk_create_transport", rpc_virtio_blk_create_transport,
		  SPDK_RPC_RUNTIME)
/* [한국어] RPC 등록. */

SPDK_LOG_REGISTER_COMPONENT(vhost_rpc)
/* [한국어] "vhost_rpc" 로그 컴포넌트 등록 — SPDK_DEBUGLOG(vhost_rpc, ...)을 활성화하려면
 * 앱 실행 시 -L vhost_rpc 옵션 또는 spdk_log_set_flag("vhost_rpc")를 호출해야 한다. */
