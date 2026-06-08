/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] Logical Volume bdev JSON-RPC 핸들러 모음 (vbdev_lvol_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK lvol/lvol store 관련 모든 JSON-RPC 2.0 메서드를 구현한다.
 * 사용자가 `spdk_rpc.py` 혹은 `rpc.py` 클라이언트를 통해 lvol 을 생성·삭제·크기조정·
 * 스냅샷·클론·이름변경·얕은복사(shallow copy) 등 모든 제어 명령을 보내면, 이 파일의
 * 핸들러 함수들이 그 요청을 수신·파싱하여 vbdev_lvol.c 의 공개 API 로 위임한다.
 * JSON 입력 파라미터 디코딩, 비동기 결과 콜백 등록, JSON 응답 직렬화, 에러 처리까지
 * 전체 RPC 레이어를 담당하며, 실제 lvol 로직은 vbdev_lvol.c / lib/lvol 이 처리한다.
 * 각 RPC 핸들러는 SPDK_RPC_REGISTER 매크로로 SPDK_RPC_RUNTIME 단계에 자동 등록된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 관리 평면(management plane)의 JSON-RPC 레이어에 위치한다.
 * 실행 흐름: JSON-RPC 클라이언트 → lib/jsonrpc (서버 소켓/파싱) → lib/rpc (메서드 디스패치) →
 *   [이 파일의 핸들러] → vbdev_lvol.c 공개 API → lib/lvol → lib/blob → 베이스 bdev.
 * 실행 컨텍스트: SPDK 첫 번째 reactor 스레드 (=메인 스레드). 모든 핸들러가 단일 스레드에서
 * 실행되므로 핸들러 내부에는 별도 락이 필요 없다. 비동기 작업의 완료는 콜백으로 처리되며,
 * 그 콜백도 동일한 스레드에서 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * (a) lib/rpc: SPDK_RPC_REGISTER 매크로 — 각 핸들러를 전역 메서드 테이블에 constructor 시점에
 *     등록. 런타임 RPC 서버가 메서드 이름으로 핸들러를 찾아 호출한다.
 * (b) lib/jsonrpc: spdk_jsonrpc_begin_result / end_result / send_error_response / send_bool_response —
 *     핸들러가 결과를 JSON으로 직렬화해 클라이언트에 응답하는 데 사용.
 * (c) lib/json: spdk_json_decode_object / spdk_json_decode_string/uint32/uint64/bool —
 *     RPC params JSON을 C 구조체로 디코딩하는 데 사용.
 * (d) vbdev_lvol.h / vbdev_lvol.c: 핸들러가 최종 위임하는 lvol control plane API.
 *     vbdev_lvs_create_ext, vbdev_lvs_destruct, vbdev_lvs_rename, vbdev_lvs_unload,
 *     vbdev_lvol_create, vbdev_lvol_destroy, vbdev_lvol_create_snapshot, _clone,
 *     _create_bdev_clone, _resize, _rename, _set_read_only, _shallow_copy,
 *     _set_external_parent, vbdev_get_lvol_store_by_{uuid,name}, vbdev_lvol_get_from_bdev,
 *     vbdev_lvol_store_first/next 를 직접 호출.
 * (e) include/spdk/bdev.h: spdk_bdev_get_by_name — lvol bdev 를 이름으로 찾을 때 사용.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_bdev_lvol_create_lvstore: "bdev_lvol_create_lvstore" RPC 핸들러.
 *     베이스 bdev 위에 새 lvol store 를 생성한다. vbdev_lvs_create_ext 로 위임.
 * - rpc_bdev_lvol_delete_lvstore: "bdev_lvol_delete_lvstore" RPC 핸들러.
 *     lvol store 와 그 안의 모든 lvol 을 영구 삭제한다. vbdev_lvs_destruct 로 위임.
 * - rpc_bdev_lvol_create: "bdev_lvol_create" RPC 핸들러.
 *     지정 lvs 안에 thin/thick lvol 을 생성한다. vbdev_lvol_create 로 위임.
 * - rpc_bdev_lvol_snapshot: "bdev_lvol_snapshot" RPC 핸들러.
 *     기존 lvol 의 read-only snapshot 을 생성한다. vbdev_lvol_create_snapshot 로 위임.
 * - rpc_bdev_lvol_clone / rpc_bdev_lvol_clone_bdev:
 *     스냅샷/외부bdev 로부터 clone 을 생성한다.
 * - rpc_bdev_lvol_resize: "bdev_lvol_resize" RPC 핸들러. 논리 크기 변경.
 * - rpc_bdev_lvol_delete: "bdev_lvol_delete" RPC 핸들러. lvol 단건 영구 삭제.
 * - rpc_bdev_lvol_get_lvstores: "bdev_lvol_get_lvstores" RPC 핸들러.
 *     등록된 lvol store 정보 조회.
 * - rpc_bdev_lvol_get_lvols: "bdev_lvol_get_lvols" RPC 핸들러.
 *     lvol 목록과 메타데이터 조회.
 * - rpc_bdev_lvol_start_shallow_copy / rpc_bdev_lvol_check_shallow_copy:
 *     할당된 클러스터를 외부 bdev 로 복사하는 비동기 shallow copy RPC.
 *     진행상황은 g_shallow_copy_status_list 로 추적하며 operation_id 로 조회.
 * - vbdev_get_lvol_store_by_uuid_xor_name: uuid 혹은 name 중 정확히 하나로 lvs 를 찾는 헬퍼.
 * - rpc_dump_lvol_store_info / rpc_dump_lvol: JSON 응답 내 lvs/lvol 정보 직렬화 헬퍼.
 * - struct rpc_shallow_copy_status: 진행 중인 shallow copy 작업의 상태 (operation_id/result/
 *     copied_clusters/total_clusters). g_shallow_copy_status_list LIST 에 관리.
 */

#include "spdk/rpc.h"
/* [한국어] SPDK JSON-RPC 메서드 등록/응답 API — SPDK_RPC_REGISTER 매크로, begin/end_result,
 * send_error_response, send_bool_response 등. */
#include "spdk/bdev.h"
/* [한국어] spdk_bdev_get_by_name — lvol bdev 를 이름으로 찾아 lvol 핸들을 회수하는 데 사용. */
#include "spdk/util.h"
/* [한국어] SPDK_COUNTOF 매크로 — spdk_json_object_decoder 배열 크기를 컴파일 타임에 계산. */
#include "vbdev_lvol.h"
/* [한국어] lvol/lvs control plane 공개 API 및 struct lvol_store_bdev/lvol_bdev 정의.
 * vbdev_lvs_create_ext/destruct/rename, vbdev_lvol_create/destroy/snapshot/clone 등. */
#include "spdk/string.h"
/* [한국어] spdk_strerror — errno 번호를 문자열로 변환해 JSON error 응답 메시지에 사용. */
#include "spdk/log.h"
/* [한국어] SPDK_INFOLOG / SPDK_ERRLOG / SPDK_LOG_REGISTER_COMPONENT —
 * 컴포넌트별 로그 채널(lvol_rpc) 등록 및 런타임 로그 출력. */

SPDK_LOG_REGISTER_COMPONENT(lvol_rpc)
/* [한국어] "lvol_rpc" 로그 컴포넌트 등록. `--logflag lvol_rpc` 으로 INFO 레벨 상세 로그 활성화 가능.
 * constructor 시점에 전역 컴포넌트 테이블에 삽입되어 SPDK_INFOLOG(lvol_rpc, ...) 채널이 동작. */

/*
 * [한국어]
 * struct rpc_shallow_copy_status - 진행 중이거나 방금 완료된 shallow copy 작업 하나의 상태
 *
 * bdev_lvol_start_shallow_copy RPC 가 호출되면 이 구조체가 힙에 할당되고
 * g_shallow_copy_status_list 에 삽입된다. bdev_lvol_check_shallow_copy 로 반복 조회 가능.
 * 작업이 완료(성공·실패)된 후 check RPC 가 "complete" 또는 "error" 상태를 응답할 때 LIST
 * 에서 제거하고 free 된다. 따라서 상태 항목의 수명은 start→check(complete/error) 사이.
 */
struct rpc_shallow_copy_status {
	uint32_t				operation_id;
	/* [한국어] 이 shallow copy 작업을 고유하게 식별하는 단조증가 번호.
	 * 설정자: rpc_bdev_lvol_start_shallow_copy 가 ++g_shallow_copy_count 로 결정.
	 * 읽는 자: rpc_bdev_lvol_check_shallow_copy 가 요청 operation_id 와 비교해 항목을 탐색.
	 * 값 범위: 1 이상의 uint32_t — 0 은 사용 불가(g_shallow_copy_count 초기값 0, 최초 할당 1).
	 * 동기화: 단일 reactor 스레드에서만 접근되므로 별도 락 불필요. */

	/*
	 * 0 means ongoing or successfully completed operation
	 * a negative value is the -errno of an aborted operation
	 */
	int					result;
	/* [한국어] shallow copy 완료 결과 코드.
	 * 설정자: rpc_bdev_lvol_shallow_copy_cb 가 vbdev_lvol_shallow_copy 완료 시 저장.
	 *   - 진행 중이거나 성공적으로 완료된 경우: 0
	 *   - 오류로 중단된 경우: -errno (음수)
	 * 읽는 자: rpc_bdev_lvol_check_shallow_copy 가 state 를 결정할 때 사용.
	 *   copied_clusters == total_clusters && result == 0 → "complete"
	 *   result != 0 → "error" 상태로 JSON 응답 후 status 항목 free.
	 * 값 범위: 0 (성공/진행중) 또는 음수 errno.
	 * 동기화: 단일 reactor 스레드 — 쓰기(cb)와 읽기(check)가 동시에 발생하지 않음. */

	uint64_t				copied_clusters;
	/* [한국어] 현재까지 목적지 bdev 에 복사 완료된 클러스터(cluster) 수.
	 * 설정자: rpc_bdev_lvol_shallow_copy_status_cb — vbdev_lvol_shallow_copy 의 진행
	 *   상황 콜백으로, 클러스터 하나가 복사될 때마다 갱신.
	 * 읽는 자: rpc_bdev_lvol_check_shallow_copy 가 현재 진행률을 응답할 때 스냅샷 후 사용.
	 * 값 범위: 0 ~ total_clusters. 완료 시 total_clusters 와 일치.
	 * 동기화: 단일 reactor 스레드 — 진행 콜백과 조회 RPC 가 동일 스레드. */

	uint64_t				total_clusters;
	/* [한국어] shallow copy 대상 소스 lvol 에 할당된 전체 클러스터 수(복사 총량).
	 * 설정자: rpc_bdev_lvol_start_shallow_copy 가 spdk_blob_get_num_allocated_clusters
	 *   호출 결과로 초기화. 복사 시작 후 변경되지 않음.
	 * 읽는 자: rpc_bdev_lvol_check_shallow_copy 가 진행률 계산 및 완료 판정에 사용.
	 * 값 범위: 0 이상 uint64_t. 0 이면 소스 lvol 이 thin-provisioned 이고 아직 write 없음.
	 * 동기화: 초기화 후 읽기 전용 — 경쟁 없음. */

	LIST_ENTRY(rpc_shallow_copy_status)	link;
	/* [한국어] g_shallow_copy_status_list LIST 의 연결 링크 (BSD queue.h LIST_ENTRY).
	 * 설정자: rpc_bdev_lvol_start_shallow_copy 의 LIST_INSERT_HEAD 호출 시 초기화.
	 * 사용자: LIST_FOREACH 가 탐색 경로로 사용.
	 * 제거: rpc_bdev_lvol_check_shallow_copy 에서 완료/에러 확인 후 LIST_REMOVE 호출.
	 * 동기화: 단일 reactor 스레드 — 삽입·탐색·제거 모두 같은 스레드. */
};

static uint32_t g_shallow_copy_count = 0;
/* [한국어] 전역 shallow copy 작업 카운터 (단조증가).
 * 새 shallow copy 작업이 시작될 때마다 ++g_shallow_copy_count 로 증가시킨 후
 * 해당 값을 operation_id 로 status 에 부여한다.
 * 설정자: rpc_bdev_lvol_start_shallow_copy.
 * 읽는 자: 동일 함수 내부에서 status->operation_id 할당 시만 참조.
 * 값 범위: 0부터 시작, UINT32_MAX 초과 시 wrap — 현실에서 발생 거의 없음.
 * 동기화: 단일 reactor 스레드에서만 접근하므로 원자적 연산 불필요. */

static LIST_HEAD(, rpc_shallow_copy_status) g_shallow_copy_status_list = LIST_HEAD_INITIALIZER(
			&g_shallow_copy_status_list);
/* [한국어] 현재 진행 중이거나 방금 완료된 shallow copy 작업 상태 목록 (BSD queue.h LIST_HEAD).
 * 각 bdev_lvol_start_shallow_copy 호출마다 rpc_shallow_copy_status 가 이 LIST 에 추가된다.
 * bdev_lvol_check_shallow_copy 가 operation_id 로 탐색하여 진행률을 반환하고, 완료 시 항목을 제거한다.
 * 설정자: rpc_bdev_lvol_start_shallow_copy (LIST_INSERT_HEAD).
 * 제거: rpc_bdev_lvol_check_shallow_copy (LIST_REMOVE, 완료/에러 응답 직전).
 * 동기화: 단일 reactor 스레드에서만 접근 — 뮤텍스 불필요. */

/*
 * [한국어]
 * struct rpc_bdev_lvol_create_lvstore - "bdev_lvol_create_lvstore" RPC params 디코딩 컨테이너
 *
 * JSON-RPC 클라이언트가 bdev_lvol_create_lvstore 를 호출할 때 전달하는 파라미터를 담는다.
 * spdk_json_decode_object 가 rpc_bdev_lvol_create_lvstore_decoders 테이블을 참조해 이 구조체
 * 필드에 값을 직접 기록한다. 사용 후 free_rpc_bdev_lvol_create_lvstore 로 문자열 멤버를 해제.
 */
struct rpc_bdev_lvol_create_lvstore {
	char *lvs_name;
	/* [한국어] 생성할 lvol store 의 사람이 읽을 수 있는 이름.
	 * 설정자: spdk_json_decode_string 이 JSON "lvs_name" 필드에서 복사해 strdup.
	 * 읽는 자: rpc_bdev_lvol_create_lvstore 가 vbdev_lvs_create_ext 호출 시 전달.
	 * 값 범위: NULL 불가 (필수 파라미터). 빈 문자열은 논리적으로 유효하지 않음.
	 * 해제: free_rpc_bdev_lvol_create_lvstore 에서 free(req->lvs_name). */

	char *bdev_name;
	/* [한국어] lvol store 를 생성할 베이스(base) bdev 의 이름.
	 * 설정자: spdk_json_decode_string 이 JSON "bdev_name" 필드에서 strdup.
	 * 읽는 자: rpc_bdev_lvol_create_lvstore 가 vbdev_lvs_create_ext 의 첫 인자로 전달.
	 * vbdev_lvs_create_ext 내부에서 spdk_bdev_get_by_name 으로 실제 bdev 를 찾는다.
	 * 값 범위: NULL 불가 (필수 파라미터).
	 * 해제: free_rpc_bdev_lvol_create_lvstore 에서 free(req->bdev_name). */

	uint32_t cluster_sz;
	/* [한국어] blobstore 클러스터(cluster) 크기(바이트). 0 이면 기본값(1 MiB) 사용.
	 * 설정자: spdk_json_decode_uint32 이 JSON "cluster_sz" 필드에서 파싱 (선택 파라미터).
	 *   지정하지 않으면 0으로 유지되어 lib/lvol 이 SPDK_LVOL_CLUSTER_SZ_DEFAULT 적용.
	 * 읽는 자: rpc_bdev_lvol_create_lvstore 가 vbdev_lvs_create_ext 의 cluster_sz 인자로 전달.
	 * 값 범위: 0 (기본) 또는 bdev block_size 의 배수인 2의 거듭제곱 값.
	 * 동기화: 단일 스레드 — 락 불필요. */

	char *clear_method;
	/* [한국어] blobstore 초기화 시 클러스터를 지우는 방법 문자열.
	 * 설정자: spdk_json_decode_string 이 JSON "clear_method" 에서 strdup (선택 파라미터).
	 *   NULL 이면 핸들러가 기본값 LVS_CLEAR_WITH_UNMAP 을 적용.
	 *   가능한 값: "none", "unmap", "write_zeroes" (대소문자 무관).
	 * 읽는 자: rpc_bdev_lvol_create_lvstore 에서 strcasecmp 로 enum lvs_clear_method 로 변환.
	 * 해제: free_rpc_bdev_lvol_create_lvstore 에서 free(req->clear_method). */

	uint32_t num_md_pages_per_cluster_ratio;
	/* [한국어] 클러스터당 메타데이터 페이지 비율 (퍼센트 단위, 선택 파라미터).
	 * 설정자: spdk_json_decode_uint32 이 JSON "num_md_pages_per_cluster_ratio" 에서 파싱.
	 *   0 이면 lib/blob 의 기본 메타데이터 할당 비율 사용.
	 * 읽는 자: rpc_bdev_lvol_create_lvstore → vbdev_lvs_create_ext 의 해당 인자로 전달.
	 * 값 범위: 0 ~ 100 (실질적 상한은 lib/lvol 에서 검증).
	 * 동기화: 단일 스레드 — 락 불필요. */

	uint32_t md_page_size;
	/* [한국어] blobstore 메타데이터 페이지 크기(바이트, 선택 파라미터).
	 * 설정자: spdk_json_decode_uint32 이 JSON "md_page_size" 에서 파싱.
	 *   0 이면 lib/blob 기본값(SPDK_BS_PAGE_SIZE) 사용.
	 * 읽는 자: rpc_bdev_lvol_create_lvstore → vbdev_lvs_create_ext 의 md_page_size 인자.
	 * 값 범위: 0 또는 512의 배수 — lib/blob 에서 유효성 검사.
	 * 동기화: 단일 스레드. */
};

/*
 * [한국어]
 * vbdev_get_lvol_store_by_uuid_xor_name - UUID 또는 이름 중 정확히 하나로 lvol store 를 탐색
 *
 * @uuid:     탐색에 사용할 UUID 문자열. NULL 이면 lvs_name 으로 탐색한다.
 *            어디서 오는가: RPC 디코딩 구조체의 uuid 필드 (선택 파라미터).
 * @lvs_name: 탐색에 사용할 lvol store 이름 문자열. NULL 이면 uuid 로 탐색한다.
 *            어디서 오는가: RPC 디코딩 구조체의 lvs_name 필드 (선택 파라미터).
 * @lvs:      [출력] 찾은 spdk_lvol_store 포인터를 기록할 포인터 변수. 실패 시 불변.
 * @return:   0 → 성공, *lvs 에 유효한 포인터 저장됨.
 *            -EINVAL → uuid 와 lvs_name 이 둘 다 NULL 이거나 둘 다 제공된 경우.
 *            -ENODEV  → 주어진 uuid/name 에 해당하는 lvs 가 존재하지 않는 경우.
 *
 * uuid 와 lvs_name 은 서로 배타적(XOR) 이어야 한다 — 둘 다 없거나 둘 다 있으면
 * -EINVAL 을 반환한다. 하나만 제공되었을 때 해당 기준으로 g_spdk_lvol_pairs 를 순회해 탐색.
 * 실행 컨텍스트: SPDK reactor 스레드 (단일 스레드). 락 불필요.
 *
 * 호출 체인:
 *   rpc_bdev_lvol_delete_lvstore → [이 함수]
 *   rpc_bdev_lvol_create         → [이 함수]
 *   rpc_bdev_lvol_get_lvstores   → [이 함수]
 *   rpc_bdev_lvol_get_lvols      → [이 함수]
 *   rpc_bdev_lvol_grow_lvstore   → [이 함수]
 *   [이 함수] → vbdev_get_lvol_store_by_uuid / vbdev_get_lvol_store_by_name
 */
static int
vbdev_get_lvol_store_by_uuid_xor_name(const char *uuid, const char *lvs_name,
				      struct spdk_lvol_store **lvs)
{
	if ((uuid == NULL && lvs_name == NULL)) { /* [한국어] 두 파라미터 모두 미제공 — 탐색 기준 없음 */
		SPDK_INFOLOG(lvol_rpc, "lvs UUID nor lvs name specified\n");
		return -EINVAL; /* [한국어] 파라미터 오류 반환 — 호출자가 JSON error 응답 발송 */
	} else if ((uuid && lvs_name)) { /* [한국어] 두 파라미터 모두 제공 — XOR 위반 */
		SPDK_INFOLOG(lvol_rpc, "both lvs UUID '%s' and lvs name '%s' specified\n", uuid,
			     lvs_name);
		return -EINVAL; /* [한국어] 모호한 지정 — 파라미터 오류 반환 */
	} else if (uuid) { /* [한국어] uuid 만 제공된 경우 — UUID 로 탐색 */
		*lvs = vbdev_get_lvol_store_by_uuid(uuid);
		/* [한국어] vbdev_lvol.c 의 헬퍼: g_spdk_lvol_pairs TAILQ 를 순회해
		 * spdk_uuid_fmt_lower 결과가 uuid 문자열과 일치하는 lvs 반환. 없으면 NULL. */

		if (*lvs == NULL) { /* [한국어] 해당 UUID 의 lvol store 없음 */
			SPDK_INFOLOG(lvol_rpc, "blobstore with UUID '%s' not found\n", uuid);
			return -ENODEV; /* [한국어] 존재하지 않는 장치 — 호출자가 에러 응답 */
		}
	} else if (lvs_name) { /* [한국어] lvs_name 만 제공된 경우 — 이름으로 탐색 */

		*lvs = vbdev_get_lvol_store_by_name(lvs_name);
		/* [한국어] vbdev_lvol.c 의 헬퍼: g_spdk_lvol_pairs TAILQ 에서 lvs->name 이
		 * lvs_name 과 일치하는 첫 번째 항목 반환. 없으면 NULL. */

		if (*lvs == NULL) { /* [한국어] 해당 이름의 lvol store 없음 */
			SPDK_INFOLOG(lvol_rpc, "blobstore with name '%s' not found\n", lvs_name);
			return -ENODEV; /* [한국어] 존재하지 않는 장치 — 호출자가 에러 응답 */
		}
	}
	return 0; /* [한국어] 탐색 성공 — *lvs 에 유효한 포인터 저장됨 */
}

/*
 * [한국어]
 * free_rpc_bdev_lvol_create_lvstore - rpc_bdev_lvol_create_lvstore 파라미터 구조체 문자열 해제
 *
 * @req: 해제할 rpc_bdev_lvol_create_lvstore 구조체 포인터.
 *        spdk_json_decode_object 가 strdup 으로 복사한 문자열 필드를 free 한다.
 * @return: 없음.
 *
 * bdev_name, lvs_name, clear_method 는 디코딩 시 strdup 으로 복사되므로 반드시 해제해야 한다.
 * cluster_sz, num_md_pages_per_cluster_ratio, md_page_size 는 정수형이므로 해제 불필요.
 * 성공 경로: vbdev_lvs_create_ext 비동기 호출 후 즉시 호출 (cb 가 request 를 관리).
 * 에러 경로: cleanup 레이블에서 에러 응답 직후 호출.
 * 실행 컨텍스트: SPDK reactor 스레드.
 *
 * 호출 체인:
 *   rpc_bdev_lvol_create_lvstore → [이 함수]
 */
static void
free_rpc_bdev_lvol_create_lvstore(struct rpc_bdev_lvol_create_lvstore *req)
{
	free(req->bdev_name);   /* [한국어] strdup 으로 복사된 베이스 bdev 이름 문자열 해제 */
	free(req->lvs_name);    /* [한국어] strdup 으로 복사된 lvol store 이름 문자열 해제 */
	free(req->clear_method); /* [한국어] strdup 으로 복사된 초기화 방법 문자열 해제 (NULL 이면 free(NULL) — 안전) */
}

/*
 * [한국어] rpc_bdev_lvol_create_lvstore_decoders - JSON 파라미터 → C 구조체 필드 매핑 테이블
 *
 * spdk_json_decode_object 에 전달되어 JSON object 의 각 키를 rpc_bdev_lvol_create_lvstore
 * 구조체의 해당 필드에 자동으로 기록한다.
 * 마지막 인자(bool)가 true 인 항목은 선택(optional) 파라미터 — JSON 에 없어도 오류 없음.
 * false 또는 생략된 항목은 필수(required) — JSON 에 없으면 spdk_json_decode_object 실패.
 */
static const struct spdk_json_object_decoder rpc_bdev_lvol_create_lvstore_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_lvol_create_lvstore, bdev_name), spdk_json_decode_string},
	/* [한국어] "bdev_name" 키 → bdev_name 필드. 필수. blobstore 를 생성할 베이스 bdev 이름. */
	{"cluster_sz", offsetof(struct rpc_bdev_lvol_create_lvstore, cluster_sz), spdk_json_decode_uint32, true},
	/* [한국어] "cluster_sz" 키 → cluster_sz 필드. 선택. blobstore 클러스터 크기(바이트). 미지정 시 0. */
	{"lvs_name", offsetof(struct rpc_bdev_lvol_create_lvstore, lvs_name), spdk_json_decode_string},
	/* [한국어] "lvs_name" 키 → lvs_name 필드. 필수. 생성할 lvol store 이름. */
	{"clear_method", offsetof(struct rpc_bdev_lvol_create_lvstore, clear_method), spdk_json_decode_string, true},
	/* [한국어] "clear_method" 키 → clear_method 필드. 선택. 없으면 NULL → 핸들러가 "unmap" 기본값 사용. */
	{"num_md_pages_per_cluster_ratio", offsetof(struct rpc_bdev_lvol_create_lvstore, num_md_pages_per_cluster_ratio), spdk_json_decode_uint32, true},
	/* [한국어] "num_md_pages_per_cluster_ratio" 키 → num_md_pages_per_cluster_ratio 필드. 선택. */
	{"md_page_size", offsetof(struct rpc_bdev_lvol_create_lvstore, md_page_size), spdk_json_decode_uint32, true},
	/* [한국어] "md_page_size" 키 → md_page_size 필드. 선택. 메타데이터 페이지 크기(바이트). */
};

