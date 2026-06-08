/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK RAID bdev JSON-RPC 핸들러 모음 (bdev_raid_rpc.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 SPDK RAID bdev 모듈이 외부로 노출하는 JSON-RPC 2.0 메서드 핸들러를 구현한다.
 * 사용자나 관리 도구(spdk_rpc.py 등)가 Unix socket(기본 /var/tmp/spdk.sock)을 통해 JSON
 * 요청을 보내면, SPDK의 JSON-RPC 서버가 이 파일에 등록된 핸들러들을 호출한다. 핸들러들은
 * JSON 파라미터를 파싱하여 RAID 코어(bdev_raid.c)의 함수를 호출하고, 결과를 JSON 응답으로
 * 직렬화하여 클라이언트에게 반환한다. SPDK_RPC_REGISTER 매크로로 각 메서드를 STARTUP 또는
 * RUNTIME 단계에 등록하며, lib/rpc의 디스패처가 메서드 이름 매칭 후 해당 핸들러를 실행한다.
 *
 * 본 파일이 제공하는 RPC 메서드:
 *  - bdev_raid_get_bdevs    : RAID bdev 목록 조회 (카테고리별 필터: all/online/configuring/offline).
 *  - bdev_raid_create       : 새 RAID bdev 생성 (레벨, base bdev 목록, strip 크기, UUID 등 지정).
 *  - bdev_raid_delete       : 기존 RAID bdev 삭제.
 *  - bdev_raid_add_base_bdev   : 기존 RAID bdev에 base bdev 동적 추가.
 *  - bdev_raid_remove_base_bdev: 기존 RAID bdev에서 base bdev 동적 제거.
 *  - bdev_raid_set_options  : RAID 전역 옵션 설정 (process_window_size_kb 등).
 *
 * === 전체 아키텍처에서의 위치 ===
 *   관리 클라이언트(spdk_rpc.py / curl 등)
 *     → Unix socket JSON-RPC 서버 (lib/jsonrpc, lib/rpc)
 *       → SPDK_RPC_REGISTER가 등록한 핸들러 (본 파일)
 *         → RAID 코어 API (bdev_raid.c의 raid_bdev_create / delete / add_base_bdev 등)
 *           → lib/bdev (bdev 레이어에 RAID 가상 bdev 등록/해제)
 *
 * 핸들러가 비동기인 경우(create, delete, add/remove base bdev): RAID 코어 함수에 콜백을 등록
 * 하고, 콜백이 호출될 때 JSON 응답을 전송한다. 이 사이 핸들러 함수는 이미 반환되었으므로
 * 컨텍스트(ctx)는 힙(calloc/malloc)으로 보관한다.
 *
 * 실행 컨텍스트: SPDK app thread (단일 스레드). JSON-RPC 핸들러와 콜백 모두 app thread에서
 * 직렬 실행되므로 별도 동기화가 필요 없다.
 *
 * === 타 모듈과의 연결 ===
 * - bdev_raid.h: RAID 코어 자료구조(raid_bdev, g_raid_bdev_list, enum raid_bdev_state,
 *   struct spdk_raid_bdev_opts), 코어 API(raid_bdev_create, raid_bdev_delete,
 *   raid_bdev_add_base_bdev, raid_bdev_remove_base_bdev, raid_bdev_find_by_name,
 *   raid_bdev_str_to_level, raid_bdev_str_to_state, raid_bdev_write_info_json,
 *   raid_bdev_get_opts, raid_bdev_set_opts).
 * - spdk/rpc.h: SPDK_RPC_REGISTER 매크로, SPDK_RPC_STARTUP/RUNTIME 상수.
 * - spdk/bdev.h: spdk_bdev_open_ext, spdk_bdev_close, spdk_bdev_desc_get_bdev 등.
 * - spdk/json.h + spdk/jsonrpc.h: JSON 파싱(spdk_json_decode_*),
 *   응답 직렬화(spdk_jsonrpc_begin_result, spdk_jsonrpc_send_bool_response 등).
 * - spdk/util.h: SPDK_COUNTOF (배열 길이 계산 매크로).
 * - spdk/string.h: spdk_strerror, spdk_uuid_fmt_lower.
 * - spdk/log.h: SPDK_ERRLOG, SPDK_DEBUGLOG.
 * - spdk/env.h: 간접 의존 (SPDK 환경 초기화).
 *
 * 데이터 흐름: JSON bytes → spdk_json_decode_object → 요청 구조체(rpc_bdev_raid_*) → RAID
 * 코어 → (비동기 결과) → JSON 직렬화 → socket 응답.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rpc_bdev_raid_get_bdevs  : get_bdevs RPC의 "category" 파라미터 보관.
 * - struct rpc_bdev_raid_create     : create RPC의 전체 파라미터 (name/strip_size_kb/level/
 *                                     base_bdevs/uuid/superblock_enabled).
 * - struct rpc_bdev_raid_create_ctx : create RPC의 비동기 진행 컨텍스트. remaining/status로
 *                                     base bdev 추가 fan-out 완료를 집계.
 * - rpc_bdev_raid_get_bdevs()       : RAID bdev 목록을 JSON 배열로 반환. 동기.
 * - rpc_bdev_raid_create()          : RAID 인스턴스 생성 후 모든 base bdev을 fan-out 추가.
 *                                     비동기(rpc_bdev_raid_create_add_base_bdev_cb로 수렴).
 * - rpc_bdev_raid_delete()          : RAID 삭제. 비동기(bdev_raid_delete_done 콜백).
 * - rpc_bdev_raid_add_base_bdev()   : 기존 RAID에 base bdev 추가. 비동기.
 * - rpc_bdev_raid_remove_base_bdev(): 기존 RAID에서 base bdev 제거. bdev open 후 비동기.
 * - rpc_bdev_raid_set_options()     : 전역 RAID 옵션 설정. 동기.
 */

/* [한국어] spdk/rpc.h: SPDK_RPC_REGISTER 매크로와 SPDK_RPC_RUNTIME/STARTUP 플래그.
 * 이 매크로는 생성자 함수 패턴으로 lib/rpc의 전역 메서드 리스트에 핸들러를 등록한다. */
#include "spdk/rpc.h"
/* [한국어] spdk/bdev.h: spdk_bdev_open_ext / close / desc_get_bdev 등 bdev 사용자 API.
 * remove_base_bdev RPC에서 bdev 이름으로 desc를 열어 raid_bdev_remove_base_bdev에 전달한다. */
#include "spdk/bdev.h"
/* [한국어] bdev_raid.h: RAID 코어 자료구조와 API 선언. g_raid_bdev_list, raid_bdev_create 등. */
#include "bdev_raid.h"
/* [한국어] spdk/util.h: SPDK_COUNTOF(arr) = sizeof(arr)/sizeof(arr[0]). decode_object 시 디코더
 * 배열 길이 산출에 사용. 기타 spdk_min, SPDK_ALIGN_CEIL 등도 포함. */
#include "spdk/util.h"
/* [한국어] spdk/string.h: spdk_strerror (errno → 문자열), spdk_uuid_fmt_lower (UUID → 소문자
 * 문자열) 등. 에러 응답 메시지와 UUID JSON 직렬화에 사용. */
#include "spdk/string.h"
/* [한국어] spdk/log.h: SPDK_ERRLOG, SPDK_DEBUGLOG 로깅 매크로. */
#include "spdk/log.h"
/* [한국어] spdk/env.h: DPDK 기반 환경 추상화(hugepage/DMA). 본 파일에서 직접 사용하진 않으나
 * RAID 헤더를 통해 간접 의존되어 일관성을 위해 포함. */
#include "spdk/env.h"

/* [한국어] RPC bdev_raid_create에서 허용하는 base bdev 최대 개수. 255는 uint8_t 최대값에
 * 맞춘 것으로, RAID 코어에서 num_base_bdevs를 uint8_t로 저장하기 때문이다.
 * 이 값을 초과하는 base bdev 목록은 decode_base_bdevs가 거부. */
#define RPC_MAX_BASE_BDEVS 255

/*
 * Input structure for bdev_raid_get_bdevs RPC
 */
/*
 * [한국어]
 * struct rpc_bdev_raid_get_bdevs - bdev_raid_get_bdevs RPC의 입력 파라미터 구조체.
 *
 * JSON-RPC 파라미터 파싱 결과를 담는 임시 구조체. 핸들러 rpc_bdev_raid_get_bdevs가
 * 스택에서 = {} 로 초기화한 후, spdk_json_decode_object로 JSON 파라미터를 채운다.
 * 파라미터 파싱 이후 사용이 끝나면 free_rpc_bdev_raid_get_bdevs로 내부 메모리를 해제.
 */
struct rpc_bdev_raid_get_bdevs {
	/* category - all or online or configuring or offline */
	char *category;
	/* [한국어] 조회할 RAID bdev 카테고리 문자열. 동적 할당(spdk_json_decode_string이
	 * strdup 수행)되므로 free_rpc_bdev_raid_get_bdevs에서 free 필요.
	 * 설정자: spdk_json_decode_object → rpc_bdev_raid_get_bdevs_decoders.
	 * 읽는 자: rpc_bdev_raid_get_bdevs에서 raid_bdev_str_to_state로 상태 변환.
	 * 값 범위: "all", "online", "configuring", "offline" 중 하나.
	 *           그 외 값은 -EINVAL 에러 응답을 반환.
	 * 동기화: 핸들러 함수 내 스택 변수 단일 사용 → lockless. */
};

/*
 * brief:
 * free_rpc_bdev_raid_get_bdevs function frees RPC bdev_raid_get_bdevs related parameters
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
/*
 * [한국어]
 * free_rpc_bdev_raid_get_bdevs - rpc_bdev_raid_get_bdevs 요청 구조체의 내부 메모리 해제.
 *
 * @req: 해제 대상 구조체 포인터. 구조체 자체는 스택에 있으므로 free하지 않고,
 *       내부에 동적 할당된 문자열 필드만 free한다.
 * @return: 없음.
 *
 * spdk_json_decode_string은 내부적으로 strndup을 호출해 힙 메모리를 할당하므로,
 * 파싱 성공/실패 여부와 관계없이 항상 free를 호출해야 메모리 누수가 없다.
 * category가 NULL이더라도 free(NULL)은 안전하다(C 표준 보장).
 *
 * 호출 체인:
 *   rpc_bdev_raid_get_bdevs → [본 함수] (cleanup 레이블 또는 정상 종료 시)
 */
static void
free_rpc_bdev_raid_get_bdevs(struct rpc_bdev_raid_get_bdevs *req)
{
	/* [한국어] spdk_json_decode_string이 strdup으로 할당한 category 문자열 해제.
	 * NULL이면 무해. */
	free(req->category);
}

/*
 * Decoder object for RPC get_raids
 */
/* [한국어] rpc_bdev_raid_get_bdevs_decoders - bdev_raid_get_bdevs RPC의 JSON 파라미터 디코더 테이블.
 *
 * spdk_json_decode_object가 이 배열을 보고 JSON 객체의 각 키를 구조체 필드로 매핑한다.
 * {"category": <string>} 형태 1개 필드만 허용. optional=false (마지막 인자 생략 = 필수).
 * 파싱 실패 시 -EINVAL을 반환하며 핸들러가 PARSE_ERROR 응답을 보낸다. */
static const struct spdk_json_object_decoder rpc_bdev_raid_get_bdevs_decoders[] = {
	/* [한국어] "category" 키 → rpc_bdev_raid_get_bdevs.category 필드로 디코딩.
	 * offsetof로 구조체 내 필드 위치를 바이트 오프셋으로 지정. */
	{"category", offsetof(struct rpc_bdev_raid_get_bdevs, category), spdk_json_decode_string},
};

/*
 * brief:
 * rpc_bdev_raid_get_bdevs function is the RPC for rpc_bdev_raid_get_bdevs. This is used to list
 * all the raid bdev names based on the input category requested. Category should be
 * one of "all", "online", "configuring" or "offline". "all" means all the raids
 * whether they are online or configuring or offline. "online" is the raid bdev which
 * is registered with bdev layer. "configuring" is the raid bdev which does not have
 * full configuration discovered yet. "offline" is the raid bdev which is not
 * registered with bdev as of now and it has encountered any error or user has
 * requested to offline the raid.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
/*
 * [한국어]
 * rpc_bdev_raid_get_bdevs - RAID bdev 목록 조회 RPC 핸들러.
 *
 * @request: JSON-RPC 응답 전송에 사용하는 요청 컨텍스트.
 * @params : JSON 파라미터 값 포인터 (spdk_json_val 배열의 첫 번째).
 * @return : 없음. 결과는 spdk_jsonrpc_send_*/end_result로 비동기 전송.
 *
 * 동기/배경: 사용자가 어떤 RAID bdev들이 존재하는지 확인하기 위해 호출. 카테고리에 따라
 * 전체(all), 온라인(online), 설정 중(configuring), 오프라인(offline) 을 필터링.
 *
 * 동작 단계:
 *  1) spdk_json_decode_object로 category 파라미터 파싱.
 *  2) raid_bdev_str_to_state로 문자열 → enum raid_bdev_state 변환. "all"이면 MAX(전체).
 *  3) g_raid_bdev_list를 TAILQ_FOREACH로 순회하며 상태 필터 후 JSON 배열로 직렬화.
 *     각 항목에 name, uuid, info(raid_bdev_write_info_json) 포함.
 *  4) 배열 JSON 응답 전송 후 파라미터 구조체 free.
 *
 * 실행 컨텍스트: app thread. g_raid_bdev_list는 app thread에서만 갱신되므로 lockless 순회 안전.
 *
 * 호출 체인:
 *   lib/rpc 디스패처 → [본 함수] → spdk_jsonrpc_end_result → 소켓 응답
 */
