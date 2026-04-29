/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation.  All rights reserved.
 */

/*
 * [한국어 설명] NVMe-oF DH-HMAC-CHAP 인증 호스트 측 구현 (nvme_auth.c)
 *
 * === 파일의 역할 ===
 * NVMe-over-Fabrics(NVMe-oF) **호스트(initiator) 측 DH-HMAC-CHAP 인증 프로토콜의 완전 구현**.
 * NVMe-oF 1.1 스펙 Section 8.13 "Authentication"에 정의된 in-band 인증 메커니즘으로,
 * Fabrics 트랜스포트(RDMA/TCP/FC) 위에서 호스트와 컨트롤러가 사전 공유 비밀키
 * (PSK; Pre-Shared Key)를 노출하지 않고 상호 신원을 증명한다.
 *
 * 핵심 가치 5가지:
 *   1) **PSK 비공개 인증**: 양 측이 같은 키를 알지만 그 키 자체는 와이어로 전송하지 않음.
 *      대신 challenge(임의 nonce) + 키 → HMAC 다이제스트로 응답 (challenge-response 패턴).
 *   2) **DH 키 교환 옵션**: NULL/ffdhe2048/3072/4096/6144/8192 6가지 DH 그룹 지원.
 *      DH 사용 시 매 세션마다 새 dhsec(forward secrecy) 도출 → PSK 노출 없이도 nonce 보강.
 *   3) **상호 인증 옵션**: dhchap_ctrlr_key 제공 시 호스트가 컨트롤러에게도 challenge를 보내
 *      success1에서 컨트롤러의 응답을 검증. 한 방향이 아닌 양방향 신원 확인.
 *   4) **다양한 해시 강도**: SHA-256/384/512 협상으로 보안-성능 트레이드오프 선택.
 *   5) **상태머신 기반 비동기 폴링**: NEGOTIATE → AWAIT_NEGOTIATE → AWAIT_CHALLENGE →
 *      AWAIT_REPLY → AWAIT_SUCCESS1 → (옵션 AWAIT_SUCCESS2) → DONE의 8 상태 머신.
 *      각 단계마다 1 RTT를 소비하므로 spdk_nvme_qpair_process_completions 폴링과 통합.
 *
 * **본 파일은 코드 수정 없이 한국어 주석만 추가/보강된 학습 사본**이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 진입 — 두 가지 경로:
 *   [경로 1: 자동 트리거] CONNECT 응답에서 컨트롤러가 atr=1로 인증 요구
 *     → nvme_fabric.c::qpair_connect_poll에서 auth.flags.atr 플래그 셋
 *     → nvme_qpair.c가 connect 후 qpair_auth_required() 검사 → 이 파일의
 *       nvme_fabric_qpair_authenticate_async 자동 호출
 *
 *   [경로 2: 사용자 명시적 호출] 컨트롤러가 인증 강제하지 않더라도 호스트가 원할 때
 *     사용자 → spdk_nvme_qpair_authenticate(qpair, cb_fn, cb_ctx)
 *     → 트랜스포트 vtable의 qpair_authenticate
 *     → (PCIe는 미지원, RDMA/TCP는) nvme_fabric_qpair_authenticate_async
 *
 * 둘 다 결국 같은 진입점:
 *   nvme_fabric_qpair_authenticate_async (이 파일):
 *     1. auth tracker 할당 (calloc + DMA 4KiB zmalloc)
 *     2. tid 채번 (ctrlr->auth_tid++ 단조증가)
 *     3. 상태 = NEGOTIATE
 *     4. 첫 nvme_fabric_qpair_authenticate_poll 호출 → NEGOTIATE 메시지 송신 트리거
 *
 * 이후 매 reactor tick에서:
 *   spdk_nvme_qpair_process_completions
 *     → 트랜스포트가 fabric_poll_status로 응답 수령 시
 *       → nvme_fabric_qpair_authenticate_poll (이 파일)
 *         → switch(state) 한 단계 진행 → state 전이 → 다음 메시지 송신/수신 시작
 *         → state == DONE 시 cleanup + auth->cb_fn 호출
 *
 * 와이어 메시지 (NVMe-oF Authentication Send/Recv 명령으로 캡슐화):
 *   AUTH_negotiate(host→ctrlr): 지원 해시 목록 + 지원 dhgroup 목록 제시
 *   DH-HMAC-CHAP_challenge(ctrlr→host): seqnum + cval(nonce) + 선택된 hash/dhgroup + ctrlr DH pubkey
 *   DH-HMAC-CHAP_reply(host→ctrlr): rval(응답 HMAC) + ctrlr_challenge(상호 인증 시) + host DH pubkey
 *   DH-HMAC-CHAP_success1(ctrlr→host): 컨트롤러 검증 결과 + (rvalid 시) 컨트롤러 응답 HMAC
 *   DH-HMAC-CHAP_success2(host→ctrlr, 상호인증 시): 호스트 검증 결과 통보
 *   AUTH_failure1/2: 어느 쪽이든 실패 시 진단 정보와 함께 종료
 *
 * 실행 컨텍스트: 인증 폴러는 트랜스포트의 process_completions 안에서 호출되므로
 *               qpair 소유 spdk_thread에 고정. ctrlr 단위 lock은 auth_seqnum/auth_tid 채번 시에만 사용.
 *
 * === 타 모듈과의 연결 ===
 *  - **OpenSSL libcrypto** (필수 의존): EVP_MAC(HMAC), EVP_PKEY(DHX), EVP_MD, OSSL_PARAM,
 *    BIGNUM/BN_*, RAND_bytes. SPDK_CONFIG_HAVE_EVP_MAC 매크로로 빌드 게이트 (없으면 stub).
 *  - **nvme_fabric.c**: 이 파일에서 사용하는 nvme_fabric_qpair_poll_cleanup,
 *    nvme_fabric_qpair_auth_cleanup 호출. 반대로 fabric.c는 connect 응답에서 atr/ascr를
 *    파싱해 auth.flags 세팅 → 이 파일이 polling 시 사용.
 *  - **nvme_qpair.c**: nvme_qpair_submit_request로 AUTH Send/Recv 캡슐 제출.
 *    qpair->reserved_req 사용(일반 free_req 풀이 비어도 인증 메시지는 항상 보낼 수 있도록 격리).
 *  - **nvme_internal.h**: struct nvme_auth(qpair 임베디드 인증 상태), enum nvme_qpair_auth_state,
 *    nvme_completion_poll_status, nvme_ctrlr_lock/unlock, nvme_wait_for_completion_poll,
 *    spdk_nvme_ctrlr_opts(dhchap_key/dhchap_ctrlr_key/dhchap_digests/dhchap_dhgroups 비트마스크).
 *  - **spdk/keyring** (spdk_key, spdk_key_get_key, spdk_key_dup, spdk_keyring_put_key):
 *    PSK 키 저장소 추상화. 키 자체는 keyring에 보관되고 핸들만 받아 사용 후 put.
 *  - **트랜스포트 레이어** (nvme_transport_qpair_authenticate): vtable로 PCIe vs Fabrics 분기.
 *    PCIe는 -ENOTSUP, Fabrics는 이 파일의 authenticate_async로 위임.
 *
 * 데이터 흐름 (메시지 1개 송신 사이클):
 *   상위 폴러 호출 → switch(state) → 메시지 빌드(dma_data 4KiB 버퍼에 헤더 + payload 채움) →
 *   nvme_auth_submit_request → reserved_req에 SQE 채움(opcode=FABRIC, fctype=AUTH_SEND/RECV) →
 *   nvme_qpair_submit_request → 트랜스포트 → 컨트롤러 ↔ 응답 수신 → 다음 폴링에서 처리
 *
 * === 주요 함수/구조체 요약 ===
 *  핵심 자료구조:
 *    g_digests[3]: 지원 해시 테이블 (SHA-256/384/512 + ID + 길이)
 *    g_dhgroups[6]: 지원 DH 그룹 테이블 (NULL + ffdhe2048~8192 + 이름)
 *    qpair->auth (struct nvme_auth, nvme_internal.h:663): 상태머신 + tid + 누적 status +
 *      ctrlr_challenge 버퍼(상호 인증) + cb_fn/cb_ctx + flags(atr/ascr/in_auth_poll)
 *    qpair->fabric_poll_status: AUTH 메시지 송수신용 4KiB DMA 버퍼 + completion poll status
 *    qpair->reserved_req: 일반 free_req 풀 고갈 시에도 인증 가능하도록 격리된 nvme_request
 *
 *  주요 함수:
 *    [공개 사전 검색]
 *    - spdk_nvme_dhchap_get_digest_id/name/length: 해시 ID↔이름 변환 + 길이 조회
 *    - spdk_nvme_dhchap_get_dhgroup_id/name: DH 그룹 ID↔이름 변환
 *    - spdk_nvme_dhchap_calculate: 표준 HMAC 계산 (외부 사용 가능, NVMe-oF target도 사용)
 *
 *    [DH 키 관리]
 *    - spdk_nvme_dhchap_generate_dhkey: EVP_PKEY DHX 키페어 생성
 *    - spdk_nvme_dhchap_dhkey_free / get_pubkey / derive_secret: 공개키 추출 / DH secret 도출
 *
 *    [상태머신 헬퍼]
 *    - nvme_auth_set_state / set_failure: 상태 전이 + 실패 경로 분기
 *    - nvme_auth_get_seqnum: 컨트롤러 challenge용 seqnum 채번 (RAND_bytes 시드)
 *    - nvme_auth_transform_key / get_key: keyring 문자열 → HMAC 입력 바이트 (CRC 검증 포함)
 *    - nvme_auth_augment_challenge: DH secret으로 challenge nonce 보강 (HMAC(MD(key) | cval))
 *
 *    [메시지 빌드/제출]
 *    - nvme_auth_submit_request: AUTH Send/Recv SQE 빌드 + nvme_qpair_submit_request 호출
 *    - nvme_auth_recv_message / send_failure2: 수신 트리거 / 실패 통지
 *    - nvme_auth_check_message: AUTH_failure1 자동 처리 + 메시지 ID 검증
 *
 *    [프로토콜 메시지]
 *    - nvme_auth_send_negotiate: AUTH_negotiate 빌드 (지원 hash/dhgroup 목록)
 *    - nvme_auth_check_challenge: 수신한 DH-HMAC-CHAP_challenge 유효성 검증
 *    - nvme_auth_send_reply: ★ 핵심 — DH 도출 + HMAC 계산 + reply 메시지 송신
 *    - nvme_auth_check_success1: success1 검증 + (상호 인증 시) ctrlr 응답 검증
 *    - nvme_auth_send_success2: 상호 인증 완료 통지
 *
 *    [상태머신 driver]
 *    - nvme_fabric_qpair_authenticate_poll: ★★★ 8 상태 dispatch + 한 단계 진행
 *    - nvme_fabric_qpair_authenticate_async: 인증 시작 (tracker 할당 + tid 채번 + 첫 polling)
 *    - spdk_nvme_qpair_authenticate: 사용자 노출 API (cb_fn 등록 + 트랜스포트 위임)
 */

#include "spdk/base64.h"
                                  /* [한국어] base64 인코딩/디코딩 — keyring의 키 문자열은 base64 형식 */
#include "spdk/crc32.h"
                                  /* [한국어] CRC32 IEEE — keyring 키 무결성 검증용 */
#include "spdk/endian.h"
                                  /* [한국어] from_le32 등 little-endian 변환 — 와이어 포맷 처리 */
#include "spdk/log.h"
                                  /* [한국어] SPDK 로그 시스템 — SPDK_ERRLOG, SPDK_DEBUGLOG */
#include "spdk/string.h"
                                  /* [한국어] spdk_strerror, spdk_memset_s(보안 메모리 클리어) */
#include "spdk/util.h"
                                  /* [한국어] SPDK_COUNTOF, SPDK_BIT 등 유틸 매크로 */
#include "spdk_internal/nvme.h"
                                  /* [한국어] SPDK 내부 NVMe 정의 — DHCHAP 관련 enum/struct (auth_negotiate, dhchap_challenge 등) */
#include "nvme_internal.h"
                                  /* [한국어] NVMe 드라이버 내부 — struct nvme_auth, nvme_qpair_auth_state,
                                   *  nvme_qpair_submit_request, NVME_INIT_REQUEST, fabric_poll_status 등 */

#ifdef SPDK_CONFIG_HAVE_EVP_MAC
                                  /* [한국어] OpenSSL 3.0+ EVP_MAC API 사용 가능 시에만 컴파일.
                                   *  EVP_MAC은 OpenSSL 3.0에서 도입된 새 MAC 인터페이스 (HMAC, CMAC 등 통합).
                                   *  미지원 환경(예: OpenSSL 1.x)에서는 인증 기능 자체 비활성화. */
#include <openssl/dh.h>
                                  /* [한국어] DH(Diffie-Hellman) 키 교환 — RFC 7919 Finite Field DH 그룹 사용 */
#include <openssl/evp.h>
                                  /* [한국어] EVP_MAC, EVP_PKEY, EVP_MD — OpenSSL 고수준 암호 인터페이스 */
#include <openssl/param_build.h>
                                  /* [한국어] OSSL_PARAM_BLD — 동적으로 OSSL_PARAM 배열 빌드 (peer key 구성에 사용) */
#include <openssl/rand.h>
                                  /* [한국어] RAND_bytes — 암호학적 안전 난수 생성 (challenge nonce, seqnum 시드) */
#endif

/*
 * [한국어] 지원되는 HMAC 해시 함수 메타데이터 항목.
 *
 * NVMe-oF DH-HMAC-CHAP 스펙에서 정의된 hash function ID와 OpenSSL이 인식하는 이름,
 * 그리고 다이제스트 길이(바이트)를 한 줄로 묶음. negotiate 시 호스트가 지원 목록을
 * desc->hash_id_list에 채워 보내고, 컨트롤러가 그중 하나를 선택해 challenge에 명시.
 */
struct nvme_auth_digest {
	uint8_t		id;
                                  /* [한국어] 스펙 ID — SPDK_NVMF_DHCHAP_HASH_SHA256/384/512 (스펙 8.13.5 표) */
	const char	*name;
                                  /* [한국어] OpenSSL EVP_MD/EVP_MAC fetch 시 사용할 알고리즘 이름 ("sha256" 등) */
	uint8_t		len;
                                  /* [한국어] 다이제스트 출력 바이트 수 (SHA-256=32, SHA-384=48, SHA-512=64).
                                   *  challenge/reply 메시지의 cval/rval 길이로 직접 사용됨. */
};

/*
 * [한국어] 지원되는 DH(Diffie-Hellman) 그룹 메타데이터 항목.
 *
 * NULL은 DH 키 교환 안 함(평문 PSK 사용), 나머지는 RFC 7919 ffdhe* 그룹.
 * 그룹 번호 끝 숫자(2048, 3072 등)는 modulus 비트 크기로, 클수록 보안 강하나 키 생성 비용 증가.
 */
struct nvme_auth_dhgroup {
	uint8_t		id;
                                  /* [한국어] 스펙 ID — SPDK_NVMF_DHCHAP_DHGROUP_NULL/2048/3072/4096/6144/8192 */
	const char	*name;
                                  /* [한국어] OpenSSL DHX fetch 시의 그룹 이름 ("ffdhe2048" 등 RFC 7919 명명) */
};

#define NVME_AUTH_DATA_SIZE			4096
                                  /* [한국어] 단일 AUTH 메시지 최대 크기 (4KiB) — 페이지 1개에 맞춤.
                                   *  challenge에 큰 dhgroup pubkey(8192-bit = 1KiB)가 포함되어도 충분. */
#define NVME_AUTH_DH_KEY_MAX_SIZE		1024
                                  /* [한국어] DH 공개키/secret 버퍼 크기 (1KiB) — 8192-bit DH의 modulus = 1024B. */
#define NVME_AUTH_CHAP_KEY_MAX_SIZE		256
                                  /* [한국어] PSK 키 문자열의 최대 크기 — base64 인코딩된 64B 키 + DHHC-1:XX: 헤더 등 포함. */

#define AUTH_DEBUGLOG(q, fmt, ...) NVME_QPAIR_LOG2(DEBUG, nvme_auth, q, fmt, ##__VA_ARGS__)
                                  /* [한국어] qpair 식별자가 자동 부착되는 디버그 로그 — nvme_auth 컴포넌트로 분류. */
#define AUTH_ERRLOG(q, fmt, ...) NVME_QPAIR_LOG(ERR, q, fmt, ##__VA_ARGS__)
                                  /* [한국어] 인증 실패/오류용 에러 로그 — qpair traddr/subnqn 자동 부착. */
