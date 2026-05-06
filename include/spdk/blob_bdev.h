/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Helper library to use spdk_bdev as the backing device for a blobstore
 */

/*
 * [한국어 설명] Blobstore ↔ bdev 어댑터 공개 헤더 (blob_bdev.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 의 Blobstore 라이브러리(lib/blob)가 SPDK bdev 레이어 위에서
 * 동작할 수 있도록 둘 사이를 연결하는 어댑터 라이브러리(lib/blob_bdev) 의
 * 공개 API 를 선언한다. Blobstore 는 SPDK 가 제공하는 "flat object store"
 * (LBA 풀 위에 가변 크기 blob 을 할당하는 객체 저장소) 인데, 그 자체는
 * I/O 를 직접 발행하지 않고 추상 인터페이스인 struct spdk_bs_dev (read/
 * write/unmap/write_zeroes/flush 등의 함수 포인터 vtable) 를 통해 하부
 * 스토리지에 위임한다. 이 어댑터 라이브러리는 그 spdk_bs_dev 인터페이스를
 * "일반 SPDK bdev" 위에 구현해, blobstore 가 어떤 bdev 든(NVMe, AIO,
 * Malloc, RAID 등) backing store 로 쓸 수 있게 만든다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 의 스토리지 스택은 [blobstore (blob)] -> [blob_bdev 어댑터] ->
 * [bdev layer] -> [bdev module (nvme/aio/...)] 순서를 가진다. blobstore 자체
 * 는 bdev 의 존재를 모르며 spdk_bs_dev 인터페이스만 사용하므로, 이 어댑터가
 * 빠지면 blobstore 는 단독으로 PCIe NVMe 같은 실제 하드웨어에 닿을 수
 * 없다. 또한 lvolstore (lib/lvol) 와 NVMe-oF subsystem 의 일부 path 도
 * 이 어댑터를 통해 blobstore 를 bdev 위에 띄운다. 호출 체인은 보통
 * 애플리케이션 또는 RPC 가 spdk_bdev_create_bs_dev_ext() 를 호출하면
 * spdk_bs_dev* 포인터가 반환되고, 이를 spdk_bs_init / spdk_bs_load 로 넘겨
 * blobstore 를 띄운다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/bdev.h (spdk_bdev_event_cb_t, spdk_bdev_module 등 bdev API),
 * 내부적으로 lib/bdev (spdk_bdev_open_ext / claim 등 사용). 사용처:
 * lib/blob (struct spdk_bs_dev 인터페이스 소비자), lib/lvol (lvolstore 가
 * blobstore 위에 구축됨), bdev_lvol/bdev_aio/bdev_malloc 위에 lvolstore
 * 를 띄우는 애플리케이션 또는 RPC. 데이터 흐름: 애플리케이션 -> bdev_name
 * 문자열 -> spdk_bdev_create_bs_dev_ext -> spdk_bs_dev (read/write 함수
 * 포인터가 bdev I/O 발행으로 매핑된 vtable) -> spdk_bs_init/load.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_bdev_create_bs_dev_ext(name, event_cb, ...): 이름으로 bdev 를 열어
 *   blobstore 가 사용할 spdk_bs_dev 를 생성. 가장 흔한 진입점.
 * - spdk_bdev_create_bs_dev(name, write, opts, opts_size, ...): 위 함수의
 *   확장형. read-only/read-write 옵션과 ABI-호환 opts 구조체를 받는다.
 * - spdk_bdev_update_bs_blockcnt(bs_dev): 하부 bdev 의 블록 수 변경을
 *   blobstore 에 알림 (resize 시 필수).
 * - spdk_bs_bdev_claim(bs_dev, module): blobstore 가 사용 중임을 bdev 에
 *   "claim" 으로 등록 (다른 모듈이 동일 bdev 를 동시 사용하지 못하게 막음).
 * 핵심 구조체:
 * - struct spdk_bdev_bs_dev_opts: ABI 호환을 위한 opts 컨테이너 (현재는
 *   opts_size 필드만 있고 향후 확장용).
 */

/* [한국어] SPDK_BLOB_BDEV_H — 헤더 가드. blobstore 와 lvol 양쪽에서 포함되므로
 * 가드는 필수이다. */
#ifndef SPDK_BLOB_BDEV_H
#define SPDK_BLOB_BDEV_H

/* [한국어] spdk/stdinc.h - 표준 정수형/bool 등 SPDK 공통 include. opts_size
 * 의 size_t, write 인자의 bool 을 위해 필요하다. */