static void
rpc_bdev_raid_get_bdevs(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	/* [한국어] RPC 파라미터 구조체. = {} 로 zero-init해 decode 전 필드가 NULL임을 보장. */
	struct rpc_bdev_raid_get_bdevs   req = {};
	/* [한국어] JSON 배열 응답 직렬화 컨텍스트 (write 함수들에 전달). */
	struct spdk_json_write_ctx  *w;
	/* [한국어] TAILQ 순회 포인터. */
	struct raid_bdev            *raid_bdev;
	/* [한국어] 조회할 상태 (RAID_BDEV_STATE_MAX = "all"). */
	enum raid_bdev_state        state;

	if (spdk_json_decode_object(params, rpc_bdev_raid_get_bdevs_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_get_bdevs_decoders),
				    &req)) {
		/* [한국어] JSON 파라미터 파싱 실패 → PARSE_ERROR(-32700) 응답 후 cleanup. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] "all" 이외의 문자열은 enum raid_bdev_state으로 변환. "all"이면 MAX 반환.
	 * 변환 실패 + "all" 도 아니면 유효하지 않은 카테고리 → EINVAL 응답. */
	state = raid_bdev_str_to_state(req.category);
	if (state == RAID_BDEV_STATE_MAX && strcmp(req.category, "all") != 0) {
		/* [한국어] 지원하지 않는 카테고리 값 → EINVAL 오류 응답. */
		spdk_jsonrpc_send_error_response(request, -EINVAL, spdk_strerror(EINVAL));
		goto cleanup;
	}

	/* [한국어] JSON 응답 쓰기 컨텍스트 획득. 이후 w를 통해 JSON을 스트리밍 직렬화. */
	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] 최상위 배열 시작. 결과는 RAID bdev 정보 객체들의 JSON 배열. */
	spdk_json_write_array_begin(w);

	/* Get raid bdev list based on the category requested */
	/* [한국어] 전역 RAID bdev 리스트(g_raid_bdev_list) 전체를 순회하며 카테고리 필터 적용.
	 * app thread에서만 리스트가 변경되며 현재도 app thread이므로 TAILQ 순회 안전. */
	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		if (raid_bdev->state == state || state == RAID_BDEV_STATE_MAX) {
			/* [한국어] 스택 임시 UUID 문자열 버퍼. SPDK_UUID_STRING_LEN = 37 (하이픈 포함). */
			char uuid_str[SPDK_UUID_STRING_LEN];

			/* [한국어] 개별 RAID bdev 정보를 JSON 객체로 직렬화. */
			spdk_json_write_object_begin(w);
			/* [한국어] RAID bdev 이름. */
			spdk_json_write_named_string(w, "name", raid_bdev->bdev.name);
			/* [한국어] UUID를 소문자 하이픈 구분 문자열로 변환(spdk_uuid_fmt_lower) 후 직렬화. */
			spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &raid_bdev->bdev.uuid);
			spdk_json_write_named_string(w, "uuid", uuid_str);
			/* [한국어] raid_bdev_write_info_json은 레벨, strip_size, base bdev 목록 등
			 * RAID 레벨별 상세 정보를 JSON 객체에 추가로 직렬화한다. */
			raid_bdev_write_info_json(raid_bdev, w);
			/* [한국어] 이 항목 JSON 객체 닫기. */
			spdk_json_write_object_end(w);
		}
	}
	/* [한국어] 최상위 배열 닫기. */
	spdk_json_write_array_end(w);
	/* [한국어] JSON 응답을 클라이언트에게 전송(소켓 write 큐에 enqueue). */
	spdk_jsonrpc_end_result(request, w);

cleanup:
	/* [한국어] JSON 디코딩 시 strdup된 category 문자열 해제. NULL이면 무해. */
	free_rpc_bdev_raid_get_bdevs(&req);
}
/* [한국어] SPDK_RPC_REGISTER: "bdev_raid_get_bdevs" 이름으로 핸들러를 RUNTIME 단계에 등록.
 * RUNTIME은 SPDK가 초기화 완료 후 사용자 I/O를 받는 단계. constructor 함수 안에 등록이 일어나
 * main() 이전에 전역 메서드 리스트에 추가됨. */
SPDK_RPC_REGISTER("bdev_raid_get_bdevs", rpc_bdev_raid_get_bdevs, SPDK_RPC_RUNTIME)

/*
 * Base bdevs in RPC bdev_raid_create
 */
