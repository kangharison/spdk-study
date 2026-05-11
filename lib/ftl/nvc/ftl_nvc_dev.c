/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

/*
 * [한국어 설명] FTL NV Cache 디바이스 타입 레지스트리 (ftl_nvc_dev.c)
 *
 * === 파일의 역할 ===
 * SPDK FTL(Flash Translation Layer)의 NVC(Non-Volatile Cache, 비휘발성 캐시) 디바이스
 * 타입을 등록·탐색하는 글로벌 레지스트리를 구현한다. NVC는 FTL 내부에서 사용자 쓰기와
 * 메타데이터를 일시적으로 보관하는 빠른 영역(주로 Optane 같은 SCM이나 일반 NAND)이며,
 * 디바이스 모델별로 메타데이터 형식·VSS(vector spare storage) 사용 여부 등이 다르다.
 * 이 파일은 그러한 모델별 구현을 "이름→ftl_nv_cache_device_type 디스크립터"로 묶고,
 * FTL 코어가 실제 bdev에 맞는 NVC 타입을 런타임에 선택할 수 있게 해 준다.
 * 등록은 모듈 로드 시 컨스트럭터(`FTL_NV_CACHE_DEVICE_TYPE_REGISTER`)로 자동 수행된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * FTL 초기화 흐름의 NVC 바인딩 단계에 해당한다. 호출 체인 예시는 다음과 같다:
 *   ftl_mngt (디바이스 init 단계)
 *     → ftl_nv_cache_init() / dev_init
 *       → ftl_nv_cache_device_get_type_by_bdev(dev, bdev)
 *         → 각 ftl_nv_cache_device_type::ops.is_bdev_compatible() 폴링
 *         → 매칭된 타입을 dev->nv_cache.nvc_type 에 저장
 * 등록 측 호출 체인은 컨스트럭터에서 ftl_nv_cache_device_register()를 호출하므로
 * `main()` 진입 전에 모든 NVC 타입이 g_devs 리스트에 들어와 있다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/ftl/nvc/ftl_nvc_bdev_vss.c (struct nvc_bdev_vss)        : VSS 메타데이터 사용 NAND 모델
 * - lib/ftl/nvc/ftl_nvc_bdev_non_vss.c (struct nvc_bdev_non_vss): VSS 미지원 일반 bdev 모델
 * - lib/ftl/nvc/ftl_nvc_dev.h                                    : 디스크립터·ops 구조체 정의
 * - lib/ftl/ftl_nv_cache.c                                       : 선택된 nvc_type을 통해 IO/메타 처리
 * 데이터 흐름: 등록 시점에는 정적 디스크립터 포인터가 g_devs에 매달리고, 조회 시에는
 * 그 포인터를 그대로 반환한다. 자체 데이터 복사는 하지 않으며, ops 호출만 위임한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - ftl_nv_cache_device_type_get_type(name)        : 이름으로 등록 디스크립터 조회 (static)
 * - ftl_nv_cache_device_valid(type)                : 디스크립터 유효성 검증 (static)
 * - ftl_nv_cache_device_register(type)             : 컨스트럭터에서 호출되는 등록 진입점
 * - ftl_nv_cache_device_get_type_by_bdev(dev,bdev) : bdev와 호환되는 NVC 타입 선택
 * - g_devs / g_devs_mutex                          : 등록된 NVC 타입 리스트와 보호 락
 */

#include "spdk/stdinc.h"        /* [한국어] SPDK 표준 include 묶음 (stdint, string, pthread 등) */
#include "spdk/queue.h"         /* [한국어] TAILQ_* 매크로 — g_devs 리스트 구현에 사용 */
#include "spdk/log.h"           /* [한국어] SPDK_NOTICELOG/SPDK_ERRLOG 매크로 */

#include "ftl_nvc_dev.h"        /* [한국어] ftl_nv_cache_device_type / ops 정의 */
#include "utils/ftl_defs.h"     /* [한국어] ftl_abort() 등 공용 매크로 */

