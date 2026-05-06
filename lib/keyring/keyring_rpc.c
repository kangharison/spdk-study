/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

/*
 * [한국어 설명] SPDK keyring 서브시스템의 JSON-RPC 메서드 핸들러 (keyring_rpc.c)
 *
 * === 파일의 역할 ===
 * keyring 라이브러리에 보관된 모든 키 목록을 JSON-RPC 클라이언트(예: scripts/
 * rpc.py keyring_get_keys, 또는 nvmf 통합테스트)에게 노출하는 RPC 핸들러
 * "keyring_get_keys"를 정의한다. 내부적으로는 keyring.c가 노출한
 * spdk_keyring_for_each_key()로 g_keyring 내 keys/removed_keys TAILQ를 락
 * 보호하에 순회하면서, 각 키마다 keyring_dump_key_info()로 JSON object를 채워
 * 클라이언트에 응답한다. 키 추가/삭제 RPC는 각 keyring 모듈(file/Linux kernel
 * keyring 등)이 자체적으로 등록하기 때문에 본 파일에는 read-only 조회 RPC만
 * 존재한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 애플리케이션이 부팅된 뒤 RUNTIME 상태(framework_start_init RPC 이후)에
 * 들어가면 활성화되는 RPC. 호출 체인:
 *   client (spdk_rpc.py) → AF_UNIX socket → lib/jsonrpc → lib/rpc/rpc.c의
 *   jsonrpc_handler → method registry에서 "keyring_get_keys" 매칭 →
 *   rpc_keyring_get_keys (본 파일) → spdk_keyring_for_each_key (keyring.c) →
 *   rpc_keyring_for_each_key_cb (본 파일) per-key → keyring_dump_key_info
 *   (keyring.c) → module->dump_info(...) (모듈별 부가 필드, 예: file 모듈은
 *   path, kernel 모듈은 key_id 등).
 * 응답 JSON: 키 객체들의 array. 각 객체는 name/module/removed/probed/refcnt +
 *   모듈별 부가 필드를 담는다.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - keyring_internal.h: keyring_dump_key_info() 선언 — 같은 라이브러리 내부
 *     헬퍼를 호출하기 위해 internal header 사용.
 *   - spdk/keyring.h: spdk_keyring_for_each_key, spdk_key opaque, FOR_EACH_ALL
 *     플래그.
 *   - spdk/rpc.h: SPDK_RPC_REGISTER 매크로, spdk_jsonrpc_request 등 RPC
 *     framework API. (실제 method registry는 lib/rpc/rpc.c의 g_rpc_methods.)
 *   - spdk/util.h, spdk/string.h: 일반 유틸 (현 시점 직접 사용은 적지만 typical).
 * 의존됨: SPDK app 시작 시 SPDK_RPC_REGISTER constructor가 method를 등록하고,
 *   런타임에 외부 클라이언트가 호출할 수 있게 된다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - rpc_keyring_for_each_key_cb: 키 1개마다 호출되는 per-key 콜백. JSON
 *     object를 열고 keyring_dump_key_info로 필드 채운 뒤 닫는다.
 *   - rpc_keyring_get_keys: keyring_get_keys RPC의 진입점. 결과 array를 열고
 *     모든 키(active+removed)를 순회하여 클라이언트에 응답.
 *   - SPDK_RPC_REGISTER(...): 컴파일 시 constructor 트릭으로 g_rpc_methods에
 *     등록. RUNTIME 상태에서만 호출 가능 — 즉 framework_start_init 이전
 *     STARTUP 단계에서는 -32603 INVALID_STATE를 반환한다.
 */

#include "keyring_internal.h"  /* [한국어] keyring_dump_key_info 선언 (내부 헤더) */
#include "spdk/keyring.h"      /* [한국어] spdk_keyring_for_each_key, FOR_EACH_ALL 플래그 */
#include "spdk/rpc.h"          /* [한국어] SPDK_RPC_REGISTER, spdk_jsonrpc_* helpers */
#include "spdk/string.h"       /* [한국어] string 헬퍼 (간접 사용) */
#include "spdk/util.h"         /* [한국어] SPDK_COUNTOF 등 일반 유틸 */

/*
 * [한국어]
 * rpc_keyring_for_each_key_cb - keyring 순회 중 키 1개마다 호출되는 per-key 콜백
 *
 * @ctx: spdk_keyring_for_each_key 호출자가 넘긴 불투명 포인터. 본 RPC 경로에서는
 *       항상 spdk_json_write_ctx*를 캐스팅해서 받는다 (응답 JSON writer).
 * @key: keyring 내부 TAILQ에서 순회 중인 현재 키. removed_keys 큐의 키도 포함될
 *       수 있다 (rpc_keyring_get_keys에서 SPDK_KEYRING_FOR_EACH_ALL 플래그를
 *       건넸으므로). NULL 불가.
 *
 * 왜 필요한가: spdk_keyring_for_each_key는 일반화된 순회 API라 호출자별로 다른
 *   per-key 동작을 콜백으로 받는다. RPC 응답 빌더에서는 키 1개를 JSON object 1개로
 *   직렬화하고 싶으므로 그 어댑터를 본 콜백으로 제공한다.
 *
 * 동작:
 *   1) 응답 array 안에 새 object를 begin.
 *   2) 라이브러리 내부 헬퍼 keyring_dump_key_info로 표준 필드(name/module/...)와
 *      모듈별 부가 필드를 한꺼번에 기록.
 *   3) object를 end.
 *
 * 실행 컨텍스트: SPDK RPC thread (보통 main reactor) 위에서, g_keyring.mutex가
 *   걸린 채로 호출된다 — keyring_for_each_key가 락을 쥐고 fn을 부른다. 따라서
 *   본 콜백 안에서 키 lifecycle을 변경하는 RPC를 reentrant 하게 호출하면
 *   recursive mutex(keyring_init에서 PTHREAD_MUTEX_RECURSIVE로 설정) 덕분에
 *   데드락은 피하지만, 일반적으로 그런 일은 없다.
 *
 * 호출 체인:
 *   spdk_keyring_for_each_key (keyring.c) → [본 콜백] → keyring_dump_key_info
 */
