/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation
 */

/*
 * [한국어 설명] NVMe-oF 타깃(컨트롤러) 측 DH-HMAC-CHAP 인증 구현 (auth.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVMe-oF target(서버/컨트롤러) 측의 in-band 인증 프로토콜을 구현한다.
 * 호스트(initiator)가 CONNECT 후 또는 Re-authentication을 요구할 때, Authentication Send
 * (fabric command opcode=0x7F, fctype=0x05)와 Authentication Receive(fctype=0x06) 두 가지
 * Fabric command를 통해 4단계 challenge-response 핸드셰이크를 수행한다 — NVMe TP4022
 * (DH-HMAC-CHAP) 스펙에 정의된 negotiate → challenge → reply → success1 → success2 흐름.
 * 호스트 측 대응 구현은 lib/nvme/nvme_auth.c에 있다(8상태 머신).
 *
 * 핵심 가치:
 *   1) **PSK 기밀성 보장**: 사전 공유키(PSK)를 그대로 와이어로 보내지 않고, HMAC을 통해
 *      challenge-response로만 노출 — 도청해도 원 키 복원 불가.
 *   2) **Diffie-Hellman forward secrecy**: dhgroup이 NULL이 아니면 ephemeral DH 키페어로
 *      세션 secret을 도출 → 장기 키가 유출되어도 과거 세션 비밀성 유지(전향 안전성).
 *   3) **상호 인증 옵션**: 호스트의 reply 메시지에 cvalid=1이 있으면 컨트롤러도 자기 신원을
 *      증명해야 함 — 양방향 인증으로 컨트롤러 사칭 차단.
 *   4) **알고리즘 협상**: SHA-256/384/512 + ffdhe2048~8192(RFC 7919)의 정책-허용 조합 중
 *      가장 강한 것 선택.
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe-oF 컨트롤러 측 명령 처리 디스패치에서 호출되는 인증 sub-handler:
 *
 *   transport rx(RDMA/TCP) → request 파싱 → ctrlr.c 의 nvmf_request_exec
 *     → fabric command 분기:
 *        ├─ CONNECT(fctype=0x01) → ctrlr.c
 *        ├─ PROPERTY_SET/GET(0x00/0x04) → ctrlr.c
 *        └─ AUTH_SEND/RECV(0x05/0x06) → **이 파일의 nvmf_auth_request_exec**
 *
 * qpair 상태 전이와의 결합:
 *   ACTIVATING(connect 직후) →
 *     atr=1 또는 ascr=1 이면 → AUTHENTICATING (인증 강제)
 *     이외는 → ENABLED (인증 생략)
 *
 * 실행 컨텍스트: qpair를 소유한 SPDK 스레드(폴 그룹의 한 reactor 스레드) 단일 스레드.
 * spdk_poller(timeout poller)와 SPDK 비동기 callback 패턴을 따른다 — 어떤 함수도 블로킹
 * I/O를 하지 않고, 응답 메시지는 nvmf_auth_request_complete로 비동기 완료시킨다.
 *
 * === 타 모듈과의 연결 ===
 * - **nvmf_internal.h**: 타깃 자료구조(spdk_nvmf_qpair, _ctrlr, _subsystem, _request, _tgt) 정의.
 *   특히 qpair->auth(=struct spdk_nvmf_qpair_auth*) 슬롯에 이 파일의 상태를 저장.
 * - **spdk/nvme.h + spdk_internal/nvme.h**: DH-HMAC-CHAP 헬퍼 4종 호출 진입점
 *   (spdk_nvme_dhchap_get_digest_length / generate_dhkey / dhkey_get_pubkey /
 *   dhkey_derive_secret / calculate). 호스트와 컨트롤러가 동일 헬퍼를 사용 → 와이어 호환.
 * - **lib/nvme/nvme_auth.c**: 호스트 측 대응 — 동일 메시지 포맷을 송수신.
 * - **subsystem.c**: nvmf_subsystem_get_dhchap_key()를 통해 spdk_keyring에 등록된
 *   호스트키/컨트롤러키를 가져온다(NVMF_AUTH_KEY_HOST/_CTRLR).
 * - **OpenSSL libcrypto**: RAND_bytes()로 challenge nonce(C_t)와 seqnum 시드 생성. 내부적으로
 *   spdk_nvme_dhchap_*는 EVP_MAC(HMAC) / EVP_PKEY(DH) API를 사용.
 * - **spdk/keyring**: 사용자/RPC가 등록한 PSK key handle을 키링에서 lookup.
 * - **transport_tcp.c**(별도): TLS 1.3 PSK identity 구성과 RFC 5705 EXPORTER key derivation은
 *   TCP 트랜스포트 측에서 처리하며 본 파일은 in-band DH-HMAC-CHAP만 담당.
 *
 * 데이터 흐름 (한 세션):
 *   host → AuthSend(NEGOTIATE) → 컨트롤러: nvmf_auth_negotiate_exec (algo/dhgroup 협상)
 *   host ← AuthRecv(CHALLENGE)  ← 컨트롤러: nvmf_auth_recv_challenge (C_t + ctrlr pubkey)
 *   host → AuthSend(REPLY)      → 컨트롤러: nvmf_auth_reply_exec (HMAC 검증, 상호인증 시 C_h 수신)
 *   host ← AuthRecv(SUCCESS1)   ← 컨트롤러: nvmf_auth_recv_success1 (호스트 인증 OK, 옵션 R_c)
 *   host → AuthSend(SUCCESS2)   → 컨트롤러: nvmf_auth_success2_exec (양방향 완료, ENABLED)
 *
 * 실패 분기:
 *   임의 단계 검증 실패 → FAILURE1 상태로 전이 → 호스트가 AuthRecv 시도하면
 *   AUTH_failure1 메시지로 응답 → 100ms 지연 후 qpair disconnect (timing-side-channel 방지).
 *
 * === 주요 함수/구조체 요약 ===
 *   - **nvmf_auth_request_exec()**: 외부 진입점. AuthSend/AuthRecv fctype 분기 + qpair 상태 검증.
 *   - **nvmf_auth_send_exec()**: AuthSend 페이로드 분기 — auth_type/auth_id로 negotiate/reply/
 *     success2/failure2 sub-handler 디스패치.
 *   - **nvmf_auth_recv_exec()**: AuthRecv — auth.state에 따라 challenge/success1/failure1 빌드.
 *   - **nvmf_auth_negotiate_exec()**: digest(SHA-256/384/512)와 dhgroup(NULL/ffdhe2048~8192) 협상.
 *     서버는 자신이 허용하는 셋과 호스트 제안을 교집합한 뒤 가장 강한 것을 선택.
 *   - **nvmf_auth_recv_challenge()**: 16~64B 랜덤 C_t 생성 + dhgroup이 NULL이 아니면 ephemeral
 *     DH 키페어 생성 후 컨트롤러 pubkey를 challenge 메시지에 첨부.
 *   - **nvmf_auth_reply_exec()**: 호스트의 R_h = HMAC(K_h, ...) 검증. cvalid=1이면 컨트롤러
 *     역방향 응답 R_c = HMAC(K_c, C_h ...)을 미리 계산해 cval에 보존(success1에서 송신).
 *   - **nvmf_auth_recv_success1()**: 호스트 인증 성공 통지. cvalid이면 컨트롤러 응답 R_c 송신
 *     (양방향 인증 진행), 아니면 즉시 ENABLED 전이.
 *   - **nvmf_auth_success2_exec()**: 양방향 인증 최종 confirm. qpair → ENABLED.
 *   - **nvmf_auth_recv_failure1()**: AUTH_failure1 메시지 송신 + 100ms 지연 후 disconnect.
 *   - **nvmf_auth_timeout_poller()**: KATO(기본 120초) 내 다음 단계 미수신 시 disconnect.
 *   - **nvmf_qpair_auth_init/destroy/dump()**: qpair lifecycle 결합 — 인증 컨텍스트 생성/해제/상태 출력.
 *
 *   - **struct spdk_nvmf_qpair_auth**: 단일 qpair의 인증 세션 상태 컨테이너
 *     (state/poller/tid/digest/dhgroup/cval/seqnum/dhkey/cvalid).
 *   - **struct nvmf_auth_common_header**: 모든 in-band 인증 메시지의 공통 4B 헤더
 *     (auth_type, auth_id, t_id) — auth_type/auth_id로 sub-handler를 디스패치.
 *
 * === Wire 프로토콜 매핑 (NVMe TP4022 §3.2~3.5) ===
 *   - Fabric Command opcode = SPDK_NVME_OPC_FABRIC (0x7F)
 *   - fctype: AUTH_SEND=0x05, AUTH_RECV=0x06
 *   - secp(Security Protocol) = 0xE9 (NVMe-oF Authentication, IEEE assigned)
 *   - spsp0=1, spsp1=1 (in-band auth)
 *   - tl(AuthSend)/al(AuthRecv): payload length
 *
 *   메시지 헤더(4B) 공통:
 *     auth_type (1B):  0x00=COMMON, 0x01=DH-HMAC-CHAP
 *     auth_id   (1B):  COMMON{0x00=NEGOTIATE, 0xF0=FAILURE1, 0xF1=FAILURE2},
 *                      DHCHAP{0x01=CHALLENGE, 0x02=REPLY, 0x03=SUCCESS1, 0x04=SUCCESS2}
 *     reserved  (2B)
 *     t_id      (2B):  transaction id (NEGOTIATE에서 host 결정 → 이후 모든 메시지 동일)
 *
 *   4단계 시퀀스(상호 인증 OFF):
 *     [Host]                          [Controller]
 *       NEGOTIATE   (AuthSend) ─────────►
 *                            ◄─────── CHALLENGE  (AuthRecv 응답)
 *       REPLY       (AuthSend) ─────────►   ← 여기서 HMAC 검증
 *                            ◄─────── SUCCESS1  (AuthRecv 응답)
 *
 *   상호 인증 ON(reply.cvalid=1) 추가 라운드:
 *       SUCCESS1에 R_c 포함 ─────────► host 검증
 *       SUCCESS2    (AuthSend) ────────►   ← 양방향 완료
 *
 * === 보안 주의사항 ===
 *   - challenge nonce(C_t)와 seqnum은 OpenSSL RAND_bytes() (CSPRNG) 사용.
 *   - HMAC 비교(memcmp)는 timing-safe가 아니지만 challenge가 랜덤이므로 실용적 영향 미미
 *     (호스트는 매번 다른 challenge에 대한 응답을 보내야 함).
 *   - FAILURE1 후 100ms 강제 지연으로 사이드채널 leakage 완화.
 *   - dhsec/response 등 일시 비밀은 stack에 두며 함수 종료 시 frame이 회수됨(추가
 *     spdk_memset_s zeroize는 호스트 측 nvme_auth.c에서 적용 — 본 파일은 미적용).
 */

#include "spdk/nvme.h"           /* [한국어] NVMe 공개 API: spdk_nvme_dhchap_* 헬퍼 진입 (digest_length/generate_dhkey/derive_secret/calculate). */
#include "spdk/json.h"           /* [한국어] JSON writer: nvmf_qpair_auth_dump()가 RPC 진단용 상태 출력에 사용. */
#include "spdk/log.h"            /* [한국어] SPDK 로그 매크로(SPDK_ERRLOG/_DEBUGLOG/_LOGDUMP) — 인증 단계별 디버깅. */
#include "spdk/stdinc.h"         /* [한국어] 표준 C 헤더 통합(string.h, stdint.h, stdbool.h, stdlib.h, assert.h 등) — SPDK 빌드 호환 래퍼. */
#include "spdk/string.h"         /* [한국어] spdk_strerror() — errno → 사람이 읽을 수 있는 문자열. */
#include "spdk/thread.h"         /* [한국어] spdk_poller_register/_unregister API — KATO 타임아웃 poller, FAILURE1 지연 poller에 사용. */
#include "spdk/util.h"           /* [한국어] SPDK_BIT(n), SPDK_COUNTOF() 등 비트/배열 유틸 — dhchap_digests/dhgroups 마스크 검사. */
#include "spdk_internal/nvme.h"  /* [한국어] DH-HMAC-CHAP 내부 API: spdk_nvme_dhchap_dhkey 자료형 + ephemeral DH 키페어 관리 함수. */

#include <openssl/rand.h>        /* [한국어] OpenSSL CSPRNG — RAND_bytes()로 challenge nonce(C_t)와 seqnum 초기 시드 생성. */

#include "nvmf_internal.h"       /* [한국어] NVMe-oF 타깃 내부 자료구조: spdk_nvmf_qpair / _ctrlr / _subsystem / _request / _tgt + qpair 상태 머신 enum + key lookup API. */

/* [한국어] 인증 단계 간 timeout (마이크로초) - keep-alive timer(KATO)가 0이면 이 기본값 사용.
 * 120초는 NVMe-oF 1.1 권고 — 호스트가 적절한 시간 안에 다음 단계를 보내지 않으면 인증 실패.
 * 너무 짧으면 정상 클라이언트 스파이크에서 실패, 너무 길면 좀비 세션 누수. */
#define NVMF_AUTH_DEFAULT_KATO_US	(120ull * 1000 * 1000)
/* [한국어] FAILURE1 메시지 송신 후 disconnect까지의 강제 지연 (100ms).
 * 사이드채널 timing 공격 완화 — 인증 실패 즉시 끊으면 어떤 단계에서 실패했는지가
 * 응답시간으로 누설될 수 있어, 의도적으로 일정 지연을 둔다. */
#define NVMF_AUTH_FAILURE1_DELAY_US	(100ull * 1000)
/* [한국어] HMAC digest 최대 길이 (64B) - SHA-512 출력 크기에 맞춤.
 * spdk_nvmf_qpair_auth.cval[]과 stack 버퍼 response[]/auth->cval에 사용.
 * SHA-256(32B) / SHA-384(48B) / SHA-512(64B) 모두 수용. */
#define NVMF_AUTH_DIGEST_MAX_SIZE	64
/* [한국어] DH 공개키/secret 최대 길이 (1024B = 8192비트 = ffdhe8192 그룹의 최대 modulus 크기).
 * dhv[]/dhsec[] stack 버퍼 사이즈로 사용 — 가장 큰 RFC 7919 그룹(8192비트)도 수용. */
#define NVMF_AUTH_DH_KEY_MAX_SIZE	1024

/* [한국어] 인증 에러 로그 매크로 — qpair → subsys/host/qid 식별 prefix 자동 부여.
 * subnqn(서브시스템 NVMe Qualified Name), hostnqn(호스트 식별자), qid(queue id) 3종 키.
 * 멀티 호스트 멀티 서브시스템 환경에서 어느 세션의 실패인지 즉시 추적 가능. */
