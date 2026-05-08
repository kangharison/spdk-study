/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation
 */

/*
 * [한국어 설명] NVMe-oF 빌드 옵션별 미사용 심볼 stub 모음 (stubs.c)
 *
 * === 파일의 역할 ===
 * SPDK NVMe-oF target은 빌드 시점에 ./configure 옵션으로 선택적으로 켜는 기능들이 있다.
 * 예: DH-HMAC-CHAP 인증(--with-evp-mac via OpenSSL EVP), RDMA 트랜스포트(--with-rdma),
 * mDNS publish(--with-avahi). 해당 옵션을 끄고 빌드하면 실제 구현 파일(auth.c, rdma.c,
 * mdns_server.c)이 컴파일에 포함되지 않으므로, 코어가 정적으로 호출하는 심볼들이 없어
 * 링크 에러가 난다. 이 파일은 그 빈자리를 채우는 **약화/no-op stub** 모음이다.
 * 모든 stub은 "기능 미지원" 상태를 적절한 NVMe 에러 또는 -ENOTSUP으로 알려준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 코어 코드 흐름에서 인증/RDMA/mDNS를 호출하는 지점:
 *   ctrlr.c (Connect) -> nvmf_qpair_auth_init() / nvmf_auth_request_exec()
 *   bdev_nvmf 또는 RPC -> spdk_nvmf_rdma_init_hooks()
 *   nvmf_rpc.c (mdns_prr_create RPC) -> nvmf_publish_mdns_prr() / nvmf_tgt_*_mdns_prr()
 * 빌드 옵션이 켜져 있으면 본 파일의 #ifndef 블록은 컴파일되지 않고 진짜 구현이 사용된다.
 * 빌드 옵션이 꺼져 있으면 stub이 컴파일되어 코어가 호출 시 우아한 미지원 처리를 한다.
 * 실행 컨텍스트: 호출하는 코어 함수와 동일 (대부분 SPDK reactor 스레드).
 *
 * === 타 모듈과의 연결 ===
 * - spdk/config.h: ./configure가 생성한 SPDK_CONFIG_* 매크로 (HAVE_EVP_MAC, RDMA, AVAHI)
 * - spdk/log.h: SPDK_ERRLOG/SPDK_LOG_REGISTER_COMPONENT - 미지원 호출 추적용 로깅
 * - spdk/nvmf_transport.h: spdk_nvme_rdma_hooks 등 RDMA 공개 타입
 * - nvmf_internal.h: 코어가 의존하는 stub 시그니처 (nvmf_qpair_auth_init 등) 선언
 *
 * === 주요 함수/구조체 요약 ===
 * - nvmf_qpair_auth_init/destroy/dump: DH-HMAC-CHAP 인증 - 미지원 시 init이 -ENOTSUP
 * - nvmf_auth_request_exec: AUTH Send/Receive 캡슐 처리 - 미지원 시 INVALID_OPCODE 응답
 * - nvmf_auth_is_supported: 인증 가용성 질의 - 항상 false 반환
 * - spdk_nvmf_rdma_init_hooks: RDMA 후크 설치 - RDMA 미빌드 시 abort (잘못된 호출)
 * - nvmf_publish_mdns_prr / nvmf_tgt_stop_mdns_prr / nvmf_tgt_update_mdns_prr:
 *   Avahi 미빌드 시 mDNS publish API들이 no-op 또는 -ENOTSUP을 반환
 */

#include "spdk/config.h"                            /* [한국어] ./configure가 생성한 SPDK_CONFIG_* 매크로 - 빌드 옵션 분기 */
#include "spdk/log.h"                               /* [한국어] SPDK_ERRLOG / SPDK_LOG_REGISTER_COMPONENT 사용 */
#include "spdk/nvmf_transport.h"                    /* [한국어] spdk_nvme_rdma_hooks 타입 등 트랜스포트 공개 정의 */

#include "nvmf_internal.h"                          /* [한국어] stub이 구현하는 코어 내부 선언(nvmf_qpair_auth_init 등) 가져오기 */

