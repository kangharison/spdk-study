/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SIGBUS 시그널 핸들러를 통한 PCI/MMIO 접근 오류 라우팅 (sigbus_handler.c)
 *
 * === 파일의 역할 ===
 * SPDK는 PCIe 디바이스의 BAR을 mmap으로 유저스페이스에 노출하여 MMIO doorbell·레지스터를
 * 직접 읽고 쓴다. 이 영역에 접근하던 중 디바이스가 surprise-removal(핫 언플러그)되거나
 * 링크가 끊기면 커널은 해당 페이지에 대해 SIGBUS를 던진다. 본 파일은 그 SIGBUS를 잡아
 * 등록된 모든 PCI 에러 핸들러(spdk_pci_error_handler) 콜백에 si_addr를 전달함으로써,
 * NVMe 같은 상위 모듈이 적절한 정리(컨트롤러 재설정, qpair invalidate 등)를 수행하도록
 * 라우팅한다. 이는 polled-mode에서 디바이스 사라짐을 감지하는 가장 핵심적인 메커니즘이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/env_dpdk 서브시스템의 시그널 어댑터. __attribute__((constructor))로 프로세스 시작
 * 시 자동으로 sigaction(SIGBUS)을 등록하므로 spdk_env_init보다도 먼저 동작한다.
 * 호출 체인: 커널 → SIGBUS → sigbus_fault_sighandler → 등록된 콜백들 (예: NVMe 모듈의 PCI
 * surprise-removal 핸들러) → spdk_nvme_ctrlr_fail 등.
 * 실행 컨텍스트: SIGBUS는 어느 스레드에서나 발생할 수 있으며 시그널 컨텍스트(async-signal
 * 환경)에서 핸들러가 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/env.h(spdk_pci_error_handler 타입), spdk/log.h, libpthread, signal.h.
 * - 본 파일을 사용하는 모듈: lib/nvme(NVMe 컨트롤러의 BAR 접근 보호), virtio, ioat 등.
 * - 데이터 흐름: PCI 디바이스 attach 시 모듈은 spdk_pci_register_error_handler로 콜백을 큐에
 *   등록한다. 추후 SIGBUS 발생 시 si_addr(접근하려던 주소)와 ctx가 콜백에 전달되며, 콜백은
 *   주소를 자기 BAR 범위와 비교해 자기 디바이스의 사라짐 여부를 판단한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct sigbus_handler: 단일 핸들러 등록 항목. 콜백 함수 포인터와 ctx, TAILQ 링크를 가짐.
 * - sigbus_fault_sighandler: SIGBUS의 sa_sigaction. 모든 등록 콜백을 순회 호출.
 * - device_set_signal: __attribute__((constructor)). 프로세스 로드 시 SIGBUS 핸들러 등록.
 * - device_destroy_signal: __attribute__((destructor)). 프로세스 종료 시 핸들러 큐 정리.
 * - spdk_pci_register_error_handler / unregister: 외부 노출 등록·해제 API.
 */

/* [한국어] SPDK 표준 인클루드 묶음. pthread_mutex_t, calloc, free, errno 등에 필요. */
#include "spdk/stdinc.h"
/* [한국어] spdk_pci_error_handler 타입과 본 파일의 공개 API 시그니처가 정의됨. */
#include "spdk/env.h"
/* [한국어] SPDK_ERRLOG 매크로 사용을 위해 포함. */
#include "spdk/log.h"

/*
 * [한국어]
 * struct sigbus_handler - 단일 SIGBUS 콜백 등록 항목
 *
 * 등록된 콜백들은 g_sigbus_handler TAILQ에 매달려 있으며, SIGBUS 발생 시 모두 순회 호출된다.
 */
