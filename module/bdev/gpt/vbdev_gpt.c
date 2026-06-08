/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * This driver reads a GPT partition table from a bdev and exposes a virtual block device for
 * each partition.
 */

/*
 * [한국어 설명] GPT 가상 bdev 모듈 (vbdev_gpt.c)
 *
 * === 파일의 역할 ===
 * SPDK 의 examine 메커니즘을 통해 새로 등록된 bdev (예: NVMe namespace, malloc bdev) 의
 * 앞쪽 LBA 들을 읽어 GPT (GUID Partition Table) 파티션 테이블을 탐지하고, 발견된 각
 * 파티션을 별개의 spdk_bdev_part 로 등록하여 SPDK 상위 계층이 "베이스 bdev 이름 + pN"
 * (예: Nvme0n1p1, Nvme0n1p2) 형태의 가상 디바이스로 다룰 수 있도록 한다. 본 파일은
 * (a) SPDK bdev module 의 examine 콜백, (b) primary GPT 파싱 실패 시 secondary 백업
 * 테이블로 폴백, (c) spdk_bdev_part 의 콜백(IO submit/dump/destruct/get_memory_domains)
 * 을 구현한다. GPT 파싱 자체는 gpt.c 에 위임.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [lib/bdev]
 *     bdev 등록 → spdk_bdev_examine_disk 콜백 호출
 *       → vbdev_gpt_examine
 *       → vbdev_gpt_read_gpt → spdk_bdev_read(LBA 0~) → gpt_bdev_complete
 *       → gpt_parse_mbr / gpt_parse_partition_table
 *       → 실패 시: vbdev_gpt_read_secondary_table → gpt_read_secondary_table_complete
 *       → 성공 시: vbdev_gpt_create_bdevs → spdk_bdev_part_construct_ext 반복
 *   [상위 사용자]
 *     - "Nvme0n1p1" 같은 part bdev 를 open → spdk_bdev_part 가 base bdev 로 I/O 포워딩.
 * 실행 컨텍스트: 호스트 유저스페이스 SPDK app thread (examine 콜백 흐름).
 *
 * === 타 모듈과의 연결 ===
 * - lib/bdev/bdev_part.* (spdk_bdev_part_*): 파티션을 가상 bdev 로 등록/관리하는 공용 인프라.
 *   이 모듈은 그 위에 GPT 인식 로직만 얹는 얇은 wrapper.
 * - gpt.c: 파싱 알고리즘 위임.
 * - spdk/bdev_module.h: SPDK_BDEV_MODULE_REGISTER, examine 콜백 시그니처.
 * - spdk/bdev_part.h: spdk_bdev_part_base_construct_ext, part_submit_request, queue_io_wait.
 * - 데이터 흐름: bdev_read 가 채운 buf → gpt 파서 → partitions[] → bdev_part 인스턴스.
 *
 * === 주요 함수/구조체 요약 ===
 * - vbdev_gpt_examine: examine 진입점. blocksize/blockcnt 사전 검사 후 read 발사.
 * - vbdev_gpt_read_gpt / gpt_bdev_complete: primary GPT 읽기/파싱 후 파티션 생성.
 * - vbdev_gpt_read_secondary_table / gpt_read_secondary_table_complete: 백업 테이블 폴백.
 * - vbdev_gpt_create_bdevs: 검증된 파티션 엔트리에서 spdk_bdev_part 들을 생성.
 * - vbdev_gpt_submit_request / _vbdev_gpt_submit_request: 가상 bdev IO 처리.
 * - vbdev_gpt_get_buf_cb / vbdev_gpt_resubmit_request / vbdev_gpt_queue_io: 버퍼/큐잉.
 * - gpt_base / gpt_disk / gpt_channel / gpt_io: 모듈 내부 상태 구조체.
 */

#define REGISTER_GUID_DEPRECATION  /* [한국어] gpt.h 내부에서 deprecated 매크로/함수를 어떻게 노출할지 조정하는 토글. 본 파일이 SPDK_GPT_PART_TYPE_GUID_OLD 같은 deprecated 심볼을 사용해야 하므로 정의. */
#include "gpt.h"                   /* [한국어] gpt.c 의 공개 API + spdk_gpt 구조체 + partition entry 구조. */

#include "spdk/endian.h"           /* [한국어] from_le16/32/64, from_be16/32/64 — GUID/엔트리 바이트 순서 변환. */
#include "spdk/env.h"              /* [한국어] spdk_zmalloc, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA — GPT 파싱 버퍼는 DMA-able 영역에서 할당. */
#include "spdk/thread.h"           /* [한국어] spdk_io_channel — bdev read 시 채널 획득. */
#include "spdk/rpc.h"              /* [한국어] (역사적 — RPC 직접 사용 없음, 호환성 인클루드). */
#include "spdk/string.h"           /* [한국어] spdk_sprintf_alloc — partition 이름 "%sp%d" 생성. */
#include "spdk/util.h"             /* [한국어] SPDK_CONTAINEROF, SPDK_COUNTOF, SPDK_STATIC_ASSERT. */

#include "spdk/bdev_module.h"      /* [한국어] SPDK_BDEV_MODULE_REGISTER, spdk_bdev_module, spdk_bdev_part_* 공개 API. */
#include "spdk/log.h"              /* [한국어] SPDK_ERRLOG/WARNLOG/DEBUGLOG. */

/* [한국어] gpt_if 구조체 초기화자에서 함수 포인터로 참조하기 위한 전방 선언들.
 * 이 세 함수는 모두 아래 gpt_if (spdk_bdev_module) 의 멤버로 등록되며, 실제 정의는 파일 하단에 있다.
 * C 는 위에서 아래로 심볼을 해석하므로, gpt_if 초기화 시점에 함수 주소를 알 수 있도록 미리 선언한다. */
static int vbdev_gpt_init(void);                    /* [한국어] 모듈 초기화 콜백 (.module_init). SPDK bdev 서브시스템 init 단계에서 1회 호출. */
static void vbdev_gpt_examine(struct spdk_bdev *bdev); /* [한국어] examine 콜백 (.examine_disk). 새 bdev 등록 시마다 호출되는 GPT 탐지 진입점. */
static int vbdev_gpt_get_ctx_size(void);            /* [한국어] per-bdev_io 컨텍스트 크기 보고 콜백 (.get_ctx_size). bdev 레이어가 bdev_io 뒤에 추가 할당할 driver_ctx 바이트 수. */

/* [한국어] 이 파일이 SPDK bdev 레이어에 자신을 "gpt" 모듈로 등록하기 위한 디스크립터.
 * SPDK 의 모든 bdev 모듈은 spdk_bdev_module 인스턴스를 정의하고 SPDK_BDEV_MODULE_REGISTER 로 등록한다.
 * bdev 코어는 새 bdev 가 나타날 때마다 등록된 모든 모듈의 .examine_disk 를 순차 호출하여
 * "이 디스크가 내 관할인가?" 를 묻는다 — 본 모듈은 그 디스크의 GPT 파티션 테이블을 검사한다.
 * 실행 컨텍스트: 모든 콜백은 bdev 코어가 호출하는 SPDK app thread 에서 실행된다. */
static struct spdk_bdev_module gpt_if = {
	.name = "gpt",                          /* [한국어] 모듈 식별 이름. RPC/로그/examine 디버깅에서 이 문자열로 모듈을 구분한다. */
	.module_init = vbdev_gpt_init,          /* [한국어] 모듈 1회 초기화 훅. 여기선 할 일이 없어 0 반환 stub. */
	.get_ctx_size = vbdev_gpt_get_ctx_size, /* [한국어] bdev_io 당 추가 컨텍스트(struct gpt_io) 크기를 bdev 코어에 알린다. */
	.examine_disk = vbdev_gpt_examine,      /* [한국어] 핵심 진입점 — 새 bdev 의 GPT 검사. examine_config 가 아닌 examine_disk 인 이유는 디스크 내용(LBA)을 실제로 읽어야 하기 때문. */

};
/* [한국어] constructor 시점(main 진입 전)에 gpt_if 를 bdev 코어의 모듈 리스트에 등록하는 매크로.
 * 링커 섹션/생성자 함수 트릭으로 동작하며, 별도 호출 없이 프로그램 시작 시 자동 등록된다. */
SPDK_BDEV_MODULE_REGISTER(gpt, &gpt_if)

/* Base block device gpt context */
/* [한국어] 하나의 베이스 bdev(예: Nvme0n1) 에 대한 GPT 파싱/파티션 관리 컨텍스트.
 * examine 시작 시 gpt_base_bdev_init 에서 1개 할당되고, 모든 파티션 생성이 끝나거나
 * 파티션이 하나도 없으면 해제된다. spdk_bdev_part 인프라의 "base" 역할을 담는다. */
struct gpt_base {
	struct spdk_gpt			gpt;
	/* [한국어] GPT 파싱 상태 전체(read 버퍼/sector_size/총 LBA/파싱된 header·partitions 포인터/parse_phase).
	 * 설정자: gpt_base_bdev_init 이 buf/sector_size/total_sectors/lba_start/lba_end 를 채우고,
	 *   gpt.c 의 gpt_parse_mbr/gpt_parse_partition_table 가 header/partitions 포인터를 설정.
	 * 읽는 자: vbdev_gpt_create_bdevs(파티션 엔트리 순회), vbdev_gpt_dump_info_json(GUID 출력).
	 * 값 범위: gpt.buf 는 DMA-able 버퍼(NULL 불가, 해제는 gpt_base_free). parse_phase 는 PRIMARY/SECONDARY.
	 * 동기화: 단일 examine 흐름(한 app thread)에서만 다뤄지므로 락 불필요. */

	struct spdk_bdev_part_base	*part_base;
	/* [한국어] 이 베이스 bdev 에 대한 spdk_bdev_part 인프라 핸들. base bdev 의 desc/io_channel/refcount 를 캡슐화.
	 * 설정자: gpt_base_bdev_init 의 spdk_bdev_part_base_construct_ext 가 마지막 인자로 채워줌.
	 * 읽는 자: 모든 함수가 base bdev/desc 조회(spdk_bdev_part_base_get_bdev/get_desc/get_ctx) 시 사용.
	 * 값 범위: 유효 포인터. 파티션 0개거나 에러 시 spdk_bdev_part_base_free 로 해제(이때 gpt_base_free 콜백 연쇄).
	 * 동기화: bdev_part 인프라가 내부적으로 base 의 refcount 를 관리. */

	SPDK_BDEV_PART_TAILQ		parts;
	/* [한국어] 이 base 에서 생성된 모든 gpt_disk(파티션 가상 bdev) 의 연결 리스트 헤드.
	 * 설정자: gpt_base_bdev_init 이 TAILQ_INIT, spdk_bdev_part_construct_ext 가 노드 추가.
	 * 읽는 자: hotremove 콜백(gpt_base_bdev_hotremove_cb)이 베이스 제거 시 전체 파티션 정리에 사용.
	 * 값 범위: 0개 이상의 gpt_disk.part 노드.
	 * 동기화: bdev_part 인프라/examine app thread 단일 컨텍스트에서만 변경. */

