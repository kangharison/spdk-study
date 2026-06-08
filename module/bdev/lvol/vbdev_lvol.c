/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] Logical Volume bdev 모듈 코어 구현 (vbdev_lvol.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 의 logical volume(lvol) 을 일반 bdev 인터페이스로 노출(virtual bdev,
 * vbdev) 하는 어댑터 모듈을 구현한다. 내부적으로 lvol 자체는 lib/lvol 의
 * spdk_lvol_store/spdk_lvol API 가 관리하며, 이는 다시 lib/blob 의 blobstore/blob 위에
 * 얇게 얹힌 의미론적 wrapper 다. 즉 데이터 경로는 "vbdev_lvol → spdk_lvol(=spdk_blob) →
 * blobstore → 베이스 spdk_bs_dev(=base bdev) → 실제 NVMe/AIO bdev" 로 흐른다.
 * 본 파일은 (1) lvol store 생성/로드/언로드/삭제, (2) 개별 lvol 의 생성·삭제·resize·rename·
 * snapshot·clone·external-snapshot(esnap) 등 control plane, (3) 각 lvol 을 spdk_bdev 로 등록
 * 하여 bdev_io 가 들어왔을 때 spdk_blob_io_*() 로 위임하는 data plane, 세 가지를 함께 처리한다.
 * thin/thick provisioning, snapshot tree, CoW, external snapshot 등은 모두 blob 레이어가
 * 책임지며 이 모듈은 그 결과를 bdev 의미론(블록 길이/uuid/alias/optimal_io_boundary)으로
 * 변환만 한다는 점이 핵심이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK bdev 스택의 "vbdev 계층"에 속한다. spdk_bdev_module 인터페이스를 통해
 * SPDK_BDEV_MODULE_REGISTER 매크로로 등록되어, generic bdev 초기화 시점에 (1) module_init →
 * vbdev_lvs_init, (2) 베이스 bdev 가 등장할 때마다 examine_config / examine_disk 콜백이
 * 호출된다. examine_disk 에서는 해당 베이스 bdev 위에 lvol store(=blobstore + lvol 메타)가
 * 있는지 disk 헤더를 읽어 자동 탐지(load)하고, 발견되면 모든 lvol 을 열어 각각 자기 자신을
 * spdk_bdev 로 등록한다. 그 결과 외부에서는 일반 bdev 와 동일하게 spdk_bdev_open/read/write
 * 할 수 있는 "Logical Volume" 제품 이름을 가진 가상 bdev 가 나타난다. RPC 핸들러(같은
 * 디렉터리의 vbdev_lvol_rpc.c)는 본 파일의 public API(vbdev_lvs_create/vbdev_lvol_create
 * /vbdev_lvol_destroy/...) 를 호출하여 사용자가 동적으로 lvol 을 만들고 지울 수 있게 한다.
 *
 * === 타 모듈과의 연결 ===
 * (a) lib/lvol (spdk_lvol_*): lvol 생성/destroy/snapshot/clone/resize 의 실제 로직을 가진
 *     상위 도메인 API. 본 파일은 ctx + 콜백을 묶어 그쪽으로 전달한다.
 * (b) lib/blob (spdk_blob_*): I/O 의 실제 실행 주체. 본 파일의 lvol_read/lvol_write/
 *     lvol_unmap/lvol_write_zeroes 가 spdk_blob_io_*() 로 위임된다. snapshot/clone 의
 *     CoW(Copy-on-Write), thin allocation 도 모두 blob 레이어가 수행.
 * (c) lib/bdev (spdk_bdev_*): 노출 경로. lvol 마다 spdk_bdev_register 로 등록, alias 추가,
 *     submit_request/io_type_supported/destruct 등 fn_table 콜백 제공. 베이스 bdev 는
 *     spdk_bdev_create_bs_dev_ext()/spdk_bs_bdev_claim() 으로 점유한다.
 * (d) module/bdev/* (밑에 깔리는 다른 bdev): NVMe/AIO/malloc 등 — examine 단계에서 베이스
 *     bdev 로 발견되며 lvol store 의 백킹이 된다. ESnap(external snapshot)의 read-only
 *     원본 bdev 도 같은 경로로 열린다.
 * (e) JSON-RPC (vbdev_lvol_rpc.c): control plane 진입점. 사용자 명령을 받아 본 파일의
 *     public API 호출 → 비동기 콜백으로 결과 반환.
 *
 * === 주요 함수/구조체 요약 ===
 * - vbdev_lvs_create_ext / vbdev_lvs_destruct / vbdev_lvs_unload / vbdev_lvs_rename:
 *     lvol store 라이프사이클 control plane.
 * - vbdev_lvol_create / _snapshot / _clone / _bdev_clone(esnap) / _resize / _rename /
 *     _set_read_only / _destroy / _shallow_copy / _set_external_parent: 개별 lvol control plane.
 * - vbdev_lvs_examine_config / vbdev_lvs_examine_disk: 베이스 bdev 등장 시 lvol store 자동
 *     탐지·로드 진입점 (bdev framework 가 호출).
 * - _create_lvol_disk: 열린 spdk_lvol 하나를 spdk_bdev 로 변환·등록하는 핵심 함수.
 * - vbdev_lvol_submit_request: bdev_io 를 받아 read/write/unmap/write_zeroes/seek 등
 *     IO 타입별로 spdk_blob_io_*() 에 위임하는 data plane 진입점.
 * - vbdev_lvol_esnap_dev_create: external snapshot(=다른 read-only bdev 를 부모로 한 thin
 *     clone) 의 부모 bs_dev 를 만든다. 부모 bdev 가 없으면 degraded 더미 bs_dev 로 대체.
 * - bs_dev_degraded: 부모 bdev 미발견 시 사용하는 더미 spdk_bs_dev — 모든 IO 가 assert/-EIO.
 * - struct lvol_store_bdev (vbdev_lvol.h): (lvol store ↔ 베이스 bdev) 한 쌍. 전역
 *     g_spdk_lvol_pairs TAILQ 가 모든 페어를 관리.
 * - struct lvol_bdev (vbdev_lvol.h): (spdk_bdev embed + 대응 spdk_lvol + 페어 백포인터)
 *     하나의 lvol 을 bdev 로 노출하는 컨테이너.
 * - struct vbdev_lvol_io: bdev_io->driver_ctx 슬롯에 저장되는 IO 별 컨텍스트
 *     (memory_domain 전달용).
 */

#include "spdk/rpc.h"
/* [한국어] JSON-RPC 서버 헤더 — RPC 등록/응답 함수 정의. 본 파일은 직접 RPC 를 등록하지는
 * 않지만 헤더 상수/타입을 일부 사용 가능 (RPC 핸들러 파일과의 ABI 공유) */
#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈/내부 API — spdk_bdev_module / SPDK_BDEV_MODULE_REGISTER /
 * spdk_bdev_register / spdk_bdev_alias_add / spdk_bdev_notify_blockcnt_change /
 * spdk_bdev_destruct_done / spdk_bdev_module_fini_start_done / examine_done 등
 * vbdev 가 bdev 코어에 자기 자신을 노출하고 라이프사이클 콜백을 등록하는 데 필요 */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG / SPDK_NOTICELOG / SPDK_INFOLOG / SPDK_DEBUGLOG / SPDK_WARNLOG /
 * SPDK_LOG_REGISTER_COMPONENT 매크로 — 컴포넌트별 로그 채널(vbdev_lvol)을 끝에서 등록 */
#include "spdk/string.h"
/* [한국어] spdk_sprintf_alloc / spdk_strerror — 동적 문자열 할당과 errno→메시지 변환에 사용 */
#include "spdk/uuid.h"
/* [한국어] spdk_uuid / spdk_uuid_parse / spdk_uuid_fmt_lower / spdk_uuid_compare —
 * lvol/lvol store UUID 처리 및 esnap 부모 bdev 의 UUID 기반 lookup */
#include "spdk/blob.h"
/* [한국어] Blob/Blobstore 공개 API — spdk_blob_io_readv_ext/writev_ext/unmap/write_zeroes,
 * spdk_blob_is_thin_provisioned/is_snapshot/is_clone/is_esnap_clone/is_read_only,
 * spdk_blob_get_num_allocated_clusters/get_num_clusters/get_clones/get_parent_snapshot,
 * spdk_blob_get_esnap_id/get_esnap_bs_dev, spdk_blob_get_next_allocated_io_unit 등.
 * lvol 의 data plane 과 metadata 조회는 모두 이 API 를 통해 진행 */

#include "vbdev_lvol.h"
/* [한국어] 모듈 내부 헤더 — struct lvol_store_bdev / struct lvol_bdev 정의와
 * 본 파일이 공개하는 vbdev_lvs_*/vbdev_lvol_* 프로토타입 선언 */

/* [한국어] === bdev_io 마다 따라다니는 모듈 전용 컨텍스트 ===
 * bdev 코어는 bdev_io 를 alloc 할 때 모듈이 등록한 get_ctx_size() 만큼의 여유 바이트를
 * bdev_io->driver_ctx 슬롯 뒤에 배치한다. 본 구조체가 그 슬롯의 실제 타입이다. */
struct vbdev_lvol_io {
	struct spdk_blob_ext_io_opts ext_io_opts;
	/* [한국어] spdk_blob_io_readv_ext/writev_ext 에 넘길 확장 옵션 컨테이너.
	 * 설정자: lvol_read() / lvol_write() 가 bdev_io 제출 직전에 size/memory_domain/
	 *         memory_domain_ctx 를 채워 넣는다 (다른 필드는 zero-initialized).
	 * 읽는 자: 하위 blob 레이어가 memory_domain 인지하에 DMA 영역을 결정.
	 * 값 범위: size 는 항상 sizeof(ext_io_opts), memory_domain 은 NULL 또는 호스트가
	 *         지정한 유효 도메인 핸들.
	 * 동기화: bdev_io 와 함께 single-thread 소유이므로 별도 락 불필요. */
};

/* [한국어] 전역 g_spdk_lvol_pairs:
 * 시스템에 로드되어 있는 모든 (lvol_store, 베이스 bdev) 쌍을 묶는 TAILQ 헤드.
 * vbdev_lvs_examine 시 새로 발견된 lvol store 를 INSERT_TAIL, unload/destruct 시 REMOVE.
 * vbdev_lvol_store_first()/next() 이터레이터로 외부에 노출되어 RPC `bdev_lvol_get_lvstores`,
 * lvol UUID/이름 lookup 등에서 사용된다.
 * 동기화: lvol 동작은 모두 첫 번째 reactor(현재는 app 메인 스레드)에서만 일어나도록
 *         설계되어 별도 락 없이 단일 스레드 접근 가정. */
static TAILQ_HEAD(, lvol_store_bdev) g_spdk_lvol_pairs = TAILQ_HEAD_INITIALIZER(
			g_spdk_lvol_pairs);

/* [한국어] forward declarations — spdk_bdev_module g_lvol_if 의 ops 멤버로 등록되어야
 * 하므로, 정의보다 앞서 선언만 둔다. */
static int vbdev_lvs_init(void);
/* [한국어] 모듈 초기화 콜백 — 현재는 no-op (return 0). */
static void vbdev_lvs_fini_start(void);
/* [한국어] 모듈 종료 시작 콜백 — 모든 lvol store 를 unload 한다. async_fini_start 가 true
 * 이므로 비동기 완료를 위해 spdk_bdev_module_fini_start_done() 호출이 필요. */
static int vbdev_lvs_get_ctx_size(void);
/* [한국어] bdev_io 마다 모듈이 요구하는 driver_ctx 바이트 수 — sizeof(struct vbdev_lvol_io). */
static void vbdev_lvs_examine_config(struct spdk_bdev *bdev);
/* [한국어] examine_config 콜백 — config 단계의 examine. esnap clone 의 부모 bdev 등장 시
 * lvol 레이어에 hotplug 알림을 보낸다. */
static void vbdev_lvs_examine_disk(struct spdk_bdev *bdev);
/* [한국어] examine_disk 콜백 — disk 단계 examine. bdev 헤더를 읽어 lvol store 가 있는지
 * 확인하고 있으면 load 한다. */

/* [한국어] g_shutdown_started: SPDK app 종료 절차가 시작되었는지(fini_start 진입) 표시.
 * lvol 의 자연스러운 unregister 와 종료 시 강제 unload 동작을 구분하는 데 사용된다.
 * 단일 스레드 환경이므로 별도 atomic 불필요. */
static bool g_shutdown_started = false;

/* [한국어] g_lvol_if: bdev 코어에 본 모듈을 등록하기 위한 정적 디스크립터.
 * - .name "lvol": bdev framework 가 모듈을 식별하는 이름.
 * - .module_init: 모듈 부팅 시 한 번 호출되는 초기화 콜백.
 * - .fini_start: app 종료 시작 시 호출 — 모든 lvol store 를 unload.
 * - .async_fini_start = true: fini_start 가 비동기 완료를 요구함을 의미.
 *   → vbdev_lvs_fini_start_iter 가 마지막에 spdk_bdev_module_fini_start_done() 호출 필수.
 * - .examine_config / .examine_disk: 베이스 bdev 가 등장할 때마다 모듈이 그 bdev 를 검사
 *   할 기회를 받는다. lvol module 은 disk 단계에서 헤더를 읽어 lvol store 자동 로드 수행.
 * - .get_ctx_size: bdev_io 당 driver_ctx 바이트 수를 알려 모듈 전용 컨텍스트를 예약. */
static struct spdk_bdev_module g_lvol_if = {
	.name = "lvol",
	.module_init = vbdev_lvs_init,
	.fini_start = vbdev_lvs_fini_start,
	.async_fini_start = true,
	.examine_config = vbdev_lvs_examine_config,
	.examine_disk = vbdev_lvs_examine_disk,
	.get_ctx_size = vbdev_lvs_get_ctx_size,

};

/* [한국어] bdev framework 에 g_lvol_if 를 컴파일 타임 등록.
 * SPDK_BDEV_MODULE_REGISTER 매크로는 constructor priority 를 이용해 main 이전에
 * 모듈 리스트에 자기 자신을 삽입하므로 별도 init 함수 호출이 필요 없다. */
SPDK_BDEV_MODULE_REGISTER(lvol, &g_lvol_if)

/* [한국어] forward declaration — vbdev_lvol_destroy 와 _vbdev_lvs_remove_lvol_cb 가 서로
 * 호출 순환이 있어 정의보다 먼저 선언만 둔다. */
static void _vbdev_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

/*
 * [한국어]
 * vbdev_get_lvs_bdev_by_lvs - 주어진 spdk_lvol_store 포인터에 대응하는 lvol_store_bdev 페어를 찾는다.
 *
 * @lvs_orig: 찾으려는 lvol store. 보통 spdk_lvol 의 lvol_store 필드에서 얻는다.
 * @return: 매칭되는 lvol_store_bdev 포인터(베이스 bdev 와의 페어). 못 찾거나 해당 페어가
 *          이미 removal_in_progress 인 경우 NULL.
 *
 * lvol 코어 라이브러리(lib/lvol)는 spdk_lvol_store 만 알고 있고, 그것이 어떤 베이스 bdev
 * 위에 얹혀 있는지(=어떤 lvol_store_bdev 페어 안에 있는지)는 본 모듈만이 안다. 이 함수가
 * 그 매핑을 제공하여 vbdev_lvs_rename / vbdev_lvol_destroy / dump_info_json 등이 베이스
 * bdev 정보를 회수할 수 있게 한다. 또한 removal_in_progress 인 경우 NULL 을 반환하여,
 * unload/destroy 중인 lvs 에 대한 새 작업 발행을 차단하는 가드 역할도 한다.
 *
 * 실행 컨텍스트: lvol 모듈을 호스팅하는 단일 스레드(보통 첫 번째 reactor). 글로벌 TAILQ
 * 를 순회하므로 cross-thread 호출 금지.
 *
 * 호출 체인: vbdev_lvol_destroy / vbdev_lvs_rename / vbdev_lvol_dump_info_json /
 *           _vbdev_lvs_remove / _vbdev_lvol_destroy → [이 함수] → vbdev_lvol_store_first/next
 */
struct lvol_store_bdev *
vbdev_get_lvs_bdev_by_lvs(struct spdk_lvol_store *lvs_orig)
{
	struct spdk_lvol_store *lvs = NULL;
	/* [한국어] 임시 변수 — 페어가 보유한 lvs 포인터를 꺼내 비교용으로 사용 */
	struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();
	/* [한국어] g_spdk_lvol_pairs TAILQ 의 첫 항목으로 이터레이션 시작.
	 * 페어가 하나도 없으면 NULL 반환. */

	while (lvs_bdev != NULL) {
		/* [한국어] 모든 (lvs, bdev) 페어 순회 */
		lvs = lvs_bdev->lvs;
		/* [한국어] 현재 페어의 lvs 포인터 추출 */
		if (lvs == lvs_orig) {
			/* [한국어] 주소 비교만 — lvs 객체는 유일하므로 포인터 동일성이 곧 동일 lvs */
			if (lvs_bdev->removal_in_progress) {
				/* We do not allow access to lvs that are being unloaded or
				 * destroyed */
				/* [한국어] unload/destroy 진행 중인 lvs 는 새 작업이 들어오지 못하게
				 * NULL 반환 — race 로 인한 use-after-free 예방 */
				SPDK_DEBUGLOG(vbdev_lvol, "lvs %s: removal in progress\n",
					      lvs_orig->name);
				return NULL;
			} else {
				/* [한국어] 정상 — 호출자가 즉시 사용 가능 */
				return lvs_bdev;
			}
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
		/* [한국어] 다음 페어로 진행 */
	}

	/* [한국어] 매칭되는 페어 없음 */
	return NULL;
}

/*
 * [한국어]
 * _vbdev_lvol_change_bdev_alias - lvol 의 bdev alias("<lvs>/<lvol>") 를 새 이름으로 교체한다.
 *
 * @lvol: 이름 변경 대상 spdk_lvol. lvol->bdev 가 등록되어 있어야 한다(degraded 아님).
 * @new_lvol_name: 사용할 새 lvol 이름. lvs 이름은 그대로 유지된다.
 * @return: 0 성공, -EINVAL alias 개수가 1 이 아닐 때, -ENOMEM 문자열 alloc 실패, 그 외는
 *          spdk_bdev_alias_add / _del 의 에러를 그대로 반환.
 *
 * lvol bdev 는 "기본 이름(=UUID)" 외에 사람이 읽을 수 있는 alias 한 개("lvs_name/lvol_name")
 * 를 가진다. lvol 이나 lvs 의 이름이 바뀌면 이 alias 도 동기화해야 한다. 이 함수는 (1)
 * 기존 alias 가 정확히 1 개인지 확인, (2) 새 alias 를 sprintf 로 만든 뒤, (3) add → del
 * 순으로 적용한다. add 가 먼저 실행되어야 일시적으로라도 lookup 이 끊기지 않는다.
 *
 * 실행 컨텍스트: lvol 모듈 단일 스레드. spdk_bdev_alias_add/_del 도 같은 스레드 가정.
 *
 * 호출 체인: _vbdev_lvs_rename_cb (lvs 이름 변경 후 자식 lvol 들의 alias 갱신) /
 *           vbdev_lvol_rename (개별 lvol 이름 변경) → [이 함수] →
 *           spdk_bdev_get_aliases / spdk_sprintf_alloc / spdk_bdev_alias_add / _del
 */
static int
_vbdev_lvol_change_bdev_alias(struct spdk_lvol *lvol, const char *new_lvol_name)
{
	struct spdk_bdev_alias *tmp;
	/* [한국어] 순회용 임시 포인터 */
	char *old_alias;
	/* [한국어] 발견된 기존 alias 의 이름 포인터 — 소유권은 bdev 가 가지므로 free 금지 */
	char *alias;
	/* [한국어] 새로 만들어 add 한 후 free 하는 임시 버퍼 */
	int rc;
	int alias_number = 0;
	/* [한국어] 발견된 alias 개수 — lvol bdev 는 정확히 1 이어야 함 */

	/* bdev representing lvols have only one alias,
	 * while we changed lvs name earlier, we have to iterate alias list to get one,
	 * and check if there is only one alias */

	TAILQ_FOREACH(tmp, spdk_bdev_get_aliases(lvol->bdev), tailq) {
		/* [한국어] bdev 의 alias TAILQ 를 순회 */
		if (++alias_number > 1) {
			/* [한국어] 2 개 이상이면 어딘가에서 alias 가 잘못 추가된 상태 — 에러 */
			SPDK_ERRLOG("There is more than 1 alias in bdev %s\n", lvol->bdev->name);
			return -EINVAL;
		}

		old_alias = tmp->alias.name;
		/* [한국어] 첫 alias 의 이름을 기억 (개수가 1 임이 확정될 때까지 임시) */
	}

	if (alias_number == 0) {
		/* [한국어] alias 가 하나도 없으면 비정상 — 이름 변경 불가 */
		SPDK_ERRLOG("There are no aliases in bdev %s\n", lvol->bdev->name);
		return -EINVAL;
	}

	alias = spdk_sprintf_alloc("%s/%s", lvol->lvol_store->name, new_lvol_name);
	/* [한국어] 새 alias 문자열 동적 할당 — lvol_store 이름은 현재 값 그대로 사용
	 * (이 함수가 lvs rename 후에도 불리는 점에 주의 — lvs->name 은 이미 새 이름) */
	if (alias == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for alias\n");
		return -ENOMEM;
	}

	rc = spdk_bdev_alias_add(lvol->bdev, alias);
	/* [한국어] 먼저 새 alias 추가 — 이름이 충돌하면 에러. add 가 성공해야만 기존 alias 삭제 */
	if (rc != 0) {
		SPDK_ERRLOG("cannot add alias '%s'\n", alias);
		free(alias);
		return rc;
	}
	free(alias);
	/* [한국어] add 가 내부적으로 strdup 하므로 호출자가 free 해야 함 */

	rc = spdk_bdev_alias_del(lvol->bdev, old_alias);
	/* [한국어] 이제 기존 alias 제거 — 실패 시 일시적으로 두 alias 가 공존 가능 */
	if (rc != 0) {
		SPDK_ERRLOG("cannot remove alias '%s'\n", old_alias);
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * vbdev_get_lvs_bdev_by_bdev - 베이스 bdev 포인터로부터 그 위에 올라가 있는 lvol_store_bdev 페어를 찾는다.
 *
 * @bdev_orig: 베이스 spdk_bdev 포인터 (NVMe / AIO / malloc 등).
 * @return: 매칭 페어 또는 NULL (페어가 removal_in_progress 인 경우도 NULL).
 *
 * 베이스 bdev hotremove 시 그 위에 얹힌 lvol store 를 찾아 자동 unload 하기 위해 필요하다.
 * vbdev_get_lvs_bdev_by_lvs() 의 역방향 lookup 이며, 같은 가드(removal_in_progress)를 적용한다.
 *
 * 실행 컨텍스트: lvol 모듈 단일 스레드.
 *
 * 호출 체인: vbdev_lvs_hotremove_cb → [이 함수] → vbdev_lvol_store_first / _next
 */
static struct lvol_store_bdev *
vbdev_get_lvs_bdev_by_bdev(struct spdk_bdev *bdev_orig)
{
	struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();
	/* [한국어] 글로벌 페어 TAILQ 시작점 */

	while (lvs_bdev != NULL) {
		if (lvs_bdev->bdev == bdev_orig) {
			/* [한국어] 베이스 bdev 포인터 비교 — 일치하면 그 위에 얹힌 lvs 발견 */
			if (lvs_bdev->removal_in_progress) {
				/* We do not allow access to lvs that are being unloaded or
				 * destroyed */
				/* [한국어] 이미 unload 진행 중이면 중복 unload 트리거 방지 */
				SPDK_DEBUGLOG(vbdev_lvol, "lvs %s: removal in progress\n",
					      lvs_bdev->lvs->name);
				return NULL;
			} else {
				return lvs_bdev;
			}
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}

	return NULL;
}

/*
 * [한국어]
 * vbdev_lvs_hotremove_cb - 베이스 bdev 가 hotremove 될 때 lvs 를 안전하게 unload.
 *
 * @bdev: 사라지는 베이스 bdev.
 *
 * SPDK_BDEV_EVENT_REMOVE 가 들어오면 베이스 bdev 는 곧 사라지므로, 그 위에 떠 있는 lvol
 * store 와 모든 lvol 을 닫아야 한다. lvol 들이 다 닫히면 lvs 도 unload 된다. 콜백은 NULL
 * 로 넘겨 "결과는 무시"한다 — hotremove 는 비동기 fire-and-forget.
 *
 * 실행 컨텍스트: bdev 코어가 이벤트 디스패치를 위해 호출 (lvol 모듈 스레드).
 *
 * 호출 체인: bdev framework event dispatch → vbdev_lvs_base_bdev_event_cb → [이 함수] →
 *           vbdev_lvs_unload
 */
static void
vbdev_lvs_hotremove_cb(struct spdk_bdev *bdev)
{
	struct lvol_store_bdev *lvs_bdev;

	lvs_bdev = vbdev_get_lvs_bdev_by_bdev(bdev);
	/* [한국어] 사라지는 bdev 위에 lvs 가 있는지 확인 */
	if (lvs_bdev != NULL) {
		/* [한국어] 발견 — lvs unload 트리거 */
		SPDK_NOTICELOG("bdev %s being removed: closing lvstore %s\n",
			       spdk_bdev_get_name(bdev), lvs_bdev->lvs->name);
		vbdev_lvs_unload(lvs_bdev->lvs, NULL, NULL);
		/* [한국어] cb_fn/cb_arg 모두 NULL — hotremove 는 결과를 기다리지 않는다 */
	}
}

/*
 * [한국어]
 * vbdev_lvs_base_bdev_event_cb - 베이스 bdev 이벤트 디스패처.
 *
 * @type: 이벤트 종류 (REMOVE 만 처리, 그 외는 로그만 남김).
 * @bdev: 이벤트 발생 대상 bdev.
 * @event_ctx: lvs open 시 NULL 로 설정되어 의미 없음.
 *
 * spdk_bdev_create_bs_dev_ext() 로 베이스 bdev 를 열 때 이 함수를 event_cb 로 등록하여,
 * 베이스 bdev 가 사라지거나 resize 되거나 미디어 매니지먼트 이벤트가 발생했을 때 통보받는다.
 * 현재는 REMOVE 만 의미 있게 처리하고 나머지는 로그만 출력.
 *
 * 실행 컨텍스트: bdev framework event dispatch.
 *
 * 호출 체인: spdk_bdev_create_bs_dev_ext 가 등록 → bdev core 이벤트 dispatch → [이 함수]
 */
static void
vbdev_lvs_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			     void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		/* [한국어] 베이스 디스크 분리 — lvs 를 안전하게 닫는다 */
		vbdev_lvs_hotremove_cb(bdev);
		break;
	default:
		/* [한국어] 그 외 이벤트(RESIZE 등)는 lvol 모듈이 다루지 않음. 로그만 출력 */
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/*
 * [한국어]
 * _vbdev_lvs_create_cb - spdk_lvs_init() 완료 콜백. 새로 생성된 lvs 를 페어로 등록.
 *
 * @cb_arg: vbdev_lvs_create_ext() 가 alloc 한 spdk_lvs_with_handle_req (bs_dev, base_bdev,
 *          사용자 cb_fn/cb_arg 보유).
 * @lvs: spdk_lvs_init 이 생성한 lvol_store 핸들 (성공 시), 실패 시 NULL.
 * @lvserrno: spdk_lvs_init 결과 errno (0=성공).
 *
 * lvol store 가 디스크 위에 새로 만들어지면 (1) baseline bdev 를 lvol 모듈 이름으로 claim
 * 하여 다른 모듈의 동시 점유를 막고, (2) (lvs, bdev) 페어 객체를 alloc 하여 글로벌 TAILQ
 * 에 삽입, (3) 사용자 콜백에 lvs 핸들을 전달한다. 실패 시에는 bs_dev 를 파괴하여 베이스
 * 점유를 해제한다.
 *
 * 실행 컨텍스트: blob/lvol 라이브러리가 비동기 완료를 보고하는 스레드 — 본 모듈의 단일
 * 스레드와 동일.
 *
 * 호출 체인: vbdev_lvs_create_ext → spdk_lvs_init → (비동기 완료) → [이 함수] →
 *           사용자 cb_fn (RPC 응답 등)
 */
static void
_vbdev_lvs_create_cb(void *cb_arg, struct spdk_lvol_store *lvs, int lvserrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	/* [한국어] vbdev_lvs_create_ext 에서 alloc 한 요청 컨텍스트 복원 */
	struct lvol_store_bdev *lvs_bdev;
	struct spdk_bdev *bdev = req->base_bdev;
	/* [한국어] 베이스 bdev 핸들 — 페어 객체 채우는 데 사용 */
	struct spdk_bs_dev *bs_dev = req->bs_dev;
	/* [한국어] 베이스 위에 만든 blobstore-device 추상화 — 실패 시 destroy 가 필요 */

	if (lvserrno != 0) {
		/* [한국어] lvs init 실패 — lib/lvol 이 이미 bs_dev 를 정리했으므로 여기서는
		 * 사용자 콜백만 호출하고 빠진다. */
		assert(lvs == NULL);
		SPDK_ERRLOG("Cannot create lvol store bdev\n");
		goto end;
	}

	lvserrno = spdk_bs_bdev_claim(bs_dev, &g_lvol_if);
	/* [한국어] 베이스 bdev 를 lvol 모듈 명의로 점유. 다른 모듈(예: 사용자가 직접 raw
	 * NVMe bdev 를 또 다른 곳에 mount) 의 동시 점유를 막는다. */
	if (lvserrno != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store base bdev already claimed by another bdev\n");
		req->bs_dev->destroy(req->bs_dev);
		/* [한국어] 점유 실패 → bs_dev 도 파기 (참조 카운트 누수 방지) */
		goto end;
	}

	assert(lvs != NULL);
	/* [한국어] 성공 경로면 lvs 는 반드시 유효해야 한다 */

	lvs_bdev = calloc(1, sizeof(*lvs_bdev));
	/* [한국어] 페어 객체 동적 할당 — 글로벌 TAILQ 에 삽입된 채 lvs 의 수명만큼 유지 */
	if (!lvs_bdev) {
		lvserrno = -ENOMEM;
		goto end;
	}
	lvs_bdev->lvs = lvs;
	/* [한국어] 페어가 가리키는 lvs 포인터 — 양방향 매핑의 한쪽 */
	lvs_bdev->bdev = bdev;
	/* [한국어] 페어가 가리키는 베이스 bdev — 다른 한쪽 */
	lvs_bdev->req = NULL;
	/* [한국어] removal 시 사용되는 보류 요청 슬롯 — 생성 시점에는 NULL */

	TAILQ_INSERT_TAIL(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);
	/* [한국어] 글로벌 페어 리스트에 등록 — 이제 RPC iter, hotremove lookup 등에서 보임 */
	SPDK_INFOLOG(vbdev_lvol, "Lvol store bdev inserted\n");

end:
	req->cb_fn(req->cb_arg, lvs, lvserrno);
	/* [한국어] 호출자(보통 RPC 핸들러)에게 lvs 핸들과 errno 를 비동기 보고 */
	free(req);
	/* [한국어] 요청 컨텍스트 해제 */

	return;
}

/*
 * [한국어]
 * vbdev_lvs_create_ext - 베이스 bdev 위에 새 lvol store 를 만든다 (확장 옵션 버전).
 *
 * @base_bdev_name: 베이스 bdev 이름 (예: "Nvme0n1"). 이미 lvol/raid 등이 점유 중이면 실패.
 * @name: 새 lvol store 이름. 1 ~ SPDK_LVS_NAME_MAX-1 글자.
 * @cluster_sz: 클러스터 크기 (바이트). 0 이면 기본값 사용.
 * @clear_method: 클러스터 회수 시 메모리 클리어 방법(none/unmap/write_zeroes). 0 이면 기본.
 * @num_md_pages_per_cluster_ratio: 클러스터당 메타데이터 페이지 수 비율. 0 이면 기본.
 * @md_page_size: 메타데이터 페이지 크기 (4096~65536, 2 의 거듭제곱). 0 이면 기본.
 * @cb_fn: 비동기 완료 콜백.
 * @cb_arg: 콜백 사용자 인자.
 * @return: 0=요청 발행 성공(완료는 콜백으로), 음수=즉시 실패 (-EINVAL/-ENOMEM 등).
 *
 * lvol store 는 baseline bdev 의 모든 LBA 영역을 차지하는 blobstore + lvol metadata 다.
 * 본 함수는 (1) 입력 검증, (2) spdk_bs_dev 생성(베이스 bdev 위에 blobstore-device 추상화
 * 어댑터를 얹음), (3) lvs_opts 채우기 (esnap_bs_dev_create 콜백을 본 모듈 함수로 등록 —
 * 외부 스냅샷 부모를 동적으로 열 수 있게 함), (4) spdk_lvs_init() 비동기 호출, (5) 완료
 * 시 _vbdev_lvs_create_cb 가 페어를 등록하는 흐름이다.
 *
 * 실행 컨텍스트: 보통 RPC 핸들러 스레드(첫 reactor).
 *
 * 호출 체인: RPC rpc_bdev_lvol_create_lvstore → [이 함수] → spdk_bdev_create_bs_dev_ext →
 *           spdk_lvs_init → (비동기) _vbdev_lvs_create_cb
 */
int
vbdev_lvs_create_ext(const char *base_bdev_name, const char *name, uint32_t cluster_sz,
		     enum lvs_clear_method clear_method, uint32_t num_md_pages_per_cluster_ratio,
		     uint32_t md_page_size, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_dev *bs_dev;
	/* [한국어] 베이스 bdev 위에 만든 blobstore-device adapter. lvs_init 의 입력 */
	struct spdk_lvs_with_handle_req *lvs_req;
	/* [한국어] 비동기 콜백 chain 을 위한 요청 컨텍스트 */
	struct spdk_lvs_opts opts;
	/* [한국어] lvs 생성 옵션 — cluster_sz/clear_method/이름/esnap 콜백 등 */
	int rc;
	int len;

	if (base_bdev_name == NULL) {
		/* [한국어] 베이스 bdev 미지정 — 진행 불가 */
		SPDK_ERRLOG("missing base_bdev_name param\n");
		return -EINVAL;
	}

	spdk_lvs_opts_init(&opts);
	/* [한국어] opts 를 기본값으로 초기화 — 이후 사용자가 지정한 비-0 필드만 덮어쓴다 */
	if (cluster_sz != 0) {
		opts.cluster_sz = cluster_sz;
		/* [한국어] thin allocation 단위. 클수록 메타데이터 작지만 단편화 가능성 증가 */
	}

	if (clear_method != 0) {
		opts.clear_method = clear_method;
		/* [한국어] 클러스터 해제 시 데이터 보안/성능 트레이드오프 결정 */
	}

	if (num_md_pages_per_cluster_ratio != 0) {
		opts.num_md_pages_per_cluster_ratio = num_md_pages_per_cluster_ratio;
		/* [한국어] 메타데이터 영역 사이즈 결정 — 너무 작으면 lvol 개수 제한 */
	}

	if (name == NULL) {
		SPDK_ERRLOG("missing name param\n");
		return -EINVAL;
	}

	len = strnlen(name, SPDK_LVS_NAME_MAX);
	/* [한국어] 이름 길이 측정 — overflow 안전 한도 SPDK_LVS_NAME_MAX */

	if (len == 0 || len == SPDK_LVS_NAME_MAX) {
		/* [한국어] 0 = 빈 문자열, MAX = NUL 없음(=overflow). 둘 다 거부 */
		SPDK_ERRLOG("name must be between 1 and %d characters\n", SPDK_LVS_NAME_MAX - 1);
		return -EINVAL;
	}
	snprintf(opts.name, sizeof(opts.name), "%s", name);
	/* [한국어] opts.name 은 고정 길이 배열 — snprintf 로 안전 복사 */
	opts.esnap_bs_dev_create = vbdev_lvol_esnap_dev_create;
	/* [한국어] external snapshot clone 의 부모를 동적으로 열기 위한 콜백 등록.
	 * 본 모듈만이 SPDK bdev 트리에서 임의의 bdev 를 esnap 부모로 열 수 있는 방법을 안다. */

	if (md_page_size != 0 && (md_page_size < 4096 || md_page_size > 65536)) {
		/* [한국어] 메타데이터 페이지 크기 범위 검증 — blob 레이어 제약 */
		SPDK_ERRLOG("Invalid metadata page size %" PRIu32 " (must be between 4096B and 65536B).\n",
			    md_page_size);
		return -EINVAL;
	}

	if (md_page_size != 0 && (!spdk_u32_is_pow2(md_page_size))) {
		/* [한국어] 2 의 거듭제곱이 아니면 NVMe LBA 계산에 안 맞아 거부 */
		SPDK_ERRLOG("Invalid metadata page size %" PRIu32 " (must be a power of 2.)\n", md_page_size);
		return -EINVAL;
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	/* [한국어] 비동기 콜백까지 살아남을 요청 컨텍스트 alloc */
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		return -ENOMEM;
	}

	rc = spdk_bdev_create_bs_dev_ext(base_bdev_name, vbdev_lvs_base_bdev_event_cb,
					 NULL, &bs_dev);
	/* [한국어] 베이스 bdev 위에 blobstore-device adapter 생성. event_cb 등록으로 hotremove
	 * 자동 처리. base_bdev_name 으로 lookup → spdk_bdev_desc open → bs_dev 래핑 */
	if (rc < 0) {
		SPDK_ERRLOG("Cannot create blobstore device\n");
		free(lvs_req);
		return rc;
	}

	if (md_page_size > bs_dev->phys_blocklen) {
		/* [한국어] 메타페이지 > 물리블록 → 한 메타페이지 쓰기가 여러 물리블록에 걸치므로
		 * write atomicity 손실 가능성 경고. 치명적은 아님 (lvol 자체는 동작) */
		SPDK_WARNLOG("Metadata page size is greater than physical block length\n");
	}

	opts.md_page_size = md_page_size;
	/* [한국어] 검증 통과한 md_page_size 를 opts 에 설정 (0 이면 lvol 라이브러리 기본) */
	lvs_req->bs_dev = bs_dev;
	/* [한국어] 콜백에서 실패 시 destroy 가 필요하므로 bs_dev 핸들 보존 */
	lvs_req->base_bdev = bs_dev->get_base_bdev(bs_dev);
	/* [한국어] bs_dev 가 감싸는 실제 bdev 포인터 — 페어 객체 채우는 데 사용 */
	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;
	/* [한국어] 사용자 콜백 — 비동기 완료 시 호출 */

	rc = spdk_lvs_init(bs_dev, &opts, _vbdev_lvs_create_cb, lvs_req);
	/* [한국어] lvol 라이브러리에 lvs 생성 위임. 내부적으로 blobstore 초기화 + 메타데이터
	 * 작성 후 _vbdev_lvs_create_cb 호출. 비동기. */
	if (rc < 0) {
		/* [한국어] 즉시 실패 시 alloc 한 자원 모두 회수 */
		free(lvs_req);
		bs_dev->destroy(bs_dev);
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * vbdev_lvs_create - vbdev_lvs_create_ext 의 호환용 래퍼 (md_page_size 지원 이전 API).
 *
 * @base_bdev_name, name, cluster_sz, clear_method, num_md_pages_per_cluster_ratio, cb_fn, cb_arg:
 *   vbdev_lvs_create_ext 와 동일.
 * @return: vbdev_lvs_create_ext 의 반환값을 그대로 전달.
 *
 * md_page_size 를 0(=기본값)으로 고정하여 ext 버전을 호출한다. 외부 API 호환성을 위해 유지.
 *
 * 호출 체인: 구 외부 코드/테스트 → [이 함수] → vbdev_lvs_create_ext
 */
int
vbdev_lvs_create(const char *base_bdev_name, const char *name, uint32_t cluster_sz,
		 enum lvs_clear_method clear_method, uint32_t num_md_pages_per_cluster_ratio,
		 spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	uint32_t md_page_size = 0;
	/* [한국어] 0 = 라이브러리 기본값 사용 */

	return vbdev_lvs_create_ext(base_bdev_name, name, cluster_sz, clear_method,
				    num_md_pages_per_cluster_ratio, md_page_size, cb_fn, cb_arg);
}

/*
 * [한국어]
 * _vbdev_lvs_rename_cb - spdk_lvs_rename 완료 콜백. 자식 lvol 들의 bdev alias 를 동기화.
 *
 * @cb_arg: vbdev_lvs_rename 에서 alloc 한 spdk_lvs_req.
 * @lvserrno: spdk_lvs_rename 결과 (0=성공).
 *
 * lvs 이름이 바뀌면 그 lvs 안의 모든 lvol bdev 의 alias("lvs_name/lvol_name") 의 앞부분도
 * 갱신되어야 한다. 성공 경로에서 lvs->lvols TAILQ 를 순회하며 각 lvol 의 alias 를 새 lvs
 * 이름으로 교체한다. (이 시점 lvs->name 은 이미 새 이름)
 *
 * 호출 체인: spdk_lvs_rename → (비동기 완료) → [이 함수] → _vbdev_lvol_change_bdev_alias →
 *           사용자 cb_fn
 */
static void
_vbdev_lvs_rename_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_req *req = cb_arg;
	/* [한국어] vbdev_lvs_rename 에서 alloc 한 컨텍스트 복원 */
	struct spdk_lvol *tmp;
	/* [한국어] 자식 lvol 순회용 임시 포인터 */

	if (lvserrno != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store rename failed\n");
		/* [한국어] rename 실패 — alias 갱신 생략, 사용자에 errno 그대로 전달 */
	} else {
		TAILQ_FOREACH(tmp, &req->lvol_store->lvols, link) {
			/* We have to pass current lvol name, since only lvs name changed */
			/* [한국어] lvol 자신의 이름은 그대로 — alias 의 lvs 부분만 동기화 */
			_vbdev_lvol_change_bdev_alias(tmp, tmp->name);
		}
	}

	req->cb_fn(req->cb_arg, lvserrno);
	/* [한국어] 사용자 콜백에 결과 전달 */
	free(req);
}

/*
 * [한국어]
 * vbdev_lvs_rename - lvol store 이름 변경 API (RPC bdev_lvol_rename_lvstore 진입점).
 *
 * @lvs: 이름 바꿀 lvol store 핸들.
 * @new_lvs_name: 새 lvs 이름.
 * @cb_fn / @cb_arg: 비동기 완료 콜백.
 *
 * 페어 lookup → 페어가 유효(removal 중 아님) → 요청 컨텍스트 alloc → lib/lvol 의 rename
 * 비동기 호출. 자식 lvol bdev alias 갱신은 콜백에서 일괄 처리.
 *
 * 호출 체인: RPC rpc_bdev_lvol_rename_lvstore → [이 함수] → spdk_lvs_rename →
 *           _vbdev_lvs_rename_cb
 */
void
vbdev_lvs_rename(struct spdk_lvol_store *lvs, const char *new_lvs_name,
		 spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	struct lvol_store_bdev *lvs_bdev;

	struct spdk_lvs_req *req;

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvs);
	/* [한국어] 페어 lookup — 없거나 removal 중이면 NULL */
	if (!lvs_bdev) {
		SPDK_ERRLOG("No such lvol store found\n");
		cb_fn(cb_arg, -ENODEV);
		/* [한국어] 즉시 실패도 cb_fn 으로 통보 — 호출자 일관 처리 위함 */
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol_store = lvs;
	/* [한국어] 콜백에서 자식 lvol 순회를 위해 lvs 포인터 저장 */

	spdk_lvs_rename(lvs, new_lvs_name, _vbdev_lvs_rename_cb, req);
	/* [한국어] lib/lvol 에 위임 — 메타데이터 디스크 업데이트 후 콜백 */
}

/*
 * [한국어]
 * _vbdev_lvs_remove_cb - spdk_lvs_unload/destroy 의 최종 완료 콜백. 페어 객체 제거.
 *
 * @cb_arg: 페어 객체 lvs_bdev (req 가 첨부되어 있음).
 * @lvserrno: lib/lvol 의 결과 errno.
 *
 * lvs 가 디스크/메모리에서 완전히 사라진 시점에 호출되어 (1) 글로벌 페어 TAILQ 에서 제거,
 * (2) 페어 객체 free, (3) 보류 중인 요청의 사용자 콜백 호출, (4) req free 까지 마무리한다.
 *
 * 호출 체인: spdk_lvs_unload/destroy → [이 함수]
 */
static void
_vbdev_lvs_remove_cb(void *cb_arg, int lvserrno)
{
	struct lvol_store_bdev *lvs_bdev = cb_arg;
	/* [한국어] 제거 대상 페어 */
	struct spdk_lvs_req *req = lvs_bdev->req;
	/* [한국어] _vbdev_lvs_remove 가 페어에 첨부해 둔 요청 컨텍스트 — 사용자 콜백 보유 */

	if (lvserrno != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store removed with error: %d.\n", lvserrno);
		/* [한국어] 에러여도 페어 정리는 진행 (메모리 누수 방지) */
	}

	TAILQ_REMOVE(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);
	/* [한국어] 글로벌 페어 리스트에서 제거 — 이후 lookup 에 더 이상 보이지 않음 */
	free(lvs_bdev);
	/* [한국어] 페어 객체 자체 해제 */

	if (req->cb_fn != NULL) {
		/* [한국어] hotremove 같이 cb_fn=NULL 인 경우는 통보 생략 */
		req->cb_fn(req->cb_arg, lvserrno);
	}
	free(req);
}

/*
 * [한국어]
 * _vbdev_lvs_remove_lvol_cb - destruct(destroy) 경로에서 lvol 들을 하나씩 지워나가는 콜백.
 *
 * @cb_arg: 페어 객체 lvs_bdev (lvs 백포인터 포함).
 * @lvolerrno: 직전 lvol 삭제 결과 (대부분 무시; 다음 lvol 진행).
 *
 * destruct 경로에서는 모든 lvol 을 디스크에서도 지워야 한다. 본 함수는 (1) 모든 lvol 이
 * 사라졌으면 lvs 자체를 destroy, (2) 아직 lvol 이 남았으면 deletable 한 첫 번째를 찾아
 * _vbdev_lvol_destroy 재호출. snapshot 트리의 의존성 때문에 child 가 먼저 삭제되어야
 * parent snapshot 이 삭제 가능 — deletable 검사가 그 의미를 가진다. 의존성 cycle 이면 진행 불가.
 *
 * 호출 체인: _vbdev_lvs_remove(destroy=true) → [이 함수] (재귀적) → _vbdev_lvol_destroy →
 *           최종 spdk_lvs_destroy → _vbdev_lvs_remove_cb
 */
static void
_vbdev_lvs_remove_lvol_cb(void *cb_arg, int lvolerrno)
{
	struct lvol_store_bdev *lvs_bdev = cb_arg;
	struct spdk_lvol_store *lvs = lvs_bdev->lvs;
	struct spdk_lvol *lvol;

	if (lvolerrno != 0) {
		SPDK_DEBUGLOG(vbdev_lvol, "Lvol removed with errno %d\n", lvolerrno);
		/* [한국어] 에러여도 다음 lvol 진행 — 부분 성공도 의미 있음 */
	}

	if (TAILQ_EMPTY(&lvs->lvols)) {
		/* [한국어] 모든 lvol 이 destroy 됨 → 이제 lvs 본체를 destroy.
		 * destroy 는 디스크의 lvs 메타데이터까지 지운다 (unload 와 차이점) */
		spdk_lvs_destroy(lvs, _vbdev_lvs_remove_cb, lvs_bdev);
		return;
	}

	lvol = TAILQ_FIRST(&lvs->lvols);
	while (lvol != NULL) {
		if (spdk_lvol_deletable(lvol)) {
			/* [한국어] deletable: snapshot 트리에서 자기 자신을 제외한 다른 lvol 의
			 * parent 가 아닌 상태(=리프 노드 또는 자식 모두 이미 삭제됨). */
			_vbdev_lvol_destroy(lvol, _vbdev_lvs_remove_lvol_cb, lvs_bdev);
			/* [한국어] 재귀적으로 다음 lvol 삭제 진행 */
			return;
		}
		lvol = TAILQ_NEXT(lvol, link);
		/* [한국어] 이 lvol 은 아직 자식이 있어 못 지움 → 다음으로 */
	}

	/* If no lvol is deletable, that means there is circular dependency. */
	/* [한국어] 어떤 lvol 도 deletable 이 아님 = 의존성 그래프에 cycle. blobstore 무결성
	 * 위반 — 정상적이라면 발생 불가. assert 로 즉시 중단하여 디버깅. */
	SPDK_ERRLOG("Lvols left in lvs, but unable to delete.\n");
	assert(false);
}

/*
 * [한국어]
 * _vbdev_lvs_are_lvols_closed - lvs 내 모든 lvol 이 close 상태인지 검사.
 *
 * @lvs: 검사할 lvol store.
 * @return: true 면 모든 lvol 이 close (ref_count == 0), false 면 하나라도 열려 있음.
 *
 * unload 경로에서 lvs 를 닫기 전, 외부에서 spdk_lvol_open 된 핸들이 남아 있는지 확인.
 * close 가 모두 끝나야 lvs 도 안전하게 unload 가능 (블롭스토어가 닫힐 때 열린 blob 가
 * 있으면 abort 됨).
 */
static bool
_vbdev_lvs_are_lvols_closed(struct spdk_lvol_store *lvs)
{
	struct spdk_lvol *lvol;

	TAILQ_FOREACH(lvol, &lvs->lvols, link) {
		/* [한국어] 모든 lvol 순회 */
		if (lvol->ref_count != 0) {
			/* [한국어] 하나라도 열린 핸들이 있으면 즉시 false */
			return false;
		}
	}
	return true;
}

/*
 * [한국어]
 * _vbdev_lvs_remove_bdev_unregistered_cb - unload 경로에서 개별 lvol bdev unregister 완료 콜백.
 *
 * @cb_arg: 페어 lvs_bdev.
 * @bdeverrno: spdk_bdev_unregister 결과.
 *
 * unload 경로(_vbdev_lvs_remove(destroy=false))에서는 lvol 자체를 디스크에서 지우지 않고
 * bdev 등록만 해제하여 핸들을 close 한다. 마지막 unregister 가 끝나 모든 lvol 이 close
 * 되면 spdk_lvs_unload 를 호출하여 메타데이터 영역만 정리.
 *
 * 호출 체인: spdk_bdev_unregister → [이 함수] → (모든 lvol close 시) spdk_lvs_unload →
 *           _vbdev_lvs_remove_cb
 */
static void
_vbdev_lvs_remove_bdev_unregistered_cb(void *cb_arg, int bdeverrno)
{
	struct lvol_store_bdev *lvs_bdev = cb_arg;
	struct spdk_lvol_store *lvs = lvs_bdev->lvs;

	if (bdeverrno != 0) {
		SPDK_DEBUGLOG(vbdev_lvol, "Lvol unregistered with errno %d\n", bdeverrno);
		/* [한국어] 에러여도 unload 시도 — 정합성보다 진행이 우선 */
	}

	/* Lvol store can be unloaded once all lvols are closed. */
	if (_vbdev_lvs_are_lvols_closed(lvs)) {
		/* [한국어] 모든 lvol close 완료 → lvs unload 가능 */
		spdk_lvs_unload(lvs, _vbdev_lvs_remove_cb, lvs_bdev);
	}
	/* [한국어] 아직 close 안 된 lvol 이 있으면 그쪽의 unregister 완료 콜백이 마지막에
	 * 이 함수를 다시 호출하게 됨 — race-free 진행 보장 */
}

/*
 * [한국어]
 * _vbdev_lvs_remove - lvs 와 그 안의 모든 lvol 을 제거하는 공통 진입점 (unload/destroy 양쪽).
 *
 * @lvs: 제거 대상 lvol store.
 * @cb_fn / @cb_arg: 비동기 완료 콜백. NULL 허용(hotremove).
 * @destroy: true 면 디스크 메타데이터까지 삭제 (destroy), false 면 메모리 상에서만 unload.
 *
 * 두 경로의 공통 작업: (1) 페어 lookup, (2) removal_in_progress 플래그 set (=신규 작업 차단),
 * (3) req 객체에 사용자 콜백 첨부.
 * 그 다음 분기:
 *   - 모든 lvol 이 이미 close → 곧바로 spdk_lvs_destroy/unload 호출.
 *   - destroy: 첫 lvol 부터 _vbdev_lvol_destroy 재귀(_vbdev_lvs_remove_lvol_cb 콜백).
 *   - unload: 모든 lvol 의 bdev 를 unregister (또는 bdev 가 없는 degraded 의 경우 lvol_close).
 *
 * 실행 컨텍스트: lvol 모듈 단일 스레드.
 *
 * 호출 체인: vbdev_lvs_unload / vbdev_lvs_destruct / hotremove → [이 함수] →
 *           (분기) _vbdev_lvol_destroy or spdk_bdev_unregister or spdk_lvs_unload/destroy
 */
static void
_vbdev_lvs_remove(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg,
		  bool destroy)
{
	struct spdk_lvs_req *req;
	struct lvol_store_bdev *lvs_bdev;
	struct spdk_lvol *lvol, *tmp;

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvs);
	/* [한국어] 페어 찾기 — 없거나 이미 removal 중이면 NULL */
	if (!lvs_bdev) {
		SPDK_ERRLOG("No such lvol store found\n");
		if (cb_fn != NULL) {
			cb_fn(cb_arg, -ENODEV);
		}
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		if (cb_fn != NULL) {
			cb_fn(cb_arg, -ENOMEM);
		}
		return;
	}

	lvs_bdev->removal_in_progress = true;
	/* [한국어] 이 시점 이후 새로운 vbdev_get_lvs_bdev_by_lvs 는 NULL 반환 → 새 작업 차단 */

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	lvs_bdev->req = req;
	/* [한국어] 최종 _vbdev_lvs_remove_cb 가 페어에서 req 를 꺼내 사용자 콜백 호출 */

	if (_vbdev_lvs_are_lvols_closed(lvs)) {
		/* [한국어] 이미 모든 lvol 이 close 상태 → 곧바로 lvs 정리 */
		if (destroy) {
			spdk_lvs_destroy(lvs, _vbdev_lvs_remove_cb, lvs_bdev);
			return;
		}
		spdk_lvs_unload(lvs, _vbdev_lvs_remove_cb, lvs_bdev);
		return;
	}
	if (destroy) {
		/* [한국어] destroy 경로: 첫 lvol 부터 디스크에서도 삭제하는 재귀 시작.
		 * 0 = 직전 errno 없음 (트리거용 의미 없는 인자) */
		_vbdev_lvs_remove_lvol_cb(lvs_bdev, 0);
		return;
	}
	/* [한국어] unload 경로: lvol 들의 bdev 만 unregister (디스크 메타데이터는 유지) */
	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		/* [한국어] _SAFE: 콜백이 lvol 을 lvs->lvols 에서 제거할 수 있음 */
		if (lvol->bdev == NULL) {
			/* [한국어] degraded lvol — bdev 등록이 안 되어 있으므로 lvol_close 만 호출 */
			spdk_lvol_close(lvol, _vbdev_lvs_remove_bdev_unregistered_cb, lvs_bdev);
			continue;
		}
		spdk_bdev_unregister(lvol->bdev, _vbdev_lvs_remove_bdev_unregistered_cb, lvs_bdev);
		/* [한국어] 정상 lvol — bdev unregister 가 destruct 콜백을 거쳐 lvol_close 까지 진행 */
	}
}

/*
 * [한국어]
 * vbdev_lvs_unload - lvs 를 메모리에서만 내린다 (디스크 메타데이터는 보존).
 *
 * @lvs, @cb_fn, @cb_arg: _vbdev_lvs_remove 와 동일.
 *
 * SPDK 재시작 후 examine 으로 다시 로드 가능. hotremove 경로에서도 사용.
 *
 * 호출 체인: RPC / hotremove / fini_start → [이 함수] → _vbdev_lvs_remove(destroy=false)
 */
void
vbdev_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	_vbdev_lvs_remove(lvs, cb_fn, cb_arg, false);
}

/*
 * [한국어]
 * vbdev_lvs_destruct - lvs 와 모든 lvol 을 디스크에서 영구 삭제.
 *
 * @lvs, @cb_fn, @cb_arg: _vbdev_lvs_remove 와 동일.
 *
 * RPC bdev_lvol_delete_lvstore 의 진입점. unload 와 달리 메타데이터까지 삭제하므로 복구 불가.
 *
 * 호출 체인: RPC rpc_bdev_lvol_delete_lvstore → [이 함수] → _vbdev_lvs_remove(destroy=true)
 */
void
vbdev_lvs_destruct(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	_vbdev_lvs_remove(lvs, cb_fn, cb_arg, true);
}

/*
 * [한국어]
 * vbdev_lvol_store_first - 글로벌 g_spdk_lvol_pairs TAILQ 의 첫 페어 반환 (이터레이터 시작).
 *
 * @return: 첫 페어 또는 빈 경우 NULL.
 *
 * RPC `bdev_lvol_get_lvstores` 가 페어 전체를 순회할 때 시작점으로 사용. lib/lvol 은 lvs
 * 와 baseline bdev 의 매핑을 모르므로 본 모듈만이 페어를 노출할 수 있다.
 */
struct lvol_store_bdev *
vbdev_lvol_store_first(void)
{
	struct lvol_store_bdev *lvs_bdev;

	lvs_bdev = TAILQ_FIRST(&g_spdk_lvol_pairs);
	/* [한국어] TAILQ 첫 노드 가져오기 — 비어 있으면 NULL */
	if (lvs_bdev) {
		SPDK_INFOLOG(vbdev_lvol, "Starting lvolstore iteration at %p\n", lvs_bdev->lvs);
	}

	return lvs_bdev;
}

/*
 * [한국어]
 * vbdev_lvol_store_next - 페어 이터레이션의 다음 노드 반환.
 *
 * @prev: 직전에 반환된 페어 (NULL 금지).
 * @return: 다음 페어 또는 끝이면 NULL.
 *
 * vbdev_lvol_store_first 와 짝을 이루어 TAILQ 순회 제공.
 */
struct lvol_store_bdev *
vbdev_lvol_store_next(struct lvol_store_bdev *prev)
{
	struct lvol_store_bdev *lvs_bdev;

	if (prev == NULL) {
		/* [한국어] 인터페이스 강제: NULL 은 first() 호출용 → 잘못 사용 */
		SPDK_ERRLOG("prev argument cannot be NULL\n");
		return NULL;
	}

	lvs_bdev = TAILQ_NEXT(prev, lvol_stores);
	/* [한국어] prev 의 다음 노드 — 마지막이었다면 NULL */
	if (lvs_bdev) {
		SPDK_INFOLOG(vbdev_lvol, "Continuing lvolstore iteration at %p\n", lvs_bdev->lvs);
	}

	return lvs_bdev;
}

/*
 * [한국어]
 * _vbdev_get_lvol_store_by_uuid - UUID(이진) 로 lvs lookup (내부 헬퍼).
 *
 * @uuid: 찾을 lvs 의 UUID (16바이트 이진 표현).
 * @return: 매칭 lvs 또는 NULL.
 *
 * 페어 TAILQ 를 선형 탐색. lvs 개수가 보통 적어 선형 탐색이면 충분.
 */
static struct spdk_lvol_store *
_vbdev_get_lvol_store_by_uuid(const struct spdk_uuid *uuid)
{
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();

	while (lvs_bdev != NULL) {
		lvs = lvs_bdev->lvs;
		if (spdk_uuid_compare(&lvs->uuid, uuid) == 0) {
			/* [한국어] memcmp 와 동등 — 16바이트 단순 비교 */
			return lvs;
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}
	return NULL;
}

/*
 * [한국어]
 * vbdev_get_lvol_store_by_uuid - 문자열 UUID 로 lvs lookup (외부 API).
 *
 * @uuid_str: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" 36자 문자열.
 * @return: 매칭 lvs 또는 NULL (UUID 파싱 실패도 NULL).
 *
 * 호출 체인: RPC vbdev_get_lvol_store_by_uuid_xor_name → [이 함수] →
 *           spdk_uuid_parse + _vbdev_get_lvol_store_by_uuid
 */
struct spdk_lvol_store *
vbdev_get_lvol_store_by_uuid(const char *uuid_str)
{
	struct spdk_uuid uuid;

	if (spdk_uuid_parse(&uuid, uuid_str)) {
		/* [한국어] 문자열이 UUID 포맷이 아니면 즉시 NULL */
		return NULL;
	}

	return _vbdev_get_lvol_store_by_uuid(&uuid);
}

/*
 * [한국어]
 * vbdev_get_lvol_store_by_name - 이름으로 lvs lookup (외부 API).
 *
 * @name: 찾을 lvs 이름 (NULL-terminated).
 * @return: 매칭 lvs 또는 NULL.
 *
 * 페어 TAILQ 를 선형 탐색. RPC `bdev_lvol_*` 가 name 인자 처리에 자주 사용.
 */
struct spdk_lvol_store *
vbdev_get_lvol_store_by_name(const char *name)
{
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();

	while (lvs_bdev != NULL) {
		lvs = lvs_bdev->lvs;
		if (strncmp(lvs->name, name, sizeof(lvs->name)) == 0) {
			/* [한국어] strncmp 로 고정 길이 배열 비교 — overflow 방지 */
			return lvs;
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}
	return NULL;
}

/* [한국어] vbdev_lvol_destroy 경로의 비동기 컨텍스트.
 * _vbdev_lvol_destroy 가 alloc → spdk_bdev_unregister 콜백 _vbdev_lvol_destroy_cb 가 free. */
struct vbdev_lvol_destroy_ctx {
	struct spdk_lvol *lvol;
	/* [한국어] 삭제 대상 lvol. spdk_bdev_unregister 후 spdk_lvol_destroy 호출에 사용.
	 * 설정자: _vbdev_lvol_destroy. 읽는 자: _vbdev_lvol_destroy_cb. */
	spdk_lvol_op_complete cb_fn;
	/* [한국어] 사용자 완료 콜백 — destroy 최종 완료 시 호출. */
	void *cb_arg;
	/* [한국어] cb_fn 의 사용자 인자. */
};

/*
 * [한국어]
 * _vbdev_lvol_unregister_unload_lvs - shutdown 중 lvol 의 마지막 bdev unregister 후 lvs unload 완료 콜백.
 *
 * @cb_arg: lvol_bdev (마지막 close 였던 lvol bdev 컨테이너).
 * @lvserrno: lvs unload 결과.
 *
 * SPDK app 종료 절차에서 모든 lvol 이 차례로 unregister 되고, 마지막 lvol 의 close 가
 * 발생할 때 lvs 도 함께 unload 했다. 이 함수는 그 unload 까지 완료된 시점의 콜백으로,
 * (1) 페어 객체를 글로벌 리스트에서 제거, (2) bdev 코어에 destruct 완료를 알림, (3)
 * lvol_bdev 컨테이너 free 까지 수행한다.
 *
 * 호출 체인: g_shutdown_started 경로의 _vbdev_lvol_unregister_cb → spdk_lvs_unload →
 *           [이 함수] → spdk_bdev_destruct_done
 */
static void
_vbdev_lvol_unregister_unload_lvs(void *cb_arg, int lvserrno)
{
	struct lvol_bdev *lvol_bdev = cb_arg;
	struct lvol_store_bdev *lvs_bdev = lvol_bdev->lvs_bdev;
	/* [한국어] lvol_bdev 가 들고 있던 페어 백포인터 — 함께 정리 필요 */

	if (lvserrno != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store removed with error: %d.\n", lvserrno);
	}

	TAILQ_REMOVE(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);
	/* [한국어] 글로벌 페어 리스트에서 제거 */
	free(lvs_bdev);
	/* [한국어] 페어 메모리 회수 */

	spdk_bdev_destruct_done(&lvol_bdev->bdev, lvserrno);
	/* [한국어] bdev 코어에 비동기 destruct 완료 통보. vbdev_lvol_unregister 가 return 1
	 * 했기 때문에 코어는 이 호출을 기다린다. */
	free(lvol_bdev);
	/* [한국어] embed 된 bdev 자체도 같이 해제 (struct 의 head 가 bdev) */
}

/*
 * [한국어]
 * _vbdev_lvol_unregister_cb - lvol close 완료 콜백 (bdev destruct 경로).
 *
 * @ctx: lvol_bdev 컨테이너.
 * @lvolerrno: lvol close 결과.
 *
 * spdk_bdev_unregister → 모듈의 destruct 콜백(vbdev_lvol_unregister) → spdk_lvol_close →
 * 완료 시 본 함수. shutdown 중이고 이 lvol 이 lvs 의 마지막 close 였다면 lvs 도 함께
 * unload (정상 SPDK 종료 흐름). 그렇지 않으면 단순히 destruct 완료를 알리고 메모리 해제.
 *
 * 호출 체인: spdk_lvol_close → [이 함수] → (분기) spdk_lvs_unload or spdk_bdev_destruct_done
 */
static void
_vbdev_lvol_unregister_cb(void *ctx, int lvolerrno)
{
	struct lvol_bdev *lvol_bdev = ctx;
	struct lvol_store_bdev *lvs_bdev = lvol_bdev->lvs_bdev;

	if (g_shutdown_started && _vbdev_lvs_are_lvols_closed(lvs_bdev->lvs)) {
		/* [한국어] app 종료 중 + 이 lvs 의 모든 lvol 이 close 됨 → lvs 도 정리할 타이밍.
		 * 평상시에는 RPC 등으로 lvs 별도 unload 가 트리거되므로 여기서는 안 함. */
		spdk_lvs_unload(lvs_bdev->lvs, _vbdev_lvol_unregister_unload_lvs, lvol_bdev);
		return;
	}

	spdk_bdev_destruct_done(&lvol_bdev->bdev, lvolerrno);
	/* [한국어] 정상 경로: bdev 코어에 비동기 destruct 완료를 알림 */
	free(lvol_bdev);
}

/*
 * [한국어]
 * vbdev_lvol_unregister - bdev 모듈의 destruct 콜백. spdk_bdev_unregister 가 호출.
 *
 * @ctx: bdev->ctxt 로 등록된 spdk_lvol 포인터.
 * @return: 1 (=비동기 destruct. spdk_bdev_destruct_done 으로 통보 필요).
 *
 * bdev 가 사라질 때(예: RPC delete, hotremove, app 종료)마다 한 lvol 당 한 번 호출된다.
 * 본 함수는 spdk_lvol_close 를 비동기로 호출하고 완료 콜백에서 destruct_done 을 통보한다.
 *
 * 실행 컨텍스트: bdev 코어가 unregister 처리 중인 스레드 (보통 첫 reactor).
 *
 * 호출 체인: spdk_bdev_unregister → vbdev_lvol_fn_table.destruct = [이 함수] →
 *           spdk_lvol_close → _vbdev_lvol_unregister_cb
 */
static int
vbdev_lvol_unregister(void *ctx)
{
	struct spdk_lvol *lvol = ctx;
	struct lvol_bdev *lvol_bdev;

	assert(lvol != NULL);
	lvol_bdev = SPDK_CONTAINEROF(lvol->bdev, struct lvol_bdev, bdev);
	/* [한국어] CONTAINEROF: embed 된 bdev 포인터에서 감싸는 lvol_bdev 컨테이너 복원 */

	spdk_lvol_close(lvol, _vbdev_lvol_unregister_cb, lvol_bdev);
	/* [한국어] blob 닫기 → lvol_close → bdev destruct_done 의 연쇄 시작 */

	/* return 1 to indicate we have an operation that must finish asynchronously before the
	 *  lvol is closed
	 */
	/* [한국어] 1 = 비동기 destruct, bdev 코어는 spdk_bdev_destruct_done 호출을 대기 */
	return 1;
}

/*
 * [한국어]
 * _vbdev_lvol_destroy_cb - destroy 경로의 bdev unregister/close 완료 콜백.
 *
 * @cb_arg: vbdev_lvol_destroy_ctx (사용자 cb 와 lvol 보유).
 * @bdeverrno: spdk_bdev_unregister 또는 spdk_lvol_close 결과.
 *
 * 비-degraded 경로: spdk_bdev_unregister 가 완료 → 본 함수 → spdk_lvol_destroy 호출(=실제
 * 디스크 메타데이터 삭제). degraded 경로: spdk_lvol_close 가 완료 → 본 함수 → 동일하게
 * spdk_lvol_destroy. 에러면 사용자에게 즉시 보고하고 ctx 만 해제.
 *
 * 호출 체인: spdk_bdev_unregister / spdk_lvol_close → [이 함수] → spdk_lvol_destroy
 */
static void
_vbdev_lvol_destroy_cb(void *cb_arg, int bdeverrno)
{
	struct vbdev_lvol_destroy_ctx *ctx = cb_arg;
	struct spdk_lvol *lvol = ctx->lvol;

	if (bdeverrno < 0) {
		/* [한국어] bdev unregister 실패 — 다른 핸들이 열려 있거나 unregister race.
		 * 디스크 메타데이터 삭제는 시도하지 않고 사용자에 errno 전달. */
		SPDK_INFOLOG(vbdev_lvol, "Could not unregister bdev during lvol (%s) destroy\n",
			     lvol->unique_id);
		ctx->cb_fn(ctx->cb_arg, bdeverrno);
		free(ctx);
		return;
	}

	spdk_lvol_destroy(lvol, ctx->cb_fn, ctx->cb_arg);
	/* [한국어] 정상 — 이제 lib/lvol 이 lvol 메타데이터를 디스크에서 지움. 사용자 콜백을
	 * 직접 위임. */
	free(ctx);
}

/*
 * [한국어]
 * _vbdev_lvol_destroy - lvol 한 개를 디스크에서 영구 삭제 (내부 구현).
 *
 * @lvol: 삭제 대상 lvol.
 * @cb_fn / @cb_arg: 비동기 완료 콜백.
 *
 * 단계: (1) snapshot 의존성 검사 — clones > 1 이면 child clone 이 있으므로 삭제 거부
 * (-EPERM), (2) ctx alloc, (3) degraded 면 bdev 가 없으므로 spdk_lvol_close 만 호출,
 * 정상이면 spdk_bdev_unregister 후 _vbdev_lvol_destroy_cb 에서 spdk_lvol_destroy 까지.
 *
 * snapshot/clone 관계 핵심: clone 은 parent snapshot 위에 CoW 로 동작하므로 parent 가 사라지면
 * 자기 데이터를 잃는다. 따라서 parent 의 destroy 는 모든 자식이 사라진 뒤만 허용된다.
 *
 * 호출 체인: vbdev_lvol_destroy / _vbdev_lvs_remove_lvol_cb → [이 함수] →
 *           spdk_blob_get_clones (의존성 검사) → spdk_bdev_unregister or spdk_lvol_close →
 *           _vbdev_lvol_destroy_cb → spdk_lvol_destroy
 */
static void
_vbdev_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct vbdev_lvol_destroy_ctx *ctx;
	size_t count;
	/* [한국어] 자식 clone 개수 — clones > 1 이면 삭제 금지 (자기 자신 포함이라 1 이 정상) */

	assert(lvol != NULL);
	assert(cb_fn != NULL);

	/* Callers other than _vbdev_lvs_remove() must ensure the lvstore is not being removed. */
	assert(cb_fn == _vbdev_lvs_remove_lvol_cb ||
	       vbdev_get_lvs_bdev_by_lvs(lvol->lvol_store) != NULL);
	/* [한국어] race 방어: lvs 삭제 진행 중 lvol destroy 가 동시에 트리거되면 안 됨.
	 * _vbdev_lvs_remove_lvol_cb 콜백 경로만 예외적으로 허용. */

	/* Check if it is possible to delete lvol */
	spdk_blob_get_clones(lvol->lvol_store->blobstore, lvol->blob_id, NULL, &count);
	/* [한국어] count 만 채워 받음(ids=NULL). count 는 "이 blob 의 children + 자기 자신" 또는
	 * 비슷한 의미로 lib/blob 이 정의. count > 1 이면 의존성 존재 */
	if (count > 1) {
		/* throw an error */
		SPDK_ERRLOG("Cannot delete lvol\n");
		cb_fn(cb_arg, -EPERM);
		/* [한국어] 자식이 있는 snapshot 은 삭제 거부 — clone 부터 destroy 후 재시도 */
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->lvol = lvol;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	if (spdk_lvol_is_degraded(lvol)) {
		/* [한국어] degraded(esnap 부모 없음 등) → bdev 등록이 안 되어 있음. unregister
		 * 대신 lvol_close 만 호출 → 이후 동일하게 _vbdev_lvol_destroy_cb 에서 destroy */
		spdk_lvol_close(lvol, _vbdev_lvol_destroy_cb, ctx);
		return;
	}

	spdk_bdev_unregister(lvol->bdev, _vbdev_lvol_destroy_cb, ctx);
	/* [한국어] 정상 lvol — bdev 를 unregister 하면 vbdev_lvol_unregister 가 lvol_close 까지
	 * 이끄나, 이 destroy 경로에서는 destruct cb 가 _vbdev_lvol_destroy_cb 로 지정되어
	 * bdev_unregister 완료 시점에 우리 함수가 직접 호출됨 */
}

/*
 * [한국어]
 * vbdev_lvol_destroy - lvol 삭제 외부 API (RPC bdev_lvol_delete 진입점).
 *
 * @lvol: 삭제 대상.
 * @cb_fn / @cb_arg: 비동기 완료 콜백.
 *
 * lvs 가 unload/destroy 진행 중이면 -ENODEV 로 거부, 아니면 _vbdev_lvol_destroy 위임.
 *
 * 호출 체인: RPC rpc_bdev_lvol_delete → [이 함수] → _vbdev_lvol_destroy
 */
void
vbdev_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct lvol_store_bdev *lvs_bdev;

	/*
	 * During destruction of an lvolstore, _vbdev_lvs_unload() iterates through lvols until they
	 * are all deleted. There may be some IO required
	 */
	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvol->lvol_store);
	/* [한국어] lvs 가 removal_in_progress 이면 NULL — 동시 race 방지 */
	if (lvs_bdev == NULL) {
		SPDK_DEBUGLOG(vbdev_lvol, "lvol %s: lvolstore is being removed\n",
			      lvol->unique_id);
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	_vbdev_lvol_destroy(lvol, cb_fn, cb_arg);
}

/*
 * [한국어]
 * vbdev_lvol_find_name - 같은 lvs 안에서 blob_id 로 lvol 이름 lookup.
 *
 * @lvol: lvs 의 어떤 lvol 이든 OK (lvs 핸들을 얻기 위해서만 사용).
 * @blob_id: 찾으려는 blob 의 64bit ID.
 * @return: 매칭 lvol 의 이름 (lvol->name 포인터) 또는 NULL.
 *
 * dump_info_json 이 snapshot/clone 정보를 출력할 때, 자식이나 부모의 blob_id 만 알 수 있는
 * 상황에서 사람이 읽을 수 있는 이름으로 변환하기 위해 사용. lvol 개수 선형 탐색.
 */
static char *
vbdev_lvol_find_name(struct spdk_lvol *lvol, spdk_blob_id blob_id)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *_lvol;

	assert(lvol != NULL);

	lvs = lvol->lvol_store;
	/* [한국어] 동일 lvs 내에서만 검색 (snapshot 관계는 같은 lvs 안에서만 성립) */

	assert(lvs);

	TAILQ_FOREACH(_lvol, &lvs->lvols, link) {
		if (_lvol->blob_id == blob_id) {
			/* [한국어] 64bit blob_id 동등 비교 — 한 lvs 안에서 유일 */
			return _lvol->name;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * vbdev_lvol_dump_info_json - bdev_get_bdevs / dump 시 lvol 별 추가 정보를 JSON 으로 출력.
 *
 * @ctx: bdev->ctxt 의 spdk_lvol.
 * @w: JSON writer.
 * @return: 0 정상, 음수 errno.
 *
 * bdev 코어 dump_info_json fn_table 콜백. 출력 항목:
 *  - "lvol": { "lvol_store_uuid", "base_bdev", "thin_provision", "num_allocated_clusters",
 *              "snapshot", "clone" (+ 부모 "base_snapshot" if clone),
 *              "snapshot" (true 면 자식 clone 들 "clones" 배열),
 *              "esnap_clone" (true 면 부모 "external_snapshot_name") }
 *
 * 호출 체인: RPC bdev_get_bdevs → bdev core dump → vbdev_lvol_fn_table.dump_info_json = [이 함수]
 */
static int
vbdev_lvol_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct spdk_lvol *lvol = ctx;
	struct lvol_store_bdev *lvs_bdev;
	struct spdk_bdev *bdev;
	struct spdk_blob *blob;
	spdk_blob_id *ids = NULL;
	/* [한국어] snapshot 의 자식 clone IDs 동적 배열 */
	size_t count, i;
	char *name;
	int rc = 0;

	spdk_json_write_named_object_begin(w, "lvol");
	/* [한국어] "lvol": { ... } 객체 시작 */

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvol->lvol_store);
	if (!lvs_bdev) {
		SPDK_ERRLOG("No such lvol store found\n");
		rc = -ENODEV;
		goto end;
	}

	bdev = lvs_bdev->bdev;
	/* [한국어] 베이스 bdev 핸들 — base_bdev 필드 출력에 사용 */

	spdk_json_write_named_uuid(w, "lvol_store_uuid", &lvol->lvol_store->uuid);
	/* [한국어] 소속 lvs UUID — 같은 lvs 의 lvol 들을 그룹핑하는 키 */

	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(bdev));
	/* [한국어] 베이스 bdev 이름 — 디버깅/관찰 용이성 */

	blob = lvol->blob;
	/* [한국어] lvol 의 실제 데이터 객체. spdk_blob_is_* 쿼리에 사용 */

	spdk_json_write_named_bool(w, "thin_provision", spdk_blob_is_thin_provisioned(blob));
	/* [한국어] thin = 실제 사용된 만큼만 클러스터 할당. thick = 생성 시점에 전체 할당 */

	spdk_json_write_named_uint64(w, "num_allocated_clusters",
				     spdk_blob_get_num_allocated_clusters(blob));
	/* [한국어] 현재까지 실제로 할당된 클러스터 수 (thin 의 사용량 측정) */

	spdk_json_write_named_bool(w, "snapshot", spdk_blob_is_snapshot(blob));
	/* [한국어] read-only 점-시점 부모 역할인지 (true=snapshot) */

	spdk_json_write_named_bool(w, "clone", spdk_blob_is_clone(blob));
	/* [한국어] 어떤 snapshot 의 자식인지 (true=clone, CoW 로 동작) */

	if (spdk_blob_is_clone(blob)) {
		/* [한국어] clone 이면 부모 snapshot 정보 추가 */
		spdk_blob_id snapshotid = spdk_blob_get_parent_snapshot(lvol->lvol_store->blobstore, lvol->blob_id);
		/* [한국어] blob_id 로 부모 검색 */
		if (snapshotid != SPDK_BLOBID_INVALID) {
			name = vbdev_lvol_find_name(lvol, snapshotid);
			if (name != NULL) {
				spdk_json_write_named_string(w, "base_snapshot", name);
				/* [한국어] 부모 snapshot 이름 출력 */
			} else {
				SPDK_ERRLOG("Cannot obtain snapshots name\n");
			}
		}
	}

	if (spdk_blob_is_snapshot(blob)) {
		/* Take a number of clones */
		/* [한국어] snapshot 이면 자식 clone 들의 이름 배열을 출력 */
		rc = spdk_blob_get_clones(lvol->lvol_store->blobstore, lvol->blob_id, NULL, &count);
		/* [한국어] 먼저 count 만 알아내기 (ids=NULL → -ENOMEM 반환 + count 채워짐) */
		if (rc == -ENOMEM && count > 0) {
			ids = malloc(sizeof(spdk_blob_id) * count);
			/* [한국어] 자식 ID 들을 담을 배열 alloc */
			if (ids == NULL) {
				SPDK_ERRLOG("Cannot allocate memory\n");
				rc = -ENOMEM;
				goto end;
			}

			rc = spdk_blob_get_clones(lvol->lvol_store->blobstore, lvol->blob_id, ids, &count);
			/* [한국어] 두 번째 호출 — 실제 ID 들 채움 */
			if (rc == 0) {
				spdk_json_write_named_array_begin(w, "clones");
				/* [한국어] "clones": [ ... ] 배열 시작 */
				for (i = 0; i < count; i++) {
					name = vbdev_lvol_find_name(lvol, ids[i]);
					if (name != NULL) {
						spdk_json_write_string(w, name);
						/* [한국어] 자식 이름 한 요소 출력 */
					} else {
						SPDK_ERRLOG("Cannot obtain clone name\n");
					}

				}
				spdk_json_write_array_end(w);
			}
			free(ids);
		}

	}

	spdk_json_write_named_bool(w, "esnap_clone", spdk_blob_is_esnap_clone(blob));
	/* [한국어] external snapshot clone 여부 — 외부 read-only bdev 위 thin clone */

	if (spdk_blob_is_esnap_clone(blob)) {
		const char *name;
		size_t name_len;

		rc = spdk_blob_get_esnap_id(blob, (const void **)&name, &name_len);
		/* [한국어] esnap_id 는 lvol 모듈 규약상 UUID 문자열. 다른 모듈은 다른 포맷 가능 */
		if (rc == 0 && name != NULL && strnlen(name, name_len) + 1 == name_len) {
			/* [한국어] NUL 종료 검증: strnlen 결과 + 1 == 길이여야 마지막이 NUL */
			spdk_json_write_named_string(w, "external_snapshot_name", name);
		}
	}

end:
	spdk_json_write_object_end(w);
	/* [한국어] "lvol": { ... } 객체 종료 */

	return rc;
}

/*
 * [한국어]
 * vbdev_lvol_write_config_json - bdev save_config 콜백 — lvol 은 디스크에 메타가 있어 no-op.
 *
 * @bdev, @w: 사용 안 함.
 *
 * lvol 의 모든 구성은 baseline bdev 위 blobstore 메타데이터 영역에 저장되므로, JSON
 * 설정 dump 가 따로 필요 없다. (NVMe bdev 처럼 PCIe BDF 를 재기록할 필요가 없다)
 */
static void
vbdev_lvol_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* Nothing to dump as lvol configuration is saved on physical device. */
}

/*
 * [한국어]
 * vbdev_lvol_get_io_channel - bdev IO 채널 요청 콜백. lib/lvol 의 채널을 그대로 반환.
 *
 * @ctx: bdev->ctxt = spdk_lvol.
 * @return: lvol 의 채널 (=내부적으로 blobstore IO 채널).
 *
 * 각 스레드(=reactor)는 lvol 에 대해 자기 전용 채널을 보유 — lockless 보장. lvol 코어가
 * blobstore 채널을 wrapping 하여 반환.
 *
 * 호출 체인: spdk_bdev_get_io_channel → fn_table.get_io_channel = [이 함수] →
 *           spdk_lvol_get_io_channel
 */
static struct spdk_io_channel *
vbdev_lvol_get_io_channel(void *ctx)
{
	struct spdk_lvol *lvol = ctx;

	return spdk_lvol_get_io_channel(lvol);
}

/*
 * [한국어]
 * vbdev_lvol_io_type_supported - bdev IO 타입 지원 여부 콜백.
 *
 * @ctx: bdev->ctxt = spdk_lvol.
 * @io_type: 검사할 IO 타입.
 * @return: 지원하면 true.
 *
 * 정책:
 *  - WRITE/UNMAP/WRITE_ZEROES: read-only blob 이면 false (snapshot 또는 set_read_only).
 *  - READ/RESET/SEEK_DATA/SEEK_HOLE: 항상 true.
 *  - 나머지(FLUSH/COMPARE/COPY 등): false.
 *
 * 호출 체인: bdev IO 제출 전 코어가 사전 검사 → fn_table.io_type_supported = [이 함수]
 */
static bool
vbdev_lvol_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct spdk_lvol *lvol = ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* [한국어] 쓰기/언맵/제로 — read-only 이면 거부 (snapshot 의 read-only 보장) */
		return !spdk_blob_is_read_only(lvol->blob);
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_SEEK_DATA:
	case SPDK_BDEV_IO_TYPE_SEEK_HOLE:
		/* [한국어] read-only 한 작업들은 항상 지원 */
		return true;
	default:
		/* [한국어] FLUSH/COMPARE/COMPARE_AND_WRITE/COPY 등 — 미지원 */
		return false;
	}
}

