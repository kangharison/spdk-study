/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK DMA memory domain 추상화 구현부 (dma.c)
 *
 * === 파일의 역할 ===
 * include/spdk/dma.h가 노출하는 "memory domain" 추상화의 실체 구현. 한 SPDK
 * 프로세스는 다양한 위치(host process RAM, GPU memory, RDMA-registered region,
 * NVMe CMB 등)에 흩어져 있는 메모리를 다룰 수 있어야 하는데, 그 각 위치를
 * spdk_memory_domain 객체로 추상화하고 본 파일이 그 lifecycle, 등록부(전역
 * TAILQ), 콜백 dispatcher를 제공한다. 도메인은 type + 6개의 콜백(translate /
 * pull / push / transfer / invalidate / memzero) + 사용자 컨텍스트로 구성되며,
 * 본 파일은 콜백을 직접 구현하지 않고 프록시처럼 호출자(bdev/transport/accel)
 * 와 도메인 등록자(RDMA transport, accel module 등) 사이의 dispatch만 담당한다.
 * "system domain" 1개는 constructor에서 자동 등록되어 평범한 host RAM을
 * 표현한다 — 데이터 콜백을 갖지 않으므로 데이터 이동 요청이 오면 -ENOTSUP.
 *
 * === 전체 아키텍처에서의 위치 ===
 * cross-cutting layer. 호출 체인:
 *   - 등록 측: lib/nvme/nvme_rdma.c, module/accel/dpdk_compressdev/* 등이
 *              spdk_memory_domain_create()로 자기 도메인 등록 → 본 파일이
 *              g_dma_memory_domains TAILQ에 link.
 *   - 사용 측: bdev I/O 제출 시 호출자가 spdk_bdev_ext_io_opts.memory_domain로
 *              자기 버퍼의 도메인을 지정 → bdev/nvme transport가 본 파일의
 *              spdk_memory_domain_translate_data / pull_data / push_data를
 *              호출 → 본 파일이 도메인 콜백으로 위임.
 *   - 결정: transport는 보통 먼저 translate를 시도(zero-copy lkey/rkey 획득).
 *           translate 콜백이 없거나 실패하면 pull/push로 호스트 경유 복사.
 * 실행 컨텍스트: 등록/destroy는 보통 init/fini 단계의 main thread. 데이터
 *   dispatcher(pull/push/transfer/translate/...)는 임의의 SPDK reactor에서
 *   호출 — 도메인 콜백 자체가 reactor-aware해야 한다. g_dma_memory_domains
 *   TAILQ만 g_dma_mutex로 보호한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/dma.h(공개 API/typedef), spdk/log.h(SPDK_ERRLOG),
 *             spdk/util.h(spdk_min), spdk/likely.h(spdk_unlikely 분기 힌트).
 * 의존됨: lib/bdev/, lib/nvme/, lib/nvmf/, lib/accel/, module/bdev/nvme 등이
 *         본 파일 함수들을 통해 cross-domain I/O를 디스패치한다.
 * 데이터 흐름:
 *   register: producer → spdk_memory_domain_create → g_dma_memory_domains TAILQ.
 *   dispatch: caller (bdev_io 처리 중) → spdk_memory_domain_<op>_data →
 *             domain->op_cb → 실제 RDMA WR/copy/etc.
 * 동기화: TAILQ 변경(create/destroy/get_first/get_next)은 g_dma_mutex로 보호.
 *   콜백 setter들은 도메인 owner가 init 시점에만 부르는 것을 가정 → 락 없음.
 *   데이터 dispatcher들은 도메인 콜백 포인터만 read하므로 setter와의 race를
 *   피하기 위해 owner는 도메인을 등록하기 전 모든 setter를 호출해야 한다(관용).
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_memory_domain (정의 위치: 본 파일):
 *       opaque 도메인 객체. 외부에서는 spdk/dma.h가 forward 선언만 노출하므로
 *       필드에 직접 접근 불가. type/콜백 6종/링크 포인터/ctx/id/user_ctx 보유.
 *   - g_system_domain: constructor가 자동 등록하는 type=DMA, id="system"인
 *       기본 도메인. 콜백이 없으므로 데이터 op는 -ENOTSUP 반환 — 단순히
 *       "host process 메모리"를 식별하는 sentinel 역할.
 *   - spdk_memory_domain_create/destroy: 동적 도메인 lifecycle. user_ctx는
 *       flexible array로 도메인 객체에 연접 할당 (cache locality).
 *   - spdk_memory_domain_set_*: 콜백 6종 setter. assert(domain) 외 검증 없음.
 *   - spdk_memory_domain_pull/push/transfer/translate/invalidate/memzero_data:
 *       호출자가 사용하는 데이터 op dispatcher. 콜백이 NULL이면 -ENOTSUP
 *       (invalidate는 void이므로 silent return).
 *   - spdk_memory_domain_get_first/_next: id 필터 지원하는 도메인 순회. id가
 *       NULL이면 전체 순회, 아니면 같은 id를 가진 첫 도메인부터.
 *   - spdk_memory_domain_get_system_domain: g_system_domain 포인터 반환.
 */

