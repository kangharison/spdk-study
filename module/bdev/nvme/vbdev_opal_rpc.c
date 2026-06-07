/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] vbdev_opal RPC 핸들러 (vbdev_opal_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 TCG Opal SED(Self-Encrypting Drive) 가상 bdev(vbdev_opal)의
 * 라이프사이클과 권한 관리를 외부에서 제어할 수 있도록 7개의 JSON-RPC 메서드를
 * 등록한다. 각 핸들러는 호스트 사용자(또는 관리 도구)가 보낸 JSON 명령(예:
 * "bdev_nvme_opal_init", "bdev_opal_create")을 파싱해 vbdev_opal.c의 C API에
 * 위임한다. SPDK가 NVMe SED 컨트롤러를 점유하면 호스트의 일반 sedutil/nvme-cli
 * 도구가 동작하지 않으므로, 본 RPC 인터페이스가 운영자가 키 관리, Locking Range
 * 생성/삭제, 잠금 해제를 수행할 유일한 진입점이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   외부 RPC 클라이언트 (scripts/rpc.py 등)
 *     → SPDK JSON-RPC 서버
 *     → 본 파일의 rpc_bdev_*_ 핸들러
 *     → vbdev_opal.h에 선언된 vbdev_opal_*() / spdk_opal_cmd_*() 호출
 *     → lib/nvme/opal: NVMe Security Send/Receive (opcode 0x81/0x82)로 SED 통신
 * 실행 컨텍스트: SPDK app 스레드의 RPC 콜백.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/rpc.h, spdk/util.h, spdk/string.h, spdk/log.h,
 *         spdk/opal.h (lib/nvme/opal 공개 API: spdk_opal_cmd_take_ownership 등),
 *         vbdev_opal.h (모듈 내부 API).
 * - 의존받음: 별도 헤더 없음. SPDK_RPC_REGISTER가 RPC 메서드 테이블에 등록.
 * - 데이터 흐름: 클라이언트 JSON {nvme_ctrlr_name, password, …}
 *               → req 구조체 → opal_dev 인증 → SED 명령 → bool 응답.
 * - 보안 주의: password 평문이 RPC 채널을 통해 전달되므로, 운영 환경에서는
 *               UNIX domain socket + 권한 제한 또는 TLS 보호된 RPC 사용을 가정.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_bdev_nvme_opal_init: SED Take Ownership + Locking SP Activate. SED를 처음 사용 가능 상태로 전환.
 * - rpc_bdev_nvme_opal_revert: TPer Revert. 모든 키/Locking Range를 공장 초기화 (데이터 소거).
 * - rpc_bdev_opal_create: 한 namespace의 LBA 범위에 Locking Range를 만들고 새 vbdev 등록.
 * - rpc_bdev_opal_get_info: Locking Range의 잠금/암호화 상태 조회.
 * - rpc_bdev_opal_delete: vbdev 제거.
 * - rpc_bdev_opal_set_lock_state: Locking Range를 RWLOCK/READONLY/RWUNLOCK 등으로 변경.
 * - rpc_bdev_opal_new_user: Admin 인증 후 새 User에 권한 부여.
 */

#include "spdk/rpc.h"        /* [한국어] SPDK_RPC_REGISTER, JSON-RPC 응답 헬퍼. */
#include "spdk/util.h"        /* [한국어] SPDK_COUNTOF (배열 길이 매크로). */
#include "spdk/string.h"      /* [한국어] spdk_strerror, spdk_sprintf_alloc. */
#include "spdk/log.h"         /* [한국어] SPDK_ERRLOG. */
#include "spdk/opal.h"        /* [한국어] lib/nvme의 Opal 드라이버 공개 API. */

#include "vbdev_opal.h"        /* [한국어] vbdev_opal_create/destruct/set_lock_state 등 모듈 API. */

/*
 * [한국어]
 * struct rpc_bdev_nvme_opal_init - bdev_nvme_opal_init RPC 입력.
 * JSON: {"nvme_ctrlr_name": "...", "password": "..."}.
 */
struct rpc_bdev_nvme_opal_init {
	char *nvme_ctrlr_name;
	/* [한국어] Opal SED를 초기화할 NVMe 컨트롤러 이름 (예: "Nvme0").
	 * 설정자: spdk_json_decode_object()가 spdk_json_decode_string()으로 strdup.
	 * 읽는 자: rpc_bdev_nvme_opal_init()에서 nvme_ctrlr_get_by_name() 인자로 사용.
	 * 값 범위: null-terminated C 문자열, NULL이면 디코딩 실패 (필수 필드).
	 * 동기화: 단일 RPC 콜백 내에서만 사용. free_rpc_*()가 해제. */

	char *password;
	/* [한국어] 새 SID(Security ID, Owner)로 설정할 패스워드 문자열.
	 * Take Ownership 절차에서 디바이스의 MSID(공장 기본 SID)를 이 값으로 교체한다.
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: spdk_opal_cmd_take_ownership() 및 spdk_opal_cmd_activate_locking_sp() 인자.
	 * 값 범위: 임의 길이의 null-terminated 문자열 (SED 펌웨어가 최대 길이 검증).
	 * 동기화: 단일 RPC 콜백 내. 보안 상 RPC 전송 후 메모리에 잔류하지 않도록 free 필수. */
};

/* [한국어] free 헬퍼: strdup된 두 문자열 모두 해제. */
static void
free_rpc_bdev_nvme_opal_init(struct rpc_bdev_nvme_opal_init *req)
{
	free(req->nvme_ctrlr_name);   /* [한국어] NULL이어도 free 안전. */
	free(req->password);
}

/* [한국어] bdev_nvme_opal_init RPC 입력 디코더 테이블 (둘 다 필수). */
static const struct spdk_json_object_decoder rpc_bdev_nvme_opal_init_decoders[] = {
	{"nvme_ctrlr_name", offsetof(struct rpc_bdev_nvme_opal_init, nvme_ctrlr_name), spdk_json_decode_string},
	{"password", offsetof(struct rpc_bdev_nvme_opal_init, password), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_nvme_opal_init - "bdev_nvme_opal_init" 핸들러: SED 초기화 (Take Ownership + Activate Locking SP).
 *
 * @request, @params: SPDK JSON-RPC 표준 인자.
 * @return: 없음.
 *
 * 동작 단계:
 *   1) JSON 디코드 → req 채움.
 *   2) nvme_ctrlr lookup, opal_dev 존재 확인 (둘 중 하나라도 없으면 SED 미지원).
 *   3) spdk_opal_cmd_take_ownership(): MSID → 새 SID 변경 (Admin SP).
 *      - SED 펌웨어가 Opal 표준의 ownership 절차 수행.
 *      - 실패 코드별 메시지: -EBUSY (다른 op 진행 중), -EACCES (이미 ownership 부여됨).
 *   4) spdk_opal_cmd_activate_locking_sp(): Locking SP를 활성화해 Locking Range 사용 가능 상태로.
 *   5) bool true 응답.
 * 실행 컨텍스트: app 스레드.
 */
