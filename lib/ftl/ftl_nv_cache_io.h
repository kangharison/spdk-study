/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL NV cache I/O 어댑터 (ftl_nv_cache_io.h)
 *
 * === 파일의 역할 ===
 * NV cache 백엔드 bdev이 metadata-per-block(VSS/별도 메타데이터 영역)을 지원하는지 여부에
 * 따라 spdk_bdev_read_blocks 계열 호출과 spdk_bdev_read_blocks_with_md 계열 호출을 자동으로
 * 분기시키는 얇은 래퍼이다. 호출자는 항상 with_md 시그니처(buf, md 포인터 모두 전달)로
 * 통일해서 호출하면 되고, 메타데이터를 지원하지 않는 bdev이면 md 인자는 무시되고 일반
 * read/write 경로가 선택된다. md == NULL 인 경우에는 g_ftl_read_buf / g_ftl_write_buf라는
 * 전역 더미 버퍼가 대신 사용되어 NVMe 드라이버에 비-NULL 메타데이터 포인터 요구를 충족시킨다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * NV cache 데이터/메타데이터 모든 read/write 경로가 거치는 단일 진입점.
 * 호출 체인: ftl_nv_cache.c / ftl_nv_cache_chunk.c / ftl_writer.c → 이 헬퍼 →
 *   spdk_bdev_*_blocks{,_with_md} → 백엔드 bdev 모듈 → NVMe 드라이버.
 * 실행 컨텍스트: SPDK reactor 스레드(NV cache 전용 io_channel을 가진 스레드).
 *   비동기 호출이며 완료는 cb 콜백으로 통보된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/bdev.h(spdk_bdev API), ftl_core.h(g_ftl_read_buf/g_ftl_write_buf 전역).
 * 의존되는 모듈: ftl_nv_cache.c, ftl_nv_cache_chunk.c, ftl_writer.c, ftl_l2p_cache.c 등
 *   NV cache에 데이터를 적재/회수하는 모든 코드.
 * 데이터 흐름: 호출자가 buf/md를 준비 → bdev 큐에 SQE 제출 → 백엔드(NVMe)가 DMA 수행 →
 *   완료 시 cb(cb_arg, success, bdev_io) 호출.
 * 공유 상태: g_ftl_read_buf, g_ftl_write_buf 전역 더미 버퍼 — md 포인터가 NULL인 경우의
 *   write-pad/read-pad 용도로 ftl_core가 사전 할당한다.
 *
 * === 주요 함수/구조체 요약 ===
 * 모두 인라인 래퍼이며 새 구조체는 정의하지 않는다.
 *   - ftl_nv_cache_bdev_read_blocks_with_md: NV cache에서 데이터+메타데이터를 비동기 read.
 *     bdev이 md 미지원이면 일반 read_blocks로 폴백.
 *   - ftl_nv_cache_bdev_write_blocks_with_md: write 버전. read와 대칭 분기.
 */

#ifndef FTL_NV_CACHE_IO_H
#define FTL_NV_CACHE_IO_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "spdk/bdev.h"
/* [한국어] spdk_bdev API — spdk_bdev_read_blocks{,_with_md}, spdk_bdev_write_blocks{,_with_md},
 * spdk_bdev_get_md_size, spdk_bdev_desc_get_bdev, spdk_bdev_io_completion_cb 타입. */
#include "ftl_core.h"
/* [한국어] g_ftl_read_buf, g_ftl_write_buf 전역 더미 버퍼 선언과 그 외 FTL 코어 타입을 가져온다.
 * 이 두 전역은 NVMe write protected information(메타데이터)이 의무인 bdev에 NULL md 인자를
 * 넘기지 않도록 안전망 역할. */

/*
 * [한국어]
 * ftl_nv_cache_bdev_read_blocks_with_md - NV cache 백엔드 bdev에서 data+md 블록을 비동기 read
 *
 * @param desc: NV cache bdev에 대한 spdk_bdev_desc(오픈 핸들).
 * @param ch: 호출 스레드의 NV cache I/O 채널.
 * @param buf: 데이터를 받을 DMA-친화 버퍼 (호출자가 hugepage memory로 할당).
 * @param md: 블록당 메타데이터(예: NVMe DIF/PI, VSS 8B 등)를 받을 버퍼. NULL이면 g_ftl_read_buf 사용.
 * @param offset_blocks: 시작 블록 오프셋 (4KiB 단위, ftl_block 기준).
 * @param num_blocks: 읽을 블록 개수.
 * @param cb: 완료 콜백 (성공/실패 포함). 채널이 속한 SPDK 스레드에서 비동기 호출됨.
 * @param cb_arg: cb에 그대로 전달될 컨텍스트 포인터.
 * @return: 0 = 제출 성공(완료는 추후 cb로), 음수 = 즉시 에러(예: -ENOMEM).
 *
 * 동기/배경: NV cache로 쓰이는 bdev은 metadata-per-block을 지원하기도 하고 안 하기도 한다.
 *   호출자가 매번 두 경로를 if/else로 가르지 않도록 통합하기 위한 어댑터.
 * 동작: spdk_bdev_get_md_size()로 md 크기 조회 → 0이 아니면 with_md API로, 0이면 일반 API로 분기.
 *   md == NULL이면 전역 g_ftl_read_buf로 대체해 NULL 인자 요구사항을 회피.
 * 실행 컨텍스트: SPDK reactor 스레드 — desc/ch가 같은 스레드에 묶여 있어야 lockless 안전.
 * caller: NV cache 영속 데이터 read 경로 전반 (chunk md 로드, L2P 캐시 페이지 read 등).
 * callee: spdk_bdev_read_blocks_with_md / spdk_bdev_read_blocks (둘 다 비동기 제출 API).
 * 에러 경로: 백엔드 bdev이 큐 가득 등으로 -ENOMEM 반환 시 즉시 에러 코드 반환 — 호출자가 재시도/큐 처리.
 *
 * 호출 체인:
 *   ftl_nv_cache_chunk_md_load / ftl_l2p_cache_read → [이 함수] → spdk_bdev_*_blocks(_with_md)
 */
