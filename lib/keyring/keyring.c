/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

/*
 * [한국어 설명] SPDK keyring 라이브러리 본체 (keyring.c)
 *
 * === 파일의 역할 ===
 * SPDK가 보안에 사용하는 다양한 비밀 키(NVMe-oF TLS PSK, NVMe in-band auth
 * DH-CHAP secret, NVMe AES-XTS DEK 등)를 단일 통합 인터페이스로 보관하는
 * "keyring" 라이브러리 본체. 키 자체의 비밀 데이터는 본 파일이 직접 다루지
 * 않고, 백엔드(file 모듈, Linux kernel keyring 모듈 등)가 다룬다 — 본 파일은
 * (1) 모듈 등록부, (2) name 기반 키 lookup TAILQ, (3) refcount lifecycle,
 * (4) "removed but referenced" 상태 보존, (5) RPC dump 헬퍼만 책임진다.
 * 키 이름은 "<keyring>:<key>" 형태인데 현재는 전역 keyring 1개만 지원하므로
 * keyring 부분은 무시되고(":key" == "key"), 모듈은 콜론 prefix를 만나면 해당
 * keyring을 찾아 삽입해야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 보안 키 사용자(nvme/nvmf transport, bdev_crypto 등) ↔ 백엔드(file/keyutils
 * 등) 사이의 중간 레이어. 호출 체인:
 *   - 등록 측: 각 keyring 모듈이 자기 init에서 SPDK_KEYRING_REGISTER_MODULE
 *              매크로로 g_keyring.modules에 등록. spdk_keyring_init()에서
 *              module->init()을 순차 호출.
 *   - add 경로: 사용자(보통 RPC 핸들러)가 spdk_keyring_add_key(opts) 호출 →
 *              본 파일이 spdk_key 객체 할당 → module->add_key 호출(키 데이터
 *              실제 보관/생성은 모듈이 함) → keys TAILQ에 link.
 *   - get 경로: 소비자(예: nvmf TLS handshake)가 spdk_keyring_get_key(name) →
 *              keys TAILQ 검색 → 미발견 시 modules의 probe_key 콜백으로
 *              backing store(file/kernel keyring)에서 자동 import → refcnt++.
 *   - put 경로: 소비자가 spdk_keyring_put_key → refcnt-- → probed 키이고
 *              마지막 소비자가 떠나면 자동 unload(remove).
 *   - remove 경로: 명시적 spdk_keyring_remove_key → 백엔드 콜백 호출 후
 *              keys → removed_keys로 이동(refcnt가 0이 될 때까지 보존).
 *   - dump: keyring_get_keys RPC → spdk_keyring_for_each_key →
 *              keyring_dump_key_info → module->dump_info.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - keyring_internal.h: dump 헬퍼 forward 선언.
 *   - spdk/keyring.h: opaque struct spdk_key, 공개 add/remove/get/put API.
 *   - spdk/keyring_module.h: 모듈 콜백 인터페이스 (add/remove/get/probe/
 *     dump_info/write_config/init/cleanup).
 *   - spdk/queue.h: TAILQ.
 *   - spdk/log.h: ERRLOG/INFOLOG.
 *   - spdk/string.h: spdk_strerror.
 * 의존됨: nvmf/tcp transport, nvme TLS, bdev_crypto, AES-XTS 등.
 *
 * 보안 고려: 키 자체의 비밀 데이터는 spdk_key 뒤에 따라오는 module ctx 영역
 *   (calloc(... + module->get_ctx_size())로 할당)에 보관된다. 본 파일은 해당
 *   영역을 read-only로 다루며, 키 free 시 spdk_strzero 호출은 하지 않는데,
 *   이는 현재 구현 한계 — 모듈이 ctx 정리 시 자체적으로 zeroize할 책임을 진다.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_key: name + refcnt + removed/probed flag + module + tailq link
 *     로 이뤄지는 키 메타데이터. 비밀 데이터는 본 구조체 뒤에 모듈별 ctx로
 *     연접 할당.
 *   - struct spdk_keyring: 전역 keyring — recursive mutex + 모듈 TAILQ +
 *     active keys TAILQ + removed keys TAILQ.
 *   - keyring_get_key_name(name): "<keyring>:<key>" 파싱 — 현재는 ":" 뒤만 추출.
 *   - keyring_find_key/probe_key: name 기반 lookup, 없으면 모듈 probe로 import.
 *   - spdk_keyring_add_key/remove_key/get_key/put_key/key_dup: 공개 lifecycle.
 *   - keyring_dump_key_info: RPC 응답용 JSON 직렬화 (keyring_internal.h).
 *   - spdk_keyring_init/cleanup: 모듈 init 순차 + 실패 rollback / 키 강제 정리.
 */

#include "keyring_internal.h"        /* [한국어] dump_key_info forward 선언 */
#include "spdk/keyring.h"            /* [한국어] opaque spdk_key, 공개 API 시그니처 */
#include "spdk/keyring_module.h"     /* [한국어] keyring 모듈 인터페이스 (probe/add/remove/dump_info 등) */
#include "spdk/log.h"                /* [한국어] SPDK_ERRLOG/INFOLOG/WARNLOG */
#include "spdk/queue.h"              /* [한국어] TAILQ_HEAD/ENTRY/INSERT/REMOVE/FOREACH */
#include "spdk/string.h"             /* [한국어] spdk_strerror — errno → 사람이 읽을 메시지 */

