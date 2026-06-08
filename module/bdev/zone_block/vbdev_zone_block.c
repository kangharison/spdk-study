/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * [한국어 설명] zone_block vbdev — 일반 블록 bdev 위에 NVMe ZNS 인터페이스를 시뮬레이션 (vbdev_zone_block.c)
 *
 * === 파일의 역할 ===
 * 일반 (non-zoned) base bdev 를 받아 그 위에 NVMe ZNS(Zoned Namespace) bdev 처럼
 * 동작하는 가상 디바이스를 만든다. ZNS 디바이스의 핵심 의미론 — (1) 디바이스가 고정
 * 크기의 zone 들로 분할되고, (2) 각 zone 은 EMPTY → OPEN → FULL 등의 상태 머신을 가지며,
 * (3) write 는 zone 내부에서 sequential (write_pointer 만 따라가도록) 강제, (4) zone
 * 단위의 reset/open/close/finish 같은 management 명령을 지원, (5) zone append (host
 * 가 LBA 를 지정하지 않고 디바이스가 wp 위치에 기록) — 를 모두 메모리상의 block_zone
 * 배열로 구현한다. 실제 데이터는 base bdev 가 갖고, write_pointer/state 같은 메타데이터만
 * 호스트 측 RAM 에 유지한다. NVMe ZNS 1.0 스펙 (또는 TP4053) 의 host-managed 동작을
 * 흉내내며, 테스트/개발용 디바이스로 주로 사용된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (상위 → 하위):
 *   [Application] → spdk_bdev_zone_*(zone API) 또는 일반 read/write
 *     → lib/bdev 가 vbdev 의 fn_table.submit_request 디스패치
 *     → zone_block_submit_request() [본 파일]
 *         ├ GET_ZONE_INFO:   zone_block_get_zone_info → 메모리상 zone_info 복사
 *         ├ ZONE_MANAGEMENT: zone_block_zone_management → state machine 전이
 *         │                  (reset/open/close/finish)
 *         ├ WRITE/APPEND:    zone_block_write → state/wp/capacity 검증 → base bdev write
 *         └ READ:            zone_block_read → range 검증 → base bdev read
 *     → [base bdev] → 실제 디바이스
 *
 * 실행 컨텍스트: I/O 는 채널을 소유한 reactor 에서 실행. zone state machine 의 일부 필드는
 * 멀티 reactor 에서 동시 접근될 수 있으므로 (특히 zone append 의 wp 갱신) zone 마다
 * pthread_spinlock 으로 보호한다. SPDK 의 lockless 원칙에서 예외적인 경우 — 동일 zone 에
 * 여러 reactor 가 동시 append 할 가능성 때문에 spinlock 이 필요.
 *
 * === 타 모듈과의 연결 ===
 * - lib/bdev: 일반 vbdev 등록과 동일 (spdk_bdev_register, module_claim_bdev). 추가로
 *   spdk_bdev_zone_info / zoned=true 등 ZNS specific 필드를 채움.
 * - lib/bdev/bdev_zone.h: SPDK_BDEV_ZONE_STATE_* / SPDK_BDEV_ZONE_TYPE_SEQWR 등 enum 정의.
 * - spdk/nvme.h: NVMe ZNS 관련 매크로/타입 (현재 파일에서는 직접 사용 거의 없음).
 * - vbdev_zone_block.h: vbdev_zone_block_create / vbdev_zone_block_delete API 를 RPC 파일과 공유.
 * - 모든 zone 메타데이터는 base bdev 상에 영속화되지 않음 (재시작 시 모든 zone 이 FULL 로
 *   초기화됨 — zone_block_init_zone_info 참조). 즉 본 모듈은 휘발성 메타데이터 모델.
 *
 * === 주요 함수/구조체 요약 ===
 * 핵심 자료구조:
 *   - struct bdev_zone_block: vbdev 인스턴스 — base_desc + zones 배열 + zone_capacity/shift.
 *   - struct block_zone: 단일 zone 의 메타데이터 + spinlock.
 *   - struct bdev_zone_block_config: deferred examine 용 association (vbdev_name ↔ bdev_name).
 *   - struct zone_block_io_channel: per-thread base bdev 채널 보관.
 * 핵심 함수:
 *   - zone_block_submit_request(): I/O 디스패처.
 *   - zone_block_write(): WRITE/APPEND 의 핵심 — state machine 검증 + wp 갱신 + base bdev write.
 *   - zone_block_zone_management(): RESET/OPEN/CLOSE/FINISH 분기.
 *   - zone_block_register(): examine 단계에서 vbdev 인스턴스 생성 + zone 초기화.
 *   - zone_block_init_zone_info(): 모든 zone 을 FULL 상태로 초기화 (write 안전을 위해).
 */

#include "spdk/stdinc.h"                                                    /* [한국어] SPDK 표준 라이브러리 wrapper (string.h, pthread.h 등). */

#include "vbdev_zone_block.h"                                               /* [한국어] vbdev_zone_block_create/delete 공개 API — RPC 파일과 공유. */

#include "spdk/config.h"                                                    /* [한국어] 빌드 시 결정된 컴파일 옵션 매크로 — 현재 직접 사용은 없으나 의존성 보존. */
#include "spdk/nvme.h"                                                      /* [한국어] NVMe 스펙 상수/구조체 (필요 시 zone descriptor 등에 참조). */
#include "spdk/bdev_zone.h"                                                 /* [한국어] SPDK 의 zoned bdev API — zone_info / state enum 정의. */

#include "spdk/log.h"                                                       /* [한국어] SPDK_ERRLOG / SPDK_DEBUGLOG / SPDK_NOTICELOG. */

