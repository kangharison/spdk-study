/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Zoned device public interface
 */

/*
 * [한국어 설명] Zoned bdev(ZNS) 공개 API (bdev_zone.h) — 296 라인
 *
 * === 파일의 역할 ===
 * NVMe ZNS(Zoned Namespace) / ZAC/ZBC(SMR HDD) 계열의 zoned 스토리지에
 * 대한 SPDK bdev 레이어 공개 API. 일반 bdev의 read/write API는 그대로
 * 사용 가능하나, zone 관리·zone append 등 zoned 특유 연산은 본 헤더로
 * 노출된다.
 *
 * Zoned 장치 특성:
 *   - 전체 LBA 공간을 고정 크기 zone으로 분할 (zone_size 배수)
 *   - 각 zone은 write pointer 기준 sequential write 강제
 *   - zone 상태머신: EMPTY → IMP_OPEN/EXP_OPEN → (CLOSED) → FULL → RESET → EMPTY
 *   - open/active zone 수에 하드웨어 상한 (max_open_zones, max_active_zones)
 *
 * 주요 기능 그룹:
 *   (1) 속성 조회: zone_size / num_zones / zone_id 계산 / max_zone_append_size /
 *       max_open/active_zones / optimal_open_zones
 *   (2) 상태 조회: get_zone_info (여러 zone 정보 배열)
 *   (3) 상태 변경: zone_management (OPEN/CLOSE/FINISH/RESET/OFFLINE)
 *   (4) Zone Append: append / appendv / append_with_md / appendv_with_md —
 *       호스트가 LBA를 선정하지 않고 장치가 wp 자동 할당 → 병렬 write 가능
 *   (5) append 완료 후 실제 할당된 LBA 조회 (get_append_location)
 *
 * === 전체 아키텍처에서의 위치 ===
 * bdev.h를 확장하는 공개 헤더. spdk_bdev_is_zoned(bdev)=true인 bdev에 한해
 * 이 API들이 의미 있다. bdev 모듈(module/bdev/nvme, module/bdev/zone_block)이
 * zone 연산을 실제 장치로 전달. 호출 흐름:
 *   app → spdk_bdev_zone_append → fn_table.submit_request →
 *   모듈(bdev_nvme) → NVMe ZONE APPEND 커맨드 (opcode 0x7D)
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/bdev.h (공통 bdev API) / spdk/stdinc.h
 * 의존하는 모듈: module/bdev/zone_block(emulated), module/bdev/nvme(ZNS 네임스페이스),
 *             module/bdev/ftl(FTL이 ZNS backed인 경우)
 *
 * === 주요 함수/구조체 요약 ===
 *   - enum spdk_bdev_zone_type:   CNV(convention)/SEQWR/SEQWP
 *   - enum spdk_bdev_zone_action: CLOSE/FINISH/OPEN/RESET/OFFLINE
 *   - enum spdk_bdev_zone_state:  EMPTY/IMP_OPEN/FULL/CLOSED/READ_ONLY/OFFLINE/EXP_OPEN/NOT_WP
 *   - struct spdk_bdev_zone_info: zone_id/write_pointer/capacity/state/type
 *   - 속성 getter 6종
 *   - get_zone_info / zone_management
 *   - zone_append 4종 (buf/iov × md 유무)
 *   - get_append_location (append 완료 후 실제 LBA 조회)
 */

#ifndef SPDK_BDEV_ZONE_H         /* [한국어] include 가드 */
#define SPDK_BDEV_ZONE_H

#include "spdk/stdinc.h"         /* [한국어] 표준 타입 */
#include "spdk/bdev.h"           /* [한국어] 공개 bdev 타입 (spdk_bdev_desc, io_channel, io_completion_cb) */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief SPDK block device.
 *
 * This is a virtual representation of a block device that is exported by the backend.
 */

struct spdk_bdev;                 /* [한국어] opaque — 정의는 bdev_module.h */

/*
 * [한국어] ZNS zone 타입 (스펙 §6.1, ZNS Command Set)
 */
enum spdk_bdev_zone_type {
	SPDK_BDEV_ZONE_TYPE_CNV		= 0x1,
                                  /* [한국어] Conventional zone — 일반 bdev처럼 무제약 R/W (일부 장치의 메타데이터 zone 용도) */
	SPDK_BDEV_ZONE_TYPE_SEQWR	= 0x2,
                                  /* [한국어] Sequential write required — wp 순차 강제 (일반 ZNS zone) */
	SPDK_BDEV_ZONE_TYPE_SEQWP	= 0x3,
                                  /* [한국어] Sequential write preferred — 순차가 권장되나 강제는 아님 (SMR HDD 계열) */
};

