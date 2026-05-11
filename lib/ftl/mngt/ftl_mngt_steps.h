/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 관리(state machine) step 콜백 카탈로그 헤더 (ftl_mngt_steps.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 ftl_mngt(FTL 관리 상태 머신)가 실행하는 모든 step 콜백 함수의 프로토타입을
 * 한 곳에 모아 노출하는 카탈로그(catalog)이다. 각 step 함수는 동일한 시그니처
 * `void f(spdk_ftl_dev *dev, ftl_mngt_process *mngt)` 를 가지며, ftl_mngt_process_desc의
 * `.steps[].action` 필드에 함수 포인터로 등록된다. 본 헤더는 어떤 모듈(L2P, P2L, MD,
 * IOCH, BAND, BDEV, MISC, RECOVERY, SELF_TEST, UPGRADE, SUPERBLOCK)이든 step 함수를
 * 정의했다면 모두 이 한 헤더에 선언되도록 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더는 ftl_mngt_startup.c / ftl_mngt_shutdown.c / ftl_mngt_recovery.c 등에서
 * step 배열을 선언적으로 구성할 때 #include 되어 모든 step 포인터를 노출한다.
 * 헤더 자체는 어떤 함수도 정의하지 않으며, 구현은 같은 디렉토리의 동일 prefix 파일들
 * (ftl_mngt_l2p.c → ftl_mngt_*l2p, ftl_mngt_p2l.c → ftl_mngt_*p2l*, 등)에 흩어져 있다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: lib/ftl/mngt/ftl_mngt.h (spdk_ftl_dev / ftl_mngt_process forward 선언).
 * - 의존받음: 같은 디렉토리의 모든 ftl_mngt_*.c (구현)와
 *   ftl_mngt_startup.c / ftl_mngt_shutdown.c / ftl_mngt_recovery.c (사용).
 * - 데이터 흐름: 함수 포인터 등록 → process_execute가 step.action을 호출 →
 *   각 step이 ftl_mngt_next_step / fail_step으로 결과 보고.
 *
 * === 주요 함수/구조체 요약 ===
 * 본 헤더에 선언된 step 함수들은 도메인별로 다음과 같이 분류:
 *  - 설정 검증/슈퍼블록: check_conf, superblock_init/deinit, init_default_sb, load_sb,
 *                       validate_sb, persist_superblock
 *  - bdev 자원: open/close base/cache_bdev
 *  - IO 채널: register/unregister_io_device, init/deinit_io_channel
 *  - 메모리 풀: init/deinit_mem_pools
 *  - 밴드: init/deinit_bands(_md), decorate_bands, initialize_band_address,
 *          finalize_init_bands, persist_band_info_metadata
 *  - 코어 폴러/리로케이터/NV cache: start/stop_core_poller, init/deinit_reloc,
 *                                   init/deinit_nv_cache, scrub_nv_cache,
 *                                   persist_nv_cache_metadata
 *  - L2P: init/deinit/clear/trim/restore/persist_l2p
 *  - 메타데이터 일반: init/deinit_md, persist_md, fast_persist_md, restore_md
 *  - 디바이스 라이프사이클: rollback_device, dump_stats, finalize_startup,
 *                          set_dirty/clean/shm_clean
 *  - 레이아웃: init_layout, layout_verify, layout_upgrade
 *  - 복구: recover, init/deinit_vld_map, init/deinit_trim_map,
 *          trim_metadata_clear, trim_log_clear
 *  - P2L: p2l_init/deinit_ckpt, p2l_wipe, p2l_log_io_wipe, p2l_free_bufs,
 *         p2l_restore_ckpt
 *  - 자가 점검: self_test
 */

#ifndef FTL_MNGT_STEPS_H
#define FTL_MNGT_STEPS_H

#include "ftl_mngt.h"
/* [한국어] spdk_ftl_dev / ftl_mngt_process 타입 정의(forward) — step 시그니처에 필요. */

/* [한국어] check_conf - 사용자가 넘긴 spdk_ftl_conf의 유효성(밴드/blocks 정합성, 캐시 크기,
 * core_mask 등)을 검사. 위반 시 fail_step. */
void ftl_mngt_check_conf(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] open_base_bdev - 데이터 저장용 베이스 NVMe bdev 열기(spdk_bdev_open_ext). */
void ftl_mngt_open_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] close_base_bdev - 베이스 bdev desc 닫기 + 채널 해제. */
void ftl_mngt_close_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] superblock_init - 슈퍼블록 메모리 버퍼/owners 초기화. 디스크 read는 별도 단계. */
void ftl_mngt_superblock_init(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] superblock_deinit - 슈퍼블록 메모리 자원 해제. */
void ftl_mngt_superblock_deinit(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] open_cache_bdev - NV(non-volatile) write cache용 bdev 열기 (예: persistent memory bdev). */
void ftl_mngt_open_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] close_cache_bdev - NV cache bdev 닫기. */
void ftl_mngt_close_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] register_io_device - spdk_io_device_register로 dev를 SPDK io_device로 등록.
 * 이후 모든 spdk_thread가 ftl_io_channel을 만들 수 있다. */
