/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK accel(가속) 프레임워크 코어 구현 (accel.c)
 *
 * === 파일의 역할 ===
 * SPDK의 가속(Accelerator) 추상화 계층의 코어 구현 파일이다. 이 파일은
 * "공통 가속 API"(`spdk_accel_submit_copy/fill/crc32c/compare/encrypt 등`)를
 * 정의하고, 실제 하드웨어/SW 모듈은 그 아래 등록된 accel 모듈(예: software,
 * idxd, ioat, dsa, ae4dma, mlx5)에게 위임한다. 또한 여러 가속 작업을
 * 체이닝하여 한 번에 수행하는 "accel sequence" 메커니즘(bounce buffer/메모리
 * 도메인 변환 포함)과 암호 키 관리(keyring), 채널 단위 통계 수집, RPC를 위한
 * 모듈 열거 헬퍼를 모두 구현한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * [상위 사용자: bdev_crypto / NVMe-oF / blobstore 등]
 *   ↓ spdk_accel_submit_<op>() / spdk_accel_sequence_*()
 * [accel 코어(이 파일)]                              ← I/O 채널 + 시퀀스 SM
 *   ↓ accel_module->submit_tasks(io_ch, task)
 * [accel 모듈: sw(accel_sw.c) / idxd / dsa / ioat / ae4dma / mlx5]
 *   ↓ HW DMA / SIMD / RDMA verbs
 * [Hardware accelerator | CPU SIMD]
 * 호스트 유저스페이스(SPDK reactor 스레드)에서만 실행되며, 채널(`accel_io_channel`)은
 * 각 SPDK 스레드(=reactor)에 하나씩 존재해 lockless 동작을 보장한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `spdk/accel_module.h`(외부에 노출되는 모듈 등록 인터페이스),
 *   `spdk/dma.h`(memory domain 변환), `spdk/iobuf.h`(bounce 버퍼 풀),
 *   `spdk/thread.h`(spdk_io_channel/iobuf_channel), `spdk/crc32.h`(SW CRC),
 *   accel_internal.h(통계/모듈열거 헬퍼).
 * - 의존하는 모듈: `module/accel/*` 의 모든 가속 모듈, `accel_sw.c`(같은 라이브러리
 *   내 SW 폴백), `accel_rpc.c`(RPC 핸들러), `bdev/crypto`, `bdev/compress`,
 *   `nvmf` 등 가속 사용자.
 * - 데이터 흐름: 사용자가 `spdk_accel_submit_*` 호출 → `_get_task()`로 task 풀에서
 *   task 할당 → opcode별로 g_modules_opc[op].module->submit_tasks() → 완료 시
 *   `spdk_accel_task_complete()`가 cb_fn 호출 (시퀀스라면 SM 진행).
 * - 공유 자료구조: `g_modules_opc[]`(opcode→모듈 매핑), `g_keyring`(암호 키 글로벌
 *   리스트, spinlock 보호), `g_stats`(글로벌 통계, spinlock 보호).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct accel_io_channel: 각 SPDK 스레드 1개씩 — task/seq/buf 풀, opcode별
 *   하위 모듈 채널, iobuf 채널, 통계.
 * - struct spdk_accel_sequence: 체이닝된 가속 작업의 묶음. enum
 *   accel_sequence_state 상태머신으로 push/pull/exec 단계를 진행한다.
 * - struct accel_buffer: 시퀀스가 동적으로 할당하는 가상/바운스 버퍼 디스크립터.
 * - _get_task() / _put_task() / spdk_accel_task_complete(): task 풀 운용 및 완료 처리.
 * - spdk_accel_submit_copy/fill/crc32c/compare/dualcast/copy_crc32c/compress/
 *   decompress/encrypt/decrypt/xor/dif_*/dix_* : 개별 op submit 진입점들.
 * - spdk_accel_sequence_finish: 시퀀스 실행 시작 (상태머신 진입).
 * - accel_process_sequence: 시퀀스 상태머신 본체 (가장 복잡한 함수).
 * - accel_create_channel / accel_destroy_channel: io channel 생성/파괴.
 * - spdk_accel_crypto_key_create/destroy: AES-XTS 등 암호 키 등록·삭제.
 */

/* [한국어] 표준 SPDK 인클루드 — size_t, uint*_t, bool, errno 등 표준 타입 모음. */
#include "spdk/stdinc.h"

/* [한국어] accel 모듈 등록·콜백 인터페이스. struct spdk_accel_module_if,
 *  spdk_accel_module_list_add, struct spdk_accel_task 등을 가져온다. */
#include "spdk/accel_module.h"

/* [한국어] 같은 라이브러리 내부 헤더 — accel_stats, module_info,
 *  _accel_for_each_module 등 RPC와 공유하는 비공개 자료구조. */
#include "accel_internal.h"

/* [한국어] memory domain API — accel sequence가 GPU/NVMe 등 서로 다른 메모리
 *  도메인 간 push/pull 변환을 수행하기 위해 사용. */
#include "spdk/dma.h"
/* [한국어] DPDK 환경 추상화 — hugepage 메모리 할당, IOVA 변환 등. */
#include "spdk/env.h"
/* [한국어] spdk_likely/spdk_unlikely 분기 힌트 매크로. 핫패스 최적화용. */
#include "spdk/likely.h"
/* [한국어] SPDK 로그 매크로 (SPDK_ERRLOG/DEBUGLOG 등). */
#include "spdk/log.h"
/* [한국어] spdk_thread/io_channel API — 채널 생성·메시지 전달. */
#include "spdk/thread.h"
/* [한국어] JSON writer — RPC 응답을 위한 객체/배열 직렬화. */
#include "spdk/json.h"
/* [한국어] SW CRC32C 계산 함수. HW 가속이 없을 때 폴백으로 사용. */
#include "spdk/crc32.h"
/* [한국어] SPDK 공용 유틸 (spdk_min/max, divide_round_up 등). */
#include "spdk/util.h"
/* [한국어] hex 문자열 ↔ 바이너리 변환 (암호 키 hex 입력 처리). */
#include "spdk/hexlify.h"
/* [한국어] 문자열 유틸 (spdk_strdup, spdk_sprintf_alloc 등). */
#include "spdk/string.h"

/* Accelerator Framework: The following provides a top level
 * generic API for the accelerator functions defined here. Modules,
 * such as the one in /module/accel/ioat, supply the implementation
 * with the exception of the pure software implementation contained
 * later in this file.
 */

/* [한국어] DMA 정렬 기준 4KB — 하드웨어 DMA 요구사항(IOMMU/페이지) 충족용. */
#define ALIGN_4K			0x1000
/* [한국어] 각 accel_io_channel이 사전 할당하는 task 객체 수.
 *  큰 워크로드의 burst를 흡수할 수 있도록 2048개로 설정. */
#define ACCEL_TASKS_PER_CHANNEL		2048
/* [한국어] iobuf 풀의 small 버퍼 채널별 캐시 크기 (성능을 위한 per-thread 캐시). */
#define ACCEL_SMALL_CACHE_SIZE		128
/* [한국어] iobuf 풀의 large 버퍼 채널별 캐시 크기. */
#define ACCEL_LARGE_CACHE_SIZE		16
/* Set MSB, so we don't return NULL pointers as buffers */
/* [한국어] accel_buffer의 "가상 포인터" 베이스 — MSB(63비트)를 1로 만들어
 *  NULL과 구분되고, 일반 가상주소와 겹치지 않는 토큰형 포인터를 만든다.
 *  실제 데이터 버퍼가 아직 할당되지 않은 시퀀스 단계에서 디스크립터로 사용. */
#define ACCEL_BUFFER_BASE		((void *)(1ull << 63))
/* [한국어] 위 베이스 아래쪽 비트 마스크. accel_buffer 토큰에서 오프셋만 추출. */
#define ACCEL_BUFFER_OFFSET_MASK	((uintptr_t)ACCEL_BUFFER_BASE - 1)

/* [한국어] crypto 작업의 기본 tweak 모드 — XTS의 i 값을 LBA로 매핑하는 단순 모드. */
#define ACCEL_CRYPTO_TWEAK_MODE_DEFAULT	SPDK_ACCEL_CRYPTO_TWEAK_MODE_SIMPLE_LBA
/* [한국어] 하나의 시퀀스가 가질 수 있는 최대 task 수. 너무 많은 task가
 *  체이닝되어 디스패치가 길어지는 것을 막는 안전 제한. */
#define ACCEL_TASKS_IN_SEQUENCE_LIMIT	8

/*
 * [한국어] struct accel_module — opcode→모듈 매핑 테이블의 한 슬롯.
 *  각 opcode는 단 하나의 accel 모듈에 바인딩되며, 모듈의 메모리 도메인 지원 여부를
 *  같이 캐시해 두어 빠른 분기 결정을 돕는다.
 */
struct accel_module {
	struct spdk_accel_module_if	*module;
	/* [한국어] 이 opcode를 담당하는 accel 모듈 객체 포인터.
	 * 설정자: 모듈 초기화 단계에서 `accel_module_finish_started()`가 우선순위/
	 *   override에 따라 결정된 모듈 포인터를 채운다.
	 * 읽는 자: `accel_submit_task()`/`accel_get_buf_*()` 등 핫패스가 op->모듈 디스패치 시 참조.
	 * 값 범위: 등록된 모듈 포인터 또는 NULL(해당 opcode가 어떤 모듈도 지원하지 않음).
	 * 동기화: 모듈 초기화 후 read-only이므로 락 불필요. */

	bool				supports_memory_domains;
	/* [한국어] 이 모듈이 spdk_memory_domain을 직접 처리할 수 있는지 캐시.
	 * 설정자: 모듈 등록 시 module->supports_memory_domains 값을 그대로 복사.
	 * 읽는 자: 시퀀스 SM이 bounce buffer를 만들어 줄지 결정할 때.
	 * 값 범위: true/false. true면 GPU/RDMA 등 호스트 외 메모리도 모듈이 직접 DMA 가능.
	 * 동기화: read-only. */
};

/* Largest context size for all accel modules */
/* [한국어] 모든 등록 모듈이 요구하는 task 컨텍스트 크기 중 최댓값.
 *  task 풀의 객체 크기를 결정하는 데 사용된다(모듈 등록 시점에 갱신). */
static size_t g_max_accel_module_size = sizeof(struct spdk_accel_task);

/* [한국어] 모듈 초기화 진행 동안 현재 finish/시작 중인 모듈 포인터 (직렬화용). */
static struct spdk_accel_module_if *g_accel_module = NULL;
/* [한국어] spdk_accel_finish 호출 시 사용자가 등록한 종료 콜백. */
static spdk_accel_fini_cb g_fini_cb_fn = NULL;
/* [한국어] 위 콜백에 전달될 사용자 ctx. */
static void *g_fini_cb_arg = NULL;
/* [한국어] 모든 모듈이 시작 완료되었는지 표시. true가 되면 옵션/매핑 변경 금지. */
static bool g_modules_started = false;
/* [한국어] accel framework 자체의 spdk_memory_domain — bounce buffer 변환 등에 사용. */
static struct spdk_memory_domain *g_accel_domain;

/* Global list of registered accelerator modules */
/* [한국어] 모듈 등록 글로벌 리스트 — spdk_accel_module_list_add()로 모듈이 자기 자신을 등록.
 *  순회는 초기화 시 한 번이라 lockless. */
static TAILQ_HEAD(, spdk_accel_module_if) spdk_accel_module_list =
	TAILQ_HEAD_INITIALIZER(spdk_accel_module_list);

/* Crypto keyring */
/* [한국어] 등록된 모든 암호 키 글로벌 리스트(키 이름 → 키 객체). spinlock 보호. */
static TAILQ_HEAD(, spdk_accel_crypto_key) g_keyring = TAILQ_HEAD_INITIALIZER(g_keyring);
/* [한국어] g_keyring 보호용 spinlock (DPDK rte_spinlock 기반). */
static struct spdk_spinlock g_keyring_spin;

/* Global array mapping capabilities to modules */
/* [한국어] opcode→모듈 매핑 테이블 — 핫패스에서 op->module로 디스패치. */
static struct accel_module g_modules_opc[SPDK_ACCEL_OPC_LAST] = {};
/* [한국어] 사용자/RPC가 opcode마다 강제 지정한 모듈 이름(override). 시작 전까지만 설정 가능. */
static char *g_modules_opc_override[SPDK_ACCEL_OPC_LAST] = {};
/* [한국어] accel driver 리스트 — driver는 "여러 opcode를 묶어 처리"하는 상위 어댑터. */
TAILQ_HEAD(, spdk_accel_driver) g_accel_drivers = TAILQ_HEAD_INITIALIZER(g_accel_drivers);
/* [한국어] 현재 활성화된 driver(있을 시) — 시퀀스 디스패치 시 driver_exec_tasks 경로로 우회. */
static struct spdk_accel_driver *g_accel_driver;
/* [한국어] accel framework 전역 옵션(채널당 풀 크기 등). RPC `accel_set_options`로 변경 가능. */
static struct spdk_accel_opts g_opts = {
	.small_cache_size = ACCEL_SMALL_CACHE_SIZE,
	.large_cache_size = ACCEL_LARGE_CACHE_SIZE,
	.task_count = ACCEL_TASKS_PER_CHANNEL,
	.sequence_count = ACCEL_TASKS_PER_CHANNEL,
	.buf_count = ACCEL_TASKS_PER_CHANNEL,
};
/* [한국어] 글로벌 통계 — `accel_get_stats`가 모든 채널 통계를 누적해 채워 응답 생성. */
static struct accel_stats g_stats;
/* [한국어] g_stats 보호용 spinlock. */
static struct spdk_spinlock g_stats_lock;

/* [한국어] opcode → 사람이 읽을 수 있는 문자열 (RPC 응답/디버그 출력에 사용).
 *  배열 인덱스 순서는 enum spdk_accel_opcode와 정확히 일치해야 한다. */
static const char *g_opcode_strings[SPDK_ACCEL_OPC_LAST] = {
	"copy", "fill", "dualcast", "compare", "crc32c", "copy_crc32c",
	"compress", "decompress", "encrypt", "decrypt", "xor",
	"dif_verify", "dif_verify_copy", "dif_generate", "dif_generate_copy",
	"dix_generate", "dix_verify"
};

/*
 * [한국어] enum accel_sequence_state — accel sequence(체이닝된 가속 작업 묶음)의
 *  상태머신. accel_process_sequence()가 이 상태를 따라가며 단계별로 진행한다.
 *  진행 단계: INIT → virtbuf 할당 → bouncebuf 할당 → pull(메모리 도메인 변환) →
 *  exec task → push(반대 방향 변환) → 다음 task 또는 종료.
 *  ERROR 상태는 한 번 진입하면 다른 상태로 빠져나갈 수 없는 흡수 상태이다.
 */
enum accel_sequence_state {
	ACCEL_SEQUENCE_STATE_INIT,
	/* [한국어] 시퀀스 진입 초기 상태. 다음에 어떤 처리(virtbuf/bouncebuf/exec)가
	 * 필요한지 판단해 분기한다. */

	ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF,
	/* [한국어] 시퀀스에 사용되는 가상 버퍼(미할당 ACCEL_BUFFER_BASE 토큰)가
	 * 실제 메모리로 매핑됐는지 확인 — 미완료면 AWAIT_VIRTBUF로. */

	ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF,
	/* [한국어] iobuf 풀 대기 중 — 풀 부족 시 wait 콜백을 등록해 깨어날 때까지 정지. */

	ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF,
	/* [한국어] 모듈이 직접 처리 못 하는 메모리 도메인의 경우 bounce buffer가 필요한지 검사. */

	ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF,
	/* [한국어] bounce buffer iobuf 풀 대기 — virtbuf와 동일한 wait 패턴. */

	ACCEL_SEQUENCE_STATE_PULL_DATA,
	/* [한국어] 호스트 외 메모리(GPU/RDMA)에서 호스트 bounce 버퍼로 데이터 가져오기
	 *  (memory_domain pull translation). */

	ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA,
	/* [한국어] 위 pull이 비동기로 진행 중 — 완료 콜백 도착 시 다음 상태로 전이. */

	ACCEL_SEQUENCE_STATE_EXEC_TASK,
	/* [한국어] 현재 task를 모듈에 실제 submit. */

	ACCEL_SEQUENCE_STATE_AWAIT_TASK,
	/* [한국어] 모듈로부터 task 완료 콜백 도착 대기. */

	ACCEL_SEQUENCE_STATE_COMPLETE_TASK,
	/* [한국어] 완료된 task의 회수 처리 후 NEXT_TASK 결정 단계. */

	ACCEL_SEQUENCE_STATE_NEXT_TASK,
	/* [한국어] 시퀀스에 남은 다음 task로 진행하거나 종료(push) 단계로 전이. */

	ACCEL_SEQUENCE_STATE_PUSH_DATA,
	/* [한국어] bounce 버퍼의 데이터를 다시 원래 메모리 도메인으로 되돌리기(push). */

	ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA,
	/* [한국어] push 완료 대기. */

	ACCEL_SEQUENCE_STATE_DRIVER_EXEC_TASKS,
	/* [한국어] driver(여러 op를 묶어 한번에 처리하는 어댑터)가 등록된 경우의 디스패치 경로. */

	ACCEL_SEQUENCE_STATE_DRIVER_AWAIT_TASKS,
	/* [한국어] driver의 배치 처리 완료 대기. */

	ACCEL_SEQUENCE_STATE_DRIVER_COMPLETE_TASKS,
	/* [한국어] driver 경로의 후처리(통계 갱신/콜백 호출). */

	ACCEL_SEQUENCE_STATE_ERROR,
	/* [한국어] 흡수 상태 — 한 번 진입하면 시퀀스 종료까지 머무르며 status 보고 후
	 *  사용자 cb_fn을 비-0으로 호출. */

	ACCEL_SEQUENCE_STATE_MAX,
	/* [한국어] sentinel — 배열 크기/검증용. 유효 상태가 아니다. */
};

static const char *g_seq_states[]
__attribute__((unused)) = {
	[ACCEL_SEQUENCE_STATE_INIT] = "init",
	[ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF] = "check-virtbuf",
	[ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF] = "await-virtbuf",
	[ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF] = "check-bouncebuf",
	[ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF] = "await-bouncebuf",
	[ACCEL_SEQUENCE_STATE_PULL_DATA] = "pull-data",
	[ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA] = "await-pull-data",
	[ACCEL_SEQUENCE_STATE_EXEC_TASK] = "exec-task",
	[ACCEL_SEQUENCE_STATE_AWAIT_TASK] = "await-task",
	[ACCEL_SEQUENCE_STATE_COMPLETE_TASK] = "complete-task",
	[ACCEL_SEQUENCE_STATE_NEXT_TASK] = "next-task",
	[ACCEL_SEQUENCE_STATE_PUSH_DATA] = "push-data",
	[ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA] = "await-push-data",
	[ACCEL_SEQUENCE_STATE_DRIVER_EXEC_TASKS] = "driver-exec-tasks",
	[ACCEL_SEQUENCE_STATE_DRIVER_AWAIT_TASKS] = "driver-await-tasks",
	[ACCEL_SEQUENCE_STATE_DRIVER_COMPLETE_TASKS] = "driver-complete-tasks",
	[ACCEL_SEQUENCE_STATE_ERROR] = "error",
	[ACCEL_SEQUENCE_STATE_MAX] = "",
};

#define ACCEL_SEQUENCE_STATE_STRING(s) \
	(((s) >= ACCEL_SEQUENCE_STATE_INIT && (s) < ACCEL_SEQUENCE_STATE_MAX) \
	 ? g_seq_states[s] : "unknown")

/*
 * [한국어] struct accel_buffer — accel sequence가 동적으로 잡는 가상/바운스 버퍼
 *  디스크립터. seq가 끝나면 풀로 반환되며, len/buf는 iobuf 풀에서 받은 실제 메모리.
 *  iobuf 필드는 풀 부족 시 wait queue에 등록하기 위한 entry.
 */
struct accel_buffer {
	struct spdk_accel_sequence	*seq;
	/* [한국어] 이 버퍼를 소유한 시퀀스(역참조). seq가 풀로 돌아갈 때 버퍼도 함께 회수.
	 * 설정자: `accel_get_buf()` 호출 시점. 읽는 자: iobuf wait 콜백 등.
	 * 값 범위: 유효한 sequence 포인터 (NULL은 미사용 풀 상태).
	 * 동기화: 채널 단위 — 단일 reactor 스레드. */

	void				*buf;
	/* [한국어] 실제 데이터 메모리 포인터 (iobuf 풀에서 받은 host 메모리).
	 * 값이 ACCEL_BUFFER_BASE 비트가 켜진 토큰일 수도 있음(아직 매핑 전).
	 * 설정자: `spdk_iobuf_get` 또는 토큰 발급 시. 읽는 자: 시퀀스 push/pull/exec 단계. */

	uint64_t			len;
	/* [한국어] 버퍼 길이(바이트). 시퀀스 사용자가 요청한 크기와 동일.
	 * 설정자: `spdk_accel_get_buf()` / `accel_sequence_alloc_buf()`. */

	struct spdk_iobuf_entry		iobuf;
	/* [한국어] iobuf 풀의 대기 큐에 자기 자신을 등록할 때 쓰는 entry. cb_fn 포함.
	 * iobuf 풀이 비면 여기에 등록되어 풀에 여유가 생기면 콜백이 호출된다. */

	spdk_accel_sequence_get_buf_cb	cb_fn;
	/* [한국어] 사용자가 `spdk_accel_sequence_get_buf`로 직접 받을 때의 완료 콜백. */

	void				*cb_ctx;
	/* [한국어] 위 cb_fn에 함께 전달할 사용자 컨텍스트. */

	SLIST_ENTRY(accel_buffer)	link;
	/* [한국어] 채널의 buf_pool 또는 시퀀스의 bounce_bufs 리스트에 묶이는 SLIST 노드. */

	struct accel_io_channel		*ch;
	/* [한국어] 이 버퍼를 발급한 채널(반환 시 필요). */
};

/*
 * [한국어] struct accel_io_channel — 한 SPDK 스레드(reactor) 당 하나씩 존재하는
 *  채널 객체. opcode별로 하위 모듈의 io_channel을 들고 있으며, task/seq/buf 풀과
 *  iobuf 채널, 통계까지 통합 관리. lockless를 유지하기 위해 모든 운영은 이
 *  채널을 소유한 스레드에서만 수행되어야 한다.
 */
struct accel_io_channel {
	struct spdk_io_channel			*module_ch[SPDK_ACCEL_OPC_LAST];
	/* [한국어] opcode별로 매핑된 모듈의 io_channel. 디스패치 시 이 채널이 모듈
	 * 콜백에 그대로 전달된다. 값 범위: NULL은 해당 op가 매핑된 모듈이 없는 경우. */

	struct spdk_io_channel			*driver_channel;
	/* [한국어] driver(있을 시)의 채널. 시퀀스의 DRIVER_EXEC_TASKS 단계에서 사용. */

	void					*task_pool_base;
	/* [한국어] task 객체 풀의 베이스 메모리 — 채널 파괴 시 free 대상.
	 * 모듈마다 task 컨텍스트 크기가 다를 수 있어 `g_max_accel_module_size` 기준으로 할당. */

	struct spdk_accel_sequence		*seq_pool_base;
	/* [한국어] sequence 객체 풀의 베이스 메모리(채널 파괴 시 free). */

	struct accel_buffer			*buf_pool_base;
	/* [한국어] accel_buffer 디스크립터 풀의 베이스 메모리. */

	struct spdk_accel_task_aux_data		*task_aux_data_base;
	/* [한국어] task aux data(IOV 등 부가정보) 풀의 베이스. */

	STAILQ_HEAD(, spdk_accel_task)		task_pool;
	/* [한국어] 비어있는(사용 가능한) task 객체의 free list. _get_task가 pop, _put_task가 push. */

	SLIST_HEAD(, spdk_accel_task_aux_data)	task_aux_data_pool;
	/* [한국어] task aux data free list (IOV 슬롯이 많이 필요한 task에서 빌려 씀). */

	SLIST_HEAD(, spdk_accel_sequence)	seq_pool;
	/* [한국어] sequence 객체 free list. */

	SLIST_HEAD(, accel_buffer)		buf_pool;
	/* [한국어] accel_buffer 디스크립터 free list. */

	struct spdk_iobuf_channel		iobuf;
	/* [한국어] iobuf 풀 채널 — small/large 버퍼 할당 인터페이스. 모듈 인덱스 'accel'로 등록. */

	struct accel_stats			stats;
	/* [한국어] 이 채널의 통계 카운터들. RPC 응답 시 모든 채널의 stats를 누적해 글로벌로 만든다. */
};

/* [한국어] sequence에 묶인 task들의 TAILQ. 순서대로 실행되는 작업 체인. */
TAILQ_HEAD(accel_sequence_tasks, spdk_accel_task);

/*
 * [한국어] struct spdk_accel_sequence — 체이닝된 가속 작업(파이프라인).
 *  여러 op(예: pull → copy_crc32c → encrypt → push)을 한 번에 등록해
 *  메모리 도메인 변환과 함께 효율적으로 실행할 수 있다. 64바이트 정렬.
 */
struct spdk_accel_sequence {
	struct accel_io_channel			*ch;
	/* [한국어] 이 시퀀스를 소유한 채널(=실행 reactor). 모든 처리는 이 스레드에서만 수행. */

	struct accel_sequence_tasks		tasks;
	/* [한국어] 시퀀스에 등록된 task들의 큐(앞에서부터 실행). 사용자가 append_* API로 추가. */

	SLIST_HEAD(, accel_buffer)		bounce_bufs;
	/* [한국어] 이 시퀀스가 잡은 bounce buffer 리스트. 시퀀스 완료 시 일괄 반환. */

	int					status;
	/* [한국어] 시퀀스 종합 상태 (0=정상, 음수=errno). 어느 단계에서든 실패가 발생하면 채워진다. */

	/* state uses enum accel_sequence_state */
	uint8_t					state;
	/* [한국어] 현재 상태(enum accel_sequence_state). 1바이트로 압축 저장. */

	bool					in_process_sequence;
	/* [한국어] 재진입 방지 플래그. accel_process_sequence 호출 중에는 true로 세워
	 * 동일 시퀀스에 대한 재귀 진입을 차단. */

	spdk_accel_completion_cb		cb_fn;
	/* [한국어] 시퀀스 완료(또는 실패) 시 호출될 사용자 콜백. cb_arg와 함께 한 번만 호출. */

	void					*cb_arg;
	/* [한국어] 위 cb_fn에 전달될 사용자 컨텍스트. */

	SLIST_ENTRY(spdk_accel_sequence)	link;
	/* [한국어] 채널의 seq_pool free list 또는 다른 시퀀스 큐에 묶이는 SLIST 노드. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_accel_sequence) == 64, "invalid size");

/* [한국어] 채널 통계 카운터를 v만큼 가산하는 매크로. lockless(per-thread)이므로 단순 증가.
 *  event는 stats 구조체 안의 필드 경로(예: retry.task, task_outstanding) */
#define accel_update_stats(ch, event, v) \
	do { \
		(ch)->stats.event += (v); \
	} while (0)

/* [한국어] 위 매크로의 opcode별 변종. task->op_code를 인덱스로 operations[]에 가산. */
#define accel_update_task_stats(ch, task, event, v) \
	accel_update_stats(ch, operations[(task)->op_code].event, v)

/* [한국어] forward declaration — 시퀀스 내 task의 완료 콜백.
 *  task_complete가 task->seq를 보고 이쪽으로 위임한다. */
static inline void accel_sequence_task_cb(struct spdk_accel_sequence *seq,
		struct spdk_accel_task *task, int status);

/*
 * [한국어]
 * accel_sequence_set_state - 시퀀스의 상태(state field)를 갱신하고 디버그 로그 출력.
 *
 * @seq: 대상 시퀀스.
 * @state: 새 상태 (enum accel_sequence_state).
 *
 * ERROR 상태에 진입한 시퀀스는 다른 상태로 빠져나갈 수 없다는 invariant를 assert로 검증.
 * 호출 컨텍스트: 시퀀스 SM 본체(accel_process_sequence)나 task 콜백.
 */
static inline void
accel_sequence_set_state(struct spdk_accel_sequence *seq, enum accel_sequence_state state)
{
	/* [한국어] 상태 전이를 로그로 남겨 디버그 시 traceback 가능. */
	SPDK_DEBUGLOG(accel, "seq=%p, setting state: %s -> %s\n", seq,
		      ACCEL_SEQUENCE_STATE_STRING(seq->state), ACCEL_SEQUENCE_STATE_STRING(state));
	/* [한국어] ERROR → ERROR가 아닌 다른 상태로의 전이는 금지(흡수 상태). */
	assert(seq->state != ACCEL_SEQUENCE_STATE_ERROR || state == ACCEL_SEQUENCE_STATE_ERROR);
	seq->state = state;
}

/*
 * [한국어]
 * accel_sequence_set_fail - 시퀀스를 실패 상태로 표시하고 errno 저장.
 *
 * @seq: 대상 시퀀스.
 * @status: 실패 코드 (음수 errno, 0이면 assert 실패).
 *
 * 이후 SM은 ERROR 상태에서 정리(bounce buf 반환 등)를 거쳐 사용자 cb_fn(status)를 호출.
 */
static void
accel_sequence_set_fail(struct spdk_accel_sequence *seq, int status)
{
	/* [한국어] 흡수 상태로 전이. 이후 어떤 단계든 정리 후 cb_fn 호출로 종료. */
	accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_ERROR);
	/* [한국어] status==0은 성공을 의미하므로 실패 처리에 부합하지 않음 — 호출자 버그. */
	assert(status != 0);
	seq->status = status;
}