/*
 * [한국어]
 * struct rpc_bdev_raid_create_base_bdevs - bdev_raid_create RPC의 base bdev 이름 목록.
 *
 * base_bdevs 필드에 최대 RPC_MAX_BASE_BDEVS(255)개의 bdev 이름 문자열을 담는다.
 * decode_base_bdevs 함수가 JSON 배열을 이 구조체로 디코딩하며, 각 이름은 spdk_json_decode_string
 * 이 strdup으로 할당하므로 free_rpc_bdev_raid_create_ctx에서 각각 free해야 한다.
 */
struct rpc_bdev_raid_create_base_bdevs {
	/* Number of base bdevs */
	size_t           num_base_bdevs;
	/* [한국어] 실제로 파싱된 base bdev 이름 수. decode_base_bdevs가 spdk_json_decode_array
	 * 호출 후 채우는 out-parameter. 0이면 base bdev 없는 빈 목록.
	 * 설정자: decode_base_bdevs → spdk_json_decode_array.
	 * 읽는 자: rpc_bdev_raid_create에서 루프 상한으로 사용.
	 * 값 범위: 0 ~ RPC_MAX_BASE_BDEVS. */

	/* List of base bdevs names */
	char             *base_bdevs[RPC_MAX_BASE_BDEVS];
	/* [한국어] base bdev 이름 문자열 포인터 배열. 각 항목은 spdk_json_decode_string이
	 * strdup으로 heap 할당한 C 문자열.
	 * 설정자: spdk_json_decode_array가 원소별 spdk_json_decode_string 호출.
	 * 읽는 자: rpc_bdev_raid_create 루프에서 raid_bdev_add_base_bdev에 전달.
	 * 값 범위: NULL(미할당) 또는 non-NULL 문자열. num_base_bdevs 이후 인덱스는 NULL.
	 * 동기화: ctx 단일 소유, app thread 직렬 접근. */
};

/*
 * Input structure for RPC rpc_bdev_raid_create
 */
/*
 * [한국어]
 * struct rpc_bdev_raid_create - bdev_raid_create RPC의 전체 입력 파라미터 구조체.
 *
 * spdk_json_decode_object가 rpc_bdev_raid_create_decoders 테이블을 사용해 JSON 파라미터를
 * 이 구조체에 채운다. 모든 동적 할당 필드(name, base_bdevs[] 내 문자열들)는
 * free_rpc_bdev_raid_create_ctx에서 해제된다.
 */
struct rpc_bdev_raid_create {
	/* Raid bdev name */
	char                                 *name;
	/* [한국어] 생성할 RAID bdev의 이름. 나중에 조회/삭제 시 이 이름을 사용.
	 * 설정자: spdk_json_decode_string. 읽는 자: raid_bdev_create 호출 인자.
	 * 값 범위: NULL이 아닌 비어있지 않은 C 문자열. */

	/* RAID strip size in KB */
	uint32_t                             strip_size_kb;
	/* [한국어] RAID 0/5F의 strip 크기 (KB 단위). 예: 64 = 64KiB strip = 128 섹터(512B).
	 * RAID 1/concat에서는 무시되며, 0이면 코어가 기본값을 적용할 수 있음.
	 * optional(디코더 테이블의 true 플래그)이므로 JSON에서 생략 가능.
	 * 설정자: spdk_json_decode_uint32 (선택). 읽는 자: raid_bdev_create. */

	/* RAID raid level */
	enum raid_level                      level;
	/* [한국어] RAID 레벨 식별자. 유효값: RAID0, RAID1, RAID5F, CONCAT 등.
	 * decode_raid_level 디코더가 문자열("raid0", "raid1" 등)을 enum으로 변환.
	 * 설정자: decode_raid_level. 읽는 자: raid_bdev_create. */

	/* Base bdevs information */
	struct rpc_bdev_raid_create_base_bdevs base_bdevs;
	/* [한국어] base bdev 이름 목록 (임베드 구조체). decode_base_bdevs가 JSON 배열을 파싱해 채움.
	 * 설정자: decode_base_bdevs. 읽는 자: rpc_bdev_raid_create 루프. */

	/* UUID for this raid bdev */
	struct spdk_uuid		     uuid;
	/* [한국어] 생성할 RAID bdev에 부여할 UUID. 생략 시 코어가 자동 생성(spdk_uuid_generate).
	 * superblock 기반 재조립 시 UUID로 RAID 인스턴스를 식별한다.
	 * optional(decode 시 true). 설정자: spdk_json_decode_uuid. */

	/* If set, information about raid bdev will be stored in superblock on each base bdev */
	bool                                 superblock_enabled;
	/* [한국어] true이면 RAID 구성 정보를 각 base bdev의 superblock에 저장(bdev_raid_sb.c).
	 * 데몬 재시작 후 superblock을 읽어 RAID를 자동 재구성할 수 있게 해준다.
	 * optional(decode 시 true, 기본값 false). */
};

/*
 * Decoder function for RPC bdev_raid_create to decode raid level
 */
/*
 * [한국어]
 * decode_raid_level - JSON 문자열 값을 enum raid_level로 디코딩하는 커스텀 디코더.
 *
 * @val: spdk_json_val 포인터. "raid0", "raid1", "raid5f", "concat" 등 레벨 문자열.
 * @out: enum raid_level*로 캐스팅된 출력 포인터.
 * @return: 0 성공, -EINVAL 유효하지 않은 레벨 문자열.
 *
 * spdk_json_decode_object의 함수 포인터로 등록되어, "raid_level" 키에 대해 호출된다.
 * 먼저 spdk_json_decode_string으로 일반 문자열을 추출한 뒤, raid_bdev_str_to_level로
 * enum 변환. INVALID_RAID_LEVEL이 반환되면 지원하지 않는 레벨이므로 -EINVAL.
 *
 * 호출 체인:
 *   spdk_json_decode_object → [본 함수] → raid_bdev_str_to_level → RAID0/RAID1/...
 */
static int
decode_raid_level(const struct spdk_json_val *val, void *out)
{
	int ret;
	/* [한국어] spdk_json_decode_string이 strdup으로 할당. 반드시 free 필요. */
	char *str = NULL;
	enum raid_level level;

	/* [한국어] JSON 값(val)을 C 문자열(str)로 변환. 실패 시 ret != 0. */
	ret = spdk_json_decode_string(val, &str);
	if (ret == 0 && str != NULL) {
		/* [한국어] 문자열을 RAID 레벨 enum으로 변환. 예: "raid0" → RAID0. */
		level = raid_bdev_str_to_level(str);
		if (level == INVALID_RAID_LEVEL) {
			/* [한국어] 알 수 없는 레벨 문자열 → 실패. */
			ret = -EINVAL;
		} else {
			/* [한국어] 변환 성공: 출력 포인터에 기록. */
			*(enum raid_level *)out = level;
		}
	}

	/* [한국어] str은 spdk_json_decode_string이 heap 할당했으므로 반드시 free.
	 * ret != 0이거나 str==NULL이어도 free(NULL)은 안전. */
	free(str);
	return ret;
}

/*
 * Decoder function for RPC bdev_raid_create to decode base bdevs list
 */
/*
 * [한국어]
 * decode_base_bdevs - JSON 배열을 rpc_bdev_raid_create_base_bdevs 구조체로 디코딩.
 *
 * @val: JSON 배열 값. 각 원소는 base bdev 이름 문자열.
 * @out: rpc_bdev_raid_create_base_bdevs* 로 캐스팅된 출력 포인터.
 * @return: spdk_json_decode_array의 반환값 (0=성공, 음수=실패).
 *
 * spdk_json_decode_array는 배열 원소마다 spdk_json_decode_string을 호출해 각 이름을
 * base_bdevs[] 배열에 채우고 num_base_bdevs를 증가시킨다. 최대 RPC_MAX_BASE_BDEVS개까지만
 * 허용한다.
 *
 * 호출 체인:
 *   spdk_json_decode_object → [본 함수] → spdk_json_decode_array(원소별 decode_string)
 */
static int
decode_base_bdevs(const struct spdk_json_val *val, void *out)
{
	/* [한국어] 출력 구조체 포인터로 캐스팅. */
	struct rpc_bdev_raid_create_base_bdevs *base_bdevs = out;
	/* [한국어] JSON 배열을 원소별로 스트링 디코딩해 base_bdevs->base_bdevs[]에 채움.
	 * 원소 크기는 char* 포인터 크기(sizeof(char*)), 최대 RPC_MAX_BASE_BDEVS개.
	 * num_base_bdevs 는 실제 파싱된 개수로 갱신됨. */
	return spdk_json_decode_array(val, spdk_json_decode_string, base_bdevs->base_bdevs,
				      RPC_MAX_BASE_BDEVS, &base_bdevs->num_base_bdevs, sizeof(char *));
}

/*
 * Decoder object for RPC bdev_raid_create
 */
/* [한국어] rpc_bdev_raid_create_decoders - bdev_raid_create RPC의 JSON 파라미터 디코더 테이블.
 *
 * spdk_json_decode_object가 이 배열을 보고 JSON 객체의 각 키를 rpc_bdev_raid_create 구조체로
 * 매핑한다. optional(마지막 인자 true) 필드는 JSON에서 생략 가능하며, 생략 시 zero-init된 값이
 * 유지된다(calloc 효과). */
