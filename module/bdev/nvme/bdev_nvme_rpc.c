/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2022 Dell Inc, or its subsidiaries. All rights reserved.
 */

/*
 * [한국어 설명] bdev_nvme 모듈의 JSON-RPC 핸들러 전체 집합 (bdev_nvme_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 bdev_nvme 모듈의 외부 제어 인터페이스인 JSON-RPC 메서드 약 30개 이상을
 * 구현하고 SPDK_RPC_REGISTER 매크로로 SPDK RPC 디스패처에 등록한다. 운영자 또는
 * 자동화 도구(scripts/rpc.py, spdk-cli 등)가 HTTP/Unix-socket JSON-RPC 2.0 프로토콜로
 * SPDK 프로세스에 명령을 내릴 때, 이 파일의 핸들러들이 해당 명령을 받아 처리한다.
 * NVMe 컨트롤러 동적 attach/detach, 멀티패스/페일오버 정책 변경, 핫플러그 on/off,
 * NVMe-oF 디스커버리 시작/중지, mDNS 디스커버리, DH-CHAP 인증 키 갱신, 펌웨어
 * 업데이트, SMART 상태 조회, I/O 통계 조회 등이 모두 이 파일을 통해 노출된다.
 * 각 핸들러는 SPDK JSON-RPC 2.0 패턴(요청 디코드 → 내부 API 위임 → 비동기 콜백
 * 응답)을 따르며, 동기 또는 비동기로 동작한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 SPDK bdev 스택의 "관리 평면(management plane)" 최상단에 위치한다.
 * 데이터 평면(I/O 경로)과는 분리되어 있으며, 순수하게 관리 명령 처리만 담당한다.
 *
 * 호출 체인 (관리 평면):
 *   외부 RPC 클라이언트 (scripts/rpc.py, custom admin tool 등)
 *     → Unix-socket / TCP JSON-RPC 요청
 *     → SPDK JSON-RPC 서버 (lib/jsonrpc/jsonrpc_server_tcp.c, lib/rpc/rpc.c)
 *     → SPDK_RPC_REGISTER로 등록된 본 파일의 rpc_bdev_nvme_*() 핸들러
 *     → bdev_nvme.h 에 선언된 내부 함수 (bdev_nvme.c 구현)
 *         → spdk_bdev_nvme_create() / spdk_bdev_nvme_delete() 등 공개 API
 *         → lib/nvme의 spdk_nvme_connect_async() / spdk_nvme_ctrlr_reset() 등
 *     → 비동기 완료 시: 각 핸들러의 _done/_cb 콜백이 JSON 응답 직렬화 후 전송
 *
 * 실행 컨텍스트: 모든 핸들러는 SPDK app 스레드(RPC 폴러가 실행되는 스레드)에서
 * 시작한다. 비동기 완료 콜백은 일반적으로 NVMe admin queue를 폴링하는 동일 스레드
 * 또는 spdk_thread_send_msg로 전달된 다른 SPDK 스레드에서 실행된다.
 * RPC 핸들러 자체는 재진입이 불가능하므로 한 번에 한 요청만 처리한다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 모듈:
 *   - bdev_nvme.h / bdev_nvme.c: bdev_nvme 내부 공개 API
 *     (spdk_bdev_nvme_create, spdk_bdev_nvme_delete, bdev_nvme_set_hotplug,
 *      bdev_nvme_start_discovery, bdev_nvme_stop_discovery,
 *      bdev_nvme_set_preferred_path, spdk_bdev_nvme_set_multipath_policy,
 *      nvme_ctrlr_get_by_name, nvme_bdev_ctrlr_get_by_name 등)
 *   - spdk/rpc.h: SPDK_RPC_REGISTER 매크로, SPDK_RPC_STARTUP/RUNTIME 상태 플래그
 *   - spdk/jsonrpc.h: spdk_jsonrpc_request, spdk_jsonrpc_begin_result,
 *     spdk_jsonrpc_end_result, spdk_jsonrpc_send_bool_response,
 *     spdk_jsonrpc_send_error_response 등
 *   - spdk/json.h: spdk_json_decode_object, spdk_json_decode_string,
 *     spdk_json_write_* 패밀리
 *   - spdk/nvme.h: spdk_nvme_transport_id, spdk_nvme_ctrlr_opts 등
 *   - spdk/nvme_spec.h: NVMe opcode/feature/log page 상수
 *   - lib/keyring: DH-CHAP 키 이름으로 secret 조회 (bdev_nvme.c 경유)
 *   - bdev_mdns_client.c: mDNS 디스커버리 (bdev_nvme_start/stop_mdns_discovery)
 *
 * 이 파일에 의존하는 모듈:
 *   - 없음. SPDK_RPC_REGISTER로 전역 RPC 테이블에 자동 등록되므로 외부 헤더 불필요.
 *
 * 데이터 흐름: 외부 JSON 문자열 → spdk_json_decode_object()로 C 구조체 채움
 *   → 내부 bdev_nvme API 호출 → 비동기 완료 콜백에서 spdk_json_write_*()로
 *   JSON 응답 빌드 → 클라이언트로 전송.
 * ctx(컨텍스트) 패턴: 비동기 RPC마다 calloc으로 컨텍스트 구조체를 할당하고,
 * 완료 콜백에서 free한다. 이 패턴으로 비동기 연산 중 요청 상태를 보존한다.
 *
 * === 주요 함수/구조체 요약 ===
 * 핵심 RPC 핸들러 (각각 SPDK_RPC_REGISTER로 등록):
 *   - rpc_bdev_nvme_set_options(): 모듈 전역 옵션 일괄 설정 (timeout/retry/DH-CHAP 등).
 *   - rpc_bdev_nvme_set_hotplug(): PCIe 핫플러그 폴링 활성/비활성 + 주기 설정.
 *   - rpc_bdev_nvme_attach_controller(): NVMe 컨트롤러 동적 연결 (PCIe/TCP/RDMA).
 *     내부에서 trid 파싱 + 멀티패스 검증 + spdk_bdev_nvme_create() 호출.
 *   - rpc_bdev_nvme_detach_controller(): 컨트롤러 또는 특정 path만 분리.
 *   - rpc_bdev_nvme_get_controllers(): 등록된 컨트롤러 전체 또는 특정 이름 조회.
 *   - rpc_bdev_nvme_apply_firmware(): Firmware Image Download + Commit (4KiB 청크 루프).
 *   - rpc_bdev_nvme_reset_controller() / _enable / _disable: 컨트롤러 상태 제어.
 *   - rpc_bdev_nvme_get_controller_health_info(): SMART/Health Log Page 0x02 조회.
 *   - rpc_bdev_nvme_start_discovery() / _stop / _get_info: NVMe-oF 자동 디스커버리.
 *   - rpc_bdev_nvme_start_mdns_discovery() / _stop / _get_info: mDNS 기반 디스커버리.
 *   - rpc_bdev_nvme_set_multipath_policy(): active-passive/active-active 정책 변경.
 *   - rpc_bdev_nvme_set_preferred_path(): 특정 path를 우선 경로로 고정.
 *   - rpc_bdev_nvme_get_io_paths(): 모든 poll_group의 io_path 상태 조회 (채널 순회).
 *   - rpc_bdev_nvme_get_path_iostat(): path별 IO 통계 누적 조회 (채널 순회).
 *   - rpc_bdev_nvme_get_transport_statistics(): transport별 poll group 통계 조회.
 *   - rpc_bdev_nvme_add/remove_error_injection(): NVMe 명령 error injection (디버깅용).
 *   - rpc_bdev_nvme_set_keys(): DH-CHAP 인증 키 동적 갱신(rotation).
 *
 * 주요 컨텍스트 구조체:
 *   - struct rpc_bdev_nvme_attach_controller_ctx: attach RPC 비동기 상태.
 *   - struct firmware_update_info: 펌웨어 업데이트 청크 진행 상태.
 *   - struct rpc_bdev_nvme_transport_stat_ctx: transport 통계 채널 순회 상태.
 *   - struct rpc_bdev_nvme_path_stat_ctx: path별 IO 통계 채널 순회 상태.
 *   - struct spdk_nvme_health_info_context: SMART 정보 admin 명령 비동기 상태.
 */

#include "spdk/stdinc.h"
/* [한국어] SPDK 공통 표준 헤더 묶음 (stdio/stdlib/string/pthread/sys/... 포함).
 * 플랫폼 차이를 흡수하고 SPDK 전체에서 공통 타입을 보장한다. */

#include "bdev_nvme.h"
/* [한국어] module/bdev/nvme/ 내부 공개 API 헤더.
 * nvme_ctrlr/nvme_bdev/nvme_ns/nvme_io_path 등 핵심 구조체 정의 +
 * spdk_bdev_nvme_create/delete, bdev_nvme_set_hotplug, bdev_nvme_start/stop_discovery,
 * nvme_ctrlr_get_by_name, nvme_bdev_ctrlr_get_by_name 등 내부 API 선언.
 * 이 파일의 모든 핸들러가 bdev_nvme.c의 구현에 접근할 수 있게 해주는 유일한 게이트. */

#include "spdk/config.h"
/* [한국어] configure 산출 헤더 (SPDK_CONFIG_* 매크로).
 * SPDK_CONFIG_NVME_CUSE, SPDK_CONFIG_RDMA 등 빌드-시 활성화된 기능 플래그를
 * 조건부 컴파일에 사용한다. 이 파일에서는 mDNS 관련 CONFIG_HAVE_AVAHI 분기에 쓸 수 있다. */

#include "spdk/string.h"
/* [한국어] SPDK 문자열 유틸 (spdk_str_trim, spdk_sprintf_alloc, spdk_strtol 등).
 * 모델/시리얼 번호의 trailing space 제거, 정수 파싱에 사용한다. */

#include "spdk/rpc.h"
/* [한국어] SPDK JSON-RPC 메서드 등록 인프라.
 * SPDK_RPC_REGISTER(name, handler, state) 매크로로 핸들러를 전역 slist에 등록한다.
 * SPDK_RPC_STARTUP: 초기화 단계에서만 호출 가능.
 * SPDK_RPC_RUNTIME: 런타임(SPDK 앱 실행 중)에 호출 가능.
 * SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME: 두 단계 모두 허용. */

#include "spdk/util.h"
/* [한국어] SPDK_COUNTOF, SPDK_BIT, spdk_min 등 범용 유틸 매크로.
 * 디코더 테이블 길이 계산(SPDK_COUNTOF), 비트 플래그 설정(SPDK_BIT),
 * 청크 크기 계산(spdk_min)에 사용된다. */

#include "spdk/env.h"
/* [한국어] SPDK 환경/메모리 추상화 (DPDK 기반).
 * spdk_zmalloc/spdk_free로 hugepage DMA 버퍼를 할당한다 (firmware 업데이트 시).
 * SPDK_ENV_LCORE_ID_ANY: 어느 NUMA node든 허용 / SPDK_MALLOC_DMA: DMA 가능 메모리 요구. */

#include "spdk/nvme.h"
/* [한국어] lib/nvme 유저스페이스 NVMe 드라이버 공개 API.
 * spdk_nvme_transport_id, spdk_nvme_ctrlr_opts, spdk_nvme_ctrlr_cmd_get_log_page,
 * spdk_nvme_ctrlr_cmd_admin_raw, spdk_nvme_ctrlr_get_data, spdk_nvme_ctrlr_get_transport_id,
 * spdk_nvme_ctrlr_reset, spdk_nvme_qpair_add/remove_cmd_error_injection 등. */

#include "spdk/nvme_spec.h"
/* [한국어] NVMe Base Spec 와이어 포맷 상수/구조체.
 * SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD(0x11), SPDK_NVME_OPC_FIRMWARE_COMMIT(0x10),
 * SPDK_NVME_OPC_GET_FEATURES, SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD,
 * SPDK_NVME_LOG_HEALTH_INFORMATION, SPDK_NVME_GLOBAL_NS_TAG,
 * struct spdk_nvme_health_information_page, struct spdk_nvme_fw_commit 등. */

#include "spdk/log.h"
/* [한국어] SPDK 로깅 매크로 (SPDK_ERRLOG, SPDK_WARNLOG, SPDK_NOTICELOG 등).
 * JSON 파싱 실패, 컨트롤러 lookup 실패 등 경고 상황에 사용한다. */

#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈 내부 API (모듈 작성자용).
 * spdk_bdev_wait_for_examine, spdk_bdev_open_ext, spdk_bdev_desc_get_bdev,
 * spdk_bdev_get_io_channel, spdk_bdev_close, spdk_bdev_add_io_stat,
 * spdk_bdev_dump_io_stat_json 등. attach RPC 완료 시 examine 대기에 필수. */

/* [한국어] TLS(NVMe-oF over TLS) 최초 사용 시 경고 로그를 1회만 출력하기 위한 가드.
 * 설정자: rpc_bdev_nvme_attach_controller()에서 psk 파라미터가 있는 첫 호출 시 true로 설정.
 * 읽는 자: 동일 함수 내의 if (!g_tls_log) 조건. true이면 다시 출력하지 않음.
 * 값 범위: false(초기값, 아직 TLS attach 없음) / true(이미 경고 출력됨).
 * 동기화: 단일 SPDK app 스레드에서만 RPC 핸들러가 실행되므로 별도 락 불필요.
 *         SPDK는 RPC를 직렬로 처리하므로 race condition 없음. */
static bool g_tls_log = false;

/*
 * [한국어]
 * rpc_decode_action_on_timeout - "none"/"abort"/"reset" 문자열을 타임아웃 동작 enum으로 변환.
 *
 * @val: JSON 문자열 값 토큰. spdk_json_strequal로 문자열 비교에 사용.
 * @out: enum spdk_bdev_timeout_action* 포인터. 해당 enum 값으로 채워진다.
 * @return: 0(성공) 또는 -EINVAL(인식 불가 문자열).
 *
 * spdk_json_object_decoder 테이블의 decode 함수 포인터로 등록되어
 * spdk_json_decode_object() 호출 시 "action_on_timeout" 키의 값을 변환한다.
 * NVMe IO 타임아웃 발생 시 SPDK가 취할 동작을 문자열로 받아 enum으로 변환:
 *   - "none":  SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE  → 타임아웃 로그만, IO는 계속 대기.
 *   - "abort": SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT → NVMe Abort 명령(0x08)으로 해당 IO 취소.
 *   - "reset": SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET → 컨트롤러 전체 reset (CC.EN 0→1).
 *
 * 호출 체인: spdk_json_decode_object() → [이 함수] (인라인 호출, 직접 호출 없음)
 * 실행 컨텍스트: SPDK app 스레드 (RPC 핸들러 내에서 동기 호출).
 */
static int
rpc_decode_action_on_timeout(const struct spdk_json_val *val, void *out)
{
	enum spdk_bdev_timeout_action *action = out; /* [한국어] 변환 결과를 저장할 out 포인터를 enum 타입으로 캐스팅. */

	if (spdk_json_strequal(val, "none") == true) {
		/* [한국어] "none": 타임아웃 이벤트를 기록만 하고 IO는 계속 진행. */
		*action = SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE;
	} else if (spdk_json_strequal(val, "abort") == true) {
		/* [한국어] "abort": NVMe Abort(opcode 0x08)로 해당 명령 취소 요청. */
		*action = SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT;
	} else if (spdk_json_strequal(val, "reset") == true) {
		/* [한국어] "reset": 컨트롤러 전체 reset (CC.EN=0 → CSTS.RDY=0 대기 → CC.EN=1). */
		*action = SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET;
	} else {
		/* [한국어] 알 수 없는 문자열 → 에러 로그 + -EINVAL 반환으로 디코더 실패 알림. */
		SPDK_NOTICELOG("Invalid parameter value: action_on_timeout\n");
		return -EINVAL;
	}

	return 0; /* [한국어] 변환 성공. */
}

/*
 * [한국어]
 * rpc_decode_digest - DH-CHAP digest 알고리즘 이름 문자열 1개를 비트 플래그로 누적.
 *
 * @val: JSON 문자열 토큰. NVMe DH-HMAC-CHAP 표준 digest 이름이어야 한다
 *       ("sha256", "sha384", "sha512"). NVMe Base Spec §8.6 DH-HMAC-CHAP 참조.
 * @out: uint32_t* 누적 비트마스크. rpc_decode_digest_array()가 배열 원소마다 이 함수를
 *       호출하며, 각 호출에서 해당 알고리즘의 비트를 OR로 추가한다.
 * @return: 0(성공) 또는 음수 에러코드 (-EINVAL: 알 수 없는 이름).
 *
 * spdk_nvme_dhchap_get_digest_id()는 lib/nvme/nvme_auth.c가 제공하며,
 * 이름을 NVMe DH-CHAP 표준 digest identifier(정수)로 변환한다.
 * SPDK_BIT(id)로 id번째 비트를 flags에 OR-set하면, 이 비트마스크로
 * 어떤 digest들이 허용되는지를 비트 집합으로 표현한다.
 * 실행 컨텍스트: SPDK app 스레드 (spdk_json_decode_array 내에서 동기 호출).
 *
 * 호출 체인: rpc_decode_digest_array() → spdk_json_decode_array() → [이 함수]
 */
static int
rpc_decode_digest(const struct spdk_json_val *val, void *out)
{
	uint32_t *flags = out;      /* [한국어] 누적 비트마스크 포인터 (rpc_decode_digest_array가 전달). */
	char *digest = NULL;        /* [한국어] JSON에서 추출한 C 문자열 (spdk_json_decode_string이 strdup). */
	int rc;

	rc = spdk_json_decode_string(val, &digest); /* [한국어] JSON 문자열 토큰 → heap 문자열로 복사. */
	if (rc != 0) {
		/* [한국어] JSON 값이 문자열 타입이 아닌 경우. 디코딩 실패 반환. */
		return rc;
	}

	rc = spdk_nvme_dhchap_get_digest_id(digest); /* [한국어] 이름 → NVMe DH-CHAP digest ID 변환. */
	if (rc >= 0) {
		/* [한국어] 유효한 digest 이름: ID 비트를 누적 마스크에 OR-set.
		 * SPDK_BIT(rc) = 1 << rc (rc는 digest ID, 최대 3개이므로 32비트 내에 수용). */
		*flags |= SPDK_BIT(rc);
		rc = 0; /* [한국어] 함수 성공 코드로 재정의 (id 값을 반환값으로 오해하지 않도록). */
	}
	free(digest); /* [한국어] spdk_json_decode_string이 strdup한 문자열 반환 (사용 완료). */

	return rc; /* [한국어] 0: 성공, 음수: 알 수 없는 digest 이름 (get_digest_id 실패). */
}

/*
 * [한국어]
 * rpc_decode_digest_array - JSON 문자열 배열의 각 digest 이름을 비트마스크로 누적.
 *
 * @val: JSON 배열 토큰 (예: ["sha256", "sha384"]).
 * @out: uint32_t* 결과 비트마스크. 배열의 모든 항목이 OR로 누적된다.
 * @return: 0(성공) 또는 음수 에러코드.
 *
 * spdk_json_object_decoder 테이블에서 "dhchap_digests" 키의 디코더로 등록된다.
 * spdk_json_decode_array()가 배열의 각 원소에 대해 rpc_decode_digest()를 호출한다.
 * 최대 32개 항목 (uint32_t 비트 수)까지 처리한다.
 * *flags를 0으로 초기화한 후 누적하므로, 배열이 비어 있으면 0(모든 digest 비활성)이 된다.
 *
 * 호출 체인: spdk_json_decode_object() → [이 함수] → spdk_json_decode_array() → rpc_decode_digest()
 */
static int
rpc_decode_digest_array(const struct spdk_json_val *val, void *out)
{
	uint32_t *flags = out; /* [한국어] 결과 비트마스크 포인터. */
	size_t count;          /* [한국어] 처리된 배열 항목 수 (호출자는 사용하지 않아 무시). */

	*flags = 0; /* [한국어] 이전 값 클리어 후 누적 시작. 배열이 비면 0이 그대로 유지됨. */

	/* [한국어] 배열 각 원소를 rpc_decode_digest()로 처리. 원소당 플래그 누적.
	 * 마지막 인자 0: element_size=0 (rpc_decode_digest 자체가 out을 받으므로 개별 크기 불필요). */
	return spdk_json_decode_array(val, rpc_decode_digest, out, 32, &count, 0);
}

/*
 * [한국어]
 * rpc_decode_dhgroup - DH-CHAP DH 그룹 이름 1개를 비트 플래그로 누적 (rpc_decode_digest와 패턴 동일).
 *
 * @val: JSON 문자열 토큰. NVMe DH-HMAC-CHAP DH 그룹 이름이어야 한다.
 *       RFC 7919 finite field DH 그룹: "null"(DH 미사용), "ffdhe2048", "ffdhe3072",
 *       "ffdhe4096", "ffdhe6144", "ffdhe8192".
 * @out: uint32_t* 누적 비트마스크. rpc_decode_dhgroup_array() 호출 시 공유되는 포인터.
 * @return: 0(성공) 또는 음수 에러코드.
 *
 * spdk_nvme_dhchap_get_dhgroup_id()는 lib/nvme/nvme_auth.c가 제공한다.
 * digest와 동일한 비트마스크 패턴으로, 어떤 DH 그룹들이 협상 시 허용되는지를 표현한다.
 * NVMe Base Spec §8.6.3: DH 그룹 null은 암호화 없이 HMAC만 수행 (보안 취약, 비권장).
 *
 * 호출 체인: rpc_decode_dhgroup_array() → spdk_json_decode_array() → [이 함수]
 */
static int
rpc_decode_dhgroup(const struct spdk_json_val *val, void *out)
{
	uint32_t *flags = out;       /* [한국어] 누적 비트마스크 포인터. */
	char *dhgroup = NULL;        /* [한국어] JSON에서 추출한 DH 그룹 이름 (heap 할당). */
	int rc;

	rc = spdk_json_decode_string(val, &dhgroup); /* [한국어] JSON 문자열 → C 문자열로 변환. */
	if (rc != 0) {
		/* [한국어] 문자열이 아닌 JSON 토큰인 경우 실패. */
		return rc;
	}

	rc = spdk_nvme_dhchap_get_dhgroup_id(dhgroup); /* [한국어] 이름 → NVMe DH 그룹 ID 변환. */
	if (rc >= 0) {
		/* [한국어] 유효한 DH 그룹 이름: 해당 비트를 마스크에 OR-set. */
		*flags |= SPDK_BIT(rc);
		rc = 0; /* [한국어] 성공 코드로 통일. */
	}
	free(dhgroup); /* [한국어] 디코드 시 할당된 문자열 반환. */

	return rc;
}

/*
 * [한국어]
 * rpc_decode_dhgroup_array - JSON DH 그룹 이름 배열을 비트마스크로 누적 (digest_array와 패턴 동일).
 *
 * @val: JSON 배열 토큰 (예: ["ffdhe2048", "ffdhe4096"]).
 * @out: uint32_t* 결과 비트마스크.
 * @return: 0(성공) 또는 음수 에러코드.
 *
 * "dhchap_dhgroups" JSON 키의 디코더로 rpc_bdev_nvme_set_options_decoders에 등록된다.
 * *flags=0 초기화 후 spdk_json_decode_array로 배열 원소마다 rpc_decode_dhgroup() 호출.
 *
 * 호출 체인: spdk_json_decode_object() → [이 함수] → spdk_json_decode_array() → rpc_decode_dhgroup()
 */
static int
rpc_decode_dhgroup_array(const struct spdk_json_val *val, void *out)
{
	uint32_t *flags = out; /* [한국어] 결과 비트마스크 포인터. */
	size_t count;          /* [한국어] 처리된 원소 수 (외부에서 사용 안 함). */

	*flags = 0; /* [한국어] 누적 전 초기화. */

	/* [한국어] 배열 원소마다 rpc_decode_dhgroup() 호출로 비트 OR 누적.
	 * 최대 32개 항목 처리 (uint32_t 비트 수). */
	return spdk_json_decode_array(val, rpc_decode_dhgroup, out, 32, &count, 0);
}

/*
 * [한국어] bdev_nvme_set_options RPC의 JSON 파라미터 디코더 테이블.
 * spdk_json_object_decoder 배열: {JSON 키, 구조체 필드 오프셋, 디코더 함수, optional}.
 * 모든 필드는 optional=true — 지정되지 않은 필드는 기존 값(spdk_bdev_nvme_get_opts로 읽은 값)을 유지.
 * 각 필드의 의미:
 *   action_on_timeout: IO 타임아웃 시 동작 (none/abort/reset).
 *   keep_alive_timeout_ms: NVMe-oF Keep Alive Timeout (0=비활성, 단위: ms).
 *   timeout_us: IO 명령 타임아웃 (0=비활성, 단위: μs).
 *   timeout_admin_us: Admin 명령 타임아웃 (0=비활성, 단위: μs).
 *   arbitration_burst: NVMe 중재 버스트 크기 (NVMe CC.AB 필드, 2^n 단위).
 *   low/medium/high_priority_weight: 우선순위 큐 가중치 (CC.APM 필드).
 *   io_queue_requests: IO qpair당 최대 동시 요청 수.
 *   nvme_adminq_poll_period_us: Admin queue 폴링 주기 (μs).
 *   nvme_ioq_poll_period_us: IO queue 폴링 주기 (μs, 0=즉시 폴링).
 *   delay_cmd_submit: SQ 도어벨을 배치 지연 제출 (MMIO 감소 목적).
 *   transport_retry_count: transport 레벨 재시도 횟수.
 *   bdev_retry_count: bdev 레벨 IO 실패 재시도 횟수 (-1=무한, 0=재시도 없음).
 *   ctrlr_loss_timeout_sec: 컨트롤러 연결 손실 후 포기까지 대기 시간 (-1=무한).
 *   reconnect_delay_sec: 재연결 시도 간격 (초).
 *   fast_io_fail_timeout_sec: 빠른 IO 실패 타임아웃 (초, ANA 우회 경로 전환 등).
 *   transport_ack_timeout: transport ACK 타임아웃 지수값 (2^n × 100ms, RDMA용).
 *   disable_auto_failback: 기본 경로 복구 후 자동 failback 비활성화.
 *   generate_uuids: namespace uuid 자동 생성 (컨트롤러가 UUID를 제공하지 않을 때).
 *   transport_tos: TCP/RDMA transport TOS(Type of Service) 값 (DSCP 등).
 *   nvme_error_stat: 에러 통계 추적 활성화.
 *   io_path_stat: path별 IO 통계 추적 활성화 (get_path_iostat RPC 사용 전제).
 *   allow_accel_sequence: accel 시퀀스(checksum offload 등) 허용.
 *   rdma_srq_size: RDMA SRQ(Shared Receive Queue) 크기.
 *   rdma_max_cq_size: RDMA CQ(Completion Queue) 최대 크기.
 *   rdma_cm_event_timeout_ms: RDMA CM 이벤트 타임아웃 (ms).
 *   dhchap_digests: 허용 DH-CHAP digest 알고리즘 배열 (비트마스크로 변환).
 *   dhchap_dhgroups: 허용 DH-CHAP DH 그룹 배열 (비트마스크로 변환).
 *   rdma_umr_per_io: RDMA UMR(User Memory Region)을 IO당 생성.
 *   tcp_connect_timeout_ms: TCP 연결 타임아웃 (ms).
 *   enable_flush: NVMe Flush 명령 지원 활성화 (bdev flush 콜에서 실제 NVMe Flush 발행).
 */
