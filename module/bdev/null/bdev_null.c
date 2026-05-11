/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * [한국어 설명] SPDK Null bdev 모듈 구현 (bdev_null.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK bdev(블록 디바이스) 추상화 계층의 하위 구현체 중 하나인 "null bdev"를
 * 정의한다. null bdev는 실제 저장 매체나 메모리 백엔드를 가지지 않는 가짜 블록 디바이스로,
 * 모든 read/write 요청을 즉시(또는 다음 poller tick에) 성공으로 완료시킨다. 주된 용도는
 * (1) bdev 코어 및 상위 스택의 오버헤드만을 측정하는 마이크로 벤치마크, (2) 새로운 bdev
 * 기능을 테스트할 때 실제 디스크 의존성을 제거하기 위한 테스트 더블, (3) 더미 read 데이터를
 * 필요로 하는 상위 모듈의 저장소 역할이다. 따라서 본 파일에는 "0으로 채운 단일 read 버퍼를
 * 모든 read 요청이 공유"하고, write 요청은 단순히 큐에 넣었다가 다음 poller에서 완료시키는
 * 미니멀한 로직이 들어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택에서 본 파일의 위치는 다음과 같다.
 *   [Application or test harness]
 *      ↓ spdk_bdev_read/write/...
 *   [bdev core (lib/bdev/bdev.c)]   - 공통 추상화 계층, spdk_bdev_io 라이프사이클 관리
 *      ↓ fn_table->submit_request   - 모듈 콜백 호출 (이 파일의 bdev_null_submit_request)
 *   [bdev_null (이 파일)]            - 모든 I/O를 즉시 완료시키는 dummy 구현
 *      ↓ spdk_bdev_io_complete      - bdev core에 완료 통보
 *   [bdev core completion path]     - 사용자 콜백 호출
 * 본 파일은 bdev module(빌드 시 spdk_bdev_module 등록)로서 SPDK 부팅 단계의 subsystem
 * 초기화에서 spdk_bdev_module->module_init = bdev_null_initialize가 호출되어 글로벌 상태를
 * 준비하고, RPC `bdev_null_create`가 들어오면 bdev_null_create()가 새 null bdev 인스턴스를
 * 만든다. I/O 처리 코드는 SPDK reactor(코어 고정 polling 스레드) 위에서 실행되며, 모든
 * I/O 채널은 코어별로 분리되어 lockless 동작이 가능하다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/bdev/ (bdev core): spdk_bdev_module / spdk_bdev / spdk_bdev_fn_table /
 *   spdk_bdev_io 등의 핵심 자료구조와 등록/완료 API(spdk_bdev_register, spdk_bdev_io_complete,
 *   spdk_bdev_unregister_by_name, spdk_bdev_open_ext)를 사용한다. SPDK_BDEV_MODULE_REGISTER
 *   매크로로 자기 자신을 bdev core에 등록한다.
 * - lib/thread/ (reactor/poller): spdk_io_device_register / spdk_get_io_channel /
 *   SPDK_POLLER_REGISTER 등을 사용해 코어별 I/O 채널과 polling 콜백을 설정한다. null bdev의
 *   "지연 완료" 동작은 이 poller 메커니즘에 의존한다.
 * - lib/util/ (DIF): SPDK_DIF_TYPE1/2/3에 해당하는 데이터 무결성 필드(Data Integrity Field)
 *   생성/검증을 위해 spdk_dif_ctx_init, spdk_dif_generate, spdk_dif_verify를 호출한다. NVMe
 *   스펙의 End-to-End Data Protection(NVMe Base Spec §8.3) 시뮬레이션에 사용된다.
 * - lib/json/ (구성 직렬화): bdev_null_write_config_json이 SPDK config save 시 호출되어
 *   JSON-RPC 명령어 형태로 자신의 생성 옵션을 출력한다.
 * - module/bdev/null/bdev_null_rpc.c (외부 파일, 같은 디렉토리): RPC 핸들러는 별도 파일에
 *   있고, 이 파일의 bdev_null_create / bdev_null_delete / bdev_null_resize 공개 함수를
 *   호출한다.
 * 데이터 흐름: 상위 → bdev core → bdev_null_submit_request → ch->io 큐(코어 로컬) →
 * null_io_poll(다음 tick) → spdk_bdev_io_complete → 상위 콜백.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct null_bdev: SPDK bdev 메타데이터 + 글로벌 리스트 노드 (g_null_bdev_head 연결).
 * - struct null_bdev_io: bdev_io당 모듈 컨텍스트(driver_ctx). pending 큐 노드 하나만 가진다.
 * - struct null_io_channel: 코어별 I/O 컨텍스트. 큐(io)와 poller를 보유.
 * - bdev_null_submit_request(): bdev core가 호출하는 핵심 진입점. I/O type별 처리.
 * - null_io_poll(): poller 콜백. 큐에 쌓인 모든 I/O를 SUCCESS로 완료시킴.
 * - bdev_null_create(): RPC가 호출하는 인스턴스 생성 함수.
 * - bdev_null_initialize() / bdev_null_finish(): 모듈 라이프사이클 훅.
 */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 헤더 포함자(stdio/stdlib/string/errno 등 OS 표준 헤더를 한 번에 포함).
 * SPDK 코드 컨벤션상 모든 .c 파일이 가장 먼저 이 헤더를 포함해 플랫폼 호환성 처리를 위임한다. */

#include "spdk/bdev.h"
/* [한국어] bdev 공개 API 헤더. spdk_bdev / spdk_bdev_io_status / spdk_bdev_open_ext /
 * spdk_bdev_close / spdk_bdev_notify_blockcnt_change 등 외부 호출 API를 선언한다.
 * bdev_null_resize에서 다른 bdev처럼 자기 bdev를 열고 크기 변경을 통보하기 위해 사용. */

#include "spdk/env.h"
/* [한국어] SPDK 환경 추상화(DPDK 래퍼) 헤더. spdk_zmalloc/spdk_free(hugepage 메모리),
 * SPDK_ENV_NUMA_ID_ANY/SPDK_MALLOC_DMA 플래그 등을 선언한다. g_null_read_buf를 DMA 가능한
 * hugepage로 할당할 때 사용된다. */

#include "spdk/thread.h"
/* [한국어] spdk_thread/poller 헤더. spdk_io_device_register/unregister, spdk_get_io_channel,
 * SPDK_POLLER_REGISTER, spdk_poller_unregister, SPDK_POLLER_IDLE/BUSY 매크로를 제공한다.
 * 코어 로컬 I/O 채널과 poller 라이프사이클 관리에 사용된다. */

#include "spdk/json.h"
/* [한국어] JSON 직렬화 API. spdk_json_write_named_string/uint32/bool/uuid 등을 사용해
 * bdev_null_write_config_json에서 RPC save_config 출력을 만든다. */

#include "spdk/string.h"
/* [한국어] SPDK 문자열 유틸리티(예: spdk_strerror). 본 파일에서는 직접 사용은 적지만
 * 헤더 의존성 차원에서 포함된다. */

#include "spdk/likely.h"
/* [한국어] 분기 예측 힌트 매크로(spdk_likely / spdk_unlikely)를 제공. 핫패스(read 처리 분기)
 * 에서 큰 I/O 검사 분기 예측에 사용된다. */

#include "spdk/bdev_module.h"
/* [한국어] bdev 백엔드 모듈 작성용 내부 API. spdk_bdev_module 구조체, SPDK_BDEV_MODULE_REGISTER,
 * spdk_bdev_register, spdk_bdev_io_complete, spdk_bdev_io_from_ctx, spdk_bdev_module_fini_done
 * 등 모듈이 코어와 통신할 때 필요한 심볼들을 선언한다. */

#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG/SPDK_NOTICELOG/SPDK_LOG_REGISTER_COMPONENT 등 로그 매크로 제공.
 * 파일 마지막의 SPDK_LOG_REGISTER_COMPONENT(bdev_null) 매크로가 이 모듈 전용 로그 카테고리를
 * 등록한다. */

#include "bdev_null.h"
/* [한국어] 같은 디렉토리의 공개 인터페이스 헤더. struct null_bdev_opts, spdk_delete_null_complete
 * 콜백 타입, bdev_null_create/delete/resize 함수 프로토타입을 노출한다. RPC 핸들러
 * (bdev_null_rpc.c)가 이 헤더를 통해 본 파일의 함수를 호출한다. */

/*
 * [한국어] struct null_bdev_io
 *
 * bdev_io당 모듈 전용 컨텍스트. SPDK bdev 코어는 spdk_bdev_io를 할당할 때 모듈이 요청한
 * 추가 바이트(get_ctx_size 콜백 반환값)를 함께 할당해 spdk_bdev_io->driver_ctx에 매핑한다.
 * null bdev는 "다음 poller tick까지 보류된 I/O 연결 리스트"의 노드 역할만 필요로 하므로
 * TAILQ_ENTRY 단 하나만 가진다.
 *
 * 라이프사이클:
 *   bdev core가 spdk_bdev_io 할당 시 driver_ctx 영역으로 자동 포함 →
 *   bdev_null_submit_request에서 ch->io 리스트에 INSERT_TAIL →
 *   다음 null_io_poll() 호출에서 DEQUEUE 후 spdk_bdev_io_complete로 완료.
 */
struct null_bdev_io {
	TAILQ_ENTRY(null_bdev_io) link;
	/* [한국어] null_io_channel::io (코어 로컬 pending 큐)의 노드 포인터 쌍.
	 * 설정자: bdev_null_submit_request의 TAILQ_INSERT_TAIL 호출.
	 * 읽는 자: null_io_poll의 TAILQ_SWAP/REMOVE, bdev_null_abort_io의 TAILQ_FOREACH/REMOVE.
	 * 값 범위: 큐 안에 있을 때만 의미 있는 포인터 쌍(없으면 미정의).
	 * 동기화: 큐는 코어 로컬(채널당 하나)이며 같은 reactor 스레드에서만 접근하므로 lock 불필요. */
};

/*
 * [한국어] struct null_bdev
 *
 * SPDK bdev 한 인스턴스의 메타데이터를 담는 구조체. spdk_bdev를 첫 번째 멤버로 임베딩하여
 * spdk_bdev * → null_bdev * 변환 시 직접 캐스트가 가능하다(SPDK는 임베딩 패턴을 광범위하게 사용).
 * 또한 모든 null bdev 인스턴스를 모듈 글로벌 리스트(g_null_bdev_head)에 연결하는 노드를 가진다.
 */
struct null_bdev {
	struct spdk_bdev	bdev;
	/* [한국어] bdev 코어가 인식하는 공개 메타데이터. name, blocklen, blockcnt, uuid,
	 * dif_*, fn_table, module 등 spdk_bdev_register에 필요한 모든 필드가 들어있다.
	 * 설정자: bdev_null_create가 RPC 옵션으로부터 채운 뒤 spdk_bdev_register 호출.
	 * 읽는 자: bdev core, 다른 bdev 모듈, fn_table 콜백.
	 * 동기화: bdev core가 등록/해지 시점의 글로벌 락으로 보호. I/O 핫패스에서는 read-only. */

	TAILQ_ENTRY(null_bdev)	tailq;
	/* [한국어] g_null_bdev_head(이 모듈에서 만든 모든 null bdev의 글로벌 리스트) 노드.
	 * 설정자: bdev_null_create의 TAILQ_INSERT_TAIL.
	 * 읽는 자: bdev_null_destruct의 TAILQ_REMOVE.
	 * 값 범위: 등록된 동안에만 유효.
	 * 동기화: 단일 init/RPC 스레드(통상 main thread)에서만 갱신되므로 추가 락 없음. */
};

/*
 * [한국어] struct null_io_channel
 *
 * I/O 채널은 SPDK에서 "I/O를 처리하는 코어 로컬 컨텍스트"이다. spdk_io_device_register로
 * 등록된 io device(여기서는 g_null_bdev_head 주소)에 대해, 각 reactor 스레드가
 * spdk_get_io_channel을 호출하면 코어별로 한 번씩 ctx_buf(=null_io_channel)가 만들어진다.
 * 따라서 본 구조체는 명시적 락 없이 자기 코어 안에서만 자유롭게 다룰 수 있다.
 */
struct null_io_channel {
	struct spdk_poller		*poller;
	/* [한국어] 이 채널에 등록된 polling 콜백 핸들. SPDK_POLLER_REGISTER가 반환한다.
	 * 설정자: null_bdev_create_cb (채널 생성 시).
	 * 읽는 자: null_bdev_destroy_cb의 spdk_poller_unregister.
	 * 값 범위: 비-NULL 포인터 (poller 등록 실패 시 NULL일 수 있으나 본 코드는 검사 생략).
	 * 동기화: 채널과 동일한 reactor에서만 접근. */

	TAILQ_HEAD(, null_bdev_io)	io;
	/* [한국어] 보류 중인 null_bdev_io 리스트 헤드. submit_request에서 enqueue되고
	 * 다음 null_io_poll tick에서 한꺼번에 dequeue되어 SUCCESS로 완료 통보된다.
	 * 설정자: null_bdev_create_cb의 TAILQ_INIT, submit_request의 TAILQ_INSERT_TAIL,
	 *         null_io_poll의 TAILQ_SWAP, abort_io의 TAILQ_REMOVE.
	 * 읽는 자: 위와 동일.
	 * 값 범위: 비어 있을 수도 있고 임의 길이일 수도 있음.
	 * 동기화: 채널이 코어 로컬이므로 별도 동기화 없이 단일 스레드에서 안전. */
};

static TAILQ_HEAD(, null_bdev) g_null_bdev_head = TAILQ_HEAD_INITIALIZER(g_null_bdev_head);
/* [한국어] 모듈 글로벌: 등록된 모든 null bdev 인스턴스의 연결 리스트.
 * 추가로 spdk_io_device_register에 io device 키로도 사용되므로(주소 자체가 키), 모듈에 단일하게
 * 존재해야 한다. 등록/해지는 RPC/init/finish 경로에서만 일어나므로 사실상 단일 스레드 접근. */

static void *g_null_read_buf;
/* [한국어] 모듈 글로벌: 모든 null bdev 인스턴스가 read 응답으로 공유하는 0으로 채운 버퍼.
 * 상위가 read 호출 시 자체 버퍼를 제공하지 않으면(iov_base == NULL) 이 버퍼를 가리키도록
 * 설정한다. 크기는 SPDK_BDEV_LARGE_BUF_MAX_SIZE이며 spdk_zmalloc으로 hugepage(DMA 가능)에
 * 할당된다. bdev_null_initialize에서 할당, bdev_null_finish 경로에서 해제. */

static int bdev_null_initialize(void);
/* [한국어] 전방 선언: 모듈 init 콜백. spdk_bdev_module.module_init에서 참조되므로 정의보다
 * 앞서 등록 매크로에서 사용된다. */

static void bdev_null_finish(void);
/* [한국어] 전방 선언: 모듈 fini 콜백. async_fini=true이므로 작업이 끝나면
 * spdk_bdev_module_fini_done()을 직접 호출해야 한다. */

/*
 * [한국어]
 * bdev_null_get_ctx_size - bdev_io당 모듈 컨텍스트 크기 반환
 *
 * @return: bdev core가 spdk_bdev_io를 alloc할 때 driver_ctx 영역으로 추가 할당해야 할 바이트 수.
 *
 * SPDK bdev core는 모든 모듈의 get_ctx_size를 합산해 공통 spdk_bdev_io_pool의 객체 크기를
 * 결정한다(가장 큰 모듈 기준이지만, 이 콜백은 모듈별 사이즈 제공이 목적). 따라서 본 함수는
 * 모듈 등록 단계에 한 번만 호출된다. null 모듈은 driver_ctx로 struct null_bdev_io만 필요로
 * 하므로 그 크기를 반환한다.
 *
 * 호출 체인:
 *   spdk_bdev_initialize → spdk_bdev_modules_init → 각 module->get_ctx_size 호출 → [이 함수]
 */
static int
bdev_null_get_ctx_size(void)
{
	return sizeof(struct null_bdev_io);
	/* [한국어] driver_ctx에 저장될 모듈 전용 데이터 크기. null_bdev_io는 TAILQ_ENTRY 1개. */
}

/*
 * [한국어] null_if (정적 할당)
 *
 * SPDK bdev 코어에 자기 자신을 등록하기 위한 spdk_bdev_module 인스턴스. 등록 매크로
 * SPDK_BDEV_MODULE_REGISTER가 컴파일 타임 init array에 이 구조체 포인터를 넣어, 부팅 시
 * spdk_bdev_initialize가 모든 모듈을 순회하며 module_init을 호출하게 한다.
 *
 * 필드 의미:
 *   .name        : 로그/RPC에서 모듈 식별에 쓰이는 문자열.
 *   .module_init : 부팅 시 한 번 호출되는 비동기 초기화. 0 또는 -ERRNO 반환.
 *   .module_fini : 종료 시 호출되는 정리 콜백.
 *   .async_fini  : true이면 module_fini가 비동기로 끝나며, 작업 종료 시
 *                  spdk_bdev_module_fini_done()을 직접 호출해야 함을 코어에 알린다.
 *   .get_ctx_size: bdev_io당 driver_ctx 크기.
 */
static struct spdk_bdev_module null_if = {
	.name = "null",
	/* [한국어] 모듈명. RPC `bdev_null_create`의 product 식별자, log 컴포넌트 매칭 등에 사용. */
	.module_init = bdev_null_initialize,
	/* [한국어] 부팅 시 호출되는 모듈 init. 글로벌 read 버퍼와 io_device를 등록한다. */
	.module_fini = bdev_null_finish,
	/* [한국어] 종료 시 호출되는 모듈 fini. 비동기 cleanup이며 io_device unregister 후
	 * _bdev_null_finish_cb에서 read 버퍼 해제 및 fini_done 통보. */
	.async_fini = true,
	/* [한국어] true → module_fini가 즉시 종료되지 않을 수 있음. 코어는
	 * spdk_bdev_module_fini_done() 호출까지 다음 모듈로 넘어가지 않는다. */
	.get_ctx_size = bdev_null_get_ctx_size,
	/* [한국어] driver_ctx 크기 콜백. 위의 bdev_null_get_ctx_size 함수 참조. */
};

SPDK_BDEV_MODULE_REGISTER(null, &null_if)
/* [한국어] 매크로 확장: 컴파일 타임 init section에 &null_if를 등록한다. 결과적으로 SPDK가
 * 시동될 때 spdk_bdev_module_list에 null 모듈이 자동 합류해 module_init이 호출된다.
 * (lib/bdev/bdev_module.h 참조) */

/*
 * [한국어]
 * bdev_null_destruct - bdev 인스턴스 파괴 콜백 (fn_table.destruct)
 *
 * @ctx: spdk_bdev->ctxt에 저장된 null_bdev 포인터. bdev_null_create에서 설정.
 * @return: 0 = 동기 destruct 완료, 1 = async 진행 중(여기는 동기이므로 0).
 *
 * bdev core가 spdk_bdev_unregister 또는 spdk_bdev_unregister_by_name에서 마지막 참조가
 * 사라지는 시점에 호출한다. 글로벌 리스트에서 제거하고, name 문자열과 null_bdev 자체를 free.
 * spdk_bdev 객체 안의 다른 필드(예: uuid)는 메모리 자체가 null_bdev에 임베드되어 있으므로
 * 별도 해제가 필요 없다.
 *
 * 호출 컨텍스트: bdev core가 bdev unregister thread (보통 main thread)에서 호출.
 *
 * 호출 체인:
 *   bdev_null_delete → spdk_bdev_unregister_by_name → ...completion path... → [이 함수]
 */
static int
bdev_null_destruct(void *ctx)
{
	struct null_bdev *bdev = ctx;
	/* [한국어] void * → null_bdev * 캐스트. bdev_null_create에서 spdk_bdev->ctxt에 자기
	 * 자신을 저장해두었으므로 안전한 변환. (bdev core의 fn_table 콜백 호출 시 ctxt가 첫 인자) */

	TAILQ_REMOVE(&g_null_bdev_head, bdev, tailq);
	/* [한국어] 모듈 글로벌 리스트에서 자신 제거. 이후 다른 init/fini 경로가 이 인스턴스를
	 * 보지 못하도록 한다. 단일 스레드(보통 main thread)에서만 갱신되므로 락 불필요.
	 * 만약 이 단계 이후 fini가 와도 g_null_bdev_head에 남아 있지 않으므로 안전. */
	free(bdev->bdev.name);
	/* [한국어] bdev_null_create에서 strdup으로 복사해둔 name 문자열 해제. bdev 본체는
	 * null_bdev 안에 임베드되어 있지만 name만 별도 동적 할당이라 명시적 free 필요. */
	free(bdev);
	/* [한국어] null_bdev 구조체 자체 해제. (spdk_bdev 본체는 임베드되어 있으므로 같이 사라짐.
	 * 이후 이 포인터는 무효이므로 함수가 즉시 반환. ctxt가 NULL을 가리키게 정리하는 작업은
	 * bdev core 측에서 spdk_bdev 객체 자체를 더 이상 참조하지 않으므로 불필요.) */

	return 0;
	/* [한국어] 0 = destruct 동기 완료. 1을 반환하면 "async 진행 중"으로 처리되어 core가
	 * spdk_bdev_destruct_done()을 기다린다. null bdev는 매체가 없어 정리할 비동기 작업이
	 * 없으므로 항상 0(=sync 완료) 반환. 이후 core는 bdev_io pool에서 이 bdev 관련 자원 해제. */
}

/*
 * [한국어]
 * bdev_null_abort_io - 보류 큐에서 특정 bdev_io를 찾아 ABORTED로 완료시킴
 *
 * @ch: I/O 채널(코어 로컬). 이 채널의 io 큐에서만 검색한다.
 * @bio_to_abort: 사용자가 abort 요청한 spdk_bdev_io 포인터.
 * @return: true = 큐에서 찾아 abort 완료, false = 큐에 없음(이미 완료되었거나 다른 채널).
 *
 * SPDK_BDEV_IO_TYPE_ABORT 처리에서 사용된다. null bdev는 모든 I/O를 다음 poller tick까지
 * ch->io에 보관하므로, 그 사이 abort가 들어오면 이 함수가 큐에서 직접 빼서 ABORTED 상태로
 * 완료시킬 수 있다. 만약 이미 null_io_poll이 처리해 큐를 비웠다면 false를 반환하고,
 * 호출자(submit_request 분기)는 abort 자체를 FAILED로 보고한다.
 *
 * 호출 컨텍스트: 채널과 같은 reactor 스레드. 큐 조작 동안 lock 불필요(코어 로컬).
 *
 * 호출 체인:
 *   bdev_null_submit_request[case ABORT] → [이 함수] → spdk_bdev_io_complete(bio_to_abort)
 */
static bool
bdev_null_abort_io(struct null_io_channel *ch, struct spdk_bdev_io *bio_to_abort)
{
	struct null_bdev_io *null_io;
	/* [한국어] TAILQ_FOREACH 순회 변수. 큐에 들어 있는 보류 I/O 중 하나를 가리킨다. */
	struct spdk_bdev_io *bdev_io;
	/* [한국어] null_io를 driver_ctx로 가지는 spdk_bdev_io. spdk_bdev_io_from_ctx로 역산. */

	TAILQ_FOREACH(null_io, &ch->io, link) {
		/* [한국어] 채널의 보류 I/O 큐를 처음부터 끝까지 순회. */
		bdev_io = spdk_bdev_io_from_ctx(null_io);
		/* [한국어] driver_ctx 포인터(null_io)에서 spdk_bdev_io 역포인터를 얻는다.
		 * (lib/bdev: container_of 매크로 기반 — driver_ctx는 spdk_bdev_io의 가변 길이 끝부분) */

		if (bdev_io == bio_to_abort) {
			/* [한국어] 같은 bdev_io 포인터를 찾았다면, 이 요청을 abort 대상으로 간주. */
			TAILQ_REMOVE(&ch->io, null_io, link);
			/* [한국어] 큐에서 제거하여 다음 poller가 처리하지 않도록 한다. */
			spdk_bdev_io_complete(bio_to_abort, SPDK_BDEV_IO_STATUS_ABORTED);
			/* [한국어] bdev core에 ABORTED 상태로 완료 통보. 사용자 콜백(cb_fn)이
			 * 이후 같은 스레드에서 실행되어 abort 사실을 인지하게 된다. */
			return true;
			/* [한국어] 호출자가 이 abort를 SUCCESS로 보고하도록 true 반환. */
		}
	}

	return false;
	/* [한국어] 큐에 없음 → 이미 완료되었거나 다른 채널 소유. 호출자는 abort를 FAILED 처리. */
}

/*
 * [한국어]
 * bdev_null_submit_request - bdev core가 모든 I/O 요청을 위임하는 핵심 진입점
 *
 * @_ch: spdk_io_channel 핸들. spdk_io_channel_get_ctx로 null_io_channel을 얻는다.
 * @bdev_io: 처리할 I/O 요청. type, offset_blocks, num_blocks, iovs/iovcnt 등이 채워져 있음.
 *
 * 이 함수는 fn_table.submit_request 콜백으로 등록되어 있어서, 상위 spdk_bdev_read/write 등의
 * 호출이 lib/bdev 코어를 거쳐 결국 본 함수로 도달한다. 호출 시점에는 이미 bdev core가
 * "어떤 reactor에서 처리할지"를 결정해 해당 코어로 메시지를 보낸 상태이므로, 본 함수는
 * 그 코어 reactor 스레드 위에서 실행된다(lockless가 가능한 이유).
 *
 * 동작 단계:
 *   1) DIF가 활성화된 bdev이면 dif_ctx를 LBA 기반으로 초기화.
 *   2) READ: 사용자 버퍼가 없으면 g_null_read_buf로 가짜 응답 버퍼를 채워준다. DIF가 켜져
 *      있으면 그 위에 PI(Protection Information) 태그를 spdk_dif_generate로 만들어 둔다.
 *   3) WRITE: DIF가 켜져 있으면 사용자 데이터의 PI를 검증한다(스토리지 매체에 쓰진 않으니
 *      검증 단계만 수행). 통과하면 큐에 넣고 다음 poller에서 SUCCESS 통보.
 *   4) WRITE_ZEROES / RESET: 즉시 큐에 넣음. (실제 매체가 없으니 단순히 SUCCESS 큐잉)
 *   5) ABORT: bdev_null_abort_io를 호출. 큐에서 빼면 SUCCESS, 못 찾으면 FAILED.
 *   6) FLUSH / UNMAP / 기타: null bdev가 지원하지 않으므로 FAILED.
 *
 * 에러 경로: 어느 단계에서든 실패 시 spdk_bdev_io_complete(FAILED)로 즉시 완료. 보류 큐에는
 * 넣지 않으므로 abort 대상이 되지 않는다.
 *
 * 호출 체인:
 *   spdk_bdev_read/write/... → bdev core → fn_table->submit_request → [이 함수]
 *     → (성공) TAILQ_INSERT_TAIL(&ch->io) → 다음 null_io_poll에서 spdk_bdev_io_complete
 *     → (실패) 즉시 spdk_bdev_io_complete(FAILED)
 */
static void
bdev_null_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct null_bdev_io *null_io = (struct null_bdev_io *)bdev_io->driver_ctx;
	/* [한국어] driver_ctx 포인터를 모듈 컨텍스트 타입으로 캐스트. 같은 메모리이지만
	 * 타입을 회복해 TAILQ 매크로에 사용한다. */
	struct null_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	/* [한국어] spdk_io_channel 핸들에서 코어 로컬 ctx_buf(null_io_channel)를 얻는다.
	 * 이 함수가 실행되는 reactor와 동일한 채널이 보장된다(thread affinity). */
	struct spdk_bdev *bdev = bdev_io->bdev;
	/* [한국어] bdev 메타데이터(blocklen, md_len, dif_type 등)에 접근하기 위한 단축 포인터. */
	struct spdk_dif_ctx dif_ctx;
	/* [한국어] DIF(Data Integrity Field) 처리 컨텍스트. blocklen/md_len/PI 포맷 등을 묶어
	 * spdk_dif_generate/verify 호출에 전달. NVMe Base Spec §8.3 End-to-End Data Protection. */
	struct spdk_dif_error err_blk;
	/* [한국어] DIF verify 실패 시 어느 블록의 어떤 필드가 틀렸는지 기록되는 출력 구조체. */
	int rc;
	/* [한국어] 임시 반환값 버퍼. spdk_dif_* 호출 결과 0/음수 검사용. */
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	/* [한국어] dif_ctx_init 확장 옵션. dif_pi_format(16b/32b/64b CRC) 지정용. */

	if (SPDK_DIF_DISABLE != bdev->dif_type &&
	    (SPDK_BDEV_IO_TYPE_READ == bdev_io->type ||
	     SPDK_BDEV_IO_TYPE_WRITE == bdev_io->type)) {
		/* [한국어] DIF가 활성화된 bdev이면서 READ/WRITE 데이터 경로일 때만 dif_ctx를 준비.
		 * RESET/ABORT/WRITE_ZEROES 등은 PI 처리가 불필요하므로 건너뜀. */
		dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
		/* [한국어] 옵션 구조체의 ABI 크기를 명시 → 향후 필드 추가 시 호환성 유지용. */
		dif_opts.dif_pi_format = bdev->dif_pi_format;
		/* [한국어] bdev 생성 시 지정된 PI 포맷을 사용(16b CRC, 32b CRC, 64b CRC 등). */
		rc = spdk_dif_ctx_init(&dif_ctx,
				       bdev->blocklen,
				       /* [한국어] 데이터+메타데이터 합산 블록 크기. */
				       bdev->md_len,
				       /* [한국어] 메타데이터(=PI 포함) 길이. */
				       bdev->md_interleave,
				       /* [한국어] true → 메타가 데이터 블록과 인터리브, false → 별도 영역. */
				       bdev->dif_is_head_of_md,
				       /* [한국어] PI가 메타데이터의 앞쪽인지(true) 뒤쪽인지(false). */
				       bdev->dif_type,
				       /* [한국어] DIF 타입(1/2/3). 각 타입별 reftag 처리 방식이 다름. */
				       bdev_io->u.bdev.dif_check_flags,
				       /* [한국어] GUARD/REFTAG/APPTAG 검사 활성화 비트마스크. */
				       bdev_io->u.bdev.offset_blocks & 0xFFFFFFFF,
				       /* [한국어] 시작 LBA의 하위 32비트(reftag 초기값). */
				       0xFFFF, 0, 0, 0, &dif_opts);
				       /* [한국어] apptag_mask, apptag, reserved, offset, ext_opts. */
		if (0 != rc) {
			/* [한국어] DIF 컨텍스트 초기화 실패 → 매개변수 모순. I/O를 즉시 FAILED 처리. */
			SPDK_ERRLOG("Failed to initialize DIF context, error %d\n", rc);
			/* [한국어] 운영자에게 원인 알림(rc는 -EINVAL 등 음수 errno). */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			/* [한국어] bdev core에 실패 보고. 사용자 콜백 cb_fn이 이후 호출됨. */
			return;
			/* [한국어] 더 이상 진행하지 않음. */
		}
	}

	switch (bdev_io->type) {
	/* [한국어] I/O 종류별 분기. spdk_bdev_io_type 열거형 참조. */
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovs[0].iov_base == NULL) {
			/* [한국어] 사용자가 버퍼를 미리 제공하지 않은 경우. SPDK는 read 시 모듈이
			 * 자체 버퍼를 채워주는 시나리오를 허용한다. null bdev는 0으로 채운 공유
			 * 버퍼(g_null_read_buf)를 가리키게 함으로써 메모리 할당 비용을 0으로 만든다. */
			assert(bdev_io->u.bdev.iovcnt == 1);
			/* [한국어] 사전 할당이 없는 경우 iovec는 1개라는 SPDK 규약 검증. */
			if (spdk_likely(bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen <=
					SPDK_BDEV_LARGE_BUF_MAX_SIZE)) {
				/* [한국어] 요청 크기가 공유 버퍼 한도 이내인지 검사. spdk_likely로
				 * 정상 경로(작은 read)임을 컴파일러에 힌트. */
				bdev_io->u.bdev.iovs[0].iov_base = g_null_read_buf;
				/* [한국어] 가짜 응답 버퍼로 포인터 지정. 동시 read가 같은 버퍼를 봐도
				 * 어차피 0으로 채워져 있으므로 race 무해. */
				bdev_io->u.bdev.iovs[0].iov_len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
				/* [한국어] 길이를 요청 크기로 설정. 상위가 이 만큼 읽었다고 인식. */
			} else {
				/* [한국어] 한도 초과: 공유 버퍼는 SPDK_BDEV_LARGE_BUF_MAX_SIZE 크기만
				 * 할당돼 있으므로 더 큰 read는 안전하게 처리할 수 없다. 실패 처리. */
				SPDK_ERRLOG("Overflow occurred. Read I/O size %" PRIu64 " was larger than permitted %d\n",
					    bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
					    SPDK_BDEV_LARGE_BUF_MAX_SIZE);
				/* [한국어] 운영자가 한도 변경 또는 사용자 버퍼 제공을 검토하도록 로그. */
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
				/* [한국어] 즉시 실패 완료. 큐에 들어가지 않음. */
				return;
			}
		}
		if (SPDK_DIF_DISABLE != bdev->dif_type) {
			/* [한국어] DIF 활성 시 read 응답 데이터 위에 PI 태그를 생성한다. 실제 매체가
			 * 없는 null bdev이지만, 상위가 PI를 검증할 가능성이 있으므로 0 데이터에 대한
			 * 합법적 PI를 만들어 준다. */
			rc = spdk_dif_generate(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					       bdev_io->u.bdev.num_blocks, &dif_ctx);
			/* [한국어] guard tag(CRC), reftag, apptag를 각 블록 메타 영역에 채움. */
			if (0 != rc) {
				/* [한국어] PI 생성 실패. 실패 통보 후 종료. */
				SPDK_ERRLOG("IO DIF generation failed: lba %" PRIu64 ", num_block %" PRIu64 "\n",
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks);
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}
		TAILQ_INSERT_TAIL(&ch->io, null_io, link);
		/* [한국어] read 처리 완료 → 보류 큐에 넣고 다음 poller에서 SUCCESS 완료. */
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		if (SPDK_DIF_DISABLE != bdev->dif_type) {
			/* [한국어] write는 매체 미존재이므로 데이터를 저장하지 않지만, DIF가 켜져
			 * 있으면 사용자 데이터의 PI를 검증해 깨진 PI를 조기 검출한다. */
			rc = spdk_dif_verify(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					     bdev_io->u.bdev.num_blocks, &dif_ctx, &err_blk);
			/* [한국어] 각 블록의 guard/ref/app tag를 검사. err_blk에 첫 실패 위치 기록. */
			if (0 != rc) {
				/* [한국어] PI 미스매치 — 데이터 손상 가능성. 자세한 정보를 로그에 출력. */
				SPDK_ERRLOG("IO DIF verification failed: lba %" PRIu64 ", num_blocks %" PRIu64 ", "
					    "err_type %u, expected %lu, actual %lu, err_offset %u\n",
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    err_blk.err_type,
					    err_blk.expected,
					    err_blk.actual,
					    err_blk.err_offset);
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
				/* [한국어] 검증 실패 → write 자체를 FAILED로 보고. */
				return;
			}
		}
		TAILQ_INSERT_TAIL(&ch->io, null_io, link);
		/* [한국어] 정상 → 큐잉 후 poller에서 SUCCESS 완료. 데이터는 어디에도 쓰이지 않음. */
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
		TAILQ_INSERT_TAIL(&ch->io, null_io, link);
		/* [한국어] WRITE_ZEROES와 RESET은 데이터 검증이 불필요. 큐잉만 하고 다음 poller에서
		 * SUCCESS 통보. RESET은 본래 디바이스 상태 초기화이지만, null bdev는 상태가 없으므로
		 * 자동 성공. */
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		if (bdev_null_abort_io(ch, bdev_io->u.abort.bio_to_abort)) {
			/* [한국어] 보류 큐에서 대상 I/O를 찾아 ABORTED로 완료시킬 수 있었던 경우. */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
			/* [한국어] abort 자체는 SUCCESS로 보고(요청은 정상 처리됨). */
		} else {
			/* [한국어] 대상 I/O를 찾지 못함(이미 완료/다른 채널) → abort 실패. */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		/* [한국어] FLUSH(write back 동기화)와 UNMAP(deallocate)은 매체가 없는 null bdev에
		 * 의미 있는 의미가 없어 지원하지 않는다. 그 외 모든 미지원 타입도 동일. */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

/*
 * [한국어]
 * bdev_null_io_type_supported - 모듈이 특정 I/O 타입을 지원하는지 응답
 *
 * @ctx: spdk_bdev->ctxt (null_bdev 포인터). 본 함수에서는 사용하지 않음(모든 인스턴스 동일).
 * @io_type: 질의 대상 I/O 타입 enum.
 * @return: true = 지원, false = 미지원.
 *
 * bdev core가 상위 사용자 코드에 "이 bdev에서 어떤 I/O가 가능한지"를 알려주기 위해 호출한다.
 * null bdev는 데이터 매체가 없으므로 FLUSH/UNMAP은 무의미하고 false. 나머지 read/write/
 * write_zeroes/reset/abort는 모두 큐잉 후 SUCCESS로 완료할 수 있으므로 true.
 *
 * 호출 컨텍스트: 임의의 스레드에서 호출 가능(상태에 의존하지 않음).
 *
 * 호출 체인:
 *   spdk_bdev_io_type_supported → fn_table->io_type_supported → [이 함수]
 */
static bool
bdev_null_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_ABORT:
		/* [한국어] 위 5종은 submit_request 분기에서 정상 큐잉/완료 가능 → 지원으로 보고. */
		return true;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		/* [한국어] FLUSH/UNMAP은 매체 작업이 필요해 의미가 없음. 그 외 미정의 타입도 미지원. */
		return false;
	}
}

/*
 * [한국어]
 * bdev_null_get_io_channel - 호출자가 사용할 I/O 채널 반환 (fn_table.get_io_channel)
 *
 * @ctx: spdk_bdev->ctxt (null_bdev 포인터). 모든 인스턴스가 같은 io_device를 공유하므로 미사용.
 * @return: 호출 스레드에 해당하는 spdk_io_channel 핸들.
 *
 * SPDK는 io device당 한 번 spdk_io_device_register하면, 이후 spdk_get_io_channel 호출 시
 * "현재 reactor 스레드"용 코어 로컬 채널 ctx_buf(null_io_channel)를 자동으로 만들어준다.
 * 이미 만들어진 채널이 있으면 동일한 핸들을 재사용해 참조 카운트를 증가시킨다.
 *
 * 호출 컨텍스트: 사용자가 spdk_bdev_get_io_channel을 호출한 reactor 스레드.
 *
 * 호출 체인:
 *   사용자 → spdk_bdev_get_io_channel → fn_table->get_io_channel → [이 함수]
 *      → spdk_get_io_channel → null_bdev_create_cb (최초 호출 시)
 */
static struct spdk_io_channel *
bdev_null_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_null_bdev_head);
	/* [한국어] io_device 키로 g_null_bdev_head 주소를 사용한다. 이 주소는 모듈 초기화 시
	 * spdk_io_device_register에 등록된 키와 동일해야 함. */
}