	/* This channel is only used for reading the partition table. */
	struct spdk_io_channel		*ch;
	/* [한국어] GPT 테이블을 읽기 위해 일시적으로 잡는 base bdev 의 I/O 채널.
	 * 설정자: vbdev_gpt_read_gpt 가 spdk_bdev_get_io_channel 로 획득.
	 * 읽는 자: spdk_bdev_read(primary/secondary) 발행 시 인자로 사용.
	 * 값 범위: read 진행 중에만 유효. 완료 콜백(gpt_bdev_complete/secondary_complete)에서 put 후 NULL 로 리셋.
	 * 동기화: 단일 app thread 전용 채널이므로 락 불필요(SPDK io_channel 은 스레드별 인스턴스). */
};

/* Context for each gpt virtual bdev */
/* [한국어] 하나의 GPT 파티션(가상 bdev, 예: Nvme0n1p1) 당 컨텍스트.
 * vbdev_gpt_create_bdevs 가 유효 파티션마다 1개씩 calloc 하고 spdk_bdev_part_construct_ext 로 등록.
 * 해제는 destruct 콜백(vbdev_gpt_destruct → spdk_bdev_part_free) 경로. */
struct gpt_disk {
	struct spdk_bdev_part	part;
	/* [한국어] spdk_bdev_part 인프라가 요구하는 임베디드 part 객체. 이 안에 실제 spdk_bdev 가 들어있다.
	 * 설정자: vbdev_gpt_create_bdevs 의 spdk_bdev_part_construct_ext(lba_start/size/이름/uuid).
	 * 읽는 자: destruct/dump_info_json/get_memory_domains 가 SPDK_CONTAINEROF 로 gpt_disk 복원 시 기준점.
	 * 값 범위: 등록 성공 후 유효. base 의 parts TAILQ 에 링크됨.
	 * 동기화: bdev_part 인프라가 IO 포워딩과 수명을 관리. */

	uint32_t		partition_index;
	/* [한국어] gpt->partitions[] 배열에서 이 파티션이 차지하는 0-based 인덱스.
	 * 설정자: vbdev_gpt_create_bdevs 가 등록 직후 d->partition_index = i 로 기록.
	 * 읽는 자: vbdev_gpt_dump_info_json 이 gpt->partitions[index] 로 type/unique GUID·이름 조회.
	 * 값 범위: 0 ~ num_partition_entries-1.
	 * 동기화: 생성 후 불변(read-only) — 별도 동기화 불필요. */
};

/* [한국어] 이 gpt 가상 bdev 의 per-thread I/O 채널 컨텍스트.
 * SPDK 는 가상 bdev 마다 스레드별 채널을 만들고, 그 안에 모듈 전용 컨텍스트(이 구조체)를 임베드한다.
 * 크기는 gpt_base_bdev_init 의 spdk_bdev_part_base_construct_ext(sizeof(struct gpt_channel)) 로 전달. */
struct gpt_channel {
	struct spdk_bdev_part_channel	part_ch;
	/* [한국어] base bdev 로 IO 를 포워딩하기 위한 bdev_part 채널(내부에 base_ch 보유).
	 * 설정자: bdev_part 인프라가 채널 생성 시 자동 초기화.
	 * 읽는 자: _vbdev_gpt_submit_request 가 spdk_bdev_part_submit_request 에 넘기고,
	 *   vbdev_gpt_queue_io 가 part_ch.base_ch 로 큐잉.
	 * 값 범위: 채널 수명 동안 유효.
	 * 동기화: 채널은 특정 spdk_thread 에 고정되어 그 스레드에서만 접근 → lockless. */
};

/* [한국어] 단일 bdev_io 처리에 필요한 임시 컨텍스트. bdev_io->driver_ctx 영역에 in-place 로 존재.
 * 크기는 vbdev_gpt_get_ctx_size 가 보고하며, bdev 코어가 bdev_io 뒤에 이 만큼 공간을 둔다.
 * 주 용도: -ENOMEM 으로 즉시 제출 실패한 IO 를 나중에 재제출하기 위한 정보 보관. */
struct gpt_io {
	struct spdk_io_channel *ch;
	/* [한국어] 이 IO 가 들어온 가상 bdev 채널. 재제출 시 _vbdev_gpt_submit_request 에 다시 넘겨야 함.
	 * 설정자: _vbdev_gpt_submit_request 가 -ENOMEM 경로에서 io->ch = _ch 로 저장.
	 * 읽는 자: vbdev_gpt_resubmit_request 가 _vbdev_gpt_submit_request(io->ch, ...) 호출 시 사용.
	 * 값 범위: 큐잉된 IO 한정 유효. 정상 즉시 제출 시엔 사용 안 됨.
	 * 동기화: IO 가 묶인 채널의 스레드에서만 접근 → lockless. */

	struct spdk_bdev_io *bdev_io;
	/* [한국어] 재제출 대상이 되는 원본 bdev_io 포인터.
	 * 설정자: _vbdev_gpt_submit_request 가 -ENOMEM 경로에서 io->bdev_io = bdev_io 로 저장.
	 * 읽는 자: vbdev_gpt_resubmit_request 가 재제출 시 사용.
	 * 값 범위: 완료(complete) 전까지 유효.
	 * 동기화: 단일 채널 스레드 컨텍스트. */

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	/* [한국어] base bdev 가 다시 IO 를 받을 여유가 생기면 콜백받기 위한 대기 엔트리(bdev_io_wait 메커니즘).
	 * 설정자: vbdev_gpt_queue_io 가 bdev/cb_fn(=vbdev_gpt_resubmit_request)/cb_arg 를 채워
	 *   spdk_bdev_queue_io_wait 에 등록.
	 * 읽는 자: bdev 코어가 자원 확보 시 이 엔트리의 cb_fn 을 호출.
	 * 값 범위: 큐잉된 IO 한정 사용.
	 * 동기화: base_ch 가 묶인 스레드 컨텍스트에서만 다뤄짐 → lockless. */
};

/*
 * [한국어]
 * gpt_base_free - gpt_base 컨텍스트와 그 DMA 버퍼를 해제하는 콜백.
 *
 * @ctx: gpt_base 포인터(void* 로 받음). spdk_bdev_part_base_construct_ext 에 cb 로 등록된 것.
 * @return: 없음.
 *
 * 동기: spdk_bdev_part_base 가 최종 해제(spdk_bdev_part_base_free)될 때, base 가 들고 있던
 *   모듈 전용 컨텍스트(gpt_base)를 정리할 훅이 필요하다. 이 함수가 그 역할을 한다.
 * 동작: GPT 파싱용 DMA 버퍼(spdk_zmalloc 으로 잡은 gpt.buf)를 spdk_free 로 반납하고,
 *   gpt_base 자체(calloc)를 free 한다.
 * 실행 컨텍스트: bdev_part 인프라가 호출하는 app thread. base 의 refcount 가 0 이 되는 시점.
 * 호출 체인: spdk_bdev_part_base_free → (등록된 cb) → [gpt_base_free].
 */
static void
gpt_base_free(void *ctx)
{
	struct gpt_base *gpt_base = ctx;          /* [한국어] void* 를 실제 타입으로 캐스팅. construct 시 등록한 cb_arg 가 그대로 전달됨. */

	spdk_free(gpt_base->gpt.buf);             /* [한국어] DMA-able 버퍼 해제. spdk_zmalloc 으로 잡았으므로 spdk_free 로 반납해야 함(일반 free 아님). */
	free(gpt_base);                           /* [한국어] gpt_base 구조체 자체 해제. calloc 으로 잡았으므로 free 사용. */
}

/*
 * [한국어]
 * gpt_base_bdev_hotremove_cb - 베이스 bdev 가 물리적으로 사라질 때 모든 파티션을 정리하는 콜백.
 *
 * @_part_base: spdk_bdev_part_base 포인터(void* 로 받음). construct 시 hotremove cb 로 등록됨.
 * @return: 없음.
 *
 * 동기: NVMe SSD hotplug-out 등으로 베이스 bdev 가 갑자기 제거되면, 그 위에 얹힌 파티션
 *   가상 bdev 들도 함께 무효화해야 한다. SPDK 의 hotremove 통지를 받아 처리하는 진입점.
 * 동작: part_base 의 컨텍스트(gpt_base)를 꺼내고, spdk_bdev_part_base_hotremove 로
 *   parts 리스트의 모든 파티션을 일괄 제거한다.
 * 실행 컨텍스트: bdev 코어가 base 제거를 통지하는 app thread.
 * 호출 체인: bdev hotplug 이벤트 → spdk_bdev_part_base hotremove dispatch → [gpt_base_bdev_hotremove_cb]
 *   → spdk_bdev_part_base_hotremove.
 */
static void
gpt_base_bdev_hotremove_cb(void *_part_base)
{
	struct spdk_bdev_part_base *part_base = _part_base;                       /* [한국어] void* 를 part_base 타입으로 복원. */
	struct gpt_base *gpt_base = spdk_bdev_part_base_get_ctx(part_base);       /* [한국어] base 에 묶어둔 모듈 컨텍스트(gpt_base) 조회 — parts 리스트 접근 위함. */

	spdk_bdev_part_base_hotremove(part_base, &gpt_base->parts);               /* [한국어] 이 base 의 모든 파티션 part 를 일괄 hotremove. 인프라가 각 part 의 unregister 를 처리. */
}

/* [한국어] 아래 vbdev_gpt_fn_table 초기화자에서 함수 포인터로 참조하기 위한 전방 선언들.
 * 이들은 각 gpt 파티션 가상 bdev 의 fn_table 콜백으로, 실제 정의는 파일 하단에 있다. */
static int vbdev_gpt_destruct(void *ctx);                                                    /* [한국어] 파티션 bdev 파괴 콜백. */
static void vbdev_gpt_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io); /* [한국어] 파티션 bdev IO 제출 콜백. */
static int vbdev_gpt_dump_info_json(void *ctx, struct spdk_json_write_ctx *w);               /* [한국어] 파티션 정보 JSON 덤프 콜백(RPC bdev_get_bdevs). */
static int vbdev_gpt_get_memory_domains(void *ctx, struct spdk_memory_domain **domains,
					int array_size);                                     /* [한국어] 메모리 도메인 질의 콜백(zero-copy/accel 경로). */

/* [한국어] gpt 파티션 가상 bdev 가 사용하는 spdk_bdev 의 함수 테이블(vtable).
 * spdk_bdev_part_construct_ext 에 직접 넘기지 않고, gpt_base_bdev_init 의 base construct 에 전달되어
 * 이 base 에서 파생되는 모든 파티션 bdev 가 공유한다. bdev 코어가 IO/관리 요청 시 이 멤버들을 디스패치. */