struct sigbus_handler {
	spdk_pci_error_handler func;
	/* [한국어] 사용자 콜백 함수 포인터.
	 * 설정자: spdk_pci_register_error_handler 호출 시 입력 sighandler 인자를 그대로 저장.
	 * 읽는 자: sigbus_fault_sighandler가 SIGBUS 발생 시 호출.
	 * 값 범위: 유효한 함수 포인터(NULL은 등록 단계에서 거부됨).
	 * 동기화: g_sighandler_mutex로 보호되는 큐에 매달리지만, 핸들러 실행 자체는
	 *         시그널 컨텍스트라는 점에 주의(콜백 내부에서 async-signal-safe 함수만 사용해야 함). */
	void *ctx;
	/* [한국어] func 호출 시 두 번째 인자로 전달되는 사용자 컨텍스트.
	 * 설정자: register 호출자가 제공.
	 * 읽는 자: func 본체. 보통 디바이스 핸들·BAR 범위 등을 담음.
	 * 값 범위: 임의 포인터(NULL 허용).
	 * 동기화: 등록 후 변경 없음. */

	TAILQ_ENTRY(sigbus_handler) tailq;
	/* [한국어] g_sigbus_handler 큐 링크.
	 * 설정자: TAILQ_INSERT_TAIL(register).
	 * 읽는 자: TAILQ_FOREACH(시그널 핸들러), TAILQ_REMOVE(unregister).
	 * 값 범위: TAILQ 매크로 내부에서 관리.
	 * 동기화: g_sighandler_mutex로 보호. */
};

/* [한국어] 큐 보호용 뮤텍스. register/unregister와 시그널 핸들러 간 경합 차단.
 * 주의: 시그널 핸들러가 mutex_lock을 호출하므로 엄밀히는 async-signal-safe가 아니지만,
 * SPDK는 SIGBUS가 매우 드문 이벤트이고 자가 데드락 가능성이 낮다는 가정으로 운용한다. */
static pthread_mutex_t g_sighandler_mutex = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] 모든 등록 콜백을 잇는 TAILQ. SPDK 전 영역에서 단 한 개의 전역 큐만 사용. */
static TAILQ_HEAD(, sigbus_handler) g_sigbus_handler =
	TAILQ_HEAD_INITIALIZER(g_sigbus_handler);

/*
 * [한국어]
 * sigbus_fault_sighandler - SIGBUS의 sa_sigaction 핸들러
 *
 * @signum: 시그널 번호(SIGBUS 고정).
 * @info:   siginfo_t. info->si_addr가 잘못된 접근이 일어난 가상주소.
 * @ctx:    ucontext_t. 본 핸들러에서는 사용하지 않음.
 *
 * 등록된 모든 콜백을 순회하며 si_addr와 콜백별 ctx를 전달한다. 각 콜백은 si_addr가 자기
 * 관할 BAR 범위에 해당하는지 검사하여 자기 디바이스가 사라졌는지 판단한다.
 *
 * 실행 컨텍스트: 시그널 핸들러 컨텍스트. 어느 스레드에서나 발생 가능. 본 함수 내부에서는
 *                pthread_mutex_lock 사용 — async-signal-safe는 아니지만 SPDK가 드문
 *                이벤트로 간주하고 허용.
 *
 * 호출 체인: 커널 → sigbus_fault_sighandler → 사용자 콜백(func)
 */
static void
sigbus_fault_sighandler(int signum, siginfo_t *info, void *ctx)
{
	struct sigbus_handler *sigbus_handler;
	/* [한국어] 큐 순회용 임시 포인터. */

	pthread_mutex_lock(&g_sighandler_mutex);
	/* [한국어] 등록/해제와의 경합을 막기 위해 락 획득.
	 * 시그널 컨텍스트 한정 우려는 위 g_sighandler_mutex 주석 참고. */
	TAILQ_FOREACH(sigbus_handler, &g_sigbus_handler, tailq) {
		/* [한국어] 모든 등록 콜백을 순회하며 호출. 어느 디바이스의 BAR인지 알 수 없으므로
		 * 모두에게 전달하고, 각 콜백이 자기 BAR 범위와 si_addr를 비교한다. */
		sigbus_handler->func(info->si_addr, sigbus_handler->ctx);
	}
	pthread_mutex_unlock(&g_sighandler_mutex);
	/* [한국어] 락 해제. */
}

