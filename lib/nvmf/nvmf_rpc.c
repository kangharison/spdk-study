/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] NVMe-oF Target JSON-RPC 핸들러 (nvmf_rpc.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 SPDK NVMe-oF Target을 외부에서 제어하기 위한 JSON-RPC 핸들러를
 * 등록한다. nvmf_create_target/nvmf_create_subsystem/nvmf_subsystem_add_listener/
 * nvmf_subsystem_add_ns/nvmf_subsystem_add_host/nvmf_create_transport 등 RPC
 * 명령을 받으면 JSON 파라미터를 디코드한 뒤 lib/nvmf의 내부 API
 * (spdk_nvmf_subsystem_*, spdk_nvmf_tgt_*, spdk_nvmf_transport_*)를 호출하여
 * NVMe-oF target의 설정을 변경한다. 모든 핸들러는 SPDK_RPC_REGISTER 매크로로
 * RPC server에 등록되며, 응답은 spdk_jsonrpc_send_bool_response/
 * begin_result/end_result API로 전송한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   외부 (rpc.py / spdk_top / 사용자 스크립트)
 *     → /var/tmp/spdk.sock UNIX 도메인 소켓
 *     → lib/jsonrpc 서버 → spdk_jsonrpc_server_handle_request
 *     → 본 파일의 rpc_nvmf_*() 핸들러
 *     → spdk_nvmf_subsystem_pause()/resume() (subsystem state machine)
 *     → spdk_nvmf_subsystem_add_ns/listener/host (실제 변경)
 *     → 콜백으로 RPC 응답 송신
 * 실행 컨텍스트:
 *   RPC 핸들러는 SPDK가 RPC를 처리하기 위해 지정한 SPDK thread (보통 master
 *   reactor thread)에서 호출된다. subsystem state 변경은 비동기로 진행되므로
 *   대부분의 핸들러는 "context 객체를 calloc하여 콜백 체인으로 전달"하는 패턴을 사용.
 *   subsystem_pause(...) → cb (paused) → 실제 변경 → subsystem_resume(...) → cb (resumed)
 *   → spdk_jsonrpc_send_bool_response() 가 일반적 흐름이다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/nvmf/subsystem.c: spdk_nvmf_subsystem_create/destroy/add_ns/add_host/
 *   pause/resume 등 핵심 subsystem 라이프사이클 API. 본 파일은 이들의 wrapper.
 * - lib/nvmf/nvmf.c: spdk_nvmf_tgt_create/find_subsystem 등 target/subsystem
 *   레지스트리 API.
 * - lib/nvmf/transport.c, rdma.c, tcp.c: spdk_nvmf_transport_create/listen 등.
 * - lib/jsonrpc: spdk_jsonrpc_request, spdk_json_decode_object 등 JSON 디코딩.
 * - lib/nvmf/auth.c: nvmf_qpair_auth_dump (qpairs 조회 시 인증 상태 표시).
 * - lib/nvmf/mdns_server.c: nvmf_publish_mdns_prr / nvmf_tgt_stop_mdns_prr.
 * - lib/keyring: spdk_keyring_get_key/put_key (DH-HMAC-CHAP 키 참조).
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_nvmf_get_subsystems / dump_nvmf_subsystem: 모든 subsystem을 JSON 배열로 출력.
 * - rpc_nvmf_create_subsystem: NQN/SN/MN/cntlid 범위/passthrough/ana_reporting 옵션으로
 *   새 subsystem을 만들고 spdk_nvmf_subsystem_start로 활성화.
 * - rpc_nvmf_delete_subsystem: subsystem_stop → remove_listeners → destroy.
 * - rpc_nvmf_subsystem_add_listener / nvmf_rpc_listen_paused: subsystem을 pause한 뒤
 *   spdk_nvmf_tgt_listen_ext + spdk_nvmf_subsystem_add_listener_ext를 수행. ANA state
 *   초기값(ana_state) 설정도 가능.
 * - rpc_nvmf_subsystem_remove_listener: subsystem_pause → remove_listener →
 *   transport_stop_listen_async → resume.
 * - rpc_nvmf_subsystem_add_ns / nvmf_rpc_ns_paused: bdev_name으로 NS 추가.
 *   nguid/eui64/uuid/anagrpid/no_auto_visible/hide_metadata 옵션 처리.
 * - rpc_nvmf_subsystem_remove_ns: NSID 기반 NS 제거.
 * - rpc_nvmf_ns_add_host / rpc_nvmf_ns_remove_host: NS 가시성(visible) 설정.
 * - rpc_nvmf_subsystem_add_host / set_keys / remove_host / allow_any_host:
 *   호스트 ACL 및 DH-HMAC-CHAP 키 설정.
 * - rpc_nvmf_create_target / delete_target / get_targets: tgt 객체 라이프사이클.
 * - rpc_nvmf_create_transport / get_transports: transport 등록 및 옵션 디코드
 *   (max_io_qpairs_per_ctrlr, in_capsule_data_size, max_io_size, kas, masked_oncs 등).
 * - rpc_nvmf_get_stats: per-poll-group I/O 통계 dump (spdk_for_each_channel 순회).
 * - rpc_nvmf_subsystem_get_controllers / get_qpairs / get_listeners: subsystem을
 *   pause한 상태로 ctrlr/qpair/listener 목록을 출력 (paused 동안만 일관성 보장).
 * - rpc_nvmf_publish/stop_mdns_prr: mDNS 기반 NVMe-oF discovery 광고 on/off.
 * - 핵심 ctx 구조체들(struct nvmf_rpc_listener_ctx / nvmf_rpc_ns_ctx /
 *   nvmf_rpc_host_ctx / nvmf_rpc_create_transport_ctx 등)은 비동기 콜백 체인을
 *   따라가는 동안 RPC 요청 + 디코드된 파라미터를 들고 다니는 컨텍스트이다.
 */

#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/bit_array.h"
#include "spdk/config.h"

#include "spdk_internal/assert.h"

#include "nvmf_internal.h"

static int rpc_ana_state_parse(const char *str, enum spdk_nvme_ana_state *ana_state);

static int
json_write_hex_str(struct spdk_json_write_ctx *w, const void *data, size_t size)
{
	static const char __spdk_nonstring hex_char[16] = "0123456789ABCDEF";
	const uint8_t *buf = data;
	char *str, *out;
	int rc;

	str = malloc(size * 2 + 1);
	if (str == NULL) {
		return -1;
	}

	out = str;
	while (size--) {
		unsigned byte = *buf++;

		out[0] = hex_char[(byte >> 4) & 0xF];
		out[1] = hex_char[byte & 0xF];

		out += 2;
	}
	*out = '\0';

	rc = spdk_json_write_string(w, str);
	free(str);

	return rc;
}

static int
hex_nybble_to_num(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}

	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 0xA;
	}

	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 0xA;
	}

	return -1;
}

static int
hex_byte_to_num(const char *str)
{
	int hi, lo;

	hi = hex_nybble_to_num(str[0]);
	if (hi < 0) {
		return hi;
	}

	lo = hex_nybble_to_num(str[1]);
	if (lo < 0) {
		return lo;
	}

	return hi * 16 + lo;
}

static int
decode_hex_string_be(const char *str, uint8_t *out, size_t size)
{
	size_t i;

	/* Decode a string in "ABCDEF012345" format to its binary representation */
	for (i = 0; i < size; i++) {
		int num = hex_byte_to_num(str);

		if (num < 0) {
			/* Invalid hex byte or end of string */
			return -1;
		}

		out[i] = (uint8_t)num;
		str += 2;
	}

	if (i != size || *str != '\0') {
		/* Length mismatch */
		return -1;
	}

	return 0;
}

static int
decode_ns_nguid(const struct spdk_json_val *val, void *out)
{
	char *str = NULL;
	int rc;

	rc = spdk_json_decode_string(val, &str);
	if (rc == 0) {
		/* 16-byte NGUID */
		rc = decode_hex_string_be(str, out, 16);
	}

	free(str);
	return rc;
}

static int
decode_ns_eui64(const struct spdk_json_val *val, void *out)
{
	char *str = NULL;
	int rc;

	rc = spdk_json_decode_string(val, &str);
	if (rc == 0) {
		/* 8-byte EUI-64 */
		rc = decode_hex_string_be(str, out, 8);
	}

	free(str);
	return rc;
}

/* [한국어] nvmf_get_subsystems RPC의 입력 JSON을 디코드하는 구조체.
 * JSON 예: {"nqn": "nqn.2016-06.io.spdk:cnode1", "tgt_name": "nvmf_tgt"}
 * 두 필드 모두 optional이며, NULL이면 모든 subsystem (그리고 default tgt)이 대상. */
struct rpc_get_subsystem {
	char *nqn;
	/* [한국어] 조회할 subsystem NQN 문자열. NULL이면 모든 subsystem 출력.
	 * 설정자: spdk_json_decode_string. 읽는 자: rpc_nvmf_get_subsystems(). free: 함수 끝. */

	char *tgt_name;
	/* [한국어] target 이름 (NVMe-oF target은 다중 인스턴스 가능). NULL이면 default tgt.
	 * 설정자: spdk_json_decode_string. 읽는 자: spdk_nvmf_get_tgt(). free: 함수 끝. */
};

static const struct spdk_json_object_decoder rpc_nvmf_get_subsystems_decoders[] = {
	{"nqn", offsetof(struct rpc_get_subsystem, nqn), spdk_json_decode_string, true},
	{"tgt_name", offsetof(struct rpc_get_subsystem, tgt_name), spdk_json_decode_string, true},
};

static void
dump_nvmf_subsystem(struct spdk_json_write_ctx *w, struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_host			*host;
	struct spdk_nvmf_subsystem_listener	*listener;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));
	spdk_json_write_name(w, "subtype");
	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) {
		spdk_json_write_string(w, "NVMe");
	} else {
		spdk_json_write_string(w, "Discovery");
	}

	spdk_json_write_named_array_begin(w, "listen_addresses");

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (!nvmf_subsystem_listener_is_active(listener)) {
			continue;
		}

		spdk_json_write_object_begin(w);
		nvmf_transport_listen_dump_trid(listener->trid, w);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_named_bool(w, "allow_any_host",
				   spdk_nvmf_subsystem_get_allow_any_host(subsystem));

	spdk_json_write_named_array_begin(w, "hosts");

	for (host = spdk_nvmf_subsystem_get_first_host(subsystem); host != NULL;
	     host = spdk_nvmf_subsystem_get_next_host(subsystem, host)) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "nqn", spdk_nvmf_host_get_nqn(host));
		if (host->dhchap_key != NULL) {
			spdk_json_write_named_string(w, "dhchap_key",
						     spdk_key_get_name(host->dhchap_key));
		}
		if (host->dhchap_ctrlr_key != NULL) {
			spdk_json_write_named_string(w, "dhchap_ctrlr_key",
						     spdk_key_get_name(host->dhchap_ctrlr_key));
		}
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) {
		struct spdk_nvmf_ns *ns;
		struct spdk_nvmf_ns_opts ns_opts;
		uint32_t max_namespaces;

		spdk_json_write_named_string(w, "serial_number", spdk_nvmf_subsystem_get_sn(subsystem));

		spdk_json_write_named_string(w, "model_number", spdk_nvmf_subsystem_get_mn(subsystem));

		max_namespaces = spdk_nvmf_subsystem_get_max_namespaces(subsystem);
		if (max_namespaces != 0) {
			spdk_json_write_named_uint32(w, "max_namespaces", max_namespaces);
		}

		spdk_json_write_named_bool(w, "passthrough", subsystem->passthrough);

		spdk_json_write_named_uint32(w, "min_cntlid", spdk_nvmf_subsystem_get_min_cntlid(subsystem));
		spdk_json_write_named_uint32(w, "max_cntlid", spdk_nvmf_subsystem_get_max_cntlid(subsystem));

		spdk_json_write_named_array_begin(w, "namespaces");
		for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
		     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
			spdk_nvmf_ns_get_opts(ns, &ns_opts, sizeof(ns_opts));
			spdk_json_write_object_begin(w);
			spdk_json_write_named_int32(w, "nsid", spdk_nvmf_ns_get_id(ns));
			spdk_json_write_named_string(w, "bdev_name",
						     spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns)));
			/* NOTE: "name" is kept for compatibility only - new code should use bdev_name. */
			spdk_json_write_named_string(w, "name",
						     spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns)));

			if (!spdk_mem_all_zero(ns_opts.nguid, sizeof(ns_opts.nguid))) {
				spdk_json_write_name(w, "nguid");
				json_write_hex_str(w, ns_opts.nguid, sizeof(ns_opts.nguid));
			}

			if (!spdk_mem_all_zero(ns_opts.eui64, sizeof(ns_opts.eui64))) {
				spdk_json_write_name(w, "eui64");
				json_write_hex_str(w, ns_opts.eui64, sizeof(ns_opts.eui64));
			}

			if (!spdk_uuid_is_null(&ns_opts.uuid)) {
				spdk_json_write_named_uuid(w, "uuid", &ns_opts.uuid);
			}

			if (spdk_nvmf_subsystem_get_ana_reporting(subsystem)) {
				spdk_json_write_named_uint32(w, "anagrpid", ns_opts.anagrpid);
			}

			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);
	}
	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * rpc_nvmf_get_subsystems - "nvmf_get_subsystems" RPC 핸들러
 *
 * @request: RPC 요청 객체. 응답 송신에 사용.
 * @params: JSON 파라미터 (optional). nqn, tgt_name 두 필드를 디코드.
 *
 * 동작:
 *  1) params 디코드 → req(nqn/tgt_name).
 *  2) tgt 룩업 (없으면 INTERNAL_ERROR 응답).
 *  3) nqn이 주어지면 해당 subsystem만, 아니면 tgt의 모든 subsystem을 dump.
 *  4) dump_nvmf_subsystem()이 각 subsystem의 NQN/subtype/listeners/hosts/serial/
 *     model/namespaces 등을 JSON으로 직렬화.
 *
 * 호출 컨텍스트: RPC 서버 스레드 (보통 master reactor). subsystem state를 변경하지
 * 않으므로 pause 없이 곧장 읽는다 - 단 race로 인한 미세한 일관성 결여 가능.
 * 호출자 → 본 함수 → dump_nvmf_subsystem → spdk_json_write_*. 응답은 동기적.
 */
