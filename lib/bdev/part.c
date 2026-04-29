/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * Common code for partition-like virtual bdevs.
 */

/*
 * [한국어 설명] partition 형식 vbdev 공통 코어 (part.c)
 *
 * === 파일의 역할 ===
 * 한 개의 base bdev를 (offset, length) 단위로 잘라 여러 개의 가상 bdev(partition)를
 * 만들어 노출하는 공통 코드. GPT 파티션 테이블 분석기(`module/bdev/gpt`),
 * SPDK Logical Volume Manager(LVOL)의 일부, blockdev split 등 "한 base 블록 디바이스 위에
 * 다수의 sub-bdev를 만들고 싶다"는 모든 vbdev 모듈이 본 코드를 라이브러리처럼 활용한다.
 *
 * 본 파일이 제공하는 핵심 기능:
 *   1) **base bdev 관리(part_base)**: 단일 base bdev를 한 번 open/claim하고, 그 위에
 *      여러 partition이 공유하도록 reference count로 추적. base가 hot-remove되면 모든
 *      partition을 unregister.
 *   2) **partition 생성(part_construct)**: spdk_bdev 구조체를 채우고 io_device 등록 +
 *      spdk_bdev_register로 SPDK bdev 시스템에 노출. UUID는 사용자가 지정하지 않으면
 *      네임스페이스 UUID + base UUID + (offset, num_blocks)로 SHA-1 결정론적으로 생성.
 *   3) **I/O remap(submit_request)**: partition으로 들어온 모든 bdev_io의 offset_blocks에
 *      partition의 base offset을 더해 base bdev에 다시 제출. R/W/Compare/Copy/UNMAP/
 *      WRITE_ZEROES/Flush/Reset/ZCopy/Compare-and-Write/Write-Uncorrectable 등 거의 모든
 *      bdev I/O 종류를 처리.
 *   4) **DIF reference tag remap**: T10 PI(Protection Information) Reference Tag는 LBA를
 *      포함하므로 partition→base offset 차이만큼 다시 계산 필요. spdk_dif_remap_ref_tag /
 *      spdk_dix_remap_ref_tag로 처리. (dif_check_flags에 REFTAG_CHECK가 있을 때만)
 *
 * 본 파일은 코드 수정 없이 한국어 주석만 추가/보강된 학습 사본이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 흐름(예: GPT 모듈):
 *   bdev_nvme가 NVMe SSD를 base bdev로 등록
 *     → module/bdev/gpt가 base bdev examine 시 GPT header 읽음
 *       → 각 GPT 엔트리마다 spdk_bdev_part_base_construct_ext() 1번
 *         + spdk_bdev_part_construct() N번 (N=파티션 수) 호출
 *           → 각 partition이 독립된 spdk_bdev로 등록 → 사용자에게 노출
 *
 * I/O 시:
 *   사용자 → spdk_bdev_writev_blocks(partition, ch, ...)
 *     → bdev.c → partition->fn_table->submit_request(ch, bdev_io)
 *       → (파티션 모듈이 정의한 submit_request — 보통 spdk_bdev_part_submit_request 그대로 사용)
 *         → spdk_bdev_part_submit_request_ext (이 파일)
 *           → offset += part->internal.offset_blocks
 *           → spdk_bdev_writev_blocks_ext(base_desc, base_ch, ...) (재귀 아님 — base bdev로 전달)
 *             → base bdev 모듈(예: bdev_nvme) → NVMe Write
 *               → 완료 → bdev_part_complete_io → partition bdev_io 완료 → 사용자 cb
 *
 * 실행 컨텍스트: I/O는 **partition io_channel을 만든 spdk_thread**에서 시작/종료.
 * channel_create_cb에서 base의 io_channel을 같은 스레드로 가져와 ch->base_ch에 저장 →
 * I/O remap 시 그 base_ch를 그대로 사용 (스레드 affinity 유지).
 * 단 base의 open/close는 base를 처음 open한 스레드에서만 수행 (base->thread). 다른
 * 스레드에서 close 시 spdk_thread_send_msg로 위임.
 *
 * === 타 모듈과의 연결 ===
 *  - **include/spdk/bdev_module.h** — `struct spdk_bdev_part`, `spdk_bdev_part_base`,
 *    `spdk_bdev_part_channel` 정의. 본 파일이 그 구현체.
 *  - **lib/bdev/bdev.c** — spdk_bdev_register/unregister/open_ext/close, claim/release,
 *    readv_blocks_ext, writev_blocks_ext, ... 등 모든 base bdev 호출의 실제 진입점.
 *  - **lib/util/dif.c** — spdk_dif_ctx_init, spdk_dif_remap_ref_tag, spdk_dix_remap_ref_tag.
 *    PI Reference Tag remap 시 사용.
 *  - **lib/thread** — spdk_io_device_register/unregister, spdk_get_io_channel,
 *    spdk_get_thread, spdk_thread_send_msg.
 *  - **호출 모듈** — module/bdev/gpt, module/bdev/split, module/bdev/lvol(부분),
 *    module/bdev/raid(과거) 등.
 *
 * === 주요 함수/구조체 요약 ===
 *  구조체:
 *    - spdk_bdev_part_base : base bdev 1개에 대한 공유 컨텍스트 (desc, ref count, claim flag,
 *      base_free_fn, ch_create/destroy_cb, remove_cb, base가 open된 thread 등).
 *    - spdk_bdev_part      : (외부 헤더에 정의) 한 partition의 전체 상태. internal.bdev이 SPDK에
 *      등록되는 spdk_bdev. internal.base가 위 part_base를 가리킴. internal.offset_blocks가
 *      partition의 시작 LBA(base 기준).
 *    - spdk_bdev_part_channel : 각 partition io_channel의 ctx — part 포인터 + base_ch.
 *
 *  함수:
 *    - spdk_bdev_part_base_construct_ext : base bdev 1개 open/claim하고 part_base 할당.
 *    - spdk_bdev_part_construct_ext       : partition 1개를 spdk_bdev로 등록.
 *    - spdk_bdev_part_submit_request_ext : partition I/O를 base bdev로 remap+forward.
 *    - bdev_part_complete_io             : base bdev 완료 → partition bdev_io 완료.
 *    - bdev_part_remap_dif               : T10 PI Reference Tag remap.
 *    - spdk_bdev_part_base_hotremove     : base bdev hot-remove 알림 시 모든 part unregister.
 *    - spdk_bdev_part_free               : partition 해제 (비동기 — destruct_done까지).
 *    - getter류 (get_bdev/get_desc/get_tailq/get_ctx/get_bdev_name/...): part_base/part 필드 노출.
 */

#include "spdk/bdev.h"
/* [한국어] bdev 공개 API — spdk_bdev_open_ext/close, get_io_channel, *_blocks_ext 등. */
#include "spdk/likely.h"
/* [한국어] spdk_likely / spdk_unlikely 분기 예측 매크로. DIF remap 우회 fast-path에 사용. */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG / SPDK_NOTICELOG 등 로깅 매크로. */
#include "spdk/string.h"
/* [한국어] spdk_strerror — 에러 코드 → 사람이 읽는 문자열. open 실패 로그에 사용. */
#include "spdk/thread.h"
/* [한국어] spdk_get_thread, spdk_thread_send_msg, spdk_io_device_register/unregister,
 *  spdk_get_io_channel 등 스레드 모델 API. base close cross-thread dispatch에 필요. */

#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈 측 API + struct spdk_bdev_part / part_base / part_channel 정의. */

/* This namespace UUID was generated using uuid_generate() method. */
#define BDEV_PART_NAMESPACE_UUID "976b899e-3e1e-4d71-ab69-c2b08e9df8b8"
/* [한국어] partition UUID 결정론적 생성 시 사용하는 네임스페이스 UUID (RFC 4122 Version 5 SHA-1).
 *  본 namespace UUID + (base bdev UUID, offset, num_blocks) → SHA-1 → partition UUID.
 *  이렇게 하면 같은 base를 같은 방식으로 재파티셔닝하면 항상 같은 UUID가 나와
 *  configuration 재시작 시에도 partition 식별자가 일관된다. uuid_generate()로 한 번 생성된
 *  하드코딩 상수이며, 변경하면 기존에 등록된 partition들의 UUID가 모두 바뀌므로 절대 변경 금지. */

struct spdk_bdev_part_base {
	struct spdk_bdev		*bdev;
	/* [한국어] 본 part_base가 매핑하는 base bdev. spdk_bdev_open_ext 성공 시점에
	 *  spdk_bdev_desc_get_bdev(desc)로 추출해 저장. 모든 partition의 I/O는 결국 이 bdev로 향함.
	 *  설정자: spdk_bdev_part_base_construct_ext. 읽는 자: 모든 getter, hotremove, submit. */

	struct spdk_bdev_desc		*desc;
	/* [한국어] base bdev에 대한 open descriptor (spdk_bdev_open_ext의 결과).
	 *  이 desc는 close될 때까지 base bdev에 대한 owning reference 역할을 함.
	 *  설정자: construct에서 설정. 해제: bdev_part_base_free 또는 spdk_bdev_part_base_free. */

	uint32_t			ref;
	/* [한국어] 이 part_base를 참조 중인 partition 개수 (refcount).
	 *  spdk_bdev_part_construct에서 ++, bdev_part_free_cb에서 --. 0이 되면
	 *  base bdev claim release + part_base 자체를 free. */

	uint32_t			channel_size;
	/* [한국어] 각 partition io_channel ctx의 바이트 크기. 모듈이 spdk_bdev_part_channel을
	 *  서브클래싱할 수 있도록 sizeof(파생 구조체)을 받아 io_device_register에 전달.
	 *  설정자: construct_ext 인자. 읽는 자: bdev_part_channel_create_cb이 io_device_register에 사용. */

	spdk_bdev_part_base_free_fn	base_free_fn;
	/* [한국어] part_base가 최종 해제될 때 호출할 모듈 콜백 (예: GPT 모듈이 자체 ctx를 free).
	 *  NULL이면 호출 안 함. 설정자: construct. 호출자: spdk_bdev_part_base_free. */

	void				*ctx;
	/* [한국어] 모듈 고유 컨텍스트 포인터 — base_free_fn에 그대로 전달됨.
	 *  본 파일에서는 의미를 부여하지 않고 단순 보관. */

