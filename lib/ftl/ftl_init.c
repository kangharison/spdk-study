/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL 디바이스 초기화/해제 진입점 (ftl_init.c)
 *
 * === 파일의 역할 ===
 * SPDK FTL의 공개 API인 spdk_ftl_dev_init()/spdk_ftl_dev_free()를 구현한다.
 * 사용자(주로 module/bdev/ftl)가 호출하면 이 파일이:
 *  1) struct spdk_ftl_dev를 calloc하여 기본 필드(properties, conf, core_thread,
 *     큐 헤드, writer 인스턴스)를 채우고,
 *  2) 관리(mngt) FSM의 startup 시퀀스(ftl_mngt_call_dev_startup)를 시작하여
 *     베이스/캐시 디바이스 attach, 메타 영역 설정, L2P/Bands 복원 등을 비동기로 진행하며,
 *  3) 최종 결과를 사용자 콜백으로 통지한다.
 * shutdown은 동일한 패턴의 반대 방향(ftl_mngt_call_dev_shutdown)으로 처리된다.
 * 초기화 실패 시 init_retry 플래그가 켜져 있으면 한 번 자동 재시도한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (위쪽이 사용자, 아래쪽이 본 파일):
 *   bdev_ftl 모듈 (module/bdev/ftl) → spdk_ftl_dev_init / spdk_ftl_dev_free
 *     → 본 파일 → ftl_mngt_call_dev_startup/shutdown (mngt FSM)
 *       → 베이스/NV cache 디바이스 open, layout 결정, L2P 초기화/복원, writer 시작 …
 * 실행 컨텍스트:
 *  - 진입(API): 사용자가 호출한 SPDK 스레드 (보통 reactor의 spdk_thread).
 *  - 코어 스레드(dev->core_thread): conf.core_mask로 분리 생성하거나 현재 스레드 사용.
 *  - mngt FSM의 단계별 콜백은 코어 스레드에서 실행되도록 보장됨.
 *
 * === 타 모듈과의 연결 ===
 * - mngt/ftl_mngt.h: ftl_mngt_call_dev_startup/shutdown — startup/shutdown 단계의 FSM.
 * - ftl_writer.h: ftl_writer_init — user/GC writer 인스턴스 초기화.
 * - ftl_core.h: struct spdk_ftl_dev 정의 (rd_sq/wr_sq/trim_sq/ioch_queue 등).
 * - ftl_properties: 디바이스 속성 키-값 저장소.
 * - spdk/thread.h: spdk_thread_create/exit/get_thread/send_msg — 코어 스레드 관리.
 * 데이터 흐름: 사용자 conf → spdk_ftl_dev 인스턴스 → 사용자 cb_fn 호출(dev 또는 NULL).
 * 공유 자료구조: dev->rd_sq/wr_sq/trim_sq(submission queues),
 * dev->ioch_queue(I/O 채널 리스트), dev->writer_user/writer_gc.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ftl_dev_init_ctx: spdk_ftl_dev_init용 콜백 컨텍스트(cb_fn/cb_arg).
 * - struct ftl_dev_free_ctx: spdk_ftl_dev_free용 콜백 컨텍스트.
 * - init_core_thread/deinit_core_thread: 코어 스레드 생성/종료.
 * - allocate_dev/free_dev: spdk_ftl_dev 인스턴스 할당/해제.
 * - dev_init_cb: startup FSM 완료 트램폴린(실패 시 retry 처리 포함).
 * - spdk_ftl_dev_init: 공개 init API. mngt startup을 시작.
 * - dev_free_cb / spdk_ftl_dev_free: 공개 deinit API와 그 트램폴린.
 */