/*
 * [한국어]
 * lvol_op_comp - blob IO 완료 → bdev_io 완료로 변환하는 공통 콜백.
 *
 * @cb_arg: spdk_bdev_io.
 * @bserrno: blob 레이어의 결과 (0 성공, 음수 에러).
 *
 * blob IO 모두가 spdk_blob_io_*() 의 콜백 시그니처 (cb_fn, cb_arg, bserrno) 를 따르므로,
 * 이를 bdev_io 의 상태 코드로 변환하여 spdk_bdev_io_complete 으로 코어에 보고한다.
 * -ENOMEM 은 nomem 상태로 매핑되어 bdev 코어가 재시도 큐로 보낸다 — back-pressure 처리.
 *
 * 실행 컨텍스트: lvol IO 처리 스레드 (제출 시점과 동일).
 */
static void
lvol_op_comp(void *cb_arg, int bserrno)
{
	struct spdk_bdev_io *bdev_io = cb_arg;
	enum spdk_bdev_io_status status = SPDK_BDEV_IO_STATUS_SUCCESS;

	if (bserrno != 0) {
		if (bserrno == -ENOMEM) {
			/* [한국어] 메모리 부족 — 코어가 재시도 큐에 넣어 나중에 다시 제출 */
			status = SPDK_BDEV_IO_STATUS_NOMEM;
		} else {
			status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	spdk_bdev_io_complete(bdev_io, status);
	/* [한국어] bdev 코어로 IO 완료 통보 → 사용자 콜백 chain 시작 */
}

/*
 * [한국어]
 * lvol_unmap - UNMAP(=trim) IO 처리. spdk_blob_io_unmap 으로 위임.
 *
 * @lvol: 대상 lvol.
 * @ch: IO 채널.
 * @bdev_io: 처리할 bdev IO (offset_blocks/num_blocks 사용).
 *
 * thin provisioning 의 경우 unmap 으로 클러스터를 회수할 수 있다 (clear_method 에 따라
 * 실제 디스크 trim 또는 zeroes write 가 수행). lvol 의 io_unit = blob 의 io_unit 단위.
 */
static void
lvol_unmap(struct spdk_lvol *lvol, struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages;
	struct spdk_blob *blob = lvol->blob;

	start_page = bdev_io->u.bdev.offset_blocks;
	/* [한국어] bdev 블록 단위 = blob io_unit 단위 (lvol_bdev 생성 시 매칭됨) */
	num_pages = bdev_io->u.bdev.num_blocks;

	spdk_blob_io_unmap(blob, ch, start_page, num_pages, lvol_op_comp, bdev_io);
	/* [한국어] blob 레이어가 클러스터 회수/zero 처리 + 메타데이터 업데이트 수행 */
}

/*
 * [한국어]
 * lvol_seek_data - SEEK_DATA: 주어진 오프셋 이후 다음 할당된 영역의 시작 위치 반환.
 *
 * @lvol: 대상.
 * @bdev_io: 입력 offset_blocks, 출력 seek.offset.
 *
 * thin lvol 에서 sparse file 처럼 "데이터가 있는 영역"을 빠르게 건너뛰는 데 사용 (sparse copy 등).
 * 동기 처리 — blob 의 in-memory 메타에서 즉시 답을 얻음.
 */
static void
lvol_seek_data(struct spdk_lvol *lvol, struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.bdev.seek.offset = spdk_blob_get_next_allocated_io_unit(lvol->blob,
				      bdev_io->u.bdev.offset_blocks);
	/* [한국어] 다음 allocated io_unit 위치 반환 — 없으면 UINT64_MAX */

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	/* [한국어] 동기 완료 — 별도 콜백 불필요 */
}

/*
 * [한국어]
 * lvol_seek_hole - SEEK_HOLE: 주어진 오프셋 이후 다음 미할당(hole) 영역의 시작 위치 반환.
 *
 * @lvol: 대상.
 * @bdev_io: 입력 offset_blocks, 출력 seek.offset.
 *
 * thin lvol 의 hole 위치 검색 — SEEK_DATA 의 반대. 마찬가지로 동기 처리.
 */
static void
lvol_seek_hole(struct spdk_lvol *lvol, struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.bdev.seek.offset = spdk_blob_get_next_unallocated_io_unit(lvol->blob,
				      bdev_io->u.bdev.offset_blocks);
	/* [한국어] 다음 unallocated(=hole) 위치 — 끝까지 없으면 UINT64_MAX */

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

/*
 * [한국어]
 * lvol_write_zeroes - WRITE_ZEROES IO 처리. spdk_blob_io_write_zeroes 위임.
 *
 * @lvol, @ch, @bdev_io: lvol_unmap 과 유사.
 *
 * 데이터를 0 으로 채우는 명령. thin lvol 에서는 클러스터 미할당 상태로 유지 가능 (read
 * 가 자연히 0 반환). thick 또는 이미 할당된 영역이면 실제 0 write.
 */
static void
lvol_write_zeroes(struct spdk_lvol *lvol, struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages;
	struct spdk_blob *blob = lvol->blob;

	start_page = bdev_io->u.bdev.offset_blocks;
	num_pages = bdev_io->u.bdev.num_blocks;

	spdk_blob_io_write_zeroes(blob, ch, start_page, num_pages, lvol_op_comp, bdev_io);
}

/*
 * [한국어]
 * lvol_read - READ IO 처리. spdk_blob_io_readv_ext 위임 (memory_domain 옵션 전달).
 *
 * @ch: IO 채널.
 * @bdev_io: 처리할 IO. bdev->ctxt 에서 lvol 복원.
 *
 * 호출 시점에 bdev_io->u.bdev.iovs 는 spdk_bdev_io_get_buf 가 채워 둠. ext_io_opts 에
 * memory_domain/_ctx 를 복사하여 blob 레이어로 전달 — DMA 직접 영역(RDMA target 등) 지원.
 * 실제 데이터는 thin clone 의 경우 부모 snapshot 또는 esnap 부모로 자동 fallback.
 *
 * 호출 체인: vbdev_lvol_submit_request → spdk_bdev_io_get_buf → lvol_get_buf_cb → [이 함수]
 */
static void
lvol_read(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages;
	struct spdk_lvol *lvol = bdev_io->bdev->ctxt;
	/* [한국어] bdev->ctxt 에 _create_lvol_disk 가 저장해 둔 lvol 포인터 복원 */
	struct spdk_blob *blob = lvol->blob;
	struct vbdev_lvol_io *lvol_io = (struct vbdev_lvol_io *)bdev_io->driver_ctx;
	/* [한국어] bdev_io 끝에 예약된 모듈 컨텍스트 — ext_io_opts 보유 */

	start_page = bdev_io->u.bdev.offset_blocks;
	num_pages = bdev_io->u.bdev.num_blocks;

	lvol_io->ext_io_opts.size = sizeof(lvol_io->ext_io_opts);
	/* [한국어] forward-compat: 이 옵션 구조체의 크기를 명시 — 새 필드 추가 시 ABI 호환 */
	lvol_io->ext_io_opts.memory_domain = bdev_io->u.bdev.memory_domain;
	/* [한국어] 호스트 메모리 도메인 — DPDK hugepage 외 영역(예: RDMA target buffer) 사용 시 */
	lvol_io->ext_io_opts.memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	/* [한국어] 도메인별 추가 정보 (RDMA QP 등) */

	spdk_blob_io_readv_ext(blob, ch, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, start_page,
			       num_pages, lvol_op_comp, bdev_io, &lvol_io->ext_io_opts);
	/* [한국어] vectored read 호출 — blob 레이어가 cluster 할당 상태에 따라 DMA 또는
	 * snapshot/esnap 부모 fetch 를 결정 */
}

/*
 * [한국어]
 * lvol_write - WRITE IO 처리. spdk_blob_io_writev_ext 위임.
 *
 * @lvol: bdev->ctxt 에서 이미 추출된 lvol (호출자가 전달).
 * @ch, @bdev_io: lvol_read 와 유사.
 *
 * thin lvol 에서 미할당 클러스터에 write 가 일어나면 blob 레이어가 자동으로 클러스터 할당.
 * clone 이면 CoW — 부모 데이터 복사 후 자기 클러스터에 변경 사항 기록.
 */
static void
lvol_write(struct spdk_lvol *lvol, struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages;
	struct spdk_blob *blob = lvol->blob;
	struct vbdev_lvol_io *lvol_io = (struct vbdev_lvol_io *)bdev_io->driver_ctx;

	start_page = bdev_io->u.bdev.offset_blocks;
	num_pages = bdev_io->u.bdev.num_blocks;

	lvol_io->ext_io_opts.size = sizeof(lvol_io->ext_io_opts);
	lvol_io->ext_io_opts.memory_domain = bdev_io->u.bdev.memory_domain;
	lvol_io->ext_io_opts.memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;

	spdk_blob_io_writev_ext(blob, ch, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, start_page,
				num_pages, lvol_op_comp, bdev_io, &lvol_io->ext_io_opts);
	/* [한국어] vectored write — CoW / 클러스터 할당 / 메타데이터 업데이트는 blob 레이어 책임 */
}

/*
 * [한국어]
 * lvol_reset - RESET IO 처리. 현재는 항상 실패 처리.
 *
 * @bdev_io: 처리할 RESET IO.
 * @return: 0 (코어가 신경 안 씀).
 *
 * blobstore 는 hardware reset 같은 의미론을 제공하지 않으므로 의미 있는 동작이 없다.
 * 항상 FAILED 로 완료 — 호출자는 RESET 미지원 의미로 해석.
 */
static int
lvol_reset(struct spdk_bdev_io *bdev_io)
{
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	/* [한국어] hardware-level reset 의미가 없는 가상 디바이스 — 실패로 처리 */

	return 0;
}

/*
 * [한국어]
 * lvol_get_buf_cb - READ 의 버퍼 할당 완료 콜백. 실제 read 진행.
 *
 * @ch: IO 채널.
 * @bdev_io: 버퍼가 채워진 bdev IO.
 * @success: 버퍼 할당 성공 여부.
 *
 * READ 는 사용자 iov 가 없는 경우(예: NVMe-oF 가 직접 페이로드 영역으로 데이터를 받을 때)
 * bdev 코어가 미리 버퍼를 할당해 줘야 한다. spdk_bdev_io_get_buf 가 비동기적으로 버퍼를
 * 준비하고, 그 콜백이 본 함수다. 실패면 IO 즉시 fail, 성공이면 lvol_read 호출.
 */
static void
lvol_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	if (!success) {
		/* [한국어] 버퍼 부족 — 즉시 실패 */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	lvol_read(ch, bdev_io);
	/* [한국어] 정상 — 본격 read 진행 */
}

/*
 * [한국어]
 * vbdev_lvol_submit_request - data plane 진입점. 모든 lvol bdev_io 가 이 함수로 들어온다.
 *
 * @ch: IO 채널.
 * @bdev_io: 처리할 IO (type 별 분기).
 *
 * bdev 코어는 spdk_bdev_io 를 submit_request 콜백으로 디스패치한다. 본 함수는 IO 타입별로
 * 적절한 lvol_* 헬퍼 호출 또는 spdk_bdev_io_get_buf (READ 의 lazy buf 할당). 미지원
 * 타입은 FAILED 로 즉시 완료.
 *
 * 실행 컨텍스트: 해당 채널이 속한 reactor 스레드 (lvol 의 IO 처리 스레드).
 *
 * 호출 체인: bdev 코어 IO 디스패치 → fn_table.submit_request = [이 함수] →
 *           lvol_read/write/unmap/write_zeroes/seek_data/seek_hole/reset
 */
static void
vbdev_lvol_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_lvol *lvol = bdev_io->bdev->ctxt;
	/* [한국어] bdev->ctxt 의 lvol 핸들 — 모든 lvol 동작의 시작점 */

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		/* [한국어] READ — 먼저 spdk_bdev_io_get_buf 로 데이터 버퍼 준비 후 lvol_read */
		spdk_bdev_io_get_buf(bdev_io, lvol_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* [한국어] WRITE — iov 는 이미 사용자가 채워 둠 → 바로 blob 에 위임 */
		lvol_write(lvol, ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		/* [한국어] RESET — 가상 디바이스에 의미 없음, 즉시 FAILED */
		lvol_reset(bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		/* [한국어] UNMAP/TRIM — 클러스터 회수 시도 */
		lvol_unmap(lvol, ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* [한국어] 0 채움 — thin 의 경우 클러스터 미할당 유지 가능 */
		lvol_write_zeroes(lvol, ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_SEEK_DATA:
		/* [한국어] sparse copy 등에서 다음 데이터 영역 검색 */
		lvol_seek_data(lvol, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_SEEK_HOLE:
		/* [한국어] sparse copy 등에서 다음 hole 영역 검색 */
		lvol_seek_hole(lvol, bdev_io);
		break;
	default:
		/* [한국어] FLUSH/COMPARE/COMPARE_AND_WRITE/COPY 등은 io_type_supported 에서
		 * 이미 false 를 반환했으므로 정상 경로에서는 도달하지 않음. 안전망. */
		SPDK_INFOLOG(vbdev_lvol, "lvol: unsupported I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	return;
}

/*
 * [한국어]
 * vbdev_lvol_get_memory_domains - 이 lvol 에서 사용 가능한 메모리 도메인 목록을 보고.
 *
 * @ctx: bdev->ctxt = spdk_lvol.
 * @domains: 호출자가 제공한 출력 배열 (NULL 가능, count 만 알고 싶을 때).
 * @array_size: domains 배열 용량.
 * @return: 전체 도메인 개수 (성공) 또는 음수 errno.
 *
 * RDMA target 같은 곳에서 zero-copy 직접 DMA 를 위해, 사용자가 "이 bdev 에 안전하게 DMA
 * 가능한 메모리 도메인은?" 질의에 답한다. lvol 은 (1) 베이스 bdev 의 도메인, (2) esnap
 * clone 이면 부모 esnap bdev 의 도메인까지 합산해야 한다. blob 가 아직 set 되지 않은 (open
 * 진행 중) 경우 -EAGAIN 으로 재시도 유도.
 *
 * 호출 체인: spdk_bdev_get_memory_domains → fn_table.get_memory_domains = [이 함수]
 */
static int
vbdev_lvol_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct spdk_lvol *lvol = ctx;
	struct spdk_bdev *base_bdev, *esnap_bdev;
	struct spdk_bs_dev *bs_dev;
	struct spdk_lvol_store *lvs;
	int base_cnt, esnap_cnt;

	lvs = lvol->lvol_store;
	base_bdev = lvs->bs_dev->get_base_bdev(lvol->lvol_store->bs_dev);
	/* [한국어] lvs 의 베이스 bdev 추출 — 이것이 1차 도메인 소스 */

	base_cnt = spdk_bdev_get_memory_domains(base_bdev, domains, array_size);
	/* [한국어] 베이스 bdev 의 도메인을 처음에 채움 */
	if (base_cnt < 0) {
		return base_cnt;
	}

	if (lvol->blob == NULL) {
		/*
		 * This is probably called due to an open happening during blobstore load. Another
		 * open will follow shortly that has lvol->blob set.
		 */
		/* [한국어] 아직 blob 핸들이 set 안 됨 (load 진행 중) — 재시도 유도 */
		return -EAGAIN;
	}

	if (!spdk_blob_is_esnap_clone(lvol->blob)) {
		/* [한국어] esnap clone 아님 → 베이스 도메인만 의미 있음 */
		return base_cnt;
	}

	bs_dev = spdk_blob_get_esnap_bs_dev(lvol->blob);
	/* [한국어] esnap 부모 bs_dev (정상 또는 bs_dev_degraded) */
	if (bs_dev == NULL) {
		assert(false);
		SPDK_ERRLOG("lvol %s is an esnap clone but has no esnap device\n", lvol->unique_id);
		return base_cnt;
	}

	if (bs_dev->get_base_bdev == NULL) {
		/*
		 * If this were a blob_bdev, we wouldn't be here. We are probably here because an
		 * lvol bdev is being registered with spdk_bdev_register() before the external
		 * snapshot bdev is loaded. Ideally, the load of a missing esnap would trigger an
		 * event that causes the lvol bdev's memory domain information to be updated.
		 */
		/* [한국어] degraded bs_dev (esnap 부모 missing) — bdev 없음 → 베이스만 반환 */
		return base_cnt;
	}

	esnap_bdev = bs_dev->get_base_bdev(bs_dev);
	if (esnap_bdev == NULL) {
		/*
		 * The esnap bdev has not yet been loaded. Anyone that has opened at this point may
		 * miss out on using memory domains if base_cnt is zero.
		 */
		/* [한국어] 등록 직전 timing — 부모 bdev 로딩 전. 베이스만 보고하고 진행 */
		SPDK_NOTICELOG("lvol %s reporting %d memory domains, not including missing esnap\n",
			       lvol->unique_id, base_cnt);
		return base_cnt;
	}

	if (base_cnt < array_size) {
		/* [한국어] 배열에 여유 있음 → esnap 도메인은 base_cnt 이후 슬롯에 채움 */
		array_size -= base_cnt;
		domains += base_cnt;
	} else {
		/* [한국어] 이미 가득 — esnap 호출은 count 만 알아내기 용 */
		array_size = 0;
		domains = NULL;
	}

	esnap_cnt = spdk_bdev_get_memory_domains(esnap_bdev, domains, array_size);
	if (esnap_cnt <= 0) {
		/* [한국어] esnap bdev 가 도메인을 가지지 않거나 에러 — 베이스만 반환 */
		return base_cnt;
	}

	return base_cnt + esnap_cnt;
	/* [한국어] 베이스 + esnap 합산 — 호출자가 둘 다 안전히 사용 가능 */
}

/* [한국어] vbdev_lvol_fn_table: lvol bdev 의 fn_table 콜백 모음.
 * 모든 lvol bdev 는 _create_lvol_disk 에서 bdev->fn_table = &vbdev_lvol_fn_table 로 설정.
 * - .destruct: bdev unregister 시 spdk_lvol_close 까지 진행 (return 1 → 비동기 완료).
 * - .io_type_supported: 지원 IO 타입 필터링 (read-only 시 write 류 차단).
 * - .submit_request: data plane 진입점, IO 타입별 분기.
 * - .get_io_channel: lib/lvol 의 채널 반환 (per-thread lockless).
 * - .dump_info_json: bdev_get_bdevs 출력에 snapshot/clone/esnap 정보 추가.
 * - .write_config_json: lvol 구성은 디스크에 있어 no-op.
 * - .get_memory_domains: 베이스+esnap 합산 도메인 보고 (RDMA zero-copy 등에 사용). */
static struct spdk_bdev_fn_table vbdev_lvol_fn_table = {
	.destruct		= vbdev_lvol_unregister,
	.io_type_supported	= vbdev_lvol_io_type_supported,
	.submit_request		= vbdev_lvol_submit_request,
	.get_io_channel		= vbdev_lvol_get_io_channel,
	.dump_info_json		= vbdev_lvol_dump_info_json,
	.write_config_json	= vbdev_lvol_write_config_json,
	.get_memory_domains	= vbdev_lvol_get_memory_domains,
};

/*
 * [한국어]
 * lvol_destroy_cb - spdk_lvol_destroy 의 최종 콜백 (no-op).
 *
 * @cb_arg, @bdeverrno: 무시.
 *
 * _create_lvol_disk 가 register 직후 alias_add 실패 시 정리 경로로 호출되는 흔치 않은
 * cleanup. lvol 자체가 사라지는 작업이므로 통보할 곳이 없어 빈 함수.
 */
static void
lvol_destroy_cb(void *cb_arg, int bdeverrno)
{
}

/*
 * [한국어]
 * _create_lvol_disk_destroy_cb - _create_lvol_disk 의 alias_add 실패 정리 (destroy=true 경로).
 *
 * @cb_arg: 정리 대상 lvol.
 * @bdeverrno: spdk_bdev_unregister 결과.
 *
 * 생성 중간에 alias 등록이 실패해 unregister 호출 → 본 함수 → lvol_destroy 까지 진행하여
 * 디스크의 메타데이터까지 삭제. (이미 사용자가 vbdev_lvol_create 한 새 lvol 이므로 destroy 옳음)
 */
static void
_create_lvol_disk_destroy_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_lvol *lvol = cb_arg;

	if (bdeverrno < 0) {
		SPDK_ERRLOG("Could not unregister bdev for lvol %s\n",
			    lvol->unique_id);
		return;
	}

	spdk_lvol_destroy(lvol, lvol_destroy_cb, NULL);
	/* [한국어] 디스크 정리까지 진행. 콜백은 빈 함수 — 통보할 곳 없음 */
}

/*
 * [한국어]
 * _create_lvol_disk_unload_cb - _create_lvol_disk 의 alias_add 실패 정리 (destroy=false 경로).
 *
 * @cb_arg: 정리 대상 lvol.
 * @bdeverrno: spdk_bdev_unregister 결과.
 *
 * examine 경로(=load) 에서는 디스크 메타데이터를 삭제하면 안 된다. bdev unregister 만 하고
 * 메모리상 lvol 만 제거. lvs->lvols 에서도 제거하여 다른 lvol 의 동시 작업에 영향 없게 함.
 */
static void
_create_lvol_disk_unload_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_lvol *lvol = cb_arg;

	if (bdeverrno < 0) {
		SPDK_ERRLOG("Could not unregister bdev for lvol %s\n",
			    lvol->unique_id);
		return;
	}

	TAILQ_REMOVE(&lvol->lvol_store->lvols, lvol, link);
	/* [한국어] lvs 의 lvol 리스트에서 제거 */
	free(lvol);
	/* [한국어] lvol 핸들 메모리 회수 (디스크 메타는 그대로 유지) */
}

/*
 * [한국어]
 * _create_lvol_disk - 열린 spdk_lvol 한 개를 spdk_bdev 로 변환하여 등록. lvol→bdev 코어 변환.
 *
 * @lvol: 이미 spdk_lvol_open / create / snapshot 등으로 열린 lvol 핸들.
 * @destroy: alias_add 실패 시 디스크에서도 지울지 여부 (true=create 경로, false=examine 경로).
 * @return: 0 성공, 음수 errno (degraded 인 경우도 0 = "지금은 bdev 안 만들지만 lvol 자체는 유효").
 *
 * 핵심 변환 단계:
 *  (1) degraded 이면 skip (esnap 부모 없음 — bdev 만들면 IO 실패하므로 부모 등장 후 재시도).
 *  (2) lvol_store_bdev 페어 lookup (반드시 존재).
 *  (3) struct lvol_bdev 컨테이너 alloc — 내부에 spdk_bdev 가 embed.
 *  (4) bdev 필드 채우기:
 *      - name: lvol->unique_id (=UUID 문자열, 시스템 전역 유일).
 *      - product_name: "Logical Volume".
 *      - blocklen: blob 의 io_unit 크기 (=섹터).
 *      - blockcnt: 클러스터 수 × 클러스터 크기 / blocklen.
 *      - uuid: lvol UUID.
 *      - required_alignment: 베이스 bdev 의 정렬 요구사항 그대로.
 *      - split_on_optimal_io_boundary + optimal_io_boundary: 클러스터 경계에서 IO 분할 권장.
 *      - ctxt: lvol 자체 (submit_request 등이 ctxt 에서 lvol 복원).
 *      - fn_table: vbdev_lvol_fn_table (콜백 셋).
 *      - module: g_lvol_if (소유자 표시).
 *      - phys_blocklen: 베이스의 물리 블록 크기 (cluster_size 와 무관, hardware 정렬용).
 *      - numa: 베이스의 NUMA 노드 (locality preserve).
 *      - reset_io_drain_timeout: SPDK_BDEV_RESET_IO_DRAIN_RECOMMENDED_VALUE — 공유 bdev 에서
 *        empty reset 폭주 방지.
 *  (5) spdk_bdev_register 호출 — 등록 실패 시 컨테이너 free.
 *  (6) alias "lvs_name/lvol_name" 추가 — 실패 시 destroy/unload 정리 콜백 호출 후 errno 전파.
 *
 * 실행 컨텍스트: lvol 모듈 단일 스레드.
 *
 * 호출 체인: _vbdev_lvol_create_cb (create) / _vbdev_lvs_examine_finish (load) /
 *           create_esnap_clone_lvol_disks (esnap hotplug) → [이 함수] → spdk_bdev_register
 */
static int
_create_lvol_disk(struct spdk_lvol *lvol, bool destroy)
{
	struct spdk_bdev *bdev;
	struct lvol_bdev *lvol_bdev;
	struct lvol_store_bdev *lvs_bdev;
	uint64_t total_size;
	unsigned char *alias;
	int rc;

	if (spdk_lvol_is_degraded(lvol)) {
		/* [한국어] degraded — esnap 부모 bdev 미존재. bdev 를 등록해 두면 IO 가 -EIO
		 * 로 떨어지므로 등록 자체를 지연. 부모가 hotplug 되면 vbdev_lvs_hotplug 가
		 * create_esnap_clone_lvol_disks 를 통해 다시 시도. */
		SPDK_NOTICELOG("lvol %s: blob is degraded: deferring bdev creation\n",
			       lvol->unique_id);
		return 0;
	}

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvol->lvol_store);
	/* [한국어] 페어 lookup — 정상 흐름에서는 반드시 발견 */
	if (lvs_bdev == NULL) {
		SPDK_ERRLOG("No spdk lvs-bdev pair found for lvol %s\n", lvol->unique_id);
		assert(false);
		return -ENODEV;
	}

	lvol_bdev = calloc(1, sizeof(struct lvol_bdev));
	/* [한국어] embed bdev 와 backptrs 를 함께 들고 다닐 컨테이너 alloc */
	if (!lvol_bdev) {
		SPDK_ERRLOG("Cannot alloc memory for lvol bdev\n");
		return -ENOMEM;
	}

	lvol_bdev->lvol = lvol;
	/* [한국어] bdev → lvol 역참조 (CONTAINEROF 대신 직접 포인터로 빠른 lookup) */
	lvol_bdev->lvs_bdev = lvs_bdev;
	/* [한국어] 페어 참조 — destruct 시 lvs unload 판단에 사용 */

	bdev = &lvol_bdev->bdev;
	/* [한국어] embed 된 spdk_bdev 의 주소 — bdev 코어에 등록할 객체 */
	bdev->name = lvol->unique_id;
	/* [한국어] bdev 이름 = UUID 문자열 (전역 유일성 보장). 사용자에게 노출되는 1차 이름 */
	bdev->product_name = "Logical Volume";
	/* [한국어] bdev_get_bdevs 출력 등에서 제품군 표시 */
	bdev->blocklen = spdk_bs_get_io_unit_size(lvol->lvol_store->blobstore);
	/* [한국어] bdev 블록 크기 = blob io_unit 크기. NVMe LBA 와 동일한 의미의 단위 */
	total_size = spdk_blob_get_num_clusters(lvol->blob) *
		     spdk_bs_get_cluster_size(lvol->lvol_store->blobstore);
	/* [한국어] lvol 총 바이트 크기 = 클러스터 수 × 클러스터 크기.
	 * thin 도 논리 크기 기준이라 같음 (실제 할당량은 num_allocated_clusters) */
	assert((total_size % bdev->blocklen) == 0);
	/* [한국어] 전체 크기가 블록 크기의 정수배여야 함 (blob 보장) */
	bdev->blockcnt = total_size / bdev->blocklen;
	/* [한국어] bdev 가 외부에 알리는 블록 개수 */
	bdev->uuid = lvol->uuid;
	/* [한국어] lvol UUID 를 bdev UUID 로 직접 사용 — RPC lookup 통합 */
	bdev->required_alignment = lvs_bdev->bdev->required_alignment;
	/* [한국어] 베이스의 정렬 요구사항 그대로 — DMA 정렬 보존 */
	bdev->split_on_optimal_io_boundary = true;
	/* [한국어] true 면 bdev 코어가 optimal_io_boundary 를 넘는 IO 를 자동 분할 */
	bdev->optimal_io_boundary = spdk_bs_get_cluster_size(lvol->lvol_store->blobstore) / bdev->blocklen;
	/* [한국어] 클러스터 경계에서 분할 — CoW/할당이 클러스터 단위로 일어나므로 단일 IO 가
	 * 두 클러스터에 걸치면 효율이 떨어진다 */

	bdev->ctxt = lvol;
	/* [한국어] 모든 fn_table 콜백에서 ctx 인자로 받게 됨 — lvol 핸들 복원의 정석 */
	bdev->fn_table = &vbdev_lvol_fn_table;
	/* [한국어] data plane 콜백 등록 */
	bdev->module = &g_lvol_if;
	/* [한국어] 소유 모듈 표시 — vbdev_lvol_get_from_bdev 가 이걸로 검증 */
	bdev->phys_blocklen = lvol->lvol_store->bs_dev->phys_blocklen;
	/* [한국어] 물리 블록 크기 (예: NVMe 의 LBA 크기). atomicity 결정에 사용 */

	bdev->numa = lvs_bdev->bdev->numa;
	/* [한국어] 베이스의 NUMA 노드 — 같은 노드 reactor 가 IO 를 처리하도록 locality 보존 */

	/* Set default bdev reset waiting time. This value indicates how much
	 * time a reset should wait before forcing a reset down to the underlying
	 * bdev module.
	 * Setting this parameter is mainly to avoid "empty" resets to a shared
	 * bdev that may be used by multiple lvols. */
	/* [한국어] 같은 베이스 bdev 위 여러 lvol 이 동시에 reset 을 보내면 베이스 SSD 가
	 * 폭주할 수 있어, drain timeout 동안 진행 중 IO 가 끝나길 기다린다. */
	bdev->reset_io_drain_timeout = SPDK_BDEV_RESET_IO_DRAIN_RECOMMENDED_VALUE;

	rc = spdk_bdev_register(bdev);
	/* [한국어] bdev 코어 등록 — 이름 충돌 / 모듈 init 미완료 등이면 실패 */
	if (rc) {
		free(lvol_bdev);
		return rc;
	}
	lvol->bdev = bdev;
	/* [한국어] lvol 에 자기 bdev 백포인터 설정 — IO 처리 시 사용 */

	alias = spdk_sprintf_alloc("%s/%s", lvs_bdev->lvs->name, lvol->name);
	/* [한국어] 사람이 읽을 수 있는 alias "lvs_name/lvol_name" */
	if (alias == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for alias\n");
		spdk_bdev_unregister(lvol->bdev, (destroy ? _create_lvol_disk_destroy_cb :
						  _create_lvol_disk_unload_cb), lvol);
		/* [한국어] 정리 — destroy 면 디스크까지, examine 이면 메모리만 회수 */
		return -ENOMEM;
	}

	rc = spdk_bdev_alias_add(bdev, alias);
	if (rc != 0) {
		SPDK_ERRLOG("Cannot add alias to lvol bdev\n");
		spdk_bdev_unregister(lvol->bdev, (destroy ? _create_lvol_disk_destroy_cb :
						  _create_lvol_disk_unload_cb), lvol);
		/* [한국어] alias 추가 실패 — 위와 같은 정리 (rc 는 이미 음수, 그대로 반환) */
	}
	free(alias);
	/* [한국어] add 가 strdup 하므로 호출자가 원본 free */

	return rc;
}

/*
 * [한국어]
 * _vbdev_lvol_create_cb - spdk_lvol_create / _snapshot / _clone 의 공통 완료 콜백.
 *
 * @cb_arg: 사용자 콜백을 담은 spdk_lvol_with_handle_req.
 * @lvol: 새로 생성된 lvol (성공 시).
 * @lvolerrno: lib/lvol 결과 errno.
 *
 * lvol 자체가 생성되면 (블롭 metadata 디스크 기록 완료), 본 함수가 _create_lvol_disk 를
 * 호출하여 bdev 로 등록까지 마무리한다 (destroy=true: 등록 실패 시 디스크에서도 지움).
 * 그 결과 errno 를 사용자 콜백에 전달.
 *
 * 호출 체인: spdk_lvol_create/_snapshot/_clone/_esnap_clone → [이 함수] → _create_lvol_disk
 */
static void
_vbdev_lvol_create_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;

	if (lvolerrno < 0) {
		/* [한국어] lvol 생성 실패 — bdev 변환은 시도하지 않음 */
		goto end;
	}

	lvolerrno = _create_lvol_disk(lvol, true);
	/* [한국어] 성공 — bdev 등록까지 진행. destroy=true: 등록 실패 시 디스크 메타까지 회수 */

end:
	req->cb_fn(req->cb_arg, lvol, lvolerrno);
	free(req);
}

/*
 * [한국어]
 * vbdev_lvol_create - 새 lvol 생성 외부 API (RPC bdev_lvol_create 진입점).
 *
 * @lvs: 어느 lvol store 안에 만들지.
 * @name: 새 lvol 이름.
 * @sz: 논리 크기 (바이트).
 * @thin_provision: true=thin (사용 시점 할당), false=thick (즉시 전체 할당).
 * @clear_method: 클러스터 회수 시 처리 방법.
 * @cb_fn / @cb_arg: 비동기 완료 콜백.
 * @return: 0=요청 발행 성공, 음수=즉시 실패.
 *
 * lib/lvol 의 spdk_lvol_create 에 위임 → 콜백에서 bdev 등록까지 자동 진행.
 *
 * 호출 체인: RPC rpc_bdev_lvol_create → [이 함수] → spdk_lvol_create → _vbdev_lvol_create_cb
 */
int
vbdev_lvol_create(struct spdk_lvol_store *lvs, const char *name, uint64_t sz,
		  bool thin_provision, enum lvol_clear_method clear_method, spdk_lvol_op_with_handle_complete cb_fn,
		  void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		return -ENOMEM;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	rc = spdk_lvol_create(lvs, name, sz, thin_provision, clear_method,
			      _vbdev_lvol_create_cb, req);
	/* [한국어] lib/lvol 에 위임 — 콜백에서 bdev 등록까지 일괄 처리 */
	if (rc != 0) {
		free(req);
	}

	return rc;
}

/*
 * [한국어]
 * vbdev_lvol_create_snapshot - 기존 lvol 의 read-only 스냅샷 생성.
 *
 * @lvol: 스냅샷의 원본 (변경 가능). 함수 호출 후 lvol 은 새 스냅샷의 자식 clone 으로 전환.
 * @snapshot_name: 스냅샷의 새 이름.
 * @cb_fn / @cb_arg: 비동기 완료 콜백 — 새로 생성된 스냅샷 lvol 을 받는다.
 *
 * 동작 원리(blob CoW): (1) 원본 lvol 의 현재 clusters 를 그대로 사용하는 새 read-only blob
 * 을 만든다(=snapshot). (2) 원본 lvol 은 그 snapshot 의 자식 clone 으로 재구성되어, 이후
 * write 가 들어오면 새 클러스터를 할당하고 거기에 기록(CoW). 결과적으로 snapshot 시점의
 * 데이터가 보존되며 빠른 백업/복제가 가능.
 *
 * 호출 체인: RPC rpc_bdev_lvol_snapshot → [이 함수] → spdk_lvol_create_snapshot →
 *           _vbdev_lvol_create_cb
 */
void
vbdev_lvol_create_snapshot(struct spdk_lvol *lvol, const char *snapshot_name,
			   spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_lvol_create_snapshot(lvol, snapshot_name, _vbdev_lvol_create_cb, req);
	/* [한국어] lib/lvol 이 CoW 메커니즘으로 snapshot blob 생성 */
}

/*
 * [한국어]
 * vbdev_lvol_create_clone - 스냅샷의 새 writable clone 생성.
 *
 * @lvol: 부모가 될 snapshot lvol (반드시 spdk_blob_is_snapshot 인 상태).
 * @clone_name: 새 clone 이름.
 * @cb_fn / @cb_arg: 비동기 완료 콜백 — 새 clone lvol 반환.
 *
 * snapshot 위에 만든 새 thin clone. write 시 자기 클러스터에 CoW 로 기록되며 read 시 자기에
 * 없으면 부모 snapshot 으로 fallback. 같은 snapshot 위에 여러 clone 을 만들어 VM 디스크 등을
 * 빠르게 복제할 수 있다.
 */
void
vbdev_lvol_create_clone(struct spdk_lvol *lvol, const char *clone_name,
			spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_lvol_create_clone(lvol, clone_name, _vbdev_lvol_create_cb, req);
	/* [한국어] lib/lvol 이 snapshot 위 thin clone 메타 생성 */
}

/*
 * [한국어]
 * ignore_bdev_event_cb - bdev open 시 등록되는 이벤트 콜백의 no-op 더미.
 *
 * @type, @bdev, @ctx: 무시.
 *
 * vbdev_lvol_create_bdev_clone / vbdev_lvol_set_external_parent 가 esnap 부모 bdev 를 잠깐
 * 열어 UUID 만 알아내고 닫을 때 사용. 이벤트가 발생해도 무시.
 */
static void
ignore_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

/*
 * [한국어]
 * vbdev_lvol_create_bdev_clone - 외부 bdev (read-only base) 를 부모로 한 ESnap clone 생성.
 *
 * @esnap_name: 부모가 될 외부 bdev 이름 또는 UUID. lvol 외부 (보통 NVMe / blob_bdev 등).
 * @lvs: clone 이 들어갈 lvol store.
 * @clone_name: 새 clone 이름.
 * @cb_fn / @cb_arg: 비동기 완료 콜백.
 *
 * ESnap(external snapshot) 의 핵심 동작:
 *  - lvol clone 의 부모를 "같은 lvs 안의 snapshot lvol" 이 아니라 "외부의 read-only bdev"
 *    로 지정. 예: 이미 만들어진 OS 디스크 이미지를 모든 VM 의 thin clone 부모로 사용.
 *  - clone 의 read 가 자기에 없으면 외부 bdev 에서 fetch, write 는 자기에 CoW.
 *  - 부모는 UUID 로 식별 — 본 함수는 (1) 부모 bdev 를 잠깐 열어 UUID 추출, (2) UUID 를
 *    esnap_id 로 lib/lvol 에 전달, (3) 부모 bdev close 후 lib/lvol 이 lvol 메타에 UUID 기록.
 *  - 이후 lvol open 시점에 vbdev_lvol_esnap_dev_create 콜백이 UUID 로 부모 bdev 를 다시
 *    찾아서 bs_dev 로 wrapping.
 *
 * 호출 체인: RPC rpc_bdev_lvol_clone_bdev → [이 함수] → spdk_bdev_open_ext (부모 UUID 추출) →
 *           spdk_lvol_create_esnap_clone → _vbdev_lvol_create_cb
 */
void
vbdev_lvol_create_bdev_clone(const char *esnap_name,
			     struct spdk_lvol_store *lvs, const char *clone_name,
			     spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	char bdev_uuid[SPDK_UUID_STRING_LEN];
	/* [한국어] esnap_id 로 lvol 메타에 저장할 UUID 문자열 (37바이트) */
	uint64_t sz;
	int rc;

	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store not specified\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	rc = spdk_bdev_open_ext(esnap_name, false, ignore_bdev_event_cb, NULL, &desc);
	/* [한국어] 부모 bdev open (write=false). UUID 만 알면 되므로 잠깐만 사용 */
	if (rc != 0) {
		SPDK_ERRLOG("bdev '%s' could not be opened: error %d\n", esnap_name, rc);
		cb_fn(cb_arg, NULL, rc);
		return;
	}
	bdev = spdk_bdev_desc_get_bdev(desc);

	rc = spdk_uuid_fmt_lower(bdev_uuid, sizeof(bdev_uuid), spdk_bdev_get_uuid(bdev));
	/* [한국어] UUID 를 표준 문자열 포맷("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx") 으로 변환 */
	if (rc != 0) {
		spdk_bdev_close(desc);
		SPDK_ERRLOG("bdev %s: unable to parse UUID\n", esnap_name);
		assert(false);
		cb_fn(cb_arg, NULL, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		spdk_bdev_close(desc);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	sz = spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev);
	/* [한국어] clone 의 논리 크기 = 부모 bdev 의 크기. 부모 read-only 이므로 같은 크기로 */
	rc = spdk_lvol_create_esnap_clone(bdev_uuid, sizeof(bdev_uuid), sz, lvs, clone_name,
					  _vbdev_lvol_create_cb, req);
	/* [한국어] lib/lvol 이 esnap_id 를 lvol 메타에 기록하고 clone blob 생성 */
	spdk_bdev_close(desc);
	/* [한국어] 부모 bdev 는 이제 닫음. 이후 lvol open 시점에 esnap_dev_create 가 다시 연다 */
	if (rc != 0) {
		cb_fn(cb_arg, NULL, rc);
		free(req);
	}
}

/*
 * [한국어]
 * _vbdev_lvol_rename_cb - spdk_lvol_rename 완료 콜백. 사용자에게 결과 전달.
 *
 * @cb_arg: vbdev_lvol_rename 에서 alloc 한 spdk_lvol_req.
 * @lvolerrno: lib/lvol rename 결과.
 *
 * 이 시점에는 이미 _vbdev_lvol_change_bdev_alias 가 bdev alias 도 갱신해 둔 상태(아래
 * vbdev_lvol_rename 의 흐름). 사용자 콜백에 결과만 전달.
 */
static void
_vbdev_lvol_rename_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	if (lvolerrno != 0) {
		SPDK_ERRLOG("Renaming lvol failed\n");
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

/*
 * [한국어]
 * vbdev_lvol_rename - lvol 이름 변경 외부 API.
 *
 * @lvol: 이름 변경 대상.
 * @new_lvol_name: 새 이름.
 * @cb_fn / @cb_arg: 비동기 완료 콜백.
 *
 * 처리 순서:
 *  (1) bdev alias 먼저 갱신 — 실패 시 즉시 종료 (lvol 메타는 건드리지 않음).
 *  (2) lib/lvol 의 spdk_lvol_rename 호출하여 디스크 메타데이터 업데이트.
 *  (3) 콜백에서 사용자에 통보.
 *
 * 알 alias 갱신을 먼저 해야 race 가 줄어든다 — alias 충돌이 있으면 디스크 메타까지 갈
 * 필요 없이 즉시 거부 가능.
 *
 * 호출 체인: RPC rpc_bdev_lvol_rename → [이 함수] → _vbdev_lvol_change_bdev_alias →
 *           spdk_lvol_rename → _vbdev_lvol_rename_cb
 */
void
vbdev_lvol_rename(struct spdk_lvol *lvol, const char *new_lvol_name,
		  spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	int rc;

	rc = _vbdev_lvol_change_bdev_alias(lvol, new_lvol_name);
	/* [한국어] bdev alias 부터 갱신 — alias 충돌 등 조기 실패 검출 */
	if (rc != 0) {
		SPDK_ERRLOG("renaming lvol to '%s' does not succeed\n", new_lvol_name);
		cb_fn(cb_arg, rc);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_lvol_rename(lvol, new_lvol_name, _vbdev_lvol_rename_cb, req);
	/* [한국어] lib/lvol 이 lvol 의 디스크 메타데이터 (이름 필드) 를 업데이트 */
}

/*
 * [한국어]
 * _vbdev_lvol_resize_cb - spdk_lvol_resize 완료 콜백. bdev blockcnt 동기화.
 *
 * @cb_arg: vbdev_lvol_resize 에서 alloc 한 spdk_lvol_req.
 * @lvolerrno: lib/lvol resize 결과.
 *
 * lvol 의 논리 크기가 바뀌면 bdev 의 blockcnt 도 동기화해야 한다. spdk_bdev_notify_blockcnt_change
 * 가 RESIZE 이벤트를 통해 모든 옵저버에게 새 크기를 통보. notify 실패도 errno 로 보고하지만
 * 메타데이터는 이미 변경되어 있으므로 호출자가 다시 호출하면 정합성 회복 가능.
 */
static void
_vbdev_lvol_resize_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;
	uint64_t total_size;

	/* change bdev size */
	if (lvolerrno != 0) {
		/* [한국어] lvol resize 실패 — bdev 크기 변경하지 않고 사용자에 보고 */
		SPDK_ERRLOG("CB function for bdev lvol %s receive error no: %d.\n", lvol->name, lvolerrno);
		goto finish;
	}

	total_size = spdk_blob_get_num_clusters(lvol->blob) *
		     spdk_bs_get_cluster_size(lvol->lvol_store->blobstore);
	/* [한국어] 새 논리 크기 계산 (resize 후 cluster 수가 변했음) */
	assert((total_size % lvol->bdev->blocklen) == 0);

	lvolerrno = spdk_bdev_notify_blockcnt_change(lvol->bdev, total_size / lvol->bdev->blocklen);
	/* [한국어] bdev RESIZE 이벤트 발행 — 옵저버(예: NVMe-oF target)가 자기 publish 크기도 갱신 */
	if (lvolerrno != 0) {
		SPDK_ERRLOG("Could not change num blocks for bdev lvol %s with error no: %d.\n",
			    lvol->name, lvolerrno);
	}

finish:
	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

/*
 * [한국어]
 * vbdev_lvol_resize - lvol 논리 크기 변경 외부 API.
 *
 * @lvol: 리사이즈 대상.
 * @sz: 새 크기 (바이트).
 * @cb_fn / @cb_arg: 비동기 완료 콜백.
 *
 * lib/lvol 위임 → 콜백에서 bdev blockcnt 동기화. thin/thick 모두 지원. shrink 도 가능하나
 * 데이터 손실 위험이 있음.
 *
 * 호출 체인: RPC rpc_bdev_lvol_resize → [이 함수] → spdk_lvol_resize → _vbdev_lvol_resize_cb
 */
void
vbdev_lvol_resize(struct spdk_lvol *lvol, uint64_t sz, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	assert(lvol->bdev != NULL);
	/* [한국어] degraded lvol 은 bdev 가 없어 resize 시도 의미 없음 — 호출자 책임 */

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->sz = sz;
	/* [한국어] 콜백에서 새 크기를 참조할 수 있게 저장 */
	req->lvol = lvol;
	/* [한국어] 콜백에서 bdev 동기화 시 lvol→bdev 경로로 접근 */

	spdk_lvol_resize(req->lvol, req->sz, _vbdev_lvol_resize_cb, req);
}

/*
 * [한국어]
 * _vbdev_lvol_set_read_only_cb - spdk_lvol_set_read_only 완료 콜백.
 *
 * @cb_arg: 요청 컨텍스트.
 * @lvolerrno: 결과.
 *
 * read-only 전환은 디스크 메타데이터에 영구 기록되며, 이후 io_type_supported 에서 write
 * 류 IO 가 자동 거부된다. 사용자에 결과만 통보.
 */
static void
_vbdev_lvol_set_read_only_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (lvolerrno != 0) {
		SPDK_ERRLOG("Could not set bdev lvol %s as read only due to error: %d.\n", lvol->name, lvolerrno);
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

/*
 * [한국어]
 * vbdev_lvol_set_read_only - lvol 을 read-only 로 전환 외부 API.
 *
 * @lvol: 대상.
 * @cb_fn / @cb_arg: 비동기 완료 콜백.
 *
 * 한 번 적용되면 writeable 로 되돌리는 단순 RPC 가 없다 (디스크 메타에 영구 기록).
 *
 * 호출 체인: RPC rpc_bdev_lvol_set_read_only → [이 함수] → spdk_lvol_set_read_only →
 *           _vbdev_lvol_set_read_only_cb
 */
void
vbdev_lvol_set_read_only(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	assert(lvol->bdev != NULL);

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;

	spdk_lvol_set_read_only(lvol, _vbdev_lvol_set_read_only_cb, req);
}

/*
 * [한국어]
 * vbdev_lvs_init - bdev module_init 콜백. no-op (lvol store 는 examine 단계에서 발견).
 *
 * @return: 0 (성공).
 *
 * SPDK bdev framework 의 module_init 호출 시점에는 아직 어떤 베이스 bdev 도 등장하지
 * 않았으므로 lvol 모듈이 할 일은 없다. 실제 lvol store 발견은 examine_disk 에서 일어남.
 */
static int
vbdev_lvs_init(void)
{
	return 0;
}

/* [한국어] forward declaration — fini_start_unload_cb 와 상호 호출 */
static void vbdev_lvs_fini_start_iter(struct lvol_store_bdev *lvs_bdev);

/*
 * [한국어]
 * vbdev_lvs_fini_start_unload_cb - shutdown 중 한 lvs unload 완료 후 다음 lvs 진행.
 *
 * @cb_arg: 방금 unload 된 lvs_bdev.
 * @lvserrno: unload 결과.
 *
 * 종료 시 모든 lvs 를 순차적으로 unload 한다. 한 lvs 가 끝나면 본 함수가 (1) 페어 리스트
 * 에서 제거, (2) 페어 객체 free, (3) 다음 lvs 로 iter 진행. 마지막에 fini_start_done 호출.
 */
static void
vbdev_lvs_fini_start_unload_cb(void *cb_arg, int lvserrno)
{
	struct lvol_store_bdev *lvs_bdev = cb_arg;
	struct lvol_store_bdev *next_lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	/* [한국어] 미리 다음 노드 포인터 확보 — 본인 free 전 */

	if (lvserrno != 0) {
		SPDK_INFOLOG(vbdev_lvol, "Lvol store removed with error: %d.\n", lvserrno);
	}

	TAILQ_REMOVE(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);
	free(lvs_bdev);

	vbdev_lvs_fini_start_iter(next_lvs_bdev);
	/* [한국어] 다음 lvs 진행 — 끝까지 진행하면 fini_start_done 호출 */
}

/*
 * [한국어]
 * vbdev_lvs_fini_start_iter - 모든 lvs 를 차례로 unload 하는 비동기 iter.
 *
 * @lvs_bdev: 시작 페어 (NULL 이면 종료).
 *
 * 모든 lvol 이 이미 close 된 lvs 만 unload 가능 (그렇지 않으면 다른 코드 경로가 close 를
 * 진행 중일 것이며, 그 콜백이 다시 fini 흐름을 이어간다). 모든 lvs 가 정리되면
 * spdk_bdev_module_fini_start_done 호출하여 bdev framework 에 모듈 종료 가능 신호.
 *
 * 호출 체인: vbdev_lvs_fini_start → [이 함수] → spdk_lvs_unload →
 *           vbdev_lvs_fini_start_unload_cb → 재귀 → spdk_bdev_module_fini_start_done
 */
static void
vbdev_lvs_fini_start_iter(struct lvol_store_bdev *lvs_bdev)
{
	struct spdk_lvol_store *lvs;

	while (lvs_bdev != NULL) {
		lvs = lvs_bdev->lvs;

		if (_vbdev_lvs_are_lvols_closed(lvs)) {
			/* [한국어] 모든 lvol close 완료 → 이 lvs unload */
			spdk_lvs_unload(lvs, vbdev_lvs_fini_start_unload_cb, lvs_bdev);
			return;
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
		/* [한국어] 아직 close 중인 lvol 이 있음 — 그쪽 콜백이 fini 흐름을 이어감.
		 * 여기서는 다음 lvs 만 확인 */
	}

	spdk_bdev_module_fini_start_done();
	/* [한국어] 모든 lvs unload 완료 — bdev framework 에 모듈 종료 가능 신호 */
}

/*
 * [한국어]
 * vbdev_lvs_fini_start - bdev module fini_start 콜백. shutdown 절차 시작.
 *
 * SPDK app 종료 시 bdev framework 가 모듈별 fini_start 를 호출. async_fini_start=true 이므로
 * 비동기 완료 시 spdk_bdev_module_fini_start_done 호출 필수.
 */
static void
vbdev_lvs_fini_start(void)
{
	g_shutdown_started = true;
	/* [한국어] 다른 unregister 콜백이 lvs unload 를 자동 트리거하게 하는 신호 */
	vbdev_lvs_fini_start_iter(vbdev_lvol_store_first());
}

/*
 * [한국어]
 * vbdev_lvs_get_ctx_size - bdev_io 당 driver_ctx 바이트 수 반환.
 *
 * @return: sizeof(struct vbdev_lvol_io).
 *
 * bdev 코어가 모듈별 ctx_size 의 최댓값을 잡아 모든 bdev_io 의 driver_ctx 영역을 보장.
 */
static int
vbdev_lvs_get_ctx_size(void)
{
	return sizeof(struct vbdev_lvol_io);
}

/*
 * [한국어]
 * _vbdev_lvs_examine_done - examine(자동 로드) 작업 전체의 종료를 사용자 콜백으로 보고하는 헬퍼.
 *
 * @req: examine 작업을 추적하는 요청 컨텍스트 (cb_fn/cb_arg 보유).
 * @lvserrno: 최종 결과 errno (0=lvs 로드 + 모든 lvol open 성공).
 *
 * examine 경로는 여러 단계(bs_dev 생성 → lvs load → 각 lvol open)에 걸친 비동기 체인이며,
 * 그 모든 단계가 성공/실패로 귀결되는 지점에서 이 한 줄 헬퍼를 통해 req->cb_fn 을 호출한다.
 * 실패 경로/성공 경로 모두 결국 이 함수로 수렴하도록 만들어 종료 보고를 단일화한다.
 * req->cb_fn 은 보통 vbdev_lvs_examine_done(=spdk_bdev_module_examine_done 호출)이다.
 *
 * 실행 컨텍스트: lvol 모듈 단일 스레드 (examine 콜백 체인).
 *
 * 호출 체인: _vbdev_lvs_examine_finish / _vbdev_lvs_examine_failed / _vbdev_lvs_examine_cb →
 *           [이 함수] → req->cb_fn (= vbdev_lvs_examine_done)
 */
static void
_vbdev_lvs_examine_done(struct spdk_lvs_req *req, int lvserrno)
{
	req->cb_fn(req->cb_arg, lvserrno);
	/* [한국어] 누적된 결과 errno 를 상위(보통 bdev examine 프레임워크)로 비동기 보고 */
}

/*
 * [한국어]
 * _vbdev_lvs_examine_failed - claim/alloc 실패로 lvs 를 다시 unload 한 뒤의 정리 콜백.
 *
 * @cb_arg: examine 요청 컨텍스트 (실패 errno 가 req->lvserrno 에 미리 저장됨).
 * @lvserrno: spdk_lvs_unload 결과 — 여기서는 무시하고 보존해 둔 req->lvserrno 를 보고한다.
 *
 * _vbdev_lvs_examine_cb 에서 lvs 를 load 했지만 base bdev claim 또는 페어 alloc 에 실패한
 * 경우, 이미 메모리에 올라온 lvs 를 다시 spdk_lvs_unload 해야 한다. 그 unload 가 끝나면 이
 * 콜백이 호출되어, 원래의 실패 원인(req->lvserrno)을 사용자에게 전달한다. unload 자체의
 * errno 가 아니라 *원래 실패 원인*을 보고하는 것이 핵심이다.
 *
 * 실행 컨텍스트: lib/lvol 의 unload 완료 콜백 스레드 (= 본 모듈 스레드).
 *
 * 호출 체인: _vbdev_lvs_examine_cb (claim/alloc 실패) → spdk_lvs_unload → [이 함수] →
 *           _vbdev_lvs_examine_done
 */
static void
_vbdev_lvs_examine_failed(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_req *req = cb_arg;
	/* [한국어] examine 요청 컨텍스트 복원 */

	_vbdev_lvs_examine_done(req, req->lvserrno);
	/* [한국어] unload 결과(lvserrno)는 버리고, 미리 저장된 원래 실패 원인을 보고 */
}

/*
 * [한국어]
 * _vbdev_lvs_examine_finish - lvs load 후 각 lvol 을 하나씩 open 하며 bdev 로 등록하는 콜백.
 *
 * @cb_arg: examine 요청 컨텍스트 (lvol_store 백포인터, opened/count 카운터 추적).
 * @lvol: 방금 open 시도가 끝난 lvol.
 * @lvolerrno: 그 open 결과 (0=성공, -ENOMEM=메모리 부족 일시 실패, 기타=영구 실패).
 *
 * lvs 로드 직후 모든 lvol 에 대해 spdk_lvol_open 이 발행되며, 각 open 의 완료가 이 콜백으로
 * 들어온다. 처리 분기:
 *   (1) -ENOMEM: 일시적 자원 부족 → lvols 에서 빼서 retry_open_lvols 로 옮기고 나중에 재시도.
 *   (2) 기타 에러: 영구 실패 → lvol 을 리스트에서 제거하고 free, count 감소.
 *   (3) 성공: _create_lvol_disk 로 spdk_bdev 등록, lvols_opened 증가.
 * 마지막에 retry 큐에 대기 중인 lvol 이 있으면 그것부터 다시 open 시도하고, 없으면서 모든
 * lvol 이 처리(opened >= count)되었으면 examine 종료를 보고한다.
 *
 * 실행 컨텍스트: lib/lvol open 완료 콜백 스레드 (= 본 모듈 스레드). 재귀적으로 자기 자신이
 * 다음 lvol 의 open 콜백으로 다시 등록된다.
 *
 * 호출 체인: spdk_lvol_open → [이 함수] → (성공)_create_lvol_disk / (재시도)spdk_lvol_open /
 *           (완료)_vbdev_lvs_examine_done
 */
static void
_vbdev_lvs_examine_finish(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_lvs_req *req = cb_arg;
	/* [한국어] examine 요청 컨텍스트 복원 */
	struct spdk_lvol_store *lvs = req->lvol_store;
	/* [한국어] 이 lvol 이 속한 lvs — opened/count 카운터와 lvols/retry 큐 보유 */

	if (lvolerrno != 0) {
		/* [한국어] open 실패 — 해당 lvol 을 일단 활성 리스트에서 분리 */
		TAILQ_REMOVE(&lvs->lvols, lvol, link);
		if (lvolerrno == -ENOMEM) {
			/* [한국어] 일시적 메모리 부족 — 영구 실패로 보지 않고 retry 큐에 보관.
			 * end: 라벨에서 retry 큐를 비우며 재시도하므로 여기서는 곧장 return. */
			TAILQ_INSERT_TAIL(&lvs->retry_open_lvols, lvol, link);
			return;
		}
		/* [한국어] 그 외 에러는 영구 실패 — lvol 객체를 폐기하고 카운트 감소 */
		SPDK_ERRLOG("Error opening lvol %s\n", lvol->unique_id);
		lvs->lvol_count--;
		/* [한국어] 더 이상 열 lvol 개수에서 제외 (종료 조건 opened>=count 보정) */
		free(lvol);
		/* [한국어] 등록 전 단계이므로 lvol 메모리 직접 해제 */
		goto end;
	}

	if (_create_lvol_disk(lvol, false)) {
		/* [한국어] open 은 됐으나 bdev 등록 실패 — lvol 자체는 lib/lvol 이 소유하므로
		 * 여기서 free 하지 않고 카운트만 보정. false = examine 시점이라 alias 등록 안 함 */
		SPDK_ERRLOG("Cannot create bdev for lvol %s\n", lvol->unique_id);
		lvs->lvol_count--;
		goto end;
	}

	lvs->lvols_opened++;
	/* [한국어] 성공적으로 bdev 까지 등록된 lvol 수 증가 — 종료 조건 판정에 사용 */
	SPDK_INFOLOG(vbdev_lvol, "Opening lvol %s succeeded\n", lvol->unique_id);

end:
	if (!TAILQ_EMPTY(&lvs->retry_open_lvols)) {
		/* [한국어] -ENOMEM 으로 미뤄둔 lvol 이 있으면 우선 그것을 다시 open 시도.
		 * 한 번에 하나씩 직렬 재시도하여 메모리 압박을 완화한다. */
		lvol = TAILQ_FIRST(&lvs->retry_open_lvols);
		TAILQ_REMOVE(&lvs->retry_open_lvols, lvol, link);
		/* [한국어] retry 큐에서 꺼내 */
		TAILQ_INSERT_HEAD(&lvs->lvols, lvol, link);
		/* [한국어] 다시 활성 리스트로 복귀시킨 뒤 */
		spdk_lvol_open(lvol, _vbdev_lvs_examine_finish, req);
		/* [한국어] 재 open — 완료 시 이 함수가 다시 콜백된다 (재귀 직렬화) */
		return;
	}
	if (lvs->lvols_opened >= lvs->lvol_count) {
		/* [한국어] 열려야 할 모든 lvol 이 처리됨 (성공+영구실패 보정 포함) → examine 종료 */
		SPDK_INFOLOG(vbdev_lvol, "Opening lvols finished\n");
		_vbdev_lvs_examine_done(req, 0);
		/* [한국어] examine 성공 보고 (개별 lvol 실패는 위에서 카운트로 흡수) */
	}
}

/* Walks a tree of clones that are no longer degraded to create bdevs. */
/*
 * [한국어]
 * create_esnap_clone_lvol_disks - esnap 부모가 (다시) 등장해 degraded 가 해제된 clone 트리를
 *                                 따라 내려가며 각 lvol 을 bdev 로 등록한다.
 *
 * @ctx: hotplug 트리거가 된 esnap 부모 bdev (로그 식별용으로만 사용).
 * @lvol: 현재 처리 중인 (더 이상 degraded 가 아닌) clone lvol.
 * @return: 0 = 이 노드와 모든 후손 처리 완료 (개별 실패는 0 으로 흡수).
 *
 * external snapshot(esnap)의 부모 bdev 가 부재하면 그 위의 clone lvol 들은 degraded 상태로
 * bdev 가 등록되지 않는다. 나중에 부모 bdev 가 hotplug 로 다시 나타나면 그 시점에 clone 과
 * 그 하위 clone 들(스냅샷 트리)을 모두 정상 bdev 로 만들어야 한다. 이 함수는 한 노드를
 * _create_lvol_disk 로 등록한 뒤, spdk_lvol_iter_immediate_clones 로 직계 자식 clone 들에
 * 대해 자기 자신을 재귀 적용하여 트리 전체를 깊이 우선으로 등록한다. 한 노드 등록이
 * 실패해도 0 을 반환해 형제/후손의 등록을 막지 않는다(부분 복구 허용).
 *
 * 실행 컨텍스트: lvol 모듈 단일 스레드 (examine_config → hotplug 콜백 경로).
 *
 * 호출 체인: vbdev_lvs_hotplug → [이 함수] (재귀) → _create_lvol_disk /
 *           spdk_lvol_iter_immediate_clones
 */
static int
create_esnap_clone_lvol_disks(void *ctx, struct spdk_lvol *lvol)
{
	struct spdk_bdev *bdev = ctx;
	/* [한국어] hotplug 의 원인이 된 부모 esnap bdev — 로그용 식별자 */
	int rc;

	rc = _create_lvol_disk(lvol, false);
	/* [한국어] 이 clone lvol 을 spdk_bdev 로 등록. false = hotplug 경로라 alias 즉시 추가 안 함 */
	if (rc != 0) {
		SPDK_ERRLOG("lvol %s: failed to create bdev after esnap hotplug of %s: %d\n",
			    lvol->unique_id, spdk_bdev_get_name(bdev), rc);
		/* Do not prevent creation of other clones in case of one failure. */
		/* [한국어] 한 노드 실패가 트리 전체 복구를 막지 않도록 0 반환 (best-effort) */
		return 0;
	}

	return spdk_lvol_iter_immediate_clones(lvol, create_esnap_clone_lvol_disks, ctx);
	/* [한국어] 직계 clone 들에 대해 동일 함수를 재귀 적용 — 스냅샷 트리 전체 등록 */
}

/*
 * [한국어]
 * vbdev_lvs_hotplug - 부재했던 esnap 부모 bdev 가 다시 나타났을 때 clone 트리를 복구하는 콜백.
 *
 * @ctx: 등장한 esnap 부모 bdev.
 * @lvol: 이 부모를 esnap 으로 쓰는 (방금 재오픈된) clone lvol.
 * @lvolerrno: clone lvol 재오픈 결과 (0=성공).
 *
 * spdk_lvs_notify_hotplug 가 "이 UUID 를 esnap 부모로 기다리던 clone" 을 찾아 재오픈한 뒤
 * 그 결과를 이 콜백으로 전달한다. 성공 시 create_esnap_clone_lvol_disks 로 트리 전체를 bdev
 * 로 등록하여 degraded 상태를 해제한다. 실패 시 로그만 남기고 등록을 생략한다.
 *
 * 실행 컨텍스트: lvol 모듈 단일 스레드 (examine_config 경로).
 *
 * 호출 체인: vbdev_lvs_examine_config → spdk_lvs_notify_hotplug → [이 함수] →
 *           create_esnap_clone_lvol_disks
 */
static void
vbdev_lvs_hotplug(void *ctx, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_bdev *esnap_clone_bdev = ctx;
	/* [한국어] 새로 등장한 esnap 부모 bdev */

	if (lvolerrno != 0) {
		/* [한국어] clone 재오픈 실패 — bdev 등록 생략하고 종료 (lvol 은 degraded 유지) */
		SPDK_ERRLOG("lvol %s: during examine of bdev %s: not creating clone bdev due to "
			    "error %d\n", lvol->unique_id, spdk_bdev_get_name(esnap_clone_bdev),
			    lvolerrno);
		return;
	}
	create_esnap_clone_lvol_disks(esnap_clone_bdev, lvol);
	/* [한국어] clone 과 그 후손 트리를 모두 정상 bdev 로 등록 (degraded 해제) */
}

/*
 * [한국어]
 * vbdev_lvs_examine_config - examine "config" 단계 콜백. 등장한 bdev 가 누군가의 esnap 부모인지 확인.
 *
 * @bdev: bdev framework 가 새로 발견하여 검사 기회를 준 bdev.
 *
 * bdev examine 은 두 단계(config → disk)로 진행된다. config 단계에서는 디스크 헤더를 읽기
 * 전, 이 bdev 의 UUID 가 어떤 lvol 의 "부재 중인 esnap 부모" 와 일치하는지를 lvol 레이어에
 * 문의한다(spdk_lvs_notify_hotplug). 일치하는 degraded clone 이 있으면 그쪽을 hotplug 하여
 * 복구한다. 어느 경우든 마지막에 spdk_bdev_module_examine_done 으로 이 단계 완료를 알려야
 * 다음 모듈/단계로 진행된다(이 호출을 빠뜨리면 examine 파이프라인이 멈춘다).
 *
 * 실행 컨텍스트: bdev examine 프레임워크가 호출 (lvol 모듈 스레드).
 *
 * 호출 체인: bdev examine framework → [이 함수] → spdk_lvs_notify_hotplug(→vbdev_lvs_hotplug) →
 *           spdk_bdev_module_examine_done
 */
static void
vbdev_lvs_examine_config(struct spdk_bdev *bdev)
{
	char uuid_str[SPDK_UUID_STRING_LEN];
	/* [한국어] bdev UUID 의 표준 문자열 표현 버퍼 — esnap 부모 매칭 키 */

	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);
	/* [한국어] bdev 의 UUID 를 소문자 표준 포맷 문자열로 변환 (bdev names 트리와 동일 포맷) */

	if (spdk_lvs_notify_hotplug(uuid_str, sizeof(uuid_str), vbdev_lvs_hotplug, bdev)) {
		/* [한국어] 이 UUID 를 esnap 부모로 기다리던 degraded clone 이 있으면 true.
		 * 콜백 vbdev_lvs_hotplug 가 그 clone 트리를 복구한다. */
		SPDK_INFOLOG(vbdev_lvol, "bdev %s: claimed by one or more esnap clones\n",
			     uuid_str);
	}
	spdk_bdev_module_examine_done(&g_lvol_if);
	/* [한국어] config 단계 완료 통보 — 필수. 빠뜨리면 examine 파이프라인이 정지 */
}

/*
 * [한국어]
 * _vbdev_lvs_examine_cb - spdk_lvs_load 완료 콜백. 디스크에서 발견한 lvs 를 페어 등록하고 모든 lvol open 시작.
 *
 * @arg: spdk_lvs_with_handle_req (base_bdev, 그리고 원래 examine req 를 cb_arg 로 보유).
 * @lvol_store: 디스크에서 로드된 lvol_store 핸들 (성공 시), 실패 시 의미 없음.
 * @lvserrno: load 결과 (0=성공, -EEXIST=이름 충돌, 기타=lvs 없음/에러).
 *
 * vbdev_lvs_examine_disk → _vbdev_lvs_examine → spdk_lvs_load_ext 로 베이스 bdev 위에 lvs
 * 가 있는지 시도한 결과가 이 콜백으로 들어온다. 처리:
 *   - -EEXIST: 같은 이름의 lvs 가 이미 로드됨 → 실패 보고 (blobstore 가 bs_dev 자체 정리).
 *   - 기타 에러: lvs 없음/손상 → 실패 보고.
 *   - 성공: (1) 베이스 bdev 를 lvol 모듈 명의로 claim, (2) (lvs,bdev) 페어 alloc 후 글로벌
 *     TAILQ 등록, (3) lvols 비어 있으면 즉시 종료, 아니면 모든 lvol 에 대해 spdk_lvol_open
 *     발행 — 각 open 완료는 _vbdev_lvs_examine_finish 가 받아 bdev 로 등록.
 * claim/alloc 실패 시 이미 메모리에 올라온 lvs 를 다시 unload 한다(_vbdev_lvs_examine_failed).
 *
 * 실행 컨텍스트: lib/lvol load 완료 콜백 스레드 (= 본 모듈 스레드).
 *
 * 호출 체인: spdk_lvs_load_ext → [이 함수] → spdk_bs_bdev_claim / spdk_lvol_open(→finish) /
 *           (실패)spdk_lvs_unload(→_vbdev_lvs_examine_failed)
 */
static void
_vbdev_lvs_examine_cb(void *arg, struct spdk_lvol_store *lvol_store, int lvserrno)
{
	struct lvol_store_bdev *lvs_bdev;
	/* [한국어] 새로 만들 (lvs, 베이스 bdev) 페어 */
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)arg;
	/* [한국어] _vbdev_lvs_examine 가 alloc 한 내부 요청 — base_bdev 와 원래 req 보유 */
	struct spdk_lvol *lvol, *tmp;
	/* [한국어] lvol 순회용 (open 발행 시 _SAFE 순회 필요) */
	struct spdk_lvs_req *ori_req = req->cb_arg;
	/* [한국어] 최초 examine 요청 — 최종 종료 보고 대상 */

	if (lvserrno == -EEXIST) {
		/* [한국어] 동일 이름 lvs 가 이미 로드되어 있음 — 중복 로드 방지 */
		SPDK_INFOLOG(vbdev_lvol,
			     "Name for lvolstore on device %s conflicts with name for already loaded lvs\n",
			     req->base_bdev->name);
		/* On error blobstore destroys bs_dev itself */
		/* [한국어] 실패 시 bs_dev 정리는 blobstore 가 담당하므로 여기서는 보고만 */
		_vbdev_lvs_examine_done(ori_req, lvserrno);
		goto end;
	} else if (lvserrno != 0) {
		/* [한국어] 이 베이스 bdev 에는 lvs 가 없거나 헤더 손상 — 정상적인 비-lvol 디스크 */
		SPDK_INFOLOG(vbdev_lvol, "Lvol store not found on %s\n", req->base_bdev->name);
		/* On error blobstore destroys bs_dev itself */
		_vbdev_lvs_examine_done(ori_req, lvserrno);
		goto end;
	}

	lvserrno = spdk_bs_bdev_claim(lvol_store->bs_dev, &g_lvol_if);
	/* [한국어] 발견한 lvs 의 베이스 bdev 를 lvol 모듈 명의로 점유 — 타 모듈 동시 사용 차단 */
	if (lvserrno != 0) {
		/* [한국어] 이미 다른 모듈이 점유 중 → 방금 로드한 lvs 를 도로 unload */
		SPDK_INFOLOG(vbdev_lvol, "Lvol store base bdev already claimed by another bdev\n");
		ori_req->lvserrno = lvserrno;
		/* [한국어] 원래 실패 원인 보존 — unload 콜백이 이 값으로 보고 */
		spdk_lvs_unload(lvol_store, _vbdev_lvs_examine_failed, ori_req);
		goto end;
	}

	lvs_bdev = calloc(1, sizeof(*lvs_bdev));
	/* [한국어] 페어 객체 alloc — 글로벌 TAILQ 에 lvs 수명만큼 유지 */
	if (!lvs_bdev) {
		/* [한국어] alloc 실패 → claim 한 lvs 도 도로 unload (정합성 복원) */
		SPDK_ERRLOG("Cannot alloc memory for lvs_bdev\n");
		ori_req->lvserrno = lvserrno;
		spdk_lvs_unload(lvol_store, _vbdev_lvs_examine_failed, ori_req);
		goto end;
	}

	lvs_bdev->lvs = lvol_store;
	/* [한국어] 페어 한쪽: 로드된 lvs */
	lvs_bdev->bdev = req->base_bdev;
	/* [한국어] 페어 다른 한쪽: 베이스 bdev */

	TAILQ_INSERT_TAIL(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);
	/* [한국어] 글로벌 페어 리스트 등록 — 이제 RPC iter/hotremove lookup 에 보임 */

	SPDK_INFOLOG(vbdev_lvol, "Lvol store found on %s - begin parsing\n",
		     req->base_bdev->name);

	lvol_store->lvols_opened = 0;
	/* [한국어] open 진행 카운터 초기화 — _vbdev_lvs_examine_finish 가 증가시키며 종료 판정 */

	ori_req->lvol_store = lvol_store;
	/* [한국어] open 콜백이 lvs->lvols/retry 큐에 접근할 수 있도록 백포인터 저장 */

	if (TAILQ_EMPTY(&lvol_store->lvols)) {
		/* [한국어] lvol 이 하나도 없는 빈 lvs → 즉시 examine 완료 */
		SPDK_INFOLOG(vbdev_lvol, "Lvol store examination done\n");
		_vbdev_lvs_examine_done(ori_req, 0);
	} else {
		/* Open all lvols */
		/* [한국어] 모든 lvol open 발행 — 각 완료는 _vbdev_lvs_examine_finish 가 bdev 등록 */
		TAILQ_FOREACH_SAFE(lvol, &lvol_store->lvols, link, tmp) {
			/* [한국어] _SAFE: finish 콜백이 실패 lvol 을 리스트에서 제거할 수 있음 */
			spdk_lvol_open(lvol, _vbdev_lvs_examine_finish, ori_req);
		}
	}

end:
	free(req);
	/* [한국어] 내부 with_handle 요청 해제 (ori_req 는 별도 수명) */
}

/*
 * [한국어]
 * _vbdev_lvs_examine - 베이스 bdev 위에 bs_dev 를 만들고 주어진 action(load/import)을 시작.
 *
 * @bdev: 검사 대상 베이스 bdev.
 * @ori_req: examine 작업 추적 요청 (최종 종료 보고 대상).
 * @action: bs_dev 에 대해 수행할 동작 — 보통 vbdev_lvs_load (디스크에서 lvs 로드).
 *
 * vbdev_lvs_examine_disk 가 호출하는 공통 헬퍼. (1) 내부 with_handle 요청 alloc, (2) 베이스
 * bdev 위에 blobstore-device 어댑터 생성(hotremove event_cb 등록), (3) action 콜백으로 lvs
 * 로드/임포트를 시작한다. action 의 완료는 _vbdev_lvs_examine_cb 가 받는다. action 을 함수
 * 포인터로 받아 load 외 다른 동작(future)에도 재사용 가능하게 했다.
 *
 * 실행 컨텍스트: examine 프레임워크 경로 (lvol 모듈 스레드).
 *
 * 호출 체인: vbdev_lvs_examine_disk → [이 함수] → spdk_bdev_create_bs_dev_ext →
 *           action(=vbdev_lvs_load) → _vbdev_lvs_examine_cb
 */
static void
_vbdev_lvs_examine(struct spdk_bdev *bdev, struct spdk_lvs_req *ori_req,
		   void (*action)(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg))
{
	struct spdk_bs_dev *bs_dev;
	/* [한국어] 베이스 bdev 위 blobstore-device 어댑터 — action 의 입력 */
	struct spdk_lvs_with_handle_req *req;
	/* [한국어] _vbdev_lvs_examine_cb 까지 살아남을 내부 요청 컨텍스트 */
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		/* [한국어] 요청 alloc 실패 — 즉시 examine 종료 보고 */
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		_vbdev_lvs_examine_done(ori_req, -ENOMEM);
		return;
	}

	rc = spdk_bdev_create_bs_dev_ext(bdev->name, vbdev_lvs_base_bdev_event_cb,
					 NULL, &bs_dev);
	/* [한국어] 베이스 bdev 위 bs_dev 생성 + hotremove event_cb 등록. 실패 시 lvs 없음과 동일 처리 */
	if (rc < 0) {
		SPDK_INFOLOG(vbdev_lvol, "Cannot create bs dev on %s\n", bdev->name);
		_vbdev_lvs_examine_done(ori_req, rc);
		free(req);
		return;
	}

	req->base_bdev = bdev;
	/* [한국어] 콜백에서 페어 등록/로그에 사용할 베이스 bdev */
	req->cb_arg = ori_req;
	/* [한국어] 최종 보고 대상 원본 요청을 cb_arg 로 체이닝 */

	action(bs_dev, _vbdev_lvs_examine_cb, req);
	/* [한국어] 실제 lvs 로드 시작 — 완료는 _vbdev_lvs_examine_cb 로 */
}

/*
 * [한국어]
 * vbdev_lvs_examine_done - examine_disk 작업의 최종 완료 콜백. examine_done 통보 후 req 해제.
 *
 * @arg: examine 요청 컨텍스트 (자기 자신을 cb_arg 로 가리킴).
 * @lvserrno: examine 결과 (성공/실패 무관 — examine 은 "검사 완료" 만 통보).
 *
 * lvs 로드 성공 여부와 무관하게, 이 bdev 에 대한 lvol 모듈의 검사가 끝났음을 bdev
 * 프레임워크에 알려야(spdk_bdev_module_examine_done) 다음 단계로 진행된다. lvol 이 없는
 * 일반 디스크여도 정상 종료로 본다.
 *
 * 호출 체인: _vbdev_lvs_examine_done(=req->cb_fn) → [이 함수] → spdk_bdev_module_examine_done
 */
static void
vbdev_lvs_examine_done(void *arg, int lvserrno)
{
	struct spdk_lvs_req *req = arg;
	/* [한국어] examine 요청 컨텍스트 복원 */

	spdk_bdev_module_examine_done(&g_lvol_if);
	/* [한국어] disk 단계 검사 완료 통보 — 필수. 빠뜨리면 examine 파이프라인 정지 */
	free(req);
	/* [한국어] 요청 컨텍스트 해제 */
}

/*
 * [한국어]
 * vbdev_lvs_load - bs_dev 로부터 esnap 생성 콜백을 설정해 lvs 를 로드하는 action 함수.
 *
 * @bs_dev: 로드할 lvs 의 베이스 blobstore-device.
 * @cb_fn / @cb_arg: 로드 완료 콜백 (= _vbdev_lvs_examine_cb).
 *
 * _vbdev_lvs_examine 의 action 파라미터로 전달되는 구체 동작. lvs_opts 를 기본 초기화한 뒤
 * esnap_bs_dev_create 콜백을 본 모듈 함수로 등록한다 — 로드되는 lvs 안에 external snapshot
 * clone 이 있으면 그 부모 bdev 를 동적으로 열 수 있게 하기 위함이다. 이후 spdk_lvs_load_ext
 * 로 실제 디스크 메타데이터를 파싱한다.
 *
 * 호출 체인: _vbdev_lvs_examine → [이 함수] → spdk_lvs_load_ext → _vbdev_lvs_examine_cb
 */
static void
vbdev_lvs_load(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_opts lvs_opts;
	/* [한국어] lvs 로드 옵션 — esnap 콜백 등록용 */

	spdk_lvs_opts_init(&lvs_opts);
	/* [한국어] 기본값 초기화 후 필요한 필드만 덮어쓴다 */
	lvs_opts.esnap_bs_dev_create = vbdev_lvol_esnap_dev_create;
	/* [한국어] 로드된 lvs 의 esnap clone 부모를 동적으로 열기 위한 콜백 등록 */
	spdk_lvs_load_ext(bs_dev, &lvs_opts, cb_fn, cb_arg);
	/* [한국어] 디스크의 lvs 메타데이터 파싱 시작 — 완료는 cb_fn(_vbdev_lvs_examine_cb) */
}

/*
 * [한국어]
 * vbdev_lvs_examine_disk - examine "disk" 단계 콜백. 베이스 bdev 헤더를 읽어 lvs 자동 로드.
 *
 * @bdev: 검사 대상 베이스 bdev.
 *
 * config 단계 다음에 호출되는 disk 단계 examine. 실제 디스크 헤더를 읽어 lvol store 가 있으면
 * 로드한다. 단, metadata(DIF/PI) 가 포맷된 bdev 는 blobstore 가 지원하지 않으므로 즉시
 * examine_done 으로 건너뛴다. 그 외에는 요청 컨텍스트를 만들어 _vbdev_lvs_examine 로 로드를
 * 시작한다. 최종 완료는 vbdev_lvs_examine_done 이 examine_done 을 통보한다.
 *
 * 실행 컨텍스트: bdev examine 프레임워크 (lvol 모듈 스레드).
 *
 * 호출 체인: bdev examine framework → [이 함수] → _vbdev_lvs_examine(vbdev_lvs_load) →
 *           ... → vbdev_lvs_examine_done → spdk_bdev_module_examine_done
 */
static void
vbdev_lvs_examine_disk(struct spdk_bdev *bdev)
{
	struct spdk_lvs_req *req;

	if (spdk_bdev_get_md_size(bdev) != 0) {
		/* [한국어] metadata(보호 정보) 포맷 bdev 는 blobstore 미지원 → 검사 건너뜀 */
		SPDK_INFOLOG(vbdev_lvol, "Cannot create bs dev on %s\n which is formatted with metadata",
			     bdev->name);
		spdk_bdev_module_examine_done(&g_lvol_if);
		/* [한국어] 검사 완료 통보 후 종료 (lvs 로드 시도 없음) */
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		/* [한국어] 요청 alloc 실패 — examine 만 완료 통보하고 종료 */
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		spdk_bdev_module_examine_done(&g_lvol_if);
		return;
	}

	req->cb_fn = vbdev_lvs_examine_done;
	/* [한국어] 최종 완료 시 호출될 콜백 — examine_done 통보 담당 */
	req->cb_arg = req;
	/* [한국어] 콜백이 자기 자신을 free 할 수 있도록 self-reference */

	_vbdev_lvs_examine(bdev, req, vbdev_lvs_load);
	/* [한국어] bs_dev 생성 + lvs 로드 시작 (action = vbdev_lvs_load) */
}

/*
 * [한국어]
 * vbdev_lvol_get_from_bdev - 주어진 bdev 가 lvol bdev 이면 그 뒤의 spdk_lvol 을 꺼낸다.
 *
 * @bdev: 검사 대상 bdev.
 * @return: lvol bdev 이면 대응 spdk_lvol, 아니면(다른 모듈 소유/ctxt 없음) NULL.
 *
 * 외부 코드가 임의의 bdev 핸들을 받았을 때, 그것이 lvol 모듈이 만든 bdev 인지 안전하게
 * 확인하고 내부 lvol 객체를 회수하는 게이트키퍼다. bdev->module 이 g_lvol_if 인지로
 * 소유권을 검증한 뒤, _create_lvol_disk 에서 bdev->ctxt 에 저장해 둔 lvol 포인터를 반환한다.
 *
 * 실행 컨텍스트: lvol 모듈 스레드 (RPC/도구 경로에서 주로 호출).
 *
 * 호출 체인: RPC/외부 도구 → [이 함수] → (bdev->ctxt 직접 접근)
 */
struct spdk_lvol *
vbdev_lvol_get_from_bdev(struct spdk_bdev *bdev)
{
	if (!bdev || bdev->module != &g_lvol_if) {
		/* [한국어] NULL 이거나 lvol 모듈 소유가 아니면 lvol 이 아님 */
		return NULL;
	}

	if (bdev->ctxt == NULL) {
		/* [한국어] lvol bdev 인데 ctxt 가 비어 있으면 내부 불변식 위반 — 방어적 NULL */
		SPDK_ERRLOG("No lvol ctx assigned to bdev %s\n", bdev->name);
		return NULL;
	}

	return (struct spdk_lvol *)bdev->ctxt;
	/* [한국어] _create_lvol_disk 가 bdev->ctxt 에 저장한 lvol 포인터 반환 */
}

/* Begin degraded blobstore device */

/*
 * When an external snapshot is missing, an instance of bs_dev_degraded is used as the blob's
 * back_bs_dev. No bdev is registered, so there should be no IO nor requests for channels. The main
 * purposes of this device are to prevent blobstore from hitting fatal runtime errors and to
 * indicate that the blob is degraded via the is_degraded() callback.
 */
/*
 * [한국어] === degraded(부재 esnap 대체) blobstore device ===
 * external snapshot 의 부모 bdev 가 부재하면, 그 clone blob 의 back_bs_dev 자리에 이 단일
 * 전역 더미 bs_dev(bs_dev_degraded)를 꽂는다. 정상이라면 부모 bs_dev 가 read CoW 의 원본
 * 데이터를 제공하지만, 부모가 없으니 실제 IO 는 불가능하다. 따라서 이 더미는 (1) blobstore
 * 가 NULL back_bs_dev 로 인해 치명적 런타임 에러를 내지 않도록 자리만 채우고, (2)
 * is_degraded()=true 로 "이 blob 은 degraded" 임을 blobstore/상위에 알린다. 정상 운영에서는
 * 이 device 의 IO/채널 콜백이 호출되어선 안 되므로 대부분 assert(false) + -EIO 로 막는다.
 * 부모 bdev 가 나중에 hotplug 되면 vbdev_lvs_hotplug 경로로 정상 bs_dev 로 교체된다.
 */

/*
 * [한국어]
 * bs_dev_degraded_read - degraded 더미의 단일 버퍼 read 콜백. 절대 호출되면 안 되는 경로.
 *
 * @dev/@channel/@payload/@lba/@lba_count: 표준 bs_dev read 시그니처 (모두 미사용).
 * @cb_args: 완료 보고용 콜백 묶음 (cb_fn/channel/cb_arg).
 *
 * degraded blob 은 bdev 가 등록되지 않아 IO 가 들어올 수 없어야 한다. 만약 호출되면 내부
 * 불변식 위반이므로 디버그 빌드에서 assert 로 즉시 중단하고, release 빌드에서는 -EIO 로
 * 완료 보고하여 데이터 손상 대신 명확한 에러를 낸다.
 *
 * 호출 체인: (정상적으로는 호출되지 않음) blobstore back_bs_dev read → [이 함수]
 */
static void
bs_dev_degraded_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		     uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	assert(false);
	/* [한국어] degraded device 로의 read 는 발생해선 안 됨 — 디버그에서 즉시 중단 */
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
	/* [한국어] release 빌드: 데이터 대신 -EIO 완료 보고 */
}

/*
 * [한국어]
 * bs_dev_degraded_readv - degraded 더미의 벡터 read 콜백. read 와 동일하게 금지 경로.
 *
 * @dev/@channel/@iov/@iovcnt/@lba/@lba_count: 표준 readv 시그니처 (미사용).
 * @cb_args: 완료 보고용 콜백 묶음.
 *
 * iovec 기반 산포 read 버전. degraded blob 에는 IO 가 없어야 하므로 assert + -EIO.
 *
 * 호출 체인: (정상 미발생) blobstore back_bs_dev readv → [이 함수]
 */
static void
bs_dev_degraded_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
		      struct spdk_bs_dev_cb_args *cb_args)
{
	assert(false);
	/* [한국어] degraded readv 금지 경로 */
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
	/* [한국어] -EIO 완료 보고 */
}

/*
 * [한국어]
 * bs_dev_degraded_readv_ext - degraded 더미의 확장 옵션 벡터 read 콜백. 금지 경로.
 *
 * @dev/@channel/@iov/@iovcnt/@lba/@lba_count: 표준 readv_ext 시그니처 (미사용).
 * @cb_args: 완료 보고용 콜백 묶음.
 * @io_opts: memory_domain 등 확장 옵션 (미사용).
 *
 * memory_domain 인지 read 버전. degraded blob 에는 IO 가 없어야 하므로 assert + -EIO.
 *
 * 호출 체인: (정상 미발생) blobstore back_bs_dev readv_ext → [이 함수]
 */
static void
bs_dev_degraded_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			  struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
			  struct spdk_bs_dev_cb_args *cb_args,
			  struct spdk_blob_ext_io_opts *io_opts)
{
	assert(false);
	/* [한국어] degraded readv_ext 금지 경로 */
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
	/* [한국어] -EIO 완료 보고 */
}

/*
 * [한국어]
 * bs_dev_degraded_is_zeroes - 주어진 LBA 범위가 0 으로 채워졌는지 질의하는 콜백 (금지 경로).
 *
 * @dev/@lba/@lba_count: 표준 시그니처 (미사용).
 * @return: false (호출 자체가 비정상이므로 의미 없는 안전값).
 *
 * blobstore 가 CoW 최적화를 위해 back_bs_dev 의 영역이 zero 인지 묻는 콜백. degraded 에는
 * 읽을 데이터가 없어 질의가 와선 안 되므로 assert + false.
 *
 * 호출 체인: (정상 미발생) blobstore → [이 함수]
 */
static bool
bs_dev_degraded_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	assert(false);
	/* [한국어] degraded 에는 zero 질의가 와선 안 됨 */
	return false;
	/* [한국어] 호출이 비정상이므로 보수적으로 "zero 아님" 반환 */
}

/*
 * [한국어]
 * bs_dev_degraded_is_range_valid - LBA 범위가 device 경계 안인지 검사하는 콜백 (금지 경로).
 *
 * @dev/@lba/@lba_count: 표준 시그니처 (미사용).
 * @return: false (비정상 호출에 대한 안전값).
 *
 * blobstore 가 IO 전 범위 유효성을 묻는 콜백. degraded 에는 IO 가 없어야 하므로 assert + false.
 *
 * 호출 체인: (정상 미발생) blobstore → [이 함수]
 */
static bool
bs_dev_degraded_is_range_valid(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	assert(false);
	/* [한국어] degraded 에는 범위 검증 질의가 와선 안 됨 */
	return false;
	/* [한국어] 보수적 invalid 반환 */
}

/*
 * [한국어]
 * bs_dev_degraded_create_channel - IO 채널 생성 콜백 (금지 경로).
 *
 * @bs_dev: 표준 시그니처 (미사용).
 * @return: NULL (채널 불필요).
 *
 * degraded 에는 IO 가 없어 채널도 필요 없다. 호출되면 불변식 위반 → assert + NULL.
 *
 * 호출 체인: (정상 미발생) blobstore channel 생성 → [이 함수]
 */
static struct spdk_io_channel *
bs_dev_degraded_create_channel(struct spdk_bs_dev *bs_dev)
{
	assert(false);
	/* [한국어] degraded 에는 IO 채널 생성 요청이 와선 안 됨 */
	return NULL;
}

/*
 * [한국어]
 * bs_dev_degraded_destroy_channel - IO 채널 파괴 콜백 (금지 경로).
 *
 * @bs_dev/@channel: 표준 시그니처 (미사용).
 *
 * create_channel 이 호출되지 않으므로 destroy_channel 도 호출되어선 안 됨 → assert.
 *
 * 호출 체인: (정상 미발생) blobstore channel 파괴 → [이 함수]
 */
static void
bs_dev_degraded_destroy_channel(struct spdk_bs_dev *bs_dev, struct spdk_io_channel *channel)
{
	assert(false);
	/* [한국어] 채널이 생성된 적 없으므로 파괴 요청도 비정상 */
}

/*
 * [한국어]
 * bs_dev_degraded_destroy - bs_dev 파괴 콜백. degraded 는 전역 정적이라 아무것도 하지 않는다.
 *
 * @bs_dev: 표준 시그니처 (미사용).
 *
 * bs_dev_degraded 는 동적 할당이 아닌 단일 전역 인스턴스이므로 free 할 대상이 없다. blobstore
 * 가 back_bs_dev 교체/해제 시 destroy 를 호출해도 안전하게 no-op 이어야 하기에 빈 함수로 둔다.
 * (read 계열과 달리 destroy 는 정상 경로에서 불릴 수 있으므로 assert 하지 않는다.)
 *
 * 호출 체인: blobstore back_bs_dev 해제 → [이 함수] (no-op)
 */
static void
bs_dev_degraded_destroy(struct spdk_bs_dev *bs_dev)
{
	/* [한국어] 전역 정적 인스턴스 — 해제할 자원 없음. no-op 가 정상 동작 */
}

/*
 * [한국어]
 * bs_dev_degraded_is_degraded - 이 back_bs_dev 가 degraded 상태인지 알리는 콜백.
 *
 * @bs_dev: 표준 시그니처 (미사용 — 항상 degraded 더미).
 * @return: 항상 true.
 *
 * 이 device 의 핵심 존재 이유 중 하나. blobstore/상위가 "이 blob 의 부모가 부재하다" 는
 * 사실을 이 콜백으로 인지하여, lvol 을 degraded 로 표시하고 IO 를 적절히 거절/대기시킨다.
 *
 * 호출 체인: blobstore/lvol degraded 질의 → [이 함수]
 */
static bool
bs_dev_degraded_is_degraded(struct spdk_bs_dev *bs_dev)
{
	return true;
	/* [한국어] 항상 degraded — 부모 esnap 부재를 명시 */
}

/* [한국어] bs_dev_degraded: 부재 esnap 대체용 단일 전역 bs_dev vtable 인스턴스.
 * 모든 IO 계열 콜백은 assert/-EIO 로 막혀 있고, is_degraded()=true, destroy()=no-op.
 * 여러 degraded clone 이 동시에 이 하나의 인스턴스를 back_bs_dev 로 공유한다 (IO 가 없으므로
 * 상태를 갖지 않아 공유 안전).
 * - blockcnt = UINT64_MAX/512: uint64 오버플로 위험 없이 가능한 한 큰 용량으로 보여, 어떤
 *   LBA 계산도 범위를 벗어나지 않게 한다 (실제 read 는 발생하지 않음).
 * - blocklen = 512: LBA 계산에서 0 으로 나누는 divide-by-zero 를 피하기 위한 최소 블록 길이.
 * 동기화: read-only 정적 데이터 + 무상태이므로 락 불필요. */
static struct spdk_bs_dev bs_dev_degraded = {
	.create_channel = bs_dev_degraded_create_channel,
	/* [한국어] IO 채널 생성 — degraded 에선 금지(assert+NULL) */
	.destroy_channel = bs_dev_degraded_destroy_channel,
	/* [한국어] IO 채널 파괴 — 금지(assert) */
	.destroy = bs_dev_degraded_destroy,
	/* [한국어] device 파괴 — 전역 정적이라 no-op */
	.read = bs_dev_degraded_read,
	/* [한국어] 단일 버퍼 read — 금지(assert+-EIO) */
	.readv = bs_dev_degraded_readv,
	/* [한국어] 벡터 read — 금지 */
	.readv_ext = bs_dev_degraded_readv_ext,
	/* [한국어] 확장 옵션 벡터 read — 금지 */
	.is_zeroes = bs_dev_degraded_is_zeroes,
	/* [한국어] zero 영역 질의 — 금지 */
	.is_range_valid = bs_dev_degraded_is_range_valid,
	/* [한국어] LBA 범위 유효성 질의 — 금지 */
	.is_degraded = bs_dev_degraded_is_degraded,
	/* [한국어] degraded 여부 — 항상 true (이 device 의 핵심 신호) */
	/* Make the device as large as possible without risk of uint64 overflow. */
	.blockcnt = UINT64_MAX / 512,
	/* [한국어] uint64 오버플로 없이 최대 용량 표현 — 어떤 LBA 도 범위 내로 보이게 */
	/* Prevent divide by zero errors calculating LBAs that will never be read. */
	.blocklen = 512,
	/* [한국어] divide-by-zero 방지용 최소 블록 길이 (실제 read 는 발생하지 않음) */
};

/* End degraded blobstore device */

/* Begin external snapshot support */

/*
 * [한국어]
 * vbdev_lvol_esnap_bdev_event_cb - esnap 부모 bdev 에 대한 이벤트 콜백 (현재 미지원 이벤트 로깅).
 *
 * @type: 발생한 bdev 이벤트 종류.
 * @bdev: 이벤트 대상 esnap 부모 bdev.
 * @event_ctx: 등록 시 NULL — 미사용.
 *
 * esnap 부모 bdev 는 read-only 원본으로만 쓰이며, REMOVE/RESIZE 등의 이벤트 처리는 아직
 * 구현되지 않았다. 따라서 어떤 이벤트가 와도 현재는 NOTICELOG 로 기록만 한다(미지원 통보).
 *
 * 실행 컨텍스트: bdev event dispatch (lvol 모듈 스레드).
 *
 * 호출 체인: spdk_bdev_create_bs_dev 가 등록 → bdev core event dispatch → [이 함수]
 */
static void
vbdev_lvol_esnap_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			       void *event_ctx)
{
	SPDK_NOTICELOG("bdev name (%s) received unsupported event type %d\n",
		       spdk_bdev_get_name(bdev), type);
	/* [한국어] esnap 부모 이벤트는 아직 미지원 — 로그만 남기고 무시 */
}

/*
 * [한국어]
 * vbdev_lvol_esnap_dev_create - external snapshot clone 의 부모 bs_dev 를 UUID 로 열어주는 핵심 콜백.
 *
 * @bs_ctx: 이 lvol 이 속한 spdk_lvol_store (degraded 등록 시 사용).
 * @blob_ctx: 이 esnap clone 에 대응하는 spdk_lvol.
 * @blob: 대응 spdk_blob (시그니처상 받지만 본문에선 직접 사용 안 함).
 * @esnap_id: 부모를 식별하는 esnap ID — lvol 에서는 부모 bdev 의 UUID 문자열.
 * @id_len: esnap_id 바이트 길이 (UUID 문자열 길이여야 함).
 * @_bs_dev: [out] 생성된 부모 bs_dev 를 돌려줄 위치 (실패 시 degraded 더미).
 * @return: 0 — 정상 bs_dev 또는 degraded 더미 중 하나가 반드시 반환됨(에러 입력만 음수).
 *
 * lvs 의 opts.esnap_bs_dev_create 로 등록되어, blobstore 가 esnap clone blob 을 열 때마다
 * "이 blob 의 부모를 열어 달라" 고 콜백한다. 본 모듈만이 SPDK bdev 트리에서 임의의 bdev 를
 * 부모로 열 수 있는 방법을 안다. 동작:
 *   (1) esnap_id 검증 — NULL/길이/UUID 파싱/재포맷 일치 확인.
 *   (2) UUID 문자열로 bdev 를 찾아 read-only bs_dev 생성(spdk_bdev_create_bs_dev) 후 claim.
 *   (3) 성공 → 그 bs_dev 를 부모로 반환.
 *   (4) 실패(부모 bdev 부재/이미 점유) → 전역 degraded 더미를 반환하고, 아직 degraded 가
 *       아니던 lvol 이면 spdk_lvs_esnap_missing_add 로 "부재 esnap" 등록 → 나중에 부모
 *       bdev 가 examine_config 에 나타나면 hotplug 로 자동 복구.
 *
 * 실행 컨텍스트: blobstore 의 blob open 경로 (lvol 모듈 스레드).
 *
 * 호출 체인: spdk_lvs_load_ext/create 시 등록 → blobstore blob open → [이 함수] →
 *           spdk_bdev_create_bs_dev / spdk_bs_bdev_claim / (실패)spdk_lvs_esnap_missing_add
 */
int
vbdev_lvol_esnap_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
			    const void *esnap_id, uint32_t id_len,
			    struct spdk_bs_dev **_bs_dev)
{
	struct spdk_lvol_store	*lvs = bs_ctx;
	/* [한국어] degraded 등록 시 필요한 소속 lvs */
	struct spdk_lvol	*lvol = blob_ctx;
	/* [한국어] 부모를 여는 대상 esnap clone lvol */
	struct spdk_bs_dev	*bs_dev = NULL;
	/* [한국어] 새로 만들 부모 bs_dev (성공 시), 실패 시 degraded 더미로 대체 */
	struct spdk_uuid	uuid;
	/* [한국어] esnap_id 문자열을 파싱한 바이너리 UUID */
	int			rc;
	char			uuid_str[SPDK_UUID_STRING_LEN] = { 0 };
	/* [한국어] bdev names 트리와 동일 포맷으로 재출력한 UUID 문자열 (lookup 키) */

