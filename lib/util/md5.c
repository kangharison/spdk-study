/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] OpenSSL EVP 위에 얹은 SPDK MD5 래퍼 (md5.c)
 *
 * === 파일의 역할 ===
 * SPDK 내부에서 MD5 해시가 필요할 때 호출되는 init/update/final 3단계 API
 * (`spdk_md5init`, `spdk_md5update`, `spdk_md5final`)를 OpenSSL의 EVP
 * (Envelope) 인터페이스 위에 얇게 래핑한다. 직접 OpenSSL을 부르는 대신
 * 일관된 SPDK 컨벤션(반환값 -1=실패, 0/1=성공/포함값, 컨텍스트 구조체
 * `spdk_md5ctx`)을 제공해 호출 측 코드가 단순해지게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK에서 MD5는 보안용이 아니라 무결성/식별 용도로 쓰인다. 대표 사용처:
 *   - iSCSI target의 CHAP 인증 challenge/response 계산 (lib/iscsi/iscsi.c).
 *   - 일부 디버그/검증 dump.
 * (참고: 보안 요구가 큰 경로는 SHA-2 등 다른 다이제스트를 사용하며 MD5는
 * 호환성/저비용 무결성 검증 위주.)
 * 호출 흐름:
 *   호출자 → spdk_md5init → spdk_md5update(N회) → spdk_md5final → 16B 해시.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: OpenSSL libcrypto의 EVP 시리즈(EVP_MD_CTX_create/EVP_md5/
 *   EVP_DigestInit_ex/EVP_DigestUpdate/EVP_DigestFinal_ex/EVP_MD_CTX_destroy).
 *   런타임 라이브러리 링크 필요(-lcrypto).
 * - 헤더: `spdk/md5.h`(spdk_md5ctx 정의 포함), `spdk/likely.h`(브랜치 힌트),
 *   `spdk/stdinc.h`.
 * - 호출자: lib/iscsi 등. 이 파일은 어떤 SPDK 핵심 자료구조도 공유하지
 *   않으며, 컨텍스트 메모리는 호출자 스택/힙에서 제공된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_md5init(ctx): EVP_MD_CTX 생성 후 MD5 알고리즘으로 DigestInit.
 *   실패 시 -1, 성공 시 OpenSSL의 1을 그대로 전달.
 * - spdk_md5update(ctx, data, len): 데이터 누적. 빈 입력은 0 반환(no-op).
 * - spdk_md5final(md5, ctx): 최종 16바이트 해시를 md5 버퍼에 기록하고
 *   EVP_MD_CTX를 해제.
 * - struct spdk_md5ctx (헤더 정의): 내부 OpenSSL EVP_MD_CTX* 한 개를 감싸는
 *   wrapper. 호출자가 init/final 사이 상태를 보존하는 용도.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 C 라이브러리 헤더 일괄 포함 (size_t, NULL 등). */
#include "spdk/md5.h"
/* [한국어] struct spdk_md5ctx 정의(`md5ctx->md5ctx` 필드는 EVP_MD_CTX*),
 * 본 파일에서 구현하는 spdk_md5* 함수들의 prototype. */
#include "spdk/likely.h"
/* [한국어] spdk_likely/spdk_unlikely — __builtin_expect 기반 분기 예측 힌트.
 * 에러 경로(NULL/실패)를 unlikely로 표시해 핫패스 명령 정렬을 돕는다. */

/*
 * [한국어]
 * spdk_md5init - MD5 컨텍스트를 초기화 (OpenSSL EVP_MD_CTX 생성).
 *
 * @md5ctx: 호출자가 제공한 spdk_md5ctx 구조체 포인터. NULL 금지.
 * @return: 성공 시 EVP_DigestInit_ex의 1, 실패 시 -1.
 *
 * 동기: MD5 누적 계산에는 OpenSSL이 내부 상태(EVP_MD_CTX)를 들고 있어야
 * 하며, 그 상태를 spdk_md5ctx 안에 보관한다. 본 함수는 그 메모리를 OpenSSL
 * 측에서 할당받고 MD5 알고리즘으로 초기 디스패치까지 수행한다.
 *
 * 실행 컨텍스트: 호출자가 단일 스레드 사용. 같은 ctx를 여러 스레드가 동시에
 * update하면 OpenSSL 내부 상태가 깨지므로 호출자가 책임지고 직렬화해야 한다.
 *
 * 호출 체인:
 *   호출자(예: iSCSI CHAP 계산) → spdk_md5init → EVP_MD_CTX_create
 *     → EVP_DigestInit_ex(EVP_md5())
 */