#include "spdk/dma.h"      /* [한국어] 공개 API 정의 — 외부 호출자와 본 파일이 공유하는 타입/콜백 typedef */
#include "spdk/log.h"      /* [한국어] SPDK_ERRLOG/INFOLOG — 등록·할당 실패 진단 출력 */
#include "spdk/util.h"     /* [한국어] spdk_min, offsetof 지원 — ctx_size 안전 truncation에 사용 */
#include "spdk/likely.h"   /* [한국어] spdk_unlikely(...) 분기 힌트 — 콜백 미설정은 hot path에서 드물다 */

/* [한국어] 전역 도메인 등록부 보호 락. create/destroy/get_first/get_next 4곳에서만 잡는다.
 * static initializer (PTHREAD_MUTEX_INITIALIZER)이므로 별도 init 호출 불필요 —
 * SPDK 라이브러리 초기화 순서에 의존하지 않고 constructor에서도 안전하게 사용 가능. */
static pthread_mutex_t g_dma_mutex = PTHREAD_MUTEX_INITIALIZER;

/* [한국어] 등록된 모든 spdk_memory_domain의 전역 TAILQ. 헤드는 sentinel.
 * 저장 순서: g_system_domain이 항상 첫 원소(constructor가 가장 먼저 INSERT_TAIL).
 * 보호: g_dma_mutex. _link 멤버는 spdk_memory_domain 구조체 안에 있다. */
static TAILQ_HEAD(, spdk_memory_domain) g_dma_memory_domains = TAILQ_HEAD_INITIALIZER(
			g_dma_memory_domains);

/* [한국어] 메모리 도메인 본체 — 외부에서는 opaque이므로 본 파일이 유일한 정의처.
 * 객체는 calloc(sizeof(*domain) + user_ctx_size)로 가변 길이 할당된다 — user_ctx[]는
 * flexible array member라 도메인 헤더와 한 번에 할당되어 캐시 locality가 좋다. */
struct spdk_memory_domain {
	enum spdk_dma_device_type type;
	/* [한국어] 도메인이 표현하는 DMA 디바이스 종류 (RDMA/DMA/etc).
	 * 설정자: spdk_memory_domain_create()의 type 인자.
	 * 읽는 자: 호출자가 spdk_memory_domain_get_dma_device_type()으로 조회 후
	 *   서로 호환되는 도메인 짝(예: 둘 다 같은 RDMA PD인지)을 결정.
	 * 값 범위: enum spdk_dma_device_type (spdk/dma.h 참조).
	 * 동기화: 등록 후 변경되지 않음 — 락 불필요. */

	spdk_memory_domain_pull_data_cb pull_cb;
	/* [한국어] src_domain → 호스트 메모리로 비동기 데이터 끌어오기 콜백.
	 * 설정자: spdk_memory_domain_set_pull(). 도메인 owner가 init 시점에 등록.
	 * 읽는 자: spdk_memory_domain_pull_data() — NULL이면 -ENOTSUP.
	 * 사용 예: GPU 메모리에서 호스트 RAM으로 cudaMemcpyAsync. */

	spdk_memory_domain_push_data_cb push_cb;
	/* [한국어] 호스트 메모리 → dst_domain으로 비동기 데이터 보내기 콜백.
	 * 설정자: spdk_memory_domain_set_push(). 읽는 자: push_data dispatcher. */

	spdk_memory_domain_transfer_data_cb transfer_cb;
	/* [한국어] src_domain → dst_domain 직접 전송 (zero-copy 가능 시).
	 * 설정자: spdk_memory_domain_set_data_transfer(). 읽는 자: transfer_data.
	 * 사용 예: GPU↔NVMe CMB 직접 DMA, GPU↔GPU peer-to-peer copy. */

	spdk_memory_domain_translate_memory_cb translate_cb;
	/* [한국어] 데이터 이동 없이 src 도메인의 주소를 dst 도메인 표현으로 번역.
	 * 설정자: spdk_memory_domain_set_translation(). 읽는 자: translate_data.
	 * 핵심: RDMA 영역에 등록된 host 버퍼라면 lkey/rkey만 반환하여 NIC가 직접
	 *   DMA 가능하게 함 — true zero-copy의 기반. */

	spdk_memory_domain_invalidate_data_cb invalidate_cb;
	/* [한국어] translate 결과(lkey 캐시 등)에 대한 invalidate 콜백 (선택).
	 * 설정자: spdk_memory_domain_set_invalidate(). 읽는 자: invalidate_data
	 *   — NULL이어도 에러 아님(void 반환), silently no-op. */

	spdk_memory_domain_memzero_cb memzero_cb;
	/* [한국어] 도메인 내 영역을 0으로 채우는 콜백 (예: NVMe write zeroes
	 *   대체용 비동기 zero fill).
	 * 설정자: set_memzero. 읽는 자: spdk_memory_domain_memzero(). */

	TAILQ_ENTRY(spdk_memory_domain) link;
	/* [한국어] g_dma_memory_domains TAILQ 링크. 보호: g_dma_mutex. */

	struct spdk_memory_domain_ctx *ctx;
	/* [한국어] create 시 호출자가 건넨 ctx의 deep copy. 본 파일이 calloc/free
	 *   를 책임진다. ctx->user_ctx 포인터는 user_ctx[] flexible array에 별도
	 *   복사된 후 ctx 안에서는 의미가 없어진다 (consumer는 get_user_context로
	 *   가져가야 함). NULL일 수 있음(create 시 ctx 인자가 NULL이었던 경우). */

