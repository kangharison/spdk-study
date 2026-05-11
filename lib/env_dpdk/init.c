/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] DPDK EAL 초기화 - SPDK 환경 부팅 진입점 (init.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 의 환경 추상화(env) 계층 중 DPDK 백엔드의 "초기화/종료
 * 파이프라인" 을 정의한다. 사용자가 spdk_env_init(opts) 를 호출했을 때 다음
 * 의 일련 절차를 수행한다:
 *   (a) spdk_env_opts 의 값을 검사·기본값 채움(spdk_env_opts_init),
 *   (b) DPDK EAL(환경 추상화 라이브러리) 가 받아들이는 argv 형식의 커맨드라인을
 *       opts 로부터 합성(build_eal_cmdline),
 *   (c) OpenSSL 초기화(NVMe-TCP TLS, NVMe-oF auth 등에서 필요),
 *   (d) rte_eal_init() 호출 — 휴지페이지 매핑/lcore 스레드 생성/PCI 스캔 등
 *       DPDK 의 핵심 초기화,
 *   (e) SPDK 의 후처리(pci_env_init / mem_map_init / vtophys_init).
 * 또한 프로그램 종료 시 DPDK 의 rte_eal_cleanup 을 destructor 로 자동 호출해
 * hugepage/소켓/PMD 자원을 회수한다.
 * 이 파일은 NVMe SSD I/O 가 도달하기 이전, "환경 자체"를 만든다는 점에서
 * SPDK 의 가장 근본 계층이다 — 거의 모든 SPDK 애플리케이션이 처음으로 호출하는
 * 함수가 본 파일의 spdk_env_init() 이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 부팅 흐름:
 *   main()/spdk_app_start()
 *     → spdk_env_opts_init()    [opts 기본값]
 *     → spdk_env_init()         (← 본 파일)
 *         ├ build_eal_cmdline() : opts → DPDK argv
 *         ├ OPENSSL_init_ssl()  : 암호 라이브러리 부팅
 *         ├ rte_eal_init()      : DPDK 본 초기화(hugepage, lcore, PCI bus)
 *         └ spdk_env_dpdk_post_init():
 *               pci_env_init() / mem_map_init() / vtophys_init()
 *     → spdk_thread_lib_init() / spdk_reactors_init()
 *     → 사용자 subsystem(bdev, nvme 등) 초기화
 *     → reactor run loop (poller 실행)
 *
 * 종료 흐름:
 *   spdk_env_fini() → spdk_env_dpdk_post_fini()
 *     → vtophys_fini / mem_map_fini / pci_env_fini
 *     → 마지막에 destructor(101) dpdk_cleanup() 가 rte_eal_cleanup() 호출
 *
 * 실행 컨텍스트: 호스트 유저스페이스. spdk_env_init() 은 main 스레드(아직
 * reactor 가 만들어지기 전)에서 호출되며, 이후 SPDK 가 lcore 스레드를 만든다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존하는 모듈:
 *     env_internal.h           : 본 파일이 호출하는 mem_disable_huge_pages,
 *                                mem_enforce_numa, mem_disable_vtophys,
 *                                pci_env_init/fini, mem_map_init/fini,
 *                                vtophys_init/fini 등의 SPDK env 내부 API.
 *     spdk/env_dpdk.h          : 외부 EAL init 협업용 spdk_env_dpdk_post_init/fini 선언.
 *     spdk/log.h, spdk/string.h: 로그/문자열 헬퍼.
 *     spdk/version.h           : SPDK_VERSION_STRING (배너 출력).
 *     OpenSSL(libssl/libcrypto): TLS/암호 초기화. NVMe-oF 가 추후 사용.
 *     DPDK(librte_eal/...)     : rte_eal_init(), rte_eal_cleanup(),
 *                                rte_vfio_noiommu_is_enabled(), rte_version().
 * - 의존받는 모듈:
 *     app/, examples/          : 모든 SPDK 앱이 spdk_env_init 을 통해 부팅.
 *     lib/event(reactor)       : env 가 준비된 뒤 thread/reactor 생성.
 *     lib/nvme(pcie)           : DPDK PCI bus 위에서 NVMe 디바이스 probe.
 *     lib/bdev/* 모든 모듈     : 메모리(rte_malloc), DMA(vtophys) 사용.
 * - 데이터 흐름:
 *     사용자 opts → 본 파일에서 g_eal_cmdline (전역) 으로 변환 → DPDK 가
 *     이 argv 를 소비해 hugepage 매핑/PCI 스캔 → SPDK 후처리에서 vtophys
 *     테이블/PCI 디바이스 리스트 구성 → 이후 모듈이 spdk_dma_malloc() 등
 *     사용 시 hugepage 메모리에서 할당.
 * - 공유 자료구조:
 *     g_eal_cmdline / g_eal_cmdline_argcount : DPDK argv 백업본 — fini 에서
 *       해제. DPDK 자체가 argv 배열을 재배열하므로 별도 사본을 dpdk_args 에
 *       만들고 free 의 책임은 SPDK 측이 보유한다.
 *     g_external_init : true = SPDK 가 아직 EAL 을 초기화하지 않음(외부에서
 *       이미 했거나 처음부터 시도). false 로 바뀌면 본 파일이 EAL 의 소유자임.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_env_opts_init(opts)         : opts 구조체에 SPDK 기본값을 채움(opts_size 보존).
 * - build_eal_cmdline(opts) [static] : opts → DPDK 명령줄 인자 배열 합성.
 *                                      core mask, hugepage 옵션, IOMMU 모드,
 *                                      PCI allow/block, log level 등을 결정.
 * - spdk_env_init(opts_user)         : DPDK EAL + SPDK env 포스트 처리까지 부팅.
 * - spdk_env_dpdk_post_init(legacy_mem)
 *                                    : EAL 후처리(pci/mem_map/vtophys).
 * - spdk_env_dpdk_post_fini()        : EAL 후처리 역순 해제.
 * - spdk_env_fini()                  : 사용자가 명시적으로 호출하는 종료 API.
 * - dpdk_cleanup() [destructor(101)] : 프로그램 종료 시 자동 rte_eal_cleanup.
 * - x86_cpu_support_iommu() / get_intel_iommu_width() / get_cpu_vendor_name()
 *                                    : x86_64 리눅스에서 IOMMU VA 폭이 48bit
 *                                      이상인지 점검 — 부족하면 iova-mode=pa 강제.
 * - _sprintf_alloc(fmt, ...)         : 동적 vsnprintf — DPDK argv 토큰을 만들 때 사용.
 * - push_arg() / free_args()         : argv 동적 배열 조작 헬퍼.
 *
 * 핵심 전역 상수(SPDK_ENV_DPDK_DEFAULT_*) 는 사용자가 opts 를 명시하지 않을 때의
 * 기본값을 정의한다.
 */

#include "spdk/stdinc.h"        /* [한국어] 표준 C 헤더 어그리게이터(stdint, stdio, stdlib, errno 등). */

#include "env_internal.h"       /* [한국어] env_dpdk 내부 API 선언 — mem_*, pci_env_*, vtophys_* 등. 외부에 노출되지 않는다. */

#include "spdk/version.h"       /* [한국어] SPDK_VERSION_STRING 매크로 — 부팅 배너에 사용. */
#include "spdk/env_dpdk.h"      /* [한국어] 외부에서 DPDK 를 직접 초기화한 뒤 SPDK 만 추가 셋업하고 싶을 때 쓰는 spdk_env_dpdk_post_init/fini 선언. */
#include "spdk/log.h"           /* [한국어] SPDK_ERRLOG/PRINTF 매크로. */
#include "spdk/config.h"        /* [한국어] configure 단계에서 결정된 빌드 기능 매크로(NUMA, RDMA 등). */
#include "spdk/string.h"        /* [한국어] spdk_str_trim() 등 문자열 유틸. */

#include <openssl/ssl.h>        /* [한국어] OPENSSL_init_ssl, OPENSSL_INIT_* 플래그 — TLS 초기화에 필요. */
#include <openssl/err.h>        /* [한국어] ERR_print_errors_fp() — OpenSSL 에러 진단 출력. */

#include <rte_config.h>         /* [한국어] DPDK 빌드 시점 설정 매크로. 다른 rte 헤더가 의존. */
#include <rte_eal.h>            /* [한국어] rte_eal_init/cleanup — DPDK 의 환경 추상화 진입점. */
#include <rte_errno.h>          /* [한국어] rte_errno — DPDK 의 모듈 전역 errno. */
#include <rte_vfio.h>           /* [한국어] rte_vfio_noiommu_is_enabled() — vfio-pci 의 noiommu 모드 판별. */

/* [한국어] DPDK EAL 의 --file-prefix 등에 쓰이는 기본 프로그램 이름. */
#define SPDK_ENV_DPDK_DEFAULT_NAME		"spdk"
/* [한국어] 다중 프로세스용 공유 메모리 ID 의 기본값. -1 = 단일 프로세스 모드(공유 disable). */
#define SPDK_ENV_DPDK_DEFAULT_SHM_ID		-1
/* [한국어] 메모리 크기 기본값 -1 = "DPDK 가 자동 결정"(시스템 hugepage 가용량 기준). */
#define SPDK_ENV_DPDK_DEFAULT_MEM_SIZE		-1
/* [한국어] main lcore 기본값 -1 = "지정 없음"(DPDK 가 자동 선택). */
#define SPDK_ENV_DPDK_DEFAULT_MAIN_CORE		-1
/* [한국어] 메모리 채널 수 기본값 -1 = "지정 없음"(DPDK 자동 검출). */
#define SPDK_ENV_DPDK_DEFAULT_MEM_CHANNEL	-1
/* [한국어] 코어 마스크 기본값 = 코어 0 만 사용(0x1). 사용자 미지정시 단일 코어. */
#define SPDK_ENV_DPDK_DEFAULT_CORE_MASK		"0x1"
/* [한국어] hugepage 매핑의 기본 가상 주소(0x2000_0000_0000 = 32 TiB).
 * 이 값은 ASAN shadow region 과 겹치지 않도록 신중히 선택된다(아래 참조). */
#define SPDK_ENV_DPDK_DEFAULT_BASE_VIRTADDR	0x200000000000

/* [한국어] DPDK 의 PCI 허용/차단 리스트 인자 키 — 신구 DPDK 모두 호환되는 표기. */
#define DPDK_ALLOW_PARAM	"--allow"
#define DPDK_BLOCK_PARAM	"--block"
/* [한국어] DPDK 의 main lcore 지정 인자 키. */
#define DPDK_MAIN_CORE_PARAM	"--main-lcore"

/* [한국어] 합성된 EAL argv 배열 백업.
 * 설정자: build_eal_cmdline() — 첫 spdk_env_init 호출 시 채움.
 * 읽는 자: spdk_env_init()(rte_eal_init 직전 복사 후 인쇄), spdk_env_dpdk_post_fini()(해제).
 * 값 범위: NULL 또는 valid argv 배열(각 원소는 strdup 된 문자열).
 * 동기화: SPDK env 초기화는 단일 스레드(main) 에서 한 번만 일어난다고 가정 — 락 없음. */
static char **g_eal_cmdline;
static int g_eal_cmdline_argcount; /* [한국어] g_eal_cmdline 의 요소 수. fini 시 free_args 의 인자로 사용. */

/* [한국어] true 면 "외부에서 DPDK 가 초기화됨/아직 안 됨" 둘 중 하나 — 즉 SPDK 가 EAL 의 소유주가 아님.
 * spdk_env_init() 이 성공하면 false 로 바뀌어 destructor 가 rte_eal_cleanup 을 호출하게 된다.
 * spdk_env_dpdk_external_init() API 가 이 값을 그대로 노출한다.
 * 동기화: 본 파일의 단일 스레드 시점에서만 변경되므로 별도 락 불필요. */
static bool g_external_init = true;

/*
 * [한국어]
 * _sprintf_alloc - 동적 크기로 vsnprintf 하여 새 버퍼를 반환
 *
 * @format: printf 형식 문자열.
 * @...   : 포맷 인자.
 * @return: malloc 으로 할당된 NUL 종료 문자열, 실패 시 NULL.
 *
 * DPDK EAL argv 토큰(예: "-c 0x1", "--allow=0000:81:00.0") 을 만들기 위해
 * 길이를 미리 알기 어려운 문자열을 안전하게 합성한다. 32B 부터 시작해 1MB
 * 까지 지수적으로 버퍼를 키우며, 일부 libc 의 잘못된 vsnprintf 반환값
 * 동작도 보호한다. 호출자가 free() 책임을 진다.
 *
 * 호출 체인:
 *   build_eal_cmdline() → push_arg() → _sprintf_alloc() → vsnprintf(3)
 */
static char *
_sprintf_alloc(const char *format, ...)
{
	va_list args;        /* [한국어] 가변 인자 리스트 — vsnprintf 1차 시도용. */
	va_list args_copy;   /* [한국어] 재시도가 필요한 경우(va_list 는 1회만 사용 가능)를 위한 사본. */
	char *buf;           /* [한국어] 후보 버퍼. */
	size_t bufsize;      /* [한국어] 현재 시도 중인 버퍼 크기. */
	int rc;              /* [한국어] vsnprintf 의 반환값(필요 길이 또는 -1). */

	va_start(args, format);  /* [한국어] 가변 인자 시작 — 첫 시도 args 초기화. */

	/* Try with a small buffer first. */
	bufsize = 32;            /* [한국어] 일반적인 토큰("-c 0x1", "-n 4") 은 32B 안에 들어감. */

	/* Limit maximum buffer size to something reasonable so we don't loop forever. */
	while (bufsize <= 1024 * 1024) {            /* [한국어] 1MB 상한 — 잘못된 fmt/인자로 무한 확장 방지. */
		buf = malloc(bufsize);              /* [한국어] 후보 버퍼 할당. */
		if (buf == NULL) {                  /* [한국어] OOM 케이스 — 즉시 NULL 반환. */
			va_end(args);
			return NULL;
		}

		va_copy(args_copy, args);                   /* [한국어] vsnprintf 가 va_list 를 소비하므로 매 시도마다 사본 사용. */
		rc = vsnprintf(buf, bufsize, format, args_copy); /* [한국어] 실제 포맷팅 — bufsize 안에 안 들어가면 truncated. */
		va_end(args_copy);                          /* [한국어] 사본 정리. */

		/*
		 * If vsnprintf() returned a count within our current buffer size, we are done.
		 * The count does not include the \0 terminator, so rc == bufsize is not OK.
		 */
		if (rc >= 0 && (size_t)rc < bufsize) {     /* [한국어] 정확히 들어감(=NUL 포함 가능) → 확정. */
			va_end(args);                       /* [한국어] 가변 인자 정리. */
			return buf;                         /* [한국어] 호출자에게 동적 문자열 반환. 호출자는 free() 의무. */
		}

		/*
		 * vsnprintf() should return the required space, but some libc versions do not
		 * implement this correctly, so just double the buffer size and try again.
		 *
		 * We don't need the data in buf, so rather than realloc(), use free() and malloc()
		 * again to avoid a copy.
		 */
		free(buf);                                 /* [한국어] 충분치 않은 버퍼 폐기 — realloc 하지 않는 이유는 데이터 복사 비용을 피하기 위함. */
		bufsize *= 2;                              /* [한국어] 다음 시도 크기 2배 확장. */
	}

	va_end(args);                                      /* [한국어] 1MB 까지도 못 들어간 비정상 케이스 — 정리 후 NULL. */
	return NULL;
}

/*
 * [한국어]
 * spdk_env_opts_init - spdk_env_opts 구조체에 SPDK 기본값을 채움
 *
 * @opts: 사용자가 zero-init 또는 임의 초기값을 가진 옵션 구조체. opts_size
 *        를 반드시 sizeof(struct spdk_env_opts) 로 미리 설정해야 한다(ABI 안정성).
 *
 * 이 함수는 사용자 코드의 첫 단계에서 호출되어 모든 필드를 SPDK 의 합리적
 * 기본값(코어 마스크 0x1, mem_size auto 등)으로 세팅한다. 그 다음 사용자가
 * 필요한 필드만 덮어쓰고 spdk_env_init 을 호출하는 패턴.
 *
 * opts->opts_size 보존: 신/구 SPDK 헤더 ABI 호환을 위해 사용자가 알고 있는
 * 구조체 크기를 그대로 두며, SET_FIELD 매크로는 그 크기 안에 있는 필드만
 * 안전하게 초기화한다(이 너머에 있는 새 필드는 사용자 코드가 모르므로 건드리면 안 됨).
 *
 * 호출 체인: 사용자 코드 → spdk_env_opts_init()
 */
void
spdk_env_opts_init(struct spdk_env_opts *opts)
{
	size_t opts_size;       /* [한국어] 사용자가 명시한 구조체 크기 백업. */

	if (!opts) {            /* [한국어] NULL 안전 — 사용자가 잘못된 포인터를 넘긴 경우 즉시 종료. */
		return;
	}

	opts_size = opts->opts_size;     /* [한국어] memset 으로 0 클리어 후에도 opts_size 를 복원하기 위해 보관. */
	memset(opts, 0, sizeof(*opts));  /* [한국어] 모든 필드 0 으로 초기화 — 사용자가 잔여값을 의도치 않게 넘기는 사고 방지. */
	opts->opts_size = opts_size;     /* [한국어] ABI 식별자(opts_size) 를 복원 — 향후 SET_FIELD 가 이 값을 기준으로 안전 검사. */

	opts->name = SPDK_ENV_DPDK_DEFAULT_NAME;             /* [한국어] DPDK file-prefix/argv[0] 의 기본 이름 = "spdk". */
	opts->core_mask = SPDK_ENV_DPDK_DEFAULT_CORE_MASK;   /* [한국어] 단일 코어(0x1) 기본 — 사용자가 multi-core 원하면 덮어씀. */
	opts->shm_id = SPDK_ENV_DPDK_DEFAULT_SHM_ID;         /* [한국어] -1 = 단일 프로세스 모드(공유 메모리 비활성). */
	opts->mem_size = SPDK_ENV_DPDK_DEFAULT_MEM_SIZE;     /* [한국어] -1 = DPDK 가 hugepage 가용량 자동 결정. */
	opts->main_core = SPDK_ENV_DPDK_DEFAULT_MAIN_CORE;   /* [한국어] -1 = main lcore 미지정. */
	opts->mem_channel = SPDK_ENV_DPDK_DEFAULT_MEM_CHANNEL;/* [한국어] -1 = 메모리 채널 자동 검출. */
	opts->base_virtaddr = SPDK_ENV_DPDK_DEFAULT_BASE_VIRTADDR;/* [한국어] 32 TiB 기본 base — ASAN shadow 회피용. */

	/* [한국어] SET_FIELD 매크로: 새로 추가된 필드는 사용자 헤더(opts_size) 가
	 * 작을 수 있으므로, 해당 필드의 끝(offset+size) 가 opts_size 안에 있을 때만
	 * 초기화. 이 패턴 덕에 SPDK 라이브러리와 사용자 헤더의 마이너 버전이 달라도 안전. */
#define SET_FIELD(field, value) \
	if (offsetof(struct spdk_env_opts, field) + sizeof(opts->field) <= opts->opts_size) { \
		opts->field = value; \
	}

	SET_FIELD(enforce_numa, false);  /* [한국어] enforce_numa 기본값 false — DPDK 의 NUMA 강제 사용 안 함. */

#undef SET_FIELD
}

/*
 * [한국어]
 * free_args - 동적 argv 배열과 각 문자열을 모두 해제
 *
 * @args:     argv 배열 포인터(NULL 가능).
 * @argcount: 배열 요소 수(0 가능).
 *
 * push_arg() 가 만든 argv 를 정리할 때 사용. 부분적으로 만들어진 상태에서도
 * 안전하게 호출되어야 하므로 NULL/0 모두 허용.
 *
 * 호출 체인:
 *   build_eal_cmdline() (실패 시) / spdk_env_dpdk_post_fini() → free_args()
 */
static void
free_args(char **args, int argcount)
{
	int i;        /* [한국어] 배열 순회 인덱스. */

	if (args == NULL) {     /* [한국어] 한 번도 push_arg 가 성공한 적 없는 경우 — 아무 일도 하지 않음. */
		return;
	}

	for (i = 0; i < argcount; i++) {  /* [한국어] 각 토큰을 strdup/_sprintf_alloc 으로 만들었으므로 각각 free. */
		free(args[i]);
	}

	if (argcount) {                  /* [한국어] argcount==0 인데 args!=NULL 인 경우는 정상에서 발생하지 않으나 방어적. */
		free(args);              /* [한국어] 마지막으로 args 배열 자체를 해제. */
	}
}

/*
 * [한국어]
 * push_arg - argv 배열에 토큰 한 개를 추가(필요 시 realloc)
 *
 * @args:     기존 argv (NULL 가능 = 첫 호출).
 * @argcount: in/out — 현재 요소 수, 함수 종료 시 +1.
 * @arg:     추가할 토큰(이미 strdup/_sprintf_alloc 으로 동적 할당된 문자열).
 *           NULL 이면 OOM 으로 간주하고 누적된 모든 자원을 해제 후 NULL 반환.
 * @return:   확장된 argv 포인터(=새 args), 실패 시 NULL(이때 호출자는 누적
 *           자원이 이미 해제된 것으로 간주해야 한다 — 함수 안에서 정리 책임을 진다).
 *
 * 이 함수는 build_eal_cmdline 의 보일러플레이트를 줄이기 위해 작성되었다.
 * 실패 시 모든 자원을 정리하므로 build_eal_cmdline 은 단순히 NULL 검사 후
 * return -1 만 하면 된다.
 */
static char **
push_arg(char *args[], int *argcount, char *arg)
{
	char **tmp;        /* [한국어] realloc 결과 임시 — 실패해도 원본 args 보존. */

	if (arg == NULL) {                                                  /* [한국어] arg==NULL 은 _sprintf_alloc 등에서 OOM 발생을 의미. */
		SPDK_ERRLOG("%s: NULL arg supplied\n", __func__);           /* [한국어] 진단 로그. */
		free_args(args, *argcount);                                 /* [한국어] 지금까지 모은 args 를 모두 정리. */
		return NULL;
	}

	tmp = realloc(args, sizeof(char *) * (*argcount + 1));              /* [한국어] 한 칸 늘려 (n+1)개 포인터 공간 확보. realloc(NULL, ...) = malloc. */
	if (tmp == NULL) {                                                  /* [한국어] realloc 실패 — args 는 그대로 유효하지만 정리 책임. */
		free(arg);                                                  /* [한국어] 새 토큰은 어디에도 들어가지 않았으므로 직접 free. */
		free_args(args, *argcount);                                 /* [한국어] 기존 args 도 정리. */
		return NULL;
	}

	tmp[*argcount] = arg;       /* [한국어] 마지막 슬롯에 토큰 저장. */
	(*argcount)++;              /* [한국어] 카운터 증가(호출자도 갱신된 값 관찰). */

	return tmp;                 /* [한국어] 확장된 args 반환 — 호출자는 args = push_arg(...) 패턴으로 갱신. */
}

#if defined(__linux__) && defined(__x86_64__)

/* TODO: Can likely get this value from rlimits in the future */
/* [한국어] DPDK 의 IOVA-VA 모드는 hugepage 를 가상 주소 그대로 IOMMU 에 매핑한다.
 * SPDK 의 base_virtaddr=0x2000_0000_0000 + 통상 메모리 크기를 커버하려면
 * IOMMU 가 적어도 48bit VA 를 처리해야 한다. 부족하면 iova-mode=pa 로 강제. */
#define SPDK_IOMMU_VA_REQUIRED_WIDTH 48
/* [한국어] Intel VT-d Capability Register: MGAW(Maximum Guest Address Width) 비트 필드 위치/마스크.
 * VTD 스펙 5.1.1 — bits [21:16] 에 MGAW 가 위치, 0 부터 시작하므로 +1 해야 실제 비트 수가 됨. */
#define VTD_CAP_MGAW_SHIFT 16
#define VTD_CAP_MGAW_MASK (0x3F << VTD_CAP_MGAW_SHIFT)
/* [한국어] AMD IOMMU 의 VAsize 필드 — 본 파일은 x86_cpu_support_iommu 가
 * Intel 만 검사하므로 현재 미사용에 가깝지만, 매크로는 보존되어 있다. */
#define RD_AMD_CAP_VASIZE_SHIFT 15
#define RD_AMD_CAP_VASIZE_MASK (0x7F << RD_AMD_CAP_VASIZE_SHIFT)

/*
 * [한국어]
 * get_cpu_vendor_name - /proc/cpuinfo 에서 vendor_id 문자열을 읽음
 *
 * @vendor_name_buf: 결과를 받는 사용자 버퍼.
 * @buf_len:         버퍼 길이(잘림 시 경고만 발생).
 * @return:          0 성공, 음수 errno(에러).
 *
 * "GenuineIntel" / "AuthenticAMD" 같은 CPU 제조사 식별 문자열을 얻는다.
 * 이 결과는 IOMMU 점검 시 Intel 한정 검사 우회/적용 결정에 쓰인다(아래
 * x86_cpu_support_iommu 참조).
 *
 * 호출 체인:
 *   x86_cpu_support_iommu() → get_cpu_vendor_name() → fopen/fgets(3)
 */
static int
get_cpu_vendor_name(char *vendor_name_buf, size_t buf_len)
{
	const char *file_path = "/proc/cpuinfo";  /* [한국어] 리눅스 표준 procfs CPU 정보. */
	FILE *file;                               /* [한국어] cpuinfo 파일 스트림. */
	char line[256];                           /* [한국어] 한 줄 버퍼 — vendor_id 라인은 보통 80자 이내. */
	char *target_substr = NULL;               /* [한국어] strcasestr/strstr 결과를 보관. */
	char *vendor_name;                        /* [한국어] ':' 이후 trim 된 vendor 문자열을 가리킴. */
	int ret;                                  /* [한국어] 반환 코드. */

	file = fopen(file_path, "r");             /* [한국어] read-only 로 cpuinfo 오픈. */
	if (file == NULL) {                       /* [한국어] /proc 미마운트(컨테이너) 등 — 음수 errno 반환. */
		SPDK_ERRLOG("open file (read_only) %s failed, errno=%d.\n", file_path, errno);
		return -errno;
	}

	while (NULL != fgets(line, sizeof(line), file)) {  /* [한국어] EOF 까지 한 줄씩 읽으며 vendor_id 라인 탐색. */
		target_substr = strcasestr(line, "vendor_id"); /* [한국어] 대소문자 무시 검색. cpuinfo 의 "vendor_id" 라인 매칭. */
		if (target_substr != NULL) {                   /* [한국어] 발견 즉시 루프 종료. */
			break;
		}
	}

	if (target_substr == NULL) {              /* [한국어] vendor_id 라인이 없는 경우(예: ARM 호환 빌드, 비정상 procfs). */
		SPDK_ERRLOG("field %s not found in file %s.\n", "vendor_id", file_path);
		ret = -ESRCH;                     /* [한국어] -ESRCH: "No such process" — 가장 적절한 "찾을 수 없음" errno. */
		goto out;
	}

	target_substr = strstr(line, ":");        /* [한국어] "vendor_id : GenuineIntel" 형식의 ':' 위치 찾기. */
	if (target_substr == NULL) {              /* [한국어] ':' 가 없으면 cpuinfo 형식이 깨진 것. */
		SPDK_ERRLOG("separator char ':' not found in field line: %s.\n", line);
		ret = -EINVAL;
		goto out;
	}

	*target_substr = 0;                       /* eliminate the separator ':'. */ /* [한국어] ':' 자리를 NUL 로 만들어 line 을 두 토막으로 분리. */
	vendor_name = target_substr + 1;          /* point to the field value. */    /* [한국어] ':' 다음 글자(공백 포함) 부터가 값. */
	spdk_str_trim(vendor_name);               /* [한국어] 앞뒤 공백/탭/개행 제거 → "GenuineIntel" 같은 깨끗한 문자열. */

	if (strlen(vendor_name) == 0) {           /* [한국어] trim 후 빈 문자열이면 비정상 데이터. */
		SPDK_ERRLOG("cpu vendor name not found in field line: %s.\n", line);
		ret = -EINVAL;
		goto out;
	}

	ret = snprintf(vendor_name_buf, buf_len, "%s", vendor_name);  /* [한국어] 사용자 버퍼로 안전 복사. ret = 이상적 길이. */
	if (ret < 0) {                                                /* [한국어] glibc snprintf 가 음수를 반환하는 드문 케이스 — 인코딩 에러. */
		SPDK_ERRLOG("copy CPU vendor name to output buf failed, ret=%d, errno=%d.\n",
			    ret, errno);
		ret = -errno;
		goto out;
	} else if ((size_t)ret >= buf_len) {                          /* [한국어] 잘림 발생(버퍼 작음) — 경고만 하고 계속. */
		SPDK_WARNLOG("CPU vendor_name truncated from %s to %s\n",
			     vendor_name, vendor_name_buf);
	}
	ret = 0;                                                       /* [한국어] 정상 종료 표식. */

out:
	fclose(file);                                                  /* [한국어] cpuinfo 핸들 정리(에러 경로/정상 경로 공통). */

	return ret;
}

/*
 * [한국어]
 * get_intel_iommu_width - sysfs 의 Intel VT-d cap 레지스터에서 MGAW 추출
 *
 * @return: MGAW(VA bit width). IOMMU 미존재/접근 불가 시 0.
 *
 * /sys/devices/virtual/iommu/dmar*[/]intel-iommu/cap 파일은 VT-d 의 64bit
 * Capability Register 값을 hex 텍스트로 노출한다. 그 안에서 비트 [21:16] 의
 * MGAW 를 읽어 +1 한 값이 IOMMU 가 처리 가능한 VA 폭이다.
 *
 * 시스템에 여러 dmar 단위가 존재할 수 있으며, SPDK 는 그 중 가장 작은 폭을
 * 선택한다(가장 보수적인 한도). 단 현재 코드의 루프 본문은 항상 [0] 인덱스
 * 의 path 를 읽고 있어 모든 반복이 동일 파일을 보는데, 결과적으로 첫 dmar
 * 의 폭이 항상 사용된다.
 *
 * 호출 체인:
 *   x86_cpu_support_iommu() → get_intel_iommu_width() → glob/fopen/fscanf
 */
static int
get_intel_iommu_width(void)
{
	int width = 0;                  /* [한국어] 누적 최소 MGAW. 한 번도 못 읽으면 0. */
	glob_t glob_results = {};       /* [한국어] glob(3) 결과 — 매칭 path 목록을 동적 배열로 보유. */

	/* Break * and / into separate strings to appease check_format.sh comment style check. */
	/* [한국어] 와일드카드 패턴 "/sys/.../dmar*[/]intel-iommu/cap" 매칭.
	 * 주석 검사 스크립트가 "dmar* /" 같은 토큰을 잘못 파싱하므로 문자열 분할 트릭. */
	glob("/sys/devices/virtual/iommu/dmar*" "/intel-iommu/cap", 0, NULL, &glob_results);

	for (size_t i = 0; i < glob_results.gl_pathc; i++) {        /* [한국어] 매칭된 모든 dmar 파일 순회. */
		const char *filename = glob_results.gl_pathv[0];    /* [한국어] (잠재적 버그) 항상 [0] 사용 — 실제로는 첫 파일만 검사. */
		FILE *file = fopen(filename, "r");                  /* [한국어] cap 파일 read-only 오픈. */
		uint64_t cap_reg = 0;                               /* [한국어] hex 64bit 값 누적. */

		if (file == NULL) {                                 /* [한국어] 권한/존재 안 함 — 다음 dmar 로 건너뜀. */
			continue;
		}

		if (fscanf(file, "%" PRIx64, &cap_reg) == 1) {                       /* [한국어] hex 표현된 cap 값을 64bit 로 파싱. */
			int mgaw = ((cap_reg & VTD_CAP_MGAW_MASK) >> VTD_CAP_MGAW_SHIFT) + 1; /* [한국어] MGAW 필드 추출 후 +1 = 실제 VA 폭. */

			if (width == 0 || (mgaw > 0 && mgaw < width)) {              /* [한국어] 누적 최소값 갱신(가장 보수적인 width 선택). */
				width = mgaw;
			}
		}

		fclose(file);                                       /* [한국어] 파일 핸들 정리. */
	}

	globfree(&glob_results);                                    /* [한국어] glob 가 alloc 한 path 배열 해제. */
	return width;                                               /* [한국어] 호출자(x86_cpu_support_iommu)에 width 전달. */
}

/*
 * [한국어]
 * x86_cpu_support_iommu - 현재 x86_64 시스템이 SPDK 가 요구하는 IOMMU
 *                         VA 폭(48bit)을 갖추었는지 판정
 *
 * @return: true = OK(또는 비-Intel 이라 검사 생략), false = VA 폭 부족.
 *
 * Intel CPU 만 sysfs 의 VT-d 정보로 정밀 검사하고, 비-Intel(AMD 등) 은
 * "지원한다" 가정으로 통과시킨다. 결과가 false 이면 build_eal_cmdline() 이
 * --iova-mode=pa 를 강제 추가해 hugepage 의 물리 주소를 그대로 IOMMU 에
 * 매핑한다(VA 모드 대신).
 *
 * 호출 체인:
 *   build_eal_cmdline() → x86_cpu_support_iommu()
 *   → get_cpu_vendor_name() / get_intel_iommu_width()
 */
static bool
x86_cpu_support_iommu(void)
{
	int rc;                          /* [한국어] get_cpu_vendor_name 반환값. */
	char cpu_vendor_name[64];        /* [한국어] vendor 문자열 — 12바이트면 충분하지만 여유 있게. */

	rc = get_cpu_vendor_name(cpu_vendor_name, sizeof(cpu_vendor_name));  /* [한국어] /proc/cpuinfo 에서 vendor 추출. */
	if (rc != 0) {                                                       /* [한국어] 추출 실패 → 진단 로그만 남기고 Intel 검사로 폴백. */
		SPDK_ERRLOG("get_cpu_vendor_name failed, return value=%d.\n", rc);
	} else if (strcasestr(cpu_vendor_name, "GenuineIntel") == NULL) {
		/* An X86_64 CPU not from Intel, assume IOMMU supported */
		return true;                                                 /* [한국어] AMD 등 — 보수적으로 "지원" 가정. */
	}

	return get_intel_iommu_width() >= SPDK_IOMMU_VA_REQUIRED_WIDTH;      /* [한국어] Intel: MGAW>=48 이면 true. */
}

#endif

/*
 * [한국어]
 * build_eal_cmdline - 사용자 opts 를 DPDK EAL 의 argv 형식으로 변환
 *
 * @opts: 사용자가 채운 옵션 구조체.
 * @return: 0 이상 = argv 토큰 개수, -1 = 실패(이미 자원은 정리됨).
 *
 * 이 함수가 SPDK env 의 핵심 정책 결정자다. opts 를 보고 다음을 결정한다:
 *   - 단일/다중 프로세스(--no-shconf, --file-prefix)
 *   - 코어 매핑(-c mask, -l list, --lcores)
 *   - 메모리 채널(-n), 메모리 크기(-m)
 *   - hugepage 옵션(--single-file-segments, --huge-unlink, --huge-dir)
 *   - PCI allow/block(--allow / --block)
 *   - 로그 레벨(--log-level=lib.eal:6 등)
 *   - IOVA 모드(--iova-mode=va|pa) — Linux x86 의 IOMMU 폭/no-huge 까지 점검
 *   - --base-virtaddr (ASAN 회피)
 *   - --match-allocations (RDMA mempool 안전성)
 *   - VFIO VF token (--vfio-vf-token=...)
 *   - 사용자 임의 추가 인자(env_context) 토큰화하여 추가
 *
 * 결과는 g_eal_cmdline / g_eal_cmdline_argcount 에 저장된다.
 *
 * 호출 체인:
 *   spdk_env_init() → build_eal_cmdline()
 *   → push_arg() / _sprintf_alloc() / strdup() / mem_disable_*()
 */
static int
build_eal_cmdline(const struct spdk_env_opts *opts)
{
	int argcount = 0;     /* [한국어] 누적 argv 토큰 수. push_arg 에 in/out 으로 전달. */
	char **args;          /* [한국어] argv 배열 — push_arg 가 realloc 으로 키움. */
	bool no_huge;         /* [한국어] hugepage 미사용 모드 여부(opts 또는 env_context 둘 중 하나만 활성화해도 true). */

	args = NULL;          /* [한국어] 첫 push_arg 호출 시 NULL → realloc(NULL, ...)=malloc 동작. */
	/* [한국어] no_huge 결정: 명시적 opts->no_huge 또는 사용자 env_context 에 "--no-huge" 가 들어간 경우. */
	no_huge = opts->no_huge || (opts->env_context && strstr(opts->env_context, "--no-huge") != NULL);

	/* set the program name */
	args = push_arg(args, &argcount, _sprintf_alloc("%s", opts->name));  /* [한국어] argv[0] = 프로그램 이름(=DPDK file-prefix 의 베이스). */
	if (args == NULL) {
		return -1;                                                    /* [한국어] OOM — push_arg 가 이미 정리. */
	}

	/* disable shared configuration files when in single process mode. This allows for cleaner shutdown */
	if (opts->shm_id < 0) {                                                /* [한국어] 단일 프로세스 모드. */
		args = push_arg(args, &argcount, _sprintf_alloc("%s", "--no-shconf")); /* [한국어] 프로세스 간 config 공유 비활성화 → cleanup 시 부수적 파일 안 만듦. */
		if (args == NULL) {
			return -1;
		}
	}

	/* Either lcore_map or core_mask must be set. If both, or none specified, fail */
	/* [한국어] (a==b)==NULL 패턴: 둘 다 설정/둘 다 미설정이면 true → 에러. 정확히 한 쪽만 있어야 진행. */
	if ((opts->core_mask == NULL) == (opts->lcore_map == NULL)) {
		if (opts->core_mask && opts->lcore_map) {
			fprintf(stderr,
				"Both, lcore map and core mask are provided, while only one can be set\n");
		} else {
			fprintf(stderr, "Core mask or lcore map must be specified\n");
		}
		free_args(args, argcount);                            /* [한국어] 지금까지 누적된 args 정리. */
		return -1;
	}

	if (opts->lcore_map) {
		/* If lcore list is set, generate --lcores parameter */
		args = push_arg(args, &argcount, _sprintf_alloc("--lcores=%s", opts->lcore_map)); /* [한국어] DPDK --lcores 표기 — "0@4,1@5" 같은 lcore↔CPU 매핑 문법. */
	} else if (opts->core_mask[0] == '-') {
		/*
		 * Set the coremask:
		 *
		 * - if it starts with '-', we presume it's literal EAL arguments such
		 *   as --lcores.
		 *
		 * - if it starts with '[', we presume it's a core list to use with the
		 *   -l option.
		 *
		 * - otherwise, it's a CPU mask of the form "0xff.." as expected by the
		 *   -c option.
		 */
		args = push_arg(args, &argcount, _sprintf_alloc("%s", opts->core_mask));  /* [한국어] 사용자가 직접 EAL 옵션을 넣은 케이스 — 그대로 패스스루. */
	} else if (opts->core_mask[0] == '[') {
		char *l_arg = _sprintf_alloc("-l %s", opts->core_mask + 1);  /* [한국어] '[' 다음부터 코어 리스트로 보고 "-l" 인자 합성. */

		if (l_arg != NULL) {
			int len = strlen(l_arg);

			if (l_arg[len - 1] == ']') {                  /* [한국어] 닫는 ']' 가 있으면 NUL 로 잘라낸다 — DPDK 가 ']' 까지 읽으면 안 됨. */
				l_arg[len - 1] = '\0';
			}
		}
		args = push_arg(args, &argcount, l_arg);                      /* [한국어] "-l 0,1,2" 형태로 push. */
	} else {
		args = push_arg(args, &argcount, _sprintf_alloc("-c %s", opts->core_mask));  /* [한국어] 16진 마스크 그대로 -c 에 전달 — DPDK 의 가장 일반적인 옵션. */
	}

	if (args == NULL) {
		return -1;
	}

	/* set the memory channel number */
	if (opts->mem_channel > 0) {                                                  /* [한국어] 명시값(>0)일 때만 -n 추가. -1 은 자동검출. */
		args = push_arg(args, &argcount, _sprintf_alloc("-n %d", opts->mem_channel));
		if (args == NULL) {
			return -1;
		}
	}

	/* set the memory size */
	if (opts->mem_size >= 0) {                                                    /* [한국어] 0 도 유효한 명시값(=hugepage 0MB). -m 으로 mem 한도 설정. */
		args = push_arg(args, &argcount, _sprintf_alloc("-m %d", opts->mem_size));
		if (args == NULL) {
			return -1;
		}
	}

	/* set no huge pages */
	if (no_huge) {
		mem_disable_huge_pages();    /* [한국어] env_dpdk 내부 상태(g_huge_pages 등) 를 변경 — vtophys 가 hugepage 의존을 끔. */
	}

	if (opts->enforce_numa) {
		mem_enforce_numa();          /* [한국어] DPDK 메모리 할당이 호출 lcore 의 NUMA 노드만 사용하도록 강제. */
	}

	/* set the main core */
	if (opts->main_core > 0) {                                                    /* [한국어] 0 은 default 코어 0 일 수 있어 >0 만 명시 옵션으로 본다. */
		args = push_arg(args, &argcount, _sprintf_alloc("%s=%d",
				DPDK_MAIN_CORE_PARAM, opts->main_core));
		if (args == NULL) {
			return -1;
		}
	}

	/* set no pci  if enabled */
	if (opts->no_pci) {                                                           /* [한국어] PCI 디바이스 사용 안 함(예: pure malloc bdev 만 쓸 때). */
		args = push_arg(args, &argcount, _sprintf_alloc("--no-pci"));
		if (args == NULL) {
			return -1;
		}
		mem_disable_vtophys();    /* [한국어] vtophys(VA→PA 매핑)도 비활성 — 어차피 DMA 안 함. */
	}

	if (no_huge) {
		if (opts->hugepage_single_segments || opts->unlink_hugepage || opts->hugedir) {  /* [한국어] no_huge 와 hugepage 전용 옵션의 충돌 검사. */
			fprintf(stderr, "--no-huge invalid with other hugepage options\n");
			free_args(args, argcount);
			return -1;
		}

		if (opts->mem_size < 0) {                                                       /* [한국어] no_huge 면 DPDK 가 mem 자동 결정 못 함 → -m 필수. */
			fprintf(stderr,
				"Disabling hugepages requires specifying how much memory "
				"will be allocated using -s parameter\n");
			free_args(args, argcount);
			return -1;
		}

		/* iova-mode=pa is incompatible with no_huge */
		if (opts->iova_mode &&
		    (strcmp(opts->iova_mode, "pa") == 0)) {                                     /* [한국어] no_huge 는 PA 매핑 불가(일반 페이지의 PFN 안정성 보장 X). */
			fprintf(stderr, "iova-mode=pa is incompatible with specified "
				"no-huge parameter\n");
			free_args(args, argcount);
			return -1;
		}

		args = push_arg(args, &argcount, _sprintf_alloc("--no-huge"));         /* [한국어] DPDK 측 no-huge 활성화. */
		args = push_arg(args, &argcount, _sprintf_alloc("--legacy-mem"));      /* [한국어] no_huge 는 dynamic mem 미지원이라 legacy 강제. */
		args = push_arg(args, &argcount, _sprintf_alloc("--iova-mode=va"));    /* [한국어] no_huge 환경에선 VA 모드 강제. */
	} else {
		/* create just one hugetlbfs file */
		if (opts->hugepage_single_segments) {                                          /* [한국어] 모든 hugepage 를 하나의 파일에 매핑(공유 메모리 단순화). */
			args = push_arg(args, &argcount, _sprintf_alloc("--single-file-segments"));
			if (args == NULL) {
				return -1;
			}
		}

		/* unlink hugepages after initialization */
		/* Note: Automatically unlink hugepage when shm_id < 0, since it means we're not using
		 * multi-process so we don't need the hugepage links anymore.  But we need to make sure
		 * we don't specify --huge-unlink implicitly if --single-file-segments was specified since
		 * DPDK doesn't support that.
		 */
		if (opts->unlink_hugepage ||
		    (opts->shm_id < 0 && !opts->hugepage_single_segments)) {                   /* [한국어] 단일 프로세스이면 hugepage 파일 링크가 불필요. */
			args = push_arg(args, &argcount, _sprintf_alloc("--huge-unlink"));
			if (args == NULL) {
				return -1;
			}
		}

		/* use a specific hugetlbfs mount */
		if (opts->hugedir) {                                                            /* [한국어] 사용자가 별도 hugetlbfs 마운트 지정한 경우. */
			args = push_arg(args, &argcount, _sprintf_alloc("--huge-dir=%s", opts->hugedir));
			if (args == NULL) {
				return -1;
			}
		}
	}

	if (opts->num_pci_addr) {                                                                /* [한국어] PCI allow/block 리스트가 있으면 모두 인자로 합성. */
		size_t i;
		char bdf[32];                                                                    /* [한국어] BDF 문자열 출력 버퍼("0000:81:00.0" 길이 12 + 여유). */
		struct spdk_pci_addr *pci_addr =
				opts->pci_blocked ? opts->pci_blocked : opts->pci_allowed;       /* [한국어] block 우선, 둘 다는 SPDK 정책상 의미 없음. */

		for (i = 0; i < opts->num_pci_addr; i++) {
			spdk_pci_addr_fmt(bdf, 32, &pci_addr[i]);                                /* [한국어] domain:bus:dev.func 형식으로 포맷팅. */
			args = push_arg(args, &argcount, _sprintf_alloc("%s=%s",
					(opts->pci_blocked ? DPDK_BLOCK_PARAM : DPDK_ALLOW_PARAM),
					bdf));                                                   /* [한국어] "--block=BDF" 또는 "--allow=BDF" 토큰 추가. */
			if (args == NULL) {
				return -1;
			}
		}
	}

	/* Disable DPDK telemetry information by default, can be modified with env_context.
	 * Prevents creation of dpdk_telemetry socket and additional pthread for it.
	 */
	args = push_arg(args, &argcount, _sprintf_alloc("--no-telemetry"));   /* [한국어] dpdk_telemetry 소켓/스레드 생성 차단 — 폴드 모델에 노이즈/오버헤드 회피. */
	if (args == NULL) {
		return -1;
	}

	/* Lower default EAL loglevel to RTE_LOG_NOTICE - normal, but significant messages.
	 * This can be overridden by specifying the same option in opts->env_context
	 */
	args = push_arg(args, &argcount, strdup("--log-level=lib.eal:6"));    /* [한국어] EAL 로그 NOTICE 레벨(6) — 의미 있는 메시지만 출력. */
	if (args == NULL) {
		return -1;
	}

	/* Lower default CRYPTO loglevel to RTE_LOG_WARNING to avoid a ton of init msgs.
	 * This can be overridden by specifying the same option in opts->env_context
	 */
	args = push_arg(args, &argcount, strdup("--log-level=lib.cryptodev:5"));  /* [한국어] cryptodev 는 init 메시지가 매우 많아 WARNING(5) 으로 낮춤. */
	if (args == NULL) {
		return -1;
	}

	/* Lower default POWER loglevel to RTE_LOG_WARNING to avoid a ton of init msgs.
	 * This can be overridden by specifying the same option in opts->env_context
	 */
	args = push_arg(args, &argcount, strdup("--log-level=lib.power:5"));   /* [한국어] power(governor) 모듈 로그도 WARNING 으로 억제. */
	if (args == NULL) {
		return -1;
	}

	/* `user1` log type is used by rte_vhost, which prints an INFO log for each received
	 * vhost user message. We don't want that. The same log type is also used by a couple
	 * of other DPDK libs, but none of which we make use right now. If necessary, this can
	 * be overridden via opts->env_context.
	 */
	args = push_arg(args, &argcount, strdup("--log-level=user1:6"));       /* [한국어] rte_vhost INFO 스팸 억제 — NOTICE(6). */
	if (args == NULL) {
		return -1;
	}

#ifdef __linux__

	if (opts->iova_mode) {
		/* iova-mode=pa is incompatible with no_huge */
		args = push_arg(args, &argcount, _sprintf_alloc("--iova-mode=%s", opts->iova_mode)); /* [한국어] 사용자가 명시한 iova 모드를 우선 적용. */
		if (args == NULL) {
			return -1;
		}
	} else {
		/* When using vfio with enable_unsafe_noiommu_mode=Y, we need iova-mode=pa,
		 * but DPDK guesses it should be iova-mode=va. Add a check and force
		 * iova-mode=pa here. */
		if (!no_huge && rte_vfio_noiommu_is_enabled()) {                /* [한국어] vfio_iommu_type1 의 noiommu 모드면 IOMMU 자체가 없으므로 PA 가 안전. */
			args = push_arg(args, &argcount, _sprintf_alloc("--iova-mode=pa"));
			if (args == NULL) {
				return -1;
			}
		}

#if defined(__x86_64__)
		/* DPDK by default guesses that it should be using iova-mode=va so that it can
		 * support running as an unprivileged user. However, some systems (especially
		 * virtual machines) don't have an IOMMU capable of handling the full virtual
		 * address space and DPDK doesn't currently catch that. Add a check in SPDK
		 * and force iova-mode=pa here. */
		if (!no_huge && !x86_cpu_support_iommu()) {                     /* [한국어] x86 IOMMU VA 폭이 48bit 미만(주로 VM) → PA 강제. */
			args = push_arg(args, &argcount, _sprintf_alloc("--iova-mode=pa"));
			if (args == NULL) {
				return -1;
			}
		}
#elif defined(__PPC64__)
		/* On Linux + PowerPC, DPDK doesn't support VA mode at all. Unfortunately, it doesn't correctly
		 * auto-detect at the moment, so we'll just force it here. */
		args = push_arg(args, &argcount, _sprintf_alloc("--iova-mode=pa"));  /* [한국어] PPC64 는 VA 모드 미지원 — 항상 PA. */
		if (args == NULL) {
			return -1;
		}
#endif
	}


	/* Set the base virtual address - it must be an address that is not in the
	 * ASAN shadow region, otherwise ASAN-enabled builds will ignore the
	 * mmap hint.
	 *
	 * Ref: https://github.com/google/sanitizers/wiki/AddressSanitizerAlgorithm
	 */
	/* [한국어] 0x2000_0000_0000 는 x86_64 ASAN shadow(보통 0x7fff_8000_0000 근처) 와 겹치지 않는다.
	 * 이 hint 를 통해 DPDK 가 hugepage 를 안전한 영역에 매핑하도록 유도. */
	args = push_arg(args, &argcount, _sprintf_alloc("--base-virtaddr=0x%" PRIx64, opts->base_virtaddr));
	if (args == NULL) {
		return -1;
	}

	/* --match-allocation prevents DPDK from merging or splitting system memory allocations under the hood.
	 * This is critical for RDMA when attempting to use an rte_mempool based buffer pool. If DPDK merges two
	 * physically or IOVA contiguous memory regions, then when we go to allocate a buffer pool, it can split
	 * the memory for a buffer over two allocations meaning the buffer will be split over a memory region.
	 */

	/* --no-huge is incompatible with --match-allocations
	 * Ref:  https://doc.dpdk.org/guides/prog_guide/env_abstraction_layer.html#hugepage-allocation-matching
	 */
	if (!no_huge &&
	    (!opts->env_context || strstr(opts->env_context, "--legacy-mem") == NULL)) {       /* [한국어] legacy-mem 모드면 dynamic 매칭이 의미 없으므로 추가하지 않는다. */
		args = push_arg(args, &argcount, _sprintf_alloc("%s", "--match-allocations"));   /* [한국어] DPDK 가 hugepage 를 합치거나 쪼개지 않도록 — RDMA mempool 의 일관성 확보. */
		if (args == NULL) {
			return -1;
		}
	}

	if (opts->shm_id < 0) {                                                                  /* [한국어] 단일 프로세스 모드 — pid 기반 prefix 로 충돌 방지. */
		args = push_arg(args, &argcount, _sprintf_alloc("--file-prefix=spdk_pid%d",
				getpid()));
		if (args == NULL) {
			return -1;
		}
	} else {                                                                                /* [한국어] 다중 프로세스 모드 — shm_id 가 prefix 의 식별자. */
		args = push_arg(args, &argcount, _sprintf_alloc("--file-prefix=spdk%d",
				opts->shm_id));
		if (args == NULL) {
			return -1;
		}

		/* set the process type, if not provided by the user */
		if (!opts->env_context || strstr(opts->env_context, "--proc-type") == NULL) {   /* [한국어] 사용자가 명시하지 않았다면 auto 로 — 첫 프로세스=primary, 이후=secondary. */
			args = push_arg(args, &argcount, _sprintf_alloc("--proc-type=auto"));
			if (args == NULL) {
				return -1;
			}
		}
	}

	/* --vfio-vf-token used for VF initialized by vfio_pci driver. */
	if (opts->vf_token) {                                                          /* [한국어] SR-IOV VF 를 vfio_pci 로 초기화할 때 PF 와 공유하는 UUID 토큰. */
		args = push_arg(args, &argcount, _sprintf_alloc("--vfio-vf-token=%s",
				opts->vf_token));
		if (args == NULL) {
			return -1;
		}
	}
#endif

	if (opts->env_context) {                                                       /* [한국어] 사용자가 직접 추가하고 싶은 EAL 옵션 문자열(공백/탭 구분). */
		char *sp = NULL;                                                       /* [한국어] strtok_r 의 saveptr — 재진입 안전 토크나이저. */
		char *ptr = strdup(opts->env_context);                                 /* [한국어] strtok_r 가 원본을 변형하므로 사본 사용. */
		char *tok = strtok_r(ptr, " \t", &sp);                                 /* [한국어] 첫 토큰 추출. */

		/* DPDK expects each argument as a separate string in the argv
		 * array, so we need to tokenize here in case the caller
		 * passed multiple arguments in the env_context string.
		 */
		while (tok != NULL) {
			args = push_arg(args, &argcount, strdup(tok));                 /* [한국어] 각 토큰을 별도 argv 슬롯으로. */
			tok = strtok_r(NULL, " \t", &sp);                              /* [한국어] 다음 토큰. */
		}

		free(ptr);                                                              /* [한국어] strdup 사본 해제. 토큰들은 strdup 별도 사본을 args 에 보유 중. */
	}

	g_eal_cmdline = args;                  /* [한국어] 합성된 argv 를 전역에 저장. spdk_env_init 이 곧 사용. */
	g_eal_cmdline_argcount = argcount;     /* [한국어] 토큰 수 저장 — fini 시 free_args 의 인자. */
	return argcount;                       /* [한국어] 호출자에게 토큰 수 반환(>=0 = 성공). */
}

/*
 * [한국어]
 * spdk_env_dpdk_post_init - rte_eal_init 이후의 SPDK 측 초기화
 *
 * @legacy_mem: DPDK 가 legacy-mem 모드인지 여부(일부 매핑 정책에 영향).
 * @return: 0 성공, 음수 errno 실패.
 *
 * 외부에서 DPDK EAL 을 직접 초기화한 사용자도 SPDK 의 메모리/PCI 후처리를
 * 수행하기 위해 이 함수를 별도로 호출할 수 있다(공개 API). spdk_env_init
 * 의 마지막 단계에서도 사용된다.
 *
 * 단계:
 *   1) pci_env_init  : SPDK 의 PCI 디바이스 드라이버 등록(driver, register, scan).
 *   2) mem_map_init  : SPDK 의 메모리 등록 콜백 시스템 부팅(huge mem 변경 추적).
 *   3) vtophys_init  : VA→PA 변환 테이블 초기화(PRP/SGL DMA 주소 계산에 필수).
 *
 * 호출 체인:
 *   spdk_env_init() / 외부 사용자 → spdk_env_dpdk_post_init()
 *   → pci_env_init() → mem_map_init() → vtophys_init()
 */
int
spdk_env_dpdk_post_init(bool legacy_mem)
{
	int rc;        /* [한국어] 각 단계 반환값. */

	rc = pci_env_init();
	if (rc < 0) {
		SPDK_ERRLOG("pci_env_init() failed\n");
		return rc;
	}

	rc = mem_map_init(legacy_mem);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to allocate mem_map\n");
		return rc;
	}

	rc = vtophys_init();
	if (rc < 0) {
		SPDK_ERRLOG("Failed to initialize vtophys\n");
		return rc;
	}

	return 0;     /* [한국어] 모든 후처리 정상 완료. */
}

