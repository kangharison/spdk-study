/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SCSI device(LUN의 컬렉션) 관리 (dev.c)
 *
 * === 파일의 역할 ===
 * SPDK SCSI target에서 "device"는 여러 LUN을 묶는 단위(SAM-3 4.4 SCSI device model)이다.
 * 본 파일은 g_devs[SPDK_SCSI_MAX_DEVS] 정적 풀에서 SCSI device를 할당/해제하고,
 * 각 device에 LUN/port를 추가/삭제하며, LUN 리스트 순회 헬퍼(get_lun, get_first/next_lun),
 * IO 채널 일괄 alloc/free, pending task 검사 등을 제공한다.
 * 주의: SCSI 명령 처리 자체는 본 파일이 아니라 lun.c가 한다. 여기서는 device 라이프사이클만 다룬다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 프론트엔드(iSCSI Target/vhost-scsi)는 "1개 target = 1 spdk_scsi_dev"로 매핑한다.
 *   spdk_scsi_dev_construct_ext() → allocate_dev() → spdk_scsi_dev_add_lun_ext() ×N
 *     → scsi_lun_construct() (lun.c)
 * destruct는:
 *   spdk_scsi_dev_destruct() → 각 LUN.scsi_lun_destruct() → 모두 빠지면 free_dev() → remove_cb()
 * SCSI 명령 도착 경로:
 *   프론트엔드 → spdk_scsi_dev_queue_task() → scsi_lun_execute_task() (lun.c)
 *
 * === 타 모듈과의 연결 ===
 * - 의존: scsi_internal.h (g_devs 풀 정의용 매크로/구조체), lun.c(생성/소멸 위임), port.c(scsi_port_construct/destruct).
 * - 사용처: 프론트엔드(iSCSI/vhost-scsi)가 본 파일의 spdk_scsi_dev_* 공개 API 호출.
 * - 데이터 흐름: g_devs[] (전역 풀) ← allocate/free; dev->luns ↔ scsi_lun (lun.c).
 * - 동기화: g_devs[] 조작은 단일 관리 스레드(보통 메인)에서만 — 별도 lock 없음.
 *
 * === 주요 함수/구조체 요약 ===
 * - g_devs[]                       : 정적 SCSI device 풀(SPDK_SCSI_MAX_DEVS 슬롯).
 * - scsi_dev_get_list              : g_devs 시작 포인터 반환(RPC 등이 사용).
 * - allocate_dev / free_dev        : g_devs 슬롯 점유/해제.
 * - spdk_scsi_dev_destruct         : LUN을 모두 빼면서 비동기 destruct 시작 → 마지막 LUN 제거 시 free_dev.
 * - spdk_scsi_dev_construct_ext    : device 생성 + LUN 일괄 추가(LUN 0 필수).
 * - spdk_scsi_dev_add_lun_ext      : 단일 LUN 추가(자동 ID 할당 또는 지정).
 * - spdk_scsi_dev_delete_lun       : LUN을 dev 리스트에서 제거 + 빈 dev면 free_dev.
 * - spdk_scsi_dev_queue_task / queue_mgmt_task : 도착한 task를 scsi_lun_execute_*로 위임.
 * - spdk_scsi_dev_add_port / delete_port / find_port_by_id: 내장 port 슬롯 관리.
 * - spdk_scsi_dev_allocate/free_io_channels : 모든 LUN의 IO 채널 일괄 alloc/free.
 * - get_lun / get_first/next_lun   : LUN 순회(removed 슬롯 자동 skip).
 * - has_pending_tasks              : 모든 LUN에 대해 pending task 존재 여부 검사.
 */

#include "scsi_internal.h"
/* [한국어] SCSI 내부 헤더 — spdk_scsi_dev/lun, scsi_lun_*, scsi_port_* 전부 필요. */

static struct spdk_scsi_dev g_devs[SPDK_SCSI_MAX_DEVS];
/* [한국어] SCSI device 정적 풀 — 동적 할당 대신 고정 슬롯 배열.
 *          allocate_dev가 빈 슬롯을 찾고, free_dev가 슬롯의 is_allocated=0으로 만든다.
 *          단일 관리 스레드에서만 다뤄지므로 lock 없음. */

/*
 * [한국어]
 * scsi_dev_get_list - g_devs 정적 풀의 시작 포인터 반환
 *
 * @return: g_devs[0]의 주소 (배열의 시작).
 *
 * scsi_rpc.c의 rpc_scsi_get_devices가 풀을 순회하기 위해 사용. 정적 배열이므로 lock 불필요.
 *
 * 호출 체인:
 *   rpc_scsi_get_devices → [scsi_dev_get_list]
 */
