/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK vhost 타깃 프레임워크 코어 (vhost.c)
 *
 * === 파일의 역할 ===
 * SPDK vhost 타깃의 "공통 코어" 레이어로, vhost-SCSI / vhost-BLK 두 백엔드를 통합 관리한다.
 * 구체적으로 (1) 전역 vhost 컨트롤러 레지스트리(레드-블랙 트리 기반 이름 검색),
 * (2) vhost 컨트롤러 등록/해제 API(vhost_dev_register/unregister), (3) 공용 락
 * (g_vhost_mutex)을 통한 컨트롤러 트리 보호, (4) coalescing(IRQ 병합) 파라미터 추상화,
 * (5) virtio-blk transport(예: vhost_user_blk) 등록/조회/생성/삭제,
 * (6) JSON-RPC config 덤프(spdk_vhost_*_config_json) 등을 제공한다.
 * 백엔드별 세부 동작(SCSI 큐 처리, BLK 요청 처리)은 backend ops 함수 포인터로 위임한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * vhost 타깃 전체 흐름:
 *   QEMU(vhost_user 클라이언트) ──UNIX socket(vhost-user 프로토콜)── SPDK vhost 타깃
 *     SPDK 측에서 receive: rte_vhost_user(DPDK lib) → vhost_user.c의 콜백 →
 *     vhost-scsi.c / vhost-blk.c 백엔드 → 본 파일 vhost.c가 컨트롤러 등록/조회 제공 →
 *     실제 I/O는 백엔드 콜백이 spdk_bdev_*로 bdev 레이어에 위임(zero-copy: shared
 *     memory 영역에 매핑된 virtio descriptor를 그대로 읽고 씀).
 * 호출 체인 (등록):
 *   RPC handler / rpc_vhost_create_*/* → vhost_dev_register() →
 *     [SCSI] vhost_user_dev_create() / [BLK] virtio_blk_construct_ctrlr()
 *     → vhost_user 등록 → QEMU connect 시 vhost-user 프로토콜 messages.
 * 호출 체인 (정리):
 *   spdk_vhost_*_fini() → vhost_user_fini(vhost_fini) →
 *     vhost_fini가 모든 vdev에 대해 spdk_vhost_dev_remove → backend->remove_device →
 *     마지막 vdev unregister 후 g_fini_cb() 호출.
 * 실행 컨텍스트:
 *   대부분의 함수는 SPDK init 스레드(메인 reactor)에서 호출되며, g_vhost_mutex로
 *   다른 thread/RPC 진입과 동기화한다. 백엔드 콜백 자체는 컨트롤러가 바인딩된
 *   reactor에서 polling 모드로 실행 — vhost-user 프로토콜 메시지는 별도 socket 폴링
 *   스레드에서 처리되어 spdk_thread_send_msg로 컨트롤러 스레드에 전달된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/env.h(DPDK 추상화), spdk/cpuset.h(CPU 마스크), vhost_internal.h
 *   (spdk_vhost_dev/backend/user_dev_backend 정의), spdk/queue.h(TAILQ/RB).
 * - 의존자: vhost_user.c(SCSI 백엔드), vhost_blk.c(BLK 백엔드), vhost_rpc.c
 *   (RPC handlers가 vhost_dev_register/find/get_*를 호출).
 * - 데이터 흐름: QEMU virtio queue ↔ vhost-user 프로토콜 ↔ spdk_vhost_dev
 *   (vdev) ↔ spdk_bdev_io. 본 파일은 vdev 인스턴스의 생명주기와 디스커버리만 관리.
 * - 공유 상태: g_vhost_devices(이름 트리), g_vhost_mutex(락), g_vhost_core_mask
 *   (가용 코어), g_virtio_blk_transports(BLK transport 인스턴스 리스트),
 *   g_spdk_virtio_blk_transport_ops(BLK transport ops 등록 리스트), g_fini_cb(전역 종료 콜백).
 *
 * === 주요 함수/구조체 요약 ===
 * - vhost_dev_register(): 새 vdev를 g_vhost_devices RB-tree에 등록하고 backend별 생성 함수 호출.
 * - vhost_dev_unregister(): vdev를 트리에서 제거 후 마지막이면 g_fini_cb 호출(종료 흐름).
 * - spdk_vhost_dev_find()/spdk_vhost_dev_next(): 이름 키 검색 / 트리 순회.
 * - spdk_vhost_lock/unlock/trylock(): g_vhost_mutex 추상화 (RPC/관리 경로 동기화).
 * - spdk_vhost_scsi_init/fini, spdk_vhost_blk_init/fini: 서브시스템 초기화/종료 진입점.
 * - virtio_blk_transport_register/create/get_ops(): BLK transport ops 등록 + 인스턴스 생성.
 * - vhost_user_config_json / spdk_vhost_*_config_json: JSON-RPC save_config 출력.
 *
 * 핵심 자료구조:
 * - struct spdk_vhost_dev (vhost_internal.h): 한 vhost 컨트롤러(=QEMU 디바이스)의 상태.
 *   필드 backend(타입 SCSI/BLK 분기), thread(전용 SPDK thread), name(키), node(RB 노드).
 * - struct spdk_virtio_blk_transport: BLK transport 인스턴스. ops가 transport 동작 정의.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 C 라이브러리(stdint, stdio, string 등) 일괄 포함 (SPDK 공통 진입). */

#include "spdk/env.h"
/* [한국어] DPDK 추상 환경 — SPDK_ENV_FOREACH_CORE 매크로(가용 코어 순회)에 필요. */
#include "spdk/likely.h"
/* [한국어] spdk_likely/unlikely (분기 예측 hint) — 성능 경로에서 사용. */
#include "spdk/string.h"
/* [한국어] strcasecmp 등 SPDK가 제공하는 문자열 헬퍼 호환성 래퍼. */
#include "spdk/util.h"
/* [한국어] SPDK 범용 매크로(SPDK_COUNTOF 등). */
#include "spdk/memory.h"
/* [한국어] SPDK 메모리 관련 유틸. (이 파일 자체는 직접 참조는 적지만 transport ops가 의존). */
#include "spdk/barrier.h"
/* [한국어] 메모리 배리어 — vhost는 여러 코어에서 vring을 공유하므로 backend가 의존. */
#include "spdk/vhost.h"
/* [한국어] 공개 vhost API (spdk_vhost_dev_*, init/fini cb 타입 등) — 여기서 정의된 함수 시그니처 일치. */
#include "vhost_internal.h"
/* [한국어] 내부 헤더 — spdk_vhost_dev, backend struct, vhost_user_* 함수 등 비공개 정의. */
#include "spdk/queue.h"
/* [한국어] TAILQ/STAILQ/RB 매크로 모음 — g_vhost_devices RB tree와 transport TAILQ에 사용. */