/* This namespace UUID was generated using uuid_generate() method. */
/* [한국어] zone_block vbdev 의 UUID 네임스페이스 — base bdev UUID 와 결합해 SHA1 으로 vbdev UUID 결정. */
#define BDEV_ZONE_BLOCK_NAMESPACE_UUID "5f3f485a-d6bb-4443-9de7-023683b77389"

/* [한국어] forward declaration — bdev_module 구조체에서 참조되므로 위에 선언. */
static int zone_block_init(void);
static int zone_block_get_ctx_size(void);
static void zone_block_finish(void);
static int zone_block_config_json(struct spdk_json_write_ctx *w);
static void zone_block_examine(struct spdk_bdev *bdev);

/* [한국어] zone_block bdev module 디스크립터. examine_config 로 모든 등장 bdev 와 매칭 시도. */
static struct spdk_bdev_module bdev_zoned_if = {
	.name = "bdev_zoned_block",                                          /* [한국어] 모듈 식별자 — RPC 응답이나 unregister_by_name 에서 사용. */
	.module_init = zone_block_init,                                      /* [한국어] init: 현재 no-op. */
	.module_fini = zone_block_finish,                                    /* [한국어] fini: 남은 g_bdev_configs 정리. */
	.config_json = zone_block_config_json,                               /* [한국어] save_config 직렬화. */
	.examine_config = zone_block_examine,                                /* [한국어] 새 bdev 등장 시 콜백. */
	.get_ctx_size = zone_block_get_ctx_size,                             /* [한국어] bdev_io driver_ctx 영역 크기 요청. */
};

SPDK_BDEV_MODULE_REGISTER(bdev_zoned_block, &bdev_zoned_if)                  /* [한국어] constructor 시점 모듈 자동 등록 (g_bdev_mgr 에 push). */

/* List of block vbdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
/* [한국어] 등록되었지만 아직 vbdev 인스턴스가 만들어지지 않은 (또는 만들어진 후에도 association 유지)
 * 설정 항목. examine 단계에서 bdev_name 매칭으로 vbdev 를 생성한다. */
struct bdev_zone_block_config {
	char					*vbdev_name;
	/* [한국어] 만들어질 zoned vbdev 의 이름.
	 * 설정자: zone_block_insert_name 에서 strdup.
	 * 읽는 자: zone_block_register 에서 spdk_bdev.name 의 소스. */

	char					*bdev_name;
	/* [한국어] 덮어쓸 base bdev 이름.
	 * 설정자: zone_block_insert_name.
	 * 읽는 자: zone_block_register 에서 examine bdev 와 strcmp. */

	uint64_t				zone_capacity;
	/* [한국어] 한 zone 의 사용 가능 LBA 수 (가용 용량). zone_size 와 다를 수 있음 —
	 * zone_size 는 zone_capacity 를 다음 2의 멱승으로 올림한 값 (LBA 인덱싱 shift 용).
	 * 사용 가능 영역 = [zone_id, zone_id+capacity), 그 사이 [capacity, zone_size) 는 hole. */

	uint64_t				optimal_open_zones;
	/* [한국어] NVMe ZNS 의 optimal_open_zones 필드 — 호스트가 동시에 OPEN 상태로
	 * 유지하기 권장되는 zone 수. 본 시뮬레이터는 실제 강제는 안 하고 단순 보고만 함. */

	TAILQ_ENTRY(bdev_zone_block_config)	link;
};
/* [한국어] 모든 association 의 전역 리스트. */
static TAILQ_HEAD(, bdev_zone_block_config) g_bdev_configs = TAILQ_HEAD_INITIALIZER(g_bdev_configs);

/* [한국어] 메모리상 단일 zone 의 표현 — base bdev 의 zone_size 만큼의 LBA 범위를 담당. */
struct block_zone {
	struct spdk_bdev_zone_info zone_info;
	/* [한국어] zone 의 메타데이터 (lib/bdev/bdev_zone.h 정의).
	 * - zone_info.zone_id: 이 zone 의 시작 LBA.
	 * - zone_info.capacity: 사용 가능 LBA 수 (= bdev_zone_block_config.zone_capacity).
	 * - zone_info.write_pointer: 다음 write 가 갈 LBA (state machine 의 핵심 상태).
	 * - zone_info.state: SPDK_BDEV_ZONE_STATE_EMPTY/OPEN/CLOSED/FULL 등.
	 * - zone_info.type: SPDK_BDEV_ZONE_TYPE_SEQWR (sequential write required).
	 * 설정자: zone_block_init_zone_info / open/close/reset/finish/write 함수.
	 * 읽는 자: get_zone_info / write/read 검증.
	 * 동기화: 본 구조체의 모든 mutation 은 아래 lock 하에서만 수행됨. */

	pthread_spinlock_t lock;
	/* [한국어] zone 단위 spinlock — 멀티 reactor 에서 동시에 같은 zone 에 append 할 때
	 * write_pointer 와 state 의 atomic update 를 보장한다.
	 * 사용 패턴: pthread_spin_lock → 상태 검증 → wp 변경 → pthread_spin_unlock → I/O submit.
	 * 짧은 critical section 이므로 spinlock 선택 (mutex 의 컨텍스트 스위치 비용 회피).
	 * SPDK 의 lockless 원칙에 어긋나지만, ZNS append 의 도메인 특성상 불가피. */
};

/* List of block vbdevs and associated info for each. */
/* [한국어] zoned vbdev 인스턴스 1개를 표현. base bdev 의 blockcnt 와 zone_size 로 zones 배열 크기 결정. */
struct bdev_zone_block {
	struct spdk_bdev		bdev;    /* the block zoned bdev */
	/* [한국어] 외부에 노출되는 SPDK bdev 본체 (embedded). zoned=true 로 마킹.
	 * SPDK_CONTAINEROF(bdev, struct bdev_zone_block, bdev) 으로 역참조. */