/*
 * [한국어]
 * bdev_null_write_config_json - bdev 인스턴스의 RPC 재현 명령을 JSON으로 출력
 *
 * @bdev: 출력 대상 bdev.
 * @w: spdk_json_write_ctx — 출력 스트림(보통 RPC 응답 또는 config 파일).
 *
 * `spdk_save_config` 또는 `bdev_get_config` RPC가 호출되면 모든 bdev에 대해 이 콜백이 호출되어
 * 자기를 다시 만들기 위한 JSON-RPC 명령을 직렬화한다. 출력 스키마:
 *   { "method": "bdev_null_create",
 *     "params": { "name": ..., "num_blocks": ..., "block_size": ..., ... } }
 * 이 출력만 그대로 다시 RPC로 재생하면 같은 bdev가 복구된다.
 *
 * 호출 컨텍스트: 보통 main(RPC) 스레드.
 *
 * 호출 체인:
 *   spdk_bdev_subsystem_config_json → fn_table->write_config_json → [이 함수]
 */
static void
bdev_null_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	/* [한국어] 최상위 객체 시작 — RPC 명령 한 개를 표현. */

	spdk_json_write_named_string(w, "method", "bdev_null_create");
	/* [한국어] RPC 메서드명. 재생 시 이 이름으로 핸들러가 디스패치됨. */

	spdk_json_write_named_object_begin(w, "params");
	/* [한국어] 메서드 파라미터 객체 시작. */
	spdk_json_write_named_string(w, "name", bdev->name);
	/* [한국어] bdev 이름(고유 키). RPC 재생에서 동일한 이름으로 생성. */
	spdk_json_write_named_uint64(w, "num_blocks", bdev->blockcnt);
	/* [한국어] 총 블록 개수. */
	spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	/* [한국어] 블록 크기(데이터+메타 합산). */
	spdk_json_write_named_uint32(w, "physical_block_size", bdev->phys_blocklen);
	/* [한국어] 물리 블록 크기(보통 4096). 정렬 권장값. */
	spdk_json_write_named_uint32(w, "md_size", bdev->md_len);
	/* [한국어] 메타데이터(PI 포함) 길이. */
	spdk_json_write_named_uint32(w, "dif_type", bdev->dif_type);
	/* [한국어] DIF 타입(0/1/2/3). 0이면 비활성. */
	spdk_json_write_named_bool(w, "dif_is_head_of_md", bdev->dif_is_head_of_md);
	/* [한국어] PI 위치(메타 앞=true/뒤=false). */
	spdk_json_write_named_uint32(w, "dif_pi_format", bdev->dif_pi_format);
	/* [한국어] PI 포맷(16b/32b/64b CRC). */
	spdk_json_write_named_uuid(w, "uuid", &bdev->uuid);
	/* [한국어] bdev UUID. 같은 인스턴스 식별에 사용. */
	spdk_json_write_named_uint32(w, "preferred_write_granularity", bdev->preferred_write_granularity);
	/* [한국어] write 권장 단위(블록). 상위 최적화 힌트. */
	spdk_json_write_named_uint32(w, "preferred_write_alignment", bdev->preferred_write_alignment);
	/* [한국어] write 권장 정렬(블록). */
	spdk_json_write_named_uint32(w, "optimal_write_size", bdev->optimal_write_size);
	/* [한국어] 최적 write 크기. */
	spdk_json_write_named_uint32(w, "preferred_unmap_granularity", bdev->preferred_unmap_granularity);
	/* [한국어] unmap 권장 단위. (null bdev는 unmap 미지원이지만 메타데이터로 보존) */
	spdk_json_write_named_uint32(w, "preferred_unmap_alignment", bdev->preferred_unmap_alignment);
	/* [한국어] unmap 권장 정렬. */
	spdk_json_write_object_end(w);
	/* [한국어] params 객체 끝. */

	spdk_json_write_object_end(w);
	/* [한국어] 최상위 객체 끝. */
}