static struct spdk_cpuset g_vhost_core_mask;
/* [한국어] 전역 — vhost가 사용할 수 있는 CPU 코어 비트마스크.
 * 설정자: spdk_vhost_scsi_init / spdk_vhost_blk_init에서 SPDK_ENV_FOREACH_CORE로 채움.
 * 읽는 자: vhost_parse_core_mask가 사용자 cpumask 검증/AND 연산에 사용.
 * 동기화: 초기화 1회 후 read-only로 사용되어 락 불필요. */

static pthread_mutex_t g_vhost_mutex = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] 전역 — 모든 컨트롤러 트리/리스트 변경을 보호하는 큰 락(big lock).
 * 설정자/읽는 자: spdk_vhost_lock/trylock/unlock으로 RPC, 등록, 종료 경로에서 획득.
 * 동기화: 자명 — pthread mutex. RPC 핸들러 ↔ vhost_user 콜백 등 cross-thread race 차단.
 * 정적 초기화로 별도 init 호출 불필요. */

static TAILQ_HEAD(, spdk_virtio_blk_transport) g_virtio_blk_transports = TAILQ_HEAD_INITIALIZER(
			g_virtio_blk_transports);
/* [한국어] 전역 — 생성된 virtio_blk transport 인스턴스 리스트(예: vhost_user_blk transport 1개).
 * 설정자: virtio_blk_transport_create 시 INSERT_TAIL.
 * 읽는 자: spdk_vhost_blk_config_json / virtio_blk_transports_destroy / get_first/next.
 * 동기화: g_vhost_mutex 보호 하에 사용 가정. */

static spdk_vhost_fini_cb g_fini_cb;
/* [한국어] 전역 — vhost 서브시스템 종료 콜백. 마지막 vdev/transport 정리 후 호출.
 * 설정자: spdk_vhost_scsi_fini / spdk_vhost_blk_fini 진입 시 1회.
 * 읽는 자: vhost_fini, vhost_dev_unregister, virtio_blk_transports_destroy.
 * 동기화: 종료는 단일 thread(init thread)에서만 트리거되므로 race 없음. */

static RB_HEAD(vhost_dev_name_tree,
	       spdk_vhost_dev) g_vhost_devices = RB_INITIALIZER(g_vhost_devices);
/* [한국어] 전역 — 모든 등록된 vdev를 이름 키로 정렬 보관하는 레드-블랙 트리.
 * 트리 매크로 RB_HEAD/RB_INITIALIZER는 spdk/queue.h(BSD queue.h 변형)에서 제공.
 * 설정자: vhost_dev_register/unregister가 RB_INSERT/REMOVE.
 * 읽는 자: spdk_vhost_dev_find(이름 검색), spdk_vhost_dev_next(순회).
 * 동기화: g_vhost_mutex 하에 변경. */

/*
 * [한국어]
 * vhost_dev_name_cmp - RB 트리 노드 비교자 (이름 사전순).
 *
 * @vdev1: 비교 대상 1
 * @vdev2: 비교 대상 2
 * @return: strcmp 결과 (음/0/양)
 *
 * RB_GENERATE_STATIC이 트리 연산(insert/find/next)에서 호출하는 콜백.
 * 두 vdev의 name 문자열을 strcmp로 비교한다. name은 등록 시 strdup으로 복제되어
 * 트리에 살아있는 동안 불변이므로 비교 안전.
 *
 * 호출 체인: RB_INSERT/RB_FIND/RB_NEXT (queue.h 매크로) → vhost_dev_name_cmp.
 */
static int
vhost_dev_name_cmp(struct spdk_vhost_dev *vdev1, struct spdk_vhost_dev *vdev2)
{
	return strcmp(vdev1->name, vdev2->name);
	/* [한국어] 사전순 비교 — RB tree는 이 결과로 균형 유지. */
}

RB_GENERATE_STATIC(vhost_dev_name_tree, spdk_vhost_dev, node, vhost_dev_name_cmp);
/* [한국어] queue.h 매크로 — vhost_dev_name_tree 타입을 위한 RB 연산 함수들
 * (RB_INSERT_VHOST_DEV_NAME_TREE 등)을 static 가시성으로 자동 생성.
 * spdk_vhost_dev의 'node' 필드를 RB_ENTRY로 사용하며 비교는 vhost_dev_name_cmp. */

/*
 * [한국어]
 * spdk_vhost_dev_next - RB 트리 in-order 순회 (이름 사전순).
 *
 * @vdev: NULL이면 트리 최소(첫 항목), 아니면 그 다음 항목.
 * @return: 다음 vdev 또는 더 없으면 NULL.
 *
 * 컨트롤러 목록 열람 (config_json 출력, 진단 RPC 등)에서 사용한다.
 * 호출자는 g_vhost_mutex를 보유한 상태로 호출해야 한다.
 *
 * 호출 체인: spdk_vhost_*_config_json → spdk_vhost_dev_next → RB_MIN/RB_NEXT.
 * 실행 컨텍스트: g_vhost_mutex 하의 SPDK init 스레드 또는 RPC 핸들러.
 */
struct spdk_vhost_dev *
spdk_vhost_dev_next(struct spdk_vhost_dev *vdev)
{
	if (vdev == NULL) {
		/* [한국어] 시작 호출 — 트리의 최소(이름 사전 첫) 노드 반환. */
		return RB_MIN(vhost_dev_name_tree, &g_vhost_devices);
	}

	return RB_NEXT(vhost_dev_name_tree, &g_vhost_devices, vdev);
	/* [한국어] 현재 vdev의 다음 in-order 노드. 끝이면 NULL. */
}

/*
 * [한국어]
 * spdk_vhost_dev_find - 이름으로 vdev 조회.
 *
 * @ctrlr_name: 찾으려는 컨트롤러 이름 (RPC에서 사용자가 지정한 문자열).
 * @return: 매칭되는 vdev 포인터 또는 NULL.
 *
 * 임시 스택 변수 'find'에 이름만 채운 후 RB_FIND로 비교자 호출 → name 일치 노드를 찾는다.
 * 호출자는 g_vhost_mutex를 보유한 상태여야 한다.
 *
 * 호출 체인: vhost_rpc.c::rpc_vhost_*/vhost_dev_register → spdk_vhost_dev_find.
 */
struct spdk_vhost_dev *
spdk_vhost_dev_find(const char *ctrlr_name)
{
	struct spdk_vhost_dev find = {};
	/* [한국어] 검색 키만 담는 임시 객체 — RB_FIND가 비교자에 넘길 의사 객체. */

	find.name = (char *)ctrlr_name;
	/* [한국어] const 캐스팅 — RB_FIND가 키만 읽으므로 안전. */

	return RB_FIND(vhost_dev_name_tree, &g_vhost_devices, &find);
	/* [한국어] 트리에서 동일 name 노드 검색. 없으면 NULL. */
}