	if (esnap_id == NULL) {
		/* [한국어] esnap ID 없음 — 부모를 식별할 수 없으므로 입력 에러 */
		SPDK_ERRLOG("lvol %s: NULL esnap ID\n", lvol->unique_id);
		return -EINVAL;
	}

	/* Guard against arbitrary names and unterminated UUID strings */
	if (id_len != SPDK_UUID_STRING_LEN) {
		/* [한국어] 길이가 UUID 문자열 길이와 다르면 임의 문자열/비종결 가능 — 거부 */
		SPDK_ERRLOG("lvol %s: Invalid esnap ID length (%u)\n", lvol->unique_id, id_len);
		return -EINVAL;
	}

	if (spdk_uuid_parse(&uuid, esnap_id)) {
		/* [한국어] UUID 로 파싱 불가 — lvol esnap 은 부모를 UUID 로만 식별하므로 거부 */
		SPDK_ERRLOG("lvol %s: Invalid esnap ID: not a UUID\n", lvol->unique_id);
		return -EINVAL;
	}

	/* Format the UUID the same as it is in the bdev names tree. */
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &uuid);
	/* [한국어] bdev lookup 은 표준 소문자 UUID 문자열로 하므로 재출력 */
	if (strcmp(uuid_str, esnap_id) != 0) {
		/* [한국어] 입력 문자열과 재포맷 결과가 다르면 비정규 입력 — 경고 + 디버그 중단.
		 * (정상 경로에선 동일해야 함; 데이터 무결성 방어) */
		SPDK_WARNLOG("lvol %s: esnap_id '%*s' does not match parsed uuid '%s'\n",
			     lvol->unique_id, SPDK_UUID_STRING_LEN, (const char *)esnap_id,
			     uuid_str);
		assert(false);
	}

	rc = spdk_bdev_create_bs_dev(uuid_str, false, NULL, 0,
				     vbdev_lvol_esnap_bdev_event_cb, NULL, &bs_dev);
	/* [한국어] UUID 로 부모 bdev 를 찾아 bs_dev 로 래핑. false = read-only(esnap 원본은 불변).
	 * 부모 bdev 가 아직 없으면 rc!=0 → degraded 경로로 */
	if (rc != 0) {
		goto fail;
	}

	rc = spdk_bs_bdev_claim(bs_dev, &g_lvol_if);
	/* [한국어] 부모 bdev 를 lvol 모듈 명의로 점유 — read-only 공유지만 claim 으로 일관 관리 */
	if (rc != 0) {
		/* [한국어] 다른 모듈이 점유 중 → bs_dev 파기 후 degraded 경로로 */
		SPDK_ERRLOG("lvol %s: unable to claim esnap bdev '%s': %d\n", lvol->unique_id,
			    uuid_str, rc);
		bs_dev->destroy(bs_dev);
		goto fail;
	}

	*_bs_dev = bs_dev;
	/* [한국어] 정상 부모 bs_dev 반환 — clone 이 CoW read 시 이 원본에서 데이터를 가져옴 */
	return 0;