/*
 * [한국어]
 * rpc_lvol_store_construct_cb - vbdev_lvs_create_ext 비동기 완료 콜백
 *
 * @cb_arg:     콜백 인자 — 원래 JSON-RPC 요청 컨텍스트(spdk_jsonrpc_request *).
 *              rpc_bdev_lvol_create_lvstore 에서 vbdev_lvs_create_ext 에 전달한 것.
 * @lvol_store: 생성된 lvol store 포인터. lvserrno != 0 이면 유효하지 않을 수 있음.
 * @lvserrno:   완료 결과. 0 = 성공, 음수 = errno 기반 오류 코드.
 * @return: 없음.
 *
 * lvol store 생성이 완료되면 lib/lvol 이 이 콜백을 호출한다.
 * 성공 시: spdk_jsonrpc_begin_result 로 응답 컨텍스트 열고, 새 lvs 의 UUID 를 JSON string
 *   으로 직렬화한 뒤 end_result 로 응답 완료 — 클라이언트가 UUID 를 수신.
 * 실패 시: invalid 레이블로 점프해 send_error_response 로 errno 메시지 전달.
 * 실행 컨텍스트: SPDK reactor 스레드 (vbdev_lvs_create_ext 가 동일 스레드에서 콜백 호출).
 *
 * 호출 체인:
 *   vbdev_lvs_create_ext(내부 async) → [이 함수] → spdk_jsonrpc_end_result
 *                                                  OR spdk_jsonrpc_send_error_response
 */