/*
 * [한국어] spdk_bdev_zone_management의 action 파라미터
 *
 * 상태 전이:
 *   OPEN    : EMPTY/CLOSED → EXP_OPEN (명시적 열기, 쓰기 준비)
 *   CLOSE   : IMP_OPEN/EXP_OPEN → CLOSED (wp 유지, 리소스 반환)
 *   FINISH  : 현 상태 → FULL (남은 공간 폐쇄, wp를 zone 끝으로 이동)
 *   RESET   : 모든 상태 → EMPTY (데이터 폐기, wp=zone_id)
 *   OFFLINE : 장치 판단으로 zone 비활성 (재활성화 불가)
 */
enum spdk_bdev_zone_action {
	SPDK_BDEV_ZONE_CLOSE,         /* [한국어] zone 자원 반환 (wp 유지) */
	SPDK_BDEV_ZONE_FINISH,        /* [한국어] zone을 FULL로 강제 마감 */
	SPDK_BDEV_ZONE_OPEN,          /* [한국어] zone 명시적 열기 (EXP_OPEN) */
	SPDK_BDEV_ZONE_RESET,         /* [한국어] zone 데이터 폐기 + wp 초기화 */
	SPDK_BDEV_ZONE_OFFLINE,       /* [한국어] zone 비활성화 (복구 불가) */
};

/*
 * [한국어] zone 상태
 *
 * OPEN/CLOSED/FULL/EMPTY 순회가 일반적. READ_ONLY·OFFLINE은 복구 실패 지표.
 */
enum spdk_bdev_zone_state {
	SPDK_BDEV_ZONE_STATE_EMPTY	= 0x0,
                                  /* [한국어] 비어있음 — write 시 IMP_OPEN으로 자동 전이 */
	SPDK_BDEV_ZONE_STATE_IMP_OPEN	= 0x1,
                                  /* [한국어] Implicit Open — write로 자동 열림. 리소스 회수 대상 */
	/* OPEN is an alias for IMP_OPEN. OPEN is kept for backwards compatibility. */
	SPDK_BDEV_ZONE_STATE_OPEN	= SPDK_BDEV_ZONE_STATE_IMP_OPEN,
                                  /* [한국어] 하위 호환: 구버전 SPDK의 OPEN 별칭 */
	SPDK_BDEV_ZONE_STATE_FULL	= 0x2,
                                  /* [한국어] zone이 가득 찬 상태 — 추가 write 불가, RESET으로만 재사용 */
	SPDK_BDEV_ZONE_STATE_CLOSED	= 0x3,
                                  /* [한국어] 닫힘 (wp 유지) — 다시 write하려면 OPEN 필요 */
	SPDK_BDEV_ZONE_STATE_READ_ONLY	= 0x4,
                                  /* [한국어] 읽기 전용 (장치 오류 대응) */
	SPDK_BDEV_ZONE_STATE_OFFLINE	= 0x5,
                                  /* [한국어] 오프라인 (복구 불가) */
	SPDK_BDEV_ZONE_STATE_EXP_OPEN	= 0x6,
                                  /* [한국어] Explicit Open — 명시적 OPEN 액션으로 열림. 자동 닫히지 않음 */
	SPDK_BDEV_ZONE_STATE_NOT_WP	= 0x7,
                                  /* [한국어] write pointer 없음 (Conventional zone) */
};

/*
 * [한국어] struct spdk_bdev_zone_info - 하나의 zone 상태 정보
 *
 * get_zone_info 완료 후 info[] 배열에 채워져 반환. 호스트가 zone 재할당
 * 정책(예: 가장 EMPTY인 zone 선택) 수립 시 참조.
 */
struct spdk_bdev_zone_info {
	uint64_t			zone_id;
                                  /* [한국어] zone 시작 LBA (zslba). zone_size 배수 */
	uint64_t			write_pointer;
                                  /* [한국어] 현재 write pointer — 다음 write가 이 LBA에 기록됨
                                   *  - EMPTY zone: wp == zone_id
                                   *  - FULL zone: wp == zone_id + capacity
                                   *  - Conventional zone: 의미 없음 */
	uint64_t			capacity;
                                  /* [한국어] 실제 write 가능 용량 (블록) — zone_size보다 작을 수 있음(장치가 일부를 metadata로 예약) */
	enum spdk_bdev_zone_state	state;
                                  /* [한국어] 현재 zone 상태 */
	enum spdk_bdev_zone_type	type;
                                  /* [한국어] zone 타입 (CNV/SEQWR/SEQWP) */
};

/**
 * Get device zone size in logical blocks.
 *
 * \param bdev Block device to query.
 * \return Size of zone for this zoned device in logical blocks.
 */