	bool				claimed;
	/* [한국어] base bdev에 대해 spdk_bdev_module_claim_bdev를 이미 호출했는지.
	 *  최초 partition construct 시 true가 되며, ref==0이 되어 release할 때 false로 돌아감.
	 *  여러 partition이 같은 base를 공유해도 claim은 단 한 번만 수행. */

	struct spdk_bdev_module		*module;
	/* [한국어] 이 part_base를 만든 bdev 모듈 식별자 (예: gpt, lvol).
	 *  spdk_bdev_module_claim_bdev에 모듈 ID로 전달 — claim 추적 + permission 검증에 사용. */

	struct spdk_bdev_fn_table	*fn_table;
	/* [한국어] partition들이 사용할 spdk_bdev fn_table 포인터. 모듈이 채워서 넘기지만
	 *  본 파일이 get_io_channel/io_type_supported 두 함수를 강제로 덮어씌움 (line 476-477).
	 *  나머지(submit_request 등)는 모듈 책임. */

	struct bdev_part_tailq		*tailq;
	/* [한국어] 이 base에 속한 모든 partition을 잇는 TAILQ의 head (모듈이 소유). 모듈 단위로
	 *  partition 리스트를 순회·hot-remove할 수 있게 하기 위함. spdk_bdev_part는 자기 tailq 노드
	 *  필드 `tailq`를 가짐. */

	spdk_io_channel_create_cb	ch_create_cb;
	/* [한국어] partition io_channel 생성 시 모듈에 추가 초기화를 위임할 콜백 (NULL 가능).
	 *  본 파일의 bdev_part_channel_create_cb이 base_ch를 가져온 후 호출.
	 *  반환값으로 추가 실패 신호를 줄 수 있음 (음수 → 채널 생성 실패). */

	spdk_io_channel_destroy_cb	ch_destroy_cb;
	/* [한국어] partition io_channel 파괴 시 모듈에 정리를 위임할 콜백 (NULL 가능).
	 *  bdev_part_channel_destroy_cb가 base_ch를 put 하기 전에 호출. */

	spdk_bdev_remove_cb_t		remove_cb;
	/* [한국어] base bdev hot-remove 이벤트가 도착하면 모듈에 통지할 콜백.
	 *  bdev_part_base_event_cb이 SPDK_BDEV_EVENT_REMOVE 수신 시 호출.
	 *  모듈은 보통 spdk_bdev_part_base_hotremove를 호출해 자신의 모든 partition을 unregister. */

	struct spdk_thread		*thread;
	/* [한국어] base bdev를 open한 스레드 (= part_base를 construct한 스레드).
	 *  spdk_bdev_close는 open한 동일 스레드에서 호출되어야 하므로 기록.
	 *  다른 스레드에서 free 시 spdk_thread_send_msg로 이 스레드에 디스패치. */
};

/*
 * [한국어]
 * spdk_bdev_part_base_get_bdev - part_base가 보유한 base bdev 포인터 반환.
 *
 * @part_base: 대상 part_base.
 * @return:    base bdev 포인터 (NULL이면 아직 open 전 — 정상 상태에서는 NULL일 수 없음).
 *
 * 모듈이 base bdev의 속성(name, blocklen 등)을 조회할 때 사용하는 단순 getter.
 * 본 파일 외부에서 part_base 필드 직접 접근을 피하기 위한 캡슐화 헬퍼.
 *
 * 호출 컨텍스트: 어느 spdk_thread에서든 안전 (read-only).
 */
struct spdk_bdev *
spdk_bdev_part_base_get_bdev(struct spdk_bdev_part_base *part_base)
{
	return part_base->bdev;
	/* [한국어] base bdev 포인터를 그대로 반환. */
}

/*
 * [한국어]
 * spdk_bdev_part_base_get_desc - part_base가 보유한 base bdev open descriptor 반환.
 *
 * @part_base: 대상 part_base.
 * @return:    base bdev desc (소유권은 part_base에 있음 — 호출자는 close 금지).
 *
 * 모듈이 base bdev에 대해 spdk_bdev_get_io_channel 등을 직접 호출하고 싶을 때 사용.
 */
struct spdk_bdev_desc *
spdk_bdev_part_base_get_desc(struct spdk_bdev_part_base *part_base)
{
	return part_base->desc;
	/* [한국어] base desc 반환 — 모듈은 이 desc로 base I/O channel을 추가로 열 수 있다. */
}

/*
 * [한국어]
 * spdk_bdev_part_base_get_tailq - part_base가 가리키는 partition tailq head 반환.
 *
 * @part_base: 대상 part_base.
 * @return:    partition들이 연결된 TAILQ head (모듈이 소유).
 *
 * 모듈이 자신의 partition 목록을 순회할 때 사용.
 */
struct bdev_part_tailq *
spdk_bdev_part_base_get_tailq(struct spdk_bdev_part_base *part_base)
{
	return part_base->tailq;
	/* [한국어] tailq head 반환 — TAILQ_FOREACH로 순회 가능. */
}

/*
 * [한국어]
 * spdk_bdev_part_base_get_ctx - 모듈이 construct 시 넘긴 ctx 포인터 반환.
 *
 * @part_base: 대상 part_base.
 * @return:    construct 시 전달된 ctx (모듈 정의 의미).
 *
 * 모듈 콜백(remove_cb, ch_create_cb 등)에서 자기 컨텍스트로 복귀할 때 사용.
 */
void *
spdk_bdev_part_base_get_ctx(struct spdk_bdev_part_base *part_base)
{
	return part_base->ctx;
	/* [한국어] 모듈 ctx 반환. */
}

/*
 * [한국어]
 * spdk_bdev_part_base_get_bdev_name - base bdev의 이름 문자열 반환.
 *
 * @part_base: 대상 part_base.
 * @return:    base bdev 이름 (소유권은 base bdev에 있음 — free 금지).
 *
 * 로깅/디버깅 헬퍼.
 */
const char *
spdk_bdev_part_base_get_bdev_name(struct spdk_bdev_part_base *part_base)
{
	return part_base->bdev->name;
	/* [한국어] base bdev 이름 그대로 반환. */
}

/*
 * [한국어]
 * bdev_part_base_free - cross-thread close용 trampoline. base->thread에서 호출되도록 메시지로 위임됨.
 *
 * @ctx: spdk_bdev_desc 포인터 (메시지로 전달된 인자).
 *
 * spdk_bdev_part_base_free에서 호출 스레드 ≠ base->thread인 경우 spdk_thread_send_msg로
 * base->thread에 본 함수를 dispatch한다. spdk_bdev_close는 thread affinity를 요구하므로
 * 이 trampoline이 필요.
 */
static void
bdev_part_base_free(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;
	/* [한국어] send_msg arg로 전달된 base desc를 받음. */

	spdk_bdev_close(desc);
	/* [한국어] base->thread에서 base bdev close — bdev 모듈에 release 통지.
	 *  모든 partition이 unregister된 이후에만 호출되므로 안전. */
}

/*
 * [한국어]
 * spdk_bdev_part_base_free - part_base 자체와 그 안의 base bdev open을 해제.
 *
 * @base: 해제할 part_base.
 *
 * 호출 시점: 마지막 partition이 free되어 ref==0이 되었을 때 (bdev_part_free_cb 안에서),
 * 또는 construct 도중 실패한 경우 cleanup용. base bdev close는 open한 스레드에서만
 * 가능하므로 thread affinity를 검사해 cross-thread면 send_msg로 위임한다.
 *
 * 호출 체인:
 *   bdev_part_free_cb (마지막 partition free) → [이 함수] → spdk_bdev_close + base_free_fn + free(base)
 */
void
spdk_bdev_part_base_free(struct spdk_bdev_part_base *base)
{
	if (base->desc) {
		/* [한국어] desc가 NULL이 아닌 경우만 close 시도. construct 실패 경로에서는
		 *  open 전이라 NULL일 수 있어 가드 필요. */
		/* Close the underlying bdev on its same opened thread. */
		if (base->thread && base->thread != spdk_get_thread()) {
			/* [한국어] 다른 스레드에서 free 호출됨 — base를 open한 스레드로 dispatch.
			 *  spdk_bdev_close는 동일 spdk_thread 컨텍스트를 요구하기 때문. */
			spdk_thread_send_msg(base->thread, bdev_part_base_free, base->desc);
			/* [한국어] base->thread의 메시지 큐에 bdev_part_base_free 푸시.
			 *  해당 reactor가 다음 polling 시 실행. lock-free MPSC ring 기반. */
		} else {
			/* [한국어] 호출 스레드가 base->thread와 같음 (또는 thread 추적 불가) — 즉시 close. */
			spdk_bdev_close(base->desc);
			/* [한국어] base bdev에 대한 open reference 해제 — base bdev은 모든 desc가
			 *  닫히고 unregister되면 destruct 진행. */
		}
	}

	if (base->base_free_fn != NULL) {
		/* [한국어] 모듈이 base 해제 시 추가 cleanup을 원하면 콜백 등록 — 여기서 호출. */
		base->base_free_fn(base->ctx);
		/* [한국어] 모듈 ctx와 함께 콜백 실행 — 보통 모듈이 자기 추가 자료구조 free. */
	}

	free(base);
	/* [한국어] part_base 구조체 자체 해제. 이 시점 이후 base 포인터는 유효하지 않음. */
}

/*
 * [한국어]
 * bdev_part_free_cb - spdk_io_device_unregister 완료 콜백.
 *
 * @io_device: 등록 시 사용된 io_device(=spdk_bdev_part 포인터).
 *
 * spdk_bdev_part_free → spdk_io_device_unregister가 모든 채널을 정리한 후 본 콜백을 호출.
 * 여기서 partition을 base의 tailq에서 떼어내고, ref count를 줄여 0이 되면 base release.
 * 마지막으로 spdk_bdev_destruct_done으로 SPDK bdev 시스템에 destruct 완료 통지.
 *
 * 호출 체인:
 *   spdk_bdev_part_free → spdk_io_device_unregister → (모든 채널 정리 후) → [이 함수]
 */
