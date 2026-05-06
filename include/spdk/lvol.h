/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Logical Volume Interface
 */

/*
 * [한국어 설명] SPDK Logical Volume 공개 API (lvol.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK Blobstore(BS) 위에 만들어지는 "Logical Volume(LVol)" 추상화의 공개
 * API 를 정의한다. Blobstore 는 한 블록 디바이스(bdev) 위에 cluster 단위로 여러 개의
 * 평면(flat) 객체(blob)를 저장하는 mini object-store 이고, lvol 은 그 위에 다음 의미를
 * 입힌 thin layer 이다: (1) 한 BS 인스턴스 = 한 LVS(Logical Volume Store), (2) 한 blob =
 * 한 LVol — 사용자에게는 가변 크기 가상 디스크, (3) blob 의 CoW snapshot/clone 능력을
 * 그대로 노출, 거기에 thin/thick provisioning · UUID · 이름 · external snapshot · shallow
 * copy · grow 같은 사용자 친화 기능을 추가. 결과적으로 사용자는 한 NVMe SSD 위에 수백~
 * 수천 개의 가변 크기 가상 디스크를 효율적으로 생성·관리할 수 있게 된다 (LVM 와 유사한
 * 컨셉이지만 user-space, polled-mode, 비동기 cb 모델).
 * 이 헤더는 LVS / LVol 에 대한 라이프사이클(create/load/unload/destroy/grow), CRUD, 부모-
 * 자식 관계(snapshot/clone/esnap_clone/inflate/decouple_parent/set_parent), 이름·UUID 조회,
 * IO channel 획득 등을 제공한다. 실제 I/O 자체는 spdk_blob_io_* 로 위임되므로 이 헤더에는
 * read/write API 가 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 스토리지 스택에서 lvol 은 "bdev backend module" 의 하나이자 동시에 "blob 위 user-
 * facing wrapper" 의 위치에 있다. 한 LVol 은 SPDK 의 bdev 레이어에 등록되어 fio/NVMe-oF
 * target/vhost/ublk 등 어떤 frontend 모듈도 그것을 일반 bdev 처럼 사용할 수 있다.
 *
 *    [Frontend module: vhost-blk / NVMe-oF / fio_plugin / iscsi / ublk]
 *           |  spdk_bdev_open_ext / readv / writev
 *           v
 *    [bdev layer]
 *           |  bdev module dispatch
 *           v
 *    [bdev_lvol module (module/bdev/lvol/)]   <— lvol bdev 등록 지점
 *           |  lvol API (이 헤더) → spdk_blob_io_* 로 변환
 *           v
 *    [lvol library (lib/lvol/)]
 *           |
 *           v
 *    [blobstore (lib/blob/)]   ← LVS = 한 spdk_blob_store
 *           |  cluster I/O
 *           v
 *    [bs_dev abstraction (spdk_bdev_create_bs_dev)]
 *           |
 *           v
 *    [실제 backend bdev: NVMe / aio / malloc / raid …]
 *
 * 실행 컨텍스트는 모두 SPDK reactor 위 spdk_thread 의 polled-mode 비동기 모델이며, lvol
 * 의 모든 CRUD 함수는 비동기 callback 패턴을 따른다 (cb_fn(cb_arg, [handle], errno)).
 * 한 LVS 는 보통 metadata 작업이 단일 spdk_thread 에 직렬화되고, 데이터 I/O 는 io_channel
 * 별로 호출자 thread 에서 처리된다.
 *
 * === 타 모듈과의 연결 ===
 * - spdk/blob.h: lvol 의 핵심 의존성. 모든 메타데이터/IO 가 spdk_blob_* / spdk_bs_* 로
 *   위임된다. lvol_clear_method / lvs_clear_method enum 은 BS enum 과 1:1 매핑.
 *   esnap_bs_dev_create 콜백 타입(spdk_bs_esnap_dev_create)도 blob.h 에서 정의.
 * - spdk/uuid.h: 각 lvol/lvs 가 UUID 로 식별 — get_by_uuid 조회 키.
 * - spdk/stdinc.h: 표준 C 헤더 묶음.
 * - spdk/bdev.h: 호출자가 backend bdev 위에 spdk_bdev_create_bs_dev() / _ext() 로 만든
 *   spdk_bs_dev 를 인자로 넘긴다 (lvs_init/load/grow). lvol 자체는 bdev 가 아니지만,
 *   module/bdev/lvol/ 에서 각 lvol 을 다시 bdev 로 등록한다.
 * - module/bdev/lvol/: lvol 을 bdev 로 노출하는 wrapping 모듈. 그쪽에서 본 헤더의 API 를
 *   호출해 LVS load 후 각 LVol 을 bdev 등록한다.
 * - SPDK_STATIC_ASSERT (spdk/assert.h): spdk_lvs_opts ABI 사이즈를 빌드 시점에 강제 검증.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_lvs_init / load / load_ext / unload / destroy / rename / grow / grow_live:
 *   Logical Volume Store 라이프사이클 (= 한 BS 인스턴스의 라이프사이클).
 * - spdk_lvol_create / destroy / close / open / rename / resize: 개별 lvol 라이프사이클.
 * - spdk_lvol_create_snapshot / create_clone / create_esnap_clone: 부모-자식 관계 형성
 *   (CoW). esnap = external snapshot (lvol 외부의 read-only 데이터 소스).
 * - spdk_lvol_inflate / decouple_parent: 부모와의 의존성 해소 (모든 cluster 를 자체 보유로
 *   변환). thick provisioning 만들기.
 * - spdk_lvol_set_parent / set_external_parent: 기존 lvol 의 부모를 변경 (snapshot 또는
 *   esnap 으로 재연결).
 * - spdk_lvol_shallow_copy: read-only lvol 을 외부 bs_dev 로 복사 (deep dependent 클러스터만).
 * - spdk_lvol_iter_immediate_clones: 한 snapshot 을 부모로 갖는 즉시 자식 lvol 들 순회.
 * - spdk_lvol_get_by_uuid / get_by_names / get_io_channel / is_degraded / deletable: 조회/상태.
 * - struct spdk_lvs_opts: LVS init/load 파라미터 묶음 (cluster_sz, clear_method, name 등).
 * - enum lvol_clear_method / lvs_clear_method: lvol/cluster 데이터 영역 초기화 정책.
 * - 콜백 타입: spdk_lvs_op_with_handle_complete / spdk_lvs_op_complete /
 *   spdk_lvol_op_with_handle_complete / spdk_lvol_op_complete / spdk_lvol_iter_cb.
 */

#ifndef SPDK_LVOL_H                       /* [한국어] include guard 시작 — 동일 컴파일
                                          * 유닛에서 헤더가 두 번 펼쳐지면 enum/struct
                                          * 중복 정의로 실패한다. SPDK 공개 헤더 공통 패턴. */
#define SPDK_LVOL_H                       /* [한국어] guard 매크로 정의 — 두 번째 include
                                          * 부터는 본문 통째 skip. */

#include "spdk/stdinc.h"                  /* [한국어] SPDK 표준 헤더 묶음 (stdint, stdbool,
                                          * stddef 등). uint32_t, uint64_t, bool, size_t
                                          * 등이 본 헤더 전반에서 사용된다. */
#include "spdk/blob.h"                    /* [한국어] Blobstore 의 enum/typedef 들을 가져옴.
                                          * - BLOB_CLEAR_WITH_DEFAULT/NONE/UNMAP/WRITE_ZEROES
                                          *   : lvol_clear_method enum 의 값으로 1:1 사용.
                                          * - BS_CLEAR_WITH_UNMAP/WRITE_ZEROES/NONE
                                          *   : lvs_clear_method enum 의 값.
                                          * - spdk_bs_esnap_dev_create: external snapshot
                                          *   bs_dev 생성 콜백 타입 (lvs_opts 의 멤버). */