static void
rpc_nvmf_get_subsystems(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_get_subsystem req = { 0 };
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_subsystem *subsystem = NULL;
	struct spdk_nvmf_tgt *tgt;

	if (params) {
		if (spdk_json_decode_object(params, rpc_nvmf_get_subsystems_decoders,
					    SPDK_COUNTOF(rpc_nvmf_get_subsystems_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free(req.tgt_name);
		free(req.nqn);
		return;
	}

	if (req.nqn) {
		subsystem = spdk_nvmf_tgt_find_subsystem(tgt, req.nqn);
		if (!subsystem) {
			SPDK_ERRLOG("subsystem '%s' does not exist\n", req.nqn);
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
			free(req.tgt_name);
			free(req.nqn);
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (subsystem) {
		dump_nvmf_subsystem(w, subsystem);
	} else {
		for (subsystem = spdk_nvmf_subsystem_get_first(tgt); subsystem != NULL;
		     subsystem = spdk_nvmf_subsystem_get_next(subsystem)) {
			dump_nvmf_subsystem(w, subsystem);
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free(req.tgt_name);
	free(req.nqn);
}
SPDK_RPC_REGISTER("nvmf_get_subsystems", rpc_nvmf_get_subsystems, SPDK_RPC_RUNTIME)

/* [한국어] nvmf_create_subsystem RPC의 입력 파라미터.
 * JSON 예: {"nqn": "nqn.2016-06.io.spdk:cnode1", "serial_number": "SPDK00001",
 *           "model_number": "SPDK_Controller", "max_namespaces": 32,
 *           "allow_any_host": false, "ana_reporting": true,
 *           "min_cntlid": 1, "max_cntlid": 0xffef, "passthrough": false} */
struct rpc_subsystem_create {
	char *nqn;
	/* [한국어] 새 subsystem의 NVMe Qualified Name. 호스트가 Connect 시 매칭하는 키.
	 * 형식: nqn.YYYY-MM.<reverse-domain>:<unique> (NVMe 1.4 Section 7.9). */

	char *serial_number;
	/* [한국어] Identify Controller의 SN(Serial Number) 20바이트. NULL이면 기본값. */

	char *model_number;
	/* [한국어] Identify Controller의 MN(Model Number) 40바이트. NULL이면 기본값. */

	char *tgt_name;
	/* [한국어] 부속할 target 이름. NULL이면 default tgt. */

	uint32_t max_namespaces;
	/* [한국어] 이 subsystem이 보유할 수 있는 최대 NS 개수. 0이면 라이브러리 기본값. */

	bool allow_any_host;
	/* [한국어] true면 호스트 ACL을 비우고 모든 호스트 접속 허용. secure_channel과 호환 안 됨. */

	bool ana_reporting;
	/* [한국어] true면 Asymmetric Namespace Access (NVMe 1.4) 보고 활성화 - Identify
	 * Controller에서 cmic.anars=1, anatt 등이 보고되고 ANA log page 지원됨. */

	uint16_t min_cntlid;
	/* [한국어] 동적 cntlid 할당 시 사용할 [min, max] 범위 하한. */

	uint16_t max_cntlid;
	/* [한국어] cntlid 상한. 0xFFFF는 reserved (호스트의 dynamic 요청 표시). */

	uint64_t max_discard_size_kib;
	/* [한국어] DSM(Dataset Management) deallocate의 최대 크기 (KiB). cdata.dmrsl로 보고. */

	uint64_t max_write_zeroes_size_kib;
	/* [한국어] Write Zeroes 명령 최대 크기 (KiB). 4 정렬 + 2의 거듭제곱 요구. cdata.wzsl로 보고. */

	bool passthrough;
	/* [한국어] true면 사용자 정의 admin 명령을 첫 NS의 bdev_nvme로 우회. */

	bool enable_nssr;
	/* [한국어] true면 NVM Subsystem Reset(NSSR) 지원. CC.NSSR=4E564D65h ("NVMe") 시 reset. */
};

static const struct spdk_json_object_decoder rpc_nvmf_create_subsystem_decoders[] = {
	{"nqn", offsetof(struct rpc_subsystem_create, nqn), spdk_json_decode_string},
	{"serial_number", offsetof(struct rpc_subsystem_create, serial_number), spdk_json_decode_string, true},
	{"model_number", offsetof(struct rpc_subsystem_create, model_number), spdk_json_decode_string, true},
	{"tgt_name", offsetof(struct rpc_subsystem_create, tgt_name), spdk_json_decode_string, true},
	{"max_namespaces", offsetof(struct rpc_subsystem_create, max_namespaces), spdk_json_decode_uint32, true},
	{"allow_any_host", offsetof(struct rpc_subsystem_create, allow_any_host), spdk_json_decode_bool, true},
	{"ana_reporting", offsetof(struct rpc_subsystem_create, ana_reporting), spdk_json_decode_bool, true},
	{"min_cntlid", offsetof(struct rpc_subsystem_create, min_cntlid), spdk_json_decode_uint16, true},
	{"max_cntlid", offsetof(struct rpc_subsystem_create, max_cntlid), spdk_json_decode_uint16, true},
	{"max_discard_size_kib", offsetof(struct rpc_subsystem_create, max_discard_size_kib), spdk_json_decode_uint64, true},
	{"max_write_zeroes_size_kib", offsetof(struct rpc_subsystem_create, max_write_zeroes_size_kib), spdk_json_decode_uint64, true},
	{"passthrough", offsetof(struct rpc_subsystem_create, passthrough), spdk_json_decode_bool, true},
	{"enable_nssr", offsetof(struct rpc_subsystem_create, enable_nssr), spdk_json_decode_bool, true},
};

static void
rpc_nvmf_subsystem_started(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (!status) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Subsystem %s start failed",
						     subsystem->subnqn);
		spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	}
}

/*
 * [한국어]
 * rpc_nvmf_create_subsystem - "nvmf_create_subsystem" RPC 핸들러
 *
 * @request: RPC 요청. 응답은 spdk_nvmf_subsystem_start 콜백에서 송신.
 * @params: JSON. nqn(필수) + serial_number/model_number/tgt_name/max_namespaces/
 *          allow_any_host/ana_reporting/min_cntlid/max_cntlid/passthrough/
 *          enable_nssr (모두 optional).
 *
 * 단계:
 *  1) req calloc + 기본 cntlid range 설정 (NVMF_MIN/MAX_CNTLID).
 *  2) JSON decode → req.
 *  3) tgt 룩업, spdk_nvmf_subsystem_create(NQN, NVMe subtype) 호출.
 *  4) SN/MN/allow_any_host/ana_reporting/cntlid_range/discard/write_zeroes/
 *     passthrough/nssr 적용.
 *  5) spdk_nvmf_subsystem_start로 INACTIVE → ACTIVE 전환 (비동기, 콜백 =
 *     rpc_nvmf_subsystem_started).
 *  6) cleanup 라벨에서 임시 문자열들 free. 실패 시 destroy.
 *
 * 호출자 → spdk_nvmf_subsystem_start → 내부 콜백 → rpc_nvmf_subsystem_started
 *   → spdk_jsonrpc_send_bool_response.
 */
static void
rpc_nvmf_create_subsystem(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_subsystem_create *req;
	struct spdk_nvmf_subsystem *subsystem = NULL;
	struct spdk_nvmf_tgt *tgt;
	int rc = -1;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation failed");
		return;
	}
	req->min_cntlid = NVMF_MIN_CNTLID;
	req->max_cntlid = NVMF_MAX_CNTLID;

	if (spdk_json_decode_object(params, rpc_nvmf_create_subsystem_decoders,
				    SPDK_COUNTOF(rpc_nvmf_create_subsystem_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto cleanup;
	}

	tgt = spdk_nvmf_get_tgt(req->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find target %s\n", req->tgt_name);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Unable to find target %s", req->tgt_name);
		goto cleanup;
	}

	subsystem = spdk_nvmf_subsystem_create(tgt, req->nqn, SPDK_NVMF_SUBTYPE_NVME,
					       req->max_namespaces);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to create subsystem %s\n", req->nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Unable to create subsystem %s", req->nqn);
		goto cleanup;
	}

	if (req->serial_number) {
		if (spdk_nvmf_subsystem_set_sn(subsystem, req->serial_number)) {
			SPDK_ERRLOG("Subsystem %s: invalid serial number '%s'\n", req->nqn, req->serial_number);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Invalid SN %s", req->serial_number);
			goto cleanup;
		}
	}

	if (req->model_number) {
		if (spdk_nvmf_subsystem_set_mn(subsystem, req->model_number)) {
			SPDK_ERRLOG("Subsystem %s: invalid model number '%s'\n", req->nqn, req->model_number);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Invalid MN %s", req->model_number);
			goto cleanup;
		}
	}

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, req->allow_any_host);

	spdk_nvmf_subsystem_set_ana_reporting(subsystem, req->ana_reporting);

	if (spdk_nvmf_subsystem_set_cntlid_range(subsystem, req->min_cntlid, req->max_cntlid)) {
		SPDK_ERRLOG("Subsystem %s: invalid cntlid range [%u-%u]\n", req->nqn, req->min_cntlid,
			    req->max_cntlid);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid cntlid range [%u-%u]", req->min_cntlid, req->max_cntlid);
		goto cleanup;
	}

	subsystem->max_discard_size_kib = req->max_discard_size_kib;

	/* max_write_zeroes_size_kib must be aligned to 4 and power of 2 */
	if (req->max_write_zeroes_size_kib == 0 || (req->max_write_zeroes_size_kib > 2 &&
			spdk_u64_is_pow2(req->max_write_zeroes_size_kib))) {
		subsystem->max_write_zeroes_size_kib = req->max_write_zeroes_size_kib;
	} else {
		SPDK_ERRLOG("Subsystem %s: invalid max_write_zeroes_size_kib %"PRIu64"\n", req->nqn,
			    req->max_write_zeroes_size_kib);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid max_write_zeroes_size_kib %"PRIu64, req->max_write_zeroes_size_kib);
		goto cleanup;
	}

	subsystem->passthrough = req->passthrough;
	subsystem->nssr_enabled = req->enable_nssr;

	rc = spdk_nvmf_subsystem_start(subsystem,
				       rpc_nvmf_subsystem_started,
				       request);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to start subsystem");
	}

cleanup:
	free(req->nqn);
	free(req->tgt_name);
	free(req->serial_number);
	free(req->model_number);
	free(req);

	if (rc && subsystem) {
		spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	}
}
SPDK_RPC_REGISTER("nvmf_create_subsystem", rpc_nvmf_create_subsystem, SPDK_RPC_RUNTIME)

struct rpc_delete_subsystem {
	char *nqn;
	char *tgt_name;
};

static void
free_rpc_delete_subsystem(struct rpc_delete_subsystem *r)
{
	free(r->nqn);
	free(r->tgt_name);
}

static void
rpc_nvmf_subsystem_destroy_complete_cb(void *cb_arg)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_nvmf_subsystem_stopped(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	int rc;

	nvmf_subsystem_remove_all_listeners(subsystem, true);
	rc = spdk_nvmf_subsystem_destroy(subsystem, rpc_nvmf_subsystem_destroy_complete_cb, request);
	if (rc) {
		if (rc == -EINPROGRESS) {
			/* response will be sent in completion callback */
			return;
		} else {
			SPDK_ERRLOG("Subsystem destruction failed, rc %d\n", rc);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							     "Subsystem destruction failed, rc %d", rc);
			return;
		}
	}
	spdk_jsonrpc_send_bool_response(request, true);
}

static const struct spdk_json_object_decoder rpc_nvmf_delete_subsystem_decoders[] = {
	{"nqn", offsetof(struct rpc_delete_subsystem, nqn), spdk_json_decode_string},
	{"tgt_name", offsetof(struct rpc_delete_subsystem, tgt_name), spdk_json_decode_string, true},
};

/*
 * [한국어]
 * rpc_nvmf_delete_subsystem - "nvmf_delete_subsystem" RPC 핸들러
 *
 * @request: RPC 요청.
 * @params: JSON. nqn(필수), tgt_name(optional).
 *
 * subsystem을 stop → listener 제거 → destroy 순으로 비동기 정리.
 * spdk_nvmf_subsystem_stop가 -EBUSY를 반환하면 다른 state change가 진행 중인 경우.
 * 콜백 체인: subsystem_stop → rpc_nvmf_subsystem_stopped (listeners 제거 →
 *           subsystem_destroy → rpc_nvmf_subsystem_destroy_complete_cb → bool_response).
 */