/*
 * [한국어]
 * spdk_env_dpdk_post_fini - post_init 의 역순 정리 + 합성 argv 해제
 *
 * 사용자가 spdk_env_fini() 또는 외부 EAL 종료 직전에 호출. 각 finalizer 는
 * 부분적으로 초기화된 상태에서도 안전하게 동작하도록 작성되어 있어 부분
 * 초기화 후 실패한 경우에도 사용 가능.
 *
 * 호출 체인:
 *   spdk_env_fini() / 외부 사용자 → spdk_env_dpdk_post_fini()
 *   → vtophys_fini() → mem_map_fini() → pci_env_fini() → free_args()
 */
void
spdk_env_dpdk_post_fini(void)
{
	vtophys_fini();        /* [한국어] vtophys 테이블/등록 해제. */
	mem_map_fini();        /* [한국어] 메모리 변경 추적 콜백 등록 해제. */
	pci_env_fini();        /* [한국어] PCI 드라이버/디바이스 정리. */

	free_args(g_eal_cmdline, g_eal_cmdline_argcount);  /* [한국어] build_eal_cmdline 이 만든 argv 해제. */
	g_eal_cmdline = NULL;                              /* [한국어] dangling 방지. */
	g_eal_cmdline_argcount = 0;                        /* [한국어] 카운터 리셋. */
}

