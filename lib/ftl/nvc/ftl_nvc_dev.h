/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
  */

/*
 * [한국어 설명] FTL NV Cache 디바이스 추상화 헤더 (ftl_nvc_dev.h)
 *
 * === 파일의 역할 ===
 * SPDK FTL이 사용하는 NV Cache(비휘발성 캐시) 백엔드를 디바이스 종류별로 플러그인화하기
 * 위한 인터페이스를 정의한다. 각 NVC 디바이스 모델(VSS 지원 NAND, 일반 bdev 등)은
 * 이 헤더의 ftl_nv_cache_device_type / ftl_nv_cache_device_ops 구조체를 채워서
 * `FTL_NV_CACHE_DEVICE_TYPE_REGISTER`로 글로벌 레지스트리에 자동 등록된다.
 * FTL 코어는 런타임에 호환되는 타입을 골라 ops 콜백을 통해 모든 NVC 동작을 위임한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   FTL 초기화 (ftl_mngt) → ftl_nv_cache_device_get_type_by_bdev()
 *     → ops.is_bdev_compatible() / ops.init() / ops.setup_layout() …
 *   FTL 사용자 IO 경로 → ftl_nv_cache.write() → 선택된 nvc_type->ops.write(io)
 *   FTL 복구 경로 → ops.recover_open_chunk() / ops.on_chunk_open/closed()
 * 즉, 이 헤더는 FTL 코어와 NVC 모델 구현(ftl_nvc_bdev_*.c) 사이의 V-table 역할이다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/ftl/nvc/ftl_nvc_dev.c                    : 등록·조회 구현 (g_devs 리스트)
 * - lib/ftl/nvc/ftl_nvc_bdev_vss.c / non_vss.c   : 실제 ops 구현체 (정적 디스크립터)
 * - lib/ftl/ftl_nv_cache.c                       : 코어가 ops 호출을 통해 NVC 사용
 * - lib/ftl/ftl_layout.h (struct ftl_md_layout_ops) : 메타데이터 영역 생성/오픈 ops 포함
 * - lib/ftl/utils/ftl_layout_tracker_bdev.c      : NVC 영역 배치 추적기
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ftl_nv_cache_device_features : (현재 placeholder) 향후 capability flag 보관
 * - struct ftl_nv_cache_device_ops      : 디바이스 V-table — init/deinit/write/recover 등
 * - struct ftl_nv_cache_device_type     : 이름 + ops + features + 내부 리스트 entry 묶음
 * - FTL_NV_CACHE_DEVICE_TYPE_REGISTER   : 컨스트럭터로 자동 등록하는 매크로
 * - ftl_nv_cache_device_register()      : 등록 진입점 (매크로가 사용)
 * - ftl_nv_cache_device_get_type_by_bdev(): bdev 호환 타입 선택 진입점
 */

#ifndef FTL_NV_CACHE_DEVICE_H
#define FTL_NV_CACHE_DEVICE_H

#include "spdk/stdinc.h"        /* [한국어] 표준 include 묶음 */
#include "spdk/bdev_module.h"   /* [한국어] struct spdk_bdev 등 bdev 모듈 API */
#include "ftl_layout.h"         /* [한국어] ftl_md_layout_ops 등 layout 정의 */

/* [한국어] 전방 선언 — 이 헤더에서는 포인터 사용만 하므로 전체 정의 include 불요. */
struct spdk_ftl_dev;            /* [한국어] FTL 인스턴스 핸들 */
struct ftl_nv_cache_chunk;      /* [한국어] NV cache 내 chunk 단위 객체 */
struct ftl_io;                  /* [한국어] FTL 내부 IO 표현 */
struct ftl_mngt_process;        /* [한국어] FTL 관리 절차(state machine) 핸들 */

/**
 * @brief NV Cache device features and capabilities
 */
/* [한국어] NVC 디바이스가 광고할 기능 플래그 묶음.
 * 현재는 빈 구조체 — 향후 어떤 NVC 기능(예: 원자적 쓰기, RAID 미러, 멀티 큐 등)이 켜져
 * 있는지 비트필드 형태로 추가될 자리이다. ABI 안정성을 위해 미리 빈 구조체로 정의해
 * 추후 필드 추가 시 디스크립터 정의를 깨지 않도록 한다. */
