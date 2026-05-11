/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

/*
 * [한국어 설명] 파일 기반 키링(keyring) 백엔드 공개 API 헤더 (file.h)
 *
 * === 파일의 역할 ===
 * SPDK 키링 시스템의 "파일 기반" 백엔드 모듈이 외부에 공개하는 API를 정의한다.
 * 키링(keyring)은 NVMe-oF in-band authentication, 디스크 암호화 키(DEK),
 * TLS PSK(pre-shared key) 등에 사용되는 비밀(secret)을 SPDK 런타임 내부에
 * 등록·조회·해제하는 일반화된 메커니즘이며, 그 백엔드는 메모리/파일/외부 KMS
 * 등 여러 구현이 존재할 수 있다. 본 헤더는 "디스크 파일 한 개 = 키 한 개"
 * 형태로 키 데이터를 보관하는 가장 단순한 백엔드의 등록/해제 진입점만 노출한다.
 * 키 본체(바이트열)는 디스크 파일 내용으로 저장되고, 키링 등록 시점에는 키
 * "이름"과 파일 "경로"만 전달된다 — 실제 read는 키가 사용되는 시점에
 * 모듈 내부에서 수행된다(지연 로딩).
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK keyring 계층은 [include/spdk/keyring.h]가 코어 인터페이스를 제공하고,
 * 각 백엔드(file, KMIP 등)는 module/keyring/<backend>/ 디렉토리에 구현된다.
 * 본 헤더는 그중 file 백엔드의 공개 진입점이며, 일반적으로 RPC 핸들러
 * (keyring_file_add_key / keyring_file_remove_key)나 애플리케이션 코드가
 * 호출한다. 호출 체인: RPC 요청(JSON) → rpc_keyring_file_add_key →
 *   spdk_keyring_file_add_key (이 헤더) → keyring 모듈 내부에서
 *   spdk_keyring_add_key()로 코어 키링에 등록 → 이후 NVMe-oF/blob 암호화 등이
 *   spdk_keyring_get_key("이름")으로 가져가 사용한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/keyring (코어 키링 등록 자료구조), VFS(스탠다드 fopen/fread)
 * - 의존받음: app/spdk_tgt 등의 RPC 코드, 사용자 스크립트(rpc.py)
 * - 데이터 흐름: 사용자 → RPC → 본 헤더 함수 → keyring 코어 → 사용 모듈
 *   (NVMe-oF/lvol/bdev 암호화)이 키 이름으로 룩업해 사용. 키 데이터(바이트)는
 *   디스크 파일에서 로드되므로, 파일 권한(0600 등)이 보안 경계가 된다.
 * - 공유 자료구조: spdk_keyring 내부의 키 테이블(이름 → 백엔드+ctx 매핑).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_keyring_file_add_key   : 이름과 파일 경로로 새 키를 키링에 등록.
 * - spdk_keyring_file_remove_key: 이름으로 등록된 키를 키링에서 제거.
 * 본 헤더에는 자료구조가 없으며, 외부에 노출되는 것은 위 두 함수뿐이다.
 * 백엔드 내부 ctx, 키 길이, 캐싱 정책 등은 module/keyring/file/keyring_file.c에
 * 캡슐화되어 있다.
 */

#ifndef SPDK_KEYRING_FILE_H
#define SPDK_KEYRING_FILE_H
/* [한국어] 다중 포함 가드 — 본 헤더가 여러 .c에 포함되더라도 한 번만 처리되게 함. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 컴파일러에서 본 헤더를 포함할 때 함수 심볼이 C 링키지로
 * 노출되도록 보장 (이름 맹글링 방지). SPDK는 C 라이브러리지만 C++ 사용자도
 * 링크할 수 있으므로 모든 공개 헤더가 이 가드를 둔다. */
#endif

/**
 * Add a file-based key to the keyring.
 *
 * \param name Name of a key.
 * \param path Path to a file containing the key.
 *
 * \return 0 on success, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_keyring_file_add_key - 키 이름과 디스크 파일 경로를 받아 file 백엔드 키를 키링에 등록.
 *
 * @name : 키링 안에서 키를 식별할 고유 이름 (예: "nvme0_dek"). NULL/중복 금지.
 *         설정자: RPC 또는 애플리케이션. 읽는 자: keyring 코어와 키 사용 모듈.
 * @path : 키 본체가 들어 있는 파일의 절대/상대 경로. 파일 내용 자체가 키이며,
 *         권한·길이·포맷은 file 백엔드의 정책(예: 텍스트 hex 1줄)에 따른다.
 * @return: 0 = 성공(키링에 정상 등록됨). 음수 = -errno (-EEXIST 중복, -ENOENT 파일 없음,
 *          -ENOMEM 할당 실패 등). 호출자는 음수일 경우 RPC 에러 응답으로 변환한다.
 *
 * 키링은 SPDK 안에서 보안 자격증명을 안전하게 보관하기 위한 일반 인터페이스이고,
 * 본 함수는 그중 "파일에 적힌 키를 이름으로 등록"하는 가장 흔한 사용 시나리오를 노출한다.
 * 내부적으로는 file 백엔드용 ctx(경로 사본)를 만들고 spdk_keyring_add_key()를 호출한다.
 * 실행 컨텍스트: 일반적으로 RPC 처리 스레드(애플리케이션 마스터 스레드) — keyring
 * 등록은 한 스레드에서 수행되며, 사용 시에는 다른 reactor 스레드에서 lookup만 한다.
 *
 * 호출 체인:
 *   RPC handler / app code → [spdk_keyring_file_add_key] → spdk_keyring_add_key → 코어 키링 테이블
 */
int spdk_keyring_file_add_key(const char *name, const char *path);

/**
 * Remove a file-based key to the keyring.
 *
 * \param name Name of a key.
 *
 * \return 0 on success, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_keyring_file_remove_key - 등록된 file 백엔드 키를 이름으로 제거.
 *
 * @name : add 시 사용한 키 이름. 미존재 시 -ENOENT 반환.
 * @return: 0 = 성공. 음수 errno = 실패. 키 사용자가 아직 ref를 들고 있으면
 *          코어 keyring 정책에 따라 즉시 해제 또는 last-ref 후 해제된다.
 *
 * 키 회전(rotate)/만료/오타 정정 시 호출되며, 제거 후에는 동일 이름으로 다른 키를
 * 새로 등록할 수 있다. 키를 사용 중인 모듈(예: NVMe-oF 인증)은 lookup 시점에
 * -ENOENT를 받게 되므로 호출자는 사용 중 제거 가능성을 인지해야 한다.
 *
 * 호출 체인:
 *   RPC handler / app code → [spdk_keyring_file_remove_key] → spdk_keyring_remove_key
 */
int spdk_keyring_file_remove_key(const char *name);

#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 종결. */
#endif

#endif /* SPDK_KEYRING_FILE_H */
/* [한국어] 다중 포함 가드 종결. */