static void
bdev_part_free_cb(void *io_device)
{
	struct spdk_bdev_part *part = io_device;
	/* [한국어] io_device로 등록된 partition 포인터 복원. */
	struct spdk_bdev_part_base *base;
	/* [한국어] partition이 속한 base 포인터 (아래에서 설정). */

	assert(part);
	/* [한국어] 정상 경로면 part는 항상 non-NULL. defensive assert. */
	assert(part->internal.base);
	/* [한국어] construct 성공한 partition은 반드시 base를 가짐. */

	base = part->internal.base;
	/* [한국어] base 포인터 추출 — 이후 ref decrement에 사용. */

	TAILQ_REMOVE(base->tailq, part, tailq);
	/* [한국어] 모듈이 관리하는 base의 partition 리스트에서 본 part 제거.
	 *  hot-remove나 모듈의 다른 순회가 더 이상 이 partition을 보지 못하게 됨. */

	if (--base->ref == 0) {
		/* [한국어] 이 partition이 base를 참조하는 마지막 partition이었음 — base도 정리.
		 *  ref decrement는 본 콜백이 동일 thread에서만 호출된다는 보장 하에 atomic 불필요. */
		spdk_bdev_module_release_bdev(base->bdev);
		/* [한국어] construct 시점에 잡았던 claim 해제. base bdev이 다른 모듈에 의해
		 *  다시 claim 가능 상태가 됨. */
		spdk_bdev_part_base_free(base);
		/* [한국어] base 자체 해제 — desc close + base_free_fn 호출 + free(base). */
	}

	spdk_bdev_destruct_done(&part->internal.bdev, 0);
	/* [한국어] SPDK bdev 시스템에 partition destruct 완료 통지 (rc=0=성공).
	 *  spdk_bdev_unregister가 호출 시점에 destruct를 비동기로 시작해놓은 상태였으므로
	 *  이 호출로 unregister 콜백 체인이 마무리된다. */
	free(part->internal.bdev.name);
	/* [한국어] construct 시 strdup으로 할당했던 partition 이름 해제. */
	free(part->internal.bdev.product_name);
	/* [한국어] construct 시 strdup으로 할당했던 product name 해제. */
	free(part);
	/* [한국어] partition 구조체 자체 해제. */
}

/*
 * [한국어]
 * spdk_bdev_part_free - partition 해제 진입점 (모듈에서 호출).
 *
 * @part: 해제할 partition.
 * @return: 1 — SPDK bdev unregister 규약상 "비동기 destruct 진행 중, 완료는
 *          spdk_bdev_destruct_done으로 통지" 의미.
 *
 * 모듈의 fn_table->destruct에서 호출된다. spdk_io_device_unregister는 모든 active 채널이
 * 정리될 때까지 비동기로 대기한 후 bdev_part_free_cb를 호출한다.
 *
 * 호출 체인:
 *   spdk_bdev_unregister → 모듈 fn_table->destruct → [이 함수] → spdk_io_device_unregister
 *     → (모든 채널 정리) → bdev_part_free_cb → spdk_bdev_destruct_done(0)
 */
int
spdk_bdev_part_free(struct spdk_bdev_part *part)
{
	spdk_io_device_unregister(part, bdev_part_free_cb);
	/* [한국어] partition io_device 등록 해제 요청. spdk_thread가 모든 채널 destroy를
	 *  코디네이션한 후 본 함수가 등록한 콜백(bdev_part_free_cb)을 호출. 비동기. */

	/* Return 1 to indicate that this is an asynchronous operation that isn't complete
	 * until spdk_bdev_destruct_done is called */
	return 1;
	/* [한국어] SPDK bdev 규약: destruct가 비동기 완료를 의미. bdev.c는 1을 받으면
	 *  destruct_done 통지를 기다린 후 실제 unregister 마무리. */
}

/*
 * [한국어]
 * spdk_bdev_part_base_hotremove - base bdev hot-remove 시 모듈이 호출하는 헬퍼.
 *
 * @part_base: hot-remove 통지를 받은 base의 part_base.
 * @tailq:     part_base에 속한 partition들의 TAILQ head.
 *
 * 모듈의 remove_cb 콜백 본체에서 호출. base에 속한 모든 partition을 spdk_bdev_unregister로
 * 하나씩 해제 → 사용자가 더 이상 I/O를 보낼 수 없도록 함. unregister는 내부적으로
 * 비동기 destruct를 trigger하므로 즉시 partition이 사라지지는 않는다.
 *
 * 호출 체인:
 *   base bdev event(REMOVE) → bdev_part_base_event_cb → 모듈 remove_cb → [이 함수]
 *     → 각 partition spdk_bdev_unregister → 모듈 fn_table->destruct → spdk_bdev_part_free
 */
void
spdk_bdev_part_base_hotremove(struct spdk_bdev_part_base *part_base, struct bdev_part_tailq *tailq)
{
	struct spdk_bdev_part *part, *tmp;
	/* [한국어] FOREACH_SAFE 순회용 iterator + next. unregister가 tailq 노드를 변경할 수 있어 SAFE 필요. */

	TAILQ_FOREACH_SAFE(part, tailq, tailq, tmp) {
		/* [한국어] 모듈의 모든 partition을 순회. 매크로의 두 번째 'tailq'는 head,
		 *  세 번째 'tailq'는 spdk_bdev_part 안의 노드 필드 이름. */
		if (part->internal.base == part_base) {
			/* [한국어] 같은 모듈이 여러 base에 partition을 만들 수 있으므로 base 일치만 필터.
			 *  본 part_base에 속한 partition만 hot-remove 대상. */
			spdk_bdev_unregister(&part->internal.bdev, NULL, NULL);
			/* [한국어] partition을 SPDK bdev 시스템에서 제거. cb=NULL — 완료 통지 받지 않음.
			 *  내부적으로 사용자 desc들에 REMOVE 이벤트를 보내고 모든 close 후 destruct 호출. */
		}
	}
}

/*
 * [한국어]
 * bdev_part_io_type_supported - partition이 특정 I/O 타입 지원하는지 질의.
 *
 * @_part:   void*로 전달된 partition 포인터 (fn_table 콜백 시그니처).
 * @io_type: 질의 대상 I/O 타입 (READ/WRITE/UNMAP/...).
 * @return:  지원하면 true, 미지원이면 false.
 *
 * 본 함수는 spdk_bdev_part_base_construct_ext에서 모듈의 fn_table->io_type_supported를
 * 강제로 덮어씀(line 476-477). 일부 NVMe passthrough 타입은 partition에서 LBA decode가
 * 불가능하므로 false 강제, 그 외는 base bdev에 위임.
 */
static bool
bdev_part_io_type_supported(void *_part, enum spdk_bdev_io_type io_type)
{
	struct spdk_bdev_part *part = _part;
	/* [한국어] void* 복원 — fn_table은 ctxt를 첫 인자로 받음. */

	/* We can't decode/modify passthrough NVMe commands, so don't report
	 *  that a partition supports these io types, even if the underlying
	 *  bdev does.
	 */
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		/* [한국어] NVMe passthrough 명령들은 raw NVMe command를 디바이스로 직접 전달하는 경로.
		 *  partition은 명령 안의 SLBA/NLB 필드를 모르고 또 안전하게 디코드할 수도 없으므로
		 *  base가 지원하더라도 partition에서는 미지원으로 보고 (오용 방지). */
		return false;
		/* [한국어] 미지원 — 사용자가 이 타입을 요청하면 -EINVAL 등으로 거부됨. */
	default:
		break;
		/* [한국어] 그 외 타입은 base bdev에 위임해 결정. */
	}

	return part->internal.base->bdev->fn_table->io_type_supported(part->internal.base->bdev->ctxt,
			io_type);
	/* [한국어] base bdev 모듈의 io_type_supported를 그대로 호출 — base가 지원하는 모든
	 *  R/W/UNMAP/FLUSH/ZCOPY/COMPARE/COPY 등을 partition도 동일하게 지원한다고 보고.
	 *  두 번째 ctxt 인자는 base bdev 모듈의 컨텍스트 (모듈 자기 데이터). */
}

/*
 * [한국어]
 * bdev_part_get_io_channel - partition의 io_channel 1개를 호출 thread에 대해 가져옴.
 *
 * @_part: void*로 전달된 partition 포인터.
 * @return: spdk_io_channel 포인터 (실패 시 NULL).
 *
 * fn_table->get_io_channel 콜백. 본 함수는 part_base_construct_ext에서 fn_table에
 * 강제 등록된다(line 476). 실제 채널 ctx 초기화는 bdev_part_channel_create_cb에서.
 */
static struct spdk_io_channel *
bdev_part_get_io_channel(void *_part)
{
	struct spdk_bdev_part *part = _part;
	/* [한국어] void* 복원. */

	return spdk_get_io_channel(part);
	/* [한국어] partition을 io_device로 등록해두었으므로 (line 661-664), 호출 thread 기준으로
	 *  io_channel을 가져온다. 같은 thread에서 두 번째 호출 시 캐시된 채널 반환 (refcount 증가). */
}

/*
 * [한국어]
 * spdk_bdev_part_get_bdev - partition 객체에서 노출된 spdk_bdev 포인터 반환.
 *
 * @part:   대상 partition.
 * @return: SPDK bdev 시스템에 등록된 spdk_bdev 포인터 (사용자가 보는 partition bdev).
 *
 * 모듈이 partition의 spdk_bdev에 접근해 추가 속성 설정/조회할 때 사용.
 */
struct spdk_bdev *
spdk_bdev_part_get_bdev(struct spdk_bdev_part *part)
{
	return &part->internal.bdev;
	/* [한국어] internal.bdev은 embedded — 주소를 그대로 반환. lifetime은 partition과 동일. */
}

/*
 * [한국어]
 * spdk_bdev_part_get_base - partition이 속한 part_base 반환.
 */
struct spdk_bdev_part_base *
spdk_bdev_part_get_base(struct spdk_bdev_part *part)
{
	return part->internal.base;
	/* [한국어] construct 시점에 설정된 base 포인터 반환. */
}

/*
 * [한국어]
 * spdk_bdev_part_get_base_bdev - partition이 노출하는 underlying base bdev 반환 (편의 헬퍼).
 */
struct spdk_bdev *
spdk_bdev_part_get_base_bdev(struct spdk_bdev_part *part)
{
	return part->internal.base->bdev;
	/* [한국어] part → base → bdev 한 번에 추적. */
}

/*
 * [한국어]
 * spdk_bdev_part_get_offset_blocks - partition이 base 안에서 차지하는 시작 LBA 반환.
 *
 * @part:   대상 partition.
 * @return: base bdev 기준 시작 블록 인덱스. I/O remap 시 더해지는 오프셋.
 */
uint64_t
spdk_bdev_part_get_offset_blocks(struct spdk_bdev_part *part)
{
	return part->internal.offset_blocks;
	/* [한국어] construct 시 받은 offset_blocks 그대로 반환 (read-only). */
}