static const struct spdk_json_object_decoder rpc_bdev_nvme_set_options_decoders[] = {
	{"action_on_timeout", offsetof(struct spdk_bdev_nvme_opts, action_on_timeout), rpc_decode_action_on_timeout, true},
	{"keep_alive_timeout_ms", offsetof(struct spdk_bdev_nvme_opts, keep_alive_timeout_ms), spdk_json_decode_uint32, true},
	{"timeout_us", offsetof(struct spdk_bdev_nvme_opts, timeout_us), spdk_json_decode_uint64, true},
	{"timeout_admin_us", offsetof(struct spdk_bdev_nvme_opts, timeout_admin_us), spdk_json_decode_uint64, true},
	{"arbitration_burst", offsetof(struct spdk_bdev_nvme_opts, arbitration_burst), spdk_json_decode_uint32, true},
	{"low_priority_weight", offsetof(struct spdk_bdev_nvme_opts, low_priority_weight), spdk_json_decode_uint32, true},
	{"medium_priority_weight", offsetof(struct spdk_bdev_nvme_opts, medium_priority_weight), spdk_json_decode_uint32, true},
	{"high_priority_weight", offsetof(struct spdk_bdev_nvme_opts, high_priority_weight), spdk_json_decode_uint32, true},
	{"io_queue_requests", offsetof(struct spdk_bdev_nvme_opts, io_queue_requests), spdk_json_decode_uint32, true},
	{"nvme_adminq_poll_period_us", offsetof(struct spdk_bdev_nvme_opts, nvme_adminq_poll_period_us), spdk_json_decode_uint64, true},
	{"nvme_ioq_poll_period_us", offsetof(struct spdk_bdev_nvme_opts, nvme_ioq_poll_period_us), spdk_json_decode_uint64, true},
	{"delay_cmd_submit", offsetof(struct spdk_bdev_nvme_opts, delay_cmd_submit), spdk_json_decode_bool, true},
	{"transport_retry_count", offsetof(struct spdk_bdev_nvme_opts, transport_retry_count), spdk_json_decode_uint32, true},
	{"bdev_retry_count", offsetof(struct spdk_bdev_nvme_opts, bdev_retry_count), spdk_json_decode_int32, true},
	{"ctrlr_loss_timeout_sec", offsetof(struct spdk_bdev_nvme_opts, ctrlr_loss_timeout_sec), spdk_json_decode_int32, true},
	{"reconnect_delay_sec", offsetof(struct spdk_bdev_nvme_opts, reconnect_delay_sec), spdk_json_decode_uint32, true},
	{"fast_io_fail_timeout_sec", offsetof(struct spdk_bdev_nvme_opts, fast_io_fail_timeout_sec), spdk_json_decode_uint32, true},
	{"transport_ack_timeout", offsetof(struct spdk_bdev_nvme_opts, transport_ack_timeout), spdk_json_decode_uint8, true},
	{"disable_auto_failback", offsetof(struct spdk_bdev_nvme_opts, disable_auto_failback), spdk_json_decode_bool, true},
	{"generate_uuids", offsetof(struct spdk_bdev_nvme_opts, generate_uuids), spdk_json_decode_bool, true},
	{"transport_tos", offsetof(struct spdk_bdev_nvme_opts, transport_tos), spdk_json_decode_uint8, true},
	{"nvme_error_stat", offsetof(struct spdk_bdev_nvme_opts, nvme_error_stat), spdk_json_decode_bool, true},
	{"io_path_stat", offsetof(struct spdk_bdev_nvme_opts, io_path_stat), spdk_json_decode_bool, true},
	{"allow_accel_sequence", offsetof(struct spdk_bdev_nvme_opts, allow_accel_sequence), spdk_json_decode_bool, true},
	{"rdma_srq_size", offsetof(struct spdk_bdev_nvme_opts, rdma_srq_size), spdk_json_decode_uint32, true},
	{"rdma_max_cq_size", offsetof(struct spdk_bdev_nvme_opts, rdma_max_cq_size), spdk_json_decode_uint32, true},
	{"rdma_cm_event_timeout_ms", offsetof(struct spdk_bdev_nvme_opts, rdma_cm_event_timeout_ms), spdk_json_decode_uint16, true},
	{"dhchap_digests", offsetof(struct spdk_bdev_nvme_opts, dhchap_digests), rpc_decode_digest_array, true},
	{"dhchap_dhgroups", offsetof(struct spdk_bdev_nvme_opts, dhchap_dhgroups), rpc_decode_dhgroup_array, true},
	{"rdma_umr_per_io", offsetof(struct spdk_bdev_nvme_opts, rdma_umr_per_io), spdk_json_decode_bool, true},
	{"tcp_connect_timeout_ms", offsetof(struct spdk_bdev_nvme_opts, tcp_connect_timeout_ms), spdk_json_decode_uint32, true},
	{"enable_flush", offsetof(struct spdk_bdev_nvme_opts, enable_flush), spdk_json_decode_bool, true},
};

/*
 * [한국어]
 * rpc_bdev_nvme_set_options - "bdev_nvme_set_options" RPC: 모듈 전역 옵션 설정.
 *
 * @request: SPDK JSON-RPC 요청 핸들. 응답 전송 시 사용.
 * @params: JSON 파라미터 토큰. NULL이면 모든 필드를 현재 기본값으로 유지.
 * @return: void. 성공 시 bool true, 실패 시 errno JSON 에러 응답.
 *
 * 이 RPC는 bdev_nvme 모듈의 전역 설정을 변경한다. 일반적으로 NVMe 컨트롤러를
 * attach하기 전(startup 단계)에 호출해야 한다. attach 이후 런타임에 호출해도
 * 이미 attach된 컨트롤러에는 적용되지 않으며 일부 옵션은 -EPERM으로 거부된다.
 *
 * 동작 단계:
 *   1) spdk_bdev_nvme_get_opts(): 현재 적용된 옵션 스냅샷을 opts에 복사.
 *      입력 JSON에 없는 필드를 현재 값으로 유지하기 위한 읽기-수정-쓰기 패턴.
 *   2) spdk_json_decode_object(): JSON 입력의 지정된 필드만 opts를 덮어씀.
 *      모든 디코더 항목이 optional=true이므로 일부 필드만 변경 가능.
 *   3) spdk_bdev_nvme_set_opts(): 수정된 opts를 전역 g_opts에 적용.
 *      컨트롤러가 이미 attach되어 있으면 -EPERM 반환.
 *
 * 실행 컨텍스트: SPDK app 스레드 (RPC 폴러가 실행되는 스레드). 동기 실행.
 * SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME으로 등록되어 두 단계 모두에서 호출 가능.
 *
 * 호출 체인:
 *   RPC 클라이언트 → JSON-RPC 서버 → [이 함수] → spdk_bdev_nvme_set_opts()
 */
static void
rpc_bdev_nvme_set_options(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct spdk_bdev_nvme_opts opts; /* [한국어] 현재 옵션을 받고 수정할 임시 opts 구조체. */
	int rc;                          /* [한국어] set_opts 반환 코드 (0: 성공, -EPERM: attach 후 거부 등). */

	/* [한국어] 1단계: 현재 전역 옵션을 읽어온다.
	 * opts에 채워진 값은 JSON에서 명시되지 않은 필드들의 기본값 역할을 한다.
	 * sizeof(opts)는 ABI 호환성을 위해 전달하는 opts_size. */
	spdk_bdev_nvme_get_opts(&opts, sizeof(opts));

	/* [한국어] 2단계: JSON params가 있으면 디코딩하여 opts 필드를 선택적으로 덮어씀.
	 * params=NULL이면 디코딩을 건너뛰고 현재 옵션을 그대로 사용한다 (no-op에 가까움).
	 * 디코딩 실패 시 오류 응답 후 즉시 반환. */
	if (params && spdk_json_decode_object(params, rpc_bdev_nvme_set_options_decoders,
					      SPDK_COUNTOF(rpc_bdev_nvme_set_options_decoders),
					      &opts)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n"); /* [한국어] 파싱 실패 로그. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return; /* [한국어] 에러 응답 후 즉시 종료. opts는 스택 변수이므로 별도 해제 불필요. */
	}

	/* [한국어] 3단계: 수정된 opts를 전역에 적용.
	 * -EPERM: 이미 컨트롤러가 attach되어 있어 변경 불가 (attach 시점에 opts를 소비하기 때문).
	 * 기타 rc: 유효성 검사 실패 등. */
	rc = spdk_bdev_nvme_set_opts(&opts);
	if (rc == -EPERM) {
		/* [한국어] 컨트롤러가 이미 존재하는 상태에서 변경 불가한 옵션을 수정하려 했을 때. */
		spdk_jsonrpc_send_error_response(request, -EPERM,
						 "RPC not permitted with nvme controllers already attached");
	} else if (rc) {
		/* [한국어] 그 외 오류 (유효 범위 초과 등). */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	} else {
		/* [한국어] 성공: JSON-RPC 응답 "result": true. */
		spdk_jsonrpc_send_bool_response(request, true);
	}

	return;
}
/* [한국어] SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME: 부팅 초기화 단계(attach 전)와
 * 런타임 단계 모두에서 호출 가능. startup 단계에 호출하는 것이 권장됨. */
SPDK_RPC_REGISTER("bdev_nvme_set_options", rpc_bdev_nvme_set_options,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_bdev_nvme_hotplug - bdev_nvme_set_hotplug RPC 입력 파라미터 구조체.
 *
 * JSON 입력 형식: {"enable": <bool>, "period_us": <uint64>?}
 * enabled와 period_us를 이 구조체에 디코딩한 뒤 bdev_nvme_set_hotplug()로 전달한다.
 */
struct rpc_bdev_nvme_hotplug {
	bool enabled;
	/* [한국어] 핫플러그 폴링 활성화 여부.
	 * 설정자: rpc_bdev_nvme_set_hotplug()가 spdk_json_decode_object()를 통해 채움.
	 * 읽는 자: rpc_bdev_nvme_set_hotplug()가 bdev_nvme_set_hotplug()에 전달.
	 * 값 범위: true(활성화) / false(비활성화). JSON 필수 필드.
	 * 동기화: SPDK app 스레드에서 단일 처리, race 없음. */

	uint64_t period_us;
	/* [한국어] PCIe 핫플러그 이벤트 감지를 위한 폴링 주기 (마이크로초 단위).
	 * 설정자: rpc_bdev_nvme_set_hotplug()가 optional로 디코딩. JSON 미지정 시 0.
	 * 읽는 자: bdev_nvme_set_hotplug()가 poller 주기로 사용.
	 * 값 범위: 0 (기본값, SPDK 내부 기본 주기 사용) 또는 양수(μs 단위 주기).
	 * 동기화: 단일 스레드 처리. */
};

/* [한국어] bdev_nvme_set_hotplug RPC 디코더 테이블.
 * "enable": bool, 필수 (optional=false).
 * "period_us": uint64, 선택 (optional=true, 생략 시 0으로 초기화된 구조체 값 유지). */
static const struct spdk_json_object_decoder rpc_bdev_nvme_set_hotplug_decoders[] = {
	{"enable", offsetof(struct rpc_bdev_nvme_hotplug, enabled), spdk_json_decode_bool, false},
	{"period_us", offsetof(struct rpc_bdev_nvme_hotplug, period_us), spdk_json_decode_uint64, true},
};

/*
 * [한국어]
 * rpc_bdev_nvme_set_hotplug - "bdev_nvme_set_hotplug" RPC: PCIe 핫플러그 감지 설정.
 *
 * @request: JSON-RPC 요청 핸들. 성공 시 bool true, 실패 시 errno 에러 응답.
 * @params: JSON 파라미터 (enable 필수, period_us 선택).
 *
 * 핫플러그 활성화 시 SPDK는 PCIe 버스를 주기적으로 스캔하여 새 NVMe SSD 장착/탈착을
 * 감지하고 자동으로 attach/detach를 수행한다. 이 기능은 PCIe NVMe 전용이며,
 * NVMe-oF 컨트롤러에 대해서는 별도의 디스커버리/페일오버 메커니즘을 사용한다.
 * period_us=0 시 SPDK 내부 기본 주기(일반적으로 100000μs = 100ms)를 사용한다.
 *
 * 동작: JSON 디코드 → bdev_nvme_set_hotplug(enabled, period_us) 호출.
 *   - 활성화: SPDK poller를 등록하여 spdk_nvme_probe()를 주기적으로 호출.
 *   - 비활성화: 해당 poller를 해제. 이미 attach된 컨트롤러는 그대로 유지.
 *
 * 실행 컨텍스트: SPDK app 스레드 (동기 실행).
 * SPDK_RPC_RUNTIME으로 등록: 런타임에만 호출 가능 (startup 단계에서는 poller 인프라 미준비).
 *
 * 호출 체인:
 *   RPC 클라이언트 → JSON-RPC 서버 → [이 함수] → bdev_nvme_set_hotplug()
 */
static void
rpc_bdev_nvme_set_hotplug(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_hotplug req = {false, 0}; /* [한국어] 기본값: 비활성화, 주기=0. */
	int rc;                                         /* [한국어] bdev_nvme_set_hotplug 반환 코드. */

	/* [한국어] JSON params 디코딩. enable은 필수이므로 JSON에 없으면 -EINVAL. */
	if (spdk_json_decode_object(params, rpc_bdev_nvme_set_hotplug_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_set_hotplug_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL; /* [한국어] 디코딩 실패 → invalid 레이블로 에러 응답. */
		goto invalid;
	}

	/* [한국어] 핫플러그 poller 등록/해제 내부 처리.
	 * 활성화: SPDK poller를 period_us 주기로 등록, 비활성화: poller 해제. */
	rc = bdev_nvme_set_hotplug(req.enabled, req.period_us);
	if (rc) {
		/* [한국어] 내부 처리 실패 (예: 이미 같은 상태, 초기화 미완료). */
		goto invalid;
	}

	/* [한국어] 성공: bool true 응답. */
	spdk_jsonrpc_send_bool_response(request, true);
	return;
invalid:
	/* [한국어] 실패: INVALID_PARAMS 에러 코드 + 에러 문자열 응답. */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
}
/* [한국어] SPDK_RPC_RUNTIME: 런타임에만 호출 가능. 핫플러그 poller는 reactor 초기화 후 사용. */
SPDK_RPC_REGISTER("bdev_nvme_set_hotplug", rpc_bdev_nvme_set_hotplug, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * enum bdev_nvme_multipath_mode - attach 시 멀티패스 모드 선택용 내부 enum.
 *
 * bdev_nvme_attach_controller RPC의 "multipath" JSON 파라미터를 표현하는 내부 enum.
 * 이를 통해 사용자가 같은 이름으로 여러 컨트롤러(path)를 attach할 때 동작을 지정한다.
 * spdk_bdev_nvme_multipath_policy (bdev_nvme.h)와는 별개 — 이 enum은 attach 시에만 쓰인다.
 */
enum bdev_nvme_multipath_mode {
	BDEV_NVME_MP_MODE_FAILOVER,
	/* [한국어] 같은 NQN 다중 trid 중 한 path만 활성(active-passive), 실패 시 다른 path로 전환.
	 * bdev_nvme_ctrlr_opts.multipath=false로 변환됨.
	 * 이 모드에서 bdev IO는 활성 path 1개로만 나가며 실패 시 다른 경로로 이동한다. */

	BDEV_NVME_MP_MODE_MULTIPATH,
	/* [한국어] 모든 path 동시 활성(active-active). selector에 따라 경로를 선택.
	 * bdev_nvme_ctrlr_opts.multipath=true로 변환됨.
	 * selector: round_robin(default) 또는 queue_depth. */

	BDEV_NVME_MP_MODE_DISABLE,
	/* [한국어] 멀티패스 비활성. 같은 이름의 컨트롤러가 이미 존재하면 attach 거부(-EALREADY).
	 * 동일 NQN 다중 접근이 필요없는 단순 PCIe single-path 환경에서 사용. */
};

/*
 * [한국어]
 * struct rpc_bdev_nvme_attach_controller - bdev_nvme_attach_controller RPC 입력 파라미터 구조체.
 *
 * JSON-RPC "bdev_nvme_attach_controller" 요청의 파라미터를 담는다.
 * spdk_json_decode_object() 호출 후 이 구조체에 파싱 결과가 채워진다.
 * 비동기 attach 완료까지 ctx(rpc_bdev_nvme_attach_controller_ctx) 안에서 유지된다.
 * 완료 콜백 후 free_rpc_bdev_nvme_attach_controller()가 heap 문자열들을 해제한다.
 */
struct rpc_bdev_nvme_attach_controller {
	char *name;
	/* [한국어] 컨트롤러 그룹 이름 (예: "Nvme0"). 생성되는 bdev는 "Nvme0n1" 형식.
	 * 설정자: spdk_json_decode_string이 strdup으로 복사.
	 * 읽는 자: trid 파싱 후 spdk_bdev_nvme_create() 첫 인자로 전달.
	 * 값 범위: 비어있지 않은 유효한 C 문자열 (NUL 포함).
	 * 동기화: 단일 스레드(RPC 처리 스레드)에서만 접근. */

	char *trtype;
	/* [한국어] NVMe 트랜스포트 종류 문자열 ("PCIe", "TCP", "RDMA", "FC").
	 * 설정자: spdk_json_decode_string().
	 * 읽는 자: spdk_nvme_transport_id_populate_trstring() + _parse_trtype()으로 trid.trtype 결정.
	 * 값 범위: NVMe-oF 스펙의 Transport Type 식별자 (대소문자 무관).
	 * 동기화: RPC 스레드 단독 접근. */

	char *adrfam;
	/* [한국어] 주소 패밀리 문자열 ("IPv4", "IPv6", "IB", "FC"). optional.
	 * 설정자: spdk_json_decode_string(). 미지정 시 NULL.
	 * 읽는 자: spdk_nvme_transport_id_parse_adrfam()으로 trid.adrfam 결정.
	 * TCP/RDMA에서 IP 버전 지정에 사용. PCIe는 불필요. */

	char *traddr;
	/* [한국어] 트랜스포트 주소 (PCIe BDF "0000:01:00.0" 또는 IP "192.168.1.1").
	 * 설정자: spdk_json_decode_string().
	 * 읽는 자: trid.traddr에 memcpy. 최대 sizeof(trid.traddr) 바이트 (256B).
	 * 값 범위: PCIe의 경우 "DDDD:BB:SS.F" 형식, NVMe-oF는 IP 문자열. */

	char *trsvcid;
	/* [한국어] 트랜스포트 서비스 ID (TCP/RDMA의 경우 포트 번호, 예: "4420"). optional.
	 * 설정자: spdk_json_decode_string(). 미지정 시 NULL.
	 * 읽는 자: trid.trsvcid에 memcpy. PCIe에서는 사용 안 함. */

	char *priority;
	/* [한국어] 연결 우선순위 (정수 문자열). optional. NVMe-oF multipath path 선택에 활용.
	 * 설정자: spdk_json_decode_string().
	 * 읽는 자: spdk_strtol로 정수 변환 후 trid.priority에 저장. */

	char *subnqn;
	/* [한국어] NVMe subsystem NQN (예: "nqn.2014-08.com.vendor:nvme.host.sys.xyz"). optional.
	 * 설정자: spdk_json_decode_string().
	 * 읽는 자: trid.subnqn에 memcpy. 최대 SPDK_NVMF_NQN_MAX_LEN 바이트.
	 * 멀티패스 시 subnqn이 다르면 에러 (같은 이름에 다른 subnqn 금지). */

	char *hostnqn;
	/* [한국어] 호스트 NQN. optional. 지정하면 drv_opts.hostnqn에 복사.
	 * NVMe-oF CONNECT 명령의 HOSTNQN 필드 (target이 호스트 식별에 사용).
	 * 미지정 시 SPDK 기본 hostnqn 사용. */

	char *hostaddr;
	/* [한국어] 호스트 측 소스 IP 주소. optional. drv_opts.src_addr에 복사.
	 * 멀티홈 호스트에서 특정 NIC를 통해 연결할 때 사용. */

	char *hostsvcid;
	/* [한국어] 호스트 측 소스 서비스 ID (포트). optional. drv_opts.src_svcid에 복사.
	 * 동일 IP에서 여러 연결을 구분할 때 사용. */

	char *psk;
	/* [한국어] NVMe-oF over TLS PSK(Pre-Shared Key) 이름. optional.
	 * 지정 시 bdev_opts.psk에 포인터 전달 (heap 소유권은 ctx가 가짐).
	 * lib/keyring에서 실제 key material 조회. TLS 지원은 실험적(experimental). */

	char *dhchap_key;
	/* [한국어] 호스트 → 컨트롤러 DH-HMAC-CHAP 인증 키 이름. optional.
	 * bdev_opts.dhchap_key에 포인터 전달. 컨트롤러 인증에 사용. */

	char *dhchap_ctrlr_key;
	/* [한국어] 컨트롤러 → 호스트 DH-HMAC-CHAP 양방향 인증 키 이름. optional.
	 * bdev_opts.dhchap_ctrlr_key에 포인터 전달. 양방향 인증(mutual auth)에 사용. */

	enum bdev_nvme_multipath_mode multipath;
	/* [한국어] 멀티패스 모드 (FAILOVER/MULTIPATH/DISABLE). optional, default=MULTIPATH.
	 * 같은 이름의 컨트롤러가 존재할 때 어떻게 처리할지 결정. */

	struct spdk_bdev_nvme_ctrlr_opts bdev_opts;
	/* [한국어] bdev_nvme 레이어 옵션 (prchk_flags, ctrlr_loss_timeout_sec 등).
	 * spdk_bdev_nvme_get_default_ctrlr_opts()로 기본값 채운 뒤 JSON 파라미터로 덮어씀. */

	struct spdk_nvme_ctrlr_opts drv_opts;
	/* [한국어] lib/nvme 드라이버 옵션 (hostnqn, num_io_queues, header_digest 등).
	 * spdk_nvme_ctrlr_get_default_ctrlr_opts()로 기본값 채운 뒤 JSON 파라미터로 덮어씀. */

	uint32_t max_bdevs;
	/* [한국어] 이번 attach에서 생성될 수 있는 최대 bdev 수.
	 * 기본값: DEFAULT_MAX_BDEVS_PER_RPC(128). names 배열 크기 결정에 사용.
	 * 0이면 에러(-EINVAL). 매우 크게 설정 시 메모리 낭비. */
};

/*
 * [한국어]
 * free_rpc_bdev_nvme_attach_controller - attach 요청 구조체 내 heap 문자열 해제.
 *
 * @req: 해제할 rpc_bdev_nvme_attach_controller 구조체 포인터.
 *       구조체 자체는 해제하지 않음 (ctx 안에 임베드되어 있으므로).
 *
 * spdk_json_decode_string()이 strdup으로 할당한 모든 문자열 필드를 free()한다.
 * NULL 포인터에 free()는 안전하므로 optional 필드들도 무조건 호출한다.
 *
 * 호출 체인: free_rpc_bdev_nvme_attach_controller_ctx() → [이 함수]
 */
static void
free_rpc_bdev_nvme_attach_controller(struct rpc_bdev_nvme_attach_controller *req)
{
	free(req->name);             /* [한국어] 컨트롤러 이름 문자열 해제. */
	free(req->trtype);           /* [한국어] 트랜스포트 타입 문자열 해제. */
	free(req->adrfam);           /* [한국어] 주소 패밀리 문자열 해제 (optional, NULL 안전). */
	free(req->traddr);           /* [한국어] 트랜스포트 주소 문자열 해제. */
	free(req->trsvcid);          /* [한국어] 서비스 ID 문자열 해제 (optional). */
	free(req->priority);         /* [한국어] 우선순위 문자열 해제 (optional). */
	free(req->subnqn);           /* [한국어] subsystem NQN 문자열 해제 (optional). */
	free(req->hostnqn);          /* [한국어] 호스트 NQN 문자열 해제 (optional). */
	free(req->hostaddr);         /* [한국어] 호스트 주소 문자열 해제 (optional). */
	free(req->hostsvcid);        /* [한국어] 호스트 서비스 ID 문자열 해제 (optional). */
	free(req->psk);              /* [한국어] TLS PSK 이름 문자열 해제 (optional). */
	free(req->dhchap_key);       /* [한국어] DH-CHAP 호스트 키 이름 해제 (optional). */
	free(req->dhchap_ctrlr_key); /* [한국어] DH-CHAP 컨트롤러 키 이름 해제 (optional). */
}

/*
 * [한국어]
 * bdev_nvme_decode_reftag - "prchk_reftag" JSON bool 값을 prchk_flags 비트에 OR-set.
 *
 * @val: JSON bool 토큰.
 * @out: uint32_t* prchk_flags (SPDK_NVME_IO_FLAGS_PRCHK_REFTAG 비트 포함 여부 결정).
 * @return: 0(성공) 또는 음수 에러코드.
 *
 * NVMe End-to-End Data Protection에서 Reference Tag(Reftag)는 LBA 번호와 연결된
 * 4바이트 태그이다 (NVMe Base Spec §8.3 Protection Information).
 * prchk_reftag=true → SPDK_NVME_IO_FLAGS_PRCHK_REFTAG 비트를 flags에 OR-set.
 * 이 비트가 세팅되면 bdev_nvme는 모든 I/O에 Reference Tag 검사를 활성화한다.
 * reftag는 LBA 하위 32비트와 비교되므로 LBA가 32비트를 넘어가는 경우 주의 필요.
 *
 * 호출 체인: spdk_json_decode_object() → [이 함수] (prchk_reftag 디코더로 등록)
 */
static int
bdev_nvme_decode_reftag(const struct spdk_json_val *val, void *out)
{
	uint32_t *flag = out; /* [한국어] prchk_flags를 가리키는 포인터. */
	bool reftag;          /* [한국어] JSON bool 값을 받을 임시 변수. */
	int rc;

	rc = spdk_json_decode_bool(val, &reftag); /* [한국어] JSON bool → C bool 변환. */
	if (rc == 0 && reftag == true) {
		/* [한국어] true인 경우에만 REFTAG 검사 비트를 OR-set (false이면 변경 없음). */
		*flag |= SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;
	}

	return rc; /* [한국어] 0: 성공, 음수: JSON 형식 오류. */
}

/*
 * [한국어]
 * bdev_nvme_decode_guard - "prchk_guard" JSON bool 값을 prchk_flags 비트에 OR-set.
 *
 * @val: JSON bool 토큰.
 * @out: uint32_t* prchk_flags (SPDK_NVME_IO_FLAGS_PRCHK_GUARD 비트 포함 여부 결정).
 * @return: 0(성공) 또는 음수 에러코드.
 *
 * NVMe E2E Data Protection의 Guard(CRC)는 데이터 무결성을 보장하는 16비트 CRC-16이다
 * (NVMe Base Spec §8.3). prchk_guard=true → SPDK_NVME_IO_FLAGS_PRCHK_GUARD 비트 set.
 * Guard 비트가 세팅되면 SPDK는 DIF/DIX Guard 필드 검증을 활성화한다.
 * reftag와 같은 디코더 패턴이지만 다른 비트를 set하는 것이 차이점이다.
 *
 * 호출 체인: spdk_json_decode_object() → [이 함수] (prchk_guard 디코더로 등록)
 */
static int
bdev_nvme_decode_guard(const struct spdk_json_val *val, void *out)
{
	uint32_t *flag = out; /* [한국어] prchk_flags 포인터. */
	bool guard;           /* [한국어] JSON bool 임시 저장. */
	int rc;

	rc = spdk_json_decode_bool(val, &guard); /* [한국어] JSON bool → C bool 변환. */
	if (rc == 0 && guard == true) {
		/* [한국어] true인 경우 GUARD 검사 비트 OR-set. */
		*flag |= SPDK_NVME_IO_FLAGS_PRCHK_GUARD;
	}

	return rc;
}

/*
 * [한국어]
 * bdev_nvme_decode_multipath - "multipath" JSON 문자열을 bdev_nvme_multipath_mode enum으로 변환.
 *
 * @val: JSON 문자열 토큰 ("failover", "multipath", "disable").
 * @out: enum bdev_nvme_multipath_mode* 포인터. 해당 enum 값으로 채워진다.
 * @return: 0(성공) 또는 -EINVAL(인식 불가 문자열).
 *
 * rpc_bdev_nvme_attach_controller_decoders 테이블의 "multipath" 필드 디코더로 등록된다.
 * 이 값은 같은 이름의 컨트롤러가 이미 존재할 때 어떻게 처리할지를 결정한다:
 *   "failover":  active-passive 모드 (bdev_opts.multipath=false).
 *   "multipath": active-active 모드 (bdev_opts.multipath=true).
 *   "disable":   멀티패스 비활성 (동일 이름 중복 attach 거부).
 *
 * 호출 체인: spdk_json_decode_object() → [이 함수]
 */
static int
bdev_nvme_decode_multipath(const struct spdk_json_val *val, void *out)
{
	enum bdev_nvme_multipath_mode *multipath = out; /* [한국어] 변환 결과를 담을 포인터. */

	if (spdk_json_strequal(val, "failover") == true) {
		/* [한국어] "failover": active-passive 모드 (하나만 활성, 실패 시 전환). */
		*multipath = BDEV_NVME_MP_MODE_FAILOVER;
	} else if (spdk_json_strequal(val, "multipath") == true) {
		/* [한국어] "multipath": active-active 모드 (모든 path 동시 활성). */
		*multipath = BDEV_NVME_MP_MODE_MULTIPATH;
	} else if (spdk_json_strequal(val, "disable") == true) {
		/* [한국어] "disable": 멀티패스 비활성 (같은 이름 중복 거부). */
		*multipath = BDEV_NVME_MP_MODE_DISABLE;
	} else {
		/* [한국어] 알 수 없는 값: 경고 로그 후 -EINVAL 반환. */
		SPDK_NOTICELOG("Invalid parameter value: multipath\n");
		return -EINVAL;
	}

	return 0; /* [한국어] 변환 성공. */
}


/* [한국어] bdev_nvme_attach_controller RPC 디코더 테이블.
 * 필수 필드(optional=false): name, trtype, traddr.
 * 선택 필드(optional=true): adrfam, trsvcid, priority, subnqn, hostnqn, hostaddr,
 *   hostsvcid, prchk_reftag, prchk_guard, hdgst, ddgst, fabrics_connect_timeout_us,
 *   multipath, num_io_queues, ctrlr_loss_timeout_sec, reconnect_delay_sec,
 *   fast_io_fail_timeout_sec, psk, max_bdevs, dhchap_key, dhchap_ctrlr_key,
 *   allow_unrecognized_csi.
 * prchk_reftag/guard: 동일한 bdev_opts.prchk_flags에 OR-set (두 디코더가 같은 필드 공유).
 * hdgst/ddgst: NVMe-oF TCP/RDMA PDU 헤더/데이터 digest (CRC32C) 활성화.
 * fabrics_connect_timeout_us: CONNECT 명령 타임아웃 (μs).
 * num_io_queues: IO queue pair 개수 (0 또는 UINT16_MAX+1 초과 시 에러).
 * allow_unrecognized_csi: 알 수 없는 Command Set Identifier를 가진 namespace도 bdev로 등록. */
static const struct spdk_json_object_decoder rpc_bdev_nvme_attach_controller_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_attach_controller, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_bdev_nvme_attach_controller, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_bdev_nvme_attach_controller, traddr), spdk_json_decode_string},

	{"adrfam", offsetof(struct rpc_bdev_nvme_attach_controller, adrfam), spdk_json_decode_string, true},
	{"trsvcid", offsetof(struct rpc_bdev_nvme_attach_controller, trsvcid), spdk_json_decode_string, true},
	{"priority", offsetof(struct rpc_bdev_nvme_attach_controller, priority), spdk_json_decode_string, true},
	{"subnqn", offsetof(struct rpc_bdev_nvme_attach_controller, subnqn), spdk_json_decode_string, true},
	{"hostnqn", offsetof(struct rpc_bdev_nvme_attach_controller, hostnqn), spdk_json_decode_string, true},
	{"hostaddr", offsetof(struct rpc_bdev_nvme_attach_controller, hostaddr), spdk_json_decode_string, true},
	{"hostsvcid", offsetof(struct rpc_bdev_nvme_attach_controller, hostsvcid), spdk_json_decode_string, true},

	{"prchk_reftag", offsetof(struct rpc_bdev_nvme_attach_controller, bdev_opts.prchk_flags), bdev_nvme_decode_reftag, true},
	{"prchk_guard", offsetof(struct rpc_bdev_nvme_attach_controller, bdev_opts.prchk_flags), bdev_nvme_decode_guard, true},
	{"hdgst", offsetof(struct rpc_bdev_nvme_attach_controller, drv_opts.header_digest), spdk_json_decode_bool, true},
	{"ddgst", offsetof(struct rpc_bdev_nvme_attach_controller, drv_opts.data_digest), spdk_json_decode_bool, true},
	{"fabrics_connect_timeout_us", offsetof(struct rpc_bdev_nvme_attach_controller, drv_opts.fabrics_connect_timeout_us), spdk_json_decode_uint64, true},
	{"multipath", offsetof(struct rpc_bdev_nvme_attach_controller, multipath), bdev_nvme_decode_multipath, true},
	{"num_io_queues", offsetof(struct rpc_bdev_nvme_attach_controller, drv_opts.num_io_queues), spdk_json_decode_uint32, true},
	{"ctrlr_loss_timeout_sec", offsetof(struct rpc_bdev_nvme_attach_controller, bdev_opts.ctrlr_loss_timeout_sec), spdk_json_decode_int32, true},
	{"reconnect_delay_sec", offsetof(struct rpc_bdev_nvme_attach_controller, bdev_opts.reconnect_delay_sec), spdk_json_decode_uint32, true},
	{"fast_io_fail_timeout_sec", offsetof(struct rpc_bdev_nvme_attach_controller, bdev_opts.fast_io_fail_timeout_sec), spdk_json_decode_uint32, true},
	{"psk", offsetof(struct rpc_bdev_nvme_attach_controller, psk), spdk_json_decode_string, true},
	{"max_bdevs", offsetof(struct rpc_bdev_nvme_attach_controller, max_bdevs), spdk_json_decode_uint32, true},
	{"dhchap_key", offsetof(struct rpc_bdev_nvme_attach_controller, dhchap_key), spdk_json_decode_string, true},
	{"dhchap_ctrlr_key", offsetof(struct rpc_bdev_nvme_attach_controller, dhchap_ctrlr_key), spdk_json_decode_string, true},
	{"allow_unrecognized_csi", offsetof(struct rpc_bdev_nvme_attach_controller, bdev_opts.allow_unrecognized_csi), spdk_json_decode_bool, true},
};