fail:
	/* Unable to open or claim the bdev. This lvol is degraded. */
	/* [한국어] 부모를 열거나 점유하지 못함 → 이 lvol 은 degraded. 더미 bs_dev 로 대체 */
	bs_dev = &bs_dev_degraded;
	/* [한국어] 전역 degraded 더미를 부모 자리로 — blobstore 치명 에러 방지 + degraded 표시 */
	SPDK_NOTICELOG("lvol %s: bdev %s not available: lvol is degraded\n", lvol->unique_id,
		       uuid_str);

	/*
	 * Be sure not to call spdk_lvs_missing_add() on an lvol that is already degraded. This can
	 * lead to a cycle in the degraded_lvols tailq.
	 */
	/* [한국어] 이미 degraded 인 lvol 에 다시 missing_add 하면 degraded_lvols TAILQ 에 cycle
	 * 이 생길 수 있으므로, degraded_set 이 비어 있을 때만 등록한다. */
	if (lvol->degraded_set == NULL) {
		rc = spdk_lvs_esnap_missing_add(lvs, lvol, uuid_str, sizeof(uuid_str));
		/* [한국어] "이 UUID 부모를 기다리는 degraded lvol" 로 등록 → 나중에 부모 bdev 가
		 * examine_config 에 나타나면 vbdev_lvs_hotplug 로 자동 복구된다 */
		if (rc != 0) {
			/* [한국어] 등록 실패 시 — 부모가 나중에 와도 자동 hotplug 되지 않음(경고만) */
			SPDK_NOTICELOG("lvol %s: unable to register missing esnap device %s: "
				       "it will not be hotplugged if added later\n",
				       lvol->unique_id, uuid_str);
		}
	}

	*_bs_dev = bs_dev;
	/* [한국어] degraded 더미를 부모로 반환 — return 0 (열기 자체는 "성공", 단 degraded 상태) */
	return 0;
}