struct spdk_scsi_dev *
scsi_dev_get_list(void)
{
	return g_devs;
	/* [한국어] 배열 자체의 디케이된 포인터 반환. 호출자는 SPDK_SCSI_MAX_DEVS만큼 순회. */
}

/*
 * [한국어]
 * allocate_dev - g_devs[]에서 비어있는 슬롯을 찾아 device를 활성화
 *
 * @return: 새 device 포인터 또는 NULL(여유 슬롯 없음).
 *
 * 첫 빈 슬롯을 zero-fill하고 id=인덱스, is_allocated=1, luns 리스트 init까지 수행.
 * 실행 컨텍스트: dev 관리 스레드(보통 메인). 동시 접근 없음.
 *
 * 호출 체인:
 *   spdk_scsi_dev_construct_ext → [allocate_dev]
 */
static struct spdk_scsi_dev *
allocate_dev(void)
{
	struct spdk_scsi_dev *dev;
	/* [한국어] 후보 슬롯 포인터. */
	int i;
	/* [한국어] 풀 순회 인덱스 (= 슬롯 id). */

	for (i = 0; i < SPDK_SCSI_MAX_DEVS; i++) {
		dev = &g_devs[i];
		/* [한국어] 현재 슬롯 포인터. */
		if (!dev->is_allocated) {
			/* [한국어] 빈 슬롯 발견 — 초기화 진행. */
			memset(dev, 0, sizeof(*dev));
			/* [한국어] 이전 사용 잔여물 제거(이름/포트/플래그 모두 0). */
			dev->id = i;
			/* [한국어] 슬롯 인덱스를 dev id로 사용 — 외부에서도 ID로 식별 가능. */
			dev->is_allocated = 1;
			/* [한국어] 사용 중 마킹. */
			TAILQ_INIT(&dev->luns);
			/* [한국어] LUN 리스트 head 초기화 — 이후 add_lun이 INSERT. */
			return dev;
		}
	}

	return NULL;
	/* [한국어] 모든 슬롯이 차있다 — 호출자에게 NULL로 실패 통지. */
}

/*
 * [한국어]
 * free_dev - device 슬롯을 해제하고 remove_cb를 호출
 *
 * @dev: 해제 대상. removed=true이고 is_allocated=1이어야 함(라이프사이클 가드).
 *
 * destruct가 시작된(removed=true) device가 모든 LUN을 잃었을 때 호출된다.
 * is_allocated=0으로 슬롯을 비우고, 등록된 remove_cb가 있으면 한번 호출(rc=0).
 * 콜백은 호출 후 NULL로 클리어 — 슬롯 재사용 시 잔여 콜백 실행 사고 방지.
 *
 * 호출 체인:
 *   spdk_scsi_dev_destruct(LUN 없음 즉시) / spdk_scsi_dev_delete_lun(마지막 LUN) → [free_dev]
 */
static void
free_dev(struct spdk_scsi_dev *dev)
{
	assert(dev->is_allocated == 1);
	/* [한국어] 빈 슬롯을 또 free하면 라이프사이클 버그 — 디버그 빌드에서 차단. */
	assert(dev->removed == true);
	/* [한국어] removed=true는 destruct 진입 표시 — 그 외 경로에서 free_dev 금지. */

	dev->is_allocated = 0;
	/* [한국어] 슬롯을 다시 빈 슬롯으로 만든다(allocate_dev 재사용 가능). */

	if (dev->remove_cb) {
		/* [한국어] 호출자가 destruct 완료 통지를 원했으면 콜백 실행. */
		dev->remove_cb(dev->remove_ctx, 0);
		/* [한국어] 두 번째 인자 0 = 성공 의미 (errno 음수가 실패). */
		dev->remove_cb = NULL;
		/* [한국어] 향후 슬롯이 재할당되어도 옛 콜백이 다시 호출되지 않도록 클리어. */
	}
}