/*
 * [한국어]
 * vhost_parse_core_mask - 사용자가 지정한 cpumask 문자열을 파싱·검증.
 *
 * @mask: 사용자 지정 cpumask 문자열(예: "0x3"). NULL이면 기본 g_vhost_core_mask 복사.
 * @cpumask: 결과를 채울 spdk_cpuset 출력 버퍼.
 * @return: 0=성공, -1=실패 (포맷 오류 또는 가용 코어 밖 / 0개 코어).
 *
 * 동기/배경: vhost 컨트롤러는 특정 코어 집합에 polling thread를 배치한다.
 * 사용자가 cpumask를 지정하면 그것이 g_vhost_core_mask(가용 코어)의 부분집합임을 검증한다.
 * 절차:
 *  1) cpumask=NULL 검증.
 *  2) mask=NULL이면 가용 코어 전체로 설정 후 종료.
 *  3) spdk_cpuset_parse로 hex 문자열 → bitset 변환.
 *  4) 사용자 마스크 ∩ NOT(가용 마스크) ≠ ∅ 면 — 잘못된 코어 포함 → 에러.
 *  5) 사용자 마스크 ∩= 가용 마스크 (혹시 모를 잉여 정리), 빈 결과면 에러.
 *
 * 호출 체인: vhost_dev_register → vhost_parse_core_mask.
 * 실행 컨텍스트: SPDK init 스레드(또는 RPC 호출 컨텍스트), g_vhost_mutex 외부에서도 안전.
 */
static int
vhost_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask)
{
	int rc;
	/* [한국어] 임시 반환 코드 보관. */
	struct spdk_cpuset negative_vhost_mask;
	/* [한국어] g_vhost_core_mask의 보집합(NOT) 저장 — "잘못된 코어" 검출용. */

	if (cpumask == NULL) {
		/* [한국어] 출력 버퍼 미지정 — 호출 오용 차단. */
		return -1;
	}

	if (mask == NULL) {
		/* [한국어] 사용자가 마스크를 지정하지 않음 → 기본값(가용 코어 전체)으로 채움. */
		spdk_cpuset_copy(cpumask, &g_vhost_core_mask);
		return 0;
	}

	rc = spdk_cpuset_parse(cpumask, mask);
	/* [한국어] hex/리스트 문자열을 cpuset 비트맵으로 파싱. */
	if (rc < 0) {
		/* [한국어] 포맷 오류. */
		SPDK_ERRLOG("invalid cpumask %s\n", mask);
		return -1;
	}

	spdk_cpuset_copy(&negative_vhost_mask, &g_vhost_core_mask);
	/* [한국어] 가용 마스크를 복사한 뒤. */
	spdk_cpuset_negate(&negative_vhost_mask);
	/* [한국어] 보집합으로 변환 — "사용 불가 코어" 비트맵. */
	spdk_cpuset_and(&negative_vhost_mask, cpumask);
	/* [한국어] (사용자 마스크 ∩ 사용 불가 코어) — 0이 아니면 잘못된 코어 포함. */

	if (spdk_cpuset_count(&negative_vhost_mask) != 0) {
		/* [한국어] 사용자 mask가 사용 불가 코어 비트를 켰다 → 거부. */
		SPDK_ERRLOG("one of selected cpu is outside of core mask(=%s)\n",
			    spdk_cpuset_fmt(&g_vhost_core_mask));
		return -1;
	}

	spdk_cpuset_and(cpumask, &g_vhost_core_mask);
	/* [한국어] 안전을 위해 사용자 mask를 가용 mask와 한 번 더 AND. */

	if (spdk_cpuset_count(cpumask) == 0) {
		/* [한국어] 결과 마스크가 비었으면(가용과 교집합 0) 사용 불가 — 거부. */
		SPDK_ERRLOG("no cpu is selected among core mask(=%s)\n",
			    spdk_cpuset_fmt(&g_vhost_core_mask));
		return -1;
	}

	return 0;
	/* [한국어] 성공 — cpumask에 사용 가능한 비트만 남았다. */
}

TAILQ_HEAD(, virtio_blk_transport_ops_list_element)
g_spdk_virtio_blk_transport_ops = TAILQ_HEAD_INITIALIZER(g_spdk_virtio_blk_transport_ops);
/* [한국어] 전역 — 등록된 virtio_blk transport ops 리스트 (예: vhost_user_blk ops).
 * SPDK_VIRTIO_BLK_TRANSPORT_REGISTER 매크로(컴파일 타임 constructor)로
 * virtio_blk_transport_register가 호출되어 채워진다.
 * 설정자: virtio_blk_transport_register.
 * 읽는 자: virtio_blk_get_transport_ops가 이름으로 검색.
 * 동기화: 등록은 init 시점, 검색은 RPC 시점 — 일반적으로 read-only로 다뤄짐. */

/*
 * [한국어]
 * virtio_blk_get_transport_ops - 이름으로 등록된 transport ops 검색.
 *
 * @transport_name: 찾을 transport 이름 (예: "vhost_user_blk", 대소문자 무시).
 * @return: ops 포인터 또는 NULL.
 *
 * 호출 체인: virtio_blk_transport_create → virtio_blk_get_transport_ops.
 */
const struct spdk_virtio_blk_transport_ops *
virtio_blk_get_transport_ops(const char *transport_name)
{
	struct virtio_blk_transport_ops_list_element *ops;
	/* [한국어] 리스트 노드 임시 포인터. */
	TAILQ_FOREACH(ops, &g_spdk_virtio_blk_transport_ops, link) {
		/* [한국어] 등록된 모든 transport ops 순회. */
		if (strcasecmp(transport_name, ops->ops.name) == 0) {
			/* [한국어] 이름이 case-insensitive로 일치하면 포인터 반환. */
			return &ops->ops;
		}
	}
	return NULL;
	/* [한국어] 못 찾음. */
}

