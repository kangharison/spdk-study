/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Zoned bdev(ZNS) 공개 API 구현 (bdev_zone.c)
 *
 * === 파일의 역할 ===
 * include/spdk/bdev_zone.h가 선언하는 zoned 디바이스 API의 호스트측
 * 구현 본체. ZNS(Zoned Namespace, NVMe 2.0 ZNS Command Set Spec) 또는 SMR HDD
 * 처럼 LBA 공간이 zone(고정 크기 sequential write 영역) 단위로 분할된 장치를
 * SPDK bdev 추상으로 노출하기 위한 두 종류의 연산을 처리한다:
 *   1) **속성 getter (in-process 즉시 응답)** — zone_size, num_zones,
 *      max_open/active_zones, optimal_open_zones, max_zone_append_size,
 *      offset_blocks → zslba 변환. struct spdk_bdev에 캐시된 필드를 단순 반환.
 *   2) **비동기 zone 명령 제출** — get_zone_info / zone_management(OPEN/CLOSE/
 *      FINISH/RESET/OFFLINE) / zone_append (buf/iov × md 유무). 각 호출은
 *      bdev_io를 channel에서 할당, 필요한 union 필드를 채운 뒤 bdev_io_submit()
 *      으로 bdev 모듈에 전달한다. 모듈은 이를 NVMe ZNS opcode(예: Zone Mgmt
 *      Send 0x79, Zone Mgmt Receive 0x7A, Zone Append 0x7D)로 변환해 장치에 발행.
 * 본 파일은 코드 수정 없이 한국어 주석만 추가/보강된 학습 사본이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 흐름:
 *   사용자 앱(예: bdevperf, FIO bdev 엔진, ZNS 인지 파일시스템)
 *     → spdk_bdev_zone_append/management/... (이 파일)
 *       → bdev_channel_get_io  (lib/bdev/bdev.c — io_pool에서 spdk_bdev_io 1개 할당)
 *       → bdev_io_init         (callback/engine 정보 채움)
 *       → bdev_io_submit       (lib/bdev/bdev.c — QoS/스플릿/모듈 디스패치)
 *         → bdev_module->fn_table->submit_request()
 *           → 예: module/bdev/nvme의 bdev_nvme_submit_request → NVMe Zone Append 커맨드
 *             → 디바이스 수행 후 CQE → bdev_io_complete() → 사용자 cb_fn(...)
 * 실행 컨텍스트: 모든 진입 함수는 **호출 spdk_thread(reactor)** 컨텍스트에서
 * 실행된다. spdk_io_channel은 thread-local이므로 다른 thread에서 받은 ch를
 * 그대로 쓰면 안 됨. bdev_io_submit이 모듈에 전달할 때도 동일 thread.
 *
 * === 타 모듈과의 연결 ===
 *  - **include/spdk/bdev_zone.h** — 이 파일이 구현하는 공개 API 선언 헤더(완료됨).
 *  - **lib/bdev/bdev.c** — `bdev_channel_get_io`, `bdev_io_init`, `bdev_io_submit`,
 *    `spdk_bdev_desc_get_bdev` 사용. 본 파일은 zone-specific 분기만 처리하고
 *    실제 큐잉/스레드 디스패치는 bdev.c가 담당.
 *  - **lib/bdev/bdev_internal.h** — 위 내부 헬퍼들의 선언 헤더. SPDK 외부에는
 *    노출되지 않는 lib/bdev 사적 인터페이스.
 *  - **bdev 모듈** (module/bdev/nvme, module/bdev/zone_block 등) — 본 파일이
 *    채운 `bdev_io->type = SPDK_BDEV_IO_TYPE_ZONE_*`을 보고 트랜스포트 명령으로
 *    번역. zone_block은 일반 bdev 위에 zone 시맨틱을 SW로 emulation.
 *  - **struct spdk_bdev** (include/spdk/bdev_module.h) — zone_size,
 *    max_zone_append_size, max_open_zones 등 zoned 속성 필드를 보유.
 *  - **spdk/likely.h, spdk/util.h** — spdk_likely, spdk_u64_is_pow2 매크로 사용.
 *
 * === 주요 함수/구조체 요약 ===
 *  - spdk_bdev_get_zone_size            : 장치 zone 크기(블록 수) 즉시 반환.
 *  - spdk_bdev_get_num_zones            : blockcnt / zone_size — 0 division 방지 가드.
 *  - spdk_bdev_get_zone_id              : LBA → zone start LBA(zslba) 변환. zone_size가
 *                                         2의 거듭제곱이면 비트 마스크로 fast-path.
 *  - spdk_bdev_get_max_zone_append_size : zone append 명령에 허용된 최대 전송 길이.
 *  - spdk_bdev_get_max_open_zones       : 동시에 OPEN 상태일 수 있는 zone 수 상한.
 *  - spdk_bdev_get_max_active_zones     : 동시에 ACTIVE(open+closed) 상태 zone 상한.
 *  - spdk_bdev_get_optimal_open_zones   : 권장 동시 open 개수(스루풋 최적).
 *  - spdk_bdev_get_zone_info            : 비동기 GET_ZONE_INFO — zone 정보 배열 채움.
 *  - spdk_bdev_zone_management          : OPEN/CLOSE/FINISH/RESET/OFFLINE 등 상태 변경.
 *  - zone_bdev_append_with_md (static)  : append 4종 공통 구현. iov 1개·md 옵션.
 *  - spdk_bdev_zone_append              : md 없는 단일 buffer append 래퍼.
 *  - spdk_bdev_zone_append_with_md      : md 있는 단일 buffer append 래퍼.
 *  - spdk_bdev_zone_appendv_with_md     : md 옵션 있는 iov vector append.
 *  - spdk_bdev_zone_appendv             : md 없는 vector append 래퍼.
 *  - spdk_bdev_io_get_append_location   : 완료된 append의 실제 할당 LBA 조회.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 C 헤더 묶음 (stdint, stdbool, string 등). uint64_t/size_t 등 사용. */

