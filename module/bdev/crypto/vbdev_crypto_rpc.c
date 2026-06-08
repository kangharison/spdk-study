/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * [한국어 설명] crypto vbdev 의 JSON-RPC 인터페이스 (vbdev_crypto_rpc.c)
 *
 * === 파일의 역할 ===
 * bdev_crypto_create / bdev_crypto_delete RPC 메서드의 핸들러 구현 파일. SPDK_RPC_REGISTER
 * 매크로로 lib/rpc 의 글로벌 RPC 메서드 테이블에 등록되며, JSON 페이로드를 파싱해
 * 핵심 로직 함수 (create_crypto_disk / delete_crypto_disk) 를 호출하고, 결과 JSON 을
 * 응답한다. 두 가지 키 관리 모드를 지원:
 *   (1) 신규 방식: 사용자가 사전에 keyring 에 등록한 key_name 만 지정 → 그 키 lookup.
 *   (2) 레거시 방식: 사용자가 cipher + hex_key (+ hex_key2 for XTS) 만 전달 →
 *       자동으로 "<vbdev_name>_<cipher>" 형식의 key_name 을 만들어 keyring 에 등록.
 * 레거시 호환을 위해 자동 생성된 키는 key_owner=true 로 표시되어 vbdev 삭제 시 함께 파괴된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   [JSON-RPC client] → spdk_rpc_listener (lib/rpc) → 본 핸들러 (rpc_bdev_crypto_create/delete)
 *     → spdk_json_decode_object 로 파라미터 파싱
 *     → spdk_accel_crypto_key_get / spdk_accel_crypto_key_create (lib/keyring 경유)
 *     → create_crypto_opts (본 파일 내) / create_crypto_disk (vbdev_crypto.c)
 *     → vbdev_crypto_insert_name → vbdev_crypto_claim (base bdev 있으면 즉시 생성, 없으면 deferred)
 *
 * 실행 컨텍스트: SPDK 의 RPC dispatcher thread (일반적으로 app 메인 reactor) 에서 동기 실행.
 *
 * === 타 모듈과의 연결 ===
 * - lib/jsonrpc: spdk_jsonrpc_request / spdk_jsonrpc_send_*_response — 요청·응답 처리.
 * - lib/json: spdk_json_decode_object / spdk_json_write_* — JSON 파싱/직렬화.
 * - lib/rpc: SPDK_RPC_REGISTER — 본 파일의 핸들러를 메서드 이름과 함께 전역 테이블 등록.
 * - lib/accel (+ lib/keyring): spdk_accel_crypto_key_get/create/destroy — DEK 관리.
 * - vbdev_crypto.c: create_crypto_disk / delete_crypto_disk / free_crypto_opts —
 *   실제 vbdev 라이프사이클 진입점.
 * - spdk/hexlify.h: hex_key/hex_key2 디코딩에 사용 (현재 파일에서는 keyring 이 처리).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rpc_construct_crypto: bdev_crypto_create 의 JSON 인자 staging 구조체.
 * - free_rpc_construct_crypto(): hex_key 등 민감 데이터를 memset(0) 으로 wipe 한 후 free.
 * - create_crypto_opts(): rpc_construct_crypto + key 로 vbdev_crypto_opts 구성.
 * - rpc_bdev_crypto_create(): 메인 핸들러 — 파라미터 파싱 + key 결정 + vbdev 생성.
 * - struct rpc_delete_crypto / rpc_bdev_crypto_delete(): 삭제 RPC 핸들러.
 * - rpc_bdev_crypto_delete_cb(): delete_crypto_disk 완료 후 JSON 응답 전송.
 */

#include "vbdev_crypto.h"                                                   /* [한국어] vbdev_crypto.c 와 공유되는 인터페이스 (create_crypto_disk / delete_crypto_disk / free_crypto_opts / BDEV_CRYPTO_DEFAULT_CIPHER). */

#include "spdk/hexlify.h"                                                   /* [한국어] hex 문자열 ↔ 바이너리 변환 유틸 — 현재는 keyring 내부에서 사용. */