/*
 * [한국어]
 * bdev_part_remap_dif - T10 PI Reference Tag를 partition→base offset 차이만큼 다시 계산.
 *
 * @bdev_io:        대상 bdev_io (READ 완료 후 또는 WRITE 직전).
 * @offset:         partition 기준 LBA (호스트가 본 offset).
 * @remapped_offset: base 기준 LBA (실제 저장 위치).
 * @return:         0=성공/필요 없음, 음수=remap 실패 (DIF context 초기화 또는 tag check 오류).
 *
 * T10 PI(Protection Information)에서 Reference Tag는 보통 LBA의 하위 32비트와 일치해야
 * 하므로 partition으로 들어온 데이터가 base에 다른 LBA로 저장될 때 PI tag도 새 LBA에
 * 맞춰 다시 써야 한다. 본 함수는 dif_check_flags에 REFTAG_CHECK가 켜져 있을 때만 동작.
 *
 *  - WRITE 경로: submit 직전에 호출 — 호스트가 partition LBA로 만든 PI를 base LBA로 변환.
 *  - READ 경로: 완료 콜백에서 호출 — 디바이스가 base LBA로 만든 PI를 호스트가 기대하는
 *               partition LBA로 변환.
 *
 * md_interleave 여부에 따라 spdk_dif_remap_ref_tag (data+md interleaved) 또는
 * spdk_dix_remap_ref_tag (separate md buffer)를 사용.
 */
static int
bdev_part_remap_dif(struct spdk_bdev_io *bdev_io, uint32_t offset,
		    uint32_t remapped_offset)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	/* [한국어] partition bdev — blocklen, md_len, dif_type 등 PI 메타 추출용. */
	struct spdk_dif_ctx dif_ctx;
	/* [한국어] DIF 연산 컨텍스트 (block size, md size, type, flags 등 포함). */
	struct spdk_dif_error err_blk = {};
	/* [한국어] DIF 검증 실패 시 어떤 블록의 어떤 필드인지 채워지는 출력 구조. {} 초기화. */
	int rc;
	/* [한국어] 반환 코드 임시 보관. */
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	/* [한국어] dif_ctx 확장 옵션 (PI format 지정). */

	if (spdk_likely(!(bdev_io->u.bdev.dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK))) {
		/* [한국어] 대다수 I/O는 PI 미사용 → REFTAG_CHECK 비트가 없음. fast-path로 즉시 0 반환.
		 *  spdk_likely로 분기 예측 힌트 — 이 함수는 모든 R/W에 호출되므로 hot path. */
		return 0;
		/* [한국어] PI 사용 안 함 — remap 불필요. */
	}

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	/* [한국어] ABI 호환성용 size 필드 — 구조체 끝 필드까지의 offset을 size로 명시.
	 *  구조체가 확장되어도 호환되도록 설계. */
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;
	/* [한국어] PI format = 16-byte (T10 PI 표준 — guard 16, app 16, ref 32비트). */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen, bdev->md_len, bdev->md_interleave,
			       bdev->dif_is_head_of_md, bdev->dif_type, bdev_io->u.bdev.dif_check_flags,
			       offset, 0, 0, 0, 0, &dif_opts);
	/* [한국어] dif_ctx 초기화 — 블록/메타 크기, interleave 여부, head 위치, DIF 타입(0/1/2/3),
	 *  검사 플래그, 시작 LBA(offset)를 모두 채움. 마지막 0,0,0,0은 app_tag/ref_tag 초기값들.
	 *  remapped_offset은 아래의 set_remapped_init_ref_tag로 별도 설정. */
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF context failed\n");
		/* [한국어] dif_ctx 초기화 실패 — 잘못된 인자 또는 unsupported 조합. 로그 후 에러 전파. */
		return rc;
		/* [한국어] 호출자에게 에러 코드 전달 — submit_request에서는 IO_STATUS_FAILED 반환. */
	}

	spdk_dif_ctx_set_remapped_init_ref_tag(&dif_ctx, remapped_offset);
	/* [한국어] remap 후 reference tag 시작 값을 base LBA(remapped_offset)로 설정.
	 *  remap 함수는 (현재 ref_tag - offset) + remapped_offset 식으로 변환. */

	if (bdev->md_interleave) {
		/* [한국어] data + metadata가 한 buffer에 interleave된 케이스 (보통 4KB+8B 단위 반복).
		 *  iov 안에 metadata가 끼어 있으므로 spdk_dif_remap_ref_tag가 자체 stride 계산. */
		rc = spdk_dif_remap_ref_tag(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					    bdev_io->u.bdev.num_blocks, &dif_ctx, &err_blk, true);
		/* [한국어] iov 전체를 순회하며 각 PI 블록의 ref_tag를 새 값으로 덮어씀.
		 *  마지막 인자 true = 검증도 함께 수행 (변환 전 값이 예상치와 일치하는지). */
	} else {
		/* [한국어] separate metadata buffer 케이스 — data와 md가 다른 buffer.
		 *  spdk_dix_remap_ref_tag를 사용해 md_buf만 순회. */
		struct iovec md_iov = {
			.iov_base	= bdev_io->u.bdev.md_buf,
			.iov_len	= bdev_io->u.bdev.num_blocks * bdev->md_len,
		};
		/* [한국어] md만 담은 임시 단일 iov 생성 — base=md_buf, len=block수×md_len.
		 *  스택 변수라 함수 종료 시 자동 해제. */

		rc = spdk_dix_remap_ref_tag(&md_iov, bdev_io->u.bdev.num_blocks, &dif_ctx, &err_blk, true);
		/* [한국어] separate md 전용 remap. true = 검증 포함. */
	}

	if (rc != 0) {
		SPDK_ERRLOG("Remapping reference tag failed. type=%d, offset=%" PRIu32 "\n",
			    err_blk.err_type, err_blk.err_offset);
		/* [한국어] remap 실패 — 보통 (a) 검증 단계에서 ref_tag 불일치 또는 (b) format 오류.
		 *  err_blk에 채워진 type/offset으로 디버깅 단서 제공. */
	}

	return rc;
	/* [한국어] 0=성공, 음수=실패. 호출자(submit/complete)는 실패 시 IO를 FAILED로 표시. */
}

/*
 * [한국어]
 * bdev_part_complete_io - base bdev I/O 완료 콜백. partition bdev_io를 완료 처리.
 *
 * @bdev_io: base bdev로 보낸 I/O의 결과 객체.
 * @success: base bdev 측 성공/실패 플래그.
 * @cb_arg:  submit 시 함께 넘긴 partition bdev_io 포인터.
 *
 * spdk_bdev_part_submit_request_ext가 base에 I/O를 발행할 때 본 함수를 콜백으로 등록.
 * READ 성공 시 PI Reference Tag remap 수행 (base LBA → partition LBA),
 * ZCOPY 시 base가 매핑한 buffer를 partition bdev_io에 set,
 * 그 외는 status만 base→partition으로 전파. partition bdev_io는 split 모드인지 여부에
 * 따라 stored_user_cb 또는 spdk_bdev_io_complete_base_io_status로 마무리.
 *
 * 호출 컨텍스트: base bdev 완료 콜백 — partition io_channel을 만든 동일 spdk_thread에서
 * 호출됨 (channel_create_cb에서 같은 thread로 base_ch를 가져왔기 때문에 자연 보장).
 */
static void
bdev_part_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *part_io = cb_arg;
	/* [한국어] submit 시 cb_arg로 넘긴 partition 측 bdev_io 복원. */
	uint32_t offset, remapped_offset;
	/* [한국어] DIF remap에 쓸 base LBA / partition LBA. READ 케이스에서만 채워짐. */
	int rc;
	/* [한국어] DIF remap 반환 코드 임시. */

	switch (bdev_io->type) {
	/* [한국어] base I/O 타입별로 완료 후 처리 분기. */
	case SPDK_BDEV_IO_TYPE_READ:
		if (success) {
			/* [한국어] READ 성공 시에만 PI remap 시도. 실패면 데이터가 무효이므로 skip. */
			offset = bdev_io->u.bdev.offset_blocks;
			/* [한국어] base bdev_io에 기록된 base 기준 LBA. */
			remapped_offset = part_io->u.bdev.offset_blocks;
			/* [한국어] partition bdev_io에 기록된 partition 기준 LBA — 호스트에게 보일 값. */

			rc = bdev_part_remap_dif(bdev_io, offset, remapped_offset);
			/* [한국어] PI Reference Tag를 base LBA → partition LBA로 변환. */
			if (rc != 0) {
				success = false;
				/* [한국어] PI 검증/remap 실패 — 호스트에게 IO 실패로 보고. */
			}
		}
		break;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		spdk_bdev_io_set_buf(part_io, bdev_io->u.bdev.iovs[0].iov_base,
				     bdev_io->u.bdev.iovs[0].iov_len);
		/* [한국어] ZCOPY START 완료 — base가 자기 buffer를 매핑해 줬으니 그 포인터/길이를
		 *  partition bdev_io에도 그대로 set. 이후 사용자에게 노출됨. */
		break;
	default:
		break;
		/* [한국어] WRITE/UNMAP/FLUSH/RESET/ABORT/COMPARE/COPY 등은 추가 처리 없음 — status만 전파. */
	}

	if (part_io->internal.f.split) {
		/* [한국어] partition bdev_io가 split된 경우 — submit_ext에 cb를 따로 넘긴 케이스.
		 *  bdev_part_submit_request_ext에서 cb!=NULL이면 split=true + stored_user_cb 저장. */
		part_io->internal.split.stored_user_cb(part_io, success, NULL);
		/* [한국어] split 사용자(예: bdev split 모듈)가 직접 등록한 콜백 호출. */
	} else {
		/* [한국어] 일반 케이스 — base IO status를 그대로 partition IO에 복사 + 완료. */
		spdk_bdev_io_complete_base_io_status(part_io, bdev_io);
		/* [한국어] base의 status (NVMe sct/sc 포함)와 함께 partition bdev_io 완료 →
		 *  사용자 cb_fn 호출까지 진행. */
	}

	spdk_bdev_free_io(bdev_io);
	/* [한국어] base bdev_io 객체를 base channel io_pool에 반납. partition bdev_io는
	 *  사용자 cb 후 사용자가 spdk_bdev_free_io로 별도 반납. */
}