#include "spdk/bdev_zone.h"
/* [한국어] 본 파일이 구현하는 공개 API 선언 — spdk_bdev_zone_* 함수 시그니처. */
#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈/내부 자료구조 선언 — struct spdk_bdev, spdk_bdev_io,
 *  SPDK_BDEV_IO_TYPE_*, spdk_bdev_desc_get_bdev 등. */
#include "spdk/likely.h"
/* [한국어] spdk_likely / spdk_unlikely 분기 예측 매크로. zone_size가 보통
 *  2의 거듭제곱이라는 가정 하에 fast-path를 우선 컴파일하기 위함. */

#include "bdev_internal.h"
/* [한국어] lib/bdev 내부 사적 헤더 — bdev_channel_get_io, bdev_io_init,
 *  bdev_io_submit, struct spdk_bdev_channel 정의. SPDK 외부 사용자에게는 비공개. */

/*
 * [한국어]
 * spdk_bdev_get_zone_size - 디바이스 zone 크기(블록 단위) 즉시 반환.
 *
 * @bdev:   대상 zoned bdev.
 * @return: zone 1개를 구성하는 블록 수. 비-zoned bdev에서는 0이 들어 있을 수 있다.
 *
 * 단순 getter. zone_size는 bdev_register 시 zone 모듈/드라이버가 한 번 채워두는
 * 정적 속성이므로 락이 필요 없다. NVMe ZNS의 ZSZE(Zone Size) Identify NS 필드에
 * 대응한다. SMR HDD라면 ZBC/ZAC의 ZONE SIZE에 대응.
 *
 * 호출 컨텍스트: 어느 spdk_thread에서든 안전. read-only 즉시 반환.
 *
 * 호출 체인:
 *   사용자 앱 / module/bdev/* → [이 함수]
 */
uint64_t
spdk_bdev_get_zone_size(const struct spdk_bdev *bdev)
{
	return bdev->zone_size;
	/* [한국어] bdev 등록 시 모듈이 채워둔 zone_size 그대로 반환. read-only 필드라 락 불필요. */
}

/*
 * [한국어]
 * spdk_bdev_get_num_zones - 디바이스의 총 zone 개수 계산해서 반환.
 *
 * @bdev:   대상 zoned bdev.
 * @return: blockcnt / zone_size. zone_size==0(비-zoned)이면 0.
 *
 * NVMe ZNS는 namespace 용량이 zone_size의 정수배가 되도록 포맷되므로 정수 나눗셈
 * 결과가 정확하다. zone_size==0인 경우 0 division을 막기 위해 삼항으로 가드한다.
 *
 * 호출 체인:
 *   사용자 / FTL/zone_block 모듈 → [이 함수]
 */
uint64_t
spdk_bdev_get_num_zones(const struct spdk_bdev *bdev)
{
	return bdev->zone_size ? bdev->blockcnt / bdev->zone_size : 0;
	/* [한국어] zone_size가 0이면 비-zoned이거나 미설정 — 0 division 방지를 위해
	 *  삼항으로 가드. 양수면 단순 정수 나눗셈으로 zone 개수 산출. */
}