int
spdk_md5init(struct spdk_md5ctx *md5ctx)
{
	int rc;
	/* [한국어] OpenSSL 호출 결과 임시 저장. EVP 함수들은 1=성공/0=실패를
	 * 반환하므로 SPDK 컨벤션(0/-1)으로 변환하기 전 일단 보관. */

	if (spdk_unlikely(md5ctx == NULL)) {
		/* [한국어] NULL 컨텍스트는 프로그램 버그. unlikely로 마킹해 정상
		 * 경로의 명령 캐시 효율을 보존. */
		return -1;
	}

	md5ctx->md5ctx = EVP_MD_CTX_create();
	/* [한국어] OpenSSL이 내부 다이제스트 상태를 담을 EVP_MD_CTX를 동적 할당.
	 * SPDK 측 wrapper의 md5ctx 필드에 저장한다. 실패 시 NULL 반환. */
	if (spdk_unlikely(md5ctx->md5ctx == NULL)) {
		/* [한국어] OpenSSL 메모리 할당 실패. 추가 작업 없이 -1로 보고. */
		return -1;
	}

	rc = EVP_DigestInit_ex(md5ctx->md5ctx, EVP_md5(), NULL);
	/* [한국어] EVP_md5(): OpenSSL에서 MD5 알고리즘을 식별하는 EVP_MD 포인터.
	 * EVP_DigestInit_ex: 컨텍스트를 MD5로 디스패치. 세번째 ENGINE 인자는
	 * NULL로 두어 기본 구현을 사용. 반환값 1=성공, 0=실패(OpenSSL 컨벤션). */
	/* For EVP_DigestInit_ex, 1 == success, 0 == failure. */
	if (spdk_unlikely(rc == 0)) {
		/* [한국어] 디스패치 실패 시 이미 할당한 EVP_MD_CTX를 누수 없이
		 * 정리하고 wrapper의 포인터도 NULL로 되돌려 부분 초기화 상태 방지. */
		EVP_MD_CTX_destroy(md5ctx->md5ctx);
		md5ctx->md5ctx = NULL;
		rc = -1;
	}
	return rc;
	/* [한국어] 성공 시 1, 실패 시 -1을 호출자에 전달. SPDK 코드는 보통
	 * `if (spdk_md5init(...) < 0)`로 검사한다. */
}

/*
 * [한국어]
 * spdk_md5final - 누적된 데이터를 MD5 다이제스트로 마무리하고 컨텍스트 파괴.
 *
 * @md5: 16바이트 출력 버퍼. 호출자가 미리 준비. NULL 금지.
 * @md5ctx: spdk_md5init으로 초기화된 컨텍스트. NULL 금지.
 * @return: EVP_DigestFinal_ex의 1(성공)/0(실패), 또는 인자 검증 실패 시 -1.
 *
 * MD5 표준상 출력은 128bit = 16B이며, 본 함수는 EVP_DigestFinal_ex 호출 후
 * 즉시 EVP_MD_CTX를 해제하므로 호출자는 이후 같은 ctx로 update/final을 다시
 * 부르면 안 된다(필요 시 spdk_md5init부터 재초기화).
 *
 * 호출 체인:
 *   호출자 → spdk_md5final → EVP_DigestFinal_ex → EVP_MD_CTX_destroy.
 */
int
spdk_md5final(void *md5, struct spdk_md5ctx *md5ctx)
{
	int rc;
	/* [한국어] OpenSSL EVP_DigestFinal_ex 반환값 캐시. */

	if (spdk_unlikely(md5ctx == NULL || md5 == NULL)) {
		/* [한국어] 어느 한쪽이라도 NULL이면 안전하게 -1. */
		return -1;
	}
	rc = EVP_DigestFinal_ex(md5ctx->md5ctx, md5, NULL);
	/* [한국어] 누적된 MD5 상태를 16B 다이제스트로 추출.
	 * 세번째 인자(unsigned int *outl)는 NULL로 두어 길이 정보를 받지 않음
	 * (MD5는 항상 16B로 고정이므로 호출자가 알고 있다). */
	EVP_MD_CTX_destroy(md5ctx->md5ctx);
	/* [한국어] OpenSSL 측 EVP_MD_CTX 메모리 해제. 누수 방지. */
	md5ctx->md5ctx = NULL;
	/* [한국어] dangling pointer 방지: wrapper의 내부 포인터를 NULL로 리셋해
	 * 호출자가 실수로 같은 ctx를 재사용해도 NULL 체크에 걸리도록 한다. */
	return rc;
	/* [한국어] DigestFinal 결과 그대로 반환 (1=성공, 0=실패). */
}

/*
 * [한국어]
 * spdk_md5update - MD5 컨텍스트에 데이터를 누적 입력.
 *
 * @md5ctx: 활성화된 컨텍스트(NULL 금지).
 * @data: 누적할 바이트 시작 주소. NULL이거나 len=0이면 no-op.
 * @len: data의 바이트 길이.
 * @return: -1(잘못된 컨텍스트) / 0(no-op or 실패) / 1(성공).
 *
 * 데이터가 메모리상 분리되어 있는 경우 여러 번 호출해 누적할 수 있다.
 * iSCSI CHAP 같은 경로에서 헤더+데이터를 차례로 update하는 패턴이 흔하다.
 *
 * 호출 체인:
 *   호출자(예: iSCSI CHAP) → spdk_md5update → EVP_DigestUpdate.
 */
int
spdk_md5update(struct spdk_md5ctx *md5ctx, const void *data, size_t len)
{
	int rc;
	/* [한국어] EVP_DigestUpdate 반환값 임시 저장. */

	if (spdk_unlikely(md5ctx == NULL)) {
		/* [한국어] 컨텍스트 미초기화/NULL은 사용자 에러로 즉시 -1. */
		return -1;
	}
	if (spdk_unlikely(data == NULL || len == 0)) {
		/* [한국어] 빈 입력은 합법적인 no-op으로 간주하고 0 반환.
		 * 호출자는 가변 길이 stream을 다룰 때 길이 0을 별도 분기 없이
		 * 그냥 update에 넘길 수 있어 코드가 깔끔해진다. */
		return 0;
	}
	rc = EVP_DigestUpdate(md5ctx->md5ctx, data, len);
	/* [한국어] OpenSSL이 내부 64B 블록 단위로 MD5 라운드를 돌려 상태를
	 * 갱신한다. 호출자는 결과를 final 시점에 회수. */
	return rc;
	/* [한국어] 성공 1 / 실패 0을 그대로 노출. SPDK 호출 측은 보통
	 * `<= 0` 검사로 에러 처리. */
}