#ifndef SPDK_CONFIG_HAVE_EVP_MAC                    /* [한국어] OpenSSL EVP MAC 미지원 빌드 - DH-HMAC-CHAP 인증 코드(auth.c) 미컴파일 */
/*
 * [한국어]
 * nvmf_qpair_auth_init - qpair용 DH-HMAC-CHAP 인증 컨텍스트 초기화 (stub)
 *
 * @qpair: 인증을 시작할 qpair
 * @return: 항상 -ENOTSUP (인증 기능이 빌드되지 않음)
 *
 * 본래 auth.c의 이 함수는 qpair->auth 컨텍스트를 할당하고 챌린지/응답 상태를 준비한다.
 * EVP_MAC가 없는 빌드에서는 인증 자체가 불가능하므로 호출자(ctrlr Connect 처리 경로)가
 * 인증 비요구 호스트만 수락하도록 -ENOTSUP을 반환한다.
 *
 * 호출 체인:
 *   ctrlr.c Connect 처리 -> [본 stub] -> 호출자가 인증 필요 시 Connect 거부
 */
int
nvmf_qpair_auth_init(struct spdk_nvmf_qpair *qpair)
{
	return -ENOTSUP;                            /* [한국어] EVP MAC 미빌드 - 인증 컨텍스트 생성 불가 */
}

/*
 * [한국어]
 * nvmf_qpair_auth_destroy - qpair 인증 컨텍스트 해제 (stub)
 *
 * @qpair: 해제 대상 qpair
 *
 * 인증이 활성화된 적이 없으므로 qpair->auth는 항상 NULL이어야 한다.
 * assert로 방어해 코드 경로 오류를 잡는다.
 */
void
nvmf_qpair_auth_destroy(struct spdk_nvmf_qpair *qpair)
{
	assert(qpair->auth == NULL);                /* [한국어] auth_init이 항상 실패했으므로 컨텍스트는 NULL이어야 - 누군가가 우회 설정했다면 버그 */
}

/*
 * [한국어]
 * nvmf_qpair_auth_dump - qpair 인증 상태를 JSON으로 덤프 (stub)
 *
 * @qpair: 대상 qpair
 * @w: SPDK JSON writer
 *
 * RPC nvmf_get_qpair에서 호출. 인증 미빌드이므로 출력할 정보 없음 - no-op.
 */
void
nvmf_qpair_auth_dump(struct spdk_nvmf_qpair *qpair, struct spdk_json_write_ctx *w)
{
}

/*
 * [한국어]
 * nvmf_auth_request_exec - AUTH Send/Receive 캡슐 처리 (stub)
 *
 * @req: 호스트가 보낸 AUTH 명령 요청
 * @return: SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS - complete를 호출했으므로 호출자에게 비동기로 알림
 *
 * NVMe-oF 1.1+ DH-HMAC-CHAP은 Connect 직후 AUTH_Send/AUTH_Receive 캡슐로 챌린지를 교환한다.
 * 인증 미빌드 시 호스트가 이 명령을 보내도 처리할 수 없어 NVMe Generic Status:
 * Invalid Opcode (sct=0x00 / sc=0x01)로 응답한다 (NVMe Base Spec, Status Code Type=Generic).
 *
 * 호출 체인:
 *   ctrlr.c -> 코어 명령 디스패치 -> [본 stub] -> spdk_nvmf_request_complete -> 트랜스포트 송신
 */
int
nvmf_auth_request_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *cpl = &req->rsp->nvme_cpl; /* [한국어] 응답 캡슐의 NVMe Completion 영역 포인터 - 여기에 status를 채움 */

	cpl->status.sct = SPDK_NVME_SCT_GENERIC;    /* [한국어] Status Code Type = Generic Command Status (0x0) - NVMe Base Spec Fig. */
	cpl->status.sc = SPDK_NVME_SC_INVALID_OPCODE; /* [한국어] Status Code = Invalid Opcode (0x01) - 호스트에 "지원되지 않는 명령" 알림 */

	spdk_nvmf_request_complete(req);            /* [한국어] 응답을 트랜스포트 큐로 비동기 송신 - 내부에서 transport->ops->req_complete 호출 */

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS; /* [한국어] 호출자에게 "이미 complete를 트리거했음" 알림 - 동기 반환과 구분 */
}

/*
 * [한국어]
 * nvmf_auth_is_supported - 인증 기능 가용성 질의 (stub)
 *
 * @return: 항상 false (EVP MAC 미빌드)
 *
 * 코어가 인증 강제 옵션을 검증할 때 호출. false면 인증 강제 옵션은 거부됨.
 */
