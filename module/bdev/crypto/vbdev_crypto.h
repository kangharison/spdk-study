/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Crypto 가상 bdev 공개 API 헤더 (vbdev_crypto.h)
 *
 * === 파일의 역할 ===
 * AES-XTS/AES-CBC 등 대칭키 암호화를 base bdev 위에 투명하게 적용하는 vbdev 모듈의 공개 API.
 * 본 모듈은 spdk_accel 프레임워크의 crypto operation을 이용해 write 시 평문 → 암호문 변환,
 * read 시 암호문 → 평문 변환을 수행한다. 따라서 가속기(QAT, AESNI-MB) 또는 소프트웨어 백엔드
 * 어느 쪽으로도 동작 가능하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [JSON-RPC] → [vbdev_crypto_rpc.c] → [이 헤더] → [vbdev_crypto.c]
 *                                                    ↓ spdk_accel_submit_encrypt/decrypt
 *                                                [accel framework] → [QAT/AESNI/SW]
 *                                                    ↓ spdk_bdev_writev_blocks
 *                                                [base bdev (NVMe/Malloc/...)]
 *
 * === 타 모듈과의 연결 ===
 * - vbdev_crypto.c : 구현부.
 * - include/spdk/accel.h, include/spdk/accel_module.h : crypto key 생성/관리 API.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct vbdev_crypto_opts : 생성 옵션(이름, base bdev, key).
 * - create_crypto_disk() / delete_crypto_disk() : vbdev 라이프사이클.
 * - create_crypto_opts_by_name() / free_crypto_opts() : 옵션 객체 생성/해제.
 */

#ifndef SPDK_VBDEV_CRYPTO_H
#define SPDK_VBDEV_CRYPTO_H

#include "spdk/rpc.h"
/* [한국어] JSON-RPC 디코더 — 옵션 파싱에서 사용. */
#include "spdk/util.h"
/* [한국어] SPDK_CONTAINEROF 등 유틸. */
#include "spdk/string.h"
/* [한국어] spdk_strerror 등 문자열 유틸. */
#include "spdk/log.h"
/* [한국어] 로그 매크로. */
#include "spdk/accel.h"
/* [한국어] spdk_accel_submit_encrypt/decrypt 등 가속 프레임워크 공개 API. */
#include "spdk/accel_module.h"
/* [한국어] spdk_accel_crypto_key 타입 — vbdev_crypto_opts.key가 참조. */

#include "spdk/bdev.h"
/* [한국어] bdev core 공개 API. */

/* [한국어] 기본 cipher 모드(AES-CBC). QAT 및 AESNI_MB 백엔드가 모두 지원. */
#define BDEV_CRYPTO_DEFAULT_CIPHER "AES_CBC" /* QAT and AESNI_MB */

/* Structure to hold crypto options */
/*
 * [한국어] struct vbdev_crypto_opts
 * Crypto vbdev 생성 시 RPC가 전달하는 파라미터 묶음.
 */
struct vbdev_crypto_opts {
	char				*vbdev_name;	/* name of the vbdev to create */
	/* [한국어] 생성할 crypto vbdev의 이름. 모듈이 strdup해 보관 후 free_crypto_opts에서 해제. */
	char				*bdev_name;	/* base bdev name */
	/* [한국어] 암호화 대상 base bdev 이름. open_ext로 열어 자식 bdev로 매핑. */
	struct spdk_accel_crypto_key	*key;		/* crypto key */
	/* [한국어] accel 프레임워크에 등록된 crypto key 핸들. 키 자체는 accel 모듈이 보관하고
	 * 본 구조체는 핸들 포인터만 가짐. NULL 불가. */
	bool				key_owner;	/* If wet to true then the key was created by RPC and needs to be destroyed */
	/* [한국어] true면 본 vbdev 생성 시점에 RPC가 임시로 만든 키 — vbdev 삭제 시 함께 destroy 필요.
	 * false면 기존에 등록된 키를 참조만 함 — destroy하지 않음. */
};

/*
 * [한국어] typedef spdk_delete_crypto_complete
 * Crypto vbdev 삭제 완료 콜백.
 */
typedef void (*spdk_delete_crypto_complete)(void *cb_arg, int bdeverrno);

/**
 * Create new crypto bdev.
 *
 * \param opts Crypto options populated by create_crypto_opts()
 * \return 0 on success, other on failure.
 */
/*
 * [한국어] create_crypto_disk - Crypto vbdev 생성
 * @opts: 옵션 구조체(create_crypto_opts_by_name 또는 직접 채움).
 * @return: 0 성공, 음수 -errno.
 */
int create_crypto_disk(struct vbdev_crypto_opts *opts);

/**
 * Delete crypto bdev.
 *
 * \param bdev_name Crypto bdev name.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
/*
 * [한국어] delete_crypto_disk - 이름 기반 비동기 unregister.
 */
void delete_crypto_disk(const char *bdev_name, spdk_delete_crypto_complete cb_fn,
			void *cb_arg);

/**
 * Create crypto opts for the given crypto vbdev name and base bdev name.
 *
 * \param name Name of crypto vbdev.
 * \param base_bdev_name Name of base bdev for crypto vbdev.
 * \param key crypto key for the vbdev.
 * \param key_owner Is key created by application/RPC.
 * \return Handle to created vbdev_crypto_opts or NULL if failed to create.
 */
/*
 * [한국어] create_crypto_opts_by_name - 옵션 객체 alloc + strdup.
 * 사용 후 free_crypto_opts로 반드시 해제할 것.
 */
struct vbdev_crypto_opts *
create_crypto_opts_by_name(char *name, char *base_bdev_name, struct spdk_accel_crypto_key *key,
			   bool key_owner);

/**
 * Release crypto opts created with create_crypto_opts()
 *
 * \param opts Crypto opts to release
 */
/*
 * [한국어] free_crypto_opts - vbdev_crypto_opts 메모리 해제.
 * key_owner=true면 spdk_accel_crypto_key_destroy도 호출됨.
 */
void free_crypto_opts(struct vbdev_crypto_opts *opts);

#endif /* SPDK_VBDEV_CRYPTO_H */