/*
 * [한국어]
 * vhost_dev_register - 새 vhost 컨트롤러(vdev)를 등록.
 *
 * @vdev: 호출자가 사전에 zero-init 또는 부분 초기화한 vdev 객체.
 *        backend에 따라 spdk_vhost_scsi_dev / spdk_vhost_blk_dev 같은 부모 구조체의 멤버일 수 있음.
 * @name: 컨트롤러 이름 (전역 유일 키). RPC에서 사용자가 지정.
 * @mask_str: cpumask 문자열 (NULL이면 가용 코어 전체).
 * @params: 백엔드별 추가 파라미터 (BLK transport opts 등). NULL 가능.
 * @backend: 공통 백엔드 ops (type=SCSI/BLK, dump_info_json/remove_device 등).
 * @user_backend: vhost-user 사용자 콜백 ops.
 * @delay: SCSI 백엔드에서만 의미 — true면 vhost-user 등록을 지연 (LUN 부착 후 일괄).
 * @return: 0=성공, -EINVAL/-EEXIST/-EIO/-기타.
 *
 * 동기/배경: SCSI/BLK 두 백엔드의 공통 등록 절차를 묶는다.
 *  1) cpumask 파싱·검증.
 *  2) g_vhost_mutex 획득 → 이름 충돌 검사.
 *  3) name 복제, backend 저장.
 *  4) 백엔드별 생성 함수 호출 (vhost_user_dev_create / virtio_blk_construct_ctrlr).
 *  5) 성공 시 RB 트리 삽입 후 락 해제.
 *
 * 호출 체인: rpc_vhost_create_*/vhost_user.c → vhost_dev_register →
 *   vhost_user_dev_create / virtio_blk_construct_ctrlr.
 * 실행 컨텍스트: SPDK init 스레드 / RPC 핸들러. 내부에서 g_vhost_mutex 직접 잠금.
 * 에러 경로: 실패 시 strdup된 vdev->name 해제, 트리 미삽입 상태로 반환.
 */
int
vhost_dev_register(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
		   const struct spdk_json_val *params, const struct spdk_vhost_dev_backend *backend,
		   const struct spdk_vhost_user_dev_backend *user_backend, bool delay)
{
	struct spdk_cpuset cpumask = {};
	/* [한국어] 검증 후 vdev 스레드에 적용할 cpu 마스크. */
	int rc;
	/* [한국어] 임시 반환 코드. */

	assert(vdev);
	/* [한국어] vdev 미지정은 호출 오용 — 디버그 빌드에서 즉시 abort. */
	if (name == NULL) {
		/* [한국어] 이름 없으면 트리에 키가 없어 등록 불가. */
		SPDK_ERRLOG("Can't register controller with no name\n");
		return -EINVAL;
	}

	if (vhost_parse_core_mask(mask_str, &cpumask) != 0) {
		/* [한국어] cpumask 검증 실패 — 사용자 입력 오류. */
		SPDK_ERRLOG("cpumask %s is invalid (core mask is 0x%s)\n",
			    mask_str, spdk_cpuset_fmt(&g_vhost_core_mask));
		return -EINVAL;
	}
	vdev->use_default_cpumask = false;
	/* [한국어] 명시적으로 default 미사용 설정 (기본). */
	if (!mask_str) {
		/* [한국어] 사용자가 mask_str 미지정 → "기본 마스크 사용" 표식 — 환경 변경 시 재계산용. */
		vdev->use_default_cpumask = true;
	}

	spdk_vhost_lock();
	/* [한국어] 트리/리스트 변경을 위해 큰 락 획득. */
	if (spdk_vhost_dev_find(name)) {
		/* [한국어] 동일 이름이 이미 존재 → -EEXIST. */
		SPDK_ERRLOG("vhost controller %s already exists.\n", name);
		spdk_vhost_unlock();
		return -EEXIST;
	}

	vdev->name = strdup(name);
	/* [한국어] 이름은 트리 키로 사용되므로 호출자 수명과 무관하게 자체 복제. */
	if (vdev->name == NULL) {
		/* [한국어] OOM. */
		spdk_vhost_unlock();
		return -EIO;
	}

	vdev->backend = backend;
	/* [한국어] 백엔드 ops 저장 — type 분기와 추후 dump/remove에서 사용. */
	if (vdev->backend->type == VHOST_BACKEND_SCSI) {
		/* [한국어] SCSI 경로 — vhost-user 등록을 user_backend로 진행. */
		rc = vhost_user_dev_create(vdev, name, &cpumask, user_backend, delay);
	} else {
		/* When VHOST_BACKEND_BLK, delay should not be true. */
		assert(delay == false);
		/* [한국어] BLK 경로는 LUN 개념이 없어 delay 의미가 없음. 디버그 검사. */
		rc = virtio_blk_construct_ctrlr(vdev, name, &cpumask, params, user_backend);
		/* [한국어] BLK transport(예: vhost_user_blk)를 통해 컨트롤러 생성. */
	}
	if (rc != 0) {
		/* [한국어] 백엔드 생성 실패 — 자원 정리 후 락 해제. */
		free(vdev->name);
		spdk_vhost_unlock();
		return rc;
	}

	RB_INSERT(vhost_dev_name_tree, &g_vhost_devices, vdev);
	/* [한국어] 모든 자원 준비 완료 — 트리에 노출 (이후 find/next에서 보임). */
	spdk_vhost_unlock();

	SPDK_INFOLOG(vhost, "Controller %s: new controller added\n", vdev->name);
	/* [한국어] 등록 성공 진단 로그. */
	return 0;
}

/*
 * [한국어]
 * vhost_dev_unregister - 등록된 vdev를 해제.
 *
 * @vdev: 해제 대상 (반드시 트리에 등록된 상태).
 * @return: 0=성공, 기타=백엔드 정리 실패 코드.
 *
 * 절차:
 *  1) g_vhost_mutex 획득.
 *  2) backend type에 맞는 정리 함수 호출 (vhost_user_dev_unregister / virtio_blk_destroy_ctrlr).
 *  3) name 해제, RB 트리에서 제거.
 *  4) 트리가 비고 g_fini_cb가 설정되어 있으면(=종료 시퀀스 중) 콜백 호출.
 *
 * 호출 체인:
 *   spdk_vhost_dev_remove → backend->remove_device → ... → vhost_dev_unregister
 *   (백엔드는 자신이 관리하는 자원 정리 후 본 함수를 마지막에 호출).
 * 실행 컨텍스트: g_fini_cb 실행 시점은 마지막 vdev unregister가 g_vhost_mutex 보유 상태에서 호출됨에 유의.
 */
int
vhost_dev_unregister(struct spdk_vhost_dev *vdev)
{
	int rc;
	/* [한국어] 백엔드 unregister 결과 코드. */

	spdk_vhost_lock();
	/* [한국어] 트리 변경 보호. */
	if (vdev->backend->type == VHOST_BACKEND_SCSI) {
		/* [한국어] SCSI 경로 — vhost-user dev unregister. */
		rc = vhost_user_dev_unregister(vdev);
	} else {
		/* [한국어] BLK 경로 — virtio_blk 컨트롤러 destroy. */
		rc = virtio_blk_destroy_ctrlr(vdev);
	}
	if (rc != 0) {
		/* [한국어] 정리 실패 — 트리에 남긴 채 반환 (재시도 가능). */
		spdk_vhost_unlock();
		return rc;
	}

	SPDK_INFOLOG(vhost, "Controller %s: removed\n", vdev->name);

	free(vdev->name);
	/* [한국어] strdup된 이름 해제. */

	RB_REMOVE(vhost_dev_name_tree, &g_vhost_devices, vdev);
	/* [한국어] 트리에서 제거 — 이후 find/next에서 보이지 않음. */
	if (RB_EMPTY(&g_vhost_devices) && g_fini_cb != NULL) {
		/* [한국어] 마지막 vdev였고 종료 시퀀스 중이면 g_fini_cb 호출 → fini 완료 신호. */
		g_fini_cb();
	}
	spdk_vhost_unlock();

	return 0;
}