#define AUTH_ERRLOG(q, fmt, ...) \
	SPDK_ERRLOG("[%s:%s:%u] " fmt, (q)->ctrlr->subsys->subnqn, (q)->ctrlr->hostnqn, \
		    (q)->qid, ## __VA_ARGS__)
/* [한국어] 인증 디버그 로그 — "nvmf_auth" 컴포넌트 활성화 시에만 출력.
 * SPDK_LOG_REGISTER_COMPONENT(nvmf_auth)와 짝. SPDK CLI: --logflag=nvmf_auth. */
#define AUTH_DEBUGLOG(q, fmt, ...) \
	SPDK_DEBUGLOG(nvmf_auth, "[%s:%s:%u] " fmt, \
		      (q)->ctrlr->subsys->subnqn, (q)->ctrlr->hostnqn, (q)->qid, ## __VA_ARGS__)
/* [한국어] 바이너리 데이터 hex dump — challenge/response/dhsec/pubkey 디버깅에 사용.
 * 운영 빌드에선 "nvmf_auth" 로그 컴포넌트가 꺼져 있어 비활성. 시크릿 노출 방지 차원. */
#define AUTH_LOGDUMP(msg, buf, len) \
	SPDK_LOGDUMP(nvmf_auth, msg, buf, len)

/*
 * [한국어] 컨트롤러 측 단일 qpair의 인증 진행 단계 enum.
 *
 * 호스트 측(nvme_auth.c)의 8상태 머신과 대칭 — 호스트가 "AWAIT_*"로 응답을 기다리는 동안
 * 컨트롤러는 그 메시지를 "*_EXEC"하기 위해 해당 상태에 머문다. 상태 전이는 nvmf_auth_set_state()
 * 단일 진입점으로만 일어난다.
 *
 * 정상 흐름:
 *   NEGOTIATE → CHALLENGE → REPLY → SUCCESS1
 *     상호인증 OFF: → COMPLETED (qpair=ENABLED)
 *     상호인증 ON:  → SUCCESS2 → COMPLETED
 *
 * 실패 흐름:
 *   임의 단계 → FAILURE1 → (FAILURE1 메시지 송신 후 100ms 지연) → ERROR + disconnect
 */
enum nvmf_qpair_auth_state {
	NVMF_QPAIR_AUTH_NEGOTIATE,
	/* [한국어] 초기 상태. 호스트가 첫 AuthSend(NEGOTIATE)를 보낼 때까지 대기.
	 * 진입자: nvmf_qpair_auth_init() — qpair 인증 컨텍스트 생성 시 자동 진입.
	 * 다음 상태: NEGOTIATE 메시지 정상 처리 후 CHALLENGE. */

	NVMF_QPAIR_AUTH_CHALLENGE,
	/* [한국어] NEGOTIATE 완료. 호스트의 AuthRecv(CHALLENGE) 요청을 기다림.
	 * 진입자: nvmf_auth_negotiate_exec() — digest/dhgroup 협상 성공 시.
	 * 다음 상태: nvmf_auth_recv_challenge()가 challenge 메시지 빌드 후 REPLY. */

	NVMF_QPAIR_AUTH_REPLY,
	/* [한국어] CHALLENGE 송신 완료. 호스트의 AuthSend(REPLY)로 R_h를 받기 대기.
	 * 진입자: nvmf_auth_recv_challenge() — challenge 응답 송신 후.
	 * 다음 상태: nvmf_auth_reply_exec()가 HMAC 검증 통과 시 SUCCESS1. */

	NVMF_QPAIR_AUTH_SUCCESS1,
	/* [한국어] REPLY 검증 완료. 호스트의 AuthRecv(SUCCESS1) 요청을 기다림.
	 * 진입자: nvmf_auth_reply_exec() — 호스트 응답 HMAC 일치 시.
	 * 다음 상태:
	 *   - cvalid=0(상호인증 OFF) → COMPLETED + qpair=ENABLED
	 *   - cvalid=1(상호인증 ON)  → SUCCESS2 (R_c 송신 후 호스트 응답 대기) */

	NVMF_QPAIR_AUTH_SUCCESS2,
	/* [한국어] SUCCESS1 송신(R_c 포함) 완료. 호스트의 AuthSend(SUCCESS2)를 기다림.
	 * 진입자: nvmf_auth_recv_success1() — cvalid=1일 때.
	 * 다음 상태: nvmf_auth_success2_exec()가 양방향 완료 → COMPLETED + qpair=ENABLED.
	 *           또는 호스트가 AUTH_failure2를 보내면 ERROR로 전이. */

	NVMF_QPAIR_AUTH_FAILURE1,
	/* [한국어] 임의 단계에서 검증 실패 → 호스트가 다음 AuthRecv를 보내면 AUTH_failure1로 응답.
	 * 진입자: nvmf_auth_request_fail1() — 모든 invalid 메시지/HMAC 불일치/키 부재 등.
	 * 다음 상태: nvmf_auth_recv_failure1()이 실패 메시지 송신 + 100ms 지연 poller 등록 →
	 *           timeout 발화 시 disconnect로 ERROR 전이. */

	NVMF_QPAIR_AUTH_COMPLETED,
	/* [한국어] 인증 정상 완료. qpair는 SPDK_NVMF_QPAIR_ENABLED로 전이됨.
	 * 이후 일반 NVMe I/O 명령 처리 가능. Re-authentication 시 다시 NEGOTIATE로 복귀. */

	NVMF_QPAIR_AUTH_ERROR,
	/* [한국어] 비가역적 에러 — qpair disconnect 진행 중 또는 disconnect 완료.
	 * 진입자: nvmf_auth_disconnect_qpair() / failure2 수신 등.
	 * 이 상태에서는 더 이상 어떤 메시지도 처리하지 않음. */
};

/*
 * [한국어] 단일 qpair에 종속된 DH-HMAC-CHAP 인증 세션 컨텍스트.
 *
 * 수명주기:
 *   생성: nvmf_qpair_auth_init() — qpair 상태가 ACTIVATING이고 인증이 필요한 경우 또는
 *         재인증 트리거 시 calloc.
 *   소멸: nvmf_qpair_auth_destroy() — qpair 종료 시. nvmf_auth_qpair_cleanup()는 진행
 *         중인 poller/dhkey만 해제(컨텍스트 자체는 보존).
 *
 * 동기화: qpair는 단일 SPDK 스레드에 affinity 고정 — 본 구조체에는 락이 필요 없음.
 *   예외: subsys->auth_seqnum 갱신만 subsys->mutex로 보호(다른 qpair와 공유).
 */
struct spdk_nvmf_qpair_auth {
	enum nvmf_qpair_auth_state	state;
	/* [한국어] 현재 인증 단계. 위 enum 정의 참조.
	 * 설정자: nvmf_auth_set_state() 단일 함수 — 디버그 로그와 함께 변경.
	 * 읽는 자: 모든 *_exec / *_recv_* 함수가 진입 시 검증.
	 * 값 범위: NVMF_QPAIR_AUTH_NEGOTIATE..ERROR (8값).
	 * 동기화: qpair 단일 스레드 고정이므로 락 불필요. */

	struct spdk_poller		*poller;
	/* [한국어] KATO timeout poller 핸들 — 다음 단계 메시지를 일정 시간 내 받지 못하면 disconnect.
	 * 또한 FAILURE1 응답 후 disconnect까지 100ms 지연 poller로도 재사용(unregister 후 재등록).
	 * 설정자: nvmf_auth_rearm_poller()(인증 진행 중) / nvmf_auth_recv_failure1()(실패 시).
	 * 읽는 자: nvmf_auth_qpair_cleanup() / nvmf_auth_timeout_poller() 자체 콜백.
	 * 값 범위: 유효한 spdk_poller* 또는 NULL(미등록).
	 * 동기화: qpair 스레드 단일 — poller도 같은 스레드에서 발화. */

	int				fail_reason;
	/* [한국어] FAILURE1 메시지의 reason code 보관 — AUTH_failure1.rce 필드로 호스트에 전달.
	 * SPDK_NVMF_AUTH_FAILED / INCORRECT_PAYLOAD / INCORRECT_PROTOCOL_MESSAGE / SCC_MISMATCH /
	 * HASH_UNUSABLE / DHGROUP_UNUSABLE / PROTOCOL_UNUSABLE 등 (NVMe TP4022 §3.6).
	 * 설정자: nvmf_auth_request_fail1() — 검증 실패 발견 시 즉시 기록.
	 * 읽는 자: nvmf_auth_recv_failure1()이 호스트의 AuthRecv 요청 시 응답에 포함.
	 * 값 범위: enum spdk_nvmf_auth_failure_reason. */

	uint16_t			tid;
	/* [한국어] 트랜잭션 ID — 호스트가 NEGOTIATE에서 결정 → 모든 이후 메시지 동일 값 검증.
	 * 한 인증 세션을 식별하는 nonce 역할. 세션 간섭/리플레이 방지의 약한 방어.
	 * 설정자: nvmf_auth_negotiate_exec() — 호스트가 보낸 msg->t_id 그대로 저장.
	 * 읽는 자: 모든 응답 메시지 빌드 시(challenge/success1/failure1) + REPLY/SUCCESS2 검증 시.
	 * 값 범위: 0..65535 (호스트가 정한 값). */

	int				digest;
	/* [한국어] 협상된 HMAC digest 알고리즘 ID — SHA-256(0x01)/SHA-384(0x02)/SHA-512(0x03).
	 * enum spdk_nvmf_dhchap_hash 값. -1은 미협상(init 직후) 상태.
	 * 설정자: nvmf_auth_negotiate_exec() — 정책 허용 셋과 호스트 제안의 교집합에서 가장 강한 것.
	 * 읽는 자: spdk_nvme_dhchap_get_digest_length()로 hl(hash length) 도출.
	 *         spdk_nvme_dhchap_calculate()의 hash 인자.
	 * 값 범위: -1 또는 1..3 (SPDK_NVMF_DHCHAP_HASH_SHA256..512). */

	int				dhgroup;
	/* [한국어] 협상된 DH 그룹 — NULL(0)/ffdhe2048(1)/ffdhe3072(2)/ffdhe4096(3)/ffdhe6144(4)/ffdhe8192(5).
	 * NULL이면 PSK만 사용(무결성), 그 외는 ephemeral DH로 forward secrecy.
	 * 설정자: nvmf_auth_negotiate_exec() — 위와 동일 정책 선택.
	 * 읽는 자: nvmf_auth_recv_challenge()(키페어 생성 여부), nvmf_auth_reply_exec()(secret 도출 여부).
	 * 값 범위: -1 또는 0..5 (RFC 7919 그룹 enum). */

	uint8_t				cval[NVMF_AUTH_DIGEST_MAX_SIZE];
	/* [한국어] 양방향 인증용 슬롯 — 두 가지 용도로 재사용:
	 *   1) CHALLENGE 단계: 컨트롤러가 호스트에게 보낸 nonce C_t (RAND_bytes로 생성)를 보존.
	 *      reply 검증 시 HMAC 입력으로 다시 사용.
	 *   2) REPLY 단계(cvalid=1): 호스트가 보낸 C_h에 대한 컨트롤러 응답 R_c를 미리 계산해서 보관.
	 *      success1 송신 시 그대로 rval 필드에 복사.
	 * 설정자: RAND_bytes (1번 용도) / spdk_nvme_dhchap_calculate (2번 용도).
	 * 읽는 자: spdk_nvme_dhchap_calculate (1번) / memcpy(success->rval, ...) (2번).
	 * 값 범위: 16..64B 임의 바이트. 보안: 평문 nonce/응답 — log dump도 디버그 빌드에서만. */

	uint32_t			seqnum;
	/* [한국어] DH-HMAC-CHAP sequence number — 컨트롤러가 호스트에게 보내는 challenge의 일련번호.
	 * 0이면 호스트가 cvalid=1로 상호인증 요청 시 reject (NVMe TP4022 §3.4 — seqnum=0 + cvalid=1 금지).
	 * 설정자: nvmf_auth_get_seqnum() — subsys 단위 카운터에서 채번 (0 wrap → 1 강제).
	 * 읽는 자: challenge.seqnum 필드로 호스트에 전달, reply 검증 시 HMAC 입력에 포함.
	 * 값 범위: 1..0xFFFFFFFF (절대 0이 아님).
	 * 동기화: subsys->mutex 보호 (여러 qpair가 같은 카운터 공유). */

	struct spdk_nvme_dhchap_dhkey	*dhkey;
	/* [한국어] Ephemeral DH 키페어 핸들 — dhgroup이 NULL이 아닐 때만 생성.
	 * 컨트롤러 측 private + public key를 OpenSSL EVP_PKEY로 보유.
	 * 호스트 pubkey와 결합하여 secret 도출에 사용. forward secrecy의 근거.
	 * 설정자: nvmf_auth_recv_challenge() — spdk_nvme_dhchap_generate_dhkey()로 그룹별 생성.
	 * 읽는 자: spdk_nvme_dhchap_dhkey_get_pubkey()(challenge에 첨부), _derive_secret()(reply 검증).
	 * 값 범위: 유효 포인터(dhgroup!=NULL) 또는 NULL.
	 * 보안: 인증 종료/실패 시 nvmf_auth_qpair_cleanup()이 즉시 free → ephemeral 보장. */

	bool				cvalid;
	/* [한국어] 호스트가 reply에서 cvalid=1로 상호인증을 요청했는지 여부.
	 * 설정자: nvmf_auth_reply_exec() — msg->cvalid==1이고 ctrlr key로 R_c 계산 성공 시 true.
	 * 읽는 자: nvmf_auth_recv_success1() — true면 success 메시지에 R_c(cval) 첨부 + SUCCESS2 진입,
	 *         false면 즉시 ENABLED.
	 * 값 범위: true/false. */
};

/*
 * [한국어] 모든 in-band 인증 메시지의 공통 4B 헤더 (NVMe TP4022 §3.2).
 *
 * 이 헤더만 보면 어느 sub-handler로 디스패치할지 결정 가능 — auth_send_exec / auth_recv가
 * 페이로드 시작 4바이트로 캐스팅하여 type 분기.
 *
 * 와이어 포맷 (host 바이트 순서, NVMe-oF는 little-endian):
 *   offset 0: auth_type (1B)
 *   offset 1: auth_id   (1B)
 *   offset 2: reserved  (2B)
 *   offset 4: t_id      (2B)
 */
struct nvmf_auth_common_header {
	uint8_t		auth_type;
	/* [한국어] 메시지 패밀리 — 0x00=COMMON, 0x01=DH-HMAC-CHAP.
	 * 설정자: 호스트가 채워서 송신 (NEGOTIATE/FAILURE2=COMMON, REPLY/SUCCESS2=DHCHAP).
	 * 읽는 자: nvmf_auth_send_exec()의 outer switch.
	 * 값 범위: SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE(0) / SPDK_NVMF_AUTH_TYPE_DHCHAP(1). */

	uint8_t		auth_id;
	/* [한국어] 메시지 종류 — auth_type과 함께 sub-handler 결정.
	 * COMMON: NEGOTIATE(0x00) / FAILURE1(0xF0) / FAILURE2(0xF1)
	 * DHCHAP: CHALLENGE(0x01) / REPLY(0x02) / SUCCESS1(0x03) / SUCCESS2(0x04)
	 * 설정자: 호스트(또는 컨트롤러 응답).
	 * 읽는 자: nvmf_auth_send_exec()의 inner switch. */

	uint8_t		reserved0[2];
	/* [한국어] 예약 영역 (2B) — 0으로 채워야 하지만 본 코드에선 검증하지 않음(완화 정책). */

	uint16_t	t_id;
	/* [한국어] 트랜잭션 ID — 한 인증 세션을 식별. NEGOTIATE에서 호스트가 결정 → 이후 모든 메시지 동일.
	 * 설정자: 호스트(NEGOTIATE에서 임의 값) / 컨트롤러 응답은 호스트가 보낸 값 echo.
	 * 읽는 자: spdk_nvmf_qpair_auth.tid에 보존 후 모든 후속 메시지에서 일치 검증.
	 * 값 범위: 0..65535 (LE 인코딩). */
};

/*
 * [한국어]
 * nvmf_auth_request_complete - AuthSend/AuthRecv 명령에 대한 NVMe completion 빌드 + 송신.
 *
 * @req: 처리 중인 fabric AuthSend/Recv 요청. req->rsp가 호스트에 보낼 CQE의 backing.
 * @sct: Status Code Type (SPDK_NVME_SCT_GENERIC / SPECIFIC / FABRIC).
 * @sc:  Status Code (SUCCESS / INVALID_FIELD / COMMAND_SEQUENCE_ERROR / INTERNAL_DEVICE_ERROR 등).
 * @dnr: Do Not Retry — 1이면 호스트가 자동 재시도하지 않도록 지시.
 *
 * 인증 sub-handler의 모든 정상/에러 경로 종단에서 호출되는 단일 응답 진입점.
 * NVMe-oF transport(TCP/RDMA)가 이 CQE를 호스트로 전송한다.
 *
 * 주의: AUTH_failure1 같은 application-level 실패는 이 함수의 sct/sc로 보내지 않고,
 *      "이 명령 자체는 성공(GENERIC/SUCCESS)"으로 응답한 뒤 다음 AuthRecv가 들어오면
 *      그때 AUTH_failure1 메시지를 페이로드로 돌려준다 (nvmf_auth_request_fail1 참조).
 *      → NVMe-oF 인증 프로토콜은 application 메시지와 NVMe CQE 레이어를 분리해 다룸.
 *
 * 호출 컨텍스트: qpair 소유 SPDK 스레드. 비블로킹.
 *
 * 호출 체인:
 *   negotiate/reply/success2/recv_*_exec → [이 함수] → spdk_nvmf_request_complete →
 *     transport tx (RDMA/TCP)
 */
static void
nvmf_auth_request_complete(struct spdk_nvmf_request *req, int sct, int sc, int dnr)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	/* [한국어] 응답 CQE 포인터 획득 — req->rsp는 transport가 미리 할당한 응답 버퍼. */

	response->status.sct = sct;
	/* [한국어] Status Code Type 비트 (3비트) — Generic/Command-Specific/Media/Path/Vendor/Fabric. */
	response->status.sc = sc;
	/* [한국어] Status Code 비트 (8비트) — sct에 따라 의미 결정 (Generic이면 SUCCESS=0/INVALID_FIELD 등). */
	response->status.dnr = dnr;
	/* [한국어] Do Not Retry 비트 — 호스트가 transparent retry를 시도하지 않도록 지시.
	 * 인증 단계 에러는 대개 dnr=1 (재시도해도 동일 결과). */

	spdk_nvmf_request_complete(req);
	/* [한국어] 트랜스포트별 completion 콜백 호출 → CQE를 호스트로 송신.
	 * RDMA: send WR enqueue. TCP: capsule response 전송. 비동기 복귀. */
}

/*
 * [한국어]
 * nvmf_auth_get_state_name - 인증 상태 enum을 디버그용 문자열로 변환.
 *
 * @state: 변환할 상태 값.
 * @return: 정적 문자열 포인터(영원히 유효, 호출자가 free하지 않음).
 *
 * 단순 lookup. AUTH_DEBUGLOG / nvmf_qpair_auth_dump (RPC)에서 호출.
 *
 * 호출 컨텍스트: 모든 컨텍스트에서 호출 가능(stateless, 락 불필요).
 */
static const char *
nvmf_auth_get_state_name(enum nvmf_qpair_auth_state state)
{
	static const char *state_names[] = {
		/* [한국어] designated initializer로 enum 인덱스에 직접 매핑 — 새 상태 추가 시
		 * 컴파일러가 갭을 NULL로 채우므로 누락 디버그 용이. */
		[NVMF_QPAIR_AUTH_NEGOTIATE] = "negotiate",
		[NVMF_QPAIR_AUTH_CHALLENGE] = "challenge",
		[NVMF_QPAIR_AUTH_REPLY] = "reply",
		[NVMF_QPAIR_AUTH_SUCCESS1] = "success1",
		[NVMF_QPAIR_AUTH_SUCCESS2] = "success2",
		[NVMF_QPAIR_AUTH_FAILURE1] = "failure1",
		[NVMF_QPAIR_AUTH_COMPLETED] = "completed",
		[NVMF_QPAIR_AUTH_ERROR] = "error",
	};

	return state_names[state];
	/* [한국어] 경계 검사 없음 — 호출자가 enum 범위 보장 책임. */
}

/*
 * [한국어]
 * nvmf_auth_set_state - 인증 상태를 변경하고 디버그 로그 출력.
 *
 * @qpair: 상태 변경 대상 qpair.
 * @state: 새 상태.
 *
 * 모든 상태 전이의 단일 진입점 — 직접 auth->state = X 대입 금지.
 * 디버그 로그 일관성 + 향후 상태 전이 hooks(검증/통계) 확장 지점.
 *
 * 동일 상태로의 전이는 no-op (로그 스팸 방지).
 *
 * 호출 컨텍스트: qpair 소유 스레드. 락 불필요(qpair 단일 스레드 affinity).
 *
 * 호출 체인:
 *   nvmf_auth_*_exec / nvmf_auth_recv_* / nvmf_qpair_auth_init → [이 함수]
 */
static void
nvmf_auth_set_state(struct spdk_nvmf_qpair *qpair, enum nvmf_qpair_auth_state state)
{
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] qpair에 부착된 인증 컨텍스트 획득 — 호출 시점엔 항상 유효해야 함. */

	if (auth->state == state) {
		/* [한국어] 같은 상태로 변경하는 것은 no-op — 로그 노이즈 절감. */
		return;
	}

	AUTH_DEBUGLOG(qpair, "auth state: %s\n", nvmf_auth_get_state_name(state));
	/* [한국어] 새 상태 이름을 디버그 로그에 출력 — 인증 흐름 추적 핵심 단서. */
	auth->state = state;
	/* [한국어] 실제 상태 갱신. 이 라인 이후 다른 sub-handler가 새 상태에서 동작. */
}