#define AUTH_LOGDUMP(msg, buf, len) \
	SPDK_LOGDUMP(nvme_auth, msg, buf, len)
                                  /* [한국어] hex dump 로그 — DH pubkey, dhsec 등 디버깅용 바이트 덤프.
                                   *  운영 빌드에서는 컴파일 아웃 권장(보안). */

/*
 * [한국어] 호스트가 지원하는 해시 알고리즘 정적 테이블.
 *
 * negotiate 송신 시 ctrlr->opts.dhchap_digests 비트마스크와 AND 연산으로 필터링되어
 * 실제 desc->hash_id_list에 채워진다. 사용자가 약한 해시(SHA-256 등)를 거부하려면
 * dhchap_digests 비트마스크에서 제외하면 됨.
 */
static const struct nvme_auth_digest g_digests[] = {
	{ SPDK_NVMF_DHCHAP_HASH_SHA256, "sha256", 32 },
                                  /* [한국어] SHA-256 — 32B 다이제스트, 가장 빠르고 가장 흔한 기본값 */
	{ SPDK_NVMF_DHCHAP_HASH_SHA384, "sha384", 48 },
                                  /* [한국어] SHA-384 — 48B 다이제스트, 중간 보안/성능 */
	{ SPDK_NVMF_DHCHAP_HASH_SHA512, "sha512", 64 },
                                  /* [한국어] SHA-512 — 64B 다이제스트, 최고 보안 (64-bit 머신에서 SHA-256보다 빠를 수도) */
};

/*
 * [한국어] 호스트가 지원하는 DH 그룹 정적 테이블.
 *
 * NULL 그룹은 DH 키 교환 없이 PSK만 사용 — 약한 보안 모드. 상호 합의된 PSK가
 * 영구적으로 노출되면 과거 트래픽 모두 풀려버림(no forward secrecy).
 * ffdhe* 그룹은 DH 키 교환으로 매 세션마다 새 dhsec 생성 → forward secrecy 확보.
 */
static const struct nvme_auth_dhgroup g_dhgroups[] = {
	{ SPDK_NVMF_DHCHAP_DHGROUP_NULL, "null" },
                                  /* [한국어] DH 미사용 — challenge에 dhvlen=0, secret 보강 없음 */
	{ SPDK_NVMF_DHCHAP_DHGROUP_2048, "ffdhe2048" },
                                  /* [한국어] RFC 7919 2048-bit DH — 가장 가벼운 forward secrecy 옵션 */
	{ SPDK_NVMF_DHCHAP_DHGROUP_3072, "ffdhe3072" },
                                  /* [한국어] RFC 7919 3072-bit DH — 보안 ↑, 키생성 비용 ↑ */
	{ SPDK_NVMF_DHCHAP_DHGROUP_4096, "ffdhe4096" },
                                  /* [한국어] RFC 7919 4096-bit DH */
	{ SPDK_NVMF_DHCHAP_DHGROUP_6144, "ffdhe6144" },
                                  /* [한국어] RFC 7919 6144-bit DH */
	{ SPDK_NVMF_DHCHAP_DHGROUP_8192, "ffdhe8192" },
                                  /* [한국어] RFC 7919 8192-bit DH — 최고 보안, 키 생성에 수십~수백 ms 소요 */
};

/*
 * [한국어]
 * nvme_auth_get_digest - g_digests에서 ID로 메타데이터 찾기 (내부 헬퍼)
 *
 * @id: SPDK_NVMF_DHCHAP_HASH_* 상수
 * @return 일치하는 nvme_auth_digest 포인터 또는 NULL.
 *
 * 사용처: 내부에서 ID를 알고 이름이나 길이가 필요할 때.
 */
static const struct nvme_auth_digest *
nvme_auth_get_digest(int id)
{
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(g_digests); ++i) {
                                  /* [한국어] g_digests 정적 테이블 선형 검색 — 항목 3개라 O(1)에 가까움 */
		if (g_digests[i].id == id) {
			return &g_digests[i];
                                  /* [한국어] 일치 — 정적 테이블 포인터 그대로 반환 (불변, free 불필요) */
		}
	}

	return NULL;
                                  /* [한국어] 미지원 ID */
}

/*
 * [한국어]
 * spdk_nvme_dhchap_get_digest_id - 해시 이름 → ID 변환 (사용자 노출 API)
 *
 * @digest: "sha256"/"sha384"/"sha512"
 * @return 해시 ID (양수) 또는 -EINVAL (미지원 이름).
 *
 * 사용처: RPC/설정 파일 파서가 사용자 입력 문자열을 ID로 변환할 때.
 */
int
spdk_nvme_dhchap_get_digest_id(const char *digest)
{
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(g_digests); ++i) {
                                  /* [한국어] 정적 테이블 순회 */
		if (strcmp(g_digests[i].name, digest) == 0) {
                                  /* [한국어] 정확 일치 (case-sensitive) */
			return g_digests[i].id;
		}
	}

	return -EINVAL;
                                  /* [한국어] 미지원 이름 — 호출자가 사용자에게 에러 메시지 표시 */
}

/*
 * [한국어]
 * spdk_nvme_dhchap_get_digest_name - 해시 ID → 이름 변환 (사용자 노출 API)
 *
 * @id: SPDK_NVMF_DHCHAP_HASH_* 상수
 * @return 해시 이름 문자열 또는 NULL (미지원).
 *
 * 사용처: 디버그 로그, RPC 응답에 사용자에게 표시할 이름 변환.
 */
const char *
spdk_nvme_dhchap_get_digest_name(int id)
{
	const struct nvme_auth_digest *digest = nvme_auth_get_digest(id);
                                  /* [한국어] ID 검색 위임 */

	return digest != NULL ? digest->name : NULL;
                                  /* [한국어] NULL 안전 — 미지원 ID는 NULL 반환 */
}

/*
 * [한국어]
 * spdk_nvme_dhchap_get_dhgroup_id - DH 그룹 이름 → ID 변환 (사용자 노출 API)
 *
 * @dhgroup: "null"/"ffdhe2048"/"ffdhe3072"/"ffdhe4096"/"ffdhe6144"/"ffdhe8192"
 * @return DH 그룹 ID (≥0) 또는 -EINVAL.
 */
int
spdk_nvme_dhchap_get_dhgroup_id(const char *dhgroup)
{
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(g_dhgroups); ++i) {
                                  /* [한국어] g_dhgroups 6항목 선형 검색 */
		if (strcmp(g_dhgroups[i].name, dhgroup) == 0) {
			return g_dhgroups[i].id;
                                  /* [한국어] 정확 일치 시 ID 반환 */
		}
	}

	return -EINVAL;
                                  /* [한국어] 미지원 이름 */
}

/*
 * [한국어]
 * spdk_nvme_dhchap_get_dhgroup_name - DH 그룹 ID → 이름 변환 (사용자 노출 API)
 */
const char *
spdk_nvme_dhchap_get_dhgroup_name(int id)
{
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(g_dhgroups); ++i) {
                                  /* [한국어] 6항목 선형 검색 — 별도 헬퍼 두지 않고 인라인 (digests와 비대칭이지만 사소함) */
		if (g_dhgroups[i].id == id) {
			return g_dhgroups[i].name;
		}
	}

	return NULL;
                                  /* [한국어] 미지원 ID */
}

/*
 * [한국어]
 * spdk_nvme_dhchap_get_digest_length - 해시 ID → 다이제스트 바이트 수 (사용자 노출 API)
 *
 * @return 32/48/64 또는 0(미지원).
 *
 * 사용처: challenge/reply 메시지의 hl 필드 검증, response 버퍼 크기 산정.
 */
uint8_t
spdk_nvme_dhchap_get_digest_length(int id)
{
	const struct nvme_auth_digest *digest = nvme_auth_get_digest(id);

	return digest != NULL ? digest->len : 0;
                                  /* [한국어] NULL 안전 — 미지원이면 0 (호출자가 0 검사로 거부) */
}

#ifdef SPDK_CONFIG_HAVE_EVP_MAC
                                  /* [한국어] 이하는 OpenSSL 3.0+ EVP_MAC가 있을 때만 컴파일.
                                   *  실제 인증 상태머신 + HMAC/DH 계산이 모두 여기에 포함. */

/*
 * [한국어]
 * nvme_auth_digest_allowed - 사용자 정책상 이 해시 사용이 허용됐는지 확인
 *
 * @qpair: 질의 대상 qpair
 * @digest: SPDK_NVMF_DHCHAP_HASH_*
 * @return true(허용) / false(거부).
 *
 * 동작: ctrlr->opts.dhchap_digests 비트마스크에서 해당 비트 확인.
 *       기본값은 모든 해시 비트 셋 (사용자가 명시 제외 안 했으면 모두 허용).
 *
 * 사용처: negotiate 송신 시 후보 필터링 + challenge 수신 시 컨트롤러 선택 검증.
 */
static bool
nvme_auth_digest_allowed(struct spdk_nvme_qpair *qpair, uint8_t digest)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;

	return ctrlr->opts.dhchap_digests & SPDK_BIT(digest);
                                  /* [한국어] 비트마스크의 해당 비트 확인 — SPDK_BIT(n) = 1u << n.
                                   *  비트 N이 1이면 해시 ID N이 허용됨 (스펙 ID = 비트 위치). */
}

/*
 * [한국어]
 * nvme_auth_dhgroup_allowed - 사용자 정책상 이 DH 그룹 사용이 허용됐는지 확인
 *
 * 위 digest와 동일 패턴 — ctrlr->opts.dhchap_dhgroups 비트마스크 검사.
 */
static bool
nvme_auth_dhgroup_allowed(struct spdk_nvme_qpair *qpair, uint8_t dhgroup)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;

	return ctrlr->opts.dhchap_dhgroups & SPDK_BIT(dhgroup);
                                  /* [한국어] 비트 N=1이면 dhgroup ID N 허용 */
}

/*
 * [한국어]
 * nvme_auth_set_state - 인증 상태머신 상태 전이 + 디버그 로그
 *
 * @qpair: 대상 qpair
 * @state: 새 상태
 *
 * 동작: 디버그 로그에 상태 이름 출력 후 qpair->auth.state 갱신.
 *       state_names 배열은 디버그 빌드에서만 의미 (release 빌드는 unused 경고 회피).
 */
static void
nvme_auth_set_state(struct spdk_nvme_qpair *qpair, enum nvme_qpair_auth_state state)
{
	static const char *state_names[] __attribute__((unused)) = {
                                  /* [한국어] 상태 → 이름 매핑 — 디버그 로그 전용. release 빌드에서 SPDK_DEBUGLOG가
                                   *  컴파일 아웃되면 이 배열도 unused가 되므로 attribute로 경고 회피. */
		[NVME_QPAIR_AUTH_STATE_NEGOTIATE] = "negotiate",
		[NVME_QPAIR_AUTH_STATE_AWAIT_NEGOTIATE] = "await-negotiate",
		[NVME_QPAIR_AUTH_STATE_AWAIT_CHALLENGE] = "await-challenge",
		[NVME_QPAIR_AUTH_STATE_AWAIT_REPLY] = "await-reply",
		[NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS1] = "await-success1",
		[NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS2] = "await-success2",
		[NVME_QPAIR_AUTH_STATE_AWAIT_FAILURE2] = "await-failure2",
		[NVME_QPAIR_AUTH_STATE_DONE] = "done",
                                  /* [한국어] designated initializer로 enum 순서와 무관하게 안전 초기화 */
	};

	AUTH_DEBUGLOG(qpair, "auth state: %s\n", state_names[state]);
                                  /* [한국어] 상태 전이 추적 — 인증 진행 흐름 디버그에 필수 */
	qpair->auth.state = state;
                                  /* [한국어] 실제 상태 갱신 — 다음 polling tick에서 이 값으로 dispatch */
}

/*
 * [한국어]
 * nvme_auth_set_failure - 실패 상태로 전이 + 종료 경로 선택
 *
 * @qpair: 대상 qpair
 * @status: 실패 errno (음수)
 * @failure2: true면 AWAIT_FAILURE2(상대방에게 failure2 통지 후 응답 대기), false면 즉시 DONE
 *
 * 동작:
 *   [1] 누적 status 보존 — 이미 0이 아니면 첫 에러 유지 (이후 에러로 덮지 않음).
 *   [2] failure2 분기:
 *       - true: 호스트가 컨트롤러에게 AUTH_failure2를 전송한 뒤 응답을 기다림.
 *               (failure2 송신 자체가 성공했을 때만 사용 — send_failure2 반환값 기반)
 *       - false: 즉시 DONE — 통지 송신 자체가 실패했거나 응답이 의미 없는 경우.
 *
 * 호출자: nvme_auth_check_message, nvme_auth_check_challenge, nvme_auth_check_success1
 *         (검증 실패 모든 경로) + nvme_fabric_qpair_authenticate_poll의 송신 실패 경로.
 */
static void
nvme_auth_set_failure(struct spdk_nvme_qpair *qpair, int status, bool failure2)
{
	if (qpair->auth.status == 0) {
                                  /* [한국어] 첫 에러만 보존 — 이후 cleanup에서 발생한 에러는 무시. */
		qpair->auth.status = status;
	}

	nvme_auth_set_state(qpair, failure2 ?
			    NVME_QPAIR_AUTH_STATE_AWAIT_FAILURE2 :
			    NVME_QPAIR_AUTH_STATE_DONE);
                                  /* [한국어] failure2=true면 통지 응답 대기, false면 즉시 종료 — 둘 다 결국 DONE에 도달.
                                   *  AWAIT_FAILURE2는 통지 송신 응답을 한 번 기다린 후 DONE으로 자동 전이. */
}

/*
 * [한국어]
 * nvme_auth_print_cpl - completion 실패 시 진단 로그
 *
 * @qpair: 대상 qpair
 * @msg: 어떤 메시지의 완료가 실패했는지 (예: "AUTH_negotiate", "DH-HMAC-CHAP_reply")
 *
 * 출력: sc(status code), sct(status code type), timed_out 여부.
 *       NVMe-oF 응답 SQE의 SC/SCT 필드를 그대로 노출 — 사용자가 스펙으로 디코딩.
 */
static void
nvme_auth_print_cpl(struct spdk_nvme_qpair *qpair, const char *msg)
{
	struct nvme_completion_poll_status *status = qpair->fabric_poll_status;
                                  /* [한국어] connect/auth 전용 폴링 상태 — fabric_poll_status에 마지막 응답이 남아있음 */

	AUTH_ERRLOG(qpair, "%s failed: sc=%d, sct=%d (timed out: %s)\n", msg, status->cpl.status.sc,
		    status->cpl.status.sct, status->timed_out ? "true" : "false");
                                  /* [한국어] sc=Status Code(7-bit), sct=Status Code Type(3-bit), timed_out=ms 초과 표시.
                                   *  사용자는 NVMe Base/Fabrics 스펙으로 의미 디코딩. */
}

/*
 * [한국어]
 * nvme_auth_get_seqnum - 컨트롤러 challenge용 seqnum 채번
 *
 * @qpair: 대상 qpair (실은 컨트롤러 단위 카운터 사용)
 * @return 1~UINT32_MAX의 seqnum 또는 0(에러).
 *
 * 동작:
 *   ctrlr->auth_seqnum이 0이면(첫 사용) RAND_bytes로 시작값 시드 → 예측 불가 시작점 보장.
 *   ++seqnum이 0이 되면(wrap-around) 1로 강제 — 0은 invalid sentinel.
 *
 * 동기화: ctrlr->lock 보호 — 같은 컨트롤러에 여러 qpair가 동시에 채번 가능.
 *
 * 사용처: 호스트가 컨트롤러에 보낼 reply 메시지의 seqnum (상호 인증 시 컨트롤러 challenge용).
 *         스펙상 0이 아니어야 하며, 매 reply마다 단조증가가 권장.
 */
static uint32_t
nvme_auth_get_seqnum(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	uint32_t seqnum;
	int rc;

	nvme_ctrlr_lock(ctrlr);
                                  /* [한국어] 동일 컨트롤러의 여러 qpair가 동시에 인증할 수 있으므로 락 필수 */
	if (ctrlr->auth_seqnum == 0) {
                                  /* [한국어] 최초 사용 — RAND로 32-bit 시작값 시드.
                                   *  단순 1부터 시작하면 예측 가능 → 보안 약화. */
		rc = RAND_bytes((void *)&ctrlr->auth_seqnum, sizeof(ctrlr->auth_seqnum));
		if (rc != 1) {
                                  /* [한국어] OpenSSL RAND 실패 — 매우 드물지만 잠금 해제 후 0 반환 */
			nvme_ctrlr_unlock(ctrlr);
			return 0;
		}
	}
	if (++ctrlr->auth_seqnum == 0) {
                                  /* [한국어] 32-bit wrap-around — UINT32_MAX → 0이 되면 1로 강제.
                                   *  0은 스펙에서 invalid이므로 사용 금지. */
		ctrlr->auth_seqnum = 1;
	}
	seqnum = ctrlr->auth_seqnum;
                                  /* [한국어] 락 해제 전에 로컬 복사 — 락 해제 후 다른 qpair가 카운터 변경 가능 */
	nvme_ctrlr_unlock(ctrlr);

	return seqnum;
}