/*
 * [한국어]
 * spdk_vhost_dev_get_name - 컨트롤러 이름 getter.
 *
 * @vdev: 컨트롤러
 * @return: 내부 strdup 문자열 (수정 금지, vdev 수명 동안 유효).
 *
 * 호출 체인: 진단/JSON 출력 함수들에서 사용.
 */
const char *
spdk_vhost_dev_get_name(struct spdk_vhost_dev *vdev)
{
	assert(vdev != NULL);
	/* [한국어] NULL 방어. */
	return vdev->name;
}

/*
 * [한국어]
 * spdk_vhost_dev_get_cpumask - 컨트롤러 스레드의 cpumask getter.
 *
 * @vdev: 컨트롤러
 * @return: vdev->thread의 cpumask 포인터.
 *
 * 백엔드 생성 시 vdev->thread = spdk_thread_create(name, &cpumask) 형태로 만들어진
 * SPDK thread의 cpu 마스크를 반환한다. RPC 진단에서 사용.
 */
const struct spdk_cpuset *
spdk_vhost_dev_get_cpumask(struct spdk_vhost_dev *vdev)
{
	assert(vdev != NULL);
	return spdk_thread_get_cpumask(vdev->thread);
	/* [한국어] thread API에 위임 — vdev->thread는 백엔드가 생성·소유. */
}

/*
 * [한국어]
 * vhost_dump_info_json - vdev의 백엔드별 진단 JSON 출력.
 *
 * @vdev: 컨트롤러
 * @w: SPDK JSON write context (RPC 응답 작성)
 *
 * 백엔드 ops의 dump_info_json 콜백을 호출 — SCSI는 LUN 목록, BLK는 bdev 이름 등 출력.
 *
 * 호출 체인: rpc_get_vhost_controllers / RPC 응답 빌더 → vhost_dump_info_json → backend->dump_info_json.
 */
void
vhost_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	assert(vdev->backend->dump_info_json != NULL);
	/* [한국어] 백엔드는 반드시 dump 콜백을 제공해야 함. */
	vdev->backend->dump_info_json(vdev, w);
}

/*
 * [한국어]
 * spdk_vhost_dev_remove - vdev 제거 진입점 (공개 API).
 *
 * @vdev: 제거 대상
 * @return: 백엔드 remove_device 결과 코드 (0=성공, 또는 비동기 진행 중을 의미).
 *
 * 백엔드 remove_device는 비동기일 수 있다 — 진행 중 I/O 완료를 기다린 뒤 vhost_dev_unregister를 호출해 마무리.
 *
 * 호출 체인: rpc_vhost_delete_controller / vhost_fini → spdk_vhost_dev_remove → backend->remove_device.
 */
int
spdk_vhost_dev_remove(struct spdk_vhost_dev *vdev)
{
	return vdev->backend->remove_device(vdev);
	/* [한국어] 백엔드별 절차에 위임 (비동기 가능). */
}

/*
 * [한국어]
 * spdk_vhost_set_coalescing - virtio IRQ 병합 파라미터 설정.
 *
 * @vdev: 컨트롤러
 * @delay_base_us: IRQ 합병 기본 지연 (마이크로초).
 * @iops_threshold: IOPS 임계값 (이하면 즉시 알림).
 * @return: 백엔드별 설정 결과.
 *
 * 백엔드(SCSI/BLK)가 자체 coalescing 파라미터를 갖고 있어 ops로 위임한다.
 * RPC vhost_controller_set_coalescing에서 호출.
 */
int
spdk_vhost_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			  uint32_t iops_threshold)
{
	assert(vdev->backend->set_coalescing != NULL);
	/* [한국어] 백엔드가 set_coalescing을 지원해야 함. */
	return vdev->backend->set_coalescing(vdev, delay_base_us, iops_threshold);
}

/*
 * [한국어]
 * spdk_vhost_get_coalescing - 현재 coalescing 파라미터 조회.
 *
 * @vdev: 컨트롤러
 * @delay_base_us: [out] 현재 지연
 * @iops_threshold: [out] 현재 임계값
 *
 * config_json 출력 등에서 호출되어 vhost_controller_set_coalescing RPC 재현용 값 조회.
 */
void
spdk_vhost_get_coalescing(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			  uint32_t *iops_threshold)
{
	assert(vdev->backend->get_coalescing != NULL);
	vdev->backend->get_coalescing(vdev, delay_base_us, iops_threshold);
	/* [한국어] 백엔드가 자체 멤버에서 값을 채워줌. */
}

/*
 * [한국어]
 * spdk_vhost_lock - 큰 락(g_vhost_mutex) 획득.
 *
 * RPC 핸들러나 외부 호출자가 트리/리스트 변경 전 호출. 단순 mutex_lock 래퍼.
 * 실행 컨텍스트: 어느 스레드에서나 호출 가능 (vhost-user 콜백 등).
 */
void
spdk_vhost_lock(void)
{
	pthread_mutex_lock(&g_vhost_mutex);
	/* [한국어] POSIX mutex — 비재귀. 같은 스레드에서 중복 호출 시 데드락 주의. */
}

/*
 * [한국어]
 * spdk_vhost_trylock - 큰 락 비차단 시도.
 *
 * @return: 0=획득 성공, 음수(=-EBUSY 등)=실패.
 *
 * 폴러처럼 락이 잡혀 있으면 다음 tick에 재시도하고 싶은 코드 경로용.
 */
int
spdk_vhost_trylock(void)
{
	return -pthread_mutex_trylock(&g_vhost_mutex);
	/* [한국어] pthread는 EBUSY 같은 양수 errno 반환 — SPDK 관행상 음수로 변환 후 반환. */
}

/*
 * [한국어]
 * spdk_vhost_unlock - 큰 락 해제.
 */
void
spdk_vhost_unlock(void)
{
	pthread_mutex_unlock(&g_vhost_mutex);
}

/*
 * [한국어]
 * spdk_vhost_scsi_init - vhost-SCSI 서브시스템 초기화.
 *
 * @init_cb: 결과 콜백 (rc 인자, 비동기 모델 호환). 항상 동기적으로 호출됨.
 *
 * 절차:
 *  1) vhost_user_init() — DPDK rte_vhost 초기화 (UNIX socket listen 인프라).
 *  2) g_vhost_core_mask를 모든 가용 코어로 설정.
 *  3) init_cb(rc) 호출.
 *
 * 호출 체인: subsystem_*/spdk_app_start → spdk_vhost_scsi_init.
 * 실행 컨텍스트: SPDK init 스레드. 다른 vhost API 사용 전에 반드시 호출.
 */