static struct spdk_bdev_fn_table vbdev_gpt_fn_table = {
	.destruct		= vbdev_gpt_destruct,         /* [한국어] bdev unregister 시 호출 — part 해제. */
	.submit_request		= vbdev_gpt_submit_request,   /* [한국어] 상위에서 온 read/write 등을 base bdev 로 포워딩. */
	.dump_info_json		= vbdev_gpt_dump_info_json,   /* [한국어] RPC 로 bdev 정보 조회 시 gpt 전용 필드(GUID/이름) 출력. */
	.get_memory_domains	= vbdev_gpt_get_memory_domains, /* [한국어] 이 bdev 가 지원하는 메모리 도메인을 상위에 보고(DMA 직접전송 가능 여부). */
};

/*
 * [한국어]
 * gpt_base_bdev_init - 베이스 bdev 하나에 대한 gpt_base 컨텍스트를 생성·초기화한다.
 *
 * @bdev: GPT 를 검사할 베이스 블록 디바이스(examine 대상). blocklen/blockcnt 등을 읽는다.
 * @return: 초기화된 gpt_base 포인터(성공) 또는 NULL(메모리/construct 실패).
 *
 * 동기: GPT 테이블을 읽고 파싱하려면 (1) bdev_part base 핸들, (2) DMA-able read 버퍼,
 *   (3) 디스크 기하 정보(sector_size/total_sectors)가 필요하다. 이 함수가 한 번에 준비한다.
 * 동작 단계:
 *   1) gpt_base 구조체 zero-할당.
 *   2) parts TAILQ 초기화 후 spdk_bdev_part_base_construct_ext 로 base 핸들 생성(이름/hotremove/fn_table 등록).
 *   3) parse_phase=PRIMARY, buf_size 결정(최소 SPDK_GPT_BUFFER_SIZE, 단 블록 1개보다 작지 않게),
 *      blocklen 정렬로 DMA 버퍼 할당.
 *   4) 디스크 기하 정보(sector_size/total_sectors/lba_start/lba_end) 기록.
 * 실행 컨텍스트: examine app thread(vbdev_gpt_read_gpt 호출 경로).
 * 호출 체인: vbdev_gpt_examine → vbdev_gpt_read_gpt → [gpt_base_bdev_init]
 *   → spdk_bdev_part_base_construct_ext / spdk_zmalloc.
 * 에러 처리: construct 실패 시 gpt_base free 후 NULL, 버퍼 할당 실패 시 part_base_free(→ gpt_base_free 연쇄) 후 NULL.
 */
static struct gpt_base *
gpt_base_bdev_init(struct spdk_bdev *bdev)
{
	struct gpt_base *gpt_base;            /* [한국어] 새로 만들 base 컨텍스트. */
	struct spdk_gpt *gpt;                 /* [한국어] gpt_base->gpt 에 대한 단축 포인터(가독성). */
	int rc;                               /* [한국어] construct 반환 코드 임시 보관. */

	gpt_base = calloc(1, sizeof(*gpt_base)); /* [한국어] zero-초기화 할당. parts/ch/part_base 등을 0/NULL 로 시작. */
	if (!gpt_base) {                         /* [한국어] OOM 방어. */
		SPDK_ERRLOG("Cannot alloc memory for gpt_base pointer\n"); /* [한국어] 할당 실패 로그. */
		return NULL;                     /* [한국어] 호출자(vbdev_gpt_read_gpt)가 examine_done 으로 마무리. */
	}

	TAILQ_INIT(&gpt_base->parts);            /* [한국어] 파티션 리스트 헤드 초기화. construct 에 빈 리스트를 넘기기 위함. */
	/* [한국어] bdev_part 인프라에 이 base 를 등록. 인자: base 이름(=베이스 bdev 이름), hotremove cb,
	 *   소유 모듈(gpt_if), 파티션 공유 fn_table, parts 리스트, base 해제 cb(gpt_base_free)+cb_arg,
	 *   채널 컨텍스트 크기(struct gpt_channel), 나머지 옵션 NULL, 결과 핸들 출력(&part_base).
	 *   이 호출이 내부적으로 베이스 bdev 를 open(desc 획득)하고 refcount 를 잡는다. */
	rc = spdk_bdev_part_base_construct_ext(spdk_bdev_get_name(bdev),
					       gpt_base_bdev_hotremove_cb,
					       &gpt_if, &vbdev_gpt_fn_table,
					       &gpt_base->parts, gpt_base_free, gpt_base,
					       sizeof(struct gpt_channel), NULL, NULL, &gpt_base->part_base);
	if (rc != 0) {                           /* [한국어] base open/등록 실패(이름 충돌, desc 획득 실패 등). */
		free(gpt_base);                  /* [한국어] 아직 base 가 없으므로 gpt_base 만 직접 free(gpt_base_free 연쇄 불가). */
		SPDK_ERRLOG("cannot construct gpt_base"); /* [한국어] 실패 로그. */
		return NULL;                     /* [한국어] 실패 전파. */
	}

	gpt = &gpt_base->gpt;                    /* [한국어] 이후 필드 설정을 위한 단축 포인터. */
	gpt->parse_phase = SPDK_GPT_PARSE_PHASE_PRIMARY; /* [한국어] 처음엔 디스크 앞쪽 primary GPT 를 시도(실패 시 secondary 폴백). */
	/* [한국어] read 버퍼 크기 결정: GPT 헤더+엔트리를 담는 표준 크기(SPDK_GPT_BUFFER_SIZE)와
	 *   블록 1개 크기 중 큰 값. 4Kn 디바이스(blocklen>버퍼)에서도 최소 1블록은 담도록. */
	gpt->buf_size = spdk_max(SPDK_GPT_BUFFER_SIZE, bdev->blocklen);
	/* [한국어] DMA 전송 대상이므로 spdk_zmalloc 으로 hugepage 기반 DMA-able 메모리 할당.
	 *   정렬은 베이스 bdev 가 요구하는 버퍼 정렬(spdk_bdev_get_buf_align), NUMA/lcore 무관(ANY), DMA 플래그. */
	gpt->buf = spdk_zmalloc(gpt->buf_size, spdk_bdev_get_buf_align(bdev), NULL,
				SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!gpt->buf) {                         /* [한국어] DMA 버퍼 OOM 방어. */
		SPDK_ERRLOG("Cannot alloc buf\n"); /* [한국어] 실패 로그. */
		spdk_bdev_part_base_free(gpt_base->part_base); /* [한국어] 이미 만든 base 해제 → gpt_base_free 콜백이 연쇄되어 gpt_base 도 정리됨. */
		return NULL;                     /* [한국어] 실패 전파(여기선 gpt_base 를 직접 free 하지 않음 — base_free 가 처리). */
	}

	gpt->sector_size = bdev->blocklen;       /* [한국어] LBA 1개의 바이트 크기. secondary offset/엔트리 계산에 사용. */
	gpt->total_sectors = bdev->blockcnt;     /* [한국어] 디스크 전체 LBA 개수. secondary GPT 위치 계산의 기준. */
	gpt->lba_start = 0;                      /* [한국어] 파서가 다룰 LBA 범위 시작(전체 디스크이므로 0). */
	gpt->lba_end = gpt->total_sectors - 1;   /* [한국어] 파서가 다룰 LBA 범위 끝(마지막 LBA). 파티션 경계 검증에 사용. */

	return gpt_base;                         /* [한국어] 준비 완료된 컨텍스트 반환 — 이후 read 발행에 사용. */
}

/*
 * [한국어]
 * vbdev_gpt_destruct - gpt 파티션 가상 bdev 파괴 콜백(.destruct).
 *
 * @ctx: gpt_disk 포인터(bdev unregister 시 bdev 코어가 넘기는 module ctx).
 * @return: spdk_bdev_part_free 결과(0=완료, 그 외=비동기 진행 중 의미는 인프라 정의).
 *
 * 동기: 파티션 bdev 가 unregister 될 때 임베디드 part 객체와 gpt_disk 메모리를 정리해야 한다.
 * 동작: spdk_bdev_part_free 에 위임 — 인프라가 part 의 refcount/리스트 제거/메모리 해제를 처리.
 * 실행 컨텍스트: bdev unregister 흐름의 app thread.
 * 호출 체인: bdev unregister → fn_table.destruct → [vbdev_gpt_destruct] → spdk_bdev_part_free.
 */
static int
vbdev_gpt_destruct(void *ctx)
{
	struct gpt_disk *gpt_disk = ctx;                  /* [한국어] module ctx 를 gpt_disk 로 캐스팅. part 가 첫 멤버라 주소 동일. */

	return spdk_bdev_part_free(&gpt_disk->part);      /* [한국어] 인프라에 part 해제 위임. gpt_disk 자체 free 도 인프라가 처리. */
}

/* [한국어] vbdev_gpt_resubmit_request 가 참조하기 위한 IO 제출 실제 구현의 전방 선언.
 * 정의는 아래에 있으며, queue_io→resubmit 경로와 submit_request→get_buf 경로 양쪽에서 호출된다. */
static void _vbdev_gpt_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io);

/*
 * [한국어]
 * vbdev_gpt_resubmit_request - bdev_io_wait 콜백: 자원 여유가 생긴 후 큐잉됐던 IO 를 재제출.
 *
 * @arg: 큐잉 시 저장한 gpt_io 포인터(cb_arg).
 * @return: 없음.
 *
 * 동기: base bdev 가 -ENOMEM 으로 IO 를 못 받았을 때, 자원이 회복되면 같은 IO 를 다시 시도해야 한다.
 * 동작: 저장해둔 채널(io->ch)과 bdev_io(io->bdev_io)로 _vbdev_gpt_submit_request 를 재호출.
 * 실행 컨텍스트: bdev 코어가 io_wait 를 깨우는 시점의 app thread(원래 채널과 동일 스레드).
 * 호출 체인: spdk_bdev_queue_io_wait → (자원 회복) → [vbdev_gpt_resubmit_request] → _vbdev_gpt_submit_request.
 */
static void
vbdev_gpt_resubmit_request(void *arg)
{
	struct gpt_io *io = (struct gpt_io *)arg;         /* [한국어] cb_arg 를 gpt_io 로 복원 — 큐잉 시 보관한 ch/bdev_io 회수. */

	_vbdev_gpt_submit_request(io->ch, io->bdev_io);   /* [한국어] 동일 채널/IO 로 재제출. 또 -ENOMEM 이면 다시 큐잉될 수 있음. */
}