/* [한국어] bdev_nvme_attach_controller RPC 1회 호출로 생성 가능한 최대 bdev 수.
 * NVMe 컨트롤러 1개에는 최대 수십~수백 개의 namespace(=bdev)가 있을 수 있으나,
 * RPC 1회 응답에서 반환하는 이름 배열 크기를 이 값으로 제한한다. 128개는 넉넉한 기본값.
 * max_bdevs JSON 파라미터로 사용자가 재정의 가능. */
#define DEFAULT_MAX_BDEVS_PER_RPC 128

/*
 * [한국어]
 * struct rpc_bdev_nvme_attach_controller_ctx - attach RPC의 비동기 처리 컨텍스트.
 *
 * attach RPC는 비동기로 완료된다(NVMe connect → namespace populate → examine 대기).
 * 그 동안 요청 상태를 보존하기 위해 이 컨텍스트를 calloc으로 할당하고,
 * 최종 완료 콜백(rpc_bdev_nvme_attach_controller_examined)에서 free한다.
 */
struct rpc_bdev_nvme_attach_controller_ctx {
	struct rpc_bdev_nvme_attach_controller req;
	/* [한국어] JSON에서 파싱된 입력 파라미터 (trid 구성, drv_opts, bdev_opts 포함).
	 * 설정자: rpc_bdev_nvme_attach_controller()가 spdk_json_decode_object로 채움.
	 * 읽는 자: spdk_bdev_nvme_create() 호출 시 trid/이름/opts를 추출.
	 * 동기화: 비동기 완료까지 ctx가 살아 있으므로 free 전에는 접근 안전. */

	size_t bdev_count;
	/* [한국어] attach로 생성된 bdev 수. spdk_bdev_nvme_create 콜백에서 채워짐.
	 * 설정자: rpc_bdev_nvme_attach_controller_done() 콜백 (bdev_count 인자로).
	 * 읽는 자: rpc_bdev_nvme_attach_controller_examined()가 names 배열 출력 범위 결정.
	 * 값 범위: 0 ~ req.max_bdevs. */

	const char **names;
	/* [한국어] 생성된 bdev 이름 배열 포인터 (크기: req.max_bdevs).
	 * 설정자: calloc으로 배열 공간 할당. spdk_bdev_nvme_create()가 각 슬롯에 이름 채움.
	 * 읽는 자: rpc_bdev_nvme_attach_controller_examined()가 JSON 배열로 직렬화.
	 * 값 범위: 각 원소는 bdev 이름 C 문자열 포인터 (bdev 레지스트리가 소유 — free 금지).
	 * 동기화: 단일 완료 콜백에서만 읽으므로 락 불필요. */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 원래 JSON-RPC 요청 핸들. 최종 응답 전송 시 사용.
	 * 설정자: rpc_bdev_nvme_attach_controller()가 ctx->request = request로 보관.
	 * 읽는 자: rpc_bdev_nvme_attach_controller_done()과 _examined()가 응답 전송.
	 * 동기화: done 콜백 전에는 RPC 라이브러리가 유효 상태 보장. 이후 응답 후 무효화. */
};

/*
 * [한국어]
 * free_rpc_bdev_nvme_attach_controller_ctx - attach 컨텍스트 전체 해제.
 *
 * @ctx: 해제할 컨텍스트 포인터.
 *
 * req 안의 heap 문자열들을 해제하고, names 배열과 ctx 자체를 free한다.
 * rpc_bdev_nvme_attach_controller_examined() 또는 에러 경로에서 호출된다.
 *
 * 호출 체인:
 *   [에러 경로] rpc_bdev_nvme_attach_controller() → cleanup → [이 함수]
 *   [정상 경로] rpc_bdev_nvme_attach_controller_examined() → [이 함수]
 */
static void
free_rpc_bdev_nvme_attach_controller_ctx(struct rpc_bdev_nvme_attach_controller_ctx *ctx)
{
	free_rpc_bdev_nvme_attach_controller(&ctx->req); /* [한국어] req 내부 heap 문자열 해제. */
	free(ctx->names);  /* [한국어] bdev 이름 포인터 배열 해제 (각 이름 자체는 bdev 레지스트리 소유, free 안 함). */
	free(ctx);         /* [한국어] 컨텍스트 구조체 자체 해제. */
}

/*
 * [한국어]
 * rpc_bdev_nvme_attach_controller_examined - 모든 bdev examine 완료 후 RPC 응답 전송.
 *
 * @cb_ctx: rpc_bdev_nvme_attach_controller_ctx 포인터 (spdk_bdev_wait_for_examine의 cb_arg).
 *
 * spdk_bdev_wait_for_examine()이 등록된 모든 examine 모듈(예: vbdev_lvol, vbdev_gpt 등)이
 * 새 bdev들을 살펴보는 것이 완료될 때까지 기다린 뒤 이 함수를 호출한다.
 * names 배열에 채워진 생성된 bdev 이름들을 JSON 배열로 직렬화하여 RPC 응답 전송.
 *
 * 실행 컨텍스트: SPDK app 스레드 (examine 완료 시 app 스레드로 전달).
 * 이 함수 호출 이후 ctx는 해제되므로 ctx 포인터 역참조 금지.
 *
 * 호출 체인:
 *   spdk_bdev_wait_for_examine() → [이 함수] → free_rpc_bdev_nvme_attach_controller_ctx()
 */
static void
rpc_bdev_nvme_attach_controller_examined(void *cb_ctx)
{
	struct rpc_bdev_nvme_attach_controller_ctx *ctx = cb_ctx; /* [한국어] 콜백 인자 → ctx 복원. */
	struct spdk_jsonrpc_request *request = ctx->request;      /* [한국어] 응답 대상 요청 핸들. */
	struct spdk_json_write_ctx *w;                            /* [한국어] JSON 직렬화 컨텍스트. */
	size_t i;                                                 /* [한국어] names 배열 인덱스. */

	/* [한국어] JSON 응답 결과 시작. */
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w); /* [한국어] 결과는 bdev 이름 문자열 배열. */
	for (i = 0; i < ctx->bdev_count; i++) {
		/* [한국어] bdev_count 개수만큼 이름 문자열을 배열에 추가. */
		spdk_json_write_string(w, ctx->names[i]);
	}
	spdk_json_write_array_end(w);          /* [한국어] 배열 닫기. */
	spdk_jsonrpc_end_result(request, w);   /* [한국어] 직렬화된 JSON을 클라이언트에 전송. */

	free_rpc_bdev_nvme_attach_controller_ctx(ctx); /* [한국어] 모든 처리 완료 후 컨텍스트 해제. */
}

/*
 * [한국어]
 * rpc_bdev_nvme_attach_controller_done - NVMe 컨트롤러 attach + namespace populate 완료 콜백.
 *
 * @cb_ctx: rpc_bdev_nvme_attach_controller_ctx 포인터 (spdk_bdev_nvme_create에 전달한 cb_arg).
 * @bdev_count: 이번 attach로 생성된 bdev 개수. 0이면 namespace가 없거나 모두 실패.
 * @rc: 0(성공) 또는 음수 에러코드. 에러 시 bdev_count는 무의미.
 *
 * spdk_bdev_nvme_create()가 NVMe 컨트롤러 연결 + 모든 namespace를 bdev로 등록하는
 * 과정이 끝날 때 이 함수가 호출된다. rc < 0이면 에러 응답 후 ctx 해제.
 * rc == 0이면 다음 단계로 spdk_bdev_wait_for_examine()을 호출하여 lvol/gpt 등 vbdev
 * 모듈들이 새 bdev를 examine할 기회를 준다. examine 완료 후 응답 전송.
 *
 * 실행 컨텍스트: NVMe admin queue poller 스레드 (nvme_ctrlr의 admin thread).
 * 주의: ctx→request는 이 함수에서 접근하나, spdk_jsonrpc_send_error_response 후
 *       ctx를 free하므로 이후 ctx 역참조 금지.
 *
 * 호출 체인:
 *   spdk_bdev_nvme_create() → [이 함수] → spdk_bdev_wait_for_examine() → _examined()
 */
static void
rpc_bdev_nvme_attach_controller_done(void *cb_ctx, size_t bdev_count, int rc)
{
	struct rpc_bdev_nvme_attach_controller_ctx *ctx = cb_ctx; /* [한국어] 콜백 인자 → ctx 복원. */
	struct spdk_jsonrpc_request *request = ctx->request;      /* [한국어] 에러 시 응답에 쓸 요청 핸들. */

	if (rc < 0) {
		/* [한국어] attach 실패: 에러 응답 전송 후 ctx 해제. 이후 ctx 접근 불가. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		free_rpc_bdev_nvme_attach_controller_ctx(ctx);
		return;
	}

	/* [한국어] attach 성공: 생성된 bdev 수를 ctx에 보관. */
	ctx->bdev_count = bdev_count;

	/* [한국어] 다음 단계: 모든 examine 모듈(vbdev_lvol, vbdev_gpt 등)이
	 * 새 bdev들을 인식/처리할 때까지 대기한다. examine이 모두 끝나면
	 * rpc_bdev_nvme_attach_controller_examined()에서 최종 응답을 전송한다.
	 * examine 모듈들은 새 bdev 위에 다른 vbdev를 쌓을 수 있으므로
	 * 이 단계를 건너뛰면 상위 레이어가 준비되기 전에 응답이 나갈 수 있다. */
	spdk_bdev_wait_for_examine(rpc_bdev_nvme_attach_controller_examined, ctx);
}

/*
 * [한국어]
 * rpc_bdev_nvme_attach_controller - "bdev_nvme_attach_controller" RPC 진입점.
 *
 * 동작 단계 요약:
 *   1) ctx 할당 + 기본 옵션 채움 (lib/nvme + bdev_nvme).
 *   2) 입력 JSON 파싱.
 *   3) trtype/traddr/trsvcid/adrfam/subnqn/hostnqn/hostaddr/hostsvcid를 spdk_nvme_transport_id로 변환.
 *   4) 같은 이름의 컨트롤러가 이미 있으면 멀티패스/페일오버 정책에 따라 처리:
 *      - DISABLE: 거부 (-EALREADY).
 *      - FAILOVER/MULTIPATH: 같은 trid면 거부, subnqn/hostnqn 다르면 거부, 그 외 path 추가.
 *   5) spdk_bdev_nvme_create() 호출 → lib/nvme의 spdk_nvme_connect_async 시작.
 *   6) 결과는 비동기로 rpc_bdev_nvme_attach_controller_done 콜백.
 *
 * 응답: 생성된 bdev 이름들의 JSON 배열 (예: ["Nvme0n1", "Nvme0n2"]).
 * 실행 컨텍스트: SPDK app 스레드.
 */
static void
rpc_bdev_nvme_attach_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_attach_controller_ctx *ctx; /* [한국어] 비동기 attach 컨텍스트. */
	struct spdk_nvme_transport_id trid = {};          /* [한국어] 파싱 결과가 채워질 transport ID (0 초기화). */
	const struct spdk_nvme_ctrlr_opts *drv_opts;     /* [한국어] 기존 컨트롤러 drv_opts (멀티패스 검증용). */
	const struct spdk_nvme_transport_id *ctrlr_trid; /* [한국어] 기존 컨트롤러 trid (중복 경로 검증용). */
	struct nvme_ctrlr *ctrlr = NULL;                 /* [한국어] 이름이 같은 기존 컨트롤러 (멀티패스 처리). */
	size_t len, maxlen;                               /* [한국어] 문자열 길이 검증 변수. */
	int rc;                                           /* [한국어] 각 단계 반환 코드. */

	/* [한국어] 비동기 컨텍스트 calloc. 모든 필드는 0으로 초기화됨. */
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		/* [한국어] 메모리 할당 실패 시 즉시 에러 응답 (컨텍스트 없이 종료). */
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	/* [한국어] drv_opts: lib/nvme 기본 옵션 채우기 (hostnqn, num_io_queues 등).
	 * sizeof(ctx->req.drv_opts)는 ABI 호환 opts_size 인자. */
	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctx->req.drv_opts, sizeof(ctx->req.drv_opts));

	/* [한국어] bdev_opts: bdev_nvme 레이어 기본 옵션 채우기
	 * (ctrlr_loss_timeout_sec, reconnect_delay_sec 등). */
	spdk_bdev_nvme_get_default_ctrlr_opts(&ctx->req.bdev_opts);

	/* [한국어] multipath 기본값: MULTIPATH (active-active). JSON 파라미터 미지정 시 이 값 유지. */
	ctx->req.multipath = BDEV_NVME_MP_MODE_MULTIPATH;

	/* [한국어] max_bdevs 기본값: 128 (DEFAULT_MAX_BDEVS_PER_RPC). */
	ctx->req.max_bdevs = DEFAULT_MAX_BDEVS_PER_RPC;

	/* [한국어] JSON params 디코딩. 필수 필드(name/trtype/traddr) 누락 시 실패. */
	if (spdk_json_decode_object(params, rpc_bdev_nvme_attach_controller_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_attach_controller_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup; /* [한국어] ctx는 cleanup에서 해제. */
	}

	/* [한국어] max_bdevs=0 검증: names 배열 크기로 사용되므로 0이면 안 됨. */
	if (ctx->req.max_bdevs == 0) {
		spdk_jsonrpc_send_error_response(request, -EINVAL, "max_bdevs cannot be zero");
		goto cleanup;
	}

	/* [한국어] bdev 이름 포인터 배열 할당 (max_bdevs 크기).
	 * spdk_bdev_nvme_create()가 각 슬롯에 생성된 bdev 이름 포인터를 채워준다. */
	ctx->names = calloc(ctx->req.max_bdevs, sizeof(char *));
	if (ctx->names == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto cleanup;
	}

	/* [한국어] trtype 문자열을 trid.trstring(사람이 읽을 수 있는 이름)에 채움.
	 * 이 함수는 "PCIe", "TCP", "RDMA" 등을 trid.trstring(256B 고정 배열)에 복사.
	 * 반환값 음수 = 유효하지 않은 trtype 문자열. */
	rc = spdk_nvme_transport_id_populate_trstring(&trid, ctx->req.trtype);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse trtype: %s\n", ctx->req.trtype);
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse trtype: %s",
						     ctx->req.trtype);
		goto cleanup;
	}

	/* [한국어] trtype 문자열을 enum spdk_nvme_transport_type으로 변환.
	 * trstring을 이미 채웠으므로 여기서 assert(rc==0) 가능 (중복 검증이지만 타입 변환 필요). */
	rc = spdk_nvme_transport_id_parse_trtype(&trid.trtype, ctx->req.trtype);
	assert(rc == 0); /* [한국어] trstring 파싱 성공 후라면 trtype 파싱도 반드시 성공해야 함. */

	/* [한국어] traddr 길이 검증 후 trid.traddr에 복사.
	 * sizeof(trid.traddr) = 256B. NUL 포함 256B 이하여야 함.
	 * strnlen이 maxlen을 반환하면 NUL이 없거나 너무 긴 문자열. */
	maxlen = sizeof(trid.traddr);
	len = strnlen(ctx->req.traddr, maxlen);
	if (len == maxlen) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "traddr too long: %s",
						     ctx->req.traddr);
		goto cleanup;
	}
	memcpy(trid.traddr, ctx->req.traddr, len + 1); /* [한국어] NUL 포함 복사 (len+1 바이트). */

	/* [한국어] adrfam 파싱 (optional). "IPv4", "IPv6", "IB" 등 → enum. */
	if (ctx->req.adrfam) {
		rc = spdk_nvme_transport_id_parse_adrfam(&trid.adrfam, ctx->req.adrfam);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse adrfam: %s\n", ctx->req.adrfam);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse adrfam: %s",
							     ctx->req.adrfam);
			goto cleanup;
		}
	}

	/* [한국어] trsvcid 복사 (optional). TCP 포트 번호 문자열. 최대 sizeof(trid.trsvcid). */
	if (ctx->req.trsvcid) {
		maxlen = sizeof(trid.trsvcid);
		len = strnlen(ctx->req.trsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "trsvcid too long: %s",
							     ctx->req.trsvcid);
			goto cleanup;
		}
		memcpy(trid.trsvcid, ctx->req.trsvcid, len + 1); /* [한국어] NUL 포함 복사. */
	}

	/* [한국어] priority 파싱 (optional). 정수 문자열 → trid.priority (연결 우선순위). */
	if (ctx->req.priority) {
		trid.priority = spdk_strtol(ctx->req.priority, 10); /* [한국어] 10진수 정수 파싱. */
	}

	/* [한국어] subnqn 복사 (optional). NVMe Subsystem NQN. 최대 SPDK_NVMF_NQN_MAX_LEN. */
	if (ctx->req.subnqn) {
		maxlen = sizeof(trid.subnqn);
		len = strnlen(ctx->req.subnqn, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "subnqn too long: %s",
							     ctx->req.subnqn);
			goto cleanup;
		}
		memcpy(trid.subnqn, ctx->req.subnqn, len + 1); /* [한국어] NUL 포함 복사. */
	}

	/* [한국어] hostnqn 복사 (optional). drv_opts.hostnqn에 직접 memcpy.
	 * CONNECT 명령에서 호스트 식별에 사용되는 NQN. */
	if (ctx->req.hostnqn) {
		maxlen = sizeof(ctx->req.drv_opts.hostnqn);
		len = strnlen(ctx->req.hostnqn, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "hostnqn too long: %s",
							     ctx->req.hostnqn);
			goto cleanup;
		}
		memcpy(ctx->req.drv_opts.hostnqn, ctx->req.hostnqn, len + 1);
	}

	/* [한국어] PSK(TLS) 파라미터 존재 시 실험적 TLS 경고 1회만 출력.
	 * g_tls_log가 false인 경우에만 출력 후 true로 설정 (중복 출력 방지). */
	if (ctx->req.psk) {
		if (!g_tls_log) {
			SPDK_NOTICELOG("TLS support is considered experimental\n");
			g_tls_log = true; /* [한국어] 이후 호출에서 경고 반복 방지. */
		}
	}

	/* [한국어] hostaddr 복사 (optional). 멀티홈 호스트에서 특정 NIC를 선택할 때.
	 * drv_opts.src_addr에 snprintf. */
	if (ctx->req.hostaddr) {
		maxlen = sizeof(ctx->req.drv_opts.src_addr);
		len = strnlen(ctx->req.hostaddr, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "hostaddr too long: %s",
							     ctx->req.hostaddr);
			goto cleanup;
		}
		snprintf(ctx->req.drv_opts.src_addr, maxlen, "%s", ctx->req.hostaddr);
	}

	/* [한국어] hostsvcid 복사 (optional). 호스트 측 소스 포트. drv_opts.src_svcid에 snprintf. */
	if (ctx->req.hostsvcid) {
		maxlen = sizeof(ctx->req.drv_opts.src_svcid);
		len = strnlen(ctx->req.hostsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "hostsvcid too long: %s",
							     ctx->req.hostsvcid);
			goto cleanup;
		}
		snprintf(ctx->req.drv_opts.src_svcid, maxlen, "%s", ctx->req.hostsvcid);
	}

	/* [한국어] 같은 이름의 컨트롤러가 이미 존재하는지 lookup.
	 * ctrlr != NULL이면 멀티패스 경로 추가 시나리오임을 의미. */
	ctrlr = nvme_ctrlr_get_by_name(ctx->req.name);

	if (ctrlr) {
		/* This controller already exists. Check what the user wants to do. */
		if (ctx->req.multipath == BDEV_NVME_MP_MODE_DISABLE) {
			/* The user does not want to do any form of multipathing. */
			/* [한국어] DISABLE 모드: 중복 이름 attach 거부. */
			spdk_jsonrpc_send_error_response_fmt(request, -EALREADY,
							     "A controller named %s already exists and multipath is disabled",
							     ctx->req.name);
			goto cleanup;
		}

		assert(ctx->req.multipath == BDEV_NVME_MP_MODE_FAILOVER ||
		       ctx->req.multipath == BDEV_NVME_MP_MODE_MULTIPATH);
		/* [한국어] 여기까지 오면 반드시 FAILOVER 또는 MULTIPATH 모드임. */

		/* The user wants to add this as a failover path or add this to create multipath. */
		/* [한국어] 기존 컨트롤러의 drv_opts와 trid를 읽어 중복/불일치 검증. */
		drv_opts = spdk_nvme_ctrlr_get_opts(ctrlr->ctrlr);
		ctrlr_trid = spdk_nvme_ctrlr_get_transport_id(ctrlr->ctrlr);

		/* [한국어] traddr/trsvcid/src_addr/src_svcid가 모두 동일하면 완전히 같은 경로 → 거부.
		 * 다른 NIC를 통하거나 다른 포트라면 다른 경로로 인정. */
		if (strncmp(trid.traddr, ctrlr_trid->traddr, sizeof(trid.traddr)) == 0 &&
		    strncmp(trid.trsvcid, ctrlr_trid->trsvcid, sizeof(trid.trsvcid)) == 0 &&
		    strncmp(ctx->req.drv_opts.src_addr, drv_opts->src_addr, sizeof(drv_opts->src_addr)) == 0 &&
		    strncmp(ctx->req.drv_opts.src_svcid, drv_opts->src_svcid, sizeof(drv_opts->src_svcid)) == 0) {
			/* Exactly same network path can't be added a second time */
			/* [한국어] 완전히 동일한 네트워크 경로 중복 → 에러. */
			spdk_jsonrpc_send_error_response_fmt(request, -EALREADY,
							     "A controller named %s already exists with the specified network path",
							     ctx->req.name);
			goto cleanup;
		}

		/* [한국어] subnqn 일치 검증: 같은 이름의 컨트롤러는 반드시 같은 subsystem을 가리켜야 함.
		 * 다른 subnqn이면 완전히 다른 디바이스이므로 다른 이름을 써야 한다. */
		if (strncmp(trid.subnqn,
			    ctrlr_trid->subnqn,
			    SPDK_NVMF_NQN_MAX_LEN) != 0) {
			/* Different SUBNQN is not allowed when specifying the same controller name. */
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
							     "A controller named %s already exists, but uses a different subnqn (%s)",
							     ctx->req.name, ctrlr_trid->subnqn);
			goto cleanup;
		}

		/* [한국어] hostnqn 일치 검증: 호스트 NQN이 다르면 같은 그룹에 묶으면 안 됨. */
		if (strncmp(ctx->req.drv_opts.hostnqn, drv_opts->hostnqn, SPDK_NVMF_NQN_MAX_LEN) != 0) {
			/* Different HOSTNQN is not allowed when specifying the same controller name. */
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
							     "A controller named %s already exists, but uses a different hostnqn (%s)",
							     ctx->req.name, drv_opts->hostnqn);
			goto cleanup;
		}

		/* [한국어] 경로 추가 시 PI 옵션 변경 금지: prchk_flags는 첫 attach에서만 설정 가능. */
		if (ctx->req.bdev_opts.prchk_flags) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
							     "A controller named %s already exists. To add a path, do not specify PI options.",
							     ctx->req.name);
			goto cleanup;
		}

		/* [한국어] 경로 추가 시 기존 컨트롤러의 prchk_flags를 그대로 승계.
		 * 새 path도 동일한 PI 설정을 사용해야 I/O 일관성을 유지할 수 있음. */
		ctx->req.bdev_opts.prchk_flags = ctrlr->opts.prchk_flags;
	}

	/* [한국어] MULTIPATH가 아닌 모드(FAILOVER 또는 DISABLE)면 multipath=false.
	 * FAILOVER: bdev IO는 하나의 활성 path로만 나가고, 실패 시 전환하는 single-active 모델. */
	if (ctx->req.multipath != BDEV_NVME_MP_MODE_MULTIPATH) {
		ctx->req.bdev_opts.multipath = false;
	}

	/* [한국어] num_io_queues 유효 범위 검증: NVMe IO Queue Pair 수는 1 ~ UINT16_MAX+1.
	 * 0이면 qpair를 만들지 않겠다는 의미(불가), UINT16_MAX+1(65536) 초과도 불가.
	 * NVMe 스펙: QID는 16비트, 0=admin queue, 1~65535가 IO queue. */
	if (ctx->req.drv_opts.num_io_queues == 0 || ctx->req.drv_opts.num_io_queues > UINT16_MAX + 1) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
						     "num_io_queues out of bounds, min: %u max: %u",
						     1, UINT16_MAX + 1);
		goto cleanup;
	}

	ctx->request = request; /* [한국어] 비동기 완료 콜백에서 응답할 때 쓸 요청 핸들 보관. */

	/* Should already be zero due to the calloc(), but set explicitly for clarity. */
	/* [한국어] from_discovery_service=false: RPC로 직접 attach한 경우 (디스커버리 서비스가 아님).
	 * 이 플래그로 bdev_nvme는 RPC-driven attach와 discovery-driven attach를 구분한다. */
	ctx->req.bdev_opts.from_discovery_service = false;

	/* [한국어] PSK/DH-CHAP 키 이름 포인터를 bdev_opts에 연결.
	 * req의 heap 문자열 포인터를 bdev_opts에 aliasing — ctx가 살아있는 동안 유효. */
	ctx->req.bdev_opts.psk = ctx->req.psk;
	ctx->req.bdev_opts.dhchap_key = ctx->req.dhchap_key;
	ctx->req.bdev_opts.dhchap_ctrlr_key = ctx->req.dhchap_ctrlr_key;

	/* [한국어] 핵심 호출: NVMe 컨트롤러 비동기 연결 시작.
	 * trid: 어디에 연결할지 (PCIe BDF or TCP/RDMA 주소).
	 * ctx->req.name: 생성할 컨트롤러 그룹 이름.
	 * ctx->names: 생성된 bdev 이름들을 채울 배열 (크기: max_bdevs).
	 * max_bdevs: 이름 배열 크기.
	 * rpc_bdev_nvme_attach_controller_done: 완료 콜백.
	 * ctx: 콜백에 전달할 사용자 데이터.
	 * drv_opts: lib/nvme 드라이버 옵션 (hostnqn, num_io_queues 등).
	 * bdev_opts: bdev_nvme 레이어 옵션 (prchk_flags, multipath, psk 등).
	 * 0 반환: 비동기 시작 성공 (완료는 콜백에서). 음수: 즉시 실패. */
	rc = spdk_bdev_nvme_create(&trid, ctx->req.name, ctx->names, ctx->req.max_bdevs,
				   rpc_bdev_nvme_attach_controller_done, ctx, &ctx->req.drv_opts,
				   &ctx->req.bdev_opts);
	if (rc) {
		/* [한국어] 즉시 실패 (예: trid 유효성 오류, 메모리 부족): 에러 응답 후 ctx 해제. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	/* [한국어] 비동기 시작 성공: 콜백에서 응답할 것이므로 여기서는 return만. ctx는 콜백에서 해제. */
	return;

cleanup:
	/* [한국어] 에러 경로: ctx(및 내부 heap 문자열, names 배열) 전체 해제. */
	free_rpc_bdev_nvme_attach_controller_ctx(ctx);
}
/* [한국어] SPDK_RPC_RUNTIME: NVMe 스택이 초기화된 런타임 단계에만 호출 가능. */
SPDK_RPC_REGISTER("bdev_nvme_attach_controller", rpc_bdev_nvme_attach_controller,
		  SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_dump_nvme_bdev_controller_info - nvme_bdev_ctrlr 하나를 JSON 객체로 직렬화.
 *
 * @nbdev_ctrlr: 직렬화할 컨트롤러 그룹. 같은 이름으로 묶인 멀티패스 컨트롤러 집합.
 * @ctx:         spdk_json_write_ctx 포인터 (void*로 전달). 이 함수가 JSON에 직접 쓴다.
 * @return:      없음 (JSON write 실패는 write_ctx 내부에서 처리됨).
 *
 * nvme_bdev_ctrlr_for_each() 또는 단일 컨트롤러 조회 시 호출되는 방문자 함수.
 * 출력 JSON 구조:
 *   {
 *     "name": "<컨트롤러 그룹 이름>",
 *     "ctrlrs": [<nvme_ctrlr_info_json 결과>, ...]
 *   }
 * 멀티패스 구성에서 ctrlrs 배열에 경로별 컨트롤러가 여러 개 들어갈 수 있다.
 * 실행 컨텍스트: SPDK 앱 스레드 (rpc_bdev_nvme_get_controllers에서 직접 호출).
 *
 * 호출 체인:
 *   nvme_bdev_ctrlr_for_each() or direct call → [rpc_dump_nvme_bdev_controller_info]
 *                                              → nvme_ctrlr_info_json()
 */
static void
rpc_dump_nvme_bdev_controller_info(struct nvme_bdev_ctrlr *nbdev_ctrlr, void *ctx)
{
	struct spdk_json_write_ctx	*w = ctx;      /* [한국어] JSON 쓰기 컨텍스트로 캐스팅. */
	struct nvme_ctrlr		*nvme_ctrlr;       /* [한국어] 멀티패스 경로 순회 변수. */

	spdk_json_write_object_begin(w); /* [한국어] JSON 객체 시작 '{'. */
	spdk_json_write_named_string(w, "name", nbdev_ctrlr->name); /* [한국어] "name": "<그룹명>" 필드. */

	spdk_json_write_named_array_begin(w, "ctrlrs"); /* [한국어] "ctrlrs": [ 배열 시작. */
	TAILQ_FOREACH(nvme_ctrlr, &nbdev_ctrlr->ctrlrs, tailq) { /* [한국어] 동일 그룹의 모든 path 순회. */
		nvme_ctrlr_info_json(w, nvme_ctrlr); /* [한국어] 경로별 ctrlr 상세 정보(trid/opts/상태) 직렬화. */
	}
	spdk_json_write_array_end(w);  /* [한국어] ] 배열 종료. */
	spdk_json_write_object_end(w); /* [한국어] } 객체 종료. */
}

struct rpc_bdev_nvme_get_controllers {
	char *name;
	/* [한국어] 조회할 컨트롤러 그룹 이름 (optional).
	 * 설정자: spdk_json_decode_object가 JSON "name" 키 값을 heap 복사.
	 * 읽는 자: rpc_bdev_nvme_get_controllers가 nvme_bdev_ctrlr_get_by_name 인자로 전달.
	 * 값 범위: NULL이면 전체 조회, non-NULL이면 해당 이름의 컨트롤러만 조회.
	 * 동기화: 단일 앱 스레드에서만 접근, 별도 락 불필요. */
};

/*
 * [한국어]
 * free_rpc_bdev_nvme_get_controllers - req 구조체 내 heap 문자열 해제.
 *
 * @r: 해제할 rpc_bdev_nvme_get_controllers 포인터. 구조체 자체는 스택 변수이므로 free 안 함.
 * @return: 없음.
 *
 * spdk_json_decode_string이 strdup한 name 필드만 free. r 자체는 스택 변수이므로 해제하지 않음.
 * 실행 컨텍스트: SPDK 앱 스레드.
 *
 * 호출 체인:
 *   rpc_bdev_nvme_get_controllers → [free_rpc_bdev_nvme_get_controllers]
 */
static void
free_rpc_bdev_nvme_get_controllers(struct rpc_bdev_nvme_get_controllers *r)
{
	free(r->name); /* [한국어] JSON 디코더가 strdup한 문자열 해제 (NULL-safe). */
}

/* [한국어] JSON 디코더 테이블: "name" 필드는 선택(optional=true)이므로 생략 가능. */
static const struct spdk_json_object_decoder rpc_bdev_nvme_get_controllers_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_get_controllers, name), spdk_json_decode_string, true},
};

