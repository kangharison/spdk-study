/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation.  All rights reserved.
 */

/*
 * [한국어 설명] SPDK 빌드 옵션 비활성화 시 사용되는 stub 함수 모음 (nvme_stubs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK가 특정 선택적 기능(NVMe CUSE, RDMA 트랜스포트, OpenSSL EVP MAC
 * 기반 in-band authentication)을 빌드 시점에 비활성화했을 때, 그 기능을 제공하는
 * 공개 API들이 링크 단계에서 unresolved symbol이 되지 않도록 **빈 stub 구현**을
 * 제공한다. 모든 stub은 호출되면 SPDK_ERRLOG로 "unsupported"를 기록하고
 * `-ENOTSUP`을 반환하거나(일부는 `abort()` — RDMA hooks 초기화처럼 호출되면
 * 안 되는 함수) 즉시 빠져나오는 방식으로 동작한다. 즉 "기능은 컴파일에서
 * 빠졌지만 ABI/링크 호환성은 유지"하기 위한 안전망이다.
 * 보호 대상 매크로:
 *   - SPDK_CONFIG_NVME_CUSE   : CUSE(Character device in Userspace) 기반
 *                                /dev/nvmeX 노드 노출 기능. 제외 시 5개 함수 stub.
 *   - SPDK_CONFIG_RDMA        : RDMA 트랜스포트(NVMe-oF용). 제외 시 1개 함수 stub.
 *   - SPDK_CONFIG_HAVE_EVP_MAC: OpenSSL 3.x EVP_MAC API 가용 여부. 제외 시
 *                                NVMe in-band authentication 3개 함수 stub.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (예시 — CUSE 미빌드 시):
 *   [User app] spdk_nvme_cuse_register(ctrlr)
 *     → [link 시 결정] CUSE 빌드 O면 module/nvme/cuse/nvme_cuse.c의 본 구현
 *                      CUSE 빌드 X면 이 파일의 stub → SPDK_ERRLOG → -ENOTSUP
 *
 * SPDK는 빌드 타임 옵션을 `mk/config.mk` → `include/spdk/config.h`로 전달하고,
 * Makefile은 SPDK_CONFIG_NVME_CUSE/RDMA/HAVE_EVP_MAC 값에 따라 본 구현 .c
 * 파일을 빌드 대상에 포함하거나 제외한다. 제외된 경우에도 `lib/nvme/libspdk_nvme.so`가
 * 공개 헤더에 선언된 심볼을 모두 정의해야 하므로, 이 stubs.c가 빈 정의를 제공한다.
 * 실행 컨텍스트: 호출자 스레드 — 일반적으로 ctrlr attach 코드 또는 NVMe-oF
 * 초기화 경로. 실제 동작이 없으므로 동기화/락 고려 없음.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/config.h: SPDK_CONFIG_* 매크로 정의 (configure 단계 산물).
 *   - spdk/log.h:    SPDK_ERRLOG 매크로 — 기능이 빌드되지 않았음을 사용자에게 알림.
 *   - spdk/nvme.h:   spdk_nvme_ctrlr/qpair, spdk_nvme_authenticate_cb 등 공개 API
 *                     선언 (stub의 시그니처가 본 구현과 일치해야 함).
 *   - spdk/stdinc.h: errno, size_t 등 표준 타입.
 *   - nvme_internal.h: nvme_fabric_qpair_authenticate_async/poll의 내부 선언
 *                       (Fabrics 인증 경로에서 호출됨).
 * 의존하는 측: lib/nvme/libspdk_nvme.so의 링크 — 본 구현이 빠진 빌드 구성.
 * 데이터 흐름: 없음 (호출 즉시 에러 반환 또는 abort).
 * 공유 자료구조: 없음.
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_nvme_cuse_get_ctrlr_name / get_ns_name / register / unregister
 *     / update_namespaces : CUSE 디바이스 노드 stub. 모두 -ENOTSUP 반환.
 *   - spdk_nvme_rdma_init_hooks : RDMA hook 등록 stub. 호출되면 abort()
 *     (이 함수는 RDMA 트랜스포트가 빌드 안 됐는데 호출하는 것 자체가 버그이므로 fail-fast).
 *   - nvme_fabric_qpair_authenticate_async / poll : Fabrics 인증 내부 stub. -ENOTSUP.
 *   - spdk_nvme_qpair_authenticate : 인증 공개 API stub. -ENOTSUP.
 */