/* [한국어] 단일 키 메타데이터. 비밀 데이터(실제 byte)는 본 구조체 뒤에 모듈이
 * 정의한 ctx 형태로 연접 할당된다 (spdk_keyring_add_key의 calloc 참고). */
struct spdk_key {
	char				*name;
	/* [한국어] 키 식별자 — strdup된 사본. "keyring:key" 또는 "key" 형식.
	 * 설정자: spdk_keyring_add_key(opts->name).
	 * 읽는 자: keyring_find_key (name 비교 시 keyring_get_key_name로 정규화),
	 *   keyring_dump_key_info (RPC 응답 필드).
	 * Lifetime: 키 객체와 함께 keyring_free_key에서 free.
	 * 동기화: 등록 후 변경되지 않음 (immutable). */

	int				refcnt;
	/* [한국어] 키 참조 횟수. add 시 1로 시작.
	 * 설정자: get_key/key_dup이 ++; put_key가 --.
	 * 읽는 자: put_key가 0/1을 검사해 free 또는 자동 unload 결정.
	 * 동기화: g_keyring.mutex 보호. */

	bool				removed;
	/* [한국어] 백엔드에서 이미 제거된 상태인지 표시.
	 * true가 되면 이 키는 keys TAILQ에서 제거되어 removed_keys TAILQ로 이동,
	 * 새로운 get_key는 이 키를 발견하지 못한다. spdk_key_get_key 호출은
	 * -ENOKEY 반환.
	 * 설정자: keyring_remove_key. 읽는 자: spdk_key_get_key, dump_info, put_key. */

	bool				probed;
	/* [한국어] 백엔드에서 자동 import(probe)된 키인지 표시.
	 * true이면 마지막 소비자가 put_key로 떠날 때 자동으로 remove를 트리거 —
	 * 즉 lazy load + auto unload. 명시적으로 add된 키는 false라 명시 remove
	 * 전까지 keyring에 남는다.
	 * 설정자: keyring_probe_key. 읽는 자: put_key의 자동 unload 분기. */

	struct spdk_keyring_module	*module;
	/* [한국어] 키를 소유하는 백엔드 모듈 포인터.
	 * 설정자: add_key가 모듈 인자를 저장. 읽는 자: get_key/remove_key/dump_info
	 *   가 module->콜백을 디스패치할 때 사용. Lifetime: 모듈 자체는 init 후
	 *   영원히 살아있음(unregister 없음). */

	TAILQ_ENTRY(spdk_key)		tailq;
	/* [한국어] g_keyring.keys 또는 g_keyring.removed_keys 큐 링크.
	 *   removed flag에 따라 두 큐 중 한 곳에만 들어 있음. 보호: g_keyring.mutex. */
};

/* [한국어] 전역 keyring 컨테이너. 현재 SPDK는 이 1개만 사용 — multi-keyring은
 * 미래 확장 여지. 이름 prefix "<keyring>:" 파싱이 일부 코드에 남아 있는 이유. */
struct spdk_keyring {
	pthread_mutex_t				mutex;
	/* [한국어] keyring 전체 보호 락 — keys/removed_keys/modules + 키별 refcnt.
	 * spdk_keyring_init에서 PTHREAD_MUTEX_RECURSIVE 속성으로 init되므로 같은
	 *   thread가 재진입해도 데드락 안 남 (예: probe_key→find_key 경로). */

	TAILQ_HEAD(, spdk_keyring_module)	modules;
	/* [한국어] 등록된 keyring 백엔드 모듈 목록.
	 * 설정자: SPDK_KEYRING_REGISTER_MODULE 매크로(constructor)가 INSERT_TAIL.
	 * 읽는 자: get_key의 probe loop, write_config, init/cleanup.
	 * 보호: 사실상 init 시점 이후 변경되지 않음(unregister 없음)이지만 mutex
	 *   하에서 접근. */

	TAILQ_HEAD(, spdk_key)			keys;
	/* [한국어] 활성 키(removed=false) 목록. find_key/for_each_key가 순회.
	 * 보호: mutex. */

	TAILQ_HEAD(, spdk_key)			removed_keys;
	/* [한국어] 백엔드 제거 후 refcnt가 남아있어 임시 보존 중인 키 목록.
	 * refcnt=0이 되는 순간 keyring_put_key가 큐에서 빼고 free.
	 * 보호: mutex. */
};

/* [한국어] 전역 단일 keyring 인스턴스. mutex는 spdk_keyring_init에서 동적 init.
 * 큐들은 정적 init으로 초기화되어 init 호출 전에도 register/find가 가능 (실제로
 * SPDK_KEYRING_REGISTER_MODULE constructor가 init 전에 modules를 채운다). */
static struct spdk_keyring g_keyring = {
	.keys = TAILQ_HEAD_INITIALIZER(g_keyring.keys),
	.removed_keys = TAILQ_HEAD_INITIALIZER(g_keyring.removed_keys),
	.modules = TAILQ_HEAD_INITIALIZER(g_keyring.modules),
};

/*
 * [한국어]
 * keyring_get_key_name - "<keyring>:<key>" 이름에서 <key> 부분만 정규화 추출
 *
 * @name: 사용자가 준 이름. "key0" 또는 ":key0" 또는 "ring0:key0" 형태.
 * @return: 콜론 뒤 부분 또는 콜론이 없으면 원본. 본 파일이 비교 시 양쪽 다
 *          이 함수로 정규화하므로 "key0"과 ":key0"은 동일 키로 취급.
 *
 * 핵심 규약: 현재 SPDK는 전역 keyring 1개만 지원 — keyring prefix가 비어 있는
 *   ":key0"도 사용 가능. multi-keyring 도입 시 첫 번째 strstr이 keyring 이름과
 *   key 이름을 분리하는 데 재사용될 수 있음.
 *
 * 주의: "ring0:k:ey0"처럼 콜론이 두 개면 첫 콜론 뒤 모두를 key 이름으로 취급
 *   ("k:ey0"). 콜론을 키 이름에 쓰려는 상위 레이어는 escape 책임이 있음.
 */