/*
 * [한국어]
 * env_copy_opts - 사용자 opts 를 ABI-안전한 방식으로 내부 opts 로 복사
 *
 * @opts:           SPDK 내부 옵션 구조체(=현재 헤더 기준 최신 크기).
 * @opts_user:      사용자가 제공한 opts(=사용자 컴파일 시점 헤더 크기).
 * @user_opts_size: 사용자 헤더의 sizeof(struct spdk_env_opts).
 *
 * sizeof(opts) 와 user_opts_size 가 다를 수 있으므로(SPDK 라이브러리/사용자
 * 헤더 버전 차이) opts_size 멤버까지는 무조건 복사하고, 그 너머의 필드는
 * SET_FIELD 매크로로 안전하게 복사한다.
 *
 * 호출 체인:
 *   spdk_env_init() → env_copy_opts() → spdk_env_opts_init()/memcpy
 */
static void
env_copy_opts(struct spdk_env_opts *opts, const struct spdk_env_opts *opts_user,
	      size_t user_opts_size)
{
	opts->opts_size = sizeof(*opts);                                                 /* [한국어] 내부에선 항상 최신 sizeof — 후속 SET_FIELD 가 이 값을 사용. */
	spdk_env_opts_init(opts);                                                        /* [한국어] 우선 기본값으로 채움. */
	memcpy(opts, opts_user, offsetof(struct spdk_env_opts, opts_size));              /* [한국어] opts_size 직전까지의 호환 영역만 그대로 덮어씀(공통 필드). */

	/* [한국어] opts_size 이후의 새 필드는 user_opts_size 가 충분할 때만 복사. */
#define SET_FIELD(field) \
	if (offsetof(struct spdk_env_opts, field) + sizeof(opts->field) <= user_opts_size) { \
		opts->field = opts_user->field; \
	}

	SET_FIELD(enforce_numa);   /* [한국어] enforce_numa 는 opts_size 이후에 추가된 필드. */

#undef SET_FIELD
}

