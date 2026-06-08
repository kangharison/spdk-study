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

#include "spdk/bdev.h"          /* [한국어] bdev 공통 추상화 API — spdk_bdev_get_name 등 NS 정보 출력에 사용 */
#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG/WARNLOG/NOTICELOG 로깅 매크로 */
#include "spdk/rpc.h"           /* [한국어] SPDK_RPC_REGISTER 매크로 + SPDK_RPC_RUNTIME/STARTUP state 상수 */
#include "spdk/env.h"           /* [한국어] SPDK 환경 초기화 헬퍼 — 직접 사용은 없으나 포함 체계상 필요 */
#include "spdk/nvme.h"          /* [한국어] spdk_nvme_transport_id, ANA state enum, transport parse 헬퍼 */
#include "spdk/nvmf.h"          /* [한국어] NVMe-oF target 공개 API (subsystem/transport/NS/host 라이프사이클) */
#include "spdk/string.h"        /* [한국어] spdk_strerror, spdk_json_strdup 등 문자열 유틸 */
#include "spdk/util.h"          /* [한국어] SPDK_COUNTOF, SPDK_SIZEOF, SPDK_STATIC_ASSERT 매크로 */
#include "spdk/bit_array.h"     /* [한국어] spdk_bit_array_count_set — ctrlr qpair_mask 집계에 사용 */
#include "spdk/config.h"        /* [한국어] SPDK_CONFIG_* 빌드 옵션 분기를 위한 매크로 */

#include "spdk_internal/assert.h" /* [한국어] SPDK_UNREACHABLE() — 도달 불가 분기에서 컴파일러 경고 억제 */

#include "nvmf_internal.h"      /* [한국어] lib/nvmf 내부 공유 자료구조/함수 선언 (subsystem 내부 구조, nvmf_subsystem_* 헬퍼 등) */

/* [한국어] rpc_ana_state_parse forward declaration — 파일 아래쪽에 정의되지만
 * 위쪽의 add_listener 핸들러에서 먼저 사용하므로 전방 선언이 필요. */
static int rpc_ana_state_parse(const char *str, enum spdk_nvme_ana_state *ana_state);

/*
 * [한국어]
 * json_write_hex_str - 바이너리 버퍼를 대문자 16진수 문자열로 JSON 출력
 *
 * @w:    JSON write context. spdk_json_write_string으로 결과 문자열을 직렬화.
 * @data: 16진수로 변환할 바이너리 버퍼 (예: nguid 16바이트, eui64 8바이트).
 * @size: 버퍼 바이트 수. 결과 문자열 길이 = size*2.
 * @return: spdk_json_write_string 반환값 (0=성공, 음수=에러). -1이면 malloc 실패.
 *
 * dump_nvmf_subsystem에서 NS의 nguid/eui64를 JSON으로 출력할 때 사용.
 * 예: {0xAB, 0xCD} → "ABCD".
 * 임시 문자열을 malloc 후 JSON 직렬화하고 즉시 free하는 단순 패턴.
 *
 * 호출 체인:
 *   dump_nvmf_subsystem → [json_write_hex_str] → spdk_json_write_string
 */
static int
json_write_hex_str(struct spdk_json_write_ctx *w, const void *data, size_t size)
{
	/* [한국어] 16진수 변환 룩업 테이블. __spdk_nonstring은 strlen 오용 방지 어노테이션. */
	static const char __spdk_nonstring hex_char[16] = "0123456789ABCDEF";
	const uint8_t *buf = data;                       /* [한국어] 바이트 단위로 접근하기 위한 포인터 */
	char *str, *out;
	int rc;

	str = malloc(size * 2 + 1);                      /* [한국어] 각 바이트 → 2 hex 문자 + NUL 종결자 */
	if (str == NULL) {                                /* [한국어] 메모리 부족 — 에러 코드 -1 반환 */
		return -1;
	}

	out = str;                                        /* [한국어] 출력 포인터를 버퍼 시작으로 초기화 */
	while (size--) {                                  /* [한국어] 모든 바이트에 대해 상위/하위 nybble 변환 */
		unsigned byte = *buf++;                   /* [한국어] 다음 바이트를 부호 없이 읽음 */

		out[0] = hex_char[(byte >> 4) & 0xF];    /* [한국어] 상위 4비트(nybble) → 16진수 문자 */
		out[1] = hex_char[byte & 0xF];           /* [한국어] 하위 4비트(nybble) → 16진수 문자 */

		out += 2;                                 /* [한국어] 출력 포인터를 2자씩 전진 */
	}
	*out = '\0';                                      /* [한국어] 문자열 NUL 종결 */

	rc = spdk_json_write_string(w, str);              /* [한국어] JSON writer에 큰따옴표 포함 문자열로 직렬화 */
	free(str);                                        /* [한국어] JSON writer가 복사를 완료했으므로 즉시 해제 */

	return rc;                                        /* [한국어] 0=성공, 음수=JSON write 에러 */
}

/*
 * [한국어]
 * hex_nybble_to_num - 16진수 문자 한 자를 정수값으로 변환
 *
 * @c: '0'-'9', 'a'-'f', 'A'-'F' 중 하나.
 * @return: 0~15 (성공), -1 (유효하지 않은 문자).
 *
 * decode_hex_string_be에서 nguid/eui64 문자열("ABCD1234…")을 바이트 배열로
 * 변환할 때 한 nybble씩 처리하는 헬퍼. 대소문자 모두 허용.
 *
 * 호출 체인:
 *   decode_ns_nguid/decode_ns_eui64 → decode_hex_string_be → hex_byte_to_num
 *     → [hex_nybble_to_num]
 */
static int
hex_nybble_to_num(char c)
{
	if (c >= '0' && c <= '9') {    /* [한국어] ASCII 숫자 '0'~'9' → 0~9 */
		return c - '0';
	}

	if (c >= 'a' && c <= 'f') {    /* [한국어] ASCII 소문자 'a'~'f' → 10~15 */
		return c - 'a' + 0xA;
	}

	if (c >= 'A' && c <= 'F') {    /* [한국어] ASCII 대문자 'A'~'F' → 10~15 */
		return c - 'A' + 0xA;
	}

	return -1;                     /* [한국어] 유효하지 않은 문자 → 에러 */
}

/*
 * [한국어]
 * hex_byte_to_num - 16진수 문자열 두 자("AB")를 1바이트 정수로 변환
 *
 * @str: 두 자 이상의 16진수 문자열. str[0]=상위 nybble, str[1]=하위 nybble.
 * @return: 0~255 (성공), -1 (첫 nybble 에러), -1 (두 번째 nybble 에러).
 *
 * decode_hex_string_be가 루프 내에서 두 자씩 처리할 때 사용.
 *
 * 호출 체인:
 *   decode_hex_string_be → [hex_byte_to_num] → hex_nybble_to_num(×2)
 */
static int
hex_byte_to_num(const char *str)
{
	int hi, lo;

	hi = hex_nybble_to_num(str[0]);  /* [한국어] 첫 글자(상위 nybble) 파싱 */
	if (hi < 0) {                    /* [한국어] 유효하지 않은 문자면 즉시 에러 반환 */
		return hi;
	}

	lo = hex_nybble_to_num(str[1]);  /* [한국어] 두 번째 글자(하위 nybble) 파싱 */
	if (lo < 0) {                    /* [한국어] 유효하지 않은 문자면 에러 반환 */
		return lo;
	}

	return hi * 16 + lo;             /* [한국어] 상위 nybble을 4비트 좌로 이동(×16) 후 하위 nybble 합산 */
}

/*
 * [한국어]
 * decode_hex_string_be - 16진수 ASCII 문자열을 big-endian 바이트 배열로 디코드
 *
 * @str:  "ABCDEF012345..." 형식의 NUL 종결 16진수 문자열.
 *        nguid는 32자(16바이트), eui64는 16자(8바이트).
 * @out:  디코드된 바이트를 저장할 버퍼. 호출자가 size 바이트 확보.
 * @size: 출력 바이트 수 (예: nguid=16, eui64=8).
 * @return: 0=성공, -1=유효하지 않은 hex 문자 또는 길이 불일치.
 *
 * NVMe 스펙에서 nguid/eui64는 고정 바이트 배열이며 16진수 ASCII 표현을 사용한다.
 * 빅엔디언이므로 str[0..1]이 out[0] (최상위 바이트)에 해당.
 *
 * 호출 체인:
 *   decode_ns_nguid / decode_ns_eui64 → [decode_hex_string_be] → hex_byte_to_num
 */
static int
decode_hex_string_be(const char *str, uint8_t *out, size_t size)
{
	size_t i;

	/* Decode a string in "ABCDEF012345" format to its binary representation */
	for (i = 0; i < size; i++) {             /* [한국어] 출력 바이트 수만큼 반복 */
		int num = hex_byte_to_num(str);  /* [한국어] 현재 위치 두 글자를 1바이트로 변환 */

		if (num < 0) {
			/* Invalid hex byte or end of string */
			return -1;               /* [한국어] 유효하지 않은 hex 문자 또는 조기 NUL — 길이 부족 */
		}

		out[i] = (uint8_t)num;           /* [한국어] 변환된 바이트를 출력 배열에 저장 */
		str += 2;                        /* [한국어] 두 글자(1바이트) 소비 후 포인터 전진 */
	}

	if (i != size || *str != '\0') {         /* [한국어] 루프 후 str이 NUL이 아니면 입력이 너무 길다 */
		/* Length mismatch */
		return -1;
	}

	return 0;                                /* [한국어] 성공 */
}

/*
 * [한국어]
 * decode_ns_nguid - JSON 값에서 NS NGUID(16바이트)를 디코드하는 spdk_json_decode_fn
 *
 * @val: JSON string 값 (예: "ABCDEF012345678901234567890ABCDEF").
 * @out: nvmf_rpc_ns_params.nguid 배열 포인터 (16바이트).
 * @return: 0=성공, 음수=잘못된 형식 또는 길이 불일치.
 *
 * spdk_json_object_decoder 배열에 함수 포인터로 등록되어 JSON 디코드 프레임워크가
 * 자동으로 호출한다. NGUID는 NVMe 스펙 §5.15.2.2의 16바이트 NS 전역 고유 ID.
 *
 * 호출 체인:
 *   spdk_json_decode_object (rpc_nvmf_namespace_decoders) → [decode_ns_nguid]
 *     → decode_hex_string_be(16)
 */
static int
decode_ns_nguid(const struct spdk_json_val *val, void *out)
{
	char *str = NULL;                                  /* [한국어] JSON 문자열을 복사할 임시 버퍼 */
	int rc;

	rc = spdk_json_decode_string(val, &str);           /* [한국어] JSON 값을 malloc된 C 문자열로 변환 */
	if (rc == 0) {
		/* 16-byte NGUID */
		rc = decode_hex_string_be(str, out, 16);   /* [한국어] 32자 16진수 문자열 → 16바이트 빅엔디언 배열 */
	}

	free(str);                                         /* [한국어] JSON 디코딩 완료 후 임시 문자열 해제 */
	return rc;
}

/*
 * [한국어]
 * decode_ns_eui64 - JSON 값에서 NS EUI-64(8바이트)를 디코드하는 spdk_json_decode_fn
 *
 * @val: JSON string 값 (예: "ABCDEF0123456789").
 * @out: nvmf_rpc_ns_params.eui64 배열 포인터 (8바이트).
 * @return: 0=성공, 음수=에러.
 *
 * EUI-64(Extended Unique Identifier 64-bit)는 IEEE에서 정의한 8바이트 세계 고유 식별자.
 * NVMe NS에서 Identify NS의 EUI64 필드(spec §5.15.2.1)로 사용.
 *
 * 호출 체인:
 *   spdk_json_decode_object (rpc_nvmf_namespace_decoders) → [decode_ns_eui64]
 *     → decode_hex_string_be(8)
 */
static int
decode_ns_eui64(const struct spdk_json_val *val, void *out)
{
	char *str = NULL;                                  /* [한국어] JSON 문자열 임시 버퍼 */
	int rc;

	rc = spdk_json_decode_string(val, &str);           /* [한국어] JSON 값 → C 문자열 변환 */
	if (rc == 0) {
		/* 8-byte EUI-64 */
		rc = decode_hex_string_be(str, out, 8);    /* [한국어] 16자 16진수 문자열 → 8바이트 빅엔디언 배열 */
	}

	free(str);                                         /* [한국어] 임시 문자열 해제 */
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

/* [한국어] rpc_nvmf_get_subsystems_decoders: nvmf_get_subsystems JSON 파라미터 디코더 배열.
 * 두 필드 모두 optional(마지막 인자 true). 필드 없으면 각각 NULL로 유지됨.
 * spdk_json_decode_object가 이 배열을 순회하며 JSON 객체의 키-값을 구조체 필드에 매핑. */
static const struct spdk_json_object_decoder rpc_nvmf_get_subsystems_decoders[] = {
	{"nqn", offsetof(struct rpc_get_subsystem, nqn), spdk_json_decode_string, true},        /* [한국어] "nqn" 키 → rpc_get_subsystem.nqn 문자열, optional */
	{"tgt_name", offsetof(struct rpc_get_subsystem, tgt_name), spdk_json_decode_string, true}, /* [한국어] "tgt_name" 키 → rpc_get_subsystem.tgt_name 문자열, optional */
};

/*
 * [한국어]
 * dump_nvmf_subsystem - 하나의 subsystem 정보를 JSON 객체로 직렬화
 *
 * @w:         spdk_json_write_ctx. 호출자(rpc_nvmf_get_subsystems)가 배열 안에서 호출.
 * @subsystem: 출력할 subsystem 포인터. pause 없이 현재 상태를 그대로 읽는다.
 *
 * 출력 JSON 구조:
 *   { "nqn": "...", "subtype": "NVMe"|"Discovery",
 *     "listen_addresses": [...], "allow_any_host": bool,
 *     "hosts": [{"nqn":"...", "dhchap_key":"..."}, ...],
 *     (NVMe subsystem만) "serial_number":"...", "model_number":"...",
 *     "max_namespaces": N, "passthrough": bool,
 *     "min_cntlid": N, "max_cntlid": N,
 *     "namespaces": [{"nsid":N, "bdev_name":"...", ...}, ...] }
 *
 * 호출 컨텍스트: RPC 서버 스레드. 비동기 없음. 응답 직렬화만 수행.
 * pause 없이 읽으므로 동시 변경이 있으면 일관성이 미세하게 어긋날 수 있다.
 *
 * 호출 체인:
 *   rpc_nvmf_get_subsystems → [dump_nvmf_subsystem] → spdk_json_write_*
 */
static void
dump_nvmf_subsystem(struct spdk_json_write_ctx *w, struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_host			*host;      /* [한국어] 호스트 ACL 순회용 임시 포인터 */
	struct spdk_nvmf_subsystem_listener	*listener; /* [한국어] listener 목록 순회용 임시 포인터 */

	spdk_json_write_object_begin(w);              /* [한국어] 이 subsystem의 JSON 객체 '{' 시작 */

	spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem)); /* [한국어] subsystem NQN 문자열 출력 */
	spdk_json_write_name(w, "subtype");           /* [한국어] "subtype" 키만 먼저 출력 (다음 줄에서 값 결정) */
	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) { /* [한국어] NVMe subsystem인지 Discovery subsystem인지 분기 */
		spdk_json_write_string(w, "NVMe");    /* [한국어] NVMe 타입 — 일반 I/O subsystem */
	} else {
		spdk_json_write_string(w, "Discovery"); /* [한국어] Discovery 타입 — NVMe-oF Discovery Log Page 제공 */
	}

	spdk_json_write_named_array_begin(w, "listen_addresses"); /* [한국어] "listen_addresses" 배열 '[' 시작 */

	TAILQ_FOREACH(listener, &subsystem->listeners, link) { /* [한국어] subsystem의 모든 listener를 순회 */
		if (!nvmf_subsystem_listener_is_active(listener)) { /* [한국어] 아직 활성화되지 않은(pending) listener는 스킵 */
			continue;
		}

		spdk_json_write_object_begin(w);          /* [한국어] listener 객체 '{' 시작 */
		nvmf_transport_listen_dump_trid(listener->trid, w); /* [한국어] trid(trtype/adrfam/traddr/trsvcid) 필드들을 JSON에 직렬화 */
		spdk_json_write_object_end(w);            /* [한국어] listener 객체 '}' 종료 */
	}
	spdk_json_write_array_end(w);                 /* [한국어] "listen_addresses" 배열 ']' 종료 */

	spdk_json_write_named_bool(w, "allow_any_host",
				   spdk_nvmf_subsystem_get_allow_any_host(subsystem)); /* [한국어] ACL 없이 모든 호스트 허용 여부 */

	spdk_json_write_named_array_begin(w, "hosts"); /* [한국어] "hosts" 배열 — 허용된 host NQN 목록 */

	for (host = spdk_nvmf_subsystem_get_first_host(subsystem); host != NULL;
	     host = spdk_nvmf_subsystem_get_next_host(subsystem, host)) { /* [한국어] ACL 리스트의 모든 호스트를 순회 */
		spdk_json_write_object_begin(w);          /* [한국어] 호스트 객체 '{' 시작 */
		spdk_json_write_named_string(w, "nqn", spdk_nvmf_host_get_nqn(host)); /* [한국어] 호스트 NQN 출력 */
		if (host->dhchap_key != NULL) {            /* [한국어] DH-HMAC-CHAP 인증 키가 설정된 경우에만 출력 */
			spdk_json_write_named_string(w, "dhchap_key",
						     spdk_key_get_name(host->dhchap_key)); /* [한국어] 호스트→컨트롤러 challenge 키 이름 */
		}
		if (host->dhchap_ctrlr_key != NULL) {      /* [한국어] 양방향 인증용 컨트롤러 키가 설정된 경우에만 출력 */
			spdk_json_write_named_string(w, "dhchap_ctrlr_key",
						     spdk_key_get_name(host->dhchap_ctrlr_key)); /* [한국어] 컨트롤러→호스트 challenge 키 이름 */
		}
		spdk_json_write_object_end(w);            /* [한국어] 호스트 객체 '}' 종료 */
	}
	spdk_json_write_array_end(w);                 /* [한국어] "hosts" 배열 ']' 종료 */

	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) { /* [한국어] NVMe subsystem만 시리얼/NS 정보 포함 (Discovery subsystem은 해당 없음) */
		struct spdk_nvmf_ns *ns;              /* [한국어] NS 목록 순회용 임시 포인터 */
		struct spdk_nvmf_ns_opts ns_opts;     /* [한국어] 각 NS의 옵션(nguid/eui64/uuid/anagrpid) 임시 저장 */
		uint32_t max_namespaces;              /* [한국어] 최대 NS 개수 (0이면 무제한) */

		spdk_json_write_named_string(w, "serial_number", spdk_nvmf_subsystem_get_sn(subsystem)); /* [한국어] Identify Controller SN(20바이트) 출력 */

		spdk_json_write_named_string(w, "model_number", spdk_nvmf_subsystem_get_mn(subsystem)); /* [한국어] Identify Controller MN(40바이트) 출력 */

		max_namespaces = spdk_nvmf_subsystem_get_max_namespaces(subsystem); /* [한국어] subsystem의 max_nsid(최대 NS 개수) 조회 */
		if (max_namespaces != 0) {             /* [한국어] 0이면 무제한이므로 JSON에서 생략 */
			spdk_json_write_named_uint32(w, "max_namespaces", max_namespaces);
		}

		spdk_json_write_named_bool(w, "passthrough", subsystem->passthrough); /* [한국어] passthrough 모드(admin cmd를 bdev로 위임) 여부 */

		spdk_json_write_named_uint32(w, "min_cntlid", spdk_nvmf_subsystem_get_min_cntlid(subsystem)); /* [한국어] 동적 cntlid 할당 하한 */
		spdk_json_write_named_uint32(w, "max_cntlid", spdk_nvmf_subsystem_get_max_cntlid(subsystem)); /* [한국어] 동적 cntlid 할당 상한 */

		spdk_json_write_named_array_begin(w, "namespaces"); /* [한국어] "namespaces" 배열 시작 */
		for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
		     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) { /* [한국어] subsystem의 모든 NS를 nsid 순으로 순회 */
			spdk_nvmf_ns_get_opts(ns, &ns_opts, sizeof(ns_opts)); /* [한국어] NS 옵션(nguid/eui64/uuid/anagrpid) 조회 */
			spdk_json_write_object_begin(w);      /* [한국어] NS 객체 '{' 시작 */
			spdk_json_write_named_int32(w, "nsid", spdk_nvmf_ns_get_id(ns)); /* [한국어] NS ID(1-based NSID) 출력 */
			spdk_json_write_named_string(w, "bdev_name",
						     spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns))); /* [한국어] 이 NS를 백업하는 bdev 이름 출력 */
			/* NOTE: "name" is kept for compatibility only - new code should use bdev_name. */
			/* [한국어] "name"은 하위 호환성 유지용. 신규 코드에서는 bdev_name을 사용. */
			spdk_json_write_named_string(w, "name",
						     spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns))); /* [한국어] bdev_name과 동일값. deprecated */

			if (!spdk_mem_all_zero(ns_opts.nguid, sizeof(ns_opts.nguid))) { /* [한국어] nguid가 0이 아닌 경우에만 출력 (0=미설정) */
				spdk_json_write_name(w, "nguid");
				json_write_hex_str(w, ns_opts.nguid, sizeof(ns_opts.nguid)); /* [한국어] 16바이트 nguid → 32자 대문자 16진수 문자열 */
			}

			if (!spdk_mem_all_zero(ns_opts.eui64, sizeof(ns_opts.eui64))) { /* [한국어] eui64가 0이 아닌 경우에만 출력 */
				spdk_json_write_name(w, "eui64");
				json_write_hex_str(w, ns_opts.eui64, sizeof(ns_opts.eui64)); /* [한국어] 8바이트 eui64 → 16자 대문자 16진수 문자열 */
			}

			if (!spdk_uuid_is_null(&ns_opts.uuid)) { /* [한국어] UUID가 nil(모두 0)이 아닌 경우에만 출력 */
				spdk_json_write_named_uuid(w, "uuid", &ns_opts.uuid); /* [한국어] RFC 4122 UUID 형식으로 직렬화 */
			}

			if (spdk_nvmf_subsystem_get_ana_reporting(subsystem)) { /* [한국어] ANA reporting이 활성화된 subsystem만 anagrpid 출력 */
				spdk_json_write_named_uint32(w, "anagrpid", ns_opts.anagrpid); /* [한국어] 이 NS가 속한 ANA 그룹 ID */
			}

			spdk_json_write_object_end(w);        /* [한국어] NS 객체 '}' 종료 */
		}
		spdk_json_write_array_end(w);             /* [한국어] "namespaces" 배열 ']' 종료 */
	}
	spdk_json_write_object_end(w);                /* [한국어] subsystem 객체 '}' 종료 */
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
	struct rpc_get_subsystem req = { 0 };           /* [한국어] 디코드된 nqn/tgt_name 저장 구조체. 스택 할당, 0으로 초기화 */
	struct spdk_json_write_ctx *w;                  /* [한국어] JSON 응답 작성 컨텍스트 */
	struct spdk_nvmf_subsystem *subsystem = NULL;   /* [한국어] 특정 NQN 조회 결과 or 전체 순회 포인터, 초기 NULL */
	struct spdk_nvmf_tgt *tgt;                      /* [한국어] 조회 대상 target */

	if (params) {                                   /* [한국어] params가 NULL이면 기본값(전체, default tgt) 사용 */
		if (spdk_json_decode_object(params, rpc_nvmf_get_subsystems_decoders,
					    SPDK_COUNTOF(rpc_nvmf_get_subsystems_decoders),
					    &req)) {            /* [한국어] JSON 파라미터를 req 구조체로 디코드 */
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters"); /* [한국어] 잘못된 파라미터 에러 응답 */
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);         /* [한국어] req.tgt_name(NULL=default)으로 target 룩업 */
	if (!tgt) {                                     /* [한국어] 지정한 target이 존재하지 않으면 에러 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free(req.tgt_name);                     /* [한국어] JSON 디코드로 malloc된 문자열 해제 */
		free(req.nqn);
		return;
	}

	if (req.nqn) {                                  /* [한국어] 특정 NQN이 주어진 경우 — 해당 subsystem만 조회 */
		subsystem = spdk_nvmf_tgt_find_subsystem(tgt, req.nqn); /* [한국어] target의 subsystem TAILQ에서 NQN으로 검색 */
		if (!subsystem) {                       /* [한국어] NQN에 해당하는 subsystem이 없으면 -ENODEV 에러 */
			SPDK_ERRLOG("subsystem '%s' does not exist\n", req.nqn);
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV)); /* [한국어] JSON-RPC 에러 코드에 errno 값 사용 */
			free(req.tgt_name);
			free(req.nqn);
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);         /* [한국어] 정상 응답 직렬화 시작 — write ctx 획득 */
	spdk_json_write_array_begin(w);                 /* [한국어] 결과 배열 '[' 시작 */

	if (subsystem) {                                /* [한국어] 특정 subsystem 요청 — 해당 하나만 dump */
		dump_nvmf_subsystem(w, subsystem);
	} else {                                        /* [한국어] 전체 subsystem 요청 — tgt의 모든 subsystem 순회 */
		for (subsystem = spdk_nvmf_subsystem_get_first(tgt); subsystem != NULL;
		     subsystem = spdk_nvmf_subsystem_get_next(subsystem)) { /* [한국어] tgt->subsystems TAILQ 순회 */
			dump_nvmf_subsystem(w, subsystem); /* [한국어] 각 subsystem의 상세 정보를 JSON 객체로 추가 */
		}
	}

	spdk_json_write_array_end(w);                   /* [한국어] 결과 배열 ']' 종료 */
	spdk_jsonrpc_end_result(request, w);            /* [한국어] 응답 전송 완료 */
	free(req.tgt_name);                             /* [한국어] JSON 디코드로 malloc된 문자열 해제 */
	free(req.nqn);
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_get_subsystems" 이름으로 RPC 핸들러를 RUNTIME state에 등록.
 * constructor 섹션에 배치되어 SPDK 초기화 시 자동으로 lib/rpc/rpc.c의 g_rpc_methods에 추가됨. */
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

/* [한국어] nvmf_create_subsystem JSON 파라미터 디코더 배열.
 * true=optional(없으면 0/NULL/false 기본값 사용), false/없음=필수.
 * 이 배열은 spdk_json_decode_object에 전달되어 JSON 키를 구조체 필드로 매핑한다. */
static const struct spdk_json_object_decoder rpc_nvmf_create_subsystem_decoders[] = {
	{"nqn", offsetof(struct rpc_subsystem_create, nqn), spdk_json_decode_string},                                                       /* [한국어] 필수: subsystem NQN 문자열 */
	{"serial_number", offsetof(struct rpc_subsystem_create, serial_number), spdk_json_decode_string, true},                             /* [한국어] optional: Identify Ctrl SN(20B) */
	{"model_number", offsetof(struct rpc_subsystem_create, model_number), spdk_json_decode_string, true},                               /* [한국어] optional: Identify Ctrl MN(40B) */
	{"tgt_name", offsetof(struct rpc_subsystem_create, tgt_name), spdk_json_decode_string, true},                                       /* [한국어] optional: target 이름 (NULL=default) */
	{"max_namespaces", offsetof(struct rpc_subsystem_create, max_namespaces), spdk_json_decode_uint32, true},                           /* [한국어] optional: 최대 NS 개수 */
	{"allow_any_host", offsetof(struct rpc_subsystem_create, allow_any_host), spdk_json_decode_bool, true},                             /* [한국어] optional: 모든 호스트 ACL 없이 허용 */
	{"ana_reporting", offsetof(struct rpc_subsystem_create, ana_reporting), spdk_json_decode_bool, true},                               /* [한국어] optional: ANA log page 지원 활성화 */
	{"min_cntlid", offsetof(struct rpc_subsystem_create, min_cntlid), spdk_json_decode_uint16, true},                                   /* [한국어] optional: 동적 cntlid 할당 하한 */
	{"max_cntlid", offsetof(struct rpc_subsystem_create, max_cntlid), spdk_json_decode_uint16, true},                                   /* [한국어] optional: 동적 cntlid 할당 상한 */
	{"max_discard_size_kib", offsetof(struct rpc_subsystem_create, max_discard_size_kib), spdk_json_decode_uint64, true},               /* [한국어] optional: DSM Deallocate 최대 크기(KiB) */
	{"max_write_zeroes_size_kib", offsetof(struct rpc_subsystem_create, max_write_zeroes_size_kib), spdk_json_decode_uint64, true},     /* [한국어] optional: Write Zeroes 최대 크기(KiB) */
	{"passthrough", offsetof(struct rpc_subsystem_create, passthrough), spdk_json_decode_bool, true},                                   /* [한국어] optional: admin cmd passthrough 모드 */
	{"enable_nssr", offsetof(struct rpc_subsystem_create, enable_nssr), spdk_json_decode_bool, true},                                   /* [한국어] optional: NVM Subsystem Reset(NSSR) 지원 */
};

