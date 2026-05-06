/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES
 *   All rights reserved.
 */

/** \file
 * Acceleration Framework
 */

/*
 * [한국어 설명] SPDK Acceleration Framework 공개 API 헤더 (accel.h)
 *
 * === 파일의 역할 ===
 * SPDK Acceleration Framework(이하 accel)는 데이터 변환 작업(메모리 복사, 채움, 비교,
 * CRC32C 계산, 압축/해제, 암호화/복호화, T10 DIF/DIX 삽입/제거 등)을 통일된 비동기
 * 인터페이스로 제공하는 추상화 계층이다. 이 헤더는 사용자(주로 lib/bdev, blob, iSCSI,
 * nvmf 같은 상위 서브시스템)가 accel 작업을 제출(submit)·체이닝(append/sequence)하고,
 * 백엔드 모듈을 조회·교체(opcode→module assignment)하며, crypto 키와 통계를
 * 관리하기 위해 호출하는 모든 공개 함수와 자료구조를 선언한다. 백엔드 구현은
 * Intel IDXD/IAA/DSA, IOAT, AMD AE4DMA 같은 하드웨어 가속기와 ISA-L/mlx5 등 SW
 * 라이브러리로 분리되어 module/accel/ 아래에 존재하며, 이 헤더는 그 디테일을
 * 사용자에게서 완전히 숨긴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택에서 accel 은 bdev/blob/nvmf 위에서 호출되고 그 아래의 하드웨어
 * 모듈을 호출하는 "데이터 변환 미들웨어" 위치이다. 호출 체인 예시:
 *   spdk_bdev_io completion path → bdev_accel_sequence → spdk_accel_append_*()
 *     → spdk_accel_sequence_finish() → accel framework dispatch
 *     → IDXD/IAA HW SQ 제출 또는 ISA-L SW 즉시 실행
 *     → poller(spdk_accel_engine_poll)가 완료 수확 → 사용자 cb_fn 호출
 * accel 은 자체 spdk_io_channel 을 갖고 있어 reactor(코어) 단위로 백엔드 채널이
 * 만들어지고, 모든 작업과 그 콜백은 동일한 spdk_thread 위에서 실행되어
 * lockless 를 보장한다. spdk_accel_initialize/finish 는 RPC startup/shutdown
 * 시퀀스 안에서 다른 서브시스템 초기화 사이에 호출된다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(consumer): lib/bdev (bdev_io→accel_sequence 자동 변환), lib/blob
 *   (blob 압축/암호화), lib/iscsi (DDGST/HDGST CRC32C), lib/nvmf (NVMe-oF DIF·crypto),
 *   module/bdev/crypto·compress (전용 bdev 모듈)
 * - 하위(provider): module/accel/dsa (Intel DSA), module/accel/iaa (Intel IAA),
 *   module/accel/ioat, module/accel/dpdk_cryptodev, module/accel/mlx5,
 *   module/accel/ae4dma, lib/accel 내 software 모듈(ISA-L 기반)
 * - 데이터 흐름: 사용자가 iovec/buf 와 spdk_memory_domain 컨텍스트를 넘기면
 *   framework 가 적합한 백엔드를 capability 매트릭스(opcode→module priority)에서
 *   선택하여 작업을 전달한다. sequence 의 경우 framework 가 중간 버퍼(iobuf cache)를
 *   소유하고 zero-copy 최적화로 인접 op 들을 합치거나 생략한다.
 * - 공유 핵심 자료구조: spdk_accel_sequence (불투명 — lib/accel 내부 정의),
 *   spdk_accel_crypto_key, spdk_io_channel(thread.h), spdk_memory_domain(dma.h),
 *   spdk_dif_ctx/spdk_dif_error(dif.h)
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_accel_initialize / spdk_accel_finish: 프레임워크 lifecycle (앱 시작/종료 시)
 * - spdk_accel_get_io_channel: per-thread accel 채널 획득 (모든 submit/append 의 첫 인자)
 * - spdk_accel_submit_*(...): 단일 작업 제출 — copy/fill/compare/dualcast/crc32c/
 *   compress/encrypt/dif_* 등 opcode 별로 함수 분리
 * - spdk_accel_append_*(...): 작업을 sequence 에 큐잉만 함 (실제 실행은 finish 시)
 * - spdk_accel_sequence_finish/abort/reverse: sequence 실행/취소/역순화
 * - spdk_accel_crypto_key_create/destroy/get: AES-CBC/AES-XTS DEK 관리
 * - spdk_accel_assign_opc / get_opc_module_name: 런타임 백엔드 매핑 변경
 * - spdk_accel_get_opcode_stats / spdk_accel_opts: 채널별 통계와 전역 옵션
 * - 핵심 구조체: spdk_accel_crypto_key_create_param(키 생성 파라미터),
 *   spdk_accel_opts(전역 튜닝 — iobuf cache·task_count·sequence_count·buf_count),
 *   spdk_accel_opcode_stats(채널×opcode 통계), spdk_accel_operation_exec_ctx
 *   (alignment 조회 컨텍스트)
 * - 핵심 enum: spdk_accel_opcode(17 종 작업 종류), spdk_accel_cipher(AES_CBC/AES_XTS),
 *   spdk_accel_comp_algo(DEFLATE/LZ4)
 */

#ifndef SPDK_ACCEL_H
/* [한국어] 헤더 중복 포함 가드 — accel.h 가 여러 파일에서 #include 되어도
 * 함수 prototype 과 struct 정의가 중복 선언되지 않도록 한다. */
#define SPDK_ACCEL_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 인클루드 묶음 — stdint, stddef, stdio, sys/uio.h(struct iovec)
 * 등 POSIX/C 표준을 SPDK 빌드 환경에 맞게 한 번에 끌어온다. struct iovec 는
 * 거의 모든 submit_/append_ API 의 데이터 인자로 등장하므로 필수. */
#include "spdk/dma.h"
/* [한국어] spdk_memory_domain 타입을 정의 — 메모리가 호스트 RAM 인지, GPU/RDMA
 * 디바이스 메모리인지, 어떤 가상 주소공간(memory domain)에 속하는지를 표현한다.
 * accel sequence 의 src/dst_domain 인자와 spdk_accel_get_memory_domain()
 * 반환 타입에 사용된다. */
#include "spdk/dif.h"
/* [한국어] spdk_dif_ctx (T10 PI/DIF 설정: PI format, GUARD/APPTAG/REFTAG 동작 모드,
 * 초기 reference tag 등)와 spdk_dif_error (검증 실패 시 상세 에러 컨텍스트)를
 * 정의 — DIF/DIX 계열 spdk_accel_submit_/append_* API 의 ctx·err 인자에 사용. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 컴파일러가 이 헤더를 include 할 때 이름 mangling 없이 C ABI 로
 * 링크되도록 보장 — SPDK 자체는 C 라이브러리이지만 C++ 사용자도 정상 링크되도록. */
#endif

#define SPDK_ACCEL_AES_XTS_128_KEY_SIZE 16
/* [한국어] AES-XTS-128 모드의 단일 키 길이(바이트). XTS 는 키 두 개를 사용하므로
 * 실제 raw key material 은 hex_key+hex_key2 합쳐 32 바이트가 된다.
 * 사용처: crypto 모듈 키 생성 시 키 길이 검증, accel_dpdk_cryptodev 같은 백엔드. */
#define SPDK_ACCEL_AES_XTS_256_KEY_SIZE 32
/* [한국어] AES-XTS-256 모드의 단일 키 길이(바이트). 두 키 합쳐 64 바이트.
 * blobstore 의 페이지 단위 암호화 등 강도 256-bit 가 요구되는 경로에서 사용. */

enum spdk_accel_comp_algo {
	/* [한국어] 압축/해제(submit_compress / submit_decompress / append_decompress_ext
	 * 등)에서 사용할 알고리즘 종류. 각 백엔드(IAA, mlx5, ISA-L)는 advertise 하는
	 * capability 에 따라 일부만 지원하며, accel framework 가 호환되는 모듈을 선택한다. */
	SPDK_ACCEL_COMP_ALGO_DEFLATE = 0,
	/* [한국어] DEFLATE(RFC 1951) — gzip/zlib 의 기반. IAA 하드웨어가 네이티브로
	 * 지원하며 ISA-L SW 백업도 존재. blob compress, bdev_compress 의 기본값. */
	SPDK_ACCEL_COMP_ALGO_LZ4
	/* [한국어] LZ4 — 압축률보다 속도 우선. 일부 SW/HW 백엔드만 지원하며
	 * spdk_accel_get_compress_level_range() 로 레벨 범위를 사전 조회해야 한다. */
};

/** Data Encryption Key identifier */
struct spdk_accel_crypto_key;
/* [한국어] DEK(Data Encryption Key) 핸들의 전방선언 — 실제 정의는 lib/accel/accel.c
 * 에 있고 사용자에게는 불투명 포인터로만 노출된다. spdk_accel_crypto_key_create()
 * 가 만들어 전역 키 테이블에 등록하고 _get(name) 로 조회, _destroy() 로 해제.
 * spdk_accel_submit_encrypt/decrypt 와 _append_encrypt/decrypt 의 key 인자로 사용. */

struct spdk_accel_crypto_key_create_param {
	/* [한국어] crypto key 생성 시 사용자가 채워서 전달하는 파라미터 묶음.
	 * spdk_accel_crypto_key_create() 호출 후 framework 는 이 구조체의 내용을
	 * 내부 키 객체로 복사하므로, 호출자는 반환 후 즉시 buffer 해제 가능. */
	char *cipher;	/**< Cipher to be used for crypto operations */
	/* [한국어] 사용할 암호 알고리즘 이름 문자열. 가능한 값: "AES_CBC", "AES_XTS"
	 * (대소문자 무시). spdk_accel_cipher enum 에 대응되는 텍스트 표현이며,
	 * RPC 인터페이스에서 사용자가 JSON 으로 입력한 값을 그대로 전달할 수 있도록
	 * 문자열로 받는다. 설정자: 사용자(또는 RPC handler accel_crypto_key_create).
	 * 읽는 자: lib/accel 내부에서 cipher enum 으로 변환 후 백엔드에 전달. */
	char *hex_key;	/**< Hexlified key */
	/* [한국어] 키 자료를 hex 문자열로 인코딩한 것 (예: "0123abcd…"). 문자열 길이는
	 * 키 바이트 수의 2 배여야 한다 (AES-128 → 32자, AES-256 → 64자). 보안을 위해
	 * framework 는 사용 후 0 으로 wipe 한다. raw 바이트 배열 대신 hex 를 받는
	 * 이유는 RPC/JSON 직렬화 호환성. */
	char *hex_key2;	/**< Hexlified key2 */
	/* [한국어] AES-XTS 의 두 번째 키(tweak key). AES-CBC 등 single-key 모드에서는
	 * NULL 또는 빈 문자열로 둔다. XTS 표준상 key1 != key2 여야 하며 framework 가
	 * 검증한다. 사용 후 0 으로 wipe. */
	char *tweak_mode;	/**< Tweak mode */
	/* [한국어] XTS 의 tweak 값(IV) 생성 방식 문자열. 가능한 값: "SIMPLE_LBA",
	 * "JOIN_NEG_LBA_WITH_LBA", "INCR_512_FULL_LBA", "INCR_512_UPPER_LBA" 등 —
	 * 어떤 방식으로 LBA → 128-bit tweak 를 derive 하는지 결정. NULL 이면 모듈
	 * 기본값 사용. blob crypto 와 NVMe 호환성에 필요하다. */
	char *key_name;	/**< Key name */
	/* [한국어] 키를 식별할 사용자 지정 이름 (전역 유일). spdk_accel_crypto_key_get(name)
	 * 으로 핸들을 다시 찾을 때 사용되며, RPC 와 JSON config dump 에도 노출된다.
	 * NULL 이거나 중복이면 _create() 가 -EINVAL/-EEXIST 반환. */
};

enum spdk_accel_opcode {
	/* [한국어] accel framework 가 처리하는 모든 작업 종류의 식별자. 각 opcode 마다
	 * 어느 백엔드 모듈이 그것을 처리할 수 있는지가 capability 매트릭스에 등록되며,
	 * spdk_accel_assign_opc(opcode, name) 으로 사용자가 우선순위를 덮어쓸 수 있다.
	 * 0 부터 시작하는 연속된 정수이므로 배열 인덱스로 직접 사용 가능 (LAST 가 크기). */
	SPDK_ACCEL_OPC_COPY			= 0,
	/* [한국어] 메모리→메모리 단순 복사 (memcpy 대체). DMA 엔진(IDXD/DSA/IOAT)이
	 * CPU 사이클을 절약하고 대용량 복사를 비동기로 수행. 스트림 출하 vs 동기적 memcpy
	 * 트레이드오프 존재. */
	SPDK_ACCEL_OPC_FILL			= 1,
	/* [한국어] 단일 바이트 패턴으로 dst 채우기 (memset 대체). NVMe write_zeroes
	 * 폴백 경로, blob 페이지 zeroing 등에 사용. */
	SPDK_ACCEL_OPC_DUALCAST			= 2,
	/* [한국어] 한 src 를 두 개의 dst (4KB 정렬) 로 동시에 복사. RAID-1 mirror 나
	 * scratchpad+영구저장 동시 쓰기 등에 사용. IDXD 가 네이티브 지원하는 IDXD 전용 op. */
	SPDK_ACCEL_OPC_COMPARE			= 3,
	/* [한국어] 두 메모리 영역의 byte 단위 동등성 비교. NVMe COMPARE 명령(CAS),
	 * compare-and-write 의 비교 단계, 결제 검증 등에 사용. status==0 이면 일치,
	 * 0 외이면 mismatch (의도적으로 errno 가 아님 — submit_compare 의 doc 참조). */
	SPDK_ACCEL_OPC_CRC32C			= 4,
	/* [한국어] CRC32C(Castagnoli, 0x1EDC6F41) 계산. NVMe HDGST/DDGST, iSCSI digest,
	 * T10 DIF Type 1 guard, Btrfs 등에서 표준. SSE4.2 의 PCLMUL/CRC32 명령과
	 * IDXD HW 가속 모두 지원. */
	SPDK_ACCEL_OPC_COPY_CRC32C		= 5,
	/* [한국어] 복사하면서 동시에 CRC32C 계산 — 메모리 한 번 순회로 두 작업 처리.
	 * NVMe write 경로의 host buffer→staging 전송 + digest 생성에서 핫패스. */
	SPDK_ACCEL_OPC_COMPRESS			= 6,
	/* [한국어] DEFLATE 또는 LZ4 압축. blob layer 의 압축 옵션, bdev_compress 모듈
	 * 등에 사용. 입력 iovec → 단일 dst buffer (output_size 로 실제 크기 반환). */
	SPDK_ACCEL_OPC_DECOMPRESS		= 7,
	/* [한국어] 압축 해제. dst 가 부족하면 -ENOMEM 반환되며, 사용자는 충분한 버퍼를
	 * 사전 추정해야 한다 (압축 헤더의 raw size 참조 또는 안전 마진). */
	SPDK_ACCEL_OPC_ENCRYPT			= 8,
	/* [한국어] AES-CBC 또는 AES-XTS 암호화. blob crypto, bdev_crypto 모듈에서 사용.
	 * 반드시 spdk_accel_crypto_key_create() 로 키 등록 후 핸들 전달. */
	SPDK_ACCEL_OPC_DECRYPT			= 9,
	/* [한국어] AES-CBC/XTS 복호화. encrypt 와 동일한 key + 동일한 iv/tweak 으로
	 * 호출되어야 데이터가 복원된다. */
	SPDK_ACCEL_OPC_XOR			= 10,
	/* [한국어] 다중 source 의 byte 단위 XOR — RAID-5 parity 계산의 핵심.
	 * dst = sources[0] ^ sources[1] ^ … ^ sources[nsrcs-1]. */
	SPDK_ACCEL_OPC_DIF_VERIFY		= 11,
	/* [한국어] T10 DIF (Data Integrity Field) 검증 — 각 데이터 블록 뒤에 붙은
	 * 8 바이트(또는 16바이트 PI format) GUARD/APPTAG/REFTAG 를 재계산해 비교.
	 * NVMe Protection Information, SCSI T10 PI 호환. */
	SPDK_ACCEL_OPC_DIF_VERIFY_COPY		= 12,
	/* [한국어] DIF 가 포함된 src → DIF 제거된 dst 로 복사 + 검증. NVMe read 경로에서
	 * disk 가 PI 를 첨부해 보낸 데이터를 host 에 전달하기 전 strip. */
	SPDK_ACCEL_OPC_DIF_GENERATE		= 13,
	/* [한국어] In-place DIF 삽입 — 데이터 블록 사이의 metadata 영역에 GUARD/APPTAG/
	 * REFTAG 를 채워넣음. iovec 가 이미 metadata 자리를 포함하고 있어야 한다. */
	SPDK_ACCEL_OPC_DIF_GENERATE_COPY	= 14,
	/* [한국어] data-only src → data+DIF dst 로 복사 + 생성. NVMe write 경로에서
	 * host 가 raw data 만 보냈을 때 디스크 전송 전에 PI 를 부착. */
	SPDK_ACCEL_OPC_DIX_GENERATE		= 15,
	/* [한국어] DIX (Data Integrity Extension) 생성 — DIF 와 달리 metadata 가
	 * 데이터와 분리된 별도 buffer (md_iov) 로 운반. 호스트 측 host-managed
	 * metadata 운영 시 사용. */
	SPDK_ACCEL_OPC_DIX_VERIFY		= 16,
	/* [한국어] DIX 검증 — data iovec 와 별도 md_iov 를 받아 PI 재계산 후 비교.
	 * 실패 시 spdk_dif_error 에 에러 종류·실패 위치 기록. */
	SPDK_ACCEL_OPC_LAST			= 17,
	/* [한국어] 마지막 valid opcode 의 다음 값 — enum 의 크기로 사용된다 (capability
	 * 매트릭스, stats 배열의 차원). 새 op 추가 시 이 값 앞에 삽입하고 LAST 자동 증가. */
};