/*
 * [한국어]
 * spdk_env_init - SPDK 환경 초기화의 공식 엔트리포인트
 *
 * @opts_user: 사용자가 spdk_env_opts_init() 후 추가 설정한 옵션. 첫 호출 시
 *             NULL 불가, 재초기화(reinit) 호출 시에는 NULL 이어야 한다.
 * @return:    0 성공, 음수 errno 실패.
 *
 * 동작 단계:
 *   - 이미 SPDK env 가 초기화된 경우(g_external_init==false) → pci_env_reinit
 *     만 수행하고 반환(예: 동적 디바이스 재스캔).
 *   - opts ABI 검증(opts_size 가 최소 크기 이상인지) → env_copy_opts.
 *   - OpenSSL 초기화(NVMe-oF TLS/auth 의 의존성).
 *   - build_eal_cmdline → 합성 argv 출력 → rte_eal_init.
 *   - legacy_mem 결정 → spdk_env_dpdk_post_init(메모리/PCI/vtophys).
 *   - 성공 시 g_external_init=false (이제 SPDK 가 EAL 의 소유자).
 *
 * 실행 컨텍스트: 일반적으로 process main 스레드, 1회만 호출.
 *
 * 호출 체인:
 *   사용자 main / spdk_app_start() → spdk_env_init()
 *   → env_copy_opts → OPENSSL_init_ssl → build_eal_cmdline → rte_eal_init
 *   → spdk_env_dpdk_post_init
 */