static const char *
keyring_get_key_name(const char *name)
{
	const char *keyname;

	/* Both "key0" and ":key0" refer to "key0" in the global keyring */
	/* [한국어] 첫 ":" 위치 검색 — 없으면 prefix 없는 단순 키 이름 */
	keyname = strstr(name, ":");
	if (keyname == NULL) {
		return name;            /* [한국어] 콜론 없음 → 원본 그대로가 key 이름 */
	}

	return keyname + 1;             /* [한국어] 콜론 직후 문자부터 key 이름 */
}

/*
 * [한국어]
 * keyring_find_key - active keys TAILQ에서 이름으로 키 검색
 *
 * @name: 찾을 키 이름 (정규화 전/후 무관 — 비교 시 정규화).
 * @return: 매칭된 spdk_key 또는 NULL.
 *
 * 동시성: 호출자가 g_keyring.mutex를 잡고 있어야 함. 락 없이 호출하면 경쟁.
 * 호출 체인: spdk_keyring_get_key, _add_key, _remove_key, keyring_probe_key.
 */
static struct spdk_key *
keyring_find_key(const char *name)
{
	struct spdk_key *key;

	/* [한국어] active keys만 검색 — removed 상태 키는 새 lookup 대상이 아님 */
	TAILQ_FOREACH(key, &g_keyring.keys, tailq) {
		/* [한국어] 양쪽 이름을 정규화하여 비교 → ":key0"과 "key0" 동일 취급 */
		if (strcmp(keyring_get_key_name(key->name),
			   keyring_get_key_name(name)) == 0) {
			return key;
		}
	}

	return NULL;             /* [한국어] 매칭 없음 — 호출자가 probe로 추가 시도할 수도 있음 */
}

/*
 * [한국어]
 * keyring_free_key - 키 객체 메모리 해제 (refcnt가 0일 때만 호출 가능)
 *
 * 보안 메모: 모듈 ctx 영역(키 데이터)은 본 함수가 zeroize하지 않는다 — 모듈이
 *   remove_key 콜백 내부에서 자체 zeroize를 수행해야 한다. 실제 file 모듈
 *   구현을 보면 spdk_memset_s로 비밀 영역을 0으로 채우는 것을 확인 가능.
 */
static void
keyring_free_key(struct spdk_key *key)
{
	assert(key->refcnt == 0);     /* [한국어] use-after-free 방지: 살아있는 ref가 있으면 절대 free 금지 */

	free(key->name);              /* [한국어] strdup된 이름 해제 */
	free(key);                    /* [한국어] 객체 자체(+ 모듈 ctx 연접 영역) 해제 */
}

/*
 * [한국어]
 * keyring_put_key - 키 refcnt 감소 + 0이 되면 free
 *
 * @key: refcnt를 감소시킬 키. NULL 불가.
 * @return: 감소 후의 refcnt (0 → 0 반환). 호출자가 추가 정리(자동 unload 등)를
 *          할 때 분기 기준으로 사용.
 *
 * 동작: refcnt가 0이 되면 key는 반드시 removed 상태여야 한다 (active 상태에서
 *   refcnt 0은 이론적으로 불가 — add_key가 1로 시작하기 때문).
 *
 * 동시성: 호출자가 g_keyring.mutex를 잡고 호출.
 */
static int
keyring_put_key(struct spdk_key *key)
{
	assert(key->refcnt > 0);     /* [한국어] underflow 방지 — 0인 키에 put이 오면 버그 */
	key->refcnt--;

	if (key->refcnt == 0) {
		assert(key->removed);                       /* [한국어] 0 ref는 removed_keys 큐에만 있어야 함 */
		TAILQ_REMOVE(&g_keyring.removed_keys, key, tailq);
		keyring_free_key(key);                      /* [한국어] 큐에서 분리 후 해제 */

		return 0;
	}

	return key->refcnt;          /* [한국어] 아직 ref 남아있음 → 호출자가 자동 unload 등 추가 처리 가능 */
}

