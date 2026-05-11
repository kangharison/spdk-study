/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK accel 라이브러리 내부 공용 헤더 (accel_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK accel(가속) 서브시스템의 내부 코드들끼리만 공유하는 자료구조와 헬퍼 함수
 * 프로토타입을 모아둔 헤더이다. 외부 사용자(예: bdev 모듈, 사용자 앱)에게
 * 노출되는 공개 API(`include/spdk/accel.h`, `include/spdk/accel_module.h`)와는
 * 별개로, accel.c / accel_sw.c / accel_rpc.c 같은 라이브러리 내부 파일들이
 * 통계 구조체(`accel_stats`), 모듈 열거 콜백(`_accel_for_each_module`),
 * 크립토 키 덤프 헬퍼(`_accel_crypto_key_dump_param`), 통계 조회
 * (`accel_get_stats`) 등 비공개 인터페이스를 공유하기 위해 사용한다.
 * 매크로 `ACCEL_AES_XTS`는 RPC 출력/검증에 쓰이는 표준 암호 식별자이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 가속 추상화 레이어(accel framework)는 다음 계층으로 구성된다:
 *   [상위 사용자 — bdev 모듈, NVMe-oF 등]
 *     ↓ spdk_accel_submit_*() / spdk_accel_append_*()
 *   [accel 코어 (lib/accel/accel.c)]      ← 본 헤더의 주된 사용자
 *     ↓ submit_tasks 콜백
 *   [accel 모듈 (sw / idxd / dsa / ioat / ae4dma / mlx5 …)]
 *     ↓ 하드웨어 또는 SW 알고리즘
 *   [HW 가속기 또는 CPU SIMD 라이브러리 (ISA-L, lz4 등)]
 * 이 헤더는 위 그림에서 accel 코어와 accel_sw, RPC 핸들러가 서로 공유하는
 * 통계/모듈정보 구조의 정의서 역할을 한다. 호스트 유저스페이스(SPDK reactor
 * 스레드)에서 동작하며, 커널 컴포넌트에는 노출되지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존하는 모듈: `spdk/accel.h`(opcode/암호 enum), `spdk/queue.h`(TAILQ 매크로),
 *   `spdk/config.h`(빌드 옵션), `spdk/stdinc.h`(공통 표준 인클루드).
 * - 이 헤더에 의존하는 파일: `lib/accel/accel.c`(본 헤더의 함수 구현 제공),
 *   `lib/accel/accel_sw.c`(SW fallback 모듈), `lib/accel/accel_rpc.c`(JSON-RPC 핸들러).
 * - 데이터 흐름: accel 코어가 각 채널(`accel_io_channel`)에서 운영하는 통계를
 *   `struct accel_stats`로 누적 → RPC 핸들러가 `accel_get_stats()`를 통해 조회 →
 *   JSON 응답으로 클라이언트에 반환.
 * - 공유 자료구조: `accel_stats`는 채널별 통계를 모두 더해 글로벌 통계를
 *   만드는 데 쓰이며, `module_info`는 모든 등록된 accel 모듈을 순회하며
 *   각 모듈이 지원하는 opcode 목록을 RPC로 덤프할 때 사용된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct module_info: 모듈 열거 시 한 모듈의 이름과 지원 opcode 목록을 담는
 *   왕복(in/out) 컨테이너. `_accel_for_each_module`의 콜백에 전달된다.
 * - struct accel_operation_stats: opcode 단위 실행 통계(성공/실패/바이트수).
 * - struct accel_stats: 전체 가속 통계 (opcode별 통계 + 시퀀스/태스크 카운터 +
 *   리트라이 카운터). `accel_get_stats()`로 RPC에 노출된다.
 * - _accel_for_each_module(info, fn): 등록된 모든 accel 모듈을 순회하며
 *   `module_info`를 채워 `fn`을 호출한다.
 * - _accel_crypto_key_dump_param / _accel_crypto_keys_dump_param: 암호 키 또는
 *   keyring 전체를 JSON으로 덤프한다 (RPC `accel_crypto_keys_get`에서 사용).
 * - accel_get_stats(cb_fn, cb_arg): 모든 채널의 통계를 비동기로 수집해
 *   콜백으로 전달.
 */

#ifndef SPDK_INTERNAL_ACCEL_INTERNAL_H
#define SPDK_INTERNAL_ACCEL_INTERNAL_H

/* [한국어] SPDK 표준 인클루드 — size_t/uint*_t/stdbool 등 표준 헤더 모음.
 *  SPDK 환경(포팅 가능한 OS 추상)을 사용하기 위한 진입점. */
#include "spdk/stdinc.h"

/* [한국어] accel 공개 API. enum spdk_accel_opcode와 SPDK_ACCEL_OPC_LAST 등을
 *  여기서 가져와 통계 배열의 크기로 사용한다. */
#include "spdk/accel.h"
/* [한국어] TAILQ/STAILQ/SLIST 매크로 정의. 본 헤더에서는 직접 쓰지 않으나
 *  포함 파일들과 일관성 유지를 위해 함께 인클루드된다. */
#include "spdk/queue.h"
/* [한국어] 빌드 시 결정된 SPDK_CONFIG_* 매크로(예: ISAL/LZ4 활성 여부).
 *  내부 헬퍼들이 조건부로 컴파일될 수 있도록 사용된다. */
#include "spdk/config.h"

/* [한국어] AES-XTS 암호 알고리즘의 표준 문자열 식별자.
 *  RPC `accel_crypto_key_create`/`accel_crypto_keys_get` 응답이나
 *  설정 입력에서 cipher 문자열을 비교/표기할 때 단일 진실 원천으로 사용. */
#define ACCEL_AES_XTS "AES_XTS"

/*
 * [한국어] struct module_info — accel 모듈 열거 시 한 모듈의 메타정보를
 *  담는 왕복 컨테이너. `_accel_for_each_module()`이 모든 등록 모듈을 순회하며
 *  이 구조를 채워 사용자 콜백에 전달한다(예: RPC `accel_get_module_info`).
 */
struct module_info {
	struct spdk_json_write_ctx *w;
	/* [한국어] JSON 출력 컨텍스트(쓰기 핸들).
	 * 설정자: 호출자(주로 RPC 핸들러)가 `spdk_jsonrpc_begin_result` 등을 통해
	 *   생성한 JSON writer를 채워 넘긴다.
	 * 읽는 자: 콜백 `fn(info)` 내부에서 모듈 이름/opcode 목록을 직렬화할 때.
	 * 값 범위: 유효한 spdk_json_write_ctx 포인터 (NULL 가능 — 비-RPC 호출자가
	 *   JSON 출력을 원치 않을 수 있음).
	 * 동기화: RPC 처리는 단일 스레드(보통 메인 reactor)에서 수행되므로 별도 락 불필요. */

	const char *name;
	/* [한국어] 현재 열거되고 있는 accel 모듈의 이름 (예: "software", "idxd").
	 * 설정자: `_accel_for_each_module()`이 각 모듈을 순회하며 모듈의
	 *   `accel_module->name` 필드를 그대로 복사해 둔다.
	 * 읽는 자: 사용자 콜백 `fn`이 JSON에 모듈명을 출력할 때.
	 * 값 범위: 모듈이 등록 시 정의한 정적 문자열에 대한 포인터(불변).
	 * 동기화: 모듈 등록은 초기화 시 한 번만 일어나므로 읽기 전용으로 안전. */

	enum spdk_accel_opcode ops[SPDK_ACCEL_OPC_LAST];
	/* [한국어] 이 모듈이 지원한다고 보고한 opcode 목록(앞에서부터 num_ops 개 유효).
	 * 설정자: `_accel_for_each_module()`이 `accel_module->supports_opcode()`를
	 *   모든 SPDK_ACCEL_OPC_*에 대해 호출해 true인 것만 차곡차곡 적재.
	 * 읽는 자: 콜백 `fn`이 지원 op 배열을 JSON에 출력할 때.
	 * 값 범위: 0 이상 SPDK_ACCEL_OPC_LAST 미만의 enum 값들.
	 * 동기화: 호출 1회당 1회 채워지므로 별도 동기화 불필요. */

	uint32_t num_ops;
	/* [한국어] `ops` 배열에 채워진 유효 항목 수.
	 * 설정자: 위와 동일. 모든 opcode 열거 후 누적된 값.
	 * 읽는 자: 콜백 `fn`이 ops를 0..num_ops-1 범위로 순회할 때.
	 * 값 범위: 0 ~ SPDK_ACCEL_OPC_LAST.
	 * 동기화: 동일 호출 컨텍스트 내부 로컬 변수처럼 동작 — 락 불필요. */
};

/*
 * [한국어] struct accel_operation_stats — 단일 opcode(예: COPY, FILL)의
 *  실행 통계. `struct accel_stats::operations[]`의 한 원소로 들어간다.
 */
struct accel_operation_stats {
	uint64_t executed;
	/* [한국어] 해당 opcode가 성공적으로 실행 완료된 누적 횟수.
	 * 설정자: `spdk_accel_task_complete()` 경로에서 status==0일 때 증가.
	 * 읽는 자: RPC `accel_get_stats` 응답 생성, 진단/로그.
	 * 값 범위: 0 이상 64비트 카운터. 오버플로는 사실상 발생 안 함.
	 * 동기화: per-channel 통계라 단일 스레드에서만 갱신, 글로벌 합산은
	 *   `accel_get_stats`가 spdk_for_each_channel을 통해 직렬 수집. */

	uint64_t failed;
	/* [한국어] 해당 opcode가 비-0 status로 실패한 누적 횟수.
	 * 설정자: 동일하게 task_complete 경로에서 status!=0일 때 증가.
	 * 읽는 자: 동일.
	 * 값 범위: 0 이상.
	 * 동기화: 위와 동일 — per-channel. */

	uint64_t num_bytes;
	/* [한국어] 처리된 바이트 누계. (예: copy 길이, crc32c 입력 길이 등)
	 * 설정자: opcode별 submit 시 누적되거나 완료 시 누적.
	 * 읽는 자: 동일.
	 * 값 범위: 0 이상.
	 * 동기화: 위와 동일. */
};

/*
 * [한국어] struct accel_stats — accel 서브시스템 전체 통계의 컨테이너.
 *  채널별로 동일한 모양의 통계를 두고, 글로벌 합산이나 RPC 출력 시 동일 타입을
 *  재사용한다. 각 필드는 32/64비트 카운터.
 */
struct accel_stats {
	struct accel_operation_stats	operations[SPDK_ACCEL_OPC_LAST];
	/* [한국어] opcode별 실행/실패/바이트 통계를 모아둔 배열.
	 * 설정자: 채널 통계는 task_complete에서 갱신, 글로벌 통계는
	 *   `accel_add_stats()`가 모든 채널 통계를 더해 만든다.
	 * 읽는 자: RPC `accel_get_stats` 응답, `spdk_accel_get_opcode_stats`.
	 * 값 범위: SPDK_ACCEL_OPC_LAST(현재 17개) 길이의 배열.
	 * 동기화: 채널별 통계는 reactor 단일 스레드, 글로벌 합산은
	 *   `spdk_for_each_channel`이 직렬 호출 보장. */

	uint64_t			sequence_executed;
	/* [한국어] 시퀀스(체이닝된 accel 작업 묶음)가 정상 완료된 누적 횟수.
	 * 설정자: `accel_sequence_complete()`가 cb_fn 호출 직전 +1.
	 * 읽는 자: RPC stats.
	 * 값 범위: 0 이상.
	 * 동기화: per-channel 갱신. */

	uint64_t			sequence_failed;
	/* [한국어] 시퀀스가 비-0 상태로 종료된 누적 횟수(전체 시퀀스 중 하나라도 실패).
	 * 설정자: `accel_sequence_complete_tasks()`가 에러 경로에서 +1.
	 * 읽는 자/값 범위/동기화: 위와 동일. */

	uint32_t			sequence_outstanding;
	/* [한국어] 현재 처리 중인(완료되지 않은) 시퀀스 수 — 게이지(즉시값).
	 * 설정자: `accel_sequence_get()`이 +1, 완료 시 -1.
	 * 읽는 자: RPC stats. 값 범위: 0 ~ 워크로드 의존.
	 * 동기화: per-channel. */

	uint32_t			task_outstanding;
	/* [한국어] 현재 처리 중인(아직 task_complete되지 않은) 단일 task 수.
	 * 설정자: `_get_task()`가 +1, `_put_task()`가 -1.
	 * 읽는 자: RPC stats. 값 범위/동기화: 위와 동일. */

	struct {
		uint64_t task;
		/* [한국어] task 풀이 비어 `_get_task()`가 NULL을 반환한 회수 — 백프레셔 지표.
		 * 설정자: `_get_task()` 실패 경로. 읽는 자: stats. 동기화: per-channel. */

		uint64_t sequence;
		/* [한국어] 시퀀스 풀(`seq_pool`)이 비어 신규 시퀀스 할당 실패한 회수.
		 * 설정자: `accel_sequence_get()` 실패 경로. 동일 동기화. */

		uint64_t iobuf;
		/* [한국어] iobuf 풀에서 빌릴 버퍼가 없어 대기 큐로 들어간 회수
		 *  (sequence가 virt/bounce buffer를 못 받아 backlog에 enqueue된 횟수).
		 * 설정자: `accel_sequence_alloc_buf()` / bounce 콜백 경로. */

		uint64_t bufdesc;
		/* [한국어] accel_buffer 디스크립터 풀(`buf_pool`)이 비어 새 디스크립터를
		 *  발급 못 한 횟수. 설정자: `accel_get_buf()` 실패 경로. */
	} retry;
	/* [한국어] 자원 부족으로 인한 리트라이/대기 카운터들.
	 * 모두 per-channel로 갱신되며 글로벌 합산은 accel_add_stats가 처리. */
};

/* [한국어] _accel_for_each_module의 콜백 타입.
 *  사용자 측에서 `module_info`를 받아 JSON 직렬화 등 처리를 수행한다. */
typedef void (*_accel_for_each_module_fn)(struct module_info *info);
/*
 * [한국어]
 * _accel_for_each_module - 등록된 모든 accel 모듈을 순회하며 콜백 호출.
 *
 * @info: 호출자가 미리 준비한 module_info(특히 w 필드 등)를 전달.
 *        함수 내부에서 모듈별로 name/ops/num_ops를 채워 fn에 넘긴다.
 * @fn: 모듈별로 호출될 사용자 콜백. info의 내용을 읽어 JSON에 출력하는 등 사용.
 *
 * 사용 컨텍스트: RPC 핸들러(`rpc_accel_get_module_info`)에서 호출.
 * 호출 체인: rpc_accel_get_module_info → [_accel_for_each_module] → fn(info)
 */
void _accel_for_each_module(struct module_info *info, _accel_for_each_module_fn fn);
/*
 * [한국어]
 * _accel_crypto_key_dump_param - 단일 암호 키의 파라미터를 JSON으로 덤프.
 *
 * @w: JSON writer 컨텍스트.
 * @key: 덤프 대상 키. spdk_accel_crypto_key는 cipher/key_name/tweak_mode 등을 가진다.
 *
 * RPC `accel_crypto_keys_get`에서 특정 키 이름을 지정했을 때 호출된다.
 * 키 자체(원시 비트열)는 절대 출력하지 않으며 메타데이터만 덤프한다.
 */
void _accel_crypto_key_dump_param(struct spdk_json_write_ctx *w, struct spdk_accel_crypto_key *key);
/*
 * [한국어]
 * _accel_crypto_keys_dump_param - keyring의 모든 암호 키 메타를 덤프.
 *
 * @w: JSON writer.
 *
 * RPC `accel_crypto_keys_get`에 키 이름이 미지정일 때 모든 키를 순회 덤프.
 * 글로벌 keyring(`g_keyring`)을 spinlock으로 보호하며 순회한다.
 */
void _accel_crypto_keys_dump_param(struct spdk_json_write_ctx *w);
/* [한국어] accel_get_stats 콜백 타입 — 모든 채널 통계 합산이 끝나면 호출됨.
 *  stats: 합산된 누적 통계(스택/heap 포인터 — 콜백 종료 후 무효).
 *  cb_arg: 호출자가 넘긴 컨텍스트(주로 RPC 요청 객체). */
typedef void (*accel_get_stats_cb)(struct accel_stats *stats, void *cb_arg);
/*
 * [한국어]
 * accel_get_stats - 비동기 통계 수집 진입점. 모든 reactor의 채널 통계를
 *                  spdk_for_each_channel로 합산해 cb_fn으로 결과를 전달.
 *
 * @cb_fn: 합산 완료 시 호출될 콜백 (NULL 불가).
 * @cb_arg: 콜백에 그대로 넘겨질 사용자 컨텍스트.
 * @return: 0 성공 / 음수 errno (예: -ENOMEM).
 *
 * 호출자: RPC `accel_get_stats` 핸들러. 콜백은 모든 채널을 다 돈 후 main 스레드에서 실행.
 */
int accel_get_stats(accel_get_stats_cb cb_fn, void *cb_arg);

#endif