/*
 * [한국어]
 * spdk_accel_get_opc_module_name - opcode에 매핑된 모듈 이름 조회.
 *
 * @opcode: 조회할 가속 opcode (예: SPDK_ACCEL_OPC_COPY).
 * @module_name: [out] 모듈 이름 문자열 포인터를 받을 변수.
 * @return: 0 성공 / -EINVAL(잘못된 opcode) / -ENOENT(해당 op에 모듈 미할당).
 *
 * 공개 API. RPC 핸들러나 진단 도구가 사용한다.
 */
int
spdk_accel_get_opc_module_name(enum spdk_accel_opcode opcode, const char **module_name)
{
	/* [한국어] 범위 검증 — 잘못된 enum 값으로 g_modules_opc[] 인덱스 오버런 방지. */
	if (opcode >= SPDK_ACCEL_OPC_LAST) {
		/* invalid opcode */
		return -EINVAL;
	}

	/* [한국어] 매핑된 모듈이 있으면 그 이름을 반환, 없으면 -ENOENT. */
	if (g_modules_opc[opcode].module) {
		*module_name = g_modules_opc[opcode].module->name;
	} else {
		return -ENOENT;
	}

	return 0;
}

/*
 * [한국어]
 * _accel_for_each_module - 등록된 모든 accel 모듈을 순회하며 콜백 호출.
 *
 * @info: 사용자 정의 출력 컨테이너 (특히 w 필드는 호출자가 미리 설정).
 * @fn: 각 모듈마다 호출될 콜백. info에 채워진 name/ops/num_ops를 사용.
 *
 * RPC `accel_get_module_info`에서 사용. 모듈 등록은 초기화 단계에서만 일어나므로
 * lockless 순회가 안전하다.
 */
void
_accel_for_each_module(struct module_info *info, _accel_for_each_module_fn fn)
{
	struct spdk_accel_module_if *accel_module;
	enum spdk_accel_opcode opcode;
	int j = 0;

	/* [한국어] 등록된 모든 모듈을 차례로 방문. */
	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		/* [한국어] 각 모듈이 어떤 opcode를 지원하는지 모두 물어보고 ops[]에 채움. */
		for (opcode = 0; opcode < SPDK_ACCEL_OPC_LAST; opcode++) {
			if (accel_module->supports_opcode(opcode)) {
				info->ops[j] = opcode;
				j++;
			}
		}
		info->name = accel_module->name;
		info->num_ops = j;
		/* [한국어] 콜백 호출 — 사용자가 JSON 출력 등을 수행. */
		fn(info);
		/* [한국어] 다음 모듈을 위해 카운터 리셋. */
		j = 0;
	}
}

/*
 * [한국어]
 * spdk_accel_get_opcode_name - opcode 숫자를 사람이 읽을 수 있는 문자열로 변환.
 *
 * @opcode: 변환할 opcode.
 * @return: 문자열 포인터 또는 NULL(범위 외).
 *
 * 공개 API. 디버그 로그나 stats 출력에 사용.
 */
const char *
spdk_accel_get_opcode_name(enum spdk_accel_opcode opcode)
{
	/* [한국어] 유효 범위인 경우에만 정적 문자열 테이블을 인덱싱. */
	if (opcode < SPDK_ACCEL_OPC_LAST) {
		return g_opcode_strings[opcode];
	}

	return NULL;
}

/*
 * [한국어]
 * spdk_accel_assign_opc - 특정 opcode에 사용할 모듈 이름을 강제 지정 (override).
 *
 * @opcode: 지정 대상 opcode.
 * @name: 사용할 모듈 이름 (예: "software", "idxd").
 * @return: 0 성공 / -EINVAL(시작 후 호출, 잘못된 opcode) / -ENOMEM.
 *
 * 호출 시점: framework 시작(`g_modules_started==true`) 이전에만 허용.
 * 이후 모듈 선택 로직은 g_modules_opc_override[]를 보고 우선 적용한다.
 */
int
spdk_accel_assign_opc(enum spdk_accel_opcode opcode, const char *name)
{
	char *copy;

	/* [한국어] 시작 이후에는 매핑 변경을 금지(동적 변경 시 in-flight task가 망가질 수 있음). */
	if (g_modules_started == true) {
		/* we don't allow re-assignment once things have started */
		return -EINVAL;
	}

	if (opcode >= SPDK_ACCEL_OPC_LAST) {
		/* invalid opcode */
		return -EINVAL;
	}

	/* [한국어] 이름은 호출자의 수명과 분리되도록 strdup으로 우리 쪽에 복사. */
	copy = strdup(name);
	if (copy == NULL) {
		return -ENOMEM;
	}

	/* module selection will be validated after the framework starts. */
	/* [한국어] 기존 override가 있으면 누수 방지 위해 먼저 free. */
	free(g_modules_opc_override[opcode]);
	g_modules_opc_override[opcode] = copy;

	return 0;
}

/*
 * [한국어]
 * _get_task - 채널의 task 풀에서 task 객체 1개를 뽑아 cb를 세팅.
 *
 * @accel_ch: 호출 스레드의 채널.
 * @cb_fn / @cb_arg: 완료 시 호출될 콜백/컨텍스트.
 * @return: 사용 가능 task 포인터, 풀이 비었으면 NULL(retry 카운터 증가).
 *
 * inline static — 모든 submit 진입점이 가장 먼저 호출하는 함수.
 */
inline static struct spdk_accel_task *
_get_task(struct accel_io_channel *accel_ch, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *accel_task;

	/* [한국어] STAILQ free list의 head를 가져와 사용. */
	accel_task = STAILQ_FIRST(&accel_ch->task_pool);
	if (spdk_unlikely(accel_task == NULL)) {
		/* [한국어] 풀 소진 — 호출자는 백오프나 큐잉 필요. retry.task 통계 증가. */
		accel_update_stats(accel_ch, retry.task, 1);
		return NULL;
	}

	/* [한국어] outstanding 카운터 +1 (게이지). */
	accel_update_stats(accel_ch, task_outstanding, 1);
	STAILQ_REMOVE_HEAD(&accel_ch->task_pool, link);
	/* [한국어] 단독 task로 사용될 수 있도록 link.next를 명시적으로 NULL. */
	accel_task->link.stqe_next = NULL;

	/* [한국어] cb 정보와 채널 역참조를 task에 기록. */
	accel_task->cb_fn = cb_fn;
	accel_task->cb_arg = cb_arg;
	accel_task->accel_ch = accel_ch;
	accel_task->s.iovs = NULL;
	accel_task->d.iovs = NULL;

	return accel_task;
}

/*
 * [한국어]
 * _put_task - 사용 끝난 task를 채널의 풀에 되돌려놓고 outstanding -1.
 *
 * @ch: 채널, @task: 반환할 task. 단일 스레드에서만 호출 → 락 불필요.
 */
static void
_put_task(struct accel_io_channel *ch, struct spdk_accel_task *task)
{
	/* [한국어] head 삽입 — 캐시 hot한 객체가 다음에 다시 잡히도록 LIFO 동작. */
	STAILQ_INSERT_HEAD(&ch->task_pool, task, link);
	accel_update_stats(ch, task_outstanding, -1);
}

/*
 * [한국어]
 * spdk_accel_task_complete - 모듈이 task를 완료했을 때 호출하는 표준 진입점.
 *
 * @accel_task: 완료된 task.
 * @status: 완료 상태(0=성공, 음수=errno).
 *
 * - task 통계 갱신 (executed/failed/num_bytes).
 * - 시퀀스에 속한 task면 accel_sequence_task_cb로 위임 (개별 cb_fn 호출하지 않음).
 * - 그 외 단독 task면 aux 회수 → task 회수 → 사용자 cb_fn 호출.
 *
 * 회수 순서가 cb_fn 호출보다 앞이어야 cb_fn 내부에서 재귀적으로 새 task를 요청해도
 * 풀이 고갈되지 않는다(주석으로 명시된 invariant).
 */
void
spdk_accel_task_complete(struct spdk_accel_task *accel_task, int status)
{
	/* [한국어] task가 어느 채널에서 발행됐는지 역참조 — 통계/풀 반환의 대상 채널. */
	struct accel_io_channel		*accel_ch = accel_task->accel_ch;
	/* [한국어] 사용자 완료 콜백 포인터 (cb_fn(cb_arg, status) 형태로 호출). */
	spdk_accel_completion_cb	cb_fn;
	/* [한국어] 위 콜백에 전달할 사용자 컨텍스트. */
	void				*cb_arg;

	/* [한국어] 실행 완료 task 카운터 +1 (성공/실패 무관, 처리량 게이지). */
	accel_update_task_stats(accel_ch, accel_task, executed, 1);
	/* [한국어] 처리한 바이트 수 누적 — 대역폭 통계. */
	accel_update_task_stats(accel_ch, accel_task, num_bytes, accel_task->nbytes);
	/* [한국어] 음수 status는 errno(실패). 실패 카운터를 별도 증가. */
	if (spdk_unlikely(status != 0)) {
		accel_update_task_stats(accel_ch, accel_task, failed, 1);
	}

	/* [한국어] 이 task가 시퀀스(체이닝)에 속하면 개별 cb_fn을 직접 부르지 않고
	 *  시퀀스 상태머신 콜백으로 위임 — 시퀀스가 다음 단계를 진행하도록 한다. */
	if (accel_task->seq) {
		accel_sequence_task_cb(accel_task->seq, accel_task, status);
		return;
	}

	/* [한국어] 단독 task 경로 — 사용자 콜백/ctx를 지역 변수로 미리 백업한다.
	 *  (아래 _put_task 후 task가 재사용될 수 있으므로 미리 복사해 둠.) */
	cb_fn = accel_task->cb_fn;
	cb_arg = accel_task->cb_arg;

	/* [한국어] 이 task가 aux iovec 버퍼를 빌렸다면 먼저 aux 풀로 반환한다. */
	if (accel_task->has_aux) {
		/* [한국어] aux를 채널의 SLIST(LIFO) free 풀 head에 다시 삽입. */
		SLIST_INSERT_HEAD(&accel_ch->task_aux_data_pool, accel_task->aux, link);
		/* [한국어] dangling 방지 위해 포인터/플래그 클리어. */
		accel_task->aux = NULL;
		accel_task->has_aux = false;
	}

	/* We should put the accel_task into the list firstly in order to avoid
	 * the accel task list is exhausted when there is recursive call to
	 * allocate accel_task in user's call back function (cb_fn)
	 */
	/* [한국어] cb_fn 호출보다 먼저 task를 풀로 반환한다 — cb_fn 안에서 재귀적으로
	 *  새 task를 요청해도 풀이 고갈되지 않도록 보장하는 핵심 invariant. */
	_put_task(accel_ch, accel_task);

	/* [한국어] 사용자 완료 콜백 호출. 이 시점에서 accel_task는 이미 풀에 반환됨. */
	cb_fn(cb_arg, status);
}

/*
 * [한국어]
 * accel_submit_task - 준비된 task를 opcode에 매핑된 하위 모듈로 실제 제출한다.
 *
 * @accel_ch: task를 발행한 accel 채널.
 * @task: op_code/iov 등이 모두 채워진 제출 직전 task.
 * @return: 모듈 submit_tasks의 반환값(0=수락, 음수=errno).
 *
 * 모든 spdk_accel_submit_* 공개 API의 마지막 디스패치 지점. opcode→모듈 매핑
 * (g_modules_opc[])과 채널별 모듈 채널(module_ch[])을 조합해 모듈의 submit_tasks
 * 콜백으로 전달한다. 단일 SPDK 스레드에서만 호출(채널 owner) → 락 불필요.
 *
 * 호출 체인:
 *   spdk_accel_submit_copy/fill/... → [accel_submit_task] → module->submit_tasks()
 */
static inline int
accel_submit_task(struct accel_io_channel *accel_ch, struct spdk_accel_task *task)
{
	/* [한국어] 이 opcode를 처리할 하위 모듈의 채널(per-thread). */
	struct spdk_io_channel *module_ch = accel_ch->module_ch[task->op_code];
	/* [한국어] opcode→모듈 매핑 테이블에서 담당 모듈 객체를 꺼낸다. */
	struct spdk_accel_module_if *module = g_modules_opc[task->op_code].module;
	/* [한국어] 모듈 제출 결과 코드. */
	int rc;

	/* [한국어] 모듈에 task를 제출. HW 가속이면 DMA 디스크립터 enqueue,
	 *  SW면 즉시 계산 후 완료 콜백을 부를 수도 있음(동기/비동기 모두 가능). */
	rc = module->submit_tasks(module_ch, task);
	/* [한국어] 제출 자체가 실패하면 실패 통계를 증가(완료 경로를 안 타므로 여기서). */
	if (spdk_unlikely(rc != 0)) {
		accel_update_task_stats(accel_ch, task, failed, 1);
	}

	/* [한국어] 호출자(공개 API)는 이 rc를 그대로 사용자에게 반환한다. */
	return rc;
}

/*
 * [한국어]
 * accel_get_iovlen - iovec 배열의 전체 바이트 길이를 합산한다.
 *
 * @iovs: iovec 배열 시작 포인터.
 * @iovcnt: iovec 개수.
 * @return: 모든 iov_len의 합(총 바이트 수).
 *
 * 벡터(scatter-gather) 형태 입력의 nbytes를 계산하기 위한 단순 헬퍼.
 * crc32cv/copy_crc32cv 등 iovec 기반 공개 API에서 nbytes 산출에 사용된다.
 *
 * 호출 체인:
 *   spdk_accel_submit_crc32cv/... → [accel_get_iovlen]
 */
static inline uint64_t
accel_get_iovlen(struct iovec *iovs, uint32_t iovcnt)
{
	/* [한국어] 누적 합 초기화. */
	uint64_t result = 0;
	/* [한국어] 반복 인덱스. */
	uint32_t i;

	/* [한국어] 모든 iovec를 순회하며 길이를 더한다. */
	for (i = 0; i < iovcnt; ++i) {
		result += iovs[i].iov_len;
	}

	/* [한국어] 합산된 총 바이트 길이 반환. */
	return result;
}

/* [한국어] ACCEL_TASK_ALLOC_AUX_BUF — task에 aux iovec 버퍼를 채널 풀에서 빌려주는 매크로.
 *  copy/fill 등 내부 iovec 저장 공간이 필요한 op가 _get_task 직후 호출한다.
 *  aux 풀이 비면 치명적 오류로 보고 task를 즉시 반환하고 -ENOMEM으로 빠져나간다.
 *  (return을 포함하므로 함수처럼 동작 — 매크로 사용 함수에서 early-return된다.) */
#define ACCEL_TASK_ALLOC_AUX_BUF(task)						\
do {										\
        /* [한국어] aux 풀 head에서 빈 aux 버퍼 하나를 가리킨다(아직 제거 전). */ \
        (task)->aux = SLIST_FIRST(&(task)->accel_ch->task_aux_data_pool);	\
        /* [한국어] 풀이 비었으면 — task 수와 aux 수가 같게 사전할당되므로 정상 상황에선 발생 불가. */ \
        if (spdk_unlikely(!(task)->aux)) {					\
                SPDK_ERRLOG("Fatal problem, aux data was not allocated\n");	\
                /* [한국어] 방금 잡은 task를 풀로 즉시 반환하고 누수 방지. */	\
                _put_task(task->accel_ch, task);				\
                /* [한국어] 디버그 빌드에서 즉시 abort — 설계 위반을 조기 발견. */ \
                assert(0);							\
                /* [한국어] 호출 함수에서 -ENOMEM으로 early-return. */		\
                return -ENOMEM;							\
        }									\
        /* [한국어] 실제로 head를 제거해 이 aux 버퍼를 소유한다. */		\
        SLIST_REMOVE_HEAD(&(task)->accel_ch->task_aux_data_pool, link);		\
        /* [한국어] 완료 시 aux를 풀로 되돌려야 함을 표시. */			\
        (task)->has_aux = true;							\
} while (0)

/* Accel framework public API for copy function */
/*
 * [한국어]
 * spdk_accel_submit_copy - dst로 src를 nbytes만큼 복사하는 가속 작업을 발행한다.
 *
 * @ch: 호출 스레드의 accel io_channel(spdk_accel_get_io_channel로 획득).
 * @dst: 목적지 호스트 버퍼.
 * @src: 원본 호스트 버퍼.
 * @nbytes: 복사 바이트 수.
 * @cb_fn/@cb_arg: 완료 시 호출될 사용자 콜백/컨텍스트.
 * @return: 0=수락(완료는 비동기 cb_fn), -ENOMEM=task 풀 소진.
 *
 * 가장 기본적인 copy op. task를 풀에서 빌리고 aux iovec를 단일 segment로 구성한 뒤
 * SPDK_ACCEL_OPC_COPY로 모듈에 제출한다. 채널 owner 스레드에서만 호출.
 *
 * 호출 체인:
 *   사용자 → [spdk_accel_submit_copy] → accel_submit_task → module->submit_tasks
 */
int
spdk_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src,
		       uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] spdk_io_channel에서 accel 전용 컨텍스트(채널 풀 등)를 꺼낸다. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	/* [한국어] 이번에 사용할 task 디스크립터. */
	struct spdk_accel_task *accel_task;

	/* [한국어] 채널 풀에서 task 하나를 빌리고 cb 정보를 기록. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	/* [한국어] 풀 소진이면 호출자에게 백오프하라고 -ENOMEM 반환. */
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	/* [한국어] copy는 내부 iovec 저장공간이 필요하므로 aux 버퍼를 빌린다(실패 시 매크로가 early-return). */
	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	/* [한국어] src/dst iovec 포인터를 aux 버퍼의 지정 슬롯으로 연결. */
	accel_task->s.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	/* [한국어] 목적지 단일 segment: 주소/길이 채우고 iovcnt=1. */
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	/* [한국어] 원본 단일 segment 동일하게 구성. */
	accel_task->s.iovs[0].iov_base = src;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	/* [한국어] 전체 작업 크기 기록(통계/모듈용). */
	accel_task->nbytes = nbytes;
	/* [한국어] opcode를 COPY로 지정 → 디스패치 테이블이 copy 담당 모듈로 보냄. */
	accel_task->op_code = SPDK_ACCEL_OPC_COPY;
	/* [한국어] 호스트 메모리이므로 메모리 도메인 없음(NULL = 일반 가상주소). */
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	/* [한국어] 최종 제출. */
	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for dual cast copy function */
/*
 * [한국어]
 * spdk_accel_submit_dualcast - 하나의 src를 두 목적지(dst1/dst2)로 동시 복사한다.
 *
 * @ch: accel io_channel. @dst1/@dst2: 두 목적지(둘 다 4K 정렬 필수).
 * @src: 원본. @nbytes: 바이트 수. @cb_fn/@cb_arg: 완료 콜백/ctx.
 * @return: 0=수락, -EINVAL=정렬 위반, -ENOMEM=풀 소진.
 *
 * 미러링/이중화 기록에 쓰이는 op. HW 가속기(DSA 등)는 dualcast에 4K 정렬을
 * 요구하므로 진입 시 정렬을 검사한다. 채널 owner 스레드 전용.
 *
 * 호출 체인:
 *   사용자 → [spdk_accel_submit_dualcast] → accel_submit_task → module->submit_tasks
 */
int
spdk_accel_submit_dualcast(struct spdk_io_channel *ch, void *dst1,
			   void *dst2, void *src, uint64_t nbytes,
			   spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 채널 컨텍스트와 task 디스크립터. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] 두 목적지 중 하나라도 4K 정렬이 아니면 HW 요구사항 위반 → -EINVAL. */
	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	/* [한국어] task 풀에서 할당, 소진 시 -ENOMEM. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	/* [한국어] src/dst1/dst2 세 iovec 저장공간이 필요 → aux 버퍼 할당. */
	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	/* [한국어] 세 iovec 포인터를 aux의 SRC/DST/DST2 슬롯으로 연결. */
	accel_task->s.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d2.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST2];
	/* [한국어] 첫 번째 목적지 단일 segment. */
	accel_task->d.iovs[0].iov_base = dst1;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	/* [한국어] 두 번째 목적지 단일 segment. */
	accel_task->d2.iovs[0].iov_base = dst2;
	accel_task->d2.iovs[0].iov_len = nbytes;
	accel_task->d2.iovcnt = 1;
	/* [한국어] 원본 단일 segment. */
	accel_task->s.iovs[0].iov_base = src;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	/* [한국어] 작업 크기/opcode/도메인 지정 후 제출. */
	accel_task->nbytes = nbytes;
	accel_task->op_code = SPDK_ACCEL_OPC_DUALCAST;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for compare function */

/*
 * [한국어]
 * spdk_accel_submit_compare - 두 버퍼(src1/src2)를 nbytes만큼 바이트 비교한다.
 *
 * @ch: accel io_channel. @src1/@src2: 비교할 두 버퍼. @nbytes: 비교 길이.
 * @cb_fn/@cb_arg: 완료 콜백/ctx. @return: 0=수락, -ENOMEM=풀 소진.
 *
 * 완료 status로 일치 여부를 보고(불일치 시 모듈이 비-0 status). dedup/검증 경로에서 사용.
 * 채널 owner 스레드 전용. 호출 체인은 다른 submit과 동일.
 */
int
spdk_accel_submit_compare(struct spdk_io_channel *ch, void *src1,
			  void *src2, uint64_t nbytes, spdk_accel_completion_cb cb_fn,
			  void *cb_arg)
{
	/* [한국어] 채널 컨텍스트와 task 디스크립터. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] task 할당, 소진 시 -ENOMEM. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	/* [한국어] 두 원본 iovec(SRC/SRC2)를 담을 aux 버퍼 할당. */
	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	/* [한국어] 두 원본 iovec 포인터를 aux 슬롯으로 연결. */
	accel_task->s.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->s2.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_SRC2];
	/* [한국어] 첫 번째 원본 단일 segment. */
	accel_task->s.iovs[0].iov_base = src1;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	/* [한국어] 두 번째 원본 단일 segment. */
	accel_task->s2.iovs[0].iov_base = src2;
	accel_task->s2.iovs[0].iov_len = nbytes;
	accel_task->s2.iovcnt = 1;
	/* [한국어] 크기/opcode(COMPARE)/도메인 후 제출. */
	accel_task->nbytes = nbytes;
	accel_task->op_code = SPDK_ACCEL_OPC_COMPARE;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for fill function */
/*
 * [한국어]
 * spdk_accel_submit_fill - dst를 1바이트 패턴 fill로 nbytes만큼 채운다(memset 가속).
 *
 * @ch: accel io_channel. @dst: 목적지 버퍼. @fill: 채울 바이트 값.
 * @nbytes: 길이. @cb_fn/@cb_arg: 완료 콜백/ctx. @return: 0=수락, -ENOMEM.
 *
 * fill_pattern을 8바이트로 복제해 모듈이 워드 단위로 빠르게 채우게 한다.
 * 디스크 초기화/zeroing 경로에서 사용. 채널 owner 스레드 전용.
 */
int
spdk_accel_submit_fill(struct spdk_io_channel *ch, void *dst,
		       uint8_t fill, uint64_t nbytes,
		       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 채널 컨텍스트와 task 디스크립터. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] task 할당, 소진 시 -ENOMEM. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	/* [한국어] 목적지 iovec만 필요 → aux 버퍼 할당. */
	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	/* [한국어] 목적지 단일 segment 구성. */
	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->nbytes = nbytes;
	/* [한국어] 1바이트 패턴을 8바이트(uint64_t)로 복제 — 모듈이 64비트 워드 store로 채울 수 있게. */
	memset(&accel_task->fill_pattern, fill, sizeof(uint64_t));
	/* [한국어] opcode(FILL)/도메인 후 제출. */
	accel_task->op_code = SPDK_ACCEL_OPC_FILL;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for CRC-32C function */
/*
 * [한국어]
 * spdk_accel_submit_crc32c - 단일 버퍼 src의 CRC-32C(Castagnoli)를 계산한다.
 *
 * @ch: accel io_channel. @crc_dst: 결과 CRC를 저장할 위치. @src: 입력 버퍼.
 * @seed: 초기 CRC 시드(연쇄 계산용). @nbytes: 입력 길이.
 * @cb_fn/@cb_arg: 완료 콜백/ctx. @return: 0=수락, -ENOMEM.
 *
 * 데이터 무결성 체크섬. SSE4.2/HW 가속 또는 SW 폴백으로 계산되며 결과는 crc_dst에 기록.
 * 채널 owner 스레드 전용.
 */