/* Reasonable bdev name length + cipher's name len */
/* [한국어] 자동 생성되는 key_name 의 최대 길이 — "vbdev_name + '_' + cipher_name" 을 안전하게 담는 크기. */
#define MAX_KEY_NAME_LEN 128

/* Structure to hold the parameters for this RPC method. */
/* [한국어] bdev_crypto_create RPC 의 JSON 입력을 staging 하는 구조체 — 파싱 직후 폐기. */
struct rpc_construct_crypto {
	char *base_bdev_name;
	/* [한국어] 덮어쓸 base bdev 의 이름.
	 * 설정자: spdk_json_decode_object (필수 인자).
	 * 읽는 자: create_crypto_opts → opts->bdev_name 으로 복제 후 사용.
	 * 동기화: RPC 핸들러 한 thread 에서만 사용 — 별도 lock 불필요. */

	char *name;
	/* [한국어] 생성될 crypto vbdev 의 이름 (필수). */

	char *crypto_pmd;
	/* [한국어] 레거시 옵션 — 과거 DPDK CryptoDev PMD 이름 지정용. 현재는 accel 모듈이
	 * 자동 선택하므로 obsolete (사용 시 경고 로그). NULL 가능. */

	struct spdk_accel_crypto_key_create_param param;
	/* [한국어] 키 생성/조회용 파라미터 묶음.
	 * - param.key_name: 사용자가 미리 keyring 에 등록한 key 의 이름 (신규 모드).
	 * - param.hex_key/hex_key2: 레거시 모드의 raw 키 (hex 인코딩).
	 * - param.cipher: 알고리즘 이름 (예: "AES_XTS").
	 * 설정자: spdk_json_decode_object.
	 * 읽는 자: spdk_accel_crypto_key_get/create.
	 * 보안: hex_key/hex_key2 는 free 전에 memset(0) 으로 wipe (cleanup 함수 참조). */
};

/* Free the allocated memory resource after the RPC handling. */
/*
 * [한국어]
 * free_rpc_construct_crypto - rpc 입력 staging 구조체의 모든 동적 메모리 해제.
 *
 * @r: 해제할 구조체 (필드 자체는 스택 변수, 내부 포인터만 free).
 *
 * 보안: hex_key/hex_key2 는 raw key material 이므로 free 전에 반드시 memset(0) 으로
 * 메모리상에서 wipe 한다 (heap 재할당 시 다른 코드가 잔여물을 읽는 것을 방지).
 */
static void
free_rpc_construct_crypto(struct rpc_construct_crypto *r)
{
	free(r->base_bdev_name);
	free(r->name);
	free(r->crypto_pmd);
	free(r->param.cipher);
	if (r->param.hex_key) {                                                  /* [한국어] hex_key 가 있으면 보안상 wipe 후 free. */
		memset(r->param.hex_key, 0, strnlen(r->param.hex_key, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH)); /* [한국어] 최대 길이까지만 검사하여 무한 strlen 회피. */
		free(r->param.hex_key);
	}
	if (r->param.hex_key2) {                                                 /* [한국어] AES-XTS 의 두 번째 키도 동일 처리. */
		memset(r->param.hex_key2, 0, strnlen(r->param.hex_key2, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH));
		free(r->param.hex_key2);
	}
	free(r->param.key_name);
}

/* Structure to decode the input parameters for this RPC method. */
/* [한국어] bdev_crypto_create RPC 의 JSON → C 구조체 변환 디코더 테이블.
 * 4번째 인자가 true 면 optional (없어도 됨). 필수 인자는 base_bdev_name 과 name 두 개만. */