static const struct spdk_json_object_decoder rpc_bdev_raid_create_decoders[] = {
	/* [한국어] "name" 키 → 필수 파라미터. RAID bdev 이름. */
	{"name", offsetof(struct rpc_bdev_raid_create, name), spdk_json_decode_string},
	/* [한국어] "strip_size_kb" 키 → 선택 파라미터. strip 크기(KB). 생략 시 0 유지. */
	{"strip_size_kb", offsetof(struct rpc_bdev_raid_create, strip_size_kb), spdk_json_decode_uint32, true},
	/* [한국어] "raid_level" 키 → 필수 파라미터. decode_raid_level 커스텀 디코더 사용. */
	{"raid_level", offsetof(struct rpc_bdev_raid_create, level), decode_raid_level},
	/* [한국어] "base_bdevs" 키 → 필수 파라미터. JSON 배열을 decode_base_bdevs로 파싱. */
	{"base_bdevs", offsetof(struct rpc_bdev_raid_create, base_bdevs), decode_base_bdevs},
	/* [한국어] "uuid" 키 → 선택 파라미터. spdk_json_decode_uuid로 UUID 문자열 → spdk_uuid 변환. */
	{"uuid", offsetof(struct rpc_bdev_raid_create, uuid), spdk_json_decode_uuid, true},
	/* [한국어] "superblock" 키 → 선택 파라미터. bool. true이면 각 base bdev에 superblock 저장. */
	{"superblock", offsetof(struct rpc_bdev_raid_create, superblock_enabled), spdk_json_decode_bool, true},
};

/*
 * [한국어]
 * struct rpc_bdev_raid_create_ctx - bdev_raid_create RPC의 비동기 실행 컨텍스트.
 *
 * rpc_bdev_raid_create 핸들러는 RAID 인스턴스를 생성한 후 base bdev들을 fan-out으로 추가하는
 * 비동기 연산을 수행한다. 각 base bdev 추가는 비동기이며 rpc_bdev_raid_create_add_base_bdev_cb
 * 가 호출될 때마다 remaining을 감소시킨다. remaining이 0이 되면(=모든 추가 완료) 전체 성공/실패를
 * 사용자에게 응답한다. 컨텍스트는 힙에 calloc으로 할당되어 콜백이 끝날 때까지 유효하다.
 */
struct rpc_bdev_raid_create_ctx {
	struct rpc_bdev_raid_create req;
	/* [한국어] 파싱된 RPC 파라미터 전체를 보관. name과 base_bdevs[] 내 문자열들은 동적 할당.
	 * 설정자: rpc_bdev_raid_create에서 decode 후 복사.
	 * 읽는 자: 에러 시 raid_bdev_delete 호출의 name, 성공/실패 응답 메시지.
	 * 동기화: 단일 스레드(app thread) 직렬 접근. */

	struct raid_bdev *raid_bdev;
	/* [한국어] 생성된 RAID bdev 포인터. raid_bdev_create 성공 시 설정.
	 * 설정자: rpc_bdev_raid_create에서 raid_bdev_create 결과로 설정.
	 * 읽는 자: 실패 시 raid_bdev_delete 호출, 콜백에서 이름 보고 등.
	 * 동기화: app thread 단일 접근. */

	struct spdk_jsonrpc_request *request;
	/* [한국어] JSON-RPC 응답을 보낼 요청 컨텍스트. remaining이 0이 되는 콜백에서 최종 응답 전송.
	 * 설정자: rpc_bdev_raid_create에서 핸들러 인자로 받아 보관.
	 * 읽는 자: rpc_bdev_raid_create_add_base_bdev_cb에서 응답 전송.
	 * 동기화: app thread 단일 접근. */

	uint8_t remaining;
	/* [한국어] 아직 완료 콜백을 받지 못한 base bdev 추가 요청 수.
	 * 초기값 = num_base_bdevs. 콜백마다 1씩 감소. 0이 되면 사용자에게 응답.
	 * 설정자: rpc_bdev_raid_create에서 num_base_bdevs로 초기화.
	 * 읽는 자: rpc_bdev_raid_create_add_base_bdev_cb.
	 * 값 범위: 0 ~ RPC_MAX_BASE_BDEVS(255).
	 * 동기화: app thread 직렬 실행이므로 atomic 불요. */

	int status;
	/* [한국어] 모든 base bdev 추가 중 하나라도 실패하면 그 errno를 보관.
	 * 마지막 실패값이 유지됨 (여러 실패 시 덮어씀).
	 * 설정자: rpc_bdev_raid_create_add_base_bdev_cb에서 status != 0일 때 저장.
	 * 읽는 자: 마지막 콜백에서 응답을 보낼 때 성공/실패 판단.
	 * 값 범위: 0(성공) 또는 음수 errno. */
};

/*
 * [한국어]
 * free_rpc_bdev_raid_create_ctx - bdev_raid_create 컨텍스트와 내부 동적 자원 해제.
 *
 * @ctx: 해제 대상 컨텍스트. NULL이면 즉시 반환.
 *
 * req.name과 req.base_bdevs.base_bdevs[] 내 각 문자열은 spdk_json_decode_string이 strdup으로
 * 할당했으므로 각각 free해야 한다. 마지막으로 ctx 구조체 자체도 free.
 *
 * 호출 체인:
 *   rpc_bdev_raid_create (에러 경로) → [본 함수]
 *   rpc_bdev_raid_create_add_base_bdev_cb (최종 콜백) → [본 함수]
 */
static void
free_rpc_bdev_raid_create_ctx(struct rpc_bdev_raid_create_ctx *ctx)
{
	struct rpc_bdev_raid_create *req;
	size_t i;

	if (!ctx) {
		/* [한국어] NULL 포인터 방어. 중간 에러 경로에서 ctx가 아직 할당 안 됐을 때. */
		return;
	}

	/* [한국어] 파라미터 구조체 참조. */
	req = &ctx->req;

	/* [한국어] RAID bdev 이름 문자열 해제 (spdk_json_decode_string의 strdup). */
	free(req->name);
	/* [한국어] base bdev 이름 배열 내 각 문자열 해제. num_base_bdevs 이후는 NULL이므로
	 * free(NULL)이 발생해도 안전. */
	for (i = 0; i < req->base_bdevs.num_base_bdevs; i++) {
		free(req->base_bdevs.base_bdevs[i]);
	}

	/* [한국어] 컨텍스트 구조체 자체 해제. */
	free(ctx);
}

/*
 * [한국어]
 * rpc_bdev_raid_create_add_base_bdev_cb - base bdev 추가 완료 콜백.
 *
 * @_ctx  : rpc_bdev_raid_create_ctx 포인터 (void*).
 * @status: 이 base bdev 추가 결과. 0=성공, 음수=실패.
 * @return: 없음.
 *
 * rpc_bdev_raid_create가 각 base bdev마다 raid_bdev_add_base_bdev(cb=본 함수)를 호출하고,
 * 이 콜백이 호출될 때마다 ctx->remaining을 감소. 실패 시 ctx->status에 errno 보관.
 * remaining이 0이 되는 순간(=마지막 콜백):
 *  - status != 0이면 raid_bdev_delete로 생성된 RAID를 롤백 후 에러 응답.
 *  - status == 0이면 true 성공 응답.
 * 어느 경우든 ctx는 free_rpc_bdev_raid_create_ctx로 해제.
 *
 * 실행 컨텍스트: app thread (RAID 코어가 add_base_bdev 비동기 완료 후 app thread에서 콜백).
 *
 * 호출 체인:
 *   raid_bdev_add_base_bdev → ... → [본 콜백] → (success) spdk_jsonrpc_send_bool_response
 *                                              → (fail) raid_bdev_delete → 에러 응답
 */
static void
rpc_bdev_raid_create_add_base_bdev_cb(void *_ctx, int status)
{
	/* [한국어] void* → 비동기 컨텍스트 복원. */
	struct rpc_bdev_raid_create_ctx *ctx = _ctx;

	if (status != 0) {
		/* [한국어] 이 base bdev 추가가 실패. errno를 ctx에 보관(마지막 실패값이 남음). */
		ctx->status = status;
	}

	/* [한국어] remaining이 이미 0이면 논리 오류 (콜백이 예상보다 많이 호출된 것). */
	assert(ctx->remaining != 0);
	if (--ctx->remaining > 0) {
		/* [한국어] 아직 완료 안 된 child가 있으면 대기. */
		return;
	}

	if (ctx->status != 0) {
		/* [한국어] 하나 이상의 base bdev 추가 실패 → 생성한 RAID 롤백.
		 * raid_bdev_delete는 비동기이나 본 경로에서 cb=NULL이면 내부에서 완료 처리. */
		raid_bdev_delete(ctx->raid_bdev, NULL, NULL);
		/* [한국어] 에러 응답. ctx->req.name과 ctx->status를 사용. spdk_strerror는 음수 errno를 처리. */
		spdk_jsonrpc_send_error_response_fmt(ctx->request, ctx->status,
						     "Failed to create RAID bdev %s: %s",
						     ctx->req.name,
						     spdk_strerror(-ctx->status));
	} else {
		/* [한국어] 모든 base bdev 추가 성공 → 성공 응답 (JSON boolean true). */
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	}

	/* [한국어] 컨텍스트와 동적 자원 해제. */
	free_rpc_bdev_raid_create_ctx(ctx);
}