	struct spdk_bdev_desc		*base_desc; /* its descriptor we get from open */
	/* [한국어] base bdev 의 open descriptor. spdk_bdev_open_ext() 결과.
	 * I/O submit 시 spdk_bdev_*_blocks 의 desc 인자. */

	struct block_zone		*zones; /* array of zones */
	/* [한국어] zone 메타 배열. 크기 = num_zones. base bdev 의 blockcnt / zone_size 로 결정.
	 * lba >> zone_shift 로 인덱싱 — zone_size 가 2의 멱승이라는 전제하에서 정수 나눗셈 회피. */

	uint64_t			num_zones; /* number of zones */
	/* [한국어] zones 배열 크기 = base_bdev->blockcnt / zone_size (절삭 — 마지막 미완 zone 은 버림). */

	uint64_t			zone_capacity; /* zone capacity */
	/* [한국어] zone 당 사용 가능 LBA 수 (config 에서 받음). zone_size 보다 작거나 같음. */

	uint64_t                        zone_shift; /* log2 of zone_size */
	/* [한국어] zone_size 의 log2 — lba 로부터 zone index 를 구할 때 shift right 로 사용.
	 * zone_size 가 2의 멱승이어야 의미가 있음 (init 단계에서 align64pow2 로 보장). */

	TAILQ_ENTRY(bdev_zone_block)	link;
	/* [한국어] g_bdev_nodes 글로벌 리스트 링크. */

	struct spdk_thread		*thread; /* thread where base device is opened */
	/* [한국어] base bdev open thread — close 도 같은 thread 에서 해야 함. cross-thread 면 send_msg 위임. */
};
/* [한국어] 활성 zoned vbdev 인스턴스 전역 리스트. */
static TAILQ_HEAD(, bdev_zone_block) g_bdev_nodes = TAILQ_HEAD_INITIALIZER(g_bdev_nodes);

/* [한국어] per-thread 채널 — base bdev 채널만 보관. zone 메타데이터는 글로벌 공유 (per-zone lock). */
struct zone_block_io_channel {
	struct spdk_io_channel	*base_ch; /* IO channel of base device */
	/* [한국어] 이 thread 의 base bdev 채널.
	 * 설정자: _zone_block_ch_create_cb.
	 * 읽는 자: I/O submit 시 base 의 ch 인자. */
};

/* [한국어] bdev_io 마다 lib/bdev 가 reserve 하는 driver_ctx 영역 — 현재는 vbdev 역참조만 보관. */
struct zone_block_io {
	/* vbdev to which IO was issued */
	struct bdev_zone_block *bdev_zone_block;
	/* [한국어] 이 I/O 가 향하는 vbdev 인스턴스. 단순 캐시. 현재 코드에서 활용도는 낮음
	 * (대부분 SPDK_CONTAINEROF 로 즉시 역참조). */
};

/*
 * [한국어]
 * zone_block_init - module.module_init — 별도 초기화 없음 (config 는 RPC 로 주입됨).
 */
static int
zone_block_init(void)
{
	return 0;
}

/*
 * [한국어]
 * zone_block_remove_config - g_bdev_configs 에서 association 제거 + 문자열 해제.
 *
 * @name: 제거할 config 노드.
 */
static void
zone_block_remove_config(struct bdev_zone_block_config *name)
{
	TAILQ_REMOVE(&g_bdev_configs, name, link);
	free(name->bdev_name);
	free(name->vbdev_name);
	free(name);
}

/*
 * [한국어]
 * zone_block_finish - module.module_fini — 남은 모든 config 항목 제거.
 *
 * 동기/배경: 모듈 종료 시 vbdev 인스턴스는 이미 unregister 되었지만 association 은
 * 남아있을 수 있으므로 명시적으로 비운다.
 */
static void
zone_block_finish(void)
{
	struct bdev_zone_block_config *name;

	while ((name = TAILQ_FIRST(&g_bdev_configs))) {                          /* [한국어] head 부터 비움. */
		zone_block_remove_config(name);
	}
}

/*
 * [한국어]
 * zone_block_get_ctx_size - module.get_ctx_size — bdev_io driver_ctx 크기 보고.
 */
static int
zone_block_get_ctx_size(void)
{
	return sizeof(struct zone_block_io);
}

/*
 * [한국어]
 * zone_block_config_json - module.config_json — save_config 시 활성 vbdev 들을 RPC 명령으로 직렬화.
 */
static int
zone_block_config_json(struct spdk_json_write_ctx *w)
{
	struct bdev_zone_block *bdev_node;
	struct spdk_bdev *base_bdev = NULL;

	TAILQ_FOREACH(bdev_node, &g_bdev_nodes, link) {                          /* [한국어] 활성 vbdev 전체 순회. */
		base_bdev = spdk_bdev_desc_get_bdev(bdev_node->base_desc);       /* [한국어] base bdev 핸들. */
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_zone_block_create"); /* [한국어] RPC 메서드 이름. */
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&bdev_node->bdev));
		spdk_json_write_named_uint64(w, "zone_capacity", bdev_node->zone_capacity);
		spdk_json_write_named_uint64(w, "optimal_open_zones", bdev_node->bdev.optimal_open_zones);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}

	return 0;
}

/* Callback for unregistering the IO device. */
/*
 * [한국어]
 * _device_unregister_cb - io_device unregister 완료 콜백 — zone spinlock destroy + 메모리 해제.
 */