/*
 * [한국어]
 * spdk_scsi_dev_destruct - SCSI device 비동기 소멸 시작
 *
 * @dev:    소멸 대상 device.
 * @cb_fn:  소멸 완료 시 호출될 콜백(NULL 가능). 시그니처: cb_fn(cb_arg, rc).
 * @cb_arg: 콜백 user context.
 *
 * 동작:
 *   1) 인자 검증 — dev==NULL 또는 이미 removed=true이면 -EINVAL로 콜백 즉시 호출.
 *   2) dev->removed=true 표시 + cb 보관.
 *   3) LUN이 없으면 즉시 free_dev (콜백 동기 호출).
 *   4) LUN이 있으면 각 LUN scsi_lun_destruct 호출 — LUN이 outstanding IO 완료 후 자체적으로
 *      spdk_scsi_dev_delete_lun을 부르며, 마지막 LUN이 빠질 때 free_dev → cb 호출.
 *
 * 즉, LUN이 있을 경우 본 함수는 비동기로 동작한다(콜백은 나중에 LUN 정리 후).
 *
 * 호출 체인:
 *   상위(iSCSI/vhost target shutdown) → [spdk_scsi_dev_destruct]
 *     → scsi_lun_destruct(lun.c) → ... → spdk_scsi_dev_delete_lun → free_dev → cb_fn
 */
void
spdk_scsi_dev_destruct(struct spdk_scsi_dev *dev,
		       spdk_scsi_dev_destruct_cb_t cb_fn, void *cb_arg)
{
	struct spdk_scsi_lun *lun, *tmp_lun;
	/* [한국어] FOREACH_SAFE 순회 — LUN이 자기 자신을 리스트에서 제거할 가능성에 대비. */

	if (dev == NULL) {
		/* [한국어] NULL인자 — 콜백이 있으면 즉시 -EINVAL로 통지. */
		if (cb_fn) {
			cb_fn(cb_arg, -EINVAL);
		}
		return;
	}

	if (dev->removed) {
		/* [한국어] 이미 destruct가 시작된 상태 — 중복 호출은 -EINVAL. */
		if (cb_fn) {
			cb_fn(cb_arg, -EINVAL);
		}
		return;
	}

	dev->removed = true;
	/* [한국어] 이후 add_lun 등은 의미 없음 — dev는 종료 단계로 진입. */
	dev->remove_cb = cb_fn;
	/* [한국어] free_dev 시점에 호출될 완료 콜백 등록. */
	dev->remove_ctx = cb_arg;
	/* [한국어] 콜백 컨텍스트 저장. */

	if (TAILQ_EMPTY(&dev->luns)) {
		/* [한국어] LUN이 하나도 없으면 즉시 free_dev로 동기 종료(콜백 즉시 호출). */
		free_dev(dev);
		return;
	}

	TAILQ_FOREACH_SAFE(lun, &dev->luns, tailq, tmp_lun) {
		/*
		 * LUN will remove itself from this dev when all outstanding IO
		 * is done. When no more LUNs, dev will be deleted.
		 */
		/* [한국어] 각 LUN을 비동기로 소멸 시작 — LUN이 모든 outstanding IO를 끝낸 뒤
		 *          spdk_scsi_dev_delete_lun(아래 함수)을 호출하여 자기 자신을 dev에서 제거.
		 *          그때 dev->luns가 비고 dev->removed가 true면 free_dev가 동작. */
		scsi_lun_destruct(lun);
	}
}

/*
 * Search the lowest free LUN ID if the LUN ID is default, or check if the LUN ID is free otherwise,
 * and also return the LUN which comes just before where we want to insert an new LUN.
 */
/*
 * [한국어]
 * scsi_dev_find_free_lun - 새 LUN을 삽입할 위치를 찾고 그 직전 LUN 포인터 반환
 *
 * @dev:      대상 device.
 * @lun_id:   원하는 LUN id, -1이면 가장 낮은 빈 ID 자동 선택.
 * @prev_lun: [out] 새 LUN이 삽입될 위치의 직전 LUN(없으면 NULL → head 삽입).
 * @return:   0 성공, -EINVAL prev_lun NULL, -ENOSPC 빈 ID 없음, -EEXIST id 충돌.
 *
 * dev->luns 리스트는 LUN id 오름차순으로 정렬되어 유지된다. 본 함수는 이를 활용해 정렬 위치를 찾는다.
 *
 * 호출 체인:
 *   spdk_scsi_dev_add_lun_ext → [scsi_dev_find_free_lun] → 그 결과를 TAILQ_INSERT_HEAD/AFTER에 사용
 */
static int
scsi_dev_find_free_lun(struct spdk_scsi_dev *dev, int lun_id,
		       struct spdk_scsi_lun **prev_lun)
{
	struct spdk_scsi_lun *lun, *_prev_lun = NULL;
	/* [한국어] 순회 변수와 결과로 반환할 prev 포인터(초기값 NULL=head 삽입). */

