/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] vfu_tgt(vfio-user 타깃) JSON-RPC 핸들러 (tgt_rpc.c)
 *
 * === 파일의 역할 ===
 * vfu_tgt 프레임워크가 클라이언트(QEMU 등)와 통신할 Unix 도메인 소켓의 "베이스 디렉토리"
 * 경로를 RPC를 통해 설정할 수 있게 하는 단일 메서드 "vfu_tgt_set_base_path"를 등록한다.
 * 실제 endpoint 생성/삭제 RPC는 module/vfu_device/* (예: vfio_user_nvme_create) 같은
 * 도메인 모듈 측에서 별도로 등록되며, 본 파일은 코어 차원의 베이스 경로 한 가지만 다룬다.
 * 베이스 경로가 정해져야 endpoint 생성 시 "<base>/<endpoint name>" 형태의 소켓 경로가
 * 합성되므로, 일반적으로 다른 vfu_* RPC보다 먼저 호출되어야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   사용자(rpc.py) → SPDK JSON-RPC 서버(lib/jsonrpc + lib/rpc)
 *     → 본 파일이 등록한 핸들러 rpc_vfu_tgt_set_base_path
 *       → spdk_vfu_set_socket_path (lib/vfu_tgt/tgt.c)
 *         → 전역 g_socket_path 갱신
 *           → 이후 spdk_vfu_create_endpoint 호출 시 이 경로 prefix 사용
 * RPC 등록 단계는 SPDK_RPC_RUNTIME — SPDK 부팅 완료 후 호출 가능.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/jsonrpc(요청 객체), spdk/rpc(SPDK_RPC_REGISTER), spdk/util(SPDK_COUNTOF),
 *   spdk/string(spdk_strerror), spdk/vfu_target(spdk_vfu_set_socket_path), tgt_internal.h.
 * - 의존받음: 사용자 RPC 클라이언트.
 * - 데이터 흐름: 클라이언트 JSON 디코딩 → 코어에 path 전달 → bool true/error 응답.
 * - 공유 자료구조: lib/vfu_tgt/tgt.c의 전역 g_socket_path(static char[]) — 단 한 번 설정.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rpc_set_vfu_path           : RPC 입력("path") 디코딩용 임시 컨테이너.
 * - rpc_vfu_tgt_set_base_path_decoders: JSON 필드 → C 멤버 매핑 디코더 테이블.
 * - free_rpc_set_vfu_path             : 디코드 결과 해제 헬퍼(strdup된 path 회수).
 * - rpc_vfu_tgt_set_base_path         : "vfu_tgt_set_base_path" 메서드 핸들러.
 */

#include "spdk/bdev.h"
/* [한국어] 본 파일에서 직접 사용하지 않지만, 다수의 헤더가 spdk_bdev_io 의존을 가지므로
 * 호환성/관용 차원에서 포함. (현재 코드 경로엔 직접 호출 없음.) */

#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG/INFOLOG 등 로그 매크로 — 디코드 실패 등 에러 메시지에 사용. */

#include "spdk/rpc.h"
/* [한국어] SPDK_RPC_REGISTER 매크로와 RPC 단계(STARTUP/RUNTIME) 정의. */

#include "spdk/env.h"
/* [한국어] DPDK 환경 헬퍼(메모리/CPU 등) — 직접 사용은 없으나 관용적 include. */

#include "spdk/string.h"
/* [한국어] spdk_strerror() — errno 코드를 사람이 읽을 수 있는 문자열로 변환해 RPC 에러 응답에 사용. */

#include "spdk/util.h"
/* [한국어] SPDK_COUNTOF(array) — 디코더 배열의 원소 개수를 컴파일 타임에 산출. */

#include "spdk/thread.h"
/* [한국어] SPDK thread API — 본 파일은 직접 호출하지 않지만 RPC 핸들러가 일반적으로 같은
 * thread 컨텍스트에 의존하므로 관용 include. */

#include "tgt_internal.h"
/* [한국어] 동일 라이브러리 내부 헤더 — struct spdk_vfu_endpoint 정의 가시화. */

struct rpc_set_vfu_path {
	/* [한국어] "vfu_tgt_set_base_path" RPC 입력 JSON을 디코딩할 임시 구조체.
	 * 호출 한 번 동안만 스택에 살아 있으며, free_rpc_set_vfu_path에서 path 해제. */

	char		*path;
	/* [한국어] 클라이언트가 지정한 vfio-user 소켓 베이스 디렉토리 경로(예: "/var/tmp").
	 * 설정자: spdk_json_decode_string가 strdup으로 새 버퍼를 채워 넣음.
	 * 읽는 자: spdk_vfu_set_socket_path()가 내부 g_socket_path로 복사.
	 * 값 범위: NUL 종결 절대 경로 — 디렉토리는 사전에 존재해야 함(코어가 검증).
	 * 동기화: RPC 한 호출의 로컬 변수 안에서만 살아 있으므로 race 없음. */
};