int
spdk_env_init(const struct spdk_env_opts *opts_user)
{
	struct spdk_env_opts opts_local = {};                  /* [한국어] 내부 opts 사본 — ABI 호환 보정 후 사용. */
	struct spdk_env_opts *opts = &opts_local;              /* [한국어] 함수 내부에서 opts 로 줄여 부르기 위한 포인터. */
	char **dpdk_args = NULL;                               /* [한국어] rte_eal_init 에 전달할 argv 사본(소유권 임시). */
	char *args_print = NULL, *args_tmp = NULL;             /* [한국어] argv 를 한 줄 문자열로 prettify 하기 위한 임시들. */
	OPENSSL_INIT_SETTINGS *settings;                       /* [한국어] OpenSSL 초기화 설정 객체. */
	int i, rc;                                             /* [한국어] 반복/반환값 변수. */
	int orig_optind;                                       /* [한국어] DPDK 가 변경할 수 있는 getopt 의 optind 백업. */
	bool legacy_mem;                                       /* [한국어] DPDK 가 legacy-mem 모드로 부팅했는지(post_init 인자). */
	size_t min_opts_size, user_opts_size;                  /* [한국어] ABI 검증을 위한 크기 변수들. */

	/* If SPDK env has been initialized before, then only pci env requires
	 * reinitialization.
	 */
	if (g_external_init == false) {                        /* [한국어] 이미 한 번 초기화됨 → reinit 경로. */
		if (opts_user != NULL) {
			fprintf(stderr, "Invalid arguments to reinitialize SPDK env\n");
			return -EINVAL;                        /* [한국어] reinit 은 opts 받지 않음. */
		}

		printf("Starting %s / %s reinitialization...\n", SPDK_VERSION_STRING, rte_version());
		pci_env_reinit();                              /* [한국어] PCI 디바이스 리스트 재스캔(hotplug 외 정적 변경 반영). */

		return 0;
	}

	if (opts_user == NULL) {                               /* [한국어] 첫 호출인데 opts 미제공 → 명시 에러. */
		fprintf(stderr, "NULL arguments to initialize DPDK\n");
		return -EINVAL;
	}

	/* [한국어] ABI 최소 크기 = opts_size 멤버까지 포함한 길이. 사용자가 그보다 작은 크기를
	 * 명시하면 강제로 min_opts_size 로 끌어올려 안전 보정한다. */
	min_opts_size = offsetof(struct spdk_env_opts, opts_size) + sizeof(opts->opts_size);
	user_opts_size = opts_user->opts_size;
	if (user_opts_size < min_opts_size) {
		fprintf(stderr, "Invalid opts->opts_size %d too small, please set opts_size correctly\n",
			(int)opts_user->opts_size);
		user_opts_size = min_opts_size;
	}

	env_copy_opts(opts, opts_user, user_opts_size);        /* [한국어] 내부 opts 채움. */

	settings = OPENSSL_INIT_new();                         /* [한국어] OpenSSL 초기화 설정 객체 생성 — 추후 free 필요. */
	if (!settings) {
		fprintf(stderr, "Failed to create openssl settings object\n");
		ERR_print_errors_fp(stderr);                   /* [한국어] OpenSSL 에러 스택을 stderr 로 출력. */
		return -ENOMEM;
	}

#if OPENSSL_VERSION_NUMBER >= 0x30000000 /* OPENSSL 3.0.0 */
	OPENSSL_INIT_set_config_file_flags(settings, 0);       /* [한국어] OpenSSL 3.0+ 에서 config 로딩 플래그 0 = 기본 동작. */
#endif
	rc = OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, settings);  /* [한국어] OpenSSL 라이브러리 초기화 — TLS 알고리즘/엔진/설정 로드. */
	if (rc != 1) {                                         /* [한국어] OpenSSL 은 성공 = 1 (POSIX 와 다름). */
		fprintf(stderr, "Failed to initialize OpenSSL\n");
		ERR_print_errors_fp(stderr);
		return -EINVAL;
	}
	OPENSSL_INIT_free(settings);                           /* [한국어] settings 객체 정리. */

	rc = build_eal_cmdline(opts);                          /* [한국어] opts → DPDK argv 합성 (g_eal_cmdline 채움). */
	if (rc < 0) {
		SPDK_ERRLOG("Invalid arguments to initialize DPDK\n");
		return -EINVAL;
	}

	SPDK_PRINTF("Starting %s / %s initialization...\n", SPDK_VERSION_STRING, rte_version());

	/* [한국어] 합성된 EAL 인자를 사람이 읽기 쉬운 한 줄로 출력 — 디버깅에 매우 유용. */
	args_print = _sprintf_alloc("[ DPDK EAL parameters: ");
	if (args_print == NULL) {
		return -ENOMEM;
	}
	for (i = 0; i < g_eal_cmdline_argcount; i++) {
		args_tmp = args_print;                                                   /* [한국어] 이전 누적 문자열을 임시 보관(이후 free). */
		args_print = _sprintf_alloc("%s%s ", args_tmp, g_eal_cmdline[i]);        /* [한국어] 새 토큰을 한 칸 띄워 누적. */
		if (args_print == NULL) {
			free(args_tmp);
			return -ENOMEM;
		}
		free(args_tmp);                                                           /* [한국어] 이전 사본 해제. */
	}
	SPDK_PRINTF("%s]\n", args_print);
	free(args_print);

	/* DPDK rearranges the array we pass to it, so make a copy
	 * before passing so we can still free the individual strings
	 * correctly.
	 */
	/* [한국어] rte_eal_init 은 argv 의 포인터 순서를 직접 재배열하므로 g_eal_cmdline 을 그대로
	 * 넘기면 우리가 추후 free_args 로 정리하기 어려워진다. 그래서 얕은 복사본만 만들어 전달하고
	 * 원본 g_eal_cmdline 은 SPDK 가 정리 책임을 보유. */
	dpdk_args = calloc(g_eal_cmdline_argcount, sizeof(char *));
	if (dpdk_args == NULL) {
		SPDK_ERRLOG("Failed to allocate dpdk_args\n");
		return -ENOMEM;
	}
	memcpy(dpdk_args, g_eal_cmdline, sizeof(char *) * g_eal_cmdline_argcount);

	fflush(stdout);                                  /* [한국어] EAL 의 stderr 메시지가 우리 stdout 메시지보다 앞서 나오지 않도록 비움. */
	orig_optind = optind;                            /* [한국어] DPDK 가 getopt(3) 의 optind 를 임의로 바꿀 수 있어 백업. */
	optind = 1;                                      /* [한국어] DPDK 의 getopt 가 argv[0]=프로그램명, argv[1]부터 옵션이라고 기대하므로 1 로 리셋. */
	rc = rte_eal_init(g_eal_cmdline_argcount, dpdk_args);  /* [한국어] DPDK 핵심 초기화 — hugepage/lcore/PCI 모두 여기서. */
	optind = orig_optind;                            /* [한국어] 호출자 getopt 상태 복원 — 사용자 main 의 옵션 파싱이 깨지지 않도록. */

	free(dpdk_args);                                 /* [한국어] 얕은 복사 배열 해제. 각 토큰 문자열은 g_eal_cmdline 이 보유하므로 free 안 함. */

	if (rc < 0) {                                    /* [한국어] EAL 초기화 실패. */
		if (rte_errno == EALREADY) {
			SPDK_ERRLOG("DPDK already initialized\n");   /* [한국어] 다른 누군가가 이미 초기화함 — SPDK 외부 라이브러리 충돌 케이스. */
		} else {
			SPDK_ERRLOG("Failed to initialize DPDK\n");
		}
		return -rte_errno;                       /* [한국어] DPDK 의 모듈 errno 를 음수로 부호 반전해 반환. */
	}

