/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] delay 가상 bdev의 JSON-RPC 핸들러 (vbdev_delay_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK delay vbdev 모듈에 대한 JSON-RPC 인터페이스를 제공한다. delay 모듈은
 * 기존 base bdev 위에 한 층을 덧대어 모든 read/write에 사용자 지정 지연(평균/p99 분리)을
 * 주입하는 가상 디바이스로, QoS 검증·성능 회귀 테스트에 사용된다.
 * 본 파일은 다음 세 RPC를 등록한다:
 *   1) bdev_delay_create        — 새 delay vbdev 생성(초기 지연 파라미터 4개 함께 설정).
 *   2) bdev_delay_delete        — delay vbdev 제거.
 *   3) bdev_delay_update_latency — 가동 중인 delay vbdev의 지연 파라미터 동적 변경.
 * 핵심 로직은 vbdev_delay.c에 있고, 본 파일은 JSON ↔ C struct 매핑과 입력 검증만 담당.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [클라이언트] → SPDK RPC 디스패처 → [이 파일의 핸들러]
 *   → create_delay_disk / delete_delay_disk / vbdev_delay_update_latency_value (vbdev_delay.c)
 *   → spdk_bdev_register / unregister 등 lib/bdev API.
 * 실행 컨텍스트는 SPDK RPC poller(메인 reactor) — bdev 등록/해제는 단일 thread에 고정되어
 * 락-프리하게 처리된다. 지연 파라미터 갱신은 단일 정수 store이므로 atomic이 보장된다.
 *
 * === 타 모듈과의 연결 ===
 * - include: vbdev_delay.h(create/delete/update 선언, enum delay_io_type), spdk/rpc.h,
 *   spdk/util.h, spdk/string.h, spdk/log.h, spdk_internal/assert.h(SPDK_UNREACHABLE).
 * - 의존: vbdev_delay.c(실제 동작), lib/jsonrpc, lib/rpc, lib/bdev.
 * - 데이터 흐름: 클라이언트 JSON → 본 파일에서 디코딩/검증 → vbdev_delay.c → bdev 등록.
 *   응답은 단순 문자열(name) 또는 boolean true.
 *
 * === 주요 함수/구조체 요약 ===
 * - `struct rpc_construct_delay`             : create RPC 입력 (base, name, uuid, 4개 지연값).
 * - `struct rpc_delete_delay`                : delete RPC 입력 (name).
 * - `struct rpc_update_latency`              : update RPC 입력 (delay_bdev_name, latency_type, us).
 * - `rpc_bdev_delay_create()`                : create RPC 핸들러.
 * - `rpc_bdev_delay_delete()`                : delete RPC 핸들러(비동기 콜백 사용).
 * - `rpc_bdev_delay_update_latency()`        : 지연 파라미터 동적 변경 RPC 핸들러.
 * - `rpc_bdev_delay_delete_cb()`             : delete 비동기 완료 콜백.
 */

/* [한국어] delay 모듈 공개 헤더 — create_delay_disk, delete_delay_disk,
 *           vbdev_delay_update_latency_value 함수 선언 및 enum delay_io_type 정의. */
#include "vbdev_delay.h"
/* [한국어] SPDK_RPC_REGISTER 등 RPC 등록/응답 API. */
#include "spdk/rpc.h"
/* [한국어] SPDK_COUNTOF, offsetof 등 매크로. */
#include "spdk/util.h"
/* [한국어] spdk_strerror — errno → 문자열 변환. */
#include "spdk/string.h"
/* [한국어] SPDK_DEBUGLOG/ERRLOG. 컴포넌트 "vbdev_delay"는 vbdev_delay.c에서 등록. */
#include "spdk/log.h"
/* [한국어] SPDK_UNREACHABLE() — 도달해서는 안 되는 경로에서 abort/조용히 컴파일 힌트. */
#include "spdk_internal/assert.h"

/* [한국어] update_latency RPC 입력. JSON:
 *           { "delay_bdev_name": "Delay0", "latency_type": "avg_read", "latency_us": 1000 } */