static void
_device_unregister_cb(void *io_device)
{
	struct bdev_zone_block *bdev_node = io_device;
	uint64_t i;

	free(bdev_node->bdev.name);                                              /* [한국어] strdup 된 vbdev 이름 해제. */
	for (i = 0; i < bdev_node->num_zones; i++) {
		pthread_spin_destroy(&bdev_node->zones[i].lock);                 /* [한국어] 각 zone 의 spinlock 자원 해제 (POSIX 요구사항). */
	}
	free(bdev_node->zones);
	free(bdev_node);
}

/*
 * [한국어]
 * _zone_block_destruct - base bdev close 의 cross-thread trampoline.
 */
static void
_zone_block_destruct(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);                                                   /* [한국어] open thread 에서 close 실행. */
}

/*
 * [한국어]
 * zone_block_destruct - fn_table.destruct — vbdev 정리 트리거.
 *
 * @return: 0 — destruct 가 즉시 완료된 것처럼 보고 (실제로는 _device_unregister_cb 가 비동기 완료).
 *
 * 단계: 리스트 제거 → base claim 해제 → base close (cross-thread 가능) → io_device unregister.
 */
static int
zone_block_destruct(void *ctx)
{
	struct bdev_zone_block *bdev_node = (struct bdev_zone_block *)ctx;

	TAILQ_REMOVE(&g_bdev_nodes, bdev_node, link);                            /* [한국어] 활성 리스트에서 제거. */

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(spdk_bdev_desc_get_bdev(bdev_node->base_desc)); /* [한국어] base bdev 소유권 반환. */

	/* Close the underlying bdev on its same opened thread. */
	if (bdev_node->thread && bdev_node->thread != spdk_get_thread()) {       /* [한국어] cross-thread close — open thread 로 메시지 전달. */
		spdk_thread_send_msg(bdev_node->thread, _zone_block_destruct, bdev_node->base_desc);
	} else {
		spdk_bdev_close(bdev_node->base_desc);                           /* [한국어] same-thread → 직접 close. */
	}

	/* Unregister the io_device. */
	spdk_io_device_unregister(bdev_node, _device_unregister_cb);             /* [한국어] 채널 정리 후 _device_unregister_cb 비동기 호출. */

	return 0;
}

/*
 * [한국어]
 * zone_block_get_zone_containing_lba - LBA → 해당 zone 의 block_zone 포인터.
 *
 * @bdev_node: vbdev 인스턴스.
 * @lba: 임의 LBA.
 * @return: zone 포인터 또는 NULL (LBA 가 범위 밖).
 *
 * 동기/배경: zone_size 가 2의 멱승이므로 lba >> zone_shift 로 빠른 인덱싱.
 */
static struct block_zone *
zone_block_get_zone_containing_lba(struct bdev_zone_block *bdev_node, uint64_t lba)
{
	size_t index = lba >> bdev_node->zone_shift;                             /* [한국어] log2(zone_size) shift — 정수 나눗셈 회피. */

	if (index >= bdev_node->num_zones) {                                     /* [한국어] 마지막 미완 zone (홀수 끝) 또는 OOB. */
		return NULL;
	}

	return &bdev_node->zones[index];
}

/*
 * [한국어]
 * zone_block_get_zone_by_slba - LBA 가 정확히 zone 의 시작 LBA(slba) 인지 확인하고 반환.
 *
 * @return: zone 포인터 (시작 LBA 일치 시) 또는 NULL.
 *
 * 동기/배경: ZNS management 명령은 반드시 zone slba 를 정확히 지정해야 함.
 */
static struct block_zone *
zone_block_get_zone_by_slba(struct bdev_zone_block *bdev_node, uint64_t start_lba)
{
	struct block_zone *zone = zone_block_get_zone_containing_lba(bdev_node, start_lba);

	if (zone && zone->zone_info.zone_id == start_lba) {                      /* [한국어] LBA 가 정확히 zone 의 head 인지 검증. */
		return zone;
	} else {
		return NULL;
	}
}

/*
 * [한국어]
 * zone_block_get_zone_info - GET_ZONE_INFO 처리 — 요청 범위의 zone 메타데이터를 사용자 버퍼에 복사.
 *
 * @bdev_node: vbdev.
 * @bdev_io: 사용자 I/O (u.zone_mgmt.buf 가 사용자 버퍼, num_zones 가 요청 개수).
 * @return: 0=성공, -EINVAL=zone 경계 mismatch.
 *
 * 동기/배경: zone 메타데이터는 메모리에 있으므로 단순 memcpy. lock 없이 읽으나
 * 짧은 inconsistency 는 무시 (read-mostly 패턴, host 가 다시 polling 함).
 */
static int
zone_block_get_zone_info(struct bdev_zone_block *bdev_node, struct spdk_bdev_io *bdev_io)
{
	struct block_zone *zone;
	struct spdk_bdev_zone_info *zone_info = bdev_io->u.zone_mgmt.buf;        /* [한국어] 사용자 출력 버퍼 (zone_info 배열). */
	uint64_t zone_id = bdev_io->u.zone_mgmt.zone_id;                         /* [한국어] 시작 LBA — 반드시 zone slba 여야 함. */
	size_t i;

	/* User can request info for more zones than exist, need to check both internal and user
	 * boundaries
	 */
	for (i = 0; i < bdev_io->u.zone_mgmt.num_zones; i++, zone_id += bdev_node->bdev.zone_size) { /* [한국어] zone_size 만큼씩 진행. */
		zone = zone_block_get_zone_by_slba(bdev_node, zone_id);          /* [한국어] 정확한 slba 매칭 검증. */
		if (!zone) {
			return -EINVAL;                                          /* [한국어] 범위 밖 또는 slba mismatch — 호출자가 NOMEM/FAILED 처리. */
		}
		memcpy(&zone_info[i], &zone->zone_info, sizeof(*zone_info));     /* [한국어] zone 상태 snapshot 복사 (lock-free read). */
	}

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);             /* [한국어] 동기 완료. */
	return 0;
}