	char *id;
	/* [한국어] 도메인 식별 문자열 (예: "system", "rdma-mlx5_0_pd0"). create의
	 *   id 인자를 strdup. NULL 허용 — 그 경우 id 기반 lookup에 매칭되지 않는다.
	 *   destroy에서 free. */

	size_t user_ctx_size;
	/* [한국어] user_ctx[]의 유효 바이트 수. ctx->user_ctx_size에서 복사.
	 *   0이면 user_ctx[]가 비어있음을 의미하고 get_user_context는 NULL 반환. */

	uint8_t user_ctx[];
	/* [한국어] flexible array — 호출자별 부가 컨텍스트 저장용 가변 영역.
	 *   예: RDMA의 경우 struct spdk_memory_domain_rdma_ctx(ibv_pd 포함)를 보관.
	 *   설정자: create가 ctx->user_ctx에서 memcpy. 읽는 자: get_user_context. */
};

/* [한국어] 모든 SPDK 프로세스에서 항상 존재하는 기본 도메인.
 * type=DMA(plain DMA-able host memory), id="system". 콜백을 하나도 갖지
 * 않으므로 데이터 op는 -ENOTSUP — 호출자는 system_domain을 만나면 일반 host
 * 버퍼로 가정하고 별도 처리(memcpy, libc allocator 등)를 한다. */
static struct spdk_memory_domain g_system_domain = {
	.type = SPDK_DMA_DEVICE_TYPE_DMA,    /* [한국어] 평범한 DMA-able host RAM */
	.id = "system",                       /* [한국어] 식별자 — 호출자가 get_first("system")로 찾을 수 있음 */
};

/*
 * [한국어]
 * _memory_domain_register - constructor: 시스템 도메인을 전역 TAILQ 첫 원소로 등록
 *
 * @return: void.
 *
 * 왜 필요한가: SPDK 라이브러리는 별도의 init 함수를 두지 않고도 dma 모듈이 사용
 *   가능해야 한다(다른 라이브러리/모듈이 자기 init에서 spdk_memory_domain_*
 *   호출 가능). __attribute__((constructor))로 main() 이전에 호출되도록 하여
 *   사용자 코드 시점에는 이미 system_domain이 TAILQ에 들어 있게 한다.
 *
 * 동시성: 프로세스 시작 single-thread 시점이라 락 불필요 (어차피 mutex는 static
 *   init이라 사용 가능하지만 race 자체가 없음).
 *
 * 호출 체인: dynamic loader (ELF .init_array) → [본 함수].
 */
static void
__attribute__((constructor))                  /* [한국어] main() 전에 자동 실행되는 ELF constructor */
_memory_domain_register(void)
{
	/* [한국어] system 도메인을 등록부 헤드로 삽입. 향후 모든 동적 도메인은
	 * 이 뒤에 INSERT_TAIL되므로 system은 항상 첫 원소가 보장된다. */
	TAILQ_INSERT_TAIL(&g_dma_memory_domains, &g_system_domain, link);
}

/*
 * [한국어]
 * spdk_memory_domain_get_system_domain - 시스템(host RAM) 도메인 sentinel 반환
 *
 * @return: 항상 유효한 g_system_domain 포인터 (정적 객체이므로 free 금지).
 *
 * 사용처: 호출자가 "그냥 호스트 메모리"임을 표현해야 하는 API에 넘길 때
 *   (예: bdev_ext_io_opts.memory_domain = system_domain).
 */
struct spdk_memory_domain *
spdk_memory_domain_get_system_domain(void)
{
	return &g_system_domain;       /* [한국어] 정적 sentinel — 락/refcnt 불필요 */
}

/*
 * [한국어]
 * spdk_memory_domain_create - 새 메모리 도메인을 생성하고 전역 등록부에 삽입
 *
 * @_domain: 출력 — 생성된 도메인 포인터를 저장할 위치. NULL이면 -EINVAL.
 * @type: 도메인이 표현하는 DMA 디바이스 종류 (RDMA/DMA/...).
 * @ctx: 호출자가 도메인에 부착하고 싶은 컨텍스트(ABI-compatible struct).
 *       NULL 허용. ctx->size로 ABI 호환성 negotiation을 지원하며, ctx->user_ctx
 *       포인터를 통해 추가 가변 길이 컨텍스트도 함께 복사된다.
 *       핵심 ABI 규약:
 *         - ctx->size == 0 이면 잘못된 호출(-EINVAL).
 *         - ctx->size가 sizeof(*ctx)보다 작아도 호환 가능 — 본 파일이 spdk_min
 *           으로 잘라 복사하고, 이후 사용 시에도 그 크기까지만 유효로 간주.
 *         - ctx->size가 user_ctx_size 필드까지 포함할 만큼 크고 ctx->user_ctx가
 *           NULL이 아닐 때만 user_ctx 영역을 복사.
 * @id: 도메인 식별 문자열. NULL 허용. NULL 아니면 strdup하여 보관.
 *
 * @return: 0 성공, 음수 errno (-EINVAL/-ENOMEM).
 *
 * 왜 필요한가: 트랜스포트/엑셀 모듈이 자기 메모리 위치를 SPDK에 등록하여,
 *   bdev_io 처리 중 호출자가 "이 버퍼는 RDMA PD #X에 등록되어 있다"라고 명시할
 *   수 있도록 함. 그래야 transport가 translate로 zero-copy 가능 여부를 결정.
 *
 * 동작:
 *   1) 입력 검증 (output ptr, ctx size).
 *   2) user_ctx_size 계산 — ABI forward-compat.
 *   3) 도메인 헤더 + flexible user_ctx[] 한 번에 calloc.
 *   4) id를 strdup (실패 시 cleanup).
 *   5) ctx를 별도 calloc하여 deep copy (ctx_size는 truncated).
 *   6) user_ctx[]에 user_ctx 복사.
 *   7) g_dma_mutex 잡고 TAILQ INSERT_TAIL.
 *   8) 출력 포인터에 저장하고 0 반환.
 *
 * 에러 경로: 단계별 cleanup — id strdup 실패 → free(domain). ctx alloc 실패 →
 *   free(id), free(domain). 그 외에는 단계별 free 가능.
 *
 * 호출 체인: 트랜스포트 init (예: nvme_rdma_ctrlr_construct) → [본 함수].
 *
 * 실행 컨텍스트: 보통 init 단계 main thread. 다만 g_dma_mutex 보호 덕분에
 *   동시 호출 자체는 안전.
 */