/*
 * [한국어]
 * vbdev_gpt_queue_io - -ENOMEM 으로 즉시 제출 실패한 IO 를 base bdev 의 io_wait 큐에 등록.
 *
 * @io: 재제출에 필요한 정보(ch/bdev_io)가 채워진 gpt_io.
 * @return: 없음.
 *
 * 동기: SPDK 의 bdev_io 풀이 일시 고갈되면 -ENOMEM 이 난다. 이때 실패시키지 않고 자원 회복 시
 *   콜백을 받도록 대기 엔트리를 등록하는 것이 표준 패턴이다(spdk_bdev_queue_io_wait).
 * 동작: bdev_io_wait 엔트리에 base bdev/재제출 cb/cb_arg 를 채우고 base 채널에 큐잉한다.
 * 실행 컨텍스트: IO 가 묶인 채널의 app thread.
 * 호출 체인: _vbdev_gpt_submit_request(-ENOMEM) → [vbdev_gpt_queue_io] → spdk_bdev_queue_io_wait.
 * 에러 처리: 큐잉 등록 자체가 실패하면 더는 방법이 없으므로 IO 를 FAILED 로 완료시킨다.
 */
static void
vbdev_gpt_queue_io(struct gpt_io *io)
{
	struct gpt_channel *ch = spdk_io_channel_get_ctx(io->ch); /* [한국어] 가상 bdev 채널에서 모듈 채널 컨텍스트 추출 — base_ch 가 필요. */
	int rc;                                                   /* [한국어] 큐잉 등록 결과. */

	io->bdev_io_wait.bdev = io->bdev_io->bdev;                /* [한국어] 어느 bdev 의 자원을 기다리는지 지정(베이스 bdev). */
	io->bdev_io_wait.cb_fn = vbdev_gpt_resubmit_request;      /* [한국어] 자원 회복 시 호출될 재제출 함수. */
	io->bdev_io_wait.cb_arg = io;                             /* [한국어] 콜백에 넘길 컨텍스트(자기 자신). */

	/* [한국어] 베이스 bdev 의 io_wait 큐에 등록. base 채널(part_ch.base_ch)에 묶어야 같은 스레드에서 콜백됨. */
	rc = spdk_bdev_queue_io_wait(io->bdev_io->bdev,
				     ch->part_ch.base_ch, &io->bdev_io_wait);
	if (rc != 0) {                                            /* [한국어] 큐잉 등록 실패(이론상 드묾) — 회복 불가. */
		SPDK_ERRLOG("Queue io failed in vbdev_gpt_queue_io, rc=%d.\n", rc); /* [한국어] 실패 로그. */
		spdk_bdev_io_complete(io->bdev_io, SPDK_BDEV_IO_STATUS_FAILED);     /* [한국어] 원본 IO 를 실패로 완료시켜 상위에 통지. */
	}
}

/*
 * [한국어]
 * vbdev_gpt_get_buf_cb - READ 용 버퍼 확보 후 실제 IO 제출로 이어주는 콜백.
 *
 * @ch: IO 가 묶인 가상 bdev 채널.
 * @bdev_io: 처리 중인 read 요청.
 * @success: 버퍼 확보 성공 여부(bdev 코어의 iobuf 풀 가용성).
 * @return: 없음.
 *
 * 동기: SPDK 에서 read 는 데이터를 담을 버퍼가 필요하다. 사용자가 버퍼를 안 줬을 수 있어
 *   bdev 코어의 풀에서 비동기로 확보하는데, 그 완료 통지를 받아 제출을 이어가야 한다.
 * 동작: 버퍼 확보 실패면 IO 를 FAILED 로 완료, 성공이면 _vbdev_gpt_submit_request 로 진행.
 * 실행 컨텍스트: 버퍼 확보 완료를 알리는 app thread(원래 채널 스레드).
 * 호출 체인: vbdev_gpt_submit_request(READ) → spdk_bdev_io_get_buf → [vbdev_gpt_get_buf_cb]
 *   → _vbdev_gpt_submit_request.
 */
static void
vbdev_gpt_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	if (!success) {                                                   /* [한국어] iobuf 풀 고갈 등으로 버퍼 확보 실패. */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED); /* [한국어] read 를 실패 처리하고 상위에 통지. */
		return;                                                  /* [한국어] 더 진행하지 않음. */
	}

	_vbdev_gpt_submit_request(ch, bdev_io);                          /* [한국어] 버퍼가 준비됐으니 실제 base 포워딩 제출로 이동. */
}

/*
 * [한국어]
 * _vbdev_gpt_submit_request - 파티션 IO 를 base bdev 로 포워딩하는 핵심 제출 함수.
 *
 * @_ch: 가상 bdev 채널(여기서 base_ch 를 꺼냄).
 * @bdev_io: 포워딩할 IO(read/write/unmap 등).
 * @return: 없음(완료는 비동기/io_wait 경로).
 *
 * 동기: gpt 파티션 bdev 는 실제 저장공간이 없고 base bdev 의 일부 LBA 범위에 매핑된다.
 *   따라서 모든 IO 를 offset 보정과 함께 base 로 넘겨야 한다. 그 위임을 bdev_part 인프라가 처리.
 * 동작: spdk_bdev_part_submit_request 로 base 제출 → 성공이면 비동기 완료에 맡기고,
 *   -ENOMEM 이면 io_wait 큐잉, 그 외 에러면 즉시 FAILED 완료.
 * 실행 컨텍스트: IO 가 묶인 채널의 app thread(lockless).
 * 호출 체인: vbdev_gpt_submit_request / vbdev_gpt_get_buf_cb / vbdev_gpt_resubmit_request
 *   → [_vbdev_gpt_submit_request] → spdk_bdev_part_submit_request (→ base bdev).
 */
static void
_vbdev_gpt_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct gpt_channel *ch = spdk_io_channel_get_ctx(_ch);          /* [한국어] 가상 채널에서 모듈 채널 컨텍스트(part_ch) 추출. */
	struct gpt_io *io = (struct gpt_io *)bdev_io->driver_ctx;       /* [한국어] bdev_io 뒤에 붙은 driver_ctx 를 gpt_io 로 해석 — 큐잉 시 ch/bdev_io 보관용. */
	int rc;                                                         /* [한국어] base 제출 결과. */

	rc = spdk_bdev_part_submit_request(&ch->part_ch, bdev_io);      /* [한국어] 인프라가 LBA offset 보정 후 base bdev 로 IO 발행. 0=수락. */
	if (rc) {                                                       /* [한국어] 제출이 즉시 거부된 경우. */
		if (rc == -ENOMEM) {                                    /* [한국어] bdev_io 풀 일시 고갈 — 회복 가능. */
			SPDK_DEBUGLOG(vbdev_gpt, "gpt: no memory, queue io\n"); /* [한국어] 큐잉 디버그 로그. */
			io->ch = _ch;                                   /* [한국어] 재제출에 쓸 채널 보관. */
			io->bdev_io = bdev_io;                          /* [한국어] 재제출에 쓸 IO 보관. */
			vbdev_gpt_queue_io(io);                         /* [한국어] io_wait 큐에 등록 — 자원 회복 시 resubmit. */
		} else {                                                /* [한국어] -ENOMEM 외의 영구적 에러. */
			SPDK_ERRLOG("gpt: error on bdev_io submission, rc=%d.\n", rc); /* [한국어] 에러 로그. */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);    /* [한국어] IO 를 실패로 완료. */
		}
	}
}

/*
 * [한국어]
 * vbdev_gpt_submit_request - 파티션 가상 bdev 의 IO 진입점(.submit_request).
 *
 * @_ch: IO 가 들어온 가상 bdev 채널.
 * @bdev_io: 상위에서 발행한 IO.
 * @return: 없음.
 *
 * 동기: bdev 코어가 이 파티션 bdev 로 보낸 모든 IO 가 여기로 들어온다. READ 는 데이터 버퍼가
 *   필요하므로 먼저 버퍼를 확보한 뒤 제출해야 하고, 그 외 타입은 곧바로 포워딩할 수 있다.
 * 동작: type 이 READ 면 spdk_bdev_io_get_buf 로 버퍼 확보(콜백에서 제출), 그 외는 즉시 포워딩.
 * 실행 컨텍스트: IO 가 묶인 채널의 app thread.
 * 호출 체인: bdev 코어 → fn_table.submit_request → [vbdev_gpt_submit_request]
 *   → spdk_bdev_io_get_buf / _vbdev_gpt_submit_request.
 */
static void
vbdev_gpt_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {                                        /* [한국어] IO 종류에 따라 버퍼 확보 필요 여부 분기. */
	case SPDK_BDEV_IO_TYPE_READ:                                    /* [한국어] READ: 읽은 데이터를 담을 버퍼가 필요. */
		/* [한국어] 필요한 버퍼 크기 = 블록 수 × 블록 길이. 확보되면 콜백에서 제출 진행. */
		spdk_bdev_io_get_buf(bdev_io, vbdev_gpt_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;                                                 /* [한국어] 버퍼 확보는 비동기 — 여기서 끝. */
	default:                                                       /* [한국어] write/unmap/flush 등: 버퍼 확보 불필요. */
		_vbdev_gpt_submit_request(_ch, bdev_io);               /* [한국어] 곧바로 base 로 포워딩. */
		break;
	}
}

/*
 * [한국어]
 * gpt_guid_to_uuid - GPT 의 16바이트 GUID(mixed-endian)를 SPDK spdk_uuid 로 변환.
 *
 * @guid: GPT 파티션 엔트리의 raw GUID(16바이트). part_type/unique GUID 둘 다 이 포맷.
 * @uuid: 출력 spdk_uuid(여기에 변환 결과를 in-place 로 채움).
 * @return: 없음.
 *
 * 동기: GPT GUID 는 RFC 4122 변형으로, 앞 3 필드(b0/b4/b6)는 big-endian, 뒤 2 필드(b8/b10)는
 *   little-endian, 마지막(b12)은 다시 little-endian 으로 저장된다(혼합 엔디언). spdk_uuid 가
 *   기대하는 바이트 배열로 정확히 맞춰야 partition uuid 가 올바르게 표기된다.
 * 동작: spdk_uuid 메모리를 6필드 구조체로 오버레이하고, 각 오프셋에서 적절한 엔디언 변환으로 채운다.
 * 실행 컨텍스트: vbdev_gpt_create_bdevs(파티션 등록 시 uuid 설정) — examine app thread.
 * 호출 체인: vbdev_gpt_create_bdevs → [gpt_guid_to_uuid] → from_be32/16, from_le16/32.
 */