/*
 * [한국어]
 * spdk_bdev_get_zone_id - 임의 LBA에 대해 그 LBA가 속한 zone의 시작 LBA(zslba) 반환.
 *
 * @bdev:          대상 zoned bdev.
 * @offset_blocks: 변환 대상 LBA (블록 인덱스).
 * @return:        해당 zone의 시작 LBA (offset_blocks가 속한 zone의 가장 낮은 LBA).
 *
 * NVMe ZNS Zone Mgmt Receive 등은 zslba(zone-aligned)를 인자로 요구한다. 이 함수는
 * 사용자 LBA를 그에 맞게 정렬해주는 헬퍼. zone_size가 2의 거듭제곱이면(대다수 ZNS
 * 디바이스 — 8MiB/16MiB 등) AND 마스크로 1 cycle에 처리, 아니면 정수 나눗셈
 * fall-back 경로를 사용한다. spdk_likely로 fast-path를 hot으로 지정.
 *
 * 호출 체인:
 *   사용자가 LBA로 write 위치 결정 → [이 함수] → spdk_bdev_zone_management(zslba)
 */
uint64_t
spdk_bdev_get_zone_id(const struct spdk_bdev *bdev, uint64_t offset_blocks)
{
	uint64_t zslba;
	/* [한국어] zone start LBA. 함수 종료 시 호출자에게 반환되는 결과 변수. */

	if (spdk_likely(spdk_u64_is_pow2(bdev->zone_size))) {
		/* [한국어] fast-path: zone_size가 2의 거듭제곱(대부분의 ZNS는 그러함).
		 *  비트 마스크로 정렬을 1 cycle에 처리. spdk_likely로 컴파일러에 분기 예측 힌트. */
		uint64_t zone_mask = bdev->zone_size - 1;
		/* [한국어] 예: zone_size=2^N이면 zone_mask는 하위 N비트가 1인 값.
		 *  offset_blocks의 하위 N비트 = zone 내부 오프셋. */
		zslba = offset_blocks & ~zone_mask;
		/* [한국어] 하위 N비트를 0으로 클리어 = zone 시작 LBA(정렬된 값). */
	} else {
		/* integer division */
		/* [한국어] slow-path: zone_size가 2의 거듭제곱이 아니면 비트 트릭 사용 불가.
		 *  정수 나눗셈으로 zone 인덱스를 구하고 다시 곱해 zone 시작 LBA 산출. */
		zslba = (offset_blocks / bdev->zone_size) * bdev->zone_size;
		/* [한국어] (offset / size) → zone 인덱스 (소수 버림), * size → zone 시작 LBA. */
	}

	return zslba;
	/* [한국어] 정렬된 zone start LBA를 호출자에게 반환. */
}

/*
 * [한국어]
 * spdk_bdev_get_max_zone_append_size - 한 번의 Zone Append 명령으로 전송 가능한
 * 최대 데이터 길이(블록 수) 반환.
 *
 * @bdev:   대상 zoned bdev.
 * @return: ZASL(Zone Append Size Limit, NVMe ZNS Identify NS 필드)에서 도출된 블록 수.
 *
 * Zone Append는 한 번에 hardware가 처리할 수 있는 최대 데이터 양에 제한이 있어
 * (ZASL), 사용자가 큰 I/O를 split해야 하는지 판단할 때 이 값을 참조한다.
 * bdev/bdev.c의 split 로직과 협력한다.
 */
uint32_t
spdk_bdev_get_max_zone_append_size(const struct spdk_bdev *bdev)
{
	return bdev->max_zone_append_size;
	/* [한국어] 모듈이 Identify Namespace ZASL을 보고 채워둔 정적 필드. read-only. */
}

/*
 * [한국어]
 * spdk_bdev_get_max_open_zones - 동시에 OPEN(IMP_OPEN+EXP_OPEN) 상태일 수 있는
 * zone 수의 하드웨어 상한 반환.
 *
 * @bdev:   대상 zoned bdev.
 * @return: 0이면 무제한, 양수면 디바이스가 강제하는 동시 open 상한 (MAR/MOR).
 *
 * 사용자가 open 상한 초과 시 디바이스는 Too Many Open Zones 에러를 반환하므로
 * 애플리케이션은 이 값을 보고 explicit close/finish를 trigger해야 한다.
 */
uint32_t
spdk_bdev_get_max_open_zones(const struct spdk_bdev *bdev)
{
	return bdev->max_open_zones;
	/* [한국어] NVMe ZNS Identify NS의 MOR(Maximum Open Resources) 대응 필드 반환. */
}