/*
 * [한국어]
 * bdev_part_init_ext_io_opts - partition bdev_io에서 ext I/O opts를 추출해 채움.
 *
 * @bdev_io: 사용자가 제출한 partition bdev_io.
 * @opts:    [out] 채울 ext_io_opts 구조 (스택 변수).
 *
 * spdk_bdev_*_blocks_ext 함수들에 넘길 opts 채우는 작은 헬퍼. memory_domain,
 * memory_domain_ctx, metadata buffer를 그대로 전달하고, dif_check_flags는
 * exclude_mask로 ~로 변환 (ext API 측 컨벤션).
 */
static inline void
bdev_part_init_ext_io_opts(struct spdk_bdev_io *bdev_io, struct spdk_bdev_ext_io_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	/* [한국어] 모든 필드 0으로 초기화 — 추가 미사용 필드의 garbage 방지. */
	opts->size = sizeof(*opts);
	/* [한국어] ABI versioning — 본 호출자가 보는 구조체 크기. base bdev API가
	 *  자신이 아는 크기와 비교해 호환성 판단. */
	opts->memory_domain = bdev_io->u.bdev.memory_domain;
	/* [한국어] DMA memory domain (예: NVMe-oF RDMA 등에서 호스트/디바이스 메모리 구분).
	 *  partition bdev_io의 값을 그대로 base I/O에 전파. */
	opts->memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	/* [한국어] memory_domain의 컨텍스트 — translation 시 보조 정보. */
	opts->metadata = bdev_io->u.bdev.md_buf;
	/* [한국어] separate metadata buffer 포인터 — base I/O에서도 동일 buffer 사용. */
	opts->dif_check_flags_exclude_mask = ~bdev_io->u.bdev.dif_check_flags;
	/* [한국어] ext API는 "이 비트들은 검사 제외"라는 음의 의미를 사용 → ~로 변환.
	 *  partition bdev_io의 dif_check_flags(검사 비트 집합)를 그대로 보존하기 위함. */
}

/*
 * [한국어]
 * spdk_bdev_part_submit_request_ext - partition bdev_io를 base bdev로 remap+forward.
 *
 * @ch:      partition io_channel context (part 포인터 + base io_channel 보유).
 * @bdev_io: 사용자가 partition bdev에 제출한 I/O 요청.
 * @cb:      NULL이 아니면 split 모드 — 완료 시 stored_user_cb로 호출됨.
 *           NULL이면 일반 모드 — base status가 partition IO에 직접 복사되어 완료.
 * @return:  0=성공 제출, 음수=실패 (즉시 FAILED), SPDK_BDEV_IO_STATUS_FAILED=DIF remap 실패.
 *
 * 모듈의 fn_table->submit_request에서 호출. 모든 I/O 타입에 대해:
 *   1) offset_blocks에 partition->offset_blocks를 더해 base LBA 계산
 *   2) WRITE 계열은 PI ref tag remap (partition LBA → base LBA)
 *   3) 해당 I/O 타입에 맞는 spdk_bdev_*_blocks 함수로 base bdev에 발행
 *   4) 완료 콜백 = bdev_part_complete_io
 *
 * 호출 체인:
 *   spdk_bdev_writev_blocks(part, ...) → bdev.c → 모듈 fn_table->submit_request
 *     → [이 함수] → spdk_bdev_writev_blocks_ext(base, ...) → base 모듈 → 디바이스 → 완료
 *       → bdev_part_complete_io → partition IO 완료 → 사용자 cb
 */
int
spdk_bdev_part_submit_request_ext(struct spdk_bdev_part_channel *ch, struct spdk_bdev_io *bdev_io,
				  spdk_bdev_io_completion_cb cb)
{
	struct spdk_bdev_part *part = ch->part;
	/* [한국어] channel ctx에 저장된 partition 포인터 — offset 계산에 사용. */
	struct spdk_io_channel *base_ch = ch->base_ch;
	/* [한국어] 같은 thread에서 미리 가져둔 base bdev io_channel — base I/O 발행에 사용. */
	struct spdk_bdev_desc *base_desc = part->internal.base->desc;
	/* [한국어] base bdev open desc — *_blocks 함수의 첫 인자. */
	struct spdk_bdev_ext_io_opts io_opts;
	/* [한국어] 스택의 ext I/O opts — bdev_part_init_ext_io_opts로 채워서 read/write_ext에 전달. */
	uint64_t offset, remapped_offset, remapped_src_offset;
	/* [한국어] partition LBA, base LBA, COPY src base LBA 임시 변수. */
	int rc = 0;
	/* [한국어] 반환 코드. switch 안에서 채워짐. */

	if (cb != NULL) {
		/* [한국어] split 호출 — bdev split 모듈 등이 자신의 콜백을 통해 후처리하고 싶을 때.
		 *  split flag를 켜고 user cb를 stored_user_cb에 보관 → bdev_part_complete_io가 그것 호출. */
		bdev_io->internal.f.split = true;
		/* [한국어] split 모드 표시 — complete에서 분기. */
		bdev_io->internal.split.stored_user_cb = cb;
		/* [한국어] 사용자 콜백 저장 — completion에서 호출. */
	}

	offset = bdev_io->u.bdev.offset_blocks;
	/* [한국어] 사용자가 partition 기준으로 지정한 LBA. */
	remapped_offset = offset + part->internal.offset_blocks;
	/* [한국어] partition LBA + partition 시작 base LBA = base bdev 기준 절대 LBA.
	 *  partition 추상화의 핵심 — 모든 I/O 타입에 동일하게 적용. */

	/* Modify the I/O to adjust for the offset within the base bdev. */
	switch (bdev_io->type) {
	/* [한국어] I/O 타입별로 base bdev에 맞는 spdk_bdev_*_blocks 함수 호출. */
	case SPDK_BDEV_IO_TYPE_READ:
		bdev_part_init_ext_io_opts(bdev_io, &io_opts);
		/* [한국어] ext opts 채움 — memory_domain/metadata/dif flags 전파.
		 *  READ는 ext 변형으로 호출해 PI 옵션을 보존해야 함. */
		rc = spdk_bdev_readv_blocks_ext(base_desc, base_ch, bdev_io->u.bdev.iovs,
						bdev_io->u.bdev.iovcnt, remapped_offset,
						bdev_io->u.bdev.num_blocks,
						bdev_part_complete_io, bdev_io, &io_opts);
		/* [한국어] base bdev에 vector READ 발행. iov는 호스트가 채울 buffer 그대로 전달.
		 *  완료 시 bdev_part_complete_io가 호출되어 PI ref tag remap (base→part) 수행 후
		 *  partition IO 완료. */
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = bdev_part_remap_dif(bdev_io, offset, remapped_offset);
		/* [한국어] WRITE는 발행 전에 PI ref tag remap (partition→base) — 호스트가 만든 PI 값을
		 *  base LBA에 맞춰 새로 계산해 buffer 안에 덮어씀. */
		if (rc != 0) {
			return SPDK_BDEV_IO_STATUS_FAILED;
			/* [한국어] PI 검증/remap 실패 — submit 자체를 즉시 실패로 처리.
			 *  bdev.c는 이 반환값을 받아 사용자 cb를 FAILED로 호출. */
		}
		bdev_part_init_ext_io_opts(bdev_io, &io_opts);
		/* [한국어] ext opts 채움. */
		rc = spdk_bdev_writev_blocks_ext(base_desc, base_ch, bdev_io->u.bdev.iovs,
						 bdev_io->u.bdev.iovcnt, remapped_offset,
						 bdev_io->u.bdev.num_blocks,
						 bdev_part_complete_io, bdev_io, &io_opts);
		/* [한국어] base bdev에 vector WRITE 발행 — PI 값은 이미 base LBA용으로 다시 계산된 상태. */
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(base_desc, base_ch, remapped_offset,
						   bdev_io->u.bdev.num_blocks, bdev_part_complete_io,
						   bdev_io);
		/* [한국어] WRITE_ZEROES — 데이터 buffer 없이 base에 0 fill 명령. NVMe Write Zeroes opcode 0x08. */
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(base_desc, base_ch, remapped_offset,
					    bdev_io->u.bdev.num_blocks, bdev_part_complete_io,
					    bdev_io);
		/* [한국어] UNMAP — TRIM/Deallocate. NVMe Dataset Management opcode 0x09. */
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(base_desc, base_ch, remapped_offset,
					    bdev_io->u.bdev.num_blocks, bdev_part_complete_io,
					    bdev_io);
		/* [한국어] FLUSH — 캐시된 write를 매체로 강제 영속화. NVMe Flush opcode 0x00. */
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(base_desc, base_ch,
				     bdev_part_complete_io, bdev_io);
		/* [한국어] RESET — base bdev 전체 reset (모든 outstanding I/O abort).
		 *  offset 인자가 없는 이유는 reset이 LBA 단위가 아니기 때문. */
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		rc = spdk_bdev_abort(base_desc, base_ch, bdev_io->u.abort.bio_to_abort,
				     bdev_part_complete_io, bdev_io);
		/* [한국어] ABORT — 특정 outstanding bdev_io 1개 취소. bio_to_abort는 partition bdev_io의
		 *  포인터인데 base bdev은 이를 모름 — bdev_abort 내부에서 매핑 처리.
		 *  (bio_to_abort가 partition IO이므로 실제 abort는 그 partition IO에 매핑된 base IO 대상). */
		break;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		rc = spdk_bdev_zcopy_start(base_desc, base_ch, NULL, 0, remapped_offset,
					   bdev_io->u.bdev.num_blocks, bdev_io->u.bdev.zcopy.populate,
					   bdev_part_complete_io, bdev_io);
		/* [한국어] ZCOPY START — base bdev이 직접 매핑한 buffer를 사용자에게 노출하는 zero-copy 모드.
		 *  populate=true면 read 의도, false면 write 의도. iov=NULL,iovcnt=0 — base가 채움.
		 *  완료 시 bdev_part_complete_io가 base가 매핑한 iov[0]을 partition IO에 set. */
		break;
	case SPDK_BDEV_IO_TYPE_COMPARE:
		if (!bdev_io->u.bdev.md_buf) {
			/* [한국어] separate md 없음 — md 미사용 또는 interleaved. 일반 comparev 사용. */
			rc = spdk_bdev_comparev_blocks(base_desc, base_ch,
						       bdev_io->u.bdev.iovs,
						       bdev_io->u.bdev.iovcnt,
						       remapped_offset,
						       bdev_io->u.bdev.num_blocks,
						       bdev_part_complete_io, bdev_io);
			/* [한국어] base에 COMPARE 발행 — 디바이스가 base LBA의 데이터와 iov 내용을 비교,
			 *  불일치 시 SPDK_NVME_SC_COMPARE_FAILURE로 실패. */
		} else {
			/* [한국어] separate md 있음 — md 포함 비교. */
			rc = spdk_bdev_comparev_blocks_with_md(base_desc, base_ch,
							       bdev_io->u.bdev.iovs,
							       bdev_io->u.bdev.iovcnt,
							       bdev_io->u.bdev.md_buf,
							       remapped_offset,
							       bdev_io->u.bdev.num_blocks,
							       bdev_part_complete_io, bdev_io);
			/* [한국어] md_buf까지 함께 비교 — separate metadata 형식 디바이스 케이스. */
		}
		break;
	case SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE:
		rc = spdk_bdev_comparev_and_writev_blocks(base_desc, base_ch, bdev_io->u.bdev.iovs,
				bdev_io->u.bdev.iovcnt,
				bdev_io->u.bdev.fused_iovs,
				bdev_io->u.bdev.fused_iovcnt,
				remapped_offset,
				bdev_io->u.bdev.num_blocks,
				bdev_part_complete_io, bdev_io);
		/* [한국어] FUSED Compare-and-Write (atomic CAS-like 연산). NVMe Fused 명령 1+2.
		 *  iovs=비교 데이터, fused_iovs=일치 시 쓸 데이터. atomic 보장. */
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		remapped_src_offset = bdev_io->u.bdev.copy.src_offset_blocks + part->internal.offset_blocks;
		/* [한국어] COPY는 src와 dst 두 LBA를 가지므로 src도 partition→base 변환 필요.
		 *  partition 내부 copy이므로 src/dst 둘 다 같은 partition offset을 더함. */
		rc = spdk_bdev_copy_blocks(base_desc, base_ch, remapped_offset, remapped_src_offset,
					   bdev_io->u.bdev.num_blocks, bdev_part_complete_io,
					   bdev_io);
		/* [한국어] base에 COPY 발행 — NVMe Copy opcode 0x19. dst, src 순서 주의. */
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_UNCORRECTABLE:
		rc = spdk_bdev_write_uncorrectable_blocks(base_desc, base_ch, remapped_offset,
				bdev_io->u.bdev.num_blocks, bdev_part_complete_io, bdev_io);
		/* [한국어] WRITE_UNCORRECTABLE — 디바이스에 의도적으로 uncorrectable 마커 기록 (테스트용).
		 *  NVMe Write Uncorrectable opcode 0x04. */
		break;
	default:
		SPDK_ERRLOG("unknown I/O type %d\n", bdev_io->type);
		/* [한국어] 본 switch에 없는 I/O 타입 — partition은 이를 지원하지 않음.
		 *  (NVMe passthrough 타입은 io_type_supported에서 이미 false로 보고했으므로 도달 불가
		 *   — 도달했다면 호출자 버그). */
		return SPDK_BDEV_IO_STATUS_FAILED;
		/* [한국어] 즉시 FAILED — bdev.c가 사용자 cb를 실패로 호출. */
	}

	return rc;
	/* [한국어] base 호출 결과 그대로 반환. 0=정상 제출, 음수=즉시 실패. */
}