/*
 * [한국어]
 * device_set_signal - 프로세스 로드 시 자동 SIGBUS 핸들러 등록
 *
 * GCC __attribute__((constructor))로 표시되어 main() 진입 전에 자동 실행된다.
 * sigaction을 사용하여 SIGBUS에 대한 sa_sigaction(3-arg form) 핸들러를 설치하며,
 * SA_SIGINFO 플래그로 si_addr 등 추가 정보를 받을 수 있게 한다.
 *
 * 실행 컨텍스트: 프로세스 초기화 단계, 단일 스레드.
 *
 * 호출 체인: dynamic loader(.init_array) → device_set_signal → sigaction(SIGBUS, ...)
 */
__attribute__((constructor)) static void
device_set_signal(void)
{
	struct sigaction sa;
	/* [한국어] 시그널 동작 명세 구조체. */

	sa.sa_sigaction = sigbus_fault_sighandler;
	/* [한국어] 3-arg 핸들러 등록(sa_handler가 아닌 sa_sigaction 사용 — siginfo 전달 받기 위함). */
	sigemptyset(&sa.sa_mask);
	/* [한국어] 핸들러 실행 중 추가 차단할 시그널 없음(빈 마스크). */
	sa.sa_flags = SA_SIGINFO;
	/* [한국어] siginfo_t 전달을 활성화. SA_RESTART 등은 의도적으로 미설정. */
	sigaction(SIGBUS, &sa, NULL);
	/* [한국어] 커널에 SIGBUS 핸들러 설치. 기존 핸들러는 무시(oldact=NULL). */
}

/*
 * [한국어]
 * device_destroy_signal - 프로세스 종료 시 등록 큐 정리
 *
 * GCC __attribute__((destructor))로 main() 반환 직후/atexit 후에 호출된다.
 * 큐에 남은 미해제 sigbus_handler 노드들을 free하여 leak 검출 도구에서 청결하게 종료되게 한다.
 *
 * 실행 컨텍스트: 프로세스 종료 단계, 단일 스레드.
 */
__attribute__((destructor)) static void
device_destroy_signal(void)
{
	struct sigbus_handler *sigbus_handler, *tmp;
	/* [한국어] 안전 순회를 위한 현재 노드와 다음 노드 임시 변수. */

	TAILQ_FOREACH_SAFE(sigbus_handler, &g_sigbus_handler, tailq, tmp) {
		/* [한국어] 순회 중 free 가능한 SAFE 변형 사용. */
		free(sigbus_handler);
		/* [한국어] 노드 메모리 반환. TAILQ_REMOVE는 굳이 하지 않음(어차피 큐 자체가 곧 사라짐). */
	}
}

/*
 * [한국어]
 * spdk_pci_register_error_handler - 사용자 SIGBUS 콜백 등록
 *
 * @sighandler: 호출할 콜백 함수. NULL 금지.
 * @ctx:        콜백 호출 시 두 번째 인자로 전달될 컨텍스트.
 * @return:     성공 시 0, 잘못된 인자/중복 등록 시 -EINVAL, 메모리 부족 시 -ENOMEM.
 *
 * 동일 함수 포인터로 두 번 등록하는 것을 막기 위해 큐를 선형 검색한 뒤 새 노드를 calloc하여
 * 큐 끝에 매단다.
 *
 * 실행 컨텍스트: 보통 PCI 드라이버의 attach/probe 콜백(메인 스레드).
 *
 * 호출 체인: spdk_pci_device_attach → 모듈 probe → spdk_pci_register_error_handler
 */
