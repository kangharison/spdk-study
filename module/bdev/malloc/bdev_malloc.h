/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] malloc bdev 모듈의 내부 헤더 (bdev_malloc.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK의 "malloc bdev" 모듈이 외부(주로 같은 모듈 내의
 * RPC 핸들러 bdev_malloc_rpc.c)에 노출하는 자료형과 API 선언을 모은다.
 * malloc bdev란 호스트의 hugepage 기반 RAM을 백엔드로 사용하는 가상
 * 블록 디바이스로서, NVMe SSD 같은 실제 저장 장치 없이도 SPDK bdev
 * 레이어 위에서 동작하는 테스트/벤치마크용 디바이스이다.
 * 이 헤더는 (1) malloc bdev 생성에 필요한 모든 옵션을 담는
 * `struct malloc_bdev_opts`, (2) 디스크 생성 함수 `create_malloc_disk`,
 * (3) 비동기 삭제 함수 `delete_malloc_disk`와 그 완료 콜백 타입을
 * 정의한다. 즉 이 모듈을 외부에서 제어할 수 있는 가장 작은 인터페이스이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택은 [Application] → [bdev API (lib/bdev)] → [bdev module]
 * → [백엔드(NVMe/AIO/RAM 등)] 구조로 되어 있다. malloc bdev는 가장 하단의
 * "백엔드" 자리에 위치한 모듈로, 실제 PCIe/파일 I/O 대신 hugepage RAM에
 * memcpy/memset만 수행한다. 호출 체인은 다음과 같다:
 *   [JSON-RPC 클라이언트] → bdev_malloc_create RPC
 *     → rpc_bdev_malloc_create() (bdev_malloc_rpc.c)
 *       → create_malloc_disk()  (bdev_malloc.c, 본 헤더에서 선언)
 *         → spdk_bdev_register() (lib/bdev) → bdev 글로벌 트리에 등록
 * 실행 컨텍스트는 일반적으로 SPDK app의 RPC 처리 스레드(보통
 * 마스터 코어의 reactor)이며, 디스크 생성 자체는 동기 호출이다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/bdev_module.h (백엔드 모듈이 구현해야 하는
 *   spdk_bdev_module / spdk_bdev_fn_table 구조체와 SPDK_BDEV_MODULE_REGISTER 매크로),
 *   spdk/stdinc.h (표준 C 헤더 일괄 포함), 내부적으로는 spdk_uuid,
 *   spdk_dif_type, spdk_dif_pi_format 정의가 필요.
 * - 사용처: bdev_malloc_rpc.c가 이 헤더를 #include하여 RPC 입력
 *   JSON을 `struct malloc_bdev_opts`로 디코딩한 뒤 create_malloc_disk()를
 *   호출. delete_malloc_disk()는 RPC 삭제 핸들러에서 비동기 콜백
 *   체인의 시작점으로 호출된다.
 * - 데이터 흐름: RPC JSON params → malloc_bdev_opts → bdev 등록.
 *   삭제 시: RPC name → spdk_bdev_unregister_by_name() →
 *           cb_fn(cb_arg, errno).
 *
 * === 주요 함수/구조체 요약 ===
 * - typedef spdk_delete_malloc_complete : 비동기 삭제 완료 콜백 타입.
 *   bdev 코어가 unregister 작업을 마치면 호출되며 (cb_arg, bdeverrno) 형식.
 * - struct malloc_bdev_opts : 디스크 한 개를 생성하기 위한 모든 파라미터.
 *   이름, UUID, 블록 수/크기, 메타데이터, DIF/DIX(Data Integrity Field)
 *   설정, NUMA 노드 ID 등.
 * - create_malloc_disk()  : 옵션을 받아 hugepage 버퍼 할당과 bdev 등록을
 *   동기적으로 수행하고, 성공 시 *bdev에 새 spdk_bdev 포인터를 넘긴다.
 * - delete_malloc_disk()  : 이름으로 디스크를 찾아 비동기 unregister를
 *   시작하고, 끝나면 cb_fn을 호출. 호출 즉시 반환되는 비동기 함수.
 */

#ifndef SPDK_BDEV_MALLOC_H
/* [한국어] 헤더 가드(매크로) - 동일 파일이 여러 번 #include 되더라도
 *           구조체/함수 선언이 중복되지 않도록 컴파일러 단에서 보호한다. */
#define SPDK_BDEV_MALLOC_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 인클루드 묶음 - <stdint.h>, <stdbool.h>, <string.h>,
 *           <stdlib.h> 등을 한 번에 포함. uint32_t, uint64_t, bool 등이
 *           이 헤더의 구조체 필드 타입으로 사용되므로 선행 포함이 필요. */

#include "spdk/bdev_module.h"
/* [한국어] SPDK bdev 백엔드 모듈 작성에 필요한 핵심 자료형 헤더.
 *           struct spdk_bdev, struct spdk_bdev_module, spdk_uuid,
 *           spdk_dif_type, spdk_dif_pi_format 등이 여기서 제공된다.
 *           본 헤더는 spdk_bdev/spdk_uuid/dif 타입을 직접 사용하므로
 *           반드시 함께 포함되어야 한다. */

typedef void (*spdk_delete_malloc_complete)(void *cb_arg, int bdeverrno);
/* [한국어] malloc bdev 비동기 삭제 완료 콜백 타입 정의.
 *  - cb_arg     : delete_malloc_disk 호출 시 전달한 사용자 컨텍스트 포인터.
 *                 보통 spdk_jsonrpc_request* 등 RPC 응답을 보낼 때 필요한 값.
 *  - bdeverrno  : 0이면 성공, 음수면 -errno (예: -ENODEV, -EBUSY).
 *  설정자: bdev 코어 (spdk_bdev_unregister 경로)가 결과 코드를 채워 호출.
 *  읽는 자: RPC 핸들러가 받아 JSON 응답으로 변환.
 *  실행 컨텍스트: bdev 코어가 unregister를 완료한 SPDK thread (보통
 *                  unregister를 시작한 스레드와 동일하지만 보장은 아님). */

struct malloc_bdev_opts {
	/* [한국어] malloc bdev 한 개를 생성하기 위한 모든 사용자 옵션을
	 *           담는 평탄한(flat) 구조체. RPC JSON 디코더가 곧장 채울 수
	 *           있도록 모든 필드는 단순 스칼라/포인터로 구성된다. */

	char *name;
	/* [한국어] 생성될 bdev의 사용자 표시 이름 (예: "Malloc0").
	 * 설정자: RPC 디코더(spdk_json_decode_string)가 strdup으로 할당.
	 *         NULL이면 create_malloc_disk()가 "Malloc%d" 형태로 자동 생성.
	 * 읽는 자: create_malloc_disk()에서 spdk_bdev->name으로 복사.
	 * 값 범위: 0 종료 C 문자열 또는 NULL. malloc()/strdup() 으로 할당된 메모리.
	 * 동기화: 한 번 생성되면 변경되지 않으므로 락 불필요 (디스크 등록 후에는
	 *         spdk_bdev->name으로 사용되어 bdev 코어 락이 적용됨). */

	struct spdk_uuid uuid;
	/* [한국어] bdev의 고유 UUID (RFC 4122). 사용자가 지정하지 않으면
	 *           모두 0(null UUID)이며, 그 경우 bdev 코어가 자동 할당.
	 * 설정자: RPC 디코더(spdk_json_decode_uuid)가 문자열을 파싱.
	 * 읽는 자: create_malloc_disk()가 spdk_uuid_is_null() 체크 후 복사.
	 * 값 범위: 16 바이트 바이너리 UUID. 모두 0이면 "지정 안 함"을 의미. */

	uint64_t num_blocks;
	/* [한국어] 디스크의 논리 블록 개수. 총 용량 = num_blocks * block_size.
	 * 설정자: RPC (필수 파라미터). 읽는 자: spdk_bdev->blockcnt에 저장.
	 * 값 범위: > 0. 0이면 create_malloc_disk()가 -EINVAL 반환.
	 * 동기화: 디스크 생성 시점에만 설정되고 이후 불변. resize는 별도 API. */

	uint32_t block_size;
	/* [한국어] 데이터 블록 크기 (바이트). NVMe LBA 크기와 동일 개념.
	 *           md_interleave가 true면 메타데이터를 포함하지 않은 데이터부.
	 * 설정자: RPC (필수 파라미터). 값 범위: 512의 배수.
	 * 읽는 자: create_malloc_disk()에서 검증 후 spdk_bdev->blocklen 계산에 사용. */

	uint32_t physical_block_size;
	/* [한국어] 물리 블록 크기 (바이트). 일부 컨슈머가 alignment 결정에 사용.
	 * 설정자: RPC (선택). 미설정 시 0이며 bdev 기본 처리.
	 * 값 범위: 0(미지정) 또는 512의 배수. 읽는 자: spdk_bdev->phys_blocklen. */

	uint32_t optimal_io_boundary;
	/* [한국어] I/O가 가급적 이 경계(블록 단위)를 넘지 않도록 split 힌트.
	 *           NVMe NOIOB(Namespace Optimal I/O Boundary) 개념과 유사.
	 * 설정자: RPC (선택). 0이면 split 비활성. 읽는 자: bdev 코어가
	 *         split_on_optimal_io_boundary와 함께 split 결정에 사용. */

	uint32_t md_size;
	/* [한국어] 블록당 메타데이터(metadata) 크기 (바이트). 0/8/16/32/64/128만 허용.
	 *           DIF(Data Integrity Field) 등을 저장하는 영역.
	 * 설정자: RPC (선택, 기본 0). 읽는 자: create_malloc_disk()가
	 *         md_interleave 모드에 따라 blocklen에 더하거나 별도 버퍼로 관리. */

	bool md_interleave;
	/* [한국어] true면 데이터 블록 뒤에 메타데이터를 인터리브 배치
	 *           (blocklen = block_size + md_size).
	 *           false면 데이터/메타데이터를 별도 버퍼(malloc_md_buf)에 분리.
	 * 설정자: RPC (선택, 기본 false). 읽는 자: bdev_malloc_readv/writev 등
	 *         DIF 관련 경로에서 분기 조건. */

	enum spdk_dif_type dif_type;
	/* [한국어] Data Integrity Field 타입 (NVMe 스펙 기반).
	 *           SPDK_DIF_DISABLE / TYPE1 / TYPE2 / TYPE3 중 하나.
	 *           TYPE1/2: 가드+레퍼런스 태그 검증, TYPE3: 가드만.
	 * 설정자: RPC (선택). 읽는 자: bdev_malloc_submit_request의 DIF 분기. */

	bool dif_is_head_of_md;
	/* [한국어] DIF가 메타데이터 앞쪽에 위치하는지 여부.
	 *           NVMe 스펙의 PI Location 비트 매핑.
	 * 설정자: RPC (선택). 읽는 자: spdk_dif_ctx_init() 인자. */

	enum spdk_dif_pi_format dif_pi_format;
	/* [한국어] DIF Protection Information 포맷 (16b/32b/64b CRC 등).
	 *           NVMe 2.0의 ELBST/ELBAT 확장 PI 형식 지원에 사용.
	 * 설정자: RPC (선택). 읽는 자: spdk_dif_ctx_init_ext_opts. */

	int32_t numa_id;
	/* [한국어] hugepage 버퍼를 할당할 NUMA 노드 ID (-1 = SPDK_ENV_NUMA_ID_ANY).
	 *           멀티 소켓 시스템에서 워커 reactor와 같은 노드에 메모리 배치
	 *           → 메모리 접근 지연 최소화 (DPDK rte_malloc_socket 활용).
	 * 설정자: RPC (선택). 기본값은 rpc 핸들러에서 SPDK_ENV_NUMA_ID_ANY로 초기화.
	 * 읽는 자: create_malloc_disk()가 spdk_zmalloc(numa_id, ...)에 전달. */
};

int create_malloc_disk(struct spdk_bdev **bdev, const struct malloc_bdev_opts *opts);
/* [한국어] malloc bdev를 동기적으로 생성하는 진입 함수.
 *  @bdev (out): 성공 시 새로 만들어진 spdk_bdev 포인터가 여기에 저장된다.
 *               호출자는 이 포인터로 spdk_bdev_get_name 등을 호출해 RPC 응답을 만든다.
 *  @opts (in) : 위에서 정의한 malloc_bdev_opts. NULL이면 -EINVAL.
 *  @return    : 0=성공, 음수=-errno (-EINVAL/-ENOMEM 등).
 *  내부 동작: 옵션 검증 → spdk_zmalloc(hugepage)로 데이터·메타 버퍼 할당
 *             → spdk_bdev 필드 채움 → DIF 옵션이면 PI 패턴 미리 기록
 *             → spdk_bdev_register() 로 bdev 코어에 등록.
 *  실행 컨텍스트: 보통 RPC 핸들러 스레드(마스터 reactor)에서 동기 호출. */

void delete_malloc_disk(const char *name, spdk_delete_malloc_complete cb_fn, void *cb_arg);
/* [한국어] 이름으로 malloc bdev를 찾아 비동기 삭제를 시작.
 *  @name   (in) : 삭제할 bdev 이름. NULL/존재하지 않으면 cb_fn에 -ENODEV로 즉시 호출.
 *  @cb_fn  (in) : 삭제 완료 콜백. NULL 금지.
 *  @cb_arg (in) : cb_fn에 그대로 전달될 사용자 컨텍스트.
 *  @return: 없음. 결과는 항상 cb_fn(cb_arg, errno)로 통지.
 *  내부 동작: spdk_bdev_unregister_by_name() 호출 → 모든 채널 닫힘 후
 *             bdev_malloc_destruct()에서 hugepage 해제 → cb_fn 호출.
 *  실행 컨텍스트: 호출은 보통 RPC 스레드, cb_fn 호출 스레드는 bdev 코어가 결정. */

#endif /* SPDK_BDEV_MALLOC_H */
/* [한국어] 헤더 가드 종료. */
