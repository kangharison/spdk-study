/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] bdev_nvme CUSE RPC 핸들러 (bdev_nvme_cuse_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK가 관리 중인 NVMe 컨트롤러를 리눅스 사용자 공간에서 표준 NVMe
 * 캐릭터 디바이스(/dev/nvmeX)처럼 보이게 하는 CUSE(Character device in USErspace)
 * 등록/해제를 수행하는 두 개의 JSON-RPC 핸들러를 정의한다. CUSE는 FUSE의 캐릭터
 * 디바이스 변형으로, libfuse가 호스트 커널에 가짜 nvmeX 디바이스를 등록하면
 * `nvme list`나 `nvme id-ctrl` 같은 일반 NVMe CLI 도구가 SPDK 점유 디바이스에도
 * 동작할 수 있다. SPDK가 PCIe NVMe를 점유하면 커널 nvme 드라이버가 unbind되어
 * /dev/nvmeX가 사라지므로, 이 RPC가 그 공백을 메우는 운영 편의 기능이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   외부 RPC 클라이언트 (예: scripts/rpc.py bdev_nvme_cuse_register)
 *     → SPDK JSON-RPC 서버 (lib/jsonrpc, lib/rpc)
 *     → rpc_bdev_nvme_cuse_register() / rpc_bdev_nvme_cuse_unregister()
 *     → spdk_nvme_cuse_register() / spdk_nvme_cuse_unregister() (lib/nvme/nvme_cuse.c)
 *     → libfuse → 커널 cuse 모듈이 /dev/spdk/nvmeX 디바이스 노드 생성/제거.
 * 실행 컨텍스트: SPDK app 스레드 (RPC 서버 콜백). 이 핸들러는 동기적으로 nvme_ctrlr를
 * lookup하고 즉시 spdk_nvme_cuse_*를 호출한 뒤 응답을 보낸다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: bdev_nvme.h (nvme_ctrlr 정의 및 nvme_ctrlr_get_by_name lookup),
 *         spdk/nvme.h (spdk_nvme_cuse_register/unregister 선언),
 *         spdk/rpc.h (SPDK_RPC_REGISTER 매크로, JSON-RPC 응답 함수),
 *         spdk/util.h (SPDK_COUNTOF), spdk/string.h (spdk_strerror),
 *         spdk/log.h (SPDK_ERRLOG).
 * - 의존받음: 별도 헤더 없음. SPDK_RPC_REGISTER 매크로가 RPC 메서드 테이블에 등록.
 * - 데이터 흐름: 클라이언트의 JSON {"name": "Nvme0"} → req 구조체 → nvme_ctrlr 포인터 →
 *               libnvme cuse 등록 → 응답 bool 반환.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_bdev_nvme_cuse_register: "bdev_nvme_cuse_register" 메서드 핸들러.
 *   주어진 이름의 NVMe 컨트롤러에 대해 CUSE 디바이스 노드를 생성한다.
 * - rpc_bdev_nvme_cuse_unregister: "bdev_nvme_cuse_unregister" 메서드 핸들러.
 *   기존에 등록된 CUSE 노드를 제거한다.
 * - struct rpc_nvme_cuse_register / rpc_nvme_cuse_unregister: JSON 디코딩용 임시 구조체.
 * - free_rpc_*: 디코더가 strdup한 문자열을 해제하는 정리 헬퍼.
 */

#include "spdk/stdinc.h"        /* [한국어] 표준 C 헤더 묶음 (stdint.h, stdlib.h 등). */

#include "bdev_nvme.h"           /* [한국어] struct nvme_ctrlr와 nvme_ctrlr_get_by_name 사용. */

#include "spdk/string.h"        /* [한국어] spdk_strerror로 errno→문자열 변환. */
#include "spdk/rpc.h"            /* [한국어] SPDK_RPC_REGISTER, JSON-RPC 응답 헬퍼. */
#include "spdk/util.h"           /* [한국어] SPDK_COUNTOF (배열 길이 매크로). */
#include "spdk/nvme.h"           /* [한국어] spdk_nvme_cuse_register/unregister 선언. */

#include "spdk/log.h"            /* [한국어] SPDK_ERRLOG (RPC 실패 로그). */

/*
 * [한국어]
 * struct rpc_nvme_cuse_register - bdev_nvme_cuse_register RPC 입력 파라미터.
 *
 * JSON 스키마: {"name": <NVMe 컨트롤러 이름>}.
 */
struct rpc_nvme_cuse_register {
	char *name;
	/* [한국어] CUSE 노드를 생성할 대상 NVMe 컨트롤러의 이름.
	 * 설정자: spdk_json_decode_object()가 디코더 테이블을 통해 strdup으로 채움.
	 * 읽는 자: rpc_bdev_nvme_cuse_register()가 nvme_ctrlr_get_by_name 인자로 사용.
	 * 값 범위: NULL 불가 (디코더가 필수로 표시). 일반적으로 "Nvme0", "NvmeMyDev" 등.
	 * 동기화: 단일 RPC 콜백 내에서만 사용되므로 락 불필요. */
};

/*
 * [한국어]
 * free_rpc_nvme_cuse_register - 디코더가 strdup한 name 문자열을 해제한다.
 *
 * @req: 호출자 스택에 있는 임시 요청 구조체.
 * @return: 없음.
 *
 * spdk_json_decode_object()가 spdk_json_decode_string으로 strdup된 메모리를
 * 채워주므로, 핸들러 종료 시 이 헬퍼로 일괄 해제해야 누수가 없다. 호출자는
 * cleanup 라벨에서 항상 호출한다 (성공/실패 모두).
 */
static void
free_rpc_nvme_cuse_register(struct rpc_nvme_cuse_register *req)
{
	free(req->name);   /* [한국어] strdup된 문자열 해제. NULL이어도 free(NULL)은 안전. */
}

/*
 * [한국어] bdev_nvme_cuse_register RPC 입력 JSON 디코더 테이블.
 * 설정자: 컴파일 타임 상수.
 * 읽는 자: spdk_json_decode_object()가 이 테이블을 스캔해 각 필드를 채움.
 * 값 의미: {필드명, 구조체 내 오프셋, 디코더 함수}. name은 필수(optional 플래그 없음).
 */
static const struct spdk_json_object_decoder rpc_bdev_nvme_cuse_register_decoders[] = {
	{"name", offsetof(struct rpc_nvme_cuse_register, name), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_nvme_cuse_register - "bdev_nvme_cuse_register" JSON-RPC 핸들러.
 *
 * @request: SPDK JSON-RPC 서버가 만든 응답용 컨텍스트.
 * @params:  요청의 params 필드 (JSON 객체) 또는 NULL.
 * @return: 없음. 응답은 spdk_jsonrpc_send_*로 전송.
 *
 * 동작 단계:
 *   1) params를 rpc_nvme_cuse_register 구조체로 디코드.
 *   2) name으로 nvme_ctrlr_get_by_name lookup (대상 NVMe 컨트롤러 찾기).
 *   3) spdk_nvme_cuse_register로 libnvme에 CUSE 노드 생성 요청.
 *      → 성공 시 호스트 OS에 /dev/spdk/nvmeX 캐릭터 디바이스가 생성됨.
 *   4) bool true 응답 또는 적절한 에러 응답.
 * 실행 컨텍스트: SPDK app 스레드의 RPC 서버 콜백.
 * 호출 체인: JSON-RPC 서버 → [본 함수] → spdk_nvme_cuse_register → libfuse.
 */