/*
 * [한국어]
 * spdk_bdev_get_max_active_zones - 동시에 ACTIVE(OPEN + CLOSED) 상태일 수 있는
 * zone 수의 하드웨어 상한 반환.
 *
 * @bdev:   대상 zoned bdev.
 * @return: 0이면 무제한, 양수면 동시 active 상한 (MAR).
 *
 * ACTIVE = OPEN ∪ CLOSED. CLOSED zone도 디바이스 내부 자원을 점유하므로 별도 상한
 * 이 있다. 초과 시 디바이스는 Too Many Active Zones 에러를 반환.
 */
uint32_t
spdk_bdev_get_max_active_zones(const struct spdk_bdev *bdev)
{
	return bdev->max_active_zones;
	/* [한국어] NVMe ZNS Identify NS의 MAR(Maximum Active Resources) 대응. */
}

/*
 * [한국어]
 * spdk_bdev_get_optimal_open_zones - 디바이스가 권장하는 최적 동시 open zone 수.
 *
 * @bdev:   대상 zoned bdev.
 * @return: 권장 동시 open 개수. 이 값을 따르면 스루풋 측면에서 최적.
 *
 * MOR(상한)와 별개로 디바이스 벤더가 워크로드 평가에 기반해 권장하는 값.
 * NVMe ZNS의 Optimal Open/Active Zones 필드 (ZNS Spec).
 */
uint32_t
spdk_bdev_get_optimal_open_zones(const struct spdk_bdev *bdev)
{
	return bdev->optimal_open_zones;
	/* [한국어] 디바이스 벤더 권장 동시 open 수 — 사용자가 throughput 최적화 시 참조. */
}

/*
 * [한국어]
 * spdk_bdev_get_zone_info - 비동기적으로 여러 zone의 상태/속성 정보를 가져오는 명령 제출.
 *
 * @desc:      open 상태인 bdev descriptor (소유자가 spdk_bdev_open으로 획득).
 * @ch:        호출 thread의 io_channel (해당 thread의 bdev I/O 큐에 enqueue됨).
 * @zone_id:   조회 시작 zone의 zslba (zone-aligned LBA. spdk_bdev_get_zone_id로 정규화).
 * @num_zones: 받아올 zone 개수. info 배열은 최소 num_zones개의 spdk_bdev_zone_info를 가져야 함.
 * @info:      결과를 채울 사용자 버퍼. 완료 시점까지 유효해야 한다 (호출자 소유).
 * @cb:        완료 콜백. 동일 io_channel 소속 thread에서 호출됨.
 * @cb_arg:    cb의 첫 번째 인자로 전달.
 * @return:    0=제출 성공(완료는 cb로), -ENOMEM=channel io_pool 고갈로 bdev_io 할당 실패.
 *
 * NVMe ZNS의 Zone Mgmt Receive(opcode 0x7A, Report Zones)에 대응. 모듈 레벨에서
 * num_zones만큼의 zone descriptor 배열을 받아 spdk_bdev_zone_info 구조로 변환해
 * 사용자 버퍼에 채운다.
 *
 * 실행 컨텍스트: 호출 spdk_thread에서 동기적으로 bdev_io 1개를 io_pool에서 할당,
 * 모듈로 dispatch까지 완료. 실제 디바이스 I/O 완료는 모듈/poller가 비동기 처리하고
 * cb를 본 thread에서 호출.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → bdev_channel_get_io → bdev_io_init → bdev_io_submit →
 *     bdev_module->fn_table->submit_request → 디바이스 → 완료 → cb_fn(cb_arg, success, bdev_io)
 */
