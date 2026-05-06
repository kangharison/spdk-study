/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

/*
 * [한국어 설명] SPDK Keyring 백엔드 모듈 작성자용 API 헤더 (keyring_module.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK keyring 라이브러리(lib/keyring) 의 백엔드 모듈을 작성하는
 * 코드(예: module/keyring/file 의 파일 기반 keyring, module/keyring/linux 의
 * Linux kernel keyring 어댑터) 가 사용해야 하는 인터페이스를 선언한다.
 * keyring 의 사용자 측 API (spdk_keyring_get_key 등) 는 keyring.h 에 있고,
 * 그 백엔드 측 vtable 인 struct spdk_keyring_module, 키 등록/해제 함수
 * (spdk_keyring_add_key / remove_key), 그리고 키에 부착된 모듈 전용 컨텍스트
 * 접근자 (spdk_key_get_ctx / get_module) 가 여기 모인다. SPDK_KEYRING_REGISTER_MODULE
 * constructor 매크로를 통해 모듈은 application start 이전에 자동 등록된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * keyring 은 정확히 두 층으로 분리된다. 위쪽은 keyring 코어(lib/keyring)
 * 가 hash table / ref-count / zeroize / namespacing 을 책임지고, 아래쪽은
 * 본 헤더가 정의하는 vtable 을 구현한 백엔드 모듈이 실제 키 저장소
 * (파일, kernel keyring, HSM 어댑터 등) 와 통신한다. 호출 흐름은 사용자
 * RPC `keyring_file_add_key` 또는 `keyring_linux_get_key` 등이 들어오면
 * 모듈의 add_key/probe_key 가 실행되어 새 spdk_key 를 만들고
 * spdk_keyring_add_key 로 코어에 등록한다. 사용자가 spdk_keyring_get_key
 * 로 키를 사용할 때마다 코어가 백엔드의 get_key 콜백을 호출해 key
 * material 을 임시 노출시킨다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h (정수형/size_t), spdk/json.h (write_config / dump_info
 * 가 JSON streaming writer 사용), spdk/keyring.h (사용자 API 측 spdk_key
 * 타입), spdk/queue.h (TAILQ_ENTRY 매크로 — 모듈 리스트 링크). 사용처:
 * module/keyring/* 디렉토리의 각 백엔드 모듈, 그리고 외부 plugin 형태의
 * keyring 백엔드. 데이터 흐름은 (백엔드 저장소 = 파일/kernel keyring) →
 * 모듈 콜백 → keyring 코어 → 사용자 ref → 사용자 임시 buffer.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_keyring_module: 모듈 vtable. name, init, cleanup,
 *   write_config, probe_key, add_key, remove_key, get_key, get_ctx_size,
 *   dump_info 함수 포인터와 모듈 리스트 링크 TAILQ_ENTRY.
 * - struct spdk_key_opts: spdk_keyring_add_key 의 입력 옵션 (size, name,
 *   module, ctx). OPTS_SIZE 패턴.
 * - spdk_keyring_add_key(opts): 모듈이 새 키를 keyring 코어에 등록.
 * - spdk_keyring_remove_key(name, module): 모듈이 자기 키를 코어에서 제거.
 * - spdk_keyring_register_module(module): 모듈 등록 (보통 매크로가 호출).
 * - SPDK_KEYRING_REGISTER_MODULE: __attribute__((constructor)) 로
 *   main() 이전에 모듈을 자동 등록하는 매크로.
 * - spdk_key_get_ctx(key) / spdk_key_get_module(key): 모듈 코드 안에서
 *   자기 컨텍스트와 소유 모듈을 역참조하는 접근자.
 */

/* [한국어] SPDK_KEYRING_MODULE_H — 헤더 가드. 백엔드 모듈 빌드 단위에서
 * 포함되므로 가드는 필수. */
#ifndef SPDK_KEYRING_MODULE_H
#define SPDK_KEYRING_MODULE_H

/* [한국어] spdk/stdinc.h - size_t/int 등 표준 정수형 묶음 헤더. */
#include "spdk/stdinc.h"
/* [한국어] spdk/json.h - write_config / dump_info 콜백이 spdk_json_write_ctx*
 * 를 통해 키 메타정보를 JSON 으로 출력하므로 필수. */
#include "spdk/json.h"
/* [한국어] spdk/keyring.h - 사용자 측 API 와 spdk_key 불투명 타입을
 * 가져오기 위한 헤더. 백엔드 모듈은 종종 자기 자신이 만든 키를 다시
 * spdk_keyring_get_key 로 다른 사용자에게 노출하므로 양방향 의존이 자연스럽다. */