/*
 * [한국어]
 * nvmf_auth_disconnect_qpair - 인증 실패/종료 시 qpair를 ERROR 상태로 표시하고 disconnect.
 *
 * @qpair: 종료할 qpair.
 *
 * 비가역적 종결 — 호출 후 더 이상 인증 메시지를 처리하지 않는다.
 * spdk_nvmf_qpair_disconnect는 비동기 — 실제 transport 자원 해제는 나중에 진행.
 *
 * 호출 컨텍스트: qpair 소유 스레드.
 *
 * 호출 체인:
 *   timeout_poller / failure2_exec / 페이로드 검증 실패 등 → [이 함수] →
 *   spdk_nvmf_qpair_disconnect → transport 정리 + nvmf_qpair_auth_destroy
 */
static void
nvmf_auth_disconnect_qpair(struct spdk_nvmf_qpair *qpair)
{
	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_ERROR);
	/* [한국어] 명시적으로 ERROR 상태 표시 — 이후 들어오는 메시지가 reuse하지 못하게 가드. */
	spdk_nvmf_qpair_disconnect(qpair);
	/* [한국어] qpair 종료 비동기 트리거 — transport별 graceful shutdown 후 callback 등록. */
}

/*
 * [한국어]
 * nvmf_auth_request_fail1 - 검증 실패를 FAILURE1 상태로 표시하고 현 명령은 success로 응답.
 *
 * @req:    실패가 발견된 AuthSend/Recv 요청.
 * @reason: SPDK_NVMF_AUTH_FAILED / INCORRECT_PAYLOAD / INCORRECT_PROTOCOL_MESSAGE 등.
 *
 * NVMe-oF 인증 프로토콜의 핵심 분리 패턴:
 *   - "이 NVMe 명령 자체"는 GENERIC/SUCCESS로 정상 응답한다.
 *   - "application 레벨 인증 실패"는 다음 호스트의 AuthRecv 시 AUTH_failure1 메시지로 통지.
 *   → 이렇게 하는 이유: NVMe CQE는 호스트가 일반 NVMe 처리하지만, AUTH 실패 정보는
 *     application(인증 SM)가 받아야 함.
 *
 * 호출 후 흐름:
 *   FAILURE1 상태 → 호스트가 다음 AuthRecv 보냄 → nvmf_auth_recv_failure1()이 메시지 빌드 →
 *   100ms 지연 후 disconnect (timing side-channel 완화).
 *
 * 호출 컨텍스트: qpair 소유 스레드.
 *
 * 호출 체인:
 *   negotiate/reply/success2/failure2_exec — 거의 모든 검증 실패 분기 → [이 함수] →
 *   nvmf_auth_request_complete (NVMe layer success)
 *   이후 host AuthRecv → nvmf_auth_recv_failure1
 */
static void
nvmf_auth_request_fail1(struct spdk_nvmf_request *req, int reason)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	/* [한국어] 요청 → 소유 qpair 역참조. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 인증 컨텍스트 획득. */

	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_FAILURE1);
	/* [한국어] 상태를 FAILURE1로 전이 — 다음 AuthRecv가 failure 메시지를 빌드하도록 지시. */
	auth->fail_reason = reason;
	/* [한국어] 실패 사유 보존 — recv_failure1이 그대로 메시지의 rce 필드로 송신. */

	/* The command itself is completed successfully, but a subsequent AUTHENTICATION_RECV
	 * command will be completed with an AUTH_failure1 message
	 */
	/* [한국어] NVMe-oF 와이어 contract: AuthSend 자체는 항상 NVMe 레벨 success로 응답.
	 * 실제 application 인증 실패는 다음 AuthRecv의 페이로드(AUTH_failure1)로 통지. */
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0);
}

/*
 * [한국어]
 * nvmf_auth_digest_allowed - 정책상 이 digest를 허용하는지 검사.
 *
 * @qpair:  검사할 qpair (target까지 chain).
 * @digest: 검사할 hash id (1=SHA-256, 2=SHA-384, 3=SHA-512).
 * @return: true=허용, false=금지.
 *
 * spdk_nvmf_tgt.dhchap_digests는 비트마스크(SPDK_BIT(id) OR'ed) — RPC로 운영자가 설정.
 * 기본값은 모두 활성. 특정 digest 비활성 시 협상에서 자동 제외.
 *
 * 호출 컨텍스트: qpair 스레드. 비블로킹.
 *
 * 호출 체인: nvmf_auth_negotiate_exec 내부 루프.
 */
static bool
nvmf_auth_digest_allowed(struct spdk_nvmf_qpair *qpair, uint8_t digest)
{
	struct spdk_nvmf_tgt *tgt = qpair->group->tgt;
	/* [한국어] qpair → poll group → 소유 target. target 단위로 정책 마스크 보유. */

	return tgt->dhchap_digests & SPDK_BIT(digest);
	/* [한국어] 비트마스크 검사 — SPDK_BIT(n) = (1 << n). 비트 셋이면 허용. */
}

/*
 * [한국어]
 * nvmf_auth_dhgroup_allowed - 정책상 이 DH 그룹을 허용하는지 검사.
 *
 * @qpair:   검사 대상.
 * @dhgroup: DH 그룹 id (0=NULL, 1=ffdhe2048, 2=3072, 3=4096, 4=6144, 5=8192).
 * @return:  true=허용.
 *
 * tgt->dhchap_dhgroups 비트마스크 — RPC로 운영자가 ffdhe2048 미만/이상을 토글 가능.
 * NULL 그룹은 PSK-only 모드 — 약한 그룹이지만 호환성/저비용 환경에서 사용.
 *
 * 호출 체인: nvmf_auth_negotiate_exec 내부 루프.
 */
static bool
nvmf_auth_dhgroup_allowed(struct spdk_nvmf_qpair *qpair, uint8_t dhgroup)
{
	struct spdk_nvmf_tgt *tgt = qpair->group->tgt;
	/* [한국어] target 정책 위치까지 동일 chain. */

	return tgt->dhchap_dhgroups & SPDK_BIT(dhgroup);
	/* [한국어] 비트 검사. */
}

/*
 * [한국어]
 * nvmf_auth_qpair_cleanup - 인증 진행 중 자원(타임아웃 poller, ephemeral DH 키페어) 해제.
 *
 * @auth: 정리할 인증 컨텍스트 (struct 자체는 free하지 않음).
 *
 * 정상 완료(COMPLETED 진입), 재인증 시작, FAILURE/disconnect 진로 모든 곳에서 호출되는
 * idempotent cleanup 헬퍼:
 *   - poller: 더 이상 timeout 발화하지 않도록 unregister.
 *   - dhkey:  ephemeral 키페어 즉시 free → forward secrecy 보장 (private key가 메모리에
 *     남아있으면 나중에 코어덤프 등으로 secret 노출 가능).
 *
 * 주의: struct spdk_nvmf_qpair_auth 자체는 nvmf_qpair_auth_destroy()가 free.
 *       이 함수만 호출하면 컨텍스트 재사용 가능 (재인증 등).
 *
 * 호출 컨텍스트: qpair 소유 스레드.
 *
 * 호출 체인:
 *   timeout_poller / success2_exec / recv_success1 / nvmf_qpair_auth_destroy → [이 함수]
 */
static void
nvmf_auth_qpair_cleanup(struct spdk_nvmf_qpair_auth *auth)
{
	spdk_poller_unregister(&auth->poller);
	/* [한국어] poller 등록 해제 — &auth->poller로 더블 포인터 전달, NULL로 자동 클리어.
	 * 같은 함수가 두 번 호출되어도 안전(NULL이면 no-op). */
	spdk_nvme_dhchap_dhkey_free(&auth->dhkey);
	/* [한국어] DH ephemeral 키페어 free + auth->dhkey=NULL 자동 클리어.
	 * 내부적으로 OpenSSL EVP_PKEY_free 호출 → BIGNUM 메모리도 zeroize.
	 * 보안 핵심: 인증 종료 후 private key를 즉시 폐기해야 forward secrecy 성립. */
}

/*
 * [한국어]
 * nvmf_auth_timeout_poller - 인증 단계 간 timeout 발화 시 호출되는 spdk_poller 콜백.
 *
 * @ctx: SPDK_POLLER_REGISTER 등록 시 전달한 qpair 포인터.
 * @return: SPDK_POLLER_BUSY — 일이 있었음을 표시 (poller 통계용).
 *
 * 동작:
 *   - 호스트가 KATO(또는 기본 120초) 시간 안에 다음 인증 메시지를 보내지 않으면 발화.
 *   - qpair 상태에 따라 분기:
 *     1) ENABLED: re-authentication 도중 timeout — 기존 세션이 유효하므로 fatal 아님.
 *        그냥 인증 SM만 COMPLETED로 표시하고 cleanup → 호스트가 다시 시도 가능.
 *     2) AUTHENTICATING: 첫 인증 도중 timeout — qpair 자체를 disconnect (연결 종료).
 *
 * 호출 컨텍스트: qpair 소유 SPDK 스레드의 poller 디스패치 시점. 단일 스레드, 재진입 없음.
 *
 * 호출 체인:
 *   spdk_thread poller dispatcher → [이 함수] → set_state + cleanup 또는 disconnect
 */
static int
nvmf_auth_timeout_poller(void *ctx)
{
	struct spdk_nvmf_qpair *qpair = ctx;
	/* [한국어] poller 등록 시 전달한 qpair 포인터 복원. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 인증 컨텍스트 — qpair 종료 직전이라도 destroy 전에는 유효. */

	AUTH_ERRLOG(qpair, "authentication timed out\n");
	/* [한국어] 인증 timeout 에러 로그 — subsys/host/qid prefix 자동 포함. */
	spdk_poller_unregister(&auth->poller);
	/* [한국어] one-shot 처리 후 자기 자신 등록 해제 — 두 번 발화 방지. */

	if (qpair->state == SPDK_NVMF_QPAIR_ENABLED) {
		/* Reauthentication timeout isn't considered to be a fatal failure */
		/* [한국어] qpair는 이미 ENABLED — 기존 세션 활성 중 재인증 시도 timeout.
		 * 기존 인증이 유효하므로 disconnect 없이 인증 SM만 종료. */
		nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_COMPLETED);
		/* [한국어] 인증 SM을 COMPLETED로 복귀 — 다음 사용자/호스트 트리거 시 재시작 가능. */
		nvmf_auth_qpair_cleanup(auth);
		/* [한국어] poller(이미 unregister됨, NULL no-op)와 dhkey만 정리. */
	} else {
		/* [한국어] AUTHENTICATING 상태 — 첫 connect 인증 도중 timeout.
		 * 세션을 활성화하지 못했으므로 qpair 종료(연결 끊기). */
		nvmf_auth_disconnect_qpair(qpair);
	}

	return SPDK_POLLER_BUSY;
	/* [한국어] poller 통계상 "유효 작업 수행" 표시. spdk_thread는 이를 idle 비율 계산에 사용. */
}

/*
 * [한국어]
 * nvmf_auth_rearm_poller - timeout poller를 (재)등록 — 다음 단계 시간 제한 시작.
 *
 * @qpair: poller를 부착할 qpair.
 * @return: 0=성공, -ENOMEM=poller 객체 할당 실패.
 *
 * 인증 진행 중 한 단계가 끝나고 다음 호스트 메시지를 기다리기 시작할 때마다 호출.
 * 기존 poller 있으면 unregister 후 새로 등록 — KATO 카운터를 리셋하는 효과.
 *
 * 타임아웃 산출:
 *   - ctrlr->feat.keep_alive_timer.bits.kato가 호스트가 SET FEATURES로 알린 값 (ms 단위).
 *   - 0이면(미설정) NVMF_AUTH_DEFAULT_KATO_US (120초) 기본값.
 *
 * 호출 컨텍스트: qpair 스레드.
 *
 * 호출 체인:
 *   negotiate_exec / reply_exec / recv_challenge / recv_success1 / nvmf_qpair_auth_init →
 *   [이 함수] → SPDK_POLLER_REGISTER → spdk_thread 내부 timer wheel
 */