void
spdk_vhost_scsi_init(spdk_vhost_init_cb init_cb)
{
	uint32_t i;
	/* [한국어] 코어 인덱스 순회용. */
	int ret = 0;
	/* [한국어] 초기화 결과. */

	ret = vhost_user_init();
	/* [한국어] vhost-user 인프라(DPDK rte_vhost) 초기화 — 실패 시 즉시 콜백. */
	if (ret != 0) {
		init_cb(ret);
		return;
	}

	spdk_cpuset_zero(&g_vhost_core_mask);
	/* [한국어] 가용 코어 마스크를 비운 후. */
	SPDK_ENV_FOREACH_CORE(i) {
		/* [한국어] DPDK가 점유한 모든 lcore에 대해. */
		spdk_cpuset_set_cpu(&g_vhost_core_mask, i, true);
		/* [한국어] 비트 set — 이후 cpumask 검증의 모집단으로 쓰임. */
	}
	init_cb(ret);
	/* [한국어] 동기 완료 알림. */
}

/*
 * [한국어]
 * vhost_fini - 모든 vdev에 remove 요청 → 마지막에 g_fini_cb 호출.
 *
 * vhost_user_fini의 백엔드 정리 콜백으로 등록되어 호출된다.
 * 트리가 비어있으면 즉시 g_fini_cb 호출, 아니면 모든 vdev에 remove 요청 후
 * 비동기 완료 시 vhost_dev_unregister가 트리 빈 시점에 g_fini_cb 호출.
 *
 * 호출 체인: spdk_vhost_scsi_fini → vhost_user_fini(..., vhost_fini) → vhost_fini → backend remove.
 * 실행 컨텍스트: vhost_user_fini가 부르는 스레드(보통 init 스레드).
 */
static void
vhost_fini(void)
{
	struct spdk_vhost_dev *vdev, *tmp;
	/* [한국어] 순회 중 remove로 next가 무효화될 수 있어 미리 tmp 보관. */

	if (spdk_vhost_dev_next(NULL) == NULL) {
		/* [한국어] 등록된 vdev 없음 — 즉시 종료 콜백. */
		g_fini_cb();
		return;
	}

	vdev = spdk_vhost_dev_next(NULL);
	/* [한국어] 첫 vdev. */
	while (vdev != NULL) {
		/* [한국어] 모든 vdev 순회. */
		tmp = spdk_vhost_dev_next(vdev);
		/* [한국어] remove로 vdev가 제거되기 전 다음을 확보. */
		spdk_vhost_dev_remove(vdev);
		/* don't care if it fails, there's nothing we can do for now */
		/* [한국어] 실패해도 종료 흐름이라 별도 처리 없음. */
		vdev = tmp;
	}

	/* g_fini_cb will get called when last device is unregistered. */
	/* [한국어] remove는 비동기일 수 있어 마지막 vdev unregister 시점에 vhost_dev_unregister가 g_fini_cb를 호출. */
}

/*
 * [한국어]
 * spdk_vhost_blk_init - vhost-BLK 서브시스템 초기화.
 *
 * @init_cb: 완료 콜백.
 *
 * 기본 transport "vhost_user_blk"를 생성하고, g_vhost_core_mask를 채운다.
 * SCSI와 달리 vhost_user_init은 SCSI 측에서 한 번만 부르면 충분하다고 가정 (또는
 * vhost_user_blk transport 내부에서 자체 초기화).
 */
void
spdk_vhost_blk_init(spdk_vhost_init_cb init_cb)
{
	uint32_t i;
	int ret = 0;

	ret = virtio_blk_transport_create("vhost_user_blk", NULL);
	/* [한국어] 항상 존재해야 하는 기본 BLK transport 생성 (params=NULL → 기본값). */
	if (ret != 0) {
		/* [한국어] transport 생성 실패 — 콜백으로 에러 전달. */
		goto out;
	}

	spdk_cpuset_zero(&g_vhost_core_mask);
	/* [한국어] 가용 코어 마스크 초기화. */
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_set_cpu(&g_vhost_core_mask, i, true);
		/* [한국어] DPDK가 차지한 모든 lcore 비트 켬. */
	}
out:
	init_cb(ret);
	/* [한국어] 단일 종료점 — 성공/실패 모두 콜백. */
}

/*
 * [한국어]
 * spdk_vhost_scsi_fini - vhost-SCSI 서브시스템 종료.
 *
 * @fini_cb: 종료 완료 콜백.
 *
 * g_fini_cb에 보관 후 vhost_user_fini를 호출하면, 그 콜백 체인에서 vhost_fini가
 * 모든 vdev remove를 요청하고, 마지막 vdev unregister가 g_fini_cb를 호출한다.
 */
void
spdk_vhost_scsi_fini(spdk_vhost_fini_cb fini_cb)
{
	g_fini_cb = fini_cb;
	/* [한국어] 종료 완료 시점에 호출할 콜백 보관 (전역 1개 — SCSI/BLK 동시 fini 가정 안 함). */

	vhost_user_fini(vhost_fini);
	/* [한국어] vhost-user 인프라 정리 시작. 정리 후 vhost_fini 호출되어 vdev remove 진행. */
}

/*
 * [한국어]
 * virtio_blk_transports_destroy - 등록된 BLK transport들을 순차 파괴.
 *
 * 재귀적 콜백 패턴: 첫 transport를 꺼내 destroy(콜백=자기 자신) → 완료되면 다음 transport...
 * 마지막에 트리가 비면 g_fini_cb 호출.
 *
 * 호출 체인: spdk_vhost_blk_fini → virtio_blk_transports_destroy → ops->destroy → 콜백 → 재진입.
 */
static void
virtio_blk_transports_destroy(void)
{
	struct spdk_virtio_blk_transport *transport = TAILQ_FIRST(&g_virtio_blk_transports);
	/* [한국어] 다음 파괴 대상 — 첫 항목. */

	if (transport == NULL) {
		/* [한국어] 더 이상 transport가 없으면 종료 콜백. */
		g_fini_cb();
		return;
	}
	TAILQ_REMOVE(&g_virtio_blk_transports, transport, tailq);
	/* [한국어] 리스트에서 제거 후 비동기 destroy. */
	virtio_blk_transport_destroy(transport, virtio_blk_transports_destroy);
	/* [한국어] 완료 콜백을 자기 자신으로 — 다음 transport 처리 재귀. */
}