/*
 * [한국어]
 * spdk_bdev_part_submit_request - cb 인자 없는 단순 진입점.
 *
 * @ch:      partition io_channel ctx.
 * @bdev_io: 사용자 I/O.
 * @return:  spdk_bdev_part_submit_request_ext 위임 결과.
 *
 * cb=NULL로 _ext 호출. 일반 partition 모듈은 split를 쓰지 않으므로 이 진입점을 사용.
 */
int
spdk_bdev_part_submit_request(struct spdk_bdev_part_channel *ch, struct spdk_bdev_io *bdev_io)
{
	return spdk_bdev_part_submit_request_ext(ch, bdev_io, NULL);
	/* [한국어] cb=NULL 모드로 위임 — completion에서 base status를 partition IO에 직접 복사. */
}

/*
 * [한국어]
 * bdev_part_channel_create_cb - partition io_channel 생성 콜백 (spdk_io_device_register에 등록).
 *
 * @io_device: spdk_io_device_register에 넘긴 partition 포인터.
 * @ctx_buf:   thread별로 할당된 채널 ctx (=spdk_bdev_part_channel 또는 그 파생).
 * @return:    0=성공, 음수=실패 (이 thread에 대한 채널 생성 실패).
 *
 * 해당 thread에 base bdev io_channel을 새로 만들어 ch->base_ch에 저장 → 이후 모든
 * I/O remap 시 이 base_ch를 사용 (스레드 affinity 보장). 모듈이 ch_create_cb를 등록한
 * 경우 그것도 호출해 모듈 추가 초기화 위임.
 */
static int
bdev_part_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_part *part = (struct spdk_bdev_part *)io_device;
	/* [한국어] io_device 등록 시 사용한 partition 포인터 복원. */
	struct spdk_bdev_part_channel *ch = ctx_buf;
	/* [한국어] thread-local 채널 ctx — channel_size 만큼 spdk_thread가 할당해 줌. */

	ch->part = part;
	/* [한국어] 채널 → partition 역참조 — submit_request에서 사용. */
	ch->base_ch = spdk_bdev_get_io_channel(part->internal.base->desc);
	/* [한국어] **핵심**: 같은 thread에서 base bdev에 대한 io_channel을 가져옴.
	 *  이후 partition I/O는 이 base_ch로 발행 → base I/O 완료 콜백도 같은 thread에서 호출되므로
	 *  partition 완료가 자연스럽게 thread affinity를 유지. */
	if (ch->base_ch == NULL) {
		return -1;
		/* [한국어] base bdev이 이 thread에서 채널을 생성할 수 없음 (메모리 부족 등) — 실패 전파. */
	}

	if (part->internal.base->ch_create_cb) {
		/* [한국어] 모듈이 채널 추가 초기화를 원하면 콜백 호출 (예: 모듈 자기 채널 ctx 초기화).
		 *  반환값을 그대로 전파해 부분 실패 시그널링도 가능. */
		return part->internal.base->ch_create_cb(io_device, ctx_buf);
	} else {
		/* [한국어] 모듈 콜백 없음 — 본 함수가 한 일만으로 충분. */
		return 0;
	}
}

/*
 * [한국어]
 * bdev_part_channel_destroy_cb - partition io_channel 파괴 콜백.
 *
 * @io_device: partition 포인터.
 * @ctx_buf:   파괴되는 채널 ctx.
 *
 * 모듈 ch_destroy_cb 호출 후 base_ch 반납. 순서 중요 — 모듈이 base_ch를 참조 중일 수
 * 있으므로 모듈 정리가 먼저, 그 다음 spdk_put_io_channel.
 */
static void
bdev_part_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_part *part = (struct spdk_bdev_part *)io_device;
	/* [한국어] partition 포인터 복원. */
	struct spdk_bdev_part_channel *ch = ctx_buf;
	/* [한국어] 파괴되는 채널 ctx. */

	if (part->internal.base->ch_destroy_cb) {
		/* [한국어] 모듈 정리 콜백 호출 — 모듈이 base_ch를 사용 중이라면 여기서 정리. */
		part->internal.base->ch_destroy_cb(io_device, ctx_buf);
	}
	spdk_put_io_channel(ch->base_ch);
	/* [한국어] base bdev io_channel 반납 — refcount 감소, 0이면 base 채널 destroy 트리거. */
}

/*
 * [한국어]
 * bdev_part_base_event_cb - base bdev 이벤트 디스패처 (REMOVE만 처리).
 *
 * @type:      이벤트 종류 (REMOVE, RESIZE 등).
 * @bdev:     이벤트 발생한 base bdev.
 * @event_ctx: open 시 cookie로 등록한 part_base 포인터.
 *
 * spdk_bdev_open_ext 호출 시 본 함수를 이벤트 콜백으로 등록한다 (line 492). REMOVE
 * 이벤트가 오면 모듈이 등록한 remove_cb를 호출 — 모듈은 보통
 * spdk_bdev_part_base_hotremove로 자기 partition들을 모두 unregister.
 */
static void
bdev_part_base_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			void *event_ctx)
{
	struct spdk_bdev_part_base *base = event_ctx;
	/* [한국어] open 시 cookie로 넘긴 part_base 포인터 복원. */

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		/* [한국어] base bdev이 hot-remove 됨 (예: NVMe SSD 분리). 모든 partition 정리 필요. */
		base->remove_cb(base);
		/* [한국어] 모듈에 위임 — 보통 spdk_bdev_part_base_hotremove로 partition 일괄 unregister. */
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		/* [한국어] RESIZE 등은 현재 partition 추상화에서 일반적으로 처리하지 않음 — 로그만 남김.
		 *  필요 시 모듈에서 별도 핸들링 가능. */
		break;
	}
}

/*
 * [한국어]
 * spdk_bdev_part_base_construct_ext - base bdev 1개를 open/claim하고 part_base 객체 생성.
 *
 * @bdev_name:    base bdev 이름 (찾기 키).
 * @remove_cb:    base hot-remove 시 모듈에 알릴 콜백.
 * @module:       이 partition을 만드는 bdev 모듈 ID.
 * @fn_table:     partition들이 사용할 fn_table — 본 함수가 get_io_channel/io_type_supported를 덮어씀.
 * @tailq:        모듈이 소유한 partition tailq head.
 * @free_fn:      part_base 해제 시 모듈 cleanup 콜백.
 * @ctx:          모듈 ctx (free_fn에 그대로 전달).
 * @channel_size: partition io_channel ctx 바이트 크기 (모듈이 spdk_bdev_part_channel을 서브클래싱 시).
 * @ch_create_cb: 채널 생성 추가 콜백 (NULL 가능).
 * @ch_destroy_cb: 채널 파괴 추가 콜백 (NULL 가능).
 * @_base:        [out] 생성된 part_base 포인터.
 * @return:       0=성공, 음수=실패. -ENODEV는 base bdev 미존재로 free 후 즉시 반환,
 *                기타 실패는 spdk_bdev_part_base_free로 풀 cleanup 후 반환.
 *
 * 호출 체인:
 *   모듈 examine_disk 또는 RPC 핸들러 → [이 함수] → spdk_bdev_open_ext + 모든 callback 설정
 *     → 이후 spdk_bdev_part_construct로 partition들을 N개 등록
 */