/*
 * [한국어]
 * rpc_nvmf_subsystem_started - spdk_nvmf_subsystem_start의 완료 콜백
 *
 * @subsystem: 방금 start 완료(또는 실패)된 subsystem.
 * @cb_arg:    spdk_jsonrpc_request 포인터 (rpc_nvmf_create_subsystem에서 전달).
 * @status:    0=성공, 음수=실패. start는 INACTIVE→ACTIVE 상태 전이를 의미.
 *
 * 성공이면 클라이언트에 true bool 응답을 보내고 종료.
 * 실패이면 에러 응답을 보내고 subsystem을 destroy(메모리 해제).
 * subsystem_destroy에 NULL/NULL을 넘기는 것은 완료 콜백 없이 동기 파괴를 의미.
 *
 * 호출 컨텍스트: SPDK RPC/reactor 스레드. spdk_nvmf_subsystem_start 완료 시 발화.
 *
 * 호출 체인:
 *   rpc_nvmf_create_subsystem → spdk_nvmf_subsystem_start
 *     → [rpc_nvmf_subsystem_started] → spdk_jsonrpc_send_bool_response
 */
static void
rpc_nvmf_subsystem_started(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg; /* [한국어] cb_arg를 RPC 요청 포인터로 복원 */

	if (!status) {                                 /* [한국어] status=0이면 start 성공 */
		spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 클라이언트에 {result:true} JSON-RPC 응답 송신 */
	} else {                                       /* [한국어] status 음수이면 start 실패 — 에러 응답 후 subsystem 파괴 */
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Subsystem %s start failed",
						     subsystem->subnqn);          /* [한국어] 에러 메시지에 subnqn 포함 */
		spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);       /* [한국어] 시작 실패한 subsystem 즉시 파괴 (콜백 없음) */
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
	struct rpc_subsystem_create *req;            /* [한국어] JSON 파라미터를 담을 컨텍스트 구조체 */
	struct spdk_nvmf_subsystem *subsystem = NULL; /* [한국어] 새로 생성될 subsystem. cleanup에서 파괴 여부 판단에 사용 */
	struct spdk_nvmf_tgt *tgt;                   /* [한국어] subsystem을 부속할 target */
	int rc = -1;                                 /* [한국어] 비동기 start 반환코드. -1로 초기화하면 cleanup에서 subsystem 파괴 판단 가능 */

	req = calloc(1, sizeof(*req));               /* [한국어] 파라미터 구조체를 heap에 할당 (스택이면 비동기 콜백에서 접근 불가) */
	if (!req) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation failed"); /* [한국어] OOM 에러 응답 */
		return;
	}
	req->min_cntlid = NVMF_MIN_CNTLID;          /* [한국어] 기본 cntlid 하한 (NVMF_MIN_CNTLID=1) — JSON 미지정 시 이 값 사용 */
	req->max_cntlid = NVMF_MAX_CNTLID;          /* [한국어] 기본 cntlid 상한 (NVMF_MAX_CNTLID=0xffef) — 0xfff0~0xffff는 NVMe spec reserved */

	if (spdk_json_decode_object(params, rpc_nvmf_create_subsystem_decoders,
				    SPDK_COUNTOF(rpc_nvmf_create_subsystem_decoders),
				    req)) {                  /* [한국어] JSON 객체를 req 구조체 필드로 디코드 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto cleanup;                        /* [한국어] 디코드 실패 — cleanup에서 req 해제 */
	}

	tgt = spdk_nvmf_get_tgt(req->tgt_name);     /* [한국어] req->tgt_name으로 target 룩업 (NULL=default) */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find target %s\n", req->tgt_name);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Unable to find target %s", req->tgt_name); /* [한국어] tgt 미존재 에러 */
		goto cleanup;
	}

	subsystem = spdk_nvmf_subsystem_create(tgt, req->nqn, SPDK_NVMF_SUBTYPE_NVME,
					       req->max_namespaces); /* [한국어] NVMe subtype의 subsystem 생성. max_namespaces=0이면 라이브러리 기본값 */
	if (!subsystem) {
		SPDK_ERRLOG("Unable to create subsystem %s\n", req->nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Unable to create subsystem %s", req->nqn); /* [한국어] NQN 중복 또는 alloc 실패 */
		goto cleanup;
	}

	if (req->serial_number) {                   /* [한국어] SN이 지정된 경우만 설정 (없으면 기본 SN 유지) */
		if (spdk_nvmf_subsystem_set_sn(subsystem, req->serial_number)) { /* [한국어] SN을 Identify Ctrl 응답에 저장 */
			SPDK_ERRLOG("Subsystem %s: invalid serial number '%s'\n", req->nqn, req->serial_number);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Invalid SN %s", req->serial_number); /* [한국어] SN이 20바이트 초과 등 형식 오류 */
			goto cleanup;
		}
	}

	if (req->model_number) {                    /* [한국어] MN이 지정된 경우만 설정 */
		if (spdk_nvmf_subsystem_set_mn(subsystem, req->model_number)) { /* [한국어] MN을 Identify Ctrl 응답에 저장 */
			SPDK_ERRLOG("Subsystem %s: invalid model number '%s'\n", req->nqn, req->model_number);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Invalid MN %s", req->model_number); /* [한국어] MN이 40바이트 초과 등 형식 오류 */
			goto cleanup;
		}
	}

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, req->allow_any_host); /* [한국어] ACL 모드 설정 (true=ACL 없이 모든 호스트 허용) */

	spdk_nvmf_subsystem_set_ana_reporting(subsystem, req->ana_reporting);   /* [한국어] ANA(Asymmetric Namespace Access) 보고 활성화 */

	if (spdk_nvmf_subsystem_set_cntlid_range(subsystem, req->min_cntlid, req->max_cntlid)) { /* [한국어] 동적 cntlid 할당 범위 설정 */
		SPDK_ERRLOG("Subsystem %s: invalid cntlid range [%u-%u]\n", req->nqn, req->min_cntlid,
			    req->max_cntlid);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid cntlid range [%u-%u]", req->min_cntlid, req->max_cntlid); /* [한국어] min>max 또는 reserved 범위 침범 */
		goto cleanup;
	}

	subsystem->max_discard_size_kib = req->max_discard_size_kib; /* [한국어] DSM Deallocate 최대 크기 — cdata.dmrsl로 Identify Ctrl에 보고 */

	/* max_write_zeroes_size_kib must be aligned to 4 and power of 2 */
	/* [한국어] Write Zeroes 최대 크기는 0(무제한) 또는 2 초과의 2의 거듭제곱이어야 한다.
	 * NVMe spec §6.1.2.5: wzsl은 power of 2, 4KiB 정렬 조건. */
	if (req->max_write_zeroes_size_kib == 0 || (req->max_write_zeroes_size_kib > 2 &&
			spdk_u64_is_pow2(req->max_write_zeroes_size_kib))) { /* [한국어] 0=무제한 허용, 또는 >2이고 2의 거듭제곱이면 유효 */
		subsystem->max_write_zeroes_size_kib = req->max_write_zeroes_size_kib;
	} else {
		SPDK_ERRLOG("Subsystem %s: invalid max_write_zeroes_size_kib %"PRIu64"\n", req->nqn,
			    req->max_write_zeroes_size_kib);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid max_write_zeroes_size_kib %"PRIu64, req->max_write_zeroes_size_kib); /* [한국어] 유효하지 않은 크기 에러 응답 */
		goto cleanup;
	}

	subsystem->passthrough = req->passthrough;          /* [한국어] passthrough 플래그 설정 — nvmf_ctrlr_process_admin_cmd에서 사용 */
	subsystem->nssr_enabled = req->enable_nssr;         /* [한국어] NSSR(NVM Subsystem Reset) 지원 플래그 설정 */

	rc = spdk_nvmf_subsystem_start(subsystem,
				       rpc_nvmf_subsystem_started,
				       request);                 /* [한국어] subsystem INACTIVE→ACTIVE 비동기 전이 시작. 완료 시 rpc_nvmf_subsystem_started 호출 */
	if (rc) {                                            /* [한국어] rc!=0이면 start 자체가 시작도 못 함 (상태 머신 오류 등) */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to start subsystem");
	}

cleanup:
	/* [한국어] 여기까지 오면 성공(rc=0, start 비동기 실행 중) 또는 에러(rc!=0) */
	free(req->nqn);                                      /* [한국어] JSON 디코드로 malloc된 문자열들 해제 */
	free(req->tgt_name);
	free(req->serial_number);
	free(req->model_number);
	free(req);                                           /* [한국어] 파라미터 구조체 자체 해제 */

	if (rc && subsystem) {                               /* [한국어] 에러가 발생했고 subsystem이 이미 생성된 경우에만 파괴 */
		spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL); /* [한국어] 불완전하게 생성된 subsystem을 동기적으로 파괴 */
	}
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_create_subsystem"을 RUNTIME state RPC로 등록.
 * constructor 섹션 배치로 SPDK 초기화 시 자동 등록. */
SPDK_RPC_REGISTER("nvmf_create_subsystem", rpc_nvmf_create_subsystem, SPDK_RPC_RUNTIME)

/* [한국어] nvmf_delete_subsystem RPC의 입력 파라미터 구조체.
 * JSON 예: {"nqn": "nqn.2016-06.io.spdk:cnode1", "tgt_name": "nvmf_tgt"} */
struct rpc_delete_subsystem {
	char *nqn;
	/* [한국어] 삭제할 subsystem NQN. 필수 필드.
	 * 설정자: spdk_json_decode_object. 읽는 자: rpc_nvmf_delete_subsystem().
	 * 값 범위: 유효한 NVMe NQN 문자열. 없으면 즉시 invalid 에러.
	 * 동기화: 단일 RPC 스레드에서만 접근. */

	char *tgt_name;
	/* [한국어] 대상 target 이름. NULL이면 default target.
	 * 설정자: spdk_json_decode_object. 읽는 자: spdk_nvmf_get_tgt().
	 * 값 범위: NULL 또는 존재하는 tgt 이름.
	 * 동기화: 단일 RPC 스레드에서만 접근. */
};

/*
 * [한국어]
 * free_rpc_delete_subsystem - rpc_delete_subsystem의 동적 할당 필드 해제
 *
 * @r: 해제할 rpc_delete_subsystem 구조체 포인터 (구조체 자체는 stack이므로 해제 불필요).
 *
 * nqn/tgt_name 두 문자열은 spdk_json_decode_string이 malloc한 것이므로 free 필요.
 *
 * 호출 체인:
 *   rpc_nvmf_delete_subsystem (에러 경로) → [free_rpc_delete_subsystem]
 */
static void
free_rpc_delete_subsystem(struct rpc_delete_subsystem *r)
{
	free(r->nqn);      /* [한국어] JSON 디코드로 malloc된 NQN 문자열 해제 */
	free(r->tgt_name); /* [한국어] JSON 디코드로 malloc된 target 이름 문자열 해제 */
}

/*
 * [한국어]
 * rpc_nvmf_subsystem_destroy_complete_cb - subsystem 파괴 완료 콜백
 *
 * @cb_arg: spdk_jsonrpc_request 포인터 (rpc_nvmf_subsystem_stopped에서 전달).
 *
 * spdk_nvmf_subsystem_destroy가 -EINPROGRESS를 반환한 경우(비동기 진행 중)
 * 실제 파괴가 완료된 시점에 이 콜백이 호출된다.
 * 클라이언트에 true 응답을 보내 RPC를 마무리한다.
 *
 * 호출 체인:
 *   rpc_nvmf_subsystem_stopped → spdk_nvmf_subsystem_destroy(-EINPROGRESS)
 *     → [rpc_nvmf_subsystem_destroy_complete_cb] → spdk_jsonrpc_send_bool_response
 */
static void
rpc_nvmf_subsystem_destroy_complete_cb(void *cb_arg)
{
	struct spdk_jsonrpc_request *request = cb_arg; /* [한국어] cb_arg를 RPC 요청 포인터로 복원 */

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 파괴 완료 — 클라이언트에 성공 응답 */
}

/*
 * [한국어]
 * rpc_nvmf_subsystem_stopped - subsystem stop 완료 콜백 (delete subsystem 흐름)
 *
 * @subsystem: stop 완료된 subsystem. ACTIVE→INACTIVE 전이 완료.
 * @cb_arg:    spdk_jsonrpc_request 포인터.
 * @status:    0=성공, 음수=stop 실패.
 *
 * stop 후 listener를 모두 제거하고 subsystem을 destroy한다.
 * destroy가 -EINPROGRESS를 반환하면 비동기 파괴 진행 중임을 의미하며
 * rpc_nvmf_subsystem_destroy_complete_cb에서 응답이 송신된다.
 * destroy가 0을 반환하면 이미 동기적으로 파괴 완료되었으므로 즉시 응답.
 *
 * 호출 체인:
 *   rpc_nvmf_delete_subsystem → spdk_nvmf_subsystem_stop
 *     → [rpc_nvmf_subsystem_stopped] → nvmf_subsystem_remove_all_listeners
 *       → spdk_nvmf_subsystem_destroy
 *         → rpc_nvmf_subsystem_destroy_complete_cb (비동기)
 *         OR spdk_jsonrpc_send_bool_response (동기)
 */
static void
rpc_nvmf_subsystem_stopped(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg; /* [한국어] RPC 요청 포인터 복원 */
	int rc;

	nvmf_subsystem_remove_all_listeners(subsystem, true); /* [한국어] subsystem의 모든 listener를 제거하고 transport socket도 닫음 (true=transport stop 포함) */
	rc = spdk_nvmf_subsystem_destroy(subsystem, rpc_nvmf_subsystem_destroy_complete_cb, request); /* [한국어] subsystem 메모리 해제 시작. -EINPROGRESS=비동기, 0=동기 완료 */
	if (rc) {
		if (rc == -EINPROGRESS) {
			/* response will be sent in completion callback */
			/* [한국어] 비동기 파괴 진행 중 — 완료 콜백(rpc_nvmf_subsystem_destroy_complete_cb)에서 응답 */
			return;
		} else {                              /* [한국어] 예상치 못한 파괴 에러 */
			SPDK_ERRLOG("Subsystem destruction failed, rc %d\n", rc);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							     "Subsystem destruction failed, rc %d", rc);
			return;
		}
	}
	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 동기 파괴 완료 — 즉시 성공 응답 */
}

/* [한국어] nvmf_delete_subsystem JSON 파라미터 디코더 배열. */
static const struct spdk_json_object_decoder rpc_nvmf_delete_subsystem_decoders[] = {
	{"nqn", offsetof(struct rpc_delete_subsystem, nqn), spdk_json_decode_string},           /* [한국어] 필수: 삭제할 subsystem NQN */
	{"tgt_name", offsetof(struct rpc_delete_subsystem, tgt_name), spdk_json_decode_string, true}, /* [한국어] optional: target 이름 */
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
	struct rpc_delete_subsystem req = { 0 };     /* [한국어] 디코드된 nqn/tgt_name. 스택 할당, 0 초기화 */
	struct spdk_nvmf_subsystem *subsystem;       /* [한국어] 삭제할 subsystem 포인터 */
	struct spdk_nvmf_tgt *tgt;                   /* [한국어] 대상 target 포인터 */
	int rc;                                      /* [한국어] subsystem_stop 반환코드 */

	if (spdk_json_decode_object(params, rpc_nvmf_delete_subsystem_decoders,
				    SPDK_COUNTOF(rpc_nvmf_delete_subsystem_decoders),
				    &req)) {                 /* [한국어] JSON 파라미터 디코드 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;                        /* [한국어] 디코드 실패 — INVALID_PARAMS 에러 응답 */
	}

	if (req.nqn == NULL) {                       /* [한국어] nqn 필수 필드 누락 검사 */
		SPDK_ERRLOG("missing name param\n");
		goto invalid;
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);       /* [한국어] target 룩업 */
	if (!tgt) {                                   /* [한국어] target 미존재 — INTERNAL_ERROR (target이 없으면 더 진행 불가) */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		goto invalid_custom_response;        /* [한국어] 커스텀 에러 응답 이미 송신했으므로 invalid 라벨 경유 안 함 */
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, req.nqn); /* [한국어] NQN으로 subsystem 룩업 */
	if (!subsystem) {                             /* [한국어] 해당 NQN subsystem 없음 */
		goto invalid;
	}

	free_rpc_delete_subsystem(&req);             /* [한국어] 이후 비동기로 진행하므로 req 문자열을 미리 해제 */

	rc = spdk_nvmf_subsystem_stop(subsystem,
				      rpc_nvmf_subsystem_stopped,
				      request);              /* [한국어] subsystem ACTIVE→INACTIVE 비동기 전이. 완료 시 rpc_nvmf_subsystem_stopped 호출 */
	if (rc == -EBUSY) {                          /* [한국어] -EBUSY: 이미 다른 state change 진행 중 — 나중에 재시도 필요 */
		SPDK_ERRLOG("Subsystem currently in another state change try again later.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Subsystem currently in another state change try again later.");
	} else if (rc != 0) {                        /* [한국어] 그 외 에러 — state machine 오류 등 */
		SPDK_ERRLOG("Unable to change state on subsystem. rc=%d\n", rc);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Unable to change state on subsystem. rc=%d", rc);
	}
	/* [한국어] rc=0이면 비동기 stop 시작됨. 응답은 rpc_nvmf_subsystem_stopped에서 송신 */

	return;

invalid:                                         /* [한국어] 표준 INVALID_PARAMS 에러 응답 후 req 해제 */
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
invalid_custom_response:                         /* [한국어] 커스텀 에러 응답 이미 보낸 경우 — req만 해제 */
	free_rpc_delete_subsystem(&req);
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_delete_subsystem"을 RUNTIME state RPC로 등록. */
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

/* [한국어] listen_address JSON sub-object 디코더 배열.
 * traddr만 필수(false), 나머지는 optional(true).
 * JSON 예: {"trtype":"TCP","adrfam":"IPv4","traddr":"192.168.1.1","trsvcid":"4420"} */
static const struct spdk_json_object_decoder rpc_nvmf_listen_address_decoders[] = {
	{"trtype", offsetof(struct rpc_listen_address, trtype), spdk_json_decode_string, true}, /* [한국어] optional: "TCP","RDMA","FC" 등. NULL이면 rpc_listen_address_to_trid에서 에러 */
	{"adrfam", offsetof(struct rpc_listen_address, adrfam), spdk_json_decode_string, true}, /* [한국어] optional: "IPv4","IPv6","IB","FC". NULL이면 기본 IPv4 */
	{"traddr", offsetof(struct rpc_listen_address, traddr), spdk_json_decode_string},       /* [한국어] 필수: IP 주소 또는 FC WWN 등 트랜스포트 주소 */
	{"trsvcid", offsetof(struct rpc_listen_address, trsvcid), spdk_json_decode_string, true}, /* [한국어] optional: TCP/RDMA의 포트 번호 */
};

/*
 * [한국어]
 * decode_rpc_listen_address - JSON 중첩 객체를 rpc_listen_address로 디코드하는 fn
 *
 * @val: "listen_address" 키의 JSON 객체 값.
 * @out: rpc_listen_address 구조체 포인터 (nvmf_rpc_listener_ctx.address 등).
 * @return: 0=성공, 음수=디코드 에러.
 *
 * spdk_json_object_decoder 배열에 함수 포인터로 등록되어 "listen_address" 키에
 * 자동으로 호출되는 중첩 객체 디코더. rpc_nvmf_listen_address_decoders를 내부 사용.
 *
 * 호출 체인:
 *   spdk_json_decode_object (add_listener/remove_listener 디코더)
 *     → [decode_rpc_listen_address] → spdk_json_decode_object (내부)
 */
static int
decode_rpc_listen_address(const struct spdk_json_val *val, void *out)
{
	struct rpc_listen_address *req = (struct rpc_listen_address *)out; /* [한국어] out을 listen_address 포인터로 캐스트 */

	return spdk_json_decode_object(val, rpc_nvmf_listen_address_decoders,
				       SPDK_COUNTOF(rpc_nvmf_listen_address_decoders), req); /* [한국어] 중첩 JSON 객체를 rpc_listen_address 필드로 디코드 */
}

/*
 * [한국어]
 * free_rpc_listen_address - rpc_listen_address의 동적 할당 필드 해제
 *
 * @r: 해제할 rpc_listen_address 구조체 포인터.
 *
 * 네 문자열 필드 모두 spdk_json_decode_string이 malloc한 것이므로 free 필요.
 * 구조체 자체는 포함된 상위 ctx의 일부이므로 해제하지 않는다.
 *
 * 호출 체인:
 *   nvmf_rpc_listener_ctx_free / nvmf_rpc_referral_ctx_free → [free_rpc_listen_address]
 */
static void
free_rpc_listen_address(struct rpc_listen_address *r)
{
	free(r->trtype);   /* [한국어] JSON 디코드로 malloc된 transport type 문자열 해제 */
	free(r->adrfam);   /* [한국어] JSON 디코드로 malloc된 address family 문자열 해제 */
	free(r->traddr);   /* [한국어] JSON 디코드로 malloc된 transport address 문자열 해제 */
	free(r->trsvcid);  /* [한국어] JSON 디코드로 malloc된 service ID 문자열 해제 */
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

/* [한국어] nvmf_subsystem_add_listener JSON 파라미터 디코더 배열.
 * relaxed decode 사용 — 알 수 없는 키는 무시하고 transport-specific 파라미터는
 * opts.transport_specific으로 원본 params를 통째로 전달해 transport가 자체 디코드. */
static const struct spdk_json_object_decoder rpc_nvmf_subsystem_add_listener_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},                                          /* [한국어] 필수: 대상 subsystem NQN */
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), decode_rpc_listen_address},                         /* [한국어] 필수: listen 주소 객체 (trtype/adrfam/traddr/trsvcid) */
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},                          /* [한국어] optional: target 이름 */
	{"secure_channel", offsetof(struct nvmf_rpc_listener_ctx, listener_opts.secure_channel), spdk_json_decode_bool, true},  /* [한국어] optional: TLS 보안 채널 강제 여부 */
	{"ana_state", offsetof(struct nvmf_rpc_listener_ctx, ana_state_str), spdk_json_decode_string, true},                    /* [한국어] optional: 초기 ANA state 문자열 ("optimized"/"non_optimized"/"inaccessible") */
	{"sock_impl", offsetof(struct nvmf_rpc_listener_ctx, listener_opts.sock_impl), spdk_json_decode_string, true},          /* [한국어] optional: 사용할 소켓 구현체 이름 ("posix"/"ssl" 등) */
};

/*
 * [한국어]
 * nvmf_rpc_listener_ctx_free - nvmf_rpc_listener_ctx 모든 동적 할당 필드 해제
 *
 * @ctx: 해제할 ctx 포인터. ctx 자체도 calloc이므로 free(ctx) 포함.
 *
 * 콜백 체인 성공/에러 모든 경로의 끝에서 호출된다.
 * response_sent 여부와 무관하게 항상 호출해야 ctx 누수가 없다.
 *
 * 호출 체인:
 *   nvmf_rpc_listen_resumed / nvmf_rpc_subsystem_listen / nvmf_rpc_stop_listen_async_done
 *   / nvmf_rpc_set_ana_state_done (에러 경로)
 *   / rpc_nvmf_subsystem_add_listener / remove_listener (에러 경로)
 *     → [nvmf_rpc_listener_ctx_free]
 */
static void
nvmf_rpc_listener_ctx_free(struct nvmf_rpc_listener_ctx *ctx)
{
	free(ctx->nqn);                    /* [한국어] subsystem NQN 문자열 해제 */
	free(ctx->tgt_name);               /* [한국어] target 이름 문자열 해제 */
	free_rpc_listen_address(&ctx->address); /* [한국어] address 구조체 내 trtype/adrfam/traddr/trsvcid 해제 */
	free(ctx->ana_state_str);          /* [한국어] ANA state 문자열 해제 */
	free(ctx);                         /* [한국어] ctx 구조체 자체 heap 메모리 해제 */
}

/*
 * [한국어]
 * nvmf_rpc_listen_resumed - listener 변경 후 subsystem resume 완료 콜백
 *
 * @subsystem: 방금 resume된 subsystem.
 * @cb_arg:    nvmf_rpc_listener_ctx 포인터.
 * @status:    0=성공, 음수=resume 실패(거의 발생 안 함).
 *
 * 모든 listener 관련 RPC(ADD/REMOVE/SET_ANA_STATE)의 공통 마지막 콜백.
 * response_sent가 false이면 성공 응답을 보내고, true이면 이미 에러 응답을 보낸
 * 것이므로 아무것도 하지 않는다. 어느 경우든 ctx를 해제한다.
 *
 * 호출 체인:
 *   nvmf_rpc_subsystem_listen / nvmf_rpc_stop_listen_async_done /
 *   nvmf_rpc_set_ana_state_done → spdk_nvmf_subsystem_resume
 *     → [nvmf_rpc_listen_resumed] → spdk_jsonrpc_send_bool_response (성공 시)
 */