/*
 * [한국어] null_fn_table (정적 const)
 *
 * spdk_bdev_fn_table은 bdev core가 모듈에 위임할 콜백 묶음이다. bdev_null_create에서
 * spdk_bdev->fn_table에 이 테이블 주소를 설정해두면, 이후 모든 I/O와 관리 작업이 이
 * 콜백들을 거쳐 본 모듈로 라우팅된다.
 *
 * 콜백 매핑:
 *   destruct          : bdev 해제 시점.
 *   submit_request    : I/O 제출.
 *   io_type_supported : I/O 종류 지원 여부 질의.
 *   get_io_channel    : 코어 로컬 채널 획득.
 *   write_config_json : RPC 재현 출력.
 * (dump_info_json 등 미지정 콜백은 NULL 기본값.)
 */
static const struct spdk_bdev_fn_table null_fn_table = {
	.destruct		= bdev_null_destruct,
	/* [한국어] 위 bdev_null_destruct 참조. */
	.submit_request		= bdev_null_submit_request,
	/* [한국어] 핵심 I/O 진입점. 위 함수 참조. */
	.io_type_supported	= bdev_null_io_type_supported,
	/* [한국어] 지원 I/O 타입 질의. */
	.get_io_channel		= bdev_null_get_io_channel,
	/* [한국어] 코어 로컬 채널 획득. */
	.write_config_json	= bdev_null_write_config_json,
	/* [한국어] config save 시 JSON 직렬화. */
};