/* [한국어] 등록된 모든 NVC 디바이스 타입을 매다는 글로벌 리스트.
 * 설정자: ftl_nv_cache_device_register() (각 NVC 타입의 컨스트럭터에서 호출)
 * 읽는 자: ftl_nv_cache_device_type_get_type()/_get_type_by_bdev()
 * 동기화: g_devs_mutex로 직렬화. 일반적으로 등록은 main() 이전 단일 스레드 컨스트럭터,
 *         조회는 FTL 초기화 시 단일 스레드라 경쟁은 드물지만 안전하게 락을 잡는다. */
static TAILQ_HEAD(, ftl_nv_cache_device_type) g_devs = TAILQ_HEAD_INITIALIZER(g_devs);
/* [한국어] g_devs 보호용 정적 초기화 mutex. 컨스트럭터 시점부터 사용 가능해야 하므로
 * PTHREAD_MUTEX_INITIALIZER로 컴파일 타임 초기화한다. */
static pthread_mutex_t g_devs_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * [한국어]
 * ftl_nv_cache_device_type_get_type - 이름으로 등록된 NVC 타입 조회 (static helper)
 *
 * @name: 비교할 NVC 타입 이름 문자열 (예: "bdev", "bdev-non-vss")
 * @return: 매칭된 ftl_nv_cache_device_type 포인터, 없으면 NULL
 *
 * 등록 단계에서 같은 이름이 중복 등록되지 않게 검사할 때 호출자가 사용한다.
 * 호출자가 g_devs_mutex를 잡고 들어와야 안전하다 (이 함수 자체는 락을 잡지 않음).
 *
 * 호출 체인:
 *   ftl_nv_cache_device_register() → [이 함수]
 */
static const struct ftl_nv_cache_device_type *
ftl_nv_cache_device_type_get_type(const char *name)
{
	struct ftl_nv_cache_device_type *entry;          /* [한국어] 순회 임시 포인터 */

	/* [한국어] g_devs 리스트를 선형 탐색 — 등록된 타입 수가 매우 적으므로 O(N) 충분. */
	TAILQ_FOREACH(entry, &g_devs, internal.entry) {
		/* [한국어] strcmp 결과 0 == 이름 일치. 동일 이름의 중복 등록을 막기 위함. */
		if (0 == strcmp(entry->name, name)) {
			return entry;                    /* [한국어] 매칭 발견 → 즉시 반환 */
		}
	}

	return NULL;                                     /* [한국어] 일치 항목 없음 */
}

/*
 * [한국어]
 * ftl_nv_cache_device_valid - 디스크립터의 최소 유효성 검증 (static helper)
 *
 * @type: 검증할 디스크립터 (NULL 가능)
 * @return: 포인터가 유효하고 name이 빈 문자열이 아니면 true, 아니면 false
 *
 * 등록 시 잘못된 정적 디스크립터를 빠르게 걸러내기 위한 보조 함수이다.
 * 이름이 비어 있으면 strcmp 기반 조회·로그가 무의미하므로 등록을 막는다.
 */
static bool
ftl_nv_cache_device_valid(const struct ftl_nv_cache_device_type *type)
{
	/* [한국어] 단계적 단락 평가 — type NULL 보호 → name 포인터 보호 → 길이 > 0 확인 */
	return type && type->name && strlen(type->name) > 0;
}

/*
 * [한국어]
 * ftl_nv_cache_device_register - NVC 디바이스 타입을 글로벌 레지스트리에 등록
 *
 * @type: 등록할 정적 디스크립터 (생명주기는 모듈 전체)
 *
 * FTL_NV_CACHE_DEVICE_TYPE_REGISTER 매크로가 만든 컨스트럭터(__attribute__((constructor)))
 * 가 main() 진입 전에 자동으로 호출한다. 이름 충돌이 발생하면 ftl_abort()로 즉시 종료한다.
 * 등록된 디스크립터는 절대 free되지 않으며, 프로세스 수명 동안 유효한 정적 객체로 가정한다.
 *
 * 실행 컨텍스트: 컨스트럭터 단계 (단일 스레드, 다른 SPDK 초기화보다 먼저 수행).
 * 동기화: g_devs_mutex로 보호 — 이론상 동시 컨스트럭터 진입을 가정해 안전하게 잡는다.
 *
 * 호출 체인:
 *   __attribute__((constructor)) ftl_nv_cache_device_register_<desc>()
 *     → [이 함수] → TAILQ_INSERT_TAIL(g_devs, type)
 */