/*
 * [한국어]
 * nvme_auth_transform_key - keyring 키 바이트 → HMAC 입력 키로 변환
 *
 * @key:    키 메타데이터 (디버그 로그용)
 * @hash:   변환에 사용할 해시 함수 (SPDK_NVMF_DHCHAP_HASH_NONE이면 raw passthrough)
 * @nqn:    HMAC 입력에 포함될 NQN (NVMe Qualified Name) — 키 바인딩 강화
 * @keyin:  원본 키 바이트
 * @keylen: 원본 키 바이트 수
 * @out:    [out] 변환 결과 버퍼
 * @outlen: out 버퍼 크기
 * @return  변환 결과 길이(>0) 또는 음수 errno.
 *
 * 동작 — 두 모드:
 *   [1] HASH_NONE: keyin → out 단순 복사 (변환 없이 raw 키 사용).
 *   [2] HASH_SHA256/384/512: HMAC(key=keyin, data=nqn || "NVMe-over-Fabrics") → out.
 *       이는 NVMe-oF 스펙 8.13.5.4 "Key Derivation"의 정의.
 *       원본 키를 NQN과 결합해 다이제스트화 → 같은 PSK여도 다른 NQN에서는 다른 키.
 *
 * 호출 체인:
 *   nvme_auth_get_key → 이 함수 (DHHC-1 keyring 문자열을 파싱한 후 호출)
 */
static int
nvme_auth_transform_key(struct spdk_key *key, uint32_t hash, const char *nqn,
			const void *keyin, size_t keylen, void *out, size_t outlen)
{
	EVP_MAC *hmac = NULL;
	EVP_MAC_CTX *ctx = NULL;
	OSSL_PARAM params[2];
	int rc;

	switch (hash) {
	case SPDK_NVMF_DHCHAP_HASH_NONE:
                                  /* [한국어] NONE 모드 — 변환 없이 그대로 복사 */
		if (keylen > outlen) {
			SPDK_ERRLOG("Key buffer too small: %zu < %zu (key=%s)\n", outlen, keylen,
				    spdk_key_get_name(key));
			return -ENOBUFS;
                                  /* [한국어] 출력 버퍼 부족 */
		}
		memcpy(out, keyin, keylen);
		return keylen;
                                  /* [한국어] 복사한 바이트 수 반환 — out에 raw 키 그대로 */
	case SPDK_NVMF_DHCHAP_HASH_SHA256:
	case SPDK_NVMF_DHCHAP_HASH_SHA384:
	case SPDK_NVMF_DHCHAP_HASH_SHA512:
		break;
                                  /* [한국어] 지원 해시 — 아래 OpenSSL 처리로 진행 */
	default:
		SPDK_ERRLOG("Unsupported key hash: 0x%x (key=%s)\n", hash, spdk_key_get_name(key));
		return -EINVAL;
                                  /* [한국어] 미지원 해시 ID */
	}

	hmac = EVP_MAC_fetch(NULL, "hmac", NULL);
                                  /* [한국어] OpenSSL HMAC 알고리즘 핸들 획득 — 첫 NULL은 default lib context */
	if (hmac == NULL) {
		return -EIO;
                                  /* [한국어] OpenSSL 내부 오류 — algorithm not found */
	}
	ctx = EVP_MAC_CTX_new(hmac);
                                  /* [한국어] HMAC 연산 컨텍스트 — init/update/final 사이클 동안 상태 유지 */
	if (ctx == NULL) {
		rc = -EIO;
		goto out;
                                  /* [한국어] OOM 등 — out 라벨에서 hmac 해제 */
	}
	params[0] = OSSL_PARAM_construct_utf8_string("digest",
			(char *)spdk_nvme_dhchap_get_digest_name(hash), 0);
                                  /* [한국어] HMAC의 내부 해시 함수 지정 — "sha256"/"sha384"/"sha512" */
	params[1] = OSSL_PARAM_construct_end();
                                  /* [한국어] 파라미터 배열 종료 sentinel */

	if (EVP_MAC_init(ctx, keyin, keylen, params) != 1) {
                                  /* [한국어] HMAC 초기화 — 키와 해시 종류 설정. 이후 update로 데이터 누적 */
		rc = -EIO;
		goto out;
	}
	if (EVP_MAC_update(ctx, nqn, strlen(nqn)) != 1) {
                                  /* [한국어] HMAC 입력 데이터 1: NQN(NVMe Qualified Name) — 키-NQN 바인딩 */
		rc = -EIO;
		goto out;
	}
	if (EVP_MAC_update(ctx, "NVMe-over-Fabrics", strlen("NVMe-over-Fabrics")) != 1) {
                                  /* [한국어] HMAC 입력 데이터 2: 고정 문자열 — 스펙 정의된 도메인 분리자
                                   *  (다른 프로토콜에서 같은 PSK 사용해도 다른 키 도출 보장) */
		rc = -EIO;
		goto out;
	}
	if (EVP_MAC_final(ctx, out, &outlen, outlen) != 1) {
                                  /* [한국어] HMAC 결과 출력 — out 버퍼에 다이제스트 기록, outlen에 실제 길이 갱신 */
		rc = -EIO;
		goto out;
	}
	rc = (int)outlen;
                                  /* [한국어] 성공 — 다이제스트 길이 반환 */
out:
	EVP_MAC_CTX_free(ctx);
                                  /* [한국어] NULL-safe — ctx==NULL이면 noop */
	EVP_MAC_free(hmac);
                                  /* [한국어] 알고리즘 핸들 해제 */

	return rc;
}

/*
 * [한국어]
 * nvme_auth_get_key - keyring에서 PSK 추출 + DHHC-1 형식 파싱 + HMAC 입력 변환
 *
 * @key:    keyring에서 받은 키 핸들
 * @nqn:    transform 시 결합할 NQN
 * @buf:    [out] HMAC 입력 키
 * @buflen: buf 크기
 * @return  변환 후 키 길이(>0) 또는 음수 errno.
 *
 * NVMe-oF DHHC-1 키 포맷 (스펙 8.13.5.7):
 *   "DHHC-1:HH:base64-encoded-key-with-crc32:"
 *   - HH: 해시 ID 16진수 (00=NONE, 01=SHA256, 02=SHA384, 03=SHA512)
 *   - base64: 32/48/64B 키 + 4B CRC32 little-endian
 *   - trailing ":" 필수
 *
 * 동작:
 *   [1] keyring API로 키 문자열 회수.
 *   [2] sscanf로 "DHHC-1:HH:" 헤더 파싱 → hash 추출.
 *   [3] secret 시작점 = keystr + 10 (= "DHHC-1:HH:" 길이), strstr로 다음 ':' 찾아 NUL 종결.
 *   [4] base64 디코드 → 36/52/68B 결과 (각각 SHA-256/384/512 키 32/48/64B + CRC4B).
 *   [5] 끝 4B = CRC32 → spdk_crc32_ieee_update로 검증 (불일치 = 키 변조 또는 잘못된 해시).
 *   [6] CRC 제외한 키 바이트 → nvme_auth_transform_key로 HMAC 입력 변환.
 *
 * 보안: 함수 종료 직전 keystr/keyb64를 spdk_memset_s로 0 클리어 → 메모리에 키 흔적 제거.
 *       (컴파일러 최적화로 일반 memset이 제거되지 않도록 secure 버전 사용)
 */
static int
nvme_auth_get_key(struct spdk_key *key, const char *nqn, void *buf, size_t buflen)
{
	char keystr[NVME_AUTH_CHAP_KEY_MAX_SIZE + 1] = {};
                                  /* [한국어] keyring 원본 문자열 — +1로 NUL 여유 */
	char keyb64[NVME_AUTH_CHAP_KEY_MAX_SIZE] = {};
                                  /* [한국어] base64 디코드 결과 버퍼 */
	char *tmp, *secret;
	uint32_t hash;
	int rc;
	size_t keylen;

	rc = spdk_key_get_key(key, keystr, NVME_AUTH_CHAP_KEY_MAX_SIZE);
                                  /* [한국어] keyring에서 키 문자열 회수 — keyring 백엔드(파일/HSM 등)가 처리 */
	if (rc < 0) {
		SPDK_ERRLOG("Failed to load key=%s: %s\n", spdk_key_get_name(key),
			    spdk_strerror(-rc));
		goto out;
                                  /* [한국어] 키 추출 실패 — out에서 메모리 클리어 후 반환 */
	}

	rc = sscanf(keystr, "DHHC-1:%02x:", &hash);
                                  /* [한국어] DHHC-1 헤더 파싱 — "DHHC-1:" 리터럴 + 2자리 16진수 hash + ":" */
	if (rc != 1) {
                                  /* [한국어] sscanf가 1개 변환 못 하면 형식 오류 */
		SPDK_ERRLOG("Invalid key format (key=%s)\n", spdk_key_get_name(key));
		rc = -EINVAL;
		goto out;

	}
	/* Start at the first character after second ":" and remove the trailing ":" */
	secret = &keystr[10];
                                  /* [한국어] 오프셋 10 = "DHHC-1:" 7자 + "HH" 2자 + ":" 1자.
                                   *  secret은 base64 시작 위치를 가리킴. */
	tmp = strstr(secret, ":");
                                  /* [한국어] base64의 끝 ':' 찾기 — 끝까지 찾은 위치를 NUL로 바꿔서 base64 부분만 분리 */
	if (!tmp) {
		SPDK_ERRLOG("Invalid key format (key=%s)\n", spdk_key_get_name(key));
		rc = -EINVAL;
		goto out;
                                  /* [한국어] trailing ':' 없음 — DHHC-1 형식 위반 */
	}

	*tmp = '\0';
                                  /* [한국어] base64 끝 위치를 NUL로 → secret이 순수 base64 NUL-terminated 문자열 */
	keylen = sizeof(keyb64);
                                  /* [한국어] base64 decode 출력 버퍼 크기를 IN/OUT으로 전달 */
	rc = spdk_base64_decode(keyb64, &keylen, secret);
                                  /* [한국어] base64 디코드 — keyb64에 raw 바이트, keylen에 실제 디코드된 바이트 수 */
	if (rc != 0) {
		SPDK_ERRLOG("Invalid key format (key=%s)\n", spdk_key_get_name(key));
		rc = -EINVAL;
		goto out;
                                  /* [한국어] base64 디코드 실패 — 비ASCII 문자 등 */
	}
	/* Only 32B, 48B, and 64B keys are supported (+ 4B, as they're followed by a crc32) */
	if (keylen != 36 && keylen != 52 && keylen != 68) {
                                  /* [한국어] SHA-256(32B+4B), SHA-384(48B+4B), SHA-512(64B+4B) 외에는 거부 */
		SPDK_ERRLOG("Invalid key size=%zu (key=%s)\n", keylen, spdk_key_get_name(key));
		rc = -EINVAL;
		goto out;
	}

	keylen -= 4;
                                  /* [한국어] CRC32 4B 제외한 실제 키 길이 — 이후 변환에 사용 */
	if (~spdk_crc32_ieee_update(keyb64, keylen, ~0) != from_le32(&keyb64[keylen])) {
                                  /* [한국어] CRC32 검증:
                                   *  - 좌변: 키 바이트의 CRC32 IEEE — ~0(=0xFFFFFFFF)로 시작 + 끝에서 ~ 보수
                                   *    (표준 CRC32 IEEE 802.3 다항식 패턴)
                                   *  - 우변: 키 끝에 little-endian 32-bit으로 저장된 CRC.
                                   *  불일치 = 키 변조 또는 잘못된 해시 ID(헤더와 본문 불일치). */
		SPDK_ERRLOG("Invalid key checksum (key=%s)\n", spdk_key_get_name(key));
		rc = -EINVAL;
		goto out;
	}

	rc = nvme_auth_transform_key(key, hash, nqn, keyb64, keylen, buf, buflen);
                                  /* [한국어] 검증된 raw 키를 HMAC 입력으로 변환 (NONE 모드면 단순 복사) */
out:
	spdk_memset_s(keystr, sizeof(keystr), 0, sizeof(keystr));
                                  /* [한국어] 보안: 함수 종료 전 키 흔적 제거 — spdk_memset_s는 컴파일러가 제거 못 하도록 보장 */
	spdk_memset_s(keyb64, sizeof(keyb64), 0, sizeof(keyb64));
                                  /* [한국어] base64 디코드 결과도 클리어 (raw 키 + CRC 노출 방지) */

	return rc;
}

/*
 * [한국어]
 * nvme_auth_augment_challenge - DH secret으로 challenge nonce 보강
 *
 * @cval:  컨트롤러가 보낸 원본 challenge nonce
 * @clen:  cval 길이
 * @key:   DH secret (NULL이면 보강 없이 통과)
 * @keylen: DH secret 길이
 * @caval: [out] 보강된 challenge ("composite challenge value")
 * @calen: [in/out] caval 버퍼 크기 / 실제 길이
 * @hash:  HMAC 해시 함수
 * @return 0 성공, 음수 errno.
 *
 * 알고리즘 (NVMe-oF 스펙 8.13.5.5):
 *   key가 NULL: caval = cval (단순 복사 — DH 미사용 경로)
 *   key가 있음:
 *     1) keydgst = MD(key)        — DH secret을 한 번 해시
 *     2) caval = HMAC(key=keydgst, data=cval)  — keydgst를 키로 cval 재HMAC
 *
 *   이렇게 하면 같은 cval이라도 매 세션의 dhsec에 따라 다른 caval이 도출되어
 *   forward secrecy 확보. 공격자가 PSK를 알아내도 과거 세션의 dhsec까지 알아야 캐로드 가능.
 *
 * 호출자: spdk_nvme_dhchap_calculate (HMAC 응답 계산의 첫 단계)
 */