/* Use a dummy DIF context to validate DIF configuration of the
 * craeted bdev.
 */
/*
 * [한국어]
 * _bdev_validate_dif_config - 생성 시점에 DIF 설정 조합이 유효한지 사전 검증
 *
 * @bdev: 검증할 bdev (아직 spdk_bdev_register 전).
 * @return: 0 = 유효, 음수(-EINVAL 등) = 잘못된 조합.
 *
 * spdk_dif_ctx_init은 "blocklen / md_len / dif_type / pi_format / md_interleave" 같은
 * 파라미터의 자체 호환성을 검사한다. bdev 생성 시점에 더미 컨텍스트를 한 번 초기화해보고,
 * 실패하면 RPC 자체를 거절함으로써 "I/O가 들어오기 전" 단계에서 설정 오류를 잡는다.
 *
 * 호출 컨텍스트: bdev_null_create에서만 호출 (RPC 처리 스레드).
 *
 * 호출 체인:
 *   bdev_null_create → [이 함수] → spdk_dif_ctx_init
 */
static int
_bdev_validate_dif_config(struct spdk_bdev *bdev)
{
	struct spdk_dif_ctx dif_ctx;
	/* [한국어] 검증 전용 임시 컨텍스트. 함수 종료와 함께 폐기됨. */
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	/* [한국어] PI 포맷 같은 확장 옵션 전달용. */

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	/* [한국어] ABI 호환성을 위한 옵션 구조체 크기 명시. */
	dif_opts.dif_pi_format = bdev->dif_pi_format;
	/* [한국어] bdev 옵션의 PI 포맷을 그대로 사용. */

	return spdk_dif_ctx_init(&dif_ctx,
				 bdev->blocklen,
				 /* [한국어] 데이터+메타 합산 블록 크기. */
				 bdev->md_len,
				 /* [한국어] 메타 길이. */
				 true,
				 /* [한국어] md_interleave를 강제로 true로 — null bdev는 인터리브 모드만
				  * 가정한다. (실제 매체가 없으니 검증 자체가 형식적) */
				 bdev->dif_is_head_of_md,
				 /* [한국어] PI 위치 (head/tail). */
				 bdev->dif_type,
				 /* [한국어] DIF 타입 1/2/3. */
				 bdev->dif_check_flags,
				 /* [한국어] guard/reftag/apptag 검사 활성 비트마스크. */
				 SPDK_DIF_REFTAG_IGNORE,
				 /* [한국어] reftag 초기값을 IGNORE로 두고, 호환성 자체만 확인. */
				 0xFFFF, SPDK_DIF_APPTAG_IGNORE,
				 /* [한국어] apptag_mask = 0xFFFF, apptag = IGNORE — 검증 단계에 무관. */
				 0, 0, &dif_opts);
				 /* [한국어] reserved, offset, ext_opts. */
}