int
spdk_accel_submit_crc32c(struct spdk_io_channel *ch, uint32_t *crc_dst,
			 void *src, uint32_t seed, uint64_t nbytes, spdk_accel_completion_cb cb_fn,
			 void *cb_arg)
{
	/* [한국어] 채널 컨텍스트와 task 디스크립터. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] task 할당, 소진 시 -ENOMEM. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	/* [한국어] 원본 iovec만 필요 → aux 버퍼 할당. */
	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	/* [한국어] 원본 단일 segment 구성. */
	accel_task->s.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->s.iovs[0].iov_base = src;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	accel_task->nbytes = nbytes;
	/* [한국어] 결과 CRC 출력 포인터와 초기 시드 기록. */
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	/* [한국어] opcode(CRC32C)/도메인 후 제출. */
	accel_task->op_code = SPDK_ACCEL_OPC_CRC32C;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for chained CRC-32C function */
/*
 * [한국어]
 * spdk_accel_submit_crc32cv - scatter-gather(iovec) 입력의 CRC-32C를 계산한다.
 *
 * @ch: accel io_channel. @crc_dst: 결과 저장 위치. @iov/@iov_cnt: 입력 벡터.
 * @seed: 초기 시드. @cb_fn/@cb_arg: 완료 콜백/ctx.
 * @return: 0=수락, -EINVAL=iov NULL 또는 cnt 0, -ENOMEM=풀 소진.
 *
 * 단일 버퍼 버전과 달리 사용자 iovec를 직접 참조하므로 aux 버퍼가 불필요하다.
 * nbytes는 accel_get_iovlen으로 합산. 채널 owner 스레드 전용.
 */
int
spdk_accel_submit_crc32cv(struct spdk_io_channel *ch, uint32_t *crc_dst,
			  struct iovec *iov, uint32_t iov_cnt, uint32_t seed,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 채널 컨텍스트와 task 디스크립터. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] iovec 포인터 자체가 NULL이면 사용자 인자 오류. */
	if (iov == NULL) {
		SPDK_ERRLOG("iov should not be NULL");
		return -EINVAL;
	}

	/* [한국어] iovec 개수가 0이면 의미 없는 요청 → 거부. */
	if (!iov_cnt) {
		SPDK_ERRLOG("iovcnt should not be zero value\n");
		return -EINVAL;
	}

	/* [한국어] task 할당 — 이 경로는 풀 소진을 치명적으로 보고 assert. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		SPDK_ERRLOG("no memory\n");
		assert(0);
		return -ENOMEM;
	}

	/* [한국어] 사용자 iovec를 그대로 원본으로 사용(aux 불필요 — 복사 없음). */
	accel_task->s.iovs = iov;
	accel_task->s.iovcnt = iov_cnt;
	/* [한국어] 전체 길이는 iovec 합산으로 산출. */
	accel_task->nbytes = accel_get_iovlen(iov, iov_cnt);
	/* [한국어] 결과 포인터/시드/opcode/도메인 지정 후 제출. */
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->op_code = SPDK_ACCEL_OPC_CRC32C;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for copy with CRC-32C function */
/*
 * [한국어]
 * spdk_accel_submit_copy_crc32c - src→dst 복사와 동시에 CRC-32C를 한 번에 계산한다.
 *
 * @ch: accel io_channel. @dst: 목적지. @src: 원본. @crc_dst: CRC 결과 위치.
 * @seed: 초기 시드. @nbytes: 길이. @cb_fn/@cb_arg: 완료 콜백/ctx.
 * @return: 0=수락, -ENOMEM=풀 소진.
 *
 * 복사+체크섬 융합 op — 데이터를 두 번 읽지 않고 단일 패스로 처리해 대역폭을 절감한다
 * (저장 후 무결성 검증 경로). 채널 owner 스레드 전용.
 */
int
spdk_accel_submit_copy_crc32c(struct spdk_io_channel *ch, void *dst,
			      void *src, uint32_t *crc_dst, uint32_t seed, uint64_t nbytes,
			      spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 채널 컨텍스트와 task 디스크립터. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] task 할당, 소진 시 -ENOMEM. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	/* [한국어] src/dst iovec를 담을 aux 버퍼 할당. */
	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	/* [한국어] src/dst 단일 segment 구성(copy와 동일). */
	accel_task->s.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->s.iovs[0].iov_base = src;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	accel_task->nbytes = nbytes;
	/* [한국어] CRC 출력/시드 추가 + opcode(COPY_CRC32C) 지정 후 제출. */
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->op_code = SPDK_ACCEL_OPC_COPY_CRC32C;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for chained copy + CRC-32C function */
/*
 * [한국어]
 * spdk_accel_submit_copy_crc32cv - scatter-gather 원본을 단일 dst로 복사하며 CRC-32C 계산.
 *
 * @ch: accel io_channel. @dst: 단일 목적지 버퍼. @src_iovs/@iov_cnt: 원본 벡터.
 * @crc_dst: CRC 결과 위치. @seed: 시드. @cb_fn/@cb_arg: 완료 콜백/ctx.
 * @return: 0=수락, -EINVAL=잘못된 iov, -ENOMEM=풀 소진.
 *
 * copy_crc32c의 벡터 입력 버전. 목적지 iovec는 aux에서 만들고, 원본은 사용자 iovec 직접 참조.
 * 채널 owner 스레드 전용.
 */
int
spdk_accel_submit_copy_crc32cv(struct spdk_io_channel *ch, void *dst,
			       struct iovec *src_iovs, uint32_t iov_cnt, uint32_t *crc_dst,
			       uint32_t seed, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 채널 컨텍스트, task, 산출할 총 길이. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	uint64_t nbytes;

	/* [한국어] 원본 iovec 포인터 NULL 검사. */
	if (src_iovs == NULL) {
		SPDK_ERRLOG("iov should not be NULL");
		return -EINVAL;
	}

	/* [한국어] iovec 개수 0 검사. */
	if (!iov_cnt) {
		SPDK_ERRLOG("iovcnt should not be zero value\n");
		return -EINVAL;
	}

	/* [한국어] task 할당 — 풀 소진을 치명적으로 보고 assert. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		SPDK_ERRLOG("no memory\n");
		assert(0);
		return -ENOMEM;
	}

	/* [한국어] 목적지 segment 길이 = 원본 iovec 합산 길이. */
	nbytes = accel_get_iovlen(src_iovs, iov_cnt);

	/* [한국어] 목적지 단일 iovec를 위한 aux 버퍼 할당. */
	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	/* [한국어] 목적지 단일 segment 구성. */
	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	/* [한국어] 원본은 사용자 iovec 직접 참조. */
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = iov_cnt;
	accel_task->nbytes = nbytes;
	/* [한국어] CRC 출력/시드/opcode 지정 후 제출. */
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->op_code = SPDK_ACCEL_OPC_COPY_CRC32C;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/*
 * [한국어]
 * spdk_accel_get_compress_level_range - 압축 모듈이 지원하는 레벨 범위를 질의한다.
 *
 * @comp_algo: 질의할 압축 알고리즘(DEFLATE 등).
 * @min_level/@max_level: 출력 — 모듈이 지원하는 최소/최대 압축 레벨.
 * @return: 0=성공, -ENOTSUP=모듈 미구현.
 *
 * 사용자가 압축 레벨을 안전하게 클램프하기 위한 capability 질의. COMPRESS opcode에
 * 매핑된 모듈의 선택적 콜백을 위임 호출한다. 모듈 미구현 시 -ENOTSUP.
 */
int
spdk_accel_get_compress_level_range(enum spdk_accel_comp_algo comp_algo,
				    uint32_t *min_level, uint32_t *max_level)
{
	/* [한국어] COMPRESS opcode를 담당하는 모듈을 매핑 테이블에서 꺼낸다. */
	struct spdk_accel_module_if *module = g_modules_opc[SPDK_ACCEL_OPC_COMPRESS].module;

	/* [한국어] 모듈이 이 선택적 콜백을 구현하지 않았으면 미지원으로 보고. */
	if (module->get_compress_level_range == NULL) {
		SPDK_ERRLOG("Module %s doesn't implement callback fn get_compress_level_range.\n", module->name);
		return -ENOTSUP;
	}

	/* [한국어] 모듈 콜백 위임 — 결과를 min/max_level에 채워준다. */
	return module->get_compress_level_range(comp_algo, min_level, max_level);
}

/*
 * [한국어]
 * _accel_check_comp_algo - 현재 COMPRESS 모듈이 지정 압축 알고리즘을 지원하는지 검증.
 *
 * @comp_algo: 검증할 알고리즘.
 * @return: 0=지원, -ENOTSUP=미지원 또는 검증 콜백 부재.
 *
 * submit_compress_ext/decompress_ext의 진입 가드. 모듈이 algo 지원 콜백을 제공하지
 * 않거나 false를 반환하면 미지원으로 처리한다. 채널 owner 스레드 전용.
 */
static int
_accel_check_comp_algo(enum spdk_accel_comp_algo comp_algo)
{
	/* [한국어] COMPRESS 담당 모듈 조회. */
	struct spdk_accel_module_if *module = g_modules_opc[SPDK_ACCEL_OPC_COMPRESS].module;

	/* [한국어] 검증 콜백이 없거나 알고리즘 미지원이면 -ENOTSUP. */
	if (!module->compress_supports_algo || !module->compress_supports_algo(comp_algo)) {
		SPDK_ERRLOG("Module %s doesn't support compression algo %d\n", module->name, comp_algo);
		return -ENOTSUP;
	}

	/* [한국어] 지원 확인. */
	return 0;
}

/*
 * [한국어]
 * spdk_accel_submit_compress_ext - 알고리즘/레벨을 지정해 src를 압축하여 dst에 쓴다.
 *
 * @ch: accel io_channel. @dst: 압축 출력 버퍼. @nbytes: dst 버퍼 용량(원본 크기).
 * @src_iovs/@src_iovcnt: 압축할 원본 벡터. @comp_algo: 알고리즘. @comp_level: 레벨.
 * @output_size: 출력 — 실제 압축된 바이트 수가 기록될 위치.
 * @cb_fn/@cb_arg: 완료 콜백/ctx. @return: 0=수락, -ENOTSUP=algo 미지원, -ENOMEM.
 *
 * 압축률은 데이터 의존적이므로 실제 출력 길이를 output_size로 비동기 보고한다.
 * 목적지는 단일 버퍼(aux iovec), 원본은 사용자 벡터 직접 참조. 채널 owner 스레드 전용.
 */
int
spdk_accel_submit_compress_ext(struct spdk_io_channel *ch, void *dst, uint64_t nbytes,
			       struct iovec *src_iovs, size_t src_iovcnt,
			       enum spdk_accel_comp_algo comp_algo, uint32_t comp_level,
			       uint32_t *output_size, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 채널 컨텍스트, task, algo 검증 결과. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	int rc;

	/* [한국어] 모듈이 지정 압축 알고리즘을 지원하는지 먼저 검증. */
	rc = _accel_check_comp_algo(comp_algo);
	if (spdk_unlikely(rc != 0)) {
		return rc;
	}

	/* [한국어] task 할당, 소진 시 -ENOMEM. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	/* [한국어] 단일 목적지 iovec를 위한 aux 버퍼 할당. */
	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	/* [한국어] 목적지 단일 segment 구성. */
	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	/* [한국어] 실제 압축 결과 길이를 받을 출력 포인터. */
	accel_task->output_size = output_size;
	/* [한국어] 원본은 사용자 벡터 직접 참조. */
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->nbytes = nbytes;
	/* [한국어] opcode(COMPRESS)/도메인/알고리즘/레벨 지정 후 제출. */
	accel_task->op_code = SPDK_ACCEL_OPC_COMPRESS;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;
	accel_task->comp.algo = comp_algo;
	accel_task->comp.level = comp_level;

	return accel_submit_task(accel_ch, accel_task);
}

/*
 * [한국어]
 * spdk_accel_submit_decompress_ext - 알고리즘을 지정해 src를 dst로 복원(압축 해제)한다.
 *
 * @ch: accel io_channel. @dst_iovs/@dst_iovcnt: 복원 출력 벡터(가변 크기 허용).
 * @src_iovs/@src_iovcnt: 압축 원본 벡터. @decomp_algo: 복원 알고리즘.
 * @output_size: 출력 — 실제 복원된 바이트 수. @cb_fn/@cb_arg: 완료 콜백/ctx.
 * @return: 0=수락, -ENOTSUP=algo 미지원, -ENOMEM.
 *
 * 복원은 출력 크기를 사전에 알 수 없으므로 dst를 iovec 벡터로 받고 실제 길이는 output_size로
 * 보고한다. 원본/목적지 모두 사용자 벡터 직접 참조(aux 불필요). 채널 owner 스레드 전용.
 */
int
spdk_accel_submit_decompress_ext(struct spdk_io_channel *ch, struct iovec *dst_iovs,
				 size_t dst_iovcnt, struct iovec *src_iovs, size_t src_iovcnt,
				 enum spdk_accel_comp_algo decomp_algo, uint32_t *output_size,
				 spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 채널 컨텍스트, task, algo 검증 결과. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	int rc;

	/* [한국어] 복원 알고리즘 지원 여부 검증. */
	rc = _accel_check_comp_algo(decomp_algo);
	if (spdk_unlikely(rc != 0)) {
		return rc;
	}

	/* [한국어] task 할당, 소진 시 -ENOMEM. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	/* [한국어] 실제 복원 길이를 받을 출력 포인터. */
	accel_task->output_size = output_size;
	/* [한국어] 원본/목적지 모두 사용자 벡터 직접 참조. */
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	/* [한국어] 입력 길이는 압축 원본 iovec 합산. */
	accel_task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	/* [한국어] opcode(DECOMPRESS)/도메인/알고리즘 지정 후 제출. */
	accel_task->op_code = SPDK_ACCEL_OPC_DECOMPRESS;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;
	accel_task->comp.algo = decomp_algo;

	return accel_submit_task(accel_ch, accel_task);
}

/*
 * [한국어]
 * spdk_accel_submit_compress - DEFLATE/레벨1 기본값으로 압축하는 간편 래퍼.
 *
 * @ch/@dst/@nbytes/@src_iovs/@src_iovcnt/@output_size/@cb_fn/@cb_arg: ext 버전과 동일.
 * @return: ext 버전과 동일.
 *
 * 알고리즘/레벨을 명시하지 않는 사용자를 위한 호환 진입점. ext 버전에 기본 알고리즘
 * (DEFLATE)과 레벨 1을 채워 위임한다.
 */
int
spdk_accel_submit_compress(struct spdk_io_channel *ch, void *dst, uint64_t nbytes,
			   struct iovec *src_iovs, size_t src_iovcnt, uint32_t *output_size,
			   spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 기본 알고리즘 DEFLATE + 레벨 1로 ext 버전 위임. */
	return spdk_accel_submit_compress_ext(ch, dst, nbytes, src_iovs, src_iovcnt,
					      SPDK_ACCEL_COMP_ALGO_DEFLATE, 1, output_size, cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_accel_submit_decompress - DEFLATE 기본값으로 복원하는 간편 래퍼.
 *
 * @ch/@dst_iovs/@dst_iovcnt/@src_iovs/@src_iovcnt/@output_size/@cb_fn/@cb_arg: ext와 동일.
 * @return: ext 버전과 동일.
 *
 * 알고리즘을 명시하지 않는 사용자를 위한 호환 진입점. ext 버전에 기본 DEFLATE를 채워 위임.
 */
int
spdk_accel_submit_decompress(struct spdk_io_channel *ch, struct iovec *dst_iovs,
			     size_t dst_iovcnt, struct iovec *src_iovs, size_t src_iovcnt,
			     uint32_t *output_size, spdk_accel_completion_cb cb_fn,
			     void *cb_arg)
{
	/* [한국어] 기본 알고리즘 DEFLATE로 ext 버전 위임. */
	return spdk_accel_submit_decompress_ext(ch, dst_iovs, dst_iovcnt, src_iovs, src_iovcnt,
						SPDK_ACCEL_COMP_ALGO_DEFLATE, output_size, cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_accel_submit_encrypt - 암호 키와 IV로 src를 dst로 암호화한다(AES-XTS 등).
 *
 * @ch: accel io_channel. @key: 등록된 spdk_accel_crypto_key.
 * @dst_iovs/@dst_iovcnt: 암호문 출력 벡터. @src_iovs/@src_iovcnt: 평문 입력 벡터.
 * @iv: 초기화 벡터(XTS의 tweak — 보통 LBA 기반). @block_size: 암호 블록 크기.
 * @cb_fn/@cb_arg: 완료 콜백/ctx. @return: 0=수락, -EINVAL=인자 오류, -ENOMEM.
 *
 * 디스크 암호화(bdev_crypto) 경로의 핵심 op. 키/블록크기/벡터 유효성을 먼저 검증한다.
 * 채널 owner 스레드 전용.
 */
int
spdk_accel_submit_encrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  uint64_t iv, uint32_t block_size,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 채널 컨텍스트와 task. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] 암호화 필수 인자(목적지/원본 벡터, 키, 블록 크기)가 하나라도 비면 -EINVAL. */
	if (spdk_unlikely(!dst_iovs || !dst_iovcnt || !src_iovs || !src_iovcnt || !key || !block_size)) {
		return -EINVAL;
	}

	/* [한국어] task 할당, 소진 시 -ENOMEM. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	/* [한국어] 사용할 암호 키 객체 연결(AES-XTS DEK 등). */
	accel_task->crypto_key = key;
	/* [한국어] 평문 원본/암호문 목적지 모두 사용자 벡터 직접 참조. */
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	/* [한국어] 전체 길이 = 원본 iovec 합산. */
	accel_task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	/* [한국어] IV(XTS tweak)와 암호 블록 크기 기록. */
	accel_task->iv = iv;
	accel_task->block_size = block_size;
	/* [한국어] opcode(ENCRYPT)/도메인 지정 후 제출. */
	accel_task->op_code = SPDK_ACCEL_OPC_ENCRYPT;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/*
 * [한국어]
 * spdk_accel_submit_decrypt - 암호 키와 IV로 src(암호문)를 dst(평문)로 복호화한다.
 *
 * @ch/@key/@dst_iovs/@dst_iovcnt/@src_iovs/@src_iovcnt/@iv/@block_size/@cb_fn/@cb_arg:
 *   encrypt 버전과 동일한 의미(방향만 반대).
 * @return: 0=수락, -EINVAL=인자 오류, -ENOMEM.
 *
 * 디스크 읽기 시 복호화 경로. encrypt와 동일 구조이며 opcode만 DECRYPT. 채널 owner 스레드 전용.
 */
int
spdk_accel_submit_decrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  uint64_t iv, uint32_t block_size,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 채널 컨텍스트와 task. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] 복호화 필수 인자 검증(encrypt와 동일). */
	if (spdk_unlikely(!dst_iovs || !dst_iovcnt || !src_iovs || !src_iovcnt || !key || !block_size)) {
		return -EINVAL;
	}

	/* [한국어] task 할당, 소진 시 -ENOMEM. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	/* [한국어] 키/벡터/길이/IV/블록크기 설정(encrypt와 동일, 방향만 반대). */
	accel_task->crypto_key = key;
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	accel_task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	accel_task->iv = iv;
	accel_task->block_size = block_size;
	/* [한국어] opcode(DECRYPT)/도메인 지정 후 제출. */
	accel_task->op_code = SPDK_ACCEL_OPC_DECRYPT;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/*
 * [한국어]
 * spdk_accel_submit_xor - 여러 원본을 비트 XOR하여 dst에 쓴다(RAID 패리티 계산).
 *
 * @ch: accel io_channel. @dst: 결과(패리티) 버퍼. @sources: 원본 포인터 배열.
 * @nsrcs: 원본 개수. @nbytes: 각 원본/결과의 바이트 길이.
 * @cb_fn/@cb_arg: 완료 콜백/ctx. @return: 0=수락, -ENOMEM.
 *
 * RAID5/6 패리티 생성이나 복구에 사용. 모든 원본을 같은 길이로 XOR한다.
 * 목적지 iovec는 aux에서 만들고 원본 배열은 nsrcs로 전달. 채널 owner 스레드 전용.
 */
int
spdk_accel_submit_xor(struct spdk_io_channel *ch, void *dst, void **sources, uint32_t nsrcs,
		      uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 채널 컨텍스트와 task. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] task 할당, 소진 시 -ENOMEM. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	/* [한국어] 목적지 단일 iovec를 위한 aux 버퍼 할당. */
	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	/* [한국어] 목적지 iovec 슬롯 연결. */
	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	/* [한국어] 원본 포인터 배열과 개수를 nsrcs 구조에 저장(XOR 전용 표현). */
	accel_task->nsrcs.srcs = sources;
	accel_task->nsrcs.cnt = nsrcs;
	/* [한국어] 목적지 단일 segment 구성. */
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->nbytes = nbytes;
	/* [한국어] opcode(XOR)/도메인 지정 후 제출. */
	accel_task->op_code = SPDK_ACCEL_OPC_XOR;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/*
 * [한국어]
 * spdk_accel_submit_dif_verify - 블록 데이터의 T10 DIF 보호정보(PI)를 검증한다.
 *
 * @ch: accel io_channel. @iovs/@iovcnt: 검증할 데이터+PI가 인터리브된 벡터.
 * @num_blocks: 블록 수. @ctx: DIF 컨텍스트(블록크기/guard 모드/ref tag 정책).
 * @err: 출력 — 검증 실패 시 위치/유형 보고. @cb_fn/@cb_arg: 완료 콜백/ctx.
 * @return: 0=수락, -ENOMEM. 실제 검증 결과(불일치)는 status/err로 보고.
 *
 * 저장된 데이터의 무결성(Guard CRC/Application Tag/Reference Tag) 검증 경로.
 * nbytes는 num_blocks*block_size로 산출. 채널 owner 스레드 전용.
 */
int
spdk_accel_submit_dif_verify(struct spdk_io_channel *ch,
			     struct iovec *iovs, size_t iovcnt, uint32_t num_blocks,
			     const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
			     spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 채널 컨텍스트와 task. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] task 할당, 소진 시 -ENOMEM. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	/* [한국어] 검증 대상 데이터(원본) 벡터 직접 참조. */
	accel_task->s.iovs = iovs;
	accel_task->s.iovcnt = iovcnt;
	/* [한국어] DIF 정책/에러 출력/블록 수를 dif 서브구조에 기록. */
	accel_task->dif.ctx = ctx;
	accel_task->dif.err = err;
	accel_task->dif.num_blocks = num_blocks;
	/* [한국어] 총 바이트 = 블록 수 × 블록당 바이트(PI 포함 블록 크기). */
	accel_task->nbytes = num_blocks * ctx->block_size;
	/* [한국어] opcode(DIF_VERIFY)/도메인 지정 후 제출. */
	accel_task->op_code = SPDK_ACCEL_OPC_DIF_VERIFY;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/*
 * [한국어]
 * spdk_accel_submit_dif_generate - 블록 데이터에 T10 DIF 보호정보(PI)를 생성·삽입한다.
 *
 * @ch: accel io_channel. @iovs/@iovcnt: in-place로 PI를 채울 데이터 벡터.
 * @num_blocks: 블록 수. @ctx: DIF 컨텍스트. @cb_fn/@cb_arg: 완료 콜백/ctx.
 * @return: 0=수락, -ENOMEM.
 *
 * 쓰기 전 무결성 태그 생성 경로(verify의 반대). 같은 버퍼에 Guard/Tag를 채운다.
 * 채널 owner 스레드 전용.
 */
int
spdk_accel_submit_dif_generate(struct spdk_io_channel *ch,
			       struct iovec *iovs, size_t iovcnt, uint32_t num_blocks,
			       const struct spdk_dif_ctx *ctx,
			       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] 채널 컨텍스트와 task. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] task 할당, 소진 시 -ENOMEM. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	/* [한국어] PI를 채울 데이터 벡터 직접 참조(in-place). */
	accel_task->s.iovs = iovs;
	accel_task->s.iovcnt = iovcnt;
	/* [한국어] DIF 정책/블록 수 기록(생성은 err 출력 불필요). */
	accel_task->dif.ctx = ctx;
	accel_task->dif.num_blocks = num_blocks;
	/* [한국어] 총 바이트 = 블록 수 × 블록 크기. */
	accel_task->nbytes = num_blocks * ctx->block_size;
	/* [한국어] opcode(DIF_GENERATE)/도메인 지정 후 제출. */
	accel_task->op_code = SPDK_ACCEL_OPC_DIF_GENERATE;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/*
 * [한국어]
 * spdk_accel_submit_dif_generate_copy - 데이터를 복사하면서 동시에 T10 DIF 보호정보(PI)를 생성·삽입한다.
 *
 * @ch: accel io_channel(채널 owner 스레드에서만 호출).
 * @dst_iovs/@dst_iovcnt: PI가 삽입된 결과(데이터+Guard/Tag)가 기록될 목적지 벡터.
 * @src_iovs/@src_iovcnt: 원본 데이터 벡터(PI 없는 순수 데이터).
 * @num_blocks: 처리할 블록 수. @ctx: DIF 정책(Guard 모드/Ref Tag/블록 크기 등).
 * @cb_fn/@cb_arg: 완료 콜백/컨텍스트(채널 owner 스레드에서 실행).
 * @return: 0=수락, -ENOMEM(task 풀 소진).
 *
 * generate(in-place)와 달리, 별도 목적지 버퍼로 복사하면서 PI를 새로 계산해 끼워 넣는
 * 결합 연산(COPY+DIF generate). 백엔드 모듈이 한 번의 패스로 복사·PI계산을 처리해
 * 메모리 대역폭/캐시 재사용 이점을 얻는다. accel_submit_task로 모듈 우선순위에 따라
 * 디스패치되며, 미지원 시 sw 모듈이 폴백 처리한다.
 *
 * 호출 체인:
 *   상위 사용자(bdev write 경로 등) → [이 함수] → accel_submit_task → 모듈 submit_tasks
 */
int
spdk_accel_submit_dif_generate_copy(struct spdk_io_channel *ch, struct iovec *dst_iovs,
				    size_t dst_iovcnt, struct iovec *src_iovs, size_t src_iovcnt,
				    uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
				    spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] io_channel에서 accel 전용 채널 컨텍스트(task/buf 풀, 통계) 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] 채널 task 풀에서 task 1개 확보. 풀 소진 시 NULL → 호출자에게 -ENOMEM 반환. */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	/* [한국어] 원본(PI 없는 데이터) 소스 벡터 연결. */
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	/* [한국어] 목적지(데이터+PI) 벡터 연결 — generate(in-place)와 달리 별도 버퍼. */
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	/* [한국어] DIF 정책 컨텍스트와 블록 수 기록(생성 경로라 err 출력 불필요). */
	accel_task->dif.ctx = ctx;
	accel_task->dif.num_blocks = num_blocks;
	/* [한국어] 총 바이트 = 블록 수 × 블록당 바이트(PI 포함 블록 크기 기준). */
	accel_task->nbytes = num_blocks * ctx->block_size;
	/* [한국어] opcode=DIF_GENERATE_COPY로 백엔드에 복사+PI생성 결합 연산임을 알림. */
	accel_task->op_code = SPDK_ACCEL_OPC_DIF_GENERATE_COPY;
	/* [한국어] 메모리 도메인 없음(호스트 일반 메모리). RDMA/GPU 등 원격 도메인 미사용. */
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	/* [한국어] 우선순위가 가장 높은 모듈로 task 제출(미지원이면 sw 폴백). */
	return accel_submit_task(accel_ch, accel_task);
}

/*
 * [한국어]
 * spdk_accel_submit_dif_verify_copy - 데이터를 복사하면서 동시에 T10 DIF 보호정보(PI)를 검증하고 PI를 제거(strip)한다.
 *
 * @ch: accel io_channel(채널 owner 스레드 전용).
 * @dst_iovs/@dst_iovcnt: PI가 제거된 순수 데이터가 기록될 목적지 벡터.
 * @src_iovs/@src_iovcnt: PI가 포함된 원본 데이터 벡터(검증 대상).
 * @num_blocks: 블록 수. @ctx: DIF 정책. @err: 검증 실패 시 오류 위치/유형을 채워 돌려주는 출력 인자.
 * @cb_fn/@cb_arg: 완료 콜백/컨텍스트.
 * @return: 0=수락, -ENOMEM.
 *
 * 읽기 경로에서 디스크 데이터를 호스트 버퍼로 옮기면서 Guard/App/Ref Tag를 검사하는
 * 결합 연산(COPY+DIF verify). generate_copy와 대칭이며, 검증 실패 시 err에 어떤 블록의
 * 어떤 태그가 어긋났는지 기록해 상위에서 EIO 처리를 하도록 한다.
 *
 * 호출 체인:
 *   상위 사용자(bdev read 경로 등) → [이 함수] → accel_submit_task → 모듈 submit_tasks
 */
int
spdk_accel_submit_dif_verify_copy(struct spdk_io_channel *ch,
				  struct iovec *dst_iovs, size_t dst_iovcnt,
				  struct iovec *src_iovs, size_t src_iovcnt, uint32_t num_blocks,
				  const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] task 1개 확보(소진 시 -ENOMEM). */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	/* [한국어] 소스 = PI 포함 원본(검증 대상). */
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	/* [한국어] 목적지 = PI 제거된 순수 데이터 출력 버퍼. */
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	/* [한국어] DIF 정책 컨텍스트 기록. */
	accel_task->dif.ctx = ctx;
	/* [한국어] 검증 실패 위치/유형을 담아 돌려줄 출력 구조체 연결(verify 경로만 필요). */
	accel_task->dif.err = err;
	accel_task->dif.num_blocks = num_blocks;
	/* [한국어] 총 바이트 = 블록 수 × PI 포함 블록 크기. */
	accel_task->nbytes = num_blocks * ctx->block_size;
	/* [한국어] opcode=DIF_VERIFY_COPY로 복사+PI검증+strip 결합 연산 지정. */
	accel_task->op_code = SPDK_ACCEL_OPC_DIF_VERIFY_COPY;
	/* [한국어] 호스트 일반 메모리(원격 도메인 없음). */
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	/* [한국어] 모듈로 제출. */
	return accel_submit_task(accel_ch, accel_task);
}

/*
 * [한국어]
 * spdk_accel_submit_dix_generate - DIX(분리 메타데이터) 방식으로 보호정보(PI)를 생성해 별도 메타데이터 버퍼에 기록한다.
 *
 * @ch: accel io_channel(채널 owner 스레드 전용).
 * @iovs/@iovcnt: 보호 대상 데이터 벡터(데이터 자체는 변경되지 않음).
 * @md_iov: PI(Guard/App/Ref Tag)가 기록될 분리된 메타데이터 버퍼(단일 iov).
 * @num_blocks: 블록 수. @ctx: DIF 정책. @cb_fn/@cb_arg: 완료 콜백/컨텍스트.
 * @return: 0=수락, -ENOMEM.
 *
 * DIF는 데이터와 PI가 한 블록 안에 섞여 있는(interleaved) 반면, DIX(Data Integrity
 * eXtension)는 데이터와 PI(메타데이터)를 물리적으로 분리해 별도 버퍼에 둔다. 따라서
 * 목적지 d.iovs는 메타데이터 버퍼 하나(d.iovcnt=1)만 가리킨다. 쓰기 전 PI를 생성하는
 * 경로이므로 err 출력 인자는 없다.
 *
 * 호출 체인:
 *   상위 사용자(DIX 지원 bdev write) → [이 함수] → accel_submit_task → 모듈 submit_tasks
 */