#include "spdk/stdinc.h"        /* [한국어] 표준 C 헤더(memset/calloc/free 등). */
#include "spdk/nvme.h"          /* [한국어] NVMe 드라이버 API — FTL 전체에서 간접적으로 사용. */
#include "spdk/thread.h"        /* [한국어] spdk_thread/poller/메시지 API — 코어 스레드 생성에 필요. */
#include "spdk/string.h"        /* [한국어] spdk_strerror 등 문자열 헬퍼. */
#include "spdk/likely.h"        /* [한국어] spdk_likely/unlikely 분기 힌트. */
#include "spdk/ftl.h"           /* [한국어] FTL 공개 API (spdk_ftl_dev_init/free 시그니처). */
#include "spdk/bdev_module.h"   /* [한국어] bdev 모듈 등록 매크로 — FTL 자체는 모듈 등록을 하지 않으나 헤더 의존. */
#include "spdk/config.h"        /* [한국어] 빌드 시 결정되는 SPDK_FTL_* 매크로. */

#include "ftl_core.h"           /* [한국어] struct spdk_ftl_dev 본체 정의. */
#include "ftl_io.h"             /* [한국어] FTL I/O 객체 — ioch_queue 등에 사용. */
#include "ftl_band.h"           /* [한국어] 밴드 관리. */
#include "ftl_debug.h"          /* [한국어] 디버그 덤프 도우미. */
#include "ftl_nv_cache.h"       /* [한국어] NV cache (write buffer) 인터페이스. */
#include "ftl_writer.h"         /* [한국어] writer(user/GC) 인스턴스 초기화. */
#include "ftl_utils.h"          /* [한국어] 공통 유틸 매크로(FTL_ERRLOG 등). */
#include "mngt/ftl_mngt.h"      /* [한국어] 관리 FSM — startup/shutdown 단계 진행기. */

/*
 * [한국어]
 * struct ftl_dev_init_ctx - spdk_ftl_dev_init 비동기 결과 전달용 컨텍스트
 */
struct ftl_dev_init_ctx {
	spdk_ftl_init_fn		cb_fn;
	/* [한국어] 사용자 init 완료 콜백.
	 * 설정자: spdk_ftl_dev_init 진입 시 인자 그대로 저장.
	 * 읽는 자: dev_init_cb가 cb_fn(dev, cb_arg, status) 형태로 호출.
	 * 시그니처: void (*)(struct spdk_ftl_dev*, void*, int).
	 * 값 범위: NULL 불가(상위 레이어가 보장). */

	/* Callback's argument */
	void				*cb_arg;
	/* [한국어] 사용자 콜백에 전달할 임의 컨텍스트 포인터.
	 * 설정자: spdk_ftl_dev_init 인자 저장.
	 * 읽는 자: dev_init_cb. 보통 module/bdev/ftl의 etl_bdev_init 컨텍스트.
	 * 동기화: 한 ctx는 한 init 호출 당 1개 — race 없음. */
};

/*
 * [한국어]
 * struct ftl_dev_free_ctx - spdk_ftl_dev_free 비동기 결과 전달용 컨텍스트
 */
struct ftl_dev_free_ctx {
	spdk_ftl_fn			cb_fn;
	/* [한국어] 사용자 shutdown 완료 콜백.
	 * 설정자: spdk_ftl_dev_free 진입 시 저장.
	 * 읽는 자: dev_free_cb가 cb_fn(cb_arg, status) 형태로 호출.
	 * 시그니처: void (*)(void*, int) — init과 달리 dev 포인터를 넘기지 않음
	 *   (이미 free되었으므로). */

	/* Callback's argument */
	void				*cb_arg;
	/* [한국어] 사용자 콜백에 전달할 임의 컨텍스트.
	 * 설정자: spdk_ftl_dev_free 인자 저장.
	 * 읽는 자: dev_free_cb. */
};