int
spdk_bdev_part_base_construct_ext(const char *bdev_name,
				  spdk_bdev_remove_cb_t remove_cb, struct spdk_bdev_module *module,
				  struct spdk_bdev_fn_table *fn_table, struct bdev_part_tailq *tailq,
				  spdk_bdev_part_base_free_fn free_fn, void *ctx,
				  uint32_t channel_size, spdk_io_channel_create_cb ch_create_cb,
				  spdk_io_channel_destroy_cb ch_destroy_cb,
				  struct spdk_bdev_part_base **_base)
{
	int rc;
	/* [한국어] 임시 반환 코드. */
	struct spdk_bdev_part_base *base;
	/* [한국어] 새로 할당할 part_base. */

	if (_base == NULL) {
		return -EINVAL;
		/* [한국어] out 파라미터 NULL — 호출자 버그. */
	}

	base = calloc(1, sizeof(*base));
	/* [한국어] 모든 필드 0 초기화로 시작 — pointer NULL/bool false/int 0이 안전 디폴트. */
	if (!base) {
		SPDK_ERRLOG("Memory allocation failure\n");
		return -ENOMEM;
		/* [한국어] heap 부족 — 즉시 반환. */
	}
	fn_table->get_io_channel = bdev_part_get_io_channel;
	/* [한국어] **모듈이 넘긴 fn_table에 강제 설정** — partition은 spdk_get_io_channel(part)
	 *  이라는 균일한 방식으로 채널을 발급받기 때문. 모듈이 다른 구현을 넣었다면 덮어써짐. */
	fn_table->io_type_supported = bdev_part_io_type_supported;
	/* [한국어] 마찬가지로 강제 설정 — NVMe passthrough 타입 차단 + 나머지는 base에 위임하는
	 *  공통 구현을 사용. */

	base->desc = NULL;
	/* [한국어] 아직 open 전 — open_ext 성공 시 채워짐. NULL 상태에서 free 시 close 시도 안 함. */
	base->ref = 0;
	/* [한국어] partition 0개 상태로 시작 — construct가 호출될 때마다 ++. */
	base->module = module;
	/* [한국어] claim에 사용할 모듈 ID. */
	base->fn_table = fn_table;
	/* [한국어] partition들이 공유할 fn_table 보관. */
	base->tailq = tailq;
	/* [한국어] partition list head. */
	base->base_free_fn = free_fn;
	/* [한국어] 최종 해제 시 모듈 콜백. */
	base->ctx = ctx;
	/* [한국어] 모듈 ctx. */
	base->claimed = false;
	/* [한국어] 아직 claim 안 한 상태 — 첫 partition construct에서 true 됨. */
	base->channel_size = channel_size;
	/* [한국어] 채널 ctx 크기 보관 — io_device_register에 전달. */
	base->ch_create_cb = ch_create_cb;
	/* [한국어] 채널 생성 시 모듈 콜백. */
	base->ch_destroy_cb = ch_destroy_cb;
	/* [한국어] 채널 파괴 시 모듈 콜백. */
	base->remove_cb = remove_cb;
	/* [한국어] base hot-remove 시 모듈 콜백. */

	rc = spdk_bdev_open_ext(bdev_name, false, bdev_part_base_event_cb, base, &base->desc);
	/* [한국어] base bdev open. write=false (read-only 모드 아님 — 두 번째 인자는 write_enabled,
	 *  partition 모듈은 read 전용으로 열지만 실제 R/W는 가능. 정확한 의미는 open_ext 정의 참조).
	 *  실제로 false는 "이 desc로 직접 write를 하지 않을 것"이라는 hint이며 base에 대한 권한을
	 *  partition들이 claim+module 통제로 관리. */
	if (rc) {
		if (rc == -ENODEV) {
			free(base);
			/* [한국어] base bdev 미존재 — 정상 케이스(아직 등록 안 됨)일 수 있어 단순 free.
			 *  spdk_bdev_part_base_free 호출하면 desc=NULL 가드는 통과하나 base_free_fn이
			 *  의도치 않게 호출될 수 있어 단순 free 사용. */
		} else {
			SPDK_ERRLOG("could not open bdev %s: %s\n", bdev_name, spdk_strerror(-rc));
			/* [한국어] 진짜 에러 — 권한, 이미 claim됨, 메모리 부족 등. 사람이 읽는 메시지로 로깅. */
			spdk_bdev_part_base_free(base);
			/* [한국어] 풀 cleanup — 모듈 ctx까지 정리. */
		}
		return rc;
		/* [한국어] 호출자에게 에러 코드 전달. */
	}

	base->bdev = spdk_bdev_desc_get_bdev(base->desc);
	/* [한국어] open 성공 — desc에서 실제 bdev 객체 추출해 보관. blocklen, md_len 등 속성 조회 시 사용. */

	/* Save the thread where the base device is opened */
	base->thread = spdk_get_thread();
	/* [한국어] **중요**: 현재 thread를 base owner thread로 기록. spdk_bdev_close는 동일 thread에서만
	 *  호출 가능하므로 free 시 cross-thread send_msg 라우팅에 사용. */

	*_base = base;
	/* [한국어] 호출자에게 결과 전달. */

	return 0;
	/* [한국어] 성공. */
}

/*
 * [한국어]
 * spdk_bdev_part_construct_opts_init - construct_opts 구조체 초기화 헬퍼.
 *
 * @opts: 초기화할 opts 구조체 (호출자 스택/힙).
 * @size: 호출자가 본 구조체 크기 (sizeof) — ABI versioning용.
 *
 * 모듈이 opts를 만들 때 호출. size를 opts->opts_size에 기록해 SPDK 내부 함수가
 * "이 호출자가 어느 버전의 opts를 알고 있는지"를 판단할 수 있게 한다.
 */
void
spdk_bdev_part_construct_opts_init(struct spdk_bdev_part_construct_opts *opts, uint64_t size)
{
	if (opts == NULL) {
		SPDK_ERRLOG("opts should not be NULL\n");
		assert(opts != NULL);
		/* [한국어] NULL 호출 — 호출자 버그. release 빌드에서는 ERRLOG만 남기고 조기 반환. */
		return;
	}
	if (size == 0) {
		SPDK_ERRLOG("size should not be zero\n");
		assert(size != 0);
		/* [한국어] size=0이면 ABI 추적 불가 — 호출자 버그. */
		return;
	}

	memset(opts, 0, size);
	/* [한국어] 모든 필드 0 — uuid는 null UUID (spdk_uuid_is_null=true)로 시작 → construct에서
	 *  자동 생성 트리거. */
	opts->opts_size = size;
	/* [한국어] 본 호출자가 보는 opts 크기 — 추후 SET_FIELD 매크로가 이를 보고 안전 복사. */
}

/*
 * [한국어]
 * part_construct_opts_copy - construct_opts를 src→dst로 ABI-safe 복사.
 *
 * @src: 호출자가 넘긴 opts.
 * @dst: 본 파일이 사용할 내부 사본.
 *
 * 호출자가 알고 있는 필드만(=opts_size 범위 안의 필드만) dst에 복사. SPDK가 진화하며
 * 새 필드가 추가되어도 이전 호출자는 자기가 모르는 새 필드 영역을 건드리지 않는다.
 */
static void
part_construct_opts_copy(const struct spdk_bdev_part_construct_opts *src,
			 struct spdk_bdev_part_construct_opts *dst)
{
	if (src->opts_size == 0) {
		SPDK_ERRLOG("size should not be zero\n");
		assert(false);
		/* [한국어] 호출자가 opts_init을 거치지 않고 직접 채운 잘못된 opts. */
	}

	memset(dst, 0, sizeof(*dst));
	/* [한국어] dst를 0으로 초기화 — 호출자가 모르는 필드는 안전 디폴트(0)로 유지. */
	dst->opts_size = src->opts_size;
	/* [한국어] 호출자가 본 크기 그대로 보존. */

#define FIELD_OK(field) \
        offsetof(struct spdk_bdev_part_construct_opts, field) + sizeof(src->field) <= src->opts_size
	/* [한국어] 매크로: 해당 필드의 마지막 바이트가 호출자가 본 opts_size 안에 들어오는지 확인.
	 *  들어오면 호출자가 그 필드를 알고 있다는 의미 → 복사 가능. */

#define SET_FIELD(field) \
        if (FIELD_OK(field)) { \
                dst->field = src->field; \
        } \
	/* [한국어] 매크로: FIELD_OK이면 dst->field에 src->field 복사. ABI-safe 패턴. */

	SET_FIELD(uuid);
	/* [한국어] 사용자가 명시적 UUID를 지정했으면 복사. 안 했으면 dst->uuid는 null UUID. */

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_part_construct_opts) == 24, "Incorrect size");
	/* [한국어] 컴파일타임 가드 — 구조체 크기를 24바이트(현재 uuid 16 + opts_size 8)로 못박아
	 *  새 필드 추가 시 누군가 SET_FIELD를 빠뜨리면 빌드가 깨지도록 함. 누군가 필드를 추가했다면
	 *  반드시 위 SET_FIELD도 같이 추가하고 이 assert의 24를 새 크기로 갱신해야 함. */

#undef FIELD_OK
#undef SET_FIELD
	/* [한국어] 매크로 스코프 정리 — 다른 함수에서 재사용되지 않도록. */
}

/*
 * [한국어]
 * spdk_bdev_part_construct_ext - partition 1개를 spdk_bdev로 생성·등록.
 *
 * @part:          이미 calloc된 partition 객체 (모듈 소유, 본 함수가 내부 필드 채움).
 * @base:          construct_base로 만든 part_base.
 * @name:          partition bdev 이름.
 * @offset_blocks: base 안에서의 시작 LBA.
 * @num_blocks:    partition 크기 (블록 수).
 * @product_name:  product name 문자열.
 * @_opts:         optional UUID 등 추가 옵션 (NULL이면 자동 생성).
 * @return:        0=성공, 음수=실패 (실패 시 base ref/claim 롤백).
 *
 * 호출 체인:
 *   모듈 (예: GPT) → spdk_bdev_part_base_construct_ext 1번 → [이 함수] N번
 *     → spdk_io_device_register + spdk_bdev_register → SPDK bdev 시스템에 노출
 */