#include "spdk/uuid.h"                    /* [한국어] struct spdk_uuid 정의 — spdk_lvol_get_by_uuid
                                          * 의 인자 타입. SPDK 의 RFC4122 UUID 표현. */

#ifdef __cplusplus                        /* [한국어] C++ 사용자 보호: 함수 심볼이 C linkage 로
                                          * export 되어야 .a/.so ABI 호환된다. SPDK 본체는 C. */
extern "C" {
#endif

struct spdk_bs_dev;                       /* [한국어] 전방 선언: SPDK Blobstore 디바이스 추상.
                                          * 정의는 spdk/blob.h 에 있고, 호출자는 보통
                                          * spdk_bdev_create_bs_dev() / _ext() 로 미리 만들어
                                          * lvs_init / lvs_load 에 넘긴다. 이 헤더에서는 포인터로만
                                          * 사용하므로 전방 선언으로 충분. */
struct spdk_lvol_store;                   /* [한국어] 전방 선언: LVol Store 핸들 (= 한 BS 인스턴스
                                          * 위 lvol 컨테이너). 정의는 lib/lvol 내부 헤더.
                                          * 사용자는 불투명 포인터로만 접근. */
struct spdk_lvol;                         /* [한국어] 전방 선언: 개별 LVol 핸들. 내부적으로 한
                                          * spdk_blob 와 1:1 매핑된다. 정의는 lib/lvol 내부 헤더. */

enum lvol_clear_method {
	LVOL_CLEAR_WITH_DEFAULT = BLOB_CLEAR_WITH_DEFAULT,
	/* [한국어] 새로 할당되는 데이터 cluster 의 기본 초기화 정책을 LVS 의 정책과 동일하게.
	 * 설정자: spdk_lvol_create() 의 clear_method 인자.
	 * 읽는 자: lvol → blob create 시 BLOB_CLEAR_WITH_DEFAULT 로 변환되어 BS 가 LVS-level
	 * clear 정책을 적용. 값 의미: "특별한 override 없음, store 기본". */

	LVOL_CLEAR_WITH_NONE = BLOB_CLEAR_WITH_NONE,
	/* [한국어] cluster 할당 시 zeroing/unmap 을 하지 않는다 — 이전 데이터가 그대로 노출될
	 * 수 있으므로 보안/정합성 책임은 호출자(상위 layer 가 직접 zeroing 함)에 있다.
	 * 사용 시나리오: thin provisioning 인데 RAW 디스크 위에서 frontend 가 항상 write 후
	 * read 하는 워크로드(stale data 노출이 무관). 가장 빠름. */

	LVOL_CLEAR_WITH_UNMAP = BLOB_CLEAR_WITH_UNMAP,
	/* [한국어] cluster 할당 시 underlying bdev 에 UNMAP/DEALLOCATE 명령을 보낸다 (NVMe
	 * Dataset Management Deallocate 또는 SCSI UNMAP). SSD 가 실제로 블록을 free 처리해
	 * write amplification 을 줄일 수 있고, 후속 read 는 0 또는 trim pattern 을 반환한다.
	 * 단, underlying bdev 가 unmap 미지원이면 lvs_init 시 BS 가 fallback 한다. */

	LVOL_CLEAR_WITH_WRITE_ZEROES = BLOB_CLEAR_WITH_WRITE_ZEROES,
	/* [한국어] cluster 할당 시 NVMe Write Zeroes / SCSI WRITE SAME(0) 로 0 패턴을 명시적
	 * 기록. UNMAP 보다 느리지만 hardware 가 unmap 의미를 제대로 못 줄 때 결정적으로
	 * 0 read 를 보장. 보안 민감 환경에서 stale data 차단 용도로 사용. */
};

enum lvs_clear_method {
	LVS_CLEAR_WITH_UNMAP = BS_CLEAR_WITH_UNMAP,
	/* [한국어] LVS 초기화/생성 시 backing bdev 의 metadata 영역을 UNMAP 으로 클리어.
	 * 설정자: spdk_lvs_opts::clear_method (lvs_init 시).
	 * 읽는 자: BS 메타데이터 페이지 초기화 경로. UNMAP 미지원 bdev 에서는 fallback. */

	LVS_CLEAR_WITH_WRITE_ZEROES = BS_CLEAR_WITH_WRITE_ZEROES,
	/* [한국어] LVS 메타데이터 영역을 Write Zeroes 명령으로 클리어. UNMAP 미지원 SSD/RAM
	 * 디스크 등에서 결정적 0 read 를 보장. */

	LVS_CLEAR_WITH_NONE = BS_CLEAR_WITH_NONE,
	/* [한국어] 클리어를 수행하지 않음. 기존 디스크에 LVS 가 깔린 적이 있어 metadata 가
	 * 이미 있는 경우 오작동 가능 — 새 디스크를 직접 LVS 화 할 때 빠른 경로. */
};

/* Must include null terminator. */
#define SPDK_LVS_NAME_MAX	64        /* [한국어] LVS 이름 최대 길이 (널 종료자 포함, 즉 실제
                                          * 사용 가능한 글자수는 63). spdk_lvs_opts::name
                                          * 배열의 정적 크기에 사용된다. RPC 입력 검증과
                                          * 메타데이터 직렬화 시 길이 제한으로 적용. */
#define SPDK_LVOL_NAME_MAX	64        /* [한국어] 개별 lvol 이름 최대 길이 (널 종료자 포함).
                                          * spdk_lvol_create / rename / get_by_names 의 이름
                                          * 인자 검증에 사용. */

/**
 * Parameters for lvolstore initialization.
 */
struct spdk_lvs_opts {
	/** Size of cluster in bytes. Must be multiple of 4KiB page size. */
	uint32_t		cluster_sz;
	/* [한국어] LVS 의 cluster 크기 (바이트). 이 값이 곧 lvol 의 thin/thick allocation 단위.
	 * 설정자: spdk_lvs_opts_init 가 default 값 채움 후 사용자 override. lvs_init 시점에 BS
	 * 에 그대로 전달되어 metadata 에 영구 기록 (load 시 옵션 값 무시되고 metadata 값 사용).
	 * 읽는 자: BS 가 cluster 할당/lookup 의 기본 단위로 사용.
	 * 값 범위: 4KiB 의 정수배 (보통 1MiB / 4MiB 권장). 너무 작으면 metadata 비대,
	 * 너무 크면 thin provisioning 의 공간 낭비.
	 * 동기화: BS 생성 시 1회 캡처되며 이후 변경 불가. */

	/** Clear method */
	enum lvs_clear_method	clear_method;
	/* [한국어] LVS 메타데이터 영역의 클리어 정책 (위 enum lvs_clear_method 참조).
	 * 설정자: lvs_opts_init 가 기본값(LVS_CLEAR_WITH_UNMAP), 사용자 override.
	 * 읽는 자: lvs_init 의 BS 메타페이지 초기화 경로.
	 * 값 범위: enum 값 3종.
	 * 동기화: lvs_init 1회 사용. */

	/** Name of the lvolstore */
	char			name[SPDK_LVS_NAME_MAX];
	/* [한국어] LVS 의 사람이 읽을 수 있는 이름. lvol path 표기 시 "<lvs_name>/<lvol_name>"
	 * 형태로 합성된다. UUID 와 별개의 보조 식별자.
	 * 설정자: lvs_init 호출자가 채움. lvs_rename 으로 변경 가능 (BS 메타데이터 갱신).
	 * 읽는 자: spdk_lvol_get_by_names, RPC list 응답.
	 * 값 범위: 1~SPDK_LVS_NAME_MAX-1 글자, 비어있으면 -EINVAL.
	 * 동기화: lvs 단위 mutex 로 변경 시 직렬화. */

