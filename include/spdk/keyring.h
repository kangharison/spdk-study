/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

/*
 * [한국어 설명] SPDK Keyring 공개 API 헤더 (keyring.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 의 통합 키 보관소(keyring) 라이브러리(lib/keyring) 가 외부에
 * 노출하는 공개 API 를 선언한다. SPDK 는 NVMe-oF TCP 의 TLS PSK (Pre-Shared
 * Key), bdev_crypto / blob 의 DEK (Data Encryption Key, AES-XTS), DH-CHAP
 * 인증 키 등 다양한 보안 키를 다루는데, 각 모듈마다 별도 보관 메커니즘을
 * 갖는 대신 단일 keyring 라이브러리에 위임해 (1) 이름 기반 조회, (2)
 * reference-count 수명 관리, (3) 메모리 zeroize, (4) 백엔드 모듈화(파일
 * 기반 / Linux kernel keyring 등) 를 통일한다. 본 헤더는 그 사용자(소비자)
 * 측 API 만 정의하며, 백엔드 모듈 작성자용 API 는 keyring_module.h 에서
 * 별도 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * keyring 은 SPDK 의 데이터 plane(I/O 스택) 보다는 control plane 의 보안
 * 영역에 속한다. 호출 흐름은: (시작 시) RPC keyring_file_add_key 등으로
 * 백엔드가 키를 등록 → 사용 시점에 NVMe-oF TCP listener / bdev_crypto
 * 생성 RPC 가 spdk_keyring_get_key("이름") 으로 ref 를 잡고, I/O 가 진행
 * 되는 동안 keyring 안에서 그 ref 가 살아 있음을 보장 받는다. I/O 발행
 * 시점에는 spdk_key_get_key 로 실제 key material 을 임시 버퍼에 복사해
 * 사용한 뒤 zeroize 한다. 종료 시 spdk_keyring_put_key 로 ref 를 해제하고,
 * spdk_keyring_cleanup 으로 라이브러리 전체를 정리한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h (size_t/int), spdk/json.h (write_config 가 JSON-RPC
 * 출력을 그대로 dump 하기 위해 spdk_json_write_ctx 사용). 사용처:
 * lib/nvmf (NVMe-oF TCP transport 가 TLS PSK 를 keyring 에서 룩업),
 * module/bdev/crypto, module/accel (DEK 보관), DH-CHAP 인증 (host-controller
 * 상호 인증 키). 백엔드: keyring_module.h 를 통해 등록된 모듈들 (file 기반
 * keyring, Linux kernel keyring 등) 이 실제 키 저장소를 제공. 데이터
 * 흐름은 (백엔드 저장소) → keyring 내부 hash table → 사용자 ref → 임시
 * key buffer (사용 후 zeroize).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_keyring_init / spdk_keyring_cleanup: 라이브러리 lifecycle.
 * - spdk_keyring_get_key(name): 이름으로 키 ref 획득 (ref-count 증가).
 *   "<keyring>:<key>" 형태의 namespacing 지원, 미지정 시 "global" 사용.
 * - spdk_keyring_put_key(key): ref-count 감소 (마지막 ref 해제 시 zeroize).
 * - spdk_key_get_name(key): 키 이름 조회 (디버깅/로깅).
 * - spdk_key_get_key(key, buf, len): 실제 key material 을 사용자 버퍼로 복사.
 *   호출자는 사용 직후 explicit_bzero 등으로 버퍼를 지워야 한다.
 * - spdk_key_dup(key): ref-count 만 증가 (실 객체 복제 아님).
 * - spdk_keyring_for_each_key(...): 모든 키 순회 (RPC list 용).
 * - spdk_keyring_write_config(w): 현재 keyring 상태를 JSON 으로 직렬화.
 * 핵심 (불투명) 타입: struct spdk_key (개별 키 핸들), struct spdk_keyring
 * (서브 keyring; 현재 API 는 NULL=전체 범위만 지원).
 */

/* [한국어] SPDK_KEYRING_H — 헤더 가드. NVMe-oF, bdev_crypto, accel 등 여러
 * 모듈에서 포함되므로 가드는 필수. */