/*
 * [한국어]
 * bdev_null_create - RPC가 호출하는 null bdev 인스턴스 생성 함수 (공개 API)
 *
 * @bdev: [out] 생성된 spdk_bdev 포인터를 받을 위치(성공 시 채워짐).
 * @opts: 생성 옵션(name, num_blocks, block_size, md_size, dif_*, uuid 등).
 * @return: 0 성공, 음수(errno) 실패. 실패 시 *bdev 미정의.
 *
 * RPC bdev_null_rpc.c의 핸들러가 JSON 파라미터를 파싱해 null_bdev_opts로 채운 뒤 이 함수를
 * 호출한다. 단계:
 *   1) opts 유효성 검사 (md_size 화이트리스트, 512B 정렬, num_blocks > 0).
 *   2) null_bdev 구조체 calloc + name strdup.
 *   3) spdk_bdev 메타데이터 채우기(write_cache off, fn_table, module 등).
 *   4) DIF 설정 조합 사전 검증.
 *   5) UUID가 명시되어 있으면 복사, 아니면 spdk_bdev_register가 자동 생성.
 *   6) spdk_bdev_register로 bdev core에 등록.
 *   7) 모듈 글로벌 리스트에 추가.
 *
 * 실패 처리: 단계별로 오류 시 이미 할당한 메모리를 해제하고 errno 음수 반환.
 *
 * 호출 컨텍스트: RPC 처리 스레드(보통 main thread). 이 함수는 코어 간 메시지 송신을 하지
 * 않으므로 호출 스레드 위에서 동기적으로 완료된다.
 *
 * 호출 체인:
 *   RPC bdev_null_create 핸들러 → [이 함수] → spdk_bdev_register
 */