/*
 * [한국어]
 * spdk_keyring_add_key - 새 키를 keyring에 등록 (공개 API)
 *
 * @opts: spdk_key_opts (spdk/keyring_module.h 정의):
 *        - name: 키 식별자 ("<keyring>:<key>" 또는 "<key>"). NULL 불가.
 *        - module: 키를 보관할 백엔드 모듈 포인터. NULL 불가.
 *        - ctx: 모듈별 add_key가 해석할 추가 인자(예: file 모듈은 path/raw bytes).
 *
 * @return: 0 성공, -EINVAL(잘못된 prefix), -EEXIST(같은 이름 존재),
 *          -ENOMEM(할당 실패), 기타 module->add_key가 반환한 음수 errno.
 *
 * 왜 필요한가: 운영자가 keyring_file_add_key/keyring_linux_add_key 같은 RPC를
 *   호출하면 해당 모듈이 본 API로 키를 등록한다. 키 데이터를 보관하는 책임은
 *   모듈에 있고 본 함수는 키 메타데이터만 관리.
 *
 * 동작:
 *   1) name prefix 검증 — ":foo"(빈 keyring prefix)는 OK, "ring:foo"는 -EINVAL
 *      (multi-keyring 미지원).
 *   2) g_keyring.mutex 잡고 중복 검사.
 *   3) calloc(sizeof(*key) + module->get_ctx_size()) — 키 헤더 + 모듈별 ctx 한꺼번.
 *   4) name strdup.
 *   5) module->add_key(key, opts->ctx) — 모듈이 비밀 데이터를 ctx 영역에 보관.
 *   6) module/refcnt=1 설정 후 keys TAILQ INSERT_TAIL.
 *
 * 에러 경로: 락 안에서 실패하면 부분 초기화된 key를 락 밖에서 free. 락 밖에서
 *   free하는 이유는 free 자체가 mutex 보호 자료구조를 건드리지 않기 때문 +
 *   락 holding time을 짧게 유지.
 *
 * 호출 체인: keyring_file/linux RPC 핸들러 → [본 함수] → module->add_key.
 */
int
spdk_keyring_add_key(const struct spdk_key_opts *opts)
{
	struct spdk_key *key = NULL;
	struct spdk_keyring_module *module = opts->module;
	const char *keyname;
	int rc = 0;

	/* For now, only global keyring is supported */
	/* [한국어] 첫 ':' 위치 확인 — 위치가 첫 글자가 아니면 비어있지 않은 keyring
	 * prefix가 있다는 뜻 → 현재는 미지원이라 -EINVAL */
	keyname = strstr(opts->name, ":");
	if (keyname != NULL && keyname != opts->name) {
		SPDK_ERRLOG("Couldn't add key '%s' to the keyring: keyring doesn't exist\n",
			    opts->name);
		return -EINVAL;
	}

	pthread_mutex_lock(&g_keyring.mutex);
	if (keyring_find_key(opts->name) != NULL) {   /* [한국어] 같은 이름이 이미 active로 존재하면 거부 */
		SPDK_ERRLOG("Key '%s' already exists\n", opts->name);
		rc = -EEXIST;
		goto out;
	}

	/* [한국어] 키 헤더 + 모듈별 ctx 영역(예: file 모듈의 raw key bytes 보관소)을
	 * 단일 calloc으로 할당 → spdk_key_get_ctx(key) == key+1 캐스팅으로 접근 */
	key = calloc(1, sizeof(*key) + module->get_ctx_size());
	if (key == NULL) {
		rc = -ENOMEM;
		goto out;
	}

	key->name = strdup(opts->name);          /* [한국어] 이름 사본 보관 — 호출자 buffer에 의존하지 않기 위해 */
	if (key->name == NULL) {
		rc = -ENOMEM;
		goto out;
	}

	/* [한국어] 모듈에 키 데이터 보관 위임. 모듈은 key+1 ctx 영역에 비밀 데이터를 넣음.
	 * 실패 시 모듈이 부분 초기화된 ctx를 자체 정리해야 함. */
	rc = module->add_key(key, opts->ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to add key '%s' to the keyring\n", opts->name);
		goto out;
	}

	key->module = module;                    /* [한국어] 백엔드 모듈 핸들 저장 */
	key->refcnt = 1;                         /* [한국어] add 시 refcnt 1 — keyring 자체가 ref 1을 보유 */
	TAILQ_INSERT_TAIL(&g_keyring.keys, key, tailq);   /* [한국어] active 큐 끝에 삽입 */
out:
	pthread_mutex_unlock(&g_keyring.mutex);
	if (rc != 0 && key != NULL) {
		/* [한국어] 락 풀린 뒤 정리 — 부분 초기화 객체에는 큐 링크가 없으므로 mutex 불필요 */
		keyring_free_key(key);
	}

	return rc;
}

/*
 * [한국어]
 * keyring_remove_key - 활성 키를 백엔드에서 제거하고 removed_keys 큐로 이동 (내부)
 *
 * @key: 제거 대상. 호출자가 mutex를 잡고 있어야 하며 !removed여야 함.
 *
 * 동작:
 *   1) removed=true 마크 (이후 spdk_key_get_key는 -ENOKEY).
 *   2) module->remove_key — 백엔드에서 비밀 데이터 zeroize/unload.
 *   3) keys → removed_keys 이동 (refcnt가 남아있으면 살아남고, 0이 되면 free).
 *   4) keyring 자체가 들고 있던 ref를 put — refcnt가 0이면 즉시 free되고
 *      removed_keys 큐에서도 빠진다.
 *
 * 호출 체인: spdk_keyring_remove_key / put_key의 자동 unload / cleanup.
 */
static void
keyring_remove_key(struct spdk_key *key)
{
	assert(!key->removed);              /* [한국어] 이중 remove 방지 */
	key->removed = true;                /* [한국어] 새 get_key 시 매칭에서 제외되도록 표시 */
	key->module->remove_key(key);       /* [한국어] 백엔드 zeroize/unload — 모듈 책임 */
	TAILQ_REMOVE(&g_keyring.keys, key, tailq);
	TAILQ_INSERT_TAIL(&g_keyring.removed_keys, key, tailq);
	keyring_put_key(key);               /* [한국어] keyring이 보유하던 ref 1 해제 — 잔여 ref 0이면 free */
}