bool
nvmf_auth_is_supported(void)
{
	return false;                               /* [한국어] 인증 코드 미빌드 - 항상 미지원 */
}

SPDK_LOG_REGISTER_COMPONENT(nvmf_auth)              /* [한국어] auth 컴포넌트 로그 카테고리 등록 - 진짜 auth.c가 빠지면 stub에서 대신 등록해야 다른 곳의 SPDK_LOG가 깨지지 않음 */
#endif /* !SPDK_CONFIG_HAVE_EVP_MAC */              /* [한국어] EVP MAC stub 블록 끝 */

#ifndef SPDK_CONFIG_RDMA                            /* [한국어] RDMA 트랜스포트 미빌드 - rdma.c 미컴파일 */
/*
 * [한국어]
 * spdk_nvmf_rdma_init_hooks - RDMA hook(메모리 등록 콜백 등) 설치 (stub)
 *
 * @hooks: 사용자 정의 RDMA hook 구조체
 *
 * 본래는 RDMA 트랜스포트가 외부 메모리 관리자(예: cuda_pinned_memory)와 연동하기 위한
 * 콜백을 등록한다. RDMA 미빌드인데 호출됐다면 빌드 구성 오류이므로 abort로 즉시 종료해
 * 잘못된 가정을 일찍 드러낸다 (silent no-op이면 디버깅이 어려움).
 */
void
spdk_nvmf_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks)
{
	SPDK_ERRLOG("spdk_nvmf_rdma_init_hooks() is unsupported: RDMA transport is not available\n"); /* [한국어] 미빌드 호출 명시 - 빌드 구성 점검 유도 */
	abort();                                    /* [한국어] 명시적 abort - 잘못된 빌드 조합을 일찍 탐지하기 위함 */
}
#endif /* !SPDK_CONFIG_RDMA */                      /* [한국어] RDMA stub 블록 끝 */

#ifndef SPDK_CONFIG_AVAHI                           /* [한국어] Avahi(mDNS) 미빌드 - mdns_server.c의 본 구현이 미컴파일 */
/*
 * [한국어]
 * nvmf_publish_mdns_prr - mDNS Pull Registration Request publish (stub)
 *
 * @tgt: 대상 NVMe-oF target
 * @return: -ENOTSUP (Avahi 미빌드)
 *
 * NVMe-oF Discovery 자동 발견을 위해 _nvme-disc._tcp 서비스를 mDNS로 publish하는 기능.
 * Avahi 라이브러리 부재 시 호출 자체를 거부한다.
 */
int
nvmf_publish_mdns_prr(struct spdk_nvmf_tgt *tgt)
{
	SPDK_ERRLOG("nvmf_publish_mdns_prr is supported when built with the --with-avahi option\n"); /* [한국어] 빌드 옵션 안내 메시지 */

	return -ENOTSUP;                            /* [한국어] mDNS publish 불가 - 호출자(RPC)가 에러 응답 송출 */
}

/*
 * [한국어]
 * nvmf_tgt_stop_mdns_prr - mDNS publish 중단 (stub)
 *
 * @tgt: 대상 target
 *
 * 진짜 구현은 Avahi entry group을 reset/free한다. 미빌드 시 publish 자체가 불가능했으므로 no-op.
 * 코어 종료 경로에서 무조건 호출되는 정리 함수이므로 에러 없이 조용히 반환해야 함.
 */
void
nvmf_tgt_stop_mdns_prr(struct spdk_nvmf_tgt *tgt)
{
}

/*
 * [한국어]
 * nvmf_tgt_update_mdns_prr - listener 변경 시 mDNS 엔트리 갱신 (stub)
 *
 * @tgt: 대상 target
 * @return: 0 (no-op)
 *
 * 진짜 구현은 listener 추가/삭제 시 호출되어 Avahi 엔트리를 reset 후 재등록한다.
 * 미빌드 시 0(성공처럼)을 반환해 코어 흐름을 깨뜨리지 않는다.
 */
int
nvmf_tgt_update_mdns_prr(struct spdk_nvmf_tgt *tgt)
{
	return 0;                                   /* [한국어] no-op이지만 호출자(예: listener add 경로)가 실패로 인식하지 않도록 0 반환 */
}
#endif /* !SPDK_CONFIG_AVAHI */                     /* [한국어] Avahi stub 블록 끝 */