int
spdk_bdev_part_construct_ext(struct spdk_bdev_part *part, struct spdk_bdev_part_base *base,
			     char *name, uint64_t offset_blocks, uint64_t num_blocks,
			     char *product_name, const struct spdk_bdev_part_construct_opts *_opts)
{
	int rc;
	/* [한국어] 임시 반환 코드. */
	bool first_claimed = false;
	/* [한국어] 본 호출이 base를 처음 claim했는지 — 실패 롤백 시 claimed=false로 되돌리기 위함. */
	struct spdk_bdev_part_construct_opts opts;
	/* [한국어] ABI-safe 사본 — 호출자가 _opts=NULL을 넘겨도 기본값으로 채워짐. */
	struct spdk_uuid ns_uuid;
	/* [한국어] UUID v5 namespace — BDEV_PART_NAMESPACE_UUID 파싱 결과. */

	if (_opts == NULL) {
		spdk_bdev_part_construct_opts_init(&opts, sizeof(opts));
		/* [한국어] 호출자가 opts 미지정 — 본 파일이 보는 최신 크기로 init. UUID는 자동 생성. */
	} else {
		part_construct_opts_copy(_opts, &opts);
		/* [한국어] 호출자가 명시 — ABI-safe 복사로 가져옴. */
	}

	part->internal.bdev.blocklen = base->bdev->blocklen;
	/* [한국어] partition은 base와 동일한 블록 크기를 사용. */
	part->internal.bdev.blockcnt = num_blocks;
	/* [한국어] partition 크기 = 호출자가 지정한 num_blocks. */
	part->internal.offset_blocks = offset_blocks;
	/* [한국어] **핵심**: I/O remap 시 더해질 base 기준 시작 LBA. */

	part->internal.bdev.write_cache = base->bdev->write_cache;
	/* [한국어] write cache 보유 여부 — base 속성 그대로. */
	part->internal.bdev.write_unit_size = base->bdev->write_unit_size;
	/* [한국어] write unit size (atomic write granularity) — base 속성 상속. */
	part->internal.bdev.required_alignment = base->bdev->required_alignment;
	/* [한국어] DMA 정렬 요구사항 (예: 4KB 정렬) — base 속성 상속. */
	part->internal.bdev.ctxt = part;
	/* [한국어] fn_table 콜백의 ctxt 인자 — partition 자기 자신 (모듈은 이를 통해 자기 ctx로 복귀). */
	part->internal.bdev.module = base->module;
	/* [한국어] 모듈 ID 상속 — bdev 등록 시 모듈 정보로 사용. */
	part->internal.bdev.fn_table = base->fn_table;
	/* [한국어] base에서 정의한 fn_table 사용 (get_io_channel/io_type_supported는 본 파일이 강제 설정). */

	part->internal.bdev.numa = base->bdev->numa;
	/* [한국어] NUMA 노드 정보 상속 — DPDK memory pool 할당 정책에 영향. */

	part->internal.bdev.md_interleave = base->bdev->md_interleave;
	/* [한국어] metadata interleave 형식 상속. */
	part->internal.bdev.md_len = base->bdev->md_len;
	/* [한국어] 블록당 metadata 길이. */
	part->internal.bdev.dif_type = base->bdev->dif_type;
	/* [한국어] T10 DIF 타입 (0/1/2/3) 상속. */
	part->internal.bdev.dif_is_head_of_md = base->bdev->dif_is_head_of_md;
	/* [한국어] DIF가 metadata의 앞쪽인지 뒤쪽인지 — PI 위치 결정. */
	part->internal.bdev.dif_check_flags = base->bdev->dif_check_flags;
	/* [한국어] PI 검증 비트 (GUARD/APPTAG/REFTAG_CHECK) 상속. */

	part->internal.bdev.name = strdup(name);
	/* [한국어] partition 이름을 별도 strdup — bdev 시스템이 free할 때까지 유효해야 함. */
	if (part->internal.bdev.name == NULL) {
		SPDK_ERRLOG("Failed to allocate name for new part of bdev %s\n", spdk_bdev_get_name(base->bdev));
		return -1;
		/* [한국어] 메모리 부족 — 다른 필드는 이미 채웠으나 등록 전이므로 단순 -1 반환. */
	}

	part->internal.bdev.product_name = strdup(product_name);
	/* [한국어] product name도 별도 strdup. */
	if (part->internal.bdev.product_name == NULL) {
		free(part->internal.bdev.name);
		/* [한국어] 위에서 할당한 name 누수 방지 cleanup. */
		SPDK_ERRLOG("Failed to allocate product name for new part of bdev %s\n",
			    spdk_bdev_get_name(base->bdev));
		return -1;
	}

	/* The caller may have already specified a UUID.  If not, we'll generate one
	 * based on the namespace UUID, the base bdev's UUID and the block range of the
	 * partition.
	 */
	if (!spdk_uuid_is_null(&opts.uuid)) {
		/* [한국어] 호출자가 명시적 UUID 지정 (opts.uuid가 non-null) — 그대로 사용. */
		spdk_uuid_copy(&part->internal.bdev.uuid, &opts.uuid);
	} else {
		/* [한국어] UUID 미지정 — 결정론적 생성. 같은 base+offset+num_blocks라면 항상 같은 UUID. */
		struct {
			struct spdk_uuid	uuid;
			uint64_t		offset_blocks;
			uint64_t		num_blocks;
		} base_name;
		/* [한국어] SHA-1 입력으로 쓸 binary blob — base UUID + offset + num_blocks 패킹.
		 *  스택 변수 — 함수 종료 시 자동 해제. */

		/* We need to create a unique base name for this partition.  We can't just use
		 * the base bdev's UUID, since it may be used for multiple partitions.  So
		 * construct a binary name consisting of the uuid + the block range for this
		 * partition.
		 */
		spdk_uuid_copy(&base_name.uuid, &base->bdev->uuid);
		/* [한국어] base bdev UUID 16바이트 복사. */
		base_name.offset_blocks = offset_blocks;
		/* [한국어] partition 시작 LBA — 같은 base의 다른 partition과 구별. */
		base_name.num_blocks = num_blocks;
		/* [한국어] partition 크기 — 동일 시작 LBA지만 크기가 다른 경우도 구별. */

		spdk_uuid_parse(&ns_uuid, BDEV_PART_NAMESPACE_UUID);
		/* [한국어] 하드코딩 namespace UUID 문자열을 spdk_uuid로 파싱. */
		rc = spdk_uuid_generate_sha1(&part->internal.bdev.uuid, &ns_uuid,
					     (const char *)&base_name, sizeof(base_name));
		/* [한국어] RFC 4122 v5 (SHA-1) UUID 생성: hash(ns_uuid || base_name) → partition uuid.
		 *  같은 입력 → 같은 출력 → 재시작 시에도 일관된 UUID 보장. */
		if (rc) {
			SPDK_ERRLOG("Could not generate new UUID\n");
			free(part->internal.bdev.name);
			free(part->internal.bdev.product_name);
			/* [한국어] UUID 생성 실패 — 위에서 할당한 모든 자원 cleanup. */
			return -1;
		}
	}

	base->ref++;
	/* [한국어] base가 이 partition에 의해 추가 참조됨 — 마지막 partition free 시 release를 위해 추적. */
	part->internal.base = base;
	/* [한국어] partition → base 역참조 보관 (모든 I/O remap에 필수). */

	if (!base->claimed) {
		/* [한국어] 첫 번째 partition construct — 아직 base를 claim하지 않은 상태. */
		int rc;

		rc = spdk_bdev_module_claim_bdev(base->bdev, base->desc, base->module);
		/* [한국어] base bdev에 대해 module 차원 claim. claim 후 다른 모듈이 동일 base를 마운트
		 *  못하게 막음 (배타적 소유권). */
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(base->bdev));
			free(part->internal.bdev.name);
			free(part->internal.bdev.product_name);
			base->ref--;
			/* [한국어] 위에서 ++한 ref를 롤백 + 할당 자원 모두 free. */
			return -1;
		}
		base->claimed = true;
		/* [한국어] claim 성공 — 다음 partition들은 이 분기를 건너뜀. */
		first_claimed = true;
		/* [한국어] 본 호출이 처음 claim한 케이스 — 아래 등록 실패 시 release 후 claimed=false 복원에 필요. */
	}

	spdk_io_device_register(part, bdev_part_channel_create_cb,
				bdev_part_channel_destroy_cb,
				base->channel_size,
				name);
	/* [한국어] partition을 io_device로 등록 — 이후 spdk_get_io_channel(part) 호출 시 본 함수가
	 *  thread-local 채널 ctx를 할당해 create_cb 호출. name은 디버깅용 식별자. */

	rc = spdk_bdev_register(&part->internal.bdev);
	/* [한국어] 마지막 단계: SPDK bdev 시스템에 partition 등록 → 사용자 공간에 노출.
	 *  등록 후 사용자가 spdk_bdev_open_ext(name, ...)로 이 partition을 사용할 수 있게 됨. */
	if (rc == 0) {
		TAILQ_INSERT_TAIL(base->tailq, part, tailq);
		/* [한국어] 등록 성공 — 모듈의 partition 리스트에 추가 (hot-remove 시 순회 대상). */
	} else {
		spdk_io_device_unregister(part, NULL);
		/* [한국어] 등록 실패 — io_device unregister로 위에서 register한 것 롤백. cb=NULL: 동기 정리. */
		if (--base->ref == 0) {
			spdk_bdev_module_release_bdev(base->bdev);
			/* [한국어] 본 partition이 유일한 참조였다면 claim 해제. base는 살려둠 (재시도 가능). */
		}
		free(part->internal.bdev.name);
		free(part->internal.bdev.product_name);
		/* [한국어] 할당 자원 정리. */
		if (first_claimed == true) {
			base->claimed = false;
			/* [한국어] 본 호출이 처음 claim한 케이스라면 claimed 플래그도 복원.
			 *  release_bdev 호출과 짝. */
		}
	}

	return rc;
	/* [한국어] register 결과 그대로 반환. */
}

/*
 * [한국어]
 * spdk_bdev_part_construct - opts 없는 단순 진입점.
 *
 * @part/base/name/offset_blocks/num_blocks/product_name: _ext와 동일.
 *
 * opts=NULL로 _ext 위임 — UUID는 결정론적으로 자동 생성.
 */
int
spdk_bdev_part_construct(struct spdk_bdev_part *part, struct spdk_bdev_part_base *base,
			 char *name, uint64_t offset_blocks, uint64_t num_blocks,
			 char *product_name)
{
	return spdk_bdev_part_construct_ext(part, base, name, offset_blocks, num_blocks,
					    product_name, NULL);
	/* [한국어] opts=NULL — UUID 자동 생성 모드. 대부분의 모듈이 이 진입점 사용. */
}