/*
 * [한국어]
 * rpc_bdev_nvme_get_controllers - "bdev_nvme_get_controllers" RPC: 등록된 컨트롤러 정보 조회.
 *
 * @request: JSON-RPC 응답 핸들.
 * @params:  JSON 파라미터 (optional). {"name": "..."} 형식이면 특정 컨트롤러만 조회.
 * @return:  없음 (결과를 request에 직접 기록).
 *
 * name 파라미터가 없으면 등록된 모든 nvme_bdev_ctrlr(그룹)를 순회하여 응답.
 * name이 있으면 해당 이름의 컨트롤러만 반환. 없으면 EINVAL.
 * 결과 JSON: [{name, ctrlrs:[...]}, ...] — ctrlrs 배열에 멀티패스 경로별 상세 정보 포함.
 * 동기 처리 (비동기 콜백 없음). 실행 컨텍스트: SPDK 앱 스레드.
 *
 * 호출 체인:
 *   JSON-RPC 프레임워크 → [rpc_bdev_nvme_get_controllers]
 *                       → nvme_bdev_ctrlr_get_by_name() or nvme_bdev_ctrlr_for_each()
 *                       → rpc_dump_nvme_bdev_controller_info()
 *                       → nvme_ctrlr_info_json()
 */
static void
rpc_bdev_nvme_get_controllers(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_get_controllers req = {};    /* [한국어] 디코드 결과를 받을 스택 구조체. */
	struct spdk_json_write_ctx *w;                     /* [한국어] JSON 응답 쓰기 컨텍스트. */
	struct nvme_bdev_ctrlr *nbdev_ctrlr = NULL;       /* [한국어] name 지정 시 조회된 컨트롤러. */

	/* [한국어] params가 있을 때만 디코딩 (name은 optional이므로 params 자체가 없을 수도 있음). */
	if (params && spdk_json_decode_object(params, rpc_bdev_nvme_get_controllers_decoders,
					      SPDK_COUNTOF(rpc_bdev_nvme_get_controllers_decoders),
					      &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] name이 지정된 경우: 해당 컨트롤러 그룹 조회. 없으면 에러. */
	if (req.name) {
		nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name(req.name); /* [한국어] 전역 리스트에서 이름 검색. */
		if (nbdev_ctrlr == NULL) {
			SPDK_ERRLOG("ctrlr '%s' does not exist\n", req.name);
			spdk_jsonrpc_send_error_response_fmt(request, EINVAL, "Controller %s does not exist", req.name);
			goto cleanup;
		}
	}

	w = spdk_jsonrpc_begin_result(request); /* [한국어] JSON 응답 직렬화 시작. */
	spdk_json_write_array_begin(w);         /* [한국어] 최상위 배열 '[' 시작. */

	/* [한국어] name 지정 시 단일 컨트롤러만, 미지정 시 전체 순회. */
	if (nbdev_ctrlr != NULL) {
		rpc_dump_nvme_bdev_controller_info(nbdev_ctrlr, w); /* [한국어] 특정 컨트롤러 직렬화. */
	} else {
		nvme_bdev_ctrlr_for_each(rpc_dump_nvme_bdev_controller_info, w); /* [한국어] 전체 순회 직렬화. */
	}

	spdk_json_write_array_end(w);           /* [한국어] 최상위 배열 ']' 종료. */

	spdk_jsonrpc_end_result(request, w);    /* [한국어] 응답 전송 완료. */

cleanup:
	free_rpc_bdev_nvme_get_controllers(&req); /* [한국어] req.name heap 문자열 해제. */
}
/* [한국어] SPDK_RPC_RUNTIME: 런타임 단계(초기화 후)에만 호출 가능한 RPC. */
SPDK_RPC_REGISTER("bdev_nvme_get_controllers", rpc_bdev_nvme_get_controllers, SPDK_RPC_RUNTIME)

struct rpc_bdev_nvme_detach_controller {
	char *name;
	/* [한국어] detach할 컨트롤러 그룹 이름 (필수).
	 * 설정자: spdk_json_decode_string이 JSON "name" 값 heap 복사.
	 * 읽는 자: rpc_bdev_nvme_detach_controller가 spdk_bdev_nvme_delete 첫 인자로 전달.
	 * 값 범위: non-NULL 필수. attach 시 지정했던 name과 동일해야 함.
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */

	char *trtype;
	/* [한국어] path 선택을 위한 transport type 문자열 (optional).
	 * 설정자: JSON "trtype" 값을 spdk_json_decode_string이 heap 복사.
	 * 읽는 자: rpc_bdev_nvme_detach_controller가 path.trid 파싱에 사용.
	 * 값 범위: NULL(미지정) 또는 "PCIe"/"TCP"/"RDMA"/"FC" 등.
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */

	char *adrfam;
	/* [한국어] path 선택을 위한 address family 문자열 (optional).
	 * 설정자: JSON "adrfam" 값을 spdk_json_decode_string이 heap 복사.
	 * 읽는 자: rpc_bdev_nvme_detach_controller가 path.trid.adrfam 파싱에 사용.
	 * 값 범위: NULL(미지정) 또는 "IPv4"/"IPv6"/"IB" 등.
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */

	char *traddr;
	/* [한국어] path 선택을 위한 transport address (optional, but required when trtype given).
	 * 설정자: JSON "traddr" 값을 spdk_json_decode_string이 heap 복사.
	 * 읽는 자: rpc_bdev_nvme_detach_controller가 path.trid.traddr에 복사.
	 * 값 범위: NULL 또는 IP 주소/PCIe BDF 문자열 (최대 255자).
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */

	char *trsvcid;
	/* [한국어] path 선택을 위한 transport service id (optional). TCP 포트 번호 문자열.
	 * 설정자: JSON "trsvcid" 값을 spdk_json_decode_string이 heap 복사.
	 * 읽는 자: path.trid.trsvcid에 복사.
	 * 값 범위: NULL 또는 포트 번호 문자열 (예: "4420").
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */

	char *subnqn;
	/* [한국어] path 선택을 위한 subsystem NQN (optional).
	 * 설정자: JSON "subnqn" 값을 spdk_json_decode_string이 heap 복사.
	 * 읽는 자: path.trid.subnqn에 복사.
	 * 값 범위: NULL 또는 NQN 문자열 (최대 SPDK_NVMF_NQN_MAX_LEN).
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */

	char *hostaddr;
	/* [한국어] path 선택을 위한 host source 주소 (optional). 멀티홈 환경에서 path 구분.
	 * 설정자: JSON "hostaddr" 값을 spdk_json_decode_string이 heap 복사.
	 * 읽는 자: path.hostid.hostaddr에 snprintf.
	 * 값 범위: NULL 또는 호스트 IP 주소 문자열.
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */

	char *hostsvcid;
	/* [한국어] path 선택을 위한 host source service id (optional).
	 * 설정자: JSON "hostsvcid" 값을 spdk_json_decode_string이 heap 복사.
	 * 읽는 자: path.hostid.hostsvcid에 snprintf.
	 * 값 범위: NULL 또는 호스트 포트 번호 문자열.
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */
};

/*
 * [한국어]
 * free_rpc_bdev_nvme_detach_controller - req 구조체 내 heap 문자열 전체 해제.
 *
 * @req: 해제할 구조체 포인터. 구조체 자체는 스택 변수이므로 free 안 함.
 * @return: 없음.
 *
 * 8개 문자열 필드를 순서대로 free. 모두 NULL-safe (free(NULL)는 no-op).
 * 실행 컨텍스트: SPDK 앱 스레드.
 *
 * 호출 체인:
 *   rpc_bdev_nvme_detach_controller → [free_rpc_bdev_nvme_detach_controller]
 */
static void
free_rpc_bdev_nvme_detach_controller(struct rpc_bdev_nvme_detach_controller *req)
{
	free(req->name);      /* [한국어] 필수 필드 name 해제. */
	free(req->trtype);    /* [한국어] optional, NULL-safe. */
	free(req->adrfam);    /* [한국어] optional, NULL-safe. */
	free(req->traddr);    /* [한국어] optional, NULL-safe. */
	free(req->trsvcid);   /* [한국어] optional, NULL-safe. */
	free(req->subnqn);    /* [한국어] optional, NULL-safe. */
	free(req->hostaddr);  /* [한국어] optional, NULL-safe. */
	free(req->hostsvcid); /* [한국어] optional, NULL-safe. */
}

/* [한국어] detach_controller RPC 디코더. name만 필수, 나머지는 path 선택용 optional.
 * trid 파라미터가 모두 NULL이면 이름 그룹 전체 detach.
 * trid 파라미터 일부 지정 시 해당 경로만 선택적 detach (멀티패스에서 단일 경로 제거). */
static const struct spdk_json_object_decoder rpc_bdev_nvme_detach_controller_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_detach_controller, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_bdev_nvme_detach_controller, trtype), spdk_json_decode_string, true},
	{"traddr", offsetof(struct rpc_bdev_nvme_detach_controller, traddr), spdk_json_decode_string, true},
	{"adrfam", offsetof(struct rpc_bdev_nvme_detach_controller, adrfam), spdk_json_decode_string, true},
	{"trsvcid", offsetof(struct rpc_bdev_nvme_detach_controller, trsvcid), spdk_json_decode_string, true},
	{"subnqn", offsetof(struct rpc_bdev_nvme_detach_controller, subnqn), spdk_json_decode_string, true},
	{"hostaddr", offsetof(struct rpc_bdev_nvme_detach_controller, hostaddr), spdk_json_decode_string, true},
	{"hostsvcid", offsetof(struct rpc_bdev_nvme_detach_controller, hostsvcid), spdk_json_decode_string, true},
};

/*
 * [한국어]
 * rpc_bdev_nvme_detach_controller_done - spdk_bdev_nvme_delete 비동기 완료 콜백.
 *
 * @arg: 원래 RPC 요청 핸들 (spdk_jsonrpc_request *).
 * @rc:  0(성공) 또는 음수 에러 코드.
 * @return: 없음.
 *
 * detach 성공 시 true 응답, 실패 시 errno 에러 응답.
 * spdk_bdev_nvme_delete는 bdev/ctrlr 정리가 완료된 후 이 콜백을 앱 스레드에서 호출한다.
 * 실행 컨텍스트: SPDK 앱 스레드 (spdk_bdev_nvme_delete 완료 시점).
 *
 * 호출 체인:
 *   spdk_bdev_nvme_delete() 완료 → [rpc_bdev_nvme_detach_controller_done]
 *                                 → spdk_jsonrpc_send_bool_response / send_error_response
 */
static void
rpc_bdev_nvme_detach_controller_done(void *arg, int rc)
{
	struct spdk_jsonrpc_request *request = arg; /* [한국어] 콜백에 전달된 RPC 요청 핸들 복원. */

	if (rc == 0) {
		/* [한국어] detach 성공: JSON 응답에 true 기록. */
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		/* [한국어] detach 실패: 에러 코드와 설명 문자열을 응답. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}

/*
 * [한국어]
 * rpc_bdev_nvme_detach_controller - "bdev_nvme_detach_controller" RPC: 컨트롤러 detach.
 *
 * @request: JSON-RPC 응답 핸들.
 * @params:  JSON 파라미터. {"name": "...", [trtype/traddr/...]} 형식.
 * @return:  없음 (비동기, 결과는 콜백에서 응답).
 *
 * 동작 모드:
 *   - name만 지정: 같은 이름 그룹의 모든 경로 detach → bdev 삭제.
 *   - name + trid 파라미터: 해당 네트워크 path만 선택적 detach (멀티패스 경로 제거).
 *
 * trid 파라미터들이 지정되면 spdk_nvme_path_id.trid에 파싱하여 spdk_bdev_nvme_delete에 전달.
 * 비동기 완료: rpc_bdev_nvme_detach_controller_done 콜백에서 응답.
 * 실행 컨텍스트: SPDK 앱 스레드.
 *
 * 호출 체인:
 *   JSON-RPC 프레임워크 → [rpc_bdev_nvme_detach_controller]
 *                       → spdk_bdev_nvme_delete()
 *                       → rpc_bdev_nvme_detach_controller_done()
 */
static void
rpc_bdev_nvme_detach_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_detach_controller req = {NULL}; /* [한국어] 스택 디코드 결과 구조체. */
	struct spdk_nvme_path_id path = {};                   /* [한국어] detach할 경로 식별자 (0 초기화). */
	size_t len, maxlen;                                    /* [한국어] 문자열 길이 검증 변수. */
	int rc = 0;                                            /* [한국어] 반환 코드. */

	/* [한국어] JSON 파라미터 디코딩. name은 필수이므로 누락 시 실패. */
	if (spdk_json_decode_object(params, rpc_bdev_nvme_detach_controller_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_detach_controller_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] trtype 지정 시 path.trid에 transport type 파싱.
	 * trstring 먼저 채우고(populate), 그 후 enum으로 변환(parse_trtype). */
	if (req.trtype != NULL) {
		rc = spdk_nvme_transport_id_populate_trstring(&path.trid, req.trtype);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse trtype: %s\n", req.trtype);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse trtype: %s",
							     req.trtype);
			goto cleanup;
		}

		rc = spdk_nvme_transport_id_parse_trtype(&path.trid.trtype, req.trtype);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse trtype: %s\n", req.trtype);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse trtype: %s",
							     req.trtype);
			goto cleanup;
		}
	}

	/* [한국어] traddr 지정 시 길이 검증 후 path.trid.traddr에 복사. */
	if (req.traddr != NULL) {
		maxlen = sizeof(path.trid.traddr);
		len = strnlen(req.traddr, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "traddr too long: %s",
							     req.traddr);
			goto cleanup;
		}
		memcpy(path.trid.traddr, req.traddr, len + 1); /* [한국어] NUL 포함 복사. */
	}

	/* [한국어] adrfam 지정 시 enum으로 파싱하여 path.trid.adrfam에 저장. */
	if (req.adrfam != NULL) {
		rc = spdk_nvme_transport_id_parse_adrfam(&path.trid.adrfam, req.adrfam);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse adrfam: %s\n", req.adrfam);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse adrfam: %s",
							     req.adrfam);
			goto cleanup;
		}
	}

	/* [한국어] trsvcid 지정 시 길이 검증 후 path.trid.trsvcid에 복사. */
	if (req.trsvcid != NULL) {
		maxlen = sizeof(path.trid.trsvcid);
		len = strnlen(req.trsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "trsvcid too long: %s",
							     req.trsvcid);
			goto cleanup;
		}
		memcpy(path.trid.trsvcid, req.trsvcid, len + 1); /* [한국어] NUL 포함 복사. */
	}

	/* Parse subnqn */
	/* [한국어] subnqn 지정 시 길이 검증 후 path.trid.subnqn에 복사. */
	if (req.subnqn != NULL) {
		maxlen = sizeof(path.trid.subnqn);
		len = strnlen(req.subnqn, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "subnqn too long: %s",
							     req.subnqn);
			goto cleanup;
		}
		memcpy(path.trid.subnqn, req.subnqn, len + 1); /* [한국어] NUL 포함 복사. */
	}

	/* [한국어] hostaddr 지정 시 길이 검증 후 path.hostid.hostaddr에 snprintf. */
	if (req.hostaddr) {
		maxlen = sizeof(path.hostid.hostaddr);
		len = strnlen(req.hostaddr, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "hostaddr too long: %s",
							     req.hostaddr);
			goto cleanup;
		}
		snprintf(path.hostid.hostaddr, maxlen, "%s", req.hostaddr);
	}

	/* [한국어] hostsvcid 지정 시 길이 검증 후 path.hostid.hostsvcid에 snprintf. */
	if (req.hostsvcid) {
		maxlen = sizeof(path.hostid.hostsvcid);
		len = strnlen(req.hostsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "hostsvcid too long: %s",
							     req.hostsvcid);
			goto cleanup;
		}
		snprintf(path.hostid.hostsvcid, maxlen, "%s", req.hostsvcid);
	}

	/* [한국어] 핵심 호출: 지정 경로(또는 전체) detach 비동기 시작.
	 * req.name: 컨트롤러 그룹 이름.
	 * &path: path 조건 (trid 전부 0이면 이름 전체 detach).
	 * rpc_bdev_nvme_detach_controller_done: 완료 콜백.
	 * request: 콜백에 전달할 RPC 요청 핸들.
	 * rc != 0이면 즉시 에러 (예: 이름 없음). */
	rc = spdk_bdev_nvme_delete(req.name, &path, rpc_bdev_nvme_detach_controller_done, request);

	if (rc != 0) {
		/* [한국어] 즉시 실패 (bdev 없음, 이름 불일치 등). 에러 응답 후 cleanup. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
	/* [한국어] rc == 0이면 비동기 진행 중. 응답은 done 콜백에서. cleanup은 항상 실행. */

cleanup:
	free_rpc_bdev_nvme_detach_controller(&req); /* [한국어] 스택 구조체 내 heap 문자열 해제. */
}
/* [한국어] SPDK_RPC_RUNTIME: 런타임 단계에만 호출 가능. */
SPDK_RPC_REGISTER("bdev_nvme_detach_controller", rpc_bdev_nvme_detach_controller,
		  SPDK_RPC_RUNTIME)

struct rpc_apply_firmware {
	char *filename;
	/* [한국어] 펌웨어 이미지 파일 경로 (필수).
	 * 설정자: spdk_json_decode_string이 JSON "filename" 값을 heap 복사.
	 * 읽는 자: rpc_bdev_nvme_apply_firmware가 fopen/stat으로 파일 로드.
	 * 값 범위: non-NULL 필수. 읽기 가능한 파일 경로여야 함.
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */

	char *bdev_name;
	/* [한국어] 펌웨어를 업데이트할 NVMe bdev 이름 (필수).
	 * 설정자: spdk_json_decode_string이 JSON "bdev_name" 값을 heap 복사.
	 * 읽는 자: rpc_bdev_nvme_apply_firmware가 spdk_bdev_get_by_name으로 bdev 조회.
	 * 값 범위: non-NULL 필수. 등록된 NVMe bdev 이름과 일치해야 함.
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */
};

/*
 * [한국어]
 * free_rpc_apply_firmware - req 구조체 내 heap 문자열 해제.
 *
 * @req: 해제할 구조체 포인터. 구조체 자체는 firmware_update_info 내부 멤버이므로 별도 free 안 함.
 * @return: 없음.
 *
 * filename, bdev_name 두 필드만 free. NULL-safe.
 * 실행 컨텍스트: apply_firmware_cleanup → SPDK 앱 스레드.
 *
 * 호출 체인:
 *   apply_firmware_cleanup → [free_rpc_apply_firmware]
 */