int
spdk_memory_domain_create(struct spdk_memory_domain **_domain, enum spdk_dma_device_type type,
			  struct spdk_memory_domain_ctx *ctx, const char *id)
{
	struct spdk_memory_domain *domain;       /* [한국어] 새로 만들 도메인 객체 */
	size_t ctx_size, user_ctx_size = 0;       /* [한국어] ctx 자체 복사 크기 / user_ctx 복사 크기 */

	if (!_domain) {                            /* [한국어] 출력 포인터 누락 — 도메인을 어디로 돌려줄지 불분명 */
		return -EINVAL;
	}

	if (ctx) {                                 /* [한국어] ctx가 있으면 ABI 검증 */
		if (ctx->size == 0) {              /* [한국어] size 0은 의미상 잘못된 ctx — 거부 */
			SPDK_ERRLOG("Context size can't be 0\n");
			return -EINVAL;
		}
		/* [한국어] ABI forward-compat: ctx의 user_ctx 필드와 user_ctx_size 필드가
		 * 모두 포함될 만큼 ctx->size가 크면, user_ctx 가변 영역을 같이 복사한다.
		 * offsetof + sizeof로 user_ctx_size 필드 끝 오프셋까지 ctx->size가
		 * 도달했는지 검사 — 이는 SPDK ABI compat 규약의 표준 패턴이다. */
		if (ctx->user_ctx &&
		    offsetof(struct spdk_memory_domain_ctx, user_ctx_size) + sizeof(ctx->user_ctx_size) <= ctx->size) {
			user_ctx_size = ctx->user_ctx_size;
		}
	}

	/* [한국어] 도메인 헤더와 user_ctx[] flexible array를 한 번에 할당 →
	 * 캐시 locality 향상 + free 1회로 끝남. calloc이라 모든 콜백/ctx 포인터는 0. */
	domain = calloc(1, sizeof(*domain) + user_ctx_size);
	if (!domain) {                             /* [한국어] OOM — 도메인 자체 할당 실패 */
		SPDK_ERRLOG("Failed to allocate memory");
		return -ENOMEM;
	}

	if (id) {                                  /* [한국어] id 문자열 보존 (선택) */
		domain->id = strdup(id);
		if (!domain->id) {                /* [한국어] strdup 실패 → 도메인 free 후 실패 반환 */
			SPDK_ERRLOG("Failed to allocate memory");
			free(domain);
			return -ENOMEM;
		}
	}

	if (ctx) {                                 /* [한국어] ctx 본체 deep copy */
		domain->ctx = calloc(1, sizeof(*domain->ctx));
		if (!domain->ctx) {               /* [한국어] OOM → id, domain 모두 cleanup */
			SPDK_ERRLOG("Failed to allocate memory");
			free(domain->id);
			free(domain);
			return -ENOMEM;
		}

		/* [한국어] ABI safety: 호출자가 더 큰 구조체를 줬더라도 본 파일이 아는
		 * 크기까지만 복사. 호출자가 더 작은 구조체를 줬다면 그만큼만 복사 →
		 * forward/backward 모두 호환. */
		ctx_size = spdk_min(sizeof(*domain->ctx), ctx->size);
		memcpy(domain->ctx, ctx, ctx_size);
		domain->ctx->size = ctx_size;     /* [한국어] 복사된 실제 크기로 size 갱신 — 후속 reader 일관성 */
	}

	if (user_ctx_size) {                       /* [한국어] user_ctx 가변 영역 복사 */
		assert(ctx);                       /* [한국어] user_ctx_size>0이면 ctx도 있어야 함 (논리 invariants) */
		memcpy(domain->user_ctx, ctx->user_ctx, user_ctx_size);
		domain->user_ctx_size = user_ctx_size;
	}

	domain->type = type;                       /* [한국어] 도메인 종류 기록 — 이후 변경 금지 */

	/* [한국어] 전역 등록부에 삽입. 락 잡는 이유: 다른 thread에서 동시 create/
	 * destroy/get_first/get_next가 일어나도 list 일관성 유지. */
	pthread_mutex_lock(&g_dma_mutex);
	TAILQ_INSERT_TAIL(&g_dma_memory_domains, domain, link);
	pthread_mutex_unlock(&g_dma_mutex);

	*_domain = domain;                         /* [한국어] 호출자에게 도메인 핸들 반환 */

	return 0;
}