static void
rpc_lvol_store_construct_cb(void *cb_arg, struct spdk_lvol_store *lvol_store, int lvserrno)
{
	struct spdk_json_write_ctx *w;                      /* [한국어] JSON 응답 직렬화 컨텍스트 */
	struct spdk_jsonrpc_request *request = cb_arg;       /* [한국어] 원본 RPC 요청 — cb_arg 로 전달받음 */

	if (lvserrno != 0) { /* [한국어] lvol store 생성 실패 — 에러 응답 경로 진입 */
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request); /* [한국어] JSON 결과 응답 시작 — write_ctx 획득 */
	spdk_json_write_uuid(w, &lvol_store->uuid); /* [한국어] 생성된 lvs UUID 를 JSON string 으로 직렬화 */
	spdk_jsonrpc_end_result(request, w); /* [한국어] 응답 완료 — 클라이언트에 UUID 문자열 전송 */
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvserrno));
	/* [한국어] lvol store 생성 오류 — errno 메시지를 JSON-RPC error 응답으로 전송 */
}

/*
 * [한국어]
 * rpc_bdev_lvol_create_lvstore - "bdev_lvol_create_lvstore" JSON-RPC 핸들러
 *
 * @request: lib/jsonrpc 가 전달하는 요청 컨텍스트. 응답 전송에 사용.
 * @params:  JSON-RPC params 객체. NULL 이면 파라미터 없음 (이 RPC 에서는 허용 불가).
 * @return: 없음 (모든 결과는 비동기 콜백으로 응답).
 *
 * JSON 파라미터를 rpc_bdev_lvol_create_lvstore 구조체로 디코딩한 뒤 clear_method 문자열을
 * enum lvs_clear_method 로 변환하고 vbdev_lvs_create_ext 를 비동기 호출한다.
 * 비동기 완료 시 rpc_lvol_store_construct_cb 가 UUID 를 응답하거나 에러를 전달한다.
 * 에러: 디코딩 실패, 잘못된 clear_method 문자열, vbdev_lvs_create_ext 즉시 실패 시
 *   각각 error response 를 즉시 전송하고 cleanup 으로 파라미터를 해제한다.
 * 실행 컨텍스트: SPDK reactor 스레드 (lib/rpc 가 메서드 디스패치 시 호출).
 *
 * 호출 체인:
 *   lib/rpc (메서드 디스패치) → [이 함수] → vbdev_lvs_create_ext
 *                                          → (async) rpc_lvol_store_construct_cb
 */
static void
rpc_bdev_lvol_create_lvstore(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_create_lvstore req = {}; /* [한국어] 파라미터 구조체 zero-초기화 */
	int rc = 0;                                    /* [한국어] vbdev_lvs_create_ext 동기 반환값 저장 */
	enum lvs_clear_method clear_method;            /* [한국어] 문자열을 변환한 클리어 방법 enum */

	if (spdk_json_decode_object(params, rpc_bdev_lvol_create_lvstore_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_create_lvstore_decoders),
				    &req)) {
		/* [한국어] JSON params 디코딩 실패 — 필수 파라미터 누락이나 타입 불일치 */
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup; /* [한국어] 에러 응답 후 파라미터 구조체 해제 */
	}

	if (req.clear_method != NULL) { /* [한국어] clear_method 파라미터가 명시된 경우 */
		if (!strcasecmp(req.clear_method, "none")) { /* [한국어] "none" → 클리어 없이 blobstore 생성 */
			clear_method = LVS_CLEAR_WITH_NONE;
		} else if (!strcasecmp(req.clear_method, "unmap")) { /* [한국어] "unmap" → SCSI UNMAP 으로 초기화 */
			clear_method = LVS_CLEAR_WITH_UNMAP;
		} else if (!strcasecmp(req.clear_method, "write_zeroes")) { /* [한국어] "write_zeroes" → 0 쓰기로 초기화 */
			clear_method = LVS_CLEAR_WITH_WRITE_ZEROES;
		} else { /* [한국어] 알 수 없는 clear_method 문자열 — 즉시 에러 */
			spdk_jsonrpc_send_error_response(request, -EINVAL, "Invalid clear_method parameter");
			goto cleanup;
		}
	} else { /* [한국어] clear_method 미지정 — 기본값으로 UNMAP 사용 */
		clear_method = LVS_CLEAR_WITH_UNMAP;
	}

	rc = vbdev_lvs_create_ext(req.bdev_name, req.lvs_name, req.cluster_sz, clear_method,
				  req.num_md_pages_per_cluster_ratio, req.md_page_size,
				  rpc_lvol_store_construct_cb, request);
	/* [한국어] 비동기 lvol store 생성 시작.
	 * 내부에서 베이스 bdev 를 찾고, spdk_bs_bdev_claim 으로 독점 소유권을 주장한 뒤
	 * spdk_lvs_init 을 비동기 호출. 완료 시 rpc_lvol_store_construct_cb 호출.
	 * 즉시 실패(rc < 0): bdev 없음, 이미 사용 중, 메모리 부족 등. */
	if (rc < 0) { /* [한국어] vbdev_lvs_create_ext 즉시 실패 — 비동기 작업이 시작되지 않음 */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}
	free_rpc_bdev_lvol_create_lvstore(&req);
	/* [한국어] 비동기 작업이 시작된 경우: 파라미터 구조체 즉시 해제.
	 * vbdev_lvs_create_ext 는 필요한 값을 내부에 복사했으므로 안전하게 해제 가능.
	 * request 포인터는 rpc_lvol_store_construct_cb 가 완료 시 사용하므로 여기서 해제하지 않음. */

	return;

cleanup:
	free_rpc_bdev_lvol_create_lvstore(&req); /* [한국어] 에러 경로 — 디코딩된 문자열 해제 */
}
SPDK_RPC_REGISTER("bdev_lvol_create_lvstore", rpc_bdev_lvol_create_lvstore, SPDK_RPC_RUNTIME)
/* [한국어] "bdev_lvol_create_lvstore" 메서드를 SPDK_RPC_RUNTIME 단계에 등록.
 * constructor 우선순위로 전역 메서드 테이블에 삽입되어 런타임 RPC 서버가 이름으로 핸들러를 찾음. */

/*
 * [한국어]
 * struct rpc_bdev_lvol_rename_lvstore - "bdev_lvol_rename_lvstore" RPC params 컨테이너
 *
 * lvol store 의 이름을 old_name 에서 new_name 으로 변경하는 RPC 요청 파라미터를 담는다.
 */
struct rpc_bdev_lvol_rename_lvstore {
	char *old_name;
	/* [한국어] 이름을 변경할 기존 lvol store 이름.
	 * 설정자: spdk_json_decode_string 이 JSON "old_name" 에서 strdup.
	 * 읽는 자: rpc_bdev_lvol_rename_lvstore 가 vbdev_get_lvol_store_by_name 호출 시 사용.
	 * 값 범위: NULL 불가 (필수 파라미터). 반드시 등록된 lvs 이름이어야 함.
	 * 해제: free_rpc_bdev_lvol_rename_lvstore 에서 free. */

	char *new_name;
	/* [한국어] lvol store 에 부여할 새 이름.
	 * 설정자: spdk_json_decode_string 이 JSON "new_name" 에서 strdup.
	 * 읽는 자: rpc_bdev_lvol_rename_lvstore 가 vbdev_lvs_rename 호출 시 전달.
	 * 값 범위: NULL 불가 (필수 파라미터). 다른 lvs 와 중복되지 않아야 함 (lib/lvol 검증).
	 * 해제: free_rpc_bdev_lvol_rename_lvstore 에서 free. */
};

/*
 * [한국어]
 * free_rpc_bdev_lvol_rename_lvstore - rpc_bdev_lvol_rename_lvstore 구조체 해제
 *
 * @req: 해제할 파라미터 구조체 포인터.
 * @return: 없음.
 *
 * 호출 체인: rpc_bdev_lvol_rename_lvstore → [이 함수]
 */
static void
free_rpc_bdev_lvol_rename_lvstore(struct rpc_bdev_lvol_rename_lvstore *req)
{
	free(req->old_name); /* [한국어] 이전 이름 문자열 해제 */
	free(req->new_name); /* [한국어] 새 이름 문자열 해제 */
}

/*
 * [한국어] rpc_bdev_lvol_rename_lvstore_decoders - JSON 파라미터 매핑 테이블 (rename lvstore)
 * old_name, new_name 모두 필수 파라미터.
 */
static const struct spdk_json_object_decoder rpc_bdev_lvol_rename_lvstore_decoders[] = {
	{"old_name", offsetof(struct rpc_bdev_lvol_rename_lvstore, old_name), spdk_json_decode_string},
	/* [한국어] "old_name" 키 → old_name 필드. 필수. 변경 전 lvol store 이름. */
	{"new_name", offsetof(struct rpc_bdev_lvol_rename_lvstore, new_name), spdk_json_decode_string},
	/* [한국어] "new_name" 키 → new_name 필드. 필수. 변경 후 lvol store 이름. */
};