uint64_t spdk_bdev_get_zone_size(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_zone_size - zone 하나의 크기(블록) 반환
 *
 * ZNS 일반 값: 1GiB (2^18 블록 × 4KB). 모든 zone은 이 크기. 마지막 zone은 예외로 capacity < zone_size일 수 있음.
 */

/**
 * Get the number of zones for the given device.
 *
 * \param bdev Block device to query.
 * \return The number of zones.
 */
uint64_t spdk_bdev_get_num_zones(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_num_zones - 총 zone 개수 (= blockcnt / zone_size, 올림)
 */

/**
 * Get the first logical block of a zone (known as zone_id or zslba)
 * for a given offset.
 *
 * \param bdev Block device to query.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \return The zone_id (also known as zslba) for the given offset.
 */
uint64_t spdk_bdev_get_zone_id(const struct spdk_bdev *bdev, uint64_t offset_blocks);
/*
 * [한국어]
 * spdk_bdev_get_zone_id - 임의 LBA가 속한 zone의 시작 LBA 반환
 * = offset_blocks - (offset_blocks % zone_size)
 * 사용처: read/write 시 경계 체크, zone_management 인자 계산
 */

/**
 * Get device maximum zone append data transfer size in logical blocks.
 *
 * If this value is 0, there is no limit.
 *
 * \param bdev Block device to query.
 * \return Maximum zone append data transfer size for this zoned device in logical blocks.
 */
uint32_t spdk_bdev_get_max_zone_append_size(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_max_zone_append_size - 단일 zone_append 최대 블록 수 (0=무제한)
 * zone append는 MDTS와 별도 한도(ZASL). 초과 시 bdev 레이어가 -EINVAL.
 */

/**
 * Get device maximum number of open zones.
 *
 * An open zone is defined as a zone being in zone state
 * SPDK_BDEV_ZONE_STATE_IMP_OPEN or SPDK_BDEV_ZONE_STATE_EXP_OPEN.
 *
 * If this value is 0, there is no limit.
 *
 * \param bdev Block device to query.
 * \return Maximum number of open zones for this zoned device.
 */
uint32_t spdk_bdev_get_max_open_zones(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_max_open_zones - 동시 열림 가능한 zone 최대 수 (0=무제한)
 * 초과 시 IMP_OPEN이 자동으로 CLOSED로 밀림. 호스트 스케줄러가 이 한도를 알아야 오픈 전략 수립 가능.
 */

/**
 * Get device maximum number of active zones.
 *
 * An active zone is defined as a zone being in zone state
 * SPDK_BDEV_ZONE_STATE_IMP_OPEN, SPDK_BDEV_ZONE_STATE_EXP_OPEN or
 * SPDK_BDEV_ZONE_STATE_CLOSED.
 *
 * If this value is 0, there is no limit.
 *
 * \param bdev Block device to query.
 * \return Maximum number of active zones for this zoned device.
 */
uint32_t spdk_bdev_get_max_active_zones(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_max_active_zones - active(open+closed) zone 최대 수 (0=무제한)
 * active 한도 초과 시 새 write 시도가 실패 → FINISH 또는 RESET로 active 줄여야 함.
 */

/**
 * Get device optimal number of open zones.
 *
 * \param bdev Block device to query.
 * \return Optimal number of open zones for this zoned device.
 */
uint32_t spdk_bdev_get_optimal_open_zones(const struct spdk_bdev *bdev);
/*
 * [한국어]
 * spdk_bdev_get_optimal_open_zones - 권장 open zone 수 (성능 최적치)
 * 장치가 힌트로 제공. 초과해도 동작하지만 내부 자원 경합·레이턴시 증가 가능.
 */

/**
 * Submit a get_zone_info request to the bdev.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param zone_id First logical block of a zone.
 * \param num_zones Number of consecutive zones info to retrieve.
 * \param info Pointer to array capable of storing num_zones elements.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_get_zone_info(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    uint64_t zone_id, size_t num_zones, struct spdk_bdev_zone_info *info,
			    spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_get_zone_info - 연속 num_zones개 zone 상태 조회
 *
 * @zone_id: 시작 zone의 zslba
 * @num_zones: 조회할 zone 수 (연속)
 * @info: num_zones 크기 배열 (호출자 할당). 완료 시 각 zone 정보로 채워짐
 *
 * 사용처: 호스트 측 zone allocator가 EMPTY/FULL/OPEN 상태 분포 파악 시.
 * NVMe Zone Management Receive 커맨드에 매핑.
 */


/**
 * Submit a zone_management request to the bdev.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param zone_id First logical block of a zone.
 * \param action Action to perform on a zone (open, close, reset, finish, offline).
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_zone_management(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      uint64_t zone_id, enum spdk_bdev_zone_action action,
			      spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_zone_management - ★ zone 상태 전환 명령 ★
 *
 * @zone_id: 대상 zone의 시작 LBA. 특수값 (SPDK_BDEV_ZONE_ID_ALL 등) 일부 장치 지원
 * @action: OPEN/CLOSE/FINISH/RESET/OFFLINE 중 하나
 *
 * 대표 시나리오:
 *   - log append 구조: write하다가 zone FULL 가까워지면 FINISH → 다음 zone OPEN
 *   - 가비지 컬렉션: valid 데이터 옮기고 RESET으로 전체 zone 재활용
 *   - 셧다운: 모든 IMP_OPEN zone을 CLOSE로 깔끔히 닫아 리소스 반환
 *
 * NVMe Zone Management Send 커맨드로 매핑.
 */

/**
 * Submit a zone_append request to the bdev.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to written from.
 * \param zone_id First logical block of a zone.
 * \param num_blocks The number of blocks to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed).
 * Appended logical block address can be obtained with spdk_bdev_io_get_append_location().
 * Return negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_zone_append(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  void *buf, uint64_t zone_id, uint64_t num_blocks,
			  spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_zone_append - ★ ZNS Zone Append (핵심 기능) ★
 *
 * 일반 write와 달리 호스트가 정확한 LBA를 지정하지 않음 — zone_id만 지정하면
 * 장치가 zone 내부에서 wp를 원자적으로 증가시키며 자동 할당. 완료 시 실제
 * 할당 LBA를 spdk_bdev_io_get_append_location()으로 조회 가능.
 *
 * 이점:
 *   - 여러 스레드가 같은 zone에 동시 append 가능 (wp race 없음)
 *   - 호스트가 순차 제약 관리 부담 없이 병렬 전송
 *   - 장치가 wp 관리하므로 write ordering 책임이 장치에 있음
 *
 * 제약: buf는 num_blocks × blocklen 이상, num_blocks ≤ max_zone_append_size.
 * NVMe ZONE APPEND(opcode 0x7D)에 매핑.
 */

/**
 * Submit a zone_append request to the bdev. This differs from
 * spdk_bdev_zone_append by allowing the data buffer to be described in a scatter
 * gather list.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iov.
 * \param zone_id First logical block of a zone.
 * \param num_blocks The number of blocks to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed).
 * Appended logical block address can be obtained with spdk_bdev_io_get_append_location().
 * Return negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_zone_appendv(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt, uint64_t zone_id, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_zone_appendv - scatter-gather 버전 Zone Append
 */

/**
 * Submit a zone_append request with metadata to the bdev.
 *
 * This function uses separate buffer for metadata transfer (valid only if bdev supports this
 * mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to written from.
 * \param md Metadata buffer.
 * \param zone_id First logical block of a zone.
 * \param num_blocks The number of blocks to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed).
 * Appended logical block address can be obtained with spdk_bdev_io_get_append_location().
 * Return negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_zone_append_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				  void *buf, void *md, uint64_t zone_id, uint64_t num_blocks,
				  spdk_bdev_io_completion_cb cb, void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_zone_append_with_md - Zone Append + 분리 메타데이터 버퍼 (PI)
 */

/**
 * Submit a zone_append request with metadata to the bdev. This differs from
 * spdk_bdev_zone_append by allowing the data buffer to be described in a scatter
 * gather list.
 *
 * This function uses separate buffer for metadata transfer (valid only if bdev supports this
 * mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iov.
 * \param md Metadata buffer.
 * \param zone_id First logical block of a zone.
 * \param num_blocks The number of blocks to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed).
 * Appended logical block address can be obtained with spdk_bdev_io_get_append_location().
 * Return negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_zone_appendv_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				   struct iovec *iov, int iovcnt, void *md, uint64_t zone_id,
				   uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
				   void *cb_arg);
/*
 * [한국어]
 * spdk_bdev_zone_appendv_with_md - scatter-gather + 메타 버퍼 Zone Append
 */

/**
 * Get append location (offset in blocks of the bdev) for this I/O.
 *
 * \param bdev_io I/O to get append location from.
 */
uint64_t spdk_bdev_io_get_append_location(struct spdk_bdev_io *bdev_io);
/*
 * [한국어]
 * spdk_bdev_io_get_append_location - Zone Append 완료 후 실제 할당된 LBA 조회
 *
 * @bdev_io: 완료 콜백으로 전달된 bdev_io (Zone Append 타입)
 * @return: 장치가 wp를 증가시키며 할당한 시작 LBA
 *
 * 사용처: 애플리케이션이 log-structured 자료구조에서 각 append 데이터의 위치를
 *   인덱스로 기록해야 할 때. 예: key-value store의 LSM-tree SSTable 인덱스.
 */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_ZONE_H */