/*
 * [한국어]
 * spdk_keyring_remove_key - 모듈 소유자만 자기 키를 제거할 수 있는 공개 API
 *
 * @name: 제거할 키 이름.
 * @module: 호출자가 자기 모듈 포인터를 검증용으로 전달. 키의 owner 모듈과
 *          다르면 -EINVAL — cross-module 제거 방지 (보안 격리).
 *
 * @return: 0 성공, -ENOKEY(이름 없음), -EINVAL(다른 모듈 소유).
 */
int
spdk_keyring_remove_key(const char *name, struct spdk_keyring_module *module)
{
	struct spdk_key *key;
	int rc = 0;

	pthread_mutex_lock(&g_keyring.mutex);
	key = keyring_find_key(name);
	if (key == NULL) {
		SPDK_ERRLOG("Key '%s' does not exist\n", name);
		rc = -ENOKEY;
		goto out;
	}

	if (key->module != module) {       /* [한국어] 소유자만 제거 가능 — file 모듈이 linux 모듈 키 못 지움 */
		SPDK_ERRLOG("Key '%s' is not owned by module '%s'\n", name, module->name);
		rc = -EINVAL;
		goto out;
	}

	keyring_remove_key(key);
out:
	pthread_mutex_unlock(&g_keyring.mutex);
	return rc;
}

/*
 * [한국어]
 * keyring_probe_key - 등록된 모듈들에게 backing store에서 키를 lazy import 시도
 *
 * @name: 찾을 키 이름.
 * @return: 성공적으로 import된 키 또는 NULL (어느 모듈도 못 찾음).
 *
 * 시나리오: 사용자가 spdk_keyring_get_key("nvmf-tls-key1")을 호출했는데 in-memory
 *   keyring에 없음. 그럼 본 함수가 모듈들에게 "이 이름의 키를 찾을 수 있냐?"
 *   순서대로 묻는다. 모듈 중 하나(예: Linux kernel keyring 모듈)가 자기
 *   backing store에서 발견하면 spdk_keyring_add_key를 통해 keyring에 삽입한 뒤
 *   0을 반환. 본 함수는 그 직후 keyring_find_key로 동일 이름을 다시 찾아 키
 *   포인터를 얻는다.
 *
 * 모듈 반환값 의미:
 *   - 0: 성공적으로 import. find_key로 객체 포인터 획득.
 *   - -ENOKEY: 이 모듈은 이 키를 모름 — 다음 모듈에 시도.
 *   - 기타 음수: 알지만 instantiate 실패 — 그 자리에서 break (다음 모듈에 시도
 *     하지 않음). 다른 모듈이 같은 이름을 다르게 해석할 수 있는데도 break하는
 *     이유는 "이 이름은 이 모듈 거다"라는 명시적 실패 의미를 보존하기 위함.
 *
 * 동시성: 호출자(spdk_keyring_get_key)가 g_keyring.mutex를 잡고 있어야 함.
 *   probe_key 콜백 안에서 모듈이 spdk_keyring_add_key를 호출하므로 recursive
 *   mutex(spdk_keyring_init에서 PTHREAD_MUTEX_RECURSIVE 설정)가 필수.
 */
static struct spdk_key *
keyring_probe_key(const char *name)
{
	struct spdk_keyring_module *module;
	struct spdk_key *key = NULL;
	int rc;

	TAILQ_FOREACH(module, &g_keyring.modules, tailq) {
		if (module->probe_key == NULL) {       /* [한국어] probe 미지원 모듈은 건너뜀 */
			continue;
		}

		rc = module->probe_key(name);          /* [한국어] 모듈 backing store 검색 + add_key 자동 호출 */
		if (rc == 0) {
			key = keyring_find_key(name);  /* [한국어] add_key 후 다시 find — 객체 포인터 회수 */
			if (key == NULL) {
				/* [한국어] 모듈 버그 — probe success 반환했는데 add를 안 한 경우 */
				SPDK_ERRLOG("Successfully probed key '%s' using module '%s', but "
					    "the key is unavailable\n", name, module->name);
				return NULL;
			}

			key->probed = true;            /* [한국어] auto-unload 대상으로 표시 */
			break;
		} else if (rc != -ENOKEY) {
			/* The module is aware of the key but couldn't instantiate it */
			/* [한국어] -ENOKEY 외 에러는 "이 모듈이 키를 알지만 못 만듦" → 다른 모듈 시도 안 함 */
			assert(keyring_find_key(name) == NULL);
			SPDK_ERRLOG("Failed to probe key '%s' using module '%s': %s\n",
				    name, module->name, spdk_strerror(-rc));
			break;
		}
		/* [한국어] -ENOKEY → 다음 모듈로 계속 */
	}

	return key;
}

/*
 * [한국어]
 * spdk_keyring_get_key - 이름으로 키 핸들을 얻고 refcnt를 +1 (공개 API)
 *
 * @name: 찾을 키 이름.
 * @return: 키 핸들 (refcnt 증가됨, 사용 후 spdk_keyring_put_key로 반환 필수)
 *          또는 NULL (없거나 probe 실패).
 *
 * 사용처: nvmf TLS handshake 직전, bdev_crypto open, NVMe DH-CHAP auth 등에서
 *   비밀 키가 필요할 때 호출. 받은 핸들로 spdk_key_get_key(buf, len)을 호출해
 *   실제 byte를 꺼낸다.
 *
 * 호출 체인: 소비자 → [본 함수] → keyring_find_key 또는 keyring_probe_key.
 */