static int
nvmf_auth_rearm_poller(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	/* [한국어] qpair가 속한 컨트롤러 — KATO feature는 ctrlr 단위 설정. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 인증 컨텍스트. */
	uint64_t timeout;
	/* [한국어] 마이크로초 단위 타임아웃. */

	timeout = ctrlr->feat.keep_alive_timer.bits.kato > 0 ?
		  ctrlr->feat.keep_alive_timer.bits.kato * 1000 :
		  NVMF_AUTH_DEFAULT_KATO_US;
	/* [한국어] 호스트가 KATO 설정했으면 그 값(ms)을 us로 변환 (×1000), 아니면 기본 120초.
	 * KATO는 일반 keep-alive timer 재사용 — 호스트는 이미 keep-alive 의미로 알고 있음. */

	spdk_poller_unregister(&auth->poller);
	/* [한국어] 기존 poller 해제 — 이전 단계 타임아웃이 남아 있을 수 있음. */
	auth->poller = SPDK_POLLER_REGISTER(nvmf_auth_timeout_poller, qpair, timeout);
	/* [한국어] 새 poller 등록 — timeout us 후 nvmf_auth_timeout_poller 1회 발화.
	 * spdk_thread는 timer wheel로 효율적으로 관리. qpair 스레드에서만 호출됨. */
	if (auth->poller == NULL) {
		/* [한국어] poller 객체 메모리 할당 실패 — 호출자가 INTERNAL_ERROR 응답하도록 -ENOMEM 반환. */
		return -ENOMEM;
	}

	return 0;
}

/*
 * [한국어]
 * nvmf_auth_check_command - AuthSend/AuthRecv 공통 헤더 필드 검증.
 *
 * @req:   처리 중인 fabric AuthSend/Recv 요청.
 * @secp:  Security Protocol 식별자 — 반드시 0xE9 (NVMe-oF Authentication).
 * @spsp0: Security Protocol Specific 바이트 0 — in-band auth는 1.
 * @spsp1: Security Protocol Specific 바이트 1 — in-band auth는 1.
 * @len:   페이로드 길이(SPDK 트랜스포트가 보낸 tl/al 필드) — req->length와 일치 필수.
 * @return: 0=정상, -EINVAL=프로토콜 위반.
 *
 * NVMe TP4022 §3.1 와이어 contract 검증:
 *   - secp=0xE9 (SPDK_NVMF_AUTH_SECP_NVME): NVMe-oF authentication 임을 식별.
 *     TCG Storage 와의 다른 secp 값(0xEC=Opal 등)과 구분.
 *   - spsp0=1, spsp1=1: in-band auth 모드. 향후 TLS 1.3 in-band 0x02/0x03 등 확장 가능.
 *   - len == req->length: 트랜스포트가 알린 길이와 실제 SGL이 받은 길이가 일치해야 함.
 *
 * 호출 컨텍스트: qpair 스레드. 비블로킹.
 *
 * 호출 체인:
 *   nvmf_auth_send_exec / nvmf_auth_recv_exec → [이 함수] (실패 시 INVALID_FIELD 응답)
 */
static int
nvmf_auth_check_command(struct spdk_nvmf_request *req, uint8_t secp,
			uint8_t spsp0, uint8_t spsp1, uint32_t len)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	/* [한국어] qpair 역참조 — 에러 로그 prefix 생성용. */

	if (secp != SPDK_NVMF_AUTH_SECP_NVME) {
		/* [한국어] secp=0xE9가 아니면 NVMe-oF auth 가 아닌 다른 보안 프로토콜 — 거부.
		 * TCG Opal(0xEC), TCG Storage(0xEF) 등이 이 fabric command 위에 올 수 있으나
		 * 본 핸들러는 0xE9만 처리 대상. */
		AUTH_ERRLOG(qpair, "invalid secp=%u\n", secp);
		return -EINVAL;
	}
	if (spsp0 != 1 || spsp1 != 1) {
		/* [한국어] in-band auth 는 (spsp0,spsp1)=(1,1) 고정. 다른 조합은 미정의/예약 — 거부. */
		AUTH_ERRLOG(qpair, "invalid spsp0=%u, spsp1=%u\n", spsp0, spsp1);
		return -EINVAL;
	}
	if (len != req->length) {
		/* [한국어] 명령에 명시된 length와 실제 수신된 SGL 길이가 다름 — 트랜스포트 무결성 문제.
		 * RDMA/TCP capsule 파싱 단계에서 정합되어야 정상. */
		AUTH_ERRLOG(qpair, "invalid length: %"PRIu32" != %"PRIu32"\n", len, req->length);
		return -EINVAL;
	}

	return 0;
	/* [한국어] 모든 검증 통과 — 호출자가 페이로드 디스패치로 진입. */
}

/*
 * [한국어]
 * nvmf_auth_get_message - 인증 페이로드 SGL이 단일 contiguous 버퍼인지 검증하고 포인터 반환.
 *
 * @req:  AuthSend/Recv 요청.
 * @size: 호출자가 기대하는 최소 메시지 크기(공통 헤더 또는 sub-message 구조체 sizeof).
 * @return: 메시지 시작 포인터 또는 NULL(요건 불충족).
 *
 * 인증 메시지는 일반적으로 작은(<=수 KB) 단일 capsule이므로 SPDK가 단일 IOV로 전달.
 * 다중 IOV(scatter)는 본 코드가 지원하지 않음 — 트랜스포트별 SGL 처리 정책.
 *
 * 호출 컨텍스트: qpair 스레드.
 *
 * 호출 체인:
 *   send_exec / recv_failure1 / recv_challenge / recv_success1 → [이 함수]
 *   실패 시 INCORRECT_PAYLOAD 으로 fail1 진입.
 */
static void *
nvmf_auth_get_message(struct spdk_nvmf_request *req, size_t size)
{
	if (req->length > 0 && req->iovcnt == 1 && req->iov[0].iov_len >= size) {
		/* [한국어] 3가지 조건 동시 만족:
		 *   - length>0: 페이로드가 비어있지 않음.
		 *   - iovcnt==1: 단일 IOV (scatter SGL 미지원).
		 *   - iov_len>=size: 호출자 요구 크기 이상 보유.
		 * 모두 충족 시 첫 IOV의 base 포인터를 반환 — 호출자가 그대로 구조체 캐스팅. */
		return req->iov[0].iov_base;
	}

	return NULL;
	/* [한국어] 어느 한 조건이라도 미충족 — 호출자가 INCORRECT_PAYLOAD 처리. */
}

/*
 * [한국어]
 * nvmf_auth_negotiate_exec - DH-HMAC-CHAP 1단계 (NEGOTIATE) 처리.
 *
 * @req: 호스트가 보낸 AuthSend(NEGOTIATE) 요청.
 * @msg: AuthSend 페이로드 — struct spdk_nvmf_auth_negotiate (가변 길이, descriptors[] 포함).
 *
 * 동작 흐름 (NVMe TP4022 §3.3):
 *   1) 상태 확인: NVMF_QPAIR_AUTH_NEGOTIATE 여야만 진행.
 *   2) tid 보존 및 메시지 길이 검증 (sizeof + napd*sizeof(desc)).
 *   3) sc_c (Secure Channel Concatenation) 확인 — 본 구현은 미지원.
 *   4) descriptors 배열에서 auth_protocol_id=DHCHAP(0x01) 항목 검색.
 *   5) digests[] 배열을 강도 순(SHA-512→256)으로 순회 + 정책 허용 여부 확인 →
 *      호스트 hash_id_list와 교집합 → 가장 강한 것 선택.
 *   6) dhgroups[] 배열을 동일 방식으로 순회 (8192→NULL) → 가장 강한 것 선택.
 *   7) timeout poller 재무장.
 *   8) 상태 → CHALLENGE 로 전이 + NVMe layer success 응답.
 *
 * 정책 우선순위 모델: 호스트가 nominate한 셋 중 컨트롤러가 허용하는 것 중 가장 강한 것.
 * 강도 순서는 코드에 하드코딩 — 정책으로 약한 것을 비활성화 가능.
 *
 * 호출 컨텍스트: qpair 스레드. 비블로킹.
 *
 * 호출 체인:
 *   nvmf_auth_send_exec → [이 함수] → set_state(CHALLENGE) → request_complete
 *   실패 분기 → nvmf_auth_request_fail1 (다음 AuthRecv가 failure1 메시지 송신)
 */
static void
nvmf_auth_negotiate_exec(struct spdk_nvmf_request *req, struct spdk_nvmf_auth_negotiate *msg)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	/* [한국어] 요청이 속한 qpair — 상태/식별 prefix용. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 인증 컨텍스트 — 협상 결과(digest/dhgroup/tid) 보존 대상. */
	struct spdk_nvmf_auth_descriptor *desc = NULL;
	/* [한국어] DHCHAP descriptor — 호스트가 보낸 napd 항목 중 DHCHAP 형 하나만 사용.
	 * NULL 초기화로 검색 후 미존재 분기 처리. */
	/* These arrays are sorted from the strongest hash/dhgroup to the weakest, so the strongest
	 * hash/dhgroup pair supported by the host is always selected
	 */
	/* [한국어] 강도 내림차순 — for 루프가 처음 만난 허용/일치 항목을 채택하면 곧 가장 강한 선택. */
	enum spdk_nvmf_dhchap_hash digests[] = {
		SPDK_NVMF_DHCHAP_HASH_SHA512,	/* [한국어] 64B digest. NIST FIPS 180-4 — 가장 안전. */
		SPDK_NVMF_DHCHAP_HASH_SHA384,	/* [한국어] 48B digest. SHA-2 family. */
		SPDK_NVMF_DHCHAP_HASH_SHA256	/* [한국어] 32B digest. NVMe-oF 호환 최소선. */
	};
	enum spdk_nvmf_dhchap_dhgroup dhgroups[] = {
		SPDK_NVMF_DHCHAP_DHGROUP_8192,	/* [한국어] ffdhe8192 (RFC 7919) — 8192비트 modulus. */
		SPDK_NVMF_DHCHAP_DHGROUP_6144,	/* [한국어] ffdhe6144. */
		SPDK_NVMF_DHCHAP_DHGROUP_4096,	/* [한국어] ffdhe4096. */
		SPDK_NVMF_DHCHAP_DHGROUP_3072,	/* [한국어] ffdhe3072. */
		SPDK_NVMF_DHCHAP_DHGROUP_2048,	/* [한국어] ffdhe2048 — 최소 안전선. */
		SPDK_NVMF_DHCHAP_DHGROUP_NULL,	/* [한국어] DH 없음 — PSK 만 사용 (forward secrecy 없음). */
	};
	int digest = -1, dhgroup = -1;
	/* [한국어] 협상 결과 임시 변수 — -1=미선택. 음수면 unusable 실패 분기. */
	size_t i, j;
	/* [한국어] 외부 루프(서버 우선순위)/내부 루프(호스트 nominate) 인덱스. */

	if (auth->state != NVMF_QPAIR_AUTH_NEGOTIATE) {
		/* [한국어] 상태가 NEGOTIATE 가 아닌데 NEGOTIATE 메시지 수신 — 프로토콜 위반.
		 * 예: 이미 CHALLENGE/REPLY로 진행 중인데 호스트가 NEGOTIATE 재송신. */
		AUTH_ERRLOG(qpair, "invalid state: %s\n", nvmf_auth_get_state_name(auth->state));
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
		return;
	}

	auth->tid = msg->t_id;
	/* [한국어] 호스트가 정한 트랜잭션 ID 보존 — 이후 모든 메시지가 같은 tid 여야 함. */
	if (req->length < sizeof(*msg) || req->length != sizeof(*msg) + msg->napd * sizeof(*desc)) {
		/* [한국어] 페이로드 길이 정합성: 헤더 + napd 개의 descriptor.
		 * 가변 길이 메시지 — napd로 정확히 계산되어야 함. */
		AUTH_ERRLOG(qpair, "invalid message length: %"PRIu32"\n", req->length);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}

	if (msg->sc_c != SPDK_NVMF_AUTH_SCC_DISABLED) {
		/* [한국어] sc_c (Secure Channel Concatenation) — TLS 위에 올린 in-band auth 와의
		 * concatenation 요구. 본 구현은 SCC=disabled (concatenation 미사용)만 지원.
		 * 호스트가 SCC 요구 시 SCC_MISMATCH 로 실패. */
		AUTH_ERRLOG(qpair, "scc mismatch\n");
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_SCC_MISMATCH);
		return;
	}

	for (i = 0; i < msg->napd; ++i) {
		/* [한국어] 호스트가 nominate한 napd(Number of Auth Protocol Descriptors) 만큼 순회.
		 * 본 구현은 DHCHAP만 지원하므로 첫 DHCHAP 항목 채택. */
		if (msg->descriptors[i].auth_id == SPDK_NVMF_AUTH_TYPE_DHCHAP) {
			desc = &msg->descriptors[i];
			/* [한국어] DHCHAP descriptor 발견 — 이후 hash/dhgroup 협상은 이 desc 안에서. */
			break;
		}
	}
	if (desc == NULL) {
		/* [한국어] 호스트 napd 안에 DHCHAP 가 없음 — 본 컨트롤러와 호환 가능 프로토콜 부재.
		 * NVMe-oF 1.1 추가 프로토콜(예: PUF 기반)이 표준화되면 이 분기 확장. */
		AUTH_ERRLOG(qpair, "no usable protocol found\n");
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_PROTOCOL_UNUSABLE);
		return;
	}
	if (desc->halen > SPDK_COUNTOF(desc->hash_id_list) ||
	    desc->dhlen > SPDK_COUNTOF(desc->dhg_id_list)) {
		/* [한국어] descriptor 내부 halen(hash 개수)/dhlen(dhgroup 개수)이 배열 한도 초과 —
		 * 와이어 포맷 정합 위반. spec 상 halen<=SPDK_COUNTOF(hash_id_list). */
		AUTH_ERRLOG(qpair, "invalid halen=%u, dhlen=%u\n", desc->halen, desc->dhlen);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}

	for (i = 0; i < SPDK_COUNTOF(digests); ++i) {
		/* [한국어] 외부 루프: 컨트롤러 우선순위 (강한 것부터). */
		if (!nvmf_auth_digest_allowed(qpair, digests[i])) {
			/* [한국어] 운영자가 정책으로 비활성화한 digest 는 후보에서 제외. */
			continue;
		}
		for (j = 0; j < desc->halen; ++j) {
			/* [한국어] 내부 루프: 호스트 nominate 셋. */
			if (digests[i] == desc->hash_id_list[j]) {
				/* [한국어] 양측이 모두 받아들이는 첫 매치 — 채택. */
				AUTH_DEBUGLOG(qpair, "selected digest: %s\n",
					      spdk_nvme_dhchap_get_digest_name(digests[i]));
				digest = digests[i];
				break;
			}
		}
		if (digest >= 0) {
			/* [한국어] 한 번 채택되면 외부 루프도 종료 — 더 약한 것은 보지 않음. */
			break;
		}
	}
	if (digest < 0) {
		/* [한국어] 모든 컨트롤러 허용 셋이 호스트 셋과 교집합 0 — 사용 가능 hash 부재. */
		AUTH_ERRLOG(qpair, "no usable digests found\n");
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_HASH_UNUSABLE);
		return;
	}

	for (i = 0; i < SPDK_COUNTOF(dhgroups); ++i) {
		/* [한국어] dhgroup 협상 — digest와 동일 패턴(서버 우선순위 × 호스트 nominate). */
		if (!nvmf_auth_dhgroup_allowed(qpair, dhgroups[i])) {
			continue;
		}
		for (j = 0; j < desc->dhlen; ++j) {
			if (dhgroups[i] == desc->dhg_id_list[j]) {
				AUTH_DEBUGLOG(qpair, "selected dhgroup: %s\n",
					      spdk_nvme_dhchap_get_dhgroup_name(dhgroups[i]));
				dhgroup = dhgroups[i];
				break;
			}
		}
		if (dhgroup >= 0) {
			break;
		}
	}
	if (dhgroup < 0) {
		/* [한국어] 사용 가능 dhgroup 부재 — NULL 까지 비활성화된 경우 또는 호스트가 NULL 미제공. */
		AUTH_ERRLOG(qpair, "no usable dhgroups found\n");
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_DHGROUP_UNUSABLE);
		return;
	}

	if (nvmf_auth_rearm_poller(qpair)) {
		/* [한국어] timeout poller 재무장 실패 — 메모리 부족.
		 * 응답 포함 완전 실패 처리: NVMe layer error + qpair disconnect.
		 * fail1 패턴이 아닌 INTERNAL_DEVICE_ERROR — 와이어 protocol 응답 불가능. */
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 1);
		nvmf_auth_disconnect_qpair(qpair);
		return;
	}

	auth->digest = digest;
	/* [한국어] 협상 결과 보존 — 이후 challenge/reply 모두 이 값 사용. */
	auth->dhgroup = dhgroup;
	/* [한국어] dhgroup 도 보존 — challenge에서 키페어 생성 여부 결정. */
	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_CHALLENGE);
	/* [한국어] 다음 단계로 전이 — 호스트의 AuthRecv(CHALLENGE) 요청 대기. */
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0);
	/* [한국어] AuthSend(NEGOTIATE) 자체는 NVMe layer success 로 응답.
	 * 실제 challenge 페이로드는 호스트가 다음 AuthRecv 보낼 때 전달. */
}