/* End external snapshot support */

/*
 * [한국어]
 * _vbdev_lvol_shallow_copy_base_bdev_event_cb - shallow copy 대상 bdev 의 이벤트 콜백 (no-op).
 *
 * @type/@bdev/@event_ctx: 표준 bdev 이벤트 시그니처 (모두 미사용).
 *
 * shallow copy 의 목적지 bdev 를 열 때 event_cb 인자가 필수이므로 빈 더미를 제공한다. 복사는
 * 짧은 동기적 작업 동안만 목적지를 점유하므로, 그 사이 들어오는 이벤트(REMOVE 등)는 별도
 * 처리하지 않는다(복사 완료 후 즉시 close 됨).
 *
 * 호출 체인: spdk_bdev_create_bs_dev_ext 가 등록 → bdev event dispatch → [이 함수] (no-op)
 */
static void
_vbdev_lvol_shallow_copy_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		void *event_ctx)
{
	/* [한국어] 복사 목적지 bdev 이벤트는 무시 — 복사 직후 close 되므로 처리 불필요 */
}

/*
 * [한국어]
 * _vbdev_lvol_shallow_copy_cb - shallow copy 완료 콜백. 목적지 bs_dev 정리 후 사용자 통보.
 *
 * @cb_arg: vbdev_lvol_shallow_copy 가 alloc 한 spdk_lvol_copy_req (lvol/ext_dev/사용자 콜백).
 * @lvolerrno: 복사 결과 (0=성공).
 *
 * spdk_lvol_shallow_copy 가 lvol 의 할당된 클러스터를 외부 bdev 로 복제한 뒤 호출된다. 성공/
 * 실패와 무관하게 목적지 ext_dev 를 destroy(=claim 해제 + bs_dev 파기)한 뒤, 사용자 콜백으로
 * 결과를 전달하고 요청 컨텍스트를 free 한다.
 *
 * 실행 컨텍스트: lib/lvol/blob 의 복사 완료 콜백 스레드 (= 본 모듈 스레드).
 *
 * 호출 체인: spdk_lvol_shallow_copy → [이 함수] → ext_dev->destroy / 사용자 cb_fn
 */