#include "spdk/config.h"        /* [한국어] SPDK_CONFIG_* 매크로 — configure가 만든 빌드 옵션 정의 */
#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG 등 로깅 매크로 — 미지원 호출 시 사용자에게 경고 */
#include "spdk/nvme.h"          /* [한국어] 공개 NVMe API 선언 — stub 시그니처가 본 구현과 일치해야 함 */
#include "spdk/stdinc.h"        /* [한국어] 표준 헤더(errno, size_t 등) 일괄 포함 */
#include "nvme_internal.h"      /* [한국어] nvme_fabric_qpair_authenticate_* 내부 함수 선언 */

#ifndef SPDK_CONFIG_NVME_CUSE
/* [한국어] === CUSE(Character device in Userspace) 미빌드 분기 ===
 *   SPDK NVMe CUSE는 SPDK가 점유한 컨트롤러를 /dev/nvmeXnY로 노출하여
 *   기존 nvme-cli 등 Linux 도구가 ioctl로 접근 가능하게 함. libfuse/cuse 의존.
 *   이 매크로가 정의되지 않은 빌드(libfuse 미설치 등)에서는 아래 stub들이 사용된다. */

/*
 * [한국어]
 * spdk_nvme_cuse_get_ctrlr_name - CUSE가 노출한 컨트롤러 디바이스 이름 조회 (stub)
 *
 * @ctrlr: 대상 컨트롤러 (stub에서는 사용하지 않음)
 * @name:  결과 이름이 기록될 버퍼 (stub에서는 사용하지 않음)
 * @size:  버퍼 크기 in/out (stub에서는 사용하지 않음)
 * @return: 항상 -ENOTSUP
 *
 * 본 구현(module/nvme/cuse/nvme_cuse.c)에서는 등록된 CUSE 디바이스의
 * "nvmeX" 형식 이름을 반환. CUSE 빌드 OFF면 이 stub이 링크되어 호출 즉시
 * 에러 로그와 함께 미지원을 알린다.
 */
int
spdk_nvme_cuse_get_ctrlr_name(struct spdk_nvme_ctrlr *ctrlr, char *name, size_t *size)
{
	SPDK_ERRLOG("spdk_nvme_cuse_get_ctrlr_name() is unsupported\n");
	                                /* [한국어] 미지원 호출 사실을 사용자 가시 로그로 남김 — 디버깅 단서 */
	return -ENOTSUP;                /* [한국어] POSIX errno: Operation not supported — 호출자가 기능 부재로 처리 */
}

/*
 * [한국어]
 * spdk_nvme_cuse_get_ns_name - CUSE가 노출한 namespace 디바이스 이름 조회 (stub)
 *
 * @ctrlr: 대상 컨트롤러
 * @nsid:  대상 namespace ID (1~)
 * @name:  결과 이름 버퍼
 * @size:  버퍼 크기 in/out
 * @return: 항상 -ENOTSUP
 *
 * 본 구현은 "nvmeXnY" 형태 이름을 반환. stub은 미지원 로그 후 -ENOTSUP.
 */
int
spdk_nvme_cuse_get_ns_name(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, char *name, size_t *size)
{
	SPDK_ERRLOG("spdk_nvme_cuse_get_ns_name() is unsupported\n");
	                                /* [한국어] 미지원 알림 로그 */
	return -ENOTSUP;                /* [한국어] 미지원 반환 */
}

/*
 * [한국어]
 * spdk_nvme_cuse_register - 컨트롤러를 CUSE 디바이스로 등록 (stub)
 *
 * @ctrlr: 대상 컨트롤러
 * @return: 항상 -ENOTSUP
 *
 * 본 구현은 /dev/nvmeX 캐릭터 디바이스를 생성하고 ioctl 핸들러를 연결한다.
 * stub은 호출 자체를 거부하여 nvme-cli 호환 경로가 사용 불가임을 알림.
 */
