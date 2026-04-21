/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * [한국어 설명] MD5 해시 래퍼 (md5.h)
 *
 * === 파일의 역할 ===
 * OpenSSL EVP 기반 MD5 메시지 다이제스트 계산을 SPDK 컨벤션에 맞춰 감싼
 * 스트리밍 API를 제공한다 (init / update / final).
 * SPDK에서 MD5 사용처는 대부분 **프로토콜 호환성 목적**이며 보안 용도가
 * 아니다:
 *   - iSCSI CHAP 인증(RFC 3720) — MD5가 프로토콜로 강제됨
 *   - NVMe-oF in-band auth의 일부 레거시 모드
 * MD5 자체는 충돌 내성이 깨졌으므로 새로운 보안 기능에 사용 금지.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 구현: lib/iscsi 등에서 직접 사용 (공용 구현 파일 없음 — 헤더 전용 래퍼 근접).
 * EVP 컨텍스트를 감싸 추상화. 런타임은 OpenSSL 구현에 위임.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/stdinc.h    - 표준 타입
 *   - openssl/md5.h    - MD5_DIGEST_LENGTH (=16) 상수
 *   - openssl/evp.h    - EVP_MD_CTX API
 * 의존하는 모듈: lib/iscsi (CHAP), lib/nvmf (legacy auth)
 * 공유 자료구조: struct spdk_md5ctx (EVP_MD_CTX* 래핑).
 *
 * === 주요 함수/구조체 요약 ===
 *   - SPDK_MD5DIGEST_LEN : MD5_DIGEST_LENGTH (=16)
 *   - struct spdk_md5ctx : EVP_MD_CTX 포인터 보관
 *   - spdk_md5init(ctx)  : MD5 컨텍스트 초기화
 *   - spdk_md5update(ctx, data, len): 데이터 청크 추가
 *   - spdk_md5final(out, ctx):       최종 다이제스트 추출 + 컨텍스트 해제
 */

#ifndef SPDK_MD5_H               /* [한국어] include 가드 */
#define SPDK_MD5_H

#include "spdk/stdinc.h"         /* [한국어] size_t 등 */

#include <openssl/md5.h>         /* [한국어] MD5_DIGEST_LENGTH 등 상수 제공 — EVP 경로만 쓰더라도 상수 포함 목적 */
#include <openssl/evp.h>         /* [한국어] EVP_MD_CTX — OpenSSL 3.x 권장 API (단독 MD5_Init 함수는 3.0에서 deprecated) */

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_MD5DIGEST_LEN MD5_DIGEST_LENGTH
                                 /* [한국어] MD5 다이제스트 길이 16바이트
                                  *  - OpenSSL 상수를 SPDK 이름으로 재노출 — 호출자가 openssl/md5.h를 직접 의존하지 않도록 */

struct spdk_md5ctx {
	EVP_MD_CTX *md5ctx;          /* [한국어] OpenSSL EVP 컨텍스트
                                  *  - 설정자: spdk_md5init (EVP_MD_CTX_new + EVP_DigestInit_ex)
                                  *  - 읽는 자: spdk_md5update(EVP_DigestUpdate), spdk_md5final(EVP_DigestFinal_ex)
                                  *  - 값 범위: 유효 OpenSSL 핸들 (NULL은 초기화 실패 상태)
                                  *  - 해제: spdk_md5final 호출 시 EVP_MD_CTX_free 수행 */
};

/**
 * Init md5 context
 *
 * \param md5ctx context
 * \return 0 on success, -1 on failure
 */
int spdk_md5init(struct spdk_md5ctx *md5ctx);
/*
 * [한국어]
 * spdk_md5init - MD5 컨텍스트 초기화
 *
 * @md5ctx: 호출자 할당 구조체
 * @return: 0 성공, -1 실패 (OpenSSL 할당 실패 등)
 *
 * 내부: EVP_MD_CTX_new + EVP_DigestInit_ex(EVP_md5()) 호출.
 * 성공 시 후속 spdk_md5update/final 호출 가능 상태.
 */

/**
 * Update \b md5ctx digest with hash of \b len bytes of \b data.
 *
 * This function can be called several times on the same md5ctx to hash additional data
 *
 * \param md5ctx context
 * \param data data pointer
 * \param len length of data buffer in bytes
 * \return 0 on success, -1 on failure
 */
int spdk_md5update(struct spdk_md5ctx *md5ctx, const void *data, size_t len);
/*
 * [한국어]
 * spdk_md5update - 해시 스트리밍 업데이트
 *
 * 여러 번 호출해 대용량 데이터를 청크로 해시 가능.
 * @return: 0 성공, -1 실패.
 */

/**
 * Retrieves the digest from \b ctx and places it in \b md5 buffer.
 *
 * \b md5 buffer must be \b SPDK_MD5DIGEST_LEN bytes length. \b md5ctx is released, it can be used again after
 * initialization via \ref spdk_md5init
 *
 * \param md5
 * \param md5ctx
 * \return 0 on success, -1 on failure
 */
int spdk_md5final(void *md5, struct spdk_md5ctx *md5ctx);
/*
 * [한국어]
 * spdk_md5final - 최종 16바이트 다이제스트 추출 + 컨텍스트 해제
 *
 * @md5: 최소 16바이트 버퍼 (SPDK_MD5DIGEST_LEN)
 * @md5ctx: init된 컨텍스트. 성공 시 내부 EVP 핸들이 해제됨 → 재사용하려면 spdk_md5init 재호출
 * @return: 0 성공, -1 실패.
 */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_MD5_H */           /* [한국어] include 가드 종료 */