static void
_vbdev_lvol_shallow_copy_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_copy_req *req = cb_arg;
	/* [한국어] 복사 요청 컨텍스트 복원 */
	struct spdk_lvol *lvol = req->lvol;
	/* [한국어] 복사 원본 lvol (로그용) */

	if (lvolerrno != 0) {
		/* [한국어] 복사 실패 — 정리는 그대로 진행하고 errno 를 사용자에 전달 */
		SPDK_ERRLOG("Could not make a shallow copy of lvol %s due to error: %d\n",
			    lvol->name, lvolerrno);
	}

	req->ext_dev->destroy(req->ext_dev);
	/* [한국어] 목적지 bs_dev 파기 — claim 해제 및 베이스 bdev desc close */
	req->cb_fn(req->cb_arg, lvolerrno);
	/* [한국어] 사용자에게 복사 결과 비동기 보고 */
	free(req);
	/* [한국어] 요청 컨텍스트 해제 */
}

/*
 * [한국어]
 * vbdev_lvol_shallow_copy - lvol 의 할당된 클러스터들을 외부 bdev 로 얕은 복사(shallow copy).
 *
 * @lvol: 복사 원본 lvol (read-only snapshot 등). NULL 불가.
 * @bdev_name: 복사 목적지 bdev 이름. NULL 불가.
 * @status_cb_fn / @status_cb_arg: 진행률 통보 콜백 (blob 레이어가 클러스터별 진행 보고).
 * @cb_fn / @cb_arg: 최종 완료 콜백.
 * @return: 0=요청 발행 성공(완료는 콜백), 음수=즉시 실패(-EINVAL/-ENOMEM 또는 하위 에러).
 *
 * shallow copy 는 lvol 에서 *실제로 할당된* 클러스터만 외부 bdev 로 복제하는 백업/이관용
 * 연산이다(미할당/zero 영역은 건너뜀 → thin 효과 유지). 동작: (1) 입력 검증, (2) 목적지
 * bdev 를 bs_dev 로 열고 lvol 모듈 명의로 claim, (3) 요청 컨텍스트 구성, (4)
 * spdk_lvol_shallow_copy 비동기 호출. 완료/실패 시 _vbdev_lvol_shallow_copy_cb 가 정리.
 *
 * 실행 컨텍스트: RPC 핸들러 스레드 (lvol 모듈 스레드).
 *
 * 호출 체인: RPC rpc_bdev_lvol_shallow_copy → [이 함수] → spdk_bdev_create_bs_dev_ext /
 *           spdk_bs_bdev_claim / spdk_lvol_shallow_copy(→_vbdev_lvol_shallow_copy_cb)
 */