static void
nvmf_rpc_listen_resumed(struct spdk_nvmf_subsystem *subsystem,
			void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;  /* [한국어] cb_arg를 listener ctx로 복원 */
	struct spdk_jsonrpc_request *request;

	request = ctx->request;                       /* [한국어] 응답 전에 ctx를 해제하므로 request를 먼저 저장 */
	if (ctx->response_sent) {
		/* If an error occurred, the response has already been sent. */
		/* [한국어] 이미 에러 응답을 보낸 경우 — ctx만 해제하고 종료 */
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	nvmf_rpc_listener_ctx_free(ctx);             /* [한국어] ctx 해제 (request는 위에서 저장했으므로 사용 가능) */

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공 응답 — listener 변경 완료 알림 */
}

/*
 * [한국어]
 * nvmf_rpc_subsystem_listen - subsystem_add_listener_ext 완료 콜백
 *
 * @cb_arg:  nvmf_rpc_listener_ctx 포인터.
 * @status:  0=listener 추가 성공, 음수=실패.
 *
 * spdk_nvmf_subsystem_add_listener_ext 호출 결과를 처리한다.
 * 실패이면 방금 생성한 transport listen socket을 stop_listen으로 되돌린다.
 * 항상 subsystem_resume을 호출하여 subsystem을 ACTIVE로 복귀시킨다.
 * resume 자체가 실패하면 subsystem이 paused로 남게 되는 비복구 상황이다.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_add_listener_ext
 *     → [nvmf_rpc_subsystem_listen] → spdk_nvmf_tgt_stop_listen (실패 시)
 *       → spdk_nvmf_subsystem_resume → nvmf_rpc_listen_resumed
 */
static void
nvmf_rpc_subsystem_listen(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;  /* [한국어] ctx 복원 */

	if (status) {                                 /* [한국어] listener 추가 실패 — transport listen socket 되돌리기 */
		/* Destroy the listener that we just created. Ignore the error code because
		 * the RPC is failing already anyway. */
		/* [한국어] tgt_stop_listen: 방금 열었던 transport listen socket을 닫음. RPC가 이미 실패 중이므로 에러 코드 무시 */
		spdk_nvmf_tgt_stop_listen(ctx->tgt, &ctx->trid);

		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");   /* [한국어] 에러 응답 — listener 추가 실패 */
		ctx->response_sent = true;            /* [한국어] resume 콜백이 중복 응답하지 않도록 표시 */
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) { /* [한국어] subsystem을 ACTIVE로 복귀 */
		if (!ctx->response_sent) {            /* [한국어] resume 실패이고 아직 응답 안 보낸 경우 */
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);      /* [한국어] resume 실패 — ctx 즉시 해제 (콜백은 오지 않음) */
		/* Can't really do anything to recover here - subsystem will remain paused. */
		/* [한국어] resume 실패 시 subsystem이 paused 상태로 남음. 복구 불가. */
	}
}

/*
 * [한국어]
 * nvmf_rpc_stop_listen_async_done - transport_stop_listen_async 완료 콜백
 *
 * @cb_arg:  nvmf_rpc_listener_ctx 포인터.
 * @status:  0=성공, 음수=stop_listen 에러.
 *
 * REMOVE op에서 transport listen socket을 비동기로 닫은 결과를 처리한다.
 * 실패이면 에러 응답을 보내고 subsystem_resume을 통해 active 상태로 복귀한다.
 *
 * 호출 체인:
 *   spdk_nvmf_transport_stop_listen_async
 *     → [nvmf_rpc_stop_listen_async_done] → spdk_nvmf_subsystem_resume
 *       → nvmf_rpc_listen_resumed
 */
static void
nvmf_rpc_stop_listen_async_done(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;  /* [한국어] ctx 복원 */

	if (status) {                                 /* [한국어] transport stop_listen 실패 */
		SPDK_ERRLOG("Unable to stop listener.\n");
		spdk_jsonrpc_send_error_response_fmt(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "error stopping listener: %d", status); /* [한국어] 에러 코드 포함 에러 응답 */
		ctx->response_sent = true;            /* [한국어] resume 콜백 중복 응답 방지 표시 */
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) { /* [한국어] subsystem ACTIVE 복귀 */
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);      /* [한국어] resume 실패 — ctx 즉시 해제 */
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}

/*
 * [한국어]
 * nvmf_rpc_set_ana_state_done - spdk_nvmf_subsystem_set_ana_state 완료 콜백
 *
 * @cb_arg:  nvmf_rpc_listener_ctx 포인터.
 * @status:  0=ANA state 변경 성공, 음수=실패.
 *
 * SET_ANA_STATE op 완료 후 subsystem_resume으로 ACTIVE 상태로 복귀한다.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_set_ana_state
 *     → [nvmf_rpc_set_ana_state_done] → spdk_nvmf_subsystem_resume
 *       → nvmf_rpc_listen_resumed
 */
static void
nvmf_rpc_set_ana_state_done(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;  /* [한국어] ctx 복원 */

	if (status) {                                 /* [한국어] ANA state 변경 실패 */
		SPDK_ERRLOG("Unable to set ANA state.\n");
		spdk_jsonrpc_send_error_response_fmt(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "error setting ANA state: %d", status); /* [한국어] 에러 코드 포함 에러 응답 */
		ctx->response_sent = true;            /* [한국어] resume 콜백 중복 응답 방지 표시 */
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) { /* [한국어] subsystem ACTIVE 복귀 */
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);      /* [한국어] resume 실패 — ctx 즉시 해제 */
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}

/*
 * [한국어]
 * nvmf_rpc_listen_paused - subsystem pause 완료 시 listener 변경 작업 실행
 *
 * @subsystem: pause 완료된 subsystem.
 * @cb_arg:    nvmf_rpc_listener_ctx 포인터.
 * @status:    pause 결과 코드 (보통 무시 — 이미 paused면 바로 콜백).
 *
 * ctx->op 값에 따라 ADD/REMOVE/SET_ANA_STATE 중 하나를 수행한다.
 * subsystem이 paused 상태여야 listener 변경이 안전하게 가능하다.
 * 변경 작업을 시작한 뒤 (비동기) return하거나, 에러 시 break 후 resume을 호출.
 *
 * ADD:
 *   1) 이미 같은 trid의 listener가 있는지 확인 (중복 방지).
 *   2) spdk_nvmf_tgt_listen_ext: transport에 listen socket 생성.
 *   3) spdk_nvmf_subsystem_add_listener_ext: subsystem에 listener 등록
 *      → 콜백 nvmf_rpc_subsystem_listen.
 *
 * REMOVE:
 *   1) spdk_nvmf_subsystem_remove_listener: subsystem의 listener 제거.
 *   2) spdk_nvmf_transport_stop_listen_async: transport listen socket 비동기 종료
 *      → 콜백 nvmf_rpc_stop_listen_async_done.
 *
 * SET_ANA_STATE:
 *   spdk_nvmf_subsystem_set_ana_state: ANA state 변경
 *   → 콜백 nvmf_rpc_set_ana_state_done.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_pause → [nvmf_rpc_listen_paused]
 *     → (ADD) spdk_nvmf_tgt_listen_ext → spdk_nvmf_subsystem_add_listener_ext
 *             → nvmf_rpc_subsystem_listen → spdk_nvmf_subsystem_resume → nvmf_rpc_listen_resumed
 *     → (REMOVE) spdk_nvmf_subsystem_remove_listener
 *             → spdk_nvmf_transport_stop_listen_async → nvmf_rpc_stop_listen_async_done
 *             → spdk_nvmf_subsystem_resume → nvmf_rpc_listen_resumed
 *     → (SET_ANA_STATE) spdk_nvmf_subsystem_set_ana_state → nvmf_rpc_set_ana_state_done
 *             → spdk_nvmf_subsystem_resume → nvmf_rpc_listen_resumed
 */
static void
nvmf_rpc_listen_paused(struct spdk_nvmf_subsystem *subsystem,
		       void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;  /* [한국어] ctx 복원 */
	int rc;

	switch (ctx->op) {                            /* [한국어] op 값에 따라 ADD/REMOVE/SET_ANA_STATE 분기 */
	case NVMF_RPC_LISTEN_ADD:                     /* [한국어] listener 추가 operation */
		if (nvmf_subsystem_find_listener(subsystem, &ctx->trid)) { /* [한국어] 동일 trid의 listener가 이미 있으면 중복 에러 */
			SPDK_ERRLOG("Listener already exists\n");
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;        /* [한국어] 에러 응답 송신 표시 */
			break;                            /* [한국어] switch 탈출 후 resume 진행 */
		}

		rc = spdk_nvmf_tgt_listen_ext(ctx->tgt, &ctx->trid, &ctx->opts); /* [한국어] transport에 소켓 생성 및 listen 시작 */
		if (rc) {                                 /* [한국어] transport listen 실패 (포트 충돌, 주소 없음 등) */
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
			break;
		}

		/* [한국어] transport listen 성공 → subsystem에 listener 등록. 완료 콜백 nvmf_rpc_subsystem_listen */
		spdk_nvmf_subsystem_add_listener_ext(ctx->subsystem, &ctx->trid, nvmf_rpc_subsystem_listen, ctx,
						     &ctx->listener_opts);
		return;                                   /* [한국어] 비동기 처리 중 — resume은 nvmf_rpc_subsystem_listen에서 수행 */
	case NVMF_RPC_LISTEN_REMOVE:                  /* [한국어] listener 제거 operation */
		rc = spdk_nvmf_subsystem_remove_listener(subsystem, &ctx->trid); /* [한국어] subsystem의 listeners 목록에서 제거 */
		if (rc) {                                 /* [한국어] 해당 trid listener가 없거나 제거 실패 */
			SPDK_ERRLOG("Unable to remove listener, rc %d\n", rc);
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
			break;
		}

		/* [한국어] subsystem listener 제거 성공 → transport listen socket을 비동기로 닫음 */
		spdk_nvmf_transport_stop_listen_async(ctx->transport, &ctx->trid, subsystem,
						      nvmf_rpc_stop_listen_async_done, ctx); /* [한국어] 완료 콜백 nvmf_rpc_stop_listen_async_done */
		return;                                   /* [한국어] 비동기 처리 중 — resume은 stop_listen_async_done에서 수행 */
	case NVMF_RPC_LISTEN_SET_ANA_STATE:           /* [한국어] ANA state 변경 operation */
		/* [한국어] 이 listener의 ana_state를 ctx->anagrpid 그룹(0=모든 그룹)에 대해 변경 */
		spdk_nvmf_subsystem_set_ana_state(subsystem, &ctx->trid, ctx->ana_state, ctx->anagrpid,
						  nvmf_rpc_set_ana_state_done, ctx); /* [한국어] 완료 콜백 nvmf_rpc_set_ana_state_done */
		return;                                   /* [한국어] 비동기 처리 중 */
	default:
		SPDK_UNREACHABLE();                       /* [한국어] 도달 불가 — op는 ADD/REMOVE/SET_ANA_STATE 중 하나임 */
	}

	/* [한국어] break로 빠져나온 에러 경로: subsystem을 resume하여 ACTIVE로 복귀 */
	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_listen_resumed, ctx)) { /* [한국어] resume 자체가 실패한 경우 */
		if (!ctx->response_sent) {            /* [한국어] 아직 에러 응답 안 보냈으면 INTERNAL_ERROR 응답 */
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}

		nvmf_rpc_listener_ctx_free(ctx);      /* [한국어] resume 실패 — ctx 즉시 해제 (콜백 안 옴) */
		/* Can't really do anything to recover here - subsystem will remain paused. */
		/* [한국어] resume 실패 시 subsystem이 paused 상태로 영구 고착. 복구 불가 */
	}
}

/*
 * [한국어]
 * rpc_listen_address_to_trid - rpc_listen_address를 spdk_nvme_transport_id로 변환
 *
 * @address: JSON에서 디코드된 listen 주소 구조체 (trtype/adrfam/traddr/trsvcid).
 * @trid:    변환 결과를 저장할 spdk_nvme_transport_id 구조체. 호출자가 할당.
 * @return:  0=성공, -EINVAL=잘못된 trtype/adrfam/traddr/trsvcid.
 *
 * NVMe-oF 스펙의 Transport ID 필드(trtype/adrfam/traddr/trsvcid)로 구성된 TRID를 만든다.
 * traddr/trsvcid는 고정 크기 배열이므로 길이 체크 후 memcpy.
 * adrfam이 NULL이면 기본값 SPDK_NVMF_ADRFAM_IPV4 사용.
 *
 * 호출 컨텍스트: RPC 서버 스레드. rpc_nvmf_subsystem_add/remove_listener에서 호출.
 *
 * 호출 체인:
 *   rpc_nvmf_subsystem_add_listener / remove_listener / listener_set_ana_state
 *     → [rpc_listen_address_to_trid]
 */
static int
rpc_listen_address_to_trid(const struct rpc_listen_address *address,
			   struct spdk_nvme_transport_id *trid)
{
	size_t len;

	memset(trid, 0, sizeof(*trid));               /* [한국어] trid 구조체 전체를 0으로 초기화 (미사용 필드 정리) */

	if (spdk_nvme_transport_id_populate_trstring(trid, address->trtype)) { /* [한국어] trtype 문자열을 trid.trstring 필드에 복사 (예: "TCP") */
		SPDK_ERRLOG("Invalid trtype string: %s\n", address->trtype);
		return -EINVAL;
	}

	if (spdk_nvme_transport_id_parse_trtype(&trid->trtype, address->trtype)) { /* [한국어] "TCP"/"RDMA"/"FC" 등 문자열을 enum으로 파싱 */
		SPDK_ERRLOG("Invalid trtype type: %s\n", address->trtype);
		return -EINVAL;
	}

	if (address->adrfam) {                        /* [한국어] adrfam이 명시된 경우 파싱, 없으면 기본값 */
		if (spdk_nvme_transport_id_parse_adrfam(&trid->adrfam, address->adrfam)) { /* [한국어] "IPv4"/"IPv6"/"IB"/"FC" → enum */
			SPDK_ERRLOG("Invalid adrfam: %s\n", address->adrfam);
			return -EINVAL;
		}
	} else {
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV4; /* [한국어] adrfam 미지정 시 기본 IPv4 사용 (가장 일반적인 경우) */
	}

	len = strlen(address->traddr);                /* [한국어] traddr 문자열 길이 측정 */
	if (len > sizeof(trid->traddr) - 1) {         /* [한국어] trid.traddr 배열 크기 초과 체크 (NUL 포함하여 -1) */
		SPDK_ERRLOG("Transport address longer than %zu characters: %s\n",
			    sizeof(trid->traddr) - 1, address->traddr);
		return -EINVAL;
	}
	memcpy(trid->traddr, address->traddr, len + 1); /* [한국어] NUL 포함하여 복사 */

	trid->trsvcid[0] = '\0';                      /* [한국어] trsvcid 초기화 (NULL 종결 — 미지정 시 빈 문자열) */
	if (address->trsvcid) {                       /* [한국어] trsvcid가 명시된 경우 복사 (TCP/RDMA의 포트 번호) */
		len = strlen(address->trsvcid);
		if (len > sizeof(trid->trsvcid) - 1) { /* [한국어] trsvcid 배열 크기 초과 체크 */
			SPDK_ERRLOG("Transport service id longer than %zu characters: %s\n",
				    sizeof(trid->trsvcid) - 1, address->trsvcid);
			return -EINVAL;
		}
		memcpy(trid->trsvcid, address->trsvcid, len + 1); /* [한국어] NUL 포함하여 복사 */
	}

	return 0;                                     /* [한국어] 변환 성공 */
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
	struct nvmf_rpc_listener_ctx *ctx;             /* [한국어] 비동기 콜백 체인 동안 상태를 들고 다닐 컨텍스트 */
	struct spdk_nvmf_subsystem *subsystem;         /* [한국어] 대상 subsystem */
	struct spdk_nvmf_tgt *tgt;                     /* [한국어] 대상 target */
	int rc;                                        /* [한국어] subsystem_pause 반환코드 */

	ctx = calloc(1, sizeof(*ctx));                 /* [한국어] ctx를 heap에 할당 — 비동기 콜백 체인 동안 살아있어야 함 */
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	ctx->request = request;                        /* [한국어] 응답 송신을 위해 request 포인터 저장 */

	spdk_nvmf_subsystem_listener_opts_init(&ctx->listener_opts, sizeof(ctx->listener_opts)); /* [한국어] listener_opts를 ABI 호환 방식으로 기본값 초기화 (size 필드 설정 포함) */

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_subsystem_add_listener_decoders,
					    SPDK_COUNTOF(rpc_nvmf_subsystem_add_listener_decoders),
					    ctx)) {             /* [한국어] relaxed decode: 알 수 없는 키 무시 (transport-specific 파라미터 포함) */
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);       /* [한국어] target 룩업 */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
	ctx->tgt = tgt;                                /* [한국어] ctx에 tgt 저장 (콜백에서 tgt_stop_listen 등에 사용) */

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn); /* [한국어] NQN으로 subsystem 룩업 */
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->subsystem = subsystem;                    /* [한국어] ctx에 subsystem 저장 (pause/resume 대상) */

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) { /* [한국어] JSON 주소를 NVMe TRID 구조체로 변환 */
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_ADD;               /* [한국어] nvmf_rpc_listen_paused 콜백에서 ADD 분기를 선택하도록 설정 */
	spdk_nvmf_listen_opts_init(&ctx->opts, sizeof(ctx->opts)); /* [한국어] listen_opts ABI 호환 초기화 */
	ctx->opts.transport_specific = params;        /* [한국어] transport-specific 파라미터를 원본 params로 설정 — transport가 자체 디코드 */
	if (spdk_nvmf_subsystem_get_allow_any_host(subsystem) && ctx->listener_opts.secure_channel) {
		/* [한국어] allow_any_host=true이면 secure_channel과 충돌: 인증 없이 연결 가능하므로 TLS 무의미 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Cannot establish secure channel, when 'allow_any_host' is set");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
	ctx->opts.secure_channel = ctx->listener_opts.secure_channel; /* [한국어] secure_channel 옵션을 listen_opts로 복사 */

	if (ctx->ana_state_str) {                     /* [한국어] ANA state 문자열이 지정된 경우 파싱 */
		if (rpc_ana_state_parse(ctx->ana_state_str, &ctx->ana_state)) { /* [한국어] "optimized"/"non_optimized"/"inaccessible" → enum */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			nvmf_rpc_listener_ctx_free(ctx);
			return;
		}
		ctx->listener_opts.ana_state = ctx->ana_state; /* [한국어] 파싱된 ANA state를 listener_opts에 설정 */
	}

	ctx->opts.sock_impl = ctx->listener_opts.sock_impl; /* [한국어] 소켓 구현체 이름(예: "posix","ssl")을 listen_opts로 복사 */

	rc = spdk_nvmf_subsystem_pause(subsystem, 0, nvmf_rpc_listen_paused, ctx); /* [한국어] nsid=0으로 subsystem 전체 pause. 완료 시 nvmf_rpc_listen_paused 호출 */
	if (rc != 0) {                                /* [한국어] pause 시작 실패 (-EBUSY: 다른 state change 진행 중 등) */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
	}
	/* [한국어] rc=0이면 비동기 pause 시작됨. 이후 콜백 체인으로 처리 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_add_listener"를 RUNTIME state RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_subsystem_add_listener", rpc_nvmf_subsystem_add_listener,
		  SPDK_RPC_RUNTIME);

/* [한국어] nvmf_subsystem_remove_listener JSON 파라미터 디코더 배열. */
static const struct spdk_json_object_decoder rpc_nvmf_subsystem_remove_listener_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},                      /* [한국어] 필수: 대상 subsystem NQN */
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), decode_rpc_listen_address},     /* [한국어] 필수: 제거할 listener 주소 */
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},      /* [한국어] optional: target 이름 */
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
	struct nvmf_rpc_listener_ctx *ctx;            /* [한국어] 비동기 콜백 체인 컨텍스트 */
	struct spdk_nvmf_subsystem *subsystem;        /* [한국어] 대상 subsystem */
	struct spdk_nvmf_tgt *tgt;                    /* [한국어] 대상 target */
	int rc;                                       /* [한국어] subsystem_pause 반환코드 */

	ctx = calloc(1, sizeof(*ctx));                /* [한국어] ctx heap 할당 */
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	ctx->request = request;                       /* [한국어] 응답용 request 포인터 저장 */

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_remove_listener_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_remove_listener_decoders),
				    ctx)) {                   /* [한국어] JSON 파라미터 디코드 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);      /* [한국어] target 룩업 */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
	ctx->tgt = tgt;                               /* [한국어] ctx에 tgt 저장 */

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn); /* [한국어] subsystem 룩업 */
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->subsystem = subsystem;                   /* [한국어] ctx에 subsystem 저장 */

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) { /* [한국어] JSON 주소 → TRID 변환 */
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->transport = spdk_nvmf_tgt_get_transport(tgt, ctx->trid.trstring); /* [한국어] trstring("TCP"/"RDMA")으로 transport 룩업 */
	if (!ctx->transport) {
		/* [한국어] 해당 trtype의 transport가 생성되지 않은 경우 — nvmf_create_transport가 선행되어야 함 */
		SPDK_ERRLOG("Unable to find %s transport. The transport must be created first also make sure it is properly registered.\n",
			    ctx->trid.trstring);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_REMOVE;            /* [한국어] nvmf_rpc_listen_paused 콜백에서 REMOVE 분기 선택 */

	rc = spdk_nvmf_subsystem_pause(subsystem, 0, nvmf_rpc_listen_paused, ctx); /* [한국어] subsystem pause 시작. 완료 시 nvmf_rpc_listen_paused(REMOVE) 호출 */
	if (rc != 0) {                                /* [한국어] pause 시작 실패 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
	}
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_remove_listener"를 RUNTIME state RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_subsystem_remove_listener", rpc_nvmf_subsystem_remove_listener,
		  SPDK_RPC_RUNTIME);

/* [한국어] nvmf_discovery_add/remove_referral RPC의 파라미터 컨텍스트.
 * referral은 NVMe-oF Discovery Log Page의 참조 항목으로, 다른 discovery controller를
 * 가리키거나 특정 subsystem NQN을 명시하는 엔트리이다.
 * 이 ctx는 스택 할당이므로 free(ctx) 없이 free 필드만 수행. */
struct nvmf_rpc_referral_ctx {
	char				*tgt_name;
	/* [한국어] 대상 target 이름 (NULL=default).
	 * 설정자: spdk_json_decode_string. 읽는 자: spdk_nvmf_get_tgt().
	 * 값 범위: NULL 또는 존재하는 target 이름. 함수 끝에서 free. */

	struct rpc_listen_address	address;
	/* [한국어] referral의 transport address (trtype/adrfam/traddr/trsvcid).
	 * decode_rpc_listen_address로 디코드됨. nvmf_rpc_referral_ctx_free에서 해제. */

	bool				secure_channel;
	/* [한국어] 이 referral로의 연결에 TLS 보안 채널을 요구할지 여부.
	 * Discovery Log Entry의 TREQ.SECREQ 비트에 해당. */

	char				*subnqn;
	/* [한국어] 이 referral이 가리키는 specific subsystem NQN (optional).
	 * NULL이면 해당 address의 모든 subsystem에 대한 referral.
	 * trid.subnqn으로 복사되어 사용. */
};

/* [한국어] nvmf_discovery_add_referral JSON 파라미터 디코더 배열. */
static const struct spdk_json_object_decoder rpc_nvmf_discovery_add_referral_decoders[] = {
	{"address", offsetof(struct nvmf_rpc_referral_ctx, address), decode_rpc_listen_address},                 /* [한국어] 필수: referral 주소 객체 */
	{"tgt_name", offsetof(struct nvmf_rpc_referral_ctx, tgt_name), spdk_json_decode_string, true},           /* [한국어] optional: target 이름 */
	{"secure_channel", offsetof(struct nvmf_rpc_referral_ctx, secure_channel), spdk_json_decode_bool, true}, /* [한국어] optional: TLS 요구 여부 */
	{"subnqn", offsetof(struct nvmf_rpc_referral_ctx, subnqn), spdk_json_decode_string, true},               /* [한국어] optional: 특정 subsystem NQN */
};

/*
 * [한국어]
 * nvmf_rpc_referral_ctx_free - nvmf_rpc_referral_ctx 동적 할당 필드 해제
 *
 * @ctx: 해제할 ctx (스택 할당이므로 ctx 자체는 free 불필요).
 *
 * 호출 체인:
 *   rpc_nvmf_discovery_add_referral / remove_referral (에러 경로, 성공 경로)
 *     → [nvmf_rpc_referral_ctx_free]
 */
static void
nvmf_rpc_referral_ctx_free(struct nvmf_rpc_referral_ctx *ctx)
{
	free(ctx->tgt_name);                 /* [한국어] JSON 디코드로 malloc된 target 이름 해제 */
	free(ctx->subnqn);                   /* [한국어] JSON 디코드로 malloc된 subnqn 해제 */
	free_rpc_listen_address(&ctx->address); /* [한국어] address 내 trtype/adrfam/traddr/trsvcid 해제 */
}

/*
 * [한국어]
 * rpc_nvmf_discovery_add_referral - "nvmf_discovery_add_referral" RPC 핸들러
 *
 * @request: JSON-RPC 요청 핸들.
 * @params: JSON. address(필수), tgt_name/secure_channel/subnqn(optional).
 * @return: void (응답은 send_bool_response로 전송).
 *
 * NVMe-oF Discovery Log Page에 referral 항목을 추가한다.
 * referral = 다른 discovery controller 또는 target의 주소를 가리키는 포인터 엔트리.
 * host는 discovery log를 읽고 referral을 따라 실제 subsystem을 찾는다.
 *
 * 동기 핸들러 — pause/resume 불필요. ctx는 스택 할당.
 * relaxed decode를 사용하여 unknown 키를 무시한다.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_discovery_add_referral]
 *     → rpc_listen_address_to_trid
 *     → spdk_nvmf_tgt_add_referral
 */
static void
rpc_nvmf_discovery_add_referral(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct nvmf_rpc_referral_ctx ctx = {};       /* [한국어] 스택 할당 — async chain 없으므로 스택 사용 가능 */
	struct spdk_nvme_transport_id trid = {};      /* [한국어] JSON 주소를 변환한 TRID */
	struct spdk_nvmf_tgt *tgt;                   /* [한국어] 대상 target 포인터 */
	struct spdk_nvmf_referral_opts opts = {};     /* [한국어] referral 생성 옵션 (size/trid/secure_channel/allow_any_host) */
	int rc;                                      /* [한국어] snprintf/add_referral 반환 코드 */

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_discovery_add_referral_decoders,
					    SPDK_COUNTOF(rpc_nvmf_discovery_add_referral_decoders),
					    &ctx)) {
		/* [한국어] relaxed 디코드 실패 — 필수 필드(address) 없거나 형식 오류 */
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);      /* [한국어] tgt_name(NULL=default)으로 target 룩업 */
	if (!tgt) {
		/* [한국어] target이 없으면 에러 응답 (nvmf_create_target 선행 필요) */
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	if (rpc_listen_address_to_trid(&ctx.address, &trid)) {
		/* [한국어] trtype/adrfam/traddr/trsvcid 유효성 검사 실패 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	if (ctx.subnqn != NULL) {
		/* [한국어] 특정 subsystem NQN이 지정된 경우 trid.subnqn에 복사 */
		rc = snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", ctx.subnqn);
		if (rc < 0 || (size_t)rc >= sizeof(trid.subnqn)) {
			/* [한국어] subnqn이 최대 길이 초과 (256바이트) */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid subsystem NQN");
			nvmf_rpc_referral_ctx_free(&ctx);
			return;
		}
	}

	if ((trid.trtype == SPDK_NVME_TRANSPORT_TCP ||
	     trid.trtype == SPDK_NVME_TRANSPORT_RDMA) &&
	    !strlen(trid.trsvcid)) {
		/* [한국어] TCP/RDMA는 포트 번호(trsvcid)가 반드시 필요 (FC는 불필요) */
		SPDK_ERRLOG("Service ID is required.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Service ID is required.");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	opts.size = SPDK_SIZEOF(&opts, allow_any_host); /* [한국어] ABI 버전 호환 — allow_any_host까지의 크기 설정 */
	opts.trid = trid;                               /* [한국어] 변환된 TRID를 opts에 복사 */
	opts.secure_channel = ctx.secure_channel;       /* [한국어] TLS 요구 여부 (Discovery Log Entry TREQ.SECREQ 비트) */
	opts.allow_any_host = true;                     /* [한국어] referral은 인증 없이 모든 host에 공개 (Discovery 목적) */

	rc = spdk_nvmf_tgt_add_referral(tgt, &opts);   /* [한국어] target의 referrals 리스트에 새 항목 추가 */
	if (rc != 0) {
		/* [한국어] 추가 실패 (메모리 부족, 중복 등) */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	nvmf_rpc_referral_ctx_free(&ctx);              /* [한국어] 성공 경로: ctx의 동적 할당 필드 해제 */

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공 응답 */
}

/* [한국어] SPDK_RPC_REGISTER: "nvmf_discovery_add_referral"을 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_discovery_add_referral", rpc_nvmf_discovery_add_referral, SPDK_RPC_RUNTIME);

/* [한국어] nvmf_discovery_remove_referral JSON 파라미터 디코더 배열.
 * address(필수), tgt_name/subnqn(optional). secure_channel 불필요 (제거는 trid로 매칭). */
static const struct spdk_json_object_decoder rpc_nvmf_discovery_remove_referral_decoders[] = {
	{"address", offsetof(struct nvmf_rpc_referral_ctx, address), decode_rpc_listen_address},            /* [한국어] 필수: 제거할 referral 주소 */
	{"tgt_name", offsetof(struct nvmf_rpc_referral_ctx, tgt_name), spdk_json_decode_string, true},      /* [한국어] optional: target 이름 */
	{"subnqn", offsetof(struct nvmf_rpc_referral_ctx, subnqn), spdk_json_decode_string, true},          /* [한국어] optional: 특정 subsystem NQN 매칭 */
};

/*
 * [한국어]
 * rpc_nvmf_discovery_remove_referral - "nvmf_discovery_remove_referral" RPC 핸들러
 *
 * @request: JSON-RPC 요청 핸들.
 * @params: JSON. address(필수), tgt_name/subnqn(optional).
 * @return: void.
 *
 * target의 referrals 목록에서 TRID가 일치하는 항목을 제거한다.
 * strict decode (알 수 없는 키 허용 안 함). 동기 핸들러 — pause 불필요.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_discovery_remove_referral]
 *     → rpc_listen_address_to_trid
 *     → spdk_nvmf_tgt_remove_referral
 */
static void
rpc_nvmf_discovery_remove_referral(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct nvmf_rpc_referral_ctx ctx = {};          /* [한국어] 스택 할당 ctx */
	struct spdk_nvme_transport_id trid = {};         /* [한국어] 변환 결과를 받을 TRID */
	struct spdk_nvmf_referral_opts opts = {};        /* [한국어] 제거 매칭에 사용할 opts (trid만 필요) */
	struct spdk_nvmf_tgt *tgt;                      /* [한국어] 대상 target */
	int rc;                                         /* [한국어] snprintf 반환 코드 */

	if (spdk_json_decode_object(params, rpc_nvmf_discovery_remove_referral_decoders,
				    SPDK_COUNTOF(rpc_nvmf_discovery_remove_referral_decoders),
				    &ctx)) {
		/* [한국어] strict decode 실패 — address 없거나 unknown 키 포함 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);          /* [한국어] target 룩업 */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	if (rpc_listen_address_to_trid(&ctx.address, &trid)) {
		/* [한국어] 주소 → TRID 변환 실패 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	if (ctx.subnqn != NULL) {
		/* [한국어] subnqn이 지정된 경우 trid.subnqn에 복사하여 매칭에 사용 */
		rc = snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", ctx.subnqn);
		if (rc < 0 || (size_t)rc >= sizeof(trid.subnqn)) {
			/* [한국어] subnqn 길이 초과 */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid subsystem NQN");
			nvmf_rpc_referral_ctx_free(&ctx);
			return;
		}
	}

	opts.size = SPDK_SIZEOF(&opts, secure_channel); /* [한국어] ABI 버전 — secure_channel 필드까지만 유효로 설정 (제거에는 불필요) */
	opts.trid = trid;                               /* [한국어] 매칭할 TRID 설정 */

	if (spdk_nvmf_tgt_remove_referral(tgt, &opts)) {
		/* [한국어] 일치하는 referral 없음 또는 제거 실패 */
		SPDK_ERRLOG("Failed to remove referral.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to remove a referral.");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	nvmf_rpc_referral_ctx_free(&ctx);               /* [한국어] 성공 경로: ctx 동적 필드 해제 */

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공 응답 */
}

/* [한국어] SPDK_RPC_REGISTER: "nvmf_discovery_remove_referral"을 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_discovery_remove_referral", rpc_nvmf_discovery_remove_referral,
		  SPDK_RPC_RUNTIME);

/*
 * [한국어]
 * dump_nvmf_referral - referral 항목 하나를 JSON 객체로 직렬화
 *
 * @w:       JSON write context (spdk_jsonrpc_begin_result 반환값).
 * @referral: 직렬화할 spdk_nvmf_referral 구조체.
 *
 * {"address": {...}, "secure_channel": bool, "subnqn": "..."} 형태 출력.
 * address는 nvmf_transport_listen_dump_trid로 trtype/adrfam/traddr/trsvcid 덤프.
 * rpc_nvmf_discovery_get_referrals에서 TAILQ_FOREACH로 각 referral에 호출.
 *
 * 호출 체인:
 *   rpc_nvmf_discovery_get_referrals → TAILQ_FOREACH → [dump_nvmf_referral]
 */
static void
dump_nvmf_referral(struct spdk_json_write_ctx *w,
		   struct spdk_nvmf_referral *referral)
{
	spdk_json_write_object_begin(w);               /* [한국어] referral JSON 객체 시작 "{" */

	spdk_json_write_named_object_begin(w, "address"); /* [한국어] "address": { 시작 */
	nvmf_transport_listen_dump_trid(&referral->trid, w); /* [한국어] trtype/adrfam/traddr/trsvcid 필드 출력 */
	spdk_json_write_object_end(w);                 /* [한국어] "address" 객체 닫기 "}" */
	spdk_json_write_named_bool(w, "secure_channel",
				   referral->entry.treq.secure_channel == SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED);
	/* [한국어] "secure_channel": TREQ.SECREQ==Required이면 true. Discovery Log Entry의 TREQ 필드에서 읽음 */
	spdk_json_write_named_string(w, "subnqn", referral->trid.subnqn);
	/* [한국어] "subnqn": referral의 subnqn (빈 문자열이면 all-subsystems referral) */

	spdk_json_write_object_end(w);                 /* [한국어] referral 객체 닫기 "}" */
}

/* [한국어] nvmf_discovery_get_referrals RPC 파라미터 컨텍스트.
 * tgt_name만 선택적으로 받음. 조회 결과는 응답에 직접 기록 후 ctx 해제. */
struct rpc_get_referrals_ctx {
	char *tgt_name;
	/* [한국어] 대상 target 이름. NULL이면 기본 target 사용.
	 * 설정자: spdk_json_decode_string. 읽는 자: spdk_nvmf_get_tgt.
	 * free_rpc_get_referrals_ctx에서 해제. */
};

/* [한국어] nvmf_discovery_get_referrals JSON 파라미터 디코더. */
static const struct spdk_json_object_decoder rpc_nvmf_discovery_get_referrals_decoders[] = {
	{"tgt_name", offsetof(struct rpc_get_referrals_ctx, tgt_name), spdk_json_decode_string, true}, /* [한국어] optional: target 이름 */
};

/*
 * [한국어]
 * free_rpc_get_referrals_ctx - rpc_get_referrals_ctx 해제
 *
 * @ctx: calloc으로 할당된 ctx (ctx 자체도 free).
 *
 * 호출 체인:
 *   rpc_nvmf_discovery_get_referrals (에러 경로, 성공 경로) → [free_rpc_get_referrals_ctx]
 */
static void
free_rpc_get_referrals_ctx(struct rpc_get_referrals_ctx *ctx)
{
	free(ctx->tgt_name); /* [한국어] JSON 디코드로 malloc된 tgt_name 해제 */
	free(ctx);           /* [한국어] ctx 구조체 자체 해제 (calloc으로 할당됨) */
}

/*
 * [한국어]
 * rpc_nvmf_discovery_get_referrals - "nvmf_discovery_get_referrals" RPC 핸들러
 *
 * @request: JSON-RPC 요청.
 * @params: JSON. tgt_name(optional). params 자체가 NULL일 수 있음 (파라미터 생략).
 * @return: void. 결과: referral 배열 [{address,secure_channel,subnqn}, ...].
 *
 * target의 referrals TAILQ를 순회하며 각 항목을 dump_nvmf_referral로 직렬화.
 * 동기 핸들러 — pause 불필요.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_discovery_get_referrals]
 *     → TAILQ_FOREACH → dump_nvmf_referral
 */
static void
rpc_nvmf_discovery_get_referrals(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_get_referrals_ctx *ctx;              /* [한국어] tgt_name을 보관할 ctx */
	struct spdk_nvmf_tgt *tgt;                      /* [한국어] 조회할 target */
	struct spdk_json_write_ctx *w;                  /* [한국어] JSON 응답 직렬화 컨텍스트 */
	struct spdk_nvmf_referral *referral;            /* [한국어] TAILQ_FOREACH 순회 포인터 */

	ctx = calloc(1, sizeof(*ctx));                  /* [한국어] ctx 할당 (params가 없어도 tgt_name=NULL 초기화) */
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	if (params) {
		/* [한국어] params가 있을 때만 디코드 — params 없이 호출 가능 */
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

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);         /* [한국어] tgt_name(NULL=default)으로 target 룩업 */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target");
		free_rpc_get_referrals_ctx(ctx);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);          /* [한국어] JSON 응답 직렬화 시작 */

	spdk_json_write_array_begin(w);                  /* [한국어] 응답 배열 "[" 시작 */

	TAILQ_FOREACH(referral, &tgt->referrals, link) { /* [한국어] target의 모든 referral 항목 순회 */
		dump_nvmf_referral(w, referral);             /* [한국어] 각 referral을 JSON 객체로 직렬화 */
	}

	spdk_json_write_array_end(w);                    /* [한국어] 배열 "]" 닫기 */

	spdk_jsonrpc_end_result(request, w);             /* [한국어] 응답 전송 완료 */

	free_rpc_get_referrals_ctx(ctx);                 /* [한국어] ctx 해제 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_discovery_get_referrals"를 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_discovery_get_referrals", rpc_nvmf_discovery_get_referrals,
		  SPDK_RPC_RUNTIME);

/* [한국어] nvmf_subsystem_listener_set_ana_state JSON 파라미터 디코더 배열.
 * ANA(Asymmetric Namespace Access) state를 특정 listener에 설정한다.
 * NVMe 1.4 spec의 ANA 기능 — multipath 환경에서 경로별 접근 상태를 선언. */
static const struct spdk_json_object_decoder rpc_nvmf_subsystem_listener_set_ana_state_decoders[] =
{
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},                         /* [한국어] 필수: 대상 subsystem NQN */
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), decode_rpc_listen_address},        /* [한국어] 필수: ANA state를 변경할 listener 주소 */
	{"ana_state", offsetof(struct nvmf_rpc_listener_ctx, ana_state_str), spdk_json_decode_string},         /* [한국어] 필수: "optimized"/"non_optimized"/"inaccessible" */
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},         /* [한국어] optional: target 이름 */
	{"anagrpid", offsetof(struct nvmf_rpc_listener_ctx, anagrpid), spdk_json_decode_uint32, true},         /* [한국어] optional: 특정 ANA group ID (0=모든 그룹) */
};

/*
 * [한국어]
 * rpc_ana_state_parse - ANA state 문자열을 enum spdk_nvme_ana_state으로 변환
 *
 * @str:       "optimized" / "non_optimized" / "inaccessible" 문자열.
 * @ana_state: 변환 결과를 저장할 포인터.
 * @return:    0=성공, -EINVAL=NULL 인자, -ENOENT=알 수 없는 문자열.
 *
 * NVMe 1.4 ANA state 이름을 SPDK enum으로 매핑:
 *   optimized     → SPDK_NVME_ANA_OPTIMIZED_STATE     (최적 경로, 정상 I/O)
 *   non_optimized → SPDK_NVME_ANA_NON_OPTIMIZED_STATE (비최적 경로, I/O 가능)
 *   inaccessible  → SPDK_NVME_ANA_INACCESSIBLE_STATE  (접근 불가, I/O 차단)
 *
 * 호출 체인:
 *   rpc_nvmf_subsystem_add_listener / listener_set_ana_state
 *     → [rpc_ana_state_parse]
 */
static int
rpc_ana_state_parse(const char *str, enum spdk_nvme_ana_state *ana_state)
{
	if (ana_state == NULL || str == NULL) { /* [한국어] NULL 인자 보호 */
		return -EINVAL;
	}

	if (strcasecmp(str, "optimized") == 0) {         /* [한국어] 대소문자 무관 비교 */
		*ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;  /* [한국어] 최적 경로 — I/O 우선 경유 대상 */
	} else if (strcasecmp(str, "non_optimized") == 0) {
		*ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE; /* [한국어] 비최적 경로 — failover 시 사용 */
	} else if (strcasecmp(str, "inaccessible") == 0) {
		*ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;  /* [한국어] 접근 불가 상태 선언 */
	} else {
		return -ENOENT;                              /* [한국어] 알 수 없는 ANA state 문자열 */
	}

	return 0;                                        /* [한국어] 변환 성공 */
}

/*
 * [한국어]
 * rpc_nvmf_subsystem_listener_set_ana_state - "nvmf_subsystem_listener_set_ana_state" RPC
 *
 * @request: JSON-RPC 요청.
 * @params: JSON. nqn/listen_address/ana_state(필수), tgt_name/anagrpid(optional).
 * @return: void.
 *
 * 특정 listener의 ANA state를 변경한다. multipath 환경에서 특정 경로를
 * optimized/non_optimized/inaccessible로 전환할 때 사용.
 *
 * 흐름:
 *   ctx calloc → decode → tgt 룩업 → subsystem 룩업 → trid 변환 →
 *   ana_state 파싱 → spdk_nvmf_subsystem_pause (op=SET_ANA_STATE)
 *     → (콜백) nvmf_rpc_listen_paused → spdk_nvmf_subsystem_set_ana_state
 *       → (콜백) nvmf_rpc_set_ana_state_done
 *         → spdk_nvmf_subsystem_resume
 *           → (콜백) nvmf_rpc_listen_resumed → send_bool_response.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_subsystem_listener_set_ana_state]
 *     → spdk_nvmf_subsystem_pause → nvmf_rpc_listen_paused (SET_ANA_STATE 분기)
 */
static void
rpc_nvmf_subsystem_listener_set_ana_state(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct nvmf_rpc_listener_ctx *ctx;               /* [한국어] 비동기 콜백 체인 컨텍스트 */
	struct spdk_nvmf_subsystem *subsystem;           /* [한국어] 대상 subsystem */
	struct spdk_nvmf_tgt *tgt;                       /* [한국어] 대상 target */

	ctx = calloc(1, sizeof(*ctx));                   /* [한국어] heap ctx 할당 — 비동기 콜백 체인 동안 유지 */
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	ctx->request = request;                          /* [한국어] 응답을 보내기 위해 request 저장 */

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_listener_set_ana_state_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_listener_set_ana_state_decoders),
				    ctx)) {
		/* [한국어] strict decode 실패 — 필수 파라미터 없거나 형식 오류 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);          /* [한국어] target 룩업 */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->tgt = tgt;                                  /* [한국어] ctx에 tgt 저장 (콜백에서 사용) */

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn); /* [한국어] NQN으로 subsystem 룩업 */
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Unable to find subsystem with NQN %s",
						     ctx->nqn);
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->subsystem = subsystem;                      /* [한국어] ctx에 subsystem 저장 (pause/resume 대상) */

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) {
		/* [한국어] 주소 → TRID 변환 실패 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	if (rpc_ana_state_parse(ctx->ana_state_str, &ctx->ana_state)) {
		/* [한국어] "optimized"/"non_optimized"/"inaccessible" 이외의 값이면 에러 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_SET_ANA_STATE;        /* [한국어] 콜백에서 SET_ANA_STATE 분기 진입하도록 설정 */

	if (spdk_nvmf_subsystem_pause(subsystem, 0, nvmf_rpc_listen_paused, ctx)) {
		/* [한국어] subsystem pause 시작 실패 (-EBUSY 등) */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
	}
	/* [한국어] 성공 시 비동기 처리 — nvmf_rpc_listen_paused(SET_ANA_STATE) 콜백 대기 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_listener_set_ana_state"를 RUNTIME RPC로 등록. */
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

/* [한국어] nvmf_subsystem_add_ns의 "namespace" sub-object 필드 디코더 배열. */
static const struct spdk_json_object_decoder rpc_nvmf_namespace_decoders[] = {
	{"nsid", offsetof(struct nvmf_rpc_ns_params, nsid), spdk_json_decode_uint32, true},                  /* [한국어] optional: NS ID (0=자동 할당) */
	{"bdev_name", offsetof(struct nvmf_rpc_ns_params, bdev_name), spdk_json_decode_string},              /* [한국어] 필수: 백엔드 bdev 이름 */
	{"ptpl_file", offsetof(struct nvmf_rpc_ns_params, ptpl_file), spdk_json_decode_string, true},        /* [한국어] optional: PR persistence 파일 경로 */
	{"nguid", offsetof(struct nvmf_rpc_ns_params, nguid), decode_ns_nguid, true},                        /* [한국어] optional: 32-hex-char NGUID 문자열 → 16바이트 */
	{"eui64", offsetof(struct nvmf_rpc_ns_params, eui64), decode_ns_eui64, true},                        /* [한국어] optional: 16-hex-char EUI-64 → 8바이트 */
	{"uuid", offsetof(struct nvmf_rpc_ns_params, uuid), spdk_json_decode_uuid, true},                    /* [한국어] optional: UUID 문자열 → spdk_uuid */
	{"anagrpid", offsetof(struct nvmf_rpc_ns_params, anagrpid), spdk_json_decode_uint32, true},          /* [한국어] optional: ANA group ID */
	{"no_auto_visible", offsetof(struct nvmf_rpc_ns_params, no_auto_visible), spdk_json_decode_bool, true}, /* [한국어] optional: 자동 host visibility 비활성화 */
	{"hide_metadata", offsetof(struct nvmf_rpc_ns_params, hide_metadata), spdk_json_decode_bool, true},  /* [한국어] optional: DIF/PI metadata 숨김 */
};

/*
 * [한국어]
 * decode_rpc_ns_params - "namespace" JSON 객체를 nvmf_rpc_ns_params로 디코드하는 래퍼
 *
 * @val: "namespace" 키의 JSON 값 객체.
 * @out: 결과를 저장할 nvmf_rpc_ns_params*.
 * @return: 0=성공, 비0=디코드 실패.
 *
 * spdk_json_decode_object의 함수 포인터 시그니처를 맞추기 위한 래퍼.
 * rpc_nvmf_subsystem_add_ns_decoders의 "namespace" 엔트리 디코더로 등록됨.
 *
 * 호출 체인:
 *   spdk_json_decode_object (rpc_nvmf_subsystem_add_ns_decoders)
 *     → [decode_rpc_ns_params]
 *       → spdk_json_decode_object (rpc_nvmf_namespace_decoders)
 */
static int
decode_rpc_ns_params(const struct spdk_json_val *val, void *out)
{
	struct nvmf_rpc_ns_params *ns_params = out;  /* [한국어] 결과를 저장할 ns_params 포인터로 캐스팅 */

	return spdk_json_decode_object(val, rpc_nvmf_namespace_decoders,
				       SPDK_COUNTOF(rpc_nvmf_namespace_decoders),
				       ns_params);
	/* [한국어] strict decode — unknown key가 있으면 실패. 0=성공, -1=실패. */
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

/* [한국어] nvmf_subsystem_add_ns JSON 파라미터 디코더. */
static const struct spdk_json_object_decoder rpc_nvmf_subsystem_add_ns_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_ns_ctx, nqn), spdk_json_decode_string},                         /* [한국어] 필수: 대상 subsystem NQN */
	{"namespace", offsetof(struct nvmf_rpc_ns_ctx, ns_params), decode_rpc_ns_params},                /* [한국어] 필수: namespace 옵션 sub-object */
	{"tgt_name", offsetof(struct nvmf_rpc_ns_ctx, tgt_name), spdk_json_decode_string, true},         /* [한국어] optional: target 이름 */
};