static const struct spdk_json_object_decoder rpc_vfu_tgt_set_base_path_decoders[] = {
	/* [한국어] 디코더 테이블 — JSON 객체의 "path" 키를 struct rpc_set_vfu_path::path 필드로 매핑.
	 * { JSON 키, 멤버 오프셋, 디코더 함수 } 순서. 4번째 요소가 없으면 기본값(필수=required). */
	{"path", offsetof(struct rpc_set_vfu_path, path), spdk_json_decode_string }
	/* [한국어] "path" 문자열을 strdup으로 복사 — 사용 후 반드시 free_rpc_set_vfu_path로 해제 필요. */
};

/*
 * [한국어]
 * free_rpc_set_vfu_path - rpc_set_vfu_path::path가 strdup으로 잡혔던 버퍼를 해제.
 *
 * @req: 디코드 결과 컨테이너(스택 변수의 주소).
 *
 * spdk_json_decode_string는 내부에서 strdup-like로 새 버퍼를 잡으므로 호출자가 명시적으로
 * 해제해야 한다. NULL 안전성은 free(NULL)가 no-op이라는 표준 보장으로 처리.
 *
 * 호출 체인:
 *   rpc_vfu_tgt_set_base_path → [free_rpc_set_vfu_path] → libc free
 */
static void
free_rpc_set_vfu_path(struct rpc_set_vfu_path *req)
{
	free(req->path);
	/* [한국어] strdup된 path 해제 — req 자체는 호출자 스택에 있으므로 따로 free하지 않음. */
}

/*
 * [한국어]
 * rpc_vfu_tgt_set_base_path - "vfu_tgt_set_base_path" RPC 핸들러.
 *
 * @request: JSON-RPC 요청 객체 (응답 빌더 입력).
 * @params : { "path": "<dir>" } 형태의 JSON 인자.
 *
 * 1) JSON 디코드 → 실패 시 INVALID_PARAMS 에러 응답.
 * 2) spdk_vfu_set_socket_path()를 호출해 코어가 경로 검증 + 전역에 저장.
 * 3) 성공 시 bool true 응답, 실패 시 errno → 사람이 읽는 메시지로 변환해 응답.
 *
 * 실행 컨텍스트: SPDK JSON-RPC 서버 thread (보통 마스터 reactor).
 *
 * 호출 체인:
 *   JSON-RPC 서버 → [rpc_vfu_tgt_set_base_path] → spdk_vfu_set_socket_path
 */
static void
rpc_vfu_tgt_set_base_path(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_set_vfu_path req = {0};
	/* [한국어] 디코드 결과 컨테이너 — 0 초기화로 path=NULL 보장(에러 경로에서 free(NULL) 안전). */
	int rc;
	/* [한국어] set_socket_path() 결과 / 디코드 실패 코드 임시 저장 — 음수 errno 형태. */

	if (spdk_json_decode_object(params, rpc_vfu_tgt_set_base_path_decoders,
				    SPDK_COUNTOF(rpc_vfu_tgt_set_base_path_decoders),
				    &req)) {
		/* [한국어] JSON 디코드 시도 — 0이 성공, non-zero가 실패.
		 * 실패 사유: 필수 필드 누락, 타입 불일치, JSON 구문 오류 등. */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		/* [한국어] 운영자가 즉시 인지할 수 있도록 ERRLOG에 기록. */
		rc = -EINVAL;
		/* [한국어] 표준 errno EINVAL(잘못된 인자) — 아래 invalid 라벨에서 strerror로 변환됨. */
		goto invalid;
		/* [한국어] 공통 정리/에러 응답 경로로 점프 — req 해제도 함께 수행. */
	}

	rc = spdk_vfu_set_socket_path(req.path);
	/* [한국어] 코어에 베이스 경로 등록 요청 — 디렉토리 존재/길이 검증 + 전역 g_socket_path에 복사.
	 * 0=성공, 음수=errno (예: -ENAMETOOLONG, -ENOENT). */
	if (rc < 0) {
		goto invalid;
		/* [한국어] 실패 시 동일 cleanup/응답 경로로 — rc는 이미 음수 errno 상태. */
	}
	free_rpc_set_vfu_path(&req);
	/* [한국어] 성공 경로에서도 디코드 시 strdup된 path는 더 이상 필요 없음(코어가 사본을 가짐) → 해제. */

	spdk_jsonrpc_send_bool_response(request, true);
	/* [한국어] SPDK RPC의 관용적 성공 신호 — 단일 bool true JSON 응답. */
	return;

invalid:
	free_rpc_set_vfu_path(&req);
	/* [한국어] 디코드 부분 성공 후 실패한 경우라도 path가 잡혔을 수 있으니 항상 해제. */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	/* [한국어] 표준 JSON-RPC INVALID_PARAMS(-32602) + errno → 메시지 변환.
	 * spdk_strerror는 음수가 아닌 양수 errno를 받으므로 -rc 적용. */
}
SPDK_RPC_REGISTER("vfu_tgt_set_base_path", rpc_vfu_tgt_set_base_path, SPDK_RPC_RUNTIME)
/* [한국어] RPC 메서드 "vfu_tgt_set_base_path" 등록 — RUNTIME 단계 한정.
 * 매크로는 컴파일 타임에 .init_array 콜백을 등록해 SPDK 시작 시 핸들러 테이블에 자동 추가.
 * RUNTIME으로 둔 이유: 부팅 직후 사용자 RPC가 도착할 수 있는 일반 운용 시점에 호출 가능하게. */