/*
 * [한국어]
 * zone_block_open_zone - OPEN_ZONE management — zone 을 OPEN 상태로 전이.
 *
 * @zone: 대상 zone.
 * @bdev_io: 사용자 I/O.
 * @return: 0=성공 (완료까지), -EINVAL=invalid state.
 *
 * State machine 의 허용 전이:
 *   EMPTY  → OPEN  (첫 활성화)
 *   OPEN   → OPEN  (idempotent)
 *   CLOSED → OPEN  (재활성화)
 *   FULL/READONLY/OFFLINE → 거부 (-EINVAL).
 *
 * 동기화: zone spinlock 으로 보호 — 상태 검증과 변경이 atomic.
 */
static int
zone_block_open_zone(struct block_zone *zone, struct spdk_bdev_io *bdev_io)
{
	pthread_spin_lock(&zone->lock);                                          /* [한국어] critical section 진입 — 다른 reactor 의 동시 mgmt 차단. */

	switch (zone->zone_info.state) {
	case SPDK_BDEV_ZONE_STATE_EMPTY:                                         /* [한국어] 비어있는 zone — OPEN 으로 전이. */
	case SPDK_BDEV_ZONE_STATE_OPEN:                                          /* [한국어] 이미 열려 있어도 idempotent. */
	case SPDK_BDEV_ZONE_STATE_CLOSED:                                        /* [한국어] explicit close 되어있던 zone 도 다시 열기 가능. */
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_OPEN;
		pthread_spin_unlock(&zone->lock);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	default:                                                                 /* [한국어] FULL/READONLY/OFFLINE 등은 OPEN 불가. */
		pthread_spin_unlock(&zone->lock);
		return -EINVAL;
	}
}

static void
_zone_block_complete_unmap(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_request.
	 */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static int
zone_block_reset_zone(struct bdev_zone_block *bdev_node, struct zone_block_io_channel *ch,
		      struct block_zone *zone, struct spdk_bdev_io *bdev_io)
{
	pthread_spin_lock(&zone->lock);

	switch (zone->zone_info.state) {
	case SPDK_BDEV_ZONE_STATE_EMPTY:
		pthread_spin_unlock(&zone->lock);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	case SPDK_BDEV_ZONE_STATE_OPEN:
	case SPDK_BDEV_ZONE_STATE_FULL:
	case SPDK_BDEV_ZONE_STATE_CLOSED:
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_EMPTY;
		zone->zone_info.write_pointer = zone->zone_info.zone_id;
		pthread_spin_unlock(&zone->lock);

		/* The unmap isn't necessary, so if the base bdev doesn't support it, we're done */
		if (!spdk_bdev_io_type_supported(spdk_bdev_desc_get_bdev(bdev_node->base_desc),
						 SPDK_BDEV_IO_TYPE_UNMAP)) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
			return 0;
		}

		return spdk_bdev_unmap_blocks(bdev_node->base_desc, ch->base_ch,
					      zone->zone_info.zone_id, zone->zone_info.capacity,
					      _zone_block_complete_unmap, bdev_io);
	default:
		pthread_spin_unlock(&zone->lock);
		return -EINVAL;
	}
}

static int
zone_block_close_zone(struct block_zone *zone, struct spdk_bdev_io *bdev_io)
{
	pthread_spin_lock(&zone->lock);

	switch (zone->zone_info.state) {
	case SPDK_BDEV_ZONE_STATE_OPEN:
	case SPDK_BDEV_ZONE_STATE_CLOSED:
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_CLOSED;
		pthread_spin_unlock(&zone->lock);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	default:
		pthread_spin_unlock(&zone->lock);
		return -EINVAL;
	}
}

static int
zone_block_finish_zone(struct block_zone *zone, struct spdk_bdev_io *bdev_io)
{
	pthread_spin_lock(&zone->lock);

	zone->zone_info.write_pointer = zone->zone_info.zone_id + zone->zone_info.capacity;
	zone->zone_info.state = SPDK_BDEV_ZONE_STATE_FULL;

	pthread_spin_unlock(&zone->lock);
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	return 0;
}

static int
zone_block_zone_management(struct bdev_zone_block *bdev_node, struct zone_block_io_channel *ch,
			   struct spdk_bdev_io *bdev_io)
{
	struct block_zone *zone;

	zone = zone_block_get_zone_by_slba(bdev_node, bdev_io->u.zone_mgmt.zone_id);
	if (!zone) {
		return -EINVAL;
	}

	switch (bdev_io->u.zone_mgmt.zone_action) {
	case SPDK_BDEV_ZONE_RESET:
		return zone_block_reset_zone(bdev_node, ch, zone, bdev_io);
	case SPDK_BDEV_ZONE_OPEN:
		return zone_block_open_zone(zone, bdev_io);
	case SPDK_BDEV_ZONE_CLOSE:
		return zone_block_close_zone(zone, bdev_io);
	case SPDK_BDEV_ZONE_FINISH:
		return zone_block_finish_zone(zone, bdev_io);
	default:
		return -EINVAL;
	}
}