/*
 * [한국어]
 * spdk_memory_domain_set_translation - translate 콜백을 도메인에 등록
 *
 * @domain: 대상 도메인. NULL 불가 (assert).
 * @translate_cb: src 도메인 주소를 dst 도메인 표현으로 번역하는 콜백.
 *                NULL을 넘기면 콜백 제거(이후 translate_data는 -ENOTSUP).
 *
 * 호출 시점: 도메인 owner가 spdk_memory_domain_create 직후 init 단계에서 호출.
 *   런타임에 변경하지 않을 것을 가정 — 락 없음.
 *
 * 호출 체인: nvme_rdma 등 transport init → [본 함수].
 */
void
spdk_memory_domain_set_translation(struct spdk_memory_domain *domain,
				   spdk_memory_domain_translate_memory_cb translate_cb)
{
	assert(domain);                  /* [한국어] domain NULL은 프로그래밍 버그 */

	domain->translate_cb = translate_cb;  /* [한국어] 콜백 슬롯에 등록 — atomic write이라 race 영향 없음 */
}

/*
 * [한국어]
 * spdk_memory_domain_set_invalidate - invalidate 콜백 등록
 *
 * @domain: 대상 도메인.
 * @invalidate_cb: translate가 발급한 lkey/rkey 등을 invalidate할 때 호출.
 *
 * 사용 맥락: NIC가 lkey 캐시를 가질 수 있는데 메모리 매핑이 바뀌면 owner가
 *   invalidate를 발행해야 한다. RDMA UMR/MR re-registration 등.
 */
void
spdk_memory_domain_set_invalidate(struct spdk_memory_domain *domain,
				  spdk_memory_domain_invalidate_data_cb invalidate_cb)
{
	assert(domain);

	domain->invalidate_cb = invalidate_cb;
}

/*
 * [한국어]
 * spdk_memory_domain_set_pull - pull 콜백 등록 (src→host 비동기 복사)
 *
 * 사용 예: GPU 메모리에 있는 데이터를 호스트 RAM으로 끌어와야 할 때 owner가
 *   GPU API 기반(예: cudaMemcpyAsync)으로 구현한 콜백을 등록.
 */
void
spdk_memory_domain_set_pull(struct spdk_memory_domain *domain,
			    spdk_memory_domain_pull_data_cb pull_cb)
{
	assert(domain);

	domain->pull_cb = pull_cb;
}

/*
 * [한국어]
 * spdk_memory_domain_set_push - push 콜백 등록 (host→dst 비동기 복사)
 */
void
spdk_memory_domain_set_push(struct spdk_memory_domain *domain,
			    spdk_memory_domain_push_data_cb push_cb)
{
	assert(domain);

	domain->push_cb = push_cb;
}

/*
 * [한국어]
 * spdk_memory_domain_set_data_transfer - transfer 콜백 등록 (src→dst 직접 전송)
 *
 * 사용 예: GPU↔NVMe peer-to-peer DMA, GPU↔GPU NVLink copy 등 호스트 RAM을
 *   거치지 않고 바로 옮길 때.
 */
void
spdk_memory_domain_set_data_transfer(struct spdk_memory_domain *domain,
				     spdk_memory_domain_transfer_data_cb transfer_cb)
{
	assert(domain);

	domain->transfer_cb = transfer_cb;
}

/*
 * [한국어]
 * spdk_memory_domain_set_memzero - memzero 콜백 등록 (도메인 내 영역 0으로 채움)
 */
void
spdk_memory_domain_set_memzero(struct spdk_memory_domain *domain,
			       spdk_memory_domain_memzero_cb memzero_cb)
{
	assert(domain);

	domain->memzero_cb = memzero_cb;
}

/*
 * [한국어]
 * spdk_memory_domain_get_context - 등록된 ctx 사본을 반환
 *
 * @return: 내부 ctx 포인터 (NULL 가능 — create 시 ctx가 NULL이었거나
 *          호출자가 부여하지 않은 도메인). 호출자는 free 금지.
 */
struct spdk_memory_domain_ctx *
spdk_memory_domain_get_context(struct spdk_memory_domain *domain)
{
	assert(domain);

	return domain->ctx;       /* [한국어] 내부 사본 그대로 노출 — 변경 금지 (관례) */
}

/*
 * [한국어]
 * spdk_memory_domain_get_user_context - flexible user_ctx[] 영역 노출
 *
 * @domain: 대상 도메인.
 * @ctx_size: 출력 — user_ctx의 유효 바이트 수.
 *
 * @return: user_ctx 시작 포인터. user_ctx_size==0이면 NULL 반환 (이때 ctx_size는
 *          쓰여지지 않으므로 호출자가 우선 NULL 검사를 해야 함).
 *
 * 사용 예: RDMA transport가 spdk_memory_domain_rdma_ctx로 캐스팅하여 ibv_pd
 *   포인터 등을 꺼냄.
 */
void *
spdk_memory_domain_get_user_context(struct spdk_memory_domain *domain, size_t *ctx_size)
{
	assert(domain);

	if (!domain->user_ctx_size) {     /* [한국어] user_ctx 미보유 — NULL 반환, ctx_size는 미설정 */
		return NULL;
	}

	*ctx_size = domain->user_ctx_size; /* [한국어] 호출자에게 유효 바이트 수 알림 */
	return domain->user_ctx;           /* [한국어] flexible array 포인터 반환 — owner는 free 금지 */
}