static int
nvme_auth_augment_challenge(const void *cval, size_t clen, const void *key, size_t keylen,
			    void *caval, size_t *calen, enum spdk_nvmf_dhchap_hash hash)
{
	EVP_MAC *hmac = NULL;
	EVP_MAC_CTX *ctx = NULL;
	EVP_MD *md = NULL;
	OSSL_PARAM params[2];
	uint8_t keydgst[NVME_AUTH_DIGEST_MAX_SIZE];
                                  /* [한국어] DH secret의 다이제스트 — 그대로 HMAC 키로 사용 */
	unsigned int dgstlen = sizeof(keydgst);
                                  /* [한국어] EVP_Digest 호출 시 IN/OUT (실제 다이제스트 길이로 갱신됨) */
	int rc = 0;

	/* If there's no key, there's nothing to augment, cval == caval */
	if (key == NULL) {
                                  /* [한국어] DH 미사용(NULL group) — cval을 그대로 caval에 복사 */
		assert(clen <= *calen);
                                  /* [한국어] 호출자가 충분한 버퍼를 줘야 함 (디버그 검증) */
		memcpy(caval, cval, clen);
		*calen = clen;
                                  /* [한국어] 출력 길이 = 입력 길이 */
		return 0;
	}

	md = EVP_MD_fetch(NULL, spdk_nvme_dhchap_get_digest_name(hash), NULL);
                                  /* [한국어] 해시 함수 핸들 획득 — DH secret 다이제스트 계산용 */
	if (!md) {
		SPDK_ERRLOG("Failed to fetch digest function: %d\n", hash);
		return -EINVAL;
	}
	if (EVP_Digest(key, keylen, keydgst, &dgstlen, md, NULL) != 1) {
                                  /* [한국어] 단발 해시 — DH secret을 다이제스트화. 결과는 keydgst (32~64B). */
		rc = -EIO;
		goto out;
	}

	hmac = EVP_MAC_fetch(NULL, "hmac", NULL);
                                  /* [한국어] HMAC 알고리즘 — 두 번째 단계용 */
	if (hmac == NULL) {
		rc = -EIO;
		goto out;
	}
	ctx = EVP_MAC_CTX_new(hmac);
	if (ctx == NULL) {
		rc = -EIO;
		goto out;
	}
	params[0] = OSSL_PARAM_construct_utf8_string("digest",
			(char *)spdk_nvme_dhchap_get_digest_name(hash), 0);
	params[1] = OSSL_PARAM_construct_end();

	if (EVP_MAC_init(ctx, keydgst, dgstlen, params) != 1) {
                                  /* [한국어] HMAC 초기화 — 키 = 위에서 계산한 keydgst */
		rc = -EIO;
		goto out;
	}
	if (EVP_MAC_update(ctx, cval, clen) != 1) {
                                  /* [한국어] HMAC 입력 = cval (원본 challenge nonce) */
		rc = -EIO;
		goto out;
	}
	if (EVP_MAC_final(ctx, caval, calen, *calen) != 1) {
                                  /* [한국어] HMAC 결과 = caval (composite challenge) — 보강된 nonce */
		rc = -EIO;
		goto out;
	}
out:
	EVP_MD_free(md);
	EVP_MAC_CTX_free(ctx);
	EVP_MAC_free(hmac);
                                  /* [한국어] NULL-safe — 부분 실패 시에도 안전 cleanup */

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_dhchap_calculate - DH-HMAC-CHAP 응답값(rval) 계산 (사용자 노출 API)
 *
 * @key:   PSK keyring 핸들
 * @hash:  사용 해시 (challenge에서 컨트롤러가 선택)
 * @type:  HMAC 입력에 포함될 type 문자열 — "HostHost" 또는 "Controller" (NVMe-oF 스펙 정의)
 * @seq:   challenge의 seqnum (또는 호스트가 채번한 seqnum)
 * @tid:   인증 transaction ID
 * @scc:   secure channel concatenation (보통 0)
 * @nqn1:  HMAC 입력의 첫 NQN (호스트→ctrlr이면 hostnqn, ctrlr→호스트면 subnqn)
 * @nqn2:  두 번째 NQN
 * @dhkey: DH secret (NULL이면 미사용)
 * @dhlen: DH secret 길이
 * @cval:  challenge nonce
 * @rval:  [out] 응답값 — 다이제스트 길이만큼 기록
 * @return 0 성공, 음수 errno.
 *
 * 알고리즘 (NVMe-oF 스펙 8.13.5.6):
 *   1) caval = augment_challenge(cval, dhkey)  — DH secret으로 challenge 보강
 *   2) keybuf = transform_key(PSK, hash, nqn1) — PSK를 NQN과 결합한 HMAC 입력 변환
 *   3) rval = HMAC(key=keybuf, data = caval || seq || tid || scc || type || nqn1 || NUL || nqn2)
 *
 * 양 측이 같은 알고리즘을 같은 입력으로 돌리면 같은 rval → challenge-response 검증 가능.
 *
 * 사용처:
 *   호스트:
 *     - reply 메시지의 rval 계산 (자기 자신 인증) — type="HostHost", nqn1=hostnqn, nqn2=subnqn
 *     - 컨트롤러용 challenge 응답 미리 계산 (상호 인증, success1 검증용) — type="Controller",
 *       nqn1=subnqn, nqn2=hostnqn
 *   target(NVMe-oF tgt 측 코드도 이 함수 사용):
 *     - 수신한 reply의 rval 검증
 *
 * SPDK_CONFIG_HAVE_EVP_MAC 가드 안에 있어 OpenSSL 3.0+ 환경 한정.
 */
int
spdk_nvme_dhchap_calculate(struct spdk_key *key, enum spdk_nvmf_dhchap_hash hash,
			   const char *type, uint32_t seq, uint16_t tid, uint8_t scc,
			   const char *nqn1, const char *nqn2, const void *dhkey, size_t dhlen,
			   const void *cval, void *rval)
{
	EVP_MAC *hmac;
	EVP_MAC_CTX *ctx;
	OSSL_PARAM params[2];
	uint8_t keybuf[NVME_AUTH_CHAP_KEY_MAX_SIZE], term = 0;
                                  /* [한국어] keybuf: 변환된 키 / term: HMAC 입력 사이의 NUL 구분자 (스펙 정의) */
	uint8_t caval[NVME_AUTH_DATA_SIZE];
                                  /* [한국어] augmented challenge — 4KiB는 안전 마진 (실제는 다이제스트 길이만큼만 사용) */
	size_t hlen, calen = sizeof(caval);
	int rc, keylen;

	hlen = spdk_nvme_dhchap_get_digest_length(hash);
                                  /* [한국어] 출력 다이제스트 길이 — final 호출에 IN/OUT으로 전달 */
	rc = nvme_auth_augment_challenge(cval, hlen, dhkey, dhlen, caval, &calen, hash);
                                  /* [한국어] 1단계: cval을 DH secret으로 보강. dhkey=NULL이면 단순 복사. */
	if (rc != 0) {
		return rc;
	}

	hmac = EVP_MAC_fetch(NULL, "hmac", NULL);
	if (hmac == NULL) {
		return -EIO;
	}

	ctx = EVP_MAC_CTX_new(hmac);
	if (ctx == NULL) {
		rc = -EIO;
		goto out;
	}

	keylen = nvme_auth_get_key(key, nqn1, keybuf, sizeof(keybuf));
                                  /* [한국어] 2단계: keyring에서 키 회수 + DHHC-1 파싱 + HMAC 입력 변환.
                                   *  nqn1과 결합 → 키-NQN 바인딩. */
	if (keylen < 0) {
		rc = keylen;
		goto out;
	}

	params[0] = OSSL_PARAM_construct_utf8_string("digest",
			(char *)spdk_nvme_dhchap_get_digest_name(hash), 0);
	params[1] = OSSL_PARAM_construct_end();

	rc = -EIO;
                                  /* [한국어] 이하 모든 OpenSSL 호출 실패 시 -EIO. 성공 시 마지막에 0으로 갱신. */
	if (EVP_MAC_init(ctx, keybuf, (size_t)keylen, params) != 1) {
                                  /* [한국어] HMAC 초기화 — 키 = transform된 PSK 기반 keybuf */
		goto out;
	}
	if (EVP_MAC_update(ctx, caval, calen) != 1) {
                                  /* [한국어] 입력 1: caval (보강된 challenge) */
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)&seq, sizeof(seq)) != 1) {
                                  /* [한국어] 입력 2: seqnum (4B little-endian 그대로 — 호스트 byte order 사용) */
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)&tid, sizeof(tid)) != 1) {
                                  /* [한국어] 입력 3: transaction ID (2B) — 와이어 포맷도 호스트 byte order */
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)&scc, sizeof(scc)) != 1) {
                                  /* [한국어] 입력 4: secure channel concatenation 1B (보통 0) */
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)type, strlen(type)) != 1) {
                                  /* [한국어] 입력 5: 타입 문자열 ("HostHost" 또는 "Controller") — 도메인 분리자 */
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)nqn1, strlen(nqn1)) != 1) {
                                  /* [한국어] 입력 6: 첫 NQN — type=HostHost면 hostnqn, Controller면 subnqn */
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)&term, sizeof(term)) != 1) {
                                  /* [한국어] 입력 7: NUL 구분자 — 두 NQN 사이 명확한 경계 표시 (스펙 정의) */
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)nqn2, strlen(nqn2)) != 1) {
                                  /* [한국어] 입력 8: 두 번째 NQN */
		goto out;
	}
	if (EVP_MAC_final(ctx, rval, &hlen, hlen) != 1) {
                                  /* [한국어] 결과 출력 — rval에 다이제스트 기록 */
		goto out;
	}
	rc = 0;
                                  /* [한국어] 모든 단계 성공 */
out:
	spdk_memset_s(keybuf, sizeof(keybuf), 0, sizeof(keybuf));
                                  /* [한국어] 보안: 변환된 키 흔적 메모리에서 제거 */
	EVP_MAC_CTX_free(ctx);
	EVP_MAC_free(hmac);

	return rc;
}

/*
 * [한국어]
 * spdk_nvme_dhchap_generate_dhkey - DH 키페어 생성 (사용자 노출 API)
 *
 * @dhgroup: 사용할 DH 그룹 (ffdhe2048 등)
 * @return EVP_PKEY 포인터 (불투명 핸들로 형변환) 또는 NULL.
 *
 * 동작:
 *   "DHX" 알고리즘(파라미터화된 DH) 컨텍스트 생성 → keygen 초기화 → group 파라미터 설정 →
 *   EVP_PKEY_generate로 비공개키 + 공개키 한 번에 생성.
 *
 *   ffdhe* 그룹은 모듈러 큰 만큼 키 생성에 시간 걸림 (8192 = 수십~수백 ms).
 *
 * 사용처: nvme_auth_send_reply에서 DH 그룹 선택 후 호스트 키페어 생성.
 *
 * 메모리: 호출자는 spdk_nvme_dhchap_dhkey_free로 해제 책임.
 */
struct spdk_nvme_dhchap_dhkey *
spdk_nvme_dhchap_generate_dhkey(enum spdk_nvmf_dhchap_dhgroup dhgroup)
{
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *key = NULL;
	OSSL_PARAM params[2];

	ctx = EVP_PKEY_CTX_new_from_name(NULL, "DHX", NULL);
                                  /* [한국어] DHX = DH eXtended (파라미터를 외부에서 지정 가능한 변형) */
	if (ctx == NULL) {
		goto error;
	}
	if (EVP_PKEY_keygen_init(ctx) != 1) {
                                  /* [한국어] keygen 모드 초기화 — 이후 set_params로 그룹 지정 후 generate */
		goto error;
	}

	params[0] = OSSL_PARAM_construct_utf8_string("group",
			(char *)spdk_nvme_dhchap_get_dhgroup_name(dhgroup), 0);
                                  /* [한국어] "ffdhe2048" 등의 RFC 7919 그룹명 지정 */
	params[1] = OSSL_PARAM_construct_end();
	if (EVP_PKEY_CTX_set_params(ctx, params) != 1) {
		SPDK_ERRLOG("Failed to set dhkey's dhgroup: %s\n",
			    spdk_nvme_dhchap_get_dhgroup_name(dhgroup));
		goto error;
	}
	if (EVP_PKEY_generate(ctx, &key) != 1) {
                                  /* [한국어] 실제 키페어 생성 — 비공개키 내부 보관, 공개키 추출 가능 */
		goto error;
	}
error:
	EVP_PKEY_CTX_free(ctx);
                                  /* [한국어] 키 생성 ctx 해제 (성공/실패 공통) — key는 별도 보존 */
	return (void *)key;
                                  /* [한국어] 불투명 포인터로 호출자에게 반환. 실패 시 NULL. */
}

/*
 * [한국어]
 * spdk_nvme_dhchap_dhkey_free - DH 키페어 해제 (사용자 노출 API)
 *
 * @key: [in/out] 키 포인터의 포인터 — 해제 후 NULL로 클리어.
 *
 * NULL-safe: key==NULL이면 noop, *key==NULL이어도 EVP_PKEY_free가 NULL 안전.
 */
void
spdk_nvme_dhchap_dhkey_free(struct spdk_nvme_dhchap_dhkey **key)
{
	if (key == NULL) {
		return;
                                  /* [한국어] 호출자가 NULL 포인터 줬으면 무시 */
	}

	EVP_PKEY_free(*(EVP_PKEY **)key);
                                  /* [한국어] EVP_PKEY 해제 — 내부 BIGNUM 등 모두 secure 해제 */
	*key = NULL;
                                  /* [한국어] dangling 방지 — 호출자 변수도 NULL로 */
}

/*
 * [한국어]
 * spdk_nvme_dhchap_dhkey_get_pubkey - DH 키페어에서 공개키 바이트 추출 (사용자 노출 API)
 *
 * @dhkey: 생성된 DH 키페어
 * @pub:   [out] 공개키 바이트 버퍼
 * @len:   [in/out] 버퍼 크기 / 실제 기록된 바이트 수
 * @return 0 성공, 음수 errno (-EIO/-EINVAL).
 *
 * 동작:
 *   EVP_PKEY_get_bits로 비트 크기 → 8로 나눠 바이트 수 산출 → 버퍼 크기 검증 →
 *   "pub" BIGNUM 파라미터 추출 → BN_bn2binpad로 big-endian 패딩된 바이트 배열로 변환.
 *
 *   bn2binpad는 padding 보장 — 작은 공개키도 num_bytes만큼 leading zero로 채움
 *   (와이어 포맷 일관성).
 */
int
spdk_nvme_dhchap_dhkey_get_pubkey(struct spdk_nvme_dhchap_dhkey *dhkey, void *pub, size_t *len)
{
	EVP_PKEY *key = (EVP_PKEY *)dhkey;
	BIGNUM *bn = NULL;
	int rc;
	const size_t num_bytes = (size_t)spdk_divide_round_up(EVP_PKEY_get_bits(key), 8);
                                  /* [한국어] 비트 수를 8로 올림 나눗셈 → 바이트 수 (예: 2048-bit DH = 256B) */

	if (num_bytes == 0) {
                                  /* [한국어] EVP_PKEY_get_bits 실패 — 잘못된 키 */
		SPDK_ERRLOG("Failed to get key size\n");
		return -EIO;
	}

	if (num_bytes > *len) {
                                  /* [한국어] 호출자 버퍼 부족 — 필요 크기 알려주고 거부 */
		SPDK_ERRLOG("Insufficient key buffer size=%zu (needed=%zu)",
			    *len, num_bytes);
		return -EINVAL;
	}
	*len = num_bytes;
                                  /* [한국어] 출력 길이 갱신 — 호출자가 정확한 길이로 와이어에 기록 */

	if (EVP_PKEY_get_bn_param(key, "pub", &bn) != 1) {
                                  /* [한국어] DH 키의 "pub" 파라미터(공개키)를 BIGNUM으로 추출 */
		rc = -EIO;
		goto error;
	}

	rc = BN_bn2binpad(bn, pub, *len);
                                  /* [한국어] BIGNUM → big-endian 바이트 + leading zero padding으로 변환.
                                   *  반환값은 기록된 바이트 수 (양수) 또는 -1(실패). */
	if (rc <= 0) {
		rc = -EIO;
		goto error;
	}
	rc = 0;
                                  /* [한국어] 성공 시 0으로 정규화 (위 rc는 바이트 수였음) */
error:
	BN_free(bn);
                                  /* [한국어] NULL-safe — bn==NULL이면 noop */
	return rc;
}

/*
 * [한국어]
 * nvme_auth_get_peerkey - 컨트롤러가 보낸 공개키 바이트 → EVP_PKEY 객체로 복원
 *
 * @peerkey: 컨트롤러 공개키 바이트 (challenge에서 추출)
 * @len:     peerkey 길이
 * @dhgroup: 어느 DH 그룹의 공개키인지 ("ffdhe2048" 등)
 * @return EVP_PKEY 포인터 또는 NULL (실패).
 *
 * 동작:
 *   DHX 컨텍스트 생성 → fromdata 모드 → 바이트 → BIGNUM → OSSL_PARAM("pub", "group") → fromdata로 키 객체 생성 → EVP_PKEY_dup로 복사 후 임시 객체 해제.
 *
 *   왜 dup?: fromdata로 만든 키는 ctx에 묶여 ctx 해제 시 무효화될 수 있음 → 별도 복제로 ctx 해제 후에도 사용 가능.
 *
 * 사용처: nvme_auth_send_reply에서 DH secret 도출 직전에 호출.
 */
static EVP_PKEY *
nvme_auth_get_peerkey(const void *peerkey, size_t len, const char *dhgroup)
{
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *result = NULL, *key = NULL;
                                  /* [한국어] result는 dup된 최종 결과, key는 fromdata가 만든 임시 객체 */
	OSSL_PARAM_BLD *bld = NULL;
	OSSL_PARAM *params = NULL;
	BIGNUM *bn = NULL;

	ctx = EVP_PKEY_CTX_new_from_name(NULL, "DHX", NULL);
                                  /* [한국어] DHX 컨텍스트 — 동일 알고리즘으로 키 import */
	if (ctx == NULL) {
		goto error;
	}
	if (EVP_PKEY_fromdata_init(ctx) != 1) {
                                  /* [한국어] fromdata 모드 초기화 — 외부 데이터로 키 객체 만들기 */
		goto error;
	}

	bn = BN_bin2bn(peerkey, len, NULL);
                                  /* [한국어] big-endian 바이트 → BIGNUM 객체 — 와이어 포맷 → 내부 표현 */
	if (bn == NULL) {
		goto error;
	}

	bld = OSSL_PARAM_BLD_new();
                                  /* [한국어] 동적으로 OSSL_PARAM 배열 빌더 — 정적 params[N] 대신 사용 */
	if (bld == NULL) {
		goto error;
	}
	if (OSSL_PARAM_BLD_push_BN(bld, "pub", bn) != 1) {
                                  /* [한국어] 빌더에 "pub" = bn 푸시 */
		goto error;
	}
	if (OSSL_PARAM_BLD_push_utf8_string(bld, "group", dhgroup, 0) != 1) {
                                  /* [한국어] 빌더에 "group" = dhgroup 문자열 푸시 */
		goto error;
	}

	params = OSSL_PARAM_BLD_to_param(bld);
                                  /* [한국어] 빌더에서 정적 OSSL_PARAM 배열 산출 */
	if (params == NULL) {
		goto error;
	}
	if (EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) != 1) {
                                  /* [한국어] params로부터 PUBLIC_KEY 전용 EVP_PKEY 객체 생성.
                                   *  비공개키 컴포넌트는 없음(상대방 비공개는 알 수 없으니 당연). */
		SPDK_ERRLOG("Failed to create dhkey peer key\n");
		goto error;
	}

	result = EVP_PKEY_dup(key);
                                  /* [한국어] ctx 종속성 끊기 위해 별도 복제 — 호출자가 자유롭게 사용/해제 */