int
vbdev_lvol_shallow_copy(struct spdk_lvol *lvol, const char *bdev_name,
			spdk_blob_shallow_copy_status status_cb_fn, void *status_cb_arg,
			spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_dev *ext_dev;
	/* [한국어] 복사 목적지 bdev 의 blobstore-device 어댑터 */
	struct spdk_lvol_copy_req *req;
	/* [한국어] 비동기 완료까지 살아남을 복사 요청 컨텍스트 */
	int rc;

	if (lvol == NULL) {
		/* [한국어] 원본 미지정 — 진행 불가 */
		SPDK_ERRLOG("lvol must not be NULL\n");
		return -EINVAL;
	}

	if (bdev_name == NULL) {
		/* [한국어] 목적지 미지정 — 진행 불가 */
		SPDK_ERRLOG("lvol %s, bdev name must not be NULL\n", lvol->name);
		return -EINVAL;
	}

	assert(lvol->bdev != NULL);
	/* [한국어] 복사 원본 lvol 은 bdev 로 등록되어 있어야 함 (degraded 아님) */

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("lvol %s, cannot alloc memory for lvol copy request\n", lvol->name);
		return -ENOMEM;
	}

	rc = spdk_bdev_create_bs_dev_ext(bdev_name, _vbdev_lvol_shallow_copy_base_bdev_event_cb,
					 NULL, &ext_dev);
	/* [한국어] 목적지 bdev 를 bs_dev 로 래핑 (event_cb 는 no-op 더미). 복사 종료 후 destroy */
	if (rc < 0) {
		SPDK_ERRLOG("lvol %s, cannot create blobstore block device from bdev %s\n", lvol->name, bdev_name);
		free(req);
		return rc;
	}

	rc = spdk_bs_bdev_claim(ext_dev, &g_lvol_if);
	/* [한국어] 복사 중 목적지를 lvol 모듈 명의로 점유 — 동시 접근 차단 */
	if (rc != 0) {
		/* [한국어] 점유 실패 → bs_dev 파기 후 자원 회수 */
		SPDK_ERRLOG("lvol %s, unable to claim bdev %s, error %d\n", lvol->name, bdev_name, rc);
		ext_dev->destroy(ext_dev);
		free(req);
		return rc;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	/* [한국어] 사용자 최종 완료 콜백 보존 */
	req->lvol = lvol;
	/* [한국어] 완료 콜백 로그/식별용 원본 lvol */
	req->ext_dev = ext_dev;
	/* [한국어] 완료 시 destroy 할 목적지 bs_dev */

	rc = spdk_lvol_shallow_copy(lvol, ext_dev, status_cb_fn, status_cb_arg, _vbdev_lvol_shallow_copy_cb,
				    req);
	/* [한국어] lib/lvol 에 실제 복사 위임 — 할당 클러스터만 ext_dev 로 복제. 비동기 */

	if (rc < 0) {
		/* [한국어] 즉시 실패 시 alloc 자원 모두 회수 (콜백은 호출되지 않음) */
		ext_dev->destroy(ext_dev);
		free(req);
	}

	return rc;
}

