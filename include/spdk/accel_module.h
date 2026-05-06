/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK Acceleration Framework 모듈 작성자용 API 헤더 (accel_module.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK accel framework (lib/accel) 의 백엔드 모듈을 작성하는
 * 코드(예: module/accel/dsa, iaa, idxd, ioat, mlx5, sw 등) 가 사용해야 하는
 * 인터페이스를 선언한다. accel framework 는 SPDK 가 제공하는 통합 가속
 * 추상화 계층으로, copy/fill/crc32c/compress/decompress/encrypt/decrypt/
 * dif_check 등 데이터 변환 op 들을 (1) Intel DSA/IAA/IOAT, (2) Mellanox/
 * NVIDIA DPU 의 mlx5 가속기, (3) ISA-L 기반 SW fallback 같은 다양한 백엔드
 * 에 위임한다. 본 헤더는 그 백엔드(=module) 가 구현해야 하는 vtable
 * (struct spdk_accel_module_if), 단일 task 표현(struct spdk_accel_task),
 * crypto key 디스크립터, sequence 실행을 위한 driver vtable, 그리고 자동
 * 등록 매크로 SPDK_ACCEL_MODULE_REGISTER / SPDK_ACCEL_DRIVER_REGISTER 를 묶는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * accel framework 는 [bdev_crypto / bdev_compress / 사용자 RPC] -> [accel
 * 코어(lib/accel)] -> [모듈 (module/accel/*)] -> [실 하드웨어/SW 라이브러리]
 * 의 흐름에서 중간 분배자 역할을 한다. 코어는 op_code 를 보고 (1) 명시적
 * spdk_accel_assign_opc 매핑 (2) 모듈 priority (높은 값 우선, SW 모듈은
 * SPDK_ACCEL_SW_PRIORITY = -1) (3) supports_opcode 콜백을 종합해 어떤
 * 모듈에 task 를 보낼지 결정한다. 또한 accel sequence (여러 op 의 chained
 * 실행) 가 들어오면 driver (struct spdk_accel_driver) 가 등록되어 있을 때
 * 그쪽으로 통째 위임할 수 있다 — DPU 같은 device 가 sequence 를 한 번에
 * 처리하는 시나리오에 적합하다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: spdk/stdinc.h, spdk/accel.h (사용자 측 enum/typedef — opcode/cipher/
 * comp_algo/completion_cb 등), spdk/queue.h (TAILQ/STAILQ/SLIST 매크로),
 * spdk/config.h (빌드 옵션 매크로). 사용처: lib/accel (코어가 등록된
 * 모듈/드라이버 vtable 을 호출), module/accel/* (모든 백엔드 구현체),
 * lib/bdev/crypto, lib/bdev/compress, lib/blob (DEK 인코딩/디코딩에 accel
 * 사용). 데이터 흐름은 사용자 -> spdk_accel_submit_* -> task 객체 생성
 * -> 모듈 결정 -> module->submit_tasks(io_ch, task) -> 백엔드 실행
 * -> spdk_accel_task_complete -> 사용자 cb_fn(cb_arg, status).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_accel_module_if: 백엔드 모듈 vtable. name/priority/init/fini/
 *   get_ctx_size/supports_opcode/get_io_channel/submit_tasks/crypto_key_init/
 *   /crypto_supports_*/compress_*/get_memory_domains/get_operation_info/tailq.
 * - struct spdk_accel_task: 단일 op 컨텍스트. op_code, src/dst iovs/도메인,
 *   nbytes, op-specific 유니언 (seed/fill_pattern/crypto_key/dif/comp), cb_fn/cb_arg.
 * - struct spdk_accel_crypto_key: 모듈에 종속된 AES-XTS 같은 키 디스크립터.
 * - struct spdk_accel_bounce_buffer: 모듈이 memory_domain 을 지원하지 않을
 *   때 코어가 임시 push/pull 용도로 사용하는 보조 버퍼.
 * - struct spdk_accel_task_aux_data: bounce + 보조 iov array.
 * - struct spdk_accel_driver: 여러 op 으로 구성된 sequence 를 한 번에 실행
 *   할 수 있는 platform driver vtable.
 * - SPDK_ACCEL_MODULE_REGISTER / SPDK_ACCEL_DRIVER_REGISTER: __constructor__
 *   기반 자동 등록 매크로.
 * - spdk_accel_task_complete / spdk_accel_sequence_continue: 모듈/드라이버가
 *   완료를 코어에 보고하는 진입점.
 * - spdk_accel_alloc_sequence_buf: sequence 안에서 임시 버퍼 할당.
 * - spdk_accel_get_module: 이름으로 등록된 모듈 룩업.
 */

/* [한국어] SPDK_ACCEL_MODULE_H — 헤더 가드. accel 코어와 모든 module/accel/*
 * 에서 포함되므로 가드는 필수. */
#ifndef SPDK_ACCEL_MODULE_H
#define SPDK_ACCEL_MODULE_H

/* [한국어] spdk/stdinc.h - 표준 정수형/bool/size_t 묶음 헤더. uint8_t/
 * uint16_t/uint32_t/uint64_t 등 task 구조체에서 사용하는 타입을 위해 필수. */
#include "spdk/stdinc.h"

/* [한국어] spdk/accel.h - 사용자 측 accel API 헤더. enum spdk_accel_opcode,
 * enum spdk_accel_cipher, enum spdk_accel_comp_algo, spdk_accel_completion_cb
 * 등 모듈/드라이버 인터페이스에 등장하는 모든 typedef/enum 이 여기 정의되어
 * 있어 반드시 포함. */
#include "spdk/accel.h"
/* [한국어] spdk/queue.h - TAILQ/STAILQ/SLIST 매크로. spdk_accel_task 가
 * 두 개의 list 링크(seq_link, link), aux_data 가 SLIST 링크, module/driver
 * 가 TAILQ 링크를 사용하므로 필수. */
#include "spdk/queue.h"
/* [한국어] spdk/config.h - SPDK 빌드 시 ./configure 가 정의한 매크로 묶음.
 * 일부 기능 분기(예: crypto/comp)가 이 헤더의 SPDK_CONFIG_* 매크로에 종속
 * 될 수 있어 포함된다. */
#include "spdk/config.h"

/* [한국어] C++ 컴파일러용 extern "C" 가드 시작. */
#ifdef __cplusplus
extern "C" {
#endif

/* [한국어] struct spdk_accel_module_if - 모듈 vtable 의 forward declaration.
 * spdk_accel_crypto_key 가 module_if 포인터를 멤버로 가지므로 미리 선언. */
struct spdk_accel_module_if;
/* [한국어] struct spdk_accel_task - task 구조체의 forward declaration.
 * spdk_accel_task_complete 가 인자로 받기 위해 먼저 선언이 필요하다. */
struct spdk_accel_task;

/*
 * [한국어]
 * spdk_accel_task_complete - 모듈이 task 실행을 완료하고 코어에 결과를 보고.
 *
 * @task: 모듈이 받았던 task 포인터.
 * @status: 0 성공 / 음수 errno (실패).
 * @return: void.
 *
 * 동기/배경: 모듈->submit_tasks 가 task 를 받은 뒤 비동기로 실행하고,
 * 완료 시점에 반드시 이 함수를 통해 코어에 결과를 알려야 한다. 코어는
 * 이를 받아 (1) sequence 안의 다음 task 진행 또는 (2) 사용자 cb_fn 호출 +
 * task 메모리 free 를 처리한다. 이 함수가 누락되면 사용자 cb 가 영원히
 * 호출되지 않아 hang 에 빠진다.
 *
 * 실행 컨텍스트: 모듈의 polling thread (io_channel 이 발급된 SPDK thread).
 * task 가 발급된 thread 와 동일해야 한다 — cross-thread 완료는 lockless
 * 보장이 깨지므로 금지.
 *
 * 호출 체인:
 *   module submit_tasks → 백엔드 실행 → 완료 폴링 → [spdk_accel_task_complete]
 *   → 코어 → (sequence) 다음 step 또는 사용자 cb_fn(cb_arg, status)
 */
void spdk_accel_task_complete(struct spdk_accel_task *task, int status);

/* [한국어] SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH = 256+1 — 사용자가 RPC 등으로
 * 입력하는 hex 인코딩된 raw key 문자열의 strnlen() 상한.
 * 이유: AES-XTS 의 최대 키 크기가 두 키(key1+key2)*32B = 64B = 128 hex char
 * 인 정도지만, 향후 더 긴 키를 받을 가능성과 NUL terminator 를 위해
 * 보수적으로 256+1 을 잡는다. RPC 파서가 이 값을 strnlen 의 maxlen 으로
 * 사용해 무한 길이 입력에 의한 DoS 를 방지. */
/** Some reasonable key length used with strnlen() */
#define SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH (256 + 1)

/* [한국어] enum spdk_accel_crypto_tweak_mode - AES-XTS 의 tweak (=IV) 가
 * LBA 로부터 어떻게 유도되는지를 나타내는 모드. AES-XTS 는 디스크 암호화
 * 표준에서 tweak 으로 sector 번호를 사용하는데, 어떤 비트 배치를 쓰느냐가
 * 사용자/HW 에 따라 다르므로 명시적으로 enum 으로 분기한다. blob 의
 * crypto, NVMe 의 inline encryption 등이 같은 디스크의 같은 sector 를
 * 일관되게 해석하려면 이 모드가 정확히 일치해야 한다. */
enum spdk_accel_crypto_tweak_mode {
	/* Tweak[127:0] = {64'b0, LBA[63:0]} */
	SPDK_ACCEL_CRYPTO_TWEAK_MODE_SIMPLE_LBA,
	/* [한국어] SIMPLE_LBA — 가장 단순한 매핑. Tweak 의 하위 64 비트에
	 * LBA 를 그대로 넣고 상위 64 비트는 0. Linux dm-crypt 등이 사용.
	 * 설정자: 사용자 RPC 또는 모듈 default. 읽는 자: AES-XTS 엔진.
	 * 동기화: 키 객체 단위 read-only. */

	/* Tweak[127:0] = {1'b0, ~LBA[62:0], LBA[63:0]} */
	SPDK_ACCEL_CRYPTO_TWEAK_MODE_JOIN_NEG_LBA_WITH_LBA,
	/* [한국어] JOIN_NEG_LBA_WITH_LBA — 일부 Intel 플랫폼이 사용하는 변형.
	 * 상위 비트에 ~LBA, 하위 비트에 LBA 를 동시에 인코딩.
	 * 설정자: 동일. 읽는 자: 동일. 동기화: 동일. */

	/* Tweak is derived from LBA that is internally incremented by 1 for every 512 bytes processed
	 * so initial lba = (BLOCK_SIZE_IN_BYTES / 512) * LBA
	 * Tweak[127:0] = {lba[127:0]} */
	SPDK_ACCEL_CRYPTO_TWEAK_MODE_INCR_512_FULL_LBA,
	/* [한국어] INCR_512_FULL_LBA — 512B 단위로 1 씩 증가하는 가상 LBA 를
	 * 사용하는 모드 (NVMe 의 일부 inline encryption 시나리오와 호환).
	 * 초기 lba = (block_size / 512) * LBA 로 환산되어 처리.
	 * 설정자/읽는 자/동기화: 동일. */

	/* Tweak is derived from LBA that is internally incremented by 1 for every 512 bytes processed
	 * so initial lba = (BLOCK_SIZE_IN_BYTES / 512) * LBA
	 * Tweak[127:0] = {lba[63:0], 64'b0} */
	SPDK_ACCEL_CRYPTO_TWEAK_MODE_INCR_512_UPPER_LBA,
	/* [한국어] INCR_512_UPPER_LBA — INCR_512 와 같은 가상 LBA 증가를 쓰되,
	 * Tweak 의 상위 64 비트에 LBA 를 두는 변형.
	 * 설정자/읽는 자/동기화: 동일. */
};

/* [한국어] struct spdk_accel_crypto_key - 모듈에 종속된 crypto 키 디스크립터.
 * 사용자가 spdk_accel_crypto_key_create 를 호출하면 코어가 이 구조체를 만들고,
 * 모듈->crypto_key_init 을 호출해 백엔드 전용 자료(priv) 를 채우게 한다.
 * 이후 task 가 이 키 핸들을 참조하여 암복호화에 사용한다. */
struct spdk_accel_crypto_key {
	void *priv;
	/* [한국어] priv — 백엔드 모듈 전용 컨텍스트 포인터.
	 * 설정자: module->crypto_key_init 안에서 모듈이 자기 자료를 alloc 해 채움
	 *         (예: HW 키 슬롯 ID, OpenSSL EVP_CIPHER_CTX 등).
	 * 읽는 자: module->submit_tasks (암복호화 op) 에서만.
	 * 값 범위: 모듈에 따라 다름. 코어는 들여다보지 않음.
	 * 동기화: 키는 init 후 read-mostly — 모듈이 자체 동기화 책임. */

	char *key;
	/* [한국어] key — 1차 키의 raw binary (또는 hex 디코딩 후 데이터).
	 * 설정자: 사용자 입력으로부터 코어가 할당·복사. 모듈은 init 시점에 읽기.
	 * 읽는 자: module->crypto_key_init.
	 * 값 범위: cipher/key_size 조합에 맞는 길이.
	 * 동기화: 메모리 할당 전후 외엔 변하지 않음. zeroize 후 free 됨. */
	size_t key_size;
	/* [한국어] key_size — key 버퍼의 바이트 길이.
	 * 설정자/읽는 자/동기화: key 와 동일. */

	char *key2;
	/* [한국어] key2 — AES-XTS 같은 dual-key cipher 에서의 2 차 키.
	 * 설정자: cipher 가 dual-key 형식이면 사용자 입력으로부터 복사.
	 *         single-key cipher (AES-CBC 등) 면 NULL.
	 * 읽는 자: module->crypto_key_init.
	 * 값 범위: NULL 또는 key2_size 만큼의 raw bytes.
	 * 동기화: key 와 동일. */
	size_t key2_size;
	/* [한국어] key2_size — key2 버퍼 바이트 길이 (NULL 이면 0).
	 * 설정자/읽는 자/동기화: key2 와 동일. */

	enum spdk_accel_cipher cipher;
	/* [한국어] cipher — AES-XTS / AES-CBC 등 사용할 cipher 종류.
	 * 설정자: 사용자가 RPC 인자로 지정.
	 * 읽는 자: module->crypto_supports_cipher / crypto_key_init / submit_tasks.
	 * 값 범위: enum spdk_accel_cipher 값 중 하나.
	 * 동기화: 키 단위 read-only. */
	enum spdk_accel_crypto_tweak_mode tweak_mode;
	/* [한국어] tweak_mode — 위 enum 의 한 값. tweak 인코딩 방식 결정.
	 * 설정자/읽는 자/동기화: cipher 와 동일. */

	struct spdk_accel_module_if *module_if;
	/* [한국어] module_if — 이 키를 처리할 모듈 vtable 포인터.
	 * 설정자: 코어가 키 생성 시 사용자 지정 또는 priority 기준으로 결정.
	 * 읽는 자: 사용자 cleanup / submit 에서 모듈로 위임할 때.
	 * 값 범위: NULL 비허용 (생성 직후 항상 채워짐).
	 * 동기화: 키 단위 read-only. */
	/**< Accel module the key belongs to */

	struct spdk_accel_crypto_key_create_param param;
	/* [한국어] param — 사용자 입력 매개변수 원본 보관.
	 * 설정자: 코어가 키 생성 시 그대로 복사. 읽는 자: write_config 등 직렬화.
	 * 값 범위: 사용자가 지정한 모든 옵션 묶음 (이름, hex 키, cipher, mode).
	 * 동기화: 키 단위 read-only. */
	/**< User input parameters */

	TAILQ_ENTRY(spdk_accel_crypto_key) link;
	/* [한국어] link — 코어 내부 키 리스트(전체 등록 키)의 TAILQ 링크.
	 * 설정자: 코어가 키 register 시 head 에 삽입.
	 * 읽는 자: 코어가 RPC list / cleanup 시 순회.
	 * 동기화: 코어 내부 mutex 로 보호. */
};

/* [한국어] struct spdk_accel_bounce_buffer - 모듈이 memory_domain 을 지원
 * 하지 않을 때 코어가 사용자 메모리 도메인의 데이터를 일반 host memory 로
 * pull/push 하기 위해 사용하는 보조 구조체. 사용자에게는 노출되지 않으며
 * 모듈도 직접 만지지 않는다 (코어 전용). */
/**
 * Describes user's buffers in remote memory domains in case a module doesn't support memory domains
 * and accel needs to pull/push the data before submitting a task.  Should only be used by accel
 * itself and should not be touched by accel modules.
 */
struct spdk_accel_bounce_buffer {
	struct iovec *orig_iovs;
	/* [한국어] orig_iovs — 원본(사용자 도메인) iovec 배열.
	 * 설정자: 코어가 task 진입 시 사용자 iov 를 잠시 잡아 두는 용도.
	 * 읽는 자: 코어 push/pull 콜백.
	 * 동기화: task 단위 — 동일 task 가 다중 thread 에서 동시 다뤄지지 않음. */
	uint32_t orig_iovcnt;
	/* [한국어] orig_iovcnt — orig_iovs 배열 길이. */
	struct spdk_memory_domain *orig_domain;
	/* [한국어] orig_domain — 사용자 데이터가 위치한 memory_domain 핸들.
	 * 예: GPU 메모리 도메인, RDMA-MR 도메인 등. */
	void *orig_domain_ctx;
	/* [한국어] orig_domain_ctx — domain 별 부가 컨텍스트(MR key 등). */
	struct iovec iov;
	/* [한국어] iov — bounce 후 host 메모리 안에 머무는 임시 iovec 1 개.
	 * 단일 iov 인 이유: bounce buffer 는 코어가 한 번에 alloc 하는
	 * contig 영역이라 단일 iov 로 충분. */
};

/* [한국어] enum spdk_accel_aux_iov_type - aux_data 안의 iovs[] 인덱스 라벨.
 * 어떤 op (예: compress) 는 src/dst 외에 src2/dst2 같은 보조 iov 를 추가로
 * 필요로 한다. 코어/모듈이 이 인덱스 enum 으로 같은 슬롯을 가리키도록 표준화. */
enum spdk_accel_aux_iov_type {
	SPDK_ACCEL_AUX_IOV_SRC,
	/* [한국어] SRC — 1차 source iov 슬롯. 일반 op 의 입력. */
	SPDK_ACCEL_AUX_IOV_DST,
	/* [한국어] DST — 1차 destination iov 슬롯. 일반 op 의 출력. */
	SPDK_ACCEL_AUX_IOV_SRC2,
	/* [한국어] SRC2 — 2차 source (예: dual-source compare/xor). */
	SPDK_ACCEL_AUX_IOV_DST2,
	/* [한국어] DST2 — 2차 destination. */
	SPDK_ACCEL_AXU_IOV_VIRT_SRC,
	/* [한국어] VIRT_SRC — virtual src (memory domain bounce 용 가상 iov).
	 * (원본의 enum 이름은 AXU 로 오타가 있으나 ABI 호환을 위해 보존됨.) */
	SPDK_ACCEL_AXU_IOV_VIRT_DST,
	/* [한국어] VIRT_DST — virtual dst. */
	SPDK_ACCEL_AUX_IOV_MAX,
	/* [한국어] MAX — 배열 크기 sentinel. iovs[] 정적 배열의 길이로 사용. */
};

/* [한국어] struct spdk_accel_task_aux_data - bounce buffer 와 보조 iov 를
 * 묶어 SLIST 로 free-list 풀링되는 구조체. task 마다 항상 필요한 게 아니
 * 라서 별도 풀에서 빌려 쓰고 task 완료 시 반납하는 패턴. */
struct spdk_accel_task_aux_data {
	SLIST_ENTRY(spdk_accel_task_aux_data) link;
	/* [한국어] link — accel 코어 내부 free-list 의 SLIST 링크.
	 * 설정자/읽는 자: 코어 alloc/free pool 로직. 동기화: per-channel
	 * pool 이라 lockless. */
	struct iovec iovs[SPDK_ACCEL_AUX_IOV_MAX];
	/* [한국어] iovs — aux 인덱스 enum 으로 슬롯 별로 채워지는 iov 배열.
	 * 설정자: 코어 / 사용자 submit 함수. 읽는 자: 모듈 submit_tasks. */
	struct {
		struct spdk_accel_bounce_buffer s;
		/* [한국어] s — source 측 bounce buffer (사용자 src 가 remote
		 *               domain 에 있을 때 host bounce). */
		struct spdk_accel_bounce_buffer d;
		/* [한국어] d — destination 측 bounce buffer. */
	} bounce;
	/* [한국어] bounce — src/dst 양쪽 bounce buffer 묶음. memory domain
	 * 을 지원하지 않는 모듈에서 코어가 데이터를 잠시 host 로 끌어와 처리. */
};

/* [한국어] struct spdk_accel_task - 단일 가속 op 의 모든 컨텍스트를 담는
 * "단위 task" 구조체. 사용자가 spdk_accel_submit_* 를 호출할 때마다 하나씩
 * 만들어지며, 모듈 submit_tasks 는 이 객체를 받아 백엔드에 발행하고
 * spdk_accel_task_complete 로 결과를 보고한다. 매우 빈번하게 alloc/free
 * 되므로 per-channel mempool 에서 관리된다. */
struct spdk_accel_task {
	TAILQ_ENTRY(spdk_accel_task) seq_link;
	/* [한국어] seq_link — accel sequence 안에서 이 task 의 자리.
	 * 설정자: spdk_accel_append_* 가 sequence 의 tail 에 삽입.
	 * 읽는 자: spdk_accel_sequence_first_task / next_task 가 순회.
	 * 동기화: sequence 는 단일 thread 에서만 관리되므로 lock 없음. */
	STAILQ_ENTRY(spdk_accel_task) link;
	/* [한국어] link — 모듈 채널의 in-flight task 큐 STAILQ 링크.
	 * 설정자: 모듈이 백엔드에 제출 후 자기 in-flight 큐에 추가.
	 * 읽는 자: 모듈 polling 코드가 완료 검사 시 head 부터 검사.
	 * 동기화: per-channel — lockless. */

	/* Uses enum spdk_accel_opcode */
	uint8_t op_code;
	/* [한국어] op_code — copy/fill/crc32c/compress/encrypt/... 등 op 종류.
	 * 설정자: 사용자 spdk_accel_submit_* 가 enum 값으로 채움.
	 * 읽는 자: 모듈 submit_tasks 가 dispatch 분기 키로 사용.
	 * 값 범위: enum spdk_accel_opcode 값. uint8_t 로 압축 저장. */
	bool has_aux;
	/* [한국어] has_aux — task 에 aux_data 가 부착되었는지 여부.
	 * 설정자: 코어가 aux 가 필요한 op 만 true.
	 * 읽는 자: 모듈 / 코어 cleanup 이 aux_data 를 풀에 반납해야 할지 결정. */
	int16_t status;
	/* [한국어] status — 완료 시 결과 코드 (0 / 음수 errno).
	 * 설정자: spdk_accel_task_complete(task, status) 가 기록.
	 * 읽는 자: 코어 cb_fn 디스패치 / sequence 진행 검사.
	 * 동기화: 단일 thread 안에서만 set/get. */
	uint8_t reserved[4];
	/* [한국어] reserved — 정렬·향후 확장용 패딩 (현재 미사용). */

	struct accel_io_channel *accel_ch;
	/* [한국어] accel_ch — 이 task 가 발급된 코어 io_channel.
	 * 설정자: spdk_accel_submit_* 가 현재 thread 의 채널을 채움.
	 * 읽는 자: 코어 cb 디스패치 / 모듈 routing.
	 * 동기화: thread-local 채널 — lockless. */
	struct spdk_accel_sequence *seq;
	/* [한국어] seq — 이 task 가 속한 sequence (chained op). 단일 op 면 NULL.
	 * 설정자: spdk_accel_append_* / sequence 매니저.
	 * 읽는 자: 완료 처리 시 다음 step 으로 진행. */
	union {
		spdk_accel_completion_cb cb_fn;
		/* [한국어] cb_fn — 단일 task 완료 콜백 시그니처
		 *   void (*)(void *cb_arg, int status).
		 * 설정자: spdk_accel_submit_*. 읽는 자: 코어 완료 디스패치.
		 * 컨텍스트: 발급된 thread 와 동일한 thread 에서 호출. */
		spdk_accel_step_cb step_cb_fn;
		/* [한국어] step_cb_fn — sequence 안의 한 step 완료 콜백.
		 * 설정자: spdk_accel_append_*. 시그니처는 op 마다 다름. */
	};
	/* [한국어] cb_fn 과 step_cb_fn 은 사용 시점이 배타적이므로 union. */
	void *cb_arg;
	/* [한국어] cb_arg — 위 cb_fn/step_cb_fn 에 전달될 사용자 컨텍스트. */

	struct spdk_memory_domain *src_domain;
	/* [한국어] src_domain — src 데이터가 위치한 memory domain (NULL=host).
	 * 설정자: 사용자 submit_*_ext / append_*_ext.
	 * 읽는 자: 모듈 (지원 시) / 코어 (bounce). */
	void *src_domain_ctx;
	/* [한국어] src_domain_ctx — src_domain 의 부가 컨텍스트 (MR key 등). */
	struct spdk_memory_domain *dst_domain;
	/* [한국어] dst_domain — dst 메모리 도메인. NULL 이면 host. */
	void *dst_domain_ctx;
	/* [한국어] dst_domain_ctx — dst_domain 부가 컨텍스트. */

	uint64_t nbytes;
	/* [한국어] nbytes — 이 op 가 처리할 총 바이트 수.
	 * 설정자: 사용자 submit_*. 읽는 자: 모듈/코어. */

	union {
		struct {
			struct iovec *iovs; /* iovs passed by the caller */
			uint32_t iovcnt;    /* iovcnt passed by the caller */
		} s;
		/* [한국어] s — 일반적인 src iov + cnt 형태의 source 입력.
		 * 사용 op: copy/encrypt/compress 등 대부분. */
		struct {
			void **srcs;
			uint32_t cnt;
		} nsrcs;
		/* [한국어] nsrcs — 다중 source 포인터 배열 (예: xor/crc 다중 입력).
		 * 사용 op: 연산 종류에 따라 분기되어 op_code 가 결정. */
	};
	/* [한국어] 두 형태는 op 마다 배타적으로 쓰이므로 union 으로 메모리 절약. */

	union {
		struct {
			struct iovec *iovs; /* iovs passed by the caller */
			uint32_t iovcnt;    /* iovcnt passed by the caller */
		} d;
		/* [한국어] d — destination iov + cnt. 대부분의 op 에서 사용. */
		struct {
			struct iovec *iovs;
			uint32_t iovcnt;
		} s2;
		/* [한국어] s2 — secondary source iov (compare 등). */
	};

	union {
		struct {
			struct iovec *iovs;
			uint32_t iovcnt;
		} d2;
		/* [한국어] d2 — secondary destination iov. */
		uint32_t seed;
		/* [한국어] seed — crc32c 등 hash 의 초기 seed. */
		uint64_t fill_pattern;
		/* [한국어] fill_pattern — fill op 의 패턴 (8B 반복). */
		struct spdk_accel_crypto_key *crypto_key;
		/* [한국어] crypto_key — 암복호화 op 에서 사용할 키 핸들. */
		struct {
			const struct spdk_dif_ctx *ctx;
			/* [한국어] ctx — DIF (T10 PI) 컨텍스트. */
			struct spdk_dif_error *err;
			/* [한국어] err — DIF 오류 보고 출력 영역. */
			uint32_t num_blocks;
			/* [한국어] num_blocks — DIF 검사 대상 블록 수. */
		} dif;
		/* [한국어] dif — DIF (T10 PI) 검사/생성 op 전용 묶음. */
		struct {
			enum spdk_accel_comp_algo algo;
			/* [한국어] algo — 사용할 압축/해제 알고리즘 (deflate 등). */
			uint32_t level;
			/* [한국어] level — 압축 강도(예: 1~9 deflate level). */
		} comp;
		/* [한국어] comp — compress/decompress op 전용 묶음. */
	};

	union {
		uint32_t *crc_dst;
		/* [한국어] crc_dst — crc32c 결과가 기록될 사용자 포인터. */
		uint32_t *output_size;
		/* [한국어] output_size — compress 후 실제 출력 크기 기록 포인터. */
		uint32_t block_size; /* for crypto op */
		/* [한국어] block_size — crypto op 의 블록 크기 (예: 4096). */
	};
	uint64_t iv;
	/* [한국어] iv — Initialization Vector / tweak 의 raw 값.
	 * AES-XTS 의 경우 tweak_mode 와 함께 LBA 기반으로 변환됨. */
	/* Initialization vector (tweak) for crypto op */

	struct spdk_accel_task_aux_data *aux;
	/* [한국어] aux — bounce + 보조 iov 를 담은 부속 객체 (필요 시 NULL 아님).
	 * 설정자: 코어가 has_aux=true 일 때 풀에서 빌려와 부착.
	 * 읽는 자: 모듈/코어. 동기화: per-channel pool. */
};

/* [한국어] struct spdk_accel_opcode_info - 모듈/드라이버가 특정 op 의 실행
 * 제약을 코어에 보고할 때 사용하는 정보 컨테이너. 코어는 사용자 buffer
 * 정렬을 검사할 때 이 정보를 참조. */
struct spdk_accel_opcode_info {
	uint8_t required_alignment;
	/* [한국어] required_alignment — 버퍼 정렬 (2 의 거듭제곱 지수).
	 * 설정자: module->get_operation_info / driver->get_operation_info.
	 * 읽는 자: 코어가 사용자 iov base 가 정렬 조건에 맞지 않으면 bounce
	 *          또는 SW fallback 으로 우회.
	 * 값 범위: 0=정렬 불필요, n>0 이면 2^n 바이트 정렬 필요.
	 * 동기화: 정적 query 결과 — 별도 sync 불필요. */
	/**
	 * Minimum buffer alignment required to execute the operation, expressed as power of 2.  The
	 * value of 0 means that the buffers don't need to be aligned.
	 */
};

/* [한국어] struct spdk_accel_module_if - accel 백엔드 모듈의 vtable.
 * 모든 함수 포인터는 코어가 dispatch 시점에 호출. NULL 가능 여부는 각
 * 필드 주석 참조. */
struct spdk_accel_module_if {
	const char *name;
	/* [한국어] name — 모듈 이름 (NUL-terminated, unique).
	 * 설정자: 모듈 정적 초기화. 읽는 자: 코어 룩업 / 사용자 priority 매핑. */
	/** Name of the module. */

	int priority;
	/* [한국어] priority — 동일 op_code 를 지원하는 모듈이 여럿일 때 코어가
	 *                    선호 순서를 정하는 값.
	 * 설정자: 모듈 정적 초기화 (HW 모듈은 큰 양수, SW 는 SPDK_ACCEL_SW_PRIORITY=-1).
	 * 읽는 자: 코어 dispatch 알고리즘.
	 * 의미: 사용자가 spdk_accel_assign_opc 로 강제 매핑하지 않은 op 에만 적용. */
	/**
	 * Priority of the module.  It's used to select a module to execute an operation when
	 * multiple modules support it.  Higher value means higher priority.  Software module has a
	 * priority of `SPDK_ACCEL_SW_PRIORITY`.  Of course, this value is only relevant when none
	 * of the modules have been explicitly assigned to execute a given operation via
	 * `spdk_accel_assign_opc()`.
	 */

	int (*module_init)(void);
	/* [한국어] module_init — 모듈 초기화 콜백 (필수 구현).
	 * 호출자: 코어가 spdk_accel_initialize 단계에서 호출.
	 * 반환: 0 성공 / 음수 errno (특히 -ENODEV 면 모듈은 비활성화되지만
	 *        framework init 은 계속 진행).
	 * 의미: 디바이스 probe, 자료구조 alloc 등. */
	/**
	 * Initialization function for the module.  Called by the application during startup.
	 *
	 * Return 0 on success or negative error code. If -ENODEV is returned - the module
	 * will not be used to handle any operation, but the error will not stop framework
	 * initialization.
	 *
	 * Modules are required to define this function.
	 */

	void (*module_fini)(void *ctx);
	/* [한국어] module_fini — 모듈 종료 콜백 (선택). NULL 가능.
	 * 호출자: 코어가 종료 흐름에서 호출.
	 * 비동기: 모듈이 정리 완료 후 spdk_accel_module_finish() 를 호출해
	 *          코어에 알려야 한다 (즉시 동기 종료가 아님). */
	/**
	 * Finish function for the module.  Called by the application before the application exits
	 * to perform any necessary cleanup.
	 *
	 * Modules are not required to define this function.
	 */

	void (*write_config_json)(struct spdk_json_write_ctx *w);
	/* [한국어] write_config_json — 모듈 설정을 JSON 으로 dump.
	 * 호출자: RPC save_config 흐름. 의미: 비밀 아닌 설정만 출력. */
	/** Write Acceleration module configuration into provided JSON context. */

	size_t (*get_ctx_size)(void);
	/* [한국어] get_ctx_size — 모듈이 task 에 부착해 사용할 추가 컨텍스트 바이트 수.
	 * 호출자: 코어가 task mempool 의 element 크기 결정 시.
	 * 반환: 0 또는 양수. */
	/** Returns the allocation size required for the modules to use for context. */

	bool (*supports_opcode)(enum spdk_accel_opcode);
	/* [한국어] supports_opcode — 모듈이 특정 op 를 처리할 수 있는지 보고.
	 * 호출자: 코어 dispatch 알고리즘.
	 * 반환: true=지원, false=비지원. */
	/** Reports whether the module supports a given operation. */

	struct spdk_io_channel *(*get_io_channel)(void);
	/* [한국어] get_io_channel — 현재 SPDK thread 에서의 모듈 io_channel 획득.
	 * 호출자: 코어가 채널을 task 에 부착할 때.
	 * 의미: thread 별 큐/리소스 분리. */
	/** Returns module's IO channel on the calling thread. */

	int (*submit_tasks)(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task);
	/* [한국어] submit_tasks — task 를 받아 백엔드에 비동기 발행.
	 * 호출자: 코어가 dispatch 결과로 호출.
	 * 반환: 0 성공 (완료는 비동기) / 음수 errno (즉시 실패).
	 * 모듈 의무: 완료 시 반드시 spdk_accel_task_complete 호출. */
	/**
	 * Submit tasks to be executed by the module.  Once a task execution is done, the module is
	 * required to complete it using `spdk_accel_task_complete()`.  `ch` is the IO channel
	 * obtained by `get_io_channel()`.
	 */

	int (*crypto_key_init)(struct spdk_accel_crypto_key *key);
	/* [한국어] crypto_key_init — crypto 키 객체에 대해 모듈 priv 를 초기화.
	 * 호출자: 사용자 keys create 시 코어가 dispatch.
	 * 반환: 0 / 음수 errno. */
	/**
	 * Create crypto key function. Module is responsible to fill all necessary parameters in
	 * \b spdk_accel_crypto_key structure
	 */

	void (*crypto_key_deinit)(struct spdk_accel_crypto_key *key);
	/* [한국어] crypto_key_deinit — crypto_key_init 에서 잡은 자원 해제. */
	/** Free any resources associated with `key` allocated during `crypto_key_init()`. */

	bool (*crypto_supports_tweak_mode)(enum spdk_accel_crypto_tweak_mode tweak_mode);
	/* [한국어] crypto_supports_tweak_mode — 특정 tweak 모드 지원 여부.
	 * 콜백 미구현 시 코어는 SIMPLE_LBA 만 지원으로 간주. */
	/**
	 * Returns true if given tweak mode is supported. If module doesn't implement that function it shall support SIMPLE LBA mode.
	 */

	bool (*crypto_supports_cipher)(enum spdk_accel_cipher cipher, size_t key_size);
	/* [한국어] crypto_supports_cipher — (cipher, key_size) 쌍 지원 여부. */
	/**
	 * Returns true if given pair (cipher, key size) is supported.
	 */

	bool (*compress_supports_algo)(enum spdk_accel_comp_algo algo);
	/* [한국어] compress_supports_algo — 압축 알고리즘 지원 여부. */
	/**
	 * Return true if compresssion algo is supported, false otherwise.
	 */

	int (*get_compress_level_range)(enum spdk_accel_comp_algo algo,
					uint32_t *min_level, uint32_t *max_level);
	/* [한국어] get_compress_level_range — algo 의 (min, max) level 보고.
	 * 호출자: RPC / 코어가 사용자 입력 level 검증 시 사용. */
	/**
	 * Returns the lowest and highest levels of the specified algorithm.
	 */

	int (*get_memory_domains)(struct spdk_memory_domain **domains, int num_domains);
	/* [한국어] get_memory_domains — 모듈이 직접 다룰 수 있는 memory domain 목록 보고.
	 * domains==NULL 이면 개수만 반환 (probing 패턴).
	 * 미구현이면 모듈은 domain 비지원 — 코어가 bounce 로 우회. */
	/**
	 * Returns memory domains supported by the module.  If NULL, the module does not support
	 * memory domains.  The `domains` array can be NULL, in which case this function only
	 * returns the number of supported memory domains.
	 *
	 * \param domains Memory domain array.
	 * \param num_domains Size of the `domains` array.
	 *
	 * \return Number of supported memory domains.
	 */

	int (*get_operation_info)(enum spdk_accel_opcode opcode,
				  const struct spdk_accel_operation_exec_ctx *ctx,
				  struct spdk_accel_opcode_info *info);
	/* [한국어] get_operation_info — op 별 제약(정렬 등) 정보 보고.
	 * 미구현이면 제약 없음으로 간주. */
	/**
	 * Returns information/constraints for a given operation.  If unimplemented, it is assumed
	 * that the module doesn't have any constraints to execute any operation.
	 */

	TAILQ_ENTRY(spdk_accel_module_if) tailq;
	/* [한국어] tailq — 코어 내부 모듈 리스트 링크.
	 * 설정자: spdk_accel_module_list_add. 동기화: register 는 constructor
	 *          시점이라 단일 thread → 이후 read-only. */
};

/*
 * [한국어]
 * spdk_accel_module_list_add - 모듈 vtable 을 코어 리스트에 등록.
 *
 * @accel_module: 모듈 vtable 포인터.
 * @return: void.
 *
 * 동기/배경: 보통 SPDK_ACCEL_MODULE_REGISTER 매크로가 main() 이전에
 * 자동으로 호출. 직접 호출은 거의 없다.
 *
 * 실행 컨텍스트: ELF constructor — 단일 스레드 환경.
 */
void spdk_accel_module_list_add(struct spdk_accel_module_if *accel_module);

/* [한국어] SPDK_ACCEL_MODULE_REGISTER - constructor 자동 등록 매크로.
 * 사용법:
 *   static struct spdk_accel_module_if g_dsa_module = { .name = "dsa", ... };
 *   SPDK_ACCEL_MODULE_REGISTER(dsa, &g_dsa_module);
 *
 * 동작 원리: __attribute__((constructor)) 가 붙은 함수는 ELF 로더가
 * main() 이전에 자동 호출하므로, accel framework 가 init 될 때 이미 모든
 * 모듈이 코어 리스트에 등록되어 있다. 매크로 인자 name 은 함수 심볼 충돌
 * 방지용 token paste. SPDK 의 다른 자동 등록 패턴 (BDEV/KEYRING/NVME 등) 과
 * 동일한 형태이며, 사용자가 별도 init 코드 추가 없이 모듈을 정적/동적
 * 링크만으로 활성화할 수 있게 한다. */
#define SPDK_ACCEL_MODULE_REGISTER(name, module) \
static void __attribute__((constructor)) _spdk_accel_module_register_##name(void) \
{ \
	spdk_accel_module_list_add(module); \
}

/* [한국어] SPDK_ACCEL_SW_PRIORITY = -1 — SW 모듈(ISA-L 기반)의 priority 값.
 * 음수로 둔 이유: 어떤 HW 모듈이든 자기 priority 를 0 이상으로 두면 자동
 * 으로 SW 보다 우선 선택되도록 보장. SW 는 항상 fallback 위치. */
/* Priority of the accel_sw module */
#define SPDK_ACCEL_SW_PRIORITY (-1)

/*
 * [한국어]
 * spdk_accel_module_finish - 모듈이 module_fini 콜백에서 시작한 비동기 정리
 *                            완료를 코어에 보고한다.
 *
 * @return: void.
 *
 * 동기/배경: module_fini 는 비동기 종료 모델이므로, 모듈은 자기 종료 작업이
 * 끝난 시점에 이 함수를 호출해 코어가 다음 모듈 / 다음 단계를 진행할 수
 * 있게 알린다.
 *
 * 실행 컨텍스트: 모듈 thread (보통 io_channel 이 발급된 SPDK thread).
 *
 * 호출 체인:
 *   module_fini → 백엔드 정리 완료 → [spdk_accel_module_finish] → 코어 next
 */
/**
 * Called by an accel module when cleanup initiated during .module_fini has completed
 */
void spdk_accel_module_finish(void);

/* [한국어] struct spdk_accel_driver - sequence 단위로 chained op 들을 한
 * 번에 실행할 수 있는 platform driver vtable. 일반 module 은 op 단위 실행을
 * 하지만 driver 는 multi-op pipeline 을 자기 device 에 그대로 위임한다 (예:
 * DPU). driver 가 등록되어 있으면 sequence 가 그쪽으로 위임되며, driver 가
 * 처리할 수 없는 op 가 끼어들면 spdk_accel_sequence_continue 를 통해 코어
 * 에 넘겨 일반 모듈 디스패치로 fallback 한다. */
/**
 * Platform driver responsible for executing tasks in a sequence.  If no driver is selected, tasks
 * are submitted to accel modules.  All drivers are required to be aware of memory domains.
 */
struct spdk_accel_driver {
	const char *name;
	/* [한국어] name — driver 이름 (unique). */
	/** Name of the driver. */

	int (*init)(void);
	/* [한국어] init — driver 초기화 콜백 (선택). 0 / 음수 errno. */
	/** Initializes the driver, called when accel initializes.  Optional. */

	void (*fini)(void);
	/* [한국어] fini — driver 종료 콜백 (선택). 동기. */
	/** Performs cleanup on resources allocated by the driver.  Optional. */

	int (*execute_sequence)(struct spdk_io_channel *ch, struct spdk_accel_sequence *seq);
	/* [한국어] execute_sequence — sequence 의 chained 실행 진입점.
	 * 호출자: 코어가 sequence 시작 시 dispatch.
	 * 반환: 0 성공 (완료는 비동기, 각 task 마다 spdk_accel_task_complete +
	 *        sequence 끝/정지 시 spdk_accel_sequence_continue),
	 *        음수 errno (즉시 실패 — 이때는 sequence_continue 호출 금지).
	 * 의무: driver 는 memory_domain 을 반드시 직접 처리해야 한다 (코어 bounce
	 *        없음). */
	/**
	 * Executes a sequence of accel operations.  The driver should notify accel about each
	 * completed task using `spdk_accel_task_complete()`.  Once all tasks are completed or the
	 * driver cannot proceed with a given task (e.g. because it doesn't handle specific opcode),
	 * accel should be notified via `spdk_accel_sequence_continue()`.  If there are tasks left
	 * in a sequence, the first will be submitted to a module, while the rest will be sent back
	 * to the driver.  `spdk_accel_sequence_continue()` should only be called if this function
	 * succeeds (i.e. returns 0).
	 *
	 * \param ch IO channel obtained by `get_io_channel()`.
	 * \param seq Sequence of tasks to execute.
	 *
	 * \return 0 on success, negative errno on failure.
	 */

	struct spdk_io_channel *(*get_io_channel)(void);
	/* [한국어] get_io_channel — driver 의 채널 (현재 thread 기준). */
	/** Returns IO channel that will be passed to `execute_sequence()`. */

	int (*get_operation_info)(enum spdk_accel_opcode opcode,
				  const struct spdk_accel_operation_exec_ctx *ctx,
				  struct spdk_accel_opcode_info *info);
	/* [한국어] get_operation_info — op 별 제약 보고. 미구현 시 제약 없음. */
	/**
	 * Returns information/constraints for a given operation.  If unimplemented, it is assumed
	 * that the driver doesn't have any constraints to execute any operation.
	 */

	TAILQ_ENTRY(spdk_accel_driver) tailq;
	/* [한국어] tailq — 코어 driver 리스트 링크. constructor 에서 등록. */
};

/*
 * [한국어]
 * spdk_accel_sequence_continue - driver 또는 모듈이 sequence 진행을 코어에 위임.
 *
 * @seq: 진행할 sequence 객체.
 * @return: void.
 *
 * 동기/배경: driver 가 자기가 처리할 수 없는 task 를 만나거나, 부분 처리
 * 만 한 뒤 나머지를 모듈 디스패치로 넘기고 싶을 때 호출. 코어는 남은
 * task 의 첫 번째를 적절한 모듈에 보내고, 그 다음은 다시 driver 로 보낸다.
 *
 * 실행 컨텍스트: driver thread (호출 시점).
 *
 * 호출 체인:
 *   driver execute_sequence → [spdk_accel_sequence_continue] → 코어 dispatch
 */
/**
 * Notifies accel that a driver has finished executing a sequence (or its part) and accel should
 * continue processing it.
 *
 * \param seq Sequence object.
 */
void spdk_accel_sequence_continue(struct spdk_accel_sequence *seq);

/*
 * [한국어]
 * spdk_accel_driver_register - driver vtable 을 코어 driver 리스트에 등록.
 *
 * @driver: driver vtable 포인터.
 * @return: void.
 *
 * 동기/배경: 보통 SPDK_ACCEL_DRIVER_REGISTER 매크로가 자동 호출.
 */
void spdk_accel_driver_register(struct spdk_accel_driver *driver);

/* [한국어] SPDK_ACCEL_DRIVER_REGISTER - driver 자동 등록 매크로.
 * MODULE 매크로와 동일한 constructor 패턴 — main() 이전 자동 등록. */
#define SPDK_ACCEL_DRIVER_REGISTER(name, driver) \
static void __attribute__((constructor)) _spdk_accel_driver_register_##name(void) \
{ \
	spdk_accel_driver_register(driver); \
}

/* [한국어] spdk_accel_sequence_get_buf_cb - sequence 에서 임시 버퍼 alloc
 * 이 즉시 성공하지 못하고 backpressure 가 걸렸을 때, 가용 시점에 호출되는
 * 콜백 시그니처. seq 와 사용자 cb_arg 를 받는다. */
typedef void (*spdk_accel_sequence_get_buf_cb)(struct spdk_accel_sequence *seq, void *cb_arg);

/*
 * [한국어]
 * spdk_accel_alloc_sequence_buf - sequence 안에서 사용할 임시 accel buffer 를 alloc.
 *
 * @seq: sequence.
 * @buf: alloc 결과를 받을 사용자 포인터 (모듈 정의된 의미).
 * @domain: alloc 할 memory domain.
 * @domain_ctx: domain 부가 컨텍스트.
 * @cb_fn: 즉시 alloc 못 했을 때, 가용 시점에 호출될 콜백.
 * @cb_ctx: cb_fn 에 전달될 사용자 컨텍스트.
 * @return: true=즉시 alloc 성공 (cb_fn 호출 안 됨) / false=대기 후 cb_fn 호출.
 *
 * 동기/배경: chained 가속 sequence 에서 중간 결과를 담을 버퍼는 매번 사용
 * 자가 따로 관리하지 않고 accel framework 가 풀에서 빌려 준다. 풀이 비어
 * 있으면 cb_fn 으로 비동기 알림.
 *
 * 실행 컨텍스트: 사용자/모듈 thread (sequence 가 활성인 thread).
 *
 * 호출 체인:
 *   sequence builder / step 콜백 → [spdk_accel_alloc_sequence_buf] →
 *   (즉시 가능) buf 채움, true 반환 / (불가) cb_fn 등록, false 반환
 */
/**
 * Allocates memory for an accel buffer in a given sequence.  The callback is only executed if the
 * buffer couldn't be allocated immediately.
 *
 * \param seq Sequence object.
 * \param buf Accel buffer to allocate.
 * \param domain Accel memory domain.
 * \param domain_ctx Memory domain context.
 * \param cb_fn Callback to be executed once the buffer is allocated.
 * \param cb_ctx Argument to be passed to `cb_fn`.
 *
 * \return true if the buffer was immediately allocated, false otherwise.
 */
bool spdk_accel_alloc_sequence_buf(struct spdk_accel_sequence *seq, void *buf,
				   struct spdk_memory_domain *domain, void *domain_ctx,
				   spdk_accel_sequence_get_buf_cb cb_fn, void *cb_ctx);

/*
 * [한국어]
 * spdk_accel_sequence_first_task - sequence 의 첫 outstanding task 반환.
 *
 * @seq: sequence.
 * @return: 다음에 실행될 task 포인터 / NULL (모두 완료됨).
 *
 * 동기/배경: driver/모듈이 sequence 의 다음 step 을 검사하기 위해 사용.
 *
 * 실행 컨텍스트: sequence 가 활성인 thread.
 */
/**
 * Returns the first task remaining to be executed in a given sequence.
 *
 * \param seq Sequence object.
 *
 * \return the first remaining task or NULL if all tasks are already completed.
 */
struct spdk_accel_task *spdk_accel_sequence_first_task(struct spdk_accel_sequence *seq);

/*
 * [한국어]
 * spdk_accel_sequence_next_task - 주어진 task 의 다음 task 반환.
 *
 * @task: 현재 task (아직 spdk_accel_task_complete 호출 전이어야 함).
 * @return: 다음 task / NULL (마지막 task).
 *
 * 동기/배경: chained 처리에서 다음 step lookahead 가 필요할 때 사용.
 *
 * 실행 컨텍스트: sequence 가 활성인 thread.
 */
/**
 * Returns the next remaining task that follows a given task in a sequence.
 *
 * \param task Accel task.  This task must be still outstanding (i.e. it wasn't completed through
 *             `spdk_accel_task_complete()`).
 *
 * \return the next task or NULL if `task` was the last task in a sequence.
 */
struct spdk_accel_task *spdk_accel_sequence_next_task(struct spdk_accel_task *task);

/*
 * [한국어]
 * spdk_accel_get_module - 이름으로 등록된 모듈 룩업.
 *
 * @name: 모듈 이름.
 * @return: 발견된 모듈 vtable 포인터 / NULL.
 *
 * 동기/배경: 사용자가 spdk_accel_assign_opc 등으로 명시적 모듈 매핑을
 * 지정할 때, 이름으로 모듈을 찾아 핸들을 얻기 위해 사용.
 *
 * 실행 컨텍스트: 임의의 SPDK thread. 등록은 constructor 에서 끝나므로 이후
 * 룩업은 lock-free.
 *
 * 호출 체인:
 *   RPC accel_assign_opc / 사용자 코드 → [spdk_accel_get_module] →
 *   코어 module 리스트 순회
 */
/**
 * Returns an accel module identified by `name`.
 *
 * \param name Name of the module.
 *
 * \return Pointer to a module or NULL if it couldn't be found.
 */
struct spdk_accel_module_if *spdk_accel_get_module(const char *name);

/* [한국어] extern "C" 가드 종료. */
#ifdef __cplusplus
}
#endif

#endif
/* [한국어] 헤더 가드 종료. SPDK_ACCEL_MODULE_H. */