struct spdk_key *
spdk_keyring_get_key(const char *name)
{
	struct spdk_key *key;

	pthread_mutex_lock(&g_keyring.mutex);
	key = keyring_find_key(name);
	if (key == NULL) {
		key = keyring_probe_key(name);   /* [한국어] in-memory에 없으면 backing store에서 lazy import */
		if (key == NULL) {
			goto out;                /* [한국어] 모든 모듈이 모름 → NULL 반환 */
		}
	}

	key->refcnt++;                            /* [한국어] 호출자 ref 추가 — put_key로 반환 책임 */
out:
	pthread_mutex_unlock(&g_keyring.mutex);

	return key;
}

/*
 * [한국어]
 * spdk_keyring_put_key - get_key/key_dup으로 얻은 핸들 반환
 *
 * @key: NULL 허용 (no-op) — 호출자 코드 단순화.
 *
 * 자동 unload 규칙:
 *   probed 키이고 본 put이 keyring 자신만 ref를 들고 있는 상태로 만들면
 *   (refcnt==1) 자동으로 keyring_remove_key 트리거 → backing store에서 unload.
 *   이는 lazy import의 짝 — 사용 끝나면 자동 정리.
 *
 * 명시적 add된 키(probed=false)는 본 put으로 unload되지 않고 명시 remove를
 *   기다린다 — 운영자가 add한 키는 운영자가 관리.
 */
void
spdk_keyring_put_key(struct spdk_key *key)
{
	int refcnt;

	if (key == NULL) {                       /* [한국어] NULL은 silently 처리 */
		return;
	}

	pthread_mutex_lock(&g_keyring.mutex);
	refcnt = keyring_put_key(key);
	/* [한국어] put 후 남은 ref가 1(=keyring 자체 ref)이고 probed 자동 import이며
	 * 아직 removed가 아니면, 마지막 소비자가 떠난 것이므로 자동 unload */
	if (refcnt == 1 && key->probed && !key->removed) {
		keyring_remove_key(key);
	}
	pthread_mutex_unlock(&g_keyring.mutex);
}

/*
 * [한국어]
 * spdk_key_dup - 동일 키에 대한 추가 ref를 발급
 *
 * 사용처: 키를 다른 컨텍스트로 복사 전달할 때 (예: bdev open 시 internal 보관용).
 *   put_key를 호출 횟수만큼 짝지어 호출해야 함.
 */
struct spdk_key *
spdk_key_dup(struct spdk_key *key)
{
	pthread_mutex_lock(&g_keyring.mutex);
	key->refcnt++;                           /* [한국어] ref만 추가 — 객체는 동일 */
	pthread_mutex_unlock(&g_keyring.mutex);

	return key;
}

/*
 * [한국어]
 * spdk_key_get_name - 키 이름 문자열 반환 (호출자 free 금지)
 */
const char *
spdk_key_get_name(struct spdk_key *key)
{
	return key->name;       /* [한국어] strdup된 내부 사본 — read-only로 사용 */
}

/*
 * [한국어]
 * spdk_key_get_key - 실제 비밀 byte를 호출자 버퍼로 꺼냄
 *
 * @key: 대상 키. removed면 -ENOKEY.
 * @buf: 출력 버퍼. 호출자 책임으로 충분히 커야 함.
 * @len: buf 크기 (바이트).
 *
 * @return: 실제 복사된 길이(바이트) 또는 음수 errno. 모듈이 자체 정책으로
 *          (예: 길이 부족 시 -ENOBUFS) 결정한다.
 *
 * 보안: 호출자는 buf를 사용 후 메모리 zeroize 책임 — keyring은 buf 관리하지 않음.
 *
 * 호출 체인: TLS handshake → [본 함수] → module->get_key (file 모듈은 raw bytes 복사,
 *   linux 모듈은 keyctl read 등).
 */
int
spdk_key_get_key(struct spdk_key *key, void *buf, int len)
{
	struct spdk_keyring_module *module = key->module;

	if (key->removed) {            /* [한국어] 이미 unload된 키는 데이터 노출 금지 */
		return -ENOKEY;
	}

	return module->get_key(key, buf, len);   /* [한국어] 모듈에 위임 — 본 파일은 byte를 보지 않음 */
}

/*
 * [한국어]
 * spdk_key_get_ctx - 키 객체 뒤에 연접 할당된 모듈별 ctx 영역 포인터 반환
 *
 * 모듈 코드가 자기 add_key/get_key 내부에서 spdk_key 핸들로부터 자기 ctx 구조체를
 * 꺼낼 때 사용. key+1 캐스팅 패턴 — sizeof(struct spdk_key) 만큼 점프 후 모듈 ctx.
 */
void *
spdk_key_get_ctx(struct spdk_key *key)
{
	return key + 1;    /* [한국어] 헤더 다음 바이트 = 모듈 ctx 시작 (calloc 시 함께 잡혔음) */
}


/*
 * [한국어]
 * spdk_key_get_module - 키 owner 모듈 포인터 반환
 *
 * 사용처: 외부 코드가 키 모듈별 분기를 할 때 (예: 키가 file 모듈인지 kernel
 *   keyring 모듈인지에 따라 다른 처리).
 */
struct spdk_keyring_module *
spdk_key_get_module(struct spdk_key *key)
{
	return key->module;
}

/*
 * [한국어]
 * spdk_keyring_write_config - SPDK config dump 시 모듈별 write_config 디스패치
 *
 * @w: JSON writer (config save 경로의 빌더).
 *
 * 사용처: save_config RPC가 호출되면 SPDK 전체 상태를 JSON으로 덤프하는데, 이때
 *   keyring 서브시스템이 자기 모듈들에게 "config 항목을 출력해라" 명령. 각
 *   모듈은 자기 add_key 호출 reproducible하게 만들 수 있는 정보를 출력 (단,
 *   비밀 데이터 자체는 보안상 출력하지 않을 수도 있음 — 모듈 정책).
 */