/*
 * [한국어]
 * vbdev_lvol_set_external_parent - 기존 lvol 의 external snapshot(부모)를 다른 bdev 로 설정.
 *
 * @lvol: 부모를 붙일 대상 lvol.
 * @esnap_name: 새 부모로 쓸 외부 bdev 이름.
 * @cb_fn / @cb_arg: 비동기 완료 콜백.
 *
 * RPC bdev_lvol_set_parent_bdev 등의 진입점. 임의의 read-only bdev 를 lvol 의 esnap 부모로
 * 연결하여, lvol 이 그 bdev 의 데이터를 기반(CoW)으로 동작하게 한다. 동작: (1) esnap bdev 를
 * 열어(open_ext) UUID 추출, (2) (디스크 로드되지 않은 lvs 의 경우) esnap 콜백/플래그를 채워
 * vbdev_lvol_esnap_dev_create 가 불릴 수 있게 하고, (3) spdk_lvol_set_external_parent 로
 * 부모를 UUID 기준으로 설정한 뒤 (4) 즉시 desc 를 close(부모는 esnap_dev_create 가 UUID 로
 * 다시 연다).
 *
 * 실행 컨텍스트: RPC 핸들러 스레드 (lvol 모듈 스레드).
 *
 * 호출 체인: RPC rpc_bdev_lvol_set_parent_bdev → [이 함수] → spdk_bdev_open_ext /
 *           spdk_lvol_set_external_parent
 */
void
vbdev_lvol_set_external_parent(struct spdk_lvol *lvol, const char *esnap_name,
			       spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bdev_desc *desc;
	/* [한국어] esnap bdev 를 잠깐 열어 UUID 를 얻기 위한 임시 desc */
	struct spdk_bdev *bdev;
	/* [한국어] desc 가 가리키는 bdev */
	char bdev_uuid[SPDK_UUID_STRING_LEN];
	/* [한국어] 부모를 식별할 UUID 문자열 — set_external_parent 의 esnap_id */
	int rc;

	rc = spdk_bdev_open_ext(esnap_name, false, ignore_bdev_event_cb, NULL, &desc);
	/* [한국어] 이름으로 esnap bdev 를 열어 UUID 확인. false = write 불필요(read-only 부모).
	 * event_cb 는 no-op(ignore) — 곧 닫을 임시 open */
	if (rc != 0) {
		/* [한국어] 부모 bdev 가 없거나 열 수 없음 → -ENODEV 보고 */
		SPDK_ERRLOG("bdev '%s' could not be opened: error %d\n", esnap_name, rc);
		cb_fn(cb_arg, -ENODEV);
		return;
	}
	bdev = spdk_bdev_desc_get_bdev(desc);
	/* [한국어] desc 에서 bdev 핸들 추출 */

	rc = spdk_uuid_fmt_lower(bdev_uuid, sizeof(bdev_uuid), spdk_bdev_get_uuid(bdev));
	/* [한국어] bdev UUID 를 표준 문자열로 변환 — 이 UUID 로 부모를 영구 식별 */
	if (rc != 0) {
		/* [한국어] UUID 포맷 실패 — 정상 bdev 라면 발생 불가. desc close 후 에러 보고 */
		spdk_bdev_close(desc);
		SPDK_ERRLOG("bdev %s: unable to parse UUID\n", esnap_name);
		assert(false);
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	/*
	 * If lvol store is not loaded from disk, and so vbdev_lvs_load is not called, these
	 * assignments are necessary to let vbdev_lvol_esnap_dev_create be called.
	 */
	/* [한국어] lvs 가 (방금 생성되어) 디스크 로드 경로를 거치지 않았다면 vbdev_lvs_load 가
	 * 설정하는 esnap 콜백/플래그가 비어 있다. 부모를 esnap_dev_create 로 열 수 있도록 여기서
	 * 보강 설정한다. */
	lvol->lvol_store->load_esnaps = true;
	/* [한국어] 이 lvs 에서 esnap 부모 로딩을 활성화 */
	lvol->lvol_store->esnap_bs_dev_create = vbdev_lvol_esnap_dev_create;
	/* [한국어] 부모 bs_dev 를 동적으로 여는 콜백을 본 모듈 함수로 등록 */

	spdk_lvol_set_external_parent(lvol, bdev_uuid, sizeof(bdev_uuid), cb_fn, cb_arg);
	/* [한국어] lib/lvol 에 부모 설정 위임 — UUID 를 esnap_id 로 저장하고 blob 을 esnap clone 화.
	 * 실제 부모 열기는 추후 esnap_dev_create 콜백에서 UUID 로 다시 수행 */

	spdk_bdev_close(desc);
	/* [한국어] UUID 만 얻으면 되므로 임시 desc 는 즉시 close (부모 점유는 esnap_dev_create 가) */
}

/* [한국어] vbdev_lvol 로그 컴포넌트 등록 — SPDK_DEBUGLOG/INFOLOG(vbdev_lvol, ...) 채널을
 * constructor 시점에 등록하여 런타임에 `--logflag vbdev_lvol` 로 켤 수 있게 한다. */
SPDK_LOG_REGISTER_COMPONENT(vbdev_lvol)