/*
 * [한국어]
 * rpc_bdev_lvol_rename_lvstore_cb - vbdev_lvs_rename 비동기 완료 콜백
 *
 * @cb_arg:   원본 spdk_jsonrpc_request 포인터.
 * @lvserrno: 이름 변경 결과. 0 = 성공, 음수 = 실패 errno.
 * @return: 없음.
 *
 * 성공 시 true 를 JSON bool 응답으로 전송.
 * 실패 시 errno 메시지를 JSON-RPC error 응답으로 전송.
 *
 * 호출 체인:
 *   vbdev_lvs_rename (async) → [이 함수] → spdk_jsonrpc_send_bool_response
 *                                         OR spdk_jsonrpc_send_error_response
 */
static void
rpc_bdev_lvol_rename_lvstore_cb(void *cb_arg, int lvserrno)
{
	struct spdk_jsonrpc_request *request = cb_arg; /* [한국어] 원본 RPC 요청 컨텍스트 복원 */

	if (lvserrno != 0) { /* [한국어] 이름 변경 실패 */
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 성공 → true JSON bool 응답 */
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvserrno));
	/* [한국어] 실패 → errno 텍스트를 JSON-RPC error 메시지로 전송 */
}

/*
 * [한국어]
 * rpc_bdev_lvol_rename_lvstore - "bdev_lvol_rename_lvstore" JSON-RPC 핸들러
 *
 * @request: JSON-RPC 요청 컨텍스트.
 * @params:  JSON-RPC params 객체. old_name, new_name 필드 포함 필수.
 * @return: 없음.
 *
 * old_name 으로 lvol store 를 탐색하고, 찾으면 vbdev_lvs_rename 을 비동기 호출한다.
 * 에러: 디코딩 실패, old_name lvs 없음 시 즉시 에러 응답.
 * 실행 컨텍스트: SPDK reactor 스레드.
 *
 * 호출 체인:
 *   lib/rpc → [이 함수] → vbdev_lvs_rename
 *                       → (async) rpc_bdev_lvol_rename_lvstore_cb
 */
static void
rpc_bdev_lvol_rename_lvstore(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_rename_lvstore req = {}; /* [한국어] 파라미터 구조체 zero-초기화 */
	struct spdk_lvol_store *lvs;                   /* [한국어] 탐색된 lvol store 포인터 */

	if (spdk_json_decode_object(params, rpc_bdev_lvol_rename_lvstore_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_rename_lvstore_decoders),
				    &req)) {
		/* [한국어] JSON params 디코딩 실패 */
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	lvs = vbdev_get_lvol_store_by_name(req.old_name);
	/* [한국어] old_name 으로 등록된 lvol store 탐색 — g_spdk_lvol_pairs 순회 */
	if (lvs == NULL) { /* [한국어] 해당 이름의 lvol store 없음 */
		SPDK_INFOLOG(lvol_rpc, "no lvs existing for given name\n");
		spdk_jsonrpc_send_error_response_fmt(request, -ENOENT, "Lvol store %s not found", req.old_name);
		goto cleanup;
	}

	vbdev_lvs_rename(lvs, req.new_name, rpc_bdev_lvol_rename_lvstore_cb, request);
	/* [한국어] 비동기 이름 변경 시작. req 구조체는 이후 cleanup 에서 해제.
	 * vbdev_lvs_rename 은 new_name 을 내부에 복사하므로 구조체 해제 전에도 안전. */

cleanup:
	free_rpc_bdev_lvol_rename_lvstore(&req); /* [한국어] 성공/실패 공통 — 파라미터 문자열 해제 */
}
SPDK_RPC_REGISTER("bdev_lvol_rename_lvstore", rpc_bdev_lvol_rename_lvstore, SPDK_RPC_RUNTIME)
/* [한국어] "bdev_lvol_rename_lvstore" 메서드를 SPDK_RPC_RUNTIME 단계에 등록. */

/*
 * [한국어]
 * struct rpc_bdev_lvol_delete_lvstore - "bdev_lvol_delete_lvstore" RPC params 컨테이너
 *
 * lvol store 를 삭제하는 RPC 의 파라미터를 담는다. uuid 또는 lvs_name 중 하나만 지정해야 한다.
 */
struct rpc_bdev_lvol_delete_lvstore {
	char *uuid;
	/* [한국어] 삭제할 lvol store 의 UUID 문자열 (선택, lvs_name 과 배타적).
	 * 설정자: spdk_json_decode_string 이 JSON "uuid" 에서 strdup.
	 * 읽는 자: rpc_bdev_lvol_delete_lvstore 에서 vbdev_get_lvol_store_by_uuid_xor_name 에 전달.
	 * 값 범위: NULL (미지정) 또는 유효한 UUID 형식 문자열.
	 * 해제: free_rpc_bdev_lvol_delete_lvstore 에서 free. */

	char *lvs_name;
	/* [한국어] 삭제할 lvol store 의 이름 (선택, uuid 와 배타적).
	 * 설정자: spdk_json_decode_string 이 JSON "lvs_name" 에서 strdup.
	 * 읽는 자: vbdev_get_lvol_store_by_uuid_xor_name 에 전달.
	 * 값 범위: NULL (미지정) 또는 등록된 lvs 이름.
	 * 해제: free_rpc_bdev_lvol_delete_lvstore 에서 free. */
};

/*
 * [한국어]
 * free_rpc_bdev_lvol_delete_lvstore - 파라미터 구조체 문자열 해제
 *
 * 호출 체인: rpc_bdev_lvol_delete_lvstore → [이 함수]
 */
static void
free_rpc_bdev_lvol_delete_lvstore(struct rpc_bdev_lvol_delete_lvstore *req)
{
	free(req->uuid);     /* [한국어] UUID 문자열 해제 (NULL 이면 free(NULL) — 안전) */
	free(req->lvs_name); /* [한국어] lvs 이름 문자열 해제 */
}

/*
 * [한국어] rpc_bdev_lvol_delete_lvstore_decoders - JSON 파라미터 매핑 테이블 (delete lvstore)
 * uuid 와 lvs_name 모두 선택 파라미터 (핸들러에서 XOR 검증).
 */
static const struct spdk_json_object_decoder rpc_bdev_lvol_delete_lvstore_decoders[] = {
	{"uuid", offsetof(struct rpc_bdev_lvol_delete_lvstore, uuid), spdk_json_decode_string, true},
	/* [한국어] "uuid" 키 → uuid 필드. 선택. 삭제할 lvs UUID. */
	{"lvs_name", offsetof(struct rpc_bdev_lvol_delete_lvstore, lvs_name), spdk_json_decode_string, true},
	/* [한국어] "lvs_name" 키 → lvs_name 필드. 선택. 삭제할 lvs 이름. */
};

/*
 * [한국어]
 * rpc_lvol_store_destroy_cb - vbdev_lvs_destruct 비동기 완료 콜백
 *
 * @cb_arg:   원본 spdk_jsonrpc_request 포인터.
 * @lvserrno: lvol store 삭제 결과. 0 = 성공, 음수 = 실패 errno.
 * @return: 없음.
 *
 * 성공 시 true 를 JSON bool 응답, 실패 시 errno 메시지를 JSON-RPC error 응답으로 전송.
 *
 * 호출 체인:
 *   vbdev_lvs_destruct (async) → [이 함수] → spdk_jsonrpc_send_bool_response
 *                                           OR spdk_jsonrpc_send_error_response
 */
static void
rpc_lvol_store_destroy_cb(void *cb_arg, int lvserrno)
{
	struct spdk_jsonrpc_request *request = cb_arg; /* [한국어] 원본 RPC 요청 컨텍스트 복원 */

	if (lvserrno != 0) { /* [한국어] 삭제 실패 — 에러 응답 경로 */
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true); /* [한국어] 삭제 성공 → true 응답 */
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvserrno));
	/* [한국어] 삭제 실패 → errno 텍스트를 JSON-RPC error 응답으로 전송 */
}

/*
 * [한국어]
 * rpc_bdev_lvol_delete_lvstore - "bdev_lvol_delete_lvstore" JSON-RPC 핸들러
 *
 * @request: JSON-RPC 요청 컨텍스트.
 * @params:  JSON-RPC params. uuid 또는 lvs_name 중 정확히 하나 포함.
 * @return: 없음.
 *
 * uuid 또는 lvs_name 으로 lvol store 를 찾아 vbdev_lvs_destruct 를 비동기 호출한다.
 * vbdev_lvs_destruct 는 lvs 내 모든 lvol 을 unregister 하고 blobstore 를 닫는다.
 * 에러: 디코딩 실패, XOR 위반, lvs 없음 시 즉시 에러 응답.
 * 실행 컨텍스트: SPDK reactor 스레드.
 *
 * 호출 체인:
 *   lib/rpc → [이 함수] → vbdev_get_lvol_store_by_uuid_xor_name
 *                       → vbdev_lvs_destruct
 *                       → (async) rpc_lvol_store_destroy_cb
 */
