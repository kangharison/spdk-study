/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * UUID types and functions
 */

/*
 * [한국어 설명] UUID 타입·유틸리티 API (uuid.h)
 *
 * === 파일의 역할 ===
 * SPDK에서 사용하는 UUID(Universally Unique Identifier, RFC 4122) 타입
 * `struct spdk_uuid`와 그 조작 함수 API를 정의한다. SPDK는 다음 목적으로
 * UUID를 광범위하게 사용한다:
 *   - NVMe 네임스페이스 GUID/NGUID/EUI64 식별
 *   - Logical Volume(lvol), blobstore blob의 영속 식별자
 *   - bdev instance 고유 ID
 *   - NVMe-oF host NQN 생성(sha1 경로)
 *
 * 128비트(16바이트) 이진 표현과 문자열 표현(`xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`)
 * 사이의 상호 변환, 비교·복사·NULL 검사·랜덤 생성(RFC 4122 §4.4)·이름 기반
 * 생성(§4.3, SHA1 버전 5) 함수를 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 공개 API 헤더 (lib 구현은 lib/util/uuid.c). 런타임 비용 경미(compare/copy는
 * O(16), 생성은 RNG 호출 한 번).
 * 실행 컨텍스트: 모든 SPDK thread 컨텍스트에서 호출 가능. 스레드 세이프
 * (내부 전역 상태 없음; 생성기는 OS 엔트로피 소스 사용).
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/stdinc.h   - size_t, uint8_t, bool
 *   - spdk/assert.h   - SPDK_STATIC_ASSERT (구조체 크기 검증)
 * 구현 측 의존(구현 파일 기준, 이 헤더가 노출하는 관계는 아님):
 *   - libuuid 또는 openssl/crypto(sha1) — lib/util/uuid.c에서 처리
 * 의존하는 모듈:
 *   - lib/blob, lib/lvol, lib/bdev, lib/nvme, lib/nvmf 등 식별자 필요 지점
 * 공유 자료구조: struct spdk_uuid는 단순 16바이트 값 객체 — lock 없이 자유 복사.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_uuid: 16바이트 raw 배열을 union으로 감싼 값 객체
 *   - SPDK_UUID_STRING_LEN: 텍스트 표현 길이(37 = 36 + NUL)
 *   - spdk_uuid_parse:        문자열 → spdk_uuid
 *   - spdk_uuid_fmt_lower:    spdk_uuid → 소문자 문자열
 *   - spdk_uuid_compare:      memcmp 비교
 *   - spdk_uuid_generate:     RFC 4122 §4.4 임의 UUID 생성
 *   - spdk_uuid_generate_sha1: §4.3 이름 기반 SHA1 UUID 생성
 *   - spdk_uuid_copy:         UUID 복사
 *   - spdk_uuid_is_null:      전체 0 여부
 *   - spdk_uuid_set_null:     전체 0으로 설정
 */

#ifndef SPDK_UUID_H              /* [한국어] include 가드 시작 */
#define SPDK_UUID_H              /* [한국어] 가드 심볼 */

#include "spdk/stdinc.h"         /* [한국어] size_t/uint8_t/bool 등 표준 타입 확보 */

#include "spdk/assert.h"         /* [한국어] SPDK_STATIC_ASSERT 매크로 확보 — struct spdk_uuid 크기 검증용 */

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_uuid {
	union {
		uint8_t raw[16];     /* [한국어] UUID의 16바이트 raw 표현 (빅엔디언 network byte order 관례)
                                  *  - RFC 4122 §4.1.2 바이트 순서 준수
                                  *  - 설정자: spdk_uuid_parse/generate/copy/set_null
                                  *  - 읽는 자: spdk_uuid_fmt_lower/compare/is_null
                                  *  - 값 범위: 임의 128비트 바이트 패턴. 전부 0이면 NULL UUID. */
	} u;                         /* [한국어] 미래 확장을 위한 union — 현재는 raw만 존재하나 timestamp·clock_seq 등 필드별 뷰 추가 대비 */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_uuid) == 16, "Incorrect size");
                                 /* [한국어] 컴파일 타임 어설션 — 구조체가 정확히 16바이트임을 강제
                                  *  - 네임스페이스·bdev 등 외부 표현 포맷과 맞지 않으면 저장된 UUID가 깨짐 */

#define SPDK_UUID_STRING_LEN 37 /* 36 characters + null terminator */
                                 /* [한국어] 텍스트 포맷 버퍼 필요 크기
                                  *  - "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" = 8+1+4+1+4+1+4+1+12 = 36자 + '\0' */

/**
 * Convert UUID in textual format into a spdk_uuid.
 *
 * \param[out] uuid User-provided UUID buffer.
 * \param uuid_str UUID in textual format in C string.
 *
 * \return 0 on success, or negative errno on failure.
 */
int spdk_uuid_parse(struct spdk_uuid *uuid, const char *uuid_str);
/*
 * [한국어]
 * spdk_uuid_parse - 문자열 표현 UUID를 spdk_uuid 구조체로 파싱
 *
 * @uuid: 출력 버퍼 (호출자 할당)
 * @uuid_str: NUL 종단 문자열. 예: "12345678-1234-5678-1234-567812345678"
 *             대/소문자 모두 허용, 하이픈 위치 고정.
 * @return: 0 성공, 음수 errno 실패 (포맷 오류 시 -EINVAL 등)
 *
 * 실행 컨텍스트: 임의 스레드. I/O 블로킹 없음 — 순수 문자열 파싱.
 * 호출 체인: RPC 핸들러/CLI 파서 → [이 함수] → 구조체 채움
 */