struct rpc_update_latency {
	char *delay_bdev_name;
	/* [한국어] 갱신할 delay vbdev의 이름 (base bdev 이름 아님 — delay vbdev 자체).
	 * 설정자: spdk_json_decode_string. 읽는 자: vbdev_delay_update_latency_value().
	 * 값 범위: 비어있지 않은 문자열. NULL이면 디코딩 실패 경로로.
	 * 동기화: 단일 RPC 호출 내에서만. */

	char *latency_type;
	/* [한국어] 어떤 지연 채널을 바꿀지 — "avg_read"/"p99_read"/"avg_write"/"p99_write".
	 * 설정자: spdk_json_decode_string.
	 * 읽는 자: 본 파일의 strncmp 분기로 enum delay_io_type 매핑.
	 * 값 범위: 위 4개 문자열 중 하나 — 그 외에는 INVALID_PARAMS 응답.
	 * 동기화: 동일. */

	uint64_t latency_us;
	/* [한국어] 새로 적용할 지연(마이크로초). 0이면 지연 비활성화 효과.
	 * 설정자: spdk_json_decode_uint64.
	 * 읽는 자: vbdev_delay_update_latency_value()가 atomic store로 갱신.
	 * 값 범위: 0 이상. 너무 크면 (수백 ms+) 시스템 응답이 매우 느려짐.
	 * 동기화: per-vbdev 단일 store, atomic. */
};

/* [한국어] update_latency 디코더 테이블. 모두 필수(옵션 플래그 없음). */
static const struct spdk_json_object_decoder rpc_bdev_delay_update_latency_decoders[] = {
	{"delay_bdev_name", offsetof(struct rpc_update_latency, delay_bdev_name), spdk_json_decode_string},
	{"latency_type", offsetof(struct rpc_update_latency, latency_type), spdk_json_decode_string},
	{"latency_us", offsetof(struct rpc_update_latency, latency_us), spdk_json_decode_uint64}
};

/*
 * [한국어]
 * free_rpc_update_latency - update_latency RPC의 strdup 문자열을 해제.
 *
 * @req: 정리할 입력 구조체.
 * @return: 없음.
 *
 * 호출 체인: rpc_bdev_delay_update_latency cleanup → [이 함수].
 * 실행 컨텍스트: RPC poller.
 */
static void
free_rpc_update_latency(struct rpc_update_latency *req)
{
	free(req->delay_bdev_name);  /* [한국어] strdup된 vbdev 이름 해제(NULL 안전). */
	free(req->latency_type);     /* [한국어] strdup된 지연 종류 문자열 해제. */
}

/*
 * [한국어]
 * rpc_bdev_delay_update_latency - "bdev_delay_update_latency" RPC 핸들러.
 *
 * @request: RPC 응답 컨텍스트.
 * @params : JSON 입력.
 * @return : void.
 *
 * 동작:
 *   1) JSON 디코딩.
 *   2) latency_type 문자열을 enum delay_io_type으로 매핑(직접 strncmp).
 *   3) vbdev_delay_update_latency_value()로 vbdev 내부 atomic 변수 갱신.
 *   4) 결과에 따라 응답 코드 매핑:
 *        -ENODEV  → INVALID_PARAMS  ("bdev does not exist")
 *        -EINVAL  → INVALID_REQUEST ("bdev is not a delay bdev") — 이름이 다른 모듈의 bdev임
 *        그 외 0  이외 값 → SPDK_UNREACHABLE() (논리적으로 도달 불가)
 *   5) 성공 시 boolean true 응답.
 *
 * 호출 체인:
 *   RPC dispatcher → [이 함수] → vbdev_delay_update_latency_value() → atomic store.
 * 실행 컨텍스트: SPDK RPC poller. 갱신은 atomic 한 번이라 별도 락 없음.
 */