static void
rpc_bdev_lvol_delete_lvstore(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_delete_lvstore req = {}; /* [한국어] 파라미터 구조체 zero-초기화 */
	struct spdk_lvol_store *lvs = NULL;            /* [한국어] 탐색된 lvol store 포인터 */
	int rc;                                        /* [한국어] XOR 탐색 반환값 */

	if (spdk_json_decode_object(params, rpc_bdev_lvol_delete_lvstore_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_delete_lvstore_decoders),
				    &req)) {
		/* [한국어] JSON params 디코딩 실패 */
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = vbdev_get_lvol_store_by_uuid_xor_name(req.uuid, req.lvs_name, &lvs);
	/* [한국어] uuid XOR lvs_name 으로 lvol store 탐색.
	 * 두 파라미터가 모두 NULL 이거나 둘 다 지정된 경우 -EINVAL 반환. */
	if (rc != 0) { /* [한국어] XOR 탐색 실패 (파라미터 오류 또는 lvs 없음) */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	vbdev_lvs_destruct(lvs, rpc_lvol_store_destroy_cb, request);
	/* [한국어] lvol store 비동기 삭제 시작.
	 * 내부에서 모든 lvol 을 spdk_bdev_unregister 하고 spdk_lvs_destruct 호출.
	 * 완료 시 rpc_lvol_store_destroy_cb 로 결과 전달. */

cleanup:
	free_rpc_bdev_lvol_delete_lvstore(&req); /* [한국어] 파라미터 문자열 해제 */
}
SPDK_RPC_REGISTER("bdev_lvol_delete_lvstore", rpc_bdev_lvol_delete_lvstore, SPDK_RPC_RUNTIME)
/* [한국어] "bdev_lvol_delete_lvstore" 메서드를 SPDK_RPC_RUNTIME 단계에 등록. */

/*
 * [한국어]
 * struct rpc_bdev_lvol_create - "bdev_lvol_create" RPC params 컨테이너
 *
 * 지정한 lvol store 안에 새 logical volume 을 생성하는 RPC 의 파라미터를 담는다.
 * lvol store 는 uuid 또는 lvs_name 중 하나로 지정한다(XOR).
 */
struct rpc_bdev_lvol_create {
	char *uuid;
	/* [한국어] 대상 lvol store 의 UUID 문자열 (선택, lvs_name 과 배타적).
	 * 설정자: spdk_json_decode_string 이 JSON "uuid" 에서 strdup.
	 * 읽는 자: rpc_bdev_lvol_create 에서 vbdev_get_lvol_store_by_uuid_xor_name 에 전달.
	 * 값 범위: NULL 또는 유효 UUID 형식.
	 * 해제: free_rpc_bdev_lvol_create 에서 free. */

	char *lvs_name;
	/* [한국어] 대상 lvol store 의 이름 (선택, uuid 와 배타적).
	 * 설정자: spdk_json_decode_string 이 JSON "lvs_name" 에서 strdup.
	 * 읽는 자: vbdev_get_lvol_store_by_uuid_xor_name 에 전달.
	 * 값 범위: NULL 또는 등록된 lvs 이름.
	 * 해제: free_rpc_bdev_lvol_create 에서 free. */

	char *lvol_name;
	/* [한국어] 생성할 lvol 의 이름 (필수).
	 * 설정자: spdk_json_decode_string 이 JSON "lvol_name" 에서 strdup.
	 * 읽는 자: rpc_bdev_lvol_create 에서 vbdev_lvol_create 에 전달.
	 *   최종 bdev 이름은 "lvs_name/lvol_name" 형태의 alias 로 등록됨.
	 * 값 범위: NULL 불가. 같은 lvs 내 중복 금지 (lib/lvol 검증).
	 * 해제: free_rpc_bdev_lvol_create 에서 free. */

	uint64_t size_in_mib;
	/* [한국어] 생성할 lvol 의 논리 크기 (MiB 단위, 필수).
	 * 설정자: spdk_json_decode_uint64 이 JSON "size_in_mib" 에서 파싱.
	 * 읽는 자: rpc_bdev_lvol_create 에서 바이트로 변환(* 1024 * 1024) 후 vbdev_lvol_create 에 전달.
	 *   thin provisioning 시 실제 클러스터 할당은 첫 write 시 발생.
	 * 값 범위: 0 초과. 베이스 bdev 총 크기 초과 시 lib/lvol 에서 오류.
	 * 동기화: 단일 스레드. */

	bool thin_provision;
	/* [한국어] thin provisioning 활성화 여부 (선택, 기본 false = thick).
	 * 설정자: spdk_json_decode_bool 이 JSON "thin_provision" 에서 파싱. 미지정 시 false.
	 * 읽는 자: rpc_bdev_lvol_create 에서 vbdev_lvol_create 의 thin_provision 인자로 전달.
	 *   true: 클러스터를 쓰기 시점에만 할당 (SPDK blob thin provisioning).
	 *   false: 생성 시점에 클러스터 전체 할당.
	 * 값 범위: true / false.
	 * 동기화: 단일 스레드. */

	char *clear_method;
	/* [한국어] lvol 초기화 방법 문자열 (선택).
	 * 설정자: spdk_json_decode_string 이 JSON "clear_method" 에서 strdup. 미지정 시 NULL.
	 *   NULL 이면 핸들러가 LVOL_CLEAR_WITH_DEFAULT 적용 (lvs 기본 clear_method 사용).
	 *   가능한 값: "none", "unmap", "write_zeroes".
	 * 읽는 자: rpc_bdev_lvol_create 에서 enum lvol_clear_method 로 변환해 vbdev_lvol_create 전달.
	 * 해제: free_rpc_bdev_lvol_create 에서 free. */
};

/*
 * [한국어]
 * free_rpc_bdev_lvol_create - rpc_bdev_lvol_create 구조체 문자열 해제
 *
 * 호출 체인: rpc_bdev_lvol_create → [이 함수]
 */
static void
free_rpc_bdev_lvol_create(struct rpc_bdev_lvol_create *req)
{
	free(req->uuid);         /* [한국어] UUID 문자열 해제 */
	free(req->lvs_name);     /* [한국어] lvs 이름 문자열 해제 */
	free(req->lvol_name);    /* [한국어] lvol 이름 문자열 해제 */
	free(req->clear_method); /* [한국어] 초기화 방법 문자열 해제 */
}

/*
 * [한국어] rpc_bdev_lvol_create_decoders - JSON 파라미터 매핑 테이블 (create lvol)
 */
static const struct spdk_json_object_decoder rpc_bdev_lvol_create_decoders[] = {
	{"uuid", offsetof(struct rpc_bdev_lvol_create, uuid), spdk_json_decode_string, true},
	/* [한국어] "uuid" → uuid. 선택. 대상 lvs UUID. */
	{"lvs_name", offsetof(struct rpc_bdev_lvol_create, lvs_name), spdk_json_decode_string, true},
	/* [한국어] "lvs_name" → lvs_name. 선택. 대상 lvs 이름. */
	{"lvol_name", offsetof(struct rpc_bdev_lvol_create, lvol_name), spdk_json_decode_string},
	/* [한국어] "lvol_name" → lvol_name. 필수. 생성할 lvol 이름. */
	{"size_in_mib", offsetof(struct rpc_bdev_lvol_create, size_in_mib), spdk_json_decode_uint64},
	/* [한국어] "size_in_mib" → size_in_mib. 필수. lvol 논리 크기(MiB). */
	{"thin_provision", offsetof(struct rpc_bdev_lvol_create, thin_provision), spdk_json_decode_bool, true},
	/* [한국어] "thin_provision" → thin_provision. 선택. 기본 false (thick). */
	{"clear_method", offsetof(struct rpc_bdev_lvol_create, clear_method), spdk_json_decode_string, true},
	/* [한국어] "clear_method" → clear_method. 선택. 미지정 시 NULL → DEFAULT 적용. */
};

/*
 * [한국어]
 * rpc_bdev_lvol_create_cb - vbdev_lvol_create 비동기 완료 콜백
 *
 * @cb_arg:    원본 spdk_jsonrpc_request 포인터.
 * @lvol:      생성된 spdk_lvol 포인터. 실패 시 유효하지 않을 수 있음.
 * @lvolerrno: 생성 결과. 0 = 성공, 음수 = 실패 errno.
 * @return: 없음.
 *
 * 성공 시 lvol->unique_id (UUID 문자열 형태) 를 JSON string 으로 응답.
 * 실패 시 errno 메시지를 JSON-RPC error 로 응답.
 * unique_id 는 "lvs_uuid/lvol_uuid" 형태로 클라이언트가 이후 RPC 에서 참조할 수 있다.
 *
 * 호출 체인:
 *   vbdev_lvol_create (async) → [이 함수] → spdk_jsonrpc_end_result
 *                                          OR spdk_jsonrpc_send_error_response
 */
static void
rpc_bdev_lvol_create_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_json_write_ctx *w;                     /* [한국어] JSON 응답 직렬화 컨텍스트 */
	struct spdk_jsonrpc_request *request = cb_arg;      /* [한국어] 원본 RPC 요청 복원 */

	if (lvolerrno != 0) { /* [한국어] lvol 생성 실패 */
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);           /* [한국어] JSON 결과 응답 시작 */
	spdk_json_write_string(w, lvol->unique_id);        /* [한국어] 생성된 lvol 의 unique_id (UUID string) 직렬화 */
	spdk_jsonrpc_end_result(request, w);               /* [한국어] 응답 완료 — 클라이언트에 unique_id 전송 */
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
	/* [한국어] 생성 실패 → errno 메시지를 JSON-RPC error 로 전송 */
}

/*
 * [한국어]
 * rpc_bdev_lvol_create - "bdev_lvol_create" JSON-RPC 핸들러
 *
 * @request: JSON-RPC 요청 컨텍스트.
 * @params:  JSON-RPC params. lvol_name, size_in_mib 필수. uuid/lvs_name 은 XOR 로 하나 필요.
 * @return: 없음.
 *
 * 파라미터를 디코딩하고, lvol store 를 찾고, clear_method 를 변환한 뒤
 * vbdev_lvol_create 를 비동기 호출한다. 완료 시 lvol unique_id 를 응답.
 * size_in_mib 는 바이트로 변환(MiB → bytes) 후 vbdev_lvol_create 에 전달.
 * 실행 컨텍스트: SPDK reactor 스레드.
 *
 * 호출 체인:
 *   lib/rpc → [이 함수] → vbdev_get_lvol_store_by_uuid_xor_name
 *                       → vbdev_lvol_create
 *                       → (async) rpc_bdev_lvol_create_cb
 */