#ifdef __FreeBSD__
	/**
	 * DPDK always uses legacy mem mode in FreeBSD.
	 */
	legacy_mem = true;                                /* [한국어] FreeBSD 빌드는 dynamic mem 미지원 — 항상 legacy. */
#else
	legacy_mem = false;
	if (opts->no_huge || (opts->env_context && strstr(opts->env_context, "--legacy-mem") != NULL)) {
		legacy_mem = true;                        /* [한국어] no_huge 또는 사용자가 legacy-mem 명시 시 legacy 모드로 간주. */
	}
#endif

	rc = spdk_env_dpdk_post_init(legacy_mem);         /* [한국어] PCI/메모리 맵/vtophys 부팅 마무리. */
	if (rc == 0) {
		g_external_init = false;                  /* [한국어] 이제 SPDK 가 EAL 의 소유주 — destructor 가 cleanup 하도록 표시. */
	}

	return rc;
}

/* We use priority 101 which is the highest priority level available
 * to applications (the toolchains reserve 1 to 100 for internal usage).
 * This ensures this destructor runs last, after any other destructors
 * that might still need the environment up and running.
 */
/*
 * [한국어]
 * dpdk_cleanup - 프로세스 종료 시 자동으로 rte_eal_cleanup 호출
 *
 * @return: void.
 *
 * GCC __attribute__((destructor(prio))) 는 main 종료 후 atexit 핸들러보다도
 * 늦게 호출되며, prio 가 클수록 더 나중에 실행된다. 1~100 은 toolchain 예약,
 * 101 은 사용자가 쓸 수 있는 가장 큰 값(=가장 늦게 실행). 이 우선순위 덕분에
 * 다른 SPDK 라이브러리들이 EAL 을 아직 사용하는 동안 정리되지 않는다.
 *
 * SPDK 가 EAL 을 직접 초기화한 경우(=g_external_init==false)에만 cleanup
 * 한다 — 외부에서 EAL 을 관리하는 호스트(예: DPDK 단독 앱과 임베드된 SPDK)
 * 에서는 호스트가 cleanup 책임을 진다.
 */