	if (prev_lun == NULL) {
		/* [한국어] 출력 인자가 NULL이면 호출자 버그 — 즉시 거부. */
		return -EINVAL;
	}

	if (lun_id == -1) {
		/* [한국어] 자동 할당 모드 — 가장 낮은 빈 ID를 찾는다. */
		lun_id = 0;
		/* [한국어] 0부터 시도. */

		TAILQ_FOREACH(lun, &dev->luns, tailq) {
			/* [한국어] 정렬된 리스트를 순회하며 lun_id가 비어있는 곳을 찾는다. */
			if (lun->id > lun_id) {
				/* [한국어] 현재 lun id가 후보 lun_id를 건너뛰었음 — 그 사이가 비어있다. */
				break;
			}
			lun_id = lun->id + 1;
			/* [한국어] 현재 ID 다음을 후보로 잡고 계속 진행. */
			_prev_lun = lun;
			/* [한국어] 후보 ID 직전 LUN 갱신. */
		}

		if (lun_id >= SPDK_SCSI_DEV_MAX_LUN) {
			/* [한국어] 0..255 모두 차있어 빈 자리 없음. */
			return -ENOSPC;
		}
	} else {
		/* [한국어] 명시 ID 모드 — 충돌 검사 + 삽입 위치 찾기. */
		TAILQ_FOREACH(lun, &dev->luns, tailq) {
			if (lun->id == lun_id) {
				/* [한국어] 동일 id가 이미 있음 — 충돌. */
				return -EEXIST;
			} else if (lun->id > lun_id) {
				/* [한국어] 정렬 리스트에서 더 큰 id 발견 — 그 직전이 삽입 위치. */
				break;
			}
			_prev_lun = lun;
			/* [한국어] 아직 lun_id보다 작은 id가 보임 — prev 후보 갱신. */
		}
	}

	*prev_lun = _prev_lun;
	/* [한국어] 호출자에게 prev 결정 결과 반환. */
	return 0;
}

/*
 * [한국어]
 * spdk_scsi_dev_add_lun - dev에 LUN을 추가(레거시 wrapper, resize_cb 없음)
 *
 * @dev:           대상 device.
 * @bdev_name:     LUN으로 사용할 bdev의 이름.
 * @lun_id:        원하는 LUN id, -1이면 자동.
 * @hotremove_cb:  LUN hot-remove 통지 콜백.
 * @hotremove_ctx: 콜백 user context.
 * @return:        0 성공, 음수 실패.
 *
 * resize_cb 인자가 없는 구버전 인터페이스 — 내부적으로 _ext 버전을 NULL resize_cb로 호출.
 *
 * 호출 체인:
 *   상위(레거시 호출자) → [spdk_scsi_dev_add_lun] → spdk_scsi_dev_add_lun_ext
 */
int
spdk_scsi_dev_add_lun(struct spdk_scsi_dev *dev, const char *bdev_name, int lun_id,
		      void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		      void *hotremove_ctx)
{
	return spdk_scsi_dev_add_lun_ext(dev, bdev_name, lun_id,
					 NULL, NULL,
					 hotremove_cb, hotremove_ctx);
	/* [한국어] resize_cb/ctx를 NULL로 채워 _ext 버전 호출. */
}

/*
 * [한국어]
 * spdk_scsi_dev_add_lun_ext - dev에 LUN을 추가(확장 — resize_cb 지원)
 *
 * @dev:           대상 device.
 * @bdev_name:     LUN의 bdev 이름.
 * @lun_id:        원하는 LUN id 또는 -1(자동).
 * @resize_cb:     bdev RESIZE 이벤트 시 콜백(NULL 가능).
 * @resize_ctx:    resize_cb 컨텍스트.
 * @hotremove_cb:  hot-remove 콜백(NULL 가능).
 * @hotremove_ctx: hot-remove 컨텍스트.
 * @return:        0 성공, -1 또는 errno 음수 실패.
 *
 * 절차:
 *   1) lun_id 상한 검사 (>=256이면 거부).
 *   2) scsi_dev_find_free_lun으로 ID 검증 + 정렬 위치(prev_lun) 결정.
 *   3) scsi_lun_construct로 LUN 객체 생성(bdev open, 콜백 등록).
 *   4) lun->dev 연결 + 결정된 id 부여.
 *   5) prev_lun이 NULL이면 head, 아니면 그 뒤에 INSERT.
 *
 * 호출 체인:
 *   상위 → [spdk_scsi_dev_add_lun_ext] → scsi_dev_find_free_lun, scsi_lun_construct(lun.c)
 */