static const struct spdk_json_object_decoder rpc_bdev_crypto_create_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_construct_crypto, base_bdev_name), spdk_json_decode_string},                /* [한국어] 필수 — 어떤 base bdev 를 덮을지. */
	{"name", offsetof(struct rpc_construct_crypto, name), spdk_json_decode_string},                                    /* [한국어] 필수 — 새 vbdev 의 이름. */
	{"crypto_pmd", offsetof(struct rpc_construct_crypto, crypto_pmd), spdk_json_decode_string, true},                  /* [한국어] 레거시 obsolete. */
	{"key", offsetof(struct rpc_construct_crypto, param.hex_key), spdk_json_decode_string, true},                      /* [한국어] 레거시 — raw hex key. */
	{"cipher", offsetof(struct rpc_construct_crypto, param.cipher), spdk_json_decode_string, true},                    /* [한국어] 레거시 — 알고리즘 (AES_XTS 등). */
	{"key2", offsetof(struct rpc_construct_crypto, param.hex_key2), spdk_json_decode_string, true},                    /* [한국어] 레거시 — XTS 의 두 번째 키. */
	{"key_name", offsetof(struct rpc_construct_crypto, param.key_name), spdk_json_decode_string, true},                /* [한국어] 신규 — 이미 keyring 에 등록된 key 의 이름. */
};

/*
 * [한국어]
 * create_crypto_opts - RPC 입력 + key 로부터 vbdev_crypto_opts 생성.
 *
 * @rpc: 파싱된 RPC 입력.
 * @key: 사용할 DEK 핸들 (조회되었거나 새로 생성됨).
 * @key_owner: 새로 만든 키면 true — vbdev 가 destroy 책임 짐.
 * @return: 새 opts (실패 시 NULL).
 *
 * 동기/배경: 본 파일 자체 helper — vbdev_crypto.c 의 create_crypto_opts_by_name 와
 * 비슷하지만 RPC 입력 형태에 맞춰 멤버 이름이 다름.
 */
static struct vbdev_crypto_opts *
create_crypto_opts(struct rpc_construct_crypto *rpc, struct spdk_accel_crypto_key *key,
		   bool key_owner)
{
	struct vbdev_crypto_opts *opts = calloc(1, sizeof(*opts));

	if (!opts) {
		return NULL;
	}

	opts->bdev_name = strdup(rpc->base_bdev_name);                           /* [한국어] RPC 입력은 free_rpc_construct_crypto 가 해제하므로 별도 복제 보관. */
	if (!opts->bdev_name) {
		free_crypto_opts(opts);
		return NULL;
	}
	opts->vbdev_name = strdup(rpc->name);
	if (!opts->vbdev_name) {
		free_crypto_opts(opts);
		return NULL;
	}

	opts->key = key;                                                         /* [한국어] DEK 포인터 인계 (소유권은 key_owner 가 결정). */
	opts->key_owner = key_owner;

	return opts;
}

/* Decode the parameters for this RPC method and properly construct the crypto
 * device. Error status returned in the failed cases.
 */
/*
 * [한국어]
 * rpc_bdev_crypto_create - bdev_crypto_create RPC 의 메인 핸들러.
 *
 * @request: JSON-RPC request 핸들 (응답 송신에 사용).
 * @params: JSON 파라미터 트리.
 *
 * 동기/배경: 사용자가 보낸 JSON 파라미터를 파싱하고, key 결정 로직(신규/레거시)을
 * 수행한 뒤, vbdev_crypto_opts 를 만들어 create_crypto_disk 에 전달한다. 성공 시
 * vbdev 이름을 문자열로 응답, 실패 시 적절한 JSON-RPC 에러 코드로 응답한다.
 *
 * 키 결정 로직:
 *   - key_name 이 명시되어 있고 keyring 에 등록되어 있으면 그 키 사용.
 *   - 그렇지 않으면 레거시 호환 모드: "<vbdev_name>_<cipher>" 형식의 자동 key_name 으로
 *     keyring 검색 → 없으면 hex_key/hex_key2 로 신규 생성. 신규 생성 시 created_key 추적.
 *
 * 단계:
 *   1) JSON 디코드.
 *   2) name 필수 검증.
 *   3) 키 결정 (위 로직).
 *   4) 키가 없으면 에러 응답.
 *   5) create_crypto_opts → create_crypto_disk.
 *   6) 성공: vbdev 이름 응답 / 실패: 에러 응답 + (created_key 가 있다면) destroy.
 *
 * 실행 컨텍스트: RPC dispatcher thread (보통 main reactor).
 * 호출 체인: lib/rpc → 본 함수 → spdk_accel_crypto_key_* → create_crypto_disk.
 */