__attribute__((destructor(101))) static void
dpdk_cleanup(void)
{
	/* Only call rte_eal_cleanup if the SPDK env library called rte_eal_init. */
	if (!g_external_init) {            /* [한국어] SPDK 가 EAL 소유주일 때만 cleanup. */
		rte_eal_cleanup();         /* [한국어] hugepage 매핑/lcore 스레드/PCI bus 모두 정리. */
	}
}

/*
 * [한국어]
 * spdk_env_fini - SPDK 환경 정상 종료(공개 API)
 *
 * spdk_env_init 의 대응 함수. 사용자 코드가 reactor 정지/스레드 라이브러리
 * 종료 후 마지막에 호출. 실제 EAL cleanup 은 destructor 에서 일어나므로
 * 여기서는 SPDK 측 후처리만 정리한다.
 *
 * 호출 체인:
 *   사용자 → spdk_env_fini() → spdk_env_dpdk_post_fini()
 */
void
spdk_env_fini(void)
{
	spdk_env_dpdk_post_fini();         /* [한국어] vtophys/mem_map/pci_env 정리 + g_eal_cmdline 해제. */
}

/*
 * [한국어]
 * spdk_env_dpdk_external_init - "현재 SPDK env 가 외부에서 초기화되었거나
 *                               아직 초기화되지 않은 상태인가?" 를 알림
 *
 * @return: true = SPDK 가 EAL 의 소유주가 아님(외부에서 init 했거나 아직 안 함),
 *          false = spdk_env_init 이 성공해 SPDK 가 EAL 소유주.
 *
 * 사용자/상위 라이브러리가 EAL cleanup 의 책임 위치를 판단할 때 사용. 예를
 * 들어 DPDK 단독 앱에 SPDK 를 임베드한 경우 호스트가 EAL 을 먼저 init 하고
 * 본 함수가 true 를 반환하는 동안 SPDK 의 destructor 는 cleanup 을 건너뛴다.
 */
bool
spdk_env_dpdk_external_init(void)
{
	return g_external_init;
}