/*
 * [한국어]
 * init_core_thread - FTL 코어 스레드 생성 또는 현재 스레드를 코어로 지정
 *
 * @dev: 새로 할당된 spdk_ftl_dev. dev->conf가 이미 채워진 상태여야 함.
 * @return: 0 성공, -EINVAL/-ENOMEM 실패.
 *
 * conf.core_mask가 지정되면 해당 CPU 마스크로 새 spdk_thread를 생성하고
 * (이름: "ftl_core_thread"), 이 스레드 위에서 모든 FTL 코어 작업이 실행된다.
 * core_mask가 없으면 호출자의 현재 spdk_thread를 그대로 사용한다.
 * 실행 컨텍스트: 사용자가 spdk_ftl_dev_init을 호출한 스레드.
 *
 * 호출 체인:
 *   spdk_ftl_dev_init → allocate_dev → [init_core_thread]
 *     → spdk_cpuset_parse / spdk_thread_create / spdk_get_thread
 */
static int
init_core_thread(struct spdk_ftl_dev *dev)
{
	struct spdk_cpuset cpumask = {}; /* [한국어] CPU 마스크 — 초기값은 비어있음(0). */

	/*
	 * If core mask is provided create core thread on first cpu that match with the mask,
	 * otherwise use current user thread
	 */
	/* [한국어] core_mask가 있으면 그 마스크에 매칭되는 첫 CPU에 신규 스레드 배치,
	 * 없으면 현재 사용자 스레드를 코어 스레드로 재사용. */
	if (dev->conf.core_mask) {
		if (spdk_cpuset_parse(&cpumask, dev->conf.core_mask)) { /* [한국어] "0x1", "[0,2-5]" 등 마스크 문자열 파싱. */
			return -EINVAL;                                         /* [한국어] 파싱 실패 — 사용자 입력 오류. */
		}
		dev->core_thread = spdk_thread_create("ftl_core_thread", &cpumask);
		/* [한국어] DPDK reactor 위에 새 spdk_thread 생성. 이름은 디버그용,
		 * cpumask는 이 스레드가 어느 reactor에 배치될지를 결정. */
	} else {
		dev->core_thread = spdk_get_thread();
		/* [한국어] 현재 호출 스레드 자체를 코어 스레드로 사용 — 별도 스레드 생성 비용 없음. */
	}

	if (dev->core_thread == NULL) { /* [한국어] 두 경로 모두에서 실패 가능 (스레드 생성 실패 또는 미초기화 환경). */
		FTL_ERRLOG(dev, "Cannot create thread for mask %s\n", dev->conf.core_mask);
		return -ENOMEM;
	}

	return 0; /* [한국어] 성공 — dev->core_thread 사용 가능. */
}

/*
 * [한국어]
 * exit_thread - spdk_thread_send_msg 트램폴린: 자기 자신 스레드를 exit
 *
 * @ctx: 종료 대상 spdk_thread 포인터.
 *
 * spdk_thread_exit는 호출자가 그 스레드 위에서 실행되어야 안전하다.
 * deinit_core_thread는 외부 스레드에서 호출되므로, 코어 스레드로 이 메시지를
 * 보내 그 안에서 자기 자신을 exit하도록 한다.
 * 실행 컨텍스트: 코어 스레드 (메시지가 디스패치된 후).
 */
static void
exit_thread(void *ctx)
{
	struct spdk_thread *thread = ctx;   /* [한국어] 종료할 스레드 포인터 복원. */

	spdk_thread_exit(thread);            /* [한국어] 스레드 종료 표시 — 다음 reactor poll 사이클에서 정리됨. */
}

/*
 * [한국어]
 * deinit_core_thread - core_mask로 만든 코어 스레드를 안전하게 종료
 *
 * @dev: FTL 디바이스 핸들.
 * @return: 없음.
 *
 * core_mask 경로로 새 스레드를 만든 경우만 종료가 필요. 호출자가 다른 스레드일 수
 * 있어 send_msg로 코어 스레드 자신에게 exit를 위임한다. 기본 경로(core_mask 없음)
 * 에서는 스레드를 빌려 쓴 것이므로 종료 책임이 사용자에게 있다.
 * 실행 컨텍스트: free_dev에서 호출 — 통상 사용자(원 호출자) 스레드.
 *
 * 호출 체인:
 *   free_dev → [deinit_core_thread] → spdk_thread_send_msg → exit_thread
 */