static void
free_rpc_apply_firmware(struct rpc_apply_firmware *req)
{
	free(req->filename);  /* [한국어] JSON 디코더가 strdup한 파일 경로 해제 (NULL-safe). */
	free(req->bdev_name); /* [한국어] JSON 디코더가 strdup한 bdev 이름 해제 (NULL-safe). */
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_apply_firmware_decoders[] = {
	{"filename", offsetof(struct rpc_apply_firmware, filename), spdk_json_decode_string},
	{"bdev_name", offsetof(struct rpc_apply_firmware, bdev_name), spdk_json_decode_string},
};

/*
 * [한국어]
 * struct firmware_update_info - apply_firmware RPC의 비동기 진행 컨텍스트.
 *
 * NVMe Firmware Image Download (Opcode 0x11, NVMe Base Spec §5.10)는 한 번에 보낼 수 있는
 * 데이터 크기에 MDTS(Maximum Data Transfer Size) 제약이 있어 4KiB 단위로 분할 발행한다.
 * 본 구조체는 분할 전송 진행 상태(offset/remaining) + DMA 버퍼 + RPC 응답 핸들을 모두 보관해
 * apply_firmware_complete → _apply_firmware_complete 비동기 콜백 체인을 잇는다.
 * 모든 청크 전송 완료 후 Firmware Commit(Opcode 0x10)을 발행하고, 성공 시 ctrlr reset.
 */
struct firmware_update_info {
	void				*fw_image;
	/* [한국어] 전체 펌웨어 이미지를 담는 DMA 가능한 hugepage 메모리 버퍼.
	 * 설정자: rpc_bdev_nvme_apply_firmware가 spdk_zmalloc(size, 0x1000, NULL, ...)으로 할당.
	 *         fread로 파일 내용 전체 로드. 4KiB 페이지 정렬 보장.
	 * 읽는 자: _apply_firmware_complete가 p를 통해 청크 단위 포인터 계산.
	 *           apply_firmware_cleanup이 spdk_free로 해제.
	 * 값 범위: non-NULL (할당 성공 시). 내용은 NVMe 펌웨어 바이너리.
	 * 동기화: 단일 앱 스레드 + DMA (디바이스 읽기 중에는 변경 금지). */

	void				*p;
	/* [한국어] 현재 전송 청크의 시작 포인터 (fw_image 내 sliding window).
	 * 설정자: rpc_bdev_nvme_apply_firmware 초기화 시 fw_image로 시작.
	 *         _apply_firmware_complete에서 p += transfer로 전진.
	 * 읽는 자: spdk_bdev_nvme_admin_passthru의 buf 인자로 전달.
	 * 값 범위: [fw_image, fw_image + size). NUL 터미네이터 없음.
	 * 동기화: DMA 전송 중에는 이 포인터의 데이터 변경 금지. */

	unsigned int			size;
	/* [한국어] 펌웨어 이미지 전체 크기 (bytes, 4의 배수여야 함).
	 * 설정자: rpc_bdev_nvme_apply_firmware가 stat() 결과로 설정.
	 * 읽는 자: spdk_zmalloc 크기 인자, size_remaining 초기값으로 사용.
	 * 값 범위: 4의 배수. NVMe 스펙: NUMD 필드는 4의 배수 크기를 요구.
	 * 동기화: 이후 읽기 전용. 멀티스레드 접근 없음. */

	unsigned int			size_remaining;
	/* [한국어] 아직 전송하지 않은 잔여 바이트. 이 값이 0이 되면 Firmware Commit 단계.
	 * 설정자: rpc_bdev_nvme_apply_firmware에서 size로 초기화.
	 *         _apply_firmware_complete에서 -= transfer로 감소.
	 * 읽는 자: _apply_firmware_complete가 switch(size_remaining)로 종료 조건 판별.
	 * 값 범위: [0, size]. 0이면 다운로드 완료, >0이면 전송 중.
	 * 동기화: 단일 앱 스레드에서만 수정 (비동기 콜백 체인 내). */

	unsigned int			offset;
	/* [한국어] 이미지 내 현재 전송 오프셋 (bytes).
	 * 설정자: rpc_bdev_nvme_apply_firmware에서 0으로 초기화.
	 *         _apply_firmware_complete에서 += transfer로 증가.
	 * 읽는 자: NVMe Firmware Image Download CDW11에 (offset >> 2)로 인코딩.
	 *           CDW11 = OFST(DWord 오프셋) → 4로 나누는 이유: NVMe는 DWord(4B) 단위.
	 * 값 범위: [0, size). 전송 완료 후 = size.
	 * 동기화: 단일 앱 스레드에서만 수정. */

	unsigned int			transfer;
	/* [한국어] 이번 청크의 전송 크기 (bytes). 보통 4096, 마지막은 size_remaining.
	 * 설정자: rpc_bdev_nvme_apply_firmware에서 첫 청크 = min(size, 4096).
	 *         _apply_firmware_complete에서 다음 청크 = min(size_remaining, 4096).
	 * 읽는 자: admin passthru의 nbytes 인자, CDW10(NUMD = NDWORD-1) 계산.
	 * 값 범위: (0, 4096]. size_remaining보다 크지 않음.
	 * 동기화: 단일 앱 스레드에서만 수정. */

	struct spdk_bdev_desc		*desc;
	/* [한국어] 열린 NVMe bdev descriptor.
	 * 설정자: rpc_bdev_nvme_apply_firmware가 spdk_bdev_open_ext로 획득.
	 * 읽는 자: spdk_bdev_nvme_admin_passthru의 첫 인자로 전달.
	 *           apply_firmware_cleanup이 spdk_bdev_close로 반납.
	 * 값 범위: non-NULL (open 성공 시).
	 * 동기화: 단일 앱 스레드에서만 사용. */

	struct spdk_io_channel		*ch;
	/* [한국어] NVMe admin passthru용 IO 채널.
	 * 설정자: rpc_bdev_nvme_apply_firmware가 spdk_bdev_get_io_channel(desc)로 획득.
	 * 읽는 자: spdk_bdev_nvme_admin_passthru의 두 번째 인자로 전달.
	 *           apply_firmware_cleanup이 spdk_put_io_channel로 반납.
	 * 값 범위: non-NULL (get 성공 시).
	 * 동기화: 이 채널을 획득한 스레드(앱 스레드)에서만 사용. */

	struct spdk_jsonrpc_request	*request;
	/* [한국어] 원래 JSON-RPC 요청 핸들.
	 * 설정자: rpc_bdev_nvme_apply_firmware에서 직접 할당.
	 * 읽는 자: apply_firmware_cleanup, apply_firmware_complete_reset이 응답 발송에 사용.
	 * 값 범위: non-NULL (firm_ctx 유효 기간 동안).
	 * 동기화: 단일 앱 스레드에서만 접근. */

	struct spdk_nvme_ctrlr		*ctrlr;
	/* [한국어] 펌웨어를 받는 lib/nvme 컨트롤러 핸들.
	 * 설정자: rpc_bdev_nvme_apply_firmware가 spdk_nvme_bdev_get_nvme_ctrlr로 획득.
	 * 읽는 자: apply_firmware_complete_reset이 spdk_nvme_ctrlr_reset에 전달.
	 *           Firmware Commit 성공 후 컨트롤러를 재시작해야 새 펌웨어가 활성화됨.
	 * 값 범위: non-NULL (firm_ctx 유효 기간 동안).
	 * 동기화: 단일 앱 스레드에서만 접근. */

	struct rpc_apply_firmware	req;
	/* [한국어] JSON 디코드 결과 (filename, bdev_name). 인라인 멤버.
	 * 설정자: rpc_bdev_nvme_apply_firmware에서 JSON 파라미터 디코딩.
	 * 읽는 자: rpc_bdev_nvme_apply_firmware 함수 내에서 파일 로드/bdev 조회에 사용.
	 *           apply_firmware_cleanup이 free_rpc_apply_firmware를 호출해 해제.
	 * 값 범위: filename/bdev_name은 non-NULL 필수 (decode 단계에서 검증됨).
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */
};

/*
 * [한국어]
 * apply_firmware_cleanup - 펌웨어 업데이트 컨텍스트 전체 정리 및 해제.
 *
 * @firm_ctx: 정리할 firmware_update_info 포인터. 이 함수 반환 후 사용 불가.
 * @return: 없음.
 *
 * 성공/실패 모든 경로에서 호출되는 공통 정리 함수.
 * 순서: DMA 버퍼 → RPC req 문자열 → IO 채널 → bdev descriptor → ctx 자체.
 * IO 채널을 채널을 닫기 전에 반납해야 bdev close가 안전하다.
 * 실행 컨텍스트: 앱 스레드 (assert로 보장).
 *
 * 호출 체인:
 *   apply_firmware_complete_reset / apply_firmware_complete / _apply_firmware_complete
 *   → [apply_firmware_cleanup]
 *   → spdk_free, free_rpc_apply_firmware, spdk_put_io_channel, spdk_bdev_close, free
 */
static void
apply_firmware_cleanup(struct firmware_update_info *firm_ctx)
{
	assert(firm_ctx != NULL);                    /* [한국어] NULL ptr 방어. */
	assert(spdk_thread_is_app_thread(NULL));     /* [한국어] 앱 스레드에서만 호출 가능 단언. */

	if (firm_ctx->fw_image) {
		/* [한국어] DMA hugepage 버퍼 해제. spdk_zmalloc으로 할당했으므로 spdk_free 필요. */
		spdk_free(firm_ctx->fw_image);
	}

	free_rpc_apply_firmware(&firm_ctx->req); /* [한국어] filename, bdev_name heap 문자열 해제. */

	if (firm_ctx->ch) {
		/* [한국어] IO 채널 반납. 참조 카운트 감소. desc close 전에 반납해야 안전. */
		spdk_put_io_channel(firm_ctx->ch);
	}

	if (firm_ctx->desc) {
		/* [한국어] bdev descriptor 닫기. 이 시점부터 bdev에 대한 접근 불가. */
		spdk_bdev_close(firm_ctx->desc);
	}

	free(firm_ctx); /* [한국어] ctx 구조체 자체 해제 (calloc 대응). */
}

/*
 * [한국어]
 * apply_firmware_complete_reset - Firmware Commit admin 명령 완료 콜백.
 *
 * @bdev_io: 완료된 admin passthru bdev_io (Firmware Commit 명령).
 * @success: true이면 Firmware Commit NVMe 명령 성공, false면 실패.
 * @cb_arg:  firmware_update_info 포인터.
 * @return:  없음.
 *
 * Firmware Commit(Opcode 0x10, NVMe Base Spec §5.9) 완료 시 호출.
 * 성공 시 spdk_nvme_ctrlr_reset()으로 컨트롤러를 재시작해야 새 펌웨어 활성화.
 * 성공/실패 모두 마지막에 apply_firmware_cleanup을 호출해 리소스 해제.
 * 실행 컨텍스트: SPDK 앱 스레드 (assert로 보장).
 *
 * 호출 체인:
 *   spdk_bdev_nvme_admin_passthru() 완료 → [apply_firmware_complete_reset]
 *   → spdk_nvme_ctrlr_reset() → apply_firmware_cleanup()
 */
static void
apply_firmware_complete_reset(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct firmware_update_info *firm_ctx = cb_arg; /* [한국어] void* → 실제 컨텍스트 포인터 복원. */
	struct spdk_json_write_ctx *w;                   /* [한국어] 성공 응답 JSON 직렬화용. */

	assert(spdk_thread_is_app_thread(NULL)); /* [한국어] 앱 스레드에서만 응답/정리 가능 단언. */

	spdk_bdev_free_io(bdev_io); /* [한국어] 완료된 bdev_io 반납 (지연 없이 즉시 해제). */

	if (!success) {
		/* [한국어] Firmware Commit 명령 실패: NVMe status 비정상 또는 타임아웃. */
		spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "firmware commit failed.");
		goto out; /* [한국어] 에러 응답 후 정리. */
	}

	/* [한국어] 컨트롤러 리셋: 새 펌웨어 이미지를 활성화하려면 필요.
	 * NVMe 스펙: Firmware Commit 후 CA(Commit Action)=1(replace & activate)이면 reset 필요.
	 * 동기 reset이므로 완료까지 블로킹 (앱 스레드에서 짧게 블록). */
	if (spdk_nvme_ctrlr_reset(firm_ctx->ctrlr) != 0) {
		spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Controller reset failed.");
		goto out; /* [한국어] reset 실패 응답 후 정리. */
	}

	/* [한국어] commit + reset 성공: 메시지 문자열 응답 (배열이나 객체가 아닌 단순 string). */
	w = spdk_jsonrpc_begin_result(firm_ctx->request);
	spdk_json_write_string(w, "firmware commit succeeded. Controller reset in progress.");
	spdk_jsonrpc_end_result(firm_ctx->request, w);
out:
	apply_firmware_cleanup(firm_ctx); /* [한국어] 성공/실패 모두 리소스 정리. */
}

/* [한국어] 전방 선언: apply_firmware_complete는 _apply_firmware_complete에서 콜백으로 등록되고,
 * _apply_firmware_complete는 apply_firmware_complete에서 호출됨. 상호 참조로 인한 전방 선언 필요. */
static void apply_firmware_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

/*
 * [한국어]
 * _apply_firmware_complete - 청크 전송 완료 후 상태 전진 및 다음 명령 발행 헬퍼.
 *
 * @firm_ctx: 현재 펌웨어 업데이트 컨텍스트. 이 함수가 상태를 전진시킨다.
 * @return:   없음.
 *
 * apply_firmware_complete 콜백에서 직접 호출되는 내부 헬퍼.
 * 책임:
 *   1) 진행 상태 업데이트: p, offset 전진, size_remaining 감소.
 *   2) size_remaining == 0이면 Firmware Commit 명령 발행.
 *   3) size_remaining > 0이면 다음 4KiB 청크 Firmware Image Download 명령 발행.
 *
 * NVMe Firmware Image Download CDW 인코딩:
 *   CDW10 = NUMD (Number of DWords - 1): spdk_nvme_bytes_to_numd(transfer).
 *   CDW11 = OFST (DWord offset): offset >> 2 (4로 나눠 DWord 오프셋).
 *
 * Firmware Commit CDW 인코딩 (NVMe §5.9):
 *   CDW10 = fw_commit 구조체: fs(Firmware Slot)=0(디바이스가 선택), ca(Commit Action)=1.
 *   commit_action = REPLACE_AND_ENABLE_IMG: 새 이미지로 교체 후 즉시 활성화.
 *
 * 실행 컨텍스트: SPDK 앱 스레드 (apply_firmware_complete 콜백 내에서 호출).
 *
 * 호출 체인:
 *   apply_firmware_complete → [_apply_firmware_complete]
 *   → spdk_bdev_nvme_admin_passthru (Commit 또는 Image Download)
 *   → apply_firmware_complete_reset (Commit 완료) or apply_firmware_complete (다음 청크)
 */
static void
_apply_firmware_complete(struct firmware_update_info *firm_ctx)
{
	struct spdk_nvme_cmd			cmd = {};   /* [한국어] admin 명령 구조체 (0 초기화). */
	struct spdk_nvme_fw_commit		fw_commit;  /* [한국어] Firmware Commit CDW10 필드 구조체. */
	int					slot = 0;   /* [한국어] 펌웨어 슬롯 0: 디바이스가 슬롯을 자동 선택. */
	int					rc;         /* [한국어] admin passthru 반환 코드. */
	enum spdk_nvme_fw_commit_action commit_action = SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG;
	/* [한국어] Commit Action: 새 이미지로 교체하고 즉시 활성화 (리셋 후 적용). */

	/* [한국어] 이전 청크 전송 완료 → 상태 전진.
	 * p: 다음 청크 시작 주소. offset: 누적 전송 바이트. size_remaining: 잔여 바이트. */
	firm_ctx->p += firm_ctx->transfer;
	firm_ctx->offset += firm_ctx->transfer;
	firm_ctx->size_remaining -= firm_ctx->transfer;

	switch (firm_ctx->size_remaining) {
	case 0:
		/* firmware download completed. Commit firmware */
		/* [한국어] 모든 청크 전송 완료. Firmware Commit 명령 발행 단계. */
		memset(&fw_commit, 0, sizeof(struct spdk_nvme_fw_commit)); /* [한국어] fw_commit 구조체 0 초기화. */
		fw_commit.fs = slot;           /* [한국어] Firmware Slot: 0 = 디바이스 자동 선택. */
		fw_commit.ca = commit_action;  /* [한국어] Commit Action: replace & enable. */

		cmd.opc = SPDK_NVME_OPC_FIRMWARE_COMMIT; /* [한국어] Admin Opcode 0x10: Firmware Commit. */
		/* [한국어] CDW10에 fw_commit 구조체를 uint32_t 단위로 복사 (4바이트).
		 * NVMe 스펙: Firmware Commit CDW10 = {CA[2:0], FS[3:0], ...}. */
		memcpy(&cmd.cdw10, &fw_commit, sizeof(uint32_t));
		/* [한국어] Firmware Commit admin passthru 발행. buf=NULL, nbytes=0 (데이터 없음). */
		rc = spdk_bdev_nvme_admin_passthru(firm_ctx->desc, firm_ctx->ch, &cmd, NULL, 0,
						   apply_firmware_complete_reset, firm_ctx);
		if (rc) {
			/* [한국어] 명령 발행 자체 실패 (채널 오류 등). 응답 후 정리. */
			spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "firmware commit failed.");
			apply_firmware_cleanup(firm_ctx);
			return;
		}
		break;
	default:
		/* [한국어] 아직 남은 데이터가 있음. 다음 청크 Firmware Image Download 발행. */
		firm_ctx->transfer = spdk_min(firm_ctx->size_remaining, 4096);
		/* [한국어] 다음 청크 크기: 남은 바이트와 4KiB 중 작은 값. */

		cmd.opc = SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD;
		/* [한국어] Admin Opcode 0x11: Firmware Image Download. */

		/* [한국어] CDW10 = NUMD (Number of DWords - 1). NVMe는 DWord(4B) 단위로 크기 표현.
		 * spdk_nvme_bytes_to_numd: bytes / 4 - 1 계산. */
		cmd.cdw10 = spdk_nvme_bytes_to_numd(firm_ctx->transfer);

		/* [한국어] CDW11 = OFST (DWord Offset). 이미지 내 위치를 DWord 단위로 표현.
		 * offset(bytes) >> 2 = offset / 4 = DWord 오프셋. */
		cmd.cdw11 = firm_ctx->offset >> 2;

		/* [한국어] 다음 청크 Firmware Image Download admin passthru 발행.
		 * buf = firm_ctx->p (DMA 버퍼 내 현재 위치 포인터). */
		rc = spdk_bdev_nvme_admin_passthru(firm_ctx->desc, firm_ctx->ch, &cmd, firm_ctx->p,
						   firm_ctx->transfer, apply_firmware_complete, firm_ctx);
		if (rc) {
			/* [한국어] 청크 명령 발행 실패. 응답 후 정리. */
			spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "firmware download failed.");
			apply_firmware_cleanup(firm_ctx);
			return;
		}
		break;
	}
}

/*
 * [한국어]
 * apply_firmware_complete - Firmware Image Download 청크 완료 콜백.
 *
 * @bdev_io: 완료된 admin passthru bdev_io (Firmware Image Download 명령).
 * @success: true이면 이번 청크 NVMe 명령 성공, false면 실패.
 * @cb_arg:  firmware_update_info 포인터.
 * @return:  없음.
 *
 * 각 4KiB 청크 다운로드가 완료될 때마다 호출되는 콜백.
 * 성공 시 _apply_firmware_complete로 상태를 전진시키고 다음 명령 발행.
 * 실패 시 에러 응답 + 정리 후 종료.
 * 실행 컨텍스트: SPDK 앱 스레드 (assert로 보장).
 *
 * 호출 체인:
 *   spdk_bdev_nvme_admin_passthru() 완료 → [apply_firmware_complete]
 *   → _apply_firmware_complete() → 다음 청크 or Commit
 */
static void
apply_firmware_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct firmware_update_info *firm_ctx = cb_arg; /* [한국어] void* → 실제 컨텍스트 포인터 복원. */

	assert(spdk_thread_is_app_thread(NULL)); /* [한국어] 앱 스레드 실행 보장 단언. */

	spdk_bdev_free_io(bdev_io); /* [한국어] 완료된 bdev_io 즉시 반납. */

	if (!success) {
		/* [한국어] 이번 청크 전송 NVMe 명령 실패. 에러 응답 후 전체 정리. */
		spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "firmware download failed .");
		apply_firmware_cleanup(firm_ctx); /* [한국어] 모든 리소스 해제. */
		return;
	}

	/* [한국어] 이번 청크 성공: 상태 전진 및 다음 명령(청크 or Commit) 발행. */
	_apply_firmware_complete(firm_ctx);
}

/*
 * [한국어]
 * apply_firmware_open_cb - spdk_bdev_open_ext용 빈 이벤트 콜백 (placeholder).
 *
 * @type:      bdev 이벤트 타입 (REMOVE, RESIZE 등).
 * @bdev:      이벤트가 발생한 bdev 포인터.
 * @event_ctx: 사용자 데이터 (여기서는 NULL로 전달됨).
 * @return:    없음.
 *
 * spdk_bdev_open_ext()는 event_cb 인자가 필수이나 (NULL 불가),
 * 이 RPC는 동기/비동기 처리 흐름에서 bdev 제거 이벤트를 별도로 처리할 필요가 없다.
 * 업데이트 완료 후 apply_firmware_cleanup에서 명시적으로 bdev를 닫으므로 이벤트 무시.
 * 실행 컨텍스트: bdev 이벤트 스레드 (하지만 실제로 아무것도 안 함).
 *
 * 호출 체인:
 *   spdk_bdev_open_ext(apply_firmware_open_cb) → (bdev 이벤트 발생 시) [apply_firmware_open_cb]
 */
static void
apply_firmware_open_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	/* [한국어] 의도적으로 비워둠 (placeholder). bdev 이벤트를 처리하지 않음. */
}

/*
 * [한국어]
 * rpc_bdev_nvme_apply_firmware - "bdev_nvme_apply_firmware" RPC: NVMe 펌웨어 이미지 업로드+커밋.
 *
 * @request: JSON-RPC 응답 핸들.
 * @params:  JSON 파라미터. {"filename": "<host 파일 경로>", "bdev_name": "<대상 bdev>"}.
 * @return:  없음 (비동기, 결과는 콜백 체인 끝에서 응답).
 *
 * 동작 단계:
 *   1) firm_ctx 비동기 컨텍스트 calloc.
 *   2) JSON 디코딩 → filename, bdev_name 획득.
 *   3) spdk_bdev_open_ext로 bdev 열기 → desc 획득.
 *   4) bdev_nvme_get_ctrlr로 lib/nvme ctrlr 핸들 획득 (reset 시 필요).
 *   5) spdk_bdev_get_io_channel로 admin passthru용 IO 채널 획득.
 *   6) open(O_RDONLY) + fstat으로 파일 크기 검증 (4의 배수여야 함).
 *   7) spdk_zmalloc(4KiB 정렬, DMA)으로 hugepage 버퍼 할당 → read로 이미지 로드.
 *   8) 첫 청크 (min(size, 4KiB)) Firmware Image Download admin passthru 발행.
 *   9) 이후 apply_firmware_complete 콜백이 _apply_firmware_complete를 통해 상태 머신 구동.
 *
 * 에러 경로는 모두 err 레이블로 jump → apply_firmware_cleanup.
 * 성공 경로: 첫 admin passthru 성공 시 return (콜백에서 이후 처리).
 *
 * NVMe CDW 인코딩:
 *   CDW10 = NUMD: spdk_nvme_bytes_to_numd(transfer) = transfer/4 - 1.
 *   CDW11 = OFST: offset >> 2 = DWord 오프셋.
 *
 * 실행 컨텍스트: SPDK 앱 스레드 (RPC 핸들러).
 *
 * 호출 체인:
 *   JSON-RPC 프레임워크 → [rpc_bdev_nvme_apply_firmware]
 *   → spdk_bdev_nvme_admin_passthru → apply_firmware_complete
 *   → _apply_firmware_complete → (반복 or Commit)
 *   → apply_firmware_complete_reset → apply_firmware_cleanup
 */
static void
rpc_bdev_nvme_apply_firmware(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	int					rc;      /* [한국어] 각 단계 반환 코드. */
	int					fd = -1; /* [한국어] 펌웨어 파일 fd. -1이면 미열림. */
	struct stat				fw_stat; /* [한국어] fstat 결과. 파일 크기 획득용. */
	struct spdk_bdev			*bdev;   /* [한국어] desc에서 얻는 bdev 포인터 (ctrlr 획득용). */
	struct spdk_nvme_cmd			cmd = {}; /* [한국어] 첫 청크 admin 명령 구조체 (0 초기화). */
	struct firmware_update_info		*firm_ctx; /* [한국어] 비동기 진행 컨텍스트. */

	/* [한국어] firm_ctx 할당. calloc → 모든 포인터 필드 0(NULL) 초기화. */
	firm_ctx = calloc(1, sizeof(struct firmware_update_info));
	if (!firm_ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error.");
		return; /* [한국어] ctx가 없으므로 cleanup 없이 직접 return. */
	}
	firm_ctx->fw_image = NULL;       /* [한국어] calloc에서 0이지만, cleanup 안전성을 위해 명시. */
	firm_ctx->request = request;     /* [한국어] 콜백 체인이 응답할 때 쓸 RPC 핸들 저장. */

	/* [한국어] JSON 파라미터 디코딩: filename, bdev_name 두 필드 추출. */
	if (spdk_json_decode_object(params, rpc_bdev_nvme_apply_firmware_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_apply_firmware_decoders), &firm_ctx->req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed.");
		goto err;
	}

	/* [한국어] bdev 열기 (write_able=true: admin 명령은 write 권한 필요).
	 * apply_firmware_open_cb: 빈 placeholder 콜백 (이벤트 무시).
	 * firm_ctx->desc: 성공 시 채워지는 bdev descriptor. */
	if (spdk_bdev_open_ext(firm_ctx->req.bdev_name, true, apply_firmware_open_cb, NULL,
			       &firm_ctx->desc) != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "bdev %s could not be opened",
						     firm_ctx->req.bdev_name);
		goto err;
	}
	bdev = spdk_bdev_desc_get_bdev(firm_ctx->desc); /* [한국어] desc → bdev 포인터 획득. */

	/* [한국어] bdev → lib/nvme 컨트롤러 핸들 획득. Firmware Commit 후 ctrlr_reset에 필요. */
	if ((firm_ctx->ctrlr = bdev_nvme_get_ctrlr(bdev)) == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Controller information for %s were not found.",
						     firm_ctx->req.bdev_name);
		goto err;
	}

	/* [한국어] admin passthru용 IO 채널 획득 (bdev 채널 참조 카운트 증가). */
	firm_ctx->ch = spdk_bdev_get_io_channel(firm_ctx->desc);
	if (!firm_ctx->ch) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "No channels were found.");
		goto err;
	}

	/* [한국어] host 파일 시스템에서 펌웨어 이미지 파일 열기 (읽기 전용). */
	fd = open(firm_ctx->req.filename, O_RDONLY);
	if (fd < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "open file failed.");
		goto err;
	}

	/* [한국어] 파일 메타데이터 획득: st_size로 펌웨어 이미지 크기 확인. */
	rc = fstat(fd, &fw_stat);
	if (rc < 0) {
		close(fd); /* [한국어] fstat 실패 시 fd 즉시 닫기. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "fstat failed.");
		goto err;
	}

	/* [한국어] 파일 크기를 firm_ctx에 저장. */
	firm_ctx->size = fw_stat.st_size;

	/* [한국어] NVMe 요구사항: 펌웨어 이미지 크기는 4의 배수(DWord 단위)여야 함. */
	if (fw_stat.st_size % 4) {
		close(fd);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Firmware image size is not multiple of 4.");
		goto err;
	}

	/* [한국어] DMA 가능한 hugepage 버퍼 할당.
	 * 크기: firm_ctx->size (전체 이미지).
	 * 정렬: 4096 (4KiB 페이지 경계). NVMe PRP는 4KiB 정렬된 물리 주소를 기대.
	 * socket: SPDK_ENV_LCORE_ID_ANY (어느 NUMA 노드든 가능).
	 * 플래그: SPDK_MALLOC_DMA (IOMMU/DPDK DMA 주소 매핑 가능). */
	firm_ctx->fw_image = spdk_zmalloc(firm_ctx->size, 4096, NULL,
					  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!firm_ctx->fw_image) {
		close(fd);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error.");
		goto err;
	}
	firm_ctx->p = firm_ctx->fw_image; /* [한국어] sliding pointer: 처음에는 버퍼 시작. */

	/* [한국어] 파일에서 전체 펌웨어 이미지를 DMA 버퍼에 한 번에 read.
	 * 반환값이 size와 다르면 읽기 실패 (EIO, truncation 등). */
	if (read(fd, firm_ctx->p, firm_ctx->size) != ((ssize_t)(firm_ctx->size))) {
		close(fd);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Read firmware image failed!");
		goto err;
	}
	close(fd); /* [한국어] 이미지 로드 완료. fd 닫기. 이후 fd는 불필요. */

	/* [한국어] 전송 상태 초기화: 처음부터 시작. */
	firm_ctx->offset = 0;                                             /* [한국어] 이미지 내 현재 위치: 0. */
	firm_ctx->size_remaining = firm_ctx->size;                        /* [한국어] 잔여 바이트: 전체 크기. */
	firm_ctx->transfer = spdk_min(firm_ctx->size_remaining, 4096);    /* [한국어] 첫 청크: min(size, 4KiB). */

	/* [한국어] 첫 청크 Firmware Image Download 명령 구성.
	 * opc = SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD (0x11, NVMe Base Spec §5.10). */
	cmd.opc = SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD;

	/* [한국어] CDW10 = NUMD (Number of DWords Minus 1).
	 * spdk_nvme_bytes_to_numd: transfer bytes / 4 - 1 = DWord 수 - 1. */
	cmd.cdw10 = spdk_nvme_bytes_to_numd(firm_ctx->transfer);

	/* [한국어] CDW11 = OFST (Offset in DWords). 처음엔 0 >> 2 = 0. */
	cmd.cdw11 = firm_ctx->offset >> 2;

	/* [한국어] 첫 청크 admin passthru 발행.
	 * 성공(rc==0): 비동기 시작. apply_firmware_complete 콜백에서 이후 진행.
	 * 실패: 즉시 에러 응답 + 정리. */
	rc = spdk_bdev_nvme_admin_passthru(firm_ctx->desc, firm_ctx->ch, &cmd, firm_ctx->p,
					   firm_ctx->transfer, apply_firmware_complete, firm_ctx);
	if (rc == 0) {
		/* normal return here. */
		/* [한국어] 비동기 시작 성공. 이후 처리는 콜백 체인에서. */
		return;
	}

	/* [한국어] admin passthru 발행 실패. 에러 응답 후 err 경로로. */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					 "Read firmware image failed!");