	/** num_md_pages_per_cluster_ratio = 100 means 1 page per cluster */
	uint32_t		num_md_pages_per_cluster_ratio;
	/* [한국어] cluster 1개당 metadata page 비율(%). 100 = 1 cluster 당 1 page (BS 기본).
	 * 작은 비율을 주면 metadata 영역이 작아져 데이터 영역이 커지지만, 동시에 만들 수 있는
	 * blob/lvol 수의 상한이 줄어든다. 큰 비율을 주면 반대.
	 * 설정자: lvs_init 호출자.
	 * 읽는 자: BS 가 metadata page count 산정에 사용. 메타데이터에 기록되어 load 시 재사용.
	 * 값 범위: > 0. 보통 100 그대로 두는 게 안전. */

	/**
	 * The size of spdk_lvol_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default
	 * values. After that, new added fields should be put in the end of the struct.
	 */
	uint32_t		opts_size;
	/* [한국어] 호출자가 컴파일된 spdk_lvs_opts 의 sizeof 값. lvol 라이브러리가 이 값을 보고
	 * "어디까지 필드가 유효한지" 판단해 ABI 호환을 보장한다 (구버전 헤더로 컴파일된
	 * 사용자가 새 라이브러리와 링크되어도 안전).
	 * 설정자: spdk_lvs_opts_init() 가 sizeof(struct spdk_lvs_opts) 로 채움. 사용자가
	 * 직접 채워도 됨.
	 * 읽는 자: 라이브러리 내부 — opts_size 보다 뒤에 있는 신규 필드는 default 로 처리.
	 * 동기화: per-call 캡처라 동기화 불필요. */

	/**
	 * A function to be called to load external snapshots. If this is NULL while the lvolstore
	 * is being loaded, the lvolstore will not support external snapshots.
	 */
	spdk_bs_esnap_dev_create esnap_bs_dev_create;
	/* [한국어] external snapshot(esnap) 클론을 로드할 때 호출되는 콜백. esnap 은 LVS 외부
	 * 데이터 소스(예: read-only NVMe namespace)를 부모로 삼는 lvol 클론에 사용된다.
	 * 라이브러리는 lvs_load 중 각 esnap_clone lvol 의 esnap_id (사용자 정의 식별자) 를 읽고,
	 * 이 콜백을 호출해 해당 id 에 해당하는 spdk_bs_dev 를 만들어 받는다.
	 * 설정자: lvs_init / lvs_load_ext 호출자가 채움. NULL 이면 esnap 미지원 모드.
	 * 읽는 자: lvol 라이브러리의 load 경로 (esnap_clone 마다 1회 호출).
	 * 동기화: 콜백은 lvs 의 metadata thread 컨텍스트에서 호출 — 콜백 안에서 cross-thread
	 * 동기 호출은 피해야 함. */

	/** Metadata page size */
	uint32_t                md_page_size;
	/* [한국어] BS metadata page 크기 (바이트). 보통 4096(4KiB) 권장. cluster_sz 와 함께
	 * BS 의 layout 을 결정하며, lvs_init 시 metadata 에 기록되어 load 시 강제 적용.
	 * 설정자: lvs_opts_init 의 기본값 또는 사용자 override.
	 * 읽는 자: BS layout 계산.
	 * 값 범위: 보통 4096 의 2 의 거듭제곱 배수. 0 이면 라이브러리가 기본값으로 채움
	 * (opts_size 가 충분히 작으면 라이브러리가 기본 페이지 크기 사용).
	 * 동기화: lvs_init 1회 사용. */
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_lvs_opts) == 92, "Incorrect size");
/* [한국어] 컴파일타임 검증: spdk_lvs_opts 의 정확한 바이트 사이즈가 92 여야 한다.
 * packed attribute 와 더불어 ABI 안정성을 강제하는 장치 — 누가 멤버를 추가/제거하면
 * 빌드가 깨지므로 의도적 ABI 변경임을 명시적으로 증빙해야 한다. */

/**
 * Initialize an spdk_lvs_opts structure to the defaults.
 *
 * \param opts Pointer to the spdk_lvs_opts structure to initialize.
 */
/*
 * [한국어]
 * spdk_lvs_opts_init - spdk_lvs_opts 구조체를 라이브러리 기본값으로 채운다.
 *
 * @opts: 사용자가 스택/힙에 잡은 spdk_lvs_opts 포인터 (NULL 금지).
 *
 * 호출자는 이 함수로 default 를 채운 뒤 필요한 필드만 override 해서 spdk_lvs_init /
 * spdk_lvs_load_ext 에 넘긴다. 함수 내부에서 opts_size 도 sizeof(*opts) 로 채워주므로
 * 호출자가 별도로 설정할 필요 없음. 새 SPDK 버전에서 추가된 필드는 모두 안전한 기본값으로
 * 들어간다 — ABI forward-compat 유지에 필수.
 *
 * 호출 체인:
 *   사용자/RPC handler → spdk_lvs_opts_init(&opts) → 일부 override → spdk_lvs_init(...)
 */
void spdk_lvs_opts_init(struct spdk_lvs_opts *opts);

/**
 * Callback definition for lvolstore operations, including handle to lvs.
 *
 * \param cb_arg Custom arguments
 * \param lvol_store Handle to lvol_store or NULL when lvserrno is set
 * \param lvserrno Error
 */
/* [한국어] LVS "with handle" 비동기 완료 콜백 타입.
 * - cb_arg: 호출자가 지정한 컨텍스트 그대로 전달.
 * - lvol_store: 성공 시 새로 만들거나 로드된 LVS 핸들 (실패 시 NULL).
 * - lvserrno: 0 = 성공, 음수 errno = 실패 사유.
 * 호출 시점: lvs_init / lvs_load / lvs_grow 의 모든 비동기 단계가 끝났을 때.
 * 호출 스레드: 작업을 발사한 spdk_thread.
 * SPDK 의 표준 "결과 핸들 + 에러 코드" 패턴 — 호출자가 핸들을 받아 후속 작업 가능. */
typedef void (*spdk_lvs_op_with_handle_complete)(void *cb_arg, struct spdk_lvol_store *lvol_store,
		int lvserrno);

/**
 * Callback definition for lvolstore operations without handle.
 *
 * \param cb_arg Custom arguments
 * \param lvserrno Error
 */
/* [한국어] LVS "without handle" 비동기 완료 콜백 타입.
 * - cb_arg: 호출자 컨텍스트.
 * - lvserrno: 0 = 성공, 음수 errno = 실패.
 * 호출 시점: lvs_unload / lvs_destroy / lvs_rename / lvs_grow_live 등 핸들 반환이 의미
 * 없는 작업 완료 시. */
typedef void (*spdk_lvs_op_complete)(void *cb_arg, int lvserrno);


/**
 * Callback definition for lvol operations with handle to lvol.
 *
 * \param cb_arg Custom arguments
 * \param lvol Handle to lvol or NULL when lvserrno is set
 * \param lvolerrno Error
 */
/* [한국어] LVol "with handle" 비동기 완료 콜백 타입.
 * - cb_arg: 호출자 컨텍스트.
 * - lvol: 성공 시 새로 만든/연 lvol 핸들 (실패 시 NULL).
 * - lvolerrno: 0 = 성공, 음수 errno = 실패.
 * 호출 시점: lvol_create / create_snapshot / create_clone / create_esnap_clone / open 완료 시. */
typedef void (*spdk_lvol_op_with_handle_complete)(void *cb_arg, struct spdk_lvol *lvol,
		int lvolerrno);