/*
 * [한국어]
 * spdk_vhost_blk_fini - vhost-BLK 서브시스템 종료 진입점.
 *
 * @fini_cb: 종료 완료 콜백.
 *
 * 모든 BLK transport를 비동기로 파괴하고 마지막에 콜백 호출.
 */
void
spdk_vhost_blk_fini(spdk_vhost_fini_cb fini_cb)
{
	g_fini_cb = fini_cb;
	/* [한국어] 종료 콜백 전역 등록. */

	virtio_blk_transports_destroy();
	/* [한국어] 첫 transport부터 파괴 시작. */
}

/*
 * [한국어]
 * vhost_user_config_json - 단일 vdev에 대한 config_json 출력 헬퍼.
 *
 * @vdev: 컨트롤러
 * @w: JSON 출력 컨텍스트
 *
 * 1) 백엔드별 write_config_json 호출 (LUN 추가 등 RPC 재현 명령 출력).
 * 2) coalescing 값이 0이 아니면 vhost_controller_set_coalescing RPC도 출력.
 *
 * save_config로 시스템 상태를 JSON으로 저장 → 후속 부팅에서 load_config로 재현.
 */
static void
vhost_user_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	uint32_t delay_base_us;
	uint32_t iops_threshold;

	vdev->backend->write_config_json(vdev, w);
	/* [한국어] 백엔드별 RPC 직렬화 — vhost_create_xxx/scsi_target_add_lun 등 출력. */

	spdk_vhost_get_coalescing(vdev, &delay_base_us, &iops_threshold);
	/* [한국어] 현재 coalescing 값 조회. */
	if (delay_base_us) {
		/* [한국어] 0이 아니면 사용자가 명시 설정한 것으로 간주 — RPC 재현 출력. */
		spdk_json_write_object_begin(w);
		/* [한국어] { 시작. */
		spdk_json_write_named_string(w, "method", "vhost_controller_set_coalescing");
		/* [한국어] RPC 메서드 이름. */

		spdk_json_write_named_object_begin(w, "params");
		/* [한국어] "params": { 시작. */
		spdk_json_write_named_string(w, "ctrlr", vdev->name);
		/* [한국어] 컨트롤러 이름. */
		spdk_json_write_named_uint32(w, "delay_base_us", delay_base_us);
		spdk_json_write_named_uint32(w, "iops_threshold", iops_threshold);
		spdk_json_write_object_end(w);
		/* [한국어] params 닫기. */

		spdk_json_write_object_end(w);
		/* [한국어] 메서드 객체 닫기. */
	}
}

/*
 * [한국어]
 * spdk_vhost_scsi_config_json - 모든 SCSI vdev의 config_json 출력.
 *
 * @w: JSON write context (RPC save_config 응답)
 *
 * 트리 순회하며 SCSI 백엔드인 vdev만 dump. 결과는 JSON 배열.
 */
void
spdk_vhost_scsi_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_dev *vdev;

	spdk_json_write_array_begin(w);
	/* [한국어] [ 시작 — RPC 명령 시퀀스 배열. */

	spdk_vhost_lock();
	/* [한국어] 트리 순회 보호. */
	for (vdev = spdk_vhost_dev_next(NULL); vdev != NULL;
	     vdev = spdk_vhost_dev_next(vdev)) {
		/* [한국어] 트리 in-order 순회. */
		if (vdev->backend->type == VHOST_BACKEND_SCSI) {
			/* [한국어] SCSI 백엔드만 출력 (BLK는 별도 함수). */
			vhost_user_config_json(vdev, w);
		}
	}
	spdk_vhost_unlock();

	spdk_json_write_array_end(w);
	/* [한국어] ] 종료. */
}

/*
 * [한국어]
 * vhost_blk_dump_config_json - BLK transport(들)에 대한 RPC 재현 출력.
 *
 * @w: JSON write context
 *
 * 기본 transport "vhost_user_blk"는 SPDK가 자동 생성하므로 출력에서 제외하고,
 * 사용자가 명시 생성한 다른 transport만 virtio_blk_create_transport RPC로 출력한다.
 */
static void
vhost_blk_dump_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_virtio_blk_transport *transport;

	/* Write vhost transports */
	TAILQ_FOREACH(transport, &g_virtio_blk_transports, tailq) {
		/* [한국어] 모든 등록된 transport 순회. */
		/* Since vhost_user_blk is always added on SPDK startup,
		 * do not emit virtio_blk_create_transport RPC. */
		if (strcasecmp(transport->ops->name, "vhost_user_blk") != 0) {
			/* [한국어] 기본이 아닌 transport만 — 재현 출력. */
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "method", "virtio_blk_create_transport");
			spdk_json_write_named_object_begin(w, "params");
			transport->ops->dump_opts(transport, w);
			/* [한국어] transport별 옵션을 ops에 위임해 출력. */
			spdk_json_write_object_end(w);
			spdk_json_write_object_end(w);
		}
	}
}

/*
 * [한국어]
 * spdk_vhost_blk_config_json - 모든 BLK vdev + transport 옵션 출력.
 *
 * @w: JSON write context
 *
 * 1) BLK 백엔드 vdev들의 RPC 재현 출력.
 * 2) 그 후 비기본 transport들의 RPC 재현 출력.
 */
void
spdk_vhost_blk_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_dev *vdev;

	spdk_json_write_array_begin(w);

	spdk_vhost_lock();
	for (vdev = spdk_vhost_dev_next(NULL); vdev != NULL;
	     vdev = spdk_vhost_dev_next(vdev)) {
		if (vdev->backend->type == VHOST_BACKEND_BLK) {
			/* [한국어] BLK 컨트롤러만. */
			vhost_user_config_json(vdev, w);
		}
	}
	spdk_vhost_unlock();

	vhost_blk_dump_config_json(w);
	/* [한국어] 추가로 transport 옵션도 같은 배열 안에 출력. */

	spdk_json_write_array_end(w);
}

/*
 * [한국어]
 * virtio_blk_transport_register - transport ops 전역 등록.
 *
 * @ops: 등록할 ops 구조 (호출자 메모리, 본 함수 내에서 복사 보관).
 *
 * 동기/배경: 새 BLK transport(예: vhost_user_blk)를 컴파일 타임에 등록하기 위해
 * SPDK_VIRTIO_BLK_TRANSPORT_REGISTER 매크로(constructor)에서 호출.
 *
 * 절차:
 *  1) 동일 이름이 이미 있으면 거부 (assert).
 *  2) 리스트 노드 calloc.
 *  3) ops 복사 후 TAILQ에 추가.
 *
 * 실행 컨텍스트: 프로세스 시작 시 1회 (constructor) — 단일 스레드.
 */