void
ftl_nv_cache_device_register(struct ftl_nv_cache_device_type *type)
{
	/* [한국어] 디스크립터 무결성 사전 점검 — 잘못된 입력은 즉시 abort 한다. */
	if (!ftl_nv_cache_device_valid(type)) {
		SPDK_ERRLOG("NV cache device descriptor is invalid\n");
		ftl_abort();                             /* [한국어] 복구 불가능 — 프로세스 종료 */
	}

	pthread_mutex_lock(&g_devs_mutex);                /* [한국어] g_devs 보호 락 획득 */
	/* [한국어] 같은 이름이 이미 등록되어 있는지 확인 — 중복 등록은 설계상 버그로 간주. */
	if (!ftl_nv_cache_device_type_get_type(type->name)) {
		TAILQ_INSERT_TAIL(&g_devs, type, internal.entry); /* [한국어] 리스트 끝에 삽입 */
		SPDK_NOTICELOG("Registered NV cache device, name: %s\n", type->name);
	} else {
		/* [한국어] 동일 이름 중복은 정적 디스크립터 정의 오류이므로 abort. */
		SPDK_ERRLOG("Cannot register NV cache device, already exists, name: %s\n", type->name);
		ftl_abort();
	}

	pthread_mutex_unlock(&g_devs_mutex);              /* [한국어] 락 해제 */
}

/*
 * [한국어]
 * ftl_nv_cache_device_get_type_by_bdev - bdev와 호환되는 NVC 타입을 선택
 *
 * @dev:  현재 초기화 중인 FTL 디바이스 (호환성 판단 컨텍스트로 전달)
 * @bdev: 후보가 되는 NVC 백엔드 SPDK bdev
 * @return: 호환 가능한 ftl_nv_cache_device_type, 없으면 NULL
 *
 * 등록된 모든 NVC 타입을 순회하며 각 타입의 ops.is_bdev_compatible() 콜백에
 * 디바이스/bdev 쌍을 넘겨 첫 번째로 true를 반환하는 타입을 채택한다.
 * 일반적으로 VSS 메타데이터 지원 여부, md_size, dif_type 등을 검사한다.
 *
 * 실행 컨텍스트: FTL 초기화의 ftl_mngt 단계 (단일 스레드, 콜백은 동기 실행).
 * 호출자는 결과를 dev->nv_cache.nvc_type 에 저장해 이후 IO/메타 처리에 사용한다.
 *
 * 호출 체인:
 *   ftl_nv_cache_init() → [이 함수] → 각 type->ops.is_bdev_compatible(dev, bdev)
 */
const struct ftl_nv_cache_device_type *
ftl_nv_cache_device_get_type_by_bdev(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	struct ftl_nv_cache_device_type *entry;          /* [한국어] 순회 임시 변수 */
	const struct ftl_nv_cache_device_type *type = NULL; /* [한국어] 매칭 결과 (없으면 NULL) */

	pthread_mutex_lock(&g_devs_mutex);                /* [한국어] 등록 리스트 보호 락 */
	/* [한국어] 등록 순으로 호환성 검사 — 첫 번째 호환 타입이 우선권을 가진다.
	 * 따라서 더 특수한 타입(VSS)을 먼저 등록하도록 컨스트럭터 순서를 의도한다. */
	TAILQ_FOREACH(entry, &g_devs, internal.entry) {
		/* [한국어] is_bdev_compatible 콜백이 정의된 경우에만 검사 가능. */
		if (entry->ops.is_bdev_compatible) {
			if (entry->ops.is_bdev_compatible(dev, bdev)) {
				type = entry;            /* [한국어] 호환 발견 → 결과 저장 */
				break;                   /* [한국어] 더 검색하지 않고 종료 */
			}
		}
	}
	pthread_mutex_unlock(&g_devs_mutex);              /* [한국어] 락 해제 */

	return type;                                      /* [한국어] 매칭된 타입(또는 NULL) 반환 */
}