/*
 * [한국어]
 * nvmf_rpc_ns_ctx_free - nvmf_rpc_ns_ctx 해제
 *
 * @ctx: calloc으로 할당된 ctx (ctx 자체도 free).
 *
 * 호출 체인:
 *   nvmf_rpc_ns_resumed / nvmf_rpc_ns_failback_resumed / 에러 경로
 *     → [nvmf_rpc_ns_ctx_free]
 */
static void
nvmf_rpc_ns_ctx_free(struct nvmf_rpc_ns_ctx *ctx)
{
	free(ctx->nqn);                  /* [한국어] JSON 디코드된 subsystem NQN 해제 */
	free(ctx->tgt_name);             /* [한국어] JSON 디코드된 target 이름 해제 */
	free(ctx->ns_params.bdev_name); /* [한국어] namespace bdev 이름 해제 */
	free(ctx->ns_params.ptpl_file); /* [한국어] Persistent Reservation 파일 경로 해제 */
	free(ctx);                       /* [한국어] ctx 구조체 자체 해제 */
}

/*
 * [한국어]
 * nvmf_rpc_ns_failback_resumed - NS 추가 롤백 후 resume 완료 콜백
 *
 * @subsystem: resume된 subsystem.
 * @cb_arg:    nvmf_rpc_ns_ctx*.
 * @status:    0=정상 resume, 비0=resume 자체 실패.
 *
 * nvmf_rpc_ns_resumed에서 NS 추가 성공 후 resume 실패 → remove_ns → 다시 resume 시
 * 도달하는 콜백. 이미 에러가 발생한 상황이므로 성공/실패 여부에 따라 메시지만 달리한다.
 *
 * 호출 체인:
 *   nvmf_rpc_ns_resumed (resume 실패 롤백 경로)
 *     → spdk_nvmf_subsystem_remove_ns → spdk_nvmf_subsystem_resume
 *       → [nvmf_rpc_ns_failback_resumed]
 */
static void
nvmf_rpc_ns_failback_resumed(struct spdk_nvmf_subsystem *subsystem,
			     void *cb_arg, int status)
{
	struct nvmf_rpc_ns_ctx *ctx = cb_arg;              /* [한국어] 콜백 컨텍스트 */
	struct spdk_jsonrpc_request *request = ctx->request; /* [한국어] 응답 대상 */

	if (status) {
		/* [한국어] rollback resume도 실패 — subsystem이 invalid state로 전락 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to add ns, subsystem in invalid state");
	} else {
		/* [한국어] rollback 후 resume 성공 — 그래도 NS 추가는 실패한 상황이라 에러 반환 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to add ns, subsystem in active state");
	}

	nvmf_rpc_ns_ctx_free(ctx);   /* [한국어] ctx 최종 해제 */
}

/*
 * [한국어]
 * nvmf_rpc_ns_resumed - NS 추가 후 subsystem resume 완료 콜백
 *
 * @subsystem: resume된 subsystem.
 * @cb_arg:    nvmf_rpc_ns_ctx*.
 * @status:    0=정상 resume, 비0=resume 실패.
 *
 * NS 추가 성공 후 resume 완료 시: nsid를 JSON 응답으로 송신.
 * resume 실패이고 NS는 추가됐다면 (response_sent=false):
 *   → NS를 다시 제거하고, 다시 resume → nvmf_rpc_ns_failback_resumed.
 * response_sent=true이면 (NS 추가 실패): ctx만 해제하고 리턴.
 *
 * 호출 체인:
 *   nvmf_rpc_ns_paused → spdk_nvmf_subsystem_resume → [nvmf_rpc_ns_resumed]
 *     (실패 롤백 시) → spdk_nvmf_subsystem_remove_ns → spdk_nvmf_subsystem_resume
 *       → nvmf_rpc_ns_failback_resumed
 */
static void
nvmf_rpc_ns_resumed(struct spdk_nvmf_subsystem *subsystem,
		    void *cb_arg, int status)
{
	struct nvmf_rpc_ns_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request; /* [한국어] 응답 대상 */
	uint32_t nsid = ctx->ns_params.nsid;                 /* [한국어] 추가된 NS의 ID (응답에 포함) */
	bool response_sent = ctx->response_sent;             /* [한국어] ctx 해제 전에 플래그 저장 */
	struct spdk_json_write_ctx *w;
	int rc;

	/* The case where the call to add the namespace was successful, but the subsystem couldn't be resumed. */
	/* [한국어] NS 추가 성공(response_sent=false)이지만 resume 실패 → 롤백 필요 */
	if (status && !ctx->response_sent) {
		rc = spdk_nvmf_subsystem_remove_ns(subsystem, nsid); /* [한국어] 추가된 NS를 다시 제거 (롤백) */
		if (rc != 0) {
			/* [한국어] 제거도 실패 — subsystem 상태 불일치. 에러 응답 후 리턴 */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Unable to add ns, subsystem in invalid state");
			nvmf_rpc_ns_ctx_free(ctx);
			return;
		}

		rc = spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_ns_failback_resumed, ctx);
		/* [한국어] 다시 resume 시도 — 완료 시 nvmf_rpc_ns_failback_resumed에서 에러 응답 */
		if (rc != 0) {
			/* [한국어] resume도 실패 — 즉시 에러 응답 */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
			nvmf_rpc_ns_ctx_free(ctx);
			return;
		}

		return; /* [한국어] 비동기 처리 중 — failback_resumed 대기 */
	}

	nvmf_rpc_ns_ctx_free(ctx); /* [한국어] ctx 해제 (response_sent 여부 무관) */

	if (response_sent) {
		return; /* [한국어] NS 추가 실패로 이미 에러 응답 보냄 — 중복 응답 방지 */
	}

	/* [한국어] 성공 경로: nsid를 JSON 응답으로 반환 */
	w = spdk_jsonrpc_begin_result(request);  /* [한국어] 응답 JSON 시작 */
	spdk_json_write_uint32(w, nsid);         /* [한국어] 할당된 nsid를 응답 값으로 직렬화 */
	spdk_jsonrpc_end_result(request, w);     /* [한국어] 응답 전송 완료 */
}

/*
 * [한국어]
 * nvmf_rpc_ns_paused - subsystem pause 완료 후 NS 추가 실행 콜백
 *
 * @subsystem: PAUSED 상태의 subsystem.
 * @cb_arg:    nvmf_rpc_ns_ctx*.
 * @status:    0=정상 pause, 비0=pause 실패.
 *
 * spdk_nvmf_subsystem_pause 완료 후 호출된다. PAUSED 상태에서만 NS 추가 가능.
 * ns_opts를 ns_params로부터 채우고 spdk_nvmf_subsystem_add_ns_ext 호출.
 * 실패 시 response_sent=true로 설정 후 resume 진행 (성공 시 응답은 nvmf_rpc_ns_resumed에서).
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_pause → [nvmf_rpc_ns_paused]
 *     → spdk_nvmf_subsystem_add_ns_ext
 *     → spdk_nvmf_subsystem_resume → nvmf_rpc_ns_resumed
 */
static void
nvmf_rpc_ns_paused(struct spdk_nvmf_subsystem *subsystem,
		   void *cb_arg, int status)
{
	struct nvmf_rpc_ns_ctx *ctx = cb_arg;              /* [한국어] 콜백 컨텍스트 */
	struct spdk_nvmf_ns_opts ns_opts;                  /* [한국어] NS 추가 옵션 구조체 */

	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts)); /* [한국어] ns_opts를 기본값으로 초기화 (ABI 호환) */
	ns_opts.nsid = ctx->ns_params.nsid;                /* [한국어] 요청된 nsid (0=자동 할당) */
	ns_opts.transport_specific = ctx->params;          /* [한국어] 원본 JSON params — transport가 자체 디코드 */

	SPDK_STATIC_ASSERT(sizeof(ns_opts.nguid) == sizeof(ctx->ns_params.nguid), "size mismatch");
	memcpy(ns_opts.nguid, ctx->ns_params.nguid, sizeof(ns_opts.nguid)); /* [한국어] 16바이트 NGUID 복사 */

	SPDK_STATIC_ASSERT(sizeof(ns_opts.eui64) == sizeof(ctx->ns_params.eui64), "size mismatch");
	memcpy(ns_opts.eui64, ctx->ns_params.eui64, sizeof(ns_opts.eui64)); /* [한국어] 8바이트 EUI-64 복사 */

	if (!spdk_uuid_is_null(&ctx->ns_params.uuid)) {
		ns_opts.uuid = ctx->ns_params.uuid; /* [한국어] UUID가 유효한 경우에만 설정 (null UUID는 무시) */
	}

	ns_opts.anagrpid = ctx->ns_params.anagrpid;         /* [한국어] ANA group ID 설정 */
	ns_opts.no_auto_visible = ctx->ns_params.no_auto_visible; /* [한국어] 자동 host visibility 비활성화 여부 */
	ns_opts.hide_metadata = ctx->ns_params.hide_metadata;   /* [한국어] DIF/PI 메타데이터 숨김 여부 */

	ctx->ns_params.nsid = spdk_nvmf_subsystem_add_ns_ext(subsystem, ctx->ns_params.bdev_name,
			      &ns_opts, sizeof(ns_opts),
			      ctx->ns_params.ptpl_file);
	/* [한국어] bdev_name의 bdev를 NS로 등록. 반환값=할당된 nsid (0이면 실패). */
	if (ctx->ns_params.nsid == 0) {
		/* [한국어] NS 추가 실패 — bdev 없음, nsid 충돌, 최대 NS 수 초과 등 */
		SPDK_ERRLOG("Unable to add namespace\n");
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true; /* [한국어] 에러 응답 송신 표시 — resume 후 nvmf_rpc_ns_resumed에서 중복 응답 방지 */
		goto resume;
	}

resume:
	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_ns_resumed, ctx)) {
		/* [한국어] resume 시작 실패 — ctx를 즉시 해제하고 에러 응답 */
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ns_ctx_free(ctx);
	}
	/* [한국어] 성공 시 비동기 처리 — nvmf_rpc_ns_resumed 콜백 대기 */
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
/*
 * [한국어]
 * rpc_nvmf_subsystem_add_ns - "nvmf_subsystem_add_ns" RPC 핸들러
 *
 * @request: JSON-RPC 요청.
 * @params: JSON. nqn(필수), namespace(필수 sub-object), tgt_name(optional).
 * @return: void. 성공 응답: 할당된 nsid (uint32).
 *
 * subsystem에 새 namespace를 추가한다. NS는 bdev를 백업으로 사용하며,
 * 호스트의 NVMe Read/Write는 해당 bdev I/O로 변환된다.
 * relaxed decode를 사용하여 transport-specific 파라미터를 ns_opts.transport_specific으로 전달.
 *
 * 흐름:
 *   ctx calloc → relaxed decode → tgt/subsystem 룩업 →
 *   spdk_nvmf_subsystem_pause(nsid)
 *     → nvmf_rpc_ns_paused: add_ns_ext
 *       → spdk_nvmf_subsystem_resume
 *         → nvmf_rpc_ns_resumed: nsid 응답 or rollback.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_subsystem_add_ns]
 *     → spdk_nvmf_subsystem_pause → nvmf_rpc_ns_paused
 */
static void
rpc_nvmf_subsystem_add_ns(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct nvmf_rpc_ns_ctx *ctx;                         /* [한국어] 비동기 체인 컨텍스트 */
	struct spdk_nvmf_subsystem *subsystem;               /* [한국어] 대상 subsystem */
	struct spdk_nvmf_tgt *tgt;                           /* [한국어] 대상 target */
	int rc;                                              /* [한국어] pause 반환 코드 */

	ctx = calloc(1, sizeof(*ctx));                       /* [한국어] ctx heap 할당 — 비동기 체인 동안 유지 */
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_subsystem_add_ns_decoders,
					    SPDK_COUNTOF(rpc_nvmf_subsystem_add_ns_decoders), ctx)) {
		/* [한국어] relaxed decode 실패 — 필수 필드(nqn, namespace) 없거나 형식 오류 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_ctx_free(ctx);
		return;
	}

	ctx->request = request;                              /* [한국어] 응답 송신을 위해 request 저장 */
	ctx->params = params;                                /* [한국어] 원본 params 보존 — transport-specific 옵션 전달용 */
	ctx->response_sent = false;                          /* [한국어] 에러 응답 여부 초기화 */

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);             /* [한국어] target 룩업 */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_ns_ctx_free(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn); /* [한국어] NQN으로 subsystem 룩업 */
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_ctx_free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_pause(subsystem, ctx->ns_params.nsid, nvmf_rpc_ns_paused, ctx);
	/* [한국어] nsid를 지정하여 해당 NS만 pause. 0=전체 pause. 완료 콜백: nvmf_rpc_ns_paused */
	if (rc != 0) {
		/* [한국어] pause 시작 실패 (-EBUSY 등) */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ns_ctx_free(ctx);
	}
	/* [한국어] 성공 시 비동기 처리 중 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_add_ns"를 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_subsystem_add_ns", rpc_nvmf_subsystem_add_ns, SPDK_RPC_RUNTIME)

/* [한국어] nvmf_subsystem_set_ns_ana_group RPC의 비동기 컨텍스트.
 * 특정 NS의 ANA group ID를 변경한다. pause → set_ns_ana_group → resume 패턴. */
struct nvmf_rpc_ana_group_ctx {
	char *nqn;
	/* [한국어] 대상 subsystem NQN. 설정자: JSON decode. 해제: nvmf_rpc_ana_group_ctx_free. */

	char *tgt_name;
	/* [한국어] target 이름 (NULL=default). */

	uint32_t nsid;
	/* [한국어] ANA group을 변경할 namespace ID. 0은 유효하지 않음 (필수). */

	uint32_t anagrpid;
	/* [한국어] 새 ANA group ID. 같은 ANA group에 속한 NS는 동일 ana_state를 공유. */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답 송신용 요청 객체. */

	bool response_sent;
	/* [한국어] 에러 경로에서 응답을 이미 보냈는지 표시 (중복 응답 방지). */
};