static void
_zone_block_complete_write(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	if (success && orig_io->type == SPDK_BDEV_IO_TYPE_ZONE_APPEND) {
		orig_io->u.bdev.offset_blocks = bdev_io->u.bdev.offset_blocks;
	}

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_request.
	 */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static int
zone_block_write(struct bdev_zone_block *bdev_node, struct zone_block_io_channel *ch,
		 struct spdk_bdev_io *bdev_io)
{
	struct block_zone *zone;
	uint64_t len = bdev_io->u.bdev.num_blocks;
	uint64_t lba = bdev_io->u.bdev.offset_blocks;
	uint64_t num_blocks_left, wp;
	int rc = 0;
	bool is_append = bdev_io->type == SPDK_BDEV_IO_TYPE_ZONE_APPEND;

	if (is_append) {
		zone = zone_block_get_zone_by_slba(bdev_node, lba);
	} else {
		zone = zone_block_get_zone_containing_lba(bdev_node, lba);
	}
	if (!zone) {
		SPDK_ERRLOG("Trying to write to invalid zone (lba 0x%" PRIx64 ")\n", lba);
		return -EINVAL;
	}

	pthread_spin_lock(&zone->lock);

	switch (zone->zone_info.state) {
	case SPDK_BDEV_ZONE_STATE_OPEN:
	case SPDK_BDEV_ZONE_STATE_EMPTY:
	case SPDK_BDEV_ZONE_STATE_CLOSED:
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_OPEN;
		break;
	default:
		SPDK_ERRLOG("Trying to write to zone in invalid state %u\n", zone->zone_info.state);
		rc = -EINVAL;
		goto write_fail;
	}

	wp = zone->zone_info.write_pointer;
	if (is_append) {
		lba = wp;
	} else {
		if (lba != wp) {
			SPDK_ERRLOG("Trying to write to zone with invalid address (lba 0x%" PRIx64 ", wp 0x%" PRIx64 ")\n",
				    lba, wp);
			rc = -EINVAL;
			goto write_fail;
		}
	}

	num_blocks_left = zone->zone_info.zone_id + zone->zone_info.capacity - wp;
	if (len > num_blocks_left) {
		SPDK_ERRLOG("Write exceeds zone capacity (lba 0x%" PRIx64 ", len 0x%" PRIx64 ", wp 0x%" PRIx64
			    ")\n", lba, len, wp);
		rc = -EINVAL;
		goto write_fail;
	}

	zone->zone_info.write_pointer += bdev_io->u.bdev.num_blocks;
	assert(zone->zone_info.write_pointer <= zone->zone_info.zone_id + zone->zone_info.capacity);
	if (zone->zone_info.write_pointer == zone->zone_info.zone_id + zone->zone_info.capacity) {
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_FULL;
	}
	pthread_spin_unlock(&zone->lock);

	rc = spdk_bdev_writev_blocks_with_md(bdev_node->base_desc, ch->base_ch,
					     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					     bdev_io->u.bdev.md_buf,
					     lba, bdev_io->u.bdev.num_blocks,
					     _zone_block_complete_write, bdev_io);

	return rc;

write_fail:
	pthread_spin_unlock(&zone->lock);
	return rc;
}

static void
_zone_block_complete_read(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_request.
	 */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static int
zone_block_read(struct bdev_zone_block *bdev_node, struct zone_block_io_channel *ch,
		struct spdk_bdev_io *bdev_io)
{
	struct block_zone *zone;
	uint64_t len = bdev_io->u.bdev.num_blocks;
	uint64_t lba = bdev_io->u.bdev.offset_blocks;
	int rc;

	zone = zone_block_get_zone_containing_lba(bdev_node, lba);
	if (!zone) {
		SPDK_ERRLOG("Trying to read from invalid zone (lba 0x%" PRIx64 ")\n", lba);
		return -EINVAL;
	}

	if ((lba + len) > (zone->zone_info.zone_id + zone->zone_info.capacity)) {
		SPDK_ERRLOG("Read exceeds zone capacity (lba 0x%" PRIx64 ", len 0x%" PRIx64 ")\n", lba, len);
		return -EINVAL;
	}

	rc = spdk_bdev_readv_blocks_with_md(bdev_node->base_desc, ch->base_ch,
					    bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					    bdev_io->u.bdev.md_buf,
					    lba, len,
					    _zone_block_complete_read, bdev_io);

	return rc;
}

static void
zone_block_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_zone_block *bdev_node = SPDK_CONTAINEROF(bdev_io->bdev, struct bdev_zone_block, bdev);
	struct zone_block_io_channel *dev_ch = spdk_io_channel_get_ctx(ch);
	int rc = 0;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_GET_ZONE_INFO:
		rc = zone_block_get_zone_info(bdev_node, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
		rc = zone_block_zone_management(bdev_node, dev_ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_ZONE_APPEND:
		rc = zone_block_write(bdev_node, dev_ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_READ:
		rc = zone_block_read(bdev_node, dev_ch, bdev_io);
		break;
	default:
		SPDK_ERRLOG("vbdev_block: unknown I/O type %u\n", bdev_io->type);
		rc = -ENOTSUP;
		break;
	}

	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_WARNLOG("ENOMEM, start to queue io for vbdev.\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static bool
zone_block_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_ZONE_APPEND:
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
zone_block_get_io_channel(void *ctx)
{
	struct bdev_zone_block *bdev_node = (struct bdev_zone_block *)ctx;

	return spdk_get_io_channel(bdev_node);
}

static int
zone_block_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_zone_block *bdev_node = (struct bdev_zone_block *)ctx;
	struct spdk_bdev *base_bdev = spdk_bdev_desc_get_bdev(bdev_node->base_desc);

	spdk_json_write_name(w, "zoned_block");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&bdev_node->bdev));
	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(base_bdev));
	spdk_json_write_named_uint64(w, "zone_capacity", bdev_node->zone_capacity);
	spdk_json_write_named_uint64(w, "optimal_open_zones", bdev_node->bdev.optimal_open_zones);
	spdk_json_write_object_end(w);

	return 0;
}

/* When we register our vbdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table zone_block_fn_table = {
	.destruct		= zone_block_destruct,
	.submit_request		= zone_block_submit_request,
	.io_type_supported	= zone_block_io_type_supported,
	.get_io_channel		= zone_block_get_io_channel,
	.dump_info_json		= zone_block_dump_info_json,
};

static void
zone_block_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct bdev_zone_block *bdev_node, *tmp;

	TAILQ_FOREACH_SAFE(bdev_node, &g_bdev_nodes, link, tmp) {
		if (bdev_find == spdk_bdev_desc_get_bdev(bdev_node->base_desc)) {
			spdk_bdev_unregister(&bdev_node->bdev, NULL, NULL);
		}
	}
}

static void
zone_block_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			      void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		zone_block_base_bdev_hotremove_cb(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

static int
_zone_block_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct zone_block_io_channel *bdev_ch = ctx_buf;
	struct bdev_zone_block *bdev_node = io_device;

	bdev_ch->base_ch = spdk_bdev_get_io_channel(bdev_node->base_desc);
	if (!bdev_ch->base_ch) {
		return -ENOMEM;
	}

	return 0;
}

static void
_zone_block_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct zone_block_io_channel *bdev_ch = ctx_buf;

	spdk_put_io_channel(bdev_ch->base_ch);
}

static int
zone_block_insert_name(const char *bdev_name, const char *vbdev_name, uint64_t zone_capacity,
		       uint64_t optimal_open_zones)
{
	struct bdev_zone_block_config *name;

	TAILQ_FOREACH(name, &g_bdev_configs, link) {
		if (strcmp(vbdev_name, name->vbdev_name) == 0) {
			SPDK_ERRLOG("block zoned bdev %s already exists\n", vbdev_name);
			return -EEXIST;
		}
		if (strcmp(bdev_name, name->bdev_name) == 0) {
			SPDK_ERRLOG("base bdev %s already claimed\n", bdev_name);
			return -EEXIST;
		}
	}

	name = calloc(1, sizeof(*name));
	if (!name) {
		SPDK_ERRLOG("could not allocate bdev_names\n");
		return -ENOMEM;
	}

	name->bdev_name = strdup(bdev_name);
	if (!name->bdev_name) {
		SPDK_ERRLOG("could not allocate name->bdev_name\n");
		free(name);
		return -ENOMEM;
	}

	name->vbdev_name = strdup(vbdev_name);
	if (!name->vbdev_name) {
		SPDK_ERRLOG("could not allocate name->vbdev_name\n");
		free(name->bdev_name);
		free(name);
		return -ENOMEM;
	}

	name->zone_capacity = zone_capacity;
	name->optimal_open_zones = optimal_open_zones;

	TAILQ_INSERT_TAIL(&g_bdev_configs, name, link);

	return 0;
}

static int
zone_block_init_zone_info(struct bdev_zone_block *bdev_node)
{
	size_t i;
	struct block_zone *zone;
	int rc = 0;

	for (i = 0; i < bdev_node->num_zones; i++) {
		zone = &bdev_node->zones[i];
		zone->zone_info.zone_id = bdev_node->bdev.zone_size * i;
		zone->zone_info.capacity = bdev_node->zone_capacity;
		zone->zone_info.write_pointer = zone->zone_info.zone_id + zone->zone_info.capacity;
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_FULL;
		zone->zone_info.type = SPDK_BDEV_ZONE_TYPE_SEQWR;
		if (pthread_spin_init(&zone->lock, PTHREAD_PROCESS_PRIVATE)) {
			SPDK_ERRLOG("pthread_spin_init() failed\n");
			rc = -ENOMEM;
			break;
		}
	}

	if (rc) {
		for (; i > 0; i--) {
			pthread_spin_destroy(&bdev_node->zones[i - 1].lock);
		}
	}

	return rc;
}

static int
zone_block_register(const char *base_bdev_name)
{
	struct spdk_bdev_desc *base_desc;
	struct spdk_bdev *base_bdev;
	struct bdev_zone_block_config *name, *tmp;
	struct bdev_zone_block *bdev_node;
	struct spdk_uuid ns_uuid;
	uint64_t zone_size;
	int rc = 0;

	spdk_uuid_parse(&ns_uuid, BDEV_ZONE_BLOCK_NAMESPACE_UUID);

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the bdev_node & bdev accordingly.
	 */
	TAILQ_FOREACH_SAFE(name, &g_bdev_configs, link, tmp) {
		if (strcmp(name->bdev_name, base_bdev_name) != 0) {
			continue;
		}

		rc = spdk_bdev_open_ext(base_bdev_name, true, zone_block_base_bdev_event_cb,
					NULL, &base_desc);
		if (rc == -ENODEV) {
			return -ENODEV;
		} else if (rc) {
			SPDK_ERRLOG("could not open bdev %s\n", base_bdev_name);
			goto free_config;
		}

		base_bdev = spdk_bdev_desc_get_bdev(base_desc);

		if (spdk_bdev_is_zoned(base_bdev)) {
			SPDK_ERRLOG("Base bdev %s is already a zoned bdev\n", base_bdev_name);
			rc = -EEXIST;
			goto zone_exist;
		}

		bdev_node = calloc(1, sizeof(struct bdev_zone_block));
		if (!bdev_node) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate bdev_node\n");
			goto zone_exist;
		}

		bdev_node->base_desc = base_desc;

		/* The base bdev that we're attaching to. */
		bdev_node->bdev.name = strdup(name->vbdev_name);
		if (!bdev_node->bdev.name) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate bdev_node name\n");
			goto strdup_failed;
		}

		zone_size = spdk_align64pow2(name->zone_capacity);
		if (zone_size == 0) {
			rc = -EINVAL;
			SPDK_ERRLOG("invalid zone size\n");
			goto roundup_failed;
		}

		bdev_node->zone_shift = spdk_u64log2(zone_size);
		bdev_node->num_zones = base_bdev->blockcnt / zone_size;

		bdev_node->zones = calloc(bdev_node->num_zones, sizeof(struct block_zone));
		if (!bdev_node->zones) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate zones\n");
			goto calloc_failed;
		}

		bdev_node->bdev.product_name = "zone_block";

		/* Copy some properties from the underlying base bdev. */
		bdev_node->bdev.write_cache = base_bdev->write_cache;
		bdev_node->bdev.required_alignment = base_bdev->required_alignment;
		bdev_node->bdev.optimal_io_boundary = base_bdev->optimal_io_boundary;

		bdev_node->bdev.blocklen = base_bdev->blocklen;
		bdev_node->bdev.blockcnt = bdev_node->num_zones * zone_size;

		if (bdev_node->num_zones * name->zone_capacity != base_bdev->blockcnt) {
			SPDK_DEBUGLOG(vbdev_zone_block,
				      "Lost %" PRIu64 " blocks due to zone capacity and base bdev size misalignment\n",
				      base_bdev->blockcnt - bdev_node->num_zones * name->zone_capacity);
		}

		bdev_node->bdev.write_unit_size = base_bdev->write_unit_size;

		bdev_node->bdev.md_interleave = base_bdev->md_interleave;
		bdev_node->bdev.md_len = base_bdev->md_len;
		bdev_node->bdev.dif_type = base_bdev->dif_type;
		bdev_node->bdev.dif_is_head_of_md = base_bdev->dif_is_head_of_md;
		bdev_node->bdev.dif_check_flags = base_bdev->dif_check_flags;
		bdev_node->bdev.dif_pi_format = base_bdev->dif_pi_format;

		bdev_node->bdev.zoned = true;
		bdev_node->bdev.ctxt = bdev_node;
		bdev_node->bdev.fn_table = &zone_block_fn_table;
		bdev_node->bdev.module = &bdev_zoned_if;

		bdev_node->bdev.numa = base_bdev->numa;

		/* Generate UUID based on namespace UUID + base bdev UUID. */
		rc = spdk_uuid_generate_sha1(&bdev_node->bdev.uuid, &ns_uuid,
					     (const char *)&base_bdev->uuid, sizeof(struct spdk_uuid));
		if (rc) {
			SPDK_ERRLOG("Unable to generate new UUID for zone block bdev\n");
			goto uuid_generation_failed;
		}

		/* bdev specific info */
		bdev_node->bdev.zone_size = zone_size;

		bdev_node->zone_capacity = name->zone_capacity;
		bdev_node->bdev.optimal_open_zones = name->optimal_open_zones;
		bdev_node->bdev.max_open_zones = 0;
		rc = zone_block_init_zone_info(bdev_node);
		if (rc) {
			SPDK_ERRLOG("could not init zone info\n");
			goto zone_info_failed;
		}

		TAILQ_INSERT_TAIL(&g_bdev_nodes, bdev_node, link);

		spdk_io_device_register(bdev_node, _zone_block_ch_create_cb, _zone_block_ch_destroy_cb,
					sizeof(struct zone_block_io_channel),
					name->vbdev_name);

		/* Save the thread where the base device is opened */
		bdev_node->thread = spdk_get_thread();

		rc = spdk_bdev_module_claim_bdev(base_bdev, base_desc, bdev_node->bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", base_bdev_name);
			goto claim_failed;
		}

		rc = spdk_bdev_register(&bdev_node->bdev);
		if (rc) {
			SPDK_ERRLOG("could not register zoned bdev\n");
			goto register_failed;
		}
	}

	return rc;