static void
rpc_bdev_nvme_cuse_register(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_nvme_cuse_register req = {};   /* [한국어] 디코드 대상 구조체를 0으로 초기화 (free시 NULL 안전). */
	struct nvme_ctrlr *bdev_ctrlr = NULL;     /* [한국어] lookup 결과 저장. */
	int rc;                                    /* [한국어] spdk_nvme_cuse_register 반환값 (0 또는 음수 errno). */

	/* [한국어] 1단계: JSON params를 req 구조체로 디코딩. 실패 시 INTERNAL_ERROR 응답 후 cleanup. */
	if (spdk_json_decode_object(params, rpc_bdev_nvme_cuse_register_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_cuse_register_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");   /* [한국어] 운영자 디버깅용 로그. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;   /* [한국어] req.name 해제 후 종료. */
	}

	/* [한국어] 2단계: 이름으로 컨트롤러 lookup. g_nvme_bdev_ctrlrs를 순회. */
	bdev_ctrlr = nvme_ctrlr_get_by_name(req.name);
	if (!bdev_ctrlr) {
		SPDK_ERRLOG("No such controller\n");
		/* [한국어] -ENODEV를 음수 errno 코드로 그대로 전달 (RPC 응답에서 표준 에러 매핑). */
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	/* [한국어] 3단계: lib/nvme의 CUSE 헬퍼 호출. 내부에서 libfuse가 cuse_lowlevel_setup으로
	 * /dev/spdk/nvmeX 캐릭터 디바이스를 생성하고 ioctl 핸들러를 등록한다. */
	rc = spdk_nvme_cuse_register(bdev_ctrlr->ctrlr);
	if (rc) {
		SPDK_ERRLOG("Failed to register CUSE devices: %s\n", spdk_strerror(-rc));
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	/* [한국어] 4단계: 성공 응답 (단순 bool true). */
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	/* [한국어] 성공/실패 모두 디코더가 strdup한 문자열을 해제. */
	free_rpc_nvme_cuse_register(&req);
}
/* [한국어] SPDK 부팅 후 런타임 단계에서 등록되는 RPC 메서드 (SPDK_RPC_RUNTIME). */
SPDK_RPC_REGISTER("bdev_nvme_cuse_register", rpc_bdev_nvme_cuse_register, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_nvme_cuse_unregister - bdev_nvme_cuse_unregister RPC 입력 파라미터.
 *
 * JSON 스키마: {"name": <NVMe 컨트롤러 이름>} (name은 디코더에서 optional=true).
 */
struct rpc_nvme_cuse_unregister {
	char *name;
	/* [한국어] CUSE 노드를 제거할 대상 NVMe 컨트롤러 이름.
	 * 설정자: spdk_json_decode_object의 spdk_json_decode_string.
	 * 읽는 자: rpc_bdev_nvme_cuse_unregister에서 nvme_ctrlr_get_by_name 인자로 사용.
	 * 값 범위: 디코더 테이블에서 optional=true이지만, 빠지면 lookup 실패로 ENODEV 응답.
	 * 동기화: 단일 RPC 콜백 내. */
};

/*
 * [한국어] free_rpc_nvme_cuse_unregister - register 버전과 동일한 구조의 정리 헬퍼.
 * @req: cleanup 단계에서 전달되는 요청 구조체.
 * @return: 없음.
 */
static void
free_rpc_nvme_cuse_unregister(struct rpc_nvme_cuse_unregister *req)
{
	free(req->name);   /* [한국어] strdup된 문자열 해제. */
}

/*
 * [한국어] bdev_nvme_cuse_unregister RPC 입력 JSON 디코더 테이블.
 * name 항목의 마지막 true는 optional 표시이지만, 실제 사용 단계에서는 사실상 필수다.
 */
static const struct spdk_json_object_decoder rpc_bdev_nvme_cuse_unregister_decoders[] = {
	{"name", offsetof(struct rpc_nvme_cuse_unregister, name), spdk_json_decode_string, true},
};

/*
 * [한국어]
 * rpc_bdev_nvme_cuse_unregister - "bdev_nvme_cuse_unregister" JSON-RPC 핸들러.
 *
 * @request: 응답용 컨텍스트.
 * @params:  요청 JSON params.
 * @return: 없음.
 *
 * 동작은 register와 거의 동일하지만 spdk_nvme_cuse_unregister를 호출해 /dev 노드를
 * 제거한다. unregister는 비동기적으로 진행될 수도 있지만 이 핸들러에서는 동기 처리로
 * 보고 즉시 결과를 반환한다.
 * 실행 컨텍스트: SPDK app 스레드의 RPC 콜백.
 * 호출 체인: JSON-RPC 서버 → [본 함수] → spdk_nvme_cuse_unregister.
 */
static void
rpc_bdev_nvme_cuse_unregister(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_nvme_cuse_unregister req = {};   /* [한국어] 입력 디코드 결과 (0 초기화). */
	struct nvme_ctrlr *bdev_ctrlr = NULL;       /* [한국어] lookup 결과. */
	int rc;                                      /* [한국어] cuse_unregister 반환 코드. */

	/* [한국어] 1단계: JSON 디코딩. */
	if (spdk_json_decode_object(params, rpc_bdev_nvme_cuse_unregister_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_cuse_unregister_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 2단계: 컨트롤러 lookup. */
	bdev_ctrlr = nvme_ctrlr_get_by_name(req.name);
	if (!bdev_ctrlr) {
		SPDK_ERRLOG("No such controller\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	/* [한국어] 3단계: CUSE 노드 제거. 내부적으로 libfuse 세션 종료, /dev 노드 unlink. */
	rc = spdk_nvme_cuse_unregister(bdev_ctrlr->ctrlr);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	/* [한국어] 4단계: 성공 응답. */
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_nvme_cuse_unregister(&req);
}
SPDK_RPC_REGISTER("bdev_nvme_cuse_unregister", rpc_bdev_nvme_cuse_unregister, SPDK_RPC_RUNTIME)