/* [한국어] nvmf_subsystem_set_ns_ana_group JSON 파라미터 디코더. */
static const struct spdk_json_object_decoder rpc_nvmf_subsystem_set_ns_ana_group_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_ana_group_ctx, nqn), spdk_json_decode_string},                   /* [한국어] 필수: 대상 subsystem NQN */
	{"nsid", offsetof(struct nvmf_rpc_ana_group_ctx, nsid), spdk_json_decode_uint32},                 /* [한국어] 필수: 대상 NS ID */
	{"anagrpid", offsetof(struct nvmf_rpc_ana_group_ctx, anagrpid), spdk_json_decode_uint32},         /* [한국어] 필수: 새 ANA group ID */
	{"tgt_name", offsetof(struct nvmf_rpc_ana_group_ctx, tgt_name), spdk_json_decode_string, true},   /* [한국어] optional: target 이름 */
};

/*
 * [한국어]
 * nvmf_rpc_ana_group_ctx_free - nvmf_rpc_ana_group_ctx 해제
 *
 * @ctx: calloc으로 할당된 ctx.
 *
 * 호출 체인:
 *   nvmf_rpc_anagrpid_resumed / nvmf_rpc_ana_group (에러 경로)
 *     → [nvmf_rpc_ana_group_ctx_free]
 */
static void
nvmf_rpc_ana_group_ctx_free(struct nvmf_rpc_ana_group_ctx *ctx)
{
	free(ctx->nqn);      /* [한국어] 동적 할당된 NQN 해제 */
	free(ctx->tgt_name); /* [한국어] 동적 할당된 target 이름 해제 */
	free(ctx);           /* [한국어] ctx 구조체 자체 해제 */
}

/*
 * [한국어]
 * nvmf_rpc_anagrpid_resumed - ANA group ID 변경 후 resume 완료 콜백
 *
 * @subsystem: resume된 subsystem.
 * @cb_arg:    nvmf_rpc_ana_group_ctx*.
 * @status:    0=정상, 비0=resume 실패.
 *
 * 성공 시 bool true 응답. response_sent=true이면 이미 에러 응답 보낸 상태 — 중복 응답 방지.
 *
 * 호출 체인:
 *   nvmf_rpc_ana_group → spdk_nvmf_subsystem_resume → [nvmf_rpc_anagrpid_resumed]
 */
static void
nvmf_rpc_anagrpid_resumed(struct spdk_nvmf_subsystem *subsystem,
			  void *cb_arg, int status)
{
	struct nvmf_rpc_ana_group_ctx *ctx = cb_arg;              /* [한국어] 콜백 컨텍스트 */
	struct spdk_jsonrpc_request *request = ctx->request;       /* [한국어] 응답 대상 */
	bool response_sent = ctx->response_sent;                   /* [한국어] ctx 해제 전 저장 */

	nvmf_rpc_ana_group_ctx_free(ctx);                          /* [한국어] ctx 해제 */

	if (response_sent) {
		return; /* [한국어] 이미 에러 응답 보냄 — 중복 응답 방지 */
	}

	spdk_jsonrpc_send_bool_response(request, true);            /* [한국어] 성공 응답 */
}

/*
 * [한국어]
 * nvmf_rpc_ana_group - subsystem pause 완료 후 ANA group ID 변경 콜백
 *
 * @subsystem: PAUSED 상태의 subsystem.
 * @cb_arg:    nvmf_rpc_ana_group_ctx*.
 * @status:    0=정상 pause.
 *
 * PAUSED 상태에서 spdk_nvmf_subsystem_set_ns_ana_group으로 nsid의 anagrpid 변경.
 * 실패 시 response_sent=true 후 resume. 성공/실패 모두 resume → nvmf_rpc_anagrpid_resumed.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_pause → [nvmf_rpc_ana_group]
 *     → spdk_nvmf_subsystem_resume → nvmf_rpc_anagrpid_resumed
 */
static void
nvmf_rpc_ana_group(struct spdk_nvmf_subsystem *subsystem,
		   void *cb_arg, int status)
{
	struct nvmf_rpc_ana_group_ctx *ctx = cb_arg;  /* [한국어] 콜백 컨텍스트 */
	int rc;

	rc = spdk_nvmf_subsystem_set_ns_ana_group(subsystem, ctx->nsid, ctx->anagrpid);
	/* [한국어] NS의 ANA group ID 변경. 실패 시 -ENOENT (nsid 없음) 등 */
	if (rc != 0) {
		SPDK_ERRLOG("Unable to change ANA group ID\n");
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;            /* [한국어] 에러 응답 송신 표시 */
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_anagrpid_resumed, ctx)) {
		/* [한국어] resume 시작 실패 — ctx 즉시 해제 */
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_ana_group_ctx_free(ctx);
	}
	/* [한국어] 성공 시 비동기 처리 중 — nvmf_rpc_anagrpid_resumed 대기 */
}

/*
 * [한국어]
 * rpc_nvmf_subsystem_set_ns_ana_group - "nvmf_subsystem_set_ns_ana_group" RPC 핸들러
 *
 * @request: JSON-RPC 요청.
 * @params: JSON. nqn/nsid/anagrpid(필수), tgt_name(optional).
 * @return: void. 성공 응답: bool true.
 *
 * 특정 NS의 ANA group ID를 변경한다. multipath 환경에서 NS를 다른 ANA group으로
 * 이동시켜 경로 설정을 조정. pause → set_ana_group → resume 패턴.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_subsystem_set_ns_ana_group]
 *     → spdk_nvmf_subsystem_pause → nvmf_rpc_ana_group
 */
static void
rpc_nvmf_subsystem_set_ns_ana_group(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct nvmf_rpc_ana_group_ctx *ctx;              /* [한국어] 비동기 체인 컨텍스트 */
	struct spdk_nvmf_subsystem *subsystem;           /* [한국어] 대상 subsystem */
	struct spdk_nvmf_tgt *tgt;                       /* [한국어] 대상 target */
	int rc;                                          /* [한국어] pause 반환 코드 */

	ctx = calloc(1, sizeof(*ctx));                   /* [한국어] ctx heap 할당 */
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_set_ns_ana_group_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_set_ns_ana_group_decoders), ctx)) {
		/* [한국어] strict decode 실패 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ana_group_ctx_free(ctx);
		return;
	}

	ctx->request = request;                          /* [한국어] 응답 송신용 request 저장 */
	ctx->response_sent = false;                      /* [한국어] 중복 응답 방지 플래그 초기화 */

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);          /* [한국어] target 룩업 */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_ana_group_ctx_free(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn); /* [한국어] NQN으로 subsystem 룩업 */
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ana_group_ctx_free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_pause(subsystem, ctx->nsid, nvmf_rpc_ana_group, ctx);
	/* [한국어] 해당 nsid를 pause. 완료 콜백: nvmf_rpc_ana_group */
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ana_group_ctx_free(ctx);
	}
	/* [한국어] 성공 시 비동기 처리 중 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_set_ns_ana_group"를 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_subsystem_set_ns_ana_group", rpc_nvmf_subsystem_set_ns_ana_group,
		  SPDK_RPC_RUNTIME)

/* [한국어] nvmf_subsystem_remove_ns RPC의 비동기 컨텍스트.
 * pause → remove_ns(nsid) → resume 패턴. response_sent로 중복 응답 방지. */
struct nvmf_rpc_remove_ns_ctx {
	char *nqn;
	/* [한국어] 대상 subsystem NQN. 해제: nvmf_rpc_remove_ns_ctx_free. */

	char *tgt_name;
	/* [한국어] target 이름 (NULL=default). */

	uint32_t nsid;
	/* [한국어] 제거할 namespace ID (필수, 0은 유효하지 않음). */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답 송신용 RPC 요청 객체. */

	bool response_sent;
	/* [한국어] 에러 응답 이미 보냄 여부 — 중복 응답 방지. */
};

/* [한국어] nvmf_subsystem_remove_ns JSON 파라미터 디코더. */
static const struct spdk_json_object_decoder rpc_nvmf_subsystem_remove_ns_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_remove_ns_ctx, nqn), spdk_json_decode_string},                  /* [한국어] 필수: 대상 subsystem NQN */
	{"nsid", offsetof(struct nvmf_rpc_remove_ns_ctx, nsid), spdk_json_decode_uint32},                /* [한국어] 필수: 제거할 NS ID */
	{"tgt_name", offsetof(struct nvmf_rpc_remove_ns_ctx, tgt_name), spdk_json_decode_string, true},  /* [한국어] optional: target 이름 */
};

/*
 * [한국어]
 * nvmf_rpc_remove_ns_ctx_free - nvmf_rpc_remove_ns_ctx 해제
 *
 * @ctx: calloc으로 할당된 ctx.
 */
static void
nvmf_rpc_remove_ns_ctx_free(struct nvmf_rpc_remove_ns_ctx *ctx)
{
	free(ctx->nqn);      /* [한국어] 동적 할당된 NQN 해제 */
	free(ctx->tgt_name); /* [한국어] 동적 할당된 target 이름 해제 */
	free(ctx);           /* [한국어] ctx 구조체 자체 해제 */
}

/*
 * [한국어]
 * nvmf_rpc_remove_ns_resumed - NS 제거 후 resume 완료 콜백
 *
 * @subsystem: resume된 subsystem.
 * @cb_arg:    nvmf_rpc_remove_ns_ctx*.
 * @status:    0=정상.
 *
 * 성공이면 bool true 응답. response_sent=true이면 에러 응답 이미 보냄 — 중복 방지.
 *
 * 호출 체인:
 *   nvmf_rpc_remove_ns_paused → spdk_nvmf_subsystem_resume
 *     → [nvmf_rpc_remove_ns_resumed]
 */
static void
nvmf_rpc_remove_ns_resumed(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct nvmf_rpc_remove_ns_ctx *ctx = cb_arg;              /* [한국어] 콜백 컨텍스트 */
	struct spdk_jsonrpc_request *request = ctx->request;       /* [한국어] 응답 대상 */
	bool response_sent = ctx->response_sent;                   /* [한국어] ctx 해제 전 저장 */

	nvmf_rpc_remove_ns_ctx_free(ctx);                         /* [한국어] ctx 해제 */

	if (response_sent) {
		return; /* [한국어] 에러 응답 이미 보냄 — 중복 방지 */
	}

	spdk_jsonrpc_send_bool_response(request, true);            /* [한국어] 성공 응답 */
}

/*
 * [한국어]
 * nvmf_rpc_remove_ns_paused - subsystem pause 완료 후 NS 제거 콜백
 *
 * @subsystem: PAUSED 상태의 subsystem.
 * @cb_arg:    nvmf_rpc_remove_ns_ctx*.
 * @status:    0=정상 pause.
 *
 * PAUSED 상태에서 spdk_nvmf_subsystem_remove_ns 호출.
 * 실패 시 에러 응답 + response_sent=true. 성공/실패 후 resume → nvmf_rpc_remove_ns_resumed.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_pause → [nvmf_rpc_remove_ns_paused]
 *     → spdk_nvmf_subsystem_resume → nvmf_rpc_remove_ns_resumed
 */
static void
nvmf_rpc_remove_ns_paused(struct spdk_nvmf_subsystem *subsystem,
			  void *cb_arg, int status)
{
	struct nvmf_rpc_remove_ns_ctx *ctx = cb_arg; /* [한국어] 콜백 컨텍스트 */
	int ret;

	ret = spdk_nvmf_subsystem_remove_ns(subsystem, ctx->nsid); /* [한국어] NS 제거 (bdev 매핑 해제) */
	if (ret < 0) {
		/* [한국어] nsid 없음, 이미 제거됨 등 */
		SPDK_ERRLOG("Unable to remove namespace ID %u\n", ctx->nsid);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;           /* [한국어] 에러 응답 송신 표시 */
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_remove_ns_resumed, ctx)) {
		/* [한국어] resume 시작 실패 — ctx 즉시 해제 */
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
		nvmf_rpc_remove_ns_ctx_free(ctx);
	}
	/* [한국어] 성공 시 비동기 처리 중 */
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
/*
 * [한국어]
 * rpc_nvmf_subsystem_remove_ns - "nvmf_subsystem_remove_ns" RPC 핸들러
 *
 * @request: JSON-RPC 요청.
 * @params: JSON. nqn/nsid(필수), tgt_name(optional).
 * @return: void. 성공: bool true.
 *
 * subsystem에서 NS를 제거. NS 제거 후 connected ctrlr에 NS Attribute Changed AEN 발생 가능.
 * pause(nsid) → remove_ns → resume → bool_response.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_subsystem_remove_ns]
 *     → spdk_nvmf_subsystem_pause → nvmf_rpc_remove_ns_paused
 */
static void
rpc_nvmf_subsystem_remove_ns(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct nvmf_rpc_remove_ns_ctx *ctx;              /* [한국어] 비동기 체인 컨텍스트 */
	struct spdk_nvmf_subsystem *subsystem;           /* [한국어] 대상 subsystem */
	struct spdk_nvmf_tgt *tgt;                       /* [한국어] 대상 target */
	int rc;                                          /* [한국어] pause 반환 코드 */

	ctx = calloc(1, sizeof(*ctx));                   /* [한국어] ctx heap 할당 */
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_remove_ns_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_remove_ns_decoders),
				    ctx)) {
		/* [한국어] strict decode 실패 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_remove_ns_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);          /* [한국어] target 룩업 */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_remove_ns_ctx_free(ctx);
		return;
	}

	ctx->request = request;                           /* [한국어] 응답 송신용 request 저장 */
	ctx->response_sent = false;                       /* [한국어] 중복 응답 방지 플래그 초기화 */

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn); /* [한국어] NQN으로 subsystem 룩업 */
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_remove_ns_ctx_free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_pause(subsystem, ctx->nsid, nvmf_rpc_remove_ns_paused, ctx);
	/* [한국어] 해당 nsid를 pause. 0이면 전체 pause. 완료 콜백: nvmf_rpc_remove_ns_paused */
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_remove_ns_ctx_free(ctx);
	}
	/* [한국어] 성공 시 비동기 처리 중 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_remove_ns"를 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_subsystem_remove_ns", rpc_nvmf_subsystem_remove_ns, SPDK_RPC_RUNTIME)

/* [한국어] nvmf_ns_add_host / nvmf_ns_remove_host RPC 공통 컨텍스트.
 * no_auto_visible로 생성된 NS에 특정 host NQN의 visibility를 add/remove한다.
 * 공통 함수 nvmf_rpc_ns_visible(visible=true/false)로 구현 공유. */
struct nvmf_rpc_ns_visible_ctx {
	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답 송신용 RPC 요청 객체. */

	char *nqn;
	/* [한국어] 대상 subsystem NQN. 해제: nvmf_rpc_ns_visible_ctx_free. */

	uint32_t nsid;
	/* [한국어] 대상 namespace ID. */

	char *host;
	/* [한국어] visibility를 부여/제거할 host NQN. */

	char *tgt_name;
	/* [한국어] target 이름 (NULL=default). */

	bool visible;
	/* [한국어] true=add_host, false=remove_host. nvmf_rpc_ns_visible에서 설정. */

	bool response_sent;
	/* [한국어] 에러 응답 이미 보냄 여부. */
};

/* [한국어] nvmf_ns_add_host / remove_host 공용 JSON 파라미터 디코더. */
static const struct spdk_json_object_decoder rpc_nvmf_ns_add_host_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_ns_visible_ctx, nqn), spdk_json_decode_string},                  /* [한국어] 필수: 대상 subsystem NQN */
	{"nsid", offsetof(struct nvmf_rpc_ns_visible_ctx, nsid), spdk_json_decode_uint32},                /* [한국어] 필수: 대상 NS ID */
	{"host", offsetof(struct nvmf_rpc_ns_visible_ctx, host), spdk_json_decode_string},                /* [한국어] 필수: host NQN */
	{"tgt_name", offsetof(struct nvmf_rpc_ns_visible_ctx, tgt_name), spdk_json_decode_string, true},  /* [한국어] optional: target 이름 */
};

/*
 * [한국어]
 * nvmf_rpc_ns_visible_ctx_free - nvmf_rpc_ns_visible_ctx 해제
 */
static void
nvmf_rpc_ns_visible_ctx_free(struct nvmf_rpc_ns_visible_ctx *ctx)
{
	free(ctx->nqn);       /* [한국어] 동적 할당된 NQN 해제 */
	free(ctx->host);      /* [한국어] 동적 할당된 host NQN 해제 */
	free(ctx->tgt_name);  /* [한국어] 동적 할당된 target 이름 해제 */
	free(ctx);            /* [한국어] ctx 구조체 자체 해제 */
}

/*
 * [한국어]
 * nvmf_rpc_ns_visible_resumed - NS visibility 변경 후 resume 완료 콜백
 *
 * @subsystem: resume된 subsystem.
 * @cb_arg:    nvmf_rpc_ns_visible_ctx*.
 * @status:    0=정상.
 *
 * response_sent=false이면 bool true 응답. true이면 이미 에러 응답 보냄.
 *
 * 호출 체인:
 *   nvmf_rpc_ns_visible_paused → spdk_nvmf_subsystem_resume
 *     → [nvmf_rpc_ns_visible_resumed]
 */
static void
nvmf_rpc_ns_visible_resumed(struct spdk_nvmf_subsystem *subsystem,
			    void *cb_arg, int status)
{
	struct nvmf_rpc_ns_visible_ctx *ctx = cb_arg;             /* [한국어] 콜백 컨텍스트 */
	struct spdk_jsonrpc_request *request = ctx->request;       /* [한국어] 응답 대상 */
	bool response_sent = ctx->response_sent;                   /* [한국어] ctx 해제 전 저장 */

	nvmf_rpc_ns_visible_ctx_free(ctx);                        /* [한국어] ctx 해제 */

	if (!response_sent) {
		spdk_jsonrpc_send_bool_response(request, true);        /* [한국어] 성공 응답 */
	}
	/* [한국어] response_sent=true이면 에러 응답 이미 보냄 — 여기서 추가 응답 없음 */
}

/*
 * [한국어]
 * nvmf_rpc_ns_visible_paused - pause 완료 후 NS host visibility 변경 콜백
 *
 * @subsystem: PAUSED 상태의 subsystem.
 * @cb_arg:    nvmf_rpc_ns_visible_ctx*.
 * @status:    0=정상 pause.
 *
 * ctx->visible에 따라 spdk_nvmf_ns_add_host 또는 remove_host 호출.
 * 실패 시 response_sent=true + 에러 응답. 이후 resume.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_pause → [nvmf_rpc_ns_visible_paused]
 *     → spdk_nvmf_subsystem_resume → nvmf_rpc_ns_visible_resumed
 */
static void
nvmf_rpc_ns_visible_paused(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct nvmf_rpc_ns_visible_ctx *ctx = cb_arg; /* [한국어] 콜백 컨텍스트 */
	int ret;

	if (ctx->visible) {
		ret = spdk_nvmf_ns_add_host(subsystem, ctx->nsid, ctx->host, 0);
		/* [한국어] visible=true: 해당 host NQN에게 NS visibility 부여 */
	} else {
		ret = spdk_nvmf_ns_remove_host(subsystem, ctx->nsid, ctx->host, 0);
		/* [한국어] visible=false: 해당 host NQN의 NS visibility 제거 */
	}
	if (ret < 0) {
		/* [한국어] nsid 없음, host NQN 없음 등의 실패 */
		SPDK_ERRLOG("Unable to add/remove %s to namespace ID %u\n", ctx->host, ctx->nsid);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;           /* [한국어] 에러 응답 송신 표시 */
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_ns_visible_resumed, ctx)) {
		/* [한국어] resume 시작 실패 — ctx 즉시 해제 */
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
		nvmf_rpc_ns_visible_ctx_free(ctx);
	}
	/* [한국어] 성공 시 비동기 처리 중 */
}

/*
 * [한국어]
 * nvmf_rpc_ns_visible - nvmf_ns_add_host/remove_host 공통 구현
 *
 * @request: JSON-RPC 요청.
 * @params:  JSON. nqn/nsid/host(필수), tgt_name(optional).
 * @visible: true=add_host, false=remove_host.
 *
 * no_auto_visible 설정된 NS에 특정 host의 접근 권한을 추가 또는 제거.
 * pause(nsid) → ns_add/remove_host → resume → bool_response.
 *
 * 호출 체인:
 *   rpc_nvmf_ns_add_host / rpc_nvmf_ns_remove_host
 *     → [nvmf_rpc_ns_visible]
 *       → spdk_nvmf_subsystem_pause → nvmf_rpc_ns_visible_paused
 */
static void
nvmf_rpc_ns_visible(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params,
		    bool visible)
{
	struct nvmf_rpc_ns_visible_ctx *ctx;             /* [한국어] 비동기 체인 컨텍스트 */
	struct spdk_nvmf_subsystem *subsystem;           /* [한국어] 대상 subsystem */
	struct spdk_nvmf_tgt *tgt;                       /* [한국어] 대상 target */
	int rc;                                          /* [한국어] pause 반환 코드 */

	ctx = calloc(1, sizeof(*ctx));                   /* [한국어] ctx heap 할당 */
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	ctx->visible = visible;                          /* [한국어] add=true / remove=false 설정 */

	if (spdk_json_decode_object(params, rpc_nvmf_ns_add_host_decoders,
				    SPDK_COUNTOF(rpc_nvmf_ns_add_host_decoders),
				    ctx)) {
		/* [한국어] strict decode 실패 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_visible_ctx_free(ctx);
		return;
	}
	ctx->request = request;                          /* [한국어] 응답 대상 저장 */

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);          /* [한국어] target 룩업 */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_ns_visible_ctx_free(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn); /* [한국어] NQN으로 subsystem 룩업 */
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_visible_ctx_free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_pause(subsystem, ctx->nsid, nvmf_rpc_ns_visible_paused, ctx);
	/* [한국어] 해당 nsid를 pause. 완료 콜백: nvmf_rpc_ns_visible_paused */
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ns_visible_ctx_free(ctx);
	}
	/* [한국어] 성공 시 비동기 처리 중 */
}

/*
 * [한국어]
 * rpc_nvmf_ns_add_host - "nvmf_ns_add_host" RPC 핸들러
 * nvmf_rpc_ns_visible(visible=true) 호출 — host NQN에게 NS visibility 부여.
 */
static void
rpc_nvmf_ns_add_host(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	nvmf_rpc_ns_visible(request, params, true); /* [한국어] visible=true → ns_add_host */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_ns_add_host"를 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_ns_add_host", rpc_nvmf_ns_add_host, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_nvmf_ns_remove_host - "nvmf_ns_remove_host" RPC 핸들러
 * nvmf_rpc_ns_visible(visible=false) 호출 — host NQN의 NS visibility 제거.
 */
static void
rpc_nvmf_ns_remove_host(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	nvmf_rpc_ns_visible(request, params, false); /* [한국어] visible=false → ns_remove_host */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_ns_remove_host"를 RUNTIME RPC로 등록. */
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

/* [한국어] nvmf_subsystem_add_host / set_keys 공용 JSON 파라미터 디코더. */
static const struct spdk_json_object_decoder rpc_nvmf_subsystem_add_host_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_host_ctx, nqn), spdk_json_decode_string},                            /* [한국어] 필수: 대상 subsystem NQN */
	{"host", offsetof(struct nvmf_rpc_host_ctx, host), spdk_json_decode_string},                          /* [한국어] 필수: 추가/변경할 host NQN */
	{"tgt_name", offsetof(struct nvmf_rpc_host_ctx, tgt_name), spdk_json_decode_string, true},            /* [한국어] optional: target 이름 */
	{"dhchap_key", offsetof(struct nvmf_rpc_host_ctx, dhchap_key), spdk_json_decode_string, true},        /* [한국어] optional: H→C 인증 키 이름 */
	{"dhchap_ctrlr_key", offsetof(struct nvmf_rpc_host_ctx, dhchap_ctrlr_key), spdk_json_decode_string, true}, /* [한국어] optional: C→H 양방향 인증 키 이름 */
};

/*
 * [한국어]
 * nvmf_rpc_host_ctx_free - nvmf_rpc_host_ctx 동적 필드 해제
 *
 * @ctx: 스택 또는 heap 할당 ctx. ctx 구조체 자체는 해제하지 않음.
 *       (heap 할당 시 별도로 free(ctx) 필요)
 *
 * 호출 체인:
 *   rpc_nvmf_subsystem_add_host / remove_host / set_keys / allow_any_host (out 라벨)
 *     → [nvmf_rpc_host_ctx_free]
 */
