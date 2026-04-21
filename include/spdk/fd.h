/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * OS filesystem utility functions
 */

/*
 * [한국어 설명] 파일 디스크립터 유틸리티 (fd.h)
 *
 * === 파일의 역할 ===
 * 파일 디스크립터로 일반적으로 얻기 까다로운 메타데이터(파일 크기, 블록
 * 크기)와 자주 사용하는 플래그 조작(O_NONBLOCK on/off)을 간단한 래퍼로
 * 제공한다. 특히:
 *   - 일반 파일과 블록 디바이스(raw /dev/nvme0n1 등)를 동일한 API로 크기
 *     조회 가능하게 만든다. 내부 구현은 fstat + BLKGETSIZE64 ioctl로 분기.
 *   - 호출자가 fcntl F_GETFL/F_SETFL의 장황한 시퀀스를 반복하지 않도록
 *     set/clear 래퍼 제공.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 공개 유틸 헤더. 구현은 lib/util/fd.c.
 * 주요 사용처:
 *   - bdev_aio/bdev_uring 모듈: 백엔드 파일/디바이스 크기 확인
 *   - 각종 CLI 도구, 테스트 바이너리
 * 실행 컨텍스트: 초기화 경로 중심(hot path 아님). 단 nonblock 토글은 런타임
 * 중에도 호출될 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h — uint32_t/64_t, int 타입.
 * 의존하는 모듈: module/bdev/aio, module/bdev/uring, app/spdk_dd 등.
 * 공유 자료구조: 없음.
 * 데이터 흐름: fd에서 메타데이터 조회 → 호출자에게 반환.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_fd_get_size(fd):       파일/블록 디바이스 크기 (바이트)
 *   - spdk_fd_get_blocklen(fd):   논리 블록 크기 (바이트)
 *   - spdk_fd_set_nonblock(fd):   O_NONBLOCK ON
 *   - spdk_fd_clear_nonblock(fd): O_NONBLOCK OFF
 */

#ifndef SPDK_FD_H                /* [한국어] include 가드 */
#define SPDK_FD_H

#include "spdk/stdinc.h"         /* [한국어] uint32_t/uint64_t/int 확보 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the file size.
 *
 * \param fd  File descriptor.
 *
 * \return    File size.
 */
uint64_t spdk_fd_get_size(int fd);
/*
 * [한국어]
 * spdk_fd_get_size - fd가 가리키는 파일/디바이스의 총 크기(바이트) 반환
 *
 * @fd: open()/dup() 등으로 얻은 유효한 파일 디스크립터
 * @return: 바이트 단위 크기. 실패 시 0 (errno는 설정될 수 있음)
 *
 * 구현 개요 (lib/util/fd.c):
 *   1) fstat(fd, &st) 호출하여 S_IFBLK/S_IFREG 판별
 *   2) 블록 디바이스: ioctl(fd, BLKGETSIZE64, &size) — 리눅스 전용. 논리 블록 수 * 블록 크기가 아닌 실제 바이트 수
 *   3) 일반 파일: st.st_size 그대로 반환
 *
 * 실행 컨텍스트: 초기화 경로. ioctl은 짧게 블로킹될 수 있으나 실질 대기 없음.
 * 호출 체인: bdev 모듈 초기화 → [이 함수] → 이후 bdev num_blocks/block_size 설정
 */

/**
 * Get the block size of the file.
 *
 * \param fd  File descriptor.
 *
 * \return    Block size.
 */
uint32_t spdk_fd_get_blocklen(int fd);
/*
 * [한국어]
 * spdk_fd_get_blocklen - 논리 블록 크기 반환
 *
 * @fd: 파일 디스크립터
 * @return: 바이트 단위 블록 크기. 블록 디바이스는 ioctl(BLKSSZGET), 일반 파일은 st.st_blksize(혹은 기본값 4096)
 *
 * 이 값은 bdev의 block_size로 노출되며, SPDK bdev I/O의 최소 단위를 결정.
 */

/**
 * Set O_NONBLOCK file status flag.
 *
 * \param fd  File descriptor.
 *
 * \return    0 on success, negative errno value on failure.
 */
int spdk_fd_set_nonblock(int fd);
/*
 * [한국어]
 * spdk_fd_set_nonblock - O_NONBLOCK 플래그 설정
 *
 * @fd: 대상 fd
 * @return: 0 성공, 음수 errno 실패
 *
 * 내부: fcntl(F_GETFL) → fl |= O_NONBLOCK → fcntl(F_SETFL) 시퀀스 캡슐화.
 * 사용처: 소켓·pipe·eventfd 등 polled-mode에서 논블로킹으로 전환
 */

/**
 * Clear O_NONBLOCK file status flag.
 *
 * \param fd  File descriptor.
 *
 * \return    0 on success, negative errno value on failure.
 */
int spdk_fd_clear_nonblock(int fd);
/*
 * [한국어]
 * spdk_fd_clear_nonblock - O_NONBLOCK 플래그 해제 (블로킹 모드로 복귀)
 *
 * 사용처: SPDK가 외부로 fd를 넘겨주기 전 블로킹 의미 보장이 필요한 경우
 */

#ifdef __cplusplus
}
#endif

#endif                           /* [한국어] include 가드 종료 */