error:
	EVP_PKEY_free(key);
                                  /* [한국어] 임시 객체 해제 — dup된 result는 살아 있음 */
	EVP_PKEY_CTX_free(ctx);
	OSSL_PARAM_BLD_free(bld);
	OSSL_PARAM_free(params);
	BN_free(bn);
                                  /* [한국어] 모든 임시 자원 NULL-safe 해제 */

	return result;
                                  /* [한국어] 성공 시 dup된 키, 실패 시 NULL */
}

/*
 * [한국어]
 * spdk_nvme_dhchap_dhkey_derive_secret - DH 공유 비밀 도출 (사용자 노출 API)
 *
 * @dhkey:  호스트 DH 키페어
 * @peer:   컨트롤러 공개키 바이트
 * @peerlen: peer 길이
 * @secret: [out] DH secret 버퍼
 * @seclen: [in/out] 버퍼 크기 / 실제 secret 길이
 * @return 0 성공, 음수 errno.
 *
 * 동작 (Diffie-Hellman):
 *   1) 호스트 키에서 group 추출 (ffdhe2048 등)
 *   2) peer key를 EVP_PKEY 객체로 복원 (같은 group 사용)
 *   3) derive 컨텍스트 → set_dh_pad(1) → set_peer → derive로 secret 산출.
 *
 *   set_dh_pad(1): leading zero를 자동 패딩 → secret 길이 일관성 (ffdhe2048이면 항상 256B).
 *
 * DH 보안 가정: 같은 그룹의 공개키 두 개 + 자신의 비공개키만으로
 *               s = peer_pub^my_priv mod p = my_pub^peer_priv mod p가 양 측에서 동일하게 도출.
 *
 * 사용처: nvme_auth_send_reply에서 컨트롤러 pubkey와 호스트 키페어로 dhsec 도출.
 */
int
spdk_nvme_dhchap_dhkey_derive_secret(struct spdk_nvme_dhchap_dhkey *dhkey,
				     const void *peer, size_t peerlen, void *secret, size_t *seclen)
{
	EVP_PKEY *key = (EVP_PKEY *)dhkey;
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *peerkey = NULL;
	char dhgroup[64] = {};
                                  /* [한국어] 호스트 키의 그룹 이름 추출 버퍼 — peer key import 시 같은 그룹 지정 */
	int rc = 0;

	if (EVP_PKEY_get_utf8_string_param(key, "group", dhgroup,
					   sizeof(dhgroup), NULL) != 1) {
                                  /* [한국어] 호스트 키에서 group 파라미터 회수 */
		return -EIO;
	}
	peerkey = nvme_auth_get_peerkey(peer, peerlen, dhgroup);
                                  /* [한국어] 같은 그룹으로 peer 공개키 import */
	if (peerkey == NULL) {
		return -EINVAL;
	}
	ctx = EVP_PKEY_CTX_new(key, NULL);
                                  /* [한국어] derive 컨텍스트 — 호스트 키를 base로 */
	if (ctx == NULL) {
		rc = -ENOMEM;
		goto out;
	}
	if (EVP_PKEY_derive_init(ctx) != 1) {
                                  /* [한국어] derive 모드 진입 */
		rc = -EIO;
		goto out;
	}
	if (EVP_PKEY_CTX_set_dh_pad(ctx, 1) <= 0) {
                                  /* [한국어] DH 패딩 활성화 — secret 길이 일관성 보장 (leading zero 패딩) */
		rc = -EIO;
		goto out;
	}
	if (EVP_PKEY_derive_set_peer(ctx, peerkey) != 1) {
                                  /* [한국어] peer 공개키 설정 — derive 입력 */
		SPDK_ERRLOG("Failed to set dhsecret's peer key\n");
		rc = -EINVAL;
		goto out;
	}
	if (EVP_PKEY_derive(ctx, secret, seclen) != 1) {
                                  /* [한국어] 실제 DH 계산 — peer_pub^my_priv mod p */
		SPDK_ERRLOG("Failed to derive dhsecret\n");
		rc = -ENOBUFS;
		goto out;
	}
out:
	EVP_PKEY_free(peerkey);
                                  /* [한국어] 임시로 import된 peer 키 해제 */
	EVP_PKEY_CTX_free(ctx);

	return rc;
}

/*
 * [한국어]
 * nvme_auth_submit_request - AUTH Send/Recv SQE 빌드 + 제출 (내부 헬퍼)
 *
 * @qpair: 대상 qpair
 * @type:  SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND 또는 _RECV
 * @len:   페이로드 길이
 * @return nvme_qpair_submit_request 결과.
 *
 * 동작:
 *   [1] reserved_req 사용 — 일반 free_req 풀이 비어 있어도 인증은 항상 가능해야 함.
 *   [2] fabric_poll_status 초기화 (cpl=0, timeout_tsc=admin_timeout_ms 후, done=false).
 *   [3] NVME_INIT_REQUEST로 cb_fn=nvme_completion_poll_cb 등록 → 응답 도착 시 status->done=true.
 *   [4] type에 따라 AUTH Send 또는 AUTH Recv 캡슐 SQE 빌드:
 *       - 공통: opcode=FABRIC, fctype=type, spsp0=spsp1=1 (security protocol specific),
 *               secp=NVME (NVMe 인증 보안 프로토콜)
 *       - Send: tl = transfer length (페이로드 크기)
 *       - Recv: al = allocation length (수신 가능 최대)
 *   [5] qpair에 제출.
 *
 * 호출자: nvme_auth_recv_message, nvme_auth_send_failure2, nvme_auth_send_negotiate,
 *         nvme_auth_send_reply, nvme_auth_send_success2.
 */
static int
nvme_auth_submit_request(struct spdk_nvme_qpair *qpair,
			 enum spdk_nvmf_fabric_cmd_types type, uint32_t len)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct nvme_request *req = qpair->reserved_req;
                                  /* [한국어] ★ 격리된 reserved_req 사용 — 일반 free_req 풀이 빈 상황에서도
                                   *  인증/CONNECT는 무조건 가능해야 하므로 qpair init 시 1슬롯 보존됨. */
	struct nvme_completion_poll_status *status = qpair->fabric_poll_status;
	struct spdk_nvmf_fabric_auth_recv_cmd rcmd = {};
	struct spdk_nvmf_fabric_auth_send_cmd scmd = {};
                                  /* [한국어] 두 종류 SQE 임시 빌드 영역 — 0 초기화 후 type 분기에서 채움 */

	assert(len <= NVME_AUTH_DATA_SIZE);
                                  /* [한국어] 4KiB DMA 버퍼 초과 금지 — 호출자가 보장해야 함 */
	memset(&status->cpl, 0, sizeof(status->cpl));
                                  /* [한국어] 이전 응답 흔적 클리어 */
	status->timeout_tsc = ctrlr->opts.admin_timeout_ms * spdk_get_ticks_hz() / 1000 +
			      spdk_get_ticks();
                                  /* [한국어] 절대 timeout tick 계산 — admin 타임아웃 ms를 tick으로 환산.
                                   *  nvme_wait_for_completion_poll이 이 값과 현재 tick 비교로 timed_out 판정. */
	status->done = false;
                                  /* [한국어] 응답 도착 플래그 초기화 — completion_poll_cb가 true로 셋 */
	NVME_INIT_REQUEST(req, nvme_completion_poll_cb, status,
			  NVME_PAYLOAD_CONTIG(status->dma_data, NULL), len, 0);
                                  /* [한국어] req 필드 일괄 초기화 — cb_fn=nvme_completion_poll_cb (status 채움),
                                   *  payload=fabric_poll_status->dma_data (4KiB DMA 버퍼), payload_size=len. */
	switch (type) {
	case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND:
                                  /* [한국어] 호스트 → 컨트롤러 메시지 송신 (negotiate, reply, success2, failure2) */
		scmd.opcode = SPDK_NVME_OPC_FABRIC;
                                  /* [한국어] 0x7F — Fabrics Command opcode */
		scmd.fctype = type;
                                  /* [한국어] Fabrics command type — AUTHENTICATION_SEND */
		scmd.spsp0 = 1;
                                  /* [한국어] Security Protocol Specific 0 = 1 (DH-HMAC-CHAP 식별) */
		scmd.spsp1 = 1;
                                  /* [한국어] Security Protocol Specific 1 = 1 */
		scmd.secp = SPDK_NVMF_AUTH_SECP_NVME;
                                  /* [한국어] Security Protocol = NVMe 인증 (다른 프로토콜과 구분) */
		scmd.tl = len;
                                  /* [한국어] Transfer Length — 송신 페이로드 크기 */
		memcpy(&req->cmd, &scmd, sizeof(scmd));
                                  /* [한국어] req->cmd(64B SQE)에 빌드한 scmd 복사 */
		break;
	case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV:
                                  /* [한국어] 컨트롤러 → 호스트 메시지 수신 트리거 (challenge, success1, failure1) */
		rcmd.opcode = SPDK_NVME_OPC_FABRIC;
		rcmd.fctype = type;
		rcmd.spsp0 = 1;
		rcmd.spsp1 = 1;
		rcmd.secp = SPDK_NVMF_AUTH_SECP_NVME;
		rcmd.al = len;
                                  /* [한국어] Allocation Length — 수신 가능 최대 (보통 4KiB) */
		memcpy(&req->cmd, &rcmd, sizeof(rcmd));
		break;
	default:
		assert(0 && "invalid command");
                                  /* [한국어] type 인자가 위 두 enum 외 — 호출자 버그 */
		return -EINVAL;
	}

	return nvme_qpair_submit_request(qpair, req);
                                  /* [한국어] qpair 일반 제출 경로 (nvme_qpair.c)로 위임 → 트랜스포트 → 와이어 송신 */
}

/*
 * [한국어]
 * nvme_auth_recv_message - AUTH Recv 명령 트리거 (수신 버퍼 준비 + 송신)
 *
 * @qpair: 대상 qpair
 * @return nvme_auth_submit_request 결과.
 *
 * 사용처: 호스트가 컨트롤러로부터 메시지를 받아야 할 때 (negotiate 보낸 후 challenge 수신,
 *         reply 보낸 후 success1 수신).
 *
 * 동작:
 *   dma_data 버퍼 0 클리어 → AUTH_RECV 명령 송신 (al=4KiB로 수신 가능 최대 통보).
 *   응답이 도착하면 dma_data에 메시지가 쓰여 있고, 폴링 시 그 위치에서 파싱.
 */
static int
nvme_auth_recv_message(struct spdk_nvme_qpair *qpair)
{
	memset(qpair->fabric_poll_status->dma_data, 0, NVME_AUTH_DATA_SIZE);
                                  /* [한국어] 이전 메시지 흔적 클리어 — 부분 응답 시 stale 데이터로 혼동 방지 */
	return nvme_auth_submit_request(qpair, SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV,
					NVME_AUTH_DATA_SIZE);
                                  /* [한국어] al=4KiB로 최대 크기 수신 요청 */
}

/*
 * [한국어]
 * nvme_auth_send_failure2 - 호스트가 컨트롤러에게 AUTH_failure2 통지
 *
 * @qpair: 대상 qpair
 * @reason: 실패 사유 코드 (SPDK_NVMF_AUTH_*)
 * @return true(송신 성공, 응답 대기 가능) / false(송신 자체 실패).
 *
 * 동작:
 *   dma_data를 failure 메시지로 채움 → COMMON_MESSAGE + FAILURE2 ID + tid + rc=FAILURE + rce=reason →
 *   AUTH Send 송신.
 *
 * 호출자: nvme_auth_check_message, nvme_auth_check_challenge, nvme_auth_check_success1
 *         (각종 검증 실패 경로) — set_failure에 send_failure2 결과를 failure2 인자로 전달.
 */
static bool
nvme_auth_send_failure2(struct spdk_nvme_qpair *qpair, enum spdk_nvmf_auth_failure_reason reason)
{
	struct spdk_nvmf_auth_failure *msg = qpair->fabric_poll_status->dma_data;
	struct nvme_auth *auth = &qpair->auth;

	memset(qpair->fabric_poll_status->dma_data, 0, NVME_AUTH_DATA_SIZE);
                                  /* [한국어] 메시지 영역 클리어 후 failure 헤더만 채움 */
	msg->auth_type = SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE;
                                  /* [한국어] failure는 DH-HMAC-CHAP가 아닌 공통 메시지 타입 */
	msg->auth_id = SPDK_NVMF_AUTH_ID_FAILURE2;
                                  /* [한국어] 메시지 ID = FAILURE2 (호스트 → ctrlr) */
	msg->t_id = auth->tid;
                                  /* [한국어] 진행 중이던 transaction ID — 컨트롤러가 어느 인증인지 매칭 */
	msg->rc = SPDK_NVMF_AUTH_FAILURE;
                                  /* [한국어] reason category = 실패 (스펙 정의 상위 분류) */
	msg->rce = reason;
                                  /* [한국어] reason extended = 상세 사유 (INCORRECT_PAYLOAD 등) */

	return nvme_auth_submit_request(qpair, SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND,
					sizeof(*msg)) == 0;
                                  /* [한국어] 송신 결과 0이면 true(응답 대기 가능), 0 아니면 false(즉시 종료). */
}

/*
 * [한국어]
 * nvme_auth_check_message - 수신 메시지의 type/id 검증 + AUTH_failure1 자동 처리
 *
 * @qpair: 대상 qpair
 * @auth_id: 기대하는 메시지 ID (SPDK_NVMF_AUTH_ID_DHCHAP_*)
 * @return 0 성공(기대 메시지 수신), -EACCES 실패(잘못된 메시지 또는 failure1 수신).
 *
 * 동작 — 3가지 경로:
 *   [1] DHCHAP 메시지 + ID 일치: 정상 — return 0.
 *   [2] DHCHAP 메시지 + ID 불일치: 프로토콜 위반 — failure2 송신 후 -EACCES.
 *   [3] COMMON_MESSAGE + FAILURE1: 컨트롤러가 호스트에게 보낸 실패 통지 — 사유 디코딩 후 설정.
 *       (호스트는 추가 응답 안 함 — 즉시 DONE으로 직행)
 *   [4] 알 수 없는 type: failure2 송신 + 종료.
 *
 * reason 디코딩 테이블: AUTH_FAILED, PROTOCOL_UNUSABLE, SCC_MISMATCH, HASH_UNUSABLE,
 *   DHGROUP_UNUSABLE, INCORRECT_PAYLOAD, INCORRECT_PROTOCOL_MESSAGE.
 */