#include "spdk/stdinc.h"
/* [한국어] spdk/bdev.h - bdev 레이어의 공개 API. spdk_bdev_event_cb_t
 * (asynchronous bdev 이벤트 콜백 typedef) 와 struct spdk_bdev_module
 * 의 (불완전) 선언을 가져온다. 이 어댑터의 거의 모든 함수가 bdev API 와
 * 직접 상호작용하므로 필수 포함. */
#include "spdk/bdev.h"

/* [한국어] C++ 컴파일러용 extern "C" 가드 시작 — SPDK 공개 헤더 표준 패턴. */
#ifdef __cplusplus
extern "C" {
#endif

/* [한국어] struct spdk_bs_dev - blobstore 가 backing storage 와 통신하기
 * 위해 정의한 추상 vtable 구조체. 정의는 spdk/blob.h 에 있고 여기서는
 * 포인터 인자만 사용하므로 forward declaration 으로 충분하다. 이 어댑터
 * 라이브러리의 핵심 산출물이 바로 이 구조체의 인스턴스(read/write 등이
 * bdev I/O 로 구현된 vtable)이다. */
struct spdk_bs_dev;
/* [한국어] struct spdk_bdev - SPDK bdev 레이어의 디바이스 핸들 구조체.
 * 정의는 spdk/bdev.h 내부에 있고, 이 헤더에서는 spdk_bs_bdev_claim 등이
 * 포인터로만 다루므로 forward declaration. 외부 사용자가 직접 필드 접근
 * 하지 못하도록 의도적으로 불완전 타입으로 노출. */
struct spdk_bdev;
/* [한국어] struct spdk_bdev_module - bdev 모듈 등록 구조체. claim 호출자
 * (예: lvol 모듈, NVMe-oF subsystem) 자신을 식별하기 위해 자기 모듈 핸들을
 * spdk_bs_bdev_claim 에 전달해야 한다. forward declaration 으로 충분. */
struct spdk_bdev_module;

/* [한국어] struct spdk_bdev_bs_dev_opts - spdk_bdev_create_bs_dev() 의
 * 옵션 컨테이너. ABI/소스 호환성 관리를 위해 size_t opts_size 를 첫 필드로
 * 두는 SPDK 의 표준 패턴(OPTS_SIZE 패턴) 을 따른다. 새로운 필드는 항상
 * 끝에 추가되며, 라이브러리가 호출자가 알고 있는 크기까지만 읽고 그 이후
 * 필드는 라이브러리 기본값으로 채우는 방식으로 forward/backward 호환성을
 * 유지한다. 현재는 확장 옵션이 없어 opts_size 만 존재한다. */
struct spdk_bdev_bs_dev_opts {
	size_t opts_size;
	/* [한국어] opts_size — 호출자가 인식하는 이 구조체의 바이트 크기.
	 * 설정자: 호출자가 sizeof(struct spdk_bdev_bs_dev_opts) 또는 자신이
	 *          빌드된 헤더 시점의 크기로 채워서 전달.
	 * 읽는 자: spdk_bdev_create_bs_dev 내부 — 이 값까지의 필드만 유효한
	 *          것으로 간주하고, 그 너머는 기본값으로 패딩.
	 * 값 범위: sizeof(struct spdk_bdev_bs_dev_opts) 이하의 양의 정수.
	 *          0 또는 일관성 없는 값이면 EINVAL.
	 * 동기화: 호출자 스레드 로컬 — 별도 sync 불필요.
	 *
	 * 첫 필드로 size_t 를 두는 이 패턴은 ABI 안정성 확보를 위한 SPDK
	 * 표준이며, 동일 패턴이 spdk_thread_opts, spdk_bdev_opts 등에도
	 * 동일하게 사용된다. */
};
/* [한국어] SPDK_STATIC_ASSERT - 구조체 크기가 8 바이트 (size_t 1 개) 임을
 * 컴파일 타임에 강제. 새 필드를 추가하면 이 assert 가 깨지므로 ABI 변화를
 * 명시적으로 인식하게 한다. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_bs_dev_opts) == 8, "Incorrect size");

/*
 * [한국어]
 * spdk_bdev_create_bs_dev_ext - 이름으로 bdev 를 열어 blobstore 가 사용
 *                                할 spdk_bs_dev 를 생성한다 (read-write).
 *
 * @bdev_name: 사용할 bdev 의 이름 (예: "Nvme0n1", "Malloc0"). bdev_name 이
 *             존재하지 않으면 -ENODEV.
 * @event_cb: bdev 가 비동기 이벤트(REMOVE, RESIZE 등)를 일으킬 때 호출되는
 *            사용자 콜백. event_cb(type, bdev, ctx) 시그니처. NULL 가능.
 * @event_ctx: event_cb 호출 시 전달될 사용자 컨텍스트 포인터.
 * @bs_dev: (out) 성공 시 새로 생성된 spdk_bs_dev 포인터가 기록된다. 호출자
 *          는 이 포인터를 spdk_bs_init/load 에 넘긴다. 사용 후 destroy 는
 *          blobstore 종료 흐름에서 자동으로 일어남.
 * @return: 0 성공 / 음수 errno (-ENODEV bdev 없음, -ENOMEM 등).
 *
 * 동기/배경: lvolstore 또는 일반 blobstore 를 띄울 때 가장 흔히 사용되는
 * 진입점. read-write 모드로 bdev 를 spdk_bdev_open_ext() 하고, 그 desc 를
 * 캡슐화한 spdk_bs_dev vtable (read/write/unmap/... 가 모두 bdev I/O 로
 * 매핑된) 을 만들어 반환한다.
 *
 * 동작:
 *   1) spdk_bdev_open_ext(bdev_name, true, event_cb, event_ctx, &desc) 호출.
 *   2) 내부적으로 alloc 한 어댑터 객체에 desc 를 저장.
 *   3) read/write/unmap/write_zeroes/flush 함수 포인터를 bdev I/O 호출자로
 *      세팅한 spdk_bs_dev 를 *bs_dev 에 기록.
 *
 * 실행 컨텍스트: 애플리케이션 init 또는 RPC 핸들러 (bdev open 은 SPDK
 * thread 에서 실행되어야 함). bdev 는 한 번 열리면 임의의 SPDK thread 에서
 * I/O 가 가능하지만, open/close 자체는 단일 스레드에서 호출.
 *
 * 호출 체인:
 *   app/RPC → [spdk_bdev_create_bs_dev_ext] → spdk_bdev_open_ext →
 *   spdk_bs_dev vtable 생성 → spdk_bs_init / spdk_bs_load
 */
/**
 * Create a blobstore block device from a bdev.
 *
 * \param bdev_name Name of the bdev to use.
 * \param event_cb Called when the bdev triggers asynchronous event.
 * \param event_ctx Argument passed to function event_cb.
 * \param bs_dev Output parameter for a pointer to the blobstore block device.
 *
 * \return 0 if operation is successful, or suitable errno value otherwise.
 */
int spdk_bdev_create_bs_dev_ext(const char *bdev_name, spdk_bdev_event_cb_t event_cb,
				void *event_ctx, struct spdk_bs_dev **bs_dev);

/*
 * [한국어]
 * spdk_bdev_create_bs_dev - read-only/read-write 와 옵션 구조체를 받는
 *                           spdk_bdev_create_bs_dev_ext 의 확장 진입점.
 *
 * @bdev_name: 사용할 bdev 이름.
 * @write: true 면 read-write, false 면 read-only 로 bdev 를 연다.
 *         read-only 모드에서는 write/unmap/write_zeroes 가 -EPERM 으로 거절
 *         되며, claim 은 read-only-many 형태로 부여된다.
 * @opts: 추가 옵션 구조체 포인터. 현재 정의된 옵션은 없으나 ABI 호환을
 *        위해 항상 opts_size 를 채워야 한다. NULL 가능 (라이브러리 기본값).
 * @opts_size: opts 구조체의 바이트 크기 (호출자가 빌드된 헤더 기준).
 * @event_cb: 비동기 이벤트 콜백.
 * @event_ctx: 콜백 컨텍스트.
 * @bs_dev: (out) 생성된 spdk_bs_dev.
 * @return: 0 성공 / 음수 errno.
 *
 * 동기/배경: 스냅샷이나 read-only clone 을 다룰 때 read-only 로만 bdev 를
 * 열어야 다중 사용자가 안전하게 공유할 수 있다. _ext 함수는 항상 RW 이므로
 * 이 변형이 필요하다. 또한 향후 옵션 확장(예: write-back caching policy)
 * 에 대비한 opts 구조체를 노출한다.
 *
 * 동작: write 인자에 따라 spdk_bdev_open_ext 의 두 번째 인자(write
 * permission)를 분기하고, 그 외엔 _ext 함수와 동일.
 *
 * 실행 컨텍스트: _ext 와 동일.
 *
 * 호출 체인:
 *   blob_load (read-only snapshot) / lvol_clone → [spdk_bdev_create_bs_dev]
 *   → spdk_bdev_open_ext(write?) → spdk_bs_dev vtable 생성
 */
/**
 * Create a blobstore block device from a bdev.
 *
 * \param bdev_name The bdev to use.
 * \param write If true, open device read-write, else open read-only.
 * \param opts Additional options; none currently supported.
 * \param opts_size Size of structure referenced by opts.
 * \param event_cb Called when the bdev triggers asynchronous event.
 * \param event_ctx Argument passed to function event_cb.
 * \param bs_dev Output parameter for a pointer to the blobstore block device.
 * \return 0 if operation is successful, or suitable errno value otherwise.
 */
int spdk_bdev_create_bs_dev(const char *bdev_name, bool write,
			    struct spdk_bdev_bs_dev_opts *opts, size_t opts_size,
			    spdk_bdev_event_cb_t event_cb, void *event_ctx,
			    struct spdk_bs_dev **bs_dev);

/*
 * [한국어]
 * spdk_bdev_update_bs_blockcnt - 하부 bdev 의 블록 수가 변경된 사실을
 *                                blobstore 측 spdk_bs_dev 에 반영한다.
 *
 * @bs_dev: 갱신할 blobstore block device.
 * @return: void.
 *
 * 동기/배경: lvol resize, NVMe namespace resize 등으로 bdev 의 num_blocks 가
 * 동적으로 변경될 수 있다. blobstore 는 init 시점의 크기를 캐시하므로,
 * 외부에서 명시적으로 이 함수를 호출해 spdk_bs_dev->blockcnt 를 다시 읽어
 * 들이게 해야 한다. 호출 누락 시 새로 추가된 영역에 blob 을 할당하지 못한다.
 *
 * 동작: 어댑터 객체에 캐시된 spdk_bs_dev->blockcnt 를 spdk_bdev_get_num_blocks
 * 의 최신 값으로 덮어쓴다.
 *
 * 실행 컨텍스트: bdev resize 이벤트 핸들러 등에서 호출.
 *
 * 호출 체인:
 *   bdev RESIZE event → 사용자 event_cb → [spdk_bdev_update_bs_blockcnt]
 */
/**
 * Updates number of blocks of a blobstore block device.
 *
 * \param bs_dev Blobstore block device.
 */
void spdk_bdev_update_bs_blockcnt(struct spdk_bs_dev *bs_dev);

/*
 * [한국어]
 * spdk_bs_bdev_claim - blobstore 가 해당 bdev 를 사용 중임을 bdev 레이어에
 *                      claim 으로 등록한다.
 *
 * @bs_dev: claim 을 요청할 spdk_bs_dev.
 * @module: claim 을 요청하는 bdev 모듈 식별자 (자기 자신).
 * @return: 0 성공 / 음수 errno (-EBUSY 다른 모듈이 이미 claim 중,
 *          -EPERM 권한 부족 등).
 *
 * 동기/배경: SPDK bdev 는 두 모듈이 동일 bdev 를 동시에 변경하는 것을
 * 막기 위해 "claim" 메커니즘을 가진다. blobstore 가 backing bdev 위에
 * 메타데이터를 깔면, 다른 사용자가 그 위에 raw I/O 를 발행해 데이터를
 * 손상시키는 일이 없도록 반드시 claim 을 잡아야 한다.
 *
 * 동작: bs_dev 가 read-write 모드면 read-write-once claim, read-only 면
 * read-only-many claim 을 spdk_bdev_module_claim_bdev_desc 로 잡는다.
 * 두 종류 중 어느 쪽이 적용되는지는 bs_dev 가 만들어질 때의 write 인자에
 * 의해 결정된다.
 *
 * 실행 컨텍스트: blobstore 초기화 흐름의 일부 — 보통 spdk_bs_init/load
 * 콜백 안에서 호출.
 *
 * 호출 체인:
 *   spdk_bs_init/load → bs_dev->claim → [spdk_bs_bdev_claim] →
 *   spdk_bdev_module_claim_bdev_desc
 */
/**
 * Claim the bdev module for the given blobstore.
 *
 * If bs_dev was opened read-write using spdk_bdev_create_bs_dev_ext(), a read-write-once claim is
 * taken. If bs_dev was opened read-only using spdk_bdev_create_bs_dev(), a read-only-many claim
 * is taken.
 *
 * \param bs_dev Blobstore block device.
 * \param module Bdev module to claim.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_bs_bdev_claim(struct spdk_bs_dev *bs_dev, struct spdk_bdev_module *module);

/* [한국어] extern "C" 블록 종료. */
#ifdef __cplusplus
}
#endif

#endif
/* [한국어] 헤더 가드 종료. SPDK_BLOB_BDEV_H. */