static inline int
ftl_nv_cache_bdev_read_blocks_with_md(struct spdk_bdev_desc *desc,
				      struct spdk_io_channel *ch,
				      void *buf, void *md,
				      uint64_t offset_blocks, uint64_t num_blocks,
				      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (spdk_bdev_get_md_size(spdk_bdev_desc_get_bdev(desc))) {
		/* [한국어] desc로부터 bdev 객체를 얻고 metadata-per-block 크기 조회.
		 * 0이 아니면 NVMe namespace에 별도 메타데이터(DIF/PI 또는 VSS)가 있는 bdev이라는 뜻이며,
		 * 모든 I/O는 데이터 버퍼와 메타데이터 버퍼 한 쌍으로 제출되어야 한다. */
		return spdk_bdev_read_blocks_with_md(desc, ch, buf, md ? : g_ftl_read_buf,
						     offset_blocks, num_blocks, cb, cb_arg);
		/* [한국어] with_md API 호출 — md != NULL 이면 그 버퍼를, NULL 이면 g_ftl_read_buf로 대체.
		 * GNU 확장 "?:" (md ? md : g_ftl_read_buf)로 한 줄 처리. 데이터 폐기처럼 메타가 무의미한
		 * 호출자도 안전하게 NULL을 넘길 수 있게 한다. 반환값은 spdk_bdev의 즉시 에러 코드. */
	} else {
		/* [한국어] bdev이 metadata-per-block 미지원(md_size == 0) — 일반 read API 사용. */
		return spdk_bdev_read_blocks(desc, ch, buf, offset_blocks, num_blocks,
					     cb, cb_arg);
		/* [한국어] 일반 read_blocks 호출 — md 인자 자체가 없는 시그니처이므로 호출자의 md는 무시.
		 * 백엔드는 데이터 영역만 DMA로 채우고 cb로 완료를 통보한다. */
	}
}

/*
 * [한국어]
 * ftl_nv_cache_bdev_write_blocks_with_md - NV cache 백엔드 bdev에 data+md 블록을 비동기 write
 *
 * @param desc: NV cache bdev에 대한 spdk_bdev_desc.
 * @param ch: 호출 스레드의 NV cache I/O 채널.
 * @param buf: 쓸 데이터가 들어있는 DMA-친화 버퍼.
 * @param md: 블록당 메타데이터 버퍼. NULL이면 g_ftl_write_buf로 대체.
 * @param offset_blocks: 쓰기 시작 블록 오프셋.
 * @param num_blocks: 쓸 블록 개수.
 * @param cb: 완료 콜백.
 * @param cb_arg: 콜백 컨텍스트.
 * @return: 0 = 제출 성공, 음수 = 즉시 에러.
 *
 * 동기/배경: read의 대칭. NV cache 데이터/메타 영속화 시 사용.
 * 실행 컨텍스트: SPDK reactor 스레드.
 *
 * 호출 체인:
 *   ftl_nv_cache 데이터 write / chunk md 영속화 → [이 함수] → spdk_bdev_*_blocks(_with_md)
 */
static inline int
ftl_nv_cache_bdev_write_blocks_with_md(struct spdk_bdev_desc *desc,
				       struct spdk_io_channel *ch,
				       void *buf, void *md,
				       uint64_t offset_blocks, uint64_t num_blocks,
				       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (spdk_bdev_get_md_size(spdk_bdev_desc_get_bdev(desc))) {
		/* [한국어] bdev이 metadata-per-block을 지원 — with_md API로 분기. */
		return spdk_bdev_write_blocks_with_md(desc, ch, buf, md ? : g_ftl_write_buf,
						      offset_blocks, num_blocks, cb, cb_arg);
		/* [한국어] md 가 NULL 이면 g_ftl_write_buf 사용. 백엔드는 데이터+메타 한 쌍을 DMA로 기록. */
	} else {
		/* [한국어] bdev이 메타데이터 미지원 — 일반 write API. */
		return spdk_bdev_write_blocks(desc, ch, buf, offset_blocks, num_blocks,
					      cb, cb_arg);
		/* [한국어] md 인자 없는 시그니처. 호출자의 md는 무시되며 데이터만 기록. */
	}
}

#endif /* FTL_NV_CACHE_IO_H */
/* [한국어] 헤더 가드 종료. */