int
spdk_nvme_cuse_register(struct spdk_nvme_ctrlr *ctrlr)
{
	SPDK_ERRLOG("spdk_nvme_cuse_register() is unsupported\n");
	                                /* [한국어] CUSE 비활성 빌드임을 알리는 로그 */
	return -ENOTSUP;                /* [한국어] 등록 거부 */
}

/*
 * [한국어]
 * spdk_nvme_cuse_unregister - CUSE 등록 해제 (stub)
 *
 * @ctrlr: 대상 컨트롤러
 * @return: 항상 -ENOTSUP
 *
 * 본 구현은 /dev/nvmeX 노드와 fuse 세션을 정리. stub은 동작 없음.
 */
int
spdk_nvme_cuse_unregister(struct spdk_nvme_ctrlr *ctrlr)
{
	SPDK_ERRLOG("spdk_nvme_cuse_unregister() is unsupported\n");
	                                /* [한국어] 미지원 로그 */
	return -ENOTSUP;                /* [한국어] 미지원 반환 */
}

/*
 * [한국어]
 * spdk_nvme_cuse_update_namespaces - namespace 변동을 CUSE 노드에 반영 (stub, void)
 *
 * @ctrlr: 대상 컨트롤러
 *
 * 본 구현은 NS Attach/Detach 후 /dev/nvmeXnY 노드를 추가/삭제. stub은 로그만 남김.
 * 반환형이 void이므로 -ENOTSUP을 반환할 수 없어 ERRLOG로만 알린다.
 */
void
spdk_nvme_cuse_update_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	SPDK_ERRLOG("spdk_nvme_cuse_update_namespaces() is unsupported\n");
	                                /* [한국어] void 반환 — 호출자가 결과 코드를 받지 않으므로 로그가 유일한 통지 수단 */
}
#endif /* !SPDK_CONFIG_NVME_CUSE */
                                  /* [한국어] CUSE 미빌드 분기 종료 */

#ifndef SPDK_CONFIG_RDMA
/* [한국어] === RDMA 트랜스포트 미빌드 분기 ===
 *   SPDK NVMe-oF의 RDMA 트랜스포트(InfiniBand/RoCE)를 위한 libibverbs/librdmacm
 *   라이브러리가 없거나 비활성화된 빌드 구성. RDMA 트랜스포트 자체가 링크되지
 *   않으므로, 그 hook 등록 함수가 호출되는 것은 사용자 코드 버그로 본다. */

/*
 * [한국어]
 * spdk_nvme_rdma_init_hooks - 사용자 정의 RDMA hook 등록 (stub — abort)
 *
 * @hooks: 사용자가 제공한 hook 디스크립터 (memory registration callback 등)
 *
 * 본 구현(lib/nvme/nvme_rdma.c)은 호스트가 자체 MR 캐시·메모리 등록 정책을
 * 주입할 수 있게 한다. RDMA가 빌드되지 않았는데 이 함수가 호출되었다면
 * 사용자 코드가 잘못된 가정을 하고 있는 것이므로 **abort()**로 즉시 중단한다.
 * 다른 stub들이 -ENOTSUP을 반환하는 것과 달리 fail-fast 정책을 취한 이유:
 *   - 반환형이 void라 에러 신호 전달 불가
 *   - hook 등록은 초기화 시점이며, 잘못 등록된 채 RDMA 경로를 시도하면
 *     훨씬 진단하기 어려운 후속 오류로 이어지므로 차라리 즉시 죽음
 */
void
spdk_nvme_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks)
{
	SPDK_ERRLOG("spdk_nvme_rdma_init_hooks() is unsupported: RDMA transport is not available\n");
	                                /* [한국어] abort 직전에 원인을 명확히 남김 — 코어 덤프와 함께 진단 단서 */
	abort();                        /* [한국어] 즉시 SIGABRT — RDMA 미빌드 환경에서 hook 등록 호출은 사용자 측 버그 */
}
#endif /* !SPDK_CONFIG_RDMA */
                                  /* [한국어] RDMA 미빌드 분기 종료 */