/*
 * [한국어]
 * nvmf_auth_reply_exec - DH-HMAC-CHAP 3단계 (REPLY) 처리.
 *
 * @req: 호스트가 보낸 AuthSend(REPLY) — challenge 에 대한 응답.
 * @msg: 페이로드 — struct spdk_nvmf_dhchap_reply (cvalid, seqnum, rval[host_resp || ctrlr_chal || pubkey] 포함).
 *
 * 동작 흐름 (NVMe TP4022 §3.4):
 *   1) 상태/길이/tid/cvalid/seqnum 정합성 검증.
 *   2) keyring에서 호스트 PSK key (NVMF_AUTH_KEY_HOST) lookup.
 *   3) dhgroup != NULL 이면 spdk_nvme_dhchap_dhkey_derive_secret() 으로 DH secret 도출.
 *      입력: 컨트롤러 private key + 호스트 pubkey(rval[2*hl..])
 *   4) spdk_nvme_dhchap_calculate("HostHost", ...) 로 기대 응답 R_h 계산.
 *      HMAC 입력: K_h(host PSK) + C_t(auth->cval) + tid + seqnum + transcript ...
 *   5) memcmp(msg->rval, response, hl) — 호스트가 보낸 응답과 일치하는지 검증.
 *      실패 시 → AUTH_FAILED 로 fail1.
 *   6) msg->cvalid=1 이면 (상호 인증 요청):
 *      - 컨트롤러 key (NVMF_AUTH_KEY_CTRLR) lookup.
 *      - spdk_nvme_dhchap_calculate("Controller", ...) 로 R_c 계산 후 auth->cval 에 저장.
 *      - 이 R_c 는 success1 메시지에서 호스트로 송신.
 *   7) timeout poller 재무장 + 상태 SUCCESS1 전이.
 *
 * 핵심 보안 핸들링:
 *   - HMAC 검증 실패도 INCORRECT_PAYLOAD 가 아닌 AUTH_FAILED — 메시지 형식은 정상이지만
 *     인증 자체가 실패함을 명확히 구분.
 *   - 정확히 하나의 out: 라벨로 keyring put 보장 — 키 reference count leak 방지.
 *
 * 호출 컨텍스트: qpair 스레드. 비블로킹.
 *
 * 호출 체인:
 *   nvmf_auth_send_exec → [이 함수] → set_state(SUCCESS1) → request_complete
 *   실패 분기 → nvmf_auth_request_fail1
 */
static void
nvmf_auth_reply_exec(struct spdk_nvmf_request *req, struct spdk_nvmf_dhchap_reply *msg)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	/* [한국어] 요청 → qpair 역참조. */
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	/* [한국어] qpair 가 속한 컨트롤러 — subsys/hostnqn 체이닝의 시작점. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 인증 컨텍스트 — digest/dhgroup/cval/seqnum/dhkey 모두 보존됨. */
	uint8_t response[NVMF_AUTH_DIGEST_MAX_SIZE];
	/* [한국어] 컨트롤러가 자체 계산한 기대 응답 R_h — 호스트가 보낸 msg->rval과 비교용.
	 * stack 버퍼: 최대 64B (SHA-512). 함수 종료 시 자동 회수 — 임시 비밀로 메모리 잔존 최소화. */
	uint8_t dhsec[NVMF_AUTH_DH_KEY_MAX_SIZE];
	/* [한국어] DH shared secret — dhgroup != NULL 일 때만 채워짐. 최대 1024B (ffdhe8192).
	 * stack 보관 — 함수 종료 시 자동 회수, ephemeral DH 의 forward secrecy 의 한 축. */
	struct spdk_key *key = NULL, *ckey = NULL;
	/* [한국어] keyring 에서 가져온 PSK 핸들. NULL 초기화로 out: 에서 안전하게 put. */
	size_t dhseclen = 0;
	/* [한국어] dhsec 의 실제 길이. 0=DH 사용 안함 → calculate 에 NULL 전달. */
	uint8_t hl;
	/* [한국어] hash length — digest 에 따라 32/48/64. msg->hl 검증 및 HMAC/memcmp 길이로 사용. */
	int rc;
	/* [한국어] 임시 반환값. */

	if (auth->state != NVMF_QPAIR_AUTH_REPLY) {
		/* [한국어] 상태가 REPLY 가 아닌데 REPLY 메시지 도착 — 프로토콜 위반. */
		AUTH_ERRLOG(qpair, "invalid state=%s\n", nvmf_auth_get_state_name(auth->state));
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
		goto out;
	}
	if (req->length < sizeof(*msg)) {
		/* [한국어] 헤더만큼도 못 채운 경우 — 와이어 트렁케이션. */
		AUTH_ERRLOG(qpair, "invalid message length=%"PRIu32"\n", req->length);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}

	hl = spdk_nvme_dhchap_get_digest_length(auth->digest);
	/* [한국어] 협상된 digest 의 길이 추출 — SHA-256→32, SHA-384→48, SHA-512→64. */
	if (hl == 0 || msg->hl != hl) {
		/* [한국어] hl=0 이면 잘못된 digest id (방어적). msg->hl 이 협상값과 다르면 호스트 오작동. */
		AUTH_ERRLOG(qpair, "hash length mismatch: %u != %u\n", msg->hl, hl);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}
	if ((msg->dhvlen % 4) != 0) {
		/* [한국어] DH value 길이는 4B 정렬 필수 — NVMe TP4022 와이어 alignment 규칙. */
		AUTH_ERRLOG(qpair, "dhvlen=%u is not multiple of 4\n", msg->dhvlen);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}
	if (req->length != sizeof(*msg) + 2 * hl + msg->dhvlen) {
		/* [한국어] 정확한 페이로드 길이: 헤더 + R_h(hl) + C_h(hl) + DH pubkey(dhvlen).
		 * 2*hl: msg->rval 영역에 [host 응답 R_h | host challenge C_h] 2개 hash 가 연이어. */
		AUTH_ERRLOG(qpair, "invalid message length: %"PRIu32" != %zu\n",
			    req->length, sizeof(*msg) + 2 * hl);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}
	if (msg->t_id != auth->tid) {
		/* [한국어] 트랜잭션 ID 불일치 — 다른 세션과 혼동/replay 가능성. */
		AUTH_ERRLOG(qpair, "transaction id mismatch: %u != %u\n", msg->t_id, auth->tid);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}
	if (msg->cvalid != 0 && msg->cvalid != 1) {
		/* [한국어] cvalid 는 boolean — 0/1 외 값 거부 (방어적 검증, 향후 spec 확장 대비). */
		AUTH_ERRLOG(qpair, "unexpected cvalid=%d\n", msg->cvalid);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}
	if (msg->cvalid && msg->seqnum == 0) {
		/* [한국어] 상호 인증 요청(cvalid=1) 시 seqnum=0 금지 — TP4022 §3.4 명시.
		 * seqnum=0 은 "이전 세션 재사용" 의미라 freshness 보장 불가. */
		AUTH_ERRLOG(qpair, "unexpected seqnum=0 with cvalid=1\n");
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}

	key = nvmf_subsystem_get_dhchap_key(ctrlr->subsys, ctrlr->hostnqn, NVMF_AUTH_KEY_HOST);
	/* [한국어] keyring 에서 host PSK lookup — subsys + hostnqn 으로 식별.
	 * RPC `nvmf_subsystem_add_host` 에서 등록한 키. reference count 증가 — out: 에서 put. */
	if (key == NULL) {
		/* [한국어] 호스트 키가 등록되어 있지 않음 — 인증 자체 불가. */
		AUTH_ERRLOG(qpair, "couldn't get DH-HMAC-CHAP key\n");
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_FAILED);
		goto out;
	}

	if (auth->dhgroup != SPDK_NVMF_DHCHAP_DHGROUP_NULL) {
		/* [한국어] DH 사용 시: 호스트 pubkey 받아 secret 도출. */
		AUTH_LOGDUMP("host pubkey:", &msg->rval[2 * hl], msg->dhvlen);
		/* [한국어] msg->rval 레이아웃: [R_h | C_h | host_pubkey]. 2*hl 오프셋부터 pubkey. */
		dhseclen = sizeof(dhsec);
		/* [한국어] 입출력 인자: 입력=버퍼 크기, 출력=실제 secret 길이. */
		rc = spdk_nvme_dhchap_dhkey_derive_secret(auth->dhkey, &msg->rval[2 * hl],
				msg->dhvlen, dhsec, &dhseclen);
		/* [한국어] DH secret 도출 — OpenSSL EVP_PKEY_derive(ctrlr_priv, host_pub) 호출.
		 * 이 secret 이 forward secrecy 의 핵심 — ephemeral 이므로 세션 종료 시 폐기. */
		if (rc != 0) {
			/* [한국어] 호스트 pubkey 가 잘못된 그룹 또는 invalid point — 거부. */
			AUTH_ERRLOG(qpair, "couldn't derive DH secret\n");
			nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_FAILED);
			goto out;
		}

		AUTH_LOGDUMP("dh secret:", dhsec, dhseclen);
		/* [한국어] 디버그 빌드에서만 dump — 운영 빌드는 차단됨. */
	}

	assert(hl <= sizeof(response) && hl <= sizeof(auth->cval));
	/* [한국어] 컴파일타임/런타임 buffer overflow 방어 — hl<=64B 보장. */
	rc = spdk_nvme_dhchap_calculate(key, (enum spdk_nvmf_dhchap_hash)auth->digest,
					"HostHost", auth->seqnum, auth->tid, 0,
					ctrlr->hostnqn, ctrlr->subsys->subnqn,
					dhseclen > 0 ? dhsec : NULL, dhseclen,
					auth->cval, response);
	/* [한국어] 호스트가 보내야 할 R_h 의 기대값 계산 — 와이어 spec §3.4:
	 *   R_h = HMAC(K_h, "HostHost" || seqnum || tid || 0 || hostnqn || subnqn || dhsec || C_t)
	 * 인자: key(K_h), digest, label("HostHost"), seqnum, tid, scc=0,
	 *       host nqn, subsys nqn, dh secret(없으면 NULL), challenge nonce(C_t),
	 *       출력: response[].
	 * label="HostHost"는 호스트가 자신을 인증하는 응답임을 식별 — controller 응답 R_c 에선 "Controller". */
	if (rc != 0) {
		/* [한국어] HMAC 계산 자체 실패 — OpenSSL 또는 메모리 오류. AUTH_FAILED 로 통일. */
		AUTH_ERRLOG(qpair, "failed to calculate challenge response: %s\n",
			    spdk_strerror(-rc));
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_FAILED);
		goto out;
	}

	if (memcmp(msg->rval, response, hl) != 0) {
		/* [한국어] 호스트 R_h vs 컨트롤러 기대 R_h 비교 — 일치하지 않으면 PSK 불일치/사칭.
		 * memcmp 는 timing-safe 가 아니지만 challenge 가 매번 random 이므로 실용 영향 미미. */
		AUTH_ERRLOG(qpair, "challenge response mismatch\n");
		AUTH_LOGDUMP("response:", msg->rval, hl);
		AUTH_LOGDUMP("expected:", response, hl);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_FAILED);
		goto out;
	}

	if (msg->cvalid) {
		/* [한국어] 호스트가 상호 인증 요청 — 컨트롤러도 자기 신원을 증명해야 함. */
		ckey = nvmf_subsystem_get_dhchap_key(ctrlr->subsys, ctrlr->hostnqn,
						     NVMF_AUTH_KEY_CTRLR);
		/* [한국어] 컨트롤러 key 별도 lookup — 일반적으로 host key 와 별개로 운영자가 지정.
		 * 양방향 인증 시 동일 키를 양쪽에서 알면 컨트롤러 사칭 위험. */
		if (ckey == NULL) {
			/* [한국어] 컨트롤러 key 미등록 — 상호 인증 불가. AUTH_FAILED 로 종료. */
			AUTH_ERRLOG(qpair, "missing DH-HMAC-CHAP ctrlr key\n");
			nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_FAILED);
			goto out;
		}
		rc = spdk_nvme_dhchap_calculate(ckey, (enum spdk_nvmf_dhchap_hash)auth->digest,
						"Controller", msg->seqnum, auth->tid, 0,
						ctrlr->subsys->subnqn, ctrlr->hostnqn,
						dhseclen > 0 ? dhsec : NULL, dhseclen,
						&msg->rval[hl], auth->cval);
		/* [한국어] R_c = HMAC(K_c, "Controller" || msg->seqnum || tid || 0 ||
		 *                    subnqn || hostnqn || dhsec || C_h)
		 * 차이점:
		 *   - key: ckey (controller PSK).
		 *   - label: "Controller".
		 *   - seqnum: 호스트가 보낸 msg->seqnum (controller seqnum 아님).
		 *   - nqn 순서 reverse: subnqn first.
		 *   - challenge: C_h (msg->rval[hl..2*hl]) — 호스트가 컨트롤러에 던진 nonce.
		 *   - 출력: auth->cval — success1 송신 시 그대로 사용. */
		if (rc != 0) {
			/* [한국어] HMAC 계산 실패 — 위와 동일 처리. */
			AUTH_ERRLOG(qpair, "failed to calculate ctrlr challenge response: %s\n",
				    spdk_strerror(-rc));
			nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_FAILED);
			goto out;
		}
		auth->cvalid = true;
		/* [한국어] success1 빌드 시 cval 을 rval 로 송신하도록 표시. */
	}

	if (nvmf_auth_rearm_poller(qpair)) {
		/* [한국어] 다음 단계(SUCCESS1) 메시지 timeout 재시작 실패 — 인프라 에러. */
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 1);
		nvmf_auth_disconnect_qpair(qpair);
		goto out;
	}

	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_SUCCESS1);
	/* [한국어] 호스트 인증 통과 — SUCCESS1 메시지 송신 대기 상태로 전이. */
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0);
	/* [한국어] AuthSend(REPLY) 자체 NVMe layer success — 호스트가 다음 AuthRecv 를 보낸다. */
out:
	spdk_keyring_put_key(ckey);
	/* [한국어] keyring reference count 감소 — NULL safe. 모든 분기 단일 종착점. */
	spdk_keyring_put_key(key);
	/* [한국어] host key 도 동일하게 정리. 두 put 모두 NULL safe 라 항상 호출 가능. */
}