int
spdk_bdev_get_zone_info(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t zone_id, size_t num_zones, struct spdk_bdev_zone_info *info,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	/* [한국어] desc(opaque)에서 실제 bdev 객체 추출. desc가 어느 bdev에 묶여 있는지
	 *  내부 매핑을 통해 가져옴 — 모듈 dispatch 시 bdev->fn_table을 사용. */
	struct spdk_bdev_io *bdev_io;
	/* [한국어] 곧 io_pool에서 할당받을 bdev I/O 요청 객체. zone 명령용 union.zone_mgmt 사용. */
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	/* [한국어] spdk_io_channel 헤더 직후 위치한 module 별 ctx 추출.
	 *  bdev의 경우 spdk_bdev_channel — io_pool, qos, stat, mgmt_ch 등을 보유. */

	bdev_io = bdev_channel_get_io(channel);
	/* [한국어] channel-local io_pool에서 spdk_bdev_io 1개를 lock-free로 pop.
	 *  poll-mode·thread-affinity 가정 하에 락 없이 안전. 풀 고갈 시 NULL 반환. */
	if (!bdev_io) {
		return -ENOMEM;
		/* [한국어] io_pool 고갈 — 사용자에게 즉시 -ENOMEM 반환. 호출자는 후퇴/재시도 결정.
		 *  pool 크기는 spdk_bdev_opts.bdev_io_pool_size로 조절 가능. */
	}

	bdev_io->internal.ch = channel;
	/* [한국어] bdev_io를 발급한 channel 기록 — 완료 시 같은 channel로 회수해야 함
	 *  (thread-affinity 위반 방지 + io_pool 일관성 유지). */
	bdev_io->internal.desc = desc;
	/* [한국어] desc 보관 — 완료 처리 시 통계/이벤트 콜백 라우팅에 사용. */
	bdev_io->type = SPDK_BDEV_IO_TYPE_GET_ZONE_INFO;
	/* [한국어] I/O 종류 태깅 → bdev_io_submit이 모듈 fn_table[type]을 디스패치할 때 사용.
	 *  module/bdev/nvme는 이를 NVMe Zone Mgmt Receive(Report Zones, action=0x0)로 번역. */
	bdev_io->u.zone_mgmt.zone_id = zone_id;
	/* [한국어] 조회 시작 zone의 zslba. 모듈은 이 값을 NVMe SLBA(Starting LBA)로 그대로 사용. */
	bdev_io->u.zone_mgmt.num_zones = num_zones;
	/* [한국어] 조회 zone 개수. 디바이스가 한 번에 반환할 수 있는 최대치를 초과하면
	 *  bdev 모듈이 split하거나 사용자 책임으로 분할 호출 필요. */
	bdev_io->u.zone_mgmt.buf = info;
	/* [한국어] 결과 buffer 포인터. 모듈은 디바이스에서 받은 zone descriptor를 파싱해
	 *  spdk_bdev_zone_info 형식으로 채워 넣는다. 완료 전까지 호출자가 유효 보장. */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	/* [한국어] bdev_io 공통 초기화 — bdev pointer, callback fn/arg 저장,
	 *  status=PENDING, internal.f.* flag 초기화 등. lib/bdev/bdev.c 정의. */

	bdev_io_submit(bdev_io);
	/* [한국어] 모듈 dispatch 진입점. QoS/스레드 라우팅/스플릿 후 모듈
	 *  fn_table->submit_request 호출. 본 함수는 비동기 — 완료는 cb로. */
	return 0;
	/* [한국어] 제출 성공. 완료/에러는 cb_fn으로 전달됨. */
}

/*
 * [한국어]
 * spdk_bdev_zone_management - 단일 zone에 대해 상태 변경 명령(OPEN/CLOSE/FINISH/RESET/OFFLINE)
 * 을 비동기 제출.
 *
 * @desc:    bdev descriptor.
 * @ch:      호출 thread io_channel.
 * @zone_id: 대상 zone의 zslba.
 * @action:  enum spdk_bdev_zone_action — CLOSE(0)/FINISH(1)/OPEN(2)/RESET(3)/OFFLINE(4).
 * @cb:      완료 콜백.
 * @cb_arg:  cb 첫 인자.
 * @return:  0=제출 성공, -ENOMEM=io_pool 고갈.
 *
 * NVMe ZNS Zone Mgmt Send(opcode 0x79)에 대응. action에 따라 zone 상태머신을 전이시킴:
 *   - OPEN  : EMPTY/CLOSED → EXP_OPEN
 *   - CLOSE : OPEN → CLOSED (open resource 회수)
 *   - FINISH: OPEN/EMPTY/CLOSED → FULL (남은 공간 강제 종료)
 *   - RESET : 모든 상태 → EMPTY (write pointer 0으로, 데이터 무효화)
 *   - OFFLINE: READ_ONLY → OFFLINE (사용 불가 영구 마킹)
 *
 * num_zones=1로 고정 — 한 번에 한 zone만 다룬다 (Select All=All Zones는 별도 API에서 미지원).
 *
 * 호출 체인:
 *   사용자/FTL → [이 함수] → bdev_io_submit → bdev_module → NVMe Zone Mgmt Send → 완료 cb
 */