static void
deinit_core_thread(struct spdk_ftl_dev *dev)
{
	if (dev->core_thread && dev->conf.core_mask) {
		/* [한국어] 분리 생성한 코어 스레드만 명시적 종료.
		 * 차용한 경우(core_mask=NULL)는 종료 책임이 호출자(상위 레이어)에 있음. */
		spdk_thread_send_msg(dev->core_thread, exit_thread,
				     dev->core_thread);
		/* [한국어] 코어 스레드로 exit 메시지 송신. lockless 메시지 큐로 전달. */
		dev->core_thread = NULL; /* [한국어] dangling 방지 — 이후 재참조 불가. */
	}
}

/*
 * [한국어]
 * free_dev - spdk_ftl_dev 인스턴스 해제 (idempotent)
 *
 * @dev: 해제 대상. NULL 허용 (no-op).
 * @return: 없음.
 *
 * 코어 스레드 → conf → properties → dev 자체 순으로 해제.
 * 실행 컨텍스트: 사용자 스레드(init 실패 롤백) 또는 dev_free_cb (shutdown 완료 후).
 *
 * 호출 체인:
 *   spdk_ftl_dev_init(error 경로) / dev_init_cb(retry 경로) / dev_free_cb
 *     → [free_dev] → deinit_core_thread / spdk_ftl_conf_deinit / ftl_properties_deinit
 */
static void
free_dev(struct spdk_ftl_dev *dev)
{
	if (!dev) {                                /* [한국어] NULL 안전 — 부분 초기화 실패에서도 호출 가능. */
		return;
	}

	deinit_core_thread(dev);                   /* [한국어] core_mask로 만든 스레드 종료(해당 시). */
	spdk_ftl_conf_deinit(&dev->conf);          /* [한국어] conf 내부 동적 멤버(문자열, mask 등) 해제. */
	ftl_properties_deinit(dev);                /* [한국어] 키-값 속성 저장소 해제. */
	free(dev);                                  /* [한국어] 인스턴스 자체 해제. */
}

/*
 * [한국어]
 * allocate_dev - spdk_ftl_dev 인스턴스 할당 및 기본 필드 초기화
 *
 * @conf: 사용자 설정. 디바이스 경로, core_mask, UUID 등.
 * @error: 실패 시 errno를 채워 호출자에 반환.
 * @return: 새 spdk_ftl_dev 또는 NULL(실패).
 *
 * 동작:
 *  1) calloc → 0초기화.
 *  2) ftl_properties_init: 키-값 속성 저장소.
 *  3) ftl_conf_init_dev: conf를 디바이스에 deep-copy.
 *  4) init_core_thread: 코어 스레드 결정.
 *  5) submission queue/ioch_queue 헤드 초기화.
 *  6) writer_user(컴팩션 경로) / writer_gc(GC 경로) 초기화.
 * 실행 컨텍스트: 호출자 스레드.
 *
 * 호출 체인:
 *   spdk_ftl_dev_init → [allocate_dev] → ftl_properties_init / ftl_conf_init_dev
 *     / init_core_thread / ftl_writer_init
 */