/*
 * brief:
 * rpc_bdev_raid_create function is the RPC for creating RAID bdevs. It takes
 * input as raid bdev name, raid level, strip size in KB and list of base bdev names.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
/*
 * [한국어]
 * rpc_bdev_raid_create - RAID bdev 생성 RPC 핸들러.
 *
 * @request: JSON-RPC 응답 컨텍스트.
 * @params : JSON 파라미터 포인터 (name, strip_size_kb, raid_level, base_bdevs, uuid, superblock).
 * @return : 없음. 결과는 rpc_bdev_raid_create_add_base_bdev_cb에서 비동기 응답.
 *
 * 동기/배경: 사용자가 새 RAID 인스턴스를 생성하고자 할 때 호출. 내부적으로 두 단계로 분리:
 *  1) raid_bdev_create: RAID 메타데이터 구조를 할당하고 base_bdev_info[] 슬롯 초기화.
 *  2) raid_bdev_add_base_bdev (반복): 각 base bdev을 비동기로 RAID에 연결.
 *
 * 비동기 집계: ctx->remaining = num_base_bdevs로 초기화되고, 각 add 콜백마다 1씩 감소.
 * remaining이 0이 될 때 rpc_bdev_raid_create_add_base_bdev_cb에서 최종 응답.
 *
 * 에러 처리:
 *  - 메모리 부족: calloc 실패 → 즉시 에러 응답.
 *  - 파라미터 파싱 실패: PARSE_ERROR 응답 후 ctx free.
 *  - 빈 이름 base bdev: EINVAL 응답 후 ctx free.
 *  - raid_bdev_create 실패: 에러 응답 후 ctx free.
 *  - raid_bdev_add_base_bdev 실패(-ENODEV 제외): remaining 조정 후 에러 응답.
 *  - 비동기 add 실패(status != 0): 콜백에서 RAID 롤백 후 에러 응답.
 *
 * 실행 컨텍스트: app thread. 단, 비동기 add 완료 콜백도 app thread에서 직렬 실행.
 *
 * 호출 체인:
 *   lib/rpc 디스패처 → [본 함수] → raid_bdev_create → raid_bdev_add_base_bdev(×N) →
 *     rpc_bdev_raid_create_add_base_bdev_cb → 응답
 */
static void
rpc_bdev_raid_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	/* [한국어] 파라미터 구조체 참조 (ctx->req의 별칭). */
	struct rpc_bdev_raid_create	*req;
	/* [한국어] 생성된 RAID bdev 포인터. raid_bdev_create 성공 후 설정. */
	struct raid_bdev		*raid_bdev;
	/* [한국어] 각 단계의 반환값. */
	int				rc;
	/* [한국어] 루프 인덱스. */
	size_t				i;
	/* [한국어] 비동기 실행 컨텍스트. 콜백이 완료될 때까지 힙에서 유효. */
	struct rpc_bdev_raid_create_ctx *ctx;
	/* [한국어] num_base_bdevs를 uint8_t로 저장한 지역 변수. loop 상한으로 사용. */
	uint8_t				num_base_bdevs;

	/* [한국어] 비동기 컨텍스트 힙 할당 (calloc = zero-init). */
	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		/* [한국어] OOM: 즉시 에러 응답. ctx 자체가 NULL이므로 cleanup goto 불필요. */
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto cleanup;
	}
	/* [한국어] 파라미터 구조체 포인터 편의 변수. */
	req = &ctx->req;

	if (spdk_json_decode_object(params, rpc_bdev_raid_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_create_decoders),
				    req)) {
		/* [한국어] JSON 파라미터 파싱 실패 → PARSE_ERROR 응답 후 ctx 해제. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}
	/* [한국어] num_base_bdevs를 uint8_t로 저장(RAID 코어 인터페이스 타입 일치). */
	num_base_bdevs = req->base_bdevs.num_base_bdevs;

	/* [한국어] 빈 이름 base bdev 검사. 빈 문자열은 bdev 이름으로 유효하지 않음. */
	for (i = 0; i < num_base_bdevs; i++) {
		if (strlen(req->base_bdevs.base_bdevs[i]) == 0) {
			/* [한국어] 빈 이름 발견 → EINVAL 에러 메시지 포함 응답. */
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
							     "The base bdev name cannot be empty: %s",
							     spdk_strerror(EINVAL));
			goto cleanup;
		}
	}

	/* [한국어] RAID 인스턴스 생성: raid_bdev 구조체와 base_bdev_info[] 슬롯 초기화.
	 * superblock_enabled가 true이면 sb도 할당. uuid가 NULL UUID이면 내부에서 자동 생성. */
	rc = raid_bdev_create(req->name, req->strip_size_kb, num_base_bdevs,
			      req->level, req->superblock_enabled, &req->uuid, &raid_bdev);
	if (rc != 0) {
		/* [한국어] 생성 실패 (중복 이름, 잘못된 레벨 등) → fmt 에러 응답 후 ctx 해제. */
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to create RAID bdev %s: %s",
						     req->name, spdk_strerror(-rc));
		goto cleanup;
	}

	/* [한국어] ctx에 생성된 RAID와 요청 컨텍스트 저장. 비동기 콜백이 사용. */
	ctx->raid_bdev = raid_bdev;
	ctx->request = request;
	/* [한국어] 남은 base bdev 추가 수 초기화. 모든 추가가 완료되어야 응답 전송. */
	ctx->remaining = num_base_bdevs;

	/* [한국어] num_base_bdevs > 0 이어야 remaining 초기화가 의미 있음. 코어가 이미 검증했지만
	 * 한 번 더 단언. */
	assert(num_base_bdevs > 0);

	/* [한국어] 모든 base bdev을 fan-out으로 비동기 추가. */
	for (i = 0; i < num_base_bdevs; i++) {
		const char *base_bdev_name = req->base_bdevs.base_bdevs[i];

		/* [한국어] 비동기 base bdev 추가. 완료 시 rpc_bdev_raid_create_add_base_bdev_cb 호출. */
		rc = raid_bdev_add_base_bdev(raid_bdev, base_bdev_name,
					     rpc_bdev_raid_create_add_base_bdev_cb, ctx);
		if (rc == -ENODEV) {
			/* [한국어] -ENODEV: base bdev이 아직 시스템에 없음 (나중에 examine 경로에서 추가될 수 있음).
			 * 비동기 콜백이 호출되지 않으므로 직접 cb(0)을 호출해 remaining을 차감해야 함.
			 * 단언: remaining > 1이거나 마지막(i+1 == num_base_bdevs) 이어야 remaining이 0이 되지 않음. */
			SPDK_DEBUGLOG(bdev_raid, "base bdev %s doesn't exist now\n", base_bdev_name);
			assert(ctx->remaining > 1 || i + 1 == num_base_bdevs);
			/* [한국어] "성공"으로 처리하여 remaining만 감소 (나중에 examine 시 추가 시도). */
			rpc_bdev_raid_create_add_base_bdev_cb(ctx, 0);
		} else if (rc != 0) {
			/* [한국어] -ENODEV 외 에러(예: 슬롯 없음, 잘못된 bdev 등) → remaining을 조정해
			 * 남은 base bdev들을 건너뛰고(콜백 없음) 마지막 콜백으로 에러 응답. */
			SPDK_DEBUGLOG(bdev_raid, "Failed to add base bdev %s to RAID bdev %s: %s",
				      base_bdev_name, req->name, spdk_strerror(-rc));
			/* [한국어] 아직 처리 안 한 base bdev들의 remaining 일괄 차감. (num - i - 1)개는
			 * 더 이상 add 시도 안 함. */
			ctx->remaining -= (num_base_bdevs - i - 1);
			/* [한국어] 이 add의 에러 status로 cb 직접 호출 → remaining이 0이 되면 에러 응답. */
			rpc_bdev_raid_create_add_base_bdev_cb(ctx, rc);
			break;
		}
	}
	/* [한국어] 비동기 연산이 진행 중이므로 함수 반환. ctx는 콜백에서 해제. */
	return;
cleanup:
	/* [한국어] 에러 경로: 파라미터 파싱 실패, OOM, 이름 오류, create 실패 등의 경우 ctx 해제. */
	free_rpc_bdev_raid_create_ctx(ctx);
}
/* [한국어] "bdev_raid_create" 메서드를 RUNTIME 단계에 등록. */
SPDK_RPC_REGISTER("bdev_raid_create", rpc_bdev_raid_create, SPDK_RPC_RUNTIME)

/*
 * Input structure for RPC deleting a raid bdev
 */
/*
 * [한국어]
 * struct rpc_bdev_raid_delete - bdev_raid_delete RPC의 입력 파라미터 구조체.
 */
struct rpc_bdev_raid_delete {
	/* raid bdev name */
	char *name;
	/* [한국어] 삭제할 RAID bdev 이름. spdk_json_decode_string이 strdup으로 heap 할당.
	 * free_rpc_bdev_raid_delete에서 해제.
	 * 설정자: spdk_json_decode_object → rpc_bdev_raid_delete_decoders.
	 * 읽는 자: rpc_bdev_raid_delete에서 raid_bdev_find_by_name 인자로 사용.
	 * 동기화: app thread 단일 접근. */
};