int
spdk_bdev_zone_management(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  uint64_t zone_id, enum spdk_bdev_zone_action action,
			  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	/* [한국어] desc → bdev 추출. 모듈 fn_table 라우팅에 사용. */
	struct spdk_bdev_io *bdev_io;
	/* [한국어] zone management용 bdev_io. union.zone_mgmt 필드 사용. */
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	/* [한국어] thread-local channel context — bdev_io 할당과 dispatch 라우팅 주체. */

	bdev_io = bdev_channel_get_io(channel);
	/* [한국어] io_pool에서 1개 할당. lock-free pop. */
	if (!bdev_io) {
		return -ENOMEM;
		/* [한국어] pool 고갈. 사용자가 backoff/재시도 결정. */
	}

	bdev_io->internal.ch = channel;
	/* [한국어] 발급 channel 기록 — 완료 시 회수처. */
	bdev_io->internal.desc = desc;
	/* [한국어] desc 기록 — 통계/이벤트 라우팅. */
	bdev_io->type = SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT;
	/* [한국어] type tagging — module/bdev/nvme는 이를 Zone Mgmt Send opcode 0x79로 번역. */
	bdev_io->u.zone_mgmt.zone_action = action;
	/* [한국어] 어떤 transition을 수행할지: OPEN/CLOSE/FINISH/RESET/OFFLINE.
	 *  모듈이 NVMe Zone Send Action 필드(ZSA)로 번역 — 1=Close, 2=Finish, 3=Open,
	 *  4=Reset, 5=Offline (NVMe ZNS Spec 4.x). */
	bdev_io->u.zone_mgmt.zone_id = zone_id;
	/* [한국어] 대상 zone의 zslba — NVMe SLBA로 사용. zone-aligned가 아니면 디바이스가
	 *  Invalid Field 에러로 거부. */
	bdev_io->u.zone_mgmt.num_zones = 1;
	/* [한국어] 한 번에 1개 zone만. Select All=1 모드는 본 API로는 노출 안 함 (확장 시 추가 가능). */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	/* [한국어] 공통 초기화 — bdev/cb/cb_arg/status=PENDING 등. */

	bdev_io_submit(bdev_io);
	/* [한국어] 모듈로 dispatch. */
	return 0;
	/* [한국어] 제출 성공. */
}

/*
 * [한국어]
 * zone_bdev_append_with_md - 단일 buffer + 선택적 metadata로 Zone Append 명령을
 * 비동기 제출하는 static 헬퍼. spdk_bdev_zone_append / spdk_bdev_zone_append_with_md의 공통 구현.
 *
 * @desc:       bdev descriptor.
 * @ch:         호출 thread io_channel.
 * @buf:        데이터 buffer (단일 contiguous, num_blocks * blocklen 바이트).
 * @md_buf:     별도 metadata buffer (NULL이면 metadata 없거나 interleaved).
 * @zone_id:    대상 zone의 zslba — Zone Append는 이 zone의 write pointer로 자동 할당.
 * @num_blocks: 추가 블록 수.
 * @cb:         완료 콜백.
 * @cb_arg:     cb 첫 인자.
 * @return:     0=성공, -ENOMEM=io_pool 고갈.
 *
 * Zone Append(NVMe ZNS opcode 0x7D)의 핵심 의미:
 *   - 호스트가 LBA를 지정하지 않고 zone start만 지정 → 디바이스가 현재 wp에서 자동 추가
 *   - 따라서 다수 outstanding append가 wp race 없이 병렬 처리 가능
 *   - 완료 시 CQE의 ALBA(Allocated LBA) 필드로 실제 할당된 LBA 반환 →
 *     spdk_bdev_io_get_append_location()으로 조회
 *   - 한 번에 max_zone_append_size 초과 불가 (사용자가 split 책임)
 *
 * 호출 체인:
 *   spdk_bdev_zone_append / spdk_bdev_zone_append_with_md → [이 함수] → bdev_io_submit
 *     → bdev_module → NVMe Zone Append cmd → 완료 → cb_fn
 */