int
spdk_accel_submit_dix_generate(struct spdk_io_channel *ch, struct iovec *iovs,
			       size_t iovcnt, struct iovec *md_iov, uint32_t num_blocks,
			       const struct spdk_dif_ctx *ctx, spdk_accel_completion_cb cb_fn,
			       void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] task 1개 확보(소진 시 -ENOMEM). */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	/* [한국어] 소스 = 보호 대상 데이터(DIX는 데이터를 건드리지 않고 PI만 산출). */
	accel_task->s.iovs = iovs;
	accel_task->s.iovcnt = iovcnt;
	/* [한국어] 목적지 = 분리된 메타데이터 버퍼 1개. DIX는 데이터/PI를 분리 저장하므로 iovcnt=1 고정. */
	accel_task->d.iovs = md_iov;
	accel_task->d.iovcnt = 1;
	/* [한국어] DIF 정책 컨텍스트와 블록 수 기록. */
	accel_task->dif.ctx = ctx;
	accel_task->dif.num_blocks = num_blocks;
	/* [한국어] 총 바이트 = 블록 수 × 데이터 블록 크기(메타데이터 제외 기준). */
	accel_task->nbytes = num_blocks * ctx->block_size;
	/* [한국어] opcode=DIX_GENERATE로 분리 메타데이터 PI 생성 지정. */
	accel_task->op_code = SPDK_ACCEL_OPC_DIX_GENERATE;
	/* [한국어] 호스트 일반 메모리(원격 도메인 없음). */
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	/* [한국어] 모듈로 제출. */
	return accel_submit_task(accel_ch, accel_task);
}

/*
 * [한국어]
 * spdk_accel_submit_dix_verify - DIX(분리 메타데이터)에 저장된 보호정보(PI)로 데이터 무결성을 검증한다.
 *
 * @ch: accel io_channel(채널 owner 스레드 전용).
 * @iovs/@iovcnt: 검증 대상 데이터 벡터.
 * @md_iov: 비교 기준이 되는 분리 메타데이터(PI) 버퍼(단일 iov).
 * @num_blocks: 블록 수. @ctx: DIF 정책. @err: 검증 실패 위치/유형 출력 인자.
 * @cb_fn/@cb_arg: 완료 콜백/컨텍스트.
 * @return: 0=수락, -ENOMEM.
 *
 * dix_generate의 대칭 연산: 데이터와 분리 저장된 메타데이터 PI를 재계산해 비교한다.
 * 읽기 경로에서 디스크 데이터의 무결성을 확인할 때 사용하며, 실패 시 err에 어긋난
 * 블록/태그 정보를 채워 상위에서 EIO 처리를 하도록 한다.
 *
 * 호출 체인:
 *   상위 사용자(DIX 지원 bdev read) → [이 함수] → accel_submit_task → 모듈 submit_tasks
 */
int
spdk_accel_submit_dix_verify(struct spdk_io_channel *ch, struct iovec *iovs,
			     size_t iovcnt, struct iovec *md_iov, uint32_t num_blocks,
			     const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
			     spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	/* [한국어] task 1개 확보(소진 시 -ENOMEM). */
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	/* [한국어] 소스 = 검증 대상 데이터. */
	accel_task->s.iovs = iovs;
	accel_task->s.iovcnt = iovcnt;
	/* [한국어] 목적지 = 비교 기준 분리 메타데이터(PI) 버퍼 1개. */
	accel_task->d.iovs = md_iov;
	accel_task->d.iovcnt = 1;
	/* [한국어] DIF 정책 컨텍스트 기록. */
	accel_task->dif.ctx = ctx;
	/* [한국어] 검증 실패 위치/유형을 담아 돌려줄 출력 구조체 연결(verify 경로). */
	accel_task->dif.err = err;
	accel_task->dif.num_blocks = num_blocks;
	/* [한국어] 총 바이트 = 블록 수 × 데이터 블록 크기. */
	accel_task->nbytes = num_blocks * ctx->block_size;
	/* [한국어] opcode=DIX_VERIFY로 분리 메타데이터 기반 PI 검증 지정. */
	accel_task->op_code = SPDK_ACCEL_OPC_DIX_VERIFY;
	/* [한국어] 호스트 일반 메모리(원격 도메인 없음). */
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	/* [한국어] 모듈로 제출. */
	return accel_submit_task(accel_ch, accel_task);
}

/*
 * [한국어]
 * accel_get_buf - 채널의 buf 디스크립터 풀에서 accel_buffer 1개를 꺼내 초기화한다.
 *
 * @ch: accel 채널 컨텍스트(채널 owner 스레드 전용, 락 불필요).
 * @len: 이 버퍼가 나중에 표현할 논리 데이터 길이(실제 메모리는 아직 미할당).
 * @return: accel_buffer 포인터, 풀 소진 시 NULL.
 *
 * accel_buffer는 "지연 할당되는 바운스 버퍼 디스크립터"로, append 단계에서는 핸들(len)만
 * 잡아 두고 실제 iobuf 메모리는 시퀀스 실행 시점(accel_get_iobuf_for_buffer)에 할당한다.
 * 이렇게 분리하면 시퀀스 빌드 중 메모리 압박으로 인한 데드락을 줄일 수 있다. 풀은 채널당
 * SLIST이고 단일 스레드 소유라 lockless하다. 풀 소진 시 retry.bufdesc 통계를 올린다.
 *
 * 호출 체인:
 *   spdk_accel_get_buf / append_* 바운스 경로 → [이 함수] → SLIST_FIRST/REMOVE_HEAD
 */
static inline struct accel_buffer *
accel_get_buf(struct accel_io_channel *ch, uint64_t len)
{
	struct accel_buffer *buf;

	/* [한국어] 채널 buf 디스크립터 free-list 머리에서 후보 1개 확인(아직 제거 전). */
	buf = SLIST_FIRST(&ch->buf_pool);
	/* [한국어] 풀이 비었으면(드문 경로) 부족 통계 증가 후 NULL 반환 → 호출자 재시도 처리. */
	if (spdk_unlikely(buf == NULL)) {
		accel_update_stats(ch, retry.bufdesc, 1);
		return NULL;
	}

	/* [한국어] free-list에서 실제로 떼어냄(단일 스레드라 락 없이 안전). */
	SLIST_REMOVE_HEAD(&ch->buf_pool, link);
	/* [한국어] 논리 길이 기록(실제 메모리 할당은 지연). */
	buf->len = len;
	/* [한국어] 아직 iobuf 미할당 상태 표시 — 실행 시점에 spdk_iobuf_get으로 채움. */
	buf->buf = NULL;
	/* [한국어] 이 버퍼를 소유한 시퀀스 역참조 초기화(할당 대기 시 연결됨). */
	buf->seq = NULL;
	/* [한국어] iobuf 대기 콜백 초기화(메모리 대기 큐에서 깨어날 때 사용). */
	buf->cb_fn = NULL;

	return buf;
}

/*
 * [한국어]
 * accel_put_buf - accel_buffer를 반납한다. 실제 iobuf 메모리가 붙어 있으면 iobuf 풀로 먼저 돌려준다.
 *
 * @ch: accel 채널 컨텍스트(채널 owner 스레드 전용).
 * @buf: 반납할 accel_buffer 디스크립터.
 *
 * accel_get_buf의 짝. 디스크립터에 실제 데이터 메모리(buf->buf)가 할당돼 있었다면
 * spdk_iobuf_put으로 iobuf mempool에 반환하고, 디스크립터 자체는 채널 buf 풀 free-list에
 * 다시 넣는다. 시퀀스 종료(accel_sequence_put)나 버퍼 해제 시 호출된다. 단일 스레드
 * 소유라 lockless.
 *
 * 호출 체인:
 *   accel_sequence_put / spdk_accel_put_buf → [이 함수] → spdk_iobuf_put + SLIST_INSERT_HEAD
 */
static inline void
accel_put_buf(struct accel_io_channel *ch, struct accel_buffer *buf)
{
	/* [한국어] 실제 데이터 메모리가 할당돼 있었으면 iobuf mempool로 반환(len 단위로 풀 매칭). */
	if (buf->buf != NULL) {
		spdk_iobuf_put(&ch->iobuf, buf->buf, buf->len);
	}

	/* [한국어] 디스크립터를 채널 free-list 머리에 재삽입(재사용 대기). */
	SLIST_INSERT_HEAD(&ch->buf_pool, buf, link);
}

/*
 * [한국어]
 * accel_sequence_get - 채널 시퀀스 풀에서 spdk_accel_sequence 1개를 꺼내 초기화한다.
 *
 * @ch: accel 채널 컨텍스트(채널 owner 스레드 전용, lockless).
 * @return: 초기화된 시퀀스 객체, 자원 부족 시 NULL.
 *
 * accel sequence는 여러 accel 연산(copy/fill/crc/encrypt 등)을 하나의 비동기 체인으로
 * 묶어 모듈 경계를 넘나드는 task들을 순차/병합 실행하는 메커니즘이다. 이 함수는 시퀀스
 * 객체를 풀에서 꺼내 빈 task 리스트/바운스 버퍼 리스트와 INIT 상태로 리셋한다.
 *
 * 데드락 방지 설계가 핵심이다: 시퀀스를 얻은 뒤에는 io buffer 할당 같은 추가 비동기
 * 연산이 최소 1회 발생할 수 있는데, 그 시점에 task 자원이 고갈되면 어떤 요청도 진행하지
 * 못하고 서로 task를 기다리는 교착이 생긴다. 이를 피하려고 "현재 가용 task 수가
 * ACCEL_TASKS_IN_SEQUENCE_LIMIT 미만이면 시퀀스 발급 자체를 거부"한다. (시퀀스 획득 후
 * 비동기 연산이 1회뿐이라는 가정 하에서만 안전; 더 많아지면 개선 필요.)
 *
 * 호출 체인:
 *   spdk_accel_append_* (시퀀스 첫 단계) → [이 함수] → SLIST_REMOVE_HEAD/TAILQ_INIT
 */
static inline struct spdk_accel_sequence *
accel_sequence_get(struct accel_io_channel *ch)
{
	struct spdk_accel_sequence *seq;

	/* [한국어] 미반납 task 수가 전체 task_count를 넘을 수 없다는 불변식 검증(디버그). */
	assert(g_opts.task_count >= ch->stats.task_outstanding);

	/* Sequence cannot be allocated if number of available task objects cannot satisfy required limit.
	 * This is to prevent potential dead lock when few requests are pending task resource and none can
	 * advance the processing. This solution should work only if there is single async operation after
	 * sequence obj obtained, so assume that is possible to happen with io buffer allocation now, if
	 * there are more async operations then solution should be improved. */
	/* [한국어] 가용 task(task_count - 미반납) 가 임계치 미만이면 데드락 위험 → 시퀀스 발급 거부. */
	if (spdk_unlikely(g_opts.task_count - ch->stats.task_outstanding < ACCEL_TASKS_IN_SEQUENCE_LIMIT)) {
		return NULL;
	}

	/* [한국어] 시퀀스 free-list 머리에서 후보 1개 확인. */
	seq = SLIST_FIRST(&ch->seq_pool);
	/* [한국어] 풀 소진 시(드문 경로) retry.sequence 통계 증가 후 NULL → 호출자 재시도. */
	if (spdk_unlikely(seq == NULL)) {
		accel_update_stats(ch, retry.sequence, 1);
		return NULL;
	}

	/* [한국어] 진행 중 시퀀스 수 +1(통계/관찰용). */
	accel_update_stats(ch, sequence_outstanding, 1);
	/* [한국어] free-list에서 실제로 떼어냄(단일 스레드 lockless). */
	SLIST_REMOVE_HEAD(&ch->seq_pool, link);

	/* [한국어] 이 시퀀스에 누적될 task 리스트 비우기. */
	TAILQ_INIT(&seq->tasks);
	/* [한국어] 도메인 변환 시 사용할 바운스 버퍼 리스트 비우기. */
	SLIST_INIT(&seq->bounce_bufs);

	/* [한국어] 소유 채널 역참조 — 실행 시 task/buf를 같은 채널에서 가져오기 위함. */
	seq->ch = ch;
	/* [한국어] 누적 완료 상태 0(성공)으로 초기화 — 단계 중 실패 시 첫 에러로 덮임. */
	seq->status = 0;
	/* [한국어] 상태머신 시작점 INIT으로 설정(accel_process_sequence가 여기서 출발). */
	seq->state = ACCEL_SEQUENCE_STATE_INIT;
	/* [한국어] 재진입 가드 초기화 — process_sequence가 중첩 호출되지 않도록 표시. */
	seq->in_process_sequence = false;

	return seq;
}

/*
 * [한국어]
 * accel_sequence_put - 시퀀스를 풀로 반납하고, 보유했던 바운스 버퍼를 모두 해제한다.
 *
 * @seq: 반납할 시퀀스(task 리스트는 이미 비어 있어야 함).
 *
 * accel_sequence_get의 짝. 시퀀스가 도메인 변환을 위해 잡았던 bounce_bufs를 하나씩
 * accel_put_buf로 돌려준 뒤, 시퀀스 객체 자체를 채널 free-list에 재삽입한다. task가
 * 남아 있으면 안 되는 불변식을 assert로 확인한다. 채널 owner 스레드 전용이라 lockless.
 *
 * 호출 체인:
 *   accel_sequence_complete / 에러 정리 경로 → [이 함수] → accel_put_buf + SLIST_INSERT_HEAD
 */
static inline void
accel_sequence_put(struct spdk_accel_sequence *seq)
{
	/* [한국어] 소유 채널 캐시(반납도 같은 채널 풀로). */
	struct accel_io_channel *ch = seq->ch;
	struct accel_buffer *buf;

	/* [한국어] 시퀀스가 잡고 있던 바운스 버퍼를 전부 반납(메모리 누수 방지). */
	while (!SLIST_EMPTY(&seq->bounce_bufs)) {
		buf = SLIST_FIRST(&seq->bounce_bufs);
		SLIST_REMOVE_HEAD(&seq->bounce_bufs, link);
		accel_put_buf(seq->ch, buf);
	}

	/* [한국어] 모든 단계가 소비되어 task 리스트가 비었는지 불변식 확인(디버그). */
	assert(TAILQ_EMPTY(&seq->tasks));
	/* [한국어] 채널 역참조 끊기 — 풀에 있는 동안은 소유 채널 없음. */
	seq->ch = NULL;

	/* [한국어] 시퀀스 객체를 free-list 머리에 재삽입(재사용 대기). */
	SLIST_INSERT_HEAD(&ch->seq_pool, seq, link);
	/* [한국어] 진행 중 시퀀스 수 -1. */
	accel_update_stats(ch, sequence_outstanding, -1);
}

/*
 * [한국어]
 * accel_sequence_get_task - 시퀀스에 추가할 task를 채널 풀에서 꺼내 단계 콜백을 설정한다.
 *
 * @ch: accel 채널 컨텍스트. @seq: 이 task가 속할 시퀀스.
 * @cb_fn: 이 단계(step) 완료 시 호출될 콜백. @cb_arg: 콜백 인자.
 * @return: 설정된 task, 풀 소진 시 NULL.
 *
 * spdk_accel_append_* 계열이 각 단계를 시퀀스에 붙일 때 공통으로 쓰는 헬퍼. 일반
 * _get_task와 달리 완료 콜백(cb_fn/cb_arg)을 직접 등록하지 않고(NULL,NULL), 시퀀스 단계
 * 콜백(step_cb_fn)과 시퀀스 역참조(seq)를 설정한다. task 완료 시 시퀀스 상태머신이
 * step_cb_fn을 호출해 다음 단계로 진행한다.
 *
 * 호출 체인:
 *   spdk_accel_append_copy/fill/crc32c/... → [이 함수] → _get_task
 */
static inline struct spdk_accel_task *
accel_sequence_get_task(struct accel_io_channel *ch, struct spdk_accel_sequence *seq,
			spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *task;

	/* [한국어] 채널 task 풀에서 task 1개 확보(완료 콜백은 시퀀스가 관리하므로 NULL). */
	task = _get_task(ch, NULL, NULL);
	/* [한국어] 풀 소진 시 NULL 그대로 반환 → 호출자가 시퀀스 롤백 처리. */
	if (spdk_unlikely(task == NULL)) {
		return task;
	}

	/* [한국어] 이 단계 완료 시 호출될 step 콜백 등록. */
	task->step_cb_fn = cb_fn;
	task->cb_arg = cb_arg;
	/* [한국어] 소속 시퀀스 역참조 — 완료 시 상태머신이 이 시퀀스를 진행시킴. */
	task->seq = seq;

	return task;
}

/*
 * [한국어]
 * spdk_accel_append_copy - 진행 중인 accel 시퀀스에 "복사(copy)" 단계를 추가한다.
 *
 * @pseq: 시퀀스 핸들의 in/out 포인터. *pseq==NULL이면 새 시퀀스를 생성해 돌려준다.
 * @ch: accel io_channel(채널 owner 스레드 전용).
 * @dst_iovs/@dst_iovcnt: 목적지 벡터. @dst_domain/@dst_domain_ctx: 목적지 메모리 도메인(RDMA/GPU 등, 없으면 NULL).
 * @src_iovs/@src_iovcnt: 소스 벡터. @src_domain/@src_domain_ctx: 소스 메모리 도메인.
 * @cb_fn/@cb_arg: 이 단계 완료 콜백/컨텍스트.
 * @return: 0=단계 추가 성공, -ENOMEM(시퀀스/task 자원 부족).
 *
 * accel 시퀀스 빌더 패턴의 한 조각이다. 사용자는 append_copy/fill/crc32c/encrypt 등을
 * 연달아 호출해 단계를 누적한 뒤 spdk_accel_sequence_finish로 한꺼번에 실행한다. 메모리
 * 도메인 인자가 있으면 소스/목적지가 호스트 일반 메모리가 아닐 수 있어(RDMA 등록 메모리,
 * GPU 메모리), 실행 시점에 바운스 버퍼를 통한 pull/push 변환이 일어날 수 있다.
 *
 * 첫 단계라면 시퀀스를 새로 잡고, 중간에 task 할당이 실패하면 (자기가 시퀀스를 새로 만든
 * 경우에만) 시퀀스를 되돌려 자원 누수를 막는다. 즉 호출자가 넘긴 기존 시퀀스는 보존한다.
 *
 * 호출 체인:
 *   상위 사용자(bdev/compress 등) → [이 함수] → accel_sequence_get/get_task → TAILQ_INSERT_TAIL
 */