static void
nvmf_rpc_host_ctx_free(struct nvmf_rpc_host_ctx *ctx)
{
	free(ctx->nqn);              /* [한국어] 동적 할당된 subsystem NQN 해제 */
	free(ctx->host);             /* [한국어] 동적 할당된 host NQN 해제 */
	free(ctx->tgt_name);         /* [한국어] 동적 할당된 target 이름 해제 */
	free(ctx->dhchap_key);       /* [한국어] 동적 할당된 DH-HMAC-CHAP 키 이름 해제 */
	free(ctx->dhchap_ctrlr_key); /* [한국어] 동적 할당된 controller 인증 키 이름 해제 */
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
	struct spdk_key *key = NULL, *ckey = NULL; /* [한국어] keyring에서 룩업한 DH-HMAC-CHAP 키 포인터 */
	int rc;                                    /* [한국어] add_host_ext 반환 코드 */

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_subsystem_add_host_decoders,
					    SPDK_COUNTOF(rpc_nvmf_subsystem_add_host_decoders),
					    &ctx)) {
		/* [한국어] relaxed decode 실패 — 필수 필드 없거나 형식 오류 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);    /* [한국어] target 룩업 */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		goto out;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx.nqn); /* [한국어] NQN으로 subsystem 룩업 */
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx.nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	if (ctx.dhchap_key != NULL) {
		/* [한국어] DH-HMAC-CHAP 키 이름이 지정된 경우 keyring에서 룩업 */
		key = spdk_keyring_get_key(ctx.dhchap_key);
		/* [한국어] SPDK keyring에서 등록된 키 참조 카운트 증가 후 반환 */
		if (key == NULL) {
			/* [한국어] keyring에 없는 키 이름 — nvmf_keyring_add_key로 사전 등록 필요 */
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP key: %s\n", ctx.dhchap_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}

	if (ctx.dhchap_ctrlr_key != NULL) {
		/* [한국어] 양방향 인증용 controller→host 키 룩업 */
		ckey = spdk_keyring_get_key(ctx.dhchap_ctrlr_key);
		if (ckey == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP ctrlr key: %s\n",
				    ctx.dhchap_ctrlr_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}

	opts.size = SPDK_SIZEOF(&opts, dhchap_ctrlr_key); /* [한국어] ABI 버전 — dhchap_ctrlr_key까지 유효 */
	opts.params = params;                              /* [한국어] 원본 JSON params (transport-specific 옵션 전달) */
	opts.dhchap_key = key;                            /* [한국어] H→C 인증 키 (NULL이면 인증 없음) */
	opts.dhchap_ctrlr_key = ckey;                     /* [한국어] C→H 양방향 인증 키 (NULL이면 단방향) */
	rc = spdk_nvmf_subsystem_add_host_ext(subsystem, ctx.host, &opts);
	/* [한국어] host NQN을 subsystem ACL에 추가. 이후 connect 허용. */
	if (rc != 0) {
		/* [한국어] 이미 존재하는 host NQN 중복 추가 등 실패 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공 응답 */
out:
	spdk_keyring_put_key(ckey); /* [한국어] ckey 참조 카운트 감소 (put_key). NULL이면 no-op */
	spdk_keyring_put_key(key);  /* [한국어] key 참조 카운트 감소. opts 사용 완료 후 반납 */
	nvmf_rpc_host_ctx_free(&ctx); /* [한국어] ctx 동적 필드 해제 (ctx 자체는 스택이므로 free 불필요) */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_add_host"를 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_subsystem_add_host", rpc_nvmf_subsystem_add_host, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_nvmf_subsystem_remove_host_done - remove_host 후 disconnect 완료 콜백
 *
 * @_ctx:   nvmf_rpc_host_ctx* (heap 할당).
 * @status: 0=성공.
 *
 * spdk_nvmf_subsystem_disconnect_host 완료 시 호출. 모든 기존 연결 끊기 완료 후
 * bool true 응답 송신 및 ctx 해제.
 *
 * 호출 체인:
 *   rpc_nvmf_subsystem_remove_host → spdk_nvmf_subsystem_disconnect_host
 *     → [rpc_nvmf_subsystem_remove_host_done]
 */
static void
rpc_nvmf_subsystem_remove_host_done(void *_ctx, int status)
{
	struct nvmf_rpc_host_ctx *ctx = _ctx;          /* [한국어] heap 할당된 ctx */

	spdk_jsonrpc_send_bool_response(ctx->request, true); /* [한국어] 성공 응답 */
	nvmf_rpc_host_ctx_free(ctx);                         /* [한국어] 동적 필드 해제 */
	free(ctx);                                           /* [한국어] ctx 구조체 자체 해제 (calloc으로 할당) */
}

/* [한국어] nvmf_subsystem_remove_host JSON 파라미터 디코더. dhchap_key 불필요 (제거는 NQN 매칭). */
static const struct spdk_json_object_decoder rpc_nvmf_subsystem_remove_host_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_host_ctx, nqn), spdk_json_decode_string},                       /* [한국어] 필수: 대상 subsystem NQN */
	{"host", offsetof(struct nvmf_rpc_host_ctx, host), spdk_json_decode_string},                     /* [한국어] 필수: 제거할 host NQN */
	{"tgt_name", offsetof(struct nvmf_rpc_host_ctx, tgt_name), spdk_json_decode_string, true},       /* [한국어] optional: target 이름 */
};

/*
 * [한국어]
 * rpc_nvmf_subsystem_remove_host - "nvmf_subsystem_remove_host" RPC 핸들러
 *
 * @request: JSON-RPC 요청.
 * @params: JSON. nqn/host(필수), tgt_name(optional).
 * @return: void. 성공: bool true (disconnect 완료 후).
 *
 * 1단계: subsystem ACL에서 host NQN 제거 (spdk_nvmf_subsystem_remove_host).
 * 2단계: 해당 host의 기존 연결 모두 끊기 (spdk_nvmf_subsystem_disconnect_host).
 *         완료 콜백 rpc_nvmf_subsystem_remove_host_done에서 응답 송신.
 * ctx는 heap 할당 — disconnect 완료 콜백에서 해제.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_subsystem_remove_host]
 *     → spdk_nvmf_subsystem_remove_host (ACL 제거)
 *     → spdk_nvmf_subsystem_disconnect_host (연결 끊기)
 *       → rpc_nvmf_subsystem_remove_host_done
 */
static void
rpc_nvmf_subsystem_remove_host(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx *ctx;                   /* [한국어] heap 할당 ctx — disconnect 콜백 대기 */
	struct spdk_nvmf_subsystem *subsystem;           /* [한국어] 대상 subsystem */
	struct spdk_nvmf_tgt *tgt;                       /* [한국어] 대상 target */
	int rc;                                          /* [한국어] remove_host/disconnect_host 반환 코드 */

	ctx = calloc(1, sizeof(*ctx));                   /* [한국어] heap 할당 — disconnect 콜백까지 살아있어야 함 */
	if (ctx == NULL) {
		SPDK_ERRLOG("Unable to allocate context to perform RPC\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	ctx->request = request;                          /* [한국어] 응답 송신용 request 저장 */

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_remove_host_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_remove_host_decoders),
				    ctx)) {
		/* [한국어] strict decode 실패 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);          /* [한국어] target 룩업 */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn); /* [한국어] NQN으로 subsystem 룩업 */
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_remove_host(subsystem, ctx->host);
	/* [한국어] 1단계: ACL 목록에서 host NQN 제거. 이후 새 Connect는 거부됨. */
	if (rc != 0) {
		/* [한국어] ACL 목록에 없는 host NQN이거나 제거 실패 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_disconnect_host(subsystem, ctx->host,
			rpc_nvmf_subsystem_remove_host_done,
			ctx);
	/* [한국어] 2단계: 이미 연결된 qpairs를 비동기로 모두 끊음. 완료 콜백: remove_host_done */
	if (rc != 0) {
		/* [한국어] disconnect 요청 실패 (이미 연결 없는 경우는 rc=0으로 즉시 완료될 수 있음) */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}
	/* [한국어] 성공 시 비동기 처리 중 — disconnect 완료 콜백 대기 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_remove_host"를 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_subsystem_remove_host", rpc_nvmf_subsystem_remove_host,
		  SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * rpc_nvmf_subsystem_set_keys - "nvmf_subsystem_set_keys" RPC 핸들러
 *
 * @request: JSON-RPC 요청.
 * @params: JSON. nqn/host(필수), tgt_name/dhchap_key/dhchap_ctrlr_key(optional).
 * @return: void. 성공: bool true.
 *
 * 이미 등록된 host의 DH-HMAC-CHAP 키를 변경. add_host_decoders를 재사용하며
 * spdk_nvmf_subsystem_set_keys로 기존 host 항목의 키만 업데이트.
 * keyring get/put 패턴: 함수 진입 시 get, out 라벨에서 put.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_subsystem_set_keys]
 *     → spdk_nvmf_subsystem_set_keys → spdk_keyring_put_key (out)
 */
static void
rpc_nvmf_subsystem_set_keys(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx ctx = {};              /* [한국어] 스택 ctx (동기 핸들러) */
	struct spdk_nvmf_subsystem *subsystem;          /* [한국어] 대상 subsystem */
	struct spdk_nvmf_subsystem_key_opts opts = {};  /* [한국어] 키 변경 옵션 */
	struct spdk_nvmf_tgt *tgt;                      /* [한국어] 대상 target */
	struct spdk_key *key = NULL, *ckey = NULL;      /* [한국어] keyring에서 룩업한 키 포인터 */
	int rc;                                         /* [한국어] set_keys 반환 코드 */

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_add_host_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_add_host_decoders), &ctx)) {
		/* [한국어] strict decode 실패 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);          /* [한국어] target 룩업 */
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Invalid parameters");
		goto out;
	}
	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx.nqn); /* [한국어] subsystem 룩업 */
	if (!subsystem) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	if (ctx.dhchap_key != NULL) {
		/* [한국어] H→C 인증 키 룩업 */
		key = spdk_keyring_get_key(ctx.dhchap_key);
		if (key == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP key: %s\n", ctx.dhchap_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}
	if (ctx.dhchap_ctrlr_key != NULL) {
		/* [한국어] C→H 양방향 인증 키 룩업 */
		ckey = spdk_keyring_get_key(ctx.dhchap_ctrlr_key);
		if (ckey == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP ctrlr key: %s\n",
				    ctx.dhchap_ctrlr_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}

	opts.size = SPDK_SIZEOF(&opts, dhchap_ctrlr_key); /* [한국어] ABI 버전 설정 */
	opts.dhchap_key = key;                             /* [한국어] 새 H→C 키 (NULL이면 인증 제거) */
	opts.dhchap_ctrlr_key = ckey;                      /* [한국어] 새 C→H 키 */
	rc = spdk_nvmf_subsystem_set_keys(subsystem, ctx.host, &opts);
	/* [한국어] 기존 host ACL 항목의 키를 새 키로 교체. -ENOENT=host 없음. */
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		/* [한국어] 에러 번호를 그대로 응답 코드로 사용 (ENOENT 등 구체적 에러 전달) */
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);  /* [한국어] 성공 응답 */
out:
	spdk_keyring_put_key(ckey); /* [한국어] ckey 참조 카운트 감소 */
	spdk_keyring_put_key(key);  /* [한국어] key 참조 카운트 감소 */
	nvmf_rpc_host_ctx_free(&ctx); /* [한국어] ctx 동적 필드 해제 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_set_keys"를 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_subsystem_set_keys", rpc_nvmf_subsystem_set_keys, SPDK_RPC_RUNTIME)

/* [한국어] nvmf_subsystem_allow_any_host JSON 파라미터 디코더.
 * allow_any_host=true이면 host ACL 우회 — 인증 없이 모든 host connect 허용. */
static const struct spdk_json_object_decoder rpc_nvmf_subsystem_allow_any_host_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_host_ctx, nqn), spdk_json_decode_string},                        /* [한국어] 필수: 대상 subsystem NQN */
	{"allow_any_host", offsetof(struct nvmf_rpc_host_ctx, allow_any_host), spdk_json_decode_bool},    /* [한국어] 필수: true=허용, false=ACL 복원 */
	{"tgt_name", offsetof(struct nvmf_rpc_host_ctx, tgt_name), spdk_json_decode_string, true},        /* [한국어] optional: target 이름 */
};

/*
 * [한국어]
 * rpc_nvmf_subsystem_allow_any_host - "nvmf_subsystem_allow_any_host" RPC 핸들러
 *
 * @request: JSON-RPC 요청.
 * @params: JSON. nqn/allow_any_host(필수), tgt_name(optional).
 * @return: void. 성공: bool true.
 *
 * subsystem의 allow_any_host 플래그를 설정. true이면 host ACL을 우회하여
 * 어떤 host NQN이든 connect 가능. false이면 등록된 host NQN만 허용.
 * secure_channel과 allow_any_host는 상호 배타적 (add_listener에서 검증).
 *
 * 동기 핸들러 — 즉시 응답.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_subsystem_allow_any_host]
 *     → spdk_nvmf_subsystem_set_allow_any_host
 */
static void
rpc_nvmf_subsystem_allow_any_host(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx ctx = {};             /* [한국어] 스택 ctx (동기 핸들러) */
	struct spdk_nvmf_subsystem *subsystem;         /* [한국어] 대상 subsystem */
	struct spdk_nvmf_tgt *tgt;                     /* [한국어] 대상 target */
	int rc;                                        /* [한국어] set_allow_any_host 반환 코드 */

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_allow_any_host_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_allow_any_host_decoders),
				    &ctx)) {
		/* [한국어] strict decode 실패 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);         /* [한국어] target 룩업 */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx.nqn); /* [한국어] NQN으로 subsystem 룩업 */
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx.nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_set_allow_any_host(subsystem, ctx.allow_any_host);
	/* [한국어] allow_any_host 플래그 설정. 이후 nvmf_qpair_access_allowed가 이 플래그를 참조. */
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공 응답 */
	nvmf_rpc_host_ctx_free(&ctx);                   /* [한국어] ctx 동적 필드 해제 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_allow_any_host"를 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_subsystem_allow_any_host", rpc_nvmf_subsystem_allow_any_host,
		  SPDK_RPC_RUNTIME)

/* [한국어] nvmf_create_target / delete_target RPC의 파라미터 컨텍스트.
 * 한 SPDK 프로세스에서 여러 target 객체를 생성 가능 — 각 tgt는 subsystem/transport/listener를 독립 관리. */
struct nvmf_rpc_target_ctx {
	char *name;
	/* [한국어] target 이름. 고유 식별자. 이후 nvmf_get_tgt(name)으로 룩업.
	 * 최대 길이: NVMF_TGT_NAME_MAX_LENGTH. 해제: out 라벨 또는 에러 경로에서 free. */

	uint32_t max_subsystems;
	/* [한국어] 이 target에 허용되는 최대 subsystem 수 (0=기본값).
	 * subsystem은 NQN으로 식별되는 NVMe-oF 논리적 단위. */

	uint32_t discovery_filter;
	/* [한국어] Discovery Log Page를 반환할 때 어떤 listener 항목을 포함할지 결정하는 비트마스크.
	 * SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY=0: 모든 항목 포함.
	 * 비트 조합으로 TRANSPORT_TYPE, TRANSPORT_ADDRESS, TRANSPORT_SVCID 필터링 가능.
	 * 설정자: decode_discovery_filter. */
};

/*
 * [한국어]
 * decode_discovery_filter - "discovery_filter" JSON 문자열을 비트마스크로 변환
 *
 * @val: "match_any" 또는 "transport,address,svcid" 쉼표 구분 JSON 문자열.
 * @out: uint32_t* — 변환된 비트마스크 저장.
 * @return: 0=성공, -EINVAL=형식 오류, -ENOMEM=메모리 부족.
 *
 * NVMe-oF Discovery Log Page 필터링 정책을 파싱:
 *   "match_any" → SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY (0, 필터 없음)
 *   "transport"  → SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE 비트 설정
 *   "address"    → SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS 비트 설정
 *   "svcid"      → SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID 비트 설정
 * match_any와 나머지 필터는 상호 배타적 (같이 사용 불가).
 * strtok_r로 쉼표 구분 토큰을 순회하며 비트 OR.
 *
 * 호출 체인:
 *   spdk_json_decode_object (rpc_nvmf_create_target_decoders의 discovery_filter 항목)
 *     → [decode_discovery_filter]
 */
static int
decode_discovery_filter(const struct spdk_json_val *val, void *out)
{
	uint32_t *_filter = out;                                           /* [한국어] 결과를 쓸 uint32_t 포인터 */
	uint32_t filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY;              /* [한국어] 기본값: 필터 없음 */
	char *tokens = spdk_json_strdup(val);                              /* [한국어] JSON 문자열을 C 문자열로 복사 (strtok_r 수정용) */
	char *tok;                                                         /* [한국어] 현재 토큰 포인터 */
	char *sp = NULL;                                                   /* [한국어] strtok_r 상태 포인터 (재진입 안전) */
	int rc = -EINVAL;                                                  /* [한국어] 기본 반환값 — 성공 시 0으로 변경 */
	bool all_specified = false;                                        /* [한국어] match_any가 이미 지정됐는지 표시 */

	if (!tokens) {
		return -ENOMEM;                                              /* [한국어] 메모리 할당 실패 */
	}

	tok = strtok_r(tokens, ",", &sp);                                  /* [한국어] 첫 번째 토큰 추출 */
	while (tok) {
		if (strncmp(tok, "match_any", 9) == 0) {
			/* [한국어] match_any는 단독으로만 사용 가능 — 이전에 다른 필터가 설정됐으면 오류 */
			if (filter != SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY) {
				goto out;                                        /* [한국어] match_any와 다른 필터 혼용 → 오류 */
			}
			filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY;         /* [한국어] 필터 없음으로 재설정 (중복이지만 명시적) */
			all_specified = true;                                /* [한국어] match_any 지정 표시 */
		} else {
			if (all_specified) {
				goto out;                                        /* [한국어] match_any 이후에 다른 필터 → 오류 */
			}
			if (strncmp(tok, "transport", 9) == 0) {
				filter |= SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE;  /* [한국어] trtype 필터 비트 설정 */
			} else if (strncmp(tok, "address", 7) == 0) {
				filter |= SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS; /* [한국어] traddr 필터 비트 설정 */
			} else if (strncmp(tok, "svcid", 5) == 0) {
				filter |= SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID; /* [한국어] trsvcid 필터 비트 설정 */
			} else {
				SPDK_ERRLOG("Invalid value %s\n", tok);
				goto out;                                        /* [한국어] 알 수 없는 필터 이름 → 오류 */
			}
		}

		tok = strtok_r(NULL, ",", &sp);                              /* [한국어] 다음 토큰 추출 */
	}

	rc = 0;                                                            /* [한국어] 모든 토큰 처리 성공 */
	*_filter = filter;                                                 /* [한국어] 최종 비트마스크를 출력 포인터에 저장 */

out:
	free(tokens);                                                      /* [한국어] strtok_r용 복사본 해제 */

	return rc;
}