static struct spdk_ftl_dev *
allocate_dev(const struct spdk_ftl_conf *conf, int *error)
{
	int rc;                                                 /* [한국어] 단계별 반환값 보관. */
	struct spdk_ftl_dev *dev = calloc(1, sizeof(*dev));     /* [한국어] 0-초기화된 인스턴스 할당. */

	if (!dev) {
		FTL_ERRLOG(dev, "Cannot allocate FTL device\n"); /* [한국어] OOM. dev=NULL이지만 매크로는 안전 처리. */
		*error = -ENOMEM;
		return NULL;
	}

	rc = ftl_properties_init(dev);                          /* [한국어] 키-값 속성 저장소 초기화. */
	if (rc) {
		*error = rc;
		goto error;
	}

	rc = ftl_conf_init_dev(dev, conf);                      /* [한국어] 사용자 conf를 dev->conf로 deep-copy. */
	if (rc) {
		*error = rc;
		goto error;
	}

	rc = init_core_thread(dev);                             /* [한국어] core_mask 기반으로 dev->core_thread 결정. */
	if (rc) {
		*error = rc;
		goto error;
	}

	TAILQ_INIT(&dev->rd_sq);                                /* [한국어] 읽기 submission queue 헤드 초기화. */
	TAILQ_INIT(&dev->wr_sq);                                /* [한국어] 쓰기 submission queue 헤드 초기화. */
	TAILQ_INIT(&dev->trim_sq);                              /* [한국어] TRIM submission queue 헤드 초기화. */
	TAILQ_INIT(&dev->ioch_queue);                           /* [한국어] I/O 채널 리스트 헤드 초기화. */

	ftl_writer_init(dev, &dev->writer_user, SPDK_FTL_LIMIT_HIGH, FTL_BAND_TYPE_COMPACTION);
	/* [한국어] 사용자(컴팩션) writer 초기화.
	 * - SPDK_FTL_LIMIT_HIGH: 이 writer는 high 압박 한도까지 진행 가능.
	 * - FTL_BAND_TYPE_COMPACTION: 사용자 데이터(또는 컴팩션) 전용 밴드 사용. */
	ftl_writer_init(dev, &dev->writer_gc, SPDK_FTL_LIMIT_CRIT, FTL_BAND_TYPE_GC);
	/* [한국어] GC writer 초기화.
	 * - SPDK_FTL_LIMIT_CRIT: GC는 critical 한도까지 동작 가능 (자유 밴드 확보가 우선).
	 * - FTL_BAND_TYPE_GC: GC 전용 밴드를 사용. */

	return dev;                                              /* [한국어] 성공. */
error:
	free_dev(dev);                                           /* [한국어] 부분 초기화 롤백. */
	return NULL;
}

/*
 * [한국어]
 * dev_init_cb - mngt startup FSM 완료 트램폴린
 *
 * @dev: startup이 진행된 FTL 디바이스.
 * @_ctx: ftl_dev_init_ctx (cb_fn/cb_arg).
 * @status: startup 결과. 0 성공, 음수 errno.
 *
 * 동작:
 *  - 실패이고 init_retry가 켜져 있으면 spdk_ftl_dev_init을 한 번 재호출하여 재시도.
 *    재시도 등록에 성공하면 현재 dev를 free하고 ctx도 free한 뒤 return —
 *    재시도 호출이 자기 자신의 ctx를 새로 만들어 종료를 책임짐.
 *  - 실패이고 retry가 안 되거나 켜지지 않았으면 dev를 free하고 dev=NULL로 사용자 cb 호출.
 *  - 성공이면 dev 그대로 사용자 cb 호출.
 * 실행 컨텍스트: 코어 스레드 (mngt FSM이 코어 스레드에서 콜백을 부름).
 *
 * 호출 체인:
 *   ftl_mngt_call_dev_startup → (mngt FSM 완료) → [dev_init_cb] → ctx->cb_fn
 *     (실패 + retry) → spdk_ftl_dev_init (재진입)
 */
