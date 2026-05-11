/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK ublk JSON-RPC 핸들러 (ublk_rpc.c)
 *
 * === 파일의 역할 ===
 * 사용자가 spdk-rpc 클라이언트로 보낸 JSON-RPC 요청을 받아 ublk
 * (Userspace Block Device) 모듈의 내부 API(`ublk_create_target`,
 * `ublk_start_disk`, `ublk_stop_disk` 등)를 호출하고 그 결과를
 * 다시 JSON 응답으로 직렬화해 클라이언트에게 돌려준다. 즉 ublk 모듈의
 * 외부 제어 평면(control plane)을 RPC 형태로 노출하는 어댑터 계층이다.
 * 본 파일이 다루는 RPC 메서드는 다음 5종이다:
 *   - ublk_create_target  : ublk target 라이프사이클 시작
 *   - ublk_destroy_target : ublk target 라이프사이클 종료
 *   - ublk_start_disk     : 특정 bdev를 ublk 디스크(/dev/ublkbN)로 노출
 *   - ublk_stop_disk      : ublk 디스크 중지
 *   - ublk_get_disks      : 등록된 ublk 디스크 목록/정보 조회
 *   - ublk_recover_disk   : 끊긴 ublk 디스크를 재연결(fast-failover)
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [spdk-rpc 클라이언트(Python)]
 *      ↓ Unix domain socket / TCP, JSON-RPC 2.0
 *   [SPDK rpc 서버 (lib/rpc)]
 *      ↓ method dispatch
 *   [본 파일: ublk_rpc.c — 메서드 핸들러]
 *      ↓ 직접 호출
 *   [ublk 모듈 코어 (lib/ublk/ublk.c)]
 *      ↓ io_uring + ublk 컨트롤 ioctl
 *   [Linux 커널 ublk 드라이버]
 *      ↓
 *   [/dev/ublkbN 블록 디바이스 (호스트 사용자)]
 * 실행 컨텍스트: SPDK rpc 서버 스레드(보통 main reactor). RPC 핸들러는
 * spdk_jsonrpc_request 객체에 동기/비동기로 응답을 작성한다. ublk 컨트롤은
 * 비동기이므로 핸들러는 콜백을 등록하고 일단 반환, 콜백에서 응답을 보낸다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `spdk/string.h`(spdk_strerror), `spdk/env.h`, `spdk/rpc.h`
 *   (SPDK_RPC_REGISTER 매크로), `spdk/util.h`(SPDK_COUNTOF), `spdk/log.h`
 *   (SPDK_ERRLOG/NOTICELOG), `ublk_internal.h`(ublk_create_target 등 진입점).
 * - 의존자: 사용자가 직접 호출하지 않는다. SPDK_RPC_REGISTER 매크로를 통해
 *   초기화 시점에 ublk_xxx 메서드가 spdk rpc 디스패처에 등록되며, JSON-RPC
 *   메서드 이름으로 호출된다.
 * - 데이터 흐름: JSON params → spdk_json_decode_object → C 구조체(rpc_*) →
 *   ublk 진입점 호출 → 콜백/즉시 응답 → spdk_jsonrpc_send_*로 응답 직렬화.
 * - 공유 자료구조: 각 RPC 메서드별로 정의된 `struct rpc_ublk_*` 구조체와
 *   `spdk_json_object_decoder` 디코더 배열이 짝을 이룬다. 비동기 응답을 위해
 *   힙 할당(calloc)된 구조체에 spdk_jsonrpc_request 핸들도 보관한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_ublk_create_target / rpc_ublk_destroy_target : target 라이프사이클 RPC.
 * - rpc_ublk_start_disk / rpc_ublk_stop_disk : 디스크 단위 lifecycle RPC (비동기 패턴).
 * - rpc_ublk_get_disks / rpc_dump_ublk_info : 디스크 enumeration & JSON 직렬화.
 * - rpc_ublk_recover_disk : 재시작된 SPDK가 기존 ublk 디스크를 회복.
 * - struct rpc_ublk_* : 각 RPC의 입력 파라미터 + 비동기 컨텍스트.
 * - rpc_*_decoders : JSON 객체를 위 구조체로 변환하는 필드 매핑 테이블.
 */

