/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * [한국어 설명] Null bdev 모듈 공개 API 헤더 (bdev_null.h)
 *
 * === 파일의 역할 ===
 * Null bdev 모듈의 외부 인터페이스를 선언한다. Null bdev는 어떠한 실 스토리지도 가지지 않는
 * "/dev/null과 유사한" 가상 블록 디바이스로서, READ는 0 채우기, WRITE는 즉시 SUCCESS, UNMAP/
 * WRITE_ZEROES는 no-op으로 처리한다. 성능 측정/스택 자체 테스트(NVMe-oF/vhost backend overhead
 * 측정)에 사용된다. DIF(Data Integrity Field) 시뮬레이션 옵션을 가질 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 본 헤더는 RPC 핸들러(bdev_null_rpc.c)와 모듈 구현(bdev_null.c) 사이의 facade.
 *   [JSON-RPC client] → [bdev_null_rpc.c] → [이 헤더의 API] → [bdev_null.c → spdk_bdev_register]
 *
 * === 타 모듈과의 연결 ===
 * - bdev_null.c : 구현부.
 * - bdev_null_rpc.c : "bdev_null_create/delete/resize" JSON-RPC 핸들러.
 * - include/spdk/stdinc.h, include/spdk/uuid.h, include/spdk/dif.h : 사용 타입.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct null_bdev_opts : 생성 옵션 묶음.
 * - bdev_null_create() : 옵션 검증 후 spdk_bdev_register.
 * - bdev_null_delete() : 이름 기반 unregister.
 * - bdev_null_resize() : 블록 개수 변경 후 spdk_bdev_notify_blockcnt_change.
 */

#ifndef SPDK_BDEV_NULL_H
#define SPDK_BDEV_NULL_H

#include "spdk/stdinc.h"
/* [한국어] 표준 헤더 모음 (uint*_t, bool 등). */

/*
 * [한국어] typedef spdk_delete_null_complete
 * Null bdev 삭제 완료 콜백 시그니처. bdeverrno=0 성공, 음수 -errno 실패.
 */
typedef void (*spdk_delete_null_complete)(void *cb_arg, int bdeverrno);

struct spdk_bdev;
/* [한국어] forward declaration — bdev 코어 타입의 전방 선언으로 헤더 의존 최소화. */
struct spdk_uuid;
/* [한국어] forward declaration — UUID 타입의 전방 선언. */

/*
 * [한국어] struct null_bdev_opts
 * Null bdev 생성 시 RPC가 모아 전달하는 옵션 묶음. 본 모듈은 실 스토리지가 없으므로
 * "관찰 가능한 메타데이터"만 결정한다(블록 수/블록 크기/DIF 시뮬레이션 등).
 */
struct null_bdev_opts {
	char *name;
	/* [한국어] bdev 등록 이름. 모듈 내부에서 strdup 후 spdk_bdev.name에 매핑.
	 * 설정자: RPC 핸들러. 읽는 자: bdev_null_create.
	 * 동기화: app thread에서만 처리. */

	struct spdk_uuid uuid;
	/* [한국어] UUID. zero이면 모듈이 임의 UUID 생성. spdk_bdev.uuid로 복사. */

	uint64_t num_blocks;
	/* [한국어] 디바이스 블록 수. spdk_bdev.blockcnt와 1:1 매핑. */

	uint32_t block_size;
	/* [한국어] 논리 블록 크기(바이트). 일반적으로 512/4096. spdk_bdev.blocklen. */

	uint32_t physical_block_size;
	/* [한국어] 물리 블록 크기(바이트). 0이면 block_size와 동일. spdk_bdev.phys_blocklen. */

	uint32_t md_size;
	/* [한국어] 메타데이터 크기(바이트). 0이면 메타데이터 미사용. DIF/T10 PI 시뮬레이션용. */

	uint32_t preferred_write_alignment;
	/* [한국어] 권장 write 정렬(블록 단위). bdev 코어가 사용자에게 제안. */
	uint32_t preferred_write_granularity;
	/* [한국어] 권장 write granularity(블록 단위). */
	uint32_t optimal_write_size;
	/* [한국어] 최적 write 크기(블록 단위). */
	uint32_t preferred_unmap_alignment;
	/* [한국어] 권장 unmap 정렬(블록 단위). */
	uint32_t preferred_unmap_granularity;
	/* [한국어] 권장 unmap granularity(블록 단위). */

	enum spdk_dif_type dif_type;
	/* [한국어] DIF 보호 유형 (NONE/TYPE1/TYPE2/TYPE3). DIF 시뮬레이션 옵션. */
	bool dif_is_head_of_md;
	/* [한국어] DIF가 메타데이터의 앞에 위치하는지(true) 뒤에 위치하는지(false). */
	enum spdk_dif_pi_format dif_pi_format;
	/* [한국어] DIF Protection Information 형식(16비트 CRC, 32비트 CRC 등). */
};

/*
 * [한국어]
 * bdev_null_create - Null bdev를 생성하고 bdev core에 등록
 *
 * @bdev: [out] 생성된 bdev 포인터를 받을 위치.
 * @opts: 생성 옵션 묶음.
 * @return: 0 성공, 음수 -errno.
 *
 * 호출 컨텍스트: app thread.
 */
int bdev_null_create(struct spdk_bdev **bdev, const struct null_bdev_opts *opts);

/**
 * Delete null bdev.
 *
 * \param bdev_name Name of null bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
/*
 * [한국어]
 * bdev_null_delete - 이름으로 lookup 후 비동기 unregister.
 *
 * @bdev_name: 삭제 대상 이름.
 * @cb_fn: 완료 콜백 (NULL 가능).
 * @cb_arg: 콜백 인자.
 */
void bdev_null_delete(const char *bdev_name, spdk_delete_null_complete cb_fn,
		      void *cb_arg);
/**
 * Resize null bdev.
 *
 * \param bdev_name Name of null bdev.
 * \param new_size_in_mb The new size in MiB for this bdev
 */
/*
 * [한국어]
 * bdev_null_resize - 블록 개수 변경 후 코어에 알림.
 *
 * @bdev_name: 대상 이름.
 * @new_size_in_mb: 새 크기(MiB).
 * @return: 0 성공, 음수 -errno.
 */
int bdev_null_resize(const char *bdev_name, const uint64_t new_size_in_mb);

#endif /* SPDK_BDEV_NULL_H */