int
spdk_scsi_dev_add_lun_ext(struct spdk_scsi_dev *dev, const char *bdev_name, int lun_id,
			  void (*resize_cb)(const struct spdk_scsi_lun *, void *),
			  void *resize_ctx,
			  void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
			  void *hotremove_ctx)
{
	struct spdk_scsi_lun *lun, *prev_lun = NULL;
	/* [한국어] 새로 만든 LUN과 정렬 삽입의 직전 LUN. */
	int rc;
	/* [한국어] find_free_lun 반환 코드. */

	if (lun_id >= SPDK_SCSI_DEV_MAX_LUN) {
		/* [한국어] 256 이상 LUN id는 SPDK_SCSI_DEV_MAX_LUN 정의에 따라 거부. */
		SPDK_ERRLOG("LUN ID %d is more than the maximum.\n", lun_id);
		return -1;
	}

	rc = scsi_dev_find_free_lun(dev, lun_id, &prev_lun);
	/* [한국어] 충돌 검사 + 삽입 위치 결정. */
	if (rc != 0) {
		SPDK_ERRLOG("%s\n", rc == -EEXIST ? "LUN ID is duplicated" : "Free LUN ID is not found");
		/* [한국어] 두 가지 실패 상황을 메시지로 구분. */
		return rc;
	}

	lun = scsi_lun_construct(bdev_name, resize_cb, resize_ctx, hotremove_cb, hotremove_ctx);
	/* [한국어] lun.c에서 bdev open + LUN 구조체 생성. 실패 시 NULL. */
	if (lun == NULL) {
		return -1;
	}

	lun->dev = dev;
	/* [한국어] LUN ↔ dev 양방향 연결의 lun 측 설정(dev 측은 INSERT_*에서). */

	if (lun_id != -1) {
		lun->id = lun_id;
		/* [한국어] 명시 모드: 인자 그대로 사용. */
	} else if (prev_lun == NULL) {
		lun->id = 0;
		/* [한국어] 자동 모드 + 리스트 비어있음 → ID 0. */
	} else {
		lun->id = prev_lun->id + 1;
		/* [한국어] 자동 모드: prev id + 1을 사용 (find_free_lun이 보장한 빈 슬롯). */
	}

	if (prev_lun == NULL) {
		TAILQ_INSERT_HEAD(&dev->luns, lun, tailq);
		/* [한국어] 가장 작은 id로 head에 삽입. */
	} else {
		TAILQ_INSERT_AFTER(&dev->luns, prev_lun, lun, tailq);
		/* [한국어] 정렬 위치(prev 다음)에 삽입 — 리스트는 id 오름차순 유지. */
	}
	return 0;
}

/*
 * [한국어]
 * spdk_scsi_dev_delete_lun - LUN을 dev의 luns 리스트에서 제거하고, dev가 빈 채로 destruct 중이면 free_dev
 *
 * @dev: LUN의 부모 device.
 * @lun: 제거할 LUN(이미 hot-remove 완료된 상태).
 *
 * lun.c의 _scsi_lun_remove가 LUN의 자원 해제 직전에 호출한다. 본 함수는 단순히 dev에서
 * LUN을 떼어내고, dev가 destruct 진행 중이면서 마지막 LUN이었다면 free_dev로 dev 슬롯도 해제한다.
 *
 * 호출 체인:
 *   _scsi_lun_remove (lun.c) → [spdk_scsi_dev_delete_lun] → free_dev (조건부)
 */
void
spdk_scsi_dev_delete_lun(struct spdk_scsi_dev *dev,
			 struct spdk_scsi_lun *lun)
{
	TAILQ_REMOVE(&dev->luns, lun, tailq);
	/* [한국어] dev->luns에서 LUN 제거. LUN의 메모리 해제는 호출자(lun.c)가 담당. */

	if (dev->removed && TAILQ_EMPTY(&dev->luns)) {
		/* [한국어] dev가 destruct 진행 중이고 LUN이 모두 빠졌으면 dev 슬롯도 free. */
		free_dev(dev);
	}
}