/**
 * Callback definition for lvol operations without handle to lvol.
 *
 * \param cb_arg Custom arguments
 * \param lvolerrno Error
 */
/* [한국어] LVol "without handle" 비동기 완료 콜백 타입.
 * - cb_arg: 호출자 컨텍스트.
 * - lvolerrno: 0 = 성공, 음수 errno = 실패.
 * 호출 시점: lvol_destroy / close / rename / inflate / decouple_parent / set_parent /
 * shallow_copy 등 핸들 반환이 의미 없는 lvol 작업 완료 시. */
typedef void (*spdk_lvol_op_complete)(void *cb_arg, int lvolerrno);

/**
 * Callback definition for spdk_lvol_iter_clones.
 *
 * \param lvol An iterated lvol.
 * \param cb_arg Opaque context passed to spdk_lvol_iter_clone().
 * \return 0 to continue iterating, any other value to stop iterating.
 */
/* [한국어] 자식 lvol 순회용 콜백 타입.
 * - cb_arg: spdk_lvol_iter_immediate_clones 에 전달된 컨텍스트.
 * - lvol: 현재 순회 중인 자식 lvol 핸들.
 * - return: 0 = 계속 순회, 그 외 = 즉시 중단 (해당 값이 호출자에게 그대로 전달됨).
 * 호출 스레드: iterator 가 호출된 spdk_thread (동기 순회 — 비동기 cb 아님).
 * 사용 패턴: 모든 immediate clone 의 상태/이름 수집, 또는 특정 조건 만족 시 break. */
typedef int (*spdk_lvol_iter_cb)(void *cb_arg, struct spdk_lvol *lvol);

/**
 * Initialize lvolstore on given bs_bdev.
 *
 * \param bs_dev This is created on the given bdev by using spdk_bdev_create_bs_dev()
 * beforehand.
 * \param o Options for lvolstore.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_lvs_init - backing bdev(bs_dev 형태) 위에 새 LVS 를 초기화한다 (= BS 생성).
 *
 * @bs_dev: spdk_bdev_create_bs_dev() / _ext() 로 미리 만든 BS 디바이스 추상.
 *          내부적으로 한 backend bdev 에 1:1 매핑됨.
 * @o     : LVS 옵션 (cluster_sz / clear_method / name / esnap_bs_dev_create 등).
 * @cb_fn : 완료 콜백 (with handle).
 * @cb_arg: 콜백에 전달될 사용자 컨텍스트.
 * @return: 0 = 비동기 작업 정상 시작. 음수 errno = 즉시 거부 (인자 검증 실패 등).
 *
 * 동작:
 *   1) 옵션 검증 (이름 유효성, cluster_sz 4KiB 배수 등).
 *   2) BS 메타데이터 영역 클리어 (clear_method 에 따라 unmap/zeroes/none).
 *   3) BS super-block 작성 + lvol 전용 메타데이터 영역(이름/UUID 매핑) 작성.
 *   4) 모든 단계 끝났을 때 cb_fn(cb_arg, lvs, 0). 실패 시 정리 후 cb_fn(.., NULL, errno).
 *
 * 주의: 기존에 BS 가 깔린 디스크에 init 을 부르면 데이터가 파괴된다 — 의도된 새 디스크에만
 * 사용. 기존 LVS 를 사용하려면 spdk_lvs_load 사용.
 *
 * 호출 체인:
 *   RPC bdev_lvol_create_lvstore → spdk_bdev_create_bs_dev_ext → spdk_lvs_init(...) →
 *   비동기 단계 → cb_fn(arg, lvs, rc)
 */