int
spdk_pci_register_error_handler(spdk_pci_error_handler sighandler, void *ctx)
{
	struct sigbus_handler *sigbus_handler;
	/* [한국어] 새로 만들 노드 또는 검색용 임시. */

	if (!sighandler) {
		/* [한국어] NULL 함수 포인터는 거부 — 시그널 핸들러에서 NULL 호출은 즉시 크래시. */
		SPDK_ERRLOG("Error handler is NULL\n");
		return -EINVAL;
	}

	pthread_mutex_lock(&g_sighandler_mutex);
	/* [한국어] 큐 검색·삽입 동안 경합 방지. */
	TAILQ_FOREACH(sigbus_handler, &g_sigbus_handler, tailq) {
		/* [한국어] 동일 함수 포인터 중복 등록 검사. */
		if (sigbus_handler->func == sighandler) {
			pthread_mutex_unlock(&g_sighandler_mutex);
			/* [한국어] 락을 풀고 에러 반환. */
			SPDK_ERRLOG("Error handler has been registered\n");
			return -EINVAL;
		}
	}
	pthread_mutex_unlock(&g_sighandler_mutex);
	/* [한국어] calloc 호출 전 락 해제 — 메모리 할당은 락 밖에서 수행하여 락 보유 시간을 줄임. */

	sigbus_handler = calloc(1, sizeof(*sigbus_handler));
	/* [한국어] 0으로 초기화된 노드 할당. 실패 시 NULL. */
	if (!sigbus_handler) {
		SPDK_ERRLOG("Failed to allocate sigbus handler\n");
		return -ENOMEM;
	}

	sigbus_handler->func = sighandler;
	/* [한국어] 콜백 함수 포인터 저장. */
	sigbus_handler->ctx = ctx;
	/* [한국어] 콜백 컨텍스트 저장. */

	pthread_mutex_lock(&g_sighandler_mutex);
	/* [한국어] 큐 삽입 동안 경합 방지. */
	TAILQ_INSERT_TAIL(&g_sigbus_handler, sigbus_handler, tailq);
	/* [한국어] 큐 끝에 추가. 순서는 등록 순. */
	pthread_mutex_unlock(&g_sighandler_mutex);
	/* [한국어] 락 해제. */

	return 0;
	/* [한국어] 등록 성공. */
}

/*
 * [한국어]
 * spdk_pci_unregister_error_handler - 등록된 SIGBUS 콜백 제거
 *
 * @sighandler: 이전에 등록된 함수 포인터. NULL이면 즉시 반환.
 *
 * 큐에서 동일 함수 포인터를 가진 첫 노드를 제거하고 free한다. 발견되지 않으면 무시.
 *
 * 실행 컨텍스트: PCI 드라이버 detach 콜백(메인 스레드).
 *
 * 호출 체인: spdk_pci_device_detach → 모듈 remove → spdk_pci_unregister_error_handler
 */
void
spdk_pci_unregister_error_handler(spdk_pci_error_handler sighandler)
{
	struct sigbus_handler *sigbus_handler;
	/* [한국어] 큐 검색용 임시. */

	if (!sighandler) {
		/* [한국어] NULL 입력은 무시(에러 반환 없이 종료) — register와 시그니처 대칭성을 위해. */
		return;
	}

	pthread_mutex_lock(&g_sighandler_mutex);
	/* [한국어] 큐 변경 보호. */
	TAILQ_FOREACH(sigbus_handler, &g_sigbus_handler, tailq) {
		/* [한국어] 첫 매칭 노드를 찾아 제거. 동일 함수 포인터 중복은 register에서 막혔으므로 1개뿐. */
		if (sigbus_handler->func == sighandler) {
			TAILQ_REMOVE(&g_sigbus_handler, sigbus_handler, tailq);
			/* [한국어] 큐에서 제거. */
			free(sigbus_handler);
			/* [한국어] 노드 메모리 반환. */
			pthread_mutex_unlock(&g_sighandler_mutex);
			/* [한국어] 락 해제 후 반환. */
			return;
		}
	}
	pthread_mutex_unlock(&g_sighandler_mutex);
	/* [한국어] 매칭 없음 — 그냥 락만 풀고 반환(이중 해제 방지 차원에서 silent). */
}