/* [한국어] astyle 포맷터 회피용 typedef — 함수 시그니처에 enum 키워드를 노출하면
 * astyle가 잘못 정렬하는 이슈 때문에 typedef 형태로 우회한다(원본 주석). */
/* We have to use the typedef in the function declaration to appease astyle. */
typedef enum spdk_dma_device_type spdk_dma_device_type_t;

/*
 * [한국어]
 * spdk_memory_domain_get_dma_device_type - 도메인의 DMA 디바이스 종류 반환
 *
 * 사용처: 호출자가 src/dst 도메인의 호환성을 체크할 때 (예: 둘 다 RDMA여도
 *   같은 PD가 아닐 수 있으므로 추가 검증 필요).
 */
spdk_dma_device_type_t
spdk_memory_domain_get_dma_device_type(struct spdk_memory_domain *domain)
{
	assert(domain);

	return domain->type;
}

/*
 * [한국어]
 * spdk_memory_domain_get_dma_device_id - 도메인 식별 문자열 반환
 *
 * @return: create 시 strdup된 id 또는 NULL (id 없는 도메인). 호출자는 free 금지.
 */
const char *
spdk_memory_domain_get_dma_device_id(struct spdk_memory_domain *domain)
{
	assert(domain);

	return domain->id;
}

/*
 * [한국어]
 * spdk_memory_domain_destroy - 동적 도메인 lifecycle 종료
 *
 * @domain: 파괴할 도메인. NULL이면 no-op (방어적 처리). g_system_domain은
 *          파괴 금지(assert) — 정적 객체이며 다른 모듈이 항상 의존하기 때문.
 *
 * 동작:
 *   1) 전역 TAILQ에서 제거 (g_dma_mutex 보호).
 *   2) ctx, id, domain 메모리 free.
 *
 * 주의: 호출자는 본 도메인을 사용 중인 모든 I/O가 완료된 후 파괴해야 한다 —
 *   본 파일은 ref count를 두지 않으므로 use-after-free 책임은 호출자에게 있다.
 *
 * 호출 체인: transport teardown → [본 함수].
 */
void
spdk_memory_domain_destroy(struct spdk_memory_domain *domain)
{
	if (!domain) {                       /* [한국어] NULL은 silently 무시 — fini 시 단순화 위해 */
		return;
	}

	assert(domain != &g_system_domain);  /* [한국어] system 도메인 destroy는 프로그래밍 버그 */

	pthread_mutex_lock(&g_dma_mutex);
	TAILQ_REMOVE(&g_dma_memory_domains, domain, link);  /* [한국어] 전역 등록부에서 제거 */
	pthread_mutex_unlock(&g_dma_mutex);

	free(domain->ctx);                   /* [한국어] create 시 calloc된 ctx 사본 해제 */
	free(domain->id);                    /* [한국어] strdup된 id 해제 */
	free(domain);                        /* [한국어] 도메인 헤더 + user_ctx[] 한꺼번에 해제 (단일 calloc이었음) */
}

/*
 * [한국어]
 * spdk_memory_domain_pull_data - src_domain에서 호스트 메모리(dst_iov)로 비동기 복사
 *
 * @src_domain: 데이터가 위치한 도메인 (예: GPU 도메인).
 * @src_domain_ctx: 도메인 owner가 정의한 per-call ctx (예: GPU stream).
 * @src_iov / @src_iov_cnt: src 도메인 내 데이터 위치(가상 주소 + 길이).
 * @dst_iov / @dst_iov_cnt: 호스트 측 수신 버퍼 (호스트 가상 주소).
 * @cpl_cb / @cpl_cb_arg: 비동기 완료 시 호출될 콜백/인자.
 *
 * @return: 0 성공(완료는 cpl_cb로), -ENOTSUP(콜백 미등록), 기타 음수 errno.
 *
 * 호출 체인: bdev/transport → [본 함수] → owner의 pull_cb (예: cudaMemcpyAsync).
 */
int
spdk_memory_domain_pull_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			     struct iovec *src_iov, uint32_t src_iov_cnt, struct iovec *dst_iov, uint32_t dst_iov_cnt,
			     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	assert(src_domain);                  /* [한국어] 필수 입력 검증 */
	assert(src_iov);
	assert(dst_iov);

	if (spdk_unlikely(!src_domain->pull_cb)) {  /* [한국어] 콜백 미등록 — 도메인이 pull을 지원하지 않음 */
		return -ENOTSUP;
	}

	/* [한국어] 도메인 owner 콜백으로 위임 — 본 dispatcher는 인자만 그대로 전달.
	 * 콜백이 비동기로 시작되었음(0)을 반환하면 cpl_cb가 나중에 호출된다. */
	return src_domain->pull_cb(src_domain, src_domain_ctx, src_iov, src_iov_cnt, dst_iov, dst_iov_cnt,
				   cpl_cb, cpl_cb_arg);
}

/*
 * [한국어]
 * spdk_memory_domain_push_data - 호스트 메모리(src_iov) → dst_domain으로 비동기 복사
 *
 * pull_data의 반대 방향. dst_domain이 콜백을 갖고 있어야 하므로 dst 측 도메인에
 * 콜백이 등록되어 있는지 확인한다 (push 콜백은 dst_domain의 책임).
 */