int spdk_lvs_init(struct spdk_bs_dev *bs_dev, struct spdk_lvs_opts *o,
		  spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Rename the given lvolstore.
 *
 * \param lvs Pointer to lvolstore.
 * \param new_name New name of lvs.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
/*
 * [한국어]
 * spdk_lvs_rename - LVS 의 이름을 변경하고 메타데이터에 영구 기록.
 *
 * @lvs: 변경 대상 LVS 핸들.
 * @new_name: 새 이름. SPDK_LVS_NAME_MAX 미만, 비어있지 않아야 함, 다른 LVS 와 중복 금지.
 * @cb_fn: 완료 콜백 (without handle).
 * @cb_arg: 콜백 컨텍스트.
 *
 * BS 메타데이터에 새 이름을 기록하는 비동기 작업이다. 내부에서 BS 의 metadata super-blob
 * 을 갱신하므로 한 번의 sync write 동반. 이름이 바뀌면 이후 spdk_lvol_get_by_names 의
 * lvs_name 키도 새 이름으로 매칭된다.
 *
 * 호출 체인:
 *   RPC bdev_lvol_rename_lvstore → spdk_lvs_rename(...) → metadata write → cb_fn(arg, rc)
 */
void spdk_lvs_rename(struct spdk_lvol_store *lvs, const char *new_name,
		     spdk_lvs_op_complete cb_fn, void *cb_arg);

/**
 * Unload lvolstore.
 *
 * All lvols have to be closed beforehand, when doing unload.
 *
 * \param lvol_store Handle to lvolstore.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_lvs_unload - LVS 를 메모리에서 내려놓는다 (디스크 상 데이터 보존).
 *
 * @lvol_store: 언로드할 LVS 핸들.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 = 비동기 시작 성공. 음수 errno = 즉시 거부 (열린 lvol 존재 시 -EBUSY 등).
 *
 * 모든 자식 lvol 이 미리 close 되어 있어야 한다. unload 는 BS 의 in-memory 자료구조와
 * io_channel 을 해제하지만 디스크의 super-block / cluster 데이터는 그대로 유지 — 이후
 * spdk_lvs_load 로 다시 올릴 수 있다. destroy 와 구분이 핵심.
 *
 * 호출 체인:
 *   RPC bdev_lvol_unregister_lvstore / app shutdown → 모든 lvol close → spdk_lvs_unload
 *   → 비동기 단계 → cb_fn(arg, rc)
 */
int spdk_lvs_unload(struct spdk_lvol_store *lvol_store,
		    spdk_lvs_op_complete cb_fn, void *cb_arg);

/**
 * Destroy lvolstore.
 *
 * All lvols have to be closed beforehand, when doing destroy.
 *
 * \param lvol_store Handle to lvolstore.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_lvs_destroy - LVS 를 영구 파괴한다 (디스크 상 메타데이터/데이터 모두 제거).
 *
 * @lvol_store: 파괴할 LVS 핸들.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 = 비동기 시작 성공. 음수 errno = 즉시 거부.
 *
 * unload 와 다르게 BS super-block 이 무효화되므로 같은 디스크는 다시 spdk_lvs_load 할
 * 수 없다 (재사용하려면 init 부터). 모든 자식 lvol 이 사전 close 되어 있어야 한다.
 * underlying bdev 의 데이터 영역도 옵션에 따라 unmap / write_zeroes 로 클리어된다.
 *
 * 호출 체인:
 *   RPC bdev_lvol_delete_lvstore → 모든 lvol close → spdk_lvs_destroy → cb_fn(arg, rc)
 */
int spdk_lvs_destroy(struct spdk_lvol_store *lvol_store,
		     spdk_lvs_op_complete cb_fn, void *cb_arg);

/**
 * Create lvol on given lvolstore with specified size.
 *
 * \param lvs Handle to lvolstore.
 * \param name Name of lvol.
 * \param sz size of lvol in bytes.
 * \param thin_provisioned Enables thin provisioning.
 * \param clear_method Changes default data clusters clear method
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_lvol_create - LVS 위에 새 lvol(가상 디스크)을 만든다.
 *
 * @lvs: 부모 LVS 핸들.
 * @name: lvol 이름. SPDK_LVOL_NAME_MAX 미만, 같은 LVS 안에서 유일해야 함.
 * @sz: 논리 크기 (바이트). cluster_sz 의 정수배로 라이브러리가 올림 처리.
 * @thin_provisioned: true = 처음에 cluster 할당 안 함, write 시점 lazy allocation.
 *                    false = 즉시 모든 cluster 할당 (thick / fully allocated).
 * @clear_method: 새로 할당되는 cluster 의 클리어 정책 (위 enum 참조).
 * @cb_fn: 완료 콜백 (with handle).
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 = 비동기 시작 성공. 음수 errno = 즉시 거부 (이름 중복, 공간 부족 등).
 *
 * 동작:
 *   1) 이름/크기 검증.
 *   2) spdk_bs_create_blob_ext() 호출 — 새 blob 생성, lvol 메타데이터 attach.
 *   3) thick 인 경우 즉시 모든 cluster 할당 (clear_method 에 따라 zero/unmap).
 *   4) blob open + lvol 객체 등록 → cb_fn(arg, lvol, 0).
 *
 * 호출 체인:
 *   RPC bdev_lvol_create → spdk_lvol_create → blob create + 옵션 적용 → cb_fn(arg, lvol, rc)
 */
int spdk_lvol_create(struct spdk_lvol_store *lvs, const char *name, uint64_t sz,
		     bool thin_provisioned, enum lvol_clear_method clear_method,
		     spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);
/**
 * Create snapshot of given lvol.
 *
 * \param lvol Handle to lvol.
 * \param snapshot_name Name of created snapshot.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
/*
 * [한국어]
 * spdk_lvol_create_snapshot - 기존 lvol 의 시점 스냅샷을 생성한다 (CoW).
 *
 * @lvol: 원본 lvol (스냅샷 대상). 스냅샷 후 자동으로 새 snapshot 의 자식이 됨 (clone 으로 변환).
 * @snapshot_name: 새 snapshot lvol 의 이름.
 * @cb_fn: 완료 콜백 (with handle — snapshot lvol 반환).
 * @cb_arg: 콜백 컨텍스트.
 *
 * BS 의 spdk_bs_create_snapshot 위임. 결과:
 *   - 원본 lvol 은 read-write 상태 유지하지만, 스냅샷 시점의 모든 cluster 는 이제
 *     snapshot lvol 이 read-only 로 소유.
 *   - 원본 lvol 은 snapshot 의 clone 이 되어, write 시 CoW 로 cluster 분기.
 *   - 새 snapshot lvol 은 read-only 이며 자체 read 시 동일 데이터 노출.
 *
 * 사용 시나리오: 백업 시점 고정, 게스트 디스크 hot-snapshot, 다단 클론 트리 구축.
 *
 * 호출 체인:
 *   RPC bdev_lvol_snapshot → spdk_lvol_create_snapshot → blob snapshot → cb_fn(arg, snap, rc)
 */
void spdk_lvol_create_snapshot(struct spdk_lvol *lvol, const char *snapshot_name,
			       spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Create clone of given snapshot.
 *
 * \param lvol Handle to lvol snapshot.
 * \param clone_name Name of created clone.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
/*
 * [한국어]
 * spdk_lvol_create_clone - 기존 snapshot 의 read-write 클론을 생성한다.
 *
 * @lvol: 부모로 사용할 snapshot lvol (반드시 read-only/snapshot 이어야 함).
 * @clone_name: 새 clone lvol 의 이름.
 * @cb_fn: 완료 콜백 (with handle).
 * @cb_arg: 콜백 컨텍스트.
 *
 * BS 의 spdk_bs_create_clone 위임. clone 은 thin-provisioned 로 시작하며, snapshot 과
 * 동일한 데이터를 공유(read 는 부모로 fall-through, write 는 자체 cluster 로 CoW). 한
 * snapshot 에 여러 clone 을 만들 수 있어 "Golden Image" 패턴(공통 OS 이미지 → 게스트별
 * 분기) 에 효과적.
 *
 * 호출 체인:
 *   RPC bdev_lvol_clone → spdk_lvol_create_clone → blob clone → cb_fn(arg, clone, rc)
 */
void spdk_lvol_create_clone(struct spdk_lvol *lvol, const char *clone_name,
			    spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Create clone of given non-lvol device.
 *
 * The bdev that is being cloned is commonly called an external snapshot or esnap. The clone is
 * commonly called an esnap clone.
 *
 * \param esnap_id The identifier that will be passed to the spdk_bs_esnap_dev_create callback.
 * \param id_len The length of esnap_id, in bytes.
 * \param size_bytes The size of the external snapshot device, in bytes. This must be an integer
 * multiple of the lvolstore's cluster size. See \c cluster_sz in \struct spdk_lvs_opts.
 * \param lvs Handle to lvolstore.
 * \param clone_name Name of created clone.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 * \return 0 if parameters pass verification checks and the esnap creation is started, in which case
 * the \c cb_fn will be used to report the completion status. If an error is encountered, a negative
 * errno will be returned and \c cb_fn will not be called.
 */
/*
 * [한국어]
 * spdk_lvol_create_esnap_clone - LVS 외부의 read-only 디바이스를 부모로 하는 esnap clone 생성.
 *
 * @esnap_id: 사용자 정의 식별자 (바이트 배열). lvs_opts::esnap_bs_dev_create 콜백에
 *            그대로 전달되어 그 콜백이 이 id 로 부모 bs_dev 를 만들 책임을 진다.
 * @id_len: esnap_id 의 바이트 길이.
 * @size_bytes: 외부 디바이스의 크기 (cluster_sz 의 정수배여야 함).
 * @lvs: 클론이 살게 될 LVS.
 * @clone_name: 새 clone 이름.
 * @cb_fn: 완료 콜백 (with handle).
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 = 검증 통과 + 비동기 시작. cb_fn 이 결과 통보. 음수 errno = 즉시 거부 (검증
 *          실패 — 이 경우 cb_fn 은 호출되지 않음). 호출자가 두 경로를 모두 처리해야 함.
 *
 * 사용 시나리오: 다른 SPDK 호스트의 NVMe-oF read-only namespace 를 esnap 으로 해서 그
 * 위에 로컬 read-write 클론을 만든다 — VM 이미지 배포, multi-tier storage 등.
 *
 * 호출 체인:
 *   RPC bdev_lvol_create_esnap_clone → spdk_lvol_create_esnap_clone → 검증 →
 *   esnap_bs_dev_create cb 호출 → blob esnap clone 생성 → cb_fn(arg, lvol, rc)
 */
int spdk_lvol_create_esnap_clone(const void *esnap_id, uint32_t id_len, uint64_t size_bytes,
				 struct spdk_lvol_store *lvs, const char *clone_name,
				 spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Rename lvol with new_name.
 *
 * \param lvol Handle to lvol.
 * \param new_name new name for lvol.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
/*
 * [한국어]
 * spdk_lvol_rename - lvol 의 이름을 변경하고 BS 메타데이터에 영구 기록.
 *
 * @lvol: 변경 대상 lvol.
 * @new_name: 새 이름. 같은 LVS 안에서 유일해야 함.
 * @cb_fn: 완료 콜백 (without handle).
 * @cb_arg: 콜백 컨텍스트.
 *
 * lvol 메타데이터(blob xattr)에 이름을 갱신하고 super-blob 인덱스를 재구성한다.
 * spdk_lvol_get_by_names 조회 결과가 새 이름 기준으로 바뀐다.
 *
 * 호출 체인:
 *   RPC bdev_lvol_rename → spdk_lvol_rename → blob xattr 업데이트 → cb_fn(arg, rc)
 */
void
spdk_lvol_rename(struct spdk_lvol *lvol, const char *new_name,
		 spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * \brief Returns if it is possible to delete an lvol (i.e. lvol is not a snapshot that have at least one clone).
 * \param lvol Handle to lvol
 */
/*
 * [한국어]
 * spdk_lvol_deletable - lvol 을 안전하게 삭제할 수 있는지 검사 (동기, 즉시 반환).
 *
 * @lvol: 검사 대상 lvol.
 * @return: true = 자식 clone 이 없거나 snapshot 이 아니어서 삭제 가능. false = 적어도
 *          하나 이상의 clone 의 부모이므로 삭제 시 자식들이 dangling 됨 — 먼저 자식들을
 *          inflate 또는 set_parent 로 분리해야 함.
 *
 * destroy 호출 전 사용자에게 친절한 에러 메시지를 주기 위한 사전 체크 용도.
 *
 * 호출 체인:
 *   RPC bdev_lvol_delete (precondition check) → spdk_lvol_deletable → bool
 */
bool spdk_lvol_deletable(struct spdk_lvol *lvol);

/**
 * Close lvol and remove information about lvol from its lvolstore.
 *
 * \param lvol Handle to lvol.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
/*
 * [한국어]
 * spdk_lvol_destroy - lvol 을 close 하고 LVS 메타데이터에서 영구 삭제한다.
 *
 * @lvol: 파괴할 lvol.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 *
 * 동작:
 *   1) 만약 자식 clone 이 있는 snapshot 이라면 -EBUSY 로 실패 — 사전에 spdk_lvol_deletable 권장.
 *   2) blob close → bs_delete_blob → cluster 회수 (clear_method 에 따라 unmap/zero/none).
 *   3) lvol 메타데이터 (이름/UUID 매핑) 제거.
 *   4) cb_fn(arg, 0).
 *
 * close 와 destroy 의 차이: close = 메모리에서만 내림 (메타데이터 보존), destroy = 영구 삭제.
 *
 * 호출 체인:
 *   RPC bdev_lvol_delete → spdk_lvol_destroy → 비동기 단계 → cb_fn(arg, rc)
 */
void spdk_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Close lvol, but information is kept on lvolstore.
 *
 * \param lvol Handle to lvol.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
/*
 * [한국어]
 * spdk_lvol_close - lvol 을 메모리에서 내려놓는다 (디스크 데이터 유지).
 *
 * @lvol: close 할 lvol.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 *
 * blob close 위임. lvol 메타데이터/데이터는 LVS 에 그대로 남아 있어 spdk_lvol_open 으로
 * 다시 올릴 수 있다. lvs_unload 의 사전조건 — 모든 lvol 이 close 되어야 unload 가능.
 *
 * 호출 체인:
 *   상위 모듈(bdev_lvol unregister 등) → spdk_lvol_close → blob close → cb_fn(arg, rc)
 */
void spdk_lvol_close(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Iterate clones of an lvol.
 *
 * Iteration stops if cb_fn(cb_arg, clone_lvol) returns non-zero.
 *
 * \param lvol Handle to lvol.
 * \param cb_fn Function to call for each lvol that clones this lvol.
 * \param cb_arg Context to pass with cb_fn.
 * \return -ENOMEM if memory allocation failed, non-zero return from cb_fn(), or 0.
 */
/*
 * [한국어]
 * spdk_lvol_iter_immediate_clones - lvol 을 직접 부모로 갖는 자식 clone 들을 순회 (동기).
 *
 * @lvol: 부모 lvol (보통 snapshot).
 * @cb_fn: 각 자식에 대해 호출. 0 반환 시 계속, 비-0 반환 시 즉시 중단.
 * @cb_arg: cb_fn 에 전달할 컨텍스트.
 * @return: 0 = 모든 자식을 순회 완료. -ENOMEM = 자식 목록 수집 중 메모리 할당 실패.
 *          그 외 비-0 = cb_fn 이 반환한 stop 코드.
 *
 * "immediate" 는 직계 자식만 — 손자 이상은 포함되지 않는다. 다단 트리 전체 순회는
 * 호출자가 재귀로 구성해야 한다. snapshot 삭제 가능 여부 판단, RPC 트리 그리기 등에 사용.
 *
 * 호출 체인:
 *   RPC bdev_lvol_get_lvols (트리 출력) → spdk_lvol_iter_immediate_clones(lvol, cb, arg)
 */
int spdk_lvol_iter_immediate_clones(struct spdk_lvol *lvol, spdk_lvol_iter_cb cb_fn, void *cb_arg);

/**
 * Get the lvol that has a particular UUID.
 *
 * \param uuid The lvol's UUID.
 * \return A pointer to the requested lvol on success, else NULL.
 */
/*
 * [한국어]
 * spdk_lvol_get_by_uuid - UUID 로 lvol 핸들 조회 (동기, 즉시 반환).
 *
 * @uuid: 찾을 lvol 의 UUID (struct spdk_uuid 포인터).
 * @return: 일치하는 lvol 핸들 또는 NULL.
 *
 * 모든 등록된 LVS 의 lvol 풀을 검색한다 (전역 인덱스). UUID 는 lvol 생성 시 무작위로
 * 부여되며 영구적이라 이름 변경에도 안정적인 식별자.
 *
 * 호출 체인:
 *   bdev_lvol 모듈의 bdev open 경로 / RPC 등 → spdk_lvol_get_by_uuid(&uuid) → 핸들 사용
 */
struct spdk_lvol *spdk_lvol_get_by_uuid(const struct spdk_uuid *uuid);

/**
 * Get the lvol that has the specified name in the specified lvolstore.
 *
 * \param lvs_name Name of the lvolstore.
 * \param lvol_name Name of the lvol.
 * \return A pointer to the requested lvol on success, else NULL.
 */
/*
 * [한국어]
 * spdk_lvol_get_by_names - (LVS 이름, lvol 이름) 쌍으로 lvol 핸들 조회.
 *
 * @lvs_name: LVS 이름 문자열.
 * @lvol_name: lvol 이름 문자열.
 * @return: 두 이름으로 매칭되는 lvol 핸들 또는 NULL (어느 쪽이라도 없으면).
 *
 * 사람이 읽는 식별자 기반 조회 — RPC, CLI, 사용자 입력 처리에 적합. 같은 lvol 이름이라도
 * 다른 LVS 에 있으면 별개로 매핑되므로 LVS 이름이 namespace 역할을 한다.
 *
 * 호출 체인:
 *   RPC bdev_lvol_get_lvols / open 경로 → spdk_lvol_get_by_names("lvs0", "data0") → 핸들
 */
struct spdk_lvol *spdk_lvol_get_by_names(const char *lvs_name, const char *lvol_name);

/**
 * Get I/O channel of bdev associated with specified lvol.
 *
 * \param lvol Handle to lvol.
 *
 * \return a pointer to the I/O channel.
 */
/*
 * [한국어]
 * spdk_lvol_get_io_channel - lvol 의 I/O 채널 (해당 spdk_thread 컨텍스트) 획득.
 *
 * @lvol: 유효한 lvol 핸들.
 * @return: spdk_io_channel * — 호출 thread 에 바인딩된 채널. 호출자는 사용 후
 *          spdk_put_io_channel 로 해제 책임.
 *
 * SPDK 의 io_channel 은 thread-local 자원이라 같은 lvol 이라도 thread 마다 별도의 채널을
 * 받는다 — lockless 디자인의 핵심. 채널 위에서 spdk_blob_io_read/write/unmap 이 발사된다.
 *
 * 호출 체인:
 *   bdev_lvol 모듈의 get_io_channel cb → spdk_lvol_get_io_channel(lvol) → bdev I/O 처리
 */
struct spdk_io_channel *spdk_lvol_get_io_channel(struct spdk_lvol *lvol);

/**
 * Load lvolstore from the given blobstore device.
 *
 * \param bs_dev Pointer to the blobstore device.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
/*
 * [한국어]
 * spdk_lvs_load - 기존 LVS 를 backing bdev 위에서 메모리에 로드한다.
 *
 * @bs_dev: spdk_bdev_create_bs_dev() 로 만든 BS 디바이스 추상.
 * @cb_fn: 완료 콜백 (with handle).
 * @cb_arg: 콜백 컨텍스트.
 *
 * BS super-block 검증 → 메타데이터 페이지 스캔 → 모든 lvol 객체 재구성 (open 은 안 함).
 * 사용자는 이후 spdk_lvol_open 으로 개별 lvol 을 열어야 한다. esnap clone 이 있어도
 * 이 함수는 esnap_bs_dev_create 콜백 없이 동작 — esnap 미지원 모드. esnap 까지 지원하려면
 * spdk_lvs_load_ext 를 사용.
 *
 * 호출 체인:
 *   RPC bdev_lvol_register_lvstore → spdk_bdev_create_bs_dev_ext → spdk_lvs_load →
 *   비동기 단계 → cb_fn(arg, lvs, rc)
 */
void spdk_lvs_load(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn,
		   void *cb_arg);

/**
 * Load lvolstore from the given blobstore device with options.
 *
 * If lvs_opts is not NULL, it should be initialized with spdk_lvs_opts_init().
 *
 * \param bs_dev Pointer to the blobstore device.
 * \param lvs_opts lvolstore options.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 * blobstore.
 */
/*
 * [한국어]
 * spdk_lvs_load_ext - lvs_opts 를 받아 LVS 를 로드한다 (esnap 콜백 등록 가능).
 *
 * @bs_dev: BS 디바이스.
 * @lvs_opts: 옵션 묶음 — 가장 중요한 멤버는 esnap_bs_dev_create. NULL 이면 spdk_lvs_load
 *            와 동일 (esnap 미지원).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 *
 * load 시 esnap clone lvol 들이 발견되면 라이브러리가 lvs_opts->esnap_bs_dev_create 를
 * 호출해 외부 부모 디바이스를 만들어 받는다. lvs_opts 는 spdk_lvs_opts_init 로 채워야
 * forward-compat 안전.
 *
 * 호출 체인:
 *   RPC bdev_lvol_register_lvstore (with esnap support) → spdk_lvs_opts_init →
 *   esnap_bs_dev_create 채움 → spdk_lvs_load_ext → cb_fn(arg, lvs, rc)
 */
void spdk_lvs_load_ext(struct spdk_bs_dev *bs_dev, const struct spdk_lvs_opts *lvs_opts,
		       spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Grow a lvstore to fill the underlying device.
 * Cannot be used on loaded lvstore.
 *
 * \param bs_dev Pointer to the blobstore device.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
/*
 * [한국어]
 * spdk_lvs_grow - 로드되지 않은 LVS 의 metadata 를 backing bdev 의 새 크기에 맞춰 확장.
 *
 * @bs_dev: BS 디바이스 (이미 underlying bdev 가 커진 상태).
 * @cb_fn: 완료 콜백 (with handle — 확장 후 LVS 핸들 반환).
 * @cb_arg: 콜백 컨텍스트.
 *
 * 사용 시나리오: 외부에서 NVMe namespace / RAID 볼륨이 확장됐을 때, LVS 의 cluster 영역도
 * 추가 공간을 인식해야 한다. 이 함수가 metadata 의 cluster bitmap 을 늘리고 super-block
 * 을 갱신한다. "cannot be used on loaded lvstore" — 동시에 in-memory 로 올라가 있으면
 * 데이터 일관성 위험이 있어 unload 상태에서만 호출 가능.
 *
 * 호출 체인:
 *   underlying bdev resize → unload → spdk_lvs_grow(bs_dev) → cb_fn(arg, lvs, rc)
 */
void spdk_lvs_grow(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn,
		   void *cb_arg);

/**
 * Grow a loaded lvstore to fill the underlying device.
 *
 * \param lvs Pointer to lvolstore.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
/*
 * [한국어]
 * spdk_lvs_grow_live - 로드되어 사용 중인 LVS 를 in-flight 상태로 확장 (online grow).
 *
 * @lvs: 현재 로드된 LVS 핸들.
 * @cb_fn: 완료 콜백 (without handle).
 * @cb_arg: 콜백 컨텍스트.
 *
 * spdk_lvs_grow 와 달리 LVS 를 unload 하지 않고 진행한다 — 진행 중 I/O 와의 정합성을
 * 보장하는 잠금 처리(BS 메타데이터 갱신 시점에만 큐 직렬화)가 내부에 들어 있다.
 * 사용 시나리오: 게스트 VM 에 노출된 NVMe-oF namespace 를 무중단 확장.
 *
 * 호출 체인:
 *   underlying bdev resize → spdk_lvs_grow_live(lvs) → 메타 업데이트 → cb_fn(arg, rc)
 */
void spdk_lvs_grow_live(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg);

/**
 * Open a lvol.
 *
 * \param lvol Handle to lvol.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
/*
 * [한국어]
 * spdk_lvol_open - lvol 의 backing blob 을 열어 I/O 가능 상태로 만든다.
 *
 * @lvol: 열 lvol 핸들 (lvs_load 후 메타만 올라온 상태).
 * @cb_fn: 완료 콜백 (with handle — 같은 lvol 반환).
 * @cb_arg: 콜백 컨텍스트.
 *
 * 동작: spdk_bs_open_blob → blob 헤더 로드 → lvol 의 in-memory 상태 활성화. 이 시점부터
 * spdk_lvol_get_io_channel + blob_io_read/write 호출 가능. close 의 짝.
 *
 * 호출 체인:
 *   bdev_lvol 모듈의 bdev open cb → spdk_lvol_open → cb_fn(arg, lvol, rc) → bdev_io 처리
 */
void spdk_lvol_open(struct spdk_lvol *lvol, spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Inflate lvol
 *
 * \param lvol Handle to lvol
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
/*
 * [한국어]
 * spdk_lvol_inflate - thin/clone lvol 의 모든 미할당/공유 cluster 를 자체 cluster 로 변환.
 *
 * @lvol: 대상 lvol (clone 또는 thin-provisioned).
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 *
 * 동작:
 *   - thin-provisioned: 모든 미할당 cluster 를 zero-fill 로 할당 → thick 화.
 *   - clone: 부모 snapshot 에서 read-fall-through 하던 cluster 들을 모두 로컬로 복사 →
 *     부모와의 의존성 완전 제거 (부모 snapshot 삭제 가능해짐).
 *
 * 결과적으로 lvol 은 자기 자신만으로 완전한 데이터를 보유 (storage 사용량 증가하지만
 * 부모 chain 의존 없음, 성능도 read fall-through 비용 제거로 향상).
 *
 * 호출 체인:
 *   RPC bdev_lvol_inflate → spdk_lvol_inflate → blob 모든 cluster materialize →
 *   cb_fn(arg, rc)
 */
void spdk_lvol_inflate(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Decouple parent of lvol
 *
 * \param lvol Handle to lvol
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
/*
 * [한국어]
 * spdk_lvol_decouple_parent - clone 이 실제로 참조 중인 부모 cluster 만 로컬로 복사.
 *
 * @lvol: 대상 clone lvol.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 *
 * inflate 와 비슷하지만 차이가 있다 — inflate 는 미할당 영역까지 포함해 모든 cluster 를
 * materialize 하지만, decouple_parent 는 "부모 chain 의존성 제거" 가 목적이라 부모로부터
 * 실제 read-through 되던 cluster 만 자체 cluster 로 복사한다. 미할당 영역(thin)은 그대로
 * 유지. 결과: lvol 은 더 이상 부모 snapshot 을 참조하지 않으나 thin 속성은 유지.
 *
 * 사용 시나리오: 부모 snapshot 을 삭제하고 싶지만 thin 속성을 유지하고 싶을 때.
 *
 * 호출 체인:
 *   RPC bdev_lvol_decouple_parent → spdk_lvol_decouple_parent → blob decouple →
 *   cb_fn(arg, rc)
 */
void spdk_lvol_decouple_parent(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Determine if an lvol is degraded. A degraded lvol cannot perform IO.
 *
 * \param lvol Handle to lvol
 * \return true if the lvol has no open blob or the lvol's blob is degraded, else false.
 */
/*
 * [한국어]
 * spdk_lvol_is_degraded - lvol 이 I/O 불가 상태인지 동기 검사.
 *
 * @lvol: 검사 대상 lvol (NULL 안전성 확인은 호출자 책임).
 * @return: true = degraded (blob 미열림 또는 esnap 부모 디바이스 누락 등 — IO 시 즉시
 *          실패), false = 정상 동작 가능.
 *
 * 사용 시나리오: bdev_lvol 모듈이 bdev I/O 를 받기 전 사전 검사하여 -EIO 즉시 반환,
 * 또는 RPC list 에서 사용자에게 상태 표시.
 *
 * 호출 체인:
 *   bdev_lvol I/O fast path → spdk_lvol_is_degraded → IO submit / 즉시 실패 분기
 */
bool spdk_lvol_is_degraded(const struct spdk_lvol *lvol);

/**
 * Make a shallow copy of lvol on given bs_dev.
 *
 * Lvol must be read only and lvol size must be less or equal than bs_dev size.
 *
 * \param lvol Handle to lvol
 * \param ext_dev The bs_dev to copy on. This is created on the given bdev by using
 * spdk_bdev_create_bs_dev_ext() beforehand
 * \param status_cb_fn Called repeatedly during operation with status updates
 * \param status_cb_arg Argument passed to function status_cb_fn.
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 *
 * \return 0 if operation starts correctly, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_lvol_shallow_copy - read-only lvol 의 "자체 보유" cluster 만 외부 bs_dev 에 복사.
 *
 * @lvol: 원본 lvol — 반드시 read-only(snapshot) 이어야 함.
 * @ext_dev: 복사 대상 외부 BS 디바이스 — lvol 보다 크거나 같아야 함.
 * @status_cb_fn: 진행 상황 콜백 (블록/cluster 단위로 반복 호출). progress bar 등에 사용.
 * @status_cb_arg: status_cb_fn 의 컨텍스트.
 * @cb_fn: 최종 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 = 정상 시작 (cb_fn 으로 결과 통보), 음수 errno = 즉시 거부 (lvol 이 RW 상태,
 *          ext_dev 크기 부족 등).
 *
 * "shallow" 의미: 부모 chain 으로부터 fall-through 되는 cluster 는 복사하지 않고 lvol 이
 * 자체 보유한 cluster 만 복사한다. 결과 ext_dev 는 그 자체로는 완전한 데이터가 아니지만,
 * 같은 부모를 가진 시스템에서는 lvol 의 차이분(diff)으로 사용 가능 — 백업/마이그레이션의
 * 증분 전송 패턴에 적합.
 *
 * 호출 체인:
 *   RPC bdev_lvol_shallow_copy → spdk_bdev_create_bs_dev_ext (target) →
 *   spdk_lvol_shallow_copy → 진행 status 업데이트 반복 → cb_fn(arg, rc)
 */
int spdk_lvol_shallow_copy(struct spdk_lvol *lvol, struct spdk_bs_dev *ext_dev,
			   spdk_blob_shallow_copy_status status_cb_fn, void *status_cb_arg,
			   spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Set a snapshot as the parent of a lvol
 *
 * This call set a snapshot as the parent of a lvol, making the lvol a clone of this snapshot.
 * The previous parent of the lvol, if any, can be another snapshot or an external snapshot; if
 * the lvol is not a clone, it must be thin-provisioned.
 * Lvol and parent snapshot must have the same size and must belong to the same lvol store.
 *
 * \param lvol Handle to lvol
 * \param snapshot Handle to the parent snapshot
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
/*
 * [한국어]
 * spdk_lvol_set_parent - 기존 lvol 의 부모를 다른 snapshot 으로 재설정 (재부착).
 *
 * @lvol: 대상 lvol — clone 이거나 thin-provisioned 여야 함.
 * @snapshot: 새 부모로 사용할 snapshot lvol. 같은 LVS 안에 있어야 하고 크기도 동일해야 함.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 *
 * 사용 시나리오: 다단 snapshot 트리에서 한 노드의 부모를 다른 노드로 옮기거나, esnap clone
 * 을 internal snapshot clone 으로 변경. 부모 변경은 BS 메타데이터 갱신만 수반 — 실제 데이터
 * 이동은 없다 (CoW 시점부터 새 부모 기준으로 fall-through). 하지만 새 부모와 기존 데이터의
 * 의미적 일관성은 호출자 책임.
 *
 * 호출 체인:
 *   RPC bdev_lvol_set_parent → spdk_lvol_set_parent → blob set parent → cb_fn(arg, rc)
 */
void spdk_lvol_set_parent(struct spdk_lvol *lvol, struct spdk_lvol *snapshot,
			  spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Set an external snapshot as the parent of a lvol
 *
 * This call set an external snapshot as the parent of a lvol, making the lvol a clone of this
 * external snapshot.
 * The previous parent of the lvol, if any, can be another external snapshot or a snapshot; if
 * the lvol is not a clone, it must be thin-provisioned.
 * The size of the external snapshot device must be an integer multiple of cluster size of
 * lvol's lvolstore.
 *
 * \param lvol Handle to lvol
 * \param esnap_id The identifier of the external snapshot.
 * \param esnap_id_len The length of esnap_id, in bytes.
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
/*
 * [한국어]
 * spdk_lvol_set_external_parent - 기존 lvol 의 부모를 esnap (외부 read-only 디바이스) 로 설정.
 *
 * @lvol: 대상 lvol — clone 또는 thin-provisioned.
 * @esnap_id: 외부 디바이스 식별자 바이트 배열. lvs_opts->esnap_bs_dev_create 콜백에
 *            전달되어 그 콜백이 부모 bs_dev 를 만들 책임.
 * @esnap_id_len: esnap_id 의 바이트 길이.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 *
 * spdk_lvol_set_parent 의 esnap 버전. 외부 디바이스 크기가 LVS cluster_sz 의 정수배여야
 * 한다. 사용 시나리오: 로컬 snapshot 을 삭제하고 원격 read-only 데이터로 부모를 옮기는
 * tier-down, 또는 처음부터 esnap clone 으로 재구성하는 마이그레이션.
 *
 * 호출 체인:
 *   RPC bdev_lvol_set_parent_bdev → spdk_lvol_set_external_parent → esnap_bs_dev_create cb
 *   호출 → blob set external parent → cb_fn(arg, rc)
 */
void spdk_lvol_set_external_parent(struct spdk_lvol *lvol, const void *esnap_id,
				   uint32_t esnap_id_len,
				   spdk_lvol_op_complete cb_fn, void *cb_arg);

#ifdef __cplusplus                        /* [한국어] C++ extern "C" 블록 닫기 */
}
#endif

#endif  /* SPDK_LVOL_H */                 /* [한국어] include guard 종료 */