void
spdk_keyring_write_config(struct spdk_json_write_ctx *w)
{
	struct spdk_keyring_module *module;

	TAILQ_FOREACH(module, &g_keyring.modules, tailq) {
		if (module->write_config != NULL) {     /* [한국어] write_config 미지원 모듈은 건너뜀 */
			module->write_config(w);
		}
	}
}

/*
 * [한국어]
 * spdk_keyring_for_each_key - 등록된 모든(또는 active만) 키에 대해 콜백 호출
 *
 * @keyring: 현재는 NULL만 허용 (multi-keyring 미지원). assert로 강제.
 * @ctx: fn에 그대로 전달될 불투명 포인터.
 * @fn: per-key 콜백.
 * @flags: SPDK_KEYRING_FOR_EACH_ALL 비트 set이면 removed_keys도 순회.
 *
 * 사용처: keyring_get_keys RPC, save_config 등.
 *
 * 동시성: g_keyring.mutex를 잡고 fn을 호출 — 콜백 안에서 keyring API를 다시
 *   호출해도 recursive mutex라 데드락은 없지만, list 변경은 TAILQ_FOREACH_SAFE
 *   덕분에 현재 원소 제거에 한해 안전 (다른 위치 제거 시 race 가능성 주의).
 */
void
spdk_keyring_for_each_key(struct spdk_keyring *keyring,
			  void *ctx, void (*fn)(void *ctx, struct spdk_key *key), uint32_t flags)
{
	struct spdk_key *key, *tmp;

	assert(keyring == NULL);                          /* [한국어] 전역 keyring 지정 강제 */
	pthread_mutex_lock(&g_keyring.mutex);
	/* [한국어] FOREACH_SAFE: 콜백이 현재 키를 remove해도 tmp 덕분에 다음 원소로 안전 진행 */
	TAILQ_FOREACH_SAFE(key, &g_keyring.keys, tailq, tmp) {
		fn(ctx, key);
	}

	if (flags & SPDK_KEYRING_FOR_EACH_ALL) {           /* [한국어] removed_keys까지 노출 (RPC dump용) */
		TAILQ_FOREACH_SAFE(key, &g_keyring.removed_keys, tailq, tmp) {
			fn(ctx, key);
		}
	}
	pthread_mutex_unlock(&g_keyring.mutex);
}

/*
 * [한국어]
 * spdk_keyring_register_module - keyring 백엔드 모듈을 전역 등록부에 추가
 *
 * SPDK_KEYRING_REGISTER_MODULE 매크로(constructor)에서 호출되며 main() 전에
 * 실행된다. 따라서 락 없이 INSERT_TAIL해도 single-thread 보장 — 단, keyring_init
 * 이후 동적 등록은 race 위험이 있으니 권장되지 않음.
 */
void
spdk_keyring_register_module(struct spdk_keyring_module *module)
{
	TAILQ_INSERT_TAIL(&g_keyring.modules, module, tailq);
}

/*
 * [한국어]
 * keyring_dump_key_info - RPC 응답용 키 메타데이터 JSON 직렬화 (internal API)
 *
 * @key: 출력 대상 키 (active/removed 모두).
 * @w: 호출자가 이미 object_begin한 JSON writer.
 *
 * 표준 필드: name/module/removed/probed/refcnt. removed=false이고 모듈이
 *   dump_info를 정의했으면 모듈별 부가 필드도 추가 (예: file 모듈은 path).
 *
 * 호출 체인: spdk_keyring_for_each_key → rpc_keyring_for_each_key_cb
 *   (keyring_rpc.c) → [본 함수] → module->dump_info.
 */
void
keyring_dump_key_info(struct spdk_key *key, struct spdk_json_write_ctx *w)
{
	struct spdk_keyring_module *module = key->module;

	spdk_json_write_named_string(w, "name", key->name);          /* [한국어] 식별자 */
	spdk_json_write_named_string(w, "module", module->name);     /* [한국어] 백엔드 모듈명 */
	spdk_json_write_named_bool(w, "removed", key->removed);      /* [한국어] 백엔드 unload 여부 */
	spdk_json_write_named_bool(w, "probed", key->probed);        /* [한국어] lazy import 여부 */
	spdk_json_write_named_int32(w, "refcnt", key->refcnt);       /* [한국어] 현재 참조 수 (디버깅용) */

	/* [한국어] active 키만 모듈별 부가 정보 노출 — removed 키는 비밀이 이미
	 * zeroize되었을 가능성 높아 안전상 기본 필드만 dump */
	if (!key->removed && module->dump_info != NULL) {
		module->dump_info(key, w);
	}
}

/*
 * [한국어]
 * spdk_keyring_init - keyring 라이브러리 초기화 (mutex + 모듈 init)
 *
 * @return: 0 성공, 음수 errno (mutex 실패 또는 모듈 init 실패).
 *
 * 동작:
 *   1) PTHREAD_MUTEX_RECURSIVE 속성으로 g_keyring.mutex 초기화. 재귀 허용은
 *      probe_key→add_key 같이 같은 thread에서 락을 다시 잡는 경로 때문에 필수.
 *   2) 등록된 모듈들을 순서대로 init.
 *   3) 모듈이 -ENODEV를 반환하면 "이 환경에는 백엔드가 없음"을 의미하므로 모듈을
 *      등록부에서 제거하고 계속 진행 (예: 컨테이너 안에서 Linux keyring 미사용).
 *   4) 다른 음수 errno면 진행 중단 + 이전 모듈들에 cleanup 호출 (rollback).
 *
 * 호출 시점: spdk_subsystem_init_from_json_config / spdk_app_start 초기화 단계.
 */