/*
 * [한국어]
 * nvmf_auth_success2_exec - 양방향 인증 마지막 5단계 (SUCCESS2) 처리.
 *
 * @req: 호스트의 AuthSend(SUCCESS2) — controller R_c 검증 통과를 호스트가 통지.
 * @msg: 페이로드 — struct spdk_nvmf_dhchap_success2 (헤더 + tid 만).
 *
 * 호스트는 success1 에서 받은 R_c 를 자기 측에서 같은 키/입력으로 재계산해 비교.
 * 일치하면 컨트롤러 인증 성공으로 보고 SUCCESS2 송신 → 본 함수가 받아 qpair=ENABLED.
 * 불일치라면 호스트는 AUTH_failure2 를 보냄 → nvmf_auth_failure2_exec 처리.
 *
 * 동작 흐름 (NVMe TP4022 §3.5):
 *   1) 상태/길이/tid 정합성 검증.
 *   2) qpair 상태 → ENABLED (정상 NVMe I/O 처리 시작).
 *   3) 인증 SM → COMPLETED.
 *   4) timeout poller / dhkey ephemeral 자원 정리.
 *
 * 호출 컨텍스트: qpair 스레드.
 *
 * 호출 체인:
 *   nvmf_auth_send_exec → [이 함수] → nvmf_qpair_set_state(ENABLED) + cleanup
 */
static void
nvmf_auth_success2_exec(struct spdk_nvmf_request *req, struct spdk_nvmf_dhchap_success2 *msg)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	/* [한국어] 요청 → qpair 역참조. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 인증 컨텍스트 — 정리 대상. */

	if (auth->state != NVMF_QPAIR_AUTH_SUCCESS2) {
		/* [한국어] 양방향 인증 흐름이 아닌데 SUCCESS2 도착 — 프로토콜 위반. */
		AUTH_ERRLOG(qpair, "invalid state=%s\n", nvmf_auth_get_state_name(auth->state));
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
		return;
	}
	if (req->length != sizeof(*msg)) {
		/* [한국어] success2 는 고정 길이 — 헤더만 있고 가변 영역 없음. */
		AUTH_ERRLOG(qpair, "invalid message length=%"PRIu32"\n", req->length);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}
	if (msg->t_id != auth->tid) {
		/* [한국어] tid 불일치 — 다른 세션 메시지 혼입. */
		AUTH_ERRLOG(qpair, "transaction id mismatch: %u != %u\n", msg->t_id, auth->tid);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}

	AUTH_DEBUGLOG(qpair, "controller authentication successful\n");
	/* [한국어] 양방향 인증 완료 — 컨트롤러 신원도 호스트에 의해 검증됨. */
	nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_ENABLED);
	/* [한국어] qpair 활성화 — 이후 일반 NVMe I/O 명령 처리 가능. */
	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_COMPLETED);
	/* [한국어] 인증 SM 도 COMPLETED 로 전이. */
	nvmf_auth_qpair_cleanup(auth);
	/* [한국어] timeout poller unregister + ephemeral DH key free — 더 이상 필요 없음. */
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0);
	/* [한국어] AuthSend(SUCCESS2) NVMe layer success 응답. */
}

/*
 * [한국어]
 * nvmf_auth_failure2_exec - 호스트 측 컨트롤러 검증 실패 통지 (AUTH_failure2) 처리.
 *
 * @req: 호스트가 보낸 AuthSend(FAILURE2).
 * @msg: 페이로드 — struct spdk_nvmf_auth_failure (rc, rce 필드 포함).
 *
 * 호스트가 success1 에서 받은 R_c 를 검증한 결과 불일치 → 컨트롤러 사칭 의심 →
 * AUTH_failure2 메시지로 통지. 컨트롤러는 이를 받아 qpair=ERROR 로 전이 (sub-handler 정리는
 * disconnect 경로에서).
 *
 * 동작:
 *   1) 상태가 SUCCESS2 인지 확인 — 그 외에서 failure2 받으면 프로토콜 위반.
 *   2) 길이/tid 검증.
 *   3) rc/rce 로깅 (디버그) + 상태 ERROR.
 *   4) 응답 자체는 NVMe layer success — disconnect 는 다른 경로에서 트리거.
 *
 * 호출 컨텍스트: qpair 스레드.
 *
 * 호출 체인:
 *   nvmf_auth_send_exec → [이 함수] → set_state(ERROR)
 */
static void
nvmf_auth_failure2_exec(struct spdk_nvmf_request *req, struct spdk_nvmf_auth_failure *msg)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	/* [한국어] 요청 → qpair 역참조. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 인증 컨텍스트. */

	/* AUTH_failure2 is only expected when we're waiting for the success2 message */
	if (auth->state != NVMF_QPAIR_AUTH_SUCCESS2) {
		/* [한국어] 영문 주석대로 — failure2 는 SUCCESS2 대기 중에만 의미. */
		AUTH_ERRLOG(qpair, "invalid state=%s\n", nvmf_auth_get_state_name(auth->state));
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
		return;
	}
	if (req->length != sizeof(*msg)) {
		/* [한국어] failure 메시지 고정 길이. */
		AUTH_ERRLOG(qpair, "invalid message length=%"PRIu32"\n", req->length);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}
	if (msg->t_id != auth->tid) {
		/* [한국어] tid 검증. */
		AUTH_ERRLOG(qpair, "transaction id mismatch: %u != %u\n", msg->t_id, auth->tid);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}

	AUTH_ERRLOG(qpair, "ctrlr authentication failed: rc=%d, rce=%d\n", msg->rc, msg->rce);
	/* [한국어] rc=Reason Code(ALL=0), rce=Reason Code Extension(detail). 운영자 진단 단서. */
	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_ERROR);
	/* [한국어] ERROR 상태 — 더 이상 인증 메시지 처리 안함. 후속 disconnect 는 transport 레벨에서. */
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0);
	/* [한국어] AuthSend(FAILURE2) 자체는 호스트의 보고이므로 NVMe layer success 응답. */
}

/*
 * [한국어]
 * nvmf_auth_send_exec - AuthSend (fctype=0x05) 명령의 페이로드 디스패치.
 *
 * @req: AuthSend 요청.
 *
 * 동작:
 *   1) 공통 fabric command 필드(secp/spsp0/spsp1/tl) 검증.
 *   2) 페이로드의 첫 4B 공통 헤더 획득.
 *   3) (auth_type, auth_id) 조합으로 sub-handler 분기:
 *      COMMON × NEGOTIATE     → nvmf_auth_negotiate_exec
 *      COMMON × FAILURE2      → nvmf_auth_failure2_exec
 *      DHCHAP × REPLY         → nvmf_auth_reply_exec
 *      DHCHAP × SUCCESS2      → nvmf_auth_success2_exec
 *      기타 조합              → INCORRECT_PROTOCOL_MESSAGE 로 fail1.
 *
 * 호출 컨텍스트: qpair 스레드.
 *
 * 호출 체인:
 *   nvmf_auth_request_exec → [이 함수] → 각 sub-handler.
 */
static void
nvmf_auth_send_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	/* [한국어] 요청 → qpair. 로그 prefix 및 컨텍스트 검색용. */
	struct spdk_nvmf_fabric_auth_send_cmd *cmd = &req->cmd->auth_send_cmd;
	/* [한국어] union nvmf_h2c_msg 의 AuthSend command 뷰 — secp/spsp0/spsp1/tl 필드 접근. */
	struct nvmf_auth_common_header *header;
	/* [한국어] 페이로드 첫 4B 캐스팅 대상 — auth_type/auth_id 디스패치 키. */
	int rc;
	/* [한국어] check_command 반환값. */

	rc = nvmf_auth_check_command(req, cmd->secp, cmd->spsp0, cmd->spsp1, cmd->tl);
	/* [한국어] secp/spsp/tl 검증 — 와이어 프로토콜 일관성. */
	if (rc != 0) {
		/* [한국어] 검증 실패 — INVALID_FIELD 로 NVMe layer 거부. fail1 패턴 미사용
		 * (이 단계는 AUTH 페이로드에 진입하기 전이라 application failure 가 아닌 명령 거부). */
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INVALID_FIELD, 1);
		return;
	}

	header = nvmf_auth_get_message(req, sizeof(*header));
	/* [한국어] 페이로드 시작 4B 가져오기 — 단일 IOV + 길이 충분 시 포인터 반환. */
	if (header == NULL) {
		/* [한국어] 페이로드 부족/scatter — 진입할 sub-handler 결정 불가. */
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}

	switch (header->auth_type) {
	case SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE:
		/* [한국어] COMMON family — NEGOTIATE/FAILURE 등 protocol-agnostic 메시지. */
		switch (header->auth_id) {
		case SPDK_NVMF_AUTH_ID_NEGOTIATE:
			/* [한국어] 1단계 — digest/dhgroup 협상 시작. */
			nvmf_auth_negotiate_exec(req, (void *)header);
			break;
		case SPDK_NVMF_AUTH_ID_FAILURE2:
			/* [한국어] 호스트가 컨트롤러 인증 실패 통지 (양방향 인증 4단계). */
			nvmf_auth_failure2_exec(req, (void *)header);
			break;
		default:
			/* [한국어] COMMON 패밀리에 속하지 않는 알 수 없는 auth_id — 거부. */
			AUTH_ERRLOG(qpair, "unexpected auth_id=%u\n", header->auth_id);
			nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
			break;
		}
		break;
	case SPDK_NVMF_AUTH_TYPE_DHCHAP:
		/* [한국어] DHCHAP family — challenge-response 본 프로토콜. */
		switch (header->auth_id) {
		case SPDK_NVMF_AUTH_ID_DHCHAP_REPLY:
			/* [한국어] 3단계 — 호스트 응답 R_h 검증. */
			nvmf_auth_reply_exec(req, (void *)header);
			break;
		case SPDK_NVMF_AUTH_ID_DHCHAP_SUCCESS2:
			/* [한국어] 5단계 — 양방향 인증 최종 confirm. */
			nvmf_auth_success2_exec(req, (void *)header);
			break;
		default:
			/* [한국어] DHCHAP 가족이지만 알 수 없는 sub-id — 거부.
			 * 참고: CHALLENGE/SUCCESS1 은 host→ctrl 송신 대상이 아님 (ctrl→host 만). */
			AUTH_ERRLOG(qpair, "unexpected auth_id=%u\n", header->auth_id);
			nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
			break;
		}
		break;
	default:
		/* [한국어] auth_type 자체가 미정의 — 향후 확장(예: DH-EC 기반 새 가족). */
		AUTH_ERRLOG(qpair, "unexpected auth_type=%u\n", header->auth_type);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
		break;
	}
}

/*
 * [한국어]
 * nvmf_auth_recv_complete - AuthRecv 응답 길이 설정 + NVMe completion.
 *
 * @req:    AuthRecv 요청.
 * @length: 페이로드에 채워 넣은 실제 길이 (challenge/success/failure 메시지 크기).
 *
 * AuthRecv 는 호스트가 컨트롤러로부터 메시지를 가져가는 명령 — req->iov 에 페이로드를 빌드해
 * 두고 req->length 를 정확한 송신 길이로 갱신해야 트랜스포트가 그 길이만 호스트로 보낸다.
 *
 * 호출 컨텍스트: qpair 스레드.
 *
 * 호출 체인:
 *   recv_challenge / recv_success1 / recv_failure1 → [이 함수] → request_complete.
 */
static void
nvmf_auth_recv_complete(struct spdk_nvmf_request *req, uint32_t length)
{
	assert(req->cmd->nvmf_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV);
	/* [한국어] 디버그 단언: 이 함수는 오직 AuthRecv 명령에 대해서만 호출. */
	req->length = length;
	/* [한국어] 트랜스포트가 이 length 만큼 SGL 데이터를 호스트로 송신하도록 갱신. */
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0);
	/* [한국어] NVMe layer success — 페이로드와 함께 호스트로 전달됨. */
}

/*
 * [한국어]
 * nvmf_auth_recv_failure1_done - FAILURE1 메시지 송신 후 100ms 지연이 끝나면 호출되는 poller.
 *
 * @ctx: qpair 포인터.
 * @return: SPDK_POLLER_BUSY.
 *
 * 인증 실패 후 즉시 disconnect 하지 않고 100ms 지연을 둔다 — 어느 단계에서 실패했는지를
 * 응답시간으로 추론하는 timing side-channel 공격을 완화. 이 poller 가 발화하면 진짜로
 * qpair 종료를 트리거.
 *
 * 호출 컨텍스트: qpair 스레드의 poller dispatcher.
 *
 * 호출 체인:
 *   spdk_thread timer wheel → [이 함수] → nvmf_auth_disconnect_qpair → transport teardown.
 */
static int
nvmf_auth_recv_failure1_done(void *ctx)
{
	struct spdk_nvmf_qpair *qpair = ctx;
	/* [한국어] poller register 시 전달한 qpair 포인터 복원. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 인증 컨텍스트 — poller 자기자신 unregister 용. */

	spdk_poller_unregister(&auth->poller);
	/* [한국어] one-shot 동작 — 재발화 방지. */
	nvmf_auth_disconnect_qpair(qpair);
	/* [한국어] qpair 종료 — ERROR 상태 표시 + transport 비동기 disconnect. */

	return SPDK_POLLER_BUSY;
	/* [한국어] poller idle 비율 통계용. */
}

/*
 * [한국어]
 * nvmf_auth_recv_failure1 - AuthRecv 응답으로 AUTH_failure1 메시지 빌드 + 100ms 지연 disconnect 예약.
 *
 * @req:         AuthRecv 요청 (응답 버퍼 보유).
 * @fail_reason: rce 필드에 들어갈 reason code.
 *
 * 동작:
 *   1) 응답 페이로드에 AUTH_failure1 메시지 작성 (auth_type=COMMON, auth_id=FAILURE1, rc, rce).
 *   2) 상태 → FAILURE1.
 *   3) recv_complete 로 NVMe layer 응답 (응답 페이로드 호스트로 송신).
 *   4) 기존 poller(KATO timeout) unregister 후 100ms 지연 poller 등록 →
 *      nvmf_auth_recv_failure1_done 발화 시 disconnect.
 *
 * 호출 컨텍스트: qpair 스레드.
 *
 * 호출 체인:
 *   nvmf_auth_recv_exec (FAILURE1 상태 또는 default) → [이 함수].
 */
static void
nvmf_auth_recv_failure1(struct spdk_nvmf_request *req, int fail_reason)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	/* [한국어] 요청 → qpair. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] tid 보유 컨텍스트. */
	struct spdk_nvmf_auth_failure *failure;
	/* [한국어] 응답 페이로드 빌드 대상. */

	failure = nvmf_auth_get_message(req, sizeof(*failure));
	/* [한국어] req->iov 가 응답을 담을 단일 IOV이며 충분한 크기인지 확인. */
	if (failure == NULL) {
		/* [한국어] 응답 버퍼 부족 — failure 메시지를 못 보냄. NVMe layer 거부 + 즉시 disconnect.
		 * 100ms 지연 없이 끊어도 무방 — failure 메시지를 송신하지 못한 fatal 케이스. */
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INVALID_FIELD, 1);
		nvmf_auth_disconnect_qpair(qpair);
		return;
	}

	failure->auth_type = SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE;
	/* [한국어] COMMON 패밀리. */
	failure->auth_id = SPDK_NVMF_AUTH_ID_FAILURE1;
	/* [한국어] FAILURE1 sub-id (0xF0). */
	failure->t_id = auth->tid;
	/* [한국어] 협상 단계에서 호스트가 정한 tid 그대로 echo. */
	failure->rc = SPDK_NVMF_AUTH_FAILURE;
	/* [한국어] reason code: 1=Authentication Failure (실패의 일반 카테고리). */
	failure->rce = fail_reason;
	/* [한국어] reason code extension — INCORRECT_PAYLOAD/SCC_MISMATCH 등 세부. */

	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_FAILURE1);
	/* [한국어] 명시적으로 FAILURE1 상태 — 이미 fail1 호출자 측에서 set 했지만 중복 안전. */
	nvmf_auth_recv_complete(req, sizeof(*failure));
	/* [한국어] 호스트에 failure 메시지 송신 — 호스트는 이를 받아 자기 측에서 인증 실패 처리. */

	spdk_poller_unregister(&auth->poller);
	/* [한국어] 기존 KATO timeout poller 해제 — 곧 100ms 지연 poller 로 교체. */
	auth->poller = SPDK_POLLER_REGISTER(nvmf_auth_recv_failure1_done, qpair,
					    NVMF_AUTH_FAILURE1_DELAY_US);
	/* [한국어] 100ms 후 disconnect 트리거 poller 등록 — timing side-channel 완화 핵심. */
}