int
spdk_accel_append_copy(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
		       struct iovec *dst_iovs, uint32_t dst_iovcnt,
		       struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
		       struct iovec *src_iovs, uint32_t src_iovcnt,
		       struct spdk_memory_domain *src_domain, void *src_domain_ctx,
		       spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트와 현재 시퀀스 핸들 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	/* [한국어] 첫 단계라면(*pseq==NULL) 시퀀스를 새로 발급. 자원 부족 시 -ENOMEM. */
	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	/* [한국어] 시퀀스는 발급 채널에 고정 — 다른 채널에서 단계 추가는 불변식 위반. */
	assert(seq->ch == accel_ch);
	/* [한국어] 이 단계용 task 확보 + step 콜백 등록. */
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		/* [한국어] task 실패. 이 호출에서 시퀀스를 새로 만들었던 경우에만 되돌림(기존 시퀀스 보존). */
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	/* [한국어] 목적지 메모리 도메인/컨텍스트 — 비-호스트 메모리면 실행 시 push 변환 트리거. */
	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	/* [한국어] 목적지 벡터 연결. */
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	/* [한국어] 소스 메모리 도메인/컨텍스트 — 비-호스트 메모리면 실행 시 pull 변환 트리거. */
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	/* [한국어] 소스 벡터 연결. */
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	/* [한국어] 복사 바이트 수 = 소스 iov 전체 길이 합산. */
	task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	/* [한국어] 단계 opcode=COPY. */
	task->op_code = SPDK_ACCEL_OPC_COPY;

	/* [한국어] 시퀀스 task 리스트 꼬리에 추가 — 실행은 추가 순서대로. */
	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	/* [한국어] (새로 만들었다면) 호출자에게 시퀀스 핸들 반환. */
	*pseq = seq;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_append_fill - 진행 중인 accel 시퀀스에 "패턴 채우기(fill)" 단계를 추가한다.
 *
 * @pseq: 시퀀스 핸들 in/out(NULL이면 새 시퀀스 생성).
 * @ch: accel io_channel. @buf/@len: 채울 대상 버퍼와 길이.
 * @domain/@domain_ctx: 대상 버퍼의 메모리 도메인(없으면 NULL).
 * @pattern: 1바이트 패턴(64비트로 확장되어 채워짐). @cb_fn/@cb_arg: 단계 완료 콜백.
 * @return: 0=성공, -ENOMEM.
 *
 * memset 류의 패턴 채우기를 시퀀스 단계로 추가한다. fill은 소스가 없고 목적지만 있으며,
 * 1바이트 패턴을 64비트 fill_pattern으로 확장해 둔다. 단일 iov 목적지를 표현하려고 task의
 * 보조 데이터(aux)에서 iov 슬롯을 빌려 쓰는 점이 copy와 다르다. aux는 시퀀스 task당 항상
 * 준비돼 있어야 하므로 미할당이면 치명적 오류로 처리한다.
 *
 * 호출 체인:
 *   상위 사용자 → [이 함수] → accel_sequence_get/get_task → TAILQ_INSERT_TAIL
 */
int
spdk_accel_append_fill(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
		       void *buf, uint64_t len,
		       struct spdk_memory_domain *domain, void *domain_ctx, uint8_t pattern,
		       spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트와 현재 시퀀스 핸들 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	/* [한국어] 첫 단계면 시퀀스 새로 발급(자원 부족 시 -ENOMEM). */
	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	/* [한국어] 시퀀스 채널 고정 불변식 확인. */
	assert(seq->ch == accel_ch);
	/* [한국어] 단계용 task 확보 + step 콜백 등록. */
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		/* [한국어] 이 호출이 시퀀스를 새로 만든 경우에만 롤백. */
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	/* [한국어] 1바이트 패턴을 8바이트(uint64_t) fill_pattern 전체에 복제 — 백엔드가 워드 단위로 채움. */
	memset(&task->fill_pattern, pattern, sizeof(uint64_t));

	/* [한국어] fill은 단일 iov 목적지를 담을 보조 iov가 필요 → 채널 aux 풀에서 슬롯 확보. */
	task->aux = SLIST_FIRST(&task->accel_ch->task_aux_data_pool);
	if (spdk_unlikely(!task->aux)) {
		/* [한국어] aux는 task와 1:1로 항상 준비돼야 함 → 미할당은 설계 위반(치명적). */
		SPDK_ERRLOG("Fatal problem, aux data was not allocated\n");
		/* [한국어] 시퀀스를 이 호출에서 만들었다면 롤백. */
		if (*pseq == NULL) {
			accel_sequence_put((seq));
		}

		/* [한국어] task의 시퀀스 역참조 끊고 풀로 반납(부분 상태 정리). */
		task->seq = NULL;
		_put_task(task->accel_ch, task);
		/* [한국어] 정상 빌드라면 도달 불가 — 디버그 빌드에서 즉시 중단. */
		assert(0);
		return -ENOMEM;
	}
	/* [한국어] aux 슬롯을 풀에서 실제로 떼어내고, task가 aux를 보유함을 표시. */
	SLIST_REMOVE_HEAD(&task->accel_ch->task_aux_data_pool, link);
	task->has_aux = true;

	/* [한국어] 목적지 iov는 aux의 DST 슬롯을 사용 — buf/len을 단일 iov로 표현. */
	task->d.iovs = &task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	task->d.iovs[0].iov_base = buf;
	task->d.iovs[0].iov_len = len;
	task->d.iovcnt = 1;
	/* [한국어] 처리 바이트 수 = 채울 길이. */
	task->nbytes = len;
	/* [한국어] fill은 소스 없음, 목적지 도메인만 설정. */
	task->src_domain = NULL;
	task->dst_domain = domain;
	task->dst_domain_ctx = domain_ctx;
	/* [한국어] 단계 opcode=FILL. */
	task->op_code = SPDK_ACCEL_OPC_FILL;

	/* [한국어] 시퀀스 task 리스트 꼬리에 추가. */
	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	/* [한국어] (새로 만들었다면) 시퀀스 핸들 반환. */
	*pseq = seq;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_append_decompress - 진행 중인 시퀀스에 기본 알고리즘(DEFLATE) 압축 해제 단계를 추가한다.
 *
 * @pseq..@cb_arg: spdk_accel_append_decompress_ext와 동일(아래 참조).
 * @return: ext 버전의 반환값 그대로(0 / -ENOMEM / 알고리즘 오류 -EINVAL 등).
 *
 * 알고리즘 인자 없는 편의 래퍼. SPDK_ACCEL_COMP_ALGO_DEFLATE를 기본값으로 채워
 * decompress_ext에 위임한다. 기존 ABI 호환을 위해 별도 심볼로 남아 있다.
 *
 * 호출 체인:
 *   상위 사용자 → [이 함수] → spdk_accel_append_decompress_ext
 */
int
spdk_accel_append_decompress(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			     struct iovec *dst_iovs, size_t dst_iovcnt,
			     struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			     struct iovec *src_iovs, size_t src_iovcnt,
			     struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			     spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] 알고리즘을 DEFLATE로 고정해 ext 구현에 전달. */
	return spdk_accel_append_decompress_ext(pseq, ch, dst_iovs, dst_iovcnt, dst_domain,
						dst_domain_ctx, src_iovs, src_iovcnt, src_domain,
						src_domain_ctx, SPDK_ACCEL_COMP_ALGO_DEFLATE,
						cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_accel_append_decompress_ext - 시퀀스에 지정 알고리즘 압축 해제(decompress) 단계를 추가한다.
 *
 * @pseq: 시퀀스 핸들 in/out(NULL이면 새 시퀀스 생성).
 * @ch: accel io_channel. @dst_iovs/@dst_iovcnt: 압축 해제 결과 목적지. @src_iovs/@src_iovcnt: 압축 입력.
 * @dst_domain/@src_domain(+ctx): 메모리 도메인(비-호스트면 바운스 변환).
 * @decomp_algo: DEFLATE/LZ4 등 압축 알고리즘. @cb_fn/@cb_arg: 단계 완료 콜백.
 * @return: 0=성공, -ENOMEM, 또는 알고리즘 미지원 시 _accel_check_comp_algo의 음수 errno.
 *
 * 압축 해제는 출력 크기가 가변이라 output_size 포인터로 실제 산출 바이트를 받을 수 있으나,
 * 현재 시퀀스 체이닝 경로에서는 아직 미지원(TODO)이라 NULL로 둔다. 먼저 알고리즘 유효성을
 * 검사한 뒤 task를 시퀀스에 누적한다.
 *
 * 호출 체인:
 *   상위 사용자(compress bdev 등) → [이 함수] → _accel_check_comp_algo + accel_sequence_get/get_task
 */
int
spdk_accel_append_decompress_ext(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
				 struct iovec *dst_iovs, size_t dst_iovcnt,
				 struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				 struct iovec *src_iovs, size_t src_iovcnt,
				 struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				 enum spdk_accel_comp_algo decomp_algo,
				 spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트와 현재 시퀀스 핸들 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;
	int rc;

	/* [한국어] 요청 알고리즘이 현재 모듈 set에서 지원되는지 먼저 검증(미지원 시 음수 반환). */
	rc = _accel_check_comp_algo(decomp_algo);
	if (spdk_unlikely(rc != 0)) {
		return rc;
	}

	/* [한국어] 첫 단계면 시퀀스 새로 발급. */
	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	/* [한국어] 시퀀스 채널 고정 불변식 확인. */
	assert(seq->ch == accel_ch);
	/* [한국어] 단계용 task 확보 + step 콜백 등록. */
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		/* [한국어] 이 호출에서 만든 시퀀스만 롤백. */
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	/* TODO: support output_size for chaining */
	/* [한국어] 체이닝 시 가변 출력 크기 전달은 아직 미지원 → NULL. */
	task->output_size = NULL;
	/* [한국어] 목적지/소스 도메인·벡터 연결. */
	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	/* [한국어] 입력(압축된) 바이트 수 = 소스 iov 길이 합. */
	task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	/* [한국어] 단계 opcode=DECOMPRESS, 알고리즘 기록. */
	task->op_code = SPDK_ACCEL_OPC_DECOMPRESS;
	task->comp.algo = decomp_algo;

	/* [한국어] 시퀀스 task 리스트 꼬리에 추가하고 핸들 반환. */
	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_append_encrypt - 시퀀스에 블록 단위 암호화(encrypt) 단계를 추가한다.
 *
 * @pseq: 시퀀스 핸들 in/out. @ch: accel io_channel.
 * @key: 암호 키 핸들(crypto 모듈에 등록된 DEK). @dst_iovs/@src_iovs(+cnt, +domain): 입출력 벡터/도메인.
 * @iv: 초기화 벡터(블록 카운터 기반 tweak, 보통 LBA). @block_size: 암호 블록 크기(예: 512/4096).
 * @cb_fn/@cb_arg: 단계 완료 콜백.
 * @return: 0=성공, -ENOMEM.
 *
 * AES-XTS 등 블록 암호로 src→dst를 암호화한다. iv와 block_size로 블록별 tweak를
 * 유도하므로 디스크 LBA 기반 암호화(at-rest encryption)에 적합하다. 필수 인자들은
 * assert로 검증한다(키/벡터/블록크기 누락 금지).
 *
 * 호출 체인:
 *   상위 사용자(crypto bdev 등) → [이 함수] → accel_sequence_get/get_task → TAILQ_INSERT_TAIL
 */
int
spdk_accel_append_encrypt(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			  struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			  uint64_t iv, uint32_t block_size,
			  spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트와 현재 시퀀스 핸들 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	/* [한국어] 암호화 필수 인자(목적지/소스 벡터, 키, 블록크기) 누락 금지 — 디버그 검증. */
	assert(dst_iovs && dst_iovcnt && src_iovs && src_iovcnt && key && block_size);

	/* [한국어] 첫 단계면 시퀀스 새로 발급. */
	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	/* [한국어] 시퀀스 채널 고정 불변식 확인. */
	assert(seq->ch == accel_ch);
	/* [한국어] 단계용 task 확보 + step 콜백 등록. */
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		/* [한국어] 이 호출에서 만든 시퀀스만 롤백. */
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	/* [한국어] 사용할 암호 키 핸들(DEK) 지정. */
	task->crypto_key = key;
	/* [한국어] 소스(평문) 도메인·벡터 연결. */
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	/* [한국어] 목적지(암호문) 도메인·벡터 연결. */
	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	/* [한국어] 처리 바이트 수 = 소스 길이 합. */
	task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	/* [한국어] 초기화 벡터(블록 tweak 기준, 보통 LBA)와 블록 크기 기록. */
	task->iv = iv;
	task->block_size = block_size;
	/* [한국어] 단계 opcode=ENCRYPT. */
	task->op_code = SPDK_ACCEL_OPC_ENCRYPT;

	/* [한국어] 시퀀스 task 리스트 꼬리에 추가하고 핸들 반환. */
	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_append_decrypt - 시퀀스에 블록 단위 복호화(decrypt) 단계를 추가한다.
 *
 * @pseq..@cb_arg: spdk_accel_append_encrypt와 동일하나 src=암호문, dst=평문.
 * @return: 0=성공, -ENOMEM.
 *
 * encrypt의 대칭 연산. 동일 키/iv/block_size로 암호문을 평문으로 되돌린다. AES-XTS는
 * 같은 tweak로 양방향 변환이 가능하므로 구조가 encrypt와 거의 동일하며 opcode만 다르다.
 *
 * 호출 체인:
 *   상위 사용자(crypto bdev read 경로) → [이 함수] → accel_sequence_get/get_task → TAILQ_INSERT_TAIL
 */
int
spdk_accel_append_decrypt(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			  struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			  uint64_t iv, uint32_t block_size,
			  spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트와 현재 시퀀스 핸들 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	/* [한국어] 복호화 필수 인자 누락 금지 — 디버그 검증. */
	assert(dst_iovs && dst_iovcnt && src_iovs && src_iovcnt && key && block_size);

	/* [한국어] 첫 단계면 시퀀스 새로 발급. */
	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	/* [한국어] 시퀀스 채널 고정 불변식 확인. */
	assert(seq->ch == accel_ch);
	/* [한국어] 단계용 task 확보 + step 콜백 등록. */
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		/* [한국어] 이 호출에서 만든 시퀀스만 롤백. */
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	/* [한국어] 사용할 암호 키 핸들(DEK) 지정. */
	task->crypto_key = key;
	/* [한국어] 소스(암호문) 도메인·벡터 연결. */
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	/* [한국어] 목적지(평문) 도메인·벡터 연결. */
	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	/* [한국어] 처리 바이트 수 = 소스 길이 합. */
	task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	/* [한국어] 암호화 때와 동일한 iv/block_size로 tweak 재현. */
	task->iv = iv;
	task->block_size = block_size;
	/* [한국어] 단계 opcode=DECRYPT. */
	task->op_code = SPDK_ACCEL_OPC_DECRYPT;

	/* [한국어] 시퀀스 task 리스트 꼬리에 추가하고 핸들 반환. */
	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_append_crc32c - 시퀀스에 CRC32C(Castagnoli) 체크섬 계산 단계를 추가한다.
 *
 * @pseq: 시퀀스 핸들 in/out. @ch: accel io_channel.
 * @dst: 산출된 CRC32C 값이 기록될 출력 포인터. @iovs/@iovcnt: 체크섬 대상 데이터 벡터.
 * @domain/@domain_ctx: 소스 메모리 도메인(없으면 NULL).
 * @seed: CRC 초기값(이전 청크와 누적 계산 시 사용). @cb_fn/@cb_arg: 단계 완료 콜백.
 * @return: 0=성공, -ENOMEM.
 *
 * CRC32C는 데이터를 변형하지 않고 읽기만 하므로 목적지 데이터 벡터가 없고, 결과 4바이트
 * 값을 crc_dst에 기록한다(목적지 도메인 NULL). T10 DIF/iSCSI 다이제스트 등에서 데이터
 * 무결성 다이제스트를 시퀀스 중간에 끼워 넣을 때 사용한다.
 *
 * 호출 체인:
 *   상위 사용자 → [이 함수] → accel_sequence_get/get_task → TAILQ_INSERT_TAIL
 */
int
spdk_accel_append_crc32c(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			 uint32_t *dst, struct iovec *iovs, uint32_t iovcnt,
			 struct spdk_memory_domain *domain, void *domain_ctx,
			 uint32_t seed, spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트와 현재 시퀀스 핸들 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	/* [한국어] 첫 단계면 시퀀스 새로 발급. */
	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	/* [한국어] 시퀀스 채널 고정 불변식 확인. */
	assert(seq->ch == accel_ch);
	/* [한국어] 단계용 task 확보 + step 콜백 등록. */
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		/* [한국어] 이 호출에서 만든 시퀀스만 롤백. */
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	/* [한국어] 체크섬 대상 소스 벡터·도메인 연결(읽기 전용). */
	task->s.iovs = iovs;
	task->s.iovcnt = iovcnt;
	task->src_domain = domain;
	task->src_domain_ctx = domain_ctx;
	/* [한국어] 처리 바이트 수 = 소스 길이 합. */
	task->nbytes = accel_get_iovlen(iovs, iovcnt);
	/* [한국어] 결과 CRC32C 값을 쓸 출력 포인터와 초기 seed 기록. */
	task->crc_dst = dst;
	task->seed = seed;
	/* [한국어] 단계 opcode=CRC32C, 목적지 데이터 없음(다이제스트만 산출). */
	task->op_code = SPDK_ACCEL_OPC_CRC32C;
	task->dst_domain = NULL;

	/* [한국어] 시퀀스 task 리스트 꼬리에 추가하고 핸들 반환. */
	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_append_dif_verify - 시퀀스에 T10 DIF 보호정보(PI) 검증 단계를 추가한다(데이터 in-place).
 *
 * @pseq: 시퀀스 핸들 in/out. @ch: accel io_channel.
 * @iovs/@iovcnt: PI가 포함된 데이터 벡터(검증 대상, 변형 없음). @domain/@domain_ctx: 메모리 도메인.
 * @num_blocks: 블록 수. @ctx: DIF 정책. @err: 검증 실패 위치/유형 출력. @cb_fn/@cb_arg: 단계 콜백.
 * @return: 0=성공, -ENOMEM.
 *
 * submit 버전 spdk_accel_submit_dif_verify의 시퀀스 단계 등록 형태. 데이터를 옮기지 않고
 * interleaved PI를 검사만 한다(목적지 도메인 없음).
 *
 * 호출 체인:
 *   상위 사용자 → [이 함수] → accel_sequence_get/get_task → TAILQ_INSERT_TAIL
 */
int
spdk_accel_append_dif_verify(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			     struct iovec *iovs, size_t iovcnt,
			     struct spdk_memory_domain *domain, void *domain_ctx,
			     uint32_t num_blocks,
			     const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
			     spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트와 현재 시퀀스 핸들 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	/* [한국어] 첫 단계면 시퀀스 새로 발급. */
	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	/* [한국어] 시퀀스 채널 고정 불변식 확인 + 단계용 task 확보. */
	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		/* [한국어] 이 호출에서 만든 시퀀스만 롤백. */
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	/* [한국어] 검증 대상 데이터(PI 포함) 소스 벡터·도메인 연결. */
	task->s.iovs = iovs;
	task->s.iovcnt = iovcnt;
	task->src_domain = domain;
	task->src_domain_ctx = domain_ctx;
	/* [한국어] in-place 검증이라 목적지 데이터 도메인 없음. */
	task->dst_domain = NULL;
	/* [한국어] DIF 정책/실패 출력/블록 수 기록. */
	task->dif.ctx = ctx;
	task->dif.err = err;
	task->dif.num_blocks = num_blocks;
	/* [한국어] 총 바이트 = 블록 수 × PI 포함 블록 크기. */
	task->nbytes = num_blocks * ctx->block_size;
	/* [한국어] 단계 opcode=DIF_VERIFY. */
	task->op_code = SPDK_ACCEL_OPC_DIF_VERIFY;

	/* [한국어] 시퀀스 task 리스트 꼬리에 추가하고 핸들 반환. */
	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_append_dif_verify_copy - 시퀀스에 "복사+DIF 검증+PI strip" 결합 단계를 추가한다.
 *
 * @pseq..@cb_arg: src=PI 포함 원본, dst=PI 제거된 출력. @err: 검증 실패 출력.
 * @return: 0=성공, -ENOMEM.
 *
 * submit 버전 spdk_accel_submit_dif_verify_copy의 시퀀스 단계 등록 형태. 읽기 경로에서
 * 데이터를 옮기며 무결성을 확인하고 PI를 떼어낸다.
 *
 * 호출 체인:
 *   상위 사용자 → [이 함수] → accel_sequence_get/get_task → TAILQ_INSERT_TAIL
 */
int
spdk_accel_append_dif_verify_copy(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
				  struct iovec *dst_iovs, size_t dst_iovcnt,
				  struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				  struct iovec *src_iovs, size_t src_iovcnt,
				  struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				  uint32_t num_blocks,
				  const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				  spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트와 현재 시퀀스 핸들 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	/* [한국어] 첫 단계면 시퀀스 새로 발급. */
	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	/* [한국어] 시퀀스 채널 고정 불변식 확인 + 단계용 task 확보. */
	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		/* [한국어] 이 호출에서 만든 시퀀스만 롤백. */
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	/* [한국어] 목적지(PI 제거 출력) 도메인·벡터 연결. */
	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	/* [한국어] 소스(PI 포함 원본) 도메인·벡터 연결. */
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	/* [한국어] DIF 정책/실패 출력/블록 수 기록. */
	task->dif.ctx = ctx;
	task->dif.err = err;
	task->dif.num_blocks = num_blocks;
	/* [한국어] 총 바이트 = 블록 수 × PI 포함 블록 크기. */
	task->nbytes = num_blocks * ctx->block_size;
	/* [한국어] 단계 opcode=DIF_VERIFY_COPY. */
	task->op_code = SPDK_ACCEL_OPC_DIF_VERIFY_COPY;

	/* [한국어] 시퀀스 task 리스트 꼬리에 추가하고 핸들 반환. */
	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_append_dif_generate - 시퀀스에 T10 DIF 보호정보(PI) 생성·삽입 단계를 추가한다(데이터 in-place).
 *
 * @pseq..@cb_arg: iovs=PI를 채워 넣을 데이터 벡터(in-place). err 없음(생성 경로).
 * @return: 0=성공, -ENOMEM.
 *
 * submit 버전 spdk_accel_submit_dif_generate의 시퀀스 단계 등록 형태. 쓰기 전 무결성
 * 태그를 같은 버퍼에 생성한다.
 *
 * 호출 체인:
 *   상위 사용자 → [이 함수] → accel_sequence_get/get_task → TAILQ_INSERT_TAIL
 */
int
spdk_accel_append_dif_generate(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			       struct iovec *iovs, size_t iovcnt,
			       struct spdk_memory_domain *domain, void *domain_ctx,
			       uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
			       spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트와 현재 시퀀스 핸들 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	/* [한국어] 첫 단계면 시퀀스 새로 발급. */
	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	/* [한국어] 시퀀스 채널 고정 불변식 확인 + 단계용 task 확보. */
	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		/* [한국어] 이 호출에서 만든 시퀀스만 롤백. */
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	/* [한국어] PI를 채울 데이터(소스=목적지 in-place) 벡터·도메인 연결. */
	task->s.iovs = iovs;
	task->s.iovcnt = iovcnt;
	task->src_domain = domain;
	task->src_domain_ctx = domain_ctx;
	/* [한국어] in-place 생성이라 별도 목적지 도메인 없음. */
	task->dst_domain = NULL;
	/* [한국어] DIF 정책/블록 수 기록(생성은 err 불필요). */
	task->dif.ctx = ctx;
	task->dif.num_blocks = num_blocks;
	/* [한국어] 총 바이트 = 블록 수 × PI 포함 블록 크기. */
	task->nbytes = num_blocks * ctx->block_size;
	/* [한국어] 단계 opcode=DIF_GENERATE. */
	task->op_code = SPDK_ACCEL_OPC_DIF_GENERATE;

	/* [한국어] 시퀀스 task 리스트 꼬리에 추가하고 핸들 반환. */
	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_append_dif_generate_copy - 시퀀스에 "복사+DIF PI 생성·삽입" 결합 단계를 추가한다.
 *
 * @pseq..@cb_arg: src=순수 데이터, dst=데이터+PI. err 없음(생성 경로).
 * @return: 0=성공, -ENOMEM.
 *
 * submit 버전 spdk_accel_submit_dif_generate_copy의 시퀀스 단계 등록 형태. 쓰기 경로에서
 * 데이터를 옮기며 PI를 새로 계산해 끼워 넣는다.
 *
 * 호출 체인:
 *   상위 사용자 → [이 함수] → accel_sequence_get/get_task → TAILQ_INSERT_TAIL
 */
int
spdk_accel_append_dif_generate_copy(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
				    struct iovec *dst_iovs, size_t dst_iovcnt,
				    struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				    struct iovec *src_iovs, size_t src_iovcnt,
				    struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				    uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
				    spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트와 현재 시퀀스 핸들 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	/* [한국어] 첫 단계면 시퀀스 새로 발급. */
	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	/* [한국어] 시퀀스 채널 고정 불변식 확인 + 단계용 task 확보. */
	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		/* [한국어] 이 호출에서 만든 시퀀스만 롤백. */
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	/* [한국어] 목적지(데이터+PI) 도메인·벡터 연결. */
	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	/* [한국어] 소스(순수 데이터) 도메인·벡터 연결. */
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	/* [한국어] DIF 정책/블록 수 기록. */
	task->dif.ctx = ctx;
	task->dif.num_blocks = num_blocks;
	/* [한국어] 총 바이트 = 블록 수 × PI 포함 블록 크기. */
	task->nbytes = num_blocks * ctx->block_size;
	/* [한국어] 단계 opcode=DIF_GENERATE_COPY. */
	task->op_code = SPDK_ACCEL_OPC_DIF_GENERATE_COPY;

	/* [한국어] 시퀀스 task 리스트 꼬리에 추가하고 핸들 반환. */
	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_append_dix_generate - 시퀀스에 DIX(분리 메타데이터) 보호정보(PI) 생성 단계를 추가한다.
 *
 * @seq: 시퀀스 핸들 in/out. @ch: accel io_channel.
 * @iovs/@iovcnt/@domain(+ctx): 보호 대상 데이터(변형 없음). @md_iov/@md_domain(+ctx): PI 출력 메타데이터 버퍼.
 * @num_blocks: 블록 수. @ctx: DIF 정책. @cb_fn/@cb_arg: 단계 콜백.
 * @return: 0=성공, -ENOMEM.
 *
 * submit 버전 spdk_accel_submit_dix_generate의 시퀀스 단계 등록 형태. DIX는 데이터와 PI를
 * 분리 저장하므로 목적지(d.iovs)는 메타데이터 버퍼 1개(d.iovcnt=1)다. 다른 append와 달리
 * 메타데이터에 별도 메모리 도메인(md_domain)을 둘 수 있다.
 *
 * 호출 체인:
 *   상위 사용자 → [이 함수] → accel_sequence_get/get_task → TAILQ_INSERT_TAIL
 */
int
spdk_accel_append_dix_generate(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			       struct iovec *iovs, size_t iovcnt, struct spdk_memory_domain *domain,
			       void *domain_ctx, struct iovec *md_iov,
			       struct spdk_memory_domain *md_domain, void *md_domain_ctx,
			       uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
			       spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트와 현재 시퀀스 핸들 추출(이 API는 인자명이 seq/pseq로 뒤바뀜 주의). */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *pseq = *seq;

	/* [한국어] 첫 단계면 시퀀스 새로 발급. */
	if (pseq == NULL) {
		pseq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(pseq == NULL)) {
			return -ENOMEM;
		}
	}

	/* [한국어] 시퀀스 채널 고정 불변식 확인 + 단계용 task 확보. */
	assert(pseq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, pseq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		/* [한국어] 이 호출에서 만든 시퀀스만 롤백. */
		if (*seq == NULL) {
			accel_sequence_put(pseq);
		}

		return -ENOMEM;
	}

	/* [한국어] 목적지 = 분리 메타데이터 버퍼 1개(+전용 메모리 도메인). */
	task->d.iovs = md_iov;
	task->d.iovcnt = 1;
	task->dst_domain = md_domain;
	task->dst_domain_ctx = md_domain_ctx;
	/* [한국어] 소스 = 보호 대상 데이터(변형 없음). */
	task->s.iovs = iovs;
	task->s.iovcnt = iovcnt;
	task->src_domain = domain;
	task->src_domain_ctx = domain_ctx;
	/* [한국어] DIF 정책/블록 수 기록(생성은 err 불필요). */
	task->dif.ctx = ctx;
	task->dif.num_blocks = num_blocks;
	/* [한국어] 총 바이트 = 블록 수 × 데이터 블록 크기. */
	task->nbytes = num_blocks * ctx->block_size;
	/* [한국어] 단계 opcode=DIX_GENERATE. */
	task->op_code = SPDK_ACCEL_OPC_DIX_GENERATE;

	/* [한국어] 시퀀스 task 리스트 꼬리에 추가하고 핸들 반환. */
	TAILQ_INSERT_TAIL(&pseq->tasks, task, seq_link);
	*seq = pseq;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_append_dix_verify - 시퀀스에 DIX(분리 메타데이터) 보호정보(PI) 검증 단계를 추가한다.
 *
 * @seq..@cb_arg: dix_generate와 동일하나 md_iov=비교 기준 PI, @err=검증 실패 출력.
 * @return: 0=성공, -ENOMEM.
 *
 * submit 버전 spdk_accel_submit_dix_verify의 시퀀스 단계 등록 형태. 데이터와 분리 저장된
 * PI를 재계산해 비교하며, 실패 시 err에 위치/유형을 기록한다.
 *
 * 호출 체인:
 *   상위 사용자 → [이 함수] → accel_sequence_get/get_task → TAILQ_INSERT_TAIL
 */
int
spdk_accel_append_dix_verify(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			     struct iovec *iovs, size_t iovcnt, struct spdk_memory_domain *domain,
			     void *domain_ctx, struct iovec *md_iov,
			     struct spdk_memory_domain *md_domain, void *md_domain_ctx,
			     uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
			     struct spdk_dif_error *err, spdk_accel_step_cb cb_fn, void *cb_arg)
{
	/* [한국어] accel 전용 채널 컨텍스트와 현재 시퀀스 핸들 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *pseq = *seq;

	/* [한국어] 첫 단계면 시퀀스 새로 발급. */
	if (pseq == NULL) {
		pseq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(pseq == NULL)) {
			return -ENOMEM;
		}
	}

	/* [한국어] 시퀀스 채널 고정 불변식 확인 + 단계용 task 확보. */
	assert(pseq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, pseq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		/* [한국어] 이 호출에서 만든 시퀀스만 롤백. */
		if (*seq == NULL) {
			accel_sequence_put(pseq);
		}

		return -ENOMEM;
	}

	/* [한국어] 목적지 = 비교 기준 분리 메타데이터(PI) 버퍼 1개(+전용 도메인). */
	task->d.iovs = md_iov;
	task->d.iovcnt = 1;
	task->dst_domain = md_domain;
	task->dst_domain_ctx = md_domain_ctx;
	/* [한국어] 소스 = 검증 대상 데이터. */
	task->s.iovs = iovs;
	task->s.iovcnt = iovcnt;
	task->src_domain = domain;
	task->src_domain_ctx = domain_ctx;
	/* [한국어] DIF 정책/실패 출력/블록 수 기록. */
	task->dif.ctx = ctx;
	task->dif.err = err;
	task->dif.num_blocks = num_blocks;
	/* [한국어] 총 바이트 = 블록 수 × 데이터 블록 크기. */
	task->nbytes = num_blocks * ctx->block_size;
	/* [한국어] 단계 opcode=DIX_VERIFY. */
	task->op_code = SPDK_ACCEL_OPC_DIX_VERIFY;

	/* [한국어] 시퀀스 task 리스트 꼬리에 추가하고 핸들 반환. */
	TAILQ_INSERT_TAIL(&pseq->tasks, task, seq_link);
	*seq = pseq;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_get_buf - 지연 할당되는 accel 바운스 버퍼를 잡고, 그것을 가리키는 메모리 도메인 핸들을 돌려준다.
 *
 * @ch: accel io_channel. @len: 버퍼 길이.
 * @buf: (out) 버퍼 "주소" — 실제로는 항상 고정 마커 ACCEL_BUFFER_BASE를 반환.
 * @domain: (out) g_accel_domain(accel 전용 메모리 도메인). @domain_ctx: (out) 실제 accel_buffer 식별자.
 * @return: 0=성공, -ENOMEM(buf 디스크립터 풀 소진).
 *
 * accel은 시퀀스 단계의 중간 버퍼를 "메모리 도메인"으로 추상화한다. 실제 데이터 메모리는
 * 아직 할당하지 않고(지연), domain=g_accel_domain + domain_ctx=accel_buffer 조합으로
 * 버퍼를 식별한다. 주소(*buf)는 의미 없는 고정 베이스(ACCEL_BUFFER_BASE)이며, 실행 시점에
 * accel이 이 도메인을 보고 진짜 iobuf를 할당해 가상 주소로 치환한다(accel_sequence_set_virtbuf).
 * 이렇게 하면 상위가 "아직 메모리 없는 버퍼"를 시퀀스에 넣어 두고 실행 직전 할당할 수 있다.
 *
 * 호출 체인:
 *   상위 사용자(중간 버퍼 필요 시) → [이 함수] → accel_get_buf
 */
int
spdk_accel_get_buf(struct spdk_io_channel *ch, uint64_t len, void **buf,
		   struct spdk_memory_domain **domain, void **domain_ctx)
{
	/* [한국어] accel 전용 채널 컨텍스트 추출. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct accel_buffer *accel_buf;

	/* [한국어] buf 디스크립터 1개 확보(실제 메모리는 미할당). 소진 시 -ENOMEM. */
	accel_buf = accel_get_buf(accel_ch, len);
	if (spdk_unlikely(accel_buf == NULL)) {
		return -ENOMEM;
	}

	/* [한국어] 나중에 같은 채널에서 실제 iobuf를 할당하기 위해 소유 채널 기록. */
	accel_buf->ch = accel_ch;

	/* We always return the same pointer and identify the buffers through domain_ctx */
	/* [한국어] 주소는 의미 없는 고정 마커 — 실제 식별은 domain_ctx(accel_buf)로 한다. */
	*buf = ACCEL_BUFFER_BASE;
	*domain_ctx = accel_buf;
	/* [한국어] accel 전용 메모리 도메인 핸들 반환 — accel 내부에서 이 도메인을 자기 버퍼로 인식. */
	*domain = g_accel_domain;

	return 0;
}

/*
 * [한국어]
 * spdk_accel_put_buf - spdk_accel_get_buf로 얻은 바운스 버퍼를 반납한다.
 *
 * @ch: accel io_channel. @buf: 항상 ACCEL_BUFFER_BASE여야 함(검증용).
 * @domain: g_accel_domain이어야 함. @domain_ctx: 실제 반납할 accel_buffer.
 *
 * get_buf의 짝. domain_ctx가 진짜 accel_buffer이며, 도메인/주소가 accel 소유임을
 * assert로 확인한 뒤 accel_put_buf로 디스크립터(및 붙어 있던 iobuf)를 반납한다.
 *
 * 호출 체인:
 *   상위 사용자 → [이 함수] → accel_put_buf
 */
void
spdk_accel_put_buf(struct spdk_io_channel *ch, void *buf,
		   struct spdk_memory_domain *domain, void *domain_ctx)
{
	/* [한국어] accel 전용 채널 컨텍스트와 실제 버퍼 핸들(domain_ctx) 복원. */
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct accel_buffer *accel_buf = domain_ctx;

	/* [한국어] 넘어온 도메인/주소가 accel 소유 마커와 일치하는지 검증(오용 방지). */
	assert(domain == g_accel_domain);
	assert(buf == ACCEL_BUFFER_BASE);

	/* [한국어] 디스크립터(및 할당돼 있던 iobuf) 반납. */
	accel_put_buf(accel_ch, accel_buf);
}

/*
 * [한국어]
 * accel_sequence_complete_task - 시퀀스에서 task 하나를 떼어내 자원을 반납하고 그 단계의 콜백을 호출한다.
 *
 * @seq: 소속 시퀀스. @task: 완료 처리할 task.
 *
 * 시퀀스가 종료되거나 단계가 끝날 때, task를 리스트에서 제거하고 보유 aux를 풀에 반납한 뒤
 * task 자체를 풀로 돌려준다. 그 후 이 단계에 등록된 step_cb_fn을 호출해 상위에게 단계 완료를
 * 알린다(콜백을 task 반납 후에 호출하는 점이 중요 — 콜백 안에서 자원 재사용이 안전).
 * 채널 owner 스레드에서 실행.
 *
 * 호출 체인:
 *   accel_sequence_complete_tasks → [이 함수] → _put_task + step_cb_fn
 */
static void
accel_sequence_complete_task(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	/* [한국어] 자원 반납은 시퀀스 소유 채널 풀로. */
	struct accel_io_channel *ch = seq->ch;
	spdk_accel_step_cb cb_fn;
	void *cb_arg;

	/* [한국어] task를 시퀀스 리스트에서 제거. */
	TAILQ_REMOVE(&seq->tasks, task, seq_link);
	/* [한국어] 콜백 정보를 task 반납 전에 미리 캡처(반납 후 task 접근 금지). */
	cb_fn = task->step_cb_fn;
	cb_arg = task->cb_arg;
	/* [한국어] 시퀀스 역참조 끊기. */
	task->seq = NULL;
	/* [한국어] aux iov 슬롯을 잡고 있었으면 채널 aux 풀로 반납. */
	if (task->has_aux) {
		SLIST_INSERT_HEAD(&ch->task_aux_data_pool, task->aux, link);
		task->aux = NULL;
		task->has_aux = false;
	}

	/* [한국어] task를 채널 풀로 반납(재사용 가능 상태로). */
	_put_task(ch, task);

	/* [한국어] 자원 반납 후 단계 콜백 호출 — 콜백이 같은 자원을 재요청해도 안전. */
	if (cb_fn != NULL) {
		cb_fn(cb_arg);
	}
}

/*
 * [한국어]
 * accel_sequence_complete_tasks - 시퀀스에 남은 모든 task를 순서대로 완료 처리한다.
 *
 * @seq: 대상 시퀀스.
 *
 * 시퀀스 종료 시 미소비 task가 남아 있을 수 있으므로(에러 중단 포함) 리스트가 빌 때까지
 * accel_sequence_complete_task를 반복해 모든 단계 콜백을 호출하고 자원을 회수한다.
 *
 * 호출 체인:
 *   accel_sequence_complete → [이 함수] → accel_sequence_complete_task
 */
static void
accel_sequence_complete_tasks(struct spdk_accel_sequence *seq)
{
	struct spdk_accel_task *task;

	/* [한국어] task 리스트가 빌 때까지 머리부터 하나씩 완료 처리. */
	while (!TAILQ_EMPTY(&seq->tasks)) {
		task = TAILQ_FIRST(&seq->tasks);
		accel_sequence_complete_task(seq, task);
	}
}

/*
 * [한국어]
 * accel_sequence_complete - 시퀀스 실행을 최종 종료한다: 통계 갱신 → 단계 콜백들 → 시퀀스 반납 → 사용자 완료 콜백.
 *
 * @seq: 완료할 시퀀스(상태/status는 이미 확정됨).
 *
 * 상태머신(accel_process_sequence)이 모든 단계를 마쳤거나 에러로 중단됐을 때 도달하는
 * 종착점. 순서가 중요하다: (1) 시퀀스 실행/실패 통계 갱신, (2) 각 단계 사용자에게 단계
 * 완료 통지(complete_tasks), (3) 시퀀스 객체 반납(바운스 버퍼 해제 포함), (4) 마지막으로
 * 시퀀스를 시작한 사용자에게 최종 status로 완료 콜백 호출. cb_fn은 채널 owner 스레드
 * 컨텍스트에서 동기적으로 호출된다.
 *
 * 호출 체인:
 *   accel_process_sequence(완료/에러 상태) → [이 함수] → accel_sequence_put + seq->cb_fn
 */
static void
accel_sequence_complete(struct spdk_accel_sequence *seq)
{
	/* [한국어] 사용자 최종 완료 콜백 정보와 누적 status를 반납 전에 캡처. */
	spdk_accel_completion_cb cb_fn = seq->cb_fn;
	void *cb_arg = seq->cb_arg;
	int status = seq->status;

	SPDK_DEBUGLOG(accel, "Completed sequence: %p with status: %d\n", seq, status);

	/* [한국어] 시퀀스 실행 완료 통계 +1. */
	accel_update_stats(seq->ch, sequence_executed, 1);
	/* [한국어] 실패(status!=0)면 실패 통계도 +1(드문 경로). */
	if (spdk_unlikely(status != 0)) {
		accel_update_stats(seq->ch, sequence_failed, 1);
	}

	/* First notify all users that appended operations to this sequence */
	/* [한국어] (1) 각 단계 추가자에게 단계 완료 통지 + task 자원 회수. */
	accel_sequence_complete_tasks(seq);
	/* [한국어] (2) 시퀀스 객체 반납(바운스 버퍼 해제 포함). */
	accel_sequence_put(seq);

	/* Then notify the user that finished the sequence */
	/* [한국어] (3) 시퀀스를 시작한 사용자에게 최종 status로 완료 콜백 호출. */
	cb_fn(cb_arg, status);
}

/*
 * [한국어]
 * accel_update_virt_iov - 바운스 버퍼 내 오프셋을 보존하며, 가상(accel) iov를 실제 할당된 버퍼 주소로 치환한다.
 *
 * @diov: (out) 실제 주소로 채워질 목적지 iov. @siov: 원본(accel 도메인) iov.
 * @accel_buf: 실제 데이터 메모리가 막 할당된 accel_buffer.
 *
 * spdk_accel_get_buf가 돌려준 가짜 주소(ACCEL_BUFFER_BASE + 페이지내 오프셋)에서 하위
 * 오프셋 비트만 추출해, 진짜 할당 버퍼(accel_buf->buf) 기준 같은 오프셋 위치로 매핑한다.
 * 즉 상위가 ACCEL_BUFFER_BASE 기준으로 잡아 둔 상대 위치를 실제 메모리에 그대로 재현한다.
 *
 * 호출 체인:
 *   accel_sequence_set_virtbuf → [이 함수]
 */
static void
accel_update_virt_iov(struct iovec *diov, struct iovec *siov, struct accel_buffer *accel_buf)
{
	uintptr_t offset;

	/* [한국어] 가짜 베이스 주소에서 페이지내 오프셋 비트만 추출(상대 위치). */
	offset = (uintptr_t)siov->iov_base & ACCEL_BUFFER_OFFSET_MASK;
	/* [한국어] 오프셋은 버퍼 길이 안이어야 함(범위 검증). */
	assert(offset < accel_buf->len);

	/* [한국어] 실제 할당 버퍼 + 같은 오프셋으로 주소 치환, 길이는 원본 유지. */
	diov->iov_base = (char *)accel_buf->buf + offset;
	diov->iov_len = siov->iov_len;
}

/*
 * [한국어]
 * accel_sequence_set_virtbuf - 방금 실제 메모리가 할당된 accel_buffer를 참조하던 시퀀스 내 모든 task의 가상 iov를 실제 주소로 교체한다.
 *
 * @seq: 대상 시퀀스. @buf: 실제 데이터 버퍼가 막 할당된 accel_buffer.
 *
 * spdk_accel_get_buf로 만든 "지연 버퍼"는 처음엔 accel 메모리 도메인(g_accel_domain) +
 * 가짜 주소로 표현된다. 실행 시점에 이 버퍼에 진짜 iobuf가 붙으면(accel_iobuf_get_virtbuf_cb),
 * 그 버퍼를 src/dst로 쓰는 모든 task를 찾아 aux의 VIRT 슬롯에 실제 주소 iov를 채우고,
 * domain을 NULL(=호스트 일반 메모리)로 내려 백엔드가 바로 접근할 수 있게 만든다.
 * 같은 accel_buffer를 여러 task가 공유할 수 있으므로 시퀀스 전체를 순회한다.
 *
 * 호출 체인:
 *   accel_iobuf_get_virtbuf_cb / check_virtbuf / alloc_sequence_buf → [이 함수] → accel_update_virt_iov
 */
static void
accel_sequence_set_virtbuf(struct spdk_accel_sequence *seq, struct accel_buffer *buf)
{
	struct spdk_accel_task *task;
	struct iovec *iov;

	/* Now that we've allocated the actual data buffer for this accel_buffer, update all tasks
	 * in a sequence that were using it.
	 */
	/* [한국어] 이 accel_buffer를 src 또는 dst로 참조하는 모든 task를 순회. */
	TAILQ_FOREACH(task, &seq->tasks, seq_link) {
		/* [한국어] 소스가 이 accel 버퍼인 task: 가상 src를 실제 주소로 치환. */
		if (task->src_domain == g_accel_domain && task->src_domain_ctx == buf) {
			/* [한국어] 치환된 iov를 담을 aux 슬롯이 없으면 채널 aux 풀에서 확보. */
			if (!task->has_aux) {
				task->aux = SLIST_FIRST(&task->accel_ch->task_aux_data_pool);
				assert(task->aux && "Can't allocate aux data structure");
				task->has_aux = true;
				SLIST_REMOVE_HEAD(&task->accel_ch->task_aux_data_pool, link);
			}

			/* [한국어] aux의 VIRT_SRC 슬롯에 실제 주소 iov 작성(가상 버퍼는 단일 iov 가정). */
			iov = &task->aux->iovs[SPDK_ACCEL_AXU_IOV_VIRT_SRC];
			assert(task->s.iovcnt == 1);
			accel_update_virt_iov(iov, &task->s.iovs[0], buf);
			/* [한국어] 이제 실제 호스트 메모리이므로 도메인 NULL로 내리고 iov 교체. */
			task->src_domain = NULL;
			task->s.iovs = iov;
		}
		/* [한국어] 목적지가 이 accel 버퍼인 task: 가상 dst를 실제 주소로 치환. */
		if (task->dst_domain == g_accel_domain && task->dst_domain_ctx == buf) {
			/* [한국어] aux 슬롯 확보(없으면). */
			if (!task->has_aux) {
				task->aux = SLIST_FIRST(&task->accel_ch->task_aux_data_pool);
				assert(task->aux && "Can't allocate aux data structure");
				task->has_aux = true;
				SLIST_REMOVE_HEAD(&task->accel_ch->task_aux_data_pool, link);
			}

			/* [한국어] aux의 VIRT_DST 슬롯에 실제 주소 iov 작성. */
			iov = &task->aux->iovs[SPDK_ACCEL_AXU_IOV_VIRT_DST];
			assert(task->d.iovcnt == 1);
			accel_update_virt_iov(iov, &task->d.iovs[0], buf);
			/* [한국어] 도메인 NULL로 내리고 iov 교체. */
			task->dst_domain = NULL;
			task->d.iovs = iov;
		}
	}
}

/* [한국어] 시퀀스 상태머신 본체 전방 선언 — 아래 iobuf 콜백들이 비동기 완료 후 상태머신을 재가동하기 위해 참조. */
static void accel_process_sequence(struct spdk_accel_sequence *seq);

/*
 * [한국어]
 * accel_iobuf_get_virtbuf_cb - 지연 accel 버퍼의 iobuf 할당이 대기 후 성공했을 때 호출되는 콜백. 시퀀스 상태머신을 재가동한다.
 *
 * @entry: spdk_iobuf 대기 엔트리(accel_buffer.iobuf에 임베드됨). @buf: 막 할당된 실제 메모리.
 *
 * spdk_iobuf_get이 즉시 할당 실패해 대기 큐에 들어갔다가, iobuf가 반납되어 가용해지면
 * iobuf 서브시스템이 이 콜백을 호출한다. CONTAINEROF로 임베드된 accel_buffer를 복원하고
 * 실제 버퍼를 연결한 뒤, 상태를 AWAIT_VIRTBUF→CHECK_VIRTBUF로 진행하고 가상 iov를 실제
 * 주소로 치환한 다음 상태머신(accel_process_sequence)을 다시 돌린다. 채널 owner 스레드
 * 컨텍스트에서 실행되며 재진입 가드(in_process_sequence)로 보호된다.
 *
 * 호출 체인:
 *   spdk_iobuf(메모리 가용) → [이 함수] → accel_sequence_set_virtbuf → accel_process_sequence
 */
static void
accel_iobuf_get_virtbuf_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct accel_buffer *accel_buf;

	/* [한국어] iobuf 엔트리에서 임베드된 accel_buffer 복원(container_of 패턴). */
	accel_buf = SPDK_CONTAINEROF(entry, struct accel_buffer, iobuf);

	/* [한국어] 대기 중이던 버퍼는 소유 시퀀스가 있어야 하고 아직 메모리 미할당이어야 함. */
	assert(accel_buf->seq != NULL);
	assert(accel_buf->buf == NULL);
	/* [한국어] 막 가용해진 실제 메모리 연결. */
	accel_buf->buf = buf;

	/* [한국어] 이 콜백은 AWAIT_VIRTBUF 상태에서만 도착해야 함 → CHECK_VIRTBUF로 전이. */
	assert(accel_buf->seq->state == ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF);
	accel_sequence_set_state(accel_buf->seq, ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF);
	/* [한국어] 이 버퍼를 쓰는 task들의 가상 iov를 실제 주소로 치환. */
	accel_sequence_set_virtbuf(accel_buf->seq, accel_buf);
	/* [한국어] 상태머신 재가동 — 다음 단계로 진행. */
	accel_process_sequence(accel_buf->seq);
}

/*
 * [한국어]
 * accel_sequence_alloc_buf - accel_buffer에 실제 iobuf 메모리를 할당한다(동기 성공 또는 비동기 대기 등록).
 *
 * @seq: 소유 시퀀스. @buf: 메모리를 붙일 accel_buffer.
 * @cb_fn: 즉시 할당 실패 시 메모리 가용해질 때 호출될 iobuf 콜백.
 * @return: true=즉시 할당 성공(또는 이미 할당됨), false=대기 등록됨(비동기로 cb_fn 호출 예정).
 *
 * 시퀀스가 중간 버퍼/바운스 버퍼의 실제 메모리를 확보할 때 쓰는 공통 헬퍼. 소유 시퀀스를
 * 역참조로 박아 두고, 메모리 도메인 변환 과정에서 이미 버퍼가 붙었으면 즉시 성공으로
 * 반환한다. 아니면 spdk_iobuf_get을 호출하는데, hugepage 기반 iobuf mempool이 고갈되면
 * NULL을 돌려주며 대기 큐에 등록되고(retry.iobuf 통계 +1) false를 반환한다 — 이 경우
 * 호출자는 시퀀스를 AWAIT 상태로 두고 콜백을 기다린다.
 *
 * 호출 체인:
 *   check_virtbuf / check_bouncebuf / alloc_sequence_buf → [이 함수] → spdk_iobuf_get
 */
static bool
accel_sequence_alloc_buf(struct spdk_accel_sequence *seq, struct accel_buffer *buf,
			 spdk_iobuf_get_cb cb_fn)
{
	struct accel_io_channel *ch = seq->ch;

	/* [한국어] 같은 버퍼를 두 시퀀스가 잡는 일이 없어야 함(불변식). */
	assert(buf->seq == NULL);

	/* [한국어] 메모리 가용 콜백이 이 시퀀스를 다시 깨우도록 소유 시퀀스 역참조. */
	buf->seq = seq;

	/* Buffer might be already allocated by memory domain translation. */
	/* [한국어] 메모리 도메인 변환 과정에서 이미 실제 메모리가 붙었으면 바로 성공. */
	if (buf->buf) {
		return true;
	}

	/* [한국어] iobuf mempool(hugepage 기반)에서 len 크기 버퍼 요청 — 실패 시 대기 엔트리 등록. */
	buf->buf = spdk_iobuf_get(&ch->iobuf, buf->len, &buf->iobuf, cb_fn);
	if (spdk_unlikely(buf->buf == NULL)) {
		/* [한국어] 즉시 실패 → 대기 등록됨. 재시도 통계 +1, false 반환(비동기 진행). */
		accel_update_stats(ch, retry.iobuf, 1);
		return false;
	}

	return true;
}

/*
 * [한국어]
 * accel_sequence_check_virtbuf - 현재 task가 참조하는 가상(accel 도메인) 버퍼들의 실제 메모리를 확보하고 iov를 치환한다.
 *
 * @seq: 진행 중 시퀀스. @task: 현재 단계 task.
 * @return: true=모든 가상 버퍼 실체화 완료(다음 단계 진행 가능), false=메모리 대기 중(비동기).
 *
 * accel_process_sequence 상태머신의 CHECK_VIRTBUF 단계 핸들러. task의 src/dst가 accel
 * 메모리 도메인(g_accel_domain)이면 지연 버퍼이므로 실제 iobuf를 할당하고 가상 iov를 실제
 * 주소로 치환한다. fill/crc32 처럼 src/dst가 없는 연산은 도메인이 NULL이라 건너뛴다.
 * 메모리가 즉시 안 잡히면 false를 반환해 상태머신을 AWAIT_VIRTBUF로 멈춘다.
 *
 * 호출 체인:
 *   accel_process_sequence(CHECK_VIRTBUF) → [이 함수] → accel_sequence_alloc_buf + set_virtbuf
 */
static bool
accel_sequence_check_virtbuf(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	/* If a task doesn't have dst/src (e.g. fill, crc32), its dst/src domain should be set to
	 * NULL */
	/* [한국어] 소스가 accel 지연 버퍼면 실제 메모리 확보(대기 시 false). */
	if (task->src_domain == g_accel_domain) {
		if (!accel_sequence_alloc_buf(seq, task->src_domain_ctx,
					      accel_iobuf_get_virtbuf_cb)) {
			return false;
		}

		/* [한국어] 할당 성공 — 소스 가상 iov를 실제 주소로 치환. */
		accel_sequence_set_virtbuf(seq, task->src_domain_ctx);
	}

	/* [한국어] 목적지가 accel 지연 버퍼면 동일하게 실제 메모리 확보. */
	if (task->dst_domain == g_accel_domain) {
		if (!accel_sequence_alloc_buf(seq, task->dst_domain_ctx,
					      accel_iobuf_get_virtbuf_cb)) {
			return false;
		}

		/* [한국어] 목적지 가상 iov 치환. */
		accel_sequence_set_virtbuf(seq, task->dst_domain_ctx);
	}

	return true;
}

/*
 * [한국어]
 * accel_sequence_get_buf_cb - spdk_accel_alloc_sequence_buf의 비동기 메모리 대기 완료 콜백. 사용자 cb_fn으로 위임한다.
 *
 * @entry: iobuf 대기 엔트리(accel_buffer.iobuf). @buf: 막 할당된 실제 메모리.
 *
 * spdk_accel_alloc_sequence_buf가 즉시 메모리를 못 잡아 대기했다가 성공할 때 호출된다.
 * 버퍼를 실체화하고 가상 iov를 치환한 뒤, 상태머신이 아니라 사용자가 등록한 cb_fn을 직접
 * 호출한다(이 경로는 상태머신 외부에서 명시적으로 버퍼만 미리 잡는 용도). 채널 owner 스레드.
 *
 * 호출 체인:
 *   spdk_iobuf(메모리 가용) → [이 함수] → accel_buf->cb_fn
 */
static void
accel_sequence_get_buf_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct accel_buffer *accel_buf;

	/* [한국어] iobuf 엔트리에서 accel_buffer 복원. */
	accel_buf = SPDK_CONTAINEROF(entry, struct accel_buffer, iobuf);

	/* [한국어] 소유 시퀀스 존재 + 미할당 상태 불변식 확인 후 실제 메모리 연결. */
	assert(accel_buf->seq != NULL);
	assert(accel_buf->buf == NULL);
	accel_buf->buf = buf;

	/* [한국어] 가상 iov 치환 후, 상태머신 대신 사용자가 등록한 콜백을 직접 호출. */
	accel_sequence_set_virtbuf(accel_buf->seq, accel_buf);
	accel_buf->cb_fn(accel_buf->seq, accel_buf->cb_ctx);
}

/*
 * [한국어]
 * spdk_accel_alloc_sequence_buf - 시퀀스의 지연 accel 버퍼에 실제 메모리를 미리(명시적으로) 할당한다.
 *
 * @seq: 대상 시퀀스. @buf: 항상 ACCEL_BUFFER_BASE(검증 없음). @domain: g_accel_domain이어야 함.
 * @domain_ctx: 실제 accel_buffer. @cb_fn/@cb_ctx: 비동기 할당 완료 콜백/컨텍스트.
 * @return: true=즉시 할당 성공, false=대기 중(나중에 cb_fn 호출).
 *
 * 상위 모듈이 시퀀스 실행 전에 중간 버퍼 메모리를 선점하고 싶을 때 쓰는 공개 API. 즉시
 * 성공하면 true, mempool 고갈로 대기하면 false를 반환하고 나중에 cb_fn을 호출한다.
 *
 * 호출 체인:
 *   상위 모듈 → [이 함수] → accel_sequence_alloc_buf
 */
bool
spdk_accel_alloc_sequence_buf(struct spdk_accel_sequence *seq, void *buf,
			      struct spdk_memory_domain *domain, void *domain_ctx,
			      spdk_accel_sequence_get_buf_cb cb_fn, void *cb_ctx)
{
	/* [한국어] domain_ctx가 실제 accel_buffer. */
	struct accel_buffer *accel_buf = domain_ctx;

	/* [한국어] accel 소유 도메인인지 검증하고 사용자 콜백 등록. */
	assert(domain == g_accel_domain);
	accel_buf->cb_fn = cb_fn;
	accel_buf->cb_ctx = cb_ctx;

	/* [한국어] 실제 메모리 확보 시도 — 대기면 false 반환(콜백으로 이어짐). */
	if (!accel_sequence_alloc_buf(seq, accel_buf, accel_sequence_get_buf_cb)) {
		return false;
	}

	/* [한국어] 즉시 성공 — 가상 iov 치환 후 true. */
	accel_sequence_set_virtbuf(seq, accel_buf);

	return true;
}

/*
 * [한국어]
 * spdk_accel_sequence_first_task - 시퀀스의 첫 번째 task를 반환한다(모듈이 시퀀스를 순회할 때 사용).
 *
 * @seq: 대상 시퀀스. @return: 첫 task, 비어 있으면 NULL.
 *
 * accel 드라이버/모듈이 시퀀스 단계를 직접 검사·병합·실행할 때 순회 시작점을 제공하는
 * 단순 접근자. TAILQ_FIRST 래퍼.
 *
 * 호출 체인:
 *   모듈/드라이버 → [이 함수] → TAILQ_FIRST
 */
struct spdk_accel_task *
spdk_accel_sequence_first_task(struct spdk_accel_sequence *seq)
{
	/* [한국어] task 리스트 머리 반환. */
	return TAILQ_FIRST(&seq->tasks);
}

/*
 * [한국어]
 * spdk_accel_sequence_next_task - 주어진 task의 다음 task를 반환한다(시퀀스 순회).
 *
 * @task: 현재 task. @return: 다음 task, 마지막이면 NULL.
 *
 * first_task와 짝을 이루는 순회 접근자. TAILQ_NEXT 래퍼.
 *
 * 호출 체인:
 *   모듈/드라이버 → [이 함수] → TAILQ_NEXT
 */
struct spdk_accel_task *
spdk_accel_sequence_next_task(struct spdk_accel_task *task)
{
	/* [한국어] seq_link 기준 다음 task 반환. */
	return TAILQ_NEXT(task, seq_link);
}

/*
 * [한국어]
 * accel_set_bounce_buffer - task의 src/dst를 원본에서 바운스 버퍼로 바꾸고, 원본 정보를 보관한다.
 *
 * @bounce: 원본 보관 + 바운스 iov를 담을 구조체(task->aux->bounce.s 또는 .d).
 * @iovs/@iovcnt/@domain/@domain_ctx: (in/out) task의 src 또는 dst 필드 포인터들.
 * @buf: 실제 메모리가 할당된 바운스 accel_buffer.
 *
 * 메모리 도메인 변환(pull/push)을 위해, 백엔드가 직접 접근 못 하는 원격 메모리(RDMA/GPU)를
 * 호스트 바운스 버퍼로 대체한다. 원본 iov/domain을 bounce->orig_*에 저장해 두었다가
 * 나중에 pull(읽기 전 원본→바운스 복사)/push(쓰기 후 바운스→원본 복사)로 복원한다.
 * 치환 후 task의 src/dst는 단일 바운스 iov + domain NULL(호스트 메모리)이 된다.
 *
 * 호출 체인:
 *   accel_iobuf_get_{src,dst}_bounce_cb / check_bouncebuf → [이 함수]
 */
static inline void
accel_set_bounce_buffer(struct spdk_accel_bounce_buffer *bounce, struct iovec **iovs,
			uint32_t *iovcnt, struct spdk_memory_domain **domain, void **domain_ctx,
			struct accel_buffer *buf)
{
	/* [한국어] 나중에 pull/push로 복원하기 위해 원본 iov/도메인 정보 보관. */
	bounce->orig_iovs = *iovs;
	bounce->orig_iovcnt = *iovcnt;
	bounce->orig_domain = *domain;
	bounce->orig_domain_ctx = *domain_ctx;
	/* [한국어] 바운스 단일 iov를 실제 할당 버퍼로 구성. */
	bounce->iov.iov_base = buf->buf;
	bounce->iov.iov_len = buf->len;

	/* [한국어] task의 src/dst를 바운스 단일 iov + 호스트 메모리(domain NULL)로 교체. */
	*iovs = &bounce->iov;
	*iovcnt = 1;
	*domain = NULL;
}

/*
 * [한국어]
 * accel_iobuf_get_src_bounce_cb - 소스 바운스 버퍼의 iobuf 비동기 할당 완료 콜백. 소스를 바운스로 치환하고 상태머신 재가동.
 *
 * @entry: iobuf 대기 엔트리. @buf: 막 할당된 실제 메모리.
 *
 * check_bouncebuf가 소스 바운스 메모리를 즉시 못 잡아 대기했다가 성공할 때 호출된다.
 * 실제 메모리를 연결하고, 현재(첫) task의 소스를 바운스 버퍼로 치환한 뒤 상태를
 * AWAIT_BOUNCEBUF→CHECK_BOUNCEBUF로 진행하고 상태머신을 다시 돌린다. 채널 owner 스레드.
 *
 * 호출 체인:
 *   spdk_iobuf(메모리 가용) → [이 함수] → accel_set_bounce_buffer → accel_process_sequence
 */
static void
accel_iobuf_get_src_bounce_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct spdk_accel_task *task;
	struct accel_buffer *accel_buf;

	/* [한국어] iobuf 엔트리에서 accel_buffer 복원 후 실제 메모리 연결(미할당 불변식 확인). */
	accel_buf = SPDK_CONTAINEROF(entry, struct accel_buffer, iobuf);
	assert(accel_buf->buf == NULL);
	accel_buf->buf = buf;

	/* [한국어] 바운스는 현재 처리 중인 첫 task에 대해 적용. */
	task = TAILQ_FIRST(&accel_buf->seq->tasks);
	assert(task != NULL);

	/* [한국어] 대기 콜백은 AWAIT_BOUNCEBUF에서만 도착 → CHECK_BOUNCEBUF로 전이. */
	assert(accel_buf->seq->state == ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF);
	accel_sequence_set_state(accel_buf->seq, ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF);
	/* [한국어] 바운스 정보를 담을 aux가 준비돼 있어야 함. */
	assert(task->aux);
	assert(task->has_aux);
	/* [한국어] 소스를 바운스 버퍼로 치환(원본은 aux->bounce.s에 보관). */
	accel_set_bounce_buffer(&task->aux->bounce.s, &task->s.iovs, &task->s.iovcnt, &task->src_domain,
				&task->src_domain_ctx, accel_buf);
	/* [한국어] 상태머신 재가동. */
	accel_process_sequence(accel_buf->seq);
}

/*
 * [한국어]
 * accel_iobuf_get_dst_bounce_cb - 목적지 바운스 버퍼의 iobuf 비동기 할당 완료 콜백. 목적지를 바운스로 치환하고 상태머신 재가동.
 *
 * @entry: iobuf 대기 엔트리. @buf: 막 할당된 실제 메모리.
 *
 * src_bounce_cb의 목적지 버전. 동작은 동일하나 task의 목적지(d.iovs 등)를 바운스로 치환하고
 * 원본을 aux->bounce.d에 보관한다.
 *
 * 호출 체인:
 *   spdk_iobuf(메모리 가용) → [이 함수] → accel_set_bounce_buffer → accel_process_sequence
 */
static void
accel_iobuf_get_dst_bounce_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct spdk_accel_task *task;
	struct accel_buffer *accel_buf;

	/* [한국어] accel_buffer 복원 후 실제 메모리 연결. */
	accel_buf = SPDK_CONTAINEROF(entry, struct accel_buffer, iobuf);
	assert(accel_buf->buf == NULL);
	accel_buf->buf = buf;

	/* [한국어] 현재 첫 task 대상. */
	task = TAILQ_FIRST(&accel_buf->seq->tasks);
	assert(task != NULL);

	/* [한국어] AWAIT_BOUNCEBUF→CHECK_BOUNCEBUF 전이. */
	assert(accel_buf->seq->state == ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF);
	accel_sequence_set_state(accel_buf->seq, ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF);
	assert(task->aux);
	assert(task->has_aux);
	/* [한국어] 목적지를 바운스 버퍼로 치환(원본은 aux->bounce.d에 보관). */
	accel_set_bounce_buffer(&task->aux->bounce.d, &task->d.iovs, &task->d.iovcnt, &task->dst_domain,
				&task->dst_domain_ctx, accel_buf);
	/* [한국어] 상태머신 재가동. */
	accel_process_sequence(accel_buf->seq);
}

/*
 * [한국어]
 * accel_sequence_check_bouncebuf - 현재 task의 src/dst가 비-호스트 메모리 도메인이면 호스트 바운스 버퍼를 마련해 치환한다.
 *
 * @seq: 진행 중 시퀀스. @task: 현재 단계 task.
 * @return: 0=치환 완료(진행 가능), -EAGAIN=메모리 대기 중(비동기), -ENOMEM=디스크립터 부족(영구 실패).
 *
 * accel_process_sequence 상태머신의 CHECK_BOUNCEBUF 단계 핸들러. 백엔드 모듈이 직접 접근할
 * 수 없는 원격 메모리(RDMA 등록 메모리, GPU 메모리 등 NULL이 아니면서 accel 도메인도 아닌
 * 도메인)를 만나면, 호스트 iobuf 바운스 버퍼를 잡아 task의 src/dst를 호스트 메모리로
 * 치환한다. 원본은 aux->bounce.s/.d에 보관되어 실행 전후 pull/push 복사로 데이터를 옮긴다.
 *
 * 각 방향에 대해: (1) 바운스 정보를 담을 aux 슬롯 확보, (2) buf 디스크립터 확보(부족 시
 * -ENOMEM), (3) 시퀀스 bounce_bufs 리스트에 등록(완료 시 일괄 해제), (4) 실제 iobuf 할당
 * (대기 시 -EAGAIN → 콜백으로 이어짐), (5) 바운스로 치환. 채널 owner 스레드.
 *
 * 호출 체인:
 *   accel_process_sequence(CHECK_BOUNCEBUF) → [이 함수] → accel_sequence_alloc_buf + accel_set_bounce_buffer
 */
static int
accel_sequence_check_bouncebuf(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	struct accel_buffer *buf;

	/* [한국어] 소스가 비-NULL 도메인이면 호스트가 직접 못 읽는 원격 메모리 → 바운스 필요. */
	if (task->src_domain != NULL) {
		/* By the time we're here, accel buffers should have been allocated */
		/* [한국어] 이 단계 이전에 accel 지연 버퍼는 이미 실체화됐어야 함(여기선 진짜 원격 도메인만). */
		assert(task->src_domain != g_accel_domain);

		/* [한국어] 바운스 원본 정보를 담을 aux 슬롯 확보(없으면). */
		if (!task->has_aux) {
			task->aux = SLIST_FIRST(&task->accel_ch->task_aux_data_pool);
			if (spdk_unlikely(!task->aux)) {
				/* [한국어] aux는 task당 항상 준비돼야 함 → 미할당은 설계 위반. */
				SPDK_ERRLOG("Can't allocate aux data structure\n");
				assert(0);
				return -EAGAIN;
			}
			task->has_aux = true;
			SLIST_REMOVE_HEAD(&task->accel_ch->task_aux_data_pool, link);
		}
		/* [한국어] 소스 전체 길이만큼의 바운스 버퍼 디스크립터 확보. */
		buf = accel_get_buf(seq->ch, accel_get_iovlen(task->s.iovs, task->s.iovcnt));
		if (buf == NULL) {
			SPDK_ERRLOG("Couldn't allocate buffer descriptor\n");
			return -ENOMEM;
		}

		/* [한국어] 시퀀스 완료 시 일괄 해제되도록 bounce_bufs 리스트에 등록. */
		SLIST_INSERT_HEAD(&seq->bounce_bufs, buf, link);
		/* [한국어] 실제 iobuf 메모리 할당 — 대기면 -EAGAIN(콜백으로 이어 진행). */
		if (!accel_sequence_alloc_buf(seq, buf, accel_iobuf_get_src_bounce_cb)) {
			return -EAGAIN;
		}

		/* [한국어] 소스를 바운스 버퍼로 치환(원본은 aux->bounce.s에 보관). */
		accel_set_bounce_buffer(&task->aux->bounce.s, &task->s.iovs, &task->s.iovcnt,
					&task->src_domain, &task->src_domain_ctx, buf);
	}

	/* [한국어] 목적지도 비-NULL 도메인이면 동일하게 바운스 처리. */
	if (task->dst_domain != NULL) {
		/* By the time we're here, accel buffers should have been allocated */
		/* [한국어] accel 지연 버퍼는 이미 실체화 완료(진짜 원격 도메인만 남음). */
		assert(task->dst_domain != g_accel_domain);

		/* [한국어] aux 슬롯 확보(없으면). */
		if (!task->has_aux) {
			task->aux = SLIST_FIRST(&task->accel_ch->task_aux_data_pool);
			if (spdk_unlikely(!task->aux)) {
				SPDK_ERRLOG("Can't allocate aux data structure\n");
				assert(0);
				return -EAGAIN;
			}
			task->has_aux = true;
			SLIST_REMOVE_HEAD(&task->accel_ch->task_aux_data_pool, link);
		}
		/* [한국어] 목적지 길이만큼 바운스 디스크립터 확보. */
		buf = accel_get_buf(seq->ch, accel_get_iovlen(task->d.iovs, task->d.iovcnt));
		if (buf == NULL) {
			/* The src buffer will be released when a sequence is completed */
			/* [한국어] 실패해도 앞서 잡은 소스 바운스는 시퀀스 완료 시 정리되므로 누수 없음. */
			SPDK_ERRLOG("Couldn't allocate buffer descriptor\n");
			return -ENOMEM;
		}

		/* [한국어] bounce_bufs 리스트 등록. */
		SLIST_INSERT_HEAD(&seq->bounce_bufs, buf, link);
		/* [한국어] 실제 iobuf 할당 — 대기면 -EAGAIN. */
		if (!accel_sequence_alloc_buf(seq, buf, accel_iobuf_get_dst_bounce_cb)) {
			return -EAGAIN;
		}

		/* [한국어] 목적지를 바운스 버퍼로 치환(원본은 aux->bounce.d에 보관). */
		accel_set_bounce_buffer(&task->aux->bounce.d, &task->d.iovs, &task->d.iovcnt,
					&task->dst_domain, &task->dst_domain_ctx, buf);
	}

	return 0;
}

static void
accel_task_pull_data_cb(void *ctx, int status)
{
	struct spdk_accel_sequence *seq = ctx;

	assert(seq->state == ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA);
	if (spdk_likely(status == 0)) {
		accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_EXEC_TASK);
	} else {
		accel_sequence_set_fail(seq, status);
	}

	accel_process_sequence(seq);
}

static void
accel_task_pull_data(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	int rc;

	assert(task->has_aux);
	assert(task->aux);
	assert(task->aux->bounce.s.orig_iovs != NULL);
	assert(task->aux->bounce.s.orig_domain != NULL);
	assert(task->aux->bounce.s.orig_domain != g_accel_domain);
	assert(!g_modules_opc[task->op_code].supports_memory_domains);

	rc = spdk_memory_domain_pull_data(task->aux->bounce.s.orig_domain,
					  task->aux->bounce.s.orig_domain_ctx,
					  task->aux->bounce.s.orig_iovs, task->aux->bounce.s.orig_iovcnt,
					  task->s.iovs, task->s.iovcnt,
					  accel_task_pull_data_cb, seq);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to pull data from memory domain: %s, rc: %d\n",
			    spdk_memory_domain_get_dma_device_id(task->aux->bounce.s.orig_domain), rc);
		accel_sequence_set_fail(seq, rc);
	}
}

static void
accel_task_push_data_cb(void *ctx, int status)
{
	struct spdk_accel_sequence *seq = ctx;

	assert(seq->state == ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA);
	if (spdk_likely(status == 0)) {
		accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_NEXT_TASK);
	} else {
		accel_sequence_set_fail(seq, status);
	}

	accel_process_sequence(seq);
}

static void
accel_task_push_data(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	int rc;

	assert(task->has_aux);
	assert(task->aux);
	assert(task->aux->bounce.d.orig_iovs != NULL);
	assert(task->aux->bounce.d.orig_domain != NULL);
	assert(task->aux->bounce.d.orig_domain != g_accel_domain);
	assert(!g_modules_opc[task->op_code].supports_memory_domains);

	rc = spdk_memory_domain_push_data(task->aux->bounce.d.orig_domain,
					  task->aux->bounce.d.orig_domain_ctx,
					  task->aux->bounce.d.orig_iovs, task->aux->bounce.d.orig_iovcnt,
					  task->d.iovs, task->d.iovcnt,
					  accel_task_push_data_cb, seq);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to push data to memory domain: %s, rc: %d\n",
			    spdk_memory_domain_get_dma_device_id(task->aux->bounce.d.orig_domain), rc);
		accel_sequence_set_fail(seq, rc);
	}
}

static void
accel_process_sequence(struct spdk_accel_sequence *seq)
{
	struct accel_io_channel *accel_ch = seq->ch;
	struct spdk_accel_task *task;
	enum accel_sequence_state state;
	int rc;

	/* Prevent recursive calls to this function */
	if (spdk_unlikely(seq->in_process_sequence)) {
		return;
	}
	seq->in_process_sequence = true;

	task = TAILQ_FIRST(&seq->tasks);
	do {
		state = seq->state;
		switch (state) {
		case ACCEL_SEQUENCE_STATE_INIT:
			if (g_accel_driver != NULL) {
				accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_DRIVER_EXEC_TASKS);
				break;
			}
		/* Fall through */
		case ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF:
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF);
			if (!accel_sequence_check_virtbuf(seq, task)) {
				/* We couldn't allocate a buffer, wait until one is available */
				break;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF);
		/* Fall through */
		case ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF:
			/* If a module supports memory domains, we don't need to allocate bounce
			 * buffers */
			if (g_modules_opc[task->op_code].supports_memory_domains) {
				accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_EXEC_TASK);
				break;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF);
			rc = accel_sequence_check_bouncebuf(seq, task);
			if (spdk_unlikely(rc != 0)) {
				/* We couldn't allocate a buffer, wait until one is available */
				if (rc == -EAGAIN) {
					break;
				}
				accel_sequence_set_fail(seq, rc);
				break;
			}
			if (task->has_aux && task->s.iovs == &task->aux->bounce.s.iov) {
				assert(task->aux->bounce.s.orig_iovs);
				accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_PULL_DATA);
				break;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_EXEC_TASK);
		/* Fall through */
		case ACCEL_SEQUENCE_STATE_EXEC_TASK:
			SPDK_DEBUGLOG(accel, "Executing %s operation, sequence: %p\n",
				      g_opcode_strings[task->op_code], seq);

			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_TASK);
			rc = accel_submit_task(accel_ch, task);
			if (spdk_unlikely(rc != 0)) {
				SPDK_ERRLOG("Failed to submit %s operation, sequence: %p\n",
					    g_opcode_strings[task->op_code], seq);
				accel_sequence_set_fail(seq, rc);
			}
			break;
		case ACCEL_SEQUENCE_STATE_PULL_DATA:
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA);
			accel_task_pull_data(seq, task);
			break;
		case ACCEL_SEQUENCE_STATE_COMPLETE_TASK:
			if (task->has_aux && task->d.iovs == &task->aux->bounce.d.iov) {
				assert(task->aux->bounce.d.orig_iovs);
				accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_PUSH_DATA);
				break;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_NEXT_TASK);
			break;
		case ACCEL_SEQUENCE_STATE_PUSH_DATA:
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA);
			accel_task_push_data(seq, task);
			break;
		case ACCEL_SEQUENCE_STATE_NEXT_TASK:
			accel_sequence_complete_task(seq, task);
			/* Check if there are any remaining tasks */
			task = TAILQ_FIRST(&seq->tasks);
			if (task == NULL) {
				/* Immediately return here to make sure we don't touch the sequence
				 * after it's completed */
				accel_sequence_complete(seq);
				return;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_INIT);
			break;
		case ACCEL_SEQUENCE_STATE_DRIVER_EXEC_TASKS:
			assert(!TAILQ_EMPTY(&seq->tasks));

			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_DRIVER_AWAIT_TASKS);
			rc = g_accel_driver->execute_sequence(accel_ch->driver_channel, seq);
			if (spdk_unlikely(rc != 0)) {
				SPDK_ERRLOG("Failed to execute sequence: %p using driver: %s\n",
					    seq, g_accel_driver->name);
				accel_sequence_set_fail(seq, rc);
			}
			break;
		case ACCEL_SEQUENCE_STATE_DRIVER_COMPLETE_TASKS:
			/* Get the task again, as the driver might have completed some tasks
			 * synchronously */
			task = TAILQ_FIRST(&seq->tasks);
			if (task == NULL) {
				/* Immediately return here to make sure we don't touch the sequence
				 * after it's completed */
				accel_sequence_complete(seq);
				return;
			}
			/* We don't want to execute the next task through the driver, so we
			 * explicitly omit the INIT state here */
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF);
			break;
		case ACCEL_SEQUENCE_STATE_ERROR:
			/* Immediately return here to make sure we don't touch the sequence
			 * after it's completed */
			assert(seq->status != 0);
			accel_sequence_complete(seq);
			return;
		case ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF:
		case ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF:
		case ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA:
		case ACCEL_SEQUENCE_STATE_AWAIT_TASK:
		case ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA:
		case ACCEL_SEQUENCE_STATE_DRIVER_AWAIT_TASKS:
			break;
		default:
			assert(0 && "bad state");
			break;
		}
	} while (seq->state != state);

	seq->in_process_sequence = false;
}