/*
 * brief:
 * free_rpc_bdev_raid_delete function is used to free RPC bdev_raid_delete related parameters
 * params:
 * req - pointer to RPC request
 * params:
 * none
 */
/*
 * [한국어]
 * free_rpc_bdev_raid_delete - rpc_bdev_raid_delete 구조체 내부 동적 자원 해제.
 *
 * @req: 해제 대상. 구조체 자체는 rpc_bdev_raid_delete_ctx 안에 임베드되어 있으므로 free 하지 않음.
 *
 * 호출 체인:
 *   bdev_raid_delete_done (최종 콜백) → [본 함수]
 *   rpc_bdev_raid_delete (에러 경로) → [본 함수]
 */
static void
free_rpc_bdev_raid_delete(struct rpc_bdev_raid_delete *req)
{
	/* [한국어] name 문자열 heap 해제. NULL이어도 안전. */
	free(req->name);
}

/*
 * Decoder object for RPC raid_bdev_delete
 */
/* [한국어] rpc_bdev_raid_delete_decoders - bdev_raid_delete RPC의 JSON 파라미터 디코더 테이블.
 * "name" 하나만 필수 파라미터로 받음. */
static const struct spdk_json_object_decoder rpc_bdev_raid_delete_decoders[] = {
	/* [한국어] "name" 키 → req.name 필드로 string 디코딩. */
	{"name", offsetof(struct rpc_bdev_raid_delete, name), spdk_json_decode_string},
};

/*
 * [한국어]
 * struct rpc_bdev_raid_delete_ctx - bdev_raid_delete RPC의 비동기 실행 컨텍스트.
 *
 * raid_bdev_delete가 비동기이므로, 완료 콜백(bdev_raid_delete_done)에서 JSON 응답을 보내기 위해
 * request 포인터와 req(이름 문자열)을 힙에 보관한다.
 */
struct rpc_bdev_raid_delete_ctx {
	struct rpc_bdev_raid_delete req;
	/* [한국어] 삭제할 RAID 이름. 에러 메시지에서 name을 참조하기 위해 보관.
	 * 동기화: app thread 단일 접근. */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 완료 콜백에서 응답을 전송할 요청 컨텍스트. NULL이면 안 됨. */
};

/*
 * brief:
 * params:
 * cb_arg - pointer to the callback context.
 * rc - return code of the deletion of the raid bdev.
 * returns:
 * none
 */
/*
 * [한국어]
 * bdev_raid_delete_done - RAID bdev 삭제 완료 콜백.
 *
 * @cb_arg: rpc_bdev_raid_delete_ctx 포인터.
 * @rc    : 삭제 결과. 0=성공, 음수=실패 errno.
 * @return: 없음.
 *
 * 삭제 성공 시 JSON true 응답, 실패 시 에러 응답 전송. 어느 경우든 ctx와 내부 자원 해제.
 *
 * 실행 컨텍스트: app thread (raid_bdev_delete의 비동기 완료 후).
 *
 * 호출 체인:
 *   raid_bdev_delete → [본 콜백] → spdk_jsonrpc_send_bool_response / send_error_response
 */
static void
bdev_raid_delete_done(void *cb_arg, int rc)
{
	/* [한국어] ctx 포인터 복원. */
	struct rpc_bdev_raid_delete_ctx *ctx = cb_arg;
	/* [한국어] 응답 전송에 사용. 로컬 변수로 복사 후 ctx->request를 참조 안전하게 사용. */
	struct spdk_jsonrpc_request *request = ctx->request;

	if (rc != 0) {
		/* [한국어] 삭제 실패: 로그 출력 후 INTERNAL_ERROR 에러 응답. */
		SPDK_ERRLOG("Failed to delete raid bdev %s (%d): %s\n",
			    ctx->req.name, rc, spdk_strerror(-rc));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto exit;
	}

	/* [한국어] 삭제 성공 → true 응답. */
	spdk_jsonrpc_send_bool_response(request, true);
exit:
	/* [한국어] 파라미터 내부 자원 해제 후 ctx free. */
	free_rpc_bdev_raid_delete(&ctx->req);
	free(ctx);
}

/*
 * brief:
 * rpc_bdev_raid_delete function is the RPC for deleting a raid bdev. It takes raid
 * name as input and delete that raid bdev including freeing the base bdev
 * resources.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
/*
 * [한국어]
 * rpc_bdev_raid_delete - RAID bdev 삭제 RPC 핸들러.
 *
 * @request: JSON-RPC 응답 컨텍스트.
 * @params : {"name": "<raid_bdev_name>"} 형태의 JSON 파라미터.
 * @return : 없음. 결과는 bdev_raid_delete_done 콜백에서 비동기 응답.
 *
 * 동작:
 *  1) ctx calloc, 파라미터 파싱.
 *  2) raid_bdev_find_by_name으로 RAID bdev 검색.
 *  3) raid_bdev_delete 비동기 호출. 완료 시 bdev_raid_delete_done.
 *
 * 에러 경로:
 *  - OOM: 즉시 에러 응답, return.
 *  - 파싱 실패: PARSE_ERROR 후 ctx 해제.
 *  - 찾을 수 없음: ENODEV 에러 응답 후 ctx 해제.
 *
 * 호출 체인:
 *   lib/rpc → [본 함수] → raid_bdev_delete → bdev_raid_delete_done → 응답
 */
static void
rpc_bdev_raid_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	/* [한국어] 비동기 컨텍스트. calloc으로 zero-init. */
	struct rpc_bdev_raid_delete_ctx *ctx;
	/* [한국어] 검색된 RAID bdev 포인터. */
	struct raid_bdev *raid_bdev;

	/* [한국어] 컨텍스트 할당. */
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		/* [한국어] OOM: 에러 응답 후 즉시 반환. ctx 해제 불필요(할당 실패). */
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_raid_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_delete_decoders),
				    &ctx->req)) {
		/* [한국어] 파싱 실패 → PARSE_ERROR 에러 응답 후 cleanup. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 이름으로 전역 RAID bdev 목록에서 검색. */
	raid_bdev = raid_bdev_find_by_name(ctx->req.name);
	if (raid_bdev == NULL) {
		/* [한국어] 존재하지 않는 이름 → ENODEV 에러 응답. */
		spdk_jsonrpc_send_error_response_fmt(request, -ENODEV,
						     "raid bdev %s not found",
						     ctx->req.name);
		goto cleanup;
	}

	/* [한국어] 응답을 보낼 request 컨텍스트 보관. */
	ctx->request = request;

	/* [한국어] 비동기 삭제 시작. 완료 시 bdev_raid_delete_done(ctx) 호출. */
	raid_bdev_delete(raid_bdev, bdev_raid_delete_done, ctx);

	/* [한국어] 비동기 진행 중 → 함수 반환. ctx는 콜백에서 해제. */
	return;

cleanup:
	/* [한국어] 에러 경로: 파라미터 내부 자원과 ctx 해제. */
	free_rpc_bdev_raid_delete(&ctx->req);
	free(ctx);
}
/* [한국어] "bdev_raid_delete" 메서드를 RUNTIME 단계에 등록. */
SPDK_RPC_REGISTER("bdev_raid_delete", rpc_bdev_raid_delete, SPDK_RPC_RUNTIME)

/*
 * Base bdevs in RPC bdev_raid_add_base_bdev
 */
/*
 * [한국어]
 * struct rpc_bdev_raid_add_base_bdev - bdev_raid_add_base_bdev RPC의 입력 파라미터 구조체.
 *
 * 기존 RAID bdev에 새로운 base bdev를 동적으로 추가하기 위한 파라미터. 두 이름(base bdev,
 * raid bdev)을 JSON에서 파싱하여 보관. 사용 후 free_rpc_bdev_raid_add_base_bdev로 해제.
 */
struct rpc_bdev_raid_add_base_bdev {
	/* Base bdev name */
	char			*base_bdev;
	/* [한국어] 추가할 base bdev의 이름 (heap 할당 문자열).
	 * 설정자: spdk_json_decode_string. 읽는 자: raid_bdev_add_base_bdev 인자.
	 * 동기화: app thread 단일 접근. */

	/* Raid bdev name */
	char			*raid_bdev;
	/* [한국어] 대상 RAID bdev의 이름 (heap 할당 문자열). raid_bdev_find_by_name에 전달.
	 * 설정자: spdk_json_decode_string. 읽는 자: rpc_bdev_raid_add_base_bdev에서 검색.
	 * 동기화: app thread 단일 접근. */
};

/*
 * brief:
 * free_rpc_bdev_raid_add_base_bdev function is to free RPC
 * bdev_raid_add_base_bdev related parameters.
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
/*
 * [한국어]
 * free_rpc_bdev_raid_add_base_bdev - rpc_bdev_raid_add_base_bdev 구조체 내부 자원 해제.
 *
 * @req: 해제 대상. base_bdev, raid_bdev 두 문자열을 free.
 *
 * 호출 체인:
 *   rpc_bdev_raid_add_base_bdev (cleanup 레이블) → [본 함수]
 */
static void
free_rpc_bdev_raid_add_base_bdev(struct rpc_bdev_raid_add_base_bdev *req)
{
	/* [한국어] base bdev 이름 문자열 해제. */
	free(req->base_bdev);
	/* [한국어] RAID bdev 이름 문자열 해제. */
	free(req->raid_bdev);
}