static void
rpc_keyring_for_each_key_cb(void *ctx, struct spdk_key *key)
{
	struct spdk_json_write_ctx *w = ctx;     /* [한국어] ctx는 응답 JSON writer로 미리 약속됨 */

	spdk_json_write_object_begin(w);          /* [한국어] 키 1개를 표현하는 JSON object 시작 */
	keyring_dump_key_info(key, w);            /* [한국어] name/module/removed/probed/refcnt + 모듈 부가 필드 기록 */
	spdk_json_write_object_end(w);            /* [한국어] 객체 닫기 — array 안 한 원소 완성 */
}

/*
 * [한국어]
 * rpc_keyring_get_keys - "keyring_get_keys" JSON-RPC 메서드 핸들러
 *
 * @request: jsonrpc layer가 만든 요청 객체. 응답을 만들어 spdk_jsonrpc_end_result
 *           로 전달하면 jsonrpc layer가 wire에 직렬화한다.
 * @params : 요청 파라미터(JSON). 본 메서드는 파라미터를 받지 않으므로 NULL이거나
 *           빈 객체일 수 있다 — 검증하지 않는다.
 *
 * 왜 필요한가: 운영자/테스트 코드가 SPDK 인스턴스에 등록된 모든 키(TLS PSK,
 *   DEK, DH-CHAP secret 등)와 그 상태(removed/probed/refcnt)를 점검할 수 있어야
 *   한다. 키 자체의 비밀 데이터는 노출하지 않고 메타데이터만 노출한다.
 *
 * 동작:
 *   1) spdk_jsonrpc_begin_result로 응답 writer 획득. 이 시점부터 application은
 *      JSON value 1개를 그려야 한다.
 *   2) 결과 컨테이너로 array를 연다.
 *   3) spdk_keyring_for_each_key(NULL, w, cb, SPDK_KEYRING_FOR_EACH_ALL)로
 *      활성(g_keyring.keys) + removed(g_keyring.removed_keys) 모든 키를 순회.
 *      첫 인자 NULL은 "전역 keyring"을 의미 (현 SPDK는 다중 keyring 미지원).
 *   4) array 닫고 응답 종료.
 *
 * 실행 컨텍스트: lib/rpc/rpc.c의 jsonrpc_handler → 본 함수. SPDK 메인 thread.
 *   STARTUP 상태에서는 호출되지 않는다 — SPDK_RPC_RUNTIME 마스크.
 *
 * 에러 경로: 본 핸들러는 명시적 에러를 반환하지 않는다 — 키가 0개여도 빈
 *   array를 응답한다.
 *
 * 호출 체인:
 *   client → jsonrpc_handler (lib/rpc/rpc.c) → [본 함수] →
 *     spdk_keyring_for_each_key → rpc_keyring_for_each_key_cb (per key)
 */
static void
rpc_keyring_get_keys(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;       /* [한국어] 응답 JSON writer 핸들 */

	w = spdk_jsonrpc_begin_result(request);   /* [한국어] 성공 응답 시작 — id 매칭/result 필드 자동 처리 */
	spdk_json_write_array_begin(w);            /* [한국어] result 값으로 array 사용 — 키 객체들의 목록 */
	/* [한국어] 첫 인자 NULL = 전역 keyring 지정. SPDK_KEYRING_FOR_EACH_ALL 플래그는
	 * removed 상태(이미 백엔드에서 제거되었지만 refcnt가 남아 보존 중인) 키도
	 * 노출하라는 의미 — 운영자가 누가 ref를 잡고 있는지 디버깅할 수 있게 함. */
	spdk_keyring_for_each_key(NULL, w, rpc_keyring_for_each_key_cb, SPDK_KEYRING_FOR_EACH_ALL);
	spdk_json_write_array_end(w);              /* [한국어] array 닫기 */

	spdk_jsonrpc_end_result(request, w);       /* [한국어] 응답 마무리 — wire로 전송 */

}
/* [한국어] SPDK_RPC_REGISTER는 컴파일러 constructor 어트리뷰트로 main() 전에
 * spdk_rpc_register_method("keyring_get_keys", rpc_keyring_get_keys, SPDK_RPC_RUNTIME)
 * 를 호출해 g_rpc_methods SLIST에 등록한다. SPDK_RPC_RUNTIME state mask는
 * framework_start_init 이후(g_rpc_state == SPDK_RPC_RUNTIME)에만 호출 가능함을
 * 의미하며, STARTUP 단계 호출은 lib/rpc/rpc.c에서 INVALID_STATE 에러로 거부한다. */
SPDK_RPC_REGISTER("keyring_get_keys", rpc_keyring_get_keys, SPDK_RPC_RUNTIME)