int
spdk_memory_domain_push_data(struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			     struct iovec *dst_iov, uint32_t dst_iovcnt, struct iovec *src_iov, uint32_t src_iovcnt,
			     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	assert(dst_domain);
	assert(dst_iov);
	assert(src_iov);

	if (spdk_unlikely(!dst_domain->push_cb)) {  /* [한국어] dst 도메인이 push를 모름 */
		return -ENOTSUP;
	}

	return dst_domain->push_cb(dst_domain, dst_domain_ctx, dst_iov, dst_iovcnt, src_iov, src_iovcnt,
				   cpl_cb, cpl_cb_arg);
}

/*
 * [한국어]
 * spdk_memory_domain_transfer_data - src_domain → dst_domain 직접 전송 (zero-copy 가능)
 *
 * @src_translation: 호출 전에 translate를 한 번 수행한 결과를 hint로 전달 가능.
 *                   transport가 lkey/rkey를 미리 알고 있으면 dst 도메인이
 *                   재변환 없이 사용할 수 있어 latency를 줄인다. NULL이어도 OK.
 *
 * dst_domain의 transfer_cb를 호출 — 즉 dst가 "내가 이 src로부터 직접 받을 수
 * 있는지" 판단하고 가능하면 P2P DMA, 불가하면 호스트 경유로 fallback.
 */
int
spdk_memory_domain_transfer_data(struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				 struct iovec *dst_iov, uint32_t dst_iovcnt,
				 struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				 struct iovec *src_iov, uint32_t src_iovcnt,
				 struct spdk_memory_domain_translation_result *src_translation,
				 spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	assert(dst_domain);
	assert(dst_iov);
	assert(src_iov);

	if (spdk_unlikely(!dst_domain->transfer_cb)) {  /* [한국어] dst가 직접 전송 미지원 → 호출자가 pull/push로 fallback해야 함 */
		return -ENOTSUP;
	}

	/* [한국어] dst 도메인 콜백에 src 정보 + 선택적 translation 결과를 전달.
	 * 콜백 내부에서 P2P 가능성 판단 후 적절한 backend(RDMA WR with src lkey,
	 * GPU peer copy 등)를 선택한다. */
	return dst_domain->transfer_cb(dst_domain, dst_domain_ctx, dst_iov, dst_iovcnt, src_domain,
				       src_domain_ctx, src_iov, src_iovcnt,
				       src_translation, cpl_cb, cpl_cb_arg);
}

/*
 * [한국어]
 * spdk_memory_domain_translate_data - 데이터 이동 없이 src→dst 도메인 주소 표현 변환
 *
 * @src_domain: 원본 도메인 — translate_cb를 가진 쪽. src의 콜백을 호출한다.
 * @src_domain_ctx: per-call ctx (예: 사용 중인 RDMA QP).
 * @dst_domain: 변환된 표현이 사용될 대상 도메인 (예: 같은 PD의 다른 QP, 또는
 *              system 도메인). src->translate_cb가 dst를 보고 적절한 형식
 *              (lkey/rkey/IOVA 등)을 결정.
 * @dst_domain_ctx: dst 측 ancillary (예: ibv_qp). spdk/dma.h의
 *                  spdk_memory_domain_translation_ctx 참조.
 * @addr/@len: 번역할 src 도메인 주소/길이.
 * @result: 출력 — translate된 iov + iov_count + lkey/rkey/...
 *
 * @return: 0 성공, -ENOTSUP, 기타 음수.
 *
 * 핵심: 이 함수가 zero-copy의 진입점이다. transport가 src→dst의 호환을 확인
 *   하고 translate_cb로 매핑 정보를 얻으면, 추가 메모리 이동 없이 바로 DMA
 *   요청을 NIC/SSD에 발행할 수 있다.
 */
int
spdk_memory_domain_translate_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				  struct spdk_memory_domain *dst_domain, struct spdk_memory_domain_translation_ctx *dst_domain_ctx,
				  void *addr, size_t len, struct spdk_memory_domain_translation_result *result)
{
	assert(src_domain);
	assert(dst_domain);
	assert(result);

	if (spdk_unlikely(!src_domain->translate_cb)) {  /* [한국어] src 도메인이 translate 미지원 → 호출자가 pull/push로 fallback */
		return -ENOTSUP;
	}

	return src_domain->translate_cb(src_domain, src_domain_ctx, dst_domain, dst_domain_ctx, addr, len,
					result);
}

/*
 * [한국어]
 * spdk_memory_domain_invalidate_data - 이전 translate 결과의 캐시 무효화
 *
 * 콜백이 NULL이어도 silent no-op (void 반환). 일부 transport는 lkey 캐싱을
 *   하지 않으므로 굳이 callback을 둘 필요가 없다.
 */
void
spdk_memory_domain_invalidate_data(struct spdk_memory_domain *domain, void *domain_ctx,
				   struct iovec *iov, uint32_t iovcnt)
{
	assert(domain);

	if (spdk_unlikely(!domain->invalidate_cb)) {  /* [한국어] invalidate 미지원 — silently 종료 */
		return;
	}

	domain->invalidate_cb(domain, domain_ctx, iov, iovcnt);
}

/*
 * [한국어]
 * spdk_memory_domain_memzero - 도메인 내 영역을 0으로 비동기 채움
 *
 * 사용처: bdev write zeroes 처리 시, 디바이스가 직접 zero를 만들 수 없는
 *   상황에서 도메인이 자체적으로 0을 채우게 한다 (예: GPU의 cuMemsetAsync).
 */
