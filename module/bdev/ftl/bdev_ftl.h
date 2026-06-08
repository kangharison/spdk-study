/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 *   Copyright 2023 Solidigm All Rights Reserved
 */

/*
 * [한국어 설명] FTL bdev 모듈 공개 API 헤더 (bdev_ftl.h)
 *
 * === 파일의 역할 ===
 * SPDK FTL(Flash Translation Layer) 라이브러리를 백엔드로 사용하는 bdev 모듈의 공개 API.
 * FTL은 RAW NAND 또는 ZNS / OCSSD 같은 "관리되지 않는" 미디어 위에 LBA → PBA(Physical Block
 * Address) 매핑, wear leveling, garbage collection, power-loss-safe 영역(NV cache) 등을
 * 구현하는 SW 레이어이다. 본 bdev 모듈은 lib/ftl을 통해 캐시 디바이스(NV cache) + base
 * 디바이스(예: 큰 ZNS SSD) 조합을 단일 spdk_bdev로 노출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [JSON-RPC] → [bdev_ftl_rpc.c] → [이 헤더] → [bdev_ftl.c]
 *                                                ↓ lib/ftl
 *                                              [NV cache bdev + base bdev]
 *
 * === 타 모듈과의 연결 ===
 * - bdev_ftl.c : 구현부.
 * - lib/ftl/* : Flash Translation Layer 코어.
 * - include/spdk/ftl.h : FTL 공개 API (spdk_ftl_conf, spdk_ftl_fn, ftl_stats).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ftl_bdev_info : 생성 콜백 인자.
 * - struct rpc_ftl_stats_ctx : stats 조회 RPC 컨텍스트.
 * - bdev_ftl_create_bdev / delete_bdev / defer_init : 라이프사이클.
 * - bdev_ftl_unmap : unmap 시뮬레이션.
 * - bdev_ftl_get_stats / get_properties / set_property : 모니터링/튜닝 RPC 백엔드.
 */

#ifndef SPDK_BDEV_FTL_H
#define SPDK_BDEV_FTL_H

#include "spdk/stdinc.h"
/* [한국어] 표준 헤더 모음. */
#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈 작성 API + spdk_bdev_unregister_cb. */
#include "spdk/ftl.h"
/* [한국어] FTL 공개 API (spdk_ftl_conf, spdk_ftl_fn, ftl_stats). */

#include "ftl_core.h"
/* [한국어] FTL 내부 헤더 — 디바이스 핸들 등 필요. */

/*
 * [한국어] struct ftl_bdev_info
 * FTL bdev 생성 콜백에 전달되는 정보 묶음 (이름 + UUID).
 */
struct ftl_bdev_info {
	const char		*name;
	/* [한국어] 생성된 bdev 이름. */
	struct spdk_uuid	uuid;
	/* [한국어] bdev UUID. */
};

/*
 * [한국어] struct rpc_ftl_stats_ctx
 * stats 조회 RPC가 비동기 콜백 사이에 보관하는 컨텍스트.
 */
struct rpc_ftl_stats_ctx {
	struct spdk_bdev_desc		*ftl_bdev_desc;
	/* [한국어] stats 조회용으로 열어둔 bdev descriptor. 콜백에서 close. */
	struct spdk_jsonrpc_request	*request;
	/* [한국어] 응답을 보낼 RPC 요청 핸들. */
	struct ftl_stats		ftl_stats;
	/* [한국어] FTL 라이브러리가 채워주는 통계(write amplification, gc activity 등). */
};

/*
 * [한국어] typedef ftl_bdev_init_fn
 * FTL bdev 생성 완료 콜백. 성공 시 info가 채워지고 status=0, 실패 시 status=음수.
 */
typedef void (*ftl_bdev_init_fn)(const struct ftl_bdev_info *, void *, int);

/*
 * [한국어] bdev_ftl_create_bdev - FTL 디바이스 + bdev 등록.
 * 비동기 — base/cache bdev open과 lib/ftl init이 끝나면 cb 호출.
 */
int bdev_ftl_create_bdev(const struct spdk_ftl_conf *conf, ftl_bdev_init_fn cb, void *cb_arg);
/*
 * [한국어] bdev_ftl_delete_bdev - FTL bdev 삭제.
 * @fast_shutdown: true면 caching된 내용을 flush 없이 종료(loss 허용 — 다음 부팅 시 복구).
 */
void bdev_ftl_delete_bdev(const char *name, bool fast_shutdown, spdk_bdev_unregister_cb cb_fn,
			  void *cb_arg);
/*
 * [한국어] bdev_ftl_defer_init - base bdev이 아직 등록 안 됐을 때 FTL 생성을 지연 큐에 보관.
 * 이후 base bdev가 등록되는 시점(examine_disk)에 자동으로 create.
 */
int bdev_ftl_defer_init(const struct spdk_ftl_conf *conf);
/*
 * [한국어] bdev_ftl_unmap - FTL bdev의 영역을 unmap (논리적으로 해제).
 */
void bdev_ftl_unmap(const char *name, uint64_t lba, uint64_t num_blocks, spdk_ftl_fn cb_fn,
		    void *cb_arg);

/**
 * @brief Get FTL bdev device statistics
 *
 * @param name The name of the FTL bdev device
 * @param cb Callback function when the stats are ready
 * @param ftl_stats_ctx The context for getting the statistics
 *
 * @note In callback function will return the context of rpc_ftl_stats_ctx
 * and it contains struct ftl_stats
 */
/*
 * [한국어] bdev_ftl_get_stats - stats 비동기 조회 + 콜백 보고.
 */
void bdev_ftl_get_stats(const char *name, spdk_ftl_fn cb, struct rpc_ftl_stats_ctx *ftl_stats_ctx);

/**
 * @brief Get FTL bdev device properties
 */
/*
 * [한국어] bdev_ftl_get_properties - FTL 튜닝 파라미터 조회 (JSON 응답으로 직접 출력).
 */
void bdev_ftl_get_properties(const char *name, spdk_ftl_fn cb_fn,
			     struct spdk_jsonrpc_request *request);

/**
 * @brief Set FTL bdev device property
 */
/*
 * [한국어] bdev_ftl_set_property - FTL 튜닝 파라미터 변경.
 */
void bdev_ftl_set_property(const char *name, const char *property, const char *value,
			   spdk_ftl_fn cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_FTL_H */