/*
 * [한국어]
 * spdk_scsi_dev_construct - 레거시 wrapper(resize_cb 없는 dev 생성)
 *
 * @name:           device 이름.
 * @bdev_name_list: LUN별 bdev 이름 배열(num_luns 길이).
 * @lun_id_list:    LUN id 배열(같은 인덱스끼리 매칭).
 * @num_luns:       LUN 개수.
 * @protocol_id:    SCSI protocol identifier.
 * @hotremove_cb:   LUN hot-remove 콜백.
 * @hotremove_ctx:  콜백 context.
 * @return:         생성된 dev 포인터 또는 NULL.
 *
 * 호출 체인:
 *   레거시 호출자 → [spdk_scsi_dev_construct] → spdk_scsi_dev_construct_ext
 */
struct spdk_scsi_dev *spdk_scsi_dev_construct(const char *name, const char *bdev_name_list[],
		int *lun_id_list, int num_luns, uint8_t protocol_id,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx)
{
	return spdk_scsi_dev_construct_ext(name, bdev_name_list, lun_id_list,
					   num_luns, protocol_id,
					   NULL, NULL,
					   hotremove_cb, hotremove_ctx);
	/* [한국어] resize_cb/ctx를 NULL로 채워 _ext 버전 호출. */
}

/*
 * [한국어]
 * spdk_scsi_dev_construct_ext - SCSI device 생성 + 초기 LUN 일괄 추가(확장 버전)
 *
 * @name/...:        위 wrapper와 동일.
 * @resize_cb/ctx:   bdev RESIZE 통지 콜백(LUN 공통).
 * @return:          dev 포인터 또는 NULL(검증 실패/슬롯 부족/LUN 생성 실패).
 *
 * 검증:
 *   - 이름 길이 ≤ SPDK_SCSI_DEV_MAX_NAME.
 *   - num_luns > 0.
 *   - LUN id 리스트에 반드시 0이 포함(SPC 요구).
 *   - 모든 bdev_name이 non-NULL.
 * 절차: allocate_dev → 이름 복사 → protocol_id 설정 → LUN 일괄 추가.
 * LUN 추가 도중 실패 시 spdk_scsi_dev_destruct로 비동기 정리(콜백 없음).
 *
 * 호출 체인:
 *   상위 → [spdk_scsi_dev_construct_ext] → allocate_dev → spdk_scsi_dev_add_lun_ext × N
 */
struct spdk_scsi_dev *spdk_scsi_dev_construct_ext(const char *name, const char *bdev_name_list[],
		int *lun_id_list, int num_luns, uint8_t protocol_id,
		void (*resize_cb)(const struct spdk_scsi_lun *, void *),
		void *resize_ctx,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx)
{
	struct spdk_scsi_dev *dev;
	/* [한국어] 새로 할당될 device 슬롯 포인터. */
	size_t name_len;
	/* [한국어] device 이름 길이(널 제외). */
	bool found_lun_0;
	/* [한국어] LUN 0 존재 여부 — SPC가 LUN 0 필수. */
	int i, rc;
	/* [한국어] 순회 인덱스와 LUN 추가 결과 코드. */

	name_len = strlen(name);
	/* [한국어] 이름 길이 측정 — 버퍼 초과 검사용. */
	if (name_len > sizeof(dev->name) - 1) {
		/* [한국어] -1은 null terminator 자리 — 안전 복사를 위해 엄격 비교. */
		SPDK_ERRLOG("device %s: name longer than maximum allowed length %zu\n",
			    name, sizeof(dev->name) - 1);
		return NULL;
	}

	if (num_luns == 0) {
		/* [한국어] LUN 0개는 의미 없는 device — 즉시 거부. */
		SPDK_ERRLOG("device %s: no LUNs specified\n", name);
		return NULL;
	}

	found_lun_0 = false;
	/* [한국어] LUN 0 검색 초기 상태. */
	for (i = 0; i < num_luns; i++) {
		if (lun_id_list[i] == 0) {
			/* [한국어] LUN 0을 발견 — SPC 요구사항 만족. */
			found_lun_0 = true;
			break;
		}
	}

	if (!found_lun_0) {
		/* [한국어] LUN 0이 없으면 INQUIRY/REPORT LUNS 등 표준 동작이 깨짐. */
		SPDK_ERRLOG("device %s: no LUN 0 specified\n", name);
		return NULL;
	}

	for (i = 0; i < num_luns; i++) {
		if (bdev_name_list[i] == NULL) {
			/* [한국어] 모든 LUN은 bdev 이름이 필수 — NULL 거부. */
			SPDK_ERRLOG("NULL spdk_scsi_lun for LUN %d\n",
				    lun_id_list[i]);
			return NULL;
		}
	}

	dev = allocate_dev();
	/* [한국어] g_devs 풀에서 빈 슬롯 획득 — 슬롯 없으면 NULL. */
	if (dev == NULL) {
		return NULL;
	}

	memcpy(dev->name, name, name_len + 1);
	/* [한국어] 널 종료자까지 복사 (+1) — 길이 검증은 위에서 끝남. */

	dev->num_ports = 0;
	/* [한국어] 포트는 별도 add_port API로 추가 — 시작은 0. */
	dev->protocol_id = protocol_id;
	/* [한국어] INQUIRY 응답 등에서 사용될 SCSI protocol ID. */

	for (i = 0; i < num_luns; i++) {
		rc = spdk_scsi_dev_add_lun_ext(dev, bdev_name_list[i], lun_id_list[i],
					       resize_cb, resize_ctx,
					       hotremove_cb, hotremove_ctx);
		/* [한국어] 각 LUN을 dev에 추가 — bdev open이 일어나는 단계. */
		if (rc < 0) {
			/* [한국어] 도중 실패 → 이미 추가된 LUN까지 포함해 비동기 정리.
			 *          콜백 NULL — 호출자에게 즉시 NULL 반환으로 실패 통지. */
			spdk_scsi_dev_destruct(dev, NULL, NULL);
			return NULL;
		}
	}

	return dev;
	/* [한국어] 모든 LUN 추가 성공 — 호출자에게 dev 핸들 반환. */
}