static void
accel_sequence_task_cb(struct spdk_accel_sequence *seq, struct spdk_accel_task *task, int status)
{
	switch (seq->state) {
	case ACCEL_SEQUENCE_STATE_AWAIT_TASK:
		accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_COMPLETE_TASK);
		if (spdk_unlikely(status != 0)) {
			SPDK_ERRLOG("Failed to execute %s operation, sequence: %p\n",
				    g_opcode_strings[task->op_code], seq);
			accel_sequence_set_fail(seq, status);
		}

		accel_process_sequence(seq);
		break;
	case ACCEL_SEQUENCE_STATE_DRIVER_AWAIT_TASKS:
		assert(g_accel_driver != NULL);
		/* Immediately remove the task from the outstanding list to make sure the next call
		 * to spdk_accel_sequence_first_task() doesn't return it */
		accel_sequence_complete_task(seq, task);
		if (spdk_unlikely(status != 0)) {
			SPDK_ERRLOG("Failed to execute %s operation, sequence: %p through "
				    "driver: %s\n", g_opcode_strings[task->op_code], seq,
				    g_accel_driver->name);
			/* Update status without using accel_sequence_set_fail() to avoid changing
			 * seq's state to ERROR until driver calls spdk_accel_sequence_continue() */
			seq->status = status;
		}
		break;
	default:
		assert(0 && "bad state");
		break;
	}
}