static void
rpc_bdev_delay_update_latency(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_update_latency req = {NULL};  /* [한국어] 입력 임시 — 모든 포인터 NULL 초기화. */
	enum delay_io_type latency_type;         /* [한국어] 매핑 결과 enum 값. */
	int rc = 0;                              /* [한국어] 갱신 함수 반환 코드. */

	/* [한국어] JSON 디코딩 실패 → INTERNAL_ERROR 응답. */
	if (spdk_json_decode_object(params, rpc_bdev_delay_update_latency_decoders,
				    SPDK_COUNTOF(rpc_bdev_delay_update_latency_decoders),
				    &req)) {
		SPDK_DEBUGLOG(vbdev_delay, "spdk_json_decode_object failed\n");  /* [한국어] 디버그 컴포넌트는 vbdev_delay.c에서 등록. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;  /* [한국어] 부분 디코딩된 문자열 free. */
	}

	/* [한국어] 문자열 → enum 매핑. strncmp의 두 번째 인자 길이는 각 토큰의 정확한 길이.
	 *           "avg_read" 8자 비교를 9로 한 것은 NUL 포함 여부 영향 없이 동치. */
	if (!strncmp(req.latency_type, "avg_read", 9)) {
		latency_type = DELAY_AVG_READ;        /* [한국어] 평균 읽기 지연 채널. */
	} else if (!strncmp(req.latency_type, "p99_read", 9)) {
		latency_type = DELAY_P99_READ;        /* [한국어] p99 읽기 지연 채널(꼬리 지연). */
	} else if (!strncmp(req.latency_type, "avg_write", 10)) {
		latency_type = DELAY_AVG_WRITE;       /* [한국어] 평균 쓰기 지연 채널. */
	} else if (!strncmp(req.latency_type, "p99_write", 10)) {
		latency_type = DELAY_P99_WRITE;       /* [한국어] p99 쓰기 지연 채널. */
	} else {
		/* [한국어] 정의되지 않은 latency_type → 클라이언트 잘못 입력. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Please specify a valid latency type.");
		goto cleanup;
	}

	/* [한국어] 핵심 호출 — vbdev 내부 atomic 변수에 새 값을 store.
	 *           반환값:
	 *             0       : 성공.
	 *             -ENODEV : 이름의 delay vbdev이 g_delay_nodes에 없음.
	 *             -EINVAL : 잘못된 type(정상 분기에서 발생 안 함 — 사전 검증으로 막힘). */
	rc = vbdev_delay_update_latency_value(req.delay_bdev_name, req.latency_us, latency_type);

	if (rc == -ENODEV) {
		/* [한국어] 이름의 bdev이 존재하지 않음 → 클라이언트 잘못 입력. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "The requested bdev does not exist.");
		goto cleanup;
	} else if (rc == -EINVAL) {
		/* [한국어] 이름이 존재하지만 delay 모듈 소유가 아님(다른 vbdev/bdev) → 잘못된 요청. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_REQUEST,
						 "The requested bdev is not a delay bdev.");
		goto cleanup;
	} else if (rc) {
		/* [한국어] 정의된 두 에러 외 다른 음수가 반환되면 논리 오류 — 디버그 빌드에서 abort. */
		SPDK_UNREACHABLE();
	}

	/* [한국어] 성공 응답. boolean true는 RPC 표준 OK 시그널. */
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_update_latency(&req);  /* [한국어] strdup 문자열 정리 — 성공/실패 무관 공통 경로. */
}
/* [한국어] "bdev_delay_update_latency" RPC를 디스패처에 등록. */
SPDK_RPC_REGISTER("bdev_delay_update_latency", rpc_bdev_delay_update_latency, SPDK_RPC_RUNTIME)

/* [한국어] bdev_delay_create RPC 입력. JSON:
 *           { "base_bdev_name":"Nvme0n1", "name":"Delay0", "uuid":"...",
 *             "avg_read_latency":100, "p99_read_latency":1000,
 *             "avg_write_latency":200, "p99_write_latency":2000 } */
struct rpc_construct_delay {
	char *base_bdev_name;
	/* [한국어] 위에 stack할 base bdev 이름.
	 * 설정자: 디코더 strdup. 읽는 자: create_delay_disk()가 spdk_bdev_get_by_name 호출에 사용.
	 * 값 범위: NULL 가능. 동기화: 단일 RPC 호출 내. */

	char *name;
	/* [한국어] 새 delay vbdev 이름 (전역 유일).
	 * 설정자: 디코더 strdup. 읽는 자: spdk_bdev_register의 bdev->name으로 설정. */

	struct spdk_uuid uuid;
	/* [한국어] vbdev에 부여할 UUID. 옵션(미지정 시 0 → 랜덤 UUID 생성됨).
	 * 설정자: spdk_json_decode_uuid. 읽는 자: create_delay_disk. */

	uint64_t avg_read_latency;
	/* [한국어] 초기 평균 읽기 지연(us).
	 * 설정자: 디코더. 읽는 자: vbdev 채널 초기화 시 atomic store.
	 * 값 범위: 0(즉시 완료) 이상. 동기화: 가동 중에는 update_latency RPC로 변경. */

	uint64_t p99_read_latency;
	/* [한국어] 초기 p99 읽기 지연(us). p99 트리거 비율은 vbdev_delay.c 내부 상수로 결정. */

	uint64_t avg_write_latency;
	/* [한국어] 초기 평균 쓰기 지연(us). */

	uint64_t p99_write_latency;
	/* [한국어] 초기 p99 쓰기 지연(us). */
};

/*
 * [한국어]
 * free_rpc_construct_delay - create RPC의 strdup 문자열 해제.
 *
 * @r: 정리 대상 입력 구조체.
 * @return: 없음.
 *
 * 호출 체인: rpc_bdev_delay_create cleanup → [이 함수]. 실행 컨텍스트: RPC poller.
 */
static void
free_rpc_construct_delay(struct rpc_construct_delay *r)
{
	free(r->base_bdev_name);  /* [한국어] base bdev 이름 strdup 해제. */
	free(r->name);            /* [한국어] vbdev 이름 strdup 해제. */
}

/* [한국어] create RPC 디코더 테이블. uuid는 옵션, 나머지는 모두 필수. */
static const struct spdk_json_object_decoder rpc_bdev_delay_create_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_construct_delay, base_bdev_name), spdk_json_decode_string},
	{"name", offsetof(struct rpc_construct_delay, name), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_construct_delay, uuid), spdk_json_decode_uuid, true},
	{"avg_read_latency", offsetof(struct rpc_construct_delay, avg_read_latency), spdk_json_decode_uint64},
	{"p99_read_latency", offsetof(struct rpc_construct_delay, p99_read_latency), spdk_json_decode_uint64},
	{"avg_write_latency", offsetof(struct rpc_construct_delay, avg_write_latency), spdk_json_decode_uint64},
	{"p99_write_latency", offsetof(struct rpc_construct_delay, p99_write_latency), spdk_json_decode_uint64},
};