/*
 * [한국어]
 * nvmf_auth_get_seqnum - DH-HMAC-CHAP sequence number 채번 (subsys 단위 카운터).
 *
 * @qpair: 채번 대상 qpair (subsys 식별용).
 * @return: 0=성공, -EIO=RAND_bytes 실패.
 *
 * 동작:
 *   1) subsys 단위 카운터 subsys->auth_seqnum 보호용 mutex lock.
 *   2) auth_seqnum=0 이면(첫 호출) RAND_bytes 로 random 시드값 채움 — 1부터 시작 안 하기 위해.
 *   3) ++auth_seqnum — 0이 되면(wrap) 1로 강제 (seqnum=0 은 의미상 reserved).
 *   4) 결과를 qpair 단위 auth->seqnum 에 저장 → challenge 메시지에 첨부.
 *
 * 동기화: 같은 subsys 의 여러 qpair 가 카운터 공유 — pthread_mutex 필수.
 *   각 qpair 는 자기 reactor 스레드에서 동작하므로 cross-thread 경합 가능.
 *
 * 보안: seqnum 시드 random 화로 challenge replay 방지 강화.
 *   spec(§3.4): seqnum != 0 이어야 cvalid=1 가능.
 *
 * 호출 컨텍스트: qpair 스레드. mutex 락 짧게만 잡음.
 *
 * 호출 체인:
 *   nvmf_auth_recv_challenge → [이 함수].
 */
static int
nvmf_auth_get_seqnum(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_subsystem *subsys = qpair->ctrlr->subsys;
	/* [한국어] qpair → ctrlr → subsys 체이닝. 카운터는 subsys 단위. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] qpair 단위 인증 컨텍스트. */
	int rc;
	/* [한국어] RAND_bytes 반환값 — OpenSSL 관례상 1=성공, 0/-1=실패. */

	pthread_mutex_lock(&subsys->mutex);
	/* [한국어] subsys 단위 카운터 보호. 같은 subsys 의 다른 qpair 가 동시 채번 가능. */
	if (subsys->auth_seqnum == 0) {
		/* [한국어] 첫 호출 — random 시드 주입.
		 * auth_seqnum 을 1 부터 단순 증가시키면 카운터 값 자체가 정보(서버 시작 후 몇 번째 인증인지)
		 * 노출 → random 시작점으로 정보 누설 차단. */
		rc = RAND_bytes((void *)&subsys->auth_seqnum, sizeof(subsys->auth_seqnum));
		if (rc != 1) {
			/* [한국어] OpenSSL CSPRNG 고갈/오류 — 거의 발생하지 않음 (initialize 실패시 등). */
			pthread_mutex_unlock(&subsys->mutex);
			return -EIO;
		}
	}
	if (++subsys->auth_seqnum == 0) {
		/* [한국어] 32-bit overflow wrap → 0 — spec 상 0 금지이므로 1 로 보정.
		 * 이론적 빈도: 4G 회 인증 후. 실용적으로 발생 가능성 매우 낮음. */
		subsys->auth_seqnum = 1;

	}
	auth->seqnum = subsys->auth_seqnum;
	/* [한국어] qpair 단위 보존 — challenge 메시지 + reply 검증의 HMAC 입력. */
	pthread_mutex_unlock(&subsys->mutex);
	/* [한국어] mutex 해제 — 다른 qpair 채번 허용. */

	return 0;
}

/*
 * [한국어]
 * nvmf_auth_recv_challenge - DH-HMAC-CHAP 2단계 (CHALLENGE) 메시지 빌드 + 송신.
 *
 * @req: AuthRecv 요청 (응답 페이로드 버퍼 보유).
 * @return: 0=성공, SPDK_NVMF_AUTH_FAILED/INCORRECT_PAYLOAD=호출자가 fail1 처리.
 *
 * 동작 흐름 (NVMe TP4022 §3.3):
 *   1) hash length 산출.
 *   2) dhgroup != NULL 이면 ephemeral DH 키페어 생성 + 컨트롤러 pubkey 추출.
 *   3) 응답 버퍼 크기 검증 (header + hl + dhvlen).
 *   4) seqnum 채번.
 *   5) C_t (challenge nonce) random 생성 (RAND_bytes) → auth->cval 보존.
 *   6) timeout poller 재무장.
 *   7) 응답 페이로드에 challenge 메시지 빌드 (auth_type=DHCHAP, auth_id=CHALLENGE, ...).
 *   8) 상태 → REPLY (호스트 응답 대기).
 *
 * 호출 컨텍스트: qpair 스레드.
 *
 * 호출 체인:
 *   nvmf_auth_recv_exec(state=CHALLENGE) → [이 함수] → recv_complete.
 */
static int
nvmf_auth_recv_challenge(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	/* [한국어] 요청 → qpair. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 인증 컨텍스트 — digest/dhgroup 결과 보유. */
	struct spdk_nvmf_dhchap_challenge *challenge;
	/* [한국어] 응답 페이로드 빌드 대상. */
	uint8_t hl, dhv[NVMF_AUTH_DH_KEY_MAX_SIZE];
	/* [한국어] hl=hash length, dhv[]=컨트롤러 DH pubkey 임시 버퍼 (1024B 최대). */
	size_t dhvlen = 0;
	/* [한국어] 실제 pubkey 길이. dhgroup=NULL 시 0 유지. */
	int rc;
	/* [한국어] 임시 반환값. */

	hl = spdk_nvme_dhchap_get_digest_length(auth->digest);
	/* [한국어] 협상된 digest → 32/48/64. */
	assert(hl > 0 && hl <= sizeof(auth->cval));
	/* [한국어] auth->cval 버퍼 overflow 방지 정합성. */

	if (auth->dhgroup != SPDK_NVMF_DHCHAP_DHGROUP_NULL) {
		/* [한국어] DH 사용 시: ephemeral 키페어 생성. */
		auth->dhkey = spdk_nvme_dhchap_generate_dhkey(auth->dhgroup);
		/* [한국어] OpenSSL EVP_PKEY_keygen 으로 ffdhe2048~8192 그룹 키페어 생성.
		 * 이 키는 forward secrecy 의 핵심 — 인증 종료 시 즉시 폐기. */
		if (auth->dhkey == NULL) {
			/* [한국어] 메모리/OpenSSL 오류 — 인증 실패. */
			AUTH_ERRLOG(qpair, "failed to generate DH key\n");
			return SPDK_NVMF_AUTH_FAILED;
		}

		dhvlen = sizeof(dhv);
		/* [한국어] 입력=버퍼 한도, 출력=실제 길이. */
		rc = spdk_nvme_dhchap_dhkey_get_pubkey(auth->dhkey, dhv, &dhvlen);
		/* [한국어] DH pubkey 직렬화 — challenge 메시지에 포함시켜 호스트로 송신. */
		if (rc != 0) {
			/* [한국어] 직렬화 실패 — 거의 발생 불가 (방어적). */
			AUTH_ERRLOG(qpair, "failed to get DH public key\n");
			return SPDK_NVMF_AUTH_FAILED;
		}

		AUTH_LOGDUMP("ctrlr pubkey:", dhv, dhvlen);
		/* [한국어] 디버그 빌드에서 dump — 운영 빌드는 nvmf_auth 컴포넌트 비활성. */
	}

	challenge = nvmf_auth_get_message(req, sizeof(*challenge) + hl + dhvlen);
	/* [한국어] 응답 SGL 이 [헤더 + C_t(hl) + DH pubkey(dhvlen)] 모두 담을 수 있는지 검증. */
	if (challenge == NULL) {
		/* [한국어] 호스트가 al 을 너무 작게 잡음 — INCORRECT_PAYLOAD. */
		AUTH_ERRLOG(qpair, "invalid message length: %"PRIu32"\n", req->length);
		return SPDK_NVMF_AUTH_INCORRECT_PAYLOAD;
	}
	rc = nvmf_auth_get_seqnum(qpair);
	/* [한국어] subsys 단위 카운터에서 새 seqnum 채번 → auth->seqnum 보존. */
	if (rc != 0) {
		return SPDK_NVMF_AUTH_FAILED;
	}
	rc = RAND_bytes(auth->cval, hl);
	/* [한국어] C_t (challenge nonce) — hl 바이트 random.
	 * 이후 reply 검증 시 HMAC 입력으로 다시 사용. challenge 마다 new random 으로 replay 방지. */
	if (rc != 1) {
		/* [한국어] OpenSSL CSPRNG 오류 — 인증 실패. */
		return SPDK_NVMF_AUTH_FAILED;
	}
	if (nvmf_auth_rearm_poller(qpair)) {
		/* [한국어] timeout poller 재무장 실패 — 인프라 오류로 즉시 disconnect. */
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 1);
		nvmf_auth_disconnect_qpair(qpair);
		return 0;
		/* [한국어] 0 반환 — 호출자가 fail1 추가 처리하지 않도록 (이미 disconnect 진행). */
	}

	memcpy(challenge->cval, auth->cval, hl);
	/* [한국어] 응답 페이로드 cval 영역에 C_t 복사. */
	memcpy(&challenge->cval[hl], dhv, dhvlen);
	/* [한국어] cval 뒤이어 컨트롤러 pubkey — 와이어 레이아웃 [C_t | pubkey]. */
	challenge->auth_type = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	/* [한국어] DHCHAP 패밀리. */
	challenge->auth_id = SPDK_NVMF_AUTH_ID_DHCHAP_CHALLENGE;
	/* [한국어] CHALLENGE sub-id (0x01). */
	challenge->t_id = auth->tid;
	/* [한국어] negotiate 단계 tid echo. */
	challenge->hl = hl;
	/* [한국어] hash length 명시 — 호스트가 동일 hl 로 응답 빌드. */
	challenge->hash_id = (uint8_t)auth->digest;
	/* [한국어] 협상된 digest id 통지. */
	challenge->dhg_id = (uint8_t)auth->dhgroup;
	/* [한국어] 협상된 dhgroup id 통지. */
	challenge->dhvlen = dhvlen;
	/* [한국어] DH pubkey 길이 (NULL 그룹이면 0). */
	challenge->seqnum = auth->seqnum;
	/* [한국어] 컨트롤러 측 채번 seqnum — reply HMAC 입력 일부. */

	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_REPLY);
	/* [한국어] 호스트 응답 R_h 대기 상태로 전이. */
	nvmf_auth_recv_complete(req, sizeof(*challenge) + hl + dhvlen);
	/* [한국어] AuthRecv 응답 송신 — challenge 메시지 + DH pubkey 전체 길이. */

	return 0;
}

/*
 * [한국어]
 * nvmf_auth_recv_success1 - DH-HMAC-CHAP 4단계 (SUCCESS1) 메시지 빌드 + 송신.
 *
 * @req: AuthRecv 요청.
 * @return: 0=성공, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD=버퍼 부족.
 *
 * 호스트 인증 통과 후 호스트에 통지. cvalid=1 이면 컨트롤러 응답 R_c 도 함께 송신
 * → 양방향 인증 진입 (SUCCESS2 대기). 아니면 즉시 qpair=ENABLED.
 *
 * 동작 흐름 (NVMe TP4022 §3.5):
 *   1) 응답 페이로드 크기 검증 (header + cvalid*hl).
 *   2) success 메시지 헤더 채우기 (auth_type=DHCHAP, auth_id=SUCCESS1, hl, rvalid 초기 0).
 *   3) cvalid=0 (단방향): qpair → ENABLED, 인증 SM → COMPLETED, 자원 정리.
 *   4) cvalid=1 (양방향): poller 재무장, R_c (auth->cval) 를 success->rval 에 복사,
 *      rvalid=1 표시, 상태 → SUCCESS2.
 *
 * 호출 컨텍스트: qpair 스레드.
 *
 * 호출 체인:
 *   nvmf_auth_recv_exec(state=SUCCESS1) → [이 함수] → recv_complete.
 */
static int
nvmf_auth_recv_success1(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	/* [한국어] 요청 → qpair. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] cvalid/cval/digest 보유. */
	struct spdk_nvmf_dhchap_success1 *success;
	/* [한국어] 응답 페이로드 빌드 대상. */
	uint8_t hl;
	/* [한국어] hash length — rval 길이로 사용. */

	hl = spdk_nvme_dhchap_get_digest_length(auth->digest);
	/* [한국어] 협상된 digest → 32/48/64. */
	success = nvmf_auth_get_message(req, sizeof(*success) + auth->cvalid * hl);
	/* [한국어] 버퍼 크기 검증: cvalid=0 이면 헤더만, cvalid=1 이면 R_c(hl) 추가.
	 * cvalid 가 bool→정수로 산술에 사용된 idiom. */
	if (success == NULL) {
		/* [한국어] 호스트의 al 부족. */
		AUTH_ERRLOG(qpair, "invalid message length: %"PRIu32"\n", req->length);
		return SPDK_NVMF_AUTH_INCORRECT_PAYLOAD;
	}

	AUTH_DEBUGLOG(qpair, "host authentication successful\n");
	/* [한국어] 호스트 인증 성공 — 디버그 로그. */
	success->auth_type = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	/* [한국어] DHCHAP 패밀리. */
	success->auth_id = SPDK_NVMF_AUTH_ID_DHCHAP_SUCCESS1;
	/* [한국어] SUCCESS1 sub-id (0x03). */
	success->t_id = auth->tid;
	/* [한국어] tid echo. */
	/* Kernel initiator always expects hl to be set, regardless of rvalid */
	success->hl = hl;
	/* [한국어] 영문 주석대로: Linux kernel initiator 호환성 — rvalid 무관 항상 hl 채움.
	 * 일부 구현은 rvalid=0 시 hl=0 으로 처리하지만 kernel initiator 는 hl 을 항상 기대. */
	success->rvalid = 0;
	/* [한국어] 초기값 0 — cvalid=1 분기에서만 1 로 갱신. */

	if (!auth->cvalid) {
		/* Host didn't request to authenticate us, we're done */
		/* [한국어] 단방향 인증 완료 — 호스트만 인증되고 컨트롤러 인증은 요청 안함. */
		nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_ENABLED);
		/* [한국어] qpair 활성화 — 일반 NVMe I/O 처리 가능. */
		nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_COMPLETED);
		/* [한국어] 인증 SM 완료. */
		nvmf_auth_qpair_cleanup(auth);
		/* [한국어] poller / DH ephemeral 키 정리 — 더 이상 필요 없음. */
	} else {
		/* [한국어] 양방향 인증 — controller R_c 송신 + SUCCESS2 응답 대기. */
		if (nvmf_auth_rearm_poller(qpair)) {
			/* [한국어] poller 재무장 실패 — 인프라 오류. */
			nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
						   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 1);
			nvmf_auth_disconnect_qpair(qpair);
			return 0;
		}
		AUTH_DEBUGLOG(qpair, "cvalid=1, starting controller authentication\n");
		/* [한국어] 양방향 인증 시작 알림 로그. */
		nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_SUCCESS2);
		/* [한국어] SUCCESS2 메시지(또는 FAILURE2) 대기 상태로 전이. */
		memcpy(success->rval, auth->cval, hl);
		/* [한국어] reply 단계에서 미리 계산한 R_c (auth->cval) 를 응답 페이로드에 복사. */
		success->rvalid = 1;
		/* [한국어] R_c 가 페이로드에 있다고 표시. */
	}

	nvmf_auth_recv_complete(req, sizeof(*success) + auth->cvalid * hl);
	/* [한국어] 응답 송신 — 단방향이면 헤더만, 양방향이면 R_c 까지. */
	return 0;
}

