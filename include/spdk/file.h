/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * file operation functions
 */

/*
 * [한국어 설명] POSIX 파일/sysfs 읽기 헬퍼 공개 API (file.h)
 *
 * === 파일의 역할 ===
 * SPDK 내부에서 자주 쓰이는 **소형 파일 읽기 유틸**(주로 텍스트성 파일을 통째로 메모리로 로드)을
 * 제공한다. 두 카테고리:
 *   1. 일반 파일 통째로 로드 (`spdk_posix_file_load*`) — 설정 파일/json/cert 등
 *   2. /sys/... 의 sysfs 속성 읽기 (`spdk_read_sysfs_attribute*`) — PCI 디바이스 정보, NUMA, CPU
 *      토폴로지 같은 커널 노출 메타데이터 조회
 *
 * 구현은 `lib/util/file.c`에 있고, 모두 동기 blocking I/O이므로 **I/O hot-path가 아닌 초기화/구성
 * 경로**에서만 호출되어야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * **유틸리티 계층(util)**. 호출자는 일반적으로 호스트 유저스페이스 SPDK thread 또는 환경 초기화
 * 단계의 메인 스레드. blocking 호출이라 reactor 루프 안에서 호출하면 잠시 멈춘다.
 *
 * 호출 체인(전형적):
 *   - 설정/인증 파일 로드: 모듈 init → spdk_posix_file_load_from_name(path, &size)
 *     → fopen → fread → 통째 메모리에 적재 → 호출자가 free
 *   - PCI 디바이스 properties 조회: env_dpdk → spdk_read_sysfs_attribute_uint32(&val,
 *     "/sys/bus/pci/devices/%s/numa_node", bdf) → 결과 NUMA 노드로 hugepage/큐 배치 결정
 *
 * === 타 모듈과의 연결 ===
 * - `spdk/stdinc.h`: FILE*, size_t, uint32_t 등
 * - 호출 예: env_dpdk(PCI 토폴로지), nvmf TLS(인증서 로드), JSON RPC(설정 파일 로드)
 * - 반환된 버퍼/문자열의 소유권은 호출자에 이전 — 반드시 free 필요 (각 함수 주석 참조).
 *
 * === 주요 함수/구조체 요약 ===
 * - `spdk_posix_file_load(FILE *f, size_t *size)`: 이미 fopen된 파일 핸들에서 EOF까지 읽어 malloc 버퍼 반환
 * - `spdk_posix_file_load_from_name(const char *name, size_t *size)`: 파일명으로 자동 fopen → 위 함수 호출 → fclose
 * - `spdk_read_sysfs_attribute(char **out, const char *fmt, ...)`: sysfs 텍스트 속성을 문자열로 읽기 (개행 trim)
 * - `spdk_read_sysfs_attribute_uint32(uint32_t *out, const char *fmt, ...)`: sysfs 속성을 uint32로 파싱
 *
 * 경계/주의:
 *   - 모두 blocking — reactor/poller hot-path에서 호출 금지
 *   - 반환된 메모리는 호출자가 free 책임
 *   - sysfs 함수들은 가변 인자에 `printf` 포맷 검증(`__attribute__((format))`) 적용 — 컴파일 시 오류 검출
 */

#ifndef SPDK_FILE_H
#define SPDK_FILE_H

/* [한국어] FILE*, size_t, uint32_t 등 표준 타입 의존을 위해 포함. */
#include "spdk/stdinc.h"