int
spdk_memory_domain_memzero(struct spdk_memory_domain *domain, void *domain_ctx, struct iovec *iov,
			   uint32_t iovcnt, spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	assert(domain);
	assert(iov);
	assert(iovcnt);              /* [한국어] iov 0개는 호출 자체가 의미 없음 */

	if (spdk_unlikely(!domain->memzero_cb)) {  /* [한국어] memzero 미지원 → 호출자가 호스트 zero 후 push로 fallback 가능 */
		return -ENOTSUP;
	}

	return domain->memzero_cb(domain, domain_ctx, iov, iovcnt, cpl_cb, cpl_cb_arg);
}

/*
 * [한국어]
 * spdk_memory_domain_get_first - id 필터로 등록부의 첫 번째 도메인 검색
 *
 * @id: 검색할 도메인 id 문자열. NULL이면 등록부 헤드(g_system_domain) 반환.
 * @return: 매칭된 도메인 또는 NULL (id가 같은 도메인이 없을 때).
 *
 * 사용처: 호출자가 도메인 enumeration을 시작할 때. _get_next와 짝을 이뤄
 *   for-loop처럼 사용:
 *     d = get_first(id); while (d) { ...; d = get_next(d, id); }
 *
 * 동시성: g_dma_mutex 보호하에 TAILQ를 읽음. 단, 반환된 포인터는 락이 풀린
 *   후에 사용되므로, 호출자는 도메인 destroy와의 race를 다른 방법으로
 *   방지해야 한다 (보통 enumeration 동안 destroy가 일어나지 않음을 가정).
 *
 * 주의: domain->id가 NULL인 도메인이 있으면 strcmp(NULL, id)는 SIGSEGV. 본
 *   파일은 id NULL 도메인 등록을 막지 않으므로(create에서 id 인자 NULL 허용),
 *   호출자는 가급적 모든 도메인에 id를 부여해야 한다.
 */
struct spdk_memory_domain *
spdk_memory_domain_get_first(const char *id)
{
	struct spdk_memory_domain *domain;

	if (!id) {                                /* [한국어] 필터 미지정 — 첫 원소 즉시 반환 */
		pthread_mutex_lock(&g_dma_mutex);
		domain = TAILQ_FIRST(&g_dma_memory_domains);  /* [한국어] system 도메인이 항상 첫 원소 */
		pthread_mutex_unlock(&g_dma_mutex);

		return domain;
	}

	pthread_mutex_lock(&g_dma_mutex);
	TAILQ_FOREACH(domain, &g_dma_memory_domains, link) {  /* [한국어] 처음부터 순회 */
		if (!strcmp(domain->id, id)) {                /* [한국어] id 일치 시 break (NULL id는 SIGSEGV 위험) */
			break;
		}
	}
	pthread_mutex_unlock(&g_dma_mutex);

	return domain;                            /* [한국어] match 없으면 TAILQ_FOREACH가 NULL로 끝남 */
}

/*
 * [한국어]
 * spdk_memory_domain_get_next - get_first/get_next 루프의 다음 도메인
 *
 * @prev: 직전에 받은 도메인. NULL이면 NULL 반환 (호출자가 검사하기 편하도록).
 * @id: 필터. NULL이면 단순히 prev 다음 원소 반환.
 *
 * @return: 다음 매칭 도메인 또는 NULL (더 이상 없음).
 *
 * 동작:
 *   1) TAILQ_NEXT(prev, link)로 다음 원소를 얻는다.
 *   2) id가 없거나 다음이 NULL이면 그대로 반환.
 *   3) 그렇지 않으면 TAILQ_FOREACH_FROM으로 다음 매칭을 찾는다.
 *
 * 주의: 락을 두 번 따로 잡아 race 가능성이 있다 — 첫 락 후 두 번째 락 사이에
 *   누가 prev를 destroy하면 next 포인터가 dangling 될 수 있다. 호출자는
 *   enumeration 동안 destroy를 막아야 한다 (관용적 SPDK init/teardown 격리).
 */
struct spdk_memory_domain *
spdk_memory_domain_get_next(struct spdk_memory_domain *prev, const char *id)
{
	struct spdk_memory_domain *domain;

	if (!prev) {                              /* [한국어] prev NULL은 enumeration 종료 신호 */
		return NULL;
	}

	pthread_mutex_lock(&g_dma_mutex);
	domain = TAILQ_NEXT(prev, link);          /* [한국어] 다음 원소 (NULL 가능) */
	pthread_mutex_unlock(&g_dma_mutex);

	if (!id || !domain) {                     /* [한국어] 필터 없거나 끝났으면 그대로 반환 */
		return domain;
	}

	pthread_mutex_lock(&g_dma_mutex);
	/* [한국어] FOREACH_FROM은 domain부터 시작하여 끝까지 순회. 락이 잠시 풀린 사이
	 * 다른 thread가 list를 변경했을 가능성이 있지만, SPDK 관례상 enumeration
	 * 중에는 변경하지 않는다고 가정. */
	TAILQ_FOREACH_FROM(domain, &g_dma_memory_domains, link) {
		if (!strcmp(domain->id, id)) {    /* [한국어] id 일치 → 반환 */
			break;
		}
	}
	pthread_mutex_unlock(&g_dma_mutex);

	return domain;
}