/*
 * Decoder object for RPC bdev_raid_add_base_bdev
 */
/* [한국어] rpc_bdev_raid_add_base_bdev_decoders - bdev_raid_add_base_bdev RPC의 JSON 파라미터
 * 디코더 테이블. "base_bdev"와 "raid_bdev" 두 필수 키를 디코딩. */
static const struct spdk_json_object_decoder rpc_bdev_raid_add_base_bdev_decoders[] = {
	/* [한국어] "base_bdev" 키 → 추가할 base bdev 이름. */
	{"base_bdev", offsetof(struct rpc_bdev_raid_add_base_bdev, base_bdev), spdk_json_decode_string},
	/* [한국어] "raid_bdev" 키 → 대상 RAID bdev 이름. */
	{"raid_bdev", offsetof(struct rpc_bdev_raid_add_base_bdev, raid_bdev), spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_raid_add_base_bdev_done - base bdev 추가 완료 콜백.
 *
 * @ctx   : spdk_jsonrpc_request 포인터 (void*로 전달).
 * @status: 추가 결과. 0=성공, 음수=실패.
 * @return: 없음.
 *
 * 성공 시 true 응답, 실패 시 에러 메시지 응답. request 컨텍스트 자체가 ctx로 전달되어
 * 별도 ctx 구조체가 필요 없다 (간소화 패턴).
 *
 * 실행 컨텍스트: app thread (raid_bdev_add_base_bdev 비동기 완료 후).
 *
 * 호출 체인:
 *   raid_bdev_add_base_bdev → [본 콜백] → spdk_jsonrpc_send_bool_response / send_error_response
 */
static void
rpc_bdev_raid_add_base_bdev_done(void *ctx, int status)
{
	/* [한국어] ctx는 곧바로 spdk_jsonrpc_request* 이다 (콜백 등록 시 request를 cb_arg로 전달). */
	struct spdk_jsonrpc_request *request = ctx;

	if (status != 0) {
		/* [한국어] 추가 실패 → 에러 코드와 메시지를 포함한 응답. */
		spdk_jsonrpc_send_error_response_fmt(request, status, "Failed to add base bdev to RAID bdev: %s",
						     spdk_strerror(-status));
		return;
	}

	/* [한국어] 추가 성공 → true 응답. */
	spdk_jsonrpc_send_bool_response(request, true);
}

/*
 * [한국어]
 * rpc_bdev_raid_event_cb - RAID bdev 이벤트 콜백 (빈 함수).
 *
 * @type     : 이벤트 타입 (REMOVE 등).
 * @bdev     : 이벤트가 발생한 bdev.
 * @event_ctx: 이벤트 컨텍스트.
 * @return   : 없음.
 *
 * rpc_bdev_raid_remove_base_bdev에서 spdk_bdev_open_ext의 이벤트 콜백으로 등록되는 더미
 * 함수. 임시 open 용도이므로 bdev 이벤트(예: REMOVE 알림)를 처리할 필요가 없다.
 * RPC가 즉시 spdk_bdev_close를 호출하므로 실질적으로 이 콜백이 호출될 일이 없다.
 */
static void
rpc_bdev_raid_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	/* [한국어] 임시 desc를 열기만 하고 곧바로 닫으므로 이벤트 처리 불필요. 빈 함수. */
}

/*
 * brief:
 * bdev_raid_add_base_bdev function is the RPC for adding base bdev to a raid bdev.
 * It takes base bdev and raid bdev names as input.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
/*
 * [한국어]
 * rpc_bdev_raid_add_base_bdev - 기존 RAID bdev에 base bdev 추가 RPC 핸들러.
 *
 * @request: JSON-RPC 응답 컨텍스트.
 * @params : {"base_bdev": "...", "raid_bdev": "..."} 형태의 JSON 파라미터.
 * @return : 없음. 결과는 rpc_bdev_raid_add_base_bdev_done에서 비동기 응답.
 *
 * 동기/배경: RAID 1처럼 동적으로 mirroring 디스크를 추가하거나, RAID 0 degraded 상태에서
 * 슬롯을 채우기 위해 사용. raid_bdev_add_base_bdev는 비동기이며 완료 시 done 콜백 호출.
 *
 * 에러 경로:
 *  - 파싱 실패: INTERNAL_ERROR 응답 후 req 해제.
 *  - RAID bdev 검색 실패: ENODEV 에러 응답.
 *  - raid_bdev_add_base_bdev 동기 실패: 에러 응답.
 *
 * 호출 체인:
 *   lib/rpc → [본 함수] → raid_bdev_add_base_bdev → rpc_bdev_raid_add_base_bdev_done → 응답
 */
static void
rpc_bdev_raid_add_base_bdev(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	/* [한국어] 스택 파라미터 구조체 (= {} 로 zero-init). */
	struct rpc_bdev_raid_add_base_bdev req = {};
	/* [한국어] 검색된 RAID bdev 포인터. */
	struct raid_bdev *raid_bdev;
	/* [한국어] 발행 반환값. */
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_raid_add_base_bdev_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_add_base_bdev_decoders),
				    &req)) {
		/* [한국어] 파싱 실패 → INTERNAL_ERROR 응답. (create와 달리 PARSE_ERROR가 아닌 점 주의:
		 * SPDK_JSONRPC_ERROR_INTERNAL_ERROR는 서버 내부 오류 에러 코드이며, 여기선 코드 일관성.) */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* [한국어] 이름으로 RAID bdev 검색. */
	raid_bdev = raid_bdev_find_by_name(req.raid_bdev);
	if (raid_bdev == NULL) {
		/* [한국어] 존재하지 않는 RAID → ENODEV 에러 응답. */
		spdk_jsonrpc_send_error_response_fmt(request, -ENODEV, "raid bdev %s is not found in config",
						     req.raid_bdev);
		goto cleanup;
	}

	/* [한국어] 비동기 base bdev 추가 시작. 완료 시 rpc_bdev_raid_add_base_bdev_done(request).
	 * cb_arg로 request 포인터를 직접 전달(별도 ctx 없음). */
	rc = raid_bdev_add_base_bdev(raid_bdev, req.base_bdev, rpc_bdev_raid_add_base_bdev_done, request);
	if (rc != 0) {
		/* [한국어] 동기 실패(슬롯 없음, 이미 존재 등) → 즉시 에러 응답. */
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to add base bdev %s to RAID bdev %s: %s",
						     req.base_bdev, req.raid_bdev,
						     spdk_strerror(-rc));
		goto cleanup;
	}

cleanup:
	/* [한국어] 성공/실패 모두 파라미터 구조체 내부 자원 해제. 비동기 성공 경로는 done 콜백에서
	 * request를 사용하므로 request는 여기서 사용하지 않는다. */
	free_rpc_bdev_raid_add_base_bdev(&req);
}
/* [한국어] "bdev_raid_add_base_bdev" 메서드를 RUNTIME 단계에 등록. */
SPDK_RPC_REGISTER("bdev_raid_add_base_bdev", rpc_bdev_raid_add_base_bdev, SPDK_RPC_RUNTIME)

/*
 * Decoder object for RPC bdev_raid_remove_base_bdev
 */
/* [한국어] rpc_bdev_raid_remove_base_bdev_decoders - bdev_raid_remove_base_bdev RPC의 JSON
 * 파라미터 디코더 테이블. "name" 하나만 받으며, offsetof 0은 디코딩 결과가 char* 타입 변수
 * 자체의 포인터로 전달된다는 의미 (out 포인터가 곧 char**). */
static const struct spdk_json_object_decoder rpc_bdev_raid_remove_base_bdev_decoders[] = {
	/* [한국어] "name" 키 → offset 0으로 지정. spdk_json_decode_object에서 out+0 = 출력 포인터.
	 * 실제로 rpc_bdev_raid_remove_base_bdev에서 &name (char**) 을 decode_object에 전달하므로
	 * offsetof=0이 올바른 매핑이다. */
	{"name", 0, spdk_json_decode_string},
};

/*
 * [한국어]
 * rpc_bdev_raid_remove_base_bdev_done - base bdev 제거 완료 콜백.
 *
 * @ctx   : spdk_jsonrpc_request 포인터 (void*).
 * @status: 제거 결과. 0=성공, 음수=실패.
 * @return: 없음.
 *
 * add_base_bdev_done과 동일한 패턴으로, request를 cb_arg로 직접 전달한다.
 * 성공 시 true 응답, 실패 시 에러 응답.
 *
 * 호출 체인:
 *   raid_bdev_remove_base_bdev → [본 콜백] → spdk_jsonrpc_send_bool_response / send_error_response
 */
static void
rpc_bdev_raid_remove_base_bdev_done(void *ctx, int status)
{
	/* [한국어] ctx는 spdk_jsonrpc_request* 이다. */
	struct spdk_jsonrpc_request *request = ctx;

	if (status != 0) {
		/* [한국어] 제거 실패 에러 응답. 에러 메시지에 이름은 포함하지 않음 (base bdev 이름이
		 * 이미 close 됐으므로 참조가 불가능한 경우도 있음). */
		spdk_jsonrpc_send_error_response_fmt(request, status, "Failed to remove base bdev from raid bdev");
		return;
	}

	/* [한국어] 제거 성공 → true 응답. */
	spdk_jsonrpc_send_bool_response(request, true);
}