/*
 * [한국어]
 * spdk_scsi_dev_queue_mgmt_task - 도착한 mgmt task(reset 등)를 LUN 실행기에 위임
 *
 * @dev:  대상 device(현재 인자만 받고 직접 사용은 안 함 — task->lun이 진짜 대상).
 * @task: SCSI mgmt task. task->lun이 가리키는 LUN에서 처리됨.
 *
 * 호출 체인:
 *   iSCSI/vhost-scsi (mgmt task 도착) → [spdk_scsi_dev_queue_mgmt_task]
 *     → scsi_lun_execute_mgmt_task (lun.c)
 */
void
spdk_scsi_dev_queue_mgmt_task(struct spdk_scsi_dev *dev,
			      struct spdk_scsi_task *task)
{
	assert(task != NULL);
	/* [한국어] NULL task는 호출자 버그. */

	scsi_lun_execute_mgmt_task(task->lun, task);
	/* [한국어] LUN 단위 mgmt 큐에 enqueue 후 실행 시도. */
}

/*
 * [한국어]
 * spdk_scsi_dev_queue_task - 도착한 IO task를 LUN 실행기에 위임
 *
 * @dev:  대상 device(인자만 받음 — task->lun이 실제 대상).
 * @task: SCSI IO task.
 *
 * 호출 체인:
 *   iSCSI/vhost-scsi (IO task 도착) → [spdk_scsi_dev_queue_task]
 *     → scsi_lun_execute_task (lun.c)
 */
void
spdk_scsi_dev_queue_task(struct spdk_scsi_dev *dev,
			 struct spdk_scsi_task *task)
{
	assert(task != NULL);
	/* [한국어] NULL task는 호출자 버그. */

	scsi_lun_execute_task(task->lun, task);
	/* [한국어] LUN 단위 IO 큐에 enqueue 후 실행 시도(또는 즉시 실행). */
}

/*
 * [한국어]
 * scsi_dev_find_free_port - dev->port[] 내장 배열에서 빈 슬롯 검색
 *
 * @dev: 대상 device.
 * @return: 빈 port 슬롯 포인터 또는 NULL(모두 사용 중).
 *
 * 내장 port 배열은 SPDK_SCSI_DEV_MAX_PORTS 크기로 고정 — 동적 할당 없음.
 *
 * 호출 체인:
 *   spdk_scsi_dev_add_port → [scsi_dev_find_free_port]
 */
static struct spdk_scsi_port *
scsi_dev_find_free_port(struct spdk_scsi_dev *dev)
{
	int i;
	/* [한국어] 슬롯 인덱스. */

	for (i = 0; i < SPDK_SCSI_DEV_MAX_PORTS; i++) {
		if (!dev->port[i].is_used) {
			/* [한국어] is_used=0이면 빈 슬롯. */
			return &dev->port[i];
		}
	}

	return NULL;
	/* [한국어] 모든 슬롯이 사용 중. */
}