static int
zone_bdev_append_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			 void *buf, void *md_buf, uint64_t zone_id, uint64_t num_blocks,
			 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	/* [한국어] desc → bdev. blocklen 계산과 모듈 dispatch에 필요. */
	struct spdk_bdev_io *bdev_io;
	/* [한국어] zone append용 bdev_io. union.bdev(read/write와 같은 union) 사용. */
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	/* [한국어] thread-local channel ctx — io_pool 출처. */

	bdev_io = bdev_channel_get_io(channel);
	/* [한국어] bdev_io 1개 할당. lock-free pop. */
	if (!bdev_io) {
		return -ENOMEM;
		/* [한국어] pool 고갈 시 즉시 -ENOMEM. */
	}

	bdev_io->internal.ch = channel;
	/* [한국어] 발급 channel 기록 — 완료 시 회수. */
	bdev_io->internal.desc = desc;
	/* [한국어] desc 기록. */
	bdev_io->type = SPDK_BDEV_IO_TYPE_ZONE_APPEND;
	/* [한국어] type tagging → 모듈은 NVMe opcode 0x7D(Zone Append)로 번역. */
	bdev_io->u.bdev.iovs = &bdev_io->iov;
	/* [한국어] iov 배열 포인터를 bdev_io 내장 1-원소 iov 배열(bdev_io->iov)로 설정.
	 *  단일 buffer 케이스이므로 별도 iov 할당 없이 inline으로 처리. */
	bdev_io->u.bdev.iovs[0].iov_base = buf;
	/* [한국어] 단일 iov의 데이터 시작 주소 = 사용자 buf. */
	bdev_io->u.bdev.iovs[0].iov_len = num_blocks * bdev->blocklen;
	/* [한국어] 단일 iov 길이 = 블록 수 × 블록 크기 = 전체 데이터 바이트.
	 *  blocklen은 보통 512 또는 4096 (LBA Format에 따라). */
	bdev_io->u.bdev.iovcnt = 1;
	/* [한국어] iov 개수 = 1 (vector가 아닌 단일 buffer 케이스). */
	bdev_io->u.bdev.md_buf = md_buf;
	/* [한국어] 별도 metadata 버퍼. NULL이면 (a) metadata 없음 또는 (b) data와 interleaved.
	 *  bdev->md_interleave 플래그와 md_len으로 모듈이 형태 결정. */
	bdev_io->u.bdev.num_blocks = num_blocks;
	/* [한국어] 추가 블록 수 — NVMe Zone Append의 NLB(Number of Logical Blocks). */
	bdev_io->u.bdev.offset_blocks = zone_id;
	/* [한국어] **중요**: 일반 read/write와 달리 여기엔 zone start LBA(zslba)를 넣음.
	 *  완료 후 동일 필드를 모듈이 ALBA(실제 할당 LBA)로 덮어 씀 →
	 *  spdk_bdev_io_get_append_location()이 그 값을 읽어 사용자에게 노출. */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	/* [한국어] 공통 초기화 — bdev/cb/cb_arg/status=PENDING. */

	bdev_io_submit(bdev_io);
	/* [한국어] 모듈로 dispatch. 비동기 — 완료는 cb로. */
	return 0;
	/* [한국어] 제출 성공. */
}

/*
 * [한국어]
 * spdk_bdev_zone_append - 메타데이터 없는 단일 buffer Zone Append 공개 API.
 *
 * @desc/ch/buf/start_lba/num_blocks/cb/cb_arg: 위 zone_bdev_append_with_md와 동일.
 *
 * md_buf=NULL로 zone_bdev_append_with_md를 호출하는 thin 래퍼.
 *
 * 호출 체인:
 *   사용자 → [이 함수] → zone_bdev_append_with_md → bdev_io_submit
 */
int
spdk_bdev_zone_append(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      void *buf, uint64_t start_lba, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return zone_bdev_append_with_md(desc, ch, buf, NULL, start_lba, num_blocks,
					cb, cb_arg);
	/* [한국어] md_buf=NULL을 명시적으로 전달 — metadata 없이 데이터만 추가하는 경로. */
}

/*
 * [한국어]
 * spdk_bdev_zone_append_with_md - 메타데이터 buffer를 별도로 지정하는 단일 buffer Zone Append.
 *
 * @md: 별도 metadata buffer (md_interleave=false인 디바이스에서 사용).
 *
 * spdk_bdev_zone_append와 거의 동일하지만 md_buf를 사용자 지정값으로 전달한다.
 * 디바이스가 separate metadata format이거나 사용자가 PI(Protection Information)
 * 영역을 직접 채울 때 사용.
 */
int
spdk_bdev_zone_append_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      void *buf, void *md, uint64_t start_lba, uint64_t num_blocks,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return zone_bdev_append_with_md(desc, ch, buf, md, start_lba, num_blocks,
					cb, cb_arg);
	/* [한국어] md를 그대로 전달 — separate metadata 모드. */
}

/*
 * [한국어]
 * spdk_bdev_zone_appendv_with_md - iovec vector + 선택적 metadata로 Zone Append 제출.
 *
 * @iov:        scatter-gather iovec 배열 (사용자 소유, 완료 전까지 유효).
 * @iovcnt:    iov 원소 개수.
 * @md_buf:    metadata buffer (NULL 가능).
 * @zone_id:   대상 zone의 zslba.
 * @num_blocks: 총 추가 블록 수 (모든 iov 길이 합 / blocklen 과 일치해야 함).
 * @cb/cb_arg: 완료 콜백.
 * @return:    0=성공, -ENOMEM=io_pool 고갈.
 *
 * 단일 buffer 버전과 달리 사용자 iovec를 그대로 보존. SG-DMA를 지원하는 NVMe
 * 디바이스에서 zero-copy로 다중 buffer를 한 번의 명령으로 전송 가능 (PRP/SGL 활용).
 */