static void
dev_init_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_dev_init_ctx *ctx = _ctx;     /* [한국어] init 컨텍스트 복원. */
	int rc;                                   /* [한국어] 재시도 호출 결과. */

	if (status) {                             /* [한국어] startup이 어떤 단계에서든 실패. */
		if (dev->init_retry) {                /* [한국어] 자동 재시도 정책이 활성화된 경우. */
			FTL_NOTICELOG(dev, "Startup retry\n");
			rc = spdk_ftl_dev_init(&dev->conf, ctx->cb_fn, ctx->cb_arg);
			/* [한국어] 동일 conf로 다시 init 시도. 새 ctx를 내부에서 생성. */
			if (!rc) {                       /* [한국어] 재시도 등록 성공. 기존 dev/ctx는 더 이상 필요 없음. */
				free_dev(dev);
				free(ctx);
				return;
			}
			FTL_NOTICELOG(dev, "Startup retry failed: %d\n", rc);
		}

		free_dev(dev);                        /* [한국어] 실패 확정 — dev 정리. */
		dev = NULL;                           /* [한국어] 사용자에게 NULL을 넘겨 init 실패를 명확히 통지. */
	}
	ctx->cb_fn(dev, ctx->cb_arg, status);     /* [한국어] 사용자 콜백 — 성공 시 dev, 실패 시 NULL과 status. */
	free(ctx);                                 /* [한국어] init ctx 해제. */
}

/*
 * [한국어]
 * spdk_ftl_dev_init - FTL 디바이스 초기화 공개 API
 *
 * @conf: 사용자 설정 (디바이스 경로, core_mask, UUID, 정책 등).
 * @cb_fn: 비동기 완료 콜백 — cb_fn(dev, cb_arg, status).
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 성공(비동기 시작), 음수 errno(동기 실패 — 콜백 호출되지 않음).
 *
 * 동작:
 *  1) ftl_dev_init_ctx 할당.
 *  2) allocate_dev로 dev 인스턴스 준비.
 *  3) ftl_mngt_call_dev_startup으로 startup FSM 시작.
 *  4) 성공이면 0 반환 (완료는 dev_init_cb로).
 *  5) 어떤 단계든 실패하면 ctx/dev를 회수하고 errno 반환.
 * 실행 컨텍스트: 사용자 호출 스레드 (보통 reactor의 spdk_thread).
 *
 * 호출 체인:
 *   bdev_ftl 모듈 등 → [spdk_ftl_dev_init] → allocate_dev / ftl_mngt_call_dev_startup
 *     → (비동기) dev_init_cb → 사용자 cb_fn
 */
int
spdk_ftl_dev_init(const struct spdk_ftl_conf *conf, spdk_ftl_init_fn cb_fn, void *cb_arg)
{
	int rc = -1;                                  /* [한국어] 기본 실패 코드. */
	struct ftl_dev_init_ctx *ctx;                  /* [한국어] 비동기 컨텍스트. */
	struct spdk_ftl_dev *dev = NULL;               /* [한국어] 인스턴스 — 실패 시 free_dev로 회수. */

	ctx = calloc(1, sizeof(*ctx));                 /* [한국어] 컨텍스트 할당. */
	if (!ctx) {
		rc = -ENOMEM;
		goto error;
	}
	ctx->cb_fn = cb_fn;                            /* [한국어] 사용자 콜백 저장. */
	ctx->cb_arg = cb_arg;                          /* [한국어] 콜백 컨텍스트 저장. */

	dev = allocate_dev(conf, &rc);                 /* [한국어] dev 할당 — 실패 시 rc에 errno. */
	if (!dev) {
		goto error;
	}

	rc = ftl_mngt_call_dev_startup(dev, dev_init_cb, ctx);
	/* [한국어] 관리 FSM의 startup 시퀀스 시작.
	 * 성공적으로 등록되면 0을 반환하고, 완료 시 dev_init_cb가 호출됨. */
	if (rc) {
		goto error;
	}

	return 0;                                       /* [한국어] 비동기 시작 성공 — 결과는 콜백으로. */

error:
	free(ctx);                                      /* [한국어] ctx가 NULL이어도 free는 안전. */
	free_dev(dev);                                  /* [한국어] dev가 NULL이어도 안전. */
	return rc;                                      /* [한국어] 동기 실패 errno. */
}