/* [한국어] nvmf_create_target JSON 파라미터 디코더. */
static const struct spdk_json_object_decoder rpc_nvmf_create_target_decoders[] = {
	{"name", offsetof(struct nvmf_rpc_target_ctx, name), spdk_json_decode_string},                              /* [한국어] 필수: target 이름 */
	{"max_subsystems", offsetof(struct nvmf_rpc_target_ctx, max_subsystems), spdk_json_decode_uint32, true},    /* [한국어] optional: 최대 subsystem 수 */
	{"discovery_filter", offsetof(struct nvmf_rpc_target_ctx, discovery_filter), decode_discovery_filter, true} /* [한국어] optional: discovery log 필터 문자열 */
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
/*
 * [한국어]
 * rpc_nvmf_create_target - "nvmf_create_target" RPC (private 등록)
 *
 * @request: RPC 요청.
 * @params: JSON. name(필수), max_subsystems/discovery_filter(optional).
 * @return: void. 성공 응답: 생성된 target 이름 문자열.
 *
 * 새로운 NVMe-oF target 객체(spdk_nvmf_tgt)를 생성한다.
 * 한 SPDK 프로세스에 여러 target이 공존 가능하며 각각 독립적인
 * subsystem/transport/listener를 갖는다.
 * discovery_filter는 host 요청 시 Discovery Log Page에 어떤 항목을 보여줄지 결정.
 * 동기 핸들러 — 즉시 응답. 성공 시 target 이름을 JSON 문자열로 반환.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_create_target]
 *     → spdk_nvmf_tgt_create
 */
static void
rpc_nvmf_create_target(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct spdk_nvmf_target_opts	opts;                              /* [한국어] target 생성 옵션 */
	struct nvmf_rpc_target_ctx	ctx = {0};                         /* [한국어] JSON 디코드 결과 저장 */
	struct spdk_nvmf_tgt		*tgt;                              /* [한국어] 생성된 target 포인터 */
	struct spdk_json_write_ctx	*w;                                /* [한국어] JSON 응답 직렬화 컨텍스트 */

	/* Decode parameters the first time to get the transport type */
	if (spdk_json_decode_object(params, rpc_nvmf_create_target_decoders,
				    SPDK_COUNTOF(rpc_nvmf_create_target_decoders),
				    &ctx)) {
		/* [한국어] strict decode 실패 — name 없거나 형식 오류 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	snprintf(opts.name, NVMF_TGT_NAME_MAX_LENGTH, "%s", ctx.name); /* [한국어] target 이름을 opts.name 배열에 복사 */
	opts.max_subsystems = ctx.max_subsystems;                       /* [한국어] 최대 subsystem 수 설정 */
	opts.discovery_filter = ctx.discovery_filter;                   /* [한국어] discovery log 필터 비트마스크 설정 */
	opts.size = SPDK_SIZEOF(&opts, discovery_filter);               /* [한국어] ABI 버전 — discovery_filter까지 유효 */

	if (spdk_nvmf_get_tgt(opts.name) != NULL) {
		/* [한국어] 동일 이름의 target이 이미 존재 — 중복 생성 거부 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Target already exists.");
		goto out;
	}

	tgt = spdk_nvmf_tgt_create(&opts);  /* [한국어] target 객체 생성 (내부적으로 subsystem 슬롯 배열 등 할당) */

	if (tgt == NULL) {
		/* [한국어] 생성 실패 (메모리 부족, 동시성 문제 등) */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to create the requested target.");
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);                          /* [한국어] JSON 응답 시작 */
	spdk_json_write_string(w, spdk_nvmf_tgt_get_name(tgt));         /* [한국어] 생성된 target 이름 반환 */
	spdk_jsonrpc_end_result(request, w);                             /* [한국어] 응답 전송 */
out:
	free(ctx.name); /* [한국어] JSON 디코드로 malloc된 target 이름 해제 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_create_target"을 private RUNTIME RPC로 등록. */
/* private */ SPDK_RPC_REGISTER("nvmf_create_target", rpc_nvmf_create_target, SPDK_RPC_RUNTIME);

/* [한국어] nvmf_delete_target JSON 파라미터 디코더. name만 필요. */
static const struct spdk_json_object_decoder rpc_nvmf_delete_target_decoders[] = {
	{"name", offsetof(struct nvmf_rpc_target_ctx, name), spdk_json_decode_string}, /* [한국어] 필수: 삭제할 target 이름 */
};

/*
 * [한국어]
 * nvmf_rpc_destroy_target_done - target destroy 완료 콜백
 *
 * @ctx:    spdk_jsonrpc_request* (request를 ctx로 전달).
 * @status: 0=성공.
 *
 * spdk_nvmf_tgt_destroy 비동기 완료 후 호출. bool true 응답 송신.
 *
 * 호출 체인:
 *   rpc_nvmf_delete_target → spdk_nvmf_tgt_destroy
 *     → [nvmf_rpc_destroy_target_done]
 */
static void
nvmf_rpc_destroy_target_done(void *ctx, int status)
{
	struct spdk_jsonrpc_request	*request = ctx; /* [한국어] ctx로 전달된 request 포인터 복원 */

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공 응답 */
}

/*
 * [한국어]
 * rpc_nvmf_delete_target - "nvmf_delete_target" RPC (private 등록)
 *
 * @request: RPC 요청.
 * @params: JSON. name(필수).
 * @return: void. 성공: bool true (destroy 완료 후).
 *
 * 지정된 이름의 target을 비동기로 파괴. 모든 subsystem/transport/listener가
 * 함께 정리됨. destroy 완료 시 nvmf_rpc_destroy_target_done에서 응답 송신.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_delete_target]
 *     → spdk_nvmf_tgt_destroy → nvmf_rpc_destroy_target_done
 */
static void
rpc_nvmf_delete_target(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct nvmf_rpc_target_ctx	ctx = {0};  /* [한국어] JSON 디코드 결과 */
	struct spdk_nvmf_tgt		*tgt;       /* [한국어] 삭제할 target 포인터 */

	/* Decode parameters the first time to get the transport type */
	if (spdk_json_decode_object(params, rpc_nvmf_delete_target_decoders,
				    SPDK_COUNTOF(rpc_nvmf_delete_target_decoders),
				    &ctx)) {
		/* [한국어] strict decode 실패 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free(ctx.name);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.name);  /* [한국어] 이름으로 target 룩업 */

	if (tgt == NULL) {
		/* [한국어] 지정한 이름의 target이 없음 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "The specified target doesn't exist, cannot delete it.");
		free(ctx.name);
		return;
	}

	spdk_nvmf_tgt_destroy(tgt, nvmf_rpc_destroy_target_done, request);
	/* [한국어] target 비동기 파괴. ctx=request를 콜백에 전달. 완료 시 bool true 응답. */
	free(ctx.name); /* [한국어] ctx.name 해제 (destroy는 비동기지만 ctx.name은 더 이상 불필요) */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_delete_target"을 private RUNTIME RPC로 등록. */
/* private */ SPDK_RPC_REGISTER("nvmf_delete_target", rpc_nvmf_delete_target, SPDK_RPC_RUNTIME);

/*
 * [한국어]
 * rpc_nvmf_get_targets - "nvmf_get_targets" RPC (private 등록)
 *
 * @request: RPC 요청.
 * @params:  반드시 NULL이어야 함 (파라미터 없는 RPC).
 * @return: void. 성공: target 이름 배열 ["name1", "name2", ...].
 *
 * g_nvmf_tgts 리스트를 순회하며 각 target 이름을 JSON 배열로 반환.
 * 동기 핸들러 — 즉시 응답.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_get_targets]
 *     → spdk_nvmf_get_first_tgt / spdk_nvmf_get_next_tgt (리스트 순회)
 */
static void
rpc_nvmf_get_targets(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx	*w;    /* [한국어] JSON 응답 직렬화 컨텍스트 */
	struct spdk_nvmf_tgt		*tgt;  /* [한국어] 순회 중인 target 포인터 */
	const char			*name; /* [한국어] 현재 target 이름 */

	if (params != NULL) {
		/* [한국어] 이 RPC는 파라미터를 받지 않음 — 있으면 오류 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "nvmf_get_targets has no parameters.");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);  /* [한국어] JSON 응답 시작 */
	spdk_json_write_array_begin(w);          /* [한국어] 배열 "[" 시작 */

	tgt = spdk_nvmf_get_first_tgt();        /* [한국어] g_nvmf_tgts 첫 번째 target 가져오기 */

	while (tgt != NULL) {
		name = spdk_nvmf_tgt_get_name(tgt);  /* [한국어] target 이름 조회 */
		spdk_json_write_string(w, name);      /* [한국어] target 이름을 JSON 배열 원소로 추가 */
		tgt = spdk_nvmf_get_next_tgt(tgt);   /* [한국어] 다음 target으로 이동 */
	}

	spdk_json_write_array_end(w);            /* [한국어] 배열 "]" 닫기 */
	spdk_jsonrpc_end_result(request, w);     /* [한국어] 응답 전송 완료 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_get_targets"를 private RUNTIME RPC로 등록. */
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
/*
 * [한국어]
 * nvmf_rpc_decode_max_io_qpairs - "max_io_qpairs_per_ctrlr" JSON 값을 opts.max_qpairs_per_ctrlr로 변환
 *
 * @val: JSON 숫자 (IO qpair 수만).
 * @out: uint16_t* — opts.max_qpairs_per_ctrlr에 해당하는 포인터.
 * @return: 0=성공, 비0=변환 실패.
 *
 * 내부적으로 max_qpairs_per_ctrlr = IO qpairs + 1(admin qpair).
 * 사용자는 IO qpair 수만 지정 → +1 후 저장. deprecated max_qpairs_per_ctrlr 혼동 방지.
 *
 * 호출 체인:
 *   spdk_json_decode_object (rpc_nvmf_create_transport_decoders) → [nvmf_rpc_decode_max_io_qpairs]
 */
static int
nvmf_rpc_decode_max_io_qpairs(const struct spdk_json_val *val, void *out)
{
	uint16_t *i = out;  /* [한국어] opts.max_qpairs_per_ctrlr 포인터 */
	int rc;

	rc = spdk_json_number_to_uint16(val, i); /* [한국어] JSON 숫자 → uint16_t */
	if (rc == 0) {
		(*i)++;  /* [한국어] admin qpair +1: 내부 opts는 admin 포함 총 qpair 수를 기대 */
	}

	return rc;
}

/*
 * [한국어]
 * decode_masked_oncs - ONCS(Optional NVM Command Support) 비트 마스킹 디코더
 *
 * @val: JSON 문자열 — "nvmcmps"/"nvmdsmsv"/"nvmwzsv"/"reservs"/"nvmcpys" 중 하나.
 * @out: spdk_nvme_cdata_oncs* — 해당 비트를 0으로 마스킹.
 * @return: 0=성공, -EINVAL=알 수 없는 ONCS 이름.
 *
 * NVMe Identify Controller의 ONCS 필드 특정 기능 비트를 강제로 0으로 설정(마스킹).
 * 호스트에게 해당 NVMe 선택적 명령을 지원하지 않는다고 보고하도록 강제.
 * 배열로 여러 기능 마스킹 가능 (decode_masked_oncs_array).
 *
 * 호출 체인:
 *   spdk_json_decode_array (decode_masked_oncs_array) → [decode_masked_oncs]
 */
static int
decode_masked_oncs(const struct spdk_json_val *val, void *out)
{
	struct spdk_nvme_cdata_oncs *oncs = out; /* [한국어] ONCS 비트필드 포인터 */
	char *name = NULL;                       /* [한국어] JSON 문자열 임시 저장 */
	int rc;

	rc = spdk_json_decode_string(val, &name); /* [한국어] JSON 문자열 → C 문자열 */
	if (rc) {
		return rc;                           /* [한국어] 문자열 디코드 실패 */
	}

	if (strcmp(name, "nvmcmps") == 0) {
		oncs->nvmcmps = 0;                   /* [한국어] NVM Compare 지원 비트 마스킹 */
	} else if (strcmp(name, "nvmdsmsv") == 0) {
		oncs->nvmdsmsv = 0;                  /* [한국어] Dataset Management 지원 비트 마스킹 */
	} else if (strcmp(name, "nvmwzsv") == 0) {
		oncs->nvmwzsv = 0;                   /* [한국어] Write Zeroes 지원 비트 마스킹 */
	} else if (strcmp(name, "reservs") == 0) {
		oncs->reservs = 0;                   /* [한국어] Reservations 지원 비트 마스킹 */
	} else if (strcmp(name, "nvmcpys") == 0) {
		oncs->nvmcpys = 0;                   /* [한국어] Copy 지원 비트 마스킹 */
	} else {
		rc = -EINVAL;                        /* [한국어] 알 수 없는 ONCS 기능 이름 */
		goto out;
	}

out:
	free(name); /* [한국어] JSON 디코드로 할당된 문자열 해제 */
	return rc;
}

/*
 * [한국어]
 * decode_masked_oncs_array - "masked_oncs" JSON 배열 디코더
 *
 * @val: JSON 배열 — ONCS 기능 이름 문자열의 배열.
 * @out: spdk_nvme_cdata_oncs* — 여러 비트를 순차적으로 마스킹.
 * @return: 0=성공.
 *
 * spdk_json_decode_array로 최대 16개 요소를 decode_masked_oncs로 디코드.
 * 예: ["nvmwzsv", "reservs"] → Write Zeroes, Reservations 비트 모두 마스킹.
 *
 * 호출 체인:
 *   spdk_json_decode_object (rpc_nvmf_create_transport_decoders)
 *     → [decode_masked_oncs_array] → decode_masked_oncs (반복)
 */
static int
decode_masked_oncs_array(const struct spdk_json_val *val, void *out)
{
	size_t count; /* [한국어] 디코드된 배열 원소 수 (사용 안 함) */

	return spdk_json_decode_array(val, decode_masked_oncs, out, 16, &count, 0);
	/* [한국어] 최대 16개 ONCS 항목 디코드. element_size=0 (oncs 비트필드에 직접 write) */
}

/*
 * [한국어]
 * decode_masked_fuses - FUSES(Fused Operations Support) 비트 마스킹 디코더
 *
 * @val: JSON 문자열 — "fcws" (Fused Compare and Write Support) 만 유효.
 * @out: spdk_nvme_cdata_fuses* — 해당 비트를 0으로 마스킹.
 * @return: 0=성공, -EINVAL=알 수 없는 이름.
 *
 * NVMe Identify Controller의 FUSES 필드 특정 기능 비트를 강제로 0으로 마스킹.
 * 현재는 "fcws" (Fused Compare and Write) 만 지원.
 *
 * 호출 체인:
 *   spdk_json_decode_array (decode_masked_fuses_array) → [decode_masked_fuses]
 */
static int
decode_masked_fuses(const struct spdk_json_val *val, void *out)
{
	struct spdk_nvme_cdata_fuses *fuses = out; /* [한국어] FUSES 비트필드 포인터 */
	char *name = NULL;                         /* [한국어] JSON 문자열 임시 저장 */
	int rc;

	rc = spdk_json_decode_string(val, &name);  /* [한국어] JSON 문자열 → C 문자열 */
	if (rc) {
		return rc;
	}

	if (strcmp(name, "fcws") == 0) {
		fuses->fcws = 0;                       /* [한국어] Fused Compare and Write 지원 비트 마스킹 */
	} else {
		rc = -EINVAL;                          /* [한국어] 알 수 없는 FUSES 이름 */
		goto out;
	}

out:
	free(name); /* [한국어] 임시 문자열 해제 */
	return rc;
}

/*
 * [한국어]
 * decode_masked_fuses_array - "masked_fuses" JSON 배열 디코더
 *
 * @val: JSON 배열 — FUSES 기능 이름 문자열의 배열.
 * @out: spdk_nvme_cdata_fuses*.
 * @return: 0=성공.
 *
 * 호출 체인:
 *   spdk_json_decode_object (rpc_nvmf_create_transport_decoders)
 *     → [decode_masked_fuses_array] → decode_masked_fuses (반복)
 */
static int
decode_masked_fuses_array(const struct spdk_json_val *val, void *out)
{
	size_t count; /* [한국어] 디코드된 배열 원소 수 */

	return spdk_json_decode_array(val, decode_masked_fuses, out, 16, &count, 0);
	/* [한국어] 최대 16개 FUSES 항목 디코드 */
}

/* [한국어] nvmf_create_transport JSON 파라미터 디코더 배열.
 * trtype(필수), 그 외 transport opts 21개 필드 (모두 optional). */
static const struct spdk_json_object_decoder rpc_nvmf_create_transport_decoders[] = {
	{"trtype", offsetof(struct nvmf_rpc_create_transport_ctx, trtype), spdk_json_decode_string},                                               /* [한국어] 필수: "TCP"/"RDMA"/"FC" 등 transport 종류 */
	{"max_queue_depth", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_queue_depth), spdk_json_decode_uint16, true},                   /* [한국어] optional: SQ(Submission Queue) 최대 깊이 */
	{"max_io_qpairs_per_ctrlr", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_qpairs_per_ctrlr), nvmf_rpc_decode_max_io_qpairs, true}, /* [한국어] optional: ctrlr당 IO qpair 수 (admin qpair +1 자동) */
	{"in_capsule_data_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.in_capsule_data_size), spdk_json_decode_uint32, true},         /* [한국어] optional: Capsule 내 inline data 최대 크기 (바이트) */
	{"max_io_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_io_size), spdk_json_decode_uint32, true},                          /* [한국어] optional: 단일 IO 최대 크기 (바이트) */
	{"io_unit_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.io_unit_size), spdk_json_decode_uint32, true},                        /* [한국어] optional: DMA I/O 단위 크기 */
	{"max_aq_depth", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_aq_depth), spdk_json_decode_uint32, true},                        /* [한국어] optional: Admin Queue 최대 깊이 */
	{"num_shared_buffers", offsetof(struct nvmf_rpc_create_transport_ctx, opts.num_shared_buffers), spdk_json_decode_uint32, true},            /* [한국어] optional: transport 공유 buffer pool 크기 */
	{"buf_cache_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.buf_cache_size), spdk_json_decode_uint32, true},                    /* [한국어] optional: per-poll-group buffer cache 크기 */
	{"dif_insert_or_strip", offsetof(struct nvmf_rpc_create_transport_ctx, opts.dif_insert_or_strip), spdk_json_decode_bool, true},            /* [한국어] optional: DIF(Data Integrity Field) insert/strip 활성화 */
	{"abort_timeout_sec", offsetof(struct nvmf_rpc_create_transport_ctx, opts.abort_timeout_sec), spdk_json_decode_uint32, true},              /* [한국어] optional: abort 명령 timeout (초) */
	{"zcopy", offsetof(struct nvmf_rpc_create_transport_ctx, opts.zcopy), spdk_json_decode_bool, true},                                        /* [한국어] optional: zero-copy I/O 활성화 (지원 transport만) */
	{"tgt_name", offsetof(struct nvmf_rpc_create_transport_ctx, tgt_name), spdk_json_decode_string, true},                                     /* [한국어] optional: target 이름 (NULL=default) */
	{"acceptor_poll_rate", offsetof(struct nvmf_rpc_create_transport_ctx, opts.acceptor_poll_rate), spdk_json_decode_uint32, true},            /* [한국어] optional: 새 연결 수락 polling 주기 (마이크로초) */
	{"ack_timeout", offsetof(struct nvmf_rpc_create_transport_ctx, opts.ack_timeout), spdk_json_decode_uint32, true},                          /* [한국어] optional: ACK timeout (RDMA 전용, 마이크로초) */
	{"data_wr_pool_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.data_wr_pool_size), spdk_json_decode_uint32, true},              /* [한국어] optional: RDMA data WR pool 크기 */
	{"disable_command_passthru", offsetof(struct nvmf_rpc_create_transport_ctx, opts.disable_command_passthru), spdk_json_decode_bool, true},  /* [한국어] optional: passthrough 명령 비활성화 */
	{"kas", offsetof(struct nvmf_rpc_create_transport_ctx, opts.kas), spdk_json_decode_uint16, true},                                          /* [한국어] optional: Keep Alive Support 값 (100ms 단위) */
	{"min_kato", offsetof(struct nvmf_rpc_create_transport_ctx, opts.min_kato), spdk_json_decode_uint32, true},                                /* [한국어] optional: 최소 Keep Alive Timeout (ms) */
	{"masked_oncs", offsetof(struct nvmf_rpc_create_transport_ctx, opts.oncs), decode_masked_oncs_array, true},                               /* [한국어] optional: 마스킹할 ONCS 기능 이름 배열 */
	{"masked_fuses", offsetof(struct nvmf_rpc_create_transport_ctx, opts.fuses), decode_masked_fuses_array, true},                            /* [한국어] optional: 마스킹할 FUSES 기능 이름 배열 */
};

/*
 * [한국어]
 * nvmf_rpc_create_transport_ctx_free - nvmf_rpc_create_transport_ctx 해제
 *
 * @ctx: calloc으로 할당된 ctx.
 *
 * 호출 체인:
 *   nvmf_rpc_transport_destroy_done_cb / nvmf_rpc_tgt_add_transport_done / 에러 경로
 *     → [nvmf_rpc_create_transport_ctx_free]
 */
static void
nvmf_rpc_create_transport_ctx_free(struct nvmf_rpc_create_transport_ctx *ctx)
{
	free(ctx->trtype);   /* [한국어] JSON 디코드된 trtype 해제 */
	free(ctx->tgt_name); /* [한국어] JSON 디코드된 target 이름 해제 */
	free(ctx);           /* [한국어] ctx 구조체 자체 해제 */
}

/*
 * [한국어]
 * nvmf_rpc_transport_destroy_done_cb - transport destroy 완료 후 에러 응답 콜백
 *
 * @cb_arg: nvmf_rpc_create_transport_ctx*.
 *
 * tgt_add_transport 실패 후 생성된 transport를 파괴하는 rollback 완료 시 호출.
 * ctx->status에 저장된 에러 코드를 응답에 포함하여 에러 응답 송신.
 *
 * 호출 체인:
 *   nvmf_rpc_tgt_add_transport_done (실패 경로)
 *     → spdk_nvmf_transport_destroy → [nvmf_rpc_transport_destroy_done_cb]
 */
static void
nvmf_rpc_transport_destroy_done_cb(void *cb_arg)
{
	struct nvmf_rpc_create_transport_ctx *ctx = cb_arg; /* [한국어] ctx 복원 */

	spdk_jsonrpc_send_error_response_fmt(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					     "Failed to add transport to tgt.(%d)", ctx->status);
	/* [한국어] tgt_add 실패 코드를 포함한 에러 응답 */
	nvmf_rpc_create_transport_ctx_free(ctx); /* [한국어] ctx 최종 해제 */
}

/*
 * [한국어]
 * nvmf_rpc_tgt_add_transport_done - spdk_nvmf_tgt_add_transport 완료 콜백
 *
 * @cb_arg: nvmf_rpc_create_transport_ctx*.
 * @status: 0=성공, 비0=실패.
 *
 * transport를 target에 등록하는 작업 완료 시 호출.
 * 실패 시: transport rollback (destroy) → nvmf_rpc_transport_destroy_done_cb → 에러 응답.
 * 성공 시: bool true 응답 후 ctx 해제.
 *
 * 호출 체인:
 *   nvmf_rpc_create_transport_done → spdk_nvmf_tgt_add_transport
 *     → [nvmf_rpc_tgt_add_transport_done]
 *       (실패) → spdk_nvmf_transport_destroy → nvmf_rpc_transport_destroy_done_cb
 */
static void
nvmf_rpc_tgt_add_transport_done(void *cb_arg, int status)
{
	struct nvmf_rpc_create_transport_ctx *ctx = cb_arg; /* [한국어] ctx 복원 */

	if (status) {
		/* [한국어] tgt에 transport 추가 실패 — 생성된 transport를 파괴(rollback) */
		SPDK_ERRLOG("Failed to add transport to tgt.(%d)\n", status);
		ctx->status = status;                                            /* [한국어] 에러 코드 저장 (destroy 콜백에서 응답에 사용) */
		spdk_nvmf_transport_destroy(ctx->transport, nvmf_rpc_transport_destroy_done_cb, ctx);
		/* [한국어] transport 비동기 파괴 → 완료 시 에러 응답 송신 */
		return;
	}

	spdk_jsonrpc_send_bool_response(ctx->request, true); /* [한국어] 성공 응답 */
	nvmf_rpc_create_transport_ctx_free(ctx);              /* [한국어] ctx 해제 */
}

/*
 * [한국어]
 * nvmf_rpc_create_transport_done - spdk_nvmf_transport_create_async 완료 콜백
 *
 * @cb_arg:    nvmf_rpc_create_transport_ctx*.
 * @transport: 생성된 transport 포인터 (NULL이면 생성 실패).
 *
 * transport 객체 생성 완료 후 호출. 성공 시 target에 등록 (tgt_add_transport).
 * 실패 시 에러 응답 후 ctx 해제.
 *
 * 호출 체인:
 *   spdk_nvmf_transport_create_async → [nvmf_rpc_create_transport_done]
 *     → spdk_nvmf_tgt_add_transport → nvmf_rpc_tgt_add_transport_done
 */
static void
nvmf_rpc_create_transport_done(void *cb_arg, struct spdk_nvmf_transport *transport)
{
	struct nvmf_rpc_create_transport_ctx *ctx = cb_arg; /* [한국어] ctx 복원 */

	if (!transport) {
		/* [한국어] transport 생성 실패 (trtype 모듈 없음, 메모리 부족 등) */
		SPDK_ERRLOG("Failed to create transport.\n");
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to create transport.");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	ctx->transport = transport;  /* [한국어] 생성된 transport 저장 (tgt_add 실패 시 rollback용) */

	spdk_nvmf_tgt_add_transport(spdk_nvmf_get_tgt(ctx->tgt_name), transport,
				    nvmf_rpc_tgt_add_transport_done, ctx);
	/* [한국어] transport를 target에 등록. 완료 콜백: nvmf_rpc_tgt_add_transport_done */
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
/*
 * rpc_nvmf_create_transport - "nvmf_create_transport" RPC 핸들러
 * (위의 §2 블록 주석 참조)
 */
static void
rpc_nvmf_create_transport(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct nvmf_rpc_create_transport_ctx *ctx;  /* [한국어] 비동기 체인 컨텍스트 */
	struct spdk_nvmf_tgt *tgt;                  /* [한국어] 대상 target */
	int rc;                                     /* [한국어] transport_create_async 반환 코드 */

	ctx = calloc(1, sizeof(*ctx));              /* [한국어] ctx heap 할당 */
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	/* Decode parameters the first time to get the transport type */
	/* [한국어] 1차 디코드: trtype을 알아내기 위해 relaxed decode (다른 옵션은 미완성 상태). */
	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_create_transport_decoders,
					    SPDK_COUNTOF(rpc_nvmf_create_transport_decoders),
					    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);     /* [한국어] target 룩업 */
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
	/* [한국어] trtype별 기본 opts 초기화. 이후 2차 decode에서 사용자 값으로 덮어씀. */
	if (!spdk_nvmf_transport_opts_init(ctx->trtype, &ctx->opts, sizeof(ctx->opts))) {
		/* This can happen if user specifies PCIE transport type which isn't valid for
		 * NVMe-oF.
		 */
		/* [한국어] PCIE나 잘못된 trtype — NVMe-oF에서는 TCP/RDMA/FC만 유효 */
		SPDK_ERRLOG("Invalid transport type '%s'\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid transport type '%s'", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	/* [한국어] 2차 디코드: trtype별 기본값이 초기화된 opts에 사용자 옵션 값을 덮어씀. */
	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_create_transport_decoders,
					    SPDK_COUNTOF(rpc_nvmf_create_transport_decoders),
					    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	if (spdk_nvmf_tgt_get_transport(tgt, ctx->trtype)) {
		/* [한국어] 이미 같은 trtype의 transport가 target에 등록됨 — 중복 거부 */
		SPDK_ERRLOG("Transport type '%s' already exists\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Transport type '%s' already exists", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	/* Transport can parse additional params themselves */
	ctx->opts.transport_specific = params;   /* [한국어] transport-specific 옵션을 원본 params로 설정 — transport가 자체 디코드 */
	ctx->request = request;                  /* [한국어] 응답 송신용 request 저장 */

	rc = spdk_nvmf_transport_create_async(ctx->trtype, &ctx->opts, nvmf_rpc_create_transport_done, ctx);
	/* [한국어] transport 비동기 생성. 완료 콜백: nvmf_rpc_create_transport_done */
	if (rc) {
		/* [한국어] transport 생성 요청 실패 (trtype 등록 없음 등) */
		SPDK_ERRLOG("Transport type '%s' create failed\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Transport type '%s' create failed", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
	}
	/* [한국어] rc=0이면 비동기 처리 중 — nvmf_rpc_create_transport_done 콜백 대기 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_create_transport"를 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_create_transport", rpc_nvmf_create_transport, SPDK_RPC_RUNTIME)

/* [한국어] nvmf_get_transports RPC 파라미터 ctx.
 * trtype 또는 tgt_name으로 필터링 가능. 모두 NULL이면 default target의 모든 transport 조회. */
struct rpc_get_transport {
	char *trtype;
	/* [한국어] 특정 trtype 필터 (NULL이면 모든 transport). 해제: 함수 끝 free. */

	char *tgt_name;
	/* [한국어] target 이름 (NULL=default). 해제: 함수 끝 free. */
};

/* [한국어] nvmf_get_transports JSON 파라미터 디코더. */
static const struct spdk_json_object_decoder rpc_nvmf_get_transports_decoders[] = {
	{"trtype", offsetof(struct rpc_get_transport, trtype), spdk_json_decode_string, true},    /* [한국어] optional: 조회할 transport 종류 필터 */
	{"tgt_name", offsetof(struct rpc_get_transport, tgt_name), spdk_json_decode_string, true}, /* [한국어] optional: target 이름 */
};

/*
 * [한국어]
 * rpc_nvmf_get_transports - "nvmf_get_transports" RPC 핸들러
 *
 * @request: RPC 요청.
 * @params:  JSON. trtype/tgt_name(optional). params 자체가 NULL이면 모든 transport 조회.
 * @return: void. 성공: transport opts 배열 [{trtype, max_queue_depth, ...}, ...].
 *
 * 지정된 target의 transport 목록을 JSON 배열로 반환. trtype 필터가 있으면 해당 transport만,
 * 없으면 모든 transport를 nvmf_transport_dump_opts로 직렬화.
 * 동기 핸들러.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_get_transports]
 *     → nvmf_transport_dump_opts (각 transport opts 직렬화)
 */
static void
rpc_nvmf_get_transports(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_get_transport req = { 0 };        /* [한국어] 요청 파라미터 저장 */
	struct spdk_json_write_ctx *w;               /* [한국어] JSON 응답 직렬화 컨텍스트 */
	struct spdk_nvmf_transport *transport = NULL; /* [한국어] 특정 trtype 필터 시 해당 transport */
	struct spdk_nvmf_tgt *tgt;                   /* [한국어] 대상 target */

	if (params) {
		/* [한국어] params가 있을 때만 디코드 */
		if (spdk_json_decode_object(params, rpc_nvmf_get_transports_decoders,
					    SPDK_COUNTOF(rpc_nvmf_get_transports_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);       /* [한국어] target 룩업 */
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free(req.trtype);
		free(req.tgt_name);
		return;
	}

	if (req.trtype) {
		/* [한국어] trtype 필터가 지정된 경우 해당 transport만 룩업 */
		transport = spdk_nvmf_tgt_get_transport(tgt, req.trtype);
		if (transport == NULL) {
			/* [한국어] 지정한 trtype의 transport 없음 */
			SPDK_ERRLOG("transport '%s' does not exist\n", req.trtype);
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
			free(req.trtype);
			free(req.tgt_name);
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);       /* [한국어] JSON 응답 시작 */
	spdk_json_write_array_begin(w);               /* [한국어] 배열 "[" 시작 */

	if (transport) {
		nvmf_transport_dump_opts(transport, w, false); /* [한국어] 단일 transport opts 직렬화 */
	} else {
		/* [한국어] 모든 transport 순회하며 직렬화 */
		for (transport = spdk_nvmf_transport_get_first(tgt); transport != NULL;
		     transport = spdk_nvmf_transport_get_next(transport)) {
			nvmf_transport_dump_opts(transport, w, false); /* [한국어] 각 transport opts 직렬화 */
		}
	}

	spdk_json_write_array_end(w);                 /* [한국어] 배열 "]" 닫기 */
	spdk_jsonrpc_end_result(request, w);          /* [한국어] 응답 전송 완료 */
	free(req.trtype);                             /* [한국어] 디코드된 trtype 해제 */
	free(req.tgt_name);                           /* [한국어] 디코드된 tgt_name 해제 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_get_transports"를 RUNTIME RPC로 등록. */
SPDK_RPC_REGISTER("nvmf_get_transports", rpc_nvmf_get_transports, SPDK_RPC_RUNTIME)

/* [한국어] nvmf_get_stats RPC의 비동기 컨텍스트.
 * spdk_for_each_channel로 각 reactor thread의 poll group을 순회하며 통계를 수집한다. */
struct rpc_nvmf_get_stats_ctx {
	char *tgt_name;
	/* [한국어] target 이름 (NULL=default). 해제: free_get_stats_ctx. */

	struct spdk_nvmf_tgt *tgt;
	/* [한국어] 조회할 target 포인터. spdk_nvmf_get_tgt로 룩업. */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답 송신용 RPC 요청 객체. */

	struct spdk_json_write_ctx *w;
	/* [한국어] JSON 응답 직렬화 컨텍스트. for_each_channel 순회 중 공유됨. */
};

/* [한국어] nvmf_get_stats JSON 파라미터 디코더. */
static const struct spdk_json_object_decoder rpc_nvmf_get_stats_decoders[] = {
	{"tgt_name", offsetof(struct rpc_nvmf_get_stats_ctx, tgt_name), spdk_json_decode_string, true}, /* [한국어] optional: target 이름 */
};

/*
 * [한국어]
 * free_get_stats_ctx - rpc_nvmf_get_stats_ctx 해제
 */
static void
free_get_stats_ctx(struct rpc_nvmf_get_stats_ctx *ctx)
{
	free(ctx->tgt_name); /* [한국어] 동적 할당된 target 이름 해제 */
	free(ctx);           /* [한국어] ctx 구조체 자체 해제 */
}

/*
 * [한국어]
 * rpc_nvmf_get_stats_done - spdk_for_each_channel 완료 콜백 (통계 수집 끝)
 *
 * @i:      io_channel_iter (ctx 포함).
 * @status: 0=성공.
 *
 * 모든 reactor의 poll group 통계 수집 완료 후 호출.
 * JSON 배열과 객체를 닫고 응답 전송 후 ctx 해제.
 *
 * 호출 체인:
 *   spdk_for_each_channel → (각 reactor에서 _rpc_nvmf_get_stats)
 *     → 모두 완료 → [rpc_nvmf_get_stats_done]
 */
static void
rpc_nvmf_get_stats_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_nvmf_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] iter에서 ctx 복원 */

	spdk_json_write_array_end(ctx->w);     /* [한국어] "poll_groups" 배열 "]" 닫기 */
	spdk_json_write_object_end(ctx->w);    /* [한국어] 최상위 객체 "}" 닫기 */
	spdk_jsonrpc_end_result(ctx->request, ctx->w); /* [한국어] 응답 전송 완료 */
	free_get_stats_ctx(ctx);               /* [한국어] ctx 해제 */
}

/*
 * [한국어]
 * _rpc_nvmf_get_stats - 각 reactor에서 poll group 통계 수집 (spdk_for_each_channel worker)
 *
 * @i: io_channel_iter (ctx 포함, 현재 채널 포함).
 *
 * spdk_for_each_channel이 각 reactor thread에서 호출하는 워커 함수.
 * 현재 reactor의 tgt IO channel → poll group 컨텍스트 획득 →
 * spdk_nvmf_poll_group_dump_stat으로 통계 직렬화 → 채널 반납 → for_each_channel_continue.
 *
 * 실행 컨텍스트: 각 reactor thread 개별 실행 (멀티스레드 — 동시 실행 아님, 순차 진행).
 *
 * 호출 체인:
 *   spdk_for_each_channel → [_rpc_nvmf_get_stats] (각 reactor별 1회)
 *     → spdk_for_each_channel_continue → ... → rpc_nvmf_get_stats_done
 */
static void
_rpc_nvmf_get_stats(struct spdk_io_channel_iter *i)
{
	struct rpc_nvmf_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] iter에서 ctx 복원 */
	struct spdk_io_channel *ch;             /* [한국어] 현재 reactor의 tgt IO 채널 */
	struct spdk_nvmf_poll_group *group;     /* [한국어] 채널에 바인딩된 poll group 컨텍스트 */

	ch = spdk_get_io_channel(ctx->tgt);     /* [한국어] 현재 reactor thread에서 tgt의 IO 채널 획득 */
	group = spdk_io_channel_get_ctx(ch);    /* [한국어] IO 채널의 사용자 컨텍스트 = poll group */

	spdk_nvmf_poll_group_dump_stat(group, ctx->w);
	/* [한국어] poll group 통계 (qpair 수, IO 카운터 등)를 JSON에 직렬화 */

	spdk_put_io_channel(ch);               /* [한국어] 채널 참조 해제 */
	spdk_for_each_channel_continue(i, 0); /* [한국어] 다음 reactor로 넘어감 (0=성공) */
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
	struct rpc_nvmf_get_stats_ctx *ctx;             /* [한국어] 비동기 체인 컨텍스트 */

	ctx = calloc(1, sizeof(*ctx));                  /* [한국어] ctx heap 할당 (for_each_channel 체인 동안 유지) */
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error");
		return;
	}
	ctx->request = request;                         /* [한국어] 응답 대상 저장 */

	if (params) {
		/* [한국어] params 제공 시에만 디코드 (tgt_name은 optional) */
		if (spdk_json_decode_object(params, rpc_nvmf_get_stats_decoders,
					    SPDK_COUNTOF(rpc_nvmf_get_stats_decoders),
					    ctx)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			free_get_stats_ctx(ctx);
			return;
		}
	}

	ctx->tgt = spdk_nvmf_get_tgt(ctx->tgt_name);   /* [한국어] tgt 룩업 (NULL=default tgt) */
	if (!ctx->tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free_get_stats_ctx(ctx);
		return;
	}

	ctx->w = spdk_jsonrpc_begin_result(ctx->request); /* [한국어] JSON 응답 시작 */
	spdk_json_write_object_begin(ctx->w);              /* [한국어] 최상위 "{" */
	spdk_json_write_named_uint64(ctx->w, "tick_rate", spdk_get_ticks_hz());
	/* [한국어] "tick_rate": TSC(Time Stamp Counter) 주파수 (Hz).
	 *          클라이언트가 poll group 통계의 tick 값을 시간(초)으로 변환하는 데 사용. */
	spdk_json_write_named_array_begin(ctx->w, "poll_groups");
	/* [한국어] "poll_groups": [ — _rpc_nvmf_get_stats에서 각 reactor별 항목이 채워짐 */

	spdk_for_each_channel(ctx->tgt,
			      _rpc_nvmf_get_stats,
			      ctx,
			      rpc_nvmf_get_stats_done);
	/* [한국어] tgt에 등록된 모든 IO channel(reactor별 1개)에 대해 _rpc_nvmf_get_stats 실행.
	 *          모든 reactor 완료 시 rpc_nvmf_get_stats_done 호출. */
}

/* [한국어] SPDK_RPC_REGISTER: "nvmf_get_stats" RUNTIME RPC 등록. */
SPDK_RPC_REGISTER("nvmf_get_stats", rpc_nvmf_get_stats, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * nvmf_cntrltype_str - spdk_nvme_ctrlr_type enum → 문자열 변환
 *
 * @type: SPDK_NVME_CTRLR_IO / DISCOVERY / ADMINISTRATIVE enum 값.
 * @return: "io" / "discovery" / "administrative" / "unknown".
 *
 * dump_nvmf_ctrlr에서 "cntrltype" JSON 필드 값으로 사용.
 *
 * 호출 체인:
 *   dump_nvmf_ctrlr → [nvmf_cntrltype_str]
 */
static const char *
nvmf_cntrltype_str(enum spdk_nvme_ctrlr_type type)
{
	switch (type) {
	case SPDK_NVME_CTRLR_IO:
		return "io";             /* [한국어] NVM Subsystem 정상 I/O controller */
	case SPDK_NVME_CTRLR_DISCOVERY:
		return "discovery";      /* [한국어] NVMe-oF Discovery Controller */
	case SPDK_NVME_CTRLR_ADMINISTRATIVE:
		return "administrative"; /* [한국어] 관리 전용 controller */
	default:
		return "unknown";
	}
}

/*
 * [한국어]
 * dump_nvmf_ctrlr - NVMe-oF controller 정보를 JSON 객체로 직렬화
 *
 * @w:     JSON write context.
 * @ctrlr: 직렬화할 spdk_nvmf_ctrlr 포인터.
 *
 * {"cntlid": N, "cntrltype": "io"|"discovery"|"administrative",
 *  "hostnqn": "...", "hostid": "uuid", "num_io_qpairs": N}
 * rpc_nvmf_get_controllers_paused에서 subsystem->ctrlrs TAILQ를 순회하며 호출.
 *
 * 호출 체인:
 *   rpc_nvmf_get_controllers_paused → TAILQ_FOREACH(ctrlrs) → [dump_nvmf_ctrlr]
 */
static void
dump_nvmf_ctrlr(struct spdk_json_write_ctx *w, struct spdk_nvmf_ctrlr *ctrlr)
{
	uint32_t count;                                      /* [한국어] 활성 IO qpair 수 */

	spdk_json_write_object_begin(w);                     /* [한국어] ctrlr 객체 "{" 시작 */

	spdk_json_write_named_uint32(w, "cntlid", ctrlr->cntlid);
	/* [한국어] "cntlid": NVMe Controller ID (1-65519). Connect 시 target이 할당. */
	spdk_json_write_named_string(w, "cntrltype", nvmf_cntrltype_str(ctrlr->cdata.cntrltype));
	/* [한국어] "cntrltype": Identify Controller response의 CNTRLTYPE 필드 (NVMe 1.4+) */
	spdk_json_write_named_string(w, "hostnqn", ctrlr->hostnqn);
	/* [한국어] "hostnqn": host의 NQN (Connect command HOSTNQN 필드) */
	spdk_json_write_named_uuid(w, "hostid", &ctrlr->hostid);
	/* [한국어] "hostid": host의 UUID (Connect command HOSTID 필드) */

	count = spdk_bit_array_count_set(ctrlr->qpair_mask);
	/* [한국어] qpair_mask: ctrlr에 등록된 qpair 비트맵. 비트 수 = 현재 IO qpair 수 */
	spdk_json_write_named_uint32(w, "num_io_qpairs", count);
	/* [한국어] "num_io_qpairs": 현재 ctrlr에 연결된 IO qpair 수 (admin qpair 제외) */

	spdk_json_write_object_end(w);                       /* [한국어] ctrlr 객체 "}" 닫기 */
}

/*
 * [한국어]
 * nvmf_qpair_state_str - spdk_nvmf_qpair_state enum → 문자열 변환
 *
 * @state: qpair 상태 enum.
 * @return: 상태 이름 문자열, 또는 NULL(unknown).
 *
 * dump_nvmf_qpair에서 "state" JSON 필드 값으로 사용.
 */
static const char *
nvmf_qpair_state_str(enum spdk_nvmf_qpair_state state)
{
	switch (state) {
	case SPDK_NVMF_QPAIR_UNINITIALIZED:
		return "uninitialized";  /* [한국어] 초기화 전 상태 (transport가 아직 accept 전) */
	case SPDK_NVMF_QPAIR_CONNECTING:
		return "connecting";     /* [한국어] Connect 명령 처리 중 */
	case SPDK_NVMF_QPAIR_AUTHENTICATING:
		return "authenticating"; /* [한국어] DH-HMAC-CHAP 인증 진행 중 */
	case SPDK_NVMF_QPAIR_ENABLED:
		return "enabled";        /* [한국어] 정상 동작 중 (I/O 처리 가능) */
	case SPDK_NVMF_QPAIR_DEACTIVATING:
		return "deactivating";   /* [한국어] 연결 해제 진행 중 */
	case SPDK_NVMF_QPAIR_ERROR:
		return "error";          /* [한국어] 에러 발생으로 비정상 종료 예정 */
	default:
		return NULL;
	}
}

/*
 * [한국어]
 * dump_nvmf_qpair - NVMe-oF qpair 정보를 JSON 객체로 직렬화
 *
 * @w:     JSON write context.
 * @qpair: 직렬화할 spdk_nvmf_qpair 포인터.
 *
 * {"cntlid", "qid", "state", "thread", "hostnqn",
 *  "listen_address": {...}, "peer_address": {...}, + auth 정보}
 * listen_trid(target측 주소), peer_trid(host측 주소)가 있으면 각각 출력.
 * nvmf_qpair_auth_dump으로 DH-HMAC-CHAP 인증 상태 추가.
 *
 * 호출 체인:
 *   rpc_nvmf_get_qpairs → [dump_nvmf_qpair] (각 매칭 qpair에 대해)
 */
static void
dump_nvmf_qpair(struct spdk_json_write_ctx *w, struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvme_transport_id trid = {};     /* [한국어] listen/peer TRID를 임시 저장할 버퍼 */

	spdk_json_write_object_begin(w);             /* [한국어] qpair 객체 "{" 시작 */

	spdk_json_write_named_uint32(w, "cntlid", qpair->ctrlr->cntlid);
	/* [한국어] "cntlid": 이 qpair가 속한 ctrlr의 Controller ID */
	spdk_json_write_named_uint32(w, "qid", qpair->qid);
	/* [한국어] "qid": Queue Pair ID (0=admin qpair, 1 이상=IO qpair) */
	spdk_json_write_named_string(w, "state", nvmf_qpair_state_str(qpair->state));
	/* [한국어] "state": 현재 qpair 상태 문자열 */
	spdk_json_write_named_string(w, "thread", spdk_thread_get_name(spdk_get_thread()));
	/* [한국어] "thread": 현재 이 qpair를 실행 중인 SPDK reactor thread 이름 */
	spdk_json_write_named_string(w, "hostnqn", qpair->ctrlr->hostnqn);
	/* [한국어] "hostnqn": host NQN (ctrlr에 저장된 값) */

	if (spdk_nvmf_qpair_get_listen_trid(qpair, &trid) == 0) {
		/* [한국어] 이 qpair가 accept된 target 측 listen TRID (= 자신의 IP:port 등) */
		spdk_json_write_named_object_begin(w, "listen_address"); /* [한국어] "listen_address": { */
		nvmf_transport_listen_dump_trid(&trid, w);               /* [한국어] trtype/adrfam/traddr/trsvcid 출력 */
		spdk_json_write_object_end(w);                           /* [한국어] "listen_address": } */
		if (qpair->transport->ops->listen_dump_opts) {
			qpair->transport->ops->listen_dump_opts(qpair->transport, &trid, w);
			/* [한국어] transport별 추가 옵션 출력 (예: TCP의 TLS 여부, sock_impl 등) */
		}
	}

	memset(&trid, 0, sizeof(trid));              /* [한국어] trid 버퍼 재초기화 (peer_trid 조회용) */
	if (spdk_nvmf_qpair_get_peer_trid(qpair, &trid) == 0) {
		/* [한국어] 반대편 host 측 TRID (= host IP:port) */
		spdk_json_write_named_object_begin(w, "peer_address");   /* [한국어] "peer_address": { */
		nvmf_transport_listen_dump_trid(&trid, w);               /* [한국어] host trtype/traddr/trsvcid */
		spdk_json_write_object_end(w);                           /* [한국어] "peer_address": } */
	}

	nvmf_qpair_auth_dump(qpair, w);             /* [한국어] DH-HMAC-CHAP 인증 상태 정보 추가 */
	spdk_json_write_object_end(w);               /* [한국어] qpair 객체 "}" 닫기 */
}

/*
 * [한국어]
 * nvme_ana_state_str - spdk_nvme_ana_state enum → 문자열 변환
 *
 * @ana_state: ANA 상태 enum (NVMe 1.4 ANA(Asymmetric Namespace Access) 스펙).
 * @return: "optimized" / "non_optimized" / "inaccessible" /
 *          "persistent_loss" / "change" / NULL.
 *
 * dump_nvmf_subsystem_listener에서 "ana_state" JSON 필드 값으로 사용.
 */
static const char *
nvme_ana_state_str(enum spdk_nvme_ana_state ana_state)
{
	switch (ana_state) {
	case SPDK_NVME_ANA_OPTIMIZED_STATE:
		return "optimized";       /* [한국어] 최적 경로 — 성능 최대 */
	case SPDK_NVME_ANA_NON_OPTIMIZED_STATE:
		return "non_optimized";   /* [한국어] 비최적 경로 — I/O 가능하지만 성능 저하 */
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
		return "inaccessible";    /* [한국어] 현재 이 경로로 접근 불가 (일시적) */
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
		return "persistent_loss"; /* [한국어] 영구 손실 — 복구 불가 */
	case SPDK_NVME_ANA_CHANGE_STATE:
		return "change";          /* [한국어] ANA 상태 전환 중 */
	default:
		return NULL;
	}
}

/*
 * [한국어]
 * dump_nvmf_subsystem_listener - subsystem listener 정보를 JSON 객체로 직렬화
 *
 * @w:        JSON write context.
 * @listener: 직렬화할 spdk_nvmf_subsystem_listener 포인터.
 *
 * {"address": {trtype/adrfam/traddr/trsvcid},
 *  "ana_states": [{ana_group: N, ana_state: "..."}, ...]}
 * ANA reporting이 활성화된 경우에만 ana_states 배열을 추가.
 * subsystem의 max_nsid 수만큼 ANA group(1-based)별 상태를 출력.
 *
 * 호출 체인:
 *   rpc_nvmf_get_listeners_paused → TAILQ_FOREACH(listeners) → [dump_nvmf_subsystem_listener]
 */
static void
dump_nvmf_subsystem_listener(struct spdk_json_write_ctx *w,
			     struct spdk_nvmf_subsystem_listener *listener)
{
	uint32_t i;                                                   /* [한국어] ANA group 순회 인덱스 (0-based) */

	spdk_json_write_object_begin(w);                              /* [한국어] listener 객체 "{" 시작 */

	spdk_json_write_named_object_begin(w, "address");             /* [한국어] "address": { */
	nvmf_transport_listen_dump_trid(listener->trid, w);           /* [한국어] trtype/adrfam/traddr/trsvcid 출력 */
	spdk_json_write_object_end(w);                                /* [한국어] "address": } */

	if (spdk_nvmf_subsystem_get_ana_reporting(listener->subsystem)) {
		/* [한국어] ANA reporting이 활성화된 subsystem에서만 ANA group 상태 출력 */
		spdk_json_write_named_array_begin(w, "ana_states");       /* [한국어] "ana_states": [ */
		for (i = 0; i < listener->subsystem->max_nsid; i++) {
			/* [한국어] nsid 인덱스 i → ANA group (i+1) 매핑 */
			spdk_json_write_object_begin(w);                  /* [한국어] 항목 "{" */
			spdk_json_write_named_uint32(w, "ana_group", i + 1);
			/* [한국어] "ana_group": ANA Group ID (1-based) */
			spdk_json_write_named_string(w, "ana_state",
						     nvme_ana_state_str(listener->ana_state[i]));
			/* [한국어] "ana_state": 이 listener의 해당 ANA group에 대한 현재 접근 상태 */
			spdk_json_write_object_end(w);                    /* [한국어] 항목 "}" */
		}
		spdk_json_write_array_end(w);                             /* [한국어] "ana_states": ] */
	}

	spdk_json_write_object_end(w);                                /* [한국어] listener 객체 "}" 닫기 */
}

/* [한국어] nvmf_subsystem_get_controllers / get_qpairs / get_listeners RPC 공용 컨텍스트.
 * 세 RPC 모두 subsystem을 PAUSED 상태로 만든 후 데이터를 조회하여 일관성을 보장한다. */
struct rpc_subsystem_query_ctx {
	char *nqn;
	/* [한국어] 대상 subsystem NQN (NVMe Qualified Name).
	 * 설정자: _rpc_nvmf_subsystem_query에서 spdk_json_decode_object로 설정.
	 * 읽는 자: spdk_nvmf_tgt_find_subsystem, 에러 로그.
	 * 값 범위: 유효한 subsystem NQN 문자열 (NULL 불가).
	 * 해제: free_rpc_subsystem_query_ctx에서 free(). */

	char *tgt_name;
	/* [한국어] target 이름 (NULL이면 default target 사용).
	 * 설정자: _rpc_nvmf_subsystem_query에서 optional 파라미터로 설정.
	 * 읽는 자: spdk_nvmf_get_tgt(ctx->tgt_name).
	 * 해제: free_rpc_subsystem_query_ctx에서 free(). */

	struct spdk_nvmf_subsystem *subsystem;
	/* [한국어] 룩업된 subsystem 포인터. pause/resume 대상.
	 * 설정자: _rpc_nvmf_subsystem_query에서 spdk_nvmf_tgt_find_subsystem으로 설정.
	 * 읽는 자: pause 콜백(paused_fn)에서 ctrlr/qpair/listener 조회에 사용.
	 * 동기화: PAUSED 상태에서 단일 reactor thread가 접근. */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답 전송 대상 RPC 요청.
	 * 설정자: _rpc_nvmf_subsystem_query에서 저장.
	 * 읽는 자: 각 paused 콜백에서 spdk_jsonrpc_begin_result/end_result로 사용. */

	struct spdk_json_write_ctx *w;
	/* [한국어] JSON 응답 직렬화 컨텍스트.
	 * get_qpairs에서 spdk_for_each_channel 중 공유되므로 ctx에 저장.
	 * get_controllers/get_listeners는 로컬 변수로 충분하지만 동일 구조체 사용. */
};

/* [한국어] subsystem query 3종 RPC 공용 파라미터 디코더 테이블. */
static const struct spdk_json_object_decoder rpc_subsystem_query_decoders[] = {
	{"nqn", offsetof(struct rpc_subsystem_query_ctx, nqn), spdk_json_decode_string},
	/* [한국어] 필수: 대상 subsystem NQN */
	{"tgt_name", offsetof(struct rpc_subsystem_query_ctx, tgt_name), spdk_json_decode_string, true},
	/* [한국어] optional: target 이름 (생략 시 default target) */
};

/*
 * [한국어]
 * free_rpc_subsystem_query_ctx - rpc_subsystem_query_ctx 메모리 해제
 *
 * @ctx: 해제할 컨텍스트.
 *
 * nqn, tgt_name(동적 할당), ctx 자체를 순서대로 해제.
 */
static void
free_rpc_subsystem_query_ctx(struct rpc_subsystem_query_ctx *ctx)
{
	free(ctx->nqn);       /* [한국어] 동적 할당된 NQN 문자열 해제 */
	free(ctx->tgt_name);  /* [한국어] 동적 할당된 target 이름 해제 (NULL 안전) */
	free(ctx);            /* [한국어] ctx 구조체 자체 해제 */
}

/*
 * [한국어]
 * rpc_nvmf_get_controllers_paused - subsystem pause 완료 후 ctrlr 목록 dump
 *
 * @subsystem: PAUSED 상태로 전환된 subsystem.
 * @cb_arg:    rpc_subsystem_query_ctx*.
 * @status:    pause 결과 (0=성공).
 *
 * PAUSED 상태에서 subsystem->ctrlrs TAILQ를 순회하며 dump_nvmf_ctrlr로 직렬화.
 * dump 완료 후 응답 전송, resume(NULL callback), ctx 해제 순으로 정리.
 *
 * 실행 컨텍스트: SPDK event loop에서 단일 thread로 실행.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_pause → [rpc_nvmf_get_controllers_paused]
 *     → TAILQ_FOREACH(ctrlrs) → dump_nvmf_ctrlr
 *     → spdk_nvmf_subsystem_resume(NULL, NULL)
 */
static void
rpc_nvmf_get_controllers_paused(struct spdk_nvmf_subsystem *subsystem,
				void *cb_arg, int status)
{
	struct rpc_subsystem_query_ctx *ctx = cb_arg; /* [한국어] 콜백 인수에서 ctx 복원 */
	struct spdk_json_write_ctx *w;                /* [한국어] JSON 응답 컨텍스트 */
	struct spdk_nvmf_ctrlr *ctrlr;               /* [한국어] TAILQ 순회 포인터 */

	w = spdk_jsonrpc_begin_result(ctx->request);  /* [한국어] JSON 응답 시작 */

	spdk_json_write_array_begin(w);               /* [한국어] ctrlr 배열 "[" 시작 */
	TAILQ_FOREACH(ctrlr, &ctx->subsystem->ctrlrs, link) {
		dump_nvmf_ctrlr(w, ctrlr);            /* [한국어] 각 ctrlr JSON 직렬화 */
	}
	spdk_json_write_array_end(w);                 /* [한국어] "]" 닫기 */

	spdk_jsonrpc_end_result(ctx->request, w);     /* [한국어] 응답 전송 */

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, NULL, NULL)) {
		/* [한국어] resume 실패 — subsystem이 PAUSED 상태로 고착되는 위험 (FIXME) */
		SPDK_ERRLOG("Resuming subsystem with NQN %s failed\n", ctx->nqn);
		/* FIXME: RPC should fail if resuming the subsystem failed. */
	}

	free_rpc_subsystem_query_ctx(ctx);            /* [한국어] ctx 해제 */
}

/*
 * [한국어]
 * rpc_nvmf_get_qpairs_done - spdk_for_each_channel 완료 후 qpair dump 마무리
 *
 * @i:      io_channel_iter (ctx 포함).
 * @status: 0=성공.
 *
 * 모든 reactor의 qpair dump 완료 후 배열 닫기("]}"), 응답 전송, resume, ctx 해제.
 *
 * 호출 체인:
 *   spdk_for_each_channel → (각 reactor: rpc_nvmf_get_qpairs)
 *     → [rpc_nvmf_get_qpairs_done]
 */
static void
rpc_nvmf_get_qpairs_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_subsystem_query_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] iter에서 ctx 복원 */

	spdk_json_write_array_end(ctx->w);             /* [한국어] qpair 배열 "]" 닫기 */
	spdk_jsonrpc_end_result(ctx->request, ctx->w); /* [한국어] 응답 전송 */

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, NULL, NULL)) {
		/* [한국어] resume 실패 */
		SPDK_ERRLOG("Resuming subsystem with NQN %s failed\n", ctx->nqn);
		/* FIXME: RPC should fail if resuming the subsystem failed. */
	}

	free_rpc_subsystem_query_ctx(ctx);             /* [한국어] ctx 해제 */
}

/*
 * [한국어]
 * rpc_nvmf_get_qpairs - 각 reactor에서 qpair dump (spdk_for_each_channel worker)
 *
 * @i: io_channel_iter (현재 채널 및 ctx 포함).
 *
 * 현재 reactor의 poll group에서 이 subsystem에 속한 qpair만 필터링하여
 * dump_nvmf_qpair로 JSON 직렬화. 완료 후 spdk_for_each_channel_continue.
 *
 * 실행 컨텍스트: 각 reactor thread마다 1번씩 실행 (병렬 아님, 순차 실행).
 *
 * 호출 체인:
 *   spdk_for_each_channel → [rpc_nvmf_get_qpairs] (각 reactor별)
 *     → dump_nvmf_qpair → spdk_for_each_channel_continue → 다음 reactor
 */
static void
rpc_nvmf_get_qpairs(struct spdk_io_channel_iter *i)
{
	struct rpc_subsystem_query_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] iter에서 ctx 복원 */
	struct spdk_io_channel *ch;                    /* [한국어] 현재 reactor의 IO 채널 */
	struct spdk_nvmf_poll_group *group;            /* [한국어] IO 채널에 바인딩된 poll group */
	struct spdk_nvmf_qpair *qpair;                /* [한국어] TAILQ 순회 포인터 */

	ch = spdk_io_channel_iter_get_channel(i);      /* [한국어] 현재 reactor의 IO 채널 획득 */
	group = spdk_io_channel_get_ctx(ch);           /* [한국어] IO 채널의 사용자 컨텍스트 = poll group */

	TAILQ_FOREACH(qpair, &group->qpairs, link) {
		if (qpair->ctrlr && qpair->ctrlr->subsys == ctx->subsystem) {
			/* [한국어] 이 subsystem에 속한 ctrlr의 qpair만 dump (다른 subsystem 제외) */
			dump_nvmf_qpair(ctx->w, qpair);    /* [한국어] qpair JSON 직렬화 */
		}
	}

	spdk_for_each_channel_continue(i, 0);          /* [한국어] 다음 reactor로 진행 (0=계속) */
}

/*
 * [한국어]
 * rpc_nvmf_get_qpairs_paused - subsystem pause 완료 후 qpair dump 체인 시작
 *
 * @subsystem: PAUSED 상태의 subsystem.
 * @cb_arg:    rpc_subsystem_query_ctx*.
 * @status:    0=pause 성공.
 *
 * pause 완료 후 JSON 응답 시작, qpair 배열 "[" 열기, spdk_for_each_channel 시작.
 * for_each_channel 완료 시 rpc_nvmf_get_qpairs_done이 배열 닫기 + 응답 전송 + resume.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_pause → [rpc_nvmf_get_qpairs_paused]
 *     → spdk_for_each_channel → rpc_nvmf_get_qpairs (각 reactor)
 *       → rpc_nvmf_get_qpairs_done
 */
static void
rpc_nvmf_get_qpairs_paused(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct rpc_subsystem_query_ctx *ctx = cb_arg;  /* [한국어] 콜백 인수에서 ctx 복원 */

	ctx->w = spdk_jsonrpc_begin_result(ctx->request); /* [한국어] JSON 응답 시작 (ctx->w에 저장) */

	spdk_json_write_array_begin(ctx->w);           /* [한국어] qpair 배열 "[" 시작 */

	spdk_for_each_channel(ctx->subsystem->tgt,
			      rpc_nvmf_get_qpairs,
			      ctx,
			      rpc_nvmf_get_qpairs_done);
	/* [한국어] tgt의 모든 IO channel에서 rpc_nvmf_get_qpairs 실행.
	 *          ctx->w를 공유하며 각 reactor가 자신의 qpair를 append. */
}

/*
 * [한국어]
 * rpc_nvmf_get_listeners_paused - subsystem pause 완료 후 listener 목록 dump
 *
 * @subsystem: PAUSED 상태의 subsystem.
 * @cb_arg:    rpc_subsystem_query_ctx*.
 * @status:    0=pause 성공.
 *
 * PAUSED 상태에서 subsystem->listeners TAILQ를 순회. nvmf_subsystem_listener_is_active로
 * 아직 활성화되지 않은 listener(추가 중인 것)를 제외하고 dump_nvmf_subsystem_listener로 직렬화.
 * 완료 후 응답 전송 + resume + ctx 해제.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_pause → [rpc_nvmf_get_listeners_paused]
 *     → TAILQ_FOREACH(listeners, active 필터) → dump_nvmf_subsystem_listener
 *     → spdk_nvmf_subsystem_resume
 */
static void
rpc_nvmf_get_listeners_paused(struct spdk_nvmf_subsystem *subsystem,
			      void *cb_arg, int status)
{
	struct rpc_subsystem_query_ctx *ctx = cb_arg;   /* [한국어] 콜백 인수에서 ctx 복원 */
	struct spdk_json_write_ctx *w;                  /* [한국어] JSON 응답 컨텍스트 */
	struct spdk_nvmf_subsystem_listener *listener;  /* [한국어] TAILQ 순회 포인터 */

	w = spdk_jsonrpc_begin_result(ctx->request);    /* [한국어] JSON 응답 시작 */

	spdk_json_write_array_begin(w);                 /* [한국어] listener 배열 "[" 시작 */

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (!nvmf_subsystem_listener_is_active(listener)) {
			continue; /* [한국어] 아직 transport가 listen 시작 전인 listener 건너뜀 */
		}

		dump_nvmf_subsystem_listener(w, listener); /* [한국어] 각 active listener JSON 직렬화 */
	}
	spdk_json_write_array_end(w);                   /* [한국어] "]" 닫기 */

	spdk_jsonrpc_end_result(ctx->request, w);       /* [한국어] 응답 전송 */

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, NULL, NULL)) {
		/* [한국어] resume 실패 — PAUSED 고착 위험 */
		SPDK_ERRLOG("Resuming subsystem with NQN %s failed\n", ctx->nqn);
		/* FIXME: RPC should fail if resuming the subsystem failed. */
	}

	free_rpc_subsystem_query_ctx(ctx);              /* [한국어] ctx 해제 */
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
	struct rpc_subsystem_query_ctx *ctx;      /* [한국어] 비동기 체인 컨텍스트 */
	struct spdk_nvmf_subsystem *subsystem;    /* [한국어] 룩업된 subsystem 포인터 */
	struct spdk_nvmf_tgt *tgt;               /* [한국어] 룩업된 target 포인터 */

	ctx = calloc(1, sizeof(*ctx));            /* [한국어] ctx heap 할당 */
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	ctx->request = request;                   /* [한국어] 응답 대상 저장 */

	if (spdk_json_decode_object(params, rpc_subsystem_query_decoders,
				    SPDK_COUNTOF(rpc_subsystem_query_decoders),
				    ctx)) {
		/* [한국어] nqn 필수 파라미터 디코드 실패 */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);  /* [한국어] tgt_name으로 target 룩업 (NULL=default) */
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	/* [한국어] NQN으로 target 내 subsystem 룩업 */
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}

	ctx->subsystem = subsystem;               /* [한국어] subsystem 포인터 ctx에 저장 */

	if (spdk_nvmf_subsystem_pause(subsystem, 0, cb_fn, ctx)) {
		/* [한국어] nsid=0: 모든 NS를 대상으로 pause.
		 *          cb_fn: rpc_nvmf_get_controllers_paused / get_qpairs_paused / get_listeners_paused */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}
	/* [한국어] pause 요청 완료 — 실제 PAUSED 상태 진입 후 cb_fn 호출 */
}

/*
 * [한국어]
 * rpc_nvmf_subsystem_get_controllers - "nvmf_subsystem_get_controllers" RPC
 *
 * @request: RPC 요청.
 * @params: JSON (nqn 필수, tgt_name optional).
 *
 * subsystem을 pause 후 ctrlr 목록을 JSON 배열로 반환.
 * _rpc_nvmf_subsystem_query에 rpc_nvmf_get_controllers_paused를 콜백으로 전달.
 */
static void
rpc_nvmf_subsystem_get_controllers(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	_rpc_nvmf_subsystem_query(request, params, rpc_nvmf_get_controllers_paused);
	/* [한국어] pause 완료 시 rpc_nvmf_get_controllers_paused 호출 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_get_controllers" RUNTIME RPC 등록. */
SPDK_RPC_REGISTER("nvmf_subsystem_get_controllers", rpc_nvmf_subsystem_get_controllers,
		  SPDK_RPC_RUNTIME);

/*
 * [한국어]
 * rpc_nvmf_subsystem_get_qpairs - "nvmf_subsystem_get_qpairs" RPC
 *
 * @request: RPC 요청.
 * @params: JSON (nqn 필수, tgt_name optional).
 *
 * subsystem을 pause 후 전체 reactor에서 qpair를 수집하여 JSON 배열로 반환.
 * spdk_for_each_channel을 사용하므로 가장 복잡한 비동기 체인을 갖는다.
 */
static void
rpc_nvmf_subsystem_get_qpairs(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	_rpc_nvmf_subsystem_query(request, params, rpc_nvmf_get_qpairs_paused);
	/* [한국어] pause 완료 시 rpc_nvmf_get_qpairs_paused → spdk_for_each_channel 체인 시작 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_get_qpairs" RUNTIME RPC 등록. */
SPDK_RPC_REGISTER("nvmf_subsystem_get_qpairs", rpc_nvmf_subsystem_get_qpairs, SPDK_RPC_RUNTIME);

/*
 * [한국어]
 * rpc_nvmf_subsystem_get_listeners - "nvmf_subsystem_get_listeners" RPC
 *
 * @request: RPC 요청.
 * @params: JSON (nqn 필수, tgt_name optional).
 *
 * subsystem을 pause 후 active listener 목록을 JSON 배열로 반환.
 * ANA reporting 활성화 시 각 listener에 ANA group 상태 배열도 포함.
 */
static void
rpc_nvmf_subsystem_get_listeners(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	_rpc_nvmf_subsystem_query(request, params, rpc_nvmf_get_listeners_paused);
	/* [한국어] pause 완료 시 rpc_nvmf_get_listeners_paused 호출 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_subsystem_get_listeners" RUNTIME RPC 등록. */
SPDK_RPC_REGISTER("nvmf_subsystem_get_listeners", rpc_nvmf_subsystem_get_listeners,
		  SPDK_RPC_RUNTIME);

/* [한국어] mDNS PRR(Public Registration Record) RPC 파라미터 구조체.
 * NVMe-oF target의 mDNS 기반 자동 발견(PRR: Persistent Resource Record)을
 * 활성화/비활성화할 때 target을 지정하는 데 사용. */
struct rpc_mdns_prr {
	char *tgt_name;
	/* [한국어] mDNS PRR을 발행/중단할 target 이름 (NULL=default).
	 * 설정자: params 있을 때 spdk_json_decode_object로 설정.
	 * 읽는 자: spdk_nvmf_get_tgt(req.tgt_name) 호출.
	 * 해제: 스택 변수이므로 free(req.tgt_name)으로 직접 해제. */
};

/* [한국어] nvmf_publish_mdns_prr / nvmf_stop_mdns_prr 공용 파라미터 디코더. */
static const struct spdk_json_object_decoder rpc_nvmf_publish_mdns_prr_decoders[] = {
	{"tgt_name", offsetof(struct rpc_mdns_prr, tgt_name), spdk_json_decode_string, true},
	/* [한국어] optional: target 이름 */
};

/*
 * [한국어]
 * rpc_nvmf_publish_mdns_prr - "nvmf_publish_mdns_prr" RPC
 *
 * @request: RPC 요청.
 * @params: JSON (tgt_name optional).
 * @return: void. 성공 시 true.
 *
 * mDNS PRR(Persistent Resource Record)을 발행하여 NVMe-oF target을 mDNS 네트워크에 광고.
 * host 측 initiator가 mDNS를 통해 target을 자동 탐색할 수 있게 한다.
 * 내부적으로 nvmf_publish_mdns_prr(tgt)를 호출 (lib/nvmf/nvmf.c).
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_publish_mdns_prr] → nvmf_publish_mdns_prr
 */
static void
rpc_nvmf_publish_mdns_prr(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	int rc;                                 /* [한국어] nvmf_publish_mdns_prr 반환값 */
	struct rpc_mdns_prr req = { 0 };        /* [한국어] 스택 파라미터 (tgt_name) */
	struct spdk_nvmf_tgt *tgt;             /* [한국어] 룩업된 target */

	if (params) {
		/* [한국어] params가 있을 때만 디코드 (tgt_name optional) */
		if (spdk_json_decode_object(params, rpc_nvmf_publish_mdns_prr_decoders,
					    SPDK_COUNTOF(rpc_nvmf_publish_mdns_prr_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name); /* [한국어] target 룩업 */
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free(req.tgt_name);
		return;
	}

	rc = nvmf_publish_mdns_prr(tgt);       /* [한국어] mDNS PRR 발행 (mdns 서버에 등록) */
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		/* [한국어] 발행 실패 — errno 기반 에러 메시지 */
		free(req.tgt_name);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공 응답 */
	free(req.tgt_name);                     /* [한국어] 동적 할당 tgt_name 해제 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_publish_mdns_prr" RUNTIME RPC 등록. */
SPDK_RPC_REGISTER("nvmf_publish_mdns_prr", rpc_nvmf_publish_mdns_prr, SPDK_RPC_RUNTIME);

/*
 * [한국어]
 * rpc_nvmf_stop_mdns_prr - "nvmf_stop_mdns_prr" RPC
 *
 * @request: RPC 요청.
 * @params: JSON (tgt_name optional).
 * @return: void. 성공 시 true.
 *
 * 이전에 nvmf_publish_mdns_prr로 발행된 mDNS PRR을 중단.
 * mDNS 네트워크에서 target 광고를 제거하여 새 initiator의 자동 탐색을 막는다.
 * 내부적으로 nvmf_tgt_stop_mdns_prr(tgt) 호출.
 *
 * 호출 체인:
 *   SPDK JSON-RPC 서버 → [rpc_nvmf_stop_mdns_prr] → nvmf_tgt_stop_mdns_prr
 */
static void
rpc_nvmf_stop_mdns_prr(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_mdns_prr req = { 0 };        /* [한국어] 스택 파라미터 (tgt_name) */
	struct spdk_nvmf_tgt *tgt;             /* [한국어] 룩업된 target */

	if (params) {
		/* [한국어] params가 있을 때만 디코드 */
		if (spdk_json_decode_object(params, rpc_nvmf_publish_mdns_prr_decoders,
					    SPDK_COUNTOF(rpc_nvmf_publish_mdns_prr_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name); /* [한국어] target 룩업 */
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free(req.tgt_name);
		return;
	}

	nvmf_tgt_stop_mdns_prr(tgt);           /* [한국어] mDNS PRR 중단 (mdns 서버에서 제거) */

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공 응답 */
	free(req.tgt_name);                     /* [한국어] 동적 할당 tgt_name 해제 */
}
/* [한국어] SPDK_RPC_REGISTER: "nvmf_stop_mdns_prr" RUNTIME RPC 등록. */
SPDK_RPC_REGISTER("nvmf_stop_mdns_prr", rpc_nvmf_stop_mdns_prr, SPDK_RPC_RUNTIME);