static void
rpc_nvmf_delete_subsystem(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_delete_subsystem req = { 0 };
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	if (spdk_json_decode_object(params, rpc_nvmf_delete_subsystem_decoders,
				    SPDK_COUNTOF(rpc_nvmf_delete_subsystem_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.nqn == NULL) {
		SPDK_ERRLOG("missing name param\n");
		goto invalid;
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		goto invalid_custom_response;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, req.nqn);
	if (!subsystem) {
		goto invalid;
	}

	free_rpc_delete_subsystem(&req);

	rc = spdk_nvmf_subsystem_stop(subsystem,
				      rpc_nvmf_subsystem_stopped,
				      request);
	if (rc == -EBUSY) {
		SPDK_ERRLOG("Subsystem currently in another state change try again later.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Subsystem currently in another state change try again later.");
	} else if (rc != 0) {
		SPDK_ERRLOG("Unable to change state on subsystem. rc=%d\n", rc);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Unable to change state on subsystem. rc=%d", rc);
	}

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
invalid_custom_response:
	free_rpc_delete_subsystem(&req);
}
SPDK_RPC_REGISTER("nvmf_delete_subsystem", rpc_nvmf_delete_subsystem, SPDK_RPC_RUNTIME)

/* [한국어] listen_address JSON 객체의 디코드 구조체.
 * 형식: {"trtype": "TCP"|"RDMA"|"FC", "adrfam": "IPv4"|"IPv6"|...,
 *        "traddr": "192.168.1.10", "trsvcid": "4420"}
 * NVMe-oF 스펙의 TRID(Transport ID) 4종 필드에 대응. */
struct rpc_listen_address {
	char *trtype;
	/* [한국어] Transport Type 문자열. SPDK의 TCP/RDMA/FC/PCIE/VFIO_USER 등. */

	char *adrfam;
	/* [한국어] Address Family. IPv4/IPv6/IB/FC. NULL이면 IPv4 기본. */

	char *traddr;
	/* [한국어] Transport Address (호스트가 접속할 주소). 필수. IP 주소 또는 FC WWN 등. */

	char *trsvcid;
	/* [한국어] Transport Service ID (TCP/RDMA의 경우 포트 번호). NULL 가능. */
};

static const struct spdk_json_object_decoder rpc_nvmf_listen_address_decoders[] = {
	{"trtype", offsetof(struct rpc_listen_address, trtype), spdk_json_decode_string, true},
	{"adrfam", offsetof(struct rpc_listen_address, adrfam), spdk_json_decode_string, true},
	{"traddr", offsetof(struct rpc_listen_address, traddr), spdk_json_decode_string},
	{"trsvcid", offsetof(struct rpc_listen_address, trsvcid), spdk_json_decode_string, true},
};

static int
decode_rpc_listen_address(const struct spdk_json_val *val, void *out)
{
	struct rpc_listen_address *req = (struct rpc_listen_address *)out;

	return spdk_json_decode_object(val, rpc_nvmf_listen_address_decoders,
				       SPDK_COUNTOF(rpc_nvmf_listen_address_decoders), req);
}

static void
free_rpc_listen_address(struct rpc_listen_address *r)
{
	free(r->trtype);
	free(r->adrfam);
	free(r->traddr);
	free(r->trsvcid);
}

/* [한국어] listener 관련 RPC가 공통으로 사용하는 op 분기. nvmf_rpc_listen_paused
 * 콜백에서 ctx->op로 분기하여 add/remove/set_ana_state 중 하나를 수행한다. */
enum nvmf_rpc_listen_op {
	NVMF_RPC_LISTEN_ADD,            /* [한국어] subsystem에 새 listener를 추가 */
	NVMF_RPC_LISTEN_REMOVE,         /* [한국어] 기존 listener 제거 */
	NVMF_RPC_LISTEN_SET_ANA_STATE,  /* [한국어] listener의 ANA state 변경 */
};

/* [한국어] listener 변경 RPC의 비동기 콜백 체인을 따라가는 컨텍스트 객체.
 * subsystem은 변경 전 pause되어야 하므로 (active 상태에서 listener 변경 불가)
 * pause → 작업 → resume → response 의 4단계 콜백이 필요하며 본 ctx가 이 동안
 * 살아 있어야 한다. */
struct nvmf_rpc_listener_ctx {
	char				*nqn;
	/* [한국어] 대상 subsystem NQN. JSON에서 디코드된 문자열, 함수 끝에서 free. */

	char				*tgt_name;
	/* [한국어] 대상 target 이름. NULL이면 default tgt. */

	struct spdk_nvmf_tgt		*tgt;
	/* [한국어] tgt_name 룩업 결과. 콜백 체인 동안 유지. */

	struct spdk_nvmf_transport	*transport;
	/* [한국어] (REMOVE op에서) listener의 trtype에 대응하는 transport. stop_listen_async에 전달. */

	struct spdk_nvmf_subsystem	*subsystem;
	/* [한국어] 대상 subsystem 포인터. pause/resume 호출 대상. */

	struct rpc_listen_address	address;
	/* [한국어] JSON에서 디코드된 listen_address 필드 (trtype/adrfam/traddr/trsvcid). */

	char				*ana_state_str;
	/* [한국어] (SET_ANA_STATE op에서) "optimized"/"non_optimized"/"inaccessible" 중 하나. */

	enum spdk_nvme_ana_state	ana_state;
	/* [한국어] 위 문자열을 enum으로 파싱한 값. ANA log page에 보고된다. */

	uint32_t			anagrpid;
	/* [한국어] (SET_ANA_STATE op에서) 변경할 ANA group ID. 0이면 모든 group. */

	struct spdk_jsonrpc_request	*request;
	/* [한국어] 응답을 송신할 RPC 요청 객체. 콜백 체인 마지막에 send_*_response. */

	struct spdk_nvme_transport_id	trid;
	/* [한국어] address를 변환한 NVMe Transport ID 구조체 (rpc_listen_address_to_trid). */

	enum nvmf_rpc_listen_op		op;
	/* [한국어] ADD/REMOVE/SET_ANA_STATE - nvmf_rpc_listen_paused에서 분기 키로 사용. */

	bool				response_sent;
	/* [한국어] 콜백 체인 도중 에러로 응답을 이미 보냈는지 표시. resume 콜백이 중복 응답을 막음. */

	struct spdk_nvmf_listen_opts	opts;
	/* [한국어] transport-specific 옵션 (secure_channel, sock_impl, transport_specific JSON 등). */

	/* Hole at bytes 705-711 */
	uint8_t reserved1[7];
	/* [한국어] 다음 listener_opts 필드를 8-byte 정렬하기 위한 패딩. */

	/* Additional options for listener creation.
	 * Must be 8-byte aligned. */
	struct spdk_nvmf_listener_opts	listener_opts;
	/* [한국어] subsystem-specific listener 옵션 (secure_channel/ana_state/sock_impl).
	 * spdk_nvmf_subsystem_listener_opts_init로 초기화 후 JSON 디코드. */
};

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_add_listener_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), decode_rpc_listen_address},
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},
	{"secure_channel", offsetof(struct nvmf_rpc_listener_ctx, listener_opts.secure_channel), spdk_json_decode_bool, true},
	{"ana_state", offsetof(struct nvmf_rpc_listener_ctx, ana_state_str), spdk_json_decode_string, true},
	{"sock_impl", offsetof(struct nvmf_rpc_listener_ctx, listener_opts.sock_impl), spdk_json_decode_string, true},
};

static void
nvmf_rpc_listener_ctx_free(struct nvmf_rpc_listener_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->tgt_name);
	free_rpc_listen_address(&ctx->address);
	free(ctx->ana_state_str);
	free(ctx);
}