/*
 * brief:
 * bdev_raid_remove_base_bdev function is the RPC for removing base bdev from a raid bdev.
 * It takes base bdev name as input.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
/*
 * [한국어]
 * rpc_bdev_raid_remove_base_bdev - 기존 RAID bdev에서 base bdev 제거 RPC 핸들러.
 *
 * @request: JSON-RPC 응답 컨텍스트.
 * @params : {"name": "<base_bdev_name>"} 형태의 JSON 파라미터. base bdev 이름을 받음.
 * @return : 없음. 결과는 rpc_bdev_raid_remove_base_bdev_done에서 비동기 응답.
 *
 * 동기/배경: RAID에서 특정 base bdev을 동적으로 제거(detach). RAID 1의 경우 한 디스크가
 * 고장났거나 교체하려 할 때 사용. 제거 후 RAID는 degraded mode로 계속 동작.
 *
 * 특이 사항: add와 달리 제거 대상은 "base bdev 이름"만 받으며, 어느 RAID에 속하는지는
 * RAID 코어가 내부 목록에서 자동 검색. 이를 위해:
 *  1) spdk_bdev_open_ext로 base bdev를 임시 open해 spdk_bdev* 포인터를 얻음.
 *  2) raid_bdev_remove_base_bdev에 spdk_bdev*를 전달.
 *  3) 즉시 spdk_bdev_close로 임시 desc를 닫음.
 *
 * 에러 경로:
 *  - 파싱 실패: PARSE_ERROR 응답 후 name 해제.
 *  - bdev open 실패: err 레이블로 점프해 done(rc) 호출.
 *  - raid_bdev_remove_base_bdev 동기 실패: err 레이블 → done(rc) 호출.
 *
 * 호출 체인:
 *   lib/rpc → [본 함수] → spdk_bdev_open_ext → raid_bdev_remove_base_bdev →
 *     rpc_bdev_raid_remove_base_bdev_done → 응답
 */
static void
rpc_bdev_raid_remove_base_bdev(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	/* [한국어] 임시 base bdev descriptor. 이름으로 bdev를 open해 포인터를 얻기 위해 사용.
	 * 사용 후 즉시 닫는다(임시 참조 목적). */
	struct spdk_bdev_desc *desc;
	/* [한국어] base bdev 이름. = {} 로 NULL 초기화. decode_object에 &name을 전달해
	 * char* 자체에 직접 decode 결과가 들어옴 (offsetof=0 디코더와 매칭). */
	char *name = NULL;
	/* [한국어] 각 단계의 반환값. */
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_raid_remove_base_bdev_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_remove_base_bdev_decoders),
				    &name)) {
		/* [한국어] 파싱 실패 → PARSE_ERROR 응답 후 즉시 return. name은 NULL 또는 부분 할당.
		 * name이 decode 중 할당됐을 수 있으므로 free(name)을 호출해야 하지만, 이 경로에서
		 * name이 NULL이면 free(NULL)은 안전하므로 에러 경로 처리가 단순화된다. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	/* [한국어] base bdev 이름으로 임시 open. 이벤트 콜백은 빈 함수(rpc_bdev_raid_event_cb).
	 * 성공 시 desc에 유효한 descriptor 설정. 실패 시 rc != 0. */
	rc = spdk_bdev_open_ext(name, false, rpc_bdev_raid_event_cb, NULL, &desc);
	/* [한국어] 이름 문자열은 더 이상 필요 없으므로 즉시 해제. open 성공/실패 무관. */
	free(name);
	if (rc != 0) {
		/* [한국어] bdev를 열 수 없음(존재하지 않거나 다른 소유자가 독점 lock 보유 등). */
		goto err;
	}

	/* [한국어] 임시 desc에서 spdk_bdev* 추출 후 RAID에서 제거 요청.
	 * raid_bdev_remove_base_bdev는 bdev 포인터로 어느 RAID에 속하는지 내부 검색. */
	rc = raid_bdev_remove_base_bdev(spdk_bdev_desc_get_bdev(desc), rpc_bdev_raid_remove_base_bdev_done,
					request);
	/* [한국어] 임시 desc 즉시 닫기. 이 시점 이후로는 desc를 사용하지 않는다.
	 * raid_bdev_remove_base_bdev는 bdev 포인터를 보관하므로 desc close는 안전. */
	spdk_bdev_close(desc);
	if (rc != 0) {
		/* [한국어] 동기 실패(이 bdev가 RAID에 속하지 않음 등) → err 경로. */
		goto err;
	}

	/* [한국어] 비동기 제거 시작 성공 → 함수 반환. 콜백에서 응답. */
	return;
err:
	/* [한국어] 에러 경로: done 콜백을 직접 호출해 에러 응답을 전송. ctx=request. */
	rpc_bdev_raid_remove_base_bdev_done(request, rc);
}
/* [한국어] "bdev_raid_remove_base_bdev" 메서드를 RUNTIME 단계에 등록. */
SPDK_RPC_REGISTER("bdev_raid_remove_base_bdev", rpc_bdev_raid_remove_base_bdev, SPDK_RPC_RUNTIME)

/* [한국어] rpc_bdev_raid_set_options_decoders - bdev_raid_set_options RPC의 JSON 파라미터
 * 디코더 테이블. spdk_raid_bdev_opts 구조체를 직접 대상으로 삼아 옵션 필드를 채운다.
 * 두 옵션 모두 선택(optional=true). */
static const struct spdk_json_object_decoder rpc_bdev_raid_set_options_decoders[] = {
	/* [한국어] "process_window_size_kb" → 백그라운드 처리(리빌드/스크럽) 창 크기(KB). 선택. */
	{"process_window_size_kb", offsetof(struct spdk_raid_bdev_opts, process_window_size_kb), spdk_json_decode_uint32, true},
	/* [한국어] "process_max_bandwidth_mb_sec" → 백그라운드 처리 최대 대역폭(MB/s). 선택. 0이면 무제한. */
	{"process_max_bandwidth_mb_sec", offsetof(struct spdk_raid_bdev_opts, process_max_bandwidth_mb_sec), spdk_json_decode_uint32, true},
};

/*
 * [한국어]
 * rpc_bdev_raid_set_options - RAID 전역 옵션 설정 RPC 핸들러.
 *
 * @request: JSON-RPC 응답 컨텍스트.
 * @params : 선택적 JSON 파라미터 객체. 생략 가능 (파라미터 없으면 현재 값 그대로 set).
 * @return : 없음. 동기 응답.
 *
 * 동기/배경: 리빌드/스크럽의 창 크기와 최대 대역폭을 런타임에 조정. 현재 값을 먼저
 * raid_bdev_get_opts로 읽어 기본값 보존 후, JSON 파라미터가 있는 항목만 덮어씀.
 *
 * STARTUP | RUNTIME: 두 단계 모두 허용. 초기화 시점과 런타임 모두에서 설정 가능.
 *
 * 동작:
 *  1) 현재 전역 opts 조회 (raid_bdev_get_opts).
 *  2) JSON 파라미터 있으면 decode_object로 opts 일부 갱신.
 *  3) raid_bdev_set_opts로 새 opts 적용.
 *  4) 성공 시 true, 실패 시 에러 응답.
 *
 * 호출 체인:
 *   lib/rpc → [본 함수] → raid_bdev_set_opts → spdk_jsonrpc_send_bool_response
 */
static void
rpc_bdev_raid_set_options(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	/* [한국어] RAID 전역 옵션 구조체. get_opts로 현재 값을 먼저 읽어 채움. */
	struct spdk_raid_bdev_opts opts;
	/* [한국어] raid_bdev_set_opts 반환값. */
	int rc;

	/* [한국어] 현재 전역 옵션 읽기. 이후 JSON으로 override될 항목만 변경되고 나머지는 유지. */
	raid_bdev_get_opts(&opts);
	if (params && spdk_json_decode_object(params, rpc_bdev_raid_set_options_decoders,
					      SPDK_COUNTOF(rpc_bdev_raid_set_options_decoders),
					      &opts)) {
		/* [한국어] params가 있는데 파싱 실패 → PARSE_ERROR 응답 후 return. */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	/* [한국어] 새 opts 적용. 유효성 검사(예: window_size가 0 초과) 실패 시 rc != 0. */
	rc = raid_bdev_set_opts(&opts);
	if (rc) {
		/* [한국어] 유효하지 않은 옵션 값 → errno 코드와 설명 에러 응답. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	} else {
		/* [한국어] 옵션 적용 성공 → true 응답. */
		spdk_jsonrpc_send_bool_response(request, true);
	}

	/* [한국어] 동기 함수이므로 여기서 자원 정리 없이 반환. opts는 스택 변수. */
	return;
}
/* [한국어] "bdev_raid_set_options" 메서드를 STARTUP과 RUNTIME 두 단계 모두에 등록.
 * STARTUP|RUNTIME: 초기화 단계(STARTUP)에서도 런타임(RUNTIME)에서도 호출 가능. */
SPDK_RPC_REGISTER("bdev_raid_set_options", rpc_bdev_raid_set_options,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