static void
gpt_guid_to_uuid(const struct spdk_gpt_guid *guid, struct spdk_uuid *uuid)
{
#pragma pack(push, 1) /* [한국어] 아래 임시 구조체에 패딩이 끼지 않도록 1바이트 정렬 강제 — uuid 바이트 레이아웃과 정확히 일치시키기 위함. */
	struct tmp_uuid {
		uint32_t b0;  /* [한국어] time_low(바이트 0~3). GPT 에선 big-endian 저장 → from_be32. */
		uint16_t b4;  /* [한국어] time_mid(바이트 4~5). big-endian → from_be16. */
		uint16_t b6;  /* [한국어] time_hi_and_version(바이트 6~7). big-endian → from_be16. */
		uint16_t b8;  /* [한국어] clock_seq(바이트 8~9). little-endian → from_le16. */
		uint16_t b10; /* [한국어] node 상위(바이트 10~11). little-endian → from_le16. */
		uint32_t b12; /* [한국어] node 하위(바이트 12~15). little-endian → from_le32. */
	} *ret = (struct tmp_uuid *)uuid; /* [한국어] spdk_uuid 메모리를 이 레이아웃으로 재해석(같은 16바이트). */
#pragma pack(pop) /* [한국어] 정렬 설정 원복 — 이후 코드에는 영향 주지 않음. */
	SPDK_STATIC_ASSERT(sizeof(*ret) == sizeof(*uuid), "wrong size"); /* [한국어] 컴파일 타임에 두 타입이 정확히 16바이트로 동일함을 보장(오버레이 안전성). */

	ret->b0 = from_be32(&guid->raw[0]);   /* [한국어] 바이트 0~3 을 big-endian 해석해 호스트 정수로. */
	ret->b4 = from_be16(&guid->raw[4]);   /* [한국어] 바이트 4~5 big-endian 변환. */
	ret->b6 = from_be16(&guid->raw[6]);   /* [한국어] 바이트 6~7 big-endian 변환. */
	ret->b8 = from_le16(&guid->raw[8]);   /* [한국어] 바이트 8~9 little-endian 변환(엔디언 전환 지점). */
	ret->b10 = from_le16(&guid->raw[10]); /* [한국어] 바이트 10~11 little-endian 변환. */
	ret->b12 = from_le32(&guid->raw[12]); /* [한국어] 바이트 12~15 little-endian 변환. */
}

/*
 * [한국어]
 * write_guid - GPT GUID 를 사람이 읽는 표준 UUID 문자열로 JSON 에 출력.
 *
 * @w: JSON writer 컨텍스트(현재 값 위치에 문자열을 쓴다).
 * @guid: 출력할 GPT GUID(16바이트 raw).
 * @return: 없음.
 *
 * 동기: dump_info_json 에서 partition_type_guid/unique_partition_guid 를 사람이 읽을 수 있는
 *   "8-4-4-4-12" hex 형식으로 보여줘야 한다. 엔디언 규칙은 gpt_guid_to_uuid 와 동일하다.
 * 동작: 각 필드를 알맞은 엔디언으로 변환해 표준 UUID 포맷 문자열로 직렬화한다.
 * 실행 컨텍스트: RPC bdev_get_bdevs 처리 app thread.
 * 호출 체인: vbdev_gpt_dump_info_json → [write_guid] → spdk_json_write_string_fmt.
 */
static void
write_guid(struct spdk_json_write_ctx *w, const struct spdk_gpt_guid *guid)
{
	/* [한국어] "%08x-%04x-%04x-%04x-%04x%08x" 표준 UUID 표기. 앞 3필드 BE 와 동일하나,
	 *   포맷팅 목적상 b0/b4/b6 는 le 로(출력 자릿수 맞춤), b8/b10/b12 는 be 로 변환하여
	 *   사람이 읽는 표준 표기와 일치시킨다(엔디언 조합은 GPT 표기 관례를 따름). */
	spdk_json_write_string_fmt(w, "%08x-%04x-%04x-%04x-%04x%08x",
				   from_le32(&guid->raw[0]),   /* [한국어] 첫 8 hex 자리. */
				   from_le16(&guid->raw[4]),   /* [한국어] 두 번째 그룹. */
				   from_le16(&guid->raw[6]),   /* [한국어] 세 번째 그룹. */
				   from_be16(&guid->raw[8]),   /* [한국어] 네 번째 그룹. */
				   from_be16(&guid->raw[10]),  /* [한국어] 다섯 번째 그룹 상위. */
				   from_be32(&guid->raw[12])); /* [한국어] 다섯 번째 그룹 하위. */
}

/*
 * [한국어]
 * write_string_utf16le - UTF-16LE 파티션 이름을 NUL 까지의 길이만큼 JSON 문자열로 출력.
 *
 * @w: JSON writer 컨텍스트.
 * @str: UTF-16LE 코드유닛 배열(GPT partition_name 필드, 최대 max_len 유닛, NUL 패딩).
 * @max_len: 배열의 최대 길이(코드유닛 수).
 * @return: 없음.
 *
 * 동기: GPT 파티션 이름은 고정 36 UTF-16LE 유닛 필드이며 끝은 0 으로 패딩된다. JSON 출력 시
 *   실제 이름 길이(첫 0 이전까지)만 써야 trailing NUL 이 문자열에 들어가지 않는다.
 * 동작: 첫 0 코드유닛 또는 max_len 까지를 세어 실제 길이를 구한 뒤 raw UTF-16LE 출력 함수에 넘긴다.
 * 실행 컨텍스트: RPC bdev_get_bdevs 처리 app thread.
 * 호출 체인: vbdev_gpt_dump_info_json → [write_string_utf16le] → spdk_json_write_string_utf16le_raw.
 */
static void
write_string_utf16le(struct spdk_json_write_ctx *w, const uint16_t *str, size_t max_len)
{
	size_t len;            /* [한국어] 실제 문자열 길이(코드유닛 수). */
	const uint16_t *p;     /* [한국어] 스캔 커서. */

	/* [한국어] NUL(0) 코드유닛을 만나거나 max_len 에 도달할 때까지 길이를 센다 — 패딩 NUL 제외. */
	for (len = 0, p = str; len < max_len && *p; p++) {
		len++;         /* [한국어] 유효 코드유닛 1개 누적. */
	}

	spdk_json_write_string_utf16le_raw(w, str, len); /* [한국어] 실제 길이만큼 UTF-16LE → JSON 문자열(내부에서 UTF-8 변환). */
}

/*
 * [한국어]
 * vbdev_gpt_dump_info_json - 파티션 bdev 의 gpt 전용 메타데이터를 JSON 으로 출력(.dump_info_json).
 *
 * @ctx: gpt_disk 의 part 멤버 주소(bdev 코어가 넘기는 module ctx).
 * @w: JSON writer 컨텍스트.
 * @return: 0(항상 성공).
 *
 * 동기: RPC bdev_get_bdevs 응답에 이 파티션이 어느 base 의 어느 offset 에 위치하고, 어떤 GPT
 *   type/unique GUID 와 이름을 갖는지 노출해야 한다. 사용자/툴이 파티션을 식별하는 근거가 된다.
 * 동작: ctx→gpt_disk→base→gpt 체인으로 partitions[index] 엔트리를 찾아 "gpt" 객체를 쓴다
 *   (base_bdev/offset_blocks/type GUID/unique GUID/partition_name).
 * 실행 컨텍스트: RPC 처리 app thread.
 * 호출 체인: bdev_get_bdevs RPC → fn_table.dump_info_json → [vbdev_gpt_dump_info_json]
 *   → write_guid / write_string_utf16le / spdk_json_write_*.
 */
static int
vbdev_gpt_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct gpt_disk *gpt_disk = SPDK_CONTAINEROF(ctx, struct gpt_disk, part);           /* [한국어] part 멤버 주소에서 바깥 gpt_disk 복원. */
	struct spdk_bdev_part_base *base_bdev = spdk_bdev_part_get_base(&gpt_disk->part);   /* [한국어] 이 파티션이 속한 base 핸들 조회. */
	struct gpt_base *gpt_base = spdk_bdev_part_base_get_ctx(base_bdev);                 /* [한국어] base 에 묶인 gpt_base(파싱 결과) 조회. */
	struct spdk_bdev *part_base_bdev = spdk_bdev_part_base_get_bdev(base_bdev);         /* [한국어] 실제 베이스 bdev(이름 출력용). */
	struct spdk_gpt *gpt = &gpt_base->gpt;                                              /* [한국어] 파싱된 GPT 상태 단축 포인터. */
	struct spdk_gpt_partition_entry *gpt_entry = &gpt->partitions[gpt_disk->partition_index]; /* [한국어] 이 파티션의 GPT 엔트리(GUID/이름 출처). */
	uint64_t offset_blocks = spdk_bdev_part_get_offset_blocks(&gpt_disk->part);         /* [한국어] base 상에서 이 파티션의 시작 LBA(=starting_lba). */

	spdk_json_write_named_object_begin(w, "gpt");                                       /* [한국어] "gpt": { 객체 시작. */

	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(part_base_bdev));   /* [한국어] 어느 베이스 bdev 위 파티션인지. */

	spdk_json_write_named_uint64(w, "offset_blocks", offset_blocks);                    /* [한국어] base 기준 시작 블록 오프셋. */

	spdk_json_write_name(w, "partition_type_guid");                                     /* [한국어] 키만 먼저 쓰고 값은 write_guid 가 채움. */
	write_guid(w, &gpt_entry->part_type_guid);                                          /* [한국어] 파티션 타입 GUID 를 UUID 문자열로. */

	spdk_json_write_name(w, "unique_partition_guid");                                   /* [한국어] 고유 파티션 GUID 키. */
	write_guid(w, &gpt_entry->unique_partition_guid);                                   /* [한국어] 고유 GUID 를 UUID 문자열로. */

	spdk_json_write_name(w, "partition_name");                                          /* [한국어] 파티션 이름 키. */
	write_string_utf16le(w, gpt_entry->partition_name, SPDK_COUNTOF(gpt_entry->partition_name)); /* [한국어] UTF-16LE 이름(최대 배열 길이)을 문자열로. */

	spdk_json_write_object_end(w);                                                      /* [한국어] "gpt" 객체 닫기 }. */

	return 0;                                                                           /* [한국어] 성공. dump 는 실패 케이스가 없음. */
}

/*
 * [한국어]
 * vbdev_gpt_get_memory_domains - 이 파티션 bdev 가 지원하는 메모리 도메인을 상위에 보고(.get_memory_domains).
 *
 * @ctx: gpt_disk 의 part 멤버 주소(module ctx).
 * @domains: 출력 도메인 배열(상위가 제공). NULL 이면 개수만 질의.
 * @array_size: domains 배열 용량.
 * @return: 지원 도메인 개수(>=0), 또는 음수 에러.
 *
 * 동기: zero-copy/accel 경로에서 상위 계층은 DMA 가능한 메모리 도메인을 알아야 한다. gpt 는
 *   실제 저장공간이 없으므로 base bdev 의 도메인을 그대로 전달(passthrough)한다. 단, DIF
 *   reftag 검사가 켜진 경우 bdev_part 가 메타데이터 버퍼를 만지고 reftag 를 remap 하므로
 *   메모리 도메인 직접 전송이 불가하여 0(도메인 없음)을 반환해야 한다.
 * 동작: reftag check 플래그가 있으면 0 반환, 아니면 base bdev 의 도메인을 위임 질의.
 * 실행 컨텍스트: 상위 bdev 사용자(예: nvmf)의 질의 app thread.
 * 호출 체인: spdk_bdev_get_memory_domains(상위) → fn_table.get_memory_domains
 *   → [vbdev_gpt_get_memory_domains] → spdk_bdev_get_memory_domains(base).
 */