int
spdk_scsi_dev_add_port(struct spdk_scsi_dev *dev, uint64_t id, const char *name)
{
	struct spdk_scsi_port *port;
	int rc;

	if (dev->num_ports == SPDK_SCSI_DEV_MAX_PORTS) {
		SPDK_ERRLOG("device already has %d ports\n", SPDK_SCSI_DEV_MAX_PORTS);
		return -1;
	}

	port = spdk_scsi_dev_find_port_by_id(dev, id);
	if (port != NULL) {
		SPDK_ERRLOG("device already has port(%" PRIu64 ")\n", id);
		return -1;
	}

	port = scsi_dev_find_free_port(dev);
	if (port == NULL) {
		assert(false);
		return -1;
	}

	rc = scsi_port_construct(port, id, dev->num_ports, name);
	if (rc != 0) {
		return rc;
	}

	dev->num_ports++;
	return 0;
}

int
spdk_scsi_dev_delete_port(struct spdk_scsi_dev *dev, uint64_t id)
{
	struct spdk_scsi_port *port;

	port = spdk_scsi_dev_find_port_by_id(dev, id);
	if (port == NULL) {
		SPDK_ERRLOG("device does not have specified port(%" PRIu64 ")\n", id);
		return -1;
	}

	scsi_port_destruct(port);

	dev->num_ports--;

	return 0;
}

struct spdk_scsi_port *
spdk_scsi_dev_find_port_by_id(struct spdk_scsi_dev *dev, uint64_t id)
{
	int i;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_PORTS; i++) {
		if (!dev->port[i].is_used) {
			continue;
		}
		if (dev->port[i].id == id) {
			return &dev->port[i];
		}
	}

	/* No matching port found. */
	return NULL;
}

void
spdk_scsi_dev_free_io_channels(struct spdk_scsi_dev *dev)
{
	struct spdk_scsi_lun *lun, *tmp_lun;

	TAILQ_FOREACH_SAFE(lun, &dev->luns, tailq, tmp_lun) {
		scsi_lun_free_io_channel(lun);
	}
}

int
spdk_scsi_dev_allocate_io_channels(struct spdk_scsi_dev *dev)
{
	struct spdk_scsi_lun *lun, *tmp_lun;
	int rc;

	TAILQ_FOREACH_SAFE(lun, &dev->luns, tailq, tmp_lun) {
		rc = scsi_lun_allocate_io_channel(lun);
		if (rc < 0) {
			spdk_scsi_dev_free_io_channels(dev);
			return -1;
		}
	}

	return 0;
}

const char *
spdk_scsi_dev_get_name(const struct spdk_scsi_dev *dev)
{
	return dev->name;
}

int
spdk_scsi_dev_get_id(const struct spdk_scsi_dev *dev)
{
	return dev->id;
}

struct spdk_scsi_lun *
spdk_scsi_dev_get_lun(struct spdk_scsi_dev *dev, int lun_id)
{
	struct spdk_scsi_lun *lun;

	TAILQ_FOREACH(lun, &dev->luns, tailq) {
		if (lun->id == lun_id) {
			if (!spdk_scsi_lun_is_removing(lun)) {
				return lun;
			} else {
				return NULL;
			}
		}
	}

	return NULL;
}

struct spdk_scsi_lun *
spdk_scsi_dev_get_first_lun(struct spdk_scsi_dev *dev)
{
	struct spdk_scsi_lun *lun;

	TAILQ_FOREACH(lun, &dev->luns, tailq) {
		if (!spdk_scsi_lun_is_removing(lun)) {
			return lun;
		}
	}

	return NULL;
}

struct spdk_scsi_lun *
spdk_scsi_dev_get_next_lun(struct spdk_scsi_lun *prev_lun)
{
	struct spdk_scsi_dev *dev;
	struct spdk_scsi_lun *lun;

	if (prev_lun == NULL) {
		return NULL;
	}

	dev = prev_lun->dev;

	lun = TAILQ_NEXT(prev_lun, tailq);
	if (lun == NULL) {
		return NULL;
	}

	TAILQ_FOREACH_FROM(lun, &dev->luns, tailq) {
		if (!spdk_scsi_lun_is_removing(lun)) {
			break;
		}
	}

	return lun;
}

bool
spdk_scsi_dev_has_pending_tasks(const struct spdk_scsi_dev *dev,
				const struct spdk_scsi_port *initiator_port)
{
	struct spdk_scsi_lun *lun;

	TAILQ_FOREACH(lun, &dev->luns, tailq) {
		if (scsi_lun_has_pending_tasks(lun, initiator_port) ||
		    scsi_lun_has_pending_mgmt_tasks(lun, initiator_port)) {
			return true;
		}
	}

	return false;
}