void
spdk_accel_sequence_continue(struct spdk_accel_sequence *seq)
{
	assert(g_accel_driver != NULL);
	assert(seq->state == ACCEL_SEQUENCE_STATE_DRIVER_AWAIT_TASKS);

	if (spdk_likely(seq->status == 0)) {
		accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_DRIVER_COMPLETE_TASKS);
	} else {
		accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_ERROR);
	}

	accel_process_sequence(seq);
}

static bool
accel_compare_iovs(struct iovec *iova, uint32_t iovacnt, struct iovec *iovb, uint32_t iovbcnt)
{
	/* For now, just do a dumb check that the iovecs arrays are exactly the same */
	if (iovacnt != iovbcnt) {
		return false;
	}

	return memcmp(iova, iovb, sizeof(*iova) * iovacnt) == 0;
}

static bool
accel_task_set_dstbuf(struct spdk_accel_task *task, struct spdk_accel_task *next)
{
	struct spdk_accel_task *prev;

	switch (task->op_code) {
	case SPDK_ACCEL_OPC_DECOMPRESS:
	case SPDK_ACCEL_OPC_FILL:
	case SPDK_ACCEL_OPC_ENCRYPT:
	case SPDK_ACCEL_OPC_DECRYPT:
	case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
	case SPDK_ACCEL_OPC_DIF_VERIFY_COPY:
		if (task->dst_domain != next->src_domain) {
			return false;
		}
		if (!accel_compare_iovs(task->d.iovs, task->d.iovcnt,
					next->s.iovs, next->s.iovcnt)) {
			return false;
		}
		task->d.iovs = next->d.iovs;
		task->d.iovcnt = next->d.iovcnt;
		task->dst_domain = next->dst_domain;
		task->dst_domain_ctx = next->dst_domain_ctx;
		break;
	case SPDK_ACCEL_OPC_CRC32C:
	case SPDK_ACCEL_OPC_DIX_GENERATE:
	case SPDK_ACCEL_OPC_DIX_VERIFY:
		/* crc32 and dix_generate/verify are special, because they do not have a dst buffer */
		if (task->src_domain != next->src_domain) {
			return false;
		}
		if (!accel_compare_iovs(task->s.iovs, task->s.iovcnt,
					next->s.iovs, next->s.iovcnt)) {
			return false;
		}
		/* We can only change operation's buffer if we can change previous task's buffer */
		prev = TAILQ_PREV(task, accel_sequence_tasks, seq_link);
		if (prev == NULL) {
			return false;
		}
		if (!accel_task_set_dstbuf(prev, next)) {
			return false;
		}
		task->s.iovs = next->d.iovs;
		task->s.iovcnt = next->d.iovcnt;
		task->src_domain = next->dst_domain;
		task->src_domain_ctx = next->dst_domain_ctx;
		break;
	default:
		return false;
	}

	return true;
}

static void
accel_sequence_merge_tasks(struct spdk_accel_sequence *seq, struct spdk_accel_task *task,
			   struct spdk_accel_task **next_task)
{
	struct spdk_accel_task *next = *next_task;

	switch (task->op_code) {
	case SPDK_ACCEL_OPC_COPY:
		/* We only allow changing src of operations that actually have a src, e.g. we never
		 * do it for fill.  Theoretically, it is possible, but we'd have to be careful to
		 * change the src of the operation after fill (which in turn could also be a fill).
		 * So, for the sake of simplicity, skip this type of operations for now.
		 */
		if (next->op_code != SPDK_ACCEL_OPC_DECOMPRESS &&
		    next->op_code != SPDK_ACCEL_OPC_COPY &&
		    next->op_code != SPDK_ACCEL_OPC_ENCRYPT &&
		    next->op_code != SPDK_ACCEL_OPC_DECRYPT &&
		    next->op_code != SPDK_ACCEL_OPC_COPY_CRC32C &&
		    next->op_code != SPDK_ACCEL_OPC_DIF_GENERATE_COPY &&
		    next->op_code != SPDK_ACCEL_OPC_DIF_VERIFY_COPY) {
			break;
		}
		if (task->dst_domain != next->src_domain) {
			break;
		}
		if (!accel_compare_iovs(task->d.iovs, task->d.iovcnt,
					next->s.iovs, next->s.iovcnt)) {
			break;
		}
		next->s.iovs = task->s.iovs;
		next->s.iovcnt = task->s.iovcnt;
		next->src_domain = task->src_domain;
		next->src_domain_ctx = task->src_domain_ctx;
		accel_sequence_complete_task(seq, task);
		break;
	case SPDK_ACCEL_OPC_DECOMPRESS:
	case SPDK_ACCEL_OPC_FILL:
	case SPDK_ACCEL_OPC_ENCRYPT:
	case SPDK_ACCEL_OPC_DECRYPT:
	case SPDK_ACCEL_OPC_CRC32C:
	case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
	case SPDK_ACCEL_OPC_DIF_VERIFY_COPY:
	case SPDK_ACCEL_OPC_DIX_GENERATE:
	case SPDK_ACCEL_OPC_DIX_VERIFY:
		/* We can only merge tasks when one of them is a copy */
		if (next->op_code != SPDK_ACCEL_OPC_COPY) {
			break;
		}
		if (!accel_task_set_dstbuf(task, next)) {
			break;
		}
		/* We're removing next_task from the tasks queue, so we need to update its pointer,
		 * so that the TAILQ_FOREACH_SAFE() loop below works correctly */
		*next_task = TAILQ_NEXT(next, seq_link);
		accel_sequence_complete_task(seq, next);
		break;
	default:
		assert(0 && "bad opcode");
		break;
	}
}

void
spdk_accel_sequence_finish(struct spdk_accel_sequence *seq,
			   spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *task, *next;

	/* Try to remove any copy operations if possible */
	TAILQ_FOREACH_SAFE(task, &seq->tasks, seq_link, next) {
		if (next == NULL) {
			break;
		}
		accel_sequence_merge_tasks(seq, task, &next);
	}

	seq->cb_fn = cb_fn;
	seq->cb_arg = cb_arg;

	accel_process_sequence(seq);
}

void
spdk_accel_sequence_reverse(struct spdk_accel_sequence *seq)
{
	struct accel_sequence_tasks tasks = TAILQ_HEAD_INITIALIZER(tasks);
	struct spdk_accel_task *task;

	TAILQ_SWAP(&tasks, &seq->tasks, spdk_accel_task, seq_link);

	while (!TAILQ_EMPTY(&tasks)) {
		task = TAILQ_FIRST(&tasks);
		TAILQ_REMOVE(&tasks, task, seq_link);
		TAILQ_INSERT_HEAD(&seq->tasks, task, seq_link);
	}
}

void
spdk_accel_sequence_abort(struct spdk_accel_sequence *seq)
{
	if (seq == NULL) {
		return;
	}

	accel_sequence_complete_tasks(seq);
	accel_sequence_put(seq);
}

struct spdk_memory_domain *
spdk_accel_get_memory_domain(void)
{
	return g_accel_domain;
}

static struct spdk_accel_module_if *
_module_find_by_name(const char *name)
{
	struct spdk_accel_module_if *accel_module = NULL;

	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		if (strcmp(name, accel_module->name) == 0) {
			break;
		}
	}

	return accel_module;
}

static inline struct spdk_accel_crypto_key *
_accel_crypto_key_get(const char *name)
{
	struct spdk_accel_crypto_key *key;

	assert(spdk_spin_held(&g_keyring_spin));

	TAILQ_FOREACH(key, &g_keyring, link) {
		if (strcmp(name, key->param.key_name) == 0) {
			return key;
		}
	}

	return NULL;
}

static void
accel_crypto_key_free_mem(struct spdk_accel_crypto_key *key)
{
	if (key->param.hex_key) {
		spdk_memset_s(key->param.hex_key, key->key_size * 2, 0, key->key_size * 2);
		free(key->param.hex_key);
	}
	if (key->param.hex_key2) {
		spdk_memset_s(key->param.hex_key2, key->key2_size * 2, 0, key->key2_size * 2);
		free(key->param.hex_key2);
	}
	free(key->param.tweak_mode);
	free(key->param.key_name);
	free(key->param.cipher);
	if (key->key) {
		spdk_memset_s(key->key, key->key_size, 0, key->key_size);
		free(key->key);
	}
	if (key->key2) {
		spdk_memset_s(key->key2, key->key2_size, 0, key->key2_size);
		free(key->key2);
	}
	free(key);
}

static void
accel_crypto_key_destroy_unsafe(struct spdk_accel_crypto_key *key)
{
	assert(key->module_if);
	assert(key->module_if->crypto_key_deinit);

	key->module_if->crypto_key_deinit(key);
	accel_crypto_key_free_mem(key);
}

/*
 * This function mitigates a timing side channel which could be caused by using strcmp()
 * Please refer to chapter "Mitigating Information Leakage Based on Variable Timing" in
 * the article [1] for more details
 * [1] https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/secure-coding/mitigate-timing-side-channel-crypto-implementation.html
 */
static bool
accel_aes_xts_keys_equal(const char *k1, size_t k1_len, const char *k2, size_t k2_len)
{
	size_t i;
	volatile size_t x = k1_len ^ k2_len;

	for (i = 0; ((i < k1_len) & (i < k2_len)); i++) {
		x |= k1[i] ^ k2[i];
	}

	return x == 0;
}

static const char *g_tweak_modes[] = {
	[SPDK_ACCEL_CRYPTO_TWEAK_MODE_SIMPLE_LBA] = "SIMPLE_LBA",
	[SPDK_ACCEL_CRYPTO_TWEAK_MODE_JOIN_NEG_LBA_WITH_LBA] = "JOIN_NEG_LBA_WITH_LBA",
	[SPDK_ACCEL_CRYPTO_TWEAK_MODE_INCR_512_FULL_LBA] = "INCR_512_FULL_LBA",
	[SPDK_ACCEL_CRYPTO_TWEAK_MODE_INCR_512_UPPER_LBA] = "INCR_512_UPPER_LBA",
};

static const char *g_ciphers[] = {
	[SPDK_ACCEL_CIPHER_AES_CBC] = "AES_CBC",
	[SPDK_ACCEL_CIPHER_AES_XTS] = "AES_XTS",
};