static void
rpc_bdev_crypto_create(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_construct_crypto req = {};                                    /* [한국어] zero-init — 모든 포인터를 NULL 로. */
	struct vbdev_crypto_opts *crypto_opts = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_accel_crypto_key *key = NULL;                                /* [한국어] 최종 사용할 키. */
	struct spdk_accel_crypto_key *created_key = NULL;                        /* [한국어] 신규 생성된 키 추적 — 에러 시 destroy 책임. */
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_crypto_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_crypto_create_decoders),
				    &req)) {                                     /* [한국어] JSON 파싱 — 실패 시 PARSE_ERROR. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "Failed to decode crypto disk create parameters.");
		goto cleanup;
	}

	if (!req.name) {                                                         /* [한국어] decoder 가 optional 처리하지만 name 은 사실상 필수 검증. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "crypto_bdev name is missing");
		goto cleanup;
	}

	if (req.param.key_name) {
		/* New config version */
		key = spdk_accel_crypto_key_get(req.param.key_name);             /* [한국어] keyring 에서 lookup (ref 안 늘림). */
		if (key) {
			if (req.param.hex_key || req.param.cipher || req.crypto_pmd) {
				SPDK_NOTICELOG("Key name specified, other parameters are ignored\n"); /* [한국어] 신규 방식에서는 다른 옵션은 무시. */
			}
			SPDK_NOTICELOG("Found key \"%s\"\n", req.param.key_name);
		}
	}

	/* No key_name. Support legacy configuration */
	if (!key) {                                                              /* [한국어] 신규 방식에서 못 찾았거나 key_name 자체가 없음 → 레거시 경로. */
		if (req.param.key_name) {                                        /* [한국어] key_name 은 명시했는데 lookup 실패 — 사용자 의도 위반이므로 에러. */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Key was not found");
			goto cleanup;
		}

		if (req.param.cipher == NULL) {                                  /* [한국어] cipher 미지정 시 기본값 (AES_XTS) 적용. */
			req.param.cipher = strdup(BDEV_CRYPTO_DEFAULT_CIPHER);
			if (req.param.cipher == NULL) {
				spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
								 "Unable to allocate memory for req.cipher");
				goto cleanup;
			}
		}
		if (req.crypto_pmd) {
			SPDK_WARNLOG("\"crypto_pmd\" parameters is obsolete and ignored\n"); /* [한국어] 레거시 옵션 무시 경고. */
		}

		req.param.key_name = calloc(1, MAX_KEY_NAME_LEN);                /* [한국어] 자동 생성될 key_name 버퍼. */
		if (!req.param.key_name) {
			/* The new API requires key name. Create it as pmd_name + cipher */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Unable to allocate memory for key_name");
			goto cleanup;
		}
		snprintf(req.param.key_name, MAX_KEY_NAME_LEN, "%s_%s", req.name, req.param.cipher); /* [한국어] "<vbdev_name>_<cipher>" 형식의 결정론적 이름. */

		/* Try to find a key with generated name, we may be loading from a json config where crypto_bdev had no key_name parameter */
		key = spdk_accel_crypto_key_get(req.param.key_name);             /* [한국어] save_config → load_config 사이클에서 이미 키가 있을 수 있음. */
		if (key) {
			SPDK_NOTICELOG("Found key \"%s\"\n", req.param.key_name);
		} else {
			rc = spdk_accel_crypto_key_create(&req.param);           /* [한국어] hex_key/hex_key2/cipher 로 keyring 에 신규 생성. */
			if (!rc) {
				key = spdk_accel_crypto_key_get(req.param.key_name); /* [한국어] 생성 후 핸들 lookup. */
				created_key = key;                                /* [한국어] 신규 생성 추적 — 실패 시 destroy 책임. */
			}
		}
	}

	if (!key) {
		/* We haven't found an existing key or were not able to create a new one */
		SPDK_ERRLOG("No key was found\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "No key was found");
		goto cleanup;
	}

	crypto_opts = create_crypto_opts(&req, key, created_key != NULL);        /* [한국어] opts 구성. created_key 가 있으면 key_owner=true. */
	if (!crypto_opts) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation failed");
		goto cleanup;
	}

	rc = create_crypto_disk(crypto_opts);                                    /* [한국어] vbdev_crypto.c 의 핵심 진입 — claim 또는 deferred. */
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		free_crypto_opts(crypto_opts);                                   /* [한국어] create 실패 시 opts 직접 해제. */
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);                                  /* [한국어] 성공 응답 시작. */
	spdk_json_write_string(w, req.name);                                     /* [한국어] 결과로 vbdev 이름 반환. */
	spdk_jsonrpc_end_result(request, w);