#ifndef SPDK_CONFIG_HAVE_EVP_MAC
/* [한국어] === OpenSSL EVP_MAC 비가용 분기 ===
 *   NVMe in-band authentication(DH-HMAC-CHAP — TP 8006)은 OpenSSL 3.x의
 *   EVP_MAC API에 의존한다. 빌드 환경의 OpenSSL이 구버전이거나 EVP_MAC를
 *   제공하지 않으면 SPDK_CONFIG_HAVE_EVP_MAC가 정의되지 않으며, 인증 경로 전체가
 *   stub으로 대체된다. */

/*
 * [한국어]
 * nvme_fabric_qpair_authenticate_async - Fabrics qpair 인증 비동기 시작 (stub)
 *
 * @qpair: 대상 Fabrics qpair (인증 진행 중 상태로 전이되어야 할 대상)
 * @return: 항상 -ENOTSUP
 *
 * 본 구현은 DH-HMAC-CHAP 챌린지/응답 프로토콜을 NVMe-oF Connect 직후 수행한다.
 * EVP_MAC가 없으면 인증 자체를 시작할 수 없으므로 -ENOTSUP. 호출자는
 * lib/nvme/nvme_fabric.c의 connect 경로에서 이 결과를 보고 인증 미지원으로
 * 처리하거나 컨트롤러 attach 실패 처리.
 */
int
nvme_fabric_qpair_authenticate_async(struct spdk_nvme_qpair *qpair)
{
	SPDK_ERRLOG("NVMe in-band authentication is unsupported\n");
	                                /* [한국어] 인증 미지원 알림 — Connect 시 secure transport 요구 시 fail */
	return -ENOTSUP;                /* [한국어] 호출자 fabric.c가 이 코드로 인증 단계 스킵/실패 결정 */
}

/*
 * [한국어]
 * nvme_fabric_qpair_authenticate_poll - 진행 중인 Fabrics 인증의 폴 단계 (stub)
 *
 * @qpair: 인증 폴 대상 qpair
 * @return: 항상 -ENOTSUP
 *
 * 본 구현은 챌린지 응답 RX/TX 단계를 polled-mode로 진행하며 0(완료) 또는
 * 음수 errno(에러)/-EAGAIN(아직 진행 중) 같은 값을 반환한다. stub은 항상 미지원.
 * 이 stub은 ERRLOG를 남기지 않는데, async가 이미 -ENOTSUP을 반환했다면
 * poll 호출은 실수로 들어온 것이므로 로그 폭주 방지를 위해 조용히 반환만 한다.
 */
int
nvme_fabric_qpair_authenticate_poll(struct spdk_nvme_qpair *qpair)
{
	return -ENOTSUP;                /* [한국어] 로그 없이 즉시 미지원 — 폴 루프에서 매 호출 로그를 찍는 폭주 방지 */
}

/*
 * [한국어]
 * spdk_nvme_qpair_authenticate - 사용자가 직접 트리거하는 qpair 인증 공개 API (stub)
 *
 * @qpair:  대상 qpair (PCIe면 의미 없음 — Fabrics 전용)
 * @cb_fn:  인증 완료 콜백 (성공/실패 여부 통지)
 * @cb_ctx: 콜백 컨텍스트
 * @return: 항상 -ENOTSUP
 *
 * 본 구현은 비동기 인증을 시작하고 완료 시 cb_fn을 호출한다. EVP_MAC 미지원
 * 빌드에서는 즉시 -ENOTSUP을 반환하며 콜백은 호출되지 않으므로 호출자는
 * 음수 반환값으로 콜백 등록을 취소한 것으로 처리해야 한다.
 */
int
spdk_nvme_qpair_authenticate(struct spdk_nvme_qpair *qpair,
			     spdk_nvme_authenticate_cb cb_fn, void *cb_ctx)
{
	SPDK_ERRLOG("NVMe in-band authentication is unsupported\n");
	                                /* [한국어] 사용자 가시 API이므로 명시적으로 로그 — 빌드 옵션 부재가 원인 */
	return -ENOTSUP;                /* [한국어] 콜백 미실행 의미 — 호출자가 cb_ctx 정리 책임 */
}
#endif /* !SPDK_CONFIG_HAVE_EVP_MAC */
                                  /* [한국어] EVP_MAC 미가용 분기 종료 */