static void
nvmf_rpc_listen_resumed(struct spdk_nvmf_subsystem *subsystem,
			void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request;

	request = ctx->request;
	if (ctx->response_sent) {
		/* If an error occurred, the response has already been sent. */
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	nvmf_rpc_listener_ctx_free(ctx);

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
nvmf_rpc_subsystem_listen(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;

	if (status) {
		/* Destroy the listener that we just created. Ignore the error code because
		 * the RPC is failing already anyway. */
		spdk_nvmf_tgt_stop_listen(ctx->tgt, &ctx->trid);

		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}
static void
nvmf_rpc_stop_listen_async_done(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;

	if (status) {
		SPDK_ERRLOG("Unable to stop listener.\n");
		spdk_jsonrpc_send_error_response_fmt(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "error stopping listener: %d", status);
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}

static void
nvmf_rpc_set_ana_state_done(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;

	if (status) {
		SPDK_ERRLOG("Unable to set ANA state.\n");
		spdk_jsonrpc_send_error_response_fmt(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "error setting ANA state: %d", status);
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}

static void
nvmf_rpc_listen_paused(struct spdk_nvmf_subsystem *subsystem,
		       void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;
	int rc;

	switch (ctx->op) {
	case NVMF_RPC_LISTEN_ADD:
		if (nvmf_subsystem_find_listener(subsystem, &ctx->trid)) {
			SPDK_ERRLOG("Listener already exists\n");
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
			break;
		}

		rc = spdk_nvmf_tgt_listen_ext(ctx->tgt, &ctx->trid, &ctx->opts);
		if (rc) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
			break;
		}

		spdk_nvmf_subsystem_add_listener_ext(ctx->subsystem, &ctx->trid, nvmf_rpc_subsystem_listen, ctx,
						     &ctx->listener_opts);
		return;
	case NVMF_RPC_LISTEN_REMOVE:
		rc = spdk_nvmf_subsystem_remove_listener(subsystem, &ctx->trid);
		if (rc) {
			SPDK_ERRLOG("Unable to remove listener, rc %d\n", rc);
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
			break;
		}

		spdk_nvmf_transport_stop_listen_async(ctx->transport, &ctx->trid, subsystem,
						      nvmf_rpc_stop_listen_async_done, ctx);
		return;
	case NVMF_RPC_LISTEN_SET_ANA_STATE:
		spdk_nvmf_subsystem_set_ana_state(subsystem, &ctx->trid, ctx->ana_state, ctx->anagrpid,
						  nvmf_rpc_set_ana_state_done, ctx);
		return;
	default:
		SPDK_UNREACHABLE();
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}

		nvmf_rpc_listener_ctx_free(ctx);
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}

static int
rpc_listen_address_to_trid(const struct rpc_listen_address *address,
			   struct spdk_nvme_transport_id *trid)
{
	size_t len;

	memset(trid, 0, sizeof(*trid));

	if (spdk_nvme_transport_id_populate_trstring(trid, address->trtype)) {
		SPDK_ERRLOG("Invalid trtype string: %s\n", address->trtype);
		return -EINVAL;
	}

	if (spdk_nvme_transport_id_parse_trtype(&trid->trtype, address->trtype)) {
		SPDK_ERRLOG("Invalid trtype type: %s\n", address->trtype);
		return -EINVAL;
	}

	if (address->adrfam) {
		if (spdk_nvme_transport_id_parse_adrfam(&trid->adrfam, address->adrfam)) {
			SPDK_ERRLOG("Invalid adrfam: %s\n", address->adrfam);
			return -EINVAL;
		}
	} else {
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;
	}

	len = strlen(address->traddr);
	if (len > sizeof(trid->traddr) - 1) {
		SPDK_ERRLOG("Transport address longer than %zu characters: %s\n",
			    sizeof(trid->traddr) - 1, address->traddr);
		return -EINVAL;
	}
	memcpy(trid->traddr, address->traddr, len + 1);

	trid->trsvcid[0] = '\0';
	if (address->trsvcid) {
		len = strlen(address->trsvcid);
		if (len > sizeof(trid->trsvcid) - 1) {
			SPDK_ERRLOG("Transport service id longer than %zu characters: %s\n",
				    sizeof(trid->trsvcid) - 1, address->trsvcid);
			return -EINVAL;
		}
		memcpy(trid->trsvcid, address->trsvcid, len + 1);
	}

	return 0;
}

/*
 * [한국어]
 * rpc_nvmf_subsystem_add_listener - "nvmf_subsystem_add_listener" RPC 핸들러
 *
 * @request: RPC 요청.
 * @params: JSON. nqn(필수), listen_address(필수), tgt_name/secure_channel/ana_state/
 *          sock_impl (optional). 추가로 transport-specific 필드는 params 그대로
 *          opts.transport_specific으로 전달되어 transport가 자체 디코드.
 *
 * 흐름:
 *   ctx calloc → JSON decode → tgt 룩업 → subsystem 룩업 →
 *   address→trid 변환 → ANA state 파싱 → spdk_nvmf_subsystem_pause
 *     → (콜백) nvmf_rpc_listen_paused (op=ADD)
 *       → spdk_nvmf_tgt_listen_ext (transport에 listen socket 생성)
 *       → spdk_nvmf_subsystem_add_listener_ext
 *         → (콜백) nvmf_rpc_subsystem_listen
 *           → spdk_nvmf_subsystem_resume
 *             → (콜백) nvmf_rpc_listen_resumed → send_bool_response.
 *
 * 호출 컨텍스트: RPC 서버 스레드. 모든 콜백은 비동기 (다른 스레드에서 발화 가능).
 * ctx는 콜백 체인 끝에서 nvmf_rpc_listener_ctx_free로 해제된다.
 */
static void
rpc_nvmf_subsystem_add_listener(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct nvmf_rpc_listener_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	ctx->request = request;

	spdk_nvmf_subsystem_listener_opts_init(&ctx->listener_opts, sizeof(ctx->listener_opts));

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_subsystem_add_listener_decoders,
					    SPDK_COUNTOF(rpc_nvmf_subsystem_add_listener_decoders),
					    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
	ctx->tgt = tgt;

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->subsystem = subsystem;

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_ADD;
	spdk_nvmf_listen_opts_init(&ctx->opts, sizeof(ctx->opts));
	ctx->opts.transport_specific = params;
	if (spdk_nvmf_subsystem_get_allow_any_host(subsystem) && ctx->listener_opts.secure_channel) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Cannot establish secure channel, when 'allow_any_host' is set");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
	ctx->opts.secure_channel = ctx->listener_opts.secure_channel;

	if (ctx->ana_state_str) {
		if (rpc_ana_state_parse(ctx->ana_state_str, &ctx->ana_state)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			nvmf_rpc_listener_ctx_free(ctx);
			return;
		}
		ctx->listener_opts.ana_state = ctx->ana_state;
	}

	ctx->opts.sock_impl = ctx->listener_opts.sock_impl;

	rc = spdk_nvmf_subsystem_pause(subsystem, 0, nvmf_rpc_listen_paused, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_listener", rpc_nvmf_subsystem_add_listener,
		  SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_remove_listener_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), decode_rpc_listen_address},
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},
};

/*
 * [한국어]
 * rpc_nvmf_subsystem_remove_listener - "nvmf_subsystem_remove_listener" RPC
 *
 * @request: RPC 요청.
 * @params: JSON. nqn(필수), listen_address(필수), tgt_name(optional).
 *
 * subsystem에서 listener를 제거하고 transport listen socket도 닫는다 (해당 trid에
 * 더 이상 다른 subsystem이 접속하지 않을 경우). pause → remove_listener →
 * transport_stop_listen_async → resume → bool_response.
 */
static void
rpc_nvmf_subsystem_remove_listener(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct nvmf_rpc_listener_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_remove_listener_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_remove_listener_decoders),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
	ctx->tgt = tgt;

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->subsystem = subsystem;

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->transport = spdk_nvmf_tgt_get_transport(tgt, ctx->trid.trstring);
	if (!ctx->transport) {
		SPDK_ERRLOG("Unable to find %s transport. The transport must be created first also make sure it is properly registered.\n",
			    ctx->trid.trstring);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_REMOVE;

	rc = spdk_nvmf_subsystem_pause(subsystem, 0, nvmf_rpc_listen_paused, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_listener", rpc_nvmf_subsystem_remove_listener,
		  SPDK_RPC_RUNTIME);

struct nvmf_rpc_referral_ctx {
	char				*tgt_name;
	struct rpc_listen_address	address;
	bool				secure_channel;
	char				*subnqn;
};

static const struct spdk_json_object_decoder rpc_nvmf_discovery_add_referral_decoders[] = {
	{"address", offsetof(struct nvmf_rpc_referral_ctx, address), decode_rpc_listen_address},
	{"tgt_name", offsetof(struct nvmf_rpc_referral_ctx, tgt_name), spdk_json_decode_string, true},
	{"secure_channel", offsetof(struct nvmf_rpc_referral_ctx, secure_channel), spdk_json_decode_bool, true},
	{"subnqn", offsetof(struct nvmf_rpc_referral_ctx, subnqn), spdk_json_decode_string, true},
};

static void
nvmf_rpc_referral_ctx_free(struct nvmf_rpc_referral_ctx *ctx)
{
	free(ctx->tgt_name);
	free(ctx->subnqn);
	free_rpc_listen_address(&ctx->address);
}

static void
rpc_nvmf_discovery_add_referral(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct nvmf_rpc_referral_ctx ctx = {};
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvmf_tgt *tgt;
	struct spdk_nvmf_referral_opts opts = {};
	int rc;

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_discovery_add_referral_decoders,
					    SPDK_COUNTOF(rpc_nvmf_discovery_add_referral_decoders),
					    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	if (rpc_listen_address_to_trid(&ctx.address, &trid)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	if (ctx.subnqn != NULL) {
		rc = snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", ctx.subnqn);
		if (rc < 0 || (size_t)rc >= sizeof(trid.subnqn)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid subsystem NQN");
			nvmf_rpc_referral_ctx_free(&ctx);
			return;
		}
	}

	if ((trid.trtype == SPDK_NVME_TRANSPORT_TCP ||
	     trid.trtype == SPDK_NVME_TRANSPORT_RDMA) &&
	    !strlen(trid.trsvcid)) {
		SPDK_ERRLOG("Service ID is required.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Service ID is required.");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	opts.size = SPDK_SIZEOF(&opts, allow_any_host);
	opts.trid = trid;
	opts.secure_channel = ctx.secure_channel;
	opts.allow_any_host = true;

	rc = spdk_nvmf_tgt_add_referral(tgt, &opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	nvmf_rpc_referral_ctx_free(&ctx);

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("nvmf_discovery_add_referral", rpc_nvmf_discovery_add_referral, SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_nvmf_discovery_remove_referral_decoders[] = {
	{"address", offsetof(struct nvmf_rpc_referral_ctx, address), decode_rpc_listen_address},
	{"tgt_name", offsetof(struct nvmf_rpc_referral_ctx, tgt_name), spdk_json_decode_string, true},
	{"subnqn", offsetof(struct nvmf_rpc_referral_ctx, subnqn), spdk_json_decode_string, true},
};

static void
rpc_nvmf_discovery_remove_referral(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct nvmf_rpc_referral_ctx ctx = {};
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvmf_referral_opts opts = {};
	struct spdk_nvmf_tgt *tgt;
	int rc;

	if (spdk_json_decode_object(params, rpc_nvmf_discovery_remove_referral_decoders,
				    SPDK_COUNTOF(rpc_nvmf_discovery_remove_referral_decoders),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	if (rpc_listen_address_to_trid(&ctx.address, &trid)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	if (ctx.subnqn != NULL) {
		rc = snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", ctx.subnqn);
		if (rc < 0 || (size_t)rc >= sizeof(trid.subnqn)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid subsystem NQN");
			nvmf_rpc_referral_ctx_free(&ctx);
			return;
		}
	}

	opts.size = SPDK_SIZEOF(&opts, secure_channel);
	opts.trid = trid;

	if (spdk_nvmf_tgt_remove_referral(tgt, &opts)) {
		SPDK_ERRLOG("Failed to remove referral.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to remove a referral.");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	nvmf_rpc_referral_ctx_free(&ctx);

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("nvmf_discovery_remove_referral", rpc_nvmf_discovery_remove_referral,
		  SPDK_RPC_RUNTIME);

static void
dump_nvmf_referral(struct spdk_json_write_ctx *w,
		   struct spdk_nvmf_referral *referral)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_object_begin(w, "address");
	nvmf_transport_listen_dump_trid(&referral->trid, w);
	spdk_json_write_object_end(w);
	spdk_json_write_named_bool(w, "secure_channel",
				   referral->entry.treq.secure_channel == SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED);
	spdk_json_write_named_string(w, "subnqn", referral->trid.subnqn);

	spdk_json_write_object_end(w);
}

struct rpc_get_referrals_ctx {
	char *tgt_name;
};

static const struct spdk_json_object_decoder rpc_nvmf_discovery_get_referrals_decoders[] = {
	{"tgt_name", offsetof(struct rpc_get_referrals_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
free_rpc_get_referrals_ctx(struct rpc_get_referrals_ctx *ctx)
{
	free(ctx->tgt_name);
	free(ctx);
}

static void
rpc_nvmf_discovery_get_referrals(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_get_referrals_ctx *ctx;
	struct spdk_nvmf_tgt *tgt;
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_referral *referral;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	if (params) {
		if (spdk_json_decode_object(params, rpc_nvmf_discovery_get_referrals_decoders,
					    SPDK_COUNTOF(rpc_nvmf_discovery_get_referrals_decoders),
					    ctx)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			free_rpc_get_referrals_ctx(ctx);
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target");
		free_rpc_get_referrals_ctx(ctx);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(referral, &tgt->referrals, link) {
		dump_nvmf_referral(w, referral);
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

	free_rpc_get_referrals_ctx(ctx);
}
SPDK_RPC_REGISTER("nvmf_discovery_get_referrals", rpc_nvmf_discovery_get_referrals,
		  SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_listener_set_ana_state_decoders[] =
{
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), decode_rpc_listen_address},
	{"ana_state", offsetof(struct nvmf_rpc_listener_ctx, ana_state_str), spdk_json_decode_string},
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},
	{"anagrpid", offsetof(struct nvmf_rpc_listener_ctx, anagrpid), spdk_json_decode_uint32, true},
};

static int
rpc_ana_state_parse(const char *str, enum spdk_nvme_ana_state *ana_state)
{
	if (ana_state == NULL || str == NULL) {
		return -EINVAL;
	}

	if (strcasecmp(str, "optimized") == 0) {
		*ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	} else if (strcasecmp(str, "non_optimized") == 0) {
		*ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	} else if (strcasecmp(str, "inaccessible") == 0) {
		*ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;
	} else {
		return -ENOENT;
	}

	return 0;
}

static void
rpc_nvmf_subsystem_listener_set_ana_state(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct nvmf_rpc_listener_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_listener_set_ana_state_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_listener_set_ana_state_decoders),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->tgt = tgt;

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Unable to find subsystem with NQN %s",
						     ctx->nqn);
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->subsystem = subsystem;

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	if (rpc_ana_state_parse(ctx->ana_state_str, &ctx->ana_state)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_SET_ANA_STATE;

	if (spdk_nvmf_subsystem_pause(subsystem, 0, nvmf_rpc_listen_paused, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_listener_set_ana_state",
		  rpc_nvmf_subsystem_listener_set_ana_state, SPDK_RPC_RUNTIME);

/* [한국어] nvmf_subsystem_add_ns RPC의 "namespace" sub-object를 디코드한다.
 * NS는 NVMe-oF에서 호스트가 보는 LBA 공간 단위이며, SPDK는 NS를 bdev로 백업한다.
 * 즉 호스트의 LBA read/write → bdev I/O로 변환된다.
 * JSON 예: {"bdev_name": "Malloc0", "nsid": 1, "nguid": "ABCD...",
 *           "uuid": "...", "anagrpid": 1, "no_auto_visible": false} */
struct nvmf_rpc_ns_params {
	char *bdev_name;
	/* [한국어] 이 NS의 백엔드 bdev 이름. NVMe Read/Write가 이 bdev로 라우팅됨. 필수. */

	char *ptpl_file;
	/* [한국어] PR(Persistent Reservation) Through Power Loss 상태 저장 파일 경로.
	 * NULL이면 reservation persistence 비활성. */

	uint32_t nsid;
	/* [한국어] Namespace ID. 0이면 자동 할당 (subsystem이 다음 빈 ID 부여). */

	char nguid[16];
	/* [한국어] NS Globally Unique Identifier (NVMe 1.4 5.15.2.2). nguid_str을 hex 디코드한 16바이트.
	 * 0이면 sentinel - Identify NS의 NGUID 필드에 보고하지 않음. */

	char eui64[8];
	/* [한국어] IEEE EUI-64 형식의 NS 고유 ID. 8바이트 hex. 0이면 미사용. */

	struct spdk_uuid uuid;
	/* [한국어] NS UUID. spdk_uuid_is_null이면 보고하지 않음. NS ID Descriptor에 포함. */

	uint32_t anagrpid;
	/* [한국어] 이 NS가 속할 ANA group ID. 같은 group의 NS는 동일 ana_state 공유. */

	bool no_auto_visible;
	/* [한국어] true면 NS를 자동으로 host에 visible하지 않음. nvmf_ns_add_host로 명시 부여 필요. */

	bool hide_metadata;
	/* [한국어] true면 metadata 영역(DIF/PI)을 호스트에 숨김 (insert/strip은 SPDK가 수행). */
};

static const struct spdk_json_object_decoder rpc_nvmf_namespace_decoders[] = {
	{"nsid", offsetof(struct nvmf_rpc_ns_params, nsid), spdk_json_decode_uint32, true},
	{"bdev_name", offsetof(struct nvmf_rpc_ns_params, bdev_name), spdk_json_decode_string},
	{"ptpl_file", offsetof(struct nvmf_rpc_ns_params, ptpl_file), spdk_json_decode_string, true},
	{"nguid", offsetof(struct nvmf_rpc_ns_params, nguid), decode_ns_nguid, true},
	{"eui64", offsetof(struct nvmf_rpc_ns_params, eui64), decode_ns_eui64, true},
	{"uuid", offsetof(struct nvmf_rpc_ns_params, uuid), spdk_json_decode_uuid, true},
	{"anagrpid", offsetof(struct nvmf_rpc_ns_params, anagrpid), spdk_json_decode_uint32, true},
	{"no_auto_visible", offsetof(struct nvmf_rpc_ns_params, no_auto_visible), spdk_json_decode_bool, true},
	{"hide_metadata", offsetof(struct nvmf_rpc_ns_params, hide_metadata), spdk_json_decode_bool, true},
};

static int
decode_rpc_ns_params(const struct spdk_json_val *val, void *out)
{
	struct nvmf_rpc_ns_params *ns_params = out;

	return spdk_json_decode_object(val, rpc_nvmf_namespace_decoders,
				       SPDK_COUNTOF(rpc_nvmf_namespace_decoders),
				       ns_params);
}

/* [한국어] nvmf_subsystem_add_ns의 비동기 콜백 컨텍스트.
 * pause → add_ns → resume의 4단계를 거치므로 ctx가 그동안 살아 있어야 한다. */
struct nvmf_rpc_ns_ctx {
	char *nqn;
	/* [한국어] 대상 subsystem NQN (필수). */

	char *tgt_name;
	/* [한국어] target 이름 (NULL이면 default). */

	struct nvmf_rpc_ns_params ns_params;
	/* [한국어] JSON에서 디코드된 namespace 옵션들 (위 구조체 참조). */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답을 송신할 RPC 요청 객체. */

	const struct spdk_json_val *params;
	/* [한국어] 원본 JSON 파라미터. transport-specific 옵션을 NS 추가 시 ns_opts.transport_specific
	 * 으로 그대로 전달해 transport가 자체 디코드하도록 하기 위함. */

	bool response_sent;
	/* [한국어] 콜백 도중 에러로 응답을 이미 보냈는지 표시 (resume 콜백 중복 응답 방지). */
};

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_add_ns_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_ns_ctx, nqn), spdk_json_decode_string},
	{"namespace", offsetof(struct nvmf_rpc_ns_ctx, ns_params), decode_rpc_ns_params},
	{"tgt_name", offsetof(struct nvmf_rpc_ns_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
nvmf_rpc_ns_ctx_free(struct nvmf_rpc_ns_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->tgt_name);
	free(ctx->ns_params.bdev_name);
	free(ctx->ns_params.ptpl_file);
	free(ctx);
}

static void
nvmf_rpc_ns_failback_resumed(struct spdk_nvmf_subsystem *subsystem,
			     void *cb_arg, int status)
{
	struct nvmf_rpc_ns_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;

	if (status) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to add ns, subsystem in invalid state");
	} else {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to add ns, subsystem in active state");
	}

	nvmf_rpc_ns_ctx_free(ctx);
}

static void
nvmf_rpc_ns_resumed(struct spdk_nvmf_subsystem *subsystem,
		    void *cb_arg, int status)
{
	struct nvmf_rpc_ns_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;
	uint32_t nsid = ctx->ns_params.nsid;
	bool response_sent = ctx->response_sent;
	struct spdk_json_write_ctx *w;
	int rc;

	/* The case where the call to add the namespace was successful, but the subsystem couldn't be resumed. */
	if (status && !ctx->response_sent) {
		rc = spdk_nvmf_subsystem_remove_ns(subsystem, nsid);
		if (rc != 0) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Unable to add ns, subsystem in invalid state");
			nvmf_rpc_ns_ctx_free(ctx);
			return;
		}

		rc = spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_ns_failback_resumed, ctx);
		if (rc != 0) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
			nvmf_rpc_ns_ctx_free(ctx);
			return;
		}

		return;
	}

	nvmf_rpc_ns_ctx_free(ctx);

	if (response_sent) {
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_uint32(w, nsid);
	spdk_jsonrpc_end_result(request, w);
}

static void
nvmf_rpc_ns_paused(struct spdk_nvmf_subsystem *subsystem,
		   void *cb_arg, int status)
{
	struct nvmf_rpc_ns_ctx *ctx = cb_arg;
	struct spdk_nvmf_ns_opts ns_opts;

	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = ctx->ns_params.nsid;
	ns_opts.transport_specific = ctx->params;

	SPDK_STATIC_ASSERT(sizeof(ns_opts.nguid) == sizeof(ctx->ns_params.nguid), "size mismatch");
	memcpy(ns_opts.nguid, ctx->ns_params.nguid, sizeof(ns_opts.nguid));

	SPDK_STATIC_ASSERT(sizeof(ns_opts.eui64) == sizeof(ctx->ns_params.eui64), "size mismatch");
	memcpy(ns_opts.eui64, ctx->ns_params.eui64, sizeof(ns_opts.eui64));

	if (!spdk_uuid_is_null(&ctx->ns_params.uuid)) {
		ns_opts.uuid = ctx->ns_params.uuid;
	}

	ns_opts.anagrpid = ctx->ns_params.anagrpid;
	ns_opts.no_auto_visible = ctx->ns_params.no_auto_visible;
	ns_opts.hide_metadata = ctx->ns_params.hide_metadata;

	ctx->ns_params.nsid = spdk_nvmf_subsystem_add_ns_ext(subsystem, ctx->ns_params.bdev_name,
			      &ns_opts, sizeof(ns_opts),
			      ctx->ns_params.ptpl_file);
	if (ctx->ns_params.nsid == 0) {
		SPDK_ERRLOG("Unable to add namespace\n");
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
		goto resume;
	}

resume:
	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_ns_resumed, ctx)) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ns_ctx_free(ctx);
	}
}

/*
 * [한국어]
 * rpc_nvmf_subsystem_add_ns - "nvmf_subsystem_add_ns" RPC 핸들러
 *
 * @request: RPC 요청.
 * @params: JSON. nqn(필수), namespace(필수, sub-object), tgt_name(optional).
 *
 * 흐름:
 *   ctx calloc → JSON decode (relaxed: 알 수 없는 키 무시) → tgt 룩업 →
 *   subsystem 룩업 → spdk_nvmf_subsystem_pause(nsid)
 *     → (콜백) nvmf_rpc_ns_paused: ns_opts 채우고 spdk_nvmf_subsystem_add_ns_ext 호출
 *       → (성공 시) nsid 저장, (실패 시) response_sent=true.
 *       → spdk_nvmf_subsystem_resume
 *         → (콜백) nvmf_rpc_ns_resumed: 성공이면 nsid를 응답 JSON으로 송신,
 *            실패면 nvmf_rpc_ns_failback_resumed 경유 에러 응답.
 *
 * 즉, 호스트의 NVMe Read/Write가 들어가기 전에 SPDK는 ns->bdev 매핑을 확립.
 * 이후 nvmf_ctrlr_process_io_cmd가 nsid를 통해 이 NS를 룩업한다.
 */
static void
rpc_nvmf_subsystem_add_ns(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct nvmf_rpc_ns_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_subsystem_add_ns_decoders,
					    SPDK_COUNTOF(rpc_nvmf_subsystem_add_ns_decoders), ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_ctx_free(ctx);
		return;
	}

	ctx->request = request;
	ctx->params = params;
	ctx->response_sent = false;

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_ns_ctx_free(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_ctx_free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_pause(subsystem, ctx->ns_params.nsid, nvmf_rpc_ns_paused, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ns_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_ns", rpc_nvmf_subsystem_add_ns, SPDK_RPC_RUNTIME)

struct nvmf_rpc_ana_group_ctx {
	char *nqn;
	char *tgt_name;
	uint32_t nsid;
	uint32_t anagrpid;

	struct spdk_jsonrpc_request *request;
	bool response_sent;
};

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_set_ns_ana_group_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_ana_group_ctx, nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct nvmf_rpc_ana_group_ctx, nsid), spdk_json_decode_uint32},
	{"anagrpid", offsetof(struct nvmf_rpc_ana_group_ctx, anagrpid), spdk_json_decode_uint32},
	{"tgt_name", offsetof(struct nvmf_rpc_ana_group_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
nvmf_rpc_ana_group_ctx_free(struct nvmf_rpc_ana_group_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->tgt_name);
	free(ctx);
}

static void
nvmf_rpc_anagrpid_resumed(struct spdk_nvmf_subsystem *subsystem,
			  void *cb_arg, int status)
{
	struct nvmf_rpc_ana_group_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;
	bool response_sent = ctx->response_sent;

	nvmf_rpc_ana_group_ctx_free(ctx);

	if (response_sent) {
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
nvmf_rpc_ana_group(struct spdk_nvmf_subsystem *subsystem,
		   void *cb_arg, int status)
{
	struct nvmf_rpc_ana_group_ctx *ctx = cb_arg;
	int rc;

	rc = spdk_nvmf_subsystem_set_ns_ana_group(subsystem, ctx->nsid, ctx->anagrpid);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to change ANA group ID\n");
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_anagrpid_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_ana_group_ctx_free(ctx);
	}
}

static void
rpc_nvmf_subsystem_set_ns_ana_group(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct nvmf_rpc_ana_group_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_set_ns_ana_group_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_set_ns_ana_group_decoders), ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ana_group_ctx_free(ctx);
		return;
	}

	ctx->request = request;
	ctx->response_sent = false;

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_ana_group_ctx_free(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ana_group_ctx_free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_pause(subsystem, ctx->nsid, nvmf_rpc_ana_group, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ana_group_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_set_ns_ana_group", rpc_nvmf_subsystem_set_ns_ana_group,
		  SPDK_RPC_RUNTIME)

struct nvmf_rpc_remove_ns_ctx {
	char *nqn;
	char *tgt_name;
	uint32_t nsid;

	struct spdk_jsonrpc_request *request;
	bool response_sent;
};

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_remove_ns_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_remove_ns_ctx, nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct nvmf_rpc_remove_ns_ctx, nsid), spdk_json_decode_uint32},
	{"tgt_name", offsetof(struct nvmf_rpc_remove_ns_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
nvmf_rpc_remove_ns_ctx_free(struct nvmf_rpc_remove_ns_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->tgt_name);
	free(ctx);
}

static void
nvmf_rpc_remove_ns_resumed(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct nvmf_rpc_remove_ns_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;
	bool response_sent = ctx->response_sent;

	nvmf_rpc_remove_ns_ctx_free(ctx);

	if (response_sent) {
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
nvmf_rpc_remove_ns_paused(struct spdk_nvmf_subsystem *subsystem,
			  void *cb_arg, int status)
{
	struct nvmf_rpc_remove_ns_ctx *ctx = cb_arg;
	int ret;

	ret = spdk_nvmf_subsystem_remove_ns(subsystem, ctx->nsid);
	if (ret < 0) {
		SPDK_ERRLOG("Unable to remove namespace ID %u\n", ctx->nsid);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_remove_ns_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
		nvmf_rpc_remove_ns_ctx_free(ctx);
	}
}

/*
 * [한국어]
 * rpc_nvmf_subsystem_remove_ns - "nvmf_subsystem_remove_ns" RPC 핸들러
 *
 * @request: RPC 요청.
 * @params: JSON. nqn(필수), nsid(필수), tgt_name(optional).
 *
 * pause → spdk_nvmf_subsystem_remove_ns(nsid) → resume → bool_response 흐름.
 * NS 제거 시 connected ctrlr들에는 NS Attribute Change AEN이 송신될 수 있다.
 */
static void
rpc_nvmf_subsystem_remove_ns(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct nvmf_rpc_remove_ns_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_remove_ns_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_remove_ns_decoders),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_remove_ns_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_remove_ns_ctx_free(ctx);
		return;
	}

	ctx->request = request;
	ctx->response_sent = false;

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_remove_ns_ctx_free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_pause(subsystem, ctx->nsid, nvmf_rpc_remove_ns_paused, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_remove_ns_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_ns", rpc_nvmf_subsystem_remove_ns, SPDK_RPC_RUNTIME)

struct nvmf_rpc_ns_visible_ctx {
	struct spdk_jsonrpc_request *request;
	char *nqn;
	uint32_t nsid;
	char *host;
	char *tgt_name;
	bool visible;
	bool response_sent;
};

static const struct spdk_json_object_decoder rpc_nvmf_ns_add_host_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_ns_visible_ctx, nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct nvmf_rpc_ns_visible_ctx, nsid), spdk_json_decode_uint32},
	{"host", offsetof(struct nvmf_rpc_ns_visible_ctx, host), spdk_json_decode_string},
	{"tgt_name", offsetof(struct nvmf_rpc_ns_visible_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
nvmf_rpc_ns_visible_ctx_free(struct nvmf_rpc_ns_visible_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->host);
	free(ctx->tgt_name);
	free(ctx);
}

static void
nvmf_rpc_ns_visible_resumed(struct spdk_nvmf_subsystem *subsystem,
			    void *cb_arg, int status)
{
	struct nvmf_rpc_ns_visible_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;
	bool response_sent = ctx->response_sent;

	nvmf_rpc_ns_visible_ctx_free(ctx);

	if (!response_sent) {
		spdk_jsonrpc_send_bool_response(request, true);
	}
}

static void
nvmf_rpc_ns_visible_paused(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct nvmf_rpc_ns_visible_ctx *ctx = cb_arg;
	int ret;

	if (ctx->visible) {
		ret = spdk_nvmf_ns_add_host(subsystem, ctx->nsid, ctx->host, 0);
	} else {
		ret = spdk_nvmf_ns_remove_host(subsystem, ctx->nsid, ctx->host, 0);
	}
	if (ret < 0) {
		SPDK_ERRLOG("Unable to add/remove %s to namespace ID %u\n", ctx->host, ctx->nsid);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_ns_visible_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
		nvmf_rpc_ns_visible_ctx_free(ctx);
	}
}

static void
nvmf_rpc_ns_visible(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params,
		    bool visible)
{
	struct nvmf_rpc_ns_visible_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	ctx->visible = visible;

	if (spdk_json_decode_object(params, rpc_nvmf_ns_add_host_decoders,
				    SPDK_COUNTOF(rpc_nvmf_ns_add_host_decoders),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_visible_ctx_free(ctx);
		return;
	}
	ctx->request = request;

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_ns_visible_ctx_free(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_visible_ctx_free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_pause(subsystem, ctx->nsid, nvmf_rpc_ns_visible_paused, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ns_visible_ctx_free(ctx);
	}
}

static void
rpc_nvmf_ns_add_host(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	nvmf_rpc_ns_visible(request, params, true);
}
SPDK_RPC_REGISTER("nvmf_ns_add_host", rpc_nvmf_ns_add_host, SPDK_RPC_RUNTIME)

static void
rpc_nvmf_ns_remove_host(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	nvmf_rpc_ns_visible(request, params, false);
}
SPDK_RPC_REGISTER("nvmf_ns_remove_host", rpc_nvmf_ns_remove_host, SPDK_RPC_RUNTIME)

/* [한국어] 호스트 ACL/key 변경 RPC들이 공통으로 쓰는 컨텍스트.
 * nvmf_subsystem_add_host / remove_host / set_keys / allow_any_host에서 사용.
 * JSON 예: {"nqn": "...", "host": "nqn.2014-08.org.nvmexpress:uuid:abc",
 *           "dhchap_key": "key0", "dhchap_ctrlr_key": "key1"} */
struct nvmf_rpc_host_ctx {
	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답 송신용 RPC 요청 객체. */

	char *nqn;
	/* [한국어] 대상 subsystem NQN. */

	char *host;
	/* [한국어] 추가/제거할 host NQN (호스트 측 NQN). */

	char *tgt_name;
	/* [한국어] target 이름 (NULL=default). */

	char *dhchap_key;
	/* [한국어] DH-HMAC-CHAP 인증 시 호스트→컨트롤러 challenge 응답 키 이름.
	 * spdk_keyring_get_key로 keyring에서 룩업되어 spdk_key*에 변환됨. */

	char *dhchap_ctrlr_key;
	/* [한국어] DH-HMAC-CHAP 양방향 인증의 컨트롤러→호스트 응답 키 이름.
	 * 양방향 인증을 원할 때만 지정. */

	bool allow_any_host;
	/* [한국어] (allow_any_host RPC에서) 호스트 ACL을 비활성화할지 여부.
	 * true면 모든 호스트 connect 허용. */
};

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_add_host_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_host_ctx, nqn), spdk_json_decode_string},
	{"host", offsetof(struct nvmf_rpc_host_ctx, host), spdk_json_decode_string},
	{"tgt_name", offsetof(struct nvmf_rpc_host_ctx, tgt_name), spdk_json_decode_string, true},
	{"dhchap_key", offsetof(struct nvmf_rpc_host_ctx, dhchap_key), spdk_json_decode_string, true},
	{"dhchap_ctrlr_key", offsetof(struct nvmf_rpc_host_ctx, dhchap_ctrlr_key), spdk_json_decode_string, true},
};

static void
nvmf_rpc_host_ctx_free(struct nvmf_rpc_host_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->host);
	free(ctx->tgt_name);
	free(ctx->dhchap_key);
	free(ctx->dhchap_ctrlr_key);
}

/*
 * [한국어]
 * rpc_nvmf_subsystem_add_host - "nvmf_subsystem_add_host" RPC 핸들러
 *
 * @request: RPC 요청.
 * @params: JSON. nqn(필수), host(필수), tgt_name/dhchap_key/dhchap_ctrlr_key (optional).
 *
 * subsystem의 host ACL에 호스트 NQN을 추가한다. 이후 그 host NQN을 가진
 * Connect만 nvmf_qpair_access_allowed에서 통과한다 (allow_any_host=false인 경우).
 * dhchap_key/dhchap_ctrlr_key가 주어지면 keyring에서 spdk_key*를 룩업하여
 * spdk_nvmf_subsystem_add_host_ext 옵션으로 전달, DH-HMAC-CHAP 인증을 활성화.
 *
 * 본 RPC는 동기적으로 처리되어 즉시 spdk_jsonrpc_send_bool_response 응답.
 * out 라벨에서 keyring put_key + ctx free 수행.
 */
static void
rpc_nvmf_subsystem_add_host(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx ctx = {};
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_host_opts opts = {};
	struct spdk_nvmf_tgt *tgt;
	struct spdk_key *key = NULL, *ckey = NULL;
	int rc;

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_subsystem_add_host_decoders,
					    SPDK_COUNTOF(rpc_nvmf_subsystem_add_host_decoders),
					    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		goto out;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx.nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx.nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	if (ctx.dhchap_key != NULL) {
		key = spdk_keyring_get_key(ctx.dhchap_key);
		if (key == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP key: %s\n", ctx.dhchap_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}

	if (ctx.dhchap_ctrlr_key != NULL) {
		ckey = spdk_keyring_get_key(ctx.dhchap_ctrlr_key);
		if (ckey == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP ctrlr key: %s\n",
				    ctx.dhchap_ctrlr_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}

	opts.size = SPDK_SIZEOF(&opts, dhchap_ctrlr_key);
	opts.params = params;
	opts.dhchap_key = key;
	opts.dhchap_ctrlr_key = ckey;
	rc = spdk_nvmf_subsystem_add_host_ext(subsystem, ctx.host, &opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);
out:
	spdk_keyring_put_key(ckey);
	spdk_keyring_put_key(key);
	nvmf_rpc_host_ctx_free(&ctx);
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_host", rpc_nvmf_subsystem_add_host, SPDK_RPC_RUNTIME)

static void
rpc_nvmf_subsystem_remove_host_done(void *_ctx, int status)
{
	struct nvmf_rpc_host_ctx *ctx = _ctx;

	spdk_jsonrpc_send_bool_response(ctx->request, true);
	nvmf_rpc_host_ctx_free(ctx);
	free(ctx);
}

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_remove_host_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_host_ctx, nqn), spdk_json_decode_string},
	{"host", offsetof(struct nvmf_rpc_host_ctx, host), spdk_json_decode_string},
	{"tgt_name", offsetof(struct nvmf_rpc_host_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_subsystem_remove_host(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Unable to allocate context to perform RPC\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_remove_host_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_remove_host_decoders),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_remove_host(subsystem, ctx->host);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_disconnect_host(subsystem, ctx->host,
			rpc_nvmf_subsystem_remove_host_done,
			ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_host", rpc_nvmf_subsystem_remove_host,
		  SPDK_RPC_RUNTIME)

static void
rpc_nvmf_subsystem_set_keys(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx ctx = {};
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_subsystem_key_opts opts = {};
	struct spdk_nvmf_tgt *tgt;
	struct spdk_key *key = NULL, *ckey = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_add_host_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_add_host_decoders), &ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Invalid parameters");
		goto out;
	}
	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx.nqn);
	if (!subsystem) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	if (ctx.dhchap_key != NULL) {
		key = spdk_keyring_get_key(ctx.dhchap_key);
		if (key == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP key: %s\n", ctx.dhchap_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}
	if (ctx.dhchap_ctrlr_key != NULL) {
		ckey = spdk_keyring_get_key(ctx.dhchap_ctrlr_key);
		if (ckey == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP ctrlr key: %s\n",
				    ctx.dhchap_ctrlr_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}

	opts.size = SPDK_SIZEOF(&opts, dhchap_ctrlr_key);
	opts.dhchap_key = key;
	opts.dhchap_ctrlr_key = ckey;
	rc = spdk_nvmf_subsystem_set_keys(subsystem, ctx.host, &opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);
out:
	spdk_keyring_put_key(ckey);
	spdk_keyring_put_key(key);
	nvmf_rpc_host_ctx_free(&ctx);
}
SPDK_RPC_REGISTER("nvmf_subsystem_set_keys", rpc_nvmf_subsystem_set_keys, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_allow_any_host_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_host_ctx, nqn), spdk_json_decode_string},
	{"allow_any_host", offsetof(struct nvmf_rpc_host_ctx, allow_any_host), spdk_json_decode_bool},
	{"tgt_name", offsetof(struct nvmf_rpc_host_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_subsystem_allow_any_host(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx ctx = {};
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_allow_any_host_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_allow_any_host_decoders),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx.nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx.nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_set_allow_any_host(subsystem, ctx.allow_any_host);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	nvmf_rpc_host_ctx_free(&ctx);
}
SPDK_RPC_REGISTER("nvmf_subsystem_allow_any_host", rpc_nvmf_subsystem_allow_any_host,
		  SPDK_RPC_RUNTIME)

struct nvmf_rpc_target_ctx {
	char *name;
	uint32_t max_subsystems;
	uint32_t discovery_filter;
};

static int
decode_discovery_filter(const struct spdk_json_val *val, void *out)
{
	uint32_t *_filter = out;
	uint32_t filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY;
	char *tokens = spdk_json_strdup(val);
	char *tok;
	char *sp = NULL;
	int rc = -EINVAL;
	bool all_specified = false;

	if (!tokens) {
		return -ENOMEM;
	}

	tok = strtok_r(tokens, ",", &sp);
	while (tok) {
		if (strncmp(tok, "match_any", 9) == 0) {
			if (filter != SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY) {
				goto out;
			}
			filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY;
			all_specified = true;
		} else {
			if (all_specified) {
				goto out;
			}
			if (strncmp(tok, "transport", 9) == 0) {
				filter |= SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE;
			} else if (strncmp(tok, "address", 7) == 0) {
				filter |= SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS;
			} else if (strncmp(tok, "svcid", 5) == 0) {
				filter |= SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID;
			} else {
				SPDK_ERRLOG("Invalid value %s\n", tok);
				goto out;
			}
		}

		tok = strtok_r(NULL, ",", &sp);
	}

	rc = 0;
	*_filter = filter;

out:
	free(tokens);

	return rc;
}

static const struct spdk_json_object_decoder rpc_nvmf_create_target_decoders[] = {
	{"name", offsetof(struct nvmf_rpc_target_ctx, name), spdk_json_decode_string},
	{"max_subsystems", offsetof(struct nvmf_rpc_target_ctx, max_subsystems), spdk_json_decode_uint32, true},
	{"discovery_filter", offsetof(struct nvmf_rpc_target_ctx, discovery_filter), decode_discovery_filter, true}
};

/*
 * [한국어]
 * rpc_nvmf_create_target - "nvmf_create_target" RPC (private 등록)
 *
 * @request: RPC 요청.
 * @params: JSON. name(필수), max_subsystems/discovery_filter (optional).
 *
 * 새로운 NVMe-oF target 객체(spdk_nvmf_tgt)를 생성. 한 SPDK 프로세스 안에 여러 tgt가
 * 공존할 수 있으며 각각 자체 subsystem/transport/listener를 갖는다. 같은 이름이
 * 이미 있으면 거절. discovery_filter는 호스트가 Discovery Log Page를 받을 때 어떤
 * 항목만 보일지 (transport 매칭, traddr 매칭, trsvcid 매칭 또는 match_any) 결정.
 */
static void
rpc_nvmf_create_target(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct spdk_nvmf_target_opts	opts;
	struct nvmf_rpc_target_ctx	ctx = {0};
	struct spdk_nvmf_tgt		*tgt;
	struct spdk_json_write_ctx	*w;

	/* Decode parameters the first time to get the transport type */
	if (spdk_json_decode_object(params, rpc_nvmf_create_target_decoders,
				    SPDK_COUNTOF(rpc_nvmf_create_target_decoders),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	snprintf(opts.name, NVMF_TGT_NAME_MAX_LENGTH, "%s", ctx.name);
	opts.max_subsystems = ctx.max_subsystems;
	opts.discovery_filter = ctx.discovery_filter;
	opts.size = SPDK_SIZEOF(&opts, discovery_filter);

	if (spdk_nvmf_get_tgt(opts.name) != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Target already exists.");
		goto out;
	}

	tgt = spdk_nvmf_tgt_create(&opts);

	if (tgt == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to create the requested target.");
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_nvmf_tgt_get_name(tgt));
	spdk_jsonrpc_end_result(request, w);
out:
	free(ctx.name);
}
/* private */ SPDK_RPC_REGISTER("nvmf_create_target", rpc_nvmf_create_target, SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_nvmf_delete_target_decoders[] = {
	{"name", offsetof(struct nvmf_rpc_target_ctx, name), spdk_json_decode_string},
};

static void
nvmf_rpc_destroy_target_done(void *ctx, int status)
{
	struct spdk_jsonrpc_request	*request = ctx;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_nvmf_delete_target(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct nvmf_rpc_target_ctx	ctx = {0};
	struct spdk_nvmf_tgt		*tgt;

	/* Decode parameters the first time to get the transport type */
	if (spdk_json_decode_object(params, rpc_nvmf_delete_target_decoders,
				    SPDK_COUNTOF(rpc_nvmf_delete_target_decoders),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free(ctx.name);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.name);

	if (tgt == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "The specified target doesn't exist, cannot delete it.");
		free(ctx.name);
		return;
	}

	spdk_nvmf_tgt_destroy(tgt, nvmf_rpc_destroy_target_done, request);
	free(ctx.name);
}
/* private */ SPDK_RPC_REGISTER("nvmf_delete_target", rpc_nvmf_delete_target, SPDK_RPC_RUNTIME);

static void
rpc_nvmf_get_targets(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx	*w;
	struct spdk_nvmf_tgt		*tgt;
	const char			*name;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "nvmf_get_targets has no parameters.");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	tgt = spdk_nvmf_get_first_tgt();

	while (tgt != NULL) {
		name = spdk_nvmf_tgt_get_name(tgt);
		spdk_json_write_string(w, name);
		tgt = spdk_nvmf_get_next_tgt(tgt);
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
/* private */ SPDK_RPC_REGISTER("nvmf_get_targets", rpc_nvmf_get_targets, SPDK_RPC_RUNTIME);

/* [한국어] nvmf_create_transport RPC의 비동기 컨텍스트.
 * transport 생성 → tgt_add_transport → (실패 시 destroy) 의 콜백 체인을 따라간다.
 * transport는 한 tgt당 trtype별 하나만 존재한다 (TCP/RDMA/FC 각각). */
struct nvmf_rpc_create_transport_ctx {
	char				*trtype;
	/* [한국어] "TCP"/"RDMA"/"FC" 등. transport opts 디코더의 trtype 분기 키. */

	char				*tgt_name;
	/* [한국어] 부착할 target 이름. NULL이면 default tgt. */

	struct spdk_nvmf_transport_opts	opts;
	/* [한국어] transport 생성 옵션 (max_queue_depth, max_io_qpairs_per_ctrlr,
	 * in_capsule_data_size, max_io_size, kas, abort_timeout_sec, zcopy 등).
	 * spdk_nvmf_transport_opts_init로 trtype별 기본값 채운 후 JSON으로 덮어쓴다. */

	struct spdk_jsonrpc_request	*request;
	/* [한국어] 응답 송신용 RPC 요청. */

	struct spdk_nvmf_transport	*transport;
	/* [한국어] 생성된 transport 객체 포인터. tgt_add_transport 실패 시 destroy 대상. */

	int				status;
	/* [한국어] tgt_add_transport 실패 코드. destroy 콜백에서 응답에 사용. */
};

/**
 * `max_qpairs_per_ctrlr` represents both admin and IO qpairs, that confuses
 * users when they configure a transport using RPC. So it was decided to
 * deprecate `max_qpairs_per_ctrlr` RPC parameter and use `max_io_qpairs_per_ctrlr`
 * But internal logic remains unchanged and SPDK expects that
 * spdk_nvmf_transport_opts::max_qpairs_per_ctrlr includes an admin qpair.
 * This function parses the number of IO qpairs and adds +1 for admin qpair.
 */
static int
nvmf_rpc_decode_max_io_qpairs(const struct spdk_json_val *val, void *out)
{
	uint16_t *i = out;
	int rc;

	rc = spdk_json_number_to_uint16(val, i);
	if (rc == 0) {
		(*i)++;
	}

	return rc;
}

static int
decode_masked_oncs(const struct spdk_json_val *val, void *out)
{
	struct spdk_nvme_cdata_oncs *oncs = out;
	char *name = NULL;
	int rc;

	rc = spdk_json_decode_string(val, &name);
	if (rc) {
		return rc;
	}

	if (strcmp(name, "nvmcmps") == 0) {
		oncs->nvmcmps = 0;
	} else if (strcmp(name, "nvmdsmsv") == 0) {
		oncs->nvmdsmsv = 0;
	} else if (strcmp(name, "nvmwzsv") == 0) {
		oncs->nvmwzsv = 0;
	} else if (strcmp(name, "reservs") == 0) {
		oncs->reservs = 0;
	} else if (strcmp(name, "nvmcpys") == 0) {
		oncs->nvmcpys = 0;
	} else {
		rc = -EINVAL;
		goto out;
	}

out:
	free(name);
	return rc;
}

static int
decode_masked_oncs_array(const struct spdk_json_val *val, void *out)
{
	size_t count;

	return spdk_json_decode_array(val, decode_masked_oncs, out, 16, &count, 0);
}

static int
decode_masked_fuses(const struct spdk_json_val *val, void *out)
{
	struct spdk_nvme_cdata_fuses *fuses = out;
	char *name = NULL;
	int rc;

	rc = spdk_json_decode_string(val, &name);
	if (rc) {
		return rc;
	}

	if (strcmp(name, "fcws") == 0) {
		fuses->fcws = 0;
	} else {
		rc = -EINVAL;
		goto out;
	}

out:
	free(name);
	return rc;
}

static int
decode_masked_fuses_array(const struct spdk_json_val *val, void *out)
{
	size_t count;

	return spdk_json_decode_array(val, decode_masked_fuses, out, 16, &count, 0);
}

static const struct spdk_json_object_decoder rpc_nvmf_create_transport_decoders[] = {
	{"trtype", offsetof(struct nvmf_rpc_create_transport_ctx, trtype), spdk_json_decode_string},
	{"max_queue_depth", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_queue_depth), spdk_json_decode_uint16, true},
	{"max_io_qpairs_per_ctrlr", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_qpairs_per_ctrlr), nvmf_rpc_decode_max_io_qpairs, true},
	{"in_capsule_data_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.in_capsule_data_size), spdk_json_decode_uint32, true},
	{"max_io_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_io_size), spdk_json_decode_uint32, true},
	{"io_unit_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.io_unit_size), spdk_json_decode_uint32, true},
	{"max_aq_depth", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_aq_depth), spdk_json_decode_uint32, true},
	{"num_shared_buffers", offsetof(struct nvmf_rpc_create_transport_ctx, opts.num_shared_buffers), spdk_json_decode_uint32, true},
	{"buf_cache_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.buf_cache_size), spdk_json_decode_uint32, true},
	{"dif_insert_or_strip", offsetof(struct nvmf_rpc_create_transport_ctx, opts.dif_insert_or_strip), spdk_json_decode_bool, true},
	{"abort_timeout_sec", offsetof(struct nvmf_rpc_create_transport_ctx, opts.abort_timeout_sec), spdk_json_decode_uint32, true},
	{"zcopy", offsetof(struct nvmf_rpc_create_transport_ctx, opts.zcopy), spdk_json_decode_bool, true},
	{"tgt_name", offsetof(struct nvmf_rpc_create_transport_ctx, tgt_name), spdk_json_decode_string, true},
	{"acceptor_poll_rate", offsetof(struct nvmf_rpc_create_transport_ctx, opts.acceptor_poll_rate), spdk_json_decode_uint32, true},
	{"ack_timeout", offsetof(struct nvmf_rpc_create_transport_ctx, opts.ack_timeout), spdk_json_decode_uint32, true},
	{"data_wr_pool_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.data_wr_pool_size), spdk_json_decode_uint32, true},
	{"disable_command_passthru", offsetof(struct nvmf_rpc_create_transport_ctx, opts.disable_command_passthru), spdk_json_decode_bool, true},
	{"kas", offsetof(struct nvmf_rpc_create_transport_ctx, opts.kas), spdk_json_decode_uint16, true},
	{"min_kato", offsetof(struct nvmf_rpc_create_transport_ctx, opts.min_kato), spdk_json_decode_uint32, true},
	{"masked_oncs", offsetof(struct nvmf_rpc_create_transport_ctx, opts.oncs), decode_masked_oncs_array, true},
	{"masked_fuses", offsetof(struct nvmf_rpc_create_transport_ctx, opts.fuses), decode_masked_fuses_array, true},
};

static void
nvmf_rpc_create_transport_ctx_free(struct nvmf_rpc_create_transport_ctx *ctx)
{
	free(ctx->trtype);
	free(ctx->tgt_name);
	free(ctx);
}

static void
nvmf_rpc_transport_destroy_done_cb(void *cb_arg)
{
	struct nvmf_rpc_create_transport_ctx *ctx = cb_arg;

	spdk_jsonrpc_send_error_response_fmt(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					     "Failed to add transport to tgt.(%d)", ctx->status);
	nvmf_rpc_create_transport_ctx_free(ctx);
}

static void
nvmf_rpc_tgt_add_transport_done(void *cb_arg, int status)
{
	struct nvmf_rpc_create_transport_ctx *ctx = cb_arg;

	if (status) {
		SPDK_ERRLOG("Failed to add transport to tgt.(%d)\n", status);
		ctx->status = status;
		spdk_nvmf_transport_destroy(ctx->transport, nvmf_rpc_transport_destroy_done_cb, ctx);
		return;
	}

	spdk_jsonrpc_send_bool_response(ctx->request, true);
	nvmf_rpc_create_transport_ctx_free(ctx);
}

static void
nvmf_rpc_create_transport_done(void *cb_arg, struct spdk_nvmf_transport *transport)
{
	struct nvmf_rpc_create_transport_ctx *ctx = cb_arg;

	if (!transport) {
		SPDK_ERRLOG("Failed to create transport.\n");
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to create transport.");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	ctx->transport = transport;

	spdk_nvmf_tgt_add_transport(spdk_nvmf_get_tgt(ctx->tgt_name), transport,
				    nvmf_rpc_tgt_add_transport_done, ctx);
}

/*
 * [한국어]
 * rpc_nvmf_create_transport - "nvmf_create_transport" RPC 핸들러
 *
 * @request: RPC 요청.
 * @params: JSON. trtype(필수), 그 외 transport_opts 모든 필드 (optional).
 *
 * 단계:
 *  1) ctx calloc.
 *  2) 1차 디코드 - trtype을 알아내기 위해 relaxed decode (다른 옵션은 무시될 수도).
 *  3) tgt 룩업.
 *  4) spdk_nvmf_transport_opts_init(trtype, opts) - trtype별 기본값 채움.
 *  5) 2차 디코드 - 사용자가 준 옵션으로 기본값 덮어쓰기.
 *  6) 같은 trtype의 transport가 이미 있으면 거절.
 *  7) opts.transport_specific = params (transport가 자체 옵션 디코드)
 *  8) spdk_nvmf_transport_create_async → 콜백 nvmf_rpc_create_transport_done
 *     → spdk_nvmf_tgt_add_transport → 콜백 nvmf_rpc_tgt_add_transport_done
 *     → send_bool_response.
 *
 * 결과: 호스트는 이제 해당 trtype/listen address로 Connect 가능.
 */
static void
rpc_nvmf_create_transport(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct nvmf_rpc_create_transport_ctx *ctx;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	/* Decode parameters the first time to get the transport type */
	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_create_transport_decoders,
					    SPDK_COUNTOF(rpc_nvmf_create_transport_decoders),
					    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	/* Initialize all the transport options (based on transport type) and decode the
	 * parameters again to update any options passed in rpc create transport call.
	 */
	if (!spdk_nvmf_transport_opts_init(ctx->trtype, &ctx->opts, sizeof(ctx->opts))) {
		/* This can happen if user specifies PCIE transport type which isn't valid for
		 * NVMe-oF.
		 */
		SPDK_ERRLOG("Invalid transport type '%s'\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid transport type '%s'", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_create_transport_decoders,
					    SPDK_COUNTOF(rpc_nvmf_create_transport_decoders),
					    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	if (spdk_nvmf_tgt_get_transport(tgt, ctx->trtype)) {
		SPDK_ERRLOG("Transport type '%s' already exists\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Transport type '%s' already exists", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	/* Transport can parse additional params themselves */
	ctx->opts.transport_specific = params;
	ctx->request = request;

	rc = spdk_nvmf_transport_create_async(ctx->trtype, &ctx->opts, nvmf_rpc_create_transport_done, ctx);
	if (rc) {
		SPDK_ERRLOG("Transport type '%s' create failed\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Transport type '%s' create failed", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_create_transport", rpc_nvmf_create_transport, SPDK_RPC_RUNTIME)

struct rpc_get_transport {
	char *trtype;
	char *tgt_name;
};

static const struct spdk_json_object_decoder rpc_nvmf_get_transports_decoders[] = {
	{"trtype", offsetof(struct rpc_get_transport, trtype), spdk_json_decode_string, true},
	{"tgt_name", offsetof(struct rpc_get_transport, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_get_transports(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_get_transport req = { 0 };
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_transport *transport = NULL;
	struct spdk_nvmf_tgt *tgt;

	if (params) {
		if (spdk_json_decode_object(params, rpc_nvmf_get_transports_decoders,
					    SPDK_COUNTOF(rpc_nvmf_get_transports_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free(req.trtype);
		free(req.tgt_name);
		return;
	}

	if (req.trtype) {
		transport = spdk_nvmf_tgt_get_transport(tgt, req.trtype);
		if (transport == NULL) {
			SPDK_ERRLOG("transport '%s' does not exist\n", req.trtype);
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
			free(req.trtype);
			free(req.tgt_name);
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (transport) {
		nvmf_transport_dump_opts(transport, w, false);
	} else {
		for (transport = spdk_nvmf_transport_get_first(tgt); transport != NULL;
		     transport = spdk_nvmf_transport_get_next(transport)) {
			nvmf_transport_dump_opts(transport, w, false);
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free(req.trtype);
	free(req.tgt_name);
}
SPDK_RPC_REGISTER("nvmf_get_transports", rpc_nvmf_get_transports, SPDK_RPC_RUNTIME)

struct rpc_nvmf_get_stats_ctx {
	char *tgt_name;
	struct spdk_nvmf_tgt *tgt;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
};

static const struct spdk_json_object_decoder rpc_nvmf_get_stats_decoders[] = {
	{"tgt_name", offsetof(struct rpc_nvmf_get_stats_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
free_get_stats_ctx(struct rpc_nvmf_get_stats_ctx *ctx)
{
	free(ctx->tgt_name);
	free(ctx);
}

static void
rpc_nvmf_get_stats_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_nvmf_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	spdk_json_write_array_end(ctx->w);
	spdk_json_write_object_end(ctx->w);
	spdk_jsonrpc_end_result(ctx->request, ctx->w);
	free_get_stats_ctx(ctx);
}

static void
_rpc_nvmf_get_stats(struct spdk_io_channel_iter *i)
{
	struct rpc_nvmf_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group;

	ch = spdk_get_io_channel(ctx->tgt);
	group = spdk_io_channel_get_ctx(ch);

	spdk_nvmf_poll_group_dump_stat(group, ctx->w);

	spdk_put_io_channel(ch);
	spdk_for_each_channel_continue(i, 0);
}


/*
 * [한국어]
 * rpc_nvmf_get_stats - "nvmf_get_stats" RPC: poll group별 통계 dump
 *
 * @request: RPC 요청.
 * @params: JSON. tgt_name(optional).
 *
 * tgt의 모든 IO channel(=각 reactor의 poll group)을 spdk_for_each_channel로 순회하며
 * spdk_nvmf_poll_group_dump_stat을 호출. 결과는 {tick_rate, poll_groups: [...]} 형식.
 * 각 group은 admin/io qpair 수, completed/current IO 카운터 등을 포함.
 */
static void
rpc_nvmf_get_stats(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_nvmf_get_stats_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error");
		return;
	}
	ctx->request = request;

	if (params) {
		if (spdk_json_decode_object(params, rpc_nvmf_get_stats_decoders,
					    SPDK_COUNTOF(rpc_nvmf_get_stats_decoders),
					    ctx)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			free_get_stats_ctx(ctx);
			return;
		}
	}

	ctx->tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!ctx->tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free_get_stats_ctx(ctx);
		return;
	}

	ctx->w = spdk_jsonrpc_begin_result(ctx->request);
	spdk_json_write_object_begin(ctx->w);
	spdk_json_write_named_uint64(ctx->w, "tick_rate", spdk_get_ticks_hz());
	spdk_json_write_named_array_begin(ctx->w, "poll_groups");

	spdk_for_each_channel(ctx->tgt,
			      _rpc_nvmf_get_stats,
			      ctx,
			      rpc_nvmf_get_stats_done);
}

SPDK_RPC_REGISTER("nvmf_get_stats", rpc_nvmf_get_stats, SPDK_RPC_RUNTIME)

static const char *
nvmf_cntrltype_str(enum spdk_nvme_ctrlr_type type)
{
	switch (type) {
	case SPDK_NVME_CTRLR_IO:
		return "io";
	case SPDK_NVME_CTRLR_DISCOVERY:
		return "discovery";
	case SPDK_NVME_CTRLR_ADMINISTRATIVE:
		return "administrative";
	default:
		return "unknown";
	}
}

static void
dump_nvmf_ctrlr(struct spdk_json_write_ctx *w, struct spdk_nvmf_ctrlr *ctrlr)
{
	uint32_t count;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint32(w, "cntlid", ctrlr->cntlid);
	spdk_json_write_named_string(w, "cntrltype", nvmf_cntrltype_str(ctrlr->cdata.cntrltype));
	spdk_json_write_named_string(w, "hostnqn", ctrlr->hostnqn);
	spdk_json_write_named_uuid(w, "hostid", &ctrlr->hostid);

	count = spdk_bit_array_count_set(ctrlr->qpair_mask);
	spdk_json_write_named_uint32(w, "num_io_qpairs", count);

	spdk_json_write_object_end(w);
}

static const char *
nvmf_qpair_state_str(enum spdk_nvmf_qpair_state state)
{
	switch (state) {
	case SPDK_NVMF_QPAIR_UNINITIALIZED:
		return "uninitialized";
	case SPDK_NVMF_QPAIR_CONNECTING:
		return "connecting";
	case SPDK_NVMF_QPAIR_AUTHENTICATING:
		return "authenticating";
	case SPDK_NVMF_QPAIR_ENABLED:
		return "enabled";
	case SPDK_NVMF_QPAIR_DEACTIVATING:
		return "deactivating";
	case SPDK_NVMF_QPAIR_ERROR:
		return "error";
	default:
		return NULL;
	}
}

static void
dump_nvmf_qpair(struct spdk_json_write_ctx *w, struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvme_transport_id trid = {};

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint32(w, "cntlid", qpair->ctrlr->cntlid);
	spdk_json_write_named_uint32(w, "qid", qpair->qid);
	spdk_json_write_named_string(w, "state", nvmf_qpair_state_str(qpair->state));
	spdk_json_write_named_string(w, "thread", spdk_thread_get_name(spdk_get_thread()));
	spdk_json_write_named_string(w, "hostnqn", qpair->ctrlr->hostnqn);

	if (spdk_nvmf_qpair_get_listen_trid(qpair, &trid) == 0) {
		spdk_json_write_named_object_begin(w, "listen_address");
		nvmf_transport_listen_dump_trid(&trid, w);
		spdk_json_write_object_end(w);
		if (qpair->transport->ops->listen_dump_opts) {
			qpair->transport->ops->listen_dump_opts(qpair->transport, &trid, w);
		}
	}

	memset(&trid, 0, sizeof(trid));
	if (spdk_nvmf_qpair_get_peer_trid(qpair, &trid) == 0) {
		spdk_json_write_named_object_begin(w, "peer_address");
		nvmf_transport_listen_dump_trid(&trid, w);
		spdk_json_write_object_end(w);
	}

	nvmf_qpair_auth_dump(qpair, w);
	spdk_json_write_object_end(w);
}

static const char *
nvme_ana_state_str(enum spdk_nvme_ana_state ana_state)
{
	switch (ana_state) {
	case SPDK_NVME_ANA_OPTIMIZED_STATE:
		return "optimized";
	case SPDK_NVME_ANA_NON_OPTIMIZED_STATE:
		return "non_optimized";
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
		return "inaccessible";
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
		return "persistent_loss";
	case SPDK_NVME_ANA_CHANGE_STATE:
		return "change";
	default:
		return NULL;
	}
}

static void
dump_nvmf_subsystem_listener(struct spdk_json_write_ctx *w,
			     struct spdk_nvmf_subsystem_listener *listener)
{
	uint32_t i;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_object_begin(w, "address");
	nvmf_transport_listen_dump_trid(listener->trid, w);
	spdk_json_write_object_end(w);

	if (spdk_nvmf_subsystem_get_ana_reporting(listener->subsystem)) {
		spdk_json_write_named_array_begin(w, "ana_states");
		for (i = 0; i < listener->subsystem->max_nsid; i++) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_uint32(w, "ana_group", i + 1);
			spdk_json_write_named_string(w, "ana_state",
						     nvme_ana_state_str(listener->ana_state[i]));
			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);
	}

	spdk_json_write_object_end(w);
}

struct rpc_subsystem_query_ctx {
	char *nqn;
	char *tgt_name;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
};

static const struct spdk_json_object_decoder rpc_subsystem_query_decoders[] = {
	{"nqn", offsetof(struct rpc_subsystem_query_ctx, nqn), spdk_json_decode_string},
	{"tgt_name", offsetof(struct rpc_subsystem_query_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
free_rpc_subsystem_query_ctx(struct rpc_subsystem_query_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->tgt_name);
	free(ctx);
}

static void
rpc_nvmf_get_controllers_paused(struct spdk_nvmf_subsystem *subsystem,
				void *cb_arg, int status)
{
	struct rpc_subsystem_query_ctx *ctx = cb_arg;
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_ctrlr *ctrlr;

	w = spdk_jsonrpc_begin_result(ctx->request);

	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(ctrlr, &ctx->subsystem->ctrlrs, link) {
		dump_nvmf_ctrlr(w, ctrlr);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(ctx->request, w);

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, NULL, NULL)) {
		SPDK_ERRLOG("Resuming subsystem with NQN %s failed\n", ctx->nqn);
		/* FIXME: RPC should fail if resuming the subsystem failed. */
	}

	free_rpc_subsystem_query_ctx(ctx);
}

static void
rpc_nvmf_get_qpairs_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_subsystem_query_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	spdk_json_write_array_end(ctx->w);
	spdk_jsonrpc_end_result(ctx->request, ctx->w);

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, NULL, NULL)) {
		SPDK_ERRLOG("Resuming subsystem with NQN %s failed\n", ctx->nqn);
		/* FIXME: RPC should fail if resuming the subsystem failed. */
	}

	free_rpc_subsystem_query_ctx(ctx);
}

static void
rpc_nvmf_get_qpairs(struct spdk_io_channel_iter *i)
{
	struct rpc_subsystem_query_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group;
	struct spdk_nvmf_qpair *qpair;

	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	TAILQ_FOREACH(qpair, &group->qpairs, link) {
		if (qpair->ctrlr && qpair->ctrlr->subsys == ctx->subsystem) {
			dump_nvmf_qpair(ctx->w, qpair);
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
rpc_nvmf_get_qpairs_paused(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct rpc_subsystem_query_ctx *ctx = cb_arg;

	ctx->w = spdk_jsonrpc_begin_result(ctx->request);

	spdk_json_write_array_begin(ctx->w);

	spdk_for_each_channel(ctx->subsystem->tgt,
			      rpc_nvmf_get_qpairs,
			      ctx,
			      rpc_nvmf_get_qpairs_done);
}

static void
rpc_nvmf_get_listeners_paused(struct spdk_nvmf_subsystem *subsystem,
			      void *cb_arg, int status)
{
	struct rpc_subsystem_query_ctx *ctx = cb_arg;
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_subsystem_listener *listener;

	w = spdk_jsonrpc_begin_result(ctx->request);

	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (!nvmf_subsystem_listener_is_active(listener)) {
			continue;
		}

		dump_nvmf_subsystem_listener(w, listener);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(ctx->request, w);

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, NULL, NULL)) {
		SPDK_ERRLOG("Resuming subsystem with NQN %s failed\n", ctx->nqn);
		/* FIXME: RPC should fail if resuming the subsystem failed. */
	}

	free_rpc_subsystem_query_ctx(ctx);
}

/*
 * [한국어]
 * _rpc_nvmf_subsystem_query - subsystem 조회 RPC들의 공통 헬퍼
 *
 * @request: RPC 요청.
 * @params: JSON (nqn 필수).
 * @cb_fn: pause 완료 시 호출될 콜백 (조회 작업의 본체).
 *
 * subsystem 조회는 일관성을 위해 pause 상태에서 수행한다. 본 함수는 ctx 생성,
 * 디코드, tgt/subsystem 룩업 후 spdk_nvmf_subsystem_pause(0, cb_fn, ctx)만 수행한다.
 * cb_fn에서 ctrlr/qpair/listener 목록을 dump한 뒤 spdk_nvmf_subsystem_resume.
 *
 * nvmf_subsystem_get_controllers, nvmf_subsystem_get_qpairs,
 * nvmf_subsystem_get_listeners RPC가 본 함수를 공유한다.
 */
static void
_rpc_nvmf_subsystem_query(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params,
			  spdk_nvmf_subsystem_state_change_done cb_fn)
{
	struct rpc_subsystem_query_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, rpc_subsystem_query_decoders,
				    SPDK_COUNTOF(rpc_subsystem_query_decoders),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}

	ctx->subsystem = subsystem;

	if (spdk_nvmf_subsystem_pause(subsystem, 0, cb_fn, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}
}

static void
rpc_nvmf_subsystem_get_controllers(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	_rpc_nvmf_subsystem_query(request, params, rpc_nvmf_get_controllers_paused);
}
SPDK_RPC_REGISTER("nvmf_subsystem_get_controllers", rpc_nvmf_subsystem_get_controllers,
		  SPDK_RPC_RUNTIME);

static void
rpc_nvmf_subsystem_get_qpairs(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	_rpc_nvmf_subsystem_query(request, params, rpc_nvmf_get_qpairs_paused);
}
SPDK_RPC_REGISTER("nvmf_subsystem_get_qpairs", rpc_nvmf_subsystem_get_qpairs, SPDK_RPC_RUNTIME);

static void
rpc_nvmf_subsystem_get_listeners(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	_rpc_nvmf_subsystem_query(request, params, rpc_nvmf_get_listeners_paused);
}
SPDK_RPC_REGISTER("nvmf_subsystem_get_listeners", rpc_nvmf_subsystem_get_listeners,
		  SPDK_RPC_RUNTIME);

struct rpc_mdns_prr {
	char *tgt_name;
};

static const struct spdk_json_object_decoder rpc_nvmf_publish_mdns_prr_decoders[] = {
	{"tgt_name", offsetof(struct rpc_mdns_prr, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_publish_mdns_prr(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	int rc;
	struct rpc_mdns_prr req = { 0 };
	struct spdk_nvmf_tgt *tgt;

	if (params) {
		if (spdk_json_decode_object(params, rpc_nvmf_publish_mdns_prr_decoders,
					    SPDK_COUNTOF(rpc_nvmf_publish_mdns_prr_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free(req.tgt_name);
		return;
	}

	rc = nvmf_publish_mdns_prr(tgt);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		free(req.tgt_name);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	free(req.tgt_name);
}
SPDK_RPC_REGISTER("nvmf_publish_mdns_prr", rpc_nvmf_publish_mdns_prr, SPDK_RPC_RUNTIME);

static void
rpc_nvmf_stop_mdns_prr(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_mdns_prr req = { 0 };
	struct spdk_nvmf_tgt *tgt;

	if (params) {
		if (spdk_json_decode_object(params, rpc_nvmf_publish_mdns_prr_decoders,
					    SPDK_COUNTOF(rpc_nvmf_publish_mdns_prr_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free(req.tgt_name);
		return;
	}

	nvmf_tgt_stop_mdns_prr(tgt);

	spdk_jsonrpc_send_bool_response(request, true);
	free(req.tgt_name);
}
SPDK_RPC_REGISTER("nvmf_stop_mdns_prr", rpc_nvmf_stop_mdns_prr, SPDK_RPC_RUNTIME);