static int
nvme_auth_check_message(struct spdk_nvme_qpair *qpair, enum spdk_nvmf_auth_id auth_id)
{
	struct spdk_nvmf_auth_failure *msg = qpair->fabric_poll_status->dma_data;
                                  /* [한국어] 일단 failure로 캐스트해서 헤더 부분(auth_type/id) 접근.
                                   *  실제 페이로드는 정상 메시지면 위 호출자가 다시 캐스트해서 사용. */
	const char *reason = NULL;
	const char *reasons[] = {
                                  /* [한국어] failure rce → 사람용 문자열 — 진단 로그 출력에만 사용 */
		[SPDK_NVMF_AUTH_FAILED] = "authentication failed",
		[SPDK_NVMF_AUTH_PROTOCOL_UNUSABLE] = "protocol not usable",
		[SPDK_NVMF_AUTH_SCC_MISMATCH] = "secure channel concatenation mismatch",
		[SPDK_NVMF_AUTH_HASH_UNUSABLE] = "hash not usable",
		[SPDK_NVMF_AUTH_DHGROUP_UNUSABLE] = "dhgroup not usable",
		[SPDK_NVMF_AUTH_INCORRECT_PAYLOAD] = "incorrect payload",
		[SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE] = "incorrect protocol message",
	};

	switch (msg->auth_type) {
	case SPDK_NVMF_AUTH_TYPE_DHCHAP:
                                  /* [한국어] DH-HMAC-CHAP 메시지 — 기대 ID와 일치 검사 */
		if (msg->auth_id == auth_id) {
			return 0;
                                  /* [한국어] 정상 — 호출자가 페이로드 후속 처리 */
		}
		AUTH_ERRLOG(qpair, "received unexpected DH-HMAC-CHAP message id: %u (expected: %u)\n",
			    msg->auth_id, auth_id);
		break;
                                  /* [한국어] 잘못된 ID — break하여 아래 send_failure2 + 종료 분기로 */
	case SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE:
		/* The only common message that we can expect to receive is AUTH_failure1 */
		if (msg->auth_id != SPDK_NVMF_AUTH_ID_FAILURE1) {
                                  /* [한국어] 호스트가 받을 수 있는 공통 메시지는 FAILURE1뿐 (FAILURE2는 호스트가 보내는 것) */
			AUTH_ERRLOG(qpair, "received unexpected common message id: %u\n",
				    msg->auth_id);
			break;
		}
		if (msg->rc == SPDK_NVMF_AUTH_FAILURE && msg->rce < SPDK_COUNTOF(reasons)) {
                                  /* [한국어] reason 코드가 테이블 범위 내면 사람용 문자열로 변환 */
			reason = reasons[msg->rce];
		}
		AUTH_ERRLOG(qpair, "received AUTH_failure1: rc=%d, rce=%d (%s)\n",
			    msg->rc, msg->rce, reason);
		nvme_auth_set_failure(qpair, -EACCES, false);
                                  /* [한국어] 컨트롤러가 이미 실패 통보했으므로 추가 송신 없이 즉시 DONE.
                                   *  failure2=false → AWAIT_FAILURE2 거치지 않음. */
		return -EACCES;
	default:
		AUTH_ERRLOG(qpair, "received unknown message type: %u\n", msg->auth_type);
		break;
                                  /* [한국어] 미정의 type — 프로토콜 위반 */
	}

	nvme_auth_set_failure(qpair, -EACCES,
			      nvme_auth_send_failure2(qpair,
					      SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE));
                                  /* [한국어] 위 break 경로(잘못된 ID 또는 알 수 없는 type) 공통 처리 —
                                   *  컨트롤러에게 INCORRECT_PROTOCOL_MESSAGE 통지 후 종료.
                                   *  send_failure2가 성공하면 AWAIT_FAILURE2 → 응답 후 DONE,
                                   *  실패하면 즉시 DONE. */
	return -EACCES;
}

/*
 * [한국어]
 * nvme_auth_send_negotiate - AUTH_negotiate 메시지 송신 (NEGOTIATE 상태에서 호출)
 *
 * @qpair: 대상 qpair
 * @return nvme_auth_submit_request 결과.
 *
 * 동작:
 *   dma_data를 negotiate 메시지로 채움:
 *     - auth_type = COMMON_MESSAGE, auth_id = NEGOTIATE
 *     - tid = 채번된 transaction ID
 *     - sc_c = SCC_DISABLED (secure channel concatenation 안 함)
 *     - napd = 1 (descriptor 1개 — DH-HMAC-CHAP만)
 *     - descriptor[0]:
 *         auth_id = DHCHAP
 *         hash_id_list[]: g_digests 중 정책 허용된 ID들
 *         dhg_id_list[]: g_dhgroups 중 정책 허용된 ID들
 *         halen, dhlen: 각 리스트 길이
 *
 * 컨트롤러는 이 negotiate를 받고 자기 정책과 교집합에서 hash + dhgroup 1개씩 선택해 challenge로 응답.
 */
static int
nvme_auth_send_negotiate(struct spdk_nvme_qpair *qpair)
{
	struct nvme_auth *auth = &qpair->auth;
	struct spdk_nvmf_auth_negotiate *msg = qpair->fabric_poll_status->dma_data;
	struct spdk_nvmf_auth_descriptor *desc = msg->descriptors;
                                  /* [한국어] descriptors 배열 첫 항목 — DH-HMAC-CHAP descriptor */
	size_t i;

	memset(qpair->fabric_poll_status->dma_data, 0, NVME_AUTH_DATA_SIZE);
                                  /* [한국어] dma_data 4KiB 0 클리어 후 헤더 + descriptor만 채움 */
	desc->auth_id = SPDK_NVMF_AUTH_TYPE_DHCHAP;
                                  /* [한국어] descriptor 타입 = DH-HMAC-CHAP (현재 SPDK 호스트는 이것만 지원) */
	assert(SPDK_COUNTOF(g_digests) <= sizeof(desc->hash_id_list));
	assert(SPDK_COUNTOF(g_dhgroups) <= sizeof(desc->dhg_id_list));
                                  /* [한국어] 컴파일 타임 어림 검증 (런타임 assert) — 정적 테이블이 descriptor 슬롯 초과하지 않도록 */

	for (i = 0; i < SPDK_COUNTOF(g_digests); ++i) {
                                  /* [한국어] 지원 해시 모두 순회 */
		if (!nvme_auth_digest_allowed(qpair, g_digests[i].id)) {
                                  /* [한국어] 사용자 정책(dhchap_digests 비트마스크)에 막힌 해시는 제외 */
			continue;
		}
		AUTH_DEBUGLOG(qpair, "digest: %u (%s)\n", g_digests[i].id,
			      spdk_nvme_dhchap_get_digest_name(g_digests[i].id));
                                  /* [한국어] 디버그: 어떤 해시를 negotiate에 포함하는지 추적 */
		desc->hash_id_list[desc->halen++] = g_digests[i].id;
                                  /* [한국어] 리스트에 ID 추가 + halen 증가 (post-increment) */
	}
	for (i = 0; i < SPDK_COUNTOF(g_dhgroups); ++i) {
                                  /* [한국어] DH 그룹도 같은 패턴 */
		if (!nvme_auth_dhgroup_allowed(qpair, g_dhgroups[i].id)) {
			continue;
		}
		AUTH_DEBUGLOG(qpair, "dhgroup: %u (%s)\n", g_dhgroups[i].id,
			      spdk_nvme_dhchap_get_dhgroup_name(g_dhgroups[i].id));
		desc->dhg_id_list[desc->dhlen++] = g_dhgroups[i].id;
	}

	msg->auth_type = SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE;
                                  /* [한국어] negotiate는 공통 메시지 (DHCHAP 협상 *전*이므로 DHCHAP 타입 사용 안 함) */
	msg->auth_id = SPDK_NVMF_AUTH_ID_NEGOTIATE;
	msg->t_id = auth->tid;
                                  /* [한국어] async 단계에서 채번한 tid */
	msg->sc_c = SPDK_NVMF_AUTH_SCC_DISABLED;
                                  /* [한국어] secure channel concatenation 비활성 — 별도 secure channel 없이 fabric 위에서 인증 */
	msg->napd = 1;
                                  /* [한국어] descriptor 개수 = 1 (DH-HMAC-CHAP 하나만) */

	return nvme_auth_submit_request(qpair, SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND,
					sizeof(*msg) + msg->napd * sizeof(*desc));
                                  /* [한국어] 페이로드 길이 = 헤더 + descriptor 1개 */
}

/*
 * [한국어]
 * nvme_auth_check_challenge - 수신한 DH-HMAC-CHAP_challenge 검증 (AWAIT_CHALLENGE에서 호출)
 *
 * @qpair: 대상 qpair
 * @return 0 정상, -EACCES 검증 실패 (failure2 송신).
 *
 * 검증 항목:
 *   [1] check_message: type=DHCHAP + id=CHALLENGE
 *   [2] tid 일치
 *   [3] seqnum != 0 (스펙 정의)
 *   [4] hash_id 지원 + 길이 일치
 *   [5] dhg_id 유효:
 *       - NULL: dhvlen=0
 *       - ffdhe*: dhvlen>0 + 헤더+hash_len+dhvlen ≤ 4KiB
 *   [6] hash_id가 호스트 정책(dhchap_digests)에 허용
 *   [7] dhg_id가 호스트 정책(dhchap_dhgroups)에 허용
 *
 * 어느 한 단계라도 실패하면 INCORRECT_PAYLOAD로 failure2 송신.
 */
static int
nvme_auth_check_challenge(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvmf_dhchap_challenge *challenge = qpair->fabric_poll_status->dma_data;
	struct nvme_auth *auth = &qpair->auth;
	uint8_t hl;
	int rc;

	rc = nvme_auth_check_message(qpair, SPDK_NVMF_AUTH_ID_DHCHAP_CHALLENGE);
                                  /* [한국어] 1단계: type/id 검증 + failure1 자동 처리 */
	if (rc != 0) {
		return rc;
                                  /* [한국어] check_message가 이미 failure 처리함 — 그대로 반환 */
	}

	if (challenge->t_id != auth->tid) {
                                  /* [한국어] 2단계: 우리가 보낸 negotiate의 tid와 challenge tid 일치 확인 */
		AUTH_ERRLOG(qpair, "unexpected tid: received=%u, expected=%u\n",
			    challenge->t_id, auth->tid);
		goto error;
	}

	if (challenge->seqnum == 0) {
                                  /* [한국어] 3단계: seqnum 0은 invalid */
		AUTH_ERRLOG(qpair, "received challenge with seqnum=0\n");
		goto error;
	}

	hl = spdk_nvme_dhchap_get_digest_length(challenge->hash_id);
                                  /* [한국어] 4단계 - 1: hash_id에 대응하는 표준 길이 조회 */
	if (hl == 0) {
                                  /* [한국어] 0 = 미지원 hash_id */
		AUTH_ERRLOG(qpair, "unsupported hash function: 0x%x\n", challenge->hash_id);
		goto error;
	}

	if (challenge->hl != hl) {
                                  /* [한국어] 4단계 - 2: 메시지의 hl 필드가 표준 길이와 일치해야 함 */
		AUTH_ERRLOG(qpair, "unexpected hash length: received=%u, expected=%u\n",
			    challenge->hl, hl);
		goto error;
	}

	switch (challenge->dhg_id) {
	case SPDK_NVMF_DHCHAP_DHGROUP_NULL:
                                  /* [한국어] 5단계 NULL: dhvlen=0 강제 */
		if (challenge->dhvlen != 0) {
			AUTH_ERRLOG(qpair, "unexpected dhvlen=%u for dhgroup 0\n",
				    challenge->dhvlen);
			goto error;
		}
		break;
	case SPDK_NVMF_DHCHAP_DHGROUP_2048:
	case SPDK_NVMF_DHCHAP_DHGROUP_3072:
	case SPDK_NVMF_DHCHAP_DHGROUP_4096:
	case SPDK_NVMF_DHCHAP_DHGROUP_6144:
	case SPDK_NVMF_DHCHAP_DHGROUP_8192:
                                  /* [한국어] 5단계 ffdhe*: dhvlen>0 + 메시지 전체가 4KiB 내 */
		if (sizeof(*challenge) + hl + challenge->dhvlen > NVME_AUTH_DATA_SIZE ||
		    challenge->dhvlen == 0) {
                                  /* [한국어] 헤더(고정) + cval(hl 바이트) + ctrlr_pubkey(dhvlen) <= 4KiB.
                                   *  dhvlen=0이면 ffdhe인데 키가 없는 모순 → 거부. */
			AUTH_ERRLOG(qpair, "invalid dhvlen=%u for dhgroup %u\n",
				    challenge->dhvlen, challenge->dhg_id);
			goto error;
		}
		break;
	default:
                                  /* [한국어] 5단계 기타: 미지원 dhgroup */
		AUTH_ERRLOG(qpair, "unsupported dhgroup: 0x%x\n", challenge->dhg_id);
		goto error;
	}

	if (!nvme_auth_digest_allowed(qpair, challenge->hash_id)) {
                                  /* [한국어] 6단계: 호스트 정책에 허용된 hash인지 (negotiate에 포함된 것 중 컨트롤러가 골랐어야 함) */
		AUTH_ERRLOG(qpair, "received disallowed digest: %u (%s)\n", challenge->hash_id,
			    spdk_nvme_dhchap_get_digest_name(challenge->hash_id));
		goto error;
	}

	if (!nvme_auth_dhgroup_allowed(qpair, challenge->dhg_id)) {
                                  /* [한국어] 7단계: 호스트 정책에 허용된 dhgroup인지 */
		AUTH_ERRLOG(qpair, "received disallowed dhgroup: %u (%s)\n", challenge->dhg_id,
			    spdk_nvme_dhchap_get_dhgroup_name(challenge->dhg_id));
		goto error;
	}

	return 0;
                                  /* [한국어] 모든 검증 통과 — challenge 사용 가능 */
error:
	nvme_auth_set_failure(qpair, -EACCES,
			      nvme_auth_send_failure2(qpair, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD));
                                  /* [한국어] 검증 실패 공통 — INCORRECT_PAYLOAD로 failure2 송신.
                                   *  성공 시 AWAIT_FAILURE2 → 응답 후 DONE, 실패 시 즉시 DONE. */
	return -EACCES;
}

/*
 * [한국어]
 * nvme_auth_send_reply - ★ 핵심 ★ DH-HMAC-CHAP_reply 빌드 + 송신 (AWAIT_CHALLENGE에서 호출)
 *
 * @qpair: 대상 qpair (challenge가 fabric_poll_status->dma_data에 도착한 상태)
 * @return 0 성공, 음수 errno.
 *
 * 7단계 동작:
 *   [1] auth->hash 저장 (이후 success1 검증 시 사용).
 *   [2] dhgroup != NULL이면 DH 키페어 생성 + 호스트 pubkey 추출 + DH secret 도출.
 *   [3] keyring에서 dhchap_key + (옵션) dhchap_ctrlr_key 회수.
 *   [4] spdk_nvme_dhchap_calculate으로 호스트 응답 rval 계산
 *       (type="HostHost", nqn1=hostnqn, nqn2=subnqn, cval=challenge->cval)
 *   [5] dhchap_ctrlr_key 있으면 (상호 인증 모드):
 *       - seqnum 채번
 *       - 호스트 측 ctrlr_challenge nonce 임의 생성 (RAND_bytes)
 *       - spdk_nvme_dhchap_calculate으로 컨트롤러 기대 응답 미리 계산
 *         (type="Controller", nqn1=subnqn, nqn2=hostnqn) — auth->challenge에 보존
 *   [6] reply 메시지 빌드:
 *       - rval[0..hl] = 호스트 응답
 *       - rval[hl..2hl] = ctrlr_challenge (상호 인증 시) 또는 0 (단방향)
 *       - rval[2hl..2hl+publen] = 호스트 DH pubkey
 *       - 헤더: tid, hl, cvalid(상호 인증 여부), dhvlen(=publen), seqnum
 *   [7] 송신.
 *
 * 메모리: keybuf/keyring 핸들은 함수 끝에서 spdk_keyring_put_key.
 *         response/dhsec/ctrlr_challenge는 스택 → 함수 종료 시 자동 클리어.
 *         (보안상 stack은 후속 함수에서 덮여 흔적 남지만, 인증 직후 일반 I/O로 빠르게 덮임.)
 */