struct ftl_nv_cache_device_features {
	/*
	 * The placeholder for NV Cache device features. It will be filled in the future.
	 */
};

/**
 * @brief NV Cache device operations interface
 */
/* [한국어] NVC 모델별 동작을 캡슐화하는 V-table.
 * 모든 ops 콜백은 FTL 코어 스레드(특정 reactor)에서 호출되며, 일부(write/process)는
 * IO 핫 패스에서 매우 자주 호출된다. 콜백은 NULL일 수 있고, 호출 전에 코어가 NULL
 * 검사를 수행한다 (각 콜백 멤버 주석에서 사용처 명시). */
struct ftl_nv_cache_device_ops {
	/* [한국어] 디바이스 초기화 콜백.
	 * 호출 시점: ftl_mngt 초기화 단계, nvc_type이 선택된 직후.
	 * 책임: P2L 로그 풀 같은 모델 고유 자원 할당.
	 * 반환: 0 성공, 음수 실패 (실패 시 FTL 초기화 자체가 중단됨). */
	int (*init)(struct spdk_ftl_dev *dev);

	/* [한국어] 디바이스 해제 콜백.
	 * 호출 시점: FTL 종료 시 (ftl_mngt deinit 단계). init이 할당한 자원을 모두 회수. */
	void (*deinit)(struct spdk_ftl_dev *dev);

	/* [한국어] chunk가 open 상태로 전이될 때 알림.
	 * 호출 시점: ftl_nv_cache_chunk_open() 등 — 새 chunk를 쓰기에 사용하기 시작할 때.
	 * 용도: 모델별 부가 자원(P2L log slot 등) 획득. */
	void (*on_chunk_open)(struct spdk_ftl_dev *dev, struct ftl_nv_cache_chunk *chunk);

	/* [한국어] chunk가 close되어 더 이상 쓰지 않을 때 알림.
	 * 호출 시점: chunk가 가득 차거나 명시적으로 닫힐 때.
	 * 용도: on_chunk_open에서 잡은 자원 반환. */
	void (*on_chunk_closed)(struct spdk_ftl_dev *dev, struct ftl_nv_cache_chunk *chunk);

	/* [한국어] 후보 bdev가 이 NVC 모델로 사용 가능한지 판정.
	 * 호출 시점: ftl_nv_cache_device_get_type_by_bdev()의 폴링 중.
	 * 검사 항목 예: spdk_bdev_get_md_size, spdk_bdev_is_md_separate, dif_type 등. */
	bool (*is_bdev_compatible)(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev);

	/* [한국어] chunk_offset(블록 단위 시작 주소)이 활성 영역인지 판정.
	 * 용도: 일부 모델(예: layout tracker로 영역을 분할 관리)이 비활성 chunk를 건너뛸 때. */
	bool (*is_chunk_active)(struct spdk_ftl_dev *dev, uint64_t chunk_offset);

	/* [한국어] 사용자 IO를 NVC에 비동기 기록.
	 * 호출 시점: FTL 사용자 쓰기 경로 (핫 패스).
	 * 책임: 모델 형식(VSS/non-VSS)에 맞춰 spdk_bdev_writev_blocks(_with_md) 호출,
	 *       완료 시 ftl_nv_cache_write_complete() 호출 보장. */
	void (*write)(struct ftl_io *io);

	/* [한국어] 주기적 처리 (poller에서 호출).
	 * 용도: P2L 로그 flush, 백그라운드 메인터넌스 등 모델별 정기 작업. */
	void (*process)(struct spdk_ftl_dev *dev);

	/* [한국어] 비정상 종료 후 재시작 시 open 상태로 남아 있던 chunk를 복구.
	 * mngt: 현재 진행 중인 FTL 관리 프로세스 핸들 — 비동기 단계 전이에 사용.
	 * 규약: 성공 시 ftl_mngt_next_step(mngt), 실패 시 ftl_mngt_fail_step(mngt). */
	void (*recover_open_chunk)(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
				   struct ftl_nv_cache_chunk *chunk);