err:
	/* [한국어] 모든 에러 경로 공통 정리 (DMA 버퍼, IO 채널, bdev desc, ctx 순서로). */
	apply_firmware_cleanup(firm_ctx);
}
/* [한국어] SPDK_RPC_RUNTIME: 런타임 단계에만 호출 가능 (초기화 전 펌웨어 업데이트 불가). */
SPDK_RPC_REGISTER("bdev_nvme_apply_firmware", rpc_bdev_nvme_apply_firmware, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_bdev_nvme_transport_stat_ctx - bdev_nvme_get_transport_statistics RPC의 진행 컨텍스트.
 *
 * spdk_for_each_channel로 모든 nvme_poll_group을 순회하며 통계를 직렬화하는 동안
 * request와 JSON writer를 유지해야 한다.
 */
struct rpc_bdev_nvme_transport_stat_ctx {
	struct spdk_jsonrpc_request *request;
	/* [한국어] 원래 RPC 요청 핸들. 모든 채널 순회 완료 후 응답 전송 대상.
	 * 설정자: 진입점에서 ctx 할당 시 채움.
	 * 동기화: 채널 순회 메시지가 모두 같은 RPC 콜백에서 발사되므로 cross-thread 안전.
	 *         하지만 done 콜백 전에는 free 금지. */

	struct spdk_json_write_ctx *w;
	/* [한국어] JSON 결과 빌더. 각 채널 콜백이 자기 통계를 append.
	 * 설정자: 진입점이 spdk_jsonrpc_begin_result로 받음.
	 * 읽는 자: per_channel/done 모두. */
};

/*
 * [한국어]
 * rpc_bdev_nvme_rdma_stats - RDMA 트랜스포트 폴 그룹의 디바이스별 통계를 JSON으로 직렬화.
 *
 * RDMA의 경우 한 폴 그룹이 여러 ibv_device를 사용할 수 있으므로 devices 배열로 출력한다.
 * 각 device: polls/idle_polls/completions(CQ 폴링 통계), send/recv WR(work request),
 * doorbell update 횟수.
 */
static void
rpc_bdev_nvme_rdma_stats(struct spdk_json_write_ctx *w,
			 struct spdk_nvme_transport_poll_group_stat *stat)
{
	struct spdk_nvme_rdma_device_stat *device_stats; /* [한국어] 순회 중인 ibv_device 통계 포인터. */
	uint32_t i; /* [한국어] 디바이스 인덱스. */

	spdk_json_write_named_array_begin(w, "devices"); /* [한국어] "devices": [ 배열 시작. */

	/* [한국어] RDMA 폴 그룹에 속한 모든 ibv_device 순회. */
	for (i = 0; i < stat->rdma.num_devices; i++) {
		device_stats = &stat->rdma.device_stats[i]; /* [한국어] i번째 디바이스 통계 포인터. */
		spdk_json_write_object_begin(w);             /* [한국어] 디바이스 JSON 객체 '{' 시작. */
		spdk_json_write_named_string(w, "dev_name", device_stats->name);           /* [한국어] ibv 디바이스 이름. */
		spdk_json_write_named_uint64(w, "polls", device_stats->polls);             /* [한국어] CQ 폴링 총 횟수. */
		spdk_json_write_named_uint64(w, "idle_polls", device_stats->idle_polls);   /* [한국어] 완료 없는 폴링 횟수. */
		spdk_json_write_named_uint64(w, "completions", device_stats->completions); /* [한국어] 처리한 CQE 수. */
		spdk_json_write_named_uint64(w, "queued_requests", device_stats->queued_requests); /* [한국어] 큐 대기 요청 수. */
		spdk_json_write_named_uint64(w, "total_send_wrs", device_stats->total_send_wrs);   /* [한국어] 총 Send WR 수. */
		spdk_json_write_named_uint64(w, "send_doorbell_updates", device_stats->send_doorbell_updates); /* [한국어] SQ doorbell update 횟수. */
		spdk_json_write_named_uint64(w, "total_recv_wrs", device_stats->total_recv_wrs);   /* [한국어] 총 Recv WR 수. */
		spdk_json_write_named_uint64(w, "recv_doorbell_updates", device_stats->recv_doorbell_updates); /* [한국어] RQ doorbell update 횟수. */
		spdk_json_write_object_end(w); /* [한국어] 디바이스 JSON 객체 '}' 종료. */
	}
	spdk_json_write_array_end(w); /* [한국어] "devices" 배열 ] 종료. */
}

/*
 * [한국어]
 * rpc_bdev_nvme_pcie_stats - PCIe 트랜스포트 폴 그룹 통계 직렬화.
 *
 * PCIe NVMe는 SQ/CQ doorbell을 MMIO 또는 shadow doorbell(NVMe 1.3+)로 갱신할 수 있다.
 * mmio_doorbell_updates: BAR MMIO write 횟수 (인터럽트/직접 쓰기).
 * shadow_doorbell_updates: shadow doorbell write 횟수 (호스트 메모리에 쓰고 디바이스가 readback).
 * shadow doorbell은 MMIO보다 빠르므로 두 카운터의 비율로 효율성을 가늠.
 */
static void
rpc_bdev_nvme_pcie_stats(struct spdk_json_write_ctx *w,
			 struct spdk_nvme_transport_poll_group_stat *stat)
{
	spdk_json_write_named_uint64(w, "polls", stat->pcie.polls);                   /* [한국어] CQ 폴링 총 횟수. */
	spdk_json_write_named_uint64(w, "idle_polls", stat->pcie.idle_polls);         /* [한국어] 완료 없는 폴링 횟수. */
	spdk_json_write_named_uint64(w, "completions", stat->pcie.completions);       /* [한국어] 처리한 CQE 수. */
	/* [한국어] CQ MMIO doorbell: 직접 MMIO write로 CQ head를 컨트롤러에 알린 횟수. */
	spdk_json_write_named_uint64(w, "cq_mmio_doorbell_updates", stat->pcie.cq_mmio_doorbell_updates);
	/* [한국어] CQ shadow doorbell: host memory 기반 shadow register 갱신 횟수 (NVMe 1.3+). */
	spdk_json_write_named_uint64(w, "cq_shadow_doorbell_updates",
				     stat->pcie.cq_shadow_doorbell_updates);
	spdk_json_write_named_uint64(w, "queued_requests", stat->pcie.queued_requests);     /* [한국어] 큐 대기 요청 수. */
	spdk_json_write_named_uint64(w, "submitted_requests", stat->pcie.submitted_requests); /* [한국어] SQ에 제출한 요청 수. */
	/* [한국어] SQ MMIO doorbell: SQ tail doorbell을 MMIO write로 갱신한 횟수. */
	spdk_json_write_named_uint64(w, "sq_mmio_doorbell_updates", stat->pcie.sq_mmio_doorbell_updates);
	/* [한국어] SQ shadow doorbell: shadow register 기반 SQ tail 갱신 횟수. */
	spdk_json_write_named_uint64(w, "sq_shadow_doorbell_updates",
				     stat->pcie.sq_shadow_doorbell_updates);
}

/*
 * [한국어]
 * rpc_bdev_nvme_tcp_stats - TCP 트랜스포트 폴 그룹 통계 직렬화.
 *
 * NVMe-TCP는 PDU 단위로 wire를 흐르며, polls/idle_polls는 sock poll 횟수,
 * socket_completions는 PDU 도착 수, nvme_completions는 CQE 디큐 수.
 */
static void
rpc_bdev_nvme_tcp_stats(struct spdk_json_write_ctx *w,
			struct spdk_nvme_transport_poll_group_stat *stat)
{
	spdk_json_write_named_uint64(w, "polls", stat->tcp.polls);               /* [한국어] sock 폴링 총 횟수. */
	spdk_json_write_named_uint64(w, "idle_polls", stat->tcp.idle_polls);     /* [한국어] 수신 데이터 없는 폴링 횟수. */
	/* [한국어] socket_completions: TCP socket에서 수신한 PDU(NVMe-TCP Packet Data Unit) 수. */
	spdk_json_write_named_uint64(w, "socket_completions", stat->tcp.socket_completions);
	/* [한국어] nvme_completions: PDU 중 실제 CQE가 담긴 NVMe Response PDU 수. */
	spdk_json_write_named_uint64(w, "nvme_completions", stat->tcp.nvme_completions);
	spdk_json_write_named_uint64(w, "queued_requests", stat->tcp.queued_requests);     /* [한국어] 큐 대기 요청 수. */
	spdk_json_write_named_uint64(w, "submitted_requests", stat->tcp.submitted_requests); /* [한국어] 전송된 요청 수. */
}

/*
 * [한국어]
 * rpc_bdev_nvme_stats_per_channel - 각 nvme_poll_group 채널에서 호출되는 통계 수집 콜백.
 *
 * spdk_for_each_channel이 모든 SPDK 스레드의 채널을 순회하면서 본 함수를 호출한다.
 * 각 호출은 해당 채널이 속한 스레드 컨텍스트에서 실행되므로, 이 함수는 그 스레드의
 * nvme_poll_group을 lock-free로 읽을 수 있다. 통계를 JSON에 추가한 뒤
 * spdk_for_each_channel_continue로 다음 채널로 진행 신호.
 */
static void
rpc_bdev_nvme_stats_per_channel(struct spdk_io_channel_iter *i)
{
	struct rpc_bdev_nvme_transport_stat_ctx *ctx; /* [한국어] 순회 컨텍스트 (request + json writer). */
	struct spdk_io_channel *ch;                    /* [한국어] 현재 채널 포인터. */
	struct nvme_poll_group *group;                 /* [한국어] 채널 내 nvme poll group (채널 ctx). */
	struct spdk_nvme_poll_group_stat *stat;        /* [한국어] 전체 poll group 통계 구조체. */
	struct spdk_nvme_transport_poll_group_stat *tr_stat; /* [한국어] transport별 통계. */
	uint32_t j;                                    /* [한국어] transport 인덱스. */
	int rc;                                        /* [한국어] get_stats 반환 코드. */

	/* [한국어] 이터레이터에서 컨텍스트, 채널, poll group 추출. */
	ctx = spdk_io_channel_iter_get_ctx(i);     /* [한국어] 진입점이 할당한 ctx 복원. */
	ch = spdk_io_channel_iter_get_channel(i);  /* [한국어] 현재 순회 중인 채널. */
	group = spdk_io_channel_get_ctx(ch);       /* [한국어] 채널 context = nvme_poll_group. */

	/* [한국어] poll group 통계 획득. 실패 시 에러 코드로 다음 채널 진행. */
	rc = spdk_nvme_poll_group_get_stats(group->group, &stat);
	if (rc) {
		spdk_for_each_channel_continue(i, rc); /* [한국어] 에러 코드와 함께 다음 채널로. */
		return;
	}

	spdk_json_write_object_begin(ctx->w); /* [한국어] poll group JSON 객체 '{' 시작. */
	/* [한국어] 현재 스레드 이름 기록 (어느 SPDK 스레드의 poll group인지 식별). */
	spdk_json_write_named_string(ctx->w, "thread", spdk_thread_get_name(spdk_get_thread()));
	spdk_json_write_named_array_begin(ctx->w, "transports"); /* [한국어] "transports": [ 배열 시작. */

	/* [한국어] transport 종류별 통계 순회 (PCIe/TCP/RDMA 등이 각각 있을 수 있음). */
	for (j = 0; j < stat->num_transports; j++) {
		tr_stat = stat->transport_stat[j]; /* [한국어] j번째 transport 통계. */
		spdk_json_write_object_begin(ctx->w); /* [한국어] transport JSON 객체 '{' 시작. */
		/* [한국어] transport 종류 이름 기록 ("RDMA"/"PCIe"/"TCP" 등). */
		spdk_json_write_named_string(ctx->w, "trname", spdk_nvme_transport_id_trtype_str(tr_stat->trtype));

		/* [한국어] transport 종류에 따라 적합한 통계 직렬화 함수 호출. */
		switch (stat->transport_stat[j]->trtype) {
		case SPDK_NVME_TRANSPORT_RDMA:
			rpc_bdev_nvme_rdma_stats(ctx->w, tr_stat); /* [한국어] RDMA: 디바이스별 통계. */
			break;
		case SPDK_NVME_TRANSPORT_PCIE:
		case SPDK_NVME_TRANSPORT_VFIOUSER: /* [한국어] VFIO-user는 PCIe 통계 구조 공유. */
			rpc_bdev_nvme_pcie_stats(ctx->w, tr_stat); /* [한국어] PCIe: doorbell 통계. */
			break;
		case SPDK_NVME_TRANSPORT_TCP:
			rpc_bdev_nvme_tcp_stats(ctx->w, tr_stat); /* [한국어] TCP: PDU/sock 통계. */
			break;
		default:
			/* [한국어] FC 등 미지원 transport: 경고만 출력. */
			SPDK_WARNLOG("Can't handle trtype %d %s\n", tr_stat->trtype,
				     spdk_nvme_transport_id_trtype_str(tr_stat->trtype));
		}
		spdk_json_write_object_end(ctx->w); /* [한국어] transport JSON 객체 '}' 종료. */
	}
	/* transports array */
	spdk_json_write_array_end(ctx->w);  /* [한국어] "transports" 배열 ] 종료. */
	spdk_json_write_object_end(ctx->w); /* [한국어] poll group JSON 객체 '}' 종료. */

	spdk_nvme_poll_group_free_stats(group->group, stat); /* [한국어] stat 메모리 반납. */
	spdk_for_each_channel_continue(i, 0); /* [한국어] 다음 채널로 계속 진행 (0 = 에러 없음). */
}

/*
 * [한국어]
 * rpc_bdev_nvme_stats_done - 모든 채널 순회 완료 후 호출되는 마무리 콜백.
 *
 * JSON 배열 닫기 → RPC 응답 종료 → 컨텍스트 free. 이 시점에 ctx->request는 종료된다.
 * 실행 컨텍스트: 마지막 채널 스레드 (SPDK 코어가 done 콜백을 호출하는 스레드).
 */
static void
rpc_bdev_nvme_stats_done(struct spdk_io_channel_iter *i, int status)
{
	/* [한국어] 이터레이터에서 ctx 복원 (이 시점에 모든 채널 순회 완료). */
	struct rpc_bdev_nvme_transport_stat_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	spdk_json_write_array_end(ctx->w);   /* [한국어] "poll_groups" 배열 ] 종료. */
	spdk_json_write_object_end(ctx->w);  /* [한국어] 최상위 JSON 객체 '}' 종료. */
	spdk_jsonrpc_end_result(ctx->request, ctx->w); /* [한국어] RPC 응답 완료 전송. */
	free(ctx); /* [한국어] ctx 구조체 해제 (채널 순회 완료 후 불필요). */
}

/*
 * [한국어]
 * rpc_bdev_nvme_get_transport_statistics - "bdev_nvme_get_transport_statistics" RPC 진입점.
 *
 * 입력 없음 (params는 NULL이어야 함).
 * 출력: {"poll_groups": [{"thread": "...", "transports": [{"trname": ..., 통계...}, ...]}]}.
 *
 * 동작: spdk_for_each_channel로 g_nvme_bdev_ctrlrs(=io device 키)에 등록된 모든
 * nvme_poll_group 채널을 순회하며 transport별 통계를 모은다.
 * 실행 컨텍스트: SPDK app 스레드 시작, 채널 콜백은 각 스레드에서.
 */
static void
rpc_bdev_nvme_get_transport_statistics(struct spdk_jsonrpc_request *request,
				       const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_transport_stat_ctx *ctx;

	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'bdev_nvme_get_transport_statistics' requires no arguments");
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error");
		return;
	}
	ctx->request = request;
	ctx->w = spdk_jsonrpc_begin_result(ctx->request);
	spdk_json_write_object_begin(ctx->w);
	spdk_json_write_named_array_begin(ctx->w, "poll_groups");

	spdk_for_each_channel(&g_nvme_bdev_ctrlrs,
			      rpc_bdev_nvme_stats_per_channel,
			      ctx,
			      rpc_bdev_nvme_stats_done);
}
SPDK_RPC_REGISTER("bdev_nvme_get_transport_statistics", rpc_bdev_nvme_get_transport_statistics,
		  SPDK_RPC_RUNTIME)

struct rpc_bdev_nvme_controller_op_req {
	char *name;
	/* [한국어] 대상 컨트롤러 그룹 이름 (필수).
	 * 설정자: spdk_json_decode_string이 JSON "name" 값을 heap 복사.
	 * 읽는 자: rpc_bdev_nvme_controller_op가 nvme_bdev_ctrlr_get_by_name 인자로 전달.
	 * 값 범위: non-NULL 필수. attach 시 지정한 이름과 동일해야 함.
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */

	uint16_t cntlid;
	/* [한국어] 대상 컨트롤러 ID (optional, 0이면 그룹 전체 대상).
	 * 설정자: spdk_json_decode_uint16이 JSON "cntlid" 값을 파싱.
	 * 읽는 자: rpc_bdev_nvme_controller_op가 cntlid==0 여부로 단일 vs 그룹 분기.
	 * 값 범위: 0(전체 그룹) 또는 NVMe Controller ID (1~0xFFFF).
	 *           calloc에 의해 0으로 초기화되므로 JSON 미지정 시 전체 그룹 동작.
	 * 동기화: 단일 앱 스레드, 별도 락 불필요. */
};

/*
 * [한국어]
 * free_rpc_bdev_nvme_controller_op_req - req 내 heap 문자열 해제.
 *
 * @r: 해제할 구조체 포인터. 구조체 자체는 스택 변수이므로 free 안 함.
 * @return: 없음.
 *
 * 실행 컨텍스트: SPDK 앱 스레드.
 */
static void
free_rpc_bdev_nvme_controller_op_req(struct rpc_bdev_nvme_controller_op_req *r)
{
	free(r->name); /* [한국어] JSON 디코더가 strdup한 name 해제 (NULL-safe). */
}

/* [한국어] controller op RPC 공통 디코더.
 * name: 필수. cntlid: optional (0이면 그룹 전체 처리). */
static const struct spdk_json_object_decoder rpc_bdev_nvme_controller_op_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_controller_op_req, name), spdk_json_decode_string},
	{"cntlid", offsetof(struct rpc_bdev_nvme_controller_op_req, cntlid), spdk_json_decode_uint16, true},
};

/*
 * [한국어]
 * rpc_bdev_nvme_controller_op_cb - reset/enable/disable 비동기 완료 콜백.
 *
 * nvme_ctrlr_op_rpc 또는 nvme_bdev_ctrlr_op_rpc가 호출한 후, 컨트롤러 op가 완료되면
 * 이 콜백이 실행되어 bool/error 응답 전송. 실행 스레드는 nvme_ctrlr의 admin polling 스레드.
 */
static void
rpc_bdev_nvme_controller_op_cb(void *cb_arg, int rc)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (rc == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}

/*
 * [한국어]
 * rpc_bdev_nvme_controller_op - reset/enable/disable RPC의 공통 본체.
 *
 * @op: NVME_CTRLR_OP_RESET / _ENABLE / _DISABLE 중 하나.
 *
 * cntlid가 0(또는 미지정)이면 그룹의 모든 컨트롤러에 대해 op를 실행 (멀티패스 일괄).
 * cntlid가 지정되면 해당 cntlid 1개 컨트롤러만.
 */
static void
rpc_bdev_nvme_controller_op(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params,
			    enum nvme_ctrlr_op op)
{
	struct rpc_bdev_nvme_controller_op_req req = {NULL};
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;

	if (spdk_json_decode_object(params, rpc_bdev_nvme_controller_op_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_controller_op_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(EINVAL));
		goto exit;
	}