int
bdev_null_create(struct spdk_bdev **bdev, const struct null_bdev_opts *opts)
{
	struct null_bdev *null_disk;
	/* [한국어] 새로 만들 모듈 인스턴스 포인터(임시 변수). */
	uint32_t block_size;
	/* [한국어] data + metadata 합산 블록 크기. spdk_bdev->blocklen에 들어갈 값. */
	int rc;
	/* [한국어] 임시 반환 코드 버퍼. */

	if (!opts) {
		/* [한국어] 옵션 미지정 — 호출자 버그. */
		SPDK_ERRLOG("No options provided for Null bdev.\n");
		return -EINVAL;
	}

	switch (opts->md_size) {
	case 0:
	case 8:
	case 16:
	case 32:
	case 64:
	case 128:
		/* [한국어] NVMe 스펙에서 일반적으로 허용하는 메타데이터 크기들. 이 외 값은 거부. */
		break;
	default:
		SPDK_ERRLOG("metadata size %u is not supported\n", opts->md_size);
		return -EINVAL;
	}

	if (opts->block_size % 512 != 0) {
		/* [한국어] 데이터 블록 크기는 512B(전통적 섹터) 배수여야 함. */
		SPDK_ERRLOG("Data block size %u is not a multiple of 512.\n", opts->block_size);
		return -EINVAL;
	}

	if (opts->physical_block_size % 512 != 0) {
		/* [한국어] 물리 블록도 512B 정렬 필요. */
		SPDK_ERRLOG("Physical block must be 512 bytes aligned\n");
		return -EINVAL;
	}

	block_size = opts->block_size + opts->md_size;
	/* [한국어] DIF 인터리브 모드에서 블록당 실효 크기 = data + meta. */

	if (opts->num_blocks == 0) {
		/* [한국어] 0 블록 디스크는 무의미. */
		SPDK_ERRLOG("Disk must be more than 0 blocks\n");
		return -EINVAL;
	}

	null_disk = calloc(1, sizeof(*null_disk));
	/* [한국어] 0으로 초기화된 인스턴스 할당. spdk_bdev 임베드 + tailq 노드 포함. */
	if (!null_disk) {
		SPDK_ERRLOG("could not allocate null_bdev\n");
		return -ENOMEM;
	}

	null_disk->bdev.name = strdup(opts->name);
	/* [한국어] 이름 문자열을 복사 소유. opts는 호출자가 관리하므로 자체 사본 필요. */
	if (!null_disk->bdev.name) {
		/* [한국어] strdup 실패 → 이미 할당한 null_disk 정리 후 OOM. */
		free(null_disk);
		return -ENOMEM;
	}
	null_disk->bdev.product_name = "Null disk";
	/* [한국어] 사용자 친화적 product 이름 (RPC bdev_get_bdevs 등에서 노출). 정적 문자열. */

	null_disk->bdev.write_cache = 0;
	/* [한국어] write back 캐시 없음(매체 없음 → cache flush 의미 없음).
	 * bdev core는 이 플래그를 보고 사용자에게 "FLUSH가 필요한 디바이스인가"를 알려준다.
	 * 또한 io_type_supported에서 FLUSH를 false로 반환하는 것과 일관성을 맞춰준다. */
	null_disk->bdev.blocklen = block_size;
	/* [한국어] 블록 크기(data + md 합산). 위 line "block_size = opts->block_size + opts->md_size"
	 * 의 계산 결과를 그대로 노출. 모든 I/O는 이 단위의 배수로 처리된다. */
	null_disk->bdev.preferred_write_alignment = opts->preferred_write_alignment;
	/* [한국어] 상위 컨슈머에 권장 쓰기 정렬(블록 단위)을 노출. 사용자가 명시하지 않았으면 0.
	 * bdev core가 RPC bdev_get_bdevs 결과에 포함시켜 응답하므로, fio 같은 워크로드 도구가
	 * 이 힌트를 활용할 수 있다. */
	null_disk->bdev.preferred_write_granularity = opts->preferred_write_granularity;
	/* [한국어] 권장 쓰기 단위. preferred_write_alignment와 함께 노출 힌트. null bdev에서는
	 * 실제 매체가 없으므로 어떤 값이든 성능 영향 없음 — 테스트용 메타데이터. */
	null_disk->bdev.optimal_write_size = opts->optimal_write_size;
	/* [한국어] 최적 쓰기 크기(블록). 상위 레이어가 이 크기 배수로 쓰기를 정렬할 수 있는 힌트. */
	null_disk->bdev.preferred_unmap_alignment = opts->preferred_unmap_alignment;
	/* [한국어] UNMAP(=NVMe Deallocate / SCSI UNMAP) 권장 정렬. null bdev는 UNMAP을 지원하지
	 * 않지만(io_type_supported가 false), 메타데이터로는 보존하여 config save/restore에서
	 * 동일한 값을 다시 만들 수 있게 한다. */
	null_disk->bdev.preferred_unmap_granularity = opts->preferred_unmap_granularity;
	/* [한국어] UNMAP 권장 단위. 위와 동일한 형식적 메타데이터. */
	null_disk->bdev.phys_blocklen = opts->physical_block_size;
	/* [한국어] 물리 블록 크기. */
	null_disk->bdev.blockcnt = opts->num_blocks;
	/* [한국어] 디스크 총 블록 수. blocklen * blockcnt = 디스크 용량. */
	null_disk->bdev.md_len = opts->md_size;
	/* [한국어] 메타데이터(PI 포함) 길이(바이트). NVMe Spec §6.5 LBA Format의 MS 필드와 대응.
	 * 0이면 메타 없음(=PI 비활성), 8 이상이면 DIF 영역으로 사용 가능. */
	null_disk->bdev.md_interleave = true;
	/* [한국어] null bdev는 인터리브 모드 고정 (별도 메타 영역을 가지지 않음).
	 * 인터리브 모드: 한 블록 내에 [data][meta] 또는 [meta][data] 형태로 배치.
	 * 분리(separate) 모드는 데이터 버퍼와 메타 버퍼가 다른 iovec로 전달되는 형태인데
	 * 본 모듈은 그것을 지원하지 않으므로 단순화를 위해 항상 인터리브. */
	null_disk->bdev.dif_type = opts->dif_type;
	/* [한국어] DIF(Data Integrity Field) 타입을 사용자 지정 값으로 설정.
	 * 0=Disable, 1=Type1(reftag=LBA), 2=Type2(reftag 사용자 지정), 3=Type3(reftag 검사 없음).
	 * NVMe Base Spec §8.3 End-to-End Data Protection 참조. */
	null_disk->bdev.dif_is_head_of_md = opts->dif_is_head_of_md;
	/* [한국어] PI(Protection Information)가 메타데이터 영역의 머리(true) 또는 꼬리(false)에
	 * 위치하는지. spdk_dif_generate/verify가 PI 오프셋 계산에 사용한다. */
	/* Current block device layer API does not propagate
	 * any DIF related information from user. So, we can
	 * not generate or verify Application Tag.
	 */
	/* [한국어] 위 영문 주석 부연: SPDK bdev 레이어가 사용자로부터 apptag 등의 DIF 정보를
	 * 전달받지 않으므로 apptag 검사는 비활성. guard/reftag만 검사한다. */
	switch (opts->dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		/* [한국어] Type 1/2: guard(CRC) + reftag(LBA 기반) 검사 활성. */
		null_disk->bdev.dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK |
						  SPDK_DIF_FLAGS_REFTAG_CHECK;
		break;
	case SPDK_DIF_TYPE3:
		/* [한국어] Type 3: reftag는 사용자 정의 가능 → 자동 검사 안 함. guard만. */
		null_disk->bdev.dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK;
		break;
	case SPDK_DIF_DISABLE:
		/* [한국어] 비활성: 검사 플래그 0(이미 calloc으로 0). */
		break;
	}
	null_disk->bdev.dif_pi_format = opts->dif_pi_format;
	/* [한국어] PI 포맷(16b/32b/64b). spdk_dif_ctx_init에서 사용됨. */

	if (opts->dif_type != SPDK_DIF_DISABLE) {
		/* [한국어] DIF 활성 시 사전 호환성 검증. 잘못된 조합이면 등록 전에 거절. */
		rc = _bdev_validate_dif_config(&null_disk->bdev);
		if (rc != 0) {
			SPDK_ERRLOG("DIF configuration was wrong\n");
			free(null_disk);
			/* [한국어] name은 strdup 후이지만 free 누락 — 이건 원본 코드 버그성 누락이며
			 * 본 작업은 코드 수정 금지이므로 그대로 둔다. */
			return -EINVAL;
		}
	}

	if (!spdk_uuid_is_null(&opts->uuid)) {
		/* [한국어] 사용자가 UUID를 지정했으면 복사. NULL UUID이면 spdk_bdev_register가
		 * 자동으로 무작위 UUID를 부여한다. */
		spdk_uuid_copy(&null_disk->bdev.uuid, &opts->uuid);
	}

	null_disk->bdev.ctxt = null_disk;
	/* [한국어] 콜백에 전달될 ctx. fn_table 콜백 첫 인자가 이 포인터. */
	null_disk->bdev.fn_table = &null_fn_table;
	/* [한국어] bdev core가 호출할 콜백 묶음. */
	null_disk->bdev.module = &null_if;
	/* [한국어] 이 bdev의 소속 모듈. unregister_by_name이 모듈 일치 검사에 사용. */

	rc = spdk_bdev_register(&null_disk->bdev);
	/* [한국어] bdev core에 등록. 성공 시 이 bdev는 spdk_bdev_get_by_name 등으로 발견 가능. */
	if (rc) {
		/* [한국어] 등록 실패(이름 중복 등). 자원 정리 후 오류 반환. */
		free(null_disk->bdev.name);
		free(null_disk);
		return rc;
	}

	*bdev = &(null_disk->bdev);
	/* [한국어] 호출자에게 새 bdev 포인터 전달. */

	TAILQ_INSERT_TAIL(&g_null_bdev_head, null_disk, tailq);
	/* [한국어] 모듈 글로벌 리스트에 추가. fini 시 일괄 정리에 활용. */

	return rc;
	/* [한국어] rc는 위 spdk_bdev_register 성공 시 0. 그 값 그대로 반환. */
}