#include "spdk/keyring.h"
/* [한국어] spdk/queue.h - TAILQ_HEAD/TAILQ_ENTRY 매크로 정의. 등록된 모듈은
 * 코어 내부의 TAILQ 에 링크되며, 그 링크 필드(tailq) 가 spdk_keyring_module
 * 의 마지막 멤버로 들어 있다. */
#include "spdk/queue.h"

/* [한국어] C++ 컴파일러용 extern "C" 가드 시작. */
#ifdef __cplusplus
extern "C" {
#endif

/* [한국어] struct spdk_keyring_module - 모듈 vtable 의 forward declaration.
 * struct spdk_key_opts 가 module 포인터를 멤버로 가지므로 먼저 선언이 필요. */
struct spdk_keyring_module;

/* [한국어] struct spdk_key_opts - spdk_keyring_add_key 의 입력 옵션.
 * 백엔드 모듈이 자기 저장소에서 키를 발견·생성한 뒤, 그 키를 keyring 코어에
 * 등록할 때 채워서 전달하는 구조체. ABI/소스 호환을 위해 size_t size 를
 * 첫 필드로 두는 SPDK 표준 OPTS_SIZE 패턴을 따른다. */
struct spdk_key_opts {
	size_t size;
	/* [한국어] size — 호출자가 인식하는 이 구조체의 바이트 크기.
	 * 설정자: 모듈 코드가 sizeof(struct spdk_key_opts) 로 채워 전달.
	 * 읽는 자: keyring 코어 — size 까지의 필드만 유효한 것으로 간주하고,
	 *          그 너머는 기본값으로 패딩.
	 * 값 범위: 0 < size <= 컴파일 시점 sizeof(struct spdk_key_opts).
	 * 동기화: 호출자 스레드 로컬 — 별도 sync 불필요. */

	const char *name;
	/* [한국어] name — keyring 안에서의 키 이름 (NUL-terminated).
	 * 설정자: 모듈이 RPC 인자에서 받은 사용자 지정 이름.
	 * 읽는 자: 코어가 hash table 키로 사용. spdk_keyring_get_key 의 룩업
	 *          name 과 매칭되어야 함.
	 * 값 범위: NULL 비허용. ":" 문자가 들어 있으면 사용자가 keyring
	 *          namespace 와 구분하기 위한 escape rule (keyring.h 참조).
	 * 동기화: name 문자열은 코어가 strdup 으로 복사 보관 — 호출자는
	 *          원본을 등록 후 free 해도 안전. */

	struct spdk_keyring_module *module;
	/* [한국어] module — 이 키를 소유하는 백엔드 모듈의 vtable 포인터.
	 * 설정자: 모듈이 자기 자신의 정적 vtable 포인터를 채워 넣음.
	 * 읽는 자: 코어가 키 생성 / get_key / remove_key 위임 시 vtable 의
	 *          함수 포인터를 사용.
	 * 값 범위: NULL 비허용. spdk_keyring_register_module 으로 이미 등록된
	 *          모듈만 허용.
	 * 동기화: vtable 은 정적 const 데이터 — 별도 sync 불필요. */

	void *ctx;
	/* [한국어] ctx — 모듈 add_key 콜백에 그대로 전달되는 사용자 정의
	 *               불투명 포인터.
	 * 설정자: 모듈 RPC 핸들러가 키 생성 시점의 입력(예: 파일 경로)을 담은
	 *          자기 정의 구조체 포인터를 넣음.
	 * 읽는 자: 모듈의 add_key 콜백이 동일 ctx 를 받아 그 입력을 해석.
	 * 값 범위: 모듈에 따라 다름; NULL 가능 (모듈이 정의).
	 * 동기화: 코어는 ctx 를 그대로 전달만 하며 보관하지 않음 — add_key
	 *          반환 직후 호출자가 ctx 를 free 해도 안전. */
};

/*
 * [한국어]
 * spdk_keyring_add_key - 백엔드 모듈이 새 키를 keyring 코어에 등록한다.
 *
 * @opts: 키 옵션 구조체 (이름, 소유 모듈, ctx, OPTS_SIZE).
 * @return: 0 성공 / 음수 errno (-EEXIST 동일 이름 키 존재, -EINVAL 옵션
 *          오류, -ENOMEM 등).
 *
 * 동기/배경: 백엔드 모듈의 RPC 핸들러 (예: keyring_file_add_key) 가 사용자
 * 입력으로부터 키 메타정보를 받아 keyring 코어에 명시적으로 추가하는
 * 진입점. 코어는 모듈->get_ctx_size() 만큼 추가 메모리를 할당해 모듈 전용
 * 컨텍스트를 spdk_key 객체 뒤에 붙인 뒤, 모듈->add_key(key, opts->ctx)
 * 콜백을 호출해 모듈이 그 컨텍스트를 채우도록 한다.
 *
 * 동작:
 *   1) opts 검증 (size/name/module 필수).
 *   2) hash table 에 동일 이름 키 존재 검사.
 *   3) module->get_ctx_size() 만큼 추가 영역을 포함해 spdk_key 할당.
 *   4) 키를 hash table 에 삽입하고 module->add_key(key, opts->ctx) 호출.
 *   5) 콜백 실패 시 키를 제거·free 한 뒤 errno 반환.
 *
 * 실행 컨텍스트: 백엔드 모듈의 RPC 핸들러 — control plane 스레드.
 *
 * 호출 체인:
 *   RPC keyring_*_add_key → [spdk_keyring_add_key] → module->add_key
 */
/**
 * Add a key to the keyring.
 *
 * \param opts Key options.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_keyring_add_key(const struct spdk_key_opts *opts);

/*
 * [한국어]
 * spdk_keyring_remove_key - 백엔드 모듈이 자기 키를 코어에서 제거.
 *
 * @name: 제거할 키 이름.
 * @module: 키를 소유한 모듈 vtable 포인터 (자기 자신).
 * @return: 0 성공 / 음수 errno (-ENOKEY 해당 키 없음, -EACCES 다른
 *          모듈의 키 삭제 시도).
 *
 * 동기/배경: 사용자가 RPC 로 명시적으로 키를 빼라고 요청했을 때, 또는
 * 백엔드가 외부 변경(파일 삭제 등)을 감지했을 때 호출. 활성 ref 가 남아
 * 있으면 키 객체는 즉시 free 되지 않고 "removed" 상태로 표시되어, 모든
 * ref 가 풀린 직후 zeroize/free 된다.
 *
 * 동작: hash table 에서 키 찾기 → 모듈 일치 검사 → 코어 가시성에서 제거
 * (이후 spdk_keyring_get_key 는 NULL 반환) → ref-count 가 0 이면 즉시
 * module->remove_key 콜백 호출 후 zeroize/free, 아니면 zombie 상태로 보관.
 *
 * 실행 컨텍스트: 백엔드 모듈의 RPC 핸들러.
 *
 * 호출 체인:
 *   RPC keyring_*_remove_key → [spdk_keyring_remove_key] → (refcnt==0)
 *   → module->remove_key
 */
/**
 * Remove a key from the keyring.
 *
 * \param name Name of the key to remove.
 * \param module Module owning the key to remove.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_keyring_remove_key(const char *name, struct spdk_keyring_module *module);

/* [한국어] struct spdk_keyring_module - keyring 백엔드 모듈의 vtable.
 * SPDK 의 다른 vtable 패턴(spdk_accel_module_if, spdk_bdev_module 등)과
 * 동일하게, 정적 const 인스턴스 하나를 모듈이 정의하고 SPDK_KEYRING_REGISTER_MODULE
 * 매크로로 등록한다. */
struct spdk_keyring_module {
	const char *name;
	/* [한국어] name — 모듈 이름 (NUL-terminated).
	 * 설정자: 모듈 코드가 정적 문자열 리터럴로 지정 (예: "file", "linux").
	 * 읽는 자: 코어가 RPC 응답·로그·write_config 출력에서 사용.
	 * 값 범위: 영숫자 + '_'/'-' 정도; 다른 모듈 이름과 unique 해야 함.
	 * 동기화: 정적 데이터 — 불변. */

	int (*init)(void);
	/* [한국어] init — 모듈 초기화 콜백.
	 * 설정자: 모듈 코드가 자기 init 함수 포인터를 지정.
	 * 호출자: spdk_keyring_init 이 등록된 모든 모듈을 순회하며 호출.
	 * 반환: 0 성공 / 음수 errno (실패 시 그 모듈은 비활성).
	 * 의미: 백엔드 저장소 핸들 생성, 환경 검증 등. */

	void (*cleanup)(void);
	/* [한국어] cleanup — 모듈 정리 콜백.
	 * 호출자: spdk_keyring_cleanup.
	 * 의미: init 에서 잡은 자원 해제. 키들은 코어가 알아서 정리하므로
	 *       모듈은 자기 전역 상태만 정리하면 됨. */

	void (*write_config)(struct spdk_json_write_ctx *w);
	/* [한국어] write_config — 모듈 설정을 JSON-RPC 형태로 직렬화하는 콜백.
	 * 호출자: spdk_keyring_write_config (RPC save_config 흐름).
	 * 의미: 비밀이 아닌 모듈 설정(파일 경로, 옵션) 만 dump. key material
	 *       자체는 절대 dump 하지 않는다 (보안). */

	int (*probe_key)(const char *name);
	/* [한국어] probe_key — keyring 코어가 모르는 이름을 룩업할 때 호출되는
	 *                    "lazy load" 콜백.
	 * 호출자: 코어가 spdk_keyring_get_key(name) 에서 hash miss 가 났을 때
	 *          등록된 각 모듈의 probe_key 를 차례로 호출.
	 * 반환: 0 (모듈이 그 이름의 키를 발견·등록했음 — 코어는 다시 hash
	 *        룩업) 또는 -ENOKEY (이 모듈은 해당 키를 모름).
	 * 의미: Linux kernel keyring 처럼 OS 측에 키가 추가될 때 사용 직전에
	 *       동적으로 가져오는 시나리오에 사용. */

	int (*add_key)(struct spdk_key *key, void *ctx);
	/* [한국어] add_key — spdk_keyring_add_key 가 새 키 객체를 만든 뒤 호출.
	 * 호출자: spdk_keyring_add_key.
	 * 인자: key — 코어가 갓 할당한 spdk_key (그 뒤에 모듈 ctx 영역이 붙음),
	 *        ctx — 사용자가 spdk_key_opts.ctx 로 전달한 입력값.
	 * 반환: 0 성공 / 음수 errno (실패 시 코어가 키를 제거).
	 * 의미: 모듈이 자기 ctx 영역에 백엔드 핸들·키 path 등을 채워넣음. */

	void (*remove_key)(struct spdk_key *key);
	/* [한국어] remove_key — 키 zeroize·free 직전에 호출되는 백엔드 정리.
	 * 호출자: spdk_keyring_remove_key 또는 마지막 ref put 시점.
	 * 의미: 백엔드가 잡고 있던 자원 해제 (파일 핸들 close, kernel keyring
	 *       refcnt 감소 등). */

	int (*get_key)(struct spdk_key *key, void *buf, int len);
	/* [한국어] get_key — 사용자 spdk_key_get_key 호출 시 위임되는 콜백.
	 * 호출자: spdk_key_get_key.
	 * 인자: buf/len — 사용자 버퍼와 길이.
	 * 반환: 복사한 바이트 수 (>=0) / 음수 errno.
	 * 의미: 백엔드 저장소에서 raw key bytes 를 buf 로 복사 (예: 파일 read,
	 *       keyctl_read 등). */

	size_t (*get_ctx_size)(void);
	/* [한국어] get_ctx_size — 모듈이 spdk_key 뒤에 붙여 사용할 컨텍스트
	 *                      바이트 수.
	 * 호출자: spdk_keyring_add_key 가 키 할당 직전에 사이즈 결정 용도로 호출.
	 * 반환: 0 (컨텍스트 불필요) 또는 양수.
	 * 의미: 모듈은 spdk_key_get_ctx 로 이 영역에 접근. */

	void (*dump_info)(struct spdk_key *key, struct spdk_json_write_ctx *w);
	/* [한국어] dump_info — 키 메타정보를 JSON 으로 출력 (RPC list 등).
	 * 호출자: keyring 코어 또는 RPC 핸들러.
	 * 의미: name 외 비밀이 아닌 속성 (모듈명, 사이즈, 파일 경로 등) 출력.
	 *        key material 은 절대 출력 금지 (보안). */

	TAILQ_ENTRY(spdk_keyring_module) tailq;
	/* [한국어] tailq — keyring 코어 내부 모듈 리스트의 링크 필드.
	 * 설정자: spdk_keyring_register_module.
	 * 읽는 자: 코어가 init/cleanup/write_config 시 모든 모듈을 순회할 때.
	 * 동기화: 모듈 등록은 main() 이전 (constructor) 에 끝나므로 이후
	 *          순회는 lock 없이 안전. */
};

/*
 * [한국어]
 * spdk_keyring_register_module - 백엔드 모듈을 코어 리스트에 등록.
 *
 * @module: 모듈 vtable 정적 인스턴스 포인터.
 * @return: void.
 *
 * 동기/배경: 보통 SPDK_KEYRING_REGISTER_MODULE 매크로가 main() 이전에
 * (constructor 속성) 자동 호출하므로, 사용자 코드가 직접 부를 일은 거의 없다.
 *
 * 실행 컨텍스트: 프로세스 시작 직후의 constructor 호출 — 단일 스레드.
 */
/**
 * Register a keyring module.
 *
 * \param module Keyring module to register.
 */
void spdk_keyring_register_module(struct spdk_keyring_module *module);

/* [한국어] SPDK_KEYRING_REGISTER_MODULE - 백엔드 모듈을 main() 이전에 자동
 * 등록하는 매크로. __attribute__((constructor)) 가 붙은 익명에 가까운 정적
 * 함수를 만들어, ELF 로더가 프로그램 시작 시점에 자동 호출하게 한다.
 *
 * 사용 예:
 *   static struct spdk_keyring_module g_module_file = { .name = "file", ... };
 *   SPDK_KEYRING_REGISTER_MODULE(file, &g_module_file);
 *
 * 이 패턴은 SPDK 가 다른 곳에서도 자주 쓰는 자동 등록 패턴 (SPDK_BDEV_MODULE_REGISTER,
 * SPDK_ACCEL_MODULE_REGISTER 등) 과 동일하다 — 사용자가 명시적인 init 코드를
 * 추가하지 않고도 모듈을 SPDK 본체와 정적/동적으로 링크하기만 하면 자동
 * 등록된다는 장점이 있다. 매크로 인자 name 은 함수 심볼 충돌을 피하기 위해
 * concatenation 으로 주입된다. */
#define SPDK_KEYRING_REGISTER_MODULE(name, module) \
static void __attribute__((constructor)) _spdk_keyring_register_##name(void) \
{ \
	spdk_keyring_register_module(module); \
}

/*
 * [한국어]
 * spdk_key_get_ctx - 키에 부착된 모듈 전용 컨텍스트 영역의 시작 포인터를 반환.
 *
 * @key: 키 객체.
 * @return: spdk_key 객체 뒤에 module->get_ctx_size() 만큼 할당된 컨텍스트
 *          영역의 첫 바이트 포인터.
 *
 * 동기/배경: 백엔드 모듈은 키마다 자기 메타데이터(파일 디스크립터, kernel
 * keyring serial, HSM 핸들 등)를 보관해야 한다. 별도 alloc 대신 spdk_key
 * 와 함께 한 번에 할당해 cache locality 를 높이고 lifecycle 을 자연스럽게
 * 묶는 패턴이며, 이 함수는 그 영역에 접근하는 표준 방법이다.
 *
 * 실행 컨텍스트: 모듈 콜백 안 — 임의의 SPDK thread.
 *
 * 호출 체인:
 *   module->add_key/remove_key/get_key/dump_info → [spdk_key_get_ctx]
 */
/**
 * Get pointer to the module context associated with a key.
 *
 * \param key Key.
 *
 * \return Key context.
 */
void *spdk_key_get_ctx(struct spdk_key *key);

/*
 * [한국어]
 * spdk_key_get_module - 키를 소유한 백엔드 모듈의 vtable 포인터를 반환.
 *
 * @key: 키 객체.
 * @return: spdk_keyring_module 포인터.
 *
 * 동기/배경: 다중 백엔드가 활성화되어 있을 때, 어떤 키든 자기 모듈을
 * 역참조해 동적으로 동작을 분기하고자 할 때 사용 (예: dump_info 일반
 * 코드에서 모듈에 따라 다른 필드를 출력).
 *
 * 실행 컨텍스트: 임의의 SPDK thread.
 *
 * 호출 체인:
 *   keyring 코어 / 모듈 dispatcher → [spdk_key_get_module]
 */
/**
 * Get keyring module owning the key.
 *
 * \param key Key.
 *
 * \return Key owner.
 */
struct spdk_keyring_module *spdk_key_get_module(struct spdk_key *key);

/* [한국어] extern "C" 가드 종료. */
#ifdef __cplusplus
}
#endif

#endif /* SPDK_KEYRING_H */
/* [한국어] 헤더 가드 종료. (원본의 코멘트 텍스트는 SPDK_KEYRING_H 라고
 * 잘못 표기되어 있지만 가드 매크로는 SPDK_KEYRING_MODULE_H 로 정의되어
 * 있어 동작상 문제는 없다 — 이 주석은 원본 텍스트를 보존한 것임.) */