enum spdk_accel_cipher {
	/* [한국어] crypto 작업의 암호 알고리즘 enum. spdk_accel_crypto_key_create_param.cipher
	 * 문자열을 framework 가 이 enum 으로 변환하여 백엔드에 전달한다. */
	SPDK_ACCEL_CIPHER_AES_CBC,
	/* [한국어] AES Cipher Block Chaining — 16바이트 IV, 블록당 의존성 (병렬화 어려움),
	 * 무결성 보장 없음. 레거시 호환용. */
	SPDK_ACCEL_CIPHER_AES_XTS,
	/* [한국어] AES XEX-based Tweaked-codebook with ciphertext Stealing — 디스크/블록
	 * 암호화 표준 (IEEE P1619). 키 두 개(key1, key2), 블록별 독립 tweak (LBA derived)
	 * 로 병렬화 용이. blob crypto 의 기본. */
};

/**
 * Acceleration operation callback.
 *
 * \param cb_arg Callback argument specified in the spdk_accel_submit* call.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_accel_completion_cb - 단일 accel 작업 완료 시 호출되는 사용자 콜백 시그니처.
 *
 * @cb_arg: spdk_accel_submit_*() 호출 시 사용자가 넘긴 불투명 컨텍스트 포인터 그대로 전달.
 *          상위 레이어는 보통 자신의 I/O 객체(spdk_bdev_io 등)를 넘기고, 콜백에서 그것을
 *          이용해 상위 콜체인의 다음 단계로 진행한다.
 * @status: 0=성공, 음수 errno=실패. SPDK_ACCEL_OPC_COMPARE 의 경우 0 외 값은 mismatch 의미
 *          (errno 와 의미가 다름). compress/decompress 의 경우 성공 시 별도 output_size
 *          포인터에 결과 길이를 기록한다.
 *
 * 비동기 모델의 핵심: submit_*() 는 즉시 0(성공 큐잉) 또는 -errno(즉시 실패)를 반환하고,
 * 실제 데이터 변환의 성공/실패는 모두 이 cb_fn 으로 통보된다. 콜백은 작업이 제출된 동일
 * spdk_thread (= 동일 reactor) 위에서 호출됨이 보장되어 cb_arg 가 가리키는 자료구조에
 * lock 없이 접근 가능하다 (SPDK 의 lockless thread-affinity 모델).
 *
 * 호출 체인:
 *   백엔드 모듈 (HW poll loop / SW 즉시 실행) → accel framework finish_task → [cb_fn]
 */
typedef void (*spdk_accel_completion_cb)(void *cb_arg, int status);

/**
 * Acceleration framework finish callback.
 *
 * \param cb_arg Callback argument.
 */
/*
 * [한국어]
 * spdk_accel_fini_cb - spdk_accel_finish() 의 cleanup 완료 알림 콜백 시그니처.
 *
 * @cb_arg: spdk_accel_finish(cb_fn, cb_arg) 호출 시 사용자가 넘긴 컨텍스트 그대로.
 *
 * SPDK 앱 종료 시퀀스(spdk_app_stop)의 일부로 호출되며, accel 채널 모두 정리·백엔드 모듈
 * unload·키 wipe 가 끝난 후 한 번만 실행된다. 상위 종료 코드는 이 콜백에서 최종 cleanup
 * 후 spdk_app_stop 또는 다음 서브시스템 finish 를 호출한다.
 *
 * 호출 체인:
 *   spdk_subsystem_fini → spdk_accel_finish → 백엔드 모듈 unload → [cb_fn]
 */
typedef void (*spdk_accel_fini_cb)(void *cb_arg);

/**
 * Initialize the acceleration framework.
 *
 * \return 0 on success.
 */
/*
 * [한국어]
 * spdk_accel_initialize - accel framework 초기화 (앱 시작 시 1회).
 *
 * @return: 0 성공, 음수 errno (예: -ENOMEM 메모리 부족, -ENODEV 모듈 등록 실패).
 *
 * 무엇을 하는가: 등록된 모든 백엔드 모듈에 대해 module_init() 을 호출하고, opcode→
 * module 디폴트 매핑 테이블을 priority 순으로 구축하며, 글로벌 iobuf cache (small/large
 * 사이즈 풀)를 spdk_accel_opts 로 설정된 크기로 생성한다. 또한 framework 자신의
 * spdk_io_device 를 등록해 spdk_accel_get_io_channel() 이 동작하도록 한다.
 *
 * 호출 컨텍스트: SPDK 앱 메인 스레드 (subsystem startup 단계). 다른 reactor 가 아직
 * accel 채널을 요청하지 않은 시점.
 *
 * 호출 체인:
 *   spdk_subsystem_init (또는 spdk_app_start) → [spdk_accel_initialize] →
 *     각 모듈의 module_if->module_init()
 */
int spdk_accel_initialize(void);

/**
 * Close the acceleration framework.
 *
 * \param cb_fn Called when the close operation completes.
 * \param cb_arg Argument passed to the callback function.
 */
/*
 * [한국어]
 * spdk_accel_finish - accel framework 종료 (앱 종료 시 1회, 비동기).
 *
 * @cb_fn: 종료 cleanup 완료 시 호출될 콜백.
 * @cb_arg: cb_fn 에 전달될 사용자 컨텍스트.
 *
 * 등록된 모든 백엔드 모듈에 module_fini() 를 호출하고 진행 중인 작업들이 모두 완료될
 * 때까지 대기한 후, 키 테이블·iobuf cache·spdk_io_device 등록을 정리한다. 비동기이므로
 * 즉시 반환하고 모든 cleanup 후 cb_fn 호출. 호출 후 더는 spdk_accel_get_io_channel
 * 이나 submit_* 를 호출해서는 안 된다.
 *
 * 호출 체인:
 *   spdk_subsystem_fini → [spdk_accel_finish] → … → cb_fn
 */
void spdk_accel_finish(spdk_accel_fini_cb cb_fn, void *cb_arg);

/**
 * Get an I/O channel for the acceleration framework.
 *
 * This I/O channel is used to submit requests.
 *
 * \return a pointer to the I/O channel on success, or NULL on failure.
 */
/*
 * [한국어]
 * spdk_accel_get_io_channel - 현재 spdk_thread 의 accel I/O 채널을 획득.
 *
 * @return: 성공 시 채널 포인터 (호출자가 사용 후 spdk_put_io_channel() 로 해제),
 *          NULL = 실패 (메모리 부족, framework 가 init 되지 않음).
 *
 * SPDK 의 spdk_io_channel 모델에 따라 채널은 호출 thread 에 종속된다. 즉 reactor A 에서
 * 받은 채널을 reactor B 에서 사용하면 안 된다 (lockless 무결성 깨짐). 채널 내부에는
 * per-thread accel task pool, sequence pool, accel buffer pool 과 각 백엔드의 sub-channel
 * (예: IDXD WQ handle) 이 들어 있다. 모든 spdk_accel_submit_*/append_* 호출의 첫 인자.
 *
 * 호출 체인:
 *   상위 모듈 init/channel-create → [spdk_accel_get_io_channel] →
 *     spdk_get_io_channel(accel_io_device) → 채널 ctor 가 백엔드별 sub-channel 생성
 */
struct spdk_io_channel *spdk_accel_get_io_channel(void);

/**
 * Create a crypto key with given parameters. Accel module copies content of \b param structure
 *
 * \param param Key parameters
 * \return 0 on success, negated errno on error
 */
/*
 * [한국어]
 * spdk_accel_crypto_key_create - DEK(Data Encryption Key)를 생성·등록.
 *
 * @param: cipher 이름·hex_key·hex_key2·tweak_mode·key_name 을 채운 파라미터 구조체.
 *         framework 가 내용을 deep-copy 하므로 호출 후 즉시 해제 가능.
 * @return: 0 성공, -EINVAL (잘못된 파라미터/cipher 미지원), -EEXIST (key_name 중복),
 *          -ENOMEM, -ENODEV (적합한 crypto 모듈 없음).
 *
 * 동작: hex 문자열을 raw key bytes 로 디코드 → cipher enum 변환 → 길이 검증
 * (SPDK_ACCEL_AES_XTS_128/256_KEY_SIZE) → 적합한 crypto 백엔드 모듈 선택 → 모듈에 키
 * provisioning 위임 (예: dpdk_cryptodev session create) → 전역 키 리스트에 추가.
 * 키 자료는 framework 메모리에서 사용 후 zeroize 된다.
 *
 * 호출 컨텍스트: 일반적으로 RPC 핸들러 (accel_crypto_key_create) 또는 앱 초기화 코드.
 * 메인 spdk_thread 에서 호출되어야 안전.
 *
 * 호출 체인:
 *   RPC handler / app init → [spdk_accel_crypto_key_create] →
 *     crypto module 의 key_create op
 */
int spdk_accel_crypto_key_create(const struct spdk_accel_crypto_key_create_param *param);

/**
 * Destroy a crypto key
 *
 * \param key Key to destroy
 * \return 0 on success, negated errno on error
 */
/*
 * [한국어]
 * spdk_accel_crypto_key_destroy - DEK 해제 및 키 자료 wipe.
 *
 * @key: spdk_accel_crypto_key_create() 가 등록한 핸들. NULL 또는 이미 해제된 키는 -EINVAL.
 * @return: 0 성공, -EINVAL (NULL 또는 미등록 키), -EBUSY (아직 사용 중인 작업 존재).
 *
 * 키 자료를 0 으로 덮어쓰고 백엔드 세션을 해제한 뒤 전역 리스트에서 제거. 진행 중인
 * encrypt/decrypt 작업이 있으면 -EBUSY 가 반환되므로 호출자는 모든 in-flight 작업이
 * 완료될 때까지 기다린 후 호출해야 한다.
 *
 * 호출 체인:
 *   RPC handler / shutdown path → [spdk_accel_crypto_key_destroy]
 */
int spdk_accel_crypto_key_destroy(struct spdk_accel_crypto_key *key);

/**
 * Find a crypto key structure by name
 * \param name Key name
 * \return Crypto key structure or NULL
 */
/*
 * [한국어]
 * spdk_accel_crypto_key_get - 이름으로 등록된 DEK 핸들 조회.
 *
 * @name: spdk_accel_crypto_key_create() 시 사용한 key_name. NULL 이면 NULL 반환.
 * @return: 매칭되는 키 핸들, 또는 NULL (없거나 destroy 됨).
 *
 * RPC/JSON 으로 키를 이름으로만 받아 사용해야 하는 모듈(bdev_crypto, blob crypto)에서
 * spdk_accel_submit_encrypt/decrypt 의 key 인자에 넘길 핸들을 얻는 데 사용. 반환된
 * 핸들은 destroy 전까지 유효하므로 ref count 가 필요한 경우 상위에서 관리한다.
 *
 * 호출 체인:
 *   상위 bdev_crypto/blob → [spdk_accel_crypto_key_get] → spdk_accel_submit_encrypt
 */
struct spdk_accel_crypto_key *spdk_accel_crypto_key_get(const char *name);

/**
 * Submit a copy request.
 *
 * \param ch I/O channel associated with this call.
 * \param dst Destination to copy to.
 * \param src Source to copy from.
 * \param nbytes Length in bytes to copy.
 * \param cb_fn Called when this copy operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_copy - 메모리→메모리 비동기 복사 작업 제출 (SPDK_ACCEL_OPC_COPY).
 *
 * @ch:     호출 thread 의 accel 채널 (spdk_accel_get_io_channel() 결과).
 * @dst:    복사 목적지 가상 주소. DMA 백엔드 사용 시 hugepage/registered memory 여야 함.
 * @src:    복사 원본 가상 주소.
 * @nbytes: 복사할 바이트 수. 0 이면 -EINVAL 반환 가능.
 * @cb_fn:  완료 콜백 — 동일 thread 에서 호출됨.
 * @cb_arg: cb_fn 의 첫 인자로 그대로 전달될 사용자 컨텍스트.
 * @return: 0 = 큐잉 성공 (실제 결과는 cb_fn 으로), 음수 errno = 즉시 실패
 *          (-ENOMEM task pool 고갈, -EINVAL 잘못된 인자).
 *
 * 어느 백엔드가 처리할지는 framework 가 SPDK_ACCEL_OPC_COPY 의 module 매핑에서 선택
 * (기본은 software, IDXD/IOAT/DSA 가 등록되어 있으면 그것). HW 백엔드라면 DMA 디스크립터
 * 를 SQ 에 enqueue 후 즉시 반환, SW 라면 thread 내에서 즉시 memcpy 수행 후 cb_fn 도
 * 동일 호출 stack 에서 호출될 수 있음 (그래서 호출자는 callback reentrancy 에 대비).
 *
 * 호출 체인:
 *   bdev/blob/사용자 코드 → [spdk_accel_submit_copy] → 백엔드 module->submit_tasks
 *     → (HW: poller 가 완료 수확) → cb_fn
 */