/* [한국어] spdk_strerror — errno → 영문 에러 문자열 변환. RPC 에러 응답에 사용. */
#include "spdk/string.h"
/* [한국어] DPDK 기반 환경 추상화. 본 파일에서 직접 사용하진 않지만 ublk_internal.h가
 *  포함하는 헤더 트리에서 필요. */
#include "spdk/env.h"
/* [한국어] SPDK_RPC_REGISTER 매크로와 spdk_jsonrpc_request/response API. */
#include "spdk/rpc.h"
/* [한국어] SPDK_COUNTOF — 정적 배열 길이 매크로. 디코더 배열 크기 계산에 사용. */
#include "spdk/util.h"
/* [한국어] SPDK_ERRLOG / SPDK_NOTICELOG 등 로깅 매크로. */
#include "spdk/log.h"

/* [한국어] ublk 모듈 내부 진입점들의 선언 (ublk_create_target 등). */
#include "ublk_internal.h"

/*
 * [한국어] struct rpc_ublk_create_target — `ublk_create_target` RPC 입력 파라미터.
 *  JSON params의 두 키("cpumask", "disable_user_copy")가 디코더 테이블을 통해
 *  이 구조체의 동일 이름 필드로 매핑된다.
 */
struct rpc_ublk_create_target {
	char		*cpumask;
	/* [한국어] ublk IO 처리 SPDK 스레드를 배치할 CPU 마스크 문자열(예: "0xF").
	 * 설정자: spdk_json_decode_string이 strdup로 동적 할당.
	 * 읽는 자: ublk_create_target에 그대로 전달.
	 * 값 범위: NULL(미지정 → 기본 마스크) 또는 "0x..." 문자열.
	 * 동기화: RPC 처리는 main reactor에서 단일 스레드이므로 락 불필요. */

	bool		disable_user_copy;
	/* [한국어] true이면 UBLK_F_USER_COPY 비활성화(전통 카피 경로 사용),
	 * false(default)이면 활성화 시도(zero-copy/효율 경로).
	 * 설정자: spdk_json_decode_bool. 읽는 자: ublk_create_target. */
};

/*
 * [한국어] rpc_ublk_create_target_decoders — JSON 키→구조체 필드 매핑.
 *  네 번째 인자 true는 "옵션" 의미(=JSON에 키가 없어도 OK).
 *  spdk_json_decode_object가 이 테이블을 순회하며 각 키를 파싱한다.
 */
static const struct spdk_json_object_decoder rpc_ublk_create_target_decoders[] = {
	/* [한국어] "cpumask" 문자열 → req.cpumask. optional=true. */
	{"cpumask", offsetof(struct rpc_ublk_create_target, cpumask), spdk_json_decode_string, true},
	/* [한국어] "disable_user_copy" boolean → req.disable_user_copy. optional=true. */
	{"disable_user_copy", offsetof(struct rpc_ublk_create_target, disable_user_copy), spdk_json_decode_bool, true},
};

/*
 * [한국어]
 * free_rpc_ublk_create_target - 디코더가 strdup로 할당한 cpumask 메모리 해제.
 *
 * @req: 정리할 입력 객체.
 *
 * 호출 컨텍스트: rpc_ublk_create_target 종료 직전 (성공/실패 모두).
 */
static void
free_rpc_ublk_create_target(struct rpc_ublk_create_target *req)
{
	/* [한국어] free(NULL)은 안전하므로 미설정/디코드 실패 시도 안전. */
	free(req->cpumask);
}

/*
 * [한국어]
 * rpc_ublk_create_target - "ublk_create_target" RPC 메서드 핸들러.
 *
 * @request: spdk rpc 서버가 만든 요청 객체. 응답을 보낼 때 사용.
 * @params: JSON params 노드(없으면 NULL).
 *
 * 동작: params 디코드 → ublk_create_target 호출 → 즉시 bool 응답.
 *  ublk_create_target 자체가 동기적으로 (또는 추가 콜백 없이) 결과를 돌려주므로 콜백 불필요.
 *
 * 호출 체인: rpc 서버 디스패처 → [rpc_ublk_create_target] → ublk_create_target
 * 호출 컨텍스트: SPDK main reactor 스레드 (RPC 서버 스레드).
 */