/**
 * Convert UUID in spdk_uuid into lowercase textual format.
 *
 * \param uuid_str User-provided string buffer to write the textual format into.
 * \param uuid_str_size Size of uuid_str buffer. Must be at least SPDK_UUID_STRING_LEN.
 * \param uuid UUID to convert to textual format.
 *
 * \return 0 on success, or negative errno on failure.
 */
int spdk_uuid_fmt_lower(char *uuid_str, size_t uuid_str_size, const struct spdk_uuid *uuid);
/*
 * [한국어]
 * spdk_uuid_fmt_lower - spdk_uuid를 소문자 텍스트 포맷으로 직렬화
 *
 * @uuid_str: 출력 버퍼
 * @uuid_str_size: 버퍼 크기. 최소 SPDK_UUID_STRING_LEN(37) 이상이어야 함
 * @uuid: 직렬화할 대상
 * @return: 0 성공, -EINVAL(버퍼 작음 등) 실패
 *
 * 사용처: RPC 응답 JSON, 로그 출력, bdev info 쿼리 등
 */

/**
 * Compare two UUIDs.
 *
 * \param u1 UUID 1.
 * \param u2 UUID 2.
 *
 * \return 0 if u1 == u2, less than 0 if u1 < u2, greater than 0 if u1 > u2.
 */
int spdk_uuid_compare(const struct spdk_uuid *u1, const struct spdk_uuid *u2);
/*
 * [한국어]
 * spdk_uuid_compare - 두 UUID를 memcmp로 비교
 *
 * 반환은 C 관례(음/0/양) — 정렬 컨테이너(tree, bsearch)에 직접 사용 가능.
 * 주의: 이 순서는 UUID의 "의미적 순서"가 아닌 바이트 사전식 순서.
 */

/**
 * Generate a new UUID.
 *
 * \param[out] uuid User-provided UUID buffer to fill.
 */
void spdk_uuid_generate(struct spdk_uuid *uuid);
/*
 * [한국어]
 * spdk_uuid_generate - 랜덤 UUID 생성 (RFC 4122 §4.4, version 4)
 *
 * @uuid: 출력 버퍼
 *
 * 내부적으로 OS 엔트로피(getrandom/urandom) 사용. 실패하지 않음 — 실패 시
 * abort 하거나 pseudo fallback을 쓰는 것은 구현 측(lib/util/uuid.c) 정책.
 */

/**
 * Generate a new UUID using SHA1 hash.
 *
 * \param[out] uuid User-provided UUID buffer to fill.
 * \param ns_uuid Well-known namespace UUID for generated UUID.
 * \param name Arbitrary, binary string.
 * \param len Length of binary string.
 *
 * \return 0 on success, or negative errno on failure.
 */
int spdk_uuid_generate_sha1(struct spdk_uuid *uuid, struct spdk_uuid *ns_uuid, const char *name,
			    size_t len);
/*
 * [한국어]
 * spdk_uuid_generate_sha1 - 이름 기반 UUID 생성 (RFC 4122 §4.3, version 5)
 *
 * @uuid: 출력 버퍼
 * @ns_uuid: 네임스페이스 UUID (예: DNS/URL 표준 네임스페이스, 또는 SPDK 고유)
 * @name: 임의의 바이너리 바이트열
 * @len: name 길이
 * @return: 0 성공, 음수 errno 실패
 *
 * 동일한 (ns_uuid, name) 입력은 항상 같은 UUID를 생성 → 영속 식별자 용도로 적합.
 * 예: bdev 이름을 기반으로 결정적인(결정론적) UUID를 할당 → 재시작해도 같은 UUID
 */

/**
 * Copy a UUID.
 *
 * \param src Source UUID to copy from.
 * \param dst Destination UUID to store.
 */
void spdk_uuid_copy(struct spdk_uuid *dst, const struct spdk_uuid *src);
/*
 * [한국어]
 * spdk_uuid_copy - src를 dst로 복사
 *
 * 주의: memcpy(dst, src, 16)과 동일한 효과지만, API로 제공해 호출측이
 * 구조체 정의를 몰라도 되도록 캡슐화.
 */

/**
 * Compare the UUID to the NULL value (all bits equal to zero).
 *
 * \param uuid The UUID to test.
 *
 * \return true if uuid is equal to the NULL value, false if not.
 */
bool spdk_uuid_is_null(const struct spdk_uuid *uuid);
/*
 * [한국어]
 * spdk_uuid_is_null - UUID가 전부 0인지 검사
 *
 * SPDK 관례: zero-init된 구조체는 "UUID 미지정"을 의미. 이 함수로 초기화 여부 판단.
 */

/**
 * Set the value of UUID to the NULL value.
 *
 * \param uuid The UUID to set.
 */
void spdk_uuid_set_null(struct spdk_uuid *uuid);
/*
 * [한국어]
 * spdk_uuid_set_null - UUID 전체를 0으로 설정 (NULL 상태로 리셋)
 *
 * 사용처: 엔트리 해제 전 값 클리어, 재사용 풀에서 이전 식별자 흔적 제거 등
 */

#ifdef __cplusplus
}
#endif

#endif                           /* [한국어] include 가드 종료 */