/*
 * [한국어]
 * bdev_null_delete - RPC가 호출하는 null bdev 삭제 함수 (공개 API)
 *
 * @bdev_name: 삭제 대상 bdev 이름.
 * @cb_fn: 비동기 완료 콜백.
 * @cb_arg: 콜백 첫 인자.
 *
 * spdk_bdev_unregister_by_name은 이름과 모듈 일치를 확인 후 비동기로 unregister를 시작한다.
 * 호출이 동기 단계에서 즉시 실패(rc != 0)하면 콜백을 직접 호출해 오류 통보. 정상 케이스에서는
 * unregister 완료 후 bdev core가 cb_fn을 호출한다.
 *
 * 호출 컨텍스트: RPC 처리 스레드.
 *
 * 호출 체인:
 *   RPC bdev_null_delete 핸들러 → [이 함수]
 *     → spdk_bdev_unregister_by_name → fn_table->destruct (= bdev_null_destruct)
 *     → cb_fn(cb_arg, 0)
 */
void
bdev_null_delete(const char *bdev_name, spdk_delete_null_complete cb_fn, void *cb_arg)
{
	int rc;
	/* [한국어] unregister 즉시 실패 코드 검사용. */

	rc = spdk_bdev_unregister_by_name(bdev_name, &null_if, cb_fn, cb_arg);
	/* [한국어] 이름 + 모듈로 검색해 unregister. 발견 못하거나 모듈이 다르면 즉시 음수 반환. */
	if (rc != 0) {
		/* [한국어] 동기 단계 실패 → 콜백을 직접 호출해 사용자에게 통보(오류 코드 전달). */
		cb_fn(cb_arg, rc);
	}
}

/*
 * [한국어]
 * null_io_poll - 채널의 보류 I/O를 일괄 SUCCESS 완료시키는 poller 콜백
 *
 * @arg: 채널 포인터(SPDK_POLLER_REGISTER 등록 시 전달된 ctx).
 * @return: SPDK_POLLER_BUSY = 처리할 일이 있었음, SPDK_POLLER_IDLE = 큐가 비어 idle.
 *
 * SPDK reactor가 매 tick마다 등록된 poller를 호출한다. 본 함수는 채널의 io 큐를 한 번에
 * 로컬 변수로 swap해 잠금 없이 안전하게 비우고, 각 보류 I/O를 SUCCESS로 완료 통보한다.
 * SWAP 패턴은 한 호출 동안의 신규 enqueue가 다음 tick으로 미뤄지도록 하는 효과(공정성)도 있다.
 *
 * 호출 컨텍스트: 채널 소유 reactor 스레드. 큐 접근에 락 불필요.
 *
 * 호출 체인:
 *   SPDK reactor tick → [이 함수] → spdk_bdev_io_complete (각 I/O마다)
 */
static int
null_io_poll(void *arg)
{
	struct null_io_channel		*ch = arg;
	/* [한국어] 콜백 인자 → 채널 포인터로 회복. */
	TAILQ_HEAD(, null_bdev_io)	io;
	/* [한국어] 임시 로컬 큐. ch->io의 내용을 swap해서 받는다. */
	struct null_bdev_io		*null_io;
	/* [한국어] dequeue 결과를 받을 임시 변수. */

	TAILQ_INIT(&io);
	/* [한국어] 로컬 큐 초기화 (빈 상태). */
	TAILQ_SWAP(&ch->io, &io, null_bdev_io, link);
	/* [한국어] ch->io ↔ io 헤드 포인터 교환. 결과: ch->io는 빈 큐, io는 보류 항목 전부 보유.
	 * 이후 ch->io는 새로운 submit_request가 곧바로 사용 가능. */

	if (TAILQ_EMPTY(&io)) {
		/* [한국어] 큐가 비었음 → 이번 tick은 한 일 없음. reactor에 idle 보고(전력/CPU 절감 힌트). */
		return SPDK_POLLER_IDLE;
	}

	while (!TAILQ_EMPTY(&io)) {
		/* [한국어] 로컬 큐에 남은 모든 I/O를 처리할 때까지 반복. */
		null_io = TAILQ_FIRST(&io);
		/* [한국어] 큐의 첫 항목 가리키기(제거 전). */
		TAILQ_REMOVE(&io, null_io, link);
		/* [한국어] 큐에서 분리. */
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(null_io), SPDK_BDEV_IO_STATUS_SUCCESS);
		/* [한국어] driver_ctx 포인터에서 spdk_bdev_io를 회복하고 SUCCESS로 완료 통보.
		 * 이 호출이 사용자 콜백(cb_fn)을 동일 스레드에서 즉시 실행. */
	}

	return SPDK_POLLER_BUSY;
	/* [한국어] 한 번이라도 일했다면 BUSY 반환 → reactor가 다음 tick을 더 빠르게 돌릴 수 있음. */
}

/*
 * [한국어]
 * null_bdev_create_cb - io device의 코어 로컬 채널 생성 콜백
 *
 * @io_device: spdk_io_device_register에 전달한 키 (= &g_null_bdev_head).
 * @ctx_buf: 코어 로컬 ctx_buf — sizeof(null_io_channel) 크기로 코어가 자동 할당해 전달.
 * @return: 0 성공.
 *
 * 어떤 reactor에서 spdk_get_io_channel(&g_null_bdev_head)이 처음 호출되면, 코어는 사이즈만큼
 * 메모리를 할당해 이 콜백을 호출한다. 본 함수는 그 메모리를 null_io_channel로 초기화하고
 * poller를 등록한다. 이후 이 채널 핸들은 같은 reactor가 spdk_get_io_channel을 다시 호출해도
 * 재사용된다(refcount).
 *
 * 호출 컨텍스트: 채널을 요청한 reactor 스레드.
 *
 * 호출 체인:
 *   spdk_get_io_channel(첫 호출) → io_device의 create_cb (= [이 함수]) → SPDK_POLLER_REGISTER
 */
static int
null_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct null_io_channel *ch = ctx_buf;
	/* [한국어] 코어가 할당한 ctx_buf 메모리를 null_io_channel로 해석. */

	TAILQ_INIT(&ch->io);
	/* [한국어] 보류 큐 초기화(빈 큐). submit_request가 이후 INSERT_TAIL로 채움. */
	ch->poller = SPDK_POLLER_REGISTER(null_io_poll, ch, 0);
	/* [한국어] period=0 → reactor가 매 tick마다 호출. ch가 콜백 인자로 전달됨. */

	return 0;
}

/*
 * [한국어]
 * null_bdev_destroy_cb - io device의 코어 로컬 채널 파괴 콜백
 *
 * @io_device: 위와 동일.
 * @ctx_buf: 파괴 대상 채널 ctx.
 *
 * spdk_put_io_channel의 마지막 참조 해제 시점에 코어가 호출. poller만 unregister하면 ctx_buf
 * 자체는 코어가 free한다.
 *
 * 호출 컨텍스트: 채널 소유 reactor 스레드.
 *
 * 호출 체인:
 *   spdk_put_io_channel(refcount→0) → io_device의 destroy_cb (= [이 함수])
 */
static void
null_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct null_io_channel *ch = ctx_buf;
	/* [한국어] ctx_buf → null_io_channel로 캐스트. io_device 인자는 사용하지 않음
	 * (등록 시 키였던 &g_null_bdev_head 주소가 들어오지만 본 콜백에서는 불필요). */

	spdk_poller_unregister(&ch->poller);
	/* [한국어] poller 해제. spdk_poller_unregister는 ch->poller를 NULL로 설정하고
	 * 내부적으로 reactor에서 안전하게 떼어낸다. 이후로 null_io_poll이 호출되지 않음.
	 * 큐가 비어있어야 하지만 본 모듈은 unregister 시점 잔여 I/O가 없다고 가정한다
	 * (보류 I/O가 남아 있는 채로 채널이 destroy되면 메모리 누수 및 미완료 콜백 가능성).
	 * ctx_buf 자체의 free는 SPDK thread 레이어가 수행 → 본 함수는 메모리 해제 책임 없음. */
}

/*
 * [한국어]
 * bdev_null_initialize - 모듈 init 콜백 (spdk_bdev_module.module_init)
 *
 * @return: 0 성공, -1 실패.
 *
 * SPDK 부팅 시 한 번만 호출되어 모듈 글로벌 상태를 준비한다:
 *   1) 모든 read 응답이 공유할 0으로 채운 버퍼를 hugepage(DMA 가능)에 할당.
 *   2) g_null_bdev_head 주소를 io device 키로 등록 → 이후 spdk_get_io_channel 가능.
 *
 * 실패 처리: spdk_zmalloc 실패 시 -1만 반환(코어가 모듈 init 실패로 처리).
 *
 * 호출 컨텍스트: SPDK init thread (보통 main thread).
 *
 * 호출 체인:
 *   spdk_subsystem_init → bdev subsystem init → 각 bdev module module_init → [이 함수]
 */