static void
rpc_ublk_create_target(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	int rc = 0;                                        /* [한국어] 결과 코드 누적. */
	struct rpc_ublk_create_target req = {};            /* [한국어] 입력 파라미터 영확장(zero-init). */

	/* [한국어] params가 NULL이면 디코드 생략 — 모든 필드 옵션이라 빈 호출도 허용. */
	if (params != NULL) {
		/* [한국어] JSON 객체 → C 구조체 디코드. 실패 시 -EINVAL로 즉시 에러 응답. */
		if (spdk_json_decode_object(params, rpc_ublk_create_target_decoders,
					    SPDK_COUNTOF(rpc_ublk_create_target_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			rc = -EINVAL;
			goto invalid;
		}
	}
	/* [한국어] ublk 코어에 target 생성 요청. cpumask=NULL이면 기본값 사용. */
	rc = ublk_create_target(req.cpumask, req.disable_user_copy);
	if (rc != 0) {
		/* [한국어] 에러 → invalid 라벨로 통합 처리 (응답 + 자원 해제). */
		goto invalid;
	}
	/* [한국어] 성공 → JSON 응답 "true". spdk-rpc 클라이언트는 bool로 받는다. */
	spdk_jsonrpc_send_bool_response(request, true);
	free_rpc_ublk_create_target(&req);
	return;
invalid:
	/* [한국어] 에러 로깅 + 표준 JSON-RPC 에러 응답(-32603 INTERNAL_ERROR + 메시지). */
	SPDK_ERRLOG("Can't create ublk target: %s\n", spdk_strerror(-rc));
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
	free_rpc_ublk_create_target(&req);
}
/* [한국어] SPDK_RPC_REGISTER — 모듈 초기화 시 RPC 디스패처에 메서드 등록.
 *  SPDK_RPC_RUNTIME: SPDK가 모든 서브시스템 초기화를 마친 후에만 호출 가능한 메서드 분류. */
SPDK_RPC_REGISTER("ublk_create_target", rpc_ublk_create_target, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * ublk_destroy_target_done - ublk_destroy_target 비동기 완료 콜백.
 *
 * @arg: 콜백 컨텍스트(=원래의 spdk_jsonrpc_request 핸들).
 *
 * 동작: 정리가 끝났음을 클라이언트에 bool true로 알리고 노티스 로깅.
 *
 * 호출 체인: ublk 코어가 cleanup 완료 시 → [ublk_destroy_target_done]
 * 호출 컨텍스트: ublk 코어가 콜백을 어디서 호출하느냐에 따라 다르지만 보통 main reactor.
 */
static void
ublk_destroy_target_done(void *arg)
{
	/* [한국어] void* → 본래 타입으로 복원. spdk_jsonrpc_request의 lifetime은 핸들러 진입~응답까지. */
	struct spdk_jsonrpc_request *req = arg;

	/* [한국어] 성공 응답. ublk 코어가 cleanup 도중 에러를 만나도 여기까지 오면 정상 처리한 것. */
	spdk_jsonrpc_send_bool_response(req, true);
	SPDK_NOTICELOG("ublk target has been destroyed\n");
}

/*
 * [한국어]
 * rpc_ublk_destroy_target - "ublk_destroy_target" RPC 핸들러.
 *
 * 비동기 패턴: 핸들러는 콜백 등록 후 즉시 반환. 콜백이 응답 송신.
 *  ublk_destroy_target이 0을 반환했을 때만 비동기 진행이 시작된 것이므로
 *  여기서는 응답을 보내지 않는다.
 *
 * @request, @params: 표준 RPC 핸들러 인자. params는 사용하지 않음(빈 호출 허용).
 */
static void
rpc_ublk_destroy_target(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	int rc = 0;

	/* [한국어] ublk 코어에 destroy 요청. 콜백 인자로 request를 그대로 넘김. */
	rc = ublk_destroy_target(ublk_destroy_target_done, request);
	if (rc != 0) {
		/* [한국어] 즉시 실패 — 비동기 진행 안 됨. 여기서 에러 응답을 보내야 클라이언트가 hang하지 않음. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		SPDK_ERRLOG("Can't destroy ublk target: %s\n", spdk_strerror(-rc));
	}
}
SPDK_RPC_REGISTER("ublk_destroy_target", rpc_ublk_destroy_target, SPDK_RPC_RUNTIME)

/*
 * [한국어] struct rpc_ublk_start_disk — `ublk_start_disk` RPC의 입력 + 비동기 컨텍스트.
 *  비동기 처리이므로 힙 할당(calloc)으로 핸들러 반환 이후에도 살아있어야 한다.
 */
struct rpc_ublk_start_disk {
	char		*bdev_name;
	/* [한국어] 노출할 SPDK bdev 이름.
	 * 설정자: spdk_json_decode_string. 읽는 자: ublk_start_disk.
	 * 동기화: 단일 스레드 처리. */

	uint32_t	ublk_id;
	/* [한국어] 부여할 ublk ID(=/dev/ublkb<id>).
	 * 설정자: spdk_json_decode_uint32. */

	uint32_t	num_queues;
	/* [한국어] io_uring 기반 큐 수. 디폴트 UBLK_DEV_NUM_QUEUE(1).
	 * 설정자: 디코더 또는 디폴트 값. */

	uint32_t	queue_depth;
	/* [한국어] 큐별 IO depth. 디폴트 UBLK_DEV_QUEUE_DEPTH(128). */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 비동기 응답을 위해 보관하는 RPC 요청 핸들. 콜백에서 사용. */
};

/*
 * [한국어] rpc_ublk_start_disk_decoders — 입력 JSON 매핑.
 *  bdev_name과 ublk_id는 필수(=4번째 인자 생략 → 0=mandatory).
 *  num_queues/queue_depth는 옵션.
 */
static const struct spdk_json_object_decoder rpc_ublk_start_disk_decoders[] = {
	{"bdev_name", offsetof(struct rpc_ublk_start_disk, bdev_name), spdk_json_decode_string},
	/* [한국어] 필수 — bdev 이름이 없으면 의미 없는 호출. */
	{"ublk_id", offsetof(struct rpc_ublk_start_disk, ublk_id), spdk_json_decode_uint32},
	/* [한국어] 필수 — 어떤 /dev/ublkbN을 만들지 결정. */
	{"num_queues", offsetof(struct rpc_ublk_start_disk, num_queues), spdk_json_decode_uint32, true},
	/* [한국어] 옵션 — 미지정 시 핸들러가 디폴트 1로 채움. */
	{"queue_depth", offsetof(struct rpc_ublk_start_disk, queue_depth), spdk_json_decode_uint32, true},
	/* [한국어] 옵션 — 미지정 시 핸들러가 디폴트 128로 채움. */
};

/*
 * [한국어]
 * free_rpc_ublk_start_disk - start_disk 컨텍스트 해제(strdup된 bdev_name + 컨테이너).
 */
static void
free_rpc_ublk_start_disk(struct rpc_ublk_start_disk *req)
{
	free(req->bdev_name);
	free(req);
}

/*
 * [한국어]
 * rpc_ublk_start_disk_done - ublk_start_disk 비동기 완료 콜백.
 *
 * @cb_arg: rpc_ublk_start_disk 컨텍스트.
 * @rc: 0 성공 / 음수 errno.
 *
 * 동작: 성공 시 ublk_id를 결과로 반환, 실패 시 에러 응답. 컨텍스트 자원 해제.
 *
 * 호출 컨텍스트: ublk 코어 처리 스레드(보통 main reactor) — 콜백이 만들어낸 응답은
 *  rpc 서버 송신 큐를 통해 클라이언트로 직렬화되어 나간다.
 */
static void
rpc_ublk_start_disk_done(void *cb_arg, int rc)
{
	struct rpc_ublk_start_disk *req = cb_arg;
	struct spdk_json_write_ctx *w;

	if (rc == 0) {
		/* [한국어] 성공 응답 — 결과 본문에 ublk_id를 uint32로 직렬화.
		 *  spdk_jsonrpc_begin_result로 응답 빌더를 받아 값을 쓰고 end_result로 송신. */
		w = spdk_jsonrpc_begin_result(req->request);
		spdk_json_write_uint32(w, req->ublk_id);
		spdk_jsonrpc_end_result(req->request, w);
	} else {
		/* [한국어] 에러 응답. JSON-RPC 에러 코드로 errno를 그대로 사용(음수 그대로 전달). */
		spdk_jsonrpc_send_error_response(req->request, rc, spdk_strerror(-rc));
	}

	/* [한국어] 컨텍스트 정리 — 핸들러에서 calloc된 메모리. */
	free_rpc_ublk_start_disk(req);
}

/*
 * [한국어]
 * rpc_ublk_start_disk - "ublk_start_disk" RPC 핸들러 (비동기).
 *
 * 동작 순서:
 *  1) 컨텍스트 calloc + 디폴트값 설정.
 *  2) JSON 디코드.
 *  3) ublk_start_disk 호출 — 성공 시 콜백 등록만 하고 반환, 실패 시 즉시 콜백 트리거.
 */
static void
rpc_ublk_start_disk(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_ublk_start_disk *req;
	int rc;

	/* [한국어] 비동기 처리 컨텍스트 — 핸들러 반환 후에도 살아있어야 하므로 힙. */
	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	/* [한국어] 콜백에서 응답을 보내야 하므로 RPC 요청 핸들 보관. */
	req->request = request;
	/* [한국어] 디폴트값 설정 — 디코더가 옵션 키를 채우지 않으면 이 값들이 유지. */
	req->queue_depth = UBLK_DEV_QUEUE_DEPTH;
	req->num_queues = UBLK_DEV_NUM_QUEUE;

	/* [한국어] params 디코드. 필수 키 누락 또는 타입 불일치 시 실패. */
	if (spdk_json_decode_object(params, rpc_ublk_start_disk_decoders,
				    SPDK_COUNTOF(rpc_ublk_start_disk_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto out;
	}

	/* [한국어] ublk 코어에 디스크 시작 요청. 콜백/컨텍스트도 함께 전달. */
	rc = ublk_start_disk(req->bdev_name, req->ublk_id, req->num_queues, req->queue_depth,
			     rpc_ublk_start_disk_done, req);
	if (rc != 0) {
		/* [한국어] 즉시 실패 — 콜백을 직접 호출해 에러 응답 + 자원 해제 통일.
		 *  이렇게 하면 성공/실패 양 경로의 정리가 한 곳(rpc_ublk_start_disk_done)에 모인다. */
		rpc_ublk_start_disk_done(req, rc);
	}

	return;

out:
	/* [한국어] 디코드 실패 경로 — 응답은 이미 보냈고 컨텍스트만 정리. */
	free_rpc_ublk_start_disk(req);
}

SPDK_RPC_REGISTER("ublk_start_disk", rpc_ublk_start_disk, SPDK_RPC_RUNTIME)

/*
 * [한국어] struct rpc_ublk_stop_disk — `ublk_stop_disk` 입력 + 비동기 컨텍스트.
 */
struct rpc_ublk_stop_disk {
	uint32_t ublk_id;
	/* [한국어] 중지할 ublk 디스크 ID. 설정자: 디코더. 읽는 자: ublk_stop_disk. */
	struct spdk_jsonrpc_request *request;
	/* [한국어] 비동기 응답용 RPC 요청 핸들. */
};

/*
 * [한국어]
 * free_rpc_ublk_stop_disk - 컨텍스트 해제(strdup 필드가 없으므로 free만).
 */
static void
free_rpc_ublk_stop_disk(struct rpc_ublk_stop_disk *req)
{
	free(req);
}

/*
 * [한국어] rpc_ublk_stop_disk_decoders — 단일 필드 ublk_id (필수).
 */
static const struct spdk_json_object_decoder rpc_ublk_stop_disk_decoders[] = {
	{"ublk_id", offsetof(struct rpc_ublk_stop_disk, ublk_id), spdk_json_decode_uint32},
	/* [한국어] 필수 — 어떤 디스크를 중지할지. */
};

/*
 * [한국어]
 * rpc_ublk_stop_disk_done - ublk_stop_disk 비동기 완료 콜백.
 *
 * 주의: 이 함수는 rc(완료 코드)를 받지만 항상 true 응답을 보낸다.
 *  현재 구현상 stop의 부분 실패는 상위에서 따로 보고하지 않으므로 단순히 종료 표시만 한다.
 */
static void
rpc_ublk_stop_disk_done(void *cb_arg, int rc)
{
	struct rpc_ublk_stop_disk *req = cb_arg;

	/* [한국어] 클라이언트에 종료 알림. rc 무시(코드 디자인상). */
	spdk_jsonrpc_send_bool_response(req->request, true);
	free_rpc_ublk_stop_disk(req);
}

/*
 * [한국어]
 * rpc_ublk_stop_disk - "ublk_stop_disk" RPC 핸들러 (비동기).
 *
 * 동작: start_disk와 동일한 패턴 — 컨텍스트 할당 → 디코드 → ublk_stop_disk 호출.
 *  단 stop은 즉시 실패 코드(rc!=0)를 받으면 콜백을 호출하지 않고 직접 에러 응답.
 */
static void
rpc_ublk_stop_disk(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_ublk_stop_disk *req;
	int rc;

	/* [한국어] 비동기 처리용 컨텍스트 — 콜백 시점까지 보존. */
	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	req->request = request;

	/* [한국어] params 디코드 — ublk_id 누락 시 실패. */
	if (spdk_json_decode_object(params, rpc_ublk_stop_disk_decoders,
				    SPDK_COUNTOF(rpc_ublk_stop_disk_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto invalid;
	}

	/* [한국어] ublk 코어에 디스크 정지 요청. */
	rc = ublk_stop_disk(req->ublk_id, rpc_ublk_stop_disk_done, req);
	if (rc) {
		/* [한국어] 즉시 실패 — 에러 응답 후 컨텍스트만 정리. 콜백은 호출되지 않음. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto invalid;
	}
	return;

invalid:
	free_rpc_ublk_stop_disk(req);
}

SPDK_RPC_REGISTER("ublk_stop_disk", rpc_ublk_stop_disk, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_dump_ublk_info - 한 ublk 디스크의 정보를 JSON 객체로 직렬화.
 *
 * @w: 응답 빌더(외부에서 array_begin 상태). 본 함수는 객체 하나를 추가.
 * @ublk: 직렬화할 디스크 핸들.
 *
 * 출력 JSON 스키마:
 *   {
 *     "ublk_device": "/dev/ublkbN",
 *     "id": <ublk_id>,
 *     "queue_depth": ...,
 *     "num_queues": ...,
 *     "bdev_name": "..."
 *   }
 */
static void
rpc_dump_ublk_info(struct spdk_json_write_ctx *w,
		   struct spdk_ublk_dev *ublk)
{
	char ublk_path[32];
	/* [한국어] /dev/ublkb<id> 경로 빌드용 버퍼. ublk_id가 uint32라 32바이트면 충분. */

	/* [한국어] "/dev/ublkb" 접두 + id 십진수 결합. snprintf로 truncation-safe. */
	snprintf(ublk_path, 32, "%s%u", "/dev/ublkb", ublk_dev_get_id(ublk));
	/* [한국어] JSON object 시작 — `{`. */
	spdk_json_write_object_begin(w);

	/* [한국어] 키-값 쌍들을 일괄 추가. spdk_json_write_named_*는 "key": value 한 쌍을 출력. */
	spdk_json_write_named_string(w, "ublk_device", ublk_path);
	spdk_json_write_named_uint32(w, "id", ublk_dev_get_id(ublk));
	spdk_json_write_named_uint32(w, "queue_depth", ublk_dev_get_queue_depth(ublk));
	spdk_json_write_named_uint32(w, "num_queues", ublk_dev_get_num_queues(ublk));
	spdk_json_write_named_string(w, "bdev_name", ublk_dev_get_bdev_name(ublk));

	/* [한국어] JSON object 종료 — `}`. */
	spdk_json_write_object_end(w);
}

/*
 * [한국어] struct rpc_ublk_get_disks — get_disks RPC 입력.
 *  ublk_id가 0(=옵션)이면 전체 디스크 enumeration, 아니면 특정 디스크 1개 조회.
 */
struct rpc_ublk_get_disks {
	uint32_t ublk_id;
	/* [한국어] 0이면 전체 enumeration, 그 외면 특정 디스크 ID 검색.
	 * 설정자: 디코더(옵션). 읽는 자: 핸들러. */
};

/*
 * [한국어] rpc_ublk_get_disks_decoders — 단일 옵션 필드.
 *  4번째 true = 옵션. params 자체가 NULL이면 모든 디스크 dump.
 */
static const struct spdk_json_object_decoder rpc_ublk_get_disks_decoders[] = {
	{"ublk_id", offsetof(struct rpc_ublk_get_disks, ublk_id), spdk_json_decode_uint32, true},
};

/*
 * [한국어]
 * rpc_ublk_get_disks - "ublk_get_disks" RPC 핸들러 (동기).
 *
 * 동작: 입력 ublk_id의 유무에 따라 특정/전체 디스크 정보를 JSON 배열로 반환.
 *  파라미터로 디스크가 지정되었지만 존재하지 않으면 -ENODEV 에러 응답.
 *
 * 호출 컨텍스트: rpc 서버 스레드(main reactor). 동기 처리 — 콜백 없음.
 */
static void
rpc_ublk_get_disks(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_ublk_get_disks req = {};                /* [한국어] 입력 영확장. ublk_id=0 default. */
	struct spdk_json_write_ctx *w;                     /* [한국어] 응답 빌더. */
	struct spdk_ublk_dev *ublk = NULL;                 /* [한국어] 단일 디스크 모드일 때 검색 결과. */

	if (params != NULL) {
		/* [한국어] params가 있다면 ublk_id를 파싱. */
		if (spdk_json_decode_object(params, rpc_ublk_get_disks_decoders,
					    SPDK_COUNTOF(rpc_ublk_get_disks_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			return;
		}

		if (req.ublk_id) {
			/* [한국어] ublk_id가 0이 아니면 단일 디스크 검색.
			 *  주의: ublk_id=0인 디스크는 명시 조회 불가(=전체 모드와 구분 불가) — 디자인상 한계. */
			ublk = ublk_dev_find_by_id(req.ublk_id);
			if (ublk == NULL) {
				SPDK_ERRLOG("ublk device '%d' does not exist\n", req.ublk_id);
				/* [한국어] -ENODEV: "디바이스 없음" 표준 errno. JSON-RPC 에러 코드로 사용. */
				spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
				return;
			}
		}
	}

	/* [한국어] 응답 빌더 초기화 — JSON-RPC result 시작. */
	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] 결과는 디스크 정보 배열. `[`. */
	spdk_json_write_array_begin(w);

	if (ublk != NULL) {
		/* [한국어] 단일 디스크 모드 — 그 디스크 하나만 dump. */
		rpc_dump_ublk_info(w, ublk);
	} else {
		/* [한국어] 전체 enumeration — first/next로 등록된 모든 디스크 순회. */
		for (ublk = ublk_dev_first(); ublk != NULL; ublk = ublk_dev_next(ublk)) {
			rpc_dump_ublk_info(w, ublk);
		}
	}

	/* [한국어] `]` 닫고 응답 송신. */
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

	return;
}
SPDK_RPC_REGISTER("ublk_get_disks", rpc_ublk_get_disks, SPDK_RPC_RUNTIME)

/*
 * [한국어] struct rpc_ublk_recover_disk — recover_disk 입력 + 비동기 컨텍스트.
 *  ublk_start_disk와 거의 동일한 입력이지만 num_queues/queue_depth는 기존 ublk 컨트롤
 *  상태에서 가져오므로 RPC 입력이 필요 없다.
 */
struct rpc_ublk_recover_disk {
	char		*bdev_name;
	/* [한국어] 다시 연결할 SPDK bdev 이름. */
	uint32_t	ublk_id;
	/* [한국어] 회복할 ublk ID(이미 커널에 등록되어 있는 디스크). */
	struct spdk_jsonrpc_request *request;
	/* [한국어] 비동기 응답용 RPC 핸들. */
};

/*
 * [한국어] rpc_ublk_recover_disk_decoders — 두 필드 모두 필수.
 */
static const struct spdk_json_object_decoder rpc_ublk_recover_disk_decoders[] = {
	{"bdev_name", offsetof(struct rpc_ublk_recover_disk, bdev_name), spdk_json_decode_string},
	{"ublk_id", offsetof(struct rpc_ublk_recover_disk, ublk_id), spdk_json_decode_uint32},
};

/*
 * [한국어]
 * free_rpc_ublk_recover_disk - 컨텍스트 해제.
 */
static void
free_rpc_ublk_recover_disk(struct rpc_ublk_recover_disk *req)
{
	free(req->bdev_name);
	free(req);
}

/*
 * [한국어]
 * rpc_ublk_recover_disk_done - ublk_start_disk_recovery 완료 콜백 역할.
 *
 * 주의: 현재 구현상 핸들러는 ublk_start_disk_recovery에 NULL 콜백을 넘기고
 *  호출 직후 본 함수를 직접 호출한다. 즉 동기적 처리 흐름이며 "콜백" 명칭은 형식적.
 *
 * @cb_arg: rpc_ublk_recover_disk 컨텍스트.
 * @rc: 0 / 음수 errno.
 */
static void
rpc_ublk_recover_disk_done(void *cb_arg, int rc)
{
	struct rpc_ublk_recover_disk *req = cb_arg;
	struct spdk_json_write_ctx *w;

	if (rc == 0) {
		/* [한국어] 성공 — 결과 본문에 ublk_id를 uint32로 반환. */
		w = spdk_jsonrpc_begin_result(req->request);
		spdk_json_write_uint32(w, req->ublk_id);
		spdk_jsonrpc_end_result(req->request, w);
	} else {
		/* [한국어] 실패 — errno → JSON-RPC 에러 응답. */
		spdk_jsonrpc_send_error_response(req->request, rc, spdk_strerror(-rc));
	}

	free_rpc_ublk_recover_disk(req);
}

/*
 * [한국어]
 * rpc_ublk_recover_disk - "ublk_recover_disk" RPC 핸들러.
 *
 * 동작:
 *  1) 컨텍스트 calloc.
 *  2) 디코드.
 *  3) ublk_start_disk_recovery 호출 — 콜백 인자에 NULL을 넘기는 점에 주목
 *     (현재 구현은 동기적 결과를 사용).
 *  4) rpc_ublk_recover_disk_done(req, rc)를 직접 호출해 응답.
 *
 * 호출 체인: rpc 서버 → [rpc_ublk_recover_disk] → ublk_start_disk_recovery
 *                                              → rpc_ublk_recover_disk_done(즉시)
 */
static void
rpc_ublk_recover_disk(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_ublk_recover_disk *req;
	int rc;

	/* [한국어] 컨텍스트 할당. */
	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	req->request = request;

	/* [한국어] 입력 디코드 — 두 필드 모두 필수. */
	if (spdk_json_decode_object(params, rpc_ublk_recover_disk_decoders,
				    SPDK_COUNTOF(rpc_ublk_recover_disk_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		/* [한국어] 디코드 실패 — 부분 할당된 bdev_name이 있을 수 있어 free 직접 호출 대신
		 *  그냥 free(req)만 부르는 점 주의(현재 구현). */
		free(req);
		return;
	}

	/* [한국어] ublk 코어의 회복 진입점 호출. ctrl_cb=NULL, ctrl_arg=NULL 전달.
	 *  현 구현은 콜백을 사용하지 않고 동기 반환만으로 결과를 판단. */
	rc = ublk_start_disk_recovery(req->bdev_name, req->ublk_id, NULL, NULL);
	/* [한국어] rc로 응답 송신 + 자원 해제. */
	rpc_ublk_recover_disk_done(req, rc);
}

SPDK_RPC_REGISTER("ublk_recover_disk", rpc_ublk_recover_disk, SPDK_RPC_RUNTIME)