void ftl_mngt_register_io_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] unregister_io_device - 비동기 unregister (모든 채널 destroy 콜백 트리거). */
void ftl_mngt_unregister_io_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] init_mem_pools - rwb/io/p2l 등 mempool 생성(huge memory 기반). */
void ftl_mngt_init_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] deinit_mem_pools - 모든 mempool 해제. */
void ftl_mngt_deinit_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] init_bands - dev->num_bands개의 ftl_band 객체 배열 할당/초기화. */
void ftl_mngt_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] init_bands_md - 밴드 메타데이터 디스크 region 핸들(ftl_md *) 준비. */
void ftl_mngt_init_bands_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] deinit_bands - ftl_band 배열 해제. */
void ftl_mngt_deinit_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] deinit_bands_md - 밴드 MD 메모리 자원 해제. */
void ftl_mngt_deinit_bands_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] init_io_channel - 코어 스레드에서 dev->ioch 획득(spdk_get_io_channel). */
void ftl_mngt_init_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] deinit_io_channel - 코어 ioch 반환(spdk_put_io_channel). */
void ftl_mngt_deinit_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] decorate_bands - 각 ftl_band에 ID/MD pin/state 등 메타 속성 부여. */
void ftl_mngt_decorate_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] initialize_band_address - 각 밴드의 시작 LBA(addr) 계산해 layout에 매핑. */
void ftl_mngt_initialize_band_address(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] init_reloc - GC/relocation 모듈 초기화(ftl_reloc 자료구조 할당). */
void ftl_mngt_init_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] deinit_reloc - reloc 자원 해제. */
void ftl_mngt_deinit_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] init_nv_cache - NV(write) cache 모듈 초기화. */
void ftl_mngt_init_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] deinit_nv_cache - NV cache 자원 해제. */
void ftl_mngt_deinit_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] init_l2p - L2P(LBA→PBA) 매핑 자료구조 메모리 할당/초기화. */
void ftl_mngt_init_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] deinit_l2p - L2P 자원 해제. */
void ftl_mngt_deinit_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] clear_l2p - 모든 LBA 매핑을 unmapped로 초기화(첫 부팅용). */
void ftl_mngt_clear_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] trim_l2p - trim_map의 보류 trim을 L2P unmap으로 일괄 반영. */
void ftl_mngt_trim_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] restore_l2p - 디스크 L2P MD 또는 P2L log replay로 메모리 L2P 복원. */
void ftl_mngt_restore_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] scrub_nv_cache - NV cache 영역을 0으로 스크럽(첫 포맷). */
void ftl_mngt_scrub_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] finalize_init_bands - 밴드 초기화 후처리(가용 밴드 큐 등록). */
void ftl_mngt_finalize_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] finalize_startup - 디바이스 상태를 RUNNING으로 전환하고 startup 마무리. */
void ftl_mngt_finalize_startup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] start_core_poller - dev->core_poller 등록(GC/wbuf flush 등 백그라운드 잡 시작). */
void ftl_mngt_start_core_poller(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] stop_core_poller - dev->core_poller 해제. */
void ftl_mngt_stop_core_poller(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] persist_l2p - 메모리 L2P → 디스크 L2P MD region flush. */
void ftl_mngt_persist_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] init_layout - dev->layout(밴드/MD region 위치) 계산 및 검증. */
void ftl_mngt_init_layout(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] layout_verify - 디스크 layout이 슈퍼블록과 일치하는지 검증. */
void ftl_mngt_layout_verify(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] layout_upgrade - 구버전 layout을 현재 버전으로 region 단위 업그레이드. */
void ftl_mngt_layout_upgrade(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] init_md - 모든 MD region(ftl_md *)의 메모리 핸들 생성. */
void ftl_mngt_init_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] deinit_md - 모든 MD region 핸들 해제. */
void ftl_mngt_deinit_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] persist_md - 모든 메타데이터 region을 디스크에 비동기 flush. */
void ftl_mngt_persist_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] fast_persist_md - SHM에만 메타데이터 flush(빠른 셧다운). */
void ftl_mngt_fast_persist_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] rollback_device - startup에서 만든 모든 자원을 역순 cleanup으로 해제. */
void ftl_mngt_rollback_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] dump_stats - 운영 통계(write/read amp, GC count 등) 로그 출력. */
void ftl_mngt_dump_stats(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] init_default_sb - 새 디바이스용 기본 슈퍼블록 값 채우기. */
void ftl_mngt_init_default_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] set_dirty - 슈퍼블록의 clean 비트를 dirty로 변경(첫 write 직전). */
void ftl_mngt_set_dirty(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] set_clean - 슈퍼블록 clean 비트 set(정상 셧다운 끝). */
void ftl_mngt_set_clean(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] set_shm_clean - SHM 슈퍼블록만 clean 마크(빠른 셧다운). */
void ftl_mngt_set_shm_clean(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] load_sb - 디스크에서 슈퍼블록 read. */
void ftl_mngt_load_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] validate_sb - 슈퍼블록 magic/CRC/version 검증. */
void ftl_mngt_validate_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] restore_md - 모든 MD region을 디스크에서 메모리로 복원. */
void ftl_mngt_restore_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] recover - dirty shutdown 후 P2L log replay 기반 풀 복구 sub-process. */
void ftl_mngt_recover(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] init_vld_map - 밴드별 valid bitmap 메모리 할당. */
void ftl_mngt_init_vld_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] deinit_vld_map - vld_map 해제. */
void ftl_mngt_deinit_vld_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] init_trim_map - trim 누적용 비트맵 할당. */
void ftl_mngt_init_trim_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] deinit_trim_map - trim_map 해제. */
void ftl_mngt_deinit_trim_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] trim_metadata_clear - trim MD region을 디스크에서 0으로 클리어. */
void ftl_mngt_trim_metadata_clear(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] trim_log_clear - trim log MD region 클리어. */
void ftl_mngt_trim_log_clear(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] p2l_init_ckpt - P2L checkpoint 모듈 초기화(슬롯/큐 준비). */
void ftl_mngt_p2l_init_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] p2l_deinit_ckpt - P2L checkpoint 모듈 해제. */
void ftl_mngt_p2l_deinit_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] p2l_wipe - 모든 P2L checkpoint MD region을 0으로 클리어(첫 포맷). */
void ftl_mngt_p2l_wipe(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] p2l_log_io_wipe - 모든 P2L IO log MD region 클리어(P2L log 옵션 활성 시만). */
void ftl_mngt_p2l_log_io_wipe(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] p2l_free_bufs - P2L checkpoint MD 버퍼 해제(메모리 회수). */
void ftl_mngt_p2l_free_bufs(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] p2l_restore_ckpt - 디스크 P2L checkpoint를 메모리로 복원(recovery). */
void ftl_mngt_p2l_restore_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] self_test - startup 마지막에 디바이스 자가 점검(read-back 등) 수행. */
void ftl_mngt_self_test(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] persist_band_info_metadata - 밴드 정보 MD만 디스크에 flush. */
void ftl_mngt_persist_band_info_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] persist_nv_cache_metadata - NV cache MD만 디스크에 flush. */
void ftl_mngt_persist_nv_cache_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

/* [한국어] persist_superblock - 슈퍼블록만 디스크에 flush. */
void ftl_mngt_persist_superblock(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

#endif /* FTL_MNGT_STEPS_H */