int spdk_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
			   spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a dual cast copy request.
 *
 * \param ch I/O channel associated with this call.
 * \param dst1 First destination to copy to (must be 4K aligned).
 * \param dst2 Second destination to copy to (must be 4K aligned).
 * \param src Source to copy from.
 * \param nbytes Length in bytes to copy.
 * \param cb_fn Called when this copy operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_dualcast - 한 source 를 두 dst 로 동시 복사 (SPDK_ACCEL_OPC_DUALCAST).
 *
 * @ch:     accel 채널.
 * @dst1:   첫 번째 목적지 — 4KB(0x1000) 정렬 필수 (IDXD HW 제약).
 * @dst2:   두 번째 목적지 — 동일하게 4KB 정렬 필수.
 * @src:    복사 원본 (정렬 제약은 백엔드별 다름).
 * @nbytes: 복사 바이트 수.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return: 0 큐잉 성공, 음수 errno (-EINVAL 정렬 위반 등).
 *
 * 단일 src 를 두 dst 로 한 번의 DMA pass 로 복사하므로 RAID-1 mirror 의 host write 단계
 * 같은 곳에서 메모리 대역폭 절반 절약. HW 미지원 시 SW 폴백은 두 번 memcpy.
 *
 * 호출 체인:
 *   사용자 → [spdk_accel_submit_dualcast] → IDXD module submit
 */