int
spdk_accel_crypto_key_create(const struct spdk_accel_crypto_key_create_param *param)
{
	struct spdk_accel_module_if *module;
	struct spdk_accel_crypto_key *key;
	size_t hex_key_size, hex_key2_size;
	bool found = false;
	size_t i;
	int rc;

	if (!param || !param->hex_key || !param->cipher || !param->key_name) {
		return -EINVAL;
	}

	if (g_modules_opc[SPDK_ACCEL_OPC_ENCRYPT].module != g_modules_opc[SPDK_ACCEL_OPC_DECRYPT].module) {
		/* hardly ever possible, but let's check and warn the user */
		SPDK_ERRLOG("Different accel modules are used for encryption and decryption\n");
	}
	module = g_modules_opc[SPDK_ACCEL_OPC_ENCRYPT].module;

	if (!module) {
		SPDK_ERRLOG("No accel module found assigned for crypto operation\n");
		return -ENOENT;
	}

	if (!module->crypto_key_init || !module->crypto_supports_cipher) {
		SPDK_ERRLOG("Module %s doesn't support crypto operations\n", module->name);
		return -ENOTSUP;
	}

	key = calloc(1, sizeof(*key));
	if (!key) {
		return -ENOMEM;
	}

	key->param.key_name = strdup(param->key_name);
	if (!key->param.key_name) {
		rc = -ENOMEM;
		goto error;
	}

	for (i = 0; i < SPDK_COUNTOF(g_ciphers); ++i) {
		assert(g_ciphers[i]);

		if (strncmp(param->cipher, g_ciphers[i], strlen(g_ciphers[i])) == 0) {
			key->cipher = i;
			found = true;
			break;
		}
	}

	if (!found) {
		SPDK_ERRLOG("Failed to parse cipher\n");
		rc = -EINVAL;
		goto error;
	}

	key->param.cipher = strdup(param->cipher);
	if (!key->param.cipher) {
		rc = -ENOMEM;
		goto error;
	}

	hex_key_size = strnlen(param->hex_key, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
	if (hex_key_size == SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH) {
		SPDK_ERRLOG("key1 size exceeds max %d\n", SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
		rc = -EINVAL;
		goto error;
	}

	if (hex_key_size == 0) {
		SPDK_ERRLOG("key1 size cannot be 0\n");
		rc = -EINVAL;
		goto error;
	}

	key->param.hex_key = strdup(param->hex_key);
	if (!key->param.hex_key) {
		rc = -ENOMEM;
		goto error;
	}

	key->key_size = hex_key_size / 2;
	key->key = spdk_unhexlify(key->param.hex_key);
	if (!key->key) {
		SPDK_ERRLOG("Failed to unhexlify key1\n");
		rc = -EINVAL;
		goto error;
	}

	if (param->hex_key2) {
		hex_key2_size = strnlen(param->hex_key2, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
		if (hex_key2_size == SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH) {
			SPDK_ERRLOG("key2 size exceeds max %d\n", SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
			rc = -EINVAL;
			goto error;
		}

		if (hex_key2_size == 0) {
			SPDK_ERRLOG("key2 size cannot be 0\n");
			rc = -EINVAL;
			goto error;
		}

		key->param.hex_key2 = strdup(param->hex_key2);
		if (!key->param.hex_key2) {
			rc = -ENOMEM;
			goto error;
		}

		key->key2_size = hex_key2_size / 2;
		key->key2 = spdk_unhexlify(key->param.hex_key2);
		if (!key->key2) {
			SPDK_ERRLOG("Failed to unhexlify key2\n");
			rc = -EINVAL;
			goto error;
		}
	}

	key->tweak_mode = ACCEL_CRYPTO_TWEAK_MODE_DEFAULT;
	if (param->tweak_mode) {
		found = false;

		key->param.tweak_mode = strdup(param->tweak_mode);
		if (!key->param.tweak_mode) {
			rc = -ENOMEM;
			goto error;
		}

		for (i = 0; i < SPDK_COUNTOF(g_tweak_modes); ++i) {
			assert(g_tweak_modes[i]);

			if (strncmp(param->tweak_mode, g_tweak_modes[i], strlen(g_tweak_modes[i])) == 0) {
				key->tweak_mode = i;
				found = true;
				break;
			}
		}

		if (!found) {
			SPDK_ERRLOG("Failed to parse tweak mode\n");
			rc = -EINVAL;
			goto error;
		}
	}

	if ((!module->crypto_supports_tweak_mode && key->tweak_mode != ACCEL_CRYPTO_TWEAK_MODE_DEFAULT) ||
	    (module->crypto_supports_tweak_mode && !module->crypto_supports_tweak_mode(key->tweak_mode))) {
		SPDK_ERRLOG("Module %s doesn't support %s tweak mode\n", module->name,
			    g_tweak_modes[key->tweak_mode]);
		rc = -EINVAL;
		goto error;
	}

	if (!module->crypto_supports_cipher(key->cipher, key->key_size)) {
		SPDK_ERRLOG("Module %s doesn't support %s cipher with %zu key size\n", module->name,
			    g_ciphers[key->cipher], key->key_size);
		rc = -EINVAL;
		goto error;
	}

	if (key->cipher == SPDK_ACCEL_CIPHER_AES_XTS) {
		if (!key->key2) {
			SPDK_ERRLOG("%s key2 is missing\n", g_ciphers[key->cipher]);
			rc = -EINVAL;
			goto error;
		}

		if (key->key_size != key->key2_size) {
			SPDK_ERRLOG("%s key size %zu is not equal to key2 size %zu\n", g_ciphers[key->cipher],
				    key->key_size,
				    key->key2_size);
			rc = -EINVAL;
			goto error;
		}

		if (accel_aes_xts_keys_equal(key->key, key->key_size, key->key2, key->key2_size)) {
			SPDK_ERRLOG("%s identical keys are not secure\n", g_ciphers[key->cipher]);
			rc = -EINVAL;
			goto error;
		}
	}

	if (key->cipher == SPDK_ACCEL_CIPHER_AES_CBC) {
		if (key->key2_size) {
			SPDK_ERRLOG("%s doesn't use key2\n", g_ciphers[key->cipher]);
			rc = -EINVAL;
			goto error;
		}
	}

	key->module_if = module;

	spdk_spin_lock(&g_keyring_spin);
	if (_accel_crypto_key_get(param->key_name)) {
		rc = -EEXIST;
	} else {
		rc = module->crypto_key_init(key);
		if (rc) {
			SPDK_ERRLOG("Module %s failed to initialize crypto key\n", module->name);
		} else {
			TAILQ_INSERT_TAIL(&g_keyring, key, link);
		}
	}
	spdk_spin_unlock(&g_keyring_spin);

	if (rc) {
		goto error;
	}

	return 0;

error:
	accel_crypto_key_free_mem(key);
	return rc;
}

int
spdk_accel_crypto_key_destroy(struct spdk_accel_crypto_key *key)
{
	if (!key || !key->module_if) {
		return -EINVAL;
	}

	spdk_spin_lock(&g_keyring_spin);
	if (!_accel_crypto_key_get(key->param.key_name)) {
		spdk_spin_unlock(&g_keyring_spin);
		return -ENOENT;
	}
	TAILQ_REMOVE(&g_keyring, key, link);
	spdk_spin_unlock(&g_keyring_spin);

	accel_crypto_key_destroy_unsafe(key);

	return 0;
}

struct spdk_accel_crypto_key *
spdk_accel_crypto_key_get(const char *name)
{
	struct spdk_accel_crypto_key *key;

	spdk_spin_lock(&g_keyring_spin);
	key = _accel_crypto_key_get(name);
	spdk_spin_unlock(&g_keyring_spin);

	return key;
}

/* Helper function when accel modules register with the framework. */
void
spdk_accel_module_list_add(struct spdk_accel_module_if *accel_module)
{
	struct spdk_accel_module_if *tmp;

	if (_module_find_by_name(accel_module->name)) {
		SPDK_NOTICELOG("Module %s already registered\n", accel_module->name);
		assert(false);
		return;
	}

	TAILQ_FOREACH(tmp, &spdk_accel_module_list, tailq) {
		if (accel_module->priority < tmp->priority) {
			break;
		}
	}

	if (tmp != NULL) {
		TAILQ_INSERT_BEFORE(tmp, accel_module, tailq);
	} else {
		TAILQ_INSERT_TAIL(&spdk_accel_module_list, accel_module, tailq);
	}
}

/* Framework level channel create callback. */
static int
accel_create_channel(void *io_device, void *ctx_buf)
{
	struct accel_io_channel	*accel_ch = ctx_buf;
	struct spdk_accel_task *accel_task;
	struct spdk_accel_task_aux_data *accel_task_aux;
	struct spdk_accel_sequence *seq;
	struct accel_buffer *buf;
	size_t task_size_aligned;
	uint8_t *task_mem;
	uint32_t i = 0, j;
	int rc;

	task_size_aligned = SPDK_ALIGN_CEIL(g_max_accel_module_size, SPDK_CACHE_LINE_SIZE);
	accel_ch->task_pool_base = aligned_alloc(SPDK_CACHE_LINE_SIZE,
				   g_opts.task_count * task_size_aligned);
	if (!accel_ch->task_pool_base) {
		return -ENOMEM;
	}
	memset(accel_ch->task_pool_base, 0, g_opts.task_count * task_size_aligned);

	accel_ch->seq_pool_base = aligned_alloc(SPDK_CACHE_LINE_SIZE,
						g_opts.sequence_count * sizeof(struct spdk_accel_sequence));
	if (accel_ch->seq_pool_base == NULL) {
		goto err;
	}
	memset(accel_ch->seq_pool_base, 0, g_opts.sequence_count * sizeof(struct spdk_accel_sequence));

	accel_ch->task_aux_data_base = calloc(g_opts.task_count, sizeof(struct spdk_accel_task_aux_data));
	if (accel_ch->task_aux_data_base == NULL) {
		goto err;
	}

	accel_ch->buf_pool_base = calloc(g_opts.buf_count, sizeof(struct accel_buffer));
	if (accel_ch->buf_pool_base == NULL) {
		goto err;
	}

	STAILQ_INIT(&accel_ch->task_pool);
	SLIST_INIT(&accel_ch->task_aux_data_pool);
	SLIST_INIT(&accel_ch->seq_pool);
	SLIST_INIT(&accel_ch->buf_pool);

	task_mem = accel_ch->task_pool_base;
	for (i = 0; i < g_opts.task_count; i++) {
		accel_task = (struct spdk_accel_task *)task_mem;
		accel_task->aux = NULL;
		STAILQ_INSERT_TAIL(&accel_ch->task_pool, accel_task, link);
		task_mem += task_size_aligned;
		accel_task_aux = &accel_ch->task_aux_data_base[i];
		SLIST_INSERT_HEAD(&accel_ch->task_aux_data_pool, accel_task_aux, link);
	}
	for (i = 0; i < g_opts.sequence_count; i++) {
		seq = &accel_ch->seq_pool_base[i];
		SLIST_INSERT_HEAD(&accel_ch->seq_pool, seq, link);
	}
	for (i = 0; i < g_opts.buf_count; i++) {
		buf = &accel_ch->buf_pool_base[i];
		SLIST_INSERT_HEAD(&accel_ch->buf_pool, buf, link);
	}

	/* Assign modules and get IO channels for each */
	for (i = 0; i < SPDK_ACCEL_OPC_LAST; i++) {
		accel_ch->module_ch[i] = g_modules_opc[i].module->get_io_channel();
		/* This can happen if idxd runs out of channels. */
		if (accel_ch->module_ch[i] == NULL) {
			SPDK_ERRLOG("Module %s failed to get io channel\n", g_modules_opc[i].module->name);
			goto err;
		}
	}

	if (g_accel_driver != NULL) {
		accel_ch->driver_channel = g_accel_driver->get_io_channel();
		if (accel_ch->driver_channel == NULL) {
			SPDK_ERRLOG("Failed to get driver's IO channel\n");
			goto err;
		}
	}

	rc = spdk_iobuf_channel_init(&accel_ch->iobuf, "accel", g_opts.small_cache_size,
				     g_opts.large_cache_size);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize iobuf accel channel\n");
		goto err;
	}

	return 0;
err:
	if (accel_ch->driver_channel != NULL) {
		spdk_put_io_channel(accel_ch->driver_channel);
	}
	for (j = 0; j < i; j++) {
		spdk_put_io_channel(accel_ch->module_ch[j]);
	}
	free(accel_ch->task_pool_base);
	free(accel_ch->task_aux_data_base);
	free(accel_ch->seq_pool_base);
	free(accel_ch->buf_pool_base);

	return -ENOMEM;
}

static void
accel_add_stats(struct accel_stats *total, struct accel_stats *stats)
{
	int i;

	total->sequence_executed += stats->sequence_executed;
	total->sequence_failed += stats->sequence_failed;
	total->sequence_outstanding += stats->sequence_outstanding;
	total->task_outstanding += stats->task_outstanding;
	total->retry.task += stats->retry.task;
	total->retry.sequence += stats->retry.sequence;
	total->retry.iobuf += stats->retry.iobuf;
	total->retry.bufdesc += stats->retry.bufdesc;
	for (i = 0; i < SPDK_ACCEL_OPC_LAST; ++i) {
		total->operations[i].executed += stats->operations[i].executed;
		total->operations[i].failed += stats->operations[i].failed;
		total->operations[i].num_bytes += stats->operations[i].num_bytes;
	}
}

/* Framework level channel destroy callback. */
static void
accel_destroy_channel(void *io_device, void *ctx_buf)
{
	struct accel_io_channel	*accel_ch = ctx_buf;
	int i;

	spdk_iobuf_channel_fini(&accel_ch->iobuf);

	if (accel_ch->driver_channel != NULL) {
		spdk_put_io_channel(accel_ch->driver_channel);
	}

	for (i = 0; i < SPDK_ACCEL_OPC_LAST; i++) {
		assert(accel_ch->module_ch[i] != NULL);
		spdk_put_io_channel(accel_ch->module_ch[i]);
		accel_ch->module_ch[i] = NULL;
	}

	/* Update global stats to make sure channel's stats aren't lost after a channel is gone */
	spdk_spin_lock(&g_stats_lock);
	accel_add_stats(&g_stats, &accel_ch->stats);
	spdk_spin_unlock(&g_stats_lock);

	free(accel_ch->task_pool_base);
	free(accel_ch->task_aux_data_base);
	free(accel_ch->seq_pool_base);
	free(accel_ch->buf_pool_base);
}

struct spdk_io_channel *
spdk_accel_get_io_channel(void)
{
	return spdk_get_io_channel(&spdk_accel_module_list);
}

static int
accel_module_initialize(void)
{
	struct spdk_accel_module_if *accel_module, *tmp_module;
	int rc = 0, module_rc;

	TAILQ_FOREACH_SAFE(accel_module, &spdk_accel_module_list, tailq, tmp_module) {
		module_rc = accel_module->module_init();
		if (module_rc) {
			TAILQ_REMOVE(&spdk_accel_module_list, accel_module, tailq);
			if (module_rc == -ENODEV) {
				SPDK_NOTICELOG("No devices for module %s, skipping\n", accel_module->name);
			} else if (!rc) {
				SPDK_ERRLOG("Module %s initialization failed with %d\n", accel_module->name, module_rc);
				rc = module_rc;
			}
			continue;
		}

		SPDK_DEBUGLOG(accel, "Module %s initialized.\n", accel_module->name);
	}

	return rc;
}

static void
accel_module_init_opcode(enum spdk_accel_opcode opcode)
{
	struct accel_module *module = &g_modules_opc[opcode];
	struct spdk_accel_module_if *module_if = module->module;

	if (module_if->get_memory_domains != NULL) {
		module->supports_memory_domains = module_if->get_memory_domains(NULL, 0) > 0;
	}
}

static int
accel_memory_domain_translate(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			      struct spdk_memory_domain *dst_domain, struct spdk_memory_domain_translation_ctx *dst_domain_ctx,
			      void *addr, size_t len, struct spdk_memory_domain_translation_result *result)
{
	struct accel_buffer *buf = src_domain_ctx;

	SPDK_DEBUGLOG(accel, "translate addr %p, len %zu\n", addr, len);

	assert(g_accel_domain == src_domain);
	assert(spdk_memory_domain_get_system_domain() == dst_domain);
	assert(buf->buf == NULL);
	assert(addr == ACCEL_BUFFER_BASE);
	assert(len == buf->len);

	buf->buf = spdk_iobuf_get(&buf->ch->iobuf, buf->len, NULL, NULL);
	if (spdk_unlikely(buf->buf == NULL)) {
		return -ENOMEM;
	}

	result->iov_count = 1;
	result->iov.iov_base = buf->buf;
	result->iov.iov_len = buf->len;
	SPDK_DEBUGLOG(accel, "translated addr %p\n", result->iov.iov_base);
	return 0;
}

static void
accel_memory_domain_invalidate(struct spdk_memory_domain *domain, void *domain_ctx,
			       struct iovec *iov, uint32_t iovcnt)
{
	struct accel_buffer *buf = domain_ctx;

	SPDK_DEBUGLOG(accel, "invalidate addr %p, len %zu\n", iov[0].iov_base, iov[0].iov_len);

	assert(g_accel_domain == domain);
	assert(iovcnt == 1);
	assert(buf->buf != NULL);
	assert(iov[0].iov_base == buf->buf);
	assert(iov[0].iov_len == buf->len);

	spdk_iobuf_put(&buf->ch->iobuf, buf->buf, buf->len);
	buf->buf = NULL;
}

int
spdk_accel_initialize(void)
{
	enum spdk_accel_opcode op;
	struct spdk_accel_module_if *accel_module = NULL;
	int rc;

	/*
	 * We need a unique identifier for the accel framework, so use the
	 * spdk_accel_module_list address for this purpose.
	 */
	spdk_io_device_register(&spdk_accel_module_list, accel_create_channel, accel_destroy_channel,
				sizeof(struct accel_io_channel), "accel");

	spdk_spin_init(&g_keyring_spin);
	spdk_spin_init(&g_stats_lock);

	rc = spdk_memory_domain_create(&g_accel_domain, SPDK_DMA_DEVICE_TYPE_ACCEL, NULL,
				       "SPDK_ACCEL_DMA_DEVICE");
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create accel memory domain\n");
		return rc;
	}

	spdk_memory_domain_set_translation(g_accel_domain, accel_memory_domain_translate);
	spdk_memory_domain_set_invalidate(g_accel_domain, accel_memory_domain_invalidate);

	g_modules_started = true;
	rc = accel_module_initialize();
	if (rc) {
		return rc;
	}

	if (g_accel_driver != NULL && g_accel_driver->init != NULL) {
		rc = g_accel_driver->init();
		if (rc != 0) {
			SPDK_ERRLOG("Failed to initialize driver %s: %s\n", g_accel_driver->name,
				    spdk_strerror(-rc));
			return rc;
		}
	}

	/* The module list is order by priority, with the highest priority modules being at the end
	 * of the list.  The software module should be somewhere at the beginning of the list,
	 * before all HW modules.
	 * NOTE: all opcodes must be supported by software in the event that no HW modules are
	 * initialized to support the operation.
	 */
	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		for (op = 0; op < SPDK_ACCEL_OPC_LAST; op++) {
			if (accel_module->supports_opcode(op)) {
				g_modules_opc[op].module = accel_module;
				SPDK_DEBUGLOG(accel, "OPC 0x%x now assigned to %s\n", op, accel_module->name);
			}
		}

		if (accel_module->get_ctx_size != NULL) {
			g_max_accel_module_size = spdk_max(g_max_accel_module_size,
							   accel_module->get_ctx_size());
		}
	}

	/* Now lets check for overrides and apply all that exist */
	for (op = 0; op < SPDK_ACCEL_OPC_LAST; op++) {
		if (g_modules_opc_override[op] != NULL) {
			accel_module = _module_find_by_name(g_modules_opc_override[op]);
			if (accel_module == NULL) {
				SPDK_ERRLOG("Invalid module name of %s\n", g_modules_opc_override[op]);
				return -EINVAL;
			}
			if (accel_module->supports_opcode(op) == false) {
				SPDK_ERRLOG("Module %s does not support op code %d\n", accel_module->name, op);
				return -EINVAL;
			}
			g_modules_opc[op].module = accel_module;
		}
	}

	if (g_modules_opc[SPDK_ACCEL_OPC_ENCRYPT].module != g_modules_opc[SPDK_ACCEL_OPC_DECRYPT].module) {
		SPDK_ERRLOG("Different accel modules are assigned to encrypt and decrypt operations");
		return -EINVAL;
	}
	if (g_modules_opc[SPDK_ACCEL_OPC_COMPRESS].module !=
	    g_modules_opc[SPDK_ACCEL_OPC_DECOMPRESS].module) {
		SPDK_ERRLOG("Different accel modules are assigned to compress and decompress operations");
		return -EINVAL;
	}

	for (op = 0; op < SPDK_ACCEL_OPC_LAST; op++) {
		assert(g_modules_opc[op].module != NULL);
		accel_module_init_opcode(op);
	}

	rc = spdk_iobuf_register_module("accel");
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register accel iobuf module\n");
		return rc;
	}

	return 0;
}

static void
accel_module_finish_cb(void)
{
	spdk_accel_fini_cb cb_fn = g_fini_cb_fn;

	cb_fn(g_fini_cb_arg);
	g_fini_cb_fn = NULL;
	g_fini_cb_arg = NULL;
}

static void
accel_write_overridden_opc(struct spdk_json_write_ctx *w, const char *opc_str,
			   const char *module_str)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "accel_assign_opc");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "opname", opc_str);
	spdk_json_write_named_string(w, "module", module_str);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static void
__accel_crypto_key_dump_param(struct spdk_json_write_ctx *w, struct spdk_accel_crypto_key *key)
{
	spdk_json_write_named_string(w, "name", key->param.key_name);
	spdk_json_write_named_string(w, "cipher", key->param.cipher);
	spdk_json_write_named_string(w, "key", key->param.hex_key);
	if (key->param.hex_key2) {
		spdk_json_write_named_string(w, "key2", key->param.hex_key2);
	}

	if (key->param.tweak_mode) {
		spdk_json_write_named_string(w, "tweak_mode", key->param.tweak_mode);
	}
}

void
_accel_crypto_key_dump_param(struct spdk_json_write_ctx *w, struct spdk_accel_crypto_key *key)
{
	spdk_json_write_object_begin(w);
	__accel_crypto_key_dump_param(w, key);
	spdk_json_write_object_end(w);
}

static void
_accel_crypto_key_write_config_json(struct spdk_json_write_ctx *w,
				    struct spdk_accel_crypto_key *key)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "accel_crypto_key_create");
	spdk_json_write_named_object_begin(w, "params");
	__accel_crypto_key_dump_param(w, key);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static void
accel_write_options(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "accel_set_options");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "small_cache_size", g_opts.small_cache_size);
	spdk_json_write_named_uint32(w, "large_cache_size", g_opts.large_cache_size);
	spdk_json_write_named_uint32(w, "task_count", g_opts.task_count);
	spdk_json_write_named_uint32(w, "sequence_count", g_opts.sequence_count);
	spdk_json_write_named_uint32(w, "buf_count", g_opts.buf_count);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static void
_accel_crypto_keys_write_config_json(struct spdk_json_write_ctx *w, bool full_dump)
{
	struct spdk_accel_crypto_key *key;

	spdk_spin_lock(&g_keyring_spin);
	TAILQ_FOREACH(key, &g_keyring, link) {
		if (full_dump) {
			_accel_crypto_key_write_config_json(w, key);
		} else {
			_accel_crypto_key_dump_param(w, key);
		}
	}
	spdk_spin_unlock(&g_keyring_spin);
}

void
_accel_crypto_keys_dump_param(struct spdk_json_write_ctx *w)
{
	_accel_crypto_keys_write_config_json(w, false);
}

void
spdk_accel_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_accel_module_if *accel_module;
	int i;

	spdk_json_write_array_begin(w);
	accel_write_options(w);

	if (g_accel_driver != NULL) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "accel_set_driver");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", g_accel_driver->name);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}

	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		if (accel_module->write_config_json) {
			accel_module->write_config_json(w);
		}
	}
	for (i = 0; i < SPDK_ACCEL_OPC_LAST; i++) {
		if (g_modules_opc_override[i]) {
			accel_write_overridden_opc(w, g_opcode_strings[i], g_modules_opc_override[i]);
		}
	}

	_accel_crypto_keys_write_config_json(w, true);

	spdk_json_write_array_end(w);
}

void
spdk_accel_module_finish(void)
{
	if (!g_accel_module) {
		g_accel_module = TAILQ_FIRST(&spdk_accel_module_list);
	} else {
		g_accel_module = TAILQ_NEXT(g_accel_module, tailq);
	}

	if (!g_accel_module) {
		if (g_accel_driver != NULL && g_accel_driver->fini != NULL) {
			g_accel_driver->fini();
		}

		spdk_spin_destroy(&g_keyring_spin);
		spdk_spin_destroy(&g_stats_lock);
		if (g_accel_domain) {
			spdk_memory_domain_destroy(g_accel_domain);
			g_accel_domain = NULL;
		}
		accel_module_finish_cb();
		return;
	}

	if (g_accel_module->module_fini) {
		spdk_thread_send_msg(spdk_get_thread(), g_accel_module->module_fini, NULL);
	} else {
		spdk_accel_module_finish();
	}
}

static void
accel_io_device_unregister_cb(void *io_device)
{
	struct spdk_accel_crypto_key *key, *key_tmp;
	enum spdk_accel_opcode op;

	spdk_spin_lock(&g_keyring_spin);
	TAILQ_FOREACH_SAFE(key, &g_keyring, link, key_tmp) {
		accel_crypto_key_destroy_unsafe(key);
	}
	spdk_spin_unlock(&g_keyring_spin);

	for (op = 0; op < SPDK_ACCEL_OPC_LAST; op++) {
		if (g_modules_opc_override[op] != NULL) {
			free(g_modules_opc_override[op]);
			g_modules_opc_override[op] = NULL;
		}
		g_modules_opc[op].module = NULL;
	}

	spdk_accel_module_finish();
}

void
spdk_accel_finish(spdk_accel_fini_cb cb_fn, void *cb_arg)
{
	assert(cb_fn != NULL);

	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;

	spdk_io_device_unregister(&spdk_accel_module_list, accel_io_device_unregister_cb);
}

static struct spdk_accel_driver *
accel_find_driver(const char *name)
{
	struct spdk_accel_driver *driver;

	TAILQ_FOREACH(driver, &g_accel_drivers, tailq) {
		if (strcmp(driver->name, name) == 0) {
			return driver;
		}
	}

	return NULL;
}

int
spdk_accel_set_driver(const char *name)
{
	struct spdk_accel_driver *driver = NULL;

	if (name != NULL && name[0] != '\0') {
		driver = accel_find_driver(name);
		if (driver == NULL) {
			SPDK_ERRLOG("Couldn't find driver named '%s'\n", name);
			return -ENODEV;
		}
	}

	g_accel_driver = driver;

	return 0;
}

const char *
spdk_accel_get_driver_name(void)
{
	if (!g_accel_driver) {
		return NULL;
	}

	return g_accel_driver->name;
}

void
spdk_accel_driver_register(struct spdk_accel_driver *driver)
{
	if (accel_find_driver(driver->name)) {
		SPDK_ERRLOG("Driver named '%s' has already been registered\n", driver->name);
		assert(0);
		return;
	}

	TAILQ_INSERT_TAIL(&g_accel_drivers, driver, tailq);
}

int
spdk_accel_set_opts(const struct spdk_accel_opts *opts)
{
	if (!opts) {
		SPDK_ERRLOG("opts cannot be NULL\n");
		return -1;
	}

	if (!opts->opts_size) {
		SPDK_ERRLOG("opts_size inside opts cannot be zero value\n");
		return -1;
	}

	if (SPDK_GET_FIELD(opts, task_count, g_opts.task_count,
			   opts->opts_size) < ACCEL_TASKS_IN_SEQUENCE_LIMIT) {
		return -EINVAL;
	}

#define SET_FIELD(field) \
        if (offsetof(struct spdk_accel_opts, field) + sizeof(opts->field) <= opts->opts_size) { \
                g_opts.field = opts->field; \
        } \

	SET_FIELD(small_cache_size);
	SET_FIELD(large_cache_size);
	SET_FIELD(task_count);
	SET_FIELD(sequence_count);
	SET_FIELD(buf_count);

	g_opts.opts_size = opts->opts_size;

#undef SET_FIELD

	return 0;
}

void
spdk_accel_get_opts(struct spdk_accel_opts *opts, size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return;
	}

	opts->opts_size = opts_size;

#define SET_FIELD(field) \
	if (offsetof(struct spdk_accel_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = g_opts.field; \
	} \

	SET_FIELD(small_cache_size);
	SET_FIELD(large_cache_size);
	SET_FIELD(task_count);
	SET_FIELD(sequence_count);
	SET_FIELD(buf_count);

#undef SET_FIELD

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_accel_opts) == 28, "Incorrect size");
}

struct accel_get_stats_ctx {
	struct accel_stats	stats;
	accel_get_stats_cb	cb_fn;
	void			*cb_arg;
};

static void
accel_get_channel_stats_done(struct spdk_io_channel_iter *iter, int status)
{
	struct accel_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(iter);

	ctx->cb_fn(&ctx->stats, ctx->cb_arg);
	free(ctx);
}

static void
accel_get_channel_stats(struct spdk_io_channel_iter *iter)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(iter);
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct accel_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(iter);

	accel_add_stats(&ctx->stats, &accel_ch->stats);
	spdk_for_each_channel_continue(iter, 0);
}

int
accel_get_stats(accel_get_stats_cb cb_fn, void *cb_arg)
{
	struct accel_get_stats_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	spdk_spin_lock(&g_stats_lock);
	accel_add_stats(&ctx->stats, &g_stats);
	spdk_spin_unlock(&g_stats_lock);

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(&spdk_accel_module_list, accel_get_channel_stats, ctx,
			      accel_get_channel_stats_done);

	return 0;
}

void
spdk_accel_get_opcode_stats(struct spdk_io_channel *ch, enum spdk_accel_opcode opcode,
			    struct spdk_accel_opcode_stats *stats, size_t size)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

#define FIELD_OK(field) \
	offsetof(struct spdk_accel_opcode_stats, field) + sizeof(stats->field) <= size

#define SET_FIELD(field, value) \
	if (FIELD_OK(field)) { \
		stats->field = value; \
	}

	SET_FIELD(executed, accel_ch->stats.operations[opcode].executed);
	SET_FIELD(failed, accel_ch->stats.operations[opcode].failed);
	SET_FIELD(num_bytes, accel_ch->stats.operations[opcode].num_bytes);

#undef FIELD_OK
#undef SET_FIELD
}

uint8_t
spdk_accel_get_buf_align(enum spdk_accel_opcode opcode,
			 const struct spdk_accel_operation_exec_ctx *ctx)
{
	struct spdk_accel_module_if *module = g_modules_opc[opcode].module;
	struct spdk_accel_opcode_info modinfo = {}, drvinfo = {};

	if (g_accel_driver != NULL && g_accel_driver->get_operation_info != NULL) {
		g_accel_driver->get_operation_info(opcode, ctx, &drvinfo);
	}

	if (module->get_operation_info != NULL) {
		module->get_operation_info(opcode, ctx, &modinfo);
	}

	/* If a driver is set, it'll execute most of the operations, while the rest will usually
	 * fall back to accel_sw, which doesn't have any alignment requirements.  However, to be
	 * extra safe, return the max(driver, module) if a driver delegates some operations to a
	 * hardware module. */
	return spdk_max(modinfo.required_alignment, drvinfo.required_alignment);
}

struct spdk_accel_module_if *
spdk_accel_get_module(const char *name)
{
	struct spdk_accel_module_if *module;

	TAILQ_FOREACH(module, &spdk_accel_module_list, tailq) {
		if (strcmp(module->name, name) == 0) {
			return module;
		}
	}

	return NULL;
}

int
spdk_accel_get_opc_memory_domains(enum spdk_accel_opcode opcode,
				  struct spdk_memory_domain **domains,
				  int array_size)
{
	assert(opcode < SPDK_ACCEL_OPC_LAST);

	if (g_modules_opc[opcode].module->get_memory_domains) {
		return g_modules_opc[opcode].module->get_memory_domains(domains, array_size);
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(accel)