cleanup:
	if (rc && created_key) {                                                 /* [한국어] 실패 경로에서 신규 키만 destroy (외부 lookup 키는 건드리지 않음). */
		spdk_accel_crypto_key_destroy(created_key);
	}
	free_rpc_construct_crypto(&req);                                         /* [한국어] hex key wipe 포함 입력 staging 해제. */
}
SPDK_RPC_REGISTER("bdev_crypto_create", rpc_bdev_crypto_create, SPDK_RPC_RUNTIME)  /* [한국어] RUNTIME state 에서 사용 가능 (RPC 서버 active 시). */

/* [한국어] bdev_crypto_delete RPC 의 JSON 입력 staging 구조체. name 한 개만 받음. */
struct rpc_delete_crypto {
	char *name;
	/* [한국어] 삭제할 crypto vbdev 의 이름 (필수).
	 * 설정자: spdk_json_decode_object.
	 * 읽는 자: delete_crypto_disk 의 첫 인자. */
};

/*
 * [한국어]
 * free_rpc_delete_crypto - delete RPC 입력 구조체의 동적 메모리 해제.
 */
static void
free_rpc_delete_crypto(struct rpc_delete_crypto *req)
{
	free(req->name);
}

/* [한국어] bdev_crypto_delete RPC 의 디코더 — name 필수. */
static const struct spdk_json_object_decoder rpc_bdev_crypto_delete_decoders[] = {
	{"name", offsetof(struct rpc_delete_crypto, name), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_crypto_delete_cb - delete_crypto_disk 비동기 완료 콜백 → JSON 응답 송신.
 *
 * @cb_arg: spdk_jsonrpc_request 포인터 (delete 시작 시 전달).
 * @bdeverrno: 0=성공, <0=에러 코드 (음수).
 *
 * 동기/배경: spdk_bdev_unregister_by_name 까지 끝나면 본 함수가 호출되어 클라이언트에게
 * true 또는 에러를 응답한다.
 */
static void
rpc_bdev_crypto_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);                  /* [한국어] 성공 — boolean true 응답. */
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno)); /* [한국어] 에러 — strerror 메시지 포함. */
	}
}

/*
 * [한국어]
 * rpc_bdev_crypto_delete - bdev_crypto_delete RPC 핸들러.
 *
 * @request / @params: 표준 RPC 인자.
 *
 * 동기/배경: 파라미터 파싱 후 delete_crypto_disk 호출. delete 자체는 비동기이므로
 * 본 함수는 즉시 return 하고 결과는 rpc_bdev_crypto_delete_cb 에서 응답.
 */
static void
rpc_bdev_crypto_delete(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_delete_crypto req = {NULL};

	if (spdk_json_decode_object(params, rpc_bdev_crypto_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_crypto_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto cleanup;
	}

	delete_crypto_disk(req.name, rpc_bdev_crypto_delete_cb, request);        /* [한국어] 비동기 삭제 트리거 — request 가 cb_arg 로 보존. */

	free_rpc_delete_crypto(&req);                                            /* [한국어] req.name 은 delete_crypto_disk 내부에서 strdup 으로 복사되므로 즉시 해제 안전. */

	return;

cleanup:
	free_rpc_delete_crypto(&req);                                            /* [한국어] 파싱 실패 경로의 정리. */
}
SPDK_RPC_REGISTER("bdev_crypto_delete", rpc_bdev_crypto_delete, SPDK_RPC_RUNTIME)  /* [한국어] RUNTIME state 에서 사용 가능. */