static int
vbdev_gpt_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct gpt_disk *gpt_disk = SPDK_CONTAINEROF(ctx, struct gpt_disk, part);         /* [한국어] part 주소에서 gpt_disk 복원. */
	struct spdk_bdev_part_base *part_base = spdk_bdev_part_get_base(&gpt_disk->part); /* [한국어] 소속 base 핸들. */
	struct spdk_bdev *part_base_bdev = spdk_bdev_part_base_get_bdev(part_base);       /* [한국어] 실제 베이스 bdev — 도메인 위임 대상. */

	if (part_base_bdev->dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK) {              /* [한국어] DIF reftag 검사 활성 여부 확인. */
		/* bdev_part remaps reftag and touches metadata buffer, that means it can't support memory domains
		 * if dif is enabled */
		/* [한국어] reftag remap 으로 bdev_part 가 메타 버퍼를 건드리므로 외부 메모리 도메인 직접전송 불가 → 도메인 0 보고. */
		return 0;
	}

	return spdk_bdev_get_memory_domains(part_base_bdev, domains, array_size);         /* [한국어] reftag 검사 없으면 base 의 도메인을 그대로 위임 보고. */
}

/*
 * [한국어]
 * vbdev_gpt_create_bdevs - 파싱된 GPT 파티션 엔트리들을 순회하며 각 SPDK 파티션을 가상 bdev 로 등록.
 *
 * @gpt_base: 파싱이 끝난(header/partitions 가 채워진) base 컨텍스트.
 * @return: 생성된 파티션 수(>=0), 또는 메모리/등록 실패 시 -1.
 *
 * 동기: gpt.c 파싱이 끝나면 partitions[] 에 GPT 엔트리들이 들어있다. 이 중 (1) 사용 중이고
 *   (2) SPDK 가 관할하는 type GUID 를 가진 (3) usable LBA 범위 안의 엔트리만 골라
 *   "<base>pN" 이름의 가상 bdev 로 노출한다. 이 모듈의 최종 산출물을 만드는 함수.
 * 동작 단계(엔트리마다):
 *   1) starting_lba==0 이면 빈 슬롯이므로 skip.
 *   2) type GUID 검사: TYPE_GUID_OLD 이면 issue #2801 호환 위해 크기 -1, TYPE_GUID 도
 *      TYPE_GUID_OLD 도 아니면 SPDK 파티션이 아니므로 skip.
 *   3) LBA 범위가 헤더의 first/last usable 범위를 벗어나면 skip(손상 방어).
 *   4) gpt_disk 할당 → "<base>pN" 이름 생성(N=i+1, 1-base) → unique GUID→uuid 변환 →
 *      spdk_bdev_part_construct_ext 로 등록 → partition_index 기록.
 * 실행 컨텍스트: gpt_bdev_complete / secondary_complete 콜백에서 호출 — examine app thread.
 * 호출 체인: gpt_bdev_complete(또는 secondary) → [vbdev_gpt_create_bdevs]
 *   → spdk_bdev_part_construct_ext (→ 파티션 bdev 등록).
 * 에러 처리: 어떤 단계든 실패하면 그때까지 만든 것은 두고 -1 반환 → 호출자가 base 정리 판단.
 */
static int
vbdev_gpt_create_bdevs(struct gpt_base *gpt_base)
{
	uint32_t num_partition_entries;          /* [한국어] GPT 헤더가 선언한 엔트리 슬롯 총수(보통 128). */
	uint64_t i, head_lba_start, head_lba_end;/* [한국어] 순회 인덱스 + 헤더의 first/last usable LBA(검증 경계). */
	uint32_t num_partitions;                 /* [한국어] 실제 생성한 파티션 수(반환값). */
	struct spdk_gpt_partition_entry *p;      /* [한국어] 현재 엔트리 포인터. */
	struct gpt_disk *d;                      /* [한국어] 새로 만들 파티션 컨텍스트. */
	struct spdk_gpt *gpt;                    /* [한국어] gpt_base->gpt 단축 포인터. */
	char *name;                              /* [한국어] "<base>pN" 동적 이름 버퍼. */
	struct spdk_bdev *base_bdev;             /* [한국어] 이름 prefix 용 베이스 bdev. */
	int rc;                                  /* [한국어] construct 결과. */

	gpt = &gpt_base->gpt;                                            /* [한국어] 파싱 결과 접근. */
	num_partition_entries = from_le32(&gpt->header->num_partition_entries); /* [한국어] 헤더(LE)에서 엔트리 개수 읽기. */
	head_lba_start = from_le64(&gpt->header->first_usable_lba);      /* [한국어] 데이터가 위치 가능한 최소 LBA(GPT 헤더/엔트리 영역 이후). */
	head_lba_end = from_le64(&gpt->header->last_usable_lba);         /* [한국어] 데이터 가능한 최대 LBA(secondary 영역 이전). */
	num_partitions = 0;                                             /* [한국어] 카운터 초기화. */

	for (i = 0; i < num_partition_entries; i++) {                   /* [한국어] 모든 엔트리 슬롯 순회. */
		p = &gpt->partitions[i];                               /* [한국어] i번째 엔트리. */
		uint64_t lba_start = from_le64(&p->starting_lba);      /* [한국어] 파티션 시작 LBA(LE). */
		uint64_t lba_end = from_le64(&p->ending_lba);          /* [한국어] 파티션 끝 LBA(포함, LE). */
		uint64_t partition_size = lba_end - lba_start + 1;     /* [한국어] 파티션 크기(블록 수). end 포함이므로 +1. */
		struct spdk_bdev_part_construct_opts opts;             /* [한국어] part 생성 옵션(uuid 등). */

		if (lba_start == 0) {                                  /* [한국어] start LBA 0 = 미사용 슬롯(GPT 규약). */
			continue;                                      /* [한국어] 다음 엔트리로. */
		}

		if (SPDK_GPT_GUID_EQUAL(&gpt->partitions[i].part_type_guid,
					&SPDK_GPT_PART_TYPE_GUID_OLD)) { /* [한국어] 구버전 SPDK type GUID 매칭. */
			/* GitHub issue #2801 - we continue to report these partitions with
			 * off-by-one sizing error to ensure we don't break layouts based
			 * on that smaller size. */
			/* [한국어] 과거 버그로 1블록 작게 보고하던 호환성 유지(이슈 #2801) — 기존 레이아웃 깨짐 방지. */
			partition_size -= 1;
		} else if (!SPDK_GPT_GUID_EQUAL(&gpt->partitions[i].part_type_guid,
						&SPDK_GPT_PART_TYPE_GUID)) { /* [한국어] 현행 SPDK type GUID 도 아니라면. */
			/* Partition type isn't TYPE_GUID or TYPE_GUID_OLD, so this isn't
			 * an SPDK partition.  Continue to the next partition.
			 */
			/* [한국어] SPDK 가 관할하지 않는 타입(예: Linux/EFI) — 노출하지 않고 skip. */
			continue;
		}

		if (lba_start < head_lba_start || lba_end > head_lba_end) { /* [한국어] usable 범위 이탈 = 손상/악성 엔트리 방어. */
			continue;                                      /* [한국어] 범위 밖이면 무시. */
		}

		d = calloc(1, sizeof(*d));                             /* [한국어] 파티션 컨텍스트 zero-할당. */
		if (!d) {                                              /* [한국어] OOM 방어. */
			SPDK_ERRLOG("Memory allocation failure\n");    /* [한국어] 로그. */
			return -1;                                     /* [한국어] 실패 — 호출자가 base 정리. */
		}

		/* index start at 1 instead of 0 to match the existing style */
		base_bdev = spdk_bdev_part_base_get_bdev(gpt_base->part_base); /* [한국어] 이름 prefix 로 쓸 베이스 bdev. */
		/* [한국어] "<base>p<번호>" 이름 생성. 번호는 i+1 (사용자 관례상 1부터, 예: Nvme0n1p1). */
		name = spdk_sprintf_alloc("%sp%" PRIu64, spdk_bdev_get_name(base_bdev), i + 1);
		if (!name) {                                           /* [한국어] 이름 할당 실패. */
			SPDK_ERRLOG("name allocation failure\n");      /* [한국어] 로그. */
			free(d);                                       /* [한국어] 방금 잡은 d 누수 방지. */
			return -1;                                     /* [한국어] 실패 전파. */
		}

		spdk_bdev_part_construct_opts_init(&opts, sizeof(opts)); /* [한국어] 옵션 구조체 기본값 초기화(ABI size 전달). */
		gpt_guid_to_uuid(&p->unique_partition_guid, &opts.uuid); /* [한국어] 고유 파티션 GUID 를 bdev uuid 로 설정 — 안정적 식별자. */
		/* [한국어] 파티션 bdev 등록: d->part 를 base 의 lba_start 부터 partition_size 만큼,
		 *   "GPT Disk" product name, opts(uuid) 와 함께 생성. 성공 시 parts TAILQ 에 링크됨. */
		rc = spdk_bdev_part_construct_ext(&d->part, gpt_base->part_base, name, lba_start,
						  partition_size, "GPT Disk", &opts);
		free(name);                                            /* [한국어] construct 가 내부 복사하므로 임시 이름 해제. */
		if (rc) {                                              /* [한국어] 등록 실패(이름 충돌 등). */
			SPDK_ERRLOG("could not construct bdev part\n"); /* [한국어] 로그. */
			/* spdk_bdev_part_construct will free name on failure */
			free(d);                                       /* [한국어] 실패 시 d 누수 방지(part 미등록 상태). */
			return -1;                                     /* [한국어] 실패 전파. */
		}
		num_partitions++;                                      /* [한국어] 성공 카운트 증가. */
		d->partition_index = i;                                /* [한국어] dump_info_json 이 엔트리를 되찾을 수 있도록 인덱스 기록. */
	}

	return num_partitions;                                         /* [한국어] 만든 파티션 수 반환(0 가능). */
}