void
virtio_blk_transport_register(const struct spdk_virtio_blk_transport_ops *ops)
{
	struct virtio_blk_transport_ops_list_element *new_ops;
	/* [한국어] 새로 추가할 리스트 노드. */

	if (virtio_blk_get_transport_ops(ops->name) != NULL) {
		/* [한국어] 동일 이름 transport가 이미 등록됨 — 프로그래밍 오류. */
		SPDK_ERRLOG("Double registering virtio blk transport type %s.\n", ops->name);
		assert(false);
		return;
	}

	new_ops = calloc(1, sizeof(*new_ops));
	/* [한국어] 노드 할당 (zero-init). */
	if (new_ops == NULL) {
		/* [한국어] OOM — 등록 실패 (assert로 진단 빌드에서 abort). */
		SPDK_ERRLOG("Unable to allocate memory to register new transport type %s.\n", ops->name);
		assert(false);
		return;
	}

	new_ops->ops = *ops;
	/* [한국어] ops 구조체 복사 — 호출자 ops 메모리 수명에 의존하지 않게. */

	TAILQ_INSERT_TAIL(&g_spdk_virtio_blk_transport_ops, new_ops, link);
	/* [한국어] 끝에 추가. */
}

/*
 * [한국어]
 * virtio_blk_transport_create - transport 인스턴스 생성.
 *
 * @transport_name: transport 이름 (등록된 ops의 키).
 * @params: ops->create에 전달할 파라미터 JSON.
 * @return: 0=성공, -EEXIST/-ENOENT/-EPERM.
 *
 * 절차:
 *  1) g_virtio_blk_transports에 동일 이름 instance가 있으면 -EEXIST.
 *  2) ops 검색, 없으면 -ENOENT.
 *  3) ops->create로 인스턴스 생성, NULL이면 -EPERM.
 *  4) 인스턴스를 g_virtio_blk_transports에 추가.
 *
 * 호출 체인: spdk_vhost_blk_init / RPC virtio_blk_create_transport → virtio_blk_transport_create.
 */
int
virtio_blk_transport_create(const char *transport_name,
			    const struct spdk_json_val *params)
{
	const struct spdk_virtio_blk_transport_ops *ops = NULL;
	struct spdk_virtio_blk_transport *transport;

	TAILQ_FOREACH(transport, &g_virtio_blk_transports, tailq) {
		/* [한국어] 동일 이름 인스턴스 존재 검사. */
		if (strcasecmp(transport->ops->name, transport_name) == 0) {
			return -EEXIST;
		}
	}

	ops = virtio_blk_get_transport_ops(transport_name);
	/* [한국어] ops 등록부에서 찾기. */
	if (!ops) {
		/* [한국어] 사용자가 빌드 시 미포함 transport를 요청. */
		SPDK_ERRLOG("Transport type '%s' unavailable.\n", transport_name);
		return -ENOENT;
	}

	transport = ops->create(params);
	/* [한국어] 백엔드별 인스턴스 생성 (소켓 listen 준비 등). */
	if (!transport) {
		SPDK_ERRLOG("Unable to create new transport of type %s\n", transport_name);
		return -EPERM;
	}

	transport->ops = ops;
	/* [한국어] 백포인터 설정 — 이후 ops 호출에 사용. */
	TAILQ_INSERT_TAIL(&g_virtio_blk_transports, transport, tailq);
	/* [한국어] 인스턴스 리스트에 추가. */
	return 0;
}

/*
 * [한국어]
 * virtio_blk_transport_get_first - 인스턴스 리스트의 첫 항목.
 *
 * @return: 첫 transport 또는 NULL.
 */
struct spdk_virtio_blk_transport *
virtio_blk_transport_get_first(void)
{
	return TAILQ_FIRST(&g_virtio_blk_transports);
}

/*
 * [한국어]
 * virtio_blk_transport_get_next - 다음 인스턴스.
 *
 * @transport: 현재 transport
 * @return: 다음 transport 또는 NULL.
 */
struct spdk_virtio_blk_transport *
virtio_blk_transport_get_next(struct spdk_virtio_blk_transport *transport)
{
	return TAILQ_NEXT(transport, tailq);
}

/*
 * [한국어]
 * virtio_blk_transport_dump_opts - 단일 transport 옵션 JSON 출력.
 *
 * @transport: 대상 transport
 * @w: JSON write context
 *
 * RPC virtio_blk_get_transports 등에서 사용 — { "name": ..., 옵션들 } 형태.
 */
void
virtio_blk_transport_dump_opts(struct spdk_virtio_blk_transport *transport,
			       struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	/* [한국어] { 시작. */

	spdk_json_write_named_string(w, "name", transport->ops->name);
	/* [한국어] transport 이름 출력. */

	if (transport->ops->dump_opts) {
		/* [한국어] 백엔드별 옵션이 있으면 위임. */
		transport->ops->dump_opts(transport, w);
	}

	spdk_json_write_object_end(w);
	/* [한국어] } 종료. */
}

/*
 * [한국어]
 * virtio_blk_tgt_get_transport - 이름으로 transport 인스턴스 조회.
 *
 * @transport_name: 찾을 이름.
 * @return: 인스턴스 포인터 또는 NULL.
 *
 * RPC vhost_create_blk_controller가 transport=... 옵션 처리 시 사용.
 */
struct spdk_virtio_blk_transport *
virtio_blk_tgt_get_transport(const char *transport_name)
{
	struct spdk_virtio_blk_transport *transport;

	TAILQ_FOREACH(transport, &g_virtio_blk_transports, tailq) {
		/* [한국어] 모든 인스턴스 순회. */
		if (strcasecmp(transport->ops->name, transport_name) == 0) {
			return transport;
		}
	}
	return NULL;
}

/*
 * [한국어]
 * virtio_blk_transport_destroy - transport 인스턴스 비동기 파괴.
 *
 * @transport: 대상 transport (호출 시점에 g_virtio_blk_transports에서 이미 제거됨).
 * @cb_fn: 파괴 완료 콜백.
 * @return: ops->destroy의 결과 (보통 0=async 시작, 또는 음수 에러).
 *
 * 호출 체인: virtio_blk_transports_destroy → virtio_blk_transport_destroy → ops->destroy → cb_fn().
 */
int
virtio_blk_transport_destroy(struct spdk_virtio_blk_transport *transport,
			     spdk_vhost_fini_cb cb_fn)
{
	return transport->ops->destroy(transport, cb_fn);
	/* [한국어] 백엔드 destroy에 위임 (소켓 close, 자원 회수). */
}

SPDK_LOG_REGISTER_COMPONENT(vhost)
/* [한국어] SPDK 로그 컴포넌트 "vhost" 등록 — SPDK_INFOLOG(vhost, ...)에서 사용. */
SPDK_LOG_REGISTER_COMPONENT(vhost_ring)
/* [한국어] SPDK 로그 컴포넌트 "vhost_ring" 등록 — virtqueue 디버그 로그용. */