	/* [한국어] 그룹 lookup. */
	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name(req.name);
	if (nbdev_ctrlr == NULL) {
		SPDK_ERRLOG("Failed at NVMe bdev controller lookup\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto exit;
	}

	if (req.cntlid == 0) {
		/* [한국어] 그룹 전체에 op 실행. */
		nvme_bdev_ctrlr_op_rpc(nbdev_ctrlr, op, rpc_bdev_nvme_controller_op_cb, request);
	} else {
		/* [한국어] cntlid 지정 → 단일 컨트롤러에만. */
		nvme_ctrlr = nvme_bdev_ctrlr_get_ctrlr_by_id(nbdev_ctrlr, req.cntlid);
		if (nvme_ctrlr == NULL) {
			SPDK_ERRLOG("Failed at NVMe controller lookup\n");
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
			goto exit;
		}
		nvme_ctrlr_op_rpc(nvme_ctrlr, op, rpc_bdev_nvme_controller_op_cb, request);
	}

exit:
	free_rpc_bdev_nvme_controller_op_req(&req);
}

/*
 * [한국어]
 * rpc_bdev_nvme_reset_controller - "bdev_nvme_reset_controller" RPC.
 * controller_op 본체에 NVME_CTRLR_OP_RESET 위임. NVMe CC.EN을 0→1 시퀀스로 reset.
 */
static void
rpc_bdev_nvme_reset_controller(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	rpc_bdev_nvme_controller_op(request, params, NVME_CTRLR_OP_RESET);
}
SPDK_RPC_REGISTER("bdev_nvme_reset_controller", rpc_bdev_nvme_reset_controller, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_bdev_nvme_enable_controller - disabled 상태에서 다시 활성화.
 * 동작 자체는 reset과 비슷하지만 disabled 플래그를 해제하는 의미가 더해진다.
 */
static void
rpc_bdev_nvme_enable_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	rpc_bdev_nvme_controller_op(request, params, NVME_CTRLR_OP_ENABLE);
}
SPDK_RPC_REGISTER("bdev_nvme_enable_controller", rpc_bdev_nvme_enable_controller, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_bdev_nvme_disable_controller - 새 IO 차단 + qpair 비활성화.
 * 컨트롤러 객체는 유지하지만 사용자 IO는 즉시 거부. multipath 우회 테스트/유지보수용.
 */
static void
rpc_bdev_nvme_disable_controller(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	rpc_bdev_nvme_controller_op(request, params, NVME_CTRLR_OP_DISABLE);
}
SPDK_RPC_REGISTER("bdev_nvme_disable_controller", rpc_bdev_nvme_disable_controller,
		  SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_get_controller_health_info - 입력 JSON 파라미터.
 * 단일 필드 name (어느 컨트롤러의 health log를 가져올지). NVMe Get Log Page 0x02 발급.
 */
struct rpc_get_controller_health_info {
	char *name;
	/* [한국어] 대상 컨트롤러 이름 (멀티패스 그룹 이름 또는 단일 컨트롤러 이름).
	 * 디코더가 strdup, 콜백 종료 후 free. */
};

/*
 * [한국어]
 * struct spdk_nvme_health_info_context - get_controller_health_info의 비동기 컨텍스트.
 *
 * Get Features (Temperature Threshold) → Get Log Page (Health) 2단계 admin 발행을
 * 거치는 동안 살아있어야 한다. health_page 버퍼는 디바이스 DMA의 대상이 되므로
 * 컨텍스트 lifetime이 명령 완료까지 유지되어야 안전.
 */
struct spdk_nvme_health_info_context {
	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답을 보낼 원래 RPC 요청. */
	struct spdk_nvme_ctrlr *ctrlr;
	/* [한국어] lib/nvme 컨트롤러 핸들. admin 명령 발행 대상. */
	struct spdk_nvme_health_information_page health_page;
	/* [한국어] NVMe Get Log Page 0x02 응답이 채워질 메모리. 512바이트 (NVMe 스펙 정의).
	 * Get Log Page는 PRP로 이 버퍼에 DMA 한다 (CMB 미사용 시 일반 호스트 메모리). */
};

/*
 * [한국어]
 * free_rpc_get_controller_health_info - 디코더가 strdup한 name 문자열 해제.
 */
static void
free_rpc_get_controller_health_info(struct rpc_get_controller_health_info *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_get_controller_health_info_decoders[] = {
	{"name", offsetof(struct rpc_get_controller_health_info, name), spdk_json_decode_string, true},
};

/*
 * [한국어]
 * nvme_health_info_cleanup - 컨텍스트 정리.
 * @response=true 이면 사용자에게 에러 응답을 먼저 보낸 뒤 free. 성공 경로에서는 false.
 */
static void
nvme_health_info_cleanup(struct spdk_nvme_health_info_context *context, bool response)
{
	if (response == true) {
		spdk_jsonrpc_send_error_response(context->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error.");
	}

	free(context);
}

/*
 * [한국어]
 * get_health_log_page_completion - NVMe Get Log Page 0x02 (Health Info) 완료 콜백.
 *
 * @cpl: NVMe CQE. SCT/SC가 0이 아니면 에러. status_field.sc/sct로 확인.
 *
 * health_page에 채워진 SMART/Health 정보(데이터 단위, 호스트 read/write, 전원 ON 시간,
 * NAND 마모도, 온도, 임시 센서값 8개 등)를 JSON으로 직렬화해 RPC 응답으로 전송.
 * NVMe 1.x 스펙의 Get Log Page Identifier=02h 참조 (NVMe Base Spec §5.16).
 * 실행 컨텍스트: 컨트롤러 admin queue poller가 호출 → app 스레드 (둘 다 같은 스레드).
 */
static void
get_health_log_page_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	int i;
	char buf[128];
	struct spdk_nvme_health_info_context *context = cb_arg;
	struct spdk_jsonrpc_request *request = context->request;
	struct spdk_json_write_ctx *w;
	struct spdk_nvme_ctrlr *ctrlr = context->ctrlr;
	const struct spdk_nvme_transport_id *trid = NULL;
	const struct spdk_nvme_ctrlr_data *cdata = NULL;
	struct spdk_nvme_health_information_page *health_page = NULL;

	if (spdk_nvme_cpl_is_error(cpl)) {
		nvme_health_info_cleanup(context, true);
		SPDK_ERRLOG("get log page failed\n");
		return;
	}

	if (ctrlr == NULL) {
		nvme_health_info_cleanup(context, true);
		SPDK_ERRLOG("ctrlr is NULL\n");
		return;
	} else {
		trid = spdk_nvme_ctrlr_get_transport_id(ctrlr);
		cdata = spdk_nvme_ctrlr_get_data(ctrlr);
		health_page = &(context->health_page);
	}

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);
	snprintf(buf, sizeof(cdata->mn) + 1, "%s", cdata->mn);
	spdk_str_trim(buf);
	spdk_json_write_named_string(w, "model_number", buf);
	snprintf(buf, sizeof(cdata->sn) + 1, "%s", cdata->sn);
	spdk_str_trim(buf);
	spdk_json_write_named_string(w, "serial_number", buf);
	snprintf(buf, sizeof(cdata->fr) + 1, "%s", cdata->fr);
	spdk_str_trim(buf);
	spdk_json_write_named_string(w, "firmware_revision", buf);
	spdk_json_write_named_string(w, "traddr", trid->traddr);
	spdk_json_write_named_uint64(w, "critical_warning", health_page->critical_warning.raw);
	spdk_json_write_named_uint64(w, "temperature_celsius", health_page->temperature - 273);
	spdk_json_write_named_uint64(w, "available_spare_percentage", health_page->available_spare);
	spdk_json_write_named_uint64(w, "available_spare_threshold_percentage",
				     health_page->available_spare_threshold);
	spdk_json_write_named_uint64(w, "percentage_used", health_page->percentage_used);
	spdk_json_write_named_uint128(w, "data_units_read",
				      health_page->data_units_read[0], health_page->data_units_read[1]);
	spdk_json_write_named_uint128(w, "data_units_written",
				      health_page->data_units_written[0], health_page->data_units_written[1]);
	spdk_json_write_named_uint128(w, "host_read_commands",
				      health_page->host_read_commands[0], health_page->host_read_commands[1]);
	spdk_json_write_named_uint128(w, "host_write_commands",
				      health_page->host_write_commands[0], health_page->host_write_commands[1]);
	spdk_json_write_named_uint128(w, "controller_busy_time",
				      health_page->controller_busy_time[0], health_page->controller_busy_time[1]);
	spdk_json_write_named_uint128(w, "power_cycles",
				      health_page->power_cycles[0], health_page->power_cycles[1]);
	spdk_json_write_named_uint128(w, "power_on_hours",
				      health_page->power_on_hours[0], health_page->power_on_hours[1]);
	spdk_json_write_named_uint128(w, "unsafe_shutdowns",
				      health_page->unsafe_shutdowns[0], health_page->unsafe_shutdowns[1]);
	spdk_json_write_named_uint128(w, "media_errors",
				      health_page->media_errors[0], health_page->media_errors[1]);
	spdk_json_write_named_uint128(w, "num_err_log_entries",
				      health_page->num_error_info_log_entries[0], health_page->num_error_info_log_entries[1]);
	spdk_json_write_named_uint64(w, "warning_temperature_time_minutes", health_page->warning_temp_time);
	spdk_json_write_named_uint64(w, "critical_composite_temperature_time_minutes",
				     health_page->critical_temp_time);
	for (i = 0; i < 8; i++) {
		if (health_page->temp_sensor[i] != 0) {
			spdk_json_write_named_uint64(w, "temperature_sensor_celsius", health_page->temp_sensor[i] - 273);
		}
	}
	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(request, w);
	nvme_health_info_cleanup(context, false);
}

/*
 * [한국어]
 * get_health_log_page - NVMe Get Log Page 0x02 (Health Information) admin 명령 발행.
 *
 * spdk_nvme_ctrlr_cmd_get_log_page는 lib/nvme의 도우미로, 내부에서 SQE를 만들어
 * admin queue에 발행한다. SPDK_NVME_GLOBAL_NS_TAG = 0xFFFFFFFF (모든 namespace 합산).
 * 완료 시 get_health_log_page_completion 호출.
 */
static void
get_health_log_page(struct spdk_nvme_health_info_context *context)
{
	struct spdk_nvme_ctrlr *ctrlr = context->ctrlr;

	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_HEALTH_INFORMATION,
					     SPDK_NVME_GLOBAL_NS_TAG,
					     &(context->health_page), sizeof(context->health_page), 0,
					     get_health_log_page_completion, context)) {
		nvme_health_info_cleanup(context, true);
		SPDK_ERRLOG("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
	}
}

/*
 * [한국어]
 * get_temperature_threshold_feature_completion - Get Features 0x04 완료 콜백.
 *
 * NVMe Get Features (Temperature Threshold)는 단순히 "온도 기능 지원 확인" 용도이고,
 * 본격 데이터는 다음 단계인 Get Log Page에서 받는다. 여기서는 에러만 검사하고
 * 성공이면 다음 명령(get_health_log_page) 발행으로 진행.
 */
static void
get_temperature_threshold_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_health_info_context *context = cb_arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		nvme_health_info_cleanup(context, true);
		SPDK_ERRLOG("feature SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD failed in completion\n");
	} else {
		get_health_log_page(context);
	}
}

/*
 * [한국어]
 * get_temperature_threshold_feature - NVMe Get Features 0x04 (Temperature Threshold) 발행.
 *
 * SQE를 raw로 만들어 admin queue에 발행 (cdw10 = feature identifier).
 * 실제로는 data 없는 admin 명령 (cpl의 result에 값이 반환됨). 본 함수는 단지 컨트롤러가
 * 응답 가능한지 핑(ping) 용도. 다음 단계 Get Log Page에서 실제 SMART 데이터 회수.
 * @return: 0 성공 (이후 비동기 완료), 양수면 즉시 실패.
 */
static int
get_temperature_threshold_feature(struct spdk_nvme_health_info_context *context)
{
	struct spdk_nvme_cmd cmd = {};

	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10 = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;

	return spdk_nvme_ctrlr_cmd_admin_raw(context->ctrlr, &cmd, NULL, 0,
					     get_temperature_threshold_feature_completion, context);
}

/*
 * [한국어]
 * get_controller_health_info - health info 비동기 조회의 진입점 (내부 헬퍼).
 *
 * 컨텍스트 할당 후 Get Features → Get Log Page 체인을 시작. 결과 응답은
 * get_health_log_page_completion에서 JSON으로 직렬화되어 전송된다.
 */
static void
get_controller_health_info(struct spdk_jsonrpc_request *request, struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_health_info_context *context;

	context = calloc(1, sizeof(struct spdk_nvme_health_info_context));
	if (!context) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error.");
		return;
	}

	context->request = request;
	context->ctrlr = ctrlr;

	if (get_temperature_threshold_feature(context)) {
		nvme_health_info_cleanup(context, true);
		SPDK_ERRLOG("feature SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD failed to submit\n");
	}

	return;
}

/*
 * [한국어]
 * rpc_bdev_nvme_get_controller_health_info - "bdev_nvme_get_controller_health_info" RPC 진입점.
 *
 * 입력: {"name": "Nvme0"}.
 * 출력: SMART/Health 정보 객체 (모델/시리얼/펌웨어/온도/마모도/Power-On 시간 등).
 *
 * NVMe Get Log Page 0x02를 비동기로 발행하고 응답 시 JSON 직렬화.
 */
static void
rpc_bdev_nvme_get_controller_health_info(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_get_controller_health_info req = {};
	struct nvme_ctrlr *nvme_ctrlr = NULL;

	if (!params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Missing device name");

		return;
	}
	if (spdk_json_decode_object(params, rpc_bdev_nvme_get_controller_health_info_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_get_controller_health_info_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		free_rpc_get_controller_health_info(&req);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Invalid parameters");

		return;
	}

	nvme_ctrlr = nvme_ctrlr_get_by_name(req.name);

	if (!nvme_ctrlr) {
		SPDK_ERRLOG("nvme ctrlr name '%s' does not exist\n", req.name);
		free_rpc_get_controller_health_info(&req);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Device not found");
		return;
	}

	get_controller_health_info(request, nvme_ctrlr->ctrlr);
	free_rpc_get_controller_health_info(&req);

	return;
}
SPDK_RPC_REGISTER("bdev_nvme_get_controller_health_info",
		  rpc_bdev_nvme_get_controller_health_info, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_bdev_nvme_start_discovery - bdev_nvme_start_discovery RPC 입력.
 *
 * 디스커버리 컨트롤러의 트랜스포트 ID와 옵션을 받는다. wait_for_attach가 true이면
 * 첫 컨트롤러 attach가 끝날 때까지 응답을 보류 (attach_timeout_ms 안에).
 */
struct rpc_bdev_nvme_start_discovery {
	char *name;
	/* [한국어] 디스커버리 결과 컨트롤러들이 등록될 베이스 이름. */
	char *trtype;
	/* [한국어] 트랜스포트 종류 ("tcp", "rdma" 등). */
	char *adrfam;
	/* [한국어] 주소 패밀리 ("IPv4"/"IPv6"). optional. */
	char *traddr;
	/* [한국어] 디스커버리 컨트롤러 IP 주소. */
	char *trsvcid;
	/* [한국어] TCP 포트 (또는 RDMA 서비스 ID). */
	char *hostnqn;
	/* [한국어] 호스트 NQN. 디스커버리 인증/필터링에 사용. optional. */
	bool wait_for_attach;
	/* [한국어] true이면 첫 attach 완료까지 응답 보류. */
	uint64_t attach_timeout_ms;
	/* [한국어] wait_for_attach=true일 때의 타임아웃. */
	struct spdk_nvme_ctrlr_opts opts;
	/* [한국어] lib/nvme 드라이버 옵션 (queue_size 등). */
	struct spdk_bdev_nvme_ctrlr_opts bdev_opts;
	/* [한국어] bdev_nvme 측 옵션 (ctrlr_loss/reconnect/fast_io_fail 등). */
};

static void
free_rpc_bdev_nvme_start_discovery(struct rpc_bdev_nvme_start_discovery *req)
{
	free(req->name);
	free(req->trtype);
	free(req->adrfam);
	free(req->traddr);
	free(req->trsvcid);
	free(req->hostnqn);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_start_discovery_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_start_discovery, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_bdev_nvme_start_discovery, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_bdev_nvme_start_discovery, traddr), spdk_json_decode_string},
	{"adrfam", offsetof(struct rpc_bdev_nvme_start_discovery, adrfam), spdk_json_decode_string, true},
	{"trsvcid", offsetof(struct rpc_bdev_nvme_start_discovery, trsvcid), spdk_json_decode_string, true},
	{"hostnqn", offsetof(struct rpc_bdev_nvme_start_discovery, hostnqn), spdk_json_decode_string, true},
	{"wait_for_attach", offsetof(struct rpc_bdev_nvme_start_discovery, wait_for_attach), spdk_json_decode_bool, true},
	{"attach_timeout_ms", offsetof(struct rpc_bdev_nvme_start_discovery, attach_timeout_ms), spdk_json_decode_uint64, true},
	{"ctrlr_loss_timeout_sec", offsetof(struct rpc_bdev_nvme_start_discovery, bdev_opts.ctrlr_loss_timeout_sec), spdk_json_decode_int32, true},
	{"reconnect_delay_sec", offsetof(struct rpc_bdev_nvme_start_discovery, bdev_opts.reconnect_delay_sec), spdk_json_decode_uint32, true},
	{"fast_io_fail_timeout_sec", offsetof(struct rpc_bdev_nvme_start_discovery, bdev_opts.fast_io_fail_timeout_sec), spdk_json_decode_uint32, true},
};

struct rpc_bdev_nvme_start_discovery_ctx {
	struct rpc_bdev_nvme_start_discovery req;
	struct spdk_jsonrpc_request *request;
};

/*
 * [한국어]
 * rpc_bdev_nvme_start_discovery_done - wait_for_attach=true일 때 첫 attach 완료 콜백.
 * 성공 시 bool true, 실패 시 errno 응답.
 */
static void
rpc_bdev_nvme_start_discovery_done(void *ctx, int status)
{
	struct spdk_jsonrpc_request *request = ctx;

	if (status != 0) {
		spdk_jsonrpc_send_error_response(request, status, spdk_strerror(-status));
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}
}

/*
 * [한국어]
 * rpc_bdev_nvme_start_discovery - "bdev_nvme_start_discovery" RPC 진입점.
 *
 * 디스커버리 컨트롤러(보통 SUBNQN=nqn.2014-08.org.nvmexpress.discovery)에 NVMe-oF로
 * 연결한 뒤 Discovery Log Page를 주기적으로 폴링하여 새로 등장한 NVM 서브시스템들을
 * 자동 attach한다. 호출 자체는 bdev_nvme.c::bdev_nvme_start_discovery로 위임.
 *
 * wait_for_attach=true이면 첫 컨트롤러 attach가 끝날 때까지 응답을 미룬다 (콜백 등록).
 * false이면 즉시 bool 응답.
 */
static void
rpc_bdev_nvme_start_discovery(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_start_discovery_ctx *ctx;
	struct spdk_nvme_transport_id trid = {};
	size_t len, maxlen;
	int rc;
	spdk_bdev_nvme_start_discovery_fn cb_fn;
	void *cb_ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctx->req.opts, sizeof(ctx->req.opts));

	if (spdk_json_decode_object(params, rpc_bdev_nvme_start_discovery_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_start_discovery_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* Parse trstring */
	rc = spdk_nvme_transport_id_populate_trstring(&trid, ctx->req.trtype);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse trtype: %s\n", ctx->req.trtype);
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse trtype: %s",
						     ctx->req.trtype);
		goto cleanup;
	}

	/* Parse trtype */
	rc = spdk_nvme_transport_id_parse_trtype(&trid.trtype, ctx->req.trtype);
	assert(rc == 0);

	/* Parse traddr */
	maxlen = sizeof(trid.traddr);
	len = strnlen(ctx->req.traddr, maxlen);
	if (len == maxlen) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "traddr too long: %s",
						     ctx->req.traddr);
		goto cleanup;
	}
	memcpy(trid.traddr, ctx->req.traddr, len + 1);

	/* Parse adrfam */
	if (ctx->req.adrfam) {
		rc = spdk_nvme_transport_id_parse_adrfam(&trid.adrfam, ctx->req.adrfam);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse adrfam: %s\n", ctx->req.adrfam);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse adrfam: %s",
							     ctx->req.adrfam);
			goto cleanup;
		}
	}

	/* Parse trsvcid */
	if (ctx->req.trsvcid) {
		maxlen = sizeof(trid.trsvcid);
		len = strnlen(ctx->req.trsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "trsvcid too long: %s",
							     ctx->req.trsvcid);
			goto cleanup;
		}
		memcpy(trid.trsvcid, ctx->req.trsvcid, len + 1);
	}

	if (ctx->req.hostnqn) {
		snprintf(ctx->req.opts.hostnqn, sizeof(ctx->req.opts.hostnqn), "%s",
			 ctx->req.hostnqn);
	}

	if (ctx->req.attach_timeout_ms != 0) {
		ctx->req.wait_for_attach = true;
	}

	ctx->request = request;
	cb_fn = ctx->req.wait_for_attach ? rpc_bdev_nvme_start_discovery_done : NULL;
	cb_ctx = ctx->req.wait_for_attach ? request : NULL;
	rc = bdev_nvme_start_discovery(&trid, ctx->req.name, &ctx->req.opts, &ctx->req.bdev_opts,
				       ctx->req.attach_timeout_ms, false, cb_fn, cb_ctx);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	} else if (!ctx->req.wait_for_attach) {
		rpc_bdev_nvme_start_discovery_done(request, 0);
	}

cleanup:
	free_rpc_bdev_nvme_start_discovery(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_start_discovery", rpc_bdev_nvme_start_discovery,
		  SPDK_RPC_RUNTIME)

struct rpc_bdev_nvme_stop_discovery {
	char *name;
};

static const struct spdk_json_object_decoder rpc_bdev_nvme_stop_discovery_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_stop_discovery, name), spdk_json_decode_string},
};

struct rpc_bdev_nvme_stop_discovery_ctx {
	struct rpc_bdev_nvme_stop_discovery req;
	struct spdk_jsonrpc_request *request;
};

/*
 * [한국어]
 * rpc_bdev_nvme_stop_discovery_done - stop_discovery 비동기 완료 콜백.
 * 디스커버리 세션이 완전히 정리되면 호출되어 bool 응답 + ctx free.
 */
static void
rpc_bdev_nvme_stop_discovery_done(void *cb_ctx)
{
	struct rpc_bdev_nvme_stop_discovery_ctx *ctx = cb_ctx;

	spdk_jsonrpc_send_bool_response(ctx->request, true);
	free(ctx->req.name);
	free(ctx);
}

/*
 * [한국어]
 * rpc_bdev_nvme_stop_discovery - "bdev_nvme_stop_discovery" RPC: 진행 중인 디스커버리 중단.
 *
 * 디스커버리 컨트롤러로의 connect를 끊고 자동 attach 흐름을 정지한다.
 * 이미 attach된 컨트롤러들은 그대로 유지 (수동 detach 필요).
 * 비동기 완료 시 rpc_bdev_nvme_stop_discovery_done 호출.
 */
static void
rpc_bdev_nvme_stop_discovery(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_stop_discovery_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_nvme_stop_discovery_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_stop_discovery_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ctx->request = request;
	rc = bdev_nvme_stop_discovery(ctx->req.name, rpc_bdev_nvme_stop_discovery_done, ctx);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	return;

cleanup:
	free(ctx->req.name);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_stop_discovery", rpc_bdev_nvme_stop_discovery,
		  SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_bdev_nvme_get_discovery_info - "bdev_nvme_get_discovery_info" RPC.
 * 활성 디스커버리 세션 정보를 JSON 배열로 출력 (bdev_nvme.c 내부 헬퍼에 위임).
 */
static void
rpc_bdev_nvme_get_discovery_info(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	bdev_nvme_get_discovery_info(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("bdev_nvme_get_discovery_info", rpc_bdev_nvme_get_discovery_info,
		  SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * enum error_injection_cmd_type - error injection 대상 명령 종류.
 *
 * lib/nvme의 cmd error injection 기능은 특정 opcode/qpair에 대해 인위적으로
 * NVMe 응답 status를 조작하거나 명령을 보류시켜 호스트 측 에러 처리 코드를 테스트한다.
 */
enum error_injection_cmd_type {
	NVME_ADMIN_CMD = 1,   /* [한국어] admin queue 명령에 inject (qpair=NULL로 전달). */
	NVME_IO_CMD,          /* [한국어] IO queue 명령에 inject (모든 채널 순회 필요). */
};

/*
 * [한국어]
 * struct rpc_add_error_injection - error injection 추가 RPC 입력.
 *
 * 어떤 opcode에 어떤 status로 몇 번 인젝션할지 지정. timeout_in_us를 지정하면
 * 응답을 일정 시간 지연시킨다 (do_not_submit과 함께 timeout 동작 테스트용).
 */
struct rpc_add_error_injection {
	char *name;                          /* [한국어] 대상 컨트롤러 이름. */
	enum error_injection_cmd_type cmd_type; /* [한국어] admin/io 구분. */
	uint8_t opc;                          /* [한국어] inject 대상 opcode. */
	bool do_not_submit;                   /* [한국어] true면 디바이스에 명령을 보내지 않고 즉시 가짜 응답. */
	uint64_t timeout_in_us;               /* [한국어] 응답 지연 (timeout 시나리오 테스트). */
	uint32_t err_count;                   /* [한국어] 몇 번까지 inject할지 (default 1). */
	uint8_t sct;                          /* [한국어] 위조할 Status Code Type. */
	uint8_t sc;                           /* [한국어] 위조할 Status Code. */
};

static void
free_rpc_add_error_injection(struct rpc_add_error_injection *req)
{
	free(req->name);
}

static int
rpc_error_injection_decode_cmd_type(const struct spdk_json_val *val, void *out)
{
	int *cmd_type = out;

	if (spdk_json_strequal(val, "admin")) {
		*cmd_type = NVME_ADMIN_CMD;
	} else if (spdk_json_strequal(val, "io")) {
		*cmd_type = NVME_IO_CMD;
	} else {
		SPDK_ERRLOG("Invalid parameter value: cmd_type\n");
		return -EINVAL;
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_add_error_injection_decoders[] = {
	{ "name", offsetof(struct rpc_add_error_injection, name), spdk_json_decode_string },
	{ "cmd_type", offsetof(struct rpc_add_error_injection, cmd_type), rpc_error_injection_decode_cmd_type },
	{ "opc", offsetof(struct rpc_add_error_injection, opc), spdk_json_decode_uint8 },
	{ "do_not_submit", offsetof(struct rpc_add_error_injection, do_not_submit), spdk_json_decode_bool, true },
	{ "timeout_in_us", offsetof(struct rpc_add_error_injection, timeout_in_us), spdk_json_decode_uint64, true },
	{ "err_count", offsetof(struct rpc_add_error_injection, err_count), spdk_json_decode_uint32, true },
	{ "sct", offsetof(struct rpc_add_error_injection, sct), spdk_json_decode_uint8, true},
	{ "sc", offsetof(struct rpc_add_error_injection, sc), spdk_json_decode_uint8, true},
};

struct rpc_add_error_injection_ctx {
	struct spdk_jsonrpc_request *request;
	struct rpc_add_error_injection rpc;
};

/*
 * [한국어]
 * rpc_add_error_injection_done - 모든 채널에 inject 적용 후 호출되는 완료 콜백.
 * 첫 채널에서 실패 status가 나오면 그 status가 그대로 전파됨.
 */
static void
rpc_add_error_injection_done(struct nvme_ctrlr *nvme_ctrlr, void *_ctx, int status)
{
	struct rpc_add_error_injection_ctx *ctx = _ctx;

	if (status) {
		spdk_jsonrpc_send_error_response(ctx->request, status,
						 "Failed to add the error injection.");
	} else {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	}

	free_rpc_add_error_injection(&ctx->rpc);
	free(ctx);
}

/*
 * [한국어]
 * rpc_add_error_injection_per_channel - 각 IO 채널에서 inject 등록.
 *
 * for_each_channel 콜백. 채널의 qpair에 spdk_nvme_qpair_add_cmd_error_injection 호출.
 * qpair=NULL이면 disconnect 상태이므로 skip. 실행 컨텍스트는 해당 채널 소속 SPDK 스레드.
 */
static void
rpc_add_error_injection_per_channel(struct nvme_ctrlr_channel_iter *i,
				    struct nvme_ctrlr *nvme_ctrlr,
				    struct nvme_ctrlr_channel *ctrlr_ch,
				    void *_ctx)
{
	struct rpc_add_error_injection_ctx *ctx = _ctx;
	struct spdk_nvme_qpair *qpair = ctrlr_ch->qpair->qpair;
	struct spdk_nvme_ctrlr *ctrlr = ctrlr_ch->qpair->ctrlr->ctrlr;
	int rc = 0;

	if (qpair != NULL) {
		rc = spdk_nvme_qpair_add_cmd_error_injection(ctrlr, qpair, ctx->rpc.opc,
				ctx->rpc.do_not_submit, ctx->rpc.timeout_in_us, ctx->rpc.err_count,
				ctx->rpc.sct, ctx->rpc.sc);
	}

	nvme_ctrlr_for_each_channel_continue(i, rc);
}

/*
 * [한국어]
 * rpc_bdev_nvme_add_error_injection - "bdev_nvme_add_error_injection" RPC 진입점.
 *
 * cmd_type=admin: 컨트롤러 admin queue에 직접 inject (qpair=NULL).
 * cmd_type=io: 모든 IO 채널을 순회하며 각 qpair에 inject.
 * 디버깅/테스트 도구이므로 일반 운영에서는 사용하지 않는다.
 */
static void
rpc_bdev_nvme_add_error_injection(
	struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *params)
{
	struct rpc_add_error_injection_ctx *ctx;
	struct nvme_ctrlr *nvme_ctrlr;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}
	ctx->rpc.err_count = 1;
	ctx->request = request;

	if (spdk_json_decode_object(params,
				    rpc_bdev_nvme_add_error_injection_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_add_error_injection_decoders),
				    &ctx->rpc)) {
		spdk_jsonrpc_send_error_response(request, -EINVAL,
						 "Failed to parse the request");
		goto cleanup;
	}

	nvme_ctrlr = nvme_ctrlr_get_by_name(ctx->rpc.name);
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("No controller with specified name was found.\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	if (ctx->rpc.cmd_type == NVME_IO_CMD) {
		nvme_ctrlr_for_each_channel(nvme_ctrlr,
					    rpc_add_error_injection_per_channel,
					    ctx,
					    rpc_add_error_injection_done);

		return;
	} else {
		rc = spdk_nvme_qpair_add_cmd_error_injection(nvme_ctrlr->ctrlr, NULL, ctx->rpc.opc,
				ctx->rpc.do_not_submit, ctx->rpc.timeout_in_us, ctx->rpc.err_count,
				ctx->rpc.sct, ctx->rpc.sc);
		if (rc) {
			spdk_jsonrpc_send_error_response(request, -rc,
							 "Failed to add the error injection");
		} else {
			spdk_jsonrpc_send_bool_response(ctx->request, true);
		}
	}

cleanup:
	free_rpc_add_error_injection(&ctx->rpc);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_add_error_injection", rpc_bdev_nvme_add_error_injection,
		  SPDK_RPC_RUNTIME)

struct rpc_remove_error_injection {
	char *name;
	enum error_injection_cmd_type cmd_type;
	uint8_t opc;
};

static void
free_rpc_remove_error_injection(struct rpc_remove_error_injection *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_remove_error_injection_decoders[] = {
	{ "name", offsetof(struct rpc_remove_error_injection, name), spdk_json_decode_string },
	{ "cmd_type", offsetof(struct rpc_remove_error_injection, cmd_type), rpc_error_injection_decode_cmd_type },
	{ "opc", offsetof(struct rpc_remove_error_injection, opc), spdk_json_decode_uint8 },
};

struct rpc_remove_error_injection_ctx {
	struct spdk_jsonrpc_request *request;
	struct rpc_remove_error_injection rpc;
};

static void
rpc_remove_error_injection_done(struct nvme_ctrlr *nvme_ctrlr, void *_ctx, int status)
{
	struct rpc_remove_error_injection_ctx *ctx = _ctx;

	if (status) {
		spdk_jsonrpc_send_error_response(ctx->request, status,
						 "Failed to remove the error injection.");
	} else {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	}

	free_rpc_remove_error_injection(&ctx->rpc);
	free(ctx);
}

static void
rpc_remove_error_injection_per_channel(struct nvme_ctrlr_channel_iter *i,
				       struct nvme_ctrlr *nvme_ctrlr,
				       struct nvme_ctrlr_channel *ctrlr_ch,
				       void *_ctx)
{
	struct rpc_remove_error_injection_ctx *ctx = _ctx;
	struct spdk_nvme_qpair *qpair = ctrlr_ch->qpair->qpair;
	struct spdk_nvme_ctrlr *ctrlr = ctrlr_ch->qpair->ctrlr->ctrlr;

	if (qpair != NULL) {
		spdk_nvme_qpair_remove_cmd_error_injection(ctrlr, qpair, ctx->rpc.opc);
	}

	nvme_ctrlr_for_each_channel_continue(i, 0);
}

/*
 * [한국어]
 * rpc_bdev_nvme_remove_error_injection - "bdev_nvme_remove_error_injection" RPC.
 * 등록된 error injection을 제거 (opcode 기준 lookup).
 */
static void
rpc_bdev_nvme_remove_error_injection(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_remove_error_injection_ctx *ctx;
	struct nvme_ctrlr *nvme_ctrlr;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}
	ctx->request = request;

	if (spdk_json_decode_object(params,
				    rpc_bdev_nvme_remove_error_injection_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_remove_error_injection_decoders),
				    &ctx->rpc)) {
		spdk_jsonrpc_send_error_response(request, -EINVAL,
						 "Failed to parse the request");
		goto cleanup;
	}

	nvme_ctrlr = nvme_ctrlr_get_by_name(ctx->rpc.name);
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("No controller with specified name was found.\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	if (ctx->rpc.cmd_type == NVME_IO_CMD) {
		nvme_ctrlr_for_each_channel(nvme_ctrlr,
					    rpc_remove_error_injection_per_channel,
					    ctx,
					    rpc_remove_error_injection_done);
		return;
	} else {
		spdk_nvme_qpair_remove_cmd_error_injection(nvme_ctrlr->ctrlr, NULL, ctx->rpc.opc);
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	}

cleanup:
	free_rpc_remove_error_injection(&ctx->rpc);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_remove_error_injection", rpc_bdev_nvme_remove_error_injection,
		  SPDK_RPC_RUNTIME)

struct rpc_get_io_paths {
	char *name;
};

static void
free_rpc_get_io_paths(struct rpc_get_io_paths *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_get_io_paths_decoders[] = {
	{"name", offsetof(struct rpc_get_io_paths, name), spdk_json_decode_string, true},
};

struct rpc_get_io_paths_ctx {
	struct rpc_get_io_paths req;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
};

/*
 * [한국어]
 * rpc_bdev_nvme_get_io_paths_done - 모든 poll group 채널 순회 완료 후 응답 마무리.
 */
static void
rpc_bdev_nvme_get_io_paths_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_get_io_paths_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	spdk_json_write_array_end(ctx->w);

	spdk_json_write_object_end(ctx->w);

	spdk_jsonrpc_end_result(ctx->request, ctx->w);

	free_rpc_get_io_paths(&ctx->req);
	free(ctx);
}

/*
 * [한국어]
 * _rpc_bdev_nvme_get_io_paths - 각 poll group 채널에서 호출되어 그 채널의 io_path를 dump.
 *
 * 채널 컨텍스트인 nvme_poll_group의 qpair_list를 순회 → 각 qpair의 io_path_list 순회.
 * name 필터가 있으면 해당 bdev 이름과 일치하는 io_path만 출력.
 * 실행 컨텍스트: 해당 채널 SPDK 스레드 (lock-free 접근 가능).
 */
static void
_rpc_bdev_nvme_get_io_paths(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_poll_group *group = spdk_io_channel_get_ctx(_ch);
	struct rpc_get_io_paths_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct nvme_qpair *qpair;
	struct nvme_io_path *io_path;
	struct nvme_bdev *nbdev;

	spdk_json_write_object_begin(ctx->w);

	spdk_json_write_named_string(ctx->w, "thread", spdk_thread_get_name(spdk_get_thread()));

	spdk_json_write_named_array_begin(ctx->w, "io_paths");

	TAILQ_FOREACH(qpair, &group->qpair_list, tailq) {
		TAILQ_FOREACH(io_path, &qpair->io_path_list, tailq) {
			nbdev = io_path->nvme_ns->bdev;

			if (ctx->req.name != NULL &&
			    strcmp(ctx->req.name, nbdev->disk.name) != 0) {
				continue;
			}

			nvme_io_path_info_json(ctx->w, io_path);
		}
	}

	spdk_json_write_array_end(ctx->w);

	spdk_json_write_object_end(ctx->w);

	spdk_for_each_channel_continue(i, 0);
}

/*
 * [한국어]
 * rpc_bdev_nvme_get_io_paths - "bdev_nvme_get_io_paths" RPC: 모든 io_path 정보 조회.
 *
 * 입력: {"name": <bdev 이름>?} optional - 특정 bdev의 path만 보고 싶을 때.
 * 출력: poll_groups 배열. 각 그룹에 thread 이름 + io_paths 배열.
 * 멀티패스 모드에서 어떤 ns/qpair pair가 어느 스레드에 활성인지 진단할 때 유용.
 */
static void
rpc_bdev_nvme_get_io_paths(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_get_io_paths_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (params != NULL &&
	    spdk_json_decode_object(params, rpc_bdev_nvme_get_io_paths_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_get_io_paths_decoders),
				    &ctx->req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "bdev_nvme_get_io_paths requires no parameters");

		free_rpc_get_io_paths(&ctx->req);
		free(ctx);
		return;
	}

	ctx->request = request;
	ctx->w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(ctx->w);

	spdk_json_write_named_array_begin(ctx->w, "poll_groups");

	spdk_for_each_channel(&g_nvme_bdev_ctrlrs,
			      _rpc_bdev_nvme_get_io_paths,
			      ctx,
			      rpc_bdev_nvme_get_io_paths_done);
}
SPDK_RPC_REGISTER("bdev_nvme_get_io_paths", rpc_bdev_nvme_get_io_paths, SPDK_RPC_RUNTIME)

struct rpc_bdev_nvme_set_preferred_path {
	char *name;
	uint16_t cntlid;
};

static void
free_rpc_bdev_nvme_set_preferred_path(struct rpc_bdev_nvme_set_preferred_path *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_set_preferred_path_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_set_preferred_path, name), spdk_json_decode_string},
	{"cntlid", offsetof(struct rpc_bdev_nvme_set_preferred_path, cntlid), spdk_json_decode_uint16},
};