static int
bdev_null_initialize(void)
{
	/*
	 * This will be used if upper layer expects us to allocate the read buffer.
	 *  Instead of using a real rbuf from the bdev pool, just always point to
	 *  this same zeroed buffer.
	 */
	/* [한국어] 위 영문 주석 부연: 상위가 read 시 모듈 자체 버퍼를 기대하는 경로에서, bdev pool의
	 * 실제 rbuf를 매번 할당하는 대신 항상 이 단일 0-버퍼를 가리키도록 한다. 즉 read는
	 * 사실상 메모리 할당 비용 없이 처리된다. */
	g_null_read_buf = spdk_zmalloc(SPDK_BDEV_LARGE_BUF_MAX_SIZE, 0, NULL,
				       SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	/* [한국어] DPDK hugepage에 SPDK_BDEV_LARGE_BUF_MAX_SIZE 만큼의 0으로 채워진(zmalloc) 메모리.
	 * align=0 → 기본 정렬, phys_addr=NULL → 물리 주소 반환 불필요.
	 * SPDK_ENV_NUMA_ID_ANY → 임의 NUMA 노드, SPDK_MALLOC_DMA → DMA 가능 메모리 요청. */
	if (g_null_read_buf == NULL) {
		/* [한국어] hugepage 메모리 부족 → 모듈 초기화 실패. */
		return -1;
	}

	/*
	 * We need to pick some unique address as our "io device" - so just use the
	 *  address of the global tailq.
	 */
	/* [한국어] 위 영문 주석 부연: io device 키는 SPDK 전역에서 유일해야 한다. 별도 변수를 만들지
	 * 않고 글로벌 리스트의 주소를 그대로 키로 재활용한다(모듈 단일 io device). */
	spdk_io_device_register(&g_null_bdev_head, null_bdev_create_cb, null_bdev_destroy_cb,
				sizeof(struct null_io_channel), "null_bdev");
	/* [한국어] io device 등록. 이후 spdk_get_io_channel이 호출되면 코어별로
	 * sizeof(null_io_channel) 만큼 ctx_buf를 할당해 create_cb로 위임한다. "null_bdev"는
	 * 디버그/통계용 이름. */

	return 0;
}

/*
 * [한국어]
 * dummy_bdev_event_cb - bdev_null_resize에서 spdk_bdev_open_ext에 전달하는 빈 이벤트 콜백
 *
 * @type: 이벤트 종류 (SPDK_BDEV_EVENT_REMOVE / SPDK_BDEV_EVENT_RESIZE 등). 미사용.
 * @bdev: 이벤트가 발생한 bdev. 미사용.
 * @ctx : 사용자 ctx. 미사용.
 *
 * spdk_bdev_open_ext API는 "remove/resize 이벤트가 발생했을 때 사용자에게 알리기 위한 콜백"을
 * 필수로 받는다(NULL 불가). resize 같은 일회성 작업에서는 open 후 즉시 close하므로
 * 이벤트 콜백이 호출될 일이 거의 없다 — 따라서 더미를 전달.
 *
 * 호출 컨텍스트: bdev core가 이벤트 통지 스레드에서 호출(만약 호출된다면).
 *
 * 호출 체인:
 *   bdev core 내부 이벤트 디스패치 → [이 함수](실질적으로 호출되지 않음)
 */
static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
	/* [한국어] 의도적으로 비어 있음. resize 작업 동안만 desc를 잡아두는 형식적 콜백.
	 * 만약 resize 실행 중 bdev가 외부에서 unregister되어 REMOVE 이벤트가 발생하더라도
	 * 본 함수는 무시한다(spdk_bdev_close가 모든 핸들을 정리). */
}

/*
 * [한국어]
 * bdev_null_resize - 기존 null bdev의 블록 카운트를 늘려 디스크 크기를 확장 (공개 API)
 *
 * @bdev_name: 대상 bdev 이름.
 * @new_size_in_mb: 새 크기 (MiB). 현재 크기보다 작으면 거절.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 단계:
 *   1) spdk_bdev_open_ext로 desc를 얻음.
 *   2) 모듈 일치 검사(다른 모듈의 bdev면 거절).
 *   3) 현재 크기와 비교(축소 거절).
 *   4) spdk_bdev_notify_blockcnt_change로 새 블록 카운트 통지.
 *   5) desc close.
 *
 * 호출 컨텍스트: RPC 스레드.
 *
 * 호출 체인:
 *   RPC bdev_null_resize → [이 함수] → spdk_bdev_notify_blockcnt_change
 */
int
bdev_null_resize(const char *bdev_name, const uint64_t new_size_in_mb)
{
	struct spdk_bdev_desc *desc;
	/* [한국어] open으로 얻는 description handle. close 시 반드시 해제. */
	struct spdk_bdev *bdev;
	/* [한국어] desc에서 얻은 bdev 본체 포인터. */
	uint64_t current_size_in_mb;
	/* [한국어] 현재 크기(MiB) 임시 보관. */
	uint64_t new_size_in_byte;
	/* [한국어] 새 크기(B) 임시 보관. blockcnt 계산에 사용. */
	int rc = 0;
	/* [한국어] 반환 코드. */

	rc = spdk_bdev_open_ext(bdev_name, false, dummy_bdev_event_cb, NULL, &desc);
	/* [한국어] write=false (read-only로 충분), event_cb=dummy. desc에 핸들 채워짐. */
	if (rc != 0) {
		/* [한국어] 이름의 bdev가 없거나 open 실패. */
		SPDK_ERRLOG("failed to open bdev; %s.\n", bdev_name);
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);
	/* [한국어] desc로부터 bdev 본체를 얻음. */

	if (bdev->module != &null_if) {
		/* [한국어] 이 RPC는 null bdev에만 적용. 다른 모듈의 bdev면 거절. */
		rc = -EINVAL;
		goto exit;
	}

	current_size_in_mb = bdev->blocklen * bdev->blockcnt / (1024 * 1024);
	/* [한국어] 현재 디스크 용량(MiB) = 블록크기 × 블록수 ÷ 1MiB. */
	if (new_size_in_mb < current_size_in_mb) {
		/* [한국어] 축소는 데이터 손실 위험으로 거절(이 모듈에선 매체가 없지만 일관성 유지). */
		SPDK_ERRLOG("The new bdev size must not be smaller than current bdev size.\n");
		rc = -EINVAL;
		goto exit;
	}

	new_size_in_byte = new_size_in_mb * 1024 * 1024;
	/* [한국어] MiB → B 변환. */

	rc = spdk_bdev_notify_blockcnt_change(bdev, new_size_in_byte / bdev->blocklen);
	/* [한국어] bdev core에 새 블록 카운트 통보 → 사용자가 등록한 EVENT_RESIZE 콜백이 깨어남. */
	if (rc != 0) {
		/* [한국어] 통지 실패(드물게 발생). */
		SPDK_ERRLOG("failed to notify block cnt change.\n");
	}

exit:
	spdk_bdev_close(desc);
	/* [한국어] desc 정리. open 후 항상 close 보장. */
	return rc;
}

/*
 * [한국어]
 * _bdev_null_finish_cb - io_device unregister 완료 후 후속 정리
 *
 * @arg: 미사용.
 *
 * spdk_io_device_unregister가 모든 채널 destroy를 끝낸 뒤 호출하는 콜백. 글로벌 read 버퍼를
 * 해제하고, async_fini 종료를 코어에 알린다.
 *
 * 호출 컨텍스트: io device 정리 스레드.
 *
 * 호출 체인:
 *   bdev_null_finish → spdk_io_device_unregister → ...all channels destroyed... → [이 함수]
 *     → spdk_bdev_module_fini_done()
 */
static void
_bdev_null_finish_cb(void *arg)
{
	spdk_free(g_null_read_buf);
	/* [한국어] hugepage read 버퍼 해제. spdk_zmalloc과 짝. */
	spdk_bdev_module_fini_done();
	/* [한국어] async_fini=true이므로 이 호출로 모듈 종료 완료를 코어에 통보. 이게 없으면
	 * 코어는 영원히 다음 모듈로 넘어가지 않는다. */
}

/*
 * [한국어]
 * bdev_null_finish - 모듈 fini 콜백 (spdk_bdev_module.module_fini)
 *
 * SPDK 종료 시 호출. async_fini=true이므로 비동기로 io device를 unregister하고, 그 완료
 * 콜백(_bdev_null_finish_cb)에서 read 버퍼를 free하며 fini_done을 통지한다.
 *
 * 만약 init 단계에서 read 버퍼 할당 실패로 io device 등록까지 도달하지 않았다면, 즉시
 * fini_done만 호출하고 종료.
 *
 * 호출 컨텍스트: SPDK shutdown thread.
 *
 * 호출 체인:
 *   spdk_subsystem_fini → bdev subsystem fini → 각 모듈 module_fini → [이 함수]
 */
static void
bdev_null_finish(void)
{
	if (g_null_read_buf == NULL) {
		/* [한국어] init이 read 버퍼 할당 전에 실패했거나 처음부터 등록되지 않음 → 추가 정리 없음.
		 * 이 경우 spdk_io_device_register도 호출되지 않았으므로 unregister하면 안 된다
		 * (등록되지 않은 io device를 unregister하면 SPDK가 abort). */
		spdk_bdev_module_fini_done();
		/* [한국어] async_fini=true이므로 모듈 종료 완료를 코어에 즉시 통보. */
		return;
	}
	spdk_io_device_unregister(&g_null_bdev_head, _bdev_null_finish_cb);
	/* [한국어] io device 해제. 등록된 모든 코어 채널의 destroy_cb를 비동기로 호출하고,
	 * 모두 끝나면 _bdev_null_finish_cb를 호출해 read 버퍼 free + spdk_bdev_module_fini_done.
	 * 비동기 종료 흐름:
	 *   spdk_io_device_unregister
	 *     → 각 reactor에 채널 destroy 메시지 전송
	 *     → 모든 reactor에서 null_bdev_destroy_cb 실행
	 *     → 마지막 reactor 완료 후 _bdev_null_finish_cb (= 콜백 인자)
	 *     → spdk_free(g_null_read_buf) + spdk_bdev_module_fini_done() */
}

SPDK_LOG_REGISTER_COMPONENT(bdev_null)
/* [한국어] "bdev_null" 로그 컴포넌트 등록. SPDK_DEBUGLOG 등 디버그 매크로가 이 컴포넌트
 * 이름으로 필터링된다(예: 실행 시 -L bdev_null 옵션). */