/*
 * [한국어]
 * nvmf_auth_recv_exec - AuthRecv (fctype=0x06) 명령 처리 진입점.
 *
 * @req: AuthRecv 요청.
 *
 * 동작:
 *   1) 공통 명령 필드 검증.
 *   2) 응답 SGL 영역 0 으로 초기화 (스택 메모리 누설 방지).
 *   3) 현재 인증 상태에 따라 분기:
 *      CHALLENGE → recv_challenge (challenge 메시지 빌드/송신).
 *      SUCCESS1  → recv_success1 (호스트 인증 성공 통지).
 *      FAILURE1  → recv_failure1 (실패 메시지 + 100ms 후 disconnect).
 *      그 외     → recv_failure1 (INCORRECT_PROTOCOL_MESSAGE).
 *   4) 빌드 단계 실패 시 fail1 메시지로 fallback.
 *
 * 호출 컨텍스트: qpair 스레드.
 *
 * 호출 체인:
 *   nvmf_auth_request_exec → [이 함수] → recv_challenge / recv_success1 / recv_failure1.
 */
static void
nvmf_auth_recv_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	/* [한국어] 요청 → qpair. */
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 현재 인증 상태 검색. */
	struct spdk_nvmf_fabric_auth_recv_cmd *cmd = &req->cmd->auth_recv_cmd;
	/* [한국어] AuthRecv command — al(allocation length) 필드 보유. */
	int rc;
	/* [한국어] 임시 반환값. */

	rc = nvmf_auth_check_command(req, cmd->secp, cmd->spsp0, cmd->spsp1, cmd->al);
	/* [한국어] secp/spsp/al 검증. */
	if (rc != 0) {
		/* [한국어] 명령 형식 자체 위반 — INVALID_FIELD 거부. */
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INVALID_FIELD, 1);
		return;
	}

	spdk_iov_memset(req->iov, req->iovcnt, 0);
	/* [한국어] 응답 SGL 0 초기화 — 빌드 누락된 영역에 stack/heap 잔재가 leak 되지 않도록.
	 * 보안 핵심: 호스트로 송신될 버퍼이므로 패딩/예약 영역도 0 보장. */
	switch (auth->state) {
	case NVMF_QPAIR_AUTH_CHALLENGE:
		/* [한국어] negotiate 완료 — challenge 메시지 빌드. */
		rc = nvmf_auth_recv_challenge(req);
		if (rc != 0) {
			/* [한국어] 빌드 실패 — fail1 메시지로 대체 송신. */
			nvmf_auth_recv_failure1(req, rc);
		}
		break;
	case NVMF_QPAIR_AUTH_SUCCESS1:
		/* [한국어] reply 검증 통과 — success1 메시지 빌드. */
		rc = nvmf_auth_recv_success1(req);
		if (rc != 0) {
			nvmf_auth_recv_failure1(req, rc);
		}
		break;
	case NVMF_QPAIR_AUTH_FAILURE1:
		/* [한국어] 이전 단계에서 fail1 호출됨 — 실제 failure 메시지를 이제 송신. */
		nvmf_auth_recv_failure1(req, auth->fail_reason);
		break;
	default:
		/* [한국어] AuthRecv 가 와선 안 되는 상태 (NEGOTIATE/REPLY/SUCCESS2/COMPLETED/ERROR).
		 * 호스트가 잘못된 시점에 AuthRecv 보낸 경우 — 프로토콜 위반 처리. */
		nvmf_auth_recv_failure1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
		break;
	}
}

/*
 * [한국어]
 * nvmf_auth_check_state - qpair 상태가 인증 명령을 받을 수 있는 상황인지 확인.
 *
 * @qpair: 요청을 받은 qpair.
 * @req:   AuthSend/Recv 요청.
 * @return: true=계속 진행, false=호출자가 즉시 반환 (응답 이미 송신됨).
 *
 * qpair 상태별 처리:
 *   AUTHENTICATING: 첫 인증 진행 중 — 그대로 진행.
 *   ENABLED:        이미 활성화된 qpair 가 인증 받음 → 재인증(re-authentication) 시작.
 *                   기존 auth 컨텍스트 없거나 COMPLETED 라면 재초기화.
 *                   재인증 중 timeout 은 fatal 아님 (timeout_poller 참조).
 *   그 외:          잘못된 시점 — COMMAND_SEQUENCE_ERROR 거부.
 *
 * 재인증 의의: 호스트가 키 rotation, 정책 갱신, 의심 사건 발생 시 명시적으로 재협상.
 *
 * 호출 컨텍스트: qpair 스레드.
 *
 * 호출 체인:
 *   nvmf_auth_request_exec → [이 함수] (실패 시 응답 직접 송신).
 */
static bool
nvmf_auth_check_state(struct spdk_nvmf_qpair *qpair, struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 현재 인증 컨텍스트 — NULL 일 수 있음(재인증 첫 진입 등). */
	int rc;
	/* [한국어] init 호출 결과. */

	switch (qpair->state) {
	case SPDK_NVMF_QPAIR_AUTHENTICATING:
		/* [한국어] CONNECT 직후 atr=1/ascr=1 이라 인증 강제 — 정상 진행. */
		break;
	case SPDK_NVMF_QPAIR_ENABLED:
		/* [한국어] 이미 활성화된 qpair — 재인증 시나리오. */
		if (auth == NULL || auth->state == NVMF_QPAIR_AUTH_COMPLETED) {
			/* [한국어] 인증 컨텍스트 없거나 이전 인증이 완료 상태 — 새 인증 세션 시작.
			 * COMPLETED 인 경우 nvmf_qpair_auth_init 이 NEGOTIATE 로 재설정. */
			rc = nvmf_qpair_auth_init(qpair);
			if (rc != 0) {
				/* [한국어] 인증 컨텍스트 할당 실패 — 인프라 에러 응답. */
				nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
							   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 0);
				return false;
			}
		}
		break;
	default:
		/* [한국어] DEACTIVATING/ERROR/UNINITIALIZED 등 — 인증 명령 처리 불가.
		 * COMMAND_SEQUENCE_ERROR (NVMe spec): 부적절한 시점에 명령 도착. */
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR, 0);
		return false;
	}

	return true;
	/* [한국어] 호출자가 fctype 분기 로 진행. */
}

/*
 * [한국어]
 * nvmf_auth_request_exec - 외부 진입점. AuthSend/AuthRecv fabric 명령 디스패치.
 *
 * @req: fabric 명령 (opcode=0x7F, fctype=0x05/0x06).
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS — 응답은 후속 비동기 callback 으로.
 *
 * NVMe-oF 컨트롤러 측 명령 디스패치(ctrlr.c)에서 fabric command 분기 시 호출.
 *
 * 동작:
 *   1) qpair 상태 검증 — 부적절하면 즉시 응답 후 반환.
 *   2) opcode=FABRIC 단언 (방어적).
 *   3) fctype 별로 send/recv sub-handler 호출.
 *
 * 호출 컨텍스트: qpair 소유 스레드(폴 그룹 reactor).
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_request_exec (fabric 분기) → [이 함수] →
 *     nvmf_auth_send_exec / nvmf_auth_recv_exec → 각 sub-handler.
 */
int
nvmf_auth_request_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	/* [한국어] 요청 → qpair. */
	union nvmf_h2c_msg *cmd = req->cmd;
	/* [한국어] host-to-controller 메시지 union — opcode/fctype 검사용. */

	if (!nvmf_auth_check_state(qpair, req)) {
		/* [한국어] 상태 부적절 — 응답 이미 송신됨. 분기 종료. */
		goto out;
	}

	assert(cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC);
	/* [한국어] fabric 명령(0x7F)이 맞는지 단언 — 디스패처 contract 위반 검출. */
	switch (cmd->nvmf_cmd.fctype) {
	case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND:
		/* [한국어] fctype=0x05 — host→ctrl 메시지 송신 (NEGOTIATE/REPLY/SUCCESS2/FAILURE2). */
		nvmf_auth_send_exec(req);
		break;
	case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV:
		/* [한국어] fctype=0x06 — host 가 ctrl 의 메시지 가져가기 (CHALLENGE/SUCCESS1/FAILURE1). */
		nvmf_auth_recv_exec(req);
		break;
	default:
		/* [한국어] 호출 contract 위반: AUTH 핸들러는 0x05/0x06 만 처리해야 함. */
		assert(0 && "invalid fctype");
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 0);
		break;
	}
out:
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	/* [한국어] 비동기 — sub-handler 가 nvmf_auth_request_complete 로 응답을 비동기 송신.
	 * 호출자(nvmf_request_exec)는 이 반환값으로 "응답을 직접 처리하지 말 것"을 인식. */
}

/*
 * [한국어]
 * nvmf_qpair_auth_init - qpair 의 인증 컨텍스트 초기화 (생성 또는 재설정).
 *
 * @qpair: 인증 시작할 qpair.
 * @return: 0=성공, -ENOMEM/기타 errno=실패.
 *
 * 두 가지 호출 시나리오:
 *   1) 첫 인증: qpair 상태가 ACTIVATING → AUTHENTICATING 전이 시 ctrlr.c 에서 호출.
 *      → calloc 으로 새 컨텍스트 할당.
 *   2) 재인증: ENABLED 상태에서 호스트가 다시 AuthSend 보냄 → check_state 에서 호출.
 *      → 기존 컨텍스트 재사용, NEGOTIATE 로 재설정.
 *
 * 동작:
 *   1) auth 가 NULL 이면 calloc — 모든 필드 0/NULL 초기화.
 *   2) digest=-1 (미협상 표시).
 *   3) 상태 → NEGOTIATE.
 *   4) timeout poller 등록 (KATO 또는 120초).
 *
 * 호출 컨텍스트: qpair 스레드.
 *
 * 호출 체인:
 *   ctrlr.c qpair activation / nvmf_auth_check_state → [이 함수].
 */
int
nvmf_qpair_auth_init(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 기존 컨텍스트 — 첫 호출이면 NULL. */
	int rc;
	/* [한국어] poller 등록 결과. */

	if (auth == NULL) {
		/* [한국어] 처음 — 새 컨텍스트 할당. */
		auth = calloc(1, sizeof(*qpair->auth));
		/* [한국어] calloc 으로 0 초기화 — state=NEGOTIATE(0), 모든 포인터 NULL, cval[]=0. */
		if (auth == NULL) {
			/* [한국어] 메모리 부족. */
			return -ENOMEM;
		}
	}

	auth->digest = -1;
	/* [한국어] -1 = 미협상 표시 (negotiate_exec 가 1..3 으로 갱신). */
	qpair->auth = auth;
	/* [한국어] qpair 슬롯에 부착 — 재인증 시 이미 같은 포인터일 수 있음. */
	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_NEGOTIATE);
	/* [한국어] 재인증 시 COMPLETED→NEGOTIATE 명시 전이. 첫 호출은 0→0 no-op. */

	rc = nvmf_auth_rearm_poller(qpair);
	/* [한국어] timeout poller 등록 — 첫 NEGOTIATE 메시지 KATO 내 도착 강제. */
	if (rc != 0) {
		/* [한국어] poller 등록 실패 — 컨텍스트 free 후 에러 반환. */
		AUTH_ERRLOG(qpair, "failed to arm timeout poller: %s\n", spdk_strerror(-rc));
		nvmf_qpair_auth_destroy(qpair);
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * nvmf_qpair_auth_destroy - qpair 종료 시 인증 컨텍스트 완전 해제.
 *
 * @qpair: 종료 대상.
 *
 * idempotent — auth 가 NULL 이면 no-op. cleanup(poller/dhkey) 후 struct 자체 free.
 *
 * 호출 컨텍스트: qpair 스레드 종료 직전.
 *
 * 호출 체인:
 *   transport disconnect callback / nvmf_qpair_auth_init 실패 → [이 함수].
 */
void
nvmf_qpair_auth_destroy(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 기존 컨텍스트 — 미할당이면 NULL. */

	if (auth != NULL) {
		/* [한국어] 컨텍스트 존재 — 자원 정리 후 free. */
		nvmf_auth_qpair_cleanup(auth);
		/* [한국어] poller unregister + dhkey ephemeral free. */
		free(qpair->auth);
		/* [한국어] struct 자체 free — calloc 짝. */
		qpair->auth = NULL;
		/* [한국어] dangling pointer 방지 — 이후 init 호출 시 NULL 비교로 신규 할당. */
	}
}

/*
 * [한국어]
 * nvmf_qpair_auth_dump - RPC 진단용으로 인증 상태를 JSON 출력.
 *
 * @qpair: 검사할 qpair.
 * @w:     spdk_json_write_ctx — RPC 응답 빌더.
 *
 * RPC `nvmf_get_qpairs` / `nvmf_subsystem_get_qpairs` 등에서 호출.
 * auth 미할당이면 "auth" 객체 자체를 생략 (호스트별 미인증 qpair 표시 회피).
 *
 * 출력 스키마:
 *   "auth": {
 *     "state":   "negotiate"|"challenge"|"reply"|"success1"|"success2"|"failure1"|"completed"|"error",
 *     "digest":  "sha256"|"sha384"|"sha512"|"unknown",
 *     "dhgroup": "null"|"ffdhe2048"|...|"ffdhe8192"|"unknown"
 *   }
 *
 * 호출 컨텍스트: RPC 스레드 (qpair 단일 스레드 가정).
 *
 * 호출 체인:
 *   subsystem RPC handler → [이 함수].
 */
void
nvmf_qpair_auth_dump(struct spdk_nvmf_qpair *qpair, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	/* [한국어] 인증 컨텍스트 — NULL 가능. */
	const char *digest, *dhgroup;
	/* [한국어] enum→문자열 변환 결과. NULL 가능 — 미협상 시. */

	if (auth == NULL) {
		/* [한국어] 인증 비활성 qpair — JSON 에 "auth" 키 자체 생략. */
		return;
	}

	spdk_json_write_named_object_begin(w, "auth");
	/* [한국어] "auth": { ... 시작. */
	spdk_json_write_named_string(w, "state", nvmf_auth_get_state_name(auth->state));
	/* [한국어] "state": "<state name>". */
	digest = spdk_nvme_dhchap_get_digest_name(auth->digest);
	/* [한국어] digest enum→문자열. -1(미협상) 시 NULL 반환. */
	spdk_json_write_named_string(w, "digest", digest ? digest : "unknown");
	/* [한국어] NULL fallback "unknown". */
	dhgroup = spdk_nvme_dhchap_get_dhgroup_name(auth->dhgroup);
	/* [한국어] dhgroup enum→문자열. */
	spdk_json_write_named_string(w, "dhgroup", dhgroup ? dhgroup : "unknown");
	/* [한국어] NULL fallback. */
	spdk_json_write_object_end(w);
	/* [한국어] } 종료. */
}

/*
 * [한국어]
 * nvmf_auth_is_supported - 본 빌드에서 인증 기능을 지원하는지 확인.
 *
 * @return: true (항상).
 *
 * 본 파일은 OpenSSL 의존(libcrypto)으로만 컴파일 — 빌드된 시점에 항상 true.
 * stub 구현(빌드 옵션 OFF)은 별도 파일(auth_stub.c 또는 inline)에서 false 반환.
 *
 * 호출 컨텍스트: 모든 컨텍스트.
 *
 * 호출 체인:
 *   ctrlr.c::nvmf_qpair_handle_connect → [이 함수] (atr/ascr 강제 검증 시).
 */
bool
nvmf_auth_is_supported(void)
{
	return true;
	/* [한국어] OpenSSL 가용 시 항상 true. */
}
SPDK_LOG_REGISTER_COMPONENT(nvmf_auth)
/* [한국어] "nvmf_auth" 로그 컴포넌트 등록 — AUTH_DEBUGLOG/AUTH_LOGDUMP 활성화 키.
 * SPDK CLI: --logflag=nvmf_auth 로 토글. 빌드 시 .gnu.linkonce.spdk_log_register 섹션에 자동 등록. */