#ifdef __cplusplus
/* [한국어] C++ 환경에서 C 링키지 보장. */
extern "C" {
#endif

/**
 * Load the input file content into a data buffer.
 *
 * \param file File handle.
 * \param size Size of bytes read from the file.
 *
 * \return data contains the content on success, NULL on failure.
 */
/*
 * [한국어]
 * spdk_posix_file_load - 이미 열린 FILE 핸들의 내용을 EOF까지 읽어 malloc 버퍼로 반환.
 *
 * @file: 호출자가 fopen으로 열어둔 FILE 포인터. 함수는 fclose하지 않는다 (호출자가 닫아야 함).
 * @size: [out] 읽은 바이트 수가 저장됨. NULL이 아니면 반드시 valid 포인터여야 함.
 * @return: 성공 시 malloc 버퍼 (size 바이트). 실패 시 NULL (errno 세팅).
 *          호출자는 반환 버퍼를 반드시 `free()` 해야 함.
 *
 * 왜 필요한가: 텍스트성 설정 파일이나 인증서 등을 한 번에 읽어들이는 단순화 헬퍼. fseek/ftell/fread를
 *              호출자가 직접 쓰는 보일러플레이트 제거.
 * 동작:
 *   1. fseek(SEEK_END) → ftell로 파일 크기 파악
 *   2. 크기만큼 malloc
 *   3. fseek(SEEK_SET, 0) 후 fread로 통째 읽기
 *   4. *size에 실제 읽은 바이트 수 기록
 * 에러 경로: malloc 실패 / fread 실패 → 버퍼 free 후 NULL 반환.
 * 실행 컨텍스트: blocking I/O — 초기화 경로에서만 사용.
 * 호출 체인:
 *   설정 로드 → [spdk_posix_file_load] → malloc + fread
 */
void *spdk_posix_file_load(FILE *file, size_t *size);

/**
 * Load content of a given file name into a data buffer.
 *
 * \param file_name File name.
 * \param size Size of bytes read from the file.
 *
 * \return data containing the content on success, NULL on failure.
 */
/*
 * [한국어]
 * spdk_posix_file_load_from_name - 파일명을 받아 직접 fopen → load → fclose 처리.
 *
 * @file_name: 읽을 파일의 경로 (절대/상대).
 * @size: [out] 읽은 바이트 수.
 * @return: 성공 시 malloc 버퍼, 실패 시 NULL. 호출자가 반드시 free 책임.
 *
 * 왜 필요한가: load의 편의 wrapper. 호출자가 FILE 핸들 라이프사이클을 직접 관리할 필요가 없음.
 * 동작:
 *   1. fopen(file_name, "r")
 *   2. spdk_posix_file_load(f, size) 호출
 *   3. fclose
 *   4. 결과 반환
 * 에러 경로: fopen 실패(권한/존재 안함) → NULL. load 실패 → NULL. fclose는 베스트-에포트.
 * 실행 컨텍스트: blocking. 호출 시 파일시스템 I/O 비용 발생.
 * 호출 체인:
 *   JSON RPC config 로드 / 인증서 로드 → [spdk_posix_file_load_from_name]
 */
void *spdk_posix_file_load_from_name(const char *file_name, size_t *size);

/**
 * Get the string value for a given sysfs attribute path
 *
 * When successful, the returned string will be null-terminated, without
 * a trailing newline.
 *
 * \param attribute output parameter for contents of the attribute; caller must
 *		    free() the buffer pointed to by attribute at some
 *		    point after a successful call
 * \param path_format format string for constructing patch to sysfs file
 *
 * \return 0 on success
 *         negative errno if unable to read the attribute
 */
/*
 * [한국어]
 * spdk_read_sysfs_attribute - **sysfs 속성**을 **문자열**로 읽기 (가변 인자 path 포맷팅).
 *
 * @attribute:   [out] 성공 시 내부에서 malloc된 NULL-terminated 문자열의 주소가 채워진다.
 *               꼬리 개행은 자동으로 제거됨. 호출자가 반드시 `free(*attribute)` 호출.
 *               실패 시 *attribute 값은 정의되지 않음 — 사용해서는 안 됨.
 * @path_format: printf 포맷 문자열로 sysfs 경로를 구성. 예:
 *               "/sys/bus/pci/devices/%s/vendor"
 *               이어지는 가변 인자가 포맷 인자로 들어감.
 * @return:      0 = 성공. 음수 errno = 실패 (예: -ENOENT 파일 없음, -EACCES 권한, -EIO).
 *
 * 왜 필요한가: PCI/NUMA/CPU 같은 커널 노출 메타데이터를 코드에서 손쉽게 읽기 위한 헬퍼. 매번
 *              snprintf로 경로 만들고 fopen/fread/trim 하는 보일러플레이트 제거.
 * 동작:
 *   1. va_list로 path_format을 vsnprintf하여 실제 경로 생성
 *   2. fopen → 내용 읽기 → 꼬리 개행 trim → malloc 버퍼 반환
 *   3. fclose
 *   4. *attribute = 버퍼, return 0
 * 컴파일 검증: `__attribute__((format(printf, 2, 3)))`로 path_format이 printf 포맷이고 가변
 *              인자가 2번째부터 시작함을 명시 — 잘못된 포맷/인자 조합을 컴파일 타임에 경고.
 * 에러 경로: 경로 빌드 실패 / fopen 실패 / 읽기 실패 → 음수 errno 반환.
 * 실행 컨텍스트: blocking — 초기화 경로 전용.
 * 호출 체인:
 *   PCI/NUMA/CPU 메타데이터 조회 → [spdk_read_sysfs_attribute] → fopen + fread
 */
int spdk_read_sysfs_attribute(char **attribute, const char *path_format, ...)
__attribute__((format(printf, 2, 3)));

/**
 * Get the uint32 value for a given sysfs attribute path
 *
 * \param attribute output parameter for contents of the attribute
 * \param path_format format string for constructing patch to sysfs file
 *
 * \return 0 on success
 *         negative errno if unable to read the attribute or it is not a uint32
 */
/*
 * [한국어]
 * spdk_read_sysfs_attribute_uint32 - sysfs 속성을 **uint32_t** 정수로 파싱하여 반환.
 *
 * @attribute:   [out] 파싱된 uint32 값 저장. 실패 시 값이 변경되지 않을 수도 있음 — 호출자는
 *               반환값 검사 후에만 사용.
 * @path_format: printf 포맷 sysfs 경로. 가변 인자 동일.
 * @return:      0 = 성공. 음수 errno = 실패 (-ENOENT 파일 없음, -ERANGE 정수 오버플로,
 *               -EINVAL 정수가 아닌 내용).
 *
 * 왜 필요한가: numa_node, vendor_id, device_id, MTU 등 sysfs에서 숫자값을 읽는 패턴이 매우 흔함.
 *              문자열로 받아 strtoul 하는 보일러플레이트를 한 함수로 통합.
 * 동작:
 *   1. 내부적으로 spdk_read_sysfs_attribute로 문자열을 읽음
 *   2. strtoul/strtol 등으로 uint32 파싱 (10진/0x16진 자동 인식)
 *   3. *attribute에 결과 대입
 * 에러 경로: 문자열 읽기 실패는 그대로 전파. 정수 변환 실패 → -EINVAL/-ERANGE.
 * 실행 컨텍스트: blocking — 초기화 경로.
 * 호출 체인:
 *   env_dpdk PCI probe → [spdk_read_sysfs_attribute_uint32] → spdk_read_sysfs_attribute → strtoul
 */
int spdk_read_sysfs_attribute_uint32(uint32_t *attribute, const char *path_format, ...)
__attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
/* [한국어] C++ extern "C" 블록 닫기. */
}
#endif

#endif
/* [한국어] SPDK_FILE_H 헤더 가드 종료. */