int
spdk_keyring_init(void)
{
	struct spdk_keyring_module *module, *tmp;
	pthread_mutexattr_t attr;
	int rc;

	rc = pthread_mutexattr_init(&attr);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize mutex attr\n");
		return -rc;            /* [한국어] pthread는 양수 errno 반환 → 본 API 규약은 음수 → 부호 변환 */
	}

	/* [한국어] RECURSIVE 속성 — 같은 thread가 락을 여러 번 잡아도 데드락 없음.
	 * keyring_probe_key가 모듈 콜백을 호출하고, 모듈이 spdk_keyring_add_key를
	 * 다시 호출하는 경로에서 필수. */
	rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to set mutex attr\n");
		pthread_mutexattr_destroy(&attr);
		return -rc;
	}

	rc = pthread_mutex_init(&g_keyring.mutex, &attr);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize mutex\n");
		pthread_mutexattr_destroy(&attr);
		return -rc;
	}

	pthread_mutexattr_destroy(&attr);    /* [한국어] attr 객체는 mutex_init 후 더 이상 필요 없음 */

	/* [한국어] 모듈 init — FOREACH_SAFE로 도중에 -ENODEV 모듈을 큐에서 제거할 수 있게 함 */
	TAILQ_FOREACH_SAFE(module, &g_keyring.modules, tailq, tmp) {
		if (module->init != NULL) {
			rc = module->init();
			if (rc != 0) {
				if (rc == -ENODEV) {
					/* [한국어] 백엔드 미사용 환경 — 조용히 제외하고 다음 모듈로 */
					SPDK_INFOLOG(keyring, "Skipping module %s\n", module->name);
					TAILQ_REMOVE(&g_keyring.modules, module, tailq);
					rc = 0;
					continue;
				}

				SPDK_ERRLOG("Failed to initialize module %s: %s\n",
					    module->name, spdk_strerror(-rc));
				break;     /* [한국어] 다른 에러는 init 중단 */
			}
		}

		SPDK_INFOLOG(keyring, "Initialized module %s\n", module->name);
	}

	if (rc != 0) {
		/* [한국어] rollback — 실패 시 이미 init된 이전 모듈들을 cleanup */
		TAILQ_FOREACH(tmp, &g_keyring.modules, tailq) {
			if (tmp == module) {        /* [한국어] 실패한 모듈에 도달하면 멈춤 (그 이후는 init 안 됨) */
				break;
			}
			if (tmp->cleanup != NULL) {
				tmp->cleanup();
			}
		}
	}

	return rc;
}

/*
 * [한국어]
 * spdk_keyring_cleanup - keyring 라이브러리 종료 (모든 키 정리 + 모듈 cleanup)
 *
 * 동작:
 *   1) active 키를 모두 keyring_remove_key (백엔드 zeroize → removed_keys 이동).
 *   2) removed_keys에 남은 키들은 ref가 누군가에게 잡혀 있다는 뜻이지만 강제로
 *      refcnt=0으로 만들고 free — leaked ref는 WARNLOG로 알림.
 *   3) 모든 모듈의 cleanup 콜백 호출.
 *
 * 위험: 강제 free는 use-after-free를 유발할 수 있다 — 정상 종료 흐름에서는
 *   소비자들이 먼저 put_key를 다 끝낸 상태여야 안전. WARNLOG는 이 위반을
 *   운영자에게 알리는 진단 신호.
 */
void
spdk_keyring_cleanup(void)
{
	struct spdk_keyring_module *module;
	struct spdk_key *key;

	/* [한국어] active 큐 비울 때까지 첫 원소를 remove — keyring_remove_key가 큐 이동 */
	while (!TAILQ_EMPTY(&g_keyring.keys)) {
		key = TAILQ_FIRST(&g_keyring.keys);
		keyring_remove_key(key);
	}

	/* [한국어] 정상 종료 시 removed_keys는 비어 있어야 정상. 비어있지 않으면 leaked ref */
	while (!TAILQ_EMPTY(&g_keyring.removed_keys)) {
		key = TAILQ_FIRST(&g_keyring.removed_keys);
		SPDK_WARNLOG("Key '%s' still has %d references\n", key->name, key->refcnt);
		key->refcnt = 0;                              /* [한국어] 강제 0 — assert 통과시켜 free 가능하게 */
		TAILQ_REMOVE(&g_keyring.removed_keys, key, tailq);
		keyring_free_key(key);
	}

	/* [한국어] 모든 모듈 cleanup 콜백 호출 — backing store 해제, 자원 정리 등 */
	TAILQ_FOREACH(module, &g_keyring.modules, tailq) {
		if (module->cleanup != NULL) {
			module->cleanup();
		}
	}
}

/* [한국어] SPDK 로그 컴포넌트 등록 — SPDK_INFOLOG(keyring, ...) 등에서 사용 가능
 * 한 컴포넌트 이름 "keyring"을 컴파일 시 등록. 운영자는 --logflag keyring 등으로
 * 본 컴포넌트 로그를 활성화할 수 있다. */
SPDK_LOG_REGISTER_COMPONENT(keyring)