struct rpc_bdev_nvme_set_preferred_path_ctx {
	struct rpc_bdev_nvme_set_preferred_path req;
	struct spdk_jsonrpc_request *request;
};

/*
 * [한국어]
 * rpc_bdev_nvme_set_preferred_path_done - preferred path 설정 완료 콜백.
 * 모든 채널에서 current_io_path 갱신이 끝나면 호출. ctx free + 응답.
 */
static void
rpc_bdev_nvme_set_preferred_path_done(void *cb_arg, int rc)
{
	struct rpc_bdev_nvme_set_preferred_path_ctx *ctx = cb_arg;

	if (rc == 0) {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	} else {
		spdk_jsonrpc_send_error_response(ctx->request, rc, spdk_strerror(-rc));
	}

	free_rpc_bdev_nvme_set_preferred_path(&ctx->req);
	free(ctx);
}

/*
 * [한국어]
 * rpc_bdev_nvme_set_preferred_path - "bdev_nvme_set_preferred_path" RPC.
 *
 * 입력: {"name": <bdev 이름>, "cntlid": <cntlid>}.
 * active-active 멀티패스에서 특정 컨트롤러(cntlid)의 경로를 모든 채널이 우선 사용하도록
 * current_io_path 캐시를 강제 변경한다. failover 모드에서는 동작하지 않는다.
 */
static void
rpc_bdev_nvme_set_preferred_path(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_set_preferred_path_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_nvme_set_preferred_path_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_set_preferred_path_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ctx->request = request;

	bdev_nvme_set_preferred_path(ctx->req.name, ctx->req.cntlid,
				     rpc_bdev_nvme_set_preferred_path_done, ctx);
	return;

cleanup:
	free_rpc_bdev_nvme_set_preferred_path(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_set_preferred_path", rpc_bdev_nvme_set_preferred_path,
		  SPDK_RPC_RUNTIME)

struct rpc_set_multipath_policy {
	char *name;
	enum spdk_bdev_nvme_multipath_policy policy;
	enum spdk_bdev_nvme_multipath_selector selector;
	uint32_t rr_min_io;
};

static void
free_rpc_set_multipath_policy(struct rpc_set_multipath_policy *req)
{
	free(req->name);
}

/*
 * [한국어]
 * rpc_decode_mp_policy - "active_passive"/"active_active" 문자열을 enum으로 디코딩.
 * 그 외 값은 -EINVAL.
 */
static int
rpc_decode_mp_policy(const struct spdk_json_val *val, void *out)
{
	enum spdk_bdev_nvme_multipath_policy *policy = out;

	if (spdk_json_strequal(val, "active_passive") == true) {
		*policy = BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE;
	} else if (spdk_json_strequal(val, "active_active") == true) {
		*policy = BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: policy\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * rpc_decode_mp_selector - "round_robin"/"queue_depth" 문자열을 enum으로 디코딩.
 * active_active 모드에서 path 선택 알고리즘을 지정.
 */
static int
rpc_decode_mp_selector(const struct spdk_json_val *val, void *out)
{
	enum spdk_bdev_nvme_multipath_selector *selector = out;

	if (spdk_json_strequal(val, "round_robin") == true) {
		*selector = BDEV_NVME_MP_SELECTOR_ROUND_ROBIN;
	} else if (spdk_json_strequal(val, "queue_depth") == true) {
		*selector = BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: selector\n");
		return -EINVAL;
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_set_multipath_policy_decoders[] = {
	{"name", offsetof(struct rpc_set_multipath_policy, name), spdk_json_decode_string},
	{"policy", offsetof(struct rpc_set_multipath_policy, policy), rpc_decode_mp_policy},
	{"selector", offsetof(struct rpc_set_multipath_policy, selector), rpc_decode_mp_selector, true},
	{"rr_min_io", offsetof(struct rpc_set_multipath_policy, rr_min_io), spdk_json_decode_uint32, true},
};

struct rpc_set_multipath_policy_ctx {
	struct rpc_set_multipath_policy req;
	struct spdk_jsonrpc_request *request;
};

/*
 * [한국어]
 * rpc_bdev_nvme_set_multipath_policy_done - 멀티패스 정책 적용 완료 콜백.
 * 모든 nvme_bdev 채널의 mp_policy/mp_selector 갱신이 끝나면 호출.
 */
static void
rpc_bdev_nvme_set_multipath_policy_done(void *cb_arg, int rc)
{
	struct rpc_set_multipath_policy_ctx *ctx = cb_arg;

	if (rc == 0) {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	} else {
		spdk_jsonrpc_send_error_response(ctx->request, rc, spdk_strerror(-rc));
	}

	free_rpc_set_multipath_policy(&ctx->req);
	free(ctx);
}

/*
 * [한국어]
 * rpc_bdev_nvme_set_multipath_policy - "bdev_nvme_set_multipath_policy" RPC.
 *
 * 입력: {"name": <bdev>, "policy": "active_passive"|"active_active",
 *        "selector"?: "round_robin"|"queue_depth", "rr_min_io"?: uint32}.
 *
 * active_active일 때만 selector를 의미하며, default selector=round_robin.
 * rr_min_io는 round_robin 모드에서 한 path에 연속 발행할 최소 I/O 수.
 */
static void
rpc_bdev_nvme_set_multipath_policy(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_set_multipath_policy_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	ctx->req.rr_min_io = UINT32_MAX;
	ctx->req.selector = UINT32_MAX;

	if (spdk_json_decode_object(params, rpc_bdev_nvme_set_multipath_policy_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_set_multipath_policy_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ctx->request = request;
	if (ctx->req.selector == UINT32_MAX) {
		if (ctx->req.policy == BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE) {
			ctx->req.selector = BDEV_NVME_MP_SELECTOR_ROUND_ROBIN;
		} else {
			ctx->req.selector = 0;
		}
	}

	if (ctx->req.policy != BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE && ctx->req.selector > 0) {
		SPDK_ERRLOG("selector only works in active_active mode\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	spdk_bdev_nvme_set_multipath_policy(ctx->req.name, ctx->req.policy, ctx->req.selector,
					    ctx->req.rr_min_io,
					    rpc_bdev_nvme_set_multipath_policy_done, ctx);
	return;

cleanup:
	free_rpc_set_multipath_policy(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_set_multipath_policy", rpc_bdev_nvme_set_multipath_policy,
		  SPDK_RPC_RUNTIME)

struct rpc_bdev_nvme_start_mdns_discovery {
	char *name;
	char *svcname;
	char *hostnqn;
	struct spdk_nvme_ctrlr_opts opts;
	struct spdk_bdev_nvme_ctrlr_opts bdev_opts;
};

static void
free_rpc_bdev_nvme_start_mdns_discovery(struct rpc_bdev_nvme_start_mdns_discovery *req)
{
	free(req->name);
	free(req->svcname);
	free(req->hostnqn);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_start_mdns_discovery_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_start_mdns_discovery, name), spdk_json_decode_string},
	{"svcname", offsetof(struct rpc_bdev_nvme_start_mdns_discovery, svcname), spdk_json_decode_string},
	{"hostnqn", offsetof(struct rpc_bdev_nvme_start_mdns_discovery, hostnqn), spdk_json_decode_string, true},
};

struct rpc_bdev_nvme_start_mdns_discovery_ctx {
	struct rpc_bdev_nvme_start_mdns_discovery req;
	struct spdk_jsonrpc_request *request;
};

/*
 * [한국어]
 * rpc_bdev_nvme_start_mdns_discovery - "bdev_nvme_start_mdns_discovery" RPC.
 *
 * 입력: {"name": <base name>, "svcname": <mDNS 서비스 이름, 예: "_nvme-disc._tcp">,
 *        "hostnqn"?: ...}.
 * mDNS(Multicast DNS)로 LAN에 광고된 NVMe-oF Discovery 컨트롤러를 자동 발견하고
 * 발견될 때마다 bdev_nvme_start_discovery로 attach 흐름을 시작한다.
 * 구현 본체는 bdev_mdns_client.c.
 */
static void
rpc_bdev_nvme_start_mdns_discovery(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_start_mdns_discovery_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctx->req.opts, sizeof(ctx->req.opts));

	if (spdk_json_decode_object(params, rpc_bdev_nvme_start_mdns_discovery_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_start_mdns_discovery_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (ctx->req.hostnqn) {
		snprintf(ctx->req.opts.hostnqn, sizeof(ctx->req.opts.hostnqn), "%s",
			 ctx->req.hostnqn);
	}
	ctx->request = request;
	rc = bdev_nvme_start_mdns_discovery(ctx->req.name, ctx->req.svcname, &ctx->req.opts,
					    &ctx->req.bdev_opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}

cleanup:
	free_rpc_bdev_nvme_start_mdns_discovery(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_start_mdns_discovery", rpc_bdev_nvme_start_mdns_discovery,
		  SPDK_RPC_RUNTIME)

struct rpc_bdev_nvme_stop_mdns_discovery {
	char *name;
};

static const struct spdk_json_object_decoder rpc_bdev_nvme_stop_mdns_discovery_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_stop_mdns_discovery, name), spdk_json_decode_string},
};

struct rpc_bdev_nvme_stop_mdns_discovery_ctx {
	struct rpc_bdev_nvme_stop_mdns_discovery req;
	struct spdk_jsonrpc_request *request;
};

/*
 * [한국어]
 * rpc_bdev_nvme_stop_mdns_discovery - "bdev_nvme_stop_mdns_discovery" RPC.
 * 진행 중인 mDNS 디스커버리 세션을 정지. Avahi 클라이언트 해제 + poller 종료.
 */
static void
rpc_bdev_nvme_stop_mdns_discovery(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_stop_mdns_discovery_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_nvme_stop_mdns_discovery_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_stop_mdns_discovery_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ctx->request = request;
	rc = bdev_nvme_stop_mdns_discovery(ctx->req.name);

	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}
	spdk_jsonrpc_send_bool_response(ctx->request, true);

cleanup:
	free(ctx->req.name);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_stop_mdns_discovery", rpc_bdev_nvme_stop_mdns_discovery,
		  SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_bdev_nvme_get_mdns_discovery_info - "bdev_nvme_get_mdns_discovery_info" RPC.
 * 현재 활성 mDNS 디스커버리 세션의 정보 (svcname, 발견된 컨트롤러 목록 등)를 JSON으로 출력.
 * 본체는 bdev_mdns_client.c::bdev_nvme_get_mdns_discovery_info.
 */
static void
rpc_bdev_nvme_get_mdns_discovery_info(struct spdk_jsonrpc_request *request,
				      const struct spdk_json_val *params)
{
	bdev_nvme_get_mdns_discovery_info(request);
}

SPDK_RPC_REGISTER("bdev_nvme_get_mdns_discovery_info", rpc_bdev_nvme_get_mdns_discovery_info,
		  SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_get_path_stat - bdev_nvme_get_path_iostat RPC 입력 (name 1개).
 */
struct rpc_get_path_stat {
	char	*name;
	/* [한국어] 통계를 조회할 nvme_bdev 이름. */
};

/*
 * [한국어]
 * struct path_stat - 한 io_path의 IO 통계 + 해당 path의 trid 식별 정보.
 *
 * 채널 순회 중 누적되며, 완료 시 trid와 stat을 JSON으로 직렬화.
 */
struct path_stat {
	struct spdk_bdev_io_stat	stat;
	/* [한국어] 채널별로 누적 add한 bdev IO 통계 (bytes_read/written, num_*_ops 등). */

	struct spdk_nvme_transport_id	trid;
	/* [한국어] 이 path가 어느 컨트롤러(trid)인지 식별. 출력 시 path별로 구분. */

	/* This pointer is cached on the app thread and may be freed while
	 * iterating over nbdev channels; it must not be dereferenced. */
	void				*nvme_ns;
	/* [한국어] nvme_ns 포인터의 "값"만 키로 사용 (역참조 금지).
	 * 채널 순회 도중 ns가 detach될 수 있어 안전하게 비교 키로만 활용. */
};

/*
 * [한국어]
 * struct rpc_bdev_nvme_path_stat_ctx - get_path_iostat 비동기 컨텍스트.
 */
struct rpc_bdev_nvme_path_stat_ctx {
	struct spdk_jsonrpc_request	*request;
	/* [한국어] 원래 RPC 요청 핸들. */

	struct path_stat		*path_stat;
	/* [한국어] num_paths 크기로 할당된 path별 통계 배열. */

	uint32_t			num_paths;
	/* [한국어] path_stat 배열 길이 = 현재 멀티패스 경로 수. */

	struct spdk_bdev_desc		*desc;
	/* [한국어] bdev open 디스크립터. 채널 순회 동안 bdev unregister를 막아준다. */
};

static void
free_rpc_get_path_stat(struct rpc_get_path_stat *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_get_path_iostat_decoders[] = {
	{"name", offsetof(struct rpc_get_path_stat, name), spdk_json_decode_string},
};

/*
 * [한국어]
 * dummy_bdev_event_cb - 통계 RPC가 bdev_open할 때 쓰는 빈 이벤트 콜백.
 * 통계는 짧은 시간에 종료되므로 별도 이벤트 처리 불필요.
 */
static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

/*
 * [한국어]
 * rpc_bdev_nvme_path_stat_per_channel - 각 nbdev 채널에서 path별 IO 통계 누적.
 *
 * 채널의 io_path_list를 순회하며, 사전에 채워둔 path_stat[*].nvme_ns 키와 매칭되는
 * io_path의 stat를 합산. 모든 채널이 합산되면 path_stat_done에서 한 번에 JSON 직렬화.
 * 실행 컨텍스트: 해당 채널의 SPDK 스레드 (각 채널 lockless).
 */
static void
rpc_bdev_nvme_path_stat_per_channel(struct nvme_bdev_channel_iter *i,
				    struct nvme_bdev *nbdev,
				    struct nvme_bdev_channel *nbdev_ch,
				    void *_ctx)
{
	struct rpc_bdev_nvme_path_stat_ctx *ctx = _ctx;
	struct nvme_io_path *io_path;
	struct path_stat *path_stat;
	uint32_t j;

	assert(ctx->num_paths != 0);

	for (j = 0; j < ctx->num_paths; j++) {
		path_stat = &ctx->path_stat[j];

		STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
			if (path_stat->nvme_ns == io_path->nvme_ns) {
				assert(io_path->stat != NULL);
				spdk_bdev_add_io_stat(&path_stat->stat, io_path->stat);
			}
		}
	}

	nvme_bdev_for_each_channel_continue(i, 0);
}

/*
 * [한국어]
 * rpc_bdev_nvme_path_stat_done - 모든 채널 통계 합산 완료 후 응답 빌드.
 *
 * 각 path를 객체로 출력 (trid + stat). spdk_bdev_dump_io_stat_json이 bytes_read/written,
 * num_*_ops, ticks_*_size_histogram 등을 표준 JSON 키로 출력. desc close + free.
 */
static void
rpc_bdev_nvme_path_stat_done(struct nvme_bdev *nbdev, void *_ctx, int status)
{
	struct rpc_bdev_nvme_path_stat_ctx *ctx = _ctx;
	struct spdk_json_write_ctx *w;
	struct path_stat *path_stat;
	uint32_t j;

	assert(ctx->num_paths != 0);

	w = spdk_jsonrpc_begin_result(ctx->request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", nbdev->disk.name);
	spdk_json_write_named_array_begin(w, "stats");

	for (j = 0; j < ctx->num_paths; j++) {
		path_stat = &ctx->path_stat[j];
		spdk_json_write_object_begin(w);

		spdk_json_write_named_object_begin(w, "trid");
		nvme_bdev_dump_trid_json(&path_stat->trid, w);
		spdk_json_write_object_end(w);

		spdk_json_write_named_object_begin(w, "stat");
		spdk_bdev_dump_io_stat_json(&path_stat->stat, w);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(ctx->request, w);

	spdk_bdev_close(ctx->desc);
	free(ctx->path_stat);
	free(ctx);
}

/*
 * [한국어]
 * rpc_bdev_nvme_get_path_iostat - "bdev_nvme_get_path_iostat" RPC.
 *
 * 멀티패스 모드에서 각 path별로 누적된 IO 통계를 조회한다. 사전에 옵션
 * io_path_stat=true로 attach해야 io_path->stat가 존재. 채널 순회는 nbdev 채널 단위
 * (그 안에서 path_list의 io_path를 ns 키로 매칭하여 누적).
 */
static void
rpc_bdev_nvme_get_path_iostat(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_get_path_stat req = {};
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_bdev *bdev;
	struct nvme_bdev *nbdev;
	struct nvme_ns *nvme_ns;
	struct path_stat *path_stat;
	struct rpc_bdev_nvme_path_stat_ctx *ctx;
	struct spdk_bdev_nvme_opts opts;
	uint32_t num_paths, i = 0;
	int rc;

	spdk_bdev_nvme_get_opts(&opts, sizeof(opts));
	if (!opts.io_path_stat) {
		SPDK_ERRLOG("RPC not enabled if enable_io_path_stat is false\n");
		spdk_jsonrpc_send_error_response(request, -EPERM,
						 "RPC not enabled if enable_io_path_stat is false");
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_nvme_get_path_iostat_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_get_path_iostat_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		free_rpc_get_path_stat(&req);
		return;
	}

	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open bdev '%s': %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		free_rpc_get_path_stat(&req);
		return;
	}

	free_rpc_get_path_stat(&req);

	ctx = calloc(1, sizeof(struct rpc_bdev_nvme_path_stat_ctx));
	if (ctx == NULL) {
		spdk_bdev_close(desc);
		SPDK_ERRLOG("Failed to allocate rpc_bdev_nvme_path_stat_ctx struct\n");
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);
	nbdev = bdev->ctxt;

	if (nbdev->ref == 0) {
		rc = -ENOENT;
		goto err;
	}

	num_paths = nbdev->ref;
	path_stat = calloc(num_paths, sizeof(struct path_stat));
	if (path_stat == NULL) {
		rc = -ENOMEM;
		SPDK_ERRLOG("Failed to allocate memory for path_stat.\n");
		goto err;
	}

	/* store the history stat */
	TAILQ_FOREACH(nvme_ns, &nbdev->nvme_ns_list, tailq) {
		assert(i < num_paths);
		path_stat[i].nvme_ns = nvme_ns;
		path_stat[i].trid = nvme_ns->ctrlr->active_path_id->trid;
		i++;
	}

	ctx->request = request;
	ctx->desc = desc;
	ctx->path_stat = path_stat;
	ctx->num_paths = num_paths;

	/* Number of paths can change while iterating over nbdev channels; stats for
	 * these will not be gathered until the next bdev_nvme_get_path_iostat call. */
	nvme_bdev_for_each_channel(nbdev,
				   rpc_bdev_nvme_path_stat_per_channel,
				   ctx,
				   rpc_bdev_nvme_path_stat_done);
	return;

err:
	spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	spdk_bdev_close(desc);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_get_path_iostat", rpc_bdev_nvme_get_path_iostat,
		  SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * struct rpc_bdev_nvme_set_keys - bdev_nvme_set_keys RPC 입력.
 *
 * NVMe-oF DH-CHAP 인증에 사용할 키 식별자(name). lib/keyring을 통해 실제 키 머티리얼 lookup.
 */
struct rpc_bdev_nvme_set_keys {
	char *name;
	/* [한국어] 대상 컨트롤러/그룹 이름. */
	char *dhchap_key;
	/* [한국어] 호스트 측 DH-CHAP 키 식별자 (NULL이면 호스트 키 미설정).
	 * spdk_keyring으로 lookup해 실제 secret 획득. */
	char *dhchap_ctrlr_key;
	/* [한국어] 컨트롤러 측 DH-CHAP 키 식별자 (양방향 인증 시 필요). */
};

static const struct spdk_json_object_decoder rpc_bdev_nvme_set_keys_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_set_keys, name), spdk_json_decode_string},
	{"dhchap_key", offsetof(struct rpc_bdev_nvme_set_keys, dhchap_key), spdk_json_decode_string, true},
	{"dhchap_ctrlr_key", offsetof(struct rpc_bdev_nvme_set_keys, dhchap_ctrlr_key), spdk_json_decode_string, true},
};

static void
free_rpc_bdev_nvme_set_keys(struct rpc_bdev_nvme_set_keys *req)
{
	free(req->name);
	free(req->dhchap_key);
	free(req->dhchap_ctrlr_key);
}

/*
 * [한국어]
 * rpc_bdev_nvme_set_keys_done - DH-CHAP 키 갱신 비동기 완료 콜백.
 * 모든 진행 중 컨트롤러에 새 키가 적용된 뒤 호출. 성공/실패에 따라 bool/errno 응답.
 */
static void
rpc_bdev_nvme_set_keys_done(void *ctx, int status)
{
	struct spdk_jsonrpc_request *request = ctx;

	if (status != 0) {
		spdk_jsonrpc_send_error_response(request, status, spdk_strerror(-status));
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}
}

/*
 * [한국어]
 * rpc_bdev_nvme_set_keys - "bdev_nvme_set_keys" RPC: 컨트롤러의 DH-CHAP 키 갱신.
 *
 * 입력: {name, dhchap_key?, dhchap_ctrlr_key?}.
 * NVMe over TCP/RDMA에서 호스트↔컨트롤러 인증 키를 동적으로 회전(rotation)할 때 사용.
 * 비동기 완료 콜백: rpc_bdev_nvme_set_keys_done.
 */
static void
rpc_bdev_nvme_set_keys(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_set_keys req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_nvme_set_keys_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_set_keys_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		return;
	}

	/* [한국어] 내부 API에 위임. 비동기로 키 갱신 후 cb 호출. */
	rc = bdev_nvme_set_keys(req.name, req.dhchap_key, req.dhchap_ctrlr_key,
				rpc_bdev_nvme_set_keys_done, request);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
	free_rpc_bdev_nvme_set_keys(&req);
}
SPDK_RPC_REGISTER("bdev_nvme_set_keys", rpc_bdev_nvme_set_keys, SPDK_RPC_RUNTIME)