static void
rpc_bdev_nvme_opal_init(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_opal_init req = {};   /* [한국어] 0-init: 실패 시 free가 NULL을 안전 처리. */
	struct nvme_ctrlr *nvme_ctrlr;              /* [한국어] lookup 결과. */
	int rc;                                      /* [한국어] Opal API 반환 코드. */

	/* [한국어] 1단계: JSON 파싱. 실패 시 INVALID_PARAMS 응답. */
	if (spdk_json_decode_object(params, rpc_bdev_nvme_opal_init_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_opal_init_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* check if opal supported */
	/* [한국어] 2단계: 컨트롤러 lookup + Opal 지원 여부 확인.
	 * nvme_ctrlr->opal_dev는 컨트롤러 attach 시 SED를 감지했을 때만 채워진다. */
	nvme_ctrlr = nvme_ctrlr_get_by_name(req.nvme_ctrlr_name);
	if (nvme_ctrlr == NULL || nvme_ctrlr->opal_dev == NULL) {
		SPDK_ERRLOG("%s not support opal\n", req.nvme_ctrlr_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* take ownership */
	/* [한국어] 3단계: SED 표준 "Take Ownership" 절차.
	 * 내부적으로 NVMe Security Send/Receive (opcode 0x81/0x82)로 Admin SP에 인증 후 SID 변경. */
	rc = spdk_opal_cmd_take_ownership(nvme_ctrlr->opal_dev, req.password);
	if (rc) {
		SPDK_ERRLOG("Take ownership failure: %d\n", rc);
		/* [한국어] 자주 발생하는 두 에러를 사용자 친화적 메시지로 매핑. */
		switch (rc) {
		case -EBUSY:
			/* [한국어] -EBUSY: SED가 다른 명령 처리 중. 잠시 후 재시도해야 함. */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "SP Busy, try again later");
			break;
		case -EACCES:
			/* [한국어] -EACCES: 이미 ownership이 부여된 SED (MSID가 아닌 상태).
			 * 사용자는 기존 password를 알고 있어야 함. */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "This drive is already enabled");
			break;
		default:
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
		goto out;
	}

	/* activate locking SP */
	/* [한국어] 4단계: Locking SP를 활성화해 Locking Range CRUD가 가능하게 한다.
	 * 신규 SED는 Locking SP가 비활성 상태이므로 이 단계가 필수. */
	rc = spdk_opal_cmd_activate_locking_sp(nvme_ctrlr->opal_dev, req.password);
	if (rc) {
		SPDK_ERRLOG("Activate locking SP failure: %d\n", rc);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		goto out;
	}

	/* [한국어] 5단계: 모든 단계 성공 → true 응답. */
	spdk_jsonrpc_send_bool_response(request, true);

out:
	free_rpc_bdev_nvme_opal_init(&req);
}
SPDK_RPC_REGISTER("bdev_nvme_opal_init", rpc_bdev_nvme_opal_init, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_bdev_nvme_opal_revert - bdev_nvme_opal_revert RPC 입력.
 * JSON: {"nvme_ctrlr_name": "...", "password": "..."}.
 * 주의: revert TPer는 SED를 공장 초기화하며, 디스크의 모든 데이터가 사용 불가능해진다 (DEK 재생성).
 */
struct rpc_bdev_nvme_opal_revert {
	char *nvme_ctrlr_name;
	/* [한국어] 공장 초기화를 수행할 대상 NVMe 컨트롤러 이름.
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: rpc_bdev_nvme_opal_revert()의 nvme_ctrlr_get_by_name() 인자.
	 * 값 범위: null-terminated 문자열. 필수.
	 * 동기화: 단일 RPC 콜백 내. */

	char *password;
	/* [한국어] Revert TPer 권한 인증용 패스워드.
	 * SED Revert는 PSID(Physical Secure ID, 보통 디바이스 라벨에 표기) 또는 SID로 인증.
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: spdk_opal_cmd_revert_tper() 인자.
	 * 값 범위: null-terminated 문자열. 필수. 잘못된 값이면 SED가 -EACCES 반환.
	 * 동기화: 단일 RPC 콜백 내. */
};

/* [한국어] free 헬퍼: revert 요청 임시 문자열 해제. */
static void
free_rpc_bdev_nvme_opal_revert(struct rpc_bdev_nvme_opal_revert *req)
{
	free(req->nvme_ctrlr_name);
	free(req->password);
}

/* [한국어] revert RPC 디코더 테이블. */
static const struct spdk_json_object_decoder rpc_bdev_nvme_opal_revert_decoders[] = {
	{"nvme_ctrlr_name", offsetof(struct rpc_bdev_nvme_opal_revert, nvme_ctrlr_name), spdk_json_decode_string},
	{"password", offsetof(struct rpc_bdev_nvme_opal_revert, password), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_nvme_opal_revert - "bdev_nvme_opal_revert" 핸들러: SED를 공장 초기화.
 *
 * 주의 사항:
 *   - Revert TPer는 SED 내부의 데이터 암호화 키(DEK)를 재생성해 기존 데이터를 영구히 읽을 수 없게 한다.
 *   - TODO 주석에 명시된 대로 현재 구현은 revert 전에 vbdev들을 자동 정리하지 않으므로,
 *     운영자가 사전에 모든 vbdev_opal을 destruct해야 안전하다.
 *
 * 동작 단계:
 *   1) JSON 디코드 → 컨트롤러 lookup → Opal 지원 확인.
 *   2) spdk_opal_cmd_revert_tper(): NVMe Security Send로 RevertSP 명령 발급.
 *   3) bool 응답.
 * 실행 컨텍스트: app 스레드.
 */
static void
rpc_bdev_nvme_opal_revert(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_opal_revert req = {};   /* [한국어] 0-init. */
	struct nvme_ctrlr *nvme_ctrlr;                /* [한국어] lookup 결과. */
	int rc;                                        /* [한국어] revert 결과. */

	/* [한국어] 1단계: JSON 디코드. */
	if (spdk_json_decode_object(params, rpc_bdev_nvme_opal_revert_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_opal_revert_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* check if opal supported */
	/* [한국어] 2단계: 컨트롤러 lookup + Opal 지원 확인. */
	nvme_ctrlr = nvme_ctrlr_get_by_name(req.nvme_ctrlr_name);
	if (nvme_ctrlr == NULL || nvme_ctrlr->opal_dev == NULL) {
		SPDK_ERRLOG("%s not support opal\n", req.nvme_ctrlr_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* TODO: delete all opal vbdev before revert TPer */
	/* [한국어] 위 TODO: revert는 SED 내부 모든 키를 무효화하므로 등록된 vbdev_opal들이 좀비 상태가 됨.
	 * 안전한 흐름은 revert 전에 모든 vbdev_opal을 destruct하는 것이지만 미구현. */

	/* [한국어] 3단계: TPer Revert 명령. SED 펌웨어가 모든 키/Locking Range를 초기화. */
	rc = spdk_opal_cmd_revert_tper(nvme_ctrlr->opal_dev, req.password);
	if (rc) {
		SPDK_ERRLOG("Revert TPer failure: %d\n", rc);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		goto out;
	}

	/* [한국어] 4단계: 성공 응답. */
	spdk_jsonrpc_send_bool_response(request, true);

out:
	free_rpc_bdev_nvme_opal_revert(&req);
}
SPDK_RPC_REGISTER("bdev_nvme_opal_revert", rpc_bdev_nvme_opal_revert, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_bdev_opal_create - bdev_opal_create RPC 입력.
 * JSON: {nvme_ctrlr_name, nsid, locking_range_id, range_start, range_length, password}.
 */
struct rpc_bdev_opal_create {
	char *nvme_ctrlr_name;
	/* [한국어] Locking Range를 만들 NVMe 컨트롤러 이름 (Opal SED 지원 필수).
	 * 설정자: spdk_json_decode_string()으로 strdup. 읽는 자: vbdev_opal_create() 첫 인자.
	 * 값 범위: null-terminated 문자열. 필수.
	 * 동기화: 단일 RPC 콜백 내. */

	uint32_t nsid;
	/* [한국어] Locking Range의 LBA 범위가 속한 NVMe namespace ID.
	 * 설정자: spdk_json_decode_uint32(). 읽는 자: vbdev_opal_create() 두 번째 인자.
	 * 값 범위: 현재 NSID_SUPPORTED(=1)만 허용 (vbdev_opal.c 제한).
	 * 동기화: 단일 RPC 콜백 내. */

	uint16_t locking_range_id;
	/* [한국어] 설정할 Locking Range 인덱스 (SED 내부 인덱스, 보통 0~7).
	 * 설정자: spdk_json_decode_uint16(). 읽는 자: vbdev_opal_create().
	 * 값 범위: 0 이상. 최대값은 디바이스에 따라 다름.
	 * 동기화: 단일 RPC 콜백 내. */

	uint64_t range_start;
	/* [한국어] Locking Range가 보호할 시작 LBA 번호 (베이스 namespace 기준 절대 LBA).
	 * 설정자: spdk_json_decode_uint64(). 읽는 자: vbdev_opal_create() 네 번째 인자.
	 * 값 범위: 0 이상, range_start + range_length <= NS 총 블록 수.
	 * 동기화: 단일 RPC 콜백 내. */

	uint64_t range_length;
	/* [한국어] Locking Range가 보호할 LBA 개수.
	 * 설정자: spdk_json_decode_uint64(). 읽는 자: vbdev_opal_create() 다섯 번째 인자.
	 * 값 범위: 1 이상. 0이면 SED가 전체 NS를 뜻하는 경우도 있으나 현재 SPDK는 미처리.
	 * 동기화: 단일 RPC 콜백 내. */

	char *password;
	/* [한국어] Locking Range 설정에 필요한 Admin SP 인증 패스워드.
	 * 설정자: spdk_json_decode_string()으로 strdup. 읽는 자: vbdev_opal_create() 여섯 번째 인자.
	 * 값 범위: null-terminated 문자열. 필수.
	 * 동기화: 단일 RPC 콜백 내. free_rpc_bdev_opal_create()에서 해제. */
};

/* [한국어] free 헬퍼: strdup된 문자열 두 개 해제 (uint 필드는 free 불필요). */
static void
free_rpc_bdev_opal_create(struct rpc_bdev_opal_create *req)
{
	free(req->nvme_ctrlr_name);
	free(req->password);
}

/* [한국어] bdev_opal_create RPC 디코더 테이블.
 * 모든 필드 필수 (optional 표시 없음). */
static const struct spdk_json_object_decoder rpc_bdev_opal_create_decoders[] = {
	{"nvme_ctrlr_name", offsetof(struct rpc_bdev_opal_create, nvme_ctrlr_name), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_bdev_opal_create, nsid), spdk_json_decode_uint32},
	{"locking_range_id", offsetof(struct rpc_bdev_opal_create, locking_range_id), spdk_json_decode_uint16},
	{"range_start", offsetof(struct rpc_bdev_opal_create, range_start), spdk_json_decode_uint64},
	{"range_length", offsetof(struct rpc_bdev_opal_create, range_length), spdk_json_decode_uint64},
	{"password", offsetof(struct rpc_bdev_opal_create, password), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_opal_create - "bdev_opal_create" 핸들러: Locking Range 설정 + 새 vbdev 등록.
 *
 * 응답 JSON: 생성된 vbdev 이름 (예: "Nvme0n1r0" - controller "Nvme0", namespace 1, range 0).
 *
 * 동작 단계:
 *   1) JSON 디코드.
 *   2) vbdev_opal_create() 호출 → SED Locking Range 설정 + spdk_bdev_register.
 *   3) 응답으로 생성된 bdev 이름 문자열 반환.
 * 실행 컨텍스트: app 스레드.
 */
static void
rpc_bdev_opal_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_opal_create req = {};        /* [한국어] 입력 컨테이너. */
	struct spdk_json_write_ctx *w;                /* [한국어] 응답 JSON writer. */
	char *opal_bdev_name;                          /* [한국어] 동적 할당된 bdev 이름. */
	int rc;                                        /* [한국어] vbdev_opal_create 결과. */

	/* [한국어] 1단계: 입력 파싱. */
	if (spdk_json_decode_object(params, rpc_bdev_opal_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_opal_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* [한국어] 2단계: vbdev_opal_create()에 위임 (SED Locking Range 메타데이터 설정 + bdev 등록). */
	rc = vbdev_opal_create(req.nvme_ctrlr_name, req.nsid, req.locking_range_id, req.range_start,
			       req.range_length, req.password);
	if (rc != 0) {
		/* [한국어] _fmt 변형으로 컨트롤러 이름과 errno 문자열을 함께 보고. */
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to create opal vbdev from '%s': %s",
						     req.nvme_ctrlr_name, spdk_strerror(-rc));
		goto out;
	}

	/* [한국어] 3단계: 응답 JSON 시작. begin_result로 result 필드 컨텍스트 획득. */
	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] bdev 이름 형식: "<ctrlr>n<nsid>r<range_id>" - 같은 prefix를 코드 곳곳에서 사용. */
	opal_bdev_name = spdk_sprintf_alloc("%sn%dr%d", req.nvme_ctrlr_name, req.nsid,
					    req.locking_range_id);
	spdk_json_write_string(w, opal_bdev_name);   /* [한국어] result에 bdev 이름 직접 쓰기 (단일 string). */
	spdk_jsonrpc_end_result(request, w);          /* [한국어] 응답 전송 완료. */
	free(opal_bdev_name);                          /* [한국어] sprintf_alloc 결과 해제. */

out:
	free_rpc_bdev_opal_create(&req);
}
SPDK_RPC_REGISTER("bdev_opal_create", rpc_bdev_opal_create, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_bdev_opal_get_info - bdev_opal_get_info RPC 입력.
 * JSON: {"bdev_name": "...", "password": "..."}.
 */
struct rpc_bdev_opal_get_info {
	char *bdev_name;
	/* [한국어] 메타데이터를 조회할 vbdev_opal bdev 이름 (예: "Nvme0n1r1").
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: rpc_bdev_opal_get_info()에서 vbdev_opal_get_info_from_bdev()의 첫 인자.
	 * 값 범위: null-terminated 문자열. 필수.
	 * 동기화: 단일 RPC 콜백 내. free_rpc_bdev_opal_get_info()에서 해제. */

	char *password;
	/* [한국어] Locking Range Get-Range 인증에 사용할 Admin 패스워드.
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: vbdev_opal_get_info_from_bdev() 내부에서 SED 인증 세션 열 때 사용.
	 * 값 범위: null-terminated 문자열. 필수.
	 * 동기화: 단일 RPC 콜백 내. free_rpc_bdev_opal_get_info()에서 해제. */
};

/* [한국어] free 헬퍼. */
static void
free_rpc_bdev_opal_get_info(struct rpc_bdev_opal_get_info *req)
{
	free(req->bdev_name);
	free(req->password);
}

/* [한국어] get_info RPC 디코더 테이블. */
static const struct spdk_json_object_decoder rpc_bdev_opal_get_info_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_opal_get_info, bdev_name), spdk_json_decode_string},
	{"password", offsetof(struct rpc_bdev_opal_get_info, password), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_opal_get_info - "bdev_opal_get_info" 핸들러: Locking Range 메타데이터 조회.
 *
 * 응답 JSON: {name, range_start, range_length, read_lock_enabled, write_lock_enabled,
 *             read_locked, write_locked}.
 *
 * 동작 단계:
 *   1) JSON 디코드.
 *   2) vbdev_opal_get_info_from_bdev() 호출 → SED에 Get-Range 명령 발급.
 *   3) info를 JSON 객체로 직렬화 후 응답.
 * 실행 컨텍스트: app 스레드.
 */
static void
rpc_bdev_opal_get_info(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_opal_get_info req = {};               /* [한국어] 입력. */
	struct spdk_json_write_ctx *w;                          /* [한국어] 응답 writer. */
	struct spdk_opal_locking_range_info *info;             /* [한국어] SED가 반환한 메타데이터. */

	/* [한국어] 1단계: 파라미터 파싱. */
	if (spdk_json_decode_object(params, rpc_bdev_opal_get_info_decoders,
				    SPDK_COUNTOF(rpc_bdev_opal_get_info_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* [한국어] 2단계: 모듈 API에 위임 (인증 + Get-Range). */
	info = vbdev_opal_get_info_from_bdev(req.bdev_name, req.password);
	if (info == NULL) {
		SPDK_ERRLOG("Get opal info failure\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		goto out;
	}

	/* [한국어] 3단계: 응답 객체 빌드. */
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);   /* [한국어] result 객체 시작. */

	spdk_json_write_named_string(w, "name", req.bdev_name);                     /* [한국어] bdev 이름. */
	spdk_json_write_named_uint64(w, "range_start", info->range_start);          /* [한국어] 시작 LBA. */
	spdk_json_write_named_uint64(w, "range_length", info->range_length);        /* [한국어] LBA 개수. */
	spdk_json_write_named_bool(w, "read_lock_enabled", info->read_lock_enabled);  /* [한국어] read 잠금 기능 활성. */
	spdk_json_write_named_bool(w, "write_lock_enabled", info->write_lock_enabled); /* [한국어] write 잠금 기능 활성. */
	spdk_json_write_named_bool(w, "read_locked", info->read_locked);             /* [한국어] 현재 read 잠겨있는가. */
	spdk_json_write_named_bool(w, "write_locked", info->write_locked);           /* [한국어] 현재 write 잠겨있는가. */

	spdk_json_write_object_end(w);   /* [한국어] result 객체 종료. */
	spdk_jsonrpc_end_result(request, w);

out:
	free_rpc_bdev_opal_get_info(&req);
}
SPDK_RPC_REGISTER("bdev_opal_get_info", rpc_bdev_opal_get_info, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_bdev_opal_delete - bdev_opal_delete RPC 입력.
 * JSON: {"bdev_name": "...", "password": "..."}.
 */
struct rpc_bdev_opal_delete {
	char *bdev_name;
	/* [한국어] 제거할 vbdev_opal bdev의 이름 (예: "Nvme0n1r1").
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: rpc_bdev_opal_delete()에서 vbdev_opal_destruct() 첫 인자.
	 * 값 범위: null-terminated 문자열. 필수.
	 * 동기화: 단일 RPC 콜백 내. free_rpc_bdev_opal_delete()에서 해제. */

	char *password;
	/* [한국어] vbdev_opal 삭제 시 Admin SP 인증에 사용할 패스워드.
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: vbdev_opal_destruct() 내부에서 SED 세션 열 때 사용.
	 * 값 범위: null-terminated 문자열. 필수.
	 * 동기화: 단일 RPC 콜백 내. free_rpc_bdev_opal_delete()에서 해제. */
};

/* [한국어] free 헬퍼. */
static void
free_rpc_bdev_opal_delete(struct rpc_bdev_opal_delete *req)
{
	free(req->bdev_name);
	free(req->password);
}

/* [한국어] delete RPC 디코더 테이블. */
static const struct spdk_json_object_decoder rpc_bdev_opal_delete_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_opal_delete, bdev_name), spdk_json_decode_string},
	{"password", offsetof(struct rpc_bdev_opal_delete, password), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_opal_delete - "bdev_opal_delete" 핸들러: vbdev_opal 제거.
 *
 * vbdev_opal_destruct()가 인증 + bdev unregister + Locking Range 잠금 복원을 수행한다.
 * 응답: bool.
 * 실행 컨텍스트: app 스레드.
 */
static void
rpc_bdev_opal_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_opal_delete req = {};   /* [한국어] 입력. */
	int rc;                                  /* [한국어] destruct 결과. */

	/* [한국어] 1단계: 파라미터 파싱. */
	if (spdk_json_decode_object(params, rpc_bdev_opal_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_opal_delete_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* [한국어] 2단계: vbdev_opal_destruct에 위임. */
	rc = vbdev_opal_destruct(req.bdev_name, req.password);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto out;
	}

	/* [한국어] 3단계: 성공 응답. */
	spdk_jsonrpc_send_bool_response(request, true);
out:
	free_rpc_bdev_opal_delete(&req);
}
SPDK_RPC_REGISTER("bdev_opal_delete", rpc_bdev_opal_delete, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_bdev_opal_set_lock_state - bdev_opal_set_lock_state RPC 입력.
 * JSON: {bdev_name, user_id, password, lock_state}.
 * lock_state: "RWLOCK"|"READONLY"|"RWUNLOCK" (vbdev_opal.c가 파싱).
 */
struct rpc_bdev_opal_set_lock_state {
	char *bdev_name;
	/* [한국어] 잠금 상태를 변경할 vbdev_opal bdev 이름 (예: "Nvme0n1r1").
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: rpc_bdev_opal_set_lock_state()에서 vbdev_opal_set_lock_state() 첫 인자.
	 * 값 범위: null-terminated 문자열. 필수.
	 * 동기화: 단일 RPC 콜백 내. free_rpc_bdev_opal_set_lock_state()에서 해제. */

	uint16_t user_id;
	/* [한국어] 인증에 사용할 User 슬롯 ID (0 = Admin, 1~N = User1~UserN).
	 * 설정자: spdk_json_decode_uint16().
	 * 읽는 자: vbdev_opal_set_lock_state() 내부에서 인증 Authority 선택에 사용.
	 * 값 범위: 0 이상. SED 사양에 따라 지원 최대값이 다름.
	 * 동기화: 정수값, 단일 RPC 콜백 내. */

	char *password;
	/* [한국어] user_id에 대응하는 패스워드 (잠금 변경 인증용).
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: vbdev_opal_set_lock_state() 내부에서 SED 세션 인증 시 사용.
	 * 값 범위: null-terminated 문자열. 필수.
	 * 동기화: 단일 RPC 콜백 내. free_rpc_bdev_opal_set_lock_state()에서 해제. */

	char *lock_state;
	/* [한국어] 변경할 잠금 상태 문자열: "RWLOCK"(읽기+쓰기 잠금), "READONLY"(쓰기 잠금), "RWUNLOCK"(잠금 해제).
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: vbdev_opal_set_lock_state()에서 문자열 비교로 enum 변환 후 SED Set-Range 발급.
	 * 값 범위: 위 3가지 문자열 중 하나. 다른 값이면 vbdev_opal_set_lock_state()가 오류 반환.
	 * 동기화: 단일 RPC 콜백 내. free_rpc_bdev_opal_set_lock_state()에서 해제. */
};

/* [한국어] free 헬퍼: 3개 strdup 문자열 해제. */
static void
free_rpc_bdev_opal_set_lock_state(struct rpc_bdev_opal_set_lock_state *req)
{
	free(req->bdev_name);
	free(req->password);
	free(req->lock_state);
}

/* [한국어] set_lock_state RPC 디코더 테이블. */
static const struct spdk_json_object_decoder rpc_bdev_opal_set_lock_state_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_opal_set_lock_state, bdev_name), spdk_json_decode_string},
	{"user_id", offsetof(struct rpc_bdev_opal_set_lock_state, user_id), spdk_json_decode_uint16},
	{"password", offsetof(struct rpc_bdev_opal_set_lock_state, password), spdk_json_decode_string},
	{"lock_state", offsetof(struct rpc_bdev_opal_set_lock_state, lock_state), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_opal_set_lock_state - "bdev_opal_set_lock_state" 핸들러: 잠금 상태 변경.
 *
 * vbdev_opal_set_lock_state()가 user_id로 인증 후 SED Set-Range 명령 발급.
 * 응답: bool.
 * 실행 컨텍스트: app 스레드.
 */
static void
rpc_bdev_opal_set_lock_state(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_opal_set_lock_state req = {};   /* [한국어] 입력. */
	int rc;                                          /* [한국어] 결과. */

	/* [한국어] 1단계: 파라미터 디코드. */
	if (spdk_json_decode_object(params, rpc_bdev_opal_set_lock_state_decoders,
				    SPDK_COUNTOF(rpc_bdev_opal_set_lock_state_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* [한국어] 2단계: 모듈 API 호출 (인증 + 잠금 변경). */
	rc = vbdev_opal_set_lock_state(req.bdev_name, req.user_id, req.password, req.lock_state);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto out;
	}

	/* [한국어] 3단계: 성공 응답. */
	spdk_jsonrpc_send_bool_response(request, true);

out:
	free_rpc_bdev_opal_set_lock_state(&req);
}
SPDK_RPC_REGISTER("bdev_opal_set_lock_state", rpc_bdev_opal_set_lock_state, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_bdev_opal_new_user - bdev_opal_new_user RPC 입력.
 * JSON: {bdev_name, admin_password, user_id, user_password}.
 * Admin이 새 User 슬롯에 권한과 패스워드를 동시에 부여.
 */
struct rpc_bdev_opal_new_user {
	char *bdev_name;
	/* [한국어] 새 User를 활성화할 대상 vbdev_opal bdev 이름 (예: "Nvme0n1r1").
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: rpc_bdev_opal_new_user()에서 vbdev_opal_enable_new_user() 첫 인자.
	 * 값 범위: null-terminated 문자열. 필수.
	 * 동기화: 단일 RPC 콜백 내. free_rpc_bdev_opal_new_user()에서 해제. */

	char *admin_password;
	/* [한국어] User 슬롯 활성화 권한을 갖는 Admin SP 인증 패스워드.
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: vbdev_opal_enable_new_user() 내부에서 Admin Authority로 SED 세션 열 때 사용.
	 * 값 범위: null-terminated 문자열. 필수.
	 * 동기화: 단일 RPC 콜백 내. free_rpc_bdev_opal_new_user()에서 해제. */

	uint16_t user_id;
	/* [한국어] 활성화할 User 슬롯 ID (1~N; 0은 Admin이므로 이 RPC에서는 1 이상 사용).
	 * 설정자: spdk_json_decode_uint16().
	 * 읽는 자: vbdev_opal_enable_new_user() 내부에서 해당 User 슬롯 enable + ACL 부여 시 사용.
	 * 값 범위: 1 이상. SED 사양에 따라 최대 슬롯 수가 다름.
	 * 동기화: 정수값, 단일 RPC 콜백 내. */

	char *user_password;
	/* [한국어] 새로 활성화되는 User 슬롯에 설정할 패스워드.
	 * 설정자: spdk_json_decode_string()으로 strdup.
	 * 읽는 자: vbdev_opal_enable_new_user() 내부에서 SED Set-Credentials 명령 발급 시 사용.
	 * 값 범위: null-terminated 문자열. 필수.
	 * 동기화: 단일 RPC 콜백 내. free_rpc_bdev_opal_new_user()에서 해제. */
};

/* [한국어] free 헬퍼: strdup된 3개 문자열 해제 (user_id는 정수). */
static void
free_rpc_bdev_opal_new_user(struct rpc_bdev_opal_new_user *req)
{
	free(req->bdev_name);
	free(req->admin_password);
	free(req->user_password);
}

/* [한국어] new_user RPC 디코더 테이블. */
static const struct spdk_json_object_decoder rpc_bdev_opal_new_user_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_opal_new_user, bdev_name), spdk_json_decode_string},
	{"admin_password", offsetof(struct rpc_bdev_opal_new_user, admin_password), spdk_json_decode_string},
	{"user_id", offsetof(struct rpc_bdev_opal_new_user, user_id), spdk_json_decode_uint16},
	{"user_password", offsetof(struct rpc_bdev_opal_new_user, user_password), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_opal_new_user - "bdev_opal_new_user" 핸들러: 새 User 활성화.
 *
 * 동작: vbdev_opal_enable_new_user → SED 내 User Authority enable + ACL 갱신 + password set.
 * 응답: bool.
 * 실행 컨텍스트: app 스레드.
 */
static void
rpc_bdev_opal_new_user(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_opal_new_user req = {};   /* [한국어] 입력. */
	int rc;                                    /* [한국어] enable 결과. */

	/* [한국어] 1단계: 파라미터 파싱. */
	if (spdk_json_decode_object(params, rpc_bdev_opal_new_user_decoders,
				    SPDK_COUNTOF(rpc_bdev_opal_new_user_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* [한국어] 2단계: User 활성화 및 패스워드 설정. */
	rc = vbdev_opal_enable_new_user(req.bdev_name, req.admin_password, req.user_id,
					req.user_password);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto out;
	}

	/* [한국어] 3단계: 성공 응답. */
	spdk_jsonrpc_send_bool_response(request, true);

out:
	free_rpc_bdev_opal_new_user(&req);
}
SPDK_RPC_REGISTER("bdev_opal_new_user", rpc_bdev_opal_new_user, SPDK_RPC_RUNTIME)
