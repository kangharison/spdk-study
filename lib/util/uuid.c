/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK UUID 유틸리티 (uuid.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 전반에서 사용되는 UUID(Universally Unique Identifier)에 대한
 * 생성/파싱/포맷팅/비교/null-체크 등의 헬퍼를 제공한다. SPDK는 다양한 자원
 * (NVMe NQN UUID, blob ID, lvol/lvolstore ID, bdev UUID 등)을 식별하기 위해
 * RFC 4122 형식의 128-bit UUID를 광범위하게 사용하므로, 외부 라이브러리
 * (Linux의 libuuid 또는 FreeBSD의 uuid)와 SPDK 내부 표현 `struct spdk_uuid`
 * 사이의 얇은 래퍼(thin wrapper) 계층을 제공하는 것이 본 파일의 목적이다.
 * 또한 RFC 4122 §4.3 (Name-based UUID, version 5/SHA-1)을 위한
 * spdk_uuid_generate_sha1을 직접 또는 OpenSSL EVP API로 구현한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 측면에서 이 파일은 거의 모든 상위 SPDK 서브시스템(bdev, blob,
 * lvol, nvmf, nvme 등)에서 호출되는 "잎(leaf) 유틸리티"이다.
 * lib/util/ 내에 위치하며 SPDK 빌드 시 항상 링크된다. 특별한 스레드 컨텍스트
 * 요구가 없는(thread-agnostic) stateless 함수들로 구성되어, reactor 스레드뿐
 * 아니라 RPC/init/cleanup 경로에서도 자유롭게 호출 가능하다. SPDK는 OS별로
 * libuuid(Linux) / native uuid(FreeBSD) 중 하나에 의존하며, 빌드 시
 * SPDK_CONFIG_HAVE_LIBUUID 매크로로 어느 백엔드를 쓸지 결정된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `<uuid/uuid.h>` (libuuid) 또는 `<uuid.h>` (FreeBSD), 그리고 환경에
 *   따라 OpenSSL EVP API(<openssl/evp.h>) — uuid_generate_sha1 미존재 시 대체.
 * - 사용처(피호출): bdev 모듈(bdev_aio/bdev_malloc/...), blobstore, lvol,
 *   nvmf 컨트롤러/네임스페이스 NQN 생성, RPC 핸들러의 UUID 인자 파싱 등.
 * - 데이터 흐름: 사용자/RPC 클라이언트 → 문자열 UUID → spdk_uuid_parse →
 *   `struct spdk_uuid`(16 bytes) → 내부 자료구조의 식별자 필드. 반대로
 *   spdk_uuid_fmt_lower 는 내부 표현을 사람이 읽을 수 있는 RFC 4122 표기
 *   ("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", 36자 + NUL)로 직렬화한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_uuid_parse        : "xxxx-..." 문자열을 16바이트 spdk_uuid 로 파싱.
 * - spdk_uuid_fmt_lower    : spdk_uuid 를 소문자 RFC 4122 문자열로 직렬화.
 * - spdk_uuid_compare      : memcmp 기반 사전식 비교(0 동등, !=0 차이).
 * - spdk_uuid_generate     : RFC 4122 §4.4 무작위 UUID(version 4) 생성.
 * - spdk_uuid_copy         : 16바이트 복사.
 * - spdk_uuid_is_null/set_null : 모두-0 UUID 검사/설정.
 * - spdk_uuid_generate_sha1: 네임스페이스 + 이름으로부터 SHA-1 기반(version 5)
 *                            UUID를 결정적(deterministic)으로 파생 — NVMe NQN
 *                            등에서 동일 입력 → 동일 UUID 보장 용도.
 * 본 파일은 별도 구조체를 정의하지 않으며, `struct spdk_uuid`는 `spdk/uuid.h`
 * 에서 정의된다(내부적으로 16바이트 raw 배열을 union으로 노출).
 */

/* [한국어] 공용 헤더: struct spdk_uuid 정의와 본 파일이 구현해야 할 API 선언. */
#include "spdk/uuid.h"
/* [한국어] 빌드 시 자동 생성되는 SPDK 설정 매크로 헤더.
 * SPDK_CONFIG_HAVE_LIBUUID, SPDK_CONFIG_HAVE_UUID_GENERATE_SHA1 같은 기능
 * 검출 매크로가 정의되어 있어 OS/라이브러리별 분기를 가능하게 한다. */
#include "spdk/config.h"
/* [한국어] SPDK 로깅 매크로(SPDK_ERRLOG 등) — generate_sha1 의 OpenSSL 경로에서
 * 진단 메시지를 출력하기 위해 필요. */
#include "spdk/log.h"

/* [한국어] libuuid 의 uuid_generate_sha1 이 빌드 환경에 없을 경우(예: 오래된
 * libuuid 또는 FreeBSD), OpenSSL EVP API 로 SHA-1 다이제스트를 직접 계산해
 * RFC 4122 §4.3 v5 UUID 를 만들기 위해 OpenSSL EVP 헤더를 포함한다. */
#ifndef SPDK_CONFIG_HAVE_UUID_GENERATE_SHA1
#include <openssl/evp.h>
#endif /* SPDK_CONFIG_HAVE_UUID_GENERATE_SHA1 */

/* [한국어] ── Linux/libuuid 백엔드 분기 시작 ──
 * Linux 진영에서 사실상 표준인 libuuid(util-linux 제공)에 위임한다. */
#if defined(SPDK_CONFIG_HAVE_LIBUUID)

/* [한국어] libuuid 의 uuid_t (실제로는 unsigned char[16]) 를 사용하기 위한 헤더. */
#include <uuid/uuid.h>

/* [한국어] 컴파일 타임 단언:
 *  struct spdk_uuid 의 크기가 libuuid의 uuid_t(16 바이트)와 정확히 같아야 함.
 *  → spdk_uuid* 를 그대로 uuid_t* 로 캐스팅해서 libuuid 함수에 넘길 수 있음을 보장. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_uuid) == sizeof(uuid_t), "Size mismatch");

/*
 * [한국어]
 * spdk_uuid_parse - RFC 4122 표기의 UUID 문자열을 16바이트 바이너리로 변환.
 *
 * @uuid:     출력. 파싱 결과를 채울 spdk_uuid 구조체(16바이트).
 * @uuid_str: 입력. 8-4-4-4-12 자리 hex + 하이픈 형식의 NUL 종료 문자열.
 * @return:   0 성공, -EINVAL 형식 오류.
 *
 * 호출자는 RPC 핸들러나 CLI 인자 파서가 일반적이며, 사용자가 입력한 문자열을
 * 내부 식별자로 변환할 때 사용된다. libuuid 의 uuid_parse 는 성공 시 0,
 * 실패 시 음수를 반환하므로 SPDK 관례에 맞게 -EINVAL 로 정규화한다.
 *
 * 호출 체인: RPC/CLI → spdk_uuid_parse → uuid_parse(libuuid)
 */
int
spdk_uuid_parse(struct spdk_uuid *uuid, const char *uuid_str)
{
	/* [한국어] libuuid 에 위임. (void *) 캐스팅은 size_assert 로 호환성이
	 * 보장되어 안전하다. uuid_parse == 0 → 성공, 그 외 → 형식 오류로 -EINVAL. */
	return uuid_parse(uuid_str, (void *)uuid) == 0 ? 0 : -EINVAL;
}

/*
 * [한국어]
 * spdk_uuid_fmt_lower - 16바이트 UUID를 소문자 RFC 4122 표기로 직렬화.
 *
 * @uuid_str:      출력 버퍼. 최소 SPDK_UUID_STRING_LEN(=37: 36자 + NUL) 크기 필요.
 * @uuid_str_size: 호출자가 제공한 버퍼 크기.
 * @uuid:         입력. 직렬화 대상 UUID.
 * @return: 0 성공, -EINVAL 버퍼가 너무 작음.
 *
 * RPC 응답이나 로그 출력에서 사람이 읽을 수 있는 형태로 변환할 때 사용된다.
 * libuuid 의 uuid_unparse_lower 는 항상 SPDK_UUID_STRING_LEN 만큼 쓰므로
 * 사전 크기 체크가 필요하다.
 */
int
spdk_uuid_fmt_lower(char *uuid_str, size_t uuid_str_size, const struct spdk_uuid *uuid)
{
	/* [한국어] 출력 버퍼가 36자 본문 + NUL 까지 담을 수 없으면 즉시 거절. */
	if (uuid_str_size < SPDK_UUID_STRING_LEN) {
		return -EINVAL;
	}

	/* [한국어] libuuid 의 소문자 unparse — "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
	 * 형태로 정확히 37바이트(NUL 포함)를 기록한다. */
	uuid_unparse_lower((void *)uuid, uuid_str);
	return 0;
}

/*
 * [한국어]
 * spdk_uuid_compare - 두 UUID 의 사전식 비교.
 *
 * @u1, @u2: 비교 대상.
 * @return:  0 동일, 음수 u1<u2, 양수 u1>u2 (memcmp 의미와 동일).
 *
 * lvol/blob 검색이나 정렬된 컨테이너에서의 키 비교에 사용된다. 내부적으로
 * libuuid 가 단순 memcmp 를 호출하므로 lock-free 하고 부작용이 없다.
 */
int
spdk_uuid_compare(const struct spdk_uuid *u1, const struct spdk_uuid *u2)
{
	/* [한국어] libuuid 의 byte-wise 비교(=memcmp)에 위임. */
	return uuid_compare((void *)u1, (void *)u2);
}

/*
 * [한국어]
 * spdk_uuid_generate - RFC 4122 §4.4 무작위(version 4) UUID 생성.
 *
 * @uuid: 출력. 16바이트 무작위 UUID 가 채워짐.
 *
 * 새 bdev/blob/lvol 등 자원이 생성될 때 자동 ID 할당 용도로 사용된다.
 * libuuid 는 가능하면 /dev/urandom 또는 getrandom() 시스템 호출을 통해
 * CSPRNG 엔트로피를 사용하므로 충돌 확률은 사실상 무시 가능하다.
 */
void
spdk_uuid_generate(struct spdk_uuid *uuid)
{
	/* [한국어] libuuid 위임 — version/variant 비트는 라이브러리가 직접 세팅. */
	uuid_generate((void *)uuid);
}

/*
 * [한국어]
 * spdk_uuid_copy - UUID 16바이트를 그대로 복사.
 *
 * @dst: 출력. @src 의 내용으로 덮어쓰여짐.
 * @src: 입력. 보존됨.
 *
 * 단순 byte-wise 복제 함수. memcpy 와 동등하며, 자료구조 복제 시
 * 의도를 명확히 드러내기 위한 의미론적 래퍼이다.
 */
void
spdk_uuid_copy(struct spdk_uuid *dst, const struct spdk_uuid *src)
{
	/* [한국어] libuuid 의 uuid_copy 는 내부적으로 memcpy(dst, src, 16). */
	uuid_copy((void *)dst, (void *)src);
}

/*
 * [한국어]
 * spdk_uuid_is_null - UUID 가 모두-0 (null UUID) 인지 검사.
 *
 * @uuid:   검사 대상.
 * @return: true 면 모두-0, false 면 적어도 한 바이트가 non-zero.
 *
 * "UUID 미할당" 또는 "초기화되지 않음" 의미로 0-UUID 를 사용하는 코드 경로
 * (예: bdev 의 optional uuid 필드)에서 분기 조건으로 자주 호출된다.
 */
bool
spdk_uuid_is_null(const struct spdk_uuid *uuid)
{
	/* [한국어] libuuid 의 uuid_is_null — 16바이트 모두 0 인지 검사. */
	return uuid_is_null((void *)uuid);
}

/*
 * [한국어]
 * spdk_uuid_set_null - UUID 를 모두-0 (null UUID) 으로 초기화.
 *
 * @uuid: 출력. 16바이트가 모두 0 으로 채워짐.
 *
 * "아직 할당되지 않음" 마커로서 자료구조를 초기화할 때 사용한다.
 */
void
spdk_uuid_set_null(struct spdk_uuid *uuid)
{
	/* [한국어] libuuid 의 uuid_clear — 내부적으로 memset(uuid, 0, 16). */
	uuid_clear((void *)uuid);
}

/* [한국어] ── Linux/libuuid 분기 끝, FreeBSD 분기 시작 ── */
#elif defined(__FreeBSD__)

/* [한국어] FreeBSD 의 native uuid(3) API 헤더 — uuid_t, uuid_from_string 등. */
#include <uuid.h>

/* [한국어] FreeBSD uuid_t 또한 16바이트 — spdk_uuid 와 호환되는지 컴파일 시 확인. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_uuid) == sizeof(uuid_t), "Size mismatch");

/*
 * [한국어]
 * spdk_uuid_parse (FreeBSD) - 문자열 → 16바이트 UUID. Linux 버전과 의미 동일.
 *
 * @uuid:     출력.
 * @uuid_str: 입력 NUL 종료 문자열.
 * @return:   0 성공, -EINVAL 실패.
 *
 * 주의: FreeBSD uuid_from_string 은 빈 문자열을 "성공 + null UUID" 로 처리하므로
 * Linux libuuid (이때 빈 문자열은 오류) 와 동작을 통일하기 위해 빈 문자열을
 * 명시적으로 거절한다.
 */
int
spdk_uuid_parse(struct spdk_uuid *uuid, const char *uuid_str)
{
	/* [한국어] uuid_from_string 의 상태 코드를 받기 위한 변수 — 0 이면 성공. */
	uint32_t status;

	/* uuid_from_string() differs from uuid_parse() in the way it handles empty strings: the
	 * former succeeds and returns a NULL UUID, while the latter treats is an error and returns
	 * non-zero exit code.  So, to keep the behavior consistent between Linux and FreeBSD, we
	 * explicitly check for an empty string here.
	 */
	/* [한국어] 빈 문자열은 OS 간 일관성을 위해 무조건 -EINVAL 로 처리. */
	if (strlen(uuid_str) == 0) {
		return -EINVAL;
	}

	/* [한국어] FreeBSD native 파서 호출 — 결과는 uuid 에, 상태는 status 에. */
	uuid_from_string(uuid_str, (uuid_t *)uuid, &status);

	/* [한국어] uuid_s_ok(=0) 만 성공으로 인정. */
	return status == 0 ? 0 : -EINVAL;
}

/*
 * [한국어]
 * spdk_uuid_fmt_lower (FreeBSD) - 16바이트 UUID → 소문자 문자열.
 *
 * FreeBSD uuid_to_string 은 동적 할당 후 포인터를 반환하므로, 호출 후
 * 사용자 버퍼에 복사한 뒤 free 해야 한다(Linux libuuid 와 다름).
 */
int
spdk_uuid_fmt_lower(char *uuid_str, size_t uuid_str_size, const struct spdk_uuid *uuid)
{
	/* [한국어] uuid_to_string 의 상태 코드. uuid_s_no_memory 인 경우 -ENOMEM. */
	uint32_t status;
	/* [한국어] FreeBSD가 malloc 으로 할당한 결과 문자열 포인터(소유권 호출자에게). */
	char *str;

	/* [한국어] 사전 크기 검증 — Linux 분기와 동일한 의미. */
	if (uuid_str_size < SPDK_UUID_STRING_LEN) {
		return -EINVAL;
	}

	/* [한국어] FreeBSD 직렬화 — str 은 malloc 결과, 사용 후 반드시 free. */
	uuid_to_string((const uuid_t *)uuid, &str, &status);

	/* [한국어] 메모리 부족이면 status 로 분기 — 별도 errno 매핑. */
	if (status == uuid_s_no_memory) {
		return -ENOMEM;
	}

	/* [한국어] 사용자 버퍼로 복사 — 너무 길어도 잘리지 않게 사전 크기 체크가 보장. */
	snprintf(uuid_str, uuid_str_size, "%s", str);
	/* [한국어] FreeBSD가 할당한 버퍼는 호출자가 free 해 줘야 메모리 누수 없음. */
	free(str);

	return 0;
}

/*
 * [한국어]
 * spdk_uuid_compare (FreeBSD) - byte-wise 비교. status 는 사용 안 함.
 */
int
spdk_uuid_compare(const struct spdk_uuid *u1, const struct spdk_uuid *u2)
{
	/* [한국어] FreeBSD uuid_compare 의 세 번째 인자는 (uint32_t *)status (옵션). */
	return uuid_compare((const uuid_t *)u1, (const uuid_t *)u2, NULL);
}

/*
 * [한국어]
 * spdk_uuid_generate (FreeBSD) - 무작위 UUID 생성.
 */
void
spdk_uuid_generate(struct spdk_uuid *uuid)
{
	/* [한국어] FreeBSD uuid_create — version/variant 자동 세팅. */
	uuid_create((uuid_t *)uuid, NULL);
}

/*
 * [한국어]
 * spdk_uuid_copy (FreeBSD) - 16바이트 복사. memcpy 로 충분하다.
 */
void
spdk_uuid_copy(struct spdk_uuid *dst, const struct spdk_uuid *src)
{
	/* [한국어] sizeof(*dst) == 16 바이트이므로 단순 memcpy 로 OK. */
	memcpy(dst, src, sizeof(*dst));
}

/*
 * [한국어]
 * spdk_uuid_is_null (FreeBSD) - null UUID 여부 검사.
 */
bool
spdk_uuid_is_null(const struct spdk_uuid *uuid)
{
	/* [한국어] FreeBSD uuid_is_nil — 16바이트가 모두 0 인지 검사. */
	return uuid_is_nil((const uuid_t *)uuid, NULL);
}

/*
 * [한국어]
 * spdk_uuid_set_null (FreeBSD) - null UUID 로 초기화.
 */
void
spdk_uuid_set_null(struct spdk_uuid *uuid)
{
	/* [한국어] FreeBSD uuid_create_nil — 모두 0 으로 채움. */
	uuid_create_nil((uuid_t *)uuid, NULL);
}

#else
/* [한국어] libuuid 도 FreeBSD 도 아닌 경우 → SPDK 가 지원하지 않는 환경. */
#error System must either have libuuid available or be FreeBSD.
#endif

/*
 * [한국어]
 * spdk_uuid_generate_sha1 - RFC 4122 §4.3 (version 5) 이름 기반 UUID 생성.
 *
 * @uuid:    출력. 결과 v5 UUID 가 채워짐.
 * @ns_uuid: 입력. 네임스페이스 UUID(예: NVMe 표준 namespace).
 * @name:    입력. 임의 길이의 바이트 시퀀스(이름).
 * @len:     name 의 길이(바이트).
 * @return:  0 성공, -EINVAL/-ENOMEM 실패.
 *
 * 동일한 (ns_uuid, name) 조합은 항상 동일한 UUID 를 산출하므로 결정적
 * (deterministic) ID 가 필요한 경우(예: NVMe NQN 에서 모델/시리얼 기반 UUID,
 * 분산 시스템에서의 안정 식별자)에 사용된다.
 *
 * 알고리즘: SHA-1(ns_uuid || name) → 앞 16바이트 → version=5(상위 4비트 0101),
 * variant=10(상위 2비트 1 0) 비트 마스킹 후 반환.
 *
 * 경로 1 (libuuid 가 uuid_generate_sha1 제공): 라이브러리에 그대로 위임.
 * 경로 2 (없을 경우): OpenSSL EVP API 로 직접 SHA-1 계산 후 비트 마스킹.
 */
int
spdk_uuid_generate_sha1(struct spdk_uuid *uuid, struct spdk_uuid *ns_uuid, const char *name,
			size_t len)
{
#ifdef SPDK_CONFIG_HAVE_UUID_GENERATE_SHA1
	/* [한국어] 시스템 libuuid 가 직접 v5 생성 함수를 제공하면 그대로 사용. */
	uuid_generate_sha1((void *)uuid, (void *)ns_uuid, name, len);
	return 0;
#else
	/* [한국어] 대체 경로: OpenSSL EVP 로 SHA-1 다이제스트 계산. */

	/* [한국어] EVP message digest 컨텍스트 — DigestInit/Update/Final 동안 상태 보존. */
	EVP_MD_CTX *mdctx;
	/* [한국어] 사용할 다이제스트 알고리즘 디스크립터 (SHA-1). */
	const EVP_MD *md;
	/* [한국어] 다이제스트 결과 저장 — SHA-1은 20바이트지만 EVP_MAX_MD_SIZE 로 안전 확보. */
	unsigned char md_value[EVP_MAX_MD_SIZE];
	/* [한국어] 실제로 출력된 다이제스트 길이(바이트) — SHA-1이면 20. */
	unsigned int md_len;

	/* [한국어] SHA-1 알고리즘 디스크립터 획득 — OpenSSL 정적 자료라 free 불필요. */
	md = EVP_sha1();
	/* [한국어] OpenSSL이 SHA-1을 지원하지 않는 환경은 사실상 없으므로 단언. */
	assert(md != NULL);

	/* [한국어] 새 EVP MD 컨텍스트 할당 — 실패 시 -ENOMEM. */
	mdctx = EVP_MD_CTX_new();
	if (mdctx == NULL) {
		return -ENOMEM;
	}

	/* [한국어] 컨텍스트를 SHA-1 으로 초기화. 1=성공, 그 외=실패. */
	if (EVP_DigestInit_ex(mdctx, md, NULL) != 1) {
		SPDK_ERRLOG("Could not initialize EVP digest!\n");
		goto err;
	}
	/* [한국어] 첫 update: 네임스페이스 UUID 16바이트를 입력으로 추가. */
	if (EVP_DigestUpdate(mdctx, ns_uuid, sizeof(struct spdk_uuid)) != 1) {
		SPDK_ERRLOG("Could update EVP digest with namespace UUID!\n");
		goto err;
	}
	/* [한국어] 두 번째 update: 사용자 이름 바이트열을 추가. */
	if (EVP_DigestUpdate(mdctx, name, len) != 1) {
		SPDK_ERRLOG("Could update EVP digest with assigned name!\n");
		goto err;
	}
	/* [한국어] 다이제스트 finalize — md_value 에 SHA-1 20바이트, md_len=20. */
	if (EVP_DigestFinal_ex(mdctx, md_value, &md_len) != 1) {
		SPDK_ERRLOG("Could not generate EVP digest!\n");
		goto err;
	}
	/* [한국어] 컨텍스트 해제 — 누수 방지. */
	EVP_MD_CTX_free(mdctx);

	/* [한국어] 20바이트 SHA-1 결과 중 앞 16바이트만 UUID 본문으로 채택. */
	memcpy(uuid, md_value, 16);
	/* This part mimics original uuid_generate_sha1() from libuuid/src/gen_uuid.c.
	 * The original uuid structure included from uuid.h looks like this:
	 * struct uuid {
	 *	uint32_t	time_low;
	 *	uint16_t	time_mid;
	 *	uint16_t	time_hi_and_version;
	 *	uint16_t	clock_seq;
	 *	uint8_t		node[6];
	 * };
	 * so uuid->u.raw[6] and uuid->u.raw[8] are time_hi_and_version and clock_seq respectively.
	 */
	/* [한국어] RFC 4122 §4.1.3: time_hi_and_version 의 상위 4비트를
	 * version 5 (= 0101_2 = 0x5) 로 강제. 하위 4비트는 SHA-1 결과 그대로 유지. */
	uuid->u.raw[6] = (uuid->u.raw[6] & 0x0f) | 0x50;
	/* [한국어] RFC 4122 §4.1.1: clock_seq_hi_and_reserved 의 상위 2비트를
	 * variant "10_2" (= 0b10xxxxxx) 로 강제. 나머지는 SHA-1 그대로. */
	uuid->u.raw[8] = (uuid->u.raw[8] & 0x3f) | 0x80;

	return 0;

err:
	/* [한국어] 에러 경로: 컨텍스트 해제 후 -EINVAL 반환. */
	EVP_MD_CTX_free(mdctx);
	return -EINVAL;

#endif /* SPDK_CONFIG_HAVE_UUID_GENERATE_SHA1 */
}