#ifndef SPDK_KEYRING_H
#define SPDK_KEYRING_H

/* [한국어] spdk/stdinc.h - 표준 정수형/size_t/uint32_t/bool 등 SPDK 공통
 * 표준 헤더 묶음. 이 헤더에서는 size_t 와 int 정도만 직접 사용하지만,
 * 일관성 차원에서 포함. */
#include "spdk/stdinc.h"
/* [한국어] spdk/json.h - SPDK 의 streaming JSON writer/reader API 를
 * 가져오기 위한 헤더. spdk_keyring_write_config 가 spdk_json_write_ctx*
 * 를 인자로 받아 keyring 상태를 RPC 출력 또는 config dump 형태로
 * 직렬화하므로 필수. */
#include "spdk/json.h"

/* [한국어] extern "C" 가드 — C++ 프로그램에서 SPDK keyring API 를 호출할
 * 때 심볼 망글링을 방지하는 표준 SPDK 패턴. */
#ifdef __cplusplus
extern "C" {
#endif

/* [한국어] struct spdk_key - 개별 키의 핸들을 가리키는 불투명 구조체.
 * 정의는 lib/keyring 내부에서만 노출되며, 사용자에게는 포인터로만 노출
 * 된다. 이렇게 한 이유는 (1) 백엔드별로 필드 구성이 다르고, (2) 사용자
 * 코드가 직접 필드를 만지면 ref-count, zeroize 보장이 깨질 수 있기 때문.
 * 사용자는 반드시 spdk_keyring_get_key 등으로 받은 포인터만 사용한다. */
struct spdk_key;

/*
 * [한국어]
 * spdk_keyring_get_key - 이름으로 키 reference 를 획득한다 (ref-count 증가).
 *
 * @name: 키 이름. "<keyring>:<key>" 형태로 keyring namespace 를 지정 가능.
 *        keyring 부분이 없으면 "global" keyring 에서 룩업한다. 키 이름에
 *        ":" 가 들어 있는 경우(예: "key0:foo") global 에서 가져오려면
 *        ":key0:foo" 처럼 빈 keyring 접두사를 명시한다 — 표준 SPDK 컨벤션.
 * @return: 키 reference (성공 시 ref-count +1) / NULL (해당 이름의 키 없음).
 *
 * 동기/배경: NVMe-oF TCP listener 생성 등에서 사용자가 RPC 인자로 "이
 * listener 는 PSK my_psk 로 TLS 한다" 라고 지정한다. SPDK 는 이를 받자
 * 마자 keyring 에서 my_psk 를 룩업해 ref 를 잡아둠으로써, 그 키가 listener
 * 가 동작하는 동안에는 절대 free 되지 않게 보장한다.
 *
 * 동작: 내부 키 hash table 에서 name 을 룩업 → 발견된 spdk_key 의 refcnt
 * 를 atomic 증가 → 포인터 반환. 락 없이 lockless 하게 구현된 변형도
 * 있을 수 있으나 일반적으로는 keyring lock 으로 보호된 hash 룩업이다.
 *
 * 실행 컨텍스트: 임의의 SPDK thread. 자주 호출되지 않는 control plane API.
 *
 * 호출 체인:
 *   RPC handler / NVMe-oF listen → [spdk_keyring_get_key] → 백엔드별 룩업
 */
/**
 * Get a reference to a key from the keyring.  The key must have been added to the keyring by
 * the appropriate keyring module.  The reference will be kept alive until its released via
 * `spdk_keyring_put_key()`.  If the key is removed from the keyring, the reference is kept alive, but
 * the key won't be usable anymore.
 *
 * \param name Name of a key.  The name can be optionally prepended with the name of a keyring to
 * retrieve the key from followed by a ":" character.  For instance, "keyring0:key0" would retrieve
 * a key "key0" from keyring "keyring0".  If omitted, "global" keyring will be used.  To get a key
 * with a ":" character in its name from the global keyring, empty keyring name should be specified
 * (e.g. ":key0:foo" refers to a key "key0:foo" in the global keyring).
 *
 * \return Reference to a key or NULL if the key doesn't exist.
 */
struct spdk_key *spdk_keyring_get_key(const char *name);

/*
 * [한국어]
 * spdk_keyring_put_key - 키 reference 를 반납한다 (ref-count 감소).
 *
 * @key: spdk_keyring_get_key 또는 spdk_key_dup 로 받은 ref. NULL 안전(no-op).
 * @return: void.
 *
 * 동기/배경: keyring 의 키 객체는 reference count 로 수명 관리된다. 모든
 * 사용자가 ref 를 반납하고 또한 백엔드 모듈이 해당 키를 remove 한 후에야
 * 실제 메모리가 free 되며, 그 시점에 key material 은 explicit_bzero 로
 * 0 채움한 뒤 free 된다 (메모리 잔류 공격 방어).
 *
 * 동작: refcnt atomic 감소 → 0 이 되고 keyring 에서 제거 상태이면 백엔드
 * remove_key 콜백을 호출하고 메모리를 zeroize/free.
 *
 * 실행 컨텍스트: 임의의 SPDK thread.
 *
 * 호출 체인:
 *   listener teardown → [spdk_keyring_put_key] → (refcnt==0 이면) zeroize/free
 */
/**
 * Release a reference to a key obtained from `spdk_keyring_get_key()`.
 *
 * \param key Reference to a key.  If NULL, this function is a no-op.
 */
void spdk_keyring_put_key(struct spdk_key *key);

/*
 * [한국어]
 * spdk_key_get_name - 키의 이름 문자열을 반환한다.
 *
 * @key: 유효한 키 ref (NULL 비허용 — 디버그 빌드에서 assert).
 * @return: NUL-terminated 문자열 포인터. 이 문자열은 키 객체의 수명에
 *          묶여 있으므로 호출자는 해제하지 않는다.
 *
 * 동기/배경: 디버깅·로깅·RPC list 출력 시 키 이름이 필요. 직접 내부 필드
 * 를 노출하지 않고 이 getter 로만 노출해 정보 은닉을 유지.
 *
 * 실행 컨텍스트: 임의의 SPDK thread. lock-free.
 *
 * 호출 체인:
 *   RPC keyring_get_keys → [spdk_key_get_name]
 */
/**
 * Get the name of a key.
 *
 * \param key Reference to a key.
 *
 * \return Name of the key.
 */
const char *spdk_key_get_name(struct spdk_key *key);

/*
 * [한국어]
 * spdk_key_get_key - 실제 key material 을 사용자 버퍼로 복사.
 *
 * @key: 유효한 키 ref.
 * @buf: 복사받을 사용자 버퍼. 호출 직후 사용한 뒤 explicit_bzero 등으로
 *       반드시 0 채움 처리할 것 (스택/힙에 key material 잔류 방지).
 * @len: buf 의 바이트 길이.
 * @return: 실제 복사된 바이트 수 (>=0) 또는 음수 errno (-ENOBUFS buf 부족,
 *          -EACCES 백엔드가 거부, -ENOKEY 키가 keyring 에서 제거됨).
 *
 * 동기/배경: TLS handshake 과정에서 PSK 를 OpenSSL 에 넘기거나, AES-XTS
 * 키 스케줄을 만들 때 raw key bytes 가 필요. keyring 은 이 시점에 백엔드
 * 모듈의 get_key 콜백을 통해 임시로 key material 을 노출한다. 보안상
 * 가능한 한 짧게 사용하고 즉시 zeroize 한다.
 *
 * 동작: 백엔드 모듈의 get_key 콜백을 호출 → 백엔드가 buf 에 key bytes 를
 * 복사 → 복사 길이를 반환.
 *
 * 실행 컨텍스트: 임의의 SPDK thread, 보통 connection setup 또는 crypto
 * key schedule 직전. hot path 가 아니다.
 *
 * 호출 체인:
 *   nvmf_tcp_set_psk / bdev_crypto_init → [spdk_key_get_key] → 백엔드
 *   get_key → buf 복사 → 호출자가 사용 후 explicit_bzero
 */
/**
 * Retrieve keying material from a key reference.
 *
 * \param key Reference to a key.
 * \param buf Buffer to write the data to.
 * \param len Size of the `buf` buffer.
 *
 * \return The number of bytes written to `buf` or negative errno on error.
 */
int spdk_key_get_key(struct spdk_key *key, void *buf, int len);

/*
 * [한국어]
 * spdk_key_dup - 키 reference 를 "복제" 한다 (실제로는 ref-count 증가만).
 *
 * @key: 복제할 키 ref.
 * @return: ref-count 가 1 증가된 동일 객체 포인터 (또는 정확히 같은 포인터).
 *
 * 동기/배경: 한 키를 두 컴포넌트가 공유 소유권으로 가져야 할 때, 각자
 * spdk_keyring_put_key 호출만으로 안전하게 정리할 수 있도록 ref-count 만
 * 분리해주는 패턴이 필요하다. 실제 객체를 복사하면 메모리 zeroize 보장이
 * 흔들리므로, "동일 객체 + ref+1" 만 수행한다.
 *
 * 동작: refcnt atomic +1 → 동일 포인터 반환.
 *
 * 실행 컨텍스트: 임의의 SPDK thread.
 *
 * 호출 체인:
 *   소유권 이전 코드 → [spdk_key_dup] → atomic_inc
 */
/**
 * Duplicate a key.  The returned key reference might be a pointer to the same exact object.  After
 * duplicating a key, the new reference should be released via `spdk_keyring_put_key()`.
 *
 * \param key Reference to a key.
 *
 * \return Pointer to the key reference.
 */
struct spdk_key *spdk_key_dup(struct spdk_key *key);

/*
 * [한국어]
 * spdk_keyring_init - keyring 라이브러리를 초기화한다.
 *
 * @return: 0 성공 / 음수 errno.
 *
 * 동기/배경: 내부 hash table, 등록된 백엔드 모듈들의 init() 콜백, "global"
 * 기본 keyring 등을 셋업한다. 이 함수가 호출되기 전까지는 add/get_key 가
 * 모두 -EAGAIN 또는 -ENODEV 로 실패한다.
 *
 * 동작: subsystem lock 잡기 → 등록된 spdk_keyring_module 리스트를 순회하며
 * 각 모듈의 init() 호출 → "global" keyring 자료구조 생성.
 *
 * 실행 컨텍스트: 애플리케이션 init 단계 (spdk_subsystem_init 에서 자동
 * 호출). 1 회만 호출.
 *
 * 호출 체인:
 *   subsystem_init → [spdk_keyring_init] → 모듈별 init 콜백
 */
/**
 * Initialize the keyring library.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_keyring_init(void);

/*
 * [한국어]
 * spdk_keyring_cleanup - keyring 라이브러리의 모든 자원을 해제.
 *
 * @return: void.
 *
 * 동기/배경: 종료 시 모든 키를 keyring 에서 제거하고, ref-count 가 0 이
 * 된 키는 즉시 zeroize/free 한다. 활성 ref 가 남아 있는 키는 사용자가
 * put_key 할 때 자동으로 정리된다.
 *
 * 동작: 모든 등록된 키에 대해 백엔드 remove_key 콜백 → 모듈별 cleanup() →
 * 내부 hash table free.
 *
 * 실행 컨텍스트: 애플리케이션 shutdown.
 *
 * 호출 체인:
 *   subsystem_fini → [spdk_keyring_cleanup] → 모듈별 cleanup → free
 */
/**
 * Free any resources acquired by the keyring library.  This function will free all of the keys.
 */
void spdk_keyring_cleanup(void);

/* [한국어] struct spdk_keyring - 서브 keyring 핸들 (불투명). 현재 공개 API
 * 는 keyring 분리 사용을 거의 지원하지 않으며 (for_each_key 가 NULL=전체
 * 만 받음), 향후 확장을 위한 forward declaration 이다. */
struct spdk_keyring;

/* [한국어] SPDK_KEYRING_FOR_EACH_ALL - spdk_keyring_for_each_key 의 flags
 * 인자에 비트 OR 로 전달하는 플래그.
 * 의미: keyring 에서 명시적으로 remove 되었으나 아직 활성 ref 가 살아 있는
 * "removed but referenced" 키까지 포함해 순회.
 * 사용처: RPC keyring_get_keys 등에서 디버깅용으로 모든 살아 있는 객체를
 * 보고 싶을 때. 일반 사용자 코드는 이 플래그 없이 active 키만 보면 충분.
 * 비트 패턴: 0x1. */
/** Iterate over all keys including those that were removed, but still have active references */
#define SPDK_KEYRING_FOR_EACH_ALL 0x1

/*
 * [한국어]
 * spdk_keyring_for_each_key - keyring 의 모든 키를 순회하며 사용자 콜백 실행.
 *
 * @keyring: 순회할 keyring. 현재 NULL (모든 keyring) 만 지원.
 * @ctx: fn 에 전달될 사용자 컨텍스트 포인터.
 * @fn: 각 키에 대해 호출되는 콜백. 콜백 안에서 spdk_key_get_name,
 *      spdk_key_get_key 등을 안전하게 호출 가능 (ref 가 keyring 에 의해
 *      유지되는 동안만).
 * @flags: SPDK_KEYRING_FOR_EACH_ALL 등 비트 OR. 0 이면 active 키만 순회.
 * @return: void.
 *
 * 동기/배경: RPC `keyring_get_keys` 가 현재 등록된 모든 키 정보를 클라이언트
 * 에 dump 하는 용도. 각 키는 백엔드별 dump_info 를 통해 비밀이 아닌 메타
 * 정보(이름, 모듈, 사이즈)만 노출된다.
 *
 * 동작: 내부 락 잡기 → hash table 또는 list 순회 → 각 항목에 대해 fn(ctx, key)
 * 호출 → 락 해제. 콜백 실행 동안 keyring 락이 잡혀 있을 수 있으므로
 * 콜백 안에서 keyring API 를 재진입 호출하면 데드락 위험.
 *
 * 실행 컨텍스트: RPC 핸들러 등 control plane.
 *
 * 호출 체인:
 *   RPC keyring_get_keys → [spdk_keyring_for_each_key] → 사용자 fn 반복 호출
 */
/**
 * Execute a function on each registered key attached to a given keyring.  For now, this function
 * only supports iterating over keys from all keyrings and the `keyring` parameter must be set to
 * NULL.
 *
 * \param keyring Keyring over which to iterate.  If NULL, iterate over keys from all keyrings.
 * \param ctx Context to pass to the function.
 * \param fn Function to call.
 * \param flags Flags controlling the keys to iterate over.
 */
void spdk_keyring_for_each_key(struct spdk_keyring *keyring, void *ctx,
			       void (*fn)(void *ctx, struct spdk_key *key), uint32_t flags);

/*
 * [한국어]
 * spdk_keyring_write_config - 현재 keyring 상태를 JSON 으로 직렬화.
 *
 * @w: SPDK JSON streaming writer 컨텍스트.
 * @return: void.
 *
 * 동기/배경: SPDK 의 RPC `save_config` 가 호출되면 모든 서브시스템이 자기
 * 상태를 JSON-RPC 호출 시퀀스로 dump 해 그대로 다시 적용 가능하게 한다.
 * keyring 도 그 흐름의 일부로 등록된 모듈별 키를 직렬화하지만, key
 * material 자체는 절대 dump 하지 않고 (보안), 모듈이 알고 있는 path
 * 정보 등 비밀 아닌 메타데이터만 출력한다.
 *
 * 동작: 등록된 백엔드 모듈을 순회하며 module->write_config(w) 콜백 호출.
 *
 * 실행 컨텍스트: RPC `save_config` 처리 스레드.
 *
 * 호출 체인:
 *   RPC save_config → [spdk_keyring_write_config] → 모듈별 write_config
 */
/**
 * Write keyring configuration to JSON.
 *
 * \param w JSON write context.
 */
void spdk_keyring_write_config(struct spdk_json_write_ctx *w);

/* [한국어] extern "C" 가드 종료. */
#ifdef __cplusplus
}
#endif

#endif /* SPDK_KEYRING_H */
/* [한국어] 헤더 가드 종료. */