	/* [한국어] NVC 영역의 메타데이터 layout을 등록.
	 * 호출 시점: layout 초기 구성 단계. md_layout_ops로 region_create/open을 사용해
	 * P2L log 영역 등을 superblock에 예약한다. */
	int (*setup_layout)(struct spdk_ftl_dev *dev);

	/* [한국어] 메타데이터 region을 만들고 여는 V-table.
	 * region_create: superblock에 영역 예약, region_open: 실제 ftl_md 핸들 구성.
	 * VSS/non-VSS 모두 ftl_nvc_bdev_common.c의 공통 구현을 공유하는 경우가 많다. */
	struct ftl_md_layout_ops md_layout_ops;
};

/**
 * @brief NV Cache device type
 */
/* [한국어] NVC 모델 디스크립터 — 정적 객체로 한 번 만들어 register하면 평생 유효.
 * 각 NVC 모델 .c 파일이 이 구조체를 한 인스턴스 정의해 두고 매크로로 컨스트럭터 등록한다. */
struct ftl_nv_cache_device_type {
	const char *name;
	/* [한국어] 모델 식별 이름 (예: "bdev", "bdev-non-vss"). NULL/빈 문자열 금지.
	 * 설정자: 디스크립터 정의자 (정적 초기자).
	 * 읽는 자: 등록·조회 코드의 strcmp 비교, 로그 출력. */

	const struct ftl_nv_cache_device_features features;
	/* [한국어] 모델이 지원하는 capability flag 묶음 (현재 placeholder).
	 * 향후 추가 필드는 디바이스 정의자가 정적 초기화로 채운다. */

	const struct ftl_nv_cache_device_ops ops;
	/* [한국어] 모델별 V-table — 위에서 정의한 콜백 묶음.
	 * 정적 초기화로 채워지며, 일부 콜백은 NULL일 수 있다. */

	/** Internal fields */
	struct {
		/* [한국어] g_devs 리스트에 매다는 link.
		 * 설정자: ftl_nv_cache_device_register()에서 TAILQ_INSERT_TAIL.
		 * 읽는 자: TAILQ_FOREACH 순회.
		 * 외부에서 직접 접근 금지 — 따라서 internal 서브구조체로 격리. */
		TAILQ_ENTRY(ftl_nv_cache_device_type) entry;
	} internal;
};

/**
 * @brief Macro to register NV Cache device type when the module is loaded
 *
 * @param desc NV Cache device type
 */
/* [한국어] 정적 디스크립터를 main() 진입 전에 자동 등록하는 매크로.
 * __attribute__((constructor))는 GCC 확장으로, ELF의 .init_array에 함수를 등록해
 * 동적 로더가 호출하도록 한다. SPDK에서는 모듈 자동 등록의 표준 패턴이다. */
#define FTL_NV_CACHE_DEVICE_TYPE_REGISTER(desc) \
static void __attribute__((constructor)) ftl_nv_cache_device_register_##desc(void) \
{ \
	ftl_nv_cache_device_register(&desc); \
}

/**
 * @brief Register NV Cache device type
 *
 * @param type NV Cache device type
 */
/* [한국어] 등록 진입점 — 위 매크로가 사용. ftl_nvc_dev.c 구현 참조. */
void ftl_nv_cache_device_register(struct ftl_nv_cache_device_type *type);

/**
 * @brief Get NV Cache device type by bdev
 *
 * @param bdev bdev for which NV Cache device type is requested
 *
 * @return NV Cache device type
 */
/* [한국어] FTL 초기화 시 bdev에 맞는 NVC 타입을 선택해 돌려준다. ftl_nvc_dev.c 참조. */
const struct ftl_nv_cache_device_type *ftl_nv_cache_device_get_type_by_bdev(
	struct spdk_ftl_dev *dev, struct spdk_bdev *bdev);

#endif /* FTL_NV_CACHE_DEVICE_H */