/*
 * [한국어]
 * gpt_read_secondary_table_complete - 백업(secondary) GPT 읽기 완료 콜백: 파싱 후 파티션 생성·examine 종료.
 *
 * @bdev_io: 완료된 secondary read IO(해제 대상).
 * @success: read 성공 여부.
 * @arg: gpt_base 포인터(read 발행 시 cb_arg).
 * @return: 없음.
 *
 * 동기: primary GPT 가 손상됐을 때 디스크 끝의 secondary GPT 로 폴백한다. 그 read 가 끝나면
 *   이 콜백이 secondary 테이블만 파싱(MBR 파싱은 생략)하고 파티션을 만든 뒤 examine 을 닫는다.
 * 동작: IO/채널 정리 → 실패면 종료 → gpt_parse_partition_table(secondary) → create_bdevs →
 *   examine_done → 파티션 0개면 base 해제.
 * 실행 컨텍스트: bdev read 완료를 알리는 app thread(폴러가 CQ 폴링 후 콜백 디스패치).
 * 호출 체인: vbdev_gpt_read_secondary_table → spdk_bdev_read → (완료) → [gpt_read_secondary_table_complete]
 *   → gpt_parse_partition_table / vbdev_gpt_create_bdevs / spdk_bdev_module_examine_done.
 */
static void
gpt_read_secondary_table_complete(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct gpt_base *gpt_base = (struct gpt_base *)arg;                        /* [한국어] cb_arg 를 base 컨텍스트로 복원. */
	struct spdk_bdev *bdev = spdk_bdev_part_base_get_bdev(gpt_base->part_base);/* [한국어] 로그용 베이스 bdev. */
	int rc, num_partitions = 0;                                               /* [한국어] 파싱 결과 / 생성 파티션 수(기본 0 → goto end 시 base 해제 트리거). */

	spdk_bdev_free_io(bdev_io);                                               /* [한국어] 완료된 read IO 반납(필수 — 안 하면 누수). */
	spdk_put_io_channel(gpt_base->ch);                                        /* [한국어] 테이블 읽기용 임시 채널 반납. */
	gpt_base->ch = NULL;                                                      /* [한국어] dangling 방지로 NULL 리셋. */

	if (!success) {                                                           /* [한국어] secondary read 자체 실패. */
		SPDK_ERRLOG("Gpt: bdev=%s io error\n", spdk_bdev_get_name(bdev)); /* [한국어] 에러 로그. */
		goto end;                                                        /* [한국어] examine 종료 + base 해제 경로로. */
	}

	rc = gpt_parse_partition_table(&gpt_base->gpt);                          /* [한국어] 버퍼의 secondary 테이블을 파싱(header/partitions 채움). */
	if (rc) {                                                                /* [한국어] secondary 도 손상 — 이 디스크는 GPT 가 아님. */
		SPDK_DEBUGLOG(vbdev_gpt, "Failed to parse secondary partition table\n"); /* [한국어] 디버그 로그. */
		goto end;                                                        /* [한국어] 정리 경로. */
	}

	SPDK_WARNLOG("Gpt: bdev=%s primary partition table broken, use the secondary\n",
		     spdk_bdev_get_name(bdev));                                  /* [한국어] primary 손상→secondary 사용을 사용자에게 경고. */

	num_partitions = vbdev_gpt_create_bdevs(gpt_base);                      /* [한국어] secondary 기반으로 파티션 가상 bdev 생성. */
	if (num_partitions < 0) {                                                /* [한국어] 생성 중 메모리/등록 실패. */
		SPDK_DEBUGLOG(vbdev_gpt, "Failed to split dev=%s by gpt table\n",
			      spdk_bdev_get_name(bdev));                         /* [한국어] 디버그 로그(num_partitions<0 → end 에서 base 해제). */
	}

end:
	spdk_bdev_module_examine_done(&gpt_if);                                  /* [한국어] 이 디스크에 대한 gpt examine 완료를 bdev 코어에 통지(필수 — 안 하면 examine 직렬화가 멈춤). */
	if (num_partitions <= 0) {                                               /* [한국어] 만든 파티션이 없거나 실패면. */
		/* If no gpt_disk instances were created, free the base context */
		spdk_bdev_part_base_free(gpt_base->part_base);                   /* [한국어] 쓸모없어진 base 해제(→ gpt_base_free 연쇄로 버퍼/구조체 정리). */
	}
}

/*
 * [한국어]
 * vbdev_gpt_read_secondary_table - 디스크 끝의 백업(secondary) GPT 영역을 읽도록 발행.
 *
 * @gpt_base: primary 파싱에 실패한 base 컨텍스트.
 * @return: spdk_bdev_read 결과(0=수락, 음수=발행 실패).
 *
 * 동기: GPT 규약상 primary 테이블이 손상되면 디스크 마지막 LBA 들에 있는 secondary 테이블로
 *   복구할 수 있다. 그 영역을 같은 버퍼로 다시 읽기 위한 함수.
 * 동작: parse_phase=SECONDARY 로 전환하고 header/partitions 포인터를 초기화(이전 primary 파싱 잔재 제거),
 *   secondary offset = 전체바이트 - buf_size 위치에서 buf_size 만큼 read 발행.
 * 실행 컨텍스트: gpt_bdev_complete(primary 파싱 실패 분기)에서 호출 — examine app thread.
 * 호출 체인: gpt_bdev_complete → [vbdev_gpt_read_secondary_table] → spdk_bdev_read
 *   → (완료) gpt_read_secondary_table_complete.
 */
static int
vbdev_gpt_read_secondary_table(struct gpt_base *gpt_base)
{
	struct spdk_gpt *gpt;                                                    /* [한국어] gpt 단축 포인터. */
	struct spdk_bdev_desc *part_base_desc;                                   /* [한국어] read 발행에 쓸 base bdev desc. */
	uint64_t secondary_offset;                                              /* [한국어] secondary GPT 가 위치한 바이트 오프셋. */

	gpt = &gpt_base->gpt;                                                    /* [한국어] 컨텍스트 접근. */
	gpt->parse_phase = SPDK_GPT_PARSE_PHASE_SECONDARY;                       /* [한국어] 파서에 "이번엔 secondary 레이아웃" 임을 알림. */
	gpt->header = NULL;                                                      /* [한국어] primary 파싱이 남긴 포인터 제거 — secondary 파싱이 새로 채움. */
	gpt->partitions = NULL;                                                  /* [한국어] 동일 이유로 초기화. */

	part_base_desc = spdk_bdev_part_base_get_desc(gpt_base->part_base);      /* [한국어] base 의 open desc 조회(read 권한). */

	secondary_offset = gpt->total_sectors * gpt->sector_size - gpt->buf_size;/* [한국어] 디스크 총 바이트에서 버퍼 크기를 뺀 위치 = 끝쪽 secondary 영역 시작. */
	/* [한국어] secondary 영역을 동일 버퍼로 읽기. 완료 시 gpt_read_secondary_table_complete 콜백. */
	return spdk_bdev_read(part_base_desc, gpt_base->ch, gpt_base->gpt.buf, secondary_offset,
			      gpt_base->gpt.buf_size, gpt_read_secondary_table_complete,
			      gpt_base);
}

/*
 * [한국어]
 * gpt_bdev_complete - primary GPT 읽기 완료 콜백: MBR/primary 파싱 → 파티션 생성 또는 secondary 폴백.
 *
 * @bdev_io: 완료된 primary read IO(해제 대상).
 * @success: read 성공 여부.
 * @arg: gpt_base 포인터(read 발행 시 cb_arg).
 * @return: 없음.
 *
 * 동기: vbdev_gpt_read_gpt 가 발행한 LBA 0~ 읽기가 끝나면 여기로 진입한다. 이 디스크가
 *   GPT 인지 판별(보호 MBR + primary 헤더/엔트리)하고, 유효하면 파티션을 만들고, primary 가
 *   깨졌으면 secondary 로 폴백하는 분기점이다.
 * 동작: IO 해제 → 실패면 종료 → gpt_parse_mbr → gpt_parse_partition_table(primary).
 *   primary 파싱 실패 시 vbdev_gpt_read_secondary_table 로 폴백(여기서 return — examine 미종료 유지).
 *   성공 시 create_bdevs → end 라벨에서 채널 정리/examine_done/파티션 0개면 base 해제.
 * 실행 컨텍스트: bdev read 완료를 알리는 app thread(폴러 디스패치).
 * 호출 체인: vbdev_gpt_read_gpt → spdk_bdev_read → (완료) → [gpt_bdev_complete]
 *   → gpt_parse_mbr / gpt_parse_partition_table / vbdev_gpt_read_secondary_table / vbdev_gpt_create_bdevs.
 */
static void
gpt_bdev_complete(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct gpt_base *gpt_base = (struct gpt_base *)arg;                        /* [한국어] cb_arg → base 복원. */
	struct spdk_bdev *bdev = spdk_bdev_part_base_get_bdev(gpt_base->part_base);/* [한국어] 로그용 베이스 bdev. */
	int rc, num_partitions = 0;                                               /* [한국어] 파싱 결과 / 생성 파티션 수(기본 0). */

	spdk_bdev_free_io(bdev_io);                                               /* [한국어] 완료된 read IO 반납. */

	if (!success) {                                                           /* [한국어] primary read 자체 실패. */
		SPDK_ERRLOG("Gpt: bdev=%s io error\n", spdk_bdev_get_name(bdev)); /* [한국어] 에러 로그. */
		goto end;                                                        /* [한국어] 정리 경로. */
	}

	rc = gpt_parse_mbr(&gpt_base->gpt);                                       /* [한국어] LBA0 의 보호 MBR(0xEE 파티션) 검사 — GPT 디스크 1차 확인. */
	if (rc) {                                                                 /* [한국어] MBR 이 GPT 보호 형식이 아님 = GPT 디스크 아님. */
		SPDK_DEBUGLOG(vbdev_gpt, "Failed to parse mbr\n");               /* [한국어] 디버그 로그. */
		goto end;                                                        /* [한국어] 파티션 0개로 종료(흔한 정상 경로). */
	}

	rc = gpt_parse_partition_table(&gpt_base->gpt);                          /* [한국어] LBA1 primary 헤더 + 엔트리 배열 파싱/CRC 검증. */
	if (rc) {                                                                 /* [한국어] primary 테이블 손상. */
		SPDK_DEBUGLOG(vbdev_gpt, "Failed to parse primary partition table\n"); /* [한국어] 디버그 로그. */
		rc = vbdev_gpt_read_secondary_table(gpt_base);                  /* [한국어] 디스크 끝 secondary 테이블 읽기 발행(비동기). */
		if (rc) {                                                        /* [한국어] secondary read 발행 자체 실패. */
			SPDK_ERRLOG("Failed to read secondary table\n");        /* [한국어] 에러 로그. */
			goto end;                                               /* [한국어] 폴백 불가 → 정리 경로. */
		}
		return;                                                          /* [한국어] secondary read 가 진행 중이므로 examine 을 닫지 않고 콜백에 위임. */
	}

	num_partitions = vbdev_gpt_create_bdevs(gpt_base);                      /* [한국어] primary 기반으로 파티션 가상 bdev 생성. */
	if (num_partitions < 0) {                                                /* [한국어] 생성 실패. */
		SPDK_DEBUGLOG(vbdev_gpt, "Failed to split dev=%s by gpt table\n",
			      spdk_bdev_get_name(bdev));                         /* [한국어] 디버그 로그(end 에서 base 해제). */
	}

end:
	spdk_put_io_channel(gpt_base->ch);                                       /* [한국어] 테이블 읽기용 임시 채널 반납. */
	gpt_base->ch = NULL;                                                     /* [한국어] dangling 방지. */
	/*
	 * Notify the generic bdev layer that the actions related to the original examine
	 *  callback are now completed.
	 */
	spdk_bdev_module_examine_done(&gpt_if);                                  /* [한국어] examine 완료 통지(필수 — bdev 코어의 모듈 examine 직렬화를 진행시킴). */

	/*
	 * vbdev_gpt_create_bdevs returns the number of bdevs created upon success.
	 * We can branch on this value.
	 */
	if (num_partitions <= 0) {                                               /* [한국어] 파티션을 하나도 못 만들었으면. */
		/* If no gpt_disk instances were created, free the base context */
		spdk_bdev_part_base_free(gpt_base->part_base);                  /* [한국어] base 해제(→ gpt_base_free 연쇄). 이 디스크는 gpt 관할 아님. */
	}
}