int spdk_accel_submit_dualcast(struct spdk_io_channel *ch, void *dst1, void *dst2, void *src,
			       uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a compare request.
 *
 * \param ch I/O channel associated with this call.
 * \param src1 First location to perform compare on.
 * \param src2 Second location to perform compare on.
 * \param nbytes Length in bytes to compare.
 * \param cb_fn Called when this compare operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, any other value means there was a miscompare.
 */
/*
 * [한국어]
 * spdk_accel_submit_compare - 두 메모리 영역의 byte-wise 비교 (SPDK_ACCEL_OPC_COMPARE).
 *
 * @ch:     accel 채널.
 * @src1, @src2: 비교 대상 두 버퍼.
 * @nbytes: 비교할 바이트 수.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return: 0 = 큐잉 성공. 다만 doc 의 "any other value means there was a miscompare"
 *          는 cb_fn 의 status 인자에 대한 설명에 가까움 — 실제 함수 반환 자체는
 *          submit 성공/실패만 의미. 콜백 status: 0 = 일치, 그 외 = mismatch
 *          (기존 SPDK 관례; errno 와 다름).
 *
 * NVMe COMPARE 명령(0x05) 의 host-side fallback 경로, fused compare-and-write 의 비교
 * 단계 등에서 사용. HW 가속(IDXD compare) 또는 SW memcmp 폴백.
 *
 * 호출 체인:
 *   bdev compare 콜체인 → [spdk_accel_submit_compare] → 모듈 submit → cb_fn(status=cmp)
 */
int spdk_accel_submit_compare(struct spdk_io_channel *ch, void *src1, void *src2, uint64_t nbytes,
			      spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a fill request.
 *
 * This operation will fill the destination buffer with the specified value.
 *
 * \param ch I/O channel associated with this call.
 * \param dst Destination to fill.
 * \param fill Constant byte to fill to the destination.
 * \param nbytes Length in bytes to fill.
 * \param cb_fn Called when this fill operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_fill - 단일 바이트 패턴으로 dst 채우기 (SPDK_ACCEL_OPC_FILL).
 *
 * @ch:     accel 채널.
 * @dst:    채울 목적지 버퍼.
 * @fill:   채울 1 바이트 패턴 — IDXD 는 이 byte 를 8-byte word 로 broadcast 후 DMA.
 * @nbytes: 채울 바이트 수.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return: 0 큐잉 성공, 음수 errno.
 *
 * NVMe Write Zeroes 명령의 host-side 폴백, blob 페이지 zeroing, 메모리 sanitization 등에
 * 사용. 0 만 채우는 경우 IOAT/IDXD 의 fill_zero 명령으로 최적화될 수 있다.
 *
 * 호출 체인:
 *   bdev write_zeroes / blob → [spdk_accel_submit_fill] → 모듈 submit → cb_fn
 */
int spdk_accel_submit_fill(struct spdk_io_channel *ch, void *dst, uint8_t fill, uint64_t nbytes,
			   spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a CRC-32C calculation request.
 *
 * This operation will calculate the 4 byte CRC32-C for the given data.
 *
 * \param ch I/O channel associated with this call.
 * \param crc_dst Destination to write the CRC-32C to.
 * \param src The source address for the data.
 * \param seed Four byte seed value.
 * \param nbytes Length in bytes.
 * \param cb_fn Called when this CRC-32C operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_crc32c - 단일 contiguous 버퍼에 대한 CRC32C 계산
 *                            (SPDK_ACCEL_OPC_CRC32C).
 *
 * @ch:      accel 채널.
 * @crc_dst: 결과 CRC32C(4 byte) 가 기록될 위치. 작업 완료(cb_fn 호출) 시점에 유효한 값.
 * @src:     CRC 계산 대상 데이터 시작 주소.
 * @seed:    CRC 초기값(initial seed). chained/iSCSI 등에서 이전 청크의 결과를 이어
 *           계산할 때 비-0 값 사용. 일반적으로 0 또는 NVMe DDGST 의 init 값 0xFFFFFFFF.
 * @nbytes:  계산 대상 길이.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:  0 큐잉 성공, 음수 errno.
 *
 * Castagnoli 다항식(0x1EDC6F41) 사용 — NVMe HDGST/DDGST, iSCSI Header/Data digest,
 * T10 DIF Type 1 GUARD 등 SPDK 가 처리하는 거의 모든 무결성 체크와 호환. SSE4.2 의
 * CRC32 명령, IDXD HW, IAA 모두 지원하므로 framework 가 자동 선택.
 *
 * 호출 체인:
 *   nvme_qpair completion handler / iSCSI digest path →
 *     [spdk_accel_submit_crc32c] → 백엔드 submit → cb_fn
 */
int spdk_accel_submit_crc32c(struct spdk_io_channel *ch, uint32_t *crc_dst, void *src,
			     uint32_t seed,
			     uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a chained CRC-32C calculation request.
 *
 * This operation will calculate the 4 byte CRC32-C for the given data.
 *
 * \param ch I/O channel associated with this call.
 * \param crc_dst Destination to write the CRC-32C to.
 * \param iovs The io vector array which stores the src data and len.
 * \param iovcnt The size of the iov.
 * \param seed Four byte seed value.
 * \param cb_fn Called when this CRC-32C operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_crc32cv - 산재된 iovec 들에 대해 chained CRC32C 계산
 *                             (vector 형식, SPDK_ACCEL_OPC_CRC32C).
 *
 * @ch:      accel 채널.
 * @crc_dst: 4-byte CRC32C 결과 기록 위치.
 * @iovs:    데이터 벡터 배열 (각 iov_base/iov_len 의 시퀀스를 마치 contiguous
 *           스트림인 것처럼 처리).
 * @iovcnt:  iovs 배열 길이.
 * @seed:    초기 CRC 값.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:  0 큐잉 성공, 음수 errno.
 *
 * iSCSI 의 PDU(여러 segment 로 분리된 BHS/AHS/Data) digest 계산이나 NVMe-oF TCP 의
 * fragmented payload digest 처럼 host buffer 가 SGL 형태로 들어올 때 사용. v 접미사는
 * "vector" 의미.
 *
 * 호출 체인:
 *   iSCSI / NVMe-oF TCP digest path → [spdk_accel_submit_crc32cv]
 */
int spdk_accel_submit_crc32cv(struct spdk_io_channel *ch, uint32_t *crc_dst, struct iovec *iovs,
			      uint32_t iovcnt, uint32_t seed, spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a copy with CRC-32C calculation request.
 *
 * This operation will copy data and calculate the 4 byte CRC32-C for the given data.
 *
 * \param ch I/O channel associated with this call.
 * \param dst Destination to write the data to.
 * \param src The source address for the data.
 * \param crc_dst Destination to write the CRC-32C to.
 * \param seed Four byte seed value.
 * \param nbytes Length in bytes.
 * \param cb_fn Called when this CRC-32C operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_copy_crc32c - 복사 + CRC32C 계산을 한 번에 (SPDK_ACCEL_OPC_COPY_CRC32C).
 *
 * @ch:      accel 채널.
 * @dst:     복사 목적지.
 * @src:     복사 원본 (CRC 계산도 src 데이터에 대해 수행).
 * @crc_dst: 4-byte CRC32C 결과 기록 위치.
 * @seed:    초기 CRC 값.
 * @nbytes:  복사 및 CRC 계산할 바이트 수.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:  0 큐잉 성공, 음수 errno.
 *
 * 메모리를 한 번만 순회하며 두 작업을 동시에 수행 — NVMe write 경로의 host buffer →
 * staging 복사 + DDGST 계산이나 NVMe-oF TCP send path 의 data segment 복사 + digest
 * 같은 핫패스에서 캐시 효율을 두 배로.
 *
 * 호출 체인:
 *   NVMe-oF transport / iSCSI write → [spdk_accel_submit_copy_crc32c]
 */
int spdk_accel_submit_copy_crc32c(struct spdk_io_channel *ch, void *dst, void *src,
				  uint32_t *crc_dst, uint32_t seed, uint64_t nbytes,
				  spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a chained copy + CRC-32C calculation request.
 *
 * This operation will calculate the 4 byte CRC32-C for the given data.
 *
 * \param ch I/O channel associated with this call.
 * \param dst Destination to write the data to.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param iovcnt The size of the io vectors.
 * \param crc_dst Destination to write the CRC-32C to.
 * \param seed Four byte seed value.
 * \param cb_fn Called when this CRC-32C operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_copy_crc32cv - vector copy + chained CRC32C
 *                                  (SPDK_ACCEL_OPC_COPY_CRC32C, scatter src).
 *
 * @ch:      accel 채널.
 * @dst:     contiguous 복사 목적지 (gather 된 결과가 들어감).
 * @src_iovs / @iovcnt: scatter 형태의 src iov 배열과 길이 — 모두 합쳐 dst 로 복사
 *           하면서 동일한 데이터에 대해 CRC32C 계산.
 * @crc_dst: 4-byte CRC32C 결과 기록 위치.
 * @seed:    초기 CRC 값.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:  0 큐잉 성공, 음수 errno.
 *
 * iSCSI/NVMe-oF host buffer 가 여러 segment 로 흩어져 들어왔을 때, 이를 디스크 I/O 용
 * 단일 contiguous 영역으로 모으면서 동시에 digest 도 계산 — 두 번의 메모리 패스를 한
 * 번으로 합치는 최적화.
 */
int spdk_accel_submit_copy_crc32cv(struct spdk_io_channel *ch, void *dst, struct iovec *src_iovs,
				   uint32_t iovcnt, uint32_t *crc_dst, uint32_t seed,
				   spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory compress request using the deflate algorithm.
 *
 * This function will build the compress descriptor and submit it.
 *
 * \param ch I/O channel associated with this call
 * \param dst Destination to write the data to.
 * \param nbytes Length in bytes.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the src io vectors.
 * \param output_size The size of the compressed data (may be NULL if not desired)
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_compress - DEFLATE 압축 (SPDK_ACCEL_OPC_COMPRESS, 알고리즘 고정 변형).
 *
 * @ch:           accel 채널.
 * @dst:          압축 결과를 받을 contiguous 버퍼. nbytes 가 dst 의 capacity (출력 최대치).
 * @nbytes:       dst 버퍼 크기 — 압축 결과가 이 크기를 넘으면 실패(-ENOSPC).
 * @src_iovs / @src_iovcnt: 압축 원본 iovec 배열.
 * @output_size:  실제 압축 결과 길이가 기록될 위치 (NULL 가능). 호출자가 이후 디스크
 *                기록·전송 시 사용.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:       0 큐잉 성공, 음수 errno (-ENODEV 압축 모듈 없음).
 *
 * 알고리즘은 DEFLATE 로 고정. blob compress, bdev_compress 모듈에서 사용. IAA 가속이
 * 등록된 시스템에서는 자동으로 IAA 사용, 아니면 ISA-L SW 폴백.
 */
int spdk_accel_submit_compress(struct spdk_io_channel *ch, void *dst,
			       uint64_t nbytes, struct iovec *src_iovs,
			       size_t src_iovcnt, uint32_t *output_size,
			       spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory decompress request using the deflate algorithm.
 *
 * This function will build the decompress descriptor and submit it.
 *
 * \param ch I/O channel associated with this call
 * \param dst_iovs The io vector array which stores the dst data and len.
 * \param dst_iovcnt The size of the dst io vectors.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the src io vectors.
 * \param output_size The size of the compressed data (may be NULL if not desired)
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_decompress - DEFLATE 압축 해제 (SPDK_ACCEL_OPC_DECOMPRESS, 고정 알고리즘).
 *
 * @ch:           accel 채널.
 * @dst_iovs / @dst_iovcnt: 해제 결과를 받을 iovec 배열 — 충분한 capacity 가 있어야 함.
 * @src_iovs / @src_iovcnt: 압축된 원본 iovec.
 * @output_size:  실제 해제 결과 길이 (NULL 가능).
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:       0 큐잉 성공, 음수 errno.
 *
 * dst 가 부족하면 cb_fn 의 status 가 -ENOSPC 또는 백엔드별 에러로 통보. dst iovec 가
 * scatter 형태이므로 사용자가 미리 안전 마진(보통 raw size 또는 1.1× 압축 크기)으로
 * 할당해야 한다.
 */
int spdk_accel_submit_decompress(struct spdk_io_channel *ch, struct iovec *dst_iovs,
				 size_t dst_iovcnt, struct iovec *src_iovs,
				 size_t src_iovcnt, uint32_t *output_size,
				 spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory compress request using the specified algorithm.
 *
 * This function will build the compress descriptor and submit it.
 *
 * \param ch I/O channel associated with this call
 * \param dst Destination to write the data to.
 * \param nbytes Length in bytes.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the src io vectors.
 * \param comp_algo The compression algorithm, enum spdk_accel_comp_algo value.
 * \param comp_level The compression algorithm level.
 * \param output_size The size of the compressed data (may be NULL if not desired)
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_compress_ext - 알고리즘/레벨 지정 압축 (확장 변형).
 *
 * @ch:           accel 채널.
 * @dst:          contiguous 출력 버퍼.
 * @nbytes:       dst 크기 (capacity).
 * @src_iovs / @src_iovcnt: 입력 iovec.
 * @comp_algo:    spdk_accel_comp_algo (DEFLATE / LZ4) 중 하나 — 사용자가 명시 선택.
 * @comp_level:   알고리즘별 압축 레벨 (DEFLATE: 0~9, LZ4 는 별도 범위).
 *                spdk_accel_get_compress_level_range() 로 사전 조회 가능.
 * @output_size:  실제 출력 길이.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:       0 큐잉 성공, 음수 errno (-EINVAL 미지원 algo/level, -ENODEV 모듈 없음).
 *
 * 기본 spdk_accel_submit_compress() 가 DEFLATE 만 처리하는 것과 달리, 사용자가 알고리즘
 * 과 레벨을 모두 제어해야 할 때 (예: 워크로드 별 압축률/속도 튜닝) 사용.
 */
int spdk_accel_submit_compress_ext(struct spdk_io_channel *ch, void *dst, uint64_t nbytes,
				   struct iovec *src_iovs, size_t src_iovcnt,
				   enum spdk_accel_comp_algo comp_algo, uint32_t comp_level,
				   uint32_t *output_size, spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory decompress request using the specified algorithm.
 *
 * This function will build the decompress descriptor and submit it.
 *
 * \param ch I/O channel associated with this call
 * \param dst_iovs The io vector array which stores the dst data and len.
 * \param dst_iovcnt The size of the dst io vectors.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the src io vectors.
 * \param decomp_algo The decompression algorithm, enum spdk_accel_comp_algo value.
 * \param output_size The size of the compressed data (may be NULL if not desired)
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_decompress_ext - 알고리즘 지정 압축 해제 (확장 변형).
 *
 * @ch:           accel 채널.
 * @dst_iovs / @dst_iovcnt: 해제 출력 scatter iovec.
 * @src_iovs / @src_iovcnt: 압축 원본 iovec.
 * @decomp_algo:  사용자 명시 알고리즘 (DEFLATE / LZ4 등).
 * @output_size:  실제 해제 길이.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:       0 큐잉 성공, 음수 errno.
 *
 * 동일한 데이터를 여러 압축 알고리즘으로 받아 해제해야 하는 경우(예: 마이그레이션,
 * 백워드 호환성) 사용. _ext 가 아닌 변형은 DEFLATE 고정.
 */
int spdk_accel_submit_decompress_ext(struct spdk_io_channel *ch, struct iovec *dst_iovs,
				     size_t dst_iovcnt, struct iovec *src_iovs, size_t src_iovcnt,
				     enum spdk_accel_comp_algo decomp_algo, uint32_t *output_size,
				     spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Gets the level range of the specified algorithm.
 *
 * \param comp_algo The compression algorithm.
 * \param min_level The lowest level supported by the compression algorithm.
 * \param max_level The highest level supported by the compression algorithm.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_get_compress_level_range - 알고리즘별 지원 레벨 범위 조회.
 *
 * @comp_algo:  조회할 알고리즘.
 * @min_level:  out — 지원되는 최소 레벨 (보통 0 = 무압축/저장).
 * @max_level:  out — 지원되는 최대 레벨 (DEFLATE: 9, LZ4: 16 등 백엔드별 상이).
 * @return:     0 성공, -EINVAL 알고리즘 미지원, -ENODEV 모듈 미등록.
 *
 * 동기(synchronous) 함수 — submit 전에 사용자가 spdk_accel_submit_compress_ext() 의
 * comp_level 인자를 안전한 범위에서 선택하도록 도움. 실제 가능 레벨은 어느 백엔드가
 * SPDK_ACCEL_OPC_COMPRESS 에 매핑되어 있느냐에 따라 달라진다.
 */
int spdk_accel_get_compress_level_range(enum spdk_accel_comp_algo comp_algo,
					uint32_t *min_level, uint32_t *max_level);

/**
 * Submit an xor request.
 *
 * \param ch I/O channel associated with this call.
 * \param dst Destination to write the data to.
 * \param sources Array of source buffers.
 * \param nsrcs Number of source buffers in the array.
 * \param nbytes Length in bytes.
 * \param cb_fn Called when this copy operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_xor - 다중 source 의 byte-wise XOR (SPDK_ACCEL_OPC_XOR).
 *
 * @ch:      accel 채널.
 * @dst:     XOR 결과 기록 위치 — 모두 같은 nbytes 길이.
 * @sources: source 버퍼들의 포인터 배열 (예: void *[3] = {a, b, c}).
 * @nsrcs:   sources 배열 길이 — 보통 RAID-5 의 (스트라이프 멤버 수 - 1).
 * @nbytes:  각 source 와 dst 의 길이.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:  0 큐잉 성공, 음수 errno.
 *
 * 결과: dst[i] = sources[0][i] ^ sources[1][i] ^ … ^ sources[nsrcs-1][i].
 * RAID-5 parity 생성/리빌드의 핵심 op. ISA-L XOR + IDXD HW XOR 가속 가능.
 *
 * 호출 체인:
 *   bdev_raid (raid5f) parity calc → [spdk_accel_submit_xor]
 */
int spdk_accel_submit_xor(struct spdk_io_channel *ch, void *dst, void **sources, uint32_t nsrcs,
			  uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a data encryption request.
 *
 * This function will build the encryption request and submit it. \b nbytes must be multiple of \b
 * block_size.  \b iv is used to encrypt the first logical block of size \b block_size. If \b
 * src_iovs describes more than one logical block then \b iv will be incremented for each next
 * logical block.  Data Encryption Key identifier should be created before calling this function
 * using methods specific to the accel module being used.
 *
 * \param ch I/O channel associated with this call
 * \param key Data Encryption Key identifier
 * \param dst_iovs The io vector array which stores the dst data and len.
 * \param dst_iovcnt The size of the destination io vectors.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the source io vectors.
 * \param iv Initialization vector (tweak) used for encryption
 * \param block_size Logical block size, if src contains more than 1 logical block, subsequent
 *        logical blocks will be encrypted with incremented \b iv
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in the completion
 *        callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_encrypt - 데이터 암호화 (SPDK_ACCEL_OPC_ENCRYPT, AES-XTS/CBC).
 *
 * @ch:           accel 채널.
 * @key:          spdk_accel_crypto_key_create()/get() 으로 얻은 DEK 핸들.
 * @dst_iovs / @dst_iovcnt: 암호문 출력 iovec.
 * @src_iovs / @src_iovcnt: 평문 입력 iovec — 총 길이는 block_size 의 배수여야 함.
 * @iv:           첫 logical block 의 tweak/IV (AES-XTS) 또는 CBC IV. 후속 블록은
 *                iv+1, iv+2 ... 로 자동 증가하여 LBA 기반 암호화에 부합.
 * @block_size:   logical block 크기 (예: 4096) — XTS 의 sector unit, CBC 의 청크 단위.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:       0 큐잉 성공, 음수 errno (-EINVAL key invalid, -EIO HW 실패 등).
 *
 * 한 번의 호출로 src 전체를 block_size 단위로 분할해 각 블록에 (iv+i) tweak 적용.
 * blob crypto 와 bdev_crypto 의 write 경로에서 핵심 호출. AES-XTS-128 키는
 * SPDK_ACCEL_AES_XTS_128_KEY_SIZE × 2, AES-XTS-256 은 ×2 길이의 키 자료를 필요로 한다.
 *
 * 호출 체인:
 *   bdev_crypto write → [spdk_accel_submit_encrypt] →
 *     dpdk_cryptodev/mlx5/SW backend → cb_fn
 */
int spdk_accel_submit_encrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			      struct iovec *dst_iovs, uint32_t dst_iovcnt,
			      struct iovec *src_iovs, uint32_t src_iovcnt,
			      uint64_t iv, uint32_t block_size,
			      spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a data decryption request.
 *
 * This function will build the decryption request and submit it. \b nbytes must be multiple of \b
 * block_size.  \b iv is used to decrypt the first logical block of size \b block_size. If \b
 * src_iovs describes more than one logical block then \b iv will be incremented for each next
 * logical block.  Data Encryption Key identifier should be created before calling this function
 * using methods specific to the accel module being used.
 *
 * \param ch I/O channel associated with this call
 * \param key Data Encryption Key identifier
 * \param dst_iovs The io vector array which stores the dst data and len.
 * \param dst_iovcnt The size of the destination io vectors.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the source io vectors.
 * \param iv Initialization vector (tweak) used for decryption. Should be the same as \b iv used for
 *        encryption of a data block
 * \param block_size Logical block size, if src contains more than 1 logical block, subsequent
 *        logical blocks will be decrypted with incremented \b iv
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in the completion
 *        callback.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_decrypt - 데이터 복호화 (SPDK_ACCEL_OPC_DECRYPT).
 *
 * @ch / @key / @dst_iovs / @dst_iovcnt / @src_iovs / @src_iovcnt: encrypt 와 동일 의미
 *      (단 src=암호문, dst=평문).
 * @iv:           암호화 시 사용했던 동일 iv. 후속 블록 iv+1, iv+2 ... 자동 증가.
 *                iv 가 다르면 복호화 실패 (XTS 의 경우 garbage 출력).
 * @block_size:   암호화 시 사용했던 동일 block_size — 전체 길이는 이 배수.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:       0 큐잉 성공, 음수 errno.
 *
 * blob crypto / bdev_crypto 의 read 경로에서 핵심 호출. encrypt 호출 시 사용한 키와
 * iv 가 정확히 일치해야 함을 호출자가 보장해야 한다.
 *
 * 호출 체인:
 *   bdev_crypto read completion → [spdk_accel_submit_decrypt] → backend → cb_fn
 */
int spdk_accel_submit_decrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			      struct iovec *dst_iovs, uint32_t dst_iovcnt,
			      struct iovec *src_iovs, uint32_t src_iovcnt,
			      uint64_t iv, uint32_t block_size,
			      spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Data Integrity Field (DIF) verify request.
 *
 * This operation computes the DIF on the data and compares it against the DIF contained
 * in the metadata.
 *
 * \param ch I/O channel associated with this call.
 * \param iovs The io vector array. The total allocated memory size needs to be at least:
 *             num_blocks * block_size (including metadata)
 * \param iovcnt The size of the io vectors array.
 * \param num_blocks Number of data blocks to check.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to check
 *            Note: the user must ensure the validity of this pointer throughout the entire operation
 *            because it is not validated along the processing path.
 * \param err DIF error detailed information.
 *            Note: the user must ensure the validity of this pointer throughout the entire operation
 *            because it is not validated along the processing path.
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_dif_verify - In-place DIF 검증 (SPDK_ACCEL_OPC_DIF_VERIFY).
 *
 * @ch:         accel 채널.
 * @iovs:       data + metadata 가 인터리브된 iovec — 총 크기 ≥ num_blocks × block_size
 *              (block_size 는 ctx 에서 정의된 data+md 합).
 * @iovcnt:     iovs 길이.
 * @num_blocks: 검증할 데이터 블록 수.
 * @ctx:        spdk_dif_ctx — PI format(Type 1/2/3, 16-byte vs 8-byte), GUARD/APPTAG/
 *              REFTAG 동작 모드, 초기 reference tag, app tag 등 모든 DIF 설정. 포인터
 *              유효성은 작업 완료까지 호출자가 보장해야 함 (framework 가 deep-copy 안 함).
 * @err:        검증 실패 시 어느 블록의 어느 필드(GUARD/APPTAG/REFTAG)가 mismatch 인지
 *              기록되는 출력 컨텍스트. 작업 동안 유효해야 함.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:     0 큐잉 성공, 음수 errno.
 *
 * NVMe disk 가 PI 와 함께 데이터를 보내는 경우 host 에서 PI 재계산 후 비교. 실패 시
 * cb_fn 의 status 가 비-0, err 에 상세 위치/필드 기록 — 상위(bdev/nvme)가 적절한
 * 에러를 사용자에게 전달.
 *
 * 호출 체인:
 *   nvme read completion (PI on, host verify) → [spdk_accel_submit_dif_verify] → cb_fn
 */
int spdk_accel_submit_dif_verify(struct spdk_io_channel *ch,
				 struct iovec *iovs, size_t iovcnt, uint32_t num_blocks,
				 const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				 spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Data Integrity Field (DIF) copy and verify request.
 *
 * This operation copies memory from the source to the destination address and removes
 * the DIF data with its verification according to the flags provided in the context.
 *
 * \param ch I/O channel associated with this call.
 * \param dst_iovs The destination I/O vector array. The total allocated memory size needs
 *		  to be at least: num_blocks * data_block_size.
 * \param dst_iovcnt The size of the destination I/O vectors array.
 * \param src_iovs The source I/O vector array. The total allocated memory size needs
 *		  to be at least: num_blocks * block_size (including metadata)
 * \param src_iovcnt The size of the source I/O vectors array.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert.
 * \param err DIF error detailed information.
 *            Note: the user must ensure the validity of this pointer throughout the entire operation
 *            because it is not validated along the processing path.
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_dif_verify_copy - DIF 검증 + DIF strip 복사
 *                                     (SPDK_ACCEL_OPC_DIF_VERIFY_COPY).
 *
 * @ch:           accel 채널.
 * @dst_iovs / @dst_iovcnt: data-only 출력 (PI 제거됨, 크기 ≥ num_blocks × data_block_size).
 * @src_iovs / @src_iovcnt: data+PI 입력 (크기 ≥ num_blocks × block_size).
 * @num_blocks:   처리할 블록 수.
 * @ctx:          DIF 설정 — verify 와 동일하게 GUARD/APPTAG/REFTAG 검증 모드 명시.
 * @err:          실패 시 상세 정보 출력.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:       0 큐잉 성공, 음수 errno.
 *
 * NVMe read 경로의 핵심 — disk 에서 PI-on 데이터를 받았을 때, host 가 보유할 raw data 만
 * 추출하면서 동시에 무결성 검증을 한 패스로 처리. write_verify_copy 와 함께 PI 변환의
 * 양방향 핵심.
 *
 * 호출 체인:
 *   nvme read with PI strip → [spdk_accel_submit_dif_verify_copy] → cb_fn
 */
int spdk_accel_submit_dif_verify_copy(struct spdk_io_channel *ch,
				      struct iovec *dst_iovs, size_t dst_iovcnt,
				      struct iovec *src_iovs, size_t src_iovcnt, uint32_t num_blocks,
				      const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				      spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Data Integrity Field (DIF) generate request.
 *
 * This operation compute the DIF on the source data and inserting the DIF in place into
 * the source data.
 *
 * \param ch I/O channel associated with this call.
 * \param iovs The io vector array. The total allocated memory size needs to be at least:
 *             num_blocks * block_size (including metadata)
 * \param iovcnt The size of the io vectors array.
 * \param num_blocks Number of data blocks.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_dif_generate - In-place DIF 생성 (SPDK_ACCEL_OPC_DIF_GENERATE).
 *
 * @ch:         accel 채널.
 * @iovs:       data + metadata 영역이 모두 포함된 iovec (총 ≥ num_blocks × block_size).
 *              호출 전에는 metadata 영역이 미사용/임의 값, 호출 후 PI(GUARD/APPTAG/
 *              REFTAG)로 채워짐.
 * @iovcnt:     iovs 길이.
 * @num_blocks: 처리할 데이터 블록 수.
 * @ctx:        spdk_dif_ctx — 어떤 PI format/모드로 생성할지 정의. 초기 ref_tag 는
 *              ctx 에서 시작해 블록마다 자동 증가.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:     0 큐잉 성공, 음수 errno.
 *
 * 이미 metadata 영역을 비워둔 buffer 를 갖고 있을 때 사용 — 별도 src→dst 복사 없이
 * 그 자리에 PI 만 채운다. nvme write 직전에 buffer 가 이미 LBA 단위로 인터리브되어
 * 있다면 이 op 한 번으로 충분.
 */
int spdk_accel_submit_dif_generate(struct spdk_io_channel *ch,
				   struct iovec *iovs, size_t iovcnt, uint32_t num_blocks,
				   const struct spdk_dif_ctx *ctx,
				   spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Data Integrity Field (DIF) copy and generate request.
 *
 * This operation copies memory from the source to the destination address,
 * while computing the DIF on the source data and inserting the DIF into
 * the output data.
 *
 * \param ch I/O channel associated with this call.
 * \param dst_iovs The destination io vector array. The total allocated memory size needs
 *		  to be at least: num_blocks * block_size (provided to spdk_dif_ctx_init())
 * \param dst_iovcnt The size of the destination io vectors array.
 * \param src_iovs The source io vector array. The total allocated memory size needs
 *		  to be at least: num_blocks * data_block_size.
 * \param src_iovcnt The size of the source io vectors array.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_dif_generate_copy - data-only src → data+DIF dst 복사·생성
 *                                       (SPDK_ACCEL_OPC_DIF_GENERATE_COPY).
 *
 * @ch:           accel 채널.
 * @dst_iovs / @dst_iovcnt: data + PI 가 인터리브될 출력 (≥ num_blocks × block_size).
 * @src_iovs / @src_iovcnt: data-only 입력 (≥ num_blocks × data_block_size).
 * @num_blocks:   처리 블록 수.
 * @ctx:          DIF 설정 — PI format, 초기 ref_tag 등.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:       0 큐잉 성공, 음수 errno.
 *
 * NVMe write 경로의 핵심 — host 가 raw data 만 보유한 상태에서 디스크 전송 직전에 PI 를
 * 부착하며 staging 버퍼로 복사. 한 번의 메모리 패스로 변환 + DMA-friendly layout 으로
 * 재배치. host buffer 가 read-only 정책이거나 in-place 가 어려운 경우에 적합.
 */
int spdk_accel_submit_dif_generate_copy(struct spdk_io_channel *ch, struct iovec *dst_iovs,
					size_t dst_iovcnt, struct iovec *src_iovs, size_t src_iovcnt,
					uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
					spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Data Integrity Extension (DIX) generate operation.
 *
 * This operation computes Protection Information (DIX) and inserts it into metadata buffer.
 *
 * \param ch I/O channel associated with this call.
 * \param iovs The source io vector array. The total allocated memory size needs to be at least:
 *	       num_blocks * block_size_no_md
 * \param iovcnt The size of the source io vectors array.
 * \param md_iov The metadata vector array. The total allocated memory size needs to be at least:
 *		  num_blocks * md_size (8B or 16B, depending on the PI format)
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIX context. Contains the DIX configuration values, including the reference
 *	      Application Tag value and initial value of the Reference Tag to insert
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_dix_generate - DIX 생성 (data 와 metadata 분리, SPDK_ACCEL_OPC_DIX_GENERATE).
 *
 * @ch:         accel 채널.
 * @iovs:       data-only 입력 (≥ num_blocks × block_size_no_md).
 * @iovcnt:     iovs 길이.
 * @md_iov:     metadata 출력 — 데이터와 분리된 별도 buffer (≥ num_blocks × md_size).
 *              md_size 는 PI format 에 따라 8 byte (Type 1, 16-bit guard) 또는
 *              16 byte (Type 1 with 32-bit guard 등 확장).
 * @num_blocks: 처리 블록 수.
 * @ctx:        DIX context — DIF 와 동일 구조체 재사용, 다만 host-managed metadata 모드.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:     0 큐잉 성공, 음수 errno.
 *
 * DIF 와 차이: DIF 는 data 와 PI 가 인터리브, DIX 는 data 와 PI 가 별도 buffer.
 * NVMe Spec 의 PRACT=1 + MD-separate 모드, host 가 metadata 를 별도로 관리할 때 사용.
 */
int spdk_accel_submit_dix_generate(struct spdk_io_channel *ch, struct iovec *iovs,
				   size_t iovcnt, struct iovec *md_iov, uint32_t num_blocks,
				   const struct spdk_dif_ctx *ctx, spdk_accel_completion_cb cb_fn,
				   void *cb_arg);

/**
 * Submit a Data Integrity Extension (DIX) verify request.
 *
 * This operation computes the Protection Information (DIX) on the data and compares it against
 * the DIX contained in the metadata.
 *
 * \param ch I/O channel associated with this call.
 * \param iovs The io vector array. The total allocated memory size needs to be at least:
 *             num_blocks * block_size
 * \param iovcnt The size of the io vectors array.
 * \param md_iov The metadata vector array. The total allocated memory size needs to be at least:
 *		 num_blocks * md_size (8B or 16B, depending on the PI format)
 * \param num_blocks Number of data blocks to check.
 * \param ctx DIX context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to check
 *            Note: the user must ensure the validity of this pointer throughout the entire
 *	      operation because it is not validated along the processing path.
 * \param err DIX error detailed information.
 *            Note: the user must ensure the validity of this pointer throughout the entire
 *	      operation because it is not validated along the processing path.
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_submit_dix_verify - DIX 검증 (data + 별도 md, SPDK_ACCEL_OPC_DIX_VERIFY).
 *
 * @ch:         accel 채널.
 * @iovs:       data-only 입력 (≥ num_blocks × block_size_no_md). 주의: 함수 doc 의
 *              "block_size" 표기는 metadata 미포함의 의미로 읽어야 함 (다른 함수의
 *              block_size 는 +md 인 것과 차이).
 * @iovcnt:     iovs 길이.
 * @md_iov:     검증할 metadata buffer (≥ num_blocks × md_size).
 * @num_blocks: 처리 블록 수.
 * @ctx:        DIX context — 검증 모드 정의.
 * @err:        실패 시 상세 에러 출력.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return:     0 큐잉 성공, 음수 errno. cb_fn status 가 비-0 이면 검증 실패.
 *
 * DIF_VERIFY 와 같지만 metadata 가 별도 buffer 인 host-managed 모드. NVMe disk 가
 * data/metadata 를 분리해 보낸 후 host 에서 무결성 검증할 때 사용.
 */
int spdk_accel_submit_dix_verify(struct spdk_io_channel *ch, struct iovec *iovs,
				 size_t iovcnt,  struct iovec *md_iov, uint32_t num_blocks,
				 const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				 spdk_accel_completion_cb cb_fn, void *cb_arg);

/** Object grouping multiple accel operations to be executed at the same point in time */
struct spdk_accel_sequence;
/* [한국어] 여러 accel 작업을 순서대로 묶어 한꺼번에 실행하는 컨테이너의 전방선언.
 * 실제 정의는 lib/accel/accel.c 에 있으며, 사용자는 불투명 포인터로만 다룬다.
 *
 * Zero-copy 메커니즘의 핵심: spdk_accel_append_*() 는 작업을 큐잉만 하고 즉시 반환,
 * 실제 디스패치는 spdk_accel_sequence_finish() 시점에 일괄 처리된다. 이때 framework 는
 * 인접한 op 들이 데이터를 직접 주고받을 수 있는지 분석해 중간 버퍼를 생략한다.
 *   예: append_decompress(A→B) + append_copy(B→C)  →  decompress 결과를 직접 C 에
 *       기록 (B 생략)
 *   예: append_copy(A→B) + append_encrypt(B→C)     →  encrypt 가 직접 A 에서 읽기
 * append 시 NULL 을 넘기면 새 sequence 가 생성되고 *seq 에 핸들이 기록된다. sequence 는
 * 단일 thread 소유 — 다른 thread 에서 append/finish 호출 금지.
 *
 * lib/bdev 는 이 메커니즘을 자동 활용하여 read+CRC, write+DIF, copy+encrypt 같은
 * 복합 패턴을 한 sequence 로 묶어 디스패치한다 (spdk_bdev_io 의 accel_sequence 멤버). */

/**
 * Completion callback of a single operation within a sequence.  After it's executed, the sequence
 * object might be freed, so users should not touch it.
 */
/*
 * [한국어]
 * spdk_accel_step_cb - sequence 내 단일 step 완료 콜백.
 *
 * @cb_arg: append_*() 호출 시 사용자가 넘긴 컨텍스트 그대로.
 *
 * 주의: sequence_finish 의 최종 cb 와 다름. step_cb 는 그 op 한 step 완료 직후 호출되며,
 * 이 시점 이후 sequence 객체는 이미 free 되었거나 다음 step 으로 진행 중일 수 있어
 * 안전하게 만질 수 없다 (sequence 핸들 dereference 금지). cb_arg 가 가리키는 사용자
 * 컨텍스트만 사용해야 함. step 별 진행 알림이 필요 없으면 NULL 도 가능 (대부분 NULL).
 */
typedef void (*spdk_accel_step_cb)(void *cb_arg);

/**
 * Append a copy operation to a sequence.  Copy operation in a sequence is special, as it is not
 * guaranteed that the data will be actually copied.  If it's possible, it will only change
 * source / destination buffers of some of the operations in a sequence.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param dst_iovs Destination I/O vector array.
 * \param dst_iovcnt Size of the `dst_iovs` array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs Source I/O vector array.
 * \param src_iovcnt Size of the `src_iovs` array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_accel_append_copy - sequence 에 copy 작업 큐잉.
 *
 * @seq:         in/out — sequence 핸들. NULL 또는 *seq==NULL 이면 새 sequence 생성하고
 *               *seq 에 기록. 두 번째 이후 호출은 동일 *seq 를 재사용.
 * @ch:          accel 채널 (sequence 의 첫 append 시 thread affinity 결정).
 * @dst_iovs / @dst_iovcnt: 복사 목적지 iov 배열.
 * @dst_domain / @dst_domain_ctx: dst 가 속한 memory domain (host RAM 이면 NULL,
 *               GPU/RDMA 메모리이면 해당 domain). framework 가 접근 가능한지 검증.
 * @src_iovs / @src_iovcnt / @src_domain / @src_domain_ctx: src 측 동일.
 * @cb_fn / @cb_arg: step 단위 콜백 / 컨텍스트 (보통 NULL).
 * @return:      0 = step 큐잉 성공, 음수 errno (-ENOMEM step 풀 부족, -EINVAL).
 *
 * 중요: 이 함수가 반환되어도 실제 복사는 발생하지 않음. sequence_finish 시점에
 * framework 가 "이 copy 가 정말 필요한지" 판단하여 인접 op 와 합쳐 생략 가능
 * (zero-copy). 그래서 step_cb 가 호출될 때 데이터가 아직 dst 에 없을 수도 있고
 * src 에서 직접 다음 op 가 읽었을 수도 있다.
 */
int spdk_accel_append_copy(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			   struct iovec *dst_iovs, uint32_t dst_iovcnt,
			   struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			   struct iovec *src_iovs, uint32_t src_iovcnt,
			   struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			   spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a fill operation to a sequence.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param buf Data buffer.
 * \param len Length of the data buffer.
 * \param domain Memory domain to which the data buffer belongs.
 * \param domain_ctx Buffer domain context.
 * \param pattern Pattern to fill the buffer with.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_accel_append_fill - sequence 에 fill 작업 큐잉.
 *
 * @seq:         in/out sequence 핸들.
 * @ch:          accel 채널.
 * @buf:         채울 buffer (contiguous, append 계열은 단일 buf 만 받음 — submit_fill
 *               과 동일 형태).
 * @len:         buf 크기.
 * @domain / @domain_ctx: buf 의 memory domain.
 * @pattern:     채울 1-byte 값.
 * @cb_fn / @cb_arg: step 콜백 / 컨텍스트.
 * @return:      0 큐잉 성공, 음수 errno.
 */
int spdk_accel_append_fill(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			   void *buf, uint64_t len,
			   struct spdk_memory_domain *domain, void *domain_ctx, uint8_t pattern,
			   spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a decompression operation using the specified algorithm to a sequence.
 *
 * \param pseq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param dst_iovs Destination I/O vector array.
 * \param dst_iovcnt Size of the `dst_iovs` array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs Source I/O vector array.
 * \param src_iovcnt Size of the `src_iovs` array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param decomp_algo The decompression algorithm, enum spdk_accel_comp_algo value.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_accel_append_decompress_ext - sequence 에 압축 해제(알고리즘 명시) 큐잉.
 *
 * @pseq:        in/out sequence 핸들 (이 함수만 매개변수명 pseq 로 표기되지만 의미 동일).
 * @ch:          accel 채널.
 * @dst_iovs / @dst_iovcnt / @dst_domain / @dst_domain_ctx: 해제 출력 위치·도메인.
 * @src_iovs / @src_iovcnt / @src_domain / @src_domain_ctx: 압축 원본 위치·도메인.
 * @decomp_algo: DEFLATE / LZ4 명시.
 * @cb_fn / @cb_arg: step 콜백 / 컨텍스트.
 * @return:      0 큐잉 성공, 음수 errno.
 *
 * blob layer 의 read 경로 핵심: append_decompress + append_copy(BounceBuf→UserBuf) 식으로
 * 묶어 finish 하면 framework 가 BounceBuf 를 생략하고 disk→user buf 직접 해제 가능.
 */
int spdk_accel_append_decompress_ext(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
				     struct iovec *dst_iovs, size_t dst_iovcnt,
				     struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				     struct iovec *src_iovs, size_t src_iovcnt,
				     struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				     enum spdk_accel_comp_algo decomp_algo,
				     spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a decompression operation using the deflate algorithm to a sequence.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param dst_iovs Destination I/O vector array.
 * \param dst_iovcnt Size of the `dst_iovs` array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs Source I/O vector array.
 * \param src_iovcnt Size of the `src_iovs` array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_accel_append_decompress - sequence 에 DEFLATE 압축 해제 큐잉.
 *
 * @seq:         sequence 핸들.
 * @ch:          accel 채널.
 * @dst_iovs / @dst_iovcnt / @dst_domain / @dst_domain_ctx: 출력.
 * @src_iovs / @src_iovcnt / @src_domain / @src_domain_ctx: 입력.
 * @cb_fn / @cb_arg: step 콜백.
 * @return:      0 큐잉 성공, 음수 errno.
 *
 * append_decompress_ext 의 DEFLATE 고정 변형. blobstore 의 기본 압축이 DEFLATE 이므로
 * 가장 흔히 쓰이는 형태.
 */
int spdk_accel_append_decompress(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
				 struct iovec *dst_iovs, size_t dst_iovcnt,
				 struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				 struct iovec *src_iovs, size_t src_iovcnt,
				 struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				 spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append an encrypt operation to a sequence.
 *
 * `nbytes` must be multiple of `block_size`.  `iv` is used to encrypt the first logical block of
 * size `block_size`.  If `src_iovs` describes more than one logical block then `iv` will be
 * incremented for each next logical block.  Data Encryption Key identifier should be created before
 * calling this function using methods specific to the accel module being used.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param key Data Encryption Key identifier
 * \param dst_iovs Destination I/O vector array.
 * \param dst_iovcnt Size of the `dst_iovs` array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs Source I/O vector array.
 * \param src_iovcnt Size of the `src_iovs` array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param iv Initialization vector (tweak) used for encryption
 * \param block_size Logical block size, if src contains more than 1 logical block, subsequent
 *        logical blocks will be encrypted with incremented `iv`.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_accel_append_encrypt - sequence 에 암호화 step 큐잉.
 *
 * @seq:         sequence 핸들.
 * @ch:          accel 채널.
 * @key:         DEK 핸들 (사전 등록).
 * @dst_iovs / @dst_iovcnt / @dst_domain / @dst_domain_ctx: 암호문 출력.
 * @src_iovs / @src_iovcnt / @src_domain / @src_domain_ctx: 평문 입력.
 * @iv:          첫 logical block 의 IV/tweak. 후속 블록은 iv+1, iv+2 …
 * @block_size:  logical block 크기 (XTS sector 단위).
 * @cb_fn / @cb_arg: step 콜백.
 * @return:      0 큐잉 성공, 음수 errno.
 *
 * blob crypto 의 write 경로: append_copy(user_buf→bounce) + append_encrypt(bounce→disk_buf)
 * → finish 하면 framework 가 user_buf 에서 직접 암호화 (bounce 생략) 가능.
 */
int spdk_accel_append_encrypt(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			      struct spdk_accel_crypto_key *key,
			      struct iovec *dst_iovs, uint32_t dst_iovcnt,
			      struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			      struct iovec *src_iovs, uint32_t src_iovcnt,
			      struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			      uint64_t iv, uint32_t block_size,
			      spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a decrypt operation to a sequence.
 *
 * `nbytes` must be multiple of `block_size`. `iv` is used to decrypt the first logical block of
 * size `block_size`. If `src_iovs` describes more than one logical block then `iv` will be
 * incremented for each next logical block.  Data Encryption Key identifier should be created before
 * calling this function using methods specific to the accel module being used.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param key Data Encryption Key identifier
 * \param dst_iovs Destination I/O vector array.
 * \param dst_iovcnt Size of the `dst_iovs` array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs Source I/O vector array.
 * \param src_iovcnt Size of the `src_iovs` array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param iv Initialization vector (tweak) used for decryption. Should be the same as `iv` used for
 *        encryption of a data block.
 * \param block_size Logical block size, if src contains more than 1 logical block, subsequent
 *        logical blocks will be decrypted with incremented `iv`.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_accel_append_decrypt - sequence 에 복호화 step 큐잉.
 *
 * @seq / @ch / @key: 동일 의미.
 * @dst_iovs / @dst_iovcnt / @dst_domain / @dst_domain_ctx: 평문 출력.
 * @src_iovs / @src_iovcnt / @src_domain / @src_domain_ctx: 암호문 입력.
 * @iv:          암호화 시 사용한 동일 IV (정확히 일치해야 데이터 복원).
 * @block_size:  암호화 시 사용한 동일 block_size.
 * @cb_fn / @cb_arg: step 콜백.
 * @return:      0 큐잉 성공, 음수 errno.
 *
 * blob crypto read: append_decrypt(disk_buf→user_buf) 한 step 만으로 zero-copy 복호화.
 */
int spdk_accel_append_decrypt(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			      struct spdk_accel_crypto_key *key,
			      struct iovec *dst_iovs, uint32_t dst_iovcnt,
			      struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			      struct iovec *src_iovs, uint32_t src_iovcnt,
			      struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			      uint64_t iv, uint32_t block_size,
			      spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a crc32c operation to a sequence.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param dst Destination to write the calculated value.
 * \param iovs Source I/O vector array.
 * \param iovcnt Size of the `iovs` array.
 * \param domain Memory domain to which the source buffers belong.
 * \param domain_ctx Source buffer domain context.
 * \param seed Initial value.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_accel_append_crc32c - sequence 에 CRC32C 계산 step 큐잉.
 *
 * @seq:         sequence 핸들.
 * @ch:          accel 채널.
 * @dst:         CRC32C 결과(4 byte) 기록 위치 — sequence_finish cb 호출 시 유효.
 * @iovs / @iovcnt: 데이터 iov.
 * @domain / @domain_ctx: 데이터 메모리 도메인.
 * @seed:        초기 CRC.
 * @cb_fn / @cb_arg: step 콜백.
 * @return:      0 큐잉 성공, 음수 errno.
 *
 * NVMe-oF TCP send: append_copy(user→staging) + append_crc32c(staging) — finish 시
 * staging 메모리 패스 한 번으로 복사+CRC 동시에 실행 (백엔드가 SPDK_ACCEL_OPC_COPY_CRC32C
 * 를 지원하면 더더욱 합쳐짐).
 */
int spdk_accel_append_crc32c(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			     uint32_t *dst, struct iovec *iovs, uint32_t iovcnt,
			     struct spdk_memory_domain *domain, void *domain_ctx,
			     uint32_t seed, spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a Data Integrity Field (DIF) verify operation to a sequence.
 *
 * This operation computes the DIF on the data and compares it against the DIF contained
 * in the metadata.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param iovs The io vector array. The total allocated memory size needs to be at least:
 *             num_blocks * block_size (including metadata)
 * \param iovcnt The size of the io vectors array.
 * \param domain Memory domain to which the data buffers belong.
 * \param domain_ctx Data buffer domain context.
 * \param num_blocks Number of data blocks to check.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to check
 *            Note: the user must ensure the validity of this pointer throughout the entire operation
 *            because it is not validated along the processing path.
 * \param err DIF error detailed information.
 *            Note: the user must ensure the validity of this pointer throughout the entire operation
 *            because it is not validated along the processing path.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_append_dif_verify - sequence 에 in-place DIF 검증 step 큐잉.
 *
 * @seq:         sequence 핸들.
 * @ch:          accel 채널.
 * @iovs / @iovcnt: data+md 인터리브 입력.
 * @domain / @domain_ctx: 데이터 도메인.
 * @num_blocks:  검증 블록 수.
 * @ctx:         DIF 설정 (호출 종료까지 유효해야 함).
 * @err:         실패 시 상세 출력 (호출 종료까지 유효해야 함).
 * @cb_fn / @cb_arg: step 콜백.
 * @return:      0 큐잉 성공, 음수 errno.
 *
 * bdev read 핫패스: nvme_qpair 가 PI 포함 데이터를 받으면 bdev 가 sequence 에
 * append_dif_verify + append_copy_strip_md 식으로 묶어 한 번에 검증·strip 처리.
 */
int spdk_accel_append_dif_verify(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
				 struct iovec *iovs, size_t iovcnt,
				 struct spdk_memory_domain *domain, void *domain_ctx,
				 uint32_t num_blocks,
				 const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				 spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a Data Integrity Field (DIF) copy and verify operation to a sequence.
 *
 * This operation copies memory from the source to the destination address and removes
 * the DIF data with its verification according to the flags provided in the context.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param dst_iovs The destination I/O vector array. The total allocated memory size needs
 *                to be at least: num_blocks * data_block_size
 * \param dst_iovcnt The size of the destination I/O vectors array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs The source I/O vector array. The total allocated memory size needs
 *                to be at least: num_blocks * block_size (including metadata)
 * \param src_iovcnt The size of the source I/O vectors array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert.
 * \param err DIF error detailed information.
 *            Note: the user must ensure the validity of this pointer throughout the entire operation
 *            because it is not validated along the processing path.
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_append_dif_verify_copy - sequence 에 DIF 검증+strip 복사 step 큐잉.
 *
 * @seq / @ch: 표준.
 * @dst_iovs / @dst_iovcnt / @dst_domain / @dst_domain_ctx: data-only 출력.
 * @src_iovs / @src_iovcnt / @src_domain / @src_domain_ctx: data+PI 입력.
 * @num_blocks:  처리 블록 수.
 * @ctx / @err:  DIF 설정·에러 상세.
 * @cb_fn / @cb_arg: step 콜백.
 * @return:      0 큐잉 성공, 음수 errno.
 *
 * NVMe read end-to-end PI: bdev read 가 disk 에서 PI-on data 를 받았을 때 sequence 에
 * 이 step 만 한 번 추가하면 검증·strip·user buf 로 복사가 한 번에. zero-copy 분석에서
 * src 와 dst 가 동일 도메인이면 단일 메모리 패스로 합쳐짐.
 */
int spdk_accel_append_dif_verify_copy(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
				      struct iovec *dst_iovs, size_t dst_iovcnt,
				      struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				      struct iovec *src_iovs, size_t src_iovcnt,
				      struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				      uint32_t num_blocks,
				      const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				      spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a Data Integrity Field (DIF) generate operation to a sequence.
 *
 * This operation compute the DIF on the source data and inserting the DIF in place into
 * the source data.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel associated with this call.
 * \param iovs The io vector array. The total allocated memory size needs to be at least:
 *             num_blocks * block_size (including metadata)
 * \param iovcnt The size of the io vectors array.
 * \param domain Memory domain to which the data buffers belong.
 * \param domain_ctx Data buffer domain context.
 * \param num_blocks Number of data blocks.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_append_dif_generate - sequence 에 in-place DIF 생성 step 큐잉.
 *
 * @seq / @ch: 표준.
 * @iovs / @iovcnt: data+md 인터리브 입출력 (md 영역에 PI 가 채워짐).
 * @domain / @domain_ctx: 메모리 도메인.
 * @num_blocks:  처리 블록 수.
 * @ctx:         DIF 설정 (PI format, 초기 ref_tag 등).
 * @cb_fn / @cb_arg: step 콜백.
 * @return:      0 큐잉 성공, 음수 errno.
 *
 * 이미 LBA 단위 layout 으로 정렬된 buffer 가 있을 때 PI 만 채워 디스크에 내려보내는
 * sequence 의 마지막 step 으로 흔히 사용.
 */
int spdk_accel_append_dif_generate(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
				   struct iovec *iovs, size_t iovcnt,
				   struct spdk_memory_domain *domain, void *domain_ctx,
				   uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
				   spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Submit a Data Integrity Field (DIF) copy and generate request.
 *
 * This operation copies memory from the source to the destination address,
 * while computing the DIF on the source data and inserting the DIF into
 * the output data.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel associated with this call.
 * \param dst_iovs The destination io vector array. The total allocated memory size needs
 *                to be at least: num_blocks * block_size (provided to spdk_dif_ctx_init())
 * \param dst_iovcnt The size of the destination io vectors array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs The source io vector array. The total allocated memory size needs
 *                to be at least: num_blocks * data_block_size.
 * \param src_iovcnt The size of the source io vectors array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_append_dif_generate_copy - sequence 에 data-only→data+DIF 복사·생성 step 큐잉.
 *
 * @seq / @ch: 표준.
 * @dst_iovs / @dst_iovcnt / @dst_domain / @dst_domain_ctx: data+PI 출력 (≥ num_blocks
 *               × block_size).
 * @src_iovs / @src_iovcnt / @src_domain / @src_domain_ctx: data-only 입력
 *               (≥ num_blocks × data_block_size).
 * @num_blocks:  처리 블록 수.
 * @ctx:         DIF 설정.
 * @cb_fn / @cb_arg: step 콜백.
 * @return:      0 큐잉 성공, 음수 errno.
 *
 * NVMe write 핫패스: bdev_io 가 user 의 raw data 만 받았을 때 sequence 에 이 step 만
 * 추가하면 disk 전송 직전 layout 변환·PI 부착이 한 번에. 추가로 append_encrypt 을
 * 뒤에 붙이면 복사·DIF 생성·암호화가 한 sequence 로 묶여 zero-copy 디스패치.
 */
int spdk_accel_append_dif_generate_copy(struct spdk_accel_sequence **seq,
					struct spdk_io_channel *ch,
					struct iovec *dst_iovs, size_t dst_iovcnt,
					struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
					struct iovec *src_iovs, size_t src_iovcnt,
					struct spdk_memory_domain *src_domain, void *src_domain_ctx,
					uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
					spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append DIX generate operation to a sequence.
 *
 * \param seq Sequence object. If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param iovs Source I/O vector array. The total allocated memory size needs to be at least:
 *	num_blocks * block_size_no_md.
 * \param iovcnt Size of the source I/O vectors' array.
 * \param domain Memory domain to which the source buffers belong.
 * \param domain_ctx Source buffer domain context.
 * \param md_iov Metadata iovec. The total allocated memory size needs to be at least:
 *	num_blocks * md_size (8B or 16B, depending on the PI format).
 * \param md_domain Memory domain to which the metadata buffers belongs.
 * \param md_domain_ctx Metadata buffer domain context.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIX context. Contains the DIX configuration values, including the reference
 *	Application Tag value and initial value of the Reference Tag to insert.
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \returns 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_append_dix_generate - sequence 에 DIX 생성 step (data, md 분리) 큐잉.
 *
 * @seq / @ch: 표준.
 * @iovs / @iovcnt: data-only 입력.
 * @domain / @domain_ctx: 데이터 도메인.
 * @md_iov:      metadata 출력 — 별도 buffer (≥ num_blocks × md_size).
 * @md_domain / @md_domain_ctx: metadata 의 도메인 (data 와 다른 도메인 가능 —
 *               예: data 는 host RAM, md 는 GPU 메모리).
 * @num_blocks:  처리 블록 수.
 * @ctx:         DIX context.
 * @cb_fn / @cb_arg: step 콜백.
 * @return:      0 큐잉 성공, 음수 errno.
 *
 * NVMe MD-separate write: data 와 md 를 분리해 운반하는 모드에서, host 가 PI 를 별도
 * md_iov 로 만들어 디스크에 보낼 때 사용.
 */
int spdk_accel_append_dix_generate(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
				   struct iovec *iovs, size_t iovcnt,
				   struct spdk_memory_domain *domain, void *domain_ctx,
				   struct iovec *md_iov, struct spdk_memory_domain *md_domain,
				   void *md_domain_ctx, uint32_t num_blocks,
				   const struct spdk_dif_ctx *ctx,
				   spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append DIX verify operation to a sequence.
 *
 * \param seq Sequence object. If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param iovs Source I/O vector array. The total allocated memory size needs to be at least:
 *	num_blocks * block_size_no_md.
 * \param iovcnt Size of the source I/O vectors' array.
 * \param domain Memory domain to which the source buffers belong.
 * \param domain_ctx Source buffer domain context.
 * \param md_iov Metadata iovec. The total allocated memory size needs to be at least:
 *	num_blocks * md_size (8B or 16B, depending on the PI format).
 * \param md_domain Memory domain to which the metadata buffers belongs.
 * \param md_domain_ctx Metadata buffer domain context.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIX context. Contains the DIX configuration values, including the reference
 *	Application Tag value and initial value of the Reference Tag to insert.
 * \param err DIX error detailed information.
 *	Note: the user must ensure the validity of this pointer throughout the entire
 *	operation because it is not validated along the processing path.
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \returns 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_accel_append_dix_verify - sequence 에 DIX 검증 step (data, md 분리) 큐잉.
 *
 * @seq / @ch: 표준.
 * @iovs / @iovcnt: data-only 입력.
 * @domain / @domain_ctx: 데이터 도메인.
 * @md_iov / @md_domain / @md_domain_ctx: metadata 입력 + 도메인.
 * @num_blocks:  검증 블록 수.
 * @ctx:         DIX context (검증 모드).
 * @err:         실패 시 상세 (호출 종료까지 유효).
 * @cb_fn / @cb_arg: step 콜백.
 * @return:      0 큐잉 성공, 음수 errno.
 *
 * MD-separate read: NVMe disk 가 data 와 md 를 분리해 보냈을 때 host 측 검증.
 * 실패 시 sequence_finish 의 final cb 로 비-0 status 가 통보된다.
 */
int spdk_accel_append_dix_verify(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
				 struct iovec *iovs, size_t iovcnt,
				 struct spdk_memory_domain *domain, void *domain_ctx,
				 struct iovec *md_iov, struct spdk_memory_domain *md_domain,
				 void *md_domain_ctx, uint32_t num_blocks,
				 const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				 spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Finish a sequence and execute all its operations. After the completion callback is executed, the
 * sequence object is automatically freed.
 *
 * \param seq Sequence to finish.
 * \param cb_fn Completion callback to be executed once all operations are executed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 */
/*
 * [한국어]
 * spdk_accel_sequence_finish - sequence 의 모든 step 을 실행하고 완료 후 객체 해제.
 *
 * @seq:    실행할 sequence (append_*() 들의 결과).
 * @cb_fn:  최종 완료 콜백 — 모든 step 이 끝난 후 단 한 번 호출. status 인자에는
 *          첫 번째로 발생한 에러 (있다면) 또는 0 (모두 성공) 이 들어감.
 * @cb_arg: cb_fn 컨텍스트.
 *
 * 동작 요약:
 *   1. zero-copy 분석: 인접 op 들의 src/dst 가 직접 연결 가능한지 검사하여 중간
 *      copy step 을 생략하거나 인자를 재작성.
 *   2. 백엔드 매핑: 각 step 의 opcode 를 처리할 모듈을 capability 매트릭스에서 선택.
 *   3. 디스패치: 단일 백엔드가 연속 step 들을 chained 로 받을 수 있으면 한 번에 제출,
 *      아니면 step 별로 개별 제출 + 다음 step 트리거.
 *   4. 모든 step 완료 후 step_cb 들이 차례로 호출되고 마지막에 cb_fn(seq 결과) 호출.
 *   5. cb_fn 호출 직전·직후에 sequence 객체 자동 free — 호출자는 seq 핸들 더 이상 사용 금지.
 *
 * 호출 컨텍스트: append 들과 동일 spdk_thread. cb_fn 도 동일 thread 에서 호출.
 *
 * 호출 체인:
 *   상위 (bdev/blob) submit → append_*() 여러 번 → [spdk_accel_sequence_finish] →
 *     백엔드 모듈 → … → cb_fn (sequence freed)
 */
void spdk_accel_sequence_finish(struct spdk_accel_sequence *seq,
				spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Reverse a sequence, so that the last operation becomes the first and vice versa.
 *
 * \param seq Sequence to reverse.
 */
/*
 * [한국어]
 * spdk_accel_sequence_reverse - sequence 의 step 순서를 역순으로 뒤집음.
 *
 * @seq: append_*() 로 빌드된 sequence (아직 finish 되지 않은 상태).
 *
 * 사용 사례: bdev 가 write path 용 sequence (compress→encrypt→DIF generate) 를 만들었지만
 * 실패해서 read 로 정정해야 하는 경우, 동일 step 시퀀스를 역순(DIF strip→decrypt→
 * decompress) 로 재사용 — src 와 dst 는 그대로지만 step 의 의미가 inverse 가 되도록
 * 사용자가 직접 op 종류를 미리 inverse 로 append 한 sequence 에 적용.
 *
 * 실제로는 bdev 의 zero-copy 회복 경로에서 자주 쓰임 — finish 직전이라면 호출 가능,
 * finish 후에는 객체가 free 되므로 호출 불가.
 */
void spdk_accel_sequence_reverse(struct spdk_accel_sequence *seq);

/**
 * Abort a sequence.  This will execute the completion callbacks of all operations that were added
 * to the sequence and will then free the sequence object.  This function can only be used before a
 * sequence is executed, i.e. before calling `spdk_accel_sequence_finish()`.
 *
 * \param seq Sequence to abort.
 */
/*
 * [한국어]
 * spdk_accel_sequence_abort - sequence 를 취소하고 객체 해제 (finish 전 only).
 *
 * @seq: 취소할 sequence.
 *
 * 동작: 큐잉된 모든 step 의 step_cb 들을 호출(보통 NULL 들이라 no-op)한 후 sequence 객체
 * free. 어떤 step 도 실제 디스패치되지 않으므로 src/dst 데이터는 변경되지 않는다.
 *
 * 사용 시점: bdev 가 sequence 를 build 하던 중 다른 쪽에서 io abort 가 들어와 더 이상
 * 진행할 필요가 없을 때, 또는 setup 단계에서 -ENOMEM 같은 에러로 롤백이 필요할 때.
 * spdk_accel_sequence_finish() 호출 후에는 사용 금지 (객체가 이미 framework 소유).
 */
void spdk_accel_sequence_abort(struct spdk_accel_sequence *seq);

/**
 * Allocate a buffer from accel domain.  These buffers can be only used with operations appended to
 * a sequence.  The actual data buffer won't be allocated immediately, but only when it's necessary
 * to execute a given operation.  In some cases, this might even mean that a data buffer won't be
 * allocated at all, if a sequence can be executed without it.
 *
 * A buffer can only be a part of one sequence, but it can be used by multiple operations within
 * that sequence.
 *
 * \param ch I/O channel.
 * \param len Length of the buffer to allocate.
 * \param buf Pointer to the allocated buffer.
 * \param domain Memory domain in which the buffer is allocated.
 * \param domain_ctx Memory domain context related to the allocated buffer.
 *
 * \return 0 if a buffer was successfully allocated, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_accel_get_buf - sequence 전용 lazy-allocated buffer 획득.
 *
 * @ch:         accel 채널.
 * @len:        요청 길이.
 * @buf:        out — 가상 주소 (실제 메모리는 lazy 할당).
 * @domain:     out — buffer 가 속한 memory domain (보통 accel-internal domain).
 * @domain_ctx: out — domain context.
 * @return:     0 성공, 음수 errno (-ENOMEM, -EINVAL).
 *
 * 핵심 아이디어: 사용자가 이 buffer 를 sequence 의 한 step 의 src/dst 로 사용해도
 * 실제 물리 메모리 할당은 finish 시점에 framework 가 정말 필요하다고 판단했을 때만
 * 발생. zero-copy 분석으로 이 buffer 가 인접 op 의 다른 buffer 로 대체 가능하면
 * 아예 할당되지 않을 수 있다.
 *
 * 제약: 한 buffer 는 단 하나의 sequence 에만 속할 수 있고, 그 sequence 내에서는
 * 여러 step 이 공유 가능. spdk_accel_put_buf() 로 해제.
 *
 * 호출 체인:
 *   bdev/blob 가 staging buffer 필요 → [spdk_accel_get_buf] → append_*() 의 인자 →
 *     spdk_accel_sequence_finish (이 시점에 실제 할당 결정)
 */
int spdk_accel_get_buf(struct spdk_io_channel *ch, uint64_t len, void **buf,
		       struct spdk_memory_domain **domain, void **domain_ctx);

/**
 * Release a buffer allocated via `spdk_accel_get_buf()`.
 *
 * \param ch I/O channel.
 * \param buf Buffer allocated via `spdk_accel_get_buf()`.
 * \param domain Memory domain in which the buffer is allocated.
 * \param domain_ctx Memory domain context related to the allocated buffer.
 */
/*
 * [한국어]
 * spdk_accel_put_buf - spdk_accel_get_buf() 로 얻은 buffer 해제.
 *
 * @ch:         accel 채널 (동일 채널이어야 함).
 * @buf:        해제할 buffer 포인터.
 * @domain:     get_buf 가 반환한 동일 domain.
 * @domain_ctx: get_buf 가 반환한 동일 domain_ctx.
 *
 * 보통 sequence_finish 의 cb_fn 안에서 호출하여 자원 누수를 막는다. 실제로 framework 가
 * 메모리를 할당했었다면 여기서 풀에 반환, 할당하지 않았다면 no-op 와 유사한 처리.
 */
void spdk_accel_put_buf(struct spdk_io_channel *ch, void *buf,
			struct spdk_memory_domain *domain, void *domain_ctx);

/**
 * Return the name of the module assigned to a specific opcode.
 *
 * \param opcode Accel Framework Opcode enum value. Valid codes can be retrieved using
 * `accel_get_opc_assignments` or `spdk_accel_get_opcode_name`.
 * \param module_name Pointer to update with module name.
 *
 * \return 0 if a valid module name was provided. -EINVAL for invalid opcode
 *  or -ENOENT no module was found at this time for the provided opcode.
 */
/*
 * [한국어]
 * spdk_accel_get_opc_module_name - 특정 opcode 를 처리하도록 매핑된 모듈 이름 조회.
 *
 * @opcode:      조회할 작업 종류.
 * @module_name: out — 모듈 이름 문자열 포인터 (framework 소유, 호출자가 free 하면 안 됨).
 * @return:      0 성공, -EINVAL 잘못된 opcode, -ENOENT 매핑된 모듈 없음.
 *
 * RPC 응답(`accel_get_opc_assignments`) 빌드, 디버그 로그, 사용자 코드의 capability
 * 확인 등에 사용. 결과 문자열은 "software", "dsa", "iaa", "ioat", "mlx5_pci",
 * "ae4dma" 등.
 */
int spdk_accel_get_opc_module_name(enum spdk_accel_opcode opcode, const char **module_name);

/**
 * Override the assignment of an opcode to an module.
 *
 * \param opcode Accel Framework Opcode enum value. Valid codes can be retrieved using
 * `accel_get_opc_assignments` or `spdk_accel_get_opcode_name`.
 * \param name Name of the module to assign. Valid module names may be retrieved
 * with `spdk_accel_get_opc_module_name`
 *
 * \return 0 if a valid opcode name was provided. -EINVAL for invalid opcode
 *  or if the framework has started (cannot change modules after startup)
 */
/*
 * [한국어]
 * spdk_accel_assign_opc - 특정 opcode 를 처리할 모듈을 사용자가 명시 지정.
 *
 * @opcode: 매핑 대상 작업 종류.
 * @name:   할당할 모듈 이름 (NULL/빈 문자열은 할당 해제 의미는 아님 — 별도 처리 필요).
 *          spdk_accel_get_opc_module_name() 으로 가능한 이름 조회.
 * @return: 0 성공, -EINVAL (opcode 부적절 또는 framework 가 이미 시작된 후 호출).
 *
 * 우선순위 결정 메커니즘: 평소에는 framework 가 모듈 priority + capability 로 자동
 * 매핑하지만, 사용자가 특정 opcode 를 강제로 다른 모듈로 보내고 싶을 때 사용.
 * 예: CRC32C 를 default IDXD 대신 software (ISA-L) 로 강제.
 *
 * 제약: spdk_accel_initialize() 가 끝난 후에는 변경 불가 — startup config 단계에서만
 * 호출. 보통 RPC `accel_assign_opc` 또는 JSON config 에서 처리.
 */
int spdk_accel_assign_opc(enum spdk_accel_opcode opcode, const char *name);

struct spdk_json_write_ctx;
/* [한국어] JSON write context 의 전방선언 — spdk/json.h 에 정의된 incremental JSON
 * builder. 여기서는 spdk_accel_write_config_json() 의 인자 타입으로만 사용되어
 * 풀 정의를 끌어오지 않고 declaration 만 명시 (헤더 의존성 최소화). */

/**
 * Write Acceleration subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
/*
 * [한국어]
 * spdk_accel_write_config_json - 현재 accel 서브시스템 설정을 JSON 으로 직렬화.
 *
 * @w: 출력 대상 JSON write context (보통 RPC 핸들러 `save_config` 에서 전달).
 *
 * 포함 내용: 등록된 crypto key 목록(이름·cipher·tweak_mode 만, 키 자료는 보안상 제외),
 * opcode → module 할당 오버라이드, accel_opts (iobuf/cache/task counts), driver 이름.
 * 출력은 다른 SPDK 서브시스템과 함께 spdk_save_config 가 한 파일에 합쳐, 다음 부팅 시
 * spdk_subsystem_init_from_json 으로 재현된다.
 */
void spdk_accel_write_config_json(struct spdk_json_write_ctx *w);

/**
 * Select platform driver to execute operation chains.
 *
 * \param name Name of the driver.  If NULL or empty string, this function will clear the driver
 * that was previously assigned.
 *
 * \return 0 on success, negetive errno otherwise.
 */
/*
 * [한국어]
 * spdk_accel_set_driver - operation chain 을 실행할 platform driver 선택.
 *
 * @name:   driver 이름 (NULL 또는 빈 문자열이면 기존 driver 해제, framework 가 백엔드를
 *          op 별로 직접 dispatch 하는 모드로 복귀).
 * @return: 0 성공, 음수 errno (-EINVAL 등록되지 않은 driver, -EBUSY framework start 후).
 *
 * Driver 와 Module 의 차이:
 *   - module: 한 opcode 를 단독 처리 (예: dsa 모듈은 COPY/FILL/CRC32C 만 처리).
 *   - driver: sequence 전체를 받아 한 번에 처리하는 통합 dispatcher (예: mlx5 driver 가
 *     copy+CRC+encrypt 를 단일 RDMA 디스크립터로 합쳐 처리).
 * driver 가 설정되면 spdk_accel_sequence_finish 가 driver 의 execute 콜백으로 위임.
 */
int spdk_accel_set_driver(const char *name);

/**
 * Get platform driver name.
 *
 * \return Name of the driver as a null-terminated string or NULL if driver not set.
 */
/*
 * [한국어]
 * spdk_accel_get_driver_name - 현재 설정된 platform driver 이름 조회.
 *
 * @return: NUL-terminated 문자열 (framework 소유, 호출자 free 금지),
 *          또는 NULL (driver 미설정 — module 단위 dispatch 모드).
 *
 * RPC 응답·디버그 로그용. driver 가 없으면 framework 의 기본 module 매핑이 그대로 사용됨.
 */
const char *spdk_accel_get_driver_name(void);

/**
 * Retrieves accel memory domain.
 *
 * \return Accel memory domain.
 */
/*
 * [한국어]
 * spdk_accel_get_memory_domain - accel framework 자체가 정의한 메모리 도메인 핸들 반환.
 *
 * @return: spdk_accel_get_buf() 가 할당하는 buffer 들이 속한 도메인.
 *
 * 사용처: 외부 모듈(bdev, blob)이 특정 buffer 가 accel-internal 인지 (= lazy 할당이거나
 * 가속기 친화적 메모리인지) 판별할 때, dst_domain 인자로 넘겨야 할 도메인이 무엇인지
 * 알아야 할 때 사용. spdk_memory_domain API 와 호환.
 */
struct spdk_memory_domain *spdk_accel_get_memory_domain(void);

struct spdk_accel_opts {
	/* [한국어] accel framework 의 전역 튜닝 파라미터. spdk_accel_set_opts() 로 init 전에
	 * 적용, spdk_accel_get_opts() 로 현재 값 조회. 기본값은 lib/accel/accel.c 의
	 * accel_opts_init() 에서 설정. ABI 호환성을 위해 opts_size 필드를 사용한 versioning. */

	/**
	 * The size of spdk_accel_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t		opts_size;
	/* [한국어] 호출자가 인지한 구조체 크기 (sizeof(spdk_accel_opts) 당시의 값). framework 는
	 * 이 값으로 호출자가 어떤 필드까지 알고 있는지 판단해, 모르는 필드는 default 로 채운다.
	 * 새 필드는 항상 끝에 추가하고, 기존 필드 순서는 절대 변경하지 않아 ABI 안정성 유지.
	 * 설정자: 사용자(memset 후 opts_size = sizeof(*opts) 로 설정).
	 * 읽는 자: spdk_accel_set_opts/get_opts 가 forward/backward 호환 처리. */

	/** Size of the small iobuf cache */
	uint32_t	small_cache_size;
	/* [한국어] per-channel 의 small iobuf (보통 4KB 이하 작은 버퍼) 캐시 슬롯 수.
	 * 각 thread 의 채널이 spdk_accel_get_buf() 호출 시 hit ratio 를 결정. 너무 작으면
	 * 풀 contention, 너무 크면 메모리 낭비. 기본값은 lib/accel 내부 상수.
	 * 설정자: spdk_accel_set_opts.
	 * 읽는 자: 채널 ctor 가 spdk_iobuf_channel_init 에 전달. */

	/** Size of the large iobuf cache */
	uint32_t	large_cache_size;
	/* [한국어] large iobuf (4KB 이상, 압축/암호화 등 큰 버퍼용) 캐시 슬롯 수. 압축
	 * 결과·DMA staging 처럼 큰 메모리가 자주 쓰이는 워크로드는 이 값을 늘려야 한다. */

	/** Maximum number of tasks per IO channel */
	uint32_t	task_count;
	/* [한국어] per-channel task 풀 크기 — 동시에 in-flight 가능한 단일 op 작업 수의 상한.
	 * 풀 고갈 시 spdk_accel_submit_*() 가 -ENOMEM 반환. 큐 깊이 큰 NVMe-oF 워크로드는
	 * 충분히 크게 (수천 단위). */

	/** Maximum number of sequences per IO channel */
	uint32_t	sequence_count;
	/* [한국어] per-channel sequence 풀 크기 — 동시에 빌드/실행 가능한 sequence 수.
	 * bdev_io 한 개 = sequence 한 개 정도로 보면 task_count 와 비슷한 수준이 적절. */

	/** Maximum number of accel buffers per IO channel */
	uint32_t	buf_count;
	/* [한국어] spdk_accel_get_buf() 가 채널에서 동시 보유 가능한 버퍼 수. 한 sequence 가
	 * 여러 buf 를 쓸 수 있으므로 sequence_count × 평균 buf/seq 정도로 사이즈. */

} __attribute__((packed));
/* [한국어] __attribute__((packed)) — 컴파일러가 padding 을 삽입하지 않도록 강제.
 * opts_size 기반 versioning 이 정확히 동작하려면 필드 offset 이 컴파일 옵션·아키텍처에
 * 무관해야 한다. 패딩이 들어가면 size 비교가 어긋나 호환성 검사 실패 가능. */

/**
 * Set the options for the accel framework.
 *
 * \param opts Accel options.
 *
 * \return 0 on success, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_accel_set_opts - accel 전역 옵션 적용 (init 전 또는 RPC 단계에서).
 *
 * @opts:   호출자가 채운 옵션 구조체. opts_size 필수.
 * @return: 0 성공, -EINVAL (잘못된 값), -EBUSY (framework 시작 후 호출).
 *
 * 보통 RPC `accel_set_options` 에서 호출되며, 이후 spdk_accel_initialize() 가 이 값으로
 * iobuf cache·task/seq/buf 풀을 생성한다. init 후에는 변경 불가.
 */
int spdk_accel_set_opts(const struct spdk_accel_opts *opts);

/**
 * Get the options for the accel framework.
 *
 * \param opts Output parameter for options.
 * \param opts_size sizeof(*opts)
 */
/*
 * [한국어]
 * spdk_accel_get_opts - 현재 적용된 옵션 조회.
 *
 * @opts:      out — 채워질 구조체 (호출자 할당).
 * @opts_size: 호출자가 인지한 sizeof(struct spdk_accel_opts) — framework 가 자기 버전과
 *             비교하여 호환되는 필드까지만 채움.
 *
 * RPC `accel_get_options` 응답·디버그용. set_opts 와 함께 ABI versioning 의 양쪽 짝.
 */
void spdk_accel_get_opts(struct spdk_accel_opts *opts, size_t opts_size);

struct spdk_accel_opcode_stats {
	/* [한국어] 채널 × opcode 단위로 누적되는 실행 통계. lock-free per-thread 카운터로
	 * 관리되며, RPC `accel_get_stats` 가 모든 채널을 순회하며 합산해 보고. 실시간
	 * 디버깅·SLA 모니터링·튜닝의 1차 자료. */

	/** Number of executed operations */
	uint64_t	executed;
	/* [한국어] 해당 channel/opcode 로 시작된 작업의 누적 카운트 (성공/실패 포함).
	 * 설정자: framework 의 finish_task 핸들러가 op 완료 시 1 증가.
	 * 읽는 자: spdk_accel_get_opcode_stats(), RPC 응답 build.
	 * 동기화: 동일 채널을 소유한 thread 만 증가하므로 lockless. */

	/** Number of failed operations */
	uint64_t	failed;
	/* [한국어] cb_fn status 가 비-0 이었던 작업 수 (compare miscompare 포함될 수 있음 —
	 * 의미상 의도된 mismatch 도 fail 로 카운트할지 백엔드 정책에 따라). 모니터링에서
	 * executed 대비 failed ratio 가 SSD 무결성 이슈·HW 가속기 오작동 신호. */

	/** Number of processed bytes */
	uint64_t	num_bytes;
	/* [한국어] 처리된 데이터 누적 바이트 — throughput 계산용. opcode 별로 의미 다름:
	 * COPY 는 nbytes, COMPRESS 는 입력 길이, ENCRYPT 는 src 길이 등. RPC 응답에서
	 * 시간 차분으로 GB/s 등을 산출. */

} __attribute__((packed));
/* [한국어] packed — 위 spdk_accel_opts 와 같은 이유로 ABI 안정성 확보 (struct 크기
 * 가 컴파일러/아키텍처 무관하게 일정). */

/**
 * Retrieve opcode statistics for a given IO channel.
 *
 * \param ch I/O channel.
 * \param opcode Operation to retrieve statistics.
 * \param stats Per-channel statistics.
 * \param size Size of the `stats` structure.
 */
/*
 * [한국어]
 * spdk_accel_get_opcode_stats - 단일 채널의 단일 opcode 통계 조회.
 *
 * @ch:     조회 대상 accel 채널 (해당 thread 에서 호출되어야 lockless 정확).
 * @opcode: 조회할 작업 종류.
 * @stats:  out — 결과 기록될 구조체.
 * @size:   sizeof(*stats) — ABI 호환을 위해 호출자 인지 크기 전달.
 *
 * 다른 thread 의 채널을 안전하게 조회하려면 spdk_thread_send_msg 로 그 thread 에서
 * 호출하도록 위임해야 한다 (lockless 모델 유지). 글로벌 누적 통계는 모든 채널을 순회·합산.
 *
 * 호출 체인:
 *   RPC accel_get_stats handler → 각 thread send_msg → [spdk_accel_get_opcode_stats]
 *     → 결과를 모아서 JSON 응답
 */
void spdk_accel_get_opcode_stats(struct spdk_io_channel *ch, enum spdk_accel_opcode opcode,
				 struct spdk_accel_opcode_stats *stats, size_t size);

/**
 * Context for the `spdk_accel_get_buf_align()` function.  Depending on the operation, some of the
 * fields might be unused.
 */
struct spdk_accel_operation_exec_ctx {
	/* [한국어] spdk_accel_get_buf_align() 에 전달되는 컨텍스트. opcode 마다 alignment
	 * 가 다른 인자에 의존하므로 (예: encrypt 는 block_size, copy 는 의존 없음), 함수
	 * signature 가 비대해지지 않도록 컨텍스트 객체로 전달. ABI versioning 위해 size 필드
	 * 운용. */

	/** Size of this structure in bytes */
	size_t size;
	/* [한국어] sizeof(struct spdk_accel_operation_exec_ctx) — ABI 호환용 크기 표시.
	 * 호출자 채워서 전달, framework 가 자기 인지 크기와 비교해 forward/backward 호환 처리. */

	/** Block size in bytes, required for encrypt and decrypt */
	uint32_t block_size;
	/* [한국어] AES-XTS/CBC 의 logical block 크기 (예: 4096). encrypt/decrypt opcode 의
	 * alignment 결정에 필요 — XTS 는 block_size 의 배수 정렬을 요구. 다른 opcode 에서는
	 * 무시 (the doc 의 "some fields might be unused"). */
};

/**
 * Get minimum buffer alignment to execute a given operation.  It accounts for constraints of a
 * module assigned to execute a given operation and the driver (if set). The alignment is returned
 * as a power of 2.  The value of 0 means that the buffers don't need to be aligned.
 *
 * \param opcode Opcode.
 * \param ctx Context in which the operation will be executed.
 *
 * \return Minimum alignment.
 */
/*
 * [한국어]
 * spdk_accel_get_buf_align - 특정 opcode 실행에 필요한 buffer 정렬(alignment) 조회.
 *
 * @opcode: 작업 종류.
 * @ctx:    실행 컨텍스트 (encrypt 라면 block_size 채워야 함).
 * @return: alignment 의 power-of-2 지수 (0 = 정렬 무관, 12 = 4KB 정렬, 등).
 *
 * 결정 로직: framework 는 (1) 해당 opcode 에 매핑된 모듈의 capability 가 요구하는
 * alignment, (2) 설정된 driver (있다면) 의 alignment 를 모두 합쳐 가장 strict 한 값을
 * 반환. 사용자는 이 값을 보고 spdk_iobuf alloc 또는 hugepage alloc 시 적절한 정렬을
 * 보장한 buffer 를 사용해야 한다 (예: IDXD dualcast 는 4KB 정렬 강제).
 *
 * 호출 체인:
 *   bdev/blob 가 staging buffer 할당 전 → [spdk_accel_get_buf_align] →
 *     spdk_dma_zmalloc(... aligned)
 */
uint8_t spdk_accel_get_buf_align(enum spdk_accel_opcode opcode,
				 const struct spdk_accel_operation_exec_ctx *ctx);

/**
 * Return memory domains used by specific opcode.
 *
 * The returned memory domains depend on the accel module which is assigned to handle the \b opcode
 *
 * \param opcode Accel Framework Opcode enum value.
 * \param domains Pointer to an array of memory domains to be filled by this function. The user should allocate big enough
  * array to keep all memory domains.
 * \param array_size size of \b domains array
 * \return the number of entries in \b domains array or negated errno. If returned value is bigger than \b array_size passed by the user
  * then the user should increase the size of \b domains array and call this function again. There is no guarantees that
  * the content of \b domains array is valid in that case.
 */
/*
 * [한국어]
 * spdk_accel_get_opc_memory_domains - 특정 opcode 가 처리할 수 있는 memory domain 목록 조회.
 *
 * @opcode:     작업 종류.
 * @domains:    out — 채워질 domain 포인터 배열 (호출자 할당). 충분히 크게 할당해야 함.
 * @array_size: domains 배열 길이.
 * @return:     실제 도메인 개수 (양수). > array_size 라면 결과 잘림 — 사용자는 더 큰
 *              배열로 재호출 필요. 음수면 errno (-EINVAL 등).
 *
 * 사용처: bdev 가 자신의 buffer 가 GPU 메모리 도메인에 있을 때, accel 모듈이 이를 직접
 * 처리할 수 있는지 사전 확인. 만약 없다면 호스트 RAM 으로 bounce 후 처리해야 함.
 * 반환 도메인은 해당 opcode 에 매핑된 모듈이 advertise 하는 dma capability 의 union.
 *
 * 호출 체인:
 *   bdev_register / domain negotiation → [spdk_accel_get_opc_memory_domains]
 */
int spdk_accel_get_opc_memory_domains(enum spdk_accel_opcode opcode,
				      struct spdk_memory_domain **domains, int array_size);

/**
 * Return the name of an operation based on the opcode.
 *
 * \param opcode Opcode.
 *
 * \return Name of the operation.
 */
/*
 * [한국어]
 * spdk_accel_get_opcode_name - opcode → 사람 읽을 문자열 변환.
 *
 * @opcode: 작업 종류 enum.
 * @return: "copy", "fill", "crc32c", "compress" 등 NUL-terminated 문자열 (framework
 *          소유, free 금지). 잘못된 opcode 는 NULL 또는 "unknown".
 *
 * RPC 응답 build, 로그 메시지, 사용자가 enum 값을 그대로 출력하지 않도록 텍스트화할 때
 * 사용. spdk_accel_assign_opc 의 인자로 받는 모듈 이름과 별개 — 이 함수는 opcode 자체의
 * canonical name.
 */
const char *spdk_accel_get_opcode_name(enum spdk_accel_opcode opcode);

#ifdef __cplusplus
}
/* [한국어] extern "C" 닫기 — C++ 컴파일러에서 위 선언 묶음을 C-ABI 로 묶었던 영역의 끝. */
#endif

#endif
/* [한국어] SPDK_ACCEL_H 가드 닫기 — 헤더 다중 포함 방지 영역의 끝. */