static void
rpc_bdev_lvol_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_create req = {};  /* [한국어] 파라미터 구조체 zero-초기화 */
	enum lvol_clear_method clear_method;   /* [한국어] 문자열에서 변환된 클리어 방법 enum */
	int rc = 0;                            /* [한국어] 함수 반환값 저장 */
	struct spdk_lvol_store *lvs = NULL;    /* [한국어] 탐색된 lvol store */

	SPDK_INFOLOG(lvol_rpc, "Creating blob\n"); /* [한국어] 디버그 로그: lvol(blob) 생성 시작 */

	if (spdk_json_decode_object(params, rpc_bdev_lvol_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_create_decoders),
				    &req)) {
		/* [한국어] JSON params 디코딩 실패 — 필수 파라미터 누락 등 */
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = vbdev_get_lvol_store_by_uuid_xor_name(req.uuid, req.lvs_name, &lvs);
	/* [한국어] uuid 또는 lvs_name 중 하나로 lvol store 탐색 */
	if (rc != 0) { /* [한국어] XOR 탐색 실패 */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	if (req.clear_method != NULL) { /* [한국어] clear_method 문자열이 명시된 경우 */
		if (!strcasecmp(req.clear_method, "none")) {
			clear_method = LVOL_CLEAR_WITH_NONE; /* [한국어] 초기화 없이 생성 */
		} else if (!strcasecmp(req.clear_method, "unmap")) {
			clear_method = LVOL_CLEAR_WITH_UNMAP; /* [한국어] UNMAP 으로 초기화 */
		} else if (!strcasecmp(req.clear_method, "write_zeroes")) {
			clear_method = LVOL_CLEAR_WITH_WRITE_ZEROES; /* [한국어] 제로 쓰기로 초기화 */
		} else {
			spdk_jsonrpc_send_error_response(request, -EINVAL, "Invalid clean_method option");
			goto cleanup; /* [한국어] 알 수 없는 clear_method — 즉시 에러 */
		}
	} else { /* [한국어] clear_method 미지정 — lvs 기본값 사용 */
		clear_method = LVOL_CLEAR_WITH_DEFAULT;
	}

	rc = vbdev_lvol_create(lvs, req.lvol_name, req.size_in_mib * 1024 * 1024,
			       req.thin_provision, clear_method, rpc_bdev_lvol_create_cb, request);
	/* [한국어] 비동기 lvol 생성 시작.
	 * size_in_mib * 1024 * 1024 로 MiB → bytes 변환.
	 * 내부에서 spdk_lvs_alloc_blob 으로 blob 생성 후 bdev 등록.
	 * 완료 시 rpc_bdev_lvol_create_cb 호출. */
	if (rc < 0) { /* [한국어] vbdev_lvol_create 즉시 실패 */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

cleanup:
	free_rpc_bdev_lvol_create(&req); /* [한국어] 성공/실패 공통 — 파라미터 문자열 해제 */
}

SPDK_RPC_REGISTER("bdev_lvol_create", rpc_bdev_lvol_create, SPDK_RPC_RUNTIME)
/* [한국어] "bdev_lvol_create" 메서드를 SPDK_RPC_RUNTIME 단계에 등록. */

/*
 * [한국어]
 * struct rpc_bdev_lvol_snapshot - "bdev_lvol_snapshot" RPC params 컨테이너
 *
 * 기존 lvol 의 read-only CoW(Copy-on-Write) 스냅샷을 생성하는 RPC 파라미터를 담는다.
 */
struct rpc_bdev_lvol_snapshot {
	char *lvol_name;
	/* [한국어] 스냅샷 원본이 될 lvol 의 bdev 이름 또는 alias (필수).
	 * 설정자: spdk_json_decode_string 이 JSON "lvol_name" 에서 strdup.
	 * 읽는 자: rpc_bdev_lvol_snapshot 에서 spdk_bdev_get_by_name 으로 bdev 탐색 시 사용.
	 * 값 범위: NULL 불가. "lvs_name/lvol_name" 형태의 alias 또는 unique_id 문자열.
	 * 해제: free_rpc_bdev_lvol_snapshot 에서 free. */

	char *snapshot_name;
	/* [한국어] 생성할 스냅샷의 이름 (필수).
	 * 설정자: spdk_json_decode_string 이 JSON "snapshot_name" 에서 strdup.
	 * 읽는 자: rpc_bdev_lvol_snapshot 에서 vbdev_lvol_create_snapshot 에 전달.
	 *   생성된 스냅샷 bdev 이름은 "lvs_name/snapshot_name" 형태.
	 * 값 범위: NULL 불가. 같은 lvs 내 중복 금지.
	 * 해제: free_rpc_bdev_lvol_snapshot 에서 free. */
};

/*
 * [한국어]
 * free_rpc_bdev_lvol_snapshot - 파라미터 구조체 문자열 해제
 *
 * 호출 체인: rpc_bdev_lvol_snapshot → [이 함수]
 */
static void
free_rpc_bdev_lvol_snapshot(struct rpc_bdev_lvol_snapshot *req)
{
	free(req->lvol_name);    /* [한국어] 원본 lvol bdev 이름 문자열 해제 */
	free(req->snapshot_name); /* [한국어] 스냅샷 이름 문자열 해제 */
}

/*
 * [한국어] rpc_bdev_lvol_snapshot_decoders - JSON 파라미터 매핑 테이블 (create snapshot)
 * lvol_name, snapshot_name 모두 필수 파라미터.
 */
static const struct spdk_json_object_decoder rpc_bdev_lvol_snapshot_decoders[] = {
	{"lvol_name", offsetof(struct rpc_bdev_lvol_snapshot, lvol_name), spdk_json_decode_string},
	/* [한국어] "lvol_name" → lvol_name. 필수. 스냅샷 대상 lvol 이름. */
	{"snapshot_name", offsetof(struct rpc_bdev_lvol_snapshot, snapshot_name), spdk_json_decode_string},
	/* [한국어] "snapshot_name" → snapshot_name. 필수. 생성할 스냅샷 이름. */
};

/*
 * [한국어]
 * rpc_bdev_lvol_snapshot_cb - vbdev_lvol_create_snapshot 비동기 완료 콜백
 *
 * @cb_arg:    원본 spdk_jsonrpc_request 포인터.
 * @lvol:      생성된 스냅샷 spdk_lvol 포인터.
 * @lvolerrno: 생성 결과. 0 = 성공, 음수 = 실패 errno.
 * @return: 없음.
 *
 * 성공 시 스냅샷 lvol 의 unique_id 를 JSON string 으로 응답.
 * 실패 시 errno 메시지를 JSON-RPC error 로 응답.
 *
 * 호출 체인:
 *   vbdev_lvol_create_snapshot (async) → [이 함수] → spdk_jsonrpc_end_result
 *                                                   OR spdk_jsonrpc_send_error_response
 */
static void
rpc_bdev_lvol_snapshot_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_json_write_ctx *w;                     /* [한국어] JSON 응답 컨텍스트 */
	struct spdk_jsonrpc_request *request = cb_arg;      /* [한국어] 원본 RPC 요청 복원 */

	if (lvolerrno != 0) { /* [한국어] 스냅샷 생성 실패 */
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);           /* [한국어] 응답 시작 */
	spdk_json_write_string(w, lvol->unique_id);        /* [한국어] 스냅샷 unique_id 직렬화 */
	spdk_jsonrpc_end_result(request, w);               /* [한국어] 응답 완료 */
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
	/* [한국어] 스냅샷 생성 실패 → errno 메시지로 에러 응답 */
}

/*
 * [한국어]
 * rpc_bdev_lvol_snapshot - "bdev_lvol_snapshot" JSON-RPC 핸들러
 *
 * @request: JSON-RPC 요청 컨텍스트.
 * @params:  JSON-RPC params. lvol_name, snapshot_name 필수.
 * @return: 없음.
 *
 * lvol_name 으로 bdev 를 찾고 vbdev_lvol_get_from_bdev 로 lvol 핸들을 얻은 뒤
 * vbdev_lvol_create_snapshot 을 비동기 호출한다.
 * 스냅샷: lvol 을 read-only 로 동결시키고 원본 lvol 은 clone 이 되어 CoW 방식으로 동작.
 * 에러: 디코딩 실패, bdev 없음, lvol 아님 시 즉시 에러 응답.
 * 실행 컨텍스트: SPDK reactor 스레드.
 *
 * 호출 체인:
 *   lib/rpc → [이 함수] → spdk_bdev_get_by_name
 *                       → vbdev_lvol_get_from_bdev
 *                       → vbdev_lvol_create_snapshot
 *                       → (async) rpc_bdev_lvol_snapshot_cb
 */
static void
rpc_bdev_lvol_snapshot(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_snapshot req = {}; /* [한국어] 파라미터 구조체 zero-초기화 */
	struct spdk_bdev *bdev;                  /* [한국어] 탐색된 bdev 포인터 */
	struct spdk_lvol *lvol;                  /* [한국어] bdev 에서 추출한 lvol 핸들 */

	SPDK_INFOLOG(lvol_rpc, "Snapshotting blob\n"); /* [한국어] 디버그 로그: 스냅샷 생성 시작 */

	if (spdk_json_decode_object(params, rpc_bdev_lvol_snapshot_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_snapshot_decoders),
				    &req)) {
		/* [한국어] JSON params 디코딩 실패 */
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.lvol_name);
	/* [한국어] bdev 레이어에서 이름으로 bdev 탐색 — lvol alias("lvs/lvol")나 unique_id 모두 가능 */
	if (bdev == NULL) { /* [한국어] 해당 이름의 bdev 없음 */
		SPDK_INFOLOG(lvol_rpc, "bdev '%s' does not exist\n", req.lvol_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	/* [한국어] bdev 가 lvol bdev 인지 확인하고 lvol 핸들을 추출.
	 * bdev 가 lvol 모듈에 속하지 않으면 NULL 반환. */
	if (lvol == NULL) { /* [한국어] lvol 이 아닌 bdev — 스냅샷 불가 */
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	vbdev_lvol_create_snapshot(lvol, req.snapshot_name, rpc_bdev_lvol_snapshot_cb, request);
	/* [한국어] 비동기 스냅샷 생성 시작.
	 * 내부에서 spdk_lvol_create_snapshot 을 호출해 blob 의 is_snapshot 플래그를 설정하고
	 * 원본 lvol 의 부모를 새 스냅샷으로 연결하는 CoW 체인을 구성.
	 * 완료 시 rpc_bdev_lvol_snapshot_cb 호출. */

cleanup:
	free_rpc_bdev_lvol_snapshot(&req); /* [한국어] 파라미터 문자열 해제 */
}

SPDK_RPC_REGISTER("bdev_lvol_snapshot", rpc_bdev_lvol_snapshot, SPDK_RPC_RUNTIME)
/* [한국어] "bdev_lvol_snapshot" 메서드를 SPDK_RPC_RUNTIME 단계에 등록. */

/*
 * [한국어]
 * struct rpc_bdev_lvol_clone - "bdev_lvol_clone" RPC params 컨테이너
 *
 * 동일 lvol store 내의 스냅샷 lvol 을 부모로 하는 CoW clone 을 생성하는 RPC 파라미터.
 * 외부 bdev(같은 lvs 소속이 아닌 bdev)로부터 clone 을 만들려면 bdev_lvol_clone_bdev 를 사용.
 */
struct rpc_bdev_lvol_clone {
	char *snapshot_name;
	/* [한국어] clone 의 부모가 될 스냅샷 lvol 의 bdev 이름 (필수).
	 * 설정자: spdk_json_decode_string 이 JSON "snapshot_name" 에서 strdup.
	 * 읽는 자: rpc_bdev_lvol_clone 에서 spdk_bdev_get_by_name → vbdev_lvol_get_from_bdev 로 lvol 취득.
	 *   반드시 is_snapshot 플래그가 설정된 read-only lvol 이어야 함 (lib/lvol 검증).
	 * 값 범위: NULL 불가. 등록된 스냅샷 bdev 이름.
	 * 해제: free_rpc_bdev_lvol_clone 에서 free. */

	char *clone_name;
	/* [한국어] 생성할 clone lvol 의 이름 (선택).
	 * 설정자: spdk_json_decode_string 이 JSON "clone_name" 에서 strdup. NULL 이면 자동 생성.
	 * 읽는 자: rpc_bdev_lvol_clone 에서 vbdev_lvol_create_clone 에 전달.
	 * 값 범위: NULL (자동) 또는 같은 lvs 내 중복되지 않는 이름.
	 * 해제: free_rpc_bdev_lvol_clone 에서 free. */
};

/*
 * [한국어]
 * free_rpc_bdev_lvol_clone - 파라미터 구조체 문자열 해제
 *
 * 호출 체인: rpc_bdev_lvol_clone / rpc_bdev_lvol_clone_bdev → [이 함수]
 */
static void
free_rpc_bdev_lvol_clone(struct rpc_bdev_lvol_clone *req)
{
	free(req->snapshot_name); /* [한국어] 스냅샷 bdev 이름 문자열 해제 */
	free(req->clone_name);    /* [한국어] clone 이름 문자열 해제 (NULL 이면 free(NULL) — 안전) */
}

/*
 * [한국어] rpc_bdev_lvol_clone_decoders - JSON 파라미터 매핑 테이블 (create clone)
 * snapshot_name 은 필수, clone_name 은 선택.
 */
static const struct spdk_json_object_decoder rpc_bdev_lvol_clone_decoders[] = {
	{"snapshot_name", offsetof(struct rpc_bdev_lvol_clone, snapshot_name), spdk_json_decode_string},
	/* [한국어] "snapshot_name" → snapshot_name. 필수. clone 부모 스냅샷 이름. */
	{"clone_name", offsetof(struct rpc_bdev_lvol_clone, clone_name), spdk_json_decode_string, true},
	/* [한국어] "clone_name" → clone_name. 선택. 미지정 시 NULL → lib/lvol 이 자동 생성. */
};

/*
 * [한국어]
 * rpc_bdev_lvol_clone_cb - vbdev_lvol_create_clone / vbdev_lvol_create_bdev_clone 비동기 완료 콜백
 *
 * @cb_arg:    원본 spdk_jsonrpc_request 포인터.
 * @lvol:      생성된 clone lvol 포인터.
 * @lvolerrno: 생성 결과. 0 = 성공, 음수 = 실패 errno.
 * @return: 없음.
 *
 * 성공 시 clone lvol 의 unique_id 를 JSON string 으로 응답.
 * 실패 시 errno 메시지를 JSON-RPC error 로 응답.
 * rpc_bdev_lvol_clone_bdev 도 동일한 콜백을 재사용한다.
 *
 * 호출 체인:
 *   vbdev_lvol_create_clone (async) → [이 함수]
 *   vbdev_lvol_create_bdev_clone (async) → [이 함수]
 */
static void
rpc_bdev_lvol_clone_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_json_write_ctx *w;                     /* [한국어] JSON 응답 컨텍스트 */
	struct spdk_jsonrpc_request *request = cb_arg;      /* [한국어] 원본 RPC 요청 복원 */

	if (lvolerrno != 0) { /* [한국어] clone 생성 실패 */
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);           /* [한국어] 응답 시작 */
	spdk_json_write_string(w, lvol->unique_id);        /* [한국어] 생성된 clone unique_id 직렬화 */
	spdk_jsonrpc_end_result(request, w);               /* [한국어] 응답 완료 */
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
	/* [한국어] clone 생성 실패 → errno 메시지로 에러 응답 */
}

/*
 * [한국어]
 * rpc_bdev_lvol_clone - "bdev_lvol_clone" JSON-RPC 핸들러
 *
 * @request: JSON-RPC 요청 컨텍스트.
 * @params:  JSON-RPC params. snapshot_name 필수, clone_name 선택.
 * @return: 없음.
 *
 * snapshot_name 으로 스냅샷 lvol 을 찾고 vbdev_lvol_create_clone 을 비동기 호출한다.
 * 생성된 clone 은 스냅샷을 공유 부모로 가지며 CoW 방식으로 독립적으로 쓰기를 수행한다.
 * 에러: 디코딩 실패, bdev 없음, lvol 아님 시 즉시 에러 응답.
 * 실행 컨텍스트: SPDK reactor 스레드.
 *
 * 호출 체인:
 *   lib/rpc → [이 함수] → spdk_bdev_get_by_name
 *                       → vbdev_lvol_get_from_bdev
 *                       → vbdev_lvol_create_clone
 *                       → (async) rpc_bdev_lvol_clone_cb
 */
static void
rpc_bdev_lvol_clone(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_clone req = {}; /* [한국어] 파라미터 구조체 zero-초기화 */
	struct spdk_bdev *bdev;               /* [한국어] 탐색된 스냅샷 bdev 포인터 */
	struct spdk_lvol *lvol;               /* [한국어] 스냅샷 lvol 핸들 */

	SPDK_INFOLOG(lvol_rpc, "Cloning blob\n"); /* [한국어] 디버그 로그: clone 생성 시작 */

	if (spdk_json_decode_object(params, rpc_bdev_lvol_clone_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_clone_decoders),
				    &req)) {
		/* [한국어] JSON params 디코딩 실패 */
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.snapshot_name);
	/* [한국어] 스냅샷 lvol bdev 이름으로 bdev 탐색 */
	if (bdev == NULL) { /* [한국어] 해당 이름의 bdev 없음 */
		SPDK_INFOLOG(lvol_rpc, "bdev '%s' does not exist\n", req.snapshot_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	/* [한국어] bdev 에서 lvol 핸들 추출 — lvol 모듈 소속 bdev 인지 확인 */
	if (lvol == NULL) { /* [한국어] lvol 이 아닌 bdev — clone 불가 */
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	vbdev_lvol_create_clone(lvol, req.clone_name, rpc_bdev_lvol_clone_cb, request);
	/* [한국어] 비동기 clone 생성 시작. lvol 은 스냅샷 부모.
	 * 내부에서 spdk_lvol_create_clone 으로 blob CoW 관계를 설정하고 bdev 등록.
	 * 완료 시 rpc_bdev_lvol_clone_cb 호출. */

cleanup:
	free_rpc_bdev_lvol_clone(&req); /* [한국어] 파라미터 문자열 해제 */
}

SPDK_RPC_REGISTER("bdev_lvol_clone", rpc_bdev_lvol_clone, SPDK_RPC_RUNTIME)
/* [한국어] "bdev_lvol_clone" 메서드를 SPDK_RPC_RUNTIME 단계에 등록. */

struct rpc_bdev_lvol_clone_bdev {
	/* name or UUID. Whichever is used, the UUID will be stored in the lvol's metadata. */
	char *bdev_name;
	char *lvs_name;
	char *clone_name;
};

static void
free_rpc_bdev_lvol_clone_bdev(struct rpc_bdev_lvol_clone_bdev *req)
{
	free(req->bdev_name);
	free(req->lvs_name);
	free(req->clone_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_clone_bdev_decoders[] = {
	{
		"bdev", offsetof(struct rpc_bdev_lvol_clone_bdev, bdev_name),
		spdk_json_decode_string, false
	},
	{
		"lvs_name", offsetof(struct rpc_bdev_lvol_clone_bdev, lvs_name),
		spdk_json_decode_string, false
	},
	{
		"clone_name", offsetof(struct rpc_bdev_lvol_clone_bdev, clone_name),
		spdk_json_decode_string, false
	},
};

static void
rpc_bdev_lvol_clone_bdev(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_clone_bdev req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol_store *lvs = NULL;
	struct spdk_lvol *lvol;
	int rc;

	SPDK_INFOLOG(lvol_rpc, "Cloning bdev\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_clone_bdev_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_clone_bdev_decoders), &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = vbdev_get_lvol_store_by_uuid_xor_name(NULL, req.lvs_name, &lvs);
	if (rc != 0) {
		SPDK_INFOLOG(lvol_rpc, "lvs_name '%s' not found\n", req.lvs_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "lvs does not exist");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.bdev_name);
	if (bdev == NULL) {
		SPDK_INFOLOG(lvol_rpc, "bdev '%s' does not exist\n", req.bdev_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "bdev does not exist");
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol != NULL && lvol->lvol_store == lvs) {
		SPDK_INFOLOG(lvol_rpc, "bdev '%s' is an lvol in lvstore '%s\n", req.bdev_name,
			     req.lvs_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "bdev is an lvol in same lvs as clone; "
						 "use bdev_lvol_clone instead");
		goto cleanup;
	}

	vbdev_lvol_create_bdev_clone(req.bdev_name, lvs, req.clone_name,
				     rpc_bdev_lvol_clone_cb, request);
cleanup:
	free_rpc_bdev_lvol_clone_bdev(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_clone_bdev", rpc_bdev_lvol_clone_bdev, SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_rename {
	char *old_name;
	char *new_name;
};

static void
free_rpc_bdev_lvol_rename(struct rpc_bdev_lvol_rename *req)
{
	free(req->old_name);
	free(req->new_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_rename_decoders[] = {
	{"old_name", offsetof(struct rpc_bdev_lvol_rename, old_name), spdk_json_decode_string},
	{"new_name", offsetof(struct rpc_bdev_lvol_rename, new_name), spdk_json_decode_string},
};

static void
rpc_bdev_lvol_rename_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_rename(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_rename req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	SPDK_INFOLOG(lvol_rpc, "Renaming lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_rename_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_rename_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.old_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.old_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	vbdev_lvol_rename(lvol, req.new_name, rpc_bdev_lvol_rename_cb, request);

cleanup:
	free_rpc_bdev_lvol_rename(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_rename", rpc_bdev_lvol_rename, SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_inflate {
	char *name;
};

static void
free_rpc_bdev_lvol_inflate(struct rpc_bdev_lvol_inflate *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_inflate_decoders[] = {
	{"name", offsetof(struct rpc_bdev_lvol_inflate, name), spdk_json_decode_string},
};

static void
rpc_bdev_lvol_inflate_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_inflate(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_inflate req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	SPDK_INFOLOG(lvol_rpc, "Inflating lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_inflate_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_inflate_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	spdk_lvol_inflate(lvol, rpc_bdev_lvol_inflate_cb, request);

cleanup:
	free_rpc_bdev_lvol_inflate(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_inflate", rpc_bdev_lvol_inflate, SPDK_RPC_RUNTIME)

static void
rpc_bdev_lvol_decouple_parent(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_inflate req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	SPDK_INFOLOG(lvol_rpc, "Decoupling parent of lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_inflate_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_inflate_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	spdk_lvol_decouple_parent(lvol, rpc_bdev_lvol_inflate_cb, request);

cleanup:
	free_rpc_bdev_lvol_inflate(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_decouple_parent", rpc_bdev_lvol_decouple_parent, SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_resize {
	char *name;
	uint64_t size_in_mib;
};

static void
free_rpc_bdev_lvol_resize(struct rpc_bdev_lvol_resize *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_resize_decoders[] = {
	{"name", offsetof(struct rpc_bdev_lvol_resize, name), spdk_json_decode_string},
	{"size_in_mib", offsetof(struct rpc_bdev_lvol_resize, size_in_mib), spdk_json_decode_uint64},
};

static void
rpc_bdev_lvol_resize_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_resize(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_resize req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	SPDK_INFOLOG(lvol_rpc, "Resizing lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_resize_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_resize_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("no bdev for provided name %s\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}


	vbdev_lvol_resize(lvol, req.size_in_mib * 1024 * 1024, rpc_bdev_lvol_resize_cb, request);

cleanup:
	free_rpc_bdev_lvol_resize(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_resize", rpc_bdev_lvol_resize, SPDK_RPC_RUNTIME)

struct rpc_set_ro_lvol_bdev {
	char *name;
};

static void
free_rpc_set_ro_lvol_bdev(struct rpc_set_ro_lvol_bdev *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_set_read_only_decoders[] = {
	{"name", offsetof(struct rpc_set_ro_lvol_bdev, name), spdk_json_decode_string},
};

static void
rpc_set_ro_lvol_bdev_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_set_read_only(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_set_ro_lvol_bdev req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	SPDK_INFOLOG(lvol_rpc, "Setting lvol as read only\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_set_read_only_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_set_read_only_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req.name == NULL) {
		SPDK_ERRLOG("missing name param\n");
		spdk_jsonrpc_send_error_response(request, -EINVAL, "Missing name parameter");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("no bdev for provided name %s\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	vbdev_lvol_set_read_only(lvol, rpc_set_ro_lvol_bdev_cb, request);

cleanup:
	free_rpc_set_ro_lvol_bdev(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_set_read_only", rpc_bdev_lvol_set_read_only, SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_delete {
	char *name;
};

static void
free_rpc_bdev_lvol_delete(struct rpc_bdev_lvol_delete *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_lvol_delete, name), spdk_json_decode_string},
};

static void
rpc_bdev_lvol_delete_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_delete req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;
	struct spdk_uuid uuid;
	char *lvs_name, *lvol_name;

	if (spdk_json_decode_object(params, rpc_bdev_lvol_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_delete_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* lvol is not degraded, get lvol via bdev name or alias */
	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev != NULL) {
		lvol = vbdev_lvol_get_from_bdev(bdev);
		if (lvol != NULL) {
			goto done;
		}
	}

	/* lvol is degraded, get lvol via UUID */
	if (spdk_uuid_parse(&uuid, req.name) == 0) {
		lvol = spdk_lvol_get_by_uuid(&uuid);
		if (lvol != NULL) {
			goto done;
		}
	}

	/* lvol is degraded, get lvol via lvs_name/lvol_name */
	lvol_name = strchr(req.name, '/');
	if (lvol_name != NULL) {
		*lvol_name = '\0';
		lvol_name++;
		lvs_name = req.name;
		lvol = spdk_lvol_get_by_names(lvs_name, lvol_name);
		if (lvol != NULL) {
			goto done;
		}
	}

	/* Could not find lvol, degraded or not. */
	spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
	goto cleanup;

done:
	vbdev_lvol_destroy(lvol, rpc_bdev_lvol_delete_cb, request);

cleanup:
	free_rpc_bdev_lvol_delete(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_delete", rpc_bdev_lvol_delete, SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_get_lvstores {
	char *uuid;
	char *lvs_name;
};

static void
free_rpc_bdev_lvol_get_lvstores(struct rpc_bdev_lvol_get_lvstores *req)
{
	free(req->uuid);
	free(req->lvs_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_get_lvstores_decoders[] = {
	{"uuid", offsetof(struct rpc_bdev_lvol_get_lvstores, uuid), spdk_json_decode_string, true},
	{"lvs_name", offsetof(struct rpc_bdev_lvol_get_lvstores, lvs_name), spdk_json_decode_string, true},
};

static void
rpc_dump_lvol_store_info(struct spdk_json_write_ctx *w, struct lvol_store_bdev *lvs_bdev)
{
	struct spdk_blob_store *bs;
	uint64_t cluster_size;

	bs = lvs_bdev->lvs->blobstore;
	cluster_size = spdk_bs_get_cluster_size(bs);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uuid(w, "uuid", &lvs_bdev->lvs->uuid);
	spdk_json_write_named_string(w, "name", lvs_bdev->lvs->name);
	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(lvs_bdev->bdev));
	spdk_json_write_named_uint64(w, "total_data_clusters", spdk_bs_total_data_cluster_count(bs));
	spdk_json_write_named_uint64(w, "free_clusters", spdk_bs_free_cluster_count(bs));
	spdk_json_write_named_uint64(w, "block_size", spdk_bs_get_io_unit_size(bs));
	spdk_json_write_named_uint64(w, "cluster_size", cluster_size);
	spdk_json_write_named_uint64(w, "max_growable_size", spdk_bs_get_max_growable_size(bs));

	spdk_json_write_object_end(w);
}

static void
rpc_bdev_lvol_get_lvstores(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_get_lvstores req = {};
	struct spdk_json_write_ctx *w;
	struct lvol_store_bdev *lvs_bdev = NULL;
	struct spdk_lvol_store *lvs = NULL;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_bdev_lvol_get_lvstores_decoders,
					    SPDK_COUNTOF(rpc_bdev_lvol_get_lvstores_decoders),
					    &req)) {
			SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			goto cleanup;
		}

		rc = vbdev_get_lvol_store_by_uuid_xor_name(req.uuid, req.lvs_name, &lvs);
		if (rc != 0) {
			spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
			goto cleanup;
		}

		lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvs);
		if (lvs_bdev == NULL) {
			spdk_jsonrpc_send_error_response(request, ENODEV, spdk_strerror(-ENODEV));
			goto cleanup;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (lvs_bdev != NULL) {
		rpc_dump_lvol_store_info(w, lvs_bdev);
	} else {
		for (lvs_bdev = vbdev_lvol_store_first(); lvs_bdev != NULL;
		     lvs_bdev = vbdev_lvol_store_next(lvs_bdev)) {
			rpc_dump_lvol_store_info(w, lvs_bdev);
		}
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_lvol_get_lvstores(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_get_lvstores", rpc_bdev_lvol_get_lvstores, SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_get_lvols {
	char *lvs_uuid;
	char *lvs_name;
};

static void
free_rpc_bdev_lvol_get_lvols(struct rpc_bdev_lvol_get_lvols *req)
{
	free(req->lvs_uuid);
	free(req->lvs_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_get_lvols_decoders[] = {
	{"lvs_uuid", offsetof(struct rpc_bdev_lvol_get_lvols, lvs_uuid), spdk_json_decode_string, true},
	{"lvs_name", offsetof(struct rpc_bdev_lvol_get_lvols, lvs_name), spdk_json_decode_string, true},
};

static void
rpc_dump_lvol(struct spdk_json_write_ctx *w, struct spdk_lvol *lvol)
{
	struct spdk_lvol_store *lvs = lvol->lvol_store;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string_fmt(w, "alias", "%s/%s", lvs->name, lvol->name);
	spdk_json_write_named_string(w, "uuid", lvol->uuid_str);
	spdk_json_write_named_string(w, "name", lvol->name);
	spdk_json_write_named_bool(w, "is_thin_provisioned", spdk_blob_is_thin_provisioned(lvol->blob));
	spdk_json_write_named_bool(w, "is_snapshot", spdk_blob_is_snapshot(lvol->blob));
	spdk_json_write_named_bool(w, "is_clone", spdk_blob_is_clone(lvol->blob));
	spdk_json_write_named_bool(w, "is_esnap_clone", spdk_blob_is_esnap_clone(lvol->blob));
	spdk_json_write_named_bool(w, "is_degraded", spdk_blob_is_degraded(lvol->blob));
	spdk_json_write_named_uint64(w, "num_allocated_clusters",
				     spdk_blob_get_num_allocated_clusters(lvol->blob));

	spdk_json_write_named_object_begin(w, "lvs");
	spdk_json_write_named_string(w, "name", lvs->name);
	spdk_json_write_named_uuid(w, "uuid", &lvs->uuid);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
rpc_dump_lvols(struct spdk_json_write_ctx *w, struct lvol_store_bdev *lvs_bdev)
{
	struct spdk_lvol_store *lvs = lvs_bdev->lvs;
	struct spdk_lvol *lvol;

	TAILQ_FOREACH(lvol, &lvs->lvols, link) {
		if (lvol->ref_count == 0) {
			continue;
		}
		rpc_dump_lvol(w, lvol);
	}
}

static void
rpc_bdev_lvol_get_lvols(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_get_lvols req = {};
	struct spdk_json_write_ctx *w;
	struct lvol_store_bdev *lvs_bdev = NULL;
	struct spdk_lvol_store *lvs = NULL;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_bdev_lvol_get_lvols_decoders,
					    SPDK_COUNTOF(rpc_bdev_lvol_get_lvols_decoders),
					    &req)) {
			SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			goto cleanup;
		}

		rc = vbdev_get_lvol_store_by_uuid_xor_name(req.lvs_uuid, req.lvs_name, &lvs);
		if (rc != 0) {
			spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
			goto cleanup;
		}

		lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvs);
		if (lvs_bdev == NULL) {
			spdk_jsonrpc_send_error_response(request, ENODEV, spdk_strerror(-ENODEV));
			goto cleanup;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (lvs_bdev != NULL) {
		rpc_dump_lvols(w, lvs_bdev);
	} else {
		for (lvs_bdev = vbdev_lvol_store_first(); lvs_bdev != NULL;
		     lvs_bdev = vbdev_lvol_store_next(lvs_bdev)) {
			rpc_dump_lvols(w, lvs_bdev);
		}
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_lvol_get_lvols(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_get_lvols", rpc_bdev_lvol_get_lvols, SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_grow_lvstore {
	char *uuid;
	char *lvs_name;
};

static void
free_rpc_bdev_lvol_grow_lvstore(struct rpc_bdev_lvol_grow_lvstore *req)
{
	free(req->uuid);
	free(req->lvs_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_grow_lvstore_decoders[] = {
	{"uuid", offsetof(struct rpc_bdev_lvol_grow_lvstore, uuid), spdk_json_decode_string, true},
	{"lvs_name", offsetof(struct rpc_bdev_lvol_grow_lvstore, lvs_name), spdk_json_decode_string, true},
};

static void
rpc_bdev_lvol_grow_lvstore_cb(void *cb_arg, int lvserrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvserrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvserrno));
}

static void
rpc_bdev_lvol_grow_lvstore(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_grow_lvstore req = {};
	struct spdk_lvol_store *lvs = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_lvol_grow_lvstore_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_grow_lvstore_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = vbdev_get_lvol_store_by_uuid_xor_name(req.uuid, req.lvs_name, &lvs);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}
	spdk_bdev_update_bs_blockcnt(lvs->bs_dev);
	spdk_lvs_grow_live(lvs, rpc_bdev_lvol_grow_lvstore_cb, request);

cleanup:
	free_rpc_bdev_lvol_grow_lvstore(&req);
}
SPDK_RPC_REGISTER("bdev_lvol_grow_lvstore", rpc_bdev_lvol_grow_lvstore, SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_shallow_copy {
	char *src_lvol_name;
	char *dst_bdev_name;
};

struct rpc_bdev_lvol_shallow_copy_ctx {
	struct spdk_jsonrpc_request *request;
	struct rpc_shallow_copy_status *status;
};

static void
free_rpc_bdev_lvol_shallow_copy(struct rpc_bdev_lvol_shallow_copy *req)
{
	free(req->src_lvol_name);
	free(req->dst_bdev_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_start_shallow_copy_decoders[] = {
	{"src_lvol_name", offsetof(struct rpc_bdev_lvol_shallow_copy, src_lvol_name), spdk_json_decode_string},
	{"dst_bdev_name", offsetof(struct rpc_bdev_lvol_shallow_copy, dst_bdev_name), spdk_json_decode_string},
};

static void
rpc_bdev_lvol_shallow_copy_cb(void *cb_arg, int lvolerrno)
{
	struct rpc_bdev_lvol_shallow_copy_ctx *ctx = cb_arg;

	ctx->status->result = lvolerrno;

	free(ctx);
}

static void
rpc_bdev_lvol_shallow_copy_status_cb(uint64_t copied_clusters, void *cb_arg)
{
	struct rpc_shallow_copy_status *status = cb_arg;

	status->copied_clusters = copied_clusters;
}

static void
rpc_bdev_lvol_start_shallow_copy(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_shallow_copy req = {};
	struct rpc_bdev_lvol_shallow_copy_ctx *ctx;
	struct spdk_lvol *src_lvol;
	struct spdk_bdev *src_lvol_bdev;
	struct rpc_shallow_copy_status *status;
	struct spdk_json_write_ctx *w;
	int rc;

	SPDK_INFOLOG(lvol_rpc, "Shallow copying lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_start_shallow_copy_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_start_shallow_copy_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	src_lvol_bdev = spdk_bdev_get_by_name(req.src_lvol_name);
	if (src_lvol_bdev == NULL) {
		SPDK_ERRLOG("lvol bdev '%s' does not exist\n", req.src_lvol_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	src_lvol = vbdev_lvol_get_from_bdev(src_lvol_bdev);
	if (src_lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	status = calloc(1, sizeof(*status));
	if (status == NULL) {
		SPDK_ERRLOG("Cannot allocate status entry for shallow copy of '%s'\n", req.src_lvol_name);
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto cleanup;
	}

	status->operation_id = ++g_shallow_copy_count;
	status->total_clusters = spdk_blob_get_num_allocated_clusters(src_lvol->blob);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Cannot allocate context for shallow copy of '%s'\n", req.src_lvol_name);
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		free(status);
		goto cleanup;
	}
	ctx->request = request;
	ctx->status = status;

	LIST_INSERT_HEAD(&g_shallow_copy_status_list, status, link);
	rc = vbdev_lvol_shallow_copy(src_lvol, req.dst_bdev_name,
				     rpc_bdev_lvol_shallow_copy_status_cb, status,
				     rpc_bdev_lvol_shallow_copy_cb, ctx);

	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-rc));
		LIST_REMOVE(status, link);
		free(ctx);
		free(status);
	} else {
		w = spdk_jsonrpc_begin_result(request);

		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint32(w, "operation_id", status->operation_id);
		spdk_json_write_object_end(w);

		spdk_jsonrpc_end_result(request, w);
	}

cleanup:
	free_rpc_bdev_lvol_shallow_copy(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_start_shallow_copy", rpc_bdev_lvol_start_shallow_copy,
		  SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_shallow_copy_status {
	char		*src_lvol_name;
	uint32_t	operation_id;
};

static void
free_rpc_bdev_lvol_shallow_copy_status(struct rpc_bdev_lvol_shallow_copy_status *req)
{
	free(req->src_lvol_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_check_shallow_copy_decoders[] = {
	{"operation_id", offsetof(struct rpc_bdev_lvol_shallow_copy_status, operation_id), spdk_json_decode_uint32},
};

static void
rpc_bdev_lvol_check_shallow_copy(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_shallow_copy_status req = {};
	struct rpc_shallow_copy_status *status;
	struct spdk_json_write_ctx *w;
	uint64_t copied_clusters, total_clusters;
	int result;

	SPDK_INFOLOG(lvol_rpc, "Shallow copy check\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_check_shallow_copy_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_check_shallow_copy_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	LIST_FOREACH(status, &g_shallow_copy_status_list, link) {
		if (status->operation_id == req.operation_id) {
			break;
		}
	}

	if (!status) {
		SPDK_ERRLOG("operation id '%d' does not exist\n", req.operation_id);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	copied_clusters = status->copied_clusters;
	total_clusters = status->total_clusters;
	result = status->result;

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint64(w, "copied_clusters", copied_clusters);
	spdk_json_write_named_uint64(w, "total_clusters", total_clusters);
	if (copied_clusters < total_clusters && result == 0) {
		spdk_json_write_named_string(w, "state", "in progress");
	} else if (copied_clusters == total_clusters && result == 0) {
		spdk_json_write_named_string(w, "state", "complete");
		LIST_REMOVE(status, link);
		free(status);
	} else {
		spdk_json_write_named_string(w, "state", "error");
		spdk_json_write_named_string(w, "error", spdk_strerror(-result));
		LIST_REMOVE(status, link);
		free(status);
	}

	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_lvol_shallow_copy_status(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_check_shallow_copy", rpc_bdev_lvol_check_shallow_copy,
		  SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_set_parent {
	char *lvol_name;
	char *parent_name;
};

static void
free_rpc_bdev_lvol_set_parent(struct rpc_bdev_lvol_set_parent *req)
{
	free(req->lvol_name);
	free(req->parent_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_set_parent_decoders[] = {
	{"lvol_name", offsetof(struct rpc_bdev_lvol_set_parent, lvol_name), spdk_json_decode_string},
	{"parent_name", offsetof(struct rpc_bdev_lvol_set_parent, parent_name), spdk_json_decode_string},
};

static void
rpc_bdev_lvol_set_parent_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_set_parent(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_set_parent req = {};
	struct spdk_lvol *lvol, *snapshot;
	struct spdk_bdev *lvol_bdev, *snapshot_bdev;

	SPDK_INFOLOG(lvol_rpc, "Set parent of lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_set_parent_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_set_parent_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	lvol_bdev = spdk_bdev_get_by_name(req.lvol_name);
	if (lvol_bdev == NULL) {
		SPDK_ERRLOG("lvol bdev '%s' does not exist\n", req.lvol_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(lvol_bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	snapshot_bdev = spdk_bdev_get_by_name(req.parent_name);
	if (snapshot_bdev == NULL) {
		SPDK_ERRLOG("snapshot bdev '%s' does not exist\n", req.parent_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	snapshot = vbdev_lvol_get_from_bdev(snapshot_bdev);
	if (snapshot == NULL) {
		SPDK_ERRLOG("snapshot does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	spdk_lvol_set_parent(lvol, snapshot, rpc_bdev_lvol_set_parent_cb, request);

cleanup:
	free_rpc_bdev_lvol_set_parent(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_set_parent", rpc_bdev_lvol_set_parent, SPDK_RPC_RUNTIME)

static void
rpc_bdev_lvol_set_parent_bdev(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_set_parent req = {};
	struct spdk_lvol *lvol;
	struct spdk_bdev *lvol_bdev;

	SPDK_INFOLOG(lvol_rpc, "Set external parent of lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_set_parent_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_set_parent_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	lvol_bdev = spdk_bdev_get_by_name(req.lvol_name);
	if (lvol_bdev == NULL) {
		SPDK_ERRLOG("lvol bdev '%s' does not exist\n", req.lvol_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(lvol_bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	vbdev_lvol_set_external_parent(lvol, req.parent_name, rpc_bdev_lvol_set_parent_cb, request);

cleanup:
	free_rpc_bdev_lvol_set_parent(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_set_parent_bdev", rpc_bdev_lvol_set_parent_bdev,
		  SPDK_RPC_RUNTIME)