/*
 * [한국어]
 * vbdev_gpt_read_gpt - GPT 검사를 위해 base 컨텍스트를 만들고 LBA 0~ primary 영역 읽기를 발행.
 *
 * @bdev: examine 대상 베이스 bdev.
 * @return: 0(read 발행 성공, 완료는 콜백) 또는 -1(초기화/발행 실패).
 *
 * 동기: examine 진입점이 "이 디스크가 GPT 인가?" 를 확인하려면 우선 앞쪽 LBA 들을 읽어야 한다.
 *   이 함수가 컨텍스트/버퍼/채널을 준비하고 첫 read 를 쏜다.
 * 동작 단계: gpt_base_bdev_init → base desc 로 io_channel 획득 → LBA 0 부터 buf_size 만큼 read 발행.
 *   이후 흐름은 모두 gpt_bdev_complete 콜백으로 이어진다(이 함수는 발행만 하고 즉시 반환).
 * 실행 컨텍스트: vbdev_gpt_examine 호출 경로의 app thread.
 * 호출 체인: vbdev_gpt_examine → [vbdev_gpt_read_gpt] → gpt_base_bdev_init / spdk_bdev_read
 *   → (완료) gpt_bdev_complete.
 * 에러 처리: 각 단계 실패 시 이미 잡은 자원(채널/base)을 역순으로 정리하고 -1 반환 →
 *   호출자(examine)가 examine_done 으로 마무리.
 */
static int
vbdev_gpt_read_gpt(struct spdk_bdev *bdev)
{
	struct gpt_base *gpt_base;              /* [한국어] 새로 만들 base 컨텍스트. */
	struct spdk_bdev_desc *part_base_desc;  /* [한국어] read 발행에 쓸 base desc. */
	int rc;                                 /* [한국어] read 발행 결과. */

	gpt_base = gpt_base_bdev_init(bdev);    /* [한국어] base/버퍼/기하 정보 초기화. */
	if (!gpt_base) {                        /* [한국어] 초기화 실패(OOM/construct 실패). */
		SPDK_ERRLOG("Cannot allocated gpt_base\n"); /* [한국어] 로그. */
		return -1;                      /* [한국어] examine 이 examine_done 으로 닫음. */
	}

	part_base_desc = spdk_bdev_part_base_get_desc(gpt_base->part_base); /* [한국어] base open desc 획득. */
	gpt_base->ch = spdk_bdev_get_io_channel(part_base_desc);            /* [한국어] 현재 스레드용 base I/O 채널 획득(테이블 읽기 전용). */
	if (gpt_base->ch == NULL) {             /* [한국어] 채널 획득 실패. */
		SPDK_ERRLOG("Failed to get an io_channel.\n"); /* [한국어] 로그. */
		spdk_bdev_part_base_free(gpt_base->part_base); /* [한국어] 이미 만든 base 정리(→ gpt_base_free). */
		return -1;                      /* [한국어] 실패 전파. */
	}

	/* [한국어] LBA 0(보호 MBR) 부터 buf_size 만큼 읽기 발행. 완료 시 gpt_bdev_complete 가 파싱/분기 수행. */
	rc = spdk_bdev_read(part_base_desc, gpt_base->ch, gpt_base->gpt.buf, 0,
			    gpt_base->gpt.buf_size, gpt_bdev_complete, gpt_base);
	if (rc < 0) {                           /* [한국어] read 발행 실패(자원/인자 오류). */
		spdk_put_io_channel(gpt_base->ch);             /* [한국어] 잡았던 채널 반납. */
		spdk_bdev_part_base_free(gpt_base->part_base); /* [한국어] base 정리. */
		SPDK_ERRLOG("Failed to send bdev_io command\n"); /* [한국어] 로그. */
		return -1;                      /* [한국어] 실패 전파. */
	}

	return 0;                               /* [한국어] read 발행 성공 — 나머지는 콜백이 처리. */
}

/*
 * [한국어]
 * vbdev_gpt_init - 모듈 1회 초기화 콜백(.module_init). 현재는 할 일이 없는 stub.
 *
 * @return: 0(성공). 음수면 bdev 서브시스템 초기화 실패로 처리됨.
 *
 * 동기: SPDK bdev 모듈 인터페이스는 module_init 을 요구한다. 일부 모듈은 여기서 전역 상태나
 *   RPC 를 등록하지만, gpt 는 examine 시점에 모든 작업을 하므로 초기화할 전역이 없다.
 * 실행 컨텍스트: bdev 서브시스템 init 단계의 app thread(프로그램 시작 시 1회).
 * 호출 체인: bdev subsystem init → gpt_if.module_init → [vbdev_gpt_init].
 */
static int
vbdev_gpt_init(void)
{
	return 0; /* [한국어] 초기화할 전역/리소스 없음 — 성공 반환. */
}

/*
 * [한국어]
 * vbdev_gpt_get_ctx_size - bdev_io 당 추가로 둘 driver_ctx 크기를 bdev 코어에 보고(.get_ctx_size).
 *
 * @return: struct gpt_io 의 바이트 크기.
 *
 * 동기: 이 모듈은 -ENOMEM 큐잉을 위해 bdev_io 마다 gpt_io(ch/bdev_io/io_wait)를 붙여야 한다.
 *   bdev 코어가 bdev_io 를 할당할 때 그 뒤에 이만큼의 공간(driver_ctx)을 예약하도록 알린다.
 * 실행 컨텍스트: 파티션 bdev 등록 시 bdev 코어가 1회 질의 — app thread.
 * 호출 체인: bdev_io 풀 셋업 → gpt_if.get_ctx_size → [vbdev_gpt_get_ctx_size].
 */
static int
vbdev_gpt_get_ctx_size(void)
{
	return sizeof(struct gpt_io); /* [한국어] _vbdev_gpt_submit_request 가 bdev_io->driver_ctx 를 gpt_io 로 쓰므로 그 크기. */
}

/*
 * [한국어]
 * vbdev_gpt_examine - 새 bdev 에 대한 GPT 검사 진입점(.examine_disk).
 *
 * @bdev: 새로 등록되어 검사 대상이 된 베이스 bdev.
 * @return: 없음(검사는 비동기, 완료는 spdk_bdev_module_examine_done 으로 통지).
 *
 * 동기: bdev 코어는 새 bdev 가 나타날 때마다 등록된 모든 모듈의 examine_disk 를 호출한다.
 *   gpt 모듈은 여기서 "이 디스크에 GPT 가 있는가?" 를 사전 검사 후 실제 read 흐름을 시작한다.
 *   중요: 모든 종료 경로에서 반드시 spdk_bdev_module_examine_done 을 호출해야 한다 —
 *   그러지 않으면 bdev 코어의 examine 직렬화가 영원히 멈춘다.
 * 동작:
 *   1) 블록 수 < 2 면 GPT 불가능(블록0=MBR, 블록1=헤더) → 즉시 examine_done.
 *   2) 블록 크기가 512 배수가 아니면 미지원 → examine_done.
 *   3) vbdev_gpt_read_gpt 로 실제 read 발행. 발행 실패 시에만 즉시 examine_done(성공 시
 *      examine_done 은 완료 콜백 체인이 담당).
 * 실행 컨텍스트: bdev 코어가 호출하는 app thread.
 * 호출 체인: bdev 등록 → gpt_if.examine_disk → [vbdev_gpt_examine] → vbdev_gpt_read_gpt.
 */
static void
vbdev_gpt_examine(struct spdk_bdev *bdev)
{
	int rc; /* [한국어] read_gpt 발행 결과. */

	/* A bdev with fewer than 2 blocks cannot have a GPT. Block 0 has
	 * the MBR and block 1 has the GPT header.
	 */
	/* [한국어] 블록0=보호MBR, 블록1=GPT 헤더이므로 최소 2블록 필요. 미만이면 GPT 불가. */
	if (spdk_bdev_get_num_blocks(bdev) < 2) {
		spdk_bdev_module_examine_done(&gpt_if); /* [한국어] read 없이 즉시 examine 종료(필수 통지). */
		return;                                 /* [한국어] 검사 종료. */
	}

	if (spdk_bdev_get_block_size(bdev) % 512 != 0) { /* [한국어] GPT 레이아웃 계산은 512 배수 섹터를 가정 — 비배수면 미지원. */
		SPDK_DEBUGLOG(vbdev_gpt,
			      "GPT module does not support block size %" PRIu32 " for bdev %s\n",
			      spdk_bdev_get_block_size(bdev), spdk_bdev_get_name(bdev)); /* [한국어] 미지원 디버그 로그. */
		spdk_bdev_module_examine_done(&gpt_if); /* [한국어] 즉시 examine 종료(필수). */
		return;                                 /* [한국어] 검사 종료. */
	}

	rc = vbdev_gpt_read_gpt(bdev);                  /* [한국어] 사전 검사 통과 → 실제 primary GPT 읽기 발행(비동기). */
	if (rc) {                                       /* [한국어] read 발행 자체 실패. */
		spdk_bdev_module_examine_done(&gpt_if); /* [한국어] 콜백이 안 올 것이므로 여기서 직접 examine 종료. */
		SPDK_ERRLOG("Failed to read info from bdev %s\n", spdk_bdev_get_name(bdev)); /* [한국어] 에러 로그. */
	}
	/* [한국어] rc==0 이면 read 가 진행 중 — examine_done 은 gpt_bdev_complete/secondary 콜백이 호출. */
}

/* [한국어] 이 파일의 SPDK_DEBUGLOG(vbdev_gpt, ...) 가 사용할 "vbdev_gpt" 로그 컴포넌트를 등록.
 * constructor 시점에 등록되며, 런타임에 이 컴포넌트의 디버그 로그를 켜고 끌 수 있게 한다. */
SPDK_LOG_REGISTER_COMPONENT(vbdev_gpt)