static int
nvme_auth_send_reply(struct spdk_nvme_qpair *qpair)
{
	struct nvme_completion_poll_status *status = qpair->fabric_poll_status;
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvmf_dhchap_challenge *challenge = status->dma_data;
                                  /* [한국어] 같은 dma_data를 challenge로 읽고 reply로 덮어쓰는 in-place 처리 */
	struct spdk_nvmf_dhchap_reply *reply = status->dma_data;
	struct nvme_auth *auth = &qpair->auth;
	struct spdk_nvme_dhchap_dhkey *dhkey;
	struct spdk_key *key = NULL, *ckey = NULL;
                                  /* [한국어] PSK 핸들 + 컨트롤러 키 핸들 (상호 인증 시) */
	uint8_t hl, response[NVME_AUTH_DATA_SIZE];
                                  /* [한국어] response: 임시 호스트 응답 — 나중에 reply->rval로 복사 */
	uint8_t pubkey[NVME_AUTH_DH_KEY_MAX_SIZE];
                                  /* [한국어] 호스트 DH pubkey 임시 — 나중에 reply에 복사 */
	uint8_t dhsec[NVME_AUTH_DH_KEY_MAX_SIZE];
                                  /* [한국어] DH secret 임시 — 응답 계산에만 사용 */
	uint8_t ctrlr_challenge[NVME_AUTH_DIGEST_MAX_SIZE] = {};
                                  /* [한국어] 호스트가 컨트롤러에게 보낼 challenge nonce (상호 인증 시) */
	size_t dhseclen = 0, publen = 0;
	uint32_t seqnum = 0;
	int rc;

	auth->hash = challenge->hash_id;
                                  /* [한국어] 컨트롤러가 선택한 해시 보존 — success1 검증에 같은 길이 사용 */
	hl = spdk_nvme_dhchap_get_digest_length(challenge->hash_id);
                                  /* [한국어] 다이제스트 길이 — 다양한 오프셋 계산에 사용 */
	if (challenge->dhg_id != SPDK_NVMF_DHCHAP_DHGROUP_NULL) {
                                  /* [한국어] DH 사용 모드 — 키페어 생성 + secret 도출 */
		dhseclen = sizeof(dhsec);
                                  /* [한국어] derive_secret 호출 시 IN/OUT — 실제 길이로 갱신됨 */
		publen = sizeof(pubkey);
                                  /* [한국어] get_pubkey 호출 시 IN/OUT */
		AUTH_LOGDUMP("ctrlr pubkey:", &challenge->cval[hl], challenge->dhvlen);
                                  /* [한국어] 디버그: 컨트롤러 pubkey가 cval[hl] 위치에 있음 (challenge 레이아웃) */
		dhkey = spdk_nvme_dhchap_generate_dhkey(
				(enum spdk_nvmf_dhchap_dhgroup)challenge->dhg_id);
                                  /* [한국어] 컨트롤러가 선택한 그룹으로 호스트 키페어 생성 */
		if (dhkey == NULL) {
			rc = -EINVAL;
			goto out;
		}
		rc = spdk_nvme_dhchap_dhkey_get_pubkey(dhkey, pubkey, &publen);
                                  /* [한국어] 호스트 pubkey 추출 → 나중에 reply에 포함하여 컨트롤러에 송신 */
		if (rc != 0) {
			spdk_nvme_dhchap_dhkey_free(&dhkey);
			goto out;
		}
		AUTH_LOGDUMP("host pubkey:", pubkey, publen);
		rc = spdk_nvme_dhchap_dhkey_derive_secret(dhkey,
				&challenge->cval[hl], challenge->dhvlen, dhsec, &dhseclen);
                                  /* [한국어] DH secret 도출 — 호스트 priv + ctrlr pub → secret.
                                   *  컨트롤러도 ctrlr priv + 우리가 보낼 host pub → 같은 secret 도출. */
		spdk_nvme_dhchap_dhkey_free(&dhkey);
                                  /* [한국어] DH 키페어는 secret 도출 후 즉시 폐기 — secret만 남으면 됨 */
		if (rc != 0) {
			goto out;
		}

		AUTH_LOGDUMP("dh secret:", dhsec, dhseclen);
                                  /* [한국어] 디버그: secret 출력. 운영 빌드에서는 끄거나 컴파일 아웃 권장. */
	}

	nvme_ctrlr_lock(ctrlr);
                                  /* [한국어] dhchap_key/ctrlr_key는 ctrlr->opts에 저장 — 동시 변경 보호 */
	key = ctrlr->opts.dhchap_key ? spdk_key_dup(ctrlr->opts.dhchap_key) : NULL;
                                  /* [한국어] PSK 키 핸들 dup (락 해제 후에도 사용하기 위해). NULL이면 인증 불가 */
	ckey = ctrlr->opts.dhchap_ctrlr_key ? spdk_key_dup(ctrlr->opts.dhchap_ctrlr_key) : NULL;
                                  /* [한국어] 컨트롤러 키 (상호 인증 모드일 때만 설정됨) */
	nvme_ctrlr_unlock(ctrlr);

	AUTH_DEBUGLOG(qpair, "key=%s, hash=%u, dhgroup=%u, seq=%u, tid=%u, subnqn=%s, hostnqn=%s, "
		      "len=%u\n", spdk_key_get_name(key), challenge->hash_id, challenge->dhg_id,
		      challenge->seqnum, auth->tid, ctrlr->trid.subnqn, ctrlr->opts.hostnqn, hl);
                                  /* [한국어] 디버그: 모든 인증 파라미터 한꺼번에 출력 */
	rc = spdk_nvme_dhchap_calculate(key, (enum spdk_nvmf_dhchap_hash)challenge->hash_id,
					"HostHost", challenge->seqnum, auth->tid, 0,
					ctrlr->opts.hostnqn, ctrlr->trid.subnqn,
					dhseclen > 0 ? dhsec : NULL, dhseclen,
					challenge->cval, response);
                                  /* [한국어] 호스트 응답 rval 계산:
                                   *  - type="HostHost": 호스트→컨트롤러 인증
                                   *  - seqnum/tid: 컨트롤러가 보낸 challenge에서 가져온 값
                                   *  - scc=0: secure channel concatenation 비활성
                                   *  - nqn1/nqn2: hostnqn 먼저, subnqn 두번째
                                   *  - dhsec: DH 사용 시 보강, NULL이면 평문 cval만 사용
                                   *  - 결과: response 버퍼에 hl 바이트 응답 */
	if (rc != 0) {
		AUTH_ERRLOG(qpair, "failed to calculate response: %s\n", spdk_strerror(-rc));
		goto out;
	}

	if (ckey != NULL) {
                                  /* [한국어] 상호 인증 모드 — 컨트롤러도 우리에게 자기 신원을 증명해야 함.
                                   *  미리 컨트롤러가 보낼 응답을 계산해두고, success1에서 비교. */
		seqnum = nvme_auth_get_seqnum(qpair);
                                  /* [한국어] 호스트가 컨트롤러용 challenge에 사용할 seqnum 채번 */
		if (seqnum == 0) {
                                  /* [한국어] RAND_bytes 실패 등 */
			rc = -EIO;
			goto out;
		}

		assert(sizeof(ctrlr_challenge) >= hl);
		rc = RAND_bytes(ctrlr_challenge, hl);
                                  /* [한국어] 컨트롤러용 challenge nonce 생성 — 매 세션 새 임의값 */
		if (rc != 1) {
			rc = -EIO;
			goto out;
		}

		rc = spdk_nvme_dhchap_calculate(ckey,
						(enum spdk_nvmf_dhchap_hash)challenge->hash_id,
						"Controller", seqnum, auth->tid, 0,
						ctrlr->trid.subnqn, ctrlr->opts.hostnqn,
						dhseclen > 0 ? dhsec : NULL, dhseclen,
						ctrlr_challenge, auth->challenge);
                                  /* [한국어] 컨트롤러가 보낼 응답을 우리가 미리 계산:
                                   *  - type="Controller": 컨트롤러→호스트 방향
                                   *  - nqn 순서 반대: subnqn 먼저, hostnqn 두번째 (스펙 정의)
                                   *  - cval = ctrlr_challenge (우리가 새로 만든 nonce)
                                   *  - 결과: auth->challenge에 hl 바이트 — success1 수신 후 컨트롤러 응답과 memcmp */
		if (rc != 0) {
			AUTH_ERRLOG(qpair, "failed to calculate controller's response: %s\n",
				    spdk_strerror(-rc));
			goto out;
		}
	}

	/* Now that the response has been calculated, send the reply */
	memset(qpair->fabric_poll_status->dma_data, 0, NVME_AUTH_DATA_SIZE);
                                  /* [한국어] dma_data를 reply 메시지로 새로 채우기 위해 클리어 (이전 challenge 흔적 제거) */
	assert(sizeof(*reply) + 2 * hl + publen <= NVME_AUTH_DATA_SIZE);
                                  /* [한국어] 헤더 + 2*hl(rval+ctrlr_challenge) + publen(host pubkey) <= 4KiB */
	memcpy(reply->rval, response, hl);
                                  /* [한국어] [0, hl): 호스트 응답 */
	memcpy(&reply->rval[1 * hl], ctrlr_challenge, hl);
                                  /* [한국어] [hl, 2hl): 컨트롤러용 challenge nonce.
                                   *  cvalid=0이어도 항상 채워야 함 (스펙 — 양 슬롯 모두 필수). */
	memcpy(&reply->rval[2 * hl], pubkey, publen);
                                  /* [한국어] [2hl, 2hl+publen): 호스트 DH pubkey */

	reply->auth_type = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	reply->auth_id = SPDK_NVMF_AUTH_ID_DHCHAP_REPLY;
	reply->t_id = auth->tid;
	reply->hl = hl;
                                  /* [한국어] 다이제스트 길이 — 컨트롤러가 reply 파싱 시 사용 */
	reply->cvalid = ckey != NULL;
                                  /* [한국어] 상호 인증 모드 여부 — 컨트롤러가 ctrlr_challenge 사용 여부 결정 */
	reply->dhvlen = publen;
                                  /* [한국어] DH 미사용 시 publen=0 → dhvlen=0 */
	reply->seqnum = seqnum;
                                  /* [한국어] 컨트롤러용 challenge의 seqnum (cvalid=0이면 0) */

	/* The 2 * reply->hl below is because the spec says that both rval[hl] and cval[hl] must
	 * always be part of the reply message, even cvalid is zero.
	 */
	rc = nvme_auth_submit_request(qpair, SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND,
				      sizeof(*reply) + 2 * reply->hl + publen);
                                  /* [한국어] 페이로드 길이 = 헤더 + rval(hl) + ctrlr_challenge(hl) + pubkey(publen).
                                   *  cvalid=0여도 ctrlr_challenge 슬롯은 0으로 채워 보냄. */
out:
	spdk_keyring_put_key(key);
                                  /* [한국어] dup된 키 핸들 반환 — keyring에 ref count 감소 */
	spdk_keyring_put_key(ckey);
                                  /* [한국어] 컨트롤러 키 (NULL 안전) */

	return rc;
}

/*
 * [한국어]
 * nvme_auth_check_success1 - 수신한 DH-HMAC-CHAP_success1 검증 (AWAIT_SUCCESS1에서 호출)
 *
 * @qpair: 대상 qpair (success1이 fabric_poll_status->dma_data에 도착)
 * @return 0 정상, -EACCES 검증 실패 (failure2 송신 후).
 *
 * 검증 항목:
 *   [1] check_message: type=DHCHAP + id=SUCCESS1 + (failure1이면 자동 처리)
 *   [2] tid 일치
 *   [3] dhchap_ctrlr_key가 있다면 (상호 인증 모드):
 *       - rvalid=1 (컨트롤러가 응답 포함)
 *       - hl 일치
 *       - msg->rval == auth->challenge (send_reply에서 미리 계산해둔 기대값)
 */
static int
nvme_auth_check_success1(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvmf_dhchap_success1 *msg = qpair->fabric_poll_status->dma_data;
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct nvme_auth *auth = &qpair->auth;
	uint8_t hl;
	int rc, status;

	rc = nvme_auth_check_message(qpair, SPDK_NVMF_AUTH_ID_DHCHAP_SUCCESS1);
                                  /* [한국어] 1단계: type/id 검증 + failure1 자동 처리 */
	if (rc != 0) {
		return rc;
	}

	if (msg->t_id != auth->tid) {
                                  /* [한국어] 2단계: tid 일치 — transaction 매칭 */
		AUTH_ERRLOG(qpair, "unexpected tid: received=%u, expected=%u\n",
			    msg->t_id, auth->tid);
		status = SPDK_NVMF_AUTH_INCORRECT_PAYLOAD;
		goto error;
	}

	if (ctrlr->opts.dhchap_ctrlr_key != NULL) {
                                  /* [한국어] 상호 인증 모드 — 컨트롤러 응답을 검증해야 함 */
		if (!msg->rvalid) {
                                  /* [한국어] 우리는 컨트롤러 응답을 기대했는데 컨트롤러가 안 보냈음 */
			AUTH_ERRLOG(qpair, "received rvalid=0, expected response\n");
			status = SPDK_NVMF_AUTH_INCORRECT_PAYLOAD;
			goto error;
		}

		hl = spdk_nvme_dhchap_get_digest_length(auth->hash);
                                  /* [한국어] reply 시 저장한 hash로 길이 산출 */
		if (msg->hl != hl) {
                                  /* [한국어] 메시지의 hl이 우리가 알고 있는 hash 길이와 일치해야 함 */
			AUTH_ERRLOG(qpair, "received invalid hl=%u, expected=%u\n", msg->hl, hl);
			status = SPDK_NVMF_AUTH_INCORRECT_PAYLOAD;
			goto error;
		}

		if (memcmp(msg->rval, auth->challenge, hl) != 0) {
                                  /* [한국어] ★ 컨트롤러 인증의 정점 ★
                                   *  send_reply에서 우리가 미리 계산해둔 기대 응답(auth->challenge)과
                                   *  컨트롤러가 실제로 보낸 응답(msg->rval) 비교.
                                   *  같으면 컨트롤러가 진짜 같은 PSK를 갖고 있다는 증명. */
			AUTH_ERRLOG(qpair, "controller challenge mismatch\n");
			AUTH_LOGDUMP("received:", msg->rval, hl);
			AUTH_LOGDUMP("expected:", auth->challenge, hl);
			status = SPDK_NVMF_AUTH_FAILED;
                                  /* [한국어] AUTH_FAILED — 신원 증명 실패 (가짜 컨트롤러일 가능성) */
			goto error;
		}
	}

	return 0;
                                  /* [한국어] 검증 통과 — 인증 성공 */
error:
	nvme_auth_set_failure(qpair, -EACCES, nvme_auth_send_failure2(qpair, status));
                                  /* [한국어] 실패 사유에 따라 failure2 송신 + 종료 */

	return -EACCES;
}

/*
 * [한국어]
 * nvme_auth_send_success2 - 호스트 측 검증 완료 통지 (상호 인증 모드 한정)
 *
 * @qpair: 대상 qpair
 * @return nvme_auth_submit_request 결과.
 *
 * 사용처: success1에서 컨트롤러 응답 검증 완료 후, 컨트롤러에게 "잘 받았고 너도 인증됨" 통지.
 *         단방향 인증(dhchap_ctrlr_key=NULL)에서는 success1만으로 종료, 이 함수 호출 안 됨.
 */
static int
nvme_auth_send_success2(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvmf_dhchap_success2 *msg = qpair->fabric_poll_status->dma_data;
	struct nvme_auth *auth = &qpair->auth;

	memset(qpair->fabric_poll_status->dma_data, 0, NVME_AUTH_DATA_SIZE);
                                  /* [한국어] dma_data 클리어 후 success2 헤더만 채움 */
	msg->auth_type = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	msg->auth_id = SPDK_NVMF_AUTH_ID_DHCHAP_SUCCESS2;
	msg->t_id = auth->tid;
                                  /* [한국어] 동일 transaction 매칭용 tid */

	return nvme_auth_submit_request(qpair, SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND,
					sizeof(*msg));
                                  /* [한국어] 헤더만 송신 — 페이로드 없음 */
}

/*
 * [한국어]
 * nvme_fabric_qpair_authenticate_poll - ★★★ 인증 상태머신 driver ★★★
 *
 * @qpair: 대상 qpair
 * @return 0(성공 종료), 음수(실패 종료), -EAGAIN(아직 진행 중).
 *
 * 호출 컨텍스트:
 *   - nvme_fabric_qpair_authenticate_async가 인증 시작 직후 1회 호출 (kick-start)
 *   - 이후 트랜스포트의 process_completions가 매 polling tick마다 호출
 *
 * 8 상태 머신 (NVMe-oF 스펙 8.13 Authentication Sequence):
 *   NEGOTIATE          : 초기 상태 — send_negotiate → AWAIT_NEGOTIATE
 *   AWAIT_NEGOTIATE    : negotiate 완료 응답 대기 → recv_message → AWAIT_CHALLENGE
 *   AWAIT_CHALLENGE    : challenge 수신 대기 → check + send_reply → AWAIT_REPLY
 *   AWAIT_REPLY        : reply 완료 응답 대기 → recv_message → AWAIT_SUCCESS1
 *   AWAIT_SUCCESS1     : success1 수신 대기 → check
 *                          상호인증 모드(ctrlr_key!=NULL): send_success2 → AWAIT_SUCCESS2
 *                          단방향                       : DONE
 *   AWAIT_SUCCESS2     : success2 완료 응답 대기 → DONE
 *   AWAIT_FAILURE2     : failure2 완료 응답 대기 → DONE
 *   DONE               : cleanup + cb_fn 호출 + 반환
 *
 * 재진입 방지: auth->flags.in_auth_poll로 같은 함수 재호출 거부
 *              (예: cb_fn 안에서 process_completions 다시 호출하는 케이스).
 *
 * 진행 루프 (do-while): prev_state와 비교해 상태 전이가 있을 때만 다음 단계 즉시 진행.
 *                       일이 없으면(같은 상태 유지) 루프 종료 후 -EAGAIN 반환.
 *                       이 패턴으로 한 번의 폴링 호출에서 여러 즉시-실행 단계(예: NEGOTIATE→AWAIT_NEGOTIATE)
 *                       를 한꺼번에 진행 가능.
 *
 * 각 AWAIT_* 상태:
 *   nvme_wait_for_completion_poll로 응답 도착 검사:
 *     - -EAGAIN: 아직 안 옴 → break하여 -EAGAIN 반환
 *     - 음수 외: completion 자체 에러 → print_cpl + set_failure
 *     - 0: 성공 → 다음 단계 진행
 */