int
spdk_bdev_zone_appendv_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			       struct iovec *iov, int iovcnt, void *md_buf, uint64_t zone_id,
			       uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
			       void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	/* [한국어] desc → bdev. */
	struct spdk_bdev_io *bdev_io;
	/* [한국어] zone append vector용 bdev_io. */
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	/* [한국어] thread-local channel ctx. */

	bdev_io = bdev_channel_get_io(channel);
	/* [한국어] io_pool에서 1개 할당. */
	if (!bdev_io) {
		return -ENOMEM;
		/* [한국어] pool 고갈. */
	}

	bdev_io->internal.ch = channel;
	/* [한국어] 발급 channel 기록. */
	bdev_io->internal.desc = desc;
	/* [한국어] desc 기록. */
	bdev_io->type = SPDK_BDEV_IO_TYPE_ZONE_APPEND;
	/* [한국어] Zone Append type. */
	bdev_io->u.bdev.iovs = iov;
	/* [한국어] **사용자 iov 배열을 직접 가리킴** — 단일 buffer 버전과 달리 inline 1-원소 배열을
	 *  쓰지 않는다. 사용자 iov는 완료 전까지 유효해야 하며 본 함수가 소유권을 가져가지 않음. */
	bdev_io->u.bdev.iovcnt = iovcnt;
	/* [한국어] iov 원소 개수. */
	bdev_io->u.bdev.md_buf = md_buf;
	/* [한국어] separate metadata buffer (NULL 가능). */
	bdev_io->u.bdev.num_blocks = num_blocks;
	/* [한국어] 총 블록 수. iov 길이 합과 일치해야 함 (책임은 호출자). */
	bdev_io->u.bdev.offset_blocks = zone_id;
	/* [한국어] zslba — append 제출 시점에는 zone 시작, 완료 후 ALBA로 덮어써짐. */
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	/* [한국어] 공통 초기화. */

	bdev_io_submit(bdev_io);
	/* [한국어] 모듈로 dispatch. */
	return 0;
	/* [한국어] 제출 성공. */
}

/*
 * [한국어]
 * spdk_bdev_zone_appendv - 메타데이터 없는 vector Zone Append 래퍼.
 *
 * md_buf=NULL로 spdk_bdev_zone_appendv_with_md 호출.
 */
int
spdk_bdev_zone_appendv(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iovs, int iovcnt, uint64_t zone_id, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return spdk_bdev_zone_appendv_with_md(desc, ch, iovs, iovcnt, NULL, zone_id, num_blocks,
					      cb, cb_arg);
	/* [한국어] md_buf=NULL — metadata 없는 vector append 경로. */
}

/*
 * [한국어]
 * spdk_bdev_io_get_append_location - 완료된 Zone Append의 실제 할당 LBA(ALBA) 조회.
 *
 * @bdev_io: 완료 콜백에서 받은 (또는 사용자가 추적해 둔) bdev_io 포인터.
 * @return:  디바이스가 wp 기반으로 할당한 실제 LBA. 호출자는 이를 사용해
 *           추가 데이터의 위치를 알 수 있다.
 *
 * Zone Append는 호스트가 LBA를 지정하지 않으므로, 완료 후에 디바이스가 ALBA를
 * CQE의 Command Specific 필드(DW0/1)로 반환한다. 모듈(bdev_nvme)이 이를 파싱해
 * bdev_io->u.bdev.offset_blocks를 zone start에서 ALBA로 덮어 쓴 뒤 cb_fn을 호출한다.
 * 본 함수는 그 필드를 그대로 노출한다 — cb 내에서만 호출해야 의미 있음 (제출 직후
 * 상태에서는 zone start만 들어 있음).
 *
 * 호출 컨텍스트: cb_fn(여전히 같은 spdk_thread)에서 호출. 완료 후 spdk_bdev_free_io 전.
 */
uint64_t
spdk_bdev_io_get_append_location(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->u.bdev.offset_blocks;
	/* [한국어] 완료 시 모듈이 ALBA로 덮어쓴 offset_blocks를 그대로 반환.
	 *  cb 이전에 호출하면 zone start LBA가 반환되어 의미가 없으므로 cb 내에서 사용. */
}