register_failed:
	spdk_bdev_module_release_bdev(&bdev_node->bdev);
claim_failed:
	TAILQ_REMOVE(&g_bdev_nodes, bdev_node, link);
	spdk_io_device_unregister(bdev_node, NULL);
zone_info_failed:
uuid_generation_failed:
	free(bdev_node->zones);
calloc_failed:
roundup_failed:
	free(bdev_node->bdev.name);
strdup_failed:
	free(bdev_node);
zone_exist:
	spdk_bdev_close(base_desc);
free_config:
	zone_block_remove_config(name);
	return rc;
}

int
vbdev_zone_block_create(const char *bdev_name, const char *vbdev_name, uint64_t zone_capacity,
			uint64_t optimal_open_zones)
{
	int rc = 0;

	if (zone_capacity == 0) {
		SPDK_ERRLOG("Zone capacity can't be 0\n");
		return -EINVAL;
	}

	if (optimal_open_zones == 0) {
		SPDK_ERRLOG("Optimal open zones can't be 0\n");
		return -EINVAL;
	}

	/* Insert the bdev into our global name list even if it doesn't exist yet,
	 * it may show up soon...
	 */
	rc = zone_block_insert_name(bdev_name, vbdev_name, zone_capacity, optimal_open_zones);
	if (rc) {
		return rc;
	}

	rc = zone_block_register(bdev_name);
	if (rc == -ENODEV) {
		/* This is not an error, even though the bdev is not present at this time it may
		 * still show up later.
		 */
		rc = 0;
	}
	return rc;
}

void
vbdev_zone_block_delete(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct bdev_zone_block_config *name_node;
	int rc;

	rc = spdk_bdev_unregister_by_name(name, &bdev_zoned_if, cb_fn, cb_arg);
	if (rc == 0) {
		TAILQ_FOREACH(name_node, &g_bdev_configs, link) {
			if (strcmp(name_node->vbdev_name, name) == 0) {
				zone_block_remove_config(name_node);
				break;
			}
		}
	} else {
		cb_fn(cb_arg, rc);
	}
}

static void
zone_block_examine(struct spdk_bdev *bdev)
{
	zone_block_register(bdev->name);

	spdk_bdev_module_examine_done(&bdev_zoned_if);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_zone_block)