int
nvme_fabric_qpair_authenticate_poll(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct nvme_auth *auth = &qpair->auth;
	struct nvme_completion_poll_status *status = qpair->fabric_poll_status;
	enum nvme_qpair_auth_state prev_state;
                                  /* [한국어] do-while 루프에서 진전 여부 판단용 */
	int rc;

	if (auth->flags.in_auth_poll) {
                                  /* [한국어] 재진입 방지 — 같은 함수가 재귀 호출되면 -EAGAIN.
                                   *  cb_fn 콜백 안에서 process_completions를 호출하는 경우 등 방어. */
		return -EAGAIN;
	}

	auth->flags.in_auth_poll = true;
                                  /* [한국어] 재진입 가드 활성화 */

	do {
		prev_state = auth->state;
                                  /* [한국어] 매 반복마다 진입 시점 상태 보존 — set_state로 변경되면 루프 한 번 더 */

		switch (auth->state) {
		case NVME_QPAIR_AUTH_STATE_NEGOTIATE:
                                  /* [한국어] 초기 상태 — negotiate 메시지 송신 */
			rc = nvme_auth_send_negotiate(qpair);
			if (rc != 0) {
                                  /* [한국어] 송신 자체 실패 — failure 통지 안 가능, 즉시 종료 */
				nvme_auth_set_failure(qpair, rc, false);
				AUTH_ERRLOG(qpair, "failed to send AUTH_negotiate: %s\n",
					    spdk_strerror(-rc));
				break;
			}
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_AWAIT_NEGOTIATE);
			/* Intentionally return here to prevent the state machine entering the DONE
			 * state on the initial kick from nvme_fabric_qpair_authenticate_async. */
			auth->flags.in_auth_poll = false;
			return -EAGAIN;
                                  /* [한국어] ★ 초기 kick 시 의도적 early return ★
                                   *  authenticate_async가 첫 polling을 호출하는데, 만약 luôn 진행시키면
                                   *  do-while이 계속 돌면서 응답 도착 전인데 다음 단계로 가버릴 수 있음.
                                   *  return -EAGAIN로 명시적으로 다음 polling tick 대기. */
		case NVME_QPAIR_AUTH_STATE_AWAIT_NEGOTIATE:
                                  /* [한국어] negotiate 송신 완료 응답 대기 (AUTH Send completion) */
			rc = nvme_wait_for_completion_poll(qpair, status);
			if (rc != 0) {
				if (rc != -EAGAIN) {
                                  /* [한국어] 응답 자체에 에러 (sct/sc != 0) — 진단 + 종료 */
					nvme_auth_print_cpl(qpair, "AUTH_negotiate");
					nvme_auth_set_failure(qpair, rc, false);
				}
				break;
                                  /* [한국어] -EAGAIN이면 그냥 break — 다음 폴링 시도 */
			}
			/* Negotiate has been sent, try to receive the challenge */
			rc = nvme_auth_recv_message(qpair);
                                  /* [한국어] negotiate 송신 완료 → 이제 challenge 수신 시작 */
			if (rc != 0) {
				nvme_auth_set_failure(qpair, rc, false);
				AUTH_ERRLOG(qpair, "failed to recv DH-HMAC-CHAP_challenge: %s\n",
					    spdk_strerror(-rc));
				break;
			}
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_AWAIT_CHALLENGE);
                                  /* [한국어] 다음 상태 — challenge 응답 대기 */
			break;
		case NVME_QPAIR_AUTH_STATE_AWAIT_CHALLENGE:
                                  /* [한국어] challenge 수신 응답 대기 (AUTH Recv completion + payload 도착) */
			rc = nvme_wait_for_completion_poll(qpair, status);
			if (rc != 0) {
				if (rc != -EAGAIN) {
					nvme_auth_print_cpl(qpair, "DH-HMAC-CHAP_challenge");
					nvme_auth_set_failure(qpair, rc, false);
				}
				break;
			}
			rc = nvme_auth_check_challenge(qpair);
                                  /* [한국어] challenge 페이로드 검증 — 실패 시 set_failure 자동 호출 */
			if (rc != 0) {
				break;
                                  /* [한국어] check_challenge가 이미 failure 처리함 */
			}
			rc = nvme_auth_send_reply(qpair);
                                  /* [한국어] ★ 핵심 ★ DH 도출 + HMAC 계산 + reply 송신 */
			if (rc != 0) {
				nvme_auth_set_failure(qpair, rc, false);
				AUTH_ERRLOG(qpair, "failed to send DH-HMAC-CHAP_reply: %s\n",
					    spdk_strerror(-rc));
				break;
			}
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_AWAIT_REPLY);
			break;
		case NVME_QPAIR_AUTH_STATE_AWAIT_REPLY:
                                  /* [한국어] reply 송신 완료 응답 대기 */
			rc = nvme_wait_for_completion_poll(qpair, status);
			if (rc != 0) {
				if (rc != -EAGAIN) {
					nvme_auth_print_cpl(qpair, "DH-HMAC-CHAP_reply");
					nvme_auth_set_failure(qpair, rc, false);
				}
				break;
			}
			/* Reply has been sent, try to receive response */
			rc = nvme_auth_recv_message(qpair);
                                  /* [한국어] reply 송신 완료 → success1 수신 시작 */
			if (rc != 0) {
				nvme_auth_set_failure(qpair, rc, false);
				AUTH_ERRLOG(qpair, "failed to recv DH-HMAC-CHAP_success1: %s\n",
					    spdk_strerror(-rc));
				break;
			}
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS1);
			break;
		case NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS1:
                                  /* [한국어] success1 수신 응답 대기 */
			rc = nvme_wait_for_completion_poll(qpair, status);
			if (rc != 0) {
				if (rc != -EAGAIN) {
					nvme_auth_print_cpl(qpair, "DH-HMAC-CHAP_success1");
					nvme_auth_set_failure(qpair, rc, false);
				}
				break;
			}
			rc = nvme_auth_check_success1(qpair);
                                  /* [한국어] success1 페이로드 검증 (상호 인증 모드면 ctrlr 응답까지) */
			if (rc != 0) {
				break;
			}
			AUTH_DEBUGLOG(qpair, "authentication completed successfully\n");
			if (ctrlr->opts.dhchap_ctrlr_key != NULL) {
                                  /* [한국어] 상호 인증 모드 — success2 송신해 컨트롤러에게 잘 받았다 통지 */
				rc = nvme_auth_send_success2(qpair);
				if (rc != 0) {
					AUTH_ERRLOG(qpair, "failed to send DH-HMAC-CHAP_success2: "
						    "%s\n", spdk_strerror(rc));
                                  /* [한국어] 주의: 위 다른 곳은 spdk_strerror(-rc), 여기는 spdk_strerror(rc).
                                   *  rc는 음수일 텐데 spdk_strerror는 양수 errno 기대 → 출력 형식 미세 버그.
                                   *  코드 수정은 안 하고 그대로 둠 (CLAUDE.md 원칙). */
					nvme_auth_set_failure(qpair, rc, false);
					break;
				}
				nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS2);
				break;
			}
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_DONE);
                                  /* [한국어] 단방향 인증 완료 — 추가 메시지 없이 DONE */
			break;
		case NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS2:
		case NVME_QPAIR_AUTH_STATE_AWAIT_FAILURE2:
                                  /* [한국어] 두 상태가 같은 처리: 마지막 송신의 응답만 받고 DONE 으로 ↓.
                                   *  성공 경로(SUCCESS2)와 실패 경로(FAILURE2) 모두 응답 도착 확인 후 DONE. */
			rc = nvme_wait_for_completion_poll(qpair, status);
			if (rc == -EAGAIN) {
				break;
                                  /* [한국어] 아직 응답 안 옴 — 다음 폴링까지 대기 */
			}
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_DONE);
                                  /* [한국어] 응답 도착 (성공/실패 무관) → DONE.
                                   *  실패 응답이어도 이미 set_failure로 status 보존되어 있음. */
			break;
		case NVME_QPAIR_AUTH_STATE_DONE:
                                  /* [한국어] 종료 상태 — cleanup + cb_fn 호출 + 결과 반환 */
			nvme_fabric_qpair_poll_cleanup(qpair);
                                  /* [한국어] fabric_poll_status 정리 (nvme_fabric.c) */
			nvme_fabric_qpair_auth_cleanup(qpair, auth->status);
                                  /* [한국어] auth.cb_fn 호출 + auth 상태 리셋 */
			auth->flags.in_auth_poll = false;
			return auth->status;
                                  /* [한국어] 누적 status 반환 — 0=성공, 음수=실패 */
		default:
			assert(0 && "invalid state");
                                  /* [한국어] 미정의 상태 — 코드 버그 */
			auth->flags.in_auth_poll = false;
			return -EINVAL;
		}
	} while (auth->state != prev_state);
                                  /* [한국어] 상태 전이가 있었으면 다음 단계 즉시 시도. 같은 상태면 대기. */

	auth->flags.in_auth_poll = false;
	return -EAGAIN;
                                  /* [한국어] 진행 중 — 다음 폴링 사이클에서 재시도 */
}

/*
 * [한국어]
 * nvme_fabric_qpair_authenticate_async - 인증 시작 (트랜스포트 콜백 또는 자동 트리거에서 호출)
 *
 * @qpair: 대상 qpair (이미 connect 완료, atr=1 등으로 인증 필요 판단됨)
 * @return 0(상태머신 시작 성공), 음수 errno.
 *
 * 동작:
 *   [1] dhchap_key 검사 — 키 없으면 인증 불가능 → -ENOKEY.
 *   [2] ascr 검사 — secure channel concatenation 미지원 → -EINVAL.
 *   [3] auth tracker(nvme_completion_poll_status) calloc.
 *   [4] DMA 4KiB 메시지 버퍼 spdk_zmalloc.
 *   [5] qpair->fabric_poll_status에 저장 (이전 connect 시 NULL이어야 함).
 *   [6] tid 채번 (ctrlr 단위 단조증가 카운터).
 *   [7] 상태 = NEGOTIATE.
 *   [8] 첫 polling으로 NEGOTIATE 메시지 송신 트리거 (kick-start).
 *
 * 첫 polling이 -EAGAIN을 반환하면 0으로 변환 (정상 시작).
 * 다른 음수면 그대로 전파.
 */
int
nvme_fabric_qpair_authenticate_async(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct nvme_completion_poll_status *status;
	struct nvme_auth *auth = &qpair->auth;
	int rc;

	if (ctrlr->opts.dhchap_key == NULL) {
                                  /* [한국어] PSK 키가 없으면 DH-HMAC-CHAP 자체 불가능 */
		AUTH_ERRLOG(qpair, "missing DH-HMAC-CHAP key\n");
		return -ENOKEY;
	}

	if (qpair->auth.flags.ascr) {
                                  /* [한국어] secure channel concatenation 요구되었으나 SPDK 호스트는 미지원 */
		AUTH_ERRLOG(qpair, "secure channel concatenation is not supported\n");
		return -EINVAL;
	}

	status = calloc(1, sizeof(*qpair->fabric_poll_status));
                                  /* [한국어] poll status 구조체 — completion 추적용 */
	if (!status) {
		AUTH_ERRLOG(qpair, "failed to allocate poll status\n");
		return -ENOMEM;
	}

	status->dma_data = spdk_zmalloc(NVME_AUTH_DATA_SIZE, 0, NULL, SPDK_ENV_LCORE_ID_ANY,
					SPDK_MALLOC_DMA);
                                  /* [한국어] AUTH 메시지 송수신용 4KiB DMA 버퍼.
                                   *  SPDK_MALLOC_DMA: hugepage + DMA addressable, NUMA local. */
	if (!status->dma_data) {
		AUTH_ERRLOG(qpair, "failed to allocate poll status\n");
		free(status);
                                  /* [한국어] 부분 실패 — calloc된 status 누수 방지 */
		return -ENOMEM;
	}

	assert(qpair->fabric_poll_status == NULL);
                                  /* [한국어] connect 시 NULL이어야 정상 — 이전 인증/connect의 잔여물이 있으면 버그 */
	qpair->fabric_poll_status = status;

	nvme_ctrlr_lock(ctrlr);
	auth->tid = ctrlr->auth_tid++;
                                  /* [한국어] tid 채번 — 컨트롤러 단위 단조증가 (post-increment).
                                   *  같은 ctrlr의 여러 qpair가 순서대로 다른 tid 보장. */
	nvme_ctrlr_unlock(ctrlr);

	nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_NEGOTIATE);
                                  /* [한국어] 초기 상태 — 첫 polling이 negotiate 송신 시작 */

	/* Do the initial poll to kick-start the state machine */
	rc = nvme_fabric_qpair_authenticate_poll(qpair);
                                  /* [한국어] kick-start polling — NEGOTIATE → 송신 시도 → AWAIT_NEGOTIATE → return -EAGAIN */
	return rc != -EAGAIN ? rc : 0;
                                  /* [한국어] -EAGAIN은 정상 시작 (응답 대기 중) → 0으로 정규화.
                                   *  진짜 에러는 그대로 전파. */
}

/*
 * [한국어]
 * spdk_nvme_qpair_authenticate - 사용자 명시적 인증 시작 API (사용자 노출)
 *
 * @qpair:  대상 qpair (이미 connect 완료된 상태여야 함)
 * @cb_fn:  인증 완료 시 호출될 콜백 (NULL이면 콜백 없음)
 * @cb_ctx: cb_fn의 첫 인자
 * @return 0 성공(인증 시작), 음수 errno.
 *           -EALREADY: 이미 다른 인증이 진행 중 (cb_fn != NULL인 상태)
 *           -ENOKEY:   dhchap_key 미설정
 *           기타: 트랜스포트별 qpair_authenticate 콜백의 에러
 *
 * 동작:
 *   [1] 동시 진행 방지 — 이미 cb_fn이 등록되어 있으면 -EALREADY.
 *   [2] 키 사전 검사.
 *   [3] 트랜스포트 vtable의 qpair_authenticate 호출 — 보통 PCIe는 -ENOTSUP, Fabrics는 위 async로.
 *   [4] 성공 시에만 cb_fn/cb_ctx 등록 — 나중에 DONE에서 호출됨.
 *
 * 사용처: 사용자가 자동 트리거 외에 수동으로 인증 시작하고 싶을 때
 *         (예: 컨트롤러가 atr=0이지만 호스트 정책상 항상 인증 원할 때).
 */
int
spdk_nvme_qpair_authenticate(struct spdk_nvme_qpair *qpair,
			     spdk_nvme_authenticate_cb cb_fn, void *cb_ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	int rc;

	if (qpair->auth.cb_fn != NULL) {
                                  /* [한국어] 동시 진행 방지 — 이전 인증이 끝나고 cb_fn이 NULL로 클리어된 후에만 새 인증 가능 */
		AUTH_ERRLOG(qpair, "authentication already in-progress\n");
		return -EALREADY;
	}

	if (ctrlr->opts.dhchap_key == NULL) {
                                  /* [한국어] 사전 검사 — 키 없으면 트랜스포트까지 갈 필요 없음 */
		AUTH_ERRLOG(qpair, "missing DH-HMAC-CHAP key\n");
		return -ENOKEY;
	}

	rc = nvme_transport_qpair_authenticate(qpair);
                                  /* [한국어] 트랜스포트 vtable로 위임 — Fabrics는 위의 async 호출, PCIe는 -ENOTSUP */
	if (rc == 0) {
		qpair->auth.cb_fn = cb_fn;
		qpair->auth.cb_ctx = cb_ctx;
                                  /* [한국어] 시작 성공 시에만 cb 등록 — 실패 시 cb 호출 의무 없음 */
	}

	return rc;
}
#endif /* SPDK_CONFIG_EVP_MAC */
                                  /* [한국어] 위 SPDK_CONFIG_HAVE_EVP_MAC 가드 종료.
                                   *  OpenSSL 3.0+ 없는 빌드에서는 위 모든 함수 컴파일 안 됨 → 인증 기능 없음. */

SPDK_LOG_REGISTER_COMPONENT(nvme_auth)
                                  /* [한국어] SPDK 로그 컴포넌트 "nvme_auth" 등록 — AUTH_DEBUGLOG/AUTH_ERRLOG가
                                   *  이 컴포넌트로 분류되어 사용자가 spdk_log_set_print_level 등으로 개별 제어 가능. */