/*
 * [한국어]
 * dev_free_cb - mngt shutdown FSM 완료 트램폴린
 *
 * @dev: 종료 진행된 FTL 디바이스.
 * @_ctx: ftl_dev_free_ctx.
 * @status: shutdown 결과 (0 성공).
 *
 * 성공 시 dev를 해제하고, 실패해도 사용자 cb는 반드시 호출한다(상태 코드로 통지).
 * 실패 시 dev를 굳이 free하지 않는 이유: shutdown이 부분적으로 완료되어 일부 모듈이
 * 활성 상태일 수 있어 free가 위험할 수 있기 때문(상위 레이어가 결정).
 * 실행 컨텍스트: 코어 스레드.
 *
 * 호출 체인:
 *   ftl_mngt_call_dev_shutdown → (FSM 완료) → [dev_free_cb] → 사용자 cb_fn
 */
static void
dev_free_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_dev_free_ctx *ctx = _ctx; /* [한국어] free 컨텍스트 복원. */

	if (!status) {                       /* [한국어] shutdown 성공 시에만 dev 메모리 회수. */
		free_dev(dev);
	}
	ctx->cb_fn(ctx->cb_arg, status);     /* [한국어] 사용자에게 결과 통지. */
	free(ctx);                            /* [한국어] free ctx 해제. */
}

/*
 * [한국어]
 * spdk_ftl_dev_free - FTL 디바이스 종료 공개 API
 *
 * @dev: 종료할 FTL 디바이스 핸들.
 * @cb_fn: 비동기 완료 콜백 (시그니처: void(*)(void*, int)).
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 성공(비동기 시작), 음수 errno(동기 실패).
 *
 * mngt FSM의 shutdown 시퀀스를 시작한다. shutdown은 사용자 I/O 정지 →
 * GC/컴팩션 정지 → L2P/Bands persist → NV cache flush → device close 순으로 진행된다.
 * 실행 컨텍스트: 사용자 호출 스레드. shutdown FSM은 내부적으로 코어 스레드 위에서 실행.
 *
 * 호출 체인:
 *   bdev_ftl 모듈 → [spdk_ftl_dev_free] → ftl_mngt_call_dev_shutdown
 *     → (비동기) dev_free_cb → 사용자 cb_fn
 */
int
spdk_ftl_dev_free(struct spdk_ftl_dev *dev, spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc = -1;                                /* [한국어] 기본 실패 코드. */
	struct ftl_dev_free_ctx *ctx;                /* [한국어] free 컨텍스트. */

	ctx = calloc(1, sizeof(*ctx));               /* [한국어] 컨텍스트 할당. */
	if (!ctx) {
		rc = -ENOMEM;
		goto error;
	}
	ctx->cb_fn = cb_fn;                          /* [한국어] 사용자 콜백 저장. */
	ctx->cb_arg = cb_arg;                        /* [한국어] 콜백 컨텍스트 저장. */

	rc = ftl_mngt_call_dev_shutdown(dev, dev_free_cb, ctx);
	/* [한국어] 관리 FSM의 shutdown 시퀀스 시작. 성공 시 0 반환, 완료는 dev_free_cb로. */
	if (rc) {
		goto error;
	}

	return 0;                                     /* [한국어] 비동기 시작 성공. */

error:
	free(ctx);                                    /* [한국어] ctx 회수. dev는 호출자 소유 그대로 유지. */
	return rc;
}

/* [한국어] SPDK 로그 컴포넌트 등록 — "ftl_init" 플래그가 SPDK 로그 시스템에 등록되어
 * spdk_log_set_flag("ftl_init")으로 본 모듈의 INFOLOG/DEBUGLOG를 토글할 수 있다. */
SPDK_LOG_REGISTER_COMPONENT(ftl_init)