/*
 * [한국어]
 * rpc_bdev_delay_create - "bdev_delay_create" RPC 핸들러.
 *
 * @request: RPC 컨텍스트.
 * @params : JSON 입력.
 * @return : void.
 *
 * 동작: JSON 디코딩 → create_delay_disk() 호출 → 성공 시 새 vbdev 이름을 응답.
 *
 * 호출 체인: RPC dispatcher → [이 함수] → create_delay_disk() (vbdev_delay.c)
 *                                          → spdk_bdev_open_ext + spdk_bdev_register.
 * 실행 컨텍스트: SPDK RPC poller.
 */
static void
rpc_bdev_delay_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_construct_delay req = {NULL};  /* [한국어] 입력 임시 저장소. */
	struct spdk_json_write_ctx *w;            /* [한국어] 응답 JSON writer. */
	int rc;                                   /* [한국어] create_delay_disk 반환 코드. */

	/* [한국어] JSON 디코딩 실패 → INTERNAL_ERROR. */
	if (spdk_json_decode_object(params, rpc_bdev_delay_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_delay_create_decoders),
				    &req)) {
		SPDK_DEBUGLOG(vbdev_delay, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 핵심 호출 — base bdev 위에 delay vbdev 등록. 4개 지연값을 초기치로 전달.
	 *           내부에서 p99 < avg 검증과 매핑 등록을 수행. ENODEV는 0으로 보정되어 반환됨. */
	rc = create_delay_disk(req.base_bdev_name, req.name, &req.uuid, req.avg_read_latency,
			       req.p99_read_latency,
			       req.avg_write_latency, req.p99_write_latency);
	if (rc != 0) {
		/* [한국어] EINVAL(p99<avg), EEXIST(이름 중복), ENOMEM 등 — 그대로 클라이언트에 전달. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	/* [한국어] 성공 응답 — 생성된 vbdev 이름을 단일 문자열로 회신. */
	w = spdk_jsonrpc_begin_result(request);     /* [한국어] 응답 writer 시작. */
	spdk_json_write_string(w, req.name);        /* [한국어] 결과: vbdev 이름 문자열. */
	spdk_jsonrpc_end_result(request, w);        /* [한국어] writer 종료 — TCP/Unix 소켓 전송. */

cleanup:
	free_rpc_construct_delay(&req);  /* [한국어] strdup 문자열 정리. */
}
/* [한국어] "bdev_delay_create" RPC 등록. */
SPDK_RPC_REGISTER("bdev_delay_create", rpc_bdev_delay_create, SPDK_RPC_RUNTIME)

/* [한국어] bdev_delay_delete RPC 입력 — 단일 필드 name. */
struct rpc_delete_delay {
	char *name;
	/* [한국어] 삭제 대상 delay vbdev 이름.
	 * 설정자: 디코더. 읽는 자: delete_delay_disk(). 동기화: 단일 RPC 호출 내. */
};

/*
 * [한국어]
 * free_rpc_delete_delay - delete RPC의 strdup 문자열 해제.
 *
 * @req: 정리 대상.
 * @return: 없음.
 */
static void
free_rpc_delete_delay(struct rpc_delete_delay *req)
{
	free(req->name);  /* [한국어] strdup된 이름 해제. */
}

/* [한국어] delete RPC 디코더 — 단일 필수 필드. */
static const struct spdk_json_object_decoder rpc_bdev_delay_delete_decoders[] = {
	{"name", offsetof(struct rpc_delete_delay, name), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_delay_delete_cb - bdev_unregister 비동기 완료 후 클라이언트에 응답하는 콜백.
 *
 * @cb_arg   : 보존된 spdk_jsonrpc_request 포인터.
 * @bdeverrno: 0=성공, 음수=errno.
 * @return   : 없음.
 *
 * 호출 체인: delete_delay_disk → spdk_bdev_unregister → 완료 → [이 콜백].
 * 실행 컨텍스트: bdev unregister가 종료되는 thread(보통 등록 thread).
 */
static void
rpc_bdev_delay_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;  /* [한국어] 보존된 RPC 요청 컨텍스트 복원. */

	if (bdeverrno == 0) {
		/* [한국어] 성공 → boolean true 응답. */
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		/* [한국어] 실패 → errno와 메시지로 에러 응답. */
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

/*
 * [한국어]
 * rpc_bdev_delay_delete - "bdev_delay_delete" RPC 핸들러.
 *
 * @request: RPC 컨텍스트.
 * @params : JSON 입력.
 * @return : void.
 *
 * delete_delay_disk()는 비동기 — 완료 시 cb가 응답을 회신. 본 함수는 cleanup 후 즉시 반환.
 *
 * 호출 체인: RPC dispatcher → [이 함수] → delete_delay_disk() (vbdev_delay.c)
 *                                          → spdk_bdev_unregister_by_name → ... → 콜백.
 * 실행 컨텍스트: SPDK RPC poller.
 */
static void
rpc_bdev_delay_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_delete_delay req = {NULL};  /* [한국어] 입력 임시 저장소. */

	/* [한국어] JSON 디코딩 실패 → INTERNAL_ERROR. */
	if (spdk_json_decode_object(params, rpc_bdev_delay_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_delay_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 비동기 unregister 시작. request를 cb_arg로 보존 — 콜백에서 응답 회신.
	 *           내부에서 spdk_bdev_unregister_by_name → 모든 채널 정리 → destruct 콜백 → 본 cb. */
	delete_delay_disk(req.name, rpc_bdev_delay_delete_cb, request);

cleanup:
	free_rpc_delete_delay(&req);  /* [한국어] 입력 임시 문자열 해제. */
}
/* [한국어] "bdev_delay_delete" RPC 등록. */
SPDK_RPC_REGISTER("bdev_delay_delete", rpc_bdev_delay_delete, SPDK_RPC_RUNTIME)
